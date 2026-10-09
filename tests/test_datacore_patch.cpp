// Unit tests for the DataCore patcher (sco::datacore::Patch, docs/design/vfs-datacore.md section 4)
// over synthetic files from tests/dcb_builder.h. Each operation is applied, the splices are applied
// through sco::vfs, and the result is read back by an independent reader written here (it shares no
// code with src/datacore/), which also checks that untouched records' root bytes didn't change.
// Drift: the same operations on two fixtures with moved tables (other struct indices, counts, pool
// offsets, a struct gaining fields, records reordered, a struct split over two mappings) and both
// record sizes give the same semantic result at different byte offsets. Refusals are checked by
// category. Truncated and corrupted inputs refuse cleanly (ASan+UBSan).
//   tools/test.sh
#include "dcb_builder.h"
#include "sco/datacore.h"
#include "sco/vfs.h"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <random>
#include <string>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using namespace sco::datacore;
using Bytes = std::vector<uint8_t>;

// ---- little-endian helpers ---------------------------------------------------------------------

static Bytes Le(uint64_t v, int n) {
    Bytes b(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) b[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
    return b;
}
static Bytes F32(float v) { uint32_t u; std::memcpy(&u, &v, 4); return Le(u, 4); }
static Bytes F64(double v) { uint64_t u; std::memcpy(&u, &v, 8); return Le(u, 8); }
static Bytes Pair(uint32_t a, uint32_t b) { Bytes x = Le(a, 4), y = Le(b, 4); x.insert(x.end(), y.begin(), y.end()); return x; }
static uint64_t GetN(const Bytes& f, uint64_t at, int n) {
    uint64_t v = 0;
    if (at + static_cast<uint64_t>(n) > f.size()) return 0;
    for (int i = 0; i < n; ++i) v |= static_cast<uint64_t>(f[at + static_cast<uint64_t>(i)]) << (8 * i);
    return v;
}
static uint32_t Get32(const Bytes& f, uint64_t at) { return static_cast<uint32_t>(GetN(f, at, 4)); }
static Guid ToGuid(const dcb::Guid& g) { Guid out; out.bytes = g; return out; }
static RecordRef Rec(std::string name) { RecordRef r; r.name = std::move(name); return r; }
static RecordRef RecG(const dcb::Guid& g) { RecordRef r; r.guid = ToGuid(g); return r; }
static InstanceSource Src(std::string record, std::string field = {}) {
    InstanceSource s;
    s.record = Rec(std::move(record));
    s.field = std::move(field);
    return s;
}

// ---- the scene: one semantic content, two layouts ----------------------------------------------

constexpr uint32_t kNullPtr = 0xFFFFFFFFu;

struct Scene {
    dcb::Builder b;
    Bytes file;
    uint32_t S(std::string_view name) const {
        for (uint32_t i = 0; i < b.structs.size(); ++i)
            if (b.structs[i].name == name) return i;
        return kNullPtr;
    }
};

// `drift` moves everything the design says changes between patches: struct indices (another order,
// an extra struct first), enum option indices, pool offsets (junk entries first), instance indices
// (junk instances first, Part split over two mappings), Ship gaining two fields, records reordered.
static Scene MakeScene(uint32_t recordSize, bool drift) {
    using dcb::Prop;
    namespace t = dcb::t;
    Scene sc;
    dcb::Builder& b = sc.b;
    b.recordSize = recordSize;
    const std::vector<std::string> order =
        drift ? std::vector<std::string>{ "Pad", "Part", "SubPart", "Orphan", "Vec2", "Ship", "Base", "Empty" }
              : std::vector<std::string>{ "Vec2", "Base", "Ship", "Part", "SubPart", "Empty", "Orphan" };
    auto S = [&](std::string_view n) -> uint16_t {
        for (size_t i = 0; i < order.size(); ++i)
            if (order[i] == n) return static_cast<uint16_t>(i);
        return 0xFFFF;
    };
    const uint16_t kindEnum = drift ? 1 : 0;
    for (const std::string& n : order) {
        dcb::Struct s;
        s.name = n;
        if (n == "Pad") s.props = { { "a", t::Int32 }, { "b", t::String } };
        else if (n == "Vec2") s.props = { { "x", t::Float }, { "y", t::Float } };
        else if (n == "Part") s.props = { { "weight", t::Float }, { "partName", t::String } };
        else if (n == "SubPart") { s.parent = S("Part"); s.props = { { "grade", t::Int32 } }; }
        else if (n == "Base") s.props = { { "flag", t::Bool }, { "label", t::String }, { "kind", t::Enum, kindEnum } };
        else if (n == "Orphan") s.props = { { "v", t::Int32 } };
        else if (n == "Ship") {
            s.parent = S("Base");
            if (drift) s.props.push_back({ "notes", t::String });
            s.props.push_back({ "speed", t::Float });
            s.props.push_back({ "mass", t::Double });
            s.props.push_back({ "small", t::Int8 });
            if (drift) s.props.push_back({ "extra", t::UInt32 });
            const std::vector<Prop> rest = {
                { "big", t::UInt64 }, { "id", t::Guid }, { "title", t::Locale }, { "pos", t::Class, S("Vec2") },
                { "engine", t::Strong, S("Part") }, { "owner", t::Weak, S("Base") }, { "maker", t::Reference },
                { "counts", t::Int32, 0, dcb::kArray1 }, { "parts", t::Strong, S("Part"), dcb::kArray2 },
                { "path", t::Class, S("Vec2"), dcb::kArray3 }, { "names", t::String, 0, dcb::kArray1 },
            };
            s.props.insert(s.props.end(), rest.begin(), rest.end());
        }
        b.structs.push_back(s);
    }
    if (drift) b.enums = { { "Other", { "A", "B" } }, { "Kind", { "Tiny", "Small", "Large" } } };
    else b.enums = { { "Kind", { "Small", "Large" } } };

    const uint32_t vOff = drift ? 2 : 0, pOff = drift ? 1 : 0;
    const uint32_t shipA = drift ? 1 : 0, shipB = drift ? 0 : 1;
    if (drift)
        b.mappings = { { S("Pad"), 3 }, { S("Part"), 2 }, { S("SubPart"), 2 }, { S("Empty"), 0 },
                       { S("Vec2"), 4 + vOff }, { S("Part"), 2 }, { S("Ship"), 2 }, { S("Base"), 1 } };
    else
        b.mappings = { { S("Vec2"), 4 }, { S("Base"), 1 }, { S("Ship"), 2 }, { S("Part"), 3 }, { S("SubPart"), 2 }, { S("Empty"), 0 } };

    auto fill = [&](std::string_view st, uint32_t i, std::string field, Bytes bytes) {
        b.fills.push_back({ S(st), i, std::move(field), std::move(bytes) });
    };
    auto str = [&](std::string_view s) { return Le(b.Value(s), 4); };
    if (drift) {
        for (uint32_t v : { 99u, 98u, 97u }) b.PushU32(dcb::Int32, v);
        b.PushString("junk");
        b.PushPointer(dcb::Strong, S("Part"), 0);
        for (uint32_t i = 0; i < vOff; ++i) fill("Vec2", i, "x", F32(-9));
        fill("Part", 0, "weight", F32(99));
        fill("Part", 0, "partName", str("junk"));
        fill("Ship", shipA, "notes", str("note"));
        fill("Ship", shipA, "extra", Le(77, 4));
    }
    for (uint32_t k = 0; k < 4; ++k) {
        fill("Vec2", vOff + k, "x", F32(static_cast<float>(k)));
        fill("Vec2", vOff + k, "y", F32(static_cast<float>(k) + 0.5f));
    }
    for (uint32_t k = 0; k < 3; ++k) {
        fill("Part", pOff + k, "weight", F32(1.0f + static_cast<float>(k)));
        fill("Part", pOff + k, "partName", str("p" + std::to_string(k)));
    }
    for (uint32_t k = 0; k < 2; ++k) {
        fill("SubPart", k, "weight", F32(10.0f + static_cast<float>(k)));
        fill("SubPart", k, "partName", str("s" + std::to_string(k)));
        fill("SubPart", k, "grade", Le(5 + k, 4));
    }
    fill("Base", 0, "flag", { 1 });
    fill("Base", 0, "label", str("base"));
    fill("Base", 0, "kind", str("Small"));

    // ShipA: arrays in every pool kind, pointers, an inline struct.
    const uint32_t countsAt = static_cast<uint32_t>(b.pools[dcb::Int32].size() / 4);
    for (uint32_t v : { 10u, 20u, 3u }) b.PushU32(dcb::Int32, v);   // 3: another array's, so counts isn't at the end
    const uint32_t partsAt = static_cast<uint32_t>(b.pools[dcb::Strong].size() / 8);
    b.PushPointer(dcb::Strong, S("Part"), pOff + 0);
    b.PushPointer(dcb::Strong, S("SubPart"), 1);
    b.PushPointer(dcb::Strong, S("Part"), pOff + 2);   // ShipB's parts
    const uint32_t namesAt = static_cast<uint32_t>(b.pools[dcb::String].size() / 4);
    b.PushString("n0");
    b.PushString("n1");
    b.PushU32(dcb::Weak, 0);   // a pool entry nobody uses, so the weak pool isn't empty
    b.PushU32(dcb::Weak, 0);
    b.PushReference(0, dcb::MakeGuid(0x20));

    const struct { uint32_t i; const char* label; const char* kind; float speed; double mass; int8_t small; uint64_t big; uint8_t id; } ships[] = {
        { shipA, "hello", "Large", 100.0f, 5.0, 1, 9, 0x70 },
        { shipB, "world", "Small", 200.0f, 6.0, 2, 0, 0x80 },
    };
    for (const auto& s : ships) {
        fill("Ship", s.i, "flag", { 0 });
        fill("Ship", s.i, "label", str(s.label));
        fill("Ship", s.i, "kind", str(s.kind));
        fill("Ship", s.i, "speed", F32(s.speed));
        fill("Ship", s.i, "mass", F64(s.mass));
        fill("Ship", s.i, "small", Le(static_cast<uint8_t>(s.small), 1));
        fill("Ship", s.i, "big", Le(s.big, 8));
        const dcb::Guid g = dcb::MakeGuid(s.id);
        fill("Ship", s.i, "id", Bytes(g.begin(), g.end()));
        fill("Ship", s.i, "title", str("@title"));
        fill("Ship", s.i, "owner", Pair(kNullPtr, kNullPtr));
    }
    fill("Ship", shipA, "pos.x", F32(0.5f));
    fill("Ship", shipA, "pos.y", F32(0.25f));
    fill("Ship", shipA, "engine", Pair(S("Part"), pOff + 1));
    fill("Ship", shipA, "counts", Pair(2, countsAt));
    fill("Ship", shipA, "parts", Pair(2, partsAt));
    fill("Ship", shipA, "path", Pair(2, vOff + 1));
    fill("Ship", shipA, "names", Pair(2, namesAt));
    fill("Ship", shipB, "pos.x", F32(0));
    fill("Ship", shipB, "pos.y", F32(0));
    fill("Ship", shipB, "engine", Pair(S("SubPart"), 0));
    fill("Ship", shipB, "counts", Pair(0, 0));
    fill("Ship", shipB, "parts", Pair(1, partsAt + 2));
    fill("Ship", shipB, "path", Pair(0, 0));
    fill("Ship", shipB, "names", Pair(0, 0));

    const dcb::Record recs[] = {
        { "ShipA", "libs/foundry/records/test/ships.xml", S("Ship"), dcb::MakeGuid(0x10), static_cast<uint16_t>(shipA), 0x1111 },
        { "ShipB", "libs/foundry/records/test/ships.xml", S("Ship"), dcb::MakeGuid(0x20), static_cast<uint16_t>(shipB), 0x1111 },
        { "BaseOne", "libs/foundry/records/test/base.xml", S("Base"), dcb::MakeGuid(0x30), 0, 0x2222 },
        { "PartX", "libs/foundry/records/test/part_x.xml", S("Part"), dcb::MakeGuid(0x40), static_cast<uint16_t>(pOff), 0x3333 },
        { "SubY", "libs/foundry/records/test/sub_y.xml", S("SubPart"), dcb::MakeGuid(0x50), 0, 0x4444 },
    };
    if (drift) {
        b.records = { { "PadRec", "libs/foundry/records/test/pad.xml", S("Pad"), dcb::MakeGuid(0x60), 0, 0 },
                      recs[4], recs[1], recs[3], recs[0], recs[2] };
    } else {
        b.records.assign(std::begin(recs), std::end(recs));
    }
    sc.file = b.Build();
    return sc;
}

// ---- an independent reader of the patched bytes ------------------------------------------------

// Knows the scene's struct definitions (patches never change them) and reads everything else from
// the bytes: header counts, mapping counts, records, pools.
struct TestReader {
    const dcb::Builder& b;
    const Bytes& f;
    uint64_t pools[dcb::kPoolCount] = {}, records = 0, values = 0, names = 0, mappings = 0;
    std::vector<uint64_t> blocks;
    std::vector<uint32_t> counts;
    bool ok = false;

    TestReader(const dcb::Builder& builder, const Bytes& file) : b(builder), f(file) {
        if (f.size() < 120) return;
        const uint64_t sc = Get32(f, 16), pc = Get32(f, 20), ec = Get32(f, 24), mc = Get32(f, 28), rc = Get32(f, 32);
        if (sc != b.structs.size() || mc != b.mappings.size()) return;
        mappings = 120 + sc * 16 + pc * 12 + ec * 8;
        records = mappings + mc * 8;
        uint64_t at = records + rc * b.recordSize;
        for (int p = 0; p < dcb::kPoolCount; ++p) {
            pools[p] = at;
            at += Count(p) * dcb::kPoolEntry[p];
        }
        at += Get32(f, 108) * 4ull;
        values = at;
        at += Get32(f, 112);
        names = at;
        at += Get32(f, 116);
        for (uint64_t m = 0; m < mc; ++m) {
            counts.push_back(Get32(f, mappings + 8 * m));
            blocks.push_back(at);
            at += counts.back() * b.Size(b.mappings[m].structIndex);
        }
        ok = at == f.size();
    }
    uint64_t Count(int pool) const {
        for (int i = 0; i < 18; ++i)
            if (dcb::kHeaderOrder[i] == pool) return Get32(f, 36 + 4 * static_cast<uint64_t>(i));
        return 0;
    }
    std::string CStr(uint64_t at) const {
        std::string s;
        while (at < f.size() && f[at]) s += static_cast<char>(f[at++]);
        return s;
    }
    std::string Text(uint32_t off) const { return CStr(values + off); }
    uint64_t Instance(uint32_t st, uint64_t i) const {
        for (size_t m = 0; m < b.mappings.size(); ++m) {
            if (b.mappings[m].structIndex != st) continue;
            if (i < counts[m]) return blocks[m] + i * b.Size(st);
            i -= counts[m];
        }
        return ~0ull;
    }
    bool Root(std::string_view name, uint32_t& st, uint64_t& at) const {
        const uint64_t rc = Get32(f, 32);
        for (uint64_t r = 0; r < rc; ++r) {
            const uint64_t e = records + r * b.recordSize, tail = e + b.recordSize - 24;
            if (CStr(names + Get32(f, e)) != name) continue;
            st = Get32(f, tail);
            at = Instance(st, GetN(f, tail + 20, 2));
            return at != ~0ull;
        }
        return false;
    }
    uint64_t Field(uint32_t st, uint64_t inst, std::string_view path) const { return inst + b.FieldOffset(st, path); }
    float F(uint64_t at) const { const uint32_t u = Get32(f, at); float v; std::memcpy(&v, &u, 4); return v; }
    double D(uint64_t at) const { const uint64_t u = GetN(f, at, 8); double v; std::memcpy(&v, &u, 8); return v; }
    uint64_t PoolEntry(int pool, uint64_t i) const { return pools[pool] + i * dcb::kPoolEntry[pool]; }

    // "Part(w=2,n=p1)", "SubPart(w=11,n=s1,g=6)", "Base(label=base)", "Vec2(1,1.5)", "null".
    std::string Describe(uint32_t st, uint32_t i) const {
        if (st == kNullPtr) return "null";
        if (st >= b.structs.size()) return "bad";
        const uint64_t at = Instance(st, i);
        if (at == ~0ull) return "missing";
        const std::string& n = b.structs[st].name;
        char buf[128];
        if (n == "Part" || n == "SubPart") {
            std::snprintf(buf, sizeof(buf), "%s(w=%g,n=%s", n.c_str(), F(Field(st, at, "weight")),
                          Text(Get32(f, Field(st, at, "partName"))).c_str());
            std::string s = buf;
            if (n == "SubPart") s += ",g=" + std::to_string(static_cast<int32_t>(Get32(f, Field(st, at, "grade"))));
            return s + ")";
        }
        if (n == "Vec2") { std::snprintf(buf, sizeof(buf), "Vec2(%g,%g)", F(at), F(at + 4)); return buf; }
        if (n == "Base") return "Base(label=" + Text(Get32(f, Field(st, at, "label"))) + ")";
        return n;
    }
};

// Everything the scene says about ShipA and ShipB, by name: identical for any layout.
static std::map<std::string, std::string> Observe(const Scene& sc, const Bytes& f) {
    std::map<std::string, std::string> o;
    TestReader r(sc.b, f);
    o["layout"] = r.ok ? "ok" : "bad";
    if (!r.ok) return o;
    for (const char* ship : { "ShipA", "ShipB" }) {
        uint32_t st = 0;
        uint64_t at = 0;
        if (!r.Root(ship, st, at)) { o[ship] = "missing"; continue; }
        const std::string p = std::string(ship) + ".";
        char buf[64];
        auto num = [&](const char* key, double v) { std::snprintf(buf, sizeof(buf), "%g", v); o[p + key] = buf; };
        num("speed", r.F(r.Field(st, at, "speed")));
        num("mass", r.D(r.Field(st, at, "mass")));
        num("pos.x", r.F(r.Field(st, at, "pos.x")));
        num("pos.y", r.F(r.Field(st, at, "pos.y")));
        o[p + "small"] = std::to_string(static_cast<int8_t>(f[r.Field(st, at, "small")]));
        o[p + "big"] = std::to_string(GetN(f, r.Field(st, at, "big"), 8));
        o[p + "flag"] = std::to_string(f[r.Field(st, at, "flag")]);
        o[p + "id"] = std::to_string(f[r.Field(st, at, "id")]);
        o[p + "label"] = r.Text(Get32(f, r.Field(st, at, "label")));
        o[p + "kind"] = r.Text(Get32(f, r.Field(st, at, "kind")));
        o[p + "title"] = r.Text(Get32(f, r.Field(st, at, "title")));
        const uint64_t eng = r.Field(st, at, "engine"), own = r.Field(st, at, "owner");
        o[p + "engine"] = r.Describe(Get32(f, eng), Get32(f, eng + 4));
        o[p + "owner"] = r.Describe(Get32(f, own), Get32(f, own + 4));
        auto list = [&](const char* field, auto&& item) {
            const uint64_t a = r.Field(st, at, field);
            const uint32_t n = Get32(f, a), first = Get32(f, a + 4);
            std::string s = "[";
            for (uint32_t i = 0; i < n; ++i) s += (i ? "," : "") + item(static_cast<uint64_t>(first) + i);
            o[p + field] = s + "]";
        };
        list("counts", [&](uint64_t i) { return std::to_string(static_cast<int32_t>(Get32(f, r.PoolEntry(dcb::Int32, i)))); });
        list("parts", [&](uint64_t i) {
            const uint64_t e = r.PoolEntry(dcb::Strong, i);
            return r.Describe(Get32(f, e), Get32(f, e + 4));
        });
        list("path", [&](uint64_t i) { return r.Describe(sc.S("Vec2"), static_cast<uint32_t>(i)); });
        list("names", [&](uint64_t i) { return r.Text(Get32(f, r.PoolEntry(dcb::String, i))); });
    }
    return o;
}

// Root instance bytes of every record not in `touched`, before and after.
static bool UntouchedSame(const Scene& sc, const Bytes& patched, std::vector<std::string> touched) {
    TestReader before(sc.b, sc.file), after(sc.b, patched);
    if (!before.ok || !after.ok) return false;
    for (const dcb::Record& rec : sc.b.records) {
        if (std::find(touched.begin(), touched.end(), rec.name) != touched.end()) continue;
        uint32_t s1 = 0, s2 = 0;
        uint64_t a1 = 0, a2 = 0;
        if (!before.Root(rec.name, s1, a1) || !after.Root(rec.name, s2, a2) || s1 != s2) return false;
        const uint64_t size = sc.b.Size(s1);
        if (!std::equal(sc.file.begin() + static_cast<std::ptrdiff_t>(a1), sc.file.begin() + static_cast<std::ptrdiff_t>(a1 + size),
                        patched.begin() + static_cast<std::ptrdiff_t>(a2)))
            return false;
    }
    return true;
}

// ---- the operations ----------------------------------------------------------------------------

// The batch every drift fixture gets. Returns the statuses, in order; all must be OK.
static std::vector<Status> RunOps(Patch& p) {
    std::vector<Status> st;
    st.push_back(p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(2.5)));
    st.push_back(p.OverrideField(RecG(dcb::MakeGuid(0x20)), "mass", Value::OfFloat(9.25)));
    st.push_back(p.OverrideField(Rec("ShipA"), "pos.y", Value::OfFloat(-1)));
    st.push_back(p.OverrideField(Rec("ShipA"), "engine.weight", Value::OfFloat(42)));   // through a strong pointer
    st.push_back(p.OverrideField(Rec("ShipA"), "label", Value::OfString("patched label")));
    st.push_back(p.OverrideField(Rec("ShipB"), "label", Value::OfString("world")));     // same string: reused
    st.push_back(p.OverrideField(Rec("ShipA"), "kind", Value::OfEnum("Small")));
    st.push_back(p.OverrideField(Rec("ShipA"), "counts[1]", Value::OfInt(21)));
    st.push_back(p.OverrideField(Rec("ShipA"), "parts[SubPart].grade", Value::OfInt(66)));
    st.push_back(p.OverrideField(Rec("ShipA"), "small", Value::OfInt(-5)));
    st.push_back(p.OverrideField(Rec("ShipA"), "big", Value::OfUInt(1ull << 40)));
    st.push_back(p.OverrideField(Rec("ShipA"), "flag", Value::OfBool(true)));
    st.push_back(p.OverrideField(Rec("ShipA"), "id", Value::OfGuid(ToGuid(dcb::MakeGuid(0x90)))));
    st.push_back(p.OverrideField(Rec("ShipA"), "title", Value::OfString("@new_title")));
    InstanceId np, nv, base;
    st.push_back(p.AddInstance("Part", Src("ShipA", "engine"), np));   // a copy of p1, weight 42 by now
    st.push_back(p.OverrideField(np, "weight", Value::OfFloat(11)));
    st.push_back(p.OverrideField(np, "partName", Value::OfString("fresh")));
    st.push_back(p.SetPointer(Rec("ShipB"), "engine", np));
    st.push_back(p.AppendElement(Rec("ShipA"), "counts", Value::OfInt(30)));   // copied to the pool end
    st.push_back(p.AppendElement(Rec("ShipA"), "parts", Value::OfInstance(np)));
    st.push_back(p.AddInstance("Vec2", {}, nv));
    st.push_back(p.OverrideField(nv, "x", Value::OfFloat(7)));
    st.push_back(p.AppendElement(Rec("ShipA"), "path", Value::OfInstance(nv)));
    st.push_back(p.AppendElement(Rec("ShipA"), "names", Value::OfString("n2")));
    st.push_back(p.AppendElement(Rec("ShipB"), "names", Value::OfString("only")));   // empty array
    st.push_back(p.AppendElement(Rec("ShipB"), "counts", Value::OfInt(5)));
    st.push_back(p.AppendElement(Rec("ShipB"), "counts", Value::OfInt(6)));          // now at the end: in place
    st.push_back(p.FindInstance(Src("BaseOne"), base));
    st.push_back(p.SetPointer(Rec("ShipA"), "owner", base));                         // a weak pointer
    st.push_back(p.OverrideField(Rec("ShipB"), "parts[0]", Value{}));                 // null
    return st;
}

