// SPDX-License-Identifier: MIT
using IntelXe.Core;
using IntelXe.Core.Efi;
using IntelXe.Core.Localisation;

namespace IntelXe.Core.Tests;

public class LocalisationTests
{
    static string Strings(string language) => Paths.AppFile("Strings", language, "Resources.resw");

    [Fact]
    public void EnglishAndTurkishHaveTheSameKeysAndPlaceholders()
    {
        var en = ReswFile.Read(Strings("en-US"));
        var tr = ReswFile.Read(Strings("tr-TR"));
        Assert.NotEmpty(en);
        Assert.Empty(ReswFile.Compare(en, "en-US", tr, "tr-TR"));
    }

    [Fact]
    public void EveryKeyTheAppUsesExists()
    {
        var en = ReswFile.Read(Strings("en-US"));
        var app = Paths.AppFile();
        var files = Directory.EnumerateFiles(app, "*.*", SearchOption.AllDirectories)
            .Where(f => (f.EndsWith(".cs") || f.EndsWith(".xaml")) && !f.Contains($"{Path.DirectorySeparatorChar}obj{Path.DirectorySeparatorChar}")
                        && !f.Contains($"{Path.DirectorySeparatorChar}bin{Path.DirectorySeparatorChar}"));
        var used = ReswFile.KeysUsedIn(files);
        Assert.NotEmpty(used);
        Assert.Empty(used.Where(k => !en.ContainsKey(k)).Order());
        Assert.Empty(PcChecks.Keys.Where(k => !en.ContainsKey(k)).ToList());
        Assert.True(en.ContainsKey(PendingEfiPlanner.MessageKey));
    }

    [Fact]
    public void TheTurkishTableIsTranslated()
    {
        var en = ReswFile.Read(Strings("en-US"));
        var tr = ReswFile.Read(Strings("tr-TR"));
        // names, numbers and formats may stay as they are; most text must not
        var same = en.Keys.Count(k => tr.TryGetValue(k, out var t) && t == en[k]);
        Assert.True(same < en.Count / 4, $"{same} of {en.Count} Turkish strings are the English ones");
    }

    [Fact]
    public void ComparesPlaceholders()
    {
        Assert.Equal([0, 1], ReswFile.Placeholders("{1} of {0}, {0}"));
        var a = new Dictionary<string, string> { ["A"] = "x {0}", ["B"] = "y" };
        var b = new Dictionary<string, string> { ["A"] = "x", ["C"] = "z" };
        Assert.Equal(3, ReswFile.Compare(a, "a", b, "b").Count);
    }
}
