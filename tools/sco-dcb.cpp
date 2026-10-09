// sco-dcb: inspect a DataCore file (Data\Game2.dcb, extracted from Data.p4k) with sco::datacore.
// Read-only; never shipped to players. docs/datacore.md has the commands and how to get the file.
//
//   sco-dcb info <file.dcb>                 header, counts, the tables with offsets and sizes, and validation
//   sco-dcb records <file.dcb>              one tab-separated line per record (research R1 input)
//   sco-dcb lint <pack>...                  .toml syntax and shape, no game file needed
//   sco-dcb check <file.dcb> <pack>...      resolves every override: OK / SKIP per operation, a verdict per pack
//   sco-dcb show <file.dcb> <record> [field]  a record's fields as paths and values, to write overrides from
//   sco-dcb diff <a.dcb> <b.dcb>            the changes from a to b as a .toml pack, on stdout
//
// A <pack> is a pack folder (its datacore/*.toml files, in name order; the folder name is the plugin
// id) or one .toml file. Several packs are taken in the order given, which stands for plugin order.
// A <record> is a record name, or its GUID (optionally prefixed guid:).
//
// Exit code: 0 when the layout is valid (info, records, show), every pack parses (lint), every
// operation of every pack applies (check) or the diff converted everything (diff); 1 when the layout
// is refused, a pack doesn't parse or doesn't fully apply, a record or field isn't found (show), or
// some change couldn't be expressed (diff, listed as comments); 2 on a usage error or a file that
// can't be read or is smaller than the header.
#include "sco/datacore.h"
#include "sco/datacore_pack.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using sco::datacore::Check;
using sco::datacore::Schema;
namespace dc = sco::datacore;
namespace fs = std::filesystem;
using ull = unsigned long long;

static const char* kUsage =
    "usage: sco-dcb info <file.dcb>\n"
    "       sco-dcb records <file.dcb>\n"
    "       sco-dcb lint <pack>...\n"
    "       sco-dcb check <file.dcb> <pack>...\n"
    "       sco-dcb show <file.dcb> <record> [field]\n"
    "       sco-dcb diff <a.dcb> <b.dcb>\n"
    "a <pack> is a pack folder (datacore/*.toml) or a .toml file\n";

static bool ReadFile(const char* path, std::vector<uint8_t>& out, std::string& error) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in) { error = "can't open"; return false; }
    const std::streamoff size = in.tellg();
    if (size < 0) { error = "can't get its size"; return false; }
    out.resize(static_cast<size_t>(size));
    in.seekg(0);
    if (size > 0 && !in.read(reinterpret_cast<char*>(out.data()), size)) { error = "read failed"; return false; }
    return true;
}

static void PrintInfo(const char* path, const Schema& s) {
    using sco::datacore::ValueKind;
    std::printf("file: %s (%llu bytes)\n", path, static_cast<ull>(s.fileSize));
    if (!s.hasHeader) return;
    const auto& h = s.header;
    std::printf("header: version %u, unknown +0 %u, +8 %u %u %u %u\n", h.version, h.unknown0,
                static_cast<unsigned>(h.unknown8[0]), static_cast<unsigned>(h.unknown8[1]),
                static_cast<unsigned>(h.unknown8[2]), static_cast<unsigned>(h.unknown8[3]));
    std::printf("counts: %u structs, %u properties, %u enums, %u mappings, %u records, %u enum options\n",
                h.structCount, h.propertyCount, h.enumCount, h.mappingCount, h.recordCount, h.enumOptionCount);
    std::printf("values:");
    for (size_t i = 0; i < h.values.size(); ++i)
        std::printf("%s %s %u", i ? "," : "", sco::datacore::ValueKindName(static_cast<ValueKind>(i)), h.values[i]);
    std::printf("\nstring pools: values %u bytes, names %u bytes\n", h.valueStringLength, h.nameStringLength);
    if (s.recordSize == 0) return;
    std::printf("record size: %u bytes (derived from the totals; the header has no field for it)\n\n", s.recordSize);
    std::printf("%-22s %12s %10s %6s %12s\n", "table", "offset", "count", "entry", "bytes");
    for (const auto& t : s.tables) {
        char entry[16] = "-";
        if (t.entrySize) std::snprintf(entry, sizeof(entry), "%llu", static_cast<ull>(t.entrySize));
        char count[16] = "-";
        if (t.entrySize || t.name == "data") std::snprintf(count, sizeof(count), "%llu", static_cast<ull>(t.count));
        std::printf("%-22s %12llu %10s %6s %12llu\n", t.name.c_str(), static_cast<ull>(t.offset), count, entry,
                    static_cast<ull>(t.bytes));
    }
    std::printf("\n");
}

