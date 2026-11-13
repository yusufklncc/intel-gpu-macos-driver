// SPDX-License-Identifier: MIT
using IntelXe.Core;

namespace IntelXe.Core.Tests;

public class CardAndPciTests
{
    [Fact]
    public void SupportedCardsComeFromPlatformsJson()
    {
        var all = SupportedCards.All;
        Assert.Contains(all, c => c.Device == 0xE20B && c.Name == "Intel Arc B580" && c.Tested);
        Assert.Contains(all, c => c.Device == 0xE20C && c.Name == "Intel Arc B570" && !c.Tested);
        Assert.Contains(all, c => c.Device == 0xE211 && c.Name == "Intel Arc Pro B60" && c.Model == "Intel Arc Pro B60 Graphics" && !c.Tested);
        Assert.Contains(all, c => c.Device == 0xE212 && c.Name == "Intel Arc Pro B50" && c.Model == "Intel Arc Pro B50 Graphics" && !c.Tested);
        Assert.DoesNotContain(all, c => c.Device is >= 0xE220 and <= 0xE223);   // BMG-G31 (Pro B70): tester boot-arg only
        Assert.All(all, c => Assert.StartsWith("8086:", c.VendorDevice));
    }

    [Theory]
    [InlineData(@"PCI\VEN_8086&DEV_E20B&SUBSYS_10208086&REV_00\6&1A2B3C4D&0&00000019", 0x8086, 0xE20B, 0x8086, 0x1020, 0)]
    [InlineData(@"pci\ven_8086&dev_e20c&subsys_00001849&rev_01", 0x8086, 0xE20C, 0x1849, 0x0000, 1)]
    public void ParsesInstanceIds(string id, int vendor, int device, int subVendor, int sub, int rev)
    {
        var p = PciId.Parse(id)!;
        Assert.Equal(vendor, p.Vendor);
        Assert.Equal(device, p.Device);
        Assert.Equal(subVendor, (int)p.SubsystemVendor!);
        Assert.Equal(sub, (int)p.Subsystem!);
        Assert.Equal(rev, (int)p.Revision!);
    }

    [Fact]
    public void FormatsVendorDeviceInLowerCase() =>
        Assert.Equal("8086:e20b", PciId.Parse(@"PCI\VEN_8086&DEV_E20B")!.VendorDevice);

    [Theory]
    [InlineData(null)]
    [InlineData("")]
    [InlineData(@"ROOT\BasicDisplay\0000")]
    [InlineData(@"USB\VID_8087&PID_0033")]
    public void NonPciIsNull(string? id) => Assert.Null(PciId.Parse(id));

    [Fact]
    public void ClassifiesCards()
    {
        Assert.Equal(CardSupport.Supported, SupportedCards.Classify(new PciId(0x8086, 0xE20B)));
        Assert.Equal(CardSupport.SupportedUntested, SupportedCards.Classify(new PciId(0x8086, 0xE20C)));
        Assert.Equal(CardSupport.SupportedUntested, SupportedCards.Classify(new PciId(0x8086, 0xE211)));   // Arc Pro B60
        Assert.Equal(CardSupport.SupportedUntested, SupportedCards.Classify(new PciId(0x8086, 0xE212)));   // Arc Pro B50
        Assert.Equal(CardSupport.OtherIntel, SupportedCards.Classify(new PciId(0x8086, 0xE223)));          // Arc Pro B70 (G31)
        Assert.Equal(CardSupport.OtherIntel, SupportedCards.Classify(new PciId(0x8086, 0x56A0)));   // Arc A770
        Assert.Equal(CardSupport.NotIntel, SupportedCards.Classify(new PciId(0x10DE, 0x2684)));
        Assert.Equal(CardSupport.NotIntel, SupportedCards.Classify(null));
    }

    [Fact]
    public void IssueFormsCarryTheReportId()
    {
        Assert.Equal("https://github.com/yusufklncc/intel-gpu-macos-driver/issues/new?template=1-bug.yml&report=XR-1A2B3C4D",
            Project.BugFormUrl("XR-1A2B3C4D").AbsoluteUri);
        Assert.Equal("https://github.com/yusufklncc/intel-gpu-macos-driver/issues/new?template=2-new-hardware.yml&reports=XR-00FF00FF",
            Project.NewGpuFormUrl("XR-00FF00FF").AbsoluteUri);
        Assert.Equal("https://github.com/yusufklncc/intel-gpu-macos-driver/issues/new?template=1-bug.yml", Project.BugFormUrl(null).AbsoluteUri);
    }

    [Fact]
    public void IssueFormFieldsExistInTheTemplates()
    {
        var dir = Paths.RepoFile(".github", "ISSUE_TEMPLATE");
        Assert.Contains("id: report\n", File.ReadAllText(Path.Combine(dir, "1-bug.yml")).Replace("\r\n", "\n"));
        Assert.Contains("id: reports\n", File.ReadAllText(Path.Combine(dir, "2-new-hardware.yml")).Replace("\r\n", "\n"));
    }
}
