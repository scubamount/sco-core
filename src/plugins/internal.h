#pragma once
// Private to src/plugins/.
#include "sco/plugins.h"
#include <string>

namespace sco::plugins::detail {

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

}  // namespace sco::plugins::detail
