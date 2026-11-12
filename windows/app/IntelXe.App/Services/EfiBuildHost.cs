// SPDX-License-Identifier: MIT
// The Windows side of building the OpenCore EFI: the elevated helper's "sniff" verb (SniffHelperMain: this exe with
// "--efi-helper sniff --scan DIR --out FILE", started through "runas"), starting it (SniffRunner), and the build's state
// that the Build and Set up pages share (BuildSession): the builder (PythonEfiBuilder over the engine beside the exe),
// the last scan, the macOS version, the build's progress and result, the saved copy. Scans and builds go to
// %LOCALAPPDATA%\IntelXe Manager\efibuild; the builder's download cache is its own (%LOCALAPPDATA%\IntelXe\efibuild).
using System.ComponentModel;
using System.Diagnostics;
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXe.Core.EfiBuild;
using IntelXe.Core.Setup;

namespace IntelXe.App;

/// "IntelXeManager.exe --efi-helper sniff --scan DIR --out FILE": Hardware Sniffer and the ACPI tables, with
/// administrator rights, through the engine beside this exe (checked against its manifest first).
static class SniffHelperMain
{
    public static int Run(string[] args)
    {
        SniffAnswer a;
        string? output = null;
        try
        {
            var q = SniffRequest.Parse(args);
            output = q.Out;
            a = !HelperMain.IsAdministrator()
                ? new SniffAnswer { Ok = false, Error = "reading the ACPI tables needs administrator rights (the app starts the scan with them)" }
                : SniffHelper.RunAsync(q, EngineLayout.Beside(AppContext.BaseDirectory), new SystemProcessRunner()).GetAwaiter().GetResult();
        }
        catch (Exception e)
        {
            a = new SniffAnswer { Ok = false, Error = e.Message };
            int i = Array.IndexOf(args, "--out");
            if (i >= 0 && i + 1 < args.Length) output = args[i + 1];
        }
        try
        {
            if (output is null) Console.Out.Write(a.ToJson());
            else HelperMain.Answer(output, a.ToJson());
        }
        catch (Exception) { return 3; }
        return a.Ok ? 0 : 1;
    }
}

/// The app's side of the scan: the helper through "runas" (the UAC prompt), its answer from a file of the app's.
static class SniffRunner
{
    public static async Task<SniffAnswer> RunElevatedAsync(SniffRequest q, CancellationToken cancel)
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "helper");
        Directory.CreateDirectory(dir);
        var output = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".json");
        var psi = new ProcessStartInfo(Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "IntelXeManager.exe"),
            CommandLine.Join((q with { Out = output }).ToArgs()))
        {
            UseShellExecute = true, Verb = "runas", WorkingDirectory = AppContext.BaseDirectory, WindowStyle = ProcessWindowStyle.Hidden,
        };
        try
        {
            Process p;
            try { p = Process.Start(psi) ?? throw new EfiBuildException("the helper did not start"); }
            catch (Win32Exception e) when (e.NativeErrorCode == 1223) { throw new EfiBuildException(L.Get("Build_Elevation_Cancelled")); }
            using (p) await p.WaitForExitAsync(cancel);
            if (!File.Exists(output)) throw new EfiBuildException(L.Get("Build_Helper_NoAnswer"));
            return SniffAnswer.FromJson(await File.ReadAllTextAsync(output, cancel));
        }
        finally
        {
            try { File.Delete(output); } catch (Exception) { }
        }
    }
}

/// What the Build page shows and the Set up page reads; it outlives the pages (navigating away keeps a build running).
sealed class BuildSession
{
    public static BuildSession Current { get; } = new();

