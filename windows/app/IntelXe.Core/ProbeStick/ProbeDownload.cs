// SPDX-License-Identifier: MIT
// Downloading the Xe probe stick's image from the project's GitHub releases, checked as the macOS app checks a package
// (app/Sources/Model.swift download(), Release/ReleaseSigning.cs):
//
//   1. the newest release (by its vX.Y.Z tag; no drafts, no pre-releases) that has an asset xe-probe-stick-V.img.zip;
//   2. that release's list of hashes, intel-gpu-macos-driver-X.Y.Z.sha256, and its signature (.sha256.sigs, else
//      .sha256.sig) must verify with a key built into the app, and name the zip: otherwise nothing is downloaded;
//   3. the zip goes to a .part file (resumed with an HTTP Range request after a cancel or a broken connection, the
//      expected hash kept beside it so a .part of another file is never continued) and becomes the zip only when its
//      SHA-256 is the signed list's;
//   4. the zip holds exactly xe-probe-stick-V.img and its .img.sha256 (release/package.sh makes it so); the image is
//      unpacked to a .part file, hashed as it is written, and kept only when it matches that .img.sha256 and has the
//      stick's layout (ProbeImage.CheckLayout). The page then checks it as any other image (ProbeImage.Verify).
//
// The image ships zipped: it is about 208 MB, mostly free FAT space, and 38 MB zipped; Windows opens a .zip without
// anything else. The release lists only the zip (signed); its .img.sha256 rides inside it. Everything goes to
// %LOCALAPPDATA%\IntelXe Manager\probe-stick\, one of the folders the page looks in (ProbeImage.Folders), and is reused
// while its hash matches.
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using IntelXe.Core.Release;

namespace IntelXe.Core.ProbeStick;

/// The release to take the image from: its zip asset and the stick's version ("0.1.0").
public sealed record ProbeRelease(GitHubRelease Release, GitHubAsset Zip, string StickVersion)
{
    public string ImageName => $"xe-probe-stick-{StickVersion}.img";
}

/// Stage: "ask" (the release list), "list" (the signed list of hashes), "download", "check" (hashing a file already
/// there), "unpack", "done". Bytes and Total count the stage's bytes (Total 0 when unknown).
public sealed record ProbeDownloadProgress(string Stage, long Bytes, long Total);

public sealed partial class ProbeDownloader : IDisposable
{
    [GeneratedRegex(@"^xe-probe-stick-(\d+(?:\.\d+){0,3})\.img\.zip$", RegexOptions.CultureInvariant)]
    private static partial Regex ZipRe();

    const string UserAgent = "IntelXe-Manager";
    /// The zip and the image inside it are at most this large (the image is about 208 MB).
    const long MaxBytes = ProbeImage.MaxSize;

    readonly HttpClient http;
    readonly IReadOnlyList<string>? keys;
    readonly string repository;

    /// handler: a fake in the tests; keys: the accepted signing keys (ReleaseSigning.Keys when null).
    public ProbeDownloader(HttpMessageHandler? handler = null, IReadOnlyList<string>? keys = null, string repository = Project.Repository)
    {
        http = new HttpClient(handler ?? new SocketsHttpHandler { UseCookies = false, AutomaticDecompression = DecompressionMethods.None })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
        http.DefaultRequestHeaders.UserAgent.ParseAdd(UserAgent);
        this.keys = keys;
        this.repository = repository;
    }

    public void Dispose() => http.Dispose();

    /// %LOCALAPPDATA%\IntelXe Manager\probe-stick: where downloads go (the last of ProbeImage.Folders).
    public static string Folder(string localAppData) => Path.Combine(localAppData, "IntelXe Manager", "probe-stick");

    /// "0.1.0" from xe-probe-stick-0.1.0.img.zip; "" for another name.
    public static string StickVersionOf(string zipName) => ZipRe().Match(zipName) is { Success: true } m ? m.Groups[1].Value : "";

    /// The newest release that ships the image, or null when none does (no release has published it yet).
    public static ProbeRelease? Newest(IEnumerable<GitHubRelease> releases)
    {
        foreach (var r in releases.Where(r => !r.Draft && !r.Prerelease && r.Version is not null).OrderByDescending(r => r.Version))
        {
            var zip = r.Assets
                .Where(a => a.State is null or "uploaded")
                .Select(a => (Asset: a, Version: StickVersionOf(a.Name)))
                .Where(a => a.Version.Length > 0)
                .OrderByDescending(a => System.Version.TryParse(a.Version.Contains('.') ? a.Version : a.Version + ".0", out var v) ? v : new System.Version(0, 0))
                .FirstOrDefault();
            if (zip.Asset is not null) return new ProbeRelease(r, zip.Asset, zip.Version);
        }
        return null;
    }

