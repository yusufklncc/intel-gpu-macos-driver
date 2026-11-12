// SPDX-License-Identifier: MIT
// The Xe probe stick in the Core: the disk rules against the vectors write-stick.sh shares
// (tools/probe-stick/stick-disk-vectors.json), the image check, the raw writer with a file standing in for the disk,
// the helper's command line, and the reports on a stick: a real one (Fixtures/probe-stick/REPORTS/, the safe scan of
// tools/probe-stick/test/qemu-test.sh: QEMU's planted values only, redacted) and made-up ones that are broken on purpose.
using System.Formats.Tar;
using System.IO.Compression;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using IntelXe.Core.ProbeStick;
using IntelXe.Core.Stick;

namespace IntelXe.Core.Tests;

public class StickDiskVectorTests
{
    static readonly Dictionary<string, int> Buses = new()
    {
        ["usb"] = 7, ["nvme"] = 17, ["sata"] = 11, ["sas"] = 10, ["raid"] = 8, ["sd"] = 12, ["spaces"] = 16, ["virtual"] = StickRules.FileBackedVirtualBus,
    };

    static JsonElement Vectors() => JsonDocument.Parse(File.ReadAllText(Paths.RepoFile("tools", "probe-stick", "stick-disk-vectors.json"))).RootElement;

    [Fact]
    public void TheLimitIsWriteSticksMaxGb() => Assert.Equal(StickRules.LargeDisk, Vectors().GetProperty("maxBytes").GetUInt64());

    [Fact]
    public void EveryVectorGivesItsCodes()
    {
        var root = Vectors();
        int n = 0;
        foreach (var v in root.GetProperty("vectors").EnumerateArray())
        {
            if (v.TryGetProperty("windows", out var w) && !w.GetBoolean()) continue;   // a partition: the app lists whole disks only
            Assert.True(v.GetProperty("whole").GetBoolean());
            bool system = v.GetProperty("system").GetBoolean();
            var disk = new StickDisk
            {
                Number = 4, FriendlyName = "disk", Size = v.GetProperty("size").GetUInt64(), BusType = Buses[v.GetProperty("bus").GetString()!],
                Removable = v.GetProperty("removable").GetBoolean(), IsBoot = system, IsSystem = system, IdentitySource = "id",
            };
            var policy = new StickPolicy(v.GetProperty("minimum").GetUInt64(), AllowLarge: v.GetProperty("allowLarge").GetBoolean());
            var want = v.GetProperty("refuse").EnumerateArray().Select(e => e.GetString()!).Order().ToList();
            Assert.True(want.SequenceEqual(StickRules.RefusalCodes(disk, policy).Order()),
                $"{v.GetProperty("name").GetString()}: {string.Join(",", StickRules.RefusalCodes(disk, policy))}, expected {string.Join(",", want)}");
            Assert.All(want, c => Assert.Contains(c, root.GetProperty("codes").EnumerateArray().Select(e => e.GetString())));
            n++;
        }
        Assert.True(n >= 15);
    }

    [Fact]
    public void AnEfiIsNotRefusedButMustBeConfirmed()
    {
        StickDisk With(params StickPartition[] p) => new() { Number = 3, FriendlyName = "x", Size = 16_000_000_000, BusType = 7, Removable = true, IdentitySource = "s", Partitions = p };
        var esp = With(new StickPartition(1, "{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}", 0, "", "FAT32", false));
        var mbrEsp = With(new StickPartition(1, "", 0xEF, "", "", false));
        var oc = With(new StickPartition(1, "", 0x0C, "E:", "FAT32", false, HoldsEfi: true));
        var plain = With(new StickPartition(1, "", 0x0C, "E:", "FAT32", false));
        var probe = With(new StickPartition(1, "", 0x0C, "E:", "FAT32", false, ProbeStick: true));
        foreach (var d in new[] { esp, mbrEsp, oc })
        {
            Assert.Empty(StickRules.Refusals(d, new StickPolicy(0)));
            Assert.True(StickRules.HoldsEfi(d));
            Assert.NotNull(StickRules.EfiNotConfirmed(d, false));
            Assert.Null(StickRules.EfiNotConfirmed(d, true));
        }
        Assert.False(StickRules.HoldsEfi(plain));
        Assert.Null(StickRules.EfiNotConfirmed(plain, false));
        Assert.False(StickRules.HoldsEfi(probe));
        Assert.True(StickRules.IsProbeStick(probe));
    }
}

