// SPDX-License-Identifier: MIT
namespace IntelXe.Core.Report;

/// What tools/report/collect-windows.ps1 printed at its end: "Report ID: XR-1A2B3C4D" and "File:      C:\...\xe-report-XR-1A2B3C4D.zip".
public sealed record ReportResult(string Id, string File);

public static class ReportRun
{
    /// The report ID and file from the script's output; null when it wrote no report.
    public static ReportResult? Parse(IEnumerable<string> lines)
    {
        string? id = null, file = null;
        foreach (var raw in lines)
        {
            var line = raw.Trim();
            if (line.StartsWith("Report ID:", StringComparison.Ordinal)) id = line["Report ID:".Length..].Trim();
            else if (line.StartsWith("File:", StringComparison.Ordinal)) file = line["File:".Length..].Trim();
        }
        if (id is null || file is null || !IsReportId(id)) return null;
        return new ReportResult(id, file);
    }

    /// XR- and eight hex digits, as the collectors make them.
    public static bool IsReportId(string id) =>
        id.Length == 11 && id.StartsWith("XR-", StringComparison.Ordinal) && id[3..].All(Uri.IsHexDigit);

    /// The arguments for Windows PowerShell 5.1 (part of Windows): this one run bypasses the execution policy, as the
    /// guide tells users to run it by hand.
    public static IReadOnlyList<string> PowerShellArguments(string script, string outDir, bool acpi, bool bootLogs = false)
    {
        var args = new List<string> { "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", script, "-OutDir", outDir };
        if (acpi) args.Add("-Acpi");
        if (bootLogs) args.Add("-BootLogs");
        return args;
    }

    /// The same run as administrator, for -BootLogs on the EFI partitions (mounting one needs it): started with "runas",
    /// which cannot pass its output back through a pipe, so -Command runs the script with every stream it prints (Write-Host
    /// included) written to outputFile, which the app reads when the run has ended. Paths are single-quoted PowerShell
    /// strings.
    public static IReadOnlyList<string> ElevatedArguments(string script, string outDir, bool acpi, bool bootLogs, string outputFile)
    {
        static string Q(string s) => "'" + s.Replace("'", "''") + "'";
        var command = "& " + Q(script) + " -OutDir " + Q(outDir) + (acpi ? " -Acpi" : "") + (bootLogs ? " -BootLogs" : "")
            + " *>&1 | Out-File -FilePath " + Q(outputFile) + " -Encoding UTF8";
        return ["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", command];
    }
}
