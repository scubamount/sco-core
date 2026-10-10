// Signature rows for sc-offline's own features: noclip's fly mode, Clear NPCs, the quantum effect
// warm-up, the reputation-service checks contracts patches and the offline OR-loop bound. Each
// resolver is the scan sc-offline ran itself (spawner.cpp, npc.cpp, quantum.cpp, contracts.cpp,
// patches.cpp), moved byte for byte; where that scan took "the last match" or "every match", the
// row now insists on the count found in 4.10.196.36804, so a game patch that adds or loses a site
// shows up as a failed row instead of a silent change.
#include "sco/game/features.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <iterator>

namespace sco::game {

namespace {

using namespace features;

SigResult Missing()          { return { SigState::Missing, nullptr, 0, nullptr }; }
SigResult Ambiguous(int n)   { return { SigState::Ambiguous, nullptr, n, nullptr }; }

// ---- noclip: the SFlyMode request wrapper (spawner.cpp) --------------------------------------

SigResult ResolveFlyMode(const Image& img) {
    const uint8_t* label = FindCString(img.rdata,
        "unsigned short __cdecl CSCActorActionHandler::Request<struct SCActorActionHandlerActions::SFlyMode,"
        "const enum ESCActorFlyMode&>(const enum ESCActorFlyMode &)");
    if (!label) return SigFail("the SFlyMode request label isn't in .rdata");
    uint8_t* wrappers[64] = {};
    const int n = FindPattern(img.text, "89 54 24 10 48 83 EC 28 48 8D 54 24 38 E8", wrappers, 64);
    if (n > 64) return SigFail("more request wrappers than the row checks");
    uint8_t* found = nullptr;
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t* req = RipTarget(wrappers[i], 0xE, 0x12);
        if (rows::InText(img, req, 0xB5) && BytesMatch(req + 0xAE, "48 8D 05") && RipTarget(req + 0xAE, 3, 7) == label) {
            found = wrappers[i];
            ++hits;
        }
    }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(found);
}

// ---- Clear NPCs: the entity system's RemoveEntity calls (npc.cpp) ----------------------------

SigResult ResolveRemoveEntityCall(const Image& img) {
    const uint8_t* es = Sig("teleport.entity_system");
    uint8_t* sites[16] = {};
    const int n = FindPattern(img.text, "48 8B 0D ?? ?? ?? ?? 48 8B 13 48 8B 01 FF 90 ?? ?? ?? ?? 48 83 C3 08 48 3B DF 75 E4",
                              sites, 16);
    if (n == 0) return Missing();
    if (n > 16) return SigFail("more RemoveEntity call sites than the row checks");
    uint8_t* first = nullptr;
    int32_t slot = 0;
    for (int i = 0; i < n; ++i) {
        if (RipTarget(sites[i], 3, 7) != es) continue;
        const int32_t s = Rel32(sites[i] + kRemoveSlotDisp);
        if (slot && s != slot) return SigFail("RemoveEntity call sites disagree on the vtable slot");
        slot = s;
        if (!first) first = sites[i];
    }
    if (!first) return SigFail("no RemoveEntity call on the entity system");
    if (slot <= 0 || slot >= 0x1000) return SigFail("RemoveEntity vtable slot out of range");
    return SigOk(first);
}

// ---- quantum: the EntityEffectSystem tag sender (quantum.cpp) --------------------------------

SigResult ResolveSendEffectTag(const Image& img) {
    const uint8_t* header = FindCString(img.rdata,
        "C:\\workspace\\CryEngine\\Code\\CryEngine\\CryCommon\\Events/VFX/EntityEffectSystem.h");
    if (!header) return SigFail("the EntityEffectSystem.h path isn't in .rdata");
    uint8_t* senders[32] = {};
    const int n = FindPattern(img.text, "48 89 5C 24 08 57 48 83 EC 50 8B 05 ?? ?? ?? ?? 48 8B FA 4C 89 44 24 20 48 8B D9 85 C0 75 19 "
                                        "41 B8 17 00 00 00 48 8D 15", senders, 32);
    if (n > 32) return SigFail("more effect senders than the row checks");
    uint8_t* found = nullptr;
    int hits = 0;
    for (int i = 0; i < n; ++i)
        if (RipTarget(senders[i] + 0x25, 3, 7) == header) { found = senders[i]; ++hits; }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(found);
}

// ---- contracts: the reputation-service checks (contracts.cpp) --------------------------------

constexpr const char* kServicesLoad = "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08";
constexpr const char* kReputationForms[] = {
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08 48 8B 51 58 48 8B C8 FF D2",
    "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08 4C 8B 41 58 48 8B C8 41 FF D0",
};

SigResult ResolveReputationServices(const Image& img) {
    const uint8_t* msg = FindCString(img.rdata, "Couldn't access reputation service internal");
    const uint8_t* lea = msg ? FindRipLea(img.text, 0x4C, 0x8D, 0x05, msg) : nullptr;
    if (!lea) return SigFail("the reputation service message isn't referenced");
    for (int back = 0; back < 0x120; ++back)
        if (BytesMatch(lea - back, kServicesLoad)) {
            const uint8_t* g = RipTarget(lea - back, 3, 7);
            if (img.base && img.size && (g < img.base || g >= img.base + img.size)) return SigFail("RIP target outside the image");
            return SigOk(g);
        }
    return SigFail("no services load before the reputation service message");
}

// Every check site that loads the services global: form A in address order, then form B. -1 when
// a form has more matches than the scan holds.
int ReputationSites(const Image& img, uint8_t** out, int max) {
    const uint8_t* services = Sig("contracts.reputation_services");
    int n = 0;
    for (const char* form : kReputationForms) {
        uint8_t* sites[32] = {};
        const int found = FindPattern(img.text, form, sites, 32);
        if (found > 32) return -1;
        for (int i = 0; i < found; ++i)
            if (RipTarget(sites[i], 3, 7) == services) {
                if (n == max) return -1;
                out[n++] = sites[i];
            }
    }
    return n;
}

template <int K>
SigResult ResolveReputationCheck(const Image& img) {
    uint8_t* sites[kReputationChecks] = {};
    if (ReputationSites(img, sites, kReputationChecks) != kReputationChecks)
        return SigFail("reputation service checks: the call site count changed");
    return SigOk(sites[K - 1]);
}

// ---- offline: the OR-loop bound (patches.cpp) ------------------------------------------------

template <int K>
SigResult ResolveOrLoop(const Image& img) {
    uint8_t* sites[kOrLoopSites + 1] = {};
    const int n = FindPattern(img.text,
        "44 8B A4 24 E8 00 00 00 8B 8C 24 D8 00 00 00 FF C3 48 FF C6 49 81 C7 90 00 00 00 "
        "48 3B 74 24 68 0F 8C ?? ?? ?? ?? 4C 8D 77 78 89 6F 08 41 8B 45 18", sites, kOrLoopSites + 1);
    if (n == 0) return Missing();
    if (n != kOrLoopSites) return SigFail("OR-loop bound: the site count changed");
    return SigOk(sites[K - 1]);
}

// ---- capabilities ----------------------------------------------------------------------------

constexpr const char* kFlyMode[] = { "spawn.request_fly_mode" };
constexpr const char* kClearNpcs[] = { "teleport.entity_system", "npc.remove_entity_call" };
constexpr const char* kEffectTag[] = { "quantum.send_effect_tag" };
constexpr const char* kReputation[] = {
    "contracts.reputation_services",
    "contracts.reputation_check.1", "contracts.reputation_check.2", "contracts.reputation_check.3",
    "contracts.reputation_check.4", "contracts.reputation_check.5", "contracts.reputation_check.6",
    "contracts.reputation_check.7", "contracts.reputation_check.8", "contracts.reputation_check.9",
    "contracts.reputation_check.10",
};
constexpr const char* kOrLoop[] = {
    "offline.or_loop_bound.1", "offline.or_loop_bound.2", "offline.or_loop_bound.3", "offline.or_loop_bound.4",
};
static_assert(std::size(kReputation) == 1 + kReputationChecks);
static_assert(std::size(kOrLoop) == kOrLoopSites);

constexpr Capability kCaps[] = {
    { "spawn.fly_mode",        kFlyMode,    std::size(kFlyMode) },
    { "npc.clear",             kClearNpcs,  std::size(kClearNpcs) },
    { "quantum.effect_tag",    kEffectTag,  std::size(kEffectTag) },
    { "contracts.reputation",  kReputation, std::size(kReputation) },
    { "offline.or_loop_bound", kOrLoop,     std::size(kOrLoop) },
};

}  // namespace

