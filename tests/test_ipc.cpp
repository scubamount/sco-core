// Unit tests for the sc_ipc.h wire and the sco.ipc service (sco/ipc.h, sco_ipc.h, scosdk/ipc.hpp).
// Host build, no game. The wire runs over plain heap memory sized exactly to the mapping, so the
// sanitizers see any access past it; the service runs over real named shared memory, with the
// peer's side written against sc_ipc.h alone (a second mapping of the same name, opened the way
// the other program opens it). The "game thread" is this test's main thread.
//   test_ipc                     (tools/test.sh, CTest test_ipc)
#include "sco/host.h"
#include "sco/ipc.h"
#include "sco/runtime.h"
#include "sc_ipc.h"
#include "sco_api.h"
#include "sco_ipc.h"
#include "scosdk/ipc.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <new>
#include <random>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#endif

using sco::Result;

static std::atomic<int> g_fail{ 0 }, g_pass{ 0 };
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const sco_api*    g_api = nullptr;
static const sco_ipc_v1* g_t = nullptr;

// ---- helpers ------------------------------------------------------------------------------------

// Heap memory of exactly n bytes, 64-byte aligned like a mapping, zeroed.
struct Buf {
    uint8_t* p;
    uint64_t n;
    explicit Buf(uint64_t bytes)
        : p(static_cast<uint8_t*>(::operator new(static_cast<size_t>(bytes), std::align_val_t(64)))), n(bytes) {
        std::memset(p, 0, static_cast<size_t>(bytes));
    }
    ~Buf() { ::operator delete(p, std::align_val_t(64)); }
    Buf(const Buf&) = delete;
    Buf& operator=(const Buf&) = delete;
};

// The clock both sides of a channel use (sco::ipc's own is internal).
static uint64_t NowMs() {
#ifdef _WIN32
    return GetTickCount64();
#else
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000u + static_cast<uint64_t>(ts.tv_nsec) / 1000000u;
#endif
}

static uint32_t Pid() {
#ifdef _WIN32
    return GetCurrentProcessId();
#else
    return static_cast<uint32_t>(getpid());
#endif
}

// The peer's way in: opens an existing mapping by name, the size from the OS, never from the
// mapping. What a bridge's other side does.
struct PeerMap {
    void*    base = nullptr;
    uint64_t bytes = 0;
#ifdef _WIN32
    HANDLE h = nullptr;
#else
    int fd = -1;
#endif
    bool Open(const std::string& name) {
        Close();
#ifdef _WIN32
        const std::wstring w(name.begin(), name.end());
        h = OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, FALSE, w.c_str());
        if (!h) return false;
        base = MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, 0);
        if (!base) return false;
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(base, &mbi, sizeof(mbi)) == 0) return false;
        bytes = mbi.RegionSize;
#else
        fd = shm_open(name.c_str(), O_RDWR, 0);
        if (fd < 0) return false;
        struct stat st {};
        if (fstat(fd, &st) != 0 || st.st_size <= 0) return false;
        bytes = static_cast<uint64_t>(st.st_size);
        base = mmap(nullptr, static_cast<size_t>(bytes), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (base == MAP_FAILED) { base = nullptr; return false; }
#endif
        return true;
    }
    void Close() {
#ifdef _WIN32
        if (base) UnmapViewOfFile(base);
        if (h) CloseHandle(h);
        h = nullptr;
#else
        if (base) munmap(base, static_cast<size_t>(bytes));
        if (fd >= 0) close(fd);
        fd = -1;
#endif
        base = nullptr;
        bytes = 0;
    }
    PeerMap() = default;
    PeerMap(const PeerMap&) = delete;
    PeerMap& operator=(const PeerMap&) = delete;
    ~PeerMap() { Close(); }
    sc_ipc_hdr* Hdr() const { return static_cast<sc_ipc_hdr*>(base); }
};

static int Pop(sc_ipc_ring_view* v, uint32_t* type, void* out, uint32_t cap, uint32_t* size = nullptr) {
    uint32_t n = cap;
    const int rc = sc_ipc_ring_pop(v, type, out, &n);
    if (size) *size = n;
    return rc;
}

// ---- the wire: header ---------------------------------------------------------------------------

