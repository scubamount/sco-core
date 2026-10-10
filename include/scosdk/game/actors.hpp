// scosdk/game/actors.hpp: the game pack's service "game.actors" (sc_actors.h) for C++ plugins.
// Header-only, over sco_api.h and sc_actors.h.
//
//   sco::sdk::game::Actors actors;
//   if (actors.Open(*this) == SCO_OK) {
//       uint64_t actor = 0, me = 0;
//       if (actors.LocalPlayer(actor, me) == SCO_OK) { ... }
//       uint64_t npc = 0;
//       if (actors.SpawnNpc("Human_NPC_Archetype", zone, pos, npc) != SCO_OK)   // zone, pos: teleport.spatial
//           Warn("spawn_npc: %s", actors.LastError().c_str());
//       actors.Despawn(npc);
//   }
//
// Game thread only, like the table. Every call answers sco_result and is noexcept. The service
// is host-owned, so an open Actors may be kept for the plugin's life. The NPCs a plugin spawns
// are its own and are removed when it unloads or crashes. Capabilities: has("game.actors.
// local_player"), has("game.actors.spawn_npc"), has("game.actors.despawn"). Reference:
// docs/game-services.md, docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_GAME_ACTORS_HPP
#define SCOSDK_GAME_ACTORS_HPP

#include "../plugin.hpp"
#include "sc_actors.h"

#include <string>

namespace sco::sdk::game {

class Actors {
public:
    // Finds game.actors 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND when the host has no
    // game pack (or the product didn't start its services). Both leave the Actors empty.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SC_ACTORS_NAME, SC_ACTORS_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sc_actors_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sc_actors_v1* Table() const noexcept { return t_; }

    // Your actor id and entity id (0 on failure). SCO_NOT_FOUND: you haven't spawned yet.
    sco_result LocalPlayer(uint64_t& actorId, uint64_t& entityId) const noexcept {
        actorId = entityId = 0;
        return t_ ? t_->local_player(&actorId, &entityId) : SCO_UNAVAILABLE;
    }
    // An NPC of archetypeClass at pos in zone zoneId's local frame; id is yours to despawn.
    sco_result SpawnNpc(const char* archetypeClass, uint64_t zoneId, const double (&pos)[3], uint64_t& id) const noexcept {
        id = 0;
        return t_ ? t_->spawn_npc(self_, archetypeClass, zoneId, pos, &id) : SCO_UNAVAILABLE;
    }
    // Removes an NPC this plugin spawned (it leaves the world within a few seconds).
    sco_result Despawn(uint64_t id) const noexcept { return t_ ? t_->despawn(self_, id) : SCO_UNAVAILABLE; }

    // The reason for this plugin's last failed spawn_npc or despawn.
    std::string LastError() const noexcept { return Error(self_); }
    // The reason for the last failed LocalPlayer (it takes no plugin handle).
    std::string LastReadError() const noexcept { return Error(nullptr); }

private:
    std::string Error(sco_plugin* who) const noexcept {
        if (!t_) return {};
        try {
            std::string s(256, '\0');
            uint32_t size = static_cast<uint32_t>(s.size());
            sco_result r = t_->last_error(who, s.data(), &size);
            if (r == SCO_TOO_MANY) {
                s.resize(size);
                r = t_->last_error(who, s.data(), &size);
            }
            if (r != SCO_OK || size == 0) return {};
            s.resize(size - 1);
            return s;
        } catch (...) {
            return {};
        }
    }

    const sc_actors_v1* t_ = nullptr;
    sco_plugin* self_ = nullptr;
};

}  // namespace sco::sdk::game

#endif  // SCOSDK_GAME_ACTORS_HPP
