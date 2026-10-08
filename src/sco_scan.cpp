#include "sco/scan.h"
#include <cstdlib>
#include <cstring>

namespace sco {

const uint8_t* FindCString(const Section& s, const char* str) {
    const size_t n = strlen(str) + 1;
    if (!s.base || s.size < n) return nullptr;
    uint8_t* const end = s.base + s.size;
    for (uint8_t* p = s.base; p + n <= end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, str[0], static_cast<size_t>(end - p) - n + 1));
        if (!p) break;
        if ((p == s.base || p[-1] == 0) && memcmp(p, str, n) == 0) return p;
    }
    return nullptr;
}

int32_t Rel32(const uint8_t* p) { int32_t v; memcpy(&v, p, sizeof(v)); return v; }

bool BytesMatch(const uint8_t* p, const char* pattern) {
    for (const char* c = pattern; *c; ) {
        if (*c == ' ') { ++c; continue; }
        if (c[0] == '?') { ++p; c += (c[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { c[0], c[1], 0 };
        if (*p++ != static_cast<uint8_t>(strtoul(hex, nullptr, 16))) return false;
        c += 2;
    }
    return true;
}

uint8_t* FindRipLea(const Section& text, uint8_t reg0, uint8_t reg1, uint8_t reg2, const uint8_t* target) {
    if (!text.base || text.size < 7) return nullptr;
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, reg0, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] == reg1 && p[2] == reg2 && p + 7 + Rel32(p + 3) == target) return p;
    }
    return nullptr;
}

int FindPattern(const Section& text, const char* pattern, uint8_t** out, int max) {
    uint8_t bytes[kMaxPatternBytes]; bool wild[kMaxPatternBytes]; size_t n = 0;
    for (const char* c = pattern; *c; ) {
        if (*c == ' ') { ++c; continue; }
        if (n == kMaxPatternBytes) return 0;
        if (c[0] == '?') { wild[n] = true; bytes[n++] = 0; c += (c[1] == '?') ? 2 : 1; continue; }
        char hex[3] = { c[0], c[1], 0 };
        bytes[n] = static_cast<uint8_t>(strtoul(hex, nullptr, 16)); wild[n++] = false; c += 2;
    }
    if (n == 0 || !text.base || text.size < n) return 0;
    int matches = 0;
    uint8_t* const end = text.base + text.size - n;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, bytes[0], static_cast<size_t>(end - p)));
        if (!p) break;
        size_t i = 1;
        while (i < n && (wild[i] || p[i] == bytes[i])) ++i;
        if (i == n) { if (matches < max) out[matches] = p; ++matches; }
    }
    return matches;
}

uint8_t* FindUniquePattern(const Section& text, const char* pattern, int& matches) {
    uint8_t* hit = nullptr;
    matches = FindPattern(text, pattern, &hit, 1);
    return matches == 1 ? hit : nullptr;
}

}  // namespace sco
