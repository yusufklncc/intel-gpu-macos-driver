// SPDX-License-Identifier: MIT
// Writing the macOS installer stick, without Windows APIs: the elevated helper's command line and answer, and the order
// of the work. The app starts its own exe with "--stick-helper write ..." through "runas" (as the EFI page does with
// --efi-helper); only that process has administrator rights, for one disk. It checks the recovery files against
// Apple's chunklist, reads the disk again and refuses it by StickRules, compares it with the one the user chose (serial
// and size, as a salted hash), reads it once more right before erasing, then erases it into one FAT32 partition
// (IStickSystem: Windows' storage API in the app, a fake in the tests), copies com.apple.recovery.boot\ and the chosen
// EFI folder, and reads the copies back against the chunklist and the EFI's hashes.
//
// GPT, not MBR: the stick is only ever started by UEFI firmware (macOS needs CSM off, so the PC boots UEFI only), UEFI
// reads a FAT32 partition on GPT as well as on MBR, and GPT is what Dortania's Windows guide makes ("convert gpt";
// it notes that MBR sticks failed to boot with one of the tools it used). One FAT32 partition of up to 16 GiB, because
// Windows formats FAT32 only up to 32 GB.
using System.Globalization;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IntelXe.Core.Stick;

public sealed class StickException(string message) : Exception(message);

public enum StickVerb { List, Check, Write, Download }

/// One helper run. List: the disks the page offers (with their identity tokens for Salt). Check: everything Write does
/// before erasing, and nothing after. Write: erase and write. Download: the recovery files (no administrator rights).
/// EraseEfi: the user confirmed that the disk holds an EFI and that it is erased (StickRules.HoldsEfi); AllowLarge: a
/// disk larger than 128 GB is meant. Both are switches without a value.
public sealed record StickHelperRequest(StickVerb Verb, uint Disk = 0, string Id = "", string Salt = "", ulong Size = 0, string Image = "",
    string? Efi = null, string? TestVhdx = null, string? Version = null, string? Out = null, string? Progress = null,
    bool EraseEfi = false, bool AllowLarge = false)
{
    public const string Switch = "--stick-helper";
    public const string Label = "MACOS";

    public IReadOnlyList<string> ToArgs()
    {
        var a = new List<string> { Switch, Verb.ToString().ToLowerInvariant() };
        void Add(string name, string? value) { if (!string.IsNullOrEmpty(value)) a.AddRange([name, value]); }
        if (Verb is StickVerb.Check or StickVerb.Write)
        {
            a.AddRange(["--disk", Disk.ToString(CultureInfo.InvariantCulture), "--size", Size.ToString(CultureInfo.InvariantCulture)]);
            Add("--id", Id);
            Add("--image", Image);
            Add("--efi", Efi);
            if (EraseEfi) a.Add("--erase-efi");
            if (AllowLarge) a.Add("--allow-large");
        }
        if (Verb is StickVerb.List or StickVerb.Check or StickVerb.Write) Add("--salt", Salt);
        if (Verb == StickVerb.Download) { Add("--version", Version); Add("--image", Image); }
        Add("--test-vhdx", TestVhdx);
        Add("--progress", Progress);
        Add("--out", Out);
        return a;
    }

    public static StickHelperRequest Parse(IReadOnlyList<string> args)
    {
        if (args.Count < 2 || args[0] != Switch) throw new StickException("the helper's command line starts with " + Switch + " list|check|write|download");
        var verb = args[1] switch
        {
            "list" => StickVerb.List, "check" => StickVerb.Check, "write" => StickVerb.Write, "download" => StickVerb.Download,
            _ => throw new StickException("no helper verb " + args[1] + " (list, check, write, download)"),
        };
        string[] known = ["--disk", "--id", "--salt", "--size", "--image", "--efi", "--test-vhdx", "--version", "--out", "--progress"];
        var v = HelperArgs.Parse(args, 2, known, ["--erase-efi", "--allow-large"], m => new StickException(m));
        string? V(string k) => v.TryGetValue(k, out var s) ? s : null;
        var q = new StickHelperRequest(verb, Salt: V("--salt") ?? "", Image: V("--image") ?? "", Efi: V("--efi"), TestVhdx: V("--test-vhdx"),
            Version: V("--version"), Out: V("--out"), Progress: V("--progress"), EraseEfi: v.ContainsKey("--erase-efi"), AllowLarge: v.ContainsKey("--allow-large"));
        if (verb is StickVerb.Check or StickVerb.Write)
        {
            if (!uint.TryParse(V("--disk"), NumberStyles.None, CultureInfo.InvariantCulture, out var disk)) throw new StickException("the helper needs --disk N");
            if (!ulong.TryParse(V("--size"), NumberStyles.None, CultureInfo.InvariantCulture, out var size)) throw new StickException("the helper needs --size BYTES");
            if (string.IsNullOrEmpty(V("--id")) || string.IsNullOrEmpty(q.Salt)) throw new StickException("nothing written: the helper needs the disk's identity (--id, --salt)");
            if (string.IsNullOrEmpty(q.Image)) throw new StickException("the helper needs --image FOLDER (the recovery download)");
            q = q with { Disk = disk, Size = size, Id = V("--id")! };
        }
        if (verb == StickVerb.Download && (string.IsNullOrEmpty(q.Version) || string.IsNullOrEmpty(q.Image)))
            throw new StickException("the helper needs --version ID and --image FOLDER");
        return q;
    }
}

