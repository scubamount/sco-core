/*
 * abi_storage.c: pins the layout of sco_storage.h (the host service "sco.storage", 1.0).
 *
 * Compile-only, like abi_v1.c: if anything here fails, the service table changed and every
 * plugin built against the old header would break. Version 1 only grows at the end of
 * sco_storage_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_storage rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_storage.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, limits ---- */
PIN(SCO_STORAGE_VERSION_1_0 == 0x00010000u);
PIN(SCO_STORAGE_MAX_KEY == 255u);
PIN(SCO_STORAGE_MAX_VALUE == 1048576u);
PIN(SCO_STORAGE_MAX_CURSORS == 64u);
PIN(sizeof(SCO_STORAGE_NAME) == 12); /* "sco.storage" */

/* ---- sco_sql_type ---- */
SIZE(sco_sql_type, 4);
PIN(SCO_SQL_NULL == 0);
PIN(SCO_SQL_INT == 1);
PIN(SCO_SQL_FLOAT == 2);
PIN(SCO_SQL_TEXT == 3);
PIN(SCO_SQL_BLOB == 4);
PIN(SCO_SQL_TYPE_FORCE32 == 0x7fffffff);

/* ---- sco_sql_value ---- */
SIZE(sco_sql_value, 16);
AT(sco_sql_value, type, 0);
AT(sco_sql_value, size, 4);
AT(sco_sql_value, v, 8);
AT(sco_sql_value, v.i, 8);
AT(sco_sql_value, v.f, 8);
AT(sco_sql_value, v.p, 8);

/* ---- sco_storage_v1 ---- */
SIZE(sco_storage_v1, 128);
AT(sco_storage_v1, size, 0);
AT(sco_storage_v1, _pad, 4);
AT(sco_storage_v1, put, 8);
AT(sco_storage_v1, get, 16);
AT(sco_storage_v1, del, 24);
AT(sco_storage_v1, next_key, 32);
AT(sco_storage_v1, begin, 40);
AT(sco_storage_v1, commit, 48);
AT(sco_storage_v1, rollback, 56);
AT(sco_storage_v1, exec, 64);
AT(sco_storage_v1, query, 72);
AT(sco_storage_v1, step, 80);
AT(sco_storage_v1, column_count, 88);
AT(sco_storage_v1, column, 96);
AT(sco_storage_v1, column_name, 104);
AT(sco_storage_v1, close, 112);
AT(sco_storage_v1, last_error, 120);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_storage_signatures(const sco_storage_v1* s) {
    sco_result (*put)(sco_plugin*, const char*, const void*, uint32_t) = s->put;
    sco_result (*get)(sco_plugin*, const char*, void*, uint32_t*) = s->get;
    sco_result (*del)(sco_plugin*, const char*) = s->del;
    sco_result (*next_key)(sco_plugin*, const char*, const char*, char*, uint32_t*) = s->next_key;
    sco_result (*begin)(sco_plugin*) = s->begin;
    sco_result (*commit)(sco_plugin*) = s->commit;
    sco_result (*rollback)(sco_plugin*) = s->rollback;
    sco_result (*exec)(sco_plugin*, const char*, const sco_sql_value*, uint32_t, int64_t*) = s->exec;
    sco_result (*query)(sco_plugin*, const char*, const sco_sql_value*, uint32_t, uint64_t*) = s->query;
    sco_result (*step)(sco_plugin*, uint64_t) = s->step;
    sco_result (*count)(sco_plugin*, uint64_t, uint32_t*) = s->column_count;
    sco_result (*column)(sco_plugin*, uint64_t, uint32_t, sco_sql_value*, void*, uint32_t*) = s->column;
    sco_result (*name)(sco_plugin*, uint64_t, uint32_t, char*, uint32_t*) = s->column_name;
    sco_result (*close_fn)(sco_plugin*, uint64_t) = s->close;
    sco_result (*last_error)(sco_plugin*, char*, uint32_t*) = s->last_error;
    (void)put; (void)get; (void)del; (void)next_key; (void)begin; (void)commit; (void)rollback;
    (void)exec; (void)query; (void)step; (void)count; (void)column; (void)name; (void)close_fn;
    (void)last_error;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_storage_pins(void);
void sco_abi_storage_pins(void) { (void)&pin_storage_signatures; }
