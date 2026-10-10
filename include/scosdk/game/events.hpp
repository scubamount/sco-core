// scosdk/game/events.hpp: subscribe helpers for the game pack's bus events (sc_game_events.h) in
// C++ plugins. Header-only, over plugin.hpp and sc_game_events.h.
//
//   sco::sdk::Subscription died_ = sco::sdk::game::OnPlayerDied(*this, [this](const sc_game_player_died& e) {
//       Info("you died (entity %llu)", static_cast<unsigned long long>(e.entity_id));
//   });
//
// Each helper subscribes through Plugin::Subscribe and hands your function a copy of the payload,
// after checking its size field covers the struct this header was built against (a payload from an
// older game pack that is shorter is dropped, not read past its end). The handler runs on the game
// thread, a frame or two after the game did it. An event whose capability isn't ready never fires
// (has("game.events.player_spawned"), "game.events.player_died", "game.events.zone_changed").
// game.vehicle.boarded and game.vehicle.exited are reserved: the game pack doesn't publish them yet,
// so there is no helper. Reference: docs/game-services.md, docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_GAME_EVENTS_HPP
#define SCOSDK_GAME_EVENTS_HPP

#include "../plugin.hpp"
#include "sc_game_events.h"

#include <cstdint>
#include <cstring>
#include <functional>
#include <utility>

namespace sco::sdk::game {

namespace detail {

template <class T>
Subscription SubscribeEvent(Plugin& plugin, const char* event, std::function<void(const T&)> fn) noexcept {
    return plugin.Subscribe(event, [fn = std::move(fn)](const void* data) {
        if (!data || !fn) return;
        uint32_t size = 0;
        std::memcpy(&size, data, sizeof(size));
        if (size < sizeof(T)) return;
        T payload;
        std::memcpy(&payload, data, sizeof(T));
        fn(payload);
    });
}

}  // namespace detail

// game.player.spawned: your player spawned (after a load or a respawn).
[[nodiscard]] inline Subscription OnPlayerSpawned(Plugin& plugin, std::function<void(const sc_game_player_spawned&)> fn) noexcept {
    return detail::SubscribeEvent<sc_game_player_spawned>(plugin, SC_GAME_EVENT_PLAYER_SPAWNED, std::move(fn));
}
// game.player.died: your player died.
[[nodiscard]] inline Subscription OnPlayerDied(Plugin& plugin, std::function<void(const sc_game_player_died&)> fn) noexcept {
    return detail::SubscribeEvent<sc_game_player_died>(plugin, SC_GAME_EVENT_PLAYER_DIED, std::move(fn));
}
// game.zone.changed: your player's zone changed.
[[nodiscard]] inline Subscription OnZoneChanged(Plugin& plugin, std::function<void(const sc_game_zone_changed&)> fn) noexcept {
    return detail::SubscribeEvent<sc_game_zone_changed>(plugin, SC_GAME_EVENT_ZONE_CHANGED, std::move(fn));
}

}  // namespace sco::sdk::game

#endif  // SCOSDK_GAME_EVENTS_HPP
