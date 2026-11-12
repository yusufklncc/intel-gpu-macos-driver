// SPDX-License-Identifier: MIT
// The EFI page's boot logging section without the page: its state from the status, which change it offers, and the
// on/off flow through the planner the page uses, on the stage 1 sample (whose Misc > Debug logs to a file already, with
// DisplayLevel 0x80000002: partly on). Off is its own change, not undo: with another change made after boot logging,
// undo would take back that one instead.
using IntelXe.Core.Efi;
using IntelXe.Core.Localisation;
using IntelXeEfi;

namespace IntelXe.Core.Tests;

public sealed class BootLoggingTests : IDisposable
{
    readonly string root = Path.Combine(Path.GetTempPath(), "ixebootlog-" + Guid.NewGuid().ToString("N")[..8]);
    string Efi => Path.Combine(root, "efi");
    string Config => Path.Combine(Efi, "EFI", "OC", "config.plist");
    EfiOptions Options => new("", "", "", "Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz", false, Path.Combine(root, "backups"));
    readonly FolderEfiPlanner planner = new();
    EfiTarget Target => new EfiTarget.Folder(Efi);

    public BootLoggingTests()
    {
        Directory.CreateDirectory(Path.Combine(Efi, "EFI", "OC"));
        File.WriteAllBytes(Path.Combine(Efi, "EFI", "OC", "OpenCore.efi"), []);
        File.Copy(Paths.RepoFile("windows", "test", "stage1", "config.plist"), Config);
    }

    public void Dispose() { if (Directory.Exists(root)) Util.Delete(root); }

    static EfiStatus Status(string? value) =>
        new(true, value is null ? new Dictionary<string, string>() : new Dictionary<string, string> { ["boot_logging"] = value });

    [Fact]
    public void TheStateFromTheStatus()
    {
        Assert.Equal(BootLogState.On, BootLogging.State(Status("on")));
        Assert.Equal(BootLogState.Off, BootLogging.State(Status("off")));
        Assert.Equal(BootLogState.Partial, BootLogging.State(Status("partial")));
        Assert.Equal(BootLogState.Missing, BootLogging.State(Status("missing")));
        // not read, unreadable (the engine gives ""), or a value from a newer engine
        Assert.Equal(BootLogState.Unknown, BootLogging.State(null));
        Assert.Equal(BootLogState.Unknown, BootLogging.State(Status(null)));
        Assert.Equal(BootLogState.Unknown, BootLogging.State(Status("")));
        Assert.Equal(BootLogState.Unknown, BootLogging.State(Status("sometimes")));
    }

    [Fact]
    public void WhichChangeIsOffered()
    {
        Assert.True(BootLogging.CanTurnOn(BootLogState.Off) && !BootLogging.CanTurnOff(BootLogState.Off));
        Assert.True(!BootLogging.CanTurnOn(BootLogState.On) && BootLogging.CanTurnOff(BootLogState.On));
        Assert.True(BootLogging.CanTurnOn(BootLogState.Partial) && BootLogging.CanTurnOff(BootLogState.Partial));
        Assert.True(!BootLogging.CanTurnOn(BootLogState.Missing) && !BootLogging.CanTurnOff(BootLogState.Missing));
        // the EFI not read yet: both, and the plan says what each would do
        Assert.True(BootLogging.CanTurnOn(BootLogState.Unknown) && BootLogging.CanTurnOff(BootLogState.Unknown));
    }

    [Fact]
    public void BothStringTablesHaveTheStates()
    {
        Assert.Equal(5, BootLogging.Keys.Distinct().Count());
        foreach (var lang in new[] { "en-US", "tr-TR" })
        {
            var table = ReswFile.Read(Paths.AppFile("Strings", lang, "Resources.resw"));
            Assert.DoesNotContain(BootLogging.Keys, k => !table.ContainsKey(k));
        }
    }

