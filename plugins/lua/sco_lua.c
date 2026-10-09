/* sco-lua: runs Lua plugins in a sandbox, through the plain-C plugin API only. See sco_lua.h.
 *
 * Every entry from the host (load, an event, a command, a task) runs in protected mode, so a
 * Lua error or an allocation failure never escapes into the game. Each entry has a step budget:
 * the VM's count hook charges HOOK_EVERY steps per HOOK_EVERY instructions, and the patched
 * string/table library loops charge one step per iteration (SCO_LUA_STEP, sco_lua_user.h).
 * The budget belongs to the outermost entry: a script invoking another script's command spends
 * from the same budget.
 * Past the budget every further instruction raises, so a script's own pcall can't hold on, and
 * the script is disabled. Finalizers (__gc) are refused: Lua runs them with hooks off. */
#include "sco_lua.h"
#include "sco_lua_user.h"
#include "lua.h"
#include "lauxlib.h"
#include "lualib.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HOOK_EVERY  1000
#define MAX_ERRORS  3    /* errors in callbacks before the script is disabled */
#define MAX_DEPTH   8    /* nested host -> script entries (a command invoking a command ...) */
#define MAX_ARGS    16
#define NAME_MAX_   64
#define REPLY_SIZE  256
#define SUBS_KEY    "sco-lua.subs"

typedef struct Script Script;

typedef struct EventSub { Script* s; int live; char name[NAME_MAX_]; } EventSub;
typedef struct CmdCtx   { Script* s; int ref; char name[NAME_MAX_]; } CmdCtx;
typedef struct Task     { Script* s; int ref; struct Task* prev; struct Task* next; } Task;

struct Script {
    int used, alive;
    sco_plugin* self;
    const sco_api* api;
    lua_State* L;
    size_t mem;
    int over;      /* past the budget in the current entry */
    int depth;     /* entries of this script on the C stack */
    int errors;
    char chunk[NAME_MAX_];
    EventSub events[SCO_LUA_MAX_EVENTS];
    CmdCtx cmds[SCO_LUA_MAX_COMMANDS];
    int ncmds;
    Task* tasks;   /* queued run_on_game_thread calls */
    int ntasks;    /* how many: each runs with a fresh budget, so they are capped */
};

void sco_lua_event_(const char* event, const void* data, void* ctx);   /* every Lua subscription */

static Script g_scripts[SCO_LUA_MAX_SCRIPTS];
static int g_depth;        /* entries of any script on the C stack */
static uint64_t g_steps;   /* steps since the outermost entry: one budget for every script it
                            * reaches, so a script can't multiply it by invoking another's commands */
static int g_over;         /* that budget ran out: every script still on the stack stops */

static Script* Of(lua_State* L) { return *(Script**)lua_getextraspace(L); }

static Script* Find(const sco_plugin* self) {
    for (int i = 0; i < SCO_LUA_MAX_SCRIPTS; ++i)
        if (g_scripts[i].used && g_scripts[i].self == self) return &g_scripts[i];
    return NULL;
}

static void Copy(char* dst, size_t n, const char* src) {
    if (!n) return;
    snprintf(dst, n, "%s", src ? src : "");
}

/* The error object on top of the stack as text, without allocating: outside protected mode a
 * failed allocation (lua_tostring converting a number at the memory cap) would abort. */
static const char* ErrorText(lua_State* L, char* buf, size_t n) {
    switch (lua_type(L, -1)) {
        case LUA_TSTRING: return lua_tostring(L, -1);   /* already a string: no conversion */
        case LUA_TNUMBER:
            if (lua_isinteger(L, -1)) snprintf(buf, n, "%lld", (long long)lua_tointeger(L, -1));
            else snprintf(buf, n, "%.14g", (double)lua_tonumber(L, -1));
            return buf;
        default: return "error object is not a string";
    }
}

static const char* ResultName(sco_result r) {
    switch (r) {
        case SCO_OK:           return "ok";
        case SCO_UNAVAILABLE:  return "unavailable";
        case SCO_NOT_FOUND:    return "not_found";
        case SCO_BAD_ARG:      return "bad_arg";
        case SCO_CRASHED:      return "crashed";
        case SCO_WRONG_THREAD: return "wrong_thread";
        case SCO_TOO_MANY:     return "too_many";
        default:               return "error";
    }
}

static const char* const kTypeNames[] = { "int", "float", "string", "bool" };

