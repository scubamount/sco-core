// SPDX-License-Identifier: GPL-3.0-only
// game.inventory 1.1's portable part (src/game/services/inventory_walk.cpp, inventory.cpp) against
// synthetic container trees: the walk's order, the 128-port cap and its TRUNCATED signal, the
// depth, cycle and budget limits, empty ports, guard failures, the adapter-side generation and
// the snapshot service's size gating. Synthetic ids only; no game, nothing here is in-game evidence.
#include "../src/game/services/inventory.h"
#include "../src/game/services/inventory_walk.h"
#include <sc_inventory.h>
#include <cstdio>
#include <cstring>
#include <map>
#include <utility>
#include <vector>

using namespace sco::game::inventory;
using sco::game::services::InventoryBackend;
using sco::game::services::SnapshotWith;

namespace {

int passed = 0, failed = 0;
#define CHECK(x) do { if (x) ++passed; else { ++failed; std::printf("FAIL line %d: %s\n", __LINE__, #x); } } while (0)

// A synthetic container tree. A container is an id; a port is {valid, type, entity, inner}.
struct Tree {
    std::map<uintptr_t, std::vector<PortInfo>> containers;
    std::map<uintptr_t, bool> badContainer;   // containerPorts fails
    std::map<uintptr_t, uint32_t> badPortAt;  // readPort fails at this index (+1; 0 = never)
};

PortInfo Item(uint64_t type, uint64_t entity, uintptr_t inner = 0) { PortInfo p; p.valid = true; p.itemType = type; p.entityId = entity; p.inner = inner; return p; }
PortInfo Empty() { return PortInfo(); }

bool ContainerPorts(void* ctx, uintptr_t container, uintptr_t* ports, uint32_t* count) {
    const Tree& t = *static_cast<const Tree*>(ctx);
    auto bad = t.badContainer.find(container);
    if (bad != t.badContainer.end() && bad->second) return false;
    auto it = t.containers.find(container);
    if (it == t.containers.end()) return false;
    *ports = container;   // the "ports pointer" is the container id; readPort indexes by it
    *count = static_cast<uint32_t>(it->second.size());
    return true;
}

bool ReadPort(void* ctx, uintptr_t ports, uint32_t index, PortInfo* out) {
    const Tree& t = *static_cast<const Tree*>(ctx);
    auto bad = t.badPortAt.find(ports);
    if (bad != t.badPortAt.end() && bad->second == index + 1) return false;
    *out = t.containers.at(ports)[index];
    return true;
}

Access AccessOf(Tree& t) {
    Access a;
    a.ctx = &t;
    a.containerPorts = ContainerPorts;
    a.readPort = ReadPort;
    return a;
}

struct Out {
    sc_inventory_port ports[SC_INVENTORY_MAX_PORTS];
    WalkResult r;
    bool ok;
};

Out Run(Tree& t, uintptr_t root, uint32_t capacity = SC_INVENTORY_MAX_PORTS) {
    Out o{};
    memset(o.ports, 0xCD, sizeof(o.ports));
    o.ok = Walk(AccessOf(t), root, capacity, o.ports, o.r);
    return o;
}

void TestFlat() {
    Tree t;
    t.containers[1] = { Item(22, 1001), Empty(), Item(95, 1003) };
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == 3 && o.r.totalSeen == 3 && o.r.flags == 0);
    CHECK(o.ports[0].size == sizeof(sc_inventory_port) && o.ports[0].valid == 1 && o.ports[0].item_class_id == 22 && o.ports[0].entity_id == 1001);
    // An empty port: valid 0 and the fields read as 0.
    CHECK(o.ports[1].valid == 0 && o.ports[1].item_class_id == 0 && o.ports[1].entity_id == 0 && o.ports[1].size == sizeof(sc_inventory_port));
    CHECK(o.ports[2].valid == 1 && o.ports[2].item_class_id == 95 && o.ports[2].entity_id == 1003);
}

void TestEmptyContainer() {
    Tree t;
    t.containers[1] = {};
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == 0 && o.r.totalSeen == 0 && o.r.flags == 0);
}

// Depth first, each port followed by the ports of the container attached to it.
void TestNestedOrder() {
    Tree t;
    t.containers[1] = { Item(26, 101, 2), Empty(), Item(24, 103, 3) };   // A (backpack), empty, C (torso)
    t.containers[2] = { Item(85, 201), Item(54, 202, 4) };               // D, E (a container)
    t.containers[3] = { Item(88, 301) };                                 // F
    t.containers[4] = { Item(81, 401) };                                 // G
    Out o = Run(t, 1);
    const uint64_t want[] = { 101, 201, 202, 401, 0, 103, 301 };         // A D E G (empty) C F
    CHECK(o.ok && o.r.count == 7 && o.r.totalSeen == 7 && o.r.flags == 0);
    for (uint32_t i = 0; i < 7 && i < o.r.count; ++i) CHECK(o.ports[i].entity_id == want[i]);
    CHECK(o.ports[4].valid == 0);
}

