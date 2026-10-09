#pragma once
// The engine's system object. Resolved by sco::ResolveAll().
//
//   system.quit  CSystem::Quit, the function that logs "CSystem::Quit invoked with - cause=$$, ..."
//                (found from that string, start at lea-0xA3, 9 layout checks plus the second
//                reference at +0x176). With ExitOnQuit the process ends inside it ("System Fast
//                Shutdown"), without WM_QUIT, so a host that must run shutdown work (sco::app::Stop)
//                hooks its entry and does it on the calling thread before the original runs.
#include <cstddef>
#include <cstdint>

namespace sco::game {

// Whole instructions at the start of CSystem::Quit that a detour may overwrite and relocate:
//   +0x00  48 89 5C 24 10    mov [rsp+10h], rbx
//   +0x05  48 89 74 24 18    mov [rsp+18h], rsi
//   +0x0A  48 89 7C 24 20    mov [rsp+20h], rdi
//   +0x0F  55                push rbp            (not stolen)
// 15 bytes covers a 14-byte absolute jmp (FF 25 00000000 <addr64>) and is position independent
// (no RIP-relative operand). A 5-byte rel32 jmp needs only the first instruction (5 bytes).
// The layout check at +0x000 pins all 15 bytes, so this holds whenever system.quit is OK.
constexpr size_t kQuitStolenBytes = 15;

struct QuitHook {
    uint8_t* fn          = nullptr;   // CSystem::Quit entry
    size_t   stolenBytes = 0;         // kQuitStolenBytes
};

// Fills `out`, or returns false (and leaves it untouched) when system.quit isn't OK.
bool QuitFunction(QuitHook& out);

}  // namespace sco::game
