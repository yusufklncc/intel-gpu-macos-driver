// SPDX-License-Identifier: MIT
// Reads what the Overview shows, through WMI/CIM, the registry and Windows APIs. Read-only: nothing is mounted, written
// or changed, and no administrator rights are asked for. Serial numbers, UUIDs, MAC addresses and the product key are
// never queried (the WMI queries name their properties); a volume's GUID path is used to look inside an EFI partition
// but is not kept.
using System.Management;
using System.Security.Principal;
using IntelXe.Core;
using Microsoft.Win32;

namespace IntelXe.App;

public static class SystemProbe
{
    static readonly System.Management.EnumerationOptions Limits = new() { Timeout = TimeSpan.FromSeconds(15), ReturnImmediately = false };

    static IEnumerable<ManagementBaseObject> Query(string scope, string wql)
    {
        using var searcher = new ManagementObjectSearcher(new ManagementScope(scope), new ObjectQuery(wql), Limits);
        foreach (var o in searcher.Get()) yield return o;
    }

    static string Str(object? v) => v?.ToString()?.Trim() ?? "";

    static void Try(Action a) { try { a(); } catch (Exception) { } }

    public static PcFacts Probe()
    {
        var f = new PcFacts();
        Try(() => ProbeGpus(f));
        Try(() =>
        {
            foreach (var o in Query(@"root\CIMV2", "SELECT Name FROM Win32_Processor")) { f.CpuName = Str(o["Name"]); break; }
        });
        Try(() =>
        {
            if (Native.CoreClasses() is { } c) { f.PerformanceCores = c.Performance; f.EfficiencyCores = c.Efficiency; }
        });
        Try(() =>
        {
            foreach (var o in Query(@"root\CIMV2", "SELECT Manufacturer, Product FROM Win32_BaseBoard"))
            {
                f.BoardMaker = Str(o["Manufacturer"]); f.BoardModel = Str(o["Product"]); break;
            }
        });
        Try(() =>
        {
            if (Native.GetFirmwareType(out int t)) f.Firmware = t switch { 1 => FirmwareMode.Legacy, 2 => FirmwareMode.Uefi, _ => FirmwareMode.Unknown };
        });
        Try(() => f.SecureBoot = ReadSecureBoot(f.Firmware));
        Try(() => f.Windows = ReadWindows());
        Try(() => f.BitLocker = ReadBitLocker());
        Try(() =>
        {
            using var id = WindowsIdentity.GetCurrent();
            f.IsAdministrator = new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
        });
        try { f.Esps.AddRange(ReadEsps()); }
        catch (Exception e) { f.EspError = e.Message; }
        return f;
    }

