// sco::datacore data packs (sco/datacore_pack.h): the .toml format of a pack's datacore\ folder,
// docs/design/vfs-datacore.md section 5 and docs/datacore.md "Pack format". ParsePack reads TOML with
// vendored toml++ (no exceptions) into PackOps, checking everything that needs no game file;
// ApplyPacks runs them through sco::datacore::Patch in plugin order with per-pack atomicity.
#include "sco/datacore_pack.h"
#include "internal.h"
#include "toml.h"
#include <algorithm>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

namespace sco::datacore {

using detail::Fmt;

namespace {

std::string Q(std::string_view s) {
    std::string q = "\"";
    for (const char c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (c == '"' || c == '\\') { q += '\\'; q += c; }
        else if (c == '\n') q += "\\n";
        else if (c == '\t') q += "\\t";
        else if (c == '\r') q += "\\r";
        else if (u < 0x20 || u == 0x7F) q += Fmt("\\u%04x", static_cast<unsigned>(u));
        else q += c;
    }
    return q + "\"";
}

std::string FloatText(double d) {
    if (std::isnan(d)) return "nan";
    if (std::isinf(d)) return d < 0 ? "-inf" : "inf";
    char buf[40] = "";
    for (int prec = 15; prec <= 17; ++prec) {
        std::snprintf(buf, sizeof(buf), "%.*g", prec, d);
        if (std::strtod(buf, nullptr) == d) break;
    }
    std::string s = buf;
    if (s.find_first_of(".en") == std::string::npos) s += ".0";
    return s;
}

std::string ValueText(const Value& v) {
    switch (v.kind) {
    case Value::Kind::Bool: return v.b ? "true" : "false";
    case Value::Kind::Int: return std::to_string(v.i);
    case Value::Kind::UInt:
        return v.u > static_cast<uint64_t>(INT64_MAX) ? "{ uint = \"" + std::to_string(v.u) + "\" }" : std::to_string(v.u);
    case Value::Kind::Float: return FloatText(v.f);
    case Value::Kind::String: return Q(v.s);
    case Value::Kind::Guid: return "{ guid = \"" + FormatGuid(v.guid) + "\" }";
    case Value::Kind::Enum: return "{ enum = " + Q(v.s) + " }";
    case Value::Kind::Null: case Value::Kind::Instance: break;
    }
    return "\"\"";
}

// ---- parsing -----------------------------------------------------------------------------------

uint32_t LineOf(const toml::node& n) { return static_cast<uint32_t>(n.source().begin.line); }

struct Parser {
    Pack& out;
    std::string& error;
    std::set<std::string> ids;                          // [[instance]] ids defined so far
    std::map<std::string, uint32_t> fieldsSet;          // target + field -> line (a field set twice)
    size_t opCount = 0;

    bool Fail(uint32_t line, const std::string& what) {
        error = line ? Fmt("%u: ", line) + what : what;
        return false;
    }

    static bool Typed(const toml::table& t, std::string_view key, std::string& s) {
        if (t.size() != 1) return false;
        const toml::node* n = t.get(key);
        if (!n || !n->is_string()) return false;
        s = n->as_string()->get();
        return true;
    }

