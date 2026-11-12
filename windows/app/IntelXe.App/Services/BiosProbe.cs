// SPDX-License-Identifier: MIT
// The few extra facts the BIOS page judges by, read like the rest of SystemProbe: without administrator rights,
// changing nothing. The storage controllers' names (RAID / Intel RST / VMD), whether the firmware publishes an ACPI DMAR
// table (VT-d on), whether Windows sees virtualization enabled in the firmware, and the core and thread counts.
using System.Management;
using System.Runtime.InteropServices;
using IntelXe.Core;

namespace IntelXe.App;

static partial class BiosProbe
{
    static string Str(object? v) => v?.ToString()?.Trim() ?? "";

    public static void Probe(PcFacts f)
    {
        try { f.StorageControllers = StorageControllers(); } catch (Exception) { }
        try { f.DmarTable = HasAcpiTable("DMAR"); } catch (Exception) { }
        try
        {
            using var s = new ManagementObjectSearcher(@"root\CIMV2", "SELECT NumberOfCores, NumberOfLogicalProcessors, VirtualizationFirmwareEnabled FROM Win32_Processor");
            int cores = 0, threads = 0;
            bool? virt = null;
            foreach (var o in s.Get())
            {
                cores += Convert.ToInt32(o["NumberOfCores"] ?? 0);
                threads += Convert.ToInt32(o["NumberOfLogicalProcessors"] ?? 0);
                if (o["VirtualizationFirmwareEnabled"] is bool b) virt = (virt ?? true) && b;
            }
            if (cores > 0) { f.Cores = cores; f.Threads = threads; }
            f.VirtualizationFirmwareEnabled = virt;
        }
        catch (Exception) { }
    }

    /// The names of the present storage controllers: SCSI/RAID adapters (Intel RST and VMD show up there), IDE/SATA
    /// controllers (AHCI) and NVMe controllers.
    static List<string> StorageControllers()
    {
        var names = new List<string>();
        using var s = new ManagementObjectSearcher(@"root\CIMV2", "SELECT Name, PNPClass FROM Win32_PnPEntity WHERE PNPClass = 'SCSIAdapter' OR PNPClass = 'HDC'");
        foreach (var o in s.Get())
        {
            var n = Str(o["Name"]);
            if (n.Length > 0 && !names.Contains(n)) names.Add(n);
        }
        return names;
    }

    [LibraryImport("kernel32.dll", SetLastError = true)]
    private static partial uint EnumSystemFirmwareTables(uint provider, byte[]? buffer, uint size);

    const uint Acpi = 0x41435049;   // 'ACPI'

    /// Whether the firmware's ACPI tables include this one (EnumSystemFirmwareTables lists their signatures; no rights
    /// needed). Null when they cannot be listed.
    static bool? HasAcpiTable(string signature)
    {
        uint size = EnumSystemFirmwareTables(Acpi, null, 0);
        if (size == 0) return null;
        var buf = new byte[size];
        if (EnumSystemFirmwareTables(Acpi, buf, size) != size) return null;
        for (int i = 0; i + 4 <= buf.Length; i += 4)
            if (System.Text.Encoding.ASCII.GetString(buf, i, 4) == signature) return true;
        return false;
    }
}
