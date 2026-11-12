// SPDX-License-Identifier: MIT
// The Report page: tools/report/collect-windows.ps1, shipped beside the app, writes the diagnostics zip on the Desktop;
// the page shows its progress and output, then the zip in Explorer, and opens the issue forms with the report ID filled
// in (as app/Sources/Report.swift on macOS). The script changes nothing; nothing is uploaded, by it or by the app.
using IntelXe.Core;
using IntelXe.Core.Report;
using Microsoft.UI.Dispatching;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace IntelXe.App.Pages;

public sealed partial class ReportPage : Page
{
    static bool acpi, bootLogs;
    static string? lastError;
    TextBox? output;
    TextBlock? elapsed;
    DispatcherQueueTimer? timer;
    ReportRunner Runner => AppState.Current.Report;

    public ReportPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            Runner.OutputChanged += OnOutput;
            Runner.Finished += OnFinished;
            timer = DispatcherQueue.CreateTimer();
            timer.Interval = TimeSpan.FromSeconds(1);
            timer.Tick += (_, _) => UpdateElapsed();
            timer.Start();
            Build();
        };
        Unloaded += (_, _) => { Runner.OutputChanged -= OnOutput; Runner.Finished -= OnFinished; timer?.Stop(); };
    }

    void OnOutput() => DispatcherQueue.TryEnqueue(() =>
    {
        if (output is null) return;
        lock (Runner.Output) output.Text = Runner.Output.ToString();
        output.SelectionStart = output.Text.Length;
    });

    void OnFinished(ReportResult? result, string? error) => DispatcherQueue.TryEnqueue(() =>
    {
        lastError = error;
        if (result is not null)
        {
            AppState.Current.LastReport = result;
            Reveal(result.File);
        }
        Build();
    });

    void UpdateElapsed()
    {
        if (elapsed is not null && Runner.Running)
            elapsed.Text = L.Format("Report_Seconds", (int)(DateTime.Now - Runner.StartedAt).TotalSeconds);
    }

    static void Reveal(string file)
    {
        try { System.Diagnostics.Process.Start("explorer.exe", $"/select,\"{file}\""); } catch (Exception) { }
    }

    void Build()
    {
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Report")));
        column.Children.Add(Ui.Section(Glyphs.Report, L.Get("Report_Section"), ReportBody()));
        if (AppState.Current.LastReport is { } r) column.Children.Add(Ui.Section(Glyphs.Document, L.Get("Report_Last"), Last(r)));
        else column.Children.Add(Ui.Section(Glyphs.Feedback, L.Get("Report_Forms"), Forms()));
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    UIElement ReportBody()
    {
        var p = new StackPanel { Spacing = 12 };
        p.Children.Add(Ui.Caption(L.Get("Report_Caption"), L.Get("Report_Detail")));
        var toggle = new ToggleSwitch { IsOn = acpi, OnContent = "", OffContent = "", MinWidth = 0, Width = 40, HorizontalAlignment = HorizontalAlignment.Right, IsEnabled = !Runner.Running };
        toggle.Toggled += (_, _) => acpi = toggle.IsOn;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(toggle, L.Get("Report_Acpi"));
        var acpiRow = new Grid { ColumnSpacing = 12 };
        acpiRow.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        acpiRow.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var acpiText = new StackPanel { Spacing = 2 };
        acpiText.Children.Add(Ui.Text(L.Get("Report_Acpi")));
        acpiText.Children.Add(Ui.Text(L.Get("Report_Acpi_Caption"), "CaptionTextBlockStyle", secondary: true));
        acpiRow.Children.Add(acpiText);
        Grid.SetColumn(toggle, 1); acpiRow.Children.Add(toggle);
        p.Children.Add(acpiRow);
        p.Children.Add(BootLogsRow());

        if (Runner.Running)
        {
            p.Children.Add(new ProgressBar { IsIndeterminate = true });
            elapsed = Ui.Text("", "CaptionTextBlockStyle", secondary: true);
            p.Children.Add(Ui.Horizontal(12, Ui.Text(L.Get("Report_Collecting")), elapsed));
            UpdateElapsed();
        }
        else
        {
            var create = Ui.Button(L.Get("Report_Create"), (_, _) => { lastError = null; Runner.Start(acpi, bootLogs); Build(); }, accent: true);
            var row = Ui.Horizontal(12, create);
            row.HorizontalAlignment = HorizontalAlignment.Right;
            p.Children.Add(row);
            if (ReportRunner.FindScript() is null) { create.IsEnabled = false; p.Children.Add(Ui.Text(L.Get("Report_NoScript"), secondary: true)); }
        }
        if (lastError is not null)
            p.Children.Add(new InfoBar { Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false, Message = lastError });

        string text;
        lock (Runner.Output) text = Runner.Output.ToString();
        if (Runner.Running || text.Length > 0)
        {
            output = new TextBox
            {
                Text = text, IsReadOnly = true, AcceptsReturn = true, TextWrapping = TextWrapping.Wrap,
                FontFamily = new FontFamily("Cascadia Mono, Consolas"), FontSize = 12, MinHeight = 120, MaxHeight = 260,
            };
            ScrollViewer.SetVerticalScrollBarVisibility(output, ScrollBarVisibility.Auto);
            var expander = new Expander
            {
                Header = L.Get("Report_Output"), Content = output, IsExpanded = Runner.Running || lastError is not null,
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            };
            p.Children.Add(expander);
        }
        else output = null;
        return p;
    }

    /// The boot logs option (-BootLogs): OpenCore's logs and the panics it saved, from USB sticks and the EFI partitions,
    /// which the script reads as administrator (Windows asks for permission when the report starts).
    UIElement BootLogsRow()
    {
        var toggle = new ToggleSwitch { IsOn = bootLogs, OnContent = "", OffContent = "", MinWidth = 0, Width = 40, HorizontalAlignment = HorizontalAlignment.Right, IsEnabled = !Runner.Running };
        toggle.Toggled += (_, _) => bootLogs = toggle.IsOn;
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(toggle, L.Get("Report_BootLogs"));
        var row = new Grid { ColumnSpacing = 12 };
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        row.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        var text = new StackPanel { Spacing = 2 };
        text.Children.Add(Ui.Text(L.Get("Report_BootLogs")));
        text.Children.Add(Ui.Text(L.Get("Report_BootLogs_Caption"), "CaptionTextBlockStyle", secondary: true));
        row.Children.Add(text);
        Grid.SetColumn(toggle, 1); row.Children.Add(toggle);
        return row;
    }

    static UIElement Last(ReportResult r)
    {
        var p = new StackPanel { Spacing = 12 };
        bool desktop = string.Equals(Path.GetDirectoryName(r.File), ReportRunner.Desktop, StringComparison.OrdinalIgnoreCase);
        p.Children.Add(Ui.Caption(desktop ? L.Format("Report_Done", r.Id) : L.Format("Report_Done_Folder", r.Id, Path.GetFileName(Path.GetDirectoryName(r.File)) ?? "")));
        var id = Ui.Text(r.Id);
        id.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        p.Children.Add(Ui.Row(L.Get("Report_Id"), id));
        var show = Ui.Button(L.Get("Report_Show"), (_, _) => Reveal(r.File), glyph: Glyphs.Folder);
        var copy = Ui.Button(L.Get("Report_Copy"), (_, _) =>
        {
            var data = new DataPackage();
            data.SetText(r.Id);
            Clipboard.SetContent(data);
        }, glyph: Glyphs.Copy);
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.Children.Add(Ui.Horizontal(8, show, copy));
        var links = Ui.Horizontal(4, Ui.Link(L.Get("Report_Bug"), Project.BugFormUrl(r.Id)), Ui.Link(L.Get("Report_NewGpu"), Project.NewGpuFormUrl(r.Id)));
        Grid.SetColumn(links, 2); g.Children.Add(links);
        p.Children.Add(g);
        p.Children.Add(Ui.Text(L.Get("Report_Attach"), "CaptionTextBlockStyle", secondary: true));
        return p;
    }

    static UIElement Forms()
    {
        var p = new StackPanel { Spacing = 8 };
        p.Children.Add(Ui.Row(L.Get("Report_Form_Bug"), Ui.Link(L.Get("Report_Bug"), Project.BugFormUrl(null))));
        p.Children.Add(Ui.Row(L.Get("Report_Form_NewGpu"), Ui.Link(L.Get("Report_NewGpu"), Project.NewGpuFormUrl(null))));
        p.Children.Add(Ui.Text(L.Get("Report_First"), "CaptionTextBlockStyle", secondary: true));
        return p;
    }
}
