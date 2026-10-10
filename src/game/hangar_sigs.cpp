// Signature rows for personal hangars (the instanced interior manager, streaming bubbles, the
// services hub), ship lifts and landing areas. Patterns and guards are Appendix B of the research
// doc "Offline ship terminals (ASOP), ship lifts, personal hangars and ATC" (build 4.10.196.36804),
// byte for byte; landing.unstow_success is added (not in the doc) to check the landing area's
// deferred-unstow field +0x318 that rc18 clears. Capabilities: src/game/asop_sigs.cpp.
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <iterator>

namespace sco::game {

namespace {

using rows::Check;
using rows::FnSpec;
using rows::InText;
using rows::ResolveFn;
using rows::ResolveRip;
using rows::RipSpec;

// ---- services hub (rc10) -----------------------------------------------------------------------

constexpr Check kHubUserChecks[] = {
    { 0x026, "48 8B 0D ?? ?? ?? ??", "layout changed at +0x026" },   // mov rcx, [hub]
};
constexpr FnSpec kHubUser{
    "40 55 53 41 55 41 57 48 8D AC 24 B8 FE FF FF 48 81 EC 48 02 00 00 45 33 ED 44 38 2D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 18 48 8B 08 48 8B 51 40",
    kHubUserChecks, std::size(kHubUserChecks), nullptr };
constexpr RipSpec kHub{ "hangar.services_hub_user", 0x026, 3, 7 };

// ---- instance request (rc7-rc11) ---------------------------------------------------------------

constexpr char kRequesting[] = "IIM_RequestInstanceImpl_Requesting";
constexpr char kRequestInstancePrologue[] = "48 89 5C 24 10 48 89 4C 24 08 55 56 57";

// RequestInstanceImpl: the function holding `lea r8, [rip+"IIM_RequestInstanceImpl_Requesting"]`
// (4.10.196 references it twice, at +0xA9 and +0x2A5; every reference must be in that function).
SigResult ResolveRequestInstance(const Image& img) {
    const uint8_t* msg = FindCString(img.rdata, kRequesting);
    if (!msg) return { SigState::Missing, nullptr, 0, nullptr };
    uint8_t* site = FindRipLea(img.text, 0x4C, 0x8D, 0x05, msg);
    if (!site) return SigFail("IIM_RequestInstanceImpl_Requesting isn't referenced");
    uint8_t* fn = FunctionStart(img, site);
    if (!fn) return SigFail("no .pdata entry for the Requesting string's function");
    if (!InText(img, fn, 13) || !BytesMatch(fn, kRequestInstancePrologue)) return SigFail("layout changed at +0x000");
    int sites = 1;
    for (uint8_t* p = site + 1; InText(img, p, 7);) {
        const Section rest{ p, static_cast<size_t>(img.text.base + img.text.size - p) };
        uint8_t* next = FindRipLea(rest, 0x4C, 0x8D, 0x05, msg);
        if (!next) break;
        ++sites;
        if (FunctionStart(img, next) != fn) return { SigState::Ambiguous, nullptr, sites, "Requesting string used by two functions" };
        p = next + 1;
    }
    return SigOk(fn);
}

constexpr FnSpec kFindHangars{ "48 89 54 24 10 48 89 4C 24 08 55 53 41 55 41 57 48 8D AC 24 68 F0 FF FF", nullptr, 0, nullptr };
constexpr FnSpec kUnstowHangar{ "40 55 53 56 57 41 55 41 56 41 57 48 8D AC 24 B0 FA FF FF", nullptr, 0, nullptr };
constexpr Check kBatchChecks[] = {
    { 0x025, "48 8B 0D ?? ?? ?? ??", "layout changed at +0x025" },   // mov rcx, [entity system]
    { 0x053, "0F B7 0D ?? ?? ?? ??", "layout changed at +0x053" },   // movzx ecx, word [IIM component type id]
    { 0x0BE, "E8 ?? ?? ?? ??",       "layout changed at +0x0be" },   // record lookup (probe only)
    { 0x0F4, "E8 ?? ?? ?? ??",       "layout changed at +0x0f4" },   // thread info (probe only)
    { 0x10C, "E8 ?? ?? ?? ??",       "layout changed at +0x10c" },   // access check (probe only)
};
constexpr FnSpec kCreationBatch{
    "48 89 5C 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 80 FB FF FF 48 81 EC 80 05 00 00 48 8B F9",
    kBatchChecks, std::size(kBatchChecks), nullptr };
constexpr RipSpec kEntitySystem{ "hangar.iim_creation_batch", 0x025, 3, 7 };
constexpr RipSpec kIimTypeId{ "hangar.iim_creation_batch", 0x053, 3, 7 };

constexpr FnSpec kRequestPermissions{
    "4C 89 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 78 FF FF FF 48 81 EC 88 01 00 00 48 8B F9",
    nullptr, 0, nullptr };
constexpr FnSpec kPermissionsContinuation{ "48 89 54 24 10 48 89 4C 24 08 55 53 56 57 48 8D AC 24 38 FE FF FF", nullptr, 0, nullptr };
constexpr FnSpec kQueueInstance{
    "48 89 5C 24 10 48 89 74 24 18 55 57 41 54 41 56 41 57 48 8D 6C 24 C9 48 81 EC B0 00 00 00 48 8D 79 50", nullptr, 0, nullptr };
constexpr FnSpec kTearDown{
    "44 88 44 24 18 48 89 54 24 10 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 48 FB FF FF", nullptr, 0, nullptr };
constexpr Check kBubbleChecks[] = {
    { 0x031, "E8 ?? ?? ?? ??", "layout changed at +0x031" },   // call PrimaryBubbleMap find
};
constexpr FnSpec kBubbleUpdate{ "48 89 5C 24 08 4C 89 4C 24 20 48 89 54 24 10 55 56 57 48 8D 6C 24 B9",
                                kBubbleChecks, std::size(kBubbleChecks), nullptr };
constexpr RipSpec kBubbleFind{ "hangar.bubble_update_instance", 0x031, 1, 5 };

// ---- ship lift (rc13, rc19) --------------------------------------------------------------------

constexpr Check kPlatformSpawnChecks[] = {
    { 0x059, "4C 8D 0D", "layout changed at +0x059" },   // disambiguates the short pattern
};
constexpr FnSpec kPlatformSpawn{ "48 89 5C 24 10 4C 89 4C 24 20 56 57", kPlatformSpawnChecks, std::size(kPlatformSpawnChecks), nullptr };
constexpr Check kLiftCloseChecks[] = {
    { 0x0BC, "FF 90 80 03 00 00", "layout changed at +0x0bc" },   // entity system vfunc 0x380: send event
};
constexpr FnSpec kLiftClose{ "48 83 EC 58 48 8B 0D ?? ?? ?? ?? 4C 8B C2", kLiftCloseChecks, std::size(kLiftCloseChecks), nullptr };
constexpr Check kLiftOpenChecks[] = {
    { 0x03B, "E8 ?? ?? ?? ??", "layout changed at +0x03b" },   // inner call
};
constexpr FnSpec kLiftOpen{ "48 83 EC 28 48 8B 0D ?? ?? ?? ?? 4C 8B C2 48 8D 54 24 48", kLiftOpenChecks, std::size(kLiftOpenChecks), nullptr };
constexpr FnSpec kLiftManagerClose{
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 C9 48 81 EC C0 00 00 00 48 8B F9 4C 8B E2",
    nullptr, 0, nullptr };

// ---- landing area (rc12, rc14, rc18, rc21) -----------------------------------------------------

constexpr FnSpec kUnstowResult{ "40 55 53 41 56 48 8D AC 24 10 F8 FF FF", nullptr, 0, nullptr };
// Added: the unstow success path (context only in the doc, 0x05ED5010 in 4.10.196) is the one
// writer of the deferred unstow id; the check pins its offset +0x318.
constexpr Check kUnstowSuccessChecks[] = {
    { 0x2220, "49 89 BD 18 03 00 00", "layout changed at +0x2220" },   // mov [r13+0x318], rdi
};
constexpr FnSpec kUnstowSuccess{ "48 8B C4 4C 89 40 18 55 41 54 41 55 48 8D A8 F8 FD FF FF 48 81 EC F0 02 00 00",
                                 kUnstowSuccessChecks, std::size(kUnstowSuccessChecks), nullptr };
constexpr FnSpec kReservingChanged{ "48 8B C4 55 56 48 8D A8 38 FF FF FF 48 81 EC C8 01 00 00 80 3D", nullptr, 0, nullptr };
constexpr FnSpec kRespawnOnVehicleSpawned{ "40 55 53 56 57 41 55 48 8D AC 24 30 FF FF FF 48 81 EC E0 01 00 00", nullptr, 0, nullptr };

}  // namespace

extern const SigDef kHangarSignatures[] = {
    { "hangar.services_hub_user",       nullptr, 0, 0, ResolveFn<kHubUser>,                 {} },
    { "hangar.services_hub",            nullptr, 0, 0, ResolveRip<kHub>,                    { "hangar.services_hub_user" } },
    { "hangar.request_instance",        nullptr, 0, 0, ResolveRequestInstance,              {} },
    { "hangar.iim_find_hangars",        nullptr, 0, 0, ResolveFn<kFindHangars>,             {} },
    { "hangar.iim_unstow_hangar",       nullptr, 0, 0, ResolveFn<kUnstowHangar>,            {} },
    { "hangar.iim_creation_batch",      nullptr, 0, 0, ResolveFn<kCreationBatch>,           {} },
    { "hangar.entity_system",           nullptr, 0, 0, ResolveRip<kEntitySystem>,           { "hangar.iim_creation_batch" } },
    { "hangar.iim_type_id",             nullptr, 0, 0, ResolveRip<kIimTypeId>,              { "hangar.iim_creation_batch" } },
    { "hangar.request_permissions",     nullptr, 0, 0, ResolveFn<kRequestPermissions>,      {} },
    { "hangar.permissions_continuation",nullptr, 0, 0, ResolveFn<kPermissionsContinuation>, {} },
    { "hangar.queue_instance",          nullptr, 0, 0, ResolveFn<kQueueInstance>,           {} },
    { "hangar.teardown_conditions_met", nullptr, 0, 0, ResolveFn<kTearDown>,                {} },
    { "hangar.bubble_update_instance",  nullptr, 0, 0, ResolveFn<kBubbleUpdate>,            {} },
    { "hangar.bubble_map_find",         nullptr, 0, 0, ResolveRip<kBubbleFind>,             { "hangar.bubble_update_instance" } },
    { "lift.spawn_on_platform",         nullptr, 0, 0, ResolveFn<kPlatformSpawn>,           {} },
    { "lift.close_request",             nullptr, 0, 0, ResolveFn<kLiftClose>,               {} },
    { "lift.open_request",              nullptr, 0, 0, ResolveFn<kLiftOpen>,                {} },
    { "lift.manager_close_handler",     nullptr, 0, 0, ResolveFn<kLiftManagerClose>,        {} },
    { "landing.unstow_result",          nullptr, 0, 0, ResolveFn<kUnstowResult>,            {} },
    { "landing.unstow_success",         nullptr, 0, 0, ResolveFn<kUnstowSuccess>,           {} },
    { "landing.on_reserving_entity_changed", nullptr, 0, 0, ResolveFn<kReservingChanged>,   {} },
    { "respawn.on_vehicle_spawned",     nullptr, 0, 0, ResolveFn<kRespawnOnVehicleSpawned>, {} },
};
extern const size_t kHangarSignatureCount = sizeof(kHangarSignatures) / sizeof(kHangarSignatures[0]);

}  // namespace sco::game
