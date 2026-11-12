// SPDX-License-Identifier: MIT
// The stick writer with Windows faked: the disks, and erasing (a temporary folder stands in for the new FAT32
// partition). The refusal tests use a system whose EraseAndFormat fails the test if it is ever called.
using System.Security.Cryptography;
using IntelXe.Core.Stick;

namespace IntelXe.Core.Tests;

public class StickTests
{
    const string Salt = "00112233445566778899AABBCCDDEEFF";

    static StickDisk Usb(uint n = 3, ulong size = 16_008_609_792, string serial = "4C530001230000123456", bool? removable = true,
        IReadOnlyList<StickPartition>? parts = null, int bus = StickRules.UsbBus, string file = "") => new()
    {
        Number = n, FriendlyName = "SanDisk Ultra", Size = size, BusType = bus, Removable = removable, IdentitySource = serial,
        PartitionStyle = "MBR", Partitions = parts ?? [new StickPartition(1, "", 0x0C, "E:", "FAT32", false)], VirtualFile = file,
    };

    static StickDisk Nvme(uint n, bool boot) => new()
    {
        Number = n, FriendlyName = "WD_BLACK SN850X 2000GB", Size = 2_000_398_934_016, BusType = 17, IsBoot = boot, IsSystem = boot,
        Removable = false, IdentitySource = "E823_8FA6_BF53_0001_001B_448B_4A1E_0000.", PartitionStyle = "GPT",
        Partitions = boot
            ? [new StickPartition(1, "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}", 0, "", "FAT32", false),
               new StickPartition(2, "{e3c9e316-0b5c-4db8-817d-f92df00215ae}", 0, "", "", false),
               new StickPartition(3, "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}", 0, "C:", "NTFS", true)]
            : [new StickPartition(1, "{c12a7328-f81f-11d2-ba4b-00a0c93ec93b}", 0, "", "FAT32", false),
               new StickPartition(2, "{7c3457ef-0000-11aa-aa11-00306543ecac}", 0, "", "", false)],
    };

    static readonly StickPolicy Policy = new(StickRules.MinimumFor(960_530_321));

    // ---- the rules ----

    [Fact]
    public void AnEmptyUsbStickIsAllowed() => Assert.Empty(StickRules.Refusals(Usb(), Policy));

    [Fact]
    public void RefusesTheInternalDisks()
    {
        var boot = StickRules.Refusals(Nvme(0, true), Policy);
        Assert.Contains(boot, r => r.StartsWith("not a USB disk (bus: NVMe)"));
        Assert.Contains("Windows booted from it (boot disk)", boot);
        Assert.Contains(boot, r => r.Contains("system (EFI) partition"));
        Assert.Contains("not a removable disk", boot);
        Assert.Contains("partition 3 holds Windows", boot);
        var mac = StickRules.Refusals(Nvme(1, false), Policy);
        Assert.Contains("partition 2 is an APFS container (macOS)", mac);
        Assert.False(StickRules.Listed(Nvme(0, true), Policy));
        Assert.False(StickRules.Listed(Nvme(1, false), Policy));
    }

    [Theory]
    [InlineData("fixed")]
    [InlineData("unknown")]
    [InlineData("readonly")]
    [InlineData("offline")]
    [InlineData("small")]
    [InlineData("windows")]
    [InlineData("linux")]
    [InlineData("noserial")]
    [InlineData("protected")]
    public void RefusesUsbDisksThatMustNotBeErased(string what)
    {
        var d = what switch
        {
            "fixed" => Usb(removable: false),
            "unknown" => Usb(removable: null),
            "readonly" => new StickDisk { Number = 3, FriendlyName = "x", Size = 16_000_000_000, BusType = 7, Removable = true, IsReadOnly = true, IdentitySource = "s" },
            "offline" => new StickDisk { Number = 3, FriendlyName = "x", Size = 16_000_000_000, BusType = 7, Removable = true, IsOffline = true, IdentitySource = "s" },
            "small" => Usb(size: 1_000_000_000),
            "windows" => Usb(parts: [new StickPartition(1, "", 7, "F:", "NTFS", true)]),
            "linux" => Usb(parts: [new StickPartition(1, "{4f68bce3-e8cd-4db1-96e7-fbcaf984b709}", 0, "", "", false)]),
            "noserial" => Usb(serial: " "),
            _ => Usb(),
        };
        var policy = what == "protected" ? Policy with { ProtectedLetters = ["E:"] } : Policy;
        Assert.NotEmpty(StickRules.Refusals(d, policy));
    }

