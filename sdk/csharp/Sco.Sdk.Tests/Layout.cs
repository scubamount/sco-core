// Layout.cs: checks every size, offset and constant the C headers pin against the C# structs.
//
// Each SIZE(T, n), AT(T, field, n) and PIN(...) line of tests/abi_*.c (Pins.cs) is parsed and checked with
// Unsafe.SizeOf / Marshal.SizeOf / Marshal.OffsetOf and the SDK's constants. A line it can't
// check is a failure, so a new pin in the C files needs the SDK (and this map) updated.
//
// Part of the sco SDK. GPL-3.0, like sco-core.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.RegularExpressions;
using Sco.Sdk.Game;
using Sco.Sdk.Interop;

namespace Sco.Sdk.Tests
{
    internal static class Layout
    {
        private static readonly Dictionary<string, Type> Types = new()
        {
            ["sco_result"] = typeof(ScoResult),
            ["sco_log_level"] = typeof(ScoLogLevel),
            ["sco_arg_type"] = typeof(ScoArgType),
            ["sco_sql_type"] = typeof(ScoSqlType),
            ["sco_arg"] = typeof(ScoArg),
            ["sco_arg_def"] = typeof(ScoArgDef),
            ["sco_command"] = typeof(ScoCommand),
            ["sco_api"] = typeof(ScoApi),
            ["sco_plugin_info"] = typeof(ScoPluginInfo),
            ["sco_sql_value"] = typeof(ScoSqlValue),
            ["sco_storage_v1"] = typeof(ScoStorageV1),
            ["sco_ui_v1"] = typeof(ScoUiV1),
            ["sco_dc_type"] = typeof(ScoDcType),
            ["sco_dc_value"] = typeof(ScoDcValue),
            ["sco_dc_report"] = typeof(ScoDcReport),
            ["sco_dc_applied"] = typeof(ScoDcApplied),
            ["sco_datacore_v1"] = typeof(ScoDatacoreV1),
            ["sc_actors_v1"] = typeof(ScActorsV1),
            ["sc_vehicle_seat_flag"] = typeof(ScVehicleSeatFlag),
            ["sc_vehicle_seat"] = typeof(ScVehicleSeat),
            ["sc_vehicles_v1"] = typeof(ScVehiclesV1),
        };

