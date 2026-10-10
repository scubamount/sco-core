// The game.* bus events (sc_game_events.h) from the game pack: game.player.spawned, game.player.died
// and game.zone.changed, and the log-only seat probe behind game.vehicle.boarded / exited
// (docs/design/game-world-spikes.md B5, B6, B7, B8).
//
// The hooks never run plugin code and never wait on the game: a detour calls the game's own
// function first, then writes one small record (which hook, the two pointers the game passed) into a
// fixed ring under a mutex held for a handful of stores (no allocation, no calls out while it is
// held). Everything else happens on the game thread, in the host's tick: it takes the records out,
// decides on the game thread whether the actor is your player (a read of game memory, so never in a
// detour), reads the ids, and dispatches through the kernel event bus, which is where plugin code
// runs. A hook thus costs the game one lock and a few stores, whichever thread it fires on.
//
// game.zone.changed has no hook: the tick polls your player's zone id every kPollMs.
//
// Every hook is installed only when its capability's rows are OK. game.events.vehicle_seat stays
// off: its hooks (the seat state's Enter and Exit) install and only log (SeatProbe), until an
// in-game run confirms which actor a transition belongs to and that the link is a vehicle seat.
#include "events.h"
#include "actors.h"
#include "sco/caps.h"
#include "sco/game/actors.h"
#include "sco/game/events.h"
#include "sco/game/reads.h"
#include "sco/hook.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include <sc_game_events.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>
#include <windows.h>

namespace sco::game::services {

namespace {

using HandleToIdFn = uint64_t*(__fastcall*)(const void* handleField, uint64_t* entityId);
using Detour3      = void(__fastcall*)(void*, void*, void*);

constexpr uint64_t kPtrMask      = 0xFFFFFFFFFFFFull;
constexpr uint32_t kPollMs       = 100;     // zone poll, and the cache of your actor
constexpr uint32_t kSpawnWaitMs  = 10000;   // a spawn record waits this long for your player to be readable
constexpr size_t   kRing         = 32;      // records between two ticks; more are dropped and counted
constexpr size_t   kWaitMax      = 8;
constexpr int      kSeatProbeMax = 100;     // log lines of the seat probe per session

constexpr const char* kCapSpawned = "game.events.player_spawned";
constexpr const char* kCapDied    = "game.events.player_died";
constexpr const char* kCapZone    = "game.events.zone_changed";
constexpr const char* kCapSeat    = "game.events.vehicle_seat";
constexpr const char* kTeleportRows[] = {   // the reads (sco/game/teleport.h)
    "teleport.to_camera", "teleport.client_mgr", "teleport.handle_from_id", "teleport.entity_system",
};
constexpr const char* kIdRows[] = { "spawn.find_seat", "spawn.handle_to_id" };   // an actor's entity id

enum class Kind : uint8_t { Spawn = 1, Death, SeatEnter, SeatExit };
struct Record { Kind kind; uintptr_t a, b; };

bool g_started = false;
std::atomic<bool> g_active{ false };   // the detours record only while this is set

// The ring: any thread writes (a hook), the game thread takes everything out.
std::mutex g_ringLock;
Record     g_ring[kRing];
size_t     g_head = 0, g_count = 0;
uint32_t   g_dropped = 0;

// Game thread only.
bool         g_capSpawned = false, g_capDied = false, g_capZone = false, g_capSeat = false;
HandleToIdFn g_handleToId = nullptr;
struct Local { uintptr_t actor; uint64_t entityId; uint64_t zoneId; };
Local        g_cache = {};            // your player as of the last poll
bool         g_haveCache = false;
uint64_t     g_zone = 0;              // your zone as of the last poll with a zone
uint32_t     g_lastPoll = 0;
struct Wait { uintptr_t actor; uint32_t since; };
Wait         g_wait[kWaitMax];
size_t       g_nwait = 0;
int          g_seatLogged = 0;

// The four hooks.
struct Hook { const char* row; void* detour; void** orig; uint8_t* at; };
void* g_origSpawn = nullptr;
void* g_origDeath = nullptr;
void* g_origEnter = nullptr;
void* g_origExit  = nullptr;

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(*reinterpret_cast<const uintptr_t*>(*reinterpret_cast<const uintptr_t*>(obj) + off))(obj, a...);
}

bool RowsOk(const std::vector<const char*>& ids) {
    for (const char* id : ids) {
        const SigResult* s = SigLookup(id);
        if (!s || s->state != SigState::Ok) return false;
    }
    return true;
}

template <class C> void AddRows(const C* caps, size_t n, const char* name, std::vector<const char*>& out) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(caps[i].name, name) == 0) out.insert(out.end(), caps[i].rows, caps[i].rows + caps[i].count);
}

