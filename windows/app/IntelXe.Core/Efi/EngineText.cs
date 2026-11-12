// SPDX-License-Identifier: MIT
using System.Text;
using System.Text.RegularExpressions;

namespace IntelXe.Core.Efi;

/// What kind of engine text a line is: a plan's step, a plan's note, a line of a change's output (the plan's text again,
/// the files written, the backup), or the message of a change or plan that failed.
public enum EngineLine { Step, Note, Result, Error }

/// The EFI engine's English lines in the window's words and language (D13 of docs/design/DESIGN-efi-rules.md).
///
/// The engine (IntelXe-EFI.ps1's, compiled into the Core) writes its plans in English for the script; its texts, the
/// plan's SHA-256 and the shared vectors stay as they are. This table maps each line the engine writes, by its template,
/// to a key of the app's string tables (Strings/*/Resources.resw) with the variable parts as arguments: the English
/// table words them for the app (no -CardAcpiPath, no "run this again"), the Turkish one translates them. A line no
/// template matches is shown as the engine wrote it. EngineTextTests holds every note, step and output line the engine
/// source can write to a template here.
///
/// Templates are the engine's text with {n} for each part it fills in; {n*} marks a part that is itself one of the
/// engine's phrases (a rule's "why", a backup's kind, a reason) and is translated too. A step or output line starts with
/// a verb column ("config   ", "  ok    "): the verb has its own key, and the column keeps its width when it fits.
public static class EngineText
{
    public sealed class Template
    {
        public string? Key { get; }
        public string Engine { get; }
        internal readonly Regex Re;
        internal readonly int Literal, Count;
        internal readonly HashSet<int> Phrases = [];
        internal readonly string Indent = "", Verb = "";
        internal readonly int Width;

        internal Template(string? key, string engine)
        {
            Key = key;
            Engine = engine;
            var body = engine;
            var col = VerbColumn.Match(engine);
            if (col.Success)
            {
                Indent = col.Groups[1].Value;
                Verb = col.Groups[2].Value;
                Width = col.Groups[2].Length + col.Groups[3].Length;
            }
            var re = new StringBuilder("^");
            var seen = new HashSet<int>();
            int at = 0;
            foreach (Match m in Placeholder.Matches(body))
            {
                re.Append(Regex.Escape(body[at..m.Index]));
                Literal += m.Index - at;
                int n = int.Parse(m.Groups[1].Value);
                if (m.Groups[2].Length > 0) Phrases.Add(n);
                re.Append(seen.Add(n) ? $"(?<a{n}>.*?)" : $@"\k<a{n}>");
                Count = Math.Max(Count, n + 1);
                at = m.Index + m.Length;
            }
            re.Append(Regex.Escape(body[at..])).Append('$');
            Literal += body.Length - at;
            Re = new Regex(re.ToString(), RegexOptions.CultureInvariant);
        }

        internal object[]? Match(string line)
        {
            var m = Re.Match(line);
            if (!m.Success) return null;
            var args = new object[Count];
            for (int i = 0; i < Count; i++)
            {
                var v = m.Groups["a" + i].Value;
                args[i] = Phrases.Contains(i) ? Phrase(v) : v;
            }
            return args;
        }
    }

    static readonly Regex Placeholder = new(@"\{(\d+)(\*?)\}");
    static readonly Regex VerbColumn = new(@"^( *)([a-z]+)( {2,})");

    static Template T(string? key, string engine) => new(key, engine);

    // Most literal text first, so that the most specific template wins (OrderByDescending keeps the written order of
    // equals).
    static IReadOnlyList<Template> Sorted(params Template[] t) => t.OrderByDescending(x => x.Literal).ToList();

