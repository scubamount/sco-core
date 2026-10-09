#pragma once
// Detours and near-code memory for x86-64 hosts: one place that patches game code, so two
// features (or two plugins) can never detour the same function or allocate over each other.
// C++ and internal: the host (sc-offline) calls this; plugins only see sco_api.h.
//
//   void* original = nullptr;
//   if (sco::hook::InstallDetour(target, stolen, &MyDetour, &original) == sco::hook::Error::None)
//       ...   // MyDetour runs instead of target; calling `original` runs the real function
//
// What the caller guarantees: `stolen` is a whole number of instructions at target (5 to 32
// bytes) with no RIP-relative operand and no relative branch, because they are copied as-is
// into the trampoline. A signature row's layout checks pin this (sco::game::QuitHook::stolenBytes
// is such a count). Instruction-length decoding is planned, not here yet.
//
// Patching is not atomic against a thread executing target at the same moment: install before
// the game runs the function, or where a torn 5-byte write can't be observed (as sc-offline does,
// from DllMain and the game thread). Everything else here is thread-safe.
#include <cstddef>
#include <cstdint>

namespace sco::hook {

constexpr size_t kMinStolen = 5, kMaxStolen = 32;
constexpr size_t kCaveBytes = 64 * 1024;   // one cave; AllocateNear never returns more

enum class Error : uint32_t {
    None = 0,
    BadArg,          // null target/detour/original, stolen outside 5..32
    AlreadyHooked,   // target already has a detour from InstallDetour
    NoCave,          // no executable memory within +-2 GiB of target
    Protect,         // the OS refused to make target writable (detail: LastOsError())
    NotHooked,       // RemoveDetour on a target without a detour
    Unsupported,     // not an x86-64 build
};
const char* ErrorName(Error e);   // "none", "bad_arg", "already_hooked", ...

// The OS error code from the last failed WriteCode on this thread (GetLastError / errno).
uint32_t LastOsError();

// n bytes of readable, writable, executable memory within +-2 GiB of anchor, 16-byte aligned,
// carved from caves this module maps (kCaveBytes at a time) and never freed: detours and code
// placed there may run until the process ends. nullptr when n is 0 or over kCaveBytes, or no
// memory could be mapped in range.
uint8_t* AllocateNear(const void* anchor, size_t n);

// Writes n bytes over code at `at`: makes the pages writable, copies, restores their protection
// and flushes the instruction cache. False when the OS refused (see LastOsError()).
bool WriteCode(void* at, const void* bytes, size_t n);

// Detours target to detour (see the top of this file for what the caller guarantees). In a cave
// near target: a 14-byte absolute jump to detour (the relay) and the trampoline (the stolen
// bytes, then an absolute jump to target + stolen). target becomes E9 <rel32 to the relay>
// followed by NOPs. *original = the trampoline, set before target is patched. One detour per
// target.
Error InstallDetour(void* target, size_t stolen, void* detour, void** original);

// Restores target's stolen bytes. The relay and trampoline stay in their cave (a thread may
// still be inside them), so an `original` pointer stays callable.
Error RemoveDetour(void* target);

bool   IsHooked(const void* target);
size_t DetourCount();

}  // namespace sco::hook
