// Storage.cs: the host service "sco.storage" 1.0 (sco_storage.h): per-plugin key-value and SQL.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>A bound SQL parameter.</summary>
    public readonly struct SqlParam
    {
        private SqlParam(ScoSqlType type, long i, double f, byte[]? bytes)
        {
            Type = type; IntValue = i; FloatValue = f; Bytes = bytes;
        }

        public ScoSqlType Type { get; }
        public long IntValue { get; }
        public double FloatValue { get; }
        public byte[]? Bytes { get; }

        public static SqlParam Null => new(ScoSqlType.Null, 0, 0, null);
        public static SqlParam Int(long v) => new(ScoSqlType.Int, v, 0, null);
        public static SqlParam Float(double v) => new(ScoSqlType.Float, 0, v, null);
        public static SqlParam Text(string v) => new(ScoSqlType.Text, 0, 0, System.Text.Encoding.UTF8.GetBytes(v ?? ""));
        public static SqlParam Blob(byte[] v) => new(ScoSqlType.Blob, 0, 0, v ?? Array.Empty<byte>());

        public static implicit operator SqlParam(long v) => Int(v);
        public static implicit operator SqlParam(int v) => Int(v);
        public static implicit operator SqlParam(double v) => Float(v);
        public static implicit operator SqlParam(string v) => Text(v);
        public static implicit operator SqlParam(byte[] v) => Blob(v);
    }

    internal unsafe delegate ScoResult SizedCall(byte* buf, uint* size);

    /// <summary>
    /// The host service "sco.storage": key-value and SQL in the plugin's own SQLite database
    /// (docs/storage.md). Open once and keep it for the plugin's life: a host service outlives
    /// every plugin. Any thread; each plugin's calls run one at a time. Every call answers a
    /// ScoResult; <see cref="LastError"/> has the message of the last failure.
    /// </summary>
    public sealed unsafe class Storage
    {
        private Plugin? _plugin;
        private ScoStorageV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries sco.storage 1.0. NotFound when the host offers no storage;
        /// Unavailable on a 1.0 host.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(Abi.StorageName, Abi.StorageVersion1_0, out ServiceRef<ScoStorageV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(120)) return ScoResult.Unavailable;   // last_error, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        private void* Self => _plugin == null ? null : _plugin.SelfPtr;

        // ---- key-value ---------------------------------------------------------------------

        public ScoResult Put(string key, ReadOnlySpan<byte> value)
        {
            if (_t == null) return ScoResult.Unavailable;
            fixed (byte* k = Utf8.Z(key))
            fixed (byte* v = value)
                return _t->put(Self, k, v, (uint)value.Length);
        }

        /// <summary>Stores an unmanaged struct as its bytes.</summary>
        public ScoResult Put<T>(string key, in T value) where T : unmanaged
        {
            T copy = value;
            return Put(key, MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref copy, 1)));
        }

        /// <summary>Stores text as UTF-8 (no NUL).</summary>
        public ScoResult PutString(string key, string value) => Put(key, System.Text.Encoding.UTF8.GetBytes(value ?? ""));

        /// <summary>Reads a value of any size (the handshake is done here). NotFound: no such key.</summary>
        public ScoResult Get(string key, out byte[] value)
        {
            value = Array.Empty<byte>();
            if (_t == null) return ScoResult.Unavailable;
            byte[] k = Utf8.Z(key)!;
            return Sized((buf, size) => { fixed (byte* kp = k) return _t->get(Self, kp, buf, size); }, out value);
        }

        /// <summary>Reads a struct stored with Put&lt;T&gt;. BadArg when the stored size differs.</summary>
        public ScoResult Get<T>(string key, out T value) where T : unmanaged
        {
            value = default;
            ScoResult r = Get(key, out byte[] bytes);
            if (r != ScoResult.Ok) return r;
            if (bytes.Length != sizeof(T)) return ScoResult.BadArg;
            value = MemoryMarshal.Read<T>(bytes);
            return ScoResult.Ok;
        }

        public ScoResult GetString(string key, out string value)
        {
            ScoResult r = Get(key, out byte[] bytes);
            value = r == ScoResult.Ok ? System.Text.Encoding.UTF8.GetString(bytes) : "";
            return r;
        }

        /// <summary>NotFound: there was no such key.</summary>
        public ScoResult Delete(string key)
        {
            if (_t == null) return ScoResult.Unavailable;
            fixed (byte* k = Utf8.Z(key)) return _t->del(Self, k);
        }

        /// <summary>Every key starting with prefix (null or "": all), in byte order.</summary>
        public ScoResult Keys(string? prefix, List<string> keys)
        {
            ArgumentNullException.ThrowIfNull(keys);
            if (_t == null) return ScoResult.Unavailable;
            byte[]? p = Utf8.Z(prefix);
            byte[]? after = null;
            for (;;)
            {
                byte[]? a = after;
                ScoResult r = Sized((buf, size) =>
                {
                    fixed (byte* pp = p)
                    fixed (byte* ap = a)
                        return _t->next_key(Self, pp, ap, buf, size);
                }, out byte[] key);
                if (r == ScoResult.NotFound) return ScoResult.Ok;
                if (r != ScoResult.Ok) return r;
                keys.Add(Utf8.FromBytes(key));
                after = key;   // NUL-terminated already
            }
        }

        // ---- transactions ------------------------------------------------------------------

        public ScoResult Begin() => _t == null ? ScoResult.Unavailable : _t->begin(Self);
        public ScoResult Commit() => _t == null ? ScoResult.Unavailable : _t->commit(Self);
        public ScoResult Rollback() => _t == null ? ScoResult.Unavailable : _t->rollback(Self);

        // ---- SQL ---------------------------------------------------------------------------

        /// <summary>Runs one statement; changes: rows changed.</summary>
        public ScoResult Exec(string sql, out long changes, params SqlParam[] args)
        {
            changes = 0;
            if (_t == null) return ScoResult.Unavailable;
            long c = 0;
            ScoResult r;
            using (var block = new SqlBlock(args ?? Array.Empty<SqlParam>()))
            fixed (byte* s = Utf8.Z(sql))
                r = _t->exec(Self, s, block.Values, block.Count, &c);
            changes = c;
            return r;
        }

        public ScoResult Exec(string sql, params SqlParam[] args) => Exec(sql, out _, args);

        /// <summary>Opens a cursor before the first row; dispose it when done.</summary>
        public ScoResult Query(string sql, out StorageCursor cursor, params SqlParam[] args)
        {
            cursor = new StorageCursor(this, 0);
            if (_t == null) return ScoResult.Unavailable;
            ulong id = 0;
            ScoResult r;
            using (var block = new SqlBlock(args ?? Array.Empty<SqlParam>()))
            fixed (byte* s = Utf8.Z(sql))
                r = _t->query(Self, s, block.Values, block.Count, &id);
            if (r == ScoResult.Ok) cursor = new StorageCursor(this, id);
            return r;
        }

        /// <summary>The message of this plugin's last failed call ("" if none).</summary>
        public string LastError()
        {
            if (_t == null) return "";
            return Sized((buf, size) => _t->last_error(Self, buf, size), out byte[] b) == ScoResult.Ok ? Utf8.FromBytes(b) : "";
        }

        internal ScoStorageV1* Table => _t;
        internal void* SelfFor => Self;

        /// <summary>The size handshake: ask the size, allocate, read; retried if the value grew.</summary>
        internal static ScoResult Sized(SizedCall call, out byte[] data)
        {
            data = Array.Empty<byte>();
            uint size = 0;
            ScoResult r = call(null, &size);
            for (int tries = 0; r == ScoResult.TooMany && tries < 4; ++tries)
            {
                var buf = new byte[size];
                fixed (byte* p = buf) r = call(p, &size);
                if (r == ScoResult.Ok)
                {
                    data = size == buf.Length ? buf : buf.AsSpan(0, (int)size).ToArray();
                    return r;
                }
            }
            return r;
        }

        private sealed class SqlBlock : IDisposable
        {
            private readonly List<nint> _bytes = new();
            public ScoSqlValue* Values { get; }
            public uint Count { get; }

            public SqlBlock(SqlParam[] args)
            {
                Count = (uint)args.Length;
                Values = args.Length == 0 ? null : (ScoSqlValue*)NativeMemory.AllocZeroed((nuint)(args.Length * sizeof(ScoSqlValue)));
                for (int i = 0; i < args.Length; ++i)
                {
                    Values[i].type = (uint)args[i].Type;
                    switch (args[i].Type)
                    {
                        case ScoSqlType.Int: Values[i].v.i = args[i].IntValue; break;
                        case ScoSqlType.Float: Values[i].v.f = args[i].FloatValue; break;
                        case ScoSqlType.Text:
                        case ScoSqlType.Blob:
                        {
                            byte[] b = args[i].Bytes ?? Array.Empty<byte>();
                            Values[i].size = (uint)b.Length;
                            if (b.Length > 0)
                            {
                                void* p = NativeMemory.Alloc((nuint)b.Length);
                                b.AsSpan().CopyTo(new Span<byte>(p, b.Length));
                                _bytes.Add((nint)p);
                                Values[i].v.p = p;
                            }
                            break;
                        }
                    }
                }
            }

            public void Dispose()
            {
                foreach (nint p in _bytes) NativeMemory.Free((void*)p);
                _bytes.Clear();
                if (Values != null) NativeMemory.Free(Values);
            }
        }
    }

    /// <summary>A storage cursor; closes itself on Dispose.</summary>
    public sealed unsafe class StorageCursor : IDisposable
    {
        private readonly Storage _s;
        private ulong _id;

        internal StorageCursor(Storage s, ulong id) { _s = s; _id = id; }

        public bool IsOpen => _id != 0;

        /// <summary>Ok: a row is ready. NotFound: no more rows.</summary>
        public ScoResult Next() => _id == 0 ? ScoResult.NotFound : _s.Table->step(_s.SelfFor, _id);

        public int ColumnCount
        {
            get
            {
                uint n = 0;
                return _id != 0 && _s.Table->column_count(_s.SelfFor, _id, &n) == ScoResult.Ok ? (int)n : 0;
            }
        }

        private ScoResult Column(int i, out ScoSqlValue v)
        {
            ScoSqlValue value = default;
            ScoResult r = _id == 0 ? ScoResult.NotFound : _s.Table->column(_s.SelfFor, _id, (uint)i, &value, null, null);
            v = value;
            return r;
        }

        public ScoSqlType Type(int i) => Column(i, out ScoSqlValue v) == ScoResult.Ok ? (ScoSqlType)v.type : ScoSqlType.Null;
        public long Int(int i) => Column(i, out ScoSqlValue v) == ScoResult.Ok && v.type == (uint)ScoSqlType.Int ? v.v.i : 0;
        public double Float(int i) => Column(i, out ScoSqlValue v) == ScoResult.Ok && v.type == (uint)ScoSqlType.Float ? v.v.f : 0.0;

        /// <summary>A BLOB column's bytes (TEXT: its UTF-8 with the NUL; empty for other types).</summary>
        public byte[] Blob(int i)
        {
            if (_id == 0) return Array.Empty<byte>();
            return Storage.Sized((buf, size) => { ScoSqlValue v; return _s.Table->column(_s.SelfFor, _id, (uint)i, &v, buf, size); }, out byte[] b) == ScoResult.Ok
                ? b : Array.Empty<byte>();
        }

        public string Text(int i) => Utf8.FromBytes(Blob(i));

        public string ColumnName(int i)
        {
            if (_id == 0) return "";
            return Storage.Sized((buf, size) => _s.Table->column_name(_s.SelfFor, _id, (uint)i, buf, size), out byte[] b) == ScoResult.Ok
                ? Utf8.FromBytes(b) : "";
        }

        public void Dispose()
        {
            if (_id != 0 && _s.Table != null) _s.Table->close(_s.SelfFor, _id);
            _id = 0;
        }
    }
}
