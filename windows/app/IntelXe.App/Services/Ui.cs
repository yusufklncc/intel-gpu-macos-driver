// SPDX-License-Identifier: MIT
// The shared pieces of the pages' look, as app/Sources/Style.swift has them on macOS: a section header with an accent
// icon, a caption with its detail behind an (i) button, a coloured status badge, a card tinted by its state, a check row.
using System.Diagnostics;
using IntelXe.Core;
using Microsoft.UI;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Controls.Primitives;
using Microsoft.UI.Xaml.Media;
using Windows.UI;

namespace IntelXe.App;

public static class Glyphs
{
    public const string Grid = "", Drive = "", Report = "", Info = "", Refresh = "",
        Chip = "", Pc = "", Firmware = "", Settings = "", Puzzle = "", Undo = "",
        Document = "", Feedback = "", Link = "", Folder = "", Copy = "", Heart = "",
        Ok = "", Warning = "", Bad = "", Neutral = "", Shield = "", List = "", Open = "";
}

public static class Ui
{
    public static FrameworkElement Root => (FrameworkElement)App.Window!.Content;

    public static Color StateColor(CheckState s)
    {
        var key = s switch
        {
            CheckState.Good => "StatusGoodColor",
            CheckState.Warning => "StatusWarningColor",
            CheckState.Bad => "StatusBadColor",
            _ => "StatusNeutralColor",
        };
        var theme = Root.ActualTheme == ElementTheme.Dark ? "Dark" : "Light";
        var dicts = Application.Current.Resources.ThemeDictionaries;
        if (dicts.TryGetValue(theme, out var d) && d is ResourceDictionary rd && rd.TryGetValue(key, out var c) && c is Color color) return color;
        return Colors.Gray;
    }

    public static SolidColorBrush Brush(CheckState s, double opacity = 1) => new(StateColor(s)) { Opacity = opacity };

    static Style Style(string key) => (Style)Application.Current.Resources[key];

    public static FontIcon Icon(string glyph, double size = 16, Brush? brush = null, string? style = null)
    {
        var i = new FontIcon { Glyph = glyph, FontSize = size };
        if (style is not null) i.Style = Style(style);
        if (brush is not null) i.Foreground = brush;
        return i;
    }

    public static string StateGlyph(CheckState s) => s switch
    {
        CheckState.Good => Glyphs.Ok, CheckState.Warning => Glyphs.Warning, CheckState.Bad => Glyphs.Bad, _ => Glyphs.Neutral,
    };

    public static TextBlock Text(string text, string? style = null, bool secondary = false, bool wrap = true)
    {
        var t = new TextBlock { Text = text, TextWrapping = wrap ? TextWrapping.Wrap : TextWrapping.NoWrap, IsTextSelectionEnabled = true };
        if (!wrap) t.TextTrimming = TextTrimming.CharacterEllipsis;
        // the secondary colour comes with a style (SecondaryBodyTextStyle for BodyTextBlockStyle, and so on: App.xaml)
        if (secondary) style = style switch { null => "SecondaryTextStyle", "BodyTextBlockStyle" => "SecondaryBodyTextStyle", "CaptionTextBlockStyle" => "SecondaryCaptionTextStyle", _ => style };
        if (style is not null) t.Style = Style(style);
        return t;
    }

    /// The page's title and its one-line caption.
    public static StackPanel PageHeader(string title, string? caption = null)
    {
        var p = new StackPanel { Spacing = 2, Margin = new Thickness(0, 0, 0, 8) };
        p.Children.Add(Text(title, "PageTitleStyle"));
        if (caption is not null) p.Children.Add(Text(caption, "BodyTextBlockStyle", secondary: true));
        return p;
    }

