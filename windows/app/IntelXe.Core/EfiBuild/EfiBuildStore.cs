// SPDX-License-Identifier: MIT
// Where scans and builds go (under %LOCALAPPDATA%\IntelXe Manager\efibuild in the app, never beside the exe), saving a
// built EFI to a folder the user chooses, the built EFI as the stick page's IEfiSource, and the check that the app's own
// EFI engine (IntelXe-EFI.ps1's, in C#) finds stage 2 complete in what the Python builder made.
using IntelXe.Core.Efi;
using IntelXe.Core.Stick;

namespace IntelXe.Core.EfiBuild;

public sealed class EfiBuildStore(string root)
{
    /// The file the builder leaves in an output folder it made (windows/efibuild engine.py MARKER): only such a folder is
    /// replaced or deleted.
    public const string Marker = ".intelxe-efibuild";

    public string Root { get; } = root;
    public string ScansDir => Path.Combine(Root, "scans");
    public string BuildsDir => Path.Combine(Root, "builds");

    static string Stamp(DateTime now) => now.ToString("yyyyMMdd-HHmmss", System.Globalization.CultureInfo.InvariantCulture);

    /// A new, not yet existing folder for a scan (the elevated helper refuses one that exists).
    public string NewScanDir(DateTime now) => Unique(ScansDir, Stamp(now));

    /// A new, not yet existing output folder for a build.
    public string NewBuildDir(DateTime now) => Unique(BuildsDir, Stamp(now));

    static string Unique(string parent, string name)
    {
        var path = Path.Combine(parent, name);
        for (int i = 2; Directory.Exists(path) || File.Exists(path); i++) path = Path.Combine(parent, $"{name}-{i}");
        return path;
    }

    /// Deletes the other builds the builder made (they hold generated serial numbers), keeping the ones named; returns
    /// how many went. A folder without the marker is never touched.
    public int PruneBuilds(params string?[] keep)
    {
        if (!Directory.Exists(BuildsDir)) return 0;
        var keepFull = keep.Where(k => !string.IsNullOrEmpty(k)).Select(k => Path.GetFullPath(k!).TrimEnd(Path.DirectorySeparatorChar)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        int n = 0;
        foreach (var dir in Directory.EnumerateDirectories(BuildsDir))
        {
            if (keepFull.Contains(Path.GetFullPath(dir).TrimEnd(Path.DirectorySeparatorChar))) continue;
            if (!File.Exists(Path.Combine(dir, Marker))) continue;
            if ((new DirectoryInfo(dir).Attributes & FileAttributes.ReparsePoint) != 0) continue;
            try { Directory.Delete(dir, true); n++; } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return n;
    }

    /// The EFI folder of a build's output folder (OUT\EFI holding BOOT and OC), checked; throws EfiSourceException.
    public static string EfiOf(string outDir) => EfiFolderCheck.Resolve(Path.Combine(outDir, "EFI"));

    /// Copies a built EFI folder to destParent\EFI and reads every file back. Refuses when destParent\EFI exists (an
    /// EFI is never overwritten here) or lies inside the build. Returns the new EFI folder.
    public static string SaveTo(string efiDir, string destParent)
    {
        var src = EfiFolderCheck.Resolve(efiDir);
        if (string.IsNullOrWhiteSpace(destParent) || !Directory.Exists(destParent)) throw new EfiBuildException($"no folder {destParent}");
        var dest = Path.Combine(Path.GetFullPath(destParent), "EFI");
        if (Directory.Exists(dest) || File.Exists(dest)) throw new EfiBuildException($"{dest} exists already: choose another folder (an EFI folder is not overwritten)");
        var srcFull = Path.GetFullPath(src).TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar;
        if ((dest + Path.DirectorySeparatorChar).StartsWith(srcFull, StringComparison.OrdinalIgnoreCase)) throw new EfiBuildException("the EFI cannot be saved inside itself");
        var tmp = Path.Combine(Path.GetFullPath(destParent), ".EFI-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            Copy(src, tmp);
            foreach (var f in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
            {
                var rel = Path.GetRelativePath(src, f);
                var copy = Path.Combine(tmp, rel);
                if (!File.Exists(copy) || EngineLayout.Sha256(copy) != EngineLayout.Sha256(f)) throw new EfiBuildException($"{rel} did not copy correctly");
            }
            Directory.Move(tmp, dest);
        }
        finally
        {
            if (Directory.Exists(tmp)) try { Directory.Delete(tmp, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return dest;
    }

    static void Copy(string src, string dst)
    {
        Directory.CreateDirectory(dst);
        foreach (var f in Directory.EnumerateFiles(src)) File.Copy(f, Path.Combine(dst, Path.GetFileName(f)));
        foreach (var d in Directory.EnumerateDirectories(src)) Copy(d, Path.Combine(dst, Path.GetFileName(d)));
    }

    /// The app's own engine on the built EFI: stage 2 (and the Safari setting when the build made it) must have nothing
    /// left to change. A difference means the Python and C# readers of efi-rules.json disagree. Returns the plan's lines
    /// when it would change something (empty when it agrees).
    public static IReadOnlyList<string> Stage2Left(string efiDir, bool safari)
    {
        var change = safari ? EfiChange.SettingsSafari : EfiChange.Settings;
        var plan = EfiWork.Plan(new EfiTarget.Folder(efiDir), efiDir, change, new EfiOptions());
        return plan.HasChanges ? plan.Lines : [];
    }
}

/// The EFI the app built (Build the EFI page), for the stick page: the build's output folder's EFI\, checked to hold
/// OpenCore when the stick is written.
public sealed class BuiltEfiSource(string outDir, string describe) : IEfiSource
{
    public string Describe => describe;

    public Task<string> PrepareAsync(CancellationToken cancel = default) => Task.FromResult(EfiBuildStore.EfiOf(outDir));
}
