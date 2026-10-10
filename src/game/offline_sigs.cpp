// Signature rows for sc-offline's boot / offline-mode patches (patches.cpp), its startup hooks
// (hooks.cpp) and its Quit hook's fast-shutdown test (dllmain.cpp). Each resolver is the scan
// sc-offline ran itself, moved byte for byte; where that scan took "every match", the row now
// insists on the count found in 4.10.196.36804, so a game patch that adds or loses a site shows up
// as a failed row instead of a silent change. Rows only find addresses: sc-offline still patches.
#include "sco/game/offline.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <cstdio>
#include <cstring>
#include <iterator>

namespace sco::game {

namespace {

using namespace offline;
using rows::Check;
using rows::FnSpec;
using rows::InText;
using rows::ResolveFn;
using rows::ResolveRip;
using rows::RipSpec;

SigResult Missing()          { return { SigState::Missing, nullptr, 0, nullptr }; }
SigResult Ambiguous(int n)   { return { SigState::Ambiguous, nullptr, n, nullptr }; }

// An .rdata string the patches point code at.
template <const char* S>
SigResult ResolveString(const Image& img) {
    const uint8_t* s = FindCString(img.rdata, S);
    return s ? SigOk(s) : Missing();
}

constexpr char kPu[]         = "PU";
constexpr char kScDefault[]  = "SC_Default";
constexpr char kMegaMapAll[] = "MegaMap.PU_All";
constexpr char kUser[]       = "%USER%";

// ---- IsOnline store --------------------------------------------------------------------------
// A pattern row: { "offline.is_online_store", "44 88 A0 0E 06 00 00", ... } in the table.

// ---- local handshake: the sessionGuid gates ------------------------------------------------------

// Every gate in address order (up to max written); sameFlag is false when their cmp targets differ.
// -1 when "sessionGuid" isn't in .rdata.
int HandshakeSites(const Image& img, uint8_t** out, int max, bool& sameFlag) {
    const Section& text = img.text;
    const uint8_t* guid = FindCString(img.rdata, "sessionGuid");
    if (!guid) return -1;
    const uint8_t* flag = nullptr;
    sameFlag = true;
    int n = 0;
    uint8_t* const end = text.base + text.size - 32;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x44, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x38 || (p[2] & 0xC7) != 0x05 || p[7] != 0x0F || p[8] != 0x84) continue;
        bool loadsGuid = false;
        for (const uint8_t* q = p + 13; q < p + 25; ++q)
            if (q[0] == 0x48 && q[1] == 0x8D && q[2] == 0x15 && q + 7 + Rel32(q + 3) == guid) { loadsGuid = true; break; }
        if (!loadsGuid) continue;
        const uint8_t* cmpTarget = p + 7 + Rel32(p + 3);
        if (flag && cmpTarget != flag) sameFlag = false;
        flag = cmpTarget;
        if (n < max) out[n] = p;
        ++n;
    }
    return n;
}

template <int K>
SigResult ResolveHandshake(const Image& img) {
    uint8_t* sites[kHandshakeSites] = {};
    bool same = true;
    const int n = HandshakeSites(img, sites, kHandshakeSites, same);
    if (n < 0) return SigFail("the sessionGuid string isn't in .rdata");
    if (n == 0) return Missing();
    if (n != kHandshakeSites) return SigFail("handshake gates: the site count changed");
    if (!same) return SigFail("handshake gates: the gates compare different flags");
    return SigOk(sites[K - 1]);
}

constexpr RipSpec kIsOnlineFlag{ "offline.handshake_gate.1", 0, 3, 7 };

// ---- megamap keeps record-name case ----------------------------------------------------------

