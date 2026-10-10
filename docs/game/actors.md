# Actor rows (spawner, Clear NPCs, infinite ammo, loadout)

The addresses sc-offline's actor features use, moved from its own scans in `src/spawner.cpp`, `src/npc.cpp`, `src/ammo.cpp` and `src/loadout.cpp`. Rows are in [`src/game/actors_sigs.cpp`](../../src/game/actors_sigs.cpp), the accessor constants and capabilities in [`include/sco/game/actors.h`](../../include/sco/game/actors.h). Checked against 4.10.196.36804.

Where the old scan took the first match, the row insists on the count found in that build (`kLandingAreaRefs`, `kObjectContainersRefs`, or exactly one match that passes the checks), so a patch that adds or loses a site fails the row. Checks marked *sweep* were added by the offset sweep: they pin offsets and vtable slots sc-offline uses as numbers.

Fly mode (`spawn.request_fly_mode`) and Clear NPCs' RemoveEntity slot (`npc.remove_entity_call`) are rows of [`features.h`](../../include/sco/game/features.h).

## Rows

| Row | How it's found | Checks | Capability |
|---|---|---|---|
| `spawn.landing_helper` | first of the 2 `lea r9` of "Landing Area could not be found.", minus 0x59 | reference count 2; prologue; `mov cl, imm8; call` +0x4F; `mov edx, 0x1000` +0x40C; calls +0x46D, +0x47D, +0x55C. *Sweep:* entity system slots 0x128 (+0x31), 0xC0 (+0x29C), 0x118 (+0x5DA), 0xC8 (+0x765), 0xD8 (+0x8E4); class registry 0x20 (+0x2A9); spawn batch 0x10 (+0x896); entity 0x390 (+0xF7); components 0x10 (+0x11D); the five entity system loads read `teleport.entity_system`, the components load reads it +8 | `spawn.helpers` |
| `spawn.team_tag` | the imm8 at helper +0x50 | (helper) | `spawn.helpers` |
| `spawn.team_category` | call at helper +0x51 | E8 | `spawn.helpers` |
| `spawn.set_flags` | call at helper +0x46D | E8 | `spawn.helpers` |
| `spawn.set_class` | call at helper +0x47D | E8 | `spawn.helpers` |
| `spawn.set_location` | call at helper +0x55C | E8 | `spawn.helpers` |
| `spawn.params_ctor` | unique pattern | - | `spawn.helpers` |
| `spawn.find_seat` | unique pattern | - | `spawn.helpers`, `spawn.seat_picker` |
| `spawn.seat_callback` | `lea rax` at find_seat +0x21C | `call [rax+0x778]` +0x1D6; callback `cmp [rdi+0x158], 0` +0x37. *Sweep:* `mov r8d, 193` +0x223 | `spawn.seat_picker` |
| `spawn.is_linked`, `.force_delink`, `.for_each_seat`, `.actor_of_user`, `.handle_to_id`, `.actor_link`, `.force_link` | calls at find_seat +0xB2, +0x19C, +0x242, +0x3CE, +0x3F1, +0x3FE, +0x40B | E8 each | `spawn.seat_picker` |
| `spawn.seat_priority` | call at seat_callback +0xD6 | E8 | `spawn.seat_picker` |
| `spawn.find_entity_by_name` | the `lea rcx` of "FindEntityByName_SlowDebugCodeOnly: %s" minus 0x36 | exactly one of the references has the prologue | `spawn.find_by_name` |
| `spawn.toggle_flight_ready` | the Events/ISC/Dashboards.h sender whose r8d line is 0x49 | exactly one sender; `48 89 5C 24` start, `mov rdi, rdx`, `mov [rsp+..], r8` | `spawn.flight_ready` |
| `spawn.game_cvars` | `mov [rip+X], rax` after the 0x98-byte allocation (unique pattern) | - | `spawn.fly_speed` |
| `spawn.fly_speed_scaler` | the `lea r8, [rdi+disp32]` 0x14 before a `lea rdx` of "g_FlyModeSpeedScaler" | exactly one; disp32 in (0, 0x4000) | `spawn.fly_speed` |
| `spawn.god_mode_call` | unique pattern (`mov rcx, [r14+0x208]; add rcx, 0x27F0; call`) | `lea r9` of the GodMode State message at -0x64 | `spawn.god_mode` |
| `spawn.set_god_mode` | call target at god_mode_call +0x12 | prologue + `mov [rcx+disp32], dl` | `spawn.god_mode` |
| `spawn.prefab_site` | first of the 2 `lea rdx` of "ObjectContainers\%s" | reference count 2; system load +0x37, `call [rax+..]` +0x41, `mov r8, [rcx+..]` +0x58, `mov rdi, [rax+..]` +0x6E, "ocFilename" at +0x72, `lea rax` +0x94, call +0xA0 | `spawn.prefabs` |
| `spawn.prefab_system` | RIP target at prefab_site +0x37 | inside the image | `spawn.prefabs` |
| `spawn.prefab_attr_writer` | RIP target at prefab_site +0x94 | inside the image | `spawn.prefabs` |
| `spawn.prefab_attr_type_id` | call at prefab_site +0xA0 | E8 | `spawn.prefabs` |
| `npc.direct_remove_call` | unique pattern (the internal remove call in RemoveEntity) | .pdata function start within 0x700 | `npc.direct_remove` |
| `npc.remove_entity` | .pdata start of that call's function | prologue; sc-offline compares it with the entity system's vtable entry at the `npc.remove_entity_call` slot | `npc.direct_remove` |
| `npc.direct_remove` | call at direct_remove_call +6 | E8 | `npc.direct_remove` |
| `ammo.set_ammo` | unique pattern (key load `mov eax, [rcx+0xC4]` inside) | *Sweep:* `xor r15d, [rcx+0xC0]` +0x25, `mov esi, [rcx+0xB8]` +0x3C | `ammo.setter` |
| `loadout.load_player_loadout` | the `lea rdx` of "Scripts/Loadouts/Player" minus 0x44 | exactly one passes: prologue, game load +0x18, `call [rax+..]` +0x27, `mov rdx, [rcx+..]` +0x30, `mov r9b, 1` +0x88, `mov r8d, 8` +0x90, `call [rax+..]` +0x99 | `loadout.loader` |
| `loadout.game` | RIP target at loader +0x18 | inside the image | `loadout.loader` |

