// Unit tests for the engine-agnostic spatial math (sco/engine/types.h) and the zone tree
// (sco/engine/zone.h). Pure math, no game. The concurrency test runs under ThreadSanitizer.
//   tools/test.sh
#include "sco/engine/types.h"
#include "sco/engine/zone.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

using sco::engine::Quatd;
using sco::engine::Transform;
using sco::engine::Vector3d;
using sco::engine::Zone;
using sco::engine::ZoneTree;

static const double kPi = std::acos(-1.0);

static bool Near(double a, double b, double eps = 1e-12) { return std::fabs(a - b) <= eps; }
static bool Near(const Vector3d& a, const Vector3d& b, double eps = 1e-12) {
    return Near(a.x, b.x, eps) && Near(a.y, b.y, eps) && Near(a.z, b.z, eps);
}

// ---- Vector3d -------------------------------------------------------------------------------

static void TestVector() {
    constexpr Vector3d a{ 1, 2, 3 }, b{ 4, -5, 6 };
    static_assert((a + b).x == 5 && (a - b).y == 7 && (a * 2.0).z == 6 && (2.0 * a).y == 4);
    static_assert(a.Dot(b) == 1 * 4 + 2 * -5 + 3 * 6);
    CHECK(Near(a + b, { 5, -3, 9 }));
    CHECK(Near(a - b, { -3, 7, -3 }));
    CHECK(Near(-a, { -1, -2, -3 }));
    CHECK(Near(a / 2.0, { 0.5, 1, 1.5 }));
    CHECK(Near(Vector3d{ 1, 0, 0 }.Cross({ 0, 1, 0 }), { 0, 0, 1 }));
    CHECK(Near(a.Cross(b), { 2 * 6 - 3 * -5, 3 * 4 - 1 * 6, 1 * -5 - 2 * 4 }));
    CHECK(Near(a.Cross(b).Dot(a), 0) && Near(a.Cross(b).Dot(b), 0));
    CHECK(Near(Vector3d{ 3, 4, 12 }.LengthSquared(), 169));
    CHECK(Near(Vector3d{ 3, 4, 12 }.Length(), 13));
    CHECK(Near(Vector3d{ 0, 0, -7 }.Normalized(), { 0, 0, -1 }));
    CHECK(Near(Vector3d{ 3, 4, 0 }.Normalized().Length(), 1));
    CHECK(Near(Vector3d{ 1e-13, 0, 0 }.Normalized(), { 0, 0, 0 }, 0));
    CHECK(Near(Vector3d{ 0, 0, 0 }.Normalized(), { 0, 0, 0 }, 0));
    CHECK(Near(a.DistanceTo({ 1, 2, 8 }), 5));
}

// ---- Quatd ----------------------------------------------------------------------------------

