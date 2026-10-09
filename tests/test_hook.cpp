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

// MemoryProtectScope makes read+execute code writable and puts the protection back.
static void TestProtectScope() {
    uint8_t* page = ExecPage();
    CHECK(page != nullptr);
    if (!page) return;
    static const uint8_t kRet3[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3 };   // mov eax, 3 ; ret
    std::memcpy(page, kRet3, sizeof kRet3);
#if defined(_WIN32)
    DWORD old = 0;
    CHECK(VirtualProtect(page, 4096, PAGE_EXECUTE_READ, &old));
#else
    CHECK(mprotect(page, 4096, PROT_READ | PROT_EXEC) == 0);
#endif
    {
        H::MemoryProtectScope writable(page + 1, 1);
        CHECK(writable.Succeeded());
        if (writable.Succeeded()) page[1] = 4;
    }
    CHECK(Call(page) == 4);
#if defined(_WIN32)
    MEMORY_BASIC_INFORMATION mbi{};
    CHECK(VirtualQuery(page, &mbi, sizeof mbi) == sizeof mbi && mbi.Protect == PAGE_EXECUTE_READ);
#endif
    CHECK(!H::MemoryProtectScope(nullptr, 1).Succeeded());
    CHECK(!H::MemoryProtectScope(page, 0).Succeeded());
}

// Instruction lengths over byte arrays (never executed).
static void TestStolenLength() {
    // CSystem::Quit: mov [rsp+10h], rbx ; mov [rsp+18h], rsi ; mov [rsp+20h], rdi ; push rbp
    static const uint8_t kQuit[] = { 0x48, 0x89, 0x5C, 0x24, 0x10, 0x48, 0x89, 0x74, 0x24, 0x18,
                                     0x48, 0x89, 0x7C, 0x24, 0x20, 0x55 };
    CHECK(H::StolenLength(kQuit, 5) == 5);
    CHECK(H::StolenLength(kQuit, 14) == 15);
    CHECK(H::StolenLength(kQuit, 16) == 16);
    static const uint8_t kPushSub[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20 };   // push rbx ; sub rsp, 20h
    CHECK(H::StolenLength(kPushSub, 5) == 6);
    static const uint8_t kRipMov[] = { 0x48, 0x8B, 0x05, 0x10, 0x20, 0x30, 0x40 };   // mov rax, [rip+...]
    CHECK(H::StolenLength(kRipMov, 5) == 0);
    static const uint8_t kRipLea[] = { 0x48, 0x8D, 0x0D, 0x10, 0x20, 0x30, 0x40 };   // lea rcx, [rip+...]
    CHECK(H::StolenLength(kRipLea, 5) == 0);
    static const uint8_t kCall[] = { 0xE8, 0x10, 0x20, 0x30, 0x40 };                 // call rel32
    CHECK(H::StolenLength(kCall, 5) == 0);
    static const uint8_t kJmp8[] = { 0x90, 0xEB, 0x05, 0x90, 0x90 };                 // nop ; jmp rel8
    CHECK(H::StolenLength(kJmp8, 5) == 0);
    static const uint8_t kJz32[] = { 0x0F, 0x84, 0x10, 0x20, 0x30, 0x40 };           // jz rel32
    CHECK(H::StolenLength(kJz32, 5) == 0);
    static const uint8_t kMovImm64[] = { 0x48, 0xB8, 1, 2, 3, 4, 5, 6, 7, 8 };      // mov rax, imm64
    CHECK(H::StolenLength(kMovImm64, 5) == 10);
    static const uint8_t kMovImm32[] = { 0xB8, 1, 2, 3, 4 };                         // mov eax, imm32
    CHECK(H::StolenLength(kMovImm32, 5) == 5);
    // endbr64 ; nop dword [rax+rax+0] ; movaps [rsp+20h], xmm6 ; movzx eax, byte [rcx]
    static const uint8_t kMixed[] = { 0xF3, 0x0F, 0x1E, 0xFA, 0x0F, 0x1F, 0x44, 0x00, 0x00,
                                      0x0F, 0x29, 0x74, 0x24, 0x20, 0x0F, 0xB6, 0x01 };
    CHECK(H::StolenLength(kMixed, 1) == 4);
    CHECK(H::StolenLength(kMixed, 5) == 9);
    CHECK(H::StolenLength(kMixed, 14) == 14);
    CHECK(H::StolenLength(kMixed, 15) == 17);
    // sub rsp, 1000h (81 /5 imm32) ; cmp cx, 1234h (66 81 /7 imm16) ; mov dword [rsp+rax*4+8], 1
    static const uint8_t kImm[] = { 0x48, 0x81, 0xEC, 0x00, 0x10, 0x00, 0x00, 0x66, 0x81, 0xF9, 0x34, 0x12,
                                    0xC7, 0x44, 0x84, 0x08, 0x01, 0x00, 0x00, 0x00 };
    CHECK(H::StolenLength(kImm, 7) == 7);
    CHECK(H::StolenLength(kImm, 8) == 12);
    CHECK(H::StolenLength(kImm, 13) == 20);
    // xor eax, eax ; ret ; int3 padding: a ret before the count is reached refuses
    static const uint8_t kShort[] = { 0x31, 0xC0, 0xC3, 0xCC, 0xCC };
    CHECK(H::StolenLength(kShort, 1) == 2);
    CHECK(H::StolenLength(kShort, 3) == 3);
    CHECK(H::StolenLength(kShort, 5) == 0);
    static const uint8_t kVex[] = { 0xC5, 0xF8, 0x77, 0x90, 0x90 };                  // vzeroupper
    CHECK(H::StolenLength(kVex, 5) == 0);
    CHECK(H::StolenLength(nullptr, 5) == 0 && H::StolenLength(kQuit, 0) == 0);
}

