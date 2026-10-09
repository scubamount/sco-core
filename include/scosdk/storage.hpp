// scosdk/storage.hpp: the host service "sco.storage" (sco_storage.h) for C++ plugins.
// Header-only, over sco_api.h and sco_storage.h.
//
//   sco::sdk::Storage store;
//   if (store.Open(*this) == SCO_OK) {
//       store.Put("spots.home", spot);                       // a trivially copyable struct
//       std::string name;
//       store.Get("player.name", name);
//       sco::sdk::StorageTransaction tx(store);              // rolls back unless committed
//       store.Exec("CREATE TABLE IF NOT EXISTS visits(place TEXT, at INTEGER)");
//       store.Exec("INSERT INTO visits VALUES (?, ?)", { sco::sdk::SqlText("Lorville"), sco::sdk::SqlInt(1) });
//       tx.Commit();
//       sco::sdk::StorageCursor c = store.Query("SELECT place FROM visits");
//       while (c.Next() == SCO_OK) { std::string place; c.Text(0, place); }
//   }
//
// Every call answers sco_result and is noexcept (out of memory is SCO_TOO_MANY). The service is
// host-owned, so the table stays valid while the plugin is loaded and a Storage may be kept
// for the plugin's life. Calls may come from any thread (see sco_storage.h). Reference:
// docs/storage.md, docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_STORAGE_HPP
#define SCOSDK_STORAGE_HPP

#include "plugin.hpp"
#include "sco_storage.h"

#include <cstring>
#include <span>
#include <string_view>

namespace sco::sdk {

// Parameter builders for Exec and Query. Text and blob values point at the caller's bytes,
// which the host copies during the call.
inline sco_sql_value SqlNull() noexcept { return sco_sql_value{}; }
inline sco_sql_value SqlInt(int64_t i) noexcept {
    sco_sql_value v{};
    v.type = SCO_SQL_INT;
    v.v.i = i;
    return v;
}
inline sco_sql_value SqlFloat(double f) noexcept {
    sco_sql_value v{};
    v.type = SCO_SQL_FLOAT;
    v.v.f = f;
    return v;
}
inline sco_sql_value SqlText(std::string_view s) noexcept {
    sco_sql_value v{};
    v.type = SCO_SQL_TEXT;
    v.size = static_cast<uint32_t>(s.size());
    v.v.p = s.data();
    return v;
}
inline sco_sql_value SqlBlob(std::span<const std::byte> b) noexcept {
    sco_sql_value v{};
    v.type = SCO_SQL_BLOB;
    v.size = static_cast<uint32_t>(b.size());
    v.v.p = b.data();
    return v;
}

class Storage;

// A query's rows. Move-only; closes the cursor on destruction. Empty (Result() says why) when
// the query failed.
class StorageCursor {
public:
    StorageCursor() noexcept = default;
    StorageCursor(StorageCursor&& o) noexcept { *this = std::move(o); }
    StorageCursor& operator=(StorageCursor&& o) noexcept {
        if (this != &o) {
            Close();
            t_ = o.t_; self_ = o.self_; id_ = o.id_; r_ = o.r_;
            o.t_ = nullptr; o.id_ = 0;
        }
        return *this;
    }
    StorageCursor(const StorageCursor&) = delete;
    StorageCursor& operator=(const StorageCursor&) = delete;
    ~StorageCursor() { Close(); }

    explicit operator bool() const noexcept { return t_ != nullptr; }
    sco_result Result() const noexcept { return r_; }