public class ProbeImageTests
{
    /// A small image with the stick's layout: an MBR whose first partition (FAT32 LBA, at 1 MiB) has a FAT32 boot sector
    /// labelled XEPROBE, and random bytes after it; with its .sha256 as build.sh writes it.
    internal static string MakeImage(string dir, string name = "xe-probe-stick-0.1.0.img", int size = 4 << 20, string label = "XEPROBE", byte type = 0x0C, bool sha = true, int seed = 1)
    {
        var b = new byte[size];
        new Random(seed).NextBytes(b);
        Array.Clear(b, 0, 512);
        b[446] = 0x80;
        b[446 + 4] = type;
        BitConverter.GetBytes(2048u).CopyTo(b, 446 + 8);
        BitConverter.GetBytes((uint)(size / 512 - 2048)).CopyTo(b, 446 + 12);
        b[510] = 0x55; b[511] = 0xAA;
        int bs = 2048 * 512;
        Array.Clear(b, bs, 512);
        b[bs] = 0xEB; b[bs + 1] = 0x58; b[bs + 2] = 0x90;
        BitConverter.GetBytes((ushort)512).CopyTo(b, bs + 11);
        Encoding.ASCII.GetBytes(label.PadRight(11)).CopyTo(b, bs + 71);
        Encoding.ASCII.GetBytes("FAT32   ").CopyTo(b, bs + 82);
        b[bs + 510] = 0x55; b[bs + 511] = 0xAA;
        var path = Path.Combine(dir, name);
        File.WriteAllBytes(path, b);
        if (sha) File.WriteAllText(path + ".sha256", Convert.ToHexStringLower(SHA256.HashData(b)) + "  " + name + "\n");
        return path;
    }

    [Fact]
    public void AcceptsTheStickWithItsHash()
    {
        var dir = Directory.CreateTempSubdirectory("ixe-probe-").FullName;
        try
        {
            var img = MakeImage(dir);
            var fractions = new List<double>();
            var info = ProbeImage.Verify(img, fractions.Add);
            Assert.Equal(4 << 20, info.Size);
            Assert.Equal("0.1.0", info.Version);
            Assert.Equal(Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(img))), info.Sha256);
            Assert.Equal(1.0, fractions[^1]);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Theory]
    [InlineData("changed", "does not match its .sha256")]
    [InlineData("nosha", "no xe-probe-stick-0.1.0.img.sha256")]
    [InlineData("badsha", "holds no SHA-256")]
    [InlineData("label", "labelled \"MACOS\", not XEPROBE")]
    [InlineData("ntfs", "not FAT32 (type 0x07)")]
    [InlineData("nombr", "no MBR")]
    [InlineData("odd", "512-byte sectors")]
    [InlineData("tiny", "not a probe stick image")]
    public void RefusesAnImageThatIsNotTheStick(string what, string message)
    {
        var dir = Directory.CreateTempSubdirectory("ixe-probe-").FullName;
        try
        {
            var img = what switch
            {
                "nosha" => MakeImage(dir, sha: false),
                "label" => MakeImage(dir, label: "MACOS"),
                "ntfs" => MakeImage(dir, type: 0x07),
                "odd" => MakeImage(dir, size: (4 << 20) + 100),
                "tiny" => Tiny(dir),
                _ => MakeImage(dir),
            };
            if (what == "changed") { var b = File.ReadAllBytes(img); b[3_000_000] ^= 1; File.WriteAllBytes(img, b); }
            if (what == "badsha") File.WriteAllText(img + ".sha256", "not a hash\n");
            if (what == "nombr") { var b = File.ReadAllBytes(img); b[510] = 0; File.WriteAllBytes(img, b); File.WriteAllText(img + ".sha256", Convert.ToHexStringLower(SHA256.HashData(b))); }
            var e = Assert.Throws<ProbeException>(() => ProbeImage.Verify(img));
            Assert.Contains(message, e.Message);
        }
        finally { Directory.Delete(dir, true); }
    }

    static string Tiny(string dir)
    {
        var p = Path.Combine(dir, "xe-probe-stick-0.1.0.img");
        var b = new byte[512 * 1024];
        File.WriteAllBytes(p, b);
        File.WriteAllText(p + ".sha256", Convert.ToHexStringLower(SHA256.HashData(b)) + "  xe-probe-stick-0.1.0.img\n");
        return p;
    }

    [Fact]
    public void ReadsTheHashFileAsSha256SumWritesIt()
    {
        var dir = Directory.CreateTempSubdirectory("ixe-probe-").FullName;
        try
        {
            var img = MakeImage(dir, name: "renamed.img");
            var h = Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(img)));
            File.WriteAllText(img + ".sha256", h.ToUpperInvariant() + " *xe-probe-stick-0.1.0.img\r\n");   // one line, another name
            Assert.Equal(h, ProbeImage.ExpectedHash(img));
            File.WriteAllText(img + ".sha256", new string('0', 64) + "  other.img\n" + h + "  renamed.img\n");
            Assert.Equal(h, ProbeImage.ExpectedHash(img));
            Assert.Equal("", ProbeImage.VersionOf(img));
            Assert.Equal(h, ProbeImage.Verify(img).Sha256);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void FindsTheNewestImageBesideTheApp()
    {
        var dir = Directory.CreateTempSubdirectory("ixe-probe-").FullName;
        try
        {
            var app = Path.Combine(dir, "app");
            Directory.CreateDirectory(Path.Combine(app, "probe-stick"));
            MakeImage(app, "xe-probe-stick-0.1.0.img");
            MakeImage(Path.Combine(app, "probe-stick"), "xe-probe-stick-0.10.0.img");
            MakeImage(app, "xe-probe-stick-0.2.0.img");
            MakeImage(app, "xe-probe-stick-0.9.0.img", sha: false);   // not checkable: not offered
            var found = ProbeImage.Find(ProbeImage.Folders(app, Path.Combine(dir, "local")));
            Assert.Equal(["0.10.0", "0.2.0", "0.1.0"], found.Select(ProbeImage.VersionOf));
        }
        finally { Directory.Delete(dir, true); }
    }
}

