// SPDX-License-Identifier: MIT
// The Report page's Probe stick sections (docs/design/DESIGN-probe-stick-windows.md). Write: the Xe probe stick's image
// (tools/probe-stick/, found beside the app or chosen, checked against its .sha256 and the stick's layout:
// Core/ProbeStick/ProbeImage.cs), a USB disk from the same list and rules as the installer stick page (WmiStickSystem,
// StickRules), the erase dialog (the disk's name and size typed, and a box of its own for a stick that holds an EFI), and
// the raw write by the elevated helper (this exe with --probe-helper write, through "runas"), which checks everything
// again, writes, and reads the stick back. Read: the XEPROBE volumes, the reports in their REPORTS folder checked against
// their .sha256 and SEAL.sha256 (Core/ProbeStick/ProbeReports.cs), the summary, Explorer, and the issue forms with the
// report ID filled in, as for the Windows report. Nothing is uploaded.
using IntelXe.Core;
using IntelXe.Core.ProbeStick;
using IntelXe.Core.Stick;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;
using Windows.ApplicationModel.DataTransfer;

namespace IntelXe.App.Pages;

public sealed partial class ReportPage
{
    sealed class ProbeState
    {
        public bool Started;
        public string? Image;
        public ProbeImageInfo? Checked;
        public string? ImageError;
        public bool Checking;
        public List<StickDisk>? Disks;
        public string? DiskError;
        public bool Listing;
        public uint? Disk;
        public bool AllowLarge;
        public bool Writing;
        public (string Stage, double Fraction)? Progress;
        public StickHelperResult? Result;
        public readonly string Salt = StickIdentity.NewSalt();
        public bool Reading;
        public string? ReadError;
        public List<(string Root, IReadOnlyList<ProbeReport> Reports)>? Sticks;
    }

    static readonly ProbeState P = new();
    ProgressBar? probeBar;