/* ---- memory and steps ---------------------------------------------------------------------- */

static void* Alloc(void* ud, void* ptr, size_t osize, size_t nsize) {
    Script* s = (Script*)ud;
    const size_t old = ptr ? osize : 0;
    if (nsize == 0) {
        free(ptr);
        s->mem -= old;
        return NULL;
    }
    if (nsize > old && s->mem - old + nsize > SCO_LUA_MEMORY_LIMIT) return NULL;
    void* p = realloc(ptr, nsize);
    if (!p) return NULL;
    s->mem = s->mem - old + nsize;
    return p;
}

static void Hook(lua_State* L, lua_Debug* ar);

void sco_lua_step(lua_State* L, int n) {
    Script* s = Of(L);
    if (!s || !s->depth) return;
    g_steps += (uint64_t)n;
    if (!g_over && g_steps <= SCO_LUA_STEP_BUDGET) return;
    g_over = 1;
    if (!s->over) {
        s->over = 1;
        lua_sethook(L, Hook, LUA_MASKCOUNT, 1);   /* from now on every instruction raises */
    }
    luaL_error(L, "ran past its step budget");
}

static void Hook(lua_State* L, lua_Debug* ar) {
    (void)ar;
    sco_lua_step(L, HOOK_EVERY);
}

/* ---- entering the script -------------------------------------------------------------------- */

/* Calls body(ud) in protected mode under the budget. LUA_OK, or an error status with the message
 * on top of the stack. Allocates nothing before the protected call. */
static int Enter(Script* s, lua_CFunction body, void* ud) {
    lua_State* L = s->L;
    if (g_depth == 0) { g_steps = 0; g_over = 0; }
    if (s->depth == 0) {
        s->over = 0;
        lua_sethook(L, Hook, LUA_MASKCOUNT, HOOK_EVERY);
    }
    ++s->depth;
    ++g_depth;
    lua_pushcfunction(L, body);
    lua_pushlightuserdata(L, ud);
    const int st = lua_pcall(L, 1, 0, 0);
    --g_depth;
    --s->depth;
    return st;
}

/* Back in script code after calling into the host (sco.invoke can run other scripts): if the
 * shared budget ran out meanwhile, this script stops too. */
static void CheckBudget(lua_State* L) { sco_lua_step(L, 0); }

static void Disable(Script* s, const char* why) {
    if (!s->alive) return;
    s->alive = 0;
    for (int i = 0; i < SCO_LUA_MAX_EVENTS; ++i) {
        EventSub* e = &s->events[i];
        if (e->live) {
            s->api->unsubscribe(s->self, e->name, sco_lua_event_);
            e->live = 0;
        }
    }
    char line[160];
    snprintf(line, sizeof(line), "script disabled: %s", why);
    s->api->log(s->self, SCO_LOG_ERROR, line);
    s->api->status(s->self, line);
}

/* Logs the error on top of the stack and pops it; disables the script on a budget overrun, a
 * memory error or the MAX_ERRORS-th error. */
static void Failed(Script* s, const char* where, int st) {
    char num[48];
    const char* msg = ErrorText(s->L, num, sizeof(num));
    char line[320];
    snprintf(line, sizeof(line), "%s: %s", where, msg);
    s->api->log(s->self, SCO_LOG_ERROR, line);
    lua_pop(s->L, 1);
    if (s->over) Disable(s, "ran past its step budget");
    else if (st == LUA_ERRMEM) Disable(s, "out of memory");
    else if (++s->errors >= MAX_ERRORS) Disable(s, "3 errors");
}

/* ---- events --------------------------------------------------------------------------------- */

typedef struct EventCall { EventSub* e; const void* data; } EventCall;

/* True when the function at index fn is still in subs[name] (index list). */
static int StillSubscribed(lua_State* L, int list, int fn) {
    const lua_Integer n = (lua_Integer)lua_rawlen(L, list);
    for (lua_Integer i = 1; i <= n; ++i) {
        lua_rawgeti(L, list, i);
        const int same = lua_rawequal(L, -1, fn);
        lua_pop(L, 1);
        if (same) return 1;
    }
    return 0;
}