    /// Asks GitHub for the project's releases; the newest that ships the image, or null.
    public async Task<ProbeRelease?> FindAsync(CancellationToken cancel = default)
    {
        using var req = new HttpRequestMessage(HttpMethod.Get, GitHubRelease.ListUrl(repository));
        req.Headers.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        var json = Encoding.UTF8.GetString(await GetSmall(req, 8 << 20, cancel).ConfigureAwait(false));
        try { return Newest(GitHubRelease.ParseList(json)); }
        catch (System.Text.Json.JsonException e) { throw new ProbeException("GitHub's list of releases could not be read: " + e.Message); }
    }

    /// Downloads, checks and unpacks the release's image into folder; returns the image's path (its .img.sha256 beside
    /// it). Throws ProbeException with the reason when anything does not match; OperationCanceledException when
    /// cancelled (the .part file stays, and the next download continues it).
    public async Task<string> DownloadAsync(ProbeRelease release, string folder, IProgress<ProbeDownloadProgress>? progress = null, CancellationToken cancel = default)
    {
        var r = release.Release;
        progress?.Report(new ProbeDownloadProgress("list", 0, 0));
        var listAsset = r.HashList ?? throw new ProbeException($"release {r.Tag} has no list of hashes (intel-gpu-macos-driver-{r.VersionText}.sha256): nothing was downloaded");
        var sigAsset = r.HashListSignature
            ?? throw new ProbeException($"release {r.Tag}'s list of hashes is not signed, so the app cannot tell that the project made it: nothing was downloaded. Download the image from the release page and choose it if you trust it.");
        var list = await GetSmall(Get(listAsset.Url), 1 << 20, cancel).ConfigureAwait(false);
        var sig = await GetSmall(Get(sigAsset.Url), 64 << 10, cancel).ConfigureAwait(false);
        if (!ReleaseSigning.SignatureValid(list, Encoding.UTF8.GetString(sig), keys))
            throw new ProbeException($"release {r.Tag}'s signature does not match the project's key: its files were not made by the project, or were changed. Nothing was downloaded.");
        var want = ReleaseSigning.ListedHash(Encoding.UTF8.GetString(list), release.Zip.Name)
            ?? throw new ProbeException($"{release.Zip.Name} is not in release {r.Tag}'s signed list of hashes: nothing was downloaded");

        Directory.CreateDirectory(folder);
        var zip = Path.Combine(folder, release.Zip.Name);
        if (!(File.Exists(zip) && await HashFile(zip, progress, cancel).ConfigureAwait(false) == want))
            await DownloadZip(release.Zip, zip, want, progress, cancel).ConfigureAwait(false);
        var image = await Task.Run(() => Unpack(zip, folder, release.ImageName, progress, cancel), cancel).ConfigureAwait(false);
        progress?.Report(new ProbeDownloadProgress("done", 0, 0));
        return image;
    }

    static HttpRequestMessage Get(string url)
    {
        if (!Uri.TryCreate(url, UriKind.Absolute, out var u) || u.Scheme != Uri.UriSchemeHttps) throw new ProbeException($"not an https address: {url}");
        return new HttpRequestMessage(HttpMethod.Get, u);
    }

    async Task<HttpResponseMessage> Send(HttpRequestMessage req, HttpCompletionOption opt, CancellationToken cancel)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        if (opt == HttpCompletionOption.ResponseContentRead) timeout.CancelAfter(TimeSpan.FromSeconds(60));
        try { return await http.SendAsync(req, opt, timeout.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new ProbeException($"{req.RequestUri?.Host} did not answer within 60 s"); }
        catch (HttpRequestException e) { throw new ProbeException($"{req.RequestUri?.Host}: {e.Message}"); }
    }

    static ProbeException HttpError(HttpRequestMessage req, HttpResponseMessage resp)
    {
        var code = (int)resp.StatusCode;
        var why = code == 403 && req.RequestUri?.Host == "api.github.com" ? " (GitHub allows 60 requests an hour without an account: try again later)" : "";
        return new ProbeException($"{req.RequestUri?.Host} answered HTTP {code}{why}");
    }

    /// A small file (the release list, the list of hashes, its signature), at most max bytes.
    async Task<byte[]> GetSmall(HttpRequestMessage req, int max, CancellationToken cancel)
    {
        using (req)
        using (var resp = await Send(req, HttpCompletionOption.ResponseContentRead, cancel).ConfigureAwait(false))
        {
            if (!resp.IsSuccessStatusCode) throw HttpError(req, resp);
            var b = await resp.Content.ReadAsByteArrayAsync(cancel).ConfigureAwait(false);
            if (b.Length > max) throw new ProbeException($"{Path.GetFileName(req.RequestUri!.AbsolutePath)} is larger than {max} bytes: refused");
            return b;
        }
    }

