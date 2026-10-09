#pragma once
// sco::storage: the host side of the "sco.storage" service (include/sco_storage.h), per-plugin
// persistent storage over the vendored SQLite (third_party/sqlite). C++ and internal: the host
// kit starts it (sco::app::Platform::dataRoot); plugins only see the C table.
//
//   sco::storage::Options o;
//   o.dataRoot = "data";                    // databases: data/storage/<plugin id>.db
//   sco::storage::Start(o);                 // publishes sco.storage 1.0 (a host-owned service)
//   ... plugins query_service("sco.storage", 0x00010000, ...) ...
//   sco::storage::Stop();                   // after every plugin unloaded: withdraws, closes
//
// Each plugin gets its own database file, opened on its first call, keyed by its sco_plugin
// handle and named after its id. The connection is configured by the host before the plugin
// sees it: WAL journal, synchronous FULL (Options::durable) so a call that returned OK
// survives a crash of the game and of the machine, a busy timeout, a page quota, defensive
// mode, no trusted schema, ATTACH limited to 0 databases, and an authorizer that refuses
// ATTACH, DETACH, transaction statements, PRAGMAs outside a read-mostly list and schema changes
// to sco_* names (the key-value table is sco_kv). Every call has a time budget (a progress
// handler interrupts a statement past it). Release(owner) (a runtime release hook) rolls back
// the plugin's open transaction, closes its cursors and its database. Reference:
// docs/storage.md.
#include "sco_storage.h"
#include "sco/runtime.h"
#include <cstdint>
#include <filesystem>

namespace sco::storage {

struct Options {
    std::filesystem::path dataRoot;           // the product's data folder; databases go in <dataRoot>/storage/
    uint64_t quotaBytes = 64ull << 20;        // per plugin database; a write past it is SCO_TOO_MANY
    uint32_t busyTimeoutMs = 1000;            // waiting for a lock another process holds
    uint32_t budgetMs = 1000;                 // per call: a statement still running then is interrupted
    bool     durable = true;                  // synchronous FULL (else NORMAL: a power cut may drop the last commits)
};

// Publishes "sco.storage" 1.0 under the host's id and installs the release hook. Any thread.
// BadArg: dataRoot empty, budgetMs 0, already started, or the name already published. TooMany:
// no release hook slot or out of memory. Nothing changes unless Ok. The storage folder is
// created on the first plugin call.
Result Start(const Options& opts);

// Withdraws "sco.storage", rolls back every open transaction and closes every database and
// cursor; later calls through the table answer SCO_UNAVAILABLE. No-op unless started. Call after
// every plugin has unloaded (sco::app::Stop does). Any thread.
void Stop();

bool Started();

// The service table (what query_service hands out). Valid for the life of the process.
const sco_storage_v1* Table();

// <dataRoot>/storage/<id>.db for the current Start; empty when not started.
std::filesystem::path DatabasePath(const char* id);

}  // namespace sco::storage
