#pragma once
// Addresses behind sc-offline's actor features: the ship/entity spawner, seat control, Daymar
// lookup, Flight Ready, noclip's fly speed, god mode and prefab spawning (spawner), Clear NPCs'
// direct removal fallback (npc), infinite ammo (ammo) and the gear menu / outfit loader (loadout).
// Resolved by sco::ResolveAll(); the rows are in src/game/actors_sigs.cpp, and docs/game/actors.md
// lists each one with what it checks.
//
// Rows are grouped into capabilities like features.h: a feature asks for its group with
// sco::caps::SetFromSignatures(c.name, c.rows, c.count) and runs only when it's ready. Fly mode
// and Clear NPCs' RemoveEntity slot are rows of features.h (spawn.request_fly_mode,
// npc.remove_entity_call).
//
// Rows only find addresses. Hooking (ammo) and calling stay in the product.
#include <cstddef>
#include <cstdint>

namespace sco::game::actors {

struct Capability {
    const char*        name;    // "spawn.helpers"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of these features, in a fixed order.
const Capability* Capabilities(size_t& count);

// ---- spawner (sc-offline src/spawner.cpp) -------------------------------------------------------
//
// spawn.landing_helper: the landing-area spawn helper: the first of the kLandingAreaRefs functions
//   that load "Landing Area could not be found." with lea r9, minus 0x59. Checks its prologue,
//   mov cl, imm8 + call at +0x4F, mov edx, 0x1000 at +0x40C and the calls at +0x46D, +0x47D,
//   +0x55C.
// spawn.team_tag: the imm8 at helper +0x50 (the helper's own team tag), read by the product.
// spawn.team_category / .set_flags / .set_class / .set_location: the calls at helper +0x51,
//   +0x46D, +0x47D, +0x55C.
// spawn.params_ctor, spawn.find_seat: unique patterns (the spawn params constructor, the seat
//   picker).
// spawn.seat_callback: the seat picker's per-seat callback (lea at find_seat +0x21C). Checks the
//   ports call [rax+0x778] at find_seat +0x1D6, the occupant test cmp [rdi+0x158], 0 at
//   callback +0x37 and the calls the seat rows read.
// spawn.is_linked / .force_delink / .for_each_seat / .actor_of_user / .handle_to_id /
//   .actor_link / .force_link: the calls at find_seat +0xB2, +0x19C, +0x242, +0x3CE, +0x3F1,
//   +0x3FE, +0x40B; spawn.seat_priority: the call at seat_callback +0xD6.
// spawn.find_entity_by_name: the function whose lea rcx at +0x36 loads
//   "FindEntityByName_SlowDebugCodeOnly: %s"; exactly one has the checked prologue.
// spawn.toggle_flight_ready: the Events/ISC/Dashboards.h sender for line 0x49 (Flight Ready);
//   exactly one sender matches.
// spawn.game_cvars: the game CVars global (mov [rip+X], rax after the 0x98-byte allocation).
// spawn.fly_speed_scaler: the lea r8, [rdi+disp32] 0x14 bytes before the lea rdx of
//   "g_FlyModeSpeedScaler"; exactly one. disp32 (at +kFlySpeedDisp) is the fly speed field
//   offset in the CVars object, checked to be in (0, 0x4000).
// spawn.god_mode_call: the call into the god mode setter (unique pattern, encodes the actor
//   component's +kGodModeData and the data's +kGodModeState), whose lea r9 at -0x64 loads the
//   "has changed GodMode State" message. spawn.set_god_mode: its target; the state byte's
//   offset is the disp32 at +kGodModeByteDisp (mov [rcx+disp32], dl).
// spawn.prefab_site: the first of the kObjectContainersRefs lea rdx of "ObjectContainers\%s";
//   checks the system load, path calls, attribute setter and the "ocFilename" lea.
//   spawn.prefab_system / .prefab_attr_writer / .prefab_attr_type_id: the RIP targets at
//   +0x37, +0x94 and the call at +0xA0. The product reads the slots at +kPrefab*.
constexpr int    kLandingAreaRefs      = 2;      // 4.10.196.36804
constexpr int    kObjectContainersRefs = 2;      // 4.10.196.36804
constexpr size_t kFlySpeedDisp         = 3;      // in spawn.fly_speed_scaler
constexpr size_t kGodModeByteDisp      = 0xC;    // in spawn.set_god_mode
constexpr size_t kPrefabPathMgrSlot    = 0x43;   // disp32 in spawn.prefab_site
constexpr size_t kPrefabPathIdSlot     = 0x5B;   // disp32
constexpr size_t kPrefabAttrSetSlot    = 0x71;   // disp8

// ---- Clear NPCs (sc-offline src/npc.cpp) ---------------------------------------------------------
//
// npc.direct_remove_call: the call into the internal remove inside RemoveEntity (unique pattern,
//   within 0x700 bytes of its function's start). npc.remove_entity: that function (from .pdata),
//   checked prologue; sc-offline compares it with the entity system's vtable entry at the slot
//   npc.remove_entity_call reads. npc.direct_remove: the call's target.

// ---- infinite ammo (sc-offline src/ammo.cpp) -----------------------------------------------------
//
// ammo.set_ammo: the magazine setter sc-offline hooks (unique pattern; the key load
// mov eax, [rcx+0xC4] is at +0x0E).

// ---- gear menu and outfits (sc-offline src/loadout.cpp) -------------------------------------------
//
// loadout.load_player_loadout: the function whose lea rdx at +0x44 loads "Scripts/Loadouts/Player";
//   exactly one has the checked layout. loadout.game: the global it loads at +0x18. The product
//   reads the framework, actor and load slots from the disp32s at +kLoadout*.
constexpr size_t kLoadoutFrameworkSlot = 0x29;   // call [rax+disp32]
constexpr size_t kLoadoutActorSlot     = 0x33;   // mov rdx, [rcx+disp32]
constexpr size_t kLoadoutLoadSlot      = 0x9B;   // call [rax+disp32]

}  // namespace sco::game::actors
