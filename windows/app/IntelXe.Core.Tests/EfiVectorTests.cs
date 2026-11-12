// SPDX-License-Identifier: MIT
// The shared test vectors of the EFI rules (release/efi-rules-vectors.json; the format is in
// docs/design/DESIGN-efi-rules.md section 5) against the Core's planner: the same vectors release/test-efi-vectors.sh
// runs against efi.sh and windows/test/Test-EfiVectors.ps1 against IntelXe-EFI.ps1. For each vector: the package and
// the EFI folder it describes, config.plist as stage 1 leaves it with the vector's edits, the facts as Windows gives
// them (the card's Location path, which the user confirms, when its instance id is one of the rules' cards), the plan's
// steps and notes, the change made through FolderEfiPlanner with the plan's hash, the values, files and list of changes
// it must leave, and undo back to the state before.
using System.Text;
using System.Text.Json;
using IntelXe.Core.Efi;
using IntelXeEfi;

namespace IntelXe.Core.Tests;

public sealed class EfiVectorTests
{
    // ---- the vectors file ------------------------------------------------------------------------------------------
    sealed class VectorFile
    {
        public required Dictionary<string, object> Root;
        public required Rules Rules;
        public required byte[] Stage1;
        public Dictionary<string, object> Tables => (Dictionary<string, object>)Root["tables"];
        public IEnumerable<Dictionary<string, object>> Vectors => List(Root, "vectors").Cast<Dictionary<string, object>>();
    }

    static readonly Lazy<VectorFile> file = new(() =>
    {
        using var doc = JsonDocument.Parse(File.ReadAllText(Paths.RepoFile("release", "efi-rules-vectors.json")));
        var root = (Dictionary<string, object>)EfiRules.Tree(doc.RootElement, "vectors");
        Assert.Equal("intelxe-efi-rules-vectors", root["format"]);
        Assert.Equal(1L, root["format_version"]);
        var stage1 = Plist.Load(Paths.RepoFile(((string)root["config"]).Split('/')));
        foreach (var e in List(root, "stage1")) Edit(stage1, (Dictionary<string, object>)e);
        return new VectorFile { Root = root, Rules = EfiRules.Embedded, Stage1 = stage1.ToBytes() };
    });

    public static TheoryData<string> Names()
    {
        var d = new TheoryData<string>();
        foreach (var v in file.Value.Vectors) d.Add((string)v["name"]);
        return d;
    }

    static List<object> List(Dictionary<string, object>? d, string key) =>
        d is not null && d.TryGetValue(key, out var v) ? (List<object>)v : [];
    static object? Key(Dictionary<string, object>? d, string key) => d is not null && d.TryGetValue(key, out var v) ? v : null;

    static byte[] Hex(string hex) => Convert.FromHexString(hex);

    static PValue Value(string type, object value) => type switch
    {
        "integer" => PValue.Int((long)value),
        "bool" => PValue.Bool((bool)value),
        "string" => PValue.Str((string)value),
        "data" => PValue.Data(Convert.FromBase64String((string)value)),
        "dict" => Rules.Value(value),
        _ => throw new InvalidOperationException("vectors: no value type " + type),
    };

    static void Edit(Plist pl, Dictionary<string, object> e)
    {
        var path = ((string)e["path"]).Split('.');
        switch ((string)e["op"])
        {
            case "set": pl.Set(path, Value((string)e["type"], e["value"])); break;
            case "remove": pl.Remove(path); break;
            case "append": pl.Append(path, Value((string)e["type"], e["value"])); break;
            case "insert": pl.Insert(path, (int)(long)e["index"], Value((string)e["type"], e["value"])); break;
            default: throw new InvalidOperationException("vectors: no edit " + e["op"]);
        }
    }

    static string Oc(string dir, string rel) => Path.Combine(dir, "EFI", "OC", Util.Native(rel));

