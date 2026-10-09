// Unit tests for host-owned services and the sco.storage service (sco/storage.h, sco_storage.h,
// scosdk/storage.hpp), through the real host table and the vendored SQLite. Host build, no game.
// The "game thread" is this test's main thread.
//   test_storage <out dir>                      (tools/test.sh, CTest test_storage)
//   test_storage --child <root> <scenario>      (spawned by the crash tests; killed mid-transaction)
#include "sco/host.h"
#include "sco/runtime.h"
#include "sco/storage.h"
#include "sco_api.h"
#include "sco_storage.h"
#include "scosdk/storage.hpp"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <csignal>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;
using sco::Result;

static std::atomic<int> g_fail{ 0 }, g_pass{ 0 };
#define CHECK(c) do { if (c) ++g_pass; else { ++g_fail; std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static const sco_api*        g_api = nullptr;
static const sco_storage_v1* g_st = nullptr;
static fs::path              g_root;
static std::string           g_exe;

static const sco_storage_v1* QueryStorage(uint32_t version = SCO_STORAGE_VERSION_1_0, sco_result* r = nullptr) {
    const void* t = nullptr;
    const sco_result got = g_api->query_service(SCO_STORAGE_NAME, version, &t);
    if (r) *r = got;
    return static_cast<const sco_storage_v1*>(t);
}

static std::string Get(sco_plugin* p, const char* key, sco_result* r = nullptr) {
    char buf[256];
    uint32_t size = sizeof(buf);
    const sco_result got = g_st->get(p, key, buf, &size);
    if (r) *r = got;
    return got == SCO_OK ? std::string(buf, size) : std::string("<") + std::to_string(got) + ">";
}

static sco_result Put(sco_plugin* p, const char* key, const std::string& v) {
    return g_st->put(p, key, v.data(), static_cast<uint32_t>(v.size()));
}

static std::string LastError(sco_plugin* p) {
    char buf[512];
    uint32_t size = sizeof(buf);
    return g_st->last_error(p, buf, &size) == SCO_OK ? std::string(buf) : "<none>";
}

static sco_result Exec(sco_plugin* p, const char* sql) { return g_st->exec(p, sql, nullptr, 0, nullptr); }

// The first column of the first row as an integer (-1 when there is none).
static int64_t QueryInt(sco_plugin* p, const char* sql) {
    uint64_t c = 0;
    if (g_st->query(p, sql, nullptr, 0, &c) != SCO_OK) return -1;
    int64_t v = -1;
    sco_sql_value out{};
    if (g_st->step(p, c) == SCO_OK && g_st->column(p, c, 0, &out, nullptr, nullptr) == SCO_OK && out.type == SCO_SQL_INT) v = out.v.i;
    g_st->close(p, c);
    return v;
}

static std::string QueryText(sco_plugin* p, const char* sql) {
    sco::sdk::Storage s;
    if (s.Open(g_api, p) != SCO_OK) return "<no storage>";
    sco::sdk::StorageCursor c = s.Query(sql);
    std::string v = "<none>";
    if (c.Next() == SCO_OK) c.Text(0, v);
    return v;
}

static sco::storage::Options Opts() {
    sco::storage::Options o;
    o.dataRoot = g_root;
    return o;
}

// ---- host-owned services --------------------------------------------------------------------

static void TestHostServices() {
    static const struct { uint32_t size; } kTable = { 4 };
    CHECK(sco::host::ProvideHostService("sco.test", 0x00010002, &kTable) == Result::Ok);
    const void* t = nullptr;
    CHECK(g_api->query_service("sco.test", 0x00010000, &t) == SCO_OK && t == &kTable);
    CHECK(sco::host::ProvideHostService("sco.test", 0x00010000, &kTable) == Result::BadArg);   // taken
    for (const char* bad : { "sco", "other.test", "scox.test", "sco.", "sco..x", "sco.Bad", "" })
        CHECK(sco::host::ProvideHostService(bad, 0x00010000, &kTable) == Result::BadArg);
    CHECK(sco::host::ProvideHostService(nullptr, 0x00010000, &kTable) == Result::BadArg);
    CHECK(sco::host::ProvideHostService("sco.null", 0x00010000, nullptr) == Result::BadArg);

    // No plugin can publish under sco: the id is refused, and another id can't reach sco.<name>.
    CHECK(sco::host::NewPlugin(sco::host::kHostId) == nullptr);
    sco_plugin* p = sco::host::NewPlugin("svc");
    CHECK(p && g_api->provide_service(p, "sco.fake", 0x00010000, &kTable) == SCO_BAD_ARG);
    CHECK(g_api->release_service(p, "sco.test") == SCO_NOT_FOUND);   // not the plugin's
    CHECK(sco::Release(p) == Result::Ok);
    CHECK(g_api->query_service("sco.test", 0x00010000, &t) == SCO_OK);   // a plugin's release leaves it

    CHECK(sco::host::WithdrawHostService("sco.test") == Result::Ok);
    CHECK(sco::host::WithdrawHostService("sco.test") == Result::NotFound);
    CHECK(g_api->query_service("sco.test", 0x00010000, &t) == SCO_NOT_FOUND && t == nullptr);
    // The host owner is never released: withdraw-all, then publish again (the next start).
    CHECK(sco::host::ProvideHostService("sco.a", 0x00010000, &kTable) == Result::Ok);
    CHECK(sco::host::ProvideHostService("sco.b", 0x00010000, &kTable) == Result::Ok);
    CHECK(sco::host::WithdrawHostServices() == 2 && sco::host::WithdrawHostServices() == 0);
    CHECK(sco::host::ProvideHostService("sco.a", 0x00010000, &kTable) == Result::Ok);
    CHECK(sco::host::WithdrawHostService("sco.a") == Result::Ok);
}

