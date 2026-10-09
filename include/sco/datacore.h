#pragma once
// sco::datacore: the game's DataCore database (Data\Game2.dcb). Schema parses the file's tables from
// bytes in memory and validates the layout as docs/design/vfs-datacore.md section 4 describes: every
// size is derived from the header and must add up to the file size; the version number is never
// trusted. Patch turns semantic overrides (a record's field, a new instance, a pointer, an array
// element, a new record) into base-offset splices for sco::vfs (section 4, "API" and "AddRecord").
// Standard library only; no engine, no Windows. A Schema is plain data: concurrent const use is fine.
#include "sco/vfs.h"
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
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
    uint32_t unknown;     // record +8 (records over 32 bytes): a name-pool offset, the owning team's tag (R1); 0 for 32-byte records
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
    NameOffset,  // rule 5: a name, record file name or record +8 offset isn't a string start in its pool
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
    std::span<const uint8_t> File() const { return file_; }   // the bytes Parse read

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

// Capability names (sco/caps.h) for what a parsed file supports. sco-core's pak adapter (plan PR 7)
// sets them; sco-dcb info prints them. datacore.add_record is on whenever layout validation passes
// (research R1: record +8 is checked by rule 5, and nothing indexes records by position).
constexpr const char* kCapPatch = "datacore.patch";
constexpr const char* kCapAddRecord = "datacore.add_record";
inline bool PatchSupported(const Schema& s) { return s.failed == Check::None && s.recordSize != 0; }
inline bool AddRecordSupported(const Schema& s) { return PatchSupported(s); }

// ---- patcher (design section 4) ----------------------------------------------------------------

// Why an operation (or Emit) was refused. Tests and callers act on the category; the message is
// for people ("record \"ShipA\" field \"speedX\": no property \"speedX\" in Ship").
enum class Refusal : uint8_t {
    None,
    Layout,             // the base file failed validation: nothing applies
    BadArgument,        // malformed field path, empty record reference, bad instance id or splice
    RecordNotFound,     // neither the GUID nor the name names a record
    StructNotFound,     // a type name names no struct
    FieldNotFound,      // a path step names no property, or can't be followed (null, weak, reference)
    IndexOutOfRange,    // name[3] past the end, or name[Type] matching no element
    TypeMismatch,       // the value (or operation) doesn't fit the field's type: nothing is written
    ValueOutOfRange,    // a number too large for the field
    UnknownEnumOption,  // not an option of the field's enum
    Opaque,             // the struct has a field of unknown type (validation rule 6)
    Unsupported,        // appending to a struct with no data block; AddRecord: a struct with no records,
                        // or records in the target file that disagree on record +8
    Duplicate,          // AddRecord: the record name or GUID already exists
    DependencyFailed,   // uses an instance whose AddInstance failed
    Corrupt,            // the file's data points outside its pools or blocks
    Limit,              // a count or the value-string pool would pass 32 bits
    Revalidation,       // Emit: the patched file fails the parser
};
const char* RefusalName(Refusal);

struct Status {
    Refusal     category = Refusal::None;
    std::string message;
    bool ok() const { return category == Refusal::None; }
    explicit operator bool() const { return ok(); }
};

// An instance in a struct's data block: (struct index, index in that struct's instances), as a
// strong or weak pointer stores it. Default-constructed it is invalid: a failed AddInstance leaves
// it so, and every operation using it is refused with DependencyFailed.
struct InstanceId {
    static constexpr uint32_t kNone = 0xFFFFFFFFu;
    uint32_t structIndex = kNone, index = kNone;
    bool valid() const { return structIndex != kNone; }
    bool operator==(const InstanceId&) const = default;
};

// A record by GUID (preferred) or name (fallback, and the readable alias in messages).
struct RecordRef {
    std::optional<Guid> guid;
    std::string        name;
};

// Where AddInstance copies from: a record and a field path resolving to an instance (the record's
// root for an empty path; strong pointers are followed). No record: the new instance is zero-filled.
struct InstanceSource {
    std::optional<RecordRef> record;
    std::string              field;
};

// A new top-level record (design section 4, "AddRecord"). Appended at the end of the record table;
// its root instance is a copy of the clone's root, appended to the struct's block; name and file path
// are appended to the end of their pools (a path already in the file reuses its offset).
struct NewRecord {
    std::string         type;       // struct name; it must already have records
    std::string         name;       // unique among records
    std::string         filePath;   // libs/foundry/records/...xml; "" = libs/foundry/records/sco/<packId>/<name>.xml
    std::optional<Guid> guid;       // none: a random version-4 GUID (PatchOptions::guidSeed)
    InstanceSource      clone;      // required: a record (empty field) of the same struct
};
struct AddedRecord {
    Guid       guid;                // as written (generated or given)
    InstanceId root;                // its root instance, usable as a pointer value
    uint32_t   index = 0;           // position in the record table (the old record count, plus earlier adds)
};

struct Value {
    enum class Kind : uint8_t { Null, Bool, Int, UInt, Float, String, Guid, Enum, Instance, Record };
    Kind        kind = Kind::Null;   // Null: a null strong or weak pointer, or a null reference
    bool        b = false;
    int64_t     i = 0;
    uint64_t    u = 0;
    double      f = 0;               // float and double fields (Int and UInt fit them too)
    std::string s;                   // String; Enum: the option name
    Guid        guid;
    InstanceId  instance;            // Instance: a pointer target, or the element to copy into an array of structs
    RecordRef   record;              // Record: a reference target (an existing record or one this patch added)

