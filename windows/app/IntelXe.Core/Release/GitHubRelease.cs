// SPDX-License-Identifier: MIT
// The project's releases as GitHub's REST API lists them (GET /repos/OWNER/REPO/releases): the fields the app reads, as
// the macOS app's Release and ReleaseAsset do (app/Sources/Model.swift). Unknown fields are ignored.
using System.Text.Json;
using System.Text.Json.Serialization;

namespace IntelXe.Core.Release;

public sealed record GitHubAsset(
    [property: JsonPropertyName("name")] string Name,
    [property: JsonPropertyName("browser_download_url")] string Url,
    [property: JsonPropertyName("size")] long Size,
    [property: JsonPropertyName("state")] string? State = null);

public sealed record GitHubRelease(
    [property: JsonPropertyName("tag_name")] string Tag,
    [property: JsonPropertyName("html_url")] string? HtmlUrl,
    [property: JsonPropertyName("draft")] bool Draft,
    [property: JsonPropertyName("prerelease")] bool Prerelease,
    [property: JsonPropertyName("assets")] IReadOnlyList<GitHubAsset> Assets)
{
    /// The API's address of the newest releases of the project (one page; the newest come first).
    public static Uri ListUrl(string repository = Project.Repository, int perPage = 30) =>
        new($"https://api.github.com/repos/{repository}/releases?per_page={perPage}");

    /// "0.1.5" from the tag v0.1.5; null for a tag that is not vX.Y.Z (as the macOS app's Version).
    public System.Version? Version =>
        Tag.StartsWith('v') && System.Version.TryParse(Tag[1..], out var v) && v.Build >= 0 && v.Revision < 0 ? v : null;

    /// The version as the release's file names carry it ("0.1.5").
    public string VersionText => Version?.ToString(3) ?? "";

    /// An uploaded asset of exactly this name, or null.
    public GitHubAsset? Asset(string name) =>
        Assets.FirstOrDefault(a => a.Name == name && (a.State is null or "uploaded"));

    /// The release's list of hashes, intel-gpu-macos-driver-VERSION.sha256.
    public GitHubAsset? HashList => Asset($"intel-gpu-macos-driver-{VersionText}.sha256");

    /// Its signature: .sha256.sigs (one line per key, during a key change) when the release has it, else .sha256.sig,
    /// as the macOS app reads them.
    public GitHubAsset? HashListSignature => Asset($"intel-gpu-macos-driver-{VersionText}.sha256.sigs") ?? Asset($"intel-gpu-macos-driver-{VersionText}.sha256.sig");

    static readonly JsonSerializerOptions Options = new() { PropertyNameCaseInsensitive = false };

    /// The API's answer: a JSON array of releases. Throws JsonException on anything else.
    public static IReadOnlyList<GitHubRelease> ParseList(string json) =>
        JsonSerializer.Deserialize<List<GitHubRelease>>(json, Options) is { } list
            ? [.. list.Where(r => r is { Tag: not null, Assets: not null })]
            : throw new JsonException("not a list of releases");
}
