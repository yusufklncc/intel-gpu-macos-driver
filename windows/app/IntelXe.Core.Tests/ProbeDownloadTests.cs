// SPDX-License-Identifier: MIT
// The release signature check (Release/Ed25519.cs, ReleaseSigning.cs) and the probe stick image's download
// (ProbeStick/ProbeDownload.cs) against a fake GitHub. Fixtures/probe-download/ (made by its make.sh): GitHub's list of
// releases in the API's shape, a release's list of hashes signed by a test key with release/signing.swift (CryptoKit),
// and an image zipped by tools/probe-stick/zip-image.py as release/package.sh zips the real one.
using System.IO.Compression;
using System.Net;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json.Nodes;
using IntelXe.Core.ProbeStick;
using IntelXe.Core.Release;

namespace IntelXe.Core.Tests;

public class ReleaseSigningTests
{
    static byte[] Hex(string s) => Convert.FromHexString(s);

    // RFC 8032, section 7.1, tests 1 to 3 (checked with CryptoKit too)
    [Theory]
    [InlineData("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a", "",
        "e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b")]
    [InlineData("3d4017c3e843895a92b70aa74d1b7ebc9c982ccf2ec4968cc0cd55f12af4660c", "72",
        "92a009a9f0d4cab8720e820b5f642540a2b27b5416503f8fb3762223ebdb69da085ac1e43e15996e458f3613d0f11d8c387b2eaeb4302aeeb00d291612bb0c00")]
    [InlineData("fc51cd8e6218a1a38da47ed00230f0580816ed13ba3303ac5deb911548908025", "af82",
        "6291d657deec24024827e69c3abe01a30ce548a284743a445e3680d7db5ac3ac18ff9b538d16f290ae67f760984dc6594a7c15e9716ed28dc027beceea1ec40a")]
    public void VerifiesRfc8032(string key, string message, string signature)
    {
        Assert.True(Ed25519.Verify(Hex(key), Hex(message), Hex(signature)));
        var m = Hex(message + "00");
        Assert.False(Ed25519.Verify(Hex(key), m, Hex(signature)));   // another message
        for (int i = 0; i < 64; i += 21)
        {
            var s = Hex(signature);
            s[i] ^= 4;
            Assert.False(Ed25519.Verify(Hex(key), Hex(message), s));
        }
        var k = Hex(key);
        k[5] ^= 1;
        Assert.False(Ed25519.Verify(k, Hex(message), Hex(signature)));
    }

    [Fact]
    public void RefusesMalformedInput()
    {
        var key = Hex("d75a980182b10ab7d54bfed3c964073a0ee172f3daa62325af021a68f707511a");
        var sig = Hex("e5564300c360ac729086e2cc806e828a84877f1eb8e5d974d873e065224901555fb8821590a33bacc61e39701cf9b46bd25bf5f0595bbe24655141438e7a100b");
        Assert.False(Ed25519.Verify(key[..31], [], sig));
        Assert.False(Ed25519.Verify(key, [], sig[..63]));
        // S + L: the same point, a non-canonical S, refused (as CryptoKit and BoringSSL do)
        var l = System.Numerics.BigInteger.Pow(2, 252) + System.Numerics.BigInteger.Parse("27742317777372353535851937790883648493");
        var s = new System.Numerics.BigInteger(sig[32..], isUnsigned: true) + l;
        var big = (byte[])sig.Clone();
        s.TryWriteBytes(big.AsSpan(32), out _, isUnsigned: true);
        Assert.False(Ed25519.Verify(key, [], big));
        // a y not below p
        var bad = Enumerable.Repeat((byte)0xFF, 32).ToArray();
        bad[31] = 0x7F;
        Assert.False(Ed25519.Verify(bad, [], sig));
    }