static void TestQuaternion() {
    const double r90 = kPi / 2;
    const Quatd qx = Quatd::FromAxisAngle({ 1, 0, 0 }, r90);
    const Quatd qy = Quatd::FromAxisAngle({ 0, 1, 0 }, r90);
    const Quatd qz = Quatd::FromAxisAngle({ 0, 0, 5 }, r90);   // axis normalized
    // Right-handed: x: y -> z, y: z -> x, z: x -> y.
    CHECK(Near(qx.Rotate({ 0, 1, 0 }), { 0, 0, 1 }));
    CHECK(Near(qx.Rotate({ 0, 0, 1 }), { 0, -1, 0 }));
    CHECK(Near(qx.Rotate({ 1, 0, 0 }), { 1, 0, 0 }));
    CHECK(Near(qy.Rotate({ 0, 0, 1 }), { 1, 0, 0 }));
    CHECK(Near(qy.Rotate({ 1, 0, 0 }), { 0, 0, -1 }));
    CHECK(Near(qz.Rotate({ 1, 0, 0 }), { 0, 1, 0 }));
    CHECK(Near(qz.Rotate({ 0, 1, 0 }), { -1, 0, 0 }));
    CHECK(Near(qz.Unrotate({ 0, 1, 0 }), { 1, 0, 0 }));
    // Composition: (a * b) applies b first.
    const Vector3d v{ 1, 2, 3 };
    CHECK(Near((qy * qx).Rotate(v), qy.Rotate(qx.Rotate(v))));
    CHECK(Near((qz * qy * qx).Rotate(v), qz.Rotate(qy.Rotate(qx.Rotate(v)))));
    CHECK(Near((qx * qy).Rotate({ 0, 0, 1 }), { 1, 0, 0 }));    // y: z -> x, then x leaves x
    CHECK(Near((qy * qx).Rotate({ 0, 1, 0 }), { 1, 0, 0 }));    // x: y -> z, then y: z -> x
    CHECK(Near((qz * qz).Rotate({ 1, 0, 0 }), { -1, 0, 0 }));   // 180 degrees
    // Four quarter turns are a full turn.
    const Quatd full = qz * qz * qz * qz;
    CHECK(Near(full.Rotate(v), v));
    // Conjugate undoes, identity does nothing.
    CHECK(Near((qx.Conjugate() * qx).Rotate(v), v));
    CHECK(Near(Quatd::Identity().Rotate(v), v, 0));
    CHECK(Near(Quatd{}.Rotate(v), v, 0));
    CHECK(Near(Quatd::FromAxisAngle({ 0, 0, 0 }, 1.0).Rotate(v), v, 0));
    // Normalized.
    const Quatd n = Quatd{ 2, 0, 0, 2 }.Normalized();
    CHECK(Near(n.w, std::sqrt(0.5)) && Near(n.z, std::sqrt(0.5)));
    CHECK(Near(n.Rotate({ 1, 0, 0 }), { 0, 1, 0 }));
    const Quatd zero = Quatd{ 0, 0, 0, 0 }.Normalized();
    CHECK(zero.w == 1 && zero.x == 0 && zero.y == 0 && zero.z == 0);
    // The game's (x, y, z, w) order.
    const double game[4] = { 0.1, 0.2, 0.3, 0.9 };
    const Quatd g = sco::engine::FromXYZW(game);
    CHECK(g.w == 0.9 && g.x == 0.1 && g.y == 0.2 && g.z == 0.3);
    double back[4] = { 0, 0, 0, 0 };
    sco::engine::ToXYZW(g, back);
    CHECK(back[0] == 0.1 && back[1] == 0.2 && back[2] == 0.3 && back[3] == 0.9);
}

// ---- Transform ------------------------------------------------------------------------------

static void TestTransform() {
    const Transform t{ { 10, -20, 30 }, Quatd::FromAxisAngle({ 1, 1, 1 }, 1.0), 2.5 };
    const Vector3d p{ 3, -4, 5 };
    CHECK(Near(t.InverseTransformPoint(t.TransformPoint(p)), p, 1e-12));
    CHECK(Near(t.TransformPoint(t.InverseTransformPoint(p)), p, 1e-12));
    // Scale, then rotation, then position.
    const Transform s{ { 1, 2, 3 }, Quatd::FromAxisAngle({ 0, 0, 1 }, kPi / 2), 2 };
    CHECK(Near(s.TransformPoint({ 1, 0, 0 }), { 1, 4, 3 }));
    CHECK(Near(Transform{}.TransformPoint(p), p, 0));
    // Composition: a * b is b's frame expressed in a's parent.
    const Transform u{ { -7, 0.5, 2 }, Quatd::FromAxisAngle({ 0, 1, 0 }, -0.3), 0.25 };
    CHECK(Near((t * u).TransformPoint(p), t.TransformPoint(u.TransformPoint(p)), 1e-12));
    CHECK(Near((t * u).InverseTransformPoint(p), u.InverseTransformPoint(t.InverseTransformPoint(p)), 1e-12));
    CHECK(Near((t * u * s).TransformPoint(p), t.TransformPoint(u.TransformPoint(s.TransformPoint(p))), 1e-12));
}

// ---- ZoneTree -------------------------------------------------------------------------------

enum : uint64_t { kSystem = 1, kPlanet = 2, kCity = 3, kShip = 4, kBridge = 5, kShip2 = 6 };

static void BuildStanton(ZoneTree& z, double planetX) {
    const Quatd none = Quatd::Identity();
    CHECK(z.Set(kSystem, 0, "Stanton", { { 0, 0, 0 }, none, 1 }));
    CHECK(z.Set(kPlanet, kSystem, "Hurston", { { planetX, 0, 0 }, none, 1 }));
    CHECK(z.Set(kCity, kPlanet, "Lorville", { { 0, 0, 1e6 }, none, 1 }));
    CHECK(z.Set(kShip, kCity, "Cutlass", { { 100, 200, 10 }, Quatd::FromAxisAngle({ 0, 0, 1 }, kPi / 2), 1 }));
    CHECK(z.Set(kBridge, kShip, "bridge", { { 0, 30, 5 }, none, 1 }));
    CHECK(z.Set(kShip2, kCity, "Aurora", { { 1000, 0, 0 }, none, 1 }));
}

