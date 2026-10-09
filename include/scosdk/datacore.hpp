// scosdk/datacore.hpp: the host service "sco.datacore" (sco_datacore.h) for C++ plugins.
// Header-only, over sco_api.h and sco_datacore.h.
//
//   sco::sdk::DataCore dc;
//   if (dc.Open(*this) == SCO_OK) {                          // SCO_NOT_FOUND: the product doesn't publish it
//       sco::sdk::DataCorePatch p = dc.Begin();
//       const char* eos = "EntityClassDefinition.QDRV_RSI_S01_Eos_SCItem";
//       p.Set(eos, "Components[SCItemQuantumDriveParams].params.spoolUpTime", 3.5);
//       sco::sdk::DataCoreInstance fast = p.AddInstance("SCItemQuantumDriveParams", eos, "Components[SCItemQuantumDriveParams]");
//       p.Set(fast.Ref(), "params.driveSpeed", 2.5e8);         // "@<id>": a field of the added instance
//       p.SetPointer("EntityClassDefinition.QDRV_WETK_S01_Beacon_SCItem", "Components[SCItemQuantumDriveParams]", fast);
//       p.Commit();                                            // after the load: saved, applies at the next launch
//   }
//   Subscribe("datacore.applied", ...);                        // then p.Reports() (keep the patch) for the results
//
// Every call answers sco_result and is noexcept (out of memory is SCO_TOO_MANY). A DataCorePatch
// is move-only and discards itself on destruction unless committed. The service is host-owned:
// the table stays valid while the plugin is loaded. Any thread (see sco_datacore.h). Reference:
// docs/datacore.md, docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_DATACORE_HPP
#define SCOSDK_DATACORE_HPP

