/*
 * sco-plugin-check: checks a built plugin folder on a dev machine, without the game.
 *
 *   sco-plugin-check <plugin folder> [--cap NAME]... [--short-tables] [--invoke NAME [ARG]...]
 *
 * For every kind it reads plugin.ini with the same rules as the host (id, name, version, api,
 * kind, entry, requires and the [settings] section) and checks the folder name matches the id. A requires name is a
 * capability, a service ("<plugin id>.<name>", "sco.storage", "game.vehicles") or a plugin id: the
 * host loads the plugin after the plugins those name and refuses cycles and missing providers; this
 * tool only checks the syntax and that the plugin doesn't require itself.
 *   data:   lists the files the host would index (missions/, rules/, scripts/, lists/).
 *   lua:    checks the entry script exists (syntax is checked separately with luac -p).
 *   native: loads the plugin, runs sco_plugin_query and sco_plugin_load against a stand-in
 *           sco_api, fires game.ready and a few ticks, runs every command that takes no
 *           arguments (and the one given with --invoke), then sco_plugin_unload. Retained
 *           sco.widgets panels get one deterministic headless interaction pass before commands.
 * --cap NAME makes has(NAME) answer 1 (default: only the built-in sco.widgets service is present). Give a --cap for a
 * capability or a host or game service the plugin requires; a plugin id or another plugin's
 * service is only found by the real host, which sees the other plugins.
 * --short-tables serves sco_api at its 1.0 extent (size = offsetof(sco_api, provide_service), minor 0),
 * as a 1.0 host does: calling a 1.1 member (provide_service .. register_raw) without first checking
 * size >= offsetof(sco_api, member) + sizeof(member) fails the check (docs/contracts.md, size / since).
 * sco.widgets is served at 1.1 by default (update_nodes, remove_nodes, capabilities, 1.1 node tails)
 * and at its 1.0 extent (size 24, tails ignored) under --short-tables, where calling a 1.1 member
 * without checking the widgets table's size fails the same way. An unchecked query_service under
 * --short-tables fails the check but still answers, so later unchecked widgets calls are reported too.
 *
 * release_service withdraws one of the plugin's own services; there is no reference to release for a
 * table it got from query_service. A release_service for a name the plugin does not provide at that
 * moment (a queried service, one never provided, or one already withdrawn) fails the check and gets
 * SCO_NOT_FOUND, the real host's answer.
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
#include <math.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sco_api.h"
#include "sco_widgets.h"

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
#define MAX_SETTINGS 32
#define MAX_PACK_FILES 4096

/* One [settings] declaration, as far as the checker reports it. */
typedef struct setting { char name[32]; char type[16]; } setting;

typedef struct manifest {
    char id[64], name[128], version[64], author[128], api[16], kind[16], entry[128];
    char requires_[MAX_REQUIRES][64];
    int  nrequires;
    int  api_major, api_minor;
    setting settings[MAX_SETTINGS];
    int  nsettings;
} manifest;

/* The rules below are the host's (src/plugins/manifest.cpp, settings_ini.cpp), one for one: a manifest passes here
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

/* ---- plugin.ini [settings] (the host's src/plugins/settings_ini.cpp, one for one) ------------- */

#define MAX_CHOICES 16

typedef struct sdecl {
    int       type;   /* 0 bool, 1 int, 2 float, 3 string, 4 enum */
    int       has_min, has_max;
    long long min_i, max_i;
    double    min_f, max_f;
    char      choices[MAX_CHOICES][32];
    int       nchoices;
} sdecl;

typedef struct tok { strv t; int quoted; } tok;
typedef struct lexer { strv s; size_t p; int bad; } lexer;

static int is_space(char c) { return c == ' ' || c == '\t'; }

/* A comment starts at ';' or '#' outside double quotes, at the start or after a space or tab. */
static strv strip_settings_comment(strv s) {
    size_t i;
    int quoted = 0;
    for (i = 0; i < s.n; ++i) {
        const char c = s.p[i];
        if (c == '"') quoted = !quoted;
        else if (!quoted && (c == ';' || c == '#') && (i == 0 || is_space(s.p[i - 1]))) { s.n = i; break; }
    }
    return s;
}

static int valid_word(strv s, size_t max) {
    size_t i;
    if (s.n < 1 || s.n > max) return 0;
    for (i = 0; i < s.n; ++i) if (!id_char(s.p[i])) return 0;
    return 1;
}

static int lex_next(lexer* l, tok* t) {
    size_t b;
    while (l->p < l->s.n && is_space(l->s.p[l->p])) ++l->p;
    if (l->p >= l->s.n) return 0;
    if (l->s.p[l->p] == '"') {
        const char* q = (const char*)memchr(l->s.p + l->p + 1, '"', l->s.n - l->p - 1);
        if (!q) { l->bad = 1; return 0; }
        t->t.p = l->s.p + l->p + 1;
        t->t.n = (size_t)(q - t->t.p);
        t->quoted = 1;
        l->p = (size_t)(q - l->s.p) + 1;
        if (l->p < l->s.n && !is_space(l->s.p[l->p])) { l->bad = 1; return 0; }
        return 1;
    }
    b = l->p;
    while (l->p < l->s.n && !is_space(l->s.p[l->p])) {
        if (l->s.p[l->p] == '"') { l->bad = 1; return 0; }
        ++l->p;
    }
    t->t.p = l->s.p + b;
    t->t.n = l->p - b;
    t->quoted = 0;
    return 1;
}

/* [+-]digits, at most 19 digits, in int64 range. */
static int parse_i64(strv s, long long* out) {
    size_t i = 0;
    int neg = 0;
    unsigned long long v = 0;
    if (s.n && (s.p[0] == '+' || s.p[0] == '-')) { neg = s.p[0] == '-'; i = 1; }
    if (i == s.n || s.n - i > 19) return 0;
    for (; i < s.n; ++i) {
        if (s.p[i] < '0' || s.p[i] > '9') return 0;
        v = v * 10 + (unsigned long long)(s.p[i] - '0');
    }
    if (neg) {
        if (v > 9223372036854775808ULL) return 0;
        *out = v == 9223372036854775808ULL ? (-9223372036854775807LL - 1) : -(long long)v;
    } else {
        if (v > 9223372036854775807ULL) return 0;
        *out = (long long)v;
    }
    return 1;
}

/* [+-]digits[.digits][(e|E)[+-]digits], finite. No nan, inf, hex or leading/trailing dot. */
static int parse_f64(strv s, double* out) {
    size_t i = 0, d;
    char buf[72], *end = NULL;
    double v;
    if (s.n > 64) return 0;
    if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) ++i;
    d = i;
    while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') ++i;
    if (i == d) return 0;
    if (i < s.n && s.p[i] == '.') {
        size_t f;
        ++i;
        f = i;
        while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') ++i;
        if (i == f) return 0;
    }
    if (i < s.n && (s.p[i] == 'e' || s.p[i] == 'E')) {
        size_t e;
        ++i;
        if (i < s.n && (s.p[i] == '+' || s.p[i] == '-')) ++i;
        e = i;
        while (i < s.n && s.p[i] >= '0' && s.p[i] <= '9') ++i;
        if (i == e) return 0;
    }
    if (i != s.n) return 0;
    memcpy(buf, s.p, s.n);
    buf[s.n] = 0;
    v = strtod(buf, &end);
    if (end != buf + s.n || !isfinite(v)) return 0;
    *out = v;
    return 1;
}

