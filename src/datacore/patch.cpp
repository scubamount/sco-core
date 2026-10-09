// sco::datacore patcher (sco/datacore.h): semantic overrides over a parsed Schema, turned into
// base-offset splices for sco::vfs. docs/design/vfs-datacore.md section 4: "API", "How each
// operation becomes splices", "When something is missing".
//
// The patch is an overlay on the base file: byte overwrites in base offsets, plus one append region
// per pool, per mapping block and for the value-string pool, each inserted at the end of what it
// extends. Reads go through the overlay, so later operations see earlier ones. Every operation
// resolves and checks everything first and only then writes, so a refused one changes nothing.
#include "internal.h"
#include "sco/datacore.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstring>
#include <map>
#include <type_traits>
#include <unordered_map>

namespace sco::datacore {

using detail::Fmt;
using detail::R32;
using detail::SatAdd;
using detail::SatMul;

namespace {

using ull = unsigned long long;
using Bytes = std::vector<uint8_t>;
constexpr uint64_t kBase = ~0ull;            // Loc::region of a base-file offset
constexpr uint64_t kNoIndex = ~0ull;         // Node::index of an inline struct member
constexpr uint32_t kNull = 0xFFFFFFFFu;      // a null pointer: struct and instance all ones (as 4.10.193 stores it)
constexpr uint64_t kU32 = 0xFFFFFFFFull;
constexpr uint64_t kBlockRank = 1ull << 32;  // append-region ranks: table index for pools, this + mapping for blocks
constexpr int kMaxDepth = 256;
constexpr size_t kKinds = static_cast<size_t>(ValueKind::Count);

struct Loc {
    uint64_t region = kBase;   // kBase, or the rank of the append region
    uint64_t off = 0;
};
Loc At(Loc l, uint64_t d) { return { l.region, SatAdd(l.off, d) }; }

void Put(uint8_t* p, uint64_t v, uint32_t n) {
    for (uint32_t i = 0; i < n; ++i) p[i] = static_cast<uint8_t>(v >> (8 * i));
}

// The value pool a data type's array elements (and pointer targets' entries) live in.
struct PoolType { ValueKind kind; uint32_t entry; };
bool PoolOf(uint16_t t, PoolType& out) {
    switch (t) {
    case type::kBool: out = { ValueKind::Bool, 1 }; return true;
    case type::kInt8: out = { ValueKind::Int8, 1 }; return true;
    case type::kInt16: out = { ValueKind::Int16, 2 }; return true;
    case type::kInt32: out = { ValueKind::Int32, 4 }; return true;
    case type::kInt64: out = { ValueKind::Int64, 8 }; return true;
    case type::kUInt8: out = { ValueKind::UInt8, 1 }; return true;
    case type::kUInt16: out = { ValueKind::UInt16, 2 }; return true;
    case type::kUInt32: out = { ValueKind::UInt32, 4 }; return true;
    case type::kUInt64: out = { ValueKind::UInt64, 8 }; return true;
    case type::kFloat: out = { ValueKind::Float, 4 }; return true;
    case type::kDouble: out = { ValueKind::Double, 8 }; return true;
    case type::kGuid: out = { ValueKind::Guid, 16 }; return true;
    case type::kString: out = { ValueKind::String, 4 }; return true;
    case type::kLocale: out = { ValueKind::Locale, 4 }; return true;
    case type::kEnum: out = { ValueKind::Enum, 4 }; return true;
    case type::kStrongPointer: out = { ValueKind::Strong, 8 }; return true;
    case type::kWeakPointer: out = { ValueKind::Weak, 8 }; return true;
    case type::kReference: out = { ValueKind::Reference, 20 }; return true;
    default: return false;
    }
}

const char* KindName(Value::Kind k) {
    switch (k) {
    case Value::Kind::Null: return "null";
    case Value::Kind::Bool: return "bool";
    case Value::Kind::Int: return "integer";
    case Value::Kind::UInt: return "unsigned integer";
    case Value::Kind::Float: return "float";
    case Value::Kind::String: return "string";
    case Value::Kind::Guid: return "guid";
    case Value::Kind::Enum: return "enum option";
    case Value::Kind::Instance: return "instance";
    }
    return "?";
}

std::string Q(std::string_view s) { return "\"" + std::string(s) + "\""; }
Status Refuse(Refusal r, std::string message) { return { r, std::move(message) }; }

}  // namespace

const char* RefusalName(Refusal r) {
    switch (r) {
    case Refusal::None: return "none";
    case Refusal::Layout: return "layout";
    case Refusal::BadArgument: return "bad argument";
    case Refusal::RecordNotFound: return "record not found";
    case Refusal::StructNotFound: return "struct not found";
    case Refusal::FieldNotFound: return "field not found";
    case Refusal::IndexOutOfRange: return "index out of range";
    case Refusal::TypeMismatch: return "type mismatch";
    case Refusal::ValueOutOfRange: return "value out of range";
    case Refusal::UnknownEnumOption: return "unknown enum option";
    case Refusal::Opaque: return "opaque struct";
    case Refusal::Unsupported: return "unsupported";
    case Refusal::DependencyFailed: return "dependency failed";
    case Refusal::Corrupt: return "corrupt data";
    case Refusal::Limit: return "limit";
    case Refusal::Revalidation: return "re-validation";
    }
    return "?";
}

struct Patch::Impl {
    struct Region {
        uint64_t at = 0;      // base offset the bytes are inserted at
        uint64_t count = 0;   // entries or instances added
        Bytes    bytes;
    };
    // What a field path resolved to.
    struct Node {
        enum Kind : uint8_t { Inst, Slot, Array } kind = Inst;
        uint32_t st = 0;               // Inst: its struct
        uint64_t index = kNoIndex;     // Inst: index among the struct's instances; kNoIndex inline
        Loc      loc;                  // Inst: its bytes. Slot: the value. Array: the inline (count, first)
        uint16_t dataType = 0, typeIndex = 0;   // Slot, Array: the property's
    };
    struct Step { std::string_view name, sel; bool hasSel = false; };
    // A value ready to write. Strings get their offset at commit time.
    struct Encoded {
        uint8_t     bytes[20] = {};
        uint32_t    size = 0;
        bool        string = false, haveOffset = false;
        uint32_t    offset = 0;
        std::string text;
    };

