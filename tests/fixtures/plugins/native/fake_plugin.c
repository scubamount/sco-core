/* Test plugin for tests/test_plugins.cpp. Built once per behavior with -DFAKE_MODE=<n>
 * (tools/test.sh builds the set as host shared libraries; tools/test-win.sh as Windows DLLs).
 * Includes only sco_api.h, like a real plugin. */
#include "sco_api.h"
#include <stddef.h>
#include <string.h>

#define MODE_OK            0  /* loads, subscribes to "tick", logs */
#define MODE_LOAD_FAILS    1  /* subscribes, then returns SCO_UNAVAILABLE from load */
#define MODE_QUERY_NULL    2  /* sco_plugin_query returns NULL */
#define MODE_API_2         3  /* built for api 2.0 */
#define MODE_WRONG_NAME    4  /* info.name differs from the manifest id */
#define MODE_LOAD_CRASHES  5  /* subscribes, then dereferences NULL inside load */
#define MODE_QUERY_CRASHES 6  /* dereferences NULL inside query */
#define MODE_NO_UNLOAD     7  /* sco_plugin_unload not exported */
#define MODE_TICK_CRASHES  8  /* loads; its tick callback dereferences NULL */
#define MODE_SMALL_INFO    9  /* info.size too small */
#define MODE_BAD_NAME_PTR 10  /* info.name points at unmapped memory: faults inside the guard */

#ifndef FAKE_MODE
#error "build with -DFAKE_MODE=<n>"
#endif
#ifndef FAKE_ID
#define FAKE_ID "fake"
#endif

static const sco_api* g_api;
static sco_plugin*    g_self;

/* Exported so the host test can see what the plugin saw. */
SCO_EXPORT int fake_unload_calls = 0;
SCO_EXPORT int fake_ticks = 0;

static void Fault(void) {
    volatile int* volatile p = NULL;
    *p = 1;
}

static void OnTick(const char* event, const void* data, void* ctx) {
    (void)event; (void)data; (void)ctx;
    ++fake_ticks;
#if FAKE_MODE == MODE_TICK_CRASHES
    Fault();
#endif
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) {
    static sco_plugin_info info;
    (void)Fault;   /* used only by the crash modes */
    info.size = sizeof(info);
    info.api_major = SCO_API_MAJOR;
    info.api_minor = SCO_API_MINOR;
    info.name = FAKE_ID;
    info.version = "1.0.0";
    info.author = "tests";
#if FAKE_MODE == MODE_API_2
    info.api_major = 2;
#elif FAKE_MODE == MODE_WRONG_NAME
    info.name = "someone_else";
#elif FAKE_MODE == MODE_QUERY_CRASHES
    Fault();
#elif FAKE_MODE == MODE_SMALL_INFO
    info.size = 8;
#elif FAKE_MODE == MODE_BAD_NAME_PTR
    info.name = (const char*)(uintptr_t)0x10;
#endif
#if FAKE_MODE == MODE_QUERY_NULL
    return NULL;
#else
    return &info;
#endif
}

SCO_EXPORT sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    g_api = api;
    g_self = self;
    sco_result r = api->subscribe(self, "tick", OnTick, NULL);
    if (r != SCO_OK) return r;
    api->log(self, SCO_LOG_INFO, "hello from " FAKE_ID);
#if FAKE_MODE == MODE_LOAD_CRASHES
    Fault();
#endif
#if FAKE_MODE == MODE_LOAD_FAILS
    return SCO_UNAVAILABLE;
#else
    return SCO_OK;
#endif
}

#if FAKE_MODE != MODE_NO_UNLOAD
SCO_EXPORT void sco_plugin_unload(void) {
    ++fake_unload_calls;
    (void)g_api; (void)g_self;
}
#endif
