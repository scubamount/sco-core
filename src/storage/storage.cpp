// sco.storage: per-plugin SQLite databases behind the C table of sco_storage.h (sco/storage.h).
//
// Locks: g_lock guards the started flag, the options and the owner -> database list; each
// database has its own lock, held for the whole of a call, so one plugin's calls run one at a
// time while different plugins never wait for each other. Order: g_lock, then the runtime's
// owner lock (detail::Released); a database lock is never taken while g_lock is held.
#include "sco/storage.h"
#include "sco/host.h"
#include "sco/log.h"
#include "../api/internal.h"
#include <sqlite3.h>
#include <atomic>
#include <chrono>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <vector>

namespace fs = std::filesystem;

namespace sco::storage {

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kMaxSqlBytes = 1000000;           // statement text
constexpr int kMaxSqlValueBytes = 16 << 20;     // one TEXT or BLOB, parameter or column

struct Cursor {
    sqlite3_stmt* stmt = nullptr;
    bool          row = false;   // a row is current
};

struct Db {
    std::mutex  lock;
    const void* owner = nullptr;
    std::string id;
    fs::path    dir, file;
    Options     opts;
    // Under lock:
    sqlite3*    db = nullptr;
    bool        closed = false;
    Result      closedResult = Result::BadArg;   // BadArg: the plugin was released; Unavailable: Stop
    bool        trusted = false;                 // a host statement: the authorizer lets it through
    Clock::time_point deadline{};
    std::map<uint64_t, Cursor> cursors;
    std::string error;                           // the last failed call's message
};

std::mutex                       g_lock;
bool                             g_started = false;
Options                          g_opts;
std::vector<std::shared_ptr<Db>> g_dbs;          // one per owner that has called since Start
std::atomic<uint64_t>            g_nextCursor{ 1 };

sco_result C(Result r) { return static_cast<sco_result>(static_cast<uint32_t>(r)); }

std::string Utf8(const fs::path& p) {
    const std::u8string s = p.u8string();
    return std::string(s.begin(), s.end());
}

// ---- the authorizer -------------------------------------------------------------------------

bool SameName(const char* a, const char* b) { return a && sqlite3_stricmp(a, b) == 0; }

bool ScoName(const char* name) { return name && sqlite3_strnicmp(name, "sco_", 4) == 0; }

// PRAGMAs a plugin may run on its own database: schema and integrity introspection, plus the
// few settings that only concern its own data. Journal, sync, page and file settings stay the
// host's.
bool PragmaAllowed(const char* name, const char* arg) {
    static const char* const kAny[] = {
        "table_info", "table_xinfo", "table_list", "index_list", "index_info", "index_xinfo",
        "foreign_key_list", "foreign_key_check", "integrity_check", "quick_check",
        "user_version", "application_id", "foreign_keys", "data_version", "freelist_count", "page_count",
    };
    for (const char* p : kAny)
        if (SameName(name, p)) return true;
    return SameName(name, "page_size") && !arg;   // read only
}

int Authorize(void* ctx, int action, const char* a1, const char* a2, const char*, const char*) {
    const Db* d = static_cast<const Db*>(ctx);
    if (d->trusted) return SQLITE_OK;
    switch (action) {
        case SQLITE_ATTACH:
        case SQLITE_DETACH:
        case SQLITE_TRANSACTION:   // transactions go through begin / commit / rollback
        case SQLITE_SAVEPOINT:
        case SQLITE_CREATE_VTABLE:
        case SQLITE_DROP_VTABLE:
            return SQLITE_DENY;
        case SQLITE_PRAGMA:
            return PragmaAllowed(a1, a2) ? SQLITE_OK : SQLITE_DENY;
        // Schema changes: sco_* tables, views, indexes and triggers are the host's.
        case SQLITE_CREATE_TABLE: case SQLITE_CREATE_TEMP_TABLE: case SQLITE_DROP_TABLE:
        case SQLITE_DROP_TEMP_TABLE: case SQLITE_CREATE_VIEW: case SQLITE_CREATE_TEMP_VIEW:
        case SQLITE_DROP_VIEW: case SQLITE_DROP_TEMP_VIEW: case SQLITE_CREATE_INDEX:
        case SQLITE_CREATE_TEMP_INDEX: case SQLITE_DROP_INDEX: case SQLITE_DROP_TEMP_INDEX:
        case SQLITE_CREATE_TRIGGER: case SQLITE_CREATE_TEMP_TRIGGER: case SQLITE_DROP_TRIGGER:
        case SQLITE_DROP_TEMP_TRIGGER: case SQLITE_ALTER_TABLE: case SQLITE_REINDEX:
            return ScoName(a1) || ScoName(a2) ? SQLITE_DENY : SQLITE_OK;
        default:
            return SQLITE_OK;
    }
}

int Progress(void* ctx) {
    const Db* d = static_cast<const Db*>(ctx);
    return Clock::now() > d->deadline ? 1 : 0;   // nonzero interrupts the statement
}

// ---- errors ---------------------------------------------------------------------------------

// Records the connection's message and maps rc: quota and memory are TooMany, a refusal by the
// authorizer BadArg, an interrupt (the time budget) Failed, anything else `otherwise`.
Result Fail(Db& d, int rc, Result otherwise) {
    const int primary = rc & 0xff;
    if (primary == SQLITE_INTERRUPT) {
        d.error = "statement ran past its time budget (" + std::to_string(d.opts.budgetMs) + " ms)";
        return Result::Failed;
    }
    d.error = d.db ? sqlite3_errmsg(d.db) : sqlite3_errstr(rc);
    if (primary == SQLITE_FULL || primary == SQLITE_NOMEM) return Result::TooMany;
    if (primary == SQLITE_AUTH) return Result::BadArg;
    return otherwise;
}

Result Refuse(Db& d, const char* why) {
    d.error = why;
    return Result::BadArg;
}

// ---- opening --------------------------------------------------------------------------------

// Runs host SQL (no plugin input) past the authorizer.
int Trusted(Db& d, const char* sql, std::string* firstValue = nullptr) {
    d.trusted = true;
    auto first = [](void* out, int n, char** values, char**) -> int {
        auto* s = static_cast<std::string*>(out);
        if (s && s->empty() && n > 0 && values[0]) *s = values[0];
        return 0;
    };
    const int rc = sqlite3_exec(d.db, sql, first, firstValue, nullptr);
    d.trusted = false;
    return rc;
}

void CloseConnection(Db& d) {
    for (auto& entry : d.cursors) sqlite3_finalize(entry.second.stmt);
    d.cursors.clear();
    if (!d.db) return;
    if (!sqlite3_get_autocommit(d.db)) {   // an open transaction: undo it
        d.deadline = Clock::now() + std::chrono::milliseconds(d.opts.budgetMs);
        Trusted(d, "ROLLBACK");
    }
    sqlite3_close_v2(d.db);
    d.db = nullptr;
}

Result Open(Db& d) {
    std::error_code ec;
    fs::create_directories(d.dir, ec);
    if (ec) {
        d.error = "cannot create " + Utf8(d.dir) + ": " + ec.message();
        Log("[storage] %s: %s", d.id.c_str(), d.error.c_str());
        return Result::Failed;
    }
    // No URI names, no symlinked database file, no shared mutex (calls are serialized by d.lock).
    const int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_NOMUTEX |
                      SQLITE_OPEN_NOFOLLOW | SQLITE_OPEN_EXRESCODE;
    int rc = sqlite3_open_v2(Utf8(d.file).c_str(), &d.db, flags, nullptr);
    if (rc == SQLITE_OK) {
        sqlite3_db_config(d.db, SQLITE_DBCONFIG_DEFENSIVE, 1, nullptr);
        sqlite3_db_config(d.db, SQLITE_DBCONFIG_TRUSTED_SCHEMA, 0, nullptr);
        sqlite3_db_config(d.db, SQLITE_DBCONFIG_DQS_DML, 0, nullptr);
        sqlite3_db_config(d.db, SQLITE_DBCONFIG_DQS_DDL, 0, nullptr);
        sqlite3_limit(d.db, SQLITE_LIMIT_ATTACHED, 0);
        sqlite3_limit(d.db, SQLITE_LIMIT_SQL_LENGTH, kMaxSqlBytes);
        sqlite3_limit(d.db, SQLITE_LIMIT_LENGTH, kMaxSqlValueBytes);
        sqlite3_busy_timeout(d.db, static_cast<int>(d.opts.busyTimeoutMs));
        sqlite3_set_authorizer(d.db, Authorize, &d);
        sqlite3_progress_handler(d.db, 1000, Progress, &d);
    }
    std::string mode, pageSize;
    if (rc == SQLITE_OK) rc = Trusted(d, "PRAGMA journal_mode = WAL", &mode);
    if (rc == SQLITE_OK && mode != "wal") {
        d.error = "WAL mode refused (journal_mode " + mode + ")";
        CloseConnection(d);
        Log("[storage] %s: %s", d.id.c_str(), d.error.c_str());
        return Result::Failed;
    }
    if (rc == SQLITE_OK) rc = Trusted(d, d.opts.durable ? "PRAGMA synchronous = FULL" : "PRAGMA synchronous = NORMAL");
    if (rc == SQLITE_OK) rc = Trusted(d, "PRAGMA page_size", &pageSize);
    if (rc == SQLITE_OK) {
        const uint64_t page = pageSize.empty() ? 4096 : std::stoull(pageSize);
        const uint64_t pages = page ? d.opts.quotaBytes / page : 0;
        const std::string sql = "PRAGMA max_page_count = " + std::to_string(pages ? pages : 1);
        rc = Trusted(d, sql.c_str());
    }
    if (rc == SQLITE_OK)
        rc = Trusted(d, "CREATE TABLE IF NOT EXISTS sco_kv(key TEXT PRIMARY KEY NOT NULL, value BLOB NOT NULL) WITHOUT ROWID");
    if (rc != SQLITE_OK) {
        const Result r = Fail(d, rc, Result::Failed);
        CloseConnection(d);
        Log("[storage] %s: cannot open %s: %s", d.id.c_str(), Utf8(d.file).c_str(), d.error.c_str());
        return r;
    }
    return Result::Ok;
}

// ---- entering a call ------------------------------------------------------------------------

// The caller's database, locked for the call. open: open the connection when it isn't yet.
struct Call {
    std::shared_ptr<Db>          db;
    std::unique_lock<std::mutex> hold;
};

Result Enter(sco_plugin* self, Call& call, bool open = true) {
    const char* id = host::PluginId(self);
    if (!id) return Result::BadArg;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return Result::Unavailable;
        if (detail::Released(self)) return Result::BadArg;
        for (const auto& d : g_dbs)
            if (d->owner == self) { call.db = d; break; }
        if (!call.db) {
            auto d = std::make_shared<Db>();
            d->owner = self;
            d->id = id;
            d->opts = g_opts;
            d->dir = g_opts.dataRoot / "storage";
            d->file = d->dir / (d->id + ".db");
            g_dbs.push_back(d);
            call.db = std::move(d);
        }
    }
    call.hold = std::unique_lock<std::mutex>(call.db->lock);
    Db& d = *call.db;
    if (d.closed) return d.closedResult;
    if (open && !d.db) {
        const Result r = Open(d);
        if (r != Result::Ok) return r;
    }
    d.deadline = Clock::now() + std::chrono::milliseconds(d.opts.budgetMs);
    return Result::Ok;
}

