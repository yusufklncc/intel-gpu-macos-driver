// SPDX-License-Identifier: MIT
// The EFI page: stages 2 and 4 of the install guide, the Safari setting and undo, on an OpenCore EFI, by the shared EFI
// rules (efi-rules.json) run by IntelXe-EFI.ps1's engine in the Core. Each change shows its plan first (steps, notes,
// where the backup goes) and is made with that plan's SHA-256 only; the whole EFI folder is backed up first, in the
// script's place and format, so that the script and the app undo each other's changes. An EFI folder (a USB stick) is
// changed in this process; an EFI partition by the elevated helper (this exe with --efi-helper, through "runas").
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXeEfi;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IntelXe.App.Pages;

public sealed partial class EfiPage : Page
{
    /// What the page keeps while the app runs: across rebuilds (a refresh of the facts, a new package) and visits.
    sealed class PageState
    {
        public readonly List<EfiTarget.Folder> Folders = [];
        public EfiTarget? Target;
        public string? CardText;
        public bool CardConfirmed = App.Options.CardConfirmed;
        public bool WithSafari;
        public EfiStatus? Status;
        public EfiTarget? StatusFor;
        public string? Busy;
        public bool PlanOpened;
        public readonly Dictionary<string, (InfoBarSeverity Severity, string Title, string Message)> Results = [];
    }

    static readonly PageState S = Initial();

    static PageState Initial()
    {
        var s = new PageState();
        if (App.Options.EfiFolder is { Length: > 0 } f) { var t = new EfiTarget.Folder(f); s.Folders.Add(t); s.Target = t; }
        return s;
    }

    ComboBox? targets;

