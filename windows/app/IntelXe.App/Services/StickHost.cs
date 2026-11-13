// SPDX-License-Identifier: MIT
// The Windows side of the installer stick: reading the disks (WmiStickSystem, Windows' storage API through WMI:
// MSFT_Disk, MSFT_Partition, Win32_DiskDrive), erasing the chosen one into one FAT32 partition, the helper's process
// ("IntelXeManager.exe --stick-helper ...", StickHelperMain) and starting it elevated with "runas" (StickRunner), as the
// EFI page does with --efi-helper. Reading needs no rights; only "write" needs administrator rights, and only the
// helper has them. Serial numbers are read into StickDisk.IdentitySource for the identity hash and go nowhere else.
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Management;
using System.Text;
using IntelXe.Core;
using IntelXe.Core.Stick;

namespace IntelXe.App;

sealed class WmiStickSystem : IStickSystem
{
    const string Storage = @"root\Microsoft\Windows\Storage";
    static readonly System.Management.EnumerationOptions Limits = new() { Timeout = TimeSpan.FromSeconds(20), ReturnImmediately = false };

    static List<ManagementObject> Query(string scope, string wql)
    {
        using var s = new ManagementObjectSearcher(new ManagementScope(scope), new ObjectQuery(wql), Limits);
        return s.Get().Cast<ManagementObject>().ToList();
    }

    static string Str(object? v) => v?.ToString()?.Trim() ?? "";

    public IReadOnlyList<StickDisk> ListDisks() =>
        Query(Storage, "SELECT * FROM MSFT_Disk").Select(ToDisk).OrderBy(d => d.Number).ToList();

    public StickDisk? Inspect(uint number) =>
        Query(Storage, $"SELECT * FROM MSFT_Disk WHERE Number = {number}").Select(ToDisk).FirstOrDefault();

    static StickDisk ToDisk(ManagementObject o)
    {
        uint n = Convert.ToUInt32(o["Number"], CultureInfo.InvariantCulture);
        var serial = Str(o["SerialNumber"]);
        if (serial.Length == 0) serial = Str(o["UniqueId"]);
        return new StickDisk
        {
            Number = n,
            FriendlyName = Str(o["FriendlyName"]),
            Size = Convert.ToUInt64(o["Size"] ?? 0UL, CultureInfo.InvariantCulture),
            BusType = Convert.ToInt32(o["BusType"] ?? 0, CultureInfo.InvariantCulture),
            IsBoot = o["IsBoot"] is true,
            IsSystem = o["IsSystem"] is true,
            IsReadOnly = o["IsReadOnly"] is true,
            IsOffline = o["IsOffline"] is true,
            Removable = Removable(n),
            VirtualFile = Convert.ToInt32(o["BusType"] ?? 0, CultureInfo.InvariantCulture) == StickRules.FileBackedVirtualBus ? Str(o["Location"]) : "",
            PartitionStyle = Convert.ToInt32(o["PartitionStyle"] ?? 0, CultureInfo.InvariantCulture) switch { 1 => "MBR", 2 => "GPT", _ => "RAW" },
            Partitions = Partitions(n),
            IdentitySource = serial,
        };
    }

    /// Win32_DiskDrive (Index = disk number): "Removable Media", or capability 7 (supports removable media).
    static bool? Removable(uint n)
    {
        try
        {
            foreach (var d in Query(@"root\CIMV2", $"SELECT MediaType, Capabilities FROM Win32_DiskDrive WHERE Index = {n}"))
                return Str(d["MediaType"]).Contains("Removable", StringComparison.OrdinalIgnoreCase)
                       || (d["Capabilities"] as ushort[] ?? []).Contains((ushort)7);
        }
        catch (Exception) { }
        return null;
    }