    // A value: number, string, bool, { guid = "..." }, { enum = "..." }, { uint = "..." }.
    bool ParseValue(const toml::node& n, Value& v, const std::string& where) {
        const uint32_t line = LineOf(n);
        if (const auto* i = n.as_integer()) { v = Value::OfInt(i->get()); return true; }
        if (const auto* f = n.as_floating_point()) { v = Value::OfFloat(f->get()); return true; }
        if (const auto* b = n.as_boolean()) { v = Value::OfBool(b->get()); return true; }
        if (const auto* s = n.as_string()) { v = Value::OfString(s->get()); return true; }
        if (const auto* t = n.as_table()) {
            std::string s;
            if (Typed(*t, "guid", s)) {
                Guid g;
                if (!ParseGuid(s, g)) return Fail(line, where + ": bad guid " + Q(s) + " (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
                v = Value::OfGuid(g);
                return true;
            }
            if (Typed(*t, "enum", s)) {
                if (s.empty()) return Fail(line, where + ": empty enum option");
                v = Value::OfEnum(s);
                return true;
            }
            if (Typed(*t, "uint", s)) {
                char* end = nullptr;
                const unsigned long long u = s.empty() || s[0] == '-' ? 0 : std::strtoull(s.c_str(), &end, 10);
                if (!end || *end || s.size() > 20 || (u == ULLONG_MAX && s != "18446744073709551615"))
                    return Fail(line, where + ": bad uint " + Q(s) + " (decimal digits, up to 18446744073709551615)");
                v = Value::OfUInt(u);
                return true;
            }
        }
        return Fail(line, where + ": a value is a number, a string, true/false, { guid = \"...\" }, { enum = \"...\" } or { uint = \"...\" }");
    }

    bool String(const toml::table& t, std::string_view key, std::string& s, const std::string& where, bool required) {
        const toml::node* n = t.get(key);
        if (!n) return required ? Fail(LineOf(t), where + ": missing " + std::string(key)) : true;
        if (!n->is_string()) return Fail(LineOf(*n), where + ": " + std::string(key) + " must be a string");
        s = n->as_string()->get();
        if (s.empty()) return Fail(LineOf(*n), where + ": empty " + std::string(key));
        return true;
    }

    bool Keys(const toml::table& t, std::initializer_list<std::string_view> allowed, const std::string& where) {
        for (auto&& [k, v] : t)
            if (std::find(allowed.begin(), allowed.end(), k.str()) == allowed.end())
                return Fail(static_cast<uint32_t>(k.source().begin.line ? k.source().begin.line : LineOf(v)),
                            where + ": unknown key " + Q(k.str()));
        return true;
    }

    bool Field(const toml::table& t, std::string& field, const std::string& where) {
        if (!String(t, "field", field, where, true)) return false;
        if (Status st = CheckFieldPath(field); !st) return Fail(LineOf(*t.get("field")), where + ": field " + Q(field) + ": " + st.message);
        return true;
    }

    bool Local(const toml::node& n, std::string& id, const std::string& where, std::string_view key) {
        const std::string s = n.as_string() ? n.as_string()->get() : std::string();
        if (s.size() < 2 || s[0] != '@')
            return Fail(LineOf(n), where + ": " + std::string(key) + " must be \"@id\" naming an earlier [[instance]]");
        id = s.substr(1);
        if (!ids.count(id)) return Fail(LineOf(n), where + ": " + Q(s) + " is not defined by an earlier [[instance]]");
        return true;
    }

    // record and/or guid -> RecordRef. False with an error when present but bad; `present` says whether any was given.
    bool Record(const toml::table& t, std::optional<RecordRef>& ref, const std::string& where) {
        ref.reset();
        RecordRef r;
        bool any = false;
        if (t.get("record")) {
            if (!String(t, "record", r.name, where, true)) return false;
            any = true;
        }
        if (const toml::node* g = t.get("guid")) {
            std::string s;
            if (!String(t, "guid", s, where, true)) return false;
            Guid id;
            if (!ParseGuid(s, id)) return Fail(LineOf(*g), where + ": bad guid " + Q(s) + " (xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx)");
            r.guid = id;
            any = true;
        }
        if (any) ref = std::move(r);
        return true;
    }

    bool Source(const toml::node& n, InstanceSource& src, const std::string& where) {
        const toml::table* t = n.as_table();
        if (!t) return Fail(LineOf(n), where + " must be { record = \"...\", field = \"...\" } (or guid = \"...\")");
        if (!Keys(*t, { "record", "guid", "field" }, where)) return false;
        if (!Record(*t, src.record, where)) return false;
        if (!src.record) return Fail(LineOf(n), where + ": needs record or guid");
        if (const toml::node* f = t->get("field")) {
            if (!String(*t, "field", src.field, where, true)) return false;
            if (Status st = CheckFieldPath(src.field); !st) return Fail(LineOf(*f), where + ": field " + Q(src.field) + ": " + st.message);
        }
        return true;
    }

    // set = { "path" = value, ... }; a nested table that isn't a typed value is a dotted path.
    bool Sets(const toml::table& t, const std::string& prefix, std::vector<std::pair<std::string, Value>>& sets,
              const std::string& where) {
        std::vector<std::pair<const toml::key*, const toml::node*>> items;
        for (auto&& [k, v] : t) items.emplace_back(&k, &v);
        std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
            const auto& x = a.first->source().begin;
            const auto& y = b.first->source().begin;
            return x.line != y.line ? x.line < y.line : x.column < y.column;
        });
        for (const auto& [k, v] : items) {
            const std::string path = prefix.empty() ? std::string(k->str()) : prefix + "." + std::string(k->str());
            const toml::table* sub = v->as_table();
            std::string s;
            if (sub && !Typed(*sub, "guid", s) && !Typed(*sub, "enum", s) && !Typed(*sub, "uint", s)) {
                if (!Sets(*sub, path, sets, where)) return false;
                continue;
            }
            if (Status st = CheckFieldPath(path); !st) return Fail(LineOf(*v), where + ": set " + Q(path) + ": " + st.message);
            Value val;
            if (!ParseValue(*v, val, where + ": set " + Q(path))) return false;
            sets.emplace_back(path, std::move(val));
        }
        return true;
    }

