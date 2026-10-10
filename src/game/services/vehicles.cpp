// game.vehicles 1.0 (sc_vehicles.h) from the game pack. Moved from sc-offline's spawner seat code
// (src/spawner.cpp: CollectSeat, EnumerateSeats, the occupant decoding, ActorOfEntity,
// LinkEntityToSeat, UnlinkEntity, the pilot seat, the dashboard search and the Flight Ready
// event), same offsets and slots; the addresses come from the spawn.* rows (sco/game/actors.h) and
// the teleport.* reads (reads.h). No scans, no fixed addresses. The crew jobs, the menu and the
// R-key fallback stay in the product.
//
// New here (T3, the Javelin bulk fill): a seat is usable only when the game's own seat picker
// would take it, i.e. its owner has a live IInteractableComponent (row spawn.seat_interactable),
// and seat() refuses the others instead of linking blind. The seat and link lines this file logs
// ("[game] vehicles: ...": every seat's interactable flag and raw occupant field, and each link's
// result) are there so one in-game run tells the remaining causes apart.
#include "vehicles.h"
#include "actors.h"
#include "spawn.h"
#include "sco/caps.h"
#include "sco/game/actors.h"
#include "sco/game/reads.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/signatures.h"
#include <sc_vehicles.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <windows.h>