    /// The macOS app's self-test signature (app/Sources/SelfTest.swift): made by CryptoKit with the project's key.
    [Fact]
    public void TheProjectsKeyVerifiesTheMacAppsSelfTestSignature()
    {
        var list = Encoding.UTF8.GetBytes("intel-gpu-macos-driver selftest list\n");
        const string good = "sG8t9iThA+B3qjfc6FM68/zDaUgkU52zyozwZ4xBTq5Nw2avuL5YtbEQD1wIDhAXDVedCT9g7qvxI0VfhND6BQ==";
        Assert.Contains(good, File.ReadAllText(Paths.RepoFile("app", "Sources", "SelfTest.swift")));
        Assert.True(ReleaseSigning.SignatureValid(list, good + "\n"));
        Assert.False(ReleaseSigning.SignatureValid(Encoding.UTF8.GetBytes("intel-gpu-macos-driver selftest lisT\n"), good));
        Assert.False(ReleaseSigning.SignatureValid(list, "A" + good[1..]));
        Assert.False(ReleaseSigning.SignatureValid(list, "not base64"));
        Assert.False(ReleaseSigning.SignatureValid(list, ""));
        // a .sigs file: one line per key, one valid line is enough
        Assert.True(ReleaseSigning.SignatureValid(list, "A" + good[1..] + "\n" + good + "\r\n"));
        Assert.False(ReleaseSigning.SignatureValid(list, "A" + good[1..] + "\nnot base64\n"));
        // only the keys given
        Assert.False(ReleaseSigning.SignatureValid(list, good, [File.ReadAllText(Fixture("test-key.txt")).Trim()]));
    }

    [Fact]
    public void TheKeysAreTheMacAppsKeys()
    {
        var swift = File.ReadAllLines(Paths.RepoFile("app", "Sources", "Signing.swift")).Single(l => l.StartsWith("let releaseSigningKeys = [", StringComparison.Ordinal));
        var keys = swift[(swift.IndexOf('[') + 1)..swift.LastIndexOf(']')].Split(',').Select(k => k.Trim().Trim('"')).ToList();
        Assert.Equal(keys, ReleaseSigning.Keys);
    }

    [Fact]
    public void ReadsAHashList()
    {
        var h = new string('a', 64);
        var list = $"{new string('0', 64)}  other.zip\n{h.ToUpperInvariant()} *x.img.zip\r\n{new string('1', 64)}  x.img.zip.more\n";
        Assert.Equal(h, ReleaseSigning.ListedHash(list, "x.img.zip"));
        Assert.Null(ReleaseSigning.ListedHash(list, "x.img"));
        Assert.Null(ReleaseSigning.ListedHash("abc  x.img.zip\n", "x.img.zip"));
    }

    internal static string Fixture(string name) => Paths.RepoFile("windows", "app", "IntelXe.Core.Tests", "Fixtures", "probe-download", name);
}

public class ProbeDownloadTests
{
    const string Api = "https://api.github.com/repos/yusufklncc/intel-gpu-macos-driver/releases?per_page=30";
    const string Base = "https://github.com/yusufklncc/intel-gpu-macos-driver/releases/download/v0.1.5/";
    const string ZipName = "xe-probe-stick-0.1.0.img.zip";
    const string ListName = "intel-gpu-macos-driver-0.1.5.sha256";

    static string Fixture(string name) => ReleaseSigningTests.Fixture(name);
    static IReadOnlyList<string> TestKeys => [File.ReadAllText(Fixture("test-key.txt")).Trim()];
    static byte[] ZipBytes => File.ReadAllBytes(Fixture(ZipName));

    /// GitHub as the downloader sees it: the release list, and the assets at their download addresses (with Range).
    sealed class FakeGitHub : HttpMessageHandler
    {
        public readonly Dictionary<string, byte[]> Files = new();
        public readonly List<(string Url, string? Range)> Requests = [];
        public bool HonourRange = true;
        public HttpStatusCode ApiStatus = HttpStatusCode.OK;
        /// Called with the bytes of the zip sent so far; the test cancels from it.
        public Action<long>? OnBytes;

        public FakeGitHub(string releases = "releases.json")
        {
            Files[Api] = File.ReadAllBytes(Fixture(releases));
            foreach (var f in new[] { ZipName, ListName, ListName + ".sig" }) Files[Base + f] = File.ReadAllBytes(Fixture(f));
        }

        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage r, CancellationToken cancel)
        {
            var url = r.RequestUri!.AbsoluteUri;
            Requests.Add((url, r.Headers.Range?.ToString()));
            Assert.Contains("IntelXe-Manager", r.Headers.UserAgent.ToString());
            if (url == Api && ApiStatus != HttpStatusCode.OK) return Task.FromResult(new HttpResponseMessage(ApiStatus));
            if (!Files.TryGetValue(url, out var bytes)) return Task.FromResult(new HttpResponseMessage(HttpStatusCode.NotFound));
            long from = 0;
            var resp = new HttpResponseMessage(HttpStatusCode.OK);
            if (HonourRange && r.Headers.Range is { } range && range.Ranges.First().From is long f)
            {
                if (f >= bytes.Length) return Task.FromResult(new HttpResponseMessage(HttpStatusCode.RequestedRangeNotSatisfiable));
                from = f;
                resp.StatusCode = HttpStatusCode.PartialContent;
            }
            resp.Content = new StreamContent(new Trickle(bytes, (int)from, url.EndsWith(ZipName, StringComparison.Ordinal) ? OnBytes : null));
            resp.Content.Headers.ContentLength = bytes.Length - from;
            if (resp.StatusCode == HttpStatusCode.PartialContent) resp.Content.Headers.ContentRange = new ContentRangeHeaderValue(from, bytes.Length - 1, bytes.Length);
            return Task.FromResult(resp);
        }