public class ProbeWriterTests
{
    const string Salt = "00112233445566778899AABBCCDDEEFF";
    const ulong DiskSize = 64UL << 20;

    static StickDisk Usb(uint n = 3, ulong size = DiskSize, string serial = "PROBE-TEST-0001", IReadOnlyList<StickPartition>? parts = null, int bus = 7, bool boot = false) => new()
    {
        Number = n, FriendlyName = "Test Stick", Size = size, BusType = bus, Removable = bus == 7, IdentitySource = serial, IsBoot = boot,
        PartitionStyle = "MBR", Partitions = parts ?? [new StickPartition(1, "", 0x0C, "E:", "FAT32", false)],
    };

    /// Windows faked: the disks, and a file for the one opened.
    sealed class FakeSystem(List<StickDisk> disks, string? file = null, int sector = 512) : IProbeSystem
    {
        public int Opened;
        public FileRawDisk? Last;
        public Func<StickDisk, StickDisk>? OnSecondLook;
        public Func<IRawDisk, IRawDisk>? Wrap;
        int looks;
        public IReadOnlyList<StickDisk> ListDisks() => disks;
        public StickDisk? Inspect(uint number)
        {
            var d = disks.FirstOrDefault(x => x.Number == number);
            if (d is not null && ++looks == 2 && OnSecondLook is not null) d = OnSecondLook(d);
            return d;
        }
        public IRawDisk Open(StickDisk disk)
        {
            Assert.NotNull(file);   // a refusal test must never get here
            Opened++;
            Last = new FileRawDisk(file!, sector);
            return Wrap is null ? Last : Wrap(Last);
        }
    }

    sealed class Workspace : IDisposable
    {
        public readonly string Dir = Directory.CreateTempSubdirectory("ixe-probew-").FullName;
        public readonly string Image;
        public readonly string Disk;
        public Workspace(int imageSize = 4 << 20, long diskSize = (long)DiskSize)
        {
            Image = ProbeImageTests.MakeImage(Dir, size: imageSize);
            Disk = Path.Combine(Dir, "disk.bin");
            // the stick's old life: a pattern everywhere, so that what the writer leaves and what it zeroes both show
            using var f = new FileStream(Disk, FileMode.Create);
            var pattern = Enumerable.Repeat((byte)0xEE, 1 << 20).ToArray();
            for (long o = 0; o < diskSize; o += pattern.Length) f.Write(pattern, 0, (int)Math.Min(pattern.Length, diskSize - o));
        }
        public void Dispose() => Directory.Delete(Dir, true);
    }