    /// The disk's partitions: their types, letters, and whether one holds Windows, an EFI boot loader (OpenCore or
    /// EFI\BOOT\BOOTx64.efi) or the probe stick (looked at through its letter or volume path; an EFI system partition is
    /// never opened, it counts as an EFI by its type; nothing is mounted).
    static List<StickPartition> Partitions(uint disk)
    {
        var list = new List<StickPartition>();
        foreach (var p in Query(Storage, $"SELECT PartitionNumber, GptType, MbrType, DriveLetter, AccessPaths FROM MSFT_Partition WHERE DiskNumber = {disk}"))
        {
            var gpt = Str(p["GptType"]);
            int mbr = p["MbrType"] is null ? 0 : Convert.ToInt32(p["MbrType"], CultureInfo.InvariantCulture);
            string letter = p["DriveLetter"] is char c && c != '\0' ? $"{char.ToUpperInvariant(c)}:" : "";
            bool windows = false, oc = false, boot = false, probe = false;
            if (!EspPartition.IsEspType(gpt) && mbr != 0xEF)
            {
                var roots = new List<string>();
                if (letter.Length > 0) roots.Add(letter + "\\");
                roots.AddRange((p["AccessPaths"] as string[] ?? []).Where(a => a.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase)));
                foreach (var root in roots)
                    try
                    {
                        windows |= File.Exists(Path.Combine(root, "Windows", "System32", "ntoskrnl.exe"));
                        probe |= File.Exists(Path.Combine(root, Core.ProbeStick.ProbeImage.Marker));
                        oc |= File.Exists(Path.Combine(root, "EFI", "OC", "OpenCore.efi"));
                        boot |= File.Exists(Path.Combine(root, "EFI", "BOOT", "BOOTx64.efi"));
                    }
                    catch (Exception) { }
            }
            // a probe stick's BOOTx64.efi is its own GRUB, not an EFI someone keeps
            list.Add(new StickPartition(Convert.ToUInt32(p["PartitionNumber"], CultureInfo.InvariantCulture), gpt, mbr, letter, "", windows,
                HoldsEfi: StickRules.VolumeHoldsEfi(oc, boot, probe), ProbeStick: probe));
        }
        return list;
    }

    // ---- erasing: only through the elevated helper, after StickWriter's checks ----------------------------------------

    const string BasicData = "{ebd0a0a2-b9e5-4433-87c0-68b6b72699c7}";

    public string EraseAndFormat(StickDisk disk, ulong partitionSize, string label)
    {
        var o = Query(Storage, $"SELECT * FROM MSFT_Disk WHERE Number = {disk.Number}").FirstOrDefault()
                ?? throw new StickException($"disk {disk.Number} is gone: nothing written");
        // the object about to be cleared is the disk the writer checked (serial and size, compared as a salted hash)
        var salt = StickIdentity.NewSalt();
        if (ToDisk(o).Token(salt) != disk.Token(salt)) throw new StickException($"disk {disk.Number} changed: nothing written");

        if (Convert.ToInt32(o["PartitionStyle"] ?? 0, CultureInfo.InvariantCulture) != 0)
            Call(o, "Clear", ("RemoveData", true), ("RemoveOEM", true), ("ZeroOutEntireDisk", false));
        o.Get();
        // Clear leaves some disks initialized (a USB stick that was GPT stayed GPT, empty, on 13 Nov: Initialize then
        // answers 41001 "already initialized"); a GPT disk with nothing on it needs no Initialize
        int style = Convert.ToInt32(o["PartitionStyle"] ?? 0, CultureInfo.InvariantCulture);
        if (style == 0) Call(o, "Initialize", ("PartitionStyle", (ushort)2));   // GPT; Windows adds its 16 MB reserved partition
        else if (style != 2) throw new StickException($"disk {disk.Number} is still MBR after clearing it: nothing more written");
        o.Get();
        // what is left after the GPT and the reserved partition, in whole MiB
        ulong free = Convert.ToUInt64(o["LargestFreeExtent"] ?? 0UL, CultureInfo.InvariantCulture) & ~((1UL << 20) - 1);
        ulong size = Math.Min(partitionSize, free);
        if (size < (512UL << 20)) throw new StickException($"only {StickRules.SizeText(free)} free on disk {disk.Number} after initializing it");
        var created = Call(o, "CreatePartition", ("Size", size), ("AssignDriveLetter", true), ("GptType", BasicData));
        var partition = created["CreatedPartition"] as ManagementBaseObject ?? throw new StickException("Windows made no partition");
        uint number = Convert.ToUInt32(partition["PartitionNumber"], CultureInfo.InvariantCulture);
        var part = Query(Storage, $"SELECT * FROM MSFT_Partition WHERE DiskNumber = {disk.Number} AND PartitionNumber = {number}").First();
        var volume = part.GetRelated("MSFT_Volume").Cast<ManagementObject>().FirstOrDefault() ?? throw new StickException("the new partition has no volume");
        Call(volume, "Format", ("FileSystem", "FAT32"), ("FileSystemLabel", label), ("Full", false), ("Force", true));

        string letter = "";
        for (int i = 0; i < 20 && letter.Length == 0; i++)
        {
            part.Get();
            if (part["DriveLetter"] is char c && c != '\0') letter = $"{char.ToUpperInvariant(c)}:";
            else if (i == 0) Call(part, "AddAccessPath", ("AssignDriveLetter", true));
            else Thread.Sleep(500);
        }
        if (letter.Length == 0) throw new StickException("the new FAT32 partition got no drive letter");
        return letter + "\\";
    }

    /// Calls a Storage Management method; a ReturnValue other than 0 throws with Windows' message.
    static ManagementBaseObject Call(ManagementObject o, string method, params (string Name, object Value)[] args)
    {
        using var input = o.GetMethodParameters(method);
        foreach (var (name, value) in args) input[name] = value;
        var output = o.InvokeMethod(method, input, null);
        uint rc = Convert.ToUInt32(output?["ReturnValue"] ?? 1u, CultureInfo.InvariantCulture);
        if (rc != 0)
        {
            string detail = "";
            if (output?["ExtendedStatus"] is ManagementBaseObject ext) detail = Str(ext["Message"]);
            throw new StickException($"{method} failed ({rc}){(detail.Length > 0 ? ": " + detail : "")}");
        }
        return output!;
    }
}