// ---- the service ----------------------------------------------------------------------------

static void TestService() {
    sco_result r;
    CHECK(QueryStorage(SCO_STORAGE_VERSION_1_0, &r) == nullptr && r == SCO_NOT_FOUND);   // not started
    CHECK(sco::storage::Start(sco::storage::Options{}) == Result::BadArg);                // no root
    CHECK(sco::storage::Start(Opts()) == Result::Ok);
    CHECK(sco::storage::Start(Opts()) == Result::BadArg);                                 // already
    g_st = QueryStorage(SCO_STORAGE_VERSION_1_0, &r);
    CHECK(r == SCO_OK && g_st == sco::storage::Table() && g_st->size == sizeof(sco_storage_v1));
    CHECK(QueryStorage(0x00010001, &r) == nullptr && r == SCO_UNAVAILABLE);   // 1.0 is older
    CHECK(QueryStorage(0x00020000, &r) == nullptr && r == SCO_UNAVAILABLE);
    CHECK(sco::storage::DatabasePath("alpha") == g_root / "storage" / "alpha.db");
    // A pointer the host didn't hand out is refused before anything is opened.
    sco_plugin* fake = reinterpret_cast<sco_plugin*>(&r);
    CHECK(g_st->put(fake, "k", "v", 1) == SCO_BAD_ARG);
}

// ---- key-value ------------------------------------------------------------------------------

