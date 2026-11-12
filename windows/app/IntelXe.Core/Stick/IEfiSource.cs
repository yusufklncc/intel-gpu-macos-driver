// SPDX-License-Identifier: MIT
// Where the stick's OpenCore EFI folder comes from. Today: a folder the user chooses (FolderEfiSource). A generated EFI
// (windows/efibuild) plugs in here as another IEfiSource: it hands the stick page a folder that holds BOOT and OC.
namespace IntelXe.Core.Stick;

public interface IEfiSource
{
    /// what the page shows: the folder, or what made it
    string Describe { get; }

    /// The EFI folder to copy to the stick's root as EFI\ (it holds BOOT\BOOTx64.efi and OC\OpenCore.efi). Throws
    /// EfiSourceException with the reason when there is none.
    Task<string> PrepareAsync(CancellationToken cancel = default);
}

public sealed class EfiSourceException(string message) : Exception(message);

/// An EFI folder chosen by the user: the folder named EFI, or a folder that holds one.
public sealed class FolderEfiSource(string path) : IEfiSource
{
    public string Describe => path;

    public Task<string> PrepareAsync(CancellationToken cancel = default) => Task.FromResult(EfiFolderCheck.Resolve(path));
}

public static class EfiFolderCheck
{
    public static readonly string[] RequiredFiles = [Path.Combine("BOOT", "BOOTx64.efi"), Path.Combine("OC", "OpenCore.efi"), Path.Combine("OC", "config.plist")];

    /// The EFI folder at path (path itself, or path\EFI), checked to hold OpenCore; throws EfiSourceException.
    public static string Resolve(string path)
    {
        if (string.IsNullOrWhiteSpace(path) || !Directory.Exists(path)) throw new EfiSourceException("no folder " + path);
        var dir = Path.GetFullPath(path);
        if (!Exists(dir, "OC") && Directory.Exists(Path.Combine(dir, "EFI"))) dir = Path.Combine(dir, "EFI");
        var missing = RequiredFiles.Where(f => !FileExists(dir, f)).ToList();
        if (missing.Count > 0) throw new EfiSourceException($"{dir} is not an OpenCore EFI folder: no {string.Join(", ", missing)}");
        return dir;
    }

    static bool Exists(string dir, string sub) => Directory.Exists(Path.Combine(dir, sub));

    // case-insensitive, as FAT32 and Windows are (the tests also run on case-sensitive file systems)
    static bool FileExists(string dir, string rel)
    {
        var cur = dir;
        foreach (var part in rel.Split(Path.DirectorySeparatorChar))
        {
            var next = Directory.EnumerateFileSystemEntries(cur).FirstOrDefault(e => string.Equals(Path.GetFileName(e), part, StringComparison.OrdinalIgnoreCase));
            if (next is null) return false;
            cur = next;
        }
        return File.Exists(cur);
    }
}
