#pragma once
// Synthetic DataCore (.dcb) files for tests (docs/design/vfs-datacore.md section 7). This writer is
// independent of src/datacore/: it has its own size rules and layout code, so a parser bug can't
// hide behind shared code. It writes the section 2 layout: header, definition tables, records of
// `recordSize` bytes (32, 36, or anything else to test refusals), value pools in file order, the two
// string pools and one data block per mapping.
//
//   dcb::Builder b = dcb::Fixture(36);   // a small valid schema with every data type
//   std::vector<uint8_t> file = b.Build();
//   b.layout.records ...                  // where each table landed, for corruption tests
#include <array>
#include <cstdint>
#include <cstring>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace dcb {

namespace t {
constexpr uint16_t Bool = 0x1, Int8 = 0x2, Int16 = 0x3, Int32 = 0x4, Int64 = 0x5, UInt8 = 0x6, UInt16 = 0x7,
                   UInt32 = 0x8, UInt64 = 0x9, String = 0xA, Float = 0xB, Double = 0xC, Locale = 0xD, Guid = 0xE,
                   Enum = 0xF, Class = 0x10, Strong = 0x110, Weak = 0x210, Reference = 0x310;
}  // namespace t

// Array kinds (PropertyDef::conversion): 0 single, 1-3 arrays (8 bytes inline: count, first index).
constexpr uint16_t kSingle = 0, kArray1 = 1, kArray2 = 2, kArray3 = 3;

struct Prop {
    std::string name;
    uint16_t    type = 0;
    uint16_t    typeIndex = 0;    // struct (Class, Strong, Weak) or enum (Enum)
    uint16_t    conversion = kSingle;
    uint32_t    rawSize = 0;      // inline bytes for a type code the builder doesn't know
};
struct Struct { std::string name; int32_t parent = -1; std::vector<Prop> props; };
struct Enum { std::string name; std::vector<std::string> options; };
struct Mapping { uint32_t structIndex = 0; uint32_t count = 0; };
using Guid = std::array<uint8_t, 16>;
struct Record {
    std::string name, fileName;
    uint32_t    structIndex = 0;
    Guid        id{};
    uint16_t    instance = 0;
    uint32_t    unknown = 0;      // written at +8 when recordSize > 32
};

// Value pools, in file order.
enum Pool { Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64, Bool, Float, Double, GuidPool, String,
            Locale, EnumPool, Strong, Weak, Reference, kPoolCount };
inline constexpr uint32_t kPoolEntry[kPoolCount] = { 1, 2, 4, 8, 1, 2, 4, 8, 1, 4, 8, 16, 4, 4, 4, 8, 8, 20 };
// The header's 18 value counts are in a different order from the pools.
inline constexpr Pool kHeaderOrder[18] = { Bool,   Int8,  Int16,  Int32,  Int64,    UInt8,  UInt16, UInt32,   UInt64,
                                           Float,  Double, GuidPool, String, Locale, EnumPool, Strong, Weak, Reference };

inline Guid MakeGuid(uint8_t seed) {
    Guid g{};
    for (size_t i = 0; i < g.size(); ++i) g[i] = static_cast<uint8_t>(seed + i);
    return g;
}

class Builder {
public:
    uint32_t recordSize = 36;
    uint32_t version = 8;
    std::vector<Struct>  structs;
    std::vector<Enum>    enums;
    std::vector<Mapping> mappings;
    std::vector<Record>  records;
    std::array<std::vector<uint8_t>, kPoolCount> pools;

    // Instance contents, written by Build over the filler bytes: `field` (a property path, inherited
    // properties included, "a.b" into inline structs) of instance `instance` of struct `structIndex`,
    // counting over all of that struct's mappings in order. A fill that names nothing is counted in
    // layout.badFills.
    struct Fill { uint32_t structIndex = 0; uint32_t instance = 0; std::string field; std::vector<uint8_t> bytes; };
    std::vector<Fill> fills;

