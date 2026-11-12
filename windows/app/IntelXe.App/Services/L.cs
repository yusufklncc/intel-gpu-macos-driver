// SPDX-License-Identifier: MIT
using System.Globalization;
using IntelXe.Core;
using Microsoft.Windows.ApplicationModel.Resources;

namespace IntelXe.App;

/// The app's strings, from Strings/<language>/Resources.resw (resources.pri beside the exe), in the language chosen on
/// the About page: English or Turkish, or the one of Windows. The Turkish texts are the macOS app's wherever the same
/// text appears there (app/Resources/tr.lproj).
public static class L
{
    static readonly ResourceManager? manager;
    static readonly ResourceMap? map;
    static ResourceContext? context;

    public static string Language { get; private set; } = "en-US";

    static L()
    {
        try
        {
            manager = new ResourceManager();
            map = manager.MainResourceMap.GetSubtree("Resources");
        }
        catch (Exception) { manager = null; map = null; }
    }

    /// "en-US", "tr-TR", or null / "" for the language of Windows (Turkish when Windows prefers it, else English).
    public static void SetLanguage(string? language)
    {
        if (string.IsNullOrEmpty(language))
            language = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName == "tr" ? "tr-TR" : "en-US";
        Language = language == "tr-TR" ? "tr-TR" : "en-US";
        var culture = new CultureInfo(Language);
        CultureInfo.DefaultThreadCurrentUICulture = culture;
        CultureInfo.CurrentUICulture = culture;
        if (manager is null) return;
        context = manager.CreateResourceContext();
        context.QualifierValues["Language"] = Language;
    }

    public static string Get(string key)
    {
        try
        {
            var value = (context is null ? map?.GetValue(key) : map?.GetValue(key, context))?.ValueAsString;
            if (!string.IsNullOrEmpty(value)) return value;
        }
        catch (Exception) { }
        return key;
    }

    public static string Format(string key, params object[] args) =>
        string.Format(CultureInfo.CurrentUICulture, Get(key), args.Select(a => a is Text t ? Resolve(t) : a).ToArray());

    /// A Core text in the language shown.
    public static string Resolve(Text text) =>
        text.Key is null ? text.Verbatim ?? "" : text.Args.Length == 0 ? Get(text.Key) : Format(text.Key, text.Args);
}
