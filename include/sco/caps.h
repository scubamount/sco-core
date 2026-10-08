#pragma once
// Capabilities: named yes/no answers to "does this feature work on this game build?".
// Host features set one where they log "[+] ... ready" today; a signature-backed feature can let
// its rows decide (SetFromSignatures). sco_api.has() and the command registry's capability check
// answer from here (sco::host::BuildApi wires the check).
//
// Thread safety: every function is safe from any thread (one mutex). Set at startup or when a
// feature's state changes; Has is cheap enough to call per command.
#include "sco/runtime.h"
#include <cstddef>

namespace sco::caps {

constexpr size_t kMaxCaps = 256;          // distinct names for the life of the process
constexpr size_t kMaxNameLen = 63;        // NUL excluded; longer is BadArg
constexpr size_t kMaxReasonLen = 127;     // NUL excluded; longer is truncated

// Names are one or more segments of lowercase letters, digits and '_', joined by '.':
// "teleport", "spawn.ship". No empty segment.
//
// Sets or updates a capability. reason says why it isn't ready ("signature teleport.to_camera
// MISSING"); it is copied, truncated to kMaxReasonLen, and nullptr means "". Names are never
// removed: a feature that stops working sets ready = false.
// BadArg for a bad name; TooMany when kMaxCaps names exist and `name` isn't one of them.
Result Set(const char* name, bool ready, const char* reason = nullptr);

// Sets `name` ready when every listed signature row is OK. Otherwise not ready, with reason
// "needs <id> (<STATE>)" for the first row that isn't OK, or "unknown signature <id>".
// Reads the registry, so call it after sco::ResolveAll(). BadArg for a null list, n == 0, a
// null id or a bad name; TooMany as Set.
Result SetFromSignatures(const char* name, const char* const* sigIds, size_t n);

// True when `name` was Set ready. Unknown names and nullptr are false.
bool Has(const char* name);

struct Entry {
    char name[kMaxNameLen + 1];
    bool ready;
    char reason[kMaxReasonLen + 1];
};

// Copies up to max entries, in the order names were first Set; returns the number of names.
// out may be nullptr when max is 0.
size_t List(Entry* out, size_t max);

}  // namespace sco::caps
