// SPDX-License-Identifier: MIT
using System.Globalization;
using System.Text.RegularExpressions;

namespace IntelXe.Core;

/// A PCI function's IDs as Windows names the device: PCI\VEN_8086&DEV_E20B&SUBSYS_10208086&REV_00\...
public sealed record PciId(ushort Vendor, ushort Device, ushort? SubsystemVendor = null, ushort? Subsystem = null, byte? Revision = null)
{
    public const ushort Intel = 0x8086;

    static readonly Regex Pattern = new(@"^PCI\\VEN_([0-9A-F]{4})&DEV_([0-9A-F]{4})(?:&SUBSYS_([0-9A-F]{8}))?(?:&REV_([0-9A-F]{2}))?",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    /// The IDs in a PnP instance or hardware ID; null when it is not a PCI device.
    public static PciId? Parse(string? instanceId)
    {
        if (string.IsNullOrWhiteSpace(instanceId)) return null;
        var m = Pattern.Match(instanceId.Trim());
        if (!m.Success) return null;
        ushort Hex16(string s) => ushort.Parse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
        ushort? subVendor = null, sub = null;
        if (m.Groups[3].Success)
        {
            // SUBSYS_ is the subsystem ID followed by the subsystem vendor ID
            sub = Hex16(m.Groups[3].Value[..4]);
            subVendor = Hex16(m.Groups[3].Value[4..]);
        }
        byte? rev = m.Groups[4].Success ? byte.Parse(m.Groups[4].Value, NumberStyles.HexNumber, CultureInfo.InvariantCulture) : null;
        return new PciId(Hex16(m.Groups[1].Value), Hex16(m.Groups[2].Value), subVendor, sub, rev);
    }

    /// vendor:device in lower case, as the guide's tables write it (8086:e20b).
    public string VendorDevice => $"{Vendor:x4}:{Device:x4}";
    public override string ToString() => VendorDevice;
}
