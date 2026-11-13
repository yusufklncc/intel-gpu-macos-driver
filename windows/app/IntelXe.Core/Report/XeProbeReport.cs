// SPDX-License-Identifier: MIT
// XeProbe's results in a macOS report bundle (tools/report/collect-macos.sh --probe, docs/PROBE-MODE.md "Results"):
// xe-report-XR-XXXXXXXX.tar.gz, or the folder it unpacks to, holds probe/results.txt (sysctl debug.xeprobe.results),
// probe/info.txt (debug.xeprobe.info; older reports lack it), probe/log.txt, probe/nvram-breadcrumb.txt, probe/panics.txt
// (the panic reports in the bundle that mention XeProbe), probe/ioreg.txt and probe/history.txt (the runs IntelXe Manager
// made on macOS). The bundle is untrusted: the archive is read with System.Formats.Tar over a GZipStream, in memory and
// never unpacked to disk; only regular files under the report's folder are read (probe/ and the panic files panics.txt
// lists), nothing with "..", no absolute path, no link, each file and the whole within a size limit.
using System.Formats.Tar;
using System.Globalization;
using System.IO.Compression;
using System.Text;
using System.Text.RegularExpressions;

namespace IntelXe.Core.Report;

/// A stage's status as the kernel prints it (kStatus in kernel/IntelXe/xe_probe.cpp).
public enum XeProbeStatus { NotRun, Ok, Failed, Refused, Stalled, Running }

/// What the page shows for a stage.
public enum XeProbeVerdict { Passed, Failed, Refused, Stalled, NotRun, Running }

/// One line "stage %d %-9s %-8s %5u ms  %s".
public sealed record XeProbeStage(int Number, string Name, XeProbeStatus Status, uint Ms, string Detail)
{
    public XeProbeVerdict Verdict => XeProbeReports.Verdict(Status);
}

/// results.txt: the header ("XeProbe v1 on 8086:e20b (BMG-G21)" or "XeProbe not attached") and the stages.
public sealed class XeProbeResults
{
    public bool Attached { get; init; }
    public int? Version { get; init; }
    /// "8086:e20b"; null when not attached or not printed.
    public string? Device { get; init; }
    /// The family name; null for "family unknown".
    public string? Family { get; init; }
    public IReadOnlyList<XeProbeStage> Stages { get; init; } = [];
    /// Lines that are neither the header nor a stage, as they were.
    public IReadOnlyList<string> Other { get; init; } = [];
}

/// info.txt: key=value lines; unknown keys are ignored.
public sealed class XeProbeInfo
{
    public int? Version { get; init; }
    public string? Device { get; init; }
    /// null for "unknown".
    public string? Family { get; init; }
    /// The GuC image the family uses (lnl_guc_70.bin); null when none.
    public string? GucImage { get; init; }
    /// The version of the GuC built into the kext for it (70.72.1); null for "no".
    public string? GucBuiltIn { get; init; }
    /// The images built in, "bmg 70.72.1" each.
    public IReadOnlyList<string> Images { get; init; } = [];
    public bool? Nvram { get; init; }
    public bool? PanicOnStall { get; init; }
    public int? LastStage { get; init; }
}

/// The NVRAM breadcrumb: the last step the previous boot started ("XeProbe v1 8086:7d55 stage 3 guc step GuC DMA seq 7").
public sealed record XeProbeBreadcrumb(string Text, string? Device, int? Stage, string? StageName, string? Step, uint? Seq);

/// A panic report probe/panics.txt lists: its path in the bundle and the watchdog's line, when the file is there and has one.
public sealed record XeProbePanic(string File, bool Found, string? Line);

