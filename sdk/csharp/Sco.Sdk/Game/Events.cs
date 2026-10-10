// Events.cs: the game pack's bus events (sc_game_events.h) for C# plugins: the payload structs and
// subscribe helpers. Sco.Sdk.Game holds the game pack's services (docs/game-services.md).
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Runtime.InteropServices;

namespace Sco.Sdk.Game
{
    /// <summary>The event names sc_game_events.h defines.</summary>
    public static partial class GameAbi
    {
        public const string EventPlayerSpawned = "game.player.spawned";     // SC_GAME_EVENT_PLAYER_SPAWNED
        public const string EventPlayerDied = "game.player.died";           // SC_GAME_EVENT_PLAYER_DIED
        public const string EventZoneChanged = "game.zone.changed";         // SC_GAME_EVENT_ZONE_CHANGED
        public const string EventVehicleBoarded = "game.vehicle.boarded";   // SC_GAME_EVENT_VEHICLE_BOARDED (reserved)
        public const string EventVehicleExited = "game.vehicle.exited";     // SC_GAME_EVENT_VEHICLE_EXITED (reserved)
    }

    /// <summary>sc_game_player_spawned: 24 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScGamePlayerSpawned
    {
        public uint size;
        public uint _pad;
        public ulong entity_id;   // your entity
        public ulong zone_id;     // the zone it is in, 0 if it couldn't be read yet
    }

    /// <summary>sc_game_player_died: 24 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScGamePlayerDied
    {
        public uint size;
        public uint _pad;
        public ulong entity_id;   // your entity
        public ulong killer_id;   // always 0 in 1.0
    }

    /// <summary>sc_game_zone_changed: 24 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScGameZoneChanged
    {
        public uint size;
        public uint _pad;
        public ulong old_zone_id;
        public ulong new_zone_id;
    }

    /// <summary>sc_game_vehicle_seat: 16 bytes; game.vehicle.boarded and exited, reserved (the game pack doesn't publish them yet).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScGameVehicleSeat
    {
        public uint size;
        public int seat_index;           // -1 when unknown
        public ulong vehicle_entity_id;
    }

    /// <summary>
    /// Subscribe helpers for the game pack's low-volume events. The handler runs on the game
    /// thread, a frame or two after the game did it, and gets a copy of the payload; a payload whose
    /// size field is shorter than the struct this SDK was built against is dropped. An event whose
    /// capability isn't ready (game.events.player_spawned, game.events.player_died,
    /// game.events.zone_changed) never fires.
    /// </summary>
    public static unsafe class GameEvents
    {
        public static Subscription OnPlayerSpawned(this Plugin plugin, Action<ScGamePlayerSpawned> handler) =>
            Sub(plugin, GameAbi.EventPlayerSpawned, handler);

        public static Subscription OnPlayerDied(this Plugin plugin, Action<ScGamePlayerDied> handler) =>
            Sub(plugin, GameAbi.EventPlayerDied, handler);

        public static Subscription OnZoneChanged(this Plugin plugin, Action<ScGameZoneChanged> handler) =>
            Sub(plugin, GameAbi.EventZoneChanged, handler);

        private static Subscription Sub<T>(Plugin plugin, string ev, Action<T> handler) where T : unmanaged
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ArgumentNullException.ThrowIfNull(handler);
            return plugin.Subscribe(ev, data =>
            {
                if (data == 0 || *(uint*)data < (uint)sizeof(T)) return;
                handler(*(T*)data);
            });
        }
    }
}
