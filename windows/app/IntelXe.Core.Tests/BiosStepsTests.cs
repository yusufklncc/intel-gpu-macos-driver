// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;
using IntelXe.Core.Bios;

namespace IntelXe.Core.Tests;

public class BiosStepsTests
{
    static BiosData Data => BiosSteps.Embedded;

    // ---- the file: every item complete in both languages, cited, with a path for every vendor ----

    [Fact]
    public void EveryItemIsCompleteInBothLanguages()
    {
        Assert.NotEmpty(Data.Items);
        Assert.Equal(Data.Items.Count, Data.Items.Select(i => i.Id).Distinct().Count());
        foreach (var i in Data.Items)
        {
            foreach (var t in new[] { i.Title, i.Want, i.Why }.Concat(i.Missing is null ? [] : [i.Missing]))
            {
                Assert.False(string.IsNullOrWhiteSpace(t.En), $"{i.Id}: English text missing");
                Assert.False(string.IsNullOrWhiteSpace(t.Tr), $"{i.Id}: Turkish text missing");
            }
            Assert.Contains(i.Group, Data.Groups.Select(g => g.Id));
            _ = i.Level;   // throws for an unknown level
            Assert.Contains(i.Fact, BiosSteps.Facts);
            Assert.True(i.Cpu is null or "intel", $"{i.Id}: cpu is intel or absent");
        }
        foreach (var g in Data.Groups) Assert.True(g.Title.En.Length > 0 && g.Title.Tr.Length > 0 && g.Caption.Tr.Length > 0);
        foreach (var (k, t) in Data.Seen.Concat(Data.Page)) Assert.True(t.En.Length > 0 && t.Tr.Length > 0, k);
    }

    [Fact]
    public void EveryItemCitesKnownSources()
    {
        foreach (var i in Data.Items)
        {
            Assert.NotEmpty(i.Sources);
            foreach (var s in i.Sources) Assert.True(Data.Sources.ContainsKey(s), $"{i.Id}: unknown source {s}");
            // every firmware setting rests on the guide or Dortania
            if (i.Group == "firmware")
                Assert.Contains(i.Sources, s => s.StartsWith("guide-", StringComparison.Ordinal) || s.StartsWith("dortania-", StringComparison.Ordinal));
        }
        foreach (var (id, s) in Data.Sources)
        {
            Assert.StartsWith("https://", s.Url);
            Assert.False(string.IsNullOrWhiteSpace(s.Title), id);
            Assert.True(s.Short.En.Length > 0 && s.Short.Tr.Length > 0, id);
        }
        Assert.True(Data.Sources.ContainsKey("dortania-bios-coffee"));
    }

    [Fact]
    public void GuideSourcesPointAtFilesAndAnchorsThatExist()
    {
        foreach (var (id, s) in Data.Sources.Where(kv => kv.Value.File is not null))
        {
            var path = Paths.RepoFile(s.File!.Split('/'));
            Assert.True(File.Exists(path), $"{id}: {s.File} does not exist");
            if (s.Anchor is { } a)
            {
                Assert.Contains($"id=\"{a}\"", File.ReadAllText(path));
                Assert.EndsWith("#" + a, s.Url);
            }
        }
    }

    [Fact]
    public void TheGuidesFirmwareLineIsCovered()
    {
        // site/guide/hardware.html and macos.html: Above 4G decoding on, Resizable BAR on, CSM off; Secure Boot off for
        // OpenCore; plus Dortania's Intel list
        string[] ids = ["bitlocker", "windows-uefi", "storage-ahci", "secure-boot", "csm", "above4g", "rebar", "igpu", "vtd", "cfg-lock",
            "xhci-handoff", "fast-boot", "vtx", "hyperthreading", "execute-disable", "sgx", "serial-port", "thunderbolt", "platform-trust"];
        Assert.Equal(ids.Order(), Data.Items.Select(i => i.Id).Order());
        Assert.Equal("required", Data.Items.Single(i => i.Id == "csm").LevelName);
        Assert.Equal("required", Data.Items.Single(i => i.Id == "above4g").LevelName);
        Assert.Equal("required", Data.Items.Single(i => i.Id == "rebar").LevelName);
        // ResizeAppleGpuBars = 10 is the guide's value, not Dortania's
        Assert.Contains("ResizeAppleGpuBars = 10", Data.Items.Single(i => i.Id == "rebar").Why.En);
        // the Windows traps come first
        var groups = Data.Items.Select(i => i.Group).ToList();
        Assert.Equal(groups.Count(g => g == "windows"), groups.TakeWhile(g => g == "windows").Count());
    }

