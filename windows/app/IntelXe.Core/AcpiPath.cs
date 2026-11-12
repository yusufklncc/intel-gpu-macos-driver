// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;

namespace IntelXe.Core;

/// The card's ACPI path, which stage 4 needs for SSDT-ARCOFF.aml. The same conversions as IntelXe-EFI.ps1
/// (EfiFolder.ConvertLocationPath / NormalizeCard) and efi.sh, for the Overview; the EFI page's plans use the engine's.
public static class AcpiPath
{
    public const string Shipped = @"\_SB.PC00.PEG1.PEGP";

    static string Unpad(string s) { var t = s.TrimEnd('_'); return t.Length == 0 ? s : t; }

    /// Device Manager's "Location paths" line, ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000), as the ACPI
    /// path of the bridge two PCI levels above the GPU (\_SB.PC00.PEG1.PEGP); "none" when that bridge has no ACPI name;
    /// null when the line is not such a path.
    public static string? FromLocationPath(string? line)
    {
        if (line is null) return null;
        var parts = line.Trim().Split('#');
        var names = new List<string>();
        bool pci = false;
        foreach (var raw in parts)
        {
            var q = raw.Trim();
            var m = Regex.Match(q, @"^ACPI\(([A-Za-z0-9_]{1,4})\)$", RegexOptions.IgnoreCase);
            if (m.Success) { if (pci) return null; names.Add(m.Groups[1].Value.ToUpperInvariant()); continue; }
            if (Regex.IsMatch(q, @"^(PCI|PCIROOT)\([0-9A-Fa-f]+\)$", RegexOptions.IgnoreCase)) { pci = true; continue; }
            return null;
        }
        if (names.Count == 0 || parts.Length < 3) return null;
        int up = parts.Length - 3;   // the GPU is the last part, its switch's downstream port the one before
        if (up >= names.Count) return "none";
        var path = new List<string>();
        for (int i = 0; i <= up; i++) path.Add(Unpad(names[i]));
        if (path.Count < 2) return null;
        return "\\" + string.Join(".", path);
    }

    /// \_SB.PC00.PEG1.PEGP, \_SB_.PC00.PEG1.PEGP (Linux) or a Location paths line, as \_SB.PC00.PEG1.PEGP; null when it is
    /// neither.
    public static string? Normalize(string? input)
    {
        if (input is null || input.Trim().Length == 0) return null;
        var s = input.Trim();
        if (s.StartsWith("ACPI(", StringComparison.OrdinalIgnoreCase))
        {
            var r = FromLocationPath(s);
            return r == "none" ? null : r;
        }
        if (!Regex.IsMatch(s, @"^\\?[A-Za-z0-9_]{1,4}(\.[A-Za-z0-9_]{1,4})+$")) return null;
        var segs = s.TrimStart('\\').ToUpperInvariant().Split('.').Select(Unpad);
        return "\\" + string.Join(".", segs);
    }
}