    /// The plan's steps (EfiFolder.PlanFields, PlanFiles, PlanHybridPM, PlanItems, PlanUndo).
    public static IReadOnlyList<Template> Steps { get; } = Sorted(
        T("Eng_Step_FieldBool", "config   {0}, item {1} ({2}): {3} {4} -> {5}"),
        T("Eng_Step_FieldString", "config   {0}, item {1} ({2}): {3} '{4}' -> '{5}'"),
        T("Eng_Step_Copy", "copy     EFI/OC/{0} (version {1})"),
        T("Eng_Step_Replace", "replace  EFI/OC/{0} (version {1} in the EFI, {2} in the package; the files differ)"),
        T("Eng_Step_AddEntry", "config   Kernel > Add: add {0} ({1})"),
        T("Eng_Step_DisableEarlier", "config   Kernel > Add, item {0} ({1}, the earlier name of {2}): Enabled true -> false (only one of the two may load)"),
        T("Eng_Step_SsdtAdapted", "copy     EFI/OC/{0} (the package's table adapted to this card's port: names {1} instead of {2})"),
        T("Eng_Step_SsdtAdapted", "replace  EFI/OC/{0} (the package's table adapted to this card's port: names {1} instead of {2})"),
        T("Eng_Step_SsdtNames", "copy     EFI/OC/{0} (names {1}, this card's port)"),
        T("Eng_Step_SsdtNames", "replace  EFI/OC/{0} (names {1}, this card's port)"),
        T("Eng_Step_AddSsdt", "config   ACPI > Add: add {0} (Enabled)"),
        T("Eng_Step_BootPath", "config   {0}: {1} -> {2} (OpenCore then records which EFI it booted from, for efi.sh on macOS)"),
        T("Eng_Step_HpmCopy", "copy     EFI/OC/{0} (version {1}; this CPU has efficiency cores: it corrects their clocks)"),
        T("Eng_Step_HpmEarlier", "replace  EFI/OC/{0} (an earlier build, {1} {2}, by the package's {3})"),
        T("Eng_Step_HpmEntry", "config   Kernel > Add: add {0} below {1}, as item {2} ({3})"),
        T("Eng_Step_ValueWhy", "config   {0}: {1*} -> {2} ({3*})"),
        T("Eng_Step_Value", "config   {0}: {1*} -> {2}"),
        T("Eng_Step_BitsWhy", "config   {0}: 0x{1} -> 0x{2} ({3*})"),
        T("Eng_Step_Bits", "config   {0}: 0x{1} -> 0x{2}"),
        T("Eng_Step_BootArgs", "config   {0}: add {1} ('{2}' -> '{3}')"),
        T("Eng_Step_AddMissing", "config   {0}: add (missing)"),
        T("Eng_Step_AddStringWhy", "config   {0}: add {1} ({2*})"),
        T("Eng_Step_AddString", "config   {0}: add {1}"),
        T("Eng_Step_UndoFrom", "from     {0} ({1*}, {2})"),
        T("Eng_Step_Restore", "restore  EFI/OC/{0}"),
        T("Eng_Step_Remove", "remove   EFI/OC/{0} (it was not there before)"));

