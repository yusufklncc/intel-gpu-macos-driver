// SPDX-License-Identifier: MIT
// The Windows side of the EFI changes: the elevated helper's process (HelperMain, MountvolMounter), the planner the EFI
// page uses (WindowsEfiPlanner: an EFI folder in this process, an EFI partition through the helper started with
// "runas"), and the package the files come from.
using System.ComponentModel;
using System.Diagnostics;
using System.Management;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXeEfi;

namespace IntelXe.App;

/// "IntelXeManager.exe --efi-helper status|plan|apply ...": parses the request, refuses a partition without
/// administrator rights, runs it (EfiHelper.Run with mountvol) and writes the answer to --out (or to the standard output).
static class HelperMain
{
    public static int Run(string[] args)
    {
        EfiHelperResult r;
        string? output = null;
        try
        {
            var q = EfiHelperRequest.Parse(args);
            output = q.Out;
            r = q.Target is EfiTarget.Partition && !IsAdministrator()
                ? new EfiHelperResult { Ok = false, Error = "mounting an EFI partition needs administrator rights (the app starts the helper with them)" }
                : EfiHelper.Run(q, new MountvolMounter());
        }
        catch (Exception e)
        {
            r = new EfiHelperResult { Ok = false, Error = EfiHelper.Message(e) };
            int i = Array.IndexOf(args, "--out");
            if (i >= 0 && i + 1 < args.Length) output = args[i + 1];
        }
        try
        {
            if (output is null) Console.Out.Write(r.ToJson());
            else Answer(output, r.ToJson());
        }
        catch (Exception) { return 3; }
        return r.Ok ? 0 : 1;
    }

    /// The answer, as a new file (never over an existing one) in a folder that is not a link elsewhere: the folder is
    /// the user's, and this process may have administrator rights.
    static void Answer(string path, string json)
    {
        var dir = new DirectoryInfo(Path.GetDirectoryName(Path.GetFullPath(path))!);
        for (var d = dir; d is not null; d = d.Parent)
            if (d.Exists && (d.Attributes & FileAttributes.ReparsePoint) != 0) throw new IOException(d.FullName + " is a link");
        using var f = new FileStream(path, FileMode.CreateNew, FileAccess.Write, FileShare.None);
        var bytes = new UTF8Encoding(false).GetBytes(json);
        f.Write(bytes);
    }

    public static bool IsAdministrator()
    {
        using var id = WindowsIdentity.GetCurrent();
        return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator);
    }
}

/// Mounting an EFI system partition as IntelXe-EFI.ps1's Mount-Esp and Dismount-Esp do: found by disk and partition
/// number (MSFT_Partition), refused unless its GPT type is the EFI system partition's, mounted with mountvol at a free
/// letter (S:, T:, ... as Get-FreeLetter picks them) or at the letter asked for; unmounted with mountvol /D only when the
/// letter still holds the volume this mounted. A partition that already has a letter is used there and left mounted.
sealed class MountvolMounter : IEspMounter
{
    static readonly string[] Letters = ["S", "T", "U", "V", "W", "X", "Y", "Z", "R", "Q", "P", "O", "N", "M"];
    string volume = "";

    public MountedEsp Mount(uint disk, uint partition, string? letter)
    {
        using var searcher = new ManagementObjectSearcher(new ManagementScope(@"root\Microsoft\Windows\Storage"),
            new ObjectQuery($"SELECT DiskNumber, PartitionNumber, GptType, DriveLetter, AccessPaths FROM MSFT_Partition WHERE DiskNumber = {disk} AND PartitionNumber = {partition}"));
        ManagementBaseObject? p = null;
        foreach (var o in searcher.Get()) { p = o; break; }
        if (p is null) throw new EfiException($"no partition {partition} on disk {disk}");
        if (!EspPartition.IsEspType(p["GptType"]?.ToString())) throw new EfiException($"disk {disk} partition {partition} is not an EFI system partition: not mounted");
        var paths = p["AccessPaths"] as string[] ?? [];
        volume = paths.FirstOrDefault(a => a.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase)) ?? "";
        if (p["DriveLetter"] is char c && c != '\0') return new MountedEsp($"{char.ToUpperInvariant(c)}:", $"{char.ToUpperInvariant(c)}:\\", false);
        if (volume.Length == 0) throw new EfiException($"disk {disk} partition {partition} has no volume path to mount");