static void TestWireHeader() {
    Buf m(8192);
    sc_ipc_chan own{}, peer{};
    sc_ipc_hdr* h = reinterpret_cast<sc_ipc_hdr*>(m.p);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_NOT_READY);   // nothing there yet
    CHECK(sc_ipc_init(m.p, m.n, 11, 7, 1, 1000, &own) == SC_IPC_OK && own.epoch == 1 && own.bytes == m.n);
    CHECK(h->magic == SC_IPC_MAGIC && h->version == SC_IPC_VERSION && h->owner_pid == 11 && h->state == SC_IPC_STATE_OPEN);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_OK && peer.epoch == 1 && peer.bytes == m.n);
    CHECK(sc_ipc_attach(m.p, m.n, 8, 1, &peer) == SC_IPC_MISMATCH);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 2, &peer) == SC_IPC_MISMATCH);
    CHECK(sc_ipc_attach(m.p, 32, 7, 1, &peer) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_attach(m.p + 8, m.n - 8, 7, 1, &peer) == SC_IPC_BAD_ARG);   // not aligned like a mapping
    CHECK(sc_ipc_attach(nullptr, m.n, 7, 1, &peer) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_init(m.p, 16, 11, 7, 1, 1000, &own) == SC_IPC_BAD_ARG);

    // A header the owner never wrote: sizes that don't fit the view, another version, bad magic.
    h->bytes = m.n + 1;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_CORRUPT);
    h->bytes = 16;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_CORRUPT);
    h->bytes = UINT64_MAX;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_CORRUPT);
    h->bytes = m.n;
    h->version = 2;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_MISMATCH);
    h->version = SC_IPC_VERSION;
    h->magic = 0x12345678u;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_NOT_READY);
    CHECK(sc_ipc_check(&peer) == SC_IPC_EPOCH);
    h->magic = 0;   // torn: the owner is mid re-init
    CHECK(sc_ipc_check(&peer) == SC_IPC_EPOCH);
    h->magic = SC_IPC_MAGIC;
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_OK && sc_ipc_check(&peer) == SC_IPC_OK);

    // The owner re-creates the channel: the old view is stale, a new attach gets the new epoch.
    CHECK(sc_ipc_init(m.p, m.n, 11, 7, 1, 2000, &own) == SC_IPC_OK && own.epoch == 2);
    CHECK(sc_ipc_check(&peer) == SC_IPC_EPOCH);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_OK && peer.epoch == 2);
    h->epoch = UINT64_MAX;   // the epoch never comes back as 0
    CHECK(sc_ipc_init(m.p, m.n, 11, 7, 1, 2000, &own) == SC_IPC_OK && own.epoch == 1);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_OK);

    // Heartbeats on one clock; never beaten, a beat from the future, an age past 32 bits.
    uint32_t age = 99;
    CHECK(sc_ipc_peer_age_ms(&own, 5000, &age) == SC_IPC_NOT_READY && age == 0);
    sc_ipc_peer_beat(&peer, 22, 4900);
    CHECK(sc_ipc_peer_age_ms(&own, 5000, &age) == SC_IPC_OK && age == 100 && h->peer_pid == 22);
    CHECK(sc_ipc_owner_age_ms(&peer, 2500, &age) == SC_IPC_OK && age == 500);
    sc_ipc_owner_beat(&own, 2600);
    CHECK(sc_ipc_owner_age_ms(&peer, 2600, &age) == SC_IPC_OK && age == 0);
    h->peer_heartbeat_ms = 9999999;
    CHECK(sc_ipc_peer_age_ms(&own, 5000, &age) == SC_IPC_OK && age == 0);
    h->peer_heartbeat_ms = 1;
    CHECK(sc_ipc_peer_age_ms(&own, UINT64_MAX, &age) == SC_IPC_OK && age == 0xFFFFFFFFu);
    CHECK(sc_ipc_peer_age_ms(&own, 5000, nullptr) == SC_IPC_BAD_ARG);

    sc_ipc_close(&own);
    CHECK(sc_ipc_check(&peer) == SC_IPC_GONE);
    CHECK(sc_ipc_attach(m.p, m.n, 7, 1, &peer) == SC_IPC_GONE);
}

// ---- the wire: blocks ---------------------------------------------------------------------------

struct Snap {
    uint64_t a, b, c;
};

static void TestWireBlocks() {
    Buf m(4096);
    sc_ipc_chan c{};
    CHECK(sc_ipc_init(m.p, m.n, 1, 1, 1, 1, &c) == SC_IPC_OK);
    Snap s{ 1, 2, 3 }, r{};
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_NOT_READY);
    CHECK(sc_ipc_block_write(&c, 128, &s, sizeof s) == SC_IPC_OK);
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_OK && r.a == 1 && r.b == 2 && r.c == 3);
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r - 8) == SC_IPC_MISMATCH);
    // Offsets: the header, unaligned, past the end, overflowing, the exact end.
    CHECK(sc_ipc_block_write(&c, 0, &s, sizeof s) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_write(&c, 56, &s, sizeof s) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_write(&c, 132, &s, sizeof s) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_write(&c, m.n - 16, &s, 8) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_write(&c, m.n - 24, &s, 8) == SC_IPC_OK);
    CHECK(sc_ipc_block_read(&c, m.n - 24, &r, 8) == SC_IPC_OK && r.a == 1);
    CHECK(sc_ipc_block_write(&c, UINT64_MAX - 7, &s, 8) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_read(&c, UINT64_MAX - 7, &r, 8) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_block_read(&c, 128, &r, 0xFFFFFFFFu) == SC_IPC_BAD_ARG);   // checked before any copy
    CHECK(sc_ipc_block_write(&c, 128, nullptr, 8) == SC_IPC_BAD_ARG);
    // A writer that died mid-write leaves seq odd: readers see BUSY until the next write.
    sc_ipc_block* b = reinterpret_cast<sc_ipc_block*>(m.p + 128);
    b->seq |= 1u;
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_BUSY);
    s.a = 10;
    CHECK(sc_ipc_block_write(&c, 128, &s, sizeof s) == SC_IPC_OK && (b->seq & 1u) == 0);
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_OK && r.a == 10);
    b->seq = UINT64_MAX - 1;   // the sequence wraps without passing through 0
    CHECK(sc_ipc_block_write(&c, 128, &s, sizeof s) == SC_IPC_OK && b->seq == 2);
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_OK);
    b->size = 0xFFFFFFFFu;   // a hostile size field is a mismatch, never a length
    CHECK(sc_ipc_block_read(&c, 128, &r, sizeof r) == SC_IPC_MISMATCH);
}

// ---- the wire: rings ----------------------------------------------------------------------------