// stolen = 0 counts the prologue: push rbx ; sub rsp, 20h is 6 bytes, so one NOP of padding.
static void TestAutoStolen() {
    uint8_t* page = ExecPage();
    CHECK(page != nullptr);
    if (!page) return;
    // push rbx ; sub rsp, 20h ; mov eax, 7 ; add rsp, 20h ; pop rbx ; ret
    static const uint8_t kG[] = { 0x40, 0x53, 0x48, 0x83, 0xEC, 0x20, 0xB8, 0x07, 0x00, 0x00, 0x00,
                                  0x48, 0x83, 0xC4, 0x20, 0x5B, 0xC3 };
    // mov rax, [rip+0] ; ret: RIP-relative, so it can't be counted
    static const uint8_t kRip[] = { 0x48, 0x8B, 0x05, 0x00, 0x00, 0x00, 0x00, 0xC3 };
    uint8_t* g = page + 64;
    uint8_t* r = page + 128;
    std::memcpy(g, kG, sizeof kG);
    std::memcpy(r, kRip, sizeof kRip);
    CHECK(Call(g) == 7);
    void* orig = nullptr;
    CHECK(H::InstallDetour(g, 0, Addr(&Two), &orig) == H::Error::None);
    CHECK(g[0] == 0xE9 && g[5] == 0x90 && g[6] == 0xB8);
    CHECK(Call(g) == 2 && orig && Call(orig) == 7);
    CHECK(H::RemoveDetour(g) == H::Error::None && std::memcmp(g, kG, sizeof kG) == 0 && Call(g) == 7);
    void* none = nullptr;
    CHECK(H::InstallDetour(r, 0, Addr(&Two), &none) == H::Error::BadArg && !none && !H::IsHooked(r));
    CHECK(H::DetourCount() == 0);
}

static uint8_t* NoNearMemory(const void*, size_t) { return nullptr; }

// No cave in reach: 14 stolen bytes take an absolute jump; fewer is NoCave.
static void TestFarPath() {
    uint8_t* page = ExecPage();
    CHECK(page != nullptr);
    if (!page) return;
    // mov eax, 3 ; mov ecx, 4 ; mov edx, 5 ; add eax, ecx ; ret   (7; three 5-byte instructions)
    static const uint8_t kF[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xB9, 0x04, 0x00, 0x00, 0x00,
                                  0xBA, 0x05, 0x00, 0x00, 0x00, 0x01, 0xC8, 0xC3 };
    static const uint8_t kOne[] = { 0xB8, 0x01, 0x00, 0x00, 0x00, 0xC3 };   // mov eax, 1 ; ret
    uint8_t* fa = page + 64;
    uint8_t* fb = page + 128;
    uint8_t* fc = page + 192;
    uint8_t* fs = page + 256;
    std::memcpy(fa, kF, sizeof kF);
    std::memcpy(fb, kF, sizeof kF);
    std::memcpy(fc, kF, sizeof kF);
    std::memcpy(fs, kOne, sizeof kOne);
    CHECK(Call(fa) == 7 && Call(fs) == 1);

    H::SetNearAllocatorForTesting(&NoNearMemory);
    void* oa = nullptr;
    void* ob = nullptr;
    void* oc = nullptr;
    CHECK(H::InstallDetour(fa, 15, Addr(&Two), &oa) == H::Error::None);
    CHECK(fa[0] == 0xFF && fa[1] == 0x25 && fa[14] == 0x90 && H::IsHooked(fa));
    CHECK(Call(fa) == 2 && oa && Call(oa) == 7);
    CHECK(H::InstallDetour(fb, 0, Addr(&Two), &ob) == H::Error::None);   // auto: 15 on the far path
    CHECK(fb[0] == 0xFF && fb[1] == 0x25 && fb[14] == 0x90);
    CHECK(Call(fb) == 2 && ob && Call(ob) == 7);
    CHECK(H::InstallDetour(fc, 10, Addr(&Two), &oc) == H::Error::NoCave && !oc && !H::IsHooked(fc));
    CHECK(H::InstallDetour(fs, 5, Addr(&Two), &oc) == H::Error::NoCave && !oc);
    CHECK(H::InstallDetour(fs, 0, Addr(&Two), &oc) == H::Error::BadArg && !oc);   // ret within 14 bytes
    CHECK(Call(fc) == 7 && Call(fs) == 1);
    H::SetNearAllocatorForTesting(nullptr);

    CHECK(H::RemoveDetour(fa) == H::Error::None && std::memcmp(fa, kF, sizeof kF) == 0 && Call(fa) == 7);
    CHECK(H::RemoveDetour(fb) == H::Error::None && Call(fb) == 7);
    CHECK(oa && Call(oa) == 7);
    // The near path is back.
    CHECK(H::InstallDetour(fs, 5, Addr(&Two), &oc) == H::Error::None && fs[0] == 0xE9 && Call(fs) == 2);
    CHECK(H::RemoveDetour(fs) == H::Error::None && Call(fs) == 1);
    CHECK(H::DetourCount() == 0);
}