void TestCycle() {
    Tree t;
    t.containers[1] = { Item(26, 101, 2), Item(24, 102) };
    t.containers[2] = { Item(54, 201, 1) };           // attaches the root again
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == 3 && o.r.totalSeen == 3);
    CHECK((o.r.flags & SC_INVENTORY_FLAG_LIMITED) && !(o.r.flags & SC_INVENTORY_FLAG_TRUNCATED));
    // A container attached to itself, and the same container attached twice: entered once.
    Tree u;
    u.containers[1] = { Item(26, 101, 1), Item(26, 102, 2), Item(26, 103, 2) };
    u.containers[2] = { Item(85, 201) };
    Out p = Run(u, 1);
    CHECK(p.ok && p.r.count == 4 && p.r.totalSeen == 4 && (p.r.flags & SC_INVENTORY_FLAG_LIMITED));
}

void TestDepthLimit() {
    Tree t;
    for (uintptr_t c = 1; c <= 20; ++c) t.containers[c] = { Item(54, 1000 + c, c + 1) };
    t.containers[21] = {};
    Out o = Run(t, 1);
    // The player's container plus kMaxDepth nested levels are read; deeper ones are not.
    CHECK(o.ok && o.r.count == kMaxDepth + 1 && o.r.totalSeen == kMaxDepth + 1);
    CHECK(o.r.flags == SC_INVENTORY_FLAG_LIMITED);
    CHECK(o.ports[kMaxDepth].entity_id == 1000 + kMaxDepth + 1);
}

void TestCapAndTruncation() {
    Tree t;
    t.containers[1].assign(200, Empty());
    for (uint32_t i = 0; i < 200; ++i) t.containers[1][i] = Item(85, 5000 + i);
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == SC_INVENTORY_MAX_PORTS && o.r.totalSeen == 200 && o.r.flags == SC_INVENTORY_FLAG_TRUNCATED);
    CHECK(o.ports[127].entity_id == 5127);
    // A smaller capacity, zero capacity, and exactly the cap.
    Out five = Run(t, 1, 5);
    CHECK(five.ok && five.r.count == 5 && five.r.totalSeen == 200 && five.r.flags == SC_INVENTORY_FLAG_TRUNCATED);
    Out zero = Run(t, 1, 0);
    CHECK(zero.ok && zero.r.count == 0 && zero.r.totalSeen == 200 && zero.r.flags == SC_INVENTORY_FLAG_TRUNCATED);
    Tree exact;
    exact.containers[1].assign(128, Empty());
    Out e = Run(exact, 1);
    CHECK(e.ok && e.r.count == 128 && e.r.totalSeen == 128 && e.r.flags == 0);
    // A capacity above the ABI maximum is clamped to it.
    Out big = Run(t, 1, 100000);
    CHECK(big.ok && big.r.count == SC_INVENTORY_MAX_PORTS && (big.r.flags & SC_INVENTORY_FLAG_TRUNCATED));
}

// The cap is hit inside a nested container: the walk keeps counting, in the same order.
void TestCapInsideNested() {
    Tree t;
    t.containers[1] = { Item(26, 1, 2), Item(24, 2) };
    t.containers[2].assign(150, Empty());
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == 128 && o.r.totalSeen == 152 && o.r.flags == SC_INVENTORY_FLAG_TRUNCATED);
    CHECK(o.ports[0].entity_id == 1 && o.ports[1].valid == 0 && o.ports[127].valid == 0);
}

void TestPortsBudget() {
    Tree t;
    t.containers[1].assign(5000, Empty());
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.count == 128 && o.r.totalSeen == kMaxPortsSeen);
    CHECK(o.r.flags == (SC_INVENTORY_FLAG_TRUNCATED | SC_INVENTORY_FLAG_LIMITED));
}

void TestContainerBudget() {
    Tree t;
    for (uintptr_t c = 1; c <= 400; ++c) t.containers[c] = {};
    for (uintptr_t c = 1; c <= 300; ++c) t.containers[1].push_back(Item(54, 9000 + c, 1 + c));
    Out o = Run(t, 1);
    CHECK(o.ok && o.r.totalSeen == 300 && o.r.count == 128 && (o.r.flags & SC_INVENTORY_FLAG_LIMITED));
}

