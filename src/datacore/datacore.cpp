// sco::datacore: parse and validate a DataCore file (sco/datacore.h). Layout and rules:
// docs/design/vfs-datacore.md sections 2 ("The DataCore binary layout") and 4 ("Parse", "Validation").
// Every count comes from the header and every size is checked against the file size before anything
// is read or allocated, so a truncated or corrupted file is refused with a reason, never read past.
#include "sco/datacore.h"
#include "internal.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <limits>

namespace sco::datacore {
namespace {

using ull = unsigned long long;
using detail::Fmt;
using detail::R16;
using detail::R32;
using detail::SatAdd;
using detail::SatMul;
constexpr int kMaxDepth = 256;   // parent chains and inline-class nesting

// The value pools in file order (section 2). `count` indexes Header::values; -1 is the enum-option count.
struct Pool { const char* name; int count; uint32_t entry; };
constexpr Pool kPools[] = {
    { "int8", static_cast<int>(ValueKind::Int8), 1 },     { "int16", static_cast<int>(ValueKind::Int16), 2 },
    { "int32", static_cast<int>(ValueKind::Int32), 4 },   { "int64", static_cast<int>(ValueKind::Int64), 8 },
    { "uint8", static_cast<int>(ValueKind::UInt8), 1 },   { "uint16", static_cast<int>(ValueKind::UInt16), 2 },
    { "uint32", static_cast<int>(ValueKind::UInt32), 4 }, { "uint64", static_cast<int>(ValueKind::UInt64), 8 },
    { "bool", static_cast<int>(ValueKind::Bool), 1 },     { "float", static_cast<int>(ValueKind::Float), 4 },
    { "double", static_cast<int>(ValueKind::Double), 8 }, { "guid", static_cast<int>(ValueKind::Guid), 16 },
    { "string", static_cast<int>(ValueKind::String), 4 }, { "locale", static_cast<int>(ValueKind::Locale), 4 },
    { "enum", static_cast<int>(ValueKind::Enum), 4 },     { "strong", static_cast<int>(ValueKind::Strong), 8 },
    { "weak", static_cast<int>(ValueKind::Weak), 8 },     { "reference", static_cast<int>(ValueKind::Reference), 20 },
    { "enum options", -1, 4 },
};

uint64_t PoolCount(const Header& h, const Pool& pool) {
    return pool.count < 0 ? h.enumOptionCount : h.values[static_cast<size_t>(pool.count)];
}

}  // namespace

uint32_t FieldSize(uint16_t t) {
    switch (t) {
    case type::kBool: case type::kInt8: case type::kUInt8: return 1;
    case type::kInt16: case type::kUInt16: return 2;
    case type::kInt32: case type::kUInt32: case type::kFloat: case type::kString: case type::kLocale:
    case type::kEnum: return 4;
    case type::kInt64: case type::kUInt64: case type::kDouble: case type::kStrongPointer:
    case type::kWeakPointer: return 8;
    case type::kGuid: return 16;
    case type::kReference: return 20;
    default: return 0;
    }
}

const char* TypeName(uint16_t t) {
    switch (t) {
    case type::kBool: return "bool";       case type::kInt8: return "int8";     case type::kInt16: return "int16";
    case type::kInt32: return "int32";     case type::kInt64: return "int64";   case type::kUInt8: return "uint8";
    case type::kUInt16: return "uint16";   case type::kUInt32: return "uint32"; case type::kUInt64: return "uint64";
    case type::kString: return "string";   case type::kFloat: return "float";   case type::kDouble: return "double";
    case type::kLocale: return "locale";   case type::kGuid: return "guid";     case type::kEnum: return "enum";
    case type::kClass: return "class";     case type::kStrongPointer: return "strong";
    case type::kWeakPointer: return "weak"; case type::kReference: return "reference";
    default: return "?";
    }
}

const char* ValueKindName(ValueKind k) {
    static const char* const kNames[] = { "bool", "int8", "int16", "int32", "int64", "uint8", "uint16", "uint32",
                                          "uint64", "float", "double", "guid", "string", "locale", "enum",
                                          "strong", "weak", "reference" };
    const auto i = static_cast<size_t>(k);
    return i < std::size(kNames) ? kNames[i] : "?";
}

const char* CheckName(Check c) {
    switch (c) {
    case Check::None: return "none";
    case Check::File: return "file";
    case Check::Totals: return "totals";
    case Check::Structure: return "structure";
    case Check::RecordSize: return "record size";
    case Check::NameOffset: return "name offset";
    }
    return "?";
}

std::string FormatGuid(const Guid& g) {
    const uint8_t* b = g.bytes.data();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%08x-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", R32(b + 4), R16(b + 2),
                  R16(b), b[15], b[14], b[13], b[12], b[11], b[10], b[9], b[8]);
    return buf;
}

