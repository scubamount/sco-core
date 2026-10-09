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
#define MAX_MANIFEST_BYTES (16 * 1024)
#define MAX_PACK_FILES 4096

typedef struct manifest {
    char id[64], name[128], version[64], author[128], api[16], kind[16], entry[128];
    char requires_[MAX_REQUIRES][64];
    int  nrequires;
    int  api_major, api_minor;
} manifest;

/* The rules below are the host's (src/plugins/manifest.cpp), one for one: a manifest passes here
 * exactly when the game would read it. Text is a pointer and a length, like the host's
 * string_view, so long lines and NUL bytes are handled the same way. */
typedef struct strv { const char* p; size_t n; } strv;

static strv trim(strv s) {
    while (s.n && (s.p[0] == ' ' || s.p[0] == '\t')) { ++s.p; --s.n; }
    while (s.n && (s.p[s.n - 1] == ' ' || s.p[s.n - 1] == '\t' || s.p[s.n - 1] == '\r')) --s.n;
    return s;
}

/* A comment starts with ';' or '#' at the start of the line or after a space or tab. */
static strv strip_comment(strv s) {
    size_t i;
    for (i = 0; i < s.n; ++i)
        if ((s.p[i] == ';' || s.p[i] == '#') && (i == 0 || s.p[i - 1] == ' ' || s.p[i - 1] == '\t')) {
            s.n = i;
            break;
        }
    return s;
}

static int is(strv s, const char* lit) { return s.n == strlen(lit) && memcmp(s.p, lit, s.n) == 0; }

static int id_char(char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }

static int valid_id(strv s) {
    size_t i;
    if (s.n < 1 || s.n > 31) return 0;
    for (i = 0; i < s.n; ++i) if (!id_char(s.p[i])) return 0;
    return 1;
}

static int reserved_id(strv s) { return is(s, "sco") || is(s, "host") || is(s, "menu") || is(s, "game"); }

/* Lowercase dotted: "teleport", "spawn.ship"; no empty part. */
static int valid_capability(strv s) {
    size_t i;
    char prev = 0;
    if (s.n < 1 || s.n > 63 || s.p[0] == '.' || s.p[s.n - 1] == '.') return 0;
    for (i = 0; i < s.n; ++i) {
        if (s.p[i] == '.') { if (prev == '.') return 0; }
        else if (!id_char(s.p[i])) return 0;
        prev = s.p[i];
    }
    return 1;
}

/* 1..max bytes, printable ASCII and UTF-8 bytes; no control characters. */
static int valid_text(strv s, size_t max) {
    size_t i;
    if (s.n < 1 || s.n > max) return 0;
    for (i = 0; i < s.n; ++i) {
        const unsigned char c = (unsigned char)s.p[i];
        if (c < 0x20 || c == 0x7f) return 0;
    }
    return 1;
}

/* A bare file name: no separators, no drive, no wildcard or reserved character, not "." or "..". */
static int valid_entry(strv s) {
    size_t i;
    if (s.n < 1 || s.n > 63 || is(s, ".") || is(s, "..")) return 0;
    for (i = 0; i < s.n; ++i) {
        const unsigned char c = (unsigned char)s.p[i];
        if (c < 0x20 || strchr("/\\:*?\"<>|", c)) return 0;
    }
    return 1;
}

/* "<major>.<minor>": digits only, at most 5 each, each at most 65535. */
static int parse_number(strv d, int* out) {
    size_t i;
    long v = 0;
    if (d.n < 1 || d.n > 5) return 0;
    for (i = 0; i < d.n; ++i) {
        if (d.p[i] < '0' || d.p[i] > '9') return 0;
        v = v * 10 + (d.p[i] - '0');
    }
    if (v > 0xffff) return 0;
    *out = (int)v;
    return 1;
}

static int parse_api(strv s, int* major, int* minor) {
    const char* dot = s.n ? (const char*)memchr(s.p, '.', s.n) : NULL;
    strv a, b;
    if (!dot) return 0;
    a.p = s.p; a.n = (size_t)(dot - s.p);
    b.p = dot + 1; b.n = s.n - a.n - 1;
    return parse_number(a, major) && parse_number(b, minor);
}

