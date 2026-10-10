// FakeHost.cs: drives the SDK's wrappers through a stand-in sco_api table, as the host would call
// them: commands, the exception boundary, events, tasks, raw handlers, the service size check, and
// the registry sweep at unload (a late call through an old ctx must find nothing).
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Sco.Sdk.Game;
using Sco.Sdk.Interop;

namespace Sco.Sdk.Tests
{
    internal static unsafe class FakeHost
    {
        // What the stand-in host keeps of each registration (copies, like the real one).
        private sealed class Cmd
        {
            public Cmd(string name, delegate* unmanaged[Cdecl]<ScoArg*, uint, void*, byte*, uint, ScoResult> fn, void* ctx, uint nargs)
            { Name = name; Fn = fn; Ctx = ctx; NArgs = nargs; }
            public readonly string Name;
            public readonly delegate* unmanaged[Cdecl]<ScoArg*, uint, void*, byte*, uint, ScoResult> Fn;
            public readonly void* Ctx;
            public readonly uint NArgs;
        }

        private sealed class Sub
        {
            public Sub(string ev, delegate* unmanaged[Cdecl]<byte*, void*, void*, void> fn, void* ctx) { Event = ev; Fn = fn; Ctx = ctx; }
            public readonly string Event;
            public readonly delegate* unmanaged[Cdecl]<byte*, void*, void*, void> Fn;
            public readonly void* Ctx;
        }

        private sealed class Task
        {
            public Task(delegate* unmanaged[Cdecl]<void*, void> fn, void* ctx) { Fn = fn; Ctx = ctx; }
            public readonly delegate* unmanaged[Cdecl]<void*, void> Fn;
            public readonly void* Ctx;
        }

        private sealed class Raw
        {
            public Raw(string name, delegate* unmanaged[Cdecl]<void*, uint, void*, uint*, void*, ScoResult> fn, void* ctx) { Name = name; Fn = fn; Ctx = ctx; }
            public readonly string Name;
            public readonly delegate* unmanaged[Cdecl]<void*, uint, void*, uint*, void*, ScoResult> Fn;
            public readonly void* Ctx;
        }

        private static readonly List<Cmd> Cmds = new();
        private static readonly List<Sub> Subs = new();
        private static readonly List<Task> Tasks = new();
        private static readonly List<Raw> Raws = new();
        private static readonly List<string> Logs = new();
        private static readonly nint Version = Marshal.StringToCoTaskMemUTF8("fake-host 1.0");   // static, never freed
        private static uint* _smallTable;   // a service table whose size covers only itself
        private static ScActorsV1* _actors;  // a stand-in game.actors
        private static ScWorldV1* _world;    // a stand-in game.world
        private static void* _self;
        private static int _failures;

        private static void Expect(bool ok, string what)
        {
            if (ok) return;
            Console.WriteLine("FAIL fake host: " + what);
            ++_failures;
        }

