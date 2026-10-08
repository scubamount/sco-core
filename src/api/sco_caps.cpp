// Capability registry: a fixed table of names, each ready or not with a reason.
#include "sco/caps.h"
#include "sco/signatures.h"
#include <cstdio>
#include <cstring>
#include <mutex>

namespace sco::caps {

namespace {

struct Cap {
    char name[kMaxNameLen + 1];
    bool ready;
    char reason[kMaxReasonLen + 1];
};

std::mutex g_lock;
Cap        g_caps[kMaxCaps];
size_t     g_count = 0;

// Segments of [a-z0-9_] joined by '.', 1..kMaxNameLen characters.
bool ValidName(const char* n) {
    if (!n || !*n || strnlen(n, kMaxNameLen + 1) > kMaxNameLen) return false;
    char prev = '.';
    for (const char* p = n; *p; ++p) {
        const char c = *p;
        if (c == '.') {
            if (prev == '.') return false;
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
            return false;
        }
        prev = c;
    }
    return prev != '.';
}

Cap* Find(const char* name) {
    for (size_t i = 0; i < g_count; ++i)
        if (strcmp(g_caps[i].name, name) == 0) return &g_caps[i];
    return nullptr;
}

}  // namespace

Result Set(const char* name, bool ready, const char* reason) {
    if (!ValidName(name)) return Result::BadArg;
    std::lock_guard<std::mutex> hold(g_lock);
    Cap* c = Find(name);
    if (!c) {
        if (g_count == kMaxCaps) return Result::TooMany;
        c = &g_caps[g_count++];
        snprintf(c->name, sizeof(c->name), "%s", name);
    }
    c->ready = ready;
    snprintf(c->reason, sizeof(c->reason), "%s", reason ? reason : "");
    return Result::Ok;
}

Result SetFromSignatures(const char* name, const char* const* sigIds, size_t n) {
    if (!sigIds || n == 0) return Result::BadArg;
    for (size_t i = 0; i < n; ++i)
        if (!sigIds[i]) return Result::BadArg;
    char why[kMaxReasonLen + 1] = "";
    bool ready = true;
    for (size_t i = 0; i < n && ready; ++i) {
        const SigResult* r = SigLookup(sigIds[i]);
        if (!r) {
            snprintf(why, sizeof(why), "unknown signature %s", sigIds[i]);
            ready = false;
        } else if (r->state != SigState::Ok) {
            snprintf(why, sizeof(why), "needs %s (%s)", sigIds[i], SigStateName(r->state));
            ready = false;
        }
    }
    return Set(name, ready, ready ? nullptr : why);
}

bool Has(const char* name) {
    if (!name) return false;
    std::lock_guard<std::mutex> hold(g_lock);
    const Cap* c = Find(name);
    return c && c->ready;
}

size_t List(Entry* out, size_t max) {
    std::lock_guard<std::mutex> hold(g_lock);
    if (out)
        for (size_t i = 0; i < g_count && i < max; ++i) {
            memcpy(out[i].name, g_caps[i].name, sizeof(out[i].name));
            out[i].ready = g_caps[i].ready;
            memcpy(out[i].reason, g_caps[i].reason, sizeof(out[i].reason));
        }
    return g_count;
}

}  // namespace sco::caps