    static ProbeHelperRequest Request(StickDisk d, Workspace w, ProbeVerb verb = ProbeVerb.Write, bool eraseEfi = false, bool large = false) =>
        new(verb, d.Number, d.Size, d.Token(Salt), Salt, w.Image, eraseEfi, large);

    [Theory]
    [InlineData(512, 4 << 20)]
    [InlineData(4096, (4 << 20) + 512)]       // an image that ends inside a 4K sector: the last sector padded with zeros
    [InlineData(512, (9 << 20) + 1024)]       // more than two blocks
    public void WritesTheImageAndReadsItBack(int sector, int imageSize)
    {
        using var w = new Workspace(imageSize);
        var d = Usb();
        var sys = new FakeSystem([d], w.Disk, sector);
        var stages = new List<string>();
        var r = ProbeWriter.Run(Request(d, w), sys, (s, _) => stages.Add(s));
        Assert.True(r.Ok, r.Error);
        Assert.True(r.Written);
        Assert.Equal(1, sys.Opened);
        var disk = File.ReadAllBytes(w.Disk);
        var image = File.ReadAllBytes(w.Image);
        Assert.True(disk.AsSpan(0, image.Length).SequenceEqual(image));
        int padded = (imageSize + sector - 1) / sector * sector;
        Assert.All(disk[image.Length..padded], b => Assert.Equal(0, b));
        Assert.All(disk[padded..(padded + 4096)], b => Assert.Equal(0xEE, b));                    // the rest is left as it was ...
        Assert.All(disk[(disk.Length - ProbeWriter.Tail)..], b => Assert.Equal(0, b));           // ... but the last MiB
        Assert.Equal(0, sys.Last!.WriteOffsets[^1]);                                              // the partition table last
        Assert.DoesNotContain(0L, sys.Last.WriteOffsets.SkipLast(1));
        Assert.True(sys.Last.Flushed);
        Assert.Contains(r.Lines, l => l.StartsWith("read back: the first " + imageSize + " bytes of the disk match the image"));
        Assert.Equal(["verify-image", "write", "read-back", "done"], stages.Distinct());
    }

    [Fact]
    public void ACheckWritesNothing()
    {
        using var w = new Workspace();
        var d = Usb();
        var r = ProbeWriter.Run(Request(d, w, ProbeVerb.Check), new FakeSystem([d]));
        Assert.True(r.Ok, r.Error);
        Assert.False(r.Written);
        Assert.Contains("check only: nothing written", r.Lines);
    }

    [Theory]
    [InlineData("nvme")]
    [InlineData("boot")]
    [InlineData("small")]
    [InlineData("large")]
    [InlineData("other")]
    [InlineData("swapped")]
    [InlineData("efi")]
    [InlineData("oc")]
    [InlineData("image")]
    [InlineData("files")]
    public void RefusesBeforeAnyWrite(string what)
    {
        using var w = new Workspace();
        var d = what switch
        {
            "nvme" => Usb(bus: 17),
            "boot" => Usb(boot: true),
            "small" => Usb(size: 2 << 20),
            "large" => Usb(size: 256_060_514_304),
            "efi" => Usb(parts: [new StickPartition(1, "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}", 0, "", "FAT32", false)]),
            "oc" => Usb(parts: [new StickPartition(1, "", 0x0C, "E:", "FAT32", false, HoldsEfi: true)]),
            "files" => Usb(parts: [new StickPartition(1, "", 0x0C, w.Image[..2] is [_, ':'] ? w.Image[..2] : "E:", "FAT32", false)]),
            _ => Usb(),
        };
        var sys = new FakeSystem(what == "other" ? [Usb(serial: "PROBE-TEST-0002")] : [d]);
        if (what == "swapped") sys.OnSecondLook = _ => Usb(serial: "PROBE-TEST-0003");
        if (what == "image") { var b = File.ReadAllBytes(w.Image); b[2_000_000] ^= 1; File.WriteAllBytes(w.Image, b); }
        var q = Request(d, w);
        if (what == "files" && w.Image[1] != ':') return;   // a drive letter only on Windows
        var r = ProbeWriter.Run(q, sys);
        Assert.False(r.Ok);
        Assert.False(r.Written);
        Assert.Equal(0, sys.Opened);
        Assert.Contains(what == "image" ? "does not match its .sha256" : "nothing written", r.Error);
        if (what is "efi" or "oc") Assert.Contains("holds an EFI", r.Error);
        if (what == "large") Assert.Contains(r.Refusals!, x => x.StartsWith("larger than 128.0 GB"));
    }