/* 1 when text is a value of d (type, range, choice); else 0 and why. */
static int check_setting(const sdecl* d, strv text, char* why, size_t cap) {
    long long i = 0;
    double f = 0;
    int k;
    switch (d->type) {
        case 0:
            if (is(text, "true") || is(text, "false")) return 1;
            snprintf(why, cap, "'%.*s' is not true or false", (int)text.n, text.p);
            return 0;
        case 1:
            if (!parse_i64(text, &i)) { snprintf(why, cap, "'%.*s' is not an integer", (int)text.n, text.p); return 0; }
            if (d->has_min && i < d->min_i) { snprintf(why, cap, "%lld is below min %lld", i, d->min_i); return 0; }
            if (d->has_max && i > d->max_i) { snprintf(why, cap, "%lld is above max %lld", i, d->max_i); return 0; }
            return 1;
        case 2:
            if (!parse_f64(text, &f)) { snprintf(why, cap, "'%.*s' is not a number", (int)text.n, text.p); return 0; }
            if (d->has_min && f < d->min_f) { snprintf(why, cap, "%.17g is below min %.17g", f, d->min_f); return 0; }
            if (d->has_max && f > d->max_f) { snprintf(why, cap, "%.17g is above max %.17g", f, d->max_f); return 0; }
            return 1;
        case 3: {
            size_t n;
            if (text.n > 255) { snprintf(why, cap, "a string is at most 255 printable characters"); return 0; }
            for (n = 0; n < text.n; ++n)
                if ((unsigned char)text.p[n] < 0x20 || (unsigned char)text.p[n] == 0x7f) {
                    snprintf(why, cap, "a string is at most 255 printable characters");
                    return 0;
                }
            return 1;
        }
        default: {
            size_t n;
            for (k = 0; k < d->nchoices; ++k) if (is(text, d->choices[k])) return 1;
            n = (size_t)snprintf(why, cap, "'%.*s' is not one of ", (int)text.n, text.p);
            for (k = 0; k < d->nchoices && n < cap; ++k)
                n += (size_t)snprintf(why + n, cap - n, "%s%s", k ? ", " : "", d->choices[k]);
            return 0;
        }
    }
}

static int setting_bad(int line, strv name, const char* what, const char* detail) {
    fail("plugin.ini line %d: setting '%.*s': %s%s", line, (int)name.n, name.p, what, detail ? detail : "");
    return 0;
}

/* Parses one declaration (comment stripped, trimmed) into m. 0 after reporting why. */
static int setting_line(strv line, manifest* m, int lineno) {
    static const char* const fields[5] = { "default", "min", "max", "label", "help" };
    enum { F_DEFAULT, F_MIN, F_MAX, F_LABEL, F_HELP };
    const char* eq = line.n ? (const char*)memchr(line.p, '=', line.n) : NULL;
    strv name, type, value[5];
    int have[5] = { 0 }, f, i;
    char why[512];
    sdecl d;
    lexer lx;
    tok key, v;
    if (!eq) return bad_line(lineno, "expected name = type ...");
    name.p = line.p;
    name.n = (size_t)(eq - line.p);
    name = trim(name);
    if (!valid_word(name, 31)) return bad_line(lineno, "setting name must be 1-31 of [a-z0-9_]");
    for (i = 0; i < m->nsettings; ++i)
        if (is(name, m->settings[i].name)) return setting_bad(lineno, name, "declared twice", NULL);
    if (m->nsettings == MAX_SETTINGS) return setting_bad(lineno, name, "more than 32 settings", NULL);

    memset(&d, 0, sizeof d);
    memset(value, 0, sizeof value);
    memset(&key, 0, sizeof key);
    memset(&v, 0, sizeof v);
    lx.s.p = eq + 1;
    lx.s.n = line.n - (size_t)(eq + 1 - line.p);
    lx.p = 0;
    lx.bad = 0;
    while (lx.p < lx.s.n && is_space(lx.s.p[lx.p])) ++lx.p;
    if (lx.s.n - lx.p >= 5 && memcmp(lx.s.p + lx.p, "enum(", 5) == 0) {
        const char* close = (const char*)memchr(lx.s.p + lx.p, ')', lx.s.n - lx.p);
        if (!close) return setting_bad(lineno, name, "enum( needs a closing )", NULL);
        type.p = lx.s.p + lx.p;
        type.n = (size_t)(close + 1 - type.p);
        lx.p = (size_t)(close + 1 - lx.s.p);
        if (lx.p < lx.s.n && !is_space(lx.s.p[lx.p])) return setting_bad(lineno, name, "expected a space after the type", NULL);
    } else {
        tok t;
        if (!lex_next(&lx, &t) || t.quoted) return setting_bad(lineno, name, "missing type (bool, int, float, string or enum(a,b,c))", NULL);
        type = t.t;
    }
    if (is(type, "bool")) d.type = 0;
    else if (is(type, "int")) d.type = 1;
    else if (is(type, "float")) d.type = 2;
    else if (is(type, "string")) d.type = 3;
    else if (type.n >= 5 && memcmp(type.p, "enum(", 5) == 0) {
        strv rest;
        d.type = 4;
        rest.p = type.p + 5;
        rest.n = type.n - 6;   /* without "enum(" and ")" */
        for (;;) {
            const char* comma = rest.n ? (const char*)memchr(rest.p, ',', rest.n) : NULL;
            strv c;
            c.p = rest.p;
            c.n = comma ? (size_t)(comma - rest.p) : rest.n;
            c = trim(c);
            if (!valid_word(c, 31)) return setting_bad(lineno, name, "enum choices are a comma list of 1-31 of [a-z0-9_]", NULL);
            for (i = 0; i < d.nchoices; ++i)
                if (is(c, d.choices[i])) return setting_bad(lineno, name, "enum lists a choice twice", NULL);
            if (d.nchoices == MAX_CHOICES) return setting_bad(lineno, name, "enum lists more than 16 choices", NULL);
            set_field(d.choices[d.nchoices++], c);
            if (!comma) break;
            rest.n -= (size_t)(comma + 1 - rest.p);
            rest.p = comma + 1;
        }
    } else {
        return setting_bad(lineno, name, "type must be bool, int, float, string or enum(a,b,c)", NULL);
    }

    while (lex_next(&lx, &key)) {
        f = -1;
        for (i = 0; i < 5; ++i) if (!key.quoted && is(key.t, fields[i])) f = i;
        if (f < 0) {
            char buf[96];
            snprintf(buf, sizeof buf, "unknown field '%.*s' (default, min, max, label, help)", (int)key.t.n, key.t.p);
            return setting_bad(lineno, name, buf, NULL);
        }
        if (have[f]) { snprintf(why, sizeof why, "field '%s' given twice", fields[f]); return setting_bad(lineno, name, why, NULL); }
        if (!lex_next(&lx, &v)) {
            if (lx.bad) break;
            snprintf(why, sizeof why, "field '%s' needs a value", fields[f]);
            return setting_bad(lineno, name, why, NULL);
        }
        have[f] = 1;
        value[f] = v.t;
    }
    if (lx.bad) return setting_bad(lineno, name, "a quote is not closed, or not at the start of a value", NULL);

    if (have[F_MIN] || have[F_MAX]) {
        if (d.type != 1 && d.type != 2) return setting_bad(lineno, name, "min and max apply to int and float only", NULL);
        const sdecl probe = d;   /* the bounds are values of the type, checked before any bound exists */
        for (f = F_MIN; f <= F_MAX; ++f) {
            if (!have[f]) continue;
            if (!check_setting(&probe, value[f], why + 8, sizeof why - 8)) {
                memcpy(why, f == F_MIN ? "min " : "max ", 4);
                memmove(why + 4, why + 8, strlen(why + 8) + 1);
                return setting_bad(lineno, name, why, NULL);
            }
            if (d.type == 1) {
                long long b = 0;
                parse_i64(value[f], &b);
                if (f == F_MIN) { d.has_min = 1; d.min_i = b; } else { d.has_max = 1; d.max_i = b; }
            } else {
                double b = 0;
                parse_f64(value[f], &b);
                if (f == F_MIN) { d.has_min = 1; d.min_f = b; } else { d.has_max = 1; d.max_f = b; }
            }
        }
        if (d.has_min && d.has_max && (d.type == 1 ? d.min_i > d.max_i : d.min_f > d.max_f)) {
            snprintf(why, sizeof why, "min %.*s is above max %.*s", (int)value[F_MIN].n, value[F_MIN].p, (int)value[F_MAX].n, value[F_MAX].p);
            return setting_bad(lineno, name, why, NULL);
        }
    }
    if (have[F_LABEL] && !valid_text(value[F_LABEL], 63)) return setting_bad(lineno, name, "label must be 1-63 printable characters", NULL);
    if (have[F_HELP] && !valid_text(value[F_HELP], 255)) return setting_bad(lineno, name, "help must be 1-255 printable characters", NULL);
    if (have[F_DEFAULT] && !check_setting(&d, value[F_DEFAULT], why, sizeof why)) return setting_bad(lineno, name, "default ", why);

    set_field(m->settings[m->nsettings].name, name);
    strcpy(m->settings[m->nsettings].type, d.type == 0 ? "bool" : d.type == 1 ? "int" : d.type == 2 ? "float" : d.type == 3 ? "string" : "enum");
    ++m->nsettings;
    return 1;
}

