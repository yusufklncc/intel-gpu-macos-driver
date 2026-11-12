// SPDX-License-Identifier: MIT
// The elevated helper's "sniff" verb (DESIGN-efi-build.md section 5): the app starts its own exe through "runas" as
//
//   IntelXeManager.exe --efi-helper sniff --scan DIR --out ANSWER.json
//
// and the helper runs the engine beside the exe (never one named on the command line), after checking it against its
// manifest: efibuild.py sniff --out DIR, which runs the pinned Hardware Sniffer (WMI) and the pinned acpidump.exe (the
// ACPI tables need administrator rights). Of the dump only the tables the build reads are kept (DSDT, SSDT, APIC, DMAR):
// the builder keeps only those, and the helper deletes any other table it finds afterwards, so MSDM (the Windows
// product key), SLIC and the rest never stay on disk. The scan folder must not exist yet and no folder above it may be a
// link: the helper has administrator rights and the folder is the user's.
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using IntelXe.Core.Efi;

namespace IntelXe.Core.EfiBuild;

public sealed record SniffRequest(string ScanDir, string? Out = null)
{
    public const string Verb = "sniff";

    public IReadOnlyList<string> ToArgs()
    {
        var a = new List<string> { EfiHelperRequest.Switch, Verb, "--scan", ScanDir };
        if (!string.IsNullOrEmpty(Out)) a.AddRange(["--out", Out]);
        return a;
    }

    /// True for "--efi-helper sniff ...".
    public static bool Is(IReadOnlyList<string> args) => args.Count >= 2 && args[0] == EfiHelperRequest.Switch && args[1] == Verb;

    public static SniffRequest Parse(IReadOnlyList<string> args)
    {
        if (!Is(args)) throw new EfiBuildException($"the scan's command line starts with {EfiHelperRequest.Switch} {Verb}");
        string? scan = null, output = null;
        for (int i = 2; i < args.Count; i++)
        {
            if (i + 1 >= args.Count) throw new EfiBuildException($"the scan does not take {args[i]} without a value");
            switch (args[i])
            {
                case "--scan" when scan is null: scan = args[++i]; break;
                case "--out" when output is null: output = args[++i]; break;
                default: throw new EfiBuildException($"the scan does not take {args[i]}" + (args[i] is "--scan" or "--out" ? " twice" : ""));
            }
        }
        if (string.IsNullOrWhiteSpace(scan) || !Path.IsPathFullyQualified(scan)) throw new EfiBuildException("the scan needs --scan with a full path");
        return new SniffRequest(scan, output);
    }
}

/// The helper's answer, as JSON in the --out file.
public sealed class SniffAnswer
{
    [JsonPropertyName("ok")] public bool Ok { get; set; }
    [JsonPropertyName("error")] public string? Error { get; set; }
    [JsonPropertyName("report")] public string? Report { get; set; }
    [JsonPropertyName("acpi")] public string? Acpi { get; set; }
    [JsonPropertyName("tables")] public List<string> Tables { get; set; } = [];
    /// Tables the helper deleted because the build does not read them (by signature).
    [JsonPropertyName("removed")] public List<string> Removed { get; set; } = [];
    /// The engine's output, for the window (Hardware Sniffer's progress lines; it prints no serial numbers).
    [JsonPropertyName("lines")] public List<string> Lines { get; set; } = [];

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };

    public string ToJson() => JsonSerializer.Serialize(this, Json);
    public static SniffAnswer FromJson(string json) => JsonSerializer.Deserialize<SniffAnswer>(json, Json) ?? throw new EfiBuildException("the scan's answer is empty");

    public HardwareScan ToScan() =>
        Ok && Report is not null && Acpi is not null ? new HardwareScan(Report, Acpi, Tables) : throw new EfiBuildException(Error ?? "the scan failed");
}

public static class AcpiTables
{
    /// The signatures OpCore Simplify reads (windows/efibuild sniff.KEEP_TABLES).
    public static readonly string[] Kept = ["DSDT", "SSDT", "APIC", "DMAR"];

