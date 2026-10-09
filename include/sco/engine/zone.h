#pragma once
// The zone tree: nested reference frames (system > planet > city > ship > room) and the transforms
// between them. Pure math over data the host feeds it: sco-core never reads the game's zone
// objects. The host (sc-offline) reads them each tick and calls Set / Remove; features and plugins
// then ask for positions in whatever zone they need.
//
//   sco::engine::ZoneTree zones;                       // the host owns one; not a singleton
//   zones.Set(planetId, systemId, "Hurston", planetTransform);
//   sco::engine::Vector3d world;
//   if (zones.LocalToWorld(shipId, seatLocal, &world)) ...
//
// Ids are the host's (the game's zone or entity ids); 0 is never a zone: as a parent it means
// "a root zone", and as a query id it names the world frame itself. Thread-safe: queries run
// concurrently with each other (shared lock), Set / Remove / Clear take the lock exclusively.
#include "sco/engine/types.h"
#include <cstddef>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace sco::engine {

struct Zone {
    uint64_t    id = 0;
    uint64_t    parentId = 0;   // 0: a root zone, its transform is relative to the world
    std::string name;
    engine::Transform local;    // this zone's frame in its parent's frame
};

class ZoneTree {
public:
    // Most zones from a zone up to its root, itself included. A longer chain, or a parent cycle,
    // makes every query through it fail.
    static constexpr int kMaxDepth = 32;

    // Inserts or updates a zone. False (nothing changed) for id 0 or parentId == id. The parent
    // need not exist yet; queries through the zone fail until it does.
    bool Set(uint64_t id, uint64_t parentId, std::string_view name, const engine::Transform& local);
    // Removes one zone. Its children stay, and their queries fail until the id is Set again.
    bool Remove(uint64_t id);
    void Clear();

    bool   Has(uint64_t id) const;
    bool   Find(uint64_t id, Zone* out) const;   // copies the zone
    size_t Size() const;

    // A point in zone `id` to the world frame, and back. False (out untouched) when out is null,
    // the zone or an ancestor is missing, or the chain is longer than kMaxDepth or has a cycle.
    bool LocalToWorld(uint64_t id, const Vector3d& local, Vector3d* out) const;
    bool WorldToLocal(uint64_t id, const Vector3d& world, Vector3d* out) const;
    // A point in zone fromId to zone toId (either may be 0, the world). Goes up only to the
    // lowest common ancestor, never through world coordinates, so two zones on one planet keep
    // full precision however far that planet is from the origin. Same failures as above.
    bool Transform(uint64_t fromId, uint64_t toId, const Vector3d& pos, Vector3d* out) const;

private:
    // Zone id, its parent, ... up to the root, under a held lock; n = 0 for id 0.
    bool ChainLocked(uint64_t id, const Zone* chain[kMaxDepth], int* n) const;

    mutable std::shared_mutex mu_;
    std::unordered_map<uint64_t, Zone> zones_;
};

}  // namespace sco::engine
