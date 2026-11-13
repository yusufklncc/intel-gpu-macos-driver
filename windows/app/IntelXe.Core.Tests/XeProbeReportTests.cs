// SPDX-License-Identifier: MIT
using System.Formats.Tar;
using System.IO.Compression;
using System.Text;
using IntelXe.Core.Localisation;
using IntelXe.Core.Report;

namespace IntelXe.Core.Tests;

/// XeProbe's results in macOS report bundles (Core/Report/XeProbeReport.cs): bundles made here, as collect-macos.sh
/// --probe writes them, in a temporary folder.
public sealed class XeProbeReportTests : IDisposable
{
    const string Id = "XR-1A2B3C4D";
    const string Top = "xe-report-" + Id;
    readonly string dir = Directory.CreateTempSubdirectory("xeprobe-test-").FullName;

    public void Dispose() { try { Directory.Delete(dir, true); } catch (IOException) { } }

    /// A line as the kernel prints it: "stage %d %-9s %-8s %5u ms  %s".
    static string Stage(int n, string name, string status, uint ms, string detail = "") =>
        $"stage {n} {name,-9} {status,-8} {ms,5} ms  {detail}";

    static string Results(string header, params string[] stages) => string.Join("\n", [header, .. stages]) + "\n";

    const string B580 = "XeProbe v1 on 8086:e20b (BMG-G21)";

    static readonly string AllOk = Results(B580,
        "stage 1 identity  ok           3 ms  PCI 8086:e20b rev 0x00 class 030000",
        Stage(2, "forcewake", "ok", 12, "GT and render awake"),
        Stage(3, "guc", "ok", 1840, "GuC 70.72.1 (bmg) running"),
        Stage(4, "store", "ok", 40, "RCS store 0xc0ffee00 seen"),
        Stage(5, "readback", "ok", 7, "16 of 16 dwords"),
        Stage(9, "release", "ok", 1, "forcewake released"));

    static readonly string GucFailed = Results(B580,
        Stage(1, "identity", "ok", 3, "PCI 8086:e20b rev 0x00 class 030000"),
        Stage(2, "forcewake", "ok", 12),
        Stage(3, "guc", "FAILED", 15002, "GuC 70.72.1 (lnl); GuC status 0x00000001"),
        Stage(4, "store", "refused", 0, "stage 3 (guc) has not passed"),
        Stage(5, "readback", "refused", 0, "stage 4 (store) has not passed"),
        Stage(9, "release", "not run", 0));

