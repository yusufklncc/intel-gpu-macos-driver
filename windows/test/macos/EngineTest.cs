// SPDX-License-Identifier: MIT
// EngineTest: the engine of windows/IntelXe-EFI.ps1 on macOS, against what release/templates/efi.sh plans and writes
// (expected/efi-sh/, made by make-expected.sh). The same steps as Test-IntelXe-EFI.ps1 runs on Windows through the
// script itself. C# 5, like the engine.
//   EngineTest STAGE1_DIR_CONFIG SAMPLE EXPECTED_DIR ENGINE_OUT_DIR WORK_DIR RULES [PACKAGE]
// RULES: release/templates/efi-rules.json, read here with System.Text.Json into the tree the script makes with
// ConvertFrom-Json (RulesJson.Load, which the WinUI app can take as it is).
using System;
using System.Collections.Generic;
using System.IO;
using System.Text;
using System.Text.Json;
using IntelXeEfi;

/// efi-rules.json (or any JSON) as the engine's tree: Dictionary<string, object> in the file's order, List<object>,
/// string, long, bool. A fraction or null is refused, as the script's ConvertTo-RulesTree refuses them.
static class RulesJson
{
    public static object Tree(JsonElement e, string at)
    {
        switch (e.ValueKind)
        {
            case JsonValueKind.Object:
                var d = new Dictionary<string, object>();
                foreach (JsonProperty p in e.EnumerateObject()) d[p.Name] = Tree(p.Value, at + "." + p.Name);
                return d;
            case JsonValueKind.Array:
                var l = new List<object>();
                int i = 0;
                foreach (JsonElement x in e.EnumerateArray()) l.Add(Tree(x, at + "." + i++));
                return l;
            case JsonValueKind.String: return e.GetString();
            case JsonValueKind.True: return true;
            case JsonValueKind.False: return false;
            case JsonValueKind.Number:
                long n;
                if (e.TryGetInt64(out n)) return n;
                break;
        }
        throw new EfiException("JSON: " + at + " is not a string, an integer, a boolean, an object or a list");
    }
    public static object Tree(string path)
    {
        using (JsonDocument doc = JsonDocument.Parse(File.ReadAllText(path))) return Tree(doc.RootElement, "rules");
    }
    public static Rules Load(string path) { return Rules.FromTree(Tree(path), path); }
}

class EngineTest
{
    static int fails;
    static Rules Ru;
    const string Apple = "7C436110-AB2A-4BBB-A880-FE41995C9F82";
    static string Stage1, NoEntries, NoHpm, Aml013, Sample, X, O, Work, F, P, Bk;
    const string Hybrid = "13th Gen Intel(R) Core(TM) i5-13400F";
    static readonly StringBuilder log = new StringBuilder();

    static void Ok(bool c, string what)
    {
        Console.WriteLine((c ? "ok    " : "FAIL  ") + what);
        if (!c) fails++;
    }
    static void Say(string line) { log.Append(line).Append('\n'); }
    static string Cfg { get { return Path.Combine(Path.Combine(Path.Combine(F, "EFI"), "OC"), "config.plist"); } }
    static string Oc(string rel) { return Path.Combine(Path.Combine(Path.Combine(F, "EFI"), "OC"), rel.Replace('/', Path.DirectorySeparatorChar)); }
    static bool SameBytes(byte[] a, byte[] b)
    {
        if (a.Length != b.Length) return false;
        for (int i = 0; i < a.Length; i++) if (a[i] != b[i]) return false;
        return true;
    }
    static EfiFolder Open(string card)
    {
        var f = new EfiFolder(F, P, Bk, Ru);
        if (card != null) f.SetCard(card);
        return f;
    }
    static EfiFolder Open(string card, string cpu)
    {
        EfiFolder f = Open(card);
        f.Cpu = cpu;
        return f;
    }
    static string Pk(string rel) { return Path.Combine(P, rel.Replace('/', Path.DirectorySeparatorChar)); }
    static Dictionary<string, string> Status(EfiFolder f)
    {
        var d = new Dictionary<string, string>();
        foreach (string l in f.Status()) { int i = l.IndexOf('='); d[l.Substring(0, i)] = l.Substring(i + 1); }
        return d;
    }
    static void Expect(EfiFolder f, string what, string key, string value)
    {
        string got = Status(f)[key];
        Ok(got == value, what + ": " + key + "=" + got + (got == value ? "" : ", expected " + value));
    }
    static void Apply(EfiFolder f, string kind)
    {
        Plan p = f.MakePlan(kind);
        f.Run(kind, p.Hash, Say);
    }
    static bool Throws(Action a, string text)
    {
        try { a(); return false; }
        catch (EfiException e) { if (!e.Message.Contains(text)) Console.WriteLine("      message: " + e.Message); return e.Message.Contains(text); }
    }

