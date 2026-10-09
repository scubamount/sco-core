// Windows crash guard: structured exception handling around one call into plugin code.
// No C++ objects with destructors in this function (MSVC C2712), so __try is allowed.
// Catches every SEH exception (access violation, divide by zero, illegal instruction, a C++
// throw escaping the plugin). Stack corruption and fast-fail (__fastfail, /GS) are not catchable:
// this limits damage, it is not a sandbox.
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>   // _resetstkoflw
#include <stdint.h>   // C header: this file also builds with clang for mingw, without libstdc++ headers

namespace sco::plugins::detail {

uint32_t DefaultGuard(void (*thunk)(void* ctx), void* ctx) {
    DWORD code = 0;
    __try {
        thunk(ctx);
    } __except (code = GetExceptionCode(), EXCEPTION_EXECUTE_HANDLER) {
        // The overflow used up the thread's guard page; without it the next overflow on this
        // (game) thread kills the process instead of raising. The stack is unwound here.
        if (code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();
        return code ? static_cast<uint32_t>(code) : 0xFFFFFFFFu;   // never report 0 for a fault
    }
    return 0;
}

}  // namespace sco::plugins::detail
#endif
