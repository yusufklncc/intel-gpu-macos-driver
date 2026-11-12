// SPDX-License-Identifier: MIT
// Downloading macOS recovery (BaseSystem.dmg and its chunklist) from Apple's recovery server, by the protocol of
// OpenCorePkg's Utilities/macrecovery/macrecovery.py (BSD-3-Clause, Copyright (c) 2019, vit9696), written again in C#:
//
//   1. GET http://osrecovery.apple.com/ ; the answer's Set-Cookie carries "session=...".
//   2. POST http://osrecovery.apple.com/InstallationPayload/RecoveryImage with that cookie, Content-Type text/plain and
//      the lines cid=<16 hex> sn=<MLB> bid=<board ID> k=<64 hex> fg=<64 hex> os=default|latest (joined by \n). The
//      MLB is the tool's default all-zero one: no real serial number is ever sent. The answer is "KEY: value" lines:
//      AP (product), AU/AT (image URL and token), CU/CT (chunklist URL and token), AH/CH (hashes the tool ignores).
//   3. GET each URL with the cookie AssetToken=<token>.
//
// All requests carry User-Agent InternetRecovery/1.0 and Connection: close, as the tool's do. The server speaks plain
// HTTP; what makes the download trustworthy is the chunklist: it must carry Apple's signature (Chunklist.Parse), it is
// fetched and checked before the image, and every piece of the image is checked against it while it arrives. A bad
// piece stops the download and leaves no image behind.
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;
using System.Text.Json.Serialization;
using System.Text.RegularExpressions;
using IntelXe.Core.Bios;

namespace IntelXe.Core.Stick;

public sealed class RecoveryException(string message) : Exception(message);

public sealed class RecoveryVersion
{
    [JsonPropertyName("id")] public string Id { get; set; } = "";
    [JsonPropertyName("major")] public int Major { get; set; }
    [JsonPropertyName("name")] public string Name { get; set; } = "";
    [JsonPropertyName("board")] public string Board { get; set; } = "";
    [JsonPropertyName("os")] public string Os { get; set; } = "default";
    [JsonPropertyName("board_newest")] public string BoardNewest { get; set; } = "";
    [JsonPropertyName("primary")] public bool Primary { get; set; }
    [JsonPropertyName("status")] public LocalText Status { get; set; } = new();
    [JsonPropertyName("guide")] public string Guide { get; set; } = "";
}

public sealed class RecoveryData
{
    [JsonPropertyName("about")] public string About { get; set; } = "";
    [JsonPropertyName("source")] public string Source { get; set; } = "";
    [JsonPropertyName("mlb")] public string Mlb { get; set; } = "";
    [JsonPropertyName("versions")] public List<RecoveryVersion> Versions { get; set; } = [];

    static RecoveryData? embedded;

    public static RecoveryData Embedded => embedded ??= JsonSerializer.Deserialize<RecoveryData>(
        typeof(RecoveryData).Assembly.GetManifestResourceStream("IntelXe.Core.macos-recovery.json")
        ?? throw new InvalidDataException("macos-recovery.json is not built into the Core")) ?? throw new InvalidDataException("macos-recovery.json is empty");

    public RecoveryVersion Find(string id) => Versions.FirstOrDefault(v => v.Id == id) ?? throw new RecoveryException("no macOS version " + id);
}

/// What the recovery server answered for one board: the product and the two files with their tokens.
public sealed record RecoveryImageInfo(string Product, Uri ImageUrl, string ImageToken, Uri ChunklistUrl, string ChunklistToken)
{
    /// "BaseSystem.dmg" / "BaseSystem.chunklist" (the server's names, checked)
    public string ImageName => Path.GetFileName(ImageUrl.AbsolutePath);
    public string ChunklistName => Path.GetFileName(ChunklistUrl.AbsolutePath);
}

public static class RecoveryProtocol
{
    public const string Host = "osrecovery.apple.com";
    public static readonly Uri SessionUrl = new("http://osrecovery.apple.com/");
    public static readonly Uri ImageInfoUrl = new("http://osrecovery.apple.com/InstallationPayload/RecoveryImage");
    public const string UserAgent = "InternetRecovery/1.0";
    public const string ZeroMlb = "00000000000000000";