namespace sco::game::services {

namespace {

using ForEachSeatFn  = void(__fastcall*)(uintptr_t itemContainer, const void* visitor, uint32_t itemType);
using SeatPriorityFn = uint32_t(__fastcall*)(uintptr_t seat);
using ActorOfUserFn  = uint64_t*(__fastcall*)(uintptr_t itemUser, uint64_t* actorHandle);
using HandleToIdFn   = uint64_t*(__fastcall*)(const void* handleField, uint64_t* entityId);
using ActorLinkFn    = uintptr_t(__fastcall*)(uintptr_t actor);
using ForceLinkFn    = void(__fastcall*)(uintptr_t actorLink, uint64_t seatEntityId);
using ForceDelinkFn  = void(__fastcall*)(uintptr_t actorLink);
using IsLinkedFn     = bool(__fastcall*)(uintptr_t actor);
using FlightReadyFn  = void(__fastcall*)(uintptr_t entitySystem, uintptr_t dashboard, const void* callback);

constexpr uint64_t kPtrMask = 0xFFFFFFFFFFFFull;
constexpr uint32_t kMaxSeats = 256;   // seats read per call; more are counted, not read

// The rows each system reads; each list is its capability (caps::SetFromSignatures).
constexpr const char* kSeatsRows[] = {
    "teleport.entity_system", "spawn.landing_helper", "spawn.find_seat", "spawn.seat_callback",
    "spawn.for_each_seat",    "spawn.seat_priority",  "spawn.handle_to_id",
};
constexpr const char* kGateRows[] = { "spawn.seat_callback", "spawn.seat_interactable" };
constexpr const char* kSeatRows[] = {
    "teleport.entity_system", "spawn.landing_helper", "spawn.find_seat",    "spawn.seat_callback",
    "spawn.for_each_seat",    "spawn.seat_priority",  "spawn.handle_to_id", "spawn.seat_interactable",
    "spawn.is_linked",        "spawn.force_delink",   "spawn.actor_of_user", "spawn.actor_link",
    "spawn.force_link",
};
constexpr const char* kFlightReadyRows[] = {
    "teleport.entity_system", "spawn.landing_helper", "spawn.find_seat",    "spawn.seat_callback",
    "spawn.for_each_seat",    "spawn.seat_priority",  "spawn.handle_to_id", "spawn.toggle_flight_ready",
};
constexpr const char kCapSeats[]       = "game.vehicles.seats";
constexpr const char kCapSeat[]        = "game.vehicles.seat";
constexpr const char kCapFlightReady[] = "game.vehicles.flight_ready";

struct Vehicles {
    bool           seats = false, gate = false, seat = false, flightReady = false;
    uintptr_t*     entitySystem = nullptr;   // teleport.entity_system
    uintptr_t*     components = nullptr;     // the global 8 bytes past it (spawn.landing_helper checks)
    ForEachSeatFn  forEachSeat = nullptr;
    SeatPriorityFn seatPriority = nullptr;
    HandleToIdFn   handleToId = nullptr;
    ActorOfUserFn  actorOfUser = nullptr;
    ActorLinkFn    actorLink = nullptr;
    ForceLinkFn    forceLink = nullptr;
    ForceDelinkFn  forceDelink = nullptr;
    IsLinkedFn     isLinked = nullptr;
    FlightReadyFn  sendFlightReady = nullptr;
};

Vehicles g_v;
bool     g_started = false;

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

template <typename T> T Row(const char* id) { return reinterpret_cast<T>(Sig(id)); }

bool RowsOk(const char* const* ids, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        const SigResult* s = SigLookup(ids[i]);
        if (!s || s->state != SigState::Ok) return false;
    }
    return true;
}

// One capability per system: its rows, and the teleport.* reads every system starts from.
bool SetCap(const char* name, const char* const* rows, size_t n, bool readsOk) {
    caps::SetFromSignatures(name, rows, n);
    if (!readsOk) caps::Set(name, false, "teleport.* rows not OK");
    return readsOk && RowsOk(rows, n);
}

void Resolve() {
    g_v = {};
    const bool readsOk = reads::Ready();
    g_v.seats       = SetCap(kCapSeats, kSeatsRows, std::size(kSeatsRows), readsOk);
    g_v.seat        = SetCap(kCapSeat, kSeatRows, std::size(kSeatRows), readsOk);
    g_v.flightReady = SetCap(kCapFlightReady, kFlightReadyRows, std::size(kFlightReadyRows), readsOk);
    g_v.gate        = g_v.seats && RowsOk(kGateRows, std::size(kGateRows));
    if (!g_v.seats) return;   // seat and flightReady include every seats row
    uint8_t* es = Sig("teleport.entity_system");
    g_v.entitySystem = reinterpret_cast<uintptr_t*>(es);
    g_v.components   = reinterpret_cast<uintptr_t*>(es + 8);
    g_v.forEachSeat  = Row<ForEachSeatFn>("spawn.for_each_seat");
    g_v.seatPriority = Row<SeatPriorityFn>("spawn.seat_priority");
    g_v.handleToId   = Row<HandleToIdFn>("spawn.handle_to_id");
    if (g_v.seat) {
        g_v.actorOfUser = Row<ActorOfUserFn>("spawn.actor_of_user");
        g_v.actorLink   = Row<ActorLinkFn>("spawn.actor_link");
        g_v.forceLink   = Row<ForceLinkFn>("spawn.force_link");
        g_v.forceDelink = Row<ForceDelinkFn>("spawn.force_delink");
        g_v.isLinked    = Row<IsLinkedFn>("spawn.is_linked");
    }
    if (g_v.flightReady) g_v.sendFlightReady = Row<FlightReadyFn>("spawn.toggle_flight_ready");
}

// ---- last_error ------------------------------------------------------------------------------
//
// Game thread: one slot per plugin for the functions that take self, one shared slot for the
// queries (no self); last_error answers the newer. Other threads: the refusal on that thread.

constexpr size_t kErrMax   = 256;
constexpr size_t kErrSlots = 64;   // plugin handles are never reused; past it the oldest slot goes
struct ErrSlot { const void* owner; uint64_t seq; char msg[kErrMax]; };
ErrSlot  g_errs[kErrSlots];
ErrSlot  g_queryErr;
uint64_t g_errSeq = 0;
thread_local char t_threadErr[kErrMax];

const ErrSlot* FindErr(const void* owner) {
    for (const ErrSlot& e : g_errs)
        if (e.owner == owner) return &e;
    return nullptr;
}

ErrSlot& ErrFor(const void* owner) {
    if (!owner) return g_queryErr;
    ErrSlot* oldest = &g_errs[0];
    for (ErrSlot& e : g_errs) {
        if (e.owner == owner) return e;
        if (e.seq < oldest->seq) oldest = &e;
    }
    *oldest = {};
    oldest->owner = owner;
    return *oldest;
}

const char* PluginName(const sco_plugin* self) {
    const char* id = self ? host::PluginId(self) : nullptr;
    return id ? id : "?";
}

// Records why a call failed (the reason for last_error) and returns r. Mutators (owner non-null)
// also log it with the plugin's id: those are the calls a player wants explained in mod.log.
sco_result Fail(const sco_plugin* owner, const char* fn, sco_result r, const char* fmt, ...) {
    ErrSlot& e = ErrFor(owner);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(e.msg, sizeof(e.msg), fmt, ap);
    va_end(ap);
    e.seq = ++g_errSeq;
    if (owner) Log("[game] vehicles: %s: %s -> %s: %s", PluginName(owner), fn, ResultName(static_cast<Result>(r)), e.msg);
    return r;
}

sco_result WrongThread() {
    snprintf(t_threadErr, sizeof(t_threadErr), "game thread only");
    return SCO_WRONG_THREAD;
}

// ---- game reads and calls, under SEH -----------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712).

uintptr_t EntityComponent(uintptr_t entity, const char* type) {
    uint8_t tmp[16] = {};
    const uint16_t* id = VCall<const uint16_t*>(*g_v.components, actors::kComponentsTypeId, tmp, type);
    if (!id) return 0;
    uint16_t typeId = *id;
    uint8_t out[16] = {};
    const uint64_t* h = VCall<const uint64_t*>(entity, actors::kEntityComponent, out, &typeId);
    return h ? (*h & kPtrMask) : 0;
}

uintptr_t EntityById(uint64_t id) {
    __try {
        return id ? reads::EntityFromId(id) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

void CopyName(char* out, size_t n, const char* name) {
    __try {
        strncpy_s(out, n, name ? name : "?", _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        strncpy_s(out, n, "?", _TRUNCATE);
    }
}

uint64_t LocalPlayerId() {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return 0;
        uint64_t id = 0;
        g_v.handleToId(reinterpret_cast<const void*>(actor + 8), &id);
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The ship you're aboard: your zone's entity, when it has item ports. 0 = in no ship; ~0 = not
// spawned.
constexpr uint64_t kNotSpawned = ~0ull;
uint64_t PlayerShip() {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return kNotSpawned;
        const uintptr_t zone = reads::EntityZone(entity);
        if (!zone) return 0;
        const uint64_t id = reads::ZoneId(zone);
        const uintptr_t ship = reads::EntityFromId(id);
        return ship && EntityComponent(ship, "IItemPortContainer") ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// ---- seats -------------------------------------------------------------------------------------

struct Seat {
    uintptr_t seat;
    uint64_t  id;         // the seat item's entity id
    uint64_t  raw;        // the occupant field, as read
    uint64_t  occupant;   // decoded; 0 = empty or unknown
    uint32_t  priority;
    int       usable;     // 1 / 0; -1 = unknown (no gate row)
    char      name[SC_VEHICLE_SEAT_NAME_MAX];
};

uintptr_t g_found[kMaxSeats];
uint32_t  g_foundCount = 0, g_foundTotal = 0;
Seat      g_list[kMaxSeats];
uint32_t  g_count = 0, g_total = 0;

// The visitor the game's for_each_seat calls for every seat of the item type asked for: the
// same shape as sc-offline's (and the seat picker's own) callback; 1 = keep going.
char __fastcall CollectSeat(uintptr_t seat) {
    if (!seat) return 1;
    if (g_foundCount < kMaxSeats) g_found[g_foundCount++] = seat;
    ++g_foundTotal;
    return 1;
}

bool ReadSeat(uintptr_t seat, Seat& s) {
    s.seat = seat;
    __try {
        s.raw = Rd<uint64_t>(seat + actors::kSeatOccupant);
        s.priority = g_v.seatPriority(seat);
        g_v.handleToId(reinterpret_cast<const void*>(seat + 8), &s.id);
        const uintptr_t owner = Rd<uint64_t>(seat + 8) & kPtrMask;
        CopyName(s.name, sizeof(s.name), owner ? VCall<const char*>(owner, actors::kEntityName) : nullptr);
        // The seat picker's first test (spawn.seat_interactable): the owner's IInteractableComponent.
        s.usable = !g_v.gate ? -1 : owner && EntityComponent(owner, actors::kSeatInteractable) ? 1 : 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// seat+0x158 holds the occupant's entity id in 4.10.196 (sc-offline's first log: 0x2e914d0fec =
// 200006242284 next to seat 200006242283); an entity handle and a pointer to a component are kept
// as fallbacks for other builds. The first that decodes to an actor is learned and logged.
int g_layout = 0;   // 0 = not learned, 1 = handle, 2 = component pointer, 3 = raw entity id

bool IsActorEntity(uint64_t id, uint64_t shipId, uint64_t seatId) {
    if (!id || id == shipId || id == seatId) return false;
    const uintptr_t e = reads::EntityFromId(id);
    return e && (EntityComponent(e, "ISCItemUser") != 0 || EntityComponent(e, "Actor") != 0);
}

uint64_t OccupantAsRawId(uint64_t raw, uint64_t shipId, uint64_t seatId) {
    __try { return IsActorEntity(raw, shipId, seatId) ? raw : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint64_t OccupantAsHandle(uintptr_t seat, uint64_t shipId, uint64_t seatId) {
    __try {
        uint64_t id = 0;
        g_v.handleToId(reinterpret_cast<const void*>(seat + actors::kSeatOccupant), &id);
        return IsActorEntity(id, shipId, seatId) ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint64_t OccupantAsComponent(uint64_t raw, uint64_t shipId, uint64_t seatId) {
    __try {
        const uintptr_t p = raw & kPtrMask;
        if (!p || (p & 7)) return 0;
        uint64_t id = 0;
        g_v.handleToId(reinterpret_cast<const void*>(p + 8), &id);
        return IsActorEntity(id, shipId, seatId) ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint64_t DecodeOccupant(const Seat& s, uint64_t shipId) {
    if (!s.raw) return 0;
    static const char* const kLayoutName[] = { "", "an entity handle", "a component pointer", "a raw entity id" };
    uint64_t id = 0;
    int layout = 0;
    if ((!g_layout || g_layout == 3) && (id = OccupantAsRawId(s.raw, shipId, s.id)))      layout = 3;
    if (!id && (!g_layout || g_layout == 1) && (id = OccupantAsHandle(s.seat, shipId, s.id))) layout = 1;
    if (!id && (!g_layout || g_layout == 2) && (id = OccupantAsComponent(s.raw, shipId, s.id))) layout = 2;
    if (id && !g_layout) {
        g_layout = layout;
        Log("[game] vehicles: seat occupant field read as %s", kLayoutName[layout]);
    }
    return id;
}

// Fills g_list with ship's seats. >= 0: the count read (g_total counts every seat the game
// visited); kNoShip: not streamed in; kNotVehicle: no item ports; kFault: the game faulted.
constexpr int kNoShip = -1, kNotVehicle = -2, kFault = -3;

int VisitSeats(uint64_t shipId) {
    g_foundCount = g_foundTotal = 0;
    __try {
        const uintptr_t ship = shipId ? reads::EntityFromId(shipId) : 0;
        if (!ship) return kNoShip;
        const uintptr_t ports = EntityComponent(ship, "IItemPortContainer");
        if (!ports) return kNotVehicle;
        const struct { void* invoke; uintptr_t manager; void* storage; } visitor = {
            reinterpret_cast<void*>(&CollectSeat), 1, nullptr };
        g_v.forEachSeat(VCall<uintptr_t>(ports, actors::kPortsSeatContainer), &visitor, actors::kSeatItemType);
        return 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_foundCount = g_foundTotal = 0;
        return kFault;
    }
}

uint64_t g_loggedShip = 0;
uint32_t g_loggedTotal = 0;

const char* Interactable(int usable) { return usable > 0 ? "yes" : usable == 0 ? "no" : "unknown"; }

int Enumerate(uint64_t shipId) {
    g_count = g_total = 0;
    if (const int r = VisitSeats(shipId)) return r;
    for (uint32_t i = 0; i < g_foundCount; ++i) {
        Seat& s = g_list[g_count];
        s = {};
        if (!ReadSeat(g_found[i], s)) return kFault;
        s.occupant = DecodeOccupant(s, shipId);
        ++g_count;
    }
    g_total = g_foundTotal;
    // The diagnostics T3 asked for, once per ship and seat count (a crew fill polls this list):
    // which seats the game's picker would take, and what the occupant field holds.
    if (shipId != g_loggedShip || g_total != g_loggedTotal) {
        g_loggedShip = shipId;
        g_loggedTotal = g_total;
        Log("[game] vehicles: ship %llu has %u seats", static_cast<unsigned long long>(shipId), g_total);
        for (uint32_t i = 0; i < g_count; ++i) {
            const Seat& s = g_list[i];
            Log("[game] vehicles: seat %u %llu '%s' prio %u interactable %s occupant 0x%llx", i,
                static_cast<unsigned long long>(s.id), s.name, s.priority, Interactable(s.usable),
                static_cast<unsigned long long>(s.raw));
        }
    }
    return static_cast<int>(g_count);
}

// Enumerate, with the failure recorded for last_error (owner: the mutator's self, or nullptr).
sco_result EnumerateOrFail(const sco_plugin* owner, const char* fn, uint64_t shipId) {
    switch (Enumerate(shipId)) {
    case kNoShip:     return Fail(owner, fn, SCO_NOT_FOUND, "ship %llu isn't streamed in", static_cast<unsigned long long>(shipId));
    case kNotVehicle: return Fail(owner, fn, SCO_NOT_FOUND, "entity %llu isn't a vehicle (no item ports)", static_cast<unsigned long long>(shipId));
    case kFault:      return Fail(owner, fn, SCO_FAILED, "fault while reading ship %llu's seats", static_cast<unsigned long long>(shipId));
    default:          return SCO_OK;
    }
}

uint32_t PilotIndex() {
    uint32_t top = 0;
    for (uint32_t i = 1; i < g_count; ++i)
        if (g_list[i].priority > g_list[top].priority) top = i;
    return top;
}

// ---- seating -----------------------------------------------------------------------------------

uintptr_t ActorOfEntity(uintptr_t entity) {
    const uintptr_t user = EntityComponent(entity, "ISCItemUser");
    if (!user) return 0;
    uint64_t handle = 0;
    g_v.actorOfUser(user, &handle);
    return handle & kPtrMask;
}

enum class Link { Ok, NoEntity, NoActor, NotLinked, Fault };

// Links actorId into seatId, unlinking it from any seat first (sc-offline's LinkEntityToSeat).
// linked: is_linked right after, as a diagnostic.
Link LinkActor(uint64_t actorId, uint64_t seatId, int& linked) {
    __try {
        const uintptr_t entity = reads::EntityFromId(actorId);
        if (!entity) return Link::NoEntity;
        const uintptr_t actor = ActorOfEntity(entity);
        if (!actor) return Link::NoActor;
        if (g_v.isLinked(actor)) g_v.forceDelink(g_v.actorLink(actor));
        g_v.forceLink(g_v.actorLink(actor), seatId);
        linked = g_v.isLinked(actor) ? 1 : 0;
        return Link::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Link::Fault;
    }
}

Link UnlinkActor(uint64_t actorId) {
    __try {
        const uintptr_t entity = reads::EntityFromId(actorId);
        if (!entity) return Link::NoEntity;
        const uintptr_t actor = ActorOfEntity(entity);
        if (!actor) return Link::NoActor;
        if (!g_v.isLinked(actor)) return Link::NotLinked;
        g_v.forceDelink(g_v.actorLink(actor));
        return Link::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Link::Fault;
    }
}

uint64_t ReadOccupantField(uintptr_t seat) {
    __try { return Rd<uint64_t>(seat + actors::kSeatOccupant); } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// You may seat or eject your player's own actor, or one you spawned through spawn.entities'
// spawn_as or game.actors' spawn_npc.
bool MaySeat(const sco_plugin* self, uint64_t actor) {
    if (SpawnedBy(self, actor) || NpcOwnedBy(self, actor)) return true;
    const uint64_t me = LocalPlayerId();
    return me && actor == me;
}

// ---- power -------------------------------------------------------------------------------------

uintptr_t DashboardOn(uintptr_t entity) {
    __try { return entity ? EntityComponent(entity, "SCItemSeatDashboard") : 0; } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint64_t ItemParentId(uintptr_t entity) {
    __try {
        uint64_t port = 0;
        VCall<void>(entity, actors::kEntityParentPort, &port, 0ull);
        if (!(port & kPtrMask)) return 0;
        uint64_t id = 0;
        const uint64_t* owner = VCall<const uint64_t*>(port & kPtrMask, actors::kPortOwnerId, &id);
        return owner ? *owner : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The pilot seat's dashboard: on the seat itself (Perseus-style), on the ship, else on a part of
// the ship, preferring one mounted on the pilot seat (F8C, Moth and most smaller ships). A spawned
// ship's parts have consecutive entity ids after its own, so the walk keeps the entities whose
// chain of parents leads back to the ship (sc-offline's CollectShipItems).
uintptr_t FindDashboard(uint64_t shipId, uint64_t seatId) {
    if (const uintptr_t d = DashboardOn(EntityById(seatId))) return d;
    if (const uintptr_t d = DashboardOn(EntityById(shipId))) return d;
    uintptr_t any = 0;
    int misses = 0;
    for (uint64_t id = shipId + 1; id < shipId + 8000 && misses < 400; ++id) {
        const uintptr_t e = EntityById(id);
        if (!e) { ++misses; continue; }
        misses = 0;
        const uint64_t parent = ItemParentId(e);
        uint64_t up = parent;
        for (int depth = 1; up && up != shipId && depth < 12; ++depth) {
            const uintptr_t next = EntityById(up);
            up = next ? ItemParentId(next) : 0;
        }
        if (up != shipId) continue;   // not part of this ship (crew, other ships, loose items)
        const uintptr_t d = DashboardOn(e);
        if (!d) continue;
        if (parent == seatId) return d;
        if (!any) any = d;
    }
    return any;
}

bool SendFlightReady(uintptr_t dashboard) {
    __try {
        const struct { void* invoke; uintptr_t manager; void* storage; } noCallback = {};
        g_v.sendFlightReady(*g_v.entitySystem, dashboard, &noCallback);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- game.vehicles (sc_vehicles.h) -------------------------------------------------------------

sco_result SvcPlayerShip(uint64_t* outShip) {
    if (!OnGameThread()) return WrongThread();
    if (outShip) *outShip = 0;
    if (!g_v.seats) return Fail(nullptr, "player_ship", SCO_UNAVAILABLE, "game.vehicles.seats isn't ready on this game build");
    if (!outShip) return Fail(nullptr, "player_ship", SCO_BAD_ARG, "out_ship is NULL");
    const uint64_t ship = PlayerShip();
    if (ship == kNotSpawned) return Fail(nullptr, "player_ship", SCO_UNAVAILABLE, "you're not spawned yet");
    if (!ship) return Fail(nullptr, "player_ship", SCO_NOT_FOUND, "you're not aboard a ship");
    *outShip = ship;
    return SCO_OK;
}

sco_result SvcSeats(uint64_t ship, sc_vehicle_seat* out, uint32_t max, uint32_t* outCount, uint32_t* outMore) {
    if (!OnGameThread()) return WrongThread();
    if (outCount) *outCount = 0;
    if (outMore) *outMore = 0;
    if (!g_v.seats) return Fail(nullptr, "seats", SCO_UNAVAILABLE, "game.vehicles.seats isn't ready on this game build");
    if (!outCount || !outMore || (!out && max)) return Fail(nullptr, "seats", SCO_BAD_ARG, "a NULL out pointer");
    if (const sco_result r = EnumerateOrFail(nullptr, "seats", ship); r != SCO_OK) return r;
    uint32_t pilot = PilotIndex();
    uint32_t n = 0;
    for (; n < g_count && n < max; ++n) {
        const Seat& s = g_list[n];
        sc_vehicle_seat& o = out[n];
        o = {};
        o.index = n;
        o.seat_id = s.id;
        o.occupant_id = s.occupant;
        o.priority = s.priority;
        o.flags = (s.usable >= 0 ? SC_SEAT_USABLE_KNOWN : 0u) | (s.usable > 0 ? SC_SEAT_USABLE : 0u) |
                  (s.raw ? SC_SEAT_OCCUPIED : 0u) | (n == pilot ? SC_SEAT_PILOT : 0u);
        memcpy(o.name, s.name, sizeof(o.name));
    }
    *outCount = n;
    *outMore = g_total > n ? 1u : 0u;
    return SCO_OK;
}

sco_result SvcSeatOccupant(uint64_t ship, uint32_t index, uint64_t* outActor) {
    if (!OnGameThread()) return WrongThread();
    if (outActor) *outActor = 0;
    if (!g_v.seats) return Fail(nullptr, "seat_occupant", SCO_UNAVAILABLE, "game.vehicles.seats isn't ready on this game build");
    if (!outActor) return Fail(nullptr, "seat_occupant", SCO_BAD_ARG, "out_actor is NULL");
    if (const sco_result r = EnumerateOrFail(nullptr, "seat_occupant", ship); r != SCO_OK) return r;
    if (index >= g_count)
        return Fail(nullptr, "seat_occupant", SCO_NOT_FOUND, "ship %llu has no seat %u (%u seats)",
                    static_cast<unsigned long long>(ship), index, g_count);
    const Seat& s = g_list[index];
    if (s.raw && !s.occupant)
        return Fail(nullptr, "seat_occupant", SCO_FAILED, "seat %u '%s' is occupied by something the game pack can't decode (occupant field 0x%llx)",
                    index, s.name, static_cast<unsigned long long>(s.raw));
    *outActor = s.occupant;
    return SCO_OK;
}

sco_result SvcSeat(sco_plugin* self, uint64_t actor, uint64_t ship, uint32_t index) {
    if (!OnGameThread()) return WrongThread();
    if (!g_v.seat) return Fail(self, "seat", SCO_UNAVAILABLE, "game.vehicles.seat isn't ready on this game build");
    if (!self || !host::PluginId(self)) return Fail(nullptr, "seat", SCO_BAD_ARG, "self isn't a plugin handle");
    if (!actor || !MaySeat(self, actor))
        return Fail(self, "seat", SCO_BAD_ARG, "actor %llu isn't yours (seat your own player, or an NPC you spawned with spawn_as or spawn_npc)",
                    static_cast<unsigned long long>(actor));
    if (const sco_result r = EnumerateOrFail(self, "seat", ship); r != SCO_OK) return r;
    if (index >= g_count)
        return Fail(self, "seat", SCO_NOT_FOUND, "ship %llu has no seat %u (%u seats)", static_cast<unsigned long long>(ship), index, g_count);
    const Seat s = g_list[index];
    Log("[game] vehicles: %s: seat actor %llu in ship %llu seat %u %llu '%s' (interactable %s, occupant 0x%llx)", PluginName(self),
        static_cast<unsigned long long>(actor), static_cast<unsigned long long>(ship), index, static_cast<unsigned long long>(s.id),
        s.name, Interactable(s.usable), static_cast<unsigned long long>(s.raw));
    if (s.usable <= 0)
        return Fail(self, "seat", SCO_FAILED, "seat %u '%s' isn't interactable (the game's seat picker skips it)", index, s.name);
    if (s.raw && s.occupant != actor)
        return Fail(self, "seat", SCO_FAILED, "seat %u '%s' is taken (occupant field 0x%llx)", index, s.name,
                    static_cast<unsigned long long>(s.raw));
    int linked = 0;
    switch (LinkActor(actor, s.id, linked)) {
    case Link::NoEntity: return Fail(self, "seat", SCO_FAILED, "actor %llu isn't streamed in yet", static_cast<unsigned long long>(actor));
    case Link::NoActor:  return Fail(self, "seat", SCO_FAILED, "entity %llu has no seat link (not an actor?)", static_cast<unsigned long long>(actor));
    case Link::Fault:    return Fail(self, "seat", SCO_FAILED, "fault while linking actor %llu", static_cast<unsigned long long>(actor));
    default: break;
    }
    Log("[game] vehicles: link actor %llu -> seat %llu: isLinked %d, occupant 0x%llx", static_cast<unsigned long long>(actor),
        static_cast<unsigned long long>(s.id), linked, static_cast<unsigned long long>(ReadOccupantField(s.seat)));
    return SCO_OK;
}

sco_result SvcEject(sco_plugin* self, uint64_t actor) {
    if (!OnGameThread()) return WrongThread();
    if (!g_v.seat) return Fail(self, "eject", SCO_UNAVAILABLE, "game.vehicles.seat isn't ready on this game build");
    if (!self || !host::PluginId(self)) return Fail(nullptr, "eject", SCO_BAD_ARG, "self isn't a plugin handle");
    if (!actor || !MaySeat(self, actor))
        return Fail(self, "eject", SCO_BAD_ARG, "actor %llu isn't yours (eject your own player, or an NPC you spawned with spawn_as or spawn_npc)",
                    static_cast<unsigned long long>(actor));
    switch (UnlinkActor(actor)) {
    case Link::Ok:
        Log("[game] vehicles: %s: ejected actor %llu", PluginName(self), static_cast<unsigned long long>(actor));
        return SCO_OK;
    case Link::NoEntity:  return Fail(self, "eject", SCO_FAILED, "actor %llu isn't streamed in", static_cast<unsigned long long>(actor));
    case Link::NoActor:   return Fail(self, "eject", SCO_FAILED, "entity %llu has no seat link (not an actor?)", static_cast<unsigned long long>(actor));
    case Link::NotLinked: return Fail(self, "eject", SCO_FAILED, "actor %llu isn't seated", static_cast<unsigned long long>(actor));
    default:              return Fail(self, "eject", SCO_FAILED, "fault while unlinking actor %llu", static_cast<unsigned long long>(actor));
    }
}

sco_result SvcPowerOn(sco_plugin* self, uint64_t ship) {
    if (!OnGameThread()) return WrongThread();
    if (!g_v.flightReady) return Fail(self, "power_on", SCO_UNAVAILABLE, "game.vehicles.flight_ready isn't ready on this game build");
    if (!self || !host::PluginId(self)) return Fail(nullptr, "power_on", SCO_BAD_ARG, "self isn't a plugin handle");
    if (!ship || (ship != PlayerShip() && !IsPlayerVehicle(ship) && !SpawnedBy(self, ship)))
        return Fail(self, "power_on", SCO_BAD_ARG,
                    "ship %llu isn't yours (the ship you're aboard, your registered vehicles, or one you spawned with spawn_as)",
                    static_cast<unsigned long long>(ship));
    if (const sco_result r = EnumerateOrFail(self, "power_on", ship); r != SCO_OK) return r;
    if (!g_count) return Fail(self, "power_on", SCO_FAILED, "ship %llu has no seats", static_cast<unsigned long long>(ship));
    const Seat& pilot = g_list[PilotIndex()];
    const uintptr_t dashboard = FindDashboard(ship, pilot.id);
    if (!dashboard)
        return Fail(self, "power_on", SCO_FAILED, "no pilot dashboard on ship %llu yet (big ships stream it in over seconds; try again)",
                    static_cast<unsigned long long>(ship));
    if (!SendFlightReady(dashboard)) return Fail(self, "power_on", SCO_FAILED, "fault while sending Flight Ready");
    Log("[game] vehicles: %s: Flight Ready sent to ship %llu (pilot seat '%s')", PluginName(self),
        static_cast<unsigned long long>(ship), pilot.name);
    return SCO_OK;
}

sco_result SvcLastError(sco_plugin* self, char* out, uint32_t* io) {
    if (!io) return SCO_BAD_ARG;
    const uint32_t capacity = *io;
    *io = 0;
    if (!out && capacity) return SCO_BAD_ARG;
    const char* msg = t_threadErr;
    if (OnGameThread()) {
        const ErrSlot* mine = self ? FindErr(self) : nullptr;
        msg = mine && mine->seq > g_queryErr.seq ? mine->msg : g_queryErr.msg;
    }
    const size_t need = strlen(msg) + 1;
    if (capacity < need) {
        *io = static_cast<uint32_t>(need);
        return SCO_TOO_MANY;
    }
    memcpy(out, msg, need);
    *io = static_cast<uint32_t>(need);
    return SCO_OK;
}

const sc_vehicles_v1 kService = {
    sizeof(sc_vehicles_v1), 0, SvcPlayerShip, SvcSeats, SvcSeatOccupant, SvcSeat, SvcEject, SvcPowerOn, SvcLastError,
};

}  // namespace

Result StartVehicles() {
    if (g_started) return Result::Ok;
    Resolve();
    if (!g_v.seats)
        Log("[game] game.vehicles.seats not ready: game.vehicles answers SCO_UNAVAILABLE (see the [core] lines)");
    else if (!g_v.gate)
        Log("[game] spawn.seat_gate rows not OK: game.vehicles lists seats with usability unknown and won't seat anyone");
    const Result r = host::ProvideGameService(SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION, &kService);
    if (r != Result::Ok) {
        g_v = {};
        return r;
    }
    g_started = true;
    return Result::Ok;
}

void StopVehicles() {
    if (!g_started) return;
    host::WithdrawGameService(SC_VEHICLES_SERVICE_NAME);
    g_v = {};
    for (ErrSlot& e : g_errs) e = {};
    g_queryErr = {};
    g_loggedShip = 0;
    g_loggedTotal = 0;
    g_started = false;
}

}  // namespace sco::game::services