    bool Count(size_t n, uint32_t line) {
        opCount += n;
        if (opCount > kMaxPackOps) return Fail(line, Fmt("more than %zu operations in one file", kMaxPackOps));
        return true;
    }

    std::string TargetKey(const PackOp& op) const {
        if (!op.instance.empty()) return "@" + op.instance;
        return op.record->name + "|" + (op.record->guid ? FormatGuid(*op.record->guid) : std::string());
    }
    bool Once(const std::string& target, const std::string& field, uint32_t line, const std::string& where) {
        const auto [it, fresh] = fieldsSet.emplace(target + "\x1f" + field, line);
        if (!fresh) return Fail(line, where + ": field " + Q(field) + " is already set at line " + std::to_string(it->second));
        return true;
    }

    bool Instance(const toml::table& t, uint32_t line) {
        const std::string where = "[[instance]]";
        if (!Keys(t, { "id", "struct", "clone", "set" }, where)) return false;
        PackOp op;
        op.kind = PackOp::Kind::Instance;
        op.line = line;
        if (!String(t, "id", op.id, where, true) || !String(t, "struct", op.type, where, true)) return false;
        if (op.id[0] == '@') return Fail(LineOf(*t.get("id")), where + ": id is written without '@' (use it as \"@" + op.id.substr(1) + "\")");
        for (const char c : op.id)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-')
                return Fail(LineOf(*t.get("id")), where + ": id " + Q(op.id) + ": letters, digits, '_' and '-' only");
        if (ids.count(op.id)) return Fail(LineOf(*t.get("id")), where + ": id " + Q(op.id) + " is defined twice");
        if (const toml::node* c = t.get("clone"))
            if (!Source(*c, op.clone, where + ": clone")) return false;
        if (const toml::node* s = t.get("set")) {
            if (!s->is_table()) return Fail(LineOf(*s), where + ": set must be a table { \"field\" = value, ... }");
            if (!Sets(*s->as_table(), {}, op.sets, where)) return false;
            for (const auto& [path, v] : op.sets)
                if (!Once("@" + op.id, path, line, where)) return false;
        }
        ids.insert(op.id);
        if (!Count(1 + op.sets.size(), line)) return false;
        out.ops.push_back(std::move(op));
        return true;
    }

    bool Change(const toml::table& t, uint32_t line, bool append) {
        const std::string where = append ? "[[append]]" : "[[set]]";
        if (append ? !Keys(t, { "record", "guid", "instance", "field", "value", "pointer", "element" }, where)
                   : !Keys(t, { "record", "guid", "instance", "field", "value", "pointer" }, where))
            return false;
        PackOp op;
        op.kind = append ? PackOp::Kind::Append : PackOp::Kind::Set;
        op.line = line;
        if (!Record(t, op.record, where)) return false;
        if (const toml::node* i = t.get("instance")) {
            if (op.record) return Fail(LineOf(*i), where + ": give record/guid or instance, not both");
            if (!Local(*i, op.instance, where, "instance")) return false;
        } else if (!op.record) {
            return Fail(line, where + ": needs record, guid or instance = \"@id\"");
        }
        if (!Field(t, op.field, where)) return false;
        const toml::node* value = t.get("value");
        const toml::node* pointer = t.get("pointer");
        const toml::node* element = t.get("element");
        if ((value ? 1 : 0) + (pointer ? 1 : 0) + (element ? 1 : 0) != 1)
            return Fail(line, where + (append ? ": needs exactly one of value, pointer or element" : ": needs exactly one of value or pointer"));
        size_t count = 1;
        if (pointer) {
            if (const auto* s = pointer->as_string(); s && s->get() == "null") {
                op.pointer.kind = PackPointer::Kind::Null;
            } else if (pointer->is_table()) {
                op.pointer.kind = PackPointer::Kind::Existing;
                if (!Source(*pointer, op.pointer.existing, where + ": pointer")) return false;
            } else {
                op.pointer.kind = PackPointer::Kind::Local;
                if (!Local(*pointer, op.pointer.local, where, "pointer")) return false;
            }
        } else if (element) {
            if (!Local(*element, op.element, where, "element")) return false;
        } else if (const toml::table* vt = value->as_table(); append && vt && vt->get("struct")) {
            // value = { struct = "...", clone = { ... }, set = { ... } }: a new element.
            const std::string w = where + ": value";
            if (!Keys(*vt, { "struct", "clone", "set" }, w) || !String(*vt, "struct", op.type, w, true)) return false;
            if (const toml::node* c = vt->get("clone"))
                if (!Source(*c, op.clone, w + ": clone")) return false;
            if (const toml::node* s = vt->get("set")) {
                if (!s->is_table()) return Fail(LineOf(*s), w + ": set must be a table { \"field\" = value, ... }");
                if (!Sets(*s->as_table(), {}, op.sets, w)) return false;
            }
            count = 2 + op.sets.size();
        } else {
            Value v;
            if (!ParseValue(*value, v, where + ": value")) return false;
            op.value = std::move(v);
        }
        if (!append && !Once(TargetKey(op), op.field, line, where)) return false;
        if (!Count(count, line)) return false;
        out.ops.push_back(std::move(op));
        return true;
    }

