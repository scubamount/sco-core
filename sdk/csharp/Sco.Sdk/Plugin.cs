// Plugin.cs: the Plugin base class, the three exports, logging, tasks, events and invoke.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>One command argument value (sco_arg), for Invoke, InvokeAsync and BindHotkey.</summary>
    public readonly struct Arg
    {
        private Arg(ScoArgType type, long i, double f, string? s)
        {
            Type = type; IntValue = i; FloatValue = f; StringValue = s;
        }

        public ScoArgType Type { get; }
        public long IntValue { get; }
        public double FloatValue { get; }
        public string? StringValue { get; }

        public static Arg Int(long v) => new(ScoArgType.Int, v, 0, null);
        public static Arg Float(double v) => new(ScoArgType.Float, 0, v, null);
        public static Arg Text(string v) => new(ScoArgType.String, 0, 0, v ?? "");
        public static Arg Bool(bool v) => new(ScoArgType.Bool, v ? 1 : 0, 0, null);

        public static implicit operator Arg(long v) => Int(v);
        public static implicit operator Arg(int v) => Int(v);
        public static implicit operator Arg(double v) => Float(v);
        public static implicit operator Arg(string v) => Text(v);
        public static implicit operator Arg(bool v) => Bool(v);
    }

    /// <summary>
    /// A plugin. Derive from it, override <see cref="OnLoad"/>, and export the three entry points
    /// with <see cref="PluginExports"/>. Everything here is a wrapper over sco_api.h: the API v1
    /// rules (threads, reply buffers, done callbacks, services, raw handlers) still apply.
    /// </summary>
    public abstract unsafe partial class Plugin
    {
        internal readonly object Lock = new();
        internal readonly NativeArena Arena = new();
        private readonly Dictionary<string, EventSlot> _slots = new();
        private ScoApi* _api;
        private void* _self;
        private volatile bool _loaded;

        /// <summary>The plugin id ("hello"): plugin.ini's id and the command prefix.</summary>
        public string Id { get; private set; } = "";

        /// <summary>The host's table; valid from OnLoad until OnUnload returns, else null.</summary>
        public ScoApi* Api => _api;

        /// <summary>This plugin's handle (sco_plugin*); 0 when not loaded.</summary>
        public nint Self => (nint)_self;

        /// <summary>True between load and unload.</summary>
        public bool IsLoaded => _loaded;

        internal void* SelfPtr => _self;

        /// <summary>Called from sco_plugin_load, on the game thread. Anything but Ok unloads the
        /// plugin; OnUnload is not called, but the SDK still releases what was registered.</summary>
        public virtual ScoResult OnLoad() => ScoResult.Ok;

        /// <summary>Called from sco_plugin_unload, on the game thread, before the SDK releases its
        /// state. Stop your own threads here.</summary>
        public virtual void OnUnload() { }

        /// <summary>True when the host's table has the function at this offset (the api->size check):
        /// <c>ApiCovers(ApiOffset.query_service)</c>.</summary>
        public bool ApiCovers(uint offset)
        {
            ScoApi* a = _api;
            return a != null && a->size > offset;
        }

        // ---- logging -----------------------------------------------------------------------

        /// <summary>The host's name and version ("sc-offline 0.8.0").</summary>
        public string HostVersion
        {
            get
            {
                ScoApi* a = _api;
                return a == null ? "" : Utf8.Read(a->host_version()) ?? "";
            }
        }

        /// <summary>Writes "[id] message" to the host's log. Any thread.</summary>
        public void Log(ScoLogLevel level, string message) => LogRaw(_api, _self, level, message);

        public void Info(string message) => Log(ScoLogLevel.Info, message);
        public void Warn(string message) => Log(ScoLogLevel.Warn, message);
        public void Error(string message) => Log(ScoLogLevel.Error, message);

        /// <summary>Shows "id: message" on the status line. Any thread.</summary>
        public void Status(string message)
        {
            ScoApi* a = _api;
            if (a == null) return;
            fixed (byte* m = Utf8.Z(message)) a->status(_self, m);
        }

        /// <summary>Logs "exception in where: Type: message" as an error. Never throws.</summary>
        public void ReportException(string where, Exception e)
        {
            try { Error($"exception in {where}: {e.GetType().Name}: {e.Message}"); }
            catch { /* nothing may escape into the host */ }
        }

        internal static void LogRaw(ScoApi* api, void* self, ScoLogLevel level, string message)
        {
            if (api == null || self == null) return;
            fixed (byte* m = Utf8.Z(message)) api->log(self, level, m);
        }

        /// <summary>api->has(capability) != 0.</summary>
        public bool Has(string capability)
        {
            ScoApi* a = _api;
            if (a == null) return false;
            fixed (byte* c = Utf8.Z(capability)) return a->has(c) != 0;
        }

        // ---- tasks -------------------------------------------------------------------------

        private sealed class TaskEntry : Entry
        {
            public TaskEntry(Plugin p, Action fn) : base(p) { Fn = fn; }
            public Action Fn { get; }
        }

        /// <summary>Queues task to run once on the game thread, at the next tick. Any thread. A task
        /// still queued at unload is dropped and never runs.</summary>
        public ScoResult RunOnGameThread(Action task)
        {
            ArgumentNullException.ThrowIfNull(task);
            ScoApi* a = _api;
            if (a == null || !_loaded) return ScoResult.BadArg;
            nint id = Registry.Add(new TaskEntry(this, task));
            ScoResult r = a->run_on_game_thread(_self, &TaskTrampoline, (void*)id);
            if (r != ScoResult.Ok) Registry.Remove(id);
            return r;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void TaskTrampoline(void* ctx)
        {
            try
            {
                TaskEntry? e = Registry.Take<TaskEntry>(ctx);
                if (e == null) return;
                try { e.Fn(); }
                catch (Exception ex) { e.Owner.ReportException("task", ex); }
            }
            catch { /* nothing may escape into the host */ }
        }

        // ---- events ------------------------------------------------------------------------

        internal sealed class Handler
        {
            public Handler(Action<nint> fn) { Fn = fn; }
            public Action<nint> Fn { get; }
            public volatile bool Alive = true;
        }

        // One host subscription per event; the SDK multiplexes handlers over it (the host keys a
        // subscription by plugin, event and function, so it can't hold two for one event).
        internal sealed class EventSlot : Entry
        {
            public EventSlot(Plugin p, string ev) : base(p) { Event = ev; }
            public string Event { get; }
            public nint Id;
            public volatile Handler[] Handlers = Array.Empty<Handler>();
        }

        /// <summary>Calls handler(data) for every dispatch of ev, from the next dispatch on, until
        /// the returned Subscription is disposed or the plugin unloads. data is the event's data
        /// pointer (tick: a uint* of milliseconds, see <see cref="TickMs"/>), valid only during the
        /// call. Any thread; handlers run on the game thread.</summary>
        public Subscription Subscribe(string ev, Action<nint> handler)
        {
            ArgumentNullException.ThrowIfNull(ev);
            ArgumentNullException.ThrowIfNull(handler);
            lock (Lock)
            {
                if (!_loaded || _api == null) return new Subscription(ScoResult.BadArg);
                if (!_slots.TryGetValue(ev, out EventSlot? slot))
                {
                    slot = new EventSlot(this, ev);
                    slot.Id = Registry.Add(slot);
                    ScoResult r;
                    fixed (byte* n = Utf8.Z(ev)) r = _api->subscribe(_self, n, EventFn, (void*)slot.Id);
                    if (r != ScoResult.Ok)
                    {
                        Registry.Remove(slot.Id);
                        return new Subscription(r);
                    }
                    _slots[ev] = slot;
                }
                var h = new Handler(handler);
                var list = new Handler[slot.Handlers.Length + 1];
                slot.Handlers.CopyTo(list, 0);
                list[^1] = h;
                slot.Handlers = list;
                return new Subscription(this, slot, h);
            }
        }

        /// <summary>Subscribe for handlers that don't read the event's data.</summary>
        public Subscription Subscribe(string ev, Action handler)
        {
            ArgumentNullException.ThrowIfNull(handler);
            return Subscribe(ev, _ => handler());
        }

        /// <summary>The milliseconds a tick event carries (0 for a NULL data pointer).</summary>
        public static uint TickMs(nint data) => data == 0 ? 0 : *(uint*)data;

        internal void Unsubscribe(EventSlot slot, Handler h)
        {
            h.Alive = false;
            lock (Lock)
            {
                if (!_loaded || _api == null) return;   // unload already took everything down
                Handler[] old = slot.Handlers;
                int i = Array.IndexOf(old, h);
                if (i < 0) return;
                var list = new Handler[old.Length - 1];
                Array.Copy(old, 0, list, 0, i);
                Array.Copy(old, i + 1, list, i, old.Length - i - 1);
                slot.Handlers = list;
                if (list.Length > 0 || !_slots.TryGetValue(slot.Event, out EventSlot? cur) || cur != slot) return;
                _slots.Remove(slot.Event);
                fixed (byte* n = Utf8.Z(slot.Event)) _api->unsubscribe(_self, n, EventFn);
                // A dispatch running on the game thread now finds no entry and calls nothing more.
                Registry.Remove(slot.Id);
            }
        }

        // The host matches unsubscribe by function pointer, and C# doesn't promise that two &method
        // expressions give the same address: take it once and pass that to both calls.
        private static readonly delegate* unmanaged[Cdecl]<byte*, void*, void*, void> EventFn = &EventTrampoline;

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void EventTrampoline(byte* ev, void* data, void* ctx)
        {
            try
            {
                EventSlot? slot = Registry.Get<EventSlot>(ctx);
                if (slot == null) return;
                foreach (Handler h in slot.Handlers)
                {
                    if (!h.Alive) continue;
                    try { h.Fn((nint)data); }
                    catch (Exception ex) { slot.Owner.ReportException("event " + slot.Event, ex); }
                }
            }
            catch { /* nothing may escape into the host */ }
        }

        // ---- invoke ------------------------------------------------------------------------

        private sealed class DoneEntry : Entry
        {
            public DoneEntry(Plugin p, Action<ScoResult, string> fn) : base(p) { Fn = fn; }
            public Action<ScoResult, string> Fn { get; }
        }

        /// <summary>Runs a command. From the game thread it runs now and reply holds its answer;
        /// from another thread it is queued, returns Ok, and reply is "".</summary>
        public ScoResult Invoke(string name, out string reply, params Arg[] args)
        {
            string got = "";
            ScoResult r = InvokeAsync(name, (_, text) => got = text, args);
            reply = got;
            return r;
        }

        /// <summary>Runs a command and calls done(result, reply) once on the game thread. From the
        /// game thread that happens before InvokeAsync returns, with the same result. From another
        /// thread any return but Ok means done is never called. A call still queued when the plugin
        /// unloads is dropped and done is not called.</summary>
        public ScoResult InvokeAsync(string name, Action<ScoResult, string> done, params Arg[] args)
        {
            ArgumentNullException.ThrowIfNull(name);
            ArgumentNullException.ThrowIfNull(done);
            ScoApi* a = _api;
            if (a == null || !_loaded) return ScoResult.BadArg;
            nint id = Registry.Add(new DoneEntry(this, done));
            ScoResult r;
            using (var block = new ArgBlock(args ?? Array.Empty<Arg>()))
            fixed (byte* n = Utf8.Z(name))
                r = a->invoke(_self, n, block.Args, block.Count, &DoneTrampoline, (void*)id);
            if (r != ScoResult.Ok) Registry.Remove(id);   // already gone if done ran
            return r;
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void DoneTrampoline(ScoResult r, byte* reply, void* ctx)
        {
            try
            {
                DoneEntry? e = Registry.Take<DoneEntry>(ctx);
                if (e == null) return;
                try { e.Fn(r, Utf8.Read(reply) ?? ""); }
                catch (Exception ex) { e.Owner.ReportException("done callback", ex); }
            }
            catch { /* nothing may escape into the host */ }
        }

        // ---- load and unload ---------------------------------------------------------------

        internal ScoResult Attach(ScoApi* api, void* self, string id)
        {
            if (api == null || self == null || api->major != Abi.ApiMajor) return ScoResult.BadArg;
            lock (Lock)
            {
                if (_loaded) return ScoResult.BadArg;
                _api = api;
                _self = self;
                Id = id;
                _loaded = true;
            }
            return ScoResult.Ok;
        }

        /// <summary>Removes everything the SDK holds for this plugin: registry entries (so a late
        /// host call finds nothing), event slots and the native string arena.</summary>
        internal void Release()
        {
            lock (Lock)
            {
                _loaded = false;
                foreach (EventSlot s in _slots.Values)
                    foreach (Handler h in s.Handlers) h.Alive = false;
                _slots.Clear();
                Registry.RemoveOwner(this);
                Arena.FreeAll();
                _api = null;
                _self = null;
            }
        }
    }

    /// <summary>A handle to one event handler. Dispose (or Reset) removes it; on the game thread the
    /// handler never runs again once that returns. From another thread a call already running on
    /// the game thread may still finish. A handle alive at unload is detached and does nothing.</summary>
    public sealed class Subscription : IDisposable
    {
        private Plugin? _owner;
        private readonly Plugin.EventSlot? _slot;
        private readonly Plugin.Handler? _handler;

        internal Subscription(ScoResult failed) { Result = failed; }

        internal Subscription(Plugin owner, Plugin.EventSlot slot, Plugin.Handler h)
        {
            _owner = owner; _slot = slot; _handler = h; Result = ScoResult.Ok;
        }

        /// <summary>The host's answer to the subscribe.</summary>
        public ScoResult Result { get; }

        /// <summary>True while the handler is registered.</summary>
        public bool IsActive => _owner != null && _handler!.Alive;

        public void Reset() => Dispose();

        public void Dispose()
        {
            Plugin? owner = System.Threading.Interlocked.Exchange(ref _owner, null);
            if (owner != null) owner.Unsubscribe(_slot!, _handler!);
        }
    }

    /// <summary>
    /// The three exports. NativeAOT exports only methods of the published assembly, so a plugin
    /// declares them itself and forwards here:
    /// <code>
    /// [UnmanagedCallersOnly(EntryPoint = "sco_plugin_query")]
    /// public static ScoPluginInfo* Query() => PluginExports.Query("hello", "1.0.0", "you");
    /// [UnmanagedCallersOnly(EntryPoint = "sco_plugin_load")]
    /// public static ScoResult Load(ScoApi* api, void* self) => PluginExports.Load&lt;Hello&gt;(api, self, "hello");
    /// [UnmanagedCallersOnly(EntryPoint = "sco_plugin_unload")]
    /// public static void Unload() => PluginExports.Unload();
    /// </code>
    /// </summary>
    public static unsafe class PluginExports
    {
        private static readonly object Lock = new();
        private static ScoPluginInfo* _info;
        private static Plugin? _instance;

        /// <summary>The plugin's sco_plugin_info (built with this SDK's ApiMajor and ApiMinor). The
        /// block is allocated once and never freed: it must stay valid until the DLL unloads. null
        /// only when memory runs out.</summary>
        public static ScoPluginInfo* Query(string id, string version, string author)
        {
            try
            {
                lock (Lock)
                {
                    if (_info != null) return _info;
                    var info = (ScoPluginInfo*)NativeMemory.AllocZeroed((nuint)sizeof(ScoPluginInfo));
                    info->size = (uint)sizeof(ScoPluginInfo);
                    info->api_major = Abi.ApiMajor;
                    info->api_minor = Abi.ApiMinor;
                    info->name = (byte*)Marshal.StringToCoTaskMemUTF8(id);
                    info->version = (byte*)Marshal.StringToCoTaskMemUTF8(version);
                    info->author = (byte*)Marshal.StringToCoTaskMemUTF8(author);
                    _info = info;
                    return info;
                }
            }
            catch { return null; }
        }

        /// <summary>Constructs T and runs its OnLoad. An exception from either is logged and
        /// answered with Failed; the SDK then releases what OnLoad registered.</summary>
        public static ScoResult Load<T>(ScoApi* api, void* self, string id) where T : Plugin, new()
        {
            try
            {
                lock (Lock) if (_instance != null) return ScoResult.BadArg;
                T plugin;
                try { plugin = new T(); }
                catch (Exception ex)
                {
                    Plugin.LogRaw(api, self, ScoLogLevel.Error, $"exception in constructor: {ex.GetType().Name}: {ex.Message}");
                    return ScoResult.Failed;
                }
                ScoResult r = LoadPlugin(plugin, api, self, id);
                if (r == ScoResult.Ok) lock (Lock) _instance = plugin;
                return r;
            }
            catch { return ScoResult.Failed; }
        }

        /// <summary>Runs OnUnload and releases the SDK's state for the plugin Load made.</summary>
        public static void Unload()
        {
            try
            {
                Plugin? p;
                lock (Lock) { p = _instance; _instance = null; }
                if (p != null) UnloadPlugin(p);
            }
            catch { /* nothing may escape into the host */ }
        }

        /// <summary>Loads one Plugin object (several may live in one binary, as in tests).</summary>
        public static ScoResult LoadPlugin(Plugin plugin, ScoApi* api, void* self, string id)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Attach(api, self, id);
            if (r != ScoResult.Ok) return r;
            try { r = plugin.OnLoad(); }
            catch (Exception ex)
            {
                plugin.ReportException("OnLoad", ex);
                r = ScoResult.Failed;
            }
            if (r != ScoResult.Ok) plugin.Release();
            return r;
        }

        /// <summary>Runs OnUnload (exceptions logged), then releases everything.</summary>
        public static void UnloadPlugin(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            if (!plugin.IsLoaded) return;
            try { plugin.OnUnload(); }
            catch (Exception ex) { plugin.ReportException("OnUnload", ex); }
            plugin.Release();
        }
    }
}
