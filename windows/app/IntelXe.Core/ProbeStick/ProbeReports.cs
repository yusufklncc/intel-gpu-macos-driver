// SPDX-License-Identifier: MIT
// The reports on a probe stick, as the Report page shows them: the stick's FAT32 volume (label XEPROBE, with the marker
// file) holds REPORTS\xe-stick-XR-XXXXXXXX.tar.gz with its .txt summary and a .sha256 of both, or, when the stick's
// privacy check failed, only xe-stick-XR-XXXXXXXX-privacy-check-failed.txt (tools/probe-stick/rootfs/usr/lib/xe-stick/
// scan.sh). Each report is checked without unpacking it to disk: the .sha256 against the .tar.gz and the .txt, and the
// SEAL.sha256 inside against every file of the archive (System.Formats.Tar over a GZipStream), every file listed and
// nothing besides.
using System.Formats.Tar;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using IntelXe.Core.Report;

namespace IntelXe.Core.ProbeStick;

public enum ProbeReportState { Sealed, Broken, PrivacyCheckFailed }

public sealed class ProbeReport
{
    public required string Id { get; init; }
    public required string Name { get; init; }
    /// The .tar.gz; null for a privacy-check-failed note.
    public string? Archive { get; init; }
    /// The .txt summary, or the privacy-check-failed note.
    public string? SummaryFile { get; init; }
    public string SummaryText { get; init; } = "";
    public long Size { get; init; }
    public DateTime Written { get; init; }
    public ProbeReportState State { get; init; }
    /// The files SEAL.sha256 lists, each found and matching.
    public int SealedFiles { get; init; }
    /// What is wrong, in English; empty for a sealed report.
    public IReadOnlyList<string> Problems { get; init; } = [];
    /// GitHub takes attachments up to 25 MB; the stick keeps its reports below that.
    public bool TooLargeForGitHub => Size > 25L * 1000 * 1000;
}

public static partial class ProbeReports
{
    public const string ReportsFolder = "REPORTS";
    /// Read at most this much of a summary into the page.
    const int SummaryLimit = 256 * 1024;
    /// A report's archive is under 25 MB; its contents, unpacked in memory one file at a time, stay below this.
    const long UnpackedLimit = 4L << 30;

    [GeneratedRegex(@"^xe-stick-(XR-[0-9A-Fa-f]{8})\.tar\.gz$", RegexOptions.CultureInvariant)]
    private static partial Regex ArchiveRe();

    [GeneratedRegex(@"^xe-stick-(XR-[0-9A-Fa-f]{8})-privacy-check-failed\.txt$", RegexOptions.CultureInvariant)]
    private static partial Regex PrivacyRe();

    /// Whether a volume is a probe stick: labelled XEPROBE and holding the marker file at its root.
    public static bool IsProbeStick(string root, string? label) =>
        string.Equals(label?.Trim(), ProbeImage.Label, StringComparison.OrdinalIgnoreCase) && File.Exists(Path.Combine(root, ProbeImage.Marker));

    /// The probe sticks among the volumes (root, label).
    public static IReadOnlyList<string> FindSticks(IEnumerable<(string Root, string? Label)> volumes) =>
        [.. volumes.Where(v => { try { return IsProbeStick(v.Root, v.Label); } catch (IOException) { return false; } catch (UnauthorizedAccessException) { return false; } })
                   .Select(v => v.Root)];

    /// The stick's reports, newest first, each checked.
    public static IReadOnlyList<ProbeReport> Read(string stickRoot, CancellationToken cancel = default)
    {
        var dir = Path.Combine(stickRoot, ReportsFolder);
        if (!Directory.Exists(dir)) return [];
        var list = new List<ProbeReport>();
        foreach (var f in Directory.EnumerateFiles(dir))
        {
            cancel.ThrowIfCancellationRequested();
            var name = Path.GetFileName(f);
            if (ArchiveRe().Match(name) is { Success: true } m) list.Add(Check(f, m.Groups[1].Value.ToUpperInvariant(), cancel));
            else if (PrivacyRe().Match(name) is { Success: true } pm)
                list.Add(new ProbeReport
                {
                    Id = pm.Groups[1].Value.ToUpperInvariant(), Name = name[..^4], SummaryFile = f, SummaryText = ReadText(f),
                    Size = new FileInfo(f).Length, Written = File.GetLastWriteTimeUtc(f), State = ProbeReportState.PrivacyCheckFailed,
                });
        }
        return [.. list.OrderByDescending(r => r.Written).ThenBy(r => r.Id, StringComparer.Ordinal)];
    }

