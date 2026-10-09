// cs_hello: the sco SDK's C# example plugin, built on Sco.Sdk and published with NativeAOT.
//
// The C# version of hello.c and cpp_hello.cpp:
//   - Info() and Status() on load,
//   - Has() to check a capability before offering a feature,
//   - game.ready and tick subscriptions (handles that unsubscribe themselves),
//   - two commands: cs_hello.wave <name> and cs_hello.ticks,
//   - a service, cs_hello.greeter, published on load and looked up on game.ready the way any
//     other plugin would look it up.
//
// Build: see README.md here. GPL-3.0, like sco-core.
using System;
using System.Runtime.InteropServices;
using System.Text;
using Sco.Sdk;
using Sco.Sdk.Interop;

namespace CsHello
{
    // The service's table: a C struct that starts with uint size, so later versions can add
    // functions at the end (callers check with ServiceRef.Covers).
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct GreeterV1
    {
        public uint size;
        public uint _pad;
        // int greet(const char* who, char* out, uint32_t out_size): bytes written, without the NUL.
        public delegate* unmanaged<byte*, byte*, uint, int> greet;
    }

    public sealed unsafe class Hello : Plugin
    {
        // Published tables must stay valid until Release or unload; this one lives as long as the
        // DLL (NativeMemory, allocated once, never freed), like a static in C.
        private static GreeterV1* _greeter;

        private Subscription? _ready, _tick;
        private uint _ticks, _firstMs, _lastReportMs;

        public override ScoResult OnLoad()
        {
            // cs_hello.wave <name>: replies "Hello, <name>". Runs on the game thread.
            ScoResult r = new CommandBuilder(this, "cs_hello.wave")
                .Title("Wave")
                .Help("Says hello on the status line")
                .Arg("name", ScoArgType.String, "Who to wave at")
                .Handle((args, reply) =>
                {
                    reply.Set("Hello, " + args.String(0));
                    return ScoResult.Ok;
                })
                .Register();
            if (r != ScoResult.Ok) return r;

            // cs_hello.ticks: no arguments, so sco-plugin-check runs it on its own.
            r = new CommandBuilder(this, "cs_hello.ticks")
                .Title("Ticks")
                .Help("How many ticks since the plugin loaded")
                .Handle((args, reply) =>
                {
                    reply.Set($"{_ticks} ticks");
                    return ScoResult.Ok;
                })
                .Register();
            if (r != ScoResult.Ok) return r;

            // Services are 1.1: on an older host Provide answers Unavailable; carry on without.
            if (_greeter == null)
            {
                _greeter = (GreeterV1*)NativeMemory.AllocZeroed((nuint)sizeof(GreeterV1));
                _greeter->size = (uint)sizeof(GreeterV1);
                _greeter->greet = &Greet;
            }
            r = Provide("cs_hello.greeter", ServiceVersion(1, 0), _greeter);
            if (r != ScoResult.Ok) Warn($"greeter service not published (result {r})");

            _ready = Subscribe("game.ready", OnReady);
            _tick = Subscribe("tick", data => OnTick(TickMs(data)));
            if (_ready.Result != ScoResult.Ok) return _ready.Result;
            if (_tick.Result != ScoResult.Ok) return _tick.Result;

            Info(HostVersion);
            Status("Hello from a C# plugin");
            return ScoResult.Ok;
        }

        // Nothing to free: the subscriptions, commands and service go with the plugin.
        public override void OnUnload() => Info($"{_ticks} ticks seen");

        private void OnReady()
        {
            // Capabilities depend on the game build. Ask before using a feature and say why it is off.
            if (Has("teleport")) Info("teleport is available on this game build");
            else Warn("teleport is not available on this game build");

            // Look the service up as another plugin would. Query when needed: a provider's table
            // goes away when it unloads.
            ScoResult r = Query("cs_hello.greeter", ServiceVersion(1, 0), out ServiceRef<GreeterV1> greeter);
            if (r == ScoResult.Ok && greeter.Covers(8))
            {
                byte* line = stackalloc byte[64];
                byte[] who = Encoding.UTF8.GetBytes("services\0");
                fixed (byte* w = who) greeter.Table->greet(w, line, 64);
                Info(Marshal.PtrToStringUTF8((nint)line) ?? "");
            }
            else
            {
                Warn("greeter service: " + (r == ScoResult.NotFound ? "not found" : "unavailable"));
            }
        }

        private void OnTick(uint now)
        {
            if (_ticks++ == 0) _firstMs = _lastReportMs = now;
            // About once a minute; unsigned subtraction survives the millisecond counter wrapping.
            if (now - _lastReportMs >= 60000)
            {
                Info($"{_ticks} ticks in {(now - _firstMs) / 1000} s");
                _lastReportMs = now;
            }
        }

        // The service function: plain C ABI, so no exception may leave it.
        [UnmanagedCallersOnly]
        private static int Greet(byte* who, byte* output, uint outSize)
        {
            try
            {
                string text = "Hello, " + (who == null ? "pilot" : Marshal.PtrToStringUTF8((nint)who));
                byte[] b = Encoding.UTF8.GetBytes(text);
                if (output == null || outSize == 0) return b.Length;
                int n = (int)Math.Min((uint)b.Length, outSize - 1);
                b.AsSpan(0, n).CopyTo(new Span<byte>(output, n));
                output[n] = 0;
                return n;
            }
            catch
            {
                return -1;
            }
        }
    }

    // The three exports. NativeAOT exports [UnmanagedCallersOnly(EntryPoint = ...)] methods of the
    // published assembly only, so they live here and forward to the SDK. "cs_hello" must equal
    // plugin.ini's id; it is also the command and service prefix.
    public static unsafe class Exports
    {
        [UnmanagedCallersOnly(EntryPoint = "sco_plugin_query")]
        public static ScoPluginInfo* Query() => PluginExports.Query("cs_hello", "1.0.0", "sco SDK example");

        [UnmanagedCallersOnly(EntryPoint = "sco_plugin_load")]
        public static ScoResult Load(ScoApi* api, void* self) => PluginExports.Load<Hello>(api, self, "cs_hello");

        [UnmanagedCallersOnly(EntryPoint = "sco_plugin_unload")]
        public static void Unload() => PluginExports.Unload();
    }
}