/// One run IntelXe Manager made: "== <when> boot <boottime> stage <what> exit <code>", then results.txt at that moment.
public sealed record XeProbeRun(string WhenText, DateTimeOffset? When, string BootText, DateTimeOffset? Boot, string What, int Exit, XeProbeResults Results)
{
    /// The stage it ran (1-5, 9), or null for "runN".
    public int? Stage => int.TryParse(What, NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : null;
    /// N for "runN" (stages 1..N), else null.
    public int? RunTo => What.StartsWith("run", StringComparison.Ordinal) && int.TryParse(What[3..], NumberStyles.None, CultureInfo.InvariantCulture, out var n) ? n : null;
}

/// What a report bundle says about XeProbe.
public sealed class XeProbeReport
{
    public required string Id { get; init; }
    /// The file or folder it was read from.
    public required string Source { get; init; }
    /// The bundle has a probe/ folder (collected with --probe).
    public bool HasProbe { get; init; }
    /// results.txt; null when absent.
    public XeProbeResults? Results { get; init; }
    /// info.txt; null when absent (older reports).
    public XeProbeInfo? Info { get; init; }
    /// The breadcrumb; null when there is none ("no breadcrumb in NVRAM ...") or no file.
    public XeProbeBreadcrumb? Breadcrumb { get; init; }
    /// nvram-breadcrumb.txt's line as it was, when it holds no breadcrumb.
    public string? BreadcrumbNote { get; init; }
    public IReadOnlyList<XeProbePanic> Panics { get; init; } = [];
    public IReadOnlyList<XeProbeRun> History { get; init; } = [];
    public string Log { get; init; } = "";
    /// ioreg.txt holds the XeProbe class; null when there is no ioreg.txt.
    public bool? IoregAttached { get; init; }
    /// Files passed over (links, paths outside the report, files over the size limit), for the page.
    public IReadOnlyList<Text> Notes { get; init; } = [];

    public bool Attached => Results?.Attached ?? IoregAttached ?? false;
    public string? Device => Results?.Device ?? Info?.Device;
    public string? Family => Results?.Family ?? Info?.Family;
    /// The boots the history covers.
    public int HistoryBoots => History.Select(r => r.BootText).Distinct(StringComparer.Ordinal).Count();
}

/// The bundle cannot be read as a report; Reason is the text for the page, Message the same in English.
public sealed class XeProbeReportException(Text reason, string message) : Exception(message)
{
    public Text Reason { get; } = reason;
}

public static partial class XeProbeReports
{
    /// Each file read from the bundle is at most this large.
    public const int FileLimit = 2 * 1024 * 1024;
    /// All files read together.
    public const long TotalLimit = 16L * 1024 * 1024;
    /// The archive unpacks (in memory, as it streams by) to at most this much.
    public const long UnpackedLimit = 1L << 30;
    /// At most this many panic reports from panics.txt.
    public const int PanicLimit = 32;

    public const string ProbeFolder = "probe";
    public static readonly IReadOnlyList<string> ProbeFiles =
        ["results.txt", "info.txt", "log.txt", "nvram-breadcrumb.txt", "panics.txt", "ioreg.txt", "history.txt"];

    /// The resw keys this file's Text values use (LocalisationTests and XeProbeReportTests check both tables).
    public static readonly IReadOnlyList<string> Keys =
    [
        "XeProbe_Error_Missing", "XeProbe_Error_NotReport", "XeProbe_Error_Zip", "XeProbe_Error_Unreadable", "XeProbe_Error_Several",
        "XeProbe_Note_Link", "XeProbe_Note_Outside", "XeProbe_Note_TooLarge", "XeProbe_Note_BadPanicPath",
        "XeProbe_Sum_NoProbe", "XeProbe_Sum_NoResults", "XeProbe_Sum_NotAttached", "XeProbe_Sum_NoneRun",
        "XeProbe_Sum_Passed", "XeProbe_Sum_PassedOne", "XeProbe_Sum_Refused", "XeProbe_Sum_RefusedOne",
        "XeProbe_Sum_Failed", "XeProbe_Sum_FailedBare", "XeProbe_Sum_Stalled", "XeProbe_Sum_StalledBare", "XeProbe_Sum_Running",
        "XeProbe_Sum_Panics",
        "XeProbe_Verdict_Passed", "XeProbe_Verdict_Failed", "XeProbe_Verdict_Refused", "XeProbe_Verdict_Stalled",
        "XeProbe_Verdict_NotRun", "XeProbe_Verdict_Running",
    ];

    [GeneratedRegex(@"^xe-report-(XR-[0-9A-Fa-f]{8})$", RegexOptions.CultureInvariant)]
    private static partial Regex TopRe();