        private static string S(byte* p) => Marshal.PtrToStringUTF8((nint)p) ?? "";

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static byte* HostVersion() => (byte*)Version;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static int Has(byte* cap) => S(cap) == "teleport" ? 1 : 0;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult RunOnGameThread(void* self, delegate* unmanaged[Cdecl]<void*, void> fn, void* ctx)
        {
            Tasks.Add(new Task(fn, ctx));
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult Subscribe(void* self, byte* ev, delegate* unmanaged[Cdecl]<byte*, void*, void*, void> fn, void* ctx)
        {
            string e = S(ev);
            foreach (Sub s in Subs) if (s.Event == e && (nint)s.Fn == (nint)fn) return ScoResult.BadArg;   // the host's rule
            Subs.Add(new Sub(e, fn, ctx));
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult Unsubscribe(void* self, byte* ev, delegate* unmanaged[Cdecl]<byte*, void*, void*, void> fn)
        {
            string e = S(ev);
            int i = Subs.FindIndex(s => s.Event == e && (nint)s.Fn == (nint)fn);
            if (i < 0) return ScoResult.NotFound;
            Subs.RemoveAt(i);
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Status(void* self, byte* m) => Logs.Add("status: " + S(m));

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void Log(void* self, ScoLogLevel level, byte* m) => Logs.Add(level + ": " + S(m));

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult RegisterCommand(void* self, ScoCommand* c)
        {
            if (c->size < sizeof(ScoCommand) || c->fn == null || c->arg_def_size != sizeof(ScoArgDef)) return ScoResult.BadArg;
            Cmds.Add(new Cmd(S(c->name), c->fn, c->ctx, c->nargs));
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult Invoke(void* self, byte* name, ScoArg* args, uint nargs,
                                        delegate* unmanaged[Cdecl]<ScoResult, byte*, void*, void> done, void* ctx)
        {
            string n = S(name);
            Cmd? c = Cmds.Find(x => x.Name == n);
            byte* reply = stackalloc byte[256];
            reply[0] = 0;
            ScoResult r = c == null ? ScoResult.NotFound : c.Fn(args, nargs, c.Ctx, reply, 256);
            if (done != null) done(r, reply, ctx);
            return r;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static uint ListCommands(ScoCommand** o, uint max) => (uint)Cmds.Count;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ProvideService(void* self, byte* name, uint version, void* table) =>
            table == null ? ScoResult.BadArg : ScoResult.Ok;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult QueryService(byte* name, uint minVersion, void** o)
        {
            *o = null;
            if (S(name) == GameAbi.ActorsName) { *o = _actors; return ScoResult.Ok; }
            if (S(name) == GameAbi.WorldName) { *o = _world; return ScoResult.Ok; }
            if (S(name) != "other.small") return ScoResult.NotFound;
            *o = _smallTable;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActLocal(ulong* actor, ulong* entity)
        {
            *actor = 5;
            *entity = 6;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActSpawn(void* self, byte* cls, ulong zone, double* pos, ulong* id)
        {
            if (self != _self || S(cls) != "Npc" || zone != 9 || pos[2] != 3.0) return ScoResult.BadArg;
            *id = 77;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActDespawn(void* self, ulong id) => self == _self && id == 77 ? ScoResult.Ok : ScoResult.NotFound;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActLastError(void* self, byte* o, uint* io)
        {
            byte[] why = System.Text.Encoding.UTF8.GetBytes((self == null ? "read" : "not yours") + "\0");
            uint cap = *io;
            *io = (uint)why.Length;
            if (cap < why.Length) return ScoResult.TooMany;
            for (int i = 0; i < why.Length; ++i) o[i] = why[i];
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActHealth(ulong id, float* cur, float* max)
        {
            *cur = 0;
            *max = 0;
            if (id != 6) return ScoResult.NotFound;
            *cur = 87.5f;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ActState(ulong id, uint* state)
        {
            *state = 0;
            return ScoResult.Unavailable;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult WorldRaycast(ulong zone, double* from, double* dir, double max, ScWorldHit* hit)
        {
            if (hit == null || hit->size < (uint)sizeof(ScWorldHit) || zone == 0) return ScoResult.BadArg;
            if (zone != 9) return ScoResult.NotFound;
            hit->pos[0] = from[0] + dir[0] * max;
            hit->pos[1] = from[1] + dir[1] * max;
            hit->pos[2] = from[2] + dir[2] * max;
            hit->distance = max;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult WorldCamera(double* pos, double* rot, ulong* zone)
        {
            pos[0] = 1; pos[1] = 2; pos[2] = 3;
            rot[0] = 0; rot[1] = 0; rot[2] = 0; rot[3] = 1;
            *zone = 9;
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult WorldLastError(void* self, byte* o, uint* io)
        {
            byte[] why = System.Text.Encoding.UTF8.GetBytes("no hit\0");
            uint cap = *io;
            *io = (uint)why.Length;
            if (cap < why.Length) return ScoResult.TooMany;
            for (int i = 0; i < why.Length; ++i) o[i] = why[i];
            return ScoResult.Ok;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult ReleaseService(void* self, byte* name) => ScoResult.Ok;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult InvokeRaw(void* self, byte* name, void* i, uint inSize, void* o, uint* outSize)
        {
            string n = S(name);
            Raw? r = Raws.Find(x => x.Name == n);
            if (r == null) { if (outSize != null) *outSize = 0; return ScoResult.NotFound; }
            return r.Fn(i, inSize, o, outSize, r.Ctx);
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult RegisterRaw(void* self, byte* name, byte* cap, delegate* unmanaged[Cdecl]<void*, uint, void*, uint*, void*, ScoResult> fn, void* ctx)
        {
            Raws.Add(new Raw(S(name), fn, ctx));
            return ScoResult.Ok;
        }

        private static ScoApi* MakeApi()
        {
            var a = (ScoApi*)NativeMemory.AllocZeroed((nuint)sizeof(ScoApi));
            a->size = (uint)sizeof(ScoApi);
            a->major = Abi.ApiMajor;
            a->minor = Abi.ApiMinor;
            a->host_version = &HostVersion;
            a->has = &Has;
            a->run_on_game_thread = &RunOnGameThread;
            a->subscribe = &Subscribe;
            a->unsubscribe = &Unsubscribe;
            a->status = &Status;
            a->log = &Log;
            a->register_command = &RegisterCommand;
            a->invoke = &Invoke;
            a->list_commands = &ListCommands;
            a->provide_service = &ProvideService;
            a->query_service = &QueryService;
            a->release_service = &ReleaseService;
            a->invoke_raw = &InvokeRaw;
            a->register_raw = &RegisterRaw;
            return a;
        }

        private static void Dispatch(string ev, uint ms)
        {
            byte[] e = System.Text.Encoding.UTF8.GetBytes(ev + "\0");
            foreach (Sub s in Subs.ToArray())
                if (s.Event == ev)
                    fixed (byte* p = e) s.Fn(p, &ms, s.Ctx);
        }

        // An event whose data is a payload struct, as the game pack dispatches the game.* events.
        private static void DispatchPayload(string ev, void* data)
        {
            byte[] e = System.Text.Encoding.UTF8.GetBytes(ev + "\0");
            foreach (Sub s in Subs.ToArray())
                if (s.Event == ev)
                    fixed (byte* p = e) s.Fn(p, data, s.Ctx);
        }

        private static void RunTasks()
        {
            Task[] now = Tasks.ToArray();
            Tasks.Clear();
            foreach (Task t in now) t.Fn(t.Ctx);
        }

        private static (ScoResult, string) Call(Cmd c, string? arg)
        {
            byte* reply = stackalloc byte[256];
            reply[0] = 0;
            ScoArg a = default;
            byte[] s = System.Text.Encoding.UTF8.GetBytes((arg ?? "") + "\0");
            fixed (byte* sp = s)
            {
                a.type = (uint)ScoArgType.String;
                a.v.s = sp;
                ScoResult r = c.Fn(arg == null ? null : &a, arg == null ? 0u : 1u, c.Ctx, reply, 256);
                return (r, S(reply));
            }
        }

        private sealed class TestPlugin : Plugin
        {
            public int Ticks, Ticks2;
            public Subscription? A, B;

            public override ScoResult OnLoad()
            {
                ScoResult r = new CommandBuilder(this, "test.echo").Title("Echo").Arg("text", ScoArgType.String, "What to echo")
                    .Handle((args, reply) => { reply.Set("echo: " + args.String(0)); return ScoResult.Ok; }).Register();
                if (r != ScoResult.Ok) return r;
                r = new CommandBuilder(this, "test.boom").Handle((args, reply) => throw new InvalidOperationException("boom")).Register();
                if (r != ScoResult.Ok) return r;
                A = Subscribe("tick", data => Ticks += (int)TickMs(data));
                B = Subscribe("tick", () => ++Ticks2);
                if (A.Result != ScoResult.Ok) return A.Result;
                return RegisterRaw<int, int>("test.double", null, (in int x, out int y) => { y = x * 2; return ScoResult.Ok; });
            }
        }

        private sealed class ThrowingPlugin : Plugin
        {
            public override ScoResult OnLoad()
            {
                new CommandBuilder(this, "bad.cmd").Handle((a, r) => ScoResult.Ok).Register();
                throw new InvalidOperationException("no");
            }
        }

        public static int Run()
        {
            _smallTable = (uint*)NativeMemory.AllocZeroed(8);
            *_smallTable = 4;
            ScoApi* api = MakeApi();
            void* self = NativeMemory.AllocZeroed(8);
            _self = self;
            _actors = (ScActorsV1*)NativeMemory.AllocZeroed((nuint)sizeof(ScActorsV1));
            _actors->size = (uint)sizeof(ScActorsV1);
            _actors->local_player = &ActLocal;
            _actors->spawn_npc = &ActSpawn;
            _actors->despawn = &ActDespawn;
            _actors->last_error = &ActLastError;
            _actors->health = &ActHealth;
            _actors->state = &ActState;
            _world = (ScWorldV1*)NativeMemory.AllocZeroed((nuint)sizeof(ScWorldV1));
            _world->size = (uint)sizeof(ScWorldV1);
            _world->raycast = &WorldRaycast;
            _world->camera = &WorldCamera;
            _world->last_error = &WorldLastError;

            var p = new TestPlugin();
            Expect(PluginExports.LoadPlugin(p, api, self, "test") == ScoResult.Ok, "load");
            Expect(Cmds.Count == 2 && Cmds[0].Name == "test.echo" && Cmds[0].NArgs == 1, "two commands registered");
            Expect(Subs.Count == 1, "one host subscription for two handlers of one event");

            (ScoResult r, string reply) = Call(Cmds[0], "hi");
            Expect(r == ScoResult.Ok && reply == "echo: hi", $"echo answered {r} \"{reply}\"");
            (r, reply) = Call(Cmds[1], null);
            Expect(r == ScoResult.Failed && reply == "test.boom failed: boom", $"a throwing command answered {r} \"{reply}\"");
            Expect(Logs.Exists(l => l.StartsWith("Error: exception in test.boom", StringComparison.Ordinal)), "the exception is logged");

            Expect(p.Invoke("test.echo", out string got, "there") == ScoResult.Ok && got == "echo: there", "Invoke");

            Dispatch("tick", 100);
            Expect(p.Ticks == 100 && p.Ticks2 == 1, "both handlers ran");
            p.B!.Dispose();
            Dispatch("tick", 100);
            Expect(p.Ticks == 200 && p.Ticks2 == 1 && Subs.Count == 1, "a disposed handler doesn't run");
            p.A!.Dispose();
            Expect(Subs.Count == 0, "the last handler's dispose unsubscribes from the host");

            int ran = 0;
            Expect(p.RunOnGameThread(() => ++ran) == ScoResult.Ok, "task queued");
            p.RunOnGameThread(() => throw new InvalidOperationException("task"));
            RunTasks();
            Expect(ran == 1 && Logs.Exists(l => l.Contains("exception in task", StringComparison.Ordinal)), "tasks run; a throwing task is logged");

            Expect(p.InvokeRaw("test.double", 21, out int doubled) == ScoResult.Ok && doubled == 42, "typed raw handler");
            Expect(p.InvokeRawBytes("test.double", BitConverter.GetBytes(1), Span<byte>.Empty, out uint need) == ScoResult.TooMany && need == 4,
                   "raw size handshake");

            Expect(p.Query("other.small", 0x10000, out ServiceRef<ScoStorageV1> small) == ScoResult.Ok && !small.Covers(8),
                   "the size check refuses a function past the provider's size");
            Expect(p.Query("nobody", 0x10000, out ServiceRef<ScoStorageV1> none) == ScoResult.NotFound && !none.IsValid, "query not found");

            var actors = new Actors();
            Expect(actors.Open(p) == ScoResult.Ok && actors.IsOpen, "game.actors opens");
            Expect(actors.LocalPlayer(out ulong actorId, out ulong entityId) == ScoResult.Ok && actorId == 5 && entityId == 6, "Actors.LocalPlayer");
            Expect(actors.SpawnNpc("Npc", 9, new double[] { 1, 2, 3 }, out ulong npc) == ScoResult.Ok && npc == 77, "Actors.SpawnNpc");
            Expect(actors.SpawnNpc("Npc", 9, new double[] { 1, 2 }, out npc) == ScoResult.BadArg && npc == 0, "SpawnNpc wants 3 coordinates");
            Expect(actors.Despawn(77) == ScoResult.Ok && actors.Despawn(78) == ScoResult.NotFound, "Actors.Despawn");
            Expect(actors.LastError() == "not yours" && actors.LastReadError() == "read", "Actors.LastError, LastReadError");
            Expect(actors.Health(6, out float hp, out float hpMax) == ScoResult.Ok && hp == 87.5f && hpMax == 0f, "Actors.Health (1.1)");
            Expect(actors.Health(7, out hp, out _) == ScoResult.NotFound && hp == 0f, "Health of an unknown id");
            Expect(actors.State(6, out ScActorState st) == ScoResult.Unavailable && st == ScActorState.Alive, "Actors.State is unavailable");
            uint savedSize = _actors->size;
            _actors->size = 40;   // a 1.0 table: the 1.1 functions are past its end
            Expect(actors.Health(6, out _, out _) == ScoResult.Unavailable && actors.State(6, out _) == ScoResult.Unavailable,
                   "a 1.0 table answers Unavailable for the 1.1 functions");
            _actors->size = savedSize;

            var world = new World();
            Expect(world.Open(p) == ScoResult.Ok && world.IsOpen, "game.world opens");
            Expect(world.Raycast(9, new double[] { 0, 0, 1 }, new double[] { 0, 0, -1 }, 10, out WorldHit hit) == ScoResult.Ok
                   && hit.Z == -9 && hit.Distance == 10 && hit.EntityId == 0 && hit.Flags == 0, "World.Raycast");
            Expect(world.Raycast(8, new double[] { 0, 0, 1 }, new double[] { 0, 0, -1 }, 10, out hit) == ScoResult.NotFound && hit == default,
                   "Raycast of an unknown zone");
            Expect(world.Raycast(0, new double[] { 0, 0, 1 }, new double[] { 0, 0, -1 }, 10, out _) == ScoResult.BadArg, "Raycast zone 0");
            Expect(world.Raycast(9, new double[] { 0, 0 }, new double[] { 0, 0, -1 }, 10, out _) == ScoResult.BadArg, "Raycast wants 3 coordinates");
            Expect(world.Camera(out WorldCamera cam) == ScoResult.Ok && cam.X == 1 && cam.Z == 3 && cam.Qw == 1 && cam.ZoneId == 9, "World.Camera");
            Expect(world.LastError() == "no hit", "World.LastError");

            ScGamePlayerDied died = default;
            int diedCount = 0;
            Subscription ds = p.OnPlayerDied(e => { died = e; ++diedCount; });
            Expect(ds.Result == ScoResult.Ok, "OnPlayerDied subscribes");
            var payload = new ScGamePlayerDied { size = (uint)sizeof(ScGamePlayerDied), entity_id = 0x8000000000000016ul };
            DispatchPayload(GameAbi.EventPlayerDied, &payload);
            Expect(diedCount == 1 && died.entity_id == 0x8000000000000016ul && died.killer_id == 0, "game.player.died reaches OnPlayerDied");
            var shortPayload = new ScGamePlayerDied { size = 8, entity_id = 5 };   // shorter than this SDK's struct: dropped
            DispatchPayload(GameAbi.EventPlayerDied, &shortPayload);
            Expect(diedCount == 1, "a payload shorter than the struct is dropped");
            ds.Dispose();

            Cmd echo = Cmds[0];
            p.RunOnGameThread(() => ++ran);   // still queued at unload: must never run
            PluginExports.UnloadPlugin(p);
            Expect(Registry.Count == 0, $"unload swept the registry ({Registry.Count} left)");
            (r, _) = Call(echo, "late");
            Expect(r == ScoResult.NotFound, "a late call through an old ctx finds nothing");
            RunTasks();
            Expect(ran == 1, "a task queued before unload doesn't run after it");

            Cmds.Clear(); Subs.Clear(); Raws.Clear();
            var bad = new ThrowingPlugin();
            Expect(PluginExports.LoadPlugin(bad, api, self, "bad") == ScoResult.Failed, "OnLoad throwing answers Failed");
            Expect(Registry.Count == 0 && !bad.IsLoaded, "a failed load releases what OnLoad registered");
            Expect(Logs.Exists(l => l.StartsWith("Error: exception in OnLoad", StringComparison.Ordinal)), "the OnLoad exception is logged");

            Console.WriteLine($"fake host: {(_failures == 0 ? "OK" : "FAILED")}");
            return _failures;
        }
    }
}
