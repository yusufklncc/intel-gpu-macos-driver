// SPDX-License-Identifier: MIT
// The elevated helper's scan (Core/EfiBuild/SniffHelper.cs) with a fake Python: its command line, the folder rules, the
// engine check, and the ACPI tables: only DSDT, SSDT, APIC and DMAR stay; MSDM (the Windows product key), SLIC and the
// rest are deleted, also when the scan fails.
using System.Text;
using IntelXe.Core.EfiBuild;

namespace IntelXe.Core.Tests;

public class SniffTests
{
    static void Table(string dir, string name, string signature) =>
        FakeEngine.Write(Path.Combine(dir, name), signature + new string('\0', 32));

    /// What efibuild.py sniff leaves: Report.json and ACPI\ (a builder that kept too much, to show the helper's own check).
    static FakeRunner Sniffer(int code = 0, bool tables = true, bool msdm = true, string? error = null) => new((s, stdout, stderr, _) =>
    {
        Assert.Equal("sniff", s.Args[6]);
        var outDir = FakeRunner.Arg(s, "--out");
        stderr("== Hardware Sniffer");
        if (tables)
        {
            var acpi = Path.Combine(outDir, "ACPI");
            Table(acpi, "dsdt.dat", "DSDT");
            Table(acpi, "ssdt1.dat", "SSDT");
            Table(acpi, "apic.dat", "APIC");
            Table(acpi, "dmar.dat", "DMAR");
            if (msdm) { Table(acpi, "msdm.dat", "MSDM"); Table(acpi, "slic.dat", "SLIC"); Table(acpi, "facp.dat", "FACP"); }
        }
        if (code == 0) FakeEngine.Write(Path.Combine(outDir, "Report.json"), "{}");
        if (error is not null) stderr("error: " + error);
        stdout("report: " + Path.Combine(outDir, "Report.json"));
        return Task.FromResult(code);
    });

    [Fact]
    public void TheHelpersCommandLine()
    {
        var full = Path.GetFullPath(Path.Combine(Path.GetTempPath(), "scan 1"));
        var q = new SniffRequest(full, Path.Combine(Path.GetTempPath(), "a.json"));
        Assert.Equal(["--efi-helper", "sniff", "--scan", full, "--out", q.Out!], q.ToArgs());
        Assert.True(SniffRequest.Is(q.ToArgs()));
        Assert.False(SniffRequest.Is(["--efi-helper", "status"]));
        Assert.Equal(q, SniffRequest.Parse(q.ToArgs()));
        Assert.Throws<EfiBuildException>(() => SniffRequest.Parse(["--efi-helper", "sniff", "--scan", "relative"]));
        Assert.Throws<EfiBuildException>(() => SniffRequest.Parse(["--efi-helper", "sniff"]));
        Assert.Throws<EfiBuildException>(() => SniffRequest.Parse(["--efi-helper", "sniff", "--scan", full, "--scan", full]));
        Assert.Throws<EfiBuildException>(() => SniffRequest.Parse(["--efi-helper", "sniff", "--scan", full, "--python", "x.exe"]));
        Assert.Throws<EfiBuildException>(() => SniffRequest.Parse(["--efi-helper", "sniff", "--scan"]));
    }

    [Fact]
    public void OnlyTheBuildsTablesStay()
    {
        using var t = new TempDir();
        var acpi = t.Sub("ACPI");
        Table(acpi, "DSDT.aml", "DSDT");
        Table(acpi, "ssdt2.aml", "SSDT");
        Table(acpi, "apic.aml", "APIC");
        Table(acpi, "dmar.aml", "DMAR");
        Table(acpi, "msdm.aml", "MSDM");
        Table(acpi, "ssdt-named-msdm.aml", "MSDM");   // by signature, not by name
        Table(acpi, "slic.aml", "SLIC");
        FakeEngine.Write(Path.Combine(acpi, "short.aml"), "DS");
        FakeEngine.Write(Path.Combine(acpi, "sub", "x.aml"), "DSDT");
        var (kept, removed) = AcpiTables.KeepOnlyBuildTables(acpi);
        Assert.Equal(["DSDT.aml", "apic.aml", "dmar.aml", "ssdt2.aml"], kept);
        Assert.Equal(5, removed.Count);
        Assert.Equal(kept.Order(), Directory.EnumerateFileSystemEntries(acpi).Select(Path.GetFileName).Order());
    }