        private static readonly Dictionary<string, long> Constants = new()
        {
            ["SCO_API_MAJOR"] = Abi.ApiMajor,
            ["SCO_API_MINOR"] = Abi.ApiMinor,
            ["SCO_OK"] = (long)ScoResult.Ok,
            ["SCO_UNAVAILABLE"] = (long)ScoResult.Unavailable,
            ["SCO_NOT_FOUND"] = (long)ScoResult.NotFound,
            ["SCO_BAD_ARG"] = (long)ScoResult.BadArg,
            ["SCO_CRASHED"] = (long)ScoResult.Crashed,
            ["SCO_WRONG_THREAD"] = (long)ScoResult.WrongThread,
            ["SCO_TOO_MANY"] = (long)ScoResult.TooMany,
            ["SCO_FAILED"] = (long)ScoResult.Failed,
            ["SCO_RESULT_FORCE32"] = (long)ScoResult.Force32,
            ["SCO_LOG_INFO"] = (long)ScoLogLevel.Info,
            ["SCO_LOG_WARN"] = (long)ScoLogLevel.Warn,
            ["SCO_LOG_ERROR"] = (long)ScoLogLevel.Error,
            ["SCO_LOG_FORCE32"] = (long)ScoLogLevel.Force32,
            ["SCO_ARG_INT"] = (long)ScoArgType.Int,
            ["SCO_ARG_FLOAT"] = (long)ScoArgType.Float,
            ["SCO_ARG_STRING"] = (long)ScoArgType.String,
            ["SCO_ARG_BOOL"] = (long)ScoArgType.Bool,
            ["SCO_ARG_FORCE32"] = (long)ScoArgType.Force32,
            ["SCO_STORAGE_VERSION_1_0"] = Abi.StorageVersion1_0,
            ["SCO_STORAGE_MAX_KEY"] = Abi.StorageMaxKey,
            ["SCO_STORAGE_MAX_VALUE"] = Abi.StorageMaxValue,
            ["SCO_STORAGE_MAX_CURSORS"] = Abi.StorageMaxCursors,
            ["SCO_SQL_NULL"] = (long)ScoSqlType.Null,
            ["SCO_SQL_INT"] = (long)ScoSqlType.Int,
            ["SCO_SQL_FLOAT"] = (long)ScoSqlType.Float,
            ["SCO_SQL_TEXT"] = (long)ScoSqlType.Text,
            ["SCO_SQL_BLOB"] = (long)ScoSqlType.Blob,
            ["SCO_SQL_TYPE_FORCE32"] = (long)ScoSqlType.Force32,
            ["SCO_UI_VERSION_1_0"] = Abi.UiVersion1_0,
            ["SCO_UI_MAX_ID"] = Abi.UiMaxId,
            ["SCO_UI_MAX_TITLE"] = Abi.UiMaxTitle,
            ["SCO_UI_MAX_BADGE"] = Abi.UiMaxBadge,
            ["SCO_UI_MAX_CHORD"] = Abi.UiMaxChord,
            ["SCO_UI_MAX_HOTKEY_ARGS"] = Abi.UiMaxHotkeyArgs,
            ["SCO_UI_MAX_ARG_STRING"] = Abi.UiMaxArgString,
            ["SCO_DATACORE_VERSION_1_0"] = Abi.DataCoreVersion1_0,
            ["SCO_DATACORE_VERSION_1_1"] = Abi.DataCoreVersion1_1,
            ["SCO_DC_OPEN"] = Abi.DcOpen,
            ["SCO_DC_LOADED"] = Abi.DcLoaded,
            ["SCO_DC_NON_ATOMIC"] = Abi.DcNonAtomic,
            ["SCO_DC_QUEUED"] = Abi.DcQueued,
            ["SCO_DC_APPLIED"] = Abi.DcApplied,
            ["SCO_DC_SKIPPED"] = Abi.DcSkipped,
            ["SCO_DC_REFUSED"] = Abi.DcRefused,
            ["SCO_DC_OP_PATCH"] = Abi.DcOpPatch,
            ["SCO_DC_MAX_OPS"] = Abi.DcMaxOps,
            ["SCO_DC_MAX_PATCHES"] = Abi.DcMaxPatches,
            ["SCO_DC_BOOL"] = (long)ScoDcType.Bool,
            ["SCO_DC_INT"] = (long)ScoDcType.Int,
            ["SCO_DC_UINT"] = (long)ScoDcType.UInt,
            ["SCO_DC_FLOAT"] = (long)ScoDcType.Float,
            ["SCO_DC_STRING"] = (long)ScoDcType.String,
            ["SCO_DC_GUID"] = (long)ScoDcType.Guid,
            ["SCO_DC_ENUM"] = (long)ScoDcType.Enum,
            ["SCO_DC_NULL"] = (long)ScoDcType.Null,
            ["SCO_DC_INSTANCE"] = (long)ScoDcType.Instance,
            ["SCO_DC_REF"] = (long)ScoDcType.Ref,
            ["SCO_DC_TYPE_FORCE32"] = (long)ScoDcType.Force32,
            ["SC_ACTORS_VERSION_1_0"] = GameAbi.ActorsVersion1_0,
            ["SC_ACTORS_MAX_NPCS"] = GameAbi.ActorsMaxNpcs,
            ["SC_VEHICLES_SERVICE_VERSION"] = GameAbi.VehiclesVersion1_0,
            ["SC_VEHICLE_SEAT_NAME_MAX"] = GameAbi.VehicleSeatNameMax,
            ["SC_SEAT_USABLE"] = (long)ScVehicleSeatFlag.Usable,
            ["SC_SEAT_USABLE_KNOWN"] = (long)ScVehicleSeatFlag.UsableKnown,
            ["SC_SEAT_OCCUPIED"] = (long)ScVehicleSeatFlag.Occupied,
            ["SC_SEAT_PILOT"] = (long)ScVehicleSeatFlag.Pilot,
            ["SC_SEAT_FLAG_FORCE32"] = (long)ScVehicleSeatFlag.Force32,
        };

        private static readonly Dictionary<string, string> Strings = new()
        {
            ["SCO_STORAGE_NAME"] = Abi.StorageName,
            ["SCO_UI_NAME"] = Abi.UiName,
            ["SCO_DATACORE_NAME"] = Abi.DataCoreName,
            ["SCO_DC_APPLIED_EVENT"] = Abi.DataCoreAppliedEvent,
            ["SC_ACTORS_NAME"] = GameAbi.ActorsName,
            ["SC_VEHICLES_SERVICE_NAME"] = GameAbi.VehiclesName,
        };

        private static readonly Regex Size = new(@"^SIZE\((\w+),\s*(\d+)\);");
        private static readonly Regex At = new(@"^AT\((\w+),\s*([\w.]+),\s*(\d+)\);");
        private static readonly Regex Pin = new(@"^PIN\((.*)\);");
        private static readonly Regex PinSizeofString = new(@"^sizeof\((\w+)\) == (\d+)$");
        private static readonly Regex PinConst = new(@"^(\w+) == (0x[0-9a-fA-F]+|\d+)u?$");

