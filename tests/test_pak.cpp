// Unit tests for sco/game/pak.h: the CryPak adapter over a fake ICryPak (an object with a real
// vtable in its own page) and a fake DataCore loader (a detourable stub that calls a C++ body).
// The engine side calls through the vtable as the game does, so the swapped slots, the load
// window, the thread rule and the passthrough are the real adapter code.
// x86-64 Windows and Linux (the loader detour); elsewhere the tests are skipped.
//   tools/test.sh
#include "sco/caps.h"
#include "sco/game/pak.h"
#include "sco/hook.h"
#include "sco/log.h"
#include "sco/vfs.h"
#include <atomic>
#include <climits>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__APPLE__)
#define SCO_TEST_PAK 1
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

#if SCO_TEST_PAK
namespace pak = sco::game::pak;
namespace vfs = sco::vfs;
using vfs::Bytes;

static std::vector<std::string> g_lines;
static void Capture(const char* line) { g_lines.emplace_back(line); }
static bool Logged(const char* needle) {
    for (const auto& l : g_lines) if (l.find(needle) != std::string::npos) return true;
    return false;
}

static void* Page(bool exec) {
#if defined(_WIN32)
    return VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, exec ? PAGE_EXECUTE_READWRITE : PAGE_READWRITE);
#else
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | (exec ? PROT_EXEC : 0), MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : p;
#endif
}

template <class F> static void* Addr(F f) {
    static_assert(sizeof(F) == sizeof(void*), "function pointer size");
    void* p;
    std::memcpy(&p, &f, sizeof p);
    return p;
}
template <class F> static F As(void* p) {
    F f;
    std::memcpy(&f, &p, sizeof f);
    return f;
}

// ---- the fake engine: files, handles and CryPak's functions -------------------------------

constexpr uint64_t kBig = (3ull << 30) + 16;   // a 3 GiB file, generated, never allocated
static uint8_t BigByte(uint64_t p) { return static_cast<uint8_t>((p ^ (p >> 8) ^ (p >> 21)) * 31u); }

static Bytes g_small;   // data/game2.dcb and data/other.dcb
struct FakeFile { const Bytes* data; uint64_t size; uint64_t pos; };

static std::atomic<int> g_engineReads{ 0 }, g_taggedReads{ 0 }, g_opens{ 0 }, g_closes{ 0 };

static uintptr_t FakeOpen(uintptr_t, const char* path, const char*, uint32_t) {
    ++g_opens;
    const std::string p = path;
    if (p == "Data\\Game2.dcb" || p == "data/other.dcb" || p == "Data/Textures.pak" || p == "data/unmounted.dcb")
        return reinterpret_cast<uintptr_t>(new FakeFile{ &g_small, g_small.size(), 0 });
    if (p == "data/big.dcb") return reinterpret_cast<uintptr_t>(new FakeFile{ nullptr, kBig, 0 });
    return 0;
}
static size_t FakeRead(uintptr_t, void* data, size_t length, size_t elems, uintptr_t file, const void* tag) {
    ++g_engineReads;
    if (tag) ++g_taggedReads;
    FakeFile* f = reinterpret_cast<FakeFile*>(file);
    if (!f || !length) return 0;
    const uint64_t want = static_cast<uint64_t>(length) * elems;
    const uint64_t n = f->pos >= f->size ? 0 : (f->size - f->pos < want ? f->size - f->pos : want);
    uint8_t* out = static_cast<uint8_t*>(data);
    for (uint64_t i = 0; i < n; ++i) out[i] = f->data ? (*f->data)[f->pos + i] : BigByte(f->pos + i);
    f->pos += n;
    return static_cast<size_t>(n / length);
}
static int FakeSeek(uintptr_t, uintptr_t file, int offset, int mode) {   // 64-bit positions, like CryPak's
    FakeFile* f = reinterpret_cast<FakeFile*>(file);
    const int64_t base = mode == SEEK_SET ? 0 : mode == SEEK_CUR ? static_cast<int64_t>(f->pos) : static_cast<int64_t>(f->size);
    if (mode < SEEK_SET || mode > SEEK_END || base + offset < 0) return -1;
    f->pos = static_cast<uint64_t>(base + offset);
    return 0;
}
static int FakeClose(uintptr_t, uintptr_t file) {
    ++g_closes;
    delete reinterpret_cast<FakeFile*>(file);
    return 0;
}

