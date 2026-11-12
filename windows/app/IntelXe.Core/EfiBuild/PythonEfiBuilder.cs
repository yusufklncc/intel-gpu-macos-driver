// SPDX-License-Identifier: MIT
// IEfiBuilder over windows/efibuild in the embedded Python beside the app (EngineLayout). The build runs as the user:
// efibuild.py build ... --result OUT\result.json --verbose as a child process without a console; its progress lines
// (the program's screens and each answer with its rule) go to the window, its result JSON is read when it ends.
// Exit code 0 is success, 1 a build that stopped (the result says why), 2 a pin or input error (its "error:" line says
// why). The scan needs administrator rights, so it goes through the elevated helper (the delegate the app gives).
namespace IntelXe.Core.EfiBuild;

public sealed class PythonEfiBuilder(EngineLayout engine, IProcessRunner runner,
    Func<SniffRequest, CancellationToken, Task<SniffAnswer>> elevatedSniff) : IEfiBuilder
{
    public EngineLayout Engine { get; } = engine;

    public bool IsAvailable => Engine.Missing().Length == 0;
    public string Unavailable => Engine.Missing();
    public string Versions => IsAvailable ? Engine.Versions() : "";

    void CheckEngine()
    {
        if (Engine.Missing() is { Length: > 0 } missing) throw new EfiBuildException(missing);
        if (Engine.Verify() is { Count: > 0 } problems)
            throw new EfiBuildException("the builder beside the app changed: " + string.Join("; ", problems.Take(5)));
    }

    public async Task<HardwareScan> ScanAsync(string outDir, IProgress<string> progress, CancellationToken cancel)
    {
        await Task.Run(CheckEngine, cancel);
        var answer = await elevatedSniff(new SniffRequest(Path.GetFullPath(outDir)), cancel);
        foreach (var line in answer.Lines.Where(ProgressFilter.Shows)) progress.Report(ProgressFilter.Clean(line));
        var scan = answer.ToScan();
        // the helper kept only these; the report and a DSDT must be there for the build
        if (!File.Exists(scan.ReportPath)) throw new EfiBuildException("the scan left no Report.json");
        var (kept, _) = AcpiTables.KeepOnlyBuildTables(scan.AcpiDir);
        if (!kept.Any(t => AcpiTables.Signature(Path.Combine(scan.AcpiDir, t)) == "DSDT")) throw new EfiBuildException("the scan left no DSDT");
        return scan with { Tables = kept };
    }

    public async Task<EfiBuildResult> BuildAsync(EfiBuildRequest request, IProgress<string> progress, CancellationToken cancel)
    {
        var outDir = Path.GetFullPath(request.OutDir);
        var resultPath = EfiBuildCommand.ResultPath(outDir);
        var logPath = EfiBuildCommand.LogPath(outDir);
        ProcessSpec spec;
        try
        {
            await Task.Run(CheckEngine, cancel);
            spec = EfiBuildCommand.Build(Engine, request with { OutDir = outDir });
        }
        catch (EfiBuildException e) { return EfiBuildResult.Failed(e.Message); }

        // the builder writes result.json at its end: one left from an earlier build in this folder must not be read
        if (File.Exists(resultPath) && File.Exists(Path.Combine(outDir, EfiBuildStore.Marker))) File.Delete(resultPath);
        var tail = new Queue<string>();
        void Err(string line)
        {
            lock (tail) { tail.Enqueue(line); if (tail.Count > 40) tail.Dequeue(); }
            if (ProgressFilter.Shows(line)) progress.Report(ProgressFilter.Clean(line));
        }
        void Out(string line) { lock (tail) { tail.Enqueue(line); if (tail.Count > 40) tail.Dequeue(); } }

        int code = await runner.RunAsync(spec, Out, Err, cancel);   // throws OperationCanceledException on cancel

        string[] last;
        lock (tail) last = [.. tail];
        // the identity the build generated, in every form: removed from everything shown, whatever the builder did
        var efi = Path.Combine(outDir, "EFI");
        var secrets = SmbiosMask.ReadSecrets(Path.Combine(efi, "OC", "config.plist"));
        string Redact(string s) => SmbiosMask.Redact(s, secrets);

        if (File.Exists(resultPath) && code is EfiBuildCommand.ExitOk or EfiBuildCommand.ExitStopped)
        {
            EfiBuildResult r;
            try { r = EfiBuildResultParser.Parse(await File.ReadAllTextAsync(resultPath, cancel), Redact, logPath, resultPath, Directory.Exists(efi) ? efi : ""); }
            catch (EfiBuildException e) { return EfiBuildResult.Failed(Redact(e.Message), logPath, resultPath); }
            if (code == EfiBuildCommand.ExitOk && !r.Ok) return r with { Ok = false, Error = r.Error ?? "the builder exited 0 with a result that is not ok" };
            if (code != EfiBuildCommand.ExitOk && r.Ok) return r with { Ok = false, Error = $"the builder ended with code {code}" };
            return r;
        }
        var error = last.LastOrDefault(l => l.StartsWith("error:", StringComparison.Ordinal))?[6..].Trim()
                    ?? $"the builder ended with code {code} without a result: {string.Join(" / ", last.Where(l => l.Trim().Length > 0).TakeLast(4))}";
        return EfiBuildResult.Failed(Redact(error), File.Exists(logPath) ? logPath : "", "");
    }
}

/// Which of the builder's --verbose lines the window shows while it works: the screens ("== Select macOS Version"), the
/// rule that answered ("   [macos: ...]") and errors. The program's own text (menus, lists of devices) stays in the
/// build log, which the builder writes with the SMBIOS values removed.
public static class ProgressFilter
{
    public static bool Shows(string line)
    {
        var t = line.TrimStart();
        return t.StartsWith("== ", StringComparison.Ordinal) || (t.StartsWith('[') && t.EndsWith(']') && t.Contains(": ")) || t.StartsWith("error:", StringComparison.Ordinal);
    }

    public static string Clean(string line)
    {
        var t = line.Trim();
        if (t.StartsWith("== ", StringComparison.Ordinal)) return t[3..].Trim();
        if (t.StartsWith('[') && t.EndsWith(']')) return "  " + t[1..^1];
        return t;
    }
}