    /// A fixture: the file it describes at DIR/EFI/OC/<its file>.
    static void Fixture(string dir, Dictionary<string, object> fx, string pkg)
    {
        var rel = (string)fx["file"];
        if (rel == "SHA256SUMS") return;   // the package's sums: Sums(), last
        var d = Oc(dir, rel);
        Util.Delete(d);
        Directory.CreateDirectory(Path.GetDirectoryName(d)!);
        switch ((string)fx["type"])
        {
            case "kext":
                var exe = Path.Combine(d, Util.Native((string)fx["executable"]));
                Directory.CreateDirectory(Path.GetDirectoryName(exe)!);
                File.WriteAllText(exe, (string)fx["text"] + "\n");
                var keys = new StringBuilder();
                if ((string)fx["bundle_id"] != "") keys.Append($"\t<key>CFBundleIdentifier</key>\n\t<string>{fx["bundle_id"]}</string>\n");
                if ((string)fx["version"] != "") keys.Append($"\t<key>CFBundleVersion</key>\n\t<string>{fx["version"]}</string>\n");
                File.WriteAllText(Path.Combine(d, "Contents", "Info.plist"),
                    $"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n{keys}</dict>\n</plist>\n");
                break;
            case "table": File.WriteAllBytes(d, Hex((string)file.Value.Tables[(string)fx["table"]])); break;
            case "dir": Directory.CreateDirectory(Path.Combine(d, "Contents", "MacOS")); break;
            case "package":
                Util.CopyTree(Oc(pkg, rel), d);
                if (Key(fx, "append") is string more)
                    foreach (var x in Directory.GetFiles(Path.Combine(d, "Contents", "MacOS"))) File.AppendAllText(x, more + "\n");
                break;
            case "none": break;
            default: throw new InvalidOperationException("vectors: no fixture type " + fx["type"]);
        }
    }

    static void Sums(string dir)
    {
        var efi = Path.Combine(dir, "EFI");
        var files = Directory.GetFiles(efi, "*", SearchOption.AllDirectories)
            .Select(f => f[(dir.Length + 1)..].Replace('\\', '/')).OrderBy(f => f, StringComparer.Ordinal);
        File.WriteAllText(Path.Combine(dir, "SHA256SUMS"),
            string.Join("\n", files.Select(rel => Util.Sha256File(Path.Combine(dir, Util.Native(rel))) + "  " + rel)) + "\n");
    }

    /// Every file under DIR but config.plist, with its SHA-256: two trees hold the same when these are the same.
    static string Tree(string dir) => string.Join("\n", Directory.GetFiles(dir, "*", SearchOption.AllDirectories)
        .Where(f => Path.GetFileName(f) != "config.plist")
        .Select(f => f[dir.Length..].Replace('\\', '/') + " " + Util.Sha256File(f)).OrderBy(x => x, StringComparer.Ordinal));

    // ---- a plan's step in the vectors' form, any tree as one comparable string (keys sorted) ------------------------
    static object ValueTree(PValue v) => v.Type switch
    {
        "dict" => v.Entries.ToDictionary(kv => kv.Key, kv => ValueTree(kv.Value)),
        "bool" => v.Text == "true",
        "integer" => long.Parse(v.Text),
        _ => v.Text,
    };

    static Dictionary<string, object> FromOp(Op op)
    {
        var d = new Dictionary<string, object> { ["op"] = op.Kind };
        switch (op.Kind)
        {
            case "copy": d["to"] = op.Rel; d["from"] = op.Bytes is not null ? "adapted" : "package"; break;
            case "set" or "append" or "insert":
                d["path"] = string.Join(".", op.Path);
                if (op.Kind == "insert") d["index"] = (long)op.Index;
                d["type"] = op.Value.Type;
                d["value"] = ValueTree(op.Value);
                break;
            case "mkdict" or "mkarray": d["path"] = string.Join(".", op.Path); break;
        }
        return d;
    }

    static string Canon(object? v) => v switch
    {
        null => "null",
        string s => "s\"" + s.Replace("\\", "\\\\").Replace("\"", "\\\"") + "\"",
        bool b => "b" + b,
        long or int => "i" + v,
        IDictionary<string, object> d => "{" + string.Join(",", d.Keys.OrderBy(k => k, StringComparer.Ordinal).Select(k => Canon(k) + "=" + Canon(d[k]))) + "}",
        System.Collections.IEnumerable l => "[" + string.Join(",", l.Cast<object>().Select(Canon)) + "]",
        _ => throw new InvalidOperationException("vectors: cannot compare a " + v.GetType().Name),
    };

