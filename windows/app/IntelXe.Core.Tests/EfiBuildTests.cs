// SPDX-License-Identifier: MIT
// The EFI builder seen from the app (Core/EfiBuild): the command lines, the result JSON as the builder writes it
// (Fixtures/efibuild/: two results of windows/efibuild's fixture runs, paths shortened), the process handling with a fake
// Python (exit codes, a missing result, cancellation, the engine's manifest), the SMBIOS values kept out of everything
// the window shows, and one run of a real process (a shell script standing in for Python, not on Windows).
using System.Text;
using IntelXe.Core.EfiBuild;

namespace IntelXe.Core.Tests;

/// A throwaway folder.
sealed class TempDir : IDisposable
{
    public string Path { get; } = System.IO.Path.Combine(System.IO.Path.GetTempPath(), "intelxe-test-" + Guid.NewGuid().ToString("N")[..10]);
    public TempDir() => Directory.CreateDirectory(Path);
    public string Sub(params string[] parts) => System.IO.Path.Combine([Path, .. parts]);
    public void Dispose() { try { Directory.Delete(Path, true); } catch (Exception) { } }
}

/// An engine folder as build.ps1 lays it out, with its manifest.
static class FakeEngine
{
    public static EngineLayout Make(string root, string? cache = null)
    {
        var e = new EngineLayout(root, cache);
        Write(e.Python, "fake python");
        Write(Path.Combine(root, "python", "python312._pth"), "python312.zip\n.\nLib\\site-packages\nimport site\n");
        Write(e.Script, "# SPDX-License-Identifier: MIT\n");
        Write(Path.Combine(e.EfiBuildDir, "intelxe_efibuild", "__init__.py"), "");
        Write(Path.Combine(e.EfiBuildDir, "upstream", "PINNED.json"), """
            {"projects": {"OpCore-Simplify": {"commit": "9aed67e7b41a1648e9b8d4d11db08c029535ff17"},
                          "Hardware-Sniffer": {"commit": "ff3b9edd89c4076982a0c8522be9c5dbec94925d"}}}
            """);
        Write(Path.Combine(e.EfiBuildDir, "downloads.lock.json"), """{"products": {"OpenCorePkg": {"version": "1.0.8"}}}""");
        Write(Path.Combine(root, "engine.lock.json"), """{"python": {"version": "3.12.10"}}""");
        Seal(e);
        return e;
    }

    public static void Seal(EngineLayout e) => File.WriteAllText(e.Manifest, EngineLayout.ManifestText(e.Root));

    public static void Write(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }
}

/// config.plist with an SMBIOS identity, built here so that no key and value stand on one line of this file.
static class FakeConfig
{
    public const string Serial = "TSTSERIAL0Q1";
    public const string Mlb = "TSTMLB000000000Q1";
    public const string Uuid = "0F1E2D3C-4B5A-6978-8796-A5B4C3D2E1F0";
    public static readonly byte[] Rom = [0x5A, 0x11, 0x22, 0x33, 0x44, 0xA5];

    static string Pair(string key, string type, string value) => $"<key>{key}</key>\n<{type}>{value}</{type}>\n";

    public static string Xml() =>
        "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" +
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" +
        "<plist version=\"1.0\">\n<dict>\n<key>PlatformInfo</key>\n<dict>\n<key>Generic</key>\n<dict>\n" +
        Pair("MLB", "string", Mlb) + Pair("ROM", "data", Convert.ToBase64String(Rom)) +
        Pair("SystemProductName", "string", "MacBookPro16,2") + Pair("SystemSerialNumber", "string", Serial) +
        Pair("SystemUUID", "string", Uuid) + "<key>SpoofVendor</key>\n<true/>\n" +
        "</dict>\n</dict>\n</dict>\n</plist>\n";

    public static string WriteEfi(string outDir)
    {
        var oc = Path.Combine(outDir, "EFI", "OC");
        FakeEngine.Write(Path.Combine(oc, "config.plist"), Xml());
        FakeEngine.Write(Path.Combine(oc, "OpenCore.efi"), "oc");
        FakeEngine.Write(Path.Combine(outDir, "EFI", "BOOT", "BOOTx64.efi"), "boot");
        return Path.Combine(outDir, "EFI");
    }
}

