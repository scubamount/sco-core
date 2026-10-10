#pragma once
// Addresses behind game.entities (docs/game-services.md, docs/design/game-world-spikes.md B3 and
// B9): the entity class name, the game's "for each entity" walk (query_radius) and the two hooks
// that see every entity stream in and out (watch). Resolved by sco::ResolveAll(); the rows are in
// src/game/entities_sigs.cpp and docs/game/entities.md lists each one with what it checks.
//
// Rows only find addresses. Calling and hooking stay in src/game/services/entities.cpp.
//
// Header-only and free of Windows types: the watch type filter (a pure function) is here too, so
// the host tests run it.
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace sco::game::entities {

struct Capability {
    const char*        name;    // "entities.class_name"
    const char* const* rows;    // signature ids, all registered by RegisterGameSignatures()
    size_t             count;
};

// Every group, in a fixed order: entities.class_name (class_of), entities.enumerate
// (query_radius), entities.stream_hooks (watch).
const Capability* Capabilities(size_t& count);

// ---- what the code relies on -----------------------------------------------------------------------
//
// "checked" = a row fails when the game's own code stops using that offset at the place named.
constexpr size_t kEntityId        = 0x08;   // checked: entities.class_site (+0xAB): const uint64_t* GetId(entity, tmp)
constexpr size_t kEntityClass     = 0x20;   // checked: entities.class_site (+0x28): IEntityClass* GetClass(entity)
constexpr size_t kClassName       = 0x18;   // checked: entities.class_site (+0x28): const char* GetName(class)
constexpr size_t kEntityHandleMask = 0xFFFFFFFFFFFFull;   // an entity handle's pointer bits

// The third argument of the game's ForEach (a byte; the dump passes a flag it was given). Its
// meaning isn't known: the diagnostic run logs the entity count for both values, and query_radius
// passes this one.
constexpr uint8_t kForEachFlag = 0;

// ---- in-game confirmation -------------------------------------------------------------------------
//
// These stay false until the maintainer's in-game run has confirmed what the spike left pending
// (docs/design/game-world-spikes.md B3 and B9). While false the capability is not set ready and
// the function answers SCO_UNAVAILABLE. Flipping one is a one-line change after that run.
constexpr bool kQueryRadiusConfirmed = false;   // B3: ForEach's thread, flag and completion
constexpr bool kWatchConfirmed       = false;   // B9: the vector layout and the hooks' thread

// ---- the watch type filter ------------------------------------------------------------------------
//
// A type is an entity class name ("AEGS_Avenger_Titan"), matched exactly (case-sensitive), or a
// class prefix ending in one '*' ("AEGS_*"). Refused, with a reason: NULL, empty, a lone "*", more
// than one '*', a '*' that isn't last ("A*B", "**"), longer than kMaxTypeLen, and any byte that
// isn't printable ASCII (0x21 to 0x7E) or is '?'. The host runs the match, so a plugin is called
// only for the entities it asked about.
constexpr size_t kMaxTypeLen = 63;   // NUL excluded

struct TypeFilter {
    char   text[kMaxTypeLen + 1];   // the exact name, or the prefix without its '*'
    size_t len = 0;
    bool   prefix = false;
};

// nullptr when type is accepted (out filled in), else the reason (a static string).
inline const char* ParseType(const char* type, TypeFilter& out) {
    out = TypeFilter{};
    if (!type) return "the type is NULL";
    const size_t n = strnlen(type, kMaxTypeLen + 2);
    if (n == 0) return "the type is empty: name an entity class or a prefix like \"AEGS_*\"";
    if (n > kMaxTypeLen) return "the type is longer than 63 characters";
    for (size_t i = 0; i < n; ++i) {
        const unsigned char c = static_cast<unsigned char>(type[i]);
        if (c == '*') {
            if (i + 1 != n) return "'*' may only end the type (\"PREFIX*\")";
            continue;
        }
        if (c < 0x21 || c > 0x7E || c == '?') return "the type has a character that isn't printable ASCII (or is '?')";
    }
    out.prefix = type[n - 1] == '*';
    out.len = out.prefix ? n - 1 : n;
    if (out.len == 0) return "a lone \"*\" matches every entity: name a class or a prefix like \"AEGS_*\"";
    memcpy(out.text, type, out.len);
    out.text[out.len] = 0;
    return nullptr;
}

// True when class name `cls` (NUL-terminated, may be NULL) matches the filter.
inline bool MatchesType(const TypeFilter& f, const char* cls) {
    if (!cls || f.len == 0) return false;
    if (f.prefix) return strncmp(cls, f.text, f.len) == 0;
    return strcmp(cls, f.text) == 0;
}

}  // namespace sco::game::entities
