/*
 * sco_datacore.h: the host service "sco.datacore", version 1.1.
 *
 * 1.1 (same table as 1.0) makes add_record work; a 1.0 host answers it SCO_UNAVAILABLE. Query with
 * SCO_DATACORE_VERSION_1_1 to require it (a 1.0 host then answers SCO_UNAVAILABLE), or with
 * SCO_DATACORE_VERSION_1_0 to take either.
 *
 * Plugins queue DataCore overrides (the game's Data\Game2.dcb) from code: the operations of a
 * data pack's datacore\*.toml files (docs/datacore.md "Pack format"), built call by call. Plain C;
 * usable from C and C++. Published by the host (a host-owned service under the reserved id "sco")
 * only when the product enables it (sco::app::Platform::dataCore); otherwise query_service
 * answers SCO_NOT_FOUND. Found with sco_api 1.1 query_service:
 *
 *   const sco_datacore_v1* dc = NULL;
 *   uint64_t patch = 0;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0, (const void**)&dc) == SCO_OK &&
 *       dc->begin(self, 0, &patch) == SCO_OK) {
 *       sco_dc_value v = { sizeof v, SCO_DC_FLOAT };
 *       v.f = 3.5;
 *       dc->set(patch, "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem",
 *               "Components[SCItemQuantumDriveParams].params.spoolUpTime", &v);
 *       dc->commit(patch);
 *   }
 *
 * Reference: docs/datacore.md "The sco.datacore service". Layout pinned by tests/abi_datacore.c.
 * The rules of sco_api.h hold here too (4-byte enums, results instead of exceptions, 64-bit
 * only), plus:
 *  - Ids, never pointers. Patches and added instances are uint64_t ids; strings and values are
 *    copied during the call. Nothing points into the patcher or the game.
 *  - Timing. The game loads DataCore once, at startup, before plugins load. state() says where
 *    the host is: SCO_DC_OPEN (before the load: committed patches apply at it) or SCO_DC_LOADED
 *    (after it: commit saves the patch as data/datacore/pending/<plugin id>.toml and it applies
 *    from the next launch, right after the plugin's own data pack, until the plugin commits
 *    another patch after a load or the file is deleted). A patch is never applied late. Overrides
 *    that must apply in the session they are made belong in a data pack.
 *  - Ownership. begin ties a patch to self. A patch of a plugin that unloads or crashes before
 *    the load is dropped; every call naming it answers SCO_NOT_FOUND.
 *  - Validation at call time covers what needs no game file: field path and GUID syntax, value
 *    shapes, instance ids from the same patch (SCO_BAD_ARG; the operation isn't queued). Names
 *    resolve at the load, with the reasons in report().
 *  - Atomicity per patch: one failing operation leaves the whole patch unapplied, unless begin
 *    had SCO_DC_NON_ATOMIC.
 *  - Any thread. Calls take a short lock and never wait for the load.
 *
 * License: GPL-3.0, like the rest of sco-core.
 */
#ifndef SCO_DATACORE_H
#define SCO_DATACORE_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_DATACORE_NAME "sco.datacore"
#define SCO_DATACORE_VERSION_1_0 0x00010000u
#define SCO_DATACORE_VERSION_1_1 0x00010001u   /* add_record works */

/* state() */
#define SCO_DC_OPEN   1u   /* before the DataCore load: committed patches apply at it */
#define SCO_DC_LOADED 2u   /* after it (patched or not): commit saves the patch for the next launch */

/* begin() flags */
#define SCO_DC_NON_ATOMIC 0x1u   /* operations apply one by one instead of all or none */

/* sco_dc_report.state */
#define SCO_DC_QUEUED  1u   /* not applied yet: before the load, or saved for the next launch */
#define SCO_DC_APPLIED 2u
#define SCO_DC_SKIPPED 3u   /* this operation failed (reason says why); the others may have applied */
#define SCO_DC_REFUSED 4u   /* not applied: the patch is refused as a whole (atomic), or the load failed */

#define SCO_DC_OP_PATCH 0xFFFFFFFFu      /* sco_dc_report.op_index of the entry for the patch itself */
#define SCO_DC_MAX_OPS  65536u           /* operations per patch */
#define SCO_DC_MAX_PATCHES 64u           /* patches per plugin, not yet committed or queued for the load */

/* The event posted on the game thread after the load: on the next tick, so after game.ready when
 * the load came first. Data: a sco_dc_applied over every source of the load (data packs, saved
 * patches, patches committed before it). */
#define SCO_DC_APPLIED_EVENT "datacore.applied"

typedef enum sco_dc_type {
    SCO_DC_BOOL     = 0,   /* i: 0 or 1 */
    SCO_DC_INT      = 1,   /* i */
    SCO_DC_UINT     = 2,   /* u */
    SCO_DC_FLOAT    = 3,   /* f: float and double fields */
    SCO_DC_STRING   = 4,   /* s: string and locale fields (UTF-8; enums take the option name too) */
    SCO_DC_GUID     = 5,   /* s: "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx" */
    SCO_DC_ENUM     = 6,   /* s: the option name */
    SCO_DC_NULL     = 7,   /* a null pointer */
    SCO_DC_INSTANCE = 8,   /* u: an instance id add_instance returned for the same patch */
    SCO_DC_REF      = 9,   /* s: a reference field's target record: a name or "guid:xxxxxxxx-..." */
    SCO_DC_TYPE_FORCE32 = 0x7fffffff
} sco_dc_type;

