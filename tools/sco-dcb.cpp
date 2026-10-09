// sco-dcb: inspect a DataCore file (Data\Game2.dcb, extracted from Data.p4k) with sco::datacore.
// Never shipped to players; the input file is only read. docs/datacore.md has the commands and how
// to get the file.
//
//   sco-dcb info <file.dcb>      header, counts, the tables with offsets and sizes, and validation
//   sco-dcb records <file.dcb>   one tab-separated line per record (research R1 input)
//   sco-dcb patch <in.dcb> <out.dcb> [options] <op>...
//                                applies a batch of patcher operations, re-validates, and writes the
//                                patched file to <out.dcb> (never over <in.dcb>). A developer check
//                                on a real file; data packs (.toml, plan PR 5) replace it
//
// Exit code: 0 when the layout is valid (patch: the batch emitted and the file was written), 1 when
// it is refused (the reason is printed), 2 on a usage error or a file that can't be read or written
// or is smaller than the header.
#include "sco/datacore.h"
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using namespace sco::datacore;
using ull = unsigned long long;

static const char* kUsage =
    "usage: sco-dcb info <file.dcb>\n"
    "       sco-dcb records <file.dcb>\n"
    "       sco-dcb patch <in.dcb> <out.dcb> [--seed N] [--pack ID] [--non-atomic] <op>...\n"
    "ops:   set <record|inst:N> <field> <value>\n"
    "       add-instance <struct> <clone record|-> <clone field|->      (the Nth is inst:N)\n"
    "       set-pointer <record|inst:N> <field> inst:N\n"
    "       append <record|inst:N> <field> <value>\n"
    "       add-record <struct> <name> <clone record> <file path|->\n"
    "value: null, true, false, an integer, a number, record:<name>, inst:N, or a string (an enum option too)\n";

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
        std::printf("%s %s %u", i ? "," : "", ValueKindName(static_cast<ValueKind>(i)), h.values[i]);
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

static bool WriteFile(const char* path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) return false;
    if (!bytes.empty()) out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out.flush());
}

static void PrintCapabilities(const Schema& s) {
    std::printf("capabilities: %s %s, %s %s\n", kCapPatch, PatchSupported(s) ? "on" : "off", kCapAddRecord,
                AddRecordSupported(s) ? "on" : "off");
}

// ---- patch -------------------------------------------------------------------------------------

namespace {
struct PatchRun {
    Patch& p;
    std::vector<InstanceId> instances;   // inst:1 is instances[0]
    bool Instance(const std::string& arg, InstanceId& out) const {
        if (arg.rfind("inst:", 0) != 0) return false;
        const unsigned long n = std::strtoul(arg.c_str() + 5, nullptr, 10);
        out = n >= 1 && n <= instances.size() ? instances[n - 1] : InstanceId{};
        return true;
    }
    Value Parse(const std::string& v) const {
        if (v == "null") return Value{};
        if (v == "true" || v == "false") return Value::OfBool(v == "true");
        if (v.rfind("record:", 0) == 0) return Value::OfRecord(RecordRef{ std::nullopt, v.substr(7) });
        InstanceId id;
        if (Instance(v, id)) return Value::OfInstance(id);
        char* end = nullptr;
        const long long i = std::strtoll(v.c_str(), &end, 10);
        if (!v.empty() && *end == 0) return Value::OfInt(i);
        const double d = std::strtod(v.c_str(), &end);
        if (!v.empty() && *end == 0) return Value::OfFloat(d);
        return Value::OfString(v);
    }
    template <class Fn>
    Status OnTarget(const std::string& target, Fn&& fn) {
        InstanceId id;
        if (Instance(target, id)) return fn(id);
        return fn(RecordRef{ std::nullopt, target });
    }
};
}  // namespace