struct FakePak { void** vt; };
static FakePak   g_pakObj;
static uintptr_t g_cryPak = 0;   // the ICryPak* global the loader reads

static void** Vt() { return g_pakObj.vt; }
static void* SlotNow(size_t off) { return Vt()[off / 8]; }
// The engine's calls: through the vtable, every time.
static uintptr_t EOpen(const char* path) {
    return As<pak::OpenFn>(SlotNow(pak::kOpenSlot))(reinterpret_cast<uintptr_t>(&g_pakObj), path, "rb", 0);
}
static size_t ERead(uintptr_t f, void* dst, size_t length, size_t elems) {
    return As<pak::ReadFn>(SlotNow(pak::kReadSlot))(reinterpret_cast<uintptr_t>(&g_pakObj), dst, length, elems, f, nullptr);
}
static int ESeek(uintptr_t f, int off, int mode) {
    return As<pak::SeekFn>(SlotNow(pak::kSeekSlot))(reinterpret_cast<uintptr_t>(&g_pakObj), f, off, mode);
}
static int EClose(uintptr_t f) {
    return As<pak::CloseFn>(SlotNow(pak::kCloseSlot))(reinterpret_cast<uintptr_t>(&g_pakObj), f);
}
static bool SlotsAreEngine() {
    return SlotNow(pak::kOpenSlot) == Addr(&FakeOpen) && SlotNow(pak::kReadSlot) == Addr(&FakeRead)
        && SlotNow(pak::kSeekSlot) == Addr(&FakeSeek) && SlotNow(pak::kCloseSlot) == Addr(&FakeClose);
}
static bool SlotsAreSwapped() {
    return SlotNow(pak::kOpenSlot) != Addr(&FakeOpen) && SlotNow(pak::kReadSlot) != Addr(&FakeRead)
        && SlotNow(pak::kSeekSlot) != Addr(&FakeSeek) && SlotNow(pak::kCloseSlot) != Addr(&FakeClose);
}

// ---- the fake loader ------------------------------------------------------------------------

// The loader body runs the current scenario on the loader's thread.
static std::function<void()> g_scenario;
static uintptr_t LoaderBody(uintptr_t, uintptr_t, uintptr_t, uintptr_t, uintptr_t) {
    if (g_scenario) g_scenario();
    return 1;
}

// The detourable entry: three `mov rax, rax` (StolenLength takes the first two), then an absolute
// jump to LoaderBody, so the trampoline and the detour run real code.
static uint8_t* MakeLoaderStub() {
    uint8_t* p = static_cast<uint8_t*>(Page(true));
    if (!p) return nullptr;
    static const uint8_t kMovs[] = { 0x48, 0x89, 0xC0, 0x48, 0x89, 0xC0, 0x48, 0x89, 0xC0 };
    std::memcpy(p, kMovs, sizeof kMovs);
    uint8_t* j = p + sizeof kMovs;
    j[0] = 0xFF; j[1] = 0x25;
    std::memset(j + 2, 0, 4);
    void* body = Addr(&LoaderBody);
    std::memcpy(j + 6, &body, 8);
    return p;
}

// Calls code we wrote. Clang's -fsanitize=function checks a type signature in front of every
// function called through a pointer, which hand-written code doesn't have.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static uintptr_t CallLoader(const void* loader) {
    return As<pak::LoaderFn>(const_cast<void*>(loader))(0, 0, 0, 0, 0);
}

// ---- mounts ---------------------------------------------------------------------------------

static std::shared_ptr<const vfs::Table> g_table;
static std::shared_ptr<const vfs::Table> CurrentMounts() { return g_table; }