static void TestKeyValue(sco_plugin* a) {
    CHECK(Put(a, "name", "Pilot") == SCO_OK);
    CHECK(Get(a, "name") == "Pilot");
    CHECK(fs::exists(g_root / "storage" / "alpha.db"));
    CHECK(Put(a, "name", "Pilot One") == SCO_OK && Get(a, "name") == "Pilot One");   // replaced

    // The size handshake: ask, too small, exact.
    uint32_t size = 0;
    CHECK(g_st->get(a, "name", nullptr, &size) == SCO_TOO_MANY && size == 9);
    char tiny[4];
    size = sizeof(tiny);
    CHECK(g_st->get(a, "name", tiny, &size) == SCO_TOO_MANY && size == 9);
    char exact[9];
    size = sizeof(exact);
    CHECK(g_st->get(a, "name", exact, &size) == SCO_OK && size == 9 && std::memcmp(exact, "Pilot One", 9) == 0);
    size = 7;
    CHECK(g_st->get(a, "name", nullptr, &size) == SCO_BAD_ARG && size == 0);   // NULL with a capacity
    CHECK(g_st->get(a, "name", exact, nullptr) == SCO_BAD_ARG);
    size = 5;
    CHECK(g_st->get(a, "missing", exact, &size) == SCO_NOT_FOUND && size == 0);

    // Empty values, binary values, the limits.
    CHECK(g_st->put(a, "empty", nullptr, 0) == SCO_OK);
    size = 0;
    CHECK(g_st->get(a, "empty", nullptr, &size) == SCO_OK && size == 0);
    const char bin[4] = { 0, '\xff', 0, 1 };
    CHECK(g_st->put(a, "bin", bin, 4) == SCO_OK && Get(a, "bin") == std::string(bin, 4));
    CHECK(g_st->put(a, "x", nullptr, 3) == SCO_BAD_ARG);
    const std::string key255(255, 'k'), key256(256, 'k');
    CHECK(Put(a, key255.c_str(), "v") == SCO_OK && Get(a, key255.c_str()) == "v");
    CHECK(Put(a, key256.c_str(), "v") == SCO_BAD_ARG);
    CHECK(Put(a, "", "v") == SCO_BAD_ARG && g_st->put(a, nullptr, "v", 1) == SCO_BAD_ARG);
    const std::string mib(SCO_STORAGE_MAX_VALUE, 'm');
    CHECK(Put(a, "big", mib) == SCO_OK);
    CHECK(g_st->put(a, "big", mib.data(), SCO_STORAGE_MAX_VALUE + 1) == SCO_BAD_ARG);
    size = 0;
    CHECK(g_st->get(a, "big", nullptr, &size) == SCO_TOO_MANY && size == SCO_STORAGE_MAX_VALUE);

    // del
    CHECK(g_st->del(a, "big") == SCO_OK && g_st->del(a, "big") == SCO_NOT_FOUND);

    // next_key: byte order, prefixes (one ending in 0xFF), resuming after a key, the handshake.
    for (const char* k : { "a.1", "a.2", "a.\xff", "a.\xff\xff", "ab", "b", "\xff\xff" }) CHECK(Put(a, k, "1") == SCO_OK);
    auto keys = [&](const char* prefix) {
        std::vector<std::string> out;
        sco::sdk::Storage s;
        if (s.Open(g_api, a) != SCO_OK || s.Keys(prefix, out) != SCO_OK) out.push_back("<error>");
        return out;
    };
    CHECK((keys("a.") == std::vector<std::string>{ "a.1", "a.2", "a.\xff", "a.\xff\xff" }));
    CHECK((keys("a.\xff") == std::vector<std::string>{ "a.\xff", "a.\xff\xff" }));
    CHECK((keys("\xff") == std::vector<std::string>{ "\xff\xff" }));
    CHECK(keys("zz").empty());
    CHECK(keys("").size() == 11 && keys(nullptr).size() == 11);   // name, empty, bin, key255 and the 7
    char kb[8];
    size = sizeof(kb);
    CHECK(g_st->next_key(a, "a.", "a.1", kb, &size) == SCO_OK && size == 4 && std::strcmp(kb, "a.2") == 0);
    size = 0;
    CHECK(g_st->next_key(a, "a.", nullptr, nullptr, &size) == SCO_TOO_MANY && size == 4);   // "a.1" + NUL
    size = sizeof(kb);
    CHECK(g_st->next_key(a, "b", "bin", kb, &size) == SCO_NOT_FOUND);
    CHECK(g_st->next_key(a, key256.c_str(), nullptr, kb, &size) == SCO_BAD_ARG);

    // The SDK forms: typed values, strings of any size.
    struct Spot { double x, y, z; uint32_t zone; };
    sco::sdk::Storage s;
    CHECK(s.Open(g_api, a) == SCO_OK && s);
    const Spot home{ 1.5, -2.0, 3.25, 7 };
    CHECK(s.Put("spot.home", home) == SCO_OK);
    Spot got{};
    CHECK(s.Get("spot.home", got) == SCO_OK && got.x == 1.5 && got.zone == 7);
    uint32_t wrong = 0;
    CHECK(s.Get("spot.home", wrong) == SCO_BAD_ARG);   // stored size != sizeof
    std::string text;
    CHECK(s.Put("long", std::string(5000, 'L')) == SCO_OK && s.Get("long", text) == SCO_OK && text == std::string(5000, 'L'));
    CHECK(s.Get("missing", text) == SCO_NOT_FOUND);
    CHECK(s.Delete("long") == SCO_OK);
}

// ---- transactions ---------------------------------------------------------------------------

static void TestTransactions(sco_plugin* a) {
    CHECK(g_st->commit(a) == SCO_BAD_ARG && LastError(a) == "no transaction is open");
    CHECK(g_st->rollback(a) == SCO_BAD_ARG);
    CHECK(g_st->begin(a) == SCO_OK);
    CHECK(g_st->begin(a) == SCO_BAD_ARG && LastError(a) == "a transaction is already open");
    CHECK(Put(a, "tx", "rolled back") == SCO_OK && Get(a, "tx") == "rolled back");   // visible inside
    CHECK(Exec(a, "CREATE TABLE tx_table(x)") == SCO_OK);
    CHECK(g_st->rollback(a) == SCO_OK);
    sco_result r;
    Get(a, "tx", &r);
    CHECK(r == SCO_NOT_FOUND);
    CHECK(QueryInt(a, "SELECT count(*) FROM sqlite_schema WHERE name = 'tx_table'") == 0);

    sco::sdk::Storage s;
    CHECK(s.Open(g_api, a) == SCO_OK);
    {
        sco::sdk::StorageTransaction tx(s);
        CHECK(tx.Result() == SCO_OK);
        CHECK(s.Put("tx", "committed") == SCO_OK);
        CHECK(tx.Commit() == SCO_OK);
    }
    {
        sco::sdk::StorageTransaction tx(s);   // destroyed without Commit: rolled back
        CHECK(s.Put("tx", "dropped") == SCO_OK);
    }
    CHECK(Get(a, "tx") == "committed");
}

// ---- SQL ------------------------------------------------------------------------------------