    /// A table's signature: its first four bytes ("" for a shorter file).
    public static string Signature(string path)
    {
        var b = new byte[4];
        using var f = File.OpenRead(path);
        return f.ReadAtLeast(b, 4, throwOnEndOfStream: false) == 4 ? Encoding.ASCII.GetString(b) : "";
    }

    /// Deletes every file in dir that is not a kept table (by signature, not by name) and every subfolder; returns the
    /// kept and the removed names.
    public static (List<string> Kept, List<string> Removed) KeepOnlyBuildTables(string dir)
    {
        var kept = new List<string>();
        var removed = new List<string>();
        foreach (var sub in Directory.EnumerateDirectories(dir))
        {
            removed.Add(Path.GetFileName(sub) + Path.DirectorySeparatorChar);
            Directory.Delete(sub, true);
        }
        foreach (var f in Directory.EnumerateFiles(dir).Order(StringComparer.Ordinal))
        {
            var name = Path.GetFileName(f);
            if (Kept.Contains(Signature(f), StringComparer.Ordinal)) kept.Add(name);
            else { File.Delete(f); removed.Add(name); }
        }
        return (kept, removed);
    }
}

public static class SniffHelper
{
    /// The scan: refuses a scan folder that exists or lies under a link, checks the engine, runs efibuild.py sniff,
    /// keeps only the build's tables. folderIsLink tells whether a folder is a link (reparse point on Windows).
    public static async Task<SniffAnswer> RunAsync(SniffRequest q, EngineLayout engine, IProcessRunner runner,
        Func<DirectoryInfo, bool>? folderIsLink = null, CancellationToken cancel = default)
    {
        var a = new SniffAnswer();
        folderIsLink ??= d => (d.Attributes & FileAttributes.ReparsePoint) != 0;
        try
        {
            var scan = Path.GetFullPath(q.ScanDir);
            if (Directory.Exists(scan) || File.Exists(scan)) throw new EfiBuildException($"{scan} exists already: the scan writes only into a new folder");
            for (var d = new DirectoryInfo(scan).Parent; d is not null; d = d.Parent)
                if (d.Exists && folderIsLink(d)) throw new EfiBuildException($"{d.FullName} is a link: nothing scanned");
            if (engine.Missing() is { Length: > 0 } missing) throw new EfiBuildException(missing);
            if (engine.Verify() is { Count: > 0 } problems) throw new EfiBuildException("the builder beside the app changed: " + string.Join("; ", problems.Take(5)));

            var lines = new List<string>();
            void Line(string s) { lock (lines) { if (lines.Count < 400) lines.Add(s); } }
            int code = await runner.RunAsync(EfiBuildCommand.Sniff(engine, scan), Line, Line, cancel);
            a.Lines = lines;
            if (code != 0)
                throw new EfiBuildException(lines.LastOrDefault(l => l.StartsWith("error:", StringComparison.Ordinal))?[6..].Trim()
                                            ?? $"the scan ended with code {code}: {string.Join(" / ", lines.TakeLast(3))}");
            var report = Path.Combine(scan, "Report.json");
            var acpi = Path.Combine(scan, "ACPI");
            if (!File.Exists(report)) throw new EfiBuildException("the scan wrote no Report.json");
            if (!Directory.Exists(acpi)) throw new EfiBuildException("the scan wrote no ACPI tables");
            var (kept, removed) = AcpiTables.KeepOnlyBuildTables(acpi);
            if (!kept.Any(t => AcpiTables.Signature(Path.Combine(acpi, t)) == "DSDT")) throw new EfiBuildException("the ACPI dump has no DSDT");
            a.Report = report;
            a.Acpi = acpi;
            a.Tables = kept;
            a.Removed = removed;
            a.Ok = true;
        }
        catch (OperationCanceledException) { throw; }
        catch (Exception e) when (e is EfiBuildException or IOException or UnauthorizedAccessException)
        {
            a.Ok = false;
            a.Error = e.Message;
        }
        finally
        {
            // whatever happened: no table the build does not read stays in the scan folder
            var acpi = Path.Combine(Path.GetFullPath(q.ScanDir), "ACPI");
            if (!a.Ok && Directory.Exists(acpi))
                try { AcpiTables.KeepOnlyBuildTables(acpi); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return a;
    }
}