    struct Layout {
        uint64_t structs = 0, properties = 0, enums = 0, mappings = 0, records = 0;
        std::array<uint64_t, kPoolCount> pools{};
        uint64_t enumOptions = 0, valueStrings = 0, nameStrings = 0, data = 0;
        std::vector<uint64_t> blocks;   // per mapping
        int badFills = 0;
    } layout;                           // filled by Build

    static constexpr uint64_t kNoField = ~0ull;
    // Offset of a field path inside an instance of struct s (section 2 rules: parent first, packed),
    // and its Prop; kNoField when the path names nothing.
    uint64_t FieldOffset(uint32_t s, std::string_view path, Prop* found = nullptr) const {
        const size_t dot = path.find('.');
        const std::string_view head = path.substr(0, dot);
        std::vector<uint32_t> chain;
        for (int64_t c = s; c >= 0; c = structs[static_cast<size_t>(c)].parent) chain.insert(chain.begin(), static_cast<uint32_t>(c));
        uint64_t off = 0;
        for (uint32_t c : chain)
            for (const Prop& p : structs[c].props) {
                if (p.name == head) {
                    if (dot == std::string_view::npos) {
                        if (found) *found = p;
                        return off;
                    }
                    if (p.conversion != kSingle || p.type != t::Class) return kNoField;
                    const uint64_t in = FieldOffset(p.typeIndex, path.substr(dot + 1), found);
                    return in == kNoField ? kNoField : off + in;
                }
                off += p.conversion != kSingle ? 8 : p.type == t::Class ? Size(p.typeIndex) : FieldBytes(p);
            }
        return kNoField;
    }

    // Interned strings: the offset of `s` in its pool, appended on first use.
    uint32_t Name(std::string_view s) { return Intern(names_, nameAt_, s); }
    uint32_t Value(std::string_view s) { return Intern(values_, valueAt_, s); }

    void Push(Pool p, const void* bytes) {
        const auto* b = static_cast<const uint8_t*>(bytes);
        pools[p].insert(pools[p].end(), b, b + kPoolEntry[p]);
    }
    void PushU32(Pool p, uint32_t v) { uint8_t b[8] = {}; Le(b, v, 4); Push(p, b); }
    void PushU64(Pool p, uint64_t v) { uint8_t b[8] = {}; Le(b, v, 8); Push(p, b); }
    void PushString(std::string_view s) { PushU32(String, Value(s)); }
    void PushPointer(Pool p, uint32_t structIndex, uint32_t instance) {
        uint8_t b[8];
        Le(b, structIndex, 4);
        Le(b + 4, instance, 4);
        Push(p, b);
    }
    void PushReference(uint32_t item, const Guid& id) {
        uint8_t b[20];
        Le(b, item, 4);
        std::memcpy(b + 4, id.data(), 16);
        Push(Reference, b);
    }

    // Instance size by the section 2 rules: parent first, packed; arrays 8; inline class its size.
    uint64_t Size(uint32_t s) const {
        const Struct& d = structs[s];
        uint64_t size = d.parent >= 0 ? Size(static_cast<uint32_t>(d.parent)) : 0;
        for (const Prop& p : d.props) {
            if (p.conversion != kSingle) size += 8;
            else if (p.type == t::Class) size += Size(p.typeIndex);
            else size += FieldBytes(p);
        }
        return size;
    }