static int read_manifest(const char* path, manifest* m) {
    enum { K_ID, K_NAME, K_VERSION, K_AUTHOR, K_API, K_KIND, K_ENTRY, K_REQUIRES, K_COUNT };
    static const char* const keys[K_COUNT] = { "id", "name", "version", "author", "api", "kind", "entry", "requires" };
    static char buf[MAX_MANIFEST_BYTES + 1];
    int seen[K_COUNT] = { 0 };
    int line = 0, k, i;
    int section = 0, seen_settings = 0;   /* 0 top level, 1 [settings], 2 another section */
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
        strv ln, raw, key, val;
        ln.p = all.p;
        ln.n = nl ? (size_t)(nl - all.p) : all.n;
        all.p += ln.n + (nl ? 1 : 0);
        all.n -= ln.n + (nl ? 1 : 0);
        ++line;
        raw = ln;
        ln = trim(strip_comment(ln));
        if (section == 1) ln = trim(strip_settings_comment(raw));   /* quotes may hold ';' or '#' */
        if (!ln.n) continue;
        if (ln.p[0] == '[') {
            strv sec;
            if (ln.p[ln.n - 1] != ']') return bad_line(line, "a [section] line ends with ]");
            sec.p = ln.p + 1;
            sec.n = ln.n - 2;
            sec = trim(sec);
            if (is(sec, "settings")) {
                if (seen_settings) { fail("plugin.ini line %d: duplicate section '[settings]'", line); return 0; }
                seen_settings = 1;
                section = 1;
            } else {
                section = 2;   /* a later minor's: skipped like an unknown key */
            }
            continue;
        }
        if (section == 1) { if (!setting_line(ln, m, line)) return 0; continue; }
        if (section == 2) continue;
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
    if (!strcmp(m->kind, "data") && m->nsettings) { fail("plugin.ini: a data pack has no settings"); return 0; }
    /* A plugin that requires itself or one of its own services is a requires cycle (discover.cpp). */
    for (i = 0; i < m->nrequires; ++i) {
        const size_t n = strlen(m->id);
        if (!strncmp(m->requires_[i], m->id, n) && (m->requires_[i][n] == 0 || m->requires_[i][n] == '.')) {
            fail("plugin.ini: requires '%s' names the plugin itself (a requires cycle)", m->requires_[i]);
            return 0;
        }
    }
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
static int short_tables;   /* --short-tables: sco_api at its 1.0 extent */
#define MAX_SERVICES 32
static char provided[MAX_SERVICES][64];   /* names the plugin publishes now */
static int  nprovided;
static char queried[MAX_SERVICES][64];    /* names it asked query_service for */
static int  nqueried;
static int widget_running = 1;
static int widget_dispatching;
static uint32_t widget_callback_logs, widget_callback_invokes;

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

/* ---- renderer-neutral retained widgets (headless stand-in only) ---------- */
/* Single-threaded, bounded copies; not a native-pointer validator, renderer,
 * game-thread detector or crash guard. Only callback fn/ctx remain borrowed. */
typedef struct widget_node_copy {
    sco_widget_node node;
    char label[SCO_WIDGETS_MAX_TEXT + 1u];
    char text[SCO_WIDGETS_MAX_TEXT + 1u];
    char tooltip[SCO_WIDGETS_MAX_TEXT + 1u];
    char** items;
    int disabled;
    int extended; /* read as a sco_widget_node_1_1: severity..visible_rows are set */
    uint32_t severity, first_row, visible_rows;
} widget_node_copy;

typedef struct widget_panel_copy {
    char id[SCO_WIDGETS_MAX_ID + 1u];
    char title[SCO_WIDGETS_MAX_TITLE + 1u];
    widget_node_copy* nodes;
    uint32_t count;
    uint64_t generation;
    sco_widget_event_fn fn;
    void* ctx;
} widget_panel_copy;

static widget_panel_copy* widget_panels[SCO_WIDGETS_MAX_PANELS];
static uint32_t widget_total_nodes;
static uint64_t widget_generation;

/* Read at most max+1 bytes, then reject malformed/overlong UTF-8, surrogates
 * and codepoints beyond U+10FFFF. Native pointers still must be readable. */
static int widget_string(char* dst, const char* src, size_t max, int required) {
    size_t n = 0, i = 0;
    if (!src) { dst[0] = 0; return !required; }
    while (n <= max && src[n]) ++n;
    if (n > max || (required && !n)) return 0;
    while (i < n) {
        const unsigned char c = (unsigned char)src[i++];
        uint32_t cp, lower;
        size_t extra, j;
        if (c < 0x80u) continue;
        if (c >= 0xc2u && c <= 0xdfu) { cp = c & 0x1fu; extra = 1; lower = 0x80u; }
        else if (c >= 0xe0u && c <= 0xefu) { cp = c & 0x0fu; extra = 2; lower = 0x800u; }
        else if (c >= 0xf0u && c <= 0xf4u) { cp = c & 7u; extra = 3; lower = 0x10000u; }
        else return 0;
        if (extra > n - i) return 0;
        for (j = 0; j < extra; ++j) {
            const unsigned char b = (unsigned char)src[i++];
            if ((b & 0xc0u) != 0x80u) return 0;
            cp = (cp << 6) | (b & 0x3fu);
        }
        if (cp < lower || cp > 0x10ffffu || (cp >= 0xd800u && cp <= 0xdfffu)) return 0;
    }
    memcpy(dst, src, n + 1u);
    return 1;
}

/* Node text limits return TOO_MANY, malformed UTF-8 returns BAD_ARG. IDs and
 * titles deliberately keep BAD_ARG for invalid/overlong syntax. */
static sco_result widget_text_error(const char* src) {
    size_t n = 0;
    if (src) while (n <= SCO_WIDGETS_MAX_TEXT && src[n]) ++n;
    return n > SCO_WIDGETS_MAX_TEXT ? SCO_TOO_MANY : SCO_BAD_ARG;
}

static int widget_id(char* dst, const char* id) {
    const size_t plen = strlen(plugin_id);
    return widget_string(dst, id, SCO_WIDGETS_MAX_ID, 1) && valid_command_name(dst) &&
           !strncmp(dst, plugin_id, plen) && dst[plen] == '.';
}

static void widget_free_nodes(widget_node_copy* nodes, uint32_t count) {
    uint32_t i, j;
    for (i = 0; i < count; ++i) {
        widget_node_copy* n = &nodes[i];
        if (n->items) {
            for (j = 0; j < n->node.item_count; ++j) free(n->items[j]);
            free(n->items);
        }
    }
}

static void widget_free(widget_panel_copy* panel) {
    if (!panel) return;
    widget_free_nodes(panel->nodes, panel->count);
    free(panel->nodes);
    free(panel);
}

static int widget_find(const char* id) {
    uint32_t i;
    for (i = 0; i < SCO_WIDGETS_MAX_PANELS; ++i)
        if (widget_panels[i] && !strcmp(widget_panels[i]->id, id)) return (int)i;
    return -1;
}

static sco_result widget_check_array(const sco_widget_node* nodes, uint32_t count, uint32_t stride) {
    if (!nodes || !count || stride < sizeof(sco_widget_node) || stride % _Alignof(sco_widget_node) ||
        (uintptr_t)nodes % _Alignof(sco_widget_node) || (size_t)stride > SIZE_MAX / count) return SCO_BAD_ARG;
    return count > SCO_WIDGETS_MAX_NODES ? SCO_TOO_MANY : SCO_OK;
}

/* LIST rows are items; TABLE rows are complete row-major cell rows. */
static uint32_t widget_rows(const sco_widget_node* n) {
    if (n->kind == SCO_WIDGET_TABLE) return n->columns ? n->item_count / n->columns : 0u;
    return n->kind == SCO_WIDGET_LIST ? n->item_count : 0u;
}

/* Copies one node whose size the caller checked (size_of_struct <= size <= stride). A node whose
 * size covers sco_widget_node_1_1 is also read as 1.1, except under --short-tables (a 1.0 table
 * ignores the tail). The disabled state is filled in later by widget_inherit. */
static sco_result widget_copy_node(const char* p, widget_node_copy* out) {
    const sco_widget_node* in = (const sco_widget_node*)p;
    uint32_t j;
    if (!in->id || in->kind < SCO_WIDGET_LABEL || in->kind > SCO_WIDGET_COLUMN ||
        (in->flags & ~(SCO_WIDGET_DISABLED | SCO_WIDGET_PASSWORD)) ||
        ((in->flags & SCO_WIDGET_PASSWORD) && in->kind != SCO_WIDGET_TEXT_INPUT)) return SCO_BAD_ARG;
    if (!isfinite(in->value) || !isfinite(in->minimum) || !isfinite(in->maximum)) return SCO_BAD_ARG;
    out->node = *in;
    out->node.items = NULL; /* even a zero-length array must not retain caller storage */
    if (!widget_string(out->label, in->label, SCO_WIDGETS_MAX_TEXT,
                       in->kind >= SCO_WIDGET_BUTTON && in->kind <= SCO_WIDGET_TEXT_INPUT))
        return widget_text_error(in->label);
    out->node.label = in->label ? out->label : NULL;
    /* Require all unused payload fields to be zero/NULL. */
    if (in->kind != SCO_WIDGET_TEXT_INPUT && in->text) return SCO_BAD_ARG;
    if (in->kind != SCO_WIDGET_TOGGLE && in->kind != SCO_WIDGET_SLIDER && in->value != 0) return SCO_BAD_ARG;
    if (in->kind != SCO_WIDGET_SLIDER && (in->minimum != 0 || in->maximum != 0)) return SCO_BAD_ARG;
    if (in->kind != SCO_WIDGET_COMBO && in->kind != SCO_WIDGET_LIST &&
        in->index != 0 && in->index != -1) return SCO_BAD_ARG;
    if (in->kind != SCO_WIDGET_TABLE && in->columns) return SCO_BAD_ARG;
    if (in->kind != SCO_WIDGET_COMBO && in->kind != SCO_WIDGET_LIST && in->kind != SCO_WIDGET_TABLE &&
        (in->items || in->item_count)) return SCO_BAD_ARG;
    if (in->kind == SCO_WIDGET_TOGGLE && in->value != 0 && in->value != 1) return SCO_BAD_ARG;
    if (in->kind == SCO_WIDGET_SLIDER && (in->minimum > in->maximum ||
        in->value < in->minimum || in->value > in->maximum)) return SCO_BAD_ARG;
    if (in->kind == SCO_WIDGET_TEXT_INPUT) {
        if (!widget_string(out->text, in->text, SCO_WIDGETS_MAX_TEXT, 0)) return widget_text_error(in->text);
        out->node.text = out->text;
    }
    if (in->item_count > SCO_WIDGETS_MAX_ITEMS || in->columns > SCO_WIDGETS_MAX_COLUMNS) return SCO_TOO_MANY;
    if ((in->item_count && !in->items) || (!in->item_count && in->items)) return SCO_BAD_ARG;
    if ((in->kind == SCO_WIDGET_COMBO || in->kind == SCO_WIDGET_LIST) &&
        (in->index < -1 || (in->index >= 0 && (uint32_t)in->index >= in->item_count))) return SCO_BAD_ARG;
    if (in->kind == SCO_WIDGET_TABLE && (!in->columns || in->item_count % in->columns)) return SCO_BAD_ARG;
    if (in->item_count) {
        out->items = (char**)calloc(in->item_count, sizeof *out->items);
        if (!out->items) return SCO_TOO_MANY;
        for (j = 0; j < in->item_count; ++j) {
            char value[SCO_WIDGETS_MAX_TEXT + 1u];
            size_t bytes;
            if (!in->items[j] || !widget_string(value, in->items[j], SCO_WIDGETS_MAX_TEXT, 0))
                return widget_text_error(in->items[j]);
            bytes = strlen(value) + 1u;
            out->items[j] = (char*)malloc(bytes);
            if (!out->items[j]) return SCO_TOO_MANY;
            memcpy(out->items[j], value, bytes);
        }
        out->node.items = (const char* const*)out->items;
    }
    if (!short_tables && in->size >= sizeof(sco_widget_node_1_1)) {
        sco_widget_node_1_1 x;
        const int pageable = in->kind == SCO_WIDGET_LIST || in->kind == SCO_WIDGET_TABLE;
        memcpy(&x, p, sizeof x);
        if (x.severity > SCO_WIDGET_SEVERITY_ERROR || x._reserved) return SCO_BAD_ARG;
        if ((!pageable && (x.first_row || x.visible_rows)) || (!x.visible_rows && x.first_row)) return SCO_BAD_ARG;
        if (x.visible_rows > SCO_WIDGETS_MAX_ITEMS) return SCO_TOO_MANY;
        if (x.first_row && x.first_row >= widget_rows(in)) return SCO_BAD_ARG;
        if (!widget_string(out->tooltip, x.tooltip, SCO_WIDGETS_MAX_TEXT, 0)) return widget_text_error(x.tooltip);
        out->extended = 1;
        out->severity = x.severity;
        out->first_row = x.first_row;
        out->visible_rows = x.visible_rows;
    }
    return SCO_OK;
}

/* Copies count preorder nodes, the stride read per node from its own size (1.0 or 1.1 layout).
 * open[] holds the parents a node may name: open[0] is the fragment's parent, then the open
 * ROW/COLUMN chain, so a closed ancestor cannot be re-entered (contiguous subtrees). single_root
 * (update_nodes): only nodes[0] may name open[0]. IDs of keep's nodes outside [skip_begin,
 * skip_end) are taken. */
static sco_result widget_parse(const sco_widget_node* nodes, uint32_t count, uint32_t stride, uint32_t root_parent,
                               int single_root, const widget_panel_copy* keep, uint32_t skip_begin,
                               uint32_t skip_end, widget_node_copy* out) {
    uint32_t open[SCO_WIDGETS_MAX_NODES + 1u], depth = 1, i, j, k;
    open[0] = root_parent;
    for (i = 0; i < count; ++i) {
        const char* p = (const char*)nodes + (size_t)i * stride;
        const sco_widget_node* in = (const sco_widget_node*)p;
        sco_result r;
        if (in->size < sizeof *in || in->size > stride) return SCO_BAD_ARG;
        for (j = 0; j < i; ++j) if (out[j].node.id == in->id) return SCO_BAD_ARG;
        if (keep)
            for (j = 0; j < keep->count; ++j)
                if ((j < skip_begin || j >= skip_end) && keep->nodes[j].node.id == in->id) return SCO_BAD_ARG;
        for (k = 0; k < depth; ++k) if (open[k] == in->parent) break;
        if (k == depth || (single_root && i && k == 0)) return SCO_BAD_ARG;
        depth = k + 1u;
        r = widget_copy_node(p, &out[i]);
        if (r != SCO_OK) return r;
        if (in->kind == SCO_WIDGET_ROW || in->kind == SCO_WIDGET_COLUMN) open[depth++] = in->id;
    }
    return SCO_OK;
}

/* Copies point label/text at their own buffers: re-point them after a copy moves. */
static void widget_relink(widget_node_copy* n) {
    if (n->node.label) n->node.label = n->label;
    if (n->node.text) n->node.text = n->text;
}

/* A node is disabled by its own flag or a disabled layout ancestor; parents precede children. */
static void widget_inherit(widget_node_copy* nodes, uint32_t count) {
    uint32_t i, j;
    for (i = 0; i < count; ++i) {
        nodes[i].disabled = (nodes[i].node.flags & SCO_WIDGET_DISABLED) != 0;
        if (!nodes[i].node.parent) continue;
        for (j = 0; j < i; ++j)
            if (nodes[j].node.id == nodes[i].node.parent) { nodes[i].disabled |= nodes[j].disabled; break; }
    }
}

static int widget_extended(const widget_panel_copy* panel) {
    uint32_t i;
    for (i = 0; i < panel->count; ++i) if (panel->nodes[i].extended) return 1;
    return 0;
}

static sco_result widget_set_panel(sco_plugin* self, const char* id, const char* title,
                                   const sco_widget_node* nodes, uint32_t count, uint32_t stride,
                                   sco_widget_event_fn fn, void* ctx) {
    char checked_id[SCO_WIDGETS_MAX_ID + 1u] = {0}, checked_title[SCO_WIDGETS_MAX_TITLE + 1u] = {0};
    uint32_t i, old_count = 0;
    int slot;
    widget_panel_copy* panel;
    widget_panel_copy* old;
    sco_result r;
    if (!widget_running) return SCO_UNAVAILABLE;
    if (self != &the_plugin || !widget_id(checked_id, id) ||
        !widget_string(checked_title, title, SCO_WIDGETS_MAX_TITLE, 1)) return SCO_BAD_ARG;
    r = widget_check_array(nodes, count, stride);
    if (r != SCO_OK) return r;
    slot = widget_find(checked_id);
    if (slot >= 0) old_count = widget_panels[slot]->count;
    else {
        for (i = 0; i < SCO_WIDGETS_MAX_PANELS; ++i) if (!widget_panels[i]) break;
        if (i == SCO_WIDGETS_MAX_PANELS) return SCO_TOO_MANY;
        slot = (int)i;
    }
    if (count > SCO_WIDGETS_MAX_TOTAL_NODES - (widget_total_nodes - old_count) ||
        widget_generation == UINT64_MAX) return SCO_TOO_MANY;
    panel = (widget_panel_copy*)calloc(1, sizeof *panel);
    if (!panel) return SCO_TOO_MANY;
    panel->nodes = (widget_node_copy*)calloc(count, sizeof *panel->nodes);
    if (!panel->nodes) { free(panel); return SCO_TOO_MANY; }
    panel->count = count;
    memcpy(panel->id, checked_id, sizeof checked_id);
    memcpy(panel->title, checked_title, sizeof checked_title);
    panel->fn = fn;
    panel->ctx = ctx;
    r = widget_parse(nodes, count, stride, 0, 0, NULL, 0, 0, panel->nodes);
    if (r != SCO_OK) { widget_free(panel); return r; }
    widget_inherit(panel->nodes, count);
    /* Publish only after every validation/allocation has succeeded. */
    old = widget_panels[slot];
    panel->generation = ++widget_generation;
    widget_panels[slot] = panel;
    widget_total_nodes = widget_total_nodes - old_count + count;
    widget_free(old);
    printf("  widgets set %s nodes=%u generation=%llu\n", panel->id, (unsigned)count,
           (unsigned long long)panel->generation);
    return SCO_OK;
}

static sco_result widget_remove_panel(sco_plugin* self, const char* id) {
    char checked_id[SCO_WIDGETS_MAX_ID + 1u];
    widget_panel_copy* old;
    int slot;
    if (!widget_running) return SCO_UNAVAILABLE;
    if (self != &the_plugin || !widget_id(checked_id, id)) return SCO_BAD_ARG;
    slot = widget_find(checked_id);
    if (slot < 0) return SCO_NOT_FOUND;
    old = widget_panels[slot];
    widget_panels[slot] = NULL;
    widget_total_nodes -= old->count;
    widget_free(old);
    printf("  widgets remove %s\n", checked_id);
    return SCO_OK;
}

static sco_result widget_own_panel(sco_plugin* self, const char* id, int* slot) {
    char checked_id[SCO_WIDGETS_MAX_ID + 1u];
    if (self != &the_plugin || !widget_id(checked_id, id)) return SCO_BAD_ARG;
    *slot = widget_find(checked_id);
    return *slot < 0 ? SCO_NOT_FOUND : SCO_OK;
}

static uint32_t widget_index(const widget_panel_copy* panel, uint32_t id) {
    uint32_t i;
    for (i = 0; i < panel->count; ++i) if (panel->nodes[i].node.id == id) break;
    return i;
}

/* End of the contiguous preorder subtree rooted at nodes[index]. */
static uint32_t widget_subtree_end(const widget_panel_copy* panel, uint32_t index) {
    uint32_t open[SCO_WIDGETS_MAX_NODES + 1u], depth = 1, end, k;
    open[0] = panel->nodes[index].node.id;
    for (end = index + 1u; end < panel->count; ++end) {
        for (k = 0; k < depth; ++k) if (open[k] == panel->nodes[end].node.parent) break;
        if (k == depth) break;
        depth = k + 1u;
        open[depth++] = panel->nodes[end].node.id;
    }
    return end;
}

static sco_result widget_update_nodes(sco_plugin* self, const char* panel_id, const sco_widget_node* nodes,
                                      uint32_t count, uint32_t stride) {
    widget_panel_copy* panel;
    widget_node_copy* fragment;
    widget_node_copy* tree;
    uint32_t begin, end, total, at, i;
    int slot;
    sco_result r;
    if (!widget_running) return SCO_UNAVAILABLE;
    r = widget_own_panel(self, panel_id, &slot);
    if (r != SCO_OK) return r;
    r = widget_check_array(nodes, count, stride);
    if (r != SCO_OK) return r;
    if (nodes->size < sizeof *nodes || nodes->size > stride) return SCO_BAD_ARG;
    panel = widget_panels[slot];
    begin = end = panel->count;
    at = widget_index(panel, nodes->id);
    if (nodes->id && at < panel->count) {
        if (panel->nodes[at].node.parent != nodes->parent) return SCO_BAD_ARG;
        begin = at;
        end = widget_subtree_end(panel, at);
    } else if (nodes->parent) {
        at = widget_index(panel, nodes->parent);
        if (at == panel->count || (panel->nodes[at].node.kind != SCO_WIDGET_ROW &&
                                   panel->nodes[at].node.kind != SCO_WIDGET_COLUMN)) return SCO_BAD_ARG;
        begin = end = widget_subtree_end(panel, at);
    }
    total = panel->count - (end - begin) + count;
    if (total > SCO_WIDGETS_MAX_NODES) return SCO_TOO_MANY;
    if (total > SCO_WIDGETS_MAX_TOTAL_NODES - (widget_total_nodes - panel->count) ||
        widget_generation == UINT64_MAX) return SCO_TOO_MANY;
    fragment = (widget_node_copy*)calloc(count, sizeof *fragment);
    tree = (widget_node_copy*)calloc(total, sizeof *tree);
    if (!fragment || !tree) { free(fragment); free(tree); return SCO_TOO_MANY; }
    r = widget_parse(nodes, count, stride, nodes->parent, 1, panel, begin, end, fragment);
    if (r != SCO_OK) {
        widget_free_nodes(fragment, count);
        free(fragment);
        free(tree);
        return r;
    }
    memcpy(tree, panel->nodes, (size_t)begin * sizeof *tree);
    memcpy(tree + begin, fragment, (size_t)count * sizeof *tree);
    memcpy(tree + begin + count, panel->nodes + end, (size_t)(panel->count - end) * sizeof *tree);
    widget_free_nodes(panel->nodes + begin, end - begin);
    free(panel->nodes);
    free(fragment);
    for (i = 0; i < total; ++i) widget_relink(&tree[i]);
    widget_inherit(tree, total);
    widget_total_nodes = widget_total_nodes - panel->count + total;
    panel->nodes = tree;
    panel->count = total;
    panel->generation = ++widget_generation;
    printf("  widgets update %s root=%u nodes=%u total=%u generation=%llu\n", panel->id, (unsigned)nodes->id,
           (unsigned)count, (unsigned)total, (unsigned long long)panel->generation);
    return SCO_OK;
}

static sco_result widget_remove_nodes(sco_plugin* self, const char* panel_id, const uint32_t* ids, uint32_t count) {
    char drop[SCO_WIDGETS_MAX_NODES];
    widget_panel_copy* panel;
    uint32_t i, at, kept = 0, removed;
    int slot;
    sco_result r;
    if (!widget_running) return SCO_UNAVAILABLE;
    r = widget_own_panel(self, panel_id, &slot);
    if (r != SCO_OK) return r;
    if (!ids || !count) return SCO_BAD_ARG;
    if (count > SCO_WIDGETS_MAX_NODES) return SCO_TOO_MANY;
    for (i = 0; i < count; ++i) if (!ids[i]) return SCO_BAD_ARG;
    if (widget_generation == UINT64_MAX) return SCO_TOO_MANY;
    panel = widget_panels[slot];
    memset(drop, 0, sizeof drop);
    for (i = 0; i < count; ++i) {
        at = widget_index(panel, ids[i]);
        if (at == panel->count) return SCO_NOT_FOUND;
        memset(drop + at, 1, (size_t)(widget_subtree_end(panel, at) - at));
    }
    for (i = 0; i < panel->count; ++i) if (!drop[i]) break;
    if (i == panel->count) return SCO_BAD_ARG; /* would empty the panel: that is remove_panel */
    for (i = 0; i < panel->count; ++i) {
        if (drop[i]) { widget_free_nodes(&panel->nodes[i], 1); continue; }
        if (kept != i) { panel->nodes[kept] = panel->nodes[i]; widget_relink(&panel->nodes[kept]); }
        ++kept;
    }
    removed = panel->count - kept;
    panel->count = kept;
    widget_total_nodes -= removed;
    panel->generation = ++widget_generation;
    printf("  widgets remove_nodes %s ids=%u removed=%u generation=%llu\n", panel->id, (unsigned)count,
           (unsigned)removed, (unsigned long long)panel->generation);
    return SCO_OK;
}

#define WIDGET_FEATURES (SCO_WIDGETS_FEATURE_UPDATE_NODES | SCO_WIDGETS_FEATURE_REMOVE_NODES | \
    SCO_WIDGETS_FEATURE_SEVERITY | SCO_WIDGETS_FEATURE_TOOLTIP | SCO_WIDGETS_FEATURE_PAGING | \
    SCO_WIDGETS_FEATURE_EVENT_ROW | SCO_WIDGETS_FEATURE_COALESCED_RENDER)