static const std::map<std::string, std::string> kExpected = {
    { "layout", "ok" },
    { "ShipA.speed", "2.5" }, { "ShipA.mass", "5" }, { "ShipA.pos.x", "0.5" }, { "ShipA.pos.y", "-1" },
    { "ShipA.small", "-5" }, { "ShipA.big", "1099511627776" }, { "ShipA.flag", "1" }, { "ShipA.id", "144" },
    { "ShipA.label", "patched label" }, { "ShipA.kind", "Small" }, { "ShipA.title", "@new_title" },
    { "ShipA.engine", "Part(w=42,n=p1)" }, { "ShipA.owner", "Base(label=base)" },
    { "ShipA.counts", "[10,21,30]" },
    { "ShipA.parts", "[Part(w=1,n=p0),SubPart(w=11,n=s1,g=66),Part(w=11,n=fresh)]" },
    { "ShipA.path", "[Vec2(1,1.5),Vec2(2,2.5),Vec2(7,0)]" }, { "ShipA.names", "[n0,n1,n2]" },
    { "ShipB.speed", "200" }, { "ShipB.mass", "9.25" }, { "ShipB.pos.x", "0" }, { "ShipB.pos.y", "0" },
    { "ShipB.small", "2" }, { "ShipB.big", "0" }, { "ShipB.flag", "0" }, { "ShipB.id", "128" },
    { "ShipB.label", "world" }, { "ShipB.kind", "Small" }, { "ShipB.title", "@title" },
    { "ShipB.engine", "Part(w=11,n=fresh)" }, { "ShipB.owner", "null" },
    { "ShipB.counts", "[5,6]" }, { "ShipB.parts", "[null]" }, { "ShipB.path", "[]" }, { "ShipB.names", "[only]" },
};
// Strings the batch appends: "patched label", "Small" (the current value differs), "@new_title",
// "fresh", "n2", "only", each with its NUL. "world" is the field's current value: reused.
constexpr uint32_t kAppendedStrings = 14 + 6 + 11 + 6 + 3 + 5;

