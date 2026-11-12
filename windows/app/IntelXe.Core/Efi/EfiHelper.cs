// SPDX-License-Identifier: MIT
// The elevated helper's side, without Windows APIs: its command line, what it does (mount, read or plan or change,
// unmount) and its answer. The app starts its own exe with "--efi-helper" through "runas" for an EFI partition: only
// that process has administrator rights, and only for one mount + plan, or mount + backup + change + unmount. A change
// is given the plan's SHA-256 (--expect) and refuses when the EFI no longer gives that plan. The mounting itself is
// IEspMounter (mountvol in the app; a fake in the tests).
using System.Globalization;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using IntelXeEfi;

namespace IntelXe.Core.Efi;

/// An EFI partition mounted for the helper: its drive letter ("S:"), the folder that holds its EFI folder ("S:\"), and
/// whether the helper mounted it (then it unmounts it again; one that had a letter already stays as it was).
public sealed record MountedEsp(string Letter, string Root, bool MountedHere);

/// Mounting an EFI partition by its disk and partition number: at the letter asked for (the one the plan was made at),
/// or a free one; it must refuse a partition that is not an EFI system partition.
public interface IEspMounter
{
    MountedEsp Mount(uint disk, uint partition, string? letter);
    void Unmount(MountedEsp esp);
}

public enum EfiHelperVerb { Status, Plan, Apply }

/// One helper run: the verb, the EFI (a folder, or a partition with the letter of the plan), the change, the plan's hash
/// for Apply, the options, and the file the answer goes to.
public sealed record EfiHelperRequest(EfiHelperVerb Verb, EfiTarget Target, EfiChange Change, EfiOptions Options,
    string? Letter = null, string? Expect = null, string? Out = null)
{
    public const string Switch = "--efi-helper";

    public IReadOnlyList<string> ToArgs()
    {
        var a = new List<string> { Switch, Verb.ToString().ToLowerInvariant() };
        if (Verb != EfiHelperVerb.Status) a.AddRange(["--change", Change.Kind()]);
        switch (Target)
        {
            case EfiTarget.Folder f: a.AddRange(["--folder", f.Path]); break;
            case EfiTarget.Partition p:
                a.AddRange(["--disk", p.DiskNumber.ToString(CultureInfo.InvariantCulture), "--partition", p.PartitionNumber.ToString(CultureInfo.InvariantCulture)]);
                break;
        }
        void Add(string name, string? value) { if (!string.IsNullOrEmpty(value)) a.AddRange([name, value]); }
        Add("--letter", Letter);
        Add("--expect", Expect);
        Add("--package", Options.PackageDir);
        Add("--card", Options.CardAcpiPath);
        Add("--detected", Options.CardDetected);
        Add("--cpu", Options.Cpu);
        Add("--backup-root", Options.BackupRoot);
        if (Options.BootPathRecorded) a.Add("--boot-path-recorded");
        Add("--out", Out);
        return a;
    }

    /// The helper's command line (args[0] is --efi-helper); an error names what is wrong.
    public static EfiHelperRequest Parse(IReadOnlyList<string> args)
    {
        if (args.Count < 2 || args[0] != Switch) throw new EfiException("the helper's command line starts with " + Switch + " status|plan|apply");
        var verb = args[1] switch
        {
            "status" => EfiHelperVerb.Status,
            "plan" => EfiHelperVerb.Plan,
            "apply" => EfiHelperVerb.Apply,
            _ => throw new EfiException("no helper verb " + args[1] + " (status, plan, apply)"),
        };
        var values = new Dictionary<string, string>(StringComparer.Ordinal);
        bool bootPath = false;
        string[] known = ["--change", "--folder", "--disk", "--partition", "--letter", "--expect", "--package", "--card", "--detected", "--cpu", "--backup-root", "--out"];
        for (int i = 2; i < args.Count; i++)
        {
            if (args[i] == "--boot-path-recorded") { bootPath = true; continue; }
            if (!known.Contains(args[i]) || i + 1 >= args.Count) throw new EfiException("the helper does not take " + args[i] + (i + 1 >= args.Count ? " without a value" : ""));
            if (!values.TryAdd(args[i], args[i + 1])) throw new EfiException("the helper takes " + args[i] + " once");
            i++;
        }
        string? V(string k) => values.TryGetValue(k, out var v) ? v : null;
        EfiTarget target;
        if (V("--folder") is { } folder)
        {
            if (V("--disk") is not null || V("--partition") is not null) throw new EfiException("the helper takes --folder or --disk and --partition, not both");
            target = new EfiTarget.Folder(folder);
        }
        else if (uint.TryParse(V("--disk"), NumberStyles.None, CultureInfo.InvariantCulture, out var disk)
                 && uint.TryParse(V("--partition"), NumberStyles.None, CultureInfo.InvariantCulture, out var part))
            target = new EfiTarget.Partition(disk, part);
        else throw new EfiException("the helper needs --folder, or --disk and --partition (numbers)");
        var change = EfiChange.Settings;
        if (verb != EfiHelperVerb.Status)
            change = EfiChanges.FromKind(V("--change")) ?? throw new EfiException("the helper needs --change settings|safari|settings-safari|driver|undo");
        if (verb == EfiHelperVerb.Apply && string.IsNullOrEmpty(V("--expect"))) throw new EfiException("nothing changed: the helper needs the plan's SHA-256 (--expect)");
        var letter = V("--letter");
        if (letter is not null && !IsLetter(letter)) throw new EfiException("not a drive letter: " + letter);
        var options = new EfiOptions(V("--package") ?? "", V("--card") ?? "", V("--detected") ?? "", V("--cpu") ?? "", bootPath, V("--backup-root"));
        return new EfiHelperRequest(verb, target, change, options, letter?.ToUpperInvariant(), V("--expect"), V("--out"));
    }

    public static bool IsLetter(string s) => s.Length == 2 && char.IsAsciiLetter(s[0]) && s[1] == ':';
}

