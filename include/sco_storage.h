/*
 * sco_storage.h: the host service "sco.storage", version 1.0.
 *
 * Per-plugin persistent storage over SQLite: a key-value store and SQL, each plugin in its own
 * database file that no other plugin can open. Plain C; usable from C and C++. Published by the
 * host (a host-owned service under the reserved id "sco"), found with sco_api 1.1 query_service:
 *
 *   const sco_storage_v1* st = NULL;
 *   if (api->size > offsetof(sco_api, query_service) &&
 *       api->query_service(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0, (const void**)&st) == SCO_OK)
 *       st->put(self, "spots.home", &spot, sizeof spot);
 *
 * Reference: docs/storage.md. Layout pinned by tests/abi_storage.c. The rules of sco_api.h hold
 * here too (4-byte enums, results instead of exceptions, 64-bit only), plus:
 *  - Ids, never pointers. A query hands back a cursor id; column values are copied into the
 *    caller's buffer. Nothing points into SQLite or the host.
 *  - Sizes use the raw-handler handshake: *inout_size holds the buffer's capacity on entry; on
 *    return the bytes written, or with SCO_TOO_MANY the bytes needed (pass out NULL with
 *    *inout_size 0 to ask). Text comes back NUL-terminated and the NUL counts.
 *  - Any thread. Each plugin's calls run one at a time (a call waits while another call of the
 *    same plugin runs); different plugins never wait for each other. A transaction and the
 *    cursors belong to the plugin, not to the thread that opened them.
 *  - When the plugin unloads or crashes, the host rolls back its open transaction and closes
 *    its cursors and its database; every later call naming that self is SCO_BAD_ARG. The table
 *    stays valid for the life of the host (host services outlive every plugin); once the host
 *    has stopped storage, calls answer SCO_UNAVAILABLE.
 *
 * License: GPL-3.0, like the rest of sco-core.
 */
#ifndef SCO_STORAGE_H
#define SCO_STORAGE_H

#include "sco_api.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SCO_STORAGE_NAME "sco.storage"
#define SCO_STORAGE_VERSION_1_0 0x00010000u

#define SCO_STORAGE_MAX_KEY     255u          /* key bytes, NUL excluded */
#define SCO_STORAGE_MAX_VALUE   (1u << 20)    /* value bytes */
#define SCO_STORAGE_MAX_CURSORS 64u           /* open cursors per plugin */

typedef enum sco_sql_type {
    SCO_SQL_NULL  = 0,
    SCO_SQL_INT   = 1,
    SCO_SQL_FLOAT = 2,
    SCO_SQL_TEXT  = 3,
    SCO_SQL_BLOB  = 4,
    SCO_SQL_TYPE_FORCE32 = 0x7fffffff
} sco_sql_type;

/* A bound parameter or a column value. type is a sco_sql_type. Frozen for 1.x: arrays of
 * sco_sql_value have a fixed 16-byte stride.
 *   parameters: INT v.i, FLOAT v.f, TEXT and BLOB v.p with size bytes (TEXT needs no NUL;
 *               v.p may be NULL only with size 0); copied during the call.
 *   columns:    type and v.i / v.f as stored; TEXT and BLOB: size = the value's bytes (no NUL),
 *               v.p = NULL, the bytes go to the caller's buffer. */
typedef struct sco_sql_value {
    uint32_t type;
    uint32_t size;
    union {
        int64_t     i;
        double      f;
        const void* p;
    } v;
} sco_sql_value;

