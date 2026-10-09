// DataCore.cs: the host service "sco.datacore" 1.0 (sco_datacore.h): DataCore overrides from code.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using Sco.Sdk.Interop;

namespace Sco.Sdk
{
    /// <summary>One sco_dc_value: the value of a set or an append.</summary>
    public readonly struct DcValue
    {
        private DcValue(ScoDcType type, long i, ulong u, double f, string? s)
        {
            Type = type; IntValue = i; UIntValue = u; FloatValue = f; Text = s;
        }

        public ScoDcType Type { get; }
        public long IntValue { get; }
        public ulong UIntValue { get; }
        public double FloatValue { get; }
        public string? Text { get; }

        public static DcValue Bool(bool v) => new(ScoDcType.Bool, v ? 1 : 0, 0, 0, null);
        public static DcValue Int(long v) => new(ScoDcType.Int, v, 0, 0, null);
        public static DcValue UInt(ulong v) => new(ScoDcType.UInt, 0, v, 0, null);
        public static DcValue Float(double v) => new(ScoDcType.Float, 0, 0, v, null);
        public static DcValue String(string v) => new(ScoDcType.String, 0, 0, 0, v ?? "");
        /// <summary>"xxxxxxxx-xxxx-xxxx-xxxx-xxxxxxxxxxxx".</summary>
        public static DcValue Guid(string v) => new(ScoDcType.Guid, 0, 0, 0, v ?? "");
        /// <summary>An enum field's option name.</summary>
        public static DcValue Enum(string option) => new(ScoDcType.Enum, 0, 0, 0, option ?? "");
        /// <summary>A null pointer.</summary>
        public static DcValue Null => new(ScoDcType.Null, 0, 0, 0, null);
        /// <summary>An instance AddInstance returned for the same patch.</summary>
        public static DcValue Instance(ulong id) => new(ScoDcType.Instance, 0, id, 0, null);
        /// <summary>A reference field's target record: a name or "guid:xxxxxxxx-...".</summary>
        public static DcValue Ref(string record) => new(ScoDcType.Ref, 0, 0, 0, record ?? "");

        public static implicit operator DcValue(bool v) => Bool(v);
        public static implicit operator DcValue(int v) => Int(v);
        public static implicit operator DcValue(long v) => Int(v);
        public static implicit operator DcValue(uint v) => UInt(v);
        public static implicit operator DcValue(ulong v) => UInt(v);
        public static implicit operator DcValue(double v) => Float(v);
        public static implicit operator DcValue(string v) => String(v);
    }

    /// <summary>One entry of a patch's report (sco_dc_report).</summary>
    public readonly record struct DcReport(uint State, uint OpIndex, string Reason)
    {
        /// <summary>True for the entry about the patch itself (op_index SCO_DC_OP_PATCH).</summary>
        public bool IsPatch => OpIndex == Abi.DcOpPatch;
    }

    /// <summary>
    /// The host service "sco.datacore" (docs/datacore.md, "The sco.datacore service"). The product
    /// must publish it: Open answers NotFound otherwise. Open once and keep it for the plugin's
    /// life. Any thread. After the game's DataCore load (the usual case) a committed patch is saved
    /// and applies from the next launch.
    /// </summary>
    public sealed unsafe class DataCore
    {
        private Plugin? _plugin;
        private ScoDatacoreV1* _t;

        public bool IsOpen => _t != null;

        /// <summary>Queries sco.datacore 1.0. NotFound when the host doesn't publish it.</summary>
        public ScoResult Open(Plugin plugin)
        {
            ArgumentNullException.ThrowIfNull(plugin);
            ScoResult r = plugin.Query(Abi.DataCoreName, Abi.DataCoreVersion1_0, out ServiceRef<ScoDatacoreV1> s);
            if (r != ScoResult.Ok) return r;
            if (!s.Covers(80)) return ScoResult.Unavailable;   // report, the last 1.0 function
            _plugin = plugin;
            _t = s.Table;
            return ScoResult.Ok;
        }

        /// <summary>Abi.DcOpen (before the load) or Abi.DcLoaded (after it); 0 when not open.</summary>
        public uint State => _t == null ? 0 : _t->state();

        /// <summary>A new patch owned by this plugin. flags: 0 or Abi.DcNonAtomic. TooMany: 64
        /// patches are open or queued. patch is empty unless Ok.</summary>
        public ScoResult Begin(out DataCorePatch patch, uint flags = 0)
        {
            patch = new DataCorePatch(this, 0);
            if (_t == null || _plugin == null) return ScoResult.Unavailable;
            ulong id = 0;
            ScoResult r = _t->begin(_plugin.SelfPtr, flags, &id);
            if (r == ScoResult.Ok) patch = new DataCorePatch(this, id);
            return r;
        }

        internal ScoDatacoreV1* Table => _t;
    }

    /// <summary>
    /// One DataCore patch. Dispose discards it unless it was committed; keep it after Commit to read
    /// its report. Every call answers a ScoResult (NotFound for an empty or discarded patch).
    /// </summary>
    public sealed unsafe class DataCorePatch : IDisposable
    {
        private readonly DataCore _dc;
        private bool _committed;

        internal DataCorePatch(DataCore dc, ulong id) { _dc = dc; Id = id; }

        /// <summary>The patch id; 0 when empty.</summary>
        public ulong Id { get; private set; }

        public bool IsValid => Id != 0;

