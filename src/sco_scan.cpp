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

uint8_t* FunctionStart(const Image& img, const uint8_t* at) {
    if (!img.base || !img.pdata.base || !at || at < img.base || at >= img.base + img.size) return nullptr;
    const uint32_t rva = static_cast<uint32_t>(at - img.base);
    struct Entry { uint32_t begin, end, unwind; };
    const auto entry = [&](size_t off, Entry& e) {   // false when the 12 bytes at off aren't inside the image
        if (off > img.size || img.size - off < sizeof(Entry)) return false;
        memcpy(&e, img.base + off, sizeof(Entry));
        return true;
    };
    if (img.pdata.base < img.base || img.pdata.base > img.base + img.size) return nullptr;
    const size_t pdata = static_cast<size_t>(img.pdata.base - img.base);
    // The entries are sorted by begin and don't overlap: binary search, as RtlLookupFunctionEntry does.
    size_t lo = 0, hi = img.pdata.size / sizeof(Entry);
    bool found = false;
    Entry e{};
    while (lo < hi) {
        const size_t mid = lo + (hi - lo) / 2;
        if (!entry(pdata + mid * sizeof(Entry), e)) return nullptr;
        if (rva < e.begin) hi = mid;
        else if (rva >= e.end) lo = mid + 1;
        else { found = true; break; }
    }
    if (!found) return nullptr;
    // A chained entry's unwind info ends with the parent RUNTIME_FUNCTION (after the 2-byte unwind
    // codes, padded to an even count): follow it to the primary function.
    constexpr uint8_t kChainInfo = 4;   // UNW_FLAG_CHAININFO
    for (int i = 0; i < 8; ++i) {
        if (e.unwind >= img.size || img.size - e.unwind < 4) return nullptr;
        const uint8_t* info = img.base + e.unwind;
        if (!((info[0] >> 3) & kChainInfo)) break;
        if (!entry(size_t{ e.unwind } + 4 + ((info[2] + 1u) & ~1u) * 2, e)) return nullptr;
    }
    return e.begin < img.size ? img.base + e.begin : nullptr;
}

uint8_t* FindUniquePattern(const Section& text, const char* pattern, int& matches) {
    uint8_t* hit = nullptr;
    matches = FindPattern(text, pattern, &hit, 1);
    return matches == 1 ? hit : nullptr;
}

}  // namespace sco