SigResult ResolveMegamapCall(const Image& img) {
    const Section& text = img.text;
    static const uint8_t kToLowerPrologue[] = { 0x4C, 0x8B, 0x09, 0x4C, 0x8B, 0xD1, 0x41, 0x0F, 0xB6, 0x01, 0x84, 0xC0 };
    const uint8_t* help = FindCString(img.rdata, "Load a map, same usage as 'megamap' cvar.");
    if (!help) return SigFail("the map command's help text isn't in .rdata");
    uint8_t* reg = FindRipLea(text, 0x48, 0x8D, 0x15, help);
    if (!reg) return SigFail("the map command's help text isn't referenced");
    const uint8_t* handler = nullptr;
    for (uint8_t* q = reg + 7; q < reg + 7 + 32; ++q)
        if (q[0] == 0x4C && q[1] == 0x8D && q[2] == 0x05) { handler = q + 7 + Rel32(q + 3); break; }
    if (!handler || handler < text.base || handler >= text.base + text.size - 0x80)
        return SigFail("no map handler (lea r8) after the registration");
    static const uint8_t leaRcx[] = { 0x48, 0x8D, 0x4C, 0x24, 0x30, 0xE8 };
    const uint8_t* call = nullptr;
    int hits = 0;
    for (const uint8_t* h = handler; h < handler + 0x60; ++h) {
        if (memcmp(h, leaRcx, 6) != 0 || memcmp(h + 10, leaRcx, 6) != 0) continue;
        const uint8_t* callee = h + 20 + Rel32(h + 16);
        if (callee < text.base || callee + sizeof(kToLowerPrologue) > text.base + text.size) continue;
        if (memcmp(callee, kToLowerPrologue, sizeof(kToLowerPrologue)) != 0) continue;
        call = h + 15;
        ++hits;
    }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(call);
}

// ---- boot into PU ------------------------------------------------------------------------------

SigResult ResolveBootFrontend(const Image& img) {
    const Section& text = img.text;
    const uint8_t* frontend   = FindCString(img.rdata, "Frontend_Main");
    const uint8_t* scFrontend = FindCString(img.rdata, "SC_Frontend");
    if (!frontend || !scFrontend) return SigFail("Frontend_Main / SC_Frontend isn't in .rdata");
    uint8_t* site = nullptr;
    int hits = 0;
    uint8_t* const end = text.base + text.size - 14;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x4C, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x05 || p + 7 + Rel32(p + 3) != scFrontend) continue;
        const uint8_t* q = p + 7;
        if (q[0] != 0x48 || q[1] != 0x8D || q[2] != 0x15 || q + 7 + Rel32(q + 3) != frontend) continue;
        if (!site) site = p;
        ++hits;
    }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(site);
}

// ---- offline player data from %USER% -----------------------------------------------------------

template <int K>
SigResult ResolveOfflineDb(const Image& img) {
    const Section& text = img.text;
    const uint8_t* offlineDb = FindCString(img.rdata, "Libs/OfflineDB");
    if (!offlineDb) return SigFail("Libs/OfflineDB isn't in .rdata");
    uint8_t* sites[kOfflineDbSites] = {};
    int n = 0;
    uint8_t* const end = text.base + text.size - 7;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x4C, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p[1] != 0x8D || p[2] != 0x05 || p + 7 + Rel32(p + 3) != offlineDb) continue;
        if (n < kOfflineDbSites) sites[n] = p;
        ++n;
    }
    if (n == 0) return Missing();
    if (n != kOfflineDbSites) return SigFail("Libs/OfflineDB references: the site count changed");
    return SigOk(sites[K - 1]);
}

// ---- ASOP / fleet manager on the offline shard -------------------------------------------------

const char* AsopGateOpenExtra(const Image&, const uint8_t* at) {
    const uint8_t* persisted = Sig("offline.is_online_flag") + 1;
    return RipTarget(at + 7, 3, 7) == persisted ? nullptr : "cmp at +7 doesn't read is_online_flag+1";
}
constexpr FnSpec kAsopGateOpen{
    "44 89 AD D0 00 00 00 44 38 2D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 51 08 48 8D 8D D0 00 00 00 E8", nullptr, 0,
    AsopGateOpenExtra };

