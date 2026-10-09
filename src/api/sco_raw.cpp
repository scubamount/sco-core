// Raw handlers: byte-in, byte-out calls registered by one owner and called by name on the game
// thread (sco/runtime.h).
#include "sco/runtime.h"
#include "internal.h"
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace sco {

namespace {

struct Handler {
    const void* owner;
    std::string name;
    std::string capability;   // empty: none
    RawFn       fn;
    void*       ctx;
};

std::mutex           g_lock;
std::vector<Handler> g_handlers;

// "<x>.<y>": segments of [a-z0-9_] joined by '.', at least two, none empty (the command name
// rule); also the capability rule with one segment allowed.
bool Segments(const char* n, size_t max, bool needDot) {
    if (!n || !*n || strnlen(n, max + 1) > max) return false;
    bool dot = false;
    char prev = '.';
    for (const char* p = n; *p; ++p) {
        const char c = *p;
        if (c == '.') {
            if (prev == '.') return false;
            dot = true;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
        prev = c;
    }
    return prev != '.' && (dot || !needDot);
}

struct RawCall { RawFn fn; const void* in; uint32_t inSize; void* out; uint32_t* outSize; void* ctx; Result r; };
void RawThunk(void* p) {
    RawCall* k = static_cast<RawCall*>(p);
    k->r = k->fn(k->in, k->inSize, k->out, k->outSize, k->ctx);
}

}  // namespace

Result RegisterRaw(const void* owner, const char* prefix, const char* name, const char* capability,
                   RawFn fn, void* ctx) {
    if (!owner || !fn || !Segments(name, kMaxNameLen, true)) return Result::BadArg;
    if (capability && !Segments(capability, kMaxCapabilityLen, false)) return Result::BadArg;
    if (prefix) {
        const size_t p = strlen(prefix);
        if (p == 0 || strncmp(name, prefix, p) != 0 || name[p] != '.') return Result::BadArg;
        for (const char* r : kReservedPrefixes)
            if (strcmp(prefix, r) == 0) return Result::BadArg;
    }
    std::lock_guard<std::mutex> hold(g_lock);
    if (detail::Released(owner)) return Result::BadArg;
    for (const Handler& h : g_handlers)
        if (h.name == name) return Result::BadArg;
    if (g_handlers.size() >= kMaxRawHandlers) return Result::TooMany;
    try {
        g_handlers.push_back({ owner, name, capability ? capability : "", fn, ctx });
    } catch (...) {
        return Result::TooMany;
    }
    return Result::Ok;
}

Result InvokeRaw(const void* caller, const char* name, const void* in, uint32_t inSize, void* out,
                 uint32_t* outSize) {
    if (outSize && !out && *outSize) return Result::BadArg;
    if (!name || (!in && inSize) || (out && !outSize) || detail::Released(caller)) {
        if (outSize) *outSize = 0;
        return Result::BadArg;
    }
    if (!OnGameThread()) { if (outSize) *outSize = 0; return Result::WrongThread; }
    const void* owner = nullptr;
    RawFn fn = nullptr;
    void* ctx = nullptr;
    char capability[kMaxCapabilityLen + 1] = "";
    {
        std::lock_guard<std::mutex> hold(g_lock);
        for (const Handler& h : g_handlers)
            if (h.name == name) {
                owner = h.owner;
                fn = h.fn;
                ctx = h.ctx;
                std::memcpy(capability, h.capability.c_str(), h.capability.size() + 1);
                break;
            }
    }
    if (!fn) { if (outSize) *outSize = 0; return Result::NotFound; }
    if (capability[0] && !detail::CapabilityAvailable(capability)) { if (outSize) *outSize = 0; return Result::Unavailable; }
    uint32_t none = 0;
    uint32_t* size = outSize ? outSize : &none;
    const uint32_t cap = *size;
    RawCall call{ fn, in, inSize, out, size, ctx, Result::Ok };
    if (!detail::Callout(owner, name, RawThunk, &call)) { *size = 0; return Result::Crashed; }
    if (call.r == Result::Ok && *size > cap) return Result::TooMany;   // a handler that overstated what it wrote
    return call.r;
}

long detail::ReleaseRaw(const void* owner) {
    std::lock_guard<std::mutex> hold(g_lock);
    long n = 0;
    for (size_t i = g_handlers.size(); i-- > 0;)
        if (g_handlers[i].owner == owner) { g_handlers.erase(g_handlers.begin() + static_cast<std::ptrdiff_t>(i)); ++n; }
    return n;
}

}  // namespace sco
