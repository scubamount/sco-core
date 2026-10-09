# UI: the `sco.ui` service

Plugins put UI in the product's menu and bind keys to commands without touching the product's renderer. A plugin registers a **tab** (a page in the menu) or an **overlay** (drawn over the game every frame) with a draw function, and the product calls that function during its own frame. A plugin binds a **hotkey**, a key chord, to a command, and the product runs the command through the command registry when the key is pressed. sco-core keeps the registry: ids, order, lifetime and the hotkey table. It draws nothing and contains no ImGui and no game code; sc-offline renders what is registered with its ImGui menu.

It is a **host-owned service** (the reserved id `sco`, [API v1 § Host-owned services](api-v1.md#host-owned-services)) named `sco.ui`, version 1.0, found with the ordinary `query_service` of sco_api 1.1. `sco_api.h` is unchanged; the table is declared in [`include/sco_ui.h`](../include/sco_ui.h) (plain C) and pinned by [`tests/abi_ui.c`](../tests/abi_ui.c). The host kit (`sco::app::Start`) always publishes it.

## Contents

- [Using it from C](#using-it-from-c)
- [Using it from C++](#using-it-from-c-1)
- [The table](#the-table)
- [Ids and order](#ids-and-order)
- [Drawing](#drawing)
- [Hotkeys](#hotkeys)
- [Threads](#threads)
- [Unload, crash and shutdown](#unload-crash-and-shutdown)
- [Hosting it](#hosting-it)
- [Lua](#lua)
- [Limits](#limits)

## Using it from C

```c
#include "sco_ui.h"

static int clicks;

/* frame is the product's frame context (sc-offline: its ImGui context), valid only now. */
static void draw_hello(void* frame, void* ctx) {
    (void)frame; (void)ctx;
    /* draw with the product's UI library, through frame */
}

sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    const sco_ui_v1* ui = NULL;
    if (api->size > offsetof(sco_api, query_service) &&
        api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, (const void**)&ui) == SCO_OK) {
        ui->register_tab(self, "hello.main", "Hello", 100, draw_hello, &clicks);
        ui->set_badge(self, "hello.main", "new");
        sco_arg who = { SCO_ARG_STRING };
        who.v.s = "Pilot";
        if (ui->bind_hotkey(self, "ctrl+alt+h", "hello.wave", &who, 1) != SCO_OK) {
            char why[128];
            uint32_t n = sizeof why;
            if (ui->last_error(self, why, &n) == SCO_OK) api->log(self, SCO_LOG_WARN, why);   /* "ctrl+alt+h is bound by ..." */
        }
    }
    return SCO_OK;   /* a host without UI: keep working without it */
}
```

A host service stays valid for as long as any plugin is loaded, so keeping `ui` for the plugin's life is fine. A host that doesn't publish it answers `SCO_NOT_FOUND`.

## Using it from C++

[`scosdk/ui.hpp`](../include/scosdk/ui.hpp) wraps the table ([C++ SDK](sdk-cpp.md#ui)):

```cpp
#include "scosdk/ui.hpp"

struct Panel { void Draw(void* frame) { /* ImGui through frame */ } };
static Panel g_panel;

sco::sdk::Ui ui;
if (ui.Open(*this) == SCO_OK) {
    ui.AddTab("hello.main", "Hello", 100, g_panel);            // g_panel.Draw(frame) each frame the tab shows
    ui.SetBadge("hello.main", "3");
    if (ui.BindHotkey("ctrl+alt+h", "hello.wave", { sco::sdk::MakeArg("Pilot") }) != SCO_OK)
        Warn("hotkey: %s", ui.LastError().c_str());
}
```

An exception thrown by `Draw` is caught in the SDK's thunk and dropped; it never crosses the C ABI.

## The table

| Function | Does |
|---|---|
| `register_tab(self, id, title, order, draw, ctx)` | Adds a tab. `title`: 1-63 bytes. `SCO_BAD_ARG`: a bad or taken id, an id outside the plugin, a bad title, `draw` NULL. `SCO_TOO_MANY`: 256 tabs live |
| `unregister_tab(self, id)` | Removes one of the plugin's tabs and its badge. `SCO_NOT_FOUND` otherwise |
| `set_badge(self, tab_id, text)` | The short text beside the tab title (at most 15 bytes); NULL or `""` clears it. `SCO_NOT_FOUND`: not one of the plugin's tabs |
| `register_overlay(self, id, draw, ctx)` | Adds an overlay; overlays draw in registration order. Refusals as for tabs; 256 live |
| `unregister_overlay(self, id)` | Removes one of the plugin's overlays |
| `bind_hotkey(self, chord, command, args, nargs)` | Binds a chord to a command with up to 16 arguments, copied (strings up to 255 bytes). `SCO_BAD_ARG`: a bad chord, command name or argument, or the chord taken (bound by anyone, or reserved by the product). `SCO_TOO_MANY`: 512 bindings live |
| `unbind_hotkey(self, chord)` | Removes the plugin's binding on chord (any accepted spelling). `SCO_NOT_FOUND` otherwise |
| `normalize_chord(chord, out, inout_size)` | The normalized chord, with the raw-handler size handshake. `SCO_BAD_ARG`: not a chord |
| `last_error(self, out, inout_size)` | The message of the plugin's last refused call (`""` if none), with the size handshake |

Every function that takes `self` answers `SCO_BAD_ARG` for a handle the host didn't make or one already released, and `SCO_UNAVAILABLE` once the host has stopped the service. Strings are copied during the call; only `draw` and `ctx` are borrowed.

## Ids and order

- Ids are `<plugin id>.<name>`: `[a-z0-9_.]`, at most 63 bytes, no leading, trailing or doubled `.`, like command names. A plugin can only use ids under its own id, so two plugins never collide, and the id says whose tab it is. Tabs and overlays have separate id spaces (`hello.main` can name both).
- Tabs are shown by ascending `order`, ties in registration order (`sco::ui::Tabs()` returns them sorted). The product places its own tabs around them; sc-offline's are its built-in features.
- Overlays draw in registration order.

## Drawing

- `draw(frame, ctx)` runs on the **game thread**, during the product's frame, when the product draws that tab (usually: the menu is open on it) or every frame for an overlay.
- `frame` is the product's frame context, passed through untouched: sc-offline passes its ImGui context. It is valid only during the call. sco-core never looks at it; what a plugin may do with it is the product's contract (sc-offline: ImGui calls inside the tab's area, with the ImGui version sc-offline ships).
- A draw runs as a **callout of its plugin**, under the same crash guard as its commands, events and tasks (`sco::plugins::ContainCallouts`). A fault in it disables that plugin and releases it, which withdraws all of its UI; every other plugin keeps drawing. The crash reason names the tab or overlay: `crashed in hello.main (0xC0000005)`.
- Nothing is locked while a draw runs: it may register, unregister (itself included) and set badges. An overlay removed during a pass is not called; one added during a pass draws from the next one.
- **Freeing `ctx`** after `unregister_tab` / `unregister_overlay`: on the game thread, as soon as the call returns (also from inside the draw itself). From another thread the game thread may be inside `draw` right now: call `run_on_game_thread` after unregister returns and free `ctx` in that task, as for `unsubscribe`.

## Hotkeys

A hotkey is a binding from a key chord to a command, kept by sco-core and dispatched by the product. Binding to a command, not to a callback, means the key does exactly what the menu's button does: the command registry checks the arguments and the capability, the command runs under its own plugin's crash guard, and a reply comes back.

**Chords** are modifiers and one key joined by `+`, case-insensitive, with spaces allowed around `+`:

| | Accepted |
|---|---|
| Modifiers | `ctrl` (`control`), `alt`, `shift` |
| Keys | `a`-`z`, `0`-`9`, `f1`-`f24`, `num0`-`num9` (`numpad0`-`numpad9`), `escape` (`esc`), `enter` (`return`), `tab`, `space`, `backspace`, `insert` (`ins`), `delete` (`del`), `home`, `end`, `pageup` (`pgup`), `pagedown` (`pgdn`), `up`, `down`, `left`, `right` |
| Punctuation, by name or character | `minus` (`-`), `equals` (`=`), `comma` (`,`), `period` (`.`), `slash` (`/`), `backslash` (`\`), `semicolon` (`;`), `apostrophe` (`'`), `grave` (`` ` ``), `lbracket` (`[`), `rbracket` (`]`) |

The host normalizes a chord to lower case, the modifiers in the order `ctrl`, `alt`, `shift`, then the key's first name: `Alt + Ctrl + Esc` is `ctrl+alt+escape`, `numpad5` is `num5`, `ctrl+-` is `ctrl+minus`. Refused: an empty part (`ctrl+`, `ctrl++a`), a modifier twice, two keys, modifiers alone, an unknown name (`win+a`, `page up`, `f25`).

**Conflicts.** One chord has one binding. Binding a chord already bound is `SCO_BAD_ARG`, whoever holds it, and `last_error` names the holder: `ctrl+alt+4 is bound by 'tester' to tester.go`. Plugins bind free keys; the first to bind a chord keeps it until it unbinds or unloads. (In game, sc-offline's F6 build mode collided with a test plugin's key: that is what this table prevents.)

**Reserved chords.** The product declares the keys it handles itself (`sco::app::Platform::reservedChords`, or `sco::ui::ReserveChord`) before plugins load. Binding one is `SCO_BAD_ARG` with `f6 is reserved by the host`, and the product never dispatches it.

**Dispatch.** When the product sees a chord it doesn't handle itself, it calls `sco::ui::Dispatch(chord)`: sco-core invokes the bound command with the stored arguments, with the binding's plugin as the caller, as `invoke` would. A command that doesn't exist (yet) or arguments that don't match its definition are answered by the registry at that moment (`NOT_FOUND`, `BAD_ARG`), not at bind time, so a plugin can bind a key to a command another plugin registers later.

## Threads

- Registration, badges, hotkeys, `normalize_chord` and `last_error`: any thread. Calls are serialized by one lock; it is never held while plugin code runs.
- Draws: the game thread only (`DrawTab` answers `WrongThread` elsewhere), because crash containment releases a faulting plugin, and `Release` runs on the game thread. A product whose renderer runs on another thread draws plugin UI from the game thread's side of its frame.
- `Dispatch`: any thread. On the game thread the command runs now and the reply comes back; from another thread the invoke is queued for the next tick (`Ok`) and the reply is dropped.

## Unload, crash and shutdown

- When a plugin unloads, fails its load or crashes, the host releases it, and a release hook removes everything it registered: tabs, badges, overlays, hotkeys and its last error. Every later call naming its handle is `SCO_BAD_ARG`. Its chords are free for others.
- `sco::app::Stop` stops the service after every plugin has unloaded: `sco.ui` is withdrawn, the reservations cleared, and the table answers `SCO_UNAVAILABLE`.

## Hosting it

The product side is [`sco/ui.h`](../include/sco/ui.h) (library `sco_ui`, [API: sco/ui.h](api.md#scouih-the-scoui-service)). `sco::app::Start` calls `ui::Start` and reserves `Platform::reservedChords`; another host calls them itself. A sketch of sc-offline's menu:

```cpp
static const char* const kReserved[] = { "f6", "f7", "f8" };   // build mode, teleport save / load
pf.reservedChords = kReserved;
pf.nReservedChords = std::size(kReserved);
sco::app::Start(pf);

// Each frame, on the game thread, inside the menu window:
for (const sco::ui::TabInfo& t : sco::ui::Tabs()) {          // sorted by order
    std::string label = t.badge.empty() ? t.title : t.title + " (" + t.badge + ")";
    if (ImGui::BeginTabItem((label + "###" + t.id).c_str())) {
        sco::ui::DrawTab(t.id.c_str(), ImGui::GetCurrentContext());
        ImGui::EndTabItem();
    }
}
sco::ui::DrawOverlays(ImGui::GetCurrentContext());           // menu open or not

// A key the product doesn't handle itself (and ImGui isn't typing into):
std::string chord = "ctrl+alt+4";                             // built from the key event
std::string reply;
if (sco::ui::Dispatch(chord.c_str(), &reply) == sco::Result::Ok && !reply.empty()) sco::Status("%s", reply.c_str());
```

| Function | Does |
|---|---|
| `Start()` / `Stop()` / `Started()` / `Table()` | Publish and withdraw `sco.ui` 1.0 (with the release hook); `Stop` clears everything |
| `NormalizeChord(text, out)` | The chord grammar above, for the product's own key events |
| `ReserveChord(chord)` | Reserves a chord for the product. `BadArg`: not a chord, or a plugin holds it |
| `Tabs()`, `Overlays()`, `Badge(id)`, `Hotkeys()`, `ReservedChords()` | Snapshots, any thread: tabs by order, overlays by registration, hotkeys by chord |
| `DrawTab(id, frame)` | Runs the tab's draw as a callout of its plugin. `NotFound`, `WrongThread`, `Crashed` |
| `DrawOverlays(frame)` | Runs every overlay; returns how many completed |
| `Dispatch(chord, reply)` | Invokes the bound command through the registry. `NotFound` for no binding or a reserved chord |

The draw context is the product's choice: sco-core passes `frame` through as `void*`. ImGui never crosses the ABI as a type; a plugin that draws with ImGui links the same ImGui version as the product, which is the product's contract to publish.

## Lua

Scripts bind hotkeys; tabs and overlays need draw callbacks from Lua, which wait for the Lua UI bindings (G018).

| Function | Returns | Notes |
|---|---|---|
| `sco.bind_hotkey(chord, command, ...)` | `true`, or `false, why, message` | The extra values are the command's arguments, typed by its definition when it is registered, otherwise by their Lua types (integer, float, string, boolean). `message` is the host's (`"f6 is reserved by the host"`) |
| `sco.unbind_hotkey(chord)` | `true`, or `false, why, message` | |

## Limits

| Limit | Value |
|---|---|
| Live tabs, overlays | 256 each, every plugin together |
| Live hotkeys | 512 |
| Reserved chords | 256 |
| Id | 63 bytes |
| Title | 63 bytes |
| Badge | 15 bytes |
| Chord | 31 bytes normalized (63 as given) |
| Hotkey arguments | 16, strings up to 255 bytes |
