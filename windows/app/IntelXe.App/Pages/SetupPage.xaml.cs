// SPDX-License-Identifier: MIT
// The "Set up" page: one guided flow from this Windows PC to a macOS install, over the app's other pages
// (Core/Setup/SetupWizard.cs): 1. this PC (the Overview's facts, what stops the install first); 2. the firmware (BIOS)
// settings, confirmed by the user; 3. the OpenCore EFI built for this PC (the Build page); 4. the macOS installer USB
// stick with that EFI (the USB installer page); 5. after macOS is installed: stage 3 on macOS (IntelXe Manager for Mac),
// stage 4 from macOS or from Windows (the EFI page, on the stick's or the disk's EFI). The step list shows each step as
// done, next or blocked. The progress is kept in the app's settings file. This page writes nothing but that file: every
// step that changes a disk happens on its own page, behind that page's confirmation.
using System.Globalization;
using IntelXe.Core;
using IntelXe.Core.Setup;
using Microsoft.UI.Text;
using Microsoft.UI.Xaml;
using Microsoft.UI.Xaml.Controls;

namespace IntelXe.App.Pages;

public sealed partial class SetupPage : Page
{
    static BuildSession B => BuildSession.Current;
    static SetupProgress P => Settings.Current.Setup;

    public SetupPage()
    {
        InitializeComponent();
        Loaded += (_, _) =>
        {
            AppState.Current.Changed += OnChanged;
            B.Changed += OnChanged;
            ActualThemeChanged += OnTheme;
            B.Load();
            Build();
        };
        Unloaded += (_, _) => { AppState.Current.Changed -= OnChanged; B.Changed -= OnChanged; ActualThemeChanged -= OnTheme; };
    }

    void OnChanged() => DispatcherQueue.TryEnqueue(Build);
    void OnTheme(FrameworkElement sender, object args) => Build();

    static void Save(SetupProgress next)
    {
        Settings.Current.Setup = next;
        Settings.Current.Save();
    }

    void Build()
    {
        var states = SetupWizard.Evaluate(P, B.SetupInputs());
        var (scroll, column) = Ui.PageColumn();
        column.Children.Add(Ui.PageHeader(L.Get("Nav_Setup"), L.Get("Setup_Caption")));
        column.Children.Add(StepList(states));
        column.Children.Add(ThisPc(states));
        column.Children.Add(Bios(states));
        column.Children.Add(BuildEfi(states));
        column.Children.Add(Stick(states));
        column.Children.Add(AfterInstall(states));
        var start = Ui.Button(L.Get("Setup_StartOver"), async (_, _) => await StartOver(), glyph: Glyphs.Undo);
        start.Margin = new Thickness(0, 16, 0, 0);
        column.Children.Add(start);
        var note = Ui.Text(L.Get("Setup_Footer"), "CaptionTextBlockStyle", secondary: true);
        note.Margin = new Thickness(4, 14, 0, 0);
        column.Children.Add(note);
        Host.Children.Clear();
        Host.Children.Add(scroll);
    }

    static string Title(SetupStep s) => s switch
    {
        SetupStep.ThisPc => L.Get("Setup_Step_Pc"),
        SetupStep.Bios => L.Get("Setup_Step_Bios"),
        SetupStep.BuildEfi => L.Get("Setup_Step_Efi"),
        SetupStep.Stick => L.Get("Setup_Step_Stick"),
        _ => L.Get("Setup_Step_After"),
    };

    static string StateText(StepState s) => s switch
    {
        StepState.Done => L.Get("Setup_State_Done"),
        StepState.Next => L.Get("Setup_State_Next"),
        _ => L.Get("Setup_State_Blocked"),
    };

    static CheckState Color(StepState s) => s switch { StepState.Done => CheckState.Good, StepState.Next => CheckState.Warning, _ => CheckState.Neutral };

    static StepStatus Of(IReadOnlyList<StepStatus> states, SetupStep step) => states.First(s => s.Step == step);

