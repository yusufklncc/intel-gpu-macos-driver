// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

public sealed record MemoryRange(ulong Start, ulong Size)
{
    public bool Above4G => Start >= 1UL << 32;
}

/// The card's BARs as Windows assigned them, and what they say about Resizable BAR and Above 4G decoding.
public sealed record BarSummary(IReadOnlyList<MemoryRange> Ranges)
{
    const ulong Mb = 1UL << 20;
    public ulong Largest => Ranges.Count == 0 ? 0 : Ranges.Max(r => r.Size);
    public ulong LargestMb => Largest / Mb;
    /// One BAR above 256 MB: the firmware resized it (Resizable BAR on).
    public bool ResizableBar => Largest > 256 * Mb;
    /// A range above 4 GB: Above 4G decoding is on.
    public bool Above4G => Ranges.Any(r => r.Above4G);
}

/// A REG_RESOURCE_LIST as Windows keeps it in Enum\<device>\Control\AllocConfig, read the same way as
/// tools/report/collect-windows.ps1 (Get-AllocRanges): WMI's Win32_DeviceMemoryAddress lists only ranges below 4 GB, so it
/// never shows a resized VRAM BAR.
public static class ResourceList
{
    /// The memory ranges in it (CM_PARTIAL_RESOURCE_DESCRIPTORs of type 3, memory, and 7, large memory). Descriptors are
    /// 20 bytes on x64; a large range's length is shifted by 8, 16 or 32 bits (flags 0x200/0x400/0x800).
    public static IReadOnlyList<MemoryRange> MemoryRanges(byte[]? v)
    {
        var list = new List<MemoryRange>();
        if (v is null || v.Length < 4) return list;
        uint full = BitConverter.ToUInt32(v, 0);
        int o = 4;
        for (uint f = 0; f < full && o + 16 <= v.Length; f++)
        {
            o += 8;   // interface type, bus number
            uint count = BitConverter.ToUInt32(v, o + 4);   // version, revision (2 x 16 bits), count
            o += 8;
            for (uint i = 0; i < count && o + 20 <= v.Length; i++)
            {
                byte type = v[o];
                ushort flags = BitConverter.ToUInt16(v, o + 2);
                if (type is 3 or 7)
                {
                    ulong start = BitConverter.ToUInt64(v, o + 4);
                    ulong length = BitConverter.ToUInt32(v, o + 12);
                    if (type == 7)
                    {
                        if ((flags & 0x200) != 0) length <<= 8;
                        else if ((flags & 0x400) != 0) length <<= 16;
                        else if ((flags & 0x800) != 0) length <<= 32;
                    }
                    list.Add(new MemoryRange(start, length));
                }
                o += 20;
            }
        }
        return list;
    }

    public static BarSummary? Summarize(byte[]? v)
    {
        var r = MemoryRanges(v);
        return r.Count == 0 ? null : new BarSummary(r);
    }
}
