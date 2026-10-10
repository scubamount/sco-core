#pragma once
// Addresses behind sc-offline's offline contracts (src/contracts.cpp there): the contract list and
// accept, mission creation, the mission log, the mission modules a contract runs, its phases and
// objectives, rewards and the offline mission service. Resolved by sco::ResolveAll(); the rows are
// in src/game/contracts_sigs.cpp and docs/game/contracts.md lists each one with what it checks.
// The reputation-service checks contracts patches are in sco/game/features.h
// (contracts.reputation).
//
// Rows are grouped into capabilities like features.h: sc-offline asks for a group with
// sco::caps::SetFromSignatures(c.name, c.rows, c.count) and uses its rows only when it's ready.
//
// Rows only find addresses. Hooks, the vtable slot writes and the reward patch stay in sc-offline,
// which resolves before patching: a row matches the game's original bytes.
//
// How rows are found (all moved byte for byte from contracts.cpp):
// - "string function": the function holding the first lea (48/4C 8D, RIP-relative) of a .rdata
//   string, from .pdata, whose prologue must match; no other function referencing the string may
//   have the same prologue (Ambiguous otherwise). Without a prologue every lea must be in the
//   same function.
// - "pattern": a unique .text pattern; "call at +N" / "RIP at +N": the target of a call or
//   RIP-relative operand inside another row; "in range": the one match of a pattern within the
//   first N bytes of another row.
// - "vtable": .rdata slots are compared as the loaded game holds them (relocated) or as the file
//   holds them (preferred image base from the PE header), so sco-sigcheck sees what the game sees.
#include <cstddef>
#include <cstdint>

