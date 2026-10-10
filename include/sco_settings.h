/*
 * sco_settings.h: the host service "sco.settings", version 1.0.
 *
 * A plugin declares typed settings in the [settings] section of its plugin.ini (docs/plugins.md);
 * the host keeps their values, shows them in the plugin's menu page and persists them. The plugin
 * only reads. Plain C; usable from C and C++. Published by the host (a host-owned service under
 * the reserved id "sco"), found with sco_api 1.1 query_service:
 *
 *   const sco_settings_v1* st = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_SETTINGS_NAME, SCO_SETTINGS_VERSION_1_0, (const void**)&st) == SCO_OK) {
 *       int64_t speed = 5;
 *       st->get_int(self, "speed", &speed);          // the declared default until the player changes it
 *   }
 *
 * Reference: docs/api-v1.md (sco.settings), docs/plugins.md (the declarations). Layout pinned by
 * tests/abi_settings.c. The rules of sco_api.h hold here too (4-byte enums, results instead of
 * exceptions, 64-bit only), plus:
 *  - A plugin reads only its own settings: the names are the ones its plugin.ini declares. There
 *    is no way to write through this table; the player changes values in the product's menu, and
 *    the host checks every value against the declaration (type, range, choice) before it keeps
 *    it, so a read always returns a value the declaration allows.
 *  - Each getter fits one declared type: get_bool a bool, get_int an int, get_float a float,
 *    get_string a string or an enum (the choice's name). Another type is SCO_BAD_ARG, and last_error
 *    says which. SCO_NOT_FOUND: this plugin declares no setting by that name (a plugin with no
 *    [settings] included). On any failure *out is untouched.
 *  - Reads are cheap and never touch storage: the host reads the values once, when the plugin is
 *    declared, and keeps them. Any thread.
 *  - Strings use the raw-handler size handshake: *inout_size holds out's capacity on entry; on
 *    return the bytes written, or with SCO_TOO_MANY the bytes needed (out may be NULL with
 *    *inout_size 0 to ask). The text is NUL-terminated and the NUL counts. A string value is at
 *    most SCO_SETTINGS_MAX_STRING bytes.
 *  - The event "settings.changed" (SCO_SETTINGS_CHANGED_EVENT) is dispatched on the game thread
 *    once per change, after the new value is stored. The event data is a const
 *    sco_settings_changed*, valid during the callback: the plugin id and the setting name. A
 *    plugin that gets it reads the new value with the getters. Setting a value to what it already
 *    is dispatches nothing. A plugin hears about its own settings and every other plugin's.
 *  - Values persist in the plugin's own sco.storage key-value namespace (keys starting with
 *    "sco.settings." belong to the host) when the host runs storage, and live in memory only
 *    otherwise. A kept value that no longer fits the declaration (the plugin changed the type,
 *    the range or the choices) is dropped at the next start: the default applies, and mod.log
 *    says so.
 *  - When the plugin unloads or crashes its settings are withdrawn; every later call naming that
 *    self is SCO_BAD_ARG. The table stays valid for the life of the host; once the host has
 *    stopped the service, calls answer SCO_UNAVAILABLE.
 *
 * License: GPL-3.0, like the rest of sco-core.
 */
#ifndef SCO_SETTINGS_H
#define SCO_SETTINGS_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_SETTINGS_NAME "sco.settings"
#define SCO_SETTINGS_VERSION_1_0 0x00010000u

#define SCO_SETTINGS_MAX_NAME   31u    /* a setting's name, NUL excluded */
#define SCO_SETTINGS_MAX_STRING 255u   /* a string value, NUL excluded */

#define SCO_SETTINGS_CHANGED_EVENT "settings.changed"

/* The data of a "settings.changed" event. Strings are the host's, valid during the callback. */
typedef struct sco_settings_changed {
    uint32_t    size;     /* sizeof(sco_settings_changed) as the host built it */
    uint32_t    _pad;
    const char* plugin;   /* the plugin whose setting changed */
    const char* name;     /* the setting */
} sco_settings_changed;

typedef struct sco_settings_v1 {
    uint32_t size; /* sizeof(sco_settings_v1) as the host built it */
    uint32_t _pad;

    /* *out = 0 or 1. SCO_BAD_ARG: not a bool setting, a NULL name or out, or a bad self. */
    sco_result (*get_bool)(sco_plugin* self, const char* name, int32_t* out);
    /* SCO_BAD_ARG: not an int setting (a float or enum included). */
    sco_result (*get_int)(sco_plugin* self, const char* name, int64_t* out);
    /* SCO_BAD_ARG: not a float setting. */
    sco_result (*get_float)(sco_plugin* self, const char* name, double* out);
    /* A string setting's text, or an enum setting's choice, with the size handshake. */
    sco_result (*get_string)(sco_plugin* self, const char* name, char* out, uint32_t* inout_size);

    /* The message of this plugin's last failed call ("" if none), NUL-terminated, with the size
     * handshake. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sco_settings_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_SETTINGS_H */