        var used = Environment.GetLogicalDrives().Select(d => d[..1].ToUpperInvariant()).ToHashSet();
        string? at = letter is not null && !used.Contains(letter[..1].ToUpperInvariant()) ? letter[..1].ToUpperInvariant() : Letters.FirstOrDefault(l => !used.Contains(l));
        if (at is null) throw new EfiException("no free drive letter between M: and Z:");
        Mountvol($"{at}:", volume);
        if (!string.Equals(VolumeAt($"{at}:\\"), volume, StringComparison.OrdinalIgnoreCase))
            throw new EfiException($"mountvol did not mount the EFI partition at {at}:");
        return new MountedEsp($"{at}:", $"{at}:\\", true);
    }

    public void Unmount(MountedEsp esp)
    {
        if (!esp.MountedHere) return;
        if (!string.Equals(VolumeAt(esp.Letter + "\\"), volume, StringComparison.OrdinalIgnoreCase))
            throw new EfiException($"{esp.Letter} no longer holds the EFI partition this mounted: unmount it yourself with mountvol {esp.Letter} /D");
        Mountvol(esp.Letter, "/D");
    }

    static void Mountvol(string letter, string what)
    {
        var exe = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "mountvol.exe");
        var psi = new ProcessStartInfo(exe) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true };
        psi.ArgumentList.Add(letter);
        psi.ArgumentList.Add(what);
        using var p = Process.Start(psi) ?? throw new EfiException("mountvol did not start");
        var text = p.StandardOutput.ReadToEnd() + p.StandardError.ReadToEnd();
        if (!p.WaitForExit(30_000)) { try { p.Kill(); } catch (Exception) { } throw new EfiException("mountvol did not finish"); }
        if (p.ExitCode != 0) throw new EfiException("mountvol failed: " + text.Trim());
    }

    [DllImport("kernel32.dll", CharSet = CharSet.Unicode, SetLastError = true)]
    static extern bool GetVolumeNameForVolumeMountPointW(string mountPoint, StringBuilder name, int size);

    static string VolumeAt(string root)
    {
        var sb = new StringBuilder(64);
        return GetVolumeNameForVolumeMountPointW(root, sb, sb.Capacity) ? sb.ToString() : "";
    }
}

/// The EFI page's planner: an EFI folder in this process (FolderEfiPlanner, the user's rights), an EFI partition through
/// the elevated helper, which Windows asks permission for each time (for a plan, for a change). --helper-for-folders
/// sends folders through the helper too (to test that path without an EFI partition).
sealed class WindowsEfiPlanner(bool helperForFolders) : IEfiPlanner
{
    readonly FolderEfiPlanner local = new();

    public bool IsAvailable => true;
    public string RulesVersion => local.RulesVersion;

    bool Local(EfiTarget t) => t is EfiTarget.Folder && !helperForFolders;

    public async Task<EfiStatus> ReadStatusAsync(EfiTarget target, EfiOptions options, CancellationToken cancel = default) =>
        Local(target) ? await local.ReadStatusAsync(target, options, cancel)
            : (await Elevated(new EfiHelperRequest(EfiHelperVerb.Status, target, EfiChange.Settings, options), cancel)).ToStatus();

    public async Task<EfiPlan> PlanAsync(EfiTarget target, EfiChange change, EfiOptions options, CancellationToken cancel = default) =>
        Local(target) ? await local.PlanAsync(target, change, options, cancel)
            : (await Elevated(new EfiHelperRequest(EfiHelperVerb.Plan, target, change, options), cancel)).ToPlan(target);

    public async Task<EfiResult> ApplyAsync(EfiPlan plan, EfiOptions options, CancellationToken cancel = default)
    {
        if (Local(plan.Target)) return await local.ApplyAsync(plan, options, cancel);
        var r = await Elevated(new EfiHelperRequest(EfiHelperVerb.Apply, plan.Target, plan.Change, options, plan.Letter, plan.Hash), cancel);
        return new EfiResult([.. r.Lines ?? [], .. r.Warning is null ? Array.Empty<string>() : ["warning  " + r.Warning]]);
    }

    /// The same exe with --efi-helper, through "runas" (the UAC prompt); its answer comes back in a file of the app's.
    static async Task<EfiHelperResult> Elevated(EfiHelperRequest q, CancellationToken cancel)
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "helper");
        Directory.CreateDirectory(dir);
        var output = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".json");
        var psi = new ProcessStartInfo(Environment.ProcessPath ?? Path.Combine(AppContext.BaseDirectory, "IntelXeManager.exe"),
            CommandLine.Join((q with { Out = output }).ToArgs()))
        {
            UseShellExecute = true, Verb = "runas", WorkingDirectory = AppContext.BaseDirectory, WindowStyle = ProcessWindowStyle.Hidden,
        };
        try
        {
            Process p;
            try { p = Process.Start(psi) ?? throw new EfiException("the helper did not start"); }
            catch (Win32Exception e) when (e.NativeErrorCode == 1223) { throw new EfiUnavailableException(L.Get("Efi_Elevation_Cancelled")); }
            using (p) await p.WaitForExitAsync(cancel);
            if (!File.Exists(output)) throw new EfiException(L.Get("Efi_Helper_NoAnswer"));
            var r = EfiHelperResult.FromJson(await File.ReadAllTextAsync(output, cancel));
            if (!r.Ok) throw new EfiException(r.Error ?? "the helper failed");
            return r;
        }
        finally
        {
            try { File.Delete(output); } catch (Exception) { }
        }
    }
}