static void TestWireRing() {
    const uint64_t cap = 256;
    Buf m(SC_IPC_HEADER_BYTES + SC_IPC_RING_BYTES + cap);   // the ring's data ends the buffer
    sc_ipc_chan own{}, peer{};
    sc_ipc_ring_view pv{}, cv{};
    CHECK(sc_ipc_init(m.p, m.n, 1, 5, 1, 1, &own) == SC_IPC_OK);
    CHECK(sc_ipc_ring_init(&own, 0, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_init(&own, 96, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_init(&own, 64, 100, SC_IPC_TO_PEER, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_init(&own, 64, 32, SC_IPC_TO_PEER, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_init(&own, 64, 512, SC_IPC_TO_PEER, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_init(&own, 64, cap, 3, &pv) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_attach(m.p, m.n, 5, 1, &peer) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_NOT_READY);
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK && cv.capacity == cap && cv.direction == SC_IPC_TO_PEER);
    sc_ipc_ring* r = reinterpret_cast<sc_ipc_ring*>(m.p + 64);

    uint8_t buf[512];
    uint32_t type = 0, n = 0;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EMPTY);
    CHECK(sc_ipc_ring_push(&pv, 5, "hello", 5) == SC_IPC_OK);
    CHECK(sc_ipc_ring_push(&pv, 6, nullptr, 0) == SC_IPC_OK);
    CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && type == 5 && n == 5 && std::memcmp(buf, "hello", 5) == 0);
    CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && type == 6 && n == 0);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EMPTY);
    CHECK(r->pushed == 2 && r->popped == 2);
    CHECK(sc_ipc_ring_push(&pv, SC_IPC_REC_PAD, "x", 1) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_push(&pv, 1, nullptr, 4) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_push(&pv, 1, buf, static_cast<uint32_t>(cap - 7)) == SC_IPC_BAD_ARG);   // can never fit

    // The size handshake: ask, too small, then fits; the record stays queued until it fits.
    std::memset(buf, 0xAB, 40);
    CHECK(sc_ipc_ring_push(&pv, 7, buf, 40) == SC_IPC_OK);
    n = 0;
    CHECK(sc_ipc_ring_pop(&cv, &type, nullptr, &n) == SC_IPC_TOO_SMALL && n == 40);
    CHECK(Pop(&cv, &type, buf + 100, 10, &n) == SC_IPC_TOO_SMALL && n == 40);
    CHECK(Pop(&cv, &type, buf + 100, 40, &n) == SC_IPC_OK && n == 40 && type == 7 && buf[139] == 0xAB);
    n = 4;
    CHECK(sc_ipc_ring_pop(&cv, &type, nullptr, &n) == SC_IPC_BAD_ARG);

    // Full and empty: 24-byte records until full, then all back in order.
    uint32_t pushed = 0;
    for (uint32_t i = 0; i < 100; ++i) {
        const int rc = sc_ipc_ring_push(&pv, 1, &i, 16);
        if (rc == SC_IPC_FULL) break;
        CHECK(rc == SC_IPC_OK);
        ++pushed;
    }
    CHECK(pushed >= 9 && pushed <= cap / 24);
    for (uint32_t i = 0; i < pushed; ++i) {
        uint32_t v = 0;
        uint8_t rec[16];
        CHECK(Pop(&cv, &type, rec, sizeof rec, &n) == SC_IPC_OK && n == 16);
        std::memcpy(&v, rec, 4);
        CHECK(v == i);
    }
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EMPTY);

    // The padding case: a record that doesn't fit before the end pads it and starts at 0.
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
    std::memset(buf, 1, 200);
    CHECK(sc_ipc_ring_push(&pv, 1, buf, 200) == SC_IPC_OK && r->head == 208);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_OK && r->tail == 208);
    std::memset(buf, 2, 100);
    CHECK(sc_ipc_ring_push(&pv, 2, buf, 100) == SC_IPC_OK && r->head == 208 + 48 + 112);
    std::memset(buf, 0, 100);
    CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && type == 2 && n == 100 && buf[0] == 2 && buf[99] == 2);
    CHECK(r->tail == 208 + 48 + 112);
    // Padding when the pad itself is all the room left: pushed, still full, then fits after a pop.
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
    CHECK(sc_ipc_ring_push(&pv, 1, buf, 120) == SC_IPC_OK);    // 128
    CHECK(sc_ipc_ring_push(&pv, 1, buf, 88) == SC_IPC_OK);     // 96 -> head 224
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_OK);       // tail 128
    CHECK(sc_ipc_ring_push(&pv, 3, buf, 128) == SC_IPC_FULL);  // pad 32 written, 136 more doesn't fit
    CHECK(r->head == 256);
    CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && n == 88);
    CHECK(sc_ipc_ring_push(&pv, 3, buf, 128) == SC_IPC_OK);
    CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && type == 3 && n == 128);   // skips the pad
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EMPTY);

    // Random sizes against a reference queue: every byte round-trips through many wraps.
    std::mt19937 rng(12345);
    std::deque<std::vector<uint8_t>> ref;
    uint64_t done = 0;
    for (int step = 0; step < 20000; ++step) {
        if (rng() % 2 == 0) {
            std::vector<uint8_t> msg(rng() % 121);
            for (auto& x : msg) x = static_cast<uint8_t>(rng());
            const int rc = sc_ipc_ring_push(&pv, static_cast<uint32_t>(msg.size()), msg.data(), static_cast<uint32_t>(msg.size()));
            CHECK(rc == SC_IPC_OK || rc == SC_IPC_FULL);
            if (rc == SC_IPC_OK) ref.push_back(std::move(msg));
        } else {
            const int rc = Pop(&cv, &type, buf, sizeof buf, &n);
            if (ref.empty()) {
                CHECK(rc == SC_IPC_EMPTY);
            } else {
                const bool same = rc == SC_IPC_OK && n == ref.front().size() && type == n &&
                                  (n == 0 || std::memcmp(buf, ref.front().data(), n) == 0);
                CHECK(same);
                ref.pop_front();
                ++done;
            }
        }
    }
    CHECK(done > 5000 && r->head > 100 * cap);

    // Sequence rollover: head and tail cross 2^64.
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    r->head = r->tail = UINT64_MAX - 63;
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
    for (uint32_t i = 0; i < 200; ++i) {
        uint8_t msg[40];
        std::memset(msg, static_cast<int>(i), sizeof msg);
        const uint32_t len = i % 41;
        CHECK(sc_ipc_ring_push(&pv, i, msg, len) == SC_IPC_OK);
        CHECK(Pop(&cv, &type, buf, sizeof buf, &n) == SC_IPC_OK && type == i && n == len && (len == 0 || buf[len - 1] == static_cast<uint8_t>(i)));
    }
    CHECK(r->head < 100000 && r->head == r->tail);

    // Epoch: the owner re-creates the channel; old ring views stop, a re-laid ring attaches again.
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_OK);
    CHECK(sc_ipc_init(m.p, m.n, 1, 5, 1, 1, &own) == SC_IPC_OK);
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_EPOCH);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EPOCH);
    CHECK(sc_ipc_attach(m.p, m.n, 5, 1, &peer) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_EPOCH);   // still laid out under the old epoch
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EMPTY);     // the old record went with the old epoch
    sc_ipc_close(&own);
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_GONE);
}

