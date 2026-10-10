# Game services

The Star Citizen game pack publishes its services under the reserved owner `game` ([design](design/game-services.md)). Plugins find them with the ordinary `query_service`; `sco_api.h` doesn't change. They exist only in builds with the game pack ([game pack](game-pack.md)), and only when the product turns them on (`sco::app::Platform::gameServices`).

| Service | Version | Header | Since game pack | What |
|---|---|---|---|---|
| `teleport.spatial` | 1.0 | [`sc_spatial.h`](../include/sc_spatial.h) | 0.1.0 | Where you are, and positions between the game's zones |
| `spawn.entities` | 1.2 | [`sc_spawn.h`](../include/sc_spawn.h) | unreleased (after 0.1.0) | Spawn entities near you, your entity and ship ids, and move what you spawned |

`teleport.spatial` and `spawn.entities` keep their names and tables for all of 1.x: sc-offline's `teleport` and `spawn` built-ins published them before the game pack did, and a plugin can't tell the difference. New game services are named `game.<name>`.

## For products

- `sco::host::ProvideGameService(name, version, table)` publishes under the owner `game` (`sco/host.h`): `game.<name>`, or one of the product names a game pack took over (`kGameCompatNames`). The owner is never released; `sco::app::Stop` withdraws game services after every plugin has unloaded.
- `sco::app::Platform::gameServices = true` starts them before any plugin loads. They read the game through the `teleport.*` and `spawn.*` rows, so resolve the rows first. A product that still publishes `teleport.spatial` or `spawn.entities` itself must leave it off, or drop its own provider: the second provider of a name is refused.
- `spawn.entities`' mover lets any plugin move the player's own vehicle once the product registers it (`sco::game::services::RegisterPlayerVehicle` / `UnregisterPlayerVehicle`, `sco/game/services.h`, game thread). Entities a plugin spawned through `spawn_as` stay movable by that plugin only, and are forgotten when it unloads or crashes.
- The implementations are in `src/game/services/` (library `sco_game_services`, Windows only: game reads run under SEH). Builds without them log `[app] game services not built` and go on.

## Internal game-pack helpers

Not services: no table, no `query_service`. Functions in `src/game/services/` (library `sco_game_services`, Windows only) that a product links and calls, because they read live game objects a signature row can't express. They run under SEH, answer 0 on a fault, and give a stable reason string. The game thread only.

| Header | Function | What |
|---|---|---|
| [`sco/game/missions.h`](../include/sco/game/missions.h) | `sco::game::missions::ScriptLibrary(manager, &reason)` | The mission script library, from the live mission manager (slot `0x48` of its vtable; canary `48 8B 4B` at `+0x32` of that function; the field displacement is the byte at `+0x35`; all checked on every call). 0 with `kReasonNotFound` or `kReasonNotYet`. sc-offline's `missions` built-in calls it; the lookup used to be a raw scan there. |
