// Unit tests for the DataCore parser (sco/datacore.h) over synthetic files from tests/dcb_builder.h:
// round trips with 32-, 36- and 40-byte records, each validation refusal, unknown types (opaque
// structs), and truncated or corrupted files (no crash, a clean refusal). Parse is a pure function
// over a byte span with no shared state, so ASan+UBSan cover it; there is nothing for TSan.
//   tools/test.sh
//   test_datacore [out-dir]    also writes the fixtures the sco-dcb tool test reads
#include "dcb_builder.h"
#include "sco/datacore.h"
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sco::datacore::Check;
using sco::datacore::Schema;
using Bytes = std::vector<uint8_t>;

static void Put32(Bytes& f, uint64_t at, uint32_t v) {
    for (int i = 0; i < 4; ++i) f[at + i] = static_cast<uint8_t>(v >> (8 * i));
}
static void Put16(Bytes& f, uint64_t at, uint16_t v) {
    f[at] = static_cast<uint8_t>(v);
    f[at + 1] = static_cast<uint8_t>(v >> 8);
}
static uint32_t Get32(const Bytes& f, uint64_t at) {
    return static_cast<uint32_t>(f[at]) | static_cast<uint32_t>(f[at + 1]) << 8 | static_cast<uint32_t>(f[at + 2]) << 16 |
           static_cast<uint32_t>(f[at + 3]) << 24;
}
static sco::datacore::Guid ToGuid(const dcb::Guid& g) {
    sco::datacore::Guid out;
    out.bytes = g;
    return out;
}

// Parses `f` and expects a refusal by `check`, with a reason in the "layout: ..." / "file: ..." form.
static bool Refused(const Bytes& f, Check check) {
    Schema s;
    const bool ok = s.Parse(f);
    const bool good = !ok && s.failed == check &&
                      (s.error.rfind("layout: ", 0) == 0 || s.error.rfind("file: ", 0) == 0);
    if (!good)
        std::printf("  expected refusal '%s', got %s '%s': %s\n", sco::datacore::CheckName(check), ok ? "OK" : "refusal",
                    sco::datacore::CheckName(s.failed), s.error.c_str());
    return good;
}

// ---- round trip --------------------------------------------------------------------------------

static void TestRoundTrip(uint32_t recordSize) {
    dcb::Builder b = dcb::Fixture(recordSize);
    const Bytes f = b.Build();
    Schema s;
    const bool ok = s.Parse(f);
    CHECK(ok);
    if (!ok) { std::printf("  %u-byte records: %s\n", recordSize, s.error.c_str()); return; }
    CHECK(s.failed == Check::None && s.error.empty());
    CHECK(s.recordSize == recordSize);
    CHECK(s.header.version == 8);
    CHECK(s.header.structCount == b.structs.size() && s.header.recordCount == b.records.size());
    CHECK(s.header.enumCount == 1 && s.header.enumOptionCount == 2 && s.header.mappingCount == 5);
    CHECK(s.header.values[static_cast<size_t>(sco::datacore::ValueKind::Int32)] == 3);
    CHECK(s.header.values[static_cast<size_t>(sco::datacore::ValueKind::String)] == 2);
    CHECK(s.header.values[static_cast<size_t>(sco::datacore::ValueKind::Reference)] == 1);

    // The tables tile the file, in file order, where the builder put them.
    uint64_t at = 0;
    bool tiled = true;
    for (const auto& t : s.tables) { tiled &= t.offset == at; at += t.bytes; }
    CHECK(tiled && at == f.size());
    CHECK(s.tables.size() == 6 + 19 + 3);
    CHECK(s.tables[1].offset == b.layout.structs && s.tables[2].offset == b.layout.properties);
    CHECK(s.tables[3].offset == b.layout.enums && s.tables[4].offset == b.layout.mappings);
    CHECK(s.tables[5].offset == b.layout.records && s.tables[5].entrySize == recordSize);
    CHECK(s.tables[6].offset == b.layout.pools[0] && s.tables[24].offset == b.layout.enumOptions);
    CHECK(s.dataOffset == b.layout.data && s.dataOffset + s.dataSize == f.size());

    for (uint32_t i = 0; i < b.structs.size(); ++i) {
        CHECK(s.StructName(i) == b.structs[i].name);
        CHECK(s.structInfo[i].size == b.Size(i));
        CHECK(!s.structInfo[i].opaque);
        CHECK(s.FindStruct(b.structs[i].name) == i);
    }
    CHECK(s.structInfo[dcb::kShip].size == 139);   // 9 inherited from Base + 130 own
    CHECK(s.FindStruct("Nope") == -1);
    CHECK(s.OpaqueCount() == 0);
    for (uint32_t m = 0; m < b.mappings.size(); ++m) CHECK(s.blockOffsets[m] == b.layout.blocks[m]);
    CHECK(s.structInfo[dcb::kPart].instances == 4 && s.structInfo[dcb::kPart].blockOffset == b.layout.blocks[3]);
    CHECK(s.structInfo[dcb::kEmpty].instances == 0);

    const std::vector<uint32_t> props = s.Properties(dcb::kShip);   // inherited first
    CHECK(props.size() == 3 + 19);
    if (props.size() == 22) {
        CHECK(s.Name(s.properties[props[0]].name) == "flag" && s.Name(s.properties[props[2]].name) == "kind");
        CHECK(s.Name(s.properties[props[3]].name) == "i8" && s.Name(s.properties[props[21]].name) == "path");
        CHECK(s.properties[props[21]].conversion == dcb::kArray3);
    }
    CHECK(s.Name(s.enums[0].name) == "Kind" && s.enums[0].optionCount == 2);
    CHECK(s.Name(s.enumOptions[s.enums[0].firstOption + 1]) == "Large");

    for (uint32_t i = 0; i < b.records.size(); ++i) {
        const dcb::Record& want = b.records[i];
        const sco::datacore::Record& r = s.records[i];
        CHECK(s.Name(r.name) == want.name);
        CHECK(s.ValueString(r.fileName) == want.fileName);
        CHECK(r.structIndex == want.structIndex && r.instanceIndex == want.instance);
        CHECK(r.structSize == b.Size(want.structIndex));
        CHECK(r.unknown == (recordSize > 32 ? b.Name(want.team) : 0u));
        CHECK(recordSize == 32 || s.Name(r.unknown) == want.team);
        CHECK(s.FindRecord(ToGuid(want.id)) == &r);
        CHECK(s.FindRecordByName(want.name) == &r);
    }
    CHECK(s.FindRecord(ToGuid(dcb::MakeGuid(0x99))) == nullptr);
    CHECK(s.FindRecordByName("Nope") == nullptr);
    CHECK(s.Name(0xFFFFFFFFu).empty() && s.ValueString(0xFFFFFFFFu).empty());
}

