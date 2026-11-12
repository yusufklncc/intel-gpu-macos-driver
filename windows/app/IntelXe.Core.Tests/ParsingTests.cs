// SPDX-License-Identifier: MIT
using IntelXe.Core;
using IntelXe.Core.Report;

namespace IntelXe.Core.Tests;

public class ParsingTests
{
    [Theory]
    [InlineData("ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)", @"\_SB.PC00.PEG1.PEGP")]
    [InlineData("ACPI(_SB_)#ACPI(PC00)#ACPI(RP05)#PCI(0000)#PCI(0000)", @"\_SB.PC00.RP05")]
    [InlineData("ACPI(_SB_)#ACPI(PC00)#PCI(0100)#PCI(0000)#PCI(0000)", "none")]
    [InlineData("PCIROOT(0)#PCI(0100)#PCI(0000)", null)]
    [InlineData("ACPI(_SB_)#PCI(0100)#ACPI(PEGP)", null)]
    [InlineData("not a path", null)]
    public void ConvertsLocationPaths(string line, string? path) => Assert.Equal(path, AcpiPath.FromLocationPath(line));

    [Theory]
    [InlineData(@"\_SB.PC00.PEG1.PEGP", @"\_SB.PC00.PEG1.PEGP")]
    [InlineData(@"\_SB_.PC00.PEG1.PEGP", @"\_SB.PC00.PEG1.PEGP")]
    [InlineData(@"_sb.pc00.peg1.pegp", @"\_SB.PC00.PEG1.PEGP")]
    [InlineData("ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)", @"\_SB.PC00.PEG1.PEGP")]
    [InlineData("PEGP", null)]
    [InlineData("", null)]
    public void NormalizesAcpiPaths(string input, string? path) => Assert.Equal(path, AcpiPath.Normalize(input));

    /// A REG_RESOURCE_LIST with one full descriptor: a 16 MB memory range below 4 GB, a port range, and a 16 GB large
    /// memory range at 0x40_0000_0000 (length stored shifted by 16 bits, flag 0x400), as a B580 with Resizable BAR has.
    static byte[] ResourceListBytes(ulong largeLength, ushort largeFlag)
    {
        var b = new List<byte>();
        b.AddRange(BitConverter.GetBytes(1u));                 // full descriptors
        b.AddRange(BitConverter.GetBytes(5u));                 // interface type: PCI
        b.AddRange(BitConverter.GetBytes(3u));                 // bus number
        b.AddRange(BitConverter.GetBytes((ushort)1));          // version
        b.AddRange(BitConverter.GetBytes((ushort)1));          // revision
        b.AddRange(BitConverter.GetBytes(3u));                 // partial descriptors
        void Descriptor(byte type, ushort flags, ulong start, uint length)
        {
            b.Add(type); b.Add(1); b.AddRange(BitConverter.GetBytes(flags));
            b.AddRange(BitConverter.GetBytes(start)); b.AddRange(BitConverter.GetBytes(length));
            b.AddRange(new byte[4]);                            // the rest of the 16-byte union
        }
        Descriptor(3, 0, 0x8200_0000, 16u << 20);
        Descriptor(1, 0, 0x4000, 0x100);
        Descriptor(7, largeFlag, 0x40_0000_0000, (uint)largeLength);
        return b.ToArray();
    }

    [Fact]
    public void ReadsResizedBars()
    {
        var bar = ResourceList.Summarize(ResourceListBytes((16UL << 30) >> 16, 0x400))!;
        Assert.Equal(2, bar.Ranges.Count);
        Assert.Equal(16384UL, bar.LargestMb);
        Assert.True(bar.ResizableBar);
        Assert.True(bar.Above4G);
    }

    [Fact]
    public void ReadsSmallBars()
    {
        var bar = ResourceList.Summarize(ResourceListBytes((256UL << 20) >> 8, 0x200))!;
        Assert.Equal(256UL, bar.LargestMb);
        Assert.False(bar.ResizableBar);
    }

    [Fact]
    public void EmptyResourceListIsUnknown()
    {
        Assert.Null(ResourceList.Summarize(null));
        Assert.Null(ResourceList.Summarize([1, 2]));
    }

