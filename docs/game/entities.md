# Entities rows

The addresses `game.entities` uses, from Appendix B of [game world spikes](../design/game-world-spikes.md) (B3 and B9). Rows are in [`src/game/entities_sigs.cpp`](../../src/game/entities_sigs.cpp), the accessor constants and capability groups in [`include/sco/game/entities.h`](../../include/sco/game/entities.h). Checked against 4.10.196.36804.

| Row | Finds | Checks |
|---|---|---|
| `entities.dump` | The console dump `EntitySystemDump` (no name filter), by the unique pattern `48 8B 0D ?? ?? ?? ?? 48 8D 55 E7 48 89 45 F7 41 80 E0 01 48 8D 45 B7` | the lambda's `lea` at +0x3B, the ForEach call at +0x46, the entity count read at +0x24D |
| `entities.index` | The entity index global (RIP operand of the `mov rcx` at +0x00) | outside the image fails |
| `entities.for_each` | The game's "for each entity" function (the call at +0x46 of the dump) | the call and its target in `.text` |
| `entities.class_site` | The dump's lambda `void (ctx, entity handle)`, from the `lea` at +0x3B | its prologue and handle mask, the class name read (entity slot `0x20`, then that class's slot `0x18`) at +0x28, entity slot `0x398` at +0x6E and the id read (entity slot `0x08`) at +0xAB: the offsets `entities.cpp` relies on |
| `entities.spawn_sinks` | `CEntitySystem::CallOnSpawnSinks`: its label's `lea` (count 1) at +0x17 of `FunctionStart` | the prologue, the sink list at +0x70, the sink call at +0xF0 |
| `entities.delete_entity` | `CEntitySystem::DeleteEntity`: both of its labels (count 1 each, +0x6C and +0x16B) lead to the same `FunctionStart` | the prologue, +0x26, the handle mask at +0xBB |

Capability groups (`sco::game::entities::Capabilities`): `entities.class_name` (class_of), `entities.enumerate` (query_radius), `entities.stream_hooks` (watch). What the rows can't pin, and the constants that stay `false` until an in-game run: [game services](../game-services.md).
