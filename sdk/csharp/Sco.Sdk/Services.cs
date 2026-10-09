// Services.cs: services (1.1: Provide, Release, Query, ServiceRef) and raw handlers (1.1:
// RegisterRaw, InvokeRaw) with the size handshake.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>A raw handler: bytes in, bytes out, on the game thread. Set written to the bytes
    /// written, or to the bytes needed and return TooMany. output is empty when the caller asks
    /// the size or wants no output.</summary>
    public delegate ScoResult RawHandler(ReadOnlySpan<byte> input, Span<byte> output, out uint written);

    /// <summary>A typed raw handler: exactly sizeof(TIn) bytes in, sizeof(TOut) bytes out.</summary>
    public delegate ScoResult RawHandler<TIn, TOut>(in TIn input, out TOut output) where TIn : unmanaged where TOut : unmanaged;

    /// <summary>A typed raw handler without input.</summary>
    public delegate ScoResult RawOutHandler<TOut>(out TOut output) where TOut : unmanaged;

    /// <summary>A service table found by Query. Empty unless Query answered Ok. A provider's table
    /// goes away when it unloads: query when needed and don't keep one across ticks (host
    /// services under "sco." may be kept for the plugin's life).</summary>
    public readonly unsafe struct ServiceRef<T> where T : unmanaged
    {
        internal ServiceRef(T* table) { Table = table; }

        /// <summary>The table, or null.</summary>
        public T* Table { get; }

        public bool IsValid => Table != null;

        /// <summary>The table's leading uint size, as the provider built it (0 when empty).</summary>
        public uint Size => Table == null ? 0 : *(uint*)Table;

        /// <summary>True when the provider's size covers the field at offset with fieldSize bytes
        /// (8 for a function pointer): the check before calling a function a later minor added.</summary>
        public bool Covers(uint offset, uint fieldSize = 8) => Table != null && (ulong)offset + fieldSize <= Size;
    }

    public abstract unsafe partial class Plugin
    {
        // ---- services ----------------------------------------------------------------------

        /// <summary>(major &lt;&lt; 16) | minor.</summary>
        public static uint ServiceVersion(ushort major, ushort minor) => Abi.ServiceVersion(major, minor);

        /// <summary>Publishes table under name ("&lt;id&gt;" or "&lt;id&gt;.&lt;name&gt;") with version. The
        /// table must start with a uint size of at least 4 (BadArg otherwise) and stay valid until
        /// Release or unload: allocate it once (NativeMemory) and keep it. Unavailable on a 1.0 host.</summary>
        public ScoResult Provide<T>(string name, uint version, T* table) where T : unmanaged
        {
            ArgumentNullException.ThrowIfNull(name);
            if (sizeof(T) < sizeof(uint) || table == null || *(uint*)table < sizeof(uint)) return ScoResult.BadArg;
            ScoApi* a = Api;
            if (a == null || !IsLoaded) return ScoResult.BadArg;
            if (!ApiCovers(ApiOffset.provide_service)) return ScoResult.Unavailable;
            fixed (byte* n = Utf8.Z(name)) return a->provide_service(SelfPtr, n, version, table);
        }

        /// <summary>Withdraws one of this plugin's services. NotFound: none by that name.</summary>
        public ScoResult Release(string name)
        {
            ArgumentNullException.ThrowIfNull(name);
            ScoApi* a = Api;
            if (a == null || !IsLoaded) return ScoResult.BadArg;
            if (!ApiCovers(ApiOffset.release_service)) return ScoResult.Unavailable;
            fixed (byte* n = Utf8.Z(name)) return a->release_service(SelfPtr, n);
        }

        /// <summary>Finds a service: Ok (same major as minVersion and at least as new), Unavailable
        /// (another major, older, or a 1.0 host), NotFound, or BadArg when the table found doesn't
        /// start with a size of at least 4. service is empty unless Ok.</summary>
        public ScoResult Query<T>(string name, uint minVersion, out ServiceRef<T> service) where T : unmanaged
        {
            ArgumentNullException.ThrowIfNull(name);
            service = default;
            ScoApi* a = Api;
            if (a == null) return ScoResult.BadArg;
            if (!ApiCovers(ApiOffset.query_service)) return ScoResult.Unavailable;
            void* table = null;
            ScoResult r;
            fixed (byte* n = Utf8.Z(name)) r = a->query_service(n, minVersion, &table);
            if (r != ScoResult.Ok) return r;
            if (table == null || *(uint*)table < sizeof(uint)) return ScoResult.BadArg;
            service = new ServiceRef<T>((T*)table);
            return ScoResult.Ok;
        }

        // ---- raw handlers ------------------------------------------------------------------

        private sealed class RawEntry : Entry
        {
            public RawEntry(Plugin p, string name, RawHandler fn) : base(p) { Name = name; Fn = fn; }
            public string Name { get; }
            public RawHandler Fn { get; }
        }

        /// <summary>Registers a byte handler under name ("&lt;id&gt;.&lt;name&gt;"), gated on capability
        /// (null: none). Any thread; the handler runs on the game thread until unload.</summary>
        public ScoResult RegisterRawBytes(string name, string? capability, RawHandler handler)
        {
            ArgumentNullException.ThrowIfNull(name);
            ArgumentNullException.ThrowIfNull(handler);
            ScoApi* a = Api;
            if (a == null || !IsLoaded) return ScoResult.BadArg;
            if (!ApiCovers(ApiOffset.register_raw)) return ScoResult.Unavailable;
            nint id = Registry.Add(new RawEntry(this, name, handler));
            ScoResult r;
            fixed (byte* n = Utf8.Z(name))
            fixed (byte* c = Utf8.Z(capability))
                r = a->register_raw(SelfPtr, n, c, &RawTrampoline, (void*)id);
            if (r != ScoResult.Ok) Registry.Remove(id);
            return r;
        }

        /// <summary>A typed raw handler. Input must be exactly sizeof(TIn) bytes (BadArg otherwise,
        /// the handler isn't called); a caller whose buffer is smaller than sizeof(TOut) gets
        /// TooMany and the size needed.</summary>
        public ScoResult RegisterRaw<TIn, TOut>(string name, string? capability, RawHandler<TIn, TOut> handler)
            where TIn : unmanaged where TOut : unmanaged
        {
            ArgumentNullException.ThrowIfNull(handler);
            return RegisterRawBytes(name, capability, (ReadOnlySpan<byte> input, Span<byte> output, out uint written) =>
            {
                written = 0;
                if (input.Length != sizeof(TIn)) return ScoResult.BadArg;
                if (output.Length < sizeof(TOut)) { written = (uint)sizeof(TOut); return ScoResult.TooMany; }
                TIn value = MemoryMarshal.Read<TIn>(input);
                ScoResult r = handler(in value, out TOut result);
                if (r == ScoResult.Ok)
                {
                    MemoryMarshal.Write(output, in result);
                    written = (uint)sizeof(TOut);
                }
                return r;
            });
        }

        /// <summary>A typed raw handler that takes no input (in_size must be 0).</summary>
        public ScoResult RegisterRaw<TOut>(string name, string? capability, RawOutHandler<TOut> handler) where TOut : unmanaged
        {
            ArgumentNullException.ThrowIfNull(handler);
            return RegisterRawBytes(name, capability, (ReadOnlySpan<byte> input, Span<byte> output, out uint written) =>
            {
                written = 0;
                if (input.Length != 0) return ScoResult.BadArg;
                if (output.Length < sizeof(TOut)) { written = (uint)sizeof(TOut); return ScoResult.TooMany; }
                ScoResult r = handler(out TOut result);
                if (r == ScoResult.Ok)
                {
                    MemoryMarshal.Write(output, in result);
                    written = (uint)sizeof(TOut);
                }
                return r;
            });
        }

        [UnmanagedCallersOnly(CallConvs = new[] { typeof(CallConvCdecl) })]
        private static ScoResult RawTrampoline(void* input, uint inSize, void* output, uint* inoutOutSize, void* ctx)
        {
            uint cap = 0;
            try
            {
                cap = inoutOutSize == null ? 0 : *inoutOutSize;
                if (inoutOutSize != null) *inoutOutSize = 0;
                RawEntry? e = Registry.Get<RawEntry>(ctx);
                if (e == null) return ScoResult.NotFound;
                var inSpan = input == null ? ReadOnlySpan<byte>.Empty : new ReadOnlySpan<byte>(input, (int)inSize);
                var outSpan = output == null ? Span<byte>.Empty : new Span<byte>(output, (int)cap);
                ScoResult r;
                uint written;
                try { r = e.Fn(inSpan, outSpan, out written); }
                catch (Exception ex)
                {
                    e.Owner.ReportException(e.Name, ex);
                    return ScoResult.Failed;
                }
                if (r == ScoResult.Ok && written > (uint)outSpan.Length)
                {
                    e.Owner.Error($"{e.Name}: the handler wrote {written} bytes into {outSpan.Length}");
                    return ScoResult.Failed;
                }
                if (inoutOutSize != null && (r == ScoResult.Ok || r == ScoResult.TooMany)) *inoutOutSize = written;
                return r;
            }
            catch
            {
                if (inoutOutSize != null) *inoutOutSize = 0;
                return ScoResult.Failed;
            }
        }

        /// <summary>Calls the raw handler under name now, on the game thread (WrongThread
        /// elsewhere). written: the bytes written, or with TooMany the bytes needed (pass an empty
        /// output to ask the size).</summary>
        public ScoResult InvokeRawBytes(string name, ReadOnlySpan<byte> input, Span<byte> output, out uint written)
        {
            ArgumentNullException.ThrowIfNull(name);
            written = 0;
            ScoApi* a = Api;
            if (a == null || !IsLoaded) return ScoResult.BadArg;
            if (!ApiCovers(ApiOffset.invoke_raw)) return ScoResult.Unavailable;
            uint size = (uint)output.Length;
            ScoResult r;
            fixed (byte* n = Utf8.Z(name))
            fixed (byte* i = input)
            fixed (byte* o = output)
                r = a->invoke_raw(SelfPtr, n, i, (uint)input.Length, o, &size);
            written = size;
            return r;
        }

        /// <summary>Calls a typed raw handler. output is written only on Ok; BadArg when the handler
        /// wrote a size other than sizeof(TOut) (the two sides disagree about the struct).</summary>
        public ScoResult InvokeRaw<TIn, TOut>(string name, in TIn input, out TOut output)
            where TIn : unmanaged where TOut : unmanaged
        {
            TIn copy = input;
            return InvokeTyped(name, MemoryMarshal.AsBytes(MemoryMarshal.CreateReadOnlySpan(ref copy, 1)), out output);
        }

        /// <summary>Calls a typed raw handler that takes no input.</summary>
        public ScoResult InvokeRaw<TOut>(string name, out TOut output) where TOut : unmanaged =>
            InvokeTyped(name, ReadOnlySpan<byte>.Empty, out output);

        private ScoResult InvokeTyped<TOut>(string name, ReadOnlySpan<byte> input, out TOut output) where TOut : unmanaged
        {
            TOut result = default;
            ScoResult r = InvokeRawBytes(name, input, MemoryMarshal.AsBytes(MemoryMarshal.CreateSpan(ref result, 1)), out uint written);
            if (r == ScoResult.Ok && written != (uint)sizeof(TOut)) r = ScoResult.BadArg;
            output = r == ScoResult.Ok ? result : default;
            return r;
        }
    }
}
