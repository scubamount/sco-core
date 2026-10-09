/*
 * abi_datacore.c: pins the layout of sco_datacore.h (the host service "sco.datacore", 1.0).
 *
 * Compile-only, like abi_v1.c and abi_storage.c: if anything here fails, the service table
 * changed and every plugin built against the old header would break. Version 1 only grows at the
 * end of sco_datacore_v1; existing lines never change.
 *
 * tools/test.sh compiles it as C11 and C++20 for the host, with -fshort-enums, and for
 * x86_64-pc-windows-msvc; the CMake test abi_datacore rebuilds it.
 */
#include <stddef.h>
#include <stdint.h>

#include "sco_datacore.h"

#ifdef __cplusplus
#define PIN(expr) static_assert(expr, #expr)
#else
#define PIN(expr) _Static_assert(expr, #expr)
#endif

#define SIZE(T, n)      PIN(sizeof(T) == (n))
#define AT(T, f, n)     PIN(offsetof(T, f) == (n))

PIN(sizeof(void*) == 8);

/* ---- name, version, constants ---- */
PIN(SCO_DATACORE_VERSION_1_0 == 0x00010000u);
PIN(SCO_DATACORE_VERSION_1_1 == 0x00010001u);
PIN(sizeof(SCO_DATACORE_NAME) == 13); /* "sco.datacore" */
PIN(sizeof(SCO_DC_APPLIED_EVENT) == 17); /* "datacore.applied" */
PIN(SCO_DC_OPEN == 1u);
PIN(SCO_DC_LOADED == 2u);
PIN(SCO_DC_NON_ATOMIC == 1u);
PIN(SCO_DC_QUEUED == 1u);
PIN(SCO_DC_APPLIED == 2u);
PIN(SCO_DC_SKIPPED == 3u);
PIN(SCO_DC_REFUSED == 4u);
PIN(SCO_DC_OP_PATCH == 0xFFFFFFFFu);
PIN(SCO_DC_MAX_OPS == 65536u);
PIN(SCO_DC_MAX_PATCHES == 64u);

/* ---- sco_dc_type ---- */
SIZE(sco_dc_type, 4);
PIN(SCO_DC_BOOL == 0);
PIN(SCO_DC_INT == 1);
PIN(SCO_DC_UINT == 2);
PIN(SCO_DC_FLOAT == 3);
PIN(SCO_DC_STRING == 4);
PIN(SCO_DC_GUID == 5);
PIN(SCO_DC_ENUM == 6);
PIN(SCO_DC_NULL == 7);
PIN(SCO_DC_INSTANCE == 8);
PIN(SCO_DC_REF == 9);
PIN(SCO_DC_TYPE_FORCE32 == 0x7fffffff);

/* ---- sco_dc_value ---- */
SIZE(sco_dc_value, 40);
AT(sco_dc_value, size, 0);
AT(sco_dc_value, type, 4);
AT(sco_dc_value, i, 8);
AT(sco_dc_value, u, 16);
AT(sco_dc_value, f, 24);
AT(sco_dc_value, s, 32);

/* ---- sco_dc_report ---- */
SIZE(sco_dc_report, 204);
AT(sco_dc_report, size, 0);
AT(sco_dc_report, state, 4);
AT(sco_dc_report, op_index, 8);
AT(sco_dc_report, reason, 12);

/* ---- sco_dc_applied (the datacore.applied event's data) ---- */
SIZE(sco_dc_applied, 16);
AT(sco_dc_applied, size, 0);
AT(sco_dc_applied, applied, 4);
AT(sco_dc_applied, skipped, 8);
AT(sco_dc_applied, refused, 12);

/* ---- sco_datacore_v1 ---- */
SIZE(sco_datacore_v1, 88);
AT(sco_datacore_v1, size, 0);
AT(sco_datacore_v1, _pad, 4);
AT(sco_datacore_v1, state, 8);
AT(sco_datacore_v1, begin, 16);
AT(sco_datacore_v1, set, 24);
AT(sco_datacore_v1, add_instance, 32);
AT(sco_datacore_v1, set_pointer, 40);
AT(sco_datacore_v1, append, 48);
AT(sco_datacore_v1, add_record, 56);
AT(sco_datacore_v1, commit, 64);
AT(sco_datacore_v1, discard, 72);
AT(sco_datacore_v1, report, 80);

/* ---- signatures: a changed parameter list fails to convert ---- */
static void pin_datacore_signatures(const sco_datacore_v1* d) {
    uint32_t (*state)(void) = d->state;
    sco_result (*begin)(sco_plugin*, uint32_t, uint64_t*) = d->begin;
    sco_result (*set)(uint64_t, const char*, const char*, const sco_dc_value*) = d->set;
    sco_result (*add_instance)(uint64_t, const char*, const char*, const char*, uint64_t*) = d->add_instance;
    sco_result (*set_pointer)(uint64_t, const char*, const char*, uint64_t) = d->set_pointer;
    sco_result (*append)(uint64_t, const char*, const char*, const sco_dc_value*) = d->append;
    sco_result (*add_record)(uint64_t, const char*, const char*, const char*, const char*, const char*, uint64_t*) = d->add_record;
    sco_result (*commit)(uint64_t) = d->commit;
    sco_result (*discard)(uint64_t) = d->discard;
    sco_result (*report)(uint64_t, uint32_t, sco_dc_report*) = d->report;
    (void)state; (void)begin; (void)set; (void)add_instance; (void)set_pointer; (void)append;
    (void)add_record; (void)commit; (void)discard; (void)report;
}

/* Keeps the pins referenced so -Wunused does not fire. */
void sco_abi_datacore_pins(void);
void sco_abi_datacore_pins(void) { (void)&pin_datacore_signatures; }