// ---- packs -------------------------------------------------------------------------------------

struct PackFile {
    std::string path;          // for messages
    dc::Pack    pack;
    bool        parsed = false;
    std::string error;
};

static bool EndsWithToml(const std::string& name) {
    if (name.size() < 5) return false;
    std::string ext = name.substr(name.size() - 5);
    for (char& c : ext) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
    return ext == ".toml";
}

static std::string Utf8(const std::u8string& u) { return std::string(u.begin(), u.end()); }
static std::string Utf8(const fs::path& p) { return Utf8(p.u8string()); }

// Loads one <pack> argument: a folder's datacore/*.toml in name order, or one file. False (with a
// message printed) when the argument can't be read at all.
static bool LoadPack(const char* arg, std::vector<PackFile>& out) {
    std::error_code ec;
    const fs::path p(reinterpret_cast<const char8_t*>(arg));
    std::vector<fs::path> files;
    std::string plugin;
    if (fs::is_directory(p, ec)) {
        plugin = Utf8(fs::absolute(p, ec).lexically_normal().filename());
        if (plugin.empty()) plugin = Utf8(fs::absolute(p, ec).lexically_normal().parent_path().filename());
        const fs::path dir = p / "datacore";
        if (!fs::is_directory(dir, ec)) {
            std::fprintf(stderr, "%s: no datacore folder\n", arg);
            return false;
        }
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
            if (it->is_regular_file() && EndsWithToml(Utf8(it->path().filename()))) files.push_back(it->path());
        if (ec) {
            std::fprintf(stderr, "%s: can't read datacore: %s\n", arg, ec.message().c_str());
            return false;
        }
        std::sort(files.begin(), files.end(), [](const fs::path& a, const fs::path& b) { return Utf8(a.filename()) < Utf8(b.filename()); });
        if (files.empty()) {
            std::fprintf(stderr, "%s: no datacore/*.toml files\n", arg);
            return false;
        }
    } else {
        files.push_back(p);
        const fs::path parent = fs::absolute(p, ec).parent_path();
        plugin = Utf8(parent.filename() == "datacore" ? parent.parent_path().filename() : p.stem());
    }
    for (const fs::path& f : files) {
        PackFile pf;
        pf.path = Utf8(f.generic_u8string());
        pf.pack.plugin = plugin;
        pf.pack.name = "datacore/" + Utf8(f.filename());
        std::ifstream in(f, std::ios::binary);
        if (!in) {
            std::fprintf(stderr, "%s: can't open\n", pf.path.c_str());
            return false;
        }
        std::string text;
        char buf[65536];
        while (in.read(buf, sizeof(buf)) || in.gcount() > 0) {
            text.append(buf, static_cast<size_t>(in.gcount()));
            if (text.size() > dc::kMaxPackBytes) break;
        }
        pf.parsed = dc::ParsePack(text, pf.pack, pf.error);
        out.push_back(std::move(pf));
    }
    return true;
}

