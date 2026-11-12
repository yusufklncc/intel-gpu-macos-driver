// SPDX-License-Identifier: MIT
// The engine's lines in the app's words (EngineText, D13 of docs/design/DESIGN-efi-rules.md): every note, step and output
// line the engine's source (windows/IntelXe-EFI.ps1, between "// BEGIN ENGINE" and "// END ENGINE") can write has a
// template, each template is the one its own text finds, every key is in both string tables, and a line no template
// knows stays as the engine wrote it.
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;
using IntelXe.Core.Efi;
using IntelXe.Core.Localisation;

namespace IntelXe.Core.Tests;

public class EngineTextTests
{
    static string Engine()
    {
        var text = File.ReadAllText(Paths.RepoFile("windows", "IntelXe-EFI.ps1")).Replace("\r\n", "\n");
        int b = text.IndexOf("// BEGIN ENGINE\n", StringComparison.Ordinal), e = text.IndexOf("// END ENGINE", StringComparison.Ordinal);
        Assert.True(b >= 0 && e > b);
        return text[b..e];
    }

    /// The arguments of each call "name(" in the source, as written.
    static IEnumerable<string> Calls(string source, string name)
    {
        foreach (Match m in Regex.Matches(source, @"(?<![\w.])" + Regex.Escape(name) + @"\("))
        {
            int depth = 1, i = m.Index + m.Length, start = i;
            for (; i < source.Length && depth > 0; i++)
            {
                char c = source[i];
                if (c == '"') i = SkipString(source, i);
                else if (c == '\'') i = source.IndexOf('\'', i + (source[i + 1] == '\\' ? 3 : 2));
                else if (c == '(') depth++;
                else if (c == ')') depth--;
            }
            yield return source[start..(i - 1)];
        }
    }

    /// The index of a string literal's closing quote (regular or verbatim).
    static int SkipString(string s, int open)
    {
        bool verbatim = open > 0 && s[open - 1] == '@';
        for (int i = open + 1; i < s.Length; i++)
        {
            if (!verbatim && s[i] == '\\') { i++; continue; }
            if (s[i] == '"') { if (verbatim && i + 1 < s.Length && s[i + 1] == '"') { i++; continue; } return i; }
        }
        throw new InvalidDataException("an unterminated string in the engine");
    }

    static readonly Dictionary<string, string[]> Variables = new()
    {
        // PlanFiles: the SSDT's verb, (ss == "absent" ? "copy" : "replace").PadRight(8)
        ["verb"] = ["copy".PadRight(8), "replace".PadRight(8)],
    };

    /// What a call's argument can write: its string literals as they are, each other operand of the top-level "+" as
    /// "X" (a variable named in Variables as each of its values). Null when no literal is at the top level (a value
    /// taken from elsewhere: a rule's note, a line of the plan's text).
    static IEnumerable<string> Samples(string expr)
    {
        var parts = new List<string>();
        int depth = 0, start = 0;
        for (int i = 0; i < expr.Length; i++)
        {
            char c = expr[i];
            if (c == '"') i = SkipString(expr, i);
            else if (c == '(') depth++;
            else if (c == ')') depth--;
            else if (c == '+' && depth == 0) { parts.Add(expr[start..i].Trim()); start = i + 1; }
        }
        parts.Add(expr[start..].Trim());
        var outs = new List<StringBuilder> { new() };
        bool literal = false;
        foreach (var p in parts)
        {
            if (Regex.IsMatch(p, @"^@?""") && SkipString(p, p.IndexOf('"')) == p.Length - 1)
            {
                literal = true;
                var s = p.StartsWith('@') ? p[2..^1].Replace("\"\"", "\"") : Regex.Unescape(p[1..^1]);
                foreach (var o in outs) o.Append(s);
            }
            else if (Variables.TryGetValue(p, out var values))
                outs = outs.SelectMany(o => values.Select(v => new StringBuilder(o.ToString()).Append(v))).ToList();
            else foreach (var o in outs) o.Append('X');
        }
        return literal ? outs.Select(o => o.ToString()) : [];
    }

    static List<string> Unknown(EngineLine kind, IEnumerable<string> exprs, out int count)
    {
        var unknown = new List<string>();
        count = 0;
        foreach (var e in exprs)
            foreach (var s in Samples(e))
            {
                count++;
                if (!EngineText.IsKnown(kind, s)) unknown.Add(s);
            }
        return unknown;
    }