    public EfiPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { AppState.Current.Changed += OnChanged; AppState.Current.FindPackage(); Build(); };
        Unloaded += (_, _) => AppState.Current.Changed -= OnChanged;
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);

    static IEfiPlanner Planner => AppState.Current.Planner;
    static Rules? RulesOrNull { get { try { return EfiRules.Embedded; } catch (Exception) { return null; } } }

    /// The card's port as Windows reports it (EfiFolder.ConvertLocationPath, as the script's "Read from Windows"):
    /// \_SB.PC00.PEG1.PEGP, "none" without an ACPI name, "" without a card or a Location path.
    static string Detected(PcFacts? f)
    {
        var loc = f?.Card?.LocationPath ?? "";
        if (loc.Length == 0 || RulesOrNull is not { } r) return "";
        return EfiFolder.ConvertLocationPath(loc, r.Int("cards.port_levels_above_gpu")) ?? "";
    }

    static EfiOptions Options()
    {
        var f = AppState.Current.Facts;
        var card = S.CardConfirmed ? (S.CardText ?? "").Trim() : "";
        return new EfiOptions(AppState.Current.Package?.Dir ?? "", card, Detected(f), f?.CpuName ?? "", false, null);
    }

    void Build()
    {
        var state = AppState.Current;
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Efi"), L.Get("Efi_Caption")));

        if (!Planner.IsAvailable)
            column.Children.Add(new InfoBar
            {
                Title = L.Get("Efi_Pending_Title"), Message = L.Get(PendingEfiPlanner.MessageKey) + " " + (Planner as PendingEfiPlanner)?.Message,
                Severity = InfoBarSeverity.Error, IsOpen = true, IsClosable = false,
                ActionButton = Ui.Link(L.Get("Efi_Open_Windows_Guide"), new Uri(Project.WindowsGuideUrl)),
            });
        if (S.Busy is { } busy)
        {
            var p = new StackPanel { Spacing = 6, Margin = new Thickness(0, 0, 0, 4) };
            p.Children.Add(Ui.Text(busy, secondary: true));
            p.Children.Add(new ProgressBar { IsIndeterminate = true });
            column.Children.Add(p);
        }

        column.Children.Add(Ui.Section(Glyphs.Drive, L.Get("Efi_Partition"), TargetChooser(state.Facts)));
        column.Children.Add(Ui.Section(Glyphs.Folder, L.Get("Pkg_Title"), PackageSection()));

        var safari = new CheckBox { Content = L.Get("Efi_With_Safari"), IsChecked = S.WithSafari };
        safari.Checked += (_, _) => S.WithSafari = true;
        safari.Unchecked += (_, _) => S.WithSafari = false;
        column.Children.Add(Ui.Section(Glyphs.Settings, L.Get("Efi_Stage2"),
            Ui.Caption(L.Get("Efi_Stage2_Caption"), L.Get("Efi_Stage2_Detail")),
            Bullets("Efi_Stage2_Item1", "Efi_Stage2_Item2", "Efi_Stage2_Item3", "Efi_Stage2_Item4"),
            Ui.Text(L.Get("Efi_Stage2_When"), "CaptionTextBlockStyle", secondary: true),
            Ui.Horizontal(4, safari, Ui.InfoButton(L.Get("Efi_With_Safari_Detail"))),
            Actions("settings", () => S.WithSafari ? EfiChange.SettingsSafari : EfiChange.Settings)));

        column.Children.Add(Ui.Section(Glyphs.Shield, L.Get("Efi_Safari"),
            Ui.Caption(L.Get("Efi_Safari_Caption"), L.Get("Efi_Safari_Detail")),
            Actions("safari", () => EfiChange.Safari)));

        var f = state.Facts;
        string hybrid = f is null ? "–" : f.HybridCpu ? L.Format("Efi_HybridPM_Yes", CpuInfo.CleanName(f.CpuName)) : L.Get("Efi_HybridPM_No");
        column.Children.Add(Ui.Section(Glyphs.Puzzle, L.Get("Efi_Stage4"),
            Ui.Caption(L.Get("Efi_Stage4_Caption"), L.Get("Efi_Stage4_Detail")),
            Bullets("Efi_Stage4_Item1", "Efi_Stage4_Item2", "Efi_Stage4_Item3", "Efi_Stage4_Item4"),
            CardPath(f),
            Ui.Row(L.Get("Efi_HybridPM"), hybrid),
            Actions("driver", () => EfiChange.Driver, needsFacts: true)));

        column.Children.Add(Ui.Section(Glyphs.Undo, L.Get("Efi_Undo"),
            Ui.Caption(L.Get("Efi_Undo_Caption")),
            Actions("undo", () => EfiChange.Undo)));

        Host.Children.Clear();
        Host.Children.Add(scroll);

        // --plan KIND: that plan's dialog, once, when this PC's facts are in (the driver's plan needs the CPU and card)
        if (!S.PlanOpened && EfiChanges.FromKind(App.Options.Plan) is { } open && f is not null && !state.Probing)
        {
            S.PlanOpened = true;
            DispatcherQueue.TryEnqueue(async () => await PlanAndApply(open == EfiChange.SettingsSafari ? "settings" : open.Kind(), open));
        }
    }

    // ---- the EFI ------------------------------------------------------------------------------------------------------
    /// The EFI to work on: an EFI partition of this PC (as the Overview lists them) or a folder chosen here. Choosing
    /// changes nothing; reading an EFI partition mounts it, through the helper, and unmounts it again.
    UIElement TargetChooser(PcFacts? f)
    {
        targets = new ComboBox { MinWidth = 320, PlaceholderText = L.Get("Efi_Choose") };
        foreach (var e in f?.Esps ?? [])
        {
            var label = $"{e.Disk} · {e.SizeText} · " + L.Format("Esp_Where", e.DiskNumber, e.PartitionNumber) + (e.IsSystem ? " · " + L.Get("Esp_System") : "");
            targets.Items.Add(new ComboBoxItem { Content = label, Tag = new EfiTarget.Partition(e.DiskNumber, e.PartitionNumber) });
        }
        foreach (var folder in S.Folders) targets.Items.Add(new ComboBoxItem { Content = folder.Path, Tag = folder });
        var selected = targets.Items.Cast<ComboBoxItem>().FirstOrDefault(i => Equals(i.Tag, S.Target));
        if (selected is not null) targets.SelectedItem = selected;
        else if (targets.Items.Count > 0) { targets.SelectedIndex = 0; S.Target = (EfiTarget?)((ComboBoxItem)targets.Items[0]).Tag; }
        targets.SelectionChanged += (_, _) =>
        {
            S.Target = (targets.SelectedItem as ComboBoxItem)?.Tag as EfiTarget;
            Build();
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(targets, L.Get("Efi_Partition"));
        var folderButton = Ui.Button(L.Get("Efi_Use_Folder"), async (_, _) => await PickFolder());
        var read = Ui.Button(L.Get("Efi_Read"), async (_, _) => await ReadStatus());
        read.IsEnabled = S.Busy is null && Planner.IsAvailable && S.Target is not null;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Ui.Caption(L.Get("Efi_Partition_Caption"), L.Get("Efi_Elevation_Detail")));
        var row = Ui.Horizontal(10, targets, folderButton);
        p.Children.Add(row);
        if (S.Target is EfiTarget.Partition)
            p.Children.Add(Ui.Text(L.Get("Efi_Elevation_Note"), "CaptionTextBlockStyle", secondary: true));
        var readRow = Ui.Horizontal(10, read);
        readRow.HorizontalAlignment = HorizontalAlignment.Right;
        p.Children.Add(readRow);
        if (S.Status is { } st && Equals(S.StatusFor, S.Target)) foreach (var r in StatusRows(st)) p.Children.Add(r);
        Result(p, "status");
        return p;
    }

    async Task PickFolder()
    {
        var picker = new Windows.Storage.Pickers.FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
        var folder = await picker.PickSingleFolderAsync();
        if (folder is null) return;
        var t = new EfiTarget.Folder(folder.Path);
        if (!S.Folders.Contains(t)) S.Folders.Add(t);
        S.Target = t;
        Build();
    }

    async Task ReadStatus()
    {
        if (S.Target is not { } target || S.Busy is not null) return;
        SetBusy(target);
        try
        {
            S.Status = await Planner.ReadStatusAsync(target, Options());
            S.StatusFor = target;
            S.Results.Remove("status");
        }
        catch (Exception e)
        {
            S.Status = null;
            S.Results["status"] = (InfoBarSeverity.Error, L.Get("Efi_Read_Failed"), Shown(e));
        }
        finally { S.Busy = null; Build(); }
    }

    /// What -Status says about the EFI, in the macOS EFI page's words.
    static IEnumerable<UIElement> StatusRows(EfiStatus s)
    {
        string File(string state, string version) => (state switch
        {
            "absent" => L.Get("Efi_File_Absent"), "same" => L.Get("Efi_File_Same"), "different" => L.Get("Efi_File_Different"),
            "present" => L.Get("Efi_File_Present"), _ => L.Get("Efi_File_Unknown"),
        }) + (version.Length > 0 && state != "absent" ? " · " + version : "");
        string Entry(string state) => state switch
        {
            "enabled" => L.Get("Efi_Entry_On"), "disabled" => L.Get("Efi_Entry_Off"), "absent" => L.Get("Efi_Entry_Absent"), _ => "",
        };
        string Both(string file, string version, string entry) => File(file, version) + (Entry(entry) is { Length: > 0 } e ? "; " + e : "");
        string Count(string n) => n switch { "" => L.Get("Efi_St_NeedsPackage"), "0" => L.Get("Efi_St_Done"), _ => L.Format("Efi_St_ToChange", n) };

        yield return Ui.Row(L.Get("Efi_St_OpenCore"), s["opencore"] == "yes" ? L.Get("Esp_OpenCore_Found") : L.Get("Efi_St_NotOpenCore"));
        if (s["efi_readable"] != "yes") { yield return Ui.Row(L.Get("Efi_St_Config"), s["efi_read_error"]); yield break; }
        yield return Ui.Row(L.Get("Efi_Stage2"), Count(s["settings_changes"]));
        yield return Ui.Row(L.Get("Efi_Stage4"), Count(s["plan_changes"]));
        yield return Ui.Row(L.Get("Efi_St_Kext"), Both(s["kext_file"], s["kext_version_efi"], s["kext_entry"]));
        yield return Ui.Row(L.Get("Efi_St_Ssdt"), Both(s["ssdt_file"], "", s["ssdt_entry"]));
        yield return Ui.Row(L.Get("Efi_St_HybridPM"), s["hybridpm_needed"] == "yes" || s["hybridpm_file"] != "absent"
            ? Both(s["hybridpm_file"], s["hybridpm_version_efi"], s["hybridpm_entry"]) : L.Get("Efi_St_NotNeeded"));
        yield return Ui.Row(L.Get("Efi_Safari"), s["secure_boot_model"] == "Disabled" ? L.Get("Efi_St_SecureBootOff") : L.Format("Efi_St_SecureBoot", s["secure_boot_model"]));
        yield return Ui.Row(L.Get("Efi_St_Backup"), s["backup"] is "none" or "" ? L.Get("Efi_St_NoBackup") : $"{s["backup_kind"]} · {s["backup"]}");
    }

    // ---- the package --------------------------------------------------------------------------------------------------
    UIElement PackageSection()
    {
        var state = AppState.Current;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Ui.Caption(L.Get("Pkg_Caption"), L.Get("Pkg_Detail")));
        if (state.Package is { } pkg)
        {
            p.Children.Add(Ui.Row(L.Get("Pkg_Folder"), pkg.Dir));
            if (pkg.Problem is not null) p.Children.Add(Ui.Row(L.Get("Pkg_Problem"), pkg.Problem));
            else
            {
                p.Children.Add(Ui.Row("IntelXe.kext", pkg.KextVersion));
                p.Children.Add(Ui.Row("HybridPM.kext", pkg.HybridPMVersion.Length > 0 ? pkg.HybridPMVersion : L.Get("Pkg_NoHybridPM")));
                var (sums, good) = pkg.Checksums switch
                {
                    "ok" => (L.Get("Pkg_Sums_Ok"), CheckState.Good),
                    "no SHA256SUMS" => (L.Get("Pkg_Sums_None"), CheckState.Warning),
                    _ => (L.Format("Pkg_Sums_Bad", pkg.Checksums), CheckState.Bad),
                };
                p.Children.Add(Ui.Row("SHA256SUMS", Ui.Badge(sums, good)));
            }
        }
        else p.Children.Add(Ui.Text(L.Get("Pkg_None"), secondary: true));
        if (state.PackageError is { } err)
            p.Children.Add(new InfoBar { Title = L.Get("Pkg_Error"), Message = err, Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false });
        var row = Ui.Horizontal(10,
            Ui.Button(L.Get("Pkg_Choose_Zip"), async (_, _) => await PickPackage(zip: true)),
            Ui.Button(L.Get("Pkg_Choose_Folder"), async (_, _) => await PickPackage(zip: false)));
        row.HorizontalAlignment = HorizontalAlignment.Right;
        p.Children.Add(row);
        return p;
    }

    async Task PickPackage(bool zip)
    {
        var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.Window);
        string? path;
        if (zip)
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".zip");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            path = (await picker.PickSingleFileAsync())?.Path;
        }
        else
        {
            var picker = new Windows.Storage.Pickers.FolderPicker();
            picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
            path = (await picker.PickSingleFolderAsync())?.Path;
        }
        if (path is null) return;
        S.Busy = L.Get("Pkg_Reading");
        Build();
        await Task.Run(() => AppState.Current.UsePackage(path));
        S.Busy = null;
        Build();
    }

    // ---- stage 4: the card's ACPI path, confirmed by the user (D3) --------------------------------------------------------
    UIElement CardPath(PcFacts? f)
    {
        var detected = Detected(f);
        string shown = f?.Card is null ? L.Get("Card_None")
            : detected switch { "" => L.Get("Value_CouldNotRead"), "none" => L.Get("Efi_Acpi_None"), _ => detected };
        // filled from Windows once this PC's facts are in; the user's text after that
        if (S.CardText is null && f is not null) S.CardText = detected is "" or "none" ? "" : detected;
        var p = new StackPanel { Spacing = 8 };
        p.Children.Add(Ui.Row(L.Get("Efi_Acpi"), shown));
        var box = new TextBox { Text = S.CardText ?? "", PlaceholderText = RulesOrNull?.Str("ssdt.shipped_port") ?? AcpiPath.Shipped, FontFamily = new FontFamily("Consolas"), MinWidth = 280 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, L.Get("Efi_Acpi_Box"));
        box.TextChanged += (_, _) => { if (box.Text != (S.CardText ?? "")) S.CardText = box.Text; };
        var confirm = new CheckBox { Content = L.Get("Efi_Acpi_Confirm"), IsChecked = S.CardConfirmed };
        confirm.Checked += (_, _) => S.CardConfirmed = true;
        confirm.Unchecked += (_, _) => S.CardConfirmed = false;
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var label = Ui.Text(L.Get("Efi_Acpi_Box"));
        label.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(label);
        Grid.SetColumn(box, 1);
        g.Children.Add(box);
        p.Children.Add(g);
        p.Children.Add(confirm);
        p.Children.Add(Ui.Text(L.Format("Efi_Acpi_Hint", RulesOrNull?.Str("ssdt.shipped_port") ?? AcpiPath.Shipped), "CaptionTextBlockStyle", secondary: true));
        return p;
    }

    static UIElement Bullets(params string[] keys)
    {
        var p = new StackPanel { Spacing = 4 };
        foreach (var k in keys)
        {
            var g = new Grid { ColumnSpacing = 8 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.Children.Add(new TextBlock { Text = "•" });
            var t = Ui.Text(L.Get(k));
            Grid.SetColumn(t, 1);
            g.Children.Add(t);
            p.Children.Add(g);
        }
        return p;
    }

    // ---- plan, confirm, change ------------------------------------------------------------------------------------------
    /// The section's button: the plan in a dialog, which makes the change only when the user chooses it there. The last
    /// answer shows under the button.
    UIElement Actions(string key, Func<EfiChange> change, bool needsFacts = false)
    {
        var plan = Ui.Button(L.Get(key == "undo" ? "Efi_Undo_Plan" : "Efi_Show_Plan"), async (_, _) => await PlanAndApply(key, change()), accent: key == "settings");
        plan.IsEnabled = S.Busy is null && Planner.IsAvailable && S.Target is not null && (!needsFacts || AppState.Current.Facts is not null);
        var row = Ui.Horizontal(10, plan);
        row.HorizontalAlignment = HorizontalAlignment.Right;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(row);
        Result(p, key);
        return p;
    }

    static void Result(StackPanel p, string key)
    {
        if (!S.Results.TryGetValue(key, out var r)) return;
        var bar = new InfoBar { Severity = r.Severity, Title = r.Title, Message = r.Message, IsOpen = true, IsClosable = true };
        bar.Closed += (_, _) => S.Results.Remove(key);
        p.Children.Add(bar);
    }

    void SetBusy(EfiTarget target)
    {
        S.Busy = L.Get(target is EfiTarget.Partition ? "Efi_Busy_Partition" : "Efi_Busy");
        Build();
    }

    async Task PlanAndApply(string key, EfiChange change)
    {
        if (S.Target is not { } target || S.Busy is not null) return;
        var options = Options();
        EfiPlan plan;
        SetBusy(target);
        try
        {
            plan = await Planner.PlanAsync(target, change, options);
            S.Results.Remove(key);
        }
        catch (Exception e)
        {
            S.Results[key] = (InfoBarSeverity.Error, L.Get("Efi_Plan_Failed"), Shown(e));
            return;
        }
        finally { S.Busy = null; Build(); }

        if (!await ConfirmPlan(plan))
        {
            S.Results[key] = (InfoBarSeverity.Informational, L.Get(plan.HasChanges ? "Efi_Cancelled" : "Efi_Plan_Title"),
                plan.HasChanges ? L.Get("Efi_Nothing_Written") : L.Get(change == EfiChange.Undo ? "Efi_Nothing_To_Undo" : "Efi_Nothing_To_Change"));
            Build();
            return;
        }
        SetBusy(target);
        EfiResult? result = null;
        string? error = null;
        try { result = await Planner.ApplyAsync(plan, options); }
        catch (Exception e) { error = Shown(e); }
        finally { S.Busy = null; }
        if (result is not null)
        {
            S.Results[key] = (InfoBarSeverity.Success, L.Get("Efi_Done_Title"), L.Get(change == EfiChange.Undo ? "Efi_Undone_Text" : "Efi_Done_Text"));
            if (Equals(S.StatusFor, target) && target is EfiTarget.Folder)
                try { S.Status = await Planner.ReadStatusAsync(target, options); } catch (Exception) { S.Status = null; }
            else S.Status = null;
        }
        else S.Results[key] = (InfoBarSeverity.Error, L.Get("Efi_Change_Failed"), error ?? "");
        Build();
        await ShowResult(plan, result, error);
    }

    /// A failure's message in the window's words (EngineText: the engine's and the helper's messages; others as they are).
    static string Shown(Exception e) => EngineText.Show(EngineLine.Error, EfiHelper.Message(e), L.Resolve);

    ContentDialog Dialog(string title)
    {
        var d = new ContentDialog { Title = title, XamlRoot = XamlRoot, RequestedTheme = Ui.Root.ActualTheme };
        d.Resources["ContentDialogMaxWidth"] = 900.0;
        return d;
    }

    /// Lines as the engine wrote them: monospace, not wrapped, selectable, scrolled when long or wide.
    static Border Mono(string text, string name)
    {
        var t = new TextBlock
        {
            Text = text, FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12, TextWrapping = TextWrapping.NoWrap,
            IsTextSelectionEnabled = true, Margin = new Thickness(10, 8, 10, 8),
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(t, name);
        var scroll = new ScrollViewer
        {
            Content = t, MaxHeight = 260, HorizontalScrollMode = ScrollMode.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Auto,
            VerticalScrollBarVisibility = ScrollBarVisibility.Auto,
        };
        return new Border
        {
            Child = scroll, CornerRadius = new CornerRadius(4), BorderThickness = new Thickness(1),
            Background = (Brush)Application.Current.Resources["CardBackgroundFillColorSecondaryBrush"],
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"],
        };
    }

    static UIElement Note(string text, CheckState state)
    {
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var icon = Ui.Icon(Ui.StateGlyph(state), 14, Ui.Brush(state));
        icon.VerticalAlignment = VerticalAlignment.Top;
        icon.Margin = new Thickness(0, 3, 0, 0);
        g.Children.Add(icon);
        var t = Ui.Text(text);
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        return g;
    }

    /// The plan: what it does, where the backup goes, its steps and notes, and the button that makes it (the default
    /// button is Cancel, as in the script's window).
    async Task<bool> ConfirmPlan(EfiPlan plan)
    {
        bool undo = plan.Change == EfiChange.Undo;
        var d = Dialog(L.Get(undo ? "Efi_Dialog_Undo" : plan.Change == EfiChange.Driver ? "Efi_Dialog_Driver" : "Efi_Dialog_Settings"));
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Ui.Text(L.Get(plan.Change switch
        {
            EfiChange.Settings => "Efi_What_Settings", EfiChange.Safari => "Efi_What_Safari", EfiChange.SettingsSafari => "Efi_What_SettingsSafari",
            EfiChange.Driver => "Efi_What_Driver", _ => "Efi_What_Undo",
        })));
        if (plan.HasChanges) p.Children.Add(Ui.Text(L.Get(undo ? "Efi_Undo_Safety" : "Efi_Plan_Safety"), secondary: true));
        var efiLine = EngineText.Show(EngineLine.Result, plan.Text.Split('\n')[0], L.Resolve);
        var steps = plan.Lines.Count > 0 ? string.Join("\n", plan.Lines.Select(l => EngineText.Show(EngineLine.Step, l, L.Resolve)))
            : L.Get(undo ? "Efi_Nothing_To_Undo" : "Efi_Nothing_To_Change");
        p.Children.Add(Ui.Text(efiLine, "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(Mono(steps, L.Get("Efi_Plan_Steps")));
        foreach (var n in plan.Notes) p.Children.Add(Note(EngineText.Show(EngineLine.Note, n, L.Resolve), CheckState.Warning));
        var ssdt = RulesOrNull?.Str("ssdt.file") ?? "SSDT-ARCOFF.aml";
        if (!undo && plan.Lines.Any(l => l.Contains(ssdt))) p.Children.Add(Note(L.Get("Efi_Ssdt_Warning"), CheckState.Bad));
        if (plan.HasChanges && plan.BackupDir.Length > 0)
            p.Children.Add(Ui.Text(L.Format(undo ? "Efi_Backups_In" : "Efi_Backup_To", plan.BackupDir), "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(Ui.Text(L.Format("Efi_Plan_Hash", plan.Hash), "CaptionTextBlockStyle", secondary: true));
        d.Content = new ScrollViewer { Content = p, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        if (plan.HasChanges)
        {
            d.PrimaryButtonText = L.Get(undo ? "Efi_Undo_Now" : "Efi_Apply_Now");
            d.CloseButtonText = L.Get("Efi_Cancel");
        }
        else d.CloseButtonText = L.Get("Efi_Close");
        d.DefaultButton = ContentDialogButton.Close;
        return await d.ShowAsync() == ContentDialogResult.Primary;
    }

    async Task ShowResult(EfiPlan plan, EfiResult? result, string? error)
    {
        bool undo = plan.Change == EfiChange.Undo;
        var d = Dialog(L.Get(result is null ? "Efi_Change_Failed" : "Efi_Done_Title"));
        var p = new StackPanel { Spacing = 10 };
        if (result is null) p.Children.Add(Note(error ?? "", CheckState.Bad));
        else
        {
            p.Children.Add(Note(L.Get(undo ? "Efi_Undone_Text" : "Efi_Done_Text"), CheckState.Good));
            if (!undo) p.Children.Add(Ui.Text(L.Get("Efi_Done_Fallback"), secondary: true));
            p.Children.Add(Mono(string.Join("\n", result.Lines.Select(l => EngineText.Show(EngineLine.Result, l, L.Resolve))), L.Get("Efi_Result_Lines")));
        }
        d.Content = new ScrollViewer { Content = p, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        d.CloseButtonText = L.Get("Efi_Close");
        d.DefaultButton = ContentDialogButton.Close;
        await d.ShowAsync();
    }
}
