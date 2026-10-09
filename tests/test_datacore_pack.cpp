// Unit tests for DataCore data packs (sco/datacore_pack.h, docs/design/vfs-datacore.md section 5):
// golden parses of every tests/fixtures/datacore/golden/*.toml (the first line says the expected
// operation count or the first error, line included), WritePack round trips, and ApplyPacks over a
// tests/dcb_builder.h scene: every patcher refusal a pack can reach, plugin-order priority with
// conflicts, per-pack atomicity and atomic = false, @id dependencies, and the summary line. It also
// writes the fixture the sample pack (sdk/examples/quantum_pack) is checked against: a small file
// with the record, struct and field names of 4.10.193 that the pack uses (tests/dcb_pack.cmake).
//   test_datacore_pack <repo root> <out dir>
#include "dcb_builder.h"
#include "sco/datacore.h"
#include "sco/datacore_pack.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace sco::datacore;
namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

static fs::path g_root, g_out;

static std::string ReadText(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream s;
    s << in.rdbuf();
    return s.str();
}
static Bytes Le(uint64_t v, int n) {
    Bytes b(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) b[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    return b;
}
static Bytes F32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return Le(u, 4); }
static Bytes Pair(uint32_t a, uint32_t b) { Bytes x = Le(a, 4), y = Le(b, 4); x.insert(x.end(), y.begin(), y.end()); return x; }

// ---- the scene ---------------------------------------------------------------------------------

// dcb::Fixture with real values: ShipA's engine is PartY's sibling Part[1], its counts [1,2] and parts
// [Part 0, Part 1]; ShipB's engine is Part[0] (PartX's root), its arrays empty. `opaque` adds a
// struct with a field of unknown type and a record of it; `corrupt` points ShipB's counts outside
// the int32 pool.
static Bytes Scene(bool opaque = false, bool corrupt = false) {
    dcb::Builder b = dcb::Fixture(36);
    using dcb::kShip;
    using dcb::kPart;
    auto fill = [&](uint32_t st, uint32_t i, std::string f, Bytes v) { b.fills.push_back({ st, i, std::move(f), std::move(v) }); };
    const char* labels[] = { "hello", "world" };
    for (uint32_t s = 0; s < 2; ++s) {
        fill(kShip, s, "label", Le(b.Value(labels[s]), 4));
        fill(kShip, s, "kind", Le(b.Value("Large"), 4));
        fill(kShip, s, "title", Le(b.Value("@test_title"), 4));
        fill(kShip, s, "speed", F32(100.0f + static_cast<float>(s)));
        fill(kShip, s, "owner", Pair(0xFFFFFFFFu, 0xFFFFFFFFu));
        fill(kShip, s, "path", Pair(0, 0));
    }
    fill(kShip, 0, "engine", Pair(kPart, 1));
    fill(kShip, 0, "counts", Pair(2, 0));
    fill(kShip, 0, "parts", Pair(2, 0));
    fill(kShip, 1, "engine", Pair(kPart, 0));
    fill(kShip, 1, "counts", corrupt ? Pair(2, 0xFFFFFFF0u) : Pair(0, 0));
    fill(kShip, 1, "parts", Pair(0, 0));
    for (uint32_t p = 0; p < 4; ++p) {
        fill(kPart, p, "weight", F32(1.0f + static_cast<float>(p)));
        fill(kPart, p, "partName", Le(b.Value("p" + std::to_string(p)), 4));
    }
    if (opaque) {
        b.structs.push_back({ "Odd", -1, { { "x", 0x77, 0, dcb::kSingle, 4 }, { "y", dcb::t::Float } } });
        b.mappings.push_back({ static_cast<uint32_t>(b.structs.size() - 1), 1 });
        b.records.push_back({ "OddOne", "libs/foundry/records/test/odd.xml", static_cast<uint32_t>(b.structs.size() - 1),
                              dcb::MakeGuid(0x70), 0, 0 });
    }
    Bytes f = b.Build();
    if (b.layout.badFills) std::printf("  scene: %d bad fills\n", b.layout.badFills);
    return f;
}