    const Schema& s;
    std::span<const uint8_t> f;
    PatchOptions opt;
    bool valid = false;
    std::array<uint64_t, kKinds> poolAt{}, poolEnd{}, poolRank{};
    uint64_t stringsEnd = 0, stringsRank = 0, mappingsAt = 0;
    std::vector<std::vector<uint32_t>> mappingsOf;   // per struct, in file order
    std::map<uint64_t, uint8_t> over;                // base offset -> new byte
    std::map<uint64_t, Region> regions;              // by rank (file order)
    std::unordered_map<std::string, uint32_t> strings;   // value-pool offsets of strings this patch added
    std::vector<OpReport> reports;

    Impl(const Schema& schema, PatchOptions o) : s(schema), f(schema.File()), opt(o) {
        if (s.failed != Check::None || s.recordSize == 0 || f.size() != s.fileSize) return;
        bool pools = true;
        for (size_t k = 0; k < kKinds; ++k) {
            const std::string want = std::string("values: ") + ValueKindName(static_cast<ValueKind>(k));
            bool found = false;
            for (size_t i = 0; i < s.tables.size(); ++i)
                if (s.tables[i].name == want) {
                    poolAt[k] = s.tables[i].offset;
                    poolEnd[k] = s.tables[i].offset + s.tables[i].bytes;
                    poolRank[k] = i;
                    found = true;
                }
            pools &= found;
        }
        bool strs = false, maps = false;
        for (size_t i = 0; i < s.tables.size(); ++i) {
            if (s.tables[i].name == "value strings") {
                stringsEnd = s.tables[i].offset + s.tables[i].bytes;
                stringsRank = i;
                strs = true;
            } else if (s.tables[i].name == "mappings") {
                mappingsAt = s.tables[i].offset;
                maps = true;
            }
        }
        mappingsOf.resize(s.structs.size());
        for (uint32_t m = 0; m < s.mappings.size(); ++m) mappingsOf[s.mappings[m].structIndex].push_back(m);
        valid = pools && strs && maps;
    }

    // ---- the overlay ---------------------------------------------------------------------------

    const Region* Find(uint64_t rank) const {
        const auto it = regions.find(rank);
        return it == regions.end() ? nullptr : &it->second;
    }
    Region& Grow(uint64_t rank, uint64_t at) {
        auto [it, fresh] = regions.try_emplace(rank);
        if (fresh) it->second.at = at;
        return it->second;
    }
    bool Read(Loc l, uint8_t* dst, uint64_t n) const {
        if (l.region == kBase) {
            if (l.off > f.size() || n > f.size() - l.off) return false;
            if (n) std::memcpy(dst, f.data() + l.off, n);
            for (auto it = over.lower_bound(l.off); it != over.end() && it->first - l.off < n; ++it)
                dst[it->first - l.off] = it->second;
            return true;
        }
        const Region* r = Find(l.region);
        if (!r || l.off > r->bytes.size() || n > r->bytes.size() - l.off) return false;
        if (n) std::memcpy(dst, r->bytes.data() + l.off, n);
        return true;
    }
    bool U32(Loc l, uint32_t& v) const {
        uint8_t b[4];
        if (!Read(l, b, 4)) return false;
        v = R32(b);
        return true;
    }
    // Callers have read the range first; out-of-range writes are dropped, never made.
    void Write(Loc l, const uint8_t* src, uint64_t n) {
        if (l.region == kBase) {
            if (l.off > f.size() || n > f.size() - l.off) return;
            for (uint64_t i = 0; i < n; ++i) over[l.off + i] = src[i];
            return;
        }
        const auto it = regions.find(l.region);
        if (it == regions.end() || l.off > it->second.bytes.size() || n > it->second.bytes.size() - l.off) return;
        if (n) std::memcpy(it->second.bytes.data() + l.off, src, n);
    }
    void WriteU32x2(Loc l, uint32_t a, uint32_t b) {
        uint8_t v[8];
        Put(v, a, 4);
        Put(v + 4, b, 4);
        Write(l, v, 8);
    }

    // ---- counts and locations ------------------------------------------------------------------

    uint64_t PoolCount(ValueKind k) const {
        const Region* r = Find(poolRank[static_cast<size_t>(k)]);
        return s.header.values[static_cast<size_t>(k)] + (r ? r->count : 0);
    }
    bool PoolLoc(ValueKind k, uint32_t entry, uint64_t i, Loc& out) const {
        const size_t ki = static_cast<size_t>(k);
        const uint64_t base = s.header.values[ki];
        if (i < base) { out = { kBase, poolAt[ki] + i * entry }; return true; }
        const Region* r = Find(poolRank[ki]);
        if (!r || i - base >= r->count) return false;
        out = { poolRank[ki], (i - base) * entry };
        return true;
    }
    uint64_t BlockRank(uint32_t st) const { return kBlockRank + mappingsOf[st].back(); }
    uint64_t Instances(uint32_t st) const {
        if (st >= s.structs.size()) return 0;
        uint64_t n = s.structInfo[st].instances;
        if (!mappingsOf[st].empty())
            if (const Region* r = Find(BlockRank(st))) n += r->count;
        return n;
    }
    bool InstanceLoc(uint32_t st, uint64_t i, Loc& out) const {
        if (st >= s.structs.size()) return false;
        const uint64_t size = s.structInfo[st].size;
        for (uint32_t m : mappingsOf[st]) {
            const uint64_t n = s.mappings[m].instanceCount;
            if (i < n) { out = { kBase, s.blockOffsets[m] + i * size }; return true; }
            i -= n;
        }
        if (mappingsOf[st].empty()) return false;
        const Region* r = Find(BlockRank(st));
        if (!r || i >= r->count) return false;
        out = { BlockRank(st), i * size };
        return true;
    }
    bool Derives(uint32_t st, uint32_t base) const {
        int depth = 0;
        for (int64_t c = st; c >= 0 && static_cast<uint64_t>(c) < s.structs.size() && depth <= kMaxDepth;
             c = s.structs[static_cast<size_t>(c)].parent, ++depth)
            if (static_cast<uint64_t>(c) == base) return true;
        return false;
    }
    std::string SName(uint32_t st) const { return std::string(s.StructName(st)); }