    [Theory]
    [InlineData("Windows 10 Pro", "25H2", 26200, 7171, "Windows 11 Pro 25H2", "26200.7171")]
    [InlineData("Windows 10 Home", "22H2", 19045, 0, "Windows 10 Home 22H2", "19045")]
    [InlineData("", "", 0, 0, "Windows", "")]
    public void NamesWindows(string product, string display, int build, int ubr, string name, string buildText)
    {
        var w = new WindowsInfo(product, display, build, ubr);
        Assert.Equal(name, w.NameAndVersion);
        Assert.Equal(buildText, w.BuildText);
    }

    [Theory]
    [InlineData(1, BitLockerState.On)]
    [InlineData(2, BitLockerState.Off)]
    [InlineData(3, BitLockerState.Encrypting)]
    [InlineData(6, BitLockerState.Locked)]
    [InlineData(8, BitLockerState.WaitingForActivation)]
    [InlineData(0, BitLockerState.Unknown)]
    [InlineData(null, BitLockerState.Unknown)]
    public void ReadsBitLockerShellState(int? value, BitLockerState state) => Assert.Equal(state, BitLocker.FromShellProperty(value));

    [Theory]
    [InlineData(104_857_600UL, "105 MB")]
    [InlineData(209_715_200UL, "210 MB")]
    [InlineData(32_000_000_000UL, "32 GB")]
    public void FormatsSizesAsTheEfiTool(ulong bytes, string text) => Assert.Equal(text, EspPartition.FormatSize(bytes));

    [Fact]
    public void RecognisesTheEspType()
    {
        Assert.True(EspPartition.IsEspType("{C12A7328-F81F-11D2-BA4B-00A0C93EC93B}"));
        Assert.False(EspPartition.IsEspType("{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}"));
        Assert.Equal(OpenCoreState.Found, EspPartition.Detect(p => p == @"EFI\OC\OpenCore.efi"));
        Assert.Equal(OpenCoreState.NotFound, EspPartition.Detect(_ => false));
    }

    [Fact]
    public void ParsesTheReportScriptsEnd()
    {
        var r = ReportRun.Parse(["Intel GPU: collecting...", "", "Report ID: XR-1A2B3C4D", @"File:      C:\Users\x\Desktop\xe-report-XR-1A2B3C4D.zip", "Took 52 s."])!;
        Assert.Equal("XR-1A2B3C4D", r.Id);
        Assert.Equal(@"C:\Users\x\Desktop\xe-report-XR-1A2B3C4D.zip", r.File);
        Assert.Null(ReportRun.Parse(["No report was written."]));
        Assert.Null(ReportRun.Parse(["Report ID: nonsense", "File: x"]));
    }

    [Fact]
    public void RunsTheReportWithBootLogsAsAdministrator()
    {
        var plain = ReportRun.PowerShellArguments(@"C:\app\tools\collect-windows.ps1", @"C:\Users\x\Desktop", acpi: false, bootLogs: true);
        Assert.Equal("-BootLogs", plain[^1]);
        var args = ReportRun.ElevatedArguments(@"C:\app's\tools\collect-windows.ps1", @"C:\Users\x\Desktop", acpi: true, bootLogs: true, @"C:\Users\x\AppData\Local\IntelXe Manager\report\1.txt");
        Assert.Equal(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command",
            @"& 'C:\app''s\tools\collect-windows.ps1' -OutDir 'C:\Users\x\Desktop' -Acpi -BootLogs *>&1 | Out-File -FilePath 'C:\Users\x\AppData\Local\IntelXe Manager\report\1.txt' -Encoding UTF8"], args);
        // "runas" takes one string (ShellExecute): the command is one quoted word in it
        Assert.EndsWith(" -Command \"" + args[^1] + "\"", IntelXe.Core.Efi.CommandLine.Join(args));
    }

    [Fact]
    public void RunsTheReportWithItsOptions()
    {
        var args = ReportRun.PowerShellArguments(@"C:\app\tools\collect-windows.ps1", @"C:\Users\x\Desktop", acpi: true);
        Assert.Equal(["-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-File", @"C:\app\tools\collect-windows.ps1", "-OutDir", @"C:\Users\x\Desktop", "-Acpi"], args);
        // the script's parameters (tools/report/collect-windows.ps1)
        var script = File.ReadAllText(Paths.RepoFile("tools", "report", "collect-windows.ps1"));
        Assert.Contains("param([string]$OutDir", script);
        Assert.Contains("[switch]$Acpi", script);
        Assert.Contains("[switch]$BootLogs", script);
        Assert.Contains("Write-Host \"Report ID: $ID\"", script);
        Assert.Contains("Write-Host \"File:      $zip\"", script);
    }
}