const char* AsopGateValidationExtra(const Image& img, const uint8_t* at) {
    const uint8_t* persisted = Sig("offline.is_online_flag") + 1;
    if (RipTarget(at, 2, 7) != persisted) return "cmp at +0 doesn't read is_online_flag+1";
    const uint8_t* msg = FindCString(img.rdata, "Can only perform ASOP operations in the PU.");
    if (!msg) return "the ASOP-in-the-PU message isn't in .rdata";
    return RipTarget(at + 27, 3, 7) == msg ? nullptr : "lea r9 at +0x1b doesn't load the ASOP-in-the-PU message";
}
constexpr FnSpec kAsopGateValidation{
    "80 3D ?? ?? ?? ?? 00 75 ?? 48 8D 44 24 60 C7 44 24 60 E3 00 00 00 48 89 44 24 70 4C 8D 0D", nullptr, 0,
    AsopGateValidationExtra };

// ---- landing zones don't destroy spawned ships -------------------------------------------------

const char* ImpoundExtra(const Image& img, const uint8_t* f) {
    const uint8_t* msg = FindCString(img.rdata, "$$: Vehicle '$$' [$$] impounded (or destroyed) by restricted area '$$' [$$]");
    const uint8_t* tag = FindCString(img.rdata, "Boundary Violation");
    if (!msg || !tag) return "the impound log strings aren't in .rdata";
    if (RipTarget(f + 0x59, 3, 7) != msg) return "lea r9 at +0x59 doesn't load the impound message";
    if (RipTarget(f + 0x79, 3, 7) != tag) return "lea r8 at +0x79 doesn't load \"Boundary Violation\"";
    return nullptr;
}
constexpr Check kImpoundChecks[] = {
    { 0x59, "4C 8D 0D", "layout changed at +0x059" },   // lea r9, [impound message]
    { 0x79, "4C 8D 05", "layout changed at +0x079" },   // lea r8, ["Boundary Violation"]
};
constexpr FnSpec kImpound{
    "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 38 FE FF FF 48 81 EC C8 02 00 00 33 D2 48 8B F1 E8",
    kImpoundChecks, std::size(kImpoundChecks), ImpoundExtra };

// ---- offline contract broker + mission service -------------------------------------------------

constexpr char kUseService[]        = "contract_broker.use_service";
constexpr char kUseOnlineMissions[] = "contract_broker.use_online_mission_service";

template <const char* Name>
SigResult ResolveCvarDefault(const Image& img) {
    const uint8_t* name = FindCString(img.rdata, Name);
    if (!name) return SigFail("the cvar name isn't in .rdata");
    uint8_t* lea = FindRipLea(img.text, 0x48, 0x8D, 0x15, name);
    if (!lea) return SigFail("the cvar name isn't referenced");
    if (!InText(img, lea - 0xF, 9)) return SigFail("registration outside .text");
    if (!BytesMatch(lea - 6, "41 B9 01 00 00 00")) return SigFail("layout changed at lea-0x6 (mov r9d, 1)");
    if (!BytesMatch(lea - 0xF, "4C 8D 43")) return SigFail("layout changed at lea-0xf (lea r8, [rbx+..])");
    return SigOk(lea - 6);
}

// ---- party/group checks without the social service ---------------------------------------------

