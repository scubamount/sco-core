// game.world 1.0 (sc_world.h) from the game pack. raycast is sc-offline's build mode ground ray
// (src/build.cpp: CastInZone, RayHit and RaySlotsOk), moved onto the rows the spike names
// (docs/design/game-world-spikes.md B1): same ray parameter block, same physics world call, same
// walk up the zone's parents. camera is the camera build mode aims with (build.cpp Target), read
// at the offsets build.camera_fields pins (B2); the field of view is not available (B2, BLOCKED).
// The addresses come from the build.* rows (sco/game/world.h) and the teleport.* rows behind the
// game pack's reads (reads.h: your player, zones, entities).
//
// Both functions are read-only queries on the game thread. A ray's length is clamped to
// SC_WORLD_RAY_MAX_DISTANCE, and nothing here waits or loops on the game: one physics call per
// zone, at most three zones.
#include "world.h"
#include "../../api/internal.h"   // detail::Released
#include "sco/caps.h"
#include "sco/game/reads.h"
#include "sco/game/services.h"
#include "sco/game/world.h"
#include "sco/host.h"
#include "sco/log.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include <sc_world.h>
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

using ReleaseGridFn = void(__fastcall*)(uintptr_t grid);

constexpr const char* kCapRay    = "game.world.raycast";
constexpr const char* kCapCamera = "game.world.camera";
constexpr const char* kTeleportRows[] = {   // the reads (sco/game/teleport.h)
    "teleport.to_camera", "teleport.client_mgr", "teleport.handle_from_id", "teleport.entity_system",
};
constexpr size_t kActorCamera = 0x208;   // actor -> camera object, checked by teleport.to_camera +0x151
constexpr int    kMaxZones    = 3;       // the zone and two parents, as build.cpp walks them

// What the cast calls, from the rows.
struct Game {
    uintptr_t*     physWorld = nullptr;      // build.phys_world
    ReleaseGridFn  releaseGrid = nullptr;    // build.release_grid
    const char*    rayTag = nullptr;         // build.ray_tag
    const uint8_t* proxySlot = nullptr;      // build.entity_ray_proxy: what a live entity's slot 0x208 must hold
    const uint8_t* skipSlot = nullptr;       // build.entity_skip_add: and slot 0x430
};

Game g_game;
bool g_started = false;
bool g_canRay = false, g_canCamera = false;
char g_whyRay[128] = "", g_whyCamera[128] = "";
int  g_raySlots = -1;   // the live-entity slot check: -1 not run, 0 failed, 1 passed

// The last failure of any query, from any thread.
std::mutex g_errLock;
char       g_error[192] = "";

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

sco_result Fail(sco_result r, const char* fmt, ...) {
    char text[sizeof(g_error)];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof(text), fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> hold(g_errLock);
    memcpy(g_error, text, sizeof(text));
    return r;
}

// A handle NewPlugin returned whose Release hasn't started.
bool LivePlugin(const sco_plugin* self) { return self && host::PluginId(self) && !sco::detail::Released(self); }

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
    g_raySlots = -1;
    size_t nw = 0;
    const world::Capability* wc = world::Capabilities(nw);
    const std::vector<const char*> teleport(std::begin(kTeleportRows), std::end(kTeleportRows));
    std::vector<const char*> ray = teleport, camera = teleport;
    AddRows(wc, nw, "build.ground_ray", ray);
    AddRows(wc, nw, "build.camera", camera);
    g_canRay    = SetCap(kCapRay, ray, g_whyRay, sizeof(g_whyRay));
    g_canCamera = SetCap(kCapCamera, camera, g_whyCamera, sizeof(g_whyCamera));
    if (!g_canRay) return;
    g_game.physWorld   = reinterpret_cast<uintptr_t*>(Sig("build.phys_world"));
    g_game.releaseGrid = reinterpret_cast<ReleaseGridFn>(Sig("build.release_grid"));
    g_game.rayTag      = reinterpret_cast<const char*>(Sig("build.ray_tag"));
    g_game.proxySlot   = Sig("build.entity_ray_proxy");
    g_game.skipSlot    = Sig("build.entity_skip_add");
}

// ---- game reads and calls, under SEH -----------------------------------------------------------
//
// No C++ object with a destructor lives in these frames (MSVC C2712).

