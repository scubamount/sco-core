// sco/hook.h: near caves, code writes and the detour registry.
#include "sco/hook.h"
#include <cstring>
#include <mutex>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <sys/mman.h>
#include <unistd.h>
#endif

#if defined(__x86_64__) || defined(_M_X64)
#define SCO_HOOK_X64 1
#else
#define SCO_HOOK_X64 0
#endif

namespace sco::hook {

namespace {

struct Cave { uint8_t* base; uint8_t* used; uint8_t* end; };
struct Detour { uint8_t* target; size_t stolen; uint8_t saved[kMaxStolen]; };

std::mutex          g_lock;     // caves and detours
std::vector<Cave>   g_caves;
std::vector<Detour> g_detours;
thread_local uint32_t g_osError = 0;

constexpr int64_t kReach = 0x7FFF0000;   // keep every byte of a cave inside rel32 range

bool InReach(const uint8_t* a, const uint8_t* lo, const uint8_t* hi) {
    const int64_t d1 = reinterpret_cast<int64_t>(lo) - reinterpret_cast<int64_t>(a);
    const int64_t d2 = reinterpret_cast<int64_t>(hi) - reinterpret_cast<int64_t>(a);
    return d1 > -kReach && d1 < kReach && d2 > -kReach && d2 < kReach;
}

uint8_t* MapAt(uintptr_t at) {
#if defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), kCaveBytes, MEM_RESERVE | MEM_COMMIT,
                                              PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(reinterpret_cast<void*>(at), kCaveBytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

void Unmap(uint8_t* p) {
#if defined(_WIN32)
    VirtualFree(p, 0, MEM_RELEASE);
#else
    munmap(p, kCaveBytes);
#endif
}

// A new cave in reach of anchor: tries addresses outward from it, 64 KiB apart (the Windows
// allocation granularity). On POSIX the address is a hint; a mapping placed elsewhere is
// released and the search goes on.
Cave* NewCave(const uint8_t* anchor) {   // g_lock held
    const uintptr_t a = reinterpret_cast<uintptr_t>(anchor) & ~static_cast<uintptr_t>(kCaveBytes - 1);
    for (uintptr_t d = kCaveBytes; d < static_cast<uintptr_t>(kReach) - 2 * kCaveBytes; d += kCaveBytes) {
        for (int side = 0; side < 2; ++side) {
            if (side == 0 && d > a) continue;
            const uintptr_t at = side == 0 ? a - d : a + d;
            uint8_t* p = MapAt(at);
            if (!p) continue;
            if (!InReach(anchor, p, p + kCaveBytes)) { Unmap(p); continue; }
            g_caves.push_back({ p, p, p + kCaveBytes });
            return &g_caves.back();
        }
    }
    return nullptr;
}

uint8_t* CarveNear(const uint8_t* anchor, size_t n) {   // g_lock held
    if (!n || n > kCaveBytes) return nullptr;
    const size_t need = (n + 15) & ~static_cast<size_t>(15);
    for (Cave& c : g_caves)
        if (static_cast<size_t>(c.end - c.used) >= need && InReach(anchor, c.base, c.end)) {
            uint8_t* p = c.used;
            c.used += need;
            return p;
        }
    Cave* c = NewCave(anchor);
    if (!c) return nullptr;
    uint8_t* p = c->used;
    c->used += need;
    return p;
}

Detour* Find(const void* target) {   // g_lock held
    for (Detour& d : g_detours)
        if (d.target == target) return &d;
    return nullptr;
}

void PutAbsJump(uint8_t* at, const void* to) {   // FF 25 00000000 <addr64>: jmp [rip+0]
    at[0] = 0xFF; at[1] = 0x25;
    std::memset(at + 2, 0, 4);
    std::memcpy(at + 6, &to, 8);
}

}  // namespace

const char* ErrorName(Error e) {
    switch (e) {
    case Error::None:          return "none";
    case Error::BadArg:        return "bad_arg";
    case Error::AlreadyHooked: return "already_hooked";
    case Error::NoCave:        return "no_cave";
    case Error::Protect:       return "protect";
    case Error::NotHooked:     return "not_hooked";
    case Error::Unsupported:   return "unsupported";
    }
    return "?";
}

uint32_t LastOsError() { return g_osError; }

uint8_t* AllocateNear(const void* anchor, size_t n) {
    if (!anchor) return nullptr;
    std::lock_guard<std::mutex> hold(g_lock);
    return CarveNear(static_cast<const uint8_t*>(anchor), n);
}

bool WriteCode(void* at, const void* bytes, size_t n) {
    if (!at || !bytes || !n) return false;
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old)) { g_osError = GetLastError(); return false; }
    std::memcpy(at, bytes, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t lo = reinterpret_cast<uintptr_t>(at) & ~(page - 1);
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(at) + n + page - 1) & ~(page - 1);
    void* start = reinterpret_cast<void*>(lo);
    if (mprotect(start, hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        g_osError = static_cast<uint32_t>(errno);
        return false;
    }
    std::memcpy(at, bytes, n);
    // POSIX can't read a page's old protection back; code pages go back to read + execute.
    mprotect(start, hi - lo, PROT_READ | PROT_EXEC);
    __builtin___clear_cache(static_cast<char*>(at), static_cast<char*>(at) + n);
#endif
    return true;
}

Error InstallDetour(void* target, size_t stolen, void* detour, void** original) {
#if !SCO_HOOK_X64
    (void)target; (void)stolen; (void)detour; (void)original;
    return Error::Unsupported;
#else
    if (!target || !detour || !original || stolen < kMinStolen || stolen > kMaxStolen) return Error::BadArg;
    uint8_t* t = static_cast<uint8_t*>(target);
    std::lock_guard<std::mutex> hold(g_lock);
    if (Find(t)) return Error::AlreadyHooked;
    g_detours.reserve(g_detours.size() + 1);   // the push below can't throw after target is patched

    uint8_t* relay = CarveNear(t, 14 + stolen + 14);
    if (!relay) return Error::NoCave;
    PutAbsJump(relay, detour);
    uint8_t* tramp = relay + 14;
    std::memcpy(tramp, t, stolen);
    PutAbsJump(tramp + stolen, t + stolen);

    uint8_t patch[kMaxStolen];
    patch[0] = 0xE9;
    const int32_t rel = static_cast<int32_t>(relay - (t + 5));
    std::memcpy(patch + 1, &rel, 4);
    std::memset(patch + 5, 0x90, stolen - 5);

    Detour d{ t, stolen, {} };
    std::memcpy(d.saved, t, stolen);
    *original = tramp;
    if (!WriteCode(t, patch, stolen)) { *original = nullptr; return Error::Protect; }
    g_detours.push_back(d);
    return Error::None;
#endif
}

Error RemoveDetour(void* target) {
    std::lock_guard<std::mutex> hold(g_lock);
    Detour* d = Find(target);
    if (!d) return Error::NotHooked;
    if (!WriteCode(d->target, d->saved, d->stolen)) return Error::Protect;
    g_detours.erase(g_detours.begin() + (d - g_detours.data()));
    return Error::None;
}

bool IsHooked(const void* target) {
    std::lock_guard<std::mutex> hold(g_lock);
    return Find(target) != nullptr;
}

size_t DetourCount() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_detours.size();
}

}  // namespace sco::hook
