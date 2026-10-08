/*
 * my_plugin: start here. Copy sdk/template/ to a new folder, then:
 *   1. pick an id: 1-31 characters from a-z, 0-9 and _ (not sco, host, menu or game),
 *   2. replace every "my_plugin" in this file, plugin.ini and CMakeLists.txt with it,
 *   3. build: cmake -S . -B build && cmake --build build && cmake --install build --prefix out
 *
 * Reference: docs/api-v1.md in the SDK. License: GPL-3.0 (see LICENSE in the SDK); plugins
 * built against sco_api.h are GPL-3.0 too, with no linking exception.
 */
#include <stdio.h>
#include <string.h>

#include "sco_api.h"

static const sco_api* api;  /* valid from sco_plugin_load until sco_plugin_unload */
static sco_plugin*    me;   /* pass back to every call that takes `self` */

static const sco_plugin_info info = {
    sizeof(sco_plugin_info), SCO_API_MAJOR, SCO_API_MINOR,
    "my_plugin",  /* = id in plugin.ini */
    "0.1.0",
    "you",
};

/* my_plugin.ping: a command with no arguments. Runs on the game thread. */
static sco_result ping(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t reply_size) {
    (void)args; (void)nargs; (void)ctx;
    snprintf(reply, reply_size, "pong");
    return SCO_OK;
}

SCO_EXPORT const sco_plugin_info* sco_plugin_query(void) { return &info; }

SCO_EXPORT sco_result sco_plugin_load(const sco_api* a, sco_plugin* self) {
    sco_command cmd;

    api = a;
    me = self;

    memset(&cmd, 0, sizeof cmd);
    cmd.size         = sizeof cmd;
    cmd.name         = "my_plugin.ping";
    cmd.title        = "Ping";
    cmd.arg_def_size = sizeof(sco_arg_def);
    cmd.fn           = ping;
    if (api->register_command(me, &cmd) != SCO_OK) return SCO_BAD_ARG;

    api->log(me, SCO_LOG_INFO, "loaded");
    return SCO_OK;  /* anything else: the host unloads the plugin */
}

SCO_EXPORT void sco_plugin_unload(void) {}