// Every social-group query site in address order (up to max written); -1 when two sites load
// different globals.
int SocialGroupSites(const Image& img, uint8_t** out, int max) {
    const Section& text = img.text;
    static const uint8_t kSlotB0[4] = { 0xB0, 0x00, 0x00, 0x00 };
    const uint8_t* global = nullptr;
    int n = 0;
    uint8_t* const end = text.base + text.size - 0xA0;
    for (uint8_t* p = text.base; p < end; ++p) {
        p = static_cast<uint8_t*>(memchr(p, 0x48, static_cast<size_t>(end - p)));   // same sites, fewer compares
        if (!p) break;
        if (p[1] != 0x8B || p[2] != 0x0D) continue;
        const uint8_t* call = nullptr;
        for (const uint8_t* q = p + 7; q < p + 7 + 0x30 && !call; ++q)
            if (q[0] == 0x48 && q[1] == 0x8B && q[2] == 0x01 && q[3] == 0xFF && q[4] == 0x50 && q[5] == 0x18) call = q + 6;
        if (!call) continue;
        const uint8_t* social = nullptr;
        for (const uint8_t* q = call; q < call + 0x18 && !social; ++q) {
            if ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC0) == 0x40 && q[3] == 0x70) social = q + 4;
            else if (q[0] == 0xFF && (q[1] & 0xF8) == 0x50 && q[2] == 0x70) social = q + 3;
        }
        if (!social) continue;
        bool sameGroup = false;
        for (const uint8_t* q = social; q < social + 0x50 && !sameGroup; ++q)
            sameGroup = ((q[0] == 0x48 || q[0] == 0x4C) && q[1] == 0x8B && (q[2] & 0xC0) == 0x80 && !memcmp(q + 3, kSlotB0, 4))
                     || (q[0] == 0xFF && (q[1] & 0xF8) == 0x90 && !memcmp(q + 2, kSlotB0, 4));
        if (!sameGroup) continue;
        const uint8_t* g = p + 7 + Rel32(p + 3);
        if (global && g != global) return -1;
        global = g;
        if (n < max) out[n] = p;
        ++n;
    }
    return n;
}

template <int K>
SigResult ResolveSocialGroup(const Image& img) {
    uint8_t* sites[kSocialGroupSites] = {};
    const int n = SocialGroupSites(img, sites, kSocialGroupSites);
    if (n < 0) return SigFail("social-group queries: the sites load different globals");
    if (n == 0) return Missing();
    if (n != kSocialGroupSites) return SigFail("social-group queries: the site count changed");
    return SigOk(sites[K - 1]);
}

constexpr RipSpec kServicesEnv{ "offline.social_group.1", 0, 3, 7 };

// ---- no reconnecting service streams -------------------------------------------------------------

// The next `lea r64, [rip+target]` (48/4C 8D, mod/rm = rip) at or after `from`.
const uint8_t* FindLeaFrom(const Section& text, const uint8_t* target, const uint8_t* from) {
    const uint8_t* const end = text.base + text.size - 7;
    for (const uint8_t* p = from; target && p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0x8D, static_cast<size_t>(end - p)));
        if (!p) break;
        if (p > text.base && (p[-1] == 0x48 || p[-1] == 0x4C) && (p[1] & 0xC7) == 0x05 && p + 6 + Rel32(p + 2) == target)
            return p - 1;
    }
    return nullptr;
}

constexpr char kPresence[]  = "presence::v1::PresenceService";
constexpr char kAnalytics[] = "analytics::v1::AnalyticsService";
constexpr char kTrace[]     = "trace::v1::TraceService";
constexpr char kEcho[]      = "echo::v1::EchoService";

// Stream function K of N for one service: every function (its .pdata start within 0x200 before
// the reference, with the stream prologue) that references the service's lambda name.
template <const char* Service, int K, int N>
SigResult ResolveServiceStream(const Image& img) {
    char name[256];
    std::snprintf(name, sizeof(name),
                  "auto __cdecl CAsyncClient<class sc::external::services::%s>::CreateClientStream::<lambda_3>::operator ()(const char *) const",
                  Service);
    const uint8_t* s = FindCString(img.rdata, name);
    if (!s) return SigFail("the stream lambda's name isn't in .rdata");
    uint8_t* fns[N] = {};
    int n = 0;
    for (const uint8_t* lea = FindLeaFrom(img.text, s, img.text.base); lea; lea = FindLeaFrom(img.text, s, lea + 8)) {
        uint8_t* fn = FunctionStart(img, lea);
        if (!fn || lea - fn > 0x200) continue;
        if (!BytesMatch(fn, "48 89 5C 24 18 55 56 57 41 56 41 57")) continue;
        if (n < N) fns[n] = fn;
        ++n;
    }
    if (n == 0) return Missing();
    if (n != N) return SigFail("service stream functions: the count changed");
    return SigOk(fns[K - 1]);
}