/// A Python that is not one: it answers each start with what the test says.
sealed class FakeRunner(Func<ProcessSpec, Action<string>, Action<string>, CancellationToken, Task<int>> run) : IProcessRunner
{
    public readonly List<ProcessSpec> Started = [];

    public Task<int> RunAsync(ProcessSpec spec, Action<string> stdout, Action<string> stderr, CancellationToken cancel)
    {
        Started.Add(spec);
        return run(spec, stdout, stderr, cancel);
    }

    public static string Arg(ProcessSpec s, string name) => s.Args[s.Args.ToList().IndexOf(name) + 1];
}

sealed class ListProgress : IProgress<string>
{
    public readonly List<string> Lines = [];
    public void Report(string value) { lock (Lines) Lines.Add(value); }
}

public class EfiBuildTests
{
    static string Fixture(string name) => Paths.RepoFile("windows", "app", "IntelXe.Core.Tests", "Fixtures", "efibuild", name);

    static (EfiBuildRequest Request, string Out) Request(TempDir t, string macos = "26", string? smbios = null, bool safari = true)
    {
        FakeEngine.Write(t.Sub("scan", "Report.json"), "{}");
        FakeEngine.Write(t.Sub("scan", "ACPI", "dsdt.aml"), "DSDT....");
        var outDir = t.Sub("out");
        return (new EfiBuildRequest(t.Sub("scan", "Report.json"), t.Sub("scan", "ACPI"), outDir, macos, smbios, safari), outDir);
    }

    static PythonEfiBuilder Builder(EngineLayout e, IProcessRunner r) =>
        new(e, r, (_, _) => Task.FromException<SniffAnswer>(new InvalidOperationException("no scan in this test")));

    // ---- the command lines ----