static bool Same(const std::map<std::string, std::string>& got, const std::map<std::string, std::string>& want) {
    bool same = true;
    for (const auto& [k, v] : want) {
        const auto it = got.find(k);
        if (it == got.end() || it->second != v) {
            std::printf("  %s: got '%s', want '%s'\n", k.c_str(), it == got.end() ? "(none)" : it->second.c_str(), v.c_str());
            same = false;
        }
    }
    return same && got.size() == want.size();
}

struct Applied { Bytes file; std::vector<sco::vfs::Splice> splices; Status emit; };

static Applied ApplyOps(const Scene& sc, const Schema& s) {
    Patch p(s);
    const std::vector<Status> st = RunOps(p);
    bool ok = true;
    for (size_t i = 0; i < st.size(); ++i)
        if (!st[i]) { ok = false; std::printf("  op %zu: %s: %s\n", i, RefusalName(st[i].category), st[i].message.c_str()); }
    CHECK(ok);
    Applied a;
    a.emit = p.Emit(a.splices);
    CHECK(a.emit.ok());
    if (!a.emit) { std::printf("  emit: %s\n", a.emit.message.c_str()); return a; }
    CHECK(ApplySplices(sc.file, a.splices, a.file).ok());
    return a;
}

// ---- tests -------------------------------------------------------------------------------------

