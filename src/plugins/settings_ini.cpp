// plugin.ini [settings]: the grammar of one declaration, and reading, checking and writing one
// value. Pure text in, Setting out; no file system, no plugin code. docs/plugins.md has the grammar.
//
//   [settings]
//   god_mode   = bool   default false label "God mode" help "Take no damage"
//   speed      = int    default 5 min 1 max 10
//   fov        = float  default 90 min 60 max 120
//   nickname   = string default "Pilot"
//   difficulty = enum(easy,normal,hard) default normal
#include "sco/plugins.h"
#include "internal.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sco::plugins {

const char* SettingTypeName(SettingType t) {
    switch (t) {
        case SettingType::Bool:   return "bool";
        case SettingType::Int:    return "int";
        case SettingType::Float:  return "float";
        case SettingType::String: return "string";
        case SettingType::Enum:   return "enum";
    }
    return "?";
}

namespace {

bool IsSpace(char c) { return c == ' ' || c == '\t'; }

std::string_view Trim(std::string_view s) {
    while (!s.empty() && IsSpace(s.front())) s.remove_prefix(1);
    while (!s.empty() && (IsSpace(s.back()) || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

bool WordChar(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

bool ValidWord(std::string_view s, size_t max) {
    if (s.empty() || s.size() > max) return false;
    for (char c : s) if (!WordChar(c)) return false;
    return true;
}

// Printable ASCII and UTF-8 bytes; no control characters. Empty only when allowEmpty.
bool ValidText(std::string_view s, size_t max, bool allowEmpty) {
    if ((s.empty() && !allowEmpty) || s.size() > max) return false;
    for (unsigned char c : s) if (c < 0x20 || c == 0x7f) return false;
    return true;
}

bool IsDigit(char c) { return c >= '0' && c <= '9'; }

// [+-]digits, at most 19 digits, in int64 range.
bool ParseInt(std::string_view s, int64_t& out) {
    size_t i = 0;
    bool neg = false;
    if (!s.empty() && (s[0] == '+' || s[0] == '-')) { neg = s[0] == '-'; i = 1; }
    if (i == s.size() || s.size() - i > 19) return false;
    uint64_t v = 0;
    for (; i < s.size(); ++i) {
        if (!IsDigit(s[i])) return false;
        v = v * 10 + static_cast<uint64_t>(s[i] - '0');
    }
    if (neg) {
        if (v > 9223372036854775808ull) return false;
        out = v == 9223372036854775808ull ? INT64_MIN : -static_cast<int64_t>(v);
    } else {
        if (v > static_cast<uint64_t>(INT64_MAX)) return false;
        out = static_cast<int64_t>(v);
    }
    return true;
}

// [+-]digits[.digits][(e|E)[+-]digits], finite. No nan, inf, hex or leading/trailing dot.
bool ParseFloat(std::string_view s, double& out) {
    if (s.size() > 64) return false;
    size_t i = 0;
    if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
    size_t d = i;
    while (i < s.size() && IsDigit(s[i])) ++i;
    if (i == d) return false;
    if (i < s.size() && s[i] == '.') {
        ++i;
        const size_t f = i;
        while (i < s.size() && IsDigit(s[i])) ++i;
        if (i == f) return false;
    }
    if (i < s.size() && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        if (i < s.size() && (s[i] == '+' || s[i] == '-')) ++i;
        const size_t e = i;
        while (i < s.size() && IsDigit(s[i])) ++i;
        if (i == e) return false;
    }
    if (i != s.size()) return false;
    char buf[72];
    std::memcpy(buf, s.data(), s.size());
    buf[s.size()] = 0;
    char* end = nullptr;
    const double v = std::strtod(buf, &end);   // the C locale: the host never calls setlocale
    if (end != buf + s.size() || !std::isfinite(v)) return false;
    out = v;
    return true;
}

std::string Quote(std::string_view s) { return "'" + std::string(s) + "'"; }

std::string Choices(const Setting& s) {
    std::string out;
    for (const auto& c : s.choices) out += (out.empty() ? "" : ", ") + c;
    return out;
}

}  // namespace

std::string FormatSettingValue(const Setting& s, const SettingValue& v) {
    switch (s.type) {
        case SettingType::Bool:   return v.b ? "true" : "false";
        case SettingType::Int:    return std::to_string(v.i);
        case SettingType::Float: {
            char buf[40];
            std::snprintf(buf, sizeof(buf), "%.17g", v.f);
            return buf;
        }
        case SettingType::String:
        case SettingType::Enum:   return v.s;
    }
    return {};
}

bool CheckSettingValue(const Setting& s, const SettingValue& v, std::string& why) {
    why.clear();
    switch (s.type) {
        case SettingType::Bool:
            return true;
        case SettingType::Int:
            if (s.hasMin && v.i < s.minI) { why = std::to_string(v.i) + " is below min " + std::to_string(s.minI); return false; }
            if (s.hasMax && v.i > s.maxI) { why = std::to_string(v.i) + " is above max " + std::to_string(s.maxI); return false; }
            return true;
        case SettingType::Float:
            if (!std::isfinite(v.f)) { why = "not a finite number"; return false; }
            if (s.hasMin && v.f < s.minF) { why = FormatSettingValue(s, v) + " is below min " + FormatSettingValue(s, SettingValue{ false, 0, s.minF, {} }); return false; }
            if (s.hasMax && v.f > s.maxF) { why = FormatSettingValue(s, v) + " is above max " + FormatSettingValue(s, SettingValue{ false, 0, s.maxF, {} }); return false; }
            return true;
        case SettingType::String:
            if (!ValidText(v.s, kMaxSettingStringLen, true)) { why = "a string is at most 255 printable characters"; return false; }
            return true;
        case SettingType::Enum:
            for (const auto& c : s.choices) if (c == v.s) return true;
            why = Quote(v.s) + " is not one of " + Choices(s);
            return false;
    }
    return false;
}

bool ParseSettingValue(const Setting& s, std::string_view text, SettingValue& out, std::string& why) {
    out = SettingValue{};
    why.clear();
    switch (s.type) {
        case SettingType::Bool:
            if (text == "true") out.b = true;
            else if (text != "false") { why = Quote(text) + " is not true or false"; return false; }
            break;
        case SettingType::Int:
            if (!ParseInt(text, out.i)) { why = Quote(text) + " is not an integer"; return false; }
            break;
        case SettingType::Float:
            if (!ParseFloat(text, out.f)) { why = Quote(text) + " is not a number"; return false; }
            break;
        case SettingType::String:
        case SettingType::Enum:
            out.s = std::string(text);
            break;
    }
    if (!CheckSettingValue(s, out, why)) { out = SettingValue{}; return false; }
    return true;
}

namespace detail {

std::string_view StripSettingsComment(std::string_view line) {
    bool quoted = false;
    for (size_t i = 0; i < line.size(); ++i) {
        const char c = line[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ';' || c == '#') && (i == 0 || IsSpace(line[i - 1]))) return line.substr(0, i);
    }
    return line;
}

namespace {

struct Token {
    std::string_view text;
    bool             quoted = false;
};

// Tokens are separated by spaces or tabs; a token is a bare word or "text in double quotes" (no
// escapes; a quote ends it, and must be followed by a separator).
struct Lexer {
    std::string_view s;
    size_t           p = 0;
    bool             bad = false;   // an unterminated or misplaced quote

    void Skip() { while (p < s.size() && IsSpace(s[p])) ++p; }

    bool Next(Token& t) {
        Skip();
        if (p >= s.size()) return false;
        if (s[p] == '"') {
            const size_t e = s.find('"', p + 1);
            if (e == std::string_view::npos) { bad = true; return false; }
            t = { s.substr(p + 1, e - p - 1), true };
            p = e + 1;
            if (p < s.size() && !IsSpace(s[p])) { bad = true; return false; }
            return true;
        }
        const size_t b = p;
        while (p < s.size() && !IsSpace(s[p])) {
            if (s[p] == '"') { bad = true; return false; }
            ++p;
        }
        t = { s.substr(b, p - b), false };
        return true;
    }
};

// "enum(a, b, c)" -> the choices.
bool ParseEnumType(std::string_view t, Setting& s, std::string& why) {
    t.remove_prefix(5);   // "enum("
    t.remove_suffix(1);   // ")"
    size_t start = 0;
    for (;;) {
        const size_t comma = t.find(',', start);
        const std::string_view c = Trim(t.substr(start, comma == std::string_view::npos ? std::string_view::npos : comma - start));
        if (!ValidWord(c, kMaxSettingChoiceLen)) { why = "enum choices are a comma list of 1-31 of [a-z0-9_]"; return false; }
        for (const auto& have : s.choices)
            if (have == c) { why = "enum lists a choice twice"; return false; }
        if (s.choices.size() == kMaxSettingChoices) { why = "enum lists more than 16 choices"; return false; }
        s.choices.emplace_back(c);
        if (comma == std::string_view::npos) break;
        start = comma + 1;
    }
    return true;
}

bool ParseLine(std::string_view line, std::vector<Setting>& out, std::string& why) {
    const size_t eq = line.find('=');
    if (eq == std::string_view::npos) { why = "expected name = type ..."; return false; }
    const std::string_view name = Trim(line.substr(0, eq));
    if (!ValidWord(name, kMaxSettingNameLen)) { why = "setting name must be 1-31 of [a-z0-9_]"; return false; }
    const std::string prefix = "setting " + Quote(name) + ": ";
    auto fail = [&](const std::string& what) { why = prefix + what; return false; };
    for (const auto& have : out)
        if (have.name == name) return fail("declared twice");
    if (out.size() == kMaxSettings) return fail("more than 32 settings");

    Setting s;
    s.name = std::string(name);
    Lexer lex;
    lex.s = line.substr(eq + 1);

    // The type: a bare word, or enum(...) which may hold spaces after its commas.
    lex.Skip();
    std::string_view type;
    if (lex.s.substr(lex.p).starts_with("enum(")) {
        const size_t close = lex.s.find(')', lex.p);
        if (close == std::string_view::npos) return fail("enum( needs a closing )");
        type = lex.s.substr(lex.p, close + 1 - lex.p);
        lex.p = close + 1;
        if (lex.p < lex.s.size() && !IsSpace(lex.s[lex.p])) return fail("expected a space after the type");
    } else {
        Token t;
        if (!lex.Next(t) || t.quoted) return fail("missing type (bool, int, float, string or enum(a,b,c))");
        type = t.text;
    }
    if (type == "bool") s.type = SettingType::Bool;
    else if (type == "int") s.type = SettingType::Int;
    else if (type == "float") s.type = SettingType::Float;
    else if (type == "string") s.type = SettingType::String;
    else if (type.starts_with("enum(")) {
        s.type = SettingType::Enum;
        std::string e;
        if (!ParseEnumType(type, s, e)) return fail(e);
    } else {
        return fail("type must be bool, int, float, string or enum(a,b,c)");
    }

    // Fields: key value pairs, in any order, each at most once.
    enum { kDefault, kMin, kMax, kLabel, kHelp, kFields };
    static constexpr const char* kNames[kFields] = { "default", "min", "max", "label", "help" };
    bool have[kFields] = {};
    std::string_view value[kFields];
    Token key, v;
    while (lex.Next(key)) {
        int f = -1;
        for (int i = 0; i < kFields; ++i) if (!key.quoted && key.text == kNames[i]) f = i;
        if (f < 0) return fail("unknown field " + Quote(key.text) + " (default, min, max, label, help)");
        if (have[f]) return fail(std::string("field '") + kNames[f] + "' given twice");
        if (!lex.Next(v)) {
            if (lex.bad) break;
            return fail(std::string("field '") + kNames[f] + "' needs a value");
        }
        have[f] = true;
        value[f] = v.text;
    }
    if (lex.bad) return fail("a quote is not closed, or not at the start of a value");

    if (have[kMin] || have[kMax]) {
        if (s.type != SettingType::Int && s.type != SettingType::Float) return fail("min and max apply to int and float only");
        Setting probe = s;   // the bounds are parsed as values of the type, before they exist
        for (int f : { static_cast<int>(kMin), static_cast<int>(kMax) }) {
            if (!have[f]) continue;
            SettingValue b;
            std::string e;
            if (!ParseSettingValue(probe, value[f], b, e)) return fail(std::string(kNames[f]) + " " + e);
            if (f == kMin) { s.hasMin = true; s.minI = b.i; s.minF = b.f; }
            else           { s.hasMax = true; s.maxI = b.i; s.maxF = b.f; }
        }
        if (s.hasMin && s.hasMax &&
            (s.type == SettingType::Int ? s.minI > s.maxI : s.minF > s.maxF))
            return fail("min " + std::string(value[kMin]) + " is above max " + std::string(value[kMax]));
    }
    if (have[kLabel]) {
        if (!ValidText(value[kLabel], kMaxSettingLabelLen, false)) return fail("label must be 1-63 printable characters");
        s.label = std::string(value[kLabel]);
    } else {
        s.label = s.name;
    }
    if (have[kHelp]) {
        if (!ValidText(value[kHelp], kMaxSettingHelpLen, false)) return fail("help must be 1-255 printable characters");
        s.help = std::string(value[kHelp]);
    }
    if (have[kDefault]) {
        std::string e;
        if (!ParseSettingValue(s, value[kDefault], s.def, e)) return fail("default " + e);
    } else {   // false, 0 (or the nearest bound), "", the first choice
        if (s.type == SettingType::Int) {
            s.def.i = 0;
            if (s.hasMin && s.def.i < s.minI) s.def.i = s.minI;
            if (s.hasMax && s.def.i > s.maxI) s.def.i = s.maxI;
        } else if (s.type == SettingType::Float) {
            s.def.f = 0;
            if (s.hasMin && s.def.f < s.minF) s.def.f = s.minF;
            if (s.hasMax && s.def.f > s.maxF) s.def.f = s.maxF;
        } else if (s.type == SettingType::Enum) {
            s.def.s = s.choices.front();
        }
    }
    out.push_back(std::move(s));
    return true;
}

}  // namespace

bool ParseSettingLine(std::string_view line, std::vector<Setting>& out, std::string& why) {
    why.clear();
    return ParseLine(line, out, why);
}

}  // namespace detail

}  // namespace sco::plugins