    [Fact]
    public void TheBuildCommandLine()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, outDir) = Request(t);
        var s = EfiBuildCommand.Build(e, q);
        Assert.Equal(e.Python, s.FileName);
        Assert.Equal(e.EfiBuildDir, s.WorkingDirectory);
        Assert.Equal(new[] { "-I", "-B", "-u", "-X", "utf8", e.Script, "build", "--report", q.ReportPath, "--acpi", q.AcpiDir,
            "--out", outDir, "--macos", "26", "--result", Path.Combine(outDir, "result.json"), "--verbose" }, s.Args);
    }

    [Fact]
    public void TheBuildCommandLineWithItsOptions()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"), cache: t.Sub("cache"));
        var (q, _) = Request(t, "15", "iMacPro1,1", safari: false);
        var a = EfiBuildCommand.Build(e, q).Args.ToList();
        Assert.Equal(["--cache", t.Sub("cache"), "build"], a.Skip(6).Take(3));
        Assert.Equal("15", a[a.IndexOf("--macos") + 1]);
        Assert.Equal("iMacPro1,1", a[a.IndexOf("--smbios") + 1]);
        Assert.Contains("--no-safari", a);
        // an argument with spaces stays one argument: the list goes to ProcessStartInfo.ArgumentList, not a shell
        var spaced = EfiBuildCommand.Build(e, q with { OutDir = t.Sub("out with spaces") }).Args;
        Assert.Contains(t.Sub("out with spaces"), spaced);
    }

    [Theory]
    [InlineData("14", null, "macOS 14")]
    [InlineData("26", "MacPro7,1; calc", "not a Mac model")]
    [InlineData("26", "--no-safari", "not a Mac model")]
    public void ARequestTheBuilderWouldRefuseIsRefusedFirst(string macos, string? smbios, string message)
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t, macos, smbios);
        Assert.Contains(message, Assert.Throws<EfiBuildException>(() => EfiBuildCommand.Build(e, q)).Message);
    }

    [Fact]
    public void AnOutputFolderTheBuilderDidNotMakeIsRefused()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, outDir) = Request(t);
        FakeEngine.Write(Path.Combine(outDir, "notes.txt"), "mine");
        Assert.Contains("not empty", Assert.Throws<EfiBuildException>(() => EfiBuildCommand.Build(e, q)).Message);
        FakeEngine.Write(Path.Combine(outDir, EfiBuildStore.Marker), "made by windows/efibuild");
        EfiBuildCommand.Build(e, q);   // an earlier build's folder may be replaced
        Assert.Throws<EfiBuildException>(() => EfiBuildCommand.Build(e, q with { ReportPath = t.Sub("none.json") }));
        Assert.Throws<EfiBuildException>(() => EfiBuildCommand.Build(e, q with { AcpiDir = t.Sub("none") }));
    }

    [Fact]
    public void TheScanCommandLine()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var s = EfiBuildCommand.Sniff(e, t.Sub("scan 1"));
        Assert.Equal(new[] { "-I", "-B", "-u", "-X", "utf8", e.Script, "sniff", "--out", t.Sub("scan 1") }, s.Args);
    }

    // ---- the result JSON ----

    [Fact]
    public void ABuiltResult()
    {
        var r = EfiBuildResultParser.Parse(File.ReadAllText(Fixture("z790-b580.result.json")));
        Assert.True(r.Ok);
        Assert.Null(r.Error);
        Assert.Equal("1.0.8", r.OpenCore);
        Assert.Equal("MacBookPro16,2", r.Smbios);
        Assert.Equal("26", r.MacOS);
        Assert.Equal(10, r.Decisions.Count);
        Assert.Equal(new EfiBuildDecision("main-menu", "OpCore Simplify", "Select an option:", "1",
            "select the hardware report; the program then asks for the macOS version, GPUs, kexts and tables"), r.Decisions[0]);
        Assert.Contains(r.Decisions, d => d.Rule == "macos" && d.Answer == "25");
        Assert.Contains(r.Changes, c => c.Step == "stage 1 (install guide)" && c.Change == "Kernel > Add: CpuTopologyRebuild.kext off" && c.Why.Length > 0);
        Assert.Contains(r.Changes, c => c.Step == "stage 2" && c.Change.Contains("ResizeAppleGpuBars"));
        Assert.True(r.OcValidate.Ok);
        Assert.Equal(1, r.OcValidate.Found);
        Assert.Single(r.OcValidate.Accepted);
        Assert.StartsWith("Booter->Quirks->ResizeAppleGpuBars is set to 10", r.OcValidate.Accepted[0].Finding);
        Assert.Contains("sleep and wake", r.OcValidate.Accepted[0].Why);
        Assert.Empty(r.OcValidate.Issues);
        Assert.Empty(r.Failures);
        Assert.Single(r.Notes);
        Assert.Equal(5, r.Firmware.Count);
        Assert.Equal(3, r.AfterInstall.Count);
        Assert.Equal(["Intel(R) Arc(TM) B580 Graphics"], r.Cards);
        Assert.Equal("Intel(R) Core(TM) i7-14700K", r.Cpu);
    }

    [Fact]
    public void AStoppedResult()
    {
        var r = EfiBuildResultParser.Parse(File.ReadAllText(Fixture("z490-report-refused.result.json")));
        Assert.False(r.Ok);
        Assert.StartsWith("the report did not pass OpCore Simplify's checks", r.Error);
        Assert.Equal(2, r.Decisions.Count);
        Assert.Empty(r.Changes);
        Assert.Same(OcValidateResult.None, r.OcValidate);
        Assert.Equal("Intel(R) Core(TM) i9-10850K", r.Cpu);
    }

    [Fact]
    public void WhatIsNotABuildResult()
    {
        Assert.Throws<EfiBuildException>(() => EfiBuildResultParser.Parse("not json"));
        Assert.Throws<EfiBuildException>(() => EfiBuildResultParser.Parse("""{"error": "x"}"""));
        Assert.Throws<EfiBuildException>(() => EfiBuildResultParser.Parse("[]"));
        var r = EfiBuildResultParser.Parse("""{"ok": false, "decisions": "not a list", "checks": []}""");
        Assert.Equal("the build did not finish", r.Error);
        Assert.Empty(r.Decisions);
    }

    [Fact]
    public void AnOcvalidateThatCouldNotRun()
    {
        var r = EfiBuildResultParser.Parse("""
            {"ok": false, "error": "the checks failed", "checks": {"ok": false, "failures": ["ocvalidate could not run: x"],
             "checks": {"ocvalidate": {"ok": false, "error": "ocvalidate did not run: x"}}}}
            """);
        Assert.False(r.OcValidate.Ok);
        Assert.Equal("ocvalidate did not run: x", r.OcValidate.Error);
        Assert.Equal(["ocvalidate could not run: x"], r.Failures);
    }

    // ---- the process ----

    static string ResultWithSecret(bool ok) => $$"""
        {"ok": {{(ok ? "true" : "false")}}, "error": {{(ok ? "null" : "\"stopped at " + FakeConfig.Serial + "\"")}},
         "policy": {"macos": "26", "smbios": "MacBookPro16,2"}, "opencore": "1.0.8",
         "decisions": [{"rule": "smbios", "screen": "Customize SMBIOS Model", "question": "Select your option:", "answer": "b",
                        "why": "serial {{FakeConfig.Serial}} and MLB {{FakeConfig.Mlb.ToLowerInvariant()}}"}],
         "changes": [{"step": "stage 2", "change": "ROM {{Convert.ToHexStringLower(FakeConfig.Rom)}} / {{Convert.ToBase64String(FakeConfig.Rom)}}"}],
         "firmware": ["UUID {{FakeConfig.Uuid}}"], "after_install": [],
         "checks": {"ok": true, "failures": [], "notes": [], "checks": {"ocvalidate": {"ok": true, "found": 0, "accepted": [], "issues": []} } } }
        """;

    static string AllText(EfiBuildResult r) => string.Join("\n",
        new[] { r.Error ?? "", r.Smbios, r.OpenCore }
            .Concat(r.Decisions.SelectMany(d => new[] { d.Rule, d.Screen, d.Question, d.Answer, d.Why }))
            .Concat(r.Changes.SelectMany(c => new[] { c.Step, c.Change, c.Why }))
            .Concat(r.Firmware).Concat(r.AfterInstall).Concat(r.Failures).Concat(r.Notes).Concat(r.OcValidate.Issues));

    [Fact]
    public async Task ABuildThroughTheFakePython()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, outDir) = Request(t);
        var runner = new FakeRunner(async (s, stdout, stderr, cancel) =>
        {
            Assert.Equal("build", s.Args[6]);
            var o = FakeRunner.Arg(s, "--out");
            FakeConfig.WriteEfi(o);
            FakeEngine.Write(Path.Combine(o, EfiBuildStore.Marker), "made by windows/efibuild");
            stderr("== Select macOS Version");
            stderr("Please enter the macOS version you want to use (default: macOS Tahoe 26):25");
            stderr("   [macos: macOS 26 (Darwin 25), the policy's version]");
            stderr("SystemSerialNumber: " + FakeConfig.Serial);   // program text: never shown while it works
            stdout("result: ok");
            await File.WriteAllTextAsync(FakeRunner.Arg(s, "--result"), ResultWithSecret(true), cancel);
            return 0;
        });
        var progress = new ListProgress();
        var r = await Builder(e, runner).BuildAsync(q, progress, CancellationToken.None);
        Assert.True(r.Ok, r.Error);
        Assert.Equal(Path.Combine(outDir, "EFI"), r.EfiDir);
        Assert.Equal(Path.Combine(outDir, "build-log.txt"), r.LogPath);
        Assert.Equal(["Select macOS Version", "  macos: macOS 26 (Darwin 25), the policy's version"], progress.Lines);
        var secrets = SmbiosMask.ReadSecrets(Path.Combine(outDir, "EFI", "OC", "config.plist"));
        Assert.NotEmpty(secrets);
        Assert.Empty(SmbiosMask.Find(AllText(r) + string.Join("\n", progress.Lines), secrets));
        Assert.Contains("serial <redacted> and MLB <redacted>", r.Decisions[0].Why);
        Assert.Equal("ROM <redacted> / <redacted>", r.Changes[0].Change);
        Assert.Single(runner.Started);
    }

    [Fact]
    public async Task ABuildThatStopped()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        var runner = new FakeRunner(async (s, _, _, cancel) =>
        {
            FakeConfig.WriteEfi(FakeRunner.Arg(s, "--out"));
            await File.WriteAllTextAsync(FakeRunner.Arg(s, "--result"), ResultWithSecret(false), cancel);
            return EfiBuildCommand.ExitStopped;
        });
        var r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
        Assert.False(r.Ok);
        Assert.Equal("stopped at <redacted>", r.Error);
    }

    [Fact]
    public async Task ExitCodeAndResultMustAgree()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        // 0 with a result that is not ok, and 1 with one that is: neither is a build
        foreach (var (code, ok) in new[] { (0, false), (1, true) })
        {
            var runner = new FakeRunner(async (s, _, _, cancel) =>
            {
                Directory.CreateDirectory(FakeRunner.Arg(s, "--out"));
                await File.WriteAllTextAsync(FakeRunner.Arg(s, "--result"), ResultWithSecret(ok), cancel);
                return code;
            });
            var r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
            Assert.False(r.Ok);
            Assert.NotNull(r.Error);
        }
    }

    [Fact]
    public async Task APinErrorBeforeAResult()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        var runner = new FakeRunner((_, _, stderr, _) =>
        {
            stderr("error: https://example.invalid/x.zip: SHA-256 0000 is not the pinned 1111");
            return Task.FromResult(EfiBuildCommand.ExitPinError);
        });
        var progress = new ListProgress();
        var r = await Builder(e, runner).BuildAsync(q, progress, CancellationToken.None);
        Assert.False(r.Ok);
        Assert.Equal("https://example.invalid/x.zip: SHA-256 0000 is not the pinned 1111", r.Error);
        Assert.Single(progress.Lines);
    }

    [Fact]
    public async Task ACrashWithoutAResult()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        var runner = new FakeRunner((_, _, stderr, _) =>
        {
            stderr("Traceback (most recent call last):");
            stderr("ModuleNotFoundError: No module named 'wmi'");
            return Task.FromResult(1);
        });
        var r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
        Assert.False(r.Ok);
        Assert.Contains("code 1 without a result", r.Error);
        Assert.Contains("No module named 'wmi'", r.Error);
    }

    [Fact]
    public async Task AResultLeftFromAnEarlierBuildIsNotRead()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, outDir) = Request(t);
        FakeEngine.Write(Path.Combine(outDir, EfiBuildStore.Marker), "made by windows/efibuild");
        FakeEngine.Write(Path.Combine(outDir, "result.json"), ResultWithSecret(true));
        var runner = new FakeRunner((_, _, _, _) => Task.FromResult(1));
        var r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
        Assert.False(r.Ok);
    }

    [Fact]
    public async Task CancellingStopsTheBuild()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        var started = new TaskCompletionSource();
        var runner = new FakeRunner(async (_, _, _, cancel) =>
        {
            started.SetResult();
            await Task.Delay(Timeout.Infinite, cancel);
            return 0;
        });
        using var cts = new CancellationTokenSource();
        var build = Builder(e, runner).BuildAsync(q, new ListProgress(), cts.Token);
        await started.Task;
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => build);
    }

    [Fact]
    public async Task AChangedEngineDoesNotRun()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        var (q, _) = Request(t);
        var runner = new FakeRunner((_, _, _, _) => Task.FromResult(0));
        // a json.py beside efibuild.py would be imported by it
        FakeEngine.Write(Path.Combine(e.EfiBuildDir, "json.py"), "import os");
        var r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
        Assert.False(r.Ok);
        Assert.Contains("extra file efibuild/json.py", r.Error);
        File.Delete(Path.Combine(e.EfiBuildDir, "json.py"));
        File.AppendAllText(e.Script, "print('changed')\n");
        r = await Builder(e, runner).BuildAsync(q, new ListProgress(), CancellationToken.None);
        Assert.Contains("changed file efibuild/efibuild.py", r.Error);
        Assert.Empty(runner.Started);
    }

    [Fact]
    public async Task WithoutTheEngine()
    {
        using var t = new TempDir();
        var e = EngineLayout.Beside(t.Path);
        var b = Builder(e, new FakeRunner((_, _, _, _) => Task.FromResult(0)));
        Assert.False(b.IsAvailable);
        Assert.Contains("no builder folder", b.Unavailable);
        Assert.Equal("", b.Versions);
        var (q, _) = Request(t);
        Assert.False((await b.BuildAsync(q, new ListProgress(), CancellationToken.None)).Ok);
    }

    // ---- the engine folder ----

    [Fact]
    public void TheEngineManifest()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        Assert.Equal("", e.Missing());
        Assert.Empty(e.Verify());
        var text = File.ReadAllText(e.Manifest);
        Assert.Contains($"{EngineLayout.Sha256(e.Script)}  efibuild/efibuild.py\n", text);
        Assert.DoesNotContain(EngineLayout.ManifestName, text);

        File.Delete(Path.Combine(e.EfiBuildDir, "intelxe_efibuild", "__init__.py"));
        Assert.Equal(["missing file efibuild/intelxe_efibuild/__init__.py"], e.Verify());
        File.WriteAllText(e.Manifest, "not a line\n");
        Assert.Contains(e.Verify(), p => p.Contains("not a \"sha256  path\" line"));
        File.Delete(e.Manifest);
        Assert.Contains("MANIFEST.sha256", e.Missing());
    }

    [Fact]
    public void TheVersions()
    {
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        Assert.Equal("OpCore Simplify 9aed67e, Hardware Sniffer ff3b9ed, OpenCore 1.0.8, Python 3.12.10", e.Versions());
        Assert.Equal(Path.Combine(t.Path, "engine"), EngineLayout.Beside(t.Path).Root);
    }

    [Theory]
    [InlineData("== Select macOS Version", true, "Select macOS Version")]
    [InlineData("   [macos: the policy's version]", true, "  macos: the policy's version")]
    [InlineData("error: no iasl", true, "error: no iasl")]
    [InlineData("1. macOS Tahoe 26", false, "")]
    [InlineData("[ ] not an answer", false, "")]
    public void TheProgressShown(string line, bool shown, string clean)
    {
        Assert.Equal(shown, ProgressFilter.Shows(line));
        if (shown) Assert.Equal(clean, ProgressFilter.Clean(line));
    }

    // ---- a real process (a shell script for Python; the Windows CI runs the fakes above) ----

    [Fact]
    public async Task ARealProcess()
    {
        if (OperatingSystem.IsWindows()) return;
        using var t = new TempDir();
        var e = FakeEngine.Make(t.Sub("engine"));
        // the "python" reads its arguments as efibuild.py would: --out and --result after the script
        File.WriteAllText(e.Python, """
            #!/bin/sh
            out=""; res=""
            while [ $# -gt 0 ]; do
              case "$1" in --out) out="$2"; shift;; --result) res="$2"; shift;; esac
              shift
            done
            mkdir -p "$out"
            echo "== Select macOS Version" >&2
            echo "   [macos: Darwin 25]" >&2
            printf '%s' '{"ok": true, "policy": {"macos": "26"}, "decisions": [{"rule": "macos", "answer": "25"}]}' > "$res"
            echo "result: ok"
            exit 0
            """.Replace("\r\n", "\n"));
        File.SetUnixFileMode(e.Python, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        FakeEngine.Seal(e);
        var (q, _) = Request(t);
        var progress = new ListProgress();
        var r = await Builder(e, new SystemProcessRunner()).BuildAsync(q with { OutDir = t.Sub("out ü") }, progress, CancellationToken.None);
        Assert.True(r.Ok, r.Error);
        Assert.Equal("25", r.Decisions.Single().Answer);
        Assert.Equal(["Select macOS Version", "  macos: Darwin 25"], progress.Lines);
    }

    [Fact]
    public async Task CancellingARealProcessKillsIt()
    {
        if (OperatingSystem.IsWindows()) return;
        using var t = new TempDir();
        var script = t.Sub("slow.sh");
        File.WriteAllText(script, "#!/bin/sh\necho started >&2\nsleep 60\n");
        File.SetUnixFileMode(script, UnixFileMode.UserRead | UnixFileMode.UserWrite | UnixFileMode.UserExecute);
        using var cts = new CancellationTokenSource();
        var started = new TaskCompletionSource();
        var watch = System.Diagnostics.Stopwatch.StartNew();
        var run = new SystemProcessRunner().RunAsync(new ProcessSpec(script, [], t.Path), _ => { }, _ => started.TrySetResult(), cts.Token);
        await started.Task.WaitAsync(TimeSpan.FromSeconds(20));
        cts.Cancel();
        await Assert.ThrowsAnyAsync<OperationCanceledException>(() => run);
        Assert.True(watch.Elapsed < TimeSpan.FromSeconds(30));
    }
}