static void TestOperations(uint32_t recordSize, bool drift) {
    const Scene sc = MakeScene(recordSize, drift);
    CHECK(sc.b.layout.badFills == 0);
    Schema s;
    CHECK(s.Parse(sc.file));
    if (!s.error.empty()) { std::printf("  scene: %s\n", s.error.c_str()); return; }
    CHECK(s.recordSize == recordSize);
    std::map<std::string, std::string> before = Observe(sc, sc.file);
    CHECK(before["ShipA.counts"] == "[10,20]" && before["ShipA.engine"] == "Part(w=2,n=p1)");

    const Applied a = ApplyOps(sc, s);
    if (!a.emit) return;
    CHECK(Same(Observe(sc, a.file), kExpected));
    CHECK(UntouchedSame(sc, a.file, { "ShipA", "ShipB" }));   // PartX, SubY, BaseOne (and PadRec) unchanged
    Schema re;
    CHECK(re.Parse(a.file) && re.recordSize == recordSize);
    CHECK(Get32(a.file, 112) == Get32(sc.file, 112) + kAppendedStrings);

    // The splices: sorted, each with its expected old bytes, the header rewritten as one overwrite.
    bool sorted = true, olds = true;
    for (size_t i = 0; i < a.splices.size(); ++i) {
        const auto& sp = a.splices[i];
        if (i && sp.at < a.splices[i - 1].at + a.splices[i - 1].removed) sorted = false;
        if (sp.removed && (!sp.old || sp.old->size() != sp.removed)) olds = false;
    }
    CHECK(sorted && olds);
    CHECK(!a.splices.empty() && a.splices[0].at == 0 && a.splices[0].removed >= 120);

    // The same splices as a sco::vfs mount serve the same bytes.
    struct Io : sco::vfs::BaseIo {
        const Bytes& b;
        uint64_t pos = 0;
        explicit Io(const Bytes& bytes) : b(bytes) {}
        bool Seek(uint64_t p) override { if (p > b.size()) return false; pos = p; return true; }
        size_t Read(void* dst, size_t n) override {
            const size_t k = static_cast<size_t>(std::min<uint64_t>(n, b.size() - pos));
            std::memcpy(dst, b.data() + pos, k);
            pos += k;
            return k;
        }
    } io(sc.file);
    std::vector<sco::vfs::Mount> mounts(1);
    mounts[0].path = "Data/Game2.dcb";
    mounts[0].source = "test";
    mounts[0].producer = sco::vfs::SpliceList{ a.splices, nullptr };
    const auto table = sco::vfs::Table::Build(std::move(mounts));
    const auto file = table->Open("data/game2.dcb", io, sc.file.size());
    CHECK(file != nullptr);
    if (file) {
        sco::vfs::Reader reader(file);
        Bytes served(static_cast<size_t>(reader.Size()));
        CHECK(reader.Read(served.data(), served.size(), io) == served.size() && served == a.file);
    }

    // Emit is repeatable, and a patch with no operations emits nothing.
    std::vector<sco::vfs::Splice> none;
    Patch empty(s);
    CHECK(empty.Emit(none).ok() && none.empty());
}

