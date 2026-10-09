# Design: game-file overrides (`sco::vfs`) and a semantic DataCore patcher

**Status: design accepted with the maintainer's decisions of 2026-10-09 ([Decisions](#decisions-maintainer-2026-10-09)). No code yet.** Phase 6 item ([Framework plan](../framework.md#phase-6-the-framework-grows)). Scope rules unchanged: offline single-player only; this is about the game's local data files as the game reads them, nothing else.

## Why

sc-offline's new quantum drive works by changing the game's DataCore database (`Data\Game2.dcb` inside `Data.p4k`) as the engine reads it. Today that change is a byte diff: 15 splices at fixed offsets, gated on one exact build of the file. Any game patch changes the file, the gate fails, and the feature turns itself off until someone rebuilds the diff by hand.

The goal of sco-core is to absorb Star Citizen's internal changes so mods don't have to. For data mods that means two things:

1. **`sco::vfs`**: a small, engine-agnostic layer that serves a *virtual* version of a game file (base ranges plus replacement bytes) through whatever read/seek calls the engine uses. sco-core owns the arithmetic and the rules; the product only connects the engine's file calls to it.
2. **`sco::datacore`**: a patcher that parses the DataCore file's own tables at load time and turns *semantic* overrides ("this record's field is now 2.5", "add an instance of this struct and point that field at it") into the splice list `sco::vfs` serves. Offsets are computed from the file being loaded, so an override written once keeps working across patches as long as the records and fields it names still exist.

Data packs then declare overrides in a text file, and modders change game data without code and without rebuilding anything per patch. Plugins can queue the same overrides from code through a service (section 6).

## 1. How the current DCB patch works

All references are to sc-offline `origin/main` at `dd79026`.

### Finding the loader and CryPak

`ResolveQuantumApi` (`src/quantum.cpp:409-440`) runs at startup:

| Step | Where | What |
|---|---|---|
| Anchor string | `quantum.cpp:417-418` | Finds `"DCB file is smaller than expected"` in `.rdata` and the `lea r9, [rip+...]` (`4C 8D 0D`) that loads it |
| Function start | `quantum.cpp:419-426` | `RtlLookupFunctionEntry` on that site, following chained unwind info (up to 8 links) to the primary function: the DataCore loader (`CDataCoreLoader::InitializeBinary`, by the tag string at `quantum.cpp:42`) |
| Prologue check | `quantum.cpp:427` | The first 24 bytes must be the expected `mov [rsp+...]` / `push` prologue, else no loader |
| CryPak global | `quantum.cpp:428-430` | In the first 0x400 bytes, `mov rcx, [rip+X]; lea r8, ...; mov rdx, [rbp+..]; xor r9d, r9d; mov rax, [rcx]; call [rax+0x148]`: `X` is the global `ICryPak*`, slot 0x148 its `FOpen` |
| Slot check | `quantum.cpp:431-436` | The loader's first 0x2400 bytes must contain `call [rax+0x160]`, `[rax+0x1D0]` and `[rax+0x1E0]` (read, seek, close) |
| Hook | `quantum.cpp:437-439` | `BuildRuns()`, then a 15-byte detour on the loader (`HookFunction`) |

The four vtable slots and their signatures are at `quantum.cpp:6-11` (`kPakOpen = 0x148`, `kPakRead = 0x160`, `kPakSeek = 0x1D0`, `kPakClose = 0x1E0`). Readiness becomes the capability `quantum.drive` (`QuantumDriveReady`, `quantum.cpp:442`; `SetCap` at `src/dllmain.cpp:165`).

### Hooking only while the loader runs

`LoadDataCoreHook` (`quantum.cpp:131-150`) records the loader's thread id, swaps the four CryPak vtable slots for its own functions (`SwapPakSlots`, `quantum.cpp:111-129`: `VirtualProtect` on the vtable, write four pointers, restore), calls the real loader, swaps the slots back, and logs one of four outcomes. The swap is on the CryPak *vtable*, not a code detour, and it lasts exactly as long as the loader call.

`PakOpenHook` (`quantum.cpp:82-91`) passes every open through. Only the first `.dcb` opened **on the loader's thread** while no other is tracked becomes the patched file: it resets the virtual position and runs `IsPatchable`.

### The gate

`IsPatchable` (`quantum.cpp:70-80`) reads the first 120 bytes and compares them with `dcbpatch::kOrigHeader` (`src/dcb_patch.h:6-12`). Then, for every edit that removes bytes, it seeks to the edit and compares the old bytes with `kOld`. It seeks back to 0 and sets `state = 1` (patch) or `-1` (leave alone).

The header holds every table count and both string-pool lengths (section 2), so comparing it pins the exact build of the file almost as tightly as a hash. `kOrigSize = 331932921` and `kPatchedSize = 331936569` (`dcb_patch.h:5`) aren't compared against the file. They size the last run (`quantum.cpp:38`) and the base for `SEEK_END` (`quantum.cpp:100`). In effect the patch is gated on that exact file.

### The run table and the read/seek arithmetic

`dcbpatch::kEdits` (`dcb_patch.h:173-190`) is 15 `{ at, removed, added, bytes, old }` entries, sorted by `at` in original-file offsets. `BuildRuns` (`quantum.cpp:30-39`) turns them into at most 31 runs that tile the patched file: `{ start (patched offset), len, orig (original offset), bytes }`. A run either copies from the original file (`bytes == nullptr`) or from `kNew`.

- `PakSeekHook` (`quantum.cpp:98-104`) never touches the real file. It moves the virtual position `g_dcb.pos` (`SEEK_END` is relative to `kPatchedSize`) and returns 0, or -1 for a bad mode or a negative position.
- `PakReadHook` (`quantum.cpp:93-96`) converts `length * elems` to bytes and calls `ReadPatched`, then returns whole elements (`done / length`).
- `ReadPatched` (`quantum.cpp:48-68`) walks the runs linearly from the first one not entirely before `pos`. Buffer runs are `memcpy`. Base runs seek the real file only if its real position `g_dcb.real` differs, then read. A short base read stops the loop and returns what was read.
- `PakCloseHook` (`quantum.cpp:106-109`) forgets the tracked file.
- `ReadTag` (`quantum.cpp:41-46`) supplies the engine's debug tag argument when the caller passed none.

### What the 15 edits mean

Decoded against the real `Game2.dcb` of 4.10.193 (layout in section 2). The diff is a set of semantic changes:

