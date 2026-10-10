/*
 * abi_settings.c: pins the layout of sco_settings.h (the host service "sco.settings", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sco_settings_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_settings rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_settings.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits, event ---- */
PIN(SCO_SETTINGS_VERSION_1_0 == 0x00010000u);
PIN(SCO_SETTINGS_MAX_NAME == 31u);
PIN(SCO_SETTINGS_MAX_STRING == 255u);
PIN(sizeof(SCO_SETTINGS_NAME) == 13); /* "sco.settings" */
PIN(sizeof(SCO_SETTINGS_CHANGED_EVENT) == 17); /* "settings.changed" */

/* ---- sco_settings_changed (the event data) ---- */
SIZE(sco_settings_changed, 24);
AT(sco_settings_changed, size, 0);
AT(sco_settings_changed, _pad, 4);
AT(sco_settings_changed, plugin, 8);
AT(sco_settings_changed, name, 16);

/* ---- sco_settings_v1 ---- */
SIZE(sco_settings_v1, 48);
AT(sco_settings_v1, size, 0);
AT(sco_settings_v1, _pad, 4);
AT(sco_settings_v1, get_bool, 8);
AT(sco_settings_v1, get_int, 16);
AT(sco_settings_v1, get_float, 24);
AT(sco_settings_v1, get_string, 32);
AT(sco_settings_v1, last_error, 40);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_settings_signatures(const sco_settings_v1* s) {
    sco_result (*get_bool)(sco_plugin*, const char*, int32_t*) = s->get_bool;
    sco_result (*get_int)(sco_plugin*, const char*, int64_t*) = s->get_int;
    sco_result (*get_float)(sco_plugin*, const char*, double*) = s->get_float;
    sco_result (*get_string)(sco_plugin*, const char*, char*, uint32_t*) = s->get_string;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = s->last_error;
    (void)get_bool; (void)get_int; (void)get_float; (void)get_string; (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_settings_pins(void);
void sco_abi_settings_pins(void) { (void)&pin_settings_signatures; }
