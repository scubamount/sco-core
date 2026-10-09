#pragma once
// Helpers shared by the DataCore parser (datacore.cpp) and the patcher (patch.cpp): saturating
// arithmetic, little-endian reads and bounded formatting. Not part of the public API.
#include <algorithm>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string>

namespace sco::datacore::detail {

constexpr uint64_t kMax = std::numeric_limits<uint64_t>::max();

inline uint64_t SatAdd(uint64_t a, uint64_t b) { return a > kMax - b ? kMax : a + b; }
inline uint64_t SatMul(uint64_t a, uint64_t b) { return b != 0 && a > kMax / b ? kMax : a * b; }

inline uint16_t R16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t R32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) | static_cast<uint32_t>(p[1]) << 8 | static_cast<uint32_t>(p[2]) << 16 |
           static_cast<uint32_t>(p[3]) << 24;
}

#if defined(__GNUC__)
__attribute__((format(printf, 1, 2)))
#endif
inline std::string Fmt(const char* fmt, ...) {
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (n < 0) return {};
    return std::string(buf, std::min(static_cast<size_t>(n), sizeof(buf) - 1));
}

}  // namespace sco::datacore::detail