// The physics world's skip list the ray block points at: empty, so nothing is skipped
// (build.cpp's PhysSkipList; build mode adds its preview entity, this service adds nothing).
struct PhysSkipList {
    uint64_t slots[8];
    uint64_t heap;
    int32_t  count, capacity;
    int64_t  lock;
    int32_t  owner;
    uint8_t  depth, pad[3];
};
static_assert(sizeof(PhysSkipList) == 0x60, "PhysSkipList layout");
PhysSkipList g_skip = { {}, 0, 0, 8, 0, -1, 0, {} };

enum class Slots { Ok, NotSpawned, Changed, Fault };

// build.cpp's RaySlotsOk: a live entity's vtable slots 0x208 and 0x430 hold the functions the rows
// found, once, on the first cast. Not spawned yet is not cached: it can be asked again.
Slots CheckRaySlots() {
    if (g_raySlots >= 0) return g_raySlots ? Slots::Ok : Slots::Changed;
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return Slots::NotSpawned;
        const uintptr_t vt = Rd<uintptr_t>(entity);
        g_raySlots = Rd<const uint8_t*>(vt + world::kEntityRayProxySlot) == g_game.proxySlot
                  && Rd<const uint8_t*>(vt + world::kEntitySkipAddSlot) == g_game.skipSlot;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return Slots::Fault;
    }
    if (!g_raySlots) Log("[game] game.world: the entity ray slots changed; raycast answers SCO_UNAVAILABLE");
    return g_raySlots ? Slots::Ok : Slots::Changed;
}

uintptr_t ZoneById(uint64_t id) {
    __try {
        return reads::ZoneFromId(id);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct ZoneLink { uint64_t id; uintptr_t parent; };

bool ReadZoneLink(uintptr_t zone, ZoneLink& out) {
    __try {
        out.parent = reads::ZoneParent(zone);
        out.id = reads::ZoneId(zone);
        return out.id != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// The cast in one zone (build.cpp CastInZone): from o along d, both in that zone's frame and |d|
// = len, the longest the ray goes. The distance to the first hit in metres, or -1.
double CastInZone(uintptr_t zone, const double o[3], const double d[3], double len, bool& fault) {
    uintptr_t ref = 0;
    __try {
        const uintptr_t* grid = VCall<const uintptr_t*>(zone, 0x30, &ref);
        alignas(16) uint8_t rp[0xA8] = {};
        alignas(16) uint8_t hits[0x60 * 2] = {};
        memcpy(rp + 0x18, o, 3 * sizeof(double));
        for (int i = 0; i < 3; ++i) reinterpret_cast<float*>(rp + 0x30)[i] = static_cast<float>(d[i]);
        *reinterpret_cast<int32_t*>(rp + 0x3C)     = 0x101;
        *reinterpret_cast<uint64_t*>(rp + 0x40)    = 0x20F;
        *reinterpret_cast<void**>(rp + 0x48)       = hits;
        *reinterpret_cast<int32_t*>(rp + 0x50)     = 1;
        *reinterpret_cast<void**>(rp + 0x70)       = &g_skip;
        *reinterpret_cast<uintptr_t*>(rp + 0x80)   = grid ? *grid : 0;
        *reinterpret_cast<const char**>(rp + 0x88) = g_game.rayTag;
        g_skip.count = 0;
        const int n = grid && *grid ? VCall<int>(*g_game.physWorld, 0x1C0, static_cast<void*>(rp), 30) : 0;
        if (ref) g_game.releaseGrid(ref);
        ref = 0;
        const float dist = *reinterpret_cast<const float*>(hits + 0x10);
        if (n <= 0 || !*reinterpret_cast<const uint64_t*>(hits) || !(dist >= 0.0f) || dist > len) return -1;
        return dist;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        fault = true;
        return -1;
    }
}

struct Camera { double pos[3]; double rot[4]; uint64_t zone; };

// nullptr, or why the camera couldn't be read.
const char* ReadCamera(Camera& c) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return "you're not spawned yet";
        const uintptr_t cam = Rd<uintptr_t>(actor + kActorCamera);
        if (!cam) return "your actor has no camera yet";
        const double* p = reinterpret_cast<const double*>(cam + world::kCameraPosition);
        const float*  q = reinterpret_cast<const float*>(cam + world::kCameraRotation);
        for (int i = 0; i < 3; ++i) c.pos[i] = p[i];
        for (int i = 0; i < 4; ++i) c.rot[i] = q[i];
        for (int i = 0; i < 3; ++i)
            if (!std::isfinite(c.pos[i])) return "the camera position isn't finite";
        for (int i = 0; i < 4; ++i)
            if (!std::isfinite(c.rot[i])) return "the camera rotation isn't finite";
        const uintptr_t zone = reads::EntityZone(entity);
        c.zone = zone ? reads::ZoneId(zone) : 0;
        return nullptr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return "fault while reading the camera";
    }
}

// ---- game.world (sc_world.h) -------------------------------------------------------------------

constexpr const char* kNotSpawned = "you're not spawned yet";

sco_result Unavailable(const char* fn, const char* cap, const char* why) {
    if (!g_started) return Fail(SCO_UNAVAILABLE, "%s: game.world is stopped", fn);
    return Fail(SCO_UNAVAILABLE, "%s: %s isn't available on this game build (%s)", fn, cap, why);
}

bool Finite3(const double v[3]) { return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]); }