// Runs a table function: nothing throws across the C ABI (out of memory is SCO_TOO_MANY).
template <class F>
sco_result Guard(F&& f) noexcept {
    try {
        return C(f());
    } catch (...) {
        return SCO_TOO_MANY;
    }
}

// ---- plugin memory ----------------------------------------------------------------------------
//
// Plugin memory (keys, values, SQL, parameters, output buffers) is read and written only while
// no lock is held: inputs are copied in before Enter, outputs copied out after the call's lock
// is released. A bad pointer from a plugin then faults in plugin-facing code with nothing held,
// and the crash guard's Release(owner) can still close the plugin's database.

// The size handshake into the caller's buffer. *io holds the capacity.
Result CopyOut(const void* src, size_t n, bool nul, void* out, uint32_t* io) {
    const size_t need = n + (nul ? 1 : 0);
    if (need > UINT32_MAX) return Result::TooMany;
    if (*io < need) {
        *io = static_cast<uint32_t>(need);
        return Result::TooMany;
    }
    if (n) std::memcpy(out, src, n);
    if (nul) static_cast<char*>(out)[n] = '\0';
    *io = static_cast<uint32_t>(need);
    return Result::Ok;
}

// Reads the capacity of an out buffer: io must be non-null; out may be null only with *io 0.
// *io is 0 until the call writes it.
bool TakeBuffer(const void* out, uint32_t* io, uint32_t& capacity) {
    if (!io) return false;
    capacity = *io;
    *io = 0;
    return out || capacity == 0;
}

