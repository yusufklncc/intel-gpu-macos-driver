// SPDX-License-Identifier: MIT
// The elevated helper's logic with the mounting abstracted (a fake IEspMounter that "mounts" a folder), the package
// (folder, zip, SHA256SUMS) and the command line. Nothing here mounts anything.
using System.IO.Compression;
using IntelXe.Core.Efi;
using IntelXeEfi;

namespace IntelXe.Core.Tests;

public sealed class EfiHelperTests : IDisposable
{
    readonly string root = Path.Combine(Path.GetTempPath(), "ixehelper-" + Guid.NewGuid().ToString("N")[..8]);
    string Efi => Path.Combine(root, "esp");
    string Pkg => Path.Combine(root, "intel-gpu-macos-driver-9.9.9-efi");
    string Backups => Path.Combine(root, "backups");
    string Config => Path.Combine(Efi, "EFI", "OC", "config.plist");

    public EfiHelperTests()
    {
        // an EFI as stage 1 leaves it (windows/test/stage1/config.plist: efi/sample-z790 set back), and a package
        Directory.CreateDirectory(Path.Combine(Efi, "EFI", "OC", "Kexts"));
        Directory.CreateDirectory(Path.Combine(Efi, "EFI", "OC", "ACPI"));
        Directory.CreateDirectory(Path.Combine(Efi, "EFI", "BOOT"));
        File.WriteAllBytes(Path.Combine(Efi, "EFI", "OC", "OpenCore.efi"), []);
        File.Copy(Paths.RepoFile("windows", "test", "stage1", "config.plist"), Config);
        var kext = Path.Combine(Pkg, "EFI", "OC", "Kexts", "IntelXe.kext", "Contents");
        Directory.CreateDirectory(Path.Combine(kext, "MacOS"));
        File.WriteAllText(Path.Combine(kext, "MacOS", "IntelXe"), "stand-in IntelXe executable\n");
        File.WriteAllText(Path.Combine(kext, "Info.plist"), "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>CFBundleVersion</key>\n\t<string>9.9.9</string>\n</dict>\n</plist>\n");
        Directory.CreateDirectory(Path.Combine(Pkg, "EFI", "OC", "ACPI"));
        File.WriteAllBytes(Path.Combine(Pkg, "EFI", "OC", "ACPI", "SSDT-ARCOFF.aml"), File.ReadAllBytes(Paths.RepoFile("windows", "test", "stage1", "SSDT-ARCOFF-0.1.3.aml")));
        WriteSums();
    }

    void WriteSums()
    {
        var files = Directory.GetFiles(Path.Combine(Pkg, "EFI"), "*", SearchOption.AllDirectories)
            .Select(f => f[(Pkg.Length + 1)..].Replace('\\', '/')).OrderBy(f => f, StringComparer.Ordinal);
        File.WriteAllText(Path.Combine(Pkg, "SHA256SUMS"), string.Concat(files.Select(f => Util.Sha256File(Path.Combine(Pkg, Util.Native(f))) + "  " + f + "\n")));
    }

    public void Dispose() { if (Directory.Exists(root)) Util.Delete(root); }

    /// "Mounts" disk 1 partition 1 by handing out the fake EFI's folder; refuses any other partition, as the real one
    /// refuses a partition that is not an EFI system partition.
    sealed class FakeMounter(string folder, string letter = "S:", bool hadLetter = false) : IEspMounter
    {
        public int Mounts, Unmounts;
        public string? Asked;
        public MountedEsp Mount(uint disk, uint partition, string? want)
        {
            if (disk != 1 || partition != 1) throw new EfiException($"disk {disk} partition {partition} is not an EFI system partition");
            Mounts++;
            Asked = want;
            return new MountedEsp(want ?? letter, folder, !hadLetter);
        }
        public void Unmount(MountedEsp esp) { Assert.True(esp.MountedHere); Unmounts++; }
    }

    EfiOptions Options => new(Pkg, "", "", "Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz", false, Backups);
    static readonly EfiTarget.Partition Esp = new(1, 1);

    EfiHelperResult Run(EfiHelperRequest q, IEspMounter m) =>
        // through the command line and the JSON answer, as the app and the helper exchange them
        EfiHelperResult.FromJson(EfiHelper.Run(EfiHelperRequest.Parse(q.ToArgs()), m).ToJson());