    bool Run(std::string_view text) {
        if (text.size() > kMaxPackBytes) return Fail(0, Fmt("file is %zu bytes, over the %zu-byte limit", text.size(), kMaxPackBytes));
        toml::parse_result r = toml::parse(text);
        if (!r) {
            const toml::parse_error& e = r.error();
            return Fail(static_cast<uint32_t>(e.source().begin.line), "TOML: " + std::string(e.description()));
        }
        const toml::table& t = r.table();
        bool haveFormat = false;
        struct Item { uint32_t line; int kind; const toml::table* t; };
        std::vector<Item> items;
        for (auto&& [k, v] : t) {
            const std::string_view key = k.str();
            const uint32_t line = static_cast<uint32_t>(k.source().begin.line);
            if (key == "format") {
                const auto* i = v.as_integer();
                if (!i || i->get() != kPackFormat) return Fail(line, Fmt("format must be %d (the pack file format; not a game version)", kPackFormat));
                haveFormat = true;
            } else if (key == "atomic") {
                const auto* b = v.as_boolean();
                if (!b) return Fail(line, "atomic must be true or false");
                out.atomic = b->get();
            } else if (key == "set" || key == "instance" || key == "append") {
                const int kind = key == "set" ? 0 : key == "instance" ? 1 : 2;
                const toml::array* a = v.as_array();
                if (!a) return Fail(line, "write [[" + std::string(key) + "]] (an array of tables), not [" + std::string(key) + "]");
                for (const toml::node& e : *a) {
                    const toml::table* et = e.as_table();
                    if (!et) return Fail(LineOf(e), std::string(key) + " entries must be tables: [[" + std::string(key) + "]]");
                    items.push_back({ LineOf(e), kind, et });
                }
            } else {
                return Fail(line, "unknown key " + Q(key) + " (format, atomic, [[set]], [[instance]], [[append]])");
            }
        }
        if (!haveFormat) return Fail(1, Fmt("missing format = %d", kPackFormat));
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.line < b.line; });
        for (const Item& it : items) {
            const bool ok = it.kind == 1 ? Instance(*it.t, it.line) : Change(*it.t, it.line, it.kind == 2);
            if (!ok) return false;
        }
        return true;
    }
};

}  // namespace

bool ParsePack(std::string_view text, Pack& out, std::string& error) {
    Pack p;
    p.plugin = out.plugin;
    p.name = out.name;
    error.clear();
    Parser parser{ p, error, {}, {}, 0 };
    if (!parser.Run(text)) {
        out.ops.clear();
        out.atomic = true;
        return false;
    }
    out = std::move(p);
    return true;
}

// ---- writing -----------------------------------------------------------------------------------

