// SPDX-License-Identifier: MIT
using System.IO.Compression;
using System.Security.Cryptography;
using IntelXeEfi;

namespace IntelXe.Core.Efi;

/// What the app knows about a package: its folder (the unpacked intel-gpu-macos-driver-VERSION-efi.zip), the version of
/// its IntelXe.kext and HybridPM.kext, and its files against its SHA256SUMS as the engine checks them before a change
/// ("ok", "no SHA256SUMS", or what differs; every file of the two kexts and the table must be listed and match).
public sealed record EfiPackageInfo(string Dir, string? Problem, string Checksums, string KextVersion, string HybridPMVersion)
{
    public bool Usable => Problem is null && (Checksums == "ok" || Checksums == "no SHA256SUMS");
}

/// Finding the package: a folder the user picks (the unpacked -efi.zip, or a folder holding it), the zip itself
/// (unpacked into the app's cache), or one beside the app.
public static class EfiPackage
{
    /// Does DIR hold EFI/OC/Kexts/IntelXe.kext's executable (by the rules' names)?
    public static bool IsPackage(string dir, Rules? rules = null)
    {
        var r = rules ?? EfiRules.Embedded;
        try
        {
            return File.Exists(Path.Combine(dir, "EFI", "OC", Util.Native(r.Str("kext.folder")), r.Str("kext.bundle"), Util.Native(r.Str("kext.executable"))));
        }
        catch (Exception) { return false; }
    }

    /// DIR when it is a package, else the one package folder right inside it (the zip's top folder); null otherwise.
    public static string? FindIn(string dir, Rules? rules = null)
    {
        if (!Directory.Exists(dir)) return null;
        if (IsPackage(dir, rules)) return Path.GetFullPath(dir);
        var inside = Directory.GetDirectories(dir).Where(d => IsPackage(d, rules)).ToList();
        return inside.Count == 1 ? Path.GetFullPath(inside[0]) : null;
    }

    /// A package beside the app: the app's folder, the folder above it (the app inside the unpacked -efi.zip), or a
    /// package folder in either.
    public static string? FindBeside(string appDir, Rules? rules = null)
    {
        var parent = Directory.GetParent(Path.GetFullPath(appDir).TrimEnd('\\', '/'))?.FullName;
        foreach (var d in new[] { appDir, parent })
            if (d is not null && FindIn(d, rules) is { } p) return p;
        return null;
    }

    /// The -efi.zip unpacked into CACHE/<the zip's SHA-256>/ (once; a half-unpacked folder is unpacked again), and the
    /// package in it. Every entry must stay inside that folder.
    public static string Unzip(string zip, string cache, Rules? rules = null)
    {
        string sha;
        using (var s = File.OpenRead(zip)) sha = Convert.ToHexStringLower(SHA256.HashData(s));
        var dest = Path.Combine(cache, sha[..16]);
        var done = Path.Combine(dest, ".unpacked");
        if (!File.Exists(done))
        {
            if (Directory.Exists(dest)) Util.Delete(dest);
            Directory.CreateDirectory(dest);
            var root = Path.GetFullPath(dest) + Path.DirectorySeparatorChar;
            using var archive = ZipFile.OpenRead(zip);
            foreach (var e in archive.Entries)
            {
                var to = Path.GetFullPath(Path.Combine(dest, e.FullName.Replace('\\', '/').Replace('/', Path.DirectorySeparatorChar)));
                if (!to.StartsWith(root, StringComparison.Ordinal)) throw new EfiException("the zip names a file outside its folder: " + e.FullName);
                if (e.FullName.EndsWith('/')) { Directory.CreateDirectory(to); continue; }
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                e.ExtractToFile(to, false);
            }
            File.WriteAllText(done, zip + "\n");
        }
        return FindIn(dest, rules) ?? throw new EfiException("no EFI/OC/Kexts/IntelXe.kext in " + Path.GetFileName(zip) + ": not an intel-gpu-macos-driver -efi.zip");
    }

    /// The package's state, by the engine (EfiFolder.CheckSums, KextVersion): the package folder itself is taken as the
    /// "EFI" here, nothing is written.
    public static EfiPackageInfo Inspect(string dir, Rules? rules = null)
    {
        var r = rules ?? EfiRules.Embedded;
        try
        {
            var f = new EfiFolder(dir, dir, Path.GetTempPath(), r);
            if (f.PackageProblem is { } problem) return new EfiPackageInfo(dir, problem, "no package", "", "");
            return new EfiPackageInfo(f.PackageDir, null, f.CheckSums(), EfiFolder.KextVersion(f.PackageKext),
                f.HpmPackage ? EfiFolder.KextVersion(f.PackageHpm) : "");
        }
        catch (Exception e) { return new EfiPackageInfo(dir, e.Message, "no package", "", ""); }
    }
}
