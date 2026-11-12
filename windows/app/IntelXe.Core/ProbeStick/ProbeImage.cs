// SPDX-License-Identifier: MIT
// The Xe probe stick's image (tools/probe-stick/build.sh: xe-probe-stick-VERSION.img and .img.sha256), as the app
// finds and checks it before writing: beside the app (or in its probe-stick folder, or the one under %LOCALAPPDATA%), or
// a file the user picked. It must look like the stick (an MBR with one FAT32 partition labelled XEPROBE) and match the
// SHA-256 in the .sha256 beside it; without one the app does not write it (write-stick.sh writes it unchecked, with a
// note; a release always ships the .sha256).
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;

namespace IntelXe.Core.ProbeStick;

public sealed class ProbeException(string message) : Exception(message);

/// A checked image: its size, SHA-256 (lower-case hex, as sha256sum prints it) and version ("" when its name has none).
public sealed record ProbeImageInfo(string Path, long Size, string Sha256, string Version);

public static partial class ProbeImage
{
    /// The FAT label of the stick's partition and the file that marks it (tools/probe-stick/image/build-inside.sh).
    public const string Label = "XEPROBE";
    public const string Marker = "xe-probe-stick.marker";
    public const string FilePattern = "xe-probe-stick-*.img";
    /// The stick's image is about 208 MB; anything above this is not one.
    public const long MaxSize = 4L << 30;

