// Signature rows for CSCAirTrafficControllerDataManager (ATC): storing, the retrieve request chain,
// landing tokens and the log-only probes. Patterns and guards are Appendix B of the research doc
// "Offline ship terminals (ASOP), ship lifts, personal hangars and ATC" (build 4.10.196.36804),
// byte for byte. Capabilities: src/game/asop_sigs.cpp.
#include "sco/scan.h"
#include "sco/signatures.h"
#include "sig_rows.h"
#include <iterator>

namespace sco::game {

namespace {

using rows::Check;
using rows::FnSpec;
using rows::ResolveFn;

// ---- store (rc3, rc19) -------------------------------------------------------------------------

constexpr FnSpec kStoreVehicle{
    "44 89 4C 24 20 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A8 FC FF FF", nullptr, 0, nullptr };

constexpr Check kStoreLambdaChecks[] = {
    { 0x012, "4C 8B 02",    "layout changed at +0x012" },   // mov r8, [rdx]: the token
    { 0x01F, "49 39 40 10", "layout changed at +0x01f" },   // cmp [r8+0x10], rax: the token's vehicle
};
constexpr FnSpec kStoreLambda{ "40 55 41 54 41 55 48 8D 6C 24 90 48 81 EC 70 01 00 00",
                               kStoreLambdaChecks, std::size(kStoreLambdaChecks), nullptr };

// ---- retrieve request chain (rc6, rc16) --------------------------------------------------------

constexpr FnSpec kProcessRequest{ "48 8B C4 48 89 48 08 55 53 57 41 55 48 8D A8 88 FE FF FF", nullptr, 0, nullptr };
constexpr FnSpec kQueueSpawnShip{
    "4C 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 B8 FE FF FF", nullptr, 0, nullptr };
constexpr FnSpec kRequestCancel{
    "48 89 5C 24 10 48 89 6C 24 18 56 57 41 54 41 56 41 57 48 83 EC 50 48 8B F1 48 8B FA", nullptr, 0, nullptr };
constexpr Check kSpawnCancelledChecks[] = {
    { 0x01C, "4C 8B E9 48 8B F2", "layout changed at +0x01c" },   // mov r13, rcx; mov rsi, rdx
};
constexpr FnSpec kSpawnCancelled{ "40 55 53 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 18 FF FF FF 48 81 EC F8 01 00 00",
                                  kSpawnCancelledChecks, std::size(kSpawnCancelledChecks), nullptr };

// ---- tokens (rc20, rc22) -----------------------------------------------------------------------

constexpr FnSpec kChangeTokenState{
    "48 89 5C 24 20 44 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 A0 FE FF FF 48 81 EC 80 02 00 00 48 8B 1A",
    nullptr, 0, nullptr };
constexpr FnSpec kClearToken{
    "44 88 4C 24 20 44 89 44 24 18 48 89 4C 24 08 55 53 56 57 41 54 41 55 41 57 48 8D AC 24 A0 FE FF FF 48 81 EC 90 02 00 00 48 8B 32",
    nullptr, 0, nullptr };

// ---- probes (log only, section 14) -------------------------------------------------------------

constexpr FnSpec kRequestTakingOff{
    "48 8B C4 48 89 48 08 55 48 8D A8 08 FE FF FF 48 81 EC F0 02 00 00 48 89 58 F0", nullptr, 0, nullptr };
constexpr FnSpec kRmRequestPermission{
    "40 55 53 56 57 41 55 41 56 41 57 48 8D AC 24 10 FF FF FF 48 81 EC F0 01 00 00 48 8D 05 ?? ?? ?? ??", nullptr, 0, nullptr };
constexpr FnSpec kOnPermissionRequested{
    "48 89 5C 24 10 48 89 6C 24 18 48 89 74 24 20 57 41 54 41 55 41 56 41 57 48 83 EC 30 4C 8D 3D ?? ?? ?? ??",
    nullptr, 0, nullptr };
constexpr FnSpec kNotifyPlayer{
    "40 55 56 57 41 54 41 55 41 56 41 57 48 8D AC 24 50 FF FF FF 48 81 EC C0 01 00 00", nullptr, 0, nullptr };
constexpr FnSpec kRmMulticastNotifyPlayer{
    "48 89 5C 24 10 4C 89 4C 24 20 4C 89 44 24 18 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 E0 48 81 EC 20 01 00 00 4C 8B E9",
    nullptr, 0, nullptr };

}  // namespace

extern const SigDef kAtcSignatures[] = {
    { "atc.store_vehicle",              nullptr, 0, 0, ResolveFn<kStoreVehicle>,            {} },
    { "atc.store_token_lambda",         nullptr, 0, 0, ResolveFn<kStoreLambda>,             {} },
    { "atc.process_request",            nullptr, 0, 0, ResolveFn<kProcessRequest>,         {} },
    { "atc.queue_spawn_ship",           nullptr, 0, 0, ResolveFn<kQueueSpawnShip>,          {} },
    { "atc.request_cancel",             nullptr, 0, 0, ResolveFn<kRequestCancel>,           {} },
    { "atc.on_vehicle_spawn_cancelled", nullptr, 0, 0, ResolveFn<kSpawnCancelled>,          {} },
    { "atc.change_token_state",         nullptr, 0, 0, ResolveFn<kChangeTokenState>,        {} },
    { "atc.clear_token",                nullptr, 0, 0, ResolveFn<kClearToken>,              {} },
    { "atc.request_taking_off",         nullptr, 0, 0, ResolveFn<kRequestTakingOff>,        {} },
    { "atc.rm_request_permission",      nullptr, 0, 0, ResolveFn<kRmRequestPermission>,     {} },
    { "atc.on_permission_requested",    nullptr, 0, 0, ResolveFn<kOnPermissionRequested>,   {} },
    { "atc.notify_player",              nullptr, 0, 0, ResolveFn<kNotifyPlayer>,            {} },
    { "atc.rm_multicast_notify_player", nullptr, 0, 0, ResolveFn<kRmMulticastNotifyPlayer>, {} },
};
extern const size_t kAtcSignatureCount = sizeof(kAtcSignatures) / sizeof(kAtcSignatures[0]);

}  // namespace sco::game
