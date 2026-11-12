// SPDX-License-Identifier: MIT
// The Windows side of the Xe probe stick: the disk opened raw (PhysicalDisk: every volume on it locked and dismounted with
// FSCTL_LOCK_VOLUME and FSCTL_DISMOUNT_VOLUME, then \\.\PhysicalDriveN with FILE_FLAG_NO_BUFFERING |
// FILE_FLAG_WRITE_THROUGH, written and read in sector-aligned buffers, IOCTL_DISK_UPDATE_PROPERTIES after the flush), the
// helper's process ("IntelXeManager.exe --probe-helper ...", ProbeHelperMain) started elevated through StickRunner as the
// installer stick's is, and the probe sticks among the drives (ProbeSticks). The disks are read by WmiStickSystem, the
// same code (and rules, StickRules) as the installer stick page.
using System.ComponentModel;
using System.Globalization;
using System.Management;
using System.Runtime.InteropServices;
using System.Text;
using IntelXe.Core.ProbeStick;
using IntelXe.Core.Stick;
using Microsoft.Win32.SafeHandles;

namespace IntelXe.App;

/// The disks (WmiStickSystem) and one opened raw for the probe writer.
sealed class WindowsProbeSystem : IProbeSystem
{
    readonly WmiStickSystem wmi = new();
    public IReadOnlyList<StickDisk> ListDisks() => wmi.ListDisks();
    public StickDisk? Inspect(uint number) => wmi.Inspect(number);

    public IRawDisk Open(StickDisk disk)
    {
        // the disk about to be opened is the one the writer checked (serial and size, compared as a salted hash)
        var salt = StickIdentity.NewSalt();
        if (wmi.Inspect(disk.Number) is not { } now || now.Token(salt) != disk.Token(salt))
            throw new ProbeException($"disk {disk.Number} changed: nothing written");
        return PhysicalDisk.Open(disk.Number, VolumesOf(disk.Number));
    }

    /// The volume paths (\\?\Volume{GUID}\) of the disk's partitions.
    static List<string> VolumesOf(uint disk)
    {
        var list = new List<string>();
        using var s = new ManagementObjectSearcher(new ManagementScope(@"root\Microsoft\Windows\Storage"),
            new ObjectQuery($"SELECT AccessPaths FROM MSFT_Partition WHERE DiskNumber = {disk}"));
        foreach (var p in s.Get().Cast<ManagementObject>())
            foreach (var a in p["AccessPaths"] as string[] ?? [])
                if (a.StartsWith(@"\\?\Volume{", StringComparison.OrdinalIgnoreCase) && !list.Contains(a, StringComparer.OrdinalIgnoreCase)) list.Add(a);
        return list;
    }
}

/// \\.\PhysicalDriveN for raw writing, with its volumes locked and dismounted until it is disposed.
sealed partial class PhysicalDisk : IRawDisk
{
    const uint GenericRead = 0x80000000, GenericWrite = 0x40000000, ShareRead = 1, ShareWrite = 2, OpenExisting = 3;
    const uint NoBuffering = 0x20000000, WriteThrough = 0x80000000;
    const uint FsctlLockVolume = 0x00090018, FsctlDismountVolume = 0x00090020, FsctlUnlockVolume = 0x0009001C;
    const uint IoctlDiskGetDriveGeometryEx = 0x000700A0, IoctlDiskUpdateProperties = 0x00070140;
    const int Alignment = 4096;

    [LibraryImport("kernel32.dll", EntryPoint = "CreateFileW", SetLastError = true, StringMarshalling = StringMarshalling.Utf16)]
    private static partial SafeFileHandle CreateFile(string name, uint access, uint share, IntPtr security, uint disposition, uint flags, IntPtr template);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool DeviceIoControl(SafeFileHandle h, uint code, void* input, uint inputSize, void* output, uint outputSize, out uint returned, IntPtr overlapped);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool SetFilePointerEx(SafeFileHandle h, long distance, out long newPointer, uint method);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool WriteFile(SafeFileHandle h, byte* buffer, uint count, out uint written, IntPtr overlapped);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static unsafe partial bool ReadFile(SafeFileHandle h, byte* buffer, uint count, out uint read, IntPtr overlapped);

    [LibraryImport("kernel32.dll", SetLastError = true)]
    [return: MarshalAs(UnmanagedType.Bool)]
    private static partial bool FlushFileBuffers(SafeFileHandle h);

    readonly SafeFileHandle disk;
    readonly List<SafeFileHandle> volumes;
    readonly unsafe byte* buffer;
    readonly int bufferSize = ProbeWriter.Block;
    bool disposed;

    public long Length { get; }
    public int SectorSize { get; }

    unsafe PhysicalDisk(SafeFileHandle disk, List<SafeFileHandle> volumes, long length, int sector)
    {
        this.disk = disk;
        this.volumes = volumes;
        Length = length;
        SectorSize = sector;
        buffer = (byte*)NativeMemory.AlignedAlloc((nuint)bufferSize, Alignment);
    }

    static Exception Error(string what) => new ProbeException($"{what}: {new Win32Exception(Marshal.GetLastWin32Error()).Message}");