static void TestSql(sco_plugin* a) {
    CHECK(Exec(a, "CREATE TABLE ships(id INTEGER PRIMARY KEY, name TEXT UNIQUE, mass REAL, data BLOB, note TEXT)") == SCO_OK);
    const char blob[3] = { 1, 0, 2 };
    const sco_sql_value row[] = { sco::sdk::SqlInt(1), sco::sdk::SqlText("Cutlass Black"), sco::sdk::SqlFloat(25.5),
                                  sco::sdk::SqlBlob(std::as_bytes(std::span(blob))), sco::sdk::SqlNull() };
    int64_t changes = -1;
    CHECK(g_st->exec(a, "INSERT INTO ships VALUES (?, ?, ?, ?, ?)", row, 5, &changes) == SCO_OK && changes == 1);
    CHECK(g_st->exec(a, "INSERT INTO ships VALUES (?, ?, ?, ?, ?)", row, 4, nullptr) == SCO_BAD_ARG);   // count
    CHECK(LastError(a) == "parameter count does not match the statement");
    // A constraint is a failure of a statement that ran.
    CHECK(g_st->exec(a, "INSERT INTO ships VALUES (?, ?, ?, ?, ?)", row, 5, nullptr) == SCO_FAILED);
    CHECK(LastError(a).find("UNIQUE") != std::string::npos);
    CHECK(Exec(a, "SELEC 1") == SCO_BAD_ARG && LastError(a).find("syntax error") != std::string::npos);
    CHECK(Exec(a, "SELECT 1; SELECT 2") == SCO_BAD_ARG && LastError(a) == "one statement per call");
    CHECK(Exec(a, "SELECT 1; -- trailing comment") == SCO_OK);
    CHECK(Exec(a, "") == SCO_BAD_ARG && g_st->exec(a, nullptr, nullptr, 0, nullptr) == SCO_BAD_ARG);
    CHECK(g_st->exec(a, "SELECT ?", nullptr, 1, nullptr) == SCO_BAD_ARG);
    sco_sql_value badText = sco::sdk::SqlText("x");
    badText.v.p = nullptr;
    CHECK(g_st->exec(a, "SELECT ?", &badText, 1, nullptr) == SCO_BAD_ARG);
    CHECK(g_st->exec(a, "UPDATE ships SET mass = mass + 1", nullptr, 0, &changes) == SCO_OK && changes == 1);
    CHECK(g_st->exec(a, "SELECT * FROM ships", nullptr, 0, &changes) == SCO_OK && changes == 0);

    uint64_t c = 0;
    CHECK(g_st->query(a, "SELECT id, name, mass, data, note FROM ships WHERE id = ?", row, 1, &c) == SCO_OK && c != 0);
    uint32_t n = 0;
    CHECK(g_st->column_count(a, c, &n) == SCO_OK && n == 5);
    sco_sql_value v{};
    CHECK(g_st->column(a, c, 0, &v, nullptr, nullptr) == SCO_BAD_ARG && LastError(a) == "no current row");
    CHECK(g_st->step(a, c) == SCO_OK);
    CHECK(g_st->column(a, c, 0, &v, nullptr, nullptr) == SCO_OK && v.type == SCO_SQL_INT && v.v.i == 1);
    CHECK(g_st->column(a, c, 2, &v, nullptr, nullptr) == SCO_OK && v.type == SCO_SQL_FLOAT && v.v.f == 26.5);
    CHECK(g_st->column(a, c, 4, &v, nullptr, nullptr) == SCO_OK && v.type == SCO_SQL_NULL);
    // TEXT: the size, then too small, then exact (NUL included); never a pointer.
    CHECK(g_st->column(a, c, 1, &v, nullptr, nullptr) == SCO_OK && v.type == SCO_SQL_TEXT && v.size == 13 && v.v.p == nullptr);
    uint32_t size = 0;
    CHECK(g_st->column(a, c, 1, &v, nullptr, &size) == SCO_TOO_MANY && size == 14);
    char name[14];
    size = 13;
    CHECK(g_st->column(a, c, 1, &v, name, &size) == SCO_TOO_MANY && size == 14);
    size = sizeof(name);
    CHECK(g_st->column(a, c, 1, &v, name, &size) == SCO_OK && size == 14 && std::strcmp(name, "Cutlass Black") == 0);
    char bytes[3];
    size = sizeof(bytes);
    CHECK(g_st->column(a, c, 3, &v, bytes, &size) == SCO_OK && v.type == SCO_SQL_BLOB && size == 3 && std::memcmp(bytes, blob, 3) == 0);
    CHECK(g_st->column(a, c, 5, &v, nullptr, nullptr) == SCO_BAD_ARG);
    char col[8];
    size = sizeof(col);
    CHECK(g_st->column_name(a, c, 1, col, &size) == SCO_OK && std::strcmp(col, "name") == 0 && size == 5);
    CHECK(g_st->step(a, c) == SCO_NOT_FOUND);
    CHECK(g_st->close(a, c) == SCO_OK && g_st->close(a, c) == SCO_NOT_FOUND && g_st->step(a, c) == SCO_NOT_FOUND);

    // The SDK cursor.
    CHECK(QueryText(a, "SELECT name FROM ships") == "Cutlass Black");

    // At most SCO_STORAGE_MAX_CURSORS open.
    std::vector<uint64_t> open;
    for (uint32_t i = 0; i < SCO_STORAGE_MAX_CURSORS; ++i) {
        uint64_t id = 0;
        if (g_st->query(a, "SELECT 1", nullptr, 0, &id) == SCO_OK) open.push_back(id);
    }
    CHECK(open.size() == SCO_STORAGE_MAX_CURSORS);
    uint64_t extra = 0;
    CHECK(g_st->query(a, "SELECT 1", nullptr, 0, &extra) == SCO_TOO_MANY && extra == 0);
    for (uint64_t id : open) g_st->close(a, id);

    // The time budget stops a runaway statement.
    CHECK(Exec(a, "WITH RECURSIVE c(x) AS (SELECT 1 UNION ALL SELECT x + 1 FROM c) SELECT count(*) FROM c") == SCO_FAILED);
    CHECK(LastError(a).find("time budget") != std::string::npos);
    CHECK(QueryInt(a, "SELECT count(*) FROM ships") == 1);   // the connection still works
}