// ---- remote console only on this PC -------------------------------------------------------------

SigResult ResolveRemoteConsoleBind(const Image& img) {
    const uint8_t* s = FindCString(img.rdata, "Remote console listening on: %u\n");
    if (!s) return SigFail("the remote console's listening line isn't in .rdata");
    const uint8_t* lea = FindLeaFrom(img.text, s, img.text.base);
    if (!lea) return SigFail("the remote console's listening line isn't referenced");
    if (FindLeaFrom(img.text, s, lea + 8)) return SigFail("the listening line is referenced more than once");
    if (!InText(img, lea - 0x100, 0x100)) return SigFail("the listening line's reference is too close to the start of .text");
    const uint8_t* at = nullptr;
    int hits = 0;
    for (const uint8_t* p = lea - 0x100; p < lea; ++p)
        if (BytesMatch(p, "33 C9 FF 15 ?? ?? ?? ?? 0F B7 CB 66 89 7C 24 48 89 44 24 4C")) {
            if (!at) at = p;
            ++hits;
        }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(at);
}

// ---- hooks.cpp ---------------------------------------------------------------------------------

constexpr Check kInstanceGroupChecks[] = {
    { 0x5B, "48 8B 0D",          "layout changed at +0x05b" },   // mov rcx, [services manager]
    { 0x99, "48 8B 01 FF 50 18", "layout changed at +0x099" },   // mov rax, [rcx]; call [rax+0x18]
};
constexpr FnSpec kInstanceGroupQuery{
    "48 89 5C 24 08 48 89 74 24 18 48 89 7C 24 20 55 48 8D AC 24 70 FE FF FF 48 81 EC 90 02 00 00 49 8B 00 49 8B F8",
    kInstanceGroupChecks, std::size(kInstanceGroupChecks), nullptr };
constexpr RipSpec kServicesManager{ "elevator.instance_group_query", 0x5B, 3, 7 };

const char* EntitlementsExtra(const Image& img, const uint8_t* f) {
    const uint8_t* msg = FindCString(img.rdata, "QueryEntitlements failed with error: $$");
    if (!msg) return "the QueryEntitlements error line isn't in .rdata";
    return RipTarget(f + 0x55, 3, 7) == msg ? nullptr : "lea r9 at +0x55 doesn't load the QueryEntitlements error line";
}
constexpr Check kEntitlementsChecks[] = {
    { 0x55, "4C 8D 0D", "layout changed at +0x055" },   // lea r9, ["QueryEntitlements failed with error: $$"]
};
constexpr FnSpec kEntitlementsResult{
    "40 55 56 41 56 48 8D AC 24 00 FF FF FF 48 81 EC 00 02 00 00 4C 8B F1 48 8B F2 48 83 C1 08 E8",
    kEntitlementsChecks, std::size(kEntitlementsChecks), EntitlementsExtra };

SigResult ResolveTakeoffCommand(const Image& img) {
    const uint8_t* usage = FindCString(img.rdata,
        "Invalid arguments. Usage: g_ATC_requestTakeOff <atc_name (autocompletable)> [<ship_archetype>] [<pad_name_filter>]");
    if (!usage) return SigFail("the g_ATC_requestTakeOff usage line isn't in .rdata");
    uint8_t* lea = FindRipLea(img.text, 0x48, 0x8D, 0x15, usage);
    if (!lea) return SigFail("the g_ATC_requestTakeOff usage line isn't referenced");
    uint8_t* cmd = lea - 0x1CD;
    if (!InText(img, cmd, 0x195)) return SigFail("the command would start outside .text");
    if (!BytesMatch(cmd, "40 55 53 48 8B EC 48 83 EC 78")) return SigFail("layout changed at +0x000");
    if (!BytesMatch(cmd + 0x72, "E8")) return SigFail("layout changed at +0x072");
    if (!BytesMatch(cmd + 0xA9, "E8")) return SigFail("layout changed at +0x0a9");
    if (!BytesMatch(cmd + 0x190, "E8")) return SigFail("layout changed at +0x190");
    return SigOk(cmd);
}
constexpr RipSpec kStringCtor{ "fleet.takeoff_command", 0x72, 1, 5 };
constexpr RipSpec kStringDtor{ "fleet.takeoff_command", 0xA9, 1, 5 };
constexpr RipSpec kRequestTakingOff{ "fleet.takeoff_command", 0x190, 1, 5 };