    [Fact]
    public void AnEfiStickIsWrittenOnceConfirmed()
    {
        using var w = new Workspace();
        var d = Usb(parts: [new StickPartition(1, "", 0x0C, "E:", "FAT32", false, HoldsEfi: true)]);
        var r = ProbeWriter.Run(Request(d, w, eraseEfi: true), new FakeSystem([d], w.Disk));
        Assert.True(r.Ok, r.Error);
        Assert.Contains(r.Lines, l => l.Contains("it holds an EFI, and erasing it was confirmed"));
    }

    [Fact]
    public void ALargeDiskOnlyWhenAllowed()
    {
        using var w = new Workspace();
        var d = Usb(size: 256_060_514_304);
        var r = ProbeWriter.Run(Request(d, w, ProbeVerb.Check, large: true), new FakeSystem([d]));
        Assert.True(r.Ok, r.Error);
    }

    /// A disk that loses one write: the read-back must say so.
    sealed class LosingDisk(IRawDisk inner, long lose) : IRawDisk
    {
        public long Length => inner.Length;
        public int SectorSize => inner.SectorSize;
        public void Write(long offset, ReadOnlySpan<byte> data) { if (offset != lose) inner.Write(offset, data); }
        public int Read(long offset, Span<byte> buffer) => inner.Read(offset, buffer);
        public void Flush() => inner.Flush();
        public void Dispose() => inner.Dispose();
    }

    [Fact]
    public void TheReadBackFindsALostWrite()
    {
        using var w = new Workspace(9 << 20);
        var d = Usb();
        var sys = new FakeSystem([d], w.Disk) { Wrap = inner => new LosingDisk(inner, ProbeWriter.Block) };
        var r = ProbeWriter.Run(Request(d, w), sys);
        Assert.False(r.Ok);
        Assert.True(r.Written);
        Assert.Contains("the read-back differs from the image", r.Error);
    }

    [Fact]
    public void AnImageChangedAfterItsCheckNeverGetsItsPartitionTable()
    {
        using var w = new Workspace(9 << 20);
        var info = ProbeImage.Verify(w.Image);
        var b = File.ReadAllBytes(w.Image);
        b[6 << 20] ^= 1;
        File.WriteAllBytes(w.Image, b);
        using var dev = new FileRawDisk(w.Disk);
        var e = Assert.Throws<ProbeException>(() => ProbeWriter.Write(info, dev));
        Assert.Contains("changed after it was checked", e.Message);
        Assert.DoesNotContain(0L, dev.WriteOffsets);
        Assert.Equal(0xEE, File.ReadAllBytes(w.Disk)[0]);
    }

    [Fact]
    public void TheCommandLineRoundTrips()
    {
        var q = new ProbeHelperRequest(ProbeVerb.Write, 4, 16_008_609_792, "AB", Salt, @"C:\x\xe-probe-stick-0.1.0.img", EraseEfi: true, AllowLarge: true,
            TestVhdx: @"C:\t.vhdx", Out: @"C:\o.json", Progress: @"C:\p.txt");
        Assert.Equal(q, ProbeHelperRequest.Parse(q.ToArgs()));
        var plain = q with { EraseEfi = false, AllowLarge = false, TestVhdx = null };
        Assert.Equal(plain, ProbeHelperRequest.Parse(plain.ToArgs()));
        Assert.DoesNotContain("--erase-efi", plain.ToArgs());
        Assert.DoesNotContain(Usb().ToString().Split(' '), a => a.Contains("PROBE-TEST"));
        var stick = new StickHelperRequest(StickVerb.Write, 3, "ID", Salt, 1, "img", null, EraseEfi: true, AllowLarge: true);
        Assert.Equal(stick, StickHelperRequest.Parse(stick.ToArgs()));
        Assert.Contains("--erase-efi", stick.ToArgs());
    }

    [Theory]
    [InlineData("--probe-helper")]
    [InlineData("--probe-helper erase --disk 3")]
    [InlineData("--probe-helper write --disk 3 --size 1 --image x")]
    [InlineData("--probe-helper write --disk x --id a --salt b --size 1 --image x")]
    [InlineData("--probe-helper write --disk 3 --id a --salt b --size 1")]
    [InlineData("--probe-helper write --disk 3 --disk 4 --id a --salt b --size 1 --image x")]
    [InlineData("--probe-helper write --disk 3 --id a --salt b --size 1 --image x --erase-efi --erase-efi")]
    [InlineData("--probe-helper write --disk 3 --id a --salt b --size 1 --image x --force")]
    [InlineData("--probe-helper write --disk 3 --id a --salt b --size 1 --image")]
    public void RefusesBadCommandLines(string line) =>
        Assert.Throws<ProbeException>(() => ProbeHelperRequest.Parse(line.Split(' ')));
}

