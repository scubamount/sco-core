// spawn.entities 1.2 (sc_spawn.h) from the game pack. Moved from sc-offline's spawn built-in
// (src/builtins/spawn_plugin.cpp: the table, who may move what, the refusals and their log lines)
// and the spawner functions it called (spawner.cpp: SpawnEntityNearPlayer, EntityClassExists,
// LocalPlayerEntityId, PlayerShipId, EntityAlive; build.cpp: MoveEntityLocal), same offsets and
// slots. The addresses come from the spawn.* rows (sco/game/actors.h) and the teleport.* rows
// (reads.h); the mover's frames convert through teleport.spatial's zone tree (ZoneOfEntity,
// PoseToZone in services.h). The spawn.ship command, the menu and the spawner's tick stay in
// the product.
#include "spawn.h"
#include "sco/game/actors.h"
#include "sco/game/reads.h"
#include "sco/game/services.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/signatures.h"
#include <sc_spawn.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>
#include <windows.h>

namespace sco::game::services {

namespace {

using SpawnParamsCtorFn = void(__fastcall*)(void* params);
using SpawnSetClassFn   = void(__fastcall*)(void* params, uintptr_t entityClass);
using SpawnSetLocFn     = void(__fastcall*)(void* params, const void* quatTS, uint64_t zoneHostId);
using SpawnSetFlagsFn   = void(__fastcall*)(void* params, uint32_t flags);
using TeamCategoryFn    = uint8_t(__fastcall*)(uint8_t tag);
using HandleToIdFn      = uint64_t*(__fastcall*)(const void* handleField, uint64_t* entityId);

constexpr uint64_t kPtrMask = 0xFFFFFFFFFFFFull;

// What the spawner calls, from the rows. ok: every spawn.helpers row is OK and the teleport.*
// reads are ready. handleToId (local_player_id) only when every spawn.seat_picker row is OK too,
// as in sc-offline.
struct Spawner {
    bool              ok = false;
    SpawnParamsCtorFn ctor = nullptr;
    SpawnSetClassFn   setClass = nullptr;
    SpawnSetLocFn     setLocation = nullptr;
    SpawnSetFlagsFn   setFlags = nullptr;
    TeamCategoryFn    teamCategory = nullptr;
    HandleToIdFn      handleToId = nullptr;
    uintptr_t*        entitySystem = nullptr;   // teleport.entity_system
    uintptr_t*        components = nullptr;     // the global 8 bytes past it (spawn.landing_helper checks)
    uint8_t           teamTag = 0;              // the spawn helper's own tag (spawn.team_tag)
};

Spawner g_sp;
bool    g_started = false;
bool    g_releaseHook = false;   // OnRelease is added (sco::AddReleaseHook)

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

template <typename T> T Row(const char* id) { return reinterpret_cast<T>(Sig(id)); }

// Every row of an actors capability (sco/game/actors.h) is OK.
bool RowsOk(const char* capability) {
    size_t n = 0;
    const actors::Capability* c = actors::Capabilities(n);
    for (size_t i = 0; i < n; ++i) {
        if (strcmp(c[i].name, capability) != 0) continue;
        for (size_t k = 0; k < c[i].count; ++k) {
            const SigResult* s = SigLookup(c[i].rows[k]);
            if (!s || s->state != SigState::Ok) return false;
        }
        return true;
    }
    return false;
}

bool Resolve() {
    g_sp = {};
    if (!reads::Ready() || !RowsOk("spawn.helpers")) return false;
    uint8_t* es = Sig("teleport.entity_system");
    g_sp.entitySystem = reinterpret_cast<uintptr_t*>(es);
    g_sp.components   = reinterpret_cast<uintptr_t*>(es + 8);
    g_sp.teamTag      = *Sig("spawn.team_tag");
    g_sp.teamCategory = Row<TeamCategoryFn>("spawn.team_category");
    g_sp.setFlags     = Row<SpawnSetFlagsFn>("spawn.set_flags");
    g_sp.setClass     = Row<SpawnSetClassFn>("spawn.set_class");
    g_sp.setLocation  = Row<SpawnSetLocFn>("spawn.set_location");
    g_sp.ctor         = Row<SpawnParamsCtorFn>("spawn.params_ctor");
    if (RowsOk("spawn.seat_picker")) g_sp.handleToId = Row<HandleToIdFn>("spawn.handle_to_id");
    g_sp.ok = true;
    return true;
}

bool Ready() { return g_started && g_sp.ok; }

// ---- game reads and calls, under SEH -----------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712).

uintptr_t ClassRegistry() { return VCall<uintptr_t>(*g_sp.entitySystem, actors::kEsClassRegistry); }

uintptr_t EntityComponent(uintptr_t entity, const char* type) {
    uint8_t tmp[16] = {};
    const uint16_t* id = VCall<const uint16_t*>(*g_sp.components, actors::kComponentsTypeId, tmp, type);
    if (!id) return 0;
    uint16_t typeId = *id;
    uint8_t out[16] = {};
    const uint64_t* h = VCall<const uint64_t*>(entity, actors::kEntityComponent, out, &typeId);
    return h ? (*h & kPtrMask) : 0;
}

bool ClassExists(const char* cls) {
    __try {
        const uintptr_t registry = ClassRegistry();
        return registry && VCall<uintptr_t>(registry, actors::kRegistryFindClass, cls) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t LocalPlayerId() {
    if (!g_sp.handleToId) return 0;
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return 0;
        uint64_t id = 0;
        g_sp.handleToId(reinterpret_cast<const void*>(actor + 8), &id);
        return id;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

uint64_t PlayerShipId() {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return 0;
        const uintptr_t zone = reads::EntityZone(entity);
        if (!zone) return 0;
        const uint64_t id = reads::ZoneId(zone);
        const uintptr_t ship = reads::EntityFromId(id);
        return ship && EntityComponent(ship, "IItemPortContainer") ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

bool EntityAlive(uint64_t id) {
    __try {
        return reads::EntityFromId(id) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

const char* PlayerZonePos(const double offset[3], uint64_t& zoneId, double pos[3]) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t zone = reads::EntityZone(entity);
        if (!zone) return "you're not in a zone";
        zoneId = reads::ZoneId(zone);
        if (!zoneId || reads::ZoneFromId(zoneId) != zone) return "zone id lookup mismatch";
        reads::EntityLocalPos(entity, pos);
        for (int i = 0; i < 3; ++i) pos[i] += offset[i];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading your position";
    }
    return nullptr;
}

const char* SpawnInZone(const char* entityClass, uint64_t zoneId, const double pos[3], uint64_t& id) {
    __try {
        const uintptr_t es = *g_sp.entitySystem;
        const uintptr_t cls = VCall<uintptr_t>(ClassRegistry(), actors::kRegistryFindClass, entityClass);
        if (!cls) return "unknown entity class";

        alignas(16) uint8_t params[actors::kSpawnParamsSize] = {};
        g_sp.ctor(params);
        g_sp.setClass(params, cls);
        const struct { double rot[4]; double pos[3]; double scale; } where = { { 0, 0, 0, 1 }, { pos[0], pos[1], pos[2] }, 1.0 };
        g_sp.setLocation(params, &where, zoneId);
        g_sp.setFlags(params, actors::kSpawnFlags);

        uintptr_t batch = 0;
        VCall<void>(es, actors::kEsCreateBatch, &batch, "starcitzenofflinemods ship spawner",
                    static_cast<uint32_t>(g_sp.teamCategory(g_sp.teamTag)), static_cast<uint32_t>(0));
        if (!batch) return "couldn't create a spawn batch";
        uintptr_t attributes[2] = {};
        VCall<void>(es, actors::kEsSpawnAttributes, attributes);
        uint64_t newId[2] = {};
        VCall<void>(batch, actors::kBatchSpawn, newId, params, attributes);
        uintptr_t owned = batch;
        VCall<void>(es, actors::kEsReleaseBatch, &owned);
        id = newId[0];
        return id ? nullptr : "the spawn batch gave no entity id";
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while spawning";
    }
}

const char* SpawnNearPlayer(const char* entityClass, const double offset[3], uint64_t& id) {
    if (!g_sp.ok) return "the spawner isn't available";
    uint64_t zoneId = 0;
    double pos[3] = {};
    if (const char* err = PlayerZonePos(offset, zoneId, pos)) return err;
    return SpawnInZone(entityClass, zoneId, pos, id);
}

// Moves (and with rot, turns) an entity within its zone, in local coordinates.
bool MoveEntityLocal(uint64_t id, const double pos[3], const double rot[4]) {
    __try {
        const uintptr_t e = reads::EntityFromId(id);
        return e && reads::SetEntityLocalPose(e, pos, rot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- who may move what -------------------------------------------------------------------------
//
// Game thread only, like every service call and the runtime's Release.

struct Owned { const void* owner; uint64_t id; };
std::vector<Owned>    g_owned;            // spawns made through spawn_as, by plugin handle
std::vector<uint64_t> g_playerVehicles;   // RegisterPlayerVehicle (the product)
constexpr size_t kMaxOwned = 4096;        // past it, ids that no longer resolve are dropped first

void ForgetOwner(const void* owner) {
    g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(), [owner](const Owned& o) { return o.owner == owner; }),
                  g_owned.end());
}

bool Remember(const void* owner, uint64_t id) {
    if (g_owned.size() >= kMaxOwned)
        g_owned.erase(std::remove_if(g_owned.begin(), g_owned.end(), [](const Owned& o) { return !EntityAlive(o.id); }),
                      g_owned.end());
    if (g_owned.size() >= kMaxOwned) return false;
    g_owned.push_back({ owner, id });
    return true;
}

bool MayMove(const void* owner, uint64_t id) {
    if (g_releaseHook)
        for (const Owned& o : g_owned)
            if (o.owner == owner && o.id == id) return true;
    return std::find(g_playerVehicles.begin(), g_playerVehicles.end(), id) != g_playerVehicles.end();
}

// A plugin unloaded or crashed (sco::AddReleaseHook): its spawns are nobody's now, so a later
// plugin given the same handle can't move them.
void OnRelease(const void* owner) { ForgetOwner(owner); }

// ---- spawn.entities (sc_spawn.h) ---------------------------------------------------------------

const char* SvcSpawnNearPlayer(const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGameThread()) return "game thread only";
    if (!Ready()) return "the spawner isn't available on this game build";
    if (!cls || !*cls || !offset || !outId) return "bad argument";
    uint64_t id = 0;
    const char* err = SpawnNearPlayer(cls, offset, id);   // guards the game calls itself
    if (!err) *outId = id;
    return err;
}

const char* SvcSpawnAs(sco_plugin* self, const char* cls, const double offset[3], uint64_t* outId) {
    if (outId) *outId = 0;
    if (!OnGameThread()) return "game thread only";
    if (!self) return "bad argument";
    const char* err = SvcSpawnNearPlayer(cls, offset, outId);
    if (!err && !Remember(self, *outId)) return "spawned, but too many entities are recorded to move it";
    return err;
}

// Why set_entity_transform answered 0, logged once per id and reason so plugin authors can tell
// the 0s apart without mod.log filling up when a plugin retries every tick.
enum class Refusal : uint8_t { BadArgument, NotYours, NotStreamedIn, NoZone, ZoneConversion, MoveFailed };
struct Logged { uint64_t id; Refusal why; };
std::vector<Logged> g_logged;
constexpr size_t kMaxLogged = 1024;   // forgotten all at once past it: at worst a reason is logged again

int Refuse(uint64_t id, Refusal why) {
    for (const Logged& l : g_logged)
        if (l.id == id && l.why == why) return 0;
    if (g_logged.size() >= kMaxLogged) g_logged.clear();
    g_logged.push_back({ id, why });
    static const char* const kWhy[] = {
        "bad argument (null self, id 0, or a position or rotation that isn't finite or a zero rotation)",
        "not yours (move only entities you spawned with spawn_as)",
        "not streamed in yet (a fresh spawn takes seconds; wait for entity_alive)",
        "zone conversion failed (the entity isn't in a zone yet)",
        "zone conversion failed (the target zone or the entity's zone can't be placed)",
        "the game refused the move (entity move slots don't match this build)",
    };
    Log("[game] warning: set_entity_transform(%llu) -> 0: %s", static_cast<unsigned long long>(id),
        kWhy[static_cast<int>(why)]);
    return 0;
}

int SvcSetEntityTransform(sco_plugin* self, uint64_t id, uint64_t zoneId, const double pos[3], const double rot[4]) {
    if (!OnGameThread() || !Ready()) return 0;   // the documented no-ops: nothing to explain per id
    if (!self || !id || !pos || !rot) return Refuse(id, Refusal::BadArgument);
    if (!MayMove(self, id)) return Refuse(id, Refusal::NotYours);
    double q[4], n = 0;
    for (int i = 0; i < 4; ++i) n += rot[i] * rot[i];
    n = std::sqrt(n);
    if (!std::isfinite(n) || n < 1e-9) return Refuse(id, Refusal::BadArgument);
    for (int i = 0; i < 4; ++i) q[i] = rot[i] / n;
    for (int i = 0; i < 3; ++i)
        if (!std::isfinite(pos[i])) return Refuse(id, Refusal::BadArgument);
    if (!EntityAlive(id)) return Refuse(id, Refusal::NotStreamedIn);
    const uint64_t in = ZoneOfEntity(id);   // the zone the entity is in: MoveEntityLocal's frame
    if (!in) return Refuse(id, Refusal::NoZone);
    double localPos[3], localRot[4];
    if (!PoseToZone(zoneId, in, pos, q, localPos, localRot)) return Refuse(id, Refusal::ZoneConversion);
    return MoveEntityLocal(id, localPos, localRot) ? 1 : Refuse(id, Refusal::MoveFailed);
}

int SvcClassExists(const char* cls) { return OnGameThread() && Ready() && cls && *cls && ClassExists(cls) ? 1 : 0; }
uint64_t SvcLocalPlayerId() { return OnGameThread() && Ready() ? LocalPlayerId() : 0; }
uint64_t SvcPlayerShipId() { return OnGameThread() && Ready() ? PlayerShipId() : 0; }
int SvcEntityAlive(uint64_t id) { return OnGameThread() && Ready() && EntityAlive(id) ? 1 : 0; }

const sc_spawn_service_v1 kService = {
    sizeof(sc_spawn_service_v1), SvcSpawnNearPlayer, SvcClassExists, SvcLocalPlayerId, SvcPlayerShipId, SvcEntityAlive,
    SvcSetEntityTransform,       SvcSpawnAs,
};

}  // namespace

Result StartSpawn() {
    if (g_started) return Result::Ok;
    if (!Resolve())
        Log("[game] spawn.helpers rows not OK: spawn.entities answers \"the spawner isn't available on this game build\" (see the [core] lines)");
    const Result r = host::ProvideGameService(SC_SPAWN_SERVICE_NAME, SC_SPAWN_SERVICE_VERSION, &kService);
    if (r != Result::Ok) {
        g_sp = {};
        return r;
    }
    // Without the hook a reloaded plugin could inherit an unloaded one's spawns, so the mover
    // refuses everything spawned through spawn_as then (spawn_as still spawns).
    g_releaseHook = AddReleaseHook(OnRelease) == Result::Ok;
    if (!g_releaseHook) Log("[game] warning: no release hook left: set_entity_transform moves no spawn_as entity");
    g_started = true;
    return Result::Ok;
}

void StopSpawn() {
    if (!g_started) return;
    if (g_releaseHook) RemoveReleaseHook(OnRelease);
    g_releaseHook = false;
    host::WithdrawGameService(SC_SPAWN_SERVICE_NAME);
    g_owned.clear();
    g_logged.clear();
    g_sp = {};
    g_started = false;
}

bool SpawnedBy(const void* owner, uint64_t id) {
    if (!g_releaseHook || !owner || !id) return false;
    for (const Owned& o : g_owned)
        if (o.owner == owner && o.id == id) return true;
    return false;
}

bool IsPlayerVehicle(uint64_t id) {
    return id && std::find(g_playerVehicles.begin(), g_playerVehicles.end(), id) != g_playerVehicles.end();
}

void RegisterPlayerVehicle(uint64_t entityId) {
    if (entityId && std::find(g_playerVehicles.begin(), g_playerVehicles.end(), entityId) == g_playerVehicles.end())
        g_playerVehicles.push_back(entityId);
}

void UnregisterPlayerVehicle(uint64_t entityId) {
    g_playerVehicles.erase(std::remove(g_playerVehicles.begin(), g_playerVehicles.end(), entityId), g_playerVehicles.end());
}

}  // namespace sco::game::services