    [Fact]
    public async Task AScan()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var runner = Sniffer();
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("scans", "1")), e, runner, _ => false);
        Assert.True(a.Ok, a.Error);
        Assert.Equal(t.Sub("scans", "1", "Report.json"), a.Report);
        Assert.Equal(["apic.dat", "dmar.dat", "dsdt.dat", "ssdt1.dat"], a.Tables);
        Assert.Equal(["facp.dat", "msdm.dat", "slic.dat"], a.Removed.Order());
        Assert.False(File.Exists(t.Sub("scans", "1", "ACPI", "msdm.dat")));
        Assert.Contains("== Hardware Sniffer", a.Lines);
        var back = SniffAnswer.FromJson(a.ToJson());
        Assert.Equal(a.Tables, back.ToScan().Tables);
        Assert.Equal(a.Acpi, back.ToScan().AcpiDir);
    }

    [Fact]
    public async Task AFailedScanLeavesNoOtherTable()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("scan")), e, Sniffer(code: 1), _ => false);
        Assert.False(a.Ok);
        Assert.Contains("code 1", a.Error);
        Assert.False(File.Exists(t.Sub("scan", "ACPI", "msdm.dat")));
        Assert.True(File.Exists(t.Sub("scan", "ACPI", "dsdt.dat")));
        Assert.Throws<EfiBuildException>(() => a.ToScan());
    }

    [Fact]
    public async Task TheBuildersErrorLine()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("scan")), e, Sniffer(code: 2, tables: false, error: "no DSDT in the ACPI dump (run as administrator)"), _ => false);
        Assert.Equal("no DSDT in the ACPI dump (run as administrator)", a.Error);
    }

    [Fact]
    public async Task AScanWithoutADsdtFails()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var runner = new FakeRunner((s, _, _, _) =>
        {
            var o = FakeRunner.Arg(s, "--out");
            FakeEngine.Write(Path.Combine(o, "Report.json"), "{}");
            Table(Path.Combine(o, "ACPI"), "msdm.dat", "MSDM");
            return Task.FromResult(0);
        });
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("scan")), e, runner, _ => false);
        Assert.False(a.Ok);
        Assert.Contains("no DSDT", a.Error);
        Assert.Empty(Directory.EnumerateFiles(t.Sub("scan", "ACPI")));
    }

    [Fact]
    public async Task TheFolderRules()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var runner = Sniffer();
        Directory.CreateDirectory(t.Sub("exists"));
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("exists")), e, runner, _ => false);
        Assert.Contains("exists already", a.Error);
        // a link anywhere above the new folder, existing or not yet
        a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("new", "scan")), e, runner, d => d.FullName.TrimEnd(Path.DirectorySeparatorChar) == t.Path);
        Assert.Contains("is a link", a.Error);
        Directory.CreateDirectory(t.Sub("linked"));
        a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("linked", "scan")), e, runner, d => d.Name == "linked");
        Assert.Contains("is a link", a.Error);
        Assert.Empty(runner.Started);
    }

    [Fact]
    public async Task AChangedEngineIsNotRunWithAdministratorRights()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        FakeEngine.Write(Path.Combine(e.EfiBuildDir, "intelxe_efibuild", "sniff.py"), "import os  # planted");
        var runner = Sniffer();
        var a = await SniffHelper.RunAsync(new SniffRequest(t.Sub("scan")), e, runner, _ => false);
        Assert.False(a.Ok);
        Assert.Contains("extra file efibuild/intelxe_efibuild/sniff.py", a.Error);
        Assert.Empty(runner.Started);
    }

    [Fact]
    public async Task TheBuildersScanGoesThroughTheElevatedHelper()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        SniffRequest? asked = null;
        var b = new PythonEfiBuilder(e, new FakeRunner((_, _, _, _) => Task.FromResult(0)), async (q, cancel) =>
        {
            asked = q;
            return await SniffHelper.RunAsync(q, e, Sniffer(), _ => false, cancel);
        });
        var progress = new ListProgress();
        var scan = await b.ScanAsync(t.Sub("scan"), progress, CancellationToken.None);
        Assert.Equal(t.Sub("scan"), asked!.ScanDir);
        Assert.Equal(t.Sub("scan", "ACPI"), scan.AcpiDir);
        Assert.Equal(4, scan.Tables.Count);
        Assert.Equal(["Hardware Sniffer"], progress.Lines);

        var failing = new PythonEfiBuilder(e, new FakeRunner((_, _, _, _) => Task.FromResult(0)),
            (_, _) => Task.FromResult(new SniffAnswer { Ok = false, Error = "the scan was not allowed" }));
        Assert.Equal("the scan was not allowed",
            (await Assert.ThrowsAsync<EfiBuildException>(() => failing.ScanAsync(t.Sub("scan2"), progress, CancellationToken.None))).Message);
    }
}