static void TestGuidFormat() {
    // unp4k's reader: int16 c, int16 b, int32 a, then bytes k..d, printed as Guid(a, b, c, d..k).
    CHECK(sco::datacore::FormatGuid(ToGuid(dcb::MakeGuid(0))) == "07060504-0302-0100-0f0e-0d0c0b0a0908");
}

static void TestNoRecords() {
    dcb::Builder b = dcb::Fixture(36);
    b.records.clear();
    const Bytes f = b.Build();
    Schema s;
    CHECK(s.Parse(f));
    CHECK(s.records.empty() && s.recordSize != 0);
}

// ---- refusals ----------------------------------------------------------------------------------

static void TestRefusals(uint32_t recordSize) {
    dcb::Builder b = dcb::Fixture(recordSize);
    const Bytes good = b.Build();
    const auto& L = b.layout;
    const uint64_t rec1 = L.records + recordSize;
    auto with = [&](auto&& change) { Bytes f = good; change(f); return f; };

    // File: smaller than the header.
    CHECK(Refused(Bytes(good.begin(), good.begin() + 100), Check::File));
    CHECK(Refused(Bytes{}, Check::File));

    // Rules 1-3: counts, string-pool lengths and mapping sizes must add up to the file size.
    CHECK(Refused(with([](Bytes& f) { Put32(f, 36, Get32(f, 36) + 1); }), Check::Totals));     // bool count
    CHECK(Refused(with([](Bytes& f) { Put32(f, 108, Get32(f, 108) + 1); }), Check::Totals));   // enum options
    CHECK(Refused(with([](Bytes& f) { Put32(f, 112, Get32(f, 112) + 4); }), Check::Totals));   // value strings
    CHECK(Refused(with([](Bytes& f) { Put32(f, 116, Get32(f, 116) - 1); }), Check::Totals));   // name strings
    CHECK(Refused(with([](Bytes& f) { Put32(f, 32, Get32(f, 32) + 1); }), Check::Totals));     // record count
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.mappings, Get32(f, L.mappings) + 1); }), Check::Totals));
    CHECK(Refused(with([](Bytes& f) { f.pop_back(); }), Check::Totals));
    CHECK(Refused(with([](Bytes& f) { f.push_back(0); }), Check::Totals));
    CHECK(Refused(with([](Bytes& f) { for (int i = 16; i < 120; i += 4) Put32(f, i, 0xFFFFFFFFu); }), Check::Totals));
    // A wrong struct count shifts every later table: refused, whichever check sees it first.
    {
        Schema s;
        CHECK(!s.Parse(with([](Bytes& f) { Put32(f, 16, Get32(f, 16) + 1); })) && s.failed != Check::None);
    }
    // Rule 2: a record entry size outside {32, 36, 40}.
    {
        dcb::Builder odd = dcb::Fixture(44);
        CHECK(Refused(odd.Build(), Check::Totals));
        dcb::Builder small = dcb::Fixture(28);
        CHECK(Refused(small.Build(), Check::Totals));
    }

    // Rule 4: a record's structSize disagrees with its struct's computed size.
    CHECK(Refused(with([&](Bytes& f) { Put16(f, rec1 - 2, 140); }), Check::RecordSize));

    // Indices and ranges.
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.structs + 16 * dcb::kShip + 4, 99); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.structs + 16 * dcb::kBase + 4, dcb::kShip); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put16(f, L.structs + 16 * dcb::kPart + 10, 40); }), Check::Structure));
    // Ship's "pos" (property 2 + 3 + 12: after Vec2's and Base's, its 13th own) as an inline struct out of
    // range, then as Ship itself.
    const uint64_t pos = L.properties + 12 * (2 + 3 + 12);
    CHECK(Refused(with([&](Bytes& f) { Put16(f, pos + 4, 99); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put16(f, pos + 4, dcb::kShip); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.mappings + 4, 99); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put16(f, L.enums + 6, 1); }), Check::Structure));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, rec1 - 24, 99); }), Check::Structure));          // struct index
    CHECK(Refused(with([&](Bytes& f) { Put16(f, rec1 - 4, 2); }), Check::Structure));            // root instance

    // Rule 5: names must land on a string start inside their pool.
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.records, 1); }), Check::NameOffset));          // mid-string
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.structs, 0x00FFFFFFu); }), Check::NameOffset));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.properties, Get32(f, L.properties) + 1); }), Check::NameOffset));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.enums, 2); }), Check::NameOffset));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.enumOptions, Get32(f, L.enumOptions) + 2); }), Check::NameOffset));
    CHECK(Refused(with([&](Bytes& f) { Put32(f, L.records + 4, 3); }), Check::NameOffset));      // file name
    CHECK(Refused(with([&](Bytes& f) { f[L.data - 1] = 'x'; }), Check::NameOffset));             // pool's last NUL
    CHECK(Refused(with([&](Bytes& f) { f[L.nameStrings - 1] = 'x'; }), Check::NameOffset));      // value pool's
    // Record +8 (records over 32 bytes) is a name-pool string start too (research R1).
    if (recordSize > 32) {
        CHECK(Refused(with([&](Bytes& f) { Put32(f, rec1 + 8, 1); }), Check::NameOffset));           // mid-string
        CHECK(Refused(with([&](Bytes& f) { Put32(f, L.records + 8, 0x00FFFFFFu); }), Check::NameOffset));
        Schema s;
        CHECK(s.Parse(with([&](Bytes& f) { Put32(f, L.records + 8, 0); })));                        // offset 0: a start
    }
}

