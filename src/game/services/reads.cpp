#include "sco/game/reads.h"
#include "sco/game/teleport.h"
#include "sco/scan.h"
#include <cmath>
#include <cstdio>
#include <cstring>

namespace sco::game::reads {

namespace {

constexpr uint64_t kPtrMask = 0xFFFFFFFFFFFFull;

TeleportAddrs g_addrs;
bool          g_ready = false;
int           g_slotsOk = -1;   // EntityRotation's check: -1 not run, 0 failed, 1 passed

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

template <typename R, typename... A> R VCall(uintptr_t obj, size_t off, A... a) {
    using Fn = R(__fastcall*)(uintptr_t, A...);
    return reinterpret_cast<Fn>(Rd<uintptr_t>(Rd<uintptr_t>(obj) + off))(obj, a...);
}

double Dot(const double a[3], const double b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// A zone's frame in the world, probed through LocalToWorld. False when the axes aren't
// orthonormal (a frame the services can't invert).
bool ZoneAxes(uintptr_t zone, double origin[3], double axis[3][3]) {
    constexpr double kArm = 1.0e6;
    const double zero[3] = {};
    LocalToWorld(zone, zero, origin);
    for (int i = 0; i < 3; ++i) {
        double probe[3] = {}, w[3];
        probe[i] = kArm;
        LocalToWorld(zone, probe, w);
        for (int k = 0; k < 3; ++k) axis[i][k] = (w[k] - origin[k]) / kArm;
    }
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(Dot(axis[i], axis[i]) - 1.0) > 1e-6) return false;
        for (int j = i + 1; j < 3; ++j)
            if (std::fabs(Dot(axis[i], axis[j])) > 1e-6) return false;
    }
    return true;
}

// The entity vtable's set-position, set-rotation and get-rotation functions, as in 4.10.196.
bool EntitySlotsOk(uintptr_t entity) {
    if (g_slotsOk < 0) {
        const uintptr_t vt = Rd<uintptr_t>(entity);
        g_slotsOk =
            BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2B0)), "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 56 41 57")
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2C0)), "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 70")
            && BytesMatch(reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(vt + 0x2C8)), "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 60 48 8B F9 41 0F B6 F0");
    }
    return g_slotsOk > 0;
}

}  // namespace

bool Init() {
    TeleportAddrs a;
    g_ready = TeleportAddresses(a);
    if (g_ready) g_addrs = a;
    return g_ready;
}

bool Ready() { return g_ready; }

bool LocalPlayer(uintptr_t& actor, uintptr_t& entity) {
    if (!g_ready) return false;
    const uintptr_t mgr = *g_addrs.clientMgr;
    if (!mgr) return false;
    const uintptr_t sub = Rd<uintptr_t>(mgr + 0xE0);
    if (!sub) return false;
    const uintptr_t info = VCall<uintptr_t>(sub, 0x2E0);
    if (!info) return false;
    uint64_t handle = 0;
    reinterpret_cast<void(__fastcall*)(uint64_t*, uint64_t)>(g_addrs.handleFromId)(&handle, Rd<uint64_t>(info + 8));
    actor = handle & kPtrMask;
    if (!actor) return false;
    const uintptr_t life = VCall<uintptr_t>(actor, 0xA08);
    if (!life || !VCall<uintptr_t>(life, 0x28)) return false;
    entity = Rd<uintptr_t>(actor + 8) & kPtrMask;
    return entity != 0;
}

uintptr_t   ZoneParent(uintptr_t zone) { return VCall<uintptr_t>(zone, 0x08); }
const char* ZoneName(uintptr_t zone) { return VCall<const char*>(zone, 0x218); }

uint64_t ZoneId(uintptr_t zone) {
    uint8_t tmp[16] = {};
    const uint64_t* id = VCall<const uint64_t*>(zone, 0x58, tmp);
    return id ? *id : 0;
}

uintptr_t EntityFromId(uint64_t entityId) {
    if (!g_ready || !entityId) return 0;
    const uintptr_t es = *g_addrs.entitySystem;
    return es ? VCall<uintptr_t>(es, 0x120, entityId) : 0;
}

uintptr_t ZoneFromId(uint64_t zoneId) {
    const uintptr_t ent = EntityFromId(zoneId);
    return ent ? VCall<uintptr_t>(ent, 0x6E0) : 0;
}

uintptr_t EntityZone(uintptr_t entity) { return VCall<uintptr_t>(entity, 0x6B8); }

void EntityLocalPos(uintptr_t entity, double out[3]) {
    double buf[4] = {};
    const double* r = VCall<const double*>(entity, 0x2B8, buf, uintptr_t(0));
    out[0] = r[0]; out[1] = r[1]; out[2] = r[2];
}

bool EntityRotation(uintptr_t entity, double rot[4]) {
    if (!EntitySlotsOk(entity)) return false;
    double buf[4] = {};
    const double* r = VCall<const double*>(entity, 0x2C8, buf, static_cast<uint8_t>(0));
    if (!r) return false;
    memcpy(rot, r, 4 * sizeof(double));
    return true;
}

void LocalToWorld(uintptr_t zone, const double local[3], double world[3]) {
    double buf[4] = {};
    const double* r = VCall<const double*>(zone, 0x198, buf, local);
    world[0] = r[0]; world[1] = r[1]; world[2] = r[2];
}

int ReadZoneChain(uintptr_t zone, ZoneFrame* out, int max) {
    int n = 0;
    // steps bounds the walk on a parent cycle, since skipped zones don't count towards max.
    int steps = 0;
    for (uintptr_t z = zone; z && n < max && steps < 64; z = ZoneParent(z), ++steps) {
        const uint64_t id = ZoneId(z);
        if (!id || ZoneFromId(id) != z) continue;   // can't be named by id, so can't be queried
        ZoneFrame& f = out[n];
        if (!ZoneAxes(z, f.origin, f.axis)) continue;
        f.id = id;
        const char* name = ZoneName(z);
        snprintf(f.name, sizeof(f.name), "%s", name ? name : "");
        ++n;
    }
    return n;
}

}  // namespace sco::game::reads
