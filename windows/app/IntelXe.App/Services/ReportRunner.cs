// SPDX-License-Identifier: MIT
using System.ComponentModel;
using System.Diagnostics;
using System.Globalization;
using System.Text;
using IntelXe.Core.Efi;
using IntelXe.Core.Report;

namespace IntelXe.App;

/// Runs tools/report/collect-windows.ps1 (shipped beside the app in tools\) in Windows PowerShell and collects its output.
/// The script only reads, writes one zip on the Desktop and uploads nothing; neither does the app. With boot logs it also
/// reads the EFI partitions, which only an administrator can mount: the app then starts it with "runas" (StartElevated).
public sealed class ReportRunner
{
    public bool Running { get; private set; }
    public DateTime StartedAt { get; private set; }
    public StringBuilder Output { get; } = new();
    public event Action? OutputChanged;
    public event Action<ReportResult?, string?>? Finished;

    /// The script beside the app (tools\collect-windows.ps1), or, when the app runs from a build in the repository, the
    /// repository's tools\report\collect-windows.ps1.
    public static string? FindScript()
    {
        var beside = Path.Combine(AppContext.BaseDirectory, "tools", "collect-windows.ps1");
        if (File.Exists(beside)) return beside;
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
        {
            var inRepo = Path.Combine(d.FullName, "tools", "report", "collect-windows.ps1");
            if (File.Exists(inRepo)) return inRepo;
        }
        return null;
    }

    static string PowerShell =>
        Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.System), "WindowsPowerShell", "v1.0", "powershell.exe");

    public static string Desktop => Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);

    /// The Desktop, as on macOS, unless --report-out names another folder.
    public static string OutputFolder => App.Options.ReportOut is { Length: > 0 } d && Directory.Exists(d) ? Path.GetFullPath(d) : Desktop;

    public void Start(bool acpi, bool bootLogs = false)
    {
        if (Running) return;
        var script = FindScript();
        Output.Clear();
        if (script is null)
        {
            Finished?.Invoke(null, L.Get("Report_NoScript"));
            return;
        }
        if (bootLogs && !HelperMain.IsAdministrator())
        {
            StartElevated(script, acpi);
            return;
        }
        var info = new ProcessStartInfo(PowerShell)
        {
            UseShellExecute = false,
            CreateNoWindow = true,
            RedirectStandardOutput = true,
            RedirectStandardError = true,
            WorkingDirectory = OutputFolder,
            // Windows PowerShell writes in the console's OEM code page
            StandardOutputEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
            StandardErrorEncoding = Encoding.GetEncoding(CultureInfo.CurrentCulture.TextInfo.OEMCodePage),
        };
        foreach (var a in ReportRun.PowerShellArguments(script, OutputFolder, acpi, bootLogs)) info.ArgumentList.Add(a);
        var process = new Process { StartInfo = info, EnableRaisingEvents = true };
        void Add(string? line)
        {
            if (line is null) return;
            lock (Output) Output.AppendLine(line);
            OutputChanged?.Invoke();
        }
        process.OutputDataReceived += (_, e) => Add(e.Data);
        process.ErrorDataReceived += (_, e) => Add(e.Data);
        process.Exited += (_, _) =>
        {
            process.WaitForExit();   // the last lines of the redirected output
            Running = false;
            string text;
            lock (Output) text = Output.ToString();
            var result = ReportRun.Parse(text.Split('\n'));
            Finished?.Invoke(result, result is null ? L.Get("Report_NoneWritten") : null);
            process.Dispose();
        };
        try
        {
            Running = true;
            StartedAt = DateTime.Now;
            process.Start();
            process.BeginOutputReadLine();
            process.BeginErrorReadLine();
        }
        catch (Exception e)
        {
            Running = false;
            Finished?.Invoke(null, e.Message);
        }
    }

    /// The report with -BootLogs as administrator: Windows PowerShell through "runas" (the UAC prompt), its output written
    /// to a file of the app's (ReportRun.ElevatedArguments) and shown when the run has ended; nothing comes while it runs.
    void StartElevated(string script, bool acpi)
    {
        var dir = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "report");
        Directory.CreateDirectory(dir);
        var log = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".txt");
        var info = new ProcessStartInfo(PowerShell, CommandLine.Join(ReportRun.ElevatedArguments(script, OutputFolder, acpi, bootLogs: true, log)))
        {
            UseShellExecute = true, Verb = "runas", WindowStyle = ProcessWindowStyle.Hidden, WorkingDirectory = OutputFolder,
        };
        Running = true;
        StartedAt = DateTime.Now;
        lock (Output) Output.AppendLine(L.Get("Report_Elevated_Running"));
        OutputChanged?.Invoke();
        _ = Task.Run(async () =>
        {
            string? error = null;
            try
            {
                using var p = Process.Start(info) ?? throw new InvalidOperationException("PowerShell did not start");
                await p.WaitForExitAsync();
            }
            catch (Win32Exception e) when (e.NativeErrorCode == 1223) { error = L.Get("Report_Elevation_Cancelled"); }
            catch (Exception e) { error = e.Message; }
            var text = "";
            try { if (File.Exists(log)) text = File.ReadAllText(log); } catch (Exception) { }
            try { File.Delete(log); } catch (Exception) { }
            lock (Output) { Output.Clear(); Output.Append(text); }
            Running = false;
            OutputChanged?.Invoke();
            var result = error is null ? ReportRun.Parse(text.Split('\n')) : null;
            Finished?.Invoke(result, error ?? (result is null ? L.Get("Report_NoneWritten") : null));
        });
    }
}
