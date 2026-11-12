// SPDX-License-Identifier: MIT
namespace IntelXe.Core.Tests;

/// Files of the repository, found upwards from the test's output folder.
static class Paths
{
    static string Root()
    {
        for (var d = new DirectoryInfo(AppContext.BaseDirectory); d is not null; d = d.Parent)
            if (File.Exists(Path.Combine(d.FullName, "VERSION")) && Directory.Exists(Path.Combine(d.FullName, "windows", "app")))
                return d.FullName;
        throw new DirectoryNotFoundException("the repository root (VERSION, windows/app) above " + AppContext.BaseDirectory);
    }

    public static string RepoFile(params string[] parts) => Path.Combine([Root(), .. parts]);
    public static string AppFile(params string[] parts) => Path.Combine([Root(), "windows", "app", "IntelXe.App", .. parts]);
}