public class ProbeReportTests
{
    static string Fixture => Paths.RepoFile("windows", "app", "IntelXe.Core.Tests", "Fixtures", "probe-stick");

    [Fact]
    public void FindsTheStickByItsLabelAndMarker()
    {
        Assert.Equal([Fixture], ProbeReports.FindSticks([(Fixture, "XEPROBE"), (Path.GetTempPath(), "XEPROBE"), (Fixture, "MACOS"), ("Z:\\nothing", "XEPROBE")]));
        Assert.True(ProbeReports.IsProbeStick(Fixture, "xeprobe "));
    }

    [Fact]
    public void ARealReportIsSealed()
    {
        var r = Assert.Single(ProbeReports.Read(Fixture));
        Assert.Empty(r.Problems);
        Assert.Equal(ProbeReportState.Sealed, r.State);
        Assert.Equal("XR-954586D6", r.Id);
        Assert.Equal(31, r.SealedFiles);
        Assert.StartsWith("Xe probe stick report XR-954586D6", r.SummaryText);
        Assert.False(r.TooLargeForGitHub);
        Assert.Equal("https://github.com/" + Project.Repository + "/issues/new?template=2-new-hardware.yml&reports=XR-954586D6", Project.NewGpuFormUrl(r.Id).AbsoluteUri);
    }