// ---- isolation ------------------------------------------------------------------------------

static void TestIsolation(sco_plugin* a, sco_plugin* b) {
    CHECK(Put(a, "secret", "alpha's") == SCO_OK);
    sco_result r;
    Get(b, "secret", &r);
    CHECK(r == SCO_NOT_FOUND);
    CHECK(Put(b, "secret", "beta's") == SCO_OK && Get(a, "secret") == "alpha's" && Get(b, "secret") == "beta's");
    CHECK(fs::exists(g_root / "storage" / "beta.db"));
    CHECK(Exec(b, "CREATE TABLE ships(x)") == SCO_OK);   // its own table of that name

    // No way into another file: ATTACH (any spelling, URIs too), VACUUM INTO.
    const std::string alphaDb = (g_root / "storage" / "alpha.db").generic_string();
    const std::string attach = "ATTACH DATABASE '" + alphaDb + "' AS a";
    CHECK(Exec(b, attach.c_str()) == SCO_BAD_ARG && LastError(b).find("not authorized") != std::string::npos);
    const std::string uri = "ATTACH 'file:" + alphaDb + "?mode=ro' AS a";
    CHECK(Exec(b, uri.c_str()) == SCO_BAD_ARG);
    const sco_sql_value path = sco::sdk::SqlText(alphaDb);
    CHECK(g_st->exec(b, "ATTACH ? AS a", &path, 1, nullptr) == SCO_BAD_ARG);
    CHECK(Exec(b, "SELECT * FROM a.sco_kv") == SCO_BAD_ARG);
    CHECK(Exec(b, "DETACH a") == SCO_BAD_ARG);
    const fs::path copy = g_root / "copy.db";
    CHECK(Exec(b, ("VACUUM INTO '" + copy.generic_string() + "'").c_str()) == SCO_BAD_ARG);
    CHECK(Exec(b, ("/* c */ VACUUM INTO '" + copy.generic_string() + "'").c_str()) != SCO_OK);
    CHECK(!fs::exists(copy));
    CHECK(Exec(b, "VACUUM") == SCO_BAD_ARG);

    // PRAGMAs: introspection and the plugin's own settings yes; the host's settings no.
    for (const char* bad : { "PRAGMA journal_mode = DELETE", "PRAGMA synchronous = OFF", "PRAGMA writable_schema = 1",
                             "PRAGMA max_page_count = 100000000", "PRAGMA page_size = 512", "PRAGMA temp_store_directory = '/tmp'",
                             "PRAGMA cell_size_check = 0", "PRAGMA mmap_size = 1000000", "PRAGMA database_list" })
        CHECK(Exec(b, bad) == SCO_BAD_ARG);
    CHECK(QueryText(b, "PRAGMA journal_mode") == "<none>");   // reading it is refused too
    CHECK(Exec(b, "PRAGMA user_version = 3") == SCO_OK && QueryInt(b, "PRAGMA user_version") == 3);
    CHECK(QueryInt(a, "PRAGMA user_version") == 0);
    CHECK(QueryText(b, "PRAGMA integrity_check") == "ok");
    CHECK(QueryInt(b, "SELECT count(*) FROM pragma_table_info('sco_kv')") == 2);
    CHECK(QueryInt(b, "PRAGMA page_size") > 0);

    // Transactions only through begin/commit; sco_* schema is the host's.
    for (const char* bad : { "BEGIN", "COMMIT", "SAVEPOINT s", "RELEASE s", "DROP TABLE sco_kv", "CREATE TABLE sco_mine(x)",
                             "CREATE INDEX i ON sco_kv(value)", "CREATE TRIGGER t AFTER INSERT ON sco_kv BEGIN SELECT 1; END",
                             "ALTER TABLE sco_kv RENAME TO kv", "CREATE VIEW sco_v AS SELECT 1", "CREATE VIRTUAL TABLE v USING json_each" })
        CHECK(Exec(b, bad) == SCO_BAD_ARG);
    CHECK(QueryInt(b, "SELECT count(*) FROM sco_kv") == 1);   // reading its own key-value table is fine

    // Cursor ids are the opener's: another plugin can't step or close them.
    uint64_t c = 0;
    CHECK(g_st->query(a, "SELECT key FROM sco_kv", nullptr, 0, &c) == SCO_OK);
    CHECK(g_st->step(b, c) == SCO_NOT_FOUND && g_st->close(b, c) == SCO_NOT_FOUND);
    sco_sql_value v{};
    CHECK(g_st->column(b, c, 0, &v, nullptr, nullptr) == SCO_NOT_FOUND);
    CHECK(g_st->step(a, c) == SCO_OK && g_st->close(a, c) == SCO_OK);
}

