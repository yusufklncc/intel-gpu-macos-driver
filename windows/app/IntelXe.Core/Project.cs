// SPDX-License-Identifier: MIT
namespace IntelXe.Core;

/// Where the project lives: the same repository as the macOS app (app/Sources/Model.swift `repository`).
public static class Project
{
    public const string Repository = "yusufklncc/intel-gpu-macos-driver";
    public const string RepositoryUrl = "https://github.com/" + Repository;
    public const string SiteUrl = "https://yusufklncc.github.io/intel-gpu-macos-driver/";
    public const string GuideUrl = SiteUrl + "guide/";
    public const string WindowsGuideUrl = SiteUrl + "guide/windows.html";
    public const string HardwareGuideUrl = SiteUrl + "guide/hardware.html";
    public const string ReportGuideUrl = SiteUrl + "guide/report.html";
    public const string InstallGuideUrl = RepositoryUrl + "/blob/main/release/README-INSTALL.md";
    public const string ReleasesUrl = RepositoryUrl + "/releases";
    public const string LicensingUrl = RepositoryUrl + "/blob/main/docs/LICENSING.md";

    /// An issue form with one field filled in. GitHub fills a form field from a query parameter named after the field's
    /// id (.github/ISSUE_TEMPLATE: 1-bug.yml has "report", 2-new-hardware.yml "reports"), as the macOS Report page does.
    public static Uri IssueFormUrl(string template, string field, string? value)
    {
        var url = RepositoryUrl + "/issues/new?template=" + Uri.EscapeDataString(template);
        if (!string.IsNullOrEmpty(value)) url += "&" + Uri.EscapeDataString(field) + "=" + Uri.EscapeDataString(value);
        return new Uri(url);
    }

    public static Uri BugFormUrl(string? reportId) => IssueFormUrl("1-bug.yml", "report", reportId);
    public static Uri NewGpuFormUrl(string? reportId) => IssueFormUrl("2-new-hardware.yml", "reports", reportId);
}
