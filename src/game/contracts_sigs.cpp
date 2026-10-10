// Signature rows for sc-offline's offline contracts (src/contracts.cpp there). Each resolver is the
// scan contracts.cpp ran itself, moved byte for byte: the string a function references and its
// prologue, the unique patterns and the call targets inside them, the vtable slots. Where that scan
// took "the first match" of something that could match more than once, the row now insists on what
// 4.10.196.36804 has (one function with that prologue, a fixed count of callers), so a game patch
// shows up as a failed row instead of a silent change. sco/game/contracts.h says how each row is
// found; docs/game/contracts.md has the table.
#include "sco/game/contracts.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <cstring>
#include <iterator>

namespace sco::game {

namespace {

using namespace contracts;
using rows::Check;
using rows::InText;
using rows::PatternBytes;
using rows::ResolveRip;
using rows::RipSpec;

SigResult Missing()        { return { SigState::Missing, nullptr, 0, nullptr }; }
SigResult Ambiguous(int n) { return { SigState::Ambiguous, nullptr, n, nullptr }; }

bool Matches(const Image& img, const uint8_t* p, const char* pattern) {
    return InText(img, p, PatternBytes(pattern)) && BytesMatch(p, pattern);
}

const uint8_t* CallAt(const Image& img, const uint8_t* insn) {
    if (!InText(img, insn, 5) || insn[0] != 0xE8) return nullptr;
    const uint8_t* t = RipTarget(insn, 1, 5);
    return InText(img, t, 1) ? t : nullptr;
}

// The next lea r64, [rip+X] (REX 48 or 4C, any register) at or after `from` whose X is `target`,
// as contracts.cpp's FindLeaTo; nullptr when there is none. Returns the REX byte.
const uint8_t* LeaTo(const Section& text, const uint8_t* target, const uint8_t* from) {
    if (!target || !text.base || text.size < 8) return nullptr;
    const uint8_t* const end = text.base + text.size - 7;
    for (const uint8_t* p = from ? from : text.base + 1; p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0x8D, static_cast<size_t>(end - p)));
        if (!p) break;
        if ((p[-1] & 0xFB) == 0x48 && (p[1] & 0xC7) == 0x05 && p + 6 + Rel32(p + 2) == target) return p - 1;
    }
    return nullptr;
}

// ---- .rdata pointers ---------------------------------------------------------------------------
// A loaded game holds relocated pointers; StarCitizen.exe read from disk (sco-sigcheck) holds them
// against the preferred base in the PE header. A slot "holds" an address in either form.

uint64_t HeaderBase(const Image& img) {
    if (!img.base || img.size < 0x40) return 0;
    uint32_t pe = 0;
    memcpy(&pe, img.base + 0x3C, 4);
    if (pe > img.size || img.size - pe < 24 + 32 || memcmp(img.base + pe, "PE\0\0", 4) != 0) return 0;
    uint16_t magic = 0;
    memcpy(&magic, img.base + pe + 24, 2);
    if (magic != 0x20B) return 0;
    uint64_t base = 0;
    memcpy(&base, img.base + pe + 24 + 24, 8);
    return base;
}

bool Holds(const Image& img, uint64_t v, const uint8_t* target) {
    if (v == reinterpret_cast<uintptr_t>(target)) return true;
    const uint64_t hb = HeaderBase(img);
    return hb && v == hb + static_cast<uint64_t>(target - img.base);
}

// The address a slot points at, in this image; nullptr when it points outside it.
const uint8_t* SlotTarget(const Image& img, const uint8_t* slot) {
    if (slot < img.base || slot + 8 > img.base + img.size) return nullptr;
    uint64_t v = 0;
    memcpy(&v, slot, 8);
    const uint64_t base = reinterpret_cast<uintptr_t>(img.base);
    if (v >= base && v - base < img.size) return img.base + (v - base);
    const uint64_t hb = HeaderBase(img);
    if (hb && v >= hb && v - hb < img.size) return img.base + (v - hb);
    return nullptr;
}

// The 8-byte .rdata slots from index `first` on that hold `target`; the first one in *at.
int SlotsHolding(const Image& img, const uint8_t* target, size_t first, const uint8_t** at) {
    int n = 0;
    *at = nullptr;
    for (size_t i = first; i < img.rdata.size / 8; ++i) {
        uint64_t v = 0;
        memcpy(&v, img.rdata.base + i * 8, 8);
        if (!Holds(img, v, target)) continue;
        if (!n) *at = img.rdata.base + i * 8;
        ++n;
    }
    return n;
}

// ---- row shapes --------------------------------------------------------------------------------

// The function holding the first lea of a .rdata string, prologue checked; no other function
// referencing the string may have that prologue (without one, no other function may reference it).
struct StrFnSpec {
    const char*  str;
    const char*  prologue;   // nullptr = no check
    const Check* checks;     // from the function start
    size_t       nChecks;
};

template <const StrFnSpec& S>
SigResult ResolveStrFn(const Image& img) {
    const uint8_t* s = FindCString(img.rdata, S.str);
    if (!s) return SigFail("the string isn't in .rdata");
    const uint8_t* lea = LeaTo(img.text, s, nullptr);
    if (!lea) return SigFail("the string isn't referenced");
    uint8_t* fn = FunctionStart(img, lea);
    if (!fn) return SigFail("no .pdata entry for the string's first reference");
    if (S.prologue && !Matches(img, fn, S.prologue)) return SigFail("prologue changed at +0x000");
    const uint8_t* seen[8] = {};
    int others = 0;
    for (const uint8_t* p = LeaTo(img.text, s, lea + 2); p; p = LeaTo(img.text, s, p + 2)) {
        const uint8_t* f = FunctionStart(img, p);
        if (f == fn || (S.prologue && !(f && Matches(img, f, S.prologue)))) continue;
        bool known = false;
        for (int i = 0; i < others && i < 8; ++i) known |= seen[i] == f;
        if (known) continue;
        if (others < 8) seen[others] = f;
        ++others;
    }
    if (others) return Ambiguous(1 + others);
    for (size_t i = 0; i < S.nChecks; ++i)
        if (!Matches(img, fn + S.checks[i].off, S.checks[i].bytes)) return SigFail(S.checks[i].why);
    return SigOk(fn);
}