/* Advertises what the real host does (src/widgets/widgets.cpp), with the 1.0 limit macros. */
static sco_result widget_capabilities(sco_plugin* self, uint32_t* out) {
    static const uint32_t known[SCO_WIDGETS_CAPS_1_1] = {
        SCO_WIDGETS_CAPS_1_1, WIDGET_FEATURES, SCO_WIDGETS_MAX_PANELS, SCO_WIDGETS_MAX_NODES,
        SCO_WIDGETS_MAX_TOTAL_NODES, SCO_WIDGETS_MAX_ITEMS, SCO_WIDGETS_MAX_COLUMNS,
        SCO_WIDGETS_MAX_TEXT, SCO_WIDGETS_MAX_EVENTS
    };
    uint32_t i, n;
    if (!widget_running) return SCO_UNAVAILABLE;
    if (self != &the_plugin || !out || out[SCO_WIDGETS_CAP_COUNT] < 2u) return SCO_BAD_ARG;
    n = out[SCO_WIDGETS_CAP_COUNT] < SCO_WIDGETS_CAPS_1_1 ? out[SCO_WIDGETS_CAP_COUNT] : SCO_WIDGETS_CAPS_1_1;
    for (i = 1; i < n; ++i) out[i] = known[i];
    out[SCO_WIDGETS_CAP_COUNT] = n;
    printf("  widgets capabilities count=%u features=0x%02x\n", (unsigned)n, (unsigned)WIDGET_FEATURES);
    return SCO_OK;
}

