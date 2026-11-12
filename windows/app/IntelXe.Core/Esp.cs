// SPDX-License-Identifier: MIT
using System.Globalization;

namespace IntelXe.Core;

/// Whether an EFI system partition holds OpenCore, as far as the app could tell without mounting it.
public enum OpenCoreState
{
    /// EFI\OC\OpenCore.efi is there
    Found,
    /// the partition could be read and has no EFI\OC\OpenCore.efi
    NotFound,
    /// Windows did not let the app read it: it has no drive letter, and reading it needs mounting (administrator)
    NeedsMount,
}

/// An EFI system partition (GPT type C12A7328-F81F-11D2-BA4B-00A0C93EC93B), the way IntelXe-EFI.ps1's Get-EspList lists
/// it: its disk's name and bus, disk and partition number, size, drive letter, and whether Windows booted from it (IsSystem). The
/// volume's GUID path is used to look inside but never shown (it is a unique ID).
public sealed record EspPartition(string Disk, string Bus, uint DiskNumber, uint PartitionNumber, ulong Size, string Letter,
    bool IsSystem, OpenCoreState OpenCore)
{
    public const string GptType = "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}";

    public static bool IsEspType(string? gptType) => string.Equals(gptType?.Trim(), GptType, StringComparison.OrdinalIgnoreCase);

    public string SizeText => FormatSize(Size);

    /// The size the way IntelXe-EFI.ps1 (Util.Size) writes it: whole GB from 1 GB up, else whole MB (decimal units).
    public static string FormatSize(ulong bytes) =>
        bytes >= 1_000_000_000UL
            ? Math.Round(bytes / 1e9).ToString(CultureInfo.InvariantCulture) + " GB"
            : Math.Round(bytes / 1e6).ToString(CultureInfo.InvariantCulture) + " MB";

    /// MSFT_Disk.BusType as Get-Disk names it.
    public static string BusName(int busType) => busType switch
    {
        1 => "SCSI", 2 => "ATAPI", 3 => "ATA", 4 => "1394", 5 => "SSA", 6 => "Fibre Channel", 7 => "USB", 8 => "RAID",
        9 => "iSCSI", 10 => "SAS", 11 => "SATA", 12 => "SD", 13 => "MMC", 14 => "Virtual", 15 => "File Backed Virtual",
        16 => "Storage Spaces", 17 => "NVMe", 18 => "SCM", 19 => "UFS",
        _ => "",
    };

    /// What a list of files at the partition's root and in EFI\OC says: OpenCore when EFI\OC\OpenCore.efi is there.
    public static OpenCoreState Detect(Func<string, bool> fileExists) =>
        fileExists(@"EFI\OC\OpenCore.efi") ? OpenCoreState.Found : OpenCoreState.NotFound;
}
