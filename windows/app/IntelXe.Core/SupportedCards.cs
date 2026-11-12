// SPDX-License-Identifier: MIT
using System.Reflection;
using System.Text.Json;

namespace IntelXe.Core;

/// A card the driver attaches to, from data/platforms.json "supported".
public sealed record SupportedCard(ushort Device, string Name, string Model, bool Tested)
{
    public string VendorDevice => $"8086:{Device:x4}";
}

/// What a GPU in this PC is to the driver.
public enum CardSupport { Supported, SupportedUntested, OtherIntel, NotIntel }

/// The supported cards, read from data/platforms.json (embedded in this assembly at build time), the one list that every
/// copy in the repository is checked against (tools/parity/devices.py).
public static class SupportedCards
{
    static IReadOnlyList<SupportedCard>? all;

    public static IReadOnlyList<SupportedCard> All => all ??= Load();

    static IReadOnlyList<SupportedCard> Load()
    {
        using var stream = Assembly.GetExecutingAssembly().GetManifestResourceStream("IntelXe.Core.platforms.json")
            ?? throw new InvalidOperationException("platforms.json is not embedded in IntelXe.Core");
        return Parse(stream);
    }

    /// The "supported" devices of a platforms.json.
    public static IReadOnlyList<SupportedCard> Parse(Stream json)
    {
        using var doc = JsonDocument.Parse(json);
        var list = new List<SupportedCard>();
        foreach (var d in doc.RootElement.GetProperty("supported").GetProperty("devices").EnumerateArray())
        {
            var id = d.GetProperty("id").GetString() ?? "";
            list.Add(new SupportedCard(Convert.ToUInt16(id, 16), "Intel Arc " + d.GetProperty("name").GetString(),
                d.GetProperty("model").GetString() ?? "", d.TryGetProperty("tested", out var t) && t.GetBoolean()));
        }
        return list;
    }

    public static SupportedCard? Find(PciId? id) =>
        id is { Vendor: PciId.Intel } ? All.FirstOrDefault(c => c.Device == id.Device) : null;

    public static CardSupport Classify(PciId? id)
    {
        if (id is null || id.Vendor != PciId.Intel) return CardSupport.NotIntel;
        var card = Find(id);
        if (card is null) return CardSupport.OtherIntel;
        return card.Tested ? CardSupport.Supported : CardSupport.SupportedUntested;
    }
}