/// The helper's answer, written as JSON to the request's --out file: what it read, planned or did, or why it could not.
public sealed class EfiHelperResult
{
    [JsonPropertyName("ok")] public bool Ok { get; set; }
    [JsonPropertyName("error")] public string? Error { get; set; }
    [JsonPropertyName("letter")] public string? Letter { get; set; }
    [JsonPropertyName("status")] public Dictionary<string, string>? Status { get; set; }
    [JsonPropertyName("plan")] public PlanData? Plan { get; set; }
    [JsonPropertyName("lines")] public List<string>? Lines { get; set; }
    /// What went wrong after the work was done (the partition could not be unmounted): the answer stands, with this.
    [JsonPropertyName("warning")] public string? Warning { get; set; }

    public sealed class PlanData
    {
        [JsonPropertyName("change")] public string Change { get; set; } = "";
        [JsonPropertyName("text")] public string Text { get; set; } = "";
        [JsonPropertyName("lines")] public List<string> Lines { get; set; } = [];
        [JsonPropertyName("notes")] public List<string> Notes { get; set; } = [];
        [JsonPropertyName("hash")] public string Hash { get; set; } = "";
        [JsonPropertyName("has_changes")] public bool HasChanges { get; set; }
        [JsonPropertyName("backup_dir")] public string BackupDir { get; set; } = "";
    }

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };

    public string ToJson() => JsonSerializer.Serialize(this, Json);
    public static EfiHelperResult FromJson(string json) => JsonSerializer.Deserialize<EfiHelperResult>(json, Json)
        ?? throw new EfiException("the helper's answer is empty");

    public EfiPlan ToPlan(EfiTarget target)
    {
        var p = Plan ?? throw new EfiException("the helper's answer has no plan");
        var change = EfiChanges.FromKind(p.Change) ?? throw new EfiException("the helper's answer names no change");
        return new EfiPlan(target, change, p.Text, p.Lines, p.Notes, p.Hash, p.HasChanges, p.BackupDir, Letter);
    }

    public EfiStatus ToStatus() => new(Status?.GetValueOrDefault("efi_readable") == "yes", Status ?? new Dictionary<string, string>());
}