        private ScoDatacoreV1* T => Id == 0 ? null : _dc.Table;

        /// <summary>Sets a field in place. record: a name or "guid:...". field: a path, name /
        /// name[3] / name[Type] joined with '.'.</summary>
        public ScoResult Set(string record, string field, DcValue value)
        {
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            using var v = new NativeDcValue(value);
            fixed (byte* r = Utf8.Z(record))
            fixed (byte* f = Utf8.Z(field))
                return t->set(Id, r, f, v.Ptr);
        }

        /// <summary>A new instance of struct type, cloned from cloneRecord's cloneField (null or "":
        /// the record's root) or zero-filled (cloneRecord null). instance: its id.</summary>
        public ScoResult AddInstance(string type, string? cloneRecord, string? cloneField, out ulong instance)
        {
            instance = 0;
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            ulong id = 0;
            ScoResult r;
            fixed (byte* ty = Utf8.Z(type))
            fixed (byte* cr = Utf8.Z(cloneRecord))
            fixed (byte* cf = Utf8.Z(cloneField))
                r = t->add_instance(Id, ty, cr, cf, &id);
            instance = id;
            return r;
        }

        /// <summary>Points a strong or weak pointer field at an added instance.</summary>
        public ScoResult SetPointer(string record, string field, ulong instance)
        {
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            fixed (byte* r = Utf8.Z(record))
            fixed (byte* f = Utf8.Z(field))
                return t->set_pointer(Id, r, f, instance);
        }

        /// <summary>Appends one element to an array field.</summary>
        public ScoResult Append(string record, string field, DcValue value)
        {
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            using var v = new NativeDcValue(value);
            fixed (byte* r = Utf8.Z(record))
            fixed (byte* f = Utf8.Z(field))
                return t->append(Id, r, f, v.Ptr);
        }

        /// <summary>A new record (sco.datacore 1.1: query with Abi.DataCoreVersion1_1 to require it). A 1.0 host answers Unavailable.</summary>
        public ScoResult AddRecord(string type, string name, string? guid, string cloneRecord, string? filePath, out ulong record)
        {
            record = 0;
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            ulong id = 0;
            ScoResult r;
            fixed (byte* ty = Utf8.Z(type))
            fixed (byte* n = Utf8.Z(name))
            fixed (byte* g = Utf8.Z(guid))
            fixed (byte* c = Utf8.Z(cloneRecord))
            fixed (byte* p = Utf8.Z(filePath))
                r = t->add_record(Id, ty, n, g, c, p, &id);
            record = id;
            return r;
        }

        /// <summary>Queues the patch for the load, or after it saves it for the next launch. No more
        /// operations either way. Failed: the file couldn't be written (commit again).</summary>
        public ScoResult Commit()
        {
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            ScoResult r = t->commit(Id);
            if (r == ScoResult.Ok) _committed = true;
            return r;
        }

        /// <summary>Drops the patch (see sco_datacore.h discard) and empties this object.</summary>
        public ScoResult Discard()
        {
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            ScoResult r = t->discard(Id);
            Id = 0;
            return r;
        }

        /// <summary>Report entry index: 0 .. operations-1, then the patch's own entry. NotFound past
        /// the last one.</summary>
        public ScoResult Report(uint index, out DcReport entry)
        {
            entry = default;
            ScoDatacoreV1* t = T;
            if (t == null) return ScoResult.NotFound;
            ScoDcReport rep = default;
            rep.size = (uint)sizeof(ScoDcReport);
            ScoResult r = t->report(Id, index, &rep);
            if (r == ScoResult.Ok)
                entry = new DcReport(rep.state, rep.op_index, Utf8.FromBytes(new ReadOnlySpan<byte>(rep.reason, 192)));
            return r;
        }

        /// <summary>Every report entry, the patch's own last.</summary>
        public List<DcReport> Reports()
        {
            var list = new List<DcReport>();
            for (uint i = 0; Report(i, out DcReport e) == ScoResult.Ok; ++i) list.Add(e);
            return list;
        }

        /// <summary>Discards the patch unless it was committed.</summary>
        public void Dispose()
        {
            if (!_committed && Id != 0) Discard();
            Id = 0;
        }

        // A sco_dc_value with its string in native memory, for one call (the host copies it).
        private readonly struct NativeDcValue : IDisposable
        {
            public ScoDcValue* Ptr { get; }

            public NativeDcValue(DcValue v)
            {
                Ptr = (ScoDcValue*)System.Runtime.InteropServices.NativeMemory.AllocZeroed((nuint)sizeof(ScoDcValue));
                Ptr->size = (uint)sizeof(ScoDcValue);
                Ptr->type = v.Type;
                Ptr->i = v.IntValue;
                Ptr->u = v.UIntValue;
                Ptr->f = v.FloatValue;
                if (v.Text != null)
                {
                    byte[] b = Utf8.Z(v.Text)!;
                    byte* s = (byte*)System.Runtime.InteropServices.NativeMemory.Alloc((nuint)b.Length);
                    b.AsSpan().CopyTo(new Span<byte>(s, b.Length));
                    Ptr->s = s;
                }
            }

            public void Dispose()
            {
                if (Ptr->s != null) System.Runtime.InteropServices.NativeMemory.Free(Ptr->s);
                System.Runtime.InteropServices.NativeMemory.Free(Ptr);
            }
        }
    }
}