typedef struct sco_dc_value {
    uint32_t    size;   /* sizeof(sco_dc_value) */
    sco_dc_type type;
    int64_t     i;      /* BOOL, INT */
    uint64_t    u;      /* UINT; INSTANCE: the instance id */
    double      f;      /* FLOAT */
    const char* s;      /* STRING, GUID, ENUM, REF; copied during the call */
} sco_dc_value;

typedef struct sco_dc_report {
    uint32_t size;          /* set by the caller: sizeof(sco_dc_report) */
    uint32_t state;         /* SCO_DC_QUEUED, SCO_DC_APPLIED, SCO_DC_SKIPPED, SCO_DC_REFUSED */
    uint32_t op_index;      /* the operation it is about, in call order; SCO_DC_OP_PATCH for the patch */
    char     reason[192];   /* NUL-terminated (truncated); "" when applied */
} sco_dc_report;

typedef struct sco_dc_applied {
    uint32_t size;          /* sizeof(sco_dc_applied) */
    uint32_t applied;       /* operations applied */
    uint32_t skipped;       /* operations that failed in sources that applied the rest */
    uint32_t refused;       /* operations of sources refused as a whole */
} sco_dc_applied;

typedef struct sco_datacore_v1 {
    uint32_t size; /* sizeof(sco_datacore_v1) as the host built it */
    uint32_t _pad;

    /* SCO_DC_OPEN or SCO_DC_LOADED. */
    uint32_t (*state)(void);
    /* A new patch owned by self. flags: 0 or SCO_DC_NON_ATOMIC. SCO_BAD_ARG: a bad self or flags.
     * SCO_TOO_MANY: SCO_DC_MAX_PATCHES of this plugin's patches are open or queued for the load. */
    sco_result (*begin)(sco_plugin* self, uint32_t flags, uint64_t* out_patch);
    /* record: a record name, or "guid:xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx". field: a field path,
     * name / name[3] / name[Type] joined with '.'. Sets a value in place; v NULL or SCO_DC_NULL
     * sets a null pointer, SCO_DC_INSTANCE points the field at an added instance. */
    sco_result (*set)(uint64_t patch, const char* record, const char* field, const sco_dc_value* v);
    /* A new instance of struct type, copied from clone_record's clone_field (NULL or "": the
     * record's root) or zero-filled (clone_record NULL). out_instance: its id, usable in this
     * patch's SCO_DC_INSTANCE values and set_pointer. */
    sco_result (*add_instance)(uint64_t patch, const char* type, const char* clone_record,
                               const char* clone_field, uint64_t* out_instance);
    /* Points a strong or weak pointer field at an added instance. */
    sco_result (*set_pointer)(uint64_t patch, const char* record, const char* field, uint64_t instance);
    /* Appends one element to an array field: a value, a pointer (SCO_DC_NULL, SCO_DC_INSTANCE), or
     * for an array of structs SCO_DC_INSTANCE, the added instance to copy in. */
    sco_result (*append)(uint64_t patch, const char* record, const char* field, const sco_dc_value* v);
    /* 1.1: a new record of struct type named name (unique among records), its root copied from the
     * record clone_record (a name or "guid:...", required; a record of the same struct). guid: NULL
     * or "" derives a stable one from the plugin id and name (the same at every launch); else
     * "xxxxxxxx-...", not zero. file_path: NULL or "" is libs/foundry/records/sco/<plugin id>/<name>.xml;
     * else libs/foundry/records/... ending in .xml. out_record: an id like add_instance's (the
     * record's root: "@<id>" as a record argument, SCO_DC_INSTANCE as a value); later operations
     * may also name the record by name or GUID, and reference fields take it as SCO_DC_REF.
     * SCO_BAD_ARG: a bad argument, or a name or GUID this patch already adds. The rest (struct
     * has records, name and GUID new to the file, clone's struct) is checked at the load.
     * In the saved .toml it is a [[record]] operation. SCO_UNAVAILABLE on a 1.0 host. */
    sco_result (*add_record)(uint64_t patch, const char* type, const char* name, const char* guid,
                             const char* clone_record, const char* file_path, uint64_t* out_record);
    /* Before the load: queues the patch for it (reports SCO_DC_QUEUED). After it: saves the patch as
     * data/datacore/pending/<plugin id>.toml (written to a temporary file, then renamed over the
     * old one) and reports SCO_DC_QUEUED "applies at the next launch"; an empty patch clears the
     * saved one. No more operations on it either way. SCO_BAD_ARG: already committed.
     * SCO_FAILED: the file couldn't be written (the patch stays open; commit again). */
    sco_result (*commit)(uint64_t patch);
    /* Drops a patch: an open one, or a committed one before the load. After it was saved or
     * applied only the id is forgotten (delete the saved file by committing an empty patch). */
    sco_result (*discard)(uint64_t patch);
    /* Entry index: 0 .. operations-1 are the operations in call order, then one entry for the
     * patch itself (op_index SCO_DC_OP_PATCH). out->size must be at least sizeof(sco_dc_report).
     * SCO_NOT_FOUND past the last entry or for an unknown, discarded or released patch. */
    sco_result (*report)(uint64_t patch, uint32_t index, sco_dc_report* out);
} sco_datacore_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_DATACORE_H */