    /// The plan's notes (EfiFolder.PlanFiles, PlanHybridPM, PlanItems' "require" and "note" items of the rules, PlanUndo).
    public static IReadOnlyList<Template> Notes { get; } = Sorted(
        T("Eng_Note_NoSums", "the package has no SHA256SUMS: its files are not checked"),
        T("Eng_Note_SsdtUnconfirmed", "not added: {0}. Add it only after macOS is installed and install.sh --apply has run (stage 3), and only for your card's ACPI path: the package's table is for {1} (for another path it is adapted to that one). Check it in Device Manager (the B580 > Properties > Details > Location paths), then confirm it (-CardAcpiPath, or the SSDT-ARCOFF box in the window); stage 4, step 2 of README-INSTALL.md.{2*}"),
        T("Eng_Note_SsdtNoName", "not added: {0}. The B580's upstream port has no ACPI name on this board; the table needs one more bridge level (stage 4, step 2 of README-INSTALL.md; please open an issue)"),
        T("Eng_Note_SsdtNotAdapted", "not added: {0}. The package's table is for {1}; the card's port here is {2}, and {3*}. Put that path into SSDT-ARCOFF.dsl, compile it with iasl and copy it to EFI/OC/{4} (stage 4, step 2 of README-INSTALL.md); then this adds its entry"),
        T("Eng_Note_SsdtKept", "kept: EFI/OC/{0} differs from {1*} but names this card's port ({2}): yours stays"),
        T("Eng_Note_HpmNotInPackage", "not added: {0}. This CPU ({1}) has efficiency cores and macOS holds it below its clocks, but this package has no {2} (it comes with 0.1.4 and later)"),
        T("Eng_Note_HpmNoLilu", "not added: {0}. This CPU ({1}) has efficiency cores and {2} corrects its clocks, but it is a Lilu plugin and this EFI does not load {3} (no file in EFI/OC/{4}, or its Kernel > Add entry is missing or off). Add Lilu (github.com/acidanthera/Lilu), then run this again (README-INSTALL.md, Hybrid Intel CPUs)"),
        T("Eng_Note_HpmAboveLilu", "{0} is item {1} of Kernel > Add, above {2} (item {3}): OpenCore loads it before Lilu and it does not start. Move its entry below {2}'s"),
        T("Eng_Note_Missing", "config.plist has no {0}: not changed"),
        T("Eng_Note_NoBackup", "no backup to put back for this EFI"),
        T("Eng_Note_UndoOther", "the backup lists EFI/OC/{0}, which these rules never change: left as it is"),
        T("Eng_Note_UndoLater", "config.plist was changed after that change: putting it back loses those later edits"),
        T("Eng_Note_BootLogSlow", "boot logging slows every boot: OpenCore rewrites its whole 256 KB log file after every line, which adds seconds from an internal SSD and can add tens of seconds or more from a USB stick. Undo this change once the logs are collected"));

    /// The output of a change besides the plan's lines (EfiFolder.Run, RunUndo, Backup; the helper's warning).
    public static IReadOnlyList<Template> Results { get; } = Sorted(
        T("Eng_Res_Where", "EFI: the folder {0}"),
        T("Eng_Res_NothingToChange", "nothing to change"),
        T("Eng_Res_Backup", "== backup of the whole EFI folder: {0}"),
        T(null, "  ok    EFI/OC/{0}"),
        T("Eng_Res_Restored", "  ok    restored EFI/OC/{0}"),
        T("Eng_Res_Removed", "  ok    removed EFI/OC/{0}"),
        T("Eng_Res_Done", "== done. Restart to use it. To go back: Undo (one change per run); the backup stays in {0}"),
        T("Eng_Res_Nothing", "== nothing to change"),
        T("Eng_Res_Undone", "== done. The backup stays in {0}."),
        T("Eng_Res_StillMounted", "warning  the EFI partition stays mounted at {0}: {1}"));

