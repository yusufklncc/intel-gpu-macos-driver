// SPDX-License-Identifier: MIT
// Writing the Xe probe stick's image raw to a USB disk, without Windows APIs: the elevated helper's command line, and the
// order of the work. The app starts its own exe with "--probe-helper write ..." through "runas", as the installer stick
// does with --stick-helper; only that process has administrator rights, for one disk. It checks the image (layout and
// SHA-256), reads the disk again and refuses it by the shared StickRules, compares it with the one the user chose (serial
// and size, as a salted hash), refuses a disk holding an EFI unless the user confirmed that (--erase-efi), reads it once
// more right before writing, then (IProbeSystem: Windows in the app, a file in the tests) locks and dismounts its volumes
// and writes \\.\PhysicalDriveN in aligned 4 MiB blocks: every block but the first, the disk's last MiB zeroed (a GPT's
// backup header from the stick's earlier life would contradict the image's MBR), the first block last (until then no
// partition table points at a half-written file system). Then it flushes, has Windows read the new partition table,
// reads the image's length back and compares its SHA-256.
using System.Globalization;
using System.Security.Cryptography;
using IntelXe.Core.Stick;

namespace IntelXe.Core.ProbeStick;

public enum ProbeVerb { Check, Write }

/// One helper run. Check: everything Write does before writing, and nothing after.
public sealed record ProbeHelperRequest(ProbeVerb Verb, uint Disk, ulong Size, string Id, string Salt, string Image,
    bool EraseEfi = false, bool AllowLarge = false, string? TestVhdx = null, string? Out = null, string? Progress = null)
{
    public const string Switch = "--probe-helper";

    public IReadOnlyList<string> ToArgs()
    {
        var a = new List<string>
        {
            Switch, Verb.ToString().ToLowerInvariant(), "--disk", Disk.ToString(CultureInfo.InvariantCulture),
            "--size", Size.ToString(CultureInfo.InvariantCulture), "--id", Id, "--salt", Salt, "--image", Image,
        };
        if (EraseEfi) a.Add("--erase-efi");
        if (AllowLarge) a.Add("--allow-large");
        void Add(string name, string? value) { if (!string.IsNullOrEmpty(value)) a.AddRange([name, value]); }
        Add("--test-vhdx", TestVhdx);
        Add("--progress", Progress);
        Add("--out", Out);
        return a;
    }

    public static ProbeHelperRequest Parse(IReadOnlyList<string> args)
    {
        if (args.Count < 2 || args[0] != Switch) throw new ProbeException("the helper's command line starts with " + Switch + " check|write");
        var verb = args[1] switch
        {
            "check" => ProbeVerb.Check, "write" => ProbeVerb.Write,
            _ => throw new ProbeException("no helper verb " + args[1] + " (check, write)"),
        };
        var v = HelperArgs.Parse(args, 2, ["--disk", "--size", "--id", "--salt", "--image", "--test-vhdx", "--out", "--progress"],
            ["--erase-efi", "--allow-large"], m => new ProbeException(m));
        string? V(string k) => v.TryGetValue(k, out var s) ? s : null;
        if (!uint.TryParse(V("--disk"), NumberStyles.None, CultureInfo.InvariantCulture, out var disk)) throw new ProbeException("the helper needs --disk N");
        if (!ulong.TryParse(V("--size"), NumberStyles.None, CultureInfo.InvariantCulture, out var size)) throw new ProbeException("the helper needs --size BYTES");
        if (string.IsNullOrEmpty(V("--id")) || string.IsNullOrEmpty(V("--salt"))) throw new ProbeException("nothing written: the helper needs the disk's identity (--id, --salt)");
        if (string.IsNullOrEmpty(V("--image"))) throw new ProbeException("the helper needs --image FILE");
        return new ProbeHelperRequest(verb, disk, size, V("--id")!, V("--salt")!, V("--image")!, v.ContainsKey("--erase-efi"), v.ContainsKey("--allow-large"),
            V("--test-vhdx"), V("--out"), V("--progress"));
    }
}

