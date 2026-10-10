// Signature rows for game.entities (docs/design/game-world-spikes.md B3 and B9): the entity class
// name, the game's "for each entity" walk behind query_radius, and the two functions every entity
// passes through when it streams in (CEntitySystem::CallOnSpawnSinks) and out
// (CEntitySystem::DeleteEntity). Each row is the locator and the byte checks the spike lists, and
// insists on the match count found in 4.10.196.36804, so a game patch that adds or loses a site
// shows up as a failed row.
#include "sco/game/entities.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <cstring>
#include <iterator>

namespace sco::game {

namespace {

using rows::Check;
using rows::FnSpec;
using rows::ResolveFn;
using rows::ResolveRip;
using rows::RipSpec;

SigResult Missing()        { return { SigState::Missing, nullptr, 0, nullptr }; }
SigResult Ambiguous(int n) { return { SigState::Ambiguous, nullptr, n, nullptr }; }

// Every `reg0 reg1 reg2 disp32` RIP-relative instruction in .text whose target is `target`, in
// address order. Returns the total; up to `max` are written to `out`.
int RipRefs(const Section& text, uint8_t reg0, uint8_t reg1, uint8_t reg2, const uint8_t* target,
            const uint8_t** out, int max) {
    if (!text.base || text.size < 7 || !target) return 0;
    int n = 0;
    const uint8_t* const end = text.base + text.size - 7;
    for (const uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, reg0, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] == reg1 && p[2] == reg2 && p + 7 + Rel32(p + 3) == target) {
            if (n < max) out[n] = p;
            ++n;
        }
    }
    return n;
}

