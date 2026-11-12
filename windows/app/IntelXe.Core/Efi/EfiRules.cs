// SPDX-License-Identifier: MIT
using System.Text.Json;
using IntelXeEfi;

namespace IntelXe.Core.Efi;

/// efi-rules.json (release/templates/; docs/design/DESIGN-efi-rules.md), embedded in the Core at build time as
/// data/platforms.json is. Read with System.Text.Json into the plain tree the engine's Rules class takes: the same tree
/// IntelXe-EFI.ps1 makes with ConvertFrom-Json (ConvertTo-RulesTree): dictionaries in the file's order, lists, strings,
/// long integers and booleans. A fraction or a null is refused, as the script refuses them.
public static class EfiRules
{
    static readonly Lazy<Rules> embedded = new(() =>
    {
        using var s = typeof(EfiRules).Assembly.GetManifestResourceStream("IntelXe.Core.efi-rules.json")
            ?? throw new EfiException("efi-rules.json is not embedded in IntelXe.Core");
        using var r = new StreamReader(s);
        return Parse(r.ReadToEnd(), "efi-rules.json (built in)");
    });

    /// The rules this build carries.
    public static Rules Embedded => embedded.Value;

    /// The rules' revision ("revision"), for messages and reports.
    public static long Revision(Rules r) => r.Int("revision");

    public static Rules Load(string path) => Parse(File.ReadAllText(path), path);

    public static Rules Parse(string json, string source)
    {
        using var doc = JsonDocument.Parse(json);
        return Rules.FromTree(Tree(doc.RootElement, "rules"), source);
    }

    /// Any JSON value as the engine's tree (also the test vectors').
    public static object Tree(JsonElement e, string at)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
                var d = new Dictionary<string, object>();
                foreach (var p in e.EnumerateObject()) d[p.Name] = Tree(p.Value, at + "." + p.Name);
                return d;
            case JsonValueKind.Array:
                var l = new List<object>();
                int i = 0;
                foreach (var x in e.EnumerateArray()) l.Add(Tree(x, at + "." + i++));
                return l;
            case JsonValueKind.String: return e.GetString()!;
            case JsonValueKind.True: return true;
            case JsonValueKind.False: return false;
            case JsonValueKind.Number when e.TryGetInt64(out long n): return n;
        }
        throw new EfiException("JSON: " + at + " is not a string, an integer, a boolean, an object or a list");
    }
}