    [Fact]
    public void EveryFirmwareSettingHasAPathForEachVendor()
    {
        Assert.Equal(BiosSteps.VendorIds.Order(), Data.Vendors.Select(v => v.Id).Order());
        foreach (var i in Data.Items)
        {
            Assert.True(i.Paths.ContainsKey("generic"), $"{i.Id}: no generic path");
            foreach (var p in i.Paths.Keys) Assert.Contains(p, BiosSteps.VendorIds);
            if (i.Group != "firmware" && i.Id != "storage-ahci") continue;
            foreach (var v in BiosSteps.VendorIds)
            {
                Assert.True(i.Paths.ContainsKey(v), $"{i.Id}: no path for {v}");
                Assert.False(string.IsNullOrWhiteSpace(i.PathFor(v, "en")));
                Assert.False(string.IsNullOrWhiteSpace(i.PathFor(v, "tr")));
            }
        }
        foreach (var v in Data.Vendors)
        {
            Assert.False(string.IsNullOrWhiteSpace(v.SetupKey));
            Assert.False(string.IsNullOrWhiteSpace(v.BootMenuKey));
            Assert.True(v.Note.En.Length > 0 && v.Note.Tr.Length > 0);
        }
    }

    [Fact]
    public void ItemsNeverTellTheAppToChangeASetting()
    {
        // the app reads; the text says what the user does. No "the app suspends" / "the app turns off".
        foreach (var i in Data.Items)
            Assert.DoesNotMatch(new Regex(@"\bthe app (suspends|turns|changes|sets|disables)\b", RegexOptions.IgnoreCase), i.Why.En);
    }

    // ---- vendors ----

    [Theory]
    [InlineData("ASUSTeK COMPUTER INC.", "asus")]
    [InlineData("Micro-Star International Co., Ltd.", "msi")]
    [InlineData("Gigabyte Technology Co., Ltd.", "gigabyte")]
    [InlineData("ASRock", "asrock")]
    [InlineData("Dell Inc.", "generic")]
    [InlineData("", "generic")]
    [InlineData(null, "generic")]
    public void FindsTheVendor(string? maker, string vendor) => Assert.Equal(vendor, BiosSteps.VendorOf(Data, maker));

    [Fact]
    public void PathsFallBackToGeneric()
    {
        var bitlocker = Data.Items.Single(i => i.Id == "bitlocker");
        Assert.StartsWith("Windows:", bitlocker.PathFor("asus", "en"));
        Assert.StartsWith("Windows:", bitlocker.PathFor("asus", "tr"));
        var a4g = Data.Items.Single(i => i.Id == "above4g");
        Assert.Equal("Advanced > PCI Subsystem Settings > Above 4G Decoding", a4g.PathFor("asus", "tr"));
        Assert.StartsWith("çoğu zaman", a4g.PathFor("generic", "tr"));
    }

    // ---- states from the facts ----

    static PcFacts Good() => new()
    {
        CpuName = "Intel(R) Core(TM) i7-14700K", BoardMaker = "ASUSTeK COMPUTER INC.", BoardModel = "PRIME Z790-P",
        Firmware = FirmwareMode.Uefi, SecureBoot = SecureBootState.Off, BitLocker = BitLockerState.Off,
        Bar = new BarSummary([new MemoryRange(0x4000000000, 16UL << 30), new MemoryRange(0x80000000, 16 << 20)]), Windows = new WindowsInfo("Windows 11 Pro", "25H2", 26200, 1),
        StorageControllers = ["Standard NVM Express Controller", "Standard SATA AHCI Controller"],
        DmarTable = false, VirtualizationFirmwareEnabled = true, Cores = 20, Threads = 28,
        Gpus = [new GpuInfo("Intel(R) Arc(TM) B580 Graphics", PciId.Parse(@"PCI\VEN_8086&DEV_E20B&SUBSYS_00000000&REV_00"))],
    };