    // ---- resolving -----------------------------------------------------------------------------

    static Status ParsePath(std::string_view path, std::vector<Step>& out) {
        out.clear();
        if (path.empty()) return {};
        size_t at = 0;
        while (true) {
            const size_t dot = path.find('.', at);
            std::string_view part = path.substr(at, dot == std::string_view::npos ? std::string_view::npos : dot - at);
            Step step;
            const size_t open = part.find('[');
            if (open != std::string_view::npos) {
                if (part.back() != ']' || open + 2 > part.size() - 1)
                    return Refuse(Refusal::BadArgument, "bad path step " + Q(part) + ": expected name[index] or name[Type]");
                step.sel = part.substr(open + 1, part.size() - open - 2);
                step.hasSel = true;
                part = part.substr(0, open);
            }
            step.name = part;
            if (step.name.empty() || step.name.find_first_of("[]") != std::string_view::npos ||
                (step.hasSel && step.sel.find_first_of("[]") != std::string_view::npos))
                return Refuse(Refusal::BadArgument, "bad path step " + Q(path.substr(at, dot - at)));
            out.push_back(step);
            if (dot == std::string_view::npos) return {};
            at = dot + 1;
        }
    }

    Status Root(const RecordRef& ref, Node& out) const {
        const Record* r = ref.guid ? s.FindRecord(*ref.guid) : nullptr;
        if (!r && !ref.name.empty()) r = s.FindRecordByName(ref.name);
        if (!r) {
            if (!ref.guid && ref.name.empty()) return Refuse(Refusal::BadArgument, "no record given");
            return Refuse(Refusal::RecordNotFound, "not found");
        }
        if (s.structInfo[r->structIndex].opaque)
            return Refuse(Refusal::Opaque, "struct " + SName(r->structIndex) + " has a field of unknown type");
        Loc l;
        if (!InstanceLoc(r->structIndex, r->instanceIndex, l)) return Refuse(Refusal::Corrupt, "its root instance doesn't exist");
        out = { Node::Inst, r->structIndex, r->instanceIndex, l, 0, 0 };
        return {};
    }
    Status Start(InstanceId id, Node& out) const {
        if (!id.valid()) return Refuse(Refusal::DependencyFailed, "the instance wasn't added (its AddInstance was refused)");
        Loc l;
        if (!InstanceLoc(id.structIndex, id.index, l)) return Refuse(Refusal::BadArgument, "no such instance");
        out = { Node::Inst, id.structIndex, id.index, l, 0, 0 };
        return {};
    }

    // Follows a strong pointer slot to its instance.
    Status Follow(Node& n, const std::string& where) const {
        uint8_t b[8];
        if (!Read(n.loc, b, 8)) return Refuse(Refusal::Corrupt, Q(where) + " can't be read");
        const uint32_t st = R32(b), idx = R32(b + 4);
        if (st == kNull) return Refuse(Refusal::FieldNotFound, Q(where) + " is a null pointer");
        Loc l;
        if (!InstanceLoc(st, idx, l))
            return Refuse(Refusal::Corrupt, Fmt("\"%s\" points at instance %u of struct %u, which doesn't exist", where.c_str(), idx, st));
        n = { Node::Inst, st, idx, l, 0, 0 };
        return {};
    }

    // The (count, first) of an array slot, checked against its pool or block.
    Status ArrayRange(const Node& n, uint32_t& count, uint32_t& first, const std::string& where) const {
        uint8_t b[8];
        if (!Read(n.loc, b, 8)) return Refuse(Refusal::Corrupt, Q(where) + " can't be read");
        count = R32(b);
        first = R32(b + 4);
        uint64_t total = 0;
        if (n.dataType == type::kClass) {
            if (n.typeIndex >= s.structs.size()) return Refuse(Refusal::Corrupt, Q(where) + " has elements of an unknown struct");
            total = Instances(n.typeIndex);
        } else {
            PoolType p;
            if (!PoolOf(n.dataType, p)) return Refuse(Refusal::Opaque, Q(where) + " has elements of unknown type");
            total = PoolCount(p.kind);
        }
        if (count != 0 && static_cast<uint64_t>(first) + count > total)
            return Refuse(Refusal::Corrupt, Fmt("\"%s\" (%u elements from %u) runs past the %llu entries of its pool",
                                                where.c_str(), count, first, static_cast<ull>(total)));
        return {};
    }

    Status Element(const Node& arr, uint64_t i, Node& out, const std::string& where) const {
        Loc l;
        if (arr.dataType == type::kClass) {
            if (!InstanceLoc(arr.typeIndex, i, l)) return Refuse(Refusal::Corrupt, Q(where) + " element can't be found");
            out = { Node::Inst, arr.typeIndex, i, l, 0, 0 };
            return {};
        }
        PoolType p;
        if (!PoolOf(arr.dataType, p) || !PoolLoc(p.kind, p.entry, i, l))
            return Refuse(Refusal::Corrupt, Q(where) + " element can't be found");
        out = { Node::Slot, 0, kNoIndex, l, arr.dataType, arr.typeIndex };
        return {};
    }