static int RunPatch(int argc, char** argv) {
    if (argc < 5) { std::fputs(kUsage, stderr); return 2; }
    const char* in = argv[2];
    const char* outPath = argv[3];
    if (std::strcmp(in, outPath) == 0) { std::fprintf(stderr, "sco-dcb patch: the output must be another file\n"); return 2; }
    PatchOptions opt;
    opt.packId = "sco-dcb";
    int i = 4;
    for (; i < argc && std::strncmp(argv[i], "--", 2) == 0; ++i) {
        if (std::strcmp(argv[i], "--non-atomic") == 0) opt.atomic = false;
        else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) opt.guidSeed = std::strtoull(argv[++i], nullptr, 10);
        else if (std::strcmp(argv[i], "--pack") == 0 && i + 1 < argc) opt.packId = argv[++i];
        else { std::fputs(kUsage, stderr); return 2; }
    }
    std::vector<uint8_t> file;
    std::string error;
    if (!ReadFile(in, file, error)) { std::fprintf(stderr, "%s: %s\n", in, error.c_str()); return 2; }
    Schema s;
    if (!s.Parse(file)) {
        std::printf("layout refused (%s): %s\n", CheckName(s.failed), s.error.c_str());
        return s.failed == Check::File ? 2 : 1;
    }
    std::printf("base: %s (%llu bytes, %u records of %u bytes)\n", in, static_cast<ull>(s.fileSize), s.header.recordCount, s.recordSize);
    PrintCapabilities(s);
    Patch p(s, opt);
    PatchRun run{ p, {} };
    int n = 0, refused = 0;
    auto need = [&](int k) { return i + k < argc; };
    while (i < argc) {
        const std::string op = argv[i];
        Status st;
        std::string note;
        if (op == "set" && need(3)) {
            const std::string field = argv[i + 2];
            const Value v = run.Parse(argv[i + 3]);
            st = run.OnTarget(argv[i + 1], [&](const auto& t) { return p.OverrideField(t, field, v); });
            i += 4;
        } else if (op == "append" && need(3)) {
            const std::string field = argv[i + 2];
            const Value v = run.Parse(argv[i + 3]);
            st = run.OnTarget(argv[i + 1], [&](const auto& t) { return p.AppendElement(t, field, v); });
            i += 4;
        } else if (op == "set-pointer" && need(3)) {
            const std::string field = argv[i + 2];
            InstanceId target;
            if (!run.Instance(argv[i + 3], target)) { std::fputs(kUsage, stderr); return 2; }
            st = run.OnTarget(argv[i + 1], [&](const auto& t) { return p.SetPointer(t, field, target); });
            i += 4;
        } else if (op == "add-instance" && need(3)) {
            InstanceSource src;
            if (std::strcmp(argv[i + 2], "-") != 0) src.record = RecordRef{ std::nullopt, argv[i + 2] };
            if (std::strcmp(argv[i + 3], "-") != 0) src.field = argv[i + 3];
            InstanceId id;
            st = p.AddInstance(argv[i + 1], src, id);
            run.instances.push_back(id);
            if (st) note = "inst:" + std::to_string(run.instances.size()) + " = instance " + std::to_string(id.index) + " of " + argv[i + 1];
            i += 4;
        } else if (op == "add-record" && need(4)) {
            NewRecord r;
            r.type = argv[i + 1];
            r.name = argv[i + 2];
            r.clone.record = RecordRef{ std::nullopt, argv[i + 3] };
            if (std::strcmp(argv[i + 4], "-") != 0) r.filePath = argv[i + 4];
            AddedRecord a;
            st = p.AddRecord(r, a);
            if (st) note = "record " + std::to_string(a.index) + ", guid " + FormatGuid(a.guid) + ", root instance " + std::to_string(a.root.index);
            i += 5;
        } else {
            std::fputs(kUsage, stderr);
            return 2;
        }
        ++n;
        const OpReport& rep = p.Reports().back();
        if (st) std::printf("op %d: OK %s%s%s\n", n, rep.op.c_str(), note.empty() ? "" : ": ", note.c_str());
        else {
            ++refused;
            std::printf("op %d: REFUSED (%s) %s\n", n, RefusalName(st.category), st.message.c_str());
        }
    }
    std::vector<sco::vfs::Splice> splices;
    const Status e = p.Emit(splices);
    if (!e) {
        std::printf("Emit: REFUSED (%s) %s\nnothing written\n", RefusalName(e.category), e.message.c_str());
        return 1;
    }
    uint64_t added = 0, overwritten = 0;
    for (const auto& sp : splices) {
        added += sp.bytes ? sp.bytes->size() : 0;
        overwritten += sp.removed;
    }
    std::printf("Emit: OK, %d operations (%d refused), %zu splices (%llu bytes replaced, %llu bytes written), re-validated\n", n,
                refused, splices.size(), static_cast<ull>(overwritten), static_cast<ull>(added));
    std::vector<uint8_t> patched;
    if (const Status a = ApplySplices(file, splices, patched); !a) {
        std::printf("apply: %s\n", a.message.c_str());
        return 1;
    }
    if (!WriteFile(outPath, patched)) { std::fprintf(stderr, "%s: can't write\n", outPath); return 2; }
    std::printf("wrote %s (%llu bytes)\n", outPath, static_cast<ull>(patched.size()));
    return 0;
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::strcmp(argv[1], "patch") == 0) return RunPatch(argc, argv);
    if (argc != 3 || (std::strcmp(argv[1], "info") != 0 && std::strcmp(argv[1], "records") != 0)) {
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
        std::fprintf(info ? stdout : stderr, "layout refused (%s): %s\n", CheckName(s.failed), s.error.c_str());
        return s.failed == Check::File ? 2 : 1;
    }
    if (info) {
        std::printf("structs: %zu, %zu opaque (a field of unknown type: overrides into them are refused)\n",
                    s.structs.size(), s.OpaqueCount());
        PrintCapabilities(s);
        std::printf("layout: OK\n");
        return 0;
    }
    // tag: the name-pool string at record +8 (research R1: the owning team), empty for 32-byte records.
    std::printf("# index\tguid\tname\tstruct\tunknown\tinstance\tstructSize\tfile\ttag\n");
    for (size_t i = 0; i < s.records.size(); ++i) {
        const auto& r = s.records[i];
        const std::string_view name = s.Name(r.name), type = s.StructName(r.structIndex), file_ = s.ValueString(r.fileName);
        const std::string_view tag = s.recordSize > 32 ? s.Name(r.unknown) : std::string_view{};
        std::printf("%zu\t%s\t%.*s\t%.*s\t0x%08x\t%u\t%u\t%.*s\t%.*s\n", i, FormatGuid(r.id).c_str(),
                    static_cast<int>(name.size()), name.data(), static_cast<int>(type.size()), type.data(), r.unknown,
                    static_cast<unsigned>(r.instanceIndex), static_cast<unsigned>(r.structSize),
                    static_cast<int>(file_.size()), file_.data(), static_cast<int>(tag.size()), tag.data());
    }
    return 0;
}
