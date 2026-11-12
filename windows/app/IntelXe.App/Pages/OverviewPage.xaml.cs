// SPDX-License-Identifier: MIT
// The Overview page: this PC as the driver sees it, read-only. The header says where it stands (Views.swift
// StatusHeader), four cards give it at a glance, "This PC" lists each requirement with its state and one line on what
// it means, and the EFI partitions are listed as IntelXe-EFI.ps1 finds them, without mounting anything.
using IntelXe.Core;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IntelXe.App.Pages;

public sealed partial class OverviewPage : Page
{
    public OverviewPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { AppState.Current.Changed += OnChanged; ActualThemeChanged += OnTheme; Build(); };
        Unloaded += (_, _) => { AppState.Current.Changed -= OnChanged; ActualThemeChanged -= OnTheme; };
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);
    void OnTheme(FrameworkElement sender, object args) => Build();

    void Build()
    {
        var state = AppState.Current;
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Header(state));
        if (state.Facts is { } f)
        {
            column.Children.Add(Ui.Section(Glyphs.Grid, L.Get("Section_AtAGlance"), Glance(f, state.Checks)));
            column.Children.Add(ThisPc(state.Checks));
            column.Children.Add(Partitions(f));
            var note = Ui.Text(L.Get("Overview_ReadOnly"), "CaptionTextBlockStyle", secondary: true);
            note.Margin = new Thickness(4, 14, 0, 0);
            column.Children.Add(note);
        }
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    static CheckState StateOf(IReadOnlyList<Check> checks, string id) => checks.FirstOrDefault(c => c.Id == id)?.State ?? CheckState.Neutral;
    static string ValueOf(IReadOnlyList<Check> checks, string id) => checks.FirstOrDefault(c => c.Id == id) is { } c ? L.Resolve(c.Value) : "";

    UIElement Header(AppState state)
    {
        var overall = state.Facts is null ? CheckState.Neutral : PcChecks.Overall(state.Checks);
        var card = state.Facts?.Card;
        string title = state.Facts is null ? L.Get("Checking") : card is null ? L.Get("Card_None") : SupportedCards.Find(card.Id)!.Name;
        string line = state.Facts is null ? L.Get("Overview_Reading") : overall switch
        {
            CheckState.Good => L.Get("Header_Good"),
            CheckState.Warning => L.Get("Header_Warning"),
            _ => L.Get("Header_Bad"),
        };

        var tile = new Border
        {
            Width = 56, Height = 56, CornerRadius = new CornerRadius(12),
            Background = Ui.Brush(overall, 0.14),
            Child = Ui.Icon(Glyphs.Chip, 28, Ui.Brush(overall)),
        };
        var texts = new StackPanel { Spacing = 2, VerticalAlignment = VerticalAlignment.Center };
        var t = Ui.Text(title, "SubtitleTextBlockStyle", wrap: false);
        texts.Children.Add(t);
        texts.Children.Add(Ui.Text(line, "BodyTextBlockStyle", secondary: true));

        var right = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 10, VerticalAlignment = VerticalAlignment.Center };
        if (state.Facts is not null)
        {
            int bad = state.Checks.Count(c => c.State == CheckState.Bad), warn = state.Checks.Count(c => c.State == CheckState.Warning);
            string badge = bad > 0 ? L.Format("Badge_Problems", bad) : warn > 0 ? L.Format("Badge_Warnings", warn) : L.Get("Summary_Met");
            right.Children.Add(Ui.Badge(badge, overall));
        }
        if (state.Probing) right.Children.Add(new ProgressRing { IsActive = true, Width = 20, Height = 20 });
        var refresh = new Button { Content = Ui.Icon(Glyphs.Refresh, 14), IsEnabled = !state.Probing, Padding = new Thickness(8) };
        ToolTipService.SetToolTip(refresh, L.Get("Refresh"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(refresh, L.Get("Refresh"));
        refresh.Click += async (_, _) => await AppState.Current.RefreshAsync();
        right.Children.Add(refresh);

        var g = new Grid { ColumnSpacing = 14 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(tile);
        Grid.SetColumn(texts, 1); g.Children.Add(texts);
        Grid.SetColumn(right, 2); g.Children.Add(right);
        return Ui.Card(g);
    }

    /// One figure: a title, a value, a line under it, on a card tinted by its state (Views.swift OverviewCard).
    static Border GlanceCard(string glyph, string title, string value, string detail, CheckState state)
    {
        var p = new StackPanel { Spacing = 4 };
        var head = Ui.Horizontal(6, Ui.Icon(glyph, 14, Ui.Brush(state)), Ui.Text(title, "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(head);
        var v = Ui.Text(value, "BodyStrongTextBlockStyle", wrap: false);
        v.FontSize = 18;
        p.Children.Add(v);
        p.Children.Add(Ui.Text(detail, "CaptionTextBlockStyle", secondary: true));
        var card = Ui.TintedCard(p, state);
        card.MinHeight = 96;
        return card;
    }

    static UIElement Glance(PcFacts f, IReadOnlyList<Check> checks)
    {
        var card = f.Card;
        string cardDetail = card?.Id is null ? L.Get("Glance_Card_None")
            : card.DriverVersion != "" ? L.Format("Glance_Card_Detail", card.Id.VendorDevice, card.DriverVersion) : card.Id.VendorDevice;
        var cpuFamily = CpuInfo.Family(f.CpuName);
        string cpuDetail = L.Get(f.HybridCpu ? "Glance_HybridPM_Needed" : "Glance_HybridPM_NotNeeded");
        if (cpuFamily.Vendor == CpuVendor.Intel && !cpuFamily.Describe().IsEmpty) cpuDetail = L.Resolve(cpuFamily.Describe()) + " · " + cpuDetail;
        var firmware = new[] { StateOf(checks, "firmware"), StateOf(checks, "secureboot"), StateOf(checks, "rebar") };
        var firmwareState = firmware.Contains(CheckState.Bad) ? CheckState.Bad : firmware.Contains(CheckState.Warning) ? CheckState.Warning
            : f.Firmware == FirmwareMode.Uefi ? CheckState.Good : CheckState.Neutral;
        string esps = f.Esps.Count == 0 ? "" : string.Join(" · ", f.Esps.Select(e => e.OpenCore switch
        {
            OpenCoreState.Found => L.Format("Glance_Esp_OpenCore", e.Disk),
            OpenCoreState.NotFound => L.Format("Glance_Esp_NoOpenCore", e.Disk),
            _ => L.Format("Glance_Esp_NotRead", e.Disk),
        }));

        var cards = new UIElement[]
        {
            GlanceCard(Glyphs.Chip, L.Get("Check_Card"), card is null ? L.Get("Glance_None") : SupportedCards.Find(card.Id)!.Name, cardDetail, StateOf(checks, "card")),
            GlanceCard(Glyphs.Settings, L.Get("Check_Cpu"), CpuInfo.CleanName(f.CpuName) is { Length: > 0 } n ? n : "–", cpuDetail, StateOf(checks, "cpu")),
            GlanceCard(Glyphs.Firmware, L.Get("Glance_Firmware"),
                L.Format("Glance_Firmware_Value", ValueOf(checks, "firmware"), ValueOf(checks, "secureboot")),
                L.Format("Glance_Firmware_Detail", ValueOf(checks, "rebar")), firmwareState),
            GlanceCard(Glyphs.Drive, L.Get("Check_Esp"), f.Esps.Count == 0 ? L.Get("Esp_None") : L.Format("Esp_Count", f.Esps.Count), esps, StateOf(checks, "esp")),
        };
        var grid = new Grid { ColumnSpacing = 12, RowSpacing = 12 };
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.ColumnDefinitions.Add(new ColumnDefinition());
        grid.RowDefinitions.Add(new RowDefinition());
        grid.RowDefinitions.Add(new RowDefinition());
        for (int i = 0; i < cards.Length; i++)
        {
            Grid.SetColumn((FrameworkElement)cards[i], i % 2);
            Grid.SetRow((FrameworkElement)cards[i], i / 2);
            grid.Children.Add(cards[i]);
        }
        return grid;
    }

    /// "This PC": the requirements, under a line that says whether they are met (the macOS app's "This Mac").
    static UIElement ThisPc(IReadOnlyList<Check> checks)
    {
        bool met = checks.All(c => c.State != CheckState.Bad);
        var list = new StackPanel { Spacing = 12 };
        foreach (var c in checks) list.Children.Add(Ui.CheckRow(c));
        var state = met ? CheckState.Good : CheckState.Bad;
        var header = Ui.Horizontal(8, Ui.Icon(met ? Glyphs.Ok : Glyphs.Bad, 16, Ui.Brush(state)),
            new TextBlock { Text = L.Get(met ? "Summary_Met" : "Summary_NotMet"), Foreground = Ui.Brush(state), FontWeight = FontWeights.SemiBold });
        var expander = new Expander
        {
            Header = header, Content = list, IsExpanded = true,
            HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
        };
        return Ui.Section(Glyphs.Pc, L.Get("Section_ThisPc"), expander);
    }

    /// The EFI system partitions: disk, bus, size, where Windows has them, and whether they hold OpenCore.
    static UIElement Partitions(PcFacts f)
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Esp_Caption"), L.Get("Esp_Detail")) };
        if (f.EspError is not null) items.Add(Ui.Text(f.EspError, secondary: true));
        else if (f.Esps.Count == 0) items.Add(Ui.Text(L.Get("Esp_None"), secondary: true));
        foreach (var e in f.Esps)
        {
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.Children.Add(Ui.Icon(Glyphs.Drive, 18));
            var texts = new StackPanel { Spacing = 1 };
            texts.Children.Add(Ui.Text(e.Bus == "" ? $"{e.Disk} · {e.SizeText}" : $"{e.Disk} ({e.Bus}) · {e.SizeText}"));
            var where = new List<string> { L.Format("Esp_Where", e.DiskNumber, e.PartitionNumber) };
            if (e.IsSystem) where.Add(L.Get("Esp_System"));
            where.Add(e.Letter != "" ? L.Format("Esp_Mounted", e.Letter) : L.Get("Esp_NotMounted"));
            texts.Children.Add(Ui.Text(string.Join(" · ", where), "CaptionTextBlockStyle", secondary: true));
            Grid.SetColumn(texts, 1); g.Children.Add(texts);
            var (text, state) = e.OpenCore switch
            {
                OpenCoreState.Found => (L.Get("Esp_OpenCore_Found"), CheckState.Good),
                OpenCoreState.NotFound => (L.Get("Esp_OpenCore_NotFound"), CheckState.Neutral),
                _ => (L.Get("Esp_OpenCore_NeedsMount"), CheckState.Neutral),
            };
            var badge = Ui.Badge(text, state);
            Grid.SetColumn(badge, 2); g.Children.Add(badge);
            items.Add(g);
        }
        return Ui.Section(Glyphs.Drive, L.Get("Check_Esp"), items.ToArray());
    }
}
