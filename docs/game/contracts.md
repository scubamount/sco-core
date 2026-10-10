# Contracts rows

sc-offline's offline contracts (`src/contracts.cpp` there) take every game address from these rows ([`src/game/contracts_sigs.cpp`](../../src/game/contracts_sigs.cpp), [`include/sco/game/contracts.h`](../../include/sco/game/contracts.h)). The scans moved byte for byte from contracts.cpp; addresses below are RVAs in 4.10.196.36804. The reputation-service checks contracts patches are `contracts.reputation_*` in [features](../../include/sco/game/features.h).

Terms used in the table:

- **string fn**: the function (from `.pdata`) holding the first `lea r64, [rip+string]` (REX 48 or 4C) of a `.rdata` string. The prologue is checked; no other function referencing the string may have that prologue (`AMBIG` otherwise), and without a prologue no other function may reference it.
- **pattern**: unique in `.text`. **call +N** / **RIP +N**: the target of the call or RIP-relative operand at +N in the row named. **in range N**: the one match within the first N bytes of the row named.
- **vtable**: the one `.rdata` slot holding a function, minus the slot offset. Slots are compared both as the loaded game holds them (relocated) and as the file holds them (the PE header's preferred base), so sco-sigcheck sees what the game sees.

| Row | RVA | How found | Checks | Capability |
|---|---|---|---|---|
| `contracts.query_reply` | 0x1b85920 | string fn "DebugAcceptGeneratorContract QueryAvailableContracts Success" | prologue | list |
| `contracts.mission_system_site` | 0x1b85dbd | in query_reply's first 0x1000 bytes: `call get; mov rcx, rax; call field` | get = `mov rax, [rip+X]; ret`, field = `mov rax, [rcx+imm8]; ret`; exactly one such site (3 calls to such a get) | list |
| `contracts.mission_system` | 0xa065150 | RIP of get (+3) | inside the image | list |
| `contracts.mission_generator_get` | 0xa136b0 | call +8 of the site | (sc-offline reads the imm8 at +3: 0x70) | list |
| `contracts.accept_stub` | 0x1bc31b0 | 0xC before the first `lea r8, "CContractBrokerOffline::AcceptContract is not implemented yet"` | prologue | accept |
| `contracts.accept_resolve` | 0x17df590 | call +0x5E of accept_stub | E8, callee prologue | accept |
| `contracts.accept_slot` | 0x82657c8 | the `.rdata` slot holding accept_stub | exactly one | accept |
| `contracts.auto_accept_caller.1`, `.2` | 0x1bb6320, 0x1bbd9d0 | distinct functions of the leas of "DebugAcceptGeneratorContract AcceptContract(after creation) Sent" | exactly 2 (4 leas) | auto_accept |
| `contracts.factory_create` | 0x1c15af0 | string fn "CMissionFactory::CreateMission" | prologue | mission_creation |
| `contracts.copy_property_map` | 0x17ff670 | pattern | | mission_creation |
| `contracts.get_shard_graph` | 0x28dc290 | string fn CUniverseHierarchyShardStore::GetUniverseShardGraph | prologue | mission_creation |
| `contracts.dump_shard_graph` | 0x289d9b0 | string fn "soc_dumpShardGraph: no shard id supplied ..." | one function | mission_creation |
| `contracts.shard_store_site` | 0x289da5a | in range 0x100 of dump_shard_graph: `mov rcx, [rip+X]; call` | callee is `mov rax, [rcx+disp32]; ret` | mission_creation |
| `contracts.shard_store_owner` | 0xa1a9b30 | RIP +0 of the site | inside the image | mission_creation |
| `contracts.shard_store_get` | 0x2d049e0 | call +7 of the site | (sc-offline reads the disp32 at +3: 0x688) | mission_creation |
| `contracts.mission_service_request` | 0x1c24000 | string fn "Mission service not accessible" | prologue | mission_creation |
| `contracts.urn_site` | 0x1c2439d | in range 0x600 of mission_service_request | one match | mission_creation |
| `contracts.urn_from_entity`, `.assign_urn` | 0x442130, 0x42b3d0 | call +10 / +25 of urn_site | | mission_creation |
| `contracts.string_init_site` | 0x1c24212 | in range 0x600 of mission_service_request | one match | mission_creation |
| `contracts.string_init` | 0x3788f0 | call +14 of string_init_site | | mission_creation |
| `contracts.add_mission` | 0x1bcb960 | string fn CSCPlayerMissionLog::AddMission | prologue | mission_log |
| `contracts.log_handle_site` | 0x1bcbf3b | in range 0x800 of add_mission | one match | mission_log |
| `contracts.log_handle`, `.make_phase_handler` | 0x3d1150, 0x1ca0570 | call +0x15 / +0x20 of log_handle_site | | mission_log |
| `contracts.phase_activate_site` | 0x1bc5260 | pattern | | mission_log |
| `contracts.phase_activate` | 0x1cad6c0 | call +33 | callee prologue | mission_log |
| `contracts.hauling_assign_site` | 0x1ca8ed7 | pattern | | mission_log |
| `contracts.hauling_assign` | 0x19449e0 | call +13 | callee prologue | mission_log |
| `contracts.copy_details_site` | 0x1bcbb40 | in range 0x400 of add_mission | one match | mission_log |
| `contracts.copy_details` | 0x1b6ca80 | call +19 | callee prologue | mission_log |
| `contracts.add_player` | 0x1bcdc90 | AddMission's callers with, within 0x20 bytes, a call preceded by `lea r64, [r64+0x80]`: that call's target | exactly 2 such callers (of 5), same target | mission_log |
| `contracts.add_active_objective` | 0x1bc5c30 | string fn CSCPlayerMissionLog::AddActiveObjective | prologue | mission_diagnostics |
| `contracts.add_active_player` | 0x1bc7710 | string fn CMissionEntity::AddActivePlayer | prologue | mission_diagnostics |
| `contracts.notify_ui_objective` | 0x1bffac0 | string fn "Notify UI Objective" | prologue | mission_diagnostics |
| `contracts.is_objective_hidden` | 0x1c7ca50 | string fn CSCPlayerMissionLog::IsObjectiveHidden | prologue | mission_diagnostics |
| `contracts.warehouse_process_queue` | 0x1cd59c0 | string fn CMissionWarehouseOrderHandler::ProcessQueue | prologue | mission_diagnostics |
| `contracts.create_mission_log_entry` | 0x1c1c270 | string fn "CMissionEntity::CreateMissionLogEntry" | prologue (2 functions reference it, one matches) | mission_diagnostics |
| `contracts.module_initialize` | 0x1c73530 | string fn CSubsumptionMissionComponent::Initialize | prologue | mission_modules |
| `contracts.module_start_mission` | 0x1d1b660 | string fn "CSubsumptionMissionComponent::StartMission" | prologue; +0x111 `lea rdx, [r15+0x150]` (module mission id); +0x769 `cmp dword [r15+0x130], 1` (module state) | mission_modules |
| `contracts.module_authority` | 0x1c637d0 | string fn "HandleAuthorityChangeEvent" | prologue | mission_modules |
| `contracts.module_entry_answer` | 0x1ba82a0 | string fn "Aborting subsumption mission module $$($$)" | prologue (2 functions reference it, one matches); +0x34E `mov eax, [rdx+0x130]` (module state) | mission_modules |
| `contracts.objective_to_player_logs` | 0x1bc6700 | string fn "AddActiveObjectiveToPlayerLogs" | prologue | mission_modules |
| `contracts.create_objective` | 0x1c1fe90 | string fn "$$[$$] - Created: $$[$$], parent id=$$, flags=$$" | prologue | mission_modules, abandon |
| `contracts.loc_id` | 0x3b6700 | pattern | | mission_modules |
| `contracts.stop_mission` | 0x1d1f6c0 | string fn "StopMission called with reason ..." | prologue | mission_modules |
| `contracts.mission_entity_vtbl` | 0x823d1d0 | vtable: the slot holding create_objective, minus 0x708 | exactly one slot | abandon |
| `contracts.mission_entity_remove_player` | 0x1cf8610 | slot 0x720 of mission_entity_vtbl | prologue | abandon |
| `contracts.offline_end_hauling` | 0x1cf9c30 | string fn "CMissionServiceOffline::RequestEndHaulingObjectiveAndPhase is not implemented yet" | one function | offline_service, abandon, phase_ends |
| `contracts.offline_service_vtbl` | 0x81d4ad0 | vtable: the slot holding offline_end_hauling, minus 0xC0 | exactly one slot | offline_service, abandon, phase_ends |
| `contracts.offline_leave_mission` | 0x1cf45d0 | slot 0x18 of offline_service_vtbl | prologue | abandon |
| `contracts.offline_end_phase` | 0x1cfaf10 | slot 0x48 of offline_service_vtbl | prologue | phase_ends |
| `contracts.send_rewards_authority` | 0x1d09b5a | 14 bytes before the first `lea r8, "CSCPlayerMissionLog::SendRewards No authority"` | `call [rax+disp32]; test al, al; jnz` (sc-offline nops the jnz at +8) | rewards |
| `contracts.find_entry` | 0x1c4f550 | function holding a pattern | prologue | rewards |
| `contracts.find_entry_any` | 0x1c4eef0 | function holding a pattern | prologue | rewards |
| `contracts.total_reward` | 0x1c5fa40 | string fn CMissionLogEntry::GetTotalReward | prologue | rewards |
| `contracts.update_balance` | 0x46de660 | string fn "CWallet::UpdateCurrencyBalanceValue" | prologue | rewards, shop_payments |
| `contracts.async_update_balance` | 0x46a0db0 | string fn "CWallet::AsyncUpdateCurrencyBalance" | prologue | shop_payments |
| `contracts.end_mission` | 0x1c30870 | pattern | | steps |
| `contracts.em_module_finished` | 0x1ca8450 | string fn CEnvironmentalMissionManager::OnMissionModuleFinished | prologue | steps |
| `contracts.actor_kill` | 0x2edb6d0 | string fn "CActor::Kill" | prologue | steps |
| `contracts.send_comms` | 0x1d07190 | string fn "SendCommsNotification Record GUID ..." | prologue | steps |
| `contracts.helper_spawned` | 0x1b84d50 | string fn "Succeeded to spawn delivery mission helper. ..." | prologue | steps |
| `contracts.mission_settings` | 0xa065158 | RIP +3 of a pattern | the pattern holds `vmovss xmm1, [rax+0x174]` (the radius sc-offline writes) | stream_radius |
| `contracts.phase_site` | 0x1c0a175 | pattern | | phases |
| `contracts.create_phase` | 0x1c097f0 | call +65 | callee prologue | phases |
| `contracts.find_phase` | 0x56e1f0 | call +4 | | phases |
| `contracts.flow_site`, `.flow_free_site` | 0x1d34226, 0x1d344dd | patterns | | phases |
| `contracts.temp_alloc`, `.flow_context`, `.update_flow` | 0x3b49c0, 0x1b6d020, 0x1d34500 | call +24 / +59 / +76 of flow_site | | phases |
| `contracts.free_flow_map` | 0x1b700c0 | call +17 of flow_free_site | | phases |
| `contracts.objective_site`, `.objective_copy_site` | 0x1b9d988, 0x1b633e0 | patterns | | phases |
| `contracts.objective_init`, `.empty_loc`, `.timer_init` | 0x4bb6b0, 0x3deba0, 0x63bf80 | call +4 / +39 / +81 of objective_site | | phases |
| `contracts.objective_copy` | 0x4bb580 | call +0 of objective_copy_site | | phases |
| `contracts.active_objective_vtbl` | 0x8230db8 | RIP +5 of objective_copy_site | inside the image | phases |
| `contracts.pending_to_objective`, `.activate_token` | 0x1c05e20, 0x1bc4bb0 | patterns | | phases |

## Offsets sc-offline uses that no row checks

contracts.cpp reads and writes many game structures it builds or receives at runtime (objectives, phase tokens, mission details, the contract broker's offers). Only offsets encoded in code a row already locates can be checked; the rest are listed here.

| Offset | Where | Status |
|---|---|---|
| module +0x130 (state), +0x150 (mission id) | module_start_mission, module_entry_answer | checked (rows above) |
| CMissionEntity vtable +0x708, +0x720 | mission_entity_vtbl, mission_entity_remove_player | checked: the vtable is found by +0x708, +0x720's prologue confirms it |
| offline service vtable +0x18, +0x48, +0xC0 | offline_service_vtbl and its slot rows | checked |
| mission settings +0x174 | mission_settings | checked (in the pattern) |
| generator +imm8 (0x70), shard store +disp32 (0x688) | mission_generator_get, shard_store_get | read from the game's code |
| module vtable +0x6C0 (Initialize) | compared with module_initialize | unchecked; a drift only stops sc-offline recognising modules |
| module vtable +0x6C8, module +0x1D0, +0x528 | creating a mission instance | unchecked (`kModuleCreateInstance`, `kModuleHasInstance`, `kModuleInstanceFailed`) |
| CMissionEntity vtable +0x710, +0x718 | ending an objective, setting a marker | unchecked (`kEntityEndObjective`, `kEntitySetMarker`) |
| mission system +0x68 (service), +0x78 (factory), +0x80 (broker); factory vtable +0x10; broker vtable +0x28; mission service vtable +0x28, +0xA0 | contract accept and mission creation | unchecked; the factory and broker slots are compared with factory_create / sc-offline's hook before use |
| objective, mission details, phase token, offer and request layouts (0x18, 0x80, 0xB8, 0x110, ...) | built or read by sc-offline | unchecked: runtime data no located code encodes |
| entity system +0x120, +0x128; entity +0x318, +0x2B8, +0x6B8; mission entity +0x1B8 | positions, zones, master mission | unchecked (shared with teleport and spawn) |