| `at` (orig) | Removed / added | Region | Meaning |
|---|---|---|---|
| 112 | 2 / 2 | Header, value-string length | `0x010632A5` -> `0x0106387F`: the value-string pool grows by 1498 bytes |
| 413652 | 1 / 1 | Data mapping 1846 (`Vec2`) | Instance count 121434 -> 121506 (+72) |
| 421004 | 1 / 1 | Mapping 2765 (`SEntityEffectSystem_Attachment_EntitySlot`) | +5 instances |
| 421132 | 17 / 17 | Mappings 2781-2783 (`..._ParticleTagEffect`, `..._ParticleTriggerEffect`, `..._ParticlePropertyLink`) | +7, +4, +2 instances |
| 437276 | 1 / 1 | Mapping 4799 (`Behavior_CustomQuantumDriveEffectsPreset`) | 0 -> 1 instance |
| 437420 | 1 / 1 | Mapping 4817 (`SCItemQuantumDriveParams_NEW`) | 0 -> 1 instance |
| 12589416 | 5 / 5 | Strong-pointer pool, entry 808880 | `(struct 4811, instance 4)` -> `(struct 4817, instance 0)`: an existing pointer now targets the new drive params. Which record owns it wasn't traced |
| 54324221 | 0 / 1498 | End of the value-string pool | New strings: sound event names (`Play_SSQT_Stage_4_Engage`, ...) and `guid\|path` effect references |
| 139869711 | 0 / 576 | End of the `Vec2` instance block | 72 instances x 8 bytes |
| 168895584 | 0 / 220 | End of block 2765 | 5 x 44 |
| 171266619 | 0 / 392 | End of block 2781 | 7 x 56 |
| 171759027 | 0 / 224 | End of block 2782 | 4 x 56 |
| 172495827 | 0 / 64 | End of block 2783 | 2 x 32 |
| 300236309 | 0 / 18 | End of block 4799 | 1 x 18 |
| 300258729 | 0 / 656 | End of block 4817 | 1 `SCItemQuantumDriveParams_NEW` |

The inserts add up to 3648 bytes, exactly `kPatchedSize - kOrigSize`. Every structural change is an **append**: new instances go at the end of their struct's block, and new strings go at the end of the pool. Because DataCore addresses instances by `(struct index, instance index)` and strings by pool offset, appending never renumbers anything already in the file, so no other pointer needs fixing. The semantic patcher (section 4) is built on this property.

### Failure modes today

| Case | What happens | Log |
|---|---|---|
| Loader, CryPak or slot calls not found | No hook; `quantum.drive` off | `game data loader not hooked (CryPak ..., its calls ...)` (`quantum.cpp:451`) |
| `ICryPak` pointer null at load | Unpatched load | `CryPak not found; game data loaded ok without the new drive` (`quantum.cpp:141`) |
| Header or old bytes differ (any game patch) | `state = -1`, the file passes through untouched | `Game2.dcb isn't the 4.10.0 one the patch was made for (game updated?)` (`quantum.cpp:146`) |
| No `.dcb` opened on the loader thread | Unpatched load | `the loader opened no .dcb` (`quantum.cpp:148`) |
| Short read from the real file | Partial read returned, as the engine would see without the patch | None |

The failure handling is sound (never a corrupt read; the game loads its own data). The cost is that **every game patch turns the feature off**: the diff must be rebuilt from a re-patched file, and there is no tool for that in the repository.

Smaller points the new design fixes on the way: positions are 64-bit, but the engine's seek takes an `int` offset (`PakSeekFn`, `quantum.cpp:9`), so files above 2 GiB need care. Run lookup is linear (fine for 31 runs, not for thousands of overrides). `FTell`, `FGetSize` and `FEof` aren't intercepted, which is safe only because this loader doesn't call them for the tracked file. The vtable swap affects every CryPak caller on every thread during the load window; other threads pass through because they aren't the tracked handle.

## 2. The DataCore binary layout

Measured read-only on `Data\Game2.dcb` from the LIVE `Data.p4k`: a zip64 entry, method 100 (Zstandard), not encrypted, 29,768,734 bytes compressed and 331,932,921 bytes uncompressed. Its first 120 bytes are identical to `kOrigHeader`, so it is the file the current patch was made for. Field layouts follow the public community format notes (the unp4k / scdatatools / StarBreaker lineage), corrected where this build differs. The check that the layout is right: **the table sizes below sum exactly to the file size, the per-struct instance sizes computed from the property tables sum exactly to the data section (270,412,986 bytes), and a record's own `structSize` equals the computed size.**

### Header (120 bytes, little-endian)

| Offset | Field | 4.10.193 value |
|---|---|---|
| 0 | `u32` unknown | 0 |
| 4 | `u32` file version | 8 |
| 8 | 4 x `u16` unknown | 0 |
| 16 | `u32` struct definition count | 6694 |
| 20 | property definition count | 23789 |
| 24 | enum definition count | 774 |
| 28 | data mapping count | 6694 |
| 32 | record count | 117022 |
| 36-104 | value counts: bool, int8, int16, int32, int64, uint8, uint16, uint32, uint64, float, double, guid, string, locale, enum, strong, weak, reference (18 x `u32`) | 45996, 10, 10, 774, 10667, 10, 10, 36, 10, 76807, 10, 159, 100731, 6837, 144578, 1817970, 184395, 748905 |
| 108 | enum option count | 6357 |
| 112 | value-string pool length | 17183397 |
| 116 | name-string pool length | 7195714 |

### Tables, in file order

| Table | Entry | Fields | Start (4.10.193) | Bytes |
|---|---|---|---|---|
| Struct definitions | 16 | `u32 name` (name pool), `i32 parent` (-1 = none), `u16 propertyCount`, `u16 firstProperty`, `u32 nodeType` | 120 | 107,104 |
| Property definitions | 12 | `u32 name`, `u16 typeIndex` (struct or enum), `u16 dataType`, `u16 conversion` (0 = single, 1-3 = array kinds), `u16 pad` | 107,224 | 285,468 |
| Enum definitions | 8 | `u32 name`, `u16 optionCount`, `u16 firstOption` | 392,692 | 6,192 |
| Data mappings | 8 | `u32 instanceCount`, `u32 structIndex` | 398,884 | 53,552 |
| Records | **36** | `u32 name` (name pool), `u32 fileName` (value pool), `u32 unknown`, `u32 structIndex`, `guid id` (16), `u16 instanceIndex`, `u16 structSize` | 452,436 | 4,212,792 |
| Value pools | 1-20 | int8, int16, int32, int64, uint8, uint16, uint32, uint64, bool, float, double, guid, string (`u32` offset), locale, enum, strong (`u32 struct, u32 instance`), weak (same), reference (`u32` + `guid`, 20), enum options (`u32` name) | 4,665,228 | 32,475,596 |
| Value strings | NUL-terminated | String values, record file paths, `guid\|path` references | 37,140,824 | 17,183,397 |
| Name strings | NUL-terminated | Struct, property, enum and record names | 54,324,221 | 7,195,714 |
| Data | Instances | One block per mapping, in mapping order; a block is `instanceCount x structSize` | 61,519,935 | 270,412,986 |

Property data types seen in this build: 1 bool, 2-5 int8-int64, 6-9 uint8-uint64, 0xA string, 0xB float, 0xC double, 0xD locale, 0xE guid, 0xF enum, 0x10 class (inline struct, size of that struct), 0x110 strong pointer, 0x210 weak pointer, 0x310 reference. An array field is 8 bytes inline (`u32 count, u32 firstIndex` into the pool or block of its element type). A struct's instance layout is its parent's properties first, then its own, packed with no padding.

**The records are 36 bytes, not the 32 of the public notes**, and the file version is still 8. The extra `u32` at record offset 8 has unknown meaning; sampled values are not offsets to string starts in either pool. Treat this as the main lesson about layout: **the parser must never trust the version number alone**. It derives every size from the header and requires the totals to add up (section 4, validation).

### What survives a game patch

