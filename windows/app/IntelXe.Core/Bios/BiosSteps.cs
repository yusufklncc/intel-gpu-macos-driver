// SPDX-License-Identifier: MIT
// The BIOS page's data and judgement: bios-steps.json (embedded at build time) lists the Windows traps and firmware
// settings macOS with this driver needs, each with its sources, its menu path per board maker and the fact that tells
// whether it is done. Evaluate() takes what the Overview read from Windows (PcFacts) and gives each item a state: done,
// to do, or cannot tell (Windows does not see most firmware settings). Nothing here changes a setting.
using System.Globalization;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IntelXe.Core.Bios;

public enum BiosState { ToDo, Done, CannotTell }

public enum BiosLevel { Required, Recommended, Optional }

/// A text in English and Turkish ("en", "tr"); English when the other is missing.
public sealed class LocalText
{
    [JsonPropertyName("en")] public string En { get; set; } = "";
    [JsonPropertyName("tr")] public string Tr { get; set; } = "";

    public string In(string lang) => lang == "tr" && Tr.Length > 0 ? Tr : En;
}

/// A menu path: the same in both languages (the firmware's own menu names), or a text in each.
[JsonConverter(typeof(MenuPathConverter))]
public sealed class MenuPath
{
    public string? Same { get; init; }
    public LocalText? Text { get; init; }
    public string In(string lang) => Same ?? Text?.In(lang) ?? "";
}

sealed class MenuPathConverter : JsonConverter<MenuPath>
{
    public override MenuPath Read(ref Utf8JsonReader reader, Type typeToConvert, JsonSerializerOptions options) =>
        reader.TokenType == JsonTokenType.String
            ? new MenuPath { Same = reader.GetString() }
            : new MenuPath { Text = JsonSerializer.Deserialize<LocalText>(ref reader, options) };

    public override void Write(Utf8JsonWriter writer, MenuPath value, JsonSerializerOptions options)
    {
        if (value.Same is not null) writer.WriteStringValue(value.Same);
        else JsonSerializer.Serialize(writer, value.Text, options);
    }
}

public sealed class BiosSource
{
    [JsonPropertyName("url")] public string Url { get; set; } = "";
    [JsonPropertyName("title")] public string Title { get; set; } = "";
    /// the link's text on the page ("Guide: hardware", "Dortania: BIOS settings")
    [JsonPropertyName("short")] public LocalText Short { get; set; } = new();
    /// the repository file the source is (for the tests: it must exist, with its anchor)
    [JsonPropertyName("file")] public string? File { get; set; }
    [JsonPropertyName("anchor")] public string? Anchor { get; set; }
}

public sealed class BiosVendor
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    /// upper-case prefixes of Win32_BaseBoard.Manufacturer ("ASUSTeK COMPUTER INC.", "Micro-Star International Co., Ltd.")
    [JsonPropertyName("match")] public List<string> Match { get; set; } = [];
    [JsonPropertyName("setup_key")] public string SetupKey { get; set; } = "";
    [JsonPropertyName("boot_menu_key")] public string BootMenuKey { get; set; } = "";
    [JsonPropertyName("advanced_key")] public string AdvancedKey { get; set; } = "";
    [JsonPropertyName("save_key")] public string SaveKey { get; set; } = "";
    [JsonPropertyName("note")] public LocalText Note { get; set; } = new();
}

public sealed class BiosGroup
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("title")] public LocalText Title { get; set; } = new();
    [JsonPropertyName("caption")] public LocalText Caption { get; set; } = new();
}

public sealed class BiosItem
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("group")] public string Group { get; set; } = "";
    [JsonPropertyName("level")] public string LevelName { get; set; } = "";
    [JsonPropertyName("fact")] public string Fact { get; set; } = "none";
    /// "intel": only for Intel CPUs (hidden on an AMD CPU)
    [JsonPropertyName("cpu")] public string? Cpu { get; set; }
    [JsonPropertyName("sources")] public List<string> Sources { get; set; } = [];
    [JsonPropertyName("title")] public LocalText Title { get; set; } = new();
    [JsonPropertyName("want")] public LocalText Want { get; set; } = new();
    [JsonPropertyName("why")] public LocalText Why { get; set; } = new();
    [JsonPropertyName("missing")] public LocalText? Missing { get; set; }
    /// vendor id -> path; "generic" for the others
    [JsonPropertyName("paths")] public Dictionary<string, MenuPath> Paths { get; set; } = [];

    public BiosLevel Level => LevelName switch
    {
        "required" => BiosLevel.Required,
        "recommended" => BiosLevel.Recommended,
        "optional" => BiosLevel.Optional,
        _ => throw new InvalidDataException($"bios-steps.json: item {Id} has no level required|recommended|optional"),
    };

    /// The path on this vendor's firmware, else the generic hint.
    public string PathFor(string vendor, string lang) =>
        (Paths.TryGetValue(vendor, out var p) ? p : Paths.TryGetValue("generic", out var g) ? g : null)?.In(lang) ?? "";
}

