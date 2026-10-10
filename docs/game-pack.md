# The Star Citizen game pack

The game pack is the part of sco-core that knows Star Citizen: the signature rows in `src/game`, the game services published under the owner `game` ([game services](game-services.md)), the CryPak adapter and DataCore. It builds with `SCO_GAME_SC` (on by default; [architecture](architecture.md#kernel-and-game-pack)).

## Versions

The game pack has its own version line ([design decision 2](design/game-services.md)):

| Line | Where | Tags |
|---|---|---|
| Kernel, `sco_api` 1.x | `sdk/VERSION`, `SCO_API_MAJOR`/`MINOR` in `sco_api.h` | `sdk-vX.Y.Z` |
| Game pack | `SCO_GAME_PACK_VERSION` in [`sc_game_pack.h`](../include/sc_game_pack.h) | `game-sc-vX.Y` |
| Each game service table | its `*_VERSION` macro, passed to `query_service` | none: changes only with the table |

A game patch that moves rows gets a game-pack patch release. The SDK zip states all three (its `VERSION`, `include/sc_game_pack.h`, and this page's list of builds).

## Verified game builds

Every row OK in `sco-sigcheck` on the build's `StarCitizen.exe`. The full reports are kept outside the repository (`sc-baseline/<build>/` on the maintainer's machine); nothing extracted from the game is committed.

| Game pack | Game build | Rows | Report |
|---|---|---|---|
| 0.1.0 | 4.10.196.36804 (PE timestamp `0x6ac64e6f`) | 70/70 OK | `sc-baseline/4.10.196.36804/sigcheck-game-pack-0.1.0.txt` |
