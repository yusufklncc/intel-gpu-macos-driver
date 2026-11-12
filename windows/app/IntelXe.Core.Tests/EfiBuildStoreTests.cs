// SPDX-License-Identifier: MIT
// The SMBIOS identity kept out of the window (SmbiosMask), the scan and build folders, saving a built EFI, the built EFI
// as the stick page's IEfiSource, and the app's own engine checking stage 2 in a built EFI.
using IntelXe.Core.EfiBuild;
using IntelXe.Core.Stick;
using IntelXeEfi;

namespace IntelXe.Core.Tests;

public class EfiBuildStoreTests
{
    // ---- masking ----

    [Fact]
    public void TheIdentityInEveryForm()
    {
        using var t = new TempDir();
        var efi = FakeConfig.WriteEfi(t.Path);
        var s = SmbiosMask.ReadSecrets(Path.Combine(efi, "OC", "config.plist"));
        var rom = FakeConfig.Rom;
        foreach (var form in new[]
                 {
                     FakeConfig.Serial, FakeConfig.Serial.ToLowerInvariant(), FakeConfig.Mlb, FakeConfig.Uuid, FakeConfig.Uuid.ToLowerInvariant(),
                     FakeConfig.Uuid.Replace("-", ""), Convert.ToHexStringLower(rom), Convert.ToHexString(rom), Convert.ToBase64String(rom),
                     string.Join(":", rom.Select(b => b.ToString("x2"))), string.Join(":", rom.Select(b => b.ToString("X2"))),
                 })
            Assert.Contains(form, s);
        Assert.DoesNotContain("MacBookPro16,2", s);
        Assert.True(s.Zip(s.Skip(1)).All(p => p.First.Length >= p.Second.Length), "longest first, so a longer form is replaced whole");

        var text = $"sn {FakeConfig.Serial}, uuid {FakeConfig.Uuid.ToLowerInvariant()}, rom {string.Join(":", rom.Select(b => b.ToString("X2")))}";
        var red = SmbiosMask.Redact(text, s);
        Assert.Equal("sn <redacted>, uuid <redacted>, rom <redacted>", red);
        Assert.Empty(SmbiosMask.Find(red, s));
        Assert.True(SmbiosMask.Find(text, s).Count >= 3);
    }

    [Fact]
    public void ConfigValuesAreMaskedInTheWindow()
    {
        using var t = new TempDir();
        var efi = FakeConfig.WriteEfi(t.Path);
        var summary = SmbiosMask.PlatformSummary(Path.Combine(efi, "OC", "config.plist"));
        Assert.Equal(["MLB", "ROM", "SystemProductName", "SystemSerialNumber", "SystemUUID", "SpoofVendor"], summary.Select(x => x.Key));
        Assert.Equal("MacBookPro16,2", summary.Single(x => x.Key == "SystemProductName").Value);
        Assert.Equal("true", summary.Single(x => x.Key == "SpoofVendor").Value);
        foreach (var k in new[] { "MLB", "ROM", "SystemSerialNumber", "SystemUUID" })
            Assert.Equal(SmbiosMask.Masked, summary.Single(x => x.Key == k).Value);
        var all = string.Join("\n", summary.Select(x => x.Value));
        Assert.Empty(SmbiosMask.Find(all, SmbiosMask.ReadSecrets(Path.Combine(efi, "OC", "config.plist"))));
    }

    [Theory]
    [InlineData("SystemSerialNumber", true)]
    [InlineData("MLB", true)]
    [InlineData("ROM", true)]
    [InlineData("SystemUUID", true)]
    [InlineData("BoardSerialNumber", true)]
    [InlineData("ChassisSerialNumber", true)]
    [InlineData("SmUUID", true)]
    [InlineData("SystemProductName", false)]
    [InlineData("ProcessorType", false)]
    [InlineData("ROMSize", false)]
    public void WhichKeysAreMasked(string key, bool secret)
    {
        Assert.Equal(secret, SmbiosMask.IsSecretKey(key));
        Assert.Equal(secret ? SmbiosMask.Masked : "x", SmbiosMask.Display(key, "x"));
    }

    [Fact]
    public void NoConfigNoSecrets()
    {
        using var t = new TempDir();
        Assert.Empty(SmbiosMask.ReadSecrets(t.Sub("none.plist")));
        FakeEngine.Write(t.Sub("bad.plist"), "<plist><dict><key>PlatformInfo");
        Assert.Empty(SmbiosMask.ReadSecrets(t.Sub("bad.plist")));
        Assert.Empty(SmbiosMask.PlatformSummary(t.Sub("bad.plist")));
        // the sample config's placeholders are read like real values (and masked the same way)
        var sample = SmbiosMask.PlatformSummary(Paths.RepoFile("efi", "sample-z790", "config.plist"));
        Assert.Equal(SmbiosMask.Masked, sample.Single(x => x.Key == "SystemSerialNumber").Value);
    }

