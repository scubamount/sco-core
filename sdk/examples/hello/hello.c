/*
 * hello: the sco SDK's native example plugin.
 *
 * Shows every part of sco_api.h a first plugin needs:
 *   - status() and log() on load,
 *   - has() to check a capability before offering a feature (greys out instead of failing),
 *   - the game.ready and tick events,
 *   - one command, hello.wave, with a string argument and a reply.
 *
 * Build: see sdk/README.md. GPL-3.0, like sco-core.
 */
#include <stdio.h>
#include <string.h>

#include "sco_api.h"

static const sco_api* api;
static sco_plugin*    me;
static uint32_t       ticks;
static uint32_t       first_ms, last_report_ms;

static const sco_plugin_info info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR,
    "hello",   /* must equal `id` in plugin.ini; also the command prefix */
    "1.0.0",
    "sco SDK example",
};

/* hello.wave <name>: replies "Hello, <name>". Runs on the game thread. */
static sco_result wave(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    (void)ctx;
    /* The host already checked the count and types against the arg defs; check anyway, so the
     * plugin stays correct if it is ever called some other way. */
    if (nargs != 1 || args[0].type != SCO_ARG_STRING || !args[0].v.s) return SCO_BAD_ARG;
    snprintf(reply, reply_size, "Hello, %s", args[0].v.s);
    return SCO_OK;
}

static const sco_arg_def wave_args[] = {
    { "name", SCO_ARG_STRING, 0, "Who to wave at" },
};

static void on_ready(const char* event, const void* data, void* ctx) {
    (void)event; (void)data; (void)ctx;
    /* Capabilities depend on the game build. Ask before using a feature and say why it is off. */
    if (api->has("teleport"))
        api->log(me, SCO_LOG_INFO, "teleport is available on this game build");
    else
        api->log(me, SCO_LOG_WARN, "teleport is not available on this game build");
}

static void on_tick(const char* event, const void* data, void* ctx) {
    (void)event; (void)ctx;
    uint32_t now = data ? *(const uint32_t*)data : 0;
    if (ticks++ == 0) { first_ms = now; last_report_ms = now; }
    /* About once a minute; unsigned subtraction survives the millisecond counter wrapping. */
    if (now - last_report_ms >= 60000) {
        char line[96];
        snprintf(line, sizeof line, "%u ticks in %u s", (unsigned)ticks, (unsigned)((now - first_ms) / 1000));
        api->log(me, SCO_LOG_INFO, line);
        last_report_ms = now;
    }
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &info; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* a, sco_plugin* self) {
    sco_command cmd;
    sco_result r;

    api = a;
    me = self;

    /* register_command copies everything but fn and ctx, so a stack struct is fine. */
    memset(&cmd, 0, sizeof cmd);
    cmd.size         = sizeof cmd;
    cmd.name         = "hello.wave";
    cmd.title        = "Wave";
    cmd.help         = "Says hello on the status line";
    cmd.capability   = NULL; /* always available */
    cmd.args         = wave_args;
    cmd.nargs        = 1;
    cmd.arg_def_size = sizeof(sco_arg_def);
    cmd.fn           = wave;
    cmd.ctx          = NULL;
    if ((r = api->register_command(me, &cmd)) != SCO_OK) return r;

    if ((r = api->subscribe(me, "game.ready", on_ready, NULL)) != SCO_OK) return r;
    if ((r = api->subscribe(me, "tick", on_tick, NULL)) != SCO_OK) return r;

    api->log(me, SCO_LOG_INFO, api->host_version());
    api->status(me, "Hello from a plugin");
    return SCO_OK;
}

/* The host removes the commands and subscriptions itself; nothing to free here. */
SCO_EXPORT void sco_plugin_unload(void) {}
