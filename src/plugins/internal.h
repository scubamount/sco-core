#pragma once
// Private to src/plugins/.
#include "sco/plugins.h"
#include <string>
#include <string_view>
#include <vector>

namespace sco::plugins::detail {

// plugin.ini [settings] (settings.cpp). A comment in a settings line starts at ';' or '#' outside
// double quotes, at the start of the line or after a space or tab.
std::string_view StripSettingsComment(std::string_view line);
// One declaration, comment stripped and trimmed: `name = type [default V] [min N] [max N] [label "x"]
// [help "x"]`. Appends to out; false with `why` ("setting 'speed': default 11 is above max 10").
bool ParseSettingLine(std::string_view line, std::vector<Setting>& out, std::string& why);

// A path component from UTF-8 text. std::filesystem::path(std::string) uses the ANSI code page on
// Windows; manifest values are UTF-8.
inline fs::path FromUtf8(const std::string& s) {
    return fs::path(std::u8string(reinterpret_cast<const char8_t*>(s.data()), s.size()));
}

// UTF-8 text of a path, '/' separated on every platform.
inline std::string ToUtf8(const fs::path& p) {
    const auto u = p.generic_u8string();
    return std::string(reinterpret_cast<const char*>(u.data()), u.size());
}

// The platform crash guard (guard_win.cpp on Windows; a plain call elsewhere).
uint32_t DefaultGuard(void (*thunk)(void* ctx), void* ctx);

// Which modules the faulting thread was running in when a guard caught a fault, innermost frame
// first (consecutive frames in one module count once). A guard reports it with RecordFaultTrace
// just before it returns a nonzero code; the loader reads it right after, to tell which plugin's
// code faulted. Per thread. A guard that doesn't report (the non-Windows default, a test guard)
// leaves the trace empty, and the fault is blamed on the callout's owner.
constexpr uint32_t kMaxTraceFrames = 32;
struct FaultTrace {
    uint32_t n = 0;
    void*    modules[kMaxTraceFrames];   // module handles, as Plugin::module holds them
};
void RecordFaultTrace(void* const* modules, uint32_t n);

}  // namespace sco::plugins::detail
