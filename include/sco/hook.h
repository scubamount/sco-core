#pragma once
// Detours and near-code memory for x86-64 hosts: one place that patches game code, so two
// features (or two plugins) can never detour the same function or allocate over each other.
// C++ and internal: the host (sc-offline) calls this; plugins only see sco_api.h.
//
//   void* original = nullptr;
//   if (sco::hook::InstallDetour(target, 0, &MyDetour, &original) == sco::hook::Error::None)
//       ...   // MyDetour runs instead of target; calling `original` runs the real function
//
// The stolen bytes (the start of target, copied into a trampoline) must be whole instructions with
// no RIP-relative operand and no relative branch, because they are copied as-is. Pass stolen = 0
// and InstallDetour counts them with StolenLength, which refuses what it can't decode or can't
// move; or pass a count (5 to 32) that a signature row's layout checks pin
// (sco::game::QuitHook::stolenBytes is such a count).
//
// Patching is not atomic against a thread executing target at the same moment: install before
// the game runs the function, or where a torn write can't be observed (as sc-offline does, from
// DllMain and the game thread). Everything else here is thread-safe, except a Transaction object,
// which belongs to one thread.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace sco::hook {

constexpr size_t kMinStolen = 5, kMaxStolen = 32;
constexpr size_t kFarStolen = 14;          // the far patch (FF 25 + address) needs this many bytes
constexpr size_t kCaveBytes = 64 * 1024;   // one cave; AllocateNear never returns more

enum class Error : uint32_t {
    None = 0,
    BadArg,          // null target/detour/original, stolen outside 5..32, or stolen = 0 and
                     // StolenLength can't count target's instructions
    AlreadyHooked,   // target already has a detour from InstallDetour
    NoCave,          // no executable memory within +-2 GiB of target, and stolen < 14 (or none at all)
    Protect,         // the OS refused to make target writable (detail: LastOsError())
    NotHooked,       // RemoveDetour on a target without a detour
    Unsupported,     // not an x86-64 build
};
const char* ErrorName(Error e);   // "none", "bad_arg", "already_hooked", ...

// The OS error code from the last failed WriteCode or MemoryProtectScope on this thread
// (GetLastError / errno).
uint32_t LastOsError();

// n bytes of readable, writable, executable memory within +-2 GiB of anchor, 16-byte aligned,
// carved from caves this module maps (kCaveBytes at a time) and never freed: detours and code
// placed there may run until the process ends. nullptr when n is 0 or over kCaveBytes, or no
// memory could be mapped in range.
uint8_t* AllocateNear(const void* anchor, size_t n);

// Makes [address, address + size) readable, writable and executable for the scope's lifetime.
// The destructor restores the old protection on Windows (what VirtualProtect reported); POSIX
// can't read a page's protection back, so the pages go back to read + execute (code pages).
// Succeeded() is false for a null address, size 0, or when the OS refused (see LastOsError());
// a scope that didn't succeed restores nothing.
class MemoryProtectScope {
public:
    MemoryProtectScope(void* address, size_t size);
    ~MemoryProtectScope();
    MemoryProtectScope(const MemoryProtectScope&) = delete;
    MemoryProtectScope& operator=(const MemoryProtectScope&) = delete;
    bool Succeeded() const { return ok_; }

private:
    void*    base_ = nullptr;   // the range the OS call covered (page-aligned on POSIX)
    size_t   size_ = 0;
    [[maybe_unused]] uint32_t old_ = 0;   // Windows: the protection to restore (unused on POSIX)
    bool     ok_ = false;
};

// Writes n bytes over code at `at`: makes the pages writable (MemoryProtectScope), copies,
// restores their protection and flushes the instruction cache. False when the OS refused (see
// LastOsError()).
bool WriteCode(void* at, const void* bytes, size_t n);

// The byte count of the whole instructions at code that cover at least atLeast bytes, or 0 when
// it meets an instruction it can't decode or can't move into a trampoline: RIP-relative ModRM
// operands, rel8/rel32 jmp/jcc/call, loop/jrcxz, xbegin. Also 0 when a ret or jmp r/m ends
// before atLeast bytes (what follows may be another function). Decodes the usual x86-64
// prologue set: 66/F2/F3 and REX prefixes, push/pop, the ALU r/m and imm families, mov/lea/test,
// mov r,imm (imm64 with REX.W), shifts, imul, nop/0F 1F, endbr64, int3, ret, movzx/movsx, cmov,
// setcc, movaps/movups/movss/movsd and xorps. See docs/api.md for the full list.
size_t StolenLength(const void* code, size_t atLeast);

