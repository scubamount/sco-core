// Internal.cs: UTF-8 strings, the per-plugin native string arena, and the callback registry.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Runtime.InteropServices;
using System.Text;
using System.Threading;
using Sco.Sdk.Interop;

[assembly: System.Runtime.CompilerServices.InternalsVisibleTo("Sco.Sdk.Tests")]

namespace Sco.Sdk
{
    internal static unsafe class Utf8
    {
        /// <summary>UTF-8 bytes with a NUL at the end, for a call that copies or reads the string
        /// only while it runs (pin with fixed). null stays null.</summary>
        public static byte[]? Z(string? s)
        {
            if (s is null) return null;
            int n = Encoding.UTF8.GetByteCount(s);
            var b = new byte[n + 1];
            Encoding.UTF8.GetBytes(s, 0, s.Length, b, 0);
            return b;
        }

        /// <summary>A NUL-terminated UTF-8 string from native memory; null for NULL.</summary>
        public static string? Read(byte* p) => p == null ? null : Marshal.PtrToStringUTF8((nint)p);

        /// <summary>A string from bytes that may end in NULs (a sized reply).</summary>
        public static string FromBytes(ReadOnlySpan<byte> b)
        {
            int n = b.IndexOf((byte)0);
            return Encoding.UTF8.GetString(n < 0 ? b : b.Slice(0, n));
        }

        /// <summary>Writes text into buf (size bytes including the NUL), cut at a whole UTF-8
        /// character. Nothing happens for a NULL buffer or size 0.</summary>
        public static void Write(string text, byte* buf, uint size)
        {
            if (buf == null || size == 0) return;
            byte[] b = Encoding.UTF8.GetBytes(text);
            int n = (int)Math.Min((uint)b.Length, size - 1);
            if (n < b.Length)
                while (n > 0 && (b[n] & 0xC0) == 0x80) --n;   // don't split a character
            b.AsSpan(0, n).CopyTo(new Span<byte>(buf, n));
            buf[n] = 0;
        }
    }

    /// <summary>Native UTF-8 strings and arrays a plugin hands the host and keeps for its whole
    /// life (command names, arg defs). Freed together when the plugin unloads.</summary>
    internal sealed unsafe class NativeArena
    {
        private readonly object _lock = new();
        private readonly List<nint> _blocks = new();

        public byte* Add(string? s)
        {
            if (s is null) return null;
            int n = Encoding.UTF8.GetByteCount(s);
            byte* p = (byte*)Alloc((nuint)n + 1);
            fixed (char* c = s) Encoding.UTF8.GetBytes(c, s.Length, p, n);
            p[n] = 0;
            return p;
        }

        public void* Alloc(nuint bytes)
        {
            void* p = NativeMemory.AllocZeroed(bytes == 0 ? 1 : bytes);
            lock (_lock) _blocks.Add((nint)p);
            return p;
        }

        public void FreeAll()
        {
            lock (_lock)
            {
                foreach (nint p in _blocks) NativeMemory.Free((void*)p);
                _blocks.Clear();
            }
        }
    }

    /// <summary>Something the host may call back: a command, a task, an event slot, a raw
    /// handler, a done callback, a draw function. Its id is the ctx the host holds.</summary>
    internal abstract class Entry
    {
        protected Entry(Plugin owner) { Owner = owner; }
        public Plugin Owner { get; }
    }

    /// <summary>The callback registry. The ctx the SDK gives the host is an id into this table,
    /// never a GCHandle or a pointer: the trampoline looks the id up on every call, and unload
    /// removes the plugin's entries, so a call the host makes late finds nothing. Ids are never
    /// reused.</summary>
    internal static unsafe class Registry
    {
        private static readonly object Lock = new();
        private static readonly Dictionary<nint, Entry> Entries = new();
        private static long _next;

        public static nint Add(Entry e)
        {
            nint id = (nint)Interlocked.Increment(ref _next);
            lock (Lock) Entries[id] = e;
            return id;
        }

        public static T? Get<T>(void* ctx) where T : Entry
        {
            lock (Lock) return Entries.TryGetValue((nint)ctx, out Entry? e) ? e as T : null;
        }

        /// <summary>Removes and returns the entry (one-shot callbacks: tasks, done).</summary>
        public static T? Take<T>(void* ctx) where T : Entry
        {
            lock (Lock)
            {
                if (!Entries.TryGetValue((nint)ctx, out Entry? e) || e is not T t) return null;
                Entries.Remove((nint)ctx);
                return t;
            }
        }

        public static void Remove(nint id)
        {
            lock (Lock) Entries.Remove(id);
        }

        public static void RemoveOwner(Plugin owner)
        {
            lock (Lock)
            {
                var dead = new List<nint>();
                foreach (var kv in Entries) if (kv.Value.Owner == owner) dead.Add(kv.Key);
                foreach (nint id in dead) Entries.Remove(id);
            }
        }

        /// <summary>Entries alive now (tests).</summary>
        public static int Count { get { lock (Lock) return Entries.Count; } }
    }

    /// <summary>sco_arg values in native memory for one call; the host copies what it keeps.</summary>
    internal sealed unsafe class ArgBlock : IDisposable
    {
        private readonly List<nint> _strings = new();
        public ScoArg* Args { get; }
        public uint Count { get; }

        public ArgBlock(ReadOnlySpan<Arg> args)
        {
            Count = (uint)args.Length;
            Args = args.Length == 0 ? null : (ScoArg*)NativeMemory.AllocZeroed((nuint)(args.Length * sizeof(ScoArg)));
            for (int i = 0; i < args.Length; ++i)
            {
                Args[i].type = (uint)args[i].Type;
                switch (args[i].Type)
                {
                    case ScoArgType.Float: Args[i].v.f = args[i].FloatValue; break;
                    case ScoArgType.String:
                    {
                        byte[] b = Utf8.Z(args[i].StringValue ?? "")!;
                        byte* p = (byte*)NativeMemory.Alloc((nuint)b.Length);
                        b.AsSpan().CopyTo(new Span<byte>(p, b.Length));
                        _strings.Add((nint)p);
                        Args[i].v.s = p;
                        break;
                    }
                    default: Args[i].v.i = args[i].IntValue; break;
                }
            }
        }

        public void Dispose()
        {
            foreach (nint p in _strings) NativeMemory.Free((void*)p);
            _strings.Clear();
            if (Args != null) NativeMemory.Free(Args);
        }
    }
}
