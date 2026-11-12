// SPDX-License-Identifier: MIT
// The "Set up" page's steps (Core/Setup/SetupWizard.cs): done / next / blocked from the progress kept in settings.json and
// what the app reads each time; what each user action records; what blocks the whole install.
using System.Text.Json;
using IntelXe.Core.Setup;

namespace IntelXe.Core.Tests;

public class SetupWizardTests
{
    static readonly DateTime Now = new(2026, 11, 13, 9, 30, 0);

    static SetupInputs Inputs(bool facts = true, bool builder = true, bool efiExists = true, params Text[] blocking) =>
        new(facts, blocking, builder, builder ? Text.Empty : Text.Raw("no builder folder"), efiExists);

    static StepState[] States(SetupProgress p, SetupInputs i) => SetupWizard.Evaluate(p, i).Select(s => s.State).ToArray();

    const StepState D = StepState.Done, N = StepState.Next, B = StepState.Blocked;

    [Fact]
    public void AtTheStartOnlyThisPcIsNext()
    {
        var s = SetupWizard.Evaluate(new SetupProgress(), Inputs(facts: false));
        Assert.Equal([N, B, B, B, B], s.Select(x => x.State));
        Assert.Equal(Text.Of("Setup_Blocked_Waiting", 1).ToString(), s[1].Reason.ToString());
        Assert.Equal(SetupStep.ThisPc, SetupWizard.Current(s));
    }

    [Fact]
    public void WithTheFactsTheBiosStepsAreNext()
    {
        var s = SetupWizard.Evaluate(new SetupProgress(), Inputs());
        Assert.Equal([D, N, B, B, B], s.Select(x => x.State));
        Assert.Equal("Setup_Blocked_Waiting(2)", s[2].Reason.ToString());
        Assert.True(SetupWizard.CanAct(s, SetupStep.Bios));
        Assert.False(SetupWizard.CanAct(s, SetupStep.BuildEfi));
    }

    [Fact]
    public void ABlockingProblemStopsEverything()
    {
        var s = SetupWizard.Evaluate(new SetupProgress(), Inputs(blocking: Text.Of("Setup_Block_Card")));
        Assert.Equal([B, B, B, B, B], s.Select(x => x.State));
        Assert.Equal("Setup_Block_Card", s[0].Reason.Key);
        Assert.Equal("Setup_Blocked_Waiting(1)", s[4].Reason.ToString());
        Assert.False(SetupWizard.CanAct(s, SetupStep.ThisPc));
    }

    [Fact]
    public void TheWholeWay()
    {
        var p = new SetupProgress();
        p = SetupWizard.ConfirmBios(p, Now);
        Assert.Equal("2026-11-13T09:30:00", p.BiosConfirmed);
        Assert.Equal([D, D, N, B, B], States(p, Inputs()));

        p = SetupWizard.RecordEfi(p, "C:/b/1", "26", Now);
        Assert.Equal([D, D, D, N, B], States(p, Inputs()));

        p = SetupWizard.RecordStick(p, "C:/b/1/EFI", "C:/b/1/EFI/");
        Assert.Equal([D, D, D, D, N], States(p, Inputs()));

        p = SetupWizard.MarkAfterInstall(p, Now);
        var all = SetupWizard.Evaluate(p, Inputs());
        Assert.All(all, x => Assert.Equal(D, x.State));
        Assert.Equal(SetupStep.AfterInstall, SetupWizard.Current(all));
    }

    [Fact]
    public void WithoutTheBuilderTheEfiStepSaysWhy()
    {
        var p = SetupWizard.ConfirmBios(new SetupProgress(), Now);
        var s = SetupWizard.Evaluate(p, Inputs(builder: false));
        Assert.Equal([D, D, B, B, B], s.Select(x => x.State));
        Assert.Equal("no builder folder", s[2].Reason.Verbatim);
        Assert.Equal("Setup_Blocked_Waiting(3)", s[3].Reason.ToString());
        Assert.False(SetupWizard.CanAct(s, SetupStep.BuildEfi));
        Assert.Equal("Setup_Blocked_NoBuilder", SetupWizard.Evaluate(p, new SetupInputs(true, [], false, Text.Empty, false))[2].Reason.Key);
    }

    [Fact]
    public void ABuiltEfiThatIsGoneIsNotDone()
    {
        var p = SetupWizard.RecordEfi(SetupWizard.ConfirmBios(new SetupProgress(), Now), "C:/b/1", "26", Now);
        Assert.Equal([D, D, N, B, B], States(p, Inputs(efiExists: false)));
    }

    [Fact]
    public void AStickWithAnotherEfiDoesNotCount()
    {
        var p = SetupWizard.RecordEfi(SetupWizard.ConfirmBios(new SetupProgress(), Now), "C:/b/1", "26", Now);
        Assert.Same(p, SetupWizard.RecordStick(p, "D:/my/EFI", "C:/b/1/EFI"));
        Assert.Same(p, SetupWizard.RecordStick(p, null, "C:/b/1/EFI"));
        var fresh = new SetupProgress();
        Assert.Same(fresh, SetupWizard.RecordStick(fresh, "C:/b/1/EFI", "C:/b/1/EFI"));
        Assert.Equal(N, States(p, Inputs())[3]);
    }

