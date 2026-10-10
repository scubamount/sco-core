// game.actors 1.0 (sc_actors.h) from the game pack. Moved from sc-offline's npc built-in
// (src/npc.cpp: SpawnNpcs, RemoveEntityById and HandleOf, the 1.5 s removal check, the direct
// removal fallback and Banish), same offsets and slots. The addresses come from the npc.* rows
// (npc.clear in sco/game/features.h, npc.direct_remove in sco/game/actors.h), the spawn.* rows
// (spawn.helpers through spawn.cpp's spawner, spawn.handle_to_id) and the teleport.* rows behind
// the game pack's reads (reads.h: your player, zones, entities).
//
// Ownership (decision 6): every NPC a plugin spawns here is recorded with its handle. spawn.cpp's
// release hook (one sco::AddReleaseHook for both services) calls ReleaseActorsOwner when the
// plugin unloads or crashes, and those NPCs are removed like a despawn. The npc built-in's menu,
// npcs.txt list and Clear NPCs stay in the product.
#include "actors.h"
#include "spawn.h"
#include "../../api/internal.h"   // detail::Released
#include "sco/caps.h"
#include "sco/game/actors.h"
#include "sco/game/features.h"
#include "sco/game/reads.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include <sc_actors.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <iterator>
#include <mutex>
#include <vector>
#include <windows.h>