// ---- the wire: a hostile peer -------------------------------------------------------------------

static void TestWireHostile() {
    const uint64_t cap = 256;
    Buf m(SC_IPC_HEADER_BYTES + SC_IPC_RING_BYTES + cap);
    sc_ipc_chan own{}, peer{};
    sc_ipc_ring_view pv{}, cv{};
    CHECK(sc_ipc_init(m.p, m.n, 1, 5, 1, 1, &own) == SC_IPC_OK);
    CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_attach(m.p, m.n, 5, 1, &peer) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
    sc_ipc_ring* r = reinterpret_cast<sc_ipc_ring*>(m.p + 64);
    uint8_t* d = m.p + 64 + SC_IPC_RING_BYTES;
    uint8_t buf[512];
    uint32_t type = 0;
    auto rec = [&](uint64_t off, uint32_t size, uint32_t t) {
        const sc_ipc_rec x{ size, t };
        std::memcpy(d + off, &x, sizeof x);
    };
    auto reset = [&] { r->head = r->tail = 0; };

    // Counters a producer can scribble: past the capacity, unaligned, behind the tail.
    r->head = r->tail + cap + 8;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_CORRUPT);
    r->head = r->tail + 4;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    r->tail = 64; r->head = 32;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    // ... and a consumer: tail ahead of head, unaligned.
    r->head = 64; r->tail = 72;
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_CORRUPT);
    r->tail = 3;
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_CORRUPT);

    // Record lengths: huge, more than queued, crossing the end.
    reset();
    rec(0, 0xFFFFFFF0u, 1);
    r->head = 16;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    rec(0, 100, 1);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    r->tail = 240; r->head = 240 + 48;
    rec(240, 40, 1);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    // Pads: the wrong size, longer than what is queued, a pad after a pad.
    rec(240, 0, SC_IPC_REC_PAD);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    r->head = 248;
    rec(240, 8, SC_IPC_REC_PAD);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);
    r->tail = 240; r->head = 256 + 8;
    rec(240, 8, SC_IPC_REC_PAD);
    rec(0, 0, SC_IPC_REC_PAD);
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_CORRUPT);

    // Geometry rewritten after attach: the views keep the capacity they validated.
    reset();
    r->capacity = 1u << 20;
    CHECK(sc_ipc_ring_push(&pv, 1, buf, 200) == SC_IPC_OK);
    CHECK(sc_ipc_ring_push(&pv, 1, buf, 200) == SC_IPC_FULL);   // still 256 bytes
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_OK);
    sc_ipc_ring_view again{};
    CHECK(sc_ipc_ring_attach(&peer, 64, &again) == SC_IPC_CORRUPT);
    r->capacity = 100;
    CHECK(sc_ipc_ring_attach(&peer, 64, &again) == SC_IPC_CORRUPT);
    r->capacity = 0;
    CHECK(sc_ipc_ring_attach(&peer, 64, &again) == SC_IPC_CORRUPT);
    r->capacity = cap;
    r->direction = 9;
    CHECK(sc_ipc_ring_attach(&peer, 64, &again) == SC_IPC_CORRUPT);
    r->direction = SC_IPC_TO_PEER;
    r->magic = 0;
    CHECK(sc_ipc_ring_push(&pv, 1, "a", 1) == SC_IPC_EPOCH);
    CHECK(sc_ipc_ring_attach(&peer, 64, &again) == SC_IPC_NOT_READY);
    r->magic = SC_IPC_RING_MAGIC;
    r->epoch = 77;
    CHECK(Pop(&cv, &type, buf, sizeof buf) == SC_IPC_EPOCH);
    r->epoch = own.epoch;
    CHECK(sc_ipc_ring_attach(&peer, 0, &again) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_attach(&peer, m.n, &again) == SC_IPC_BAD_ARG);
    CHECK(sc_ipc_ring_attach(&peer, UINT64_MAX - 63, &again) == SC_IPC_BAD_ARG);

    // Random scribbles of counters, record headers and data, then pops and pushes: whatever the
    // results, nothing is read or written outside the buffer (the sanitizers would stop the test).
    std::mt19937_64 rng(777);
    int ok = 0, corrupt = 0;
    for (int step = 0; step < 50000; ++step) {
        if (step % 200 == 0) {
            CHECK(sc_ipc_ring_init(&own, 64, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
            CHECK(sc_ipc_ring_attach(&peer, 64, &cv) == SC_IPC_OK);
        }
        switch (rng() % 6) {
            case 0: r->head = r->tail + 8 * (rng() % 40); break;
            case 1: r->tail = rng(); break;
            case 2: rec(8 * (rng() % (cap / 8)), static_cast<uint32_t>(rng() % 300), static_cast<uint32_t>(rng() % 3 == 0 ? SC_IPC_REC_PAD : rng())); break;
            case 3: d[rng() % cap] = static_cast<uint8_t>(rng()); break;
            case 4: r->head = rng(); break;
            default: break;
        }
        uint32_t n = static_cast<uint32_t>(rng() % sizeof buf);
        const int a = sc_ipc_ring_pop(&cv, &type, buf, &n);
        CHECK(a == SC_IPC_OK || a == SC_IPC_EMPTY || a == SC_IPC_CORRUPT || a == SC_IPC_TOO_SMALL);
        CHECK(a != SC_IPC_OK || n <= cap);
        const int b = sc_ipc_ring_push(&pv, 1, buf, static_cast<uint32_t>(rng() % 120));
        CHECK(b == SC_IPC_OK || b == SC_IPC_FULL || b == SC_IPC_CORRUPT);
        ok += a == SC_IPC_OK;
        corrupt += a == SC_IPC_CORRUPT;
    }
    CHECK(ok > 0 && corrupt > 0);
}

// ---- the wire: a writer and a reader on threads ---------------------------------------------------

static void TestWireThreads() {
    const uint64_t cap = 4096, ringAt = 64, blockAt = ringAt + SC_IPC_RING_BYTES + cap;
    Buf m(blockAt + SC_IPC_BLOCK_BYTES + sizeof(Snap));
    sc_ipc_chan own{}, peer{};
    sc_ipc_ring_view pv{}, cv{};
    CHECK(sc_ipc_init(m.p, m.n, 1, 9, 1, 1, &own) == SC_IPC_OK);
    CHECK(sc_ipc_ring_init(&own, ringAt, cap, SC_IPC_TO_PEER, &pv) == SC_IPC_OK);
    CHECK(sc_ipc_attach(m.p, m.n, 9, 1, &peer) == SC_IPC_OK);
    CHECK(sc_ipc_ring_attach(&peer, ringAt, &cv) == SC_IPC_OK);
    constexpr uint32_t kCount = 20000;
    std::atomic<int> bad{ 0 };
    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount;) {
            uint8_t msg[64];
            std::memset(msg, static_cast<int>(i & 0xFF), sizeof msg);
            std::memcpy(msg, &i, 4);
            const int rc = sc_ipc_ring_push(&pv, i, msg, 4 + i % 60);
            if (rc == SC_IPC_OK) ++i;
            else if (rc == SC_IPC_FULL && bad == 0) std::this_thread::yield();
            else { ++bad; return; }
        }
    });
    std::thread consumer([&] {
        for (uint32_t i = 0; i < kCount && bad == 0;) {
            uint8_t msg[64];
            uint32_t type = 0, n = sizeof msg;
            const int rc = sc_ipc_ring_pop(&cv, &type, msg, &n);
            if (rc == SC_IPC_EMPTY) { std::this_thread::yield(); continue; }
            uint32_t v = 0;
            std::memcpy(&v, msg, 4);
            if (rc != SC_IPC_OK || v != i || type != i || n != 4 + i % 60 || (n > 4 && msg[n - 1] != static_cast<uint8_t>(i & 0xFF))) {
                ++bad;
                return;
            }
            ++i;
        }
    });
    std::atomic<bool> writing{ true };
    std::thread writer([&] {
        for (uint64_t i = 1; i <= kCount; ++i) {
            const Snap s{ i, i * 2, i * 3 };
            if (sc_ipc_block_write(&own, blockAt, &s, sizeof s) != SC_IPC_OK) ++bad;
        }
        writing = false;
    });
    std::thread reader([&] {
        uint64_t last = 0;
        int ok = 0;
        while (writing || last < kCount) {
            Snap s{};
            const int rc = sc_ipc_block_read(&peer, blockAt, &s, sizeof s);
            if (rc == SC_IPC_OK) {
                if (s.b != s.a * 2 || s.c != s.a * 3 || s.a < last) { ++bad; return; }   // never torn, never backwards
                last = s.a;
                ++ok;
            } else if (rc != SC_IPC_BUSY && rc != SC_IPC_NOT_READY) {
                ++bad;
                return;
            }
            if (!writing && rc == SC_IPC_OK && last == kCount) break;
        }
        if (ok == 0) ++bad;
    });
    producer.join();
    consumer.join();
    writer.join();
    reader.join();
    CHECK(bad == 0);
}