    /// The messages of a plan or change that could not be made (the engine's EfiException, the helper's).
    public static IReadOnlyList<Template> Errors { get; } = Sorted(
        T("Eng_Err_NotOpenCore", "no OC/OpenCore.efi and OC/config.plist in {0}: not an OpenCore EFI"),
        T("Eng_Err_NoOc", "no OC folder in {0}: choose the EFI folder of your OpenCore EFI (the one that holds OC and BOOT)"),
        T("Eng_Err_NoFolder", "the folder {0} does not exist"),
        T("Eng_Err_CannotRead", "cannot read {0}: {1}"),
        T("Eng_Err_NoPackage", "no package folder given (the unpacked intel-gpu-macos-driver-VERSION-efi.zip)"),
        T("Eng_Err_PackageFiles", "no EFI/OC/{0} and EFI/OC/{1} in {2}"),
        T("Eng_Err_NotAdmin", "cannot write to {0}. An EFI partition mounted with mountvol can be written only by an administrator: start PowerShell as administrator (or use the window's \"Restart as administrator\")"),
        T("Eng_Err_CannotWrite", "cannot write to {0}: {1}"),
        T("Eng_Err_BackupSpace", "not enough space for a backup of the EFI folder ({0}) in {1}: choose another backup folder. Nothing changed"),
        T("Eng_Err_BackupIncomplete", "the backup in {0} is incomplete ({1} of {2} files): nothing changed in the EFI"),
        T("Eng_Err_Sums", "the package's EFI files differ from its SHA256SUMS (damaged or changed): nothing changed ({0})"),
        T("Eng_Err_Changed", "the EFI changed since the plan was shown (plan {0}, expected {1}): nothing changed"),
        T("Eng_Err_NotHeld", "the edited config.plist does not hold {0} = {1}: nothing changed"),
        T("Eng_Err_ReadBack", "the edited config.plist does not read back as edited: nothing changed"),
        T("Eng_Err_EfiSpace", "not enough space on the EFI's volume ({0} free): nothing changed"),
        T("Eng_Err_NoBackup", "no backup to put back for this EFI (backups: {0})"),
        T("Eng_Err_OtherLetter", "the EFI partition was mounted at {0}, not at {1} as for the plan: nothing changed. Show the plan again"),
        T("Eng_Err_NotEsp", "disk {0} partition {1} is not an EFI system partition: not mounted"),
        T("Eng_Err_NoLetter", "no free drive letter between M: and Z:"));

    /// The engine's phrases inside other lines: the rules' "why" texts (efi-rules.json), the backups' kinds
    /// (backup_kinds), why a table could not be adapted (Aml.Adapt), what Windows reports for the card, "missing".
    public static IReadOnlyList<Template> Fragments { get; } = Sorted(
        T("Eng_Why_Bars", "a 1 GB window on the B580's VRAM"),
        T("Eng_Why_Reconnect", "with true, OpenCore's RELEASE build boots without a console and watchdogd panics"),
        T("Eng_Why_Csr", "SIP lets unsigned kexts load from the Auxiliary Kernel Collection"),
        T("Eng_Why_Delete", "so that the new value replaces the old one at boot"),
        T("Eng_Why_SecureBoot", "Apple Secure Boot refuses a system volume with a changed seal: needed for the system-volume copy, Safari on the GPU"),
        T("Eng_Why_LogTarget", "logging on, to a file: opencore-<date>.txt in the root of the EFI volume OpenCore starts from"),
        T("Eng_Why_LogDisplay", "errors, warnings and info; this only filters the onscreen log, and info lines exist only in DEBUG builds"),
        T("Eng_Why_AppleDebug", "the macOS boot loader's log goes into OpenCore's log, macOS 10.15.4 and later"),
        T("Eng_Why_ApplePanic", "the next boot after a kernel panic saves it as panic-<date>.txt in the root of the EFI volume"),
        T("Eng_Kind_Driver", "IntelXe files"),
        T("Eng_Kind_Settings", "settings"),
        T("Eng_Kind_Safari", "Safari setting"),
        T("Eng_Kind_SettingsSafari", "settings and Safari setting"),
        T("Eng_Kind_BootLog", "boot logging"),
        T("Eng_Kind_Change", "change"),
        T("Eng_Missing", "missing"),
        T("Eng_Seen_NoName", "Windows reports a Location path for the B580 here in which the card's port has no ACPI name."),
        T("Eng_Seen_Port", "Windows reports the card's port here as {0}."),
        T("Eng_Adapt_NotPath", "{0} is not an ACPI path this script can write into the table"),
        T("Eng_Adapt_Layout", "the package's {0} is not laid out as SSDT-ARCOFF.dsl builds it"),
        T("Eng_Adapt_ReadBack", "the table built for {0} did not read back as built"),
        T("Eng_Kept_Adapted", "the package's adapted to this card's port"),
        T("Eng_Kept_Package", "the package's"));