static int EventBody(lua_State* L) {
    EventCall* k = (EventCall*)lua_touserdata(L, 1);
    Script* s = k->e->s;
    lua_getfield(L, LUA_REGISTRYINDEX, SUBS_KEY);
    lua_getfield(L, -1, k->e->name);
    if (!lua_istable(L, -1)) return 0;
    const int live = lua_gettop(L);
    const lua_Integer n = (lua_Integer)lua_rawlen(L, live);
    lua_createtable(L, (int)n, 0);                /* snapshot: subscribe applies from the next one */
    const int snap = lua_gettop(L);
    for (lua_Integer i = 1; i <= n; ++i) { lua_rawgeti(L, live, i); lua_rawseti(L, snap, i); }
    for (lua_Integer i = 1; i <= n && s->alive; ++i) {
        lua_rawgeti(L, snap, i);
        const int fn = lua_gettop(L);
        if (!StillSubscribed(L, live, fn)) { lua_pop(L, 1); continue; }   /* unsubscribe applies at once */
        lua_pushstring(L, k->e->name);
        if (strcmp(k->e->name, "tick") == 0 && k->data) lua_pushinteger(L, *(const uint32_t*)k->data);
        else lua_pushnil(L);
        const int st = lua_pcall(L, 2, 0, 0);
        if (st != LUA_OK) {
            char where[NAME_MAX_ + 16];
            snprintf(where, sizeof(where), "event %s", k->e->name);
            Failed(s, where, st);
        }
        lua_settop(L, snap);
    }
    return 0;
}

void sco_lua_event_(const char* event, const void* data, void* ctx) {
    (void)event;
    EventSub* e = (EventSub*)ctx;
    Script* s = e->s;
    if (!s->alive || !s->L || g_depth >= MAX_DEPTH || !lua_checkstack(s->L, 4)) return;
    const int top = lua_gettop(s->L);
    EventCall k = { e, data };
    const int st = Enter(s, EventBody, &k);
    if (st != LUA_OK) {
        char where[NAME_MAX_ + 16];
        snprintf(where, sizeof(where), "event %s", e->name);
        Failed(s, where, st);
    }
    lua_settop(s->L, top);
}

/* ---- commands ------------------------------------------------------------------------------- */

typedef struct CmdCall {
    CmdCtx* c;
    const sco_arg* args;
    uint32_t nargs;
    char* reply;
    uint32_t replySize;
} CmdCall;

static int CmdBody(lua_State* L) {
    CmdCall* k = (CmdCall*)lua_touserdata(L, 1);
    luaL_checkstack(L, (int)k->nargs + 2, "too many arguments");
    lua_rawgeti(L, LUA_REGISTRYINDEX, k->c->ref);
    for (uint32_t i = 0; i < k->nargs; ++i) {
        const sco_arg* a = &k->args[i];
        switch (a->type) {
            case SCO_ARG_INT:    lua_pushinteger(L, (lua_Integer)a->v.i); break;
            case SCO_ARG_FLOAT:  lua_pushnumber(L, a->v.f); break;
            case SCO_ARG_STRING: lua_pushstring(L, a->v.s); break;
            case SCO_ARG_BOOL:   lua_pushboolean(L, a->v.i != 0); break;
            default:             lua_pushnil(L); break;
        }
    }
    lua_call(L, (int)k->nargs, 1);
    if (lua_isnil(L, -1)) Copy(k->reply, k->replySize, "");
    else if (lua_type(L, -1) == LUA_TSTRING) Copy(k->reply, k->replySize, lua_tostring(L, -1));
    else return luaL_error(L, "reply must be a string or nil, not %s", luaL_typename(L, -1));
    return 0;
}

static sco_result CmdThunk(const sco_arg* args, uint32_t nargs, void* ctx, char* reply, uint32_t replySize) {
    CmdCtx* c = (CmdCtx*)ctx;
    Script* s = c->s;
    if (!s->alive || !s->L) { Copy(reply, replySize, "script disabled"); return SCO_UNAVAILABLE; }
    if (g_depth >= MAX_DEPTH || !lua_checkstack(s->L, 4)) { Copy(reply, replySize, "calls nested too deep"); return SCO_TOO_MANY; }
    const int top = lua_gettop(s->L);
    CmdCall k = { c, args, nargs, reply, replySize };
    const int st = Enter(s, CmdBody, &k);
    if (st == LUA_OK) { lua_settop(s->L, top); return SCO_OK; }
    char num[48];
    Copy(reply, replySize, ErrorText(s->L, num, sizeof(num)));
    const sco_result r = s->over ? SCO_CRASHED : st == LUA_ERRMEM ? SCO_TOO_MANY : SCO_BAD_ARG;
    Failed(s, c->name, st);
    lua_settop(s->L, top);
    return r;
}

