/*
 * sco_api.h: the sco plugin ABI, version 1.0-pre.
 *
 * The only header a plugin includes. Plain C; usable from C and C++.
 * Reference: docs/api-v1.md. Layout pinned by tests/abi_v1.c.
 *
 * Status: pre-release. Until the sdk-v1.0.0 tag this can still change;
 * after it, version 1 only grows at the end of structs (see the rules below).
 *
 * Rules that keep the ABI stable:
 *  - No C++ types, no exceptions, no varargs across the boundary.
 *  - sco_api, sco_command and sco_plugin_info start with uint32_t size. The
 *    host only reads fields the plugin's size covers; the plugin only calls
 *    functions the host's size covers. sco_arg_def arrays are read with the
 *    stride in sco_command.arg_def_size, so sco_arg_def can grow too. sco_arg
 *    is frozen for major 1 (its 8-byte union holds every argument type).
 *    New fields and functions go at the end. Nothing is removed or reordered
 *    within major version 1.
 *  - Every enum is 4 bytes (each has a _FORCE32 member; never pass it).
 *  - Pointers passed to a plugin are valid only for the duration of the call
 *    unless stated otherwise. Strings are UTF-8 and NUL-terminated.
 *  - Calls return sco_result. Nothing throws; a fault in host code reached
 *    from a call comes back as SCO_CRASHED.
 *  - 64-bit only (x64 Windows is the target; the host tests build on
 *    64-bit macOS and Linux with the same layout).
 *
 * License: GPL-3.0, like the rest of sco-core. Plugins built against this
 * header are GPL-3.0; there is no linking exception.
 */
#ifndef SCO_API_H
#define SCO_API_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_API_MAJOR 1
#define SCO_API_MINOR 0

#if defined(_WIN32)
#define SCO_EXPORT __declspec(dllexport)
#elif defined(__GNUC__)
#define SCO_EXPORT __attribute__((visibility("default")))
#else
#define SCO_EXPORT
#endif

/* ---- results and levels ------------------------------------------------ */

typedef enum sco_result {
    SCO_OK           = 0,
    SCO_UNAVAILABLE  = 1, /* capability missing on this game build */
    SCO_NOT_FOUND    = 2, /* unknown command or subscription */
    SCO_BAD_ARG      = 3, /* NULL, bad or duplicate name, wrong argument count or type */
    SCO_CRASHED      = 4, /* the call faulted inside game or plugin code */
    SCO_WRONG_THREAD = 5, /* called from a thread the function forbids */
    SCO_TOO_MANY     = 6, /* queue or table full, or out of memory */
    SCO_RESULT_FORCE32 = 0x7fffffff
} sco_result;

typedef enum sco_log_level {
    SCO_LOG_INFO  = 0,
    SCO_LOG_WARN  = 1,
    SCO_LOG_ERROR = 2,
    SCO_LOG_FORCE32 = 0x7fffffff
} sco_log_level;

/* ---- handles and callbacks --------------------------------------------- */

/* Opaque: the host's handle for one loaded plugin. */
typedef struct sco_plugin sco_plugin;

typedef void (*sco_task_fn)(void* ctx);
typedef void (*sco_event_fn)(const char* event, const void* data, void* ctx);

/* ---- commands ---------------------------------------------------------- */

typedef enum sco_arg_type {
    SCO_ARG_INT    = 0,
    SCO_ARG_FLOAT  = 1,
    SCO_ARG_STRING = 2,
    SCO_ARG_BOOL   = 3,
    SCO_ARG_FORCE32 = 0x7fffffff
} sco_arg_type;

/* One argument value. type is a sco_arg_type; read the matching union member
 * (SCO_ARG_BOOL uses i: 0 or 1, anything else is SCO_BAD_ARG). Frozen for
 * major 1: arrays of sco_arg have a fixed 16-byte stride. */
typedef struct sco_arg {
    uint32_t type;
    uint32_t _pad;
    union {
        int64_t     i;
        double      f;
        const char* s;
    } v;
} sco_arg;

/* Describes one argument of a command, for the menu and for type checks.
 * Read with the stride in sco_command.arg_def_size; later minors may add
 * fields at the end. */
typedef struct sco_arg_def {
    const char* name;
    uint32_t    type; /* sco_arg_type */
    uint32_t    _pad;
    const char* help;
} sco_arg_def;

/* Runs on the game thread. Writes a short human reply ("Spawned Cutlass
 * Black") into reply, at most reply_size bytes including the NUL. */
typedef sco_result (*sco_command_fn)(const sco_arg* args, uint32_t nargs, void* ctx,
                                     char* reply, uint32_t reply_size);

