// Ui.cs: the host service "sco.ui" 1.0 (sco_ui.h): tabs, overlays, badges and hotkeys.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>
    /// The host service "sco.ui" (docs/ui.md). The product draws tabs and overlays by calling the
    /// draw function you register, on the game thread, with its frame context (sc-offline: its
    /// ImGui context), valid only during the call. Registration and hotkeys work from any thread.
    /// Open once and keep it for the plugin's life. An exception from a draw function is logged
    /// and dropped.
    /// </summary>
    public sealed unsafe class Ui
    {
        private Plugin? _plugin;
        private ScoUiV1* _t;
        private readonly object _lock = new();
        private readonly Dictionary<string, nint> _tabs = new();
        private readonly Dictionary<string, nint> _overlays = new();

        public bool IsOpen => _t != null;

        /// <summary>Queries sco.ui 1.0. NotFound when the host offers no UI service.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(Abi.UiName, Abi.UiVersion1_0, out ServiceRef<ScoUiV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(72)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        private sealed class DrawEntry : Entry
        {
            public DrawEntry(Plugin p, string id, Action<nint> fn) : base(p) { Id = id; Fn = fn; }
            public string Id { get; }
            public Action<nint> Fn { get; }
        }

        /// <summary>Adds tab id ("&lt;plugin id&gt;.&lt;name&gt;") with title at order. draw(frame) runs
        /// on the game thread until RemoveTab or unload.</summary>
        public ScoResult AddTab(string id, string title, int order, Action<nint> draw)
        {
            ArgumentNullException.ThrowIfNull(id);
            ArgumentNullException.ThrowIfNull(draw);
            if (_t == null || _plugin == null || !_plugin.IsLoaded) return ScoResult.Unavailable;
            nint key = Registry.Add(new DrawEntry(_plugin, id, draw));
            ScoResult r;
            fixed (byte* i = Utf8.Z(id))
            fixed (byte* t = Utf8.Z(title))
                r = _t->register_tab(Self, i, t, order, &DrawTrampoline, (void*)key);
            if (r != ScoResult.Ok) { Registry.Remove(key); return r; }
            lock (_lock) _tabs[id] = key;
            return r;
        }

        /// <summary>Removes one of this plugin's tabs and its badge.</summary>
        public ScoResult RemoveTab(string id)
        {
            if (_t == null) return ScoResult.Unavailable;
            ScoResult r;
            fixed (byte* i = Utf8.Z(id)) r = _t->unregister_tab(Self, i);
            Forget(_tabs, id);
            return r;
        }

        /// <summary>Text beside the tab title (at most 15 bytes); null or "" clears it.</summary>
        public ScoResult SetBadge(string tabId, string? text)
        {
            if (_t == null) return ScoResult.Unavailable;
            fixed (byte* i = Utf8.Z(tabId))
            fixed (byte* t = Utf8.Z(text))
                return _t->set_badge(Self, i, t);
        }

        /// <summary>Adds overlay id; draw(frame) runs every frame until RemoveOverlay or unload.</summary>
        public ScoResult AddOverlay(string id, Action<nint> draw)
        {
            ArgumentNullException.ThrowIfNull(id);
            ArgumentNullException.ThrowIfNull(draw);
            if (_t == null || _plugin == null || !_plugin.IsLoaded) return ScoResult.Unavailable;
            nint key = Registry.Add(new DrawEntry(_plugin, id, draw));
            ScoResult r;
            fixed (byte* i = Utf8.Z(id)) r = _t->register_overlay(Self, i, &DrawTrampoline, (void*)key);
            if (r != ScoResult.Ok) { Registry.Remove(key); return r; }
            lock (_lock) _overlays[id] = key;
            return r;
        }

        public ScoResult RemoveOverlay(string id)
        {
            if (_t == null) return ScoResult.Unavailable;
            ScoResult r;
            fixed (byte* i = Utf8.Z(id)) r = _t->unregister_overlay(Self, i);
            Forget(_overlays, id);
            return r;
        }

        // After unregister the draw function is no longer called; a draw running on the game thread
        // right now holds its own reference to the entry, so nothing is freed under it.
        private void Forget(Dictionary<string, nint> map, string id)
        {
            lock (_lock)
            {
                if (!map.Remove(id, out nint key)) return;
                Registry.Remove(key);
            }
        }

        /// <summary>Binds chord ("ctrl+alt+h") to command with args (copied). BadArg when the chord
        /// is taken or reserved: LastError names the holder.</summary>
        public ScoResult BindHotkey(string chord, string command, params Arg[] args)
        {
            if (_t == null) return ScoResult.Unavailable;
            using var block = new ArgBlock(args ?? Array.Empty<Arg>());
            fixed (byte* c = Utf8.Z(chord))
            fixed (byte* n = Utf8.Z(command))
                return _t->bind_hotkey(Self, c, n, block.Args, block.Count);
        }

        public ScoResult UnbindHotkey(string chord)
        {
            if (_t == null) return ScoResult.Unavailable;
            fixed (byte* c = Utf8.Z(chord)) return _t->unbind_hotkey(Self, c);
        }

        /// <summary>The normalized chord ("Alt + Ctrl + Esc" -> "ctrl+alt+escape"). BadArg: not a chord.</summary>
        public ScoResult NormalizeChord(string chord, out string normalized)
        {
            normalized = "";
            if (_t == null) return ScoResult.Unavailable;
            byte[] c = Utf8.Z(chord)!;
            ScoResult r = Storage.Sized((buf, size) => { fixed (byte* cp = c) return _t->normalize_chord(cp, buf, size); }, out byte[] b);
            if (r == ScoResult.Ok) normalized = Utf8.FromBytes(b);
            return r;
        }

        /// <summary>The message of this plugin's last refused call ("" if none).</summary>
        public string LastError()
        {
            if (_t == null) return "";
            return Storage.Sized((buf, size) => _t->last_error(Self, buf, size), out byte[] b) == ScoResult.Ok ? Utf8.FromBytes(b) : "";
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static void DrawTrampoline(void* frame, void* ctx)
        {
            try
            {
                DrawEntry? e = Registry.Get<DrawEntry>(ctx);
                if (e == null) return;
                try { e.Fn((nint)frame); }
                catch (Exception ex) { e.Owner.ReportException("draw " + e.Id, ex); }
            }
            catch { /* nothing may escape into the host */ }
        }
    }
}
