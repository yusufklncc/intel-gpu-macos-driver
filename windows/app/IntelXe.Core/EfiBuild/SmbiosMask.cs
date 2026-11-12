// SPDX-License-Identifier: MIT
// The SMBIOS identity a build generates (serial number, MLB, UUID, ROM: macserial of the pinned OpenCore, through OpCore
// Simplify) stays in the generated config.plist only: never logged, shown or sent. The builder already removes every
// form of it from its log and result (windows/efibuild/intelxe_efibuild/validate.py, read_secrets/redact); the app does
// it again with the values it reads from config.plist before anything reaches the window, and masks them wherever it
// shows config values.
using System.Text.RegularExpressions;
using System.Xml;
using System.Xml.Linq;

namespace IntelXe.Core.EfiBuild;

public static partial class SmbiosMask
{
    public const string Redacted = "<redacted>";

    /// What the window shows in place of a masked value.
    public const string Masked = "••••••••";

    [GeneratedRegex(@"(Serial|UUID)|^(MLB|ROM)$", RegexOptions.IgnoreCase)]
    private static partial Regex SecretKeyRegex();

    /// A PlatformInfo key whose value identifies the machine (SystemSerialNumber, MLB, SystemUUID, ROM,
    /// BoardSerialNumber, ChassisSerialNumber, SmUUID, ...).
    public static bool IsSecretKey(string key) => SecretKeyRegex().IsMatch(key);

    /// The value as the window may show it: masked when its key identifies the machine.
    public static string Display(string key, string value) => IsSecretKey(key) && value.Length > 0 ? Masked : value;

    /// Every form in which the identity in config.plist's PlatformInfo could be printed: a string as it is, in lower and
    /// upper case (and a UUID without its dashes); data as hex (both cases, also with colons) and base64. Longest first,
    /// at least 6 characters. Empty when the file cannot be read.
    public static IReadOnlyList<string> ReadSecrets(string configPath)
    {
        var forms = new HashSet<string>(StringComparer.Ordinal);
        foreach (var (_, key, type, text) in PlatformValues(configPath))
        {
            if (!IsSecretKey(key)) continue;
            if (type == "data")
            {
                byte[] bytes;
                try { bytes = Convert.FromBase64String(Regex.Replace(text, @"\s+", "")); }
                catch (FormatException) { continue; }
                if (bytes.Length == 0) continue;
                var hex = Convert.ToHexStringLower(bytes);
                forms.UnionWith([hex, hex.ToUpperInvariant(), Convert.ToBase64String(bytes),
                    string.Join(":", bytes.Select(b => b.ToString("x2"))), string.Join(":", bytes.Select(b => b.ToString("X2")))]);
            }
            else if (type == "string" && text.Length >= 6)
            {
                forms.UnionWith([text, text.ToLowerInvariant(), text.ToUpperInvariant()]);
                if (text.Contains('-')) { var bare = text.Replace("-", ""); forms.UnionWith([bare, bare.ToLowerInvariant(), bare.ToUpperInvariant()]); }
            }
        }
        return forms.Where(f => f.Length >= 6).OrderByDescending(f => f.Length).ThenBy(f => f, StringComparer.Ordinal).ToList();
    }

    /// The text with every secret replaced by <redacted>.
    public static string Redact(string text, IReadOnlyList<string> secrets)
    {
        foreach (var s in secrets) text = text.Replace(s, Redacted, StringComparison.Ordinal);
        return text;
    }

    /// The secrets that appear in a text (a test that nothing leaked).
    public static IReadOnlyList<string> Find(string text, IReadOnlyList<string> secrets) =>
        secrets.Where(s => text.Contains(s, StringComparison.Ordinal)).ToList();

    /// PlatformInfo > Generic as the window shows it: the Mac model as it is, the identity masked, in config order.
    public static IReadOnlyList<(string Key, string Value)> PlatformSummary(string configPath) =>
        PlatformValues(configPath)
            .Where(v => v.Section == "Generic" && v.Type is "string" or "data" or "integer" or "true" or "false")
            .Select(v => (v.Key, Display(v.Key, v.Type is "true" or "false" ? v.Type : v.Text)))
            .ToList();

    /// One scalar of PlatformInfo's dictionaries: its dictionary (Generic, DataHub, PlatformNVRAM, SMBIOS), key, plist
    /// type and text (data stays base64).
    readonly record struct Scalar(string Section, string Key, string Type, string Text);

    static IEnumerable<Scalar> PlatformValues(string configPath)
    {
        XElement? root;
        try
        {
            var settings = new XmlReaderSettings { DtdProcessing = DtdProcessing.Ignore, XmlResolver = null };
            using var reader = XmlReader.Create(configPath, settings);
            root = XDocument.Load(reader).Root?.Element("dict");
        }
        catch (Exception e) when (e is IOException or XmlException or UnauthorizedAccessException) { yield break; }
        if (root is null) yield break;
        var platform = Pairs(root).FirstOrDefault(p => p.Key == "PlatformInfo").Value;
        if (platform is null || platform.Name != "dict") yield break;
        foreach (var (section, sub) in Pairs(platform))
        {
            if (sub.Name != "dict") continue;
            foreach (var (key, value) in Pairs(sub))
                if (value.Name != "dict" && value.Name != "array")
                    yield return new Scalar(section, key, value.Name.LocalName, value.Value.Trim());
        }
    }

    static IEnumerable<(string Key, XElement Value)> Pairs(XElement dict)
    {
        string? key = null;
        foreach (var e in dict.Elements())
        {
            if (e.Name == "key") { key = e.Value; continue; }
            if (key is not null) yield return (key, e);
            key = null;
        }
    }
}
