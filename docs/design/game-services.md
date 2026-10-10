# Design: game services, the SDK as the platform every plugin builds on

Status: decided 2026-10-10 (maintainer decisions 1 to 8 below). No code yet.

Maintainer decisions of 2026-10-10 that this page builds on:

1. Game services live in sco-core, in the Star Citizen game pack (`SCO_GAME_SC`), not in sc-offline's built-ins.
2. The game pack gets its own version line, separate from the kernel's `sco_api` 1.x.
3. Game services come first. Service safety (M1, [service-safety.md](service-safety.md)), command-slot recycling (L5) and hot reload follow.
4. The goal is a platform: every plugin, sc-offline's own features included, reaches the game's systems through the SDK and nothing else.
5. Lua gets the `game.*` services read-only.
6. An entity a plugin spawned is removed from the world when that plugin unloads or crashes.
7. Any plugin may move the player's own vehicles.
8. High-volume events are filtered by entity type when a plugin subscribes.

## The problem, in today's code

- **Two game services, both owned by the product.** `teleport.spatial` ([`sc_spatial.h`](../../include/sc_spatial.h): read-only pose and frame conversion) and `spawn.entities` 1.2 ([`sc_spawn.h`](../../include/sc_spawn.h): spawn near the player, move what you spawned). sc-offline's built-ins publish them (`src/builtins/teleport_plugin.cpp`, 313 lines; `spawn_plugin.cpp`, 259), on top of `spawner.cpp` (1,725) and `teleport.cpp` (578).
- **Every other system is private to sc-offline.** Seats and crew, NPCs, loadouts, ammo, quantum, build mode and contracts are reachable only from sc-offline's own code. A third party cannot write an NPC companion, a ship spawner or a bounty mission.
- **Raw scans.** sc-offline still has 38 raw pattern scans in 7 files: contracts 16, quantum 9, spawner 5, patches 5, npc, ammo and hooks 1 each. They are moving into signature rows in `src/game/` (branch `game-rows-features`, `src/game/features_sigs.cpp`). Rows make those systems survive game patches. **They do not make them callable from a plugin.** That takes a service.
- **Events.** The bus carries `game.ready`, `game.exit` and `net.state`. Nothing a gameplay mod can react to.

## Goals and limits

- `sco_api.h` does not change: still 128 bytes, append-only. Game systems are service tables reached with `query_service`, like every service since 1.1.
- The kernel build (`SCO_GAME_SC` off) is untouched. Game services compile only into the game pack, and the `kernel-only` CI job stays the gate.
- Code that reads or writes game memory lives only in `src/game/` (rows and the code built on them). The `game.*` tables are the only public route to it.
- sc-offline's built-ins use the same public headers as a third-party plugin.
- Scope rules don't change: no anti-cheat bypass, no entitlement tampering, no CIG services. The `game.*` services are local to one player's offline session.
- This is not an engine-neutral abstraction. `sco-titanlink-northstar` is an IPC peer, not a second sco host (decision of 2026-10-10). The types are Star Citizen's: zones, 64-bit entity ids, quaternions in the game's order.

## Layers

```text
L4  Third-party plugins    C, C++20, C#, Lua         public headers only (L0 + L2)
L3  Product (sc-offline)   launcher, offline patches, built-in features as consumers,
                           creative/cheats as a separate DLL mod
L2  Game services          game.entities  game.vehicles  game.actors  game.world  game.* events
L1  Game pack internals    src/game rows, pak/vfs, DataCore: the only code that touches game memory
L0  Kernel                 sco_api 1.x, loader, tasks, commands, events, services,
                           sco.storage, sco.ui, sco.ipc, sco.net, hook and scan engines
```

L1 and L2 together are the game pack: `sco_game_sc` and the libraries that need it (CMakeLists.txt, `SCO_GAME_SC`).

## Names and ownership

