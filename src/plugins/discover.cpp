// Plugin discovery: lists data/plugins/<id>/plugin.ini folders and checks each one. Never opens
// an entry file and never runs plugin code.
#include "sco/plugins.h"
#include "internal.h"
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <system_error>

namespace sco::plugins {

const char* StateName(State s) {
    switch (s) {
        case State::Off:      return "off";
        case State::Disabled: return "disabled";
        case State::Refused:  return "refused";
        case State::Ready:    return "ready";
        case State::Loaded:   return "loaded";
        case State::Crashed:  return "crashed";
        case State::Unloaded: return "unloaded";
    }
    return "?";
}

using detail::FromUtf8;
static std::string Utf8(const fs::path& p) { return detail::ToUtf8(p); }

// A regular file that is not a symlink (a plugin can't point outside its folder).
static bool PlainFile(const fs::path& p) {
    std::error_code ec;
    const auto st = fs::symlink_status(p, ec);
    return !ec && st.type() == fs::file_type::regular;
}

// Reads at most kMaxManifestBytes + 1 bytes, so an oversized file is detected without
// reading all of it.
static bool ReadCapped(const fs::path& p, std::string& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.resize(kMaxManifestBytes + 1);
    f.read(out.data(), static_cast<std::streamsize>(out.size()));
    out.resize(static_cast<size_t>(f.gcount()));
    return !f.bad();
}

// True when one of the host's built-ins has this id: the built-in owns it (its command prefix,
// its services), so a folder may not take it, whatever its kind.
static bool BuiltinId(const Options& opts, const std::string& id) {
    for (size_t i = 0; opts.builtins && i < opts.nBuiltins; ++i)
        if (opts.builtins[i].id && id == opts.builtins[i].id) return true;
    return false;
}

// The host's own id and the reserved command prefixes (kReservedIds) can't be plugin folders.
static bool ReservedFolder(const std::string& folder) {
    for (const char* r : kReservedIds)
        if (folder == r) return true;
    return false;
}

static void Refuse(Plugin& p, std::string reason) {
    p.state = State::Refused;
    p.reason = std::move(reason);
}

// ---- requires: providers, load order, cycles -------------------------------------------------

static const Plugin* ByFolder(const std::vector<Plugin>& list, const std::string& id) {
    for (const Plugin& q : list)
        if (q.folder == id) return &q;
    return nullptr;
}

// Why a plugin in p.after can't provide for p, or "". While discovering, a Ready plugin will load
// and an id not in the list is a built-in (the list holds folders only); at load time the provider
// must be Loaded (a data pack, indexed after the loads, Ready), and an id not in the list is unmet.
static std::string Unmet(const Plugin& p, const std::vector<Plugin>& list, bool discovering) {
    for (const std::string& id : p.after) {
        const Plugin* q = ByFolder(list, id);
        if (!q) {
            if (discovering) continue;
            return "requires " + id + ": plugin '" + id + "' is not in the plugin list";
        }
        const bool ok = discovering ? (q->state == State::Ready || q->state == State::Loaded)
                                    : (q->state == State::Loaded ||
                                       (q->state == State::Ready && q->manifest.kind == Kind::Data));
        if (ok) continue;
        std::string why = "requires " + id + ": plugin '" + id + "' is " + StateName(q->state);
        if (!q->reason.empty()) why += " (" + q->reason + ")";
        return why;
    }
    return {};
}

// Points every Ready plugin's requires at what provides it, or refuses it.
static void ResolveRequires(std::vector<Plugin>& list, const Options& opts) {
    for (Plugin& p : list) {
        if (p.state != State::Ready) continue;
        for (const std::string& name : p.manifest.requires_) {
            const std::string head = name.substr(0, name.find('.'));   // the plugin id of "<id>.<name>"
            std::string provider;
            if (opts.has && opts.has(name.c_str())) {
                continue;                                               // a capability, a host or game service
            } else if (head == p.folder) {
                provider = p.folder;                                    // itself: a cycle, refused below
            } else if (BuiltinId(opts, head)) {
                provider = head;
            } else if (!ReservedFolder(head)) {
                if (const Plugin* q = ByFolder(list, head)) provider = q->folder;
            }
            if (provider.empty()) {
                Refuse(p, name.find('.') == std::string::npos ? "missing capability '" + name + "'"
                                                              : "requires service " + name + ": no plugin provides it");
                p.after.clear();
                break;
            }
            if (std::find(p.after.begin(), p.after.end(), provider) == p.after.end()) p.after.push_back(provider);
        }
    }
}

// Kahn's algorithm over the Ready plugins, always taking the lowest list index that is free, so
// folder order is kept wherever nothing requires otherwise. Returns every index; plugins left in
// `stuck` (a cycle, or waiting on one) are appended in list order.
static std::vector<size_t> Sorted(const std::vector<Plugin>& list, std::vector<size_t>& stuck) {
    const size_t n = list.size();
    std::vector<std::vector<size_t>> deps(n);
    for (size_t i = 0; i < n; ++i) {
        if (list[i].state != State::Ready) continue;
        for (const std::string& id : list[i].after)
            for (size_t j = 0; j < n; ++j)
                if (list[j].state == State::Ready && list[j].folder == id) deps[i].push_back(j);
    }
    std::vector<char> placed(n, 0);
    std::vector<size_t> order;
    stuck.clear();
    while (order.size() < n) {
        size_t pick = n;
        for (size_t i = 0; i < n && pick == n; ++i) {
            if (placed[i]) continue;
            bool free = true;
            for (size_t j : deps[i]) free = free && placed[j];
            if (free) pick = i;
        }
        if (pick == n) break;
        placed[pick] = 1;
        order.push_back(pick);
    }
    for (size_t i = 0; i < n; ++i)
        if (!placed[i]) { stuck.push_back(i); order.push_back(i); }
    return order;
}

// Refuses the plugins of one cycle among the stuck ones, "requires cycle: a -> b -> a" (the
// member that sorts first leads). Walks from the first stuck plugin to the first of its providers
// that is stuck too, until a plugin repeats: that is a cycle, since each stuck plugin waits on one.
static void RefuseOneCycle(std::vector<Plugin>& list, const std::vector<size_t>& stuck) {
    std::vector<char> isStuck(list.size(), 0);
    for (size_t i : stuck) isStuck[i] = 1;
    std::vector<size_t> path;
    std::vector<long> at(list.size(), -1);
    size_t cur = stuck.front();
    while (at[cur] < 0) {
        at[cur] = static_cast<long>(path.size());
        path.push_back(cur);
        size_t next = list.size();
        for (const std::string& id : list[cur].after)
            for (size_t j = 0; j < list.size(); ++j)
                if (isStuck[j] && list[j].state == State::Ready && list[j].folder == id && j < next) next = j;
        cur = next;   // a stuck plugin always has a stuck provider
    }
    std::vector<size_t> cycle(path.begin() + at[cur], path.end());
    std::rotate(cycle.begin(), std::min_element(cycle.begin(), cycle.end()), cycle.end());
    std::string text = "requires cycle: ";
    for (size_t i : cycle) text += list[i].folder + " -> ";
    text += list[cycle.front()].folder;
    for (size_t i : cycle) { Refuse(list[i], text); list[i].after.clear(); }
}

std::vector<size_t> LoadOrder(const std::vector<Plugin>& list) {
    std::vector<size_t> stuck;
    return Sorted(list, stuck);
}

std::string UnmetRequires(const Plugin& p, const std::vector<Plugin>& list) { return Unmet(p, list, false); }

// After every folder is listed: resolve the names, refuse cycles, then refuse what waits on a
// plugin that was refused (in load order, so a refusal reaches the whole chain).
static void CheckRequires(std::vector<Plugin>& list, const Options& opts) {
    ResolveRequires(list, opts);
    for (std::vector<size_t> stuck;;) {
        Sorted(list, stuck);
        if (stuck.empty()) break;
        RefuseOneCycle(list, stuck);
    }
    std::vector<size_t> none;
    for (size_t i : Sorted(list, none)) {
        if (list[i].state != State::Ready) continue;
        const std::string why = Unmet(list[i], list, true);
        if (!why.empty()) { Refuse(list[i], why); list[i].after.clear(); }
    }
}

std::vector<Plugin> Discover(const fs::path& root, const Options& opts) {
    std::vector<Plugin> list;
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return list;

    std::vector<fs::path> dirs;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const auto st = it->symlink_status(ec);
        if (ec || st.type() != fs::file_type::directory) continue;   // files and symlinks skipped
        if (!PlainFile(it->path() / "plugin.ini")) continue;
        dirs.push_back(it->path());
    }
    std::sort(dirs.begin(), dirs.end(),
              [](const fs::path& a, const fs::path& b) { return Utf8(a.filename()) < Utf8(b.filename()); });

