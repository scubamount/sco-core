# Storage: the `sco.storage` service

Per-plugin persistent storage: a key-value store and SQL, over SQLite, in place of the loose text files plugins and features keep under `data/` today. Each plugin gets its own database file, which no other plugin can open; a crash at any moment leaves the last committed state.

It is a **host-owned service** (the reserved id `sco`, [API v1 § Host-owned services](api-v1.md#host-owned-services)) named `sco.storage`, version 1.0, found with the ordinary `query_service` of sco_api 1.1. `sco_api.h` is unchanged; the table is declared in [`include/sco_storage.h`](../include/sco_storage.h) (plain C) and pinned by [`tests/abi_storage.c`](../tests/abi_storage.c).

## Contents

- [Using it from C](#using-it-from-c)
- [Using it from C++](#using-it-from-c)
- [The table](#the-table)
- [Threads](#threads)
- [Transactions and crash safety](#transactions-and-crash-safety)
- [SQL: what a plugin may run](#sql-what-a-plugin-may-run)
- [Limits](#limits)
- [Unload, crash and shutdown](#unload-crash-and-shutdown)
- [Hosting it](#hosting-it)
- [Lua](#lua)

## Using it from C

```c
#include "sco_storage.h"

static const sco_storage_v1* st;

sco_result sco_plugin_load(const sco_api* api, sco_plugin* self) {
    if (api->size <= offsetof(sco_api, query_service) ||
        api->query_service(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0, (const void**)&st) != SCO_OK)
        st = NULL;   /* a host without storage: keep working without it */
    if (st) {
        my_spot home = { 1.0, 2.0, 3.0 };
        st->put(self, "spots.home", &home, sizeof home);

        my_spot got;
        uint32_t size = sizeof got;
        if (st->get(self, "spots.home", &got, &size) == SCO_OK && size == sizeof got) { ... }

        st->exec(self, "CREATE TABLE IF NOT EXISTS visits(place TEXT, at INTEGER)", NULL, 0, NULL);
        sco_sql_value row[2] = { { SCO_SQL_TEXT, 8 }, { SCO_SQL_INT } };
        row[0].v.p = "Lorville";
        row[1].v.i = 1;
        st->exec(self, "INSERT INTO visits VALUES (?, ?)", row, 2, NULL);
    }
    return SCO_OK;
}
```

A host service stays valid for as long as any plugin is loaded (host services are withdrawn after every plugin unloaded), so keeping `st` for the plugin's life is fine. A host that doesn't publish storage answers `SCO_NOT_FOUND`.

## Using it from C++

[`scosdk/storage.hpp`](../include/scosdk/storage.hpp) wraps the table ([C++ SDK](sdk-cpp.md#storage)):

```cpp
sco::sdk::Storage store;
if (store.Open(*this) == SCO_OK) {
    store.Put("spots.home", spot);                         // a trivially copyable struct
    std::string name;
    store.Get("player.name", name);                        // any size; the handshake is done for you
    std::vector<std::string> keys;
    store.Keys("spots.", keys);
    {
        sco::sdk::StorageTransaction tx(store);            // rolled back unless committed
        store.Exec("INSERT INTO visits VALUES (?, ?)", { sco::sdk::SqlText("Lorville"), sco::sdk::SqlInt(1) });
        tx.Commit();
    }
    sco::sdk::StorageCursor c = store.Query("SELECT place FROM visits WHERE at > ?", { sco::sdk::SqlInt(0) });
    std::string place;
    while (c.Next() == SCO_OK) c.Text(0, place);
}
```

Every call is `noexcept` and answers `sco_result`; `Storage::LastError()` gives the message of the last failure.

## The table

All functions take the plugin's `self`; the plugin can only reach its own database.

| Function | Does | Results besides `SCO_OK` |
|---|---|---|
| `put(self, key, value, size)` | Writes a value (bytes), replacing any old one | `BAD_ARG` bad key or size; `TOO_MANY` quota full |
| `get(self, key, out, &size)` | Reads a value with the size handshake | `NOT_FOUND`; `TOO_MANY` + needed size |
| `del(self, key)` | Removes a key | `NOT_FOUND` |
| `next_key(self, prefix, after, out, &size)` | The first key with `prefix` that sorts after `after` (NULL: from the start), byte order | `NOT_FOUND` when there are no more |
| `begin` / `commit` / `rollback` | One transaction per plugin, over key-value and SQL alike | `BAD_ARG` wrong state |
| `exec(self, sql, params, n, &changes)` | Runs one statement; rows are dropped | `BAD_ARG` refused or malformed; `FAILED` ran and failed; `TOO_MANY` quota |
| `query(self, sql, params, n, &cursor)` | Opens a cursor before the first row | as `exec`; `TOO_MANY` 64 cursors open |
| `step(self, cursor)` | Next row | `NOT_FOUND` no more rows (or not this plugin's cursor) |
| `column_count`, `column`, `column_name` | Read the current row; TEXT and BLOB are copied with the size handshake | `BAD_ARG` no row, index out of range |
| `close(self, cursor)` | Closes a cursor | `NOT_FOUND` |
| `last_error(self, out, &size)` | The message of the plugin's last failed call | |

The rules from `sco_api.h` hold, plus:

- **Ids, never pointers.** A query hands back a cursor id (an opaque `uint64_t`, unique in the process); column values are copied into the caller's buffer. Nothing points into SQLite or the host ([lesson 6](framework.md#6-services-hand-out-ids-never-pointers)). Another plugin's cursor id answers `SCO_NOT_FOUND`.
- **The size handshake** is the raw handlers' one: `*inout_size` is the buffer's capacity on the way in, the bytes written on the way out, or with `SCO_TOO_MANY` the bytes needed. Pass `out = NULL` with `*inout_size = 0` to ask the size. Keys, column names, error messages and TEXT columns come back NUL-terminated, and the NUL counts.
- **Keys** are 1-255 bytes and sort by their bytes; **values** are 0 bytes to 1 MiB. Parameters (`sco_sql_value`) are copied during the call.
- **The key-value store is a table** in the plugin's database, `sco_kv(key TEXT PRIMARY KEY, value BLOB)`. SQL may read and change its rows; its schema is the host's.

## Threads

Any thread. Each plugin's calls run one at a time: a call waits while another call of the same plugin runs, and different plugins never wait for each other. Calls are short and synchronous; a statement that runs past the time budget (1 s by default) is interrupted with `SCO_FAILED`, so the game thread never waits for longer than that on another thread of its own plugin.

The transaction and the cursors belong to the plugin, not to the thread: a `put` from a second thread while the first has a transaction open joins that transaction. A plugin that writes from several threads and uses transactions coordinates its own threads.

## Transactions and crash safety

- Databases run in WAL mode with `synchronous = FULL`: a `put`, `del` or `exec` outside a transaction, and a `commit`, are durable when they return, across a crash of the game or of the machine. (The host can choose `NORMAL`, `Options::durable = false`: still consistent after any crash, but a power cut may drop the last commits.)
- Outside a transaction every call is its own transaction. Between `begin` and `commit` nothing is durable; a crash, a kill, an unload or `rollback` undoes all of it. `begin` takes the write lock at once (`BEGIN IMMEDIATE`).
- A batch of writes is much faster inside one transaction: each autocommit write waits for the disk.
- Transactions go through `begin`, `commit` and `rollback` only; `BEGIN`, `COMMIT`, `SAVEPOINT` and `RELEASE` in SQL are refused, so the host always knows the state.

`tests/test_storage.cpp` kills a child process (SIGKILL / `TerminateProcess`) in the middle of a 6 MiB transaction and in a loop of committing transactions, then checks that only committed state is there, never half a transaction, and that `PRAGMA integrity_check` says `ok`.

## SQL: what a plugin may run

Any SQLite statement on its own database, one per call (trailing whitespace and comments are fine), with bound parameters (`?`, `?NNN`, `:name`, in index order; their number must match). Refused with `SCO_BAD_ARG`:

| Refused | Why |
|---|---|
| `ATTACH`, `DETACH` (also with URI names or a bound path) | No way into another file. On top of the authorizer, the connection allows 0 attached databases |
| `VACUUM`, `VACUUM INTO` | Rewrites the file, or writes a copy anywhere |
| `BEGIN`, `COMMIT`, `END`, `ROLLBACK`, `SAVEPOINT`, `RELEASE` | Use `begin` / `commit` / `rollback` |
| `CREATE` / `DROP` / `ALTER` of tables, views, indexes or triggers named `sco_*`, or on a `sco_*` table | The host's (the key-value table) |
| `CREATE VIRTUAL TABLE`, `DROP` of one | Not needed; keeps the module surface small |
| Every `PRAGMA` except: `table_info`, `table_xinfo`, `table_list`, `index_list`, `index_info`, `index_xinfo`, `foreign_key_list`, `foreign_key_check`, `integrity_check`, `quick_check`, `user_version`, `application_id`, `foreign_keys`, `data_version`, `freelist_count`, `page_count`, and reading `page_size` | Journal, sync, page, cache, mmap and file settings are the host's |

The connection is also built defensively: `SQLITE_DBCONFIG_DEFENSIVE` on, untrusted schema, double-quoted strings off, no URI file names, no symlinked database file, no extension loading (not compiled in), temporary data in memory. See [third_party/sqlite](../third_party/sqlite/README.md) for the compile options.

## Limits

| Limit | Value |
|---|---|
| Key | 1-255 bytes |
| Value | 1 MiB (`SCO_STORAGE_MAX_VALUE`) |
| Statement text | 1,000,000 bytes |
| One TEXT or BLOB in SQL | 16 MiB |
| Open cursors | 64 per plugin (`SCO_STORAGE_MAX_CURSORS`) |
| Database size | 64 MiB per plugin by default (`Options::quotaBytes`, as `max_page_count`); a write past it is `SCO_TOO_MANY`, deleting still works |
| Time per call | 1 s by default (`Options::budgetMs`), then `SCO_FAILED` "statement ran past its time budget" |
| Waiting for a lock another process holds | 1 s by default (`Options::busyTimeoutMs`), then `SCO_FAILED` |

## Unload, crash and shutdown

When a plugin unloads, fails to load or crashes, `sco::Release(self)` runs a release hook that rolls back the plugin's open transaction, closes its cursors and closes its database. From then on every call naming that `self` is `SCO_BAD_ARG`, from any thread. A reload of the plugin (a new handle) opens the same file and sees what was committed; the old handle's cursor ids mean nothing to it.

The host reads and writes plugin memory (keys, values, SQL, buffers) only while it holds no lock, so a bad pointer from a plugin faults in plugin-facing code with nothing held, and the crash guard can still release the plugin.

At shutdown `sco::app::Stop` unloads every plugin first, then stops storage: `sco.storage` is withdrawn, any transaction still open is rolled back and every database is closed. Calls through a table still held answer `SCO_UNAVAILABLE`.

## Hosting it

The host kit starts storage when the product sets a data folder ([API: sco/app.h](api.md#scoapph-the-host-kit)):

```cpp
sco::app::Platform pf;
pf.dataRoot = "data";              // sc-offline: databases in data/storage/<plugin id>.db
sco::app::Start(pf);               // publishes sco.storage before any plugin loads
...
sco::app::Stop();                  // after UnloadAll: storage::Stop, host::WithdrawHostServices
```

Without `dataRoot` there is no storage, and plugins get `SCO_NOT_FOUND`. A product that doesn't use the host kit calls [`sco::storage::Start` / `Stop`](api.md#scostorageh-the-scostorage-service) itself (library `sco_storage`).

Data lives in `<dataRoot>/storage/<plugin id>.db` (plus SQLite's `-wal` and `-shm` files), outside the plugin's folder, so updating, reinstalling or disabling a plugin keeps its data. Plugin ids are `[a-z0-9_]`, so file names never clash or escape the folder. Removing a plugin's data is an explicit action (delete its three files while the game is closed).

## Lua

sco-lua has no service bindings yet (it doesn't query any service today). `sco.store.get/put/delete/list` and `sco.store.sql` for scripts, counting against the step budget, are planned as a follow-up (G018); until then storage is for native and C++ plugins.
