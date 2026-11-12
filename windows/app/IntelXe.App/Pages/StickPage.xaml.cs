// SPDX-License-Identifier: MIT
// The stick page: a macOS installer USB stick made from Windows, as Dortania's Windows guide makes it, in the app.
// 1. the macOS version (Core/Stick/macos-recovery.json: 26 Tahoe, 15 Sequoia); 2. its recovery image from Apple's
// recovery server, every piece checked against Apple's signed chunklist (Core/Stick/Recovery.cs, in this process);
// 3. the USB stick, from the USB disks only, each refused disk with its reasons (StickRules); 4. an OpenCore EFI folder
// to copy, optional (IEfiSource); 5. erasing and writing, after the user typed the disk's name and size, by the
// elevated helper (this exe with --stick-helper write, through "runas"), which checks everything again first.
using IntelXe.Core;
using IntelXe.Core.Bios;
using IntelXe.Core.Stick;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;
using Microsoft.UI.Xaml.Media;

namespace IntelXe.App.Pages;

public sealed partial class StickPage : Page
{
    sealed class PageState
    {
        public string Version = RecoveryData.Embedded.Versions.FirstOrDefault(v => v.Primary)?.Id ?? "tahoe";
        public readonly Dictionary<string, RecoveryFiles?> Files = [];
        public bool Downloading;
        public RecoveryProgress? Progress;
        public string? DownloadError;
        public CancellationTokenSource? Cancel;
        public List<StickDisk>? Disks;
        public string? DiskError;
        public bool Listing;
        public uint? Disk;
        public string? Efi;
        public string? EfiError;
        public bool Writing;
        public (string Stage, double Fraction)? WriteProgress;
        public StickHelperResult? Result;
        public bool ConfirmOpened;
        public readonly string Salt = StickIdentity.NewSalt();
    }

    static readonly PageState S = new();

    public StickPage()
    {
        InitializeComponent();
        Loaded += (_, _) => { ActualThemeChanged += OnTheme; Build(); if (S.Disks is null) _ = ListDisks(); };
        Unloaded += (_, _) => ActualThemeChanged -= OnTheme;
    }

    void OnTheme(FrameworkElement sender, object args) => Build();

