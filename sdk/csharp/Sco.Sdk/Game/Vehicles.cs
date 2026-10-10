// Vehicles.cs: the game service "game.vehicles" 1.0 (sc_vehicles.h) for C# plugins.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk.Game
{
    /// <summary>The names, versions and limits sc_vehicles.h defines.</summary>
    public static partial class GameAbi
    {
        public const string VehiclesName = "game.vehicles";       // SC_VEHICLES_SERVICE_NAME
        public const uint VehiclesVersion1_0 = 0x00010000u;      // SC_VEHICLES_SERVICE_VERSION
        public const int VehicleSeatNameMax = 64;                // SC_VEHICLE_SEAT_NAME_MAX
    }

    /// <summary>sc_vehicle_seat_flag: ScVehicleSeat.flags bits.</summary>
    [Flags]
    public enum ScVehicleSeatFlag
    {
        Usable = 0x1,        // the game's own seat picker takes it; seat() refuses the others
        UsableKnown = 0x2,   // Usable could be checked on this game build
        Occupied = 0x4,      // someone or something is in it
        Pilot = 0x8,         // the ship's highest-priority seat
        Force32 = 0x7fffffff,
    }

    /// <summary>sc_vehicle_seat: one seat, 96 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScVehicleSeat
    {
        public uint index;
        public uint flags;
        public ulong seat_id;
        public ulong occupant_id;
        public uint priority;
        public uint _pad;
        public fixed byte name[GameAbi.VehicleSeatNameMax];

        public readonly ScVehicleSeatFlag Flags => (ScVehicleSeatFlag)flags;

        /// <summary>The seat item's entity name.</summary>
        public readonly string Name
        {
            get
            {
                fixed (byte* p = name) return Utf8.FromBytes(new ReadOnlySpan<byte>(p, GameAbi.VehicleSeatNameMax));
            }
        }
    }

    /// <summary>sc_vehicles_v1, the table of the game service "game.vehicles": 64 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScVehiclesV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<ulong*, ScoResult> player_ship;
        public delegate* unmanaged[Cdecl]<ulong, ScVehicleSeat*, uint, uint*, uint*, ScoResult> seats;
        public delegate* unmanaged[Cdecl]<ulong, uint, ulong*, ScoResult> seat_occupant;
        public delegate* unmanaged[Cdecl]<void*, ulong, ulong, uint, ScoResult> seat;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> eject;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> power_on;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>
    /// The game service "game.vehicles" (sc_vehicles.h): the ship you're aboard, its seats, seating
    /// your own actors and Flight Ready. Game thread only. Open once: the game pack publishes it
    /// before plugins load and withdraws it after they unload. Every call answers a ScoResult;
    /// <see cref="LastError"/> has the reason for the last failure. Check the capabilities
    /// game.vehicles.seats / .seat / .flight_ready with Plugin.Has.
    /// </summary>
    public sealed unsafe class Vehicles
    {
        private Plugin? _plugin;
        private ScVehiclesV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries game.vehicles 1.0. NotFound without the game pack or its services.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(GameAbi.VehiclesName, GameAbi.VehiclesVersion1_0, out ServiceRef<ScVehiclesV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(56)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        public ScoResult PlayerShip(out ulong ship)
        {
            ulong id = 0;
            ScoResult r = _t == null ? ScoResult.Unavailable : _t->player_ship(&id);
            ship = id;
            return r;
        }

        /// <summary>Fills seats with the ship's seats; count = how many were written, more = the ship has more.</summary>
        public ScoResult Seats(ulong ship, Span<ScVehicleSeat> seats, out uint count, out bool more)
        {
            count = 0;
            more = false;
            if (_t == null) return ScoResult.Unavailable;
            uint n = 0, m = 0;
            ScoResult r;
            fixed (ScVehicleSeat* p = seats) r = _t->seats(ship, p, (uint)seats.Length, &n, &m);
            count = n;
            more = m != 0;
            return r;
        }

        public ScoResult SeatOccupant(ulong ship, uint seatIndex, out ulong actor)
        {
            ulong id = 0;
            ScoResult r = _t == null ? ScoResult.Unavailable : _t->seat_occupant(ship, seatIndex, &id);
            actor = id;
            return r;
        }

        public ScoResult Seat(ulong actor, ulong ship, uint seatIndex) => _t == null ? ScoResult.Unavailable : _t->seat(Self, actor, ship, seatIndex);
        public ScoResult Eject(ulong actor) => _t == null ? ScoResult.Unavailable : _t->eject(Self, actor);
        public ScoResult PowerOn(ulong ship) => _t == null ? ScoResult.Unavailable : _t->power_on(Self, ship);

        /// <summary>The reason for the last failed call ("" if none).</summary>
        public string LastError()
        {
            if (_t == null) return "";
            return Storage.Sized((buf, size) => _t->last_error(Self, buf, size), out byte[] b) == ScoResult.Ok ? Utf8.FromBytes(b) : "";
        }
    }
}
