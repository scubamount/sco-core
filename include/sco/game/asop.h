#pragma once
// Addresses behind offline ship terminals (ASOP), ship lifts, personal hangars and ATC. Resolved by
// sco::ResolveAll(); the rows are in src/game/asop_sigs.cpp (terminal, list, deliver, retrieve,
// insurance), src/game/atc_sigs.cpp (ATC data manager) and src/game/hangar_sigs.cpp (hangar
// instances, ship lifts, landing areas). docs/game/asop.md lists every row with its pattern source,
// its checks and the root cause (rc1..rc22 of the research doc) it serves.
//
// Rows are grouped into capabilities: a feature asks for its capability's rows with
// sco::caps::SetFromSignatures(c.name, c.rows, c.count) and runs only when that is ready, so one
// broken group disables one feature. A row may be in several groups.
//
// Resolve before patching or hooking: the rows match the game's original bytes (a 5-byte hook
// jmp over a prologue, or the OnRequestOpen patch at +0x95E, makes a later ResolveAll fail them).
#include <cstddef>
#include <cstdint>

namespace sco::game::asop {

struct Capability {
    const char*        name;    // "asop.terminal"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
    const char*        causes;  // root causes of the research doc it fixes, "rc1" / "rc7-rc11"
};

// Every ASOP / hangar / ATC capability, in a fixed order.
const Capability* Capabilities(size_t& count);

// Values the rows pin with byte checks: when the row is OK the game uses exactly these.
constexpr uint32_t kOpenAtcReload       = 0x616;   // OnRequestOpen: mov rdi, [rbp+0xD8] (asop.on_request_open)
constexpr uint32_t kOpenPatchSite       = 0x95E;   // OnRequestOpen: server half's jmp (E9 rel32) ...
constexpr uint32_t kOpenExit            = 0xDC0;   //   ... to the function's exit
constexpr uint32_t kOpenClientHalf      = 0x963;   // OnRequestOpen: client half, cmp byte [client gate], 0
constexpr uint32_t kCallerLookupSlot    = 0xA0;    // pGame vtable: remote-method caller lookup (asop.rm_request_deliver +0x3F)
constexpr uint32_t kInventoryConfigSlot = 0x1C0;   // pGame vtable: inventory configuration object (+0xEA)
constexpr uint32_t kInventoryLookupSlot = 0x08;    // its GetPersonalLocationInventoryConfiguration (+0x10D)
constexpr uint32_t kInventoryResultOk   = 0x30;    // result buffer [rbp+0x110], success byte [rbp+0x140] (+0xF7, +0x117)
constexpr uint32_t kKioskAtc            = 0x9F8;   // kiosk: ATC entity id (asop.request_vehicle +0x2F, asop.on_request_open +0x189)
constexpr uint32_t kKioskLocation       = 0x9E8;   // kiosk: location id written by the server half (asop.on_request_open +0x958)
constexpr uint32_t kKioskRetrieving     = 0xA28;   // kiosk: vehicle being retrieved (asop.request_vehicle +0x12F, +0x1A4, +0x5CE)
constexpr uint32_t kInsuranceTimeout    = 0x48;    // SInsuranceComponentConfig: request timeout, int32 seconds (insurance.update_pending_requests +0x9F)
constexpr uint32_t kDeferredUnstow      = 0x318;   // landing area component: deferred unstow vehicle id (landing.unstow_success +0x2220)
constexpr uint32_t kLiftSendEventSlot   = 0x380;   // entity system vtable: send a component event (lift.close_request +0xBC)

}  // namespace sco::game::asop
