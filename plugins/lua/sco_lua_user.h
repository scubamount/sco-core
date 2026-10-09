/* sco-lua: the hook the vendored Lua includes at the end of luaconf.h.
 * SCO_LUA_STEP charges the running script's instruction budget from C loops in the string and
 * table libraries, which the Lua count hook can't see (string.rep, string.find on a long
 * subject, table.concat, table.sort, ...). Defined in sco_lua.c. */
#ifndef SCO_LUA_USER_H
#define SCO_LUA_USER_H

struct lua_State;
void sco_lua_step(struct lua_State* L, int n);
#define SCO_LUA_STEP(L, n) sco_lua_step((L), (n))

#endif