public sealed class BiosData
{
    [JsonPropertyName("about")] public string About { get; set; } = "";
    [JsonPropertyName("sources")] public Dictionary<string, BiosSource> Sources { get; set; } = [];
    [JsonPropertyName("vendors")] public List<BiosVendor> Vendors { get; set; } = [];
    [JsonPropertyName("groups")] public List<BiosGroup> Groups { get; set; } = [];
    [JsonPropertyName("seen")] public Dictionary<string, LocalText> Seen { get; set; } = [];
    [JsonPropertyName("page")] public Dictionary<string, LocalText> Page { get; set; } = [];
    [JsonPropertyName("items")] public List<BiosItem> Items { get; set; } = [];

    public string PageText(string key, string lang) => Page.TryGetValue(key, out var t) ? t.In(lang) : key;
}

/// What Windows shows about one item: a key of "seen" and its arguments.
public sealed record BiosSeen(string Key, params object[] Args)
{
    public string In(BiosData data, string lang) =>
        data.Seen.TryGetValue(Key, out var t) ? string.Format(CultureInfo.InvariantCulture, t.In(lang), Args) : Key;
}

/// One item for this PC: the item, its state, what Windows shows, and its path on this board's firmware.
public sealed record BiosStep(BiosItem Item, BiosState State, BiosSeen Seen);

public static class BiosSteps
{
    /// The facts the items can name; "none" for a setting Windows cannot see.
    public static readonly IReadOnlyList<string> Facts =
    [
        "none", "bitlocker", "windows-uefi", "storage-ahci", "secure-boot", "csm", "above4g", "rebar", "igpu", "vtd", "vtx",
        "hyperthreading", "nx",
    ];

    public static readonly IReadOnlyList<string> VendorIds = ["asus", "msi", "gigabyte", "asrock", "generic"];

    static BiosData? embedded;

    /// bios-steps.json as built into the Core.
    public static BiosData Embedded => embedded ??= Load(typeof(BiosSteps).Assembly.GetManifestResourceStream("IntelXe.Core.bios-steps.json")
        ?? throw new InvalidDataException("bios-steps.json is not built into the Core"));

    public static BiosData Load(Stream json) =>
        JsonSerializer.Deserialize<BiosData>(json) ?? throw new InvalidDataException("bios-steps.json is empty");

    /// "en" or "tr" for a UI language ("tr-TR" and the like).
    public static string Lang(string? uiLanguage) => uiLanguage?.StartsWith("tr", StringComparison.OrdinalIgnoreCase) == true ? "tr" : "en";

    /// The board maker's vendor id by Win32_BaseBoard.Manufacturer: asus, msi, gigabyte, asrock, or generic.
    public static string VendorOf(BiosData data, string? maker)
    {
        var m = (maker ?? "").Trim().ToUpperInvariant();
        if (m.Length == 0) return "generic";
        foreach (var v in data.Vendors)
            if (v.Match.Any(x => m.StartsWith(x, StringComparison.Ordinal))) return v.Id;
        return "generic";
    }

    public static BiosVendor Vendor(BiosData data, string id) =>
        data.Vendors.FirstOrDefault(v => v.Id == id) ?? data.Vendors.First(v => v.Id == "generic");

    /// The Dortania BIOS page for this CPU: the Coffee Lake page for 8th and 9th gen, else the Comet Lake page (the
    /// guide uses it for 10th to 14th gen, which Dortania has no own desktop page for).
    public static string SourceFor(string sourceId, PcFacts? f) =>
        sourceId == "dortania-bios" && f is not null && CpuInfo.Family(f.CpuName).Generation is 8 or 9 ? "dortania-bios-coffee" : sourceId;

    /// Every item that applies to this PC, in the file's order (the Windows traps first), with its state.
    public static IReadOnlyList<BiosStep> Evaluate(BiosData data, PcFacts f)
    {
        bool amd = CpuInfo.Vendor(f.CpuName) == CpuVendor.Amd;
        var list = new List<BiosStep>();
        foreach (var group in data.Groups)
            foreach (var item in data.Items.Where(i => i.Group == group.Id))
            {
                if (item.Cpu == "intel" && amd) continue;
                var (state, seen) = Judge(item.Fact, f);
                list.Add(new BiosStep(item, state, seen));
            }
        return list;
    }

