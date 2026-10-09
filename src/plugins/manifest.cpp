// plugin.ini parsing. Pure text in, Manifest out; no file system, no plugin code.
#include "sco/plugins.h"
#include <cstdio>
#include <cstring>

namespace sco::plugins {

const char* KindName(Kind k) {
    switch (k) {
        case Kind::Native: return "native";
        case Kind::Lua:    return "lua";
        case Kind::Data:   return "data";
        case Kind::Builtin: return "builtin";
    }
    return "?";
}

static std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// A comment starts with ';' or '#' at the start of the value or after a space or tab.
static std::string_view StripComment(std::string_view s) {
    for (size_t i = 0; i < s.size(); ++i)
        if ((s[i] == ';' || s[i] == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t'))
            return s.substr(0, i);
    return s;
}

static bool IdChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static bool ValidId(std::string_view s) {
    if (s.empty() || s.size() > kMaxIdLen) return false;
    for (char c : s) if (!IdChar(c)) return false;
    return true;
}

// Lowercase dotted: "teleport", "spawn.ship". Same alphabet as command names.
static bool ValidCapability(std::string_view s) {
    if (s.empty() || s.size() > kMaxCapabilityLen || s.front() == '.' || s.back() == '.') return false;
    char prev = 0;
    for (char c : s) {
        if (c == '.') { if (prev == '.') return false; }
        else if (!IdChar(c)) return false;
        prev = c;
    }
    return true;
}

// Printable ASCII and UTF-8 bytes; no control characters.
static bool ValidText(std::string_view s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

// A bare file name: no separators, no drive, not "." or "..".
static bool ValidEntry(std::string_view s) {
    if (s.empty() || s.size() > kMaxEntryLen || s == "." || s == "..") return false;
    for (unsigned char c : s)
        if (c < 0x20 || c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' ||
            c == '<' || c == '>' || c == '|') return false;
    return true;
}

static bool ParseApi(std::string_view s, uint16_t& major, uint16_t& minor) {
    const size_t dot = s.find('.');
    if (dot == std::string_view::npos || dot == 0 || dot + 1 == s.size()) return false;
    auto num = [](std::string_view d, uint16_t& out) {
        if (d.size() > 5) return false;
        uint32_t v = 0;
        for (char c : d) { if (c < '0' || c > '9') return false; v = v * 10 + static_cast<uint32_t>(c - '0'); }
        if (v > 0xffff) return false;
        out = static_cast<uint16_t>(v);
        return true;
    };
    return num(s.substr(0, dot), major) && num(s.substr(dot + 1), minor);
}

static bool Fail(std::string& error, size_t line, const char* what) {
    char buf[160];
    if (line) std::snprintf(buf, sizeof(buf), "line %zu: %s", line, what);
    else      std::snprintf(buf, sizeof(buf), "%s", what);
    error = buf;
    return false;
}

static bool ParseInto(std::string_view text, Manifest& out, std::string& error);

bool ParseManifest(std::string_view text, Manifest& out, std::string& error) {
    out = Manifest{};
    error.clear();
    if (ParseInto(text, out, error)) return true;
    out = Manifest{};   // no half-filled manifest on failure
    return false;
}

static bool ParseInto(std::string_view text, Manifest& out, std::string& error) {
    error.clear();
    if (text.size() > kMaxManifestBytes) return Fail(error, 0, "too big");
    // A UTF-16 BOM (FF FE or FE FF): every key would hold NULs and the error would be a confusing
    // "missing key 'id'".
    if (text.size() >= 2 && ((text[0] == '\xFF' && text[1] == '\xFE') || (text[0] == '\xFE' && text[1] == '\xFF')))
        return Fail(error, 0, "plugin.ini must be UTF-8");
    if (text.size() >= 3 && std::memcmp(text.data(), "\xEF\xBB\xBF", 3) == 0) text.remove_prefix(3);

    enum Key { kId, kName, kVersion, kAuthor, kApi, kKind, kEntry, kRequires, kCount };
    static constexpr const char* kKeys[kCount] = { "id", "name", "version", "author", "api", "kind", "entry", "requires" };
    bool seen[kCount] = {};

    size_t lineNo = 0;
    while (!text.empty()) {
        ++lineNo;
        const size_t nl = text.find('\n');
        std::string_view line = text.substr(0, nl);
        text.remove_prefix(nl == std::string_view::npos ? text.size() : nl + 1);

        line = Trim(StripComment(line));
        if (line.empty()) continue;
        const size_t eq = line.find('=');
        if (eq == std::string_view::npos) return Fail(error, lineNo, "expected key = value");
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));

        int k = -1;
        for (int i = 0; i < kCount; ++i) if (key == kKeys[i]) k = i;
        if (k < 0) continue;                                    // unknown key: a later minor's
        if (seen[k]) return Fail(error, lineNo, (std::string("duplicate key '") + kKeys[k] + "'").c_str());
        seen[k] = true;

        switch (k) {
            case kId:
                if (!ValidId(value)) return Fail(error, lineNo, "id must be 1-31 of [a-z0-9_]");
                for (const char* r : kReservedIds)
                    if (value == r) return Fail(error, lineNo, "id is reserved");
                out.id = value;
                break;
            case kName:
                if (!ValidText(value, kMaxNameLen)) return Fail(error, lineNo, "name must be 1-63 printable characters");
                out.name = value;
                break;
            case kVersion:
                if (!ValidText(value, kMaxVersionLen)) return Fail(error, lineNo, "version must be 1-31 printable characters");
                out.version = value;
                break;
            case kAuthor:
                if (!ValidText(value, kMaxAuthorLen)) return Fail(error, lineNo, "author must be 1-63 printable characters");
                out.author = value;
                break;
            case kApi:
                if (!ParseApi(value, out.apiMajor, out.apiMinor)) return Fail(error, lineNo, "api must be <major>.<minor>");
                break;
            case kKind:
                if (value == "native")    out.kind = Kind::Native;
                else if (value == "lua")  out.kind = Kind::Lua;
                else if (value == "data") out.kind = Kind::Data;
                else return Fail(error, lineNo, "kind must be native, lua or data");
                break;
            case kEntry:
                if (!ValidEntry(value)) return Fail(error, lineNo, "entry must be a file name in the plugin folder");
                out.entry = value;
                break;
            case kRequires: {
                std::string_view rest = value;
                while (true) {
                    const size_t comma = rest.find(',');
                    const std::string_view cap = Trim(rest.substr(0, comma));
                    if (!ValidCapability(cap)) return Fail(error, lineNo, "requires must be a comma list of capability names");
                    for (const auto& have : out.requires_)
                        if (have == cap) return Fail(error, lineNo, "requires lists a capability twice");
                    if (out.requires_.size() == kMaxRequires) return Fail(error, lineNo, "requires lists more than 16 capabilities");
                    out.requires_.emplace_back(cap);
                    if (comma == std::string_view::npos) break;
                    rest.remove_prefix(comma + 1);
                }
                break;
            }
        }
    }

    for (int k : { kId, kName, kVersion, kApi, kKind })
        if (!seen[k]) return Fail(error, 0, (std::string("missing key '") + kKeys[k] + "'").c_str());
    if (out.kind == Kind::Data && seen[kEntry]) return Fail(error, 0, "a data pack has no entry");
    if (out.kind != Kind::Data && !seen[kEntry]) return Fail(error, 0, "missing key 'entry'");
    return true;
}

}  // namespace sco::plugins