## Capabilities

| Capability | Rows | sc-offline feature |
|---|---|---|
| `spawn.helpers` | `teleport.entity_system`, landing helper and its calls, params ctor, find_seat | ship spawner (and NPCs, build mode, crates that ride it) |
| `spawn.seat_picker` | find_seat, seat_callback and the eight call rows | seat control (Crew & seats), "sit in pilot seat" |
| `spawn.find_by_name` | `spawn.find_entity_by_name` | spawn above Daymar |
| `spawn.flight_ready` | `spawn.toggle_flight_ready` | Flight Ready after spawning (else presses R) |
| `spawn.fly_speed` | `spawn.game_cvars`, `spawn.fly_speed_scaler` | noclip speed |
| `spawn.god_mode` | `spawn.god_mode_call`, `spawn.set_god_mode` | god mode |
| `spawn.prefabs` | the four prefab rows | outposts / prefabs |
| `npc.direct_remove` | `npc.remove_entity_call` and the three npc rows | Clear NPCs' fallback when RemoveEntity doesn't finish |
| `ammo.setter` | `teleport.entity_system`, `ammo.set_ammo` | infinite ammo |
| `loadout.loader` | `loadout.load_player_loadout`, `loadout.game` | gear menu, outfits |

## Offsets sc-offline uses that no row checks

`kSpawnParamsSize` (0x800, sc-offline's own buffer), `kGameVehicleRecord` (game slot 0x298) and `kVehicleRecordSize` (+0x10, the menu's size sort only), `kEntityOocZone` (slot 0x6E0, Daymar's zone; sc-offline confirms it with ZoneId/ZoneFromId), `kEntityName` (slot 0x78, read only), `kEntityParentPort` / `kPortOwnerId` (slots 0x150 / 0x8, ammo's "is this mine" walk; a wrong answer only skips a refill). Entity slots 0x120, 0x2B8 and 0x6B8 are checked by `teleport.to_camera`.