// One value of the patched file, read back through a fresh parse: the FieldView text.
static std::string Val(const Bytes& file, const char* record, const char* field) {
    Schema s;
    if (!s.Parse(file)) return "(layout: " + s.error + ")";
    Patch p(s);
    RecordRef r;
    r.name = record;
    std::vector<FieldView> v;
    if (Status st = p.ReadFields(r, field, v, 0); !st || v.empty()) return "(" + st.message + ")";
    return v[0].text;
}

static Pack Parse(const std::string& text, const char* plugin = "p", const char* name = "datacore/a.toml") {
    Pack p;
    p.plugin = plugin;
    p.name = name;
    std::string error;
    if (!ParsePack(text, p, error)) std::printf("  parse (%s): %s\n", plugin, error.c_str());
    return p;
}

static Bytes Applied(const Bytes& base, const PackResult& r) {
    Bytes out;
    if (!r.status || !ApplySplices(base, r.splices, out)) return {};
    return out;
}

static std::string Set(const char* rec, const char* field, const char* value) {
    return std::string("\n[[set]]\nrecord = \"") + rec + "\"\nfield = \"" + field + "\"\nvalue = " + value + "\n";
}

// ---- golden parses -----------------------------------------------------------------------------

static void TestGolden() {
    int files = 0;
    std::vector<fs::path> paths;
    for (const auto& e : fs::directory_iterator(g_root / "tests" / "fixtures" / "datacore" / "golden")) paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());
    for (const fs::path& p : paths) {
        const std::string text = ReadText(p);
        const std::string prefix = "# expect: ";
        CHECK(text.rfind(prefix, 0) == 0);
        std::string expect = text.substr(prefix.size(), text.find('\n') - prefix.size());
        if (!expect.empty() && expect.back() == '\r') expect.pop_back();
        Pack pack;
        std::string error;
        const bool ok = ParsePack(text, pack, error);
        bool good;
        if (expect.rfind("ok ", 0) == 0) {
            good = ok && std::to_string(pack.ops.size()) == expect.substr(3);
            if (good) {   // the canonical form parses back to the same canonical form
                Pack again;
                std::string e2;
                const std::string w = WritePack(pack, "round trip");
                good = ParsePack(w, again, e2) && WritePack(again, "round trip") == w && again.ops.size() == pack.ops.size();
                if (!good) std::printf("  %s: round trip: %s\n%s\n", p.filename().string().c_str(), e2.c_str(), w.c_str());
            }
        } else {
            good = !ok && error.rfind(expect, 0) == 0 && pack.ops.empty();
        }
        if (!good)
            std::printf("  %s: expected \"%s\", got %s \"%s\" (%zu ops)\n", p.filename().string().c_str(), expect.c_str(),
                        ok ? "ok" : "error", error.c_str(), pack.ops.size());
        CHECK(good);
        ++files;
    }
    CHECK(files >= 20);
    // Limits and the BOM / CRLF forms.
    std::string error;
    Pack p;
    CHECK(!ParsePack(std::string(kMaxPackBytes + 1, '#'), p, error) && error.find("over the") != std::string::npos);
    CHECK(ParsePack("\xEF\xBB\xBF" "format = 1\r\n[[set]]\r\nrecord = \"A\"\r\nfield = \"x\"\r\nvalue = 1\r\n", p, error) && p.ops.size() == 1);
    std::string many = "format = 1\n";
    for (size_t i = 0; i <= kMaxPackOps; ++i) many += "[[append]]\nrecord = \"A\"\nfield = \"x\"\nvalue = 1\n";
    CHECK(!ParsePack(many, p, error) && error.find("more than 65536 operations") != std::string::npos);
    // plugin and name survive a parse; a failed parse leaves no operations.
    p = Pack{};
    p.plugin = "keep";
    p.name = "datacore/k.toml";
    CHECK(ParsePack("format = 1\n", p, error) && p.plugin == "keep" && p.name == "datacore/k.toml");
    // GUID text round trip.
    Guid g;
    CHECK(ParseGuid("17161514-1312-1110-1F1E-1D1C1B1A1918", g) && g.bytes == dcb::MakeGuid(0x10));
    CHECK(FormatGuid(g) == "17161514-1312-1110-1f1e-1d1c1b1a1918");
    CHECK(!ParseGuid("17161514-1312-1110-1f1e-1d1c1b1a191", g) && !ParseGuid("17161514+1312-1110-1f1e-1d1c1b1a1918", g) &&
          !ParseGuid("1716151g-1312-1110-1f1e-1d1c1b1a1918", g));
}