    // SCO_OK: a row is ready. SCO_NOT_FOUND: no more rows.
    sco_result Next() noexcept { return t_ ? t_->step(self_, id_) : r_; }
    uint32_t Columns() const noexcept {
        uint32_t n = 0;
        if (t_) t_->column_count(self_, id_, &n);
        return n;
    }
    // The column's type (sco_sql_type) and, for INT and FLOAT, its value; SCO_SQL_NULL on error.
    sco_sql_value Value(uint32_t index) const noexcept {
        sco_sql_value v{};
        if (t_) t_->column(self_, id_, index, &v, nullptr, nullptr);
        return v;
    }
    sco_result Int(uint32_t index, int64_t& out) const noexcept {
        sco_sql_value v{};
        const sco_result r = t_ ? t_->column(self_, id_, index, &v, nullptr, nullptr) : r_;
        if (r == SCO_OK && v.type != SCO_SQL_INT) return SCO_BAD_ARG;
        if (r == SCO_OK) out = v.v.i;
        return r;
    }
    sco_result Float(uint32_t index, double& out) const noexcept {
        sco_sql_value v{};
        const sco_result r = t_ ? t_->column(self_, id_, index, &v, nullptr, nullptr) : r_;
        if (r == SCO_OK && v.type != SCO_SQL_FLOAT && v.type != SCO_SQL_INT) return SCO_BAD_ARG;
        if (r == SCO_OK) out = v.type == SCO_SQL_INT ? static_cast<double>(v.v.i) : v.v.f;
        return r;
    }
    // TEXT (or BLOB) bytes; NULL reads as "".
    sco_result Text(uint32_t index, std::string& out) const noexcept { return Bytes(index, out); }
    sco_result Blob(uint32_t index, std::string& out) const noexcept { return Bytes(index, out); }

    void Close() noexcept {
        if (t_) t_->close(self_, id_);
        t_ = nullptr;
        id_ = 0;
    }

private:
    friend class Storage;
    sco_result Bytes(uint32_t index, std::string& out) const noexcept {
        if (!t_) return r_;
        sco_sql_value v{};
        sco_result r = t_->column(self_, id_, index, &v, nullptr, nullptr);
        if (r != SCO_OK) return r;
        if (v.type == SCO_SQL_NULL) { out.clear(); return SCO_OK; }
        if (v.type != SCO_SQL_TEXT && v.type != SCO_SQL_BLOB) return SCO_BAD_ARG;
        try {
            out.resize(static_cast<size_t>(v.size) + 1);   // TEXT comes back with its NUL
        } catch (...) {
            return SCO_TOO_MANY;
        }
        uint32_t size = static_cast<uint32_t>(out.size());
        r = t_->column(self_, id_, index, &v, out.data(), &size);
        if (r != SCO_OK) { out.clear(); return r; }
        out.resize(v.type == SCO_SQL_TEXT && size ? size - 1 : size);
        return SCO_OK;
    }

    const sco_storage_v1* t_ = nullptr;
    sco_plugin*           self_ = nullptr;
    uint64_t              id_ = 0;
    sco_result            r_ = SCO_UNAVAILABLE;
};

class Storage {
public:
    // Finds sco.storage 1.x. SCO_UNAVAILABLE on a 1.0 host or a host without storage (that is
    // SCO_NOT_FOUND from the host; both leave the Storage empty).
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SCO_STORAGE_NAME, SCO_STORAGE_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sco_storage_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sco_storage_v1* Table() const noexcept { return t_; }

    // ---- key-value ----