    [Fact]
    public void EveryNoteTheEngineCanWriteHasATemplate()
    {
        var engine = Engine();
        var calls = Calls(engine, "p.Notes.Add").ToList();
        Assert.True(calls.Count >= 12, $"{calls.Count} notes in the engine");
        Assert.Empty(Unknown(EngineLine.Note, calls, out int n));
        Assert.True(n >= 11);
        // the notes that are not written there: a "require" or "note" item's note in the rules (PlanItems)
        Assert.Equal(["R.Str(it + \"note\")", "R.Str(it + \"note\")"], calls.Where(c => !Samples(c).Any()).Select(c => c.Trim()));
        var rules = EfiRules.Load(Paths.RepoFile("release", "templates", "efi-rules.json"));
        int ruleNotes = 0;
        foreach (var list in RuleLists)
            for (int i = 0; i < rules.List(list).Count; i++)
                if (rules.StrOr($"{list}.{i}.note") is { Length: > 0 } note)
                {
                    ruleNotes++;
                    Assert.True(EngineText.IsKnown(EngineLine.Note, note), note);
                    // one template, no more: the most specific one is the only one whose text it is
                    Assert.Single(EngineText.Notes, t => Regex.IsMatch(note, "^" + Regex.Replace(Regex.Escape(t.Engine), @"\\\{\d+(\\\*)?}", ".*") + "$"));
                }
        Assert.True(ruleNotes >= 4, $"{ruleNotes} notes in the rules");
    }

    /// The rules' lists of items (PlanItems): stage 2, the Safari setting, boot logging.
    static readonly string[] RuleLists = ["settings", "safari", "bootlog"];

    [Fact]
    public void EveryRuleListIsCovered()
    {
        // a new list in efi-rules.json needs its notes and "why" texts here (and their keys in both string tables)
        using var doc = JsonDocument.Parse(File.ReadAllText(Paths.RepoFile("release", "templates", "efi-rules.json")));
        var lists = doc.RootElement.EnumerateObject()
            .Where(p => p.Value.ValueKind == JsonValueKind.Array && p.Value.EnumerateArray().Any(e => e.ValueKind == JsonValueKind.Object && e.TryGetProperty("kind", out _)))
            .Select(p => p.Name).Order(StringComparer.Ordinal);
        Assert.Equal(RuleLists.Order(StringComparer.Ordinal), lists);
    }

    [Fact]
    public void EveryStepAndOutputLineHasATemplate()
    {
        var engine = Engine();
        Assert.Empty(Unknown(EngineLine.Step, Calls(engine, "p.Lines.Add"), out int steps));
        Assert.True(steps >= 25, $"{steps} step lines");
        Assert.Empty(Unknown(EngineLine.Result, Calls(engine, "say"), out int said));
        Assert.True(said >= 6, $"{said} output lines");
        // the plan's text, which a change's output repeats
        foreach (var l in new[] { "EFI: the folder X (EFI, FAT32, 200 MB)", "nothing to change", "note     the package has no SHA256SUMS: its files are not checked" })
            Assert.True(EngineText.IsKnown(EngineLine.Result, l), l);
    }

    [Fact]
    public void ThePhrasesInsideLinesHaveTemplates()
    {
        var rules = EfiRules.Load(Paths.RepoFile("release", "templates", "efi-rules.json"));
        var phrases = new List<string>();
        foreach (var list in RuleLists)
            for (int i = 0; i < rules.List(list).Count; i++)
                if (rules.StrOr($"{list}.{i}.why") is { Length: > 0 } why) phrases.Add(why);
        foreach (var kind in new[] { "driver", "settings", "safari", "settings-safari", "bootlog" }) phrases.Add(rules.Str("backup_kinds." + kind));
        // what Aml.Adapt says when a table cannot be adapted, and what Windows reports for the card (PlanFiles' "seen")
        var engine = Engine();
        foreach (Match m in Regex.Matches(engine, @"\b(?:why|seen) = ([^;]+);"))
            phrases.AddRange(Samples(m.Groups[1].Value).Select(s => s.Trim()).Where(s => s.Length > 0));
        Assert.True(phrases.Count >= 19, string.Join(" | ", phrases));
        var unknown = phrases.Where(p => !EngineText.Fragments.Any(f => Regex.IsMatch(p, "^" + Regex.Escape(f.Engine).Replace(@"\{0}", ".*") + "$"))).ToList();
        Assert.Empty(unknown);
    }