    Status Walk(Node cur, std::string_view path, Node& out) const {
        std::vector<Step> steps;
        if (Status st = ParsePath(path, steps); !st) return st;
        std::string done;
        for (const Step& step : steps) {
            if (cur.kind == Node::Slot && cur.dataType == type::kStrongPointer)
                if (Status st = Follow(cur, done); !st) return st;
            if (cur.kind != Node::Inst) {
                if (cur.kind == Node::Slot && (cur.dataType == type::kWeakPointer || cur.dataType == type::kReference))
                    return Refuse(Refusal::FieldNotFound, Q(done) + " is a " + TypeName(cur.dataType) +
                                                              ": weak pointers and references aren't followed; name the record they point at");
                return Refuse(Refusal::FieldNotFound, Q(done) + " is " + (cur.kind == Node::Array ? "an array" : "a value") +
                                                          " (" + TypeName(cur.dataType) + "), it has no fields");
            }
            if (cur.st >= s.structs.size()) return Refuse(Refusal::Corrupt, Fmt("struct %u doesn't exist", cur.st));
            if (s.structInfo[cur.st].opaque)
                return Refuse(Refusal::Opaque, "struct " + SName(cur.st) + " has a field of unknown type");
            // The property and its offset: parent's properties first, packed.
            uint64_t off = 0;
            const PropertyDef* prop = nullptr;
            for (uint32_t k : s.Properties(cur.st)) {
                const PropertyDef& d = s.properties[k];
                if (s.Name(d.name) == step.name) { prop = &d; break; }
                if (d.conversion != 0) off = SatAdd(off, 8);
                else if (d.dataType == type::kClass) off = SatAdd(off, s.structInfo[d.typeIndex].size);
                else off = SatAdd(off, FieldSize(d.dataType));
            }
            const std::string here = done.empty() ? std::string(step.name) : done + "." + std::string(step.name);
            if (!prop) return Refuse(Refusal::FieldNotFound, "no property " + Q(step.name) + " in " + SName(cur.st));
            const Loc at = At(cur.loc, off);
            done = here;
            if (prop->conversion == 0) {
                if (step.hasSel) return Refuse(Refusal::TypeMismatch, Q(here) + " is a " + TypeName(prop->dataType) + ", not an array");
                if (prop->dataType == type::kClass) cur = { Node::Inst, prop->typeIndex, kNoIndex, at, 0, 0 };
                else cur = { Node::Slot, 0, kNoIndex, at, prop->dataType, prop->typeIndex };
                continue;
            }
            Node arr{ Node::Array, 0, kNoIndex, at, prop->dataType, prop->typeIndex };
            if (!step.hasSel) { cur = arr; continue; }
            uint32_t count = 0, first = 0;
            if (Status st = ArrayRange(arr, count, first, here); !st) return st;
            done = here + "[" + std::string(step.sel) + "]";
            if (step.sel.find_first_not_of("0123456789") == std::string_view::npos) {
                uint64_t i = 0;
                for (char c : step.sel) i = SatAdd(SatMul(i, 10), static_cast<uint64_t>(c - '0'));
                if (i >= count)
                    return Refuse(Refusal::IndexOutOfRange, Fmt("\"%s\" has %u elements", here.c_str(), count));
                if (Status st = Element(arr, first + i, cur, done); !st) return st;
                continue;
            }
            const int64_t want = s.FindStruct(step.sel);
            if (want < 0) return Refuse(Refusal::StructNotFound, "no struct " + Q(step.sel));
            const auto wst = static_cast<uint32_t>(want);
            bool found = false;
            if (prop->dataType == type::kClass) {
                if (count > 0 && Derives(prop->typeIndex, wst)) {
                    if (Status st = Element(arr, first, cur, done); !st) return st;
                    found = true;
                }
            } else if (prop->dataType == type::kStrongPointer || prop->dataType == type::kWeakPointer) {
                for (uint32_t i = 0; i < count && !found; ++i) {
                    Node e;
                    if (Status st = Element(arr, static_cast<uint64_t>(first) + i, e, done); !st) return st;
                    uint8_t b[8];
                    if (!Read(e.loc, b, 8)) return Refuse(Refusal::Corrupt, Q(done) + " can't be read");
                    const uint32_t est = R32(b);
                    if (est != kNull && est < s.structs.size() && Derives(est, wst)) { cur = e; found = true; }
                }
            } else {
                return Refuse(Refusal::TypeMismatch, Q(here) + " is an array of " + TypeName(prop->dataType) +
                                                         ": [Type] selects only among structs and pointers");
            }
            if (!found) return Refuse(Refusal::IndexOutOfRange, Q(here) + " has no element of type " + std::string(step.sel));
        }
        out = cur;
        return {};
    }

    // ---- values --------------------------------------------------------------------------------

    Status CheckTarget(InstanceId id, uint32_t typeIndex) const {
        if (!id.valid()) return Refuse(Refusal::DependencyFailed, "the target instance wasn't added (its AddInstance was refused)");
        if (id.structIndex >= s.structs.size() || id.index >= Instances(id.structIndex))
            return Refuse(Refusal::BadArgument, Fmt("no instance %u of struct %u", id.index, id.structIndex));
        if (typeIndex >= s.structs.size()) return Refuse(Refusal::Corrupt, Fmt("the field's struct %u doesn't exist", typeIndex));
        if (!Derives(id.structIndex, typeIndex))
            return Refuse(Refusal::TypeMismatch, "the target is a " + SName(id.structIndex) + ", not a " + SName(typeIndex));
        return {};
    }