#include "plugin.hpp"
#include "service.hpp"
#include "sco_datacore.h"

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace sco::sdk {

// Value forms that aren't plain C++ types.
struct DataCoreGuid { std::string_view text; };     // "xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx"
struct DataCoreEnum { std::string_view option; };   // an enum field's option by name

// An instance a patch added. Usable as a value (pointer fields, arrays of structs) and, through
// Ref(), as the record argument naming the instance itself.
class DataCoreInstance {
public:
    DataCoreInstance() noexcept = default;
    DataCoreInstance(uint64_t id, sco_result r) noexcept : id_(id), r_(r) {}
    uint64_t Id() const noexcept { return id_; }
    sco_result Result() const noexcept { return r_; }
    explicit operator bool() const noexcept { return id_ != 0; }
    // "@<id>": pass as the record of Set / Append to address the instance's own fields.
    std::string Ref() const noexcept {
        try {
            return "@" + std::to_string(id_);
        } catch (...) {
            return {};
        }
    }

private:
    uint64_t   id_ = 0;
    sco_result r_ = SCO_NOT_FOUND;
};

class DataCorePatch {
public:
    DataCorePatch() noexcept = default;
    DataCorePatch(const sco_datacore_v1* t, uint64_t id, sco_result r) noexcept : t_(t), id_(id), r_(r) {}
    DataCorePatch(DataCorePatch&& o) noexcept { *this = std::move(o); }
    DataCorePatch& operator=(DataCorePatch&& o) noexcept {
        if (this != &o) {
            Discard();
            t_ = o.t_; id_ = o.id_; r_ = o.r_; committed_ = o.committed_;
            o.t_ = nullptr; o.id_ = 0;
        }
        return *this;
    }
    DataCorePatch(const DataCorePatch&) = delete;
    DataCorePatch& operator=(const DataCorePatch&) = delete;
    ~DataCorePatch() {
        if (!committed_) Discard();
    }

    explicit operator bool() const noexcept { return t_ != nullptr; }
    sco_result Result() const noexcept { return r_; }   // begin's answer when empty
    uint64_t Id() const noexcept { return id_; }

    // A value in place. record: a name, "guid:...", or an added instance's Ref().
    template <std::floating_point F>
    sco_result Set(std::string_view record, std::string_view field, F v) noexcept { return Put(true, record, field, Float(v)); }
    template <std::integral I>
        requires(!std::same_as<I, bool>)
    sco_result Set(std::string_view record, std::string_view field, I v) noexcept { return Put(true, record, field, Int(v)); }
    sco_result Set(std::string_view record, std::string_view field, bool v) noexcept { return Put(true, record, field, Bool(v)); }
    sco_result Set(std::string_view record, std::string_view field, std::string_view v) noexcept { return Put(true, record, field, Str(SCO_DC_STRING, v)); }
    sco_result Set(std::string_view record, std::string_view field, const char* v) noexcept { return Set(record, field, std::string_view(v ? v : "")); }
    sco_result Set(std::string_view record, std::string_view field, DataCoreGuid v) noexcept { return Put(true, record, field, Str(SCO_DC_GUID, v.text)); }
    sco_result Set(std::string_view record, std::string_view field, DataCoreEnum v) noexcept { return Put(true, record, field, Str(SCO_DC_ENUM, v.option)); }
    sco_result Set(std::string_view record, std::string_view field, const DataCoreInstance& v) noexcept { return Put(true, record, field, Inst(v)); }
    sco_result Set(std::string_view record, std::string_view field, std::nullptr_t) noexcept { return Put(true, record, field, Null()); }

    // One element at the end of an array: the same value forms.
    template <class T>
    sco_result Append(std::string_view record, std::string_view field, const T& v) noexcept {
        appending_ = true;
        const sco_result r = Set(record, field, v);
        appending_ = false;
        return r;
    }

    // A new instance of type, copied from cloneRecord's cloneField ("" : its root), or zero-filled
    // when cloneRecord is empty.
    DataCoreInstance AddInstance(std::string_view type, std::string_view cloneRecord = {}, std::string_view cloneField = {}) noexcept {
        if (!t_) return { 0, r_ };
        try {
            const std::string ty(type), cr(cloneRecord), cf(cloneField);
            uint64_t id = 0;
            const sco_result r = t_->add_instance(id_, ty.c_str(), cr.empty() ? nullptr : cr.c_str(), cf.empty() ? nullptr : cf.c_str(), &id);
            return { r == SCO_OK ? id : 0, r };
        } catch (...) {
            return { 0, SCO_TOO_MANY };
        }
    }
    sco_result SetPointer(std::string_view record, std::string_view field, const DataCoreInstance& target) noexcept {
        if (!t_) return r_;
        try {
            const std::string rec(record), f(field);
            return t_->set_pointer(id_, rec.c_str(), f.c_str(), target.Id());
        } catch (...) {
            return SCO_TOO_MANY;
        }
    }
    // SCO_UNAVAILABLE in sco.datacore 1.0 (AddRecord comes with the patcher's plan PR 4).
    sco_result AddRecord(std::string_view type, std::string_view name, std::string_view guid, std::string_view cloneRecord,
                         std::string_view filePath = {}) noexcept {
        if (!t_) return r_;
        try {
            const std::string a(type), b(name), c(guid), d(cloneRecord), e(filePath);
            uint64_t out = 0;
            return t_->add_record(id_, a.c_str(), b.c_str(), c.c_str(), d.c_str(), e.empty() ? nullptr : e.c_str(), &out);
        } catch (...) {
            return SCO_TOO_MANY;
        }
    }

    // Queues the patch for the load, or after it saves it for the next launch (sco_datacore.h).
    sco_result Commit() noexcept {
        if (!t_) return r_;
        const sco_result r = t_->commit(id_);
        if (r == SCO_OK) committed_ = true;
        return r;
    }
    // Drops the patch (see sco_datacore.h). The object becomes empty.
    sco_result Discard() noexcept {
        if (!t_) return SCO_NOT_FOUND;
        const sco_result r = t_->discard(id_);
        t_ = nullptr;
        id_ = 0;
        return r;
    }
    // One entry per operation, then one for the patch (op_index SCO_DC_OP_PATCH). Empty on error.
    std::vector<sco_dc_report> Reports() const noexcept {
        std::vector<sco_dc_report> out;
        if (!t_) return out;
        try {
            for (uint32_t i = 0; i <= SCO_DC_MAX_OPS; ++i) {
                sco_dc_report rep{};
                rep.size = sizeof(rep);
                if (t_->report(id_, i, &rep) != SCO_OK) break;
                out.push_back(rep);
            }
        } catch (...) {
            out.clear();
        }
        return out;
    }

private:
    static sco_dc_value Base(sco_dc_type type) noexcept {
        sco_dc_value v{};
        v.size = sizeof(v);
        v.type = type;
        return v;
    }
    template <class F>
    static sco_dc_value Float(F f) noexcept { sco_dc_value v = Base(SCO_DC_FLOAT); v.f = static_cast<double>(f); return v; }
    template <class I>
    static sco_dc_value Int(I i) noexcept {
        if constexpr (std::is_unsigned_v<I>) { sco_dc_value v = Base(SCO_DC_UINT); v.u = static_cast<uint64_t>(i); return v; }
        else { sco_dc_value v = Base(SCO_DC_INT); v.i = static_cast<int64_t>(i); return v; }
    }
    static sco_dc_value Bool(bool b) noexcept { sco_dc_value v = Base(SCO_DC_BOOL); v.i = b ? 1 : 0; return v; }
    static sco_dc_value Null() noexcept { return Base(SCO_DC_NULL); }
    static sco_dc_value Inst(const DataCoreInstance& i) noexcept { sco_dc_value v = Base(SCO_DC_INSTANCE); v.u = i.Id(); return v; }
    sco_dc_value Str(sco_dc_type type, std::string_view s) noexcept {
        sco_dc_value v = Base(type);
        try {
            text_.assign(s);
            v.s = text_.c_str();
        } catch (...) {
            v.size = 0;   // refused below as out of memory
        }
        return v;
    }
    sco_result Put(bool, std::string_view record, std::string_view field, const sco_dc_value& v) noexcept {
        if (!t_) return r_;
        if (v.size == 0) return SCO_TOO_MANY;
        try {
            const std::string rec(record), f(field);
            return appending_ ? t_->append(id_, rec.c_str(), f.c_str(), &v) : t_->set(id_, rec.c_str(), f.c_str(), &v);
        } catch (...) {
            return SCO_TOO_MANY;
        }
    }

    const sco_datacore_v1* t_ = nullptr;
    uint64_t    id_ = 0;
    sco_result  r_ = SCO_NOT_FOUND;
    bool        committed_ = false;
    bool        appending_ = false;
    std::string text_;
};

// The service for one plugin. Open once (OnLoad); keep it for the plugin's life.
class DataCore {
public:
    // SCO_OK, SCO_NOT_FOUND (the product doesn't publish sco.datacore), SCO_UNAVAILABLE (a 1.0 host).
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        self_ = self;
        return ref_.Query(api, SCO_DATACORE_NAME, SCO_DATACORE_VERSION_1_0);
    }
    explicit operator bool() const noexcept { return static_cast<bool>(ref_); }
    // SCO_DC_OPEN or SCO_DC_LOADED; SCO_DC_LOADED when not open.
    uint32_t State() const noexcept { return ref_ ? ref_->state() : SCO_DC_LOADED; }
    // A new patch; empty (Result() says why) on failure. flags: 0 or SCO_DC_NON_ATOMIC.
    DataCorePatch Begin(uint32_t flags = 0) const noexcept {
        if (!ref_) return { nullptr, 0, SCO_UNAVAILABLE };
        uint64_t id = 0;
        const sco_result r = ref_->begin(self_, flags, &id);
        if (r != SCO_OK) return { nullptr, 0, r };
        return { ref_.Get(), id, SCO_OK };
    }

private:
    ServiceRef<sco_datacore_v1> ref_;
    sco_plugin* self_ = nullptr;
};

}  // namespace sco::sdk

#endif  // SCOSDK_DATACORE_HPP