static void TestZoneChain() {
    ZoneTree z;
    BuildStanton(z, 5e6);
    CHECK(z.Size() == 6 && z.Has(kBridge) && !z.Has(99));
    Zone found;
    CHECK(z.Find(kShip, &found) && found.id == kShip && found.parentId == kCity && found.name == "Cutlass");
    CHECK(!z.Find(99, &found) && found.id == kShip);
    CHECK(!z.Find(kShip, nullptr));

    // bridge (1,2,3) -> ship (1,32,8) -> rotated 90 about z (-32,1,8) -> city (68,201,18)
    // -> planet (68,201,1000018) -> system (5000068,201,1000018).
    const Vector3d local{ 1, 2, 3 };
    Vector3d world, back;
    CHECK(z.LocalToWorld(kBridge, local, &world));
    CHECK(Near(world, { 5000068, 201, 1000018 }, 1e-6));
    CHECK(z.WorldToLocal(kBridge, world, &back));
    CHECK(Near(back, local, 1e-6));
    Vector3d inCity;
    CHECK(z.Transform(kBridge, kCity, local, &inCity) && Near(inCity, { 68, 201, 18 }, 1e-9));
    // World frame (0) as either end, and a zone to itself.
    Vector3d w2;
    CHECK(z.Transform(kBridge, 0, local, &w2) && Near(w2, world, 0));
    CHECK(z.Transform(0, 0, local, &w2) && Near(w2, local, 0));
    CHECK(z.Transform(kBridge, kBridge, local, &w2) && Near(w2, local, 0));
    // Sibling zones: the bridge origin seen from the other ship in the same city.
    // bridge origin -> ship (0,30,5) -> city (70,200,15) -> Aurora (-930,200,15).
    Vector3d sib;
    CHECK(z.Transform(kBridge, kShip2, { 0, 0, 0 }, &sib) && Near(sib, { -930, 200, 15 }, 1e-9));
    CHECK(z.Transform(kShip2, kBridge, sib, &back) && Near(back, { 0, 0, 0 }, 1e-9));

    // Stanton scale: the planet at 1e11 m. A world coordinate there has a resolution of about
    // 1.5e-5 m (one double ulp), so world round trips hold to that; zone-to-zone transforms
    // under the planet stop at the common ancestor and keep full precision.
    BuildStanton(z, 1e11);   // updates in place
    CHECK(z.Size() == 6);
    CHECK(z.LocalToWorld(kBridge, local, &world));
    CHECK(Near(world, { 1e11 + 68, 201, 1000018 }, 1e-4));
    CHECK(z.WorldToLocal(kBridge, world, &back) && Near(back, local, 1e-4));
    CHECK(z.Transform(kBridge, kShip2, { 0, 0, 0 }, &sib) && Near(sib, { -930, 200, 15 }, 1e-6));
    CHECK(z.Transform(kShip2, kBridge, sib, &back) && Near(back, { 0, 0, 0 }, 1e-6));
    CHECK(z.Transform(kBridge, kPlanet, local, &inCity) && Near(inCity, { 68, 201, 1000018 }, 1e-6));
    CHECK(z.Transform(kPlanet, kBridge, inCity, &back) && Near(back, local, 1e-6));
}