    [Fact]
    public void AVhdxCountsOnlyWhenNamedOnTheCommandLine()
    {
        var vhdx = Usb(bus: StickRules.FileBackedVirtualBus, removable: false, file: Path.Combine(Path.GetTempPath(), "stick-test.vhdx"));
        Assert.False(StickRules.Listed(vhdx, Policy));
        Assert.Contains(StickRules.Refusals(vhdx, Policy), r => r.StartsWith("not a USB disk"));
        var test = Policy with { TestVirtualFile = Path.Combine(Path.GetTempPath(), "STICK-TEST.vhdx") };
        Assert.True(StickRules.Listed(vhdx, test));
        Assert.Empty(StickRules.Refusals(vhdx, test));
        // another file, or a disk that is not file-backed, is not the test disk
        Assert.NotEmpty(StickRules.Refusals(vhdx, Policy with { TestVirtualFile = Path.Combine(Path.GetTempPath(), "other.vhdx") }));
        Assert.NotEmpty(StickRules.Refusals(Nvme(1, false), test));
        Assert.NotEmpty(StickRules.Refusals(Usb(bus: 17, removable: false, file: test.TestVirtualFile!), test));
        // the switch does not lift the other rules
        var bootVhdx = new StickDisk { Number = 5, FriendlyName = "v", Size = 2UL << 30, BusType = 15, VirtualFile = test.TestVirtualFile!, IsBoot = true, IdentitySource = "x" };
        Assert.Contains("Windows booted from it (boot disk)", StickRules.Refusals(bootVhdx, test));
    }

    [Fact]
    public void TheIdentityNeverShowsTheSerial()
    {
        var d = Usb();
        Assert.DoesNotContain("4C530001230000123456", d.ToString());
        Assert.DoesNotContain("4C530001230000123456", d.Token(Salt));
        Assert.Equal(d.Token(Salt), Usb().Token(Salt));
        Assert.NotEqual(d.Token(Salt), Usb(serial: "4C530001230000123457").Token(Salt));
        Assert.NotEqual(d.Token(Salt), Usb(size: 16_008_609_793).Token(Salt));
        Assert.NotEqual(d.Token(Salt), d.Token(StickIdentity.NewSalt()));
        Assert.Equal("SanDisk Ultra 16.0 GB", d.ConfirmText);
        var list = StickWriter.List(new FakeSystem([Nvme(0, true), Usb()]), Policy, Salt);
        Assert.DoesNotContain("4C530001230000123456", list.ToJson());
        Assert.Single(list.Disks!);
    }

    [Theory]
    [InlineData(16_008_609_792UL, "16.0 GB")]
    [InlineData(2_147_483_648UL, "2.1 GB")]
    [InlineData(512_000_000UL, "512 MB")]
    public void SizeText(ulong bytes, string text) => Assert.Equal(text, StickRules.SizeText(bytes));

    // ---- the helper's command line ----

    [Fact]
    public void TheCommandLineRoundTrips()
    {
        var q = new StickHelperRequest(StickVerb.Write, 3, "AB", Salt, 16_008_609_792, @"C:\cache\tahoe", @"D:\EFI", @"C:\t\stick-test.vhdx", Out: @"C:\o.json", Progress: @"C:\p.txt");
        var back = StickHelperRequest.Parse(q.ToArgs());
        Assert.Equal(q, back);
        Assert.DoesNotContain(q.ToArgs(), a => a.Contains("4C53", StringComparison.Ordinal));
        Assert.Equal(StickVerb.List, StickHelperRequest.Parse(["--stick-helper", "list", "--salt", Salt, "--out", "x"]).Verb);
        Assert.Equal("tahoe", StickHelperRequest.Parse(["--stick-helper", "download", "--version", "tahoe", "--image", "d"]).Version);
    }

    [Theory]
    [InlineData("--stick-helper")]
    [InlineData("--stick-helper erase --disk 3")]
    [InlineData("--stick-helper write --disk 3 --size 1 --image x")]                 // no identity
    [InlineData("--stick-helper write --disk x --id a --salt b --size 1 --image x")]
    [InlineData("--stick-helper write --disk 3 --id a --salt b --size 1")]          // no image
    [InlineData("--stick-helper write --disk 3 --disk 4 --id a --salt b --size 1 --image x")]
    [InlineData("--stick-helper write --disk 3 --id a --salt b --size 1 --image x --force yes")]
    [InlineData("--stick-helper download --version tahoe")]
    public void RefusesBadCommandLines(string line) =>
        Assert.Throws<StickException>(() => StickHelperRequest.Parse(line.Split(' ')));

    // ---- the writer ----

