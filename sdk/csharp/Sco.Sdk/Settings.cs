// Settings.cs: the host service "sco.settings" 1.0 (sco_settings.h): the typed [settings] a plugin
// declares in plugin.ini, read-only.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>
    /// The host service "sco.settings" (docs/api-v1.md, docs/plugins.md). The plugin declares its
    /// settings in the [settings] section of plugin.ini; the player changes them in the product's
    /// menu; the plugin only reads. A read returns the declared default until the player changes
    /// it, and always a value the declaration allows. Open once and keep it for the plugin's life.
    /// Reads work from any thread. To hear about a change, subscribe to the event
    /// <see cref="Abi.SettingsChangedEvent"/> and decode its data with <see cref="TryReadChanged"/>.
    /// </summary>
    public sealed unsafe class Settings
    {
        private Plugin? _plugin;
        private ScoSettingsV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries sco.settings 1.0. NotFound when the host offers no such service.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(Abi.SettingsName, Abi.SettingsVersion1_0, out ServiceRef<ScoSettingsV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(40)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        // NotFound: this plugin declares no setting by that name. BadArg: it has another type.
        // The out value is the type's zero value on failure.

        public ScoResult GetBool(string name, out bool value)
        {
            value = false;
            if (_t == null) return ScoResult.Unavailable;
            int v = 0;
            ScoResult r;
            fixed (byte* n = Utf8.Z(name)) r = _t->get_bool(Self, n, &v);
            if (r == ScoResult.Ok) value = v != 0;
            return r;
        }

        public ScoResult GetInt(string name, out long value)
        {
            value = 0;
            if (_t == null) return ScoResult.Unavailable;
            long v = 0;
            ScoResult r;
            fixed (byte* n = Utf8.Z(name)) r = _t->get_int(Self, n, &v);
            if (r == ScoResult.Ok) value = v;
            return r;
        }

        public ScoResult GetFloat(string name, out double value)
        {
            value = 0;
            if (_t == null) return ScoResult.Unavailable;
            double v = 0;
            ScoResult r;
            fixed (byte* n = Utf8.Z(name)) r = _t->get_float(Self, n, &v);
            if (r == ScoResult.Ok) value = v;
            return r;
        }

        /// <summary>A string setting's text, or an enum setting's choice.</summary>
        public ScoResult GetString(string name, out string value)
        {
            value = "";
            if (_t == null) return ScoResult.Unavailable;
            byte[] n = Utf8.Z(name) ?? Array.Empty<byte>();
            ScoResult r = Storage.Sized((buf, size) => { fixed (byte* np = n) return _t->get_string(Self, np, buf, size); }, out byte[] b);
            if (r == ScoResult.Ok) value = Utf8.FromBytes(b);
            return r;
        }

        /// <summary>The value, or fallback when the call fails for any reason (the service missing included).</summary>
        public bool Bool(string name, bool fallback = false) => GetBool(name, out bool v) == ScoResult.Ok ? v : fallback;
        public long Int(string name, long fallback = 0) => GetInt(name, out long v) == ScoResult.Ok ? v : fallback;
        public double Float(string name, double fallback = 0) => GetFloat(name, out double v) == ScoResult.Ok ? v : fallback;
        public string String(string name, string fallback = "") => GetString(name, out string v) == ScoResult.Ok ? v : fallback;

        /// <summary>The message of this plugin's last failed call ("" if none).</summary>
        public string LastError()
        {
            if (_t == null) return "";
            return Storage.Sized((buf, size) => _t->last_error(Self, buf, size), out byte[] b) == ScoResult.Ok ? Utf8.FromBytes(b) : "";
        }

        /// <summary>Decodes the data of a "settings.changed" event (the nint a Subscribe handler gets):
        /// the plugin id and the setting name. False when data is null or a size this SDK doesn't cover.</summary>
        public static bool TryReadChanged(nint data, out string plugin, out string name)
        {
            plugin = name = "";
            if (data == 0) return false;
            ScoSettingsChanged* c = (ScoSettingsChanged*)data;
            if (c->size < sizeof(ScoSettingsChanged)) return false;
            plugin = Utf8.Read(c->plugin) ?? "";
            name = Utf8.Read(c->name) ?? "";
            return true;
        }
    }
}