    public static string DataRoot => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "efibuild");

    public IEfiBuilder Builder { get; } = new PythonEfiBuilder(EngineLayout.Beside(AppContext.BaseDirectory), new SystemProcessRunner(), SniffRunner.RunElevatedAsync);
    public EfiBuildStore Store { get; } = new(DataRoot);

    public HardwareScan? Scan { get; private set; }
    public string? ScanError { get; private set; }
    public bool Scanning { get; private set; }

    public string MacOS { get; set; } = "26";

    public bool Building { get; private set; }
    public List<string> Progress { get; } = [];
    public EfiBuildResult? Result { get; private set; }
    /// the app's own engine on the built EFI: the stage-2 lines it would still change (empty: it agrees)
    public IReadOnlyList<string>? Stage2Left { get; private set; }
    public IReadOnlyList<(string Key, string Value)> Platform { get; private set; } = [];
    public string? Saved { get; set; }
    public string? SaveError { get; set; }
    CancellationTokenSource? cancel;
    bool loaded;

    public event Action? Changed;
    void Notify() => Changed?.Invoke();

    static SetupProgress Setup => Settings.Current.Setup;

    /// The last scan and build of an earlier session, from settings.json, when their folders are still there.
    public void Load()
    {
        if (loaded) return;
        loaded = true;
        var s = Setup;
        if (s.LastScan.Length > 0 && File.Exists(Path.Combine(s.LastScan, "Report.json")) && Directory.Exists(Path.Combine(s.LastScan, "ACPI")))
            Scan = new HardwareScan(Path.Combine(s.LastScan, "Report.json"), Path.Combine(s.LastScan, "ACPI"),
                [.. Directory.EnumerateFiles(Path.Combine(s.LastScan, "ACPI")).Select(f => Path.GetFileName(f)!).Order(StringComparer.Ordinal)]);
        if (BuiltOutDir() is { } outDir && File.Exists(EfiBuildCommand.ResultPath(outDir)))
        {
            try
            {
                var efi = EfiBuildStore.EfiOf(outDir);
                var secrets = SmbiosMask.ReadSecrets(Path.Combine(efi, "OC", "config.plist"));
                Result = EfiBuildResultParser.Parse(File.ReadAllText(EfiBuildCommand.ResultPath(outDir)), t => SmbiosMask.Redact(t, secrets),
                    EfiBuildCommand.LogPath(outDir), EfiBuildCommand.ResultPath(outDir), efi);
                Platform = SmbiosMask.PlatformSummary(Path.Combine(efi, "OC", "config.plist"));
                MacOS = s.EfiMacOS.Length > 0 ? s.EfiMacOS : MacOS;
            }
            catch (Exception) { Result = null; }
        }
    }

    /// The output folder of the last successful build when its EFI is still there.
    public static string? BuiltOutDir()
    {
        var dir = Setup.EfiOutDir;
        if (dir.Length == 0) return null;
        try { EfiBuildStore.EfiOf(dir); return dir; }
        catch (Exception) { return null; }
    }

    /// Its EFI folder (holding BOOT and OC), or null.
    public static string? BuiltEfiDir()
    {
        try { return BuiltOutDir() is { } d ? EfiBuildStore.EfiOf(d) : null; }
        catch (Exception) { return null; }
    }

    public async Task ScanAsync()
    {
        if (Scanning || Building) return;
        Scanning = true;
        ScanError = null;
        Notify();
        try
        {
            var dir = Store.NewScanDir(DateTime.Now);
            var progress = new Progress<string>(_ => { });
            Scan = await Builder.ScanAsync(dir, progress, CancellationToken.None);
            // the earlier scans are not needed any more (each holds this PC's report and tables)
            foreach (var old in Directory.EnumerateDirectories(Store.ScansDir).Where(d => !string.Equals(Path.GetFullPath(d), Path.GetFullPath(dir), StringComparison.OrdinalIgnoreCase)))
                if (File.Exists(Path.Combine(old, "Report.json")) || Directory.Exists(Path.Combine(old, "ACPI")))
                    try { Directory.Delete(old, true); } catch (Exception) { }
            Setup.LastScan = dir;
            Settings.Current.Save();
        }
        catch (Exception e) { ScanError = e.Message; }
        finally
        {
            Scanning = false;
            Notify();
        }
    }

    public async Task BuildAsync()
    {
        if (Building || Scanning || Scan is null) return;
        Building = true;
        Result = null;
        Stage2Left = null;
        Platform = [];
        Saved = null;
        SaveError = null;
        lock (Progress) Progress.Clear();
        cancel = new CancellationTokenSource();
        Notify();
        var outDir = Store.NewBuildDir(DateTime.Now);
        try
        {
            // reported on this (UI) thread
            var progress = new Progress<string>(line => { lock (Progress) { Progress.Add(line); if (Progress.Count > 200) Progress.RemoveAt(0); } Notify(); });
            var r = await Builder.BuildAsync(new EfiBuildRequest(Scan.ReportPath, Scan.AcpiDir, outDir, MacOS), progress, cancel.Token);
            Result = r;
            if (r.Ok && r.EfiDir.Length > 0)
            {
                var config = Path.Combine(r.EfiDir, "OC", "config.plist");
                Platform = SmbiosMask.PlatformSummary(config);
                try { Stage2Left = await Task.Run(() => EfiBuildStore.Stage2Left(r.EfiDir, safari: true)); }
                catch (Exception e) { Stage2Left = [e.Message]; }
                Settings.Current.Setup = SetupWizard.RecordEfi(Setup, outDir, MacOS, DateTime.Now);
                Settings.Current.Save();
                Store.PruneBuilds(outDir);
            }
        }
        catch (OperationCanceledException) { Result = EfiBuildResult.Failed(L.Get("Build_Cancelled")); }
        catch (Exception e) { Result = EfiBuildResult.Failed(e.Message); }
        finally
        {
            Building = false;
            cancel?.Dispose();
            cancel = null;
            Notify();
        }
    }

    public void CancelBuild() => cancel?.Cancel();

    /// The stick page wrote a stick with this EFI folder: the setup's stick step is done when it is the built EFI.
    public static void RecordStick(string? efiWritten)
    {
        if (BuiltEfiDir() is not { } built) return;
        var next = SetupWizard.RecordStick(Setup, efiWritten, built);
        if (ReferenceEquals(next, Setup)) return;
        Settings.Current.Setup = next;
        Settings.Current.Save();
    }

    /// The wizard's inputs from what the app knows now.
    public SetupInputs SetupInputs()
    {
        var state = AppState.Current;
        return new SetupInputs(state.Facts is not null, SetupWizard.Blocking(state.Facts, state.Checks), Builder.IsAvailable,
            Builder.IsAvailable ? Text.Empty : Text.Of("Setup_Blocked_NoBuilder"), BuiltOutDir() is not null);
    }
}