    [Fact]
    public void EachTemplateIsTheOneItsOwnTextFinds()
    {
        foreach (var (kind, set) in new[] { (EngineLine.Step, EngineText.Steps), (EngineLine.Note, EngineText.Notes), (EngineLine.Result, EngineText.Results), (EngineLine.Error, EngineText.Errors) })
            foreach (var t in set)
            {
                var line = Regex.Replace(t.Engine, @"\{(\d+)\*?\}", m => "v" + m.Groups[1].Value);
                var m = EngineText.Match(kind, line);
                Assert.True(m is not null && m.Value.Template == t, $"{line}: found {m?.Template.Engine ?? "nothing"}");
            }
    }

    [Fact]
    public void BothStringTablesHaveEveryKey()
    {
        foreach (var lang in new[] { "en-US", "tr-TR" })
        {
            var table = ReswFile.Read(Paths.AppFile("Strings", lang, "Resources.resw"));
            Assert.DoesNotContain(EngineText.Keys, k => !table.ContainsKey(k));
            // a key's placeholders are its templates' (the values may leave none out: each part is shown)
            foreach (var t in EngineText.Steps.Concat(EngineText.Notes).Concat(EngineText.Results).Concat(EngineText.Errors).Concat(EngineText.Fragments))
                if (t.Key is not null)
                {
                    var want = Regex.Matches(t.Engine, @"\{(\d+)\*?\}").Select(m => int.Parse(m.Groups[1].Value)).Distinct().Order();
                    Assert.True(want.SequenceEqual(ReswFile.Placeholders(table[t.Key])), $"{lang} {t.Key}: {table[t.Key]}");
                }
        }
    }

    static Func<Text, string> Resolver(string lang)
    {
        var table = ReswFile.Read(Paths.AppFile("Strings", lang, "Resources.resw"));
        string R(Text t) => t.Key is null ? t.Verbatim ?? ""
            : string.Format(table[t.Key], t.Args.Select(a => a is Text x ? R(x) : a).ToArray());
        return R;
    }

    [Fact]
    public void LinesInTurkish()
    {
        var tr = Resolver("tr-TR");
        Assert.Equal("ayar     Booter > Quirks > ResizeAppleGpuBars: 0 -> 10 (B580'in VRAM'ine 1 GB'lık pencere)",
            EngineText.Show(EngineLine.Step, "config   Booter > Quirks > ResizeAppleGpuBars: 0 -> 10 (a 1 GB window on the B580's VRAM)", tr));
        Assert.Equal("değiştir EFI/OC/ACPI/SSDT-ARCOFF.aml (\\_SB.PC00.PEG2.PEGP adını taşır: bu kartın portu)",
            EngineText.Show(EngineLine.Step, "replace  EFI/OC/ACPI/SSDT-ARCOFF.aml (names \\_SB.PC00.PEG2.PEGP, this card's port)", tr));
        Assert.Equal("kaynak   C:\\b\\001-x (IntelXe dosyaları, 2026-11-12 10:00:00)",
            EngineText.Show(EngineLine.Step, "from     C:\\b\\001-x (IntelXe files, 2026-11-12 10:00:00)", tr));
        Assert.Equal("ayar     Misc > Debug > Target: 0x3 -> 0x43 (günlük açık, bir dosyaya: OpenCore'un açıldığı EFI biriminin kökünde opencore-<tarih>.txt)",
            EngineText.Show(EngineLine.Step, "config   Misc > Debug > Target: 0x3 -> 0x43 (logging on, to a file: opencore-<date>.txt in the root of the EFI volume OpenCore starts from)", tr));
        Assert.Equal("kaynak   C:\\b\\002-x (açılış günlüğü, 2026-11-13 10:00:00)",
            EngineText.Show(EngineLine.Step, "from     C:\\b\\002-x (boot logging, 2026-11-13 10:00:00)", tr));
        Assert.StartsWith("Açılış günlüğü her açılışı yavaşlatır: ", EngineText.Show(EngineLine.Note,
            "boot logging slows every boot: OpenCore rewrites its whole 256 KB log file after every line, which adds seconds from an internal SSD and can add tens of seconds or more from a USB stick. Undo this change once the logs are collected", tr));
        Assert.Equal("  tamam geri kondu EFI/OC/config.plist", EngineText.Show(EngineLine.Result, "  ok    restored EFI/OC/config.plist", tr));
        Assert.Equal("  tamam EFI/OC/Kexts/IntelXe.kext", EngineText.Show(EngineLine.Result, "  ok    EFI/OC/Kexts/IntelXe.kext", tr));
        Assert.Equal("not      Pakette SHA256SUMS yok: dosyaları denetlenmiyor.",
            EngineText.Show(EngineLine.Result, "note     the package has no SHA256SUMS: its files are not checked", tr));
        var note = EngineText.Show(EngineLine.Note,
            "not added: SSDT-ARCOFF.aml. Add it only after macOS is installed and install.sh --apply has run (stage 3), and only for your card's ACPI path: the package's table is for \\_SB.PC00.PEG1.PEGP (for another path it is adapted to that one). Check it in Device Manager (the B580 > Properties > Details > Location paths), then confirm it (-CardAcpiPath, or the SSDT-ARCOFF box in the window); stage 4, step 2 of README-INSTALL.md. Windows reports the card's port here as \\_SB.PC00.PEG2.PEGP.", tr);
        Assert.StartsWith("Eklenmedi: SSDT-ARCOFF.aml. ", note);
        Assert.EndsWith("Windows bu kartın portunu burada \\_SB.PC00.PEG2.PEGP olarak bildiriyor.", note);
        Assert.DoesNotContain("-CardAcpiPath", note);
    }