/* ---- tasks ---------------------------------------------------------------------------------- */

static void Unlink(Task* t) {
    if (t->prev) t->prev->next = t->next; else t->s->tasks = t->next;
    if (t->next) t->next->prev = t->prev;
    --t->s->ntasks;
}

static int TaskBody(lua_State* L) {
    Task* t = (Task*)lua_touserdata(L, 1);
    lua_rawgeti(L, LUA_REGISTRYINDEX, t->ref);
    lua_call(L, 0, 0);
    return 0;
}

static void TaskThunk(void* ctx) {
    Task* t = (Task*)ctx;
    Script* s = t->s;
    Unlink(t);
    if (s->L) {
        const int top = lua_gettop(s->L);
        if (s->alive && g_depth < MAX_DEPTH && lua_checkstack(s->L, 4)) {
            const int st = Enter(s, TaskBody, t);
            if (st != LUA_OK) Failed(s, "task", st);
        }
        lua_settop(s->L, top);
        luaL_unref(s->L, LUA_REGISTRYINDEX, t->ref);
    }
    free(t);
}

/* ---- the sco table -------------------------------------------------------------------------- */

static int PushResult(lua_State* L, sco_result r) {
    if (r == SCO_OK) { lua_pushboolean(L, 1); return 1; }
    lua_pushboolean(L, 0);
    lua_pushstring(L, ResultName(r));
    return 2;
}

static int L_host_version(lua_State* L) {
    const char* v = Of(L)->api->host_version();
    lua_pushstring(L, v ? v : "");
    return 1;
}

static int L_has(lua_State* L) {
    lua_pushboolean(L, Of(L)->api->has(luaL_checkstring(L, 1)) != 0);
    return 1;
}

static int L_status(lua_State* L) {
    Script* s = Of(L);
    s->api->status(s->self, luaL_checkstring(L, 1));
    return 0;
}

static int L_log(lua_State* L) {
    static const char* const levels[] = { "info", "warn", "error", NULL };
    const int level = luaL_checkoption(L, 1, NULL, levels);
    Script* s = Of(L);
    s->api->log(s->self, (sco_log_level)level, luaL_checkstring(L, 2));
    return 0;
}

static int L_print(lua_State* L) {
    const int n = lua_gettop(L);
    luaL_Buffer b;
    luaL_buffinit(L, &b);
    for (int i = 1; i <= n; ++i) {
        if (i > 1) luaL_addchar(&b, '\t');
        luaL_tolstring(L, i, NULL);
        luaL_addvalue(&b);
    }
    luaL_pushresult(&b);
    Script* s = Of(L);
    s->api->log(s->self, SCO_LOG_INFO, lua_tostring(L, -1));
    return 0;
}

/* subs[event], created when create is set; pushes it (or nil). */
static void PushSubs(lua_State* L, const char* event, int create) {
    lua_getfield(L, LUA_REGISTRYINDEX, SUBS_KEY);
    lua_getfield(L, -1, event);
    if (lua_isnil(L, -1) && create) {
        lua_pop(L, 1);
        lua_newtable(L);
        lua_pushvalue(L, -1);
        lua_setfield(L, -3, event);
    }
    lua_remove(L, -2);
}

static int L_subscribe(lua_State* L) {
    Script* s = Of(L);
    if (!s->alive) return PushResult(L, SCO_UNAVAILABLE);   /* disabled, an outer frame still running */
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    if (strlen(event) >= NAME_MAX_ || !*event) return PushResult(L, SCO_BAD_ARG);
    PushSubs(L, event, 1);
    const int list = lua_gettop(L);
    if (StillSubscribed(L, list, 2)) return PushResult(L, SCO_BAD_ARG);   /* same key twice */
    /* Dispatch rescans the list before each callback, uncharged: keep it short. */
    if (lua_rawlen(L, list) >= SCO_LUA_MAX_SUBSCRIBERS) return PushResult(L, SCO_TOO_MANY);
    EventSub* e = NULL;
    for (int i = 0; i < SCO_LUA_MAX_EVENTS && !e; ++i)
        if (s->events[i].live && strcmp(s->events[i].name, event) == 0) e = &s->events[i];
    if (!e) {
        for (int i = 0; i < SCO_LUA_MAX_EVENTS && !e; ++i) if (!s->events[i].live) e = &s->events[i];
        if (!e) return PushResult(L, SCO_TOO_MANY);
        e->s = s;
        Copy(e->name, sizeof(e->name), event);
        const sco_result r = s->api->subscribe(s->self, event, sco_lua_event_, e);
        if (r != SCO_OK) return PushResult(L, r);
        e->live = 1;
    }
    lua_pushvalue(L, 2);
    lua_rawseti(L, list, (lua_Integer)lua_rawlen(L, list) + 1);
    return PushResult(L, SCO_OK);
}

