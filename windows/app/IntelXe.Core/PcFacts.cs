// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

/// A display adapter as Windows lists it (Win32_VideoController): its name, PCI IDs, Windows driver version and the
/// "Location paths" line Device Manager shows (for the ACPI path stage 4 needs).
public sealed record GpuInfo(string Name, PciId? Id, string DriverVersion = "", string LocationPath = "");

/// What the Overview knows about this PC. The app fills it from WMI/CIM, the registry and Windows APIs, reading only;
/// Core judges it (PcChecks). No serial numbers, UUIDs, MAC addresses or product keys are ever put in here.
public sealed class PcFacts
{
    public List<GpuInfo> Gpus { get; init; } = [];
    public string CpuName { get; set; } = "";
    /// performance and efficiency cores by the cores' efficiency class (GetLogicalProcessorInformationEx); null when unknown
    public int? PerformanceCores { get; set; }
    public int? EfficiencyCores { get; set; }
    public string BoardMaker { get; set; } = "";
    public string BoardModel { get; set; } = "";
    public FirmwareMode Firmware { get; set; }
    public SecureBootState SecureBoot { get; set; }
    /// the supported card's BARs; null when they could not be read
    public BarSummary? Bar { get; set; }
    public WindowsInfo? Windows { get; set; }
    public BitLockerState BitLocker { get; set; }
    public List<EspPartition> Esps { get; init; } = [];
    /// why the EFI partitions could not be listed; null when they were
    public string? EspError { get; set; }
    public bool IsAdministrator { get; set; }

    // ---- for the BIOS page (BiosSteps), read-only like the rest ----
    /// the storage controllers' names (SCSI/RAID, NVMe and IDE/SATA classes); null when they could not be read
    public List<string>? StorageControllers { get; set; }
    /// whether the firmware publishes an ACPI DMAR table (VT-d on); null when the tables could not be read
    public bool? DmarTable { get; set; }
    /// Win32_Processor.VirtualizationFirmwareEnabled; null when not reported
    public bool? VirtualizationFirmwareEnabled { get; set; }
    /// Win32_Processor.NumberOfCores and NumberOfLogicalProcessors; null when not reported
    public int? Cores { get; set; }
    public int? Threads { get; set; }

    /// The integrated graphics Windows lists: an Intel display adapter that is not a supported card and not a discrete
    /// Arc (A- or B-series, "Intel(R) Arc(TM) B580 Graphics").
    public IEnumerable<GpuInfo> IntegratedGpus => Gpus.Where(g => g.Id?.Vendor == PciId.Intel && SupportedCards.Find(g.Id) is null
        && !System.Text.RegularExpressions.Regex.IsMatch(g.Name, @"Arc\S*\s+(\(TM\)\s*)?[AB]\d{3}", System.Text.RegularExpressions.RegexOptions.IgnoreCase));

    /// The first supported card; null without one.
    public GpuInfo? Card => Gpus.FirstOrDefault(g => SupportedCards.Find(g.Id) is not null);

    /// The first other Intel GPU (an Arc the driver does not support yet, or integrated graphics).
    public GpuInfo? OtherIntelGpu => Gpus.FirstOrDefault(g => g.Id?.Vendor == PciId.Intel && SupportedCards.Find(g.Id) is null);

    public bool HybridCpu => CpuInfo.IsHybrid(CpuName);
}
