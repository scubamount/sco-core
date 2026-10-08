# sco-core

The game-facing core of [sc-offline](https://github.com/scubamount/sc-offline), the Star Citizen offline mod.

Features in sc-offline never look up game addresses themselves. Every address is one named row in a signature table here. At startup sco-core finds each row and writes one report, so after a game patch you can see which rows broke and which features they turn off:

```
[core] signatures: 3/4 OK
[core] FAILED   teleport.to_camera (layout changed at +0x2b0)
[core] BLOCKED  teleport.client_mgr (needs teleport.to_camera)
```

Status: early. The API is internal (version 0) and can change in any commit. sc-offline is its only user.

## Layout

| Path | What |
|---|---|
| `include/sco/scan.h`, `src/sco_scan.cpp` | Byte-pattern, string and RIP-relative scanners over a PE image |
| `include/sco/signatures.h`, `src/sco_signatures.cpp` | The registry: rows, dependency order, per-row result, the report |
| `include/sco/log.h`, `include/sco/status.h` | Where log lines go, and the one-line status features show the player |
| `include/sco/game/`, `src/game/` | The game's signature tables, one file per feature area, plus typed accessors |
| `include/sco/pe_file.h`, `src/sco_pe_file.cpp` | Host tools only: lays out `StarCitizen.exe` from disk for offline checks |
| `tools/sco-sigcheck.cpp` | Runs every table against a `StarCitizen.exe` file without the game |

## Checking a game build without the game

```sh
tools/sigcheck.sh /path/to/StarCitizen.exe          # build + run; exit 0 = every row OK
tools/sigcheck.sh /path/to/StarCitizen.exe -v       # also list the OK rows with their RVAs
```

It reads the file and compares bytes. It doesn't run game code. `--catalog <runtime_catalog.inc>` labels rows whose address matches an entry in an sdk_dumper catalog. The catalog is only used for labels; sco-core never resolves anything from it.

## Tests

```sh
tools/test.sh      # host unit tests (clang or gcc, AddressSanitizer + UBSan)
```

## Use from sc-offline

sc-offline includes this repository as a git submodule at `external/sco-core` and compiles the sources into its one `dinput8.dll`. There is no second DLL and no binary interface between the two.

## License

GPL-3.0, the same as sc-offline. See `LICENSE`.
