/*
 * abi_ui.c: pins the layout of sco_ui.h (the host service "sco.ui", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sco_ui_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_ui rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_ui.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SCO_UI_VERSION_1_0 == 0x00010000u);
PIN(sizeof(SCO_UI_NAME) == 7); /* "sco.ui" */
PIN(SCO_UI_MAX_ID == 63u);
PIN(SCO_UI_MAX_TITLE == 63u);
PIN(SCO_UI_MAX_BADGE == 15u);
PIN(SCO_UI_MAX_CHORD == 31u);
PIN(SCO_UI_MAX_HOTKEY_ARGS == 16u);
PIN(SCO_UI_MAX_ARG_STRING == 255u);

/* ---- sco_ui_v1 ---- */
SIZE(sco_ui_v1, 80);
AT(sco_ui_v1, size, 0);
AT(sco_ui_v1, _pad, 4);
AT(sco_ui_v1, register_tab, 8);
AT(sco_ui_v1, unregister_tab, 16);
AT(sco_ui_v1, set_badge, 24);
AT(sco_ui_v1, register_overlay, 32);
AT(sco_ui_v1, unregister_overlay, 40);
AT(sco_ui_v1, bind_hotkey, 48);
AT(sco_ui_v1, unbind_hotkey, 56);
AT(sco_ui_v1, normalize_chord, 64);
AT(sco_ui_v1, last_error, 72);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_ui_signatures(const sco_ui_v1* u) {
    sco_ui_draw_fn draw = (void (*)(void*, void*))0;
    sco_result (*register_tab)(sco_plugin*, const char*, const char*, int32_t, sco_ui_draw_fn, void*) = u->register_tab;
    sco_result (*unregister_tab)(sco_plugin*, const char*) = u->unregister_tab;
    sco_result (*set_badge)(sco_plugin*, const char*, const char*) = u->set_badge;
    sco_result (*register_overlay)(sco_plugin*, const char*, sco_ui_draw_fn, void*) = u->register_overlay;
    sco_result (*unregister_overlay)(sco_plugin*, const char*) = u->unregister_overlay;
    sco_result (*bind_hotkey)(sco_plugin*, const char*, const char*, const sco_arg*, uint32_t) = u->bind_hotkey;
    sco_result (*unbind_hotkey)(sco_plugin*, const char*) = u->unbind_hotkey;
    sco_result (*normalize_chord)(const char*, char*, uint32_t*) = u->normalize_chord;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = u->last_error;
    (void)draw; (void)register_tab; (void)unregister_tab; (void)set_badge; (void)register_overlay;
    (void)unregister_overlay; (void)bind_hotkey; (void)unbind_hotkey; (void)normalize_chord;
    (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_ui_pins(void);
void sco_abi_ui_pins(void) { (void)&pin_ui_signatures; }