    sealed class FakeSystem(List<StickDisk> disks, string? root = null) : IStickSystem
    {
        public int Erased;
        public Func<StickDisk, StickDisk>? OnSecondLook;
        int looks;
        public IReadOnlyList<StickDisk> ListDisks() => disks;
        public StickDisk? Inspect(uint number)
        {
            var d = disks.FirstOrDefault(x => x.Number == number);
            if (d is not null && ++looks == 2 && OnSecondLook is not null) d = OnSecondLook(d);
            return d;
        }
        public string EraseAndFormat(StickDisk disk, ulong partitionSize, string label)
        {
            Assert.NotNull(root);   // a refusal test must never get here
            Erased++;
            Assert.Equal("MACOS", label);
            Assert.True(partitionSize <= StickRules.MaxPartition);
            foreach (var e in Directory.EnumerateFileSystemEntries(root!)) { if (Directory.Exists(e)) Directory.Delete(e, true); else File.Delete(e); }
            return root!;
        }
    }

    sealed class Workspace : IDisposable
    {
        public readonly string Dir = Directory.CreateTempSubdirectory("ixe-stick-").FullName;
        public string Image => Path.Combine(Dir, "cache");
        public string Root => Path.Combine(Dir, "stick");
        public string Efi => Path.Combine(Dir, "myefi", "EFI");
        public readonly RSA Key = RSA.Create(2048);
        public byte[] ImageBytes = new byte[200_000];

        public Workspace()
        {
            new Random(3).NextBytes(ImageBytes);
            var boot = Path.Combine(Image, RecoveryFiles.BootFolder);
            Directory.CreateDirectory(boot);
            File.WriteAllBytes(Path.Combine(boot, "BaseSystem.dmg"), ImageBytes);
            File.WriteAllBytes(Path.Combine(boot, "BaseSystem.chunklist"), Chunklist.Build(ImageBytes, 65536, Key));
            File.WriteAllText(Path.Combine(Image, RecoveryFiles.Marker), "{\"image\":\"BaseSystem.dmg\",\"chunklist\":\"BaseSystem.chunklist\",\"product\":\"000-00000\"}");
            Directory.CreateDirectory(Root);
            Directory.CreateDirectory(Path.Combine(Efi, "BOOT"));
            Directory.CreateDirectory(Path.Combine(Efi, "OC", "Kexts", "Lilu.kext"));
            File.WriteAllText(Path.Combine(Efi, "BOOT", "BOOTx64.efi"), "boot");
            File.WriteAllText(Path.Combine(Efi, "OC", "OpenCore.efi"), "oc");
            File.WriteAllText(Path.Combine(Efi, "OC", "config.plist"), "<plist/>");
            File.WriteAllText(Path.Combine(Efi, "OC", "Kexts", "Lilu.kext", "Info.plist"), "<plist/>");
        }

        public RSAParameters Public => Key.ExportParameters(false);
        public void Dispose() { Key.Dispose(); Directory.Delete(Dir, true); }
    }

    static StickHelperRequest Write(StickDisk d, Workspace w, bool efi = true, StickVerb verb = StickVerb.Write) =>
        new(verb, d.Number, d.Token(Salt), Salt, d.Size, w.Image, efi ? Path.GetDirectoryName(w.Efi) : null);

    [Fact]
    public void WritesTheStickAndReadsItBack()
    {
        using var w = new Workspace();
        var d = Usb();
        var sys = new FakeSystem([Nvme(0, true), d], w.Root);
        var stages = new List<string>();
        var r = StickWriter.Run(Write(d, w), sys, (s, _) => stages.Add(s), key: w.Public);
        Assert.True(r.Ok, r.Error);
        Assert.True(r.Written);
        Assert.Equal(1, sys.Erased);
        Assert.Equal(w.ImageBytes, File.ReadAllBytes(Path.Combine(w.Root, "com.apple.recovery.boot", "BaseSystem.dmg")));
        Assert.True(File.Exists(Path.Combine(w.Root, "com.apple.recovery.boot", "BaseSystem.chunklist")));
        Assert.True(File.Exists(Path.Combine(w.Root, "EFI", "OC", "Kexts", "Lilu.kext", "Info.plist")));
        Assert.Contains(r.Lines, l => l.StartsWith("read back: the image on the stick matches"));
        Assert.Contains(r.Lines, l => l == "read back: 4 EFI files match");
        Assert.Equal("done", stages[^1]);
    }