namespace features {

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace features

#define SCO_REP(k) { "contracts.reputation_check." #k, nullptr, 0, 0, ResolveReputationCheck<k>, { "contracts.reputation_services" } }
#define SCO_ORL(k) { "offline.or_loop_bound." #k, nullptr, 0, 0, ResolveOrLoop<k>, {} }

extern const SigDef kFeatureSignatures[] = {
    { "spawn.request_fly_mode",        nullptr, 0, 0, ResolveFlyMode,            {} },
    { "npc.remove_entity_call",        nullptr, 0, 0, ResolveRemoveEntityCall,   { "teleport.entity_system" } },
    { "quantum.send_effect_tag",       nullptr, 0, 0, ResolveSendEffectTag,      {} },
    { "contracts.reputation_services", nullptr, 0, 0, ResolveReputationServices, {} },
    SCO_REP(1), SCO_REP(2), SCO_REP(3), SCO_REP(4), SCO_REP(5),
    SCO_REP(6), SCO_REP(7), SCO_REP(8), SCO_REP(9), SCO_REP(10),
    SCO_ORL(1), SCO_ORL(2), SCO_ORL(3), SCO_ORL(4),
};
extern const size_t kFeatureSignatureCount = std::size(kFeatureSignatures);

#undef SCO_REP
#undef SCO_ORL

}  // namespace sco::game