/* --short-tables: what a plugin finds past the end of a 1.0 sco.widgets table. A real 1.0 table
 * has nothing there; these stand-ins report the call instead of jumping to garbage. */
#define SHORT_WIDGETS_SIZE offsetof(sco_widgets_v1, update_nodes)
#define WIDGETS_EXTENT(m) (offsetof(sco_widgets_v1, m) + sizeof(((const sco_widgets_v1*)0)->m))

static void widget_short_fail(const char* member, size_t extent) {
    fail("plugin called sco_widgets_v1.%s, but this host's sco.widgets table size is %u (a 1.0 table, "
         "--short-tables): check size >= %u (offsetof + sizeof of %s) and that the pointer is non-NULL "
         "before calling it", member, (unsigned)SHORT_WIDGETS_SIZE, (unsigned)extent, member);
}

static sco_result short_update_nodes(sco_plugin* self, const char* panel_id, const sco_widget_node* nodes,
                                     uint32_t count, uint32_t stride) {
    (void)self; (void)panel_id; (void)nodes; (void)count; (void)stride;
    widget_short_fail("update_nodes", WIDGETS_EXTENT(update_nodes));
    return SCO_UNAVAILABLE;
}

static sco_result short_remove_nodes(sco_plugin* self, const char* panel_id, const uint32_t* ids, uint32_t count) {
    (void)self; (void)panel_id; (void)ids; (void)count;
    widget_short_fail("remove_nodes", WIDGETS_EXTENT(remove_nodes));
    return SCO_UNAVAILABLE;
}

