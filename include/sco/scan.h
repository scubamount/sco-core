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

// Target of a RIP-relative operand: the instruction at `insn`, displacement at `insn + dispOffset`,
// instruction length `insnSize`.
inline uint8_t* RipTarget(const uint8_t* insn, size_t dispOffset, size_t insnSize) {
    return const_cast<uint8_t*>(insn) + insnSize + Rel32(insn + dispOffset);
}

#ifdef _WIN32
Image ModuleImage();   // the process's main executable
#endif

}  // namespace sco