// Rule 6: an unknown type code or array kind makes the struct opaque, not the file invalid.
static void TestOpaque() {
    const dcb::Prop mystery{ "mystery", 0x77, 0, dcb::kSingle, 4 };
    {   // No instances: nothing to size.
        dcb::Builder b = dcb::Fixture(36);
        b.structs[dcb::kEmpty].props.push_back(mystery);
        const Bytes f = b.Build();
        Schema s;
        CHECK(s.Parse(f));
        CHECK(s.OpaqueCount() == 1 && s.structInfo[dcb::kEmpty].opaque);
    }
    {   // Unknown array kind.
        dcb::Builder b = dcb::Fixture(36);
        b.structs[dcb::kEmpty].props.push_back({ "list", dcb::t::Int32, 0, 7 });
        const Bytes f = b.Build();
        Schema s;
        CHECK(s.Parse(f) && s.structInfo[dcb::kEmpty].opaque);
    }
    {   // Instances, sized from the records (PartX and PartY agree on 12 bytes).
        dcb::Builder b = dcb::Fixture(36);
        b.structs[dcb::kPart].props.push_back(mystery);
        const Bytes f = b.Build();
        Schema s;
        CHECK(s.Parse(f));
        CHECK(s.structInfo[dcb::kPart].opaque && s.structInfo[dcb::kPart].size == 12);
        CHECK(s.OpaqueCount() == 1);
    }
    {   // In a parent: the derived struct is opaque too; each is sized by its own records.
        dcb::Builder b = dcb::Fixture(32);
        b.structs[dcb::kBase].props.push_back(mystery);
        const Bytes f = b.Build();
        Schema s;
        CHECK(s.Parse(f));
        CHECK(s.OpaqueCount() == 2 && s.structInfo[dcb::kShip].opaque && s.structInfo[dcb::kShip].size == 143);
    }
    {   // Instances and no record to size them: the data section can't be checked.
        dcb::Builder b = dcb::Fixture(36);
        b.structs[dcb::kPart].props.push_back(mystery);
        b.records.resize(3);
        CHECK(Refused(b.Build(), Check::Totals));
    }
    {   // Records that disagree on the size.
        dcb::Builder b = dcb::Fixture(36);
        b.structs[dcb::kPart].props.push_back(mystery);
        Bytes f = b.Build();
        Put16(f, b.layout.records + 36 * 5 - 2, 13);
        CHECK(Refused(f, Check::Totals));
    }
}