namespace {

std::string RefText(const RecordRef& r) {
    std::string s;
    if (!r.name.empty()) s += "record = " + Q(r.name);
    if (r.guid) s += std::string(s.empty() ? "" : ", ") + "guid = " + Q(FormatGuid(*r.guid));
    return s;
}
std::string SourceText(const InstanceSource& src) {
    std::string s = "{ " + (src.record ? RefText(*src.record) : std::string());
    if (!src.field.empty()) s += ", field = " + Q(src.field);
    return s + " }";
}
std::string SetsText(const std::vector<std::pair<std::string, Value>>& sets) {
    std::string s = "{ ";
    for (size_t i = 0; i < sets.size(); ++i) s += (i ? ", " : "") + Q(sets[i].first) + " = " + ValueText(sets[i].second);
    return s + " }";
}
std::string RefLines(const RecordRef& r) {
    std::string s;
    if (!r.name.empty()) s += "record = " + Q(r.name) + "\n";
    if (r.guid) s += "guid = " + Q(FormatGuid(*r.guid)) + "\n";
    return s;
}

}  // namespace

std::string WritePack(const Pack& p, std::string_view comment) {
    std::string s;
    while (!comment.empty()) {
        const size_t nl = comment.find('\n');
        s += "# " + std::string(comment.substr(0, nl)) + "\n";
        if (nl == std::string_view::npos) break;
        comment.remove_prefix(nl + 1);
    }
    s += Fmt("format = %d\n", kPackFormat);
    s += std::string("atomic = ") + (p.atomic ? "true" : "false") + "\n";
    for (const PackOp& op : p.ops) {
        if (op.kind == PackOp::Kind::Instance) {
            s += "\n[[instance]]\nid = " + Q(op.id) + "\nstruct = " + Q(op.type) + "\n";
            if (op.clone.record) s += "clone = " + SourceText(op.clone) + "\n";
            if (!op.sets.empty()) s += "set = " + SetsText(op.sets) + "\n";
            continue;
        }
        s += op.kind == PackOp::Kind::Set ? "\n[[set]]\n" : "\n[[append]]\n";
        if (!op.instance.empty()) s += "instance = " + Q("@" + op.instance) + "\n";
        else if (op.record) s += RefLines(*op.record);
        s += "field = " + Q(op.field) + "\n";
        if (op.pointer.kind == PackPointer::Kind::Null) s += "pointer = \"null\"\n";
        else if (op.pointer.kind == PackPointer::Kind::Local) s += "pointer = " + Q("@" + op.pointer.local) + "\n";
        else if (op.pointer.kind == PackPointer::Kind::Existing) s += "pointer = " + SourceText(op.pointer.existing) + "\n";
        else if (!op.element.empty()) s += "element = " + Q("@" + op.element) + "\n";
        else if (!op.type.empty()) {
            s += "value = { struct = " + Q(op.type);
            if (op.clone.record) s += ", clone = " + SourceText(op.clone);
            if (!op.sets.empty()) s += ", set = " + SetsText(op.sets);
            s += " }\n";
        } else if (op.value) s += "value = " + ValueText(*op.value) + "\n";
    }
    return s;
}

// ---- applying ----------------------------------------------------------------------------------

const char* PackStateName(PackState s) {
    switch (s) {
    case PackState::Applied: return "applied";
    case PackState::Partial: return "partial";
    case PackState::Refused: return "refused";
    }
    return "?";
}

namespace {

using SlotKey = std::pair<uint64_t, uint64_t>;
struct Owner { size_t pack; uint32_t line; std::string op; };

// Runs one PackOp's patcher calls on `p`. Appends one report per call; `slots` gets the slots written.
struct Exec {
    Patch& p;
    std::map<std::string, InstanceId>& ids;
    std::vector<PackOpReport>& reports;
    std::vector<std::pair<SlotKey, size_t>>& slots;   // slot -> index into reports

