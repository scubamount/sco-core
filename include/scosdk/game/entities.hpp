// scosdk/game/entities.hpp: the game pack's service "game.entities" (sc_entities.h) for C++ plugins.
// Header-only, over sco_api.h and sc_entities.h.
//
//   sco::sdk::game::Entities ent;
//   if (ent.Open(*this) == SCO_OK) {
//       const double pos[3] = { 0, 0, 100 }, rot[4] = { 0, 0, 0, 1 };
//       uint64_t id = 0;
//       if (ent.Spawn("DRAK_Cutlass_Black", zone, pos, rot, id) != SCO_OK)   // zone, pos: teleport.spatial
//           Warn("spawn: %s", ent.LastError().c_str());
//       std::string cls;
//       if (ent.Alive(id) && ent.ClassOf(id, cls) == SCO_OK) { ... }
//       ent.SetTransform(id, zone, pos, rot);
//       ent.Despawn(id);
//   }
//
// Game thread only, like the table. Every call answers sco_result and is noexcept. The service is
// host-owned, so an open Entities may be kept for the plugin's life. The entities a plugin spawns
// are its own, and its watches end, when it unloads or crashes. Capabilities:
// has("game.entities.transform"), has("game.entities.spawn"), has("game.entities.class_of"),
// has("game.entities.query_radius") and has("game.entities.watch"); the last two stay off until
// an in-game check has confirmed them. Reference: docs/game-services.md, docs/sdk-cpp.md.
// GPL-3.0, like sco-core.
#ifndef SCOSDK_GAME_ENTITIES_HPP
#define SCOSDK_GAME_ENTITIES_HPP

#include "../plugin.hpp"
#include "sc_entities.h"

#include <string>

namespace sco::sdk::game {

class Entities {
public:
    // Finds game.entities 1.x. SCO_UNAVAILABLE on a 1.0 host; SCO_NOT_FOUND when the host has no
    // game pack (or the product didn't start its services). Both leave the Entities empty.
    sco_result Open(const Plugin& plugin) noexcept { return Open(plugin.Api(), plugin.Self()); }
    // The same for code that holds the C handles (a C-style plugin, a test).
    sco_result Open(const sco_api* api, sco_plugin* self) noexcept {
        t_ = nullptr;
        self_ = self;
        if (!api || !self_) return SCO_BAD_ARG;
        if (!Covers(api->size, offsetof(sco_api, query_service))) return SCO_UNAVAILABLE;
        const void* table = nullptr;
        const sco_result r = api->query_service(SC_ENTITIES_NAME, SC_ENTITIES_VERSION_1_0, &table);
        if (r != SCO_OK) return r;
        t_ = static_cast<const sc_entities_v1*>(table);
        return SCO_OK;
    }
    explicit operator bool() const noexcept { return t_ != nullptr; }
    const sc_entities_v1* Table() const noexcept { return t_; }

    // True when the entity is streamed in.
    bool Alive(uint64_t id) const noexcept { return t_ && t_->alive(id) != 0; }
    // The entity's class name (the name Spawn takes). SCO_NOT_FOUND: not streamed in.
    sco_result ClassOf(uint64_t id, std::string& cls) const noexcept {
        cls.clear();
        if (!t_) return SCO_UNAVAILABLE;
        try {
            std::string s(128, '\0');
            uint32_t size = static_cast<uint32_t>(s.size());
            sco_result r = t_->class_of(id, s.data(), &size);
            if (r == SCO_TOO_MANY) {
                s.resize(size);
                r = t_->class_of(id, s.data(), &size);
            }
            if (r != SCO_OK || size == 0) return r == SCO_OK ? SCO_FAILED : r;
            s.resize(size - 1);
            cls = std::move(s);
            return SCO_OK;
        } catch (...) {
            return SCO_FAILED;
        }
    }
    // Position (metres) and rotation (x, y, z, w) in the entity's zone's local frame, and that
    // zone's id. SCO_NOT_FOUND: not streamed in, or not in a zone.
    sco_result GetTransform(uint64_t id, double (&pos)[3], double (&rot)[4], uint64_t& zoneId) const noexcept {
        zoneId = 0;
        pos[0] = pos[1] = pos[2] = 0;
        rot[0] = rot[1] = rot[2] = rot[3] = 0;
        return t_ ? t_->get_transform(id, pos, rot, &zoneId) : SCO_UNAVAILABLE;
    }
    // Moves and turns an entity you may move (one you spawned, or the player's own vehicle); pos
    // and rot are in zone zoneId's local frame.
    sco_result SetTransform(uint64_t id, uint64_t zoneId, const double (&pos)[3], const double (&rot)[4]) const noexcept {
        return t_ ? t_->set_transform(self_, id, zoneId, pos, rot) : SCO_UNAVAILABLE;
    }
    // An entity of entityClass at pos, facing rot, in zone zoneId's local frame, anywhere; id is
    // yours to despawn. It streams in a few seconds later.
    sco_result Spawn(const char* entityClass, uint64_t zoneId, const double (&pos)[3], const double (&rot)[4],
                     uint64_t& id) const noexcept {
        id = 0;
        return t_ ? t_->spawn(self_, entityClass, zoneId, pos, rot, &id) : SCO_UNAVAILABLE;
    }
    // Removes an entity this plugin spawned (it leaves the world within a few seconds).
    sco_result Despawn(uint64_t id) const noexcept { return t_ ? t_->despawn(self_, id) : SCO_UNAVAILABLE; }

    // Calls fn(ctx, what, id, class) on the game thread for every entity of `type` (a class name,
    // or "PREFIX*") that streams in or out (what: SC_ENTITY_STREAMED_IN, SC_ENTITY_STREAMED_OUT or
    // both ORed), until Unwatch or until this plugin unloads. SCO_UNAVAILABLE until the in-game
    // check of the hooks has confirmed them.
    sco_result Watch(uint32_t what, const char* type, sc_entity_watch_fn fn, void* ctx, uint64_t& watchId) const noexcept {
        watchId = 0;
        return t_ ? t_->watch(self_, what, type, fn, ctx, &watchId) : SCO_UNAVAILABLE;
    }
    sco_result Unwatch(uint64_t watchId) const noexcept { return t_ ? t_->unwatch(self_, watchId) : SCO_UNAVAILABLE; }

    // Ids of streamed-in entities within radius metres of pos in zone zoneId's frame, of class
    // classFilter (a class name or "PREFIX*"; nullptr or "" = any): up to max are written to ids,
    // count of them, and more = how many matched but didn't fit. SCO_UNAVAILABLE until the
    // in-game check of the entity walk has confirmed it.
    sco_result QueryRadius(uint64_t zoneId, const double (&pos)[3], double radius, const char* classFilter, uint64_t* ids,
                           uint32_t max, uint32_t& count, uint32_t& more) const noexcept {
        count = more = 0;
        return t_ ? t_->query_radius(zoneId, pos, radius, classFilter, ids, max, &count, &more) : SCO_UNAVAILABLE;
    }

    // The reason for this plugin's last failed SetTransform, Spawn, Despawn, Watch or Unwatch.
    std::string LastError() const noexcept { return Error(self_); }
    // The reason for the last failed ClassOf, GetTransform or QueryRadius (they take no plugin handle).
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

    const sc_entities_v1* t_ = nullptr;
    sco_plugin* self_ = nullptr;
};

}  // namespace sco::sdk::game

#endif  // SCOSDK_GAME_ENTITIES_HPP
