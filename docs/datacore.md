# DataCore files and packs: `sco-dcb`

`sco-dcb` reads the game's DataCore database (`Data\Game2.dcb`) with `sco::datacore`. `info` prints what the parser sees: the header, the tables with their offsets and sizes, the derived record size, and whether the layout passes validation. `records` lists every record, the input for research step R1 of the [DataCore design](design/vfs-datacore.md#addrecord). For data packs, `lint` checks `.toml` override files without the game, `check` resolves them against a real `Game2.dcb`, `show` prints a record's fields as the paths overrides use, and `diff` turns the difference between two files into a pack ([Pack format](#pack-format)). It is a host tool, like [`sco-sigcheck`](sigcheck.md): built with sco-core, never shipped to players, read-only on every file.

The layout and the validation rules are in the design, [section 2](design/vfs-datacore.md#2-the-datacore-binary-layout) and [section 4](design/vfs-datacore.md#validation-all-must-hold-else-the-patcher-refuses-the-file). In short: every size is derived from the header, the record entry size is the one of 32, 36 or 40 bytes for which the tables, pools, string pools and the data the mappings need add up exactly to the file size, records must agree with their struct's computed size, and names must land on string starts. The version number is never trusted.

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
| `unknown` | The `u32` at record +8 (36-byte records), in hex: what R1 measures. `0x00000000` for 32-byte records |
| `instance`, `structSize` | The root instance in the struct's block, and the size the record states |
| `file` | The record's file path (value pool) |

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

`show` follows inline structs and strong pointers and lists arrays element by element. Values print in pack syntax (`2.5`, `"text"`, `{ enum = "QuantumDrive" }`, `{ guid = "..." }`), pointers as `-> Struct[index]`, `weak -> ...` or `null`, references as `ref {guid} (record name)`:

```text
$ sco-dcb show Game2.dcb EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem "Components[SCItemQuantumDriveParams]"
Components[SCItemQuantumDriveParams] = -> SCItemQuantumDriveParams[6]
  Components[SCItemQuantumDriveParams].params.driveSpeed = 1.992727e+08
  Components[SCItemQuantumDriveParams].params.cooldownTime = 10.8
  ...
  Components[SCItemQuantumDriveParams].params.spoolUpTime = 5.1
```

`diff` expects b to be a with values changed and things appended, as the patcher writes them: the same struct and property definitions. It walks every record of b from its root (matched to a by GUID), compares values by path, and prints `[[set]]`, `[[instance]]` and `[[append]]` blocks. New instances are written zero-filled and then fully set; arrays that grew get appends. What it can't express (a new record, a shrunk array, a reference field, an existing instance no record path reaches) is listed as `# not converted:` comments and makes it exit 1. Paths it writes are index-based (`Components[1]`), so edit them to `[Type]` selectors before keeping the pack. The output is checked to parse before it is printed.

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
```

| Key | In | Meaning |
|---|---|---|
| `record`, `guid` | `[[set]]`, `[[append]]`, `clone`, `pointer` | The record: by name, by GUID (the form `sco-dcb records` and `show` print), or both (GUID first, name as fallback) |
| `instance = "@id"` | `[[set]]`, `[[append]]` | Instead of a record: an instance an earlier `[[instance]]` of this file added |
| `field` | all | A field path: `name`, `name[3]`, `name[Type]` (first element of that struct or a derived one), joined with `.`; through inline structs and strong pointers |
| `value` | `[[set]]`, `[[append]]` | A number (integer or float; floats and doubles take either), a string (also locales and enums), `true`/`false`, `{ guid = "..." }`, `{ enum = "Option" }`, `{ uint = "18446744073709551615" }` (uint64 past int64) |
| `pointer` | `[[set]]`, `[[append]]` | `"@id"`, `"null"`, or `{ record = ..., field = ... }` naming an existing instance |
| `element` | `[[append]]` | `"@id"`: an added instance copied into an array of structs |
| `id`, `struct`, `clone`, `set` | `[[instance]]` | Local name (letters, digits, `_`, `-`; used as `"@id"`), the struct, the instance to copy, and field values (`"path" = value`; an unquoted dotted key is a path too) |

**Order and priority.** Operations run in file order, whatever their kind. Packs apply in plugin order (folder-name order, built-ins first), a plugin's files in name order. A later pack wins a field an earlier one set; the conflict is logged with both sources (`... alpha (datacore/a.toml:3) overridden by beta (datacore/b.toml:3) (later in plugin order)`). There is no `priority` key (design decision 7). Within one file a field may be set once: the same target and path twice is a lint error, and two paths to the same field are caught when the pack applies (the later operation is refused).

**Atomicity.** By default a file applies whole or not at all: one failing operation (record or field not found, a value that doesn't fit, ...) refuses the file, with that reason, and the other packs still apply. With `atomic = false` each operation applies on its own; an `[[instance]]` with its `set` table is one operation, and anything using an `@id` whose instance failed is refused too. The game reads either its own bytes or a fully re-validated patched file.

**Checked without the game** (`lint`, and `sco-host-sim` for every indexed pack): TOML syntax, `format = 1`, known keys, value shapes, field path and GUID syntax, `@id` defined before use and only once, a field set twice, at most 4 MiB and 65,536 operations per file. Names resolve only against a real file: `check`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The layout is valid (`info`, `records`, `show`); every file parses (`lint`); every operation of every pack applies (`check`); everything converted (`diff`) |
| 1 | The layout is refused (the reason names the first failing check); a pack doesn't parse or doesn't fully apply; a record or field isn't found (`show`); some change couldn't be converted (`diff`) |
| 2 | Bad arguments, a file or pack folder that can't be read, or a file smaller than the 120-byte header |

## Patching

The patcher is a library API, `sco::datacore::Patch` ([api.md](api.md#patching-datacore)); packs reach it through `sco::datacore::ApplyPacks` and `sco-dcb check` ([above](#packs-lint-check-show-diff)). Checked locally on 4.10.193 with a scratch program over the library (read-only, nothing committed), overriding one float field of one record by name:

```text
parsed 331932921 bytes, 117022 records, record size 36 (39 ms)
1 overrides (0 refused); first: record "Character.SHOPKEEP2_Gruff" field "nicknameChance" = 2.5
Emit: OK, 1 splices, 91 ms including re-validation
applied: 331932921 bytes; re-parse: layout: OK
splice 0: at 134562223, removes 4, adds 4; patched value reads 2.5
```

The same with one float override in each of 1,000 records: 1.9 ms for the operations, `Emit` 89 ms including re-validation (which composes and re-parses the whole patched file in memory), re-parse `layout: OK`.

## Patch-day routine

1. Extract `Game2.dcb` from the new `Data.p4k` (above).
2. `sco-dcb info Game2.dcb`.
3. Exit 0: the layout is still the one sco-core understands. Exit 1: the reason says which total or check broke; the format changed, and data overrides stay off until the parser follows.
4. `sco-dcb check Game2.dcb <pack>...` for every data pack: `SKIP` lines name each record or field that no longer resolves.