// ---- the service --------------------------------------------------------------------------------

static void TestService() {
    const void* table = nullptr;
    CHECK(g_api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &table) == SCO_OK && table == g_t);
    CHECK(g_t->size == sizeof(sco_ipc_v1));
    sco_plugin* p = sco::host::NewPlugin("alpha");
    sco_plugin* q = sco::host::NewPlugin("beta");
    uint64_t ch = 0;
    // Names and sizes.
    CHECK(g_t->create(p, nullptr, 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "Link", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "a/b", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "a.b", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "..\\x", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "abcdefghijklmnopqrstuvwxyz012345", 65536, 1, 1, &ch) == SCO_BAD_ARG);   // 32
    CHECK(g_t->create(p, "link", 0, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "link", SCO_IPC_MIN_CHANNEL_BYTES - 1, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "link", SCO_IPC_MAX_CHANNEL_BYTES + 1ull, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "link", 65536, 1, 1, nullptr) == SCO_BAD_ARG);
    CHECK(g_t->create(nullptr, "link", 65536, 1, 1, &ch) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "link", 65536, 42, 3, &ch) == SCO_OK && ch != 0);
    uint64_t dup = 0;
    CHECK(g_t->create(p, "link", 65536, 42, 3, &dup) == SCO_BAD_ARG);
    const std::string name = sco::ipc::MappingName("alpha", "link");
    CHECK(name.size() > std::strlen(sco::ipc::kPrefix) && name.compare(0, std::strlen(sco::ipc::kPrefix), sco::ipc::kPrefix) == 0);
    CHECK(name.size() >= 10 && name.compare(name.size() - 10, 10, "alpha.link") == 0);
    CHECK(sco::ipc::MappingName("Alpha", "link").empty() && sco::ipc::MappingName("alpha", "a.b").empty());

    void* base = nullptr;
    uint64_t bytes = 0;
    CHECK(g_t->view(p, ch, &base, &bytes) == SCO_OK && base && bytes == 65536);
    const sc_ipc_hdr* hdr = static_cast<const sc_ipc_hdr*>(base);
    CHECK(hdr->magic == SC_IPC_MAGIC && hdr->layout_id == 42 && hdr->layout_version == 3 && hdr->bytes == 65536 && hdr->owner_pid == Pid());

    // The peer opens it by name, with sc_ipc.h alone.
    PeerMap pm;
    CHECK(pm.Open(name) && pm.bytes >= 65536);
    sc_ipc_chan peer{};
    CHECK(sc_ipc_attach(pm.base, pm.bytes, 42, 3, &peer) == SC_IPC_OK && peer.bytes == 65536);

    // Blocks.
    Snap s{ 7, 8, 9 }, r{};
    CHECK(g_t->block_write(p, ch, 0, &s, sizeof s) == SCO_BAD_ARG);   // the header is the host's
    CHECK(g_t->block_write(p, ch, 8, &s, sizeof s) == SCO_BAD_ARG);
    CHECK(g_t->block_write(p, ch, 65536 - 16, &s, sizeof s) == SCO_BAD_ARG);
    CHECK(g_t->block_write(p, ch, 64, nullptr, 8) == SCO_BAD_ARG);
    CHECK(g_t->block_read(p, ch, 1024, &r, sizeof r) == SCO_NOT_FOUND);
    CHECK(g_t->block_write(p, ch, 64, &s, sizeof s) == SCO_OK);
    CHECK(g_t->block_read(p, ch, 64, &r, sizeof r) == SCO_OK && r.a == 7 && r.c == 9);
    CHECK(g_t->block_read(p, ch, 64, &r, 8) == SCO_BAD_ARG);
    CHECK(sc_ipc_block_read(&peer, 64, &r, sizeof r) == SC_IPC_OK && r.b == 8);
    s.a = 70;
    CHECK(sc_ipc_block_write(&peer, 128, &s, sizeof s) == SC_IPC_OK);
    CHECK(g_t->block_read(p, ch, 128, &r, sizeof r) == SCO_OK && r.a == 70);
    reinterpret_cast<sc_ipc_block*>(static_cast<uint8_t*>(pm.base) + 128)->seq = 5;   // left torn by the peer
    CHECK(g_t->block_read(p, ch, 128, &r, sizeof r) == SCO_FAILED);

    // Rings.
    CHECK(g_t->ring_init(p, ch, 4096, 4096, SCO_IPC_TO_PEER) == SCO_OK);
    CHECK(g_t->ring_init(p, ch, 8192 + 192, 4096, SCO_IPC_FROM_PEER) == SCO_OK);
    CHECK(g_t->ring_init(p, ch, 4096 + 64, 1024, SCO_IPC_TO_PEER) == SCO_BAD_ARG);    // overlaps
    CHECK(g_t->ring_init(p, ch, 8192, 64, SCO_IPC_TO_PEER) == SCO_BAD_ARG);           // overlaps the next
    CHECK(g_t->ring_init(p, ch, 32768, 4096, 0) == SCO_BAD_ARG);
    CHECK(g_t->ring_init(p, ch, 32768, 100, SCO_IPC_TO_PEER) == SCO_BAD_ARG);
    CHECK(g_t->ring_init(p, ch, 65536 - 128, 64, SCO_IPC_TO_PEER) == SCO_BAD_ARG);
    CHECK(g_t->ring_init(p, ch, 32768, 1ull << 62, SCO_IPC_TO_PEER) == SCO_BAD_ARG);
    CHECK(g_t->ring_init(p, ch, UINT64_MAX - 63, 64, SCO_IPC_TO_PEER) == SCO_BAD_ARG);
    CHECK(g_t->ring_push(p, ch, 8192 + 192, 1, "x", 1) == SCO_BAD_ARG);   // FROM_PEER: the peer pushes
    CHECK(g_t->ring_push(p, ch, 16384, 1, "x", 1) == SCO_NOT_FOUND);
    CHECK(g_t->ring_push(p, ch, 4096, SC_IPC_REC_PAD, "x", 1) == SCO_BAD_ARG);
    char msg[64];
    uint32_t type = 0, n = sizeof msg;
    CHECK(g_t->ring_pop(p, ch, 4096, &type, msg, &n) == SCO_BAD_ARG);      // TO_PEER: the peer pops
    for (uint32_t i = 0; i < 3; ++i) CHECK(g_t->ring_push(p, ch, 4096, 10 + i, &i, sizeof i) == SCO_OK);
    sc_ipc_ring_view in{}, out{};
    CHECK(sc_ipc_ring_attach(&peer, 4096, &in) == SC_IPC_OK && in.direction == SC_IPC_TO_PEER);
    CHECK(sc_ipc_ring_attach(&peer, 8192 + 192, &out) == SC_IPC_OK && out.direction == SC_IPC_FROM_PEER);
    for (uint32_t i = 0; i < 3; ++i) {
        uint32_t v = 99;
        CHECK(Pop(&in, &type, &v, sizeof v) == SC_IPC_OK && v == i && type == 10 + i);
    }
    CHECK(sc_ipc_ring_push(&out, 9, "pong", 4) == SC_IPC_OK);
    n = 0;
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, nullptr, &n) == SCO_TOO_MANY && n == 4);
    n = 2;
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, msg, &n) == SCO_TOO_MANY && n == 4);
    n = sizeof msg;
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, msg, &n) == SCO_OK && n == 4 && type == 9 && std::memcmp(msg, "pong", 4) == 0);
    n = sizeof msg;
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, msg, &n) == SCO_NOT_FOUND);
    n = 0xFFFFFFFFu;   // a capacity the caller can't have: the host never trusts it past the ring
    CHECK(sc_ipc_ring_push(&out, 9, "pong", 4) == SC_IPC_OK);
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, msg, &n) == SCO_OK && n == 4);
    // A hostile peer: impossible counters are refused, the cached geometry never moves.
    sc_ipc_ring* outRing = reinterpret_cast<sc_ipc_ring*>(static_cast<uint8_t*>(pm.base) + 8192 + 192);
    outRing->head = outRing->tail + 12;
    n = sizeof msg;
    CHECK(g_t->ring_pop(p, ch, 8192 + 192, &type, msg, &n) == SCO_FAILED);
    outRing->head = outRing->tail;
    sc_ipc_ring* inRing = reinterpret_cast<sc_ipc_ring*>(static_cast<uint8_t*>(pm.base) + 4096);
    inRing->capacity = 1ull << 40;
    CHECK(g_t->ring_push(p, ch, 4096, 1, msg, 16) == SCO_OK);
    inRing->tail = inRing->head + 64;
    CHECK(g_t->ring_push(p, ch, 4096, 1, msg, 16) == SCO_FAILED);
    CHECK(g_t->ring_init(p, ch, 4096, 4096, SCO_IPC_TO_PEER) == SCO_OK);   // laid out again: works
    CHECK(g_t->ring_push(p, ch, 4096, 1, msg, 16) == SCO_OK);

    // Heartbeats: the peer's age, and the owner's written by Tick.
    uint32_t age = 1;
    CHECK(g_t->peer_age_ms(p, ch, &age) == SCO_NOT_FOUND && age == 0);
    sc_ipc_peer_beat(&peer, Pid(), NowMs() - 50);
    CHECK(g_t->peer_age_ms(p, ch, &age) == SCO_OK && age >= 50 && age < 60000);
    CHECK(g_t->peer_age_ms(p, ch, nullptr) == SCO_BAD_ARG);
    pm.Hdr()->owner_heartbeat_ms = 0;
    sco::ipc::Tick();
    CHECK(pm.Hdr()->owner_heartbeat_ms != 0);
    CHECK(sc_ipc_owner_age_ms(&peer, NowMs(), &age) == SC_IPC_OK && age < 60000);

    // Another plugin can't reach it by id.
    void* other = nullptr;
    CHECK(g_t->view(q, ch, &other, &bytes) == SCO_NOT_FOUND && other == nullptr);
    CHECK(g_t->block_write(q, ch, 64, &s, sizeof s) == SCO_NOT_FOUND);
    CHECK(g_t->close(q, ch) == SCO_NOT_FOUND);
    CHECK(g_t->view(p, ch + 1000, &other, &bytes) == SCO_NOT_FOUND);

    // Quotas: channels per plugin, bytes per plugin.
    std::vector<uint64_t> extra;
    for (int i = 0; i < 15; ++i) {
        uint64_t id = 0;
        const std::string nm = "c" + std::to_string(i);
        CHECK(g_t->create(p, nm.c_str(), 4096, 1, 1, &id) == SCO_OK);
        extra.push_back(id);
    }
    uint64_t id = 0;
    CHECK(g_t->create(p, "c99", 4096, 1, 1, &id) == SCO_TOO_MANY);
    for (uint64_t e : extra) CHECK(g_t->close(p, e) == SCO_OK);
    uint64_t big1 = 0, big2 = 0;
    CHECK(g_t->create(p, "big1", SCO_IPC_MAX_CHANNEL_BYTES, 1, 1, &big1) == SCO_OK);
    CHECK(g_t->create(p, "big2", SCO_IPC_MAX_CHANNEL_BYTES, 1, 1, &big2) == SCO_TOO_MANY);   // + link's 64 KiB
    CHECK(g_t->create(p, "big2", SCO_IPC_MAX_CHANNEL_BYTES - 65536, 1, 1, &big2) == SCO_OK);
    CHECK(g_t->create(p, "c0", 4096, 1, 1, &id) == SCO_TOO_MANY);
    CHECK(g_t->close(p, big1) == SCO_OK && g_t->close(p, big2) == SCO_OK);
    CHECK(g_t->create(q, "big1", SCO_IPC_MAX_CHANNEL_BYTES, 1, 1, &big1) == SCO_OK);   // beta's own quota
    CHECK(g_t->close(q, big1) == SCO_OK);

    // Close: the peer sees it at once; the id is gone.
    CHECK(g_t->close(p, ch) == SCO_OK);
    CHECK(g_t->close(p, ch) == SCO_NOT_FOUND);
    CHECK(g_t->view(p, ch, &other, &bytes) == SCO_NOT_FOUND);
    CHECK(sc_ipc_check(&peer) == SC_IPC_GONE);
    CHECK(sc_ipc_ring_push(&out, 1, "x", 1) == SC_IPC_GONE);
    // Re-created while the peer still maps the old one: its view is stale either way (Windows
    // re-opens the same section under a new epoch; POSIX unlinked it, so the old one stays closed).
    CHECK(g_t->create(p, "link", 65536, 42, 3, &ch) == SCO_OK);
    CHECK(sc_ipc_check(&peer) != SC_IPC_OK);
    PeerMap pm2;
    sc_ipc_chan peer2{};
    CHECK(pm2.Open(name) && sc_ipc_attach(pm2.base, pm2.bytes, 42, 3, &peer2) == SC_IPC_OK);

    // Unload: closed for the peer, the heartbeat stops, every call naming p is refused.
    CHECK(sco::Release(p) == Result::Ok);
    CHECK(sc_ipc_check(&peer2) == SC_IPC_GONE);
    pm2.Hdr()->owner_heartbeat_ms = 0;
    sco::ipc::Tick();
    CHECK(pm2.Hdr()->owner_heartbeat_ms == 0);
    CHECK(g_t->view(p, ch, &other, &bytes) == SCO_BAD_ARG);
    CHECK(g_t->create(p, "again", 65536, 1, 1, &id) == SCO_BAD_ARG);
    CHECK(g_t->close(p, ch) == SCO_BAD_ARG);
    CHECK(sco::Release(q) == Result::Ok);
}

