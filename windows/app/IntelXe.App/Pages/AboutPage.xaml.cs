// SPDX-License-Identifier: MIT
// The About page: version (the repository's VERSION), what the app does, the licences and the credits as the macOS
// About window has them, the project's links, and the app's two settings (language, appearance).
using System.Reflection;
using IntelXe.Core;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media.Imaging;

namespace IntelXe.App.Pages;

public sealed partial class AboutPage : Page
{
    public static string Version =>
        typeof(App).Assembly.GetCustomAttribute<AssemblyInformationalVersionAttribute>()?.InformationalVersion ?? "–";

    static readonly (string Name, string? Url, string Key)[] Credits =
    [
        ("nullmoth (NullMoth Systems)", "https://github.com/nullmoth/nvidia-macos-driver", "Credit_Nullmoth"),
        ("metal2vulkan, Anees Iqbal", "https://github.com/steelbrain/metal2vulkan", "Credit_Metal2vulkan"),
        ("Linux xe and i915 (Intel)", null, "Credit_Linux"),
        ("Mesa", "https://mesa3d.org", "Credit_Mesa"),
        ("Emil J. Tywoniak (widlarizer/mesa)", "https://github.com/widlarizer/mesa", "Credit_Widlarizer"),
        ("FFmpeg", null, "Credit_Ffmpeg"),
    ];

    public AboutPage()
    {
        InitializeComponent();
        Loaded += (_, _) => Build();
    }

    void Build()
    {
        var (scroll, column) = Ui.PageColumn();

        var head = new StackPanel { Spacing = 6, HorizontalAlignment = HorizontalAlignment.Center, Margin = new Thickness(0, 8, 0, 4) };
        head.Children.Add(new Image { Source = new BitmapImage(new Uri("ms-appx:///Assets/AppIcon.png")), Width = 96, Height = 96 });
        var name = Ui.Text("IntelXe Manager", "TitleTextBlockStyle");
        name.HorizontalAlignment = HorizontalAlignment.Center;
        head.Children.Add(name);
        var version = Ui.Text(L.Format("About_Version", Version), secondary: true);
        version.HorizontalAlignment = HorizontalAlignment.Center;
        head.Children.Add(version);
        var what = Ui.Text(L.Get("About_What"));
        what.TextAlignment = TextAlignment.Center;
        what.MaxWidth = 520;
        head.Children.Add(what);
        var link = Ui.Link("github.com/" + Project.Repository, new Uri(Project.RepositoryUrl));
        link.HorizontalAlignment = HorizontalAlignment.Center;
        head.Children.Add(link);
        column.Children.Add(head);

        column.Children.Add(Ui.Section(Glyphs.Link, L.Get("About_Links"),
            Ui.Row(L.Get("About_Site"), Ui.Link(Project.SiteUrl.Replace("https://", "").TrimEnd('/'), new Uri(Project.SiteUrl))),
            Ui.Row(L.Get("About_Guide"), Ui.Link(L.Get("About_Guide_Windows"), new Uri(Project.WindowsGuideUrl))),
            Ui.Row(L.Get("About_Releases"), Ui.Link(L.Get("About_Releases_Link"), new Uri(Project.ReleasesUrl)))));

        var credits = new StackPanel { Spacing = 8 };
        foreach (var c in Credits)
        {
            var item = new StackPanel { Spacing = 0 };
            if (c.Url is not null)
            {
                var l = Ui.Link(c.Name, new Uri(c.Url));
                l.Padding = new Thickness(0);
                item.Children.Add(l);
            }
            else item.Children.Add(Ui.Text(c.Name, "BodyStrongTextBlockStyle"));
            item.Children.Add(Ui.Text(L.Get(c.Key), "CaptionTextBlockStyle", secondary: true));
            credits.Children.Add(item);
        }
        column.Children.Add(Ui.Section(Glyphs.Document, L.Get("About_License"),
            Ui.Text(L.Get("About_License_Text"), secondary: true),
            Ui.Link(L.Get("About_Licensing_Link"), new Uri(Project.LicensingUrl))));
        column.Children.Add(Ui.Section(Glyphs.Heart, L.Get("About_Credits"), credits,
            Ui.Text(L.Get("About_Trademarks"), "CaptionTextBlockStyle", secondary: true)));

        column.Children.Add(Ui.Section(Glyphs.Settings, L.Get("About_App"), AppSettings()));
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    static UIElement AppSettings()
    {
        var language = new ComboBox { MinWidth = 180 };
        foreach (var (tag, key) in new[] { ("", "Setting_System"), ("en-US", "Setting_English"), ("tr-TR", "Setting_Turkish") })
            language.Items.Add(new ComboBoxItem { Content = key == "Setting_English" ? "English" : key == "Setting_Turkish" ? "Türkçe" : L.Get(key), Tag = tag });
        language.SelectedIndex = App.Settings.Language switch { "en-US" => 1, "tr-TR" => 2, _ => 0 };
        language.SelectionChanged += (_, _) =>
        {
            var tag = (language.SelectedItem as ComboBoxItem)?.Tag as string ?? "";
            if (tag == App.Settings.Language) return;
            App.Settings.Language = tag;
            App.Settings.Save();
            L.SetLanguage(tag);
            App.Window?.ApplyLanguage();
        };

        var theme = new ComboBox { MinWidth = 180 };
        foreach (var (tag, key) in new[] { ("", "Setting_System"), ("light", "Setting_Light"), ("dark", "Setting_Dark") })
            theme.Items.Add(new ComboBoxItem { Content = L.Get(key), Tag = tag });
        theme.SelectedIndex = App.Settings.Theme switch { "light" => 1, "dark" => 2, _ => 0 };
        theme.SelectionChanged += (_, _) =>
        {
            var tag = (theme.SelectedItem as ComboBoxItem)?.Tag as string ?? "";
            App.Settings.Theme = tag;
            App.Settings.Save();
            App.Window?.ApplyTheme(tag);
        };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(language, L.Get("Setting_Language"));
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(theme, L.Get("Setting_Appearance"));
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Ui.Row(L.Get("Setting_Language"), language));
        p.Children.Add(Ui.Row(L.Get("Setting_Appearance"), theme));
        return p;
    }
}