    /// The keys of the verb column.
    public static IReadOnlyDictionary<string, string> Verbs { get; } = new Dictionary<string, string>(StringComparer.Ordinal)
    {
        ["config"] = "Eng_Verb_Config", ["copy"] = "Eng_Verb_Copy", ["replace"] = "Eng_Verb_Replace",
        ["restore"] = "Eng_Verb_Restore", ["remove"] = "Eng_Verb_Remove", ["from"] = "Eng_Verb_From",
        ["note"] = "Eng_Verb_Note", ["ok"] = "Eng_Verb_Ok", ["warning"] = "Eng_Verb_Warning",
    };

    /// Every key this table names (the app's string tables must have them all).
    public static IReadOnlyList<string> Keys { get; } =
        Steps.Concat(Notes).Concat(Results).Concat(Errors).Concat(Fragments)
            .Select(t => t.Key).OfType<string>().Concat(Verbs.Values).Distinct().Order(StringComparer.Ordinal).ToList();

    static IReadOnlyList<Template> Set(EngineLine kind) => kind switch
    {
        EngineLine.Step => Steps,
        EngineLine.Note => Notes,
        EngineLine.Result => Results,
        _ => Errors,
    };

    /// A phrase inside a line: its key, or the text as it is.
    static object Phrase(string s)
    {
        var t = s.Trim();
        foreach (var f in Fragments)
            if (f.Match(t) is { } args) return Text.Of(f.Key!, args);
        return s;
    }

    /// The template a line matches and its arguments, or null.
    public static (Template Template, object[] Args)? Match(EngineLine kind, string line)
    {
        foreach (var t in Set(kind))
            if (t.Match(line) is { } args) return (t, args);
        return null;
    }

    /// True when the line has a template: the window shows it in its own words.
    public static bool IsKnown(EngineLine kind, string line) => Render(kind, line, _ => "") is not null;

    /// The line in the window's words and language (resolve: a key with its arguments in the language shown), or the
    /// engine's text when no template matches.
    public static string Show(EngineLine kind, string line, Func<Text, string> resolve) => Render(kind, line, resolve) ?? line;

    static string? Render(EngineLine kind, string line, Func<Text, string> resolve)
    {
        line = line.TrimEnd('\r');
        if (kind == EngineLine.Result)
        {
            // a change's output repeats the plan's text: its steps and its "note     ..." lines
            if (Match(EngineLine.Result, line) is { } r) return Compose(r.Template, r.Args, resolve);
            if (line.StartsWith("note     ", StringComparison.Ordinal))
                return Render(EngineLine.Note, line[9..], resolve) is { } note ? Column("", "note", 9, resolve) + note : null;
            return Render(EngineLine.Step, line, resolve);
        }
        return Match(kind, line) is { } m ? Compose(m.Template, m.Args, resolve) : null;
    }

    static string Column(string indent, string verb, int width, Func<Text, string> resolve)
    {
        var shown = Verbs.TryGetValue(verb, out var key) ? resolve(Text.Of(key)) : verb;
        if (shown.Length == 0) shown = verb;
        return indent + shown + new string(' ', Math.Max(1, width - shown.Length));
    }

    static string Compose(Template t, object[] args, Func<Text, string> resolve)
    {
        var head = t.Verb.Length > 0 ? Column(t.Indent, t.Verb, t.Width, resolve) : "";
        string body;
        if (t.Key is not null) body = resolve(Text.Of(t.Key, args));
        else
        {
            // no key: the rest stays as the engine wrote it (a path)
            var rest = VerbColumn.Replace(t.Engine, "");
            body = Placeholder.Replace(rest, m => args[int.Parse(m.Groups[1].Value)] is string s ? s : m.Value);
        }
        return head + body;
    }
}
