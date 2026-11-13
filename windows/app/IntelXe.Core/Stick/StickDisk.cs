// SPDX-License-Identifier: MIT
// The disks a stick may be written to, and the rules that refuse the others: one set for the macOS installer stick
// (Stick/) and the Xe probe stick (ProbeStick/). The app reads a disk (MSFT_Disk, MSFT_Partition, Win32_DiskDrive) into a
// StickDisk; StickRules says why it must not be erased. Only USB disks are listed; a USB disk is still refused when it is
// the boot or system disk, not removable, read-only, offline, too small, larger than 128 GB (unless the user says they
// mean it), or holds an operating system or the files being copied. The rules that write-stick.sh shares (bus,
// removable, system, size) give the answers of tools/probe-stick/stick-disk-vectors.json, by code (StickRefusal.Code).
// A disk that holds an EFI (an EFI system partition, or a volume with EFI\OC\OpenCore.efi or EFI\BOOT\BOOTx64.efi) is
// not refused, but erasing it needs its own confirmation (HoldsEfi; the helpers' --erase-efi). The disk is
// re-identified by its serial number and size right before it is erased (StickIdentity): the serial is used only
// inside that hash and never shown or logged.
using System.Security.Cryptography;
using System.Text;

namespace IntelXe.Core.Stick;

/// A partition of a disk, as far as the rules need it. HoldsEfi: its volume has EFI\OC\OpenCore.efi, or an
/// EFI\BOOT\BOOTx64.efi that is not a probe stick's (an EFI system partition counts by its type, unopened). ProbeStick:
/// its volume is a probe stick (label XEPROBE, the marker file), whose reports in REPORTS\ the erase would lose.
public sealed record StickPartition(uint Number, string GptType, int MbrType, string Letter, string FileSystem, bool HoldsWindows,
    bool HoldsEfi = false, bool ProbeStick = false);

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
/// (--test-vhdx PATH), which Windows reports as a file-backed virtual disk, never as USB. AllowLarge: the user said they
/// mean a disk larger than 128 GB (write-stick.sh's --allow-large).
public sealed record StickPolicy(ulong MinimumSize, string? TestVirtualFile = null, IReadOnlyList<string>? ProtectedLetters = null, bool AllowLarge = false);

/// One reason to refuse a disk: its code (the shared ones are write-stick.sh's: partition, virtual, not-usb,
/// not-removable, system, too-small, too-large; the others are Windows': read-only, offline, os, files-here, no-identity)
/// and its English text.
public sealed record StickRefusal(string Code, string Text);

public static class StickRules
{
    public const int UsbBus = 7, VirtualBus = 14, FileBackedVirtualBus = 15;

    /// Larger than this, a disk is refused unless the policy allows large disks: more likely a backup disk than a stick
    /// (write-stick.sh's MAXGB).
    public const ulong LargeDisk = 128_000_000_000UL;

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
    public static IReadOnlyList<string> Refusals(StickDisk d, StickPolicy p) => [.. Check(d, p).Select(r => r.Text).Distinct()];

    /// The codes of the reasons, each once: what stick-disk-vectors.json holds.
    public static IReadOnlyList<string> RefusalCodes(StickDisk d, StickPolicy p) => [.. Check(d, p).Select(r => r.Code).Distinct()];