// The whole point: the same batch, tables moved, both record sizes; same meaning, other offsets.
static void TestDrift() {
    const std::pair<uint32_t, uint32_t> sizes[] = { { 32, 36 }, { 36, 32 }, { 36, 36 } };
    for (const auto& [r1, r2] : sizes) {
        const Scene a = MakeScene(r1, false), b = MakeScene(r2, true);
        Schema sa, sb;
        CHECK(sa.Parse(a.file) && sb.Parse(b.file));
        CHECK(sa.FindStruct("Ship") != sb.FindStruct("Ship"));
        CHECK(sa.header.values != sb.header.values);
        const Applied pa = ApplyOps(a, sa), pb = ApplyOps(b, sb);
        if (!pa.emit || !pb.emit) continue;
        const auto oa = Observe(a, pa.file), ob = Observe(b, pb.file);
        CHECK(oa == ob);
        CHECK(Same(ob, kExpected));
        CHECK(UntouchedSame(b, pb.file, { "ShipA", "ShipB" }));
        // Different bytes moved: the speed overwrite lands elsewhere.
        bool differ = pa.splices.size() != pb.splices.size();
        for (size_t i = 0; !differ && i < pa.splices.size(); ++i) differ = pa.splices[i].at != pb.splices[i].at;
        CHECK(differ);
    }
}

