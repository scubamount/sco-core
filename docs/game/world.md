# World rows: build mode, missions, cvars, quantum boost

The rows in [`src/game/world_sigs.cpp`](../../src/game/world_sigs.cpp) are the scans sc-offline's `build.cpp`, `missions.cpp`, `cvars.cpp` and `quantum.cpp` ran themselves, moved byte for byte. Where the old scan took the first match, the row insists on the count found in 4.10.196.36804. Accessor: [`sco/game/world.h`](../../include/sco/game/world.h).

| Capability | Rows |
|---|---|
| `build.free_cam` | `build.free_cam_on`, `build.free_cam_off`, `build.free_cam_flag` |
| `build.ground_ray` | `build.ray_tag`, `build.ground_ray`, `build.phys_world`, `build.release_grid`, `build.entity_vtable`, `build.entity_ray_proxy`, `build.entity_skip_add` |
| `build.entity_move` | `build.entity_get_rotation`, `build.entity_vtable`, `build.entity_set_position`, `build.entity_set_rotation` |
| `build.camera` | `teleport.to_camera`, `build.camera_fields` |
| `missions.start` | `missions.create`, `missions.settings` |
| `missions.load_all` | `missions.load_all` |
| `missions.scripts` | `missions.load_all`, `missions.subsumption`, `missions.file_change`, `missions.xml_system` |
| `cvars.console` | `cvars.console` |
| `cvars.qdrive_kept_on` | `cvars.quantum_travel_allowed`, `cvars.quantum_boost_allowed`, `cvars.ignore_blocked_boost`, `cvars.ignore_blocked_travel`, `cvars.qdrive_logging`, `cvars.target_lock_angular`, `cvars.target_lock_linear` |
| `quantum.boost` | `quantum.on_action`, `quantum.start_use`, `quantum.drive_input`, `quantum.effect_update`, `quantum.charge`, `quantum.spline_get_y`, `quantum.audio_system`, `quantum.handle_valid` |

## Rows and checks

| Row | Found from | Checks |
|---|---|---|
| `build.free_cam_on` / `_off` | the one `lea r8, [handler]` registered with `FreeCamEnable` / `FreeCamDisable` | `+0x23` / `+0x11` load "Enabling free cam" / "Disabling free cam"; `_off` starts `48 83 EC 28 80 3D ?? ?? ?? ?? 00` |
| `build.free_cam_flag` | `cmp byte [rip+X], 0` at `build.free_cam_off+4` | |
| `build.ray_tag` | "PlanetRayIntersection" in .rdata | |
| `build.ground_ray` | the one `lea rax, [tag]` with the planet ray cast's layout | `-0xBF`, `-0x42`, `+0x07`, `+0x26`, `+0x80`, `+0x9A`, `+0xF2`; the release it calls loads the same world global |
| `build.phys_world`, `build.release_grid` | `build.ground_ray-0xBF` (mov), `+0xF4` (call) | |
| `build.entity_get_rotation` | unique pattern (the slot 0x2C8 function) | |
| `build.entity_vtable` | the one .rdata vtable holding it at 0x2C8 | slots 0x208, 0x2B0, 0x2C0, 0x2C8, 0x430 hold functions with the bytes sc-offline checked at run time |
| `build.entity_*` | that vtable's slot | |
| `build.camera_fields` | `teleport.to_camera+0x17A` | camera rotation at `+0x6D38..+0x6D44`, position at `+0x6D20..+0x6D30` (`kCameraRotation`, `kCameraPosition`) |
| `missions.settings` | the one `lea rcx, [fmt]` of the spawn mission request line preceded by `48 8B 0D ?? ?? ?? ?? 83 79 0C 00` | |
| `missions.load_all` | the handler registered with `mission_load_all` | prologue; `+0x2E` loads the subsumption global and calls `[rax+0xA0]` |
| `missions.file_change` | the prologue within 0x400 bytes before the OnFileChange label | `+0x203` loads the system global |
| `missions.create` | the one `dgs.subsumption.mission.create` registration with `48 8B 59 18` / `48 8D 05` before it | prologue |
| `cvars.console` | `mov rcx, [console]` before each "debugGUI_enable 1" call | all `kConsoleSites` (7) sites, same global |
| `cvars.*` (seven) | the one registration of each name (`call [rax+0x40]`, floats `0x48`) | storage `lea r8` within 0x20 bytes before |
| `quantum.*` | one unique pattern each; `quantum.audio_system` is the global the match reads at `+0x2F` | |

Not checked by any row (sc-offline keeps them as constants): the mission manager's slot 0x48 and the library offset read from it at run time, `kLoadXmlFileSlot` 0x3C0 / `kLoadXmlBufferSlot` 0x3B8, the console slots 0x130 / 0xC0 and cvar slots 0x38 / 0x20 (0x130 is checked by `cvars.console`'s sites), the zone grid slot 0x30 and physical world slot 0x1C0 the ground ray calls, the entity registry slots 0xC0 / 0x20, and the quantum drive fields (`+0xB60`, `+0xC48`, `+0xCA8`, `+0x7F0`, `+0x800`, ...).