    static readonly string[] Required = ["AP", "AU", "AT", "CU", "CT"];
    static readonly Regex FileName = new(@"^[A-Za-z0-9_-]{1,64}\.(dmg|chunklist)$", RegexOptions.CultureInvariant);
    static readonly Regex Board = new(@"^Mac-[0-9A-F]{8}([0-9A-F]{8})?$", RegexOptions.CultureInvariant);

    public static string RandomHex(int length)
    {
        var bytes = RandomNumberGenerator.GetBytes((length + 1) / 2);
        return Convert.ToHexString(bytes)[..length];
    }

    static void Common(HttpRequestMessage r)
    {
        r.Headers.TryAddWithoutValidation("User-Agent", UserAgent);
        r.Headers.ConnectionClose = true;
    }

    public static HttpRequestMessage SessionRequest()
    {
        var r = new HttpRequestMessage(HttpMethod.Get, SessionUrl);
        Common(r);
        return r;
    }

    /// The "session=..." cookie from the answer's Set-Cookie headers.
    public static string ParseSession(IEnumerable<string> setCookies)
    {
        foreach (var header in setCookies)
            foreach (var part in header.Split(';', StringSplitOptions.TrimEntries))
                if (part.StartsWith("session=", StringComparison.Ordinal) && part.Length > "session=".Length) return part;
        throw new RecoveryException("Apple's recovery server gave no session");
    }

    /// The POST body, in the tool's order: cid, sn, bid, k, fg, os. The MLB is always the all-zero one.
    public static string ImageInfoBody(string board, string os, Func<int, string>? randomHex = null)
    {
        if (!Board.IsMatch(board)) throw new RecoveryException("not a board ID: " + board);
        if (os is not ("default" or "latest")) throw new RecoveryException("os is default or latest, not " + os);
        randomHex ??= RandomHex;
        return string.Join("\n",
            "cid=" + randomHex(16), "sn=" + ZeroMlb, "bid=" + board, "k=" + randomHex(64), "fg=" + randomHex(64), "os=" + os);
    }

    public static HttpRequestMessage ImageInfoRequest(string session, string body)
    {
        var r = new HttpRequestMessage(HttpMethod.Post, ImageInfoUrl) { Content = new ByteArrayContent(Encoding.ASCII.GetBytes(body)) };
        r.Content.Headers.ContentType = new MediaTypeHeaderValue("text/plain");
        r.Headers.TryAddWithoutValidation("Cookie", session);
        Common(r);
        return r;
    }

    /// The server's "KEY: value" answer. Refuses an answer without the files, URLs that are not Apple's over HTTP(S),
    /// and file names other than NAME.dmg + NAME.chunklist.
    public static RecoveryImageInfo ParseImageInfo(string text)
    {
        var info = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var raw in text.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            int i = line.IndexOf(": ", StringComparison.Ordinal);
            if (i <= 0) continue;
            info.TryAdd(line[..i], line[(i + 2)..].Trim());
        }
        foreach (var k in Required)
            if (!info.TryGetValue(k, out var v) || v.Length == 0) throw new RecoveryException($"Apple's answer has no {k}");
        var image = AppleUrl(info["AU"]);
        var chunklist = AppleUrl(info["CU"]);
        var r = new RecoveryImageInfo(info["AP"], image, info["AT"], chunklist, info["CT"]);
        if (!FileName.IsMatch(r.ImageName) || !r.ImageName.EndsWith(".dmg", StringComparison.Ordinal))
            throw new RecoveryException("unexpected image name " + r.ImageName);
        if (!FileName.IsMatch(r.ChunklistName) || Path.GetFileNameWithoutExtension(r.ChunklistName) != Path.GetFileNameWithoutExtension(r.ImageName)
            || !r.ChunklistName.EndsWith(".chunklist", StringComparison.Ordinal))
            throw new RecoveryException("unexpected chunklist name " + r.ChunklistName);
        return r;
    }

    static Uri AppleUrl(string s)
    {
        if (!Uri.TryCreate(s, UriKind.Absolute, out var u) || u.Scheme is not ("http" or "https")
            || !(u.Host == "apple.com" || u.Host.EndsWith(".apple.com", StringComparison.OrdinalIgnoreCase)))
            throw new RecoveryException("not an Apple download URL: " + s);
        return u;
    }

    public static HttpRequestMessage AssetRequest(Uri url, string token)
    {
        var r = new HttpRequestMessage(HttpMethod.Get, url);
        r.Headers.TryAddWithoutValidation("Cookie", "AssetToken=" + token);
        Common(r);
        return r;
    }
}

