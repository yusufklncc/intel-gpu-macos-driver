// SPDX-License-Identifier: MIT
// IntelXe Manager for Windows: check this PC, prepare the OpenCore EFI (through the shared EFI rules), collect a
// diagnostics report. The counterpart of the macOS app in app/; the same sections, wording and accent.
using Microsoft.UI.Xaml;

namespace IntelXe.App;

public partial class App : Application
{
    public static MainWindow? Window { get; private set; }
    public static Options Options { get; } = Options.Parse(Environment.GetCommandLineArgs().Skip(1).ToArray());
    public static Settings Settings => IntelXe.App.Settings.Current;

    public App()
    {
        System.Text.Encoding.RegisterProvider(System.Text.CodePagesEncodingProvider.Instance);   // the report script's console output
        InitializeComponent();
    }

    protected override void OnLaunched(LaunchActivatedEventArgs args)
    {
        L.SetLanguage(Options.Language ?? Settings.Current.Language);
        Window = new MainWindow();
        Window.Activate();
    }
}

/// The command line, as the macOS app's --section: for screenshots and support instructions.
/// --section overview|efi|report|about, --lang en-US|tr-TR, --theme light|dark, --size WIDTHxHEIGHT (in effective pixels),
/// --scroll PIXELS (the page scrolled down by that much, for a screenshot of its lower part), --report-out FOLDER (where
/// the diagnostics report goes instead of the Desktop). The EFI page: --efi-folder FOLDER (chosen as the EFI), --package
/// FOLDER|ZIP, --card-confirmed (the card's ACPI path confirmed), --plan settings|safari|settings-safari|driver|undo (that
/// plan's dialog opened), --helper-for-folders (an EFI folder through the elevated helper too, to test it without an EFI
/// partition). The BIOS page: --bios-vendor asus|msi|gigabyte|asrock|generic (that maker's menu names), --bios-html FILE
/// (its printable page written there once the facts are read). The stick page:
/// --recovery-dir FOLDER (where the recovery downloads are kept instead of %LOCALAPPDATA%), --stick-confirm (the erase
/// dialog opened, for the test disk only), and, for tests only,
/// --test-vhdx PATH (that VHD/VHDX file, attached, is offered and may be written although it is not a USB stick).
public sealed record Options(string? Section, string? Language, string? Theme, int? Width, int? Height, int? Scroll, string? ReportOut,
    string? EfiFolder = null, string? Package = null, string? Plan = null, bool CardConfirmed = false, bool HelperForFolders = false,
    string? BiosVendor = null, string? TestVhdx = null, bool StickConfirm = false, string? BiosHtml = null, string? RecoveryDir = null)
{
    public static Options Parse(string[] args)
    {
        string? Value(string name)
        {
            int i = Array.IndexOf(args, name);
            return i >= 0 && i + 1 < args.Length ? args[i + 1] : null;
        }
        int? w = null, h = null;
        var size = Value("--size")?.Split('x');
        if (size is { Length: 2 } && int.TryParse(size[0], out var sw) && int.TryParse(size[1], out var sh)) { w = sw; h = sh; }
        int? scroll = int.TryParse(Value("--scroll"), out var sc) ? sc : null;
        return new Options(Value("--section"), Value("--lang"), Value("--theme"), w, h, scroll, Value("--report-out"),
            Value("--efi-folder"), Value("--package"), Value("--plan"), args.Contains("--card-confirmed"), args.Contains("--helper-for-folders"),
            Value("--bios-vendor"), Value("--test-vhdx"), args.Contains("--stick-confirm"), Value("--bios-html"), Value("--recovery-dir"));
    }
}