static std::shared_ptr<const Bytes> B(std::initializer_list<uint8_t> b) { return std::make_shared<const Bytes>(b); }
static std::shared_ptr<const Bytes> Slice(const Bytes& from, size_t at, size_t n) {
    return std::make_shared<const Bytes>(from.begin() + static_cast<std::ptrdiff_t>(at), from.begin() + static_cast<std::ptrdiff_t>(at + n));
}

// Splices on data/game2.dcb: 4 bytes replaced at 100, 5 inserted at 5000.
static vfs::Mount GameMount(bool goodHeader) {
    vfs::Mount m;
    m.path = "data/game2.dcb";
    m.source = goodHeader ? "test" : "stale";
    vfs::SpliceList list;
    list.splices.push_back({ 100, 4, B({ 'A', 'B', 'C', 'D' }), Slice(g_small, 100, 4) });
    list.splices.push_back({ 5000, 0, B({ 1, 2, 3, 4, 5 }), nullptr });
    list.header = goodHeader ? Slice(g_small, 0, 16) : B({ 0xEE, 0xEE });
    m.producer = std::move(list);
    return m;
}
static Bytes Patched() {
    Bytes v(g_small.begin(), g_small.begin() + 100);
    v.insert(v.end(), { 'A', 'B', 'C', 'D' });
    v.insert(v.end(), g_small.begin() + 104, g_small.begin() + 5000);
    v.insert(v.end(), { 1, 2, 3, 4, 5 });
    v.insert(v.end(), g_small.begin() + 5000, g_small.end());
    return v;
}
// data/big.dcb: 4 bytes replaced at 2.5 GiB.
constexpr uint64_t kBigAt = (5ull << 29);
static vfs::Mount BigMount() {
    vfs::Mount m;
    m.path = "data/big.dcb";
    m.source = "big";
    m.producer = vfs::SpliceList{ { { kBigAt, 4, B({ 0xAA, 0xBB, 0xCC, 0xDD }), nullptr } }, nullptr };
    return m;
}
static void Publish(std::vector<vfs::Mount> mounts) { g_table = vfs::Table::Build(std::move(mounts)); }

static Bytes ReadAll(uintptr_t f, size_t chunk) {
    Bytes out;
    std::vector<uint8_t> buf(chunk);
    for (size_t got; (got = ERead(f, buf.data(), 1, chunk)) > 0;) out.insert(out.end(), buf.begin(), buf.begin() + static_cast<std::ptrdiff_t>(got));
    return out;
}

static pak::Targets g_targets;
static pak::Options Opts() {
    pak::Options o;
    o.mounts = &CurrentMounts;
    return o;
}

// ---- tests ----------------------------------------------------------------------------------

static void TestEnableRefusals() {
    pak::Options o = Opts();
    CHECK(pak::Enable(o) == sco::Result::Unavailable);   // no pak.* rows resolved in this process
    CHECK(!sco::caps::Has("vfs.pak"));
    o.size = 4;
    CHECK(pak::Enable(o, g_targets) == sco::Result::BadArg);
    o = Opts();
    o.scope = pak::Scope::AllFiles;
    CHECK(pak::Enable(o, g_targets) == sco::Result::Unavailable);
    o = Opts();
    o.mounts = nullptr;
    CHECK(pak::Enable(o, g_targets) == sco::Result::BadArg);
    sco::caps::Entry e[8];
    const size_t n = sco::caps::List(e, 8);
    bool reason = false;
    for (size_t i = 0; i < n && i < 8; ++i) reason |= !std::strcmp(e[i].name, "vfs.pak") && !e[i].ready && std::strstr(e[i].reason, "mount table");
    CHECK(reason);
    pak::Targets none;
    CHECK(pak::Enable(Opts(), none) == sco::Result::BadArg);
    CHECK(!pak::Enabled() && sco::hook::DetourCount() == 0 && SlotsAreEngine());
    CHECK(pak::LastLoad().outcome == pak::Outcome::None);
}