// A key or prefix: at most 255 bytes (a key at least 1), NUL-terminated.
bool CopyKey(const char* key, std::string& out, bool emptyOk = false) {
    if (!key) return false;
    const size_t n = strnlen(key, SCO_STORAGE_MAX_KEY + 1);
    if (n > SCO_STORAGE_MAX_KEY || (n == 0 && !emptyOk)) return false;
    out.assign(key, n);
    return true;
}

struct Param {
    uint32_t    type;
    int64_t     i = 0;
    double      f = 0;
    std::string bytes;
};

// Copies sql and params. False (with why) for a bad argument.
bool CopyStatement(const char* sql, const sco_sql_value* params, uint32_t n, std::string& text,
                   std::vector<Param>& out, const char*& why) {
    why = "bad arguments";
    if (!sql || (n && !params)) return false;
    const size_t len = strnlen(sql, static_cast<size_t>(kMaxSqlBytes) + 1);
    if (len > static_cast<size_t>(kMaxSqlBytes)) { why = "statement too long"; return false; }
    text.assign(sql, len);
    if (n > 32766) { why = "too many parameters"; return false; }   // SQLITE_MAX_VARIABLE_NUMBER
    out.resize(n);
    for (uint32_t k = 0; k < n; ++k) {
        const sco_sql_value& v = params[k];
        Param& p = out[k];
        p.type = v.type;
        switch (v.type) {
            case SCO_SQL_NULL:  break;
            case SCO_SQL_INT:   p.i = v.v.i; break;
            case SCO_SQL_FLOAT: p.f = v.v.f; break;
            case SCO_SQL_TEXT:
            case SCO_SQL_BLOB:
                if (!v.v.p && v.size) { why = "bad parameter"; return false; }
                if (v.size > static_cast<uint32_t>(kMaxSqlValueBytes)) { why = "parameter too big"; return false; }
                if (v.size) p.bytes.assign(static_cast<const char*>(v.v.p), v.size);
                break;
            default: why = "bad parameter"; return false;
        }
    }
    return true;
}

