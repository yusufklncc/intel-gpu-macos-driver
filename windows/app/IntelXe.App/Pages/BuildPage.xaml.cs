// SPDX-License-Identifier: MIT
// The "Build the EFI" page: an OpenCore EFI for this PC, made by the builder beside the app (windows/efibuild in the
// embedded Python, driving a pinned OpCore Simplify without questions; docs/design/DESIGN-efi-build.md):
// 1. scan this PC (Hardware Sniffer and the ACPI tables the build needs, through the elevated helper: one UAC prompt);
// 2. the macOS version (26 Tahoe, default; 15 Sequoia); 3. build (as the user; the program's screens and the rule that
// answered each question are shown as they come; it can be cancelled); 4. the result: every recorded answer, the
// builder's changes, ocvalidate's verdict, the app's own engine on stage 2, the firmware steps and what comes after the
// install, PlatformInfo with the identity masked; 5. save the EFI folder, or use it for the USB installer stick.
// The SMBIOS serial number, MLB, UUID and ROM stay in config.plist: the builder and the app remove them from everything
// shown, and the window masks them. This page writes only into the app's own data folder and into a new EFI folder
// inside a folder the user chooses.
using IntelXe.Core;
using IntelXe.Core.EfiBuild;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IntelXe.App.Pages;

public sealed partial class BuildPage : Page
{
    static BuildSession S => BuildSession.Current;