static int Lint(int argc, char** argv) {
    std::vector<PackFile> packs;
    for (int i = 2; i < argc; ++i)
        if (!LoadPack(argv[i], packs)) return 2;
    int bad = 0;
    for (const PackFile& p : packs) {
        if (p.parsed)
            std::printf("OK   %s: %zu operation%s%s\n", p.path.c_str(), p.pack.ops.size(), p.pack.ops.size() == 1 ? "" : "s",
                        p.pack.atomic ? "" : " (atomic = false)");
        else {
            std::printf("FAIL %s:%s\n", p.path.c_str(), p.error.c_str());
            ++bad;
        }
    }
    std::printf("lint: %zu file%s, %d failed\n", packs.size(), packs.size() == 1 ? "" : "s", bad);
    return bad ? 1 : 0;
}

static bool LoadSchema(const char* path, std::vector<uint8_t>& bytes, Schema& s, int& rc) {
    std::string error;
    if (!ReadFile(path, bytes, error)) {
        std::fprintf(stderr, "%s: %s\n", path, error.c_str());
        rc = 2;
        return false;
    }
    if (!s.Parse(bytes)) {
        std::printf("%s: layout refused (%s): %s\n", path, sco::datacore::CheckName(s.failed), s.error.c_str());
        rc = s.failed == Check::File ? 2 : 1;
        return false;
    }
    return true;
}

static int CheckPacks(int argc, char** argv) {
    std::vector<PackFile> files;
    for (int i = 3; i < argc; ++i)
        if (!LoadPack(argv[i], files)) return 2;
    std::vector<uint8_t> bytes;
    Schema s;
    int rc = 0;
    if (!LoadSchema(argv[2], bytes, s, rc)) return rc;
    std::printf("%s: %llu bytes, %zu records, layout OK\n", argv[2], static_cast<ull>(s.fileSize), s.records.size());

    std::vector<dc::Pack> packs;
    bool ok = true;
    for (const PackFile& f : files) {
        if (f.parsed) packs.push_back(f.pack);
        else {
            std::printf("\npack %s %s: REFUSED (doesn't parse)\n  %s:%s\n", f.pack.plugin.c_str(), f.pack.name.c_str(), f.path.c_str(), f.error.c_str());
            ok = false;
        }
    }
    const dc::PackResult res = dc::ApplyPacks(s, packs);
    for (const dc::PackReport& r : res.packs) {
        std::printf("\npack %s %s (%s): %s", r.plugin.c_str(), r.name.c_str(), r.atomic ? "atomic" : "atomic = false",
                    r.state == dc::PackState::Refused ? "REFUSED" : r.state == dc::PackState::Partial ? "PARTIAL" : "APPLIED");
        std::printf(" %zu/%zu\n", r.applied, r.applied + r.skipped);
        for (const dc::PackOpReport& op : r.ops) {
            if (op.status)
                std::printf("  line %u: OK   %s%s\n", op.line, op.op.c_str(), r.state == dc::PackState::Refused ? " (not applied: the pack is refused)" : "");
            else std::printf("  line %u: SKIP %s (%s)\n", op.line, op.status.message.c_str(), dc::RefusalName(op.status.category));
        }
        if (r.state != dc::PackState::Applied) {
            std::printf("  -> %s\n", r.reason.c_str());
            ok = false;
        }
    }
    for (const std::string& c : res.conflicts) std::printf("\nconflict: %s", c.c_str());
    if (!res.conflicts.empty()) std::printf("\n");
    if (res.status) std::printf("\nemit: OK, %zu splice%s, the patched file re-validated\n", res.splices.size(), res.splices.size() == 1 ? "" : "s");
    else {
        std::printf("\nemit: refused: %s\n", res.status.message.c_str());
        ok = false;
    }
    std::printf("%s\n", dc::Summary(res).c_str());
    return ok ? 0 : 1;
}

// ---- show --------------------------------------------------------------------------------------

static dc::RecordRef RecordArg(const char* arg) {
    dc::RecordRef r;
    std::string a = arg;
    if (a.rfind("guid:", 0) == 0) a = a.substr(5);
    dc::Guid g;
    if (dc::ParseGuid(a, g)) r.guid = g;
    else r.name = arg;
    return r;
}

