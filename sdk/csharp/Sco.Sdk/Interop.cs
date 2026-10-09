// Interop.cs: the C ABI of sco_api.h 1.1, sco_storage.h 1.0, sco_ui.h 1.0 and sco_datacore.h 1.0,
// as blittable C# structs.
//
// Field names are the C names, so the layout test (Sco.Sdk.Tests) can check every offset that
// tests/abi_v1.c, tests/abi_storage.c, tests/abi_ui.c and tests/abi_datacore.c pin. Function pointers are
// delegate* unmanaged[Cdecl]: the headers declare no calling convention, and on x64 the platform
// default and cdecl are one convention. 64-bit only, like the headers.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System.Runtime.InteropServices;

namespace Sco.Sdk
{
    /// <summary>sco_result. 4 bytes; never pass Force32.</summary>
    public enum ScoResult : int
    {
        Ok = 0,
        Unavailable = 1,
        NotFound = 2,
        BadArg = 3,
        Crashed = 4,
        WrongThread = 5,
        TooMany = 6,
        Failed = 7,
        Force32 = 0x7fffffff,
    }

    /// <summary>sco_log_level.</summary>
    public enum ScoLogLevel : int
    {
        Info = 0,
        Warn = 1,
        Error = 2,
        Force32 = 0x7fffffff,
    }

    /// <summary>sco_arg_type.</summary>
    public enum ScoArgType : int
    {
        Int = 0,
        Float = 1,
        String = 2,
        Bool = 3,
        Force32 = 0x7fffffff,
    }

    /// <summary>sco_sql_type (sco_storage.h).</summary>
    public enum ScoSqlType : int
    {
        Null = 0,
        Int = 1,
        Float = 2,
        Text = 3,
        Blob = 4,
        Force32 = 0x7fffffff,
    }

    /// <summary>sco_dc_type (sco_datacore.h).</summary>
    public enum ScoDcType : int
    {
        Bool = 0,
        Int = 1,
        UInt = 2,
        Float = 3,
        String = 4,
        Guid = 5,
        Enum = 6,
        Null = 7,
        Instance = 8,
        Ref = 9,
        Force32 = 0x7fffffff,
    }

    /// <summary>The version numbers, names and limits the C headers define.</summary>
    public static class Abi
    {
        public const int ApiMajor = 1;                    // SCO_API_MAJOR
        public const int ApiMinor = 1;                    // SCO_API_MINOR

        public const string StorageName = "sco.storage";  // SCO_STORAGE_NAME
        public const uint StorageVersion1_0 = 0x00010000u;
        public const uint StorageMaxKey = 255u;
        public const uint StorageMaxValue = 1u << 20;
        public const uint StorageMaxCursors = 64u;

        public const string UiName = "sco.ui";            // SCO_UI_NAME
        public const uint UiVersion1_0 = 0x00010000u;
        public const uint UiMaxId = 63u;
        public const uint UiMaxTitle = 63u;
        public const uint UiMaxBadge = 15u;
        public const uint UiMaxChord = 31u;
        public const uint UiMaxHotkeyArgs = 16u;
        public const uint UiMaxArgString = 255u;

        public const string DataCoreName = "sco.datacore";        // SCO_DATACORE_NAME
        public const uint DataCoreVersion1_0 = 0x00010000u;
        public const string DataCoreAppliedEvent = "datacore.applied"; // SCO_DC_APPLIED_EVENT
        public const uint DcOpen = 1u;             // state()
        public const uint DcLoaded = 2u;
        public const uint DcNonAtomic = 0x1u;      // begin() flags
        public const uint DcQueued = 1u;           // sco_dc_report.state
        public const uint DcApplied = 2u;
        public const uint DcSkipped = 3u;
        public const uint DcRefused = 4u;
        public const uint DcOpPatch = 0xFFFFFFFFu; // sco_dc_report.op_index of the patch's own entry
        public const uint DcMaxOps = 65536u;
        public const uint DcMaxPatches = 64u;

        /// <summary>A service version: (major &lt;&lt; 16) | minor.</summary>
        public static uint ServiceVersion(ushort major, ushort minor) => ((uint)major << 16) | minor;
    }