    // ---- the facts as Windows gives them -----------------------------------------------------------------------------
    static string InstanceId(string pci)
    {
        var p = pci.Split(':');
        return $@"PCI\VEN_{p[0].ToUpperInvariant()}&DEV_{p[1].ToUpperInvariant()}&SUBSYS_00000000&REV_00\4&0&0&0008";
    }

    /// Device Manager's Location path: the port's ACPI names, then one PCI part per level down to the GPU ("none": the
    /// port is a PCI part too).
    static string LocationPath(string port, long levels)
    {
        var parts = new List<string>();
        long pci = levels;
        if (port == "none") { parts.AddRange(["ACPI(_SB_)", "ACPI(PC00)"]); pci = levels + 1; }
        else parts.AddRange(port.TrimStart('\\').Split('.').Select(s => $"ACPI({s.PadRight(4, '_')})"));
        for (int i = 0; i < pci; i++) parts.Add("PCI(0000)");
        return string.Join("#", parts);
    }

    // ---- one vector ---------------------------------------------------------------------------------------------------
    [Theory]
    [MemberData(nameof(Names))]
    public async Task Vector(string name)
    {
        var vf = file.Value;
        var rules = vf.Rules;
        var V = vf.Vectors.Single(v => (string)v["name"] == name);
        var root = Path.Combine(Path.GetTempPath(), "ixevec-" + Guid.NewGuid().ToString("N")[..8]);
        try
        {
            string P0 = Path.Combine(root, "pkg0"), P = Path.Combine(root, "pkg"), F = Path.Combine(root, "efi"), Bk = Path.Combine(root, "backups");
            foreach (var fx in List(vf.Root, "package")) Fixture(P0, (Dictionary<string, object>)fx, "");
            var cfg = Oc(F, "config.plist");
            foreach (var d in new[] { "Kexts", "ACPI" }) Directory.CreateDirectory(Oc(F, d));
            Directory.CreateDirectory(Path.Combine(F, "EFI", "BOOT"));
            File.WriteAllBytes(Oc(F, "OpenCore.efi"), []);
            Util.CopyTree(P0, P);
            bool sums = true;
            foreach (Dictionary<string, object> fx in List(V, "package"))
            {
                if ((string)fx["file"] == "SHA256SUMS" && (string)fx["type"] == "none") sums = false;
                Fixture(P, fx, "");
            }
            if (sums) Sums(P);
            var pl = Plist.Parse(vf.Stage1, "stage 1");
            foreach (Dictionary<string, object> e in List(V, "edits")) Edit(pl, e);
            File.WriteAllBytes(cfg, pl.ToBytes());
            foreach (Dictionary<string, object> fx in List(V, "efi")) Fixture(F, fx, P);
            var before = Plist.Canonical(Plist.Load(cfg).Root);
            var beforeTree = Tree(F);

            // the facts
            var facts = (Dictionary<string, object>)V["facts"];
            long levels = rules.Int("cards.port_levels_above_gpu");
            string loc = "", detected = "";
            if (Key(facts, "card") is Dictionary<string, object> card && rules.IsCardInstance(InstanceId((string)card["pci"])))
            {
                loc = LocationPath((string)card["port"], levels);               // what Device Manager shows, confirmed
                detected = EfiFolder.ConvertLocationPath(loc, levels) ?? "";     // what the app reads from Windows
            }
            var options = new EfiOptions(P, loc, detected, (string)facts["cpu"], (bool)facts["boot_path_recorded"], Bk);
            var change = EfiChanges.FromKind((string)V["plan"]) ?? throw new InvalidOperationException("vectors: plan " + V["plan"]);
            var x = (Dictionary<string, object>)V["expect"];
            var planner = new FolderEfiPlanner(rules);
            var target = new EfiTarget.Folder(F);

            // the plan: its steps (the engine's) and the planner's notes and hash
            var made = EfiWork.Open(F, options, rules).MakePlan(change.Kind());
            var plan = await planner.PlanAsync(target, change, options);
            Assert.Equal(made.Hash, plan.Hash);
            Assert.Equal(made.Text, plan.Text);
            Assert.Equal(Canon(List(x, "ops")), Canon(made.Ops.Select(FromOp).ToList()));
            var notes = List(x, "notes").Cast<string>().ToList();
            Assert.True(plan.Notes.Count == notes.Count && plan.Notes.Zip(notes).All(n => n.First.Contains(n.Second)),
                $"the notes: {string.Join(" | ", plan.Notes)}; the vector: {string.Join(" | ", notes)}");
            Assert.Equal(made.HasChanges, plan.HasChanges);

            // make it, and what it must leave
            if (List(x, "ops").Count == 0) return;
            await planner.ApplyAsync(plan, options);
            var why = new StringBuilder();
            var after = (Dictionary<string, object>?)Key(x, "after");
            var done = Plist.Load(cfg);
            foreach (Dictionary<string, object> a in List(after, "config"))
            {
                var e = done.Get(((string)a["path"]).Split('.'));
                if (Key(a, "absent") is not null) { if (e is not null) why.Append($" {a["path"]} is there;"); }
                else if (Plist.Raw(e) != Value((string)a["type"], a["value"]).Raw) why.Append($" {a["path"]}={Plist.Raw(e)};");
            }
            foreach (Dictionary<string, object> a in List(after, "files"))
            {
                var f = Oc(F, (string)a["file"]);
                if (Key(a, "state") is "package")
                {
                    var src = Oc(P, (string)a["file"]);
                    bool same = Directory.Exists(src) ? Directory.Exists(f) && Tree(src) == Tree(f) : Util.SameFile(src, f);
                    if (!same) why.Append($" {a["file"]} is not the package's;");
                }
                else if (Key(a, "state") is "absent") { if (Util.Exists(f)) why.Append($" {a["file"]} is there;"); }
                else if (Key(a, "sha256") is string sha) { if (!File.Exists(f) || Util.Sha256File(f) != sha) why.Append($" {a["file"]}'s SHA-256;"); }
                else if (Key(a, "table") is string t) { if (!Util.SameBytes(File.ReadAllBytes(f), Hex((string)vf.Tables[t]))) why.Append($" {a["file"]} is not {t};"); }
            }
            if (List(after, "changed").Count > 0)
            {
                var b = EfiWork.Open(F, options, rules).LastBackup();
                var listed = b is null ? "" : File.ReadAllText(Path.Combine(b, "changed")).TrimEnd('\n').Replace('\n', ' ');
                if (listed != string.Join(" ", List(after, "changed").Cast<string>())) why.Append($" the backup lists {listed};");
            }
            Assert.True(why.Length == 0, "applied:" + why);

            // undo: back to the state before
            if (Key(x, "undo") is not true) return;
            var undo = await planner.PlanAsync(target, EfiChange.Undo, options);
            Assert.True(undo.HasChanges);
            await planner.ApplyAsync(undo, options);
            Assert.Equal(before, Plist.Canonical(Plist.Load(cfg).Root));
            Assert.Equal(beforeTree, Tree(F));
        }
        finally
        {
            if (Directory.Exists(root)) Util.Delete(root);
        }
    }