    [Fact]
    public void PlanThenApplyOnAMountedPartition()
    {
        var m = new FakeMounter(Efi);
        var plan = Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Settings, Options), m);
        Assert.True(plan.Ok, plan.Error);
        Assert.Equal("S:", plan.Letter);
        Assert.Equal((1, 1), (m.Mounts, m.Unmounts));
        var p = plan.ToPlan(Esp);
        Assert.True(p.HasChanges);
        Assert.Equal("S:", p.Letter);
        Assert.Contains("ResizeAppleGpuBars", p.Text);
        Assert.Equal(EfiWork.Plan(Esp, Efi, EfiChange.Settings, Options).Hash, p.Hash);

        var before = File.ReadAllBytes(Config);
        var done = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Settings, Options, p.Letter, p.Hash), m);
        Assert.True(done.Ok, done.Error);
        Assert.Equal("S:", m.Asked);
        Assert.Equal((2, 2), (m.Mounts, m.Unmounts));
        Assert.NotEqual(before, File.ReadAllBytes(Config));
        Assert.Contains(done.Lines!, l => l.StartsWith("== backup of the whole EFI folder: "));
        Assert.Equal("10", Plist.Raw(Plist.Load(Config).Get("Booter", "Quirks", "ResizeAppleGpuBars")));

        // and undo, with its own plan
        var undo = Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Undo, Options), m).ToPlan(Esp);
        Assert.True(undo.HasChanges);
        Assert.Contains(undo.Lines, l => l.StartsWith("restore  EFI/OC/config.plist"));
        var back = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Undo, Options, undo.Letter, undo.Hash), m);
        Assert.True(back.Ok, back.Error);
        Assert.Equal(before, File.ReadAllBytes(Config));
        Assert.Equal((4, 4), (m.Mounts, m.Unmounts));
    }

    [Fact]
    public void ARefusedChangeWritesNothingAndStillUnmounts()
    {
        var m = new FakeMounter(Efi);
        var before = File.ReadAllBytes(Config);
        var r = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Settings, Options, "S:", new string('0', 64)), m);
        Assert.False(r.Ok);
        Assert.Contains("changed since the plan was shown", r.Error);
        Assert.Equal((1, 1), (m.Mounts, m.Unmounts));
        Assert.Equal(before, File.ReadAllBytes(Config));
        Assert.False(Directory.Exists(Backups));
    }

    [Fact]
    public void TheEfiChangedBetweenPlanAndChange()
    {
        var m = new FakeMounter(Efi);
        var p = Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Settings, Options), m).ToPlan(Esp);
        var pl = Plist.Load(Config);
        pl.Set(["Kernel", "Quirks", "DisableIoMapper"], PValue.Bool(true));   // someone else made one of the steps
        File.WriteAllBytes(Config, pl.ToBytes());
        var edited = File.ReadAllBytes(Config);
        var r = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Settings, Options, p.Letter, p.Hash), m);
        Assert.False(r.Ok);
        Assert.Contains("changed since the plan was shown", r.Error);
        Assert.Equal(edited, File.ReadAllBytes(Config));
    }

    [Fact]
    public void AChangeMustSeeThePartitionAtThePlansLetter()
    {
        var p = Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Settings, Options), new FakeMounter(Efi)).ToPlan(Esp);
        // the partition already has another letter now (the user mounted it): the plan named S:
        var m = new FakeMounter(Efi, "T:", hadLetter: true);
        var ignoresWish = new LetterMounter(m, "T:");
        var r = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Settings, Options, p.Letter, p.Hash), ignoresWish);
        Assert.False(r.Ok);
        Assert.Contains("not at S:", r.Error);
        Assert.Equal(0, m.Unmounts);   // it had a letter before: left mounted
    }

    sealed class LetterMounter(FakeMounter inner, string letter) : IEspMounter
    {
        public MountedEsp Mount(uint disk, uint partition, string? want) => inner.Mount(disk, partition, null) with { Letter = letter };
        public void Unmount(MountedEsp esp) => inner.Unmount(esp);
    }

    sealed class StuckMounter(FakeMounter inner) : IEspMounter
    {
        public MountedEsp Mount(uint disk, uint partition, string? want) => inner.Mount(disk, partition, want);
        public void Unmount(MountedEsp esp) => throw new EfiException("mountvol failed: in use");
    }

    [Fact]
    public void AChangeThatWasMadeStaysReportedWhenUnmountingFails()
    {
        var m = new StuckMounter(new FakeMounter(Efi));
        var p = Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Settings, Options), m);
        Assert.True(p.Ok, p.Error);
        Assert.Contains("stays mounted at S:", p.Warning);
        var done = Run(new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.Settings, Options, "S:", p.ToPlan(Esp).Hash), m);
        Assert.True(done.Ok, done.Error);
        Assert.Contains("in use", done.Warning);
        Assert.Equal("10", Plist.Raw(Plist.Load(Config).Get("Booter", "Quirks", "ResizeAppleGpuBars")));
    }

    [Fact]
    public void NotAnEspIsRefused()
    {
        var r = Run(new EfiHelperRequest(EfiHelperVerb.Plan, new EfiTarget.Partition(0, 3), EfiChange.Settings, Options), new FakeMounter(Efi));
        Assert.False(r.Ok);
        Assert.Contains("not an EFI system partition", r.Error);
        var none = EfiHelper.Run(new EfiHelperRequest(EfiHelperVerb.Plan, Esp, EfiChange.Settings, Options), null);
        Assert.False(none.Ok);
    }

    [Fact]
    public void AFolderNeedsNoMounting()
    {
        var folder = new EfiTarget.Folder(Path.Combine(Efi, "EFI"));
        var status = Run(new EfiHelperRequest(EfiHelperVerb.Status, folder, EfiChange.Settings, Options), new FakeMounter("/nowhere"));
        Assert.True(status.Ok, status.Error);
        var s = status.ToStatus();
        Assert.True(s.Readable);
        Assert.Equal("yes", s["opencore"]);
        Assert.Equal("ok", s["package_checksums"]);
        Assert.Equal("none", s["backup"]);
        Assert.Null(status.Letter);
    }

    [Fact]
    public void TheCommandLine()
    {
        var q = new EfiHelperRequest(EfiHelperVerb.Apply, Esp, EfiChange.SettingsSafari,
            new EfiOptions(@"C:\Users\x\IntelXe Manager\pkg\", @"\_SB.PC00.PEG1.PEGP", "none", "13th Gen Intel(R) Core(TM) i5-13400F", true, null),
            "s:", "ab12", @"C:\out dir\r.json");
        var back = EfiHelperRequest.Parse(q.ToArgs());
        Assert.Equal(q with { Letter = "S:" }, back);
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "apply", "--change", "settings", "--disk", "1", "--partition", "1"]));   // no --expect
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "plan", "--change", "settings"]));   // no EFI
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "plan", "--change", "settings", "--folder", "x", "--disk", "1", "--partition", "1"]));
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "plan", "--change", "nonsense", "--folder", "x"]));
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "plan", "--change", "settings", "--folder", "x", "--letter", "SS"]));
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "mount", "--folder", "x"]));
        Assert.Throws<EfiException>(() => EfiHelperRequest.Parse(["--efi-helper", "plan", "--change", "settings", "--folder", "x", "--rm", "y"]));
    }

    [Theory]
    [InlineData("plain", "plain")]
    [InlineData("", "\"\"")]
    [InlineData(@"C:\Program Files\x", "\"C:\\Program Files\\x\"")]
    [InlineData(@"E:\", @"E:\")]
    [InlineData(@"E:\a b\", "\"E:\\a b\\\\\"")]
    [InlineData("say \"hi\"", "\"say \\\"hi\\\"\"")]
    [InlineData(@"a\""b", "\"a\\\\\\\"b\"")]
    public void Quoting(string arg, string quoted) => Assert.Equal(quoted, CommandLine.Quote(arg));

    // ---- the package -------------------------------------------------------------------------------------------------
    [Fact]
    public void ThePackageFolderAndItsSums()
    {
        Assert.Equal(Pkg, EfiPackage.FindIn(Pkg));
        Assert.Equal(Pkg, EfiPackage.FindIn(root));   // the folder holding the unpacked zip's top folder
        Assert.Null(EfiPackage.FindIn(Efi));
        Assert.Equal(Pkg, EfiPackage.FindBeside(Path.Combine(Pkg, "IntelXeManager")));
        var info = EfiPackage.Inspect(Pkg);
        Assert.Equal(("ok", "9.9.9", true), (info.Checksums, info.KextVersion, info.Usable));
        File.AppendAllText(Path.Combine(Pkg, "EFI", "OC", "Kexts", "IntelXe.kext", "Contents", "MacOS", "IntelXe"), "changed\n");
        info = EfiPackage.Inspect(Pkg);
        Assert.Equal("differs from SHA256SUMS: EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe", info.Checksums);
        Assert.False(info.Usable);
        // and the change refuses it, before anything is written
        var p = EfiWork.Plan(new EfiTarget.Folder(Efi), Efi, EfiChange.Driver, Options);
        var e = Assert.Throws<EfiException>(() => EfiWork.Apply(Efi, EfiChange.Driver, p.Hash, Options));
        Assert.Contains("differ from its SHA256SUMS", e.Message);
        Assert.False(Directory.Exists(Backups));
        File.Delete(Path.Combine(Pkg, "SHA256SUMS"));
        Assert.Equal("no SHA256SUMS", EfiPackage.Inspect(Pkg).Checksums);
        Assert.NotNull(EfiPackage.Inspect(Efi).Problem);
    }

    [Fact]
    public void TheZip()
    {
        var zip = Path.Combine(root, "intel-gpu-macos-driver-9.9.9-efi.zip");
        ZipFile.CreateFromDirectory(Pkg, zip, CompressionLevel.Fastest, includeBaseDirectory: true);
        var cache = Path.Combine(root, "cache");
        var dir = EfiPackage.Unzip(zip, cache);
        Assert.StartsWith(cache, dir);
        Assert.Equal("ok", EfiPackage.Inspect(dir).Checksums);
        Assert.Equal(dir, EfiPackage.Unzip(zip, cache));   // once

        var evil = Path.Combine(root, "evil.zip");
        using (var a = ZipFile.Open(evil, ZipArchiveMode.Create))
            using (var w = new StreamWriter(a.CreateEntry("../outside.txt").Open())) w.Write("x");
        Assert.Throws<EfiException>(() => EfiPackage.Unzip(evil, cache));
        Assert.False(File.Exists(Path.Combine(cache, "outside.txt")));
        var empty = Path.Combine(root, "empty.zip");
        using (ZipFile.Open(empty, ZipArchiveMode.Create)) { }
        Assert.Throws<EfiException>(() => EfiPackage.Unzip(empty, cache));
    }
}