// ---- host statements --------------------------------------------------------------------------

// A prepared host statement over the key-value table, finalized on scope exit; runs trusted.
struct HostStmt {
    Db&           d;
    sqlite3_stmt* s = nullptr;
    int           rc;
    HostStmt(Db& db, const char* sql) : d(db) {
        d.trusted = true;
        rc = sqlite3_prepare_v3(d.db, sql, -1, 0, &s, nullptr);
    }
    ~HostStmt() {
        sqlite3_finalize(s);
        d.trusted = false;
    }
    HostStmt(const HostStmt&) = delete;
    HostStmt& operator=(const HostStmt&) = delete;
};

void BindText(sqlite3_stmt* s, int at, const std::string& v) {
    sqlite3_bind_text(s, at, v.data(), static_cast<int>(v.size()), SQLITE_STATIC);
}

// ---- key-value --------------------------------------------------------------------------------

sco_result Put(sco_plugin* self, const char* key, const void* value, uint32_t size) {
    return Guard([&] {
        std::string k, v;
        if (!CopyKey(key, k) || size > SCO_STORAGE_MAX_VALUE || (!value && size)) return Result::BadArg;
        if (size) v.assign(static_cast<const char*>(value), size);
        Call call;
        if (const Result r = Enter(self, call); r != Result::Ok) return r;
        Db& d = *call.db;
        HostStmt q(d, "INSERT OR REPLACE INTO sco_kv(key, value) VALUES (?1, ?2)");
        if (q.rc != SQLITE_OK) return Fail(d, q.rc, Result::Failed);
        BindText(q.s, 1, k);
        if (size) sqlite3_bind_blob(q.s, 2, v.data(), static_cast<int>(v.size()), SQLITE_STATIC);
        else sqlite3_bind_zeroblob(q.s, 2, 0);   // a null pointer would bind NULL
        const int rc = sqlite3_step(q.s);
        return rc == SQLITE_DONE ? Result::Ok : Fail(d, rc, Result::Failed);
    });
}

sco_result Get(sco_plugin* self, const char* key, void* out, uint32_t* io) {
    return Guard([&] {
        uint32_t capacity = 0;
        std::string k, value;
        if (!TakeBuffer(out, io, capacity) || !CopyKey(key, k)) return Result::BadArg;
        Result r;
        {
            Call call;
            r = Enter(self, call);
            if (r == Result::Ok) {
                Db& d = *call.db;
                HostStmt q(d, "SELECT value FROM sco_kv WHERE key = ?1");
                if (q.rc != SQLITE_OK) return Fail(d, q.rc, Result::Failed);
                BindText(q.s, 1, k);
                const int rc = sqlite3_step(q.s);
                if (rc == SQLITE_DONE) return Result::NotFound;
                if (rc != SQLITE_ROW) return Fail(d, rc, Result::Failed);
                const void* bytes = sqlite3_column_blob(q.s, 0);
                const int n = sqlite3_column_bytes(q.s, 0);
                if (!bytes && n > 0) return Fail(d, SQLITE_NOMEM, Result::TooMany);
                if (n) value.assign(static_cast<const char*>(bytes), static_cast<size_t>(n));
            }
        }
        if (r != Result::Ok) return r;
        *io = capacity;
        return CopyOut(value.data(), value.size(), false, out, io);
    });
}

