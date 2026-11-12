// SPDX-License-Identifier: MIT
// The disks the stick page may write, and the rules that refuse the others. The app reads a disk (MSFT_Disk,
// MSFT_Partition, Win32_DiskDrive) into a StickDisk; StickRules says why it must not be erased. Only USB disks are listed;
// a USB disk is still refused when it is the boot or system disk, not removable, read-only, offline, too small, or holds
// an operating system or the files being copied. The disk is re-identified by its serial number and size right before
// it is erased (StickIdentity): the serial is used only inside that hash and never shown or logged.
using System.Security.Cryptography;
using System.Text;

namespace IntelXe.Core.Stick;

/// A partition of a disk, as far as the rules need it.
public sealed record StickPartition(uint Number, string GptType, int MbrType, string Letter, string FileSystem, bool HoldsWindows);

/// A disk as the stick page sees it. Not a record: its ToString must not print the serial number.
public sealed class StickDisk
{
    public uint Number { get; init; }
    public string FriendlyName { get; init; } = "";
    public ulong Size { get; init; }
    /// MSFT_Disk.BusType: 7 USB, 15 file-backed virtual (a VHD/VHDX), 17 NVMe ...
    public int BusType { get; init; }
    public bool IsBoot { get; init; }
    public bool IsSystem { get; init; }
    public bool IsReadOnly { get; init; }
    public bool IsOffline { get; init; }
    /// Win32_DiskDrive: media type "Removable Media" or capability 7 (supports removable media); null when unknown
    public bool? Removable { get; init; }
    /// the backing file of a file-backed virtual disk (MSFT_Disk.Location); "" otherwise
    public string VirtualFile { get; init; } = "";
    /// "GPT", "MBR", "RAW" or ""
    public string PartitionStyle { get; init; } = "";
    public IReadOnlyList<StickPartition> Partitions { get; init; } = [];
    /// The serial number (or the unique ID when there is none): for StickIdentity only. Never shown or logged.
    public string IdentitySource { private get; init; } = "";

    public string BusName => EspPartition.BusName(BusType);

    /// "SanDisk Ultra 16 GB": what the user types to confirm.
    public string ConfirmText => $"{FriendlyName.Trim()} {StickRules.SizeText(Size)}";

    public string Token(string salt) => StickIdentity.Token(IdentitySource, Size, salt);
    public bool HasIdentity => IdentitySource.Trim().Length > 0;

    public override string ToString() => $"disk {Number} {FriendlyName} {StickRules.SizeText(Size)} {BusName}";
}

public static class StickIdentity
{
    /// SHA-256 of a per-run salt, the serial and the size: the helper compares it with the disk it finds, so that the
    /// serial never goes on a command line.
    public static string Token(string serial, ulong size, string salt) =>
        Convert.ToHexString(SHA256.HashData(Encoding.UTF8.GetBytes(salt + "\n" + serial.Trim() + "\n" + size.ToString(System.Globalization.CultureInfo.InvariantCulture))));

    public static string NewSalt() => Convert.ToHexString(RandomNumberGenerator.GetBytes(16));
}

/// What a check allows beyond a USB stick: only for the tests, a VHD/VHDX file named on the command line
/// (--test-vhdx PATH), which Windows reports as a file-backed virtual disk, never as USB.
public sealed record StickPolicy(ulong MinimumSize, string? TestVirtualFile = null, IReadOnlyList<string>? ProtectedLetters = null);

public static class StickRules
{
    public const int UsbBus = 7, FileBackedVirtualBus = 15;

    // partitions that mean an operating system lives on the disk (not the Microsoft reserved partition: Windows puts
    // one on every disk it initializes as GPT, a stick written here too)
    static readonly Dictionary<string, string> OsGptTypes = new(StringComparer.OrdinalIgnoreCase)
    {
        ["{7c3457ef-0000-11aa-aa11-00306543ecac}"] = "an APFS container (macOS)",
        ["{426f6f74-0000-11aa-aa11-00306543ecac}"] = "an Apple boot partition (macOS)",
        ["{de94bba4-06d1-4d40-a16a-bfd50179d6ac}"] = "a Windows recovery partition",
        ["{4f68bce3-e8cd-4db1-96e7-fbcaf984b709}"] = "a Linux root partition",
        ["{0fc63daf-8483-4772-8e79-3d69d8477de4}"] = "a Linux file system",
        ["{e6d6d379-f507-44c2-a23c-238f2a3df928}"] = "a Linux LVM volume",
    };

