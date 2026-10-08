/*
 * abi_v1.c: pins the layout of every sco_api.h v1 type and enum value.
 *
 * Compile-only: if anything here fails, a v1 declaration changed and every
 * plugin built against the old header would break. After sdk-v1.0.0 this
 * file is append-only: new fields and functions get new lines at the end
 * of their struct's block; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and as C++20.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_api.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

/* v1 is 64-bit only. */
PIN(sizeof(void*) == 8);
PIN(sizeof(void (*)(void)) == 8);

/* ---- versions ---- */
PIN(SCO_API_MAJOR == 1);
PIN(SCO_API_MINOR == 0);

/* ---- sco_result ---- */
SIZE(sco_result, 4);
PIN(SCO_OK == 0);
PIN(SCO_UNAVAILABLE == 1);
PIN(SCO_NOT_FOUND == 2);
PIN(SCO_BAD_ARG == 3);
PIN(SCO_CRASHED == 4);
PIN(SCO_WRONG_THREAD == 5);
PIN(SCO_TOO_MANY == 6);

/* ---- sco_log_level ---- */
SIZE(sco_log_level, 4);
PIN(SCO_LOG_INFO == 0);
PIN(SCO_LOG_WARN == 1);
PIN(SCO_LOG_ERROR == 2);

/* ---- sco_arg_type ---- */
SIZE(sco_arg_type, 4);
PIN(SCO_ARG_INT == 0);
PIN(SCO_ARG_FLOAT == 1);
PIN(SCO_ARG_STRING == 2);
PIN(SCO_ARG_BOOL == 3);

/* ---- sco_arg ---- */
SIZE(sco_arg, 16);
AT(sco_arg, type, 0);
AT(sco_arg, _pad, 4);
AT(sco_arg, v, 8);
AT(sco_arg, v.i, 8);
AT(sco_arg, v.f, 8);
AT(sco_arg, v.s, 8);

/* ---- sco_arg_def ---- */
SIZE(sco_arg_def, 24);
AT(sco_arg_def, name, 0);
AT(sco_arg_def, type, 8);
AT(sco_arg_def, _pad, 12);
AT(sco_arg_def, help, 16);

/* ---- sco_command ---- */
SIZE(sco_command, 72);
AT(sco_command, size, 0);
AT(sco_command, _pad0, 4);
AT(sco_command, name, 8);
AT(sco_command, title, 16);
AT(sco_command, help, 24);
AT(sco_command, capability, 32);
AT(sco_command, args, 40);
AT(sco_command, nargs, 48);
AT(sco_command, _pad1, 52);
AT(sco_command, fn, 56);
AT(sco_command, ctx, 64);

/* ---- sco_api ---- */
SIZE(sco_api, 88);
AT(sco_api, size, 0);
AT(sco_api, major, 4);
AT(sco_api, minor, 6);
AT(sco_api, host_version, 8);
AT(sco_api, has, 16);
AT(sco_api, run_on_game_thread, 24);
AT(sco_api, subscribe, 32);
AT(sco_api, unsubscribe, 40);
AT(sco_api, status, 48);
AT(sco_api, log, 56);
AT(sco_api, register_command, 64);
AT(sco_api, invoke, 72);
AT(sco_api, list_commands, 80);

/* ---- sco_plugin_info ---- */
SIZE(sco_plugin_info, 32);
AT(sco_plugin_info, size, 0);
AT(sco_plugin_info, api_major, 4);
AT(sco_plugin_info, api_minor, 6);
AT(sco_plugin_info, name, 8);
AT(sco_plugin_info, version, 16);
AT(sco_plugin_info, author, 24);

/* ---- signatures: a changed parameter list fails to convert ---- */
static const sco_plugin_query_fn  pin_query  = sco_plugin_query;
static const sco_plugin_load_fn   pin_load   = sco_plugin_load;
static const sco_plugin_unload_fn pin_unload = sco_plugin_unload;

static void pin_api_signatures(const sco_api* a) {
    const char* (*host_version)(void) = a->host_version;
    int (*has)(const char*) = a->has;
    sco_result (*run)(sco_plugin*, sco_task_fn, void*) = a->run_on_game_thread;
    sco_result (*sub)(sco_plugin*, const char*, sco_event_fn, void*) = a->subscribe;
    sco_result (*unsub)(sco_plugin*, const char*, sco_event_fn) = a->unsubscribe;
    void (*status)(sco_plugin*, const char*) = a->status;
    void (*log)(sco_plugin*, sco_log_level, const char*) = a->log;
    sco_result (*reg)(sco_plugin*, const sco_command*) = a->register_command;
    sco_result (*inv)(sco_plugin*, const char*, const sco_arg*, uint32_t, sco_invoke_done, void*) = a->invoke;
    uint32_t (*list)(const sco_command**, uint32_t) = a->list_commands;
    void (*task)(void*) = (sco_task_fn)0;
    void (*event)(const char*, const void*, void*) = (sco_event_fn)0;
    sco_result (*cmd)(const sco_arg*, uint32_t, void*, char*, uint32_t) = (sco_command_fn)0;
    void (*done)(sco_result, const char*, void*) = (sco_invoke_done)0;
    (void)host_version; (void)has; (void)run; (void)sub; (void)unsub; (void)status;
    (void)log; (void)reg; (void)inv; (void)list; (void)task; (void)event; (void)cmd; (void)done;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_v1_pins(void);
void sco_abi_v1_pins(void) {
    (void)pin_query; (void)pin_load; (void)pin_unload;
    (void)pin_api_signatures;
}