static int Show(int argc, char** argv) {
    std::vector<uint8_t> bytes;
    Schema s;
    int rc = 0;
    if (!LoadSchema(argv[2], bytes, s, rc)) return rc;
    const dc::RecordRef ref = RecordArg(argv[3]);
    const dc::Record* r = ref.guid ? s.FindRecord(*ref.guid) : s.FindRecordByName(ref.name);
    if (!r) {
        std::printf("record %s not found\n", argv[3]);
        return 1;
    }
    const std::string_view name = s.Name(r->name), type = s.StructName(r->structIndex), file = s.ValueString(r->fileName);
    std::printf("record \"%.*s\" guid %s struct %.*s file %.*s\n", static_cast<int>(name.size()), name.data(),
                dc::FormatGuid(r->id).c_str(), static_cast<int>(type.size()), type.data(), static_cast<int>(file.size()), file.data());
    dc::Patch p(s);
    std::vector<dc::FieldView> fields;
    const dc::Status st = p.ReadFields(ref, argc > 4 ? argv[4] : "", fields);
    if (!st) {
        std::printf("%s (%s)\n", st.message.c_str(), dc::RefusalName(st.category));
        return 1;
    }
    for (const dc::FieldView& f : fields)
        std::printf("%*s%s = %s\n", static_cast<int>(2 * f.depth), "", f.path.empty() ? "(value)" : f.path.c_str(), f.text.c_str());
    return 0;
}

// ---- diff --------------------------------------------------------------------------------------

// The changes from a to b as a .toml pack. b is expected to be a with things appended (as the
// patcher writes): both must have the same struct and property definitions. Records are matched by
// GUID and walked from their roots (sco::datacore::Patch::ReadFields); values are compared by path.
struct Differ {
    const Schema& a;
    const Schema& b;
    dc::Patch pa, pb;
    std::string out;
    std::vector<std::string> notes;
    std::map<std::pair<uint32_t, uint32_t>, std::string> made;   // b instance -> its [[instance]] id
    int next = 0;
    size_t changes = 0;

    Differ(const Schema& sa, const Schema& sb) : a(sa), b(sb), pa(sa), pb(sb) {}