void ZeroHit(sc_world_hit* h) {
    const uint32_t size = h->size;
    memset(h, 0, sizeof(*h));
    h->size = size;
}

sco_result SvcRaycast(uint64_t zoneId, const double from[3], const double dir[3], double maxDist, sc_world_hit* outHit) {
    if (!outHit) return Fail(SCO_BAD_ARG, "raycast: out_hit is NULL");
    if (outHit->size < sizeof(sc_world_hit))
        return Fail(SCO_BAD_ARG, "raycast: out_hit->size is %u, not sizeof(sc_world_hit) (%zu)", outHit->size, sizeof(sc_world_hit));
    ZeroHit(outHit);
    if (!OnGameThread()) return Fail(SCO_WRONG_THREAD, "raycast: game thread only");
    if (!g_started || !g_canRay) return Unavailable("raycast", kCapRay, g_whyRay);
    if (!from || !dir) return Fail(SCO_BAD_ARG, "raycast: from or dir is NULL");
    if (!zoneId) return Fail(SCO_BAD_ARG, "raycast: zone 0; pass a zone id (teleport.spatial player_pose)");
    if (!Finite3(from) || !Finite3(dir)) return Fail(SCO_BAD_ARG, "raycast: from or dir isn't finite");
    if (!std::isfinite(maxDist) || maxDist <= 0) return Fail(SCO_BAD_ARG, "raycast: max_dist must be positive and finite");
    const double norm = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1] + dir[2] * dir[2]);
    if (!(norm > 0) || !std::isfinite(norm)) return Fail(SCO_BAD_ARG, "raycast: dir has no direction");
    const double len = maxDist > SC_WORLD_RAY_MAX_DISTANCE ? static_cast<double>(SC_WORLD_RAY_MAX_DISTANCE) : maxDist;

    switch (CheckRaySlots()) {
        case Slots::Ok: break;
        case Slots::NotSpawned: return Fail(SCO_NOT_FOUND, "raycast: %s", kNotSpawned);
        case Slots::Changed: return Fail(SCO_UNAVAILABLE, "raycast: the entity ray slots aren't the ones the rows found on this game build");
        case Slots::Fault: return Fail(SCO_FAILED, "raycast: fault while checking the entity ray slots");
    }
    uintptr_t zone = ZoneById(zoneId);
    if (!zone) return Fail(SCO_NOT_FOUND, "raycast: zone %llu isn't streamed in", static_cast<unsigned long long>(zoneId));

    double unit[3], end[3];
    for (int i = 0; i < 3; ++i) {
        unit[i] = dir[i] / norm;
        end[i] = from[i] + unit[i] * len;
    }
    const double rot0[4] = { 0, 0, 0, 1 };
    bool fault = false;
    for (int depth = 0; zone && depth < kMaxZones; ++depth) {
        ZoneLink link = {};
        if (!ReadZoneLink(zone, link)) break;
        if (!link.parent) break;   // the world's root zone isn't cast in (build.cpp RayHit)
        double o[3] = { from[0], from[1], from[2] }, e[3] = { end[0], end[1], end[2] };
        if (depth > 0) {   // the same ray in the parent's frame: a rigid move, so the length is the same
            double ro[4];
            if (!PoseToZone(zoneId, link.id, from, rot0, o, ro) || !PoseToZone(zoneId, link.id, end, rot0, e, ro)) break;
        }
        const double d[3] = { e[0] - o[0], e[1] - o[1], e[2] - o[2] };
        const double dl = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dl >= 0.01) {
            const double dist = CastInZone(zone, o, d, dl, fault);
            if (fault) return Fail(SCO_FAILED, "raycast: fault in the game's ray cast");
            if (dist >= 0) {
                for (int i = 0; i < 3; ++i) outHit->pos[i] = from[i] + unit[i] * dist;
                outHit->distance = dist;
                return SCO_OK;
            }
        }
        zone = link.parent;
    }
    return Fail(SCO_NOT_FOUND, "raycast: no hit within %.1f m", len);
}