    /// A report as scan.sh packs it: NAME/ with the files and SEAL.sha256, the .txt and the .sha256 beside it.
    static string MakeReport(string reports, string id, Action<Dictionary<string, string>>? beforeSeal = null, Action<Dictionary<string, string>>? afterSeal = null,
        Action<TarWriter>? extra = null)
    {
        Directory.CreateDirectory(reports);
        var name = "xe-stick-" + id;
        var files = new Dictionary<string, string>
        {
            ["summary.txt"] = "Xe probe stick report " + id + "\n",
            ["before/system/dmi.txt"] = "sys_vendor XsTest\n",
            ["trace/xe-0000:03:00.0.txt"] = "bind result: 0\n",
        };
        beforeSeal?.Invoke(files);
        var seal = string.Concat(files.OrderBy(f => f.Key, StringComparer.Ordinal).Select(f => Convert.ToHexStringLower(SHA256.HashData(Encoding.UTF8.GetBytes(f.Value))) + "  " + f.Key + "\n"));
        afterSeal?.Invoke(files);
        if (seal.Length > 0) files["SEAL.sha256"] = seal;
        var archive = Path.Combine(reports, name + ".tar.gz");
        using (var f = File.Create(archive))
        using (var gz = new GZipStream(f, CompressionLevel.Fastest))
        using (var tar = new TarWriter(gz, TarEntryFormat.Gnu))
        {
            tar.WriteEntry(new GnuTarEntry(TarEntryType.Directory, name + "/"));
            foreach (var (path, text) in files)
                tar.WriteEntry(new GnuTarEntry(TarEntryType.RegularFile, name + "/" + path) { DataStream = new MemoryStream(Encoding.UTF8.GetBytes(text)) });
            extra?.Invoke(tar);
        }
        File.WriteAllText(Path.Combine(reports, name + ".txt"), files.GetValueOrDefault("summary.txt", ""));
        string H(string p) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(Path.Combine(reports, p))));
        File.WriteAllText(Path.Combine(reports, name + ".sha256"), $"{H(name + ".tar.gz")}  {name}.tar.gz\n{H(name + ".txt")}  {name}.txt\n");
        return archive;
    }

    [Theory]
    [InlineData("good", "")]
    [InlineData("changed", "before/system/dmi.txt does not match SEAL.sha256")]
    [InlineData("unsealed", "stick/extra.txt is in the archive but not in SEAL.sha256")]
    [InlineData("missing", "SEAL.sha256 lists trace/xe-0000:03:00.0.txt, which is not in the archive")]
    [InlineData("outside", "the archive holds other/x.txt, outside xe-stick-XR-0000ABCD/")]
    [InlineData("link", "as a SymbolicLink, not a file")]
    [InlineData("txt", "xe-stick-XR-0000ABCD.txt does not match xe-stick-XR-0000ABCD.sha256")]
    [InlineData("nosums", "xe-stick-XR-0000ABCD.sha256 is missing")]
    [InlineData("corrupt", "the archive cannot be read")]
    [InlineData("noseal", "the archive holds no SEAL.sha256")]
    public void ChecksTheSeals(string what, string problem)
    {
        var dir = Directory.CreateTempSubdirectory("ixe-reports-").FullName;
        try
        {
            var reports = Path.Combine(dir, "REPORTS");
            const string id = "XR-0000ABCD";
            var archive = what switch
            {
                "changed" => MakeReport(reports, id, afterSeal: f => f["before/system/dmi.txt"] = "sys_vendor Changed\n"),
                "unsealed" => MakeReport(reports, id, afterSeal: f => f["stick/extra.txt"] = "x"),
                "missing" => MakeReport(reports, id, afterSeal: f => f.Remove("trace/xe-0000:03:00.0.txt")),
                "outside" => MakeReport(reports, id, extra: t => t.WriteEntry(new GnuTarEntry(TarEntryType.RegularFile, "other/x.txt") { DataStream = new MemoryStream([1]) })),
                "link" => MakeReport(reports, id, extra: t => t.WriteEntry(new GnuTarEntry(TarEntryType.SymbolicLink, "xe-stick-" + id + "/l") { LinkName = "/etc/passwd" })),
                "noseal" => MakeReport(reports, id, beforeSeal: f => f.Clear()),
                _ => MakeReport(reports, id),
            };
            if (what == "txt") File.AppendAllText(Path.ChangeExtension(Path.ChangeExtension(archive, null), ".txt"), "changed");
            if (what == "nosums") File.Delete(Path.Combine(reports, "xe-stick-" + id + ".sha256"));
            if (what == "corrupt")
            {
                var b = File.ReadAllBytes(archive);
                for (int i = 20; i < b.Length - 8; i++) b[i] ^= 0x5A;
                File.WriteAllBytes(archive, b);
                string H(string p) => Convert.ToHexStringLower(SHA256.HashData(File.ReadAllBytes(p)));
                File.WriteAllText(Path.Combine(reports, "xe-stick-" + id + ".sha256"), $"{H(archive)}  xe-stick-{id}.tar.gz\n");
            }
            var r = Assert.Single(ProbeReports.Read(dir));
            Assert.Equal(id, r.Id);
            if (what == "good")
            {
                Assert.Empty(r.Problems);
                Assert.Equal(ProbeReportState.Sealed, r.State);
                Assert.Equal(3, r.SealedFiles);
            }
            else
            {
                Assert.Equal(ProbeReportState.Broken, r.State);
                Assert.Contains(r.Problems, p => p.Contains(problem));
            }
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void APrivacyCheckThatFailedIsShownAsSuch()
    {
        var dir = Directory.CreateTempSubdirectory("ixe-reports-").FullName;
        try
        {
            var reports = Path.Combine(dir, "REPORTS");
            MakeReport(reports, "XR-11112222");
            File.WriteAllText(Path.Combine(reports, "xe-stick-XR-33334444-privacy-check-failed.txt"), "Xe probe stick XR-33334444: the privacy check failed, so no report was written.\n");
            File.SetLastWriteTimeUtc(Path.Combine(reports, "xe-stick-XR-11112222.tar.gz"), new DateTime(2026, 1, 1, 0, 0, 0, DateTimeKind.Utc));
            File.WriteAllText(Path.Combine(reports, "xe-stick-state.txt"), "skip abc collect-after\n");
            File.WriteAllText(Path.Combine(reports, "README.txt"), "x");
            var list = ProbeReports.Read(dir);
            Assert.Equal(["XR-33334444", "XR-11112222"], list.Select(r => r.Id));
            Assert.Equal(ProbeReportState.PrivacyCheckFailed, list[0].State);
            Assert.Null(list[0].Archive);
            Assert.Contains("privacy check failed", list[0].SummaryText);
            Assert.Equal(ProbeReportState.Sealed, list[1].State);
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public void AStickWithoutReportsHasNone()
    {
        var dir = Directory.CreateTempSubdirectory("ixe-reports-").FullName;
        try { Assert.Empty(ProbeReports.Read(dir)); }
        finally { Directory.Delete(dir, true); }
    }
}