    [Fact]
    public void ANewEfiNeedsANewStick()
    {
        var p = SetupWizard.RecordEfi(SetupWizard.ConfirmBios(new SetupProgress(), Now), "C:/b/1", "26", Now);
        p = SetupWizard.RecordStick(p, "C:/b/1/EFI", "C:/b/1/EFI");
        Assert.Equal(D, States(p, Inputs())[3]);
        p = SetupWizard.RecordEfi(p, "C:/b/2", "15", Now.AddMinutes(5));
        Assert.Equal("", p.StickWith);
        Assert.Equal(N, States(p, Inputs())[3]);
        // a stick the user made another way stays made
        p = SetupWizard.MarkStickManual(p);
        p = SetupWizard.RecordEfi(p, "C:/b/3", "26", Now.AddMinutes(9));
        Assert.Equal(SetupWizard.Manual, p.StickWith);
        Assert.Equal(D, States(p, Inputs())[3]);
    }

    [Fact]
    public void DoingAStepAgainUndoesTheLaterOnes()
    {
        var p = SetupWizard.MarkAfterInstall(SetupWizard.RecordStick(SetupWizard.RecordEfi(SetupWizard.ConfirmBios(new SetupProgress(), Now), "C:/b/1", "26", Now), "C:/b/1/EFI", "C:/b/1/EFI"), Now);
        var q = SetupWizard.Reopen(p, SetupStep.BuildEfi);
        Assert.Equal(p.BiosConfirmed, q.BiosConfirmed);
        Assert.Equal("", q.EfiBuilt);
        Assert.Equal("", q.EfiOutDir);
        Assert.Equal("", q.StickWith);
        Assert.Equal("", q.AfterInstallDone);
        Assert.Equal([D, D, N, B, B], States(q, Inputs()));
        Assert.Equal([D, N, B, B, B], States(SetupWizard.Reopen(p, SetupStep.ThisPc), Inputs()));
        Assert.Equal("C:/b/1", p.EfiOutDir);   // the old record is not changed
    }

    [Fact]
    public void TheProgressSurvivesTheSettingsFile()
    {
        var p = SetupWizard.RecordEfi(SetupWizard.ConfirmBios(new SetupProgress(), Now), @"C:\Users\x\AppData\Local\IntelXe Manager\efibuild\builds\1", "26", Now);
        var back = JsonSerializer.Deserialize<SetupProgress>(JsonSerializer.Serialize(p))!;
        Assert.Equal(p.EfiOutDir, back.EfiOutDir);
        Assert.Equal(p.BiosConfirmed, back.BiosConfirmed);
        Assert.Equal(States(p, Inputs()), States(back, Inputs()));
        // a settings file from before the wizard has no "Setup": the defaults
        Assert.Equal([D, N, B, B, B], States(JsonSerializer.Deserialize<SetupProgress>("{}")!, Inputs()));
    }

    [Fact]
    public void WhatBlocksTheInstall()
    {
        var b580 = new GpuInfo("Intel(R) Arc(TM) B580 Graphics", new PciId(0x8086, 0xE20B));
        var good = new PcFacts { Gpus = [b580], CpuName = "Intel(R) Core(TM) i7-14700K" };
        Assert.Empty(SetupWizard.Blocking(good, PcChecks.Evaluate(good)));

        var amd = new PcFacts { Gpus = [b580], CpuName = "AMD Ryzen 7 7700X 8-Core Processor" };
        Assert.Equal(["Setup_Block_Cpu"], SetupWizard.Blocking(amd, PcChecks.Evaluate(amd)).Select(t => t.Key));

        var noCard = new PcFacts { Gpus = [new GpuInfo("Intel(R) UHD Graphics 770", new PciId(0x8086, 0xA780))], CpuName = "Intel(R) Core(TM) i5-12400" };
        Assert.Equal(["Setup_Block_Card"], SetupWizard.Blocking(noCard, PcChecks.Evaluate(noCard)).Select(t => t.Key));

        // a CPU Windows did not name blocks nothing (the builder decides from its own report)
        var unknown = new PcFacts { Gpus = [b580] };
        Assert.Empty(SetupWizard.Blocking(unknown, PcChecks.Evaluate(unknown)));
    }

    [Fact]
    public void ProblemsFirst()
    {
        var f = new PcFacts { Gpus = [new GpuInfo("B580", new PciId(0x8086, 0xE20B))], CpuName = "Intel(R) Core(TM) i7-14700K",
            Firmware = FirmwareMode.Legacy, SecureBoot = SecureBootState.On };
        var checks = PcChecks.Evaluate(f);
        var ordered = SetupWizard.Ordered(checks);
        Assert.Equal(checks.Count, ordered.Count);
        Assert.Equal("firmware", ordered[0].Id);
        Assert.Equal(CheckState.Warning, ordered[1].State);
        Assert.Equal(ordered.Select(c => c.State == CheckState.Bad ? 0 : c.State == CheckState.Warning ? 1 : 2), ordered.Select(c => c.State == CheckState.Bad ? 0 : c.State == CheckState.Warning ? 1 : 2).Order());
    }
}
