// Actors.cs: the game pack's service "game.actors" 1.0 (sc_actors.h): your player, and NPCs a
// plugin spawns and despawns. Sco.Sdk.Game holds the game pack's services (docs/game-services.md).
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk.Game
{
    /// <summary>The names, versions and limits of the game pack's C headers.</summary>
    public static partial class GameAbi
    {
        public const string ActorsName = "game.actors";      // SC_ACTORS_NAME
        public const uint ActorsVersion1_0 = 0x00010000u;   // SC_ACTORS_VERSION_1_0
        public const uint ActorsMaxNpcs = 1024u;            // SC_ACTORS_MAX_NPCS
    }

    /// <summary>sc_actors_v1, the table of the game service "game.actors": 40 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScActorsV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<ulong*, ulong*, ScoResult> local_player;
        public delegate* unmanaged[Cdecl]<void*, byte*, ulong, double*, ulong*, ScoResult> spawn_npc;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> despawn;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>
    /// The game service "game.actors" (docs/game-services.md). Published by the Star Citizen game
    /// pack: Open answers NotFound on a host without it. Game thread only (a command, a tick or a
    /// RunOnGameThread task); from another thread every call answers WrongThread. An NPC you spawn
    /// is yours: only you may despawn it, and the host removes it when your plugin unloads or
    /// crashes. Ids are the game's session handles, never keys to store. Capabilities:
    /// game.actors.local_player, game.actors.spawn_npc, game.actors.despawn.
    /// </summary>
    public sealed unsafe class Actors
    {
        private Plugin? _plugin;
        private ScActorsV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries game.actors 1.0. NotFound without the game pack's services;
        /// Unavailable on a 1.0 host.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(GameAbi.ActorsName, GameAbi.ActorsVersion1_0, out ServiceRef<ScActorsV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(32)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        /// <summary>Your actor id and entity id (0 on failure). NotFound: you haven't spawned yet.</summary>
        public ScoResult LocalPlayer(out ulong actorId, out ulong entityId)
        {
            ulong a = 0, e = 0;
            ScoResult r = _t == null ? ScoResult.Unavailable : _t->local_player(&a, &e);
            actorId = a;
            entityId = e;
            return r;
        }

        /// <summary>Spawns an NPC of archetypeClass at pos (x, y, z metres) in zone zoneId's local
        /// frame; id is yours to despawn. The NPC streams in a few seconds later.</summary>
        public ScoResult SpawnNpc(string archetypeClass, ulong zoneId, ReadOnlySpan<double> pos, out ulong id)
        {
            ArgumentNullException.ThrowIfNull(archetypeClass);
            id = 0;
            if (pos.Length != 3) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            ulong n = 0;
            ScoResult r;
            fixed (byte* c = Utf8.Z(archetypeClass))
            fixed (double* p = pos)
                r = _t->spawn_npc(Self, c, zoneId, p, &n);
            id = n;
            return r;
        }

        /// <summary>Removes an NPC this plugin spawned; it leaves the world within a few seconds.</summary>
        public ScoResult Despawn(ulong id) => _t == null ? ScoResult.Unavailable : _t->despawn(Self, id);

        /// <summary>The reason for this plugin's last failed SpawnNpc or Despawn ("" if none).</summary>
        public string LastError() => Error((nint)Self);

        /// <summary>The reason for the last failed LocalPlayer, which takes no plugin handle.</summary>
        public string LastReadError() => Error(0);

        private string Error(nint who)
        {
            if (_t == null) return "";
            nint t = (nint)_t;
            return Storage.Sized((buf, size) => ((ScActorsV1*)t)->last_error((void*)who, buf, size), out byte[] b) == ScoResult.Ok
                ? Utf8.FromBytes(b) : "";
        }
    }
}