| Stable (by design or in practice) | Changes every patch |
|---|---|
| Record GUIDs: they are the record's identity in the game's own references (`guid\|path`, reference values) | Every count in the header, and so every table offset |
| Record names and file paths, mostly (renames happen but are rare) | Struct, property and enum *indices* |
| Struct, property and enum *names* and the property order within a struct (fields get added, rarely renamed) | Instance indices of pooled objects, and pool offsets of strings |
| The file structure (tables, pools, mapping-ordered data) for a given header shape | Byte offset of every instance and field |

The patcher therefore addresses everything by **name or GUID** and recomputes indices and offsets from the tables of the file actually being loaded.

## 3. `sco::vfs`: game-file overrides

### Split

| Part | Lives in | Knows about |
|---|---|---|
| `sco::vfs` core: virtual files, mount table, reader arithmetic, limits | sco-core `include/sco/vfs.h`, `src/vfs/` | Bytes, offsets, paths. No engine, no Windows |
| Producers: whole-file replacement from a pack file; splice lists; `sco::datacore` | sco-core | The file formats they patch |
| Engine adapter `sco::game::pak`: signature rows plus the hooks on CryPak open/read/seek/close (and size/tell/eof), forwarding to `sco::vfs` | sco-core: `include/sco/game/pak.h`, `src/game/pak_sigs.cpp`, `src/game/pak_hooks.cpp` | Star Citizen's CryPak |
| Enabling it, and the product's own data | The product (sc-offline): one `Enable` call at startup, plus its built-in data pack | Its own features |

