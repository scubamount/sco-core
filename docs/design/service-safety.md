# Design: service calls that fault, and providers that unload (M1)

Status: Part 1 and Part 3 implemented (2026-10-10; no ABI change). Part 2 (`guard_call`) and `ServiceRef` are not built. As built: the Windows filter walks at most 32 frames and stops at the guard's own frame; a stack overflow is not walked (blamed on the owner); the faulting-module trace is per thread and read by `CallPlugin`; load, unload, query and script-load faults keep owner blame; the pin flag is per owner and set by `QueryService`, read after `Release` (when no new table can be handed out); the POSIX default guard doesn't contain faults at all, so POSIX has owner blame unless a test guard reports the module. The problem section below describes the code before this change. Audit item M1 (2026-10-10): `query_service` hands the caller the provider's own function table, so the host neither guards nor tracks calls into it.

## The problem, in today's code

- `QueryService` (`src/api/sco_services.cpp:63`) returns `s.table`, the provider's pointer. The caller calls through it directly.
- **Wrong blame.** A fault inside the provider unwinds to the nearest guard, which is the caller's callout (`ContainedCallout`, `src/plugins/loader.cpp:304`). The caller is marked Crashed and unloaded. The provider stays Loaded and keeps faulting for the next caller. Off the game thread, nothing catches it at all.
- **Use after unload.** When a provider unloads or crashes, `detail::ReleaseServices` drops its entries, and `UnloadCode` then `FreeLibrary`s the module (`loader.cpp:329`). A caller that kept the table jumps into unmapped memory.
- **Built-ins are not affected** by the unload half (they never unmap), but they are by the blame half.

## Goals and limits

- `sco_api.h` stays append-only. `provide_service`, `query_service` and `release_service` keep their signatures and meaning, and 1.0/1.1 plugins work unchanged.
- A caller that doesn't opt in pays nothing on the fast path. One that does pays one indirect call and one atomic load.
- No JIT and no generated thunks. The host cannot wrap an opaque C table it can't describe (unknown arity, unknown calling contract).

## Part 1: blame the code that faulted (no ABI change)

The guard already sees the exception. On Windows, `DefaultGuard`'s filter gets `EXCEPTION_POINTERS`. It will walk the faulting thread's frames from `ContextRecord` (`RtlLookupFunctionEntry` + `RtlVirtualUnwind`, at most 32 frames) and map each instruction pointer to a module (`GetModuleHandleExW(FROM_ADDRESS | UNCHANGED_REFCOUNT)`). The first frame inside a **plugin module** names the culprit.

- **Culprit = callout owner:** today's behavior.
- **Culprit = another plugin (a provider):** that plugin is marked Crashed. Its services are withdrawn and its callouts stop. The caller's callout still did not complete, because its stack was unwound, but the caller is **not** marked Crashed. `mod.log`: `[plugin] <provider> crashed in a service call from <caller> (0x...)`.
- **No plugin frame** (a fault in game or host code): blame the owner, as today.
- **Built-ins** share the product's module, so the walk can't tell them apart. They keep today's owner-based blame. That's acceptable: a built-in fault is a product bug.
- **Off the game thread** the filter can only record the culprit. `MarkCrashed` is game-thread only, so it is queued with `run_on_game_thread` semantics. Threads the host doesn't own stay unguarded, as now.
- **POSIX (host tests, sco-host-sim):** the signal guard gets the same walk via `_Unwind_Backtrace` + `dladdr`. If that proves unreliable, the POSIX side keeps owner blame; it is test-only.

This fixes wrong blame for every existing plugin.

## Part 2: a provider-side guard (opt-in, `sco_api` 1.2)

Part 1 still aborts the caller's whole callout. A provider that wants its caller to get an error code instead wraps its entry points with a new appended function:

```c
/* 1.2. Any thread. Runs fn(ctx) as `provider`: SCO_OK; SCO_CRASHED when fn faulted (provider is
 * then marked crashed); SCO_UNAVAILABLE when provider is no longer loaded (fn not called). */
sco_result (*guard_call)(sco_plugin* provider, void (*fn)(void* ctx), void* ctx);
```

The SDK hides the thunk. `scosdk/service.hpp` gets `scosdk::guarded<&Impl>`, which turns `R Impl(Args...)` into a C entry point that packs the arguments, calls `guard_call` and maps the result to a declared error value. A C macro does the same for plain C. Per call this costs one indirect call, a `__try` frame (table-based on x64, free when nothing faults) and one atomic state load.

The `SCO_UNAVAILABLE` branch also covers a caller that kept the table of a provider that has since unloaded or crashed: the wrapper refuses before touching provider state. That only works if the code is still mapped (Part 3).

## Part 3: unload safety

**Now: pin.** A native module whose service table was ever handed out by `query_service` is never unmapped during the session. On unload or crash its services are withdrawn and its state changes as today, but `ops->close` is skipped and logged as `kept mapped: its service table was handed out`. It costs a few hundred KB at most, only for providers, and only when a plugin unloads mid-session. That is rare today: hot reload is a v1 non-goal and normal unload happens at game exit. With Part 2, calls into a pinned, unloaded provider answer `SCO_UNAVAILABLE`. Without it, they run the provider's code against its torn-down state, which is the provider's bug to guard.

**Later, with hot reload: `ServiceRef`.** Appended (1.2 or later):

```c
sco_result (*acquire_service)(sco_plugin* self, const char* name, uint32_t min_version, const void** out_vtable);
sco_result (*release_service_ref)(sco_plugin* self, const char* name);
```

- The host counts references per (consumer, service). A consumer's references drop when it unloads or crashes.
- A provider whose count is 0 and that was never handed out through plain `query_service` can be unmapped after unload. Otherwise it stays pinned.
- `query_service` remains, and counts as a permanent reference (pin).
- A reload of the provider gets a new table. Holders must re-acquire; the host publishes `service.changed` (name) so they know.

## What changes where

- **Part 1:** `src/plugins/guard_win.cpp` (the filter), `loader.cpp` (`MarkCrashed` for a culprit other than the owner, queued off-thread), `ContainedCallout`. Test: `test_plugins` gets a fake provider plugin whose service function faults; a second fake plugin calls it from a command. Expect the provider Crashed, the caller Loaded, and the command answering `crashed`.
- **Part 2:** `sco_api.h` + `tests/abi_v1.c` (size 128 -> 136, one pinned offset), `sco_host.cpp`, `scosdk/service.hpp`, the C# wrapper (and sco-lua, if scripts may provide services), docs. Test: same fixture, provider wrapped. The caller gets `SCO_CRASHED` and continues; after unload it gets `SCO_UNAVAILABLE`.
- **Part 3 (pin):** `loader.cpp` `UnloadCode` checks a "table handed out" flag set by `QueryService`. Test: a counting `ModuleOps.close`.

## Recommendation

Ship Part 1 and the pin first, as one sco-core PR with no ABI change. They fix both reported defects for every plugin that exists today. Part 2 follows as the 1.2 ABI append once a third-party provider asks for error codes instead of aborted callouts. `ServiceRef` waits for hot reload.

## Rejected

- **Host trampolines per service function.** These need each table's layout and signatures, which an opaque C table doesn't carry, or a JIT thunk pool. Both are fragile and slow.
- **Returning a host proxy table.** This is the same problem: the host can't generate forwarding functions without signatures.
- **Making every caller guard its own service calls.** It puts the burden on every consumer and still blames the consumer.