    [GeneratedRegex(@"^xe-probe-stick-(\d+(?:\.\d+){0,3})\.img$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant)]
    private static partial Regex NameRe();

    /// "0.1.0" from xe-probe-stick-0.1.0.img; "" for another name.
    public static string VersionOf(string path) => NameRe().Match(System.IO.Path.GetFileName(path)) is { Success: true } m ? m.Groups[1].Value : "";

    /// The folders the app looks in, in order: beside the exe, its probe-stick folder, the download folder.
    public static IReadOnlyList<string> Folders(string appFolder, string localAppData) =>
        [appFolder, System.IO.Path.Combine(appFolder, "probe-stick"), System.IO.Path.Combine(localAppData, "IntelXe Manager", "probe-stick")];

    /// The images in those folders that have a .sha256 beside them, the newest version first.
    public static IReadOnlyList<string> Find(IEnumerable<string> folders)
    {
        var found = new List<string>();
        foreach (var dir in folders)
        {
            try
            {
                if (!Directory.Exists(dir)) continue;
                foreach (var f in Directory.EnumerateFiles(dir, FilePattern))
                    if (VersionOf(f).Length > 0 && File.Exists(f + ".sha256") && !found.Contains(System.IO.Path.GetFullPath(f), StringComparer.OrdinalIgnoreCase))
                        found.Add(System.IO.Path.GetFullPath(f));
            }
            catch (IOException) { }
            catch (UnauthorizedAccessException) { }
        }
        return [.. found.OrderByDescending(f => System.Version.TryParse(Pad(VersionOf(f)), out var v) ? v : new System.Version(0, 0))];
    }

    static string Pad(string v) => v.Contains('.') ? v : v + ".0";

    /// The SHA-256 the .sha256 beside the image names: sha256sum's "HASH  NAME" (or "HASH *NAME"); with several lines, the
    /// one naming this file; a single bare hash is taken too.
    public static string ExpectedHash(string imagePath)
    {
        var file = imagePath + ".sha256";
        if (!File.Exists(file)) throw new ProbeException($"no {System.IO.Path.GetFileName(file)} beside the image: it cannot be checked, so it is not written");
        var info = new FileInfo(file);
        if (info.Length > 64 * 1024) throw new ProbeException($"{info.Name} is not a SHA-256 file");
        var lines = File.ReadAllLines(file).Select(l => l.Trim()).Where(l => l.Length > 0 && !l.StartsWith('#')).ToList();
        string name = System.IO.Path.GetFileName(imagePath);
        var entries = lines.Select(l => l.Split((char[]?)null, 2, StringSplitOptions.RemoveEmptyEntries))
            .Where(p => IsHex64(p[0]))
            .Select(p => (Hash: p[0], Name: p.Length > 1 ? p[1].TrimStart('*').Trim() : ""))
            .ToList();
        // one line: its hash, whatever name it gives (a renamed image; write-stick.sh reads the first field only)
        string? hash = entries.Count == 1 ? entries[0].Hash
            : entries.FirstOrDefault(e => string.Equals(System.IO.Path.GetFileName(e.Name), name, StringComparison.OrdinalIgnoreCase)).Hash;
        return hash?.ToLowerInvariant() ?? throw new ProbeException($"{info.Name} holds no SHA-256 for {name}");
    }

    static bool IsHex64(string s) => s.Length == 64 && s.All(Uri.IsHexDigit);

    /// Whether the bytes are the stick's layout: an MBR whose first partition is FAT32 and starts inside the image, with a
    /// FAT32 boot sector labelled XEPROBE there. Throws with the reason.
    public static void CheckLayout(Stream s)
    {
        long length = s.Length;
        if (length < (1 << 20) || length > MaxSize) throw new ProbeException($"{length} bytes: not a probe stick image (about 200 MB)");
        if (length % 512 != 0) throw new ProbeException("its size is not a whole number of 512-byte sectors: not a disk image");
        var mbr = new byte[512];
        s.Position = 0;
        s.ReadExactly(mbr);
        if (mbr[510] != 0x55 || mbr[511] != 0xAA) throw new ProbeException("no MBR: not a probe stick image");
        byte type = mbr[446 + 4];
        long start = BitConverter.ToUInt32(mbr, 446 + 8);
        if (type is not (0x0B or 0x0C)) throw new ProbeException($"its first partition is not FAT32 (type 0x{type:X2}): not a probe stick image");
        if (start == 0 || (start + 1) * 512 > length) throw new ProbeException("its first partition starts outside the image");
        var boot = new byte[512];
        s.Position = start * 512;
        s.ReadExactly(boot);
        if (boot[510] != 0x55 || boot[511] != 0xAA || Encoding.ASCII.GetString(boot, 82, 8) != "FAT32   ")
            throw new ProbeException("its partition holds no FAT32 file system: not a probe stick image");
        var label = Encoding.ASCII.GetString(boot, 71, 11).TrimEnd();
        if (label != Label) throw new ProbeException($"its partition is labelled \"{label}\", not {Label}: not a probe stick image");
    }

    /// The image checked: its layout, then its SHA-256 against the .sha256 beside it. progress: 0..1 while hashing.
    public static ProbeImageInfo Verify(string path, Action<double>? progress = null, CancellationToken cancel = default)
    {
        if (!File.Exists(path)) throw new ProbeException($"no image {path}");
        var want = ExpectedHash(path);
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        CheckLayout(f);
        f.Position = 0;
        var got = Hash(f, f.Length, progress, cancel);
        if (got != want) throw new ProbeException($"{System.IO.Path.GetFileName(path)} does not match its .sha256 (SHA-256 {got[..12]}…, expected {want[..12]}…): download it again");
        return new ProbeImageInfo(System.IO.Path.GetFullPath(path), f.Length, got, VersionOf(path));
    }

    /// SHA-256 of the next length bytes of the stream, lower-case hex.
    public static string Hash(Stream s, long length, Action<double>? progress = null, CancellationToken cancel = default)
    {
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        var buf = new byte[4 << 20];
        long done = 0;
        while (done < length)
        {
            cancel.ThrowIfCancellationRequested();
            int n = s.Read(buf, 0, (int)Math.Min(buf.Length, length - done));
            if (n <= 0) throw new ProbeException($"the image ended after {done} of {length} bytes");
            h.AppendData(buf, 0, n);
            done += n;
            progress?.Invoke((double)done / length);
        }
        return Convert.ToHexStringLower(h.GetHashAndReset());
    }

    public static string SizeText(long bytes) => (bytes / 1e6).ToString("0", CultureInfo.InvariantCulture) + " MB";
}