    // `current`: the slot being overwritten, whose string is reused when it already says `text`.
    Status Encode(uint16_t t, uint16_t typeIndex, const Value& v, const Loc* current, Encoded& e) const {
        using K = Value::Kind;
        const auto mismatch = [&] {
            return Refuse(Refusal::TypeMismatch, std::string("a ") + KindName(v.kind) + " value doesn't fit a " + TypeName(t) + " field");
        };
        e = Encoded{};
        switch (t) {
        case type::kBool:
            if (v.kind != K::Bool) return mismatch();
            e.size = 1;
            e.bytes[0] = static_cast<uint8_t>(v.b ? 1 : 0);
            return {};
        case type::kInt8: case type::kInt16: case type::kInt32: case type::kInt64: {
            const uint32_t n = FieldSize(t);
            const int64_t hi = n == 8 ? INT64_MAX : (int64_t{ 1 } << (8 * n - 1)) - 1, lo = -hi - 1;
            int64_t x = 0;
            if (v.kind == K::Int) x = v.i;
            else if (v.kind == K::UInt) {
                if (v.u > static_cast<uint64_t>(hi))
                    return Refuse(Refusal::ValueOutOfRange, Fmt("%llu is out of range for a %s field", static_cast<ull>(v.u), TypeName(t)));
                x = static_cast<int64_t>(v.u);
            } else return mismatch();
            if (x < lo || x > hi)
                return Refuse(Refusal::ValueOutOfRange, Fmt("%lld is out of range for a %s field", static_cast<long long>(x), TypeName(t)));
            e.size = n;
            Put(e.bytes, static_cast<uint64_t>(x), n);
            return {};
        }
        case type::kUInt8: case type::kUInt16: case type::kUInt32: case type::kUInt64: {
            const uint32_t n = FieldSize(t);
            const uint64_t hi = n == 8 ? UINT64_MAX : (uint64_t{ 1 } << (8 * n)) - 1;
            uint64_t x = 0;
            if (v.kind == K::UInt) x = v.u;
            else if (v.kind == K::Int) {
                if (v.i < 0)
                    return Refuse(Refusal::ValueOutOfRange, Fmt("%lld is out of range for a %s field", static_cast<long long>(v.i), TypeName(t)));
                x = static_cast<uint64_t>(v.i);
            } else return mismatch();
            if (x > hi) return Refuse(Refusal::ValueOutOfRange, Fmt("%llu is out of range for a %s field", static_cast<ull>(x), TypeName(t)));
            e.size = n;
            Put(e.bytes, x, n);
            return {};
        }
        case type::kFloat: case type::kDouble: {
            double d = 0;
            if (v.kind == K::Float) d = v.f;
            else if (v.kind == K::Int) d = static_cast<double>(v.i);
            else if (v.kind == K::UInt) d = static_cast<double>(v.u);
            else return mismatch();
            if (t == type::kFloat) {
                if (std::isfinite(d) && std::fabs(d) > FLT_MAX)
                    return Refuse(Refusal::ValueOutOfRange, Fmt("%g is out of range for a float field", d));
                const float x = static_cast<float>(d);
                uint32_t bits;
                std::memcpy(&bits, &x, 4);
                e.size = 4;
                Put(e.bytes, bits, 4);
            } else {
                uint64_t bits;
                std::memcpy(&bits, &d, 8);
                e.size = 8;
                Put(e.bytes, bits, 8);
            }
            return {};
        }
        case type::kGuid:
            if (v.kind != K::Guid) return mismatch();
            e.size = 16;
            std::memcpy(e.bytes, v.guid.bytes.data(), 16);
            return {};
        case type::kString: case type::kLocale:
            if (v.kind != K::String) return mismatch();
            e.text = v.s;
            break;
        case type::kEnum: {
            if (v.kind != K::Enum && v.kind != K::String) return mismatch();
            if (typeIndex >= s.enums.size()) return Refuse(Refusal::Corrupt, Fmt("the field's enum %u doesn't exist", static_cast<unsigned>(typeIndex)));
            const EnumDef& en = s.enums[typeIndex];
            bool known = false;
            for (uint32_t k = en.firstOption; k < static_cast<uint32_t>(en.firstOption) + en.optionCount && k < s.enumOptions.size(); ++k)
                known |= s.Name(s.enumOptions[k]) == v.s;
            if (!known)
                return Refuse(Refusal::UnknownEnumOption, Q(v.s) + " is not an option of enum " + std::string(s.Name(en.name)));
            e.text = v.s;
            break;
        }
        case type::kStrongPointer: case type::kWeakPointer:
            e.size = 8;
            if (v.kind == K::Null) {
                Put(e.bytes, kNull, 4);
                Put(e.bytes + 4, kNull, 4);
                return {};
            }
            if (v.kind != K::Instance) return mismatch();
            if (Status st = CheckTarget(v.instance, typeIndex); !st) return st;
            Put(e.bytes, v.instance.structIndex, 4);
            Put(e.bytes + 4, v.instance.index, 4);
            return {};
        case type::kReference:
            return Refuse(Refusal::Unsupported, "reference fields wait for research R1 (the u32 before the GUID)");
        case type::kClass:
            return Refuse(Refusal::TypeMismatch, "a struct field takes no value; clone an instance with AddInstance");
        default:
            return Refuse(Refusal::Opaque, Fmt("unknown data type 0x%x", static_cast<unsigned>(t)));
        }
        // Strings (string, locale, enum): a value-pool offset.
        e.string = true;
        e.size = 4;
        if (e.text.find('\0') != std::string::npos) return Refuse(Refusal::BadArgument, "a string can't contain NUL");
        uint32_t cur = 0;
        if (current && U32(*current, cur) && cur < s.header.valueStringLength && s.ValueString(cur) == e.text) {
            e.haveOffset = true;
            e.offset = cur;
        } else if (const auto it = strings.find(e.text); it != strings.end()) {
            e.haveOffset = true;
            e.offset = it->second;
        } else {
            const Region* r = Find(stringsRank);
            const uint64_t len = static_cast<uint64_t>(s.header.valueStringLength) + (r ? r->bytes.size() : 0);
            if (SatAdd(len, e.text.size() + 1) > kU32) return Refuse(Refusal::Limit, "the value-string pool would pass 4 GiB");
        }
        return {};
    }

    // Commit (cannot fail): the string, then the bytes.
    void Finish(Encoded& e) {
        if (!e.string) return;
        if (!e.haveOffset) {
            Region& r = Grow(stringsRank, stringsEnd);
            e.offset = static_cast<uint32_t>(s.header.valueStringLength + r.bytes.size());
            r.bytes.insert(r.bytes.end(), e.text.begin(), e.text.end());
            r.bytes.push_back(0);
            strings.emplace(e.text, e.offset);
            e.haveOffset = true;
        }
        Put(e.bytes, e.offset, 4);
    }

