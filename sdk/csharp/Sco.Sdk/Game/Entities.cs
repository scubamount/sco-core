// Entities.cs: the game pack's service "game.entities" 1.0 (sc_entities.h): ask about any entity,
// spawn and despawn entities of your own anywhere, move what you may move, and watch entities
// stream in and out. Sco.Sdk.Game holds the game pack's services (docs/game-services.md).
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
        public const string EntitiesName = "game.entities";      // SC_ENTITIES_NAME
        public const uint EntitiesVersion1_0 = 0x00010000u;     // SC_ENTITIES_VERSION_1_0
        public const uint EntitiesMaxOwned = 1024u;             // SC_ENTITIES_MAX_OWNED
        public const uint EntitiesMaxWatches = 64u;             // SC_ENTITIES_MAX_WATCHES
        public const uint EntitiesMaxTypeLen = 63u;             // SC_ENTITIES_MAX_TYPE_LEN
        public const uint EntityStreamedIn = 1u;                // SC_ENTITY_STREAMED_IN
        public const uint EntityStreamedOut = 2u;               // SC_ENTITY_STREAMED_OUT
    }

    /// <summary>sc_entities_v1, the table of the game service "game.entities": 88 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScEntitiesV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<ulong, int> alive;
        public delegate* unmanaged[Cdecl]<ulong, byte*, uint*, ScoResult> class_of;
        public delegate* unmanaged[Cdecl]<ulong, double*, double*, ulong*, ScoResult> get_transform;
        public delegate* unmanaged[Cdecl]<void*, ulong, ulong, double*, double*, ScoResult> set_transform;
        public delegate* unmanaged[Cdecl]<void*, byte*, ulong, double*, double*, ulong*, ScoResult> spawn;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> despawn;
        public delegate* unmanaged[Cdecl]<void*, uint, byte*, delegate* unmanaged[Cdecl]<void*, uint, ulong, byte*, void>, void*, ulong*, ScoResult> watch;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> unwatch;
        public delegate* unmanaged[Cdecl]<ulong, double*, double, byte*, ulong*, uint, uint*, uint*, ScoResult> query_radius;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>
    /// The game service "game.entities" (docs/game-services.md). Published by the Star Citizen game
    /// pack: Open answers NotFound on a host without it. Game thread only (a command, a tick or a
    /// RunOnGameThread task); from another thread every call answers WrongThread. An entity you
    /// spawn is yours: only you may despawn it, and the host removes it, and ends your watches,
    /// when your plugin unloads or crashes. Ids are the game's session handles, never keys to
    /// store. Positions are teleport.spatial's: metres in a zone's local frame, rotations
    /// x, y, z, w. Capabilities: game.entities.transform, .spawn, .class_of, .query_radius and
    /// .watch; the last two stay off until an in-game check has confirmed them.
    /// </summary>
    public sealed unsafe class Entities
    {
        private Plugin? _plugin;
        private ScEntitiesV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries game.entities 1.0. NotFound without the game pack's services;
        /// Unavailable on a 1.0 host.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(GameAbi.EntitiesName, GameAbi.EntitiesVersion1_0, out ServiceRef<ScEntitiesV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(80)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        /// <summary>True when the entity is streamed in.</summary>
        public bool Alive(ulong id) => _t != null && _t->alive(id) != 0;

        /// <summary>The entity's class name (the name Spawn takes). NotFound: not streamed in.</summary>
        public ScoResult ClassOf(ulong id, out string cls)
        {
            cls = "";
            if (_t == null) return ScoResult.Unavailable;
            nint t = (nint)_t;
            ScoResult r = Storage.Sized((buf, size) => ((ScEntitiesV1*)t)->class_of(id, buf, size), out byte[] b);
            if (r == ScoResult.Ok) cls = Utf8.FromBytes(b);
            return r;
        }

        /// <summary>The entity's position (3 doubles, metres) and rotation (4 doubles, x y z w) in
        /// its zone's local frame, and that zone's id. NotFound: not streamed in, or not in a zone.</summary>
        public ScoResult GetTransform(ulong id, Span<double> pos, Span<double> rot, out ulong zoneId)
        {
            zoneId = 0;
            if (pos.Length != 3 || rot.Length != 4) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            ulong z = 0;
            ScoResult r;
            fixed (double* p = pos)
            fixed (double* q = rot)
                r = _t->get_transform(id, p, q, &z);
            zoneId = z;
            return r;
        }

        /// <summary>Moves and turns an entity you may move (one you spawned, or the player's own
        /// vehicle); pos and rot are in zone zoneId's local frame.</summary>
        public ScoResult SetTransform(ulong id, ulong zoneId, ReadOnlySpan<double> pos, ReadOnlySpan<double> rot)
        {
            if (pos.Length != 3 || rot.Length != 4) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            fixed (double* p = pos)
            fixed (double* q = rot)
                return _t->set_transform(Self, id, zoneId, p, q);
        }

        /// <summary>Spawns an entity of entityClass at pos, facing rot, in zone zoneId's local frame,
        /// anywhere; id is yours to despawn. It streams in a few seconds later.</summary>
        public ScoResult Spawn(string entityClass, ulong zoneId, ReadOnlySpan<double> pos, ReadOnlySpan<double> rot, out ulong id)
        {
            ArgumentNullException.ThrowIfNull(entityClass);
            id = 0;
            if (pos.Length != 3 || rot.Length != 4) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            ulong n = 0;
            ScoResult r;
            fixed (byte* c = Utf8.Z(entityClass))
            fixed (double* p = pos)
            fixed (double* q = rot)
                r = _t->spawn(Self, c, zoneId, p, q, &n);
            id = n;
            return r;
        }

        /// <summary>Removes an entity this plugin spawned; it leaves the world within a few seconds.</summary>
        public ScoResult Despawn(ulong id) => _t == null ? ScoResult.Unavailable : _t->despawn(Self, id);

        /// <summary>Calls fn(ctx, what, id, className) on the game thread for every entity of
        /// <paramref name="type"/> (a class name, or "PREFIX*") that streams in or out (what:
        /// GameAbi.EntityStreamedIn, EntityStreamedOut or both ORed), until Unwatch or until this
        /// plugin unloads. Unavailable until the in-game check of the hooks has confirmed them.</summary>
        public ScoResult Watch(uint what, string type, delegate* unmanaged[Cdecl]<void*, uint, ulong, byte*, void> fn, void* ctx, out ulong watchId)
        {
            ArgumentNullException.ThrowIfNull(type);
            watchId = 0;
            if (_t == null) return ScoResult.Unavailable;
            ulong w = 0;
            ScoResult r;
            fixed (byte* c = Utf8.Z(type))
                r = _t->watch(Self, what, c, fn, ctx, &w);
            watchId = w;
            return r;
        }

        /// <summary>Ends a watch this plugin registered.</summary>
        public ScoResult Unwatch(ulong watchId) => _t == null ? ScoResult.Unavailable : _t->unwatch(Self, watchId);

        /// <summary>Ids of streamed-in entities within radius metres of pos in zone zoneId's frame,
        /// of class classFilter (a class name or "PREFIX*"; null or "" = any): up to ids.Length are
        /// written, count of them, and more = how many matched but didn't fit. Unavailable until the
        /// in-game check of the entity walk has confirmed it.</summary>
        public ScoResult QueryRadius(ulong zoneId, ReadOnlySpan<double> pos, double radius, string? classFilter, Span<ulong> ids, out uint count, out uint more)
        {
            count = more = 0;
            if (pos.Length != 3) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            uint c = 0, m = 0;
            ScoResult r;
            fixed (byte* f = classFilter == null ? null : Utf8.Z(classFilter))
            fixed (double* p = pos)
            fixed (ulong* o = ids)
                r = _t->query_radius(zoneId, p, radius, f, o, (uint)ids.Length, &c, &m);
            count = c;
            more = m;
            return r;
        }

        /// <summary>The reason for this plugin's last failed SetTransform, Spawn, Despawn, Watch or
        /// Unwatch ("" if none).</summary>
        public string LastError() => Error((nint)Self);

        /// <summary>The reason for the last failed ClassOf, GetTransform or QueryRadius, which take no
        /// plugin handle.</summary>
        public string LastReadError() => Error(0);

        private string Error(nint who)
        {
            if (_t == null) return "";
            nint t = (nint)_t;
            return Storage.Sized((buf, size) => ((ScEntitiesV1*)t)->last_error((void*)who, buf, size), out byte[] b) == ScoResult.Ok
                ? Utf8.FromBytes(b) : "";
        }
    }
}