    /// <summary>offsetof(sco_api, f) for the functions added after 1.0: check
    /// <c>api->size &gt; offset</c> before calling one (<see cref="Plugin.ApiCovers"/>).</summary>
    public static class ApiOffset
    {
        public const uint provide_service = 88;
        public const uint query_service = 96;
        public const uint release_service = 104;
        public const uint invoke_raw = 112;
        public const uint register_raw = 120;
    }
}

namespace Sco.Sdk.Interop
{
    /// <summary>The union in sco_arg: 8 bytes.</summary>
    [StructLayout(LayoutKind.Explicit, Size = 8)]
    public unsafe struct ScoArgValue
    {
        [FieldOffset(0)] public long i;
        [FieldOffset(0)] public double f;
        [FieldOffset(0)] public byte* s;
    }

    /// <summary>sco_arg: 16 bytes, frozen for major 1.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScoArg
    {
        public uint type;   // ScoArgType
        public uint _pad;
        public ScoArgValue v;
    }

    /// <summary>sco_arg_def: 24 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoArgDef
    {
        public byte* name;
        public uint type;   // ScoArgType
        public uint _pad;
        public byte* help;
    }

    /// <summary>sco_command: 72 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoCommand
    {
        public uint size;
        public uint _pad0;
        public byte* name;
        public byte* title;
        public byte* help;
        public byte* capability;
        public ScoArgDef* args;
        public uint nargs;
        public uint arg_def_size;
        public delegate* unmanaged[Cdecl]<ScoArg*, uint, void*, byte*, uint, ScoResult> fn;
        public void* ctx;
    }

    /// <summary>sco_api, the host's table: 128 bytes (88 on a 1.0 host).</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoApi
    {
        public uint size;
        public ushort major;
        public ushort minor;
        public delegate* unmanaged[Cdecl]<byte*> host_version;
        public delegate* unmanaged[Cdecl]<byte*, int> has;
        public delegate* unmanaged[Cdecl]<void*, delegate* unmanaged[Cdecl]<void*, void>, void*, ScoResult> run_on_game_thread;
        public delegate* unmanaged[Cdecl]<void*, byte*, delegate* unmanaged[Cdecl]<byte*, void*, void*, void>, void*, ScoResult> subscribe;
        public delegate* unmanaged[Cdecl]<void*, byte*, delegate* unmanaged[Cdecl]<byte*, void*, void*, void>, ScoResult> unsubscribe;
        public delegate* unmanaged[Cdecl]<void*, byte*, void> status;
        public delegate* unmanaged[Cdecl]<void*, ScoLogLevel, byte*, void> log;
        public delegate* unmanaged[Cdecl]<void*, ScoCommand*, ScoResult> register_command;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoArg*, uint, delegate* unmanaged[Cdecl]<ScoResult, byte*, void*, void>, void*, ScoResult> invoke;
        public delegate* unmanaged[Cdecl]<ScoCommand**, uint, uint> list_commands;
        // 1.1: check size > ApiOffset.<name> first.
        public delegate* unmanaged[Cdecl]<void*, byte*, uint, void*, ScoResult> provide_service;
        public delegate* unmanaged[Cdecl]<byte*, uint, void**, ScoResult> query_service;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoResult> release_service;
        public delegate* unmanaged[Cdecl]<void*, byte*, void*, uint, void*, uint*, ScoResult> invoke_raw;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, delegate* unmanaged[Cdecl]<void*, uint, void*, uint*, void*, ScoResult>, void*, ScoResult> register_raw;
    }

    /// <summary>sco_plugin_info, what sco_plugin_query returns: 32 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoPluginInfo
    {
        public uint size;
        public ushort api_major;
        public ushort api_minor;
        public byte* name;
        public byte* version;
        public byte* author;
    }

    /// <summary>The union in sco_sql_value: 8 bytes.</summary>
    [StructLayout(LayoutKind.Explicit, Size = 8)]
    public unsafe struct ScoSqlUnion
    {
        [FieldOffset(0)] public long i;
        [FieldOffset(0)] public double f;
        [FieldOffset(0)] public void* p;
    }