    static std::string Q(const std::string& s) {
        std::string q = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') q += '\\';
            q += c;
        }
        return q + "\"";
    }
    bool IsNew(dc::InstanceId t) const {
        return t.valid() && t.structIndex < a.structInfo.size() && t.index >= a.structInfo[t.structIndex].instances;
    }
    static std::string RecordLines(const dc::Record& r, const Schema& s) {
        return "record = " + Q(std::string(s.Name(r.name))) + "\nguid = " + Q(dc::FormatGuid(r.id)) + "\n";
    }
    // A pointer value for an existing instance of a: a record root, or a path inside `owner`'s tree.
    bool Existing(dc::InstanceId t, const dc::Record* owner, const std::vector<dc::FieldView>& ownerFields, std::string& text) {
        for (const dc::Record& r : a.records)
            if (r.structIndex == t.structIndex && r.instanceIndex == t.index) {
                text = "{ record = " + Q(std::string(a.Name(r.name))) + " }";
                return true;
            }
        if (owner)
            for (const dc::FieldView& f : ownerFields)
                if (f.target == t && (f.dataType == sco::datacore::type::kStrongPointer || f.dataType == sco::datacore::type::kClass)) {
                    text = "{ record = " + Q(std::string(a.Name(owner->name))) + ", field = " + Q(f.path) + " }";
                    return true;
                }
        return false;
    }
    static size_t SubtreeEnd(const std::vector<dc::FieldView>& v, size_t j) {
        size_t k = j + 1;
        while (k < v.size() && v[k].depth > v[j].depth) ++k;
        return k;
    }
    // "pointer = ..." (or "element = ..." for a struct element) for b's value `f`; false with a note when
    // it can't be expressed.
    bool PointerLine(const dc::FieldView& f, const dc::Record* owner, const std::vector<dc::FieldView>& ownerA, const std::string& key,
                     std::string& line) {
        if (!f.target.valid()) { line = key + " = \"null\"\n"; return true; }
        if (IsNew(f.target)) { line = key + " = " + Q("@" + Make(f.target, owner, ownerA)) + "\n"; return true; }
        std::string text;
        if (!Existing(f.target, owner, ownerA, text)) return false;
        line = key + " = " + text + "\n";
        return true;
    }
    // Emits the operations that build b's view entries [from, to) onto `target` (a [[set]] /
    // [[append]] target block).
    void Fill(const std::vector<dc::FieldView>& v, size_t from, size_t to, const std::string& target, const dc::Record* owner,
              const std::vector<dc::FieldView>& ownerA, const std::string& where) {
        using namespace sco::datacore;
        for (size_t j = from; j < to;) {
            const FieldView& f = v[j];
            const size_t end = SubtreeEnd(v, j);
            if (f.array) {
                for (size_t k = j + 1; k < end;) {
                    const FieldView& e = v[k];
                    const size_t eend = SubtreeEnd(v, k);
                    Append(f.path, e, target, owner, ownerA, where);
                    k = eend;
                }
            } else if (f.dataType == type::kStrongPointer || f.dataType == type::kWeakPointer) {
                std::string line;
                if (PointerLine(f, owner, ownerA, "pointer", line)) Block("set", target, f.path, line);
                else Note(where + " field " + f.path + ": points at an existing instance no record path reaches");
            } else if (f.dataType == type::kReference) {
                Note(where + " field " + f.path + ": reference fields aren't patched yet (research R1)");
            } else if (f.text.rfind("(", 0) == 0) {
                Note(where + " field " + f.path + ": " + f.text);
            } else {
                Block("set", target, f.path, "value = " + f.text + "\n");
            }
            j = end;
        }
    }
    void Append(const std::string& path, const dc::FieldView& e, const std::string& target, const dc::Record* owner,
                const std::vector<dc::FieldView>& ownerA, const std::string& where) {
        using namespace sco::datacore;
        if (e.dataType == type::kClass) {
            Block("append", target, path, "element = " + Q("@" + Make(e.target, owner, ownerA, true)) + "\n");
        } else if (e.dataType == type::kStrongPointer || e.dataType == type::kWeakPointer) {
            std::string line;
            if (PointerLine(e, owner, ownerA, "pointer", line)) Block("append", target, path, line);
            else Note(where + " field " + e.path + ": points at an existing instance no record path reaches");
        } else if (e.dataType == type::kReference) {
            Note(where + " field " + e.path + ": reference arrays aren't patched yet (research R1)");
        } else {
            Block("append", target, path, "value = " + e.text + "\n");
        }
    }
    void Block(const char* kind, const std::string& target, const std::string& field, const std::string& body) {
        out += std::string("\n[[") + kind + "]]\n" + target + "field = " + Q(field) + "\n" + body;
        ++changes;
    }
    void Note(const std::string& n) { notes.push_back(n); }
    // An [[instance]] reproducing b's instance `t` (zero-filled, then every value set), once.
    std::string Make(dc::InstanceId t, const dc::Record* owner, const std::vector<dc::FieldView>& ownerA, bool copy = false) {
        const auto key = std::make_pair(t.structIndex, t.index);
        if (!copy)
            if (const auto it = made.find(key); it != made.end()) return it->second;
        const std::string id = "new" + std::to_string(++next);
        if (!copy) made[key] = id;
        out += "\n[[instance]]\nid = " + Q(id) + "\nstruct = " + Q(std::string(b.StructName(t.structIndex))) + "\n";
        ++changes;
        std::vector<dc::FieldView> v;
        if (const dc::Status st = pb.ReadFields(t, "", v, 32); !st) {
            Note("instance " + id + ": " + st.message);
            return id;
        }
        Fill(v, 0, v.size(), "instance = " + Q("@" + id) + "\n", owner, ownerA, "instance " + id);
        return id;
    }

    void Record(const dc::Record& rb) {
        using namespace sco::datacore;
        const dc::Record* ra = a.FindRecord(rb.id);
        const std::string rname(b.Name(rb.name));
        if (!ra) {
            Note("record " + rname + " {" + FormatGuid(rb.id) + "} is new in b (AddRecord isn't supported yet)");
            return;
        }
        RecordRef ref;
        ref.guid = rb.id;
        std::vector<FieldView> va, vb;
        if (!pa.ReadFields(ref, "", va, 32) || !pb.ReadFields(ref, "", vb, 32)) {
            Note("record " + rname + ": can't be read");
            return;
        }
        std::map<std::string, size_t> at;
        for (size_t i = 0; i < va.size(); ++i) at.emplace(va[i].path, i);
        const std::string target = RecordLines(rb, b);
        const std::string where = "record " + rname;
        Compare(va, vb, at, 0, vb.size(), target, rb, where);
    }
    // Compares b's entries [from, to) with a's by path; returns where it stopped (past `to` when an
    // entry's subtree runs beyond it).
    size_t Compare(const std::vector<dc::FieldView>& va, const std::vector<dc::FieldView>& vb, const std::map<std::string, size_t>& at,
                   size_t from, size_t to, const std::string& target, const dc::Record& rb, const std::string& where) {
        using namespace sco::datacore;
        size_t j = from;
        while (j < to) {
            const FieldView& f = vb[j];
            const size_t end = SubtreeEnd(vb, j);
            const auto it = at.find(f.path);
            if (it == at.end()) {
                Note(where + " field " + f.path + ": only in b");
                j = end;
                continue;
            }
            const FieldView& g = va[it->second];
            if (f.array) {
                // A nested array: handled like a top-level one.
                if (f.count < g.count) {
                    Note(where + " field " + f.path + ": the array shrank from " + std::to_string(g.count) + " to " + std::to_string(f.count) + " elements");
                    j = end;
                    continue;
                }
                size_t k = j + 1, idx = 0;
                while (k < end && idx < g.count) {
                    k = SubtreeEnd(vb, k);
                    ++idx;
                }
                for (size_t e = k; e < end; e = SubtreeEnd(vb, e)) Append(f.path, vb[e], target, &rb, va, where);
                Compare(va, vb, at, j + 1, k, target, rb, where);
                j = end;
                continue;
            }
            if (f.dataType == type::kClass) {   // a struct element: its own index may move (arrays are copied); compare inside
                ++j;
                continue;
            }
            if (f.dataType == type::kStrongPointer || f.dataType == type::kWeakPointer) {
                const bool same = f.target == g.target || (!f.target.valid() && !g.target.valid());
                if (same) { ++j; continue; }   // the same target: its subtree is compared entry by entry
                std::string line;
                if (PointerLine(f, &rb, va, "pointer", line)) Block("set", target, f.path, line);
                else Note(where + " field " + f.path + ": points at an existing instance no record path reaches");
                j = end;                         // another instance: never compared with the old one
                continue;
            }
            if (f.text != g.text) {
                if (f.dataType == type::kReference) Note(where + " field " + f.path + ": reference fields aren't patched yet (research R1)");
                else if (f.text.rfind("(", 0) == 0) Note(where + " field " + f.path + ": " + f.text);
                else Block("set", target, f.path, "value = " + f.text + "\n");
            }
            ++j;
        }
        return j;
    }
};

