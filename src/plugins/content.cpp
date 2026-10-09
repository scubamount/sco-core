// Data-pack content index: what each Ready data pack ships, by kind. A pack never runs code;
// features query the index (missions, rules, scripts, lists) and read the files themselves.
#include "sco/plugins.h"
#include "sco/log.h"
#include "internal.h"
#include <algorithm>
#include <system_error>

namespace sco::plugins {

const char* ContentKindName(ContentKind k) {
    switch (k) {
        case ContentKind::Mission: return "mission";
        case ContentKind::Rules:   return "rules";
        case ContentKind::Script:  return "script";
        case ContentKind::List:    return "list";
        case ContentKind::DataCore: return "datacore";
    }
    return "?";
}

static std::string Utf8(const fs::path& p) { return detail::ToUtf8(p); }

static bool EndsWithNoCase(const std::string& s, std::string_view ext) {
    if (s.size() < ext.size()) return false;
    for (size_t i = 0; i < ext.size(); ++i) {
        char c = s[s.size() - ext.size() + i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != ext[i]) return false;
    }
    return true;
}

struct Rule {
    ContentKind kind;
    const char* folder;
    const char* ext;
    bool        recursive;
};
static constexpr Rule kRules[] = {
    { ContentKind::Mission, "missions", ".cwmission", false },
    { ContentKind::Rules,   "rules",    ".rules",     false },
    { ContentKind::Script,  "scripts",  ".xml",       true  },
    { ContentKind::List,    "lists",    ".txt",       false },
    { ContentKind::DataCore, "datacore", ".toml",     false },
};

// Collects one pack's items. Empty on success, else why the pack is refused: more than
// kMaxPackFiles matching files, or a content folder that couldn't be read to the end (a pack is
// never loaded with part of its files). Symlinked files and folders are skipped, so nothing
// outside the pack is indexed.
static std::string Collect(const Plugin& p, std::vector<ContentItem>& out) {
    for (const Rule& r : kRules) {
        const fs::path base = p.dir / r.folder;
        std::error_code ec;
        const auto st = fs::symlink_status(base, ec);
        if (ec || st.type() != fs::file_type::directory) continue;

        auto take = [&](const fs::directory_entry& e) {
            std::error_code e2;
            if (e.symlink_status(e2).type() != fs::file_type::regular || e2) return true;
            const std::string rel = Utf8(fs::relative(e.path(), p.dir, e2));
            if (e2 || !EndsWithNoCase(rel, r.ext)) return true;
            if (out.size() >= kMaxPackFiles) return false;
            out.push_back(ContentItem{ r.kind, p.manifest.id, rel, e.path() });
            return true;
        };
        if (r.recursive) {
            // directory_iterator options default: symlinked folders are not followed.
            for (fs::recursive_directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                if (it.depth() >= kMaxScriptDepth) it.disable_recursion_pending();   // seen, not entered
                if (!take(*it)) return "too many files";
            }
        } else {
            for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec))
                if (!take(*it)) return "too many files";
        }
        if (ec) return std::string("cannot read ") + r.folder + ": " + ec.message();
    }
    return {};
}

size_t ContentIndex::Build(std::vector<Plugin>& list) {
    items_.clear();
    for (auto& p : list) {
        // Loaded too: a rebuild re-reads every pack it indexed before.
        if ((p.state != State::Ready && p.state != State::Loaded) || p.manifest.kind != Kind::Data) continue;
        std::vector<ContentItem> mine;
        std::string why = Collect(p, mine);
        if (!why.empty()) {
            p.state = State::Refused;
            p.reason = std::move(why);
            sco::Log("[plugin] refused %s: %s", p.manifest.id.c_str(), p.reason.c_str());
            continue;
        }
        std::sort(mine.begin(), mine.end(), [](const ContentItem& a, const ContentItem& b) { return a.name < b.name; });
        sco::Log("[plugin] loaded %s %s (data, %zu files)", p.manifest.id.c_str(), p.manifest.version.c_str(), mine.size());
        p.state = State::Loaded;
        for (auto& it : mine) items_.push_back(std::move(it));
    }
    return items_.size();
}

void ContentIndex::Clear() { items_.clear(); }

std::vector<const ContentItem*> ContentIndex::Items(ContentKind kind) const {
    std::vector<const ContentItem*> out;
    for (const auto& it : items_) if (it.kind == kind) out.push_back(&it);
    return out;
}

std::vector<const ContentItem*> ContentIndex::Find(ContentKind kind, std::string_view name) const {
    std::vector<const ContentItem*> out;
    for (const auto& it : items_) if (it.kind == kind && it.name == name) out.push_back(&it);
    return out;
}

std::vector<const ContentItem*> ContentIndex::FromPlugin(std::string_view plugin) const {
    std::vector<const ContentItem*> out;
    for (const auto& it : items_) if (it.plugin == plugin) out.push_back(&it);
    return out;
}

}  // namespace sco::plugins