    [Fact]
    public void TheEnglishTableWordsThemForTheApp()
    {
        var en = Resolver("en-US");
        var note = EngineText.Show(EngineLine.Note,
            "not added: SSDT-ARCOFF.aml. Add it only after macOS is installed and install.sh --apply has run (stage 3), and only for your card's ACPI path: the package's table is for \\_SB.PC00.PEG1.PEGP (for another path it is adapted to that one). Check it in Device Manager (the B580 > Properties > Details > Location paths), then confirm it (-CardAcpiPath, or the SSDT-ARCOFF box in the window); stage 4, step 2 of README-INSTALL.md.", en);
        Assert.DoesNotContain("-CardAcpiPath", note);
        Assert.Contains("tick the box", note);
        Assert.Equal("config   Misc > Security > SecureBootModel: Default -> Disabled (Apple Secure Boot refuses a system volume with a changed seal: needed for the system-volume copy, Safari on the GPU)",
            EngineText.Show(EngineLine.Step, "config   Misc > Security > SecureBootModel: Default -> Disabled (Apple Secure Boot refuses a system volume with a changed seal: needed for the system-volume copy, Safari on the GPU)", en));
        Assert.DoesNotContain("PowerShell", EngineText.Show(EngineLine.Error,
            "cannot write to M:\\EFI\\OC. An EFI partition mounted with mountvol can be written only by an administrator: start PowerShell as administrator (or use the window's \"Restart as administrator\")", en));
    }

    [Fact]
    public void AnUnknownLineStaysAsTheEngineWroteIt()
    {
        var tr = Resolver("tr-TR");
        foreach (var kind in Enum.GetValues<EngineLine>())
        {
            Assert.False(EngineText.IsKnown(kind, "a line from a newer engine"));
            Assert.Equal("a line from a newer engine", EngineText.Show(kind, "a line from a newer engine", tr));
        }
        // a known template with a phrase it does not know keeps that phrase in English
        Assert.Equal("ayar     X > Y: 1 -> 2 (a newer reason)", EngineText.Show(EngineLine.Step, "config   X > Y: 1 -> 2 (a newer reason)", tr));
    }

    /// Every line and note of the plans and changes the shared vectors make (EfiVectorTests calls this).
    internal static void AssertKnown(EfiPlan plan, EfiResult? result)
    {
        foreach (var l in plan.Lines) Assert.True(EngineText.IsKnown(EngineLine.Step, l), "step: " + l);
        foreach (var n in plan.Notes) Assert.True(EngineText.IsKnown(EngineLine.Note, n), "note: " + n);
        Assert.True(EngineText.IsKnown(EngineLine.Result, plan.Text.Split('\n')[0]), plan.Text.Split('\n')[0]);
        foreach (var l in result?.Lines ?? []) Assert.True(EngineText.IsKnown(EngineLine.Result, l), "output: " + l);
    }
}