bool Schema::Refuse(Check check, std::string reason) {
    failed = check;
    error = std::move(reason);
    return false;
}

bool Schema::Parse(std::span<const uint8_t> file) {
    *this = Schema{};
    file_ = file;
    fileSize = file.size();
    const uint8_t* p = file.data();
    if (fileSize < kHeaderSize)
        return Refuse(Check::File, Fmt("file: %llu bytes, smaller than the %llu-byte header", static_cast<ull>(fileSize),
                                       static_cast<ull>(kHeaderSize)));

    Header& h = header;
    h.unknown0 = R32(p);
    h.version = R32(p + 4);
    for (size_t i = 0; i < h.unknown8.size(); ++i) h.unknown8[i] = R16(p + 8 + 2 * i);
    h.structCount = R32(p + 16);
    h.propertyCount = R32(p + 20);
    h.enumCount = R32(p + 24);
    h.mappingCount = R32(p + 28);
    h.recordCount = R32(p + 32);
    for (size_t i = 0; i < h.values.size(); ++i) h.values[i] = R32(p + 36 + 4 * i);
    h.enumOptionCount = R32(p + 108);
    h.valueStringLength = R32(p + 112);
    h.nameStringLength = R32(p + 116);
    hasHeader = true;

    // Rules 1-3 need the record size, which is derived below. First make sure the definition tables
    // fit at all, so nothing is read or allocated from counts the file can't hold. Counts are u32 and
    // entries at most 20 bytes, so these sums can't overflow 64 bits.
    const uint64_t sc = h.structCount, pc = h.propertyCount, ec = h.enumCount, mc = h.mappingCount,
                   rc = h.recordCount;
    const uint64_t defsEnd = kHeaderSize + sc * 16 + pc * 12 + ec * 8 + mc * 8;
    uint64_t poolBytes = 0;
    for (const Pool& pool : kPools) poolBytes += PoolCount(h, pool) * pool.entry;
    const uint64_t fixed = defsEnd + poolBytes + h.valueStringLength + h.nameStringLength;
    if (fixed + rc * 32 > fileSize)
        return Refuse(Check::Totals,
                      Fmt("layout: the header's tables need at least %llu bytes, the file has %llu (game format changed)",
                          static_cast<ull>(fixed + rc * 32), static_cast<ull>(fileSize)));

    const uint8_t* q = p + kHeaderSize;
    structs.resize(h.structCount);
    for (StructDef& s : structs) {
        s = { R32(q), static_cast<int32_t>(R32(q + 4)), R16(q + 8), R16(q + 10), R32(q + 12) };
        q += 16;
    }
    properties.resize(h.propertyCount);
    for (PropertyDef& d : properties) {
        d = { R32(q), R16(q + 4), R16(q + 6), R16(q + 8), R16(q + 10) };
        q += 12;
    }
    enums.resize(h.enumCount);
    for (EnumDef& e : enums) {
        e = { R32(q), R16(q + 4), R16(q + 6) };
        q += 8;
    }
    mappings.resize(h.mappingCount);
    for (DataMapping& m : mappings) {
        m = { R32(q), R32(q + 4) };
        q += 8;
    }

    // Indices the sizes depend on (names aren't located yet: the pools sit after the records).
    for (uint32_t i = 0; i < structs.size(); ++i) {
        const StructDef& s = structs[i];
        if (s.parent < -1 || s.parent >= static_cast<int64_t>(sc))
            return Refuse(Check::Structure, Fmt("layout: struct %u has parent %d, outside the %llu structs", i,
                                                s.parent, static_cast<ull>(sc)));
        if (static_cast<uint64_t>(s.firstProperty) + s.propertyCount > pc)
            return Refuse(Check::Structure, Fmt("layout: struct %u's properties %u..%u are outside the %llu properties",
                                                i, static_cast<unsigned>(s.firstProperty),
                                                static_cast<unsigned>(s.firstProperty + s.propertyCount),
                                                static_cast<ull>(pc)));
        int depth = 0;
        for (int32_t a = s.parent; a >= 0; a = structs[static_cast<size_t>(a)].parent)
            if (++depth > kMaxDepth)
                return Refuse(Check::Structure,
                              Fmt("layout: struct %u's parent chain loops or is deeper than %d", i, kMaxDepth));
    }
    for (uint32_t i = 0; i < properties.size(); ++i) {
        const PropertyDef& d = properties[i];
        if (d.conversion == 0 && d.dataType == type::kClass && d.typeIndex >= sc)
            return Refuse(Check::Structure, Fmt("layout: property %u is an inline struct %u, outside the %llu structs",
                                                i, static_cast<unsigned>(d.typeIndex), static_cast<ull>(sc)));
    }
    for (uint32_t i = 0; i < enums.size(); ++i)
        if (static_cast<uint64_t>(enums[i].firstOption) + enums[i].optionCount > h.enumOptionCount)
            return Refuse(Check::Structure, Fmt("layout: enum %u's options are outside the %u enum options", i,
                                                h.enumOptionCount));
    for (uint32_t i = 0; i < mappings.size(); ++i)
        if (mappings[i].structIndex >= sc)
            return Refuse(Check::Structure, Fmt("layout: data mapping %u is for struct %u, outside the %llu structs", i,
                                                mappings[i].structIndex, static_cast<ull>(sc)));

    // Computed instance sizes: the parent's properties first, then the struct's own, packed. An array
    // is 8 bytes inline; an inline class is its struct's size. Rule 6: an unknown type code or array
    // kind makes the struct (and anything containing it inline, or deriving from it) opaque.
    structInfo.assign(structs.size(), {});
    std::vector<uint8_t> state(structs.size(), 0);   // 0 new, 1 in progress, 2 done
    auto sizeOf = [&](auto& self, uint32_t i, int depth) -> bool {
        if (state[i] == 2) return true;
        if (state[i] == 1) return Refuse(Check::Structure, Fmt("layout: struct %u contains itself inline", i));
        if (depth > kMaxDepth)
            return Refuse(Check::Structure, Fmt("layout: struct %u nests inline structs deeper than %d", i, kMaxDepth));
        state[i] = 1;
        const StructDef& d = structs[i];
        uint64_t size = 0;
        bool opaque = false;
        if (d.parent >= 0) {
            const auto parent = static_cast<uint32_t>(d.parent);
            if (!self(self, parent, depth + 1)) return false;
            size = structInfo[parent].size;
            opaque = structInfo[parent].opaque;
        }
        for (uint32_t k = d.firstProperty; k < static_cast<uint32_t>(d.firstProperty) + d.propertyCount; ++k) {
            const PropertyDef& prop = properties[k];
            if (prop.conversion != 0) {
                if (prop.conversion > 3) opaque = true;
                else size = SatAdd(size, 8);
            } else if (prop.dataType == type::kClass) {
                if (!self(self, prop.typeIndex, depth + 1)) return false;
                if (structInfo[prop.typeIndex].opaque) opaque = true;
                else size = SatAdd(size, structInfo[prop.typeIndex].size);
            } else if (const uint32_t fs = FieldSize(prop.dataType)) {
                size = SatAdd(size, fs);
            } else {
                opaque = true;
            }
        }
        state[i] = 2;
        structInfo[i].size = opaque ? 0 : size;
        structInfo[i].opaque = opaque;
        return true;
    };
    for (uint32_t i = 0; i < structs.size(); ++i)
        if (!sizeOf(sizeOf, i, 0)) return false;

    // Rules 1-3 and 2: the record entry size is the one of 32, 36, 40 for which the tables, pools,
    // string pools and the data the mappings need add up exactly to the file size. An opaque struct
    // with instances is sized from its records' structSize (all must agree), else the data can't be.
    auto inferredSize = [&](uint32_t rs, uint32_t s) -> uint64_t {
        uint64_t found = 0;
        const uint8_t* r = p + defsEnd;
        for (uint64_t i = 0; i < rc; ++i, r += rs) {
            const uint8_t* tail = r + rs - 24;
            if (R32(tail) != s) continue;
            const uint16_t sz = R16(tail + 22);
            if (found != 0 && found != sz) return 0;
            found = sz;
        }
        return found;
    };
    struct Candidate { uint32_t size = 0; bool fits = false; uint64_t data = 0, need = 0; int64_t unsized = -1; };
    Candidate cands[3];
    for (size_t i = 0; i < 3; ++i) cands[i].size = 32 + 4 * static_cast<uint32_t>(i);
    for (Candidate& c : cands) {
        if (fixed + rc * c.size > fileSize) continue;
        c.fits = true;
        c.data = fileSize - fixed - rc * c.size;
        for (const DataMapping& m : mappings) {
            if (m.instanceCount == 0) continue;
            uint64_t sz = structInfo[m.structIndex].size;
            if (structInfo[m.structIndex].opaque && (sz = inferredSize(c.size, m.structIndex)) == 0) {
                c.unsized = m.structIndex;
                break;
            }
            c.need = SatAdd(c.need, SatMul(m.instanceCount, sz));
        }
    }
    const Candidate* pick = nullptr;
    for (const Candidate& c : cands)
        if (c.fits && c.unsized < 0 && c.need == c.data) { pick = &c; break; }
    if (!pick) {
        for (const Candidate& c : cands)
            if (c.fits && c.unsized >= 0)
                return Refuse(Check::Totals,
                              Fmt("layout: struct %lld has a field of unknown type and instances, and no record gives "
                                  "its size",
                                  static_cast<long long>(c.unsized)));
        std::string sizes;
        uint64_t need = 0;
        for (const Candidate& c : cands) {
            if (!sizes.empty()) sizes += ", ";
            sizes += c.fits ? Fmt("%llu with %u-byte records", static_cast<ull>(c.data), c.size)
                            : Fmt("none with %u-byte records", c.size);
            if (c.fits) need = c.need;
        }
        return Refuse(Check::Totals, Fmt("layout: records don't add up (game format changed): the mappings need %llu "
                                         "data bytes; the file leaves %s",
                                         static_cast<ull>(need), sizes.c_str()));
    }
    recordSize = pick->size;
    dataSize = pick->data;

    uint64_t off = 0;
    auto add = [&](std::string name, uint64_t count, uint64_t entry, uint64_t bytes) {
        tables.push_back({ std::move(name), off, count, entry, bytes });
        off += bytes;
    };
    add("header", 1, kHeaderSize, kHeaderSize);
    add("structs", sc, 16, sc * 16);
    add("properties", pc, 12, pc * 12);
    add("enums", ec, 8, ec * 8);
    add("mappings", mc, 8, mc * 8);
    add("records", rc, recordSize, rc * recordSize);
    uint64_t enumOptionsAt = 0;
    for (const Pool& pool : kPools) {
        if (pool.count < 0) enumOptionsAt = off;
        add(std::string("values: ") + pool.name, PoolCount(h, pool), pool.entry, PoolCount(h, pool) * pool.entry);
    }
    valuePool_ = off;
    add("value strings", 0, 0, h.valueStringLength);
    namePool_ = off;
    add("name strings", 0, 0, h.nameStringLength);
    dataOffset = off;
    add("data", mc, 0, dataSize);

    enumOptions.resize(h.enumOptionCount);
    for (uint64_t i = 0; i < enumOptions.size(); ++i) enumOptions[i] = R32(p + enumOptionsAt + 4 * i);
    records.resize(h.recordCount);
    for (uint64_t i = 0; i < records.size(); ++i) {
        const uint8_t* e = p + defsEnd + i * recordSize;
        const uint8_t* tail = e + recordSize - 24;
        Record& r = records[i];
        r.name = R32(e);
        r.fileName = R32(e + 4);
        r.unknown = recordSize > 32 ? R32(e + 8) : 0;
        r.structIndex = R32(tail);
        std::memcpy(r.id.bytes.data(), tail + 4, 16);
        r.instanceIndex = R16(tail + 20);
        r.structSize = R16(tail + 22);
    }
    for (uint32_t i = 0; i < mappings.size(); ++i) {
        // An opaque struct's size is its records' structSize (the candidate check above proved they agree
        // for every opaque struct with instances).
        StructInfo& si = structInfo[mappings[i].structIndex];
        if (si.opaque && si.size == 0) si.size = inferredSize(recordSize, mappings[i].structIndex);
    }
    blockOffsets.resize(mappings.size());
    uint64_t at = dataOffset;
    for (uint32_t i = 0; i < mappings.size(); ++i) {
        StructInfo& si = structInfo[mappings[i].structIndex];
        if (si.instances == 0) si.blockOffset = at;
        blockOffsets[i] = at;
        si.instances += mappings[i].instanceCount;
        at += mappings[i].instanceCount * si.size;
    }

    // Rule 4, and each record's root instance must exist.
    for (uint32_t i = 0; i < records.size(); ++i) {
        const Record& r = records[i];
        if (r.structIndex >= sc)
            return Refuse(Check::Structure, Fmt("layout: record %u is of struct %u, outside the %llu structs", i,
                                                r.structIndex, static_cast<ull>(sc)));
        const StructInfo& si = structInfo[r.structIndex];
        if (si.opaque && si.size == 0) continue;   // no instances and no agreeing size: nothing to compare
        if (r.structSize != si.size)
            return Refuse(Check::RecordSize,
                          Fmt("layout: record %u (%.*s) says struct %.*s is %u bytes; its properties make %llu (game "
                              "format changed)",
                              i, static_cast<int>(Name(r.name).size()), Name(r.name).data(),
                              static_cast<int>(StructName(r.structIndex).size()), StructName(r.structIndex).data(),
                              static_cast<unsigned>(r.structSize), static_cast<ull>(si.size)));
        if (r.instanceIndex >= si.instances)
            return Refuse(Check::Structure, Fmt("layout: record %u's root instance %u is past struct %u's %llu instances",
                                                i, static_cast<unsigned>(r.instanceIndex), r.structIndex,
                                                static_cast<ull>(si.instances)));
    }

    // Rule 5: names land on a string start in the name pool (offset 0 or after a NUL), and both pools
    // end in a NUL so every string ends inside its pool. Record file names are value-pool strings.
    const uint64_t nlen = h.nameStringLength, vlen = h.valueStringLength;
    if (nlen != 0 && p[namePool_ + nlen - 1] != 0)
        return Refuse(Check::NameOffset, "layout: the name pool doesn't end in a NUL");
    if (vlen != 0 && p[valuePool_ + vlen - 1] != 0)
        return Refuse(Check::NameOffset, "layout: the value-string pool doesn't end in a NUL");
    auto start = [&](uint64_t pool, uint64_t len, uint32_t o) { return o < len && (o == 0 || p[pool + o - 1] == 0); };
    auto bad = [&](const char* what, uint32_t i, uint32_t o, const char* pool) {
        return Refuse(Check::NameOffset,
                      Fmt("layout: %s %u's name offset %u isn't a string start in the %s pool", what, i, o, pool));
    };
    for (uint32_t i = 0; i < structs.size(); ++i)
        if (!start(namePool_, nlen, structs[i].name)) return bad("struct", i, structs[i].name, "name");
    for (uint32_t i = 0; i < properties.size(); ++i)
        if (!start(namePool_, nlen, properties[i].name)) return bad("property", i, properties[i].name, "name");
    for (uint32_t i = 0; i < enums.size(); ++i)
        if (!start(namePool_, nlen, enums[i].name)) return bad("enum", i, enums[i].name, "name");
    for (uint32_t i = 0; i < enumOptions.size(); ++i)
        if (!start(namePool_, nlen, enumOptions[i])) return bad("enum option", i, enumOptions[i], "name");
    for (uint32_t i = 0; i < records.size(); ++i) {
        if (!start(namePool_, nlen, records[i].name)) return bad("record", i, records[i].name, "name");
        if (!start(valuePool_, vlen, records[i].fileName))
            return Refuse(Check::NameOffset,
                          Fmt("layout: record %u's file name offset %u isn't a string start in the value-string pool",
                              i, records[i].fileName));
    }

    for (uint32_t i = 0; i < structs.size(); ++i) structByName_.emplace(Name(structs[i].name), i);
    for (uint32_t i = 0; i < records.size(); ++i) {
        recordByName_.emplace(Name(records[i].name), i);
        recordByGuid_.emplace(std::string(reinterpret_cast<const char*>(records[i].id.bytes.data()), 16), i);
    }
    return true;
}