    sco_result Put(const char* key, std::span<const std::byte> value) noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        if (value.size() > SCO_STORAGE_MAX_VALUE) return SCO_BAD_ARG;
        return t_->put(self_, key, value.data(), static_cast<uint32_t>(value.size()));
    }
    sco_result Put(const char* key, std::string_view value) noexcept { return Put(key, std::as_bytes(std::span(value))); }
    sco_result Put(const char* key, const char* value) noexcept { return Put(key, std::string_view(value ? value : "")); }
    // A trivially copyable value, stored as its bytes.
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_array_v<T>)
    sco_result Put(const char* key, const T& value) noexcept {
        return Put(key, std::span<const std::byte>(std::as_bytes(std::span<const T, 1>(&value, 1))));
    }

    // Reads a value of any size into out (the handshake is done here).
    sco_result Get(const char* key, std::string& out) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        for (int attempt = 0; attempt < 4; ++attempt) {   // the value may change between calls
            uint32_t size = static_cast<uint32_t>(out.capacity() < UINT32_MAX ? out.capacity() : UINT32_MAX);
            try {
                out.resize(size);
            } catch (...) {
                return SCO_TOO_MANY;
            }
            const sco_result r = t_->get(self_, key, size ? out.data() : nullptr, &size);
            if (r == SCO_OK) { out.resize(size); return SCO_OK; }
            if (r != SCO_TOO_MANY) { out.clear(); return r; }
            try {
                out.reserve(size);
            } catch (...) {
                return SCO_TOO_MANY;
            }
        }
        return SCO_FAILED;
    }
    // A trivially copyable value: SCO_BAD_ARG unless the stored size is exactly sizeof(T).
    template <class T>
        requires(std::is_trivially_copyable_v<T> && !std::is_pointer_v<T> && !std::is_same_v<T, std::string>)
    sco_result Get(const char* key, T& out) const noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        T got{};
        uint32_t size = sizeof(T);
        const sco_result r = t_->get(self_, key, &got, &size);
        if (r == SCO_TOO_MANY) return SCO_BAD_ARG;
        if (r != SCO_OK) return r;
        if (size != sizeof(T)) return SCO_BAD_ARG;
        out = got;
        return SCO_OK;
    }
    sco_result Delete(const char* key) noexcept { return t_ ? t_->del(self_, key) : SCO_UNAVAILABLE; }

    // Every key starting with prefix, in byte order.
    sco_result Keys(const char* prefix, std::vector<std::string>& out) const noexcept {
        out.clear();
        if (!t_) return SCO_UNAVAILABLE;
        try {
            char buf[SCO_STORAGE_MAX_KEY + 1];
            std::string last;
            for (;;) {
                uint32_t size = sizeof(buf);
                const sco_result r = t_->next_key(self_, prefix, out.empty() ? nullptr : last.c_str(), buf, &size);
                if (r == SCO_NOT_FOUND) return SCO_OK;
                if (r != SCO_OK) return r;
                last.assign(buf, size - 1);
                out.push_back(last);
            }
        } catch (...) {
            return SCO_TOO_MANY;
        }
    }

    // ---- transactions ----

    sco_result Begin() noexcept { return t_ ? t_->begin(self_) : SCO_UNAVAILABLE; }
    sco_result Commit() noexcept { return t_ ? t_->commit(self_) : SCO_UNAVAILABLE; }
    sco_result Rollback() noexcept { return t_ ? t_->rollback(self_) : SCO_UNAVAILABLE; }

    // ---- SQL ----

    sco_result Exec(const char* sql, std::initializer_list<sco_sql_value> params = {}, int64_t* changes = nullptr) noexcept {
        if (!t_) return SCO_UNAVAILABLE;
        return t_->exec(self_, sql, params.size() ? params.begin() : nullptr, static_cast<uint32_t>(params.size()), changes);
    }
    StorageCursor Query(const char* sql, std::initializer_list<sco_sql_value> params = {}) noexcept {
        StorageCursor c;
        c.self_ = self_;
        if (!t_) return c;
        uint64_t id = 0;
        c.r_ = t_->query(self_, sql, params.size() ? params.begin() : nullptr, static_cast<uint32_t>(params.size()), &id);
        if (c.r_ == SCO_OK) {
            c.t_ = t_;
            c.id_ = id;
        }
        return c;
    }

    // The message of the plugin's last failed storage call.
    std::string LastError() const noexcept {
        if (!t_) return {};
        try {
            std::string s(256, '\0');
            uint32_t size = static_cast<uint32_t>(s.size());
            sco_result r = t_->last_error(self_, s.data(), &size);
            if (r == SCO_TOO_MANY) {
                s.resize(size);
                r = t_->last_error(self_, s.data(), &size);
            }
            if (r != SCO_OK || size == 0) return {};
            s.resize(size - 1);
            return s;
        } catch (...) {
            return {};
        }
    }

private:
    const sco_storage_v1* t_ = nullptr;
    sco_plugin*           self_ = nullptr;
};

// Begins a transaction on construction and rolls it back on destruction unless Commit() ran.
// Result() is begin's answer.
class StorageTransaction {
public:
    explicit StorageTransaction(Storage& s) noexcept : s_(&s), r_(s.Begin()) {}
    StorageTransaction(const StorageTransaction&) = delete;
    StorageTransaction& operator=(const StorageTransaction&) = delete;
    ~StorageTransaction() {
        if (r_ == SCO_OK && !done_) s_->Rollback();
    }
    sco_result Result() const noexcept { return r_; }
    sco_result Commit() noexcept {
        if (r_ != SCO_OK || done_) return SCO_BAD_ARG;
        const sco_result r = s_->Commit();
        if (r == SCO_OK) done_ = true;
        return r;
    }

private:
    Storage*   s_;
    sco_result r_;
    bool       done_ = false;
};

}  // namespace sco::sdk

#endif  // SCOSDK_STORAGE_HPP