static sco_result short_capabilities(sco_plugin* self, uint32_t* out) {
    (void)self; (void)out;
    widget_short_fail("capabilities", WIDGETS_EXTENT(capabilities));
    return SCO_UNAVAILABLE;
}

/* sco.widgets 1.1 by default; under --short-tables the 1.0 extent (size 24), as a 1.0 host serves. */
static const sco_widgets_v1 widget_api = {
    (uint32_t)sizeof(sco_widgets_v1), 0, widget_set_panel, widget_remove_panel,
    widget_update_nodes, widget_remove_nodes, widget_capabilities
};
static const sco_widgets_v1 widget_api_short = {
    (uint32_t)SHORT_WIDGETS_SIZE, 0, widget_set_panel, widget_remove_panel,
    short_update_nodes, short_remove_nodes, short_capabilities
};

static void widget_stop(void) {
    uint32_t i, removed = 0;
    widget_running = 0;
    for (i = 0; i < SCO_WIDGETS_MAX_PANELS; ++i) {
        if (widget_panels[i]) ++removed;
        widget_free(widget_panels[i]);
        widget_panels[i] = NULL;
    }
    widget_total_nodes = 0;
    printf("  widgets unload removed=%u retained=0 callbacks=0\n", (unsigned)removed);
}

static const char* host_version(void) { return "sco-plugin-check " "1.0"; }