/* Called on the game thread with the result of an invoke(). reply is valid
 * only during the call. See invoke() for when it is called. */
typedef void (*sco_invoke_done)(sco_result r, const char* reply, void* ctx);

typedef struct sco_command {
    uint32_t           size;       /* sizeof(sco_command) as the caller built it */
    uint32_t           _pad0;
    const char*        name;       /* "<owner>.<action>": "spawn.ship", "hello.wave" */
    const char*        title;      /* menu label: "Spawn ship" */
    const char*        help;
    const char*        capability; /* has() gate; NULL = always available */
    const sco_arg_def* args;
    uint32_t           nargs;
    uint32_t           arg_def_size; /* sizeof(sco_arg_def) as the caller built it */
    sco_command_fn     fn;
    void*              ctx;
} sco_command;

/* ---- the host's function table ----------------------------------------- */

typedef struct sco_api {
    uint32_t size; /* sizeof(sco_api) as the host built it */
    uint16_t major, minor;

    /* "sc-offline 0.8.0". Static string. */
    const char* (*host_version)(void);
    /* 1 if the capability is available on this game build, else 0. */
    int (*has)(const char* capability);

    /* Any thread. Runs fn(ctx) on the game thread, in order, on the next
     * tick. SCO_TOO_MANY when the queue (256) is full. */
    sco_result (*run_on_game_thread)(sco_plugin* self, sco_task_fn fn, void* ctx);
    /* Any thread. Applies from the next dispatch. */
    sco_result (*subscribe)(sco_plugin* self, const char* event, sco_event_fn fn, void* ctx);
    /* Any thread. Applies at once: a running dispatch won't call fn again.
     * Freeing ctx: on the game thread, right after unsubscribe returns (also
     * from inside fn itself). From another thread fn may be running now; call
     * run_on_game_thread after unsubscribe returns and free ctx in that task.
     * Tasks never run while a dispatch is in progress. */
    sco_result (*unsubscribe)(sco_plugin* self, const char* event, sco_event_fn fn);

    /* Any thread. Shown as "<plugin>: message" / logged as "[<plugin>] message". */
    void (*status)(sco_plugin* self, const char* message);
    void (*log)(sco_plugin* self, sco_log_level level, const char* message);

    /* Commands: the one way to use a feature. The host copies cmd, its
     * strings and its arg defs; only fn and ctx must stay valid until the
     * plugin unloads. */
    sco_result (*register_command)(sco_plugin* self, const sco_command* cmd);
    /* From the game thread: runs now, calls done (if set) once before
     * returning, and returns the same result.
     * From another thread: copies name and args, queues the call and returns
     * SCO_OK; done then runs exactly once, on the game thread. Any other
     * return (SCO_BAD_ARG, or SCO_TOO_MANY when the queue is full or memory
     * runs out) means nothing was queued and done is never called. A call
     * still queued when the plugin unloads is dropped without calling done. */
    sco_result (*invoke)(sco_plugin* self, const char* name, const sco_arg* args,
                         uint32_t nargs, sco_invoke_done done, void* ctx);
    /* Writes up to max command pointers to out; returns the number of live
     * commands. The pointers stay valid until their owning plugin unloads. */
    uint32_t (*list_commands)(const sco_command** out, uint32_t max);
} sco_api;

/* ---- what a plugin exports --------------------------------------------- */

typedef struct sco_plugin_info {
    uint32_t    size;      /* sizeof(sco_plugin_info) as the plugin built it */
    uint16_t    api_major; /* SCO_API_MAJOR the plugin was built against */
    uint16_t    api_minor; /* SCO_API_MINOR the plugin was built against */
    const char* name;      /* "hello"; also the command-name prefix */
    const char* version;   /* "1.0.0" */
    const char* author;
} sco_plugin_info;

/* Exported by every plugin DLL. The returned pointer must stay valid until
 * the DLL unloads (use a static). */
SCO_EXPORT const sco_plugin_info* sco_plugin_query(void);
/* api and self stay valid until sco_plugin_unload. Return SCO_OK to stay
 * loaded; anything else unloads the plugin. On unload, failed load or a
 * crash the host removes everything the plugin registered: subscriptions,
 * commands, queued tasks and queued invokes. */
SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self);
/* The game is shutting down. */
SCO_EXPORT void sco_plugin_unload(void);

/* Pointer types for the three exports, for the host's loader. */
typedef const sco_plugin_info* (*sco_plugin_query_fn)(void);
typedef sco_result (*sco_plugin_load_fn)(const sco_api* api, sco_plugin* self);
typedef void (*sco_plugin_unload_fn)(void);

#ifdef __cplusplus
}
#endif

#endif /* SCO_API_H */