    public BuildPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { S.Changed += OnChanged; ActualThemeChanged += OnTheme; S.Load(); Build(); };
        Unloaded += (_, _) => { S.Changed -= OnChanged; ActualThemeChanged -= OnTheme; };
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);
    void OnTheme(FrameworkElement sender, object args) => Build();

    bool Busy => S.Scanning || S.Building;

    void Build()
    {
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Build"), L.Get("Build_Caption")));
        if (!S.Builder.IsAvailable)
            column.Children.Add(new InfoBar
            {
                Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false,
                Title = L.Get("Build_Unavailable"), Message = L.Format("Build_Unavailable_Detail", S.Builder.Unavailable),
            });
        column.Children.Add(ScanSection());
        column.Children.Add(VersionSection());
        column.Children.Add(RunSection());
        if (S.Result is { } r) column.Children.Add(ResultSection(r));
        if (S.Result is { Ok: true }) column.Children.Add(SaveSection());
        var note = Ui.Text(S.Builder.IsAvailable ? L.Format("Build_Footer", S.Builder.Versions) : L.Get("Build_Footer_NoBuilder"), "CaptionTextBlockStyle", secondary: true);
        note.Margin = new Thickness(4, 14, 0, 0);
        column.Children.Add(note);
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    // ---- 1. scan ----
    UIElement ScanSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Build_Scan_Caption"), L.Get("Build_Scan_Detail")) };
        if (S.Scanning)
        {
            items.Add(Ui.Horizontal(10, new ProgressRing { IsActive = true, Width = 20, Height = 20 }, Ui.Text(L.Get("Build_Scanning"), secondary: true)));
        }
        else if (S.Scan is { } scan)
        {
            items.Add(Note(L.Format("Build_Scan_Done", scan.Tables.Count), CheckState.Good));
            items.Add(Ui.Text(string.Join(", ", scan.Tables), "CaptionTextBlockStyle", secondary: true));
        }
        if (S.ScanError is { } err) items.Add(Note(L.Format("Build_Scan_Failed", err), CheckState.Bad));
        var b = Ui.Button(L.Get(S.Scan is null ? "Build_Scan_Button" : "Build_Scan_Again"), async (_, _) => await S.ScanAsync(), accent: S.Scan is null, glyph: Glyphs.Pc);
        b.IsEnabled = S.Builder.IsAvailable && !Busy;
        items.Add(b);
        items.Add(Ui.Text(L.Get("Build_Scan_Uac"), "CaptionTextBlockStyle", secondary: true));
        return Ui.Section(Glyphs.Pc, L.Get("Build_Scan"), items.ToArray());
    }

    // ---- 2. macOS ----
    UIElement VersionSection()
    {
        var radios = new RadioButtons { MaxColumns = 1 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(radios, L.Get("Build_Version"));
        radios.Items.Add(new RadioButton { Content = L.Get("Build_Version_26"), Tag = "26", IsEnabled = !Busy });
        radios.Items.Add(new RadioButton { Content = L.Get("Build_Version_15"), Tag = "15", IsEnabled = !Busy });
        radios.SelectedIndex = S.MacOS == "15" ? 1 : 0;
        radios.SelectionChanged += (_, _) => { if (radios.SelectedItem is RadioButton { Tag: string v } && v != S.MacOS) { S.MacOS = v; Build(); } };
        return Ui.Section(Glyphs.List, L.Get("Build_Version"), Ui.Caption(L.Get("Build_Version_Caption")), radios);
    }

    // ---- 3. build ----
    UIElement RunSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Build_Run_Caption"), L.Get("Build_Run_Detail")) };
        if (S.Scan is null) items.Add(Note(L.Get("Build_Run_NeedScan"), CheckState.Neutral));
        if (S.Building)
        {
            items.Add(new ProgressBar { IsIndeterminate = true });
            string[] lines;
            lock (S.Progress) lines = [.. S.Progress.TakeLast(14)];
            if (lines.Length > 0) items.Add(Mono(string.Join("\n", lines)));
            items.Add(Ui.Button(L.Get("Build_Cancel"), (_, _) => S.CancelBuild()));
        }
        else
        {
            var b = Ui.Button(L.Get(S.Result is { Ok: true } ? "Build_Run_Again" : "Build_Run_Button"), async (_, _) => await S.BuildAsync(),
                accent: S.Scan is not null && S.Result is not { Ok: true }, glyph: Glyphs.Settings);
            b.IsEnabled = S.Builder.IsAvailable && S.Scan is not null && !Busy;
            items.Add(b);
        }
        return Ui.Section(Glyphs.Settings, L.Get("Build_Run"), items.ToArray());
    }

    // ---- 4. the result ----
    UIElement ResultSection(EfiBuildResult r)
    {
        var items = new List<UIElement>
        {
            r.Ok ? Note(L.Format("Build_Ok", r.MacOS, r.OpenCore, r.Smbios), CheckState.Good) : Note(L.Format("Build_Failed", r.Error ?? ""), CheckState.Bad),
        };
        if (r.Ok)
        {
            // ocvalidate of the same OpenCore release
            var oc = r.OcValidate;
            items.Add(Ui.Text(L.Get("Build_OcValidate"), "BodyStrongTextBlockStyle"));
            items.Add(Note(oc.Error is not null ? L.Format("Build_OcValidate_Error", oc.Error)
                : oc.Ok ? (oc.Found == 0 ? L.Get("Build_OcValidate_Clean") : L.Format("Build_OcValidate_Accepted", oc.Found))
                : L.Format("Build_OcValidate_Issues", oc.Issues.Count), oc.Ok ? CheckState.Good : CheckState.Bad));
            foreach (var (finding, why) in oc.Accepted) items.Add(Bullet(finding, why));
            foreach (var issue in oc.Issues) items.Add(Bullet(issue, null));

            // the app's own engine (IntelXe-EFI.ps1's) on the same rules
            if (S.Stage2Left is { } left)
                items.Add(left.Count == 0 ? Note(L.Get("Build_Stage2_Agrees"), CheckState.Good)
                    : Note(L.Format("Build_Stage2_Differs", string.Join("; ", left)), CheckState.Warning));
        }
        foreach (var f in r.Failures) items.Add(Bullet(f, null));

        if (r.Decisions.Count > 0)
        {
            items.Add(Ui.Text(L.Format("Build_Answers", r.Decisions.Count), "BodyStrongTextBlockStyle"));
            items.Add(Ui.Text(L.Get("Build_Answers_Caption"), "CaptionTextBlockStyle", secondary: true));
            var list = new StackPanel { Spacing = 8 };
            foreach (var d in r.Decisions)
                list.Children.Add(Bullet(L.Format("Build_Answer_Line", d.Screen, d.Question, d.Answer), L.Format("Build_Answer_Why", d.Rule, d.Why)));
            items.Add(new Expander { Header = L.Get("Build_Answers_Show"), Content = list, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch });
        }
        if (r.Changes.Count > 0)
        {
            var list = new StackPanel { Spacing = 8 };
            foreach (var c in r.Changes) list.Children.Add(Bullet($"{c.Step}: {c.Change}", c.Why.Length > 0 ? c.Why : null));
            items.Add(new Expander { Header = L.Format("Build_Changes", r.Changes.Count), Content = list, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch });
        }
        if (r.Firmware.Count > 0)
        {
            items.Add(Ui.Text(L.Get("Build_Firmware"), "BodyStrongTextBlockStyle"));
            foreach (var f in r.Firmware) items.Add(Bullet(f, null));
            items.Add(Ui.Button(L.Get("Build_Open_Bios"), (_, _) => App.Window?.Open("bios"), glyph: Glyphs.Firmware));
        }
        if (r.AfterInstall.Count > 0)
        {
            items.Add(Ui.Text(L.Get("Build_AfterInstall"), "BodyStrongTextBlockStyle"));
            foreach (var a in r.AfterInstall) items.Add(Bullet(a, null));
        }
        if (S.Platform.Count > 0)
        {
            var rows = new StackPanel { Spacing = 4 };
            foreach (var (key, value) in S.Platform) rows.Children.Add(Ui.Row(key, value));
            rows.Children.Add(Ui.Text(L.Get("Build_Platform_Masked"), "CaptionTextBlockStyle", secondary: true));
            items.Add(new Expander { Header = L.Get("Build_Platform"), Content = rows, HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch });
        }
        var buttons = Ui.Horizontal(10);
        if (r.LogPath.Length > 0 && File.Exists(r.LogPath))
            buttons.Children.Add(Ui.Button(L.Get("Build_Open_Log"), (_, _) => Shell(r.LogPath), glyph: Glyphs.Document));
        if (r.EfiDir.Length > 0 && Directory.Exists(r.EfiDir))
            buttons.Children.Add(Ui.Button(L.Get("Build_Open_Folder"), (_, _) => Shell(r.EfiDir), glyph: Glyphs.Folder));
        if (buttons.Children.Count > 0) items.Add(buttons);
        items.Add(Ui.Text(L.Get("Build_English"), "CaptionTextBlockStyle", secondary: true));
        return Ui.Section(r.Ok ? Glyphs.Ok : Glyphs.Warning, L.Get("Build_Result"), items.ToArray());
    }

    // ---- 5. save ----
    UIElement SaveSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Build_Save_Caption"), L.Get("Build_Save_Detail")) };
        if (S.Saved is { } saved) items.Add(Note(L.Format("Build_Saved", saved), CheckState.Good));
        if (S.SaveError is { } err) items.Add(Note(err, CheckState.Bad));
        var save = Ui.Button(L.Get("Build_Save_Button"), async (_, _) => await Save(), glyph: Glyphs.Folder);
        save.IsEnabled = !Busy;
        var stick = Ui.Button(L.Get("Build_Use_Stick"), (_, _) => App.Window?.Open("stick"), accent: true, glyph: Glyphs.Drive);
        stick.IsEnabled = !Busy;
        items.Add(Ui.Horizontal(10, stick, save));
        items.Add(Ui.Text(L.Get("Build_Save_Next"), "CaptionTextBlockStyle", secondary: true));
        return Ui.Section(Glyphs.Folder, L.Get("Build_Save"), items.ToArray());
    }

    async Task Save()
    {
        if (S.Result is not { Ok: true }) return;
        var r = S.Result;
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker();
            picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
            var folder = await picker.PickSingleFolderAsync();
            if (folder is null) return;
            S.Saved = await Task.Run(() => EfiBuildStore.SaveTo(r.EfiDir, folder.Path));
            S.SaveError = null;
        }
        catch (Exception e) { S.Saved = null; S.SaveError = e.Message; }
        Build();
    }

    static void Shell(string path)
    {
        try { System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo(path) { UseShellExecute = true }); } catch (Exception) { }
    }

    // ---- pieces ----
    static UIElement Note(string text, CheckState state)
    {
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var icon = Ui.Icon(state == CheckState.Neutral ? Glyphs.Info : Ui.StateGlyph(state), 14, Ui.Brush(state));
        icon.VerticalAlignment = VerticalAlignment.Top;
        icon.Margin = new Thickness(0, 3, 0, 0);
        g.Children.Add(icon);
        var t = Ui.Text(text);
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        return g;
    }

    /// A line and, under it, its reason in the caption style.
    static UIElement Bullet(string text, string? why)
    {
        var p = new StackPanel { Spacing = 1, Margin = new Thickness(4, 0, 0, 0) };
        p.Children.Add(Ui.Text("• " + text));
        if (!string.IsNullOrEmpty(why))
        {
            var w = Ui.Text(why, "CaptionTextBlockStyle", secondary: true);
            w.Margin = new Thickness(12, 0, 0, 0);
            p.Children.Add(w);
        }
        return p;
    }

    static Border Mono(string text)
    {
        var t = new TextBlock
        {
            Text = text, FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12, TextWrapping = TextWrapping.Wrap,
            IsTextSelectionEnabled = true, Margin = new Thickness(10, 8, 10, 8),
        };
        return new Border
        {
            Child = t, CornerRadius = new CornerRadius(4), BorderThickness = new Thickness(1),
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorSecondaryBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
        };
    }
}