- **Owner id `game`.** It is already reserved (`kReservedIds` in `include/sco/plugins.h:52`, `kReservedPrefixes` in `include/sco/runtime.h:198`), and the bus already uses it for `game.ready` and `game.exit`. Host services are published by `host::ProvideHostService` under `kHostId = "sco"` (`include/sco/host.h:63-71`). A sibling, `host::ProvideGameService(name, version, table)`, publishes under the runtime owner `game`, which is never released. The game pack calls it at start-up, and only when built.
- **Three tiers of names.** `sco.*` for kernel services, `game.*` for the game pack, a product prefix (`teleport.`, `asop.`, `economy.`) for a product's own features.
- **Headers.** `include/sc_<name>.h`, following the existing rule: "the sc_ prefix marks a product's service: sco_ is reserved for host services" (`sc_spawn.h`). Each table's layout is pinned by `tests/abi_game_<name>.c`. The SDK zip lists them in a game-pack section.
- **Wrappers, in the same PR as the table** (as the C# layer did in #33): `include/scosdk/game/*.hpp`, `Sco.Sdk.Game` in C#, `sco.game.*` in Lua. **Lua is read-only** (decision 5): queries and events, never a function that takes `self` and changes the game. A Lua plugin owns no entities.
- **Compatibility.** The game pack also publishes `teleport.spatial` and `spawn.entities` (same tables, same versions). sc-offline's built-ins stop providing them. Both names stay for all of 1.x.

## Versioning

- **Kernel:** `sco_api` 1.x and `sdk-vX.Y` tags, unchanged.
- **Game pack:** `SCO_GAME_PACK_VERSION`, tags `game-sc-vX.Y`, plus the list of game builds it was verified on (sigcheck reports). A game patch that moves rows gets a game-pack patch release. Table versions change only when a table changes.
- Each `game.*` table keeps its own version (`0x00010000u` style), passed to `query_service`, as today.
- The SDK zip states all three: kernel version, game-pack version, verified game builds.
- **Capabilities per system** (`has("game.vehicles.seats")`). After a patch, a plugin sees one missing system, not a dead SDK.

## Rules for every `game.*` table

- `uint32_t size` first. The version is the `query_service` argument, never a field. Returns are `sco_result` or `int`; enums are 4 bytes.
- **Game thread only** unless the function says otherwise. Called from another thread it fails without touching the game, as `sc_spawn.h` already specifies.
- **Every function that changes the game takes `sco_plugin* self` first**, for ownership, quotas and attribution in `mod.log`. Read-only queries don't.
- **Ownership.** An entity a plugin spawns through `game.*` belongs to it, and the host **removes it from the world when that plugin unloads or crashes** (decision 6). Nothing a plugin spawned outlives it. A plugin may move or despawn its own entities. **Any plugin may move the player's own vehicles** (decision 7) once sc-offline has registered them as retrieved or delivered (the existing `sc_spawn.h` rule), as long as the capability is there. It may not touch anything else.
- **Ids.** Entity and zone ids are session handles that answer 0 once streamed out, never persistent keys. Tables give the stable identity the game has (entity class name, DataCore record GUID) for anything a plugin stores.
- **No allocation across the boundary.** Lists go into caller buffers with a count and a "more" flag.
- **Every failure has a reason**, from `last_error(self, …)`, as `sco.storage` does.

## The service set

| Table | Functions (v1.0) | Comes from today | Rows |
|---|---|---|---|
| `game.entities` | `alive`, `class_of`, `get_transform` (pos, rot, zone), `set_transform(self, …)`, `spawn(self, class, zone, pos, rot, &id)` (anywhere, not only near the player), `despawn(self, id)`, `query_radius(zone, pos, radius, class filter, ids, max)` | `spawn.entities`, `teleport.spatial`, `spawner.cpp`, `build.cpp`'s mover | Spawn and mover exist. Entity enumeration for `query_radius` needs a new row: spike |
| `game.vehicles` | `player_ship`, `seats(ship, out, max)`, `seat_occupant`, `seat(self, actor, ship, seat)`, `eject(self, actor)`, `power_on(self, ship)` / Flight Ready | crew built-in, `spawner.cpp` seat code | In the rows work. The Javelin bulk fill (10 of 15 seats) gets fixed here, since the code moves anyway |
| `game.actors` | `local_player`, `spawn_npc(self, archetype, zone, pos, &id)`, `despawn(self, id)`, `health`, `state` (alive, incapacitated, dead) | npc built-in, `npc.cpp` | NPC spawn: in the rows work. Health and state: spike |
| `game.world` | `raycast(zone, from, dir, max, &hit)` (entity, position, normal), `camera(&pos, &rot, &fov)` | build mode (reported to read camera and terrain; not yet verified) | Spike: no rows yet |
| `game.*` events | On the bus: `game.player.spawned`, `game.player.died`, `game.zone.changed`, `game.vehicle.boarded` / `exited`. Filtered watch: entity `streamed_in` / `streamed_out` | none | Spike per event: each needs a hook row. Payloads are size-prefixed structs in `sc_game_events.h` |

`teleport.spatial`'s frame conversion stays as it is. `game.entities` uses its frames and units.

**High-volume events are not on the bus** (decision 8). The event bus subscribes by name only, and entity streaming can fire thousands of times a second, so it goes through a filtered watch on `game.entities`:

```c
/* type: an entity class name, or a class prefix ending in '*' ("AEGS_*"). An empty or lone "*"
 * filter is refused. Game thread; fn runs on the game thread for matching entities only. */
sco_result (*watch)(sco_plugin* self, uint32_t what /* streamed_in, streamed_out */,
                    const char* type, sc_entity_watch_fn fn, void* ctx);
sco_result (*unwatch)(sco_plugin* self, uint64_t watch_id);
```

The host matches the type before calling into the plugin, so a plugin pays only for the entities it asked about. Watches are withdrawn when the plugin unloads, like its other registrations. The low-volume player, zone and vehicle events stay on the bus.

## What changes where

| Repo | Change |
|---|---|
| sco-core | `include/sco/host.h`: `ProvideGameService`. `src/game/services/`: the table implementations, including code moved from sc-offline (teleport's spatial service, spawn's mover, seats, NPC spawn). `include/sc_*.h`, `tests/abi_game_*.c`, `include/scosdk/game/`, C# and Lua wrappers. `sdk/package.py`: game-pack section and versions. CMake: the services built only with `SCO_GAME_SC`. `docs/game-services.md` |
| sc-offline | Built-ins stop providing `teleport.spatial` and `spawn.entities` and call `game.*` instead. Code that moved is deleted. Gate: `src/builtins/` compiles against the SDK zip's include folder only, never `include/sco/` |

## PR plan

1. This page.
2. **The move, with no new API.** `ProvideGameService`, the game-pack version line, and `teleport.spatial` and `spawn.entities` published from the game pack. sc-offline repins and drops its providers. This proves the layering before anything new is added.
3. `game.vehicles` 1.0, after `game-rows-features` merges. The crew built-in becomes a consumer.
4. `game.actors` 1.0 (NPC spawn first, health and state when their rows exist).
5. `game.entities` 1.0 (general spawn and despawn; `query_radius` after its spike).
6. `game.world` and the events, each after its spike produces rows checked on the current build.
7. M1 ([service-safety.md](service-safety.md)) and L5, then dependency order (sort by `requires`, extended to service names), typed `[settings]`, hot reload.
8. sc-offline: built-ins on SDK headers only, and the creative/cheats DLL mod split.

PRs 3 to 5 depend on the rows workers' output. PR 2 depends on nothing and can start now.

## Why game services before M1 is safe

M1 has two halves. **Use after unload** can't happen with `game.*`: the tables belong to the host, which never unloads them before plugins, the same as `sco.*`. **Wrong blame** does apply: a fault inside game-pack code is caught by the calling plugin's guard and blames the caller. That is the same as a built-in fault today (a product bug), and M1 Part 1 fixes it for both. Plugin-provided services keep both risks until M1 lands.

## Rejected

- **Appending game functions to `sco_api`.** It would put game concepts in the kernel ABI forever and break the kernel-only build.
- **An engine-neutral `sco.world`.** There is no second host to justify one (Northstar is an IPC peer).
- **Moving `sc_spatial.h` and `sc_spawn.h` into sc-offline.** Superseded by #50's game pack.
- **Keeping implementations in sc-offline's built-ins** (decision 1). Rows and the code that uses them would sit in two repositories, and no other product could reuse them.

## Decided

The open questions of the first draft were answered by the maintainer on 2026-10-10. They are decisions 5 to 8 at the top of this page: Lua read-only, a plugin's entities removed when it unloads, the player's vehicles movable by any plugin, and type filters for high-volume events.
