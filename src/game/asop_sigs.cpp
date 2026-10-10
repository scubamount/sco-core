// Signature rows for the ship terminal (ASOP): open, list, deliver, retrieve, the insurance claim
// timeout and the log-only probes. Patterns and in-function guards are Appendix B of the research
// doc "Offline ship terminals (ASOP), ship lifts, personal hangars and ATC" (build 4.10.196.36804),
// byte for byte; checks marked "added" are not in the doc and were read from that build.
// The capability table for all ASOP / hangar / ATC rows is at the end of this file.
#include "sco/game/asop.h"
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <iterator>

namespace sco::game {

namespace {

using rows::Check;
using rows::FnSpec;
using rows::ResolveFn;
using rows::ResolveRip;
using rows::RipSpec;

// ---- terminal open (rc1) -----------------------------------------------------------------------

const char* OpenExtra(const Image&, const uint8_t* f) {
    // The server half's jmp at +0x95E must land on the exit at +0xDC0.
    if (f + asop::kOpenClientHalf + Rel32(f + asop::kOpenPatchSite + 1) != f + asop::kOpenExit)
        return "jmp at +0x95e doesn't reach +0xdc0";
    return nullptr;
}

constexpr Check kOpenChecks[] = {
    { 0x028, "44 38 2D ?? ?? ?? ??",  "layout changed at +0x028" },   // cmp [IsShardPersisted], r13b
    { 0x189, "48 8B 9E F8 09 00 00",  "layout changed at +0x189" },   // added: mov rbx, [rsi+0x9F8] (kiosk ATC id)
    { 0x616, "48 8B BD D8 00 00 00",  "layout changed at +0x616" },   // mov rdi, [rbp+0xD8] (ATC handle)
    { 0x958, "89 BE E8 09 00 00",     "layout changed at +0x958" },   // added: mov [rsi+0x9E8], edi (location id)
    { 0x95E, "E9 ?? ?? ?? ??",        "layout changed at +0x95e" },   // patch site
    { 0x963, "80 3D ?? ?? ?? ?? 00",  "layout changed at +0x963" },   // client half: cmp byte [client gate], 0
};
constexpr FnSpec kOpen{ "48 89 54 24 10 55 53 56 57 41 54 41 55 41 57 48 8D 6C 24 80",
                        kOpenChecks, std::size(kOpenChecks), OpenExtra };

constexpr RipSpec kShardPersisted{ "asop.on_request_open", 0x028, 3, 7 };
constexpr RipSpec kClientGate{ "asop.on_request_open", 0x963, 2, 7 };

// Shard gate, site 1: the pattern starts inside OnRequestOpen, at +0x21.
const char* GateOpenExtra(const Image&, const uint8_t* at) {
    return at == Sig("asop.on_request_open") + 0x21 ? nullptr : "not at asop.on_request_open+0x21";
}
constexpr FnSpec kGateOpen{ "44 89 AD D0 00 00 00 44 38 2D ?? ?? ?? ?? 0F 85 ?? ?? ?? ?? 48 8B 51 08 48 8D 8D D0 00 00 00 E8",
                            nullptr, 0, GateOpenExtra };

// Shard gate, site 2: inside the remote-method validation that the Deliver handler calls at +0xC3.
const char* GateValidationExtra(const Image& img, const uint8_t* at) {
    const uint8_t* fn = FunctionStart(img, at);
    if (!fn) return "no .pdata entry for the gate's function";
    return fn == Sig("asop.validate_caller") ? nullptr : "not in asop.validate_caller";
}
constexpr FnSpec kGateValidation{ "80 3D ?? ?? ?? ?? 00 75 ?? 48 8D 44 24 60 C7 44 24 60 E3 00 00 00 48 89 44 24 70 4C 8D 0D",
                                  nullptr, 0, GateValidationExtra };

// ---- list (rc4, rc15) --------------------------------------------------------------------------

constexpr FnSpec kFetchVehicles{ "48 89 4C 24 08 55 53 57 41 54 41 56 48 8D AC 24 00 FD FF FF", nullptr, 0, nullptr };
constexpr FnSpec kSetDataList{ "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 56 41 57 48 83 EC 50 48 8B FA 48 8B E9",
                               nullptr, 0, nullptr };
constexpr FnSpec kSetDeliveredOrClaimed{
    "4C 89 4C 24 20 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 88 FC FF FF", nullptr, 0, nullptr };
constexpr FnSpec kSetRetrieved{
    "48 89 5C 24 10 48 89 74 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D AC 24 50 FE FF FF 48 81 EC B0 02 00 00 48 8D 05 ?? ?? ?? ??",
    nullptr, 0, nullptr };
constexpr FnSpec kSetSpawned{
    "48 89 5C 24 18 48 89 54 24 10 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D9 48 81 EC 00 01 00 00 48 8D 05 ?? ?? ?? ??",
    nullptr, 0, nullptr };

// ---- deliver and caller lookup (rc2, rc3) ------------------------------------------------------

constexpr Check kDeliverChecks[] = {
    { 0x02C, "48 8B 0D ?? ?? ?? ??",       "layout changed at +0x02c" },   // mov rcx, [pGame]
    { 0x03F, "48 8B 01 FF 90 A0 00 00 00", "layout changed at +0x03f" },   // call [rax+0xA0]: caller lookup
    { 0x0C3, "E8 ?? ?? ?? ??",             "layout changed at +0x0c3" },   // call remote-method validation
    { 0x0EA, "FF 90 C0 01 00 00",          "layout changed at +0x0ea" },   // pGame->vfunc[0x1C0]
    { 0x0F7, "48 8D 95 10 01 00 00",       "layout changed at +0x0f7" },   // result buffer
    { 0x105, "C6 44 24 20 ??",             "layout changed at +0x105" },   // inventory kind immediate at +0x109
    { 0x10D, "4C 8B 51 08",                "layout changed at +0x10d" },   // vfunc 0x08
    { 0x117, "44 38 AD 40 01 00 00",       "layout changed at +0x117" },   // success byte at result +0x30
};
constexpr FnSpec kDeliver{ "48 89 5C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 30 FE FF FF 48 81 EC D0 02 00 00",
                           kDeliverChecks, std::size(kDeliverChecks), nullptr };
constexpr RipSpec kGame{ "asop.rm_request_deliver", 0x02C, 3, 7 };
constexpr RipSpec kValidateCaller{ "asop.rm_request_deliver", 0x0C3, 1, 5 };

// The inventory kind is an imm8 the feature reads from the game: the row is its address.
SigResult ResolveInventoryKind(const Image&) {
    uint8_t* d = Sig("asop.rm_request_deliver");
    return d ? SigOk(d + 0x109) : SigFail("source row missing");
}

constexpr FnSpec kDeliverSecond{ "40 55 53 41 54 41 55 41 56 41 57 48 8D AC 24 68 FE FF FF 48 81 EC 98 02 00 00",
                                 nullptr, 0, nullptr };

// Fleet manager retrieve: only read for the ATC component getter called at +0xDD.
constexpr Check kFleetRetrieveChecks[] = {
    { 0x0C7, "49 8B 95 F8 09 00 00", "layout changed at +0x0c7" },   // mov rdx, [r13+0x9F8]
    { 0x0DD, "E8 ?? ?? ?? ??",       "layout changed at +0x0dd" },   // call GetATCComponent
};
constexpr FnSpec kFleetRetrieve{
    "48 89 54 24 10 55 53 41 55 41 57 48 8D AC 24 A8 FE FF FF 48 81 EC 68 02 00 00 4C 8B FA 4C 8B E9 48 8B 51 08 48 8D 8D 80 01 00 00 E8",
    kFleetRetrieveChecks, std::size(kFleetRetrieveChecks), nullptr };
constexpr RipSpec kAtcGetComponent{ "asop.fleet_retrieve", 0x0DD, 1, 5 };

// ---- retrieve (rc17) ---------------------------------------------------------------------------

constexpr Check kRequestVehicleChecks[] = {
    { 0x02F, "49 8B 96 F8 09 00 00", "layout changed at +0x02f" },   // mov rdx, [r14+0x9F8]
    { 0x12F, "4D 8B 86 28 0A 00 00", "layout changed at +0x12f" },   // added: mov r8, [r14+0xA28]
    { 0x1A4, "49 8B 8E 28 0A 00 00", "layout changed at +0x1a4" },   // added: mov rcx, [r14+0xA28]
    { 0x5CE, "49 89 BE 28 0A 00 00", "layout changed at +0x5ce" },   // added: mov [r14+0xA28], rdi
};
constexpr FnSpec kRequestVehicle{ "4C 89 44 24 18 55 53 56 57 41 54 41 56 41 57 48 8D 6C 24 D9",
                                  kRequestVehicleChecks, std::size(kRequestVehicleChecks), nullptr };

// ---- claim timeout (rc5) -----------------------------------------------------------------------

constexpr Check kPendingChecks[] = {
    { 0x058, "48 8B 05 ?? ?? ?? ??", "layout changed at +0x058" },   // mov rax, [cached SInsuranceComponentConfig*]
    { 0x09F, "8B 70 48",             "layout changed at +0x09f" },   // mov esi, [rax+0x48]
};
constexpr FnSpec kPending{ "48 8B C4 55 41 57 48 8D A8 58 FF FF FF", kPendingChecks, std::size(kPendingChecks), nullptr };
constexpr RipSpec kInsuranceConfig{ "insurance.update_pending_requests", 0x058, 3, 7 };

// ---- probes (log only, section 14) -------------------------------------------------------------

constexpr Check kSubscribeChecks[] = {
    { 0x083, "8B 05 ?? ?? ?? ??", "layout changed at +0x083" },   // mov eax, [fetch event id]
};
constexpr FnSpec kSubscribe{ "40 55 53 56 57 41 57 48 8B EC 48 83 EC 70 48 8B F1 48 81 C1 50 01 00 00",
                             kSubscribeChecks, std::size(kSubscribeChecks), nullptr };
constexpr RipSpec kFetchEventId{ "asop.provider_subscribe", 0x083, 2, 6 };
constexpr FnSpec kFetchSender{
    "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 60 8B 1D ?? ?? ?? ?? 48 8D 79 08 48 8B F2 85 DB 75 1B 41 B8 66 01 00 00",
    nullptr, 0, nullptr };
constexpr FnSpec kContextSender{ "40 55 41 56 41 57 48 8B EC 48 83 EC 70 4C 8B F2 4C 8B F9", nullptr, 0, nullptr };
constexpr FnSpec kMobiglasSender{ "48 89 5C 24 20 56 57 41 57 48 83 EC 60 48 8B F1", nullptr, 0, nullptr };
constexpr FnSpec kInteractionTrigger{ "40 55 41 56 41 57 48 8D 6C 24 A0 48 81 EC 60 01 00 00 4C 8B 81 C8 00 00 00",
                                      nullptr, 0, nullptr };
constexpr FnSpec kSetStored{ "48 89 5C 24 18 48 89 7C 24 20 55 41 54 41 55 41 56 41 57 48 8D 6C 24 D1", nullptr, 0, nullptr };

}  // namespace

extern const SigDef kAsopSignatures[] = {
    // terminal open
    { "asop.on_request_open",         nullptr, 0, 0, ResolveFn<kOpen>,                  {} },
    { "asop.shard_persisted",         nullptr, 0, 0, ResolveRip<kShardPersisted>,       { "asop.on_request_open" } },
    { "asop.client_gate",             nullptr, 0, 0, ResolveRip<kClientGate>,           { "asop.on_request_open" } },
    { "asop.shard_gate_open",         nullptr, 0, 0, ResolveFn<kGateOpen>,              { "asop.on_request_open" } },
    { "asop.shard_gate_validation",   nullptr, 0, 0, ResolveFn<kGateValidation>,        { "asop.validate_caller" } },
    // list
    { "asop.fetch_vehicles",          nullptr, 0, 0, ResolveFn<kFetchVehicles>,         {} },
    { "asop.set_vehicle_data_list",   nullptr, 0, 0, ResolveFn<kSetDataList>,           {} },
    { "asop.set_delivered_or_claimed",nullptr, 0, 0, ResolveFn<kSetDeliveredOrClaimed>, {} },
    { "asop.set_retrieved",           nullptr, 0, 0, ResolveFn<kSetRetrieved>,          {} },
    { "asop.set_spawned",             nullptr, 0, 0, ResolveFn<kSetSpawned>,            {} },
    // deliver, caller lookup
    { "asop.rm_request_deliver",      nullptr, 0, 0, ResolveFn<kDeliver>,               {} },
    { "asop.game",                    nullptr, 0, 0, ResolveRip<kGame>,                 { "asop.rm_request_deliver" } },
    { "asop.validate_caller",         nullptr, 0, 0, ResolveRip<kValidateCaller>,       { "asop.rm_request_deliver" } },
    { "asop.inventory_kind",          nullptr, 0, 0, ResolveInventoryKind,              { "asop.rm_request_deliver" } },
    { "asop.rm_request_deliver_2",    nullptr, 0, 0, ResolveFn<kDeliverSecond>,         {} },
    { "asop.fleet_retrieve",          nullptr, 0, 0, ResolveFn<kFleetRetrieve>,         {} },
    { "atc.get_component",            nullptr, 0, 0, ResolveRip<kAtcGetComponent>,      { "asop.fleet_retrieve" } },
    // retrieve
    { "asop.request_vehicle",         nullptr, 0, 0, ResolveFn<kRequestVehicle>,        {} },
    // claim timeout
    { "insurance.update_pending_requests", nullptr, 0, 0, ResolveFn<kPending>,          {} },
    { "insurance.config",             nullptr, 0, 0, ResolveRip<kInsuranceConfig>,     { "insurance.update_pending_requests" } },
    // probes
    { "asop.provider_subscribe",      nullptr, 0, 0, ResolveFn<kSubscribe>,             {} },
    { "asop.fetch_event_id",          nullptr, 0, 0, ResolveRip<kFetchEventId>,         { "asop.provider_subscribe" } },
    { "asop.fetch_sender",            nullptr, 0, 0, ResolveFn<kFetchSender>,           {} },
    { "asop.context_sender",          nullptr, 0, 0, ResolveFn<kContextSender>,         {} },
    { "asop.mobiglas_sender",         nullptr, 0, 0, ResolveFn<kMobiglasSender>,        {} },
    { "asop.interaction_trigger",     nullptr, 0, 0, ResolveFn<kInteractionTrigger>,    {} },
    { "asop.set_stored",              nullptr, 0, 0, ResolveFn<kSetStored>,             {} },
};
extern const size_t kAsopSignatureCount = sizeof(kAsopSignatures) / sizeof(kAsopSignatures[0]);

namespace asop {

namespace {

constexpr const char* kTerminal[] = {
    "asop.on_request_open", "asop.client_gate", "asop.shard_gate_open", "asop.shard_gate_validation",
    "asop.validate_caller",
};
constexpr const char* kCaller[] = { "asop.rm_request_deliver", "asop.game" };
constexpr const char* kList[] = {
    "asop.fetch_vehicles", "asop.set_vehicle_data_list", "asop.set_delivered_or_claimed", "asop.set_retrieved",
    "asop.set_spawned",
};
constexpr const char* kDeliverRows[] = {
    "asop.rm_request_deliver", "asop.rm_request_deliver_2", "asop.game", "asop.inventory_kind", "atc.store_vehicle",
    "asop.fleet_retrieve", "atc.get_component",
};
constexpr const char* kClaimTimeout[] = { "insurance.update_pending_requests", "insurance.config", "asop.fetch_vehicles" };
constexpr const char* kRetrieve[] = {
    "asop.request_vehicle", "atc.process_request", "atc.queue_spawn_ship", "atc.request_cancel",
    "atc.on_vehicle_spawn_cancelled", "asop.fleet_retrieve", "atc.get_component", "landing.unstow_result", "landing.unstow_success",
    "respawn.on_vehicle_spawned", "asop.set_retrieved", "asop.set_spawned", "asop.set_delivered_or_claimed",
};
constexpr const char* kLift[] = {
    "lift.spawn_on_platform", "lift.close_request", "lift.open_request", "lift.manager_close_handler",
};
constexpr const char* kStore[] = {
    "atc.store_token_lambda", "atc.store_vehicle", "asop.fleet_retrieve", "atc.get_component", "lift.close_request", "lift.open_request",
    "lift.manager_close_handler",
};
constexpr const char* kInstance[] = {
    "hangar.services_hub_user", "hangar.services_hub", "hangar.request_instance", "hangar.iim_find_hangars",
    "hangar.iim_unstow_hangar", "hangar.iim_creation_batch", "hangar.entity_system", "hangar.iim_type_id",
    "hangar.request_permissions", "hangar.queue_instance", "hangar.teardown_conditions_met",
    "hangar.bubble_update_instance", "hangar.bubble_map_find",
};
constexpr const char* kTokens[] = { "atc.change_token_state", "atc.clear_token", "landing.on_reserving_entity_changed" };
constexpr const char* kDiagnostics[] = {
    "asop.shard_persisted", "asop.provider_subscribe", "asop.fetch_event_id", "asop.fetch_sender",
    "asop.context_sender", "asop.mobiglas_sender", "asop.interaction_trigger", "asop.set_stored",
    "atc.request_taking_off", "atc.rm_request_permission", "atc.on_permission_requested", "atc.notify_player",
    "atc.rm_multicast_notify_player", "hangar.permissions_continuation",
};

constexpr Capability kCaps[] = {
    { "asop.terminal",     kTerminal,     std::size(kTerminal),     "rc1" },
    { "asop.caller",       kCaller,       std::size(kCaller),       "rc2" },
    { "asop.list",         kList,         std::size(kList),         "rc4" },
    { "asop.deliver",      kDeliverRows,  std::size(kDeliverRows),  "rc3" },
    { "asop.claim_timeout",kClaimTimeout, std::size(kClaimTimeout), "rc5" },
    { "asop.retrieve",     kRetrieve,     std::size(kRetrieve),     "rc6 rc12 rc14-rc18" },
    { "hangar.lift",       kLift,         std::size(kLift),         "rc13" },
    { "atc.store",         kStore,        std::size(kStore),        "rc19" },
    { "hangar.instance",   kInstance,     std::size(kInstance),     "rc7-rc11" },
    { "atc.tokens",        kTokens,       std::size(kTokens),       "rc20-rc22" },
    { "asop.diagnostics",  kDiagnostics,  std::size(kDiagnostics),  "-" },
};

}  // namespace

const Capability* Capabilities(size_t& count) {
    count = std::size(kCaps);
    return kCaps;
}

}  // namespace asop

}  // namespace sco::game
