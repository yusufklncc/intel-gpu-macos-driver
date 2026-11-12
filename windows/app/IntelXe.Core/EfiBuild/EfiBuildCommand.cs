// SPDX-License-Identifier: MIT
// The builder's command lines (windows/efibuild/intelxe_efibuild/__main__.py):
//
//   python.exe -I -B -u -X utf8 efibuild.py [--cache DIR] build --report R --acpi A --out O --macos 26|15
//              [--smbios MODEL] [--no-safari] --result O\result.json --verbose
//   python.exe -I -B -u -X utf8 efibuild.py [--cache DIR] sniff --out DIR
//
// -I keeps the user's environment, user site and current folder out of Python (efibuild.py puts only its own folder on
// sys.path), -B writes no __pycache__ beside the engine (its manifest would then have extra files), -u streams the
// output, -X utf8 makes it UTF-8 whatever the Windows code page (-I ignores PYTHONIOENCODING). The arguments go to the
// process as a list (ProcessStartInfo.ArgumentList), never through a shell.
using System.Text.RegularExpressions;

namespace IntelXe.Core.EfiBuild;

/// A process to start: the program, its arguments one by one, the folder it starts in.
public sealed record ProcessSpec(string FileName, IReadOnlyList<string> Args, string WorkingDirectory);

public static partial class EfiBuildCommand
{
    public static readonly string[] MacOSVersions = ["26", "15"];

    [GeneratedRegex(@"^[A-Za-z]{3,16}\d{1,2},\d{1,2}$")]
    private static partial Regex MacModel();

    /// The builder's exit codes: 0 every step passed, 1 the build stopped (the result JSON says why), 2 a pin or input
    /// error (before a result).
    public const int ExitOk = 0, ExitStopped = 1, ExitPinError = 2;

    static List<string> Prefix(EngineLayout engine)
    {
        var a = new List<string> { "-I", "-B", "-u", "-X", "utf8", engine.Script };
        if (!string.IsNullOrEmpty(engine.CacheDir)) a.AddRange(["--cache", engine.CacheDir]);
        return a;
    }

    /// The result JSON's place in the output folder.
    public static string ResultPath(string outDir) => Path.Combine(outDir, "result.json");

    /// The build log's place (the builder's default: OUT\build-log.txt).
    public static string LogPath(string outDir) => Path.Combine(outDir, "build-log.txt");

    /// The build's command line; throws EfiBuildException for a request the builder would refuse.
    public static ProcessSpec Build(EngineLayout engine, EfiBuildRequest q)
    {
        Check(q);
        var a = Prefix(engine);
        a.AddRange(["build", "--report", Path.GetFullPath(q.ReportPath), "--acpi", Path.GetFullPath(q.AcpiDir),
            "--out", Path.GetFullPath(q.OutDir), "--macos", q.MacOS]);
        if (!string.IsNullOrEmpty(q.Smbios)) a.AddRange(["--smbios", q.Smbios]);
        if (!q.Safari) a.Add("--no-safari");
        a.AddRange(["--result", ResultPath(Path.GetFullPath(q.OutDir)), "--verbose"]);
        return new ProcessSpec(engine.Python, a, engine.EfiBuildDir);
    }

    /// The scan's command line (run by the elevated helper).
    public static ProcessSpec Sniff(EngineLayout engine, string outDir)
    {
        if (string.IsNullOrWhiteSpace(outDir)) throw new EfiBuildException("the scan needs a folder");
        var a = Prefix(engine);
        a.AddRange(["sniff", "--out", Path.GetFullPath(outDir)]);
        return new ProcessSpec(engine.Python, a, engine.EfiBuildDir);
    }

    public static void Check(EfiBuildRequest q)
    {
        if (!MacOSVersions.Contains(q.MacOS)) throw new EfiBuildException($"macOS {q.MacOS} is not one the builder makes (26 or 15)");
        if (q.Smbios is { Length: > 0 } m && !MacModel().IsMatch(m)) throw new EfiBuildException($"not a Mac model: {m}");
        if (string.IsNullOrWhiteSpace(q.ReportPath) || !File.Exists(q.ReportPath)) throw new EfiBuildException($"no hardware report {q.ReportPath}");
        if (string.IsNullOrWhiteSpace(q.AcpiDir) || !Directory.Exists(q.AcpiDir)) throw new EfiBuildException($"no ACPI folder {q.AcpiDir}");
        if (string.IsNullOrWhiteSpace(q.OutDir)) throw new EfiBuildException("the build needs an output folder");
        if (Directory.Exists(q.OutDir) && Directory.EnumerateFileSystemEntries(q.OutDir).Any()
            && !File.Exists(Path.Combine(q.OutDir, EfiBuildStore.Marker)))
            throw new EfiBuildException($"{q.OutDir} is not empty and was not made by the builder: choose an empty folder");
    }
}
