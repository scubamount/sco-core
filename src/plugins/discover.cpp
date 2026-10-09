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
            for (const auto& cap : p.manifest.requires_)
                if (!opts.has || !opts.has(cap.c_str())) { Refuse(p, "missing capability '" + cap + "'"); break; }
        }
        list.push_back(std::move(p));
    }
    return list;
}

}  // namespace sco::plugins
