# Adding a signature

This is how a sc-offline feature's addresses move into sco-core. Teleport ([`src/game/teleport_sigs.cpp`](../src/game/teleport_sigs.cpp)) was the first and is the example to copy.

## Rules

1. **Move, don't change.** When a feature moves over, its patterns, offsets and checks move byte for byte. Fix them in a separate commit, so a broken row can be traced to one change.
2. **One address per row.** If a feature reads a global at `+0x24` inside a function, that's two rows: the function, and the global with `needs = { "<feature>.<function>" }`.
3. **Unique or nothing.** A pattern must match exactly once. Two matches is `AMBIG`, never "take the first".
4. **Check what you rely on.** If the feature calls slot `0x2E0` of a vtable, the resolver checks the bytes that prove it, and fails with a reason saying which check failed (`layout changed at +0x035`).
5. **Reasons are static strings.** `SigFail("...")` keeps the pointer; don't pass a buffer.
6. **Resolvers read only their needs.** Call `Sig()` only on ids listed in the row's `needs`. Resolvers must not throw, allocate per call, or write to the game.
7. **No game files.** Patterns and the names of strings the game references are fine. Don't commit pieces of `StarCitizen.exe` or anything extracted from it.

## Steps

### 1. Write the table

Create `src/game/<feature>_sigs.cpp`:

```cpp
#include "sco/game/<feature>.h"
#include "sco/scan.h"
#include "sco/signatures.h"

namespace sco::game {

namespace {

SigResult ResolveThing(const Image& img) {
    const uint8_t* name = FindCString(img.rdata, "SomeConsoleCommand");
    uint8_t* lea = name ? FindRipLea(img.text, 0x48, 0x8D, 0x15, name) : nullptr;
    if (!lea) return SigFail("SomeConsoleCommand isn't referenced");
    uint8_t* f = lea - 0x40;
    if (!BytesMatch(f, "40 53 48 83 EC 20")) return SigFail("layout changed at +0x000");
    return SigOk(f);
}

SigResult ResolveGlobal(const Image&) {
    return SigOk(RipTarget(Sig("<feature>.thing") + 0x12, 3, 7));   // mov rax, [rip+X] at +0x12
}

}  // namespace

extern const SigDef k<Feature>Signatures[] = {
    // pattern row: the match itself
    { "<feature>.helper", "48 89 5C 24 ?? 57 48 83 EC 30 8B FA", 0, 0, nullptr, {} },
    // resolver rows
    { "<feature>.thing",  nullptr, 0, 0, ResolveThing,  {} },
    { "<feature>.global", nullptr, 0, 0, ResolveGlobal, { "<feature>.thing" } },
};
extern const size_t k<Feature>SignatureCount = sizeof(k<Feature>Signatures) / sizeof(k<Feature>Signatures[0]);

}  // namespace sco::game
```

For many rows of the same shape (a unique pattern plus byte checks at offsets, or a RIP target inside another row), use the private helpers in [`src/game/sig_rows.h`](../src/game/sig_rows.h): declare a `constexpr rows::FnSpec` / `rows::RipSpec` and use `rows::ResolveFn<spec>` / `rows::ResolveRip<spec>` as the row's resolver. Each check carries its own static reason (`layout changed at +0x616`), and `FnSpec::extra` takes one more check (a jmp target, the row's position inside another row). The ASOP, hangar and ATC tables ([rows and capabilities](game/asop.md)) are written this way.

A pattern row can also take a RIP target directly: `{ "<feature>.global", "48 8B 05 ?? ?? ?? ?? 48 85 C0", 3, 7, nullptr, {} }` returns the address `mov rax, [rip+X]` reads, not the instruction.

### 2. Add a typed accessor

Create `include/sco/game/<feature>.h` with a comment listing each row and how it's found (see [`teleport.h`](../include/sco/game/teleport.h)), and one function that hands the feature all its addresses at once, or returns false if any row isn't OK:

```cpp
struct <Feature>Addrs { void* thing = nullptr; uintptr_t* global = nullptr; };
bool <Feature>Addresses(<Feature>Addrs& out);   // false, and `out` untouched, unless every row is OK
```

Features use this accessor instead of calling `Sig()` with string ids.

### 3. Register the table

In [`src/game/signatures.cpp`](../src/game/signatures.cpp), declare the two `extern`s and add a `RegisterSignatures(k<Feature>Signatures, k<Feature>SignatureCount)` call to `RegisterGameSignatures()`.

### 4. Build files

Only one build file needs the new `.cpp`: sc-offline's `src/sc-offline-dll.vcxproj` (one `<ClCompile>` line). `tools/sigcheck.sh` here and sc-offline's `tools/check.sh` already pick up every `src/game/*.cpp`.

### 5. Check against a real game build

```sh
tools/sigcheck.sh /path/to/StarCitizen.exe -v
```

Every new row should be `OK`. If you moved code, compare each address with what the old code found (log it from the old code once, or compare against sdk_dumper's catalog with `--catalog`). Then change one byte the resolver checks in a copy of the exe and confirm the row reports `FAILED` with the reason you expect, and its dependents report `BLOCKED`.

### 6. Switch the feature over

In sc-offline, replace the feature's scanning code with the accessor and delete the old patterns in the same commit. The feature should start only if the accessor returns true, and log one line pointing at the `[core]` report when it doesn't, for example `[tp] teleport disabled (see the [core] lines in mod.log)` from `ResolveTeleportApi()` in sc-offline's `src/teleport.cpp`.

### 7. Test

- `tools/test.sh` here (the registry tests don't need a game).
- sc-offline: `tools/check.sh`, the Windows build, and the feature in game. Say in the PR what you played and what you didn't.

## When a game patch breaks a row

1. Run `tools/sigcheck.sh` on the new `StarCitizen.exe`. The `FAILED` line names the row and the check that failed.
2. Find the code again in the new build (a disassembler, the string the resolver starts from, or sdk_dumper's catalog).
3. Update the pattern or offsets in that row only, and re-run until it's `OK`.
4. Play-test the feature, then open the PR with the game build number.
