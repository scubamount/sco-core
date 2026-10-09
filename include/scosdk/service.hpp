// scosdk/service.hpp: services (sco_api 1.1) for C++ plugins. Header-only, over sco_api.h.
//
// A service table is a C struct of function pointers that starts with uint32_t size (so it can
// grow at the end); the static_asserts below enforce the start.
//
//   struct greeter_v1 { uint32_t size; int (*greet)(const char* who, char* out, uint32_t n); };
//   static const greeter_v1 kGreeter = { sizeof(greeter_v1), Greet };
//   sco::sdk::Provide(*this, "hello.greeter", sco::sdk::ServiceVersion(1, 0), &kGreeter);
//
//   sco::sdk::ServiceRef<greeter_v1> g;                      // in another plugin
//   if (g.Query(*this, "hello.greeter", sco::sdk::ServiceVersion(1, 0)) == SCO_OK) g->greet(...);
//
// A provider's table goes away when it unloads: query when you need it, don't keep a ServiceRef
// across ticks. Reference: docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_SERVICE_HPP
#define SCOSDK_SERVICE_HPP

#include "plugin.hpp"

namespace sco::sdk {

namespace detail {

template <class T>
constexpr void CheckServiceTable() noexcept {
    static_assert(std::is_standard_layout_v<T>, "a service table is a standard-layout struct");
    static_assert(std::is_same_v<decltype(T::size), uint32_t>, "a service table starts with uint32_t size");
    static_assert(offsetof(T, size) == 0, "a service table starts with uint32_t size");
}

}  // namespace detail

// Publishes table under name ("<plugin id>" or "<plugin id>.<name>") with version
// ServiceVersion(major, minor), until Release or unload. The table must stay valid until then
// (use a static). SCO_UNAVAILABLE on a 1.0 host; else the host's answer (SCO_BAD_ARG: a bad or
// taken name, or a name outside the plugin's id). Any thread.
template <class T>
sco_result Provide(Plugin& plugin, const char* name, uint32_t version, const T* table) noexcept {
    detail::CheckServiceTable<T>();
    if (!plugin.Api()) return SCO_BAD_ARG;
    if (!plugin.ApiCovers(offsetof(sco_api, provide_service))) return SCO_UNAVAILABLE;
    return plugin.Api()->provide_service(plugin.Self(), name, version, table);
}

// Withdraws one of plugin's services. SCO_NOT_FOUND: it publishes nothing by that name.
inline sco_result Release(Plugin& plugin, const char* name) noexcept {
    if (!plugin.Api()) return SCO_BAD_ARG;
    if (!plugin.ApiCovers(offsetof(sco_api, release_service))) return SCO_UNAVAILABLE;
    return plugin.Api()->release_service(plugin.Self(), name);
}

// A looked-up service table. Empty until Query succeeds; check it (explicit operator bool)
// before operator->. Copyable: it is a pointer.
template <class T>
class ServiceRef {
public:
    ServiceRef() noexcept { detail::CheckServiceTable<T>(); }

    // SCO_OK: found, same major as minVersion and at least as new. SCO_UNAVAILABLE: another
    // major, older, or a 1.0 host (no query_service). SCO_NOT_FOUND: nothing by that name. The
    // ref is empty unless SCO_OK. Any thread.
    sco_result Query(const sco_api* api, const char* name, uint32_t minVersion) noexcept {
        table_ = nullptr;
        if (!api) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* out = nullptr;
        const sco_result r = api->query_service(name, minVersion, &out);
        if (r == SCO_OK) table_ = static_cast<const T*>(out);
        return r;
    }
    sco_result Query(const Plugin& plugin, const char* name, uint32_t minVersion) noexcept {
        return Query(plugin.Api(), name, minVersion);
    }

    const T* Get() const noexcept { return table_; }
    const T* operator->() const noexcept { return table_; }
    explicit operator bool() const noexcept { return table_ != nullptr; }
    void Reset() noexcept { table_ = nullptr; }

private:
    const T* table_ = nullptr;
};

// True when the provider's table (its size field) covers field: the way to call a function a
// later minor of the table added. HasMember(ref, &greeter_v2::farewell).
template <class T, class M>
bool HasMember(const ServiceRef<T>& ref, M T::*field) noexcept {
    const T* t = ref.Get();
    if (!t || !field) return false;
    const uintptr_t base = reinterpret_cast<uintptr_t>(t);
    const uintptr_t at = reinterpret_cast<uintptr_t>(std::addressof(t->*field));
    return static_cast<uint64_t>(t->size) >= static_cast<uint64_t>(at - base) + sizeof(M);
}

}  // namespace sco::sdk

#endif  // SCOSDK_SERVICE_HPP