// Applies one operation to a fresh patch over the normal scene and returns its category; the
// patch must then emit nothing.
template <class Fn>
static Refusal One(const Schema& s, Fn&& fn) {
    Patch p(s, PatchOptions{ false });
    const Status st = fn(p);
    std::vector<sco::vfs::Splice> out;
    const Status e = p.Emit(out);
    if (!st && (!e || !out.empty())) {
        std::printf("  a refused operation left something to emit (%s)\n", st.message.c_str());
        return Refusal::None;
    }
    return st.category;
}

static void TestRefusals() {
    const Scene sc = MakeScene(36, false);
    Schema s;
    CHECK(s.Parse(sc.file));
    using R = Refusal;
    auto set = [&](std::string rec, std::string path, Value v) {
        return One(s, [&](Patch& p) { return p.OverrideField(Rec(rec), path, v); });
    };
    const Value one = Value::OfFloat(1);
    // Missing records and fields (design section 4, "When something is missing").
    CHECK(set("Nope", "speed", one) == R::RecordNotFound);
    CHECK(One(s, [&](Patch& p) { RecordRef r; r.guid = ToGuid(dcb::MakeGuid(0x99)); return p.OverrideField(r, "speed", one); }) == R::RecordNotFound);
    CHECK(One(s, [&](Patch& p) { RecordRef r; return p.OverrideField(r, "speed", one); }) == R::BadArgument);
    CHECK(One(s, [&](Patch& p) { RecordRef r = RecG(dcb::MakeGuid(0x99)); r.name = "ShipA"; return p.OverrideField(r, "speed", one); }) == R::None);
    CHECK(set("ShipA", "speedX", one) == R::FieldNotFound);
    CHECK(set("ShipA", "engine.nope", one) == R::FieldNotFound);
    CHECK(set("ShipA", "owner.flag", one) == R::FieldNotFound);     // weak pointers aren't followed
    CHECK(set("ShipA", "maker.x", one) == R::FieldNotFound);        // nor references
    CHECK(set("ShipA", "speed.x", one) == R::FieldNotFound);
    CHECK(set("ShipB", "engine.grade", Value::OfInt(1)) == R::None);   // SubPart's own field, through the pointer
    CHECK(set("ShipB", "parts[0].weight", one) == R::None);
    CHECK(set("ShipA", "counts[2]", Value::OfInt(1)) == R::IndexOutOfRange);
    CHECK(set("ShipA", "counts[99999999999999999999]", Value::OfInt(1)) == R::IndexOutOfRange);
    CHECK(set("ShipA", "parts[Vec2].x", one) == R::IndexOutOfRange);
    CHECK(set("ShipA", "parts[Nope].weight", one) == R::StructNotFound);
    CHECK(set("ShipA", "parts[Part].weight", one) == R::None);
    CHECK(set("ShipA", "counts[Part]", one) == R::TypeMismatch);
    CHECK(set("ShipA", "path[Vec2].x", one) == R::None);
    for (const char* bad : { "", "speed..x", "counts[", "counts[]", "[1]", "counts[1]x", ".speed", "speed." })
        CHECK(set("ShipA", bad, one) == R::BadArgument || set("ShipA", bad, one) == R::TypeMismatch);
    CHECK(set("ShipA", "counts[", one) == R::BadArgument && set("ShipA", "speed..x", one) == R::BadArgument);
    // Values that don't fit: refused, never written.
    CHECK(set("ShipA", "speed", Value::OfString("fast")) == R::TypeMismatch);
    CHECK(set("ShipA", "label", one) == R::TypeMismatch);
    CHECK(set("ShipA", "flag", Value::OfInt(1)) == R::TypeMismatch);
    CHECK(set("ShipA", "small", one) == R::TypeMismatch);
    CHECK(set("ShipA", "id", Value::OfString("x")) == R::TypeMismatch);
    CHECK(set("ShipA", "pos", one) == R::TypeMismatch);              // a struct
    CHECK(set("ShipA", "counts", Value::OfInt(1)) == R::TypeMismatch);   // an array
    CHECK(set("ShipA", "speed[0]", one) == R::TypeMismatch);
    CHECK(set("ShipA", "engine", one) == R::TypeMismatch);
    CHECK(set("ShipA", "small", Value::OfInt(300)) == R::ValueOutOfRange);
    CHECK(set("ShipA", "small", Value::OfInt(-129)) == R::ValueOutOfRange);
    CHECK(set("ShipA", "small", Value::OfUInt(128)) == R::ValueOutOfRange);
    CHECK(set("ShipA", "small", Value::OfInt(-128)) == R::None);
    CHECK(set("ShipA", "big", Value::OfInt(-1)) == R::ValueOutOfRange);
    CHECK(set("ShipA", "speed", Value::OfFloat(1e300)) == R::ValueOutOfRange);
    CHECK(set("ShipA", "speed", Value::OfInt(3)) == R::None);
    CHECK(set("ShipA", "kind", Value::OfEnum("Huge")) == R::UnknownEnumOption);
    CHECK(set("ShipA", "kind", Value::OfEnum("Large")) == R::None);
    CHECK(set("ShipA", "label", Value::OfString(std::string("a\0b", 3))) == R::BadArgument);
    CHECK(set("ShipA", "maker", Value::OfGuid(ToGuid(dcb::MakeGuid(0x10)))) == R::Unsupported);   // research R1
    // Pointers.
    CHECK(One(s, [&](Patch& p) {
              InstanceId v;
              if (!p.FindInstance(Src("ShipA", "path[0]"), v)) return Status{};
              return p.SetPointer(Rec("ShipA"), "engine", v);   // a Vec2 isn't a Part
          }) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) {
              InstanceId v;
              if (!p.FindInstance(Src("SubY"), v)) return Status{};
              return p.SetPointer(Rec("ShipA"), "engine", v);   // a SubPart is a Part
          }) == R::None);
    CHECK(One(s, [&](Patch& p) { return p.SetPointer(Rec("ShipA"), "speed", InstanceId{ 0, 0 }); }) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) { return p.SetPointer(Rec("ShipA"), "engine", InstanceId{ sc.S("Part"), 999 }); }) == R::BadArgument);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.FindInstance(Src("ShipA", "pos"), x); }) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.FindInstance(Src("ShipB", "owner"), x); }) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.FindInstance(Src("ShipA", "parts[2]"), x); }) == R::IndexOutOfRange);
    // A failed AddInstance: everything depending on it is refused.
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Nope", {}, x); }) == R::StructNotFound);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Orphan", {}, x); }) == R::Unsupported);   // no data block
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Part", Src("ShipA", "pos"), x); }) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Part", Src("Nope"), x); }) == R::RecordNotFound);
    CHECK(One(s, [&](Patch& p) { InstanceId x; InstanceSource src; src.field = "x"; return p.AddInstance("Part", src, x); }) == R::BadArgument);
    CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Empty", {}, x); }) == R::None);   // zero-size instances
    {
        Patch p(s, PatchOptions{ false });
        InstanceId bad;
        CHECK(p.AddInstance("Nope", {}, bad).category == R::StructNotFound && !bad.valid());
        CHECK(p.SetPointer(Rec("ShipA"), "engine", bad).category == R::DependencyFailed);
        CHECK(p.AppendElement(Rec("ShipA"), "parts", Value::OfInstance(bad)).category == R::DependencyFailed);
        CHECK(p.OverrideField(bad, "weight", one).category == R::DependencyFailed);
        CHECK(p.Reports().size() == 4 && !p.Reports()[1].status);
    }
    // Appends.
    auto append = [&](std::string rec, std::string path, Value v) {
        return One(s, [&](Patch& p) { return p.AppendElement(Rec(rec), path, v); });
    };
    CHECK(append("ShipA", "speed", one) == R::TypeMismatch);
    CHECK(append("ShipA", "counts", one) == R::TypeMismatch);
    CHECK(append("ShipA", "counts", Value::OfInt(1ll << 40)) == R::ValueOutOfRange);
    CHECK(append("ShipA", "names", one) == R::TypeMismatch);
    CHECK(append("ShipA", "path", one) == R::TypeMismatch);
    CHECK(One(s, [&](Patch& p) {
              InstanceId x;
              if (!p.FindInstance(Src("PartX"), x)) return Status{};
              return p.AppendElement(Rec("ShipA"), "path", Value::OfInstance(x));   // a Part into Vec2[]
          }) == R::TypeMismatch);
    CHECK(append("ShipA", "parts", Value{}) == R::None);   // a null element
}

