/*
 * sco-plugin-check: checks a built plugin folder on a dev machine, without the game.
 *
 *   sco-plugin-check <plugin folder> [--cap NAME]... [--invoke NAME [ARG]...]
 *
 * For every kind it reads plugin.ini with the same rules as the host (id, name, version, api,
 * kind, entry, requires) and checks the folder name matches the id.
 *   data:   lists the files the host would index (missions/, rules/, scripts/, lists/).
 *   lua:    checks the entry script exists (syntax is checked separately with luac -p).
 *   native: loads the plugin, runs sco_plugin_query and sco_plugin_load against a stand-in
 *           sco_api, fires game.ready and a few ticks, runs every command that takes no
 *           arguments (and the one given with --invoke), then sco_plugin_unload.
 * --cap NAME makes has(NAME) answer 1 (default: every capability is missing).
 *
 * Exit 0 = every check passed. 1 = a check failed (the reason is printed). 2 = bad usage.
 *
 * The stand-in host enforces the documented rules (copied strings, name prefix, arg defs read
 * with arg_def_size, argument type checks), so a plugin that passes here does what the real
 * host expects. It is not the real host: no game thread, no crash guard, no game features.
 *
 * Part of the sco SDK. GPL-3.0.
 */
#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dirent.h>
#include <dlfcn.h>
#include <sys/stat.h>
#endif
#include <ctype.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sco_api.h"

static int failures;

