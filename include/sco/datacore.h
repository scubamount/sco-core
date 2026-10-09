#pragma once
// sco::datacore: the game's DataCore database (Data\Game2.dcb), read-only part. Parses the file's
// tables from bytes in memory and validates the layout as docs/design/vfs-datacore.md section 4
// describes: every size is derived from the header and must add up to the file size; the version
// number is never trusted. Patch operations and Emit come in later PRs.
// Standard library only; no engine, no Windows. A Schema is plain data: concurrent const use is fine.
#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace sco::datacore {

// PropertyDef::dataType codes seen in 4.10.193. Anything else makes its struct opaque.
namespace type {
constexpr uint16_t kBool = 0x1, kInt8 = 0x2, kInt16 = 0x3, kInt32 = 0x4, kInt64 = 0x5;
constexpr uint16_t kUInt8 = 0x6, kUInt16 = 0x7, kUInt32 = 0x8, kUInt64 = 0x9;
constexpr uint16_t kString = 0xA, kFloat = 0xB, kDouble = 0xC, kLocale = 0xD, kGuid = 0xE, kEnum = 0xF;
constexpr uint16_t kClass = 0x10, kStrongPointer = 0x110, kWeakPointer = 0x210, kReference = 0x310;
}  // namespace type

// Bytes a single (non-array) field of `dataType` takes inline; 0 for kClass (the struct's size)
// and for unknown codes.
uint32_t FieldSize(uint16_t dataType);
const char* TypeName(uint16_t dataType);   // "float", "strong", ...; "?" for unknown codes

// The 18 value counts, in header order (Header::values).
enum class ValueKind : uint32_t {
    Bool, Int8, Int16, Int32, Int64, UInt8, UInt16, UInt32, UInt64,
    Float, Double, Guid, String, Locale, Enum, Strong, Weak, Reference, Count
};
const char* ValueKindName(ValueKind);

constexpr uint64_t kHeaderSize = 120;

struct Header {
    uint32_t unknown0 = 0;
    uint32_t version = 0;
    std::array<uint16_t, 4> unknown8{};
    uint32_t structCount = 0, propertyCount = 0, enumCount = 0, mappingCount = 0, recordCount = 0;
    std::array<uint32_t, static_cast<size_t>(ValueKind::Count)> values{};
    uint32_t enumOptionCount = 0;
    uint32_t valueStringLength = 0;
    uint32_t nameStringLength = 0;
};

struct Guid {
    std::array<uint8_t, 16> bytes{};
    bool operator==(const Guid&) const = default;
};
// The string form the unp4k / unforge lineage prints (and `guid|path` values use): the 16 file
// bytes read as int16 c, int16 b, int32 a, then bytes k..d, formatted as .NET Guid(a, b, c, d..k).
std::string FormatGuid(const Guid&);

struct StructDef   { uint32_t name; int32_t parent; uint16_t propertyCount, firstProperty; uint32_t nodeType; };
struct PropertyDef { uint32_t name; uint16_t typeIndex, dataType, conversion, pad; };
struct EnumDef     { uint32_t name; uint16_t optionCount, firstOption; };
struct DataMapping { uint32_t instanceCount, structIndex; };
struct Record {
    uint32_t name;        // name pool
    uint32_t fileName;    // value pool
    uint32_t unknown;     // the u32 at +8 of 36-byte records (research R1); 0 for 32-byte records
    uint32_t structIndex;
    Guid     id;
    uint16_t instanceIndex;
    uint16_t structSize;
};

// One table of section 2, in file order. entrySize is 0 for the string pools and the data section.
struct Table { std::string name; uint64_t offset = 0, count = 0, entrySize = 0, bytes = 0; };

// Which validation refused the file (the first one that failed).
enum class Check {
    None,        // valid
    File,        // smaller than the header
    Totals,      // rules 1-3: tables + pools + data don't add up to the file size for any record size
    Structure,   // an index or range in the definition tables or records points outside its table
    RecordSize,  // rule 4: a record's structSize differs from its struct's computed size
    NameOffset,  // rule 5: a name (or record file name) offset isn't a string start in its pool
};
const char* CheckName(Check);

struct StructInfo {
    uint64_t size = 0;          // computed instance size (for an opaque struct: from its records, else 0)
    bool     opaque = false;    // rule 6: a field of unknown type or array kind, own, inherited or inline
    uint64_t instances = 0;     // over all mappings of this struct
    uint64_t blockOffset = 0;   // file offset of its first mapping's block (0 if it has none)
};

struct Schema {
    // Parses and validates `file`. The bytes must outlive the Schema and stay unchanged: names are
    // views into its string pools. Returns false with `failed` and `error` set when the layout is
    // refused; the fields below then hold whatever was read before the failing check (the header
    // once `hasHeader`, the tables once `recordSize` is non-zero).
    bool Parse(std::span<const uint8_t> file);

    Check       failed = Check::None;
    std::string error;

    bool     hasHeader = false;
    Header   header;
    uint64_t fileSize = 0;
    uint32_t recordSize = 0;            // derived: 32, 36 or 40
    std::vector<Table> tables;          // in file order, summing to fileSize
    uint64_t dataOffset = 0, dataSize = 0;

    std::vector<StructDef>   structs;
    std::vector<PropertyDef> properties;
    std::vector<EnumDef>     enums;
    std::vector<DataMapping> mappings;
    std::vector<Record>      records;
    std::vector<uint32_t>    enumOptions;     // name-pool offsets
    std::vector<StructInfo>  structInfo;      // per struct
    std::vector<uint64_t>    blockOffsets;    // per mapping

    std::string_view Name(uint32_t offset) const;          // name pool; "" when out of range
    std::string_view ValueString(uint32_t offset) const;   // value pool; "" when out of range
    std::string_view StructName(uint32_t index) const;
    size_t OpaqueCount() const;

    // Property indices of a struct, inherited first (root ancestor's properties, then down to its own).
    std::vector<uint32_t> Properties(uint32_t structIndex) const;
    // Lookups; the first entry wins when a name or GUID repeats. -1 / nullptr when absent.
    int64_t FindStruct(std::string_view name) const;
    const Record* FindRecord(const Guid& id) const;
    const Record* FindRecordByName(std::string_view name) const;

private:
    bool Refuse(Check check, std::string reason);
    std::span<const uint8_t> file_;
    uint64_t valuePool_ = 0, namePool_ = 0;
    std::unordered_map<std::string_view, uint32_t> structByName_, recordByName_;
    std::unordered_map<std::string, uint32_t> recordByGuid_;
};

}  // namespace sco::datacore
