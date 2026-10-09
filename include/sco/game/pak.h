#pragma once
// sco::game::pak: the engine adapter for sco::vfs. Serves a mounted game file to Star Citizen's
// CryPak (the engine's own file I/O) by switching four ICryPak vtable slots (open, read, seek,
// close) while the DataCore loader runs. Design: docs/design/vfs-datacore.md § 3 ("The
// game-specific part"); reference: docs/api.md § sco/game/pak.h.
//
//   sco::game::RegisterGameSignatures();
//   sco::ResolveAll(image);
//   sco::game::pak::Options o;
//   o.mounts = [] { return g_mountTable.Current(); };   // the product's sco::vfs::MountTable
//   sco::game::pak::Enable(o);                            // detours the loader; sets "vfs.pak"
//
// Rows (src/game/pak_sigs.cpp, moved byte for byte from sc-offline's quantum.cpp):
//   pak.datacore_loader  CDataCoreLoader::InitializeBinary: the function that loads the string
//                        "DCB file is smaller than expected" (lea r9), found through .pdata
//                        (chained unwind info followed to the primary entry), prologue checked
//   pak.crypak           the ICryPak* global: in the loader's first 0x400 bytes,
//                        mov rcx, [rip+X] ... mov rax, [rcx]; call [rax+0x148] (FOpen)
//   pak.slots            the loader calls [rax+0x160], [rax+0x1D0] and [rax+0x1E0] (read, seek,
//                        close) in its first 0x2400 bytes; pins kReadSlot, kSeekSlot, kCloseSlot
//
// Scope::DataCoreLoad (the only scope until plan PR 10): the slots are swapped only while the
// loader runs, and only the first ".dcb" the loader's own thread opens is tracked. Reads and
// seeks on that handle, on that thread, are served by a sco::vfs::Reader over the mount; every
// other handle and every other thread pass straight through to the engine's functions. Tell,
// size and eof aren't routed: the loader doesn't call them on the file (pak.slots checks the
// calls it does make), and plan PR 10 adds them with persistent hooks.
#include "sco/runtime.h"
#include "sco/vfs.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace sco::game::pak {

// ICryPak vtable byte offsets. kOpenSlot is pinned by pak.crypak, the others by pak.slots.
constexpr size_t kOpenSlot = 0x148, kReadSlot = 0x160, kSeekSlot = 0x1D0, kCloseSlot = 0x1E0;

// The engine's functions (x86-64: one calling convention). `tag` is the engine's debug tag:
// a pointer to a pointer to a function-name string.
using LoaderFn = uintptr_t (*)(uintptr_t loader, uintptr_t path, uintptr_t a3, uintptr_t a4, uintptr_t a5);
using OpenFn   = uintptr_t (*)(uintptr_t pak, const char* path, const char* mode, uint32_t flags);
using ReadFn   = size_t (*)(uintptr_t pak, void* data, size_t length, size_t elems, uintptr_t file, const void* tag);
using SeekFn   = int (*)(uintptr_t pak, uintptr_t file, int offset, int mode);   // 0 = ok
using CloseFn  = int (*)(uintptr_t pak, uintptr_t file);

// What Enable hooks. Resolve fills it from the pak.* rows (after sco::ResolveAll); tests pass
// their own.
struct Targets {
    void*      loader = nullptr;   // the DataCore loader (LoaderFn)
    uintptr_t* cryPak = nullptr;   // the global holding the ICryPak* (read at each load)
};
bool Resolve(Targets& out);   // false (out untouched) unless all three pak.* rows are OK

enum class Scope { DataCoreLoad, AllFiles };   // AllFiles arrives with persistent hooks (plan PR 10)

struct Options {
    uint32_t size  = sizeof(Options);
    Scope    scope = Scope::DataCoreLoad;
    // The current mount snapshot (sco::vfs::MountTable::Current), read once per tracked open.
    // Required: sco-core's host kit owns no mount table yet.
    std::shared_ptr<const vfs::Table> (*mounts)() = nullptr;
};

// Installs the loader detour (sco::hook::InstallDetour, stolen bytes counted by StolenLength) and
// sets the capability "vfs.pak". Ok, or nothing changes and vfs.pak is off with the reason:
// BadArg (size, null mounts, null targets, already enabled), Unavailable (Scope::AllFiles, a
// pak.* row not OK), Failed (the detour was refused).
Result Enable(const Options& options);
Result Enable(const Options& options, const Targets& targets);

// Removes the loader detour and restores the slots (sco::hook::RestoreSlot), clears "vfs.pak".
// Idempotent. A load running on another thread keeps its slots until it returns (the slots are
// restored at its end either way), so calling Disable mid-load never cuts a file in half.
void Disable();
bool Enabled();

// What the last DataCore load did with its file (the product logs or reports it).
enum class Outcome {
    None,       // no load since Enable
    Applied,    // the tracked .dcb was served from its mount
    Passed,     // a .dcb was tracked but passed through: not mounted, or the mount is inert
    NoDcb,      // the loader opened no .dcb
    NoCryPak,   // the ICryPak* was null at load time: nothing swapped
    SwapFailed, // the slots couldn't be swapped (reason names the error)
};
const char* OutcomeName(Outcome o);   // "applied", "passed", ...

struct LoadReport {
    Outcome     outcome = Outcome::None;
    bool        loaderOk = false;     // the loader returned true (low byte)
    std::string path;                 // the tracked .dcb as the engine named it
    uint64_t    baseSize = 0, size = 0;   // the real file and the virtual one (Applied)
    std::string reason;               // why it passed through, or what failed
};
LoadReport LastLoad();

}  // namespace sco::game::pak