// Per-batch atomicity: one refused operation and the batch emits nothing, unless non-atomic.
static void TestAtomic() {
    const Scene sc = MakeScene(32, false);
    Schema s;
    CHECK(s.Parse(sc.file));
    std::vector<sco::vfs::Splice> out;
    {
        Patch p(s);
        CHECK(p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(3)).ok());
        CHECK(p.OverrideField(Rec("ShipA"), "nope", Value::OfFloat(3)).category == Refusal::FieldNotFound);
        CHECK(p.OverrideField(Rec("ShipB"), "speed", Value::OfFloat(4)).ok());
        const Status e = p.Emit(out);
        CHECK(e.category == Refusal::FieldNotFound && out.empty());
    }
    {
        Patch p(s, PatchOptions{ false });
        CHECK(p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(3)).ok());
        CHECK(!p.OverrideField(Rec("ShipA"), "nope", Value::OfFloat(3)));
        CHECK(!p.AppendElement(Rec("ShipA"), "names", Value::OfFloat(1)));   // refused before any string is added
        CHECK(p.Emit(out).ok());
        Bytes f;
        CHECK(ApplySplices(sc.file, out, f).ok());
        const auto o = Observe(sc, f);
        CHECK(o.at("ShipA.speed") == "3" && o.at("ShipA.names") == "[n0,n1]");
        CHECK(Get32(f, 112) == Get32(sc.file, 112) && f.size() == sc.file.size());
        CHECK(p.Reports().size() == 3 && p.Reports()[0].status.ok() && !p.Reports()[1].status);
    }
}