    static string LocalAppData => Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData);

    /// The write and read sections, below the diagnostics report's. The first time, it looks for the image, the disks and
    /// the sticks.
    IEnumerable<UIElement> ProbeSections()
    {
        if (!P.Started)
        {
            P.Started = true;
            DispatcherQueue.TryEnqueue(async () => { await CheckImage(null); await ListProbeDisks(); await ReadSticks(); });
        }
        return [ProbeWriteSection(), ProbeReadSection()];
    }

    // ---- writing ----

    static StickPolicy ProbePolicy()
    {
        var letters = new List<string> { AppContext.BaseDirectory[..2] };
        if (P.Checked?.Path is { Length: >= 2 } img && img[1] == ':') letters.Add(img[..2]);
        return new StickPolicy((ulong)(P.Checked?.Size ?? 0), App.Options.TestVhdx, letters, P.AllowLarge);
    }

    static IReadOnlyList<string> ProbeRefusals(StickDisk d) => StickRules.Refusals(d, ProbePolicy());

    UIElement ProbeWriteSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Probe_Caption"), L.Get("Probe_Detail")) };
        items.Add(AppState.Current.Facts?.SecureBoot switch
        {
            SecureBootState.On => ProbeNote(L.Get("Probe_SecureBoot_On"), CheckState.Warning),
            SecureBootState.Off => ProbeNote(L.Get("Probe_SecureBoot_Off"), CheckState.Good),
            _ => ProbeNote(L.Get("Probe_SecureBoot_Other"), CheckState.Neutral),
        });

        // the image
        probeBar = null;
        if (P.Checking)
        {
            probeBar = new ProgressBar { Minimum = 0, Maximum = 1, Value = 0 };
            items.Add(probeBar);
            items.Add(Ui.Text(L.Get("Probe_Image_Checking"), secondary: true));
        }
        else if (P.Checked is { } img)
            items.Add(ProbeNote(L.Format("Probe_Image_Ok", Path.GetFileName(img.Path), img.Version.Length > 0 ? img.Version : "?", ProbeImage.SizeText(img.Size)), CheckState.Good));
        else if (P.ImageError is not null)
            items.Add(ProbeNote(L.Format("Probe_Image_Bad", P.ImageError), CheckState.Bad));
        else
            items.Add(ProbeNote(L.Get("Probe_Image_None"), CheckState.Neutral));

        // the disks
        if (P.Listing && P.Disks is null) items.Add(new ProgressRing { IsActive = true, Width = 20, Height = 20, HorizontalAlignment = HorizontalAlignment.Left });
        if (P.DiskError is not null) items.Add(ProbeNote(P.DiskError, CheckState.Bad));
        else if (P.Disks is { Count: 0 }) items.Add(ProbeNote(L.Get("Probe_Disk_None"), CheckState.Neutral));
        if (P.Disks is { Count: > 0 } disks)
        {
            var radios = new RadioButtons { MaxColumns = 1 };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(radios, L.Get("Probe_Disk"));
            foreach (var d in disks)
            {
                var refusals = ProbeRefusals(d);
                var p = new StackPanel { Spacing = 1 };
                p.Children.Add(new TextBlock { Text = L.Format("Stick_Disk_Line", d.FriendlyName, StickRules.SizeText(d.Size), d.BusName, d.Number) });
                if (refusals.Count > 0)
                {
                    var t = Ui.Text(L.Format("Stick_Disk_Refused", string.Join("; ", refusals)), "CaptionTextBlockStyle");
                    t.Foreground = Ui.Brush(CheckState.Bad);
                    p.Children.Add(t);
                }
                else p.Children.Add(Ui.Text(L.Get("Stick_Disk_Ok"), "CaptionTextBlockStyle", secondary: true));
                if (StickRules.HoldsEfi(d))
                {
                    var t = Ui.Text(L.Get("Probe_Disk_Efi"), "CaptionTextBlockStyle");
                    t.Foreground = Ui.Brush(CheckState.Warning);
                    p.Children.Add(t);
                }
                if (StickRules.IsProbeStick(d)) p.Children.Add(Ui.Text(L.Get("Probe_Disk_Probe"), "CaptionTextBlockStyle", secondary: true));
                radios.Items.Add(new RadioButton { Content = p, Tag = d.Number, IsEnabled = refusals.Count == 0 && !P.Writing });
            }
            radios.SelectedIndex = disks.FindIndex(d => d.Number == P.Disk);
            radios.SelectionChanged += (_, _) =>
            {
                if (radios.SelectedItem is RadioButton { Tag: uint n } && n != P.Disk) { P.Disk = n; P.Result = null; Build(); }
            };
            items.Add(radios);
            if (disks.Any(d => d.Size > StickRules.LargeDisk))
            {
                var large = new CheckBox { Content = L.Get("Probe_AllowLarge"), IsChecked = P.AllowLarge, IsEnabled = !P.Writing };
                large.Click += (_, _) => { P.AllowLarge = large.IsChecked == true; Build(); };
                items.Add(large);
            }
        }

        // writing
        if (P.Writing)
        {
            var (stage, f) = P.Progress ?? ("start", 0);
            items.Add(new ProgressBar { Minimum = 0, Maximum = 1, Value = f, IsIndeterminate = stage == "start" });
            items.Add(Ui.Text(L.Get(ProbeStageKey(stage)), secondary: true));
        }
        if (P.Result is { } r)
        {
            items.Add(r.Ok
                ? ProbeNote(L.Get("Probe_Write_Done"), CheckState.Good)
                : ProbeNote(L.Format(r.Written ? "Probe_Write_FailedAfter" : "Stick_Write_Failed", r.Error ?? ""), CheckState.Bad));
            if (r.Lines.Count > 0) items.Add(ProbeMono(string.Join("\n", r.Lines)));
            if (r.Ok) items.Add(Ui.Text(L.Get("Probe_Write_Next"), secondary: true));
        }

        var disk = P.Disks?.FirstOrDefault(d => d.Number == P.Disk);
        var choose = Ui.Button(L.Get("Probe_Image_Choose"), async (_, _) => await ChooseImage(), glyph: Glyphs.Folder);
        choose.IsEnabled = !P.Writing && !P.Checking;
        var refresh = Ui.Button(L.Get("Refresh"), async (_, _) => await ListProbeDisks(), glyph: Glyphs.Refresh);
        refresh.IsEnabled = !P.Listing && !P.Writing;
        var write = Ui.Button(L.Get("Probe_Write_Button"), async (_, _) => { if (P.Disks?.FirstOrDefault(d => d.Number == P.Disk) is { } d) await ProbeConfirm(d); },
            accent: true, glyph: Glyphs.Drive);
        write.IsEnabled = P.Checked is not null && disk is not null && ProbeRefusals(disk).Count == 0 && !P.Writing && !P.Checking;
        var row = Ui.Horizontal(10, choose, refresh, write);
        items.Add(row);
        if (P.Checked is null && !P.Checking) items.Add(Ui.Text(L.Get("Probe_Write_NeedImage"), "CaptionTextBlockStyle", secondary: true));
        else if (disk is null && !P.Writing) items.Add(Ui.Text(L.Get("Probe_Write_NeedDisk"), "CaptionTextBlockStyle", secondary: true));
        items.Add(Ui.Text(L.Get("Probe_Footer"), "CaptionTextBlockStyle", secondary: true));
        return Ui.Section(Glyphs.Drive, L.Get("Probe_Section"), items.ToArray());
    }

    static string ProbeStageKey(string stage) => stage switch
    {
        "verify-image" => "Probe_Stage_Verify", "write" => "Probe_Stage_Write", "read-back" => "Stick_Stage_ReadBack", "done" => "Stick_Stage_Done",
        _ => "Stick_Stage_Start",
    };

    /// The image found beside the app (null) or chosen, checked off the window's thread.
    async Task CheckImage(string? chosen)
    {
        if (P.Checking || P.Writing) return;
        string? path = chosen;
        if (path is null)
        {
            try { path = (await Task.Run(() => ProbeImage.Find(ProbeImage.Folders(AppContext.BaseDirectory, LocalAppData)))).FirstOrDefault(); }
            catch (Exception) { path = null; }
        }
        P.Image = path;
        P.Checked = null;
        P.ImageError = null;
        P.Result = null;
        if (path is null) { Build(); return; }
        P.Checking = true;
        Build();
        try
        {
            P.Checked = await Task.Run(() => ProbeImage.Verify(path, f => DispatcherQueue.TryEnqueue(() => { if (probeBar is not null) probeBar.Value = f; })));
        }
        catch (Exception e) { P.ImageError = e.Message; }
        finally { P.Checking = false; Build(); }
    }

    async Task ChooseImage()
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FileOpenPicker();
            picker.FileTypeFilter.Add(".img");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
            var file = await picker.PickSingleFileAsync();
            if (file is null) return;
            await CheckImage(file.Path);
        }
        catch (Exception e) { P.ImageError = e.Message; Build(); }
    }

    async Task ListProbeDisks()
    {
        if (P.Listing) return;
        P.Listing = true;
        Build();
        try
        {
            var all = await Task.Run(() => new WmiStickSystem().ListDisks());
            P.Disks = [.. all.Where(d => StickRules.Listed(d, ProbePolicy()))];
            P.DiskError = null;
            if (P.Disk is uint n && P.Disks.All(d => d.Number != n)) P.Disk = null;
        }
        catch (Exception e) { P.DiskError = e.Message; P.Disks = []; }
        finally { P.Listing = false; Build(); }
    }

    async Task ProbeConfirm(StickDisk disk)
    {
        if (P.Checked is not { } image) return;
        var d = new ContentDialog { Title = L.Format("Stick_Confirm_Title", disk.FriendlyName), XamlRoot = XamlRoot, RequestedTheme = Ui.Root.ActualTheme };
        d.Resources["ContentDialogMaxWidth"] = 640.0;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(ProbeNote(L.Format("Stick_Confirm_Erase", disk.FriendlyName, StickRules.SizeText(disk.Size), disk.BusName, disk.Number), CheckState.Bad));
        p.Children.Add(Ui.Text(disk.Partitions.Count == 0 ? L.Get("Stick_Confirm_Empty")
            : L.Format("Stick_Confirm_Holds", string.Join(", ", disk.Partitions.Select(ProbePartitionText))), "CaptionTextBlockStyle", secondary: true));
        var box = new TextBox { IsSpellCheckEnabled = false };   // no placeholder: an empty box must look empty
        var expected = disk.ConfirmText;
        CheckBox? efiBox = null;
        efiBox = EraseWarnings.AddEfi(p, disk, () => d.IsPrimaryButtonEnabled = Ready());
        EraseWarnings.AddProbe(p, disk);
        p.Children.Add(Ui.Text(L.Format("Probe_Confirm_What", image.Version.Length > 0 ? image.Version : Path.GetFileName(image.Path))));
        p.Children.Add(Ui.Text(L.Get("Stick_Confirm_Uac"), "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(Ui.Text(L.Get("Stick_Confirm_Type")));
        var shown = Ui.Text(expected, "BodyStrongTextBlockStyle");
        shown.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        p.Children.Add(shown);
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, L.Get("Stick_Confirm_Type"));
        p.Children.Add(box);
        d.Content = new ScrollViewer { Content = p, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
        d.PrimaryButtonText = L.Get("Stick_Confirm_Button");
        d.CloseButtonText = L.Get("Efi_Cancel");
        d.DefaultButton = ContentDialogButton.Close;
        d.IsPrimaryButtonEnabled = false;
        bool Ready() => string.Equals(box.Text.Trim(), expected, StringComparison.OrdinalIgnoreCase) && EraseWarnings.Confirmed(efiBox);
        box.TextChanged += (_, _) => d.IsPrimaryButtonEnabled = Ready();
        if (await d.ShowAsync() != ContentDialogResult.Primary) return;
        if (!Ready()) return;
        await ProbeWrite(disk, image, efiBox?.IsChecked == true);
    }

    /// As the installer stick's dialog names them: an EFI system partition, a drive letter, or the partition's number.
    static string ProbePartitionText(StickPartition p) =>
        EspPartition.IsEspType(p.GptType) || p.MbrType == 0xEF ? L.Get("Stick_Partition_Esp")
        : p.Letter.Length > 0 ? p.Letter
        : L.Format("Stick_Partition_Number", p.Number);

    async Task ProbeWrite(StickDisk disk, ProbeImageInfo image, bool eraseEfi)
    {
        P.Writing = true;
        P.Result = null;
        P.Progress = null;
        Build();
        try
        {
            var q = new ProbeHelperRequest(ProbeVerb.Write, disk.Number, disk.Size, disk.Token(P.Salt), P.Salt, image.Path, eraseEfi, P.AllowLarge, App.Options.TestVhdx);
            P.Result = await ProbeHelperMain.RunElevatedAsync(q, (stage, f) => DispatcherQueue.TryEnqueue(() => { P.Progress = (stage, f); Build(); }));
        }
        catch (Exception e) { P.Result = new StickHelperResult { Ok = false, Error = e.Message }; }
        finally
        {
            P.Writing = false;
            Build();
            await ListProbeDisks();
            if (P.Result?.Ok == true)
            {
                await Task.Delay(TimeSpan.FromSeconds(3));   // Windows mounts the new XEPROBE volume
                await ReadSticks();
            }
        }
    }

    // ---- reading ----

    async Task ReadSticks()
    {
        if (P.Reading) return;
        P.Reading = true;
        P.ReadError = null;
        Build();
        try
        {
            P.Sticks = await Task.Run(() => ProbeSticks.Find().Select(root => (root, ProbeReports.Read(root))).ToList());
        }
        catch (Exception e) { P.ReadError = e.Message; P.Sticks = []; }
        finally { P.Reading = false; Build(); }
    }

    UIElement ProbeReadSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Probe_Read_Caption"), L.Get("Probe_Read_Detail")) };
        if (P.Reading) items.Add(new ProgressRing { IsActive = true, Width = 20, Height = 20, HorizontalAlignment = HorizontalAlignment.Left });
        if (P.ReadError is not null) items.Add(ProbeNote(P.ReadError, CheckState.Bad));
        else if (P.Sticks is { Count: 0 } && !P.Reading) items.Add(ProbeNote(L.Get("Probe_Read_None"), CheckState.Neutral));
        foreach (var (root, reports) in P.Sticks ?? [])
        {
            items.Add(Ui.Text(L.Format("Probe_Read_Stick", root, reports.Count), "BodyStrongTextBlockStyle"));
            if (reports.Count == 0) items.Add(Ui.Text(L.Get("Probe_Read_Empty"), secondary: true));
            for (int i = 0; i < reports.Count; i++) items.Add(ProbeReportCard(reports[i], expanded: i == 0));
        }
        var look = Ui.Button(L.Get("Probe_Read_Look"), async (_, _) => await ReadSticks(), glyph: Glyphs.Refresh);
        look.IsEnabled = !P.Reading;
        items.Add(look);
        return Ui.Section(Glyphs.Document, L.Get("Probe_Read"), items.ToArray());
    }

    static UIElement ProbeReportCard(ProbeReport r, bool expanded)
    {
        var p = new StackPanel { Spacing = 8 };
        p.Children.Add(r.State switch
        {
            ProbeReportState.Sealed => ProbeNote(L.Format("Probe_Report_Sealed", r.Id, r.SealedFiles, ProbeImage.SizeText(r.Size)), CheckState.Good),
            ProbeReportState.PrivacyCheckFailed => ProbeNote(L.Format("Probe_Report_Privacy", r.Id), CheckState.Warning),
            _ => ProbeNote(L.Format("Probe_Report_Broken", r.Id), CheckState.Bad),
        });
        if (r.Problems.Count > 0) p.Children.Add(ProbeMono(string.Join("\n", r.Problems)));
        if (r.TooLargeForGitHub) p.Children.Add(ProbeNote(L.Get("Probe_Report_TooLarge"), CheckState.Warning));
        if (r.SummaryText.Length > 0)
        {
            var text = new ScrollViewer { Content = ProbeMono(r.SummaryText), MaxHeight = 320, VerticalScrollBarVisibility = ScrollBarVisibility.Auto };
            p.Children.Add(new Expander
            {
                Header = L.Get("Probe_Report_Summary"), Content = text, IsExpanded = expanded,
                HorizontalAlignment = HorizontalAlignment.Stretch, HorizontalContentAlignment = HorizontalAlignment.Stretch,
            });
        }
        var file = r.Archive ?? r.SummaryFile;
        var show = Ui.Button(L.Get("Report_Show"), (_, _) => { if (file is not null) ProbeSticks.Reveal(file); }, glyph: Glyphs.Folder);
        show.IsEnabled = file is not null;
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
        var links = Ui.Horizontal(4, Ui.Link(L.Get("Report_NewGpu"), Project.NewGpuFormUrl(r.Id)), Ui.Link(L.Get("Report_Bug"), Project.BugFormUrl(r.Id)));
        Grid.SetColumn(links, 2);
        g.Children.Add(links);
        p.Children.Add(g);
        if (r.State != ProbeReportState.PrivacyCheckFailed) p.Children.Add(Ui.Text(L.Get("Probe_Report_Attach"), "CaptionTextBlockStyle", secondary: true));
        return Ui.TintedCard(p, r.State switch { ProbeReportState.Sealed => CheckState.Good, ProbeReportState.PrivacyCheckFailed => CheckState.Warning, _ => CheckState.Bad });
    }

    // ---- pieces (as the installer stick page draws them) ----

    static UIElement ProbeNote(string text, CheckState state)
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

    static Border ProbeMono(string text)
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