namespace sco::game::services {

namespace {

using HandleFromIdFn = void(__fastcall*)(uint64_t* handle, uint64_t id);
using HandleToIdFn   = uint64_t*(__fastcall*)(const void* handleField, uint64_t* entityId);
using DirectRemoveFn = bool(__fastcall*)(uintptr_t entitySystem, uint64_t handle);

constexpr uint64_t kPtrMask    = 0xFFFFFFFFFFFFull;
constexpr uint64_t kVerifyMs   = 1500;    // npc.cpp: a removal is checked 1.5 s after it was asked for
constexpr uint64_t kStreamInMs = 60000;   // a despawned NPC that hasn't streamed in by then is given up

constexpr const char* kCapLocal   = "game.actors.local_player";
constexpr const char* kCapSpawn   = "game.actors.spawn_npc";
constexpr const char* kCapDespawn = "game.actors.despawn";
constexpr const char* kTeleportRows[] = {   // the reads (sco/game/teleport.h)
    "teleport.to_camera", "teleport.client_mgr", "teleport.handle_from_id", "teleport.entity_system",
};

// What the removal calls, from the rows.
struct Game {
    uintptr_t*     entitySystem = nullptr;   // teleport.entity_system
    HandleFromIdFn handleFromId = nullptr;   // teleport.handle_from_id
    HandleToIdFn   handleToId = nullptr;     // spawn.handle_to_id
    int32_t        removeSlot = 0;           // RemoveEntity's vtable slot (npc.remove_entity_call)
    bool           directRows = false;       // every npc.direct_remove row is OK
    DirectRemoveFn directRemove = nullptr;   // checked against the slot on first use
    int            directState = 0;          // 0 not tried, 1 found, -1 not usable
};

Game g_game;
bool g_started = false;
bool g_canLocal = false, g_canSpawn = false, g_canDespawn = false;
char g_whyLocal[128] = "", g_whySpawn[128] = "", g_whyDespawn[128] = "";

// Game thread only. Reserved at start (SC_ACTORS_MAX_NPCS each): an NPC moves from g_npcs to
// g_removals and spawning counts both, so neither ever grows past its reservation.
struct Npc { const void* owner; uint64_t id; };
enum class Stage : uint8_t { Waiting, Removed, Direct };
struct Removal { uint64_t id; uint64_t at; Stage stage; };
std::vector<Npc>     g_npcs;
std::vector<Removal> g_removals;

// Any thread (last_error, and the wrong-thread refusals). One entry per plugin handle plus the
// no-handle slot (owner nullptr), reserved at start.
struct Err { const void* owner; char text[160]; };
std::mutex       g_errLock;
std::vector<Err> g_errors;

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

sco_result Fail(const void* owner, Result r, const char* fmt, ...) {
    char text[sizeof(Err::text)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> hold(g_errLock);
    Err* e = nullptr;
    for (Err& x : g_errors)
        if (x.owner == owner) e = &x;
    if (!e && g_errors.size() < g_errors.capacity()) {   // never reallocates: nothing throws here
        g_errors.push_back({ owner, {} });
        e = &g_errors.back();
    }
    if (e) memcpy(e->text, text, sizeof(text));
    return static_cast<sco_result>(r);
}

// A handle NewPlugin returned whose Release hasn't started.
bool LivePlugin(const sco_plugin* self) { return self && host::PluginId(self) && !sco::detail::Released(self); }

bool RowOk(const char* id) {
    const SigResult* s = SigLookup(id);
    return s && s->state == SigState::Ok;
}

template <class C> void AddRows(const C* caps, size_t n, const char* name, std::vector<const char*>& out) {
    for (size_t i = 0; i < n; ++i)
        if (strcmp(caps[i].name, name) == 0) out.insert(out.end(), caps[i].rows, caps[i].rows + caps[i].count);
}

// Sets capability name from rows; true when it's ready and the reads are. why: the first row
// that isn't OK, for the refusals.
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

void Resolve() {
    g_game = {};
    if (RowOk("teleport.entity_system")) g_game.entitySystem = reinterpret_cast<uintptr_t*>(Sig("teleport.entity_system"));
    if (RowOk("teleport.handle_from_id")) g_game.handleFromId = reinterpret_cast<HandleFromIdFn>(Sig("teleport.handle_from_id"));
    if (RowOk("spawn.handle_to_id")) g_game.handleToId = reinterpret_cast<HandleToIdFn>(Sig("spawn.handle_to_id"));
    if (RowOk("npc.remove_entity_call")) g_game.removeSlot = Rel32(Sig("npc.remove_entity_call") + features::kRemoveSlotDisp);

    size_t na = 0, nf = 0;
    const actors::Capability* ac = actors::Capabilities(na);
    const features::Capability* fc = features::Capabilities(nf);
    std::vector<const char*> direct;
    AddRows(ac, na, "npc.direct_remove", direct);
    g_game.directRows = !direct.empty();
    for (const char* id : direct) g_game.directRows = g_game.directRows && RowOk(id);

    const std::vector<const char*> teleport(std::begin(kTeleportRows), std::end(kTeleportRows));
    std::vector<const char*> local = teleport, spawn = teleport, despawn = teleport;
    local.push_back("spawn.handle_to_id");
    AddRows(ac, na, "spawn.helpers", spawn);
    AddRows(fc, nf, "npc.clear", spawn);     // no spawn that the host couldn't remove again
    AddRows(fc, nf, "npc.clear", despawn);
    g_canLocal   = SetCap(kCapLocal, local, g_whyLocal, sizeof(g_whyLocal)) && g_game.handleToId;
    g_canSpawn   = SetCap(kCapSpawn, spawn, g_whySpawn, sizeof(g_whySpawn)) && g_game.removeSlot;
    g_canDespawn = SetCap(kCapDespawn, despawn, g_whyDespawn, sizeof(g_whyDespawn)) && g_game.removeSlot;
}

// ---- game reads and calls, under SEH -----------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712).

const char* const kNotSpawned = "you're not spawned yet";

const char* ReadLocal(uint64_t& actorId, uint64_t& entityId) {
    __try {
        uintptr_t actor, entity;
        uint64_t aid = 0, id = 0;
        if (!reads::LocalPlayer(actor, entity, aid)) return kNotSpawned;
        g_game.handleToId(reinterpret_cast<const void*>(actor + 8), &id);
        if (!aid || !id) return "your actor or entity id read as 0";
        actorId = aid;
        entityId = id;
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your player";
    }
}

bool ZoneStreamed(uint64_t zoneId) {
    __try {
        return reads::ZoneFromId(zoneId) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool EntityExists(uint64_t id) {
    __try {
        return reads::EntityFromId(id) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// RemoveEntity takes an entity handle (pointer and tag bits), not an id. The entity system's
// handle-by-id slot works for spawned NPCs; teleport.handle_from_id is the second try (npc.cpp).
uint64_t HandleOf(uint64_t id) {
    uint64_t handle = 0;
    __try {
        uint64_t out = 0;
        if (const uint64_t* h = VCall<const uint64_t*>(*g_game.entitySystem, actors::kEsHandleById, &out, id)) handle = *h;
        if (!(handle & kPtrMask) && g_game.handleFromId) g_game.handleFromId(&handle, id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return (handle & kPtrMask) ? handle : 0;
}

bool CallRemove(uint64_t id) {
    if (!g_game.removeSlot || !g_game.entitySystem) return false;
    const uint64_t handle = HandleOf(id);
    if (!handle) return false;
    __try {
        return VCall<bool>(*g_game.entitySystem, static_cast<size_t>(g_game.removeSlot), handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// RemoveEntity can hand the removal to a manager that never finishes it offline; the internal
// function it ends in removes the entity directly. Used only when the entity system's slot still
// points at the RemoveEntity npc.remove_entity found.
void ResolveDirectRemove() {
    if (g_game.directState) return;
    g_game.directState = -1;
    if (!g_game.directRows || !g_game.entitySystem) return;
    __try {
        const uint8_t* fn = *reinterpret_cast<const uint8_t* const*>(Rd<uintptr_t>(*g_game.entitySystem) + g_game.removeSlot);
        if (fn != Sig("npc.remove_entity")) {
            Log("[game] game.actors: RemoveEntity's code changed; direct removal fallback disabled");
            return;
        }
        g_game.directRemove = reinterpret_cast<DirectRemoveFn>(Sig("npc.direct_remove"));
        g_game.directState = 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        Log("[game] game.actors: fault while locating the direct removal fallback");
    }
}

bool CallDirectRemove(uint64_t id) {
    ResolveDirectRemove();
    const uint64_t handle = g_game.directRemove ? HandleOf(id) : 0;
    if (!handle) return false;
    __try {
        return g_game.directRemove(*g_game.entitySystem, handle);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Last resort when the game won't delete it: ~17,000 km away, far outside streaming range.
bool MoveAway(uint64_t id) {
    __try {
        static const double kAway[3] = { 1.0e7, 1.0e7, 1.0e7 };
        const uintptr_t e = reads::EntityFromId(id);
        return e && reads::SetEntityLocalPose(e, kAway, nullptr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void Banish(uint64_t id) {
    if (!MoveAway(id)) Log("[game] warning: game.actors: couldn't remove or move NPC %llu", static_cast<unsigned long long>(id));
}

// ---- removals ----------------------------------------------------------------------------------

uint64_t Now() { return GetTickCount64(); }

// One step of a removal (npc.cpp's RemoveEntityById and VerifyRemovals); false once it's done.
bool Step(Removal& r, uint64_t now) {
    switch (r.stage) {
        case Stage::Waiting:   // not streamed in yet: remove it once it is
            if (EntityExists(r.id)) {
                CallRemove(r.id);
                r.stage = Stage::Removed;
                r.at = now;
                return true;
            }
            if (now - r.at < kStreamInMs) return true;
            Log("[game] warning: game.actors: NPC %llu never streamed in; not removed", static_cast<unsigned long long>(r.id));
            return false;
        case Stage::Removed:
            if (now - r.at < kVerifyMs) return true;
            if (!EntityExists(r.id)) return false;
            if (CallDirectRemove(r.id)) {   // check once more after it's had a frame
                r.stage = Stage::Direct;
                r.at = now;
                return true;
            }
            Banish(r.id);
            return false;
        case Stage::Direct:
            if (now - r.at < kVerifyMs) return true;
            if (EntityExists(r.id)) Banish(r.id);
            return false;
    }
    return false;
}

void StartRemoval(uint64_t id) {
    g_removals.push_back({ id, Now(), Stage::Waiting });   // within the reservation (see g_npcs)
    if (!Step(g_removals.back(), g_removals.back().at)) g_removals.pop_back();
}

void ProcessRemovals() {
    const uint64_t now = Now();
    for (size_t i = 0; i < g_removals.size();) {
        if (Step(g_removals[i], now)) {
            ++i;
        } else {
            g_removals[i] = g_removals.back();
            g_removals.pop_back();
        }
    }
}

void OnTick(const char*, const void*, void*) { ProcessRemovals(); }

// ---- game.actors (sc_actors.h) -----------------------------------------------------------------

sco_result Unavailable(const void* owner, const char* fn, const char* cap, const char* why) {
    if (!g_started) return Fail(owner, Result::Unavailable, "%s: game.actors is stopped", fn);
    return Fail(owner, Result::Unavailable, "%s: %s isn't available on this game build (%s)", fn, cap, why);
}

sco_result SvcLocalPlayer(uint64_t* outActorId, uint64_t* outEntityId) {
    if (outActorId) *outActorId = 0;
    if (outEntityId) *outEntityId = 0;
    if (!OnGameThread()) return Fail(nullptr, Result::WrongThread, "local_player: game thread only");
    if (!outActorId && !outEntityId) return Fail(nullptr, Result::BadArg, "local_player: both out pointers are NULL");
    if (!g_started || !g_canLocal) return Unavailable(nullptr, "local_player", kCapLocal, g_whyLocal);
    uint64_t actorId = 0, entityId = 0;
    if (const char* err = ReadLocal(actorId, entityId))
        return Fail(nullptr, err == kNotSpawned ? Result::NotFound : Result::Failed, "local_player: %s", err);
    if (outActorId) *outActorId = actorId;
    if (outEntityId) *outEntityId = entityId;
    return SCO_OK;
}

sco_result SvcSpawnNpc(sco_plugin* self, const char* cls, uint64_t zoneId, const double pos[3], uint64_t* outId) {
    if (outId) *outId = 0;
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "spawn_npc: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "spawn_npc: self is not a loaded plugin's handle");
    if (!g_started || !g_canSpawn) return Unavailable(self, "spawn_npc", kCapSpawn, g_whySpawn);
    if (!SpawnReleaseHooked())
        return Fail(self, Result::Unavailable, "spawn_npc: no release hook, so the host couldn't remove your NPCs when you unload");
    if (!cls || !*cls || !pos || !outId) return Fail(self, Result::BadArg, "spawn_npc: archetype_class, pos or out_id is NULL or empty");
    if (!zoneId) return Fail(self, Result::BadArg, "spawn_npc: zone 0; pass a zone id (teleport.spatial player_pose)");
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i])) return Fail(self, Result::BadArg, "spawn_npc: the position isn't finite");
    if (g_npcs.size() + g_removals.size() >= SC_ACTORS_MAX_NPCS)
        return Fail(self, Result::TooMany, "spawn_npc: %u NPCs are alive or being removed", SC_ACTORS_MAX_NPCS);
    if (!ZoneStreamed(zoneId))
        return Fail(self, Result::NotFound, "spawn_npc: zone %llu isn't streamed in", static_cast<unsigned long long>(zoneId));
    uint64_t id = 0;
    if (const char* err = SpawnEntityInZone(cls, zoneId, pos, id))
        return Fail(self, strcmp(err, "unknown entity class") == 0 ? Result::NotFound : Result::Failed, "spawn_npc(%.64s): %s",
                    cls, err);
    g_npcs.push_back({ self, id });   // within the reservation: checked above
    *outId = id;
    Log("[game] game.actors: %s spawned %.64s as NPC %llu", host::PluginId(self), cls, static_cast<unsigned long long>(id));
    return SCO_OK;
}

sco_result SvcDespawn(sco_plugin* self, uint64_t id) {
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "despawn: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "despawn: self is not a loaded plugin's handle");
    if (!g_started || !g_canDespawn) return Unavailable(self, "despawn", kCapDespawn, g_whyDespawn);
    if (!id) return Fail(self, Result::BadArg, "despawn: id 0");
    for (size_t i = 0; i < g_npcs.size(); ++i) {
        if (g_npcs[i].owner != self || g_npcs[i].id != id) continue;
        g_npcs[i] = g_npcs.back();
        g_npcs.pop_back();
        StartRemoval(id);
        return SCO_OK;
    }
    return Fail(self, Result::NotFound, "despawn: %llu is not an NPC this plugin spawned with spawn_npc (or it's despawned already)",
                static_cast<unsigned long long>(id));
}

sco_result SvcLastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (!inoutSize) return SCO_BAD_ARG;
    const uint32_t capacity = *inoutSize;
    *inoutSize = 0;
    if ((!out && capacity) || (self && !LivePlugin(self))) return SCO_BAD_ARG;
    char text[sizeof(Err::text)] = "";
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        for (const Err& e : g_errors)
            if (e.owner == self) memcpy(text, e.text, sizeof(text));
    }
    const size_t need = strnlen(text, sizeof(text) - 1) + 1;
    *inoutSize = static_cast<uint32_t>(need);
    if (capacity < need) return SCO_TOO_MANY;
    memcpy(out, text, need - 1);
    out[need - 1] = 0;
    return SCO_OK;
}

const sc_actors_v1 kActors = {
    sizeof(sc_actors_v1), 0, SvcLocalPlayer, SvcSpawnNpc, SvcDespawn, SvcLastError,
};

void LogCap(const char* cap, bool ready, const char* why) {
    if (!ready) Log("[game] %s not ready: %s; it answers SCO_UNAVAILABLE", cap, why);
}

}  // namespace

Result StartActors() {
    if (g_started) return Result::Ok;
    Resolve();
    g_npcs.reserve(SC_ACTORS_MAX_NPCS);
    g_removals.reserve(SC_ACTORS_MAX_NPCS);
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        g_errors.reserve(host::kMaxPlugins + 1);
    }
    Result r = host::ProvideGameService(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, &kActors);
    if (r == Result::Ok) {
        r = Subscribe(host::GameOwner(), "tick", OnTick, nullptr);
        if (r != Result::Ok) host::WithdrawGameService(SC_ACTORS_NAME);
    }
    if (r != Result::Ok) {
        for (const char* cap : { kCapLocal, kCapSpawn, kCapDespawn }) caps::Set(cap, false, "game.actors not published");
        g_game = {};
        g_canLocal = g_canSpawn = g_canDespawn = false;
        return r;
    }
    LogCap(kCapLocal, g_canLocal, g_whyLocal);
    LogCap(kCapSpawn, g_canSpawn, g_whySpawn);
    LogCap(kCapDespawn, g_canDespawn, g_whyDespawn);
    g_started = true;
    return Result::Ok;
}

void StopActors() {
    if (!g_started) return;
    // Every plugin has unloaded (Stop runs after UnloadAll), so anything still owned belongs to a
    // plugin whose release failed: ask the game to remove it now. Removals in flight end here;
    // RemoveEntity was asked for each one already, the checks after it don't run.
    if (!g_npcs.empty()) {
        Log("[game] game.actors stopping: removing %zu NPCs still owned", g_npcs.size());
        for (const Npc& n : g_npcs) CallRemove(n.id);
    }
    Unsubscribe(host::GameOwner(), "tick", OnTick);
    host::WithdrawGameService(SC_ACTORS_NAME);
    for (const char* cap : { kCapLocal, kCapSpawn, kCapDespawn }) caps::Set(cap, false, "stopped");
    g_npcs.clear();
    g_removals.clear();
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        g_errors.clear();
    }
    g_game = {};
    g_canLocal = g_canSpawn = g_canDespawn = false;
    g_started = false;
}

void ReleaseActorsOwner(const void* owner) {
    size_t n = 0;
    for (size_t i = 0; i < g_npcs.size();) {
        if (g_npcs[i].owner != owner) { ++i; continue; }
        const uint64_t id = g_npcs[i].id;
        g_npcs[i] = g_npcs.back();
        g_npcs.pop_back();
        StartRemoval(id);
        ++n;
    }
    if (n) {
        const char* id = host::PluginId(static_cast<const sco_plugin*>(owner));
        Log("[game] game.actors: %s unloaded: removing the %zu NPCs it spawned", id ? id : "?", n);
    }
    std::lock_guard<std::mutex> hold(g_errLock);
    for (size_t i = 0; i < g_errors.size(); ++i)
        if (g_errors[i].owner == owner) {
            g_errors[i] = g_errors.back();
            g_errors.pop_back();
            break;
        }
}

}  // namespace sco::game::services