// ---- applying ----------------------------------------------------------------------------------

static void TestApplyAll() {
    const Bytes base = Scene();
    Schema s;
    CHECK(s.Parse(base));
    const Pack pack = Parse(ReadText(g_root / "tests" / "fixtures" / "datacore" / "golden" / "good_all.toml"));
    const PackResult r = ApplyPacks(s, std::vector<Pack>{ pack });
    CHECK(r.status.ok() && r.packs.size() == 1);
    for (const PackOpReport& op : r.packs[0].ops)
        if (!op.status) std::printf("  line %u: %s\n", op.line, op.status.message.c_str());
    CHECK(r.packs[0].state == PackState::Applied && r.packs[0].skipped == 0 && r.packs[0].applied == 15);
    const Bytes f = Applied(base, r);
    CHECK(!f.empty());
    CHECK(Val(f, "ShipA", "speed") == "2.5");
    CHECK(Val(f, "ShipB", "label") == "\"patched \\\"label\\\"\"");
    CHECK(Val(f, "ShipA", "kind") == "{ enum = \"Small\" }");
    CHECK(Val(f, "ShipA", "id") == "{ guid = \"93929190-9594-9796-9f9e-9d9c9b9a9998\" }");
    CHECK(Val(f, "ShipA", "u64") == "{ uint = \"18446744073709551615\" }");
    CHECK(Val(f, "ShipB", "engine.weight") == "11.0" && Val(f, "ShipB", "engine.partName") == "\"fresh\"");
    CHECK(Val(f, "PartX", "weight") == "1.0");                  // the clone source is untouched
    CHECK(Val(f, "ShipA", "owner") == "weak -> Base[0]");
    CHECK(Val(f, "ShipA", "counts") == "[3]" && Val(f, "ShipA", "counts[2]") == "30");
    CHECK(Val(f, "ShipA", "parts") == "[3]" && Val(f, "ShipA", "parts[2]") == "null");
    CHECK(Val(f, "ShipA", "path") == "[1]" && Val(f, "ShipA", "path[0].x") == "7.0");
    CHECK(Summary(r) == "[datacore] 1 pack: p 15/15 applied");
}