    static Value OfBool(bool v) { Value x; x.kind = Kind::Bool; x.b = v; return x; }
    static Value OfInt(int64_t v) { Value x; x.kind = Kind::Int; x.i = v; return x; }
    static Value OfUInt(uint64_t v) { Value x; x.kind = Kind::UInt; x.u = v; return x; }
    static Value OfFloat(double v) { Value x; x.kind = Kind::Float; x.f = v; return x; }
    static Value OfString(std::string v) { Value x; x.kind = Kind::String; x.s = std::move(v); return x; }
    static Value OfGuid(const Guid& v) { Value x; x.kind = Kind::Guid; x.guid = v; return x; }
    static Value OfEnum(std::string option) { Value x; x.kind = Kind::Enum; x.s = std::move(option); return x; }
    static Value OfInstance(InstanceId v) { Value x; x.kind = Kind::Instance; x.instance = v; return x; }
    static Value OfRecord(RecordRef v) { Value x; x.kind = Kind::Record; x.record = std::move(v); return x; }
};

struct PatchOptions {
    bool atomic = true;   // Emit refuses the whole batch if any operation was refused (design: per pack)
    std::string packId = "sco";            // AddRecord's default file path: libs/foundry/records/sco/<packId>/<name>.xml
    std::optional<uint64_t> guidSeed;      // AddRecord's GUID generator (std::mt19937_64); none: std::random_device
};

struct OpReport {
    std::string op;       // "OverrideField record \"ShipA\" speed"
    Status      status;
};

// One batch of semantic overrides against one parsed file. Field paths walk from a record's root
// instance (or an added instance): `name` (a property, inherited ones included), `name[3]` (array
// element), `name[Type]` (the first element whose struct is Type or derives from it); inline structs
// and strong pointers are followed, weak pointers and references are not.
//
// References are written as the target record's root instanceIndex (u32) and its GUID, never the
// record's position (research R1); a null reference is 0xFFFFFFFF and a zero GUID. Records added by
// AddRecord are addressable by name and GUID in later operations of the same batch.
//
// Every operation is checked in full before it changes anything: a refused one leaves the patch as
// it was and is recorded in Reports(). Nothing existing is renumbered: values are overwritten in
// place, and instances, array copies and strings are appended to the end of their block or pool.
// The Schema (and its bytes) must outlive the Patch. Not thread-safe; const use is.
class Patch {
public:
    explicit Patch(const Schema& base, PatchOptions options = {});
    ~Patch();
    Patch(Patch&&) noexcept;
    Patch& operator=(Patch&&) noexcept;

    // Scalars, strings, enums (by option name), locales, guids, pointers (Instance or Null) and
    // references (Record or Null), in place.
    Status OverrideField(const RecordRef& rec, std::string_view fieldPath, const Value& v);
    Status OverrideField(InstanceId inst, std::string_view fieldPath, const Value& v);
    // A new instance at the end of struct `type`'s block, copied from `cloneFrom` (an instance of
    // exactly that struct) or zero-filled. `out` is usable as a pointer value; invalid on refusal.
    Status AddInstance(std::string_view type, const InstanceSource& cloneFrom, InstanceId& out);
    // Points a strong or weak pointer field (or pointer array element) at an instance of its type.
    Status SetPointer(const RecordRef& rec, std::string_view fieldPath, InstanceId target);
    Status SetPointer(InstanceId inst, std::string_view fieldPath, InstanceId target);
    // Appends an element to an array field: the existing elements and the new one are copied to the
    // end of the pool (or, for an array of structs, the struct's block; `v` is then the instance to
    // copy), unless the array already ends there. The old range is left unreferenced.
    Status AppendElement(const RecordRef& rec, std::string_view arrayPath, const Value& v);
    Status AppendElement(InstanceId inst, std::string_view arrayPath, const Value& v);
    // A new record (design "AddRecord"): checks every rule, then appends the record entry, its root
    // instance (cloned), its name and (unless the path exists) its file path. Record +8 (records over
    // 32 bytes) is the value of the records already in that file, else the clone's (research R1).
    Status AddRecord(const NewRecord& rec, AddedRecord& out);
    // An existing instance (record root, array element or strong pointer target) as a pointer target.
    Status FindInstance(const InstanceSource& source, InstanceId& out) const;

    // Turns the accepted operations into base-offset splices (sorted, non-overlapping, each with its
    // expected old bytes; the header rewritten as one 120-byte overwrite when a count changes), then
    // re-validates: applies them to the base and re-parses the result, and checks every added record
    // (record count, entry size, root instance struct and size). Any failure leaves `out` empty.
    // With options.atomic, a batch with a refused operation is refused here with that operation's
    // category.
    Status Emit(std::vector<vfs::Splice>& out) const;

    const std::vector<OpReport>& Reports() const;   // one per operation, in call order

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Applies splices (as Emit returns them) to a base held in memory through sco::vfs's Compose and
// Reader, checking each splice's expected old bytes. For tests and tools; the game reads through
// sco::vfs mounts instead.
Status ApplySplices(std::span<const uint8_t> base, std::span<const vfs::Splice> splices, std::vector<uint8_t>& out);

}  // namespace sco::datacore