    /// A .tar.gz of (path, content) entries; content null makes a folder entry.
    string Archive(string name, IEnumerable<(string Path, string? Content)> entries, Action<TarWriter>? more = null)
    {
        var path = Path.Combine(dir, name);
        using (var f = File.Create(path))
        using (var gz = new GZipStream(f, CompressionLevel.Fastest))
        using (var tar = new TarWriter(gz, TarEntryFormat.Pax))
        {
            foreach (var (p, content) in entries)
            {
                if (content is null) { tar.WriteEntry(new PaxTarEntry(TarEntryType.Directory, p)); continue; }
                tar.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, p) { DataStream = new MemoryStream(Encoding.UTF8.GetBytes(content)) });
            }
            more?.Invoke(tar);
        }
        return path;
    }

    /// A report's entries: its folder, summary.txt, logs/, and probe/ with the files given (none: no probe/ folder).
    static List<(string, string?)> Bundle(Dictionary<string, string>? probe, Dictionary<string, string>? other = null)
    {
        var list = new List<(string, string?)> { (Top + "/", null), (Top + "/summary.txt", "IntelXe report " + Id + "\n"), (Top + "/logs/", null) };
        if (probe is not null)
        {
            list.Add((Top + "/probe/", null));
            foreach (var (k, v) in probe) list.Add(($"{Top}/probe/{k}", v));
        }
        foreach (var (k, v) in other ?? []) list.Add(($"{Top}/{k}", v));
        return list;
    }

    static string Joined(IReadOnlyList<Text> parts) => string.Join("; ", parts);

    [Fact]
    public void ParsesTheKernelsPaddedLines()
    {
        var r = XeProbeReports.ParseResults(AllOk);
        Assert.True(r.Attached);
        Assert.Equal(1, r.Version);
        Assert.Equal("8086:e20b", r.Device);
        Assert.Equal("BMG-G21", r.Family);
        Assert.Equal([1, 2, 3, 4, 5, 9], r.Stages.Select(s => s.Number));
        Assert.Equal(["identity", "forcewake", "guc", "store", "readback", "release"], r.Stages.Select(s => s.Name));
        Assert.All(r.Stages, s => Assert.Equal(XeProbeVerdict.Passed, s.Verdict));
        Assert.Equal("PCI 8086:e20b rev 0x00 class 030000", r.Stages[0].Detail);
        Assert.Equal(3u, r.Stages[0].Ms);
        Assert.Equal(1840u, r.Stages[2].Ms);
        Assert.Empty(r.Other);

        // an empty detail ("ms  " then nothing), "not run" with its space, six-digit milliseconds, "running"
        var x = XeProbeReports.ParseResults("XeProbe v1 on 8086:7d55 (family unknown)\n" + Stage(9, "release", "not run", 0) + "\n"
            + Stage(3, "guc", "STALLED", 123456, "step GuC DMA") + "\n" + Stage(4, "store", "running", 5) + "\r\n");
        Assert.Null(x.Family);
        Assert.Equal("8086:7d55", x.Device);
        Assert.Equal((XeProbeStatus.NotRun, ""), (x.Stages[0].Status, x.Stages[0].Detail));
        Assert.Equal((XeProbeStatus.Stalled, 123456u, "step GuC DMA"), (x.Stages[1].Status, x.Stages[1].Ms, x.Stages[1].Detail));
        Assert.Equal(XeProbeVerdict.Running, x.Stages[2].Verdict);

        var na = XeProbeReports.ParseResults("XeProbe not attached\n");
        Assert.False(na.Attached);
        Assert.Empty(na.Stages);
    }

    [Fact]
    public void AllStagesPassOnAB580()
    {
        var file = Archive(Top + ".tar.gz", Bundle(new()
        {
            ["results.txt"] = AllOk,
            ["info.txt"] = "version=1\ndevice=8086:E20B\nfamily=BMG\nguc_image=bmg_guc_70.bin\nguc_built_in=70.72.1\nimages=bmg 70.72.1,lnl 70.72.1\nnvram=0\npanic_on_stall=1\nlast_stage=9\nfuture_key=whatever\n",
            ["log.txt"] = "XeProbe: stage 1 (identity) begins\nXeProbe: stage 1 (identity): ok in 3 ms\n",
            ["nvram-breadcrumb.txt"] = "no breadcrumb in NVRAM (the probe has not run with debug.xeprobe.nvram=1 or -xeprobecrumb)\n",
            ["panics.txt"] = "",
            ["ioreg.txt"] = "+-o XeProbe  <class XeProbe, id 0x100000abc, registered, matched, active, busy 0 (0 ms), retain 6>\n",
        }));
        var r = XeProbeReports.Read(file);
        Assert.Equal(Id, r.Id);
        Assert.True(r.HasProbe);
        Assert.True(r.Attached);
        Assert.True(r.IoregAttached);
        Assert.Equal("8086:e20b", r.Device);
        Assert.Equal("BMG-G21", r.Family);
        Assert.NotNull(r.Info);
        Assert.Equal(1, r.Info.Version);
        Assert.Equal("8086:e20b", r.Info.Device);
        Assert.Equal("BMG", r.Info.Family);
        Assert.Equal("bmg_guc_70.bin", r.Info.GucImage);
        Assert.Equal("70.72.1", r.Info.GucBuiltIn);
        Assert.Equal(["bmg 70.72.1", "lnl 70.72.1"], r.Info.Images);
        Assert.Equal((false, true, 9), (r.Info.Nvram, r.Info.PanicOnStall, r.Info.LastStage));
        Assert.Null(r.Breadcrumb);
        Assert.StartsWith("no breadcrumb in NVRAM", r.BreadcrumbNote);
        Assert.Empty(r.Panics);
        Assert.Empty(r.History);
        Assert.Contains("stage 1 (identity) begins", r.Log);
        Assert.Empty(r.Notes);
        Assert.Equal("XeProbe_Sum_Passed(1-5, 9)", Joined(XeProbeReports.Summary(r)));
    }

    [Fact]
    public void StageThreeFailsAndTheRestIsRefused()
    {
        var file = Archive(Top + ".tgz", Bundle(new() { ["results.txt"] = GucFailed }));
        var r = XeProbeReports.Read(file);
        Assert.Equal([XeProbeVerdict.Passed, XeProbeVerdict.Passed, XeProbeVerdict.Failed, XeProbeVerdict.Refused, XeProbeVerdict.Refused, XeProbeVerdict.NotRun],
            r.Results!.Stages.Select(s => s.Verdict));
        Assert.Equal(15002u, r.Results.Stages[2].Ms);
        Assert.Equal("XeProbe_Sum_Passed(1-2); XeProbe_Sum_Failed(3, guc, GuC 70.72.1 (lnl); GuC status 0x00000001); XeProbe_Sum_Refused(4-5)",
            Joined(XeProbeReports.Summary(r)));
        Assert.Equal(CheckState.Bad, XeProbeReports.VerdictState(XeProbeVerdict.Failed));
        Assert.Equal(CheckState.Good, XeProbeReports.VerdictState(XeProbeVerdict.Passed));
        Assert.Equal("XeProbe_Verdict_Refused", XeProbeReports.VerdictText(XeProbeVerdict.Refused).Key);
        // one stage alone, and a failure without a detail
        var one = XeProbeReports.ParseResults(Results(B580, Stage(1, "identity", "ok", 3), Stage(2, "forcewake", "FAILED", 50), Stage(3, "guc", "refused", 0)));
        Assert.Equal("XeProbe_Sum_PassedOne(1); XeProbe_Sum_FailedBare(2, forcewake); XeProbe_Sum_RefusedOne(3)", Joined(XeProbeReports.Summary(one)));
        var none = XeProbeReports.ParseResults(Results(B580, Stage(1, "identity", "not run", 0)));
        Assert.Equal("XeProbe_Sum_NoneRun", Joined(XeProbeReports.Summary(none)));
    }

    [Fact]
    public void AStallWithItsBreadcrumbAndTheWatchdogsPanic()
    {
        const string panicRel = "logs/panics/Kernel-2026-11-13-102201.panic.txt";
        const string watchdog = "XeProbe watchdog: stage 3 (guc) on 8086:7d55 stalled in step \"GuC DMA\" (15002 ms in the step, limit 15000 ms for the stage)";
        // a .panic file: one line of JSON header, then JSON whose panicString escapes quotes and line ends
        var panic = "{\"bug_type\":\"210\",\"timestamp\":\"2026-11-13 10:22:01.00 +0000\"}\n{\"panicString\":\"panic(cpu 2 caller 0xffffff8001234567): "
                    + watchdog.Replace("\"", "\\\"") + " @xe_probe.cpp:285\\nDebugger message: panic\\n\"}\n";
        var file = Archive(Top + ".tar.gz", Bundle(new()
        {
            ["results.txt"] = Results("XeProbe v1 on 8086:7d55 (MTL)", Stage(1, "identity", "ok", 2), Stage(2, "forcewake", "ok", 9),
                Stage(3, "guc", "STALLED", 15002, "stalled in step GuC DMA")),
            ["nvram-breadcrumb.txt"] = "XeProbe v1 8086:7d55 stage 3 guc step GuC DMA seq 7%00\n",
            ["panics.txt"] = panicRel + "\nlogs/panics/Kernel-2026-11-12-090000.panic.txt\n",
        }, new()
        {
            [panicRel] = panic,
            ["logs/panics/Kernel-2026-11-12-090000.panic.txt"] = "panic(cpu 0): XeProbe: a page fault in stage 2\n",
        }));
        var r = XeProbeReports.Read(file);
        Assert.NotNull(r.Breadcrumb);
        Assert.Equal("XeProbe v1 8086:7d55 stage 3 guc step GuC DMA seq 7", r.Breadcrumb.Text);
        Assert.Equal(("8086:7d55", 3, "guc", "GuC DMA", 7u), (r.Breadcrumb.Device, r.Breadcrumb.Stage, r.Breadcrumb.StageName, r.Breadcrumb.Step, r.Breadcrumb.Seq));
        Assert.Null(r.BreadcrumbNote);
        Assert.Equal(2, r.Panics.Count);
        Assert.Equal(new XeProbePanic(panicRel, true, watchdog), r.Panics[0]);
        Assert.Equal(new XeProbePanic("logs/panics/Kernel-2026-11-12-090000.panic.txt", true, null), r.Panics[1]);
        Assert.Equal(XeProbeVerdict.Stalled, r.Results!.Stages[2].Verdict);
        Assert.Equal("XeProbe_Sum_Passed(1-2); XeProbe_Sum_Stalled(3, guc, stalled in step GuC DMA); XeProbe_Sum_Panics(1)", Joined(XeProbeReports.Summary(r)));

        // a plain-text panic: the line ends at "for the stage)"
        Assert.Equal(watchdog, XeProbeReports.WatchdogLine("panic(cpu 2 caller 0x1): " + watchdog + " @xe_probe.cpp:285\nBacktrace\n"));
        Assert.Null(XeProbeReports.WatchdogLine("panic(cpu 0): something else\n"));
    }

    [Fact]
    public void NotAttached()
    {
        var file = Archive(Top + ".tar.gz", Bundle(new()
        {
            ["results.txt"] = "XeProbe not attached\n",
            ["ioreg.txt"] = "",
            ["nvram-breadcrumb.txt"] = "no breadcrumb in NVRAM (the probe has not run with debug.xeprobe.nvram=1 or -xeprobecrumb)\n",
        }));
        var r = XeProbeReports.Read(file);
        Assert.True(r.HasProbe);
        Assert.False(r.Attached);
        Assert.False(r.IoregAttached);
        Assert.Null(r.Device);
        Assert.Equal("XeProbe_Sum_NotAttached", Joined(XeProbeReports.Summary(r)));
    }

    [Fact]
    public void AReportWithoutProbeResults()
    {
        var r = XeProbeReports.Read(Archive(Top + ".tar.gz", Bundle(null)));
        Assert.Equal(Id, r.Id);
        Assert.False(r.HasProbe);
        Assert.Null(r.Results);
        Assert.Equal("XeProbe_Sum_NoProbe", Joined(XeProbeReports.Summary(r)));

        // a probe/ folder whose results could not be read
        var empty = XeProbeReports.Read(Archive("empty.tar.gz", Bundle(new() { ["log.txt"] = "" })));
        Assert.True(empty.HasProbe);
        Assert.Equal("XeProbe_Sum_NoResults", Joined(XeProbeReports.Summary(empty)));
    }

    [Fact]
    public void AnOlderReportWithoutInfo()
    {
        var r = XeProbeReports.Read(Archive(Top + ".tar.gz", Bundle(new() { ["results.txt"] = GucFailed, ["log.txt"] = "x\n" })));
        Assert.Null(r.Info);
        Assert.Empty(r.History);
        Assert.Equal("8086:e20b", r.Device);
        Assert.Equal("BMG-G21", r.Family);

        // info.txt's empty and "no"/"unknown" values
        var i = XeProbeReports.ParseInfo("version=1\ndevice=8086:64a0\nfamily=unknown\nguc_image=\nguc_built_in=no\nimages=\nnvram=1\npanic_on_stall=0\nlast_stage=0\nnot a key line\n");
        Assert.Null(i.Family);
        Assert.Null(i.GucImage);
        Assert.Null(i.GucBuiltIn);
        Assert.Empty(i.Images);
        Assert.Equal((true, false, 0), (i.Nvram, i.PanicOnStall, i.LastStage));
    }

    [Fact]
    public void TheManagersHistoryOverTwoBoots()
    {
        var history = "# IntelXe Manager: XeProbe runs, newest last\n"
            + "== 2026-11-12T21:04:10Z boot 2026-11-12T20:58:31Z stage 3 exit 1\n" + GucFailed
            + "== 2026-11-13T10:22:01Z boot 2026-11-13T10:15:44Z stage run5 exit 0\n" + AllOk;
        var r = XeProbeReports.Read(Archive(Top + ".tar.gz", Bundle(new() { ["results.txt"] = AllOk, ["history.txt"] = history })));
        Assert.Equal(2, r.History.Count);
        Assert.Equal(2, r.HistoryBoots);
        var (a, b) = (r.History[0], r.History[1]);
        Assert.Equal(new DateTimeOffset(2026, 11, 12, 21, 4, 10, TimeSpan.Zero), a.When);
        Assert.Equal(new DateTimeOffset(2026, 11, 12, 20, 58, 31, TimeSpan.Zero), a.Boot);
        Assert.Equal(("3", 3, (int?)null, 1), (a.What, a.Stage, a.RunTo, a.Exit));
        Assert.Equal(XeProbeVerdict.Failed, a.Results.Stages[2].Verdict);
        Assert.Equal(("run5", (int?)null, 5, 0), (b.What, b.Stage, b.RunTo, b.Exit));
        Assert.Equal(6, b.Results.Stages.Count);
        Assert.Equal("XeProbe_Sum_Passed(1-5, 9)", Joined(XeProbeReports.Summary(b.Results)));

        // one boot, a run before any header ignored
        var same = XeProbeReports.ParseHistory("stage 1 junk\n== 2026-11-13T10:00:00Z boot 2026-11-13T09:00:00Z stage 1 exit 0\nXeProbe not attached\n"
            + "== 2026-11-13T10:01:00Z boot 2026-11-13T09:00:00Z stage 9 exit 0\n");
        Assert.Equal(2, same.Count);
        Assert.False(same[0].Results.Attached);
        Assert.Single(same.Select(x => x.BootText).Distinct());
    }

    [Fact]
    public void AHostileArchiveIsReadOnlyWhereItIsSafe()
    {
        var big = new string('A', XeProbeReports.FileLimit + 1);
        var file = Archive(Top + ".tar.gz", Bundle(new()
        {
            ["results.txt"] = GucFailed,
            ["log.txt"] = big,
            ["panics.txt"] = "../../etc/passwd\n/etc/passwd\nC:/Windows/win.ini\nlogs/panics/link.txt\n",
        }), tar =>
        {
            tar.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, "../evil.txt") { DataStream = new MemoryStream("x"u8.ToArray()) });
            tar.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, Top + "/../../evil.txt") { DataStream = new MemoryStream("x"u8.ToArray()) });
            tar.WriteEntry(new PaxTarEntry(TarEntryType.RegularFile, "/etc/evil") { DataStream = new MemoryStream("x"u8.ToArray()) });
            tar.WriteEntry(new PaxTarEntry(TarEntryType.SymbolicLink, Top + "/probe/info.txt") { LinkName = "/etc/passwd" });
            tar.WriteEntry(new PaxTarEntry(TarEntryType.SymbolicLink, Top + "/logs/panics/link.txt") { LinkName = "/etc/passwd" });
            tar.WriteEntry(new PaxTarEntry(TarEntryType.HardLink, Top + "/probe/history.txt") { LinkName = "/etc/passwd" });
        });
        var r = XeProbeReports.Read(file);
        Assert.NotNull(r.Results);
        Assert.Equal(6, r.Results.Stages.Count);
        Assert.Null(r.Info);
        Assert.Empty(r.History);
        Assert.Equal("", r.Log);
        var notes = r.Notes.Select(n => n.ToString()).ToList();
        Assert.Contains("XeProbe_Note_TooLarge(probe/log.txt, 2)", notes);
        Assert.Contains("XeProbe_Note_Outside(../evil.txt)", notes);
        Assert.Contains($"XeProbe_Note_Outside({Top}/../../evil.txt)", notes);
        Assert.Contains("XeProbe_Note_Outside(/etc/evil)", notes);
        Assert.Contains("XeProbe_Note_Link(probe/info.txt)", notes);
        Assert.Contains("XeProbe_Note_Link(probe/history.txt)", notes);
        Assert.Contains("XeProbe_Note_Link(logs/panics/link.txt)", notes);
        Assert.Contains("XeProbe_Note_BadPanicPath(../../etc/passwd)", notes);
        Assert.Contains("XeProbe_Note_BadPanicPath(/etc/passwd)", notes);
        Assert.Contains("XeProbe_Note_BadPanicPath(C:/Windows/win.ini)", notes);
        // only the path inside the report is looked for; it is a link, so not read
        Assert.Equal([new XeProbePanic("logs/panics/link.txt", false, null)], r.Panics);
    }

    [Fact]
    public void FilesThatAreNotReports()
    {
        var text = Path.Combine(dir, "notes.tar.gz");
        File.WriteAllText(text, "hello");
        Assert.Equal("XeProbe_Error_NotReport", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(text)).Reason.Key);

        var zip = Path.Combine(dir, "xe-report-XR-00000000.zip");
        using (var z = ZipFile.Open(zip, ZipArchiveMode.Create)) z.CreateEntry("summary.txt");
        Assert.Equal("XeProbe_Error_Zip", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(zip)).Reason.Key);

        var other = Archive("other.tar.gz", [("something/", null), ("something/probe/results.txt", AllOk)]);
        var e = Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(other));
        Assert.Equal("XeProbe_Error_NotReport", e.Reason.Key);
        Assert.Contains("other.tar.gz", e.Message);

        // gzip of something that is not a tar
        var gzText = Path.Combine(dir, "text.tar.gz");
        using (var f = File.Create(gzText))
        using (var gz = new GZipStream(f, CompressionLevel.Fastest)) gz.Write(Encoding.UTF8.GetBytes(new string('x', 2000)));
        Assert.Contains(Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(gzText)).Reason.Key, new[] { "XeProbe_Error_NotReport", "XeProbe_Error_Unreadable" });

        // a truncated archive
        var whole = File.ReadAllBytes(Archive("whole.tar.gz", Bundle(new() { ["results.txt"] = AllOk, ["log.txt"] = new string('y', 100_000) })));
        var cut = Path.Combine(dir, "cut.tar.gz");
        File.WriteAllBytes(cut, whole[..(whole.Length / 2)]);
        Assert.Equal("XeProbe_Error_Unreadable", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(cut)).Reason.Key);

        Assert.Equal("XeProbe_Error_Missing", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(Path.Combine(dir, "nope.tar.gz"))).Reason.Key);
    }

    [Fact]
    public void AnExtractedFolder()
    {
        var holder = Path.Combine(dir, "Downloads");
        var top = Path.Combine(holder, Top);
        Directory.CreateDirectory(Path.Combine(top, "probe"));
        Directory.CreateDirectory(Path.Combine(top, "logs", "panics"));
        const string watchdog = "XeProbe watchdog: stage 3 (guc) on 8086:e20b stalled in step \"GuC DMA\" (15002 ms in the step, limit 15000 ms for the stage)";
        File.WriteAllText(Path.Combine(top, "probe", "results.txt"), GucFailed.Replace("\n", "\r\n"));
        File.WriteAllText(Path.Combine(top, "probe", "info.txt"), "version=1\ndevice=8086:e20b\nfamily=BMG\n");
        File.WriteAllText(Path.Combine(top, "probe", "panics.txt"), "logs/panics/Kernel-1.panic.txt\nlogs/panics/gone.txt\n../outside.txt\n");
        File.WriteAllText(Path.Combine(top, "logs", "panics", "Kernel-1.panic.txt"), "panic(cpu 1): " + watchdog + "\n");
        File.WriteAllText(Path.Combine(holder, "outside.txt"), "XeProbe watchdog: outside\n");

        foreach (var pick in new[] { top, holder, Path.Combine(top, "probe") })
        {
            var r = XeProbeReports.Read(pick);
            Assert.Equal(Id, r.Id);
            Assert.True(r.HasProbe);
            Assert.Equal(XeProbeVerdict.Failed, r.Results!.Stages[2].Verdict);
            Assert.Equal("BMG", r.Info!.Family);
            Assert.Equal([new XeProbePanic("logs/panics/Kernel-1.panic.txt", true, watchdog), new XeProbePanic("logs/panics/gone.txt", false, null)], r.Panics);
            Assert.Equal(["XeProbe_Note_BadPanicPath(../outside.txt)"], r.Notes.Select(n => n.ToString()));
        }

        // a link inside the report is not followed (macOS and Linux; Windows needs a privilege to make one)
        if (!OperatingSystem.IsWindows())
        {
            File.CreateSymbolicLink(Path.Combine(top, "probe", "log.txt"), Path.Combine(holder, "outside.txt"));
            File.WriteAllText(Path.Combine(top, "probe", "history.txt"), new string('h', XeProbeReports.FileLimit + 10));
            var r = XeProbeReports.Read(top);
            Assert.Equal("", r.Log);
            Assert.Empty(r.History);
            Assert.Contains("XeProbe_Note_Link(probe/log.txt)", r.Notes.Select(n => n.ToString()));
            Assert.Contains("XeProbe_Note_TooLarge(probe/history.txt, 2)", r.Notes.Select(n => n.ToString()));
        }

        // a folder without a report, and one with two
        Assert.Equal("XeProbe_Error_NotReport", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(Path.Combine(top, "logs"))).Reason.Key);
        Directory.CreateDirectory(Path.Combine(holder, "xe-report-XR-DEADBEEF"));
        Assert.Equal("XeProbe_Error_Several", Assert.Throws<XeProbeReportException>(() => XeProbeReports.Read(holder)).Reason.Key);
        // a folder without probe/
        Assert.False(XeProbeReports.Read(Path.Combine(holder, "xe-report-XR-DEADBEEF")).HasProbe);
    }

    [Fact]
    public void Ranges()
    {
        Assert.Equal("1-3, 5, 9", XeProbeReports.Ranges([9, 1, 2, 3, 5, 2]));
        Assert.Equal("", XeProbeReports.Ranges([]));
    }

    [Fact]
    public void EveryKeyIsInBothTables()
    {
        foreach (var language in new[] { "en-US", "tr-TR" })
        {
            var table = ReswFile.Read(Paths.AppFile("Strings", language, "Resources.resw"));
            Assert.Empty(XeProbeReports.Keys.Where(k => !table.ContainsKey(k)).ToList());
            Assert.Empty(Enum.GetValues<XeProbeVerdict>().Select(v => XeProbeReports.VerdictText(v).Key!).Where(k => !table.ContainsKey(k)).ToList());
        }
    }
}