    void SetMappingCount(uint32_t st) {
        const uint32_t m = mappingsOf[st].back();
        const Region* r = Find(BlockRank(st));
        uint8_t b[4];
        Put(b, s.mappings[m].instanceCount + (r ? r->count : 0), 4);
        Write({ kBase, mappingsAt + 8ull * m }, b, 4);
    }
    // Appends instances of `st` (bytes already sized), returning the index of the first.
    uint64_t PushInstances(uint32_t st, const Bytes& add, uint64_t n) {
        const uint64_t first = Instances(st);
        const uint32_t m = mappingsOf[st].back();
        Region& r = Grow(BlockRank(st), s.blockOffsets[m] + s.mappings[m].instanceCount * s.structInfo[st].size);
        r.bytes.insert(r.bytes.end(), add.begin(), add.end());
        r.count += n;
        SetMappingCount(st);
        return first;
    }
    Status CanAddInstances(uint32_t st, uint64_t n) const {
        if (s.structInfo[st].opaque) return Refuse(Refusal::Opaque, "struct " + SName(st) + " has a field of unknown type");
        if (mappingsOf[st].empty()) return Refuse(Refusal::Unsupported, "struct " + SName(st) + " has no data block to append to");
        const Region* r = Find(BlockRank(st));
        const uint64_t mcount = s.mappings[mappingsOf[st].back()].instanceCount + (r ? r->count : 0);
        if (SatAdd(Instances(st), n) > kU32 || SatAdd(mcount, n) > kU32)
            return Refuse(Refusal::Limit, "struct " + SName(st) + " would pass 2^32 instances");
        return {};
    }

    // ---- operations ----------------------------------------------------------------------------

    Status Override(const Node& root, std::string_view path, const Value& v) {
        Node n;
        if (Status st = Walk(root, path, n); !st) return st;
        if (n.kind == Node::Inst)
            return Refuse(Refusal::TypeMismatch, "it is a " + SName(n.st) + " struct, not a value; clone one with AddInstance and point at it");
        if (n.kind == Node::Array)
            return Refuse(Refusal::TypeMismatch, std::string("it is an array of ") + TypeName(n.dataType) + "; use AppendElement or an index");
        uint8_t probe[20];
        if (!Read(n.loc, probe, FieldSize(n.dataType))) return Refuse(Refusal::Corrupt, "the field can't be read");
        Encoded e;
        if (Status st = Encode(n.dataType, n.typeIndex, v, &n.loc, e); !st) return st;
        Finish(e);
        Write(n.loc, e.bytes, e.size);
        return {};
    }

    Status Point(const Node& root, std::string_view path, InstanceId target) {
        Node n;
        if (Status st = Walk(root, path, n); !st) return st;
        if (n.kind != Node::Slot || (n.dataType != type::kStrongPointer && n.dataType != type::kWeakPointer))
            return Refuse(Refusal::TypeMismatch, "it is not a strong or weak pointer");
        return Override(root, path, Value::OfInstance(target));
    }

    Status Append(const Node& root, std::string_view path, const Value& v) {
        Node n;
        if (Status st = Walk(root, path, n); !st) return st;
        if (n.kind != Node::Array) return Refuse(Refusal::TypeMismatch, "it is not an array");
        uint32_t count = 0, first = 0;
        if (Status st = ArrayRange(n, count, first, std::string(path)); !st) return st;
        if (count == kU32) return Refuse(Refusal::Limit, "the array has 2^32-1 elements");
        if (n.dataType == type::kClass) {
            const uint32_t st = n.typeIndex;   // checked by ArrayRange
            if (v.kind != Value::Kind::Instance)
                return Refuse(Refusal::TypeMismatch, "elements are " + SName(st) + " structs: pass the instance to copy");
            if (Status c = CheckTarget(v.instance, st); !c) return c;
            if (v.instance.structIndex != st)
                return Refuse(Refusal::TypeMismatch, "the array holds " + SName(st) + " structs exactly, not " + SName(v.instance.structIndex));
            const uint64_t total = Instances(st);
            const bool inPlace = count > 0 && static_cast<uint64_t>(first) + count == total;
            const uint64_t add = inPlace ? 1 : static_cast<uint64_t>(count) + 1;
            if (Status c = CanAddInstances(st, add); !c) return c;
            const uint64_t size = s.structInfo[st].size;
            Bytes bytes(SatMul(add, size));
            Loc l;
            for (uint64_t i = 0; i + 1 < add; ++i)
                if (!InstanceLoc(st, first + i, l) || !Read(l, bytes.data() + i * size, size))
                    return Refuse(Refusal::Corrupt, "an element can't be read");
            if (!InstanceLoc(st, v.instance.index, l) || !Read(l, bytes.data() + (add - 1) * size, size))
                return Refuse(Refusal::Corrupt, "the instance to copy can't be read");
            const uint64_t at = PushInstances(st, bytes, add);
            WriteU32x2(n.loc, count + 1, static_cast<uint32_t>(inPlace ? first : at));
            return {};
        }
        PoolType p;
        if (!PoolOf(n.dataType, p)) return Refuse(Refusal::Opaque, "elements of unknown type");
        Encoded e;
        if (Status st = Encode(n.dataType, n.typeIndex, v, nullptr, e); !st) return st;
        const uint64_t total = PoolCount(p.kind);
        const bool inPlace = count > 0 && static_cast<uint64_t>(first) + count == total;
        const uint64_t add = inPlace ? 1 : static_cast<uint64_t>(count) + 1;
        if (SatAdd(total, add) > kU32) return Refuse(Refusal::Limit, std::string("the ") + ValueKindName(p.kind) + " pool would pass 2^32 entries");
        Bytes bytes(SatMul(add, p.entry));
        for (uint64_t i = 0; i + 1 < add; ++i) {
            Loc l;
            if (!PoolLoc(p.kind, p.entry, first + i, l) || !Read(l, bytes.data() + i * p.entry, p.entry))
                return Refuse(Refusal::Corrupt, "an element can't be read");
        }
        Finish(e);
        std::memcpy(bytes.data() + (add - 1) * p.entry, e.bytes, p.entry);
        const size_t k = static_cast<size_t>(p.kind);
        Region& r = Grow(poolRank[k], poolEnd[k]);
        r.bytes.insert(r.bytes.end(), bytes.begin(), bytes.end());
        r.count += add;
        WriteU32x2(n.loc, count + 1, static_cast<uint32_t>(inPlace ? first : total));
        return {};
    }