std::string_view Schema::Name(uint32_t offset) const {
    if (recordSize == 0 || offset >= header.nameStringLength) return {};
    const char* s = reinterpret_cast<const char*>(file_.data() + namePool_ + offset);
    const size_t max = header.nameStringLength - offset;
    const void* z = std::memchr(s, 0, max);
    return { s, z ? static_cast<size_t>(static_cast<const char*>(z) - s) : max };
}

std::string_view Schema::ValueString(uint32_t offset) const {
    if (recordSize == 0 || offset >= header.valueStringLength) return {};
    const char* s = reinterpret_cast<const char*>(file_.data() + valuePool_ + offset);
    const size_t max = header.valueStringLength - offset;
    const void* z = std::memchr(s, 0, max);
    return { s, z ? static_cast<size_t>(static_cast<const char*>(z) - s) : max };
}

std::string_view Schema::StructName(uint32_t index) const {
    return index < structs.size() ? Name(structs[index].name) : std::string_view{};
}

size_t Schema::OpaqueCount() const {
    return static_cast<size_t>(std::count_if(structInfo.begin(), structInfo.end(),
                                             [](const StructInfo& s) { return s.opaque; }));
}

std::vector<uint32_t> Schema::Properties(uint32_t structIndex) const {
    std::vector<uint32_t> chain, out;
    for (int64_t s = structIndex; s >= 0 && static_cast<uint64_t>(s) < structs.size() && chain.size() <= static_cast<size_t>(kMaxDepth);
         s = structs[static_cast<size_t>(s)].parent)
        chain.push_back(static_cast<uint32_t>(s));
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
        const StructDef& d = structs[*it];
        for (uint32_t k = d.firstProperty; k < static_cast<uint32_t>(d.firstProperty) + d.propertyCount; ++k)
            if (k < properties.size()) out.push_back(k);
    }
    return out;
}

int64_t Schema::FindStruct(std::string_view name) const {
    const auto it = structByName_.find(name);
    return it == structByName_.end() ? int64_t{ -1 } : static_cast<int64_t>(it->second);
}

const Record* Schema::FindRecord(const Guid& id) const {
    const auto it = recordByGuid_.find(std::string(reinterpret_cast<const char*>(id.bytes.data()), 16));
    return it == recordByGuid_.end() ? nullptr : &records[it->second];
}

const Record* Schema::FindRecordByName(std::string_view name) const {
    const auto it = recordByName_.find(name);
    return it == recordByName_.end() ? nullptr : &records[it->second];
}

}  // namespace sco::datacore
