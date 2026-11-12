// SPDX-License-Identifier: MIT
using IntelXe.Core;
using IntelXe.Core.Efi;

namespace IntelXe.Core.Tests;

public class ChecksTests
{
    static PcFacts TestMachine() => new()
    {
        Gpus = [new GpuInfo("Intel(R) UHD Graphics 770", PciId.Parse(@"PCI\VEN_8086&DEV_A780")),
                new GpuInfo("Intel(R) Arc(TM) B580 Graphics", PciId.Parse(@"PCI\VEN_8086&DEV_E20B&SUBSYS_10208086&REV_00"))],
        CpuName = "Intel(R) Core(TM) i7-14700K", PerformanceCores = 8, EfficiencyCores = 12,
        BoardMaker = "Board maker", BoardModel = "Z790 board",
        Firmware = FirmwareMode.Uefi, SecureBoot = SecureBootState.Off,
        Bar = new BarSummary([new MemoryRange(0x40_0000_0000, 16UL << 30)]),
        Windows = new WindowsInfo("Windows 10 Pro", "25H2", 26200, 7171),
        BitLocker = BitLockerState.Off,
        Esps = [new EspPartition("Disk A", "NVMe", 0, 1, 104_857_600, "", true, OpenCoreState.NeedsMount)],
    };

    static Check Get(IReadOnlyList<Check> list, string id) => list.Single(c => c.Id == id);

    [Fact]
    public void TheTestMachineMeetsTheRequirements()
    {
        var list = PcChecks.Evaluate(TestMachine());
        Assert.Equal(CheckState.Good, PcChecks.Overall(list));
        Assert.Equal("Intel Arc B580 (8086:e20b)", Get(list, "card").Value.Verbatim);
        Assert.Equal("HybridPM_Needed", Get(list, "hybridpm").Value.Key);
        Assert.Equal("Rebar_On", Get(list, "rebar").Value.Key);
        Assert.Equal("Esp_CountUnread", Get(list, "esp").Value.Key);
    }

    [Fact]
    public void NoSupportedCardIsAProblem()
    {
        var f = TestMachine();
        f.Gpus.RemoveAt(1);
        var card = Get(PcChecks.Evaluate(f), "card");
        Assert.Equal(CheckState.Bad, card.State);
        Assert.Equal("Card_Help_Other", card.Help.Key);
        f.Gpus.Clear();
        Assert.Equal("Card_Help_None", Get(PcChecks.Evaluate(f), "card").Help.Key);
    }

    [Fact]
    public void TheB570IsAWarning()
    {
        var f = TestMachine();
        f.Gpus[1] = new GpuInfo("Intel(R) Arc(TM) B570 Graphics", new PciId(0x8086, 0xE20C));
        Assert.Equal(CheckState.Warning, Get(PcChecks.Evaluate(f), "card").State);
    }

    [Fact]
    public void FirmwareLimitsComeFromTheGuide()
    {
        var f = TestMachine();
        f.Firmware = FirmwareMode.Legacy; f.SecureBoot = SecureBootState.On; f.BitLocker = BitLockerState.On;
        f.Bar = new BarSummary([new MemoryRange(0x8000_0000, 256UL << 20)]);
        var list = PcChecks.Evaluate(f);
        Assert.Equal(CheckState.Bad, Get(list, "firmware").State);
        Assert.Equal(CheckState.Warning, Get(list, "secureboot").State);
        Assert.Equal(CheckState.Warning, Get(list, "rebar").State);
        Assert.Equal(CheckState.Warning, Get(list, "bitlocker").State);
        Assert.Equal(CheckState.Bad, PcChecks.Overall(list));
    }

    [Fact]
    public void UnknownsAreNeutral()
    {
        var list = PcChecks.Evaluate(new PcFacts { Gpus = [new GpuInfo("B580", new PciId(0x8086, 0xE20B))] });
        Assert.All(list.Where(c => c.Id != "card"), c => Assert.Equal(CheckState.Neutral, c.State));
    }

    [Fact]
    public void AmdIsAWarning()
    {
        var f = TestMachine();
        f.CpuName = "AMD Ryzen 7 7800X3D 8-Core Processor";
        var list = PcChecks.Evaluate(f);
        Assert.Equal(CheckState.Warning, Get(list, "cpu").State);
        Assert.Equal("HybridPM_NotNeeded", Get(list, "hybridpm").Value.Key);
    }

    [Fact]
    public void EveryKeyTheChecksUseIsListed()
    {
        // all states, so that every branch runs
        var keys = new HashSet<string>();
        void Collect(PcFacts f)
        {
            void Add(Text t)
            {
                if (t.Key is not null) keys.Add(t.Key);
                foreach (var a in t.Args) if (a is Text inner) Add(inner);
            }
            foreach (var c in PcChecks.Evaluate(f))
                foreach (var t in new[] { c.Title, c.Value, c.Help }) Add(t);
        }
        foreach (var fw in Enum.GetValues<FirmwareMode>())
        foreach (var sb in Enum.GetValues<SecureBootState>())
        foreach (var bl in Enum.GetValues<BitLockerState>())
        {
            var f = TestMachine(); f.Firmware = fw; f.SecureBoot = sb; f.BitLocker = bl;
            Collect(f);
        }
        var g = TestMachine(); g.Gpus.Clear(); g.CpuName = ""; g.Bar = null; g.Windows = null; g.BoardModel = ""; g.Esps.Clear(); Collect(g);
        g = TestMachine(); g.Gpus.RemoveAt(1); g.CpuName = "AMD Ryzen 9 7950X"; g.Bar = new BarSummary([new MemoryRange(0, 256UL << 20)]); Collect(g);
        g = TestMachine(); g.CpuName = "Some CPU"; g.Esps[0] = g.Esps[0] with { OpenCore = OpenCoreState.Found }; Collect(g);
        g = TestMachine(); g.Gpus[1] = g.Gpus[1] with { Id = new PciId(0x8086, 0xE20C) }; g.CpuName = "Intel(R) Core(TM) i9-9900K"; g.PerformanceCores = null;
        g.Esps[0] = g.Esps[0] with { OpenCore = OpenCoreState.NotFound }; Collect(g);
        Assert.Empty(keys.Except(PcChecks.Keys));
    }

    [Fact]
    public async Task ThePendingPlannerWritesNothing()
    {
        IEfiPlanner p = new PendingEfiPlanner();
        Assert.False(p.IsAvailable);
        var status = await p.ReadStatusAsync(new EfiTarget.Folder(@"E:\EFI"));
        Assert.False(status.Readable);
        var e = await Assert.ThrowsAsync<EfiUnavailableException>(() => p.PlanAsync(new EfiTarget.Partition(0, 1), EfiChange.Settings));
        Assert.Contains("shared rules", e.Message);
        await Assert.ThrowsAsync<EfiUnavailableException>(() => p.ApplyAsync(new EfiTarget.Partition(0, 1), EfiChange.Driver, "hash"));
    }
}