// The window: slots swapped only while the loader runs, a mount served on the tracked handle,
// other files, a second .dcb and a second thread untouched.
static void TestServedLoad() {
    Publish({ GameMount(true) });
    CHECK(pak::Enable(Opts(), g_targets) == sco::Result::Ok);
    CHECK(pak::Enabled() && sco::caps::Has("vfs.pak") && sco::hook::IsHooked(g_targets.loader));
    CHECK(pak::Enable(Opts(), g_targets) == sco::Result::BadArg);   // already enabled
    CHECK(SlotsAreEngine() && sco::hook::SlotCount() == 0);          // outside the window

    const Bytes want = Patched();
    bool ran = false;
    g_scenario = [&] {
        ran = true;
        CHECK(SlotsAreSwapped() && sco::hook::SlotCount() == 4);
        // Another file first: passes through.
        const uintptr_t tex = EOpen("Data/Textures.pak");
        uint8_t t[8];
        CHECK(ERead(tex, t, 1, 8) == 8 && std::memcmp(t, g_small.data(), 8) == 0);
        // The DataCore file: the virtual bytes, from position 0 without a seek.
        const int readsBefore = g_engineReads.load();
        const uintptr_t f = EOpen("Data\\Game2.dcb");
        CHECK(f != 0);
        CHECK(g_engineReads.load() > readsBefore);   // the size probe and the mount check read it
        uint8_t head[8];
        CHECK(ERead(f, head, 1, 8) == 8 && std::memcmp(head, want.data(), 8) == 0);
        CHECK(ESeek(f, 0, SEEK_SET) == 0);
        CHECK(ReadAll(f, 333) == want);
        // Seeks: SET, CUR, END, and the refusals the Reader makes.
        uint8_t q[6];
        CHECK(ESeek(f, 98, SEEK_SET) == 0 && ERead(f, q, 1, 6) == 6 && std::memcmp(q, want.data() + 98, 6) == 0);
        CHECK(ESeek(f, 4896, SEEK_CUR) == 0 && ERead(f, q, 1, 6) == 6 && std::memcmp(q, want.data() + 5000, 6) == 0);
        CHECK(ESeek(f, -6, SEEK_END) == 0 && ERead(f, q, 1, 6) == 6 && std::memcmp(q, want.data() + want.size() - 6, 6) == 0);
        CHECK(ESeek(f, -1, SEEK_SET) == -1 && ESeek(f, 0, 7) == -1);
        CHECK(ESeek(f, 10, SEEK_END) == 0 && ERead(f, q, 1, 6) == 0);   // past the end: reads 0
        // Element semantics: whole elements out.
        uint32_t words[3];
        CHECK(ESeek(f, -10, SEEK_END) == 0 && ERead(f, words, 4, 3) == 2);
        CHECK(ERead(f, words, 0, 3) == 0 && ERead(f, words, 4, 0) == 0);
        // A second .dcb on the loader thread: only the first is tracked.
        const uintptr_t other = EOpen("data/other.dcb");
        CHECK(ReadAll(other, 4096) == g_small);
        CHECK(EClose(other) == 0);
        // A second thread opens the same file and reads it, and reads another file, during the
        // window: the engine's bytes. It starts after the swap and is joined before the loader
        // returns (std::thread construction and join order it with the loader thread).
        bool threadOk = false;
        std::thread t2([&] {
            const uintptr_t g = EOpen("Data\\Game2.dcb");
            const uintptr_t x = EOpen("Data/Textures.pak");
            threadOk = g && x && ReadAll(g, 1000) == g_small && ReadAll(x, 777) == g_small;
            EClose(g);
            EClose(x);
        });
        t2.join();
        CHECK(threadOk);
        // The tracked handle still serves the virtual file after all that.
        CHECK(ESeek(f, 100, SEEK_SET) == 0 && ERead(f, q, 1, 4) == 4 && std::memcmp(q, "ABCD", 4) == 0);
        CHECK(EClose(f) == 0 && EClose(tex) == 0);
    };
    g_lines.clear();
    CHECK(CallLoader(g_targets.loader) == 1 && ran);
    CHECK(SlotsAreEngine() && sco::hook::SlotCount() == 0);   // the window closed
    const pak::LoadReport r = pak::LastLoad();
    CHECK(r.outcome == pak::Outcome::Applied && r.loaderOk && r.path == "Data\\Game2.dcb");
    CHECK(r.baseSize == g_small.size() && r.size == g_small.size() + 5);
    CHECK(Logged("[pak] Data\\Game2.dcb served from its mount (10000 -> 10005 bytes), load ok"));
    CHECK(std::strcmp(pak::OutcomeName(r.outcome), "applied") == 0);
    CHECK(g_taggedReads.load() > 0);   // the adapter's own reads pass the loader's tag

    // Outside the window the engine's functions are called directly, whatever the thread.
    const uintptr_t f = EOpen("Data\\Game2.dcb");
    CHECK(ReadAll(f, 4096) == g_small);
    EClose(f);
}