// The function holding a unique pattern, prologue checked.
struct FnOfSpec { const char* pattern; const char* prologue; };

template <const FnOfSpec& S>
SigResult ResolveFnOf(const Image& img) {
    SigResult r = SigPattern(img.text, S.pattern);
    if (r.state != SigState::Ok) return r;
    uint8_t* fn = FunctionStart(img, r.at);
    if (!fn) return SigFail("no .pdata entry for the match");
    if (!Matches(img, fn, S.prologue)) return SigFail("prologue changed at +0x000");
    return SigOk(fn);
}

// The target of the call at `off` in another row, prologue checked.
struct CallSpec { const char* from; size_t off; const char* prologue; };

template <const CallSpec& S>
SigResult ResolveCall(const Image& img) {
    const uint8_t* from = Sig(S.from);
    if (!from) return SigFail("source row missing");
    const uint8_t* t = CallAt(img, from + S.off);
    if (!t) return SigFail("no call at the expected offset");
    if (!Matches(img, t, S.prologue)) return SigFail("callee prologue changed at +0x000");
    return SigOk(t);
}

// The one match of a pattern in the first `len` bytes of another row.
using Extra = const char* (*)(const Image& img, const uint8_t* at);

struct InRangeSpec { const char* from; size_t len; const char* pattern; Extra extra; };

template <const InRangeSpec& S>
SigResult ResolveInRange(const Image& img) {
    const uint8_t* from = Sig(S.from);
    if (!from) return SigFail("source row missing");
    if (!InText(img, from, S.len + PatternBytes(S.pattern))) return SigFail("range runs past .text");
    const uint8_t* hit = nullptr;
    int n = 0;
    for (const uint8_t* p = from; p < from + S.len; ++p)
        if (BytesMatch(p, S.pattern)) { if (!n) hit = p; ++n; }
    if (!n) return Missing();
    if (n > 1) return Ambiguous(n);
    if (S.extra)
        if (const char* why = S.extra(img, hit)) return SigFail(why);
    return SigOk(hit);
}

// The function in a vtable slot, prologue checked.
struct VtblFnSpec { const char* vtbl; size_t slot; const char* prologue; };

template <const VtblFnSpec& S>
SigResult ResolveVtblFn(const Image& img) {
    const uint8_t* vtbl = Sig(S.vtbl);
    if (!vtbl) return SigFail("source row missing");
    const uint8_t* fn = SlotTarget(img, vtbl + S.slot);
    if (!fn || !InText(img, fn, 1)) return SigFail("the slot doesn't point into .text");
    if (!Matches(img, fn, S.prologue)) return SigFail("prologue changed at +0x000");
    return SigOk(fn);
}

// A vtable found by the one .rdata slot holding a known function at `slot`.
struct VtblSpec { const char* fn; size_t slot; };

template <const VtblSpec& S>
SigResult ResolveVtbl(const Image& img) {
    const uint8_t* fn = Sig(S.fn);
    if (!fn) return SigFail("source row missing");
    const uint8_t* at = nullptr;
    const int n = SlotsHolding(img, fn, S.slot / 8, &at);
    if (!n) return Missing();
    if (n > 1) return Ambiguous(n);
    return SigOk(at - S.slot);
}

// ---- contracts.list ----------------------------------------------------------------------------

constexpr StrFnSpec kQueryReply{ "DebugAcceptGeneratorContract QueryAvailableContracts Success", "48 89 4C 24 08 55 53 56 57 41 55", nullptr, 0 };

// call get; mov rcx, rax; call field, with get = mov rax, [rip+X]; ret and field = mov rax, [rcx+imm8]; ret.
SigResult ResolveMissionSystemSite(const Image& img) {
    uint8_t* fn = Sig("contracts.query_reply");
    if (!fn) return SigFail("source row missing");
    uint8_t* end = fn + 0x1000;
    if (end > img.text.base + img.text.size - 13) end = img.text.base + img.text.size - 13;
    uint8_t* found = nullptr;
    int n = 0;
    for (uint8_t* p = fn; p < end; ++p) {
        if (p[0] != 0xE8 || !BytesMatch(p + 5, "48 8B C8 E8")) continue;
        const uint8_t* get = p + 5 + Rel32(p + 1);
        const uint8_t* field = p + 13 + Rel32(p + 9);
        if (!Matches(img, get, "48 8B 05 ?? ?? ?? ?? C3") || !Matches(img, field, "48 8B 41 ?? C3")) continue;
        if (!n) found = p;
        ++n;
    }
    if (!n) return Missing();
    if (n > 1) return Ambiguous(n);
    return SigOk(found);
}

SigResult ResolveMissionSystem(const Image& img) {
    const uint8_t* site = Sig("contracts.mission_system_site");
    if (!site) return SigFail("source row missing");
    const uint8_t* g = RipTarget(RipTarget(site, 1, 5), 3, 7);
    if (g < img.base || g >= img.base + img.size) return SigFail("RIP target outside the image");
    return SigOk(g);
}

constexpr RipSpec kGeneratorGet{ "contracts.mission_system_site", 8, 1, 5 };

// ---- contracts.accept --------------------------------------------------------------------------