static int has(const char* capability) {
    int i;
    if (!capability) return 0;
    if (!strcmp(capability, SCO_WIDGETS_NAME)) return widget_running;
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
    if (widget_dispatching) ++widget_callback_logs;
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
    if (widget_dispatching) {
        ++widget_callback_invokes;
        printf("  widgets invoke %s result=%u reply=\"%s\"\n", name, (unsigned)r, reply);
    }
    if (done) done(r, reply, ctx);
    return r;
}

static uint32_t list_commands(const sco_command** out, uint32_t max) {
    uint32_t i;
    for (i = 0; i < (uint32_t)ncmds && i < max; ++i) out[i] = &cmds[i].c;
    return (uint32_t)ncmds;
}

static int find_name(char (*list)[64], int n, const char* name) {
    int i;
    for (i = 0; i < n; ++i) if (!strcmp(list[i], name)) return i;
    return -1;
}

static void remember_name(char (*list)[64], int* n, const char* name) {
    if (strlen(name) < 64 && *n < MAX_SERVICES && find_name(list, *n, name) < 0) strcpy(list[(*n)++], name);
}

/* 1.1. Only the built-in widgets service is queryable. Plugin-provided
 * services and raw handlers are listed, not connected to other plugins. */
static sco_result provide_service(sco_plugin* self, const char* name, uint32_t version, const void* vtable) {
    (void)self;
    if (!name || !vtable) return SCO_BAD_ARG;
    remember_name(provided, &nprovided, name);
    printf("  service %s %u.%u\n", name, version >> 16, version & 0xFFFFu);
    return SCO_OK;
}

static sco_result widgets_lookup(const char* name, uint32_t min_version, const void** out) {
    const uint32_t served = short_tables ? SCO_WIDGETS_VERSION_1_0 : SCO_WIDGETS_VERSION_1_1;
    if (!name || !out) return SCO_BAD_ARG;
    remember_name(queried, &nqueried, name);
    if (strcmp(name, SCO_WIDGETS_NAME)) return SCO_NOT_FOUND;
    if (!widget_running) return SCO_NOT_FOUND;
    if ((min_version >> 16) != (served >> 16) || min_version > served) return SCO_UNAVAILABLE;
    *out = short_tables ? &widget_api_short : &widget_api;
    return SCO_OK;
}

static sco_result query_service(const char* name, uint32_t min_version, const void** out) {
    if (out) *out = NULL;
    return widgets_lookup(name, min_version, out);
}

/* Withdraws one of the plugin's own services (sco_api.h). Not a reference release: a table from
 * query_service is borrowed, and releasing its name is the host's SCO_NOT_FOUND. */
static sco_result release_service(sco_plugin* self, const char* name) {
    int i;
    (void)self;
    if (!name) return SCO_BAD_ARG;
    i = find_name(provided, nprovided, name);
    if (i >= 0) {
        memmove(provided[i], provided[i + 1], (size_t)(nprovided - i - 1) * sizeof provided[0]);
        --nprovided;
        printf("  release %s\n", name);
        return SCO_OK;
    }
    if (find_name(queried, nqueried, name) >= 0)
        fail("release_service '%s': the plugin queried this service but does not provide it; release_service "
             "withdraws one of the plugin's own services (provide_service), a queried table is borrowed and "
             "has no reference to release (the host returns not_found)", name);
    else
        fail("release_service '%s': the plugin does not provide a service by that name, or already withdrew it "
             "(the host returns not_found)", name);
    return SCO_NOT_FOUND;
}

/* --short-tables: what a 1.1 plugin finds past the end of a 1.0 host's sco_api. A real 1.0 host
 * has nothing there; these stand-ins report the call instead of jumping to garbage. */
#define SHORT_API_SIZE offsetof(sco_api, provide_service)
#define API_EXTENT(m) (offsetof(sco_api, m) + sizeof(((const sco_api*)0)->m))

static void short_table_fail(const char* member, size_t extent) {
    fail("plugin called sco_api.%s, but this host's sco_api.size is %u (a 1.0 host, --short-tables): "
         "check size >= %u (offsetof + sizeof of %s) before reading that pointer",
         member, (unsigned)SHORT_API_SIZE, (unsigned)extent, member);
}

static sco_result short_provide_service(sco_plugin* self, const char* name, uint32_t version, const void* vtable) {
    (void)self; (void)name; (void)version; (void)vtable;
    short_table_fail("provide_service", API_EXTENT(provide_service));
    return SCO_UNAVAILABLE;
}

static sco_result short_query_service(const char* name, uint32_t min_version, const void** out) {
    if (out) *out = NULL;
    short_table_fail("query_service", API_EXTENT(query_service));
    /* Already a failed check; answering (with the 1.0 widgets table) lets later unchecked calls show too. */
    return widgets_lookup(name, min_version, out);
}

static sco_result short_release_service(sco_plugin* self, const char* name) {
    (void)self; (void)name;
    short_table_fail("release_service", API_EXTENT(release_service));
    return SCO_UNAVAILABLE;
}

static sco_result short_invoke_raw(sco_plugin* self, const char* name, const void* in, uint32_t in_size,
                                   void* out, uint32_t* out_size) {
    (void)self; (void)name; (void)in; (void)in_size; (void)out;
    if (out_size) *out_size = 0;
    short_table_fail("invoke_raw", API_EXTENT(invoke_raw));
    return SCO_UNAVAILABLE;
}