    static readonly Dictionary<int, string> OsMbrTypes = new()
    {
        [0x27] = "a Windows recovery partition",
        [0x83] = "a Linux file system",
        [0x8E] = "a Linux LVM volume",
    };

    /// The size the way the page shows it: decimal GB with one decimal from 1 GB, else whole MB.
    public static string SizeText(ulong bytes) =>
        bytes >= 1_000_000_000UL
            ? (bytes / 1e9).ToString("0.0", System.Globalization.CultureInfo.InvariantCulture) + " GB"
            : Math.Round(bytes / 1e6).ToString(System.Globalization.CultureInfo.InvariantCulture) + " MB";

    /// Whether the page lists the disk at all: USB disks, and the test VHDX when it is asked for.
    public static bool Listed(StickDisk d, StickPolicy p) => d.BusType == UsbBus || IsTestDisk(d, p);

    static bool IsTestDisk(StickDisk d, StickPolicy p) =>
        p.TestVirtualFile is { Length: > 0 } f && d.BusType == FileBackedVirtualBus && d.VirtualFile.Length > 0
        && string.Equals(Path.GetFullPath(d.VirtualFile), Path.GetFullPath(f), StringComparison.OrdinalIgnoreCase)
        && (f.EndsWith(".vhdx", StringComparison.OrdinalIgnoreCase) || f.EndsWith(".vhd", StringComparison.OrdinalIgnoreCase));

    /// Why the disk must not be erased; empty when it may be. Each reason is English, for the helper's answer and the
    /// page (which shows them as they are).
    public static IReadOnlyList<string> Refusals(StickDisk d, StickPolicy p)
    {
        var r = new List<string>();
        bool test = IsTestDisk(d, p);
        if (d.BusType != UsbBus && !test)
            r.Add($"not a USB disk (bus: {(d.BusName.Length > 0 ? d.BusName : d.BusType.ToString(System.Globalization.CultureInfo.InvariantCulture))})");
        if (d.IsBoot) r.Add("Windows booted from it (boot disk)");
        if (d.IsSystem) r.Add("it holds the system (EFI) partition Windows started from");
        if (d.IsReadOnly) r.Add("it is read-only");
        if (d.IsOffline) r.Add("it is offline");
        if (!test && d.Removable != true) r.Add(d.Removable is null ? "Windows does not say it is removable" : "not a removable disk");
        if (d.Size < p.MinimumSize) r.Add($"too small: {SizeText(d.Size)}, needs at least {SizeText(p.MinimumSize)}");
        foreach (var part in d.Partitions)
        {
            if (part.HoldsWindows) r.Add($"partition {part.Number} holds Windows");
            if (OsGptTypes.TryGetValue(part.GptType.Trim(), out var what)) r.Add($"partition {part.Number} is {what}");
            if (OsMbrTypes.TryGetValue(part.MbrType, out var mbr)) r.Add($"partition {part.Number} is {mbr}");
            if (part.Letter.Length > 0 && p.ProtectedLetters?.Any(l => string.Equals(l.TrimEnd(':', '\\'), part.Letter.TrimEnd(':', '\\'), StringComparison.OrdinalIgnoreCase)) == true)
                r.Add($"it holds {part.Letter.TrimEnd(':', '\\')}:, where files to copy are");
        }
        if (!d.HasIdentity) r.Add("Windows reports no serial number or unique ID, so the disk cannot be identified again before erasing");
        return r.Distinct().ToList();
    }

    /// The FAT32 partition's size: the whole disk up to 16 GiB. Windows formats FAT32 only up to 32 GB, and the
    /// recovery image and an EFI need about 1.2 GB; Dortania's Windows guide asks for a stick of at least 4 GB.
    public const ulong MaxPartition = 16UL << 30;

    /// The smallest disk that takes the image, an EFI and room for the file system: the image plus 256 MB.
    public static ulong MinimumFor(long imageSize) => (ulong)Math.Max(0, imageSize) + (256UL << 20);
}