    /// One report: its .sha256 and the SEAL.sha256 inside.
    public static ProbeReport Check(string archive, string id, CancellationToken cancel = default)
    {
        var problems = new List<string>();
        var dir = Path.GetDirectoryName(archive)!;
        var name = Path.GetFileName(archive)[..^".tar.gz".Length];
        var summary = Path.Combine(dir, name + ".txt");
        var hashFile = Path.Combine(dir, name + ".sha256");
        if (!ReportRun.IsReportId(id)) problems.Add($"{id} is not a report ID");

        // the .sha256 the stick wrote beside them: sha256sum of the .tar.gz and the .txt
        if (!File.Exists(hashFile)) problems.Add($"{name}.sha256 is missing");
        else
        {
            var sums = ParseSums(ReadText(hashFile), problems, name + ".sha256");
            foreach (var (file, required) in new[] { (name + ".tar.gz", true), (name + ".txt", false) })
            {
                var path = Path.Combine(dir, file);
                if (!sums.TryGetValue(file, out var want)) { if (required) problems.Add($"{name}.sha256 does not list {file}"); continue; }
                if (!File.Exists(path)) { problems.Add($"{file} is missing"); continue; }
                using var s = File.OpenRead(path);
                if (ProbeImage.Hash(s, s.Length, null, cancel) != want) problems.Add($"{file} does not match {name}.sha256");
            }
        }
        if (!File.Exists(summary)) problems.Add($"{name}.txt (the summary) is missing");

        int sealedFiles = 0;
        try { sealedFiles = CheckSeal(archive, name, problems, cancel); }
        catch (Exception e) when (e is InvalidDataException or IOException or FormatException or ArgumentException)
        {
            problems.Add($"the archive cannot be read: {e.Message}");
        }

        return new ProbeReport
        {
            Id = id, Name = name, Archive = archive, SummaryFile = File.Exists(summary) ? summary : null,
            SummaryText = File.Exists(summary) ? ReadText(summary) : "", Size = new FileInfo(archive).Length,
            Written = File.GetLastWriteTimeUtc(archive), SealedFiles = sealedFiles, Problems = problems,
            State = problems.Count == 0 ? ProbeReportState.Sealed : ProbeReportState.Broken,
        };
    }

    /// Every file in the archive (under the one folder NAME/) hashed as it streams by; then SEAL.sha256's lines checked
    /// against them: each listed file present and matching, no file that is not listed. Returns the files that matched.
    static int CheckSeal(string archive, string name, List<string> problems, CancellationToken cancel)
    {
        var hashes = new Dictionary<string, string>(StringComparer.Ordinal);
        string? seal = null;
        long unpacked = 0;
        using (var file = File.OpenRead(archive))
        using (var gz = new GZipStream(file, CompressionMode.Decompress))
        using (var tar = new TarReader(gz))
        {
            var buf = new byte[1 << 20];
            TarEntry? e;
            while ((e = tar.GetNextEntry()) is not null)
            {
                cancel.ThrowIfCancellationRequested();
                var path = e.Name.Replace('\\', '/');
                if (path.StartsWith("./", StringComparison.Ordinal)) path = path[2..];
                if (path.TrimEnd('/') == name && e.EntryType is TarEntryType.Directory) continue;
                if (!path.StartsWith(name + "/", StringComparison.Ordinal) || path.Split('/').Contains(".."))
                {
                    problems.Add($"the archive holds {path}, outside {name}/");
                    continue;
                }
                var rel = path[(name.Length + 1)..];
                if (e.EntryType is TarEntryType.Directory) continue;
                if (e.EntryType is not (TarEntryType.RegularFile or TarEntryType.V7RegularFile))
                {
                    problems.Add($"the archive holds {rel} as a {e.EntryType}, not a file");
                    continue;
                }
                unpacked += e.Length;
                if (unpacked > UnpackedLimit) throw new InvalidDataException("it unpacks to more than 4 GB");
                using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                var data = e.DataStream;
                MemoryStream? keep = rel == "SEAL.sha256" ? new MemoryStream() : null;
                if (data is not null)
                {
                    int n;
                    while ((n = data.Read(buf, 0, buf.Length)) > 0) { h.AppendData(buf, 0, n); keep?.Write(buf, 0, n); }
                }
                if (keep is not null) seal = Encoding.UTF8.GetString(keep.ToArray());
                else if (!hashes.TryAdd(rel, Convert.ToHexStringLower(h.GetHashAndReset()))) problems.Add($"the archive holds {rel} twice");
            }
        }
        if (seal is null) { problems.Add("the archive holds no SEAL.sha256"); return 0; }
        var listed = ParseSums(seal, problems, "SEAL.sha256");
        int ok = 0;
        foreach (var (rel, want) in listed)
        {
            if (!hashes.TryGetValue(rel, out var got)) problems.Add($"SEAL.sha256 lists {rel}, which is not in the archive");
            else if (got != want) problems.Add($"{rel} does not match SEAL.sha256");
            else ok++;
        }
        foreach (var rel in hashes.Keys.Where(k => !listed.ContainsKey(k)).Order(StringComparer.Ordinal))
            problems.Add($"{rel} is in the archive but not in SEAL.sha256");
        return ok;
    }

    /// sha256sum's lines ("HASH  NAME", or "HASH *NAME"): name to lower-case hash.
    static Dictionary<string, string> ParseSums(string text, List<string> problems, string what)
    {
        var d = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            int sp = line.IndexOf(' ');
            var hash = sp < 0 ? line : line[..sp];
            var file = sp < 0 ? "" : line[(sp + 1)..].TrimStart(' ', '*');
            if (hash.Length != 64 || !hash.All(Uri.IsHexDigit) || file.Length == 0) { problems.Add($"{what} has a line that is not a SHA-256 and a name"); continue; }
            if (!d.TryAdd(file, hash.ToLowerInvariant())) problems.Add($"{what} lists {file} twice");
        }
        return d;
    }

    static string ReadText(string path)
    {
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite);
        var buf = new byte[(int)Math.Min(f.Length, SummaryLimit)];
        f.ReadExactly(buf);
        return Encoding.UTF8.GetString(buf).Replace("\r\n", "\n");
    }
}
