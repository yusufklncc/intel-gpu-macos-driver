// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;

namespace IntelXe.Core;

public enum CpuVendor { Unknown, Intel, Amd, Other }

/// What the EFI rules and the Overview need from the CPU's name (Win32_Processor.Name, e.g.
/// "13th Gen Intel(R) Core(TM) i7-13700K" or "Intel(R) Core(TM) i7-14700K"): its vendor, its family, and whether it is a
/// hybrid Alder Lake / Raptor Lake part that HybridPM.kext is for.
public sealed record CpuFamily(CpuVendor Vendor, int? Generation, string? Codename, bool CoreUltra)
{
    /// "14th gen Core (Raptor Lake Refresh)", "Core Ultra (Arrow Lake)", "AMD"; "" when nothing is known.
    public string Label
    {
        get
        {
            if (Vendor == CpuVendor.Amd) return "AMD";
            if (Vendor != CpuVendor.Intel) return "";
            string name = CoreUltra ? "Core Ultra" : Generation is int g ? Ordinal(g) + " gen Core" : "Intel";
            return Codename is null ? name : $"{name} ({Codename})";
        }
    }

    /// The label for the window, in its language: "Cpu_Gen" (14th gen Core) or Core Ultra, with the codename after it.
    public Text Describe()
    {
        if (Vendor == CpuVendor.Amd) return Text.Raw("AMD");
        if (Vendor != CpuVendor.Intel) return Text.Empty;
        Text name = CoreUltra ? Text.Raw("Core Ultra") : Generation is int g ? Text.Of("Cpu_Gen", g) : Text.Raw("Intel");
        return Codename is null ? name : Text.Of("Cpu_WithCodename", name, Codename);
    }

    static string Ordinal(int n) => n + (n % 100 is 11 or 12 or 13 ? "th" : (n % 10) switch { 1 => "st", 2 => "nd", 3 => "rd", _ => "th" });
}

public static class CpuInfo
{
    // The same rule as efi.sh, IntelXe-EFI.ps1 (EfiFolder.HybridCpu) and the macOS app (Health.swift hybridCPU):
    // i3/i5/i7/i9-12xxx..14xxx, without Core Ultra (HybridPM leaves it alone) and the desktop models with P-cores only.
    static readonly Regex HybridName = new(@"i[3579]-1[2-4]([0-9][0-9][0-9]|[0-9][0-9][UP])", RegexOptions.CultureInvariant);
    static readonly Regex PCoresOnly = new(@"Ultra|i3-1[234]100|i3-12300|i5-12400|i5-12490|i5-12500(?!H)|i5-12600(?![KH])", RegexOptions.CultureInvariant);

    /// A 12th-14th gen Core with efficiency cores, by its name: HybridPM.kext applies to it.
    public static bool IsHybrid(string? name) =>
        !string.IsNullOrEmpty(name) && HybridName.IsMatch(name) && !PCoresOnly.IsMatch(name);

    /// The name without the trademark signs, the "CPU @ 3.60GHz" tail and the "13th Gen" prefix Windows adds:
    /// "Intel Core i7-13700K".
    public static string CleanName(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return "";
        var s = Regex.Replace(name, @"\((R|TM|tm|r)\)|®|™", "");
        s = Regex.Replace(s, @"\s+CPU\b", " ");
        s = Regex.Replace(s, @"\s*@\s*[0-9.]+\s*GHz", "", RegexOptions.IgnoreCase);
        s = Regex.Replace(s, @"^\s*\d{1,2}th Gen\s+", "", RegexOptions.IgnoreCase);
        s = Regex.Replace(s, @"(\s+\d+-Core)?(\s+Processor)?\s*$", "", RegexOptions.IgnoreCase);
        s = Regex.Replace(s, @"\s{2,}", " ").Trim();
        return s;
    }

    public static CpuVendor Vendor(string? name)
    {
        if (string.IsNullOrWhiteSpace(name)) return CpuVendor.Unknown;
        if (Regex.IsMatch(name, @"\bAMD\b|\bRyzen\b|\bEPYC\b|\bThreadripper\b|\bAthlon\b", RegexOptions.IgnoreCase)) return CpuVendor.Amd;
        if (Regex.IsMatch(name, @"\bIntel\b|Core\(TM\)|\bCore\b|\bXeon\b|\bPentium\b|\bCeleron\b", RegexOptions.IgnoreCase)) return CpuVendor.Intel;
        return CpuVendor.Other;
    }

    /// The family by the model number: Core iN-GGxxx (10th gen on), Core iN-Gxxx (before) or Core Ultra N SSS.
    public static CpuFamily Family(string? name)
    {
        var vendor = Vendor(name);
        if (vendor != CpuVendor.Intel || name is null) return new CpuFamily(vendor, null, null, false);
        var ultra = Regex.Match(name, @"Ultra\s*[3579]\s+(\d)(\d)(\d)([A-Z]*)", RegexOptions.CultureInvariant);
        if (ultra.Success)
        {
            int series = ultra.Groups[1].Value[0] - '0';
            string suffix = ultra.Groups[4].Value;
            string? code = series switch
            {
                1 => "Meteor Lake",
                2 => suffix.StartsWith('V') ? "Lunar Lake" : "Arrow Lake",
                3 => "Panther Lake",
                _ => null,
            };
            return new CpuFamily(vendor, null, code, true);
        }
        var core = Regex.Match(name, @"i[3579]-(\d{4,5})", RegexOptions.CultureInvariant);
        if (!core.Success) return new CpuFamily(vendor, null, null, false);
        // 10th gen and later start with 1 and their generation (i7-10700, i7-1165G7, i5-1335U, i7-14700K); before them
        // one digit (i7-9700K, i7-8750H)
        string digits = core.Groups[1].Value;
        int gen = digits[0] == '1' ? int.Parse(digits[..2]) : digits[0] - '0';
        string? codename = gen switch
        {
            14 => "Raptor Lake Refresh",
            13 => "Raptor Lake",
            12 => "Alder Lake",
            11 => "Rocket Lake / Tiger Lake",
            10 => "Comet Lake",
            9 or 8 => "Coffee Lake",
            7 => "Kaby Lake",
            6 => "Skylake",
            _ => null,
        };
        return new CpuFamily(vendor, gen, codename, false);
    }
}
