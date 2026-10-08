#include "sco/signatures.h"
#include "sco/log.h"
#include <cstring>

namespace sco {

namespace {

constexpr size_t kMaxRows = 512;

struct Row { const SigDef* def; SigResult res; uint8_t visiting; };

Row    g_rows[kMaxRows];
size_t g_count = 0;

Row* Find(const char* id) {
    for (size_t i = 0; i < g_count; ++i)
        if (strcmp(g_rows[i].def->id, id) == 0) return &g_rows[i];
    return nullptr;
}

const Image* g_img = nullptr;

void Resolve(Row& r) {
    if (r.res.state != SigState::NotRun) return;
    if (r.visiting) { r.res = SigFail("circular dependency"); return; }
    r.visiting = 1;
    for (const char* need : r.def->needs) {
        if (!need) break;
        Row* dep = Find(need);
        if (!dep) { r.res = SigFail("needs an unknown signature"); r.visiting = 0; return; }
        Resolve(*dep);
        if (dep->res.state != SigState::Ok) {
            r.res = { SigState::Blocked, nullptr, 0, dep->def->id };
            r.visiting = 0;
            return;
        }
    }
    if (r.def->resolve) {
        r.res = r.def->resolve(*g_img);
    } else if (r.def->pattern) {
        r.res = SigPattern(g_img->text, r.def->pattern);
        if (r.res.state == SigState::Ok && r.def->ripDisp > 0)
            r.res.at = RipTarget(r.res.at, static_cast<size_t>(r.def->ripDisp), static_cast<size_t>(r.def->ripSize));
    } else {
        r.res = SigFail("row has neither a pattern nor a resolver");
    }
    if (r.res.state == SigState::Ok && !r.res.at) r.res = SigFail("resolver returned no address");
    r.visiting = 0;
}

}  // namespace

const char* SigStateName(SigState s) {
    switch (s) {
    case SigState::NotRun:    return "NOT RUN";
    case SigState::Ok:        return "OK";
    case SigState::Missing:   return "MISSING";
    case SigState::Ambiguous: return "AMBIG";
    case SigState::Failed:    return "FAILED";
    case SigState::Blocked:   return "BLOCKED";
    }
    return "?";
}

SigResult SigPattern(const Section& text, const char* pattern) {
    int n = 0;
    uint8_t* at = FindUniquePattern(text, pattern, n);
    if (n == 0) return { SigState::Missing, nullptr, 0, nullptr };
    if (n > 1)  return { SigState::Ambiguous, nullptr, n, nullptr };
    return { SigState::Ok, at, 1, nullptr };
}

bool RegisterSignatures(const SigDef* rows, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        if (g_count == kMaxRows) { Log("[core] signature table full at %s", rows[i].id); return false; }
        if (Find(rows[i].id))    { Log("[core] duplicate signature id %s", rows[i].id); return false; }
        g_rows[g_count++] = { &rows[i], {}, 0 };
    }
    return true;
}

void ResolveAll(const Image& img) {
    g_img = &img;
    for (size_t i = 0; i < g_count; ++i) { g_rows[i].res = {}; g_rows[i].visiting = 0; }
    for (size_t i = 0; i < g_count; ++i) Resolve(g_rows[i]);
    g_img = nullptr;
}

const SigResult* SigLookup(const char* id) {
    const Row* r = Find(id);
    return r ? &r->res : nullptr;
}

uint8_t* Sig(const char* id) {
    const SigResult* r = SigLookup(id);
    return r && r->state == SigState::Ok ? r->at : nullptr;
}

bool SigReady(const char* id) { return Sig(id) != nullptr; }

size_t           SignatureCount()          { return g_count; }
const SigDef*    SignatureDef(size_t i)    { return i < g_count ? g_rows[i].def : nullptr; }
const SigResult& SignatureResult(size_t i) { static const SigResult none{}; return i < g_count ? g_rows[i].res : none; }

void LogSignatureReport(bool verbose) {
    size_t ok = 0;
    for (size_t i = 0; i < g_count; ++i) ok += g_rows[i].res.state == SigState::Ok;
    Log("[core] signatures: %zu/%zu OK", ok, g_count);
    for (size_t i = 0; i < g_count; ++i) {
        const Row& r = g_rows[i];
        switch (r.res.state) {
        case SigState::Ok:
            if (verbose) Log("[core] OK       %s", r.def->id);
            break;
        case SigState::Missing:   Log("[core] MISSING  %s (pattern: 0 matches)", r.def->id); break;
        case SigState::Ambiguous: Log("[core] AMBIG    %s (pattern: %d matches)", r.def->id, r.res.matches); break;
        case SigState::Blocked:   Log("[core] BLOCKED  %s (needs %s)", r.def->id, r.res.why); break;
        case SigState::Failed:    Log("[core] FAILED   %s (%s)", r.def->id, r.res.why ? r.res.why : "no reason"); break;
        case SigState::NotRun:    Log("[core] NOT RUN  %s", r.def->id); break;
        }
    }
}

}  // namespace sco