    /// <summary>sco_sql_value: 16 bytes, frozen for 1.x.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScoSqlValue
    {
        public uint type;   // ScoSqlType
        public uint size;
        public ScoSqlUnion v;
    }

    /// <summary>sco_storage_v1, the table of the host service "sco.storage": 128 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoStorageV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<void*, byte*, void*, uint, ScoResult> put;
        public delegate* unmanaged[Cdecl]<void*, byte*, void*, uint*, ScoResult> get;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoResult> del;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, byte*, uint*, ScoResult> next_key;
        public delegate* unmanaged[Cdecl]<void*, ScoResult> begin;
        public delegate* unmanaged[Cdecl]<void*, ScoResult> commit;
        public delegate* unmanaged[Cdecl]<void*, ScoResult> rollback;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoSqlValue*, uint, long*, ScoResult> exec;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoSqlValue*, uint, ulong*, ScoResult> query;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> step;
        public delegate* unmanaged[Cdecl]<void*, ulong, uint*, ScoResult> column_count;
        public delegate* unmanaged[Cdecl]<void*, ulong, uint, ScoSqlValue*, void*, uint*, ScoResult> column;
        public delegate* unmanaged[Cdecl]<void*, ulong, uint, byte*, uint*, ScoResult> column_name;
        public delegate* unmanaged[Cdecl]<void*, ulong, ScoResult> close;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>sco_ui_v1, the table of the host service "sco.ui": 80 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoUiV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, int, delegate* unmanaged[Cdecl]<void*, void*, void>, void*, ScoResult> register_tab;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoResult> unregister_tab;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, ScoResult> set_badge;
        public delegate* unmanaged[Cdecl]<void*, byte*, delegate* unmanaged[Cdecl]<void*, void*, void>, void*, ScoResult> register_overlay;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoResult> unregister_overlay;
        public delegate* unmanaged[Cdecl]<void*, byte*, byte*, ScoArg*, uint, ScoResult> bind_hotkey;
        public delegate* unmanaged[Cdecl]<void*, byte*, ScoResult> unbind_hotkey;
        public delegate* unmanaged[Cdecl]<byte*, byte*, uint*, ScoResult> normalize_chord;
        public delegate* unmanaged[Cdecl]<void*, byte*, uint*, ScoResult> last_error;
    }

    /// <summary>sco_dc_value: 40 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoDcValue
    {
        public uint size;
        public ScoDcType type;
        public long i;
        public ulong u;
        public double f;
        public byte* s;
    }

    /// <summary>sco_dc_report: 204 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoDcReport
    {
        public uint size;
        public uint state;
        public uint op_index;
        public fixed byte reason[192];
    }

    /// <summary>sco_dc_applied, the data of the event datacore.applied: 16 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public struct ScoDcApplied
    {
        public uint size;
        public uint applied;
        public uint skipped;
        public uint refused;
    }

    /// <summary>sco_datacore_v1, the table of the host service "sco.datacore": 88 bytes.</summary>
    [StructLayout(LayoutKind.Sequential)]
    public unsafe struct ScoDatacoreV1
    {
        public uint size;
        public uint _pad;
        public delegate* unmanaged[Cdecl]<uint> state;
        public delegate* unmanaged[Cdecl]<void*, uint, ulong*, ScoResult> begin;
        public delegate* unmanaged[Cdecl]<ulong, byte*, byte*, ScoDcValue*, ScoResult> set;
        public delegate* unmanaged[Cdecl]<ulong, byte*, byte*, byte*, ulong*, ScoResult> add_instance;
        public delegate* unmanaged[Cdecl]<ulong, byte*, byte*, ulong, ScoResult> set_pointer;
        public delegate* unmanaged[Cdecl]<ulong, byte*, byte*, ScoDcValue*, ScoResult> append;
        public delegate* unmanaged[Cdecl]<ulong, byte*, byte*, byte*, byte*, byte*, ulong*, ScoResult> add_record;
        public delegate* unmanaged[Cdecl]<ulong, ScoResult> commit;
        public delegate* unmanaged[Cdecl]<ulong, ScoResult> discard;
        public delegate* unmanaged[Cdecl]<ulong, uint, ScoDcReport*, ScoResult> report;
    }
}
