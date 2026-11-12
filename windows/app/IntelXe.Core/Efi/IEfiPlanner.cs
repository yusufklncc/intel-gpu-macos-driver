// SPDX-License-Identifier: MIT
namespace IntelXe.Core.Efi;

/// The changes the EFI page offers: stage 2 (the settings), the Safari setting (alone or with stage 2, one backup and one
/// restart), stage 4 (the driver's files) and undo. The same as IntelXe-EFI.ps1 -Plan settings|safari|settings-safari|
/// driver|undo and efi.sh.
public enum EfiChange { Settings, Safari, SettingsSafari, Driver, Undo }

public static class EfiChanges
{
    /// The plan's name in the engine, the script and efi-rules.json's backup_kinds.
    public static string Kind(this EfiChange c) => c switch
    {
        EfiChange.Settings => "settings",
        EfiChange.Safari => "safari",
        EfiChange.SettingsSafari => "settings-safari",
        EfiChange.Driver => "driver",
        _ => "undo",
    };

    public static EfiChange? FromKind(string? kind) => kind switch
    {
        "settings" => EfiChange.Settings,
        "safari" => EfiChange.Safari,
        "settings-safari" => EfiChange.SettingsSafari,
        "driver" => EfiChange.Driver,
        "undo" => EfiChange.Undo,
        _ => null,
    };
}

/// The EFI to work on: an EFI folder (a USB stick's, or a copy), or an EFI system partition by disk and partition number
/// (mounted for each read or change, by the elevated helper).
public abstract record EfiTarget
{
    public sealed record Folder(string Path) : EfiTarget;
    public sealed record Partition(uint DiskNumber, uint PartitionNumber) : EfiTarget;
}

/// What a plan or a change takes besides the EFI: the package (the unpacked -efi.zip; stage 2 does not need one), the
/// card's ACPI path as the user confirmed it ("" when not confirmed: no SSDT-ARCOFF.aml; "none" for a port without an
/// ACPI name), what Windows reports for the card (for the notes only), the CPU's name (HybridPM.kext), whether OpenCore
/// records its boot path (Windows cannot tell: false), and another backup folder (null: where IntelXe-EFI.ps1 puts them).
public sealed record EfiOptions(
    string PackageDir = "",
    string CardAcpiPath = "",
    string CardDetected = "",
    string Cpu = "",
    bool BootPathRecorded = false,
    string? BackupRoot = null);

/// What a planner read from an EFI: "key=value" facts as IntelXe-EFI.ps1 -Status and efi.sh --status print them
/// (opencore, kext_file, kext_entry, hybridpm_needed, ...).
public sealed record EfiStatus(bool Readable, IReadOnlyDictionary<string, string> Values, string? Message = null)
{
    public string this[string key] => Values.TryGetValue(key, out var v) ? v : "";
}

/// A plan: the text the engine shows (its "EFI:" line, the steps, the notes), its steps and notes, and its SHA-256, which
/// a change must be given back so that nothing is written when the EFI changed between the plan the user read and the
/// confirmation. Letter: the drive letter an EFI partition was mounted at for the plan (the plan names it, so the change
/// mounts it there again); BackupDir: where the backups of this EFI go.
public sealed record EfiPlan(EfiTarget Target, EfiChange Change, string Text, IReadOnlyList<string> Lines, IReadOnlyList<string> Notes,
    string Hash, bool HasChanges, string BackupDir, string? Letter = null);

/// What a change did: the engine's lines (the plan again, each file written, where the backup is).
public sealed record EfiResult(IReadOnlyList<string> Lines);

/// The outcome of a planner call that could not be done, with a message for the user.
public sealed class EfiUnavailableException(string message) : Exception(message);

/// The seam between the EFI page and the EFI rules. The page only calls this interface.
public interface IEfiPlanner
{
    /// True when this planner can read and change an EFI.
    bool IsAvailable { get; }

    /// The version of the shared rules it runs, or a short note on why there is none.
    string RulesVersion { get; }

    Task<EfiStatus> ReadStatusAsync(EfiTarget target, EfiOptions options, CancellationToken cancel = default);

    /// The plan of one change; nothing is written.
    Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, EfiOptions options, CancellationToken cancel = default);

    /// Backs up the whole EFI folder and makes the change (undo: puts the newest backup back), only when the EFI still
    /// gives a plan with the hash of the one shown.
    Task<EfiResult> ApplyAsync(EfiPlan plan, EfiOptions options, CancellationToken cancel = default);
}

/// A planner that has nothing to work with and says why (the page shows the message).
public sealed class PendingEfiPlanner(string? why = null) : IEfiPlanner
{
    /// The message every call gives (a key in the app's string tables).
    public const string MessageKey = "Efi_Pending";

    public string Message { get; } = why ?? "no EFI rules";
    public bool IsAvailable => false;
    public string RulesVersion => "none";

    public Task<EfiStatus> ReadStatusAsync(EfiTarget target, EfiOptions options, CancellationToken cancel = default) =>
        Task.FromResult(new EfiStatus(false, new Dictionary<string, string>(), Message));

    public Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, EfiOptions options, CancellationToken cancel = default) =>
        Task.FromException<EfiPlan>(new EfiUnavailableException(Message));

    public Task<EfiResult> ApplyAsync(EfiPlan plan, EfiOptions options, CancellationToken cancel = default) =>
        Task.FromException<EfiResult>(new EfiUnavailableException(Message));
}
