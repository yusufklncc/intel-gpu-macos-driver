// SPDX-License-Identifier: MIT
using IntelXeEfi;

namespace IntelXe.Core.Efi;

/// Status, plan and change of one EFI folder through the engine of IntelXe-EFI.ps1 (EfiFolder): the same lines, notes,
/// plan hash, backups and undo as the script, because it is the same code. No Windows APIs; the folder may be on a USB
/// stick or on an EFI partition mounted by the caller.
public static class EfiWork
{
    /// The engine on this EFI folder with these options (EfiFolder takes the EFI folder, the folder or drive above it).
    public static EfiFolder Open(string efiPath, EfiOptions o, Rules? rules = null)
    {
        var f = new EfiFolder(efiPath, o.PackageDir, o.BackupRoot ?? "", rules ?? EfiRules.Embedded);
        if (o.CardAcpiPath.Trim().Length > 0) f.SetCard(o.CardAcpiPath);
        f.CardDetected = o.CardDetected;
        f.Cpu = o.Cpu;
        f.BootPathRecorded = o.BootPathRecorded;
        f.ProbeDevice = o.ProbeDevice;
        return f;
    }

    public static EfiStatus Status(string efiPath, EfiOptions o, Rules? rules = null)
    {
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var line in Open(efiPath, o, rules).Status())
        {
            int i = line.IndexOf('=');
            if (i > 0) values[line[..i]] = line[(i + 1)..];
        }
        return new EfiStatus(values.GetValueOrDefault("efi_readable") == "yes", values);
    }

    public static EfiPlan Plan(EfiTarget target, string efiPath, EfiChange change, EfiOptions o, Rules? rules = null, string? letter = null)
    {
        var f = Open(efiPath, o, rules);
        return From(target, change, f.MakePlan(change.Kind()), f.BackupDir, letter);
    }

    public static EfiPlan From(EfiTarget target, EfiChange change, Plan p, string backupDir, string? letter = null) =>
        new(target, change, p.Text, p.Lines.ToArray(), p.Notes.ToArray(), p.Hash, p.HasChanges, backupDir, letter);

    /// The change (undo too) when the plan is still the one with this hash: EfiFolder.Run, which backs up the whole EFI
    /// folder first and refuses a package whose files differ from its SHA256SUMS.
    public static EfiResult Apply(string efiPath, EfiChange change, string expectedHash, EfiOptions o, Rules? rules = null)
    {
        var lines = new List<string>();
        Open(efiPath, o, rules).Run(change.Kind(), expectedHash, lines.Add);
        return new EfiResult(lines);
    }
}

/// The planner for EFI folders, in this process: a USB stick's EFI folder or a copy needs no administrator rights. An EFI
/// partition needs mounting, which the app's elevated helper does (EfiHelper).
public sealed class FolderEfiPlanner(Rules? rules = null) : IEfiPlanner
{
    readonly Rules rules = rules ?? EfiRules.Embedded;

    public bool IsAvailable => true;
    public string RulesVersion => "efi-rules " + EfiRules.Revision(rules);

    static string Folder(EfiTarget t) => t is EfiTarget.Folder f
        ? f.Path
        : throw new EfiUnavailableException("an EFI partition is mounted by the elevated helper, not in this process");

    public Task<EfiStatus> ReadStatusAsync(EfiTarget target, EfiOptions options, CancellationToken cancel = default) =>
        Task.Run(() => EfiWork.Status(Folder(target), options, rules), cancel);

    public Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, EfiOptions options, CancellationToken cancel = default) =>
        Task.Run(() => EfiWork.Plan(target, Folder(target), change, options, rules), cancel);

    public Task<EfiResult> ApplyAsync(EfiPlan plan, EfiOptions options, CancellationToken cancel = default) =>
        Task.Run(() => EfiWork.Apply(Folder(plan.Target), plan.Change, plan.Hash, options, rules), cancel);
}