    static BiosState StateOf(PcFacts f, string id) => BiosSteps.Evaluate(Data, f).Single(s => s.Item.Id == id).State;

    [Fact]
    public void AWellSetPcIsDoneWhereWindowsCanTell()
    {
        var steps = BiosSteps.Evaluate(Data, Good());
        Assert.Equal(Data.Items.Count, steps.Count);
        foreach (var id in new[] { "bitlocker", "windows-uefi", "storage-ahci", "secure-boot", "above4g", "rebar", "igpu", "vtd", "vtx", "hyperthreading", "execute-disable" })
            Assert.Equal(BiosState.Done, steps.Single(s => s.Item.Id == id).State);
        foreach (var id in new[] { "csm", "cfg-lock", "xhci-handoff", "fast-boot", "sgx", "serial-port", "thunderbolt", "platform-trust" })
            Assert.Equal(BiosState.CannotTell, steps.Single(s => s.Item.Id == id).State);
        Assert.Equal((0, 11, 8), BiosSteps.Count(steps));
        Assert.Equal(Data.Items.Select(i => i.Id), steps.Select(s => s.Item.Id));
    }

    [Fact]
    public void ABadPcHasItsItemsToDo()
    {
        var f = Good();
        f.Firmware = FirmwareMode.Legacy;
        f.SecureBoot = SecureBootState.NotSupported;
        f.BitLocker = BitLockerState.On;
        f.Bar = new BarSummary([new MemoryRange(0x80000000, 256 << 20)]);
        f.StorageControllers = ["Intel(R) RST VMD Controller 9A0B"];
        f.DmarTable = true;
        f.Gpus.Add(new GpuInfo("Intel(R) UHD Graphics 770", PciId.Parse(@"PCI\VEN_8086&DEV_A780&SUBSYS_00000000&REV_04")));
        foreach (var id in new[] { "bitlocker", "windows-uefi", "storage-ahci", "csm", "above4g", "rebar", "igpu", "vtd" })
            Assert.Equal(BiosState.ToDo, StateOf(f, id));
        Assert.Equal(BiosState.CannotTell, StateOf(f, "secure-boot"));
        var raid = BiosSteps.Evaluate(Data, f).Single(s => s.Item.Id == "storage-ahci");
        Assert.Contains("RST VMD", raid.Seen.In(Data, "en"));
        Assert.Contains("listeliyor", raid.Seen.In(Data, "tr"));
        Assert.Contains("UHD Graphics 770", BiosSteps.Evaluate(Data, f).Single(s => s.Item.Id == "igpu").Seen.In(Data, "en"));
    }

    [Fact]
    public void SecureBootOnMeansCsmOff()
    {
        var f = Good();
        f.SecureBoot = SecureBootState.On;
        Assert.Equal(BiosState.ToDo, StateOf(f, "secure-boot"));
        Assert.Equal(BiosState.Done, StateOf(f, "csm"));
    }

    [Fact]
    public void UnknownFactsCannotTell()
    {
        var f = new PcFacts { CpuName = "Intel(R) Core(TM) i5-12400" };
        foreach (var s in BiosSteps.Evaluate(Data, f).Where(s => s.Item.Id != "igpu"))
            Assert.Equal(BiosState.CannotTell, s.State);
        f.Cores = 6; f.Threads = 6;
        Assert.Equal(BiosState.CannotTell, StateOf(f, "hyperthreading"));
        f.VirtualizationFirmwareEnabled = false;   // reported false while Hyper-V runs: not "to do"
        Assert.Equal(BiosState.CannotTell, StateOf(f, "vtx"));
        f.BitLocker = BitLockerState.WaitingForActivation;
        Assert.Equal(BiosState.Done, StateOf(f, "bitlocker"));
    }

