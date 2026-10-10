// Windows crash guard: structured exception handling around one call into plugin code.
// No C++ objects with destructors in this function (MSVC C2712), so __try is allowed.
// Catches every SEH exception (access violation, divide by zero, illegal instruction, a C++
// throw escaping the plugin). Stack corruption and fast-fail (__fastfail, /GS) are not catchable:
// this limits damage, it is not a sandbox.
//
// It also reports which modules the faulting thread was running in (docs/design/service-safety.md,
// part 1), so the loader can blame the plugin whose code faulted rather than the one whose
// callout was running: the filter walks the frames from the exception's context.
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>   // _resetstkoflw
#include <stdint.h>   // C header: this file also builds with clang for mingw, without libstdc++ headers

namespace sco::plugins::detail {

void RecordFaultTrace(void* const* modules, uint32_t n);   // loader.cpp

namespace {

constexpr uint32_t kMaxFrames = 32;   // == kMaxTraceFrames (internal.h, not included here)

#if defined(_M_X64) || defined(__x86_64__)

// The module (its base address, which is its HMODULE) that holds ip, or null.
void* ModuleOf(DWORD64 ip) {
    HMODULE m = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(static_cast<uintptr_t>(ip)), &m))
        return nullptr;
    return m;
}

// Walks the faulting thread's frames, innermost first, and appends each frame's module to
// modules[] (consecutive duplicates once), at most kMaxFrames frames. Stops at the guard's own
// frame: the stack of whatever called the guard is not part of the fault. Its own __try: a
// corrupt stack makes the unwinder fault, and a fault inside a filter would be swallowed as
// "keep searching" and kill the process. On such a fault the frames found so far are kept.
void Walk(const EXCEPTION_POINTERS* ep, uintptr_t guardFrame, void** modules, uint32_t* count) {
    __try {
        CONTEXT ctx = *ep->ContextRecord;
        for (uint32_t i = 0; i < kMaxFrames; ++i) {
            if (!ctx.Rip || ctx.Rsp > guardFrame) break;
            void* m = ModuleOf(ctx.Rip);
            if (m && (*count == 0 || modules[*count - 1] != m)) modules[(*count)++] = m;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
            if (fn) {
                PVOID handlerData = nullptr;
                DWORD64 establisher = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fn, &ctx, &handlerData, &establisher, nullptr);
            } else {   // a leaf function without unwind info: the return address is at the top of the stack
                ctx.Rip = *reinterpret_cast<const DWORD64*>(ctx.Rsp);
                ctx.Rsp += 8;
            }
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

#endif

// Runs in the filter, on the faulting thread's stack, before anything is unwound.
void TraceFault(const EXCEPTION_POINTERS* ep, uintptr_t guardFrame) {
    void* modules[kMaxFrames];
    uint32_t n = 0;
#if defined(_M_X64) || defined(__x86_64__)
    // A stack overflow leaves the thread about a page of stack: too little to walk on. The fault
    // is blamed on the owner, as before.
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_STACK_OVERFLOW) Walk(ep, guardFrame, modules, &n);
#else
    (void)ep; (void)guardFrame;
#endif
    RecordFaultTrace(modules, n);
}

}  // namespace

uint32_t DefaultGuard(void (*thunk)(void* ctx), void* ctx) {
    DWORD code = 0;
    volatile char frame = 0;   // its address bounds the walk: the guard's own frame holds it
    __try {
        thunk(ctx);
    } __except (code = GetExceptionCode(),
                TraceFault(GetExceptionInformation(), reinterpret_cast<uintptr_t>(&frame)),
                EXCEPTION_EXECUTE_HANDLER) {
        // The overflow used up the thread's guard page; without it the next overflow on this
        // (game) thread kills the process instead of raising. The stack is unwound here.
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? static_cast<uint32_t>(code) : 0xFFFFFFFFu;   // never report 0 for a fault
    }
    return 0;
}

}  // namespace sco::plugins::detail
#endif
