// SPDX-License-Identifier: MIT
// The EFI page, a placeholder in this version: it describes stages 2 and 4 of the install guide as the macOS EFI page
// and IntelXe-EFI.ps1 make them, shows what this PC tells about them (the card's ACPI path, HybridPM.kext), and its
// buttons go to IEfiPlanner. PendingEfiPlanner answers until the shared rules are wired in; nothing is mounted or
// written.
using IntelXe.Core;
using IntelXe.Core.Efi;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IntelXe.App.Pages;

public sealed partial class EfiPage : Page
{
    ComboBox? targets;

    public EfiPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { AppState.Current.Changed += OnChanged; Build(); };
        Unloaded += (_, _) => AppState.Current.Changed -= OnChanged;
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);

    void Build()
    {
        var state = AppState.Current;
        var planner = state.Planner;
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Efi"), L.Get("Efi_Caption")));

        if (!planner.IsAvailable)
        {
            var bar = new InfoBar
            {
                Title = L.Get("Efi_Pending_Title"), Message = L.Get(PendingEfiPlanner.MessageKey) + " " + L.Get("Efi_Pending_Until"),
                Severity = InfoBarSeverity.Informational, IsOpen = true, IsClosable = false,
                ActionButton = Ui.Link(L.Get("Efi_Open_Windows_Guide"), new Uri(Project.WindowsGuideUrl)),
            };
            column.Children.Add(bar);
        }

        column.Children.Add(Ui.Section(Glyphs.Drive, L.Get("Efi_Partition"), TargetChooser(state.Facts)));

        column.Children.Add(Ui.Section(Glyphs.Settings, L.Get("Efi_Stage2"),
            Ui.Caption(L.Get("Efi_Stage2_Caption"), L.Get("Efi_Stage2_Detail")),
            Bullets("Efi_Stage2_Item1", "Efi_Stage2_Item2", "Efi_Stage2_Item3", "Efi_Stage2_Item4"),
            Ui.Text(L.Get("Efi_Stage2_When"), "CaptionTextBlockStyle", secondary: true),
            Actions(EfiChange.Settings, planner)));

        var f = state.Facts;
        var card = f?.Card;
        string acpi = card is null ? L.Get("Card_None")
            : AcpiPath.FromLocationPath(card.LocationPath) is { } p ? (p == "none" ? L.Get("Efi_Acpi_None") : p) : L.Get("Value_CouldNotRead");
        string hybrid = f is null ? "–" : f.HybridCpu ? L.Format("Efi_HybridPM_Yes", CpuInfo.CleanName(f.CpuName)) : L.Get("Efi_HybridPM_No");
        column.Children.Add(Ui.Section(Glyphs.Puzzle, L.Get("Efi_Stage4"),
            Ui.Caption(L.Get("Efi_Stage4_Caption"), L.Get("Efi_Stage4_Detail")),
            Bullets("Efi_Stage4_Item1", "Efi_Stage4_Item2", "Efi_Stage4_Item3", "Efi_Stage4_Item4"),
            Ui.Row(L.Get("Efi_Acpi"), acpi),
            Ui.Row(L.Get("Efi_HybridPM"), hybrid),
            Actions(EfiChange.Driver, planner)));

        column.Children.Add(Ui.Section(Glyphs.Undo, L.Get("Efi_Undo"),
            Ui.Caption(L.Get("Efi_Undo_Caption")),
            Actions(EfiChange.Undo, planner)));

        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    /// The EFI to work on: an EFI partition of this PC (as the Overview lists them) or a folder chosen here. Choosing
    /// changes nothing; the planner gets it.
    UIElement TargetChooser(PcFacts? f)
    {
        targets = new ComboBox { MinWidth = 320, PlaceholderText = L.Get("Efi_Choose") };
        foreach (var e in f?.Esps ?? [])
        {
            var label = $"{e.Disk} · {e.SizeText} · " + L.Format("Esp_Where", e.DiskNumber, e.PartitionNumber) + (e.IsSystem ? " · " + L.Get("Esp_System") : "");
            targets.Items.Add(new ComboBoxItem { Content = label, Tag = new EfiTarget.Partition(e.DiskNumber, e.PartitionNumber) });
        }
        if (targets.Items.Count > 0) targets.SelectedIndex = 0;
        var folder = Ui.Button(L.Get("Efi_Use_Folder"), async (_, _) => await PickFolder());
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Ui.Caption(L.Get("Efi_Partition_Caption")));
        p.Children.Add(Ui.Horizontal(10, targets, folder));
        return p;
    }

    async Task PickFolder()
    {
        var picker = new Windows.Storage.Pickers.FolderPicker();
        picker.FileTypeFilter.Add("*");
        WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
        var folder = await picker.PickSingleFolderAsync();
        if (folder is null || targets is null) return;
        var item = new ComboBoxItem { Content = folder.Path, Tag = new EfiTarget.Folder(folder.Path) };
        targets.Items.Add(item);
        targets.SelectedItem = item;
    }

    EfiTarget Target => (targets?.SelectedItem as ComboBoxItem)?.Tag as EfiTarget ?? new EfiTarget.Folder("");

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

    /// Show the plan, and apply it (only a planner that is available can; applying asks first, as on macOS). The answer
    /// shows under the buttons.
    UIElement Actions(EfiChange change, IEfiPlanner planner)
    {
        var result = new InfoBar { IsOpen = false, IsClosable = true };
        var plan = Ui.Button(L.Get("Efi_Show_Plan"), async (_, _) => await ShowPlan(change, result), accent: change == EfiChange.Settings);
        var apply = Ui.Button(L.Get(change == EfiChange.Undo ? "Efi_Undo_Apply" : "Efi_Apply"), (_, _) => { });
        apply.IsEnabled = false;   // writes come with the shared rules and a confirmation that shows the plan
        ToolTipService.SetToolTip(apply, L.Get("Efi_Apply_Later"));
        var row = Ui.Horizontal(10, plan, apply);
        row.HorizontalAlignment = HorizontalAlignment.Right;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(row);
        p.Children.Add(result);
        return p;
    }

    async Task ShowPlan(EfiChange change, InfoBar result)
    {
        try
        {
            var plan = await AppState.Current.Planner.PlanAsync(Target, change);
            result.Severity = InfoBarSeverity.Informational;
            result.Title = L.Get("Efi_Plan_Title");
            result.Message = plan.HasChanges ? string.Join("\n", plan.Lines.Concat(plan.Notes)) : L.Get("Efi_Nothing_To_Change");
        }
        catch (EfiUnavailableException)
        {
            result.Severity = InfoBarSeverity.Informational;
            result.Title = L.Get("Efi_Pending_Title");
            result.Message = L.Get(PendingEfiPlanner.MessageKey);
        }
        catch (Exception e)
        {
            result.Severity = InfoBarSeverity.Error;
            result.Title = L.Get("Efi_Plan_Failed");
            result.Message = e.Message;
        }
        result.IsOpen = true;
    }
}