The core builds and is tested on Linux and Windows like the rest of sco-core. The adapter is the only engine-specific code, and it lives in sco-core too ([decision 1](#decisions-maintainer-2026-10-09)), so any product on sco-core gets file overrides by enabling it.

### Virtual files

```cpp
namespace sco::vfs {
struct Splice { uint64_t at; uint64_t removed; std::shared_ptr<const Bytes> bytes; };   // in base offsets

// The patched file as a sorted list of segments that tile [0, size).
struct Segment { uint64_t start, len; enum Kind { Base, Buffer, File } kind; uint64_t from; const void* src; };
struct Composed {
    uint64_t baseSize, size;
    std::vector<Segment> segments;      // sorted by start; adjacent; no gaps
};

// Validates (sorted, non-overlapping, at + removed <= baseSize, total size <= kMaxFileSize),
// then builds segments. Error text names the first bad splice.
Result Compose(uint64_t baseSize, std::span<const Splice> splices, Composed& out);
}
```

`Compose` generalizes `BuildRuns` (`quantum.cpp:30-39`): the same tiling, with 64-bit sizes, a validated input, and a third segment kind `File` for whole-file replacements streamed from a pack file instead of held in memory.

### Reading

Each open handle of a mounted file gets a `Reader { const Composed* file; uint64_t pos; uint64_t basePos; }`:

- `Seek(off, whence)`: `SET`, `CUR`, `END` (relative to `Composed::size`); a negative result is an error, and the position is unchanged. Seeking past the end is allowed and reads return 0, matching C `fseek` semantics. The adapter narrows the engine's `int` offset carefully.
- `Read(dst, n, BaseIo&)`: binary-search the segment containing `pos` (`O(log n)`), then copy segment by segment. `Base` segments read through `BaseIo` (the engine's original read/seek for this handle), seeking only when `basePos` differs, as today. A short base read ends the call with the bytes read so far, never with garbage. `File` segments read from the pack file through a handle owned by the reader.
- `Tell`, `Size`, `Eof` answer from the virtual file. The adapter must route every call the engine makes on a mounted handle; any slot left unrouted reports base values. Section 7's real-file check verifies which slots the loader uses.
- Element semantics (`fread(ptr, size, count)`) stay in the adapter: bytes in, `done / size` out, as `PakReadHook` does.

### Mount table

```cpp
struct Mount {
    std::string path;          // normalized: lowercase, '/' separators, no leading '/', e.g. "data/game2.dcb"
    int         priority;      // higher wins; from plugin order (later pack = higher) unless a pack sets one
    std::string source;        // pack id, for the report
    Producer    producer;      // Replace{file} | Splices{list} | Transform{fn(BaseIo&, baseSize) -> splices}
};
```

- **Replace** wins outright: the highest-priority `Replace` for a path is served and lower ones are logged as shadowed. A `Transform` *over* a replacement isn't supported in v1, and the conflict is logged.
- **Splices and transforms** merge: every producer for a path contributes splices against the *same base*. Overlapping splices from two sources are a conflict. The higher priority wins, and the loser's splice is dropped with a log line naming both packs.
- `sco::datacore` is a `Transform`: all packs' DataCore overrides are collected into one patcher run (section 4). Field-level conflicts resolve by priority before any splice exists, so the merge step sees no overlaps from DataCore.
- The table is built once at startup from the content index (section 5) and published as an immutable snapshot (`std::shared_ptr<const Table>`, swapped atomically). A rebuild (developer reload) makes a new snapshot; open handles keep the one they opened with.

### Threads

CryPak is called from the main thread, the loading thread and streaming workers. The rules:

1. **The open hook decides.** It normalizes the path and looks it up in the current snapshot (lock-free read of a `shared_ptr` plus a hash lookup). Unmounted paths pass straight through with no allocation. This is the fast path for every other file the game opens.
2. **Composition happens once per path and base identity** (`baseSize` plus a hash of the first 4 KiB). It runs under a per-path once-guard, so two threads opening the same file at once compute it once, and the second waits. The result is cached for the process lifetime and the memory counts against the budget.
3. **Per-handle state** lives in a sharded map keyed by the engine's file handle, inserted at open and erased at close. A read or seek on an unknown handle passes through. The engine already serializes use of one handle, so `Reader` itself needs no lock. **No sco-core lock is held while calling the engine's original read or seek.**
4. **Hooks stay installed.** The current patch swaps slots only during the loader call. A general VFS needs them for the process lifetime, so the cost of rule 1 must stay at one hash lookup per open, and one sharded-map lookup per read or seek on any handle. Before then, `sco::game::pak` keeps the scoped window (`Scope::DataCoreLoad`, section 3), and persistent hooks come in plan PR 10.
5. A producer that throws or fails (a `Transform` that can't parse) makes that mount inert for the run: the file passes through, and the failure is a log line and a status reason. **A failure never produces a partial virtual file.**

### Limits

| Limit | Default | Over it |
|---|---|---|
| In-memory replacement bytes, all mounts | 64 MiB | Mounts refused in priority order, lowest first, with a reason |
| Splices per path | 65536 | That path's mount refused |
| Virtual file size | 4 GiB (64-bit internally; the adapter checks the engine's `int` seeks) | Mount refused |
| `Replace` files | Streamed (`File` segments), not counted against memory | |

The DataCore patch today is 3.6 KB of buffers. A pack that changes thousands of fields still needs well under 1 MiB.

### The game-specific part: `sco::game::pak` in sco-core

Everything engine-specific lives in sco-core, beside the other game rows ([decision 1](#decisions-maintainer-2026-10-09)). sc-offline only enables it and supplies its data.

**Signature rows** (`src/game/pak_sigs.cpp`), moved byte for byte from `quantum.cpp` per [Adding a signature](../adding-signatures.md), so `sco-sigcheck` reports them on patch day:

| Row | Kind | From |
|---|---|---|
| `pak.datacore_loader` | Resolver: string `"DCB file is smaller than expected"`, `lea r9` site, chained unwind to the function start, prologue check. The detour on it uses `StolenLength` (hook v2) | `quantum.cpp:417-427` |
| `pak.crypak` | Resolver (needs `pak.datacore_loader`): the `mov rcx, [rip+X] ... call [rax+0x148]` site, result the `ICryPak*` global | `quantum.cpp:428-430` |
| `pak.slots` | Layout check (needs `pak.datacore_loader`): the loader calls slots 0x160, 0x1D0, 0x1E0. The accessor exposes the four slot offsets as constants pinned by this row (as `stolenBytes` is for `system.quit`) | `quantum.cpp:11, 431-436` |

**Hooks** (`src/game/pak_hooks.cpp`, Windows): the four CryPak vtable slots are switched with a new `sco::hook::SwapSlot(void** slot, void* fn, void** original)` ([decision 2](#decisions-maintainer-2026-10-09)). It goes through sco::hook's registry, so two users can't swap the same slot, and it writes through `WriteCode`. It joins a `Transaction` so the four slots switch together or not at all. Slot swaps need no stolen-byte analysis, are trivially reversible, and catch every call through `ICryPak`, which is how engine file access goes. Detours on the slot targets would also catch devirtualized calls but hook the implementation for every caller, so they're not used.

**API** (`include/sco/game/pak.h`):

```cpp
namespace sco::game::pak {
enum class Scope { DataCoreLoad, AllFiles };     // AllFiles arrives with persistent hooks (plan PR 10)
struct Options {
    uint32_t size = sizeof(Options);
    Scope    scope = Scope::DataCoreLoad;
    const vfs::Table* (*mounts)();               // the current mount snapshot; the host kit's by default
};
Result Enable(const Options&);   // after sco::ResolveAll; installs the loader detour; sets "vfs.pak"
void   Disable();                // restores slots and the detour (tests, unload)
}
```

With `Scope::DataCoreLoad`, the slots are swapped only while the DataCore loader runs, on the loader's thread, exactly as `quantum.cpp` does today. That keeps the current, proven risk profile until persistent hooks are measured. The capabilities are `vfs.pak` (rows OK and hooks in) and `datacore.patch` (section 4).

**What sc-offline keeps:** a call to `sco::game::pak::Enable` from `StartOffline` (DllMain time, before the game loads DataCore, where `ResolveQuantumApi` hooks today), and its data. At first that data is the existing `dcb_patch.h` edits, mounted as a `Splices` producer with expected old bytes. Later it is a built-in data pack (plan PRs 8-9).

A `Splices` producer may carry each splice's expected old bytes and an expected base header, the generalization of `IsPatchable` (`quantum.cpp:70-80`). If they don't match, the mount is inert for the run and the reason is logged.

## 4. The semantic DataCore patcher

### Parse

`sco::datacore::Schema::Parse(BaseIo&, uint64_t size)` reads, through the *base* reader:

- the header, the definition tables, the mappings and the records (about 4.7 MB in 4.10.193);
- the name-string pool (7.2 MB);
- **on demand**, single values from the pools and instance bytes from the data section, by computed offset (typically a few KB per override).

It does not read the 17 MB value-string pool in full. String overrides only append, and dedupe lookups are confined to the strings an override needs (see Strings below).

It builds lookup tables: struct name to index, property list per struct (inherited first), record GUID to record, record name to record, and, per struct, its size and its data block offset (`dataStart + sum of instanceCount x size` over earlier mappings).

### Validation (all must hold, else the patcher refuses the file)

1. `120 + sum(count x entrySize) + both string-pool lengths + data length == file size`.
2. The record entry size is derived, not assumed: the only size in {32, 36, 40} that satisfies (1) given the data length from (3).
3. `sum over mappings(instanceCount x computedSize(struct)) == data length`.
4. Every record's `structSize == computedSize(structIndex)`.
5. Every name offset lands inside the name pool on a string start. A string start is checked as offset 0 or a preceding NUL.
6. Every `dataType` is a known code. An unknown one marks its struct "opaque": overrides into it are refused, though the file is otherwise usable.

A refusal turns the capability `datacore.patch` off with the first failing check as the reason (`"layout: records don't add up (game format changed)"`). The file passes through untouched.

### API (C++, inside sco-core; packs reach it through section 5, plugins through section 6)

```cpp
namespace sco::datacore {
struct RecordRef { std::optional<Guid> guid; std::string name; };    // GUID preferred; name as fallback/readable alias
struct Value     { /* bool, int64, uint64, double, string, guid, enum option name, pointer target, null */ };

class Patch {
public:
    // Scalars, strings, enums, locales, guids, reference targets, in place.
    Status OverrideField(const RecordRef& rec, std::string_view fieldPath, const Value& v);
    // New instance appended to the end of struct `type`'s block, copied from `cloneFrom` (a field path
    // resolving to an instance of the same struct) or zero-filled. Returns a handle usable as a pointer value.
    Status AddInstance(std::string_view type, const InstanceSource& cloneFrom, InstanceId& out);
    // Point a strong/weak pointer field at an instance (existing or added).
    Status SetPointer(const RecordRef& rec, std::string_view fieldPath, InstanceId target);
    // Append an element to an array field (copies the array to the pool end, see below).
    Status AppendElement(const RecordRef& rec, std::string_view arrayPath, const Value& v);
    // New top-level record (v1, decision 3). Refused while research R1 leaves the record layout open.
    Status AddRecord(std::string_view type, std::string_view name, const Guid& id, const InstanceSource& cloneFrom);

    // Turns everything accepted so far into base-offset splices for sco::vfs.
    Result Emit(std::vector<vfs::Splice>& out) const;
};
}
```

**Field paths** walk from a record's root instance: `params.spoolUpTime`, `Components[SCItemQuantumDriveParams].params`. The steps are:

- `name`: a property of the current struct (inherited properties included).
- `name[3]`: array element by index.
- `name[TypeName]`: the first element whose struct is `TypeName` or derives from it. This is how modders select components by type rather than by a position that shifts between patches.
- Inline class members and **strong pointers** are followed transparently. Weak pointers and references are not followed in v1, because they point at other records, which the override should name directly.

### How each operation becomes splices

Every operation is either an **in-place overwrite** (`removed == added`) or an **append** at the end of a pool, block or table, plus in-place count updates. Nothing existing is ever renumbered. This generalizes exactly what the current hand-made diff does (section 1).

| Operation | Splices |
|---|---|
| Scalar (bool, ints, float, double, guid, enum, locale) | Overwrite `size(dataType)` bytes at `blockOffset(struct) + index x size(struct) + fieldOffset`. An enum value takes the offset of the option name; unknown option names are refused |
| String | Reuse an existing pool string if the patcher already knows its offset (e.g. the field's current value equals it). Otherwise append `value\0` to the end of the value-string pool, update header +112, and overwrite the field's `u32` offset |
| `AddInstance` | Insert `size(struct)` bytes at the end of the struct's block and overwrite the mapping count. The new index is the old count, and existing indices are unchanged |
| Pointer | Overwrite the 8-byte `(struct, instance)` value. If the field is inline, write it there; if the pointer lives in the strong pool (an array element), write there |
| `AppendElement` | Arrays are contiguous `(count, first)` ranges in a pool or block. Appending in place would shift every later array, so instead: copy the existing elements plus the new one to the end of that pool (insert), update the pool's header count, and overwrite the field's `(count, first)`. The old range becomes unreferenced (a few bytes of waste per patch) |
| `AddRecord` | Insert one record entry (36 bytes in 4.10.193) at the end of the record table, or at its sorted position if R1 finds the table sorted and nothing indexes it by position. Update header +32. The record's root instance comes from `AddInstance` (cloned). Append the name to the name pool (header +116) and the file path to the value pool (header +112). The record field at +8 is filled per R1's finding. Rules: [AddRecord](#addrecord) |

`Emit` sorts and merges the splices, and also rewrites the 120-byte header as one overwrite. It then **re-validates the result**: it composes the virtual file in memory against the base reader and runs validation steps 1-5 over the *patched* header, mappings and records. Only then does it hand the splices to `sco::vfs`. A patch that would produce a file the parser itself rejects is never mounted.

### AddRecord

New top-level records are in v1 ([decision 3](#decisions-maintainer-2026-10-09)). Three things about the record table are unknown, so a research step (**R1**) comes first. R1 is read-only on 4.10.193 and its results are written into this doc before the patcher PR that implements `AddRecord`.

| R1 question | How to measure (read-only, on `Game2.dcb` 4.10.193, with `sco-dcb records` from plan PR 2) |
|---|---|
| What the `u32` at record +8 is | Over all 117,022 records: min, max, distinct count, and whether it is shared by records with the same file path. Test the hypotheses in order, on every record. (a) An offset into either string pool landing on a string start (sampled values didn't). (b) An index into a value pool, where the range fits a pool count. (c) A hash, comparing CRC32 / CRC32C / FNV-1a / the engine's `CCrc32` of the record name, the file path and the lowercased path. (d) An offset or index tied to the file path, where records sharing a path share it. The result is one sentence and a check `sco-dcb info` runs on every record |
| Is the record table sorted? | Check order by GUID (as bytes and as the game's string form), by name, by file path and by struct index, and report each as "sorted" or "first out-of-order at index N" |
| Does anything index records by position? | Check whether the first `u32` of each reference value (20 bytes: `u32` plus GUID) equals the target record's index, its `instanceIndex`, or neither, over all 748,905 references. Search the `int32`/`uint32` pools and record +8 for values that track record indices |
| Does the engine accept an appended record? | In game, not read-only: one cloned record added through `sco-dcb`'s offline output and looked up by name in the maintainer's checklist (the spawner takes a class name). Only after the three measurements above |

R1 decides the implementation:

- **+8 field:** if it is identified, the patcher computes it. If it is a per-file value, the patcher copies it from the clone source and refuses a new file path that would need a new value. If it is still unidentified, `AddRecord` stays **refused** with `"record field +8 not understood"`, and the capability `datacore.add_record` is off while every other operation works.
- **Order:** if the table isn't sorted, records are appended. If it is sorted and nothing indexes by position, records are inserted at their sorted position, which shifts later record indices (harmless by that finding). If it is sorted *and* positions are referenced, `AddRecord` is refused until that index is understood.

Validation and refusal rules, each one an override failure (section 4, "When something is missing") with this reason:

| Rule | Refusal reason |
|---|---|
| The GUID is new: not in the file and not added by another source this load | `guid ... already exists (record "...")` |
| The name is unique among records | `record name "..." already exists` |
| The type is a known struct that already has at least one record (a record-capable type) | `struct "..." has no records` |
| `clone` is required in v1 and must be a record of the same struct | `clone must be a record of the same struct`. Zero-filled records are refused: required sub-objects would be null |
| The file path, if given, is `libs/foundry/records/...` and ends in `.xml`. Default: `libs/foundry/records/sco/<pack id>/<name>.xml` | `bad file path` |
| After `Emit`, the re-validation (record count, record entry size, root instance struct and size) passes | The whole patch is refused (`re-validation failed: ...`) |

### When something is missing

Each override resolves independently, at load, against the file the game is loading:

| Case | Result |
|---|---|
| Record GUID not found, name not found | Override skipped: `[datacore] gladius_qt: record "AEGS_Gladius" (guid ...) not found` |
| Field path step not found, or an index out of range | Skipped, naming the step: `... field "params.spoolUpTimeX": no property "spoolUpTimeX" in SCItemQuantumDriveParams` |
| Value type doesn't fit (string into float, out of range for int8, unknown enum option) | Skipped |
| A pointer target from an `AddInstance` that failed | Every override depending on it skipped |
| Layout validation failed | Nothing applied; `datacore.patch` off |

The default is **per-pack atomic**: if any override in a pack fails, none of that pack's overrides are applied. A half-applied gameplay change (a drive that references effects that weren't added) is worse than none. The pack is reported `Refused` with the first reason, the other packs still apply, and its capability `datacore.pack.<id>` is off, so a plugin that depends on it can grey out. A pack can opt out with `atomic = false` for independent tweaks (a list of unrelated numbers). In every case the game reads either its own bytes or a fully validated patched file, never a corrupt read.

### Cost

Parsing reads about 12 MB through the engine's own reader plus a few KB per override. It runs once, inside the first `open` of the `.dcb`, on the loader thread, as `IsPatchable` does today. Target: under 300 ms on the loader thread for 1000 overrides, measured in section 7. The schema is freed after `Emit`; only the splices (small) stay.

## 5. Data packs: overrides without code

A new content kind, `datacore`: files `datacore\*.toml` in a data pack (the content index rule `{ ContentKind::DataCore, "datacore", ".toml", false }` beside the four in `src/plugins/content.cpp:39-44`).

```toml
# data/plugins/gladius_qt/datacore/drive.toml
format = 1                         # file format version, not a game version
atomic = true                      # default

# Plain field overrides
[[set]]
record = "SCItem_QuantumDrive_XL_Example"     # record name, or:
# guid = "e2a4f2c0-...-..."                    # preferred when known; name is then only for messages
field  = "Components[SCItemQuantumDriveParams].params.driveSpeed"
value  = 283046752.0

# New objects, named locally with id, used below with "@id"
[[instance]]
id     = "new_params"
struct = "SCItemQuantumDriveParams_NEW"
clone  = { record = "SCItem_QuantumDrive_S1_Example", field = "Components[SCItemQuantumDriveParams].params" }
set    = { "spoolUpTime" = 4.0, "stageOneAccelRate" = 0.75 }

[[set]]
record  = "AEGS_Gladius_Example"
field   = "Components[SCItemQuantumDriveParams]"
pointer = "@new_params"

[[append]]
record = "..."
field  = "effects"
value  = { struct = "SEntityEffectSystem_ParticleTagEffect", clone = { ... } }
```

(Record and field names above are illustrative.)

- **Order and priority.** Packs apply in plugin order (folder-name order, as the content index already uses), and a later pack wins a field both set. The conflict is logged with both pack ids. Within one pack, a field set twice is an error.
- **Checking without the game.** `sco-dcb lint <pack>` parses the `.toml` (syntax, known keys, value shapes, `@id` references defined before use), with exit codes like `sco-sigcheck`. `sco-host-sim` runs the same check when it indexes a pack. `sco-plugin-check` is C and is built by plugin authors, so it only reports that the `datacore\` folder was indexed. Resolving names needs a real `.dcb`, which `sco-dcb check` does (section 7).
- **Format: TOML, parsed by toml++** ([decision 4](#decisions-maintainer-2026-10-09)). toml++ is header-only and MIT-licensed, which is compatible with sco-core's GPL-3.0: the combined work is distributed under GPL-3.0, and the MIT notice ships with it. It is vendored as a pinned release at `third_party/tomlplusplus/` (`toml.hpp` plus `LICENSE`), untouched, with a sha256 recorded and checked, like Lua and ImGui. It is compiled only into the `sco_datacore` library and the `sco-dcb` tool, with its own warnings off. It is built with `TOML_EXCEPTIONS=0`, so a parse error is a `parse_result` turned into a refusal reason, never an exception crossing sco-core. It needs C++17; sco-core is C++20.
- **Report.** `[datacore] 3 packs: gladius_qt 12/12 applied; ui_tweaks 40/41 applied (1 skipped: ...); old_mod refused (record ... not found)`, and per-pack status in `LogReport`.

Whole-file replacement (`Replace` in section 3), for any other game file, is a separate content kind, `files\<game path>`. It is out of scope for the first PRs because it needs the persistent hooks and more slot coverage. It is listed so that the mount table is designed for it from the start.

## 6. Plugins: the `sco.datacore` service

Native, Lua and C++ plugins queue the same operations from code ([decision 5](#decisions-maintainer-2026-10-09)). Use this for computed overrides (a balance plugin scaling every ship's values) and for overrides that depend on settings; fixed ones belong in a data pack.

### A service, not an `sco_api` minor

It is a service named `sco.datacore` (version 1.0), published by sco-core's host, not a new `sco_api` minor.

- Plugins already have `query_service` (1.1), so no new host-table entry is needed and `sco_api.h`'s layout and `tests/abi_v1.c` stay unchanged.
- The service is optional per product and per build. A host without the pak adapter, or a game build where `datacore.patch` is off, doesn't publish it, and `query_service` answers `SCO_NOT_FOUND`, the existing way to say "not here".
- It is versioned on its own (`(1 << 16) | 0`), so it can grow without an ABI minor.
- No hook point needs ABI: the report goes out as an event on the existing bus.

Two internal additions, no ABI change: the service registry gains a host owner (the reserved id `sco`, so the name `sco.datacore` passes the `<id>.` rule), and the table's header `include/sco_datacore.h` (plain C, beside `sco_api.h`) is pinned by its own `tests/abi_datacore.c`.

### The table: ids, never pointers

Following [API v1 § Services](../api-v1.md#services-11), everything the caller holds is an opaque `uint64_t` id: patches, added instances and added records. Nothing points into the patcher or the game. Strings and values are copied during the call.

```c
/* include/sco_datacore.h: service "sco.datacore", version 1.0 */
typedef enum sco_dc_type { SCO_DC_BOOL, SCO_DC_INT, SCO_DC_UINT, SCO_DC_FLOAT, SCO_DC_STRING,
                           SCO_DC_GUID, SCO_DC_ENUM, SCO_DC_NULL, SCO_DC_INSTANCE, SCO_DC_TYPE_FORCE32 = 0x7fffffff } sco_dc_type;

typedef struct sco_dc_value {
    uint32_t    size;          /* sizeof(sco_dc_value) */
    sco_dc_type type;
    int64_t     i;             /* BOOL, INT */
    uint64_t    u;             /* UINT; an instance id for INSTANCE */
    double      f;             /* FLOAT (float and double fields) */
    const char* s;             /* STRING, GUID ("xxxxxxxx-..."), ENUM option name; copied */
} sco_dc_value;

typedef struct sco_dc_report {
    uint32_t size;
    uint32_t state;            /* SCO_DC_QUEUED, SCO_DC_APPLIED, SCO_DC_SKIPPED, SCO_DC_REFUSED */
    uint32_t op_index;         /* the operation it is about, in call order; ~0u for the patch itself */
    char     reason[192];      /* "" when applied */
} sco_dc_report;

typedef struct sco_datacore_v1 {
    uint32_t size;
    /* SCO_DC_OPEN (before the load: queued patches will apply), SCO_DC_LOADED (patched or not; begin refuses) */
    uint32_t   (*state)(void);
    sco_result (*begin)(sco_plugin* self, uint32_t flags /* SCO_DC_NON_ATOMIC */, uint64_t* out_patch);
    /* record: "guid:xxxxxxxx-..." or a record name; field: a section 4 field path */
    sco_result (*set)(uint64_t patch, const char* record, const char* field, const sco_dc_value* v);
    sco_result (*add_instance)(uint64_t patch, const char* type, const char* clone_record,
                               const char* clone_field, uint64_t* out_instance);
    sco_result (*set_pointer)(uint64_t patch, const char* record, const char* field, uint64_t instance);
    sco_result (*append)(uint64_t patch, const char* record, const char* field, const sco_dc_value* v);
    sco_result (*add_record)(uint64_t patch, const char* type, const char* name, const char* guid,
                             const char* clone_record, const char* file_path, uint64_t* out_record);
    sco_result (*commit)(uint64_t patch);      /* queues it for the load; no more calls on it */
    sco_result (*discard)(uint64_t patch);
    /* reports after the load, or SCO_DC_QUEUED before it; SCO_NOT_FOUND past the last */
    sco_result (*report)(uint64_t patch, uint32_t index, sco_dc_report* out);
} sco_datacore_v1;
```

The rules:

- **Ownership.** `begin` ties the patch to `self`. Every other call checks that the patch id belongs to a live plugin, and answers `SCO_NOT_FOUND` for an unknown, discarded or released id. When a plugin unloads or crashes before the load, `Release(owner)` drops its patches, committed or not; after the load they're already applied.
- **Validation at call time** covers only what needs no file: syntax of the field path and GUID, value shape, `@`-style instance ids from this patch. A failure is `SCO_BAD_ARG`, and the operation isn't queued. Names resolve at load.
- **Threads.** Any thread, before `commit`. Calls take the queue lock briefly and never wait for the load. At the load (the loader thread) the patcher takes the committed patches under that lock and switches `state` to `SCO_DC_LOADED`. From then on, `begin` answers `SCO_UNAVAILABLE`.
- **Timing.** A plugin that loads after the DataCore load gets `state() == SCO_DC_LOADED`, and `begin` answers `SCO_UNAVAILABLE`. Its patch has no effect in this run. It's never applied late, because the engine has already built its objects from the file. Whether plugins load before the DataCore load in sc-offline is not known yet (new open question 1). If they don't, the service is only useful once the host kit starts earlier, and data packs are the way until then. Data packs don't have this problem: their `.toml` files are read when the `.dcb` is opened, with no plugin code.
- **Ordering against data packs.** One rule for both ([decision 7](#decisions-maintainer-2026-10-09)). Every source has its plugin's position in plugin order (folder-name order, built-ins first). Within one plugin, its pack's `.toml` files come first, then its patches in commit order. A later source wins a field two sources set, and the conflict is logged with both ids.
- **Atomicity.** Per patch, the same rule as per pack ([decision 6](#decisions-maintainer-2026-10-09)): one failing operation leaves the whole patch unapplied, unless `begin` had `SCO_DC_NON_ATOMIC`.
- **Report back.** After the load, the host posts the event `datacore.applied` on the game thread: on the next tick, or at `game.ready` if the load came first. Its data is `{ uint32_t size; uint32_t applied, skipped, refused; }` over all sources. A plugin then reads its own results with `report(patch, i, &out)`, which gives one entry per operation plus one for the patch (`op_index == ~0u`). Reasons are the section 4 messages, copied into `reason`.

### Lua and C++

- **sco-lua** queries the service in its runtime and adds `sco.datacore.begin()`. That returns a patch object with `:set(record, field, value)`, `:add_instance(...)`, `:set_pointer(...)`, `:append(...)`, `:add_record(...)`, `:commit()` and `:report()` (a list of `{state, op, reason}`), plus `sco.datacore.state()`. Values map from Lua types: integer, number, string, boolean, nil to NULL, and instances as the userdata `add_instance` returned. Calls count against the step budget like other host calls. The sandbox is unchanged: no file access.
- **scosdk** (`include/scosdk/datacore.hpp`) wraps it. `sco::sdk::DataCorePatch` is a move-only builder over `ServiceRef<sco_datacore_v1>`, with typed `Set(record, field, double | int64_t | bool | std::string_view | Guid)`, `AddInstance(...)` returning a `DataCoreInstance` (holding the id), `SetPointer`, `Append`, `AddRecord`, `Commit()` and `Reports()`. The destructor discards an uncommitted patch. Every call is `noexcept` and answers `sco_result`, like the rest of the SDK. Results arrive via `Subscribe("datacore.applied", ...)`.

## 7. Tests

### sco-core CI (Linux sanitizers, Windows MSVC)

- **vfs arithmetic.** Generated from a fixed seed: random base buffers, random valid splice lists, and random sequences of `Seek`/`Read`/`Tell` with random sizes. Every result is compared with a materialized reference buffer. Edge cases: empty file, splice at 0 and at end, a pure deletion, a short read from `BaseIo` (fake that returns fewer bytes), `SEEK_END` with negative offsets, a position past the end. `Compose` rejects overlapping, unsorted and out-of-range splices.
- **vfs threads (TSan).** N threads open, read and close the same mounted path and unmounted paths concurrently against a fake engine. Assertions: one composition per path, no data race, correct bytes. No sleeps: threads start on a latch and finish on join.
- **Synthetic DCB fixtures.** A small writer in `tests/dcb_builder.h` emits valid DataCore files: a few structs with inheritance, every data type, the three array kinds, strong and weak pointers, references, enums, strings in both pools, records with GUIDs, and a record size of 32 or 36. Tests cover:
  - **Parser:** round-trip, and each validation rule failing on a corrupted fixture (wrong count, wrong record size, a bad name offset, an unknown type), which gives the right refusal reason.
  - **Patcher:** each operation. Apply the patch, build the patched bytes through `sco::vfs`, re-parse them with the parser and with an **independent** minimal reader written for the test, and check the value. Also check that every untouched record's root bytes are unchanged.
  - **Missing data:** every row of the section 4 table produces its message and applies nothing, honoring per-pack atomicity.
  - **Drift:** the same `.toml` applied to two fixtures that differ in table sizes, a struct gaining a field, and records reordered. Both results carry the override, at different byte offsets. This is the test of the whole point of the design.
- **Pack format.** Golden parses of valid and invalid `.toml` files through `sco-dcb lint`, including the exit code and the first error. The vendored toml++ sha256 is checked like Lua's.
- **AddRecord.** On fixtures with 32- and 36-byte records, sorted and unsorted tables: each refusal rule in [AddRecord](#addrecord), and an appended record that the independent reader finds by GUID and by name with its cloned root instance.
- **The `sco.datacore` service.** Through `sco-host-sim` with a fixture `.dcb` and a fake pak: a C plugin, a Lua script and a scosdk plugin each queue a patch. Check the ordering against a data pack, per-patch atomicity, `begin` refused after the load, patches dropped when their plugin unloads first, and `datacore.applied` plus `report` contents. `tests/abi_datacore.c` pins the table.
- **`sco::game::pak` logic.** On Linux and Windows: a fake `ICryPak` object with a real vtable, swapped by `SwapSlot` in a `Transaction`, driven by a fake loader that reads a fixture `.dcb`. The scoped window, the thread gate and the passthrough of other files are tested with the real adapter code.

### Against the real game (local, like `sco-sigcheck`)

A tool `sco-dcb`, built like `sco-sigcheck`, never shipped to players:

| Command | Does |
|---|---|
| `sco-dcb info <file.dcb>` | Prints the section 2 tables (counts, offsets, record size) and runs validation. Exit 0 if it passes, 1 if the layout is refused, 2 for a bad file |
| `sco-dcb check <file.dcb> <pack>...` | Resolves every override of the packs and prints `OK`/`SKIP <reason>` per override and the per-pack verdict. Exit 0 if every pack applies, 1 otherwise. **The patch-day routine for data mods** |
| `sco-dcb show <file.dcb> <record> [field]` | Prints a record's fields as paths and values, so modders can find what to write |
| `sco-dcb diff <a.dcb> <b.dcb>` | A semantic diff: changed fields, added instances, pointers, new strings, emitted as a `.toml` pack. Used once to convert sc-offline's byte patch (section 8) |
| `sco-dcb records <file.dcb>` | The R1 measurements ([AddRecord](#addrecord)): record +8 statistics and hypothesis tests, sort order, reference first-`u32` correlation |
| `sco-dcb lint <pack>` | `.toml` syntax and shape, no game file needed |

Getting the `.dcb` out of `Data.p4k` (a zip64 entry, Zstandard): with an external extractor, documented in `docs/sigcheck.md`'s patch-day routine ([decision 8](#decisions-maintainer-2026-10-09)). sco-core vendors no zstd. For the measurements here the entry was read and decompressed read-only. Encrypted entries are out of scope.

In game, the quantum feature's existing checklist in sc-offline's `docs/features.md` runs on the pack-based patch.

The equivalence check for the migration (plan PR 9): on 4.10.193, the pack produced by `sco-dcb diff` must make `sco-dcb` emit a virtual file **byte-identical** to the one `dcb_patch.h` produces today (same size 331,936,569, same bytes). That proves the patcher reproduces the hand-made patch exactly before the hand-made one is deleted.

## 8. Plan: pull requests in order

| # | Repo | PR | Done when |
|---|---|---|---|
| 1 | sco-core | `sco::vfs` core: `Compose`, `Reader`, mount table snapshot, limits, `Splices` with expected old bytes; tests (arithmetic, threads). No game code | CI green on all jobs, including TSan |
| 2 | sco-core | `sco::datacore` parser, validation, `tests/dcb_builder.h`, `sco-dcb info` and `sco-dcb records` | Fixtures pass. Locally, `sco-dcb info` on 4.10.193 prints the section 2 tables and exits 0 |
| R1 | sco-core (docs) | Research: run `sco-dcb records` on 4.10.193, read-only, and write the answers to the [AddRecord](#addrecord) questions into this doc | The three read-only questions answered, each with its measurement |
| 3 | sco-core | Patcher operations (`OverrideField`, `AddInstance`, `SetPointer`, `AppendElement`), `Emit` with re-validation; drift tests | CI green |
| 4 | sco-core | `AddRecord` per R1, with the refusal rules and the `datacore.add_record` capability; fixture tests for both record sizes and both orders | CI green. Locally, `sco-dcb` adds a cloned record to 4.10.193 and re-validates |
| 5 | sco-core | toml++ vendored at `third_party/tomlplusplus/`; the `datacore` content kind, the `.toml` format, per-pack atomicity and report; `sco-dcb check`, `show`, `diff`, `lint` | CI green; a sample pack in `sdk/examples/` |
| 6 | sco-core | The `sco.datacore` service: host-owned services, `include/sco_datacore.h`, `tests/abi_datacore.c`, the `datacore.applied` event, sco-lua's `sco.datacore`, `scosdk/datacore.hpp`, `sco-host-sim` tests. Can be developed beside PR 5 | CI green; the `sdk-cpp.md`, `lua.md` and `api-v1.md` pages document it |
| 7 | sco-core | `sco::game::pak`: the `pak.*` rows (byte for byte from `quantum.cpp`), `sco::hook::SwapSlot` in `Transaction`, `pak_hooks.cpp` with `Scope::DataCoreLoad`, `Enable`/`Disable`, the fake-CryPak tests | CI green; `sco-sigcheck` on 4.10.193 shows the `pak.*` rows OK |
| 8 | sc-offline | Adopt `sco::game::pak`: bump the sco-core submodule, call `Enable` from `StartOffline`, mount the unchanged `dcb_patch.h` edits as a `Splices` producer with expected old bytes and header. **Delete `quantum.cpp`'s DataCore hook code** (`quantum.cpp:6-150` and the loader/CryPak part of `ResolveQuantumApi`). `quantum.drive` follows `vfs.pak` and the mount's state | No behavior change; the in-game quantum checklist passes; `mod.log` shows the mount applied |
| 9 | sc-offline | The quantum drive's data becomes a built-in data pack (`datacore/quantum_drive.toml`, from `sco-dcb diff`); `dcb_patch.h` removed | Byte-identical check (section 7) on 4.10.193; in-game checklist; `sco-dcb check` exit 0 |
| 10 | sco-core, sc-offline | `Scope::AllFiles` (persistent hooks, frame-time measured first), `Replace` mounts (`files\` content kind), `Tell`/`Size`/`Eof` slots | Separate design review on the extra slots and the measured cost |

PRs 1-6 need no game and can land before 7-9. PR 8 changes nothing visible but proves the adapter in game. PR 9 is where the feature stops breaking on every patch.

## Risks

| Risk | Mitigation |
|---|---|
| The format changes again without a version bump (as the 32 -> 36 byte records did) | Sizes derived and cross-checked (validation 1-4), refusal with a clear reason; `sco-dcb info` on patch day; fixtures for both record sizes |
| The unknown record `u32` matters for added records | R1 measures it first; `AddRecord` is refused (and `datacore.add_record` off) while it is not understood |
| The engine expects records or blocks in an order (sorted by GUID, by name) | Instances and pool entries only append. R1 checks record order and positional references; `AddRecord` inserts in order or refuses accordingly |
| Plugins load after the DataCore load | Then `sco.datacore` answers `SCO_UNAVAILABLE` and never patches late; data packs are unaffected. New open question 1 |
| A toml++ update changes parsing | Pinned release, sha256 checked, golden parse tests |
| An override applies to a renamed or repurposed field and changes gameplay in ways nobody intended | Fields addressed by name and type, so a type change refuses rather than writes; `sco-dcb check` on patch day shows what still resolves; per-pack atomicity |
| The engine reads the `.dcb` by another path (memory map, async read, a second open) | The adapter routes by handle; `sco-dcb` + one in-game run per patch checks the loader still uses open/read/seek/close (the `pak.slots` row checks the calls exist) |
| Persistent CryPak hooks cost frame time on streaming threads | One hash lookup per open and one per read; measured before PR 10, which stays out of scope until then |
| Two packs disagree | Priority by plugin order, every conflict logged with both ids |
| Memory | Section 3 limits; the schema is freed after `Emit` |

## Decisions (maintainer, 2026-10-09)

| # | Question | Decision |
|---|---|---|
| 1 | Where the engine adapter lives | **In sco-core, as `sco::game::pak`**: signature rows and hooks in sco-core; sc-offline only enables it and supplies its data ([section 3](#the-game-specific-part-scogamepak-in-sco-core); plan PRs 7-8) |
| 2 | Hook mechanism | **Vtable slot swap through a new `sco::hook::SwapSlot`**, joined to `Transaction` |
| 3 | `AddRecord` in v1 | **Yes.** Research R1 first, then `AddRecord` with its validation and refusal rules in the patcher PRs ([AddRecord](#addrecord); plan R1 and PR 4) |
| 4 | Pack file format | **TOML via vendored toml++** (header-only, MIT, GPL-3.0 compatible) at `third_party/tomlplusplus/`, untouched like Lua and ImGui ([section 5](#5-data-packs-overrides-without-code)) |
| 5 | Exposing it to plugins | **Yes, now: the `sco.datacore` service**, published by sco-core's host; `sco_api.h` unchanged ([section 6](#6-plugins-the-scodatacore-service); plan PR 6) |
| 6 | Atomicity default | **Per pack** (and per patch for the service); `atomic = false` / `SCO_DC_NON_ATOMIC` to opt out |
| 7 | Priority | **Plugin order** (folder-name order, built-ins first); later wins, conflicts logged. No `priority` key |
| 8 | Extracting the `.dcb` | **An external extractor**, documented; no zstd vendored |

## Open questions

1. **When does DataCore load relative to the host kit?** sc-offline starts the host kit on the first main-thread tick (`StartHostKit`, `src/dllmain.cpp:173-193`, via `OnMainThreadTick`). The DataCore loader may well run before that, during engine init. Any existing `mod.log` answers it: compare the order of `[+] new quantum drive: game data patched` and the `[core]`/`[plugin]` lines. If DataCore loads first, the `sco.datacore` service can't affect the run in which a plugin loads. Options then: (a) accept that, and plugins use data packs for load-time data; or (b) an early plugin phase that runs `sco_plugin_load` of plugins marked `datacore = early` from the pak open hook on the loader thread (a new host-kit start path, with its own thread rules). Recommendation: measure first; (a) for v1.
2. **Host-owned services.** The reserved owner id `sco` for services the host publishes (`sco.datacore`, later others): fine as a reserved plugin id, or would you rather have a separate host namespace?
3. **The converted quantum pack.** Commit `sco-dcb diff`'s `.toml` as hand-maintained source in sc-offline (proposed), or regenerate it from two `.dcb` files in CI?