    [Fact]
    public void ThereAreVectors() => Assert.True(file.Value.Vectors.Count() >= 32);

    [Fact]
    public void CpuNames()
    {
        foreach (Dictionary<string, object> c in List(file.Value.Root, "cpus"))
            Assert.True(EfiFolder.HybridCpu(file.Value.Rules, (string)c["cpu"]) == (bool)c["hybridpm"], $"{c["cpu"]}: the vectors say {c["hybridpm"]}");
    }

    [Fact]
    public void PciDevices()
    {
        foreach (Dictionary<string, object> c in List(file.Value.Root, "cards"))
            Assert.True(file.Value.Rules.IsCardInstance(InstanceId((string)c["pci"])) == (bool)c["card"], $"PCI {c["pci"]}: the vectors say card={c["card"]}");
    }

    [Fact]
    public void TheRulesAreTheRepositorys()
    {
        // the embedded file is release/templates/efi-rules.json, the one efi.sh and IntelXe-EFI.ps1 read
        var repo = EfiRules.Load(Paths.RepoFile("release", "templates", "efi-rules.json"));
        Assert.Equal(EfiRules.Revision(repo), EfiRules.Revision(EfiRules.Embedded));
        Assert.Equal(repo.Strings("cards.device_ids"), EfiRules.Embedded.Strings("cards.device_ids"));
        // and the cards are the app's supported cards (data/platforms.json)
        foreach (var id in repo.Strings("cards.device_ids"))
            Assert.NotNull(SupportedCards.Find(new PciId(0x8086, Convert.ToUInt16(id, 16))));
    }
}
