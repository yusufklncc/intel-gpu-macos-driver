// SPDX-License-Identifier: MIT
// The "Set up" page's guided flow from a Windows PC to a macOS install (the steps of release/README-INSTALL.md, in the
// app): 1. this PC, 2. the firmware (BIOS) settings, 3. the OpenCore EFI built for this PC, 4. the macOS installer USB
// stick with that EFI, 5. after macOS is installed (stage 3 on macOS, stage 4 from either side). Each step is done, the
// next one, or blocked (by an earlier step or by its own reason); a done step stays done. What the user did is kept in
// the app's settings file (SetupProgress); everything else is read again each time (this PC's facts, whether the built
// EFI is still there). The wizard itself changes nothing on disk: the destructive steps stay on their pages, behind
// their own confirmations.
namespace IntelXe.Core.Setup;

public enum SetupStep { ThisPc, Bios, BuildEfi, Stick, AfterInstall }

public enum StepState { Done, Next, Blocked }

/// One step as the page shows it: its state and, when blocked, why (a Text the app translates).
public sealed record StepStatus(SetupStep Step, StepState State, Text Reason)
{
    public bool IsDone => State == StepState.Done;
}

/// What the user did, kept in settings.json ("Setup"). Plain properties for System.Text.Json.
public sealed class SetupProgress
{
    /// The user confirmed the BIOS steps ("I did these"), and when (ISO 8601).
    public string BiosConfirmed { get; set; } = "";

    /// The last EFI built on the Build page: its output folder (holding EFI\), the macOS version it is for, when.
    public string EfiOutDir { get; set; } = "";
    public string EfiMacOS { get; set; } = "";
    public string EfiBuilt { get; set; } = "";

    /// The installer stick was written with the EFI built at EfiBuilt (the stick page records it; "" for none), or the
    /// user said they made the stick another way ("manual").
    public string StickWith { get; set; } = "";

    /// The user marked the last step done.
    public string AfterInstallDone { get; set; } = "";

    /// The folder of the last scan of this PC (Report.json, ACPI\), for a build after the app was closed. Not a step.
    public string LastScan { get; set; } = "";

    public SetupProgress Clone() => (SetupProgress)MemberwiseClone();
}

/// What the wizard reads besides the progress: whether this PC's facts were read, the problems that stop the whole
/// install (no supported card, a CPU the builder refuses), whether the builder is there, whether the built EFI is still
/// on disk.
public sealed record SetupInputs(bool FactsRead, IReadOnlyList<Text> Blocking, bool BuilderAvailable, Text BuilderMissing, bool BuiltEfiExists);

public static class SetupWizard
{
    public const string Manual = "manual";

    /// Every string key the wizard can give the page, for the tests that hold the string tables to them.
    public static IReadOnlyList<string> Keys { get; } = ["Setup_Blocked_Waiting", "Setup_Blocked_NoBuilder", "Setup_Block_Card", "Setup_Block_Cpu"];

    public static readonly SetupStep[] Steps = [SetupStep.ThisPc, SetupStep.Bios, SetupStep.BuildEfi, SetupStep.Stick, SetupStep.AfterInstall];

    /// Whether a step's own record says it is done.
    static bool OwnDone(SetupStep step, SetupProgress p, SetupInputs i) => step switch
    {
        SetupStep.ThisPc => i.FactsRead && i.Blocking.Count == 0,
        SetupStep.Bios => p.BiosConfirmed.Length > 0,
        SetupStep.BuildEfi => p.EfiBuilt.Length > 0 && i.BuiltEfiExists,
        SetupStep.Stick => p.StickWith.Length > 0 && (p.StickWith == Manual || p.StickWith == p.EfiBuilt),
        _ => p.AfterInstallDone.Length > 0,
    };

    /// Why a step cannot be done now on its own account (empty when nothing stops it).
    static Text OwnBlock(SetupStep step, SetupInputs i) => step switch
    {
        SetupStep.ThisPc when i.Blocking.Count > 0 => i.Blocking[0],
        SetupStep.BuildEfi when !i.BuilderAvailable => i.BuilderMissing.IsEmpty ? Text.Of("Setup_Blocked_NoBuilder") : i.BuilderMissing,
        _ => Text.Empty,
    };

    /// Every step's state: done steps stay done; the first step that is not done is next (or blocked by its own reason);
    /// every later one that is not done is blocked by the first one.
    public static IReadOnlyList<StepStatus> Evaluate(SetupProgress p, SetupInputs i)
    {
        var list = new List<StepStatus>();
        SetupStep? first = null;
        foreach (var s in Steps)
        {
            if (OwnDone(s, p, i)) { list.Add(new StepStatus(s, StepState.Done, Text.Empty)); continue; }
            if (first is null)
            {
                first = s;
                var own = OwnBlock(s, i);
                list.Add(own.IsEmpty ? new StepStatus(s, StepState.Next, Text.Empty) : new StepStatus(s, StepState.Blocked, own));
                continue;
            }
            var block = OwnBlock(s, i);
            list.Add(new StepStatus(s, StepState.Blocked, block.IsEmpty ? Text.Of("Setup_Blocked_Waiting", (int)first.Value + 1) : block));
        }
        return list;
    }