// ---- quota ----------------------------------------------------------------------------------

static void TestQuota() {
    sco::storage::Stop();
    sco::storage::Options o = Opts();
    o.quotaBytes = 256 * 1024;
    CHECK(sco::storage::Start(o) == Result::Ok);
    sco_plugin* q = sco::host::NewPlugin("quota");
    const std::string chunk(16 * 1024, 'q');
    sco_result r = SCO_OK;
    int written = 0;
    for (int i = 0; i < 64 && r == SCO_OK; ++i) {
        r = Put(q, ("k" + std::to_string(i)).c_str(), chunk);
        if (r == SCO_OK) ++written;
    }
    CHECK(r == SCO_TOO_MANY && written > 4 && written < 16);
    CHECK(LastError(q).find("full") != std::string::npos);
    CHECK(g_st->del(q, "k0") == SCO_OK);   // deleting still works
    CHECK(sco::Release(q) == Result::Ok);
    sco::storage::Stop();
    CHECK(sco::storage::Start(Opts()) == Result::Ok);
}

// ---- unload and stop ------------------------------------------------------------------------

static void TestUnload() {
    sco_plugin* d = sco::host::NewPlugin("delta");
    CHECK(Put(d, "kept", "1") == SCO_OK);
    CHECK(g_st->begin(d) == SCO_OK && Put(d, "dropped", "1") == SCO_OK);
    uint64_t c = 0;
    CHECK(g_st->query(d, "SELECT key FROM sco_kv", nullptr, 0, &c) == SCO_OK);
    // Unload (or a crash): the transaction is rolled back, the cursor closed, the handle refused.
    CHECK(sco::Release(d) == Result::Ok);
    sco_result r;
    Get(d, "kept", &r);
    CHECK(r == SCO_BAD_ARG);
    CHECK(g_st->step(d, c) == SCO_BAD_ARG && g_st->close(d, c) == SCO_BAD_ARG && g_st->commit(d) == SCO_BAD_ARG);
    uint32_t size = 0;
    CHECK(g_st->last_error(d, nullptr, &size) == SCO_BAD_ARG);
    // The reload gets its committed data back, and none of the old handle's ids.
    sco_plugin* d2 = sco::host::NewPlugin("delta");
    CHECK(d2 && d2 != d && Get(d2, "kept") == "1");
    Get(d2, "dropped", &r);
    CHECK(r == SCO_NOT_FOUND);
    CHECK(g_st->step(d2, c) == SCO_NOT_FOUND && g_st->commit(d2) == SCO_BAD_ARG);

    // Stop withdraws the service; the table answers UNAVAILABLE; a restart finds the data.
    CHECK(g_st->begin(d2) == SCO_OK && Put(d2, "in flight", "1") == SCO_OK);
    sco::storage::Stop();
    CHECK(QueryStorage(SCO_STORAGE_VERSION_1_0, &r) == nullptr && r == SCO_NOT_FOUND);
    CHECK(Put(d2, "after stop", "1") == SCO_UNAVAILABLE && Get(d2, "kept") == "<1>");
    sco::storage::Stop();   // no-op
    CHECK(sco::storage::Start(Opts()) == Result::Ok && QueryStorage() == g_st);
    CHECK(Get(d2, "kept") == "1");
    Get(d2, "in flight", &r);
    CHECK(r == SCO_NOT_FOUND);   // Stop rolled it back
    CHECK(sco::Release(d2) == Result::Ok);
}