public static class EfiHelper
{
    /// Does what the request says: for a partition, mount it (at the plan's letter for a change), work on its EFI folder,
    /// and unmount it again whatever happened (when this run mounted it).
    public static EfiHelperResult Run(EfiHelperRequest q, IEspMounter? mounter, Rules? rules = null)
    {
        var r = new EfiHelperResult();
        try
        {
            MountedEsp? esp = null;
            string efi;
            if (q.Target is EfiTarget.Folder f) efi = f.Path;
            else
            {
                var p = (EfiTarget.Partition)q.Target;
                if (mounter is null) throw new EfiException("this helper cannot mount an EFI partition");
                esp = mounter.Mount(p.DiskNumber, p.PartitionNumber, q.Letter);
                r.Letter = esp.Letter;
                efi = esp.Root;
            }
            try
            {
                // the plan names the EFI by its path: a change must see it at the same letter
                if (esp is not null && q.Verb == EfiHelperVerb.Apply && q.Letter is not null && !string.Equals(esp.Letter, q.Letter, StringComparison.OrdinalIgnoreCase))
                    throw new EfiException($"the EFI partition was mounted at {esp.Letter}, not at {q.Letter} as for the plan: nothing changed. Show the plan again");
                switch (q.Verb)
                {
                    case EfiHelperVerb.Status:
                        r.Status = new Dictionary<string, string>(EfiWork.Status(efi, q.Options, rules).Values);
                        break;
                    case EfiHelperVerb.Plan:
                        var plan = EfiWork.Plan(q.Target, efi, q.Change, q.Options, rules, esp?.Letter);
                        r.Plan = new EfiHelperResult.PlanData
                        {
                            Change = plan.Change.Kind(), Text = plan.Text, Lines = [.. plan.Lines], Notes = [.. plan.Notes],
                            Hash = plan.Hash, HasChanges = plan.HasChanges, BackupDir = plan.BackupDir,
                        };
                        break;
                    case EfiHelperVerb.Apply:
                        r.Lines = [.. EfiWork.Apply(efi, q.Change, q.Expect ?? "", q.Options, rules).Lines];
                        break;
                }
                r.Ok = true;
            }
            finally
            {
                // a change that was made stays reported as made when only the unmounting fails
                if (esp is { MountedHere: true })
                    try { mounter!.Unmount(esp); }
                    catch (Exception e) { r.Warning = $"the EFI partition stays mounted at {esp.Letter}: {Message(e)}"; }
            }
        }
        catch (Exception e)
        {
            r.Ok = false;
            r.Error = Message(e);
        }
        return r;
    }

    /// The engine's own message, not a wrapper's.
    public static string Message(Exception e)
    {
        while (e is AggregateException { InnerException: { } inner }) e = inner;
        return e.Message;
    }
}

/// A Windows command line from separate arguments, quoted so that the C runtime's and .NET's parsers give them back as
/// they were (backslashes before a quote doubled; a path ending in a backslash keeps it).
public static class CommandLine
{
    public static string Join(IEnumerable<string> args) => string.Join(" ", args.Select(Quote));

    public static string Quote(string arg)
    {
        if (arg.Length > 0 && arg.IndexOfAny([' ', '\t', '\n', '\v', '"']) < 0) return arg;
        var sb = new StringBuilder("\"");
        int slashes = 0;
        foreach (char c in arg)
        {
            if (c == '\\') { slashes++; continue; }
            if (c == '"') { sb.Append('\\', 2 * slashes + 1).Append('"'); slashes = 0; continue; }
            sb.Append('\\', slashes).Append(c);
            slashes = 0;
        }
        sb.Append('\\', 2 * slashes).Append('"');
        return sb.ToString();
    }
}