        public int Count(string name) => Requests.Count(q => q.Url.EndsWith("/" + name, StringComparison.Ordinal));
    }

    /// A body that comes in pieces of 1000 bytes, telling the test how far it got.
    sealed class Trickle(byte[] data, int from, Action<long>? onBytes) : Stream
    {
        readonly int start = from;
        int pos = from;
        public override int Read(byte[] buffer, int offset, int count)
        {
            int n = Math.Min(Math.Min(count, 1000), data.Length - pos);
            Array.Copy(data, pos, buffer, offset, n);
            pos += n;
            if (n > 0) onBytes?.Invoke(pos);
            return n;
        }
        public override Task<int> ReadAsync(byte[] buffer, int offset, int count, CancellationToken cancel)
        {
            cancel.ThrowIfCancellationRequested();
            return Task.FromResult(Read(buffer, offset, count));
        }
        public override ValueTask<int> ReadAsync(Memory<byte> buffer, CancellationToken cancel = default)
        {
            cancel.ThrowIfCancellationRequested();
            var tmp = new byte[buffer.Length];
            int n = Read(tmp, 0, tmp.Length);
            tmp.AsSpan(0, n).CopyTo(buffer.Span);
            return ValueTask.FromResult(n);
        }
        public override bool CanRead => true;
        public override bool CanSeek => false;
        public override bool CanWrite => false;
        public override long Length => data.Length - start;
        public override long Position { get => pos - start; set => throw new NotSupportedException(); }
        public override void Flush() { }
        public override long Seek(long offset, SeekOrigin origin) => throw new NotSupportedException();
        public override void SetLength(long value) => throw new NotSupportedException();
        public override void Write(byte[] buffer, int offset, int count) => throw new NotSupportedException();
    }

    sealed class SyncProgress(Action<ProbeDownloadProgress> a) : IProgress<ProbeDownloadProgress>
    {
        public void Report(ProbeDownloadProgress value) => a(value);
    }

    sealed class Folder : IDisposable
    {
        public readonly string Path = Directory.CreateTempSubdirectory("ixe-probedl-").FullName;
        public string File(string name) => System.IO.Path.Combine(Path, name);
        public void Dispose() => Directory.Delete(Path, true);
    }

    static string Sha(byte[] b) => Convert.ToHexStringLower(SHA256.HashData(b));

    [Fact]
    public async Task FindsTheNewestPublishedReleaseWithTheImage()
    {
        var fake = new FakeGitHub();
        using var d = new ProbeDownloader(fake, TestKeys);
        var r = await d.FindAsync();
        Assert.NotNull(r);
        // the draft (v0.1.7) and the pre-release (v0.1.6) have newer images, and are passed over
        Assert.Equal("v0.1.5", r!.Release.Tag);
        Assert.Equal(ZipName, r.Zip.Name);
        Assert.Equal("0.1.0", r.StickVersion);
        Assert.Equal("xe-probe-stick-0.1.0.img", r.ImageName);
        Assert.Equal(ListName, r.Release.HashList?.Name);
        Assert.Equal(ListName + ".sig", r.Release.HashListSignature?.Name);
        Assert.Equal([Api], fake.Requests.Select(q => q.Url));
    }

    [Fact]
    public async Task SaysWhenNoReleaseHasTheImage()
    {
        using var d = new ProbeDownloader(new FakeGitHub("releases-none.json"), TestKeys);
        Assert.Null(await d.FindAsync());
    }

    [Fact]
    public async Task GitHubsRefusalIsAnError()
    {
        using var d = new ProbeDownloader(new FakeGitHub { ApiStatus = HttpStatusCode.Forbidden }, TestKeys);
        var e = await Assert.ThrowsAsync<ProbeException>(() => d.FindAsync());
        Assert.Contains("HTTP 403", e.Message);
        Assert.Contains("60 requests an hour", e.Message);
    }

    [Fact]
    public void OrdersReleasesByVersion()
    {
        GitHubRelease R(string tag, params string[] assets) =>
            new(tag, null, false, false, [.. assets.Select(a => new GitHubAsset(a, "https://example.invalid/" + a, 1))]);
        var list = new[]
        {
            R("v0.1.9", "xe-probe-stick-0.1.0.img.zip"),
            R("v0.1.10", "xe-probe-stick-0.1.1.img.zip", "xe-probe-stick-0.1.10.img.zip", "xe-probe-stick-0.1.2.img"),
            R("v0.2.0", "xe-probe-stick-0.2.0.img", "xe-probe-stick-0.2.0.img.sha256"),   // not zipped: not one
            R("latest", "xe-probe-stick-9.0.0.img.zip"),
        };
        var r = ProbeDownloader.Newest(list);
        Assert.Equal("v0.1.10", r?.Release.Tag);
        Assert.Equal("0.1.10", r?.StickVersion);
        Assert.Null(ProbeDownloader.Newest([R("v0.1.4", "intel-gpu-macos-driver-0.1.4-efi.zip")]));
        Assert.Equal("", ProbeDownloader.StickVersionOf("xe-probe-stick-0.1.0.img.zip.sha256"));
    }

    [Fact]
    public async Task DownloadsChecksAndUnpacks()
    {
        var fake = new FakeGitHub();
        using var dir = new Folder();
        using var d = new ProbeDownloader(fake, TestKeys);
        var r = (await d.FindAsync())!;
        var stages = new List<string>();
        var image = await d.DownloadAsync(r, dir.Path, new SyncProgress(p => stages.Add(p.Stage)));
        Assert.Equal(dir.File("xe-probe-stick-0.1.0.img"), image);
        // what the page then checks, as any image beside the app
        var info = ProbeImage.Verify(image);
        Assert.Equal("0.1.0", info.Version);
        Assert.Equal(2 << 20, info.Size);
        Assert.Equal([image], ProbeImage.Find([dir.Path]));
        Assert.True(File.Exists(dir.File(ZipName)));
        Assert.Empty(Directory.GetFiles(dir.Path, "*.part*"));
        // the list and its signature first, then the zip, once
        Assert.Equal([Api, Base + ListName, Base + ListName + ".sig", Base + ZipName], fake.Requests.Select(q => q.Url));
        Assert.Equal(["list", "download", "unpack", "done"], stages.Distinct());

        // again: the zip and the image are there with their hashes, nothing is downloaded or unpacked
        var written = File.GetLastWriteTimeUtc(image);
        var again = new FakeGitHub();
        using var d2 = new ProbeDownloader(again, TestKeys);
        stages.Clear();
        Assert.Equal(image, await d2.DownloadAsync(r, dir.Path, new SyncProgress(p => stages.Add(p.Stage))));
        Assert.Equal(0, again.Count(ZipName));
        Assert.DoesNotContain("unpack", stages);
        Assert.Equal(written, File.GetLastWriteTimeUtc(image));
    }

    [Fact]
    public async Task RefusesASignatureByAnotherKey()
    {
        var fake = new FakeGitHub();
        using var dir = new Folder();
        using var d = new ProbeDownloader(fake);   // the project's key: the fixture is signed by the test key
        var r = (await d.FindAsync())!;
        var e = await Assert.ThrowsAsync<ProbeException>(() => d.DownloadAsync(r, dir.Path));
        Assert.Contains("signature does not match the project's key", e.Message);
        Assert.Equal(0, fake.Count(ZipName));
        Assert.Empty(Directory.GetFiles(dir.Path));
    }

    [Fact]
    public async Task RefusesAChangedList()
    {
        var fake = new FakeGitHub();
        var list = Encoding.UTF8.GetString(fake.Files[Base + ListName]);
        // the zip's line pointing at another file: the signature no longer covers the list
        fake.Files[Base + ListName] = Encoding.UTF8.GetBytes(list.Replace(Sha(ZipBytes), new string('0', 64)));
        using var dir = new Folder();
        using var d = new ProbeDownloader(fake, TestKeys);
        var e = await Assert.ThrowsAsync<ProbeException>(async () => await d.DownloadAsync((await d.FindAsync())!, dir.Path));
        Assert.Contains("signature does not match", e.Message);
        Assert.Equal(0, fake.Count(ZipName));
    }

    [Fact]
    public async Task RefusesAnUnsignedRelease()
    {
        var json = JsonNode.Parse(File.ReadAllText(Fixture("releases.json")))!.AsArray();
        foreach (var rel in json)
        {
            var assets = rel!["assets"]!.AsArray();
            foreach (var a in assets.Where(a => a!["name"]!.GetValue<string>().EndsWith(".sig", StringComparison.Ordinal)).ToList()) assets.Remove(a);
        }
        var fake = new FakeGitHub();
        fake.Files[Api] = Encoding.UTF8.GetBytes(json.ToJsonString());
        using var dir = new Folder();
        using var d = new ProbeDownloader(fake, TestKeys);
        var e = await Assert.ThrowsAsync<ProbeException>(async () => await d.DownloadAsync((await d.FindAsync())!, dir.Path));
        Assert.Contains("not signed", e.Message);
        Assert.Equal(0, fake.Count(ZipName));
    }

    [Fact]
    public async Task ADamagedDownloadLeavesNothing()
    {
        var fake = new FakeGitHub();
        var z = ZipBytes;
        z[z.Length / 2] ^= 1;
        fake.Files[Base + ZipName] = z;
        using var dir = new Folder();
        using var d = new ProbeDownloader(fake, TestKeys);
        var e = await Assert.ThrowsAsync<ProbeException>(async () => await d.DownloadAsync((await d.FindAsync())!, dir.Path));
        Assert.Contains("does not match the release's signed list of hashes", e.Message);
        Assert.Empty(Directory.GetFiles(dir.Path));
    }

    [Fact]
    public async Task ACancelledDownloadContinuesWhereItStopped()
    {
        var fake = new FakeGitHub();
        using var dir = new Folder();
        using var cancel = new CancellationTokenSource();
        fake.OnBytes = n => { if (n >= 3000) cancel.Cancel(); };
        using (var d = new ProbeDownloader(fake, TestKeys))
        {
            var r = (await d.FindAsync())!;
            await Assert.ThrowsAnyAsync<OperationCanceledException>(() => d.DownloadAsync(r, dir.Path, null, cancel.Token));
        }
        var part = dir.File(ZipName + ".part");
        Assert.True(File.Exists(part));
        long have = new FileInfo(part).Length;
        Assert.InRange(have, 1000, ZipBytes.Length - 1);   // what was read after the cancel is not written
        Assert.False(File.Exists(dir.File(ZipName)));
        Assert.False(File.Exists(dir.File("xe-probe-stick-0.1.0.img")));

        var again = new FakeGitHub();
        using var d2 = new ProbeDownloader(again, TestKeys);
        var image = await d2.DownloadAsync((await d2.FindAsync())!, dir.Path);
        Assert.Equal($"bytes={have}-", again.Requests.Single(q => q.Url.EndsWith(ZipName, StringComparison.Ordinal)).Range);
        Assert.Equal(ZipBytes, File.ReadAllBytes(dir.File(ZipName)));
        ProbeImage.Verify(image);
        Assert.Empty(Directory.GetFiles(dir.Path, "*.part*"));
    }

    [Fact]
    public async Task AServerWithoutRangesSendsItAllAgain()
    {
        using var dir = new Folder();
        File.WriteAllBytes(dir.File(ZipName + ".part"), ZipBytes[..2500]);
        File.WriteAllText(dir.File(ZipName + ".part.sha256"), Sha(ZipBytes) + "\n");
        var fake = new FakeGitHub { HonourRange = false };
        using var d = new ProbeDownloader(fake, TestKeys);
        var image = await d.DownloadAsync((await d.FindAsync())!, dir.Path);
        Assert.Equal("bytes=2500-", fake.Requests.Single(q => q.Url.EndsWith(ZipName, StringComparison.Ordinal)).Range);
        Assert.Equal(ZipBytes, File.ReadAllBytes(dir.File(ZipName)));
        ProbeImage.Verify(image);
    }

    [Fact]
    public async Task ABadPartIsDownloadedAgainOnce()
    {
        using var dir = new Folder();
        var bad = ZipBytes[..2500];
        bad[100] ^= 1;
        File.WriteAllBytes(dir.File(ZipName + ".part"), bad);
        File.WriteAllText(dir.File(ZipName + ".part.sha256"), Sha(ZipBytes) + "\n");
        var fake = new FakeGitHub();
        using var d = new ProbeDownloader(fake, TestKeys);
        var image = await d.DownloadAsync((await d.FindAsync())!, dir.Path);
        var zipRequests = fake.Requests.Where(q => q.Url.EndsWith(ZipName, StringComparison.Ordinal)).Select(q => q.Range).ToList();
        Assert.Equal(["bytes=2500-", null], zipRequests);
        ProbeImage.Verify(image);
    }

    [Fact]
    public async Task APartOfAnotherFileIsNotContinued()
    {
        using var dir = new Folder();
        File.WriteAllBytes(dir.File(ZipName + ".part"), new byte[2500]);
        File.WriteAllText(dir.File(ZipName + ".part.sha256"), new string('0', 64) + "\n");
        var fake = new FakeGitHub();
        using var d = new ProbeDownloader(fake, TestKeys);
        ProbeImage.Verify(await d.DownloadAsync((await d.FindAsync())!, dir.Path));
        Assert.Null(fake.Requests.Single(q => q.Url.EndsWith(ZipName, StringComparison.Ordinal)).Range);
    }

    [Fact]
    public async Task AWholePartIsOnlyChecked()
    {
        using var dir = new Folder();
        File.WriteAllBytes(dir.File(ZipName + ".part"), ZipBytes);
        File.WriteAllText(dir.File(ZipName + ".part.sha256"), Sha(ZipBytes) + "\n");
        var fake = new FakeGitHub();
        using var d = new ProbeDownloader(fake, TestKeys);
        ProbeImage.Verify(await d.DownloadAsync((await d.FindAsync())!, dir.Path));
        Assert.Equal(0, fake.Count(ZipName));
    }

    // ---- the zip's contents (after its hash matched the signed list) ----

    static string MakeZip(string dir, Action<ZipArchive, byte[], string> fill)
    {
        var src = Path.Combine(dir, "src");
        Directory.CreateDirectory(src);
        var img = ProbeImageTests.MakeImage(src);
        var zip = Path.Combine(dir, ZipName);
        using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            fill(z, File.ReadAllBytes(img), File.ReadAllText(img + ".sha256"));
        return zip;
    }

    static void Add(ZipArchive z, string name, byte[] data)
    {
        using var s = z.CreateEntry(name).Open();
        s.Write(data);
    }

    [Theory]
    [InlineData("extra", "not xe-probe-stick-0.1.0.img and")]
    [InlineData("folder", "not xe-probe-stick-0.1.0.img and")]
    [InlineData("nosha", "not xe-probe-stick-0.1.0.img and")]
    [InlineData("othername", "holds no SHA-256 for")]
    [InlineData("changed", "does not match its .img.sha256")]
    [InlineData("layout", "labelled")]
    public void RefusesAZipThatIsNotTheStick(string what, string message)
    {
        using var dir = new Folder();
        const string img = "xe-probe-stick-0.1.0.img";
        var zip = MakeZip(dir.Path, (z, bytes, sha) =>
        {
            if (what == "changed") bytes[3_000_000] ^= 1;
            if (what == "layout")
            {
                Encoding.ASCII.GetBytes("MACOS      ").CopyTo(bytes, 2048 * 512 + 71);
                sha = Sha(bytes) + "  " + img + "\n";
            }
            if (what == "othername") sha = sha.Replace(img, "other.img");
            Add(z, what == "folder" ? "x/" + img : img, bytes);
            if (what != "nosha") Add(z, img + ".sha256", Encoding.UTF8.GetBytes(sha));
            if (what == "extra") Add(z, "autorun.inf", [1]);
        });
        var out_ = Path.Combine(dir.Path, "out");
        Directory.CreateDirectory(out_);
        var e = Assert.Throws<ProbeException>(() => ProbeDownloader.Unpack(zip, out_, img));
        Assert.Contains(message, e.Message);
        Assert.Empty(Directory.GetFiles(out_));
    }

    [Fact]
    public void ReplacesADamagedImage()
    {
        using var dir = new Folder();
        const string img = "xe-probe-stick-0.1.0.img";
        var zip = MakeZip(dir.Path, (z, bytes, sha) => { Add(z, img, bytes); Add(z, img + ".sha256", Encoding.UTF8.GetBytes(sha)); });
        var out_ = Path.Combine(dir.Path, "out");
        Directory.CreateDirectory(out_);
        var path = ProbeDownloader.Unpack(zip, out_, img);
        var good = File.ReadAllBytes(path);
        var b = (byte[])good.Clone();
        b[2_000_000] ^= 1;
        File.WriteAllBytes(path, b);
        Assert.Throws<ProbeException>(() => ProbeImage.Verify(path));
        ProbeDownloader.Unpack(zip, out_, img);
        Assert.Equal(good, File.ReadAllBytes(path));
        ProbeImage.Verify(path);
    }
}