    /// The step the page opens on: the first one not done (the last step when all are).
    public static SetupStep Current(IReadOnlyList<StepStatus> states) =>
        states.FirstOrDefault(s => !s.IsDone)?.Step ?? SetupStep.AfterInstall;

    /// Whether the user may mark a step done (or record it) now: every step before it is done.
    public static bool CanAct(IReadOnlyList<StepStatus> states, SetupStep step) =>
        states.TakeWhile(s => s.Step != step).All(s => s.IsDone) && states.First(s => s.Step == step).State != StepState.Blocked;

    // ---- what the user does ----

    public static SetupProgress ConfirmBios(SetupProgress p, DateTime now)
    {
        var n = p.Clone();
        n.BiosConfirmed = Iso(now);
        return n;
    }

    /// A new EFI was built: the stick made with an earlier one no longer counts (its EFI is not this one), unless the
    /// user made the stick another way.
    public static SetupProgress RecordEfi(SetupProgress p, string outDir, string macOS, DateTime now)
    {
        var n = p.Clone();
        n.EfiOutDir = outDir;
        n.EfiMacOS = macOS;
        n.EfiBuilt = Iso(now);
        if (n.StickWith != Manual) n.StickWith = "";
        return n;
    }

    /// The stick page wrote a stick with an EFI folder: it counts when that folder is the built EFI.
    public static SetupProgress RecordStick(SetupProgress p, string? efiWritten, string builtEfiDir)
    {
        if (string.IsNullOrEmpty(efiWritten) || p.EfiBuilt.Length == 0 || builtEfiDir.Length == 0) return p;
        if (!string.Equals(Normal(efiWritten), Normal(builtEfiDir), StringComparison.OrdinalIgnoreCase)) return p;
        var n = p.Clone();
        n.StickWith = p.EfiBuilt;
        return n;
    }

    /// "I made the stick myself": the step counts as done whatever EFI it carries.
    public static SetupProgress MarkStickManual(SetupProgress p)
    {
        var n = p.Clone();
        n.StickWith = Manual;
        return n;
    }

    public static SetupProgress MarkAfterInstall(SetupProgress p, DateTime now)
    {
        var n = p.Clone();
        n.AfterInstallDone = Iso(now);
        return n;
    }

    /// Undoes one step's record and every later one's (the page's "Do this step again").
    public static SetupProgress Reopen(SetupProgress p, SetupStep step)
    {
        var n = p.Clone();
        if (step <= SetupStep.Bios) n.BiosConfirmed = "";
        if (step <= SetupStep.BuildEfi) { n.EfiOutDir = ""; n.EfiMacOS = ""; n.EfiBuilt = ""; }
        if (step <= SetupStep.Stick) n.StickWith = "";
        n.AfterInstallDone = "";
        return n;
    }

    static string Iso(DateTime t) => t.ToString("yyyy-MM-dd'T'HH:mm:ss", System.Globalization.CultureInfo.InvariantCulture);

    static string Normal(string path) => Path.GetFullPath(path).TrimEnd(Path.DirectorySeparatorChar, Path.AltDirectorySeparatorChar);

    /// The facts that stop the install before anything else: no supported card (the "card" check is a problem), or a CPU
    /// that is not Intel (the builder makes EFIs for Intel CPUs only, as the driver is tested on Intel only).
    public static IReadOnlyList<Text> Blocking(PcFacts? facts, IReadOnlyList<Check> checks)
    {
        var list = new List<Text>();
        if (checks.FirstOrDefault(c => c.Id == "card") is { State: CheckState.Bad }) list.Add(Text.Of("Setup_Block_Card"));
        if (facts is not null && CpuInfo.CleanName(facts.CpuName) != "" && CpuInfo.Family(facts.CpuName).Vendor != CpuVendor.Intel)
            list.Add(Text.Of("Setup_Block_Cpu"));
        return list;
    }

    /// The Overview's checks for step 1: problems first, then warnings, then the rest, each in the Overview's order.
    public static IReadOnlyList<Check> Ordered(IReadOnlyList<Check> checks) =>
        checks.Select((c, i) => (c, i)).OrderBy(x => x.c.State switch { CheckState.Bad => 0, CheckState.Warning => 1, _ => 2 }).ThenBy(x => x.i).Select(x => x.c).ToList();
}