    [GeneratedRegex(@"^XeProbe v(\d{1,4}) on (8086:[0-9A-Fa-f]{4}) \((.*)\)$", RegexOptions.CultureInvariant)]
    private static partial Regex HeaderRe();

    [GeneratedRegex(@"^stage\s+(\d{1,2})\s+([A-Za-z0-9_-]+)\s+(not run|ok|FAILED|refused|STALLED|running)\s+(\d{1,10})\s+ms(?:\s+(.*))?$", RegexOptions.CultureInvariant)]
    private static partial Regex StageRe();

    [GeneratedRegex(@"^XeProbe v\d{1,4} ([0-9A-Fa-f]{4}:[0-9A-Fa-f]{4}) stage (-?\d{1,2}) (\S+) step (.*) seq (\d{1,10})$", RegexOptions.CultureInvariant)]
    private static partial Regex CrumbRe();

    [GeneratedRegex(@"^== (\S+) boot (\S+) stage (\S+) exit (-?\d{1,10})$", RegexOptions.CultureInvariant)]
    private static partial Regex RunRe();

    // ---- reading a bundle ----

    /// A report bundle: a .tar.gz / .tgz, or a folder (the report's own, the one holding it, or its probe/ folder).
    public static XeProbeReport Read(string path, CancellationToken cancel = default)
    {
        if (Directory.Exists(path)) return ReadFolder(path, cancel);
        if (File.Exists(path)) return ReadArchive(path, cancel);
        throw Error(Text.Of("XeProbe_Error_Missing", path), $"{path} does not exist");
    }

    /// The archive, read twice at most: probe/ first, then the panic reports probe/panics.txt lists.
    public static XeProbeReport ReadArchive(string file, CancellationToken cancel = default)
    {
        var name = Path.GetFileName(file);
        using (var f = File.OpenRead(file))
        {
            Span<byte> head = stackalloc byte[2];
            int n = f.ReadAtLeast(head, 2, throwOnEndOfStream: false);
            if (n == 2 && head[0] == (byte)'P' && head[1] == (byte)'K')
                throw Error(Text.Of("XeProbe_Error_Zip", name), $"{name} is a zip (a Windows report?); XeProbe's results are in a macOS report's .tar.gz");
            if (n < 2 || head[0] != 0x1f || head[1] != 0x8b)
                throw Error(Text.Of("XeProbe_Error_NotReport", name), $"{name} is not a report bundle (xe-report-XR-XXXXXXXX.tar.gz)");
        }
        var notes = new List<Text>();
        long total = 0;
        var first = Scan(file, null, rel => IsProbeFile(rel), notes, strays: true, ref total, cancel);
        if (first.Top is null) throw Error(Text.Of("XeProbe_Error_NotReport", name), $"{name} is not a report bundle: it holds no xe-report-XR-XXXXXXXX/ folder");
        var listed = PanicPaths(first.Files.GetValueOrDefault("probe/panics.txt"), notes);
        var panicFiles = new Dictionary<string, string>(StringComparer.Ordinal);
        if (listed.Count > 0)
        {
            var want = listed.ToHashSet(StringComparer.Ordinal);
            panicFiles = Scan(file, first.Top, want.Contains, notes, strays: false, ref total, cancel).Files;
        }
        return Build(IdOf(first.Top), file, first.SawProbe, first.Files, listed, panicFiles, notes);
    }

    sealed record ScanResult(string? Top, bool SawProbe, Dictionary<string, string> Files);

