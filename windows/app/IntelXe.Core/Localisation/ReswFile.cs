// SPDX-License-Identifier: MIT
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace IntelXe.Core.Localisation;

/// A .resw string table (Strings/<language>/Resources.resw): its keys and values, and the checks that keep the
/// languages in step: the same keys, and the same {n} placeholders in each value.
public static class ReswFile
{
    public static IReadOnlyDictionary<string, string> Read(string path) => Parse(XDocument.Load(path));

    public static IReadOnlyDictionary<string, string> Parse(XDocument doc)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var data in doc.Root?.Elements("data") ?? [])
        {
            var name = (string?)data.Attribute("name");
            if (name is null) continue;
            if (map.ContainsKey(name)) throw new InvalidDataException($"key '{name}' appears twice");
            map[name] = (string?)data.Element("value") ?? "";
        }
        return map;
    }

    static readonly Regex Placeholder = new(@"\{(\d+)(?:[,:][^}]*)?\}");

    /// The {n} placeholders of a value, in order of their number.
    public static IReadOnlyList<int> Placeholders(string value) =>
        Placeholder.Matches(value).Select(m => int.Parse(m.Groups[1].Value)).Distinct().Order().ToList();

    /// The differences between two tables: keys only in one, values with other placeholders, empty values.
    public static IReadOnlyList<string> Compare(IReadOnlyDictionary<string, string> a, string aName, IReadOnlyDictionary<string, string> b, string bName)
    {
        var problems = new List<string>();
        foreach (var k in a.Keys.Except(b.Keys).Order()) problems.Add($"{k}: only in {aName}");
        foreach (var k in b.Keys.Except(a.Keys).Order()) problems.Add($"{k}: only in {bName}");
        foreach (var k in a.Keys.Intersect(b.Keys).Order())
        {
            if (!Placeholders(a[k]).SequenceEqual(Placeholders(b[k])))
                problems.Add($"{k}: placeholders {{{string.Join(",", Placeholders(a[k]))}}} in {aName}, {{{string.Join(",", Placeholders(b[k]))}}} in {bName}");
            if (a[k].Trim() == "") problems.Add($"{k}: empty in {aName}");
            if (b[k].Trim() == "") problems.Add($"{k}: empty in {bName}");
        }
        return problems;
    }

    // L.Get("Key"), L.Format("Key", ...) and {loc:L Key=Key} in the app's sources
    static readonly Regex[] Uses =
    [
        new(@"\bL\.(?:Get|Format)\(\s*""([A-Za-z0-9_]+)"""),
        new(@"\{\w+:L\s+Key=([A-Za-z0-9_]+)\s*\}"),
    ];

    /// The keys the app's .cs and .xaml files name literally.
    public static IReadOnlySet<string> KeysUsedIn(IEnumerable<string> files)
    {
        var used = new HashSet<string>(StringComparer.Ordinal);
        foreach (var file in files)
        {
            var text = File.ReadAllText(file);
            foreach (var re in Uses)
                foreach (Match m in re.Matches(text)) used.Add(m.Groups[1].Value);
        }
        return used;
    }
}