// ---- truncated and corrupted files -------------------------------------------------------------

static bool Consistent(const Schema& s, size_t size) {
    uint64_t at = 0;
    for (const auto& t : s.tables) {
        if (t.offset != at) return false;
        at += t.bytes;
    }
    if (at != size) return false;
    for (const auto& r : s.records) {
        if (r.structIndex >= s.structs.size()) return false;
        (void)s.Name(r.name);
        (void)s.ValueString(r.fileName);
        (void)sco::datacore::FormatGuid(r.id);
    }
    for (uint32_t i = 0; i < s.structs.size(); ++i) (void)s.Properties(i);
    return true;
}

static void TestFuzz(uint32_t recordSize) {
    dcb::Builder b = dcb::Fixture(recordSize);
    const Bytes good = b.Build();

    // Every truncation is refused. Each copy is its own allocation, so ASan sees any read past it.
    int wrong = 0;
    for (size_t n = 0; n < good.size(); ++n) {
        const Bytes cut(good.begin(), good.begin() + static_cast<std::ptrdiff_t>(n));
        Schema s;
        if (s.Parse(cut) || s.failed == Check::None || s.error.empty()) ++wrong;
    }
    CHECK(wrong == 0);

    // Seeded corruption (std::mt19937's output is fixed by the standard): 1-4 random bytes, or a
    // u32 field anywhere before the data section set to an edge value. No crash, and either a
    // reasoned refusal or a schema whose tables still tile the file.
    std::mt19937 rng(20261009u + recordSize);
    const uint32_t edges[] = { 0u, 1u, 0x7FFFFFFFu, 0x80000000u, 0xFFFFFFFFu, 0xFFFFu, 0x10000u };
    int refused = 0, accepted = 0, bad = 0;
    for (int iter = 0; iter < 4000; ++iter) {
        Bytes f = good;
        if (rng() % 2) {
            const uint32_t n = 1 + rng() % 4;
            for (uint32_t k = 0; k < n; ++k) f[rng() % f.size()] = static_cast<uint8_t>(rng());
        } else {
            const uint64_t at = (rng() % (b.layout.data / 2)) * 2;
            if (at + 4 <= f.size()) Put32(f, at, edges[rng() % std::size(edges)]);
        }
        Schema s;
        if (s.Parse(f)) {
            ++accepted;
            if (!Consistent(s, f.size())) ++bad;
        } else {
            ++refused;
            if (s.failed == Check::None || s.error.empty()) ++bad;
        }
    }
    CHECK(bad == 0);
    CHECK(refused > 0);
    std::printf("  fuzz, %u-byte records: %d truncations refused, %d corruptions refused, %d accepted\n", recordSize,
                static_cast<int>(good.size()), refused, accepted);
}

// ---- fixtures for the sco-dcb tool test --------------------------------------------------------

static bool Write(const std::string& path, const Bytes& f) {
    FILE* out = std::fopen(path.c_str(), "wb");
    if (!out) return false;
    const bool ok = std::fwrite(f.data(), 1, f.size(), out) == f.size();
    return std::fclose(out) == 0 && ok;
}

static void WriteFixtures(const std::string& dir) {
    dcb::Builder b36 = dcb::Fixture(36), b32 = dcb::Fixture(32);
    Bytes bad = b36.Build();
    bad.pop_back();
    CHECK(Write(dir + "/datacore_36.dcb", b36.Build()));
    CHECK(Write(dir + "/datacore_32.dcb", b32.Build()));
    CHECK(Write(dir + "/datacore_bad.dcb", bad));
}

int main(int argc, char** argv) {
    TestRoundTrip(32);
    TestRoundTrip(36);
    TestRoundTrip(40);
    TestGuidFormat();
    TestNoRecords();
    TestRefusals(32);
    TestRefusals(36);
    TestOpaque();
    TestFuzz(32);
    TestFuzz(36);
    if (argc > 1) WriteFixtures(argv[1]);
    std::printf("sco-core datacore tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
