#pragma once
// Byte scanning over a loaded PE image. Pure C++: no Windows headers, so the same code runs
// inside the game and in host tools that map StarCitizen.exe from disk.
#include <cstddef>
#include <cstdint>

namespace sco {

struct Section { uint8_t* base = nullptr; size_t size = 0; };

// The parts of the game image the scanners need. `base` is where the image starts in memory
// (the module handle in game, a buffer laid out at section RVAs in host tools).
struct Image {
    uint8_t* base      = nullptr;
    Section  text, rdata;
    Section  pdata;           // the exception directory: RUNTIME_FUNCTION entries (12 bytes each)
    uint32_t timestamp = 0;   // PE header TimeDateStamp
    uint32_t size      = 0;   // SizeOfImage
};

// "48 8B ?? 05" style patterns: two hex digits per byte, `?` or `??` for any byte.
const uint8_t* FindCString(const Section& s, const char* str);          // NUL-bounded match
int32_t        Rel32(const uint8_t* p);
bool           BytesMatch(const uint8_t* p, const char* pattern);
uint8_t*       FindRipLea(const Section& text, uint8_t reg0, uint8_t reg1, uint8_t reg2, const uint8_t* target);
// Every match, up to `max` written to `out`; returns the total count. A pattern longer than
// kMaxPatternBytes matches nothing.
constexpr size_t kMaxPatternBytes = 96;
int            FindPattern(const Section& text, const char* pattern, uint8_t** out, int max);
uint8_t*       FindUniquePattern(const Section& text, const char* pattern, int& matches);

// The start of the function containing `at`, from the image's .pdata, as RtlLookupFunctionEntry
// would find it: the RUNTIME_FUNCTION whose [begin, end) holds at, then its chained unwind info
// (UNW_FLAG_CHAININFO) followed, up to 8 links, to the primary entry. nullptr when at isn't
// inside the image, no entry holds it, or an entry or its unwind info points outside the image.
uint8_t*       FunctionStart(const Image& img, const uint8_t* at);

// Target of a RIP-relative operand: the instruction at `insn`, displacement at `insn + dispOffset`,
// instruction length `insnSize`.
inline uint8_t* RipTarget(const uint8_t* insn, size_t dispOffset, size_t insnSize) {
    return const_cast<uint8_t*>(insn) + insnSize + Rel32(insn + dispOffset);
}

#ifdef _WIN32
Image ModuleImage();   // the process's main executable
#endif

}  // namespace sco
