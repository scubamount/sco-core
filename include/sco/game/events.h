#pragma once
// Addresses behind game.actors 1.1's health and state (actor status) and the game.* bus events
// (docs/game-services.md, docs/design/game-world-spikes.md B4, B5, B6 and B8). Resolved by
// sco::ResolveAll(); the rows are in src/game/events_sigs.cpp, and docs/game/events.md lists each
// one with what it checks. game.zone.changed has no rows of its own: it polls the teleport.* reads.
//
// Rows are grouped into capabilities like sco/game/features.h: the game pack asks for its group
// with sco::caps::SetFromSignatures(c.name, c.rows, c.count) and installs a hook only when the
// group is ready.
//
// Rows only find addresses. Hooking and reading stay in the game pack's services.
#include <cstddef>
#include <cstdint>

namespace sco::game::events {

struct Capability {
    const char*        name;    // "game.actors.health"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of these rows, in a fixed order.
const Capability* Capabilities(size_t& count);

// actor.health_site: the code that logs "...HealthPool: %.4f, Stun: %.4f" (one unique pattern):
//   +0x00 calls the status accessor, +0x0D calls GetStat(status, 6) (Stun), +0x1D and +0x2A call
//   the accessor and GetStat(status, 0x0B) (HealthPool) again. The checks pin the accessor
//   (mov rax,[rcx+208h]; add rax,74A0h; ret) and that both pairs of calls reach the same functions.
// actor.status_accessor: the accessor: actor -> CSCActorStatus (actor + kActorStatusObject +
//   kActorStatus). actor.get_stat: float GetStat(status, int stat).
constexpr size_t   kActorStatusObject = 0x208;   // checked: actor.health_site (accessor)
constexpr size_t   kActorStatus       = 0x74A0;   // checked: actor.health_site (accessor)
constexpr int      kStatHealthPool    = 0x0B;     // checked: actor.health_site +0x22
constexpr int      kStatStun          = 6;        // checked: actor.health_site +0x05
// actor.state_site: the "not fully alive" test, P2(status) || P1(status) (one unique pattern):
//   +0x00 calls the status accessor, +0x0B calls P2, and +0x17 loads status vtable slot
//   kStatusPredicateSlot (P1). actor.is_dead_confirmed: P2, checked to be the function that
//   labels itself CSCActorStatus::IsDeadConfirmed. Which of P1 and P2 means dead and which
//   incapacitated is not pinned by any byte: it needs one in-game run, so game.actors.state stays
//   off and the game pack only logs both (see docs/game-services.md).
constexpr size_t   kStatusPredicateSlot = 0x60;   // checked: actor.state_site +0x17

// event.player_spawn: SCigEventDispatcher::QueueEvent<SPI_Player_OnSpawn>, void (actor*, bool, void*):
//   one unique prologue pattern; +0x2C calls entity slot 0x7A8, +0x60 loads its label.
// event.player_death: QueueEvent<SPI_Player_OnDeath>, void (actor*, const hit_info*, void*): found by
//   its label (+0x414) and .pdata; prologue, +0x22 and +0x69 checked.
// event.seat_enter / event.seat_exit: CSCActorResultStateLinked::Enter / ::Exit,
//   void (state*, host*, const SData*): found by their labels and .pdata; prologues checked.
//   Hooked for log-only diagnostics until the in-game run confirms the payload.

}  // namespace sco::game::events