    // the lines that say what changes: the plan without its "EFI:" line and its notes
    static List<string> Actions(string text, bool fromEfiSh)
    {
        var list = new List<string>();
        foreach (string raw in text.Replace("\r\n", "\n").Split('\n'))
        {
            string l = raw;
            if (l.Length == 0 || l.StartsWith("EFI: ") || l.StartsWith("note ")) continue;
            if (fromEfiSh) l = l.Replace("for this script)", "for efi.sh on macOS)");
            list.Add(l);
        }
        return list;
    }
    static void ComparePlan(Plan p, string name)
    {
        List<string> want = Actions(File.ReadAllText(Path.Combine(Path.Combine(X, "efi-sh"), name + ".plan")), true), got = Actions(p.Text, false);
        bool same = string.Join("\n", want.ToArray()) == string.Join("\n", got.ToArray());
        Ok(same, "plan " + name + ": the same " + want.Count + " lines as efi.sh's");
        if (!same) { Console.WriteLine("      efi.sh:\n        " + string.Join("\n        ", want.ToArray())); Console.WriteLine("      engine:\n        " + string.Join("\n        ", got.ToArray())); }
    }
    static string Header(string text)
    {
        string[] l = text.Replace("\r\n", "\n").Split('\n');
        return l[0] + "\n" + l[1];
    }
    static void CompareConfig(string name, string input)
    {
        Plist got = Plist.Load(Cfg), want = Plist.Load(Path.Combine(Path.Combine(X, "efi-sh"), name + ".plist"));
        string d = Plist.Difference(want.Root, got.Root, "config.plist");
        Ok(d == null, name + ": the keys and values of efi.sh's" + (d == null ? "" : ": " + d));
        string text = File.ReadAllText(Cfg), before = File.ReadAllText(input);
        Ok(Plist.IsSubsequence(Plist.KeyPaths(Plist.Load(input).Root, "", null), Plist.KeyPaths(got.Root, "", null)), name + ": the keys it had, in their order");
        Ok(Header(text) == Header(before) && !text.Contains(" />"), name + ": XML declaration and DOCTYPE kept, no '<x />'");
        Console.WriteLine("      " + Util.ChangedLines(before, text) + " lines differ from the input");
        File.Copy(Cfg, Path.Combine(O, name + ".plist"), true);
    }

    static void MakePackage()
    {
        string k = Path.Combine(Path.Combine(Path.Combine(Path.Combine(P, "EFI"), "OC"), "Kexts"), "IntelXe.kext");
        Directory.CreateDirectory(Path.Combine(Path.Combine(k, "Contents"), "MacOS"));
        Directory.CreateDirectory(Path.Combine(Path.Combine(Path.Combine(P, "EFI"), "OC"), "ACPI"));
        File.WriteAllText(Path.Combine(Path.Combine(Path.Combine(k, "Contents"), "MacOS"), "IntelXe"), "stand-in IntelXe executable\n");
        File.WriteAllText(Path.Combine(Path.Combine(k, "Contents"), "Info.plist"),
            "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n" +
            "<plist version=\"1.0\">\n<dict>\n\t<key>CFBundleVersion</key>\n\t<string>9.9.9</string>\n</dict>\n</plist>\n");
        string h = Path.Combine(Path.Combine(Path.Combine(Path.Combine(P, "EFI"), "OC"), "Kexts"), "HybridPM.kext");
        Directory.CreateDirectory(Path.Combine(Path.Combine(h, "Contents"), "MacOS"));
        File.WriteAllText(Path.Combine(Path.Combine(Path.Combine(h, "Contents"), "MacOS"), "HybridPM"), "stand-in HybridPM executable\n");
        File.WriteAllText(Path.Combine(Path.Combine(h, "Contents"), "Info.plist"), InfoPlist("io.github.yusufklncc.HybridPM", "9.9.8"));
        File.WriteAllBytes(Path.Combine(Path.Combine(Path.Combine(Path.Combine(P, "EFI"), "OC"), "ACPI"), "SSDT-ARCOFF.aml"),
            Encoding.ASCII.GetBytes("SSDT\x2f\x04_SB_PC00PEG1PEGP stand-in"));
        WriteSums();
    }
    static string InfoPlist(string id, string version)
    {
        return "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n\t<key>CFBundleIdentifier</key>\n\t<string>" + id +
            "</string>\n\t<key>CFBundleVersion</key>\n\t<string>" + version + "</string>\n</dict>\n</plist>\n";
    }
    static void WriteSums()
    {
        var files = new List<string>(Directory.GetFiles(Path.Combine(P, "EFI"), "*", SearchOption.AllDirectories));
        files.Sort(StringComparer.Ordinal);
        var sb = new StringBuilder();
        foreach (string f in files) sb.Append(Util.Sha256File(f)).Append("  ").Append(f.Substring(P.Length + 1).Replace('\\', '/')).Append('\n');
        File.WriteAllText(Path.Combine(P, "SHA256SUMS"), sb.ToString());
    }
    static void FreshEfi(string config)
    {
        if (Directory.Exists(F)) Util.Delete(F);
        Directory.CreateDirectory(Path.Combine(Path.Combine(Path.Combine(F, "EFI"), "OC"), "Kexts"));
        Directory.CreateDirectory(Path.Combine(Path.Combine(Path.Combine(F, "EFI"), "OC"), "ACPI"));
        Directory.CreateDirectory(Path.Combine(Path.Combine(F, "EFI"), "BOOT"));
        File.WriteAllBytes(Oc("OpenCore.efi"), new byte[0]);
        File.Copy(config, Cfg, true);
        if (Directory.Exists(Bk)) Util.Delete(Bk);
    }
    static void Edit(Action<Plist> change)
    {
        Plist pl = Plist.Load(Cfg);
        change(pl);
        File.WriteAllBytes(Cfg, pl.ToBytes());
    }

