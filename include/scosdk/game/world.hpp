// scosdk/game/world.hpp: the game pack's service "game.world" (sc_world.h) for C++ plugins.
// Header-only, over sco_api.h and sc_world.h.
//
//   sco::sdk::game::World world;
//   if (world.Open(*this) == SCO_OK) {
//       double pos[3], rot[4];
//       uint64_t zone = 0;
//       if (world.Camera(pos, rot, zone) == SCO_OK) { ... }     // the world frame; zone: where you are
//       sc_world_hit hit;
//       const double from[3] = { 0, 0, 2 }, down[3] = { 0, 0, -1 };   // a zone's local frame (teleport.spatial)
//       if (world.Raycast(zone, from, down, 100.0, hit) == SCO_OK) { ... hit.pos, hit.distance }
//       else Warn("raycast: %s", world.LastError().c_str());
//   }
//
// Game thread only, like the table. Every call answers sco_result and is noexcept. Both queries are
// read-only, and the service is host-owned, so an open World may be kept for the plugin's life.
// Capabilities: has("game.world.raycast"), has("game.world.camera"). There is no field of view (the
// game doesn't pin one: sc_world.h). Reference: docs/game-services.md, docs/sdk-cpp.md.
// GPL-3.0, like sco-core.
#ifndef SCOSDK_GAME_WORLD_HPP
#define SCOSDK_GAME_WORLD_HPP

#include "../plugin.hpp"
#include "sc_world.h"

#include <cstring>
#include <string>

namespace sco::sdk::game {

class World {
public:
    // Finds game.world 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND when the host has no
    // game pack (or the product didn't start its services). Both leave the World empty.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SC_WORLD_NAME, SC_WORLD_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sc_world_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sc_world_v1* Table() const noexcept { return t_; }

    // A ray from from (metres, in zone's local frame) along dir for at most maxDist metres (longer
    // is clamped to SC_WORLD_RAY_MAX_DISTANCE). hit is zeroed first and filled on SCO_OK;
    // SCO_NOT_FOUND: nothing hit (LastError says which of "no hit", "zone not streamed in", "not spawned").
    sco_result Raycast(uint64_t zone, const double (&from)[3], const double (&dir)[3], double maxDist, sc_world_hit& hit) const noexcept {
        std::memset(&hit, 0, sizeof(hit));
        hit.size = sizeof(hit);
        return t_ ? t_->raycast(zone, from, dir, maxDist, &hit) : SCO_UNAVAILABLE;
    }
    // The camera: pos in the world frame, rot (x, y, z, w) the same frame's rotation, zone the zone
    // your player is in. All zero on failure.
    sco_result Camera(double (&pos)[3], double (&rot)[4], uint64_t& zone) const noexcept {
        std::memset(pos, 0, sizeof(pos));
        std::memset(rot, 0, sizeof(rot));
        zone = 0;
        return t_ ? t_->camera(pos, rot, &zone) : SCO_UNAVAILABLE;
    }

    // The reason for the last failed call of any plugin.
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
    const sc_world_v1* t_ = nullptr;
    sco_plugin* self_ = nullptr;
};

}  // namespace sco::sdk::game

#endif  // SCOSDK_GAME_WORLD_HPP
