# DataCore files and packs: `sco-dcb`

`sco-dcb` reads the game's DataCore database (`Data\Game2.dcb`) with `sco::datacore`. `info` prints what the parser sees: the header, the tables with their offsets and sizes, the derived record size, and whether the layout passes validation. `records` lists every record, the input for research step R1 of the [DataCore design](design/vfs-datacore.md#addrecord). For data packs, `lint` checks `.toml` override files without the game, `check` resolves them against a real `Game2.dcb`, `show` prints a record's fields as the paths overrides use, and `diff` turns the difference between two files into a pack ([Pack format](#pack-format)). `patch` applies a batch of patcher operations given on the command line and writes the patched file somewhere else, to check the patcher on a real file. It is a host tool, like [`sco-sigcheck`](sigcheck.md): built with sco-core, never shipped to players, and it only reads the game's file.

The layout and the validation rules are in the design, [section 2](design/vfs-datacore.md#2-the-datacore-binary-layout) and [section 4](design/vfs-datacore.md#validation-all-must-hold-else-the-patcher-refuses-the-file). In short: every size is derived from the header, the record entry size is the one of 32, 36 or 40 bytes for which the tables, pools, string pools and the data the mappings need add up exactly to the file size, records must agree with their struct's computed size, and names (record +8 included, research R1) must land on string starts. The version number is never trusted.

## Getting `Game2.dcb`

The file is an entry in `Data.p4k` (zip64, Zstandard-compressed, not encrypted). sco-core vendors no zstd ([decision 8](design/vfs-datacore.md#decisions-maintainer-2026-10-09)); use an external extractor. With [unp4k](https://github.com/dolkensp/unp4k) (a release that handles ZStd entries), from a new empty folder outside any repository:

```bat
mkdir C:\dcb
cd /d C:\dcb
unp4k.exe "E:\Games\Star Citizen\StarCitizen\LIVE\Data.p4k" Game2.dcb
```

It prints `ZStd | Plain | Data/Game2.dcb` and writes `C:\dcb\Data\Game2.dcb` (331,932,921 bytes for 4.10.193). The filter is a substring match, so this takes only that entry. The game folder is only read.

**Never commit game data**, in this repository or any other: no `.dcb`, no extracts, no record lists.

## Run it

CMake builds `sco-dcb` with everything else ([Building](building.md)):

```sh
cmake -S . -B build -A x64 && cmake --build build --config RelWithDebInfo --target sco-dcb
build/RelWithDebInfo/sco-dcb info C:/dcb/Data/Game2.dcb
build/RelWithDebInfo/sco-dcb records C:/dcb/Data/Game2.dcb > C:/dcb/records.tsv
```

## `info`

On 4.10.193 (`Game2.dcb` from the LIVE `Data.p4k`, 2026-10-09):

```text
file: C:/dcb/Data/Game2.dcb (331932921 bytes)
header: version 8, unknown +0 0, +8 0 0 0 0
counts: 6694 structs, 23789 properties, 774 enums, 6694 mappings, 117022 records, 6357 enum options
values: bool 45996, int8 10, int16 10, int32 774, int64 10667, uint8 10, uint16 10, uint32 36, uint64 10, float 76807, double 10, guid 159, string 100731, locale 6837, enum 144578, strong 1817970, weak 184395, reference 748905
string pools: values 17183397 bytes, names 7195714 bytes
record size: 36 bytes (derived from the totals; the header has no field for it)

table                        offset      count  entry        bytes
header                            0          1    120          120
structs                         120       6694     16       107104
properties                   107224      23789     12       285468
enums                        392692        774      8         6192
mappings                     398884       6694      8        53552
records                      452436     117022     36      4212792
values: int8                4665228         10      1           10
...
values: enum options       37115396       6357      4        25428
value strings              37140824          -      -     17183397
name strings               54324221          -      -      7195714
data                       61519935       6694      -    270412986

structs: 6694, 0 opaque (a field of unknown type: overrides into them are refused)
capabilities: datacore.patch on, datacore.add_record on
layout: OK
```

The data row's count is the number of mappings (one block each). A refused file prints as much of this as was read, then `layout refused (<check>): <reason>`, for example `layout refused (totals): layout: records don't add up (game format changed): ...` with the data sizes each record size would leave.

## `records`

One tab-separated line per record, after a `#` header line:

| Column | What |
|---|---|
| `index` | Position in the record table |
| `guid` | The record's id, in the string form unp4k prints and `guid\|path` values use |
| `name`, `struct` | Record name and its struct's name (name pool) |
| `unknown` | The `u32` at record +8 (36-byte records), in hex: what R1 measured, a name-pool offset. `0x00000000` for 32-byte records |
| `instance`, `structSize` | The root instance in the struct's block, and the size the record states |
| `file` | The record's file path (value pool) |
| `tag` | The name-pool string at record +8: the team that owns the record's file (`SystemsDesign`, `Unknown`, ...). Empty for 32-byte records |

## Packs: `lint`, `check`, `show`, `diff`

```sh
sco-dcb lint <pack>...                    # syntax and shape; no game file
sco-dcb check Game2.dcb <pack>...         # every override resolved: OK / SKIP per operation, a verdict per pack
sco-dcb show Game2.dcb <record> [field]   # the record's values as field paths, to write overrides from
sco-dcb diff a.dcb b.dcb > changes.toml   # what changed from a to b, as a pack
```

A `<pack>` is a pack folder (its `datacore/*.toml` files in name order; the folder name is the plugin id) or one `.toml` file. Several packs are applied in the order given, which stands for plugin order, so `check` also shows conflicts between them. A `<record>` is its name or its GUID (`guid:` prefix optional).

`check` is the patch-day routine for data mods. On 4.10.193 (`Game2.dcb` from the LIVE `Data.p4k`, 2026-10-09), the SDK's sample pack ([`sdk/examples/quantum_pack`](../sdk/examples/quantum_pack/datacore/eos.toml)):

```text
$ sco-dcb check C:/dcb/Data/Game2.dcb sdk/examples/quantum_pack
C:/dcb/Data/Game2.dcb: 331932921 bytes, 117022 records, layout OK

pack quantum_pack datacore/eos.toml (atomic): APPLIED 6/6
  line 8: OK   OverrideField record "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" field "Components[SCItemQuantumDriveParams].params.spoolUpTime"
  line 14: OK   OverrideField record "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" field "Components[SCItemQuantumDriveParams].params.cooldownTime"
  line 21: OK   AddInstance struct "SCItemQuantumDriveParams"
  line 21: OK   OverrideField instance 63 of SCItemQuantumDriveParams field "params.driveSpeed"
  line 21: OK   OverrideField instance 63 of SCItemQuantumDriveParams field "params.spoolUpTime"
  line 28: OK   SetPointer record "EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem" field "Components[SCItemQuantumDriveParams]"

emit: OK, 5 splices, the patched file re-validated
[datacore] 1 pack: quantum_pack 6/6 applied
```

It took 0.37 s, parse and re-validation of the whole patched file included. A pack naming a field the struct doesn't have is refused as a whole (it is atomic), with the reason, and `check` exits 1:

```text
pack badpack datacore/typo.toml (atomic): REFUSED 0/2
  line 3: OK   OverrideField record "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" field "Components[SCItemQuantumDriveParams].params.spoolUpTime" (not applied: the pack is refused)
  line 8: SKIP record "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" field "Components[SCItemQuantumDriveParams].params.spoolTime": no property "spoolTime" in SQuantumDriveParams (field not found)
  -> line 8: record "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" field "Components[SCItemQuantumDriveParams].params.spoolTime": no property "spoolTime" in SQuantumDriveParams
```

`show` follows inline structs and strong pointers and lists arrays element by element. Values print in pack syntax (`2.5`, `"text"`, `{ enum = "QuantumDrive" }`, `{ guid = "..." }`), pointers as `-> Struct[index]`, `weak -> ...` or `null`, references as `{ ref = "guid:..." }` followed by the record's name as a comment:

```text
$ sco-dcb show Game2.dcb EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem "Components[SCItemQuantumDriveParams]"
Components[SCItemQuantumDriveParams] = -> SCItemQuantumDriveParams[6]
  Components[SCItemQuantumDriveParams].params.driveSpeed = 1.992727e+08
  Components[SCItemQuantumDriveParams].params.cooldownTime = 10.8
  ...
  Components[SCItemQuantumDriveParams].params.spoolUpTime = 5.1
```

`diff` expects b to be a with values changed and things appended, as the patcher writes them: the same struct and property definitions. It walks every record of b from its root (matched to a by GUID), compares values by path, and prints `[[set]]`, `[[instance]]` and `[[append]]` blocks. New instances are written zero-filled and then fully set; arrays that grew get appends. A record only in b becomes a `[[record]]` (its name, GUID and file as in b) cloned from the record of a, of the same struct, whose values differ least (records in the same file tried first, at most 256), followed by `[[set]]`s for the values that still differ; new records come first in the output, so later operations can point at them. What it can't express (a new record of a struct a has no record of, a shrunk array, a reference field, an existing instance no record path reaches) is listed as `# not converted:` comments and makes it exit 1. Paths it writes are index-based (`Components[1]`), so edit them to `[Type]` selectors before keeping the pack. The output is checked to parse before it is printed.

## Pack format

A data pack's `datacore\*.toml` files (content kind `datacore`) hold overrides. Example: the SDK's [`quantum_pack`](../sdk/examples/quantum_pack/datacore/eos.toml).

```toml
format = 1                 # required: the file format, not a game version
atomic = true              # default; false lets each operation apply on its own

[[set]]                    # a value in place
record = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem"   # record name, and/or:
guid   = "08a5bfdb-1972-421f-83fe-be03b7ac5222"           # preferred when given; the name is then for messages
field  = "Components[SCItemQuantumDriveParams].params.spoolUpTime"
value  = 3.5

[[instance]]               # a new instance, named in this file
id     = "eos_fast"
struct = "SCItemQuantumDriveParams"
clone  = { record = "...", field = "Components[SCItemQuantumDriveParams]" }   # optional; else zero-filled
set    = { "params.driveSpeed" = 2.5e8 }                                       # optional

[[set]]
record  = "EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem"
field   = "Components[SCItemQuantumDriveParams]"
pointer = "@eos_fast"      # or "null", or { record = "...", field = "..." } for an existing instance

[[append]]                 # one element at the end of an array
record = "..."
field  = "effects"
value  = { struct = "SEntityEffectSystem_ParticleTagEffect", clone = { record = "...", field = "..." }, set = { ... } }
# or value = <scalar> for arrays of values, pointer = ... for arrays of pointers,
#    element = "@id" for an array of structs (the instance is copied in)

[[record]]                 # a new top-level record (design "AddRecord")
id     = "eos_sco"          # optional: "@eos_sco" is its root instance in later operations
struct = "EntityClassDefinition"
name   = "EntityClassDefinition.QDRV_SCO_Example_SCItem"   # unique among records
guid   = "..."              # optional; default: derived from the plugin id and the name, stable
clone  = { record = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem" }   # required: a record of the same struct
file   = "libs/foundry/records/...xml"   # optional; default libs/foundry/records/sco/<plugin id>/<name>.xml
set    = { "..." = 1.0 }    # optional: values on the new record's root

[[set]]                    # later operations name it like any record (or use instance = "@eos_sco")
record = "..."
field  = "someReference"
value  = { ref = "EntityClassDefinition.QDRV_SCO_Example_SCItem" }
```

| Key | In | Meaning |
|---|---|---|
| `record`, `guid` | `[[set]]`, `[[append]]`, `clone`, `pointer` | The record: by name, by GUID (the form `sco-dcb records` and `show` print), or both (GUID first, name as fallback) |
| `instance = "@id"` | `[[set]]`, `[[append]]` | Instead of a record: an instance an earlier `[[instance]]` of this file added |
| `field` | all | A field path: `name`, `name[3]`, `name[Type]` (first element of that struct or a derived one), joined with `.`; through inline structs and strong pointers |
| `value` | `[[set]]`, `[[append]]` | A number (integer or float; floats and doubles take either), a string (also locales and enums), `true`/`false`, `{ guid = "..." }`, `{ enum = "Option" }`, `{ ref = "RecordName" }` or `{ ref = "guid:..." }` (a reference field's target record), `{ uint = "18446744073709551615" }` (uint64 past int64). `pointer = "null"` clears a reference too |
| `pointer` | `[[set]]`, `[[append]]` | `"@id"`, `"null"`, or `{ record = ..., field = ... }` naming an existing instance |
| `element` | `[[append]]` | `"@id"`: an added instance copied into an array of structs |
| `id`, `struct`, `clone`, `set` | `[[instance]]` | Local name (letters, digits, `_`, `-`; used as `"@id"`), the struct, the instance to copy, and field values (`"path" = value`; an unquoted dotted key is a path too) |
| `struct`, `name`, `clone` | `[[record]]` | Required: the new record's struct (one that already has records), its name (unique among the file's records and every record added before it), and the record whose root is copied (`{ record = ... }` and/or `guid`, no `field`; the same struct) |
| `id`, `guid`, `file`, `set` | `[[record]]` | Optional: a local name shared with `[[instance]]` ids (`"@id"` is the record's root: a `pointer`, an `instance` target, an `element`); the GUID (not zero, not in the file; default derived from plugin id and name, so it is the same at every launch and saved games that store it still find it); the file path (`libs/foundry/records/` ... `.xml`; default `libs/foundry/records/sco/<plugin id>/<name>.xml`); values on its root |

**Records.** A `[[record]]` follows the patcher's AddRecord rules ([design](design/vfs-datacore.md#addrecord), research R1): the record is appended at the end of the record table, its root instance is a copy of the clone's root, its name and file path are appended to their pools, and record +8 (the owning team's tag) is taken from the records already in that file, else from the clone. The copy is shallow: strong pointers and arrays in the root still share the clone's sub-objects and array storage, so give the new record its own sub-object with a `pointer` to a new `[[instance]]` rather than setting a field through a shared path (that would change the clone too). Later operations of the same file and of later packs name it by `name` or `guid` like any record, `{ ref = "<name>" }` points a reference field at it, and `"@id"` addresses its root. Refusals at the load: `no struct "..."`, `struct "..." has no records`, `record name "..." already exists`, `guid ... already exists (record "...")`, `clone must be a record of the same struct`, `bad file path`, and R1's `records in file "..." disagree on record +8` / `record +8 of clone "..." is not a name-pool string`.

**Order and priority.** Operations run in file order, whatever their kind. Packs apply in plugin order (folder-name order, built-ins first), a plugin's files in name order. A later pack wins a field an earlier one set; the conflict is logged with both sources (`... alpha (datacore/a.toml:3) overridden by beta (datacore/b.toml:3) (later in plugin order)`). There is no `priority` key (design decision 7). Within one file a field may be set once: the same target and path twice is a lint error, and two paths to the same field are caught when the pack applies (the later operation is refused).

**Atomicity.** By default a file applies whole or not at all: one failing operation (record or field not found, a value that doesn't fit, ...) refuses the file, with that reason, and the other packs still apply. With `atomic = false` each operation applies on its own; an `[[instance]]` with its `set` table is one operation, and anything using an `@id` whose instance failed is refused too. The game reads either its own bytes or a fully re-validated patched file.

**Checked without the game** (`lint`, and `sco-host-sim` for every indexed pack): TOML syntax, `format = 1`, known keys, value shapes, field path and GUID syntax, `@id` defined before use and only once, a field set twice, a `[[record]]`'s required keys, its clone being a record (no `field`), its file path rule and a name or GUID it adds twice, at most 4 MiB and 65,536 operations per file. Names resolve only against a real file: `check`.

## The `sco.datacore` service

Plugins (native C, C++ through `scosdk/datacore.hpp`, Lua through `sco.datacore`) queue the same operations a pack's `.toml` holds, from code: computed overrides, or overrides that depend on settings. Fixed ones belong in a data pack. It is a **host-owned service** (`sco.datacore`, version 1.1, [`sco_datacore.h`](../include/sco_datacore.h), pinned by `tests/abi_datacore.c`), found with `query_service`; `sco_api.h` is unchanged. The host publishes it **only when the product enables it**: `sco::app::Platform::dataCore` with a `dataRoot`. sc-offline turns it on with its CryPak adapter (design plan PR 8); until then `query_service` answers `SCO_NOT_FOUND` and Lua's `sco.datacore` is `nil`.

```c
const sco_datacore_v1* dc = NULL;
uint64_t patch = 0, fast = 0;
if (api->query_service(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, (const void**)&dc) == SCO_OK &&
    dc->begin(self, 0, &patch) == SCO_OK) {
    sco_dc_value v = { sizeof v, SCO_DC_FLOAT };
    v.f = 3.5;
    dc->set(patch, "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem", "Components[SCItemQuantumDriveParams].params.spoolUpTime", &v);
    dc->add_instance(patch, "SCItemQuantumDriveParams", "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem",
                     "Components[SCItemQuantumDriveParams]", &fast);
    char at[24];
    snprintf(at, sizeof at, "@%llu", (unsigned long long)fast);  /* a field of the added instance */
    v.f = 2.5e8;
    dc->set(patch, at, "params.driveSpeed", &v);
    dc->set_pointer(patch, "EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem", "Components[SCItemQuantumDriveParams]", fast);
    dc->commit(patch);
}
```

| Function | Does |
|---|---|
| `state()` | `SCO_DC_OPEN` before the game's DataCore load, `SCO_DC_LOADED` after it |
| `begin(self, flags, &patch)` | A patch owned by `self`; `SCO_DC_NON_ATOMIC` lets operations apply one by one. At most 64 open or queued per plugin |
| `set(patch, record, field, value)` | A value in place; `SCO_DC_NULL` (or a NULL value) a null pointer; `SCO_DC_INSTANCE` points at an added instance. A field once per patch |
| `add_instance(patch, type, clone_record, clone_field, &id)` | A new instance, cloned or zero-filled |
| `set_pointer(patch, record, field, id)` | A pointer at an added instance |
| `append(patch, record, field, value)` | One array element: a value, a pointer, or (arrays of structs) an added instance copied in |
| `add_record(patch, type, name, guid, clone_record, file_path, &id)` | 1.1: a new record, a `[[record]]` in pack terms: cloned from `clone_record` (required), `guid` and `file_path` optional (NULL: the pack defaults, a GUID derived from plugin id and name and `libs/foundry/records/sco/<plugin id>/<name>.xml`). `id` is its root, used like an added instance's; later operations also name the record by name or GUID. A 1.0 host answers `SCO_UNAVAILABLE` |
| `commit(patch)` | Before the load: queued for it. After it: saved (below). No more operations on it |
| `discard(patch)` | Drops it (open, or queued before the load) |
| `report(patch, i, &out)` | One entry per operation in call order, then one for the patch (`op_index` `SCO_DC_OP_PATCH`): `SCO_DC_QUEUED`, `APPLIED`, `SKIPPED` (this operation failed) or `REFUSED` (the patch was refused whole), with the reason |

`record` is a record name, `"guid:xxxxxxxx-..."`, or `"@<id>"` for an instance (or record) this patch added (its own fields). `field` is a path as in packs. Values: `SCO_DC_BOOL`, `INT`, `UINT`, `FLOAT`, `STRING` (UTF-8; also enums by option name), `GUID`, `ENUM`, `REF` (a reference field's target record, by name or `guid:...`), `NULL`, `INSTANCE`. Call-time checks need no game file: path and GUID syntax, value shapes, UTF-8, instance ids of the same patch, `add_record`'s file path rule and a record name or GUID the patch adds twice (`SCO_BAD_ARG`; nothing is queued). Names resolve at the load.

**Versions.** 1.1 has the same table as 1.0 and makes `add_record` work (in 1.0 it answered `SCO_UNAVAILABLE`). A caller that needs records queries `SCO_DATACORE_VERSION_1_1`: a 1.0 host then answers `SCO_UNAVAILABLE` at the query instead of at the call; querying `SCO_DATACORE_VERSION_1_0` takes either. The minor is bumped because an existing slot changed behavior in a way callers must be able to detect before relying on it; the layout (`tests/abi_datacore.c`) is unchanged.

**Timing (design decision 9).** In sc-offline the game loads DataCore before plugins load, so in practice a plugin's patch is committed after the load. It is never applied late. `commit` validates it, saves it in the pack format as `<dataRoot>/datacore/pending/<plugin id>.toml` (a temporary file renamed over the old one, so a crash leaves the old one), and reports `SCO_DC_QUEUED` with the reason `applies at the next launch`. At every later launch the load reads it right after that plugin's own data pack, with the same per-patch atomicity and report, until the plugin commits another patch after the load (which replaces it; an empty patch clears it) or the file is deleted. The saved patch of a plugin that is no longer installed, or is disabled, off, refused or crashed, is skipped and logged. Patches committed before a load (a product whose plugins load first) apply at it, after the plugin's saved patch.

**Order** (decision 7): plugins in plugin order (built-ins first, then folder-name order); within one plugin its pack's `.toml` files, then its saved patch, then its patches committed before the load. A later source wins a field, and the conflict is logged with both ids. **Ownership:** a plugin that unloads or crashes before the load loses its uncommitted and queued patches; their ids answer `SCO_NOT_FOUND`.

**Results.** After the load the host posts `datacore.applied` on the game thread (the next tick; after `game.ready` when the load came first) with a `sco_dc_applied { size; applied, skipped, refused; }` counting operations over every source. A plugin reads its own results with `report`. The load logs `[datacore] N packs: ...`, one line per skipped source and per conflict.

**Host side** ([`sco/datacore_service.h`](../include/sco/datacore_service.h)): `service::Start({ dataRoot })` and `Stop()` (the host kit calls them), and `service::Load(schema, pluginList, contentIndex, dataRoot)`, which the CryPak adapter (plan PR 7) calls at the `.dcb` open on the loader thread. It switches the state to `SCO_DC_LOADED`, takes the queued patches, reads every source in the order above, applies them with `ApplyPacks`, stores each patch's report and posts the event. Plugin calls take the service's lock briefly and never wait for the load. `tests/test_datacore_service.cpp` runs a simulated launch sequence: launch 1 loads (a data pack plus patches committed before it), a plugin commits after it, launch 2 applies the saved patch in order and wins the conflict, and launch 3, with the plugin uninstalled, skips it.

### From C++ and Lua

```cpp
#include "scosdk/datacore.hpp"                         // not in scosdk.hpp

sco::sdk::DataCore dc;
if (dc.Open(*this) == SCO_OK) {
    sco::sdk::DataCorePatch p = dc.Begin();            // discarded on destruction unless committed
    p.Set(eos, "Components[SCItemQuantumDriveParams].params.spoolUpTime", 3.5);
    sco::sdk::DataCoreInstance fast = p.AddInstance("SCItemQuantumDriveParams", eos, "Components[SCItemQuantumDriveParams]");
    p.Set(fast.Ref(), "params.driveSpeed", 2.5e8);
    p.SetPointer(beacon, "Components[SCItemQuantumDriveParams]", fast);
    sco::sdk::DataCoreInstance mine = p.AddRecord("EntityClassDefinition", "EntityClassDefinition.QDRV_SCO_Mine", eos);
    p.Commit();                                        // mine.Ref() and the name address the new record
}
```

```lua
if sco.datacore then                                   -- nil when the product doesn't publish it
  local p = sco.datacore.begin()                       -- or begin({ atomic = false })
  p:set(eos, "Components[SCItemQuantumDriveParams].params.spoolUpTime", 3.5)
  local fast = p:add_instance("SCItemQuantumDriveParams", eos, "Components[SCItemQuantumDriveParams]")
  p:set_pointer(beacon, "Components[SCItemQuantumDriveParams]", fast)
  p:add_record("EntityClassDefinition", "EntityClassDefinition.QDRV_SCO_Mine", eos)   -- [, guid [, file]]
  p:commit()
end
```

Details: [C++ SDK § DataCore](sdk-cpp.md#datacore), [Lua § sco.datacore](../sdk/docs/lua.md#scodatacore).

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The layout is valid (`info`, `records`, `show`); `patch`: the batch emitted, re-validated, and the output was written; every file parses (`lint`); every operation of every pack applies (`check`); everything converted (`diff`) |
| 1 | The layout is refused (the reason names the first failing check); a pack doesn't parse or doesn't fully apply; a record or field isn't found (`show`); some change couldn't be converted (`diff`); `patch`: an operation or `Emit` was refused, and nothing is written |
| 2 | Bad arguments, a file or pack folder that can't be read (or, for `patch`, written), or a file smaller than the 120-byte header |

## Patching

The patcher is a library API, `sco::datacore::Patch` ([api.md](api.md#patching-datacore)). Packs reach it through `sco::datacore::ApplyPacks` and `sco-dcb check` ([above](#packs-lint-check-show-diff)). `sco-dcb patch` runs it from the command line, to check it on a real file; it is a developer tool, and packs are the way to ship overrides.

```text
sco-dcb patch <in.dcb> <out.dcb> [--seed N] [--pack ID] [--non-atomic] <op>...
  set <record|inst:N> <field> <value>                  OverrideField
  append <record|inst:N> <field> <value>               AppendElement
  set-pointer <record|inst:N> <field> inst:N           SetPointer
  add-instance <struct> <clone record|-> <field|->     AddInstance; the Nth one is inst:N
  add-record <struct> <name> <clone record> <path|->   AddRecord; - is libs/foundry/records/sco/<pack>/<name>.xml
value: null, true, false, an integer, a number, record:<name> (a reference), inst:N, else a string or enum option
```

It prints one line per operation (`OK` or `REFUSED (<category>) <reason>`), then `Emit`, and writes the patched file only when the batch emitted and re-validated. `<out.dcb>` must be another file; the input is only read. `--seed` makes the GUIDs of added records reproducible (default: `std::random_device`), `--pack` sets the pack id of default record paths (default `sco-dcb`), `--non-atomic` emits the accepted operations when some are refused.

On 4.10.193 (`Game2.dcb` from the LIVE `Data.p4k`, 2026-10-09), one float override, one cloned instance, one cloned record, and a reference to the new record, then a refusal:

```text
$ sco-dcb patch Game2.dcb patched.dcb --seed 20261009 set Character.SHOPKEEP2_Gruff nicknameChance 2.5 add-instance ResourceType ResourceType.Electricity - add-record ResourceType ResourceType.SCO_AddRecordCheck ResourceType.Electricity - set AmmoParams.VehicleCounterMeasureFlares resourceType record:ResourceType.SCO_AddRecordCheck
base: Game2.dcb (331932921 bytes, 117022 records of 36 bytes)
capabilities: datacore.patch on, datacore.add_record on
op 1: OK OverrideField record "Character.SHOPKEEP2_Gruff" field "nicknameChance"
op 2: OK AddInstance struct "ResourceType": inst:1 = instance 207 of ResourceType
op 3: OK AddRecord record "ResourceType.SCO_AddRecordCheck" (ResourceType): record 117022, guid 809bb038-b753-41c0-950c-f6cfc73b74ba, root instance 208
op 4: OK OverrideField record "AmmoParams.VehicleCounterMeasureFlares" field "resourceType"
Emit: OK, 4 operations (0 refused), 8 splices (148 bytes replaced, 447 bytes written), re-validated
wrote patched.dcb (331933220 bytes)

$ sco-dcb info patched.dcb
counts: 6694 structs, 23789 properties, 774 enums, 6694 mappings, 117023 records, 6357 enum options
...
layout: OK

$ sco-dcb records patched.dcb      (two lines of 117,024)
88      bcc8cde9-...-37d4aaa507eb  ResourceType.Electricity           ResourceType  0x000918b3  0    81  libs/foundry/records/resourcetypedatabase/resourcetypedatabase.xml  SystemsDesign
117022  809bb038-...-f6cfc73b74ba  ResourceType.SCO_AddRecordCheck    ResourceType  0x000918b3  208  81  libs/foundry/records/sco/sco-dcb/ResourceType.SCO_AddRecordCheck.xml   SystemsDesign

$ sco-dcb patch Game2.dcb refused.dcb set Character.SHOPKEEP2_Gruff nicknameChanceX 2.5
op 1: REFUSED (field not found) record "Character.SHOPKEEP2_Gruff" field "nicknameChanceX": no property "nicknameChanceX" in Character
Emit: REFUSED (field not found) operation 0 refused, so the batch applies nothing: ...
nothing written
```

The new record is the last of the table, its name and path are at the end of their pools, and its record +8 is the clone's `SystemsDesign` (its path is new). The reference holds the new record's root instance (208) and its GUID.

Earlier, with a scratch program over the library (read-only, nothing committed), overriding one float field of one record by name:

```text
parsed 331932921 bytes, 117022 records, record size 36 (39 ms)
1 overrides (0 refused); first: record "Character.SHOPKEEP2_Gruff" field "nicknameChance" = 2.5
Emit: OK, 1 splices, 91 ms including re-validation
applied: 331932921 bytes; re-parse: layout: OK
splice 0: at 134562223, removes 4, adds 4; patched value reads 2.5
```

The same with one float override in each of 1,000 records: 1.9 ms for the operations, `Emit` 89 ms including re-validation (which composes and re-parses the whole patched file in memory), re-parse `layout: OK`.


## In game: `sco::game::pak`

`sco-dcb` works on a file on disk. In game, the engine reads `Game2.dcb` through CryPak, and `sco::game::pak` (`sco/game/pak.h`, library `sco_pak`) serves it from a `sco::vfs` mount while the DataCore loader runs. The tracked file gets the virtual bytes on the loader's thread; every other file and thread gets the engine's own. A mount that doesn't apply (expected bytes differ, a transform refuses) leaves the game loading its own data. The product enables it once after `sco::ResolveAll` (sc-offline: plan PR 8), and `sco-sigcheck` checks its `pak.*` rows on patch day. Each load ends with a `LoadReport` (outcome, path, sizes, reason, `durationMs`), which `LastLoad` returns and the optional `Options::onLoad` callback receives on the loader's thread, outside the adapter's lock. Reference: [API § sco/game/pak.h](api.md#scogamepakh-the-crypak-adapter).

## Patch-day routine

1. Extract `Game2.dcb` from the new `Data.p4k` (above).
2. `sco-dcb info Game2.dcb`.
3. Exit 0: the layout is still the one sco-core understands. Exit 1: the reason says which total or check broke; the format changed, and data overrides stay off until the parser follows.
4. `sco-dcb check Game2.dcb <pack>...` for every data pack: `SKIP` lines name each record or field that no longer resolves.
