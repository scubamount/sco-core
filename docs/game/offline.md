# Rows for sc-offline's offline patches and startup hooks

Rows that sc-offline's boot / offline-mode patches (`src/patches.cpp`), its startup hooks (`src/hooks.cpp`) and its `CSystem::Quit` hook (`src/dllmain.cpp`) used to find by scanning the game themselves, moved into sco-core (`src/game/offline_sigs.cpp`, `include/sco/game/offline.h`) so `sco-sigcheck` checks them against a new `StarCitizen.exe` before anyone plays. Each resolver is the old scan, moved byte for byte. Where the old scan took "every match", the row insists on the count found in 4.10.196.36804 (numbered rows), so a patch that adds or loses a site shows as a failed row.

Rows only find addresses. sc-offline still writes the patches and installs the hooks, after `ResolveAll` (rows match the game's original bytes), and keeps its per-patch report lines (`[+]/[!] <patch>: patched N site(s)`). Each patch or hook runs only when its capability is ready. The OR-loop bound patch uses `offline.or_loop_bound` from [features](features.md); the fleet manager retrieve hook uses the ASOP rows `asop.fleet_retrieve` and `atc.get_component` ([ASOP](asop.md)), whose pattern and checks are the ones sc-offline scanned for.

| Row | Found by | Checks | Capability |
|---|---|---|---|
| `offline.is_online_store` | pattern `44 88 A0 0E 06 00 00` (`mov [rax+0x60E], r12b`) | unique | `offline.force_offline` |
| `offline.handshake_gate.1`..`.4` | every `cmp [rip+X], r8b..r15b` with `0F 84` at +7 and `lea rdx, "sessionGuid"` within +13..+25, in address order | exactly 4; all compare the same flag; je at +7 | `offline.handshake` |
| `offline.is_online_flag` | RIP target of `offline.handshake_gate.1` | inside the image | `offline.handshake`, `offline.asop_shard_gate` |
| `offline.megamap_tolower_call` | the `lea r8` handler within 32 bytes after `lea rdx, "Load a map, same usage as 'megamap' cvar."`; in its first 0x60 bytes, the `call` at +15 of two `lea rcx, [rsp+0x30]; call` pairs | handler inside .text; callee prologue `4C 8B 09 4C 8B D1 41 0F B6 01 84 C0`; exactly one | `offline.megamap_case` |
| `offline.boot_frontend_request` | every `lea r8, "SC_Frontend"` directly followed by `lea rdx, "Frontend_Main"` | exactly one | `offline.boot_pu`, `offline.boot_pu_all` |
| `offline.str_pu`, `offline.str_sc_default`, `offline.str_megamap_pu_all` | the .rdata strings `PU`, `SC_Default`, `MegaMap.PU_All` | present | `offline.boot_pu` / `offline.boot_pu_all` |
| `offline.offline_db_path.1`..`.2` | every `lea r8, "Libs/OfflineDB"`, in address order | exactly 2 | `offline.db_path` |
| `offline.str_user` | the .rdata string `%USER%` | present | `offline.db_path` |
| `offline.asop_gate_open` | unique pattern `44 89 AD D0 00 00 00 44 38 2D ...` | `cmp` at +7 reads `offline.is_online_flag` + 1 | `offline.asop_shard_gate` |
| `offline.asop_gate_validation` | unique pattern `80 3D ?? ?? ?? ?? 00 75 ?? ...` | `cmp` reads `offline.is_online_flag` + 1; `lea r9` at +0x1B loads "Can only perform ASOP operations in the PU." | `offline.asop_shard_gate` |
| `offline.restricted_area_impound` | unique prologue pattern `40 55 53 56 57 ... 33 D2 48 8B F1 E8` | `lea r9` at +0x59 loads the "... impounded (or destroyed) by restricted area ..." format; `lea r8` at +0x79 loads "Boundary Violation" | `offline.no_impound` |
| `offline.use_service_default`, `offline.use_online_mission_service_default` | 6 bytes before the first `lea rdx, "contract_broker.use_service"` / `"contract_broker.use_online_mission_service"` | `41 B9 01 00 00 00` there; `4C 8D 43` at lea-0xF | `offline.mission_services` |
| `offline.social_group.1`..`.23` | every `mov rcx, [rip+env]` followed by `call [rax+0x18]` (0x30), a slot 0x70 load or call (0x18) and a slot 0xB0 load or call (0x50), in address order | exactly 23; all load the same global | `offline.social_group` |
| `offline.services_env` | RIP target of `offline.social_group.1` | inside the image | `offline.social_group` |
| `offline.service_stream.presence` / `.analytics` / `.trace` / `.echo.1` / `.echo.2` | for each reference to the service's `CAsyncClient<...>::CreateClientStream::<lambda_3>` name, its function's .pdata start | within 0x200 of the reference; prologue `48 89 5C 24 18 55 56 57 41 56 41 57`; exactly 1 per service, 2 for echo | `offline.service_streams` |
| `offline.remote_console_bind` | within 0x100 before the reference to "Remote console listening on: %u\n", pattern `33 C9 FF 15 ?? ?? ?? ?? 0F B7 CB 66 89 7C 24 48 89 44 24 4C` | one reference; exactly one match | `offline.remote_console` |
| `offline.profiler_listen_branch` | unique pattern `73 ?? 0F 1F 40 00 ... FF 15` | unique | `offline.profiler_server` |
| `inventory.validate_filter`, `inventory.validate_projection` | unique prologue patterns | unique | `inventory.filter_validator`, `inventory.projection_validator` |
| `elevator.instance_group_query` | unique prologue pattern `48 89 5C 24 08 ... 49 8B 00 49 8B F8` | `48 8B 0D` at +0x5B; `48 8B 01 FF 50 18` at +0x99 | `elevator.crash_guard` |
| `elevator.services_manager` | RIP target of the `mov rcx` at `elevator.instance_group_query` +0x5B | inside the image | `elevator.crash_guard` |
| `fleet.entitlements_result` | unique prologue pattern `40 55 56 41 56 ... 48 83 C1 08 E8` | `lea r9` at +0x55 loads "QueryEntitlements failed with error: $$" | `fleet.ship_list` |
| `fleet.takeoff_command` | 0x1CD before the first `lea rdx, "Invalid arguments. Usage: g_ATC_requestTakeOff ..."` | prologue `40 55 53 48 8B EC 48 83 EC 78`; `E8` at +0x72, +0xA9, +0x190 | `fleet.retrieve_atc` |
| `fleet.string_ctor`, `fleet.string_dtor`, `fleet.request_taking_off` | targets of the calls at `fleet.takeoff_command` +0x72, +0xA9, +0x190 | inside the image | `fleet.retrieve_atc` |
| `system.quit_fast_shutdown` | `system.quit` + 0x31A | `49 8B 8E ?? ?? ?? ?? 48 85 C9 74 0A 48 8B 01 FF 50 10 85 C0 75 0E 41 80 BE ?? ?? ?? ?? 00 0F 84`; CSystem offsets at +3 and +0x19 | `system.quit_hook` |

Capabilities (`sco::game::offline::Capabilities`): `offline.force_offline`, `offline.handshake`, `offline.megamap_case`, `offline.boot_pu`, `offline.boot_pu_all`, `offline.db_path`, `offline.asop_shard_gate`, `offline.no_impound`, `offline.mission_services`, `offline.social_group`, `offline.service_streams`, `offline.remote_console`, `offline.profiler_server`, `inventory.filter_validator`, `inventory.projection_validator`, `elevator.crash_guard`, `fleet.ship_list`, `fleet.retrieve_atc` (with `asop.fleet_retrieve`, `atc.get_component`), `system.quit_hook` (with `system.quit`).

Checked locally with `sco-sigcheck` on 4.10.196.36804: 148/148 OK, every address equal to what sc-offline's old scans found.