// ---- threads (ThreadSanitizer run) ----------------------------------------------------------

static void TestConcurrency() {
    sco_plugin* c1 = sco::host::NewPlugin("conc1");
    sco_plugin* c2 = sco::host::NewPlugin("conc2");
    constexpr int kPerThread = 60;
    std::atomic<int> bad{ 0 };
    auto writer = [&](sco_plugin* p, int t) {
        for (int i = 0; i < kPerThread; ++i) {
            const std::string key = "t" + std::to_string(t) + "." + std::to_string(i);
            if (Put(p, key.c_str(), key) != SCO_OK || Get(p, key.c_str()) != key) bad.fetch_add(1);
        }
    };
    // c2 batches in transactions from one thread (a transaction is the plugin's, not the thread's)
    // and runs SQL beside its own key-value writer.
    auto batcher = [&] {
        if (Exec(c2, "CREATE TABLE log(i INTEGER)") != SCO_OK) bad.fetch_add(1);
        for (int b = 0; b < 6; ++b) {
            if (g_st->begin(c2) != SCO_OK) { bad.fetch_add(1); continue; }
            for (int i = 0; i < 10; ++i) {
                const sco_sql_value v = sco::sdk::SqlInt(b * 10 + i);
                if (g_st->exec(c2, "INSERT INTO log VALUES (?)", &v, 1, nullptr) != SCO_OK) bad.fetch_add(1);
            }
            if (g_st->commit(c2) != SCO_OK) bad.fetch_add(1);
        }
    };
    std::vector<std::thread> threads;
    threads.emplace_back(writer, c1, 0);
    threads.emplace_back(writer, c1, 1);
    threads.emplace_back(writer, c2, 2);
    threads.emplace_back(batcher);
    for (auto& t : threads) t.join();
    CHECK(bad.load() == 0);
    sco::sdk::Storage s1, s2;
    std::vector<std::string> k1, k2;
    CHECK(s1.Open(g_api, c1) == SCO_OK && s1.Keys("t", k1) == SCO_OK && k1.size() == 2 * kPerThread);
    CHECK(s2.Open(g_api, c2) == SCO_OK && s2.Keys("t", k2) == SCO_OK && k2.size() == kPerThread);
    CHECK(QueryInt(c2, "SELECT count(*) FROM log") == 60);

    // Release on the game thread while another thread keeps calling: the calls end in BAD_ARG,
    // never touch a closed database.
    std::atomic<bool> released{ false };
    std::atomic<int> afterRelease{ 0 };
    std::thread caller([&] {
        int i = 0;
        while (afterRelease.load() < 3) {
            const sco_result r = Put(c1, ("late." + std::to_string(i++)).c_str(), "x");
            if (released.load() && r == SCO_BAD_ARG) afterRelease.fetch_add(1);
            else if (r != SCO_OK && r != SCO_BAD_ARG) bad.fetch_add(1);
        }
    });
    sco_result first = Put(c1, "before", "x");
    CHECK(first == SCO_OK);
    CHECK(sco::Release(c1) == Result::Ok);
    released.store(true);
    caller.join();
    CHECK(bad.load() == 0);
    CHECK(sco::Release(c2) == Result::Ok);
}

// ---- crash safety: a child process killed mid-transaction ------------------------------------

// Child scenarios. "open": commits state, opens a big transaction (spilling into the WAL), prints
// READY and waits to be killed. "loop": commits counter and mirror together in a loop, printing
// READY after the first commit, until killed.
static int ChildMain(const char* root, const char* scenario) {
    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "test_storage child" });
    g_root = root;
    if (sco::storage::Start(Opts()) != Result::Ok) return 2;
    g_st = QueryStorage();
    if (std::strcmp(scenario, "open") == 0) {
        sco_plugin* p = sco::host::NewPlugin("crash1");
        if (Put(p, "state", "committed") != SCO_OK) return 3;
        if (g_st->begin(p) != SCO_OK || Put(p, "state", "uncommitted") != SCO_OK) return 4;
        if (Exec(p, "CREATE TABLE half(x)") != SCO_OK) return 5;
        const std::string page(4096, 'j');
        for (int i = 0; i < 1500; ++i)   // ~6 MiB: more than the page cache, so it reaches the WAL
            if (Put(p, ("junk." + std::to_string(i)).c_str(), page) != SCO_OK) return 6;
    } else {
        sco_plugin* p = sco::host::NewPlugin("crash2");
        for (int64_t i = 1;; ++i) {
            const std::string v = std::to_string(i);
            if (g_st->begin(p) != SCO_OK || Put(p, "counter", v) != SCO_OK || Put(p, "mirror", v) != SCO_OK ||
                g_st->commit(p) != SCO_OK) return 7;
            if (i == 1) { std::printf("READY\n"); std::fflush(stdout); }
        }
    }
    std::printf("READY\n");
    std::fflush(stdout);
    for (;;) std::this_thread::sleep_for(std::chrono::hours(1));   // until killed
}