// Detours target to detour (see the top of this file for the stolen bytes; stolen = 0 counts
// them with StolenLength). Near path: in a cave near target, a 14-byte absolute jump to detour
// (the relay) and the trampoline (the stolen bytes, then an absolute jump to target + stolen);
// target becomes E9 <rel32 to the relay> followed by NOPs. Far path, when no cave can be mapped
// within +-2 GiB of target: target becomes FF 25 00000000 <detour address> (14 bytes) followed
// by NOPs, and the trampoline goes in any executable memory. The far path needs 14 stolen bytes
// (stolen = 0 recounts with StolenLength(target, 14)); with fewer it is NoCave. *original = the
// trampoline, set before target is patched. One detour per target.
Error InstallDetour(void* target, size_t stolen, void* detour, void** original);

// Restores target's stolen bytes. The relay and trampoline stay in their cave (a thread may
// still be inside them), so an `original` pointer stays callable.
Error RemoveDetour(void* target);

// Restores every installed detour (host shutdown, tests), last installed first. Returns how many
// were removed; one the OS refused to restore stays installed (see LastOsError()).
size_t RemoveAll();

bool   IsHooked(const void* target);
size_t DetourCount();

// Vtable slots: switches the function pointer at `slot` (an entry of a C++ object's vtable) to fn.
// *original = the pointer that was there, set before the slot changes, so fn can call it from
// the first call on. The slot is made writable with MemoryProtectScope (on Windows its old
// protection comes back; on POSIX the page goes back to read + execute, as for code) and written
// with one atomic pointer store, so a thread calling through the vtable at that moment sees the
// old or the new function, never a torn pointer. Every caller through the object's vtable on any
// thread reaches fn while it is swapped: fn must pass calls it doesn't handle to *original.
// No stolen bytes, no trampoline: RestoreSlot writes the original back. One swap per slot.
// BadArg: a null or unaligned slot, null fn or original. AlreadyHooked: slot already swapped.
// Protect: the OS refused (LastOsError()).
Error SwapSlot(void** slot, void* fn, void** original);
// Writes back what SwapSlot found in slot. NotHooked when slot isn't swapped; Protect as above
// (the slot stays swapped and registered).
Error RestoreSlot(void** slot);
bool  IsSlotSwapped(void* const* slot);
size_t SlotCount();

// Hooks that only make sense together: all of them are installed, or none.
//
//   sco::hook::Transaction tx;
//   tx.Add(a, 0, &DetourA, &origA);
//   tx.Add(b, 0, &DetourB, &origB);
//   if (tx.Commit() != sco::hook::Error::None) ...   // neither a nor b is hooked
//
// Opt-in, and not for independent hooks: a product whose features stand alone (sc-offline: one
// missing pattern must leave the other hooks in place) installs each with InstallDetour.
// Not thread-safe itself (one owner); the hooks it installs are ordinary detours.
class Transaction {
public:
    Transaction() = default;
    ~Transaction() = default;   // committed hooks stay installed; queued ones are dropped
    Transaction(const Transaction&) = delete;
    Transaction& operator=(const Transaction&) = delete;

    // Queues a detour; nothing is checked or patched until Commit.
    void Add(void* target, size_t stolen, void* detour, void** original);
    // Queues a vtable slot swap (SwapSlot); it commits and rolls back with the detours.
    void AddSlot(void** slot, void* fn, void** original);
    // Installs the queued detours and slot swaps in order. If one fails, removes the ones this Commit
    // installed (last first) and returns that first Error. Either way the queue is emptied.
    Error Commit();
    // Removes every detour and restores every slot this transaction's Commits installed (last
    // first) and drops queued ones. Returns the first error; one the OS refused to restore stays
    // installed and is retried by the next Rollback.
    Error Rollback();

private:
    struct Item { void* target; size_t stolen; void* detour; void** original; bool slot; };
    struct Installed { void* target; bool slot; };
    std::vector<Item>      queued_;
    std::vector<Installed> installed_;
};

// Test-only: replaces the near-cave search InstallDetour uses (nullptr restores the default), so a
// test can force the far path. Called with the module lock held: it must not call into
// sco::hook. Not for products.
using NearAllocator = uint8_t* (*)(const void* anchor, size_t n);
void SetNearAllocatorForTesting(NearAllocator allocator);

}  // namespace sco::hook