    /// A section: the accent icon and title above, the content in a card below (the macOS app's Section2 in a grouped form).
    public static StackPanel Section(string glyph, string title, params UIElement[] content)
    {
        var header = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8, Margin = new Thickness(4, 16, 0, 6) };
        header.Children.Add(Icon(glyph, 16, style: "AccentIconStyle"));
        header.Children.Add(Text(title, "SectionTitleStyle"));
        var body = new StackPanel { Spacing = 10 };
        foreach (var c in content) body.Children.Add(c);
        var p = new StackPanel();
        p.Children.Add(header);
        p.Children.Add(Card(body));
        return p;
    }

    /// The grouped card of a section: Windows' card fill and stroke.
    public static Border Card(UIElement child) => new() { Child = child, Style = Style("CardStyle") };

    /// A card filled with a light wash of its state's colour, with an edge that stays visible on the light theme
    /// (Style.swift TintedCard).
    public static Border TintedCard(UIElement child, CheckState state) => new()
    {
        Child = child,
        Padding = new Thickness(14, 12, 14, 12),
        CornerRadius = new CornerRadius(8),
        Background = Brush(state, 0.10),
        BorderBrush = Brush(state, 0.32),
        BorderThickness = new Thickness(1),
    };

    /// A capsule with a dot and a short state, in the state's colour (Style.swift StatusBadge).
    public static Border Badge(string text, CheckState state)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 6 };
        p.Children.Add(new Microsoft.UI.Xaml.Shapes.Ellipse { Width = 8, Height = 8, Fill = Brush(state), VerticalAlignment = VerticalAlignment.Center });
        p.Children.Add(new TextBlock { Text = text, FontWeight = FontWeights.SemiBold, Foreground = Brush(state), VerticalAlignment = VerticalAlignment.Center });
        return new Border { Child = p, Padding = new Thickness(10, 5, 12, 5), CornerRadius = new CornerRadius(14), Background = Brush(state, 0.16), VerticalAlignment = VerticalAlignment.Center };
    }

    /// One short secondary line, and the long explanation behind an (i) button (Style.swift Caption).
    public static UIElement Caption(string text, string? detail = null)
    {
        var g = new Grid { ColumnSpacing = 6 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Text(text, "BodyTextBlockStyle", secondary: true));
        if (detail is not null)
        {
            var b = InfoButton(detail);
            Grid.SetColumn(b, 1);
            g.Children.Add(b);
        }
        return g;
    }

    public static Button InfoButton(string detail)
    {
        var b = new Button
        {
            Content = Icon(Glyphs.Info, 14, style: "SecondaryIconStyle"),
            Padding = new Thickness(4),
            Background = new SolidColorBrush(Colors.Transparent),
            BorderThickness = new Thickness(0),
            VerticalAlignment = VerticalAlignment.Top,
            Flyout = new Flyout { Content = new TextBlock { Text = detail, TextWrapping = TextWrapping.Wrap, MaxWidth = 340, IsTextSelectionEnabled = true } },
        };
        ToolTipService.SetToolTip(b, L.Get("More_Information"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(b, L.Get("More_Information"));
        return b;
    }

    /// A row: title on the left, value on the right (LabeledContent).
    public static Grid Row(string title, UIElement value)
    {
        var g = new Grid { ColumnSpacing = 16 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var label = Text(title);
        label.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(label);
        if (value is FrameworkElement fe) { fe.HorizontalAlignment = HorizontalAlignment.Right; fe.VerticalAlignment = VerticalAlignment.Center; }
        Grid.SetColumn((FrameworkElement)value, 1);
        g.Children.Add(value);
        return g;
    }

    public static Grid Row(string title, string value)
    {
        var t = Text(value, secondary: true);
        t.TextAlignment = TextAlignment.Right;
        return Row(title, t);
    }

    /// A check: state icon, title, value on the right, and its one-line explanation under it (Views.swift CheckRow).
    public static StackPanel CheckRow(Check check)
    {
        var g = new Grid { ColumnSpacing = 10 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var icon = Icon(StateGlyph(check.State), 16, Brush(check.State));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(icon, StateName(check.State));
        g.Children.Add(icon);
        var title = Text(L.Resolve(check.Title));
        Grid.SetColumn(title, 1);
        g.Children.Add(title);
        var value = Text(L.Resolve(check.Value), secondary: true);
        value.TextAlignment = TextAlignment.Right;
        value.HorizontalAlignment = HorizontalAlignment.Right;
        Grid.SetColumn(value, 2);
        g.Children.Add(value);
        var p = new StackPanel { Spacing = 2 };
        p.Children.Add(g);
        if (!check.Help.IsEmpty)
        {
            var help = Text(L.Resolve(check.Help), "CaptionTextBlockStyle", secondary: true);
            help.Margin = new Thickness(26, 0, 0, 0);
            p.Children.Add(help);
        }
        return p;
    }

    public static string StateName(CheckState s) => s switch
    {
        CheckState.Good => L.Get("State_Ok"), CheckState.Warning => L.Get("State_Warning"), CheckState.Bad => L.Get("State_Problem"), _ => "",
    };

    public static Button Button(string text, RoutedEventHandler click, bool accent = false, string? glyph = null)
    {
        object content = text;
        if (glyph is not null)
        {
            var sp = new StackPanel { Orientation = Orientation.Horizontal, Spacing = 8 };
            sp.Children.Add(Icon(glyph, 14));
            sp.Children.Add(new TextBlock { Text = text });
            content = sp;
        }
        var b = new Button { Content = content };
        if (accent) b.Style = (Style)Application.Current.Resources["AccentButtonStyle"];
        b.Click += click;
        return b;
    }

    public static HyperlinkButton Link(string text, Uri uri)
    {
        var b = new HyperlinkButton { Content = text, Padding = new Thickness(4, 2, 4, 2) };
        b.Click += (_, _) => OpenUri(uri);
        ToolTipService.SetToolTip(b, uri.AbsoluteUri);
        return b;
    }

    public static void OpenUri(Uri uri)
    {
        try { Process.Start(new ProcessStartInfo(uri.AbsoluteUri) { UseShellExecute = true }); } catch (Exception) { }
    }

    public static StackPanel Horizontal(double spacing, params UIElement[] items)
    {
        var p = new StackPanel { Orientation = Orientation.Horizontal, Spacing = spacing };
        foreach (var i in items) p.Children.Add(i);
        return p;
    }

    /// A page's scrolling column, as wide as the window allows up to a readable width.
    public static (ScrollViewer Scroll, StackPanel Column) PageColumn()
    {
        var column = new StackPanel { MaxWidth = 960, Padding = new Thickness(28, 20, 28, 28), HorizontalAlignment = HorizontalAlignment.Stretch };
        var scroll = new ScrollViewer { Content = column, HorizontalScrollMode = ScrollMode.Disabled, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        if (App.Options.Scroll is int offset)
            scroll.SizeChanged += (_, _) => scroll.ChangeView(null, offset, null, true);
        return (scroll, column);
    }
}