static void fail(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("FAIL ", stdout);
    vprintf(fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
    ++failures;
}

/* ---- plugin.ini --------------------------------------------------------- */

#define MAX_REQUIRES 16

typedef struct manifest {
    char id[64], name[128], version[64], author[128], api[16], kind[16], entry[128];
    char requires_[MAX_REQUIRES][64];
    int  nrequires;
    int  api_major, api_minor;
} manifest;

static char* trim(char* s) {
    char* e;
    while (*s == ' ' || *s == '\t') ++s;
    e = s + strlen(s);
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    *e = 0;
    return s;
}

/* A comment starts with ';' or '#' at the start of the text or after a space or tab. */
static void strip_comment(char* s) {
    size_t i;
    for (i = 0; s[i]; ++i)
        if ((s[i] == ';' || s[i] == '#') && (i == 0 || s[i - 1] == ' ' || s[i - 1] == '\t')) {
            s[i] = 0;
            return;
        }
}

static int valid_id(const char* s) {
    size_t n = strlen(s), i;
    if (n < 1 || n > 31) return 0;
    for (i = 0; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_')) return 0;
    return strcmp(s, "sco") && strcmp(s, "host") && strcmp(s, "menu") && strcmp(s, "game");
}

static int valid_capability(const char* s) {
    size_t n = strlen(s), i;
    if (n < 1 || n > 63 || s[0] == '.' || s[n - 1] == '.') return 0;
    for (i = 0; i < n; ++i)
        if (!((s[i] >= 'a' && s[i] <= 'z') || (s[i] >= '0' && s[i] <= '9') || s[i] == '_' || s[i] == '.'))
            return 0;
    return 1;
}

static int set_field(char* dst, size_t cap, const char* v, const char* key, int line) {
    if (strlen(v) >= cap) { fail("plugin.ini line %d: %s is too long", line, key); return 0; }
    strcpy(dst, v);
    return 1;
}

static int read_manifest(const char* path, manifest* m) {
    FILE* f = fopen(path, "rb");
    char  buf[1024], seen[16][16];
    int   nseen = 0, line = 0, ok = 1, i;
    if (!f) { fail("%s: can't open", path); return 0; }
    memset(m, 0, sizeof *m);
    while (fgets(buf, sizeof buf, f)) {
        char *s = buf, *eq, *key, *val;
        ++line;
        if (line == 1 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
            s += 3;
        strip_comment(s);
        s = trim(s);
        if (!*s) continue;
        eq = strchr(s, '=');
        if (!eq) { fail("plugin.ini line %d: no '='", line); ok = 0; continue; }
        *eq = 0;
        key = trim(s);
        val = trim(eq + 1);
        for (i = 0; i < nseen; ++i)
            if (!strcmp(seen[i], key)) { fail("plugin.ini line %d: duplicate key '%s'", line, key); ok = 0; }
        if (nseen < 16 && strlen(key) < 16) strcpy(seen[nseen++], key);

        if      (!strcmp(key, "id"))      ok &= set_field(m->id, sizeof m->id, val, key, line);
        else if (!strcmp(key, "name"))    ok &= set_field(m->name, 64, val, key, line);
        else if (!strcmp(key, "version")) ok &= set_field(m->version, 32, val, key, line);
        else if (!strcmp(key, "author"))  ok &= set_field(m->author, 64, val, key, line);
        else if (!strcmp(key, "api"))     ok &= set_field(m->api, sizeof m->api, val, key, line);
        else if (!strcmp(key, "kind"))    ok &= set_field(m->kind, sizeof m->kind, val, key, line);
        else if (!strcmp(key, "entry"))   ok &= set_field(m->entry, 64, val, key, line);
        else if (!strcmp(key, "requires")) {
            char* tok = strtok(val, ",");
            while (tok) {
                char* cap = trim(tok);
                if (!valid_capability(cap)) { fail("plugin.ini line %d: bad capability '%s'", line, cap); ok = 0; }
                else if (m->nrequires == MAX_REQUIRES) { fail("plugin.ini line %d: more than 16 requires", line); ok = 0; }
                else strcpy(m->requires_[m->nrequires++], cap);
                tok = strtok(NULL, ",");
            }
        }
        /* Unknown keys are ignored: a later minor may add keys. */
    }
    fclose(f);

    if (!m->id[0])      { fail("plugin.ini: id is missing"); ok = 0; }
    else if (!valid_id(m->id)) { fail("plugin.ini: id '%s' must be 1-31 of a-z 0-9 _ and not reserved", m->id); ok = 0; }
    if (!m->name[0])    { fail("plugin.ini: name is missing"); ok = 0; }
    if (!m->version[0]) { fail("plugin.ini: version is missing"); ok = 0; }
    if (!m->api[0])     { fail("plugin.ini: api is missing"); ok = 0; }
    else if (sscanf(m->api, "%d.%d", &m->api_major, &m->api_minor) != 2) { fail("plugin.ini: api must be <major>.<minor>"); ok = 0; }
    else if (m->api_major != SCO_API_MAJOR || m->api_minor > SCO_API_MINOR) {
        fail("plugin.ini: api %s, but this SDK is %d.%d", m->api, SCO_API_MAJOR, SCO_API_MINOR); ok = 0;
    }
    if (strcmp(m->kind, "native") && strcmp(m->kind, "lua") && strcmp(m->kind, "data")) {
        fail("plugin.ini: kind must be native, lua or data"); ok = 0;
    } else if (!strcmp(m->kind, "data") && m->entry[0]) {
        fail("plugin.ini: a data pack has no entry"); ok = 0;
    } else if (strcmp(m->kind, "data") && !m->entry[0]) {
        fail("plugin.ini: kind %s needs entry", m->kind); ok = 0;
    }
    if (strpbrk(m->entry, "/\\:") || !strcmp(m->entry, "..") || !strcmp(m->entry, ".")) {
        fail("plugin.ini: entry must be a bare file name"); ok = 0;
    }
    return ok;
}

/* ---- files -------------------------------------------------------------- */

static int file_exists(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

static const char* base_name(const char* path) {
    const char* b = path;
    const char* p;
    for (p = path; *p; ++p)
        if ((*p == '/' || *p == '\\') && p[1]) b = p + 1;
    return b;
}

static int ends_with_nocase(const char* s, const char* ext) {
    size_t n = strlen(s), m = strlen(ext), i;
    if (n < m) return 0;
    for (i = 0; i < m; ++i)
        if (tolower((unsigned char)s[n - m + i]) != ext[i]) return 0;
    return 1;
}

/* Prints and counts the files under dir/sub ending in ext (recursive when deep). */
static int list_content(const char* dir, const char* sub, const char* ext, int deep) {
    char path[1024];
    int  count = 0;
    snprintf(path, sizeof path, "%s/%s", dir, sub);
#if defined(_WIN32)
    {
        WIN32_FIND_DATAA fd;
        char pattern[1100];
        HANDLE h;
        snprintf(pattern, sizeof pattern, "%s/*", path);
        if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE) return 0;
        do {
            char rel[1100];
            if (!strcmp(fd.cFileName, ".") || !strcmp(fd.cFileName, "..")) continue;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
            snprintf(rel, sizeof rel, "%s/%s", sub, fd.cFileName);
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (deep) count += list_content(dir, rel, ext, deep); }
            else if (ends_with_nocase(fd.cFileName, ext)) { printf("  content %s\n", rel); ++count; }
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
#else
    {
        DIR* d = opendir(path);
        struct dirent* e;
        if (!d) return 0;
        while ((e = readdir(d)) != NULL) {
            char rel[1100], full[1200];
            struct stat st;
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            snprintf(rel, sizeof rel, "%s/%s", sub, e->d_name);
            snprintf(full, sizeof full, "%s/%s", dir, rel);
            if (lstat(full, &st) != 0 || S_ISLNK(st.st_mode)) continue; /* the host skips symlinks */
            if (S_ISDIR(st.st_mode)) { if (deep) count += list_content(dir, rel, ext, deep); }
            else if (ends_with_nocase(e->d_name, ext)) { printf("  content %s\n", rel); ++count; }
        }
        closedir(d);
    }
#endif
    return count;
}

/* ---- stand-in host ------------------------------------------------------ */

#define MAX_SUBS 32
#define MAX_CMDS 64
#define MAX_TASKS 256
#define MAX_ARGS 16

typedef struct sub  { char event[64]; sco_event_fn fn; void* ctx; int live; } sub;
typedef struct task { sco_task_fn fn; void* ctx; } task;
typedef struct cmd {
    sco_command c;            /* strings point into the buffers below */
    sco_arg_def defs[MAX_ARGS];
    char name[64], title[64], help[256], cap[64];
    char def_name[MAX_ARGS][32], def_help[MAX_ARGS][128];
} cmd;

struct sco_plugin { int id; };

static sco_plugin the_plugin;
static char  plugin_id[64];
static const char* caps[32];
static int   ncaps;
static sub   subs[MAX_SUBS];
static cmd   cmds[MAX_CMDS];
static int   ncmds;
static task  tasks[MAX_TASKS];
static int   ntasks;

static int copy_str(char* dst, size_t cap, const char* src) {
    if (!src) { dst[0] = 0; return 1; }
    if (strlen(src) >= cap) return 0;
    strcpy(dst, src);
    return 1;
}

static void api_fail(const char* what) { fail("plugin called %s", what); }

static const char* host_version(void) { return "sco-plugin-check " "1.0"; }

static int has(const char* capability) {
    int i;
    if (!capability) return 0;
    for (i = 0; i < ncaps; ++i)
        if (!strcmp(caps[i], capability)) return 1;
    return 0;
}

static sco_result run_on_game_thread(sco_plugin* self, sco_task_fn fn, void* ctx) {
    if (self != &the_plugin || !fn) { api_fail("run_on_game_thread with a bad self or NULL fn"); return SCO_BAD_ARG; }
    if (ntasks == MAX_TASKS) return SCO_TOO_MANY;
    tasks[ntasks].fn = fn;
    tasks[ntasks].ctx = ctx;
    ++ntasks;
    return SCO_OK;
}

static sco_result subscribe(sco_plugin* self, const char* event, sco_event_fn fn, void* ctx) {
    int i;
    if (self != &the_plugin || !event || !*event || !fn || strlen(event) >= 64) {
        api_fail("subscribe with a bad self, event or fn");
        return SCO_BAD_ARG;
    }
    for (i = 0; i < MAX_SUBS; ++i)
        if (subs[i].live && subs[i].fn == fn && !strcmp(subs[i].event, event)) {
            api_fail("subscribe twice with the same event and fn");
            return SCO_BAD_ARG;
        }
    for (i = 0; i < MAX_SUBS; ++i)
        if (!subs[i].live) {
            strcpy(subs[i].event, event);
            subs[i].fn = fn;
            subs[i].ctx = ctx;
            subs[i].live = 1;
            return SCO_OK;
        }
    return SCO_TOO_MANY;
}

static sco_result unsubscribe(sco_plugin* self, const char* event, sco_event_fn fn) {
    int i;
    if (self != &the_plugin || !event || !fn) { api_fail("unsubscribe with a bad self, event or fn"); return SCO_BAD_ARG; }
    for (i = 0; i < MAX_SUBS; ++i)
        if (subs[i].live && subs[i].fn == fn && !strcmp(subs[i].event, event)) { subs[i].live = 0; return SCO_OK; }
    return SCO_NOT_FOUND;
}

static void status(sco_plugin* self, const char* message) {
    if (self != &the_plugin || !message) { api_fail("status with a bad self or NULL message"); return; }
    printf("  status  %s: %s\n", plugin_id, message);
}

static void log_(sco_plugin* self, sco_log_level level, const char* message) {
    static const char* names[] = { "info", "warn", "error" };
    if (self != &the_plugin || !message || (unsigned)level > SCO_LOG_ERROR) { api_fail("log with a bad self, level or message"); return; }
    printf("  log     [%s] %s: %s\n", plugin_id, names[level], message);
}

static sco_result register_command(sco_plugin* self, const sco_command* in) {
    cmd* c;
    size_t plen = strlen(plugin_id);
    uint32_t i;
    int j;
    if (self != &the_plugin || !in) { api_fail("register_command with a bad self or NULL cmd"); return SCO_BAD_ARG; }
    if (in->size < offsetof(sco_command, ctx) + sizeof(void*)) { api_fail("register_command with size too small"); return SCO_BAD_ARG; }
    if (!in->name || strncmp(in->name, plugin_id, plen) || in->name[plen] != '.' || !in->name[plen + 1]) {
        fail("register_command '%s': the name must start with '%s.'", in->name ? in->name : "(null)", plugin_id);
        return SCO_BAD_ARG;
    }
    for (j = 0; j < ncmds; ++j)
        if (!strcmp(cmds[j].name, in->name)) { fail("register_command '%s': duplicate", in->name); return SCO_BAD_ARG; }
    if (!in->title || !in->fn) { fail("register_command '%s': title and fn are required", in->name); return SCO_BAD_ARG; }
    if (in->nargs > MAX_ARGS || (in->nargs && (!in->args || in->arg_def_size < sizeof(sco_arg_def)))) {
        fail("register_command '%s': bad args, nargs or arg_def_size", in->name);
        return SCO_BAD_ARG;
    }
    if (ncmds == MAX_CMDS) return SCO_TOO_MANY;
    c = &cmds[ncmds];
    memset(c, 0, sizeof *c);
    if (!copy_str(c->name, sizeof c->name, in->name) || !copy_str(c->title, sizeof c->title, in->title) ||
        !copy_str(c->help, sizeof c->help, in->help) || !copy_str(c->cap, sizeof c->cap, in->capability)) {
        fail("register_command '%s': a string is too long (name/title/capability 63, help 255)", in->name);
        return SCO_BAD_ARG;
    }
    for (i = 0; i < in->nargs; ++i) {
        /* Read with the plugin's stride, as the real host does. */
        const sco_arg_def* d = (const sco_arg_def*)((const char*)in->args + (size_t)i * in->arg_def_size);
        if (!d->name || d->type > SCO_ARG_BOOL || !copy_str(c->def_name[i], 32, d->name) ||
            !copy_str(c->def_help[i], 128, d->help)) {
            fail("register_command '%s': arg %u has a bad name, type or help", in->name, (unsigned)i);
                return SCO_BAD_ARG;
        }
        c->defs[i].name = c->def_name[i];
        c->defs[i].type = d->type;
        c->defs[i].help = d->help ? c->def_help[i] : NULL;
    }
    c->c.size = sizeof(sco_command);
    c->c.name = c->name;
    c->c.title = c->title;
    c->c.help = in->help ? c->help : NULL;
    c->c.capability = in->capability ? c->cap : NULL;
    c->c.args = c->defs;
    c->c.nargs = in->nargs;
    c->c.arg_def_size = sizeof(sco_arg_def);
    c->c.fn = in->fn;
    c->c.ctx = in->ctx;
    ++ncmds;
    return SCO_OK;
}

static sco_result run_command(const cmd* c, const sco_arg* args, uint32_t nargs, char* reply, uint32_t reply_size) {
    uint32_t i;
    reply[0] = 0;
    if (nargs != c->c.nargs) return SCO_BAD_ARG;
    for (i = 0; i < nargs; ++i) {
        if (args[i].type != c->defs[i].type) return SCO_BAD_ARG;
        if (args[i].type == SCO_ARG_BOOL && args[i].v.i != 0 && args[i].v.i != 1) return SCO_BAD_ARG;
        if (args[i].type == SCO_ARG_STRING && !args[i].v.s) return SCO_BAD_ARG;
    }
    if (c->c.capability && !has(c->c.capability)) return SCO_UNAVAILABLE;
    {
        sco_result r = c->c.fn(args, nargs, c->c.ctx, reply, reply_size);
        reply[reply_size - 1] = 0; /* the host NUL-terminates whatever fn wrote */
        return r;
    }
}

static sco_result invoke(sco_plugin* self, const char* name, const sco_arg* args, uint32_t nargs,
                         sco_invoke_done done, void* ctx) {
    char reply[256];
    sco_result r = SCO_NOT_FOUND;
    int i;
    if (self != &the_plugin || !name || (nargs && !args)) { api_fail("invoke with a bad self, name or args"); return SCO_BAD_ARG; }
    for (i = 0; i < ncmds; ++i)
        if (!strcmp(cmds[i].name, name)) { r = run_command(&cmds[i], args, nargs, reply, sizeof reply); break; }
    if (i == ncmds) reply[0] = 0;
    if (done) done(r, reply, ctx);
    return r;
}

static uint32_t list_commands(const sco_command** out, uint32_t max) {
    uint32_t i;
    for (i = 0; i < (uint32_t)ncmds && i < max; ++i) out[i] = &cmds[i].c;
    return (uint32_t)ncmds;
}

static void dispatch(const char* event, const void* data) {
    int i;
    for (i = 0; i < MAX_SUBS; ++i)
        if (subs[i].live && !strcmp(subs[i].event, event)) subs[i].fn(event, data, subs[i].ctx);
}

static void tick(uint32_t now_ms) {
    int i, n = ntasks;
    for (i = 0; i < n; ++i) tasks[i].fn(tasks[i].ctx);
    memmove(tasks, tasks + n, (size_t)(ntasks - n) * sizeof(task));
    ntasks -= n;
    dispatch("tick", &now_ms);
}

static const char* result_name(sco_result r) {
    switch (r) {
        case SCO_OK: return "ok";
        case SCO_UNAVAILABLE: return "unavailable";
        case SCO_NOT_FOUND: return "not_found";
        case SCO_BAD_ARG: return "bad_arg";
        case SCO_CRASHED: return "crashed";
        case SCO_WRONG_THREAD: return "wrong_thread";
        case SCO_TOO_MANY: return "too_many";
        default: return "?";
    }
}

/* Parses text as the type the command's arg def wants. */
static int parse_arg(const char* text, uint32_t type, sco_arg* out) {
    char* end;
    memset(out, 0, sizeof *out);
    out->type = type;
    switch (type) {
        case SCO_ARG_INT:    out->v.i = strtoll(text, &end, 10); return *text && !*end;
        case SCO_ARG_FLOAT:  out->v.f = strtod(text, &end);       return *text && !*end;
        case SCO_ARG_STRING: out->v.s = text;                     return 1;
        case SCO_ARG_BOOL:
            if (!strcmp(text, "1") || !strcmp(text, "true"))  { out->v.i = 1; return 1; }
            if (!strcmp(text, "0") || !strcmp(text, "false")) { out->v.i = 0; return 1; }
            return 0;
        default: return 0;
    }
}

static void check_native(const char* dir, const manifest* m, const char* invoke_name,
                         char** invoke_args, int ninvoke_args) {
    char path[1024];
    const sco_plugin_info* info;
    sco_plugin_query_fn  query;
    sco_plugin_load_fn   load;
    sco_plugin_unload_fn unload;
    sco_api api;
    sco_result r;
    int i;
#if defined(_WIN32)
    HMODULE mod;
#else
    void* mod;
#endif

    snprintf(path, sizeof path, "%s/%s", dir, m->entry);
#if defined(_WIN32)
    {
        /* LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR needs a full path, as the host passes. */
        char full[MAX_PATH];
        if (GetFullPathNameA(path, sizeof full, full, NULL) - 1u >= sizeof full - 1u) { fail("%s: path too long", path); return; }
        strcpy(path, full);
    }
    mod = LoadLibraryExA(path, NULL, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!mod) { fail("%s: can't load (error %lu)", path, (unsigned long)GetLastError()); return; }
#define SYM(name) ((void*)GetProcAddress(mod, name))
#else
    mod = dlopen(path, RTLD_NOW | RTLD_LOCAL);
    if (!mod) { fail("%s: can't load (%s)", path, dlerror()); return; }
#define SYM(name) dlsym(mod, name)
#endif
    /* Object to function pointer: same size on every platform the SDK targets (abi_v1.c). */
    {
        void* q = SYM("sco_plugin_query");
        void* l = SYM("sco_plugin_load");
        void* u = SYM("sco_plugin_unload");
        memcpy((void*)&query, &q, sizeof query); /* cast: MSVC C4090 misreads the const in the return type */
        memcpy(&load, &l, sizeof load);
        memcpy(&unload, &u, sizeof unload);
    }
#undef SYM
    if (!query || !load || !unload) {
        fail("%s: missing export (needs sco_plugin_query, sco_plugin_load, sco_plugin_unload)", m->entry);
        return;
    }

    info = query();
    if (!info) { fail("sco_plugin_query returned NULL"); return; }
    if (info->size < offsetof(sco_plugin_info, author) + sizeof(void*)) { fail("sco_plugin_info.size too small"); return; }
    if (info->api_major != SCO_API_MAJOR || info->api_minor > SCO_API_MINOR) {
        fail("built for api %u.%u; this SDK is %d.%d", info->api_major, info->api_minor, SCO_API_MAJOR, SCO_API_MINOR);
        return;
    }
    if (!info->name || strcmp(info->name, m->id)) {
        fail("sco_plugin_info.name '%s' does not match id '%s'", info->name ? info->name : "(null)", m->id);
        return;
    }
    printf("  query   %s %s by %s (api %u.%u)\n", info->name, info->version ? info->version : "?",
           info->author ? info->author : "?", info->api_major, info->api_minor);

    memset(&api, 0, sizeof api);
    api.size = sizeof api;
    api.major = SCO_API_MAJOR;
    api.minor = SCO_API_MINOR;
    api.host_version = host_version;
    api.has = has;
    api.run_on_game_thread = run_on_game_thread;
    api.subscribe = subscribe;
    api.unsubscribe = unsubscribe;
    api.status = status;
    api.log = log_;
    api.register_command = register_command;
    api.invoke = invoke;
    api.list_commands = list_commands;

    r = load(&api, &the_plugin);
    if (r != SCO_OK) { fail("sco_plugin_load returned %s", result_name(r)); unload(); return; }
    printf("  load    ok: %d command(s)\n", ncmds);

    dispatch("game.ready", NULL);
    for (i = 1; i <= 3; ++i) tick((uint32_t)(i * 100));

    for (i = 0; i < ncmds; ++i) {
        char reply[256];
        sco_arg args[MAX_ARGS];
        const cmd* c = &cmds[i];
        printf("  command %s \"%s\" (%u arg%s%s%s)\n", c->name, c->title, (unsigned)c->c.nargs,
               c->c.nargs == 1 ? "" : "s", c->c.capability ? ", needs " : "", c->c.capability ? c->cap : "");
        if (invoke_name && !strcmp(invoke_name, c->name)) {
            int k;
            if ((uint32_t)ninvoke_args != c->c.nargs) { fail("--invoke %s: needs %u argument(s)", c->name, (unsigned)c->c.nargs); continue; }
            for (k = 0; k < ninvoke_args; ++k)
                if (!parse_arg(invoke_args[k], c->defs[k].type, &args[k])) { fail("--invoke %s: bad argument '%s'", c->name, invoke_args[k]); break; }
            if (k < ninvoke_args) continue;
        } else if (c->c.nargs) {
            continue;  /* needs arguments: run it with --invoke */
        }
        r = run_command(c, args, c->c.nargs, reply, sizeof reply);
        printf("  invoke  %s -> %s \"%s\"\n", c->name, result_name(r), reply);
        if (r != SCO_OK && r != SCO_UNAVAILABLE) fail("%s returned %s", c->name, result_name(r));
    }
    if (invoke_name) {
        for (i = 0; i < ncmds; ++i) if (!strcmp(cmds[i].name, invoke_name)) break;
        if (i == ncmds) fail("--invoke %s: no such command", invoke_name);
    }

    dispatch("game.exit", NULL);
    unload();
    printf("  unload  ok\n");
    /* Not closed: a check run ends here, and some C runtimes dislike unloading mid-exit. */
}

int main(int argc, char** argv) {
    const char* dir = NULL;
    const char* invoke_name = NULL;
    char** invoke_args = NULL;
    int ninvoke_args = 0, i;
    char path[1024], folder[256];
    manifest m;

    for (i = 1; i < argc; ++i) {
        if (!strcmp(argv[i], "--cap") && i + 1 < argc && ncaps < 32) caps[ncaps++] = argv[++i];
        else if (!strcmp(argv[i], "--invoke") && i + 1 < argc) {
            invoke_name = argv[++i];
            invoke_args = argv + i + 1;
            ninvoke_args = argc - i - 1;
            break;
        } else if (!dir && argv[i][0] != '-') dir = argv[i];
        else { dir = NULL; break; }
    }
    if (!dir) {
        fprintf(stderr, "usage: sco-plugin-check <plugin folder> [--cap NAME]... [--invoke NAME [ARG]...]\n");
        return 2;
    }

    snprintf(folder, sizeof folder, "%s", base_name(dir));
    for (i = (int)strlen(folder); i > 0 && (folder[i - 1] == '/' || folder[i - 1] == '\\'); --i) folder[i - 1] = 0;
    printf("%s\n", dir);
    snprintf(path, sizeof path, "%s/plugin.ini", dir);
    if (read_manifest(path, &m)) {
        strcpy(plugin_id, m.id);
        printf("  ini     id=%s kind=%s api=%s%s%s\n", m.id, m.kind, m.api, m.entry[0] ? " entry=" : "", m.entry);
        if (strcmp(folder, m.id)) fail("id '%s' does not match folder '%s'", m.id, folder);
        for (i = 0; i < m.nrequires; ++i)
            printf("  needs   %s%s\n", m.requires_[i], has(m.requires_[i]) ? "" : " (missing: the host won't load it)");
        if (m.entry[0]) {
            snprintf(path, sizeof path, "%s/%s", dir, m.entry);
            if (!file_exists(path)) fail("entry '%s' not found", m.entry);
        }
        if (!failures && !strcmp(m.kind, "data")) {
            int n = list_content(dir, "missions", ".cwmission", 0) + list_content(dir, "rules", ".rules", 0) +
                    list_content(dir, "scripts", ".xml", 1) + list_content(dir, "lists", ".txt", 0);
            if (n == 0) fail("data pack has no content (missions/*.cwmission, rules/*.rules, scripts/**.xml, lists/*.txt)");
            else printf("  pack    %d file(s)\n", n);
        } else if (!failures && !strcmp(m.kind, "native")) {
            check_native(dir, &m, invoke_name, invoke_args, ninvoke_args);
        }
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
