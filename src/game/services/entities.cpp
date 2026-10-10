// game.entities 1.0 (sc_entities.h) from the game pack. New in this service: the entity class name
// and id reads, query_radius, and the streamed_in / streamed_out watch (docs/design/game-world-spikes.md
// B3 and B9). The rest reuses what the game pack has: the spawner and the mover (spawn.cpp), the
// removal game.actors uses for its NPCs (actors.cpp), the teleport.* reads (reads.h) and
// teleport.spatial's frames (services.h). The addresses come from the entities.* rows
// (sco/game/entities.h) and the rows those use.
//
// Ownership (decision 6): every entity a plugin spawns here is recorded with its handle.
// spawn.cpp's release hook (one sco::AddReleaseHook for all three services) calls
// ReleaseEntitiesOwner when the plugin unloads or crashes: its entities are removed like a
// despawn and its watches end.
//
// Two capabilities stay off until the maintainer's in-game run (sco::game::entities::
// kQueryRadiusConfirmed, kWatchConfirmed): the spike found where the game's entity walk and its
// two streaming functions are, but not which thread they run on, nor the vector layout and the
// walk's flag. Until then query_radius and watch answer SCO_UNAVAILABLE. Setting the environment
// variable SCO_ENTITIES_DIAG=1 installs the two streaming hooks log-only, and runs the walk once
// 20 s after the player has spawned, logging what that run has to confirm ("[game] game.entities
// diag: ..." lines in mod.log); it changes nothing else.
#include "entities.h"
#include "actors.h"
#include "spawn.h"
#include "../../api/internal.h"   // detail::Released
#include "sco/caps.h"
#include "sco/game/actors.h"
#include "sco/game/entities.h"
#include "sco/game/features.h"
#include "sco/game/reads.h"
#include "sco/game/services.h"
#include "sco/hook.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/signatures.h"
#include <sc_entities.h>
#include <atomic>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <vector>
#include <windows.h>