// Transaction: all or nothing; RemoveAll.
static void TestTransactionAndRemoveAll() {
    uint8_t* page = ExecPage();
    CHECK(page != nullptr);
    if (!page) return;
    static const uint8_t kThree[] = { 0xB8, 0x03, 0x00, 0x00, 0x00, 0xC3 };   // mov eax, 3 ; ret
    static const uint8_t kFour[] = { 0xB8, 0x04, 0x00, 0x00, 0x00, 0xC3 };    // mov eax, 4 ; ret
    uint8_t* f3 = page + 64;
    uint8_t* f4 = page + 128;
    std::memcpy(f3, kThree, sizeof kThree);
    std::memcpy(f4, kFour, sizeof kFour);
    void* o3 = nullptr;
    void* o4 = nullptr;
    void* on = nullptr;

    {   // Commit installs in order; Rollback removes them.
        H::Transaction tx;
        tx.Add(f3, 0, Addr(&Two), &o3);
        tx.Add(f4, 5, Addr(&Two), &o4);
        CHECK(H::DetourCount() == 0);   // nothing until Commit
        CHECK(tx.Commit() == H::Error::None);
        CHECK(H::IsHooked(f3) && H::IsHooked(f4) && Call(f3) == 2 && Call(f4) == 2);
        CHECK(o3 && o4 && Call(o3) == 3 && Call(o4) == 4);
        CHECK(tx.Rollback() == H::Error::None);
        CHECK(!H::IsHooked(f3) && !H::IsHooked(f4) && Call(f3) == 3 && Call(f4) == 4);
        CHECK(tx.Rollback() == H::Error::None);   // nothing left to remove
    }
    {   // A failing Add undoes the ones before it and returns its error.
        H::Transaction tx;
        tx.Add(f3, 5, Addr(&Two), &o3);
        tx.Add(nullptr, 5, Addr(&Two), &on);
        tx.Add(f4, 5, Addr(&Two), &o4);
        CHECK(tx.Commit() == H::Error::BadArg);
        CHECK(!H::IsHooked(f3) && !H::IsHooked(f4) && H::DetourCount() == 0 && Call(f3) == 3);
        CHECK(tx.Rollback() == H::Error::None);
    }
    {   // AlreadyHooked from the second Add: the first is undone, the earlier detour is untouched.
        CHECK(H::InstallDetour(f4, 5, Addr(&Two), &o4) == H::Error::None);
        H::Transaction tx;
        tx.Add(f3, 5, Addr(&Two), &o3);
        tx.Add(f4, 5, Addr(&Two), &on);
        CHECK(tx.Commit() == H::Error::AlreadyHooked);
        CHECK(!H::IsHooked(f3) && H::IsHooked(f4) && Call(f4) == 2);
        CHECK(H::RemoveDetour(f4) == H::Error::None);
    }
    {   // The destructor leaves committed detours installed.
        H::Transaction tx;
        tx.Add(f3, 5, Addr(&Two), &o3);
        tx.Add(f4, 5, Addr(&Two), &o4);
        CHECK(tx.Commit() == H::Error::None);
    }
    CHECK(H::IsHooked(f3) && H::IsHooked(f4) && H::DetourCount() == 2);

    // RemoveAll restores every detour and counts them.
    CHECK(H::RemoveAll() == 2);
    CHECK(H::DetourCount() == 0 && Call(f3) == 3 && Call(f4) == 4);
    CHECK(std::memcmp(f3, kThree, sizeof kThree) == 0 && std::memcmp(f4, kFour, sizeof kFour) == 0);
    CHECK(H::RemoveAll() == 0);
}
#endif

int main() {
#if SCO_TEST_HOOKS
    TestHooks();
    TestProtectScope();
    TestStolenLength();
    TestAutoStolen();
    TestFarPath();
    TestTransactionAndRemoveAll();
    std::printf("sco-core hook tests: %d passed, %d failed\n", g_pass, g_fail);
#else
    std::printf("sco-core hook tests: skipped (x86-64 Windows/Linux only)\n");
#endif
    return g_fail ? 1 : 0;
}