    /// One pass over the archive: the report's folder (the first entry's, or top), and the files want accepts (paths
    /// relative to it) read as text. strays: note the entries outside the folder and those with "..".
    static ScanResult Scan(string file, string? top, Func<string, bool> want, List<Text> notes, bool strays, ref long total, CancellationToken cancel)
    {
        var files = new Dictionary<string, string>(StringComparer.Ordinal);
        bool sawProbe = false;
        var name = Path.GetFileName(file);
        try
        {
            using var f = File.OpenRead(file);
            using var gz = new GZipStream(f, CompressionMode.Decompress);
            using var limited = new LimitedStream(gz, UnpackedLimit);
            using var tar = new TarReader(limited);
            TarEntry? e;
            while ((e = tar.GetNextEntry()) is not null)
            {
                cancel.ThrowIfCancellationRequested();
                if (e.EntryType is TarEntryType.GlobalExtendedAttributes) continue;
                var raw = e.Name;
                var segments = Segments(raw);
                if (segments is null)
                {
                    if (strays) notes.Add(Text.Of("XeProbe_Note_Outside", Clip(raw)));
                    continue;
                }
                if (segments.Length == 0) continue;
                if (top is null)
                {
                    if (!TopRe().IsMatch(segments[0]))
                    {
                        if (strays) notes.Add(Text.Of("XeProbe_Note_Outside", Clip(raw)));
                        continue;
                    }
                    top = segments[0];
                }
                else if (segments[0] != top)
                {
                    if (strays) notes.Add(Text.Of("XeProbe_Note_Outside", Clip(raw)));
                    continue;
                }
                var rel = string.Join('/', segments.Skip(1));
                if (rel == ProbeFolder || rel.StartsWith(ProbeFolder + "/", StringComparison.Ordinal)) sawProbe = true;
                if (rel.Length == 0 || !want(rel)) continue;
                if (e.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile))
                {
                    if (e.EntryType is not TarEntryType.Directory) notes.Add(Text.Of("XeProbe_Note_Link", rel));
                    continue;
                }
                if (files.ContainsKey(rel)) continue;   // the first copy counts
                if (e.Length > FileLimit || total + e.Length > TotalLimit)
                {
                    notes.Add(Text.Of("XeProbe_Note_TooLarge", rel, FileLimit / (1024 * 1024)));
                    continue;
                }
                total += e.Length;
                files[rel] = ReadText(e.DataStream, (int)e.Length);
            }
        }
        catch (Exception ex) when (ex is InvalidDataException or IOException or FormatException or ArgumentException or OverflowException)
        {
            throw Error(Text.Of("XeProbe_Error_Unreadable", name, ex.Message), $"{name} cannot be read: {ex.Message}");
        }
        return new ScanResult(top, sawProbe, files);
    }

    /// An entry's path as segments, without "." and empty ones; null for an absolute path or one with "..".
    static string[]? Segments(string path)
    {
        var p = path.Replace('\\', '/');
        if (p.StartsWith('/') || (p.Length >= 2 && p[1] == ':') || p.Contains('\0')) return null;
        var parts = p.Split('/').Where(s => s.Length > 0 && s != ".").ToArray();
        return parts.Contains("..") ? null : parts;
    }

    static bool IsProbeFile(string rel) =>
        rel.StartsWith(ProbeFolder + "/", StringComparison.Ordinal) && ProbeFiles.Contains(rel[(ProbeFolder.Length + 1)..]);

