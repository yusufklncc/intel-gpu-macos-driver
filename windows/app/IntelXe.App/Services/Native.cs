// SPDX-License-Identifier: MIT
using System.Runtime.InteropServices;

namespace IntelXe.App;

/// The few Windows functions the app calls directly; all of them only read.
static partial class Native
{
    [LibraryImport("user32.dll")]
    internal static partial uint GetDpiForWindow(IntPtr hwnd);

    /// FIRMWARE_TYPE: 1 BIOS (legacy), 2 UEFI.
    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool GetFirmwareType(out int firmwareType);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    internal static partial bool GetLogicalProcessorInformationEx(int relationship, IntPtr buffer, ref uint length);

    [LibraryImport("advapi32.dll", StringMarshalling = StringMarshalling.Utf16)]
    private static partial int RegGetValueW(IntPtr key, string subKey, string value, uint flags, out uint type, byte[]? data, ref uint size);

    static readonly IntPtr LocalMachine = new(unchecked((int)0x80000002));
    const uint AnyType = 0x0000ffff;   // RRF_RT_ANY

    /// A value of HKEY_LOCAL_MACHINE as raw bytes, whatever its type: .NET's RegistryKey.GetValue does not return
    /// REG_RESOURCE_LIST (the card's AllocConfig). Null when it is not there or cannot be read.
    internal static byte[]? ReadMachineValue(string subKey, string value)
    {
        uint size = 0;
        if (RegGetValueW(LocalMachine, subKey, value, AnyType, out _, null, ref size) != 0 || size == 0) return null;
        var data = new byte[size];
        if (RegGetValueW(LocalMachine, subKey, value, AnyType, out _, data, ref size) != 0) return null;
        return size == data.Length ? data : data[..(int)size];
    }

    const int RelationProcessorCore = 0;

    /// Cores by efficiency class (PROCESSOR_RELATIONSHIP.EfficiencyClass): on a hybrid CPU the P-cores have the higher
    /// class. (performance, efficiency); null when Windows does not tell.
    internal static (int Performance, int Efficiency)? CoreClasses()
    {
        uint length = 0;
        GetLogicalProcessorInformationEx(RelationProcessorCore, IntPtr.Zero, ref length);
        if (length == 0) return null;
        var buffer = Marshal.AllocHGlobal((int)length);
        try
        {
            if (!GetLogicalProcessorInformationEx(RelationProcessorCore, buffer, ref length)) return null;
            var classes = new List<byte>();
            for (uint offset = 0; offset < length;)
            {
                var item = buffer + (int)offset;
                int relationship = Marshal.ReadInt32(item);
                int size = Marshal.ReadInt32(item, 4);
                if (size <= 0) break;
                if (relationship == RelationProcessorCore) classes.Add(Marshal.ReadByte(item, 9));   // Flags at 8, EfficiencyClass at 9
                offset += (uint)size;
            }
            if (classes.Count == 0) return null;
            byte top = classes.Max();
            int performance = classes.Count(c => c == top);
            return (performance, classes.Count - performance);
        }
        finally { Marshal.FreeHGlobal(buffer); }
    }
}
