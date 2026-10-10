#pragma once
// Addresses behind sc-offline's boot / offline-mode patches (patches.cpp), the hooks it installs at
// startup (hooks.cpp) and its CSystem::Quit hook's fast-shutdown test (dllmain.cpp). Resolved by
// sco::ResolveAll(); the rows are in src/game/offline_sigs.cpp, and docs/game/offline.md lists each
// one with what it checks. Each resolver is the scan sc-offline ran itself, moved byte for byte;
// where that scan took "every match", the row insists on the count found in 4.10.196.36804
// (numbered rows <id>.1..N), so a game patch that adds or loses a site shows as a failed row.
//
// Rows are grouped into capabilities like the feature rows (sco/game/features.h): sc-offline sets
// each with sco::caps::SetFromSignatures(c.name, c.rows, c.count) and patches or hooks only when it
// is ready. The OR-loop bound patch uses features' offline.or_loop_bound capability.
//
// Rows only find addresses. sc-offline writes the patches and installs the hooks, after
// ResolveAll: every row matches the game's original bytes.
#include <cstddef>
#include <cstdint>

namespace sco::game::offline {

struct Capability {
    const char*        name;    // "offline.handshake"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every capability of these patches and hooks, in a fixed order.
const Capability* Capabilities(size_t& count);

// ---- patches.cpp --------------------------------------------------------------------------------
// offline.is_online_store: the one `mov [rax+0x60E], r12b` (44 88 A0 0E 06 00 00), the IsOnline
//   store; the pattern pins the field offset 0x60E. sc-offline overwrites it with a 0 store.
// offline.handshake_gate.1 .. .<kHandshakeSites>: every `cmp [rip+flag], r8b-r15b; je` (44 38 ModRM
//   with mod/rm = rip, 0F 84 at +7) followed within +13..+25 by `lea rdx, "sessionGuid"`, in address
//   order. All of them compare the same flag. The je sc-offline nops (6 bytes) is at +kHandshakeJe.
constexpr int    kHandshakeSites = 4;
constexpr size_t kHandshakeJe    = 7;
// offline.is_online_flag: the flag those gates compare (RIP target of handshake_gate.1). The
//   persisted-shard byte the ASOP gate rows check is the next byte (+1).
// offline.megamap_tolower_call: in the `map` command's handler (the lea r8 within 32 bytes after the
//   `lea rdx, "Load a map, same usage as 'megamap' cvar."` registration, the handler inside .text),
//   the one `call` at +15 of two `lea rcx, [rsp+0x30]; call` pairs 10 bytes apart whose second
//   callee starts with the lowercase-copy prologue 4C 8B 09 4C 8B D1 41 0F B6 01 84 C0. 5 bytes.
// offline.boot_frontend_request: the one `lea r8, "SC_Frontend"` followed directly by
//   `lea rdx, "Frontend_Main"`; sc-offline repoints the rel32s at +3 and +10.
// offline.str_pu, offline.str_sc_default, offline.str_megamap_pu_all: those .rdata strings ("PU",
//   "SC_Default", "MegaMap.PU_All"; the "PU_All" name is at +kMegaMapPuAllName).
constexpr size_t kMegaMapPuAllName = 8;
// offline.offline_db_path.1 .. .<kOfflineDbSites>: every `lea r8, "Libs/OfflineDB"`, in address
//   order; offline.str_user: the "%USER%" string sc-offline repoints them to.
constexpr int kOfflineDbSites = 2;
// offline.asop_gate_open: the unique shard-gate pattern in OnRequestOpen whose `cmp [rip+X], r13b`
//   at +7 reads offline.is_online_flag + 1; sc-offline rewrites its jne at +kAsopGateOpenJne.
// offline.asop_gate_validation: the unique `cmp byte [rip+X], 0; jne` pattern reading that byte,
//   whose `lea r9` at +27 loads "Can only perform ASOP operations in the PU."; sc-offline makes
//   its jne at +kAsopGateValidationJne a jmp.
constexpr size_t kAsopGateOpenJne       = 14;
constexpr size_t kAsopGateValidationJne = 7;
// offline.restricted_area_impound: the unique impound function pattern whose `lea r9` at +0x59
//   loads the "... impounded (or destroyed) by restricted area ..." format and `lea r8` at +0x79
//   loads "Boundary Violation".
// offline.use_service_default, offline.use_online_mission_service_default: the
//   `mov r9d, 1` (41 B9 01 00 00 00) default of the contract_broker.use_service /
//   contract_broker.use_online_mission_service cvar registrations: 6 bytes before the first
//   `lea rdx, <name>`, with `lea r8, [rbx+..]` (4C 8D 43) 15 bytes before it.
// offline.social_group.1 .. .<kSocialGroupSites>: every `mov rcx, [rip+env]` followed within 0x30
//   by `mov rax, [rcx]; call [rax+0x18]` (services), within 0x18 of that by a load or call of
//   slot 0x70 (social API) and within 0x50 by slot 0xB0 (same group), in address order; all load
//   the same global. sc-offline repoints the rel32 at +3 to its stand-in.
// offline.services_env: that global (RIP target of social_group.1).
constexpr int    kSocialGroupSites = 23;   // 4.10.196.36804
constexpr size_t kEnvServicesSlot  = 0x18; // env -> services (checked by the scan)
constexpr size_t kSocialApiSlot    = 0x70; // services -> social API (checked by the scan)
constexpr size_t kSameGroupSlot    = 0xB0; // social API -> same group (checked by the scan)
// offline.service_stream.presence / .analytics / .trace, .echo.1 / .echo.2: the functions (.pdata
//   start within 0x200 before the reference, prologue 48 89 5C 24 18 55 56 57 41 56 41 57) that
//   reference the "CAsyncClient<...::<Service>>::CreateClientStream::<lambda_3>" name, one per
//   reference (echo has two). sc-offline makes them return.
constexpr int kEchoStreams = 2;
// offline.remote_console_bind: within 0x100 before the one reference to "Remote console listening
//   on: %u\n", the one `xor ecx, ecx; call [rip+X]; movzx ecx, bx; mov [rsp+0x48], di;
//   mov [rsp+0x4C], eax` (the bind address); sc-offline loads 127.0.0.1 there (8 bytes).
// offline.profiler_listen_branch: the unique `jae` before the Optick server's socket setup;
//   sc-offline makes it a jmp.

// ---- hooks.cpp ----------------------------------------------------------------------------------
// inventory.validate_filter, inventory.validate_projection: the unique prologue patterns.
// elevator.instance_group_query: the unique prologue pattern, with `mov rcx, [rip+X]` at +0x5B and
//   `mov rax, [rcx]; call [rax+0x18]` at +0x99; elevator.services_manager: that X.
constexpr size_t kServicesHubSlot = 0x18;   // services manager -> hub (checked at +0x99)
// fleet.entitlements_result: the unique prologue pattern whose `lea r9` at +0x55 loads
//   "QueryEntitlements failed with error: $$".
// fleet.takeoff_command: the g_ATC_requestTakeOff command, 0x1CD before the first
//   `lea rdx, "Invalid arguments. Usage: g_ATC_requestTakeOff ..."`; prologue 40 55 53 48 8B EC
//   48 83 EC 78 and calls at +0x72, +0xA9, +0x190. fleet.string_ctor / fleet.string_dtor /
//   fleet.request_taking_off: those three callees.
// The retrieve hook's target and ATC getter are the ASOP rows asop.fleet_retrieve and
// atc.get_component (same pattern and checks as sc-offline's old scan).

// ---- dllmain.cpp --------------------------------------------------------------------------------
// system.quit_fast_shutdown: system.quit + 0x31A, the fast-shutdown test
//   `mov rcx, [r14+cvar]; test rcx, rcx; je; mov rax, [rcx]; call [rax+0x10]; test eax, eax; jne;
//   cmp byte [r14+testMode], 0; je`. The CSystem offsets are the disp32s at +kQuitCVarDisp and
//   +kQuitTestModeDisp; GetIVal is cvar vtable slot kCVarGetIValSlot.
constexpr size_t kQuitCVarDisp     = 3;
constexpr size_t kQuitTestModeDisp = 0x19;
constexpr size_t kCVarGetIValSlot  = 0x10;

}  // namespace sco::game::offline
