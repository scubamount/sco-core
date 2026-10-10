#include "sco/game/missions.h"
#include "sco/scan.h"
#include <windows.h>

namespace sco::game::missions {

namespace {

// Checked at runtime (see missions.h): the vtable slot of the function whose code names the
// library's field, the canary inside it, and where that code keeps the field's displacement.
constexpr size_t kFunctionSlot = 0x48;
constexpr size_t kCanaryOffset = 0x32;
constexpr size_t kDispOffset   = 0x35;
constexpr const char* kCanary  = "48 8B 4B";

template <typename T> T Rd(uintptr_t p) { return *reinterpret_cast<const T*>(p); }

}  // namespace

// No C++ object with a destructor lives in this frame (MSVC C2712).
uintptr_t ScriptLibrary(uintptr_t manager, const char** reason) {
    uintptr_t   library = 0;
    const char* why = kReasonNotFound;
    __try {
        const uint8_t* fn = manager ? reinterpret_cast<const uint8_t*>(Rd<uintptr_t>(Rd<uintptr_t>(manager) + kFunctionSlot)) : nullptr;
        if (fn && BytesMatch(fn + kCanaryOffset, kCanary)) {
            library = Rd<uintptr_t>(manager + fn[kDispOffset]);
            why = library ? nullptr : kReasonNotYet;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        library = 0;
        why = kReasonNotFound;
    }
    if (reason) *reason = why;
    return library;
}

}  // namespace sco::game::missions
