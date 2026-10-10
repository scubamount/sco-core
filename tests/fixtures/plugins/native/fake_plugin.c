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
#define MODE_PROVIDER     11  /* loads, then provides the service FAKE_ID ".svc"; its boom() faults */
#define MODE_CALLER       12  /* loads, then registers commands that call svc_provider's service */

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

/* The provider's service table (tests/test_plugins.cpp reads the same layout). */
typedef struct fake_service {
    uint32_t size;
    int (*ok)(void);     /* returns 7 */
    int (*boom)(void);   /* faults inside the provider's module */
} fake_service;

#if FAKE_MODE == MODE_PROVIDER
static int SvcOk(void) { return 7; }
static int SvcBoom(void) { Fault(); return 1; }
static const fake_service g_svc = { sizeof(fake_service), SvcOk, SvcBoom };
#endif

#if FAKE_MODE == MODE_CALLER
static const fake_service* g_cached;   /* the table kept from the last query, as a caching plugin would */

static sco_result Fetch(void) {
    const void* t = NULL;
    const sco_result r = g_api->query_service("svc_provider.svc", 1u << 16, &t);
    if (r != SCO_OK) return r;
    g_cached = (const fake_service*)t;
    return SCO_OK;
}
/* ok: query and call ok(). boom: query and call boom() (faults inside the provider). cached: call
 * boom() through the kept table, even after the provider is gone. self: fault in this module. */
static sco_result CmdOk(const sco_arg* a, uint32_t n, void* c, char* reply, uint32_t size) {
    (void)a; (void)n; (void)c; (void)reply; (void)size;
    const sco_result r = Fetch();
    if (r != SCO_OK) return r;
    return g_cached->ok() == 7 ? SCO_OK : SCO_UNAVAILABLE;
}
static sco_result CmdBoom(const sco_arg* a, uint32_t n, void* c, char* reply, uint32_t size) {
    (void)a; (void)n; (void)c; (void)reply; (void)size;
    const sco_result r = Fetch();
    if (r != SCO_OK) return r;
    g_cached->boom();
    return SCO_OK;
}
static sco_result CmdCached(const sco_arg* a, uint32_t n, void* c, char* reply, uint32_t size) {
    (void)a; (void)n; (void)c; (void)reply; (void)size;
    if (!g_cached) return SCO_NOT_FOUND;
    g_cached->boom();
    return SCO_OK;
}
static sco_result CmdSelf(const sco_arg* a, uint32_t n, void* c, char* reply, uint32_t size) {
    (void)a; (void)n; (void)c; (void)reply; (void)size;
    Fault();
    return SCO_OK;
}
static sco_result Register(const char* name, sco_command_fn fn) {
    sco_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.size = sizeof(cmd);
    cmd.name = name;
    cmd.title = name;
    cmd.fn = fn;
    return g_api->register_command(g_self, &cmd);
}
#endif

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
#if FAKE_MODE == MODE_PROVIDER
    r = api->provide_service(self, FAKE_ID ".svc", 1u << 16, &g_svc);
    if (r != SCO_OK) return r;
#elif FAKE_MODE == MODE_CALLER
    if ((r = Register(FAKE_ID ".ok", CmdOk)) != SCO_OK) return r;
    if ((r = Register(FAKE_ID ".boom", CmdBoom)) != SCO_OK) return r;
    if ((r = Register(FAKE_ID ".cached", CmdCached)) != SCO_OK) return r;
    if ((r = Register(FAKE_ID ".self", CmdSelf)) != SCO_OK) return r;
#endif
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