    bool Note(const PackOp& op, Status st, const std::string& fallback) {
        const bool ok = st.ok();
        const auto& r = p.Reports();
        std::string text = fallback;
        if (!r.empty() && fallback.empty()) text = r.back().op;
        if (ok && fallback.empty() && !r.empty() && r.back().slot)
            slots.emplace_back(SlotKey{ r.back().slot->region, r.back().slot->offset }, reports.size());
        reports.push_back({ op.line, std::move(text), std::move(st) });
        return ok;
    }
    InstanceId Id(const std::string& local) const {
        const auto it = ids.find(local);
        return it == ids.end() ? InstanceId{} : it->second;
    }
    template <class Fn>
    Status OnTarget(const PackOp& op, Fn&& fn) {
        if (!op.instance.empty()) return fn(Id(op.instance));
        return fn(*op.record);
    }
    std::string Where(const PackOp& op) const {
        std::string t;
        if (!op.instance.empty()) t = "instance @" + op.instance;
        else if (!op.record->name.empty()) t = "record " + Q(op.record->name);
        else t = "record {" + FormatGuid(*op.record->guid) + "}";
        return t + " field " + Q(op.field);
    }
    // AddInstance plus its set entries; the id is valid only if all of them applied.
    bool NewInstance(const PackOp& op, InstanceId& out) {
        out = {};
        InstanceId id;
        bool ok = Note(op, p.AddInstance(op.type, op.clone, id), {});
        for (const auto& [path, v] : op.sets) ok &= Note(op, p.OverrideField(id, path, v), {});
        if (ok) out = id;
        return ok;
    }
    bool Pointer(const PackOp& op, InstanceId& target, bool& isNull) {
        isNull = false;
        switch (op.pointer.kind) {
        case PackPointer::Kind::Null: isNull = true; return true;
        case PackPointer::Kind::Local: target = Id(op.pointer.local); return true;
        case PackPointer::Kind::Existing: {
            Status st = p.FindInstance(op.pointer.existing, target);
            if (st) return true;
            st.message = "pointer target: " + st.message;
            return Note(op, std::move(st), (op.kind == PackOp::Kind::Set ? "SetPointer " : "AppendElement ") + Where(op));
        }
        case PackPointer::Kind::None: break;
        }
        return true;
    }
    bool Run(const PackOp& op) {
        switch (op.kind) {
        case PackOp::Kind::Instance: {
            InstanceId id;
            const bool ok = NewInstance(op, id);
            ids[op.id] = id;
            return ok;
        }
        case PackOp::Kind::Set: {
            if (op.pointer.kind != PackPointer::Kind::None) {
                InstanceId target;
                bool isNull = false;
                if (!Pointer(op, target, isNull)) return false;
                if (isNull) return Note(op, OnTarget(op, [&](auto t) { return p.OverrideField(t, op.field, Value{}); }), {});
                return Note(op, OnTarget(op, [&](auto t) { return p.SetPointer(t, op.field, target); }), {});
            }
            return Note(op, OnTarget(op, [&](auto t) { return p.OverrideField(t, op.field, *op.value); }), {});
        }
        case PackOp::Kind::Append: {
            Value v;
            if (op.pointer.kind != PackPointer::Kind::None) {
                InstanceId target;
                bool isNull = false;
                if (!Pointer(op, target, isNull)) return false;
                if (!isNull) v = Value::OfInstance(target);
            } else if (!op.element.empty()) {
                v = Value::OfInstance(Id(op.element));
            } else if (!op.type.empty()) {
                InstanceId id;
                if (!NewInstance(op, id)) {
                    Note(op, Status{ Refusal::DependencyFailed, Where(op) + ": the new element wasn't created" }, "AppendElement " + Where(op));
                    return false;
                }
                v = Value::OfInstance(id);
            } else {
                v = *op.value;
            }
            return Note(op, OnTarget(op, [&](auto t) { return p.AppendElement(t, op.field, v); }), {});
        }
        }
        return false;
    }
};

std::string Origin(const Pack& p, uint32_t line) {
    return p.plugin + " (" + p.name + (line ? Fmt(":%u", line) : std::string()) + ")";
}

}  // namespace

