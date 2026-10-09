/* sco-lua: runs Lua plugins (kind = lua) in a sandbox, through the plain-C plugin API only.
 *
 * sco-lua includes nothing from sco-core but sco_api.h: everything a script does goes through
 * the same sco_api table a native plugin gets. The host compiles it in (sc-offline links it
 * into dinput8.dll) and hands it to the plugin loader:
 *
 *   static const sco::plugins::ScriptRuntime kLua{ sco_lua_load, sco_lua_unload };
 *   sco::plugins::LoadScript(p, api, self, kLua);
 *
 * Game thread only, every function. Script reference: sdk/docs/lua.md. */
#ifndef SCO_LUA_H
#define SCO_LUA_H

#include "sco_api.h"
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Limits for every script. */
#define SCO_LUA_STEP_BUDGET   1000000u          /* per callback: VM instructions + library steps */
#define SCO_LUA_MEMORY_LIMIT  (64u * 1024 * 1024) /* bytes per script */
#define SCO_LUA_MAX_SOURCE    (1024u * 1024)     /* entry script size */
#define SCO_LUA_MAX_SCRIPTS   64
#define SCO_LUA_MAX_EVENTS    16                 /* distinct event names one script subscribes to */
#define SCO_LUA_MAX_COMMANDS  64                 /* commands one script registers */

/* Creates a sandboxed state for plugin `self`, then runs `source` (text only; bytecode is
 * refused) once, under the step budget. `chunkname` names the script in error messages
 * ("main.lua"). SCO_OK: the script is loaded and its callbacks are live. Anything else: nothing
 * of the script is left and err holds why ("main.lua:3: attempt to call a nil value ...",
 * "ran past its step budget", "out of memory"); the caller should still sco::Release(self) to
 * drop anything the script registered before it failed.
 * SCO_BAD_ARG: null argument, source over SCO_LUA_MAX_SOURCE, or self already has a script.
 * SCO_TOO_MANY: SCO_LUA_MAX_SCRIPTS loaded, or no memory. */
sco_result sco_lua_load(const sco_api* api, sco_plugin* self, const char* chunkname,
                        const char* source, size_t size, char* err, size_t err_size);

/* Frees the script of `self`. Call after the host released `self` (sco::Release), so nothing
 * can call into the script any more. No-op for a handle without a script. */
void sco_lua_unload(sco_plugin* self);

/* 1 while the script of `self` is loaded and not disabled, else 0. */
int sco_lua_alive(const sco_plugin* self);

#ifdef __cplusplus
}
#endif

#endif /* SCO_LUA_H */