// Plugin order: the later pack wins a field both set; the conflict names both.
static void TestOrder() {
    const Bytes base = Scene();
    Schema s;
    CHECK(s.Parse(base));
    const std::string head = "format = 1\n";
    const Pack a = Parse(head + Set("ShipA", "speed", "1.0") + Set("ShipB", "speed", "5.0"), "alpha", "datacore/a.toml");
    const Pack b = Parse(head + Set("ShipA", "speed", "2.0"), "beta", "datacore/b.toml");
    // Another path to the same field: ShipB's engine is Part[0], so is PartX's root.
    const Pack c = Parse(head + Set("PartX", "weight", "8.0"), "gamma", "datacore/c.toml");
    const Pack d = Parse(head + Set("ShipB", "engine.weight", "9.0"), "delta", "datacore/d.toml");

    PackResult r = ApplyPacks(s, std::vector<Pack>{ a, b });
    Bytes f = Applied(base, r);
    CHECK(Val(f, "ShipA", "speed") == "2.0" && Val(f, "ShipB", "speed") == "5.0");
    CHECK(r.conflicts.size() == 1);
    if (!r.conflicts.empty()) {
        CHECK(r.conflicts[0].find("alpha (datacore/a.toml:3) overridden by beta (datacore/b.toml:3)") != std::string::npos);
        CHECK(r.conflicts[0].find("field \"speed\"") != std::string::npos);
    }
    CHECK(Summary(r) == "[datacore] 2 packs: alpha 2/2 applied; beta 1/1 applied; 1 conflict");

    r = ApplyPacks(s, std::vector<Pack>{ b, a });
    f = Applied(base, r);
    CHECK(Val(f, "ShipA", "speed") == "1.0" && r.conflicts.size() == 1);

    r = ApplyPacks(s, std::vector<Pack>{ c, d });
    f = Applied(base, r);
    CHECK(Val(f, "PartX", "weight") == "9.0" && r.conflicts.size() == 1);

    // A refused pack takes part in no conflict; the next pack sees the base value.
    const Pack bad = Parse(head + Set("ShipA", "speed", "3.0") + Set("ShipA", "nope", "1.0"), "bad", "datacore/x.toml");
    r = ApplyPacks(s, std::vector<Pack>{ a, bad, b });
    f = Applied(base, r);
    CHECK(Val(f, "ShipA", "speed") == "2.0" && r.conflicts.size() == 1 && r.packs[1].state == PackState::Refused);

    // One plugin, two files: both named plugin/file in the summary; one file setting a field twice
    // by two paths has the later operation refused.
    Pack a2 = a;
    a2.name = "datacore/z.toml";
    a2.ops.erase(a2.ops.begin());
    r = ApplyPacks(s, std::vector<Pack>{ a, a2 });
    CHECK(Summary(r).find("alpha/datacore/a.toml 2/2 applied; alpha/datacore/z.toml 1/1 applied") != std::string::npos);
    const Pack twice = Parse(head + Set("PartX", "weight", "8.0") + Set("ShipB", "engine.weight", "9.0"), "twice");
    r = ApplyPacks(s, std::vector<Pack>{ twice });
    CHECK(r.packs[0].state == PackState::Refused && r.packs[0].ops.size() == 2);
    CHECK(r.packs[0].ops[1].status.category == Refusal::BadArgument &&
          r.packs[0].ops[1].status.message.find("sets the same field as line 3") != std::string::npos);
}

// Per-pack atomicity, atomic = false, and @id dependencies.
static void TestAtomicity() {
    const Bytes base = Scene();
    Schema s;
    CHECK(s.Parse(base));
    const std::string body = Set("ShipA", "speed", "3.0") + Set("ShipA", "nope", "1.0") + Set("ShipB", "speed", "4.0");
    const Pack atomic = Parse("format = 1\n" + body, "at");
    const Pack loose = Parse("format = 1\natomic = false\n" + body, "loose");
    const Pack other = Parse("format = 1\n" + Set("ShipB", "mass", "7.0"), "other");

    PackResult r = ApplyPacks(s, std::vector<Pack>{ atomic, other });
    Bytes f = Applied(base, r);
    CHECK(r.packs[0].state == PackState::Refused && r.packs[0].applied == 0 && r.packs[0].skipped == 3);
    CHECK(r.packs[0].reason.rfind("line 8: record \"ShipA\" field \"nope\": no property \"nope\" in Ship", 0) == 0);
    CHECK(r.packs[0].ops[0].status.ok() && r.packs[0].ops[1].status.category == Refusal::FieldNotFound);
    CHECK(Val(f, "ShipA", "speed") == "100.0" && Val(f, "ShipB", "speed") == "101.0" && Val(f, "ShipB", "mass") == "7.0");
    CHECK(Summary(r) == "[datacore] 2 packs: at refused (" + r.packs[0].reason + "); other 1/1 applied");

    r = ApplyPacks(s, std::vector<Pack>{ loose });
    f = Applied(base, r);
    CHECK(r.packs[0].state == PackState::Partial && r.packs[0].applied == 2 && r.packs[0].skipped == 1);
    CHECK(Val(f, "ShipA", "speed") == "3.0" && Val(f, "ShipB", "speed") == "4.0");
    CHECK(Summary(r).rfind("[datacore] 1 pack: loose 2/3 applied (1 skipped: line 9: ", 0) == 0);

    // An [[instance]] is all or nothing, also with atomic = false; what uses it is refused.
    const Pack dep = Parse(R"(format = 1
atomic = false
[[instance]]
id = "n"
struct = "Part"
set = { "weight" = 5.0, "missing" = 1.0 }
[[set]]
record = "ShipA"
field = "engine"
pointer = "@n"
)" + Set("ShipA", "speed", "6.0"),
                           "dep");
    r = ApplyPacks(s, std::vector<Pack>{ dep });
    f = Applied(base, r);
    CHECK(r.packs[0].state == PackState::Partial && r.packs[0].applied == 1);
    CHECK(r.packs[0].ops.size() == 5);
    if (r.packs[0].ops.size() == 5) {
        CHECK(r.packs[0].ops[0].status.category == Refusal::DependencyFailed);   // AddInstance: dropped with its set table
        CHECK(r.packs[0].ops[2].status.category == Refusal::FieldNotFound);
        CHECK(r.packs[0].ops[3].status.category == Refusal::DependencyFailed);   // the pointer to it
    }
    CHECK(Val(f, "ShipA", "engine") == "-> Part[1]" && Val(f, "ShipA", "speed") == "6.0");
    // No new Part was added: the mapping count is unchanged.
    Schema re;
    CHECK(re.Parse(f) && re.structInfo[dcb::kPart].instances == s.structInfo[dcb::kPart].instances);
}

