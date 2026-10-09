// Signature rows for the engine's system object. New in sco-core (not moved from sc-offline):
// found in build 4.10.193.11644 (CL 12660092) at RVA 0x7237c60, the .pdata start of the only
// function that references the string below.
#include "sco/game/system.h"
#include "sco/scan.h"
#include "sco/signatures.h"

namespace sco::game {

namespace {

// The "<SystemQuit>" log line CSystem::Quit writes before it shuts the game down.
constexpr char kQuitLog[] =
    "CSystem::Quit invoked with - cause=$$, reason=$$, exitCode=$$, thread id=$$, main thread id=$$";

// The function references kQuitLog twice (lea r9 at +0xA3 and +0x176, one per log path).
// FindRipLea returns the first; the check at +0x176 proves the second belongs to the same function.
SigResult ResolveQuit(const Image& img) {
    const uint8_t* fmt = FindCString(img.rdata, kQuitLog);
    if (!fmt) return { SigState::Missing, nullptr, 0, nullptr };
    uint8_t* lea = FindRipLea(img.text, 0x4C, 0x8D, 0x0D, fmt);
    if (!lea) return SigFail("CSystem::Quit log line isn't referenced");
    uint8_t* f = lea - 0xA3;
    static const struct { size_t off; const char* bytes; const char* why; } kChecks[] = {
        { 0x000, "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20", "layout changed at +0x000" },
        { 0x00F, "55 41 54 41 55 41 56 41 57",                   "layout changed at +0x00f" },
        { 0x018, "48 8D AC 24 00 FF FF FF",                      "layout changed at +0x018" },
        { 0x020, "48 81 EC 00 02 00 00",                         "layout changed at +0x020" },
        { 0x027, "45 8B F8 48 8B F2 4C 8B F1",                   "layout changed at +0x027" },
        { 0x035, "41 83 F8 4E",                                  "layout changed at +0x035" },
        { 0x0BB, "4C 8D 05 ?? ?? ?? ??",                         "layout changed at +0x0bb" },
        { 0x0DD, "E8 ?? ?? ?? ??",                               "layout changed at +0x0dd" },
        { 0x176, "4C 8D 0D ?? ?? ?? ??",                         "layout changed at +0x176" },
    };
    for (const auto& c : kChecks)
        if (!BytesMatch(f + c.off, c.bytes)) return SigFail(c.why);
    if (RipTarget(f + 0x176, 3, 7) != fmt) return SigFail("second Quit log line not at +0x176");
    return SigOk(f);
}

}  // namespace

extern const SigDef kSystemSignatures[] = {
    { "system.quit", nullptr, 0, 0, ResolveQuit, {} },
};
extern const size_t kSystemSignatureCount = sizeof(kSystemSignatures) / sizeof(kSystemSignatures[0]);

bool QuitFunction(QuitHook& out) {
    uint8_t* f = Sig("system.quit");
    if (!f) return false;
    out.fn          = f;
    out.stolenBytes = kQuitStolenBytes;
    return true;
}

}  // namespace sco::game