    static string Lang => BiosSteps.Lang(L.Language);
    static RecoveryVersion Version => RecoveryData.Embedded.Find(S.Version);
    static string CacheRoot => App.Options.RecoveryDir is { Length: > 0 } dir ? Path.GetFullPath(dir)
        : Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "recovery");
    static string CacheFor(string id) => Path.Combine(CacheRoot, id);
    static StickPolicy Policy(long imageSize) => new(StickRules.MinimumFor(imageSize), App.Options.TestVhdx, [AppContext.BaseDirectory[..2]]);

    static RecoveryFiles? FilesFor(string id)
    {
        if (!S.Files.TryGetValue(id, out var f)) S.Files[id] = f = RecoveryFiles.Find(CacheFor(id));
        return f;
    }

    void Build()
    {
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Stick"), L.Get("Stick_Caption")));
        if (App.Options.TestVhdx is { Length: > 0 } vhdx)
            column.Children.Add(new InfoBar { Severity = InfoBarSeverity.Warning, IsOpen = true, IsClosable = false, Title = L.Get("Stick_TestMode"), Message = L.Format("Stick_TestMode_Detail", vhdx) });
        column.Children.Add(VersionSection());
        column.Children.Add(ImageSection());
        column.Children.Add(DiskSection());
        column.Children.Add(EfiSection());
        column.Children.Add(WriteSection());
        var note = Ui.Text(L.Get("Stick_Footer"), "CaptionTextBlockStyle", secondary: true);
        note.Margin = new Thickness(4, 14, 0, 0);
        column.Children.Add(note);
        Host.Children.Clear();
        Host.Children.Add(scroll);

        if (App.Options.StickConfirm && !S.ConfirmOpened && S.Disks is not null && Writable() is { } d && FilesFor(S.Version) is not null)
        {
            S.ConfirmOpened = true;
            DispatcherQueue.TryEnqueue(async () => await Confirm(d));
        }
    }

    // ---- 1. the version ----
    UIElement VersionSection()
    {
        var radios = new RadioButtons { MaxColumns = 1 };
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(radios, L.Get("Stick_Version"));
        foreach (var v in RecoveryData.Embedded.Versions)
        {
            var p = new StackPanel { Spacing = 1 };
            p.Children.Add(new TextBlock { Text = v.Primary ? L.Format("Stick_Version_Recommended", v.Name) : v.Name });
            p.Children.Add(Ui.Text(v.Status.In(Lang), "CaptionTextBlockStyle", secondary: true));
            radios.Items.Add(new RadioButton { Content = p, Tag = v.Id, IsEnabled = !S.Downloading && !S.Writing });
        }
        radios.SelectedIndex = RecoveryData.Embedded.Versions.FindIndex(v => v.Id == S.Version);
        radios.SelectionChanged += (_, _) =>
        {
            if (radios.SelectedItem is RadioButton { Tag: string id } && id != S.Version) { S.Version = id; S.DownloadError = null; S.Result = null; Build(); }
        };
        var guide = Ui.Link(L.Format("Stick_Version_Guide", Version.Name), new Uri(Version.Guide));
        return Ui.Section(Glyphs.List, L.Get("Stick_Version"), Ui.Caption(L.Get("Stick_Version_Caption")), radios, guide);
    }

    // ---- 2. the recovery image ----
    UIElement ImageSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Stick_Image_Caption"), L.Get("Stick_Image_Detail")) };
        var files = FilesFor(S.Version);
        if (S.Downloading)
        {
            var p = S.Progress;
            double total = p?.Total ?? 0;
            var bar = new ProgressBar { Minimum = 0, Maximum = 1, Value = total > 0 ? p!.Bytes / total : 0, IsIndeterminate = total <= 0 };
            items.Add(bar);
            items.Add(Ui.Text(p is { Total: > 0 }
                ? L.Format("Stick_Downloading", Mb(p.Bytes), Mb(p.Total), p.Chunks, p.ChunkCount)
                : L.Get(p?.Stage == "chunklist" ? "Stick_Downloading_List" : "Stick_Downloading_Ask"), secondary: true));
            items.Add(Ui.Button(L.Get("Stick_Cancel"), (_, _) => S.Cancel?.Cancel()));
        }
        else
        {
            if (files is not null)
                items.Add(Note(L.Format("Stick_Image_Ready", Version.Name, files.Product, Mb(files.Size)), CheckState.Good));
            else
                items.Add(Note(L.Format("Stick_Image_None", Version.Name), CheckState.Neutral));
            if (S.DownloadError is not null) items.Add(Note(L.Format("Stick_Download_Failed", S.DownloadError), CheckState.Bad));
            items.Add(Ui.Horizontal(10,
                Ui.Button(L.Get(files is null ? "Stick_Download" : "Stick_Download_Again"), async (_, _) => await Download(), accent: files is null, glyph: Glyphs.Refresh)));
            items.Add(Ui.Text(L.Format("Stick_Cache", CacheFor(S.Version)), "CaptionTextBlockStyle", secondary: true));
        }
        return Ui.Section(Glyphs.Shield, L.Get("Stick_Image"), items.ToArray());
    }

    static string Mb(long bytes) => (bytes / 1e6).ToString("0", System.Globalization.CultureInfo.CurrentUICulture);

    async Task Download()
    {
        if (S.Downloading || S.Writing) return;
        var v = Version;
        S.Downloading = true;
        S.DownloadError = null;
        S.Progress = null;
        S.Cancel = new CancellationTokenSource();
        Build();
        try
        {
            using var client = new RecoveryClient();
            var progress = new Progress<RecoveryProgress>(p => { S.Progress = p; Build(); });   // reported on this (UI) thread
            S.Files[v.Id] = await client.DownloadAsync(v, CacheFor(v.Id), progress, S.Cancel.Token);
        }
        catch (OperationCanceledException) { S.DownloadError = L.Get("Stick_Download_Cancelled"); S.Files.Remove(v.Id); }
        catch (Exception e) { S.DownloadError = e.Message; S.Files.Remove(v.Id); }
        finally
        {
            S.Downloading = false;
            S.Cancel = null;
            Build();
        }
    }

    // ---- 3. the stick ----
    async Task ListDisks()
    {
        if (S.Listing) return;
        S.Listing = true;
        Build();
        try
        {
            var all = await Task.Run(() => new WmiStickSystem().ListDisks());
            S.Disks = [.. all.Where(d => StickRules.Listed(d, Policy(0)))];
            S.DiskError = null;
            if (S.Disk is uint n && S.Disks.All(d => d.Number != n)) S.Disk = null;
        }
        catch (Exception e) { S.DiskError = e.Message; S.Disks = []; }
        finally { S.Listing = false; Build(); }
    }

    static long ImageSize => FilesFor(S.Version)?.Size ?? 1_100_000_000;

    static IReadOnlyList<string> RefusalsOf(StickDisk d) => StickRules.Refusals(d, Policy(ImageSize));

    static StickDisk? Writable() =>
        S.Disks?.FirstOrDefault(d => d.Number == S.Disk && RefusalsOf(d).Count == 0) ?? ConfirmTarget();

    /// --stick-confirm opens the dialog only for the test VHDX (--test-vhdx), never for a real stick.
    static StickDisk? ConfirmTarget() =>
        App.Options.StickConfirm && App.Options.TestVhdx is { Length: > 0 } vhdx
            ? S.Disks?.FirstOrDefault(d => d.BusType == StickRules.FileBackedVirtualBus && string.Equals(d.VirtualFile, Path.GetFullPath(vhdx), StringComparison.OrdinalIgnoreCase) && RefusalsOf(d).Count == 0)
            : null;

    UIElement DiskSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Stick_Disk_Caption"), L.Get("Stick_Disk_Detail")) };
        if (S.Listing && S.Disks is null) items.Add(new ProgressRing { IsActive = true, Width = 20, Height = 20, HorizontalAlignment = HorizontalAlignment.Left });
        if (S.DiskError is not null) items.Add(Note(S.DiskError, CheckState.Bad));
        else if (S.Disks is { Count: 0 }) items.Add(Note(L.Get("Stick_Disk_None"), CheckState.Neutral));
        if (S.Disks is { Count: > 0 } disks)
        {
            var radios = new RadioButtons { MaxColumns = 1 };
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(radios, L.Get("Stick_Disk"));
            foreach (var d in disks)
            {
                var refusals = RefusalsOf(d);
                var p = new StackPanel { Spacing = 1 };
                p.Children.Add(new TextBlock { Text = L.Format("Stick_Disk_Line", d.FriendlyName, StickRules.SizeText(d.Size), d.BusName, d.Number) });
                if (refusals.Count > 0)
                {
                    var t = Ui.Text(L.Format("Stick_Disk_Refused", string.Join("; ", refusals)), "CaptionTextBlockStyle");
                    t.Foreground = Ui.Brush(CheckState.Bad);
                    p.Children.Add(t);
                }
                else p.Children.Add(Ui.Text(L.Get("Stick_Disk_Ok"), "CaptionTextBlockStyle", secondary: true));
                radios.Items.Add(new RadioButton { Content = p, Tag = d.Number, IsEnabled = refusals.Count == 0 && !S.Writing });
            }
            var sel = disks.FindIndex(d => d.Number == S.Disk);
            if (sel < 0 && ConfirmTarget() is { } target) sel = disks.FindIndex(d => d.Number == target.Number);
            radios.SelectedIndex = sel;
            radios.SelectionChanged += (_, _) =>
            {
                if (radios.SelectedItem is RadioButton { Tag: uint n } && n != S.Disk) { S.Disk = n; S.Result = null; Build(); }
            };
            items.Add(radios);
        }
        var refresh = Ui.Button(L.Get("Refresh"), async (_, _) => await ListDisks(), glyph: Glyphs.Refresh);
        refresh.IsEnabled = !S.Listing && !S.Writing;
        items.Add(refresh);
        return Ui.Section(Glyphs.Drive, L.Get("Stick_Disk"), items.ToArray());
    }

    // ---- 4. the EFI ----
    UIElement EfiSection()
    {
        var items = new List<UIElement> { Ui.Caption(L.Get("Stick_Efi_Caption"), L.Get("Stick_Efi_Detail")) };
        if (S.Efi is not null) items.Add(Note(L.Format("Stick_Efi_Chosen", S.Efi), CheckState.Good));
        if (S.EfiError is not null) items.Add(Note(S.EfiError, CheckState.Bad));
        var choose = Ui.Button(L.Get("Stick_Efi_Choose"), async (_, _) => await ChooseEfi(), glyph: Glyphs.Folder);
        choose.IsEnabled = !S.Writing;
        var row = Ui.Horizontal(10, choose);
        // the EFI built on the Build page, through the same IEfiSource seam
        if (BuildSession.BuiltOutDir() is { } built)
        {
            var useBuilt = Ui.Button(L.Get("Stick_Efi_UseBuilt"), async (_, _) => await UseEfi(new Core.EfiBuild.BuiltEfiSource(built, built)), accent: S.Efi is null, glyph: Glyphs.Settings);
            useBuilt.IsEnabled = !S.Writing && !string.Equals(S.Efi, BuildSession.BuiltEfiDir(), StringComparison.OrdinalIgnoreCase);
            row.Children.Insert(0, useBuilt);
        }
        if (S.Efi is not null)
        {
            var clear = Ui.Button(L.Get("Stick_Efi_Remove"), (_, _) => { S.Efi = null; S.EfiError = null; Build(); });
            clear.IsEnabled = !S.Writing;
            row.Children.Add(clear);
        }
        items.Add(row);
        return Ui.Section(Glyphs.Folder, L.Get("Stick_Efi"), items.ToArray());
    }

    async Task ChooseEfi()
    {
        try
        {
            var picker = new Windows.Storage.Pickers.FolderPicker();
            picker.FileTypeFilter.Add("*");
            WinRT.Interop.InitializeWithWindow.Initialize(picker, WinRT.Interop.WindowNative.GetWindowHandle(App.Window));
            var folder = await picker.PickSingleFolderAsync();
            if (folder is null) return;
            S.Efi = await new FolderEfiSource(folder.Path).PrepareAsync();
            S.EfiError = null;
        }
        catch (Exception e) { S.Efi = null; S.EfiError = e.Message; }
        Build();
    }

    async Task UseEfi(IEfiSource source)
    {
        try
        {
            S.Efi = await source.PrepareAsync();
            S.EfiError = null;
        }
        catch (Exception e) { S.Efi = null; S.EfiError = e.Message; }
        Build();
    }

    // ---- 5. erase and write ----
    UIElement WriteSection()
    {
        var items = new List<UIElement>();
        var files = FilesFor(S.Version);
        var disk = S.Disks?.FirstOrDefault(d => d.Number == S.Disk);
        items.Add(Ui.Text(L.Get("Stick_Write_Caption"), "BodyTextBlockStyle", secondary: true));
        if (files is null) items.Add(Note(L.Get("Stick_Write_NeedImage"), CheckState.Neutral));
        if (disk is null) items.Add(Note(L.Get("Stick_Write_NeedDisk"), CheckState.Neutral));
        if (S.Writing)
        {
            var (stage, f) = S.WriteProgress ?? ("start", 0);
            items.Add(new ProgressBar { Minimum = 0, Maximum = 1, Value = f, IsIndeterminate = stage is "start" or "erase" or "copy-efi" });
            items.Add(Ui.Text(L.Get(StageKey(stage)), secondary: true));
        }
        if (S.Result is { } r)
        {
            items.Add(r.Ok
                ? Note(L.Get("Stick_Write_Done"), CheckState.Good)
                : Note(L.Format(r.Written ? "Stick_Write_FailedAfter" : "Stick_Write_Failed", r.Error ?? ""), CheckState.Bad));
            if (r.Lines.Count > 0) items.Add(Mono(string.Join("\n", r.Lines)));
            if (r.Ok) items.Add(Ui.Text(L.Get("Stick_Write_Next"), secondary: true));
        }
        var write = Ui.Button(L.Get("Stick_Write_Button"), async (_, _) => { if (Writable() is { } d) await Confirm(d); }, accent: true, glyph: Glyphs.Drive);
        write.IsEnabled = files is not null && disk is not null && RefusalsOf(disk).Count == 0 && !S.Writing && !S.Downloading;
        items.Add(write);
        return Ui.Section(Glyphs.Warning, L.Get("Stick_Write"), items.ToArray());
    }

    static string StageKey(string stage) => stage switch
    {
        "verify-source" => "Stick_Stage_Verify", "erase" => "Stick_Stage_Erase", "copy-image" => "Stick_Stage_Copy", "copy-efi" => "Stick_Stage_CopyEfi",
        "verify-stick" => "Stick_Stage_ReadBack", "done" => "Stick_Stage_Done", _ => "Stick_Stage_Start",
    };

    async Task Confirm(StickDisk disk)
    {
        var files = FilesFor(S.Version);
        if (files is null) return;
        var d = new ContentDialog { Title = L.Format("Stick_Confirm_Title", disk.FriendlyName), XamlRoot = XamlRoot, RequestedTheme = Ui.Root.ActualTheme };
        d.Resources["ContentDialogMaxWidth"] = 640.0;
        var p = new StackPanel { Spacing = 10 };
        p.Children.Add(Note(L.Format("Stick_Confirm_Erase", disk.FriendlyName, StickRules.SizeText(disk.Size), disk.BusName, disk.Number), CheckState.Bad));
        p.Children.Add(Ui.Text(disk.Partitions.Count == 0 ? L.Get("Stick_Confirm_Empty")
            : L.Format("Stick_Confirm_Holds", string.Join(", ", disk.Partitions.Select(PartitionText))), "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(Ui.Text(L.Format(S.Efi is null ? "Stick_Confirm_What" : "Stick_Confirm_WhatEfi", Version.Name)));
        p.Children.Add(Ui.Text(L.Get("Stick_Confirm_Uac"), "CaptionTextBlockStyle", secondary: true));
        p.Children.Add(Ui.Text(L.Get("Stick_Confirm_Type")));
        var expected = disk.ConfirmText;
        var shown = Ui.Text(expected, "BodyStrongTextBlockStyle");
        shown.FontFamily = new FontFamily("Cascadia Mono, Consolas");
        p.Children.Add(shown);
        var box = new TextBox { IsSpellCheckEnabled = false };   // no placeholder: an empty box must look empty
        Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(box, L.Get("Stick_Confirm_Type"));
        p.Children.Add(box);
        d.Content = p;
        d.PrimaryButtonText = L.Get("Stick_Confirm_Button");
        d.CloseButtonText = L.Get("Efi_Cancel");
        d.DefaultButton = ContentDialogButton.Close;
        d.IsPrimaryButtonEnabled = false;
        box.TextChanged += (_, _) => d.IsPrimaryButtonEnabled = string.Equals(box.Text.Trim(), expected, StringComparison.OrdinalIgnoreCase);
        if (await d.ShowAsync() != ContentDialogResult.Primary) return;
        if (!string.Equals(box.Text.Trim(), expected, StringComparison.OrdinalIgnoreCase)) return;
        await Write(disk, files);
    }

    /// "EFI system partition", "E:", "partition 2": what the user may recognise (an EFI stick they keep, a drive letter).
    static string PartitionText(StickPartition p) =>
        EspPartition.IsEspType(p.GptType) || p.MbrType == 0xEF ? L.Get("Stick_Partition_Esp")
        : p.Letter.Length > 0 ? p.Letter
        : L.Format("Stick_Partition_Number", p.Number);

    async Task Write(StickDisk disk, RecoveryFiles files)
    {
        S.Writing = true;
        S.Result = null;
        S.WriteProgress = null;
        Build();
        try
        {
            var q = new StickHelperRequest(StickVerb.Write, disk.Number, disk.Token(S.Salt), S.Salt, disk.Size, files.Folder, S.Efi, App.Options.TestVhdx);
            S.Result = await StickRunner.RunElevatedAsync(q, (stage, f) => DispatcherQueue.TryEnqueue(() => { S.WriteProgress = (stage, f); Build(); }));
            if (S.Result.Ok) BuildSession.RecordStick(S.Efi);   // the Set up page's stick step, when it carries the built EFI
        }
        catch (Exception e) { S.Result = new StickHelperResult { Ok = false, Error = e.Message }; }
        finally
        {
            S.Writing = false;
            Build();
            _ = ListDisks();
        }
    }

    // ---- pieces ----
    static UIElement Note(string text, CheckState state)
    {
        var g = new Grid { ColumnSpacing = 8 };
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
        g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
        // an (i), not the neutral circle: next to radio buttons a circle reads as one
        var icon = Ui.Icon(state == CheckState.Neutral ? Glyphs.Info : Ui.StateGlyph(state), 14, Ui.Brush(state));
        icon.VerticalAlignment = VerticalAlignment.Top;
        icon.Margin = new Thickness(0, 3, 0, 0);
        g.Children.Add(icon);
        var t = Ui.Text(text);
        Grid.SetColumn(t, 1);
        g.Children.Add(t);
        return g;
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