/// "IntelXeManager.exe --stick-helper list|check|write|download ...": the work and its answer file. Write needs
/// administrator rights (the app starts it with "runas"); the other verbs do not.
static class StickHelperMain
{
    public static int Run(string[] args)
    {
        StickHelperResult r;
        string? output = null;
        FileStream? progressFile = null;
        try
        {
            var q = StickHelperRequest.Parse(args);
            output = q.Out;
            if (q.Progress is { Length: > 0 } pp) progressFile = HelperMain.NewFile(pp);
            void Progress(string stage, double f)
            {
                if (progressFile is null) return;
                try
                {
                    var b = Encoding.ASCII.GetBytes(stage + " " + f.ToString("0.000", CultureInfo.InvariantCulture));
                    progressFile.SetLength(0);
                    progressFile.Write(b);
                    progressFile.Flush();
                }
                catch (Exception) { }
            }
            var sys = new WmiStickSystem();
            r = q.Verb switch
            {
                StickVerb.List => StickWriter.List(sys, new StickPolicy(0, q.TestVhdx), q.Salt),
                StickVerb.Download => Download(q, Progress),
                StickVerb.Write when !HelperMain.IsAdministrator() => new StickHelperResult { Ok = false, Error = "erasing a disk needs administrator rights (the app starts the helper with them): nothing written" },
                _ => StickWriter.Run(q, sys, Progress, [AppContext.BaseDirectory[..2]]),
            };
        }
        catch (Exception e)
        {
            r = new StickHelperResult { Ok = false, Error = e.Message };
            int i = Array.IndexOf(args, "--out");
            if (i >= 0 && i + 1 < args.Length) output = args[i + 1];
        }
        finally { progressFile?.Dispose(); }
        try
        {
            if (output is null) Console.Out.Write(r.ToJson());
            else HelperMain.Answer(output, r.ToJson());
        }
        catch (Exception) { return 3; }
        return r.Ok ? 0 : 1;
    }

    static StickHelperResult Download(StickHelperRequest q, Action<string, double> progress)
    {
        var v = RecoveryData.Embedded.Find(q.Version!);
        using var client = new RecoveryClient();
        var files = client.DownloadAsync(v, q.Image, new Progress<RecoveryProgress>(p => progress(p.Stage, p.Total > 0 ? (double)p.Bytes / p.Total : 0))).GetAwaiter().GetResult();
        return new StickHelperResult
        {
            Ok = true,
            Lines = [$"{v.Name}: product {files.Product}, {files.Size} bytes, every piece matches Apple's signed chunklist", files.Image, files.ChunklistFile],
        };
    }
}

/// The app's side: the helper started through "runas" (the UAC prompt), its progress read from a file while it works,
/// its answer from another.
static class StickRunner
{
    public static Task<StickHelperResult> RunElevatedAsync(StickHelperRequest q, Action<string, double>? progress, CancellationToken cancel = default) =>
        RunElevatedAsync((output, progressFile) => (q with { Out = output, Progress = progressFile }).ToArgs(), progress, cancel);

    /// Any helper (--stick-helper, --probe-helper): args gets the answer and progress files' paths.
    public static async Task<StickHelperResult> RunElevatedAsync(Func<string, string, IReadOnlyList<string>> args, Action<string, double>? progress, CancellationToken cancel = default)
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "helper");
        Directory.CreateDirectory(dir);
        var id = Guid.NewGuid().ToString("N");
        var output = Path.Combine(dir, id + ".json");
        var progressPath = Path.Combine(dir, id + ".progress");
        var psi = new ProcessStartInfo(Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "IntelXeManager.exe"),
            Core.Efi.CommandLine.Join(args(output, progressPath)))
        {
            UseShellExecute = true, Verb = "runas", WorkingDirectory = AppContext.BaseDirectory, WindowStyle = ProcessWindowStyle.Hidden,
        };
        try
        {
            Process p;
            try { p = Process.Start(psi) ?? throw new StickException("the helper did not start"); }
            catch (Win32Exception e) when (e.NativeErrorCode == 1223) { throw new StickException(L.Get("Stick_Elevation_Cancelled")); }
            using (p)
                while (!p.HasExited)
                {
                    try { await p.WaitForExitAsync(cancel).WaitAsync(TimeSpan.FromMilliseconds(400), cancel); }
                    catch (TimeoutException) { }
                    if (progress is not null && ReadProgress(progressPath) is { } pr) progress(pr.Stage, pr.Fraction);
                }
            if (!File.Exists(output)) throw new StickException(L.Get("Stick_Helper_NoAnswer"));
            return StickHelperResult.FromJson(await File.ReadAllTextAsync(output, cancel));
        }
        finally
        {
            try { File.Delete(output); } catch (Exception) { }
            try { File.Delete(progressPath); } catch (Exception) { }
        }
    }

    static (string Stage, double Fraction)? ReadProgress(string path)
    {
        try
        {
            using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            var text = new StreamReader(f).ReadToEnd().Trim().Split(' ');
            if (text.Length == 2 && double.TryParse(text[1], NumberStyles.Float, CultureInfo.InvariantCulture, out var v)) return (text[0], v);
        }
        catch (Exception) { }
        return null;
    }
}