static sco_result short_register_raw(sco_plugin* self, const char* name, const char* capability, sco_raw_fn fn, void* ctx) {
    (void)self; (void)name; (void)capability; (void)fn; (void)ctx;
    short_table_fail("register_raw", API_EXTENT(register_raw));
    return SCO_UNAVAILABLE;
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

/* A pass snapshots event payloads and panel generations, not plugin pointers.
 * Replacing/removing any panel suppresses its stale pending events, including
 * remove+re-add in the same slot. Replacement trees wait for a future pass. */
typedef struct widget_pending_event {
    uint32_t slot, node_index;
    uint64_t generation;
    char panel_id[SCO_WIDGETS_MAX_ID + 1u];
    char text[SCO_WIDGETS_MAX_TEXT + 1u];
    sco_widget_event event;
    int extended;   /* the panel holds a 1.1 node: deliver a sco_widget_event_1_1 */
    int32_t row;
} widget_pending_event;

static const char* widget_kind_name(uint32_t kind) {
    static const char* const names[] = {
        "?", "LABEL", "BUTTON", "TOGGLE", "SLIDER", "COMBO", "LIST", "TEXT_INPUT", "TABLE", "ROW", "COLUMN"
    };
    return kind <= SCO_WIDGET_COLUMN ? names[kind] : "?";
}

static void widget_simulate(void) {
    widget_pending_event pending[SCO_WIDGETS_MAX_EVENTS];
    uint32_t i, j, count = 0, delivered = 0, stale = 0, limited = 0;
    if (!widget_running) return;
    for (i = 0; i < SCO_WIDGETS_MAX_PANELS; ++i) {
        const widget_panel_copy* panel = widget_panels[i];
        if (!panel) continue;
        printf("  widgets panel %s title=\"%s\" nodes=%u\n", panel->id, panel->title, (unsigned)panel->count);
        for (j = 0; j < panel->count; ++j) {
            const widget_node_copy* node = &panel->nodes[j];
            const sco_widget_node* n = &node->node;
            widget_pending_event* p;
            printf("  widgets node %s id=%u parent=%u kind=%u(%s) disabled=%d label=\"%s\"",
                   panel->id, (unsigned)n->id, (unsigned)n->parent, (unsigned)n->kind,
                   widget_kind_name(n->kind), node->disabled, n->label ? n->label : "");
            if (node->extended)
                printf(" severity=%u first_row=%u visible_rows=%u tooltip=\"%s\"", (unsigned)node->severity,
                       (unsigned)node->first_row, (unsigned)node->visible_rows, node->tooltip);
            printf("\n");
            if (!panel->fn || node->disabled || n->kind < SCO_WIDGET_BUTTON || n->kind > SCO_WIDGET_TEXT_INPUT ||
                ((n->kind == SCO_WIDGET_COMBO || n->kind == SCO_WIDGET_LIST) && !n->item_count)) continue;
            if (count == SCO_WIDGETS_MAX_EVENTS) { ++limited; continue; }
            p = &pending[count++];
            memset(p, 0, sizeof *p);
            p->slot = i; p->node_index = j; p->generation = panel->generation;
            memcpy(p->panel_id, panel->id, sizeof p->panel_id);
            p->event.size = sizeof p->event;
            p->event.node_id = n->id; p->event.kind = n->kind;
            p->extended = widget_extended(panel);
            p->row = -1;
            switch (n->kind) {
                case SCO_WIDGET_TOGGLE: p->event.value = n->value == 0 ? 1 : 0; break;
                case SCO_WIDGET_SLIDER:
                    /* Half first so even [-DBL_MAX,DBL_MAX] stays finite. */
                    p->event.value = n->minimum * 0.5 + n->maximum * 0.5;
                    /* Half-subnormal endpoints can round toward zero. */
                    if (p->event.value < n->minimum) p->event.value = n->minimum;
                    if (p->event.value > n->maximum) p->event.value = n->maximum;
                    if (p->event.value == n->value)
                        p->event.value = n->value == n->maximum ? n->minimum : n->maximum;
                    break;
                case SCO_WIDGET_COMBO: case SCO_WIDGET_LIST:
                    p->event.index = (n->index + 1) % (int32_t)n->item_count;
                    if (n->kind == SCO_WIDGET_LIST) p->row = p->event.index;
                    break;
                case SCO_WIDGET_TEXT_INPUT:
                    strcpy(p->text, !strcmp(node->text, "headless input") ? "headless input (2)" : "headless input");
                    p->event.text = p->text; break;
                default: break;
            }
        }
    }
    for (i = 0; i < count; ++i) {
        widget_pending_event* p = &pending[i];
        widget_panel_copy* panel = widget_panels[p->slot];
        widget_node_copy* node;
        sco_widget_event_fn fn;
        void* ctx;
        sco_widget_event_1_1 wire;
        char row_text[48] = "";
        int before_failures = failures;
        uint32_t before_logs = widget_callback_logs, before_invokes = widget_callback_invokes;
        if (!panel || panel->generation != p->generation) { ++stale; continue; }
        node = &panel->nodes[p->node_index];
        if (p->event.kind == SCO_WIDGET_TOGGLE || p->event.kind == SCO_WIDGET_SLIDER) node->node.value = p->event.value;
        if (p->event.kind == SCO_WIDGET_COMBO || p->event.kind == SCO_WIDGET_LIST) node->node.index = p->event.index;
        if (p->event.kind == SCO_WIDGET_TEXT_INPUT) strcpy(node->text, p->text);
        fn = panel->fn; ctx = panel->ctx;
        memset(&wire, 0, sizeof wire);
        wire.base = p->event;
        if (p->extended) {
            wire.base.size = sizeof wire;
            wire.row = p->row;
            snprintf(row_text, sizeof row_text, " size=%u row=%d", (unsigned)wire.base.size, (int)wire.row);
        }
        printf("  widgets event %s node=%u kind=%u(%s) value=%.17g index=%d text=\"%s\"%s\n",
               p->panel_id, (unsigned)p->event.node_id, (unsigned)p->event.kind,
               widget_kind_name(p->event.kind), p->event.value, (int)p->event.index, p->event.text ? p->event.text : "",
               row_text);
        widget_dispatching = 1;
        fn(p->panel_id, &wire.base, ctx);
        widget_dispatching = 0;
        ++delivered;
        /* Never dereference panel/node/fn/ctx after the plugin callout: the
         * callback may have removed its panel and freed its own context. */
        printf("  widgets callback %s node=%u returned logs=%u invokes=%u\n", p->panel_id,
               (unsigned)p->event.node_id, (unsigned)(widget_callback_logs - before_logs),
               (unsigned)(widget_callback_invokes - before_invokes));
        if (failures != before_failures) {
            printf("  widgets events stopped after checker-detected callback failure (no crash containment)\n");
            break;
        }
    }
    printf("  widgets events delivered=%u stale=%u limited=%u\n", (unsigned)delivered, (unsigned)stale, (unsigned)limited);
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
        strncpy(path, full, sizeof path - 1);
        path[sizeof path - 1] = '\0';
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
    if (short_tables) {
        api.size = (uint32_t)SHORT_API_SIZE;
        api.minor = 0;
        api.provide_service = short_provide_service;
        api.query_service = short_query_service;
        api.release_service = short_release_service;
        api.invoke_raw = short_invoke_raw;
        api.register_raw = short_register_raw;
        printf("  api     1.0 table: size %u, minor 0 (--short-tables)\n", (unsigned)api.size);
    }

    r = load(&api, &the_plugin);
    if (r != SCO_OK) { fail("sco_plugin_load returned %s", result_name(r)); unload(); widget_stop(); return; }
    printf("  load    ok: %d command(s)\n", ncmds);

    dispatch("game.ready", NULL);
    for (i = 1; i <= 3; ++i) tick((uint32_t)(i * 100));
    widget_simulate();

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
    widget_stop();
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
        else if (!strcmp(argv[i], "--short-tables")) short_tables = 1;
        else if (!strcmp(argv[i], "--invoke") && i + 1 < argc) {
            invoke_name = argv[++i];
            invoke_args = argv + i + 1;
            ninvoke_args = argc - i - 1;
            break;
        } else if (!dir && argv[i][0] != '-') dir = argv[i];
        else { dir = NULL; break; }
    }
    if (!dir) {
        fprintf(stderr, "usage: sco-plugin-check <plugin folder> [--cap NAME]... [--short-tables] [--invoke NAME [ARG]...]\n");
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
            printf("  needs   %s%s\n", m.requires_[i], has(m.requires_[i]) ? "" : " (no --cap: the host checks it, loading this plugin after the plugin that provides it)");
        for (i = 0; i < m.nsettings; ++i)
            printf("  setting %s (%s)\n", m.settings[i].name, m.settings[i].type);
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
