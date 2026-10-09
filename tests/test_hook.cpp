// Unit tests for sco/hook.h: detours over small functions written into executable memory.
// x86-64 Windows and Linux; elsewhere the tests are skipped.
//   tools/test.sh
#include "sco/hook.h"
#include <cstdint>
#include <cstdio>
#include <cstring>

static int g_fail = 0, g_pass = 0;
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

#if (defined(__x86_64__) || defined(_M_X64)) && !defined(__APPLE__)
#define SCO_TEST_HOOKS 1
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#endif

#if SCO_TEST_HOOKS
namespace H = sco::hook;
using Fn = int (*)();

// Calls machine code we wrote. Clang's -fsanitize=function checks a type signature in front of
// every function called through a pointer, which hand-written code doesn't have.
#if defined(__clang__)
__attribute__((no_sanitize("function")))
#endif
static int Call(const void* code) {
    Fn f;
    std::memcpy(&f, &code, sizeof f);
    return f();
}

template <class F> static void* Addr(F f) {
    static_assert(sizeof(F) == sizeof(void*), "function pointer size");
    void* p;
    std::memcpy(&p, &f, sizeof p);
    return p;
}

static uint8_t* ExecPage() {
#if defined(_WIN32)
    return static_cast<uint8_t*>(VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
#else
    void* p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? nullptr : static_cast<uint8_t*>(p);
#endif
}

static int Two() { return 2; }
static void* g_original = nullptr;
static int PlusForty() { return Call(g_original) + 40; }

static void TestHooks() {
    CHECK(std::strcmp(H::ErrorName(H::Error::AlreadyHooked), "already_hooked") == 0);
    uint8_t* page = ExecPage();
    CHECK(page != nullptr);
    if (!page) return;
    // f1: mov eax, 1 ; ret        (stolen 5 = the mov)
    static const uint8_t kF1[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };
    // f2: mov eax, 5 ; nop x3 ; ret   (stolen 8 = the mov and three nops)
    static const uint8_t kF2[] = { 0xB8, 0x05, 0x00, 0x00, 0x00, 0x90, 0x90, 0x90, 0xC3 };
    uint8_t* f1 = page + 64;
    uint8_t* f2 = page + 128;
    std::memcpy(f1, kF1, sizeof kF1);
    std::memcpy(f2, kF2, sizeof kF2);
    CHECK(Call(f1) == 1 && Call(f2) == 5);

    void* orig = nullptr;
    CHECK(H::InstallDetour(f1, 4, Addr(&Two), &orig) == H::Error::BadArg);
    CHECK(H::InstallDetour(f1, 33, Addr(&Two), &orig) == H::Error::BadArg);
    CHECK(H::InstallDetour(nullptr, 5, Addr(&Two), &orig) == H::Error::BadArg);
    CHECK(H::InstallDetour(f1, 5, nullptr, &orig) == H::Error::BadArg);
    CHECK(H::InstallDetour(f1, 5, Addr(&Two), nullptr) == H::Error::BadArg);
    CHECK(H::DetourCount() == 0 && Call(f1) == 1);

    // Detour, original through the trampoline, one detour per target, removal.
    CHECK(H::InstallDetour(f1, 5, Addr(&Two), &orig) == H::Error::None);
    CHECK(f1[0] == 0xE9 && H::IsHooked(f1) && H::DetourCount() == 1);
    CHECK(Call(f1) == 2);
    CHECK(orig && Call(orig) == 1);
    void* again = nullptr;
    CHECK(H::InstallDetour(f1, 5, Addr(&Two), &again) == H::Error::AlreadyHooked && !again);
    CHECK(H::RemoveDetour(f1) == H::Error::None);
    CHECK(std::memcmp(f1, kF1, sizeof kF1) == 0 && !H::IsHooked(f1) && Call(f1) == 1);
    CHECK(orig && Call(orig) == 1);   // the trampoline outlives the detour
    CHECK(H::RemoveDetour(f1) == H::Error::NotHooked);

    // A detour that calls the original, stealing more than one instruction (NOP padding).
    CHECK(H::InstallDetour(f2, 8, Addr(&PlusForty), &g_original) == H::Error::None);
    CHECK(f2[0] == 0xE9 && f2[5] == 0x90 && f2[7] == 0x90);
    CHECK(Call(f2) == 45);
    CHECK(H::RemoveDetour(f2) == H::Error::None && Call(f2) == 5);
    CHECK(H::DetourCount() == 0);

    // Re-hooking after removal works (a new relay and trampoline).
    CHECK(H::InstallDetour(f1, 5, Addr(&Two), &orig) == H::Error::None && Call(f1) == 2);
    CHECK(H::RemoveDetour(f1) == H::Error::None && Call(f1) == 1);

    // Near memory: in rel32 reach, aligned, distinct, writable and executable.
    uint8_t* a = H::AllocateNear(f1, 24);
    uint8_t* b = H::AllocateNear(f1, 24);
    CHECK(a && b && a != b);
    const auto reach = [&](const uint8_t* p) {
        const int64_t d = reinterpret_cast<int64_t>(p) - reinterpret_cast<int64_t>(f1);
        return d > -0x7FFF0000LL && d < 0x7FFF0000LL;
    };
    CHECK(a && b && reach(a) && reach(b + 24));
    CHECK(a && b && reinterpret_cast<uintptr_t>(a) % 16 == 0 && (b >= a + 32 || a >= b + 32));
    if (a) { std::memcpy(a, kF1, sizeof kF1); a[1] = 9; CHECK(Call(a) == 9); }
    CHECK(H::AllocateNear(f1, 0) == nullptr && H::AllocateNear(f1, H::kCaveBytes + 1) == nullptr);
    CHECK(H::AllocateNear(nullptr, 16) == nullptr);

    // WriteCode over code: change f1's immediate.
    const uint8_t seven = 7;
    CHECK(H::WriteCode(f1 + 1, &seven, 1) && Call(f1) == 7);
    CHECK(!H::WriteCode(nullptr, &seven, 1) && !H::WriteCode(f1, nullptr, 1));
}
#endif

int main() {
#if SCO_TEST_HOOKS
    TestHooks();
    std::printf("sco-core hook tests: %d passed, %d failed\n", g_pass, g_fail);
#else
    std::printf("sco-core hook tests: skipped (x86-64 Windows/Linux only)\n");
#endif
    return g_fail ? 1 : 0;
}