SigResult ResolveAcceptStub(const Image& img) {
    const uint8_t* s = FindCString(img.rdata, "CContractBrokerOffline::AcceptContract is not implemented yet");
    const uint8_t* lea = s ? FindRipLea(img.text, 0x4C, 0x8D, 0x05, s) : nullptr;
    if (!lea) return SigFail("the AcceptContract message isn't referenced");
    const uint8_t* stub = lea - 0xC;
    if (!Matches(img, stub, "40 53 48 81 EC D0 00 00 00 48 8B DA 4C 8D 05")) return SigFail("prologue changed at +0x000");
    return SigOk(stub);
}

constexpr CallSpec kAcceptResolve{ "contracts.accept_stub", 0x5E, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57" };

SigResult ResolveAcceptSlot(const Image& img) {
    const uint8_t* stub = Sig("contracts.accept_stub");
    if (!stub) return SigFail("source row missing");
    const uint8_t* at = nullptr;
    const int n = SlotsHolding(img, stub, 0, &at);
    if (!n) return Missing();
    if (n > 1) return Ambiguous(n);
    return SigOk(at);
}

// ---- contracts.auto_accept ---------------------------------------------------------------------

template <int K>
SigResult ResolveAutoAccept(const Image& img) {
    const uint8_t* s = FindCString(img.rdata, "DebugAcceptGeneratorContract AcceptContract(after creation) Sent");
    if (!s) return SigFail("the string isn't in .rdata");
    const uint8_t* fns[kAutoAcceptCallers + 1] = {};
    int n = 0;
    for (const uint8_t* lea = LeaTo(img.text, s, nullptr); lea; lea = LeaTo(img.text, s, lea + 8)) {
        const uint8_t* f = FunctionStart(img, lea);
        bool known = !f;
        for (int i = 0; i < n && i <= kAutoAcceptCallers && !known; ++i) known = fns[i] == f;
        if (known) continue;
        if (n <= kAutoAcceptCallers) fns[n] = f;
        ++n;
    }
    if (!n) return Missing();
    if (n != kAutoAcceptCallers) return SigFail("auto-accept callers: the function count changed");
    return SigOk(fns[K - 1]);
}

// ---- contracts.mission_creation ----------------------------------------------------------------

constexpr StrFnSpec kFactoryCreate{ "CMissionFactory::CreateMission", "4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10", nullptr, 0 };
constexpr StrFnSpec kGetShardGraph{
    "class std::shared_ptr<struct IUniverseHierarchyShardGraph> __cdecl CUniverseHierarchyShardStore::GetUniverseShardGraph(const char *) const",
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57", nullptr, 0 };
constexpr StrFnSpec kDumpShardGraph{ "soc_dumpShardGraph: no shard id supplied and gEnv->GetShardId() is empty", nullptr, nullptr, 0 };

const char* ShardStoreExtra(const Image& img, const uint8_t* at) {
    const uint8_t* get = at + 12 + Rel32(at + 8);
    return Matches(img, get, "48 8B 81 ?? ?? ?? ?? C3") ? nullptr : "the store getter changed (mov rax, [rcx+disp32]; ret)";
}
constexpr InRangeSpec kShardStoreSite{ "contracts.dump_shard_graph", 0x100, "48 8B 0D ?? ?? ?? ?? E8", ShardStoreExtra };
constexpr RipSpec kShardStoreOwner{ "contracts.shard_store_site", 0, 3, 7 };
constexpr RipSpec kShardStoreGet{ "contracts.shard_store_site", 7, 1, 5 };

constexpr StrFnSpec kMissionServiceRequest{ "Mission service not accessible", "48 8B C4 55 41 57 48 8D A8", nullptr, 0 };
constexpr InRangeSpec kUrnSite{ "contracts.mission_service_request", 0x600,
                                "49 8B 57 18 88 85 ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 48 8D 8D ?? ?? ?? ?? E8", nullptr };
constexpr RipSpec kUrnFromEntity{ "contracts.urn_site", 10, 1, 5 };
constexpr RipSpec kAssignUrn{ "contracts.urn_site", 25, 1, 5 };
constexpr InRangeSpec kStringInitSite{ "contracts.mission_service_request", 0x600, "C6 85 ?? ?? ?? ?? 05 44 88 A5 ?? ?? ?? ?? E8", nullptr };
constexpr RipSpec kStringInit{ "contracts.string_init_site", 14, 1, 5 };

constexpr const char* kCopyPropertyMap =
    "48 89 5C 24 20 41 56 48 83 EC 20 4C 8B F2 48 8B D9 48 3B CA 0F 84 ?? ?? ?? ?? 48 89 7C 24 40 48 8B 79 10 48 85 FF 74 ?? "
    "48 89 6C 24 30 48 89 74 24 38 48 8B 17 48 8B CB E8 ?? ?? ?? ?? 0F B6 57 48 48 8D 4F 28 48 8B 77 08";

// ---- contracts.mission_log ---------------------------------------------------------------------

constexpr StrFnSpec kAddMission{
    "void __cdecl CSCPlayerMissionLog::AddMission(const struct CryGUID &,const struct SMissionEntryDetails &,int,bool,const bool)",
    "44 89 4C 24 20 4C 89 44 24 18", nullptr, 0 };
constexpr InRangeSpec kLogHandleSite{ "contracts.add_mission", 0x800,
    "48 8B 01 FF 50 60 48 39 18 75 1A 48 8D 95 ?? ?? ?? ?? 49 8B CE E8 ?? ?? ?? ?? 48 8B CE 48 8B 10 E8", nullptr };
constexpr RipSpec kLogHandle{ "contracts.log_handle_site", 0x15, 1, 5 };
constexpr RipSpec kMakePhaseHandler{ "contracts.log_handle_site", 0x20, 1, 5 };
constexpr const char* kPhaseActivateSite =
    "49 8B 8D 48 02 00 00 4C 8D 43 20 48 8D 44 24 30 44 88 64 24 28 4D 8D 48 10 48 89 44 24 20 48 8B D5 E8";
constexpr CallSpec kPhaseActivate{ "contracts.phase_activate_site", 33, "41 56 48 83 EC 30 41 83 79 04 01" };
constexpr const char* kHaulingAssignSite = "38 42 68 0F 85 ?? ?? ?? ?? 48 8D 4D ?? E8 ?? ?? ?? ?? 48 85 FF 0F 84";
constexpr CallSpec kHaulingAssign{ "contracts.hauling_assign_site", 13, "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 E8" };
constexpr InRangeSpec kCopyDetailsSite{ "contracts.add_mission", 0x400, "48 8B D6 48 8D 4C 24 ?? 48 89 44 24 ?? C5 F8 11 44 24 ?? E8", nullptr };
constexpr CallSpec kCopyDetails{ "contracts.copy_details_site", 19, "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56" };

// Every call to AddMission followed within 0x20 bytes by a call with lea r64, [r64+0x80] between
// the two: the second call's target, the same for all kAddPlayerSites of them.
SigResult ResolveAddPlayer(const Image& img) {
    const uint8_t* fn = Sig("contracts.add_mission");
    if (!fn) return SigFail("source row missing");
    if (img.text.size < 0x30) return Missing();
    const uint8_t* target = nullptr;
    int n = 0;
    uint8_t* const end = img.text.base + img.text.size - 0x30;
    for (uint8_t* p = img.text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0xE8, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p + 5 + Rel32(p + 1) != fn) continue;
        for (uint8_t* q = p + 5; q < p + 0x20; ++q) {
            if (q[0] != 0xE8) continue;
            bool leaRdx80 = false;
            for (uint8_t* r = p + 5; r + 7 <= q && !leaRdx80; ++r) leaRdx80 = BytesMatch(r + 1, "8D ?? 80 00 00 00") && (r[0] & 0xF8) == 0x48;
            if (leaRdx80) {
                const uint8_t* t = q + 5 + Rel32(q + 1);
                if (target && t != target) return SigFail("AddPlayer call sites disagree on the target");
                target = t;
                ++n;
            }
            break;
        }
    }
    if (!n) return Missing();
    if (n != kAddPlayerSites) return SigFail("AddPlayer: the call site count changed");
    if (!InText(img, target, 1)) return SigFail("AddPlayer target outside .text");
    return SigOk(target);
}

// ---- contracts.mission_diagnostics -------------------------------------------------------------

constexpr StrFnSpec kAddActiveObjective{
    "void __cdecl CSCPlayerMissionLog::AddActiveObjective(const struct ActiveMissionObjective &,const bool)",
    "44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 55", nullptr, 0 };
constexpr StrFnSpec kAddActivePlayer{ "void __cdecl CMissionEntity::AddActivePlayer(class EntityId,bool)", "44 88 44 24 18 48 89 54 24 10 55 53 56 57", nullptr, 0 };
constexpr StrFnSpec kNotifyUiObjective{ "Notify UI Objective", "44 89 4C 24 20 89 54 24 10 55 53 56 41 54", nullptr, 0 };
constexpr StrFnSpec kIsObjectiveHidden{
    "bool __cdecl CSCPlayerMissionLog::IsObjectiveHidden(const struct CryGUID &,const class CryStringT<char> &,bool,bool) const",
    "48 8B C4 44 88 48 20 48 89 48 08 55 53 56 41 57", nullptr, 0 };
constexpr StrFnSpec kWarehouseQueue{ "void __cdecl CMissionWarehouseOrderHandler::ProcessQueue(void)", "48 8B C4 55 41 54 48 8D A8", nullptr, 0 };
constexpr StrFnSpec kCreateLogEntry{ "CMissionEntity::CreateMissionLogEntry", "48 8B C4 44 88 40 18 48 89 50 10 48 89 48 08 55 53 41 54", nullptr, 0 };

// ---- contracts.mission_modules -----------------------------------------------------------------

constexpr StrFnSpec kModuleInitialize{ "void __cdecl CSubsumptionMissionComponent::Initialize(void)", "48 89 4C 24 08 55 53 56 57", nullptr, 0 };
constexpr Check kStartMissionChecks[] = {
    { 0x111, "49 8D 97 50 01 00 00",    "layout changed at +0x111 (lea rdx, [r15+0x150]: the module's mission id)" },
    { 0x769, "41 83 BF 30 01 00 00 01", "layout changed at +0x769 (cmp dword [r15+0x130], 1: the module's state)" },
};
constexpr StrFnSpec kModuleStartMission{ "CSubsumptionMissionComponent::StartMission", "48 8B C4 48 89 48 08 55 48 8D A8",
                                         kStartMissionChecks, std::size(kStartMissionChecks) };
constexpr StrFnSpec kModuleAuthority{ "HandleAuthorityChangeEvent", "48 8B C4 48 89 50 10 48 89 48 08 55", nullptr, 0 };
constexpr Check kEntryAnswerChecks[] = {
    { 0x34E, "8B 82 30 01 00 00", "layout changed at +0x34e (mov eax, [rdx+0x130]: the module's state)" },
};
constexpr StrFnSpec kModuleEntryAnswer{ "Aborting subsumption mission module $$($$)",
                                        "40 55 53 56 41 56 48 8D AC 24 58 FF FF FF 48 81 EC A8 01 00 00",
                                        kEntryAnswerChecks, std::size(kEntryAnswerChecks) };
constexpr StrFnSpec kToPlayerLogs{ "AddActiveObjectiveToPlayerLogs", "40 55 53 56 57 48 8D 6C 24 C1", nullptr, 0 };
constexpr StrFnSpec kCreateObjective{ "$$[$$] - Created: $$[$$], parent id=$$, flags=$$", "44 88 4C 24 20 55 56 41 55", nullptr, 0 };
constexpr const char* kLocId = "48 89 5C 24 10 56 48 83 EC 20 C7 01 00 00 00 00 48 8B DA 48 8B F1 48 85 D2 0F 84 ?? ?? ?? ?? 80 3A 00 0F 84";

// ---- contracts.abandon -------------------------------------------------------------------------

constexpr StrFnSpec kStopMission{ "StopMission called with reason [$$] subsumptionState [$$] callstack \n $$ $$($$)",
                                  "89 54 24 10 48 89 4C 24 08 55 53 56 57 41 56 48 8D AC 24", nullptr, 0 };
constexpr VtblSpec kEntityVtbl{ "contracts.create_objective", kEntityCreateObjectiveSlot };
constexpr VtblFnSpec kEntityRemovePlayer{ "contracts.mission_entity_vtbl", kEntityRemovePlayerSlot,
                                          "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 60 48 8B D9 45 84 C0" };
constexpr StrFnSpec kEndHauling{ "CMissionServiceOffline::RequestEndHaulingObjectiveAndPhase is not implemented yet", nullptr, nullptr, 0 };
constexpr VtblSpec kServiceVtbl{ "contracts.offline_end_hauling", kServiceEndHaulingSlot };
constexpr VtblFnSpec kLeaveMission{ "contracts.offline_service_vtbl", kServiceLeaveMissionSlot,
                                    "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24" };
constexpr VtblFnSpec kEndPhase{ "contracts.offline_service_vtbl", kServiceEndPhaseSlot,
                                "48 89 5C 24 08 48 89 74 24 10 48 89 7C 24 18 4C 89 74 24 20 55 48 8D 6C 24 C1 48 81 EC D0 00 00 00" };

// ---- contracts.rewards, contracts.shop_payments, contracts.steps -------------------------------

SigResult ResolveSendRewardsAuthority(const Image& img) {
    const uint8_t* msg = FindCString(img.rdata, "CSCPlayerMissionLog::SendRewards No authority");
    uint8_t* lea = msg ? FindRipLea(img.text, 0x4C, 0x8D, 0x05, msg) : nullptr;
    if (!lea) return SigFail("the SendRewards message isn't referenced");
    if (!Matches(img, lea - 14, "FF 90 ?? ?? ?? ?? 84 C0 0F 85")) return SigFail("layout changed at -0x00e (call; test al, al; jnz)");
    return SigOk(lea - 14);
}

constexpr FnOfSpec kFindEntry{
    "48 8D 83 40 04 00 00 EB 0C 48 8D 8B 20 04 00 00 E8 ?? ?? ?? ?? 48 8B 08 48 8B 50 08 48 3B CA 74 25 4C 8B 07 "
    "66 0F 1F 44 00 00 4C 39 41 08 75 0A 48 8B 47 08 48 39 41 10 74 1E 48 81 C1 50 02 00 00",
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 48 8B D9 48 8B FA" };
constexpr FnOfSpec kFindEntryAny{ "B9 28 04 00 00 41 B8 40 04 00 00 44 0F 44 C1 49 8B 04 38 49 8B 54 38 08",
                                  "48 89 5C 24 08 57 48 83 EC 20 48 8B DA 48 8B F9" };
constexpr StrFnSpec kTotalReward{ "int __cdecl CMissionLogEntry::GetTotalReward(void) const",
                                  "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 60", nullptr, 0 };
constexpr StrFnSpec kUpdateBalance{ "CWallet::UpdateCurrencyBalanceValue", "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 C0", nullptr, 0 };
constexpr StrFnSpec kAsyncUpdateBalance{ "CWallet::AsyncUpdateCurrencyBalance",
                                         "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 81 EC C0 00 00 00", nullptr, 0 };
constexpr const char* kEndMission = "48 89 5C 24 10 48 89 6C 24 18 56 57 41 56 48 83 EC 40 48 8B F9 41 8B F1 48 83 C1 08 41 8B E8 4C 8B F2 E8";
constexpr StrFnSpec kEmFinished{
    "void __cdecl CEnvironmentalMissionManager::OnMissionModuleFinished(const class EntityId &,enum EMissionModuleStopReason)",
    "44 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57", nullptr, 0 };
constexpr StrFnSpec kActorKill{ "CActor::Kill", "4C 89 44 24 18 48 89 54 24 10 55 53 57 41 54", nullptr, 0 };
constexpr StrFnSpec kSendComms{ "SendCommsNotification Record GUID [$$] is invalid - Mission: [$$], Player: $$[$$]",
                                "48 8B C4 4C 89 40 18 48 89 50 10 55 53 56 57 41 56 41 57", nullptr, 0 };
constexpr StrFnSpec kHelperSpawned{ "Succeeded to spawn delivery mission helper. entityId: $$, missionId: $$",
                                    "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54", nullptr, 0 };

// ---- contracts.stream_radius -------------------------------------------------------------------

constexpr const char* kMissionSettings =
    "48 8B 05 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? C5 FA 10 88 74 01 00 00 48 8B 01 C5 F2 5A C9 48 FF A0 20 03 00 00";

// ---- contracts.phases --------------------------------------------------------------------------

constexpr const char* kPhaseSite =
    "48 8D 53 20 E8 ?? ?? ?? ?? 48 85 C0 74 ?? 48 8B 94 24 ?? ?? ?? ?? 49 8D 8F 88 00 00 00 48 89 6C 24 38 4D 8D 8F B8 00 00 00 "
    "48 89 54 24 30 4C 8B C0 4C 89 64 24 28 49 8B D6 48 89 4C 24 20 48 8B CE E8";
constexpr CallSpec kCreatePhase{ "contracts.phase_site", 65,
                                 "48 89 5C 24 08 4C 89 4C 24 20 4C 89 44 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57" };
constexpr RipSpec kFindPhase{ "contracts.phase_site", 4, 1, 5 };
constexpr const char* kFlowSite =
    "48 8D 4C 24 40 BA 38 00 00 00 4C 89 AC 24 68 01 00 00 C5 FA 7F 44 24 40 E8 ?? ?? ?? ?? 49 8D 57 30 48 8D 4D A8 48 89 00 "
    "48 89 40 08 48 89 40 10 66 C7 40 18 01 01 48 89 44 24 40 E8 ?? ?? ?? ?? 45 33 C0 48 8D 54 24 40 48 8D 4D A8 E8";
constexpr const char* kFlowFreeSite = "83 F8 06 74 07 41 89 87 68 01 00 00 48 8D 4C 24 40 E8";
constexpr RipSpec kTempAlloc{ "contracts.flow_site", 24, 1, 5 };
constexpr RipSpec kFlowContext{ "contracts.flow_site", 59, 1, 5 };
constexpr RipSpec kUpdateFlow{ "contracts.flow_site", 76, 1, 5 };
constexpr RipSpec kFreeFlowMap{ "contracts.flow_free_site", 17, 1, 5 };
constexpr const char* kObjectiveSite =
    "48 8D 4D B0 E8 ?? ?? ?? ?? C5 F9 EF C0 C5 F1 EF C9 C5 FA 7F 45 30 C5 FA 7F 4D 40 4C 89 6D B0 4C 89 65 28 4C 89 65 50 E8 ?? ?? ?? ?? "
    "C5 F9 EF C0 33 D2 8B 08 89 4D 58 48 8D 8D 80 00 00 00 C5 FA 11 75 78 C5 FA 11 75 7C C5 FA 7F 45 60 4C 89 65 70 E8";
constexpr const char* kObjectiveCopySite = "E8 ?? ?? ?? ?? 48 8D 05 ?? ?? ?? ?? 48 8D 53 78 48 89 07 48 8D 4F 78 E8";
constexpr RipSpec kObjectiveInit{ "contracts.objective_site", 4, 1, 5 };
constexpr RipSpec kEmptyLoc{ "contracts.objective_site", 39, 1, 5 };
constexpr RipSpec kTimerInit{ "contracts.objective_site", 81, 1, 5 };
constexpr RipSpec kObjectiveCopy{ "contracts.objective_copy_site", 0, 1, 5 };
constexpr RipSpec kActiveObjectiveVtbl{ "contracts.objective_copy_site", 5, 3, 7 };
constexpr const char* kPendingToObjective =
    "48 89 5C 24 08 57 48 83 EC 20 48 8B FA 48 8B D9 48 8D 51 08 48 8B CF E8 ?? ?? ?? ?? 48 8D 53 10 48 8D 4F 08 E8 ?? ?? ?? ?? "
    "C5 F8 10 43 18 C5 F8 11 47 10 8B 43 2C 48 8B CB 89 47 38 8B 43 28 89";
constexpr const char* kActivateToken =
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 4D 8B E0 48 8B F2 48 8B D9 E8";

// ---- capabilities ------------------------------------------------------------------------------

constexpr const char* kList[] = {
    "contracts.query_reply", "contracts.mission_system_site", "contracts.mission_system", "contracts.mission_generator_get",
};
constexpr const char* kAccept[] = { "contracts.accept_stub", "contracts.accept_resolve", "contracts.accept_slot" };
constexpr const char* kAutoAccept[] = { "contracts.auto_accept_caller.1", "contracts.auto_accept_caller.2" };
static_assert(std::size(kAutoAccept) == kAutoAcceptCallers);
constexpr const char* kCreation[] = {
    "contracts.factory_create", "contracts.copy_property_map", "contracts.get_shard_graph", "contracts.dump_shard_graph",
    "contracts.shard_store_site", "contracts.shard_store_owner", "contracts.shard_store_get", "contracts.mission_service_request",
    "contracts.urn_site", "contracts.urn_from_entity", "contracts.assign_urn", "contracts.string_init_site", "contracts.string_init",
};
constexpr const char* kMissionLog[] = {
    "contracts.add_mission", "contracts.log_handle_site", "contracts.log_handle", "contracts.make_phase_handler",
    "contracts.phase_activate_site", "contracts.phase_activate", "contracts.hauling_assign_site", "contracts.hauling_assign",
    "contracts.copy_details_site", "contracts.copy_details", "contracts.add_player",
};
constexpr const char* kDiagnostics[] = {
    "contracts.add_active_objective", "contracts.add_active_player", "contracts.notify_ui_objective",
    "contracts.is_objective_hidden", "contracts.warehouse_process_queue", "contracts.create_mission_log_entry",
};
constexpr const char* kModules[] = {
    "contracts.module_initialize", "contracts.module_start_mission", "contracts.module_authority", "contracts.module_entry_answer",
    "contracts.objective_to_player_logs", "contracts.create_objective", "contracts.loc_id", "contracts.stop_mission",
};
constexpr const char* kOfflineService[] = { "contracts.offline_end_hauling", "contracts.offline_service_vtbl" };
constexpr const char* kAbandon[] = {
    "contracts.offline_end_hauling", "contracts.offline_service_vtbl", "contracts.offline_leave_mission",
    "contracts.create_objective", "contracts.mission_entity_vtbl", "contracts.mission_entity_remove_player",
};
constexpr const char* kPhaseEnds[] = {
    "contracts.offline_end_hauling", "contracts.offline_service_vtbl", "contracts.offline_end_phase",
};
constexpr const char* kRewards[] = {
    "contracts.send_rewards_authority", "contracts.find_entry", "contracts.find_entry_any", "contracts.total_reward",
    "contracts.update_balance",
};
constexpr const char* kShopPayments[] = { "contracts.update_balance", "contracts.async_update_balance" };
constexpr const char* kSteps[] = {
    "contracts.end_mission", "contracts.em_module_finished", "contracts.actor_kill", "contracts.send_comms", "contracts.helper_spawned",
};
constexpr const char* kStreamRadiusRows[] = { "contracts.mission_settings" };
constexpr const char* kPhases[] = {
    "contracts.phase_site", "contracts.create_phase", "contracts.find_phase", "contracts.flow_site", "contracts.flow_free_site",
    "contracts.temp_alloc", "contracts.flow_context", "contracts.update_flow", "contracts.free_flow_map", "contracts.objective_site",
    "contracts.objective_copy_site", "contracts.objective_init", "contracts.empty_loc", "contracts.timer_init",
    "contracts.objective_copy", "contracts.active_objective_vtbl", "contracts.pending_to_objective", "contracts.activate_token",
};

constexpr Capability kCaps[] = {
    { "contracts.list",                kList,             std::size(kList) },
    { "contracts.accept",              kAccept,           std::size(kAccept) },
    { "contracts.auto_accept",         kAutoAccept,       std::size(kAutoAccept) },
    { "contracts.mission_creation",    kCreation,         std::size(kCreation) },
    { "contracts.mission_log",         kMissionLog,       std::size(kMissionLog) },
    { "contracts.mission_diagnostics", kDiagnostics,      std::size(kDiagnostics) },
    { "contracts.mission_modules",     kModules,          std::size(kModules) },
    { "contracts.offline_service",     kOfflineService,   std::size(kOfflineService) },
    { "contracts.abandon",             kAbandon,          std::size(kAbandon) },
    { "contracts.phase_ends",          kPhaseEnds,        std::size(kPhaseEnds) },
    { "contracts.rewards",             kRewards,          std::size(kRewards) },
    { "contracts.shop_payments",       kShopPayments,     std::size(kShopPayments) },
    { "contracts.steps",               kSteps,            std::size(kSteps) },
    { "contracts.stream_radius",       kStreamRadiusRows, std::size(kStreamRadiusRows) },
    { "contracts.phases",              kPhases,           std::size(kPhases) },
};

}  // namespace

namespace contracts {

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace contracts

#define SCO_STR(id, spec)  { id, nullptr, 0, 0, ResolveStrFn<spec>, {} }
#define SCO_PAT(id, pat)   { id, pat, 0, 0, nullptr, {} }

extern const SigDef kContractsSignatures[] = {
    // list
    SCO_STR("contracts.query_reply", kQueryReply),
    { "contracts.mission_system_site",      nullptr, 0, 0, ResolveMissionSystemSite, { "contracts.query_reply" } },
    { "contracts.mission_system",           nullptr, 0, 0, ResolveMissionSystem,     { "contracts.mission_system_site" } },
    { "contracts.mission_generator_get",    nullptr, 0, 0, ResolveRip<kGeneratorGet>, { "contracts.mission_system_site" } },
    // accept
    { "contracts.accept_stub",              nullptr, 0, 0, ResolveAcceptStub,          {} },
    { "contracts.accept_resolve",           nullptr, 0, 0, ResolveCall<kAcceptResolve>, { "contracts.accept_stub" } },
    { "contracts.accept_slot",              nullptr, 0, 0, ResolveAcceptSlot,          { "contracts.accept_stub" } },
    { "contracts.auto_accept_caller.1",     nullptr, 0, 0, ResolveAutoAccept<1>,       {} },
    { "contracts.auto_accept_caller.2",     nullptr, 0, 0, ResolveAutoAccept<2>,       {} },
    // mission creation
    SCO_STR("contracts.factory_create", kFactoryCreate),
    SCO_PAT("contracts.copy_property_map", kCopyPropertyMap),
    SCO_STR("contracts.get_shard_graph", kGetShardGraph),
    SCO_STR("contracts.dump_shard_graph", kDumpShardGraph),
    { "contracts.shard_store_site",         nullptr, 0, 0, ResolveInRange<kShardStoreSite>, { "contracts.dump_shard_graph" } },
    { "contracts.shard_store_owner",        nullptr, 0, 0, ResolveRip<kShardStoreOwner>,    { "contracts.shard_store_site" } },
    { "contracts.shard_store_get",          nullptr, 0, 0, ResolveRip<kShardStoreGet>,      { "contracts.shard_store_site" } },
    SCO_STR("contracts.mission_service_request", kMissionServiceRequest),
    { "contracts.urn_site",                 nullptr, 0, 0, ResolveInRange<kUrnSite>,        { "contracts.mission_service_request" } },
    { "contracts.urn_from_entity",          nullptr, 0, 0, ResolveRip<kUrnFromEntity>,      { "contracts.urn_site" } },
    { "contracts.assign_urn",               nullptr, 0, 0, ResolveRip<kAssignUrn>,          { "contracts.urn_site" } },
    { "contracts.string_init_site",         nullptr, 0, 0, ResolveInRange<kStringInitSite>, { "contracts.mission_service_request" } },
    { "contracts.string_init",              nullptr, 0, 0, ResolveRip<kStringInit>,         { "contracts.string_init_site" } },
    // mission log
    SCO_STR("contracts.add_mission", kAddMission),
    { "contracts.log_handle_site",          nullptr, 0, 0, ResolveInRange<kLogHandleSite>,  { "contracts.add_mission" } },
    { "contracts.log_handle",               nullptr, 0, 0, ResolveRip<kLogHandle>,          { "contracts.log_handle_site" } },
    { "contracts.make_phase_handler",       nullptr, 0, 0, ResolveRip<kMakePhaseHandler>,   { "contracts.log_handle_site" } },
    SCO_PAT("contracts.phase_activate_site", kPhaseActivateSite),
    { "contracts.phase_activate",           nullptr, 0, 0, ResolveCall<kPhaseActivate>,     { "contracts.phase_activate_site" } },
    SCO_PAT("contracts.hauling_assign_site", kHaulingAssignSite),
    { "contracts.hauling_assign",           nullptr, 0, 0, ResolveCall<kHaulingAssign>,     { "contracts.hauling_assign_site" } },
    { "contracts.copy_details_site",        nullptr, 0, 0, ResolveInRange<kCopyDetailsSite>, { "contracts.add_mission" } },
    { "contracts.copy_details",             nullptr, 0, 0, ResolveCall<kCopyDetails>,       { "contracts.copy_details_site" } },
    { "contracts.add_player",               nullptr, 0, 0, ResolveAddPlayer,                { "contracts.add_mission" } },
    // mission diagnostics
    SCO_STR("contracts.add_active_objective", kAddActiveObjective),
    SCO_STR("contracts.add_active_player", kAddActivePlayer),
    SCO_STR("contracts.notify_ui_objective", kNotifyUiObjective),
    SCO_STR("contracts.is_objective_hidden", kIsObjectiveHidden),
    SCO_STR("contracts.warehouse_process_queue", kWarehouseQueue),
    SCO_STR("contracts.create_mission_log_entry", kCreateLogEntry),
    // mission modules
    SCO_STR("contracts.module_initialize", kModuleInitialize),
    SCO_STR("contracts.module_start_mission", kModuleStartMission),
    SCO_STR("contracts.module_authority", kModuleAuthority),
    SCO_STR("contracts.module_entry_answer", kModuleEntryAnswer),
    SCO_STR("contracts.objective_to_player_logs", kToPlayerLogs),
    SCO_STR("contracts.create_objective", kCreateObjective),
    SCO_PAT("contracts.loc_id", kLocId),
    // abandon, phase ends, hauling ends
    SCO_STR("contracts.stop_mission", kStopMission),
    { "contracts.mission_entity_vtbl",      nullptr, 0, 0, ResolveVtbl<kEntityVtbl>,          { "contracts.create_objective" } },
    { "contracts.mission_entity_remove_player", nullptr, 0, 0, ResolveVtblFn<kEntityRemovePlayer>, { "contracts.mission_entity_vtbl" } },
    SCO_STR("contracts.offline_end_hauling", kEndHauling),
    { "contracts.offline_service_vtbl",     nullptr, 0, 0, ResolveVtbl<kServiceVtbl>,         { "contracts.offline_end_hauling" } },
    { "contracts.offline_leave_mission",    nullptr, 0, 0, ResolveVtblFn<kLeaveMission>,      { "contracts.offline_service_vtbl" } },
    { "contracts.offline_end_phase",        nullptr, 0, 0, ResolveVtblFn<kEndPhase>,          { "contracts.offline_service_vtbl" } },
    // rewards, shop payments, steps
    { "contracts.send_rewards_authority",   nullptr, 0, 0, ResolveSendRewardsAuthority,       {} },
    { "contracts.find_entry",               nullptr, 0, 0, ResolveFnOf<kFindEntry>,           {} },
    { "contracts.find_entry_any",           nullptr, 0, 0, ResolveFnOf<kFindEntryAny>,        {} },
    SCO_STR("contracts.total_reward", kTotalReward),
    SCO_STR("contracts.update_balance", kUpdateBalance),
    SCO_STR("contracts.async_update_balance", kAsyncUpdateBalance),
    SCO_PAT("contracts.end_mission", kEndMission),
    SCO_STR("contracts.em_module_finished", kEmFinished),
    SCO_STR("contracts.actor_kill", kActorKill),
    SCO_STR("contracts.send_comms", kSendComms),
    SCO_STR("contracts.helper_spawned", kHelperSpawned),
    // stream radius
    { "contracts.mission_settings",         kMissionSettings, 3, 7, nullptr, {} },
    // phases
    SCO_PAT("contracts.phase_site", kPhaseSite),
    { "contracts.create_phase",             nullptr, 0, 0, ResolveCall<kCreatePhase>,         { "contracts.phase_site" } },
    { "contracts.find_phase",               nullptr, 0, 0, ResolveRip<kFindPhase>,            { "contracts.phase_site" } },
    SCO_PAT("contracts.flow_site", kFlowSite),
    SCO_PAT("contracts.flow_free_site", kFlowFreeSite),
    { "contracts.temp_alloc",               nullptr, 0, 0, ResolveRip<kTempAlloc>,            { "contracts.flow_site" } },
    { "contracts.flow_context",             nullptr, 0, 0, ResolveRip<kFlowContext>,          { "contracts.flow_site" } },
    { "contracts.update_flow",              nullptr, 0, 0, ResolveRip<kUpdateFlow>,           { "contracts.flow_site" } },
    { "contracts.free_flow_map",            nullptr, 0, 0, ResolveRip<kFreeFlowMap>,          { "contracts.flow_free_site" } },
    SCO_PAT("contracts.objective_site", kObjectiveSite),
    SCO_PAT("contracts.objective_copy_site", kObjectiveCopySite),
    { "contracts.objective_init",           nullptr, 0, 0, ResolveRip<kObjectiveInit>,        { "contracts.objective_site" } },
    { "contracts.empty_loc",                nullptr, 0, 0, ResolveRip<kEmptyLoc>,             { "contracts.objective_site" } },
    { "contracts.timer_init",               nullptr, 0, 0, ResolveRip<kTimerInit>,            { "contracts.objective_site" } },
    { "contracts.objective_copy",           nullptr, 0, 0, ResolveRip<kObjectiveCopy>,        { "contracts.objective_copy_site" } },
    { "contracts.active_objective_vtbl",    nullptr, 0, 0, ResolveRip<kActiveObjectiveVtbl>,  { "contracts.objective_copy_site" } },
    SCO_PAT("contracts.pending_to_objective", kPendingToObjective),
    SCO_PAT("contracts.activate_token", kActivateToken),
};
extern const size_t kContractsSignatureCount = std::size(kContractsSignatures);

#undef SCO_STR
#undef SCO_PAT

}  // namespace sco::game