    [Fact]
    public void TheChangesKinds()
    {
        Assert.Equal("bootlog-off", EfiChange.BootLogOff.Kind());
        Assert.Equal(EfiChange.BootLogOff, EfiChanges.FromKind("bootlog-off"));
        Assert.Equal(EfiChange.BootLog, EfiChanges.FromKind("bootlog"));
        // and through the elevated helper's command line (an EFI partition)
        var q = new EfiHelperRequest(EfiHelperVerb.Apply, new EfiTarget.Partition(1, 1), EfiChange.BootLogOff, Options, "S:", new string('0', 64));
        Assert.Equal(EfiChange.BootLogOff, EfiHelperRequest.Parse(q.ToArgs()).Change);
    }

    async Task<BootLogState> State() => BootLogging.State(await planner.ReadStatusAsync(Target, Options));

    async Task<EfiPlan> Make(EfiChange change)
    {
        var plan = await planner.PlanAsync(Target, change, Options);
        Assert.True(plan.HasChanges, change + ": " + plan.Text);
        EngineTextTests.AssertKnown(plan, await planner.ApplyAsync(plan, Options));
        return plan;
    }

    string Debug(string key) => Plist.Raw(Plist.Load(Config).Get("Misc", "Debug", key));

    [Fact]
    public async Task OnThenOff()
    {
        // the sample: the file bit, AppleDebug and ApplePanic on, DisplayLevel without DEBUG_INFO
        Assert.Equal(BootLogState.Partial, await State());
        var on = await Make(EfiChange.BootLog);
        Assert.Contains(on.Notes, n => n.StartsWith("boot logging slows every boot", StringComparison.Ordinal));
        Assert.Equal(BootLogState.On, await State());
        Assert.False((await planner.PlanAsync(Target, EfiChange.BootLog, Options)).HasChanges);

        var off = await Make(EfiChange.BootLogOff);
        Assert.Empty(off.Notes);
        Assert.Equal(BootLogState.Off, await State());
        Assert.Equal(("3", "false", "false", "2147483714"), (Debug("Target"), Debug("AppleDebug"), Debug("ApplePanic"), Debug("DisplayLevel")));
        Assert.Equal("boot logging off", (await planner.ReadStatusAsync(Target, Options))["backup_kind"]);
        Assert.False((await planner.PlanAsync(Target, EfiChange.BootLogOff, Options)).HasChanges);

        // the off change has its own backup: undo puts boot logging back on
        await Make(EfiChange.Undo);
        Assert.Equal(BootLogState.On, await State());
    }

    [Fact]
    public async Task OffIsNotUndoWhenAnotherChangeCameAfter()
    {
        await Make(EfiChange.BootLog);
        await Make(EfiChange.Settings);   // stage 2 after the logs were turned on
        var status = await planner.ReadStatusAsync(Target, Options);
        Assert.Equal(("on", "0", "settings"), (status["boot_logging"], status["settings_changes"], status["backup_kind"]));
        // undo would take back stage 2, not boot logging
        var undo = await planner.PlanAsync(Target, EfiChange.Undo, Options);
        Assert.Contains("(settings, ", undo.Lines[0]);
        // the off change leaves stage 2 as it is
        await Make(EfiChange.BootLogOff);
        status = await planner.ReadStatusAsync(Target, Options);
        Assert.Equal(("off", "0"), (status["boot_logging"], status["settings_changes"]));
    }

    [Fact]
    public async Task NoMiscDebug()
    {
        var pl = Plist.Load(Config);
        pl.Remove(["Misc", "Debug"]);
        File.WriteAllBytes(Config, pl.ToBytes());
        Assert.Equal(BootLogState.Missing, await State());
        foreach (var c in new[] { EfiChange.BootLog, EfiChange.BootLogOff })
        {
            var plan = await planner.PlanAsync(Target, c, Options);
            Assert.False(plan.HasChanges);
            Assert.Equal(["config.plist has no Misc > Debug: not changed"], plan.Notes);
        }
    }
}