    std::vector<uint8_t> Build() {
        // Names first, in table order, so the pools are deterministic.
        std::vector<uint32_t> structName, propName, enumName, optionName, recName, recFile;
        for (const Struct& s : structs) structName.push_back(Name(s.name));
        for (const Struct& s : structs)
            for (const Prop& p : s.props) propName.push_back(Name(p.name));
        for (const Enum& e : enums) enumName.push_back(Name(e.name));
        for (const Enum& e : enums)
            for (const std::string& o : e.options) optionName.push_back(Name(o));
        for (const Record& r : records) {
            recName.push_back(Name(r.name));
            recFile.push_back(Value(r.fileName));
        }
        uint32_t propCount = 0;
        for (const Struct& s : structs) propCount += static_cast<uint32_t>(s.props.size());

        std::vector<uint8_t> f(120, 0);
        auto at = [&](uint64_t off, uint64_t v, int n) { Le(f.data() + off, v, n); };
        at(4, version, 4);
        at(16, structs.size(), 4);
        at(20, propCount, 4);
        at(24, enums.size(), 4);
        at(28, mappings.size(), 4);
        at(32, records.size(), 4);
        for (int i = 0; i < 18; ++i) at(36 + 4 * i, pools[kHeaderOrder[i]].size() / kPoolEntry[kHeaderOrder[i]], 4);
        at(108, optionName.size(), 4);
        at(112, values_.size(), 4);
        at(116, names_.size(), 4);

        layout = Layout{};
        layout.structs = f.size();
        uint32_t first = 0;
        for (size_t i = 0; i < structs.size(); ++i) {
            uint8_t e[16] = {};
            Le(e, structName[i], 4);
            Le(e + 4, static_cast<uint32_t>(structs[i].parent), 4);
            Le(e + 8, structs[i].props.size(), 2);
            Le(e + 10, first, 2);
            Append(f, e, 16);
            first += static_cast<uint32_t>(structs[i].props.size());
        }
        layout.properties = f.size();
        size_t k = 0;
        for (const Struct& s : structs)
            for (const Prop& p : s.props) {
                uint8_t e[12] = {};
                Le(e, propName[k++], 4);
                Le(e + 4, p.typeIndex, 2);
                Le(e + 6, p.type, 2);
                Le(e + 8, p.conversion, 2);
                Append(f, e, 12);
            }
        layout.enums = f.size();
        first = 0;
        for (size_t i = 0; i < enums.size(); ++i) {
            uint8_t e[8] = {};
            Le(e, enumName[i], 4);
            Le(e + 4, enums[i].options.size(), 2);
            Le(e + 6, first, 2);
            Append(f, e, 8);
            first += static_cast<uint32_t>(enums[i].options.size());
        }
        layout.mappings = f.size();
        for (const Mapping& m : mappings) {
            uint8_t e[8];
            Le(e, m.count, 4);
            Le(e + 4, m.structIndex, 4);
            Append(f, e, 8);
        }
        layout.records = f.size();
        for (size_t i = 0; i < records.size(); ++i) {
            const Record& r = records[i];
            std::vector<uint8_t> e(recordSize, 0);
            Le(e.data(), recName[i], 4);
            Le(e.data() + 4, recFile[i], 4);
            if (recordSize > 32) Le(e.data() + 8, r.unknown, 4);
            uint8_t* tail = e.data() + recordSize - 24;
            Le(tail, r.structIndex, 4);
            std::memcpy(tail + 4, r.id.data(), 16);
            Le(tail + 20, r.instance, 2);
            Le(tail + 22, Size(r.structIndex), 2);
            Append(f, e.data(), e.size());
        }
        for (int p = 0; p < kPoolCount; ++p) {
            layout.pools[static_cast<size_t>(p)] = f.size();
            Append(f, pools[static_cast<size_t>(p)].data(), pools[static_cast<size_t>(p)].size());
        }
        layout.enumOptions = f.size();
        for (uint32_t o : optionName) {
            uint8_t e[4];
            Le(e, o, 4);
            Append(f, e, 4);
        }
        layout.valueStrings = f.size();
        Append(f, reinterpret_cast<const uint8_t*>(values_.data()), values_.size());
        layout.nameStrings = f.size();
        Append(f, reinterpret_cast<const uint8_t*>(names_.data()), names_.size());
        layout.data = f.size();
        for (const Mapping& m : mappings) {
            layout.blocks.push_back(f.size());
            const uint64_t bytes = m.count * Size(m.structIndex);
            // Recognizable, non-zero instance bytes: block index and position.
            for (uint64_t b = 0; b < bytes; ++b) f.push_back(static_cast<uint8_t>(layout.blocks.size() * 16 + b));
        }
        for (const Fill& fill : fills) {
            uint64_t inst = kNoField, i = fill.instance;
            for (size_t m = 0; m < mappings.size() && inst == kNoField; ++m) {
                if (mappings[m].structIndex != fill.structIndex) continue;
                if (i < mappings[m].count) inst = layout.blocks[m] + i * Size(fill.structIndex);
                else i -= mappings[m].count;
            }
            const uint64_t off = inst == kNoField ? kNoField : FieldOffset(fill.structIndex, fill.field);
            if (off == kNoField || inst + off + fill.bytes.size() > f.size()) { ++layout.badFills; continue; }
            std::memcpy(f.data() + inst + off, fill.bytes.data(), fill.bytes.size());
        }
        return f;
    }

private:
    std::string names_, values_;
    std::map<std::string, uint32_t, std::less<>> nameAt_, valueAt_;