    /// The zip into zip.part, continued when a .part for the same expected hash is there, then moved into place when its
    /// SHA-256 is want. A continued .part that does not end up matching is thrown away and the download done once more
    /// from the start.
    async Task DownloadZip(GitHubAsset asset, string zip, string want, IProgress<ProbeDownloadProgress>? progress, CancellationToken cancel)
    {
        var part = zip + ".part";
        var wantFile = part + ".sha256";
        // a .part is continued only when it was started for this very file
        if (File.Exists(part) && (!File.Exists(wantFile) || File.ReadAllText(wantFile).Trim() != want)) File.Delete(part);
        bool resumed = File.Exists(part) && new FileInfo(part).Length > 0;
        if (!File.Exists(part)) File.WriteAllText(wantFile, want + "\n");
        var got = await Fetch(asset, part, progress, cancel).ConfigureAwait(false);
        if (got != want && resumed)
        {
            File.Delete(part);
            got = await Fetch(asset, part, progress, cancel).ConfigureAwait(false);
        }
        if (got != want)
        {
            TryDelete(part);
            TryDelete(wantFile);
            throw new ProbeException($"{asset.Name} does not match the release's signed list of hashes (SHA-256 {got[..12]}…, expected {want[..12]}…): it was damaged or replaced, and was deleted. Try again.");
        }
        File.Move(part, zip, overwrite: true);
        TryDelete(wantFile);
    }

