// teleport.spatial 1.0 (sc_spatial.h) from the game pack, and the game services' start and stop.
// Moved from sc-offline's teleport built-in (src/builtins/teleport_plugin.cpp): a tick
// subscription feeds a sco::engine::ZoneTree with your zone chain, and the conversions read
// through that tree. The tree holds ids and transforms only, never a game pointer, and is rebuilt
// every tick; a zone queried that isn't in it yet is read on the spot.
#include "sco/game/services.h"
#include "sco/engine/zone.h"
#include "sco/game/reads.h"
#include "sco/host.h"
#include "sco/log.h"
#include <sc_spatial.h>
#include <cmath>
#include <cstring>
#include <windows.h>

namespace sco::game::services {

namespace {

using engine::Quatd;
using engine::Transform;
using engine::Vector3d;
using reads::ZoneFrame;

bool             g_started = false;
engine::ZoneTree g_zones;          // cleared and refilled every tick; ids and transforms only
constexpr int    kChainMax = 16;   // zones per chain; the game's run about 6 deep

// Game reads under SEH, into plain structs. No C++ object with a destructor lives in these
// frames (MSVC C2712); the tree is fed outside them.
int ChainOfZoneId(uint64_t zoneId, ZoneFrame* out) {
    __try {
        const uintptr_t zone = reads::ZoneFromId(zoneId);
        return zone ? reads::ReadZoneChain(zone, out, kChainMax) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

int ChainOfPlayer(ZoneFrame* out) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return 0;
        const uintptr_t zone = reads::EntityZone(entity);
        return zone ? reads::ReadZoneChain(zone, out, kChainMax) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

struct Pose { double pos[3]; double rot[4]; uint64_t zone; };

bool ReadPlayerPose(Pose& p) {
    __try {
        uintptr_t actor, entity;
        if (!reads::LocalPlayer(actor, entity)) return false;
        const uintptr_t zone = reads::EntityZone(entity);
        if (!zone) return false;
        p.zone = reads::ZoneId(zone);
        if (!p.zone || reads::ZoneFromId(p.zone) != zone) return false;
        reads::EntityLocalPos(entity, p.pos);
        return reads::EntityRotation(entity, p.rot);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

uint64_t ZoneIdOfEntity(uint64_t entityId) {
    __try {
        const uintptr_t ent = reads::EntityFromId(entityId);
        const uintptr_t zone = ent ? reads::EntityZone(ent) : 0;
        if (!zone) return 0;
        const uint64_t id = reads::ZoneId(zone);
        return id && reads::ZoneFromId(id) == zone ? id : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// The rotation whose matrix has columns a[0], a[1], a[2] (the world directions of a zone's axes).
Quatd FromAxes(const double a[3][3]) {
    const double m00 = a[0][0], m11 = a[1][1], m22 = a[2][2];
    const double m01 = a[1][0], m10 = a[0][1], m02 = a[2][0], m20 = a[0][2], m12 = a[2][1], m21 = a[1][2];
    const double t = m00 + m11 + m22;
    Quatd q;
    if (t > 0) {
        const double s = std::sqrt(t + 1.0) * 2.0;
        q = { 0.25 * s, (m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s };
    } else if (m00 > m11 && m00 > m22) {
        const double s = std::sqrt(1.0 + m00 - m11 - m22) * 2.0;
        q = { (m21 - m12) / s, 0.25 * s, (m01 + m10) / s, (m02 + m20) / s };
    } else if (m11 > m22) {
        const double s = std::sqrt(1.0 + m11 - m00 - m22) * 2.0;
        q = { (m02 - m20) / s, (m01 + m10) / s, 0.25 * s, (m12 + m21) / s };
    } else {
        const double s = std::sqrt(1.0 + m22 - m00 - m11) * 2.0;
        q = { (m10 - m01) / s, (m02 + m20) / s, (m12 + m21) / s, 0.25 * s };
    }
    return q.Normalized();
}

bool RightHanded(const double a[3][3]) {
    const Vector3d x{ a[0][0], a[0][1], a[0][2] }, y{ a[1][0], a[1][1], a[1][2] }, z{ a[2][0], a[2][1], a[2][2] };
    return x.Cross(y).Dot(z) > 0;
}

Transform WorldFrame(const ZoneFrame& f) {
    return { { f.origin[0], f.origin[1], f.origin[2] }, FromAxes(f.axis), 1.0 };
}

// Puts a chain (innermost first) into the tree, each zone relative to the next one up; the
// outermost is a root zone with its world frame. A mirrored frame (no rotation can express it)
// cuts the chain below it: the zones inside it still go in, the outermost of them as a root.
void Feed(const ZoneFrame* f, int n) {
    for (int i = 0; i < n; ++i)
        if (!RightHanded(f[i].axis)) { n = i; break; }
    for (int i = 0; i < n; ++i) {
        const Transform world = WorldFrame(f[i]);
        if (i + 1 < n) {
            const Transform parent = WorldFrame(f[i + 1]);
            const Transform local = { parent.InverseTransformPoint(world.position),
                                      (parent.rotation.Conjugate() * world.rotation).Normalized(), 1.0 };
            g_zones.Set(f[i].id, f[i + 1].id, f[i].name, local);
        } else {
            g_zones.Set(f[i].id, 0, f[i].name, world);
        }
    }
}

void RefreshZones() {
    g_zones.Clear();
    if (!reads::Ready()) return;
    ZoneFrame chain[kChainMax];
    Feed(chain, ChainOfPlayer(chain));
}

bool Ready() { return g_started && reads::Ready() && OnGameThread(); }

// The zone is in the tree, read this tick: the player's chain from the tick, or its own chain
// read now. 0, the world, always is.
bool EnsureZone(uint64_t zoneId) {
    if (!zoneId || g_zones.Has(zoneId)) return true;
    ZoneFrame chain[kChainMax];
    Feed(chain, ChainOfZoneId(zoneId, chain));
    return g_zones.Has(zoneId);
}

Vector3d V(const double p[3]) { return { p[0], p[1], p[2] }; }
void     Out(const Vector3d& v, double p[3]) { p[0] = v.x; p[1] = v.y; p[2] = v.z; }

// The rotation of zone id's frame in the world: its parents' rotations composed down to it.
bool WorldRotation(uint64_t zoneId, Quatd* out) {
    Quatd q = Quatd::Identity();
    for (int depth = 0; zoneId; ++depth) {
        engine::Zone z;
        if (depth >= engine::ZoneTree::kMaxDepth || !g_zones.Find(zoneId, &z)) return false;
        q = z.local.rotation * q;
        zoneId = z.parentId;
    }
    *out = q.Normalized();
    return true;
}

int SvcPlayerPose(double pos[3], double rot[4], uint64_t* zoneId) {
    if (!Ready() || !pos || !rot || !zoneId) return 0;
    Pose p = {};
    if (!ReadPlayerPose(p)) return 0;
    memcpy(pos, p.pos, sizeof(p.pos));
    memcpy(rot, p.rot, sizeof(p.rot));
    *zoneId = p.zone;
    return 1;
}

int SvcZoneOfEntity(uint64_t entityId, uint64_t* zoneId) {
    if (!Ready() || !zoneId) return 0;
    const uint64_t id = ZoneIdOfEntity(entityId);
    if (!id || !EnsureZone(id)) return 0;
    *zoneId = id;
    return 1;
}

int SvcLocalToWorld(uint64_t zoneId, const double local[3], double world[3]) {
    if (!Ready() || !local || !world || !EnsureZone(zoneId)) return 0;
    Vector3d w;
    if (!g_zones.LocalToWorld(zoneId, V(local), &w)) return 0;
    Out(w, world);
    return 1;
}

int SvcWorldToLocal(uint64_t zoneId, const double world[3], double local[3]) {
    if (!Ready() || !world || !local || !EnsureZone(zoneId)) return 0;
    Vector3d l;
    if (!g_zones.WorldToLocal(zoneId, V(world), &l)) return 0;
    Out(l, local);
    return 1;
}

int SvcZoneToZone(uint64_t from, uint64_t to, const double in[3], double out[3]) {
    if (!Ready() || !in || !out || !EnsureZone(from) || !EnsureZone(to)) return 0;
    Vector3d p;
    if (!g_zones.Transform(from, to, V(in), &p)) return 0;
    Out(p, out);
    return 1;
}

int SvcZoneName(uint64_t zoneId, char* out, uint32_t cap) {
    if (!Ready() || !out || !cap || !zoneId || !EnsureZone(zoneId)) return 0;
    engine::Zone z;
    if (!g_zones.Find(zoneId, &z)) return 0;
    const size_t n = z.name.size() < cap - 1 ? z.name.size() : cap - 1;
    memcpy(out, z.name.data(), n);
    out[n] = 0;
    return 1;
}

const sc_spatial_v1 kSpatial = {
    sizeof(sc_spatial_v1), SvcPlayerPose, SvcZoneOfEntity, SvcLocalToWorld, SvcWorldToLocal, SvcZoneToZone, SvcZoneName,
};

void OnTick(const char*, const void*, void*) { RefreshZones(); }

}  // namespace

Result Start() {
    if (g_started) return Result::Ok;
    if (!reads::Init()) Log("[game] teleport.* rows not OK: teleport.spatial answers 0 (see the [core] lines)");
    Result r = host::ProvideGameService(SC_SPATIAL_SERVICE_NAME, SC_SPATIAL_SERVICE_VERSION, &kSpatial);
    if (r != Result::Ok) return r;
    r = Subscribe(host::GameOwner(), "tick", OnTick, nullptr);
    if (r != Result::Ok) {
        host::WithdrawGameService(SC_SPATIAL_SERVICE_NAME);
        return r;
    }
    g_started = true;
    Log("[game] game services published: %s 1.0", SC_SPATIAL_SERVICE_NAME);
    return Result::Ok;
}

void Stop() {
    if (!g_started) return;
    Unsubscribe(host::GameOwner(), "tick", OnTick);
    host::WithdrawGameService(SC_SPATIAL_SERVICE_NAME);
    g_zones.Clear();
    g_started = false;
}

bool Started() { return g_started; }

uint64_t ZoneOfEntity(uint64_t entityId) {
    if (!Ready()) return 0;
    const uint64_t id = ZoneIdOfEntity(entityId);
    return id && EnsureZone(id) ? id : 0;
}

bool PoseToZone(uint64_t from, uint64_t to, const double pos[3], const double rot[4], double outPos[3],
                double outRot[4]) {
    if (!Ready() || !EnsureZone(from) || !EnsureZone(to)) return false;
    Vector3d p;
    Quatd rf, rt;
    if (!g_zones.Transform(from, to, V(pos), &p) || !WorldRotation(from, &rf) || !WorldRotation(to, &rt)) return false;
    Out(p, outPos);
    engine::ToXYZW((rt.Conjugate() * rf * engine::FromXYZW(rot)).Normalized(), outRot);
    return true;
}

}  // namespace sco::game::services