/// A whole disk opened for raw writing. Offsets and lengths are whole sectors (Windows' unbuffered I/O requires it;
/// the file stand-in checks it too).
public interface IRawDisk : IDisposable
{
    /// The disk's size in bytes.
    long Length { get; }
    int SectorSize { get; }
    void Write(long offset, ReadOnlySpan<byte> data);
    /// Reads buffer.Length bytes (or up to the disk's end) at offset; returns how many.
    int Read(long offset, Span<byte> buffer);
    /// Writes everything through to the disk and has Windows read its new partition table (IOCTL_DISK_UPDATE_PROPERTIES).
    void Flush();
}

/// What the probe writer needs from Windows: the disks, and one opened raw with its volumes locked and dismounted.
public interface IProbeSystem : IStickDisks
{
    /// Locks and dismounts every volume of the disk (checked by the caller just before), and opens it for raw writing
    /// and reading. Disposing the disk closes it and releases the volumes.
    IRawDisk Open(StickDisk disk);
}

public static class ProbeWriter
{
    public const int Block = 4 << 20;
    /// The disk's last MiB is zeroed: where a GPT keeps its backup header.
    public const int Tail = 1 << 20;

    /// Check or Write. progress: a stage (verify-image, write, read-back, done) and a fraction 0..1. protectedLetters:
    /// drives that must not be written besides the rules' (where the app is).
    public static StickHelperResult Run(ProbeHelperRequest q, IProbeSystem sys, Action<string, double>? progress = null,
        IReadOnlyList<string>? protectedLetters = null, CancellationToken cancel = default)
    {
        var r = new StickHelperResult();
        try
        {
            progress?.Invoke("verify-image", 0);
            var image = ProbeImage.Verify(q.Image, f => progress?.Invoke("verify-image", f), cancel);
            r.Lines.Add($"image {Path.GetFileName(image.Path)}: {ProbeImage.SizeText(image.Size)}, the stick's layout, SHA-256 matches its .sha256 ({image.Sha256[..16]}…)");

            var letters = new List<string>(protectedLetters ?? []);
            if (image.Path is { Length: >= 2 } p && p[1] == ':') letters.Add(p[..2]);
            var policy = new StickPolicy((ulong)image.Size, q.TestVhdx, letters, q.AllowLarge);

            var disk = StickWriter.Reinspect(sys, q.Disk, q.Size, q.Id, q.Salt, policy, q.EraseEfi, r);
            if (disk is null) return r;
            r.Lines.Add($"disk {disk.Number}: {disk.FriendlyName} {StickRules.SizeText(disk.Size)} {disk.BusName}, the one chosen, nothing refuses it"
                + (StickRules.HoldsEfi(disk) ? "; it holds an EFI, and erasing it was confirmed" : ""));
            if (q.Verb == ProbeVerb.Check)
            {
                r.Lines.Add("check only: nothing written");
                r.Ok = true;
                return r;
            }

            // read it once more right before writing
            disk = StickWriter.Reinspect(sys, q.Disk, q.Size, q.Id, q.Salt, policy, q.EraseEfi, r);
            if (disk is null) return r;
            progress?.Invoke("write", 0);
            using (var dev = sys.Open(disk))
            {
                r.Written = true;
                Write(image, dev, f => progress?.Invoke("write", f), cancel);
                r.Lines.Add($"wrote {ProbeImage.SizeText(image.Size)} to disk {disk.Number} in {Block >> 20} MiB blocks (sector {dev.SectorSize} bytes), the first block last; the disk's last MiB zeroed");
                progress?.Invoke("read-back", 0);
                var back = ReadBack(dev, image.Size, f => progress?.Invoke("read-back", f), cancel);
                if (back != image.Sha256) throw new ProbeException($"the read-back differs from the image (SHA-256 {back[..12]}…): try another stick");
                r.Lines.Add($"read back: the first {image.Size} bytes of the disk match the image (SHA-256 {back[..16]}…)");
            }
            progress?.Invoke("done", 1);
            r.Ok = true;
        }
        catch (Exception e)
        {
            r.Ok = false;
            r.Error = e is AggregateException { InnerException: { } inner } ? inner.Message : e.Message;
        }
        return r;
    }