namespace sco::game::services {

namespace {

using ForEachFn = void(__fastcall*)(uintptr_t index, void* callable, uint8_t flag);
using SinksFn   = void(__fastcall*)(void* entitySystem, const void* batch);
using DeleteFn  = void(__fastcall*)(void* entitySystem, const uint64_t* handle);

constexpr uint64_t kPtrMask   = entities::kEntityHandleMask;
constexpr size_t   kClassBuf  = 96;           // longest class name read (NUL included)
constexpr uint64_t kPruneAgeMs = 120000;      // an owned entity this old that isn't streamed in is forgotten
constexpr uint64_t kDiagWaitMs = 20000;       // the walk's probe runs this long after Start, once you've spawned
constexpr uint32_t kDiagLogs   = 24;          // callbacks logged per hook
constexpr size_t   kMaxBatch   = 1024;        // handles read from one spawn batch
constexpr size_t   kMaxCollect = 65536;       // entities one query_radius walk can see

constexpr const char* kCapTransform = "game.entities.transform";
constexpr const char* kCapSpawn     = "game.entities.spawn";
constexpr const char* kCapClassOf   = "game.entities.class_of";
constexpr const char* kCapQuery     = "game.entities.query_radius";
constexpr const char* kCapWatch     = "game.entities.watch";
constexpr const char* kTeleportRows[] = {   // the reads (sco/game/teleport.h)
    "teleport.to_camera", "teleport.client_mgr", "teleport.handle_from_id", "teleport.entity_system",
};

struct Game {
    uintptr_t* entitySystem = nullptr;   // teleport.entity_system
    uintptr_t* index = nullptr;          // entities.index
    ForEachFn  forEach = nullptr;        // entities.for_each
    uint8_t*   sinks = nullptr;          // entities.spawn_sinks
    uint8_t*   deleteEntity = nullptr;   // entities.delete_entity
};

Game g_game;
bool g_started = false;
bool g_canTransform = false, g_canSpawn = false, g_canClass = false, g_canQuery = false, g_canWatch = false;
char g_whyTransform[128] = "", g_whySpawn[128] = "", g_whyClass[128] = "", g_whyQuery[160] = "", g_whyWatch[160] = "";
bool g_diag = false;
bool g_hooksInstalled = false;
uint64_t g_startedAt = 0;

// Game thread only. Reserved at start (SC_ENTITIES_MAX_OWNED), so nothing here allocates later.
struct Owned { const void* owner; uint64_t id; uint64_t at; };
std::vector<Owned> g_owned;

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

// True when every row is OK and the teleport.* reads are ready; else why names the first that
// isn't.
bool RowsReady(const std::vector<const char*>& rows, char* why, size_t n) {
    why[0] = 0;
    for (const char* id : rows) {
        const SigResult* s = SigLookup(id);
        if (!s) { snprintf(why, n, "unknown signature %s", id); return false; }
        if (s->state != SigState::Ok) { snprintf(why, n, "needs %s (%s)", id, SigStateName(s->state)); return false; }
    }
    if (!reads::Ready()) { snprintf(why, n, "the teleport.* reads aren't ready"); return false; }
    return true;
}

// Sets capability name from rows; true when it's ready.
bool SetCap(const char* name, const std::vector<const char*>& rows, char* why, size_t n) {
    const bool ready = RowsReady(rows, why, n);
    caps::Set(name, ready, why);
    return ready;
}

void Resolve() {
    g_game = {};
    if (RowOk("teleport.entity_system")) g_game.entitySystem = reinterpret_cast<uintptr_t*>(Sig("teleport.entity_system"));
    if (RowOk("entities.index")) g_game.index = reinterpret_cast<uintptr_t*>(Sig("entities.index"));
    if (RowOk("entities.for_each")) g_game.forEach = reinterpret_cast<ForEachFn>(Sig("entities.for_each"));
    if (RowOk("entities.spawn_sinks")) g_game.sinks = Sig("entities.spawn_sinks");
    if (RowOk("entities.delete_entity")) g_game.deleteEntity = Sig("entities.delete_entity");

    size_t na = 0, nf = 0, ne = 0;
    const actors::Capability* ac = actors::Capabilities(na);
    const features::Capability* fc = features::Capabilities(nf);
    const entities::Capability* ec = entities::Capabilities(ne);
    const std::vector<const char*> teleport(std::begin(kTeleportRows), std::end(kTeleportRows));

    g_canTransform = SetCap(kCapTransform, teleport, g_whyTransform, sizeof(g_whyTransform));

    std::vector<const char*> spawn = teleport;
    AddRows(ac, na, "spawn.helpers", spawn);
    AddRows(fc, nf, "npc.clear", spawn);   // no spawn that the host couldn't remove again
    g_canSpawn = RowsReady(spawn, g_whySpawn, sizeof(g_whySpawn));
    if (g_canSpawn && !EntityRemovalReady()) {
        g_canSpawn = false;
        snprintf(g_whySpawn, sizeof(g_whySpawn), "game.actors.despawn isn't ready: the host couldn't remove your entities");
    }
    if (g_canSpawn && !SpawnReleaseHooked()) {
        g_canSpawn = false;
        snprintf(g_whySpawn, sizeof(g_whySpawn), "no release hook: the host couldn't remove your entities when you unload");
    }
    caps::Set(kCapSpawn, g_canSpawn, g_whySpawn);

    std::vector<const char*> cls = teleport;
    AddRows(ec, ne, "entities.class_name", cls);
    g_canClass = SetCap(kCapClassOf, cls, g_whyClass, sizeof(g_whyClass));

    // The two that wait for the in-game run: the rows decide whether it could work, the constant
    // whether it's confirmed.
    std::vector<const char*> query = teleport;
    AddRows(ec, ne, "entities.enumerate", query);
    g_canQuery = RowsReady(query, g_whyQuery, sizeof(g_whyQuery));
    if (g_canQuery && !entities::kQueryRadiusConfirmed) {
        g_canQuery = false;
        snprintf(g_whyQuery, sizeof(g_whyQuery), "waiting for the in-game check of the game's entity walk (B3)");
    }
    caps::Set(kCapQuery, g_canQuery, g_whyQuery);

    std::vector<const char*> watch = teleport;
    AddRows(ec, ne, "entities.stream_hooks", watch);
    g_canWatch = RowsReady(watch, g_whyWatch, sizeof(g_whyWatch));
    if (g_canWatch && !entities::kWatchConfirmed) {
        g_canWatch = false;
        snprintf(g_whyWatch, sizeof(g_whyWatch), "waiting for the in-game check of the streaming hooks (B9)");
    }
    caps::Set(kCapWatch, g_canWatch, g_whyWatch);
}

// ---- game reads and calls, under SEH -----------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712). The first three are also
// called from the stream hooks, on whatever thread the game runs them.

// entity's class name into out (NUL-terminated): its length, or -1 when the game faulted or the
// text isn't a plausible class name (empty, longer than cap - 1, or not printable ASCII).
int ReadClassName(uintptr_t entity, char* out, size_t cap) {
    __try {
        const uintptr_t cls = VCall<uintptr_t>(entity, entities::kEntityClass);
        if (!cls) return -1;
        const char* name = VCall<const char*>(cls, entities::kClassName);
        if (!name) return -1;
        size_t n = 0;
        for (; name[n]; ++n) {
            const unsigned char c = static_cast<unsigned char>(name[n]);
            if (n + 1 >= cap || c < 0x21 || c > 0x7E) return -1;
        }
        if (!n) return -1;
        memcpy(out, name, n);
        out[n] = 0;
        return static_cast<int>(n);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}

bool ReadEntityId(uintptr_t entity, uint64_t& id) {
    __try {
        uint64_t tmp[2] = {};
        const uint64_t* p = VCall<const uint64_t*>(entity, entities::kEntityId, tmp);
        if (!p || !*p) return false;
        id = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The id and class name of a (streamed-in or just-removed) entity.
bool ReadEntity(uintptr_t entity, uint64_t& id, char* cls, size_t cap) {
    return ReadEntityId(entity, id) && ReadClassName(entity, cls, cap) > 0;
}

uintptr_t EntityPtr(uint64_t id) {
    __try {
        return reads::EntityFromId(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool EntityAlive(uint64_t id) { return EntityPtr(id) != 0; }

// A candidate of the entity walk: its id, its zone's id and its position in that zone's frame.
bool ReadCandidate(uintptr_t entity, uint64_t& id, uint64_t& zoneId, double local[3]) {
    if (!entity || !ReadEntityId(entity, id)) return false;
    __try {
        const uintptr_t zone = reads::EntityZone(entity);
        zoneId = zone ? reads::ZoneId(zone) : 0;
        if (!zoneId) return false;
        reads::EntityLocalPos(entity, local);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ZoneStreamed(uint64_t zoneId) {
    __try {
        return reads::ZoneFromId(zoneId) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t Now() { return GetTickCount64(); }

enum class Pose : uint8_t { Ok, NotStreamedIn, NoZone, SlotsChanged, Fault };

Pose ReadPose(uint64_t id, double pos[3], double rot[4], uint64_t& zoneId) {
    __try {
        const uintptr_t entity = reads::EntityFromId(id);
        if (!entity) return Pose::NotStreamedIn;
        const uintptr_t zone = reads::EntityZone(entity);
        if (!zone) return Pose::NoZone;
        const uint64_t zid = reads::ZoneId(zone);
        if (!zid || reads::ZoneFromId(zid) != zone) return Pose::NoZone;
        reads::EntityLocalPos(entity, pos);
        if (!reads::EntityRotation(entity, rot)) return Pose::SlotsChanged;
        zoneId = zid;
        return Pose::Ok;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Pose::Fault;
    }
}

// ---- watches -----------------------------------------------------------------------------------
//
// The stream hooks run on whatever thread the game runs the two functions on (B9: not known until
// the in-game run). They match each entity against the watches under one lock and queue the match
// in the watch's ring; the game thread's tick posts one task per watch with events, as a callout
// of the watch's owner (so a plugin that faults in its callback is the one blamed), and that task
// calls the plugin. Nothing of a plugin runs on the hooks' thread.

struct Event { uint64_t id; uint32_t what; char cls[kClassBuf]; };
constexpr uint32_t kRing = 256;   // events a watch holds; past it the oldest are dropped
constexpr int      kBatch = 256;  // callbacks one task makes

struct Watch {
    bool        used = false;
    bool        taskQueued = false;
    uint32_t    gen = 0;
    const void* owner = nullptr;
    uint32_t    what = 0;
    entities::TypeFilter filter;
    sc_entity_watch_fn fn = nullptr;
    void*       ctx = nullptr;
    uint32_t    head = 0, count = 0, dropped = 0;
    Event       ring[kRing];
};

Watch                 g_watches[SC_ENTITIES_MAX_WATCHES];
std::mutex            g_wLock;
std::atomic<uint32_t> g_active{ 0 };   // watches in use: the hooks' fast path
uint32_t              g_nextGen = 0;

void Push(Watch& w, const Event& e) {
    if (w.count == kRing) {
        w.head = (w.head + 1) % kRing;
        --w.count;
        ++w.dropped;
    }
    w.ring[(w.head + w.count) % kRing] = e;
    ++w.count;
}

void ClearWatch(Watch& w) {
    w.used = false;
    w.taskQueued = false;
    w.owner = nullptr;
    w.fn = nullptr;
    w.ctx = nullptr;
    w.head = w.count = w.dropped = 0;
    g_active.fetch_sub(1, std::memory_order_relaxed);
}

// Queues one entity's event for every watch that wants it. Any thread.
void Queue(uint32_t what, const Event& e) {
    std::lock_guard<std::mutex> hold(g_wLock);
    for (Watch& w : g_watches)
        if (w.used && (w.what & what) && entities::MatchesType(w.filter, e.cls)) Push(w, e);
}

// What the hooks do with an entity that is streaming in or out. Any thread; nothing here may
// throw or block on the game.
void Observe(uint32_t what, uintptr_t entity) {
    if (g_active.load(std::memory_order_relaxed) == 0 || !entity) return;
    Event e = {};
    e.what = what;
    if (!ReadEntity(entity, e.id, e.cls, sizeof(e.cls))) return;
    Queue(what, e);
}

// ---- the stream hooks and their diagnostics ----------------------------------------------------

SinksFn g_origSinks = nullptr;
DeleteFn g_origDelete = nullptr;
std::atomic<uint32_t> g_dSinks{ 0 }, g_dSinksGame{ 0 }, g_dSinksBad{ 0 }, g_dDeletes{ 0 }, g_dDeletesGame{ 0 };
std::atomic<uint32_t> g_dEntities{ 0 };

// [batch], [batch + 8]: what MSVC's std::vector keeps as its first and last (B9 asks the in-game
// run to confirm the layout: raw gets the first four qwords for the log). The handles are copied
// (up to max) and the element count returned; sane false when the pointers don't describe a
// vector of 8-byte handles.
size_t ReadBatch(const void* batch, uint64_t* out, size_t max, uintptr_t raw[4], bool& sane) {
    sane = false;
    __try {
        const uintptr_t* v = static_cast<const uintptr_t*>(batch);
        for (int i = 0; i < 4; ++i) raw[i] = v[i];
        const uintptr_t first = v[0], last = v[1];
        if (!first || last < first || ((last - first) & 7) || ((last - first) >> 3) > 65536) return 0;
        const size_t n = static_cast<size_t>((last - first) >> 3);
        const uint64_t* h = reinterpret_cast<const uint64_t*>(first);
        for (size_t i = 0; i < n && i < max; ++i) out[i] = h[i];
        sane = true;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool ReadHandle(const uint64_t* p, uint64_t& h) {
    __try {
        h = *p;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* YesNo(bool b) { return b ? "yes" : "no"; }

void DiagEntity(const char* what, uint64_t handle) {
    uint64_t id = 0;
    char cls[kClassBuf] = "";
    const uintptr_t entity = static_cast<uintptr_t>(handle & kPtrMask);
    const bool idOk = entity && ReadEntityId(entity, id);
    const bool clsOk = entity && ReadClassName(entity, cls, sizeof(cls)) > 0;
    Log("[game] game.entities diag:   %s handle %#llx: id %s%llu, class %s%s", what, static_cast<unsigned long long>(handle),
        idOk ? "" : "(unreadable) ", static_cast<unsigned long long>(id), clsOk ? "" : "(unreadable) ", clsOk ? cls : "");
}

void DiagSinks(const void* batch, size_t count, const uint64_t* handles, size_t have, const uintptr_t raw[4], bool sane) {
    const bool game = OnGameThread();
    const uint32_t n = g_dSinks.fetch_add(1) + 1;
    if (game) g_dSinksGame.fetch_add(1);
    if (!sane) g_dSinksBad.fetch_add(1);
    g_dEntities.fetch_add(static_cast<uint32_t>(count));
    if (n > kDiagLogs) return;
    Log("[game] game.entities diag: CallOnSpawnSinks #%u: thread %lu, game thread: %s; batch %p = [%#llx %#llx %#llx %#llx] "
        "-> %s, %zu handles",
        n, static_cast<unsigned long>(GetCurrentThreadId()), YesNo(game), batch, static_cast<unsigned long long>(raw[0]),
        static_cast<unsigned long long>(raw[1]), static_cast<unsigned long long>(raw[2]), static_cast<unsigned long long>(raw[3]),
        sane ? "a vector of handles" : "NOT a vector of handles (layout differs)", count);
    for (size_t i = 0; i < have && i < 3; ++i) DiagEntity("streamed in", handles[i]);
}

void SinksBatch(const void* batch) {
    uint64_t handles[kMaxBatch];
    uintptr_t raw[4] = {};
    bool sane = false;
    const size_t count = ReadBatch(batch, handles, kMaxBatch, raw, sane);
    const size_t have = count < kMaxBatch ? count : kMaxBatch;
    if (g_diag) DiagSinks(batch, count, handles, have, raw, sane);
    if (sane)
        for (size_t i = 0; i < have; ++i) Observe(SC_ENTITY_STREAMED_IN, static_cast<uintptr_t>(handles[i] & kPtrMask));
}

void DeletedEntity(const uint64_t* handlePtr) {
    uint64_t handle = 0;
    if (!ReadHandle(handlePtr, handle)) return;
    if (g_diag) {
        const bool game = OnGameThread();
        const uint32_t n = g_dDeletes.fetch_add(1) + 1;
        if (game) g_dDeletesGame.fetch_add(1);
        if (n <= kDiagLogs) {
            Log("[game] game.entities diag: DeleteEntity #%u: thread %lu, game thread: %s", n,
                static_cast<unsigned long>(GetCurrentThreadId()), YesNo(game));
            DiagEntity("streamed out", handle);
        }
    }
    Observe(SC_ENTITY_STREAMED_OUT, static_cast<uintptr_t>(handle & kPtrMask));
}

// CEntitySystem::CallOnSpawnSinks: the original first, so the batch's entities are registered
// when they are looked at.
void __fastcall SinksDetour(void* es, const void* batch) {
    g_origSinks(es, batch);
    SinksBatch(batch);
}

// CEntitySystem::DeleteEntity: looked at first, while the entity is still there.
void __fastcall DeleteDetour(void* es, const uint64_t* handle) {
    DeletedEntity(handle);
    g_origDelete(es, handle);
}

bool InstallHooks() {
    if (g_hooksInstalled) return true;
    if (!g_game.sinks || !g_game.deleteEntity) return false;
    hook::Error e = hook::InstallDetour(g_game.sinks, 0, reinterpret_cast<void*>(&SinksDetour), reinterpret_cast<void**>(&g_origSinks));
    if (e != hook::Error::None) {
        Log("[game] game.entities: couldn't hook CallOnSpawnSinks (%s)", hook::ErrorName(e));
        return false;
    }
    e = hook::InstallDetour(g_game.deleteEntity, 0, reinterpret_cast<void*>(&DeleteDetour), reinterpret_cast<void**>(&g_origDelete));
    if (e != hook::Error::None) {
        Log("[game] game.entities: couldn't hook DeleteEntity (%s)", hook::ErrorName(e));
        hook::RemoveDetour(g_game.sinks);
        return false;
    }
    g_hooksInstalled = true;
    return true;
}

void RemoveHooks() {
    if (!g_hooksInstalled) return;
    hook::RemoveDetour(g_game.sinks);
    hook::RemoveDetour(g_game.deleteEntity);
    g_hooksInstalled = false;
}

// ---- the game's entity walk (query_radius and its diagnostic probe) ----------------------------

// ForEach keeps a pointer to its callable and may run it later or elsewhere (B3): the callables
// and what they fill are static.
struct Callable { void* ctx; void (*fn)(void* ctx, uint64_t handle); };

std::mutex            g_collectLock;
std::vector<uint64_t> g_collected;           // reserved at start when query_radius can run
std::atomic<uint32_t> g_collectOver{ 0 };

void CollectFn(void*, uint64_t handle) {
    std::lock_guard<std::mutex> hold(g_collectLock);
    if (g_collected.size() < g_collected.capacity()) g_collected.push_back(handle);
    else g_collectOver.fetch_add(1);
}
Callable g_collectCallable = { nullptr, CollectFn };

bool CallForEach(Callable* c, uint8_t flag) {
    __try {
        g_game.forEach(*g_game.index, c, flag);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::atomic<uint32_t> g_pCalls{ 0 }, g_pOff{ 0 };
std::atomic<unsigned long> g_pFirstThread{ 0 };

void ProbeFn(void*, uint64_t handle) {
    const uint32_t n = g_pCalls.fetch_add(1) + 1;
    if (!OnGameThread()) g_pOff.fetch_add(1);
    unsigned long none = 0;
    g_pFirstThread.compare_exchange_strong(none, static_cast<unsigned long>(GetCurrentThreadId()));
    if (n <= 3) DiagEntity("walk", handle);
}
Callable g_probeCallable = { nullptr, ProbeFn };

struct Probe { int step = 0; uint8_t flag = 0; uint64_t at = 0; bool done = false; };
Probe g_probe;

void LogProbe(const char* when) {
    Log("[game] game.entities diag: ForEach(flag %u) %s: %u callbacks, %u of them off the game thread, first callback on thread %lu "
        "(game thread: %s)",
        static_cast<unsigned>(g_probe.flag), when, g_pCalls.load(), g_pOff.load(), g_pFirstThread.load(),
        YesNo(g_pFirstThread.load() == 0 || g_pOff.load() == 0));
}

bool PlayerKnown() {
    __try {
        uintptr_t actor, entity;
        return reads::LocalPlayer(actor, entity);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RunProbe(uint8_t flag) {
    g_pCalls = 0;
    g_pOff = 0;
    g_pFirstThread = 0;
    g_probe.flag = flag;
    const bool ok = CallForEach(&g_probeCallable, flag);
    g_probe.at = Now();
    if (!ok) {
        Log("[game] game.entities diag: ForEach(flag %u) faulted", static_cast<unsigned>(flag));
        g_probe.step = 3;   // nothing to wait for
        return;
    }
    LogProbe("returned");
    g_probe.step = 1;
}

// The one-run confirmation of B3 and B9 (SCO_ENTITIES_DIAG=1): the walk with each flag value, and
// the class name of your own entity, 20 s after you've spawned. The hooks log as they are called.
void DiagTick() {
    if (!g_diag || g_probe.done) return;
    const uint64_t now = Now();
    if (g_probe.step == 0) {
        if (now - g_startedAt < kDiagWaitMs || !PlayerKnown()) return;
        uintptr_t actor = 0, entity = 0;
        uint64_t id = 0;
        char cls[kClassBuf] = "";
        __try {
            if (reads::LocalPlayer(actor, entity) && ReadEntity(entity, id, cls, sizeof(cls)))
                Log("[game] game.entities diag: your entity: id %llu, class %s", static_cast<unsigned long long>(id), cls);
            else
                Log("[game] game.entities diag: your entity's id or class name couldn't be read");
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            Log("[game] game.entities diag: faulted reading your entity");
        }
        if (!g_game.forEach || !g_game.index || !*g_game.index) {
            Log("[game] game.entities diag: the entity walk isn't available (entities.* rows not OK)");
            g_probe.done = true;
            return;
        }
        RunProbe(0);
        return;
    }
    if (g_probe.step == 1 && now - g_probe.at >= 1000) {
        LogProbe("1 s later");
        g_probe.step = 2;
    } else if (g_probe.step >= 2 && now - g_probe.at >= 3000) {
        if (g_probe.step == 2) LogProbe("3 s later");
        if (g_probe.flag == 0) {
            RunProbe(1);
        } else {
            Log("[game] game.entities diag: stream hooks so far: %u spawn batches (%u on the game thread, %u not vectors of handles, "
                "%u entities), %u deletes (%u on the game thread)",
                g_dSinks.load(), g_dSinksGame.load(), g_dSinksBad.load(), g_dEntities.load(), g_dDeletes.load(), g_dDeletesGame.load());
            g_probe.done = true;
        }
    }
}

// ---- watch events to plugins -------------------------------------------------------------------

// One task of a watch's owner (ctx: its slot and generation). Calls the plugin for the events
// queued, up to kBatch; every call is re-checked against the slot, because the plugin may unwatch
// or unload from inside its own callback.
void* TaskCtx(uint32_t slot, uint32_t gen) { return reinterpret_cast<void*>((static_cast<uintptr_t>(gen) << 8) | slot); }

void FlushTask(void* ctx) {
    const uintptr_t v = reinterpret_cast<uintptr_t>(ctx);
    const uint32_t slot = static_cast<uint32_t>(v & 0xFF), gen = static_cast<uint32_t>(v >> 8);
    if (slot >= SC_ENTITIES_MAX_WATCHES) return;
    for (int i = 0; i < kBatch; ++i) {
        Event e;
        sc_entity_watch_fn fn;
        void* fnCtx;
        {
            std::lock_guard<std::mutex> hold(g_wLock);
            Watch& w = g_watches[slot];
            if (!w.used || w.gen != gen) return;   // unwatched, or released: nothing of it may run
            if (w.count == 0) {
                w.taskQueued = false;
                return;
            }
            e = w.ring[w.head];
            w.head = (w.head + 1) % kRing;
            --w.count;
            fn = w.fn;
            fnCtx = w.ctx;
        }
        fn(fnCtx, e.what, e.id, e.cls);
    }
    std::lock_guard<std::mutex> hold(g_wLock);
    Watch& w = g_watches[slot];
    if (w.used && w.gen == gen) w.taskQueued = false;   // what is left goes out with the next tick's task
}

void PostWatchTasks() {
    if (g_active.load(std::memory_order_relaxed) == 0) return;
    struct Pending { uint32_t slot, gen; const void* owner; };
    Pending pending[SC_ENTITIES_MAX_WATCHES];
    size_t n = 0;
    {
        std::lock_guard<std::mutex> hold(g_wLock);
        for (uint32_t i = 0; i < SC_ENTITIES_MAX_WATCHES; ++i) {
            Watch& w = g_watches[i];
            if (!w.used) continue;
            if (w.dropped) {
                Log("[game] warning: game.entities: %s's watch on %s%s dropped %u events (it didn't keep up)", host::PluginId(static_cast<const sco_plugin*>(w.owner)),
                    w.filter.text, w.filter.prefix ? "*" : "", w.dropped);
                w.dropped = 0;
            }
            if (w.count == 0 || w.taskQueued) continue;
            w.taskQueued = true;
            pending[n++] = { i, w.gen, w.owner };
        }
    }
    for (size_t i = 0; i < n; ++i) {
        if (Post(FlushTask, TaskCtx(pending[i].slot, pending[i].gen), pending[i].owner) == Result::Ok) continue;
        std::lock_guard<std::mutex> hold(g_wLock);   // couldn't queue (queue full, or the owner released): try again next tick
        Watch& w = g_watches[pending[i].slot];
        if (w.used && w.gen == pending[i].gen) w.taskQueued = false;
    }
}

void OnTick(const char*, const void*, void*) {
    DiagTick();
    PostWatchTasks();
}

// ---- game.entities (sc_entities.h) -------------------------------------------------------------

sco_result Unavailable(const void* owner, const char* fn, const char* cap, const char* why) {
    if (!g_started) return Fail(owner, Result::Unavailable, "%s: game.entities is stopped", fn);
    return Fail(owner, Result::Unavailable, "%s: %s isn't available on this game build (%s)", fn, cap, why);
}

// Normalises rot (x y z w) into q; false for a rotation that isn't finite or is zero.
bool Normalise(const double rot[4], double q[4]) {
    double n = 0;
    for (int i = 0; i < 4; ++i) n += rot[i] * rot[i];
    n = std::sqrt(n);
    if (!std::isfinite(n) || n < 1e-9) return false;
    for (int i = 0; i < 4; ++i) q[i] = rot[i] / n;
    return true;
}

bool Finite3(const double v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

int SvcAlive(uint64_t id) { return OnGameThread() && g_started && g_canTransform && id && EntityAlive(id) ? 1 : 0; }

sco_result SvcClassOf(uint64_t id, char* out, uint32_t* inoutSize) {
    if (!inoutSize) return Fail(nullptr, Result::BadArg, "class_of: inout_size is NULL");
    const uint32_t capacity = *inoutSize;
    *inoutSize = 0;
    if (out && capacity) out[0] = 0;
    if (!OnGameThread()) return Fail(nullptr, Result::WrongThread, "class_of: game thread only");
    if (!out && capacity) return Fail(nullptr, Result::BadArg, "class_of: out is NULL but the size isn't 0");
    if (!g_started || !g_canClass) return Unavailable(nullptr, "class_of", kCapClassOf, g_whyClass);
    if (!id) return Fail(nullptr, Result::BadArg, "class_of: id 0");
    const uintptr_t entity = EntityPtr(id);
    if (!entity) return Fail(nullptr, Result::NotFound, "class_of: %llu isn't streamed in", static_cast<unsigned long long>(id));
    char cls[kClassBuf];
    const int n = ReadClassName(entity, cls, sizeof(cls));
    if (n <= 0) return Fail(nullptr, Result::Failed, "class_of: the game gave no plausible class name for %llu", static_cast<unsigned long long>(id));
    *inoutSize = static_cast<uint32_t>(n) + 1;
    if (capacity < static_cast<uint32_t>(n) + 1) return SCO_TOO_MANY;
    memcpy(out, cls, static_cast<size_t>(n) + 1);
    return SCO_OK;
}

sco_result SvcGetTransform(uint64_t id, double outPos[3], double outRot[4], uint64_t* outZone) {
    if (outPos) outPos[0] = outPos[1] = outPos[2] = 0;
    if (outRot) outRot[0] = outRot[1] = outRot[2] = outRot[3] = 0;
    if (outZone) *outZone = 0;
    if (!OnGameThread()) return Fail(nullptr, Result::WrongThread, "get_transform: game thread only");
    if (!outPos && !outRot && !outZone) return Fail(nullptr, Result::BadArg, "get_transform: every out pointer is NULL");
    if (!g_started || !g_canTransform) return Unavailable(nullptr, "get_transform", kCapTransform, g_whyTransform);
    if (!id) return Fail(nullptr, Result::BadArg, "get_transform: id 0");
    double pos[3] = {}, rot[4] = {};
    uint64_t zone = 0;
    switch (ReadPose(id, pos, rot, zone)) {
        case Pose::Ok: break;
        case Pose::NotStreamedIn:
            return Fail(nullptr, Result::NotFound, "get_transform: %llu isn't streamed in", static_cast<unsigned long long>(id));
        case Pose::NoZone:
            return Fail(nullptr, Result::NotFound, "get_transform: %llu isn't in a zone", static_cast<unsigned long long>(id));
        case Pose::SlotsChanged:
            return Fail(nullptr, Result::Failed, "get_transform: the entity's rotation slots aren't the ones checked in 4.10.196");
        case Pose::Fault: return Fail(nullptr, Result::Failed, "get_transform: fault while reading the entity");
    }
    if (outPos) memcpy(outPos, pos, sizeof(pos));
    if (outRot) memcpy(outRot, rot, sizeof(rot));
    if (outZone) *outZone = zone;
    return SCO_OK;
}

bool OwnedBy(const void* owner, uint64_t id) {
    for (const Owned& o : g_owned)
        if (o.owner == owner && o.id == id) return true;
    return false;
}

// Decision 7: what a plugin may move: the entities it spawned (here, spawn_as, or an NPC) and the
// player's own vehicles, once the product registered them.
bool MayMove(const void* owner, uint64_t id) {
    return OwnedBy(owner, id) || SpawnedBy(owner, id) || NpcOwnedBy(owner, id) || IsPlayerVehicle(id);
}

sco_result SvcSetTransform(sco_plugin* self, uint64_t id, uint64_t zoneId, const double pos[3], const double rot[4]) {
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "set_transform: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "set_transform: self is not a loaded plugin's handle");
    if (!g_started || !g_canTransform) return Unavailable(self, "set_transform", kCapTransform, g_whyTransform);
    if (!id || !pos || !rot) return Fail(self, Result::BadArg, "set_transform: id 0, or pos or rot is NULL");
    double q[4];
    if (!Normalise(rot, q)) return Fail(self, Result::BadArg, "set_transform: the rotation isn't finite or is zero");
    if (!Finite3(pos)) return Fail(self, Result::BadArg, "set_transform: the position isn't finite");
    if (!MayMove(self, id))
        return Fail(self, Result::Failed, "set_transform: %llu is not yours to move (only what you spawned, and the player's own vehicles)",
                    static_cast<unsigned long long>(id));
    switch (MoveEntity(id, zoneId, pos, q)) {
        case MoveResult::Ok: return SCO_OK;
        case MoveResult::NotStreamedIn:
            return Fail(self, Result::NotFound, "set_transform: %llu isn't streamed in yet (a fresh spawn takes seconds: wait for alive)",
                        static_cast<unsigned long long>(id));
        case MoveResult::NoZone:
            return Fail(self, Result::NotFound, "set_transform: %llu isn't in a zone yet", static_cast<unsigned long long>(id));
        case MoveResult::ZoneConversion:
            return Fail(self, Result::NotFound, "set_transform: zone %llu (or the entity's zone) can't be placed: not streamed in?",
                        static_cast<unsigned long long>(zoneId));
        case MoveResult::Refused:
            return Fail(self, Result::Failed, "set_transform: the game refused the move (entity move slots don't match this build)");
    }
    return Fail(self, Result::Failed, "set_transform: failed");
}

void PruneOwned() {
    const uint64_t now = Now();
    for (size_t i = 0; i < g_owned.size();) {
        if (now - g_owned[i].at >= kPruneAgeMs && !EntityAlive(g_owned[i].id)) {
            g_owned[i] = g_owned.back();
            g_owned.pop_back();
        } else {
            ++i;
        }
    }
}

sco_result SvcSpawn(sco_plugin* self, const char* cls, uint64_t zoneId, const double pos[3], const double rot[4], uint64_t* outId) {
    if (outId) *outId = 0;
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "spawn: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "spawn: self is not a loaded plugin's handle");
    if (!g_started || !g_canSpawn) return Unavailable(self, "spawn", kCapSpawn, g_whySpawn);
    if (!SpawnReleaseHooked() || !EntityRemovalReady())
        return Fail(self, Result::Unavailable, "spawn: the host couldn't remove your entities when you unload, so it spawns none");
    if (!cls || !*cls || !pos || !rot || !outId) return Fail(self, Result::BadArg, "spawn: entity_class, pos, rot or out_id is NULL or empty");
    if (!zoneId) return Fail(self, Result::BadArg, "spawn: zone 0; pass a zone id (teleport.spatial player_pose)");
    if (!Finite3(pos)) return Fail(self, Result::BadArg, "spawn: the position isn't finite");
    double q[4];
    if (!Normalise(rot, q)) return Fail(self, Result::BadArg, "spawn: the rotation isn't finite or is zero");
    if (g_owned.size() >= SC_ENTITIES_MAX_OWNED) PruneOwned();
    if (g_owned.size() >= SC_ENTITIES_MAX_OWNED)
        return Fail(self, Result::TooMany, "spawn: %u entities spawned through game.entities are alive", SC_ENTITIES_MAX_OWNED);
    if (!ZoneStreamed(zoneId))
        return Fail(self, Result::NotFound, "spawn: zone %llu isn't streamed in", static_cast<unsigned long long>(zoneId));
    uint64_t id = 0;
    if (const char* err = SpawnEntityInZone(cls, zoneId, pos, id, q))
        return Fail(self, strcmp(err, "unknown entity class") == 0 ? Result::NotFound : Result::Failed, "spawn(%.64s): %s", cls, err);
    g_owned.push_back({ self, id, Now() });   // within the reservation: checked above
    *outId = id;
    Log("[game] game.entities: %s spawned %.64s as %llu", host::PluginId(self), cls, static_cast<unsigned long long>(id));
    return SCO_OK;
}

sco_result SvcDespawn(sco_plugin* self, uint64_t id) {
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "despawn: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "despawn: self is not a loaded plugin's handle");
    if (!g_started || !g_canSpawn) return Unavailable(self, "despawn", kCapSpawn, g_whySpawn);
    if (!id) return Fail(self, Result::BadArg, "despawn: id 0");
    for (size_t i = 0; i < g_owned.size(); ++i) {
        if (g_owned[i].owner != self || g_owned[i].id != id) continue;
        g_owned[i] = g_owned.back();
        g_owned.pop_back();
        RemoveEntityLater(id);
        return SCO_OK;
    }
    return Fail(self, Result::NotFound, "despawn: %llu is not an entity this plugin spawned with game.entities (or it's despawned already)",
                static_cast<unsigned long long>(id));
}

sco_result SvcWatch(sco_plugin* self, uint32_t what, const char* type, sc_entity_watch_fn fn, void* ctx, uint64_t* outId) {
    if (outId) *outId = 0;
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "watch: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "watch: self is not a loaded plugin's handle");
    if (!g_started || !g_canWatch) return Unavailable(self, "watch", kCapWatch, g_whyWatch);
    if (!fn || !outId) return Fail(self, Result::BadArg, "watch: fn or out_watch_id is NULL");
    if (what == 0 || (what & ~(SC_ENTITY_STREAMED_IN | SC_ENTITY_STREAMED_OUT)))
        return Fail(self, Result::BadArg, "watch: what must be SC_ENTITY_STREAMED_IN, SC_ENTITY_STREAMED_OUT or both");
    entities::TypeFilter filter;
    if (const char* why = entities::ParseType(type, filter)) return Fail(self, Result::BadArg, "watch: %s", why);
    std::lock_guard<std::mutex> hold(g_wLock);
    for (uint32_t i = 0; i < SC_ENTITIES_MAX_WATCHES; ++i) {
        Watch& w = g_watches[i];
        if (w.used) continue;
        w.used = true;
        w.taskQueued = false;
        w.gen = ++g_nextGen;
        w.owner = self;
        w.what = what;
        w.filter = filter;
        w.fn = fn;
        w.ctx = ctx;
        w.head = w.count = w.dropped = 0;
        g_active.fetch_add(1, std::memory_order_relaxed);
        *outId = (static_cast<uint64_t>(w.gen) << 8) | i;   // never 0: gen starts at 1
        return SCO_OK;
    }
    return Fail(self, Result::TooMany, "watch: %u watches exist", SC_ENTITIES_MAX_WATCHES);
}

sco_result SvcUnwatch(sco_plugin* self, uint64_t watchId) {
    const void* who = LivePlugin(self) ? self : nullptr;
    if (!OnGameThread()) return Fail(who, Result::WrongThread, "unwatch: game thread only");
    if (!who) return Fail(nullptr, Result::BadArg, "unwatch: self is not a loaded plugin's handle");
    const uint32_t slot = static_cast<uint32_t>(watchId & 0xFF);
    const uint64_t gen = watchId >> 8;
    if (slot < SC_ENTITIES_MAX_WATCHES) {
        std::lock_guard<std::mutex> hold(g_wLock);
        Watch& w = g_watches[slot];
        if (w.used && w.gen == gen && w.owner == self) {
            ClearWatch(w);
            return SCO_OK;
        }
    }
    return Fail(self, Result::NotFound, "unwatch: %llu is not a watch this plugin registered (or it's ended already)",
                static_cast<unsigned long long>(watchId));
}

// One walk of the game's entity list: every handle it hands out is collected, then filtered on the
// game thread. Reached only once the in-game run confirmed that the walk is synchronous.
sco_result SvcQueryRadius(uint64_t zoneId, const double pos[3], double radius, const char* classFilter, uint64_t* outIds,
                          uint32_t max, uint32_t* outCount, uint32_t* outMore) {
    if (outCount) *outCount = 0;
    if (outMore) *outMore = 0;
    if (!OnGameThread()) return Fail(nullptr, Result::WrongThread, "query_radius: game thread only");
    if (!pos || !outCount || !outMore || (!outIds && max))
        return Fail(nullptr, Result::BadArg, "query_radius: pos, out_count or out_more is NULL, or out_ids is NULL with max > 0");
    if (!g_started || !g_canQuery) return Unavailable(nullptr, "query_radius", kCapQuery, g_whyQuery);
    if (!zoneId || !Finite3(pos) || !std::isfinite(radius) || radius <= 0)
        return Fail(nullptr, Result::BadArg, "query_radius: zone 0, a position that isn't finite, or a radius that isn't finite and positive");
    entities::TypeFilter filter;
    const bool filtered = classFilter && *classFilter;
    if (filtered)
        if (const char* why = entities::ParseType(classFilter, filter)) return Fail(nullptr, Result::BadArg, "query_radius: %s", why);
    if (!ZoneStreamed(zoneId))
        return Fail(nullptr, Result::NotFound, "query_radius: zone %llu isn't streamed in", static_cast<unsigned long long>(zoneId));
    {
        std::lock_guard<std::mutex> hold(g_collectLock);
        g_collected.clear();
    }
    g_collectOver = 0;
    if (!CallForEach(&g_collectCallable, entities::kForEachFlag))
        return Fail(nullptr, Result::Failed, "query_radius: fault in the game's entity walk");
    std::vector<uint64_t> handles;
    {
        std::lock_guard<std::mutex> hold(g_collectLock);
        handles = g_collected;
    }
    if (g_collectOver.load()) return Fail(nullptr, Result::Failed, "query_radius: more than %zu entities", kMaxCollect);
    uint32_t count = 0, more = 0;
    const double r2 = radius * radius;
    for (const uint64_t handle : handles) {
        const uintptr_t entity = static_cast<uintptr_t>(handle & kPtrMask);
        uint64_t id = 0, entityZone = 0;
        double local[3] = {};
        if (!ReadCandidate(entity, id, entityZone, local)) continue;
        double at[3] = { local[0], local[1], local[2] };
        if (entityZone != zoneId) {
            const double identity[4] = { 0, 0, 0, 1 };
            double rot[4];
            if (!PoseToZone(entityZone, zoneId, local, identity, at, rot)) continue;
        }
        const double dx = at[0] - pos[0], dy = at[1] - pos[1], dz = at[2] - pos[2];
        if (dx * dx + dy * dy + dz * dz > r2) continue;
        if (filtered) {
            char cls[kClassBuf];
            if (ReadClassName(entity, cls, sizeof(cls)) <= 0 || !entities::MatchesType(filter, cls)) continue;
        }
        if (count < max) outIds[count++] = id;
        else ++more;
    }
    *outCount = count;
    *outMore = more;
    return SCO_OK;
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

const sc_entities_v1 kEntities = {
    sizeof(sc_entities_v1), 0,        SvcAlive, SvcClassOf, SvcGetTransform, SvcSetTransform,
    SvcSpawn,               SvcDespawn, SvcWatch, SvcUnwatch, SvcQueryRadius,  SvcLastError,
};

void LogCap(const char* cap, bool ready, const char* why) {
    if (!ready) Log("[game] %s not ready: %s; it answers SCO_UNAVAILABLE", cap, why);
}

void ResetState() {
    for (const char* cap : { kCapTransform, kCapSpawn, kCapClassOf, kCapQuery, kCapWatch }) caps::Set(cap, false, "game.entities stopped");
    g_canTransform = g_canSpawn = g_canClass = g_canQuery = g_canWatch = false;
    g_game = {};
}

}  // namespace

Result StartEntities() {
    if (g_started) return Result::Ok;
    char env[8] = "";
    GetEnvironmentVariableA("SCO_ENTITIES_DIAG", env, sizeof(env));
    g_diag = env[0] == '1';
    g_startedAt = Now();
    g_probe = {};
    Resolve();
    g_owned.reserve(SC_ENTITIES_MAX_OWNED);
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        g_errors.reserve(host::kMaxPlugins + 1);
    }
    if (g_canQuery || (g_diag && g_game.forEach)) {
        std::lock_guard<std::mutex> hold(g_collectLock);
        g_collected.reserve(kMaxCollect);
    }
    Result r = host::ProvideGameService(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_0, &kEntities);
    if (r == Result::Ok) {
        r = Subscribe(host::GameOwner(), "tick", OnTick, nullptr);
        if (r != Result::Ok) host::WithdrawGameService(SC_ENTITIES_NAME);
    }
    if (r != Result::Ok) {
        ResetState();
        return r;
    }
    // The hooks go in only when something uses them: the diagnostic run, or a confirmed watch.
    // watch needs them: it is ready only when they are installed.
    const bool wantHooks = g_diag || entities::kWatchConfirmed;
    if (wantHooks && g_game.sinks && g_game.deleteEntity) {
        if (InstallHooks()) {
            if (g_diag) Log("[game] game.entities diag ON (SCO_ENTITIES_DIAG=1): the stream hooks are installed and only log; watch stays %s",
                            g_canWatch ? "ready" : "off");
        } else if (g_canWatch) {
            g_canWatch = false;
            snprintf(g_whyWatch, sizeof(g_whyWatch), "the stream hooks couldn't be installed");
            caps::Set(kCapWatch, false, g_whyWatch);
        }
    }
    LogCap(kCapTransform, g_canTransform, g_whyTransform);
    LogCap(kCapSpawn, g_canSpawn, g_whySpawn);
    LogCap(kCapClassOf, g_canClass, g_whyClass);
    LogCap(kCapQuery, g_canQuery, g_whyQuery);
    LogCap(kCapWatch, g_canWatch, g_whyWatch);
    g_started = true;
    return Result::Ok;
}

void StopEntities() {
    if (!g_started) return;
    RemoveHooks();
    // Every plugin has unloaded (Stop runs after UnloadAll), so anything still owned belongs to a
    // plugin whose release failed: ask the game to remove it now (game.actors is still started).
    if (!g_owned.empty()) {
        Log("[game] game.entities stopping: removing %zu entities still owned", g_owned.size());
        for (const Owned& o : g_owned) RemoveEntityLater(o.id);
    }
    if (g_diag)
        Log("[game] game.entities diag: stopping after %u spawn batches (%u on the game thread) and %u deletes (%u on the game thread)",
            g_dSinks.load(), g_dSinksGame.load(), g_dDeletes.load(), g_dDeletesGame.load());
    Unsubscribe(host::GameOwner(), "tick", OnTick);
    host::WithdrawGameService(SC_ENTITIES_NAME);
    {
        std::lock_guard<std::mutex> hold(g_wLock);
        for (Watch& w : g_watches)
            if (w.used) ClearWatch(w);
    }
    g_owned.clear();
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        g_errors.clear();
    }
    {
        std::lock_guard<std::mutex> hold(g_collectLock);
        g_collected.clear();
    }
    ResetState();
    g_diag = false;
    g_started = false;
}

void ReleaseEntitiesOwner(const void* owner) {
    if (!g_started) return;
    size_t n = 0;
    for (size_t i = 0; i < g_owned.size();) {
        if (g_owned[i].owner != owner) { ++i; continue; }
        const uint64_t id = g_owned[i].id;
        g_owned[i] = g_owned.back();
        g_owned.pop_back();
        RemoveEntityLater(id);
        ++n;
    }
    if (n) {
        const char* id = host::PluginId(static_cast<const sco_plugin*>(owner));
        Log("[game] game.entities: %s unloaded: removing the %zu entities it spawned", id ? id : "?", n);
    }
    {
        std::lock_guard<std::mutex> hold(g_wLock);
        for (Watch& w : g_watches)
            if (w.used && w.owner == owner) ClearWatch(w);
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