void TestGuardFailures() {
    Tree t;
    t.containers[1] = { Item(26, 1, 2), Item(24, 2) };
    t.containers[2] = { Item(85, 3) };
    // The player's own container failing a guard: no answer.
    t.badContainer[1] = true;
    CHECK(!Run(t, 1).ok);
    t.badContainer.clear();
    // Unknown root, null root.
    CHECK(!Run(t, 7).ok);
    CHECK(!Run(t, 0).ok);
    // A port of the player's container unreadable: no answer, even after earlier ports were read.
    t.badPortAt[1] = 2;
    CHECK(!Run(t, 1).ok);
    t.badPortAt.clear();
    // A nested container (or one of its ports) failing: skipped and flagged, siblings stay.
    t.badContainer[2] = true;
    Out n = Run(t, 1);
    CHECK(n.ok && n.r.count == 2 && n.r.totalSeen == 2 && n.r.flags == SC_INVENTORY_FLAG_LIMITED);
    t.badContainer.clear();
    t.badPortAt[2] = 1;
    Out m = Run(t, 1);
    CHECK(m.ok && m.r.count == 2 && (m.r.flags & SC_INVENTORY_FLAG_LIMITED));
    // Missing callbacks and a null out with capacity.
    Access none;
    sc_inventory_port ports[1];
    WalkResult r;
    CHECK(!Walk(none, 1, 1, ports, r));
    CHECK(!Walk(AccessOf(t), 1, 1, nullptr, r));
    CHECK(Walk(AccessOf(t), 1, 0, nullptr, r) && r.totalSeen == 2);
}

void TestGeneration() {
    sc_inventory_port a[3];
    memset(a, 0, sizeof(a));
    a[0].valid = 1; a[0].item_class_id = 22; a[0].entity_id = 1;
    a[1].valid = 1; a[1].item_class_id = 95; a[1].entity_id = 2;
    Generation g;
    CHECK(g.Update(a, 2, 2) == 1);
    CHECK(g.Update(a, 2, 2) == 1);                      // the same set: no change
    a[1].entity_id = 3;
    CHECK(g.Update(a, 2, 2) == 2);                      // an item replaced
    CHECK(g.Update(a, 2, 2) == 2);
    std::swap(a[0], a[1]);
    CHECK(g.Update(a, 2, 2) == 3);                      // the order is part of the set
    CHECK(g.Update(a, 2, 5) == 4);                      // more ports beyond the cap
    CHECK(g.Update(a, 1, 1) == 5);                      // a port gone
    a[2].valid = 0;
    CHECK(g.Update(a, 0, 0) == 6);
    g.Reset();
    CHECK(g.Update(a, 0, 0) == 1);
    // An empty port that gains an item changes the set; the same set read twice does not.
    Generation h;
    sc_inventory_port e[1];
    memset(e, 0, sizeof(e));
    const uint64_t first = h.Update(e, 1, 1);
    e[0].valid = 1; e[0].item_class_id = 85; e[0].entity_id = 9;
    CHECK(h.Update(e, 1, 1) == first + 1 && h.Update(e, 1, 1) == first + 1);
}

// ---- the snapshot service's gating ----------------------------------------------------------

struct FakeBackend {
    uint32_t count = 0, total = 0, flags = 0;
    uint64_t generation = 7;
    uint32_t sawCapacity = 0, sawSize = 0;
    bool answer = true;
};

bool BackendSnapshot(void* ctx, sc_inventory_snapshot* io) {
    FakeBackend& b = *static_cast<FakeBackend*>(ctx);
    b.sawCapacity = io->count;
    b.sawSize = io->size;
    if (!b.answer) return false;
    uint32_t n = b.count < io->count ? b.count : io->count;
    for (uint32_t i = 0; i < n; ++i) { io->ports[i].size = sizeof(sc_inventory_port); io->ports[i].valid = 1; io->ports[i].item_class_id = 22; io->ports[i].entity_id = 100 + i; }
    io->count = n;
    io->generation = b.generation;
    io->total_seen = b.total;
    io->flags = b.flags;
    return true;
}

// A caller's buffer with a canary after the 1.0 part, to see what the service writes.
struct Caller {
    sc_inventory_snapshot s;
    Caller(uint32_t size, uint32_t capacity) { memset(&s, 0xEE, sizeof(s)); s.size = size; s.count = capacity; }
};