    /// The image onto the disk: blocks 1.. first, then the disk's last MiB zeroed, then block 0; then Flush.
    public static void Write(ProbeImageInfo image, IRawDisk dev, Action<double>? progress = null, CancellationToken cancel = default)
    {
        int sector = dev.SectorSize;
        if (sector <= 0 || Block % sector != 0 || Tail % sector != 0) throw new ProbeException($"the disk's sector size {sector} is not supported");
        long padded = (image.Size + sector - 1) / sector * sector;
        if (dev.Length < padded) throw new ProbeException($"the disk ({dev.Length} bytes) is smaller than the image");
        using var src = new FileStream(image.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        if (src.Length != image.Size) throw new ProbeException("the image changed after it was checked");
        var buf = new byte[Block];
        long blocks = (padded + Block - 1) / Block;
        long done = 0;
        byte[]? first = null;
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        for (long b = 0; b < blocks; b++)
        {
            cancel.ThrowIfCancellationRequested();
            long offset = b * Block;
            int want = (int)Math.Min(Block, padded - offset);
            Array.Clear(buf, 0, want);
            int got = 0, n;
            while (got < want && (n = src.Read(buf, got, (int)Math.Min(want, image.Size - offset) - got)) > 0) got += n;
            if (got != Math.Min(want, image.Size - offset)) throw new ProbeException("the image ended early while writing");
            hash.AppendData(buf, 0, got);
            if (b == 0) { first = buf[..want]; continue; }   // the partition table goes on last
            dev.Write(offset, buf.AsSpan(0, want));
            done += want;
            progress?.Invoke((double)done / padded);
        }
        // the bytes written are the bytes checked: a file changed between the check and here is not left on the disk
        if (Convert.ToHexStringLower(hash.GetHashAndReset()) != image.Sha256) throw new ProbeException("the image changed after it was checked: the stick is not usable, write it again");

        long tailStart = Math.Max(padded, (dev.Length - Tail) / sector * sector);
        if (tailStart < dev.Length)
        {
            var zeros = new byte[(int)Math.Min(Tail, dev.Length - tailStart) / sector * sector];
            if (zeros.Length > 0) dev.Write(tailStart, zeros);
        }
        dev.Write(0, first!);
        progress?.Invoke(1);
        dev.Flush();
    }

    /// SHA-256 of the disk's first length bytes, read in whole sectors.
    public static string ReadBack(IRawDisk dev, long length, Action<double>? progress = null, CancellationToken cancel = default)
    {
        int sector = dev.SectorSize;
        long padded = (length + sector - 1) / sector * sector;
        var buf = new byte[Block];
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        for (long offset = 0; offset < padded; offset += Block)
        {
            cancel.ThrowIfCancellationRequested();
            int want = (int)Math.Min(Block, padded - offset);
            int got = dev.Read(offset, buf.AsSpan(0, want));
            if (got != want) throw new ProbeException($"the disk ended after {offset + got} bytes while reading it back");
            hash.AppendData(buf, 0, (int)Math.Min(want, length - offset));
            progress?.Invoke((double)Math.Min(offset + want, length) / length);
        }
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }
}

/// A file standing in for a disk (the tests; a VHD's file is not written this way): a fixed length, and only whole
/// sectors at sector-aligned offsets, as Windows' unbuffered I/O on \\.\PhysicalDriveN allows.
public sealed class FileRawDisk(string path, int sectorSize = 512) : IRawDisk
{
    readonly FileStream f = new(path, FileMode.Open, FileAccess.ReadWrite, FileShare.Read);
    public long Length => f.Length;
    public int SectorSize => sectorSize;
    public int Writes { get; private set; }
    public List<long> WriteOffsets { get; } = [];
    public bool Flushed { get; private set; }

    void Aligned(long offset, int length)
    {
        if (offset % sectorSize != 0 || length % sectorSize != 0) throw new IOException($"unaligned access at {offset}, {length} bytes (sector {sectorSize})");
    }

    public void Write(long offset, ReadOnlySpan<byte> data)
    {
        Aligned(offset, data.Length);
        if (offset + data.Length > f.Length) throw new IOException("write past the disk's end");
        f.Position = offset;
        f.Write(data);
        Writes++;
        WriteOffsets.Add(offset);
    }

    public int Read(long offset, Span<byte> buffer)
    {
        Aligned(offset, buffer.Length);
        f.Position = offset;
        int total = 0, n;
        while (total < buffer.Length && (n = f.Read(buffer[total..])) > 0) total += n;
        return total;
    }

    public void Flush() { f.Flush(true); Flushed = true; }
    public void Dispose() => f.Dispose();
}
