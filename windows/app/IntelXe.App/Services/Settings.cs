// SPDX-License-Identifier: MIT
using System.Text.Json;

namespace IntelXe.App;

/// The app's own two choices, kept in %LOCALAPPDATA%\IntelXe Manager\settings.json (nothing in the registry).
public sealed class Settings
{
    /// "" for the language of Windows, "en-US" or "tr-TR"
    public string Language { get; set; } = "";
    /// "" for the system's, "light" or "dark"
    public string Theme { get; set; } = "";

    static readonly string File = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "IntelXe Manager", "settings.json");

    public static Settings Current { get; } = Load();

    static Settings Load()
    {
        try { return JsonSerializer.Deserialize<Settings>(System.IO.File.ReadAllText(File)) ?? new Settings(); }
        catch (Exception) { return new Settings(); }
    }

    public void Save()
    {
        try
        {
            Directory.CreateDirectory(Path.GetDirectoryName(File)!);
            System.IO.File.WriteAllText(File, JsonSerializer.Serialize(this));
        }
        catch (Exception) { }
    }
}