    static uint32_t Intern(std::string& pool, std::map<std::string, uint32_t, std::less<>>& at, std::string_view s) {
        if (const auto it = at.find(s); it != at.end()) return it->second;
        const auto off = static_cast<uint32_t>(pool.size());
        pool.append(s);
        pool.push_back('\0');
        at.emplace(std::string(s), off);
        return off;
    }
    static void Le(uint8_t* p, uint64_t v, int n) {
        for (int i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
    }
    static void Append(std::vector<uint8_t>& f, const uint8_t* b, size_t n) {
        if (n) f.insert(f.end(), b, b + n);
    }
    static uint32_t FieldBytes(const Prop& p) {
        switch (p.type) {
        case t::Bool: case t::Int8: case t::UInt8: return 1;
        case t::Int16: case t::UInt16: return 2;
        case t::Int32: case t::UInt32: case t::Float: case t::String: case t::Locale: case t::Enum: return 4;
        case t::Int64: case t::UInt64: case t::Double: case t::Strong: case t::Weak: return 8;
        case t::Guid: return 16;
        case t::Reference: return 20;
        default: return p.rawSize;
        }
    }
};

// Struct indices of Fixture().
enum FixtureStruct : uint32_t { kVec2, kBase, kShip, kPart, kEmpty };

// A small valid file: inheritance (Ship derives from Base), every data type, an inline struct
// (Vec2), strong and weak pointers, a reference, an enum, the three array kinds, strings in both
// pools, a struct with no instances (Empty), and five records with GUIDs. Five records keep the
// 32/36/40-byte candidates 20 bytes apart, so no single-count corruption can match another size.
inline Builder Fixture(uint32_t recordSize = 36) {
    Builder b;
    b.recordSize = recordSize;
    b.structs = {
        { "Vec2", -1, { { "x", t::Float }, { "y", t::Float } } },
        { "Base", -1, { { "flag", t::Bool }, { "label", t::String }, { "kind", t::Enum, 0 } } },
        { "Ship", kBase,
          { { "i8", t::Int8 }, { "i16", t::Int16 }, { "i32", t::Int32 }, { "i64", t::Int64 },
            { "u8", t::UInt8 }, { "u16", t::UInt16 }, { "u32", t::UInt32 }, { "u64", t::UInt64 },
            { "speed", t::Float }, { "mass", t::Double }, { "id", t::Guid }, { "title", t::Locale },
            { "pos", t::Class, kVec2 }, { "engine", t::Strong, kPart }, { "owner", t::Weak, kBase },
            { "maker", t::Reference }, { "counts", t::Int32, 0, kArray1 }, { "parts", t::Strong, kPart, kArray2 },
            { "path", t::Class, kVec2, kArray3 } } },
        { "Part", -1, { { "weight", t::Float }, { "partName", t::String } } },
        { "Empty", -1, {} },
    };
    b.enums = { { "Kind", { "Small", "Large" } } };
    b.mappings = { { kVec2, 3 }, { kBase, 1 }, { kShip, 2 }, { kPart, 4 }, { kEmpty, 0 } };
    b.records = {
        { "ShipA", "libs/foundry/records/test/ships.xml", kShip, MakeGuid(0x10), 0, 0x1111 },
        { "ShipB", "libs/foundry/records/test/ships.xml", kShip, MakeGuid(0x20), 1, 0x1111 },
        { "BaseOne", "libs/foundry/records/test/base_one.xml", kBase, MakeGuid(0x30), 0, 0x2222 },
        { "PartX", "libs/foundry/records/test/part_x.xml", kPart, MakeGuid(0x40), 0, 0x3333 },
        { "PartY", "libs/foundry/records/test/part_y.xml", kPart, MakeGuid(0x50), 2, 0x4444 },
    };
    uint8_t one[16] = { 1 };
    for (Pool p : { Int8, Int16, Int64, UInt8, UInt16, UInt32, UInt64, Bool, Double }) b.Push(p, one);
    for (uint32_t v : { 1u, 2u, 3u }) b.PushU32(Int32, v);
    float f = 1.5f;
    uint32_t fbits;
    std::memcpy(&fbits, &f, 4);
    b.PushU32(Float, fbits);
    b.Push(GuidPool, MakeGuid(0x60).data());
    b.PushString("hello");
    b.PushString("world");
    b.PushU32(Locale, b.Value("@test_title"));
    b.PushU32(EnumPool, b.Value("Large"));
    b.PushPointer(Strong, kPart, 0);
    b.PushPointer(Strong, kPart, 1);
    b.PushPointer(Weak, kBase, 0);
    b.PushReference(0, MakeGuid(0x10));
    return b;
}

// Fixture() with readable values in Ship and Part (the plain one leaves filler bytes there): labels,
// the Kind enum, speeds, null owners, empty arrays, ShipA's engine Part[1] and ShipB's Part[0]
// (PartX's root), Part weights 1-4 and names p0-p3. For tests that apply overrides through
// pointers and arrays (test_datacore_service, test_lua).
inline Builder ValuedFixture(uint32_t recordSize = 36) {
    Builder b = Fixture(recordSize);
    auto le = [](uint64_t v, int n) {
        std::vector<uint8_t> x(static_cast<size_t>(n));
        for (int i = 0; i < n; ++i) x[static_cast<size_t>(i)] = static_cast<uint8_t>(v >> (8 * i));
        return x;
    };
    auto f32 = [&](float v) { uint32_t u; std::memcpy(&u, &v, 4); return le(u, 4); };
    auto pair = [&](uint32_t a, uint32_t c) { std::vector<uint8_t> x = le(a, 4), y = le(c, 4); x.insert(x.end(), y.begin(), y.end()); return x; };
    auto fill = [&](uint32_t st, uint32_t i, std::string f, std::vector<uint8_t> v) { b.fills.push_back({ st, i, std::move(f), std::move(v) }); };
    for (uint32_t s = 0; s < 2; ++s) {
        fill(kShip, s, "label", le(b.Value(s ? "world" : "hello"), 4));
        fill(kShip, s, "kind", le(b.Value("Large"), 4));
        fill(kShip, s, "title", le(b.Value("@t"), 4));
        fill(kShip, s, "speed", f32(100.0f + static_cast<float>(s)));
        fill(kShip, s, "owner", pair(0xFFFFFFFFu, 0xFFFFFFFFu));
        fill(kShip, s, "counts", pair(0, 0));
        fill(kShip, s, "parts", pair(0, 0));
        fill(kShip, s, "path", pair(0, 0));
    }
    fill(kShip, 0, "engine", pair(kPart, 1));
    fill(kShip, 1, "engine", pair(kPart, 0));
    for (uint32_t p = 0; p < 4; ++p) {
        fill(kPart, p, "weight", f32(1.0f + static_cast<float>(p)));
        fill(kPart, p, "partName", le(b.Value("p" + std::to_string(p)), 4));
    }
    return b;
}

}  // namespace dcb