static void set_field(char* dst, strv v) {   /* v fits: its length was checked */
    memcpy(dst, v.p, v.n);
    dst[v.n] = 0;
}

static int bad_line(int line, const char* what) { fail("plugin.ini line %d: %s", line, what); return 0; }

static int read_manifest(const char* path, manifest* m) {
    enum { K_ID, K_NAME, K_VERSION, K_AUTHOR, K_API, K_KIND, K_ENTRY, K_REQUIRES, K_COUNT };
    static const char* const keys[K_COUNT] = { "id", "name", "version", "author", "api", "kind", "entry", "requires" };
    static char buf[MAX_MANIFEST_BYTES + 1];
    int seen[K_COUNT] = { 0 };
    int line = 0, k, i;
    size_t len;
    strv all;
    FILE* f = fopen(path, "rb");
    if (!f) { fail("%s: can't open", path); return 0; }
    len = fread(buf, 1, sizeof buf, f);
    fclose(f);
    memset(m, 0, sizeof *m);
    if (len > MAX_MANIFEST_BYTES) { fail("plugin.ini: too big (over %d bytes)", MAX_MANIFEST_BYTES); return 0; }
    all.p = buf;
    all.n = len;
    if (all.n >= 3 && memcmp(all.p, "\xEF\xBB\xBF", 3) == 0) { all.p += 3; all.n -= 3; }

    while (all.n) {
        const char* nl = (const char*)memchr(all.p, '\n', all.n);
        const char* eq;
        strv ln, key, val;
        ln.p = all.p;
        ln.n = nl ? (size_t)(nl - all.p) : all.n;
        all.p += ln.n + (nl ? 1 : 0);
        all.n -= ln.n + (nl ? 1 : 0);
        ++line;
        ln = trim(strip_comment(ln));
        if (!ln.n) continue;
        eq = (const char*)memchr(ln.p, '=', ln.n);
        if (!eq) return bad_line(line, "expected key = value");
        key.p = ln.p; key.n = (size_t)(eq - ln.p);
        val.p = eq + 1; val.n = ln.n - key.n - 1;
        key = trim(key);
        val = trim(val);
        for (k = 0; k < K_COUNT && !is(key, keys[k]); ++k) {}
        if (k == K_COUNT) continue;   /* unknown key, even twice: a later minor's */
        if (seen[k]) { fail("plugin.ini line %d: duplicate key '%s'", line, keys[k]); return 0; }
        seen[k] = 1;
        switch (k) {
            case K_ID:
                if (!valid_id(val)) return bad_line(line, "id must be 1-31 of [a-z0-9_]");
                if (reserved_id(val)) return bad_line(line, "id is reserved");
                set_field(m->id, val);
                break;
            case K_NAME:
                if (!valid_text(val, 63)) return bad_line(line, "name must be 1-63 printable characters");
                set_field(m->name, val);
                break;
            case K_VERSION:
                if (!valid_text(val, 31)) return bad_line(line, "version must be 1-31 printable characters");
                set_field(m->version, val);
                break;
            case K_AUTHOR:
                if (!valid_text(val, 63)) return bad_line(line, "author must be 1-63 printable characters");
                set_field(m->author, val);
                break;
            case K_API:
                if (!parse_api(val, &m->api_major, &m->api_minor)) return bad_line(line, "api must be <major>.<minor>");
                set_field(m->api, val);
                break;
            case K_KIND:
                if (!is(val, "native") && !is(val, "lua") && !is(val, "data")) return bad_line(line, "kind must be native, lua or data");
                set_field(m->kind, val);
                break;
            case K_ENTRY:
                if (!valid_entry(val)) return bad_line(line, "entry must be a file name in the plugin folder");
                set_field(m->entry, val);
                break;
            default: {   /* K_REQUIRES */
                strv rest = val;
                for (;;) {
                    const char* comma = rest.n ? (const char*)memchr(rest.p, ',', rest.n) : NULL;
                    strv cap;
                    cap.p = rest.p;
                    cap.n = comma ? (size_t)(comma - rest.p) : rest.n;
                    cap = trim(cap);
                    if (!valid_capability(cap)) return bad_line(line, "requires must be a comma list of capability names");
                    for (i = 0; i < m->nrequires; ++i)
                        if (is(cap, m->requires_[i])) return bad_line(line, "requires lists a capability twice");
                    if (m->nrequires == MAX_REQUIRES) return bad_line(line, "requires lists more than 16 capabilities");
                    set_field(m->requires_[m->nrequires++], cap);
                    if (!comma) break;
                    rest.n -= (size_t)(comma + 1 - rest.p);
                    rest.p = comma + 1;
                }
                break;
            }
        }
    }

    {
        static const int required[] = { K_ID, K_NAME, K_VERSION, K_API, K_KIND };
        for (i = 0; i < 5; ++i)
            if (!seen[required[i]]) { fail("plugin.ini: missing key '%s'", keys[required[i]]); return 0; }
    }
    if (!strcmp(m->kind, "data") && seen[K_ENTRY]) { fail("plugin.ini: a data pack has no entry"); return 0; }
    if (strcmp(m->kind, "data") && !seen[K_ENTRY]) { fail("plugin.ini: missing key 'entry'"); return 0; }
    /* What discovery checks next (src/plugins/discover.cpp). */
    if (m->api_major != SCO_API_MAJOR || m->api_minor > SCO_API_MINOR) {
        fail("plugin.ini: api %s, but this SDK is %d.%d", m->api, SCO_API_MAJOR, SCO_API_MINOR);
        return 0;
    }
    return 1;
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
#define MAX_DEPTH 8   /* nested invokes, as in sco-lua */

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

/* "<x>.<y>": lowercase letters, digits, '_' and '.'; no empty part; at most 63 bytes. */
static int valid_command_name(const char* n) {
    int dot = 0;
    char prev = '.';
    const char* p;
    if (!n || !*n || strlen(n) > 63) return 0;
    for (p = n; *p; ++p) {
        if (*p == '.') { if (prev == '.') return 0; dot = 1; }
        else if (!id_char(*p)) return 0;
        prev = *p;
    }
    return dot && prev != '.';
}

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
    /* The host's rules (src/host/sco_host.cpp RegisterCommandC, src/api/sco_commands.cpp
     * RegisterCommand): title, help and capability are optional, args are read with the plugin's
     * arg_def_size, which must be a multiple of sco_arg_def's alignment. */
    if (self != &the_plugin || !in) { api_fail("register_command with a bad self or NULL cmd"); return SCO_BAD_ARG; }
    if (in->size < sizeof(sco_command)) { api_fail("register_command with size too small"); return SCO_BAD_ARG; }
    if (!valid_command_name(in->name) || strncmp(in->name, plugin_id, plen) || in->name[plen] != '.') {
        fail("register_command '%s': the name must be '%s.<action>' (a-z 0-9 _ and dots, at most 63 bytes)",
             in->name ? in->name : "(null)", plugin_id);
        return SCO_BAD_ARG;
    }
    for (j = 0; j < ncmds; ++j)
        if (!strcmp(cmds[j].name, in->name)) { fail("register_command '%s': duplicate", in->name); return SCO_BAD_ARG; }
    if (!in->fn) { fail("register_command '%s': fn is required", in->name); return SCO_BAD_ARG; }
    if (in->nargs > MAX_ARGS || (in->nargs && (!in->args || in->arg_def_size < sizeof(sco_arg_def) ||
                                               in->arg_def_size % _Alignof(sco_arg_def)))) {
        fail("register_command '%s': bad args, nargs or arg_def_size", in->name);
        return SCO_BAD_ARG;
    }
    if (ncmds == MAX_CMDS) return SCO_TOO_MANY;
    c = &cmds[ncmds];
    memset(c, 0, sizeof *c);
    if (!copy_str(c->name, sizeof c->name, in->name) || !copy_str(c->title, sizeof c->title, in->title) ||
        !copy_str(c->help, sizeof c->help, in->help) || !copy_str(c->cap, sizeof c->cap, in->capability)) {
        fail("register_command '%s': a string is too long (title/capability 63, help 255)", in->name);
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
    c->c.title = in->title ? c->title : NULL;
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
    static int depth;   /* a command invoking itself would otherwise overflow the stack */
    char reply[256];
    sco_result r = SCO_NOT_FOUND;
    int i;
    if (self != &the_plugin || !name || (nargs && !args)) { api_fail("invoke with a bad self, name or args"); return SCO_BAD_ARG; }
    for (i = 0; i < ncmds; ++i)
        if (!strcmp(cmds[i].name, name)) {
            if (depth >= MAX_DEPTH) {
                snprintf(reply, sizeof reply, "calls nested too deep");
                r = SCO_TOO_MANY;
            } else {
                ++depth;
                r = run_command(&cmds[i], args, nargs, reply, sizeof reply);
                --depth;
            }
            break;
        }
    if (i == ncmds) reply[0] = 0;
    if (done) done(r, reply, ctx);
    return r;
}

static uint32_t list_commands(const sco_command** out, uint32_t max) {
    uint32_t i;
    for (i = 0; i < (uint32_t)ncmds && i < max; ++i) out[i] = &cmds[i].c;
    return (uint32_t)ncmds;
}

/* 1.1. The checker loads one plugin, so services and raw handlers are only listed; nothing
 * can be found or invoked. */
static sco_result provide_service(sco_plugin* self, const char* name, uint32_t version, const void* vtable) {
    (void)self;
    if (!name || !vtable) return SCO_BAD_ARG;
    printf("  service %s %u.%u\n", name, version >> 16, version & 0xFFFFu);
    return SCO_OK;
}

static sco_result query_service(const char* name, uint32_t min_version, const void** out) {
    (void)min_version;
    if (out) *out = NULL;
    return name && out ? SCO_NOT_FOUND : SCO_BAD_ARG;
}

static sco_result release_service(sco_plugin* self, const char* name) {
    (void)self;
    return name ? SCO_OK : SCO_BAD_ARG;
}

static sco_result invoke_raw(sco_plugin* self, const char* name, const void* in, uint32_t in_size,
                             void* out, uint32_t* out_size) {
    (void)self; (void)in; (void)in_size; (void)out;
    if (out_size) *out_size = 0;
    return name ? SCO_NOT_FOUND : SCO_BAD_ARG;
}

static sco_result register_raw(sco_plugin* self, const char* name, const char* capability, sco_raw_fn fn, void* ctx) {
    (void)self; (void)ctx;
    if (!name || !fn) return SCO_BAD_ARG;
    printf("  raw     %s%s%s\n", name, capability ? " needs " : "", capability ? capability : "");
    return SCO_OK;
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
        case SCO_FAILED: return "failed";
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
    api.provide_service = provide_service;
    api.query_service = query_service;
    api.release_service = release_service;
    api.invoke_raw = invoke_raw;
    api.register_raw = register_raw;

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
            /* .toml files in datacore: indexed here; their syntax and names are checked by sco-dcb lint and check. */
            int dcb = list_content(dir, "datacore", ".toml", 0);
            if (dcb) printf("  datacore %d file(s) indexed; check them with sco-dcb lint (syntax) and sco-dcb check (against Game2.dcb)\n", dcb);
            n += dcb;
            if (n == 0) fail("data pack has no content (missions/*.cwmission, rules/*.rules, scripts/**.xml, lists/*.txt, datacore/*.toml)");
            else if (n > MAX_PACK_FILES) fail("data pack has %d content files; the host refuses more than %d", n, MAX_PACK_FILES);
            else printf("  pack    %d file(s)\n", n);
        } else if (!failures && !strcmp(m.kind, "native")) {
            check_native(dir, &m, invoke_name, invoke_args, ninvoke_args);
        }
    }
    printf("%s: %d failure(s)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
