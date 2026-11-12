// SPDX-License-Identifier: MIT
using IntelXe.App.Pages;
using Microsoft.UI;
using Microsoft.UI.Windowing;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.Graphics;

namespace IntelXe.App;

/// The window: Mica, the app's own title bar, and the sidebar with the macOS app's groups (Status, Install, Help) and
/// the pages this version has.
public sealed partial class MainWindow : Window
{
    public MainWindow()
    {
        InitializeComponent();
        SystemBackdrop = new MicaBackdrop();
        ExtendsContentIntoTitleBar = true;
        SetTitleBar(AppTitleBar);
        AppWindow.SetIcon(Path.Combine(AppContext.BaseDirectory, "Assets", "AppIcon.ico"));
        AppWindow.TitleBar.ButtonBackgroundColor = Colors.Transparent;
        AppWindow.TitleBar.ButtonInactiveBackgroundColor = Colors.Transparent;

        double scale = Root.XamlRoot?.RasterizationScale ?? GetDpiScale();
        AppWindow.Resize(new SizeInt32((int)((App.Options.Width ?? 1080) * scale), (int)((App.Options.Height ?? 780) * scale)));
        if (AppWindow.Presenter is OverlappedPresenter p)
        {
            p.PreferredMinimumWidth = (int)(640 * scale);
            p.PreferredMinimumHeight = (int)(480 * scale);
        }

        ApplyTheme(App.Options.Theme ?? Settings.Current.Theme);
        ApplyLanguage();
        Root.ActualThemeChanged += (_, _) => UpdateTitleBarButtons();

        var start = App.Options.Section switch { "efi" => ItemEfi, "report" => ItemReport, "about" => ItemAbout, _ => ItemOverview };
        Nav.SelectedItem = start;
        _ = AppState.Current.RefreshAsync();
    }

    double GetDpiScale()
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(this);
        return Native.GetDpiForWindow(hwnd) / 96.0;
    }

    /// The sidebar's texts in the language chosen; the page shown is built again.
    public void ApplyLanguage()
    {
        Title = L.Get("App_Name");
        AppTitle.Text = L.Get("App_Name");
        HeaderStatus.Content = L.Get("Group_Status");
        HeaderInstall.Content = L.Get("Group_Install");
        HeaderHelp.Content = L.Get("Group_Help");
        ItemOverview.Content = L.Get("Nav_Overview");
        ItemEfi.Content = L.Get("Nav_Efi");
        ItemReport.Content = L.Get("Nav_Report");
        ItemAbout.Content = L.Get("Nav_About");
        if (Nav.SelectedItem is NavigationViewItem item) Show(item);
    }

    /// "light", "dark" or anything else for the system's.
    public void ApplyTheme(string? theme)
    {
        Root.RequestedTheme = theme switch { "light" => ElementTheme.Light, "dark" => ElementTheme.Dark, _ => ElementTheme.Default };
        UpdateTitleBarButtons();
    }

    void UpdateTitleBarButtons()
    {
        bool dark = Root.ActualTheme == ElementTheme.Dark;
        AppWindow.TitleBar.ButtonForegroundColor = dark ? Colors.White : Colors.Black;
        AppWindow.TitleBar.ButtonHoverForegroundColor = dark ? Colors.White : Colors.Black;
        AppWindow.TitleBar.ButtonHoverBackgroundColor = dark ? ColorHelper.FromArgb(0x18, 0xFF, 0xFF, 0xFF) : ColorHelper.FromArgb(0x10, 0, 0, 0);
        AppWindow.TitleBar.ButtonInactiveForegroundColor = dark ? ColorHelper.FromArgb(0x80, 0xFF, 0xFF, 0xFF) : ColorHelper.FromArgb(0x80, 0, 0, 0);
    }

    void OnSelectionChanged(NavigationView sender, NavigationViewSelectionChangedEventArgs args)
    {
        if (args.SelectedItem is NavigationViewItem item) Show(item);
    }

    void Show(NavigationViewItem item)
    {
        var type = (item.Tag as string) switch
        {
            "efi" => typeof(EfiPage),
            "report" => typeof(ReportPage),
            "about" => typeof(AboutPage),
            _ => typeof(OverviewPage),
        };
        ContentFrame.Navigate(type, null, new Microsoft.UI.Xaml.Media.Animation.EntranceNavigationTransitionInfo());
        ContentFrame.BackStack.Clear();
    }

    /// Opens another page, as the macOS app's "Open the ... page" buttons do.
    public void Open(string tag)
    {
        Nav.SelectedItem = tag switch { "efi" => ItemEfi, "report" => ItemReport, "about" => ItemAbout, _ => ItemOverview };
    }
}