        public static bool IsPinLine(string line) => Size.IsMatch(line) || At.IsMatch(line) || Pin.IsMatch(line);

        /// <summary>Checks one pin line; null when it holds, else why not.</summary>
        public static string? Check(string line)
        {
            Match m;
            if ((m = Size.Match(line)).Success)
            {
                if (!Types.TryGetValue(m.Groups[1].Value, out Type? t)) return "no C# type for " + m.Groups[1].Value;
                long want = long.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture);
                long got = t.IsEnum ? Marshal.SizeOf(Enum.GetUnderlyingType(t)) : Marshal.SizeOf(t);
                long unsafeSize = t.IsEnum ? got : SizeOf(t);
                return got == want && unsafeSize == want ? null : $"size {got} (managed {unsafeSize}), C pins {want}";
            }
            if ((m = At.Match(line)).Success)
            {
                if (!Types.TryGetValue(m.Groups[1].Value, out Type? t)) return "no C# type for " + m.Groups[1].Value;
                long want = long.Parse(m.Groups[3].Value, CultureInfo.InvariantCulture);
                long got = 0;
                foreach (string field in m.Groups[2].Value.Split('.'))
                {
                    System.Reflection.FieldInfo? f = t.GetField(field);
                    if (f == null) return $"{t.Name} has no field {field}";
                    got += (long)Marshal.OffsetOf(t, field);
                    t = f.FieldType;
                }
                return got == want ? null : $"offset {got}, C pins {want}";
            }
            if ((m = Pin.Match(line)).Success)
            {
                string e = m.Groups[1].Value.Trim();
                if (e == "sizeof(void*) == 8") return IntPtr.Size == 8 ? null : "not 64-bit";
                if (e == "sizeof(void (*)(void)) == 8") return sizeof_fnptr() == 8 ? null : "function pointers aren't 8 bytes";
                Match s = PinSizeofString.Match(e);
                if (s.Success && Strings.TryGetValue(s.Groups[1].Value, out string? str))
                {
                    long n = Encoding.UTF8.GetByteCount(str) + 1;
                    return n == long.Parse(s.Groups[2].Value, CultureInfo.InvariantCulture) ? null : $"\"{str}\" is {n} bytes with the NUL";
                }
                Match c = PinConst.Match(e);
                if (c.Success && Constants.TryGetValue(c.Groups[1].Value, out long have))
                {
                    string v = c.Groups[2].Value;
                    long want = v.StartsWith("0x", StringComparison.Ordinal)
                        ? long.Parse(v.AsSpan(2), NumberStyles.HexNumber, CultureInfo.InvariantCulture)
                        : long.Parse(v, CultureInfo.InvariantCulture);
                    return have == want ? null : $"C# has {have}";
                }
                return "the C# layout test can't check this pin; update the SDK and Layout.cs";
            }
            return "not a pin line";
        }

        private static unsafe int sizeof_fnptr() => sizeof(delegate* unmanaged[Cdecl]<void>);

        private static int SizeOf(Type t)
        {
            // Unsafe.SizeOf<T> for a runtime type, without reflection-emitted generics: the set is fixed.
            if (t == typeof(ScoArg)) return Unsafe.SizeOf<ScoArg>();
            if (t == typeof(ScoArgDef)) return Unsafe.SizeOf<ScoArgDef>();
            if (t == typeof(ScoCommand)) return Unsafe.SizeOf<ScoCommand>();
            if (t == typeof(ScoApi)) return Unsafe.SizeOf<ScoApi>();
            if (t == typeof(ScoPluginInfo)) return Unsafe.SizeOf<ScoPluginInfo>();
            if (t == typeof(ScoSqlValue)) return Unsafe.SizeOf<ScoSqlValue>();
            if (t == typeof(ScoStorageV1)) return Unsafe.SizeOf<ScoStorageV1>();
            if (t == typeof(ScoUiV1)) return Unsafe.SizeOf<ScoUiV1>();
            if (t == typeof(ScoDcValue)) return Unsafe.SizeOf<ScoDcValue>();
            if (t == typeof(ScoDcReport)) return Unsafe.SizeOf<ScoDcReport>();
            if (t == typeof(ScoDcApplied)) return Unsafe.SizeOf<ScoDcApplied>();
            if (t == typeof(ScoDatacoreV1)) return Unsafe.SizeOf<ScoDatacoreV1>();
            if (t == typeof(ScActorsV1)) return Unsafe.SizeOf<ScActorsV1>();
            if (t == typeof(ScVehicleSeat)) return Unsafe.SizeOf<ScVehicleSeat>();
            if (t == typeof(ScVehiclesV1)) return Unsafe.SizeOf<ScVehiclesV1>();
            return -1;
        }
    }
}