// Opaque structs and refused layouts; splices that don't match the base.
static void TestOpaqueAndLayout() {
    {
        Scene sc = MakeScene(36, false);
        sc.b.structs[sc.S("Part")].props.push_back({ "mystery", 0x77, 0, dcb::kSingle, 4 });
        sc.file = sc.b.Build();
        Schema s;
        CHECK(s.Parse(sc.file) && s.structInfo[sc.S("Part")].opaque);
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("PartX"), "weight", Value::OfFloat(1)); }) == Refusal::Opaque);
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("ShipA"), "engine.weight", Value::OfFloat(1)); }) == Refusal::Opaque);
        CHECK(One(s, [&](Patch& p) { InstanceId x; return p.AddInstance("Part", {}, x); }) == Refusal::Opaque);
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(1)); }) == Refusal::None);
    }
    {
        const Scene sc = MakeScene(36, false);
        Bytes cut(sc.file.begin(), sc.file.end() - 1);
        Schema s;
        CHECK(!s.Parse(cut));
        Patch p(s);
        InstanceId x;
        CHECK(p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(1)).category == Refusal::Layout);
        CHECK(p.AddInstance("Part", {}, x).category == Refusal::Layout);
        CHECK(p.FindInstance(Src("ShipA"), x).category == Refusal::Layout);
        std::vector<sco::vfs::Splice> out;
        CHECK(p.Emit(out).category == Refusal::Layout && out.empty());
    }
    {   // Array ranges and pointers that point outside their pools: Corrupt, nothing read past.
        Scene sc = MakeScene(36, false);
        sc.b.fills.push_back({ sc.S("Ship"), 0, "counts", Pair(2, 0xFFFFFFF0u) });
        sc.b.fills.push_back({ sc.S("Ship"), 0, "engine", Pair(sc.S("Part"), 77) });
        sc.b.fills.push_back({ sc.S("Ship"), 1, "engine", Pair(99, 0) });
        sc.file = sc.b.Build();
        Schema s;
        CHECK(s.Parse(sc.file));
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("ShipA"), "counts[0]", Value::OfInt(1)); }) == Refusal::Corrupt);
        CHECK(One(s, [&](Patch& p) { return p.AppendElement(Rec("ShipA"), "counts", Value::OfInt(1)); }) == Refusal::Corrupt);
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("ShipA"), "engine.weight", Value::OfFloat(1)); }) == Refusal::Corrupt);
        CHECK(One(s, [&](Patch& p) { return p.OverrideField(Rec("ShipB"), "engine.weight", Value::OfFloat(1)); }) == Refusal::Corrupt);
        CHECK(One(s, [&](Patch& p) { return p.SetPointer(Rec("ShipA"), "engine", InstanceId{ sc.S("Part"), 0 }); }) == Refusal::None);
    }
    {   // ApplySplices checks the expected old bytes and the splice order.
        const Scene sc = MakeScene(32, false);
        Schema s;
        CHECK(s.Parse(sc.file));
        Patch p(s);
        CHECK(p.OverrideField(Rec("ShipA"), "speed", Value::OfFloat(7)).ok());
        std::vector<sco::vfs::Splice> out;
        CHECK(p.Emit(out).ok() && out.size() == 1 && out[0].removed == 4);
        Bytes other = sc.file, f;
        other[out[0].at] ^= 0xFF;
        CHECK(ApplySplices(other, out, f).category == Refusal::BadArgument);
        out.push_back(out[0]);
        CHECK(ApplySplices(sc.file, out, f).category == Refusal::BadArgument);
    }
}

// Every truncation, and seeded corruptions: the batch never reads past the input, and either
// refuses with a category or emits splices whose result re-parses.
static void TestFuzz(uint32_t recordSize) {
    const Scene sc = MakeScene(recordSize, false);
    int layout = 0, wrong = 0;
    for (size_t n = 0; n < sc.file.size(); n += 7) {
        const Bytes cut(sc.file.begin(), sc.file.begin() + static_cast<std::ptrdiff_t>(n));
        Schema s;
        if (s.Parse(cut)) continue;
        Patch p(s);
        for (const Status& st : RunOps(p)) wrong += st.category != Refusal::Layout;
        std::vector<sco::vfs::Splice> out;
        wrong += p.Emit(out).category != Refusal::Layout || !out.empty();
        ++layout;
    }
    CHECK(wrong == 0 && layout > 0);

    std::mt19937 rng(20261009u + recordSize);
    int parsed = 0, emitted = 0, refusedOps = 0, bad = 0;
    for (int iter = 0; iter < 1500; ++iter) {
        Bytes f = sc.file;
        const uint32_t n = 1 + rng() % 6;
        for (uint32_t k = 0; k < n; ++k) {
            // Mostly the data section and pools, where the patcher reads what the parser doesn't check.
            const uint64_t at = rng() % 4 ? sc.b.layout.pools[0] + rng() % (f.size() - sc.b.layout.pools[0]) : rng() % f.size();
            f[at] = static_cast<uint8_t>(rng() % 3 == 0 ? 0xFF : rng());
        }
        Schema s;
        if (!s.Parse(f)) continue;
        ++parsed;
        Patch p(s, PatchOptions{ false });
        for (const Status& st : RunOps(p)) {
            if (!st) { ++refusedOps; if (st.message.empty()) ++bad; }
        }
        std::vector<sco::vfs::Splice> out;
        const Status e = p.Emit(out);
        if (!e) { if (!out.empty() || e.message.empty()) ++bad; continue; }
        ++emitted;
        Bytes g;
        Schema re;
        if (!ApplySplices(f, out, g) || !re.Parse(g)) ++bad;
    }
    CHECK(bad == 0);
    CHECK(parsed > 0 && emitted > 0 && refusedOps > 0);
    std::printf("  fuzz, %u-byte records: %d truncations refused (layout), %d corruptions parsed, %d emitted, %d operations refused\n",
                recordSize, layout, parsed, emitted, refusedOps);
}

int main() {
    TestOperations(32, false);
    TestOperations(36, false);
    TestOperations(32, true);
    TestOperations(36, true);
    TestDrift();
    TestRefusals();
    TestAtomic();
    TestOpaqueAndLayout();
    TestFuzz(32);
    TestFuzz(36);
    std::printf("sco-core datacore patch tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