// Every patcher refusal a pack can reach, by category (Limit needs a 4 GiB pool and Revalidation a
// patcher bug; the patch tests cover both paths).
static void TestRefusals() {
    const Bytes base = Scene(true, true);
    Schema s;
    CHECK(s.Parse(base));
    struct Case { const char* body; Refusal want; };
    const Case cases[] = {
        { "[[set]]\nrecord = \"Nope\"\nfield = \"speed\"\nvalue = 1.0\n", Refusal::RecordNotFound },
        { "[[set]]\nguid = \"00000000-0000-0000-0000-000000000000\"\nfield = \"speed\"\nvalue = 1.0\n", Refusal::RecordNotFound },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"speedX\"\nvalue = 1.0\n", Refusal::FieldNotFound },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"counts[5]\"\nvalue = 1\n", Refusal::IndexOutOfRange },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"parts[Nope].weight\"\nvalue = 1.0\n", Refusal::StructNotFound },
        { "[[instance]]\nid = \"x\"\nstruct = \"Nope\"\n", Refusal::StructNotFound },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"speed\"\nvalue = \"fast\"\n", Refusal::TypeMismatch },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"speed\"\npointer = \"null\"\n", Refusal::TypeMismatch },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"i8\"\nvalue = 300\n", Refusal::ValueOutOfRange },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"kind\"\nvalue = { enum = \"Huge\" }\n", Refusal::UnknownEnumOption },
        { "[[set]]\nrecord = \"OddOne\"\nfield = \"y\"\nvalue = 1.0\n", Refusal::Opaque },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"maker\"\nvalue = { guid = \"17161514-1312-1110-1f1e-1d1c1b1a1918\" }\n", Refusal::Unsupported },
        { "[[instance]]\nid = \"x\"\nstruct = \"Part\"\nclone = { record = \"Nope\" }\n[[set]]\nrecord = \"ShipA\"\nfield = \"engine\"\npointer = \"@x\"\n",
          Refusal::RecordNotFound },
        { "[[set]]\nrecord = \"ShipA\"\nfield = \"engine\"\npointer = { record = \"ShipA\", field = \"pos\" }\n", Refusal::TypeMismatch },
        { "[[set]]\nrecord = \"ShipB\"\nfield = \"counts[0]\"\nvalue = 1\n", Refusal::Corrupt },
    };
    for (const Case& c : cases) {
        const Pack p = Parse(std::string("format = 1\n") + c.body, "r");
        const PackResult r = ApplyPacks(s, std::vector<Pack>{ p });
        Refusal got = Refusal::None;
        for (const PackOpReport& op : r.packs[0].ops)
            if (!op.status) { got = op.status.category; break; }
        const bool ok = got == c.want && r.packs[0].state == PackState::Refused && r.splices.empty() && r.status.ok();
        if (!ok) std::printf("  refusal case: wanted %s, got %s:\n%s\n", RefusalName(c.want), RefusalName(got), c.body);
        CHECK(ok);
    }
    // The dependency on a refused [[instance]] in an atomic pack is reported DependencyFailed too.
    {
        const Pack p = Parse("format = 1\n[[instance]]\nid = \"x\"\nstruct = \"Nope\"\n[[set]]\nrecord = \"ShipA\"\nfield = \"engine\"\npointer = \"@x\"\n", "r");
        const PackResult r = ApplyPacks(s, std::vector<Pack>{ p });
        CHECK(r.packs[0].ops.size() == 2 && r.packs[0].ops[1].status.category == Refusal::DependencyFailed);
    }
    // A refused layout: every pack refused, nothing emitted, the reason names the layout.
    {
        Bytes cut = Scene();
        cut.pop_back();
        Schema bad;
        CHECK(!bad.Parse(cut));
        const Pack p = Parse("format = 1\n" + Set("ShipA", "speed", "1.0"), "l");
        const PackResult r = ApplyPacks(bad, std::vector<Pack>{ p, p });
        CHECK(!r.status && r.status.category == Refusal::Layout && r.splices.empty());
        CHECK(r.packs.size() == 2 && r.packs[0].state == PackState::Refused && r.packs[1].ops[0].status.category == Refusal::Layout);
        CHECK(Summary(r).rfind("[datacore] 2 packs (nothing applied: the base file failed validation: ", 0) == 0);
    }
    // No packs: nothing to do.
    {
        const PackResult r = ApplyPacks(s, std::vector<Pack>{});
        CHECK(r.status.ok() && r.splices.empty() && Summary(r) == "[datacore] 0 packs: none");
    }
}

