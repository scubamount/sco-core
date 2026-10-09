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
[core] signatures: 5/5 OK
OK       teleport.to_camera                   0x2e20f30
OK       teleport.client_mgr                  0xa19cf28
OK       teleport.handle_from_id              0x2d3ce50
OK       teleport.entity_system               0x9e2e708
OK       system.quit                          0x7237c60
```

- `image:` identifies the build: the PE timestamp and image size change with every game build.
- The `[core]` lines are exactly what `mod.log` shows for the same build.
- The table lists each row's address as an RVA (offset from the image base). With `--catalog`, a matching row gets `= <name>` after it.

A broken build looks like this:

```
[core] signatures: 1/5 OK
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

## What it can't tell you

- Whether the feature works. A row being found means the bytes match; only playing proves the game still behaves the same.
- Anything about addresses sc-offline still scans for itself (everything except teleport and the CSystem::Quit hook, for now).
- Anything about patches applied by other means (the launcher, data files).
