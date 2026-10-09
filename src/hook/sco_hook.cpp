// sco/hook.h: near caves, code writes, instruction lengths and the detour registry.
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

std::mutex          g_lock;     // caves, detours and the near allocator
std::vector<Cave>   g_caves;
std::vector<Detour> g_detours;
NearAllocator       g_nearAllocator = nullptr;   // SetNearAllocatorForTesting; nullptr = CarveNear
thread_local uint32_t g_osError = 0;

constexpr int64_t kReach = 0x7FFF0000;   // keep every byte of a cave inside rel32 range
constexpr size_t  kAbsJump = 14;         // FF 25 00000000 <addr64>

bool InReach(const uint8_t* a, const uint8_t* lo, const uint8_t* hi) {
    const int64_t d1 = reinterpret_cast<int64_t>(lo) - reinterpret_cast<int64_t>(a);
    const int64_t d2 = reinterpret_cast<int64_t>(hi) - reinterpret_cast<int64_t>(a);
    return d1 > -kReach && d1 < kReach && d2 > -kReach && d2 < kReach;
}

uint8_t* MapAt(uintptr_t at) {   // at = 0: anywhere
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

size_t Rounded(size_t n) { return (n + 15) & ~static_cast<size_t>(15); }

uint8_t* CarveNear(const void* anchorPtr, size_t n) {   // g_lock held
    const uint8_t* anchor = static_cast<const uint8_t*>(anchorPtr);
    if (!n || n > kCaveBytes) return nullptr;
    const size_t need = Rounded(n);
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

// n bytes of executable memory anywhere (the far path's trampoline jumps back absolutely).
uint8_t* CarveAny(size_t n) {   // g_lock held
    if (!n || n > kCaveBytes) return nullptr;
    const size_t need = Rounded(n);
    for (Cave& c : g_caves)
        if (static_cast<size_t>(c.end - c.used) >= need) {
            uint8_t* p = c.used;
            c.used += need;
            return p;
        }
    uint8_t* p = MapAt(0);
    if (!p) return nullptr;
    g_caves.push_back({ p, p + need, p + kCaveBytes });
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

// ---- instruction lengths ------------------------------------------------------------------

// The ModRM byte and what follows it (SIB, displacement); 0 for a RIP-relative operand, which
// would point somewhere else once copied.
size_t ModRmLength(const uint8_t* m) {
    const unsigned mod = static_cast<unsigned>(m[0]) >> 6, rm = m[0] & 7u;
    if (mod == 3) return 1;
    size_t n = 1;
    if (rm == 4) {
        ++n;                                                 // SIB
        if (mod == 0 && (m[1] & 7u) == 5) return n + 4;      // [index*s + disp32], no base
    } else if (mod == 0 && rm == 5) {
        return 0;                                            // [rip + disp32]
    }
    return n + (mod == 1 ? 1 : mod == 2 ? 4 : 0);
}

// One instruction at p: its length, or 0 when it isn't decoded here or can't be moved.
// *ends: an unconditional ret or jmp r/m, after which the bytes may not belong to this function.
size_t InsnLength(const uint8_t* p, bool* ends) {
    *ends = false;
    size_t i = 0;
    bool opsize = false, rexw = false;
    for (;; ++i) {   // legacy prefixes: operand size, rep/repne (mandatory SSE prefixes)
        if (i == 4) return 0;
        if (p[i] == 0x66) opsize = true;
        else if (p[i] != 0xF2 && p[i] != 0xF3) break;
    }
    if ((p[i] & 0xF0u) == 0x40) { rexw = (p[i] & 8u) != 0; ++i; }
    const unsigned op = p[i++];
    const size_t imm = rexw ? 4 : opsize ? 2 : 4;   // the z-sized immediate (imm16 / imm32)

    if (op == 0x0F) {
        const unsigned op2 = p[i++];
        const bool modrm = op2 == 0x10 || op2 == 0x11            // movups/movss/movupd/movsd
                        || op2 == 0x1E || op2 == 0x1F            // endbr64 (F3 0F 1E FA), nop r/m
                        || op2 == 0x28 || op2 == 0x29            // movaps/movapd
                        || op2 == 0x57                           // xorps/xorpd
                        || (op2 >= 0x40 && op2 <= 0x4F)          // cmovcc
                        || (op2 >= 0x90 && op2 <= 0x9F)          // setcc
                        || op2 == 0xAF                           // imul r, r/m
                        || op2 == 0xB6 || op2 == 0xB7 || op2 == 0xBE || op2 == 0xBF;   // movzx/movsx
        if (!modrm) return 0;   // jcc rel32 (0F 80-8F) and everything not listed
        const size_t m = ModRmLength(p + i);
        return m ? i + m : 0;
    }
    if (op < 0x40) {   // add/or/adc/sbb/and/sub/xor/cmp
        switch (op & 7u) {
        case 0: case 1: case 2: case 3: {
            const size_t m = ModRmLength(p + i);
            return m ? i + m : 0;
        }
        case 4: return i + 1;     // op al, imm8
        case 5: return i + imm;   // op eax, imm32
        default: return 0;        // segment prefixes, invalid opcodes in 64-bit mode
        }
    }
    if (op >= 0x50 && op <= 0x5F) return i;                               // push/pop r
    if (op >= 0x90 && op <= 0x99) return i;                               // nop, xchg eax, cdqe, cqo
    if (op >= 0xB0 && op <= 0xB7) return i + 1;                           // mov r8, imm8
    if (op >= 0xB8 && op <= 0xBF) return i + (rexw ? 8 : opsize ? 2 : 4); // mov r, imm (imm64 with REX.W)

    size_t after = 0;   // immediate bytes after the ModRM operand
    const auto reg = [&] { return (static_cast<unsigned>(p[i]) >> 3) & 7u; };   // ModRM.reg
    switch (op) {
    case 0x63:                                       // movsxd
    case 0x84: case 0x85: case 0x86: case 0x87:      // test, xchg
    case 0x88: case 0x89: case 0x8A: case 0x8B:      // mov
    case 0x8D:                                       // lea
    case 0xD0: case 0xD1: case 0xD2: case 0xD3:      // shifts by 1 / cl
    case 0xFE:                                       // inc/dec r/m8
        break;
    case 0x80: case 0x83: case 0xC0: case 0xC1: case 0x6B:   // imm8 forms
        after = 1;
        break;
    case 0x81: case 0x69:
        after = imm;
        break;
    case 0xC6: case 0xC7:   // mov r/m, imm; reg 7 is xabort / xbegin rel32
        if (reg() != 0) return 0;
        after = op == 0xC6 ? 1 : imm;
        break;
    case 0xF6: case 0xF7:   // test r/m, imm (reg 0/1); not/neg/mul/imul/div/idiv
        if (reg() < 2) after = op == 0xF6 ? 1 : imm;
        break;
    case 0xFF:              // inc, dec, call r/m, jmp r/m, push r/m; far forms refused
        if (reg() == 3 || reg() == 5 || reg() == 7) return 0;
        *ends = reg() == 4;
        break;
    case 0x68: return i + 4;            // push imm32
    case 0x6A: case 0xA8: return i + 1; // push imm8, test al, imm8
    case 0xA9: return i + imm;          // test eax, imm32
    case 0xC2: *ends = true; return i + 2;   // ret imm16
    case 0xC3: *ends = true; return i;       // ret
    case 0xC9: case 0xCC: return i;          // leave, int3
    default: return 0;   // rel8/rel32 jmp/jcc/call, loop/jrcxz, VEX, and anything not listed
    }
    const size_t m = ModRmLength(p + i);
    return m ? i + m + after : 0;
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
    return CarveNear(anchor, n);
}

MemoryProtectScope::MemoryProtectScope(void* address, size_t size) {
    if (!address || !size) return;
#if defined(_WIN32)
    DWORD old = 0;
    if (!VirtualProtect(address, size, PAGE_EXECUTE_READWRITE, &old)) { g_osError = GetLastError(); return; }
    base_ = address;
    size_ = size;
    old_ = static_cast<uint32_t>(old);
#else
    const uintptr_t page = static_cast<uintptr_t>(sysconf(_SC_PAGESIZE));
    const uintptr_t lo = reinterpret_cast<uintptr_t>(address) & ~(page - 1);
    const uintptr_t hi = (reinterpret_cast<uintptr_t>(address) + size + page - 1) & ~(page - 1);
    if (mprotect(reinterpret_cast<void*>(lo), hi - lo, PROT_READ | PROT_WRITE | PROT_EXEC) != 0) {
        g_osError = static_cast<uint32_t>(errno);
        return;
    }
    base_ = reinterpret_cast<void*>(lo);
    size_ = hi - lo;
#endif
    ok_ = true;
}

MemoryProtectScope::~MemoryProtectScope() {
    if (!ok_) return;
#if defined(_WIN32)
    DWORD old = 0;
    VirtualProtect(base_, size_, static_cast<DWORD>(old_), &old);
#else
    // POSIX can't read a page's old protection back; code pages go back to read + execute.
    mprotect(base_, size_, PROT_READ | PROT_EXEC);
#endif
}

bool WriteCode(void* at, const void* bytes, size_t n) {
    if (!at || !bytes || !n) return false;
    {
        MemoryProtectScope writable(at, n);
        if (!writable.Succeeded()) return false;
        std::memcpy(at, bytes, n);
    }
#if defined(_WIN32)
    FlushInstructionCache(GetCurrentProcess(), at, n);
#else
    __builtin___clear_cache(static_cast<char*>(at), static_cast<char*>(at) + n);
#endif
    return true;
}

size_t StolenLength(const void* code, size_t atLeast) {
    if (!code || !atLeast) return 0;
    const uint8_t* p = static_cast<const uint8_t*>(code);
    size_t total = 0;
    while (total < atLeast) {
        bool ends = false;
        const size_t n = InsnLength(p + total, &ends);
        if (!n) return 0;
        total += n;
        if (ends && total < atLeast) return 0;
    }
    return total;
}

Error InstallDetour(void* target, size_t stolen, void* detour, void** original) {
#if !SCO_HOOK_X64
    (void)target; (void)stolen; (void)detour; (void)original;
    return Error::Unsupported;
#else
    if (!target || !detour || !original || (stolen && (stolen < kMinStolen || stolen > kMaxStolen)))
        return Error::BadArg;
    uint8_t* t = static_cast<uint8_t*>(target);
    std::lock_guard<std::mutex> hold(g_lock);
    if (Find(t)) return Error::AlreadyHooked;   // before decoding: a hooked target starts with our jump
    size_t n = stolen ? stolen : StolenLength(t, kMinStolen);
    if (!n || n > kMaxStolen) return Error::BadArg;
    g_detours.reserve(g_detours.size() + 1);   // the push below can't throw after target is patched

    uint8_t patch[kMaxStolen];
    uint8_t* tramp = nullptr;
    uint8_t* relay = g_nearAllocator ? g_nearAllocator(t, kAbsJump + n + kAbsJump)
                                     : CarveNear(t, kAbsJump + n + kAbsJump);
    if (relay) {   // near: E9 rel32 to a relay in the cave
        PutAbsJump(relay, detour);
        tramp = relay + kAbsJump;
        patch[0] = 0xE9;
        const int32_t rel = static_cast<int32_t>(relay - (t + 5));
        std::memcpy(patch + 1, &rel, 4);
        std::memset(patch + 5, 0x90, n - 5);
    } else {       // far: the absolute jump itself goes over target
        if (n < kFarStolen) {
            if (stolen) return Error::NoCave;
            n = StolenLength(t, kFarStolen);
            if (!n || n > kMaxStolen) return Error::BadArg;
        }
        tramp = CarveAny(n + kAbsJump);
        if (!tramp) return Error::NoCave;
        PutAbsJump(patch, detour);
        std::memset(patch + kAbsJump, 0x90, n - kAbsJump);
    }
    std::memcpy(tramp, t, n);
    PutAbsJump(tramp + n, t + n);

    Detour d{ t, n, {} };
    std::memcpy(d.saved, t, n);
    *original = tramp;
    if (!WriteCode(t, patch, n)) { *original = nullptr; return Error::Protect; }
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

size_t RemoveAll() {
    std::lock_guard<std::mutex> hold(g_lock);
    size_t removed = 0;
    for (size_t i = g_detours.size(); i-- > 0;) {
        const Detour& d = g_detours[i];
        if (!WriteCode(d.target, d.saved, d.stolen)) continue;   // stays installed; LastOsError()
        g_detours.erase(g_detours.begin() + static_cast<std::ptrdiff_t>(i));
        ++removed;
    }
    return removed;
}

bool IsHooked(const void* target) {
    std::lock_guard<std::mutex> hold(g_lock);
    return Find(target) != nullptr;
}

size_t DetourCount() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_detours.size();
}

void SetNearAllocatorForTesting(NearAllocator allocator) {
    std::lock_guard<std::mutex> hold(g_lock);
    g_nearAllocator = allocator;
}

// ---- Transaction --------------------------------------------------------------------------

void Transaction::Add(void* target, size_t stolen, void* detour, void** original) {
    queued_.push_back({ target, stolen, detour, original });
}

Error Transaction::Commit() {
    std::vector<Item> items;
    items.swap(queued_);
    const size_t before = installed_.size();
    installed_.reserve(before + items.size());
    for (const Item& it : items) {
        const Error e = InstallDetour(it.target, it.stolen, it.detour, it.original);
        if (e == Error::None) { installed_.push_back(it.target); continue; }
        // Undo this Commit, last first. One the OS refused to restore stays listed for Rollback.
        std::vector<void*> stuck;
        for (size_t i = installed_.size(); i-- > before;)
            if (RemoveDetour(installed_[i]) == Error::Protect) stuck.push_back(installed_[i]);
        installed_.resize(before);
        installed_.insert(installed_.end(), stuck.begin(), stuck.end());
        return e;
    }
    return Error::None;
}

Error Transaction::Rollback() {
    queued_.clear();
    Error first = Error::None;
    std::vector<void*> stuck;
    for (size_t i = installed_.size(); i-- > 0;) {
        const Error e = RemoveDetour(installed_[i]);
        if (e != Error::None && first == Error::None) first = e;
        if (e == Error::Protect) stuck.push_back(installed_[i]);   // NotHooked: already gone
    }
    installed_.swap(stuck);
    return first;
}

}  // namespace sco::hook