PackResult ApplyPacks(const Schema& base, std::span<const Pack> packs) {
    PackResult res;
    Patch main(base, PatchOptions{ false });
    std::map<SlotKey, Owner> owners;

    for (size_t pi = 0; pi < packs.size(); ++pi) {
        const Pack& pack = packs[pi];
        PackReport rep;
        rep.plugin = pack.plugin;
        rep.name = pack.name;
        rep.atomic = pack.atomic;
        std::map<std::string, InstanceId> ids;
        std::map<SlotKey, size_t> mine;                    // slots this pack wrote -> report index
        std::vector<std::pair<SlotKey, size_t>> accepted;  // slots of operations kept

        // A field this pack already set (through another path): refuse the later operation.
        auto checkOwn = [&](std::vector<std::pair<SlotKey, size_t>>& slots) {
            bool ok = true;
            for (const auto& [slot, ri] : slots) {
                const auto [it, fresh] = mine.emplace(slot, ri);
                if (fresh) continue;
                PackOpReport& r = rep.ops[ri];
                r.status = { Refusal::BadArgument, r.op + ": sets the same field as line " + std::to_string(rep.ops[it->second].line) +
                                                       " of this file (a field may be set once per file)" };
                ok = false;
            }
            return ok;
        };

        Patch trial = main.Fork();
        if (pack.atomic) {
            std::vector<std::pair<SlotKey, size_t>> slots;
            Exec ex{ trial, ids, rep.ops, slots };
            bool ok = true;
            for (const PackOp& op : pack.ops) ok &= ex.Run(op);
            ok &= checkOwn(slots);
            if (ok) accepted = std::move(slots);
        } else {
            for (const PackOp& op : pack.ops) {
                Patch step = trial.Fork();
                std::vector<std::pair<SlotKey, size_t>> slots;
                const size_t first = rep.ops.size();
                Exec ex{ step, ids, rep.ops, slots };
                bool ok = ex.Run(op);
                ok &= checkOwn(slots);
                if (ok) {
                    trial = std::move(step);
                    accepted.insert(accepted.end(), slots.begin(), slots.end());
                    continue;
                }
                // The group is dropped as a whole: its accepted calls aren't applied either.
                for (const auto& [slot, ri] : slots)
                    if (const auto it = mine.find(slot); it != mine.end() && it->second == ri) mine.erase(it);
                for (size_t k = first; k < rep.ops.size(); ++k)
                    if (rep.ops[k].status)
                        rep.ops[k].status = { Refusal::DependencyFailed,
                                              rep.ops[k].op + ": not applied: another part of the same operation failed" };
                if (op.kind == PackOp::Kind::Instance) ids[op.id] = {};
            }
        }

        for (const PackOpReport& r : rep.ops) {
            if (r.status) ++rep.applied;
            else {
                ++rep.skipped;
                if (rep.reason.empty()) rep.reason = Fmt("line %u: ", r.line) + r.status.message;
            }
        }
        if (pack.atomic && rep.skipped) {
            rep.state = PackState::Refused;
            rep.applied = 0;
            rep.skipped = rep.ops.size();
        } else if (rep.skipped) {
            rep.state = rep.applied ? PackState::Partial : PackState::Refused;
        }
        if (rep.state != PackState::Refused || rep.applied) {
            main = std::move(trial);
            for (const auto& [slot, ri] : accepted) {
                Owner o{ pi, rep.ops[ri].line, rep.ops[ri].op };
                const auto it = owners.find(slot);
                if (it != owners.end() && it->second.pack != pi)
                    res.conflicts.push_back(o.op + ": " + Origin(packs[it->second.pack], it->second.line) + " overridden by " +
                                            Origin(pack, o.line) + " (later in plugin order)");
                owners[slot] = std::move(o);
            }
        }
        res.packs.push_back(std::move(rep));
    }

    res.status = main.Emit(res.splices);
    if (!res.status) {
        res.splices.clear();
        for (PackReport& r : res.packs) {
            if (r.state == PackState::Refused && !r.applied && r.skipped) continue;
            r.state = PackState::Refused;
            r.skipped = r.ops.size();
            r.applied = 0;
            r.reason = "nothing applied: " + res.status.message;
        }
    }
    return res;
}

std::string Summary(const PackResult& res) {
    std::map<std::string, int> perPlugin;
    for (const PackReport& r : res.packs) ++perPlugin[r.plugin];
    std::string s = Fmt("[datacore] %zu pack%s", res.packs.size(), res.packs.size() == 1 ? "" : "s");
    if (!res.status) s += " (nothing applied: " + res.status.message + ")";
    s += ":";
    if (res.packs.empty()) s += " none";
    for (size_t i = 0; i < res.packs.size(); ++i) {
        const PackReport& r = res.packs[i];
        const std::string label = perPlugin[r.plugin] > 1 ? r.plugin + "/" + r.name : r.plugin;
        s += i ? "; " : " ";
        if (r.state == PackState::Refused) s += label + " refused (" + r.reason + ")";
        else {
            s += label + Fmt(" %zu/%zu applied", r.applied, r.applied + r.skipped);
            if (r.skipped) s += Fmt(" (%zu skipped: ", r.skipped) + r.reason + ")";
        }
    }
    if (!res.conflicts.empty()) s += Fmt("; %zu conflict%s", res.conflicts.size(), res.conflicts.size() == 1 ? "" : "s");
    return s;
}

}  // namespace sco::datacore
