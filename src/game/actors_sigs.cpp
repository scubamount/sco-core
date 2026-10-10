// Signature rows for sc-offline's actor features: the spawner and what rides on it (seat control,
// Daymar lookup, Flight Ready, noclip's fly speed, god mode, prefabs), Clear NPCs' direct removal
// fallback, infinite ammo and the gear menu / outfit loader. Each resolver is the scan sc-offline
// ran itself (spawner.cpp, npc.cpp, ammo.cpp, loadout.cpp), moved byte for byte; where that scan
// took "the first match", the row now insists on the count found in 4.10.196.36804, so a game
// patch that adds or loses a site shows up as a failed row instead of a silent change.
#include "sco/game/actors.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <cstring>
#include <iterator>

namespace sco::game {

namespace {

using namespace actors;
using rows::Check;
using rows::FnSpec;
using rows::ResolveFn;
using rows::ResolveRip;
using rows::RipSpec;

SigResult Missing()        { return { SigState::Missing, nullptr, 0, nullptr }; }
SigResult Ambiguous(int n) { return { SigState::Ambiguous, nullptr, n, nullptr }; }

// Every `reg0 reg1 reg2 disp32` RIP-relative instruction in .text whose target is `target`, in
// address order (FindRipLea's scan, not stopping at the first). Returns the total count; up to
// `max` are written to `out`.
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

// The target of the call (E8 rel32) at `off` in row `from`.
struct CallSpec {
    const char* from;
    size_t      off;
    const char* why;   // static reason when there's no call there
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

// ---- spawner: the landing-area spawn helper and its calls ---------------------------------------

constexpr Check kHelperChecks[] = {
    { 0x000, "48 89 5C 24 10 4C 89 4C 24 20 56 57 41 54 41 56 41 57", "layout changed at +0x000" },
    { 0x04F, "B1 ?? E8",       "layout changed at +0x04f" },   // mov cl, team tag; call team category
    { 0x40C, "BA 00 10 00 00", "layout changed at +0x40c" },   // mov edx, 0x1000 (spawn flags)
    { 0x46D, "E8",             "layout changed at +0x46d" },   // call set flags
    { 0x47D, "E8",             "layout changed at +0x47d" },   // call set class
    { 0x55C, "E8",             "layout changed at +0x55c" },   // call set location
};

// The entity system calls sc-offline's spawner, npc, ammo and loadout code make with the same
// slots, as this helper makes them (offset sweep; not part of the scan sc-offline ran).
constexpr Check kHelperSlotChecks[] = {
    { 0x031, "48 8B 01 FF 90 28 01 00 00", "entity system slot 0x128 changed (+0x031)" },   // handle by id
    { 0x0F7, "48 8B B8 90 03 00 00",       "entity slot 0x390 changed (+0x0f7)" },          // component by type
    { 0x11D, "48 8B 01 FF 50 10",          "components slot 0x10 changed (+0x11d)" },       // type id by name
    { 0x29C, "48 8B 01 FF 90 C0 00 00 00", "entity system slot 0xc0 changed (+0x29c)" },    // class registry
    { 0x2A9, "48 8B 08 4C 8B 41 20",       "class registry slot 0x20 changed (+0x2a9)" },   // find class
    { 0x5DA, "48 8B 01 FF 90 18 01 00 00", "entity system slot 0x118 changed (+0x5da)" },   // spawn attributes
    { 0x765, "49 8B 06 48 8B 98 C8 00 00 00", "entity system slot 0xc8 changed (+0x765)" }, // create batch
    { 0x896, "48 8B 01 FF 50 10",          "spawn batch slot 0x10 changed (+0x896)" },      // spawn
    { 0x8E4, "4C 8B 80 D8 00 00 00",       "entity system slot 0xd8 changed (+0x8e4)" },    // release batch
};
// Where the helper loads the entity system global (teleport.entity_system) for those calls, and
// the components global next to it (+8; sc-offline reads it as gEnv+0xB0 = gEnv+0xA8+8).
constexpr size_t kHelperEntitySystemLoads[] = { 0x019, 0x295, 0x5CB, 0x75C, 0x8D2 };
constexpr size_t kHelperComponentsLoad = 0x10A;

const char* HelperSlots(const Image& img, const uint8_t* f) {
    for (const Check& c : kHelperSlotChecks)
        if (!rows::InText(img, f + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(f + c.off, c.bytes)) return c.why;
    const uint8_t* es = Sig("teleport.entity_system");
    for (size_t off : kHelperEntitySystemLoads)
        if (!BytesMatch(f + off + 1, "8B ?? ?? ?? ?? ??") || RipTarget(f + off, 3, 7) != es)
            return "the helper's entity system loads don't read teleport.entity_system";
    if (!BytesMatch(f + kHelperComponentsLoad, "48 8B 0D") || RipTarget(f + kHelperComponentsLoad, 3, 7) != es + 8)
        return "the components global isn't teleport.entity_system+8 (+0x10a)";
    return nullptr;
}

SigResult ResolveLandingHelper(const Image& img) {
    const uint8_t* msg = FindCString(img.rdata, "Landing Area could not be found.");
    if (!msg) return SigFail("the Landing Area message isn't in .rdata");
    const uint8_t* refs[kLandingAreaRefs] = {};
    const int n = RipRefs(img.text, 0x4C, 0x8D, 0x0D, msg, refs, kLandingAreaRefs);
    if (n == 0) return SigFail("the Landing Area message isn't referenced");
    if (n != kLandingAreaRefs) return SigFail("Landing Area message: the reference count changed");
    const uint8_t* f = refs[0] - 0x59;
    for (const Check& c : kHelperChecks)
        if (!rows::InText(img, f + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(f + c.off, c.bytes)) return SigFail(c.why);
    if (const char* why = HelperSlots(img, f)) return SigFail(why);
    return SigOk(f);
}

// The team tag is an imm8 the spawner passes back to the team category call: the row is its address.
SigResult ResolveTeamTag(const Image&) {
    uint8_t* f = Sig("spawn.landing_helper");
    return f ? SigOk(f + 0x50) : SigFail("source row missing");
}

constexpr CallSpec kTeamCategory{ "spawn.landing_helper", 0x051, "no call at +0x051" };
constexpr CallSpec kSetFlags{     "spawn.landing_helper", 0x46D, "no call at +0x46d" };
constexpr CallSpec kSetClass{     "spawn.landing_helper", 0x47D, "no call at +0x47d" };
constexpr CallSpec kSetLocation{  "spawn.landing_helper", 0x55C, "no call at +0x55c" };

constexpr FnSpec kParamsCtor{
    "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 20 33 F6 48 B8 00 00 00 00 00 00 F0 3F 48 89 71 08 48 8B F9 "
    "48 89 71 10 48 89 71 18 48 89 71 30 48 89 71 38 48 89 71 40 48 89 31", nullptr, 0, nullptr };
constexpr FnSpec kFindSeat{
    "40 55 41 54 41 56 48 8D AC 24 30 FC FF FF 48 81 EC D0 04 00 00 4D 8B E0 4C 8B F2 48 85 C9 0F 84", nullptr, 0, nullptr };

// ---- spawner: the seat picker's calls ------------------------------------------------------------

constexpr Check kSeatChecks[] = {
    { 0x1D6, "FF 90 78 07 00 00", "seat picker changed at +0x1d6" },   // call [rax+0x778]: the seat container
    { 0x223, "41 B8 C1 00 00 00", "seat item type 193 changed (+0x223)" },   // sweep: for_each_seat's item type
};
constexpr Check kSeatCallbackChecks[] = {
    { 0x037, "48 83 BF 58 01 00 00 00", "seat callback changed at +0x037" },   // cmp [rdi+0x158], 0
};

SigResult ResolveSeatCallback(const Image& img) {
    const uint8_t* s = Sig("spawn.find_seat");
    if (!s) return SigFail("source row missing");
    if (!rows::InText(img, s + 0x21C, 7) || !BytesMatch(s + 0x21C, "48 8D 05")) return SigFail("seat picker changed at +0x21c");
    for (const Check& c : kSeatChecks)
        if (!rows::InText(img, s + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(s + c.off, c.bytes)) return SigFail(c.why);
    const uint8_t* cb = RipTarget(s + 0x21C, 3, 7);
    for (const Check& c : kSeatCallbackChecks)
        if (!rows::InText(img, cb + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(cb + c.off, c.bytes)) return SigFail(c.why);
    return SigOk(cb);
}

// The seat callback's first test, before the occupant test: the seat picker skips a seat whose owner
// (seat+8) has no live IInteractableComponent. lea rdx, [rsp+X]; mov rcx, rdi; call getter;
// mov rcx, rax; call alive; test al, al; je.
constexpr Check kSeatGateCheck{
    0x01A, "48 8D 54 24 ?? 48 8B CF E8 ?? ?? ?? ?? 48 8B C8 E8 ?? ?? ?? ?? 84 C0 0F 84",
    "seat callback's interactable test changed (+0x01a)" };
// The getter it calls: the seat's owner handle, the entity's component slot, the components
// global (checked to be teleport.entity_system+8, the one EntityComponent reads), the component
// name (checked below) and the registry's name-to-type-id slot.
constexpr Check kSeatInteractableChecks[] = {
    { 0x006, "48 8B 41 08",          "interactable getter changed at +0x006" },   // mov rax, [rcx+8]
    { 0x048, "48 8B B0 90 03 00 00", "entity slot 0x390 changed (+0x048)" },     // mov rsi, [rax+0x390]
    { 0x05B, "48 8B 0D",             "interactable getter changed at +0x05b" },   // mov rcx, [components]
    { 0x062, "4C 8D 05",             "interactable getter changed at +0x062" },   // lea r8, "IInteractableComponent"
    { 0x06E, "48 8B 01 FF 50 10",    "components slot 0x10 changed (+0x06e)" },   // call [rax+0x10]
};

SigResult ResolveSeatInteractable(const Image& img) {
    const uint8_t* cb = Sig("spawn.seat_callback");
    const uint8_t* es = Sig("teleport.entity_system");
    if (!cb || !es) return SigFail("source row missing");
    const Check& g = kSeatGateCheck;
    if (!rows::InText(img, cb + g.off, rows::PatternBytes(g.bytes)) || !BytesMatch(cb + g.off, g.bytes)) return SigFail(g.why);
    const uint8_t* f = RipTarget(cb + 0x22, 1, 5);
    if (!rows::InText(img, f, 0x74)) return SigFail("interactable getter outside .text");
    for (const Check& c : kSeatInteractableChecks)
        if (!BytesMatch(f + c.off, c.bytes)) return SigFail(c.why);
    if (RipTarget(f + 0x5B, 3, 7) != es + 8) return SigFail("the getter's components global isn't teleport.entity_system+8");
    const uint8_t* name = RipTarget(f + 0x62, 3, 7);
    const size_t n = strlen(kSeatInteractable) + 1;
    if (!img.rdata.base || name < img.rdata.base || name + n > img.rdata.base + img.rdata.size || memcmp(name, kSeatInteractable, n) != 0)
        return SigFail("the getter doesn't name IInteractableComponent (+0x062)");
    return SigOk(f);
}

constexpr CallSpec kIsLinked{     "spawn.find_seat", 0x0B2, "seat picker changed at +0x0b2" };
constexpr CallSpec kForceDelink{  "spawn.find_seat", 0x19C, "seat picker changed at +0x19c" };
constexpr CallSpec kForEachSeat{  "spawn.find_seat", 0x242, "seat picker changed at +0x242" };
constexpr CallSpec kActorOfUser{  "spawn.find_seat", 0x3CE, "seat picker changed at +0x3ce" };
constexpr CallSpec kHandleToId{   "spawn.find_seat", 0x3F1, "seat picker changed at +0x3f1" };
constexpr CallSpec kActorLink{    "spawn.find_seat", 0x3FE, "seat picker changed at +0x3fe" };
constexpr CallSpec kForceLink{    "spawn.find_seat", 0x40B, "seat picker changed at +0x40b" };
constexpr CallSpec kSeatPriority{ "spawn.seat_callback", 0x0D6, "seat callback changed at +0x0d6" };

// ---- spawner: entity lookup by name (Daymar) ----------------------------------------------------

SigResult ResolveFindEntityByName(const Image& img) {
    const uint8_t* fmsg = FindCString(img.rdata, "FindEntityByName_SlowDebugCodeOnly: %s");
    if (!fmsg) return SigFail("the FindEntityByName message isn't in .rdata");
    const uint8_t* refs[16] = {};
    const int n = RipRefs(img.text, 0x48, 0x8D, 0x0D, fmsg, refs, 16);
    if (n > 16) return SigFail("more FindEntityByName references than the row checks");
    const uint8_t* found = nullptr;
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t* f = refs[i] - 0x36;
        if (refs[i] - img.text.base >= 0x36 && BytesMatch(f, "48 89 5C 24 08 48 89 74 24 10 57 48 83 EC 40 48 C7 02 00 00 00 00")) {
            found = f;
            ++hits;
        }
    }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(found);
}

// ---- spawner: the Flight Ready event sender ------------------------------------------------------

bool Contains(const uint8_t* from, const uint8_t* to, const char* pattern) {
    for (const uint8_t* q = from; q < to; ++q)
        if (BytesMatch(q, pattern)) return true;
    return false;
}

// Every event declared in Events/ISC/Dashboards.h gets a small sender function that loads that file
// name's address and passes the event's source line in r8d. Flight Ready is the one on line 0x49.
// The senders are found through their references to the file name string rather than by a fixed byte
// pattern, because game updates keep adding events with identical code.
SigResult ResolveFlightReady(const Image& img) {
    const Section& text = img.text;
    const uint8_t* header = FindCString(img.rdata, "C:\\workspace\\CryEngine\\Code\\CryEngine\\CryCommon\\Events/ISC/Dashboards.h");
    if (!header) return SigFail("the Dashboards.h path isn't in .rdata");
    if (text.size < 0x208) return Missing();
    const uint8_t* found = nullptr;
    int hits = 0;
    const uint8_t* const end = text.base + text.size - 8;
    for (const uint8_t* p = text.base + 0x200; p < end; ++p) {
        p = static_cast<const uint8_t*>(memchr(p, 0x8D, static_cast<size_t>(end - p)));
        if (!p) break;
        if ((p[-1] != 0x48 && p[-1] != 0x4C) || (p[1] & 0xC7) != 0x05 || p + 6 + Rel32(p + 2) != header) continue;
        const uint8_t* lea = p - 1;
        const uint8_t* start = nullptr;
        for (const uint8_t* q = lea; q > lea - 0x60 && q > text.base + 2; --q)
            if (q[-1] == 0xCC && q[-2] == 0xCC) { start = q; break; }
        if (!start || !BytesMatch(lea, "48 8D 15")) continue;
        uint32_t line = 0;
        for (const uint8_t* q = start; q + 6 <= lea; ++q)
            if (q[0] == 0x41 && q[1] == 0xB8) memcpy(&line, q + 2, 4);
        if (line == 0x49 && BytesMatch(start, "48 89 5C 24") && Contains(start, lea, "48 8B FA") && Contains(start, lea, "4C 89 44 24")) {
            found = start;
            ++hits;
        }
    }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(found);
}

// ---- spawner: noclip's fly speed -----------------------------------------------------------------

SigResult ResolveFlySpeed(const Image& img) {
    const uint8_t* speedName = FindCString(img.rdata, "g_FlyModeSpeedScaler");
    if (!speedName) return SigFail("g_FlyModeSpeedScaler isn't in .rdata");
    const uint8_t* refs[16] = {};
    const int n = RipRefs(img.text, 0x48, 0x8D, 0x15, speedName, refs, 16);
    if (n > 16) return SigFail("more g_FlyModeSpeedScaler references than the row checks");
    const uint8_t* found = nullptr;
    int hits = 0;
    for (int i = 0; i < n; ++i)
        if (refs[i] - img.text.base >= 0x14 && BytesMatch(refs[i] - 0x14, "4C 8D 87")) { found = refs[i] - 0x14; ++hits; }
    if (!hits) return Missing();
    if (hits > 1) return Ambiguous(hits);
    const int32_t off = Rel32(found + kFlySpeedDisp);
    if (off <= 0 || off >= 0x4000) return SigFail("fly speed field offset out of range");
    return SigOk(found);
}

// ---- spawner: god mode ---------------------------------------------------------------------------

SigResult ResolveGodModeCall(const Image& img) {
    const uint8_t* godMsg = FindCString(img.rdata, "Actor [$$] has changed GodMode State from [$$] to [$$]");
    if (!godMsg) return SigFail("the GodMode State message isn't in .rdata");
    SigResult r = SigPattern(img.text, "49 8B 8E 08 02 00 00 41 0F B6 D4 48 81 C1 F0 27 00 00 E8");
    if (r.state != SigState::Ok) return r;
    const uint8_t* p = r.at;
    if (p - img.text.base < 0x64 || !BytesMatch(p - 0x64, "4C 8D 0D") || RipTarget(p - 0x64, 3, 7) != godMsg)
        return SigFail("the GodMode State message isn't loaded at -0x064");
    return r;
}

SigResult ResolveSetGodMode(const Image& img) {
    const uint8_t* p = Sig("spawn.god_mode_call");
    if (!p) return SigFail("source row missing");
    const uint8_t* set = RipTarget(p + 0x12, 1, 5);
    if (!rows::InText(img, set, 0x10)) return SigFail("god mode setter outside .text");
    if (!BytesMatch(set, "48 89 5C 24 08 57 48 83 EC 20 88 91")) return SigFail("god mode setter changed at +0x000");
    return SigOk(set);
}

// ---- spawner: prefabs (ObjectContainers) ---------------------------------------------------------

constexpr Check kPrefabChecks[] = {
    { 0x37, "48 8B 0D", "prefab spawn changed at +0x37" },   // mov rcx, [system]
    { 0x41, "FF 90",    "prefab spawn changed at +0x41" },   // call [rax+path manager slot]
    { 0x58, "4C 8B 81", "prefab spawn changed at +0x58" },   // mov r8, [rcx+path id slot]
    { 0x6E, "48 8B 78", "prefab spawn changed at +0x6e" },   // mov rdi, [rax+attribute setter slot]
    { 0x72, "48 8D 05", "prefab spawn changed at +0x72" },   // lea rax, "ocFilename"
    { 0x94, "48 8D 05", "prefab spawn changed at +0x94" },   // lea rax, attribute writer
    { 0xA0, "E8",       "prefab spawn changed at +0xa0" },   // call attribute type id
};

SigResult ResolvePrefabSite(const Image& img) {
    const uint8_t* fmt = FindCString(img.rdata, "ObjectContainers\\%s");
    const uint8_t* ocName = FindCString(img.rdata, "ocFilename");
    if (!fmt || !ocName) return SigFail("ObjectContainers\\%s or ocFilename isn't in .rdata");
    const uint8_t* refs[kObjectContainersRefs] = {};
    const int n = RipRefs(img.text, 0x48, 0x8D, 0x15, fmt, refs, kObjectContainersRefs);
    if (n == 0) return SigFail("ObjectContainers\\%s isn't referenced");
    if (n != kObjectContainersRefs) return SigFail("ObjectContainers\\%s: the reference count changed");
    const uint8_t* s = refs[0];
    for (const Check& c : kPrefabChecks)
        if (!rows::InText(img, s + c.off, rows::PatternBytes(c.bytes)) || !BytesMatch(s + c.off, c.bytes)) return SigFail(c.why);
    if (RipTarget(s + 0x72, 3, 7) != ocName) return SigFail("prefab spawn doesn't load ocFilename at +0x72");
    return SigOk(s);
}

constexpr RipSpec  kPrefabSystem{ "spawn.prefab_site", 0x37, 3, 7 };
constexpr RipSpec  kPrefabAttrWriter{ "spawn.prefab_site", 0x94, 3, 7 };
constexpr CallSpec kPrefabAttrTypeId{ "spawn.prefab_site", 0xA0, "prefab spawn changed at +0xa0" };

// ---- Clear NPCs: RemoveEntity's internal remove (npc.cpp) ----------------------------------------

constexpr const char* kRemoveEntityPrologue = "48 89 5C 24 08 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57";

const char* DirectRemoveExtra(const Image& img, const uint8_t* at) {
    const uint8_t* fn = FunctionStart(img, at);
    if (!fn) return "no .pdata entry for the internal remove call";
    return at - fn < 0x700 ? nullptr : "the internal remove call is past +0x700 of its function";
}
constexpr FnSpec kDirectRemoveCall{ "48 8B D3 49 8B CD E8 ?? ?? ?? ?? 84 C0 74 0A 49 23 DC 81 4B 08 00 10 00 00",
                                    nullptr, 0, DirectRemoveExtra };

SigResult ResolveRemoveEntity(const Image& img) {
    const uint8_t* call = Sig("npc.direct_remove_call");
    if (!call) return SigFail("source row missing");
    const uint8_t* fn = FunctionStart(img, call);
    if (!fn) return SigFail("no .pdata entry for the internal remove call");
    if (!BytesMatch(fn, kRemoveEntityPrologue)) return SigFail("RemoveEntity changed at +0x000");
    return SigOk(fn);
}

constexpr CallSpec kDirectRemove{ "npc.direct_remove_call", 6, "no call at +0x06" };

// ---- infinite ammo: the magazine setter (ammo.cpp) -----------------------------------------------

// Sweep: the magazine fields sc-offline reads (the key, +0xC4, is in the pattern).
constexpr Check kSetAmmoChecks[] = {
    { 0x25, "44 33 B9 C0 00 00 00", "magazine count field changed (+0x25)" },   // xor r15d, [rcx+0xC0]
    { 0x3C, "8B B1 B8 00 00 00",    "magazine maximum field changed (+0x3c)" }, // mov esi, [rcx+0xB8]
};
constexpr FnSpec kSetAmmo{ "40 56 57 41 54 41 57 48 81 EC 88 00 00 00 8B 81 C4 00 00 00 45 33 FF 45 0F B6 E0 48 8B F9",
                           kSetAmmoChecks, std::size(kSetAmmoChecks), nullptr };

// ---- gear menu and outfits: the player loadout loader (loadout.cpp) ------------------------------

constexpr Check kLoadoutChecks[] = {
    { 0x00, "40 53 48 83 EC 20 48 8B 01 48 8B D9 FF 50 08 83 F8 01", "loadout loader changed at +0x00" },
    { 0x18, "48 8B 0D",          "loadout loader changed at +0x18" },   // mov rcx, [game]
    { 0x27, "FF 90",             "loadout loader changed at +0x27" },   // call [rax+framework slot]
    { 0x30, "48 8B 91",          "loadout loader changed at +0x30" },   // mov rdx, [rcx+actor slot]
    { 0x88, "41 B1 01",          "loadout loader changed at +0x88" },
    { 0x90, "41 B8 08 00 00 00", "loadout loader changed at +0x90" },
    { 0x99, "FF 90",             "loadout loader changed at +0x99" },   // call [rax+load slot]
};

SigResult ResolveLoadoutLoader(const Image& img) {
    const uint8_t* folder = FindCString(img.rdata, "Scripts/Loadouts/Player");
    if (!folder) return SigFail("Scripts/Loadouts/Player isn't in .rdata");
    const uint8_t* refs[16] = {};
    const int n = RipRefs(img.text, 0x48, 0x8D, 0x15, folder, refs, 16);
    if (n > 16) return SigFail("more Scripts/Loadouts/Player references than the row checks");
    const uint8_t* found = nullptr;
    const char* why = nullptr;
    int hits = 0;
    for (int i = 0; i < n; ++i) {
        const uint8_t* h = refs[i] - 0x44;
        if (refs[i] - img.text.base < 0x44 || !rows::InText(img, h, 0x9F)) continue;
        const char* fail = nullptr;
        for (const Check& c : kLoadoutChecks)
            if (!BytesMatch(h + c.off, c.bytes)) { fail = c.why; break; }
        if (fail) { if (!why) why = fail; continue; }
        found = h;
        ++hits;
    }
    if (!hits) return why ? SigFail(why) : Missing();
    if (hits > 1) return Ambiguous(hits);
    return SigOk(found);
}

constexpr RipSpec kLoadoutGame{ "loadout.load_player_loadout", 0x18, 3, 7 };

// ---- capabilities ----------------------------------------------------------------------------------

constexpr const char* kHelpers[] = {
    "teleport.entity_system", "spawn.landing_helper", "spawn.team_tag", "spawn.team_category", "spawn.set_flags",
    "spawn.set_class", "spawn.set_location", "spawn.params_ctor", "spawn.find_seat",
};
constexpr const char* kSeatPicker[] = {
    "spawn.find_seat", "spawn.seat_callback", "spawn.is_linked", "spawn.force_delink", "spawn.for_each_seat",
    "spawn.actor_of_user", "spawn.handle_to_id", "spawn.actor_link", "spawn.force_link", "spawn.seat_priority",
};
constexpr const char* kSeatGate[] = { "spawn.seat_callback", "spawn.seat_interactable" };
constexpr const char* kFindByName[] = { "spawn.find_entity_by_name" };
constexpr const char* kFlightReady[] = { "spawn.toggle_flight_ready" };
constexpr const char* kFlySpeed[] = { "spawn.game_cvars", "spawn.fly_speed_scaler" };
constexpr const char* kGodMode[] = { "spawn.god_mode_call", "spawn.set_god_mode" };
constexpr const char* kPrefabs[] = {
    "spawn.prefab_site", "spawn.prefab_system", "spawn.prefab_attr_writer", "spawn.prefab_attr_type_id",
};
constexpr const char* kDirectRemoveRows[] = {
    "npc.remove_entity_call", "npc.direct_remove_call", "npc.remove_entity", "npc.direct_remove",
};
constexpr const char* kAmmo[] = { "teleport.entity_system", "ammo.set_ammo" };
constexpr const char* kLoadout[] = { "loadout.load_player_loadout", "loadout.game" };

constexpr Capability kCaps[] = {
    { "spawn.helpers",       kHelpers,          std::size(kHelpers) },
    { "spawn.seat_picker",   kSeatPicker,       std::size(kSeatPicker) },
    { "spawn.seat_gate",     kSeatGate,         std::size(kSeatGate) },
    { "spawn.find_by_name",  kFindByName,       std::size(kFindByName) },
    { "spawn.flight_ready",  kFlightReady,      std::size(kFlightReady) },
    { "spawn.fly_speed",     kFlySpeed,         std::size(kFlySpeed) },
    { "spawn.god_mode",      kGodMode,          std::size(kGodMode) },
    { "spawn.prefabs",       kPrefabs,          std::size(kPrefabs) },
    { "npc.direct_remove",   kDirectRemoveRows, std::size(kDirectRemoveRows) },
    { "ammo.setter",         kAmmo,             std::size(kAmmo) },
    { "loadout.loader",      kLoadout,          std::size(kLoadout) },
};

}  // namespace

namespace actors {

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace actors

extern const SigDef kActorsSignatures[] = {
    // spawner
    { "spawn.landing_helper",       nullptr, 0, 0, ResolveLandingHelper,             { "teleport.entity_system" } },
    { "spawn.team_tag",             nullptr, 0, 0, ResolveTeamTag,                   { "spawn.landing_helper" } },
    { "spawn.team_category",        nullptr, 0, 0, ResolveCall<kTeamCategory>,       { "spawn.landing_helper" } },
    { "spawn.set_flags",            nullptr, 0, 0, ResolveCall<kSetFlags>,           { "spawn.landing_helper" } },
    { "spawn.set_class",            nullptr, 0, 0, ResolveCall<kSetClass>,           { "spawn.landing_helper" } },
    { "spawn.set_location",         nullptr, 0, 0, ResolveCall<kSetLocation>,        { "spawn.landing_helper" } },
    { "spawn.params_ctor",          nullptr, 0, 0, ResolveFn<kParamsCtor>,           {} },
    { "spawn.find_seat",            nullptr, 0, 0, ResolveFn<kFindSeat>,             {} },
    // seat picker
    { "spawn.seat_callback",        nullptr, 0, 0, ResolveSeatCallback,              { "spawn.find_seat" } },
    { "spawn.is_linked",            nullptr, 0, 0, ResolveCall<kIsLinked>,           { "spawn.find_seat" } },
    { "spawn.force_delink",         nullptr, 0, 0, ResolveCall<kForceDelink>,        { "spawn.find_seat" } },
    { "spawn.for_each_seat",        nullptr, 0, 0, ResolveCall<kForEachSeat>,        { "spawn.find_seat" } },
    { "spawn.actor_of_user",        nullptr, 0, 0, ResolveCall<kActorOfUser>,        { "spawn.find_seat" } },
    { "spawn.handle_to_id",         nullptr, 0, 0, ResolveCall<kHandleToId>,         { "spawn.find_seat" } },
    { "spawn.actor_link",           nullptr, 0, 0, ResolveCall<kActorLink>,          { "spawn.find_seat" } },
    { "spawn.force_link",           nullptr, 0, 0, ResolveCall<kForceLink>,          { "spawn.find_seat" } },
    { "spawn.seat_priority",        nullptr, 0, 0, ResolveCall<kSeatPriority>,       { "spawn.seat_callback" } },
    { "spawn.seat_interactable",    nullptr, 0, 0, ResolveSeatInteractable,          { "spawn.seat_callback", "teleport.entity_system" } },
    // Daymar, Flight Ready, fly speed, god mode
    { "spawn.find_entity_by_name",  nullptr, 0, 0, ResolveFindEntityByName,          {} },
    { "spawn.toggle_flight_ready",  nullptr, 0, 0, ResolveFlightReady,               {} },
    { "spawn.game_cvars",           "48 89 83 38 01 00 00 B9 98 00 00 00 48 89 05", 0x0F, 0x13, nullptr, {} },
    { "spawn.fly_speed_scaler",     nullptr, 0, 0, ResolveFlySpeed,                  {} },
    { "spawn.god_mode_call",        nullptr, 0, 0, ResolveGodModeCall,               {} },
    { "spawn.set_god_mode",         nullptr, 0, 0, ResolveSetGodMode,                { "spawn.god_mode_call" } },
    // prefabs
    { "spawn.prefab_site",          nullptr, 0, 0, ResolvePrefabSite,                {} },
    { "spawn.prefab_system",        nullptr, 0, 0, ResolveRip<kPrefabSystem>,        { "spawn.prefab_site" } },
    { "spawn.prefab_attr_writer",   nullptr, 0, 0, ResolveRip<kPrefabAttrWriter>,    { "spawn.prefab_site" } },
    { "spawn.prefab_attr_type_id",  nullptr, 0, 0, ResolveCall<kPrefabAttrTypeId>,   { "spawn.prefab_site" } },
    // Clear NPCs
    { "npc.direct_remove_call",     nullptr, 0, 0, ResolveFn<kDirectRemoveCall>,     {} },
    { "npc.remove_entity",          nullptr, 0, 0, ResolveRemoveEntity,              { "npc.direct_remove_call" } },
    { "npc.direct_remove",          nullptr, 0, 0, ResolveCall<kDirectRemove>,       { "npc.direct_remove_call" } },
    // infinite ammo
    { "ammo.set_ammo",              nullptr, 0, 0, ResolveFn<kSetAmmo>,              {} },
    // gear menu and outfits
    { "loadout.load_player_loadout",nullptr, 0, 0, ResolveLoadoutLoader,             {} },
    { "loadout.game",               nullptr, 0, 0, ResolveRip<kLoadoutGame>,         { "loadout.load_player_loadout" } },
};
extern const size_t kActorsSignatureCount = std::size(kActorsSignatures);

}  // namespace sco::game