    // ---- the step list ----
    UIElement StepList(IReadOnlyList<StepStatus> states)
    {
        var list = new StackPanel { Spacing = 10 };
        foreach (var s in states)
        {
            var g = new Grid { ColumnSpacing = 12 };
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = new GridLength(1, GridUnitType.Star) });
            g.ColumnDefinitions.Add(new ColumnDefinition { Width = GridLength.Auto });
            var icon = Ui.Icon(s.State switch { StepState.Done => Glyphs.Ok, StepState.Next => Glyphs.Open, _ => Glyphs.Neutral }, 18, Ui.Brush(Color(s.State)));
            icon.VerticalAlignment = VerticalAlignment.Top;
            Microsoft.UI.Xaml.Automation.AutomationProperties.SetName(icon, StateText(s.State));
            g.Children.Add(icon);
            var texts = new StackPanel { Spacing = 1 };
            var title = Ui.Text(L.Format("Setup_Step_Line", (int)s.Step + 1, Title(s.Step)));
            if (s.State == StepState.Next) title.FontWeight = FontWeights.SemiBold;
            texts.Children.Add(title);
            if (s.State == StepState.Blocked && !s.Reason.IsEmpty) texts.Children.Add(Ui.Text(L.Resolve(s.Reason), "CaptionTextBlockStyle", secondary: true));
            Grid.SetColumn(texts, 1);
            g.Children.Add(texts);
            var badge = Ui.Badge(StateText(s.State), Color(s.State));
            Grid.SetColumn(badge, 2);
            g.Children.Add(badge);
            list.Children.Add(g);
        }
        return Ui.Section(Glyphs.List, L.Get("Setup_Steps"), list);
    }

    /// A step's section: its number and title, its state, then its content.
    static UIElement StepSection(StepStatus s, string glyph, List<UIElement> items)
    {
        var badge = Ui.Badge(StateText(s.State), Color(s.State));
        badge.HorizontalAlignment = HorizontalAlignment.Left;
        items.Insert(0, badge);
        if (s.State == StepState.Blocked && !s.Reason.IsEmpty) items.Insert(1, Note(L.Resolve(s.Reason), CheckState.Neutral));
        return Ui.Section(glyph, L.Format("Setup_Step_Line", (int)s.Step + 1, Title(s.Step)), items.ToArray());
    }

    // ---- 1. this PC ----
    UIElement ThisPc(IReadOnlyList<StepStatus> states)
    {
        var s = Of(states, SetupStep.ThisPc);
        var state = AppState.Current;
        var items = new List<UIElement> { Ui.Caption(L.Get("Setup_Pc_Caption")) };
        if (state.Facts is null)
            items.Add(Ui.Horizontal(10, new ProgressRing { IsActive = true, Width = 20, Height = 20 }, Ui.Text(L.Get("Overview_Reading"), secondary: true)));
        else
        {
            foreach (var b in SetupWizard.Blocking(state.Facts, state.Checks)) items.Add(Note(L.Resolve(b), CheckState.Bad));
            foreach (var c in SetupWizard.Ordered(state.Checks)) items.Add(Ui.CheckRow(c));
        }
        var refresh = Ui.Button(L.Get("Refresh"), async (_, _) => await AppState.Current.RefreshAsync(), glyph: Glyphs.Refresh);
        refresh.IsEnabled = !state.Probing;
        items.Add(Ui.Horizontal(10, Ui.Button(L.Get("Setup_Open_Overview"), (_, _) => App.Window?.Open("overview"), glyph: Glyphs.Grid), refresh));
        return StepSection(s, Glyphs.Pc, items);
    }

    // ---- 2. BIOS ----
    UIElement Bios(IReadOnlyList<StepStatus> states)
    {
        var s = Of(states, SetupStep.Bios);
        var items = new List<UIElement> { Ui.Caption(L.Get("Setup_Bios_Caption"), L.Get("Setup_Bios_Detail")) };
        var open = Ui.Button(L.Get("Setup_Open_Bios"), (_, _) => App.Window?.Open("bios"), glyph: Glyphs.Firmware);
        if (s.IsDone)
        {
            items.Add(Note(L.Format("Setup_Bios_Done", When(P.BiosConfirmed)), CheckState.Good));
            items.Add(Ui.Horizontal(10, open, Again(SetupStep.Bios)));
        }
        else
        {
            var done = Ui.Button(L.Get("Setup_Bios_Confirm"), (_, _) => { Save(SetupWizard.ConfirmBios(P, DateTime.Now)); Build(); }, accent: true, glyph: Glyphs.Ok);
            done.IsEnabled = SetupWizard.CanAct(states, SetupStep.Bios);
            items.Add(Ui.Horizontal(10, open, done));
        }
        return StepSection(s, Glyphs.Firmware, items);
    }

    // ---- 3. the EFI ----
    UIElement BuildEfi(IReadOnlyList<StepStatus> states)
    {
        var s = Of(states, SetupStep.BuildEfi);
        var items = new List<UIElement> { Ui.Caption(L.Get("Setup_Efi_Caption"), L.Get("Setup_Efi_Detail")) };
        var open = Ui.Button(L.Get("Setup_Open_Build"), (_, _) => App.Window?.Open("build"), accent: s.State == StepState.Next, glyph: Glyphs.Settings);
        if (s.IsDone)
        {
            items.Add(Note(L.Format("Setup_Efi_Done", P.EfiMacOS, When(P.EfiBuilt)), CheckState.Good));
            items.Add(Ui.Text(BuildSession.BuiltEfiDir() ?? "", "CaptionTextBlockStyle", secondary: true));
            items.Add(Ui.Horizontal(10, open, Again(SetupStep.BuildEfi)));
        }
        else
        {
            if (B.Building) items.Add(Note(L.Get("Setup_Efi_Building"), CheckState.Neutral));
            items.Add(open);
        }
        return StepSection(s, Glyphs.Settings, items);
    }

    // ---- 4. the stick ----
    UIElement Stick(IReadOnlyList<StepStatus> states)
    {
        var s = Of(states, SetupStep.Stick);
        var items = new List<UIElement> { Ui.Caption(L.Get("Setup_Stick_Caption"), L.Get("Setup_Stick_Detail")) };
        var open = Ui.Button(L.Get("Setup_Open_Stick"), (_, _) => App.Window?.Open("stick"), accent: s.State == StepState.Next, glyph: Glyphs.Drive);
        if (s.IsDone)
        {
            items.Add(Note(L.Get(P.StickWith == SetupWizard.Manual ? "Setup_Stick_DoneManual" : "Setup_Stick_Done"), CheckState.Good));
            items.Add(Ui.Horizontal(10, open, Again(SetupStep.Stick)));
        }
        else
        {
            var manual = Ui.Button(L.Get("Setup_Stick_Manual"), (_, _) => { Save(SetupWizard.MarkStickManual(P)); Build(); });
            manual.IsEnabled = SetupWizard.CanAct(states, SetupStep.Stick);
            items.Add(Ui.Horizontal(10, open, manual));
        }
        return StepSection(s, Glyphs.Drive, items);
    }

    // ---- 5. after the install ----
    UIElement AfterInstall(IReadOnlyList<StepStatus> states)
    {
        var s = Of(states, SetupStep.AfterInstall);
        var items = new List<UIElement>
        {
            Ui.Caption(L.Get("Setup_After_Caption")),
            Ui.Text(L.Get("Setup_After_Boot")),
            Ui.Text(L.Get("Setup_After_Stage3")),
            Ui.Text(L.Get("Setup_After_Stage4")),
        };
        if (B.Result is { Ok: true, AfterInstall.Count: > 0 } r)
        {
            items.Add(Ui.Text(L.Get("Setup_After_FromBuild"), "BodyStrongTextBlockStyle"));
            foreach (var line in r.AfterInstall) items.Add(Ui.Text("• " + line, "CaptionTextBlockStyle", secondary: true));
        }
        var buttons = Ui.Horizontal(10,
            Ui.Button(L.Get("Setup_Open_Efi"), (_, _) => App.Window?.Open("efi"), glyph: Glyphs.Puzzle),
            Ui.Link(L.Get("Setup_Open_Guide"), new Uri(Project.InstallGuideUrl)));
        if (s.IsDone) items.Add(Note(L.Format("Setup_After_Done", When(P.AfterInstallDone)), CheckState.Good));
        else
        {
            var done = Ui.Button(L.Get("Setup_After_Mark"), (_, _) => { Save(SetupWizard.MarkAfterInstall(P, DateTime.Now)); Build(); }, glyph: Glyphs.Ok);
            done.IsEnabled = SetupWizard.CanAct(states, SetupStep.AfterInstall);
            buttons.Children.Add(done);
        }
        items.Add(buttons);
        return StepSection(s, Glyphs.Heart, items);
    }

    /// "Do this step again": its record and every later one's go; nothing on disk changes.
    Button Again(SetupStep step) => Ui.Button(L.Get("Setup_Again"), (_, _) => { Save(SetupWizard.Reopen(P, step)); Build(); }, glyph: Glyphs.Undo);

    async Task StartOver()
    {
        var d = new ContentDialog
        {
            Title = L.Get("Setup_StartOver"), Content = Ui.Text(L.Get("Setup_StartOver_Detail")), XamlRoot = XamlRoot, RequestedTheme = Ui.Root.ActualTheme,
            PrimaryButtonText = L.Get("Setup_StartOver"), CloseButtonText = L.Get("Efi_Cancel"), DefaultButton = ContentDialogButton.Close,
        };
        if (await d.ShowAsync() != ContentDialogResult.Primary) return;
        Save(SetupWizard.Reopen(P, SetupStep.ThisPc));
        Build();
    }

    static string When(string iso) =>
        DateTime.TryParse(iso, CultureInfo.InvariantCulture, DateTimeStyles.None, out var t) ? t.ToString("g", CultureInfo.CurrentUICulture) : iso;

    static UIElement Note(string text, CheckState state)
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
}