// Spawns `<this exe> --child <root> <scenario>`, waits for its READY line (CRLF on Windows: the
// child's stdout is in text mode), kills it hard. False
// if it couldn't be started or exited without READY.
static bool RunAndKill(const char* scenario) {
#ifdef _WIN32
    SECURITY_ATTRIBUTES sa{ sizeof(sa), nullptr, TRUE };
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 0)) return false;
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    wchar_t exe[32768];
    const DWORD exeLen = GetModuleFileNameW(nullptr, exe, 32768);
    if (exeLen == 0 || exeLen >= 32768) return false;
    std::wstring cmd = L"\"" + std::wstring(exe, exeLen) + L"\" --child \"" + g_root.wstring() + L"\" " +
                       fs::path(scenario).wstring();
    STARTUPINFOW si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = writeEnd;
    si.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
    CloseHandle(writeEnd);
    if (!ok) { CloseHandle(readEnd); return false; }
    std::string got;
    char ch;
    DWORD n = 0;
    while (got.find("READY") == std::string::npos && ReadFile(readEnd, &ch, 1, &n, nullptr) && n == 1) got += ch;
    const bool ready = got.find("READY") != std::string::npos;
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    CloseHandle(readEnd);
    return ready;
#else
    int fds[2];
    if (pipe(fds) != 0) return false;
    const pid_t pid = fork();
    if (pid < 0) return false;
    if (pid == 0) {
        dup2(fds[1], 1);
        close(fds[0]);
        close(fds[1]);
        const std::string root = g_root.string();
        execl(g_exe.c_str(), g_exe.c_str(), "--child", root.c_str(), scenario, static_cast<char*>(nullptr));
        _exit(127);
    }
    close(fds[1]);
    std::string got;
    char ch;
    while (got.find("READY") == std::string::npos && read(fds[0], &ch, 1) == 1) got += ch;
    const bool ready = got.find("READY") != std::string::npos;
    kill(pid, SIGKILL);
    int status = 0;
    waitpid(pid, &status, 0);
    close(fds[0]);
    return ready;
#endif
}

static void TestCrash() {
    CHECK(RunAndKill("open"));
    sco_plugin* p = sco::host::NewPlugin("crash1");
    CHECK(Get(p, "state") == "committed");
    char key[64];
    uint32_t size = sizeof(key);
    CHECK(g_st->next_key(p, "junk.", nullptr, key, &size) == SCO_NOT_FOUND);
    CHECK(QueryInt(p, "SELECT count(*) FROM sqlite_schema WHERE name = 'half'") == 0);
    CHECK(QueryText(p, "PRAGMA integrity_check") == "ok");
    CHECK(Put(p, "state", "after") == SCO_OK && Get(p, "state") == "after");   // writable again
    CHECK(sco::Release(p) == Result::Ok);

    CHECK(RunAndKill("loop"));
    sco_plugin* q = sco::host::NewPlugin("crash2");
    const std::string counter = Get(q, "counter"), mirror = Get(q, "mirror");
    CHECK(counter == mirror && std::atoll(counter.c_str()) >= 1);   // never half a transaction
    CHECK(QueryText(q, "PRAGMA integrity_check") == "ok");
    std::printf("crash: killed at counter %s, mirror %s\n", counter.c_str(), mirror.c_str());
    CHECK(sco::Release(q) == Result::Ok);
}

int main(int argc, char** argv) {
    if (argc == 4 && std::strcmp(argv[1], "--child") == 0) {
        g_exe = argv[0];
        return ChildMain(argv[2], argv[3]);
    }
    if (argc < 2) {
        std::printf("usage: test_storage <out dir>\n");
        return 2;
    }
    g_exe = argv[0];
    g_root = fs::path(argv[1]) / "storage_root";
    std::error_code ec;
    fs::remove_all(g_root, ec);
    fs::create_directories(g_root, ec);
    sco::SetGameThread();
    g_api = sco::host::BuildApi({ "test_storage" });

    TestHostServices();
    TestService();
    sco_plugin* a = sco::host::NewPlugin("alpha");
    sco_plugin* b = sco::host::NewPlugin("beta");
    TestKeyValue(a);
    TestTransactions(a);
    TestSql(a);
    TestIsolation(a, b);
    CHECK(sco::Release(a) == Result::Ok && sco::Release(b) == Result::Ok);
    TestCrash();
    TestQuota();
    TestUnload();
    TestConcurrency();
    sco::storage::Stop();
    std::printf("sco-core storage tests: %d passed, %d failed\n", g_pass.load(), g_fail.load());
    return g_fail.load() ? 1 : 0;
}
