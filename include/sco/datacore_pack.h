#pragma once
// sco::datacore data packs: the `.toml` override files of a data pack's datacore\ folder
// (docs/design/vfs-datacore.md section 5, docs/datacore.md "Pack format"), parsed into patcher
// operations and applied in plugin order with per-pack atomicity, a per-pack report and logged
// conflicts. ParsePack is the only code that reads TOML (vendored toml++, no exceptions); the rest is
// plain data over sco::datacore::Patch. Standard library only otherwise; no engine, no Windows.
#include "sco/datacore.h"
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace sco::datacore {

constexpr int    kPackFormat = 1;                 // `format = 1`: the file format, not a game version
constexpr size_t kMaxPackBytes = 4 * 1024 * 1024; // per .toml file
constexpr size_t kMaxPackOps = 65536;             // operations per file ([[instance]] set entries count)

// What a pointer is set to: `pointer = "@id"` (an instance this pack added), `pointer = "null"`, or
// `pointer = { record = "...", field = "..." }` (an existing instance).
struct PackPointer {
    enum class Kind : uint8_t { None, Null, Local, Existing };
    Kind           kind = Kind::None;
    std::string    local;      // Local: the [[instance]] id, without '@'
    InstanceSource existing;   // Existing
};

struct PackOp {
    enum class Kind : uint8_t { Set, Instance, Append };
    Kind     kind = Kind::Set;
    uint32_t line = 0;                      // of its [[...]] header; 0 when built in code
    // Set, Append: the target, a record (`record` name and/or `guid`) or an added instance (`instance = "@id"`).
    std::optional<RecordRef> record;
    std::string instance;                   // without '@'
    std::string field;
    // Set, Append: one of `value`, `pointer`, or for Append `element = "@id"` / `value = { struct = ... }`.
    std::optional<Value> value;             // Bool, Int, UInt, Float, String, Guid, Enum
    PackPointer pointer;
    std::string element;                    // Append: an added instance to copy into an array of structs
    // Instance (`id`, `struct`, optional `clone`, optional `set`); Append with an inline
    // `value = { struct = ..., clone = ..., set = ... }` uses type, clone and sets for its element.
    std::string id, type;
    InstanceSource clone;                   // no record: zero-filled
    std::vector<std::pair<std::string, Value>> sets;
};

struct Pack {
    std::string plugin;        // the owning plugin id; its position in plugin order is the pack's priority
    std::string name;          // "datacore/drive.toml"; "pending" for a saved sco.datacore patch
    bool        atomic = true; // `atomic = false` opts out of per-pack atomicity
    std::vector<PackOp> ops;   // file order
};

// Parses one .toml file. On failure: false, `out` empty, and `error` the first problem with its
// line, "12: [[set]]: unknown key \"feild\"". Checked here, without a game file: TOML syntax, known
// keys, value shapes, field path syntax, GUID syntax, `@id` defined (by an earlier [[instance]])
// before use and unique, a field set twice by one file (same target, same path), the size and
// operation limits. Names (records, structs, fields) resolve only against a .dcb (ApplyPacks).
bool ParsePack(std::string_view text, Pack& out, std::string& error);

// The canonical .toml for a pack, `comment` lines first (each prefixed "# "). ParsePack of the
// result gives the same operations back (lines aside). Values that TOML can't hold as they are
// (uint64 past int64) use the `{ uint = "..." }` form.
std::string WritePack(const Pack& pack, std::string_view comment = {});

// ---- applying ----------------------------------------------------------------------------------

enum class PackState : uint8_t {
    Applied,   // every operation applied
    Partial,   // atomic = false and some operations were skipped
    Refused,   // nothing applied: an operation failed in an atomic pack, or the whole patch failed
};
const char* PackStateName(PackState);   // "applied", "partial", "refused"

struct PackOpReport {
    uint32_t    line = 0;
    std::string op;            // "OverrideField record \"ShipA\" field \"speed\""
    Status      status;        // its own result; in a Refused pack the others are not applied either
};

struct PackReport {
    std::string plugin, name;
    bool        atomic = true;
    PackState   state = PackState::Applied;
    size_t      applied = 0, skipped = 0;
    std::string reason;                  // Refused, Partial: the first failure ("line 12: ...")
    std::vector<PackOpReport> ops;       // one per patcher operation, in order
};

struct PackResult {
    Status status;                       // Emit's. Not ok: nothing applies and every pack is Refused
    std::vector<vfs::Splice> splices;    // for sco::vfs (empty when nothing changes)
    std::vector<PackReport> packs;       // in input order
    std::vector<std::string> conflicts;  // "record \"ShipA\" field \"speed\": a (datacore/x.toml:4) overridden by b (...:2)"
};

// Applies packs in the given order, which is plugin order (folder-name order, built-ins first; a
// plugin's own .toml files in name order, then its saved patch). Each pack is tried on a copy of
// everything accepted so far and kept or dropped as a whole (atomic) or per operation (atomic =
// false). A later pack wins a field an earlier one set, and the conflict is listed; a pack that
// sets one field twice (by two paths) has that operation refused. Then one Emit for everything.
PackResult ApplyPacks(const Schema& base, std::span<const Pack> packs);

// "[datacore] 3 packs: gladius_qt 12/12 applied; ui_tweaks 40/41 applied (1 skipped: line 9: ...);
// old_mod refused (line 3: ...)". A plugin with several files is named plugin/file.
std::string Summary(const PackResult& result);

}  // namespace sco::datacore