void TestServiceGating() {
    static_assert(SC_INVENTORY_SNAPSHOT_SIZE_1_0 == offsetof(sc_inventory_snapshot, flags), "1.0 size is where the tail starts");
    static_assert(sizeof(sc_inventory_snapshot) == SC_INVENTORY_SNAPSHOT_SIZE_1_0 + 8, "the tail is flags and total_seen");
    FakeBackend fb;
    fb.count = 10; fb.total = 10;
    InventoryBackend be{ &fb, BackendSnapshot };

    // A 1.0 caller (size 3088): ports and generation, no tail written.
    Caller old(SC_INVENTORY_SNAPSHOT_SIZE_1_0, 128);
    CHECK(SnapshotWith(be, &old.s) == SCO_OK);
    CHECK(old.s.size == SC_INVENTORY_SNAPSHOT_SIZE_1_0 && old.s.count == 10 && old.s.generation == 7);
    CHECK(old.s.ports[9].entity_id == 109);
    CHECK(old.s.flags == 0xEEEEEEEEu && old.s.total_seen == 0xEEEEEEEEu);
    // The backend always sees a full-size snapshot, whatever the caller passed.
    CHECK(fb.sawSize == sizeof(sc_inventory_snapshot) && fb.sawCapacity == 128);

    // A 1.1 caller gets the tail.
    Caller cur(sizeof(sc_inventory_snapshot), 128);
    CHECK(SnapshotWith(be, &cur.s) == SCO_OK && cur.s.count == 10 && cur.s.total_seen == 10 && cur.s.flags == 0);

    // Capacity below the ports: the backend sees the capacity; truncation is derived and reported.
    fb.count = 50; fb.total = 50;
    Caller small(sizeof(sc_inventory_snapshot), 4);
    CHECK(SnapshotWith(be, &small.s) == SCO_OK && fb.sawCapacity == 4);
    CHECK(small.s.count == 4 && small.s.total_seen == 50 && small.s.flags == SC_INVENTORY_FLAG_TRUNCATED);
    // The ports past the capacity are left alone.
    CHECK(small.s.ports[4].entity_id == 0xEEEEEEEEEEEEEEEEull);

    // A capacity above the maximum is clamped; zero capacity writes no ports.
    Caller huge(sizeof(sc_inventory_snapshot), 5000);
    CHECK(SnapshotWith(be, &huge.s) == SCO_OK && fb.sawCapacity == SC_INVENTORY_MAX_PORTS && huge.s.count == 50);
    Caller none(sizeof(sc_inventory_snapshot), 0);
    CHECK(SnapshotWith(be, &none.s) == SCO_OK && none.s.count == 0 && none.s.total_seen == 50 && (none.s.flags & SC_INVENTORY_FLAG_TRUNCATED));

    // The service keeps the tail consistent whatever the backend said.
    fb.count = 3; fb.total = 1; fb.flags = SC_INVENTORY_FLAG_TRUNCATED | 0x80;   // total below count, stale TRUNCATED, unknown bit
    Caller odd(sizeof(sc_inventory_snapshot), 128);
    CHECK(SnapshotWith(be, &odd.s) == SCO_OK && odd.s.total_seen == 3 && odd.s.flags == 0);
    fb.flags = SC_INVENTORY_FLAG_LIMITED; fb.total = 3;
    Caller lim(sizeof(sc_inventory_snapshot), 128);
    CHECK(SnapshotWith(be, &lim.s) == SCO_OK && lim.s.flags == SC_INVENTORY_FLAG_LIMITED);

    // UNAVAILABLE: size below the 1.0 snapshot, null out, no backend, a backend with nothing to say.
    Caller tiny(SC_INVENTORY_SNAPSHOT_SIZE_1_0 - 1, 128);
    CHECK(SnapshotWith(be, &tiny.s) == SCO_UNAVAILABLE);
    CHECK(SnapshotWith(be, nullptr) == SCO_UNAVAILABLE);
    CHECK(SnapshotWith(InventoryBackend{}, &cur.s) == SCO_UNAVAILABLE);
    fb.answer = false;
    Caller no(sizeof(sc_inventory_snapshot), 128);
    CHECK(SnapshotWith(be, &no.s) == SCO_UNAVAILABLE && no.s.count == 128 && no.s.generation == 0xEEEEEEEEEEEEEEEEull);
}

}  // namespace

int main() {
    TestFlat();
    TestEmptyContainer();
    TestNestedOrder();
    TestCycle();
    TestDepthLimit();
    TestCapAndTruncation();
    TestCapInsideNested();
    TestPortsBudget();
    TestContainerBudget();
    TestGuardFailures();
    TestGeneration();
    TestServiceGating();
    std::printf("game.inventory walk and service: %d passed, %d failed\n", passed, failed);
    return failed ? 1 : 0;
}