/// Progress of a download: the stage, the bytes so far and in all, the pieces checked so far and in all.
public sealed record RecoveryProgress(string Stage, long Bytes, long Total, int Chunks, int ChunkCount);

/// A recovery download in a folder: FOLDER\com.apple.recovery.boot\BaseSystem.dmg and .chunklist, checked.
public sealed record RecoveryFiles(string Folder, string Image, string ChunklistFile, string Product, long Size)
{
    public const string BootFolder = "com.apple.recovery.boot";
    public const string Marker = "intelxe-verified.json";

    /// The files of an earlier download in this folder, when its chunklist still verifies with Apple's key and the
    /// image has the size it says (the pieces are checked again before the stick is written); null otherwise.
    public static RecoveryFiles? Find(string folder, RSAParameters? key = null)
    {
        try
        {
            var boot = Path.Combine(folder, BootFolder);
            var marker = Path.Combine(folder, Marker);
            if (!File.Exists(marker)) return null;
            var m = JsonSerializer.Deserialize<Dictionary<string, string>>(File.ReadAllText(marker));
            if (m is null || !m.TryGetValue("image", out var imageName) || !m.TryGetValue("chunklist", out var listName)) return null;
            var image = Path.Combine(boot, Path.GetFileName(imageName));
            var list = Path.Combine(boot, Path.GetFileName(listName));
            if (!File.Exists(image) || !File.Exists(list)) return null;
            var cl = Chunklist.Parse(File.ReadAllBytes(list), key);
            if (new FileInfo(image).Length != cl.ImageSize) return null;
            return new RecoveryFiles(folder, image, list, m.GetValueOrDefault("product", ""), cl.ImageSize);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ChunklistException or JsonException) { return null; }
    }
}

/// The client: asks for a version's files and downloads them, checking every piece.
public sealed class RecoveryClient : IDisposable
{
    readonly HttpClient http;
    readonly RSAParameters? key;

    /// handler: a fake in the tests; key: the chunklist key (Apple's when null).
    public RecoveryClient(HttpMessageHandler? handler = null, RSAParameters? key = null)
    {
        http = new HttpClient(handler ?? new SocketsHttpHandler { UseCookies = false, AutomaticDecompression = DecompressionMethods.None })
        {
            Timeout = Timeout.InfiniteTimeSpan,
        };
        this.key = key;
    }

    public void Dispose() => http.Dispose();

    static async Task<HttpResponseMessage> Send(HttpClient http, HttpRequestMessage r, CancellationToken cancel, HttpCompletionOption opt = HttpCompletionOption.ResponseContentRead)
    {
        using var timeout = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        if (opt == HttpCompletionOption.ResponseContentRead) timeout.CancelAfter(TimeSpan.FromSeconds(60));
        var resp = await http.SendAsync(r, opt, timeout.Token).ConfigureAwait(false);
        if (!resp.IsSuccessStatusCode)
        {
            var code = (int)resp.StatusCode;
            resp.Dispose();
            throw new RecoveryException($"{r.RequestUri?.Host} answered HTTP {code}");
        }
        return resp;
    }

    public async Task<RecoveryImageInfo> GetInfoAsync(RecoveryVersion v, CancellationToken cancel = default)
    {
        string session;
        using (var s = await Send(http, RecoveryProtocol.SessionRequest(), cancel).ConfigureAwait(false))
            session = RecoveryProtocol.ParseSession(s.Headers.TryGetValues("Set-Cookie", out var c) ? c : []);
        using var r = await Send(http, RecoveryProtocol.ImageInfoRequest(session, RecoveryProtocol.ImageInfoBody(v.Board, v.Os)), cancel).ConfigureAwait(false);
        return RecoveryProtocol.ParseImageInfo(await r.Content.ReadAsStringAsync(cancel).ConfigureAwait(false));
    }

