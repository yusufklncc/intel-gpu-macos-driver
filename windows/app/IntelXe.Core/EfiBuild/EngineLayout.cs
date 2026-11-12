// SPDX-License-Identifier: MIT
// The builder's folder beside the app (windows/app/build.ps1 puts it there):
//
//   engine\python\            python.org's embeddable Python (signed python.exe), wmi, pywin32, certifi
//   engine\efibuild\          windows/efibuild: efibuild.py, intelxe_efibuild\, downloads.lock.json, upstream\
//   engine\efibuild\upstream\OpCore-Simplify-<commit>\, Hardware-Sniffer-<commit>\   the pinned, checked trees
//   engine\engine.lock.json   what build.ps1 fetched (Python and the wheels, with their SHA-256)
//   engine\MANIFEST.sha256    "sha256  relative/path" of every other file under engine\
//
// The manifest is checked before every scan and build: a changed, missing or extra file refuses the engine (an extra
// json.py next to efibuild.py would be imported by it), so the elevated scan never runs code that build.ps1 did not put
// there.
using System.Security.Cryptography;
using System.Text.Json;

namespace IntelXe.Core.EfiBuild;

public sealed record EngineLayout(string Root, string? CacheDir = null)
{
    public const string ManifestName = "MANIFEST.sha256";

    /// The engine beside the app's exe.
    public static EngineLayout Beside(string appDir, string? cacheDir = null) => new(Path.Combine(appDir, "engine"), cacheDir);

    public string Python => Path.Combine(Root, "python", OperatingSystem.IsWindows() ? "python.exe" : "python");
    public string EfiBuildDir => Path.Combine(Root, "efibuild");
    public string Script => Path.Combine(EfiBuildDir, "efibuild.py");
    public string Manifest => Path.Combine(Root, ManifestName);

    /// "" when python, efibuild.py and the manifest are there, else what is missing.
    public string Missing()
    {
        if (!Directory.Exists(Root)) return $"no builder folder {Root}";
        foreach (var f in new[] { Python, Script, Manifest })
            if (!File.Exists(f)) return $"the builder folder has no {Path.GetRelativePath(Root, f)}";
        return "";
    }

    /// The problems of the engine against its manifest: changed, missing and extra files. Empty when it matches.
    public IReadOnlyList<string> Verify()
    {
        var problems = new List<string>();
        if (!File.Exists(Manifest)) return [$"no {ManifestName}"];
        var listed = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var raw in File.ReadAllLines(Manifest))
        {
            var line = raw.TrimEnd('\r');
            if (line.Length == 0) continue;
            if (line.Length < 67 || line[64] != ' ' || line[65] != ' ' || !line[..64].All(Uri.IsHexDigit))
            {
                problems.Add($"{ManifestName}: not a \"sha256  path\" line: {line}");
                continue;
            }
            var rel = line[66..];
            if (rel.Contains("..") || Path.IsPathRooted(rel)) { problems.Add($"{ManifestName}: unsafe path {rel}"); continue; }
            listed[rel] = line[..64].ToLowerInvariant();
        }
        if (listed.Count == 0) problems.Add($"{ManifestName} lists no file");
        var seen = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var path in Directory.EnumerateFiles(Root, "*", SearchOption.AllDirectories))
        {
            var rel = Path.GetRelativePath(Root, path).Replace('\\', '/');
            if (string.Equals(rel, ManifestName, StringComparison.OrdinalIgnoreCase)) continue;
            if (!listed.TryGetValue(rel, out var want)) { problems.Add($"extra file {rel}"); continue; }
            seen.Add(rel);
            if (Sha256(path) != want) problems.Add($"changed file {rel}");
        }
        foreach (var rel in listed.Keys.Where(k => !seen.Contains(k)).Order(StringComparer.Ordinal)) problems.Add($"missing file {rel}");
        return problems;
    }

    /// The manifest text of a folder, as build.ps1 writes it (sorted by path, forward slashes, LF).
    public static string ManifestText(string root) =>
        string.Concat(Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)
            .Select(p => Path.GetRelativePath(root, p).Replace('\\', '/'))
            .Where(rel => !string.Equals(rel, ManifestName, StringComparison.OrdinalIgnoreCase))
            .Order(StringComparer.Ordinal)
            .Select(rel => $"{Sha256(Path.Combine(root, rel))}  {rel}\n"));

    public static string Sha256(string path)
    {
        using var f = File.OpenRead(path);
        return Convert.ToHexStringLower(SHA256.HashData(f));
    }

    /// "OpCore Simplify 9aed67e, Hardware Sniffer ff3b9ed, OpenCore 1.0.8, Python 3.12.10" from the engine's own files;
    /// what cannot be read is left out.
    public string Versions()
    {
        var parts = new List<string>();
        try
        {
            using var pinned = JsonDocument.Parse(File.ReadAllText(Path.Combine(EfiBuildDir, "upstream", "PINNED.json")));
            foreach (var p in pinned.RootElement.GetProperty("projects").EnumerateObject())
                if (p.Value.TryGetProperty("commit", out var c) && c.GetString() is { Length: >= 7 } commit)
                    parts.Add($"{p.Name.Replace('-', ' ')} {commit[..7]}");
        }
        catch (Exception e) when (e is IOException or JsonException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException) { }
        try
        {
            using var lockDoc = JsonDocument.Parse(File.ReadAllText(Path.Combine(EfiBuildDir, "downloads.lock.json")));
            if (lockDoc.RootElement.GetProperty("products").GetProperty("OpenCorePkg").GetProperty("version").GetString() is { Length: > 0 } oc)
                parts.Add("OpenCore " + oc);
        }
        catch (Exception e) when (e is IOException or JsonException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException) { }
        try
        {
            using var engine = JsonDocument.Parse(File.ReadAllText(Path.Combine(Root, "engine.lock.json")));
            if (engine.RootElement.GetProperty("python").GetProperty("version").GetString() is { Length: > 0 } py)
                parts.Add("Python " + py);
        }
        catch (Exception e) when (e is IOException or JsonException or KeyNotFoundException or InvalidOperationException or UnauthorizedAccessException) { }
        return string.Join(", ", parts);
    }
}