bool ChecksMatch(const Image& img, const uint8_t* at, const Check* checks, size_t n, const char** why) {
    for (size_t i = 0; i < n; ++i) {
        const Check& c = checks[i];
        if (!rows::InText(img, at + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(at + c.off, c.bytes)) {
            *why = c.why;
            return false;
        }
    }
    return true;
}

// The target of the call (E8 rel32) at `off` in row `from`.
struct CallSpec {
    const char* from;
    size_t      off;
    const char* why;
};

template <const CallSpec& S>
SigResult ResolveCall(const Image& img) {
    const uint8_t* from = Sig(S.from);
    if (!from) return SigFail("source row missing");
    if (!rows::InText(img, from + S.off, 5) || from[S.off] != 0xE8) return SigFail(S.why);
    const uint8_t* t = RipTarget(from + S.off, 1, 5);
    if (!rows::InText(img, t, 1)) return SigFail("call target outside .text");
    return SigOk(t);
}

// ---- EntitySystemDump: the walk and the class name (B3) -----------------------------------------------

// The dump without a name filter calls ForEach(*g_entityIndex, &callable, flag) with a lambda that
// reads each entity's class name (entity slot 0x20, then that class's slot 0x18) and its id (entity
// slot 0x08). The pattern is unique on its own; the checks pin the lambda's address (+0x3B), the
// ForEach call (+0x46) and the entity count read (+0x24D).
constexpr Check kDumpChecks[] = {
    { 0x03B, "48 8D 05",             "EntitySystemDump changed at +0x03b (the lambda)" },
    { 0x046, "E8",                   "EntitySystemDump changed at +0x046 (the ForEach call)" },
    { 0x24D, "44 8B 86 28 01 00 00", "EntitySystemDump changed at +0x24d (the entity count)" },
};
constexpr FnSpec kDump{ "48 8B 0D ?? ?? ?? ?? 48 8D 55 E7 48 89 45 F7 41 80 E0 01 48 8D 45 B7",
                        kDumpChecks, std::size(kDumpChecks), nullptr };

// The entity index global: the operand of the mov rcx at the pattern's start.
constexpr RipSpec kIndex{ "entities.dump", 0x00, 3, 7 };
constexpr CallSpec kForEach{ "entities.dump", 0x46, "no ForEach call at +0x046" };

// The dump's lambda: void (ctx, entity handle). The checks pin the handle mask, the two virtual
// calls that read the class name, the entity slot 0x398 call and the id read; they are the
// offsets entities.cpp relies on (entities::kEntityClass, kClassName, kEntityId).
constexpr Check kClassSiteChecks[] = {
    { 0x000, "48 89 5C 24 10 48 89 74 24 18 57 48 83 EC 20 48 B8 FF FF FF FF FF FF 00 00 48 8B FA 48 23 F8",
      "the dump's lambda changed at +0x000" },
    { 0x028, "48 8B 07 FF 50 20 48 8B C8 48 8B 10 FF 52 18", "the lambda's class name read changed (+0x028)" },
    { 0x06E, "FF 90 98 03 00 00",                             "the lambda's entity slot 0x398 changed (+0x06e)" },
    { 0x0AB, "48 8B 07 48 8D 54 24 30 48 8B 5E 20 48 8B CF FF 50 08", "the lambda's id read changed (+0x0ab)" },
};

SigResult ResolveClassSite(const Image& img) {
    const uint8_t* dump = Sig("entities.dump");
    if (!dump) return SigFail("source row missing");
    const uint8_t* fn = RipTarget(dump + 0x3B, 3, 7);
    if (!rows::InText(img, fn, 0xBD)) return SigFail("the lambda is outside .text");
    const char* why = nullptr;
    if (!ChecksMatch(img, fn, kClassSiteChecks, std::size(kClassSiteChecks), &why)) return SigFail(why);
    return SigOk(fn);
}

// ---- the streaming hooks (B9) --------------------------------------------------------------------------

// The function that loads `label` with `r0 r1 r2` (a RIP-relative lea) exactly once, whose
// FunctionStart is `leaOff` bytes before that lea.
SigResult LabelFunction(const Image& img, const char* label, uint8_t r0, uint8_t r1, uint8_t r2, size_t leaOff) {
    const uint8_t* str = FindCString(img.rdata, label);
    if (!str) return SigFail("the label isn't in .rdata");
    const uint8_t* refs[4] = {};
    const int n = RipRefs(img.text, r0, r1, r2, str, refs, 4);
    if (n == 0) return Missing();
    if (n > 1) return Ambiguous(n);
    const uint8_t* fn = FunctionStart(img, refs[0]);
    if (!fn) return SigFail("no .pdata entry for the function");
    if (static_cast<size_t>(refs[0] - fn) != leaOff) return SigFail("the label isn't loaded where it was");
    return SigOk(fn);
}

// void CEntitySystem::CallOnSpawnSinks(es* rcx, const std::vector<IEntityPtr>* rdx): hands the
// batch to every registered sink (the sink list at [es+0x30 .. es+0x38], sink->vtbl[1](sink, rdx)).
constexpr Check kSpawnSinksChecks[] = {
    { 0x000, "4C 8B DC 49 89 5B 10 49 89 6B 18 49 89 73 20 57 48 81 EC 80 00 00 00", "CallOnSpawnSinks changed at +0x000" },
    { 0x070, "48 8B 5F 30 33 C9 48 8B 7F 38",                                       "CallOnSpawnSinks' sink list changed (+0x070)" },
    { 0x0F0, "48 8B 0B 48 8B D5 48 8B 01 FF 50 08",                                  "CallOnSpawnSinks' sink call changed (+0x0f0)" },
};

SigResult ResolveSpawnSinks(const Image& img) {
    SigResult r = LabelFunction(img,
        "void __cdecl CEntitySystem::CallOnSpawnSinks(const class std::vector<class IEntityPtr,struct TempAllocator<class IEntityPtr> > &)",
        0x48, 0x8D, 0x05, 0x17);
    if (r.state != SigState::Ok) return r;
    const char* why = nullptr;
    if (!ChecksMatch(img, r.at, kSpawnSinksChecks, std::size(kSpawnSinksChecks), &why)) return SigFail(why);
    return r;
}

// void CEntitySystem::DeleteEntity(es* rcx, CEntityPtr* rdx): the entity is *rdx & 0xFFFFFFFFFFFF.
// Both of its labels must lead to the same function.
constexpr Check kDeleteChecks[] = {
    { 0x000, "48 8B C4 48 89 58 10 48 89 70 20 57 41 54 41 55 41 56 41 57", "DeleteEntity changed at +0x000" },
    { 0x026, "48 8B FA",                                                     "DeleteEntity changed at +0x026" },
    { 0x0BB, "48 8B 1F 49 23 DC",                                            "DeleteEntity's handle mask changed (+0x0bb)" },
};

SigResult ResolveDeleteEntity(const Image& img) {
    SigResult r = LabelFunction(img, "CEntitySystem::DeleteEntity", 0x4C, 0x8D, 0x05, 0x6C);
    if (r.state != SigState::Ok) return r;
    const SigResult other = LabelFunction(img, "void __cdecl CEntitySystem::DeleteEntity(class CEntityPtr)", 0x48, 0x8D, 0x05, 0x16B);
    if (other.state != SigState::Ok) return other;
    if (other.at != r.at) return SigFail("DeleteEntity's two labels lead to different functions");
    const char* why = nullptr;
    if (!ChecksMatch(img, r.at, kDeleteChecks, std::size(kDeleteChecks), &why)) return SigFail(why);
    return r;
}

// ---- capabilities ----------------------------------------------------------------------------------

constexpr const char* kClassName[] = { "teleport.entity_system", "entities.dump", "entities.class_site" };
constexpr const char* kEnumerate[] = { "teleport.entity_system", "entities.dump", "entities.index", "entities.for_each", "entities.class_site" };
constexpr const char* kStreamHooks[] = { "teleport.entity_system", "entities.class_site", "entities.spawn_sinks", "entities.delete_entity" };

constexpr entities::Capability kCaps[] = {
    { "entities.class_name",  kClassName,   std::size(kClassName) },
    { "entities.enumerate",   kEnumerate,   std::size(kEnumerate) },
    { "entities.stream_hooks", kStreamHooks, std::size(kStreamHooks) },
};

}  // namespace

namespace entities {

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace entities

extern const SigDef kEntitiesSignatures[] = {
    { "entities.dump",          nullptr, 0, 0, ResolveFn<kDump>,          {} },
    { "entities.index",         nullptr, 0, 0, ResolveRip<kIndex>,        { "entities.dump" } },
    { "entities.for_each",      nullptr, 0, 0, ResolveCall<kForEach>,     { "entities.dump" } },
    { "entities.class_site",    nullptr, 0, 0, ResolveClassSite,          { "entities.dump" } },
    { "entities.spawn_sinks",   nullptr, 0, 0, ResolveSpawnSinks,         {} },
    { "entities.delete_entity", nullptr, 0, 0, ResolveDeleteEntity,       {} },
};
extern const size_t kEntitiesSignatureCount = std::size(kEntitiesSignatures);

}  // namespace sco::game