    for (const auto& dir : dirs) {
        Plugin p;
        p.dir = dir;
        p.folder = Utf8(dir.filename());
        // With plugins off every folder is listed Off, past the cap too.
        if (opts.enabled && list.size() >= kMaxPlugins) { Refuse(p, "too many plugins"); list.push_back(std::move(p)); continue; }

        std::string text, error;
        const bool read = ReadCapped(dir / "plugin.ini", text);
        const bool parsed = read && ParseManifest(text, p.manifest, error);
        p.manifestOk = parsed;
        if (!parsed) p.manifest = Manifest{};

        if (!opts.enabled) {
            p.state = State::Off;
        } else if (ReservedFolder(p.folder)) {
            Refuse(p, "the folder name '" + p.folder + "' is reserved for the host");
        } else if (std::error_code dec; fs::symlink_status(dir / "disabled", dec).type() != fs::file_type::not_found) {
            // Any entry named "disabled" (file, folder, link) switches the plugin off.
            p.state = State::Disabled;
            p.reason = "disabled file";
        } else if (!read) {
            Refuse(p, "plugin.ini: unreadable");
        } else if (!parsed) {
            Refuse(p, "plugin.ini: " + error);
        } else if (p.manifest.id != p.folder) {
            Refuse(p, "id '" + p.manifest.id + "' does not match folder '" + p.folder + "'");
        } else if (BuiltinId(opts, p.manifest.id)) {
            Refuse(p, "the id belongs to a built-in plugin");
        } else if (p.manifest.apiMajor != opts.hostMajor || p.manifest.apiMinor > opts.hostMinor) {
            char buf[48];
            std::snprintf(buf, sizeof(buf), "built for api %u.%u", p.manifest.apiMajor, p.manifest.apiMinor);
            Refuse(p, buf);
        } else if (p.manifest.kind != Kind::Data && !PlainFile(dir / FromUtf8(p.manifest.entry))) {
            Refuse(p, "entry '" + p.manifest.entry + "' not found");
        } else {
            p.state = State::Ready;
        }
        list.push_back(std::move(p));
    }
    if (opts.enabled) CheckRequires(list, opts);
    return list;
}

}  // namespace sco::plugins