// ---- the sample pack's fixture -----------------------------------------------------------------

// The names sdk/examples/quantum_pack uses, as in 4.10.193: an EntityClassDefinition's Components
// array of strong pointers to component params, one of them SCItemQuantumDriveParams with inline
// `params` and `splineJumpParams`. Values are those `sco-dcb show` prints for the real Eos drive.
static Bytes QuantumFixture() {
    using namespace dcb;
    Builder b;
    b.recordSize = 36;
    enum : uint32_t { kComp, kQParams, kQItem, kAttach, kEntity };
    b.structs = {
        { "DataForgeComponentParams", -1, {} },
        { "SQuantumDriveParams", -1,
          { { "driveSpeed", t::Float }, { "cooldownTime", t::Float }, { "stageOneAccelRate", t::Float }, { "spoolUpTime", t::Float } } },
        { "SCItemQuantumDriveParams", kComp, { { "params", t::Class, kQParams }, { "splineJumpParams", t::Class, kQParams } } },
        { "SAttachableComponentParams", kComp, { { "Size", t::Int32 } } },
        { "EntityClassDefinition", -1, { { "Components", t::Strong, kComp, kArray2 } } },
    };
    b.mappings = { { kComp, 0 }, { kQParams, 0 }, { kQItem, 2 }, { kAttach, 2 }, { kEntity, 2 } };
    auto guid = [](const char* text) {
        sco::datacore::Guid g;
        ParseGuid(text, g);
        return g.bytes;
    };
    const char* dir = "libs/foundry/records/entities/scitem/ships/quantumdrive/";
    b.records = {
        { "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem", std::string(dir) + "qdrv_rsi_s01_eos_scitem.xml", kEntity,
          guid("08a5bfdb-1972-421f-83fe-be03b7ac5222"), 0, 0 },
        { "EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem", std::string(dir) + "qdrv_wetk_s01_beacon_scitem.xml", kEntity,
          guid("b5cbef86-f37f-4e05-8aff-e0f61afe166e"), 1, 0 },
    };
    for (uint32_t i = 0; i < 2; ++i) {
        b.PushPointer(Strong, kAttach, i);
        b.PushPointer(Strong, kQItem, i);
        b.fills.push_back({ kEntity, i, "Components", Pair(2, 2 * i) });
        b.fills.push_back({ kAttach, i, "Size", Le(1, 4) });
        for (const char* part : { "params", "splineJumpParams" }) {
            const std::string p = part;
            b.fills.push_back({ kQItem, i, p + ".driveSpeed", F32(i ? 1.6e8f : 1.992727e8f) });
            b.fills.push_back({ kQItem, i, p + ".cooldownTime", F32(10.8f) });
            b.fills.push_back({ kQItem, i, p + ".stageOneAccelRate", F32(7685182.0f) });
            b.fills.push_back({ kQItem, i, p + ".spoolUpTime", F32(5.1f) });
        }
    }
    Bytes f = b.Build();
    if (b.layout.badFills) std::printf("  quantum fixture: %d bad fills\n", b.layout.badFills);
    return f;
}