sco_result SvcCamera(double outPos[3], double outRot[4], uint64_t* outZone) {
    if (outPos) memset(outPos, 0, 3 * sizeof(double));
    if (outRot) memset(outRot, 0, 4 * sizeof(double));
    if (outZone) *outZone = 0;
    if (!OnGameThread()) return Fail(SCO_WRONG_THREAD, "camera: game thread only");
    if (!outPos && !outRot && !outZone) return Fail(SCO_BAD_ARG, "camera: every out pointer is NULL");
    if (!g_started || !g_canCamera) return Unavailable("camera", kCapCamera, g_whyCamera);
    Camera c = {};
    if (const char* err = ReadCamera(c))
        return Fail(err == kNotSpawned || strcmp(err, kNotSpawned) == 0 ? SCO_NOT_FOUND : SCO_FAILED, "camera: %s", err);
    if (outPos) memcpy(outPos, c.pos, sizeof(c.pos));
    if (outRot) memcpy(outRot, c.rot, sizeof(c.rot));
    if (outZone) *outZone = c.zone;
    return SCO_OK;
}

sco_result SvcLastError(sco_plugin* self, char* out, uint32_t* inoutSize) {
    if (!inoutSize) return SCO_BAD_ARG;
    const uint32_t capacity = *inoutSize;
    *inoutSize = 0;
    if ((!out && capacity) || (self && !LivePlugin(self))) return SCO_BAD_ARG;
    char text[sizeof(g_error)];
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        memcpy(text, g_error, sizeof(text));
    }
    const size_t need = strnlen(text, sizeof(text) - 1) + 1;
    *inoutSize = static_cast<uint32_t>(need);
    if (capacity < need) return SCO_TOO_MANY;
    memcpy(out, text, need - 1);
    out[need - 1] = 0;
    return SCO_OK;
}

const sc_world_v1 kWorld = { sizeof(sc_world_v1), 0, SvcRaycast, SvcCamera, SvcLastError };

void LogCap(const char* cap, bool ready, const char* why) {
    if (!ready) Log("[game] %s not ready: %s; it answers SCO_UNAVAILABLE", cap, why);
}

}  // namespace

Result StartWorld() {
    if (g_started) return Result::Ok;
    Resolve();
    const Result r = host::ProvideGameService(SC_WORLD_NAME, SC_WORLD_VERSION_1_0, &kWorld);
    if (r != Result::Ok) {
        for (const char* cap : { kCapRay, kCapCamera }) caps::Set(cap, false, "game.world not published");
        g_game = {};
        g_canRay = g_canCamera = false;
        return r;
    }
    LogCap(kCapRay, g_canRay, g_whyRay);
    LogCap(kCapCamera, g_canCamera, g_whyCamera);
    g_started = true;
    return Result::Ok;
}

void StopWorld() {
    if (!g_started) return;
    host::WithdrawGameService(SC_WORLD_NAME);
    for (const char* cap : { kCapRay, kCapCamera }) caps::Set(cap, false, "stopped");
    {
        std::lock_guard<std::mutex> hold(g_errLock);
        g_error[0] = 0;
    }
    g_game = {};
    g_canRay = g_canCamera = false;
    g_started = false;
}

}  // namespace sco::game::services
