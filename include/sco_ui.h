/*
 * sco_ui.h: the host service "sco.ui", version 1.0.
 *
 * Plugins contribute UI to the product's menu and bind keys to commands, without ever touching
 * the product's renderer. Plain C; usable from C and C++. Published by the host (a host-owned
 * service under the reserved id "sco"), found with sco_api 1.1 query_service:
 *
 *   const sco_ui_v1* ui = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_UI_NAME, SCO_UI_VERSION_1_0, (const void**)&ui) == SCO_OK) {
 *       ui->register_tab(self, "hello.main", "Hello", 100, draw_hello, NULL);
 *       ui->bind_hotkey(self, "ctrl+alt+h", "hello.wave", NULL, 0);
 *   }
 *
 * The split: sco-core keeps the registry (ids, order, lifetime, the hotkey table) and draws
 * nothing; the product (sc-offline: its ImGui menu) lists what is registered and, during its own
 * frame, calls each tab's or overlay's draw function. A plugin never hooks a renderer.
 *
 * Reference: docs/ui.md. Layout pinned by tests/abi_ui.c. The rules of sco_api.h hold here too
 * (4-byte enums, results instead of exceptions, 64-bit only), plus:
 *  - Ids are "<plugin id>.<name>" ([a-z0-9_.], at most 63 bytes, no leading, trailing or doubled
 *    '.'), like command names; an id outside the caller's own id is SCO_BAD_ARG. Tabs and
 *    overlays have separate id spaces.
 *  - Strings are copied during the call; only draw and ctx are borrowed, until they are
 *    unregistered or the plugin unloads.
 *  - Registration, badges and hotkeys: any thread. Draw callbacks run on the game thread, during
 *    the product's frame; the frame pointer is the product's (sc-offline: its ImGui context) and
 *    is valid only during the call. A draw callback runs as a callout of the plugin, under the
 *    same crash guard as its commands and events: a fault disables the plugin.
 *  - Freeing ctx after unregister_tab / unregister_overlay: on the game thread, as soon as the
 *    call returns (also from inside the draw callback itself). From another thread the game
 *    thread may be inside draw right now: call run_on_game_thread after unregister returns and
 *    free ctx in that task, as for unsubscribe.
 *  - When the plugin unloads or crashes, everything it registered (tabs, overlays, badges,
 *    hotkeys) is withdrawn; every later call naming that self is SCO_BAD_ARG. The table stays
 *    valid for the life of the host; once the host has stopped the service, calls answer
 *    SCO_UNAVAILABLE.
 *  - A refused call leaves a message for last_error, such as
 *    "ctrl+alt+4 is bound by 'tester' to tester.go".
 *
 * Chords: modifiers and one key joined by '+', case-insensitive, spaces around '+' allowed:
 * "f6", "ctrl+alt+4", "Shift + F1". Modifiers: ctrl (control), alt, shift. Keys: a-z, 0-9,
 * f1-f24, num0-num9, escape (esc), enter (return), tab, space, backspace, insert (ins),
 * delete (del), home, end, pageup (pgup), pagedown (pgdn), up, down, left, right, and by name
 * or character: minus (-), equals (=), comma (,), period (.), slash (/), backslash (\),
 * semicolon (;), apostrophe ('), grave (`), lbracket ([), rbracket (]). The host normalizes a
 * chord to lower case, modifiers in the order ctrl, alt, shift, then the key's first name
 * above: "Alt + Ctrl + Esc" is "ctrl+alt+escape". One chord has one binding; the product
 * reserves its own keys.
 *
 * License: GPL-3.0, like the rest of sco-core.
 */
#ifndef SCO_UI_H
#define SCO_UI_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_UI_NAME "sco.ui"
#define SCO_UI_VERSION_1_0 0x00010000u

#define SCO_UI_MAX_ID          63u   /* tab and overlay ids, NUL excluded */
#define SCO_UI_MAX_TITLE       63u   /* tab titles */
#define SCO_UI_MAX_BADGE       15u   /* badge text */
#define SCO_UI_MAX_CHORD       31u   /* chord text, given or normalized */
#define SCO_UI_MAX_HOTKEY_ARGS 16u   /* arguments stored with a binding */
#define SCO_UI_MAX_ARG_STRING  255u  /* one string argument stored with a binding */

/* Draws a tab's body or an overlay. frame: the product's frame context (sc-offline: its ImGui
 * context), valid only during the call. Game thread. */
typedef void (*sco_ui_draw_fn)(void* frame, void* ctx);

typedef struct sco_ui_v1 {
    uint32_t size; /* sizeof(sco_ui_v1) as the host built it */
    uint32_t _pad;

    /* ---- tabs: a page in the product's menu ------------------------------------------------ */

    /* Adds tab id with its title (1-63 bytes) at order: the product shows tabs by ascending
     * order, ties in registration order. SCO_BAD_ARG: a bad or taken id, an id outside this
     * plugin, a bad title, draw NULL. SCO_TOO_MANY: the host's tab table is full. */
    sco_result (*register_tab)(sco_plugin* self, const char* id, const char* title, int32_t order,
                               sco_ui_draw_fn draw, void* ctx);
    /* Removes one of this plugin's tabs, and its badge. SCO_NOT_FOUND: it has none by that id. */
    sco_result (*unregister_tab)(sco_plugin* self, const char* id);
    /* Sets the short text the product shows beside the tab title ("3", "new"; at most 15
     * bytes); NULL or "" clears it. SCO_NOT_FOUND: this plugin has no tab by that id. */
    sco_result (*set_badge)(sco_plugin* self, const char* tab_id, const char* text);

    /* ---- overlays: drawn over the game every frame, menu open or not ------------------------ */

    /* Adds overlay id; overlays draw in registration order. SCO_BAD_ARG / SCO_TOO_MANY as for
     * register_tab. */
    sco_result (*register_overlay)(sco_plugin* self, const char* id, sco_ui_draw_fn draw, void* ctx);
    /* SCO_NOT_FOUND: this plugin has no overlay by that id. */
    sco_result (*unregister_overlay)(sco_plugin* self, const char* id);

    /* ---- hotkeys: a chord runs a command -------------------------------------------------- */

    /* Binds chord to the command name ("<x>.<y>"; any plugin's or a host feature's) with nargs
     * arguments (args may be NULL with 0), which are copied, strings included. When the product
     * sees the chord it invokes the command through the command registry, as invoke would, with
     * this plugin as the caller; the command needn't exist yet. SCO_BAD_ARG: a bad chord or
     * command name, a bad argument, or the chord taken: bound already (by anyone, this plugin
     * included) or reserved by the product; last_error names the owner. SCO_TOO_MANY: the
     * host's hotkey table is full. */
    sco_result (*bind_hotkey)(sco_plugin* self, const char* chord, const char* command,
                              const sco_arg* args, uint32_t nargs);
    /* SCO_NOT_FOUND: this plugin has no binding on that chord (a bad chord included). */
    sco_result (*unbind_hotkey)(sco_plugin* self, const char* chord);
    /* Writes chord's normalized form, NUL-terminated, with the raw-handler size handshake
     * (*inout_size: capacity in; bytes written, or needed with SCO_TOO_MANY, out). SCO_BAD_ARG:
     * not a chord. No plugin handle needed; any thread. */
    sco_result (*normalize_chord)(const char* chord, char* out, uint32_t* inout_size);

    /* The message of this plugin's last refused call ("" if none), NUL-terminated, with the
     * size handshake. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sco_ui_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_UI_H */
