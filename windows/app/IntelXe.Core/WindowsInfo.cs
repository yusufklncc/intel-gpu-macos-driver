// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

/// Windows' version from HKLM\SOFTWARE\Microsoft\Windows NT\CurrentVersion (ProductName, EditionID, DisplayVersion,
/// CurrentBuildNumber, UBR). Nothing about the product key is read.
public sealed record WindowsInfo(string ProductName, string DisplayVersion, int Build, int Ubr)
{
    /// ProductName still says "Windows 10" on Windows 11; the build number tells them apart (22000 and later is 11).
    public string Name
    {
        get
        {
            var name = string.IsNullOrWhiteSpace(ProductName) ? "Windows" : ProductName.Trim();
            if (Build >= 22000 && name.StartsWith("Windows 10", StringComparison.OrdinalIgnoreCase)) name = "Windows 11" + name[10..];
            return name;
        }
    }

    /// "Windows 11 Pro 25H2"
    public string NameAndVersion => string.IsNullOrWhiteSpace(DisplayVersion) ? Name : Name + " " + DisplayVersion.Trim();

    /// "26200.7171"; "" when the build is not known
    public string BuildText => Build <= 0 ? "" : Ubr > 0 ? $"{Build}.{Ubr}" : $"{Build}";

    public bool IsWindows11 => Build >= 22000;
}
