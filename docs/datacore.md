# Checking a DataCore file: `sco-dcb`

`sco-dcb` reads the game's DataCore database (`Data\Game2.dcb`) with `sco::datacore` and prints what the parser sees: the header, the tables with their offsets and sizes, the derived record size, and whether the layout passes validation. `sco-dcb records` lists every record, the input for research step R1 of the [DataCore design](design/vfs-datacore.md#addrecord). It is a host tool, like [`sco-sigcheck`](sigcheck.md): built with sco-core, never shipped to players, read-only on the file.

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

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The layout is valid |
| 1 | The layout is refused; the reason names the first failing check |
| 2 | Bad arguments, the file can't be read, or it is smaller than the 120-byte header |

## Patching

The patcher is a library API, `sco::datacore::Patch` ([api.md](api.md#patching-datacore)); `sco-dcb` has no patch command. The design's tool commands for packs (`check`, `show`, `diff`, `lint`) come with the `.toml` format in plan PR 5. Checked locally on 4.10.193 with a scratch program over the library (read-only, nothing committed), overriding one float field of one record by name:

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
