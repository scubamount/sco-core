#pragma once
// Table-driven resolvers for rows that are "a unique pattern plus byte checks at offsets from
// the match" or "a RIP-relative operand inside another row". Private to src/game/: each table
// declares its specs as constexpr objects and instantiates ResolveFn / ResolveRip with them,
// so every row still gets its own SigResolver and its own static failure reasons.
#include "sco/scan.h"
#include "sco/signatures.h"
#include <cstddef>
#include <cstdint>

namespace sco::game::rows {

struct Check {
    size_t      off;     // from the match
    const char* bytes;   // BytesMatch pattern
    const char* why;     // static reason when it doesn't match
};

// Extra validation after the checks pass: nullptr = OK, else a static reason. May call Sig() on
// the row's needs.
using Extra = const char* (*)(const Image& img, const uint8_t* at);

struct FnSpec {
    const char*  pattern;   // unique in .text
    const Check* checks;
    size_t       nChecks;
    Extra        extra;
};

struct RipSpec {
    const char* from;      // the row holding the instruction (must be in the row's needs)
    size_t      off;       // instruction offset from that row's address
    size_t      dispOff;   // displacement offset within the instruction
    size_t      size;      // instruction length
};

inline bool InText(const Image& img, const uint8_t* p, size_t n) {
    return p >= img.text.base && p <= img.text.base + img.text.size
        && static_cast<size_t>(img.text.base + img.text.size - p) >= n;
}

inline size_t PatternBytes(const char* p) {
    size_t n = 0;
    for (; *p; ++p)
        if (*p != ' ' && (p[1] == ' ' || p[1] == 0)) ++n;
    return n;
}

template <const FnSpec& S>
SigResult ResolveFn(const Image& img) {
    SigResult r = SigPattern(img.text, S.pattern);
    if (r.state != SigState::Ok) return r;
    for (size_t i = 0; i < S.nChecks; ++i) {
        const Check& c = S.checks[i];
        if (!InText(img, r.at + c.off, PatternBytes(c.bytes)) || !BytesMatch(r.at + c.off, c.bytes)) return SigFail(c.why);
    }
    if (S.extra)
        if (const char* why = S.extra(img, r.at)) return SigFail(why);
    return r;
}

// The RIP target of an operand in another row; fails when it points outside the image.
template <const RipSpec& S>
SigResult ResolveRip(const Image& img) {
    const uint8_t* from = Sig(S.from);
    if (!from) return SigFail("source row missing");
    const uint8_t* t = RipTarget(from + S.off, S.dispOff, S.size);
    if (img.base && img.size && (t < img.base || t >= img.base + img.size)) return SigFail("RIP target outside the image");
    return SigOk(t);
}

}  // namespace sco::game::rows