/// A disk in the helper's list answer: what the page shows, the refusals, and the identity token for the request's salt.
public sealed class StickDiskInfo
{
    [JsonPropertyName("number")] public uint Number { get; set; }
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("size")] public ulong Size { get; set; }
    [JsonPropertyName("bus")] public string Bus { get; set; } = "";
    [JsonPropertyName("refusals")] public List<string> Refusals { get; set; } = [];
    [JsonPropertyName("id")] public string? Id { get; set; }
}

public sealed class StickHelperResult
{
    [JsonPropertyName("ok")] public bool Ok { get; set; }
    [JsonPropertyName("error")] public string? Error { get; set; }
    [JsonPropertyName("refusals")] public List<string>? Refusals { get; set; }
    [JsonPropertyName("lines")] public List<string> Lines { get; set; } = [];
    [JsonPropertyName("disks")] public List<StickDiskInfo>? Disks { get; set; }
    /// whether anything on the disk was changed (false for a refusal, a check, a failure before erasing)
    [JsonPropertyName("written")] public bool Written { get; set; }

    static readonly JsonSerializerOptions Json = new() { WriteIndented = true, DefaultIgnoreCondition = JsonIgnoreCondition.WhenWritingNull };
    public string ToJson() => JsonSerializer.Serialize(this, Json);
    public static StickHelperResult FromJson(string json) => JsonSerializer.Deserialize<StickHelperResult>(json, Json) ?? throw new StickException("the helper's answer is empty");
}

/// A helper's "--name value" pairs and value-less switches; each once.
public static class HelperArgs
{
    public static Dictionary<string, string> Parse(IReadOnlyList<string> args, int start, IReadOnlyCollection<string> withValue,
        IReadOnlyCollection<string> switches, Func<string, Exception> error)
    {
        var v = new Dictionary<string, string>(StringComparer.Ordinal);
        for (int i = start; i < args.Count; i++)
        {
            var name = args[i];
            string value;
            if (switches.Contains(name)) value = "";
            else if (withValue.Contains(name) && i + 1 < args.Count) value = args[++i];
            else throw error("the helper does not take " + name + (withValue.Contains(name) ? " without a value" : ""));
            if (!v.TryAdd(name, value)) throw error("the helper takes " + name + " once");
        }
        return v;
    }
}

/// The disks as Windows reports them: the list, and one disk read again.
public interface IStickDisks
{
    IReadOnlyList<StickDisk> ListDisks();
    /// The disk as it is now; null when there is no such disk.
    StickDisk? Inspect(uint number);
}

/// What the writer needs from Windows: the disks, and erasing one into a single FAT32 partition.
public interface IStickSystem : IStickDisks
{
    /// Erases the disk (checked by the caller just before), makes it GPT with one FAT32 partition of partitionSize
    /// bytes labelled label, and returns the partition's root ("E:\").
    string EraseAndFormat(StickDisk disk, ulong partitionSize, string label);
}