// Sets capability name from rows (and the reads); true when it's ready. why: the first row that
// isn't OK.
bool SetCap(const char* name, const std::vector<const char*>& rows, char* why, size_t n) {
    why[0] = 0;
    for (const char* id : rows) {
        const SigResult* s = SigLookup(id);
        if (!s) { snprintf(why, n, "unknown signature %s", id); break; }
        if (s->state != SigState::Ok) { snprintf(why, n, "needs %s (%s)", id, SigStateName(s->state)); break; }
    }
    caps::SetFromSignatures(name, rows.data(), rows.size());
    const bool ready = caps::Has(name) && reads::Ready();
    if (!ready && !why[0]) snprintf(why, n, "the teleport.* reads aren't ready");
    return ready;
}

// ---- the hooks: record and return --------------------------------------------------------------

void Push(Kind kind, void* a, void* b) noexcept {
    if (!g_active.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> hold(g_ringLock);
    if (g_count == kRing) { ++g_dropped; return; }
    g_ring[(g_head + g_count) % kRing] = { kind, reinterpret_cast<uintptr_t>(a), reinterpret_cast<uintptr_t>(b) };
    ++g_count;
}

// SCigEventDispatcher::QueueEvent<SPI_Player_OnSpawn>(actor*, bool, void*): the game's own queue
// first, then the record. The bool arrives in dl; the register is passed on untouched.
void __fastcall SpawnDetour(void* a, void* b, void* c) noexcept {
    reinterpret_cast<Detour3>(g_origSpawn)(a, b, c);
    Push(Kind::Spawn, a, nullptr);
}
// QueueEvent<SPI_Player_OnDeath>(actor*, const hit_info*, void*)
void __fastcall DeathDetour(void* a, void* b, void* c) noexcept {
    reinterpret_cast<Detour3>(g_origDeath)(a, b, c);
    Push(Kind::Death, a, nullptr);
}
// CSCActorResultStateLinked::Enter / Exit (state*, host*, const SData*)
void __fastcall EnterDetour(void* a, void* b, void* c) noexcept {
    reinterpret_cast<Detour3>(g_origEnter)(a, b, c);
    Push(Kind::SeatEnter, a, b);
}
void __fastcall ExitDetour(void* a, void* b, void* c) noexcept {
    reinterpret_cast<Detour3>(g_origExit)(a, b, c);
    Push(Kind::SeatExit, a, b);
}

template <class F> void* Code(F f) {
    static_assert(sizeof(F) == sizeof(void*), "function pointer size");
    void* p;
    memcpy(&p, &f, sizeof p);
    return p;
}

Hook g_hooks[] = {
    { "event.player_spawn", Code(&SpawnDetour), &g_origSpawn, nullptr },
    { "event.player_death", Code(&DeathDetour), &g_origDeath, nullptr },
    { "event.seat_enter",   Code(&EnterDetour), &g_origEnter, nullptr },
    { "event.seat_exit",    Code(&ExitDetour),  &g_origExit,  nullptr },
};

// Installs a hook; its row must be OK. False (and why) when the patch is refused.
bool InstallHook(Hook& h, char* why, size_t n) {
    h.at = Sig(h.row);
    const hook::Error e = hook::InstallDetour(h.at, 0, h.detour, h.orig);
    if (e == hook::Error::None) return true;
    snprintf(why, n, "hook %s refused: %s", h.row, hook::ErrorName(e));
    h.at = nullptr;
    return false;
}

void RemoveHooks() {
    for (Hook& h : g_hooks) {
        if (!h.at) continue;
        const hook::Error e = hook::RemoveDetour(h.at);
        if (e != hook::Error::None) Log("[game] events: removing the %s hook: %s", h.row, hook::ErrorName(e));
        h.at = nullptr;
    }
}

// ---- game reads, under SEH ---------------------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712).

bool ReadLocal(Local& l) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return false;
        l.actor = actor;
        l.entityId = 0;
        if (g_handleToId) {
            uint64_t id = 0;
            g_handleToId(reinterpret_cast<const void*>(actor + 8), &id);
            l.entityId = id;
        }
        l.zoneId = 0;
        const uintptr_t zone = reads::EntityZone(entity);
        if (zone) {
            const uint64_t zid = reads::ZoneId(zone);
            if (zid && reads::ZoneFromId(zid) == zone) l.zoneId = zid;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t ParentIdRaw(uintptr_t entity) {   // the item port an entity is attached to, and its owner
    uint64_t port = 0;
    VCall<void>(entity, actors::kEntityParentPort, &port, 0ull);
    if (!(port & kPtrMask)) return 0;
    uint64_t id = 0;
    const uint64_t* owner = VCall<const uint64_t*>(port & kPtrMask, actors::kPortOwnerId, &id);
    return owner ? *owner : 0;
}

struct SeatInfo { uintptr_t actor; int n; uint64_t id[4]; char name[4][48]; };

// Your player's chain of parent entities (the ship and seat it is in), for the seat probe.
bool ReadSeatInfo(SeatInfo& s) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return false;
        s.actor = actor;
        s.n = 0;
        uint64_t id = ParentIdRaw(entity);
        for (; id && s.n < 4; ++s.n) {
            s.id[s.n] = id;
            const uintptr_t e = reads::EntityFromId(id);
            const char* name = e ? VCall<const char*>(e, actors::kEntityName) : nullptr;
            snprintf(s.name[s.n], sizeof(s.name[s.n]), "%s", name ? name : "");
            id = e ? ParentIdRaw(e) : 0;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- the tick: drain, decide, dispatch ---------------------------------------------------------

void Publish(const char* event, const void* payload) {
    size_t called = 0;
    const Result r = Dispatch(event, payload, &called);
    Log("[game] event %s: %zu subscriber%s%s", event, called, called == 1 ? "" : "s", r == Result::Ok ? "" : " (dispatch refused)");
}

void PublishSpawned(const Local& l) {
    sc_game_player_spawned p = { sizeof(p), 0, l.entityId, l.zoneId };
    if (l.zoneId) g_zone = l.zoneId;   // the baseline: a spawn is not a zone change
    Publish(SC_GAME_EVENT_PLAYER_SPAWNED, &p);
}

void SeatProbe(const Record& r) {
    if (g_seatLogged >= kSeatProbeMax) return;
    if (++g_seatLogged == kSeatProbeMax) Log("[game] vehicle_seat probe: %d lines logged; no more this session", kSeatProbeMax);
    SeatInfo s = {};
    const bool ok = ReadSeatInfo(s);
    char chain[256] = "";
    for (int i = 0; ok && i < s.n; ++i) {
        const size_t used = strlen(chain);
        snprintf(chain + used, sizeof(chain) - used, "%s%llu '%s'", i ? " > " : "", static_cast<unsigned long long>(s.id[i]), s.name[i]);
    }
    Log("[game] vehicle_seat probe: %s: state=%p host=%p localActor=%p parents: %s", r.kind == Kind::SeatEnter ? "Enter" : "Exit",
        reinterpret_cast<void*>(r.a), reinterpret_cast<void*>(r.b), ok ? reinterpret_cast<void*>(s.actor) : nullptr,
        !ok ? "(your player couldn't be read)" : s.n ? chain : "(none)");
}

void OnTick(const char*, const void* data, void*) {
    const uint32_t now = data ? *static_cast<const uint32_t*>(data) : 0;

    Record batch[kRing];
    size_t n = 0;
    uint32_t dropped = 0;
    {
        std::lock_guard<std::mutex> hold(g_ringLock);
        for (; n < g_count; ++n) batch[n] = g_ring[(g_head + n) % kRing];
        g_head = g_count = 0;
        dropped = g_dropped;
        g_dropped = 0;
    }
    if (dropped) Log("[game] events: %u hook records dropped (more than %zu in one tick)", dropped, kRing);

    const bool wantLocal = g_capSpawned || g_capDied || g_capZone;
    const bool poll = wantLocal && (n || g_nwait || static_cast<uint32_t>(now - g_lastPoll) >= kPollMs);
    Local cur = {};
    bool have = false;
    if (poll) {
        g_lastPoll = now;
        have = ReadLocal(cur);
    }

    for (size_t i = 0; i < n; ++i) {
        const Record& r = batch[i];
        switch (r.kind) {
        case Kind::Spawn:
            if (!g_capSpawned) break;
            if (have && r.a == cur.actor) PublishSpawned(cur);
            else if (!have && g_nwait < kWaitMax) g_wait[g_nwait++] = { r.a, now };   // not readable yet: try again
            break;   // have, and another actor (an NPC, another player): not yours
        case Kind::Death: {
            if (!g_capDied) break;
            const bool cached = g_haveCache && r.a == g_cache.actor;
            if (!cached && !(have && r.a == cur.actor)) break;   // not your player
            sc_game_player_died p = { sizeof(p), 0, cached ? g_cache.entityId : cur.entityId, 0 };
            Publish(SC_GAME_EVENT_PLAYER_DIED, &p);
            ProbeActorState(r.a, "died");
            break;
        }
        case Kind::SeatEnter:
        case Kind::SeatExit:
            SeatProbe(r);
            break;
        }
    }

    for (size_t i = 0; i < g_nwait;) {   // spawn records that couldn't be matched yet
        bool done = false;
        if (have) {
            if (g_wait[i].actor == cur.actor) PublishSpawned(cur);
            done = true;
        } else if (static_cast<uint32_t>(now - g_wait[i].since) >= kSpawnWaitMs) {
            Log("[game] events: your player wasn't readable within %u s of a spawn record; game.player.spawned dropped", kSpawnWaitMs / 1000);
            done = true;
        }
        if (done) g_wait[i] = g_wait[--g_nwait];
        else ++i;
    }

    if (have) {
        g_cache = cur;
        g_haveCache = true;
        if (g_capZone && cur.zoneId) {
            if (g_zone && g_zone != cur.zoneId) {
                sc_game_zone_changed p = { sizeof(p), 0, g_zone, cur.zoneId };
                g_zone = cur.zoneId;
                Publish(SC_GAME_EVENT_ZONE_CHANGED, &p);
            }
            g_zone = cur.zoneId;
        }
    }
}

}  // namespace

Result StartEvents() {
    if (g_started) return Result::Ok;
    const std::vector<const char*> teleport(std::begin(kTeleportRows), std::end(kTeleportRows));
    std::vector<const char*> spawned = teleport, died = teleport, zone = teleport, seat = teleport;
    for (const char* id : kIdRows) { spawned.push_back(id); died.push_back(id); }
    size_t ne = 0;
    const events::Capability* ec = events::Capabilities(ne);
    AddRows(ec, ne, kCapSpawned, spawned);
    AddRows(ec, ne, kCapDied, died);
    AddRows(ec, ne, kCapSeat, seat);

    g_handleToId = RowsOk({ "spawn.find_seat", "spawn.handle_to_id" }) ? reinterpret_cast<HandleToIdFn>(Sig("spawn.handle_to_id")) : nullptr;
    char whySpawned[128], whyDied[128], whyZone[128], whySeat[128];
    g_capSpawned = SetCap(kCapSpawned, spawned, whySpawned, sizeof(whySpawned));
    g_capDied    = SetCap(kCapDied, died, whyDied, sizeof(whyDied));
    g_capZone    = SetCap(kCapZone, zone, whyZone, sizeof(whyZone));
    const bool seatRows = SetCap(kCapSeat, seat, whySeat, sizeof(whySeat));

    g_active.store(true, std::memory_order_release);
    if (g_capSpawned && !InstallHook(g_hooks[0], whySpawned, sizeof(whySpawned))) g_capSpawned = false;
    if (g_capDied && !InstallHook(g_hooks[1], whyDied, sizeof(whyDied))) g_capDied = false;
    // The seat hooks install for the probe (log only) when both rows are OK, and the capability
    // stays off whatever they say.
    if (seatRows && InstallHook(g_hooks[2], whySeat, sizeof(whySeat)) && !InstallHook(g_hooks[3], whySeat, sizeof(whySeat))) {
        hook::RemoveDetour(g_hooks[2].at);
        g_hooks[2].at = nullptr;
    }
    g_capSeat = false;

    const Result r = Subscribe(host::GameOwner(), "tick", OnTick, nullptr);
    if (r != Result::Ok) {
        g_active.store(false, std::memory_order_release);
        RemoveHooks();
        for (const char* cap : { kCapSpawned, kCapDied, kCapZone, kCapSeat }) caps::Set(cap, false, "game events not started");
        g_capSpawned = g_capDied = g_capZone = false;
        return r;
    }
    if (!g_capSpawned) caps::Set(kCapSpawned, false, whySpawned);
    if (!g_capDied) caps::Set(kCapDied, false, whyDied);
    if (!g_capZone) caps::Set(kCapZone, false, whyZone);
    caps::Set(kCapSeat, false, "which actor a seat transition belongs to, and whether it is a vehicle seat, needs an in-game run");
    if (!g_capSpawned) Log("[game] %s not ready: %s", kCapSpawned, whySpawned);
    if (!g_capDied) Log("[game] %s not ready: %s", kCapDied, whyDied);
    if (!g_capZone) Log("[game] %s not ready: %s", kCapZone, whyZone);
    Log("[game] game.events.vehicle_seat is off until an in-game run confirms its payload; %s",
        g_hooks[2].at && g_hooks[3].at ? "the seat probe logs Enter and Exit" : "the seat hooks aren't installed (rows not OK)");
    g_started = true;
    return Result::Ok;
}

void StopEvents() {
    if (!g_started) return;
    g_active.store(false, std::memory_order_release);
    Unsubscribe(host::GameOwner(), "tick", OnTick);
    RemoveHooks();
    for (const char* cap : { kCapSpawned, kCapDied, kCapZone, kCapSeat }) caps::Set(cap, false, "stopped");
    {
        std::lock_guard<std::mutex> hold(g_ringLock);
        g_head = g_count = 0;
        g_dropped = 0;
    }
    g_capSpawned = g_capDied = g_capZone = g_capSeat = false;
    g_handleToId = nullptr;
    g_cache = {};
    g_haveCache = false;
    g_zone = 0;
    g_lastPoll = 0;
    g_nwait = 0;
    g_seatLogged = 0;
    g_started = false;
}

}  // namespace sco::game::services