static bool LoadPackDir(const fs::path& dir, std::vector<Pack>& out) {
    std::vector<fs::path> files;
    for (const auto& e : fs::directory_iterator(dir / "datacore"))
        if (e.path().extension() == ".toml") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (const fs::path& f : files) {
        Pack p;
        p.plugin = dir.filename().string();
        p.name = "datacore/" + f.filename().string();
        std::string error;
        if (!ParsePack(ReadText(f), p, error)) {
            std::printf("  %s: %s\n", f.string().c_str(), error.c_str());
            return false;
        }
        out.push_back(std::move(p));
    }
    return !out.empty();
}

static void TestSamplePack() {
    const Bytes base = QuantumFixture();
    Schema s;
    CHECK(s.Parse(base));
    std::vector<Pack> packs;
    CHECK(LoadPackDir(g_root / "sdk" / "examples" / "quantum_pack", packs));
    const PackResult r = ApplyPacks(s, packs);
    CHECK(r.status.ok() && r.packs.size() == 1 && r.packs[0].state == PackState::Applied);
    const Bytes f = Applied(base, r);
    const char* eos = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem";
    const char* beacon = "EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem";
    CHECK(Val(f, eos, "Components[SCItemQuantumDriveParams].params.spoolUpTime") == "3.5");
    CHECK(Val(f, eos, "Components[SCItemQuantumDriveParams].params.cooldownTime") == "8.0");
    CHECK(Val(f, beacon, "Components[SCItemQuantumDriveParams].params.driveSpeed") == "2.5e+08");
    CHECK(Val(f, beacon, "Components[SCItemQuantumDriveParams].params.spoolUpTime") == "4.0");
    CHECK(Val(f, beacon, "Components[SCItemQuantumDriveParams].params.cooldownTime") == "8.0");   // cloned after the Eos change
    CHECK(Val(f, beacon, "Components[SCItemQuantumDriveParams]") == "-> SCItemQuantumDriveParams[2]");

    std::vector<Pack> bad;
    CHECK(LoadPackDir(g_root / "tests" / "fixtures" / "datacore" / "missing_field", bad));
    const PackResult rb = ApplyPacks(s, bad);
    CHECK(rb.packs.size() == 1 && rb.packs[0].state == PackState::Refused && rb.packs[0].reason.find("no property \"spoolTime\"") != std::string::npos);

    // The files tests/dcb_pack.cmake runs sco-dcb on: the fixture, and the same with the sample applied (diff).
    std::ofstream(g_out / "quantum_fixture.dcb", std::ios::binary).write(reinterpret_cast<const char*>(base.data()), static_cast<std::streamsize>(base.size()));
    std::ofstream(g_out / "quantum_fixture_patched.dcb", std::ios::binary).write(reinterpret_cast<const char*>(f.data()), static_cast<std::streamsize>(f.size()));
    CHECK(fs::file_size(g_out / "quantum_fixture.dcb") == base.size() && fs::file_size(g_out / "quantum_fixture_patched.dcb") == f.size());
}

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: test_datacore_pack <repo root> <out dir>\n");
        return 2;
    }
    g_root = argv[1];
    g_out = argv[2];
    fs::create_directories(g_out);
    TestGolden();
    TestApplyAll();
    TestOrder();
    TestAtomicity();
    TestRefusals();
    TestSamplePack();
    std::printf("sco-core datacore pack tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