public static class StickWriter
{
    /// The disks to list, with their refusals and tokens.
    public static StickHelperResult List(IStickSystem sys, StickPolicy policy, string salt)
    {
        var r = new StickHelperResult { Ok = true, Disks = [] };
        foreach (var d in sys.ListDisks().Where(d => StickRules.Listed(d, policy)).OrderBy(d => d.Number))
            r.Disks.Add(new StickDiskInfo
            {
                Number = d.Number, Name = d.FriendlyName, Size = d.Size, Bus = d.BusName, Refusals = [.. StickRules.Refusals(d, policy)],
                Id = salt.Length > 0 && d.HasIdentity ? d.Token(salt) : null,
            });
        return r;
    }

    /// Check or Write. progress: a stage name and a fraction 0..1. protectedLetters: drives that must not be erased
    /// besides the rules' (where the app and the files are).
    public static StickHelperResult Run(StickHelperRequest q, IStickSystem sys, Action<string, double>? progress = null,
        IReadOnlyList<string>? protectedLetters = null, System.Security.Cryptography.RSAParameters? key = null, CancellationToken cancel = default)
    {
        var r = new StickHelperResult();
        try
        {
            if (q.Verb is not (StickVerb.Check or StickVerb.Write)) throw new StickException("Run takes check or write");

            // the files first: nothing is erased for files that would not boot
            var files = RecoveryFiles.Find(q.Image, key) ?? throw new StickException($"no checked recovery download in {q.Image}: download it again");
            var list = Chunklist.Parse(File.ReadAllBytes(files.ChunklistFile), key);
            progress?.Invoke("verify-source", 0);
            list.VerifyFile(files.Image, b => progress?.Invoke("verify-source", (double)b / list.ImageSize), cancel);
            r.Lines.Add($"recovery image {Path.GetFileName(files.Image)}: {list.Chunks.Count} pieces match Apple's signed chunklist");
            string? efi = q.Efi is { Length: > 0 } ? EfiFolderCheck.Resolve(q.Efi) : null;
            if (efi is not null) r.Lines.Add("EFI folder " + efi + ": OpenCore found");

            var letters = new List<string>(protectedLetters ?? []);
            foreach (var p in new[] { q.Image, efi })
                if (p is { Length: >= 2 } && p[1] == ':') letters.Add(p[..2]);
            var policy = new StickPolicy(StickRules.MinimumFor(list.ImageSize + (efi is null ? 0 : DirSize(efi))), q.TestVhdx, letters, q.AllowLarge);

            var disk = Checked(sys, q, policy, r);
            if (disk is null) return r;
            r.Lines.Add($"disk {disk.Number}: {disk.FriendlyName} {StickRules.SizeText(disk.Size)} {disk.BusName}, the one chosen, nothing refuses it");
            if (q.Verb == StickVerb.Check)
            {
                r.Lines.Add("check only: nothing written");
                r.Ok = true;
                return r;
            }

            // read it once more right before erasing
            disk = Checked(sys, q, policy, r);
            if (disk is null) return r;
            ulong partSize = Math.Min(disk.Size - (8UL << 20), StickRules.MaxPartition);
            progress?.Invoke("erase", 0);
            r.Written = true;
            var root = sys.EraseAndFormat(disk, partSize, StickHelperRequest.Label);
            r.Lines.Add($"disk {disk.Number} erased: GPT, one FAT32 partition of up to {StickRules.SizeText(partSize)} at {root}");

            var boot = Path.Combine(root, RecoveryFiles.BootFolder);
            Directory.CreateDirectory(boot);
            var dstList = Path.Combine(boot, Path.GetFileName(files.ChunklistFile));
            var dstImage = Path.Combine(boot, Path.GetFileName(files.Image));
            Copy(files.ChunklistFile, dstList, null, cancel);
            Copy(files.Image, dstImage, b => progress?.Invoke("copy-image", (double)b / list.ImageSize), cancel);
            r.Lines.Add($"copied {RecoveryFiles.BootFolder}\\{Path.GetFileName(files.Image)} and {Path.GetFileName(files.ChunklistFile)}");
            if (efi is not null)
            {
                progress?.Invoke("copy-efi", 0);
                CopyTree(efi, Path.Combine(root, "EFI"), cancel);
                r.Lines.Add("copied the EFI folder to EFI\\");
            }

            // read back what was written
            progress?.Invoke("verify-stick", 0);
            if (!File.ReadAllBytes(dstList).AsSpan().SequenceEqual(File.ReadAllBytes(files.ChunklistFile))) throw new StickException("the chunklist on the stick differs from the download");
            list.VerifyFile(dstImage, b => progress?.Invoke("verify-stick", (double)b / list.ImageSize), cancel);
            r.Lines.Add("read back: the image on the stick matches Apple's chunklist");
            if (efi is not null)
            {
                int n = CompareTree(efi, Path.Combine(root, "EFI"));
                r.Lines.Add($"read back: {n} EFI files match");
            }
            progress?.Invoke("done", 1);
            r.Ok = true;
        }
        catch (Exception e)
        {
            r.Ok = false;
            r.Error = e is AggregateException { InnerException: { } inner } ? inner.Message : e.Message;
        }
        return r;
    }

