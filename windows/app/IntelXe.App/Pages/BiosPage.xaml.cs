// SPDX-License-Identifier: MIT
// The BIOS page: the firmware settings this PC needs for macOS with the driver, and the Windows traps to clear first,
// worded with this board maker's menu names (Core/Bios/bios-steps.json). Each item is marked done, to do, or cannot
// tell, from what the Overview read from Windows; each names its sources. The page reads only: it changes no firmware
// setting and not BitLocker. "Save as a page..." writes the steps as one HTML file to open on a phone or print.
using IntelXe.Core;
using IntelXe.Core.Bios;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IntelXe.App.Pages;

public sealed partial class BiosPage : Page
{
    /// the vendor chosen in the box, else the board's; kept while the app runs
    static string? chosenVendor = App.Options.BiosVendor;
    static string? savedTo;
    static string? saveError;
    static bool exported;

    public BiosPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { AppState.Current.Changed += OnChanged; ActualThemeChanged += OnTheme; Build(); };
        Unloaded += (_, _) => { AppState.Current.Changed -= OnChanged; ActualThemeChanged -= OnTheme; };
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);
    void OnTheme(FrameworkElement sender, object args) => Build();

    static string Lang => BiosSteps.Lang(L.Language);
    static BiosData? DataOrNull { get { try { return BiosSteps.Embedded; } catch (Exception) { return null; } } }

    void Build()
    {
        var state = AppState.Current;
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Bios"), L.Get("Bios_Caption")));
        if (DataOrNull is not { } data)
        {
            column.Children.Add(new InfoBar { Severity = InfoBarSeverity.Error, IsOpen = true, IsClosable = false, Title = L.Get("Bios_NoData") });
        }
        else if (state.Facts is not { } f)
        {
            column.Children.Add(Ui.Card(Ui.Horizontal(10, new ProgressRing { IsActive = true, Width = 18, Height = 18 }, Ui.Text(L.Get("Overview_Reading"), secondary: true))));
        }
        else
        {
            var vendor = chosenVendor ?? BiosSteps.VendorOf(data, f.BoardMaker);
            var steps = BiosSteps.Evaluate(data, f);
            if (App.Options.BiosHtml is { Length: > 0 } path && !exported)
            {
                exported = true;
                Save(path, data, steps, f, vendor);
            }
            column.Children.Add(Head(data, f, steps, vendor));
            foreach (var group in data.Groups)
            {
                var mine = steps.Where(s => s.Item.Group == group.Id).ToList();
                if (mine.Count == 0) continue;
                var items = new List<UIElement> { Ui.Text(group.Caption.In(Lang), "BodyTextBlockStyle", secondary: true) };
                foreach (var s in mine) items.Add(Row(data, f, s, vendor));
                column.Children.Add(Ui.Section(group.Id == "windows" ? Glyphs.Shield : Glyphs.Firmware, group.Title.In(Lang), items.ToArray()));
            }
            var note = Ui.Text(L.Get("Bios_ReadOnly"), "CaptionTextBlockStyle", secondary: true);
            note.Margin = new Thickness(4, 14, 0, 0);
            column.Children.Add(note);
        }
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    UIElement Head(BiosData data, PcFacts f, IReadOnlyList<BiosStep> steps, string vendorId)
    {
        var p = new StackPanel { Spacing = 10 };
        var board = $"{f.BoardMaker} {f.BoardModel}".Trim();
        p.Children.Add(Ui.Row(L.Get("Check_Board"), board.Length > 0 ? board : L.Get("Value_NotReported")));

        var detected = BiosSteps.VendorOf(data, f.BoardMaker);
        var box = new ComboBox { MinWidth = 220 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, L.Get("Bios_Menus"));
        foreach (var v in data.Vendors)
        {
            var name = v.Id == "generic" ? L.Get("Bios_Vendor_Other") : v.Name;
            if (v.Id == detected) name = L.Format("Bios_Vendor_Detected", name);
            box.Items.Add(new ComboBoxItem { Content = name, Tag = v.Id });
        }
        box.SelectedIndex = Math.Max(0, data.Vendors.FindIndex(v => v.Id == vendorId));
        box.SelectionChanged += (_, _) =>
        {
            if (box.SelectedItem is ComboBoxItem { Tag: string id } && id != vendorId) { chosenVendor = id; Build(); }
        };
        p.Children.Add(Ui.Row(L.Get("Bios_Menus"), box));

        var vendor = BiosSteps.Vendor(data, vendorId);
        var keys = new List<string> { L.Format("Bios_Key_Setup", vendor.SetupKey) };
        if (vendor.AdvancedKey.Length > 0) keys.Add(L.Format("Bios_Key_Advanced", vendor.AdvancedKey));
        keys.Add(L.Format("Bios_Key_Save", vendor.SaveKey));
        keys.Add(L.Format("Bios_Key_BootMenu", vendor.BootMenuKey));
        p.Children.Add(Ui.Text(string.Join(" · ", keys), "BodyStrongTextBlockStyle"));
        p.Children.Add(Ui.Text(vendor.Note.In(Lang), "CaptionTextBlockStyle", secondary: true));

        var (todo, done, unknown) = BiosSteps.Count(steps);
        var badges = Ui.Horizontal(8,
            Ui.Badge(L.Format("Bios_Count_ToDo", todo), todo > 0 ? CheckState.Warning : CheckState.Good),
            Ui.Badge(L.Format("Bios_Count_Done", done), CheckState.Good),
            Ui.Badge(L.Format("Bios_Count_Unknown", unknown), CheckState.Neutral));
        p.Children.Add(badges);

        var save = Ui.Button(L.Get("Bios_Save"), async (_, _) => await SaveWithPicker(data, steps, f, vendorId), accent: true, glyph: Glyphs.Document);
        var buttons = Ui.Horizontal(10, save);
        if (savedTo is not null)
        {
            var open = Ui.Button(L.Get("Bios_Open"), (_, _) => Ui.OpenUri(new Uri(savedTo)), glyph: Glyphs.Open);
            buttons.Children.Add(open);
        }
        p.Children.Add(buttons);
        p.Children.Add(Ui.Text(L.Get("Bios_Save_Detail"), "CaptionTextBlockStyle", secondary: true));
        if (savedTo is not null) p.Children.Add(Ui.Text(L.Format("Bios_Saved", savedTo), "CaptionTextBlockStyle", secondary: true));
        if (saveError is not null) p.Children.Add(Ui.Text(saveError, "CaptionTextBlockStyle"));
        return Ui.Section(Glyphs.Pc, L.Get("Bios_ThisBoard"), p);
    }

    static string StateText(BiosState s) => s switch
    {
        BiosState.Done => L.Get("Bios_State_Done"), BiosState.ToDo => L.Get("Bios_State_ToDo"), _ => L.Get("Bios_State_Unknown"),
    };

    static CheckState StateColour(BiosState s, BiosLevel level) => s switch
    {
        BiosState.Done => CheckState.Good,
        BiosState.ToDo => level == BiosLevel.Required ? CheckState.Bad : CheckState.Warning,
        _ => CheckState.Neutral,
    };

    static string LevelText(BiosLevel l) => l switch
    {
        BiosLevel.Required => L.Get("Bios_Level_Required"), BiosLevel.Recommended => L.Get("Bios_Level_Recommended"), _ => L.Get("Bios_Level_Optional"),
    };

    UIElement Row(BiosData data, PcFacts f, BiosStep s, string vendor)
    {
        var item = s.Item;
        var colour = StateColour(s.State, item.Level);
        var g = new Grid { ColumnSpacing = 10 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var icon = Ui.Icon(Ui.StateGlyph(colour), 16, Ui.Brush(colour));
        icon.VerticalAlignment = VerticalAlignment.Top;
        icon.Margin = new Thickness(0, 3, 0, 0);
        g.Children.Add(icon);

        var body = new StackPanel { Spacing = 3 };
        var title = new TextBlock { TextWrapping = TextWrapping.Wrap, IsTextSelectionEnabled = true };
        title.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = item.Title.In(Lang), FontWeight = FontWeights.SemiBold });
        title.Inlines.Add(new Microsoft.UI.Xaml.Documents.Run { Text = "  " + LevelText(item.Level), FontSize = 12, Foreground = (Brush)Application.Current.Resources["TextFillColorSecondaryBrush"] });
        body.Children.Add(title);
        body.Children.Add(Ui.Text("→ " + item.Want.In(Lang), "BodyStrongTextBlockStyle"));
        var path = item.PathFor(vendor, Lang);
        if (path.Length > 0)
        {
            var t = Ui.Text(L.Format("Bios_Where", path), "CaptionTextBlockStyle");
            t.FontFamily = new FontFamily("Cascadia Mono, Consolas, Segoe UI");
            body.Children.Add(t);
        }
        body.Children.Add(Ui.Text(L.Format("Bios_Seen", s.Seen.In(data, Lang)), "CaptionTextBlockStyle", secondary: true));
        body.Children.Add(Ui.Text(item.Why.In(Lang), "BodyTextBlockStyle", secondary: true));
        if (item.Missing is { } m) body.Children.Add(Ui.Text(L.Format("Bios_Missing", m.In(Lang)), "CaptionTextBlockStyle", secondary: true));
        var links = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 2 };
        links.Children.Add(Ui.Text(L.Get("Bios_Sources"), "CaptionTextBlockStyle", secondary: true, wrap: false));
        foreach (var id in item.Sources)
            if (data.Sources.TryGetValue(BiosSteps.SourceFor(id, f), out var src))
            {
                var link = Ui.Link(src.Short.In(Lang), new Uri(src.Url));
                link.FontSize = 12;
                ToolTipService.SetToolTip(link, src.Title);
                links.Children.Add(link);
            }
        body.Children.Add(new ScrollViewer { Content = links, HorizontalScrollMode = ScrollMode.Auto, HorizontalScrollBarVisibility = ScrollBarVisibility.Hidden, VerticalScrollMode = ScrollMode.Disabled });
        Grid.SetColumn(body, 1);
        g.Children.Add(body);

        var badge = Ui.Badge(StateText(s.State), colour);
        badge.VerticalAlignment = VerticalAlignment.Top;
        Grid.SetColumn(badge, 2);
        g.Children.Add(badge);
        var b = new Border { Child = g, Padding = new Thickness(0, 8, 0, 8), BorderThickness = new Thickness(0, 1, 0, 0),
            BorderBrush = (Brush)Application.Current.Resources["CardStrokeColorDefaultBrush"] };
        return b;
    }

    async Task SaveWithPicker(BiosData data, IReadOnlyList<BiosStep> steps, PcFacts f, string vendor)
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FileSavePicker { SuggestedFileName = L.Get("Bios_FileName"), DefaultFileExtension = ".html" };
            picker.FileTypeChoices.Add(L.Get("Bios_FileType"), [".html"]);
            picker.SuggestedStartLocation = Windows.Storage.Pickers.PickerLocationId.Desktop;
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
            var file = await picker.PickSaveFileAsync();
            if (file is null) return;
            Save(file.Path, data, steps, f, vendor);
        }
        catch (Exception e) { saveError = e.Message; }
        Build();
    }

    static void Save(string path, BiosData data, IReadOnlyList<BiosStep> steps, PcFacts f, string vendor)
    {
        try
        {
            File.WriteAllText(path, BiosHtml.Render(data, steps, f, vendor, Lang, DateTime.Now), new System.Text.UTF8Encoding(false));
            savedTo = Path.GetFullPath(path);
            saveError = null;
        }
        catch (Exception e) { saveError = e.Message; }
    }
}