namespace sco::game::contracts {

struct Capability {
    const char*        name;    // "contracts.list"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of the contracts feature, in a fixed order.
const Capability* Capabilities(size_t& count);

// ---- contracts.list -----------------------------------------------------------------------------
// contracts.query_reply: string function "DebugAcceptGeneratorContract QueryAvailableContracts
//   Success", prologue 48 89 4C 24 08 55 53 56 57 41 55.
// contracts.mission_system_site: within query_reply's first 0x1000 bytes, the one
//   "call get; mov rcx, rax; call field" where get is "mov rax, [rip+X]; ret" and field is
//   "mov rax, [rcx+imm8]; ret" (4.10.196.36804: 3 calls to such a get, one with such a field).
// contracts.mission_system: the global get reads (RIP at get+3).
// contracts.mission_generator_get: the field getter (call at +8); sc-offline reads the generator
//   offset from its imm8 at +kGeneratorOffDisp.
constexpr size_t kGeneratorOffDisp = 3;

// ---- contracts.accept ---------------------------------------------------------------------------
// contracts.accept_stub: CContractBrokerOffline::AcceptContract, 0xC before the first
//   lea r8, "CContractBrokerOffline::AcceptContract is not implemented yet"; prologue checked.
// contracts.accept_resolve: the future's resolve, call at accept_stub+0x5E; prologue checked.
// contracts.accept_slot: the one .rdata slot holding accept_stub (the broker's vtable entry
//   sc-offline replaces).

// ---- contracts.auto_accept ----------------------------------------------------------------------
// contracts.auto_accept_caller.1 .. .<kAutoAcceptCallers>: the distinct functions holding a lea of
//   "DebugAcceptGeneratorContract AcceptContract(after creation) Sent", in address order of the
//   leas (4.10.196.36804: 4 leas in 2 functions).
constexpr int kAutoAcceptCallers = 2;

// ---- contracts.mission_creation -----------------------------------------------------------------
// contracts.factory_create: string function "CMissionFactory::CreateMission".
// contracts.copy_property_map: pattern.
// contracts.get_shard_graph: string function CUniverseHierarchyShardStore::GetUniverseShardGraph.
// contracts.dump_shard_graph: string function "soc_dumpShardGraph: no shard id supplied ..." (no
//   prologue check).
// contracts.shard_store_site: in range 0x100 of dump_shard_graph, "mov rcx, [rip+X]; call get"
//   whose get is "mov rax, [rcx+disp32]; ret".
// contracts.shard_store_owner: RIP at shard_store_site+0. contracts.shard_store_get: call at +7;
//   sc-offline reads the store's offset from its disp32 at +kShardStoreOffDisp.
constexpr size_t kShardStoreOffDisp = 3;
// contracts.mission_service_request: string function "Mission service not accessible".
// contracts.urn_site, contracts.string_init_site: in range 0x600 of mission_service_request.
// contracts.urn_from_entity, contracts.assign_urn: calls at urn_site+10 / +25.
// contracts.string_init: call at string_init_site+14.

// ---- contracts.mission_log ----------------------------------------------------------------------
// contracts.add_mission: string function CSCPlayerMissionLog::AddMission.
// contracts.log_handle_site: in range 0x800 of add_mission; contracts.log_handle and
//   contracts.make_phase_handler: calls at +0x15 / +0x20.
// contracts.phase_activate_site: pattern; contracts.phase_activate: call at +33, prologue checked.
// contracts.hauling_assign_site: pattern; contracts.hauling_assign: call at +13, prologue checked.
// contracts.copy_details_site: in range 0x400 of add_mission; contracts.copy_details: call at +19,
//   prologue checked.
// contracts.add_player: AddMission's callers followed within 0x20 bytes by a call with a
//   "lea r64, [r64+0x80]" before it: that call's target. Exactly kAddPlayerSites such callers,
//   all calling the same function.
constexpr int kAddPlayerSites = 2;

// ---- contracts.mission_diagnostics --------------------------------------------------------------
// contracts.add_active_objective, .add_active_player, .notify_ui_objective, .is_objective_hidden,
// .warehouse_process_queue: string functions (their names, or "Notify UI Objective").
// contracts.create_mission_log_entry: string function "CMissionEntity::CreateMissionLogEntry" (2
//   functions reference it; only one has the prologue).

// ---- contracts.mission_modules ------------------------------------------------------------------
// contracts.module_initialize, .module_start_mission, .module_authority, .module_entry_answer,
// .objective_to_player_logs, .create_objective: string functions. module_start_mission also checks
//   lea rdx, [r15+kModuleMission] at +0x111 and cmp dword [r15+kModuleState], 1 at +0x769;
//   module_entry_answer checks mov eax, [rdx+kModuleState] at +0x34E.
constexpr size_t kModuleState   = 0x130;   // checked (see above)
constexpr size_t kModuleMission = 0x150;   // checked (see above)
// contracts.loc_id: pattern.

// ---- contracts.abandon --------------------------------------------------------------------------
// contracts.stop_mission: string function "StopMission called with reason ...".
// contracts.mission_entity_vtbl: the CMissionEntity vtable: the one .rdata slot holding
//   create_objective, minus kEntityCreateObjectiveSlot.
// contracts.mission_entity_remove_player: the function in slot kEntityRemovePlayerSlot of that
//   vtable, prologue checked (sc-offline used to check the prologue of the live entity's slot).
constexpr size_t kEntityCreateObjectiveSlot = 0x708;
constexpr size_t kEntityRemovePlayerSlot    = 0x720;
// contracts.offline_service_vtbl: CMissionServiceOffline's vtable: the one .rdata slot holding
//   offline_end_hauling, minus kServiceEndHaulingSlot.
// contracts.offline_end_hauling: string function "CMissionServiceOffline::
//   RequestEndHaulingObjectiveAndPhase is not implemented yet" (no prologue check).
// contracts.offline_leave_mission / contracts.offline_end_phase: the functions in slots
//   kServiceLeaveMissionSlot / kServiceEndPhaseSlot, prologues checked.
constexpr size_t kServiceLeaveMissionSlot = 0x18;
constexpr size_t kServiceEndPhaseSlot     = 0x48;
constexpr size_t kServiceEndHaulingSlot   = 0xC0;

// ---- contracts.rewards / contracts.shop_payments / contracts.steps -------------------------------
// contracts.send_rewards_authority: "call [rax+disp32]; test al, al; jnz" 14 bytes before the first
//   lea r8, "CSCPlayerMissionLog::SendRewards No authority"; sc-offline nops the 6-byte jnz at
//   +kSendRewardsJnz.
constexpr size_t kSendRewardsJnz = 8;
// contracts.find_entry, contracts.find_entry_any: the function holding a unique pattern, prologue
//   checked. contracts.total_reward, .update_balance, .async_update_balance, .em_module_finished,
//   .actor_kill, .send_comms, .helper_spawned: string functions. contracts.end_mission: pattern.

// ---- contracts.stream_radius --------------------------------------------------------------------
// contracts.mission_settings: RIP at +3 of a unique pattern that also checks the radius load
//   vmovss xmm1, [rax+kStreamRadius].
constexpr size_t kStreamRadius = 0x174;

// ---- contracts.phases ---------------------------------------------------------------------------
// contracts.phase_site: pattern; contracts.create_phase: call at +65, prologue checked;
//   contracts.find_phase: call at +4.
// contracts.flow_site, contracts.flow_free_site: patterns; contracts.temp_alloc,
//   .flow_context, .update_flow: calls at flow_site+24 / +59 / +76; contracts.free_flow_map: call
//   at flow_free_site+17.
// contracts.objective_site, contracts.objective_copy_site: patterns; contracts.objective_init,
//   .empty_loc, .timer_init: calls at objective_site+4 / +39 / +81; contracts.objective_copy: call
//   at objective_copy_site+0; contracts.active_objective_vtbl: RIP at objective_copy_site+5.
// contracts.pending_to_objective, contracts.activate_token: patterns.

// ---- offsets sc-offline uses that no row checks -------------------------------------------------
// A drift in these isn't caught here; docs/game/contracts.md lists every one with the reason.
constexpr size_t kModuleInitSlot       = 0x6C0;   // module vtable: Initialize (compared, so a drift only hides modules)
constexpr size_t kModuleCreateInstance = 0x6C8;   // module vtable: create the mission instance (called)
constexpr size_t kModuleHasInstance    = 0x1D0;   // module: byte
constexpr size_t kModuleInstanceFailed = 0x528;   // module: byte
constexpr size_t kEntityEndObjective   = 0x710;   // CMissionEntity vtable slot (called)
constexpr size_t kEntitySetMarker      = 0x718;   // CMissionEntity vtable slot (called)

}  // namespace sco::game::contracts