sco_result Del(sco_plugin* self, const char* key) {
    return Guard([&] {
        std::string k;
        if (!CopyKey(key, k)) return Result::BadArg;
        Call call;
        if (const Result r = Enter(self, call); r != Result::Ok) return r;
        Db& d = *call.db;
        HostStmt q(d, "DELETE FROM sco_kv WHERE key = ?1");
        if (q.rc != SQLITE_OK) return Fail(d, q.rc, Result::Failed);
        BindText(q.s, 1, k);
        const int rc = sqlite3_step(q.s);
        if (rc != SQLITE_DONE) return Fail(d, rc, Result::Failed);
        return sqlite3_changes(d.db) ? Result::Ok : Result::NotFound;
    });
}

// The smallest byte string greater than every string starting with prefix: drop trailing 0xFF
// bytes, then add one to the last. Empty when there is none (no upper bound).
std::string UpperBound(std::string p) {
    while (!p.empty() && static_cast<unsigned char>(p.back()) == 0xFF) p.pop_back();
    if (!p.empty()) p.back() = static_cast<char>(static_cast<unsigned char>(p.back()) + 1);
    return p;
}

sco_result NextKey(sco_plugin* self, const char* prefix, const char* after, char* out, uint32_t* io) {
    return Guard([&] {
        uint32_t capacity = 0;
        std::string p, a, key;
        if (!TakeBuffer(out, io, capacity)) return Result::BadArg;
        if (prefix && !CopyKey(prefix, p, true)) return Result::BadArg;
        if (after && !CopyKey(after, a)) return Result::BadArg;
        Result r;
        {
            Call call;
            r = Enter(self, call);
            if (r == Result::Ok) {
                Db& d = *call.db;
                const std::string upper = UpperBound(p);
                // Plain range conditions, so the primary key bounds the scan.
                std::string sql = "SELECT key FROM sco_kv WHERE key >= ?1";
                if (!upper.empty()) sql += " AND key < ?2";
                if (after) sql += " AND key > ?3";
                sql += " ORDER BY key LIMIT 1";
                HostStmt q(d, sql.c_str());
                if (q.rc != SQLITE_OK) return Fail(d, q.rc, Result::Failed);
                BindText(q.s, 1, p);
                if (!upper.empty()) BindText(q.s, 2, upper);
                if (after) BindText(q.s, 3, a);
                const int rc = sqlite3_step(q.s);
                if (rc == SQLITE_DONE) return Result::NotFound;
                if (rc != SQLITE_ROW) return Fail(d, rc, Result::Failed);
                const unsigned char* k = sqlite3_column_text(q.s, 0);
                if (!k) return Fail(d, SQLITE_NOMEM, Result::TooMany);
                key.assign(reinterpret_cast<const char*>(k), static_cast<size_t>(sqlite3_column_bytes(q.s, 0)));
            }
        }
        if (r != Result::Ok) return r;
        *io = capacity;
        return CopyOut(key.data(), key.size(), true, out, io);
    });
}

// ---- transactions -----------------------------------------------------------------------------

Result TxnStatement(sco_plugin* self, bool wantOpen, const char* sql, const char* wrongState) {
    Call call;
    if (const Result r = Enter(self, call); r != Result::Ok) return r;
    Db& d = *call.db;
    const bool open = !sqlite3_get_autocommit(d.db);
    if (open != wantOpen) return Refuse(d, wrongState);
    const int rc = Trusted(d, sql);
    return rc == SQLITE_OK ? Result::Ok : Fail(d, rc, Result::Failed);
}

sco_result Begin(sco_plugin* self) {
    // IMMEDIATE takes the write lock now, so a later write can't fail to upgrade.
    return Guard([&] { return TxnStatement(self, false, "BEGIN IMMEDIATE", "a transaction is already open"); });
}

sco_result Commit(sco_plugin* self) {
    return Guard([&] { return TxnStatement(self, true, "COMMIT", "no transaction is open"); });
}

