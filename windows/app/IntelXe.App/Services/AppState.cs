// SPDX-License-Identifier: MIT
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXe.Core.Report;

namespace IntelXe.App;

/// What the pages share: this PC's facts (read once at start and on Refresh), the EFI planner and the package its files
/// come from, the last report.
public sealed class AppState
{
    public static AppState Current { get; } = new();

    public PcFacts? Facts { get; private set; }
    public IReadOnlyList<Check> Checks { get; private set; } = [];
    public bool Probing { get; private set; }
    public event Action? Changed;

    /// The EFI rules built into the app (efi-rules.json), run by IntelXe-EFI.ps1's engine; PendingEfiPlanner when they
    /// cannot be loaded.
    public IEfiPlanner Planner { get; set; } = MakePlanner();

    static IEfiPlanner MakePlanner()
    {
        try
        {
            _ = EfiRules.Embedded;
            return new WindowsEfiPlanner(App.Options.HelperForFolders);
        }
        catch (Exception e) { return new PendingEfiPlanner(e.Message); }
    }

    /// The package (the unpacked -efi.zip) the EFI page takes IntelXe.kext, SSDT-ARCOFF.aml and HybridPM.kext from, and
    /// why the last choice was not one.
    public EfiPackageInfo? Package { get; private set; }
    public string? PackageError { get; private set; }
    bool packageLooked;

    /// Where a chosen -efi.zip is unpacked.
    public static string PackageCache => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "packages");

    /// The package given on the command line or chosen last time, else one beside the app; once.
    public void FindPackage()
    {
        if (packageLooked) return;
        packageLooked = true;
        foreach (var p in new[] { App.Options.Package, Settings.Current.Package })
            if (!string.IsNullOrEmpty(p) && (File.Exists(p) || Directory.Exists(p)) && UsePackage(p, save: false)) return;
        if (EfiPackage.FindBeside(AppContext.BaseDirectory) is { } beside) UsePackage(beside, save: false);
    }

    /// A folder (the unpacked -efi.zip or the folder holding it) or the zip itself. False, with PackageError, when it is
    /// not a package.
    public bool UsePackage(string path, bool save = true)
    {
        try
        {
            string? dir = File.Exists(path) && path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)
                ? EfiPackage.Unzip(path, PackageCache)
                : EfiPackage.FindIn(path);
            if (dir is null) { PackageError = L.Format("Pkg_NotAPackage", path); return false; }
            Package = EfiPackage.Inspect(dir);
            PackageError = null;
            if (save) { Settings.Current.Package = path; Settings.Current.Save(); }
            return true;
        }
        catch (Exception e)
        {
            PackageError = e.Message;
            return false;
        }
        finally { Changed?.Invoke(); }
    }

    public ReportResult? LastReport { get; set; }
    public ReportRunner Report { get; } = new();

    public async Task RefreshAsync()
    {
        if (Probing) return;
        Probing = true;
        Changed?.Invoke();
        try
        {
            var facts = await Task.Run(SystemProbe.Probe);
            Facts = facts;
            Checks = PcChecks.Evaluate(facts);
        }
        finally
        {
            Probing = false;
            Changed?.Invoke();
        }
    }
}
