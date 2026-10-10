#pragma once
// The mission script library behind sc-offline's missions feature (docs/game-services.md, internal
// game-pack helpers). Moved from sc-offline's missions.cpp, where the lookup was the one raw scan
// helper left under src/ (a BytesMatch on a function reached through a live object). Same reads,
// same canary, same constants. Game pack, Windows only (sco_game_services).
//
// Game thread. ScriptLibrary reads game memory under __try itself; Init() isn't needed (it reads
// through the manager you pass, not through a row).
#include <cstdint>

namespace sco::game::missions {

// Why ScriptLibrary answered 0. Stable strings: a product logs them as they are.
constexpr const char* kReasonNotFound = "the mission manager's script library wasn't found";
constexpr const char* kReasonNotYet   = "the script library isn't there yet";

// manager: the live mission manager, the object vtable slot 0xA0 of the missions.subsumption
// global returns. Returns the script library (the object the XML file-change listener is called
// on at +8), or 0 with *reason (when not null) set to one of the strings above:
//   kReasonNotFound  the manager is 0, the manager's vtable[0x48] (mission_load_all's handler) is 0,
//                    its bytes at +0x32 aren't "48 8B 4B" (mov rcx, [rbx+disp8]), or a read faulted;
//   kReasonNotYet    the library field is still 0 (the game hasn't created it).
// On success *reason is set to nullptr.
//
// Checked-at-runtime constants, valid for 4.10.196.36804 and verified by the canary on every call,
// never trusted: slot 0x48 of the manager's vtable, the canary at +0x32 of the function it holds,
// and the field displacement, the disp8 byte at +0x35 of that function (the library is at
// manager + that byte).
uintptr_t ScriptLibrary(uintptr_t manager, const char** reason);

}  // namespace sco::game::missions