    // An instance from a source path: strong pointers followed; `block` requires a block instance.
    Status Source(const InstanceSource& src, Node& out, bool block) const {
        if (!src.record) return Refuse(Refusal::BadArgument, "no record given");
        Node root;
        if (Status st = Root(*src.record, root); !st) return st;
        if (Status st = Walk(root, src.field, out); !st) return st;
        if (out.kind == Node::Slot && out.dataType == type::kStrongPointer)
            if (Status st = Follow(out, src.field); !st) return st;
        if (out.kind != Node::Inst) return Refuse(Refusal::TypeMismatch, "it is not an instance");
        if (block && out.index == kNoIndex)
            return Refuse(Refusal::TypeMismatch, "it is an inline struct member, not an instance a pointer can target");
        return {};
    }

    Status AddInstance(std::string_view type, const InstanceSource& src, InstanceId& out) {
        const int64_t found = s.FindStruct(type);
        if (found < 0) return Refuse(Refusal::StructNotFound, "no struct " + Q(type));
        const auto st = static_cast<uint32_t>(found);
        if (Status c = CanAddInstances(st, 1); !c) return c;
        const uint64_t size = s.structInfo[st].size;
        Bytes bytes(size, 0);
        if (src.record) {
            Node n;
            if (Status c = Source(src, n, false); !c) return Refuse(c.category, "clone source: " + c.message);
            if (n.st != st) return Refuse(Refusal::TypeMismatch, "clone source is a " + SName(n.st) + ", not a " + SName(st));
            if (!Read(n.loc, bytes.data(), size)) return Refuse(Refusal::Corrupt, "clone source can't be read");
        } else if (!src.field.empty()) {
            return Refuse(Refusal::BadArgument, "clone source has a field but no record");
        }
        out = { st, static_cast<uint32_t>(PushInstances(st, bytes, 1)) };
        return {};
    }

    // ---- emit ----------------------------------------------------------------------------------

    Status Emit(std::vector<vfs::Splice>& out) const {
        out.clear();
        if (!valid) return Refuse(Refusal::Layout, "the base file failed validation: " + s.error);
        if (opt.atomic)
            for (size_t i = 0; i < reports.size(); ++i)
                if (!reports[i].status)
                    return Refuse(reports[i].status.category, Fmt("operation %zu refused, so the batch applies nothing: ", i) +
                                                                  reports[i].op + ": " + reports[i].status.message);

        // The header: pool counts and the value-string length, as one 120-byte overwrite.
        std::map<uint64_t, uint8_t> w = over;
        uint8_t h[kHeaderSize];
        std::memcpy(h, f.data(), kHeaderSize);
        for (size_t k = 0; k < kKinds; ++k)
            if (const Region* r = Find(poolRank[k]); r && r->count)
                Put(h + 36 + 4 * k, s.header.values[k] + r->count, 4);
        if (const Region* r = Find(stringsRank); r && !r->bytes.empty())
            Put(h + 112, s.header.valueStringLength + r->bytes.size(), 4);
        if (std::memcmp(h, f.data(), kHeaderSize) != 0)
            for (uint64_t i = 0; i < kHeaderSize; ++i) w[i] = h[i];

        struct Run { uint64_t at; Bytes bytes; };
        std::vector<Run> runs, ins;
        for (const auto& [o, b] : w) {
            if (!runs.empty() && runs.back().at + runs.back().bytes.size() == o) runs.back().bytes.push_back(b);
            else runs.push_back({ o, { b } });
        }
        // Regions in rank order are in file order, so `at` never decreases; equal ones (an empty
        // pool or block before another) merge in that order.
        for (const auto& kv : regions) {
            const Region& r = kv.second;
            if (r.bytes.empty()) continue;
            if (!ins.empty() && ins.back().at == r.at) ins.back().bytes.insert(ins.back().bytes.end(), r.bytes.begin(), r.bytes.end());
            else ins.push_back({ r.at, r.bytes });
        }

        auto splice = [&](uint64_t at, uint64_t removed, Bytes bytes) {
            vfs::Splice sp;
            sp.at = at;
            sp.removed = removed;
            sp.bytes = std::make_shared<const Bytes>(std::move(bytes));
            if (removed) sp.old = std::make_shared<const Bytes>(f.begin() + static_cast<std::ptrdiff_t>(at),
                                                                f.begin() + static_cast<std::ptrdiff_t>(at + removed));
            out.push_back(std::move(sp));
        };
        size_t i = 0;
        for (const Run& run : runs) {
            const uint64_t end = run.at + run.bytes.size();
            for (; i < ins.size() && ins[i].at < run.at; ++i) splice(ins[i].at, 0, ins[i].bytes);
            // An insert inside an overwritten run joins it: one splice, insert bytes before the byte at its offset.
            Bytes bytes;
            for (uint64_t q = run.at; q < end; ++q) {
                if (i < ins.size() && ins[i].at == q) {
                    bytes.insert(bytes.end(), ins[i].bytes.begin(), ins[i].bytes.end());
                    ++i;
                }
                bytes.push_back(run.bytes[q - run.at]);
            }
            splice(run.at, run.bytes.size(), std::move(bytes));
        }
        for (; i < ins.size(); ++i) splice(ins[i].at, 0, ins[i].bytes);

        // Re-validation: the patched file must pass the parser.
        Bytes patched;
        if (Status st = ApplySplices(f, out, patched); !st) {
            out.clear();
            return Refuse(Refusal::Revalidation, "re-validation failed: " + st.message);
        }
        Schema re;
        if (!re.Parse(patched)) {
            out.clear();
            return Refuse(Refusal::Revalidation, "re-validation failed: " + re.error);
        }
        return {};
    }