typedef struct sco_storage_v1 {
    uint32_t size; /* sizeof(sco_storage_v1) as the host built it */
    uint32_t _pad;

    /* ---- key-value: string keys (1-255 bytes), byte values (0 to 1 MiB) ------------------ */

    /* Writes value (value_size bytes; value may be NULL with 0), replacing any old one. Durable
     * when it returns, unless a transaction is open. SCO_TOO_MANY: the plugin's quota is full. */
    sco_result (*put)(sco_plugin* self, const char* key, const void* value, uint32_t value_size);
    /* Reads a value with the size handshake. SCO_NOT_FOUND: no such key (*inout_size 0). */
    sco_result (*get)(sco_plugin* self, const char* key, void* out, uint32_t* inout_size);
    /* Removes a key. SCO_NOT_FOUND: there was none. */
    sco_result (*del)(sco_plugin* self, const char* key);
    /* The first key that starts with prefix (NULL or "": any key) and sorts after "after" (NULL:
     * from the start), in byte order, copied NUL-terminated with the size handshake. Iterate by
     * passing the last key back as after. SCO_NOT_FOUND: no more keys. */
    sco_result (*next_key)(sco_plugin* self, const char* prefix, const char* after, char* out,
                           uint32_t* inout_size);

    /* ---- transactions: one per plugin, over key-value and SQL alike ----------------------- */

    /* SCO_BAD_ARG: one is already open. Until commit nothing the plugin writes is durable; a
     * crash, an unload or rollback undoes all of it. */
    sco_result (*begin)(sco_plugin* self);
    /* Makes everything since begin durable. SCO_BAD_ARG: none open. */
    sco_result (*commit)(sco_plugin* self);
    /* Undoes everything since begin. SCO_BAD_ARG: none open. */
    sco_result (*rollback)(sco_plugin* self);

    /* ---- SQL on the plugin's own database ------------------------------------------------- */

    /* Runs one statement with nparams bound parameters (?, ?NNN, :name in index order; nparams
     * must equal the statement's count; params may be NULL with 0). Rows it returns are
     * dropped. out_changes (optional): rows changed. SCO_BAD_ARG: not exactly one statement, a
     * syntax error, a wrong parameter count, or something storage refuses (ATTACH, DETACH,
     * BEGIN, COMMIT, SAVEPOINT, VACUUM, a PRAGMA outside the list in docs/storage.md, schema
     * changes to a table or index named sco_*). SCO_FAILED: it ran and failed (a constraint,
     * its time budget, the database busy); last_error says why. SCO_TOO_MANY: the quota is full. */
    sco_result (*exec)(sco_plugin* self, const char* sql, const sco_sql_value* params,
                       uint32_t nparams, int64_t* out_changes);
    /* Prepares one statement as exec does and opens a cursor before its first row. Close it
     * when done. SCO_TOO_MANY: SCO_STORAGE_MAX_CURSORS already open. */
    sco_result (*query)(sco_plugin* self, const char* sql, const sco_sql_value* params,
                        uint32_t nparams, uint64_t* out_cursor);
    /* Moves to the next row. SCO_OK: a row is ready. SCO_NOT_FOUND: no more rows, or an id this
     * plugin doesn't hold. SCO_FAILED / SCO_TOO_MANY: as for exec. */
    sco_result (*step)(sco_plugin* self, uint64_t cursor);
    sco_result (*column_count)(sco_plugin* self, uint64_t cursor, uint32_t* out_count);
    /* Column index of the current row. out gets the type and value; TEXT and BLOB bytes go to
     * buf with the size handshake (TEXT NUL-terminated; inout_size NULL: no bytes wanted).
     * SCO_BAD_ARG: no current row, or index out of range. */
    sco_result (*column)(sco_plugin* self, uint64_t cursor, uint32_t index, sco_sql_value* out,
                         void* buf, uint32_t* inout_size);
    /* The column's name, NUL-terminated, with the size handshake. */
    sco_result (*column_name)(sco_plugin* self, uint64_t cursor, uint32_t index, char* out,
                              uint32_t* inout_size);
    /* Closes a cursor. SCO_NOT_FOUND: this plugin holds no cursor by that id. */
    sco_result (*close)(sco_plugin* self, uint64_t cursor);

    /* The message of this plugin's last failed call ("" if none), NUL-terminated, with the
     * size handshake. */
    sco_result (*last_error)(sco_plugin* self, char* out, uint32_t* inout_size);
} sco_storage_v1;

#ifdef __cplusplus
}
#endif

#endif /* SCO_STORAGE_H */
