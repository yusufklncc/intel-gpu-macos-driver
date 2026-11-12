// SPDX-License-Identifier: MIT
using IntelXe.Core;

namespace IntelXe.Core.Tests;

public class CpuTests
{
    // the CPUs windows/test/make-expected.sh and release/test-efi-flow.sh run efi.sh with, and what the rule says of them
    [Theory]
    [InlineData("Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz", false)]
    [InlineData("Intel(R) Core(TM) i7-14700K", true)]
    [InlineData("12th Gen Intel(R) Core(TM) i9-12900K", true)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12400F", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12500", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12500H", true)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12600", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12600K", true)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12600H", true)]
    [InlineData("12th Gen Intel(R) Core(TM) i5-12490F", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i3-12100F", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i3-12300", false)]
    [InlineData("13th Gen Intel(R) Core(TM) i3-13100", false)]
    [InlineData("13th Gen Intel(R) Core(TM) i5-13400F", true)]
    [InlineData("13th Gen Intel(R) Core(TM) i7-13620H", true)]
    [InlineData("14th Gen Intel(R) Core(TM) i9-14900K", true)]
    [InlineData("14th Gen Intel(R) Core(TM) i5-14400F", true)]
    [InlineData("Intel(R) Core(TM) i3-14100", false)]
    [InlineData("Intel(R) Core(TM) Ultra 7 155H", false)]
    [InlineData("12th Gen Intel(R) Core(TM) i7-1255U", true)]
    [InlineData("13th Gen Intel(R) Core(TM) i7-1360P", true)]
    [InlineData("AMD Ryzen 7 7800X3D 8-Core Processor", false)]
    [InlineData("", false)]
    public void HybridRuleMatchesEfiSh(string name, bool hybrid) => Assert.Equal(hybrid, CpuInfo.IsHybrid(name));

    [Theory]
    [InlineData("13th Gen Intel(R) Core(TM) i7-13700K", "Intel Core i7-13700K")]
    [InlineData("Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz", "Intel Core i9-9900K")]
    [InlineData("Intel(R) Core(TM) Ultra 7 155H", "Intel Core Ultra 7 155H")]
    [InlineData("AMD Ryzen 7 7800X3D 8-Core Processor", "AMD Ryzen 7 7800X3D")]
    [InlineData("  ", "")]
    public void CleansNames(string name, string clean) => Assert.Equal(clean, CpuInfo.CleanName(name));

    [Theory]
    [InlineData("Intel(R) Core(TM) i7-14700K", CpuVendor.Intel, 14, "14th gen Core (Raptor Lake Refresh)")]
    [InlineData("13th Gen Intel(R) Core(TM) i5-13400F", CpuVendor.Intel, 13, "13th gen Core (Raptor Lake)")]
    [InlineData("12th Gen Intel(R) Core(TM) i7-1255U", CpuVendor.Intel, 12, "12th gen Core (Alder Lake)")]
    [InlineData("11th Gen Intel(R) Core(TM) i7-1165G7 @ 2.80GHz", CpuVendor.Intel, 11, "11th gen Core (Rocket Lake / Tiger Lake)")]
    [InlineData("Intel(R) Core(TM) i7-10700K CPU @ 3.80GHz", CpuVendor.Intel, 10, "10th gen Core (Comet Lake)")]
    [InlineData("Intel(R) Core(TM) i9-9900K CPU @ 3.60GHz", CpuVendor.Intel, 9, "9th gen Core (Coffee Lake)")]
    [InlineData("AMD Ryzen 9 7950X 16-Core Processor", CpuVendor.Amd, null, "AMD")]
    public void NamesTheFamily(string name, CpuVendor vendor, int? gen, string label)
    {
        var f = CpuInfo.Family(name);
        Assert.Equal(vendor, f.Vendor);
        Assert.Equal(gen, f.Generation);
        Assert.Equal(label, f.Label);
    }

    [Fact]
    public void DescribesTheFamilyForTheWindow()
    {
        var t = CpuInfo.Family("Intel(R) Core(TM) i7-14700K").Describe();
        Assert.Equal("Cpu_WithCodename", t.Key);
        Assert.Equal("Cpu_Gen", ((Text)t.Args[0]).Key);
        Assert.Equal(14, ((Text)t.Args[0]).Args[0]);
        Assert.Equal("Raptor Lake Refresh", t.Args[1]);
        Assert.True(CpuInfo.Family("").Describe().IsEmpty);
    }

    [Theory]
    [InlineData("Intel(R) Core(TM) Ultra 7 155H", "Core Ultra (Meteor Lake)")]
    [InlineData("Intel(R) Core(TM) Ultra 9 285K", "Core Ultra (Arrow Lake)")]
    [InlineData("Intel(R) Core(TM) Ultra 7 258V", "Core Ultra (Lunar Lake)")]
    public void NamesCoreUltra(string name, string label)
    {
        var f = CpuInfo.Family(name);
        Assert.True(f.CoreUltra);
        Assert.Equal(label, f.Label);
    }
}