    static StickDisk? Checked(IStickSystem sys, StickHelperRequest q, StickPolicy policy, StickHelperResult r) =>
        Reinspect(sys, q.Disk, q.Size, q.Id, q.Salt, policy, q.EraseEfi, r);

    /// The disk, read now: refused by the rules, not the one chosen (serial and size), or holding an EFI that the user
    /// did not confirm to erase, gives null with the reasons in r. Shared by the installer and probe stick helpers.
    public static StickDisk? Reinspect(IStickDisks sys, uint number, ulong size, string id, string salt, StickPolicy policy, bool eraseEfi, StickHelperResult r)
    {
        var disk = sys.Inspect(number);
        if (disk is null) { r.Error = $"no disk {number}: nothing written"; return null; }
        var refusals = StickRules.Refusals(disk, policy);
        if (refusals.Count > 0)
        {
            r.Refusals = [.. refusals];
            r.Error = $"disk {number} refused, nothing written: " + string.Join("; ", refusals);
            return null;
        }
        if (disk.Size != size || !CryptographicOperations.FixedTimeEquals(Encoding.ASCII.GetBytes(disk.Token(salt)), Encoding.ASCII.GetBytes(id)))
        {
            r.Error = $"disk {number} is not the disk that was chosen (another serial number or size): nothing written. Choose the stick again";
            return null;
        }
        if (StickRules.EfiNotConfirmed(disk, eraseEfi) is { } efi)
        {
            r.Refusals = [efi];
            r.Error = efi;
            return null;
        }
        return disk;
    }

    static long DirSize(string dir) => new DirectoryInfo(dir).EnumerateFiles("*", SearchOption.AllDirectories).Sum(f => f.Length);

    static void Copy(string src, string dst, Action<long>? progress, CancellationToken cancel)
    {
        using var i = new FileStream(src, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        using var o = new FileStream(dst, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, FileOptions.WriteThrough);
        var buf = new byte[4 << 20];
        long done = 0;
        int n;
        while ((n = i.Read(buf, 0, buf.Length)) > 0)
        {
            cancel.ThrowIfCancellationRequested();
            o.Write(buf, 0, n);
            done += n;
            progress?.Invoke(done);
        }
        o.Flush(true);
    }

    static void CopyTree(string src, string dst, CancellationToken cancel)
    {
        Directory.CreateDirectory(dst);
        foreach (var d in Directory.EnumerateDirectories(src, "*", SearchOption.AllDirectories))
            Directory.CreateDirectory(Path.Combine(dst, Path.GetRelativePath(src, d)));
        foreach (var f in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
            Copy(f, Path.Combine(dst, Path.GetRelativePath(src, f)), null, cancel);
    }

    static int CompareTree(string src, string dst)
    {
        int n = 0;
        foreach (var f in Directory.EnumerateFiles(src, "*", SearchOption.AllDirectories))
        {
            var other = Path.Combine(dst, Path.GetRelativePath(src, f));
            if (!File.Exists(other) || !SHA256.HashData(File.ReadAllBytes(f)).AsSpan().SequenceEqual(SHA256.HashData(File.ReadAllBytes(other))))
                throw new StickException("the EFI copy differs at " + Path.GetRelativePath(src, f));
            n++;
        }
        return n;
    }
}