    static (BiosState, BiosSeen) Judge(string fact, PcFacts f)
    {
        switch (fact)
        {
            case "bitlocker":
                return f.BitLocker switch
                {
                    BitLockerState.Off => (BiosState.Done, new BiosSeen("bitlocker-off")),
                    BitLockerState.WaitingForActivation => (BiosState.Done, new BiosSeen("bitlocker-waiting")),
                    BitLockerState.Unknown => (BiosState.CannotTell, new BiosSeen("bitlocker-unknown")),
                    // suspended protection reads as Paused only while encryption is paused; On stays "to do": the
                    // app cannot tell a suspended protector from Explorer's value
                    _ => (BiosState.ToDo, new BiosSeen("bitlocker-on")),
                };
            case "windows-uefi":
                return f.Firmware switch
                {
                    FirmwareMode.Uefi => (BiosState.Done, new BiosSeen("uefi")),
                    FirmwareMode.Legacy => (BiosState.ToDo, new BiosSeen("legacy")),
                    _ => (BiosState.CannotTell, new BiosSeen("firmware-unknown")),
                };
            case "storage-ahci":
                if (f.StorageControllers is null) return (BiosState.CannotTell, new BiosSeen("storage-unknown"));
                var raid = f.StorageControllers.Where(IsRaidController).ToList();
                return raid.Count > 0
                    ? (BiosState.ToDo, new BiosSeen("storage-raid", string.Join(", ", raid)))
                    : (BiosState.Done, new BiosSeen("storage-ahci", string.Join(", ", f.StorageControllers.Distinct())));
            case "secure-boot":
                return f.SecureBoot switch
                {
                    SecureBootState.Off => (BiosState.Done, new BiosSeen("secureboot-off")),
                    SecureBootState.On => (BiosState.ToDo, new BiosSeen("secureboot-on")),
                    SecureBootState.NotSupported => (BiosState.CannotTell, new BiosSeen("secureboot-legacy")),
                    _ => (BiosState.CannotTell, new BiosSeen("secureboot-unknown")),
                };
            case "csm":
                // Windows cannot read CSM; but legacy mode needs it on, and Secure Boot only runs with it off
                if (f.Firmware == FirmwareMode.Legacy) return (BiosState.ToDo, new BiosSeen("csm-legacy"));
                if (f.Firmware == FirmwareMode.Uefi && f.SecureBoot == SecureBootState.On) return (BiosState.Done, new BiosSeen("csm-off"));
                return f.Firmware == FirmwareMode.Uefi ? (BiosState.CannotTell, new BiosSeen("csm-unknown")) : (BiosState.CannotTell, new BiosSeen("firmware-unknown"));
            case "above4g":
                if (f.Bar is null) return (BiosState.CannotTell, new BiosSeen("bar-unknown"));
                return f.Bar.Above4G ? (BiosState.Done, new BiosSeen("above4g-on")) : (BiosState.ToDo, new BiosSeen("above4g-off"));
            case "rebar":
                if (f.Bar is null) return (BiosState.CannotTell, new BiosSeen("bar-unknown"));
                return f.Bar.ResizableBar ? (BiosState.Done, new BiosSeen("rebar-on", f.Bar.LargestMb)) : (BiosState.ToDo, new BiosSeen("rebar-off", f.Bar.LargestMb));
            case "igpu":
                var igpu = f.IntegratedGpus.Select(g => g.Name).ToList();
                return igpu.Count > 0 ? (BiosState.ToDo, new BiosSeen("igpu-on", string.Join(", ", igpu))) : (BiosState.Done, new BiosSeen("igpu-off"));
            case "vtd":
                return f.DmarTable switch
                {
                    true => (BiosState.ToDo, new BiosSeen("vtd-on")),
                    false => (BiosState.Done, new BiosSeen("vtd-off")),
                    null => (BiosState.CannotTell, new BiosSeen("vtd-unknown")),
                };
            case "vtx":
                // false is not "off": Windows reports false while Hyper-V or VBS runs
                return f.VirtualizationFirmwareEnabled == true ? (BiosState.Done, new BiosSeen("vtx-on")) : (BiosState.CannotTell, new BiosSeen("vtx-unknown"));
            case "hyperthreading":
                if (f.Cores is not int c || f.Threads is not int t || c <= 0) return (BiosState.CannotTell, new BiosSeen("ht-notread"));
                return t > c ? (BiosState.Done, new BiosSeen("ht-on", c, t)) : (BiosState.CannotTell, new BiosSeen("ht-unknown", c));
            case "nx":
                return f.Windows is null ? (BiosState.CannotTell, new BiosSeen("none")) : (BiosState.Done, new BiosSeen("nx"));
            case "none":
                return (BiosState.CannotTell, new BiosSeen("none"));
            default:
                throw new InvalidDataException("bios-steps.json names an unknown fact: " + fact);
        }
    }

    /// A storage controller in RAID mode, Intel RST or VMD, by its name ("Intel(R) RST VMD Controller 9A0B",
    /// "Intel(R) Chipset SATA/PCIe RST Premium Controller", "Volume Management Device NVMe RAID Controller").
    public static bool IsRaidController(string name) =>
        System.Text.RegularExpressions.Regex.IsMatch(name, @"\bRAID\b|\bRST\b|\bVMD\b|Volume Management Device|Rapid Storage|Optane",
            System.Text.RegularExpressions.RegexOptions.IgnoreCase);

    /// "3 to do, 5 done, 9 to check in the firmware."
    public static (int ToDo, int Done, int CannotTell) Count(IEnumerable<BiosStep> steps)
    {
        var l = steps.ToList();
        return (l.Count(s => s.State == BiosState.ToDo), l.Count(s => s.State == BiosState.Done), l.Count(s => s.State == BiosState.CannotTell));
    }
}
