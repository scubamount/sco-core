// Signature rows for CryPak and the DataCore loader, moved byte for byte from sc-offline's
// src/quantum.cpp (ResolveQuantumApi, origin/main dd79026, lines 417-436): the same string, the
// same lea r9 site, the same prologue, CryPak pattern and slot calls. One change of mechanism:
// the function start comes from the image's .pdata through FunctionStart (what
// RtlLookupFunctionEntry does in game), so sco-sigcheck resolves the rows from the exe on disk.
// Two additions: a second lea r9 reference or a second CryPak load naming a different
// function or global makes the row Ambiguous instead of taking the first.
#include "sco/game/pak.h"
#include "sco/scan.h"
#include "sco/signatures.h"

namespace sco::game {

namespace {

constexpr char kDcbSmaller[] = "DCB file is smaller than expected";
constexpr char kLoaderPrologue[] = "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 55 41 54 41 55 41 56 41 57";
constexpr char kCryPakLoad[] = "48 8B 0D ?? ?? ?? ?? 4C 8D 05 ?? ?? ?? ?? 48 8B 55 ?? 45 33 C9 48 8B 01 FF 90 48 01 00 00";
constexpr size_t kCryPakLoadBytes = 30;
constexpr size_t kCryPakWindow = 0x400;    // the CryPak load is in the loader's first 0x400 bytes
constexpr size_t kSlotWindow = 0x2400;     // the slot calls are in its first 0x2400 bytes

bool InText(const Image& img, const uint8_t* p, size_t n) {
    return p >= img.text.base && p <= img.text.base + img.text.size
        && static_cast<size_t>(img.text.base + img.text.size - p) >= n;
}

// The loader: the function holding `lea r9, [rip+"DCB file is smaller than expected"]`.
SigResult ResolveLoader(const Image& img) {
    const uint8_t* msg = FindCString(img.rdata, kDcbSmaller);
    if (!msg) return { SigState::Missing, nullptr, 0, nullptr };
    uint8_t* site = FindRipLea(img.text, 0x4C, 0x8D, 0x0D, msg);
    if (!site) return SigFail("DCB size message isn't referenced");
    uint8_t* loader = FunctionStart(img, site);
    if (!loader) return SigFail("no .pdata entry for the DCB size message's function");
    if (!InText(img, loader, 24) || !BytesMatch(loader, kLoaderPrologue)) return SigFail("loader prologue changed");
    // Every other lea r9 of the message must be in the same function.
    int sites = 1;
    for (uint8_t* p = site + 1; InText(img, p, 7);) {
        const Section rest{ p, static_cast<size_t>(img.text.base + img.text.size - p) };
        uint8_t* next = FindRipLea(rest, 0x4C, 0x8D, 0x0D, msg);
        if (!next) break;
        ++sites;
        if (FunctionStart(img, next) != loader) return { SigState::Ambiguous, nullptr, sites, "DCB size message used by two functions" };
        p = next + 1;
    }
    return SigOk(loader);
}

// The ICryPak* global, from the loader's FOpen call (slot 0x148).
SigResult ResolveCryPak(const Image& img) {
    uint8_t* loader = Sig("pak.datacore_loader");
    if (!loader) return SigFail("pak.datacore_loader missing");
    if (!InText(img, loader, kCryPakWindow + kCryPakLoadBytes)) return SigFail("loader too close to the end of .text");
    uint8_t* global = nullptr;
    int found = 0;
    for (size_t i = 0; i < kCryPakWindow; ++i) {
        if (!BytesMatch(loader + i, kCryPakLoad)) continue;
        uint8_t* g = RipTarget(loader + i, 3, 7);
        ++found;
        if (!global) global = g;
        else if (g != global) return { SigState::Ambiguous, nullptr, found, "two CryPak globals in the loader" };
    }
    if (!global) return SigFail("CryPak FOpen call not in the loader's first 0x400 bytes");
    return SigOk(global);
}

// The loader calls read, seek and close through the same vtable: call [rax+slot] (FF 90 <slot>).
SigResult CheckSlots(const Image& img) {
    uint8_t* loader = Sig("pak.datacore_loader");
    if (!loader) return SigFail("pak.datacore_loader missing");
    if (!InText(img, loader, kSlotWindow + 6)) return SigFail("loader too close to the end of .text");
    static const struct { size_t slot; const char* why; } kSlots[] = {
        { pak::kReadSlot,  "no call [rax+0x160] (CryPak read) in the loader" },
        { pak::kSeekSlot,  "no call [rax+0x1d0] (CryPak seek) in the loader" },
        { pak::kCloseSlot, "no call [rax+0x1e0] (CryPak close) in the loader" },
    };
    uint8_t* first = nullptr;
    for (const auto& s : kSlots) {
        uint8_t* at = nullptr;
        for (size_t i = 0; !at && i < kSlotWindow; ++i)
            if (loader[i] == 0xFF && loader[i + 1] == 0x90 && Rel32(loader + i + 2) == static_cast<int32_t>(s.slot)) at = loader + i;
        if (!at) return SigFail(s.why);
        if (!first) first = at;
    }
    return SigOk(first);   // the read call
}

}  // namespace

extern const SigDef kPakSignatures[] = {
    { "pak.datacore_loader", nullptr, 0, 0, ResolveLoader, {} },
    { "pak.crypak",          nullptr, 0, 0, ResolveCryPak, { "pak.datacore_loader" } },
    { "pak.slots",           nullptr, 0, 0, CheckSlots,    { "pak.datacore_loader" } },
};
extern const size_t kPakSignatureCount = sizeof(kPakSignatures) / sizeof(kPakSignatures[0]);

namespace pak {

bool Resolve(Targets& out) {
    uint8_t* loader = Sig("pak.datacore_loader");
    uint8_t* global = Sig("pak.crypak");
    if (!loader || !global || !SigReady("pak.slots")) return false;
    out.loader = loader;
    out.cryPak = reinterpret_cast<uintptr_t*>(global);
    return true;
}

}  // namespace pak

}  // namespace sco::game