    /// Downloads a version into FOLDER\com.apple.recovery.boot: the chunklist first (refused unless Apple signed it),
    /// then the image, each piece checked as it arrives, into a .part file that becomes the image only when every piece
    /// matched. The marker file records the download.
    public async Task<RecoveryFiles> DownloadAsync(RecoveryVersion v, string folder, IProgress<RecoveryProgress>? progress = null, CancellationToken cancel = default)
    {
        progress?.Report(new RecoveryProgress("ask", 0, 0, 0, 0));
        var info = await GetInfoAsync(v, cancel).ConfigureAwait(false);
        var boot = Path.Combine(folder, RecoveryFiles.BootFolder);
        Directory.CreateDirectory(boot);
        var marker = Path.Combine(folder, RecoveryFiles.Marker);
        File.Delete(marker);

        progress?.Report(new RecoveryProgress("chunklist", 0, 0, 0, 0));
        byte[] listBytes;
        using (var r = await Send(http, RecoveryProtocol.AssetRequest(info.ChunklistUrl, info.ChunklistToken), cancel).ConfigureAwait(false))
            listBytes = await r.Content.ReadAsByteArrayAsync(cancel).ConfigureAwait(false);
        if (listBytes.Length > 4 << 20) throw new RecoveryException("the chunklist is too large");
        var list = Chunklist.Parse(listBytes, key);
        var listPath = Path.Combine(boot, info.ChunklistName);
        await File.WriteAllBytesAsync(listPath, listBytes, cancel).ConfigureAwait(false);

        var image = Path.Combine(boot, info.ImageName);
        var part = image + ".part";
        File.Delete(image);
        var verifier = new Chunklist.Verifier(list);
        try
        {
            using var r = await Send(http, RecoveryProtocol.AssetRequest(info.ImageUrl, info.ImageToken), cancel, HttpCompletionOption.ResponseHeadersRead).ConfigureAwait(false);
            if (r.Content.Headers.ContentLength is long len && len != list.ImageSize)
                throw new RecoveryException($"the image is {len} bytes, the chunklist says {list.ImageSize}: refused");
            await using (var src = await r.Content.ReadAsStreamAsync(cancel).ConfigureAwait(false))
            await using (var dst = new FileStream(part, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20))
            {
                var buf = new byte[1 << 20];
                long last = 0;
                int n;
                while ((n = await ReadWithTimeout(src, buf, cancel).ConfigureAwait(false)) > 0)
                {
                    verifier.Feed(buf.AsSpan(0, n));
                    await dst.WriteAsync(buf.AsMemory(0, n), cancel).ConfigureAwait(false);
                    if (verifier.Bytes - last >= 4 << 20 || verifier.Bytes == list.ImageSize)
                    {
                        last = verifier.Bytes;
                        progress?.Report(new RecoveryProgress("image", verifier.Bytes, list.ImageSize, verifier.ChunksVerified, verifier.ChunkCount));
                    }
                }
                verifier.Finish();
                await dst.FlushAsync(cancel).ConfigureAwait(false);
            }
            File.Move(part, image, overwrite: true);
        }
        catch
        {
            try { File.Delete(part); } catch (Exception) { }
            throw;
        }
        var m = new Dictionary<string, string>
        {
            ["version"] = v.Id, ["board"] = v.Board, ["product"] = info.Product, ["image"] = info.ImageName, ["chunklist"] = info.ChunklistName,
            ["size"] = list.ImageSize.ToString(System.Globalization.CultureInfo.InvariantCulture), ["chunks"] = list.Chunks.Count.ToString(System.Globalization.CultureInfo.InvariantCulture),
            ["date"] = DateTime.UtcNow.ToString("u", System.Globalization.CultureInfo.InvariantCulture),
        };
        await File.WriteAllTextAsync(marker, JsonSerializer.Serialize(m, new JsonSerializerOptions { WriteIndented = true }), cancel).ConfigureAwait(false);
        progress?.Report(new RecoveryProgress("done", list.ImageSize, list.ImageSize, list.Chunks.Count, list.Chunks.Count));
        return new RecoveryFiles(folder, image, listPath, info.Product, list.ImageSize);
    }

    /// A read that gives up after 60 s without data (a stalled connection), not after a fixed total time.
    static async Task<int> ReadWithTimeout(Stream s, byte[] buf, CancellationToken cancel)
    {
        using var t = CancellationTokenSource.CreateLinkedTokenSource(cancel);
        t.CancelAfter(TimeSpan.FromSeconds(60));
        try { return await s.ReadAsync(buf, t.Token).ConfigureAwait(false); }
        catch (OperationCanceledException) when (!cancel.IsCancellationRequested) { throw new RecoveryException("the download stalled for 60 s"); }
    }
}
