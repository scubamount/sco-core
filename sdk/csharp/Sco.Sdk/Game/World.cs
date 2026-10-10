// World.cs: the game pack's service "game.world" 1.0 (sc_world.h): ray casts into the game's physics
// and the camera the player sees through. Sco.Sdk.Game holds the game pack's services
// (docs/game-services.md).
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk.Game
{
    /// <summary>The names, versions and limits sc_world.h defines.</summary>
    public static partial class GameAbi
    {
        public const string WorldName = "game.world";            // SC_WORLD_NAME
        public const uint WorldVersion1_0 = 0x00010000u;        // SC_WORLD_VERSION_1_0
        public const int WorldRayMaxDistance = 20000;           // SC_WORLD_RAY_MAX_DISTANCE, metres
    }

    /// <summary>sc_world_hit_flag: ScWorldHit.flags bits. 1.0 sets neither.</summary>
    [Flags]
    public enum ScWorldHitFlag
    {
        Entity = 0x1,    // entity_id is the entity that was hit
        Normal = 0x2,    // normal is the surface normal there
        Force32 = 0x7fffffff,
    }

    /// <summary>sc_world_hit: 72 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScWorldHit
    {
        public uint size;
        public uint flags;
        public ulong entity_id;
        public fixed double pos[3];
        public fixed double normal[3];
        public double distance;
    }

    /// <summary>sc_world_v1, the table of the game service "game.world": 32 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScWorldV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<ulong, double*, double*, double, ScWorldHit*, ScoResult> raycast;
        public delegate* unmanaged[Cdecl]<double*, double*, ulong*, ScoResult> camera;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>A ray's hit: where, how far, and which optional fields the game pack filled.</summary>
    public readonly record struct WorldHit(ScWorldHitFlag Flags, ulong EntityId, double X, double Y, double Z,
                                           double NormalX, double NormalY, double NormalZ, double Distance);

    /// <summary>The camera: the world frame's position (metres) and rotation (x, y, z, w), and the zone your player is in.</summary>
    public readonly record struct WorldCamera(double X, double Y, double Z, double Qx, double Qy, double Qz, double Qw, ulong ZoneId);

    /// <summary>
    /// The game service "game.world" (docs/game-services.md). Published by the Star Citizen game
    /// pack: Open answers NotFound on a host without it. Game thread only (a command, a tick or a
    /// RunOnGameThread task); from another thread every call answers WrongThread. Both calls are
    /// read-only. Capabilities: game.world.raycast, game.world.camera. There is no field of view: the
    /// game doesn't pin one (sc_world.h).
    /// </summary>
    public sealed unsafe class World
    {
        private Plugin? _plugin;
        private ScWorldV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries game.world 1.0. NotFound without the game pack's services;
        /// Unavailable on a 1.0 host.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(GameAbi.WorldName, GameAbi.WorldVersion1_0, out ServiceRef<ScWorldV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(24)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        /// <summary>A ray from from (x, y, z metres, in zone's local frame) along dir for at most
        /// maxDist metres (longer is clamped to GameAbi.WorldRayMaxDistance). Ok: hit is filled.
        /// NotFound: nothing hit, the zone isn't streamed in, or you haven't spawned (LastError says
        /// which). BadArg: a bad pointer, zone 0, a zero or non-finite direction or distance.</summary>
        public ScoResult Raycast(ulong zoneId, ReadOnlySpan<double> from, ReadOnlySpan<double> dir, double maxDist, out WorldHit hit)
        {
            hit = default;
            if (from.Length != 3 || dir.Length != 3) return ScoResult.BadArg;
            if (_t == null) return ScoResult.Unavailable;
            ScWorldHit h = default;
            h.size = (uint)sizeof(ScWorldHit);
            ScoResult r;
            fixed (double* f = from)
            fixed (double* d = dir)
                r = _t->raycast(zoneId, f, d, maxDist, &h);
            if (r == ScoResult.Ok)
                hit = new WorldHit((ScWorldHitFlag)h.flags, h.entity_id, h.pos[0], h.pos[1], h.pos[2],
                                   h.normal[0], h.normal[1], h.normal[2], h.distance);
            return r;
        }

        /// <summary>The camera you see through. NotFound: you haven't spawned yet.</summary>
        public ScoResult Camera(out WorldCamera camera)
        {
            camera = default;
            if (_t == null) return ScoResult.Unavailable;
            double* pos = stackalloc double[3];
            double* rot = stackalloc double[4];
            ulong zone = 0;
            ScoResult r = _t->camera(pos, rot, &zone);
            if (r == ScoResult.Ok) camera = new WorldCamera(pos[0], pos[1], pos[2], rot[0], rot[1], rot[2], rot[3], zone);
            return r;
        }

        /// <summary>The reason for the last failed call of any plugin ("" if none).</summary>
        public string LastError()
        {
            if (_t == null) return "";
            nint t = (nint)_t;
            nint self = (nint)(_plugin == null ? null : _plugin.SelfPtr);
            return Storage.Sized((buf, size) => ((ScWorldV1*)t)->last_error((void*)self, buf, size), out byte[] b) == ScoResult.Ok
                ? Utf8.FromBytes(b) : "";
        }
    }
}