    /// Fetches the asset into part, continuing what part already holds when the server takes the Range; returns the
    /// SHA-256 of the whole part.
    async Task<string> Fetch(GitHubAsset asset, string part, IProgress<ProbeDownloadProgress>? progress, CancellationToken cancel)
    {
        using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        long have = File.Exists(part) ? new FileInfo(part).Length : 0;
        if (have >= MaxBytes) { File.Delete(part); have = 0; }
        long total = asset.Size > 0 ? asset.Size : 0;
        if (have > 0 && total > 0 && have >= total)
        {
            // already whole (cancelled after the last byte): only its hash is left to see
            await using var f = new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            return Convert.ToHexStringLower(await SHA256.HashDataAsync(f, cancel).ConfigureAwait(false));
        }

        using var req = Get(asset.Url);
        if (have > 0) req.Headers.Range = new RangeHeaderValue(have, null);
        using var resp = await Send(req, HttpCompletionOption.ResponseHeadersRead, cancel).ConfigureAwait(false);
        bool append = false;
        if (resp.StatusCode == HttpStatusCode.PartialContent && have > 0 && resp.Content.Headers.ContentRange is { From: long from } && from == have)
            append = true;
        else if (resp.StatusCode == HttpStatusCode.RequestedRangeNotSatisfiable && have > 0)
        {
            // the server has nothing after what part holds: part is longer than the file, or already whole
            await using var f = new FileStream(part, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            return Convert.ToHexStringLower(await SHA256.HashDataAsync(f, cancel).ConfigureAwait(false));
        }
        else if (resp.StatusCode != HttpStatusCode.OK) throw HttpError(req, resp);

        await using var dst = new FileStream(part, append ? FileMode.Open : FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20);
        long done = 0;
        if (append)
        {
            var buf0 = new byte[1 << 20];
            int n0;
            while ((n0 = await dst.ReadAsync(buf0, cancel).ConfigureAwait(false)) > 0) { hash.AppendData(buf0, 0, n0); done += n0; }
            dst.Position = done;
        }
        else if (resp.Content.Headers.ContentLength is long len) total = len;
        if (total > MaxBytes) throw new ProbeException($"{asset.Name} is {total} bytes: not a probe stick image");
        progress?.Report(new ProbeDownloadProgress("download", done, total));
        await using var src = await resp.Content.ReadAsStreamAsync(cancel).ConfigureAwait(false);
        var buf = new byte[1 << 20];
        long last = done;
        int n;
        while ((n = await ReadWithTimeout(src, buf, cancel).ConfigureAwait(false)) > 0)
        {
            if (done + n > MaxBytes) { await dst.DisposeAsync().ConfigureAwait(false); File.Delete(part); throw new ProbeException($"{asset.Name} is larger than {MaxBytes} bytes: not a probe stick image"); }
            hash.AppendData(buf, 0, n);
            await dst.WriteAsync(buf.AsMemory(0, n), cancel).ConfigureAwait(false);
            done += n;
            if (done - last >= 1 << 20) { last = done; progress?.Report(new ProbeDownloadProgress("download", done, total)); }
        }
        await dst.FlushAsync(cancel).ConfigureAwait(false);
        progress?.Report(new ProbeDownloadProgress("download", done, Math.Max(total, done)));
        return Convert.ToHexStringLower(hash.GetHashAndReset());
    }

    /// A read that gives up after 60 s without data (a stalled connection), not after a fixed total time.
    static async Task<int> ReadWithTimeout(Stream s, byte[] buf, CancellationToken cancel)
    {
        using var t = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        t.CancelAfter(TimeSpan.FromSeconds(60));
        try { return await s.ReadAsync(buf, t.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new ProbeException("the download stalled for 60 s; Download again continues it"); }
        catch (IOException e) when (!cancel.IsCancellationRequested) { throw new ProbeException($"the download broke off ({e.Message}); Download again continues it"); }
    }

    static async Task<string> HashFile(string path, IProgress<ProbeDownloadProgress>? progress, CancellationToken cancel)
    {
        progress?.Report(new ProbeDownloadProgress("check", 0, 0));
        await using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexStringLower(await SHA256.HashDataAsync(f, cancel).ConfigureAwait(false));
    }

    /// The image out of the checked zip: exactly the image and its .img.sha256, the image hashed as it is written and
    /// kept only when it matches. An image already there with that hash is kept as it is.
    public static string Unpack(string zip, string folder, string imageName, IProgress<ProbeDownloadProgress>? progress = null, CancellationToken cancel = default)
    {
        var shaName = imageName + ".sha256";
        using var archive = ZipFile.OpenRead(zip);
        var names = archive.Entries.Select(e => e.FullName).Order(StringComparer.Ordinal).ToList();
        if (!names.SequenceEqual([imageName, shaName], StringComparer.Ordinal))
            throw new ProbeException($"{Path.GetFileName(zip)} holds {string.Join(", ", names)}, not {imageName} and {shaName}");
        var shaEntry = archive.GetEntry(shaName)!;
        var imgEntry = archive.GetEntry(imageName)!;
        if (shaEntry.Length > 4096) throw new ProbeException($"{shaName} is not a SHA-256 file");
        if (imgEntry.Length > MaxBytes) throw new ProbeException($"{imageName} is {imgEntry.Length} bytes: not a probe stick image");
        string shaText;
        using (var s = shaEntry.Open()) using (var reader = new StreamReader(s)) shaText = reader.ReadToEnd();
        var fields = shaText.Split((char[]?)null, 3, StringSplitOptions.RemoveEmptyEntries);
        if (fields.Length < 2 || fields[0].Length != 64 || !fields[0].All(Uri.IsHexDigit) || fields[1].TrimStart('*') != imageName)
            throw new ProbeException($"{shaName} in {Path.GetFileName(zip)} holds no SHA-256 for {imageName}");
        var want = fields[0].ToLowerInvariant();

        var image = Path.Combine(folder, imageName);
        var sha = image + ".sha256";
        if (File.Exists(image) && new FileInfo(image).Length == imgEntry.Length)
        {
            progress?.Report(new ProbeDownloadProgress("check", 0, 0));
            using var f = new FileStream(image, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
            if (ProbeImage.Hash(f, f.Length, null, cancel) == want)
            {
                File.WriteAllText(sha, shaText);
                return image;
            }
        }

        var part = image + ".part";
        try
        {
            using var hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
            using (var src = imgEntry.Open())
            using (var dst = new FileStream(part, FileMode.Create, FileAccess.ReadWrite, FileShare.None, 1 << 20))
            {
                var buf = new byte[4 << 20];
                long done = 0, total = imgEntry.Length;
                int n;
                while ((n = src.Read(buf, 0, buf.Length)) > 0)
                {
                    cancel.ThrowIfCancellationRequested();
                    done += n;
                    if (done > total) throw new ProbeException($"{imageName} in {Path.GetFileName(zip)} is longer than its entry says");
                    hash.AppendData(buf, 0, n);
                    dst.Write(buf, 0, n);
                    progress?.Report(new ProbeDownloadProgress("unpack", done, total));
                }
                if (Convert.ToHexStringLower(hash.GetHashAndReset()) != want)
                    throw new ProbeException($"{imageName} in {Path.GetFileName(zip)} does not match its .img.sha256");
                dst.Position = 0;
                ProbeImage.CheckLayout(dst);
                dst.Flush(true);
            }
            File.Move(part, image, overwrite: true);
            File.WriteAllText(sha, shaText);
            return image;
        }
        catch
        {
            TryDelete(part);
            throw;
        }
    }

    static void TryDelete(string path)
    {
        try { File.Delete(path); } catch (IOException) { } catch (UnauthorizedAccessException) { }
    }
}
