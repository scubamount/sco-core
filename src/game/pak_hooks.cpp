// sco/game/pak.h: the CryPak adapter. The loader detour opens a window (Scope::DataCoreLoad) in
// which four ICryPak vtable slots point here; the first .dcb the loader's thread opens is served
// through sco::vfs, everything else goes to the engine's own functions. Generalizes sc-offline's
// quantum.cpp:82-150 (LoadDataCoreHook, SwapPakSlots, PakOpen/Read/Seek/CloseHook).
//
// Not in sco_core (sco-sigcheck and the signature tests don't need hooks): library sco_pak.
#include "sco/caps.h"
#include "sco/game/pak.h"
#include "sco/hook.h"
#include "sco/log.h"
#include <atomic>
#include <chrono>
#include <climits>
#include <cstdio>
#include <cstring>
#include <exception>
#include <mutex>
#include <thread>
#if defined(_MSC_VER)
#include <excpt.h>    // __try / __except, GetExceptionCode
#include <malloc.h>   // _resetstkoflw
#endif

namespace sco::game::pak {

namespace {

// ---- state --------------------------------------------------------------------------------

// The engine's functions, from SwapSlot. Written by the loader thread before the slots point
// here; read by any thread that calls through the slots.
std::atomic<OpenFn>  g_open{ nullptr };
std::atomic<ReadFn>  g_read{ nullptr };
std::atomic<SeekFn>  g_seek{ nullptr };
std::atomic<CloseFn> g_close{ nullptr };

std::mutex        g_mu;              // everything below except the load-window atomics
bool              g_enabled = false;
Options           g_options;
Targets           g_targets;
LoaderFn          g_loaderOrig = nullptr;
hook::Transaction g_slots;           // the four swaps of the current load window
bool              g_swapped = false;
LoadReport        g_last;

std::atomic<bool>            g_inLoad{ false };     // a load window is open
std::atomic<std::thread::id> g_loaderThread{};      // its thread
std::atomic<uintptr_t>       g_tracked{ 0 };        // the tracked handle, 0 = none
std::atomic<bool>            g_baseMoved{ false };  // another thread moved the tracked handle

// Loader thread only (set in the open hook, used by read/seek, cleared at the end of the load).
vfs::Reader g_reader;
bool        g_served = false;        // g_reader serves the tracked handle
std::shared_ptr<const vfs::Table> (*g_loadMounts)() = nullptr;   // Options.mounts of this load
void (*g_loadOnLoad)(const LoadReport&) = nullptr;                // Options.onLoad of this load
LoadReport  g_load;                  // the report being built

bool OnLoaderThread() {
    return g_inLoad.load(std::memory_order_acquire) && g_loaderThread.load(std::memory_order_acquire) == std::this_thread::get_id();
}

// The tag argument the engine's read takes, for the reads the adapter makes itself (quantum.cpp
// ReadTag): a pointer to a pointer to the loader's name.
const void* AdapterTag() {
    static const char* const kName = "bool __cdecl CDataCoreLoader::InitializeBinary(const class CryStringT<char> &,bool)";
    return &kName;
}

// ---- the real file under the tracked handle -----------------------------------------------

// The engine's read and seek for one handle. Seek narrows the 64-bit position to the engine's
// int offset: SEEK_SET up to INT_MAX, then SEEK_CUR steps of at most INT_MAX.
class PakBase final : public vfs::BaseIo {
public:
    PakBase(uintptr_t pak, uintptr_t file, const void* tag) : pak_(pak), file_(file), tag_(tag) {}
    bool Seek(uint64_t pos) override {
        const SeekFn seek = g_seek.load(std::memory_order_acquire);
        const int first = pos > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(pos);
        if (seek(pak_, file_, first, SEEK_SET) != 0) return false;
        for (uint64_t left = pos - static_cast<uint64_t>(first); left;) {
            const int step = left > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(left);
            if (seek(pak_, file_, step, SEEK_CUR) != 0) return false;
            left -= static_cast<uint64_t>(step);
        }
        return true;
    }
    size_t Read(void* dst, size_t n) override {
        return g_read.load(std::memory_order_acquire)(pak_, dst, 1, n, file_, tag_);
    }

private:
    uintptr_t   pak_, file_;
    const void* tag_;
};

// The real file's size. CryPak's size and tell slots aren't pinned by a row yet (plan PR 10), so
// it is probed with seek + a one-byte read: doubling from 4 KiB, then bisecting, about
// 2 x log2(size) probes. Positions past the end seek fine and read 0 bytes (C fseek semantics).
bool ProbeSize(vfs::BaseIo& base, uint64_t& size) {
    const auto readable = [&](uint64_t pos) {
        uint8_t b;
        return base.Seek(pos) && base.Read(&b, 1) == 1;
    };
    if (!readable(0)) { size = 0; return base.Seek(0); }
    uint64_t lo = 0, hi = 4096;   // lo readable; hi: first candidate past the end
    while (readable(hi)) {
        lo = hi;
        if (hi > vfs::kMaxFileSize) return false;   // bigger than any virtual file may be
        hi *= 2;
    }
    while (hi - lo > 1) {   // invariant: lo readable, hi not
        const uint64_t mid = lo + (hi - lo) / 2;
        (readable(mid) ? lo : hi) = mid;
    }
    size = hi;
    return true;
}

bool EndsWithDcb(const char* path) {
    const size_t n = path ? std::strlen(path) : 0;
    if (n <= 4) return false;
    const char* e = path + n - 4;
    return e[0] == '.' && (e[1] | 0x20) == 'd' && (e[2] | 0x20) == 'c' && (e[3] | 0x20) == 'b';
}

// The first .dcb the loader thread opens: compose it from the mount, or let it pass.
void Track(uintptr_t pak, uintptr_t file, const char* path) {
    g_tracked.store(file, std::memory_order_release);
    g_baseMoved.store(false, std::memory_order_relaxed);
    g_reader = vfs::Reader();
    g_served = false;
    g_load.outcome = Outcome::Passed;
    try {
        g_load.path = path;
        const std::shared_ptr<const vfs::Table> table = g_loadMounts ? g_loadMounts() : nullptr;
        if (!table || !table->Mounted(path)) { g_load.reason = "not mounted"; return; }
        PakBase base(pak, file, AdapterTag());
        uint64_t baseSize = 0;
        if (!ProbeSize(base, baseSize)) g_load.reason = "the file's size couldn't be read";
        else if (std::shared_ptr<const vfs::Composed> c = table->Open(path, base, baseSize)) {
            g_reader = vfs::Reader(c);
            g_served = true;
            g_load.outcome = Outcome::Applied;
            g_load.baseSize = c->baseSize;
            g_load.size = c->size;
        } else {
            g_load.baseSize = baseSize;
            g_load.reason = "every mount of it is inert";
            const std::string norm = vfs::NormalizePath(path);
            for (const vfs::MountInfo& m : table->Mounts())
                if (m.path == norm && !m.reason.empty()) { g_load.reason = m.source + ": " + m.reason; break; }
        }
        // The probe and Table::Open moved the real handle. The engine expects a fresh handle at 0:
        // a passed-through file reads from there, and a served one doesn't care (its Reader
        // seeks the base before the first base read).
        if (!g_served && !base.Seek(0)) {
            g_load.reason += "; seeking the file back to 0 failed";
            Log("[pak] %s: seeking back to 0 after the mount check failed; the load may fail", path);
        }
    } catch (const std::exception& e) {   // an allocation in the report: the file passes through
        g_reader = vfs::Reader();
        g_served = false;
        g_load.outcome = Outcome::Passed;
        Log("[pak] mount check failed (%s); the file passes through", e.what());
    }
}

// The tracked handle is redirected only on the loader thread. Another thread using it moves the
// real position, so the reader forgets where the base is (it seeks before its next base read).
bool Redirected(uintptr_t file) {
    if (!file || file != g_tracked.load(std::memory_order_acquire)) return false;
    if (!OnLoaderThread()) { g_baseMoved.store(true, std::memory_order_release); return false; }
    if (!g_served) return false;
    if (g_baseMoved.exchange(false, std::memory_order_acq_rel)) {
        const uint64_t pos = g_reader.Tell();
        g_reader = vfs::Reader(g_reader.File());
        g_reader.Seek(static_cast<int64_t>(pos), vfs::Whence::Set);
    }
    return true;
}

// ---- the four slots -----------------------------------------------------------------------

uintptr_t OpenHook(uintptr_t pak, const char* path, const char* mode, uint32_t flags) {
    const uintptr_t file = g_open.load(std::memory_order_acquire)(pak, path, mode, flags);
    if (file && OnLoaderThread() && !g_tracked.load(std::memory_order_acquire) && EndsWithDcb(path))
        Track(pak, file, path);
    return file;
}

size_t ReadHook(uintptr_t pak, void* data, size_t length, size_t elems, uintptr_t file, const void* tag) {
    if (!Redirected(file)) return g_read.load(std::memory_order_acquire)(pak, data, length, elems, file, tag);
    if (!length || !elems) return 0;
    if (elems > SIZE_MAX / length) return 0;   // the byte count doesn't fit: nothing read
    PakBase base(pak, file, tag ? tag : AdapterTag());
    return g_reader.Read(data, length * elems, base) / length;
}

int SeekHook(uintptr_t pak, uintptr_t file, int offset, int mode) {
    if (!Redirected(file)) return g_seek.load(std::memory_order_acquire)(pak, file, offset, mode);
    const vfs::Whence w = mode == SEEK_SET ? vfs::Whence::Set : mode == SEEK_CUR ? vfs::Whence::Cur : vfs::Whence::End;
    if (mode != SEEK_SET && mode != SEEK_CUR && mode != SEEK_END) return -1;
    return g_reader.Seek(offset, w) ? 0 : -1;
}

int CloseHook(uintptr_t pak, uintptr_t file) {
    uintptr_t tracked = file;
    if (file) g_tracked.compare_exchange_strong(tracked, 0, std::memory_order_acq_rel);   // forget it, from any thread
    return g_close.load(std::memory_order_acquire)(pak, file);
}

template <class F> void* Code(F f) {
    static_assert(sizeof(F) == sizeof(void*), "function pointer size");
    void* p;
    std::memcpy(&p, &f, sizeof p);
    return p;
}

// ---- the load window ----------------------------------------------------------------------

// Swaps the four slots of the current ICryPak's vtable (g_mu held).
void SwapSlots(uintptr_t pak) {
    void** vt = nullptr;
    std::memcpy(&vt, reinterpret_cast<const void*>(pak), sizeof vt);
    void* orig[4] = {};
    g_slots.AddSlot(vt + kOpenSlot / 8, Code(&OpenHook), &orig[0]);
    g_slots.AddSlot(vt + kReadSlot / 8, Code(&ReadHook), &orig[1]);
    g_slots.AddSlot(vt + kSeekSlot / 8, Code(&SeekHook), &orig[2]);
    g_slots.AddSlot(vt + kCloseSlot / 8, Code(&CloseHook), &orig[3]);
    // SwapSlot sets each original before its slot changes, but the hooks read the atomics: store
    // them from the vtable first, so a call through a just-swapped slot finds them.
    OpenFn o; ReadFn r; SeekFn s; CloseFn c;
    std::memcpy(&o, vt + kOpenSlot / 8, sizeof o);
    std::memcpy(&r, vt + kReadSlot / 8, sizeof r);
    std::memcpy(&s, vt + kSeekSlot / 8, sizeof s);
    std::memcpy(&c, vt + kCloseSlot / 8, sizeof c);
    g_open.store(o, std::memory_order_release);
    g_read.store(r, std::memory_order_release);
    g_seek.store(s, std::memory_order_release);
    g_close.store(c, std::memory_order_release);
    const hook::Error e = g_slots.Commit();
    g_swapped = e == hook::Error::None;
    if (!g_swapped) g_load.reason = std::string("slot swap refused: ") + hook::ErrorName(e);
}

void LogLoad(const LoadReport& r) {
    const char* ok = r.loaderOk ? "ok" : "FAILED";
    switch (r.outcome) {
    case Outcome::Applied:
        Log("[+] [pak] %s served from its mount (%llu -> %llu bytes), load %s", r.path.c_str(),
            static_cast<unsigned long long>(r.baseSize), static_cast<unsigned long long>(r.size), ok);
        break;
    case Outcome::Passed:
        Log("[pak] %s passed through (%s), load %s", r.path.c_str(), r.reason.c_str(), ok);
        break;
    case Outcome::NoDcb:      Log("[pak] the DataCore loader opened no .dcb, load %s", ok); break;
    case Outcome::NoCryPak:   Log("[!] [pak] CryPak not found at load time; game data loaded %s unchanged", ok); break;
    case Outcome::SwapFailed: Log("[!] [pak] %s; game data loaded %s unchanged", r.reason.c_str(), ok); break;
    case Outcome::None: break;
    }
}

// Opens the load window on this thread (g_mu held).
void OpenWindow() {
    g_load = LoadReport();
    g_load.outcome = Outcome::NoDcb;
    g_loadMounts = g_options.mounts;
    g_loadOnLoad = g_options.onLoad;
    g_tracked.store(0, std::memory_order_release);
    g_loaderThread.store(std::this_thread::get_id(), std::memory_order_release);
    g_inLoad.store(true, std::memory_order_release);
    const uintptr_t pak = g_targets.cryPak ? *g_targets.cryPak : 0;
    if (!pak) g_load.outcome = Outcome::NoCryPak;
    else {
        SwapSlots(pak);
        if (!g_swapped) g_load.outcome = Outcome::SwapFailed;
    }
}

// Options.onLoad, with no lock held. An escaping C++ exception is caught here; on MSVC a
// structured exception is caught by GuardedOnLoad (no C++ objects in it: C2712).
struct OnLoadCall {
    void (*fn)(const LoadReport&);
    const LoadReport* report;
};

void OnLoadThunk(void* p) noexcept {
    const OnLoadCall& c = *static_cast<const OnLoadCall*>(p);
    try {
        c.fn(*c.report);
    } catch (const std::exception& e) {
        Log("[!] [pak] Options.onLoad threw (%s); the load's result is unchanged", e.what());
    } catch (...) {
        Log("[!] [pak] Options.onLoad threw; the load's result is unchanged");
    }
}

#if defined(_MSC_VER)
unsigned long GuardedOnLoad(void* p) {
    unsigned long code = 0;
    __try {
        OnLoadThunk(p);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        if (code == 0xC00000FDul) _resetstkoflw();   // EXCEPTION_STACK_OVERFLOW: restore the guard page
        return code ? code : 0xFFFFFFFFul;
    }
    return 0;
}
#endif

void CallOnLoad(void (*fn)(const LoadReport&), const LoadReport& report) {
    OnLoadCall c{ fn, &report };
#if defined(_MSC_VER)
    if (const unsigned long code = GuardedOnLoad(&c))
        Log("[!] [pak] Options.onLoad crashed (exception 0x%08lX); the load's result is unchanged", code);
#else
    OnLoadThunk(&c);
#endif
}

// Calls the loader through its detour trampoline, which has no type signature in front of it for
// clang's -fsanitize=function (sanitizer builds of the tests) to check.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
uintptr_t LoaderHook(uintptr_t loader, uintptr_t path, uintptr_t a3, uintptr_t a4, uintptr_t a5) {
    LoaderFn orig;
    bool window;
    {
        std::lock_guard<std::mutex> hold(g_mu);
        orig = g_loaderOrig;
        // A second load while one runs (or a call racing Disable): the engine's own, no window.
        window = g_enabled && !g_inLoad.load(std::memory_order_acquire);
        if (window) OpenWindow();
    }
    if (!window) return orig(loader, path, a3, a4, a5);
    const auto start = std::chrono::steady_clock::now();
    const uintptr_t ok = orig(loader, path, a3, a4, a5);
    const auto took = std::chrono::steady_clock::now() - start;
    LoadReport report;
    void (*onLoad)(const LoadReport&) = nullptr;
    {
        std::lock_guard<std::mutex> hold(g_mu);
        if (g_swapped) {
            const hook::Error e = g_slots.Rollback();
            if (e != hook::Error::None) Log("[!] [pak] restoring the CryPak slots: %s", hook::ErrorName(e));
            g_swapped = false;
        }
        g_inLoad.store(false, std::memory_order_release);
        g_loaderThread.store(std::thread::id(), std::memory_order_release);
        g_tracked.store(0, std::memory_order_release);
        g_reader = vfs::Reader();
        g_served = false;
        g_load.loaderOk = (ok & 0xFF) != 0;
        g_load.durationMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(took).count());
        g_last = g_load;
        LogLoad(g_last);
        onLoad = g_loadOnLoad;
        g_loadOnLoad = nullptr;
        if (onLoad) report = g_last;
    }
    if (onLoad) CallOnLoad(onLoad, report);   // no lock held: it may call LastLoad and the rest
    return ok;
}

Result Refuse(Result r, const char* reason) {
    caps::Set("vfs.pak", false, reason);
    Log("[!] [pak] not enabled: %s", reason);
    return r;
}

}  // namespace