    // ---- GPUs: the display adapters, their PCI IDs and Windows driver, and the supported card's BARs and location ----
    static void ProbeGpus(PcFacts f)
    {
        var drivers = new Dictionary<string, (string Name, string Version)>(StringComparer.OrdinalIgnoreCase);
        Try(() =>
        {
            foreach (var o in Query(@"root\CIMV2", "SELECT Name, PNPDeviceID, DriverVersion FROM Win32_VideoController"))
                drivers[Str(o["PNPDeviceID"])] = (Str(o["Name"]), Str(o["DriverVersion"]));
        });
        // every present display-class device, also one without a working driver
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var o in Query(@"root\CIMV2", "SELECT Name, DeviceID FROM Win32_PnPEntity WHERE PNPClass = 'Display'"))
        {
            var id = Str(o["DeviceID"]);
            if (!seen.Add(id)) continue;
            var pci = PciId.Parse(id);
            drivers.TryGetValue(id, out var d);
            string location = SupportedCards.Find(pci) is not null ? LocationPath(o as ManagementObject) : "";
            f.Gpus.Add(new GpuInfo(d.Name is { Length: > 0 } ? d.Name : Str(o["Name"]), pci, d.Version ?? "", location));
            if (f.Bar is null && SupportedCards.Find(pci) is not null) f.Bar = ReadBars(id);
        }
        foreach (var (id, d) in drivers)
            if (seen.Add(id)) f.Gpus.Add(new GpuInfo(d.Name, PciId.Parse(id), d.Version));
    }

    /// DEVPKEY_Device_LocationPaths' ACPI(...) line, as Device Manager shows it (Win32_PnPEntity.GetDeviceProperties).
    static string LocationPath(ManagementObject? device)
    {
        if (device is null) return "";
        try
        {
            using var input = device.GetMethodParameters("GetDeviceProperties");
            input["devicePropertyKeys"] = new[] { "DEVPKEY_Device_LocationPaths" };
            using var output = device.InvokeMethod("GetDeviceProperties", input, null);
            if (output?["deviceProperties"] is ManagementBaseObject[] props)
                foreach (var p in props)
                    if (p["Data"] is string[] lines)
                        foreach (var line in lines)
                            if (line.StartsWith("ACPI(", StringComparison.OrdinalIgnoreCase)) return line;
        }
        catch (Exception) { }
        return "";
    }

    /// The card's memory ranges from Enum\<instance>\Control\AllocConfig (a REG_RESOURCE_LIST), as collect-windows.ps1.
    static BarSummary? ReadBars(string instanceId) =>
        ResourceList.Summarize(Native.ReadMachineValue(@"SYSTEM\CurrentControlSet\Enum\" + instanceId + @"\Control", "AllocConfig"));

    // ---- firmware, Windows, BitLocker ---------------------------------------------------------------------------------
    static SecureBootState ReadSecureBoot(FirmwareMode firmware)
    {
        using var key = Registry.LocalMachine.OpenSubKey(@"SYSTEM\CurrentControlSet\Control\SecureBoot\State");
        if (key?.GetValue("UEFISecureBootEnabled") is int v) return v == 1 ? SecureBootState.On : SecureBootState.Off;
        return firmware == FirmwareMode.Legacy ? SecureBootState.NotSupported : SecureBootState.Unknown;
    }

    static WindowsInfo? ReadWindows()
    {
        using var key = Registry.LocalMachine.OpenSubKey(@"SOFTWARE\Microsoft\Windows NT\CurrentVersion");
        if (key is null) return null;
        int.TryParse(Str(key.GetValue("CurrentBuildNumber")), out int build);
        int ubr = key.GetValue("UBR") is int u ? u : 0;
        return new WindowsInfo(Str(key.GetValue("ProductName")), Str(key.GetValue("DisplayVersion")), build, ubr);
    }

    /// The system drive's BitLocker state: Win32_EncryptableVolume when the app runs as administrator, else the shell's
    /// System.Volume.BitLockerProtection (what Explorer shows), which needs no rights. Only read.
    static BitLockerState ReadBitLocker()
    {
        var drive = (Environment.GetEnvironmentVariable("SystemDrive") ?? "C:").TrimEnd('\\');
        try
        {
            foreach (var o in Query(@"root\CIMV2\Security\MicrosoftVolumeEncryption",
                         $"SELECT ProtectionStatus FROM Win32_EncryptableVolume WHERE DriveLetter = '{drive}'"))
                if (o["ProtectionStatus"] is uint status) return BitLocker.FromProtectionStatus(status);
        }
        catch (Exception) { }   // access denied without administrator rights

        object? value = null;
        var thread = new Thread(() =>
        {
            try
            {
                var type = Type.GetTypeFromProgID("Shell.Application");
                if (type is null) return;
                dynamic shell = Activator.CreateInstance(type)!;
                dynamic folder = shell.NameSpace(drive + "\\");
                value = folder?.Self?.ExtendedProperty("System.Volume.BitLockerProtection");
            }
            catch (Exception) { }
        });
        thread.SetApartmentState(ApartmentState.STA);
        thread.Start();
        thread.Join(TimeSpan.FromSeconds(10));
        return BitLocker.FromShellProperty(value);
    }

    // ---- EFI system partitions: the same detection as IntelXe-EFI.ps1's Get-EspList (GPT type), nothing mounted ------
    static IEnumerable<EspPartition> ReadEsps()
    {
        const string storage = @"root\Microsoft\Windows\Storage";
        var disks = new Dictionary<uint, (string Name, string Bus)>();
        foreach (var o in Query(storage, "SELECT Number, FriendlyName, BusType FROM MSFT_Disk"))
            disks[Convert.ToUInt32(o["Number"])] = (Str(o["FriendlyName"]), EspPartition.BusName(Convert.ToInt32(o["BusType"] ?? 0)));
        var list = new List<EspPartition>();
        foreach (var o in Query(storage, "SELECT DiskNumber, PartitionNumber, Size, GptType, DriveLetter, AccessPaths, IsSystem FROM MSFT_Partition"))
        {
            if (!EspPartition.IsEspType(Str(o["GptType"]))) continue;
            uint disk = Convert.ToUInt32(o["DiskNumber"]);
            char letter = o["DriveLetter"] is char c && c != '\0' ? c : '\0';
            var paths = o["AccessPaths"] as string[] ?? [];
            var root = letter != '\0' ? $"{letter}:\\" : paths.FirstOrDefault(p => p.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase));
            disks.TryGetValue(disk, out var d);
            list.Add(new EspPartition(d.Name is { Length: > 0 } ? d.Name : "?", d.Bus ?? "", disk, Convert.ToUInt32(o["PartitionNumber"]),
                Convert.ToUInt64(o["Size"]), letter != '\0' ? $"{letter}:" : "", o["IsSystem"] is true, LookForOpenCore(root)));
        }
        return list.OrderBy(e => e.DiskNumber).ThenBy(e => e.PartitionNumber);
    }

    /// OpenCore when EFI\OC\OpenCore.efi is on the partition; NeedsMount when Windows does not let the app look (an EFI
    /// partition without a drive letter is closed to programs that are not administrators; only the EFI page's elevated
    /// helper mounts one).
    static OpenCoreState LookForOpenCore(string? root)
    {
        if (root is null) return OpenCoreState.NeedsMount;
        try
        {
            _ = Directory.EnumerateFileSystemEntries(root).Take(1).ToList();   // throws when the volume cannot be read
            return EspPartition.Detect(rel => File.Exists(Path.Combine(root, rel)));
        }
        catch (Exception) { return OpenCoreState.NeedsMount; }
    }
}