static int L_unsubscribe(lua_State* L) {
    Script* s = Of(L);
    const char* event = luaL_checkstring(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    PushSubs(L, event, 0);
    if (!lua_istable(L, -1)) return PushResult(L, SCO_NOT_FOUND);
    const int list = lua_gettop(L);
    const lua_Integer n = (lua_Integer)lua_rawlen(L, list);
    lua_Integer at = 0;
    for (lua_Integer i = 1; i <= n && !at; ++i) {
        lua_rawgeti(L, list, i);
        if (lua_rawequal(L, -1, 2)) at = i;
        lua_pop(L, 1);
    }
    if (!at) return PushResult(L, SCO_NOT_FOUND);
    for (lua_Integer i = at; i < n; ++i) { lua_rawgeti(L, list, i + 1); lua_rawseti(L, list, i); }
    lua_pushnil(L);
    lua_rawseti(L, list, n);
    if (n == 1) {
        for (int i = 0; i < SCO_LUA_MAX_EVENTS; ++i) {
            EventSub* e = &s->events[i];
            if (e->live && strcmp(e->name, event) == 0) {
                s->api->unsubscribe(s->self, event, sco_lua_event_);
                e->live = 0;
            }
        }
        lua_getfield(L, LUA_REGISTRYINDEX, SUBS_KEY);
        lua_pushnil(L);
        lua_setfield(L, -2, event);
    }
    return PushResult(L, SCO_OK);
}

static int L_run_on_game_thread(lua_State* L) {
    Script* s = Of(L);
    if (!s->alive) return PushResult(L, SCO_UNAVAILABLE);
    luaL_checktype(L, 1, LUA_TFUNCTION);
    if (s->ntasks >= SCO_LUA_MAX_TASKS) return PushResult(L, SCO_TOO_MANY);
    Task* t = (Task*)malloc(sizeof(Task));
    if (!t) return PushResult(L, SCO_TOO_MANY);
    lua_pushvalue(L, 1);
    t->s = s;
    t->ref = luaL_ref(L, LUA_REGISTRYINDEX);   /* raises on no memory: t leaks once, bounded */
    const sco_result r = s->api->run_on_game_thread(s->self, TaskThunk, t);
    if (r != SCO_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, t->ref);
        free(t);
        return PushResult(L, r);
    }
    t->prev = NULL;
    t->next = s->tasks;
    if (s->tasks) s->tasks->prev = t;
    s->tasks = t;
    ++s->ntasks;
    return PushResult(L, SCO_OK);
}

/* t[key] as an optional string (NULL when nil); pushes the value. */
static const char* OptField(lua_State* L, int t, const char* key, int* bad) {
    const int ty = lua_getfield(L, t, key);
    if (ty == LUA_TNIL) return NULL;
    if (ty != LUA_TSTRING) { *bad = 1; return NULL; }
    return lua_tostring(L, -1);
}

static int TypeOf(const char* name) {
    for (int i = 0; i < 4; ++i) if (name && strcmp(name, kTypeNames[i]) == 0) return i;
    return -1;
}

