// Services: function tables published by one owner and found by name (sco/runtime.h).
#include "sco/runtime.h"
#include "internal.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sco {

namespace {

struct Service {
    const void* owner;
    std::string name;
    uint32_t    version;
    const void* table;
};

std::mutex           g_lock;
std::vector<Service> g_services;

bool ValidServiceName(const char* name) {
    if (!name) return false;
    const size_t n = strnlen(name, kMaxServiceNameLen + 1);
    if (n == 0 || n > kMaxServiceNameLen || name[0] == '.' || name[n - 1] == '.') return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (c == '.') { if (name[i + 1] == '.') return false; continue; }
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) return false;
    }
    return true;
}

}  // namespace

Result ProvideService(const void* owner, const char* prefix, const char* name, uint32_t version,
                      const void* table) {
    if (!owner || !table || !ValidServiceName(name)) return Result::BadArg;
    if (prefix) {
        const size_t p = strlen(prefix);
        if (p == 0 || strncmp(name, prefix, p) != 0 || (name[p] != '\0' && name[p] != '.')) return Result::BadArg;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    if (detail::Released(owner)) return Result::BadArg;
    for (const Service& s : g_services)
        if (s.name == name) return Result::BadArg;
    try {
        g_services.push_back({ owner, name, version, table });
    } catch (...) {
        return Result::TooMany;
    }
    return Result::Ok;
}

Result QueryService(const char* name, uint32_t minVersion, const void** out) {
    if (out) *out = nullptr;
    if (!name || !out) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_lock);
    for (const Service& s : g_services) {
        if (s.name != name) continue;
        if ((s.version >> 16) != (minVersion >> 16) || s.version < minVersion) return Result::Unavailable;
        *out = s.table;
        return Result::Ok;
    }
    return Result::NotFound;
}

long detail::ReleaseServices(const void* owner) {
    std::lock_guard<std::mutex> hold(g_lock);
    long n = 0;
    for (size_t i = g_services.size(); i-- > 0;)
        if (g_services[i].owner == owner) { g_services.erase(g_services.begin() + static_cast<std::ptrdiff_t>(i)); ++n; }
    return n;
}

}  // namespace sco
