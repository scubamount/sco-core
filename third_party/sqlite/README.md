# SQLite (vendored)

The official SQLite amalgamation, used by `sco.storage` ([docs/storage.md](../../docs/storage.md)). The two files are byte-for-byte the release's; nothing here is edited.

| | |
|---|---|
| Version | 3.53.4 (`SQLITE_SOURCE_ID` `2026-07-24 19:02:57 bf7c7f30031888f4e796e429ab3978879485813aaca6f641c7b33e4e09459bcc`) |
| Download | <https://sqlite.org/2026/sqlite-amalgamation-3530400.zip> (2,946,650 bytes), from <https://sqlite.org/download.html> |
| Zip SHA3-256 | `628a44cfe82c66aed1ccbbe85a562d2e33ebe64b3288981ed76285612227934e` (matches the download page) |
| Zip SHA-256 | `1e71ddf93849c6a6ecf58b827c0692073d2dd7ee40196158068f7b29f422e87d` |
| `sqlite3.c` SHA-256 | `b1dd5d74ec7f29055a6684fa06fb3c2f6821c87dd38f9a458dfd2e8a1db28189` (9,515,341 bytes) |
| `sqlite3.h` SHA-256 | `919e7f2e8ed1d8f56ac17b412b8971c76aa5d1a879752cc6058f75e7d5910e1d` (690,838 bytes) |
| License | Public domain ([sqlite.org/copyright.html](https://sqlite.org/copyright.html)) |

Only `sqlite3.c` and `sqlite3.h` are kept from the zip (`shell.c` and `sqlite3ext.h` are left out: no shell, no extensions).

## How it is built

CMake target `sco_sqlite` (static, C as `-std=gnu11`) and `tools/test.sh` compile it with the same options; keep the two lists in step:

| Option | Why |
|---|---|
| `SQLITE_THREADSAFE=2` | Multi-thread mode: `sco::storage` serializes each connection with its own lock, so SQLite's per-connection mutexes are not needed (global state stays locked) |
| `SQLITE_DEFAULT_WAL_SYNCHRONOUS=1` | NORMAL as the WAL default; `sco::storage` still sets `synchronous = FULL` per connection unless `Options::durable` is false |
| `SQLITE_OMIT_LOAD_EXTENSION` | No extension loading, no `dlopen` |
| `SQLITE_DQS=0` | Double-quoted strings are identifiers only |
| `SQLITE_TEMP_STORE=2` | Temporary tables and indexes in memory, never in the OS temp folder |
| `SQLITE_TRUSTED_SCHEMA=0` | Functions in the schema (views, triggers) must be innocuous |
| `SQLITE_USE_URI=0` | No URI file names |
| `SQLITE_OMIT_SHARED_CACHE`, `SQLITE_OMIT_DEPRECATED` | Less code, nothing sco-core uses |
| `SQLITE_DEFAULT_MEMSTATUS=0`, `SQLITE_LIKE_DOESNT_MATCH_BLOBS` | The recommended options |
| `SQLITE_ENABLE_API_ARMOR` | Bad arguments to the C API fail instead of crashing |

SQLite has no network code. Its own warnings are off for this target only (`/w`, `-w`); sco-core's code keeps `/W4 /WX` and `-Wall -Wextra -Werror`. Under `SCO_SANITIZE` it gets the sanitizers too, minus clang's `-fsanitize=function` (SQLite calls its destructors through a common pointer type).

## Updating

1. Download the latest `sqlite-amalgamation-3XXYYZZ.zip` from <https://sqlite.org/download.html> and check its SHA3-256 against the page.
2. Replace `sqlite3.c` and `sqlite3.h`, update the table above.
3. Run `tools/test.sh` and the CMake build; `test_storage` covers the hardening (refused `ATTACH`, `PRAGMA`, `VACUUM INTO`, the quota and the time budget).