const char* OutcomeName(Outcome o) {
    switch (o) {
    case Outcome::None:       return "none";
    case Outcome::Applied:    return "applied";
    case Outcome::Passed:     return "passed";
    case Outcome::NoDcb:      return "no_dcb";
    case Outcome::NoCryPak:   return "no_crypak";
    case Outcome::SwapFailed: return "swap_failed";
    }
    return "?";
}

Result Enable(const Options& options) {
    Targets t;
    if (!Resolve(t)) {
        static const char* const kRows[] = { "pak.datacore_loader", "pak.crypak", "pak.slots" };
        caps::SetFromSignatures("vfs.pak", kRows, 3);
        Log("[!] [pak] not enabled: a pak.* signature row isn't OK");
        return Result::Unavailable;
    }
    return Enable(options, t);
}

Result Enable(const Options& options, const Targets& targets) {
    if (options.size != sizeof(Options)) return Refuse(Result::BadArg, "Options.size mismatch");
    if (options.scope != Scope::DataCoreLoad) return Refuse(Result::Unavailable, "Scope::AllFiles arrives with persistent hooks (plan PR 10)");
    if (!options.mounts) return Refuse(Result::BadArg, "no mount table (Options.mounts)");
    if (!targets.loader || !targets.cryPak) return Refuse(Result::BadArg, "no loader or CryPak global");
    std::lock_guard<std::mutex> hold(g_mu);
    if (g_enabled) return Result::BadArg;   // vfs.pak stays as it is
    void* orig = nullptr;
    const hook::Error e = hook::InstallDetour(targets.loader, 0, Code(&LoaderHook), &orig);
    if (e != hook::Error::None) {
        char reason[64];
        std::snprintf(reason, sizeof reason, "loader detour refused: %s", hook::ErrorName(e));
        return Refuse(Result::Failed, reason);
    }
    std::memcpy(&g_loaderOrig, &orig, sizeof orig);
    g_options = options;
    g_targets = targets;
    g_last = LoadReport();
    g_enabled = true;
    caps::Set("vfs.pak", true);
    Log("[+] [pak] DataCore loader hooked (CryPak open/read/seek/close swapped while it runs)");
    return Result::Ok;
}

void Disable() {
    std::lock_guard<std::mutex> hold(g_mu);
    if (!g_enabled) return;
    const hook::Error e = hook::RemoveDetour(g_targets.loader);
    if (e != hook::Error::None && e != hook::Error::NotHooked)
        Log("[!] [pak] removing the loader detour: %s", hook::ErrorName(e));
    // A load in progress restores its slots when it returns (LoaderHook); none is left otherwise.
    g_enabled = false;
    caps::Set("vfs.pak", false, "disabled");
}

bool Enabled() {
    std::lock_guard<std::mutex> hold(g_mu);
    return g_enabled;
}

LoadReport LastLoad() {
    std::lock_guard<std::mutex> hold(g_mu);
    return g_last;
}

}  // namespace sco::game::pak