static int L_register_command(lua_State* L) {
    Script* s = Of(L);
    if (!s->alive) return PushResult(L, SCO_UNAVAILABLE);
    luaL_checktype(L, 1, LUA_TTABLE);
    luaL_checkstack(L, 8 + 4 * MAX_ARGS, "register_command");
    int bad = 0;
    const char* name = OptField(L, 1, "name", &bad);
    const char* title = OptField(L, 1, "title", &bad);
    const char* help = OptField(L, 1, "help", &bad);
    const char* cap = OptField(L, 1, "capability", &bad);
    if (lua_getfield(L, 1, "fn") != LUA_TFUNCTION) bad = 1;
    const int fn = lua_gettop(L);
    sco_arg_def defs[MAX_ARGS];
    uint32_t nargs = 0;
    const int ty = lua_getfield(L, 1, "args");
    if (ty == LUA_TTABLE) {
        const int args = lua_gettop(L);
        const lua_Integer n = (lua_Integer)lua_rawlen(L, args);
        if (n > MAX_ARGS) bad = 1;
        for (lua_Integer i = 1; i <= n && !bad; ++i) {
            if (lua_rawgeti(L, args, i) != LUA_TTABLE) { bad = 1; break; }
            const int a = lua_gettop(L);
            defs[nargs].name = OptField(L, a, "name", &bad);
            const int type = TypeOf(OptField(L, a, "type", &bad));
            defs[nargs].help = OptField(L, a, "help", &bad);
            if (!defs[nargs].name || type < 0) bad = 1;
            defs[nargs].type = (uint32_t)(type < 0 ? 0 : type);
            defs[nargs]._pad = 0;
            ++nargs;
        }
    } else if (ty != LUA_TNIL) {
        bad = 1;
    }
    if (bad || !name || strlen(name) >= NAME_MAX_) return PushResult(L, SCO_BAD_ARG);
    if (s->ncmds == SCO_LUA_MAX_COMMANDS) return PushResult(L, SCO_TOO_MANY);
    CmdCtx* c = &s->cmds[s->ncmds];
    lua_pushvalue(L, fn);
    c->s = s;
    c->ref = luaL_ref(L, LUA_REGISTRYINDEX);
    Copy(c->name, sizeof(c->name), name);
    sco_command cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.size = sizeof(sco_command);
    cmd.name = name;
    cmd.title = title;
    cmd.help = help;
    cmd.capability = cap;
    cmd.args = nargs ? defs : NULL;
    cmd.nargs = nargs;
    cmd.arg_def_size = sizeof(sco_arg_def);
    cmd.fn = CmdThunk;
    cmd.ctx = c;
    const sco_result r = s->api->register_command(s->self, &cmd);
    if (r != SCO_OK) {
        luaL_unref(L, LUA_REGISTRYINDEX, c->ref);
        return PushResult(L, r);
    }
    ++s->ncmds;
    return PushResult(L, SCO_OK);
}

static const sco_command* FindCommand(const sco_api* api, const char* name) {
    const sco_command* list[512];
    const uint32_t n = api->list_commands(list, 512);
    for (uint32_t i = 0; i < n && i < 512; ++i)
        if (list[i] && list[i]->name && strcmp(list[i]->name, name) == 0) return list[i];
    return NULL;
}

typedef struct Done { int called; sco_result r; char reply[REPLY_SIZE]; } Done;

static void DoneCb(sco_result r, const char* reply, void* ctx) {
    Done* d = (Done*)ctx;
    d->called = 1;
    d->r = r;
    Copy(d->reply, sizeof(d->reply), reply);
}

static int L_invoke(lua_State* L) {
    Script* s = Of(L);
    const char* name = luaL_checkstring(L, 1);
    const int nargs = lua_gettop(L) - 1;
    const sco_command* c = FindCommand(s->api, name);
    if (!c) return PushResult(L, SCO_NOT_FOUND);
    if (nargs > MAX_ARGS || (uint32_t)nargs != c->nargs) return PushResult(L, SCO_BAD_ARG);
    sco_arg args[MAX_ARGS];
    const unsigned char* at = (const unsigned char*)c->args;
    for (int i = 0; i < nargs; ++i, at += c->arg_def_size) {
        const sco_arg_def* d = (const sco_arg_def*)at;
        const int v = i + 2;
        sco_arg* a = &args[i];
        memset(a, 0, sizeof(*a));
        a->type = d->type;
        int ok = 0;
        switch (d->type) {
            case SCO_ARG_INT:    { int isint = 0; a->v.i = lua_tointegerx(L, v, &isint); ok = isint && lua_isinteger(L, v); break; }   /* 2.0 is a float */
            case SCO_ARG_FLOAT:  ok = lua_type(L, v) == LUA_TNUMBER; a->v.f = lua_tonumber(L, v); break;
            case SCO_ARG_STRING: ok = lua_type(L, v) == LUA_TSTRING; a->v.s = lua_tostring(L, v); break;
            case SCO_ARG_BOOL:   ok = lua_type(L, v) == LUA_TBOOLEAN; a->v.i = lua_toboolean(L, v); break;
            default: break;
        }
        if (!ok) return PushResult(L, SCO_BAD_ARG);
    }
    Done d;
    memset(&d, 0, sizeof(d));
    const sco_result r = s->api->invoke(s->self, name, nargs ? args : NULL, (uint32_t)nargs, DoneCb, &d);
    CheckBudget(L);
    if (r != SCO_OK && !d.called) return PushResult(L, r);
    if (d.r == SCO_OK) { lua_pushboolean(L, 1); lua_pushstring(L, d.reply); return 2; }
    lua_pushboolean(L, 0);
    lua_pushstring(L, ResultName(d.r));
    lua_pushstring(L, d.reply);
    return 3;
}