sco_result Rollback(sco_plugin* self) {
    return Guard([&] { return TxnStatement(self, true, "ROLLBACK", "no transaction is open"); });
}

// ---- SQL --------------------------------------------------------------------------------------

// True when sql's first word is VACUUM (it rewrites the file; VACUUM INTO writes anywhere).
// ATTACH being limited to 0 databases stops it as well; this gives the clear refusal.
bool IsVacuum(const std::string& sql) {
    size_t i = 0;
    while (i < sql.size() && (sql[i] == ' ' || sql[i] == '\t' || sql[i] == '\r' || sql[i] == '\n')) ++i;
    if (sql.size() - i < 6 || sqlite3_strnicmp(sql.c_str() + i, "vacuum", 6) != 0) return false;
    const char c = i + 6 < sql.size() ? sql[i + 6] : ' ';
    return !((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_');
}

// Prepares exactly one plugin statement and binds its parameters. On Ok *out holds it.
Result Prepare(Db& d, const std::string& sql, const std::vector<Param>& params, sqlite3_stmt** out) {
    *out = nullptr;
    if (IsVacuum(sql)) return Refuse(d, "VACUUM is not allowed");
    sqlite3_stmt* s = nullptr;
    const char* tail = nullptr;
    int rc = sqlite3_prepare_v3(d.db, sql.c_str(), static_cast<int>(sql.size()), 0, &s, &tail);
    if (rc != SQLITE_OK) return Fail(d, rc, Result::BadArg);
    if (!s) return Refuse(d, "no statement");
    if (tail && *tail) {   // only whitespace and comments may follow
        sqlite3_stmt* extra = nullptr;
        rc = sqlite3_prepare_v3(d.db, tail, -1, 0, &extra, nullptr);
        const bool more = rc != SQLITE_OK || extra;
        sqlite3_finalize(extra);
        if (more) {
            sqlite3_finalize(s);
            return Refuse(d, "one statement per call");
        }
    }
    if (static_cast<size_t>(sqlite3_bind_parameter_count(s)) != params.size()) {
        sqlite3_finalize(s);
        return Refuse(d, "parameter count does not match the statement");
    }
    for (size_t k = 0; k < params.size(); ++k) {
        const Param& p = params[k];
        const int at = static_cast<int>(k) + 1;
        int b = SQLITE_OK;
        switch (p.type) {
            case SCO_SQL_NULL:  b = sqlite3_bind_null(s, at); break;
            case SCO_SQL_INT:   b = sqlite3_bind_int64(s, at, p.i); break;
            case SCO_SQL_FLOAT: b = sqlite3_bind_double(s, at, p.f); break;
            case SCO_SQL_TEXT:  b = sqlite3_bind_text(s, at, p.bytes.data(), static_cast<int>(p.bytes.size()), SQLITE_TRANSIENT); break;
            default:
                b = p.bytes.empty() ? sqlite3_bind_zeroblob(s, at, 0)
                                    : sqlite3_bind_blob(s, at, p.bytes.data(), static_cast<int>(p.bytes.size()), SQLITE_TRANSIENT);
                break;
        }
        if (b != SQLITE_OK) {
            sqlite3_finalize(s);
            return Fail(d, b, Result::BadArg);
        }
    }
    *out = s;
    return Result::Ok;
}

sco_result Exec(sco_plugin* self, const char* sql, const sco_sql_value* params, uint32_t n, int64_t* changes) {
    return Guard([&] {
        if (changes) *changes = 0;
        std::string text;
        std::vector<Param> ps;
        const char* why = nullptr;
        const bool copied = CopyStatement(sql, params, n, text, ps, why);
        int64_t count = 0;
        Result r;
        {
            Call call;
            r = Enter(self, call);
            if (r != Result::Ok) return r;
            Db& d = *call.db;
            if (!copied) return Refuse(d, why);
            sqlite3_stmt* s = nullptr;
            r = Prepare(d, text, ps, &s);
            if (r != Result::Ok) return r;
            int rc;
            while ((rc = sqlite3_step(s)) == SQLITE_ROW) {}
            if (rc != SQLITE_DONE) r = Fail(d, rc, Result::Failed);
            else if (!sqlite3_stmt_readonly(s)) count = sqlite3_changes64(d.db);
            sqlite3_finalize(s);
        }
        if (r == Result::Ok && changes) *changes = count;
        return r;
    });
}

sco_result Query(sco_plugin* self, const char* sql, const sco_sql_value* params, uint32_t n, uint64_t* cursor) {
    return Guard([&] {
        if (!cursor) return Result::BadArg;
        *cursor = 0;
        std::string text;
        std::vector<Param> ps;
        const char* why = nullptr;
        const bool copied = CopyStatement(sql, params, n, text, ps, why);
        uint64_t id = 0;
        {
            Call call;
            if (const Result r = Enter(self, call); r != Result::Ok) return r;
            Db& d = *call.db;
            if (!copied) return Refuse(d, why);
            if (d.cursors.size() >= SCO_STORAGE_MAX_CURSORS) {
                d.error = "too many open cursors";
                return Result::TooMany;
            }
            sqlite3_stmt* s = nullptr;
            if (const Result r = Prepare(d, text, ps, &s); r != Result::Ok) return r;
            id = g_nextCursor.fetch_add(1);
            try {
                d.cursors.emplace(id, Cursor{ s, false });
            } catch (...) {
                sqlite3_finalize(s);
                throw;
            }
        }
        *cursor = id;
        return Result::Ok;
    });
}

Cursor* FindCursor(Db& d, uint64_t id) {
    const auto it = d.cursors.find(id);
    return it == d.cursors.end() ? nullptr : &it->second;
}

sco_result Step(sco_plugin* self, uint64_t cursor) {
    return Guard([&] {
        Call call;
        if (const Result r = Enter(self, call); r != Result::Ok) return r;
        Db& d = *call.db;
        Cursor* c = FindCursor(d, cursor);
        if (!c) return Result::NotFound;
        const int rc = sqlite3_step(c->stmt);
        c->row = rc == SQLITE_ROW;
        if (rc == SQLITE_ROW) return Result::Ok;
        if (rc == SQLITE_DONE) return Result::NotFound;
        return Fail(d, rc, Result::Failed);
    });
}

sco_result ColumnCount(sco_plugin* self, uint64_t cursor, uint32_t* count) {
    return Guard([&] {
        if (!count) return Result::BadArg;
        *count = 0;
        uint32_t n = 0;
        {
            Call call;
            if (const Result r = Enter(self, call); r != Result::Ok) return r;
            Cursor* c = FindCursor(*call.db, cursor);
            if (!c) return Result::NotFound;
            n = static_cast<uint32_t>(sqlite3_column_count(c->stmt));
        }
        *count = n;
        return Result::Ok;
    });
}

sco_result Column(sco_plugin* self, uint64_t cursor, uint32_t index, sco_sql_value* out, void* buf, uint32_t* io) {
    return Guard([&] {
        if (!out) {
            if (io) *io = 0;
            return Result::BadArg;
        }
        *out = sco_sql_value{};
        uint32_t capacity = 0;
        if (io && !TakeBuffer(buf, io, capacity)) return Result::BadArg;
        sco_sql_value v{};
        std::string bytes;
        {
            Call call;
            if (const Result r = Enter(self, call); r != Result::Ok) return r;
            Db& d = *call.db;
            Cursor* c = FindCursor(d, cursor);
            if (!c) return Result::NotFound;
            if (!c->row) return Refuse(d, "no current row");
            if (index >= static_cast<uint32_t>(sqlite3_column_count(c->stmt))) return Refuse(d, "column index out of range");
            const int i = static_cast<int>(index);
            const int type = sqlite3_column_type(c->stmt, i);
            if (type == SQLITE_INTEGER) {
                v.type = SCO_SQL_INT;
                v.v.i = sqlite3_column_int64(c->stmt, i);
            } else if (type == SQLITE_FLOAT) {
                v.type = SCO_SQL_FLOAT;
                v.v.f = sqlite3_column_double(c->stmt, i);
            } else if (type == SQLITE_NULL) {
                v.type = SCO_SQL_NULL;
            } else {
                const bool text = type == SQLITE_TEXT;
                const void* p = text ? static_cast<const void*>(sqlite3_column_text(c->stmt, i)) : sqlite3_column_blob(c->stmt, i);
                const int n = sqlite3_column_bytes(c->stmt, i);
                if (!p && (text || n > 0)) return Fail(d, SQLITE_NOMEM, Result::TooMany);
                v.type = text ? SCO_SQL_TEXT : SCO_SQL_BLOB;
                v.size = static_cast<uint32_t>(n);
                if (io && n) bytes.assign(static_cast<const char*>(p), static_cast<size_t>(n));
            }
        }
        *out = v;
        if (!io || (v.type != SCO_SQL_TEXT && v.type != SCO_SQL_BLOB)) return Result::Ok;
        *io = capacity;
        return CopyOut(bytes.data(), bytes.size(), v.type == SCO_SQL_TEXT, buf, io);
    });
}

sco_result ColumnName(sco_plugin* self, uint64_t cursor, uint32_t index, char* out, uint32_t* io) {
    return Guard([&] {
        uint32_t capacity = 0;
        if (!TakeBuffer(out, io, capacity)) return Result::BadArg;
        std::string name;
        {
            Call call;
            if (const Result r = Enter(self, call); r != Result::Ok) return r;
            Db& d = *call.db;
            Cursor* c = FindCursor(d, cursor);
            if (!c) return Result::NotFound;
            if (index >= static_cast<uint32_t>(sqlite3_column_count(c->stmt))) return Refuse(d, "column index out of range");
            const char* p = sqlite3_column_name(c->stmt, static_cast<int>(index));
            if (!p) return Fail(d, SQLITE_NOMEM, Result::TooMany);
            name = p;
        }
        *io = capacity;
        return CopyOut(name.data(), name.size(), true, out, io);
    });
}

sco_result CloseCursor(sco_plugin* self, uint64_t cursor) {
    return Guard([&] {
        Call call;
        if (const Result r = Enter(self, call); r != Result::Ok) return r;
        Db& d = *call.db;
        const auto it = d.cursors.find(cursor);
        if (it == d.cursors.end()) return Result::NotFound;
        sqlite3_finalize(it->second.stmt);
        d.cursors.erase(it);
        return Result::Ok;
    });
}

sco_result LastError(sco_plugin* self, char* out, uint32_t* io) {
    return Guard([&] {
        uint32_t capacity = 0;
        if (!TakeBuffer(out, io, capacity)) return Result::BadArg;
        std::string error;
        {
            Call call;
            if (const Result r = Enter(self, call, false); r != Result::Ok) return r;
            error = call.db->error;
        }
        *io = capacity;
        return CopyOut(error.data(), error.size(), true, out, io);
    });
}

const sco_storage_v1 kTable = {
    sizeof(sco_storage_v1), 0,
    Put, Get, Del, NextKey,
    Begin, Commit, Rollback,
    Exec, Query, Step, ColumnCount, Column, ColumnName, CloseCursor,
    LastError,
};

// ---- lifetime ---------------------------------------------------------------------------------

void CloseDb(Db& d, Result why) {
    std::lock_guard<std::mutex> hold(d.lock);
    if (d.closed) return;
    d.closed = true;
    d.closedResult = why;
    CloseConnection(d);
}

// The runtime calls this once Release(owner) has removed the owner's items.
void OnRelease(const void* owner) {
    std::shared_ptr<Db> d;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (size_t i = 0; i < g_dbs.size(); ++i)
            if (g_dbs[i]->owner == owner) {
                d = std::move(g_dbs[i]);
                g_dbs.erase(g_dbs.begin() + static_cast<std::ptrdiff_t>(i));
                break;
            }
    }
    if (d) CloseDb(*d, Result::BadArg);
}

}  // namespace