// ---- scosdk/ipc.hpp -----------------------------------------------------------------------------

static void TestSdk() {
    sco_plugin* p = sco::host::NewPlugin("gamma");
    sco::sdk::Ipc ipc;
    CHECK(!ipc && ipc.Open(g_api, p) == SCO_OK && ipc);
    sco::sdk::IpcChannel bad = ipc.Create("Bad Name", 65536, 3, 1);
    CHECK(!bad && bad.Result() == SCO_BAD_ARG && bad.Write(64, Snap{}) == SCO_BAD_ARG);
    PeerMap pm;
    sc_ipc_chan peer{};
    {
        sco::sdk::IpcChannel link = ipc.Create("sdk", 65536, 3, 1);
        CHECK(link && link.Id() != 0 && link.Result() == SCO_OK);
        CHECK(link.InitRing(4096, 1024, SCO_IPC_TO_PEER) == SCO_OK);
        CHECK(link.InitRing(8192, 1024, SCO_IPC_FROM_PEER) == SCO_OK);
        CHECK(link.Push(4096, 5, Snap{ 1, 2, 3 }) == SCO_OK);
        CHECK(link.Write(128, Snap{ 4, 5, 6 }) == SCO_OK);
        Snap r{};
        CHECK(link.Read(128, r) == SCO_OK && r.b == 5);
        CHECK(link.View().size() == 65536);
        uint32_t ms = 0;
        CHECK(link.PeerAgeMs(ms) == SCO_NOT_FOUND);

        CHECK(pm.Open(sco::ipc::MappingName("gamma", "sdk")) && sc_ipc_attach(pm.base, pm.bytes, 3, 1, &peer) == SC_IPC_OK);
        sc_ipc_ring_view in{}, out{};
        CHECK(sc_ipc_ring_attach(&peer, 4096, &in) == SC_IPC_OK && sc_ipc_ring_attach(&peer, 8192, &out) == SC_IPC_OK);
        Snap got{};
        uint32_t type = 0;
        CHECK(Pop(&in, &type, &got, sizeof got) == SC_IPC_OK && type == 5 && got.c == 3);
        std::vector<uint8_t> big(300, 0x5A);
        CHECK(sc_ipc_ring_push(&out, 8, big.data(), static_cast<uint32_t>(big.size())) == SC_IPC_OK);
        std::vector<std::byte> msg;
        CHECK(link.Pop(8192, type, msg) == SCO_OK && type == 8 && msg.size() == 300 && msg[299] == std::byte{ 0x5A });
        CHECK(link.Pop(8192, type, msg) == SCO_NOT_FOUND && msg.empty());
        sco::sdk::IpcChannel moved = std::move(link);
        CHECK(!link && moved);
    }   // closed by the destructor
    CHECK(sc_ipc_check(&peer) == SC_IPC_GONE);
    CHECK(sco::Release(p) == Result::Ok);
}