// ---- dllmain.cpp: CSystem::Quit's fast-shutdown test ---------------------------------------------

SigResult ResolveQuitFastShutdown(const Image& img) {
    uint8_t* q = Sig("system.quit");
    if (!q) return SigFail("source row missing");
    uint8_t* t = q + 0x31A;
    if (!InText(img, t, 34)) return SigFail("CSystem::Quit +0x31a outside .text");
    if (!BytesMatch(t, "49 8B 8E ?? ?? ?? ?? 48 85 C9 74 0A 48 8B 01 FF 50 10 85 C0 75 0E "
                       "41 80 BE ?? ?? ?? ?? 00 0F 84"))
        return SigFail("CSystem::Quit's fast-shutdown test moved (+0x31a)");
    return SigOk(t);
}

// ---- capabilities ----------------------------------------------------------------------------

constexpr const char* kForceOffline[] = { "offline.is_online_store" };
constexpr const char* kHandshake[] = {
    "offline.handshake_gate.1", "offline.handshake_gate.2", "offline.handshake_gate.3", "offline.handshake_gate.4",
    "offline.is_online_flag",
};
constexpr const char* kMegamapCase[] = { "offline.megamap_tolower_call" };
constexpr const char* kBootPu[]      = { "offline.boot_frontend_request", "offline.str_pu", "offline.str_sc_default" };
constexpr const char* kBootPuAll[]   = { "offline.boot_frontend_request", "offline.str_megamap_pu_all", "offline.str_sc_default" };
constexpr const char* kOfflineDb[]   = { "offline.offline_db_path.1", "offline.offline_db_path.2", "offline.str_user" };
constexpr const char* kAsopGate[]    = { "offline.is_online_flag", "offline.asop_gate_open", "offline.asop_gate_validation" };
constexpr const char* kNoImpound[]   = { "offline.restricted_area_impound" };
constexpr const char* kMissions[]    = { "offline.use_service_default", "offline.use_online_mission_service_default" };
constexpr const char* kSocialGroup[] = {
    "offline.social_group.1",  "offline.social_group.2",  "offline.social_group.3",  "offline.social_group.4",
    "offline.social_group.5",  "offline.social_group.6",  "offline.social_group.7",  "offline.social_group.8",
    "offline.social_group.9",  "offline.social_group.10", "offline.social_group.11", "offline.social_group.12",
    "offline.social_group.13", "offline.social_group.14", "offline.social_group.15", "offline.social_group.16",
    "offline.social_group.17", "offline.social_group.18", "offline.social_group.19", "offline.social_group.20",
    "offline.social_group.21", "offline.social_group.22", "offline.social_group.23",
    "offline.services_env",
};
constexpr const char* kStreams[] = {
    "offline.service_stream.presence", "offline.service_stream.analytics", "offline.service_stream.trace",
    "offline.service_stream.echo.1",   "offline.service_stream.echo.2",
};
constexpr const char* kRemoteConsole[] = { "offline.remote_console_bind" };
constexpr const char* kProfiler[]      = { "offline.profiler_listen_branch" };
constexpr const char* kFilterHook[]     = { "inventory.validate_filter" };
constexpr const char* kProjectionHook[] = { "inventory.validate_projection" };
constexpr const char* kElevatorGuard[]  = { "elevator.instance_group_query", "elevator.services_manager" };
constexpr const char* kShipList[]       = { "fleet.entitlements_result" };
constexpr const char* kRetrieveAtc[]    = {
    "asop.fleet_retrieve", "atc.get_component", "fleet.takeoff_command", "fleet.string_ctor", "fleet.string_dtor",
    "fleet.request_taking_off",
};
constexpr const char* kQuitHook[] = { "system.quit", "system.quit_fast_shutdown" };
static_assert(std::size(kHandshake) == kHandshakeSites + 1);
static_assert(std::size(kOfflineDb) == kOfflineDbSites + 1);
static_assert(std::size(kSocialGroup) == kSocialGroupSites + 1);
static_assert(std::size(kStreams) == 3 + kEchoStreams);