static void SetString(lua_State* L, const char* key, const char* v) {
    if (!v) return;
    lua_pushstring(L, v);
    lua_setfield(L, -2, key);
}

static int L_list_commands(lua_State* L) {
    Script* s = Of(L);
    const sco_command* list[512];
    uint32_t n = s->api->list_commands(list, 512);
    if (n > 512) n = 512;
    lua_createtable(L, (int)n, 0);
    for (uint32_t i = 0; i < n; ++i) {
        const sco_command* c = list[i];
        if (!c) continue;
        lua_createtable(L, 0, 5);
        SetString(L, "name", c->name);
        SetString(L, "title", c->title);
        SetString(L, "help", c->help);
        SetString(L, "capability", c->capability);
        lua_createtable(L, (int)c->nargs, 0);
        const unsigned char* at = (const unsigned char*)c->args;
        for (uint32_t k = 0; k < c->nargs; ++k, at += c->arg_def_size) {
            const sco_arg_def* d = (const sco_arg_def*)at;
            lua_createtable(L, 0, 3);
            SetString(L, "name", d->name);
            SetString(L, "type", d->type < 4 ? kTypeNames[d->type] : "?");
            SetString(L, "help", d->help);
            lua_rawseti(L, -2, (lua_Integer)k + 1);
        }
        lua_setfield(L, -2, "args");
        lua_rawseti(L, -2, (lua_Integer)i + 1);
    }
    return 1;
}

/* setmetatable without finalizers: Lua runs __gc with hooks off, out of the budget's reach. */
static int L_setmetatable(lua_State* L) {
    if (lua_istable(L, 2)) {
        lua_pushliteral(L, "__gc");
        if (lua_rawget(L, 2) != LUA_TNIL) return luaL_error(L, "__gc is not allowed in sco-lua");
        lua_pop(L, 1);
    }
    lua_pushvalue(L, lua_upvalueindex(1));
    lua_insert(L, 1);
    lua_call(L, lua_gettop(L) - 1, 1);
    return 1;
}

/* ---- setup ---------------------------------------------------------------------------------- */

typedef struct LoadCall { const char* source; size_t size; const char* chunk; } LoadCall;

