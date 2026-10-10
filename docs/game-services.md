# Game services

The Star Citizen game pack publishes its services under the reserved owner `game` ([design](design/game-services.md)). Plugins find them with the ordinary `query_service`; `sco_api.h` doesn't change. They exist only in builds with the game pack ([game pack](game-pack.md)), and only when the product turns them on (`sco::app::Platform::gameServices`).

| Service | Version | Header | Since game pack | What |
|---|---|---|---|---|
| `teleport.spatial` | 1.0 | [`sc_spatial.h`](../include/sc_spatial.h) | 0.1.0 | Where you are, and positions between the game's zones |

`teleport.spatial` keeps its name and table for all of 1.x: sc-offline's `teleport` built-in published it before game pack 0.1.0, and a plugin can't tell the difference. New game services are named `game.<name>`.

## For products

- `sco::host::ProvideGameService(name, version, table)` publishes under the owner `game` (`sco/host.h`): `game.<name>`, or one of the product names a game pack took over (`kGameCompatNames`). The owner is never released; `sco::app::Stop` withdraws game services after every plugin has unloaded.
- `sco::app::Platform::gameServices = true` starts them before any plugin loads. They read the game through the `teleport.*` rows, so resolve the rows first. A product that still publishes `teleport.spatial` itself must leave it off: the second provider of a name is refused.
- The implementations are in `src/game/services/` (library `sco_game_services`, Windows only: game reads run under SEH). Builds without them log `[app] game services not built` and go on.