    [Fact]
    public void ACheckWritesNothing()
    {
        using var w = new Workspace();
        var d = Usb();
        var r = StickWriter.Run(Write(d, w, verb: StickVerb.Check), new FakeSystem([d]), key: w.Public);
        Assert.True(r.Ok, r.Error);
        Assert.False(r.Written);
        Assert.Contains("check only: nothing written", r.Lines);
    }

    [Fact]
    public void RefusesTheBootDiskBeforeAnyWrite()
    {
        using var w = new Workspace();
        foreach (var disk in new[] { Nvme(0, true), Nvme(1, false) })
        {
            // even with a matching identity, as if the user had picked it
            var r = StickWriter.Run(Write(disk, w), new FakeSystem([disk]), key: w.Public);
            Assert.False(r.Ok);
            Assert.False(r.Written);
            Assert.NotEmpty(r.Refusals!);
            Assert.Contains("nothing written", r.Error);
        }
    }

    [Fact]
    public void RefusesAnotherDiskAtTheSameNumber()
    {
        using var w = new Workspace();
        var chosen = Usb();
        var swapped = Usb(serial: "OTHER-STICK");
        var r = StickWriter.Run(Write(chosen, w), new FakeSystem([swapped]), key: w.Public);
        Assert.False(r.Ok);
        Assert.False(r.Written);
        Assert.Contains("not the disk that was chosen", r.Error);
        var resized = Usb(size: 32_000_000_000);
        Assert.False(StickWriter.Run(Write(chosen, w), new FakeSystem([resized]), key: w.Public).Ok);
    }

    [Fact]
    public void LooksAgainRightBeforeErasing()
    {
        using var w = new Workspace();
        var d = Usb();
        // the stick is pulled and another one put in between the check and the erase
        var sys = new FakeSystem([d]) { OnSecondLook = _ => Usb(serial: "SWAPPED") };
        var r = StickWriter.Run(Write(d, w), sys, key: w.Public);
        Assert.False(r.Ok);
        Assert.False(r.Written);
        Assert.Contains("not the disk that was chosen", r.Error);
        // or it became a disk that holds Windows
        var sys2 = new FakeSystem([d]) { OnSecondLook = x => Usb(parts: [new StickPartition(1, "", 7, "F:", "NTFS", true)]) };
        var r2 = StickWriter.Run(Write(d, w), sys2, key: w.Public);
        Assert.False(r2.Written);
        Assert.NotEmpty(r2.Refusals!);
    }

    [Fact]
    public void RefusesAChangedImageBeforeErasing()
    {
        using var w = new Workspace();
        var d = Usb();
        var bytes = (byte[])w.ImageBytes.Clone();
        bytes[100] ^= 1;
        File.WriteAllBytes(Path.Combine(w.Image, "com.apple.recovery.boot", "BaseSystem.dmg"), bytes);
        var r = StickWriter.Run(Write(d, w), new FakeSystem([d]), key: w.Public);
        Assert.False(r.Ok);
        Assert.False(r.Written);
        Assert.Contains("chunk 1", r.Error);
    }

    [Fact]
    public void RefusesAFolderThatIsNotAnEfi()
    {
        using var w = new Workspace();
        File.Delete(Path.Combine(w.Efi, "OC", "OpenCore.efi"));
        var d = Usb();
        var r = StickWriter.Run(Write(d, w), new FakeSystem([d]), key: w.Public);
        Assert.False(r.Ok);
        Assert.False(r.Written);
        Assert.Contains("not an OpenCore EFI folder", r.Error);
    }

    [Fact]
    public async Task TheFolderSourceFindsTheEfi()
    {
        using var w = new Workspace();
        Assert.Equal(w.Efi, await new FolderEfiSource(Path.GetDirectoryName(w.Efi)!).PrepareAsync());
        Assert.Equal(w.Efi, await new FolderEfiSource(w.Efi).PrepareAsync());
        await Assert.ThrowsAsync<EfiSourceException>(() => new FolderEfiSource(w.Image).PrepareAsync());
    }

    [Fact]
    public void TheVhdxTestSwitchIsCarriedToTheRules()
    {
        using var w = new Workspace();
        var file = Path.Combine(w.Dir, "stick-test.vhdx");
        var vhdx = Usb(bus: StickRules.FileBackedVirtualBus, removable: false, file: file);
        var without = StickWriter.Run(Write(vhdx, w, efi: false), new FakeSystem([vhdx]), key: w.Public);
        Assert.False(without.Written);
        var with = StickWriter.Run(Write(vhdx, w, efi: false) with { TestVhdx = file }, new FakeSystem([vhdx], w.Root), key: w.Public);
        Assert.True(with.Ok, with.Error);
    }
}