Result Start(const Options& opts) {
    if (opts.dataRoot.empty() || opts.budgetMs == 0) return Result::BadArg;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (g_started) return Result::BadArg;
        try {
            g_opts = opts;
        } catch (...) {
            return Result::TooMany;
        }
        g_started = true;
    }
    Result r = AddReleaseHook(OnRelease);
    if (r == Result::Ok) {
        r = host::ProvideHostService(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0, &kTable);
        if (r != Result::Ok) RemoveReleaseHook(OnRelease);
    }
    if (r != Result::Ok) {
        std::lock_guard<std::mutex> hold(g_lock);
        g_started = false;
    }
    return r;
}

void Stop() {
    std::vector<std::shared_ptr<Db>> dbs;
    {
        std::lock_guard<std::mutex> hold(g_lock);
        if (!g_started) return;
        g_started = false;
        dbs.swap(g_dbs);
    }
    host::WithdrawHostService(SCO_STORAGE_NAME);
    for (const auto& d : dbs) CloseDb(*d, Result::Unavailable);
    RemoveReleaseHook(OnRelease);
}

bool Started() {
    std::lock_guard<std::mutex> hold(g_lock);
    return g_started;
}

const sco_storage_v1* Table() { return &kTable; }

fs::path DatabasePath(const char* id) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (!g_started || !id) return {};
    return g_opts.dataRoot / "storage" / (std::string(id) + ".db");
}

}  // namespace sco::storage
