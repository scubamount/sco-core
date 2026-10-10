#pragma once
// Addresses behind sc-offline's own features: noclip's fly mode (spawner), Clear NPCs (npc), the
// quantum effect warm-up (quantum), the reputation-service checks contracts patch (contracts) and
// the offline OR-loop bound patch (offline). Resolved by sco::ResolveAll(); the rows are in
// src/game/features_sigs.cpp, and docs/game/features.md lists each one with what it checks.
//
// Rows are grouped into capabilities like the ASOP ones (sco/game/asop.h): a feature asks for its
// group with sco::caps::SetFromSignatures(c.name, c.rows, c.count) and runs only when it's ready.
//
// Rows only find addresses. Patching (contracts, offline) and hooking stay in the product, and
// resolve before patching: a row matches the game's original bytes.
#include <cstddef>
#include <cstdint>

namespace sco::game::features {

struct Capability {
    const char*        name;    // "spawn.fly_mode"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of these features, in a fixed order.
const Capability* Capabilities(size_t& count);

// spawn.request_fly_mode: CSCActorActionHandler::Request<SFlyMode> (the wrapper the game calls).
// npc.remove_entity_call: the first of the entity system's RemoveEntity calls (mov rcx, [entity
//   system]; ...; call [rax+slot]); every such call agrees on the slot, read at +kRemoveSlotDisp.
constexpr size_t kRemoveSlotDisp = 15;
// quantum.send_effect_tag: the EntityEffectSystem tag sender.
// contracts.reputation_services: the services global the reputation checks load.
// contracts.reputation_check.1 .. .<kReputationChecks>: each check site, in address order per
//   form (form A tail "48 8B 51 58 48 8B C8 FF D2", then form B "4C 8B 41 58 48 8B C8 41 FF D0").
//   The tail starts at +kReputationTail; contracts writes its patch at +kReputationPatch.
constexpr int    kReputationChecks = 10;   // 4.10.196.36804: 9 of form A, 1 of form B
constexpr size_t kReputationTail   = 16;
constexpr size_t kReputationPatch  = 13;
// offline.or_loop_bound.1 .. .<kOrLoopSites>: the four OR-loop sites; the bound check sc-offline
//   rewrites is at +kOrLoopPatch.
constexpr int    kOrLoopSites = 4;
constexpr size_t kOrLoopPatch = 27;

}  // namespace sco::game::features
