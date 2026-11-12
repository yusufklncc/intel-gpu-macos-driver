// SPDX-License-Identifier: MIT
namespace IntelXe.Core.Efi;

/// The changes the EFI page offers: stage 2 (the settings) and stage 4 (the driver's files) of the install guide, and
/// undo. The same three as IntelXe-EFI.ps1 -Plan settings|driver|undo and efi.sh.
public enum EfiChange { Settings, Driver, Undo }

/// The EFI to work on: an EFI folder (a USB stick's, or a copy), or an EFI system partition by disk and partition number
/// (mounted by the planner when it needs to read it).
public abstract record EfiTarget
{
    public sealed record Folder(string Path) : EfiTarget;
    public sealed record Partition(uint DiskNumber, uint PartitionNumber) : EfiTarget;
}

/// What a planner read from an EFI: "key=value" facts as IntelXe-EFI.ps1 -Status and efi.sh --status print them
/// (opencore, kext_file, kext_entry, hybridpm_needed, ...), so that the page can show them before the rules are wired.
public sealed record EfiStatus(bool Readable, IReadOnlyDictionary<string, string> Values, string? Message = null);

/// A plan: the lines it would change, its notes, and the SHA-256 that ApplyAsync must be given back, so that nothing is
/// written when the EFI changed between the plan the user read and the confirmation.
public sealed record EfiPlan(EfiChange Change, IReadOnlyList<string> Lines, IReadOnlyList<string> Notes, string Hash)
{
    public bool HasChanges => Lines.Count > 0;
}

/// The outcome of a planner call that could not be done, with a message for the user.
public sealed class EfiUnavailableException(string message) : Exception(message);

/// The seam between the EFI page and the EFI rules. The page only calls this interface; the implementation that reads
/// and writes config.plist will come from the shared rules file (one JSON with test vectors for efi.sh,
/// IntelXe-EFI.ps1 and this app). Until then PendingEfiPlanner answers every call.
public interface IEfiPlanner
{
    /// True when this planner can read and change an EFI.
    bool IsAvailable { get; }

    /// The version of the shared rules it runs, or a short note on why there is none.
    string RulesVersion { get; }

    Task<EfiStatus> ReadStatusAsync(EfiTarget target, CancellationToken cancel = default);

    /// The plan of one change; nothing is written.
    Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, CancellationToken cancel = default);

    /// Backs up the whole EFI folder and makes the change, only when the EFI still gives the plan with this hash.
    Task ApplyAsync(EfiTarget target, EfiChange change, string expectedPlanHash, CancellationToken cancel = default);
}

/// The planner until the shared rules are wired in: it reads and writes nothing and says why.
public sealed class PendingEfiPlanner : IEfiPlanner
{
    /// The message every call gives (a key in the app's string tables).
    public const string MessageKey = "Efi_Pending";
    public const string Message = "This comes with the shared rules: the EFI changes are being moved into one rules file that efi.sh, IntelXe-EFI.ps1 and this app will share.";

    public bool IsAvailable => false;
    public string RulesVersion => "not wired yet";

    public Task<EfiStatus> ReadStatusAsync(EfiTarget target, CancellationToken cancel = default) =>
        Task.FromResult(new EfiStatus(false, new Dictionary<string, string>(), Message));

    public Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, CancellationToken cancel = default) =>
        Task.FromException<EfiPlan>(new EfiUnavailableException(Message));

    public Task ApplyAsync(EfiTarget target, EfiChange change, string expectedPlanHash, CancellationToken cancel = default) =>
        Task.FromException(new EfiUnavailableException(Message));
}