    /// The folder of a report (xe-report-XR-…/), the one holding exactly one, or a report's probe/ folder.
    public static XeProbeReport ReadFolder(string folder, CancellationToken cancel = default)
    {
        var dir = new DirectoryInfo(folder);
        DirectoryInfo top;
        if (TopRe().IsMatch(dir.Name)) top = dir;
        else if (dir.Name == ProbeFolder && dir.Parent is { } parent && TopRe().IsMatch(parent.Name)) top = parent;
        else
        {
            var found = dir.EnumerateDirectories().Where(d => TopRe().IsMatch(d.Name)).OrderBy(d => d.Name, StringComparer.Ordinal).ToList();
            if (found.Count == 0) throw Error(Text.Of("XeProbe_Error_NotReport", dir.Name), $"{dir.Name} is not a report folder: it holds no xe-report-XR-XXXXXXXX folder");
            if (found.Count > 1) throw Error(Text.Of("XeProbe_Error_Several", dir.Name, found.Count), $"{dir.Name} holds {found.Count} reports; choose one of them");
            top = found[0];
        }
        var notes = new List<Text>();
        long total = 0;
        var files = new Dictionary<string, string>(StringComparer.Ordinal);
        var probe = new DirectoryInfo(Path.Combine(top.FullName, ProbeFolder));
        bool sawProbe = probe.Exists;
        if (sawProbe && IsLink(probe)) { notes.Add(Text.Of("XeProbe_Note_Link", ProbeFolder)); sawProbe = false; }
        if (sawProbe)
            foreach (var f in ProbeFiles)
            {
                cancel.ThrowIfCancellationRequested();
                var rel = ProbeFolder + "/" + f;
                if (ReadFile(top, rel, notes, ref total) is { } text) files[rel] = text;
            }
        var listed = PanicPaths(files.GetValueOrDefault("probe/panics.txt"), notes);
        var panicFiles = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var rel in listed)
        {
            cancel.ThrowIfCancellationRequested();
            if (ReadFile(top, rel, notes, ref total) is { } text) panicFiles[rel] = text;
        }
        return Build(IdOf(top.Name), top.FullName, sawProbe, files, listed, panicFiles, notes);
    }

    /// A regular file at rel under top, no link on the way, within the limits; null (with a note) otherwise.
    static string? ReadFile(DirectoryInfo top, string rel, List<Text> notes, ref long total)
    {
        if (Segments(rel) is not { Length: > 0 } segments) return null;
        var root = Path.GetFullPath(top.FullName);
        var path = Path.GetFullPath(Path.Combine([root, .. segments]));
        if (!path.StartsWith(root.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.Ordinal)) return null;
        // every folder between the report's and the file, then the file: none may be a link
        var at = root;
        for (int i = 0; i < segments.Length - 1; i++)
        {
            at = Path.Combine(at, segments[i]);
            var d = new DirectoryInfo(at);
            if (!d.Exists) return null;
            if (IsLink(d)) { notes.Add(Text.Of("XeProbe_Note_Link", rel)); return null; }
        }
        var fi = new FileInfo(path);
        if (!fi.Exists) return null;
        if (IsLink(fi)) { notes.Add(Text.Of("XeProbe_Note_Link", rel)); return null; }
        if (fi.Length > FileLimit || total + fi.Length > TotalLimit)
        {
            notes.Add(Text.Of("XeProbe_Note_TooLarge", rel, FileLimit / (1024 * 1024)));
            return null;
        }
        total += fi.Length;
        using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        return ReadText(s, (int)Math.Min(fi.Length, FileLimit));
    }

    static bool IsLink(FileSystemInfo f) => f.LinkTarget is not null || f.Attributes.HasFlag(FileAttributes.ReparsePoint);

    /// At most length bytes (never more than the file limit) as UTF-8, with "\n" line ends.
    static string ReadText(Stream? s, int length)
    {
        if (s is null || length <= 0) return "";
        var buf = new byte[Math.Min(length, FileLimit)];
        int n = s.ReadAtLeast(buf, buf.Length, throwOnEndOfStream: false);
        return Encoding.UTF8.GetString(buf, 0, n).Replace("\r\n", "\n");
    }

    /// probe/panics.txt's paths, relative to the report's folder; a path that is absolute or has ".." is noted and passed over.
    static List<string> PanicPaths(string? text, List<Text> notes)
    {
        var list = new List<string>();
        if (text is null) return list;
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.Trim();
            if (line.Length == 0) continue;
            if (Segments(line) is not { Length: > 0 } segments) { notes.Add(Text.Of("XeProbe_Note_BadPanicPath", Clip(line))); continue; }
            var rel = string.Join('/', segments);
            if (!list.Contains(rel, StringComparer.Ordinal)) list.Add(rel);
            if (list.Count >= PanicLimit) break;
        }
        return list;
    }

    static XeProbeReport Build(string id, string source, bool sawProbe, Dictionary<string, string> files, List<string> listed,
        Dictionary<string, string> panicFiles, List<Text> notes)
    {
        string? F(string name) => files.GetValueOrDefault(ProbeFolder + "/" + name);
        var crumbText = F("nvram-breadcrumb.txt");
        var crumb = crumbText is null ? null : ParseBreadcrumb(crumbText);
        return new XeProbeReport
        {
            Id = id, Source = source, HasProbe = sawProbe,
            Results = F("results.txt") is { } r ? ParseResults(r) : null,
            Info = F("info.txt") is { } i ? ParseInfo(i) : null,
            Breadcrumb = crumb,
            BreadcrumbNote = crumb is null && crumbText is not null ? FirstLine(crumbText) : null,
            Panics = [.. listed.Select(p => panicFiles.TryGetValue(p, out var t) ? new XeProbePanic(p, true, WatchdogLine(t)) : new XeProbePanic(p, false, null))],
            History = F("history.txt") is { } h ? ParseHistory(h) : [],
            Log = F("log.txt")?.TrimEnd('\0', '\n') ?? "",
            IoregAttached = F("ioreg.txt") is { } io ? io.Contains("<class XeProbe", StringComparison.Ordinal) : null,
            Notes = notes,
        };
    }

    static string IdOf(string top) => TopRe().Match(top).Groups[1].Value.ToUpperInvariant();

    static string Clip(string s) => s.Length > 120 ? s[..120] + "…" : s;

    static string FirstLine(string text) => text.Split('\n').Select(l => l.Trim().TrimEnd('\0')).FirstOrDefault(l => l.Length > 0) ?? "";

    static XeProbeReportException Error(Text reason, string message) => new(reason, message);

    // ---- the files ----

    /// results.txt (and each run of history.txt).
    public static XeProbeResults ParseResults(string text)
    {
        bool attached = false;
        int? version = null;
        string? device = null, family = null;
        var stages = new List<XeProbeStage>();
        var other = new List<string>();
        foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.TrimEnd(' ', '\t', '\r', '\0');
            if (line.Trim().Length == 0) continue;
            if (line.Trim() == "XeProbe not attached") { attached = false; continue; }
            if (HeaderRe().Match(line) is { Success: true } h)
            {
                attached = true;
                version = int.Parse(h.Groups[1].Value, CultureInfo.InvariantCulture);
                device = h.Groups[2].Value.ToLowerInvariant();
                var fam = h.Groups[3].Value.Trim();
                family = fam.Length == 0 || fam == "family unknown" ? null : fam;
                continue;
            }
            if (StageRe().Match(line) is { Success: true } m && uint.TryParse(m.Groups[4].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var ms))
            {
                stages.Add(new XeProbeStage(int.Parse(m.Groups[1].Value, CultureInfo.InvariantCulture), m.Groups[2].Value, StatusOf(m.Groups[3].Value), ms,
                    m.Groups[5].Success ? m.Groups[5].Value.Trim() : ""));
                continue;
            }
            other.Add(line);
        }
        return new XeProbeResults { Attached = attached, Version = version, Device = device, Family = family, Stages = stages, Other = other };
    }

    static XeProbeStatus StatusOf(string s) => s switch
    {
        "ok" => XeProbeStatus.Ok, "FAILED" => XeProbeStatus.Failed, "refused" => XeProbeStatus.Refused,
        "STALLED" => XeProbeStatus.Stalled, "running" => XeProbeStatus.Running, _ => XeProbeStatus.NotRun,
    };

    /// info.txt.
    public static XeProbeInfo ParseInfo(string text)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.Trim().TrimEnd('\0');
            int eq = line.IndexOf('=');
            if (line.StartsWith('#') || eq <= 0) continue;
            d.TryAdd(line[..eq].Trim(), line[(eq + 1)..].Trim());
        }
        static int? Int(string? s) => int.TryParse(s, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var n) ? n : null;
        static bool? Bool(string? s) => s switch { "1" => true, "0" => false, _ => null };
        static string? NonEmpty(string? s) => string.IsNullOrEmpty(s) ? null : s;
        var family = NonEmpty(d.GetValueOrDefault("family"));
        var builtIn = NonEmpty(d.GetValueOrDefault("guc_built_in"));
        return new XeProbeInfo
        {
            Version = Int(d.GetValueOrDefault("version")),
            Device = NonEmpty(d.GetValueOrDefault("device"))?.ToLowerInvariant(),
            Family = family == "unknown" ? null : family,
            GucImage = NonEmpty(d.GetValueOrDefault("guc_image")),
            GucBuiltIn = builtIn == "no" ? null : builtIn,
            Images = [.. (d.GetValueOrDefault("images") ?? "").Split(',').Select(s => s.Trim()).Where(s => s.Length > 0)],
            Nvram = Bool(d.GetValueOrDefault("nvram")),
            PanicOnStall = Bool(d.GetValueOrDefault("panic_on_stall")),
            LastStage = Int(d.GetValueOrDefault("last_stage")),
        };
    }

    /// nvram-breadcrumb.txt: null for "no breadcrumb in NVRAM ..." or an empty file. nvram prints a trailing NUL as %00.
    public static XeProbeBreadcrumb? ParseBreadcrumb(string text)
    {
        var line = FirstLine(text);
        while (line.EndsWith("%00", StringComparison.Ordinal)) line = line[..^3];
        line = line.Trim();
        if (line.Length == 0 || line.StartsWith("no breadcrumb in NVRAM", StringComparison.Ordinal)) return null;
        if (CrumbRe().Match(line) is not { Success: true } m) return new XeProbeBreadcrumb(line, null, null, null, null, null);
        return new XeProbeBreadcrumb(line, m.Groups[1].Value.ToLowerInvariant(), int.Parse(m.Groups[2].Value, CultureInfo.InvariantCulture), m.Groups[3].Value,
            m.Groups[4].Value, uint.TryParse(m.Groups[5].Value, NumberStyles.None, CultureInfo.InvariantCulture, out var seq) ? seq : null);
    }

    /// The watchdog's panic line in a panic report: from "XeProbe watchdog:" to "for the stage)", or to the end of its
    /// line (a .panic file's panicString is JSON, with \n and \" escaped).
    public static string? WatchdogLine(string text)
    {
        const string start = "XeProbe watchdog:", end = "for the stage)";
        int i = text.IndexOf(start, StringComparison.Ordinal);
        if (i < 0) return null;
        int e = text.IndexOf(end, i, StringComparison.Ordinal);
        int stop = e >= 0 ? e + end.Length : text.Length;
        foreach (var brk in new[] { "\n", "\r", "\\n" })
        {
            int b = text.IndexOf(brk, i, StringComparison.Ordinal);
            if (b >= 0 && b < stop) stop = b;
        }
        var line = text[i..stop].Replace("\\\"", "\"").Trim();
        return line.Length > 400 ? line[..400] + "…" : line;
    }

    /// history.txt: the runs, oldest first, as the Manager appended them.
    public static IReadOnlyList<XeProbeRun> ParseHistory(string text)
    {
        var runs = new List<XeProbeRun>();
        Match? head = null;
        var body = new StringBuilder();
        void Flush()
        {
            if (head is null) return;
            var g = head.Groups;
            runs.Add(new XeProbeRun(g[1].Value, Time(g[1].Value), g[2].Value, Time(g[2].Value), g[3].Value,
                int.TryParse(g[4].Value, NumberStyles.AllowLeadingSign, CultureInfo.InvariantCulture, out var exit) ? exit : -1, ParseResults(body.ToString())));
            body.Clear();
        }
        foreach (var raw in text.Replace("\r\n", "\n").Split('\n'))
        {
            var line = raw.TrimEnd(' ', '\t', '\r', '\0');
            if (RunRe().Match(line) is { Success: true } m) { Flush(); head = m; if (runs.Count >= 1000) break; continue; }
            if (head is null || line.StartsWith('#')) continue;
            body.Append(line).Append('\n');
        }
        Flush();
        return runs;
    }

    static DateTimeOffset? Time(string s) =>
        DateTimeOffset.TryParse(s, CultureInfo.InvariantCulture, DateTimeStyles.AssumeUniversal | DateTimeStyles.AdjustToUniversal, out var t) ? t : null;

    // ---- verdicts ----

    public static XeProbeVerdict Verdict(XeProbeStatus s) => s switch
    {
        XeProbeStatus.Ok => XeProbeVerdict.Passed, XeProbeStatus.Failed => XeProbeVerdict.Failed, XeProbeStatus.Refused => XeProbeVerdict.Refused,
        XeProbeStatus.Stalled => XeProbeVerdict.Stalled, XeProbeStatus.Running => XeProbeVerdict.Running, _ => XeProbeVerdict.NotRun,
    };

    public static Text VerdictText(XeProbeVerdict v) => Text.Of("XeProbe_Verdict_" + v);

    public static CheckState VerdictState(XeProbeVerdict v) => v switch
    {
        XeProbeVerdict.Passed => CheckState.Good, XeProbeVerdict.Failed or XeProbeVerdict.Stalled => CheckState.Bad,
        XeProbeVerdict.Refused or XeProbeVerdict.Running => CheckState.Warning, _ => CheckState.Neutral,
    };

    /// The report in one line, as parts the page joins with "; ": "stages 1-2 passed", "stage 3 (guc) failed: …",
    /// "stages 4-5 refused", and the watchdog panics.
    public static IReadOnlyList<Text> Summary(XeProbeReport r)
    {
        if (!r.HasProbe) return [Text.Of("XeProbe_Sum_NoProbe")];
        var parts = new List<Text>();
        if (r.Results is null) parts.Add(Text.Of("XeProbe_Sum_NoResults"));
        else if (!r.Results.Attached) parts.Add(Text.Of("XeProbe_Sum_NotAttached"));
        else parts.AddRange(Summary(r.Results));
        int panics = r.Panics.Count(p => p.Line is not null);
        if (panics > 0) parts.Add(Text.Of("XeProbe_Sum_Panics", panics));
        return parts;
    }

    /// One results.txt in one line (also each run of the history).
    public static IReadOnlyList<Text> Summary(XeProbeResults results)
    {
        if (!results.Attached) return [Text.Of("XeProbe_Sum_NotAttached")];
        var parts = new List<Text>();
        var passed = results.Stages.Where(s => s.Verdict == XeProbeVerdict.Passed).Select(s => s.Number).ToList();
        if (passed.Count > 0) parts.Add(Text.Of(passed.Count == 1 ? "XeProbe_Sum_PassedOne" : "XeProbe_Sum_Passed", Ranges(passed)));
        foreach (var s in results.Stages)
        {
            bool detail = s.Detail.Length > 0;
            if (s.Verdict == XeProbeVerdict.Failed) parts.Add(detail ? Text.Of("XeProbe_Sum_Failed", s.Number, s.Name, s.Detail) : Text.Of("XeProbe_Sum_FailedBare", s.Number, s.Name));
            else if (s.Verdict == XeProbeVerdict.Stalled) parts.Add(detail ? Text.Of("XeProbe_Sum_Stalled", s.Number, s.Name, s.Detail) : Text.Of("XeProbe_Sum_StalledBare", s.Number, s.Name));
            else if (s.Verdict == XeProbeVerdict.Running) parts.Add(Text.Of("XeProbe_Sum_Running", s.Number, s.Name));
        }
        var refused = results.Stages.Where(s => s.Verdict == XeProbeVerdict.Refused).Select(s => s.Number).ToList();
        if (refused.Count > 0) parts.Add(Text.Of(refused.Count == 1 ? "XeProbe_Sum_RefusedOne" : "XeProbe_Sum_Refused", Ranges(refused)));
        if (parts.Count == 0) parts.Add(Text.Of("XeProbe_Sum_NoneRun"));
        return parts;
    }

    /// 1,2,3,5,9 → "1-3, 5, 9".
    public static string Ranges(IEnumerable<int> numbers)
    {
        var n = numbers.Distinct().Order().ToList();
        var parts = new List<string>();
        for (int i = 0; i < n.Count;)
        {
            int j = i;
            while (j + 1 < n.Count && n[j + 1] == n[j] + 1) j++;
            parts.Add(j > i ? $"{n[i]}-{n[j]}" : n[i].ToString(CultureInfo.InvariantCulture));
            i = j + 1;
        }
        return string.Join(", ", parts);
    }
}

/// A read-only stream that fails once more than limit bytes have been read through it (a gzip bomb).
sealed class LimitedStream(Stream inner, long limit) : Stream
{
    long read;
    public override bool CanRead => true;
    public override bool CanSeek => false;
    public override bool CanWrite => false;
    public override long Length => throw new NotSupportedException();
    public override long Position { get => read; set => throw new NotSupportedException(); }
    public override void Flush() { }
    public override int Read(byte[] buffer, int offset, int count) => Count(inner.Read(buffer, offset, count));
    public override int Read(Span<byte> buffer) => Count(inner.Read(buffer));
    int Count(int n)
    {
        read += n;
        if (read > limit) throw new InvalidDataException($"it unpacks to more than {limit >> 20} MB");
        return n;
    }
    public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
    public override void SetLength(long value) => throw new NotSupportedException();
    public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    protected override void Dispose(bool disposing) { if (disposing) inner.Dispose(); base.Dispose(disposing); }
}
