# Checking a game build with sco-sigcheck

`sco-sigcheck` runs every signature table against a `StarCitizen.exe` file and prints the same report the game writes to `mod.log` at startup. It answers "does the mod still find its addresses in this build?" without starting the game, on macOS or Linux.

## Run it

```sh
tools/sigcheck.sh <StarCitizen.exe> [-v] [--catalog <runtime_catalog.inc>]
```

`tools/sigcheck.sh` builds `tests/out/sco-sigcheck` with clang (or gcc) and runs it. Set `CXX` to pick a compiler.

| Option | Does |
|---|---|
| `-v` | Also list rows that aren't OK in the table at the end (OK rows are always listed) |
| `--catalog <file>` | Label rows whose address matches an entry in an sdk_dumper `runtime_catalog.inc`. Labels only: nothing is resolved from the catalog |

The exe usually lives at `StarCitizen\LIVE\Bin64\StarCitizen.exe`. Copy it to the machine you run the check on; it's large (about 175 MB for build 12660092) and the tool only reads it.

## Output

```
image: timestamp 0x6aa945e3, size 0x12a9b000, preferred base 0x140000000
[core] signatures: 8/8 OK
OK       teleport.to_camera                   0x2e20f30
OK       teleport.client_mgr                  0xa19cf28
OK       teleport.handle_from_id              0x2d3ce50
OK       teleport.entity_system               0x9e2e708
OK       system.quit                          0x7237c60
OK       pak.datacore_loader                  0x70e0820
OK       pak.crypak                           0x9e2e6a0
OK       pak.slots                            0x70e0b49
```

That is build 4.10.193.11644 (CL 12660092). The rows, by table:

| Table | Rows | Used by |
|---|---|---|
| teleport | `teleport.to_camera`, `teleport.client_mgr`, `teleport.handle_from_id`, `teleport.entity_system` | sc-offline's teleport |
| system | `system.quit` | the host kit's shutdown hook |
| pak | `pak.datacore_loader` (the DataCore loader), `pak.crypak` (the `ICryPak*` global), `pak.slots` (the loader's read/seek/close calls) | `sco::game::pak`, the CryPak adapter for game-file overrides ([API](api.md#scogamepakh-the-crypak-adapter)) |
| asop, atc, hangar | 62 rows: `asop.*`, `insurance.*`, `atc.*`, `hangar.*`, `lift.*`, `landing.*`, `respawn.*` | sc-offline's ship terminal, hangar and ATC module, grouped into 11 capabilities ([rows](game/asop.md)) |

On 4.10.196.36804 (timestamp `0x6ac64e6f`) the report is `[core] signatures: 70/70 OK`.

The `pak.*` rows need the exe's `.pdata` (the loader's start comes from its unwind data), which `sco-sigcheck` reads from the file.

- `image:` identifies the build: the PE timestamp and image size change with every game build.
- The `[core]` lines are exactly what `mod.log` shows for the same build.
- The table lists each row's address as an RVA (offset from the image base). With `--catalog`, a matching row gets `= <name>` after it.

A broken build looks like this:

```
[core] signatures: 4/8 OK
[core] FAILED   teleport.to_camera (layout changed at +0x2b0)
[core] BLOCKED  teleport.client_mgr (needs teleport.to_camera)
...
```

Fix the `FAILED` (or `MISSING`, `AMBIG`) rows; the `BLOCKED` ones follow. See [Adding a signature § When a game patch breaks a row](adding-signatures.md#when-a-game-patch-breaks-a-row).

## Exit codes

| Code | Means |
|---|---|
| 0 | Every row is OK |
| 1 | At least one row isn't OK |
| 2 | Bad arguments, the file isn't a 64-bit PE image, or a table failed to register |

Scripts can gate on the exit code. CI checks that the tool builds and exits 2 on a non-PE file; it can't check a real build because the game exe isn't public.

## Patch-day routine

1. Copy the new `StarCitizen.exe` from a PC with the game.
2. `tools/sigcheck.sh StarCitizen.exe`.
3. Exit 0: sco-core's rows survived. Features that haven't moved to sco-core yet still need checking in game.
4. Exit 1: fix the rows it names, re-run, then play-test those features.
5. The game's data: extract `Game2.dcb` from the new `Data.p4k` and run `sco-dcb info` on it ([Checking a DataCore file](datacore.md#patch-day-routine)).

## What it can't tell you

- Whether the feature works. A row being found means the bytes match; only playing proves the game still behaves the same.
- Anything about addresses sc-offline still scans for itself (everything except teleport, the CSystem::Quit hook, the ASOP/hangar/ATC rows once sc-offline's module reads them and, once sc-offline enables `sco::game::pak`, the DataCore loader and CryPak).
- Whether the DataCore loader still reads its file only through open/read/seek/close. `pak.slots` proves those calls exist; the in-game load line (`[pak] ... served from its mount`) proves the file was served.
- Anything about patches applied by other means (the launcher, data files).