static int SetupBody(lua_State* L) {
    LoadCall* k = (LoadCall*)lua_touserdata(L, 1);
    Script* s = Of(L);

    luaL_requiref(L, LUA_GNAME, luaopen_base, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_STRLIBNAME, luaopen_string, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_TABLIBNAME, luaopen_table, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_MATHLIBNAME, luaopen_math, 1);
    lua_pop(L, 1);
    luaL_requiref(L, LUA_UTF8LIBNAME, luaopen_utf8, 1);
    lua_pop(L, 1);

    /* Globals: keep the safe base functions and the four libraries, drop the rest. */
    static const char* const keep[] = {
        "assert", "error", "ipairs", "next", "pairs", "pcall", "select", "tonumber", "tostring",
        "type", "xpcall", "rawequal", "rawget", "rawlen", "rawset", "getmetatable", "_G",
        "_VERSION", LUA_STRLIBNAME, LUA_TABLIBNAME, LUA_MATHLIBNAME, LUA_UTF8LIBNAME, NULL };
    lua_pushglobaltable(L);
    const int g = lua_gettop(L);
    lua_getfield(L, g, "setmetatable");
    lua_pushcclosure(L, L_setmetatable, 1);
    lua_newtable(L);                                  /* names to drop */
    const int drop = lua_gettop(L);
    lua_pushnil(L);
    while (lua_next(L, g)) {
        lua_pop(L, 1);
        int kept = 0;
        if (lua_type(L, -1) == LUA_TSTRING)
            for (int i = 0; keep[i] && !kept; ++i) kept = strcmp(lua_tostring(L, -1), keep[i]) == 0;
        if (!kept) { lua_pushvalue(L, -1); lua_rawseti(L, drop, (lua_Integer)lua_rawlen(L, drop) + 1); }
    }
    for (lua_Integer i = 1, n = (lua_Integer)lua_rawlen(L, drop); i <= n; ++i) {
        lua_rawgeti(L, drop, i);
        lua_pushnil(L);
        lua_rawset(L, g);
    }
    lua_pop(L, 1);                                    /* drop */
    lua_setfield(L, g, "setmetatable");               /* the __gc-refusing wrapper */
    lua_pushcfunction(L, L_print);
    lua_setfield(L, g, "print");
    lua_getfield(L, g, LUA_STRLIBNAME);
    lua_pushnil(L);
    lua_setfield(L, -2, "dump");                      /* bytecode out: nothing can load it, but no */
    lua_pop(L, 1);
    lua_getfield(L, g, LUA_MATHLIBNAME);              /* math.random stays; seeding is harmless */
    lua_pop(L, 1);

    static const luaL_Reg fns[] = {
        { "host_version", L_host_version }, { "has", L_has }, { "status", L_status },
        { "log", L_log }, { "subscribe", L_subscribe }, { "unsubscribe", L_unsubscribe },
        { "run_on_game_thread", L_run_on_game_thread }, { "register_command", L_register_command },
        { "invoke", L_invoke }, { "list_commands", L_list_commands }, { NULL, NULL } };
    luaL_newlib(L, fns);
    lua_pushinteger(L, s->api->major);
    lua_setfield(L, -2, "api_major");
    lua_pushinteger(L, s->api->minor);
    lua_setfield(L, -2, "api_minor");
    lua_setfield(L, g, "sco");

    lua_newtable(L);
    lua_setfield(L, LUA_REGISTRYINDEX, SUBS_KEY);

    lua_pushfstring(L, "@%s", k->chunk);
    const int st = luaL_loadbufferx(L, k->source, k->size, lua_tostring(L, -1), "t");
    if (st != LUA_OK) return lua_error(L);
    lua_call(L, 0, 0);
    return 0;
}

static void Close(Script* s) {
    while (s->tasks) { Task* t = s->tasks; s->tasks = t->next; free(t); }
    if (s->L) lua_close(s->L);
    memset(s, 0, sizeof(*s));
}

sco_result sco_lua_load(const sco_api* api, sco_plugin* self, const char* chunkname,
                        const char* source, size_t size, char* err, size_t err_size) {
    if (err && err_size) err[0] = 0;
    if (!api || !self || !chunkname || !source) { Copy(err, err_size, "null argument"); return SCO_BAD_ARG; }
    if (size > SCO_LUA_MAX_SOURCE) { Copy(err, err_size, "script too big"); return SCO_BAD_ARG; }
    if (Find(self)) { Copy(err, err_size, "this plugin already has a script"); return SCO_BAD_ARG; }
    Script* s = NULL;
    for (int i = 0; i < SCO_LUA_MAX_SCRIPTS && !s; ++i) if (!g_scripts[i].used) s = &g_scripts[i];
    if (!s) { Copy(err, err_size, "too many scripts"); return SCO_TOO_MANY; }
    memset(s, 0, sizeof(*s));
    s->used = 1;
    s->api = api;
    s->self = self;
    Copy(s->chunk, sizeof(s->chunk), chunkname);
    s->L = lua_newstate(Alloc, s);
    if (!s->L) { Close(s); Copy(err, err_size, "out of memory"); return SCO_TOO_MANY; }
    *(Script**)lua_getextraspace(s->L) = s;
    s->alive = 1;
    LoadCall k = { source, size, s->chunk };
    const int st = Enter(s, SetupBody, &k);
    if (st == LUA_OK && s->alive) return SCO_OK;
    sco_result r = SCO_BAD_ARG;
    if (st == LUA_OK) {
        Copy(err, err_size, "script disabled while loading");   /* the log says why */
    } else {
        char num[48];
        Copy(err, err_size, ErrorText(s->L, num, sizeof(num)));
        if (st == LUA_ERRMEM) r = SCO_TOO_MANY;
    }
    for (int i = 0; i < SCO_LUA_MAX_EVENTS; ++i)
        if (s->events[i].live) api->unsubscribe(self, s->events[i].name, sco_lua_event_);
    Close(s);
    return r;
}

void sco_lua_unload(sco_plugin* self) {
    Script* s = Find(self);
    if (s && !s->depth) Close(s);
}

int sco_lua_alive(const sco_plugin* self) {
    const Script* s = Find(self);
    return s && s->alive;
}