// ---- the service under threads ------------------------------------------------------------------

static void TestServiceThreads() {
    sco_plugin* p = sco::host::NewPlugin("delta");
    uint64_t ch = 0;
    CHECK(g_t->create(p, "spin", 65536, 2, 1, &ch) == SCO_OK);
    CHECK(g_t->ring_init(p, ch, 4096, 4096, SCO_IPC_TO_PEER) == SCO_OK);
    void* base = nullptr;
    uint64_t bytes = 0;
    CHECK(g_t->view(p, ch, &base, &bytes) == SCO_OK);
    sc_ipc_chan peer{};
    sc_ipc_ring_view in{};
    CHECK(sc_ipc_attach(base, bytes, 2, 1, &peer) == SC_IPC_OK && sc_ipc_ring_attach(&peer, 4096, &in) == SC_IPC_OK);
    constexpr uint32_t kCount = 5000;
    std::atomic<int> bad{ 0 };
    std::atomic<bool> done{ false };
    std::thread producer([&] {
        for (uint32_t i = 0; i < kCount;) {
            const sco_result r = g_t->ring_push(p, ch, 4096, 1, &i, sizeof i);
            if (r == SCO_OK) ++i;
            else if (r == SCO_TOO_MANY && bad == 0) std::this_thread::yield();
            else { ++bad; return; }
        }
    });
    std::thread consumer([&] {   // the peer, on the same mapping
        for (uint32_t i = 0; i < kCount && bad == 0;) {
            uint32_t v = 0, type = 0, n = sizeof v;
            const int rc = sc_ipc_ring_pop(&in, &type, &v, &n);
            if (rc == SC_IPC_EMPTY) { std::this_thread::yield(); continue; }
            if (rc != SC_IPC_OK || v != i) { ++bad; return; }
            if (i % 64 == 0) sc_ipc_peer_beat(&peer, 1, NowMs());
            ++i;
        }
    });
    std::thread ticker([&] {
        while (!done) {
            sco::ipc::Tick();
            uint32_t age = 0;
            const sco_result r = g_t->peer_age_ms(p, ch, &age);
            if (r != SCO_OK && r != SCO_NOT_FOUND) ++bad;
            std::this_thread::yield();
        }
    });
    producer.join();
    consumer.join();
    done = true;
    ticker.join();
    CHECK(bad == 0);
    CHECK(sco::Release(p) == Result::Ok);
}

