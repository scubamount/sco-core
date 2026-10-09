# Checking a DataCore file: `sco-dcb`

`sco-dcb` reads the game's DataCore database (`Data\Game2.dcb`) with `sco::datacore` and prints what the parser sees: the header, the tables with their offsets and sizes, the derived record size, and whether the layout passes validation. `sco-dcb records` lists every record, the input for research step R1 of the [DataCore design](design/vfs-datacore.md#addrecord). `sco-dcb patch` applies a batch of patcher operations and writes the patched file somewhere else, to check the patcher on a real file. It is a host tool, like [`sco-sigcheck`](sigcheck.md): built with sco-core, never shipped to players, and it only reads the game's file.

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

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The layout is valid (`patch`: the batch emitted, re-validated, and the output was written) |
| 1 | The layout is refused; the reason names the first failing check (`patch`: an operation or `Emit` was refused; nothing is written) |
| 2 | Bad arguments, a file can't be read or written, or it is smaller than the 120-byte header |

## Patching

The patcher is a library API, `sco::datacore::Patch` ([api.md](api.md#patching-datacore)). `sco-dcb patch` runs it from the command line, to check it on a real file; it is a developer tool, and the design's commands for packs (`check`, `show`, `diff`, `lint`) come with the `.toml` format in plan PR 5.

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

## Patch-day routine

1. Extract `Game2.dcb` from the new `Data.p4k` (above).
2. `sco-dcb info Game2.dcb`.
3. Exit 0: the layout is still the one sco-core understands. Exit 1: the reason says which total or check broke; the format changed, and data overrides stay off until the parser follows.