// A mount whose expected bytes don't match: inert, the file passes through from position 0
// (the adapter seeks back after the probe and the mount check). An unmounted .dcb: same.
static void TestPassThrough() {
    Publish({ GameMount(false) });
    g_scenario = [] {
        const uintptr_t f = EOpen("Data\\Game2.dcb");
        uint8_t head[16];
        CHECK(ERead(f, head, 1, 16) == 16 && std::memcmp(head, g_small.data(), 16) == 0);   // seek-back after Open
        CHECK(ESeek(f, 0, SEEK_SET) == 0 && ReadAll(f, 512) == g_small);
        CHECK(ESeek(f, -4, SEEK_END) == 0);   // the engine's own seek: the base size
        EClose(f);
    };
    CHECK(CallLoader(g_targets.loader) == 1);
    pak::LoadReport r = pak::LastLoad();
    CHECK(r.outcome == pak::Outcome::Passed && r.baseSize == g_small.size());
    CHECK(r.reason.find("stale") != std::string::npos);   // names the inert mount
    CHECK(SlotsAreEngine());

    Publish({ GameMount(true) });
    g_scenario = [] {
        const uintptr_t f = EOpen("data/unmounted.dcb");
        CHECK(ReadAll(f, 999) == g_small);
        EClose(f);
    };
    CHECK(CallLoader(g_targets.loader) == 1);
    r = pak::LastLoad();
    CHECK(r.outcome == pak::Outcome::Passed && r.reason == "not mounted" && r.path == "data/unmounted.dcb");

    g_scenario = [] {
        const uintptr_t f = EOpen("Data/Textures.pak");
        EClose(f);
    };
    CHECK(CallLoader(g_targets.loader) == 1);
    CHECK(pak::LastLoad().outcome == pak::Outcome::NoDcb);

    const uintptr_t saved = g_cryPak;
    g_cryPak = 0;
    bool swapped = true;
    g_scenario = [&] { swapped = !SlotsAreEngine(); };
    CHECK(CallLoader(g_targets.loader) == 1 && !swapped);
    CHECK(pak::LastLoad().outcome == pak::Outcome::NoCryPak);
    g_cryPak = saved;
}