// Runs one section and says so, so a hang or a crash names its section in the log.
static void Run(const char* name, void (*fn)()) {
    const int before = g_fail.load();
    fn();
    std::printf("%s: %s\n", name, g_fail.load() == before ? "ok" : "FAILED");
    std::fflush(stdout);
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    Run("TestWireHeader", TestWireHeader);
    Run("TestWireBlocks", TestWireBlocks);
    Run("TestWireRing", TestWireRing);
    Run("TestWireHostile", TestWireHostile);
    Run("TestWireThreads", TestWireThreads);

    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "test_ipc" });
    sco::ipc::Options bad;
    bad.prefix = "Global\\SCO_";
    CHECK(sco::ipc::Start(bad) == Result::BadArg && !sco::ipc::Started());
    bad.prefix = std::string(sco::ipc::kPrefix) + "x/y";
    CHECK(sco::ipc::Start(bad) == Result::BadArg);
    sco::ipc::Options o;
    o.prefix = std::string(sco::ipc::kPrefix) + "t" + std::to_string(Pid()) + "_";
    CHECK(sco::ipc::Start(o) == Result::Ok && sco::ipc::Started());
    CHECK(sco::ipc::Start(o) == Result::BadArg);
    g_t = sco::ipc::Table();

    Run("TestService", TestService);
    Run("TestSdk", TestSdk);
    Run("TestServiceThreads", TestServiceThreads);

    sco_plugin* late = sco::host::NewPlugin("late");
    uint64_t ch = 0;
    CHECK(g_t->create(late, "x", 65536, 1, 1, &ch) == SCO_OK);
    sco::ipc::Stop();
    CHECK(!sco::ipc::Started());
    const void* table = nullptr;
    CHECK(g_api->query_service(SCO_IPC_NAME, SCO_IPC_VERSION_1_0, &table) == SCO_NOT_FOUND);
    CHECK(g_t->create(late, "y", 65536, 1, 1, &ch) == SCO_UNAVAILABLE);
    CHECK(g_t->close(late, ch) == SCO_UNAVAILABLE);
    sco::ipc::Stop();   // twice: a no-op
    CHECK(sco::Release(late) == Result::Ok);

    std::printf("sco-core ipc tests: %d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