    public static unsafe PhysicalDisk Open(uint number, IReadOnlyList<string> volumePaths)
    {
        var locked = new List<SafeFileHandle>();
        try
        {
            foreach (var path in volumePaths)
            {
                var v = CreateFile(@"\\.\" + path.TrimEnd('\\')[4..], GenericRead | GenericWrite, ShareRead | ShareWrite, IntPtr.Zero, OpenExisting, 0, IntPtr.Zero);
                if (v.IsInvalid) { v.Dispose(); continue; }   // a volume that is gone needs no lock
                bool ok = false;
                for (int i = 0; i < 10 && !ok; i++)
                {
                    ok = DeviceIoControl(v, FsctlLockVolume, null, 0, null, 0, out _, IntPtr.Zero);
                    if (!ok) Thread.Sleep(500);
                }
                if (!ok)
                {
                    v.Dispose();
                    throw new ProbeException("a program has files open on the stick (Explorer, an editor, a virus scanner); close it and write again: nothing written");
                }
                locked.Add(v);
                if (!DeviceIoControl(v, FsctlDismountVolume, null, 0, null, 0, out _, IntPtr.Zero)) throw Error("dismounting a volume of the stick failed: nothing written");
            }

            var h = CreateFile($@"\\.\PhysicalDrive{number.ToString(CultureInfo.InvariantCulture)}", GenericRead | GenericWrite, ShareRead | ShareWrite,
                IntPtr.Zero, OpenExisting, NoBuffering | WriteThrough, IntPtr.Zero);
            if (h.IsInvalid) { h.Dispose(); throw Error($"opening disk {number} failed: nothing written"); }
            byte* geometry = stackalloc byte[256];
            if (!DeviceIoControl(h, IoctlDiskGetDriveGeometryEx, null, 0, geometry, 256, out var got, IntPtr.Zero) || got < 32)
            {
                h.Dispose();
                throw Error($"reading disk {number}'s geometry failed: nothing written");
            }
            int sector = *(int*)(geometry + 20);   // DISK_GEOMETRY.BytesPerSector
            long size = *(long*)(geometry + 24);    // DISK_GEOMETRY_EX.DiskSize
            if (sector is not (512 or 1024 or 2048 or 4096)) { h.Dispose(); throw new ProbeException($"disk {number} reports {sector}-byte sectors: nothing written"); }
            return new PhysicalDisk(h, locked, size, sector);
        }
        catch
        {
            foreach (var v in locked) v.Dispose();
            throw;
        }
    }

    void Seek(long offset)
    {
        if (!SetFilePointerEx(disk, offset, out _, 0)) throw Error($"seeking to {offset} failed");
    }

    public unsafe void Write(long offset, ReadOnlySpan<byte> data)
    {
        if (offset % SectorSize != 0 || data.Length % SectorSize != 0) throw new ProbeException($"unaligned write at {offset}");
        for (int done = 0; done < data.Length;)
        {
            int n = Math.Min(bufferSize, data.Length - done);
            data.Slice(done, n).CopyTo(new Span<byte>(buffer, n));
            Seek(offset + done);
            if (!WriteFile(disk, buffer, (uint)n, out var written, IntPtr.Zero) || written != n) throw Error($"writing at {offset + done} failed");
            done += n;
        }
    }

    public unsafe int Read(long offset, Span<byte> target)
    {
        if (offset % SectorSize != 0 || target.Length % SectorSize != 0) throw new ProbeException($"unaligned read at {offset}");
        int done = 0;
        while (done < target.Length)
        {
            int n = Math.Min(bufferSize, target.Length - done);
            Seek(offset + done);
            if (!ReadFile(disk, buffer, (uint)n, out var read, IntPtr.Zero)) throw Error($"reading at {offset + done} failed");
            if (read == 0) break;
            new Span<byte>(buffer, (int)read).CopyTo(target[done..]);
            done += (int)read;
        }
        return done;
    }

    public unsafe void Flush()
    {
        if (!FlushFileBuffers(disk)) throw Error("flushing the disk failed");
        // Windows reads the new partition table (the stick's FAT32 volume appears once the volumes are released)
        DeviceIoControl(disk, IoctlDiskUpdateProperties, null, 0, null, 0, out _, IntPtr.Zero);
    }

    public unsafe void Dispose()
    {
        if (disposed) return;
        disposed = true;
        disk.Dispose();
        foreach (var v in volumes)
        {
            DeviceIoControl(v, FsctlUnlockVolume, null, 0, null, 0, out _, IntPtr.Zero);
            v.Dispose();
        }
        NativeMemory.AlignedFree(buffer);
    }
}

/// "IntelXeManager.exe --probe-helper check|write ...": the work and its answer file. Write needs administrator rights
/// (the app starts it with "runas").
static class ProbeHelperMain
{
    public static int Run(string[] args)
    {
        StickHelperResult r;
        string? output = null;
        FileStream? progressFile = null;
        try
        {
            var q = ProbeHelperRequest.Parse(args);
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
            r = q.Verb == ProbeVerb.Write && !HelperMain.IsAdministrator()
                ? new StickHelperResult { Ok = false, Error = "writing a disk needs administrator rights (the app starts the helper with them): nothing written" }
                : ProbeWriter.Run(q, new WindowsProbeSystem(), Progress, [AppContext.BaseDirectory[..2]]);
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

    /// The window's side: the helper through "runas", its progress while it works.
    public static Task<StickHelperResult> RunElevatedAsync(ProbeHelperRequest q, Action<string, double>? progress) =>
        StickRunner.RunElevatedAsync((output, progressFile) => (q with { Out = output, Progress = progressFile }).ToArgs(), progress);
}

/// The probe sticks among this PC's drives: a FAT volume labelled XEPROBE with the marker file.
static class ProbeSticks
{
    public static IReadOnlyList<string> Find()
    {
        var volumes = new List<(string, string?)>();
        foreach (var d in DriveInfo.GetDrives())
        {
            try
            {
                if ((d.DriveType is DriveType.Removable or DriveType.Fixed) && d.IsReady) volumes.Add((d.RootDirectory.FullName, d.VolumeLabel));
            }
            catch (Exception) { }
        }
        return ProbeReports.FindSticks(volumes);
    }

    public static void Reveal(string file)
    {
        try { System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{file}\""); } catch (Exception) { }
    }
}