    static int Main(string[] args)
    {
        Stage1 = Path.GetFullPath(args[0]);
        NoEntries = Path.Combine(Path.GetDirectoryName(Stage1), "config-noentries.plist");
        NoHpm = Path.Combine(Path.GetDirectoryName(Stage1), "config-nohybridpm.plist");
        Aml013 = Path.Combine(Path.GetDirectoryName(Stage1), "SSDT-ARCOFF-0.1.3.aml");
        Sample = Path.GetFullPath(args[1]);
        X = Path.GetFullPath(args[2]);
        O = Path.GetFullPath(args[3]);
        Work = Path.GetFullPath(args[4]);
        Ru = RulesJson.Load(Path.GetFullPath(args[5]));
        F = Path.Combine(Work, "efi");
        P = Path.Combine(Work, "pkg");
        Bk = Path.Combine(Work, "backups");
        Directory.CreateDirectory(Work);
        Directory.CreateDirectory(O);
        byte[] stage1 = File.ReadAllBytes(Stage1);

        // ---- reading and writing ----
        foreach (string f in new[] { Stage1, NoEntries, NoHpm, Sample })
            Ok(SameBytes(Plist.Load(f).ToBytes(), File.ReadAllBytes(f)), Path.GetFileName(f) + " read and written back: the same bytes");
        byte[] crlf = Encoding.UTF8.GetBytes(Encoding.UTF8.GetString(stage1).Replace("\n", "\r\n"));
        Ok(SameBytes(Plist.Parse(crlf, "crlf").ToBytes(), crlf), "with CRLF line ends: the same bytes");
        var bom = new byte[stage1.Length + 3];
        bom[0] = 0xEF; bom[1] = 0xBB; bom[2] = 0xBF;
        Array.Copy(stage1, 0, bom, 3, stage1.Length);
        Ok(SameBytes(Plist.Parse(bom, "bom").ToBytes(), bom), "with a byte order mark: the same bytes");
        Ok(Throws(delegate { Plist.Parse(Encoding.ASCII.GetBytes("bplist00xxxx"), "b"); }, "binary property list"), "a binary plist is refused");
        Ok(Throws(delegate { Plist.Parse(Encoding.ASCII.GetBytes("<plist><dict><key>a</dict></plist>"), "x"); }, "not a valid property list"), "broken XML is refused");

        // ---- the card's path ----
        Ok(EfiFolder.ConvertLocationPath("ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)", 2) == @"\_SB.PC00.PEG1.PEGP", "Location path: the shipped table's port");
        Ok(EfiFolder.ConvertLocationPath("ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#PCI(0000)#PCI(0100)#PCI(0000)", 2) == "none", "Location path with three PCI parts: none");
        Ok(EfiFolder.ConvertLocationPath("ACPI(_SB_)#ACPI(PC00)#ACPI(RP05)#ACPI(PXSX)#ACPI(DSB0)#PCI(0000)", 2) == @"\_SB.PC00.RP05.PXSX", "Location path with a named downstream port: two above the GPU");
        Ok(EfiFolder.ConvertLocationPath("PCIROOT(0)#PCI(0100)#PCI(0000)#PCI(0100)#PCI(0000)", 2) == null, "PCIROOT line: not an ACPI path");
        Ok(EfiFolder.NormalizeCard(@"\_SB_.PC00.PEG1.PEGP", 2) == @"\_SB.PC00.PEG1.PEGP" && EfiFolder.NormalizeCard("_sb.pc00.peg1.pegp", 2) == @"\_SB.PC00.PEG1.PEGP", "ACPI path forms (Linux's _SB_, lower case)");
        Ok(Throws(delegate { EfiFolder.NormalizeCard("PEG1 slot", 2); }, "is not an ACPI path"), "a wrong path is refused");

        // ---- stage 1 -> settings -> driver, undo ----
        P = Path.Combine(Work, "pkg");
        MakePackage();
        FreshEfi(Stage1);
        EfiFolder ef = Open(null);
        Expect(ef, "stage 1", "opencore", "yes");
        Expect(ef, "stage 1", "kext_file", "absent");
        Expect(ef, "stage 1", "kext_entry", "disabled");
        Expect(ef, "stage 1", "ssdt_file", "absent");
        Expect(ef, "stage 1", "ssdt_entry", "disabled");
        Expect(ef, "stage 1", "package_checksums", "ok");
        Expect(ef, "stage 1", "settings_changes", "6");
        Expect(ef, "stage 1", "plan_changes", "3");
        Expect(ef, "stage 1", "backup", "none");

        Plan s = ef.MakePlan("settings");
        ComparePlan(s, "settings");
        Ok(Throws(delegate { Open(null).Run("settings", new string('0', 64), Say); }, "changed since the plan was shown"), "apply with another plan's hash refuses");
        Ok(Throws(delegate { Open(null).Run("settings", "", Say); }, "give the SHA-256"), "apply without a hash refuses");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1) && !Directory.Exists(Bk), "... and nothing changed, no backup made");
        Apply(Open(null), "settings");
        CompareConfig("after-settings", Stage1);
        Expect(Open(null), "after settings", "settings_changes", "0");
        Expect(Open(null), "after settings", "backup_kind", "settings");
        byte[] afterSettings = File.ReadAllBytes(Cfg);
        string b1 = Status(Open(null))["backup"];
        Ok(SameBytes(File.ReadAllBytes(Path.Combine(Path.Combine(Path.Combine(Path.Combine(b1, "EFI"), "OC")), "config.plist")), stage1)
            && File.Exists(Path.Combine(Path.Combine(Path.Combine(b1, "EFI"), "OC"), "OpenCore.efi")), "the backup holds the whole EFI folder as it was");

        Plan d = Open(null).MakePlan("driver");
        ComparePlan(d, "driver-nossdt");
        Ok(d.Text.Contains("note     not added: SSDT-ARCOFF.aml. Add it only after macOS is installed"), "without a confirmed path SSDT-ARCOFF is left out, and the note says why");
        Apply(Open(null), "driver");
        CompareConfig("after-settings-driver-nossdt", Stage1);
        Ok(Util.SameFile(Oc("Kexts/IntelXe.kext/Contents/MacOS/IntelXe"), Path.Combine(P, "EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe".Replace('/', Path.DirectorySeparatorChar))) && !File.Exists(Oc("ACPI/SSDT-ARCOFF.aml")), "IntelXe.kext copied, no SSDT-ARCOFF.aml");
        Expect(Open(null), "after driver", "plan_changes", "0");
        Expect(Open(null), "after driver", "kext_file", "same");

        Plan u = Open(null).MakePlan("undo");
        Ok(u.Text.Contains("restore  EFI/OC/config.plist\n") && u.Text.Contains("remove   EFI/OC/Kexts/IntelXe.kext (it was not there before)"), "undo plan: config.plist back, the kext removed");
        Open(null).Run("undo", u.Hash, Say);
        Ok(SameBytes(File.ReadAllBytes(Cfg), afterSettings) && !Directory.Exists(Oc("Kexts/IntelXe.kext")), "undo: as after settings");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1), "second undo: config.plist byte for byte as in stage 1");
        Ok(Throws(delegate { Apply(Open(null), "undo"); }, "no backup to put back"), "nothing left to undo");
        // undo acts only on what the rules change (undo_paths): another line in the backup's list is named and left alone
        Directory.CreateDirectory(Oc("Drivers"));
        File.WriteAllText(Oc("Drivers/OpenRuntime.efi"), "driver\n");
        Apply(Open(null), "driver");
        string bu = Status(Open(null))["backup"];
        File.AppendAllText(Path.Combine(bu, "changed"), "Drivers/OpenRuntime.efi\n");
        File.Delete(Path.Combine(Path.Combine(Path.Combine(Path.Combine(bu, "EFI"), "OC"), "Drivers"), "OpenRuntime.efi"));
        u = Open(null).MakePlan("undo");
        Ok(u.Text.Contains("note     the backup lists EFI/OC/Drivers/OpenRuntime.efi, which these rules never change: left as it is") && !u.Text.Contains("remove   EFI/OC/Drivers"), "a path outside undo_paths: named in the undo plan, not removed");
        Open(null).Run("undo", u.Hash, Say);
        Ok(File.Exists(Oc("Drivers/OpenRuntime.efi")) && SameBytes(File.ReadAllBytes(Cfg), stage1) && !Directory.Exists(Oc("Kexts/IntelXe.kext")), "... undo puts the rest back and leaves that file");
        Util.Delete(Oc("Drivers"));

        // ---- with the card's path confirmed ----
        Apply(Open(null), "settings");
        Ok(SameBytes(File.ReadAllBytes(Cfg), afterSettings), "settings again: the same bytes");
        Plan c = Open(@"\_SB.PC00.PEG1.PEGP").MakePlan("driver");
        ComparePlan(c, "driver-ssdt");
        Ok(Open("ACPI(_SB_)#ACPI(PC00)#ACPI(PEG1)#ACPI(PEGP)#PCI(0100)#PCI(0000)").MakePlan("driver").Hash == c.Hash, "a Location paths line gives the same plan");
        Apply(Open(@"\_SB.PC00.PEG1.PEGP"), "driver");
        CompareConfig("after-settings-driver-ssdt", Stage1);
        Ok(Util.SameFile(Oc("ACPI/SSDT-ARCOFF.aml"), Path.Combine(P, "EFI/OC/ACPI/SSDT-ARCOFF.aml".Replace('/', Path.DirectorySeparatorChar))), "SSDT-ARCOFF.aml copied");
        Edit(delegate(Plist pl) { pl.Set(new[] { "Misc", "Boot", "Timeout" }, PValue.Int(9)); });
        u = Open(null).MakePlan("undo");
        Ok(u.Text.Contains("note     config.plist was changed after that change"), "undo plan warns about a later edit");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), afterSettings), "undo: as after settings (the later edit is gone, as warned)");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1), "undo: stage 1");

        // ---- the package ----
        string aml = Path.Combine(P, "EFI/OC/ACPI/SSDT-ARCOFF.aml".Replace('/', Path.DirectorySeparatorChar));
        byte[] amlBytes = File.ReadAllBytes(aml);
        File.AppendAllText(aml, "x");
        Ok(Throws(delegate { Apply(Open(@"\_SB.PC00.PEG1.PEGP"), "driver"); }, "differ from its SHA256SUMS"), "a damaged package is refused");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1) && !Directory.Exists(Oc("Kexts/IntelXe.kext")), "... and nothing changed");
        File.WriteAllBytes(aml, amlBytes);
        string extra = Path.Combine(P, "EFI/OC/Kexts/IntelXe.kext/Contents/extra".Replace('/', Path.DirectorySeparatorChar));
        File.WriteAllText(extra, "x");
        Ok(Open(null).CheckSums().StartsWith("not listed"), "a file in the kext that SHA256SUMS does not list is refused");
        File.Delete(extra);

        // ---- a different kext; driver before settings ----
        ComparePlan(Open(null).MakePlan("driver"), "driver-stage1");
        Apply(Open(null), "driver");
        CompareConfig("after-driver-nossdt", Stage1);
        File.AppendAllText(Oc("Kexts/IntelXe.kext/Contents/MacOS/IntelXe"), "older build\n");
        Expect(Open(null), "an older kext in the EFI", "kext_file", "different");
        Ok(Open(null).MakePlan("driver").Text.Contains("replace  EFI/OC/Kexts/IntelXe.kext (version 9.9.9 in the EFI, 9.9.9 in the package; the files differ)"), "plan replaces it");
        Apply(Open(null), "driver");
        Expect(Open(null), "after apply", "kext_file", "same");
        Apply(Open(null), "undo");
        Expect(Open(null), "undo puts the older one back", "kext_file", "different");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1) && !Directory.Exists(Oc("Kexts/IntelXe.kext")), "undo: stage 1");

        // ---- SSDT-ARCOFF: the port ----
        Plan o = Open(@"\_SB.PC00.PEG0.PEGP").MakePlan("driver");
        Ok(!o.Text.Contains("EFI/OC/ACPI/SSDT-ARCOFF.aml (names") && o.Text.Contains(@"the card's port here is \_SB.PC00.PEG0.PEGP"), "another port: the table is not copied, and the note says why");
        Ok(Open("none").MakePlan("driver").Text.Contains("has no ACPI name"), "a port without an ACPI name: says so");
        File.WriteAllBytes(Oc("ACPI/SSDT-ARCOFF.aml"), Encoding.ASCII.GetBytes("SSDT\x2f\x04_SB_PC00PEG0PEGP my own build"));
        o = Open(@"\_SB.PC00.PEG0.PEGP").MakePlan("driver");
        Ok(o.Text.Contains("kept: EFI/OC/ACPI/SSDT-ARCOFF.aml differs") && o.Text.Contains("ACPI > Add, item 5 (SSDT-ARCOFF.aml): Enabled false -> true"), "your own table for that port is kept, its entry enabled");
        File.Delete(Oc("ACPI/SSDT-ARCOFF.aml"));
        EfiFolder seen = Open(null);
        seen.CardDetected = @"\_SB.PC00.PEG1.PEGP";
        Ok(seen.MakePlan("driver").Text.Contains(@"Windows reports the card's port here as \_SB.PC00.PEG1.PEGP."), "what Windows reports goes into the note");

        // ---- an earlier B580.kext entry; a wider SIP value ----
        Edit(delegate(Plist pl) { pl.Append(new[] { "Kernel", "Add" }, PValue.Dict("BundlePath", "B580.kext", "Enabled", true, "ExecutablePath", "Contents/MacOS/B580", "PlistPath", "Contents/Info.plist")); });
        Ok(Open(null).MakePlan("driver").Text.Contains("item 16 (B580.kext, the earlier name of IntelXe.kext): Enabled true -> false"), "an earlier B580.kext entry is turned off");
        File.WriteAllBytes(Cfg, stage1);
        Apply(Open(null), "settings");
        Edit(delegate(Plist pl) { pl.Set(new[] { "NVRAM", "Add", Apple, "csr-active-config" }, PValue.Data(EfiFolder.CsrBytes(0xfef))); });
        Ok(Open(null).MakePlan("settings").Text.Contains("\nnothing to change\n"), "a wider SIP value (0xfef) is kept");
        Ok(File.ReadAllText(Cfg).Contains("<data>\n\t\t\t\t7w8AAA==\n\t\t\t\t</data>"), "data keeps its layout");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1), "undo: stage 1");

        // ---- adding the entries ----
        FreshEfi(NoEntries);
        ComparePlan(Open(@"\_SB.PC00.PEG1.PEGP").MakePlan("driver"), "noentries-driver-ssdt");
        Apply(Open(@"\_SB.PC00.PEG1.PEGP"), "driver");
        CompareConfig("after-noentries-driver-ssdt", NoEntries);
        string t = File.ReadAllText(Cfg);
        Ok(t.Contains("\t\t\t<dict>\n\t\t\t\t<key>Arch</key>\n\t\t\t\t<string>Any</string>\n\t\t\t\t<key>BundlePath</key>\n\t\t\t\t<string>IntelXe.kext</string>")
            && t.Contains("\t\t\t\t<key>MaxKernel</key>\n\t\t\t\t<string></string>") && t.Contains("<string>Contents/Info.plist</string>\n\t\t\t</dict>\n\t\t</array>"),
            "an added entry is indented like its neighbours");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), File.ReadAllBytes(NoEntries)), "undo: as before");

        // ---- missing parts of NVRAM ----
        File.WriteAllBytes(Cfg, stage1);
        Edit(delegate(Plist pl)
        {
            System.Xml.XmlElement add = pl.Get("NVRAM", "Add"), apple = pl.Get("NVRAM", "Add", Apple);
            add.RemoveChild(apple.PreviousSibling.PreviousSibling.PreviousSibling);   // its whitespace and key
            add.RemoveChild(apple.PreviousSibling.PreviousSibling);
            add.RemoveChild(apple.PreviousSibling);
            add.RemoveChild(apple);
            System.Xml.XmlElement nv = pl.Get("NVRAM"), del = pl.Get("NVRAM", "Delete");
            nv.RemoveChild(del.PreviousSibling.PreviousSibling);
            nv.RemoveChild(del.PreviousSibling);
            nv.RemoveChild(del);
        });
        Ok(Plist.Load(Cfg).Get("NVRAM", "Add", Apple) == null && Plist.Load(Cfg).Get("NVRAM", "Delete") == null, "a config.plist without NVRAM > Add > 7C43... and NVRAM > Delete");
        s = Open(null).MakePlan("settings");
        Ok(s.Text.Contains("NVRAM > Add > " + Apple + ": add (missing)") && s.Text.Contains("NVRAM > Delete: add (missing)"), "the settings plan adds them");
        Apply(Open(null), "settings");
        Plist done = Plist.Load(Cfg);
        Ok(Plist.Raw(done.Get("NVRAM", "Add", Apple, "csr-active-config")) == "AwgAAA==" &&
           Plist.Raw(done.Get("NVRAM", "Add", Apple, "boot-args")) == "amfi_get_out_of_my_way=0x1 intelxe_surfvram=2" &&
           Plist.Canonical(done.Get("NVRAM", "Delete", Apple)) == "[s\"csr-active-config\",s\"boot-args\",]", "... with csr-active-config 03080000, the two boot-args, both in NVRAM > Delete");
        Ok(Open(null).MakePlan("settings").Lines.Count == 0, "... and then nothing to change");
        Apply(Open(null), "undo");

        // ---- CRLF ----
        File.WriteAllBytes(Cfg, crlf);
        Apply(Open(null), "settings");
        string after = File.ReadAllText(Cfg);
        Ok(!after.Replace("\r\n", "").Contains("\n") && after.Replace("\r\n", "\n") == Encoding.UTF8.GetString(afterSettings), "a CRLF config.plist stays CRLF, otherwise the same edit");

        // ---- HybridPM.kext: by the CPU's name, below Lilu.kext, only when Lilu loads ----
        int cpus = 0, cpuFails = 0;
        foreach (string line in File.ReadAllLines(Path.Combine(Path.Combine(X, "efi-sh"), "hybrid-cpus.txt")))
        {
            int bar = line.IndexOf('|');
            if (bar < 0) continue;
            cpus++;
            if (EfiFolder.HybridCpu(Ru, line.Substring(bar + 1)) != (line.Substring(0, bar) == "yes"))
            {
                cpuFails++;
                Console.WriteLine("      " + line.Substring(bar + 1) + ": efi.sh says " + line.Substring(0, bar));
            }
        }
        Ok(cpus >= 10 && cpuFails == 0, "hybrid CPU names: the same answer as efi.sh for all " + cpus);
        FreshEfi(Stage1);
        Directory.CreateDirectory(Oc("Kexts/Lilu.kext/Contents/MacOS"));
        Ok(!Open(null).MakePlan("driver").Text.Contains("HybridPM"), "no CPU name: no HybridPM in the plan");
        Expect(Open(null, Hybrid), "hybrid CPU", "hybridpm_needed", "yes");
        Expect(Open(null, Hybrid), "hybrid CPU", "hybridpm_package", "yes");
        Expect(Open(null, Hybrid), "hybrid CPU", "hybridpm_version_package", "9.9.8");
        Expect(Open(null, Hybrid), "hybrid CPU", "hybridpm_file", "absent");
        Expect(Open(null, Hybrid), "hybrid CPU", "hybridpm_entry", "disabled");
        Expect(Open(null, Hybrid), "hybrid CPU", "lilu_entry", "enabled");
        ComparePlan(Open(null, Hybrid).MakePlan("driver"), "hybrid-driver");
        Apply(Open(null, Hybrid), "driver");
        CompareConfig("after-hybrid-driver", Stage1);
        Ok(Util.SameFile(Oc("Kexts/HybridPM.kext/Contents/MacOS/HybridPM"), Pk("EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM")), "HybridPM.kext copied");
        Expect(Open(null, Hybrid), "hybrid, after apply", "hybridpm_file", "same");
        Expect(Open(null, Hybrid), "hybrid, after apply", "hybridpm_entry", "enabled");
        Expect(Open(null, Hybrid), "hybrid, after apply", "hybridpm_version_efi", "9.9.8");
        Expect(Open(null, Hybrid), "hybrid, after apply", "plan_changes", "0");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), stage1) && !Directory.Exists(Oc("Kexts/HybridPM.kext")), "undo: stage 1, HybridPM.kext removed");

        FreshEfi(NoHpm);
        Directory.CreateDirectory(Oc("Kexts/Lilu.kext/Contents/MacOS"));
        Expect(Open(null, Hybrid), "no HybridPM entry", "hybridpm_entry", "absent");
        ComparePlan(Open(null, Hybrid).MakePlan("driver"), "nohybridpm-driver");
        Apply(Open(null, Hybrid), "driver");
        CompareConfig("after-nohybridpm-driver", NoHpm);
        Plist ins = Plist.Load(Cfg), was = Plist.Load(NoHpm);
        Ok(Plist.Raw(ins.Get("Kernel", "Add", "0", "BundlePath")) == "Lilu.kext" && Plist.Raw(ins.Get("Kernel", "Add", "1", "BundlePath")) == "HybridPM.kext"
            && Plist.Raw(ins.Get("Kernel", "Add", "1", "Enabled")) == "true"
            && Plist.Canonical(ins.Get("Kernel", "Add", "2")) == Plist.Canonical(was.Get("Kernel", "Add", "1")), "HybridPM.kext is item 1, right below Lilu.kext, on; the others one further down");
        string ht = File.ReadAllText(Cfg);
        Ok(ht.Contains("\t\t\t</dict>\n\t\t\t<dict>\n\t\t\t\t<key>Arch</key>\n\t\t\t\t<string>Any</string>\n\t\t\t\t<key>BundlePath</key>\n\t\t\t\t<string>HybridPM.kext</string>")
            && ht.Contains("<string>Contents/Info.plist</string>\n\t\t\t</dict>\n\t\t\t<dict>\n"), "the inserted entry is indented like its neighbours");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), File.ReadAllBytes(NoHpm)), "undo: as before");
        // the plan names IntelXe.kext's entry by its index (13 here) and then inserts HybridPM's above it: the read-back must
        // check each value where it was set, not at that index afterwards (it refused the change when the entry that then
        // sits there, AirportItlwm's here, was off)
        Edit(delegate(Plist pl) { pl.Set(new[] { "Kernel", "Add", "13", "Enabled" }, PValue.Bool(false)); });
        byte[] offAbove = File.ReadAllBytes(Cfg);
        Apply(Open(null, Hybrid), "driver");
        Plist moved = Plist.Load(Cfg);
        Ok(Plist.Raw(moved.Get("Kernel", "Add", "15", "BundlePath")) == "IntelXe.kext" && Plist.Raw(moved.Get("Kernel", "Add", "15", "Enabled")) == "true"
            && Plist.Raw(moved.Get("Kernel", "Add", "14", "Enabled")) == "false", "an entry turned on and then moved down by the insert: applied, the one above still off");
        Apply(Open(null), "undo");
        Ok(SameBytes(File.ReadAllBytes(Cfg), offAbove), "undo: as before");

        FreshEfi(Stage1);
        Plan lp = Open(null, Hybrid).MakePlan("driver");
        Ok(lp.Text.Contains("this EFI does not load Lilu.kext") && !lp.Text.Contains("EFI/OC/Kexts/HybridPM.kext") && !lp.Text.Contains("item 8 (HybridPM.kext)"), "no Lilu.kext file: a note, HybridPM left alone");
        Directory.CreateDirectory(Oc("Kexts/Lilu.kext"));
        Edit(delegate(Plist pl) { pl.Set(new[] { "Kernel", "Add", "0", "Enabled" }, PValue.Bool(false)); });
        Expect(Open(null, Hybrid), "Lilu off", "lilu_entry", "disabled");
        lp = Open(null, Hybrid).MakePlan("driver");
        Ok(lp.Text.Contains("this EFI does not load Lilu.kext") && !lp.Text.Contains("EFI/OC/Kexts/HybridPM.kext"), "Lilu's entry off: the same note");
        File.WriteAllBytes(Cfg, stage1);
        string oldHpm = Oc("Kexts/HybridPM.kext");
        Directory.CreateDirectory(Path.Combine(Path.Combine(oldHpm, "Contents"), "MacOS"));
        File.WriteAllText(Path.Combine(Path.Combine(Path.Combine(oldHpm, "Contents"), "MacOS"), "HybridPM"), "org.hackintosh build\n");
        File.WriteAllText(Path.Combine(Path.Combine(oldHpm, "Contents"), "Info.plist"), InfoPlist("org.hackintosh.HybridPM", "1.0.0"));
        Expect(Open(null, Hybrid), "an earlier HybridPM build", "hybridpm_file", "different");
        Expect(Open(null, Hybrid), "an earlier HybridPM build", "hybridpm_version_efi", "1.0.0");
        Ok(Open(null, Hybrid).MakePlan("driver").Text.Contains("replace  EFI/OC/Kexts/HybridPM.kext (an earlier build, org.hackintosh.HybridPM 1.0.0, by the package's 9.9.8)"), "plan replaces the earlier build");
        Apply(Open(null, Hybrid), "driver");
        Expect(Open(null, Hybrid), "after replacing the earlier build", "hybridpm_file", "same");
        Apply(Open(null), "undo");
        Ok(File.ReadAllText(Path.Combine(Path.Combine(Path.Combine(oldHpm, "Contents"), "MacOS"), "HybridPM")).Contains("org.hackintosh build"), "undo puts the earlier build back");
        Util.Delete(oldHpm);
        Edit(delegate(Plist pl) { pl.Insert(new[] { "Kernel", "Add" }, 0, PValue.Dict("BundlePath", "HybridPM.kext", "Enabled", true, "ExecutablePath", "Contents/MacOS/HybridPM", "PlistPath", "Contents/Info.plist")); });
        Ok(Open(null, Hybrid).MakePlan("driver").Text.Contains("HybridPM.kext is item 0 of Kernel > Add, above Lilu.kext (item 1)"), "an entry above Lilu's: a note");
        File.WriteAllBytes(Cfg, stage1);
        string hexe = Pk("EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM");
        File.AppendAllText(hexe, "x");
        Ok(Throws(delegate { Apply(Open(null, Hybrid), "driver"); }, "differ from its SHA256SUMS"), "a damaged HybridPM.kext is refused");
        File.WriteAllText(hexe, "stand-in HybridPM executable\n");
        string P1 = P;
        P = Path.Combine(Work, "pkg-nohpm");
        MakePackage();
        Util.Delete(Pk("EFI/OC/Kexts/HybridPM.kext"));
        WriteSums();
        lp = Open(null, Hybrid).MakePlan("driver");
        Ok(lp.Text.Contains("this package has no HybridPM.kext") && !lp.Text.Contains("EFI/OC/Kexts/HybridPM.kext"), "a package without HybridPM.kext: a note, nothing copied");
        Expect(Open(null, Hybrid), "a package without HybridPM.kext", "hybridpm_package", "no");

        // ---- SSDT-ARCOFF adapted to the card's port (the 0.1.3 table) ----
        P = Path.Combine(Work, "pkg-aml");
        MakePackage();
        File.Copy(Aml013, Pk("EFI/OC/ACPI/SSDT-ARCOFF.aml"), true);
        WriteSums();
        FreshEfi(Stage1);
        Expect(Open(@"\_SB.PC00.PEG1.PEGP"), "the 0.1.3 table, the shipped port", "ssdt_package_names_card", "yes");
        Ok(Open(@"\_SB.PC00.PEG1.PEGP").MakePlan("driver").Text.Contains(@"copy     EFI/OC/ACPI/SSDT-ARCOFF.aml (names \_SB.PC00.PEG1.PEGP, this card's port)"), "... copied as it is");
        string[][] ports = { new[] { "rp05", @"\_SB.PC00.RP05.PXSX" }, new[] { "gpp8", @"\_SB.PCI0.GPP8" }, new[] { "deep", @"\_SB.PCI0.GPP8.UP00.DP40.UP00" } };
        foreach (string[] port in ports)
        {
            FreshEfi(Stage1);
            Expect(Open(port[1]), "the 0.1.3 table, " + port[1], "ssdt_package_names_card", "adaptable");
            ComparePlan(Open(port[1]).MakePlan("driver"), "adapt-" + port[0]);
            Apply(Open(port[1]), "driver");
            Ok(SameBytes(File.ReadAllBytes(Oc("ACPI/SSDT-ARCOFF.aml")), File.ReadAllBytes(Path.Combine(Path.Combine(X, "efi-sh"), "adapt-" + port[0] + ".aml"))),
                "adapt-" + port[0] + ": byte for byte the table efi.sh writes");
            if (port[0] == "rp05") CompareConfig("after-adapt-rp05", Stage1);
            Expect(Open(port[1]), "adapted, after apply", "ssdt_file", "same");
            Expect(Open(port[1]), "adapted, after apply", "plan_changes", "0");
            Apply(Open(null), "undo");
            Ok(SameBytes(File.ReadAllBytes(Cfg), stage1) && !File.Exists(Oc("ACPI/SSDT-ARCOFF.aml")), "undo: stage 1, the adapted table removed");
        }
        File.WriteAllBytes(Oc("ACPI/SSDT-ARCOFF.aml"), Encoding.ASCII.GetBytes("SSDT\x2f\x04_SB_PC00RP05PXSX my own build"));
        Ok(Open(@"\_SB.PC00.RP05.PXSX").MakePlan("driver").Text.Contains(@"kept: EFI/OC/ACPI/SSDT-ARCOFF.aml differs from the package's adapted to this card's port but names this card's port (\_SB.PC00.RP05.PXSX): yours stays"),
            "your own table for that port is kept");
        File.Delete(Oc("ACPI/SSDT-ARCOFF.aml"));
        P = P1;
        Expect(Open(@"\_SB.PC00.PEG1"), "the stand-in table, a shorter path", "ssdt_package_names_card", "no");
        Ok(!Open(@"\_SB.PC00.PEG1").MakePlan("driver").Text.Contains("EFI/OC/ACPI/SSDT-ARCOFF.aml ("), "a shorter path inside the package's does not count as named");

        // ---- the folder ----
        FreshEfi(Stage1);
        Ok(new EfiFolder(Path.Combine(F, "EFI"), P, Bk, Ru).EfiDir == Open(null).EfiDir && new EfiFolder(Path.Combine(F, "EFI"), P, Bk, Ru).BackupDir == Open(null).BackupDir, "the EFI folder or the folder holding it: the same EFI, the same backups");
        Ok(Throws(delegate { new EfiFolder(Work, P, Bk, Ru); }, "no OC folder"), "a folder without OC is refused");
        Ok(Throws(delegate { new EfiFolder(F, P, Path.Combine(Path.Combine(F, "EFI"), "backups"), Ru); }, "inside the EFI folder"), "backups inside the EFI folder are refused");
        Ok(new EfiFolder(F, "", Bk, Ru).MakePlan("settings").Lines.Count == 6, "stage 2 needs no package");
        Ok(Throws(delegate { new EfiFolder(F, "", Bk, Ru).MakePlan("driver"); }, "no package folder"), "stage 4 needs one");
        File.Delete(Oc("OpenCore.efi"));
        Ok(Throws(delegate { Open(null).MakePlan("settings"); }, "not an OpenCore EFI"), "without OpenCore.efi: not an OpenCore EFI");

        // ---- an unpacked -efi.zip (optional 6th argument) ----
        if (args.Length > 6)
        {
            FreshEfi(Stage1);
            var real = new EfiFolder(F, args[6], Bk, Ru);
            string version = EfiFolder.KextVersion(real.PackageKext);
            Ok(real.PackageProblem == null && real.CheckSums() == "ok", "the package " + args[6] + ": its IntelXe.kext and SSDT-ARCOFF.aml match its SHA256SUMS");
            Ok(real.MakePlan("driver").Text.Contains("copy     EFI/OC/Kexts/IntelXe.kext (version " + version + ")"), "its kext version in the plan: " + version);
        }

        Console.WriteLine("== " + fails + " failure(s)");
        return fails;
    }
}
