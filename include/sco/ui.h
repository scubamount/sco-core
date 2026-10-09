#pragma once
// sco::ui: the host side of the "sco.ui" service (include/sco_ui.h): the registry of plugin tabs,
// overlays and badges, and the hotkey table. C++ and internal: the product renders what is
// registered and feeds it keys; plugins only see the C table. sco-core has no renderer: no
// ImGui, no game code.
//
//   static const char* const kReserved[] = { "f6", "f7", "f8" };   // the product's own keys
//   sco::ui::Start();                                  // publishes sco.ui 1.0 (sco::app::Start does)
//   for (const char* c : kReserved) sco::ui::ReserveChord(c);
//   ... each frame, on the game thread:
//   for (const sco::ui::TabInfo& t : sco::ui::Tabs())  // ordered; draw the tab strip
//       if (ImGui::BeginTabItem(t.title.c_str())) { sco::ui::DrawTab(t.id.c_str(), ImGui::GetCurrentContext()); ... }
//   sco::ui::DrawOverlays(ImGui::GetCurrentContext());
//   ... on a key press the product doesn't handle itself:
//   std::string chord;
//   if (sco::ui::NormalizeChord("ctrl+alt+4", chord)) sco::ui::Dispatch(chord.c_str(), &reply);
//   sco::ui::Stop();                                   // after every plugin unloaded
//
// Owners are runtime owners: a plugin's sco_plugin handle. A release hook (sco::AddReleaseHook)
// withdraws everything an owner registered once Release(owner) runs: unload, failed load, crash.
// Draw callbacks run through the runtime's callout guard as callouts of their owner (where = the
// tab or overlay id), so with sco::plugins::ContainCallouts installed a fault in one disables
// that plugin, releases it and with it its UI; the other plugins keep drawing. Reference:
// docs/ui.md.
#include "sco_ui.h"
#include "sco/runtime.h"
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace sco::ui {

constexpr size_t kMaxTabs = 256;       // live tabs, every plugin together
constexpr size_t kMaxOverlays = 256;   // live overlays
constexpr size_t kMaxHotkeys = 512;    // live bindings
constexpr size_t kMaxReserved = 256;   // chords the product reserves

// Publishes "sco.ui" 1.0 under the host's id and installs the release hook. Any thread. BadArg:
// already started, or the name already published. TooMany: no release hook slot, out of memory.
// Nothing changes unless Ok.
Result Start();

// Withdraws "sco.ui" and clears every tab, overlay, binding, reservation and error message;
// later calls through the table answer SCO_UNAVAILABLE. No-op unless started. Call after every
// plugin has unloaded (sco::app::Stop does). Any thread.
void Stop();

bool Started();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_ui_v1* Table();

// ---- chords -----------------------------------------------------------------------------------

// The chord grammar of sco_ui.h. True and out = the normalized chord ("ctrl+alt+escape"); false
// (out untouched) for anything else. Any thread.
bool NormalizeChord(const char* text, std::string& out);

// Reserves a chord for the product: no plugin can bind it (bind_hotkey answers SCO_BAD_ARG,
// "f6 is reserved by the host"), and Dispatch answers NotFound for it, so the product handles it
// itself. Reserve before plugins load. Ok also when already reserved. BadArg: not a chord, or a
// plugin holds it now (unbind it first). TooMany: kMaxReserved reached. Needs Start. Any thread.
Result ReserveChord(const char* chord);

// ---- what the product reads (any thread; snapshots) --------------------------------------------

struct TabInfo {
    std::string id;      // "<plugin id>.<name>"
    std::string title;
    std::string badge;   // "" when none
    std::string owner;   // the plugin id
    int32_t     order = 0;
};
// Every live tab, by ascending order, ties in registration order.
std::vector<TabInfo> Tabs();

struct OverlayInfo {
    std::string id;
    std::string owner;
};
// Every live overlay, in registration order.
std::vector<OverlayInfo> Overlays();

// The badge of tab id; "" when it has none or there is no such tab.
std::string Badge(const char* tabId);

struct HotkeyInfo {
    std::string chord;     // normalized
    std::string command;
    std::string owner;     // the plugin id that bound it
    uint32_t    nargs = 0;
};
// Every live binding, sorted by chord.
std::vector<HotkeyInfo> Hotkeys();

// The product's reserved chords, sorted.
std::vector<std::string> ReservedChords();

// ---- drawing and dispatch ------------------------------------------------------------------------

// Calls tab id's draw(frame, ctx) now, as a callout of its owner. frame: the product's frame
// context (sc-offline: its ImGui context), passed through untouched. Game thread only.
// Ok: it ran. NotFound: no such tab (or the service isn't started). WrongThread: not the game
// thread. Crashed: the callout did not complete (the guard caught a fault, or the owner may not
// be called any more); with ContainCallouts the plugin is disabled and its UI withdrawn.
// Nothing is locked while draw runs: it may register, unregister (itself included) and badge.
Result DrawTab(const char* id, void* frame);

// Calls every live overlay's draw(frame, ctx), in registration order, each as a callout of its
// owner; one that faults doesn't stop the rest. An overlay registered during the pass draws
// from the next one; one removed during the pass is not called. Returns the number that ran to
// completion. Game thread only (0 otherwise).
size_t DrawOverlays(void* frame);

// Runs the binding of chord (any form NormalizeChord accepts): sco::Invoke(command, its stored
// args) with the binding's owner as the caller, so the command registry checks the command, its
// arguments and capability exactly as for invoke. On the game thread the command runs now and
// *reply (optional) gets its reply; from another thread the call is queued (Ok) and the reply is
// dropped. NotFound: no binding (a bad or reserved chord included) or no such command;
// otherwise the command's or Invoke's result.
Result Dispatch(const char* chord, std::string* reply = nullptr);

}  // namespace sco::ui