constexpr Capability kCaps[] = {
    { "offline.force_offline",           kForceOffline,   std::size(kForceOffline) },
    { "offline.handshake",               kHandshake,      std::size(kHandshake) },
    { "offline.megamap_case",            kMegamapCase,    std::size(kMegamapCase) },
    { "offline.boot_pu",                 kBootPu,         std::size(kBootPu) },
    { "offline.boot_pu_all",             kBootPuAll,      std::size(kBootPuAll) },
    { "offline.db_path",                 kOfflineDb,      std::size(kOfflineDb) },
    { "offline.asop_shard_gate",         kAsopGate,       std::size(kAsopGate) },
    { "offline.no_impound",              kNoImpound,      std::size(kNoImpound) },
    { "offline.mission_services",        kMissions,       std::size(kMissions) },
    { "offline.social_group",            kSocialGroup,    std::size(kSocialGroup) },
    { "offline.service_streams",         kStreams,        std::size(kStreams) },
    { "offline.remote_console",          kRemoteConsole,  std::size(kRemoteConsole) },
    { "offline.profiler_server",         kProfiler,       std::size(kProfiler) },
    { "inventory.filter_validator",      kFilterHook,     std::size(kFilterHook) },
    { "inventory.projection_validator",  kProjectionHook, std::size(kProjectionHook) },
    { "elevator.crash_guard",            kElevatorGuard,  std::size(kElevatorGuard) },
    { "fleet.ship_list",                 kShipList,       std::size(kShipList) },
    { "fleet.retrieve_atc",              kRetrieveAtc,    std::size(kRetrieveAtc) },
    { "system.quit_hook",                kQuitHook,       std::size(kQuitHook) },
};

}  // namespace