static bool SameSchema(const Schema& a, const Schema& b, std::string& why) {
    if (a.structs.size() != b.structs.size() || a.properties.size() != b.properties.size() || a.enums.size() != b.enums.size()) {
        why = "different numbers of structs, properties or enums";
        return false;
    }
    for (size_t i = 0; i < a.structs.size(); ++i)
        if (a.StructName(static_cast<uint32_t>(i)) != b.StructName(static_cast<uint32_t>(i)) || a.structs[i].parent != b.structs[i].parent ||
            a.structs[i].propertyCount != b.structs[i].propertyCount || a.structs[i].firstProperty != b.structs[i].firstProperty) {
            why = "struct " + std::string(a.StructName(static_cast<uint32_t>(i))) + " differs";
            return false;
        }
    for (size_t i = 0; i < a.properties.size(); ++i) {
        const auto& x = a.properties[i];
        const auto& y = b.properties[i];
        if (a.Name(x.name) != b.Name(y.name) || x.dataType != y.dataType || x.typeIndex != y.typeIndex || x.conversion != y.conversion) {
            why = "property " + std::string(a.Name(x.name)) + " differs";
            return false;
        }
    }
    return true;
}

static int Diff(int, char** argv) {
    std::vector<uint8_t> ab, bb;
    Schema a, b;
    int rc = 0;
    if (!LoadSchema(argv[2], ab, a, rc) || !LoadSchema(argv[3], bb, b, rc)) return rc;
    std::string why;
    if (!SameSchema(a, b, why)) {
        std::printf("# the files have different definitions (%s): diff needs b to be a with values changed and things appended\n", why.c_str());
        return 1;
    }
    Differ d(a, b);
    for (const dc::Record& r : b.records) d.Record(r);
    std::string text = "# sco-dcb diff: the changes from " + std::string(argv[2]) + " to " + std::string(argv[3]) + "\n";
    for (const std::string& n : d.notes) text += "# not converted: " + n + "\n";
    text += "format = 1\natomic = true\n" + d.out;
    // The output must be a valid pack.
    dc::Pack check;
    std::string error;
    if (!dc::ParsePack(text, check, error)) {
        std::fputs(text.c_str(), stdout);
        std::fprintf(stderr, "sco-dcb diff: the generated pack doesn't parse (%s)\n", error.c_str());
        return 1;
    }
    std::fputs(text.c_str(), stdout);
    std::fprintf(stderr, "sco-dcb diff: %zu operations, %zu changes not converted\n", check.ops.size(), d.notes.size());
    return d.notes.empty() ? 0 : 1;
}

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";
    if (cmd == "lint" && argc >= 3) return Lint(argc, argv);
    if (cmd == "check" && argc >= 4) return CheckPacks(argc, argv);
    if (cmd == "show" && (argc == 4 || argc == 5)) return Show(argc, argv);
    if (cmd == "diff" && argc == 4) return Diff(argc, argv);
    if (argc != 3 || (cmd != "info" && cmd != "records")) {
        std::fputs(kUsage, stderr);
        return 2;
    }
    const bool info = std::strcmp(argv[1], "info") == 0;
    const char* path = argv[2];
    std::vector<uint8_t> file;
    std::string error;
    if (!ReadFile(path, file, error)) {
        std::fprintf(stderr, "%s: %s\n", path, error.c_str());
        return 2;
    }
    Schema s;
    const bool ok = s.Parse(file);
    if (info) PrintInfo(path, s);
    if (!ok) {
        std::fprintf(info ? stdout : stderr, "layout refused (%s): %s\n", sco::datacore::CheckName(s.failed),
                     s.error.c_str());
        return s.failed == Check::File ? 2 : 1;
    }
    if (info) {
        std::printf("structs: %zu, %zu opaque (a field of unknown type: overrides into them are refused)\n",
                    s.structs.size(), s.OpaqueCount());
        std::printf("layout: OK\n");
        return 0;
    }
    std::printf("# index\tguid\tname\tstruct\tunknown\tinstance\tstructSize\tfile\n");
    for (size_t i = 0; i < s.records.size(); ++i) {
        const auto& r = s.records[i];
        const std::string_view name = s.Name(r.name), type = s.StructName(r.structIndex), file_ = s.ValueString(r.fileName);
        std::printf("%zu\t%s\t%.*s\t%.*s\t0x%08x\t%u\t%u\t%.*s\n", i, sco::datacore::FormatGuid(r.id).c_str(),
                    static_cast<int>(name.size()), name.data(), static_cast<int>(type.size()), type.data(), r.unknown,
                    static_cast<unsigned>(r.instanceIndex), static_cast<unsigned>(r.structSize),
                    static_cast<int>(file_.size()), file_.data());
    }
    return 0;
}