static void TestZoneFailures() {
    ZoneTree z;
    BuildStanton(z, 5e6);
    Vector3d out{ 7, 7, 7 };
    const Vector3d untouched{ 7, 7, 7 };
    // Missing zone, and a null out.
    CHECK(!z.LocalToWorld(99, { 1, 2, 3 }, &out) && Near(out, untouched, 0));
    CHECK(!z.WorldToLocal(99, { 1, 2, 3 }, &out) && Near(out, untouched, 0));
    CHECK(!z.Transform(kBridge, 99, { 1, 2, 3 }, &out) && Near(out, untouched, 0));
    CHECK(!z.LocalToWorld(kBridge, { 1, 2, 3 }, nullptr));
    // Bad Set calls change nothing.
    CHECK(!z.Set(0, kSystem, "zero", {}));
    CHECK(!z.Set(7, 7, "self", {}));
    CHECK(z.Size() == 6 && !z.Has(7));
    // A missing ancestor: removing the city strands the ships until it comes back.
    CHECK(z.Remove(kCity) && !z.Remove(kCity) && !z.Has(kCity) && z.Has(kShip));
    CHECK(!z.LocalToWorld(kBridge, { 1, 2, 3 }, &out) && Near(out, untouched, 0));
    CHECK(!z.Transform(kBridge, kShip2, { 1, 2, 3 }, &out));
    CHECK(z.LocalToWorld(kPlanet, { 0, 0, 0 }, &out) && Near(out, { 5e6, 0, 0 }, 0));
    CHECK(z.Set(kCity, kPlanet, "Lorville", { { 0, 0, 1e6 }, Quatd::Identity(), 1 }));
    CHECK(z.LocalToWorld(kBridge, { 1, 2, 3 }, &out) && Near(out, { 5000068, 201, 1000018 }, 1e-6));
    // A parent set before it exists.
    CHECK(z.Set(50, 51, "orphan", {}));
    CHECK(!z.LocalToWorld(50, { 0, 0, 0 }, &out));
    CHECK(z.Set(51, 0, "late root", { { 1, 0, 0 }, Quatd::Identity(), 1 }));
    CHECK(z.LocalToWorld(50, { 0, 0, 0 }, &out) && Near(out, { 1, 0, 0 }, 0));

    // A cycle: a -> b -> a.
    CHECK(z.Set(100, 101, "a", {}) && z.Set(101, 100, "b", {}));
    out = untouched;
    CHECK(!z.LocalToWorld(100, { 0, 0, 0 }, &out) && Near(out, untouched, 0));
    CHECK(!z.WorldToLocal(101, { 0, 0, 0 }, &out));
    CHECK(!z.Transform(100, kBridge, { 0, 0, 0 }, &out));

    // Depth: a chain of kMaxDepth zones works, one more fails.
    ZoneTree deep;
    const uint64_t base = 1000;
    for (uint64_t i = 0; i <= static_cast<uint64_t>(ZoneTree::kMaxDepth); ++i)
        CHECK(deep.Set(base + i, i == 0 ? 0 : base + i - 1, "level", { { 1, 0, 0 }, Quatd::Identity(), 1 }));
    const uint64_t last = base + ZoneTree::kMaxDepth - 1;   // chain of exactly kMaxDepth zones
    CHECK(deep.LocalToWorld(last, { 0, 0, 0 }, &out) && Near(out, { 32, 0, 0 }, 0));
    CHECK(!deep.LocalToWorld(last + 1, { 0, 0, 0 }, &out));
    CHECK(!deep.Transform(last + 1, base, { 0, 0, 0 }, &out));

    z.Clear();
    CHECK(z.Size() == 0 && !z.Has(kSystem));
    CHECK(!z.LocalToWorld(kBridge, { 0, 0, 0 }, &out));
}

// Four readers query while one writer keeps moving the ship (run under ThreadSanitizer). The ship
// only turns about z and keeps z = 10, so a bridge point's city z stays 18 whatever the writer did.
static void TestConcurrentReaders() {
    ZoneTree z;
    BuildStanton(z, 5e6);
    std::atomic<bool> stop{ false };
    std::atomic<int> reads{ 0 }, bad{ 0 };
    std::vector<std::thread> readers;
    for (int r = 0; r < 4; ++r) {
        readers.emplace_back([&] {
            Vector3d p, q;
            Zone copy;
            while (!stop.load()) {
                if (!z.Transform(kBridge, kCity, { 1, 2, 3 }, &p) || !Near(p.z, 18, 1e-9) ||
                    !z.LocalToWorld(kBridge, { 1, 2, 3 }, &q) || !Near(q.z, 1000018, 1e-6) ||
                    !z.Find(kShip, &copy) || copy.name != "Cutlass")
                    bad.fetch_add(1);
                reads.fetch_add(1);
            }
        });
    }
    while (reads.load() < 4) std::this_thread::yield();   // readers are running before the writer starts
    for (int i = 0; i < 2000; ++i) {
        const double angle = i * 0.01;
        z.Set(kShip, kCity, "Cutlass", { { 100.0 + i, 200, 10 }, Quatd::FromAxisAngle({ 0, 0, 1 }, angle), 1 });
        z.Set(200 + static_cast<uint64_t>(i % 8), kCity, "debris", {});   // inserts rehash the map
    }
    stop.store(true);
    for (auto& t : readers) t.join();
    CHECK(bad.load() == 0);
    CHECK(reads.load() >= 4);
}

int main() {
    TestVector();
    TestQuaternion();
    TestTransform();
    TestZoneChain();
    TestZoneFailures();
    TestConcurrentReaders();
    std::printf("sco-core spatial tests: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