    public static IReadOnlyList<StickRefusal> Check(StickDisk d, StickPolicy p)
    {
        var r = new List<StickRefusal>();
        void Add(string code, string text) => r.Add(new StickRefusal(code, text));
        bool test = IsTestDisk(d, p);
        if (!test && d.BusType is VirtualBus or FileBackedVirtualBus)
            Add("virtual", $"not a USB disk (bus: {BusText(d)}): a virtual disk (VHD/VHDX)");
        else if (d.BusType != UsbBus && !test)
            Add("not-usb", $"not a USB disk (bus: {BusText(d)})");
        if (d.IsBoot) Add("system", "Windows booted from it (boot disk)");
        if (d.IsSystem) Add("system", "it holds the system (EFI) partition Windows started from");
        if (d.IsReadOnly) Add("read-only", "it is read-only");
        if (d.IsOffline) Add("offline", "it is offline");
        if (!test && d.Removable != true) Add("not-removable", d.Removable is null ? "Windows does not say it is removable" : "not a removable disk");
        if (d.Size < p.MinimumSize) Add("too-small", $"too small: {SizeText(d.Size)}, needs at least {SizeText(p.MinimumSize)}");
        if (!p.AllowLarge && d.Size > LargeDisk)
            Add("too-large", $"larger than {SizeText(LargeDisk)} ({SizeText(d.Size)}): more likely a backup disk than a stick (allow a larger disk if you mean it)");
        foreach (var part in d.Partitions)
        {
            if (part.HoldsWindows) Add("system", $"partition {part.Number} holds Windows");
            if (OsGptTypes.TryGetValue(part.GptType.Trim(), out var what)) Add("os", $"partition {part.Number} is {what}");
            if (OsMbrTypes.TryGetValue(part.MbrType, out var mbr)) Add("os", $"partition {part.Number} is {mbr}");
            if (part.Letter.Length > 0 && p.ProtectedLetters?.Any(l => string.Equals(l.TrimEnd(':', '\\'), part.Letter.TrimEnd(':', '\\'), StringComparison.OrdinalIgnoreCase)) == true)
                Add("files-here", $"it holds {part.Letter.TrimEnd(':', '\\')}:, where files to copy are");
        }
        if (!d.HasIdentity) Add("no-identity", "Windows reports no serial number or unique ID, so the disk cannot be identified again before erasing");
        return r;
    }

    static string BusText(StickDisk d) => d.BusName.Length > 0 ? d.BusName : d.BusType.ToString(System.Globalization.CultureInfo.InvariantCulture);

    /// Whether erasing the disk loses an EFI: an EFI system partition (by its GPT or MBR type; never opened), or a volume
    /// that holds OpenCore or another EFI boot loader. Not a refusal (an EFI stick is often the one to rewrite), but the
    /// page asks for it to be confirmed on its own, and the helpers refuse such a disk without --erase-efi.
    public static bool HoldsEfi(StickDisk d) =>
        d.Partitions.Any(p => EspPartition.IsEspType(p.GptType) || p.MbrType == 0xEF || p.HoldsEfi);

    /// Whether a volume holds an EFI by what is on it: OpenCore (EFI\OC\OpenCore.efi), or EFI\BOOT\BOOTx64.efi that is
    /// not a probe stick's own GRUB (the volume has xe-probe-stick.marker). The same rule as write-stick.sh's efi_rules,
    /// held together by stick-disk-vectors.json's efiVectors.
    public static bool VolumeHoldsEfi(bool openCore, bool bootLoader, bool probeStick) => openCore || (bootLoader && !probeStick);

    /// Whether the disk is a probe stick now (its reports would be lost).
    public static bool IsProbeStick(StickDisk d) => d.Partitions.Any(p => p.ProbeStick);

    /// The helpers' refusal of an EFI disk that was not confirmed; null when there is nothing to confirm.
    public static string? EfiNotConfirmed(StickDisk d, bool eraseEfi) =>
        HoldsEfi(d) && !eraseEfi
            ? $"disk {d.Number} holds an EFI (an EFI system partition or OpenCore), and erasing it was not confirmed: nothing written"
            : null;

    /// The FAT32 partition's size: the whole disk up to 16 GiB. Windows formats FAT32 only up to 32 GB, and the
    /// recovery image and an EFI need about 1.2 GB; Dortania's Windows guide asks for a stick of at least 4 GB.
    public const ulong MaxPartition = 16UL << 30;

    /// The smallest disk that takes the image, an EFI and room for the file system: the image plus 256 MB.
    public static ulong MinimumFor(long imageSize) => (ulong)Math.Max(0, imageSize) + (256UL << 20);
}