// Positions past 2 GiB: the engine's seek takes an int, so the engine reaches them with SEEK_SET
// INT_MAX plus SEEK_CUR steps, and the adapter reads the base the same way.
static void TestBigFile() {
    Publish({ BigMount() });
    g_scenario = [] {
        const uintptr_t f = EOpen("data/big.dcb");
        uint8_t q[8];
        CHECK(ESeek(f, INT_MAX, SEEK_SET) == 0);
        CHECK(ESeek(f, static_cast<int>(kBigAt - 4 - INT_MAX), SEEK_CUR) == 0);
        CHECK(ERead(f, q, 1, 8) == 8);
        const uint8_t want[8] = { BigByte(kBigAt - 4), BigByte(kBigAt - 3), BigByte(kBigAt - 2), BigByte(kBigAt - 1),
                                  0xAA, 0xBB, 0xCC, 0xDD };
        CHECK(std::memcmp(q, want, 8) == 0);
        CHECK(ERead(f, q, 1, 2) == 2 && q[0] == BigByte(kBigAt + 4) && q[1] == BigByte(kBigAt + 5));
        CHECK(ESeek(f, -3, SEEK_END) == 0 && ERead(f, q, 1, 8) == 3);
        CHECK(q[0] == BigByte(kBig - 3) && q[2] == BigByte(kBig - 1));
        EClose(f);
    };
    CHECK(CallLoader(g_targets.loader) == 1);
    const pak::LoadReport r = pak::LastLoad();
    CHECK(r.outcome == pak::Outcome::Applied && r.baseSize == kBig && r.size == kBig);
}

// Disable: restores the detour, idempotent; mid-load it leaves the window to close itself.
static void TestDisable() {
    Publish({ GameMount(true) });
    bool inside = false;
    g_scenario = [&] {
        pak::Disable();   // from inside the window
        inside = SlotsAreSwapped() && !pak::Enabled();
        const uintptr_t f = EOpen("Data\\Game2.dcb");
        CHECK(ReadAll(f, 4096) == Patched());   // this load is still served whole
        EClose(f);
    };
    CHECK(CallLoader(g_targets.loader) == 1 && inside);
    CHECK(SlotsAreEngine() && sco::hook::SlotCount() == 0 && !sco::hook::IsHooked(g_targets.loader));
    CHECK(!sco::caps::Has("vfs.pak"));
    pak::Disable();   // idempotent
    CHECK(!pak::Enabled());

    // Disabled: the next load is the engine's own.
    bool swapped = true;
    g_scenario = [&] { swapped = !SlotsAreEngine(); };
    const pak::LoadReport before = pak::LastLoad();
    CHECK(CallLoader(g_targets.loader) == 1 && !swapped);
    CHECK(pak::LastLoad().outcome == before.outcome);

    // Enable again after Disable.
    CHECK(pak::Enable(Opts(), g_targets) == sco::Result::Ok);
    g_scenario = [] { EClose(EOpen("Data\\Game2.dcb")); };
    CHECK(CallLoader(g_targets.loader) == 1 && pak::LastLoad().outcome == pak::Outcome::Applied);
    pak::Disable();
    CHECK(sco::hook::DetourCount() == 0 && sco::hook::SlotCount() == 0 && SlotsAreEngine());
}
#endif

int main() {
#if SCO_TEST_PAK
    sco::SetLogSink(Capture);
    g_small.resize(10000);
    for (size_t i = 0; i < g_small.size(); ++i) g_small[i] = static_cast<uint8_t>(i * 7 + (i >> 8));
    void** vt = static_cast<void**>(Page(false));
    uint8_t* stub = MakeLoaderStub();
    CHECK(vt && stub);
    if (!vt || !stub) return 1;
    vt[pak::kOpenSlot / 8] = Addr(&FakeOpen);
    vt[pak::kReadSlot / 8] = Addr(&FakeRead);
    vt[pak::kSeekSlot / 8] = Addr(&FakeSeek);
    vt[pak::kCloseSlot / 8] = Addr(&FakeClose);
    g_pakObj.vt = vt;
    g_cryPak = reinterpret_cast<uintptr_t>(&g_pakObj);
    g_targets.loader = stub;
    g_targets.cryPak = &g_cryPak;

    TestEnableRefusals();
    TestServedLoad();
    TestPassThrough();
    TestBigFile();
    TestDisable();
    CHECK(g_opens.load() == g_closes.load());   // every handle the tests opened was closed
    std::printf("sco-core pak tests: %d passed, %d failed\n", g_pass, g_fail);
#else
    std::printf("sco-core pak tests: skipped (x86-64 Windows/Linux only)\n");
#endif
    return g_fail ? 1 : 0;
}