namespace offline {

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace offline

#define SCO_HS(k)  { "offline.handshake_gate." #k, nullptr, 0, 0, ResolveHandshake<k>, {} }
#define SCO_DB(k)  { "offline.offline_db_path." #k, nullptr, 0, 0, ResolveOfflineDb<k>, {} }
#define SCO_SG(k)  { "offline.social_group." #k, nullptr, 0, 0, ResolveSocialGroup<k>, {} }

extern const SigDef kOfflineSignatures[] = {
    // patches.cpp
    { "offline.is_online_store",            "44 88 A0 0E 06 00 00", 0, 0, nullptr, {} },
    SCO_HS(1), SCO_HS(2), SCO_HS(3), SCO_HS(4),
    { "offline.is_online_flag",             nullptr, 0, 0, ResolveRip<kIsOnlineFlag>, { "offline.handshake_gate.1" } },
    { "offline.megamap_tolower_call",       nullptr, 0, 0, ResolveMegamapCall, {} },
    { "offline.boot_frontend_request",      nullptr, 0, 0, ResolveBootFrontend, {} },
    { "offline.str_pu",                     nullptr, 0, 0, ResolveString<kPu>, {} },
    { "offline.str_sc_default",             nullptr, 0, 0, ResolveString<kScDefault>, {} },
    { "offline.str_megamap_pu_all",         nullptr, 0, 0, ResolveString<kMegaMapAll>, {} },
    SCO_DB(1), SCO_DB(2),
    { "offline.str_user",                   nullptr, 0, 0, ResolveString<kUser>, {} },
    { "offline.asop_gate_open",             nullptr, 0, 0, ResolveFn<kAsopGateOpen>, { "offline.is_online_flag" } },
    { "offline.asop_gate_validation",       nullptr, 0, 0, ResolveFn<kAsopGateValidation>, { "offline.is_online_flag" } },
    { "offline.restricted_area_impound",    nullptr, 0, 0, ResolveFn<kImpound>, {} },
    { "offline.use_service_default",        nullptr, 0, 0, ResolveCvarDefault<kUseService>, {} },
    { "offline.use_online_mission_service_default", nullptr, 0, 0, ResolveCvarDefault<kUseOnlineMissions>, {} },
    SCO_SG(1),  SCO_SG(2),  SCO_SG(3),  SCO_SG(4),  SCO_SG(5),  SCO_SG(6),  SCO_SG(7),  SCO_SG(8),
    SCO_SG(9),  SCO_SG(10), SCO_SG(11), SCO_SG(12), SCO_SG(13), SCO_SG(14), SCO_SG(15), SCO_SG(16),
    SCO_SG(17), SCO_SG(18), SCO_SG(19), SCO_SG(20), SCO_SG(21), SCO_SG(22), SCO_SG(23),
    { "offline.services_env",               nullptr, 0, 0, ResolveRip<kServicesEnv>, { "offline.social_group.1" } },
    { "offline.service_stream.presence",    nullptr, 0, 0, ResolveServiceStream<kPresence, 1, 1>, {} },
    { "offline.service_stream.analytics",   nullptr, 0, 0, ResolveServiceStream<kAnalytics, 1, 1>, {} },
    { "offline.service_stream.trace",       nullptr, 0, 0, ResolveServiceStream<kTrace, 1, 1>, {} },
    { "offline.service_stream.echo.1",      nullptr, 0, 0, ResolveServiceStream<kEcho, 1, kEchoStreams>, {} },
    { "offline.service_stream.echo.2",      nullptr, 0, 0, ResolveServiceStream<kEcho, 2, kEchoStreams>, {} },
    { "offline.remote_console_bind",        nullptr, 0, 0, ResolveRemoteConsoleBind, {} },
    { "offline.profiler_listen_branch",
      "73 ?? 0F 1F 40 00 0F 1F 84 00 00 00 00 00 48 8B BD ?? ?? ?? ?? 0F B7 CE 66 44 89 67 10 44 89 7F 14 FF 15", 0, 0, nullptr, {} },
    // hooks.cpp
    { "inventory.validate_filter",
      "48 89 5C 24 18 48 89 74 24 20 57 48 83 EC 20 49 89 50 28 49 8B F8 41 8B 41 1C 48 8B DA", 0, 0, nullptr, {} },
    { "inventory.validate_projection",
      "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 20 48 8D 05 ?? ?? ?? ?? 49 8B F0", 0, 0, nullptr, {} },
    { "elevator.instance_group_query",      nullptr, 0, 0, ResolveFn<kInstanceGroupQuery>, {} },
    { "elevator.services_manager",          nullptr, 0, 0, ResolveRip<kServicesManager>, { "elevator.instance_group_query" } },
    { "fleet.entitlements_result",          nullptr, 0, 0, ResolveFn<kEntitlementsResult>, {} },
    { "fleet.takeoff_command",              nullptr, 0, 0, ResolveTakeoffCommand, {} },
    { "fleet.string_ctor",                  nullptr, 0, 0, ResolveRip<kStringCtor>, { "fleet.takeoff_command" } },
    { "fleet.string_dtor",                  nullptr, 0, 0, ResolveRip<kStringDtor>, { "fleet.takeoff_command" } },
    { "fleet.request_taking_off",           nullptr, 0, 0, ResolveRip<kRequestTakingOff>, { "fleet.takeoff_command" } },
    // dllmain.cpp
    { "system.quit_fast_shutdown",          nullptr, 0, 0, ResolveQuitFastShutdown, { "system.quit" } },
};
extern const size_t kOfflineSignatureCount = std::size(kOfflineSignatures);

#undef SCO_HS
#undef SCO_DB
#undef SCO_SG

}  // namespace sco::game