    // ---- reporting -----------------------------------------------------------------------------

    static std::string Ref(const RecordRef& r) {
        if (!r.name.empty()) return "record " + Q(r.name);
        return r.guid ? "record {" + FormatGuid(*r.guid) + "}" : std::string("record (none)");
    }
    std::string Ref(InstanceId id) const {
        if (!id.valid()) return "instance (not added)";
        return Fmt("instance %u of ", id.index) + (id.structIndex < s.structs.size() ? SName(id.structIndex) : Fmt("struct %u", id.structIndex));
    }
    Status Report(std::string op, const std::string& where, Status st) {
        if (!st) st.message = where + ": " + st.message;
        reports.push_back({ std::move(op), st });
        return st;
    }
    template <class Target, class Fn>
    Status Run(const char* name, const Target& target, std::string_view path, Fn&& fn) {
        const std::string where = Ref(target) + (path.empty() ? std::string() : " field " + Q(path));
        const std::string op = std::string(name) + " " + where;
        if (!valid) return Report(op, where, Refuse(Refusal::Layout, "the base file failed validation: " + s.error));
        Node root;
        Status st;
        if constexpr (std::is_same_v<Target, RecordRef>) st = Root(target, root);
        else st = Start(target, root);
        if (st) st = fn(root);
        return Report(op, where, std::move(st));
    }
};

Patch::Patch(const Schema& base, PatchOptions options) : impl_(std::make_unique<Impl>(base, options)) {}
Patch::~Patch() = default;
Patch::Patch(Patch&&) noexcept = default;
Patch& Patch::operator=(Patch&&) noexcept = default;

Status Patch::OverrideField(const RecordRef& rec, std::string_view path, const Value& v) {
    return impl_->Run("OverrideField", rec, path, [&](const Impl::Node& n) { return impl_->Override(n, path, v); });
}
Status Patch::OverrideField(InstanceId inst, std::string_view path, const Value& v) {
    return impl_->Run("OverrideField", inst, path, [&](const Impl::Node& n) { return impl_->Override(n, path, v); });
}
Status Patch::SetPointer(const RecordRef& rec, std::string_view path, InstanceId target) {
    return impl_->Run("SetPointer", rec, path, [&](const Impl::Node& n) { return impl_->Point(n, path, target); });
}
Status Patch::SetPointer(InstanceId inst, std::string_view path, InstanceId target) {
    return impl_->Run("SetPointer", inst, path, [&](const Impl::Node& n) { return impl_->Point(n, path, target); });
}
Status Patch::AppendElement(const RecordRef& rec, std::string_view path, const Value& v) {
    return impl_->Run("AppendElement", rec, path, [&](const Impl::Node& n) { return impl_->Append(n, path, v); });
}
Status Patch::AppendElement(InstanceId inst, std::string_view path, const Value& v) {
    return impl_->Run("AppendElement", inst, path, [&](const Impl::Node& n) { return impl_->Append(n, path, v); });
}

Status Patch::AddInstance(std::string_view type, const InstanceSource& cloneFrom, InstanceId& out) {
    out = {};
    const std::string where = "struct " + Q(type);
    const std::string op = "AddInstance " + where;
    if (!impl_->valid)
        return impl_->Report(op, where, Refuse(Refusal::Layout, "the base file failed validation: " + impl_->s.error));
    return impl_->Report(op, where, impl_->AddInstance(type, cloneFrom, out));
}

Status Patch::FindInstance(const InstanceSource& source, InstanceId& out) const {
    out = {};
    if (!impl_->valid) return Refuse(Refusal::Layout, "the base file failed validation: " + impl_->s.error);
    Impl::Node n;
    if (Status st = impl_->Source(source, n, true); !st) return st;
    out = { n.st, static_cast<uint32_t>(n.index) };
    return {};
}

Status Patch::Emit(std::vector<vfs::Splice>& out) const { return impl_->Emit(out); }
const std::vector<OpReport>& Patch::Reports() const { return impl_->reports; }

namespace {
class MemoryIo final : public vfs::BaseIo {
public:
    explicit MemoryIo(std::span<const uint8_t> b) : b_(b) {}
    bool Seek(uint64_t pos) override {
        if (pos > b_.size()) return false;
        pos_ = pos;
        return true;
    }
    size_t Read(void* dst, size_t n) override {
        const size_t k = static_cast<size_t>(std::min<uint64_t>(n, b_.size() - pos_));
        if (k) std::memcpy(dst, b_.data() + pos_, k);
        pos_ += k;
        return k;
    }

private:
    std::span<const uint8_t> b_;
    uint64_t pos_ = 0;
};
}  // namespace

Status ApplySplices(std::span<const uint8_t> base, std::span<const vfs::Splice> splices, std::vector<uint8_t>& out) {
    vfs::Composed c;
    if (vfs::Result r = vfs::Compose(base.size(), splices, c); !r) return Refuse(Refusal::BadArgument, r.error);
    for (size_t i = 0; i < splices.size(); ++i) {
        const vfs::Splice& sp = splices[i];
        if (sp.old && !std::equal(sp.old->begin(), sp.old->end(), base.begin() + static_cast<std::ptrdiff_t>(sp.at)))
            return Refuse(Refusal::BadArgument, Fmt("splice %zu (at %llu): the base bytes differ from its expected old bytes", i,
                                                    static_cast<ull>(sp.at)));
    }
    MemoryIo io(base);
    vfs::Reader reader(std::make_shared<const vfs::Composed>(std::move(c)));
    Bytes data(static_cast<size_t>(reader.Size()));
    if (!data.empty() && reader.Read(data.data(), data.size(), io) != data.size())
        return Refuse(Refusal::BadArgument, "the composed file reads short");
    out = std::move(data);
    return {};
}

}  // namespace sco::datacore
