// scosdk/game/vehicles.hpp: the game service "game.vehicles" (sc_vehicles.h) for C++ plugins.
// Header-only, over sco_api.h and sc_vehicles.h; a thin layer that passes self for you.
//
//   sco::sdk::game::Vehicles veh;
//   if (veh.Open(*this) == SCO_OK && Has("game.vehicles.seats")) {
//       uint64_t ship = 0;
//       sc_vehicle_seat seats[32];
//       uint32_t n = 0;
//       bool more = false;
//       if (veh.PlayerShip(ship) == SCO_OK && veh.Seats(ship, seats, n, more) == SCO_OK)
//           for (uint32_t i = 0; i < n; ++i)
//               if ((seats[i].flags & SC_SEAT_USABLE) && !(seats[i].flags & SC_SEAT_OCCUPIED))
//                   veh.Seat(myNpc, ship, seats[i].index);   // an NPC you spawned with spawn_as
//   }
//
// Every call answers sco_result and is noexcept; on failure LastError has the reason. Game
// thread only (sc_vehicles.h). The game pack publishes the service before plugins load and
// withdraws it after they unload, so a Vehicles may be kept for the plugin's life.
// Reference: docs/api-v1.md (game.vehicles), docs/sdk-cpp.md. GPL-3.0, like sco-core.
#ifndef SCOSDK_GAME_VEHICLES_HPP
#define SCOSDK_GAME_VEHICLES_HPP

#include "../plugin.hpp"
#include "../service.hpp"
#include "sc_vehicles.h"

#include <cstdint>
#include <span>

namespace sco::sdk::game {

class Vehicles {
public:
    // SCO_OK: found (1.0 or a later 1.x). SCO_NOT_FOUND: no game pack, or the product didn't turn
    // the game services on. SCO_UNAVAILABLE: a 1.0 host.
    sco_result Open(const Plugin& plugin) noexcept {
        self_ = plugin.Self();
        return ref_.Query(plugin, SC_VEHICLES_SERVICE_NAME, SC_VEHICLES_SERVICE_VERSION);
    }
    explicit operator bool() const noexcept { return static_cast<bool>(ref_); }
    const sc_vehicles_v1* Table() const noexcept { return ref_.Get(); }

    sco_result PlayerShip(uint64_t& ship) const noexcept {
        ship = 0;
        return ref_ ? ref_->player_ship(&ship) : SCO_UNAVAILABLE;
    }
    // Fills out with the ship's seats; count = how many were written, more = the ship has more.
    sco_result Seats(uint64_t ship, std::span<sc_vehicle_seat> out, uint32_t& count, bool& more) const noexcept {
        count = 0;
        more = false;
        if (!ref_) return SCO_UNAVAILABLE;
        uint32_t m = 0;
        const sco_result r = ref_->seats(ship, out.data(), static_cast<uint32_t>(out.size()), &count, &m);
        more = m != 0;
        return r;
    }
    sco_result SeatOccupant(uint64_t ship, uint32_t seatIndex, uint64_t& actor) const noexcept {
        actor = 0;
        return ref_ ? ref_->seat_occupant(ship, seatIndex, &actor) : SCO_UNAVAILABLE;
    }
    sco_result Seat(uint64_t actor, uint64_t ship, uint32_t seatIndex) const noexcept {
        return ref_ ? ref_->seat(self_, actor, ship, seatIndex) : SCO_UNAVAILABLE;
    }
    sco_result Eject(uint64_t actor) const noexcept { return ref_ ? ref_->eject(self_, actor) : SCO_UNAVAILABLE; }
    sco_result PowerOn(uint64_t ship) const noexcept { return ref_ ? ref_->power_on(self_, ship) : SCO_UNAVAILABLE; }

    // The reason for the last failed call into out, NUL-terminated ("" when none). SCO_TOO_MANY:
    // out is too small and stays ""; 256 bytes always fit the game pack's reasons.
    sco_result LastError(std::span<char> out) const noexcept {
        if (out.empty()) return SCO_BAD_ARG;
        out[0] = '\0';
        if (!ref_) return SCO_UNAVAILABLE;
        uint32_t size = static_cast<uint32_t>(out.size());
        return ref_->last_error(self_, out.data(), &size);
    }

private:
    ServiceRef<sc_vehicles_v1> ref_;
    sco_plugin* self_ = nullptr;
};

}  // namespace sco::sdk::game

#endif  // SCOSDK_GAME_VEHICLES_HPP
