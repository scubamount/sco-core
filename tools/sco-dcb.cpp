// sco-dcb: inspect a DataCore file (Data\Game2.dcb, extracted from Data.p4k) with sco::datacore.
// Read-only; never shipped to players. docs/datacore.md has the commands and how to get the file.
//
//   sco-dcb info <file.dcb>      header, counts, the tables with offsets and sizes, and validation
//   sco-dcb records <file.dcb>   one tab-separated line per record (research R1 input)
//
// Exit code: 0 when the layout is valid, 1 when it is refused (the reason is printed), 2 on a usage
// error or a file that can't be read or is smaller than the header.
#include "sco/datacore.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

using sco::datacore::Check;
using sco::datacore::Schema;
using ull = unsigned long long;

static const char* kUsage = "usage: sco-dcb info <file.dcb>\n       sco-dcb records <file.dcb>\n";

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

int main(int argc, char** argv) {
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