    // ---- the folders ----

    [Fact]
    public void ScanAndBuildFolders()
    {
        using var t = new TempDir();
        var store = new EfiBuildStore(t.Path);
        var now = new DateTime(2026, 11, 13, 1, 2, 3);
        var a = store.NewBuildDir(now);
        Assert.Equal(Path.Combine(t.Path, "builds", "20261113-010203"), a);
        Directory.CreateDirectory(a);
        Assert.Equal(Path.Combine(t.Path, "builds", "20261113-010203-2"), store.NewBuildDir(now));
        Assert.Equal(Path.Combine(t.Path, "scans", "20261113-010203"), store.NewScanDir(now));
    }

    [Fact]
    public void OnlyTheBuildersOldBuildsAreDeleted()
    {
        using var t = new TempDir();
        var store = new EfiBuildStore(t.Path);
        string Build(string name, bool marker)
        {
            var d = Path.Combine(store.BuildsDir, name);
            FakeConfig.WriteEfi(d);
            if (marker) FakeEngine.Write(Path.Combine(d, EfiBuildStore.Marker), "made by windows/efibuild");
            return d;
        }
        var old1 = Build("1", true);
        var keep = Build("2", true);
        var current = Build("3", true);
        var mine = Build("4", false);
        Assert.Equal(1, store.PruneBuilds(current, keep));
        Assert.False(Directory.Exists(old1));
        Assert.True(Directory.Exists(keep) && Directory.Exists(current) && Directory.Exists(mine));
        Assert.Equal(0, new EfiBuildStore(t.Sub("nothing")).PruneBuilds());
    }

    [Fact]
    public void SavingTheBuiltEfi()
    {
        using var t = new TempDir();
        var efi = FakeConfig.WriteEfi(t.Sub("build"));
        Directory.CreateDirectory(t.Sub("usb"));
        var saved = EfiBuildStore.SaveTo(efi, t.Sub("usb"));
        Assert.Equal(t.Sub("usb", "EFI"), saved);
        Assert.Equal(File.ReadAllText(Path.Combine(efi, "OC", "config.plist")), File.ReadAllText(Path.Combine(saved, "OC", "config.plist")));
        Assert.Equal(saved, EfiFolderCheck.Resolve(t.Sub("usb")));
        // never over an EFI folder, never into itself, never without a folder
        Assert.Contains("exists already", Assert.Throws<EfiBuildException>(() => EfiBuildStore.SaveTo(efi, t.Sub("usb"))).Message);
        Assert.Throws<EfiBuildException>(() => EfiBuildStore.SaveTo(efi, efi));
        Assert.Throws<EfiBuildException>(() => EfiBuildStore.SaveTo(efi, t.Sub("none")));
        Assert.Throws<EfiSourceException>(() => EfiBuildStore.SaveTo(t.Sub("usb", "nothing"), t.Path));
        Assert.Single(Directory.EnumerateFileSystemEntries(t.Sub("usb")));   // no temporary folder left
    }

    [Fact]
    public async Task TheBuiltEfiForTheStick()
    {
        using var t = new TempDir();
        var efi = FakeConfig.WriteEfi(t.Sub("build"));
        IEfiSource source = new BuiltEfiSource(t.Sub("build"), "built for macOS 26");
        Assert.Equal("built for macOS 26", source.Describe);
        Assert.Equal(efi, await source.PrepareAsync());
        Directory.Delete(t.Sub("build"), true);
        await Assert.ThrowsAsync<EfiSourceException>(() => source.PrepareAsync());
    }

    [Fact]
    public void TheAppsEngineFindsStage2DoneOrNot()
    {
        using var t = new TempDir();
        var efi = FakeConfig.WriteEfi(t.Path);
        var config = Path.Combine(efi, "OC", "config.plist");
        // the sample EFI of the test machine has stage 2 and the Safari setting made
        File.Copy(Paths.RepoFile("efi", "sample-z790", "config.plist"), config, overwrite: true);
        Assert.Empty(EfiBuildStore.Stage2Left(efi, safari: true));
        Assert.Empty(EfiBuildStore.Stage2Left(efi, safari: false));
        // as OpCore Simplify leaves it, before stage 2
        var pl = Plist.Load(config);
        pl.Set(["Booter", "Quirks", "ResizeAppleGpuBars"], PValue.Int(-1));
        File.WriteAllBytes(config, pl.ToBytes());
        Assert.Contains(EfiBuildStore.Stage2Left(efi, safari: true), l => l.Contains("ResizeAppleGpuBars"));
    }
}
