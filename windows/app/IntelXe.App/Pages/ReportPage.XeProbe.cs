// SPDX-License-Identifier: MIT
// The Report page's XeProbe section: a macOS report bundle collected with --probe (xe-report-XR-XXXXXXXX.tar.gz, or the
// folder it unpacks to), read in memory without unpacking it (Core/Report/XeProbeReport.cs): the device, its family and
// GuC, each stage's verdict, the previous boot's NVRAM breadcrumb, the watchdog's panics, the runs IntelXe Manager made
// on macOS and the probe's log. Nothing is uploaded.
using IntelXe.Core;
using IntelXe.Core.Report;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IntelXe.App.Pages;

public sealed partial class ReportPage
{
    sealed class XeProbeState
    {
        public bool Reading;
        public XeProbeReport? Report;
        public string? Error;
    }

    static readonly XeProbeState X = new();

    UIElement XeProbeSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("XeProbe_Caption"), L.Get("XeProbe_Detail")) };
        var open = Ui.Button(L.Get("XeProbe_Open"), async (_, _) => await OpenXeProbe(folder: false), glyph: Glyphs.Open);
        open.IsEnabled = !X.Reading;
        var openFolder = Ui.Button(L.Get("XeProbe_OpenFolder"), async (_, _) => await OpenXeProbe(folder: true), glyph: Glyphs.Folder);
        openFolder.IsEnabled = !X.Reading;
        items.Add(Ui.Horizontal(10, open, openFolder));
        if (X.Reading)
        {
            items.Add(new ProgressRing { IsActive = true, Width = 20, Height = 20, HorizontalAlignment = HorizontalAlignment.Left });
            items.Add(Ui.Text(L.Get("XeProbe_Reading"), secondary: true));
        }
        if (X.Error is not null)
            items.Add(new InfoBar { Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false, Message = X.Error });
        if (X.Report is { } r && !X.Reading) items.AddRange(XeProbeBody(r));
        return Ui.Section(Glyphs.Chip, L.Get("XeProbe_Section"), items.ToArray());
    }

    async Task OpenXeProbe(bool folder)
    {
        string? path;
        try
        {
            var hwnd = WinRT.Interop.WindowNative.GetWindowHandle(App.Window);
            if (folder)
            {
                var picker = new Windows.Storage.Pickers.FolderPicker();
                picker.FileTypeFilter.Add("*");
                WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
                path = (await picker.PickSingleFolderAsync())?.Path;
            }
            else
            {
                var picker = new Windows.Storage.Pickers.FileOpenPicker();
                picker.FileTypeFilter.Add(".gz");   // .tar.gz: the picker filters by the last extension
                picker.FileTypeFilter.Add(".tgz");
                WinRT.Interop.InitializeWithWindow.Initialize(picker, hwnd);
                path = (await picker.PickSingleFileAsync())?.Path;
            }
        }
        catch (Exception e) { X.Error = e.Message; Build(); return; }
        if (path is null) return;
        X.Reading = true;
        X.Error = null;
        X.Report = null;
        Build();
        try { X.Report = await Task.Run(() => XeProbeReports.Read(path)); }
        catch (XeProbeReportException e) { X.Error = L.Resolve(e.Reason); }
        catch (Exception e) { X.Error = e.Message; }
        finally { X.Reading = false; Build(); }
    }

    static IEnumerable<UIElement> XeProbeBody(XeProbeReport r)
    {
        var items = new List<UIElement>();
        var summary = string.Join("; ", XeProbeReports.Summary(r).Select(L.Resolve));
        var overall = !r.HasProbe || r.Results is null || !r.Attached ? CheckState.Neutral
            : r.Results.Stages.Any(s => s.Verdict is XeProbeVerdict.Failed or XeProbeVerdict.Stalled) || r.Panics.Any(p => p.Line is not null) ? CheckState.Bad
            : r.Results.Stages.Any(s => s.Verdict is XeProbeVerdict.Refused or XeProbeVerdict.Running) ? CheckState.Warning
            : r.Results.Stages.Any(s => s.Verdict == XeProbeVerdict.Passed) ? CheckState.Good : CheckState.Neutral;
        items.Add(Ui.Text(L.Format("XeProbe_Report", r.Id, Path.GetFileName(r.Source)), "BodyStrongTextBlockStyle"));
        items.Add(ProbeNote(summary, overall));
        if (!r.HasProbe) return items;

        // the device, its family and GuC
        if (r.Device is { } device)
            items.Add(Ui.Row(L.Get("XeProbe_Device"), r.Family is { } family ? L.Format("XeProbe_Device_Value", device, family) : L.Format("XeProbe_Device_Unknown", device)));
        if (r.Info is { } info)
        {
            var guc = info.GucImage is null ? L.Get("XeProbe_Guc_None")
                : info.GucBuiltIn is { } v ? L.Format("XeProbe_Guc_Value", info.GucImage, v)
                : L.Format("XeProbe_Guc_NotBuiltIn", info.GucImage);
            if (info.Images.Count > 0) guc += " · " + L.Format("XeProbe_Guc_Images", string.Join(", ", info.Images));
            items.Add(Ui.Row(L.Get("XeProbe_Guc"), guc));
        }

        // the stages
        if (r.Results is { Stages.Count: > 0 } results)
        {
            var list = new StackPanel { Spacing = 6 };
            foreach (var s in results.Stages) list.Children.Add(XeProbeStageRow(s));
            items.Add(Ui.Text(L.Get("XeProbe_Stages"), "BodyStrongTextBlockStyle"));
            items.Add(list);
        }

        // the previous boot's breadcrumb, the watchdog's panics
        if (r.Breadcrumb is { } crumb)
            items.Add(Ui.Row(L.Get("XeProbe_Breadcrumb"), crumb.Stage is { } n && crumb.Step is { } step
                ? L.Format("XeProbe_Breadcrumb_Value", n, crumb.StageName ?? "", step)
                : crumb.Text));
        if (r.Panics.Count > 0)
        {
            items.Add(Ui.Text(L.Get("XeProbe_Panics"), "BodyStrongTextBlockStyle"));
            foreach (var p in r.Panics)
                items.Add(p.Line is { } line ? ProbeNote(Path.GetFileName(p.File) + "\n" + line, CheckState.Bad)
                    : ProbeNote(L.Format(p.Found ? "XeProbe_Panic_NoLine" : "XeProbe_Panic_Missing", p.File), CheckState.Neutral));
        }

        // IntelXe Manager's runs on macOS
        if (r.History.Count > 0)
        {
            var runs = new StackPanel { Spacing = 4 };
            foreach (var run in r.History)
            {
                var what = run.RunTo is { } to ? L.Format("XeProbe_Run_All", to) : L.Format("XeProbe_Run_Stage", run.What);
                var when = run.When is { } t ? t.ToString("yyyy-MM-dd HH:mm 'UTC'", System.Globalization.CultureInfo.InvariantCulture) : run.WhenText;
                runs.Children.Add(Ui.Text(L.Format("XeProbe_Run_Line", when, what, run.Exit, string.Join("; ", XeProbeReports.Summary(run.Results).Select(L.Resolve))),
                    "CaptionTextBlockStyle"));
            }
            items.Add(new Expander
            {
                Header = L.Format("XeProbe_History", r.History.Count, r.HistoryBoots), Content = runs,
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            });
        }

        // the log
        if (r.Log.Length > 0)
        {
            var text = new ScrollViewer { Content = ProbeMono(r.Log), MaxHeight = 320, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
            items.Add(new Expander
            {
                Header = L.Get("XeProbe_Log"), Content = text,
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            });
        }

        if (r.Notes.Count > 0)
        {
            items.Add(Ui.Text(L.Get("XeProbe_Notes"), "CaptionTextBlockStyle", secondary: true));
            items.Add(ProbeMono(string.Join("\n", r.Notes.Select(L.Resolve))));
        }
        return items;
    }

    /// "3  guc  [failed]  15002 ms" and the stage's detail below.
    static UIElement XeProbeStageRow(XeProbeStage s)
    {
        var g = new Grid { ColumnSpacing = 12 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(120) });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        var name = Ui.Text($"{s.Number}  {s.Name}", wrap: false);
        name.VerticalAlignment = VerticalAlignment.Center;
        g.Children.Add(name);
        var badge = Ui.Badge(L.Resolve(XeProbeReports.VerdictText(s.Verdict)), XeProbeReports.VerdictState(s.Verdict));
        Grid.SetColumn(badge, 1);
        g.Children.Add(badge);
        var ms = Ui.Text(L.Format("XeProbe_Ms", s.Ms), secondary: true);
        ms.VerticalAlignment = VerticalAlignment.Center;
        Grid.SetColumn(ms, 2);
        g.Children.Add(ms);
        var p = new StackPanel { Spacing = 2 };
        p.Children.Add(g);
        if (s.Detail.Length > 0) p.Children.Add(Ui.Text(s.Detail, "CaptionTextBlockStyle", secondary: true));
        return p;
    }
}