    [Fact]
    public void IntelOnlyItemsAreLeftOutOnAmd()
    {
        var f = Good();
        f.CpuName = "AMD Ryzen 7 7800X3D 8-Core Processor";
        var ids = BiosSteps.Evaluate(Data, f).Select(s => s.Item.Id).ToList();
        Assert.DoesNotContain("cfg-lock", ids);
        Assert.DoesNotContain("vtd", ids);
        Assert.Contains("csm", ids);
    }

    [Theory]
    [InlineData("Intel(R) Arc(TM) B580 Graphics", false)]
    [InlineData("Intel(R) Arc(TM) A770 Graphics", false)]
    [InlineData("Intel(R) UHD Graphics 770", true)]
    [InlineData("Intel(R) Arc(TM) Graphics", true)]   // Core Ultra's integrated Arc
    public void TellsIntegratedGraphics(string name, bool integrated)
    {
        var f = new PcFacts { Gpus = [new GpuInfo(name, PciId.Parse(@"PCI\VEN_8086&DEV_1234&SUBSYS_00000000&REV_00"))] };
        Assert.Equal(integrated, f.IntegratedGpus.Any());
    }

    [Theory]
    [InlineData("Intel(R) RST VMD Controller 9A0B", true)]
    [InlineData("Intel(R) Chipset SATA/PCIe RST Premium Controller", true)]
    [InlineData("Standard SATA AHCI Controller", false)]
    [InlineData("Standard NVM Express Controller", false)]
    [InlineData("Microsoft Storage Spaces Controller", false)]
    public void TellsRaidControllers(string name, bool raid) => Assert.Equal(raid, BiosSteps.IsRaidController(name));

    [Fact]
    public void PicksDortaniasPageByGeneration()
    {
        Assert.Equal("dortania-bios-coffee", BiosSteps.SourceFor("dortania-bios", new PcFacts { CpuName = "Intel(R) Core(TM) i7-9700K CPU @ 3.60GHz" }));
        Assert.Equal("dortania-bios", BiosSteps.SourceFor("dortania-bios", new PcFacts { CpuName = "Intel(R) Core(TM) i7-14700K" }));
        Assert.Equal("guide-hardware", BiosSteps.SourceFor("guide-hardware", null));
    }

    // ---- the printable page ----

    [Fact]
    public void ThePrintablePageIsSelfContainedAndEscaped()
    {
        var f = Good();
        f.BoardModel = "<script>alert(1)</script> & Z790";
        var steps = BiosSteps.Evaluate(Data, f);
        foreach (var lang in new[] { "en", "tr" })
        {
            var html = BiosHtml.Render(Data, steps, f, "asus", lang, new DateTime(2026, 11, 12, 10, 0, 0));
            Assert.StartsWith("<!doctype html>", html);
            Assert.Contains($"<html lang=\"{lang}\">", html);
            Assert.Contains("name=\"viewport\"", html);
            Assert.Contains("Content-Security-Policy", html);
            Assert.DoesNotContain("<script", html);
            Assert.Contains("&lt;script&gt;alert(1)&lt;/script&gt; &amp; Z790", html);
            Assert.DoesNotMatch(new Regex(@"<(link|img|iframe)\b|src=", RegexOptions.IgnoreCase), html);
            foreach (var s in steps) Assert.Contains(BiosHtml.E(s.Item.Title.In(lang)), html);
            Assert.Contains("Advanced &gt; PCI Subsystem Settings &gt; Above 4G Decoding", html);
            Assert.Contains("2026-11-12 10:00", html);
            Assert.Contains("prefers-color-scheme: dark", html);
            Assert.Contains("@media print", html);
        }
        Assert.Contains("Bu bilgisayar için BIOS ayarları", BiosHtml.Render(Data, steps, f, "generic", "tr", DateTime.Now));
    }
}
