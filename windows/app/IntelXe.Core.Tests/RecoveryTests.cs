// SPDX-License-Identifier: MIT
// The recovery protocol and the chunklist. Fixtures/recovery/ holds what Apple's recovery server answered on
// 2026-11-12 for the two board IDs (the asset tokens replaced by REDACTED-ASSET-TOKEN) and the two real chunklists it
// pointed to (BaseSystem.chunklist of macOS 26 and 15: Apple's signature over the image's piece hashes). The images
// themselves are too big for the repository: the download path is tested with images made here, signed with a key made
// here, and with a fake HTTP server.
using System.Net;
using System.Security.Cryptography;
using System.Text;
using IntelXe.Core.Stick;

namespace IntelXe.Core.Tests;

public class RecoveryTests
{
    static string Fixture(string name) => Paths.RepoFile("windows", "app", "IntelXe.Core.Tests", "Fixtures", "recovery", name);

    // ---- the versions ----

    [Fact]
    public void TheVersionsAreTahoeFirstThenSequoia()
    {
        var d = RecoveryData.Embedded;
        Assert.Equal(["tahoe", "sequoia"], d.Versions.Select(v => v.Id));
        Assert.Equal("00000000000000000", d.Mlb);
        Assert.Equal(RecoveryProtocol.ZeroMlb, d.Mlb);
        var tahoe = d.Find("tahoe");
        Assert.True(tahoe.Primary);
        Assert.Equal(("Mac-CFF7D910A743CAAF", "latest", 26), (tahoe.Board, tahoe.Os, tahoe.Major));   // macrecovery recovery_urls.txt
        var sequoia = d.Find("sequoia");
        Assert.False(sequoia.Primary);
        Assert.Equal(("Mac-7BA5B2D9E42DDD94", "default", 15), (sequoia.Board, sequoia.Os, sequoia.Major));
        Assert.StartsWith("26.", tahoe.BoardNewest);   // boards.json: the newest macOS the board gets
        Assert.StartsWith("15.", sequoia.BoardNewest);
        foreach (var v in d.Versions)
        {
            Assert.True(v.Status.En.Length > 0 && v.Status.Tr.Length > 0);
            Assert.True(File.Exists(Paths.RepoFile("site", "guide", Path.GetFileName(new Uri(v.Guide).AbsolutePath))), v.Guide);
        }
    }

    // ---- the requests ----

    [Fact]
    public void BuildsTheImageRequestAsMacrecoveryDoes()
    {
        int calls = 0;
        string Hex(int n) => new string((char)('A' + calls++), n);
        var body = RecoveryProtocol.ImageInfoBody("Mac-CFF7D910A743CAAF", "latest", Hex);
        Assert.Equal("cid=AAAAAAAAAAAAAAAA\nsn=00000000000000000\nbid=Mac-CFF7D910A743CAAF\nk=" + new string('B', 64) + "\nfg=" + new string('C', 64) + "\nos=latest", body);
        var r = RecoveryProtocol.ImageInfoRequest("session=ABC", body);
        Assert.Equal(HttpMethod.Post, r.Method);
        Assert.Equal("http://osrecovery.apple.com/InstallationPayload/RecoveryImage", r.RequestUri!.AbsoluteUri);
        Assert.Equal("text/plain", r.Content!.Headers.ContentType!.ToString());
        Assert.Equal("session=ABC", string.Join(";", r.Headers.GetValues("Cookie")));
        Assert.Equal("InternetRecovery/1.0", string.Join(" ", r.Headers.GetValues("User-Agent")));
        Assert.True(r.Headers.ConnectionClose);
        var real = RecoveryProtocol.ImageInfoBody("Mac-7BA5B2D9E42DDD94", "default");
        Assert.Matches(@"^cid=[0-9A-F]{16}\nsn=0{17}\nbid=Mac-7BA5B2D9E42DDD94\nk=[0-9A-F]{64}\nfg=[0-9A-F]{64}\nos=default$", real);
    }

    [Theory]
    [InlineData("Mac-XYZ", "latest")]
    [InlineData("Mac-CFF7D910A743CAAF\nsn=F5K105303J9K3F71M", "latest")]
    [InlineData("Mac-CFF7D910A743CAAF", "newest")]
    public void RefusesOtherBoardsAndOsTypes(string board, string os) =>
        Assert.Throws<RecoveryException>(() => RecoveryProtocol.ImageInfoBody(board, os));

    [Fact]
    public void ReadsTheSessionCookie()
    {
        Assert.Equal("session=abc123", RecoveryProtocol.ParseSession(["session=abc123; Domain=osrecovery.apple.com; Path=/; HttpOnly"]));
        Assert.Equal("session=x", RecoveryProtocol.ParseSession(["other=1; Path=/", "Path=/; session=x"]));
        Assert.Throws<RecoveryException>(() => RecoveryProtocol.ParseSession(["other=1"]));
        Assert.Throws<RecoveryException>(() => RecoveryProtocol.ParseSession([]));
    }

    [Theory]
    [InlineData("tahoe", "140-93589")]
    [InlineData("sequoia", "696-28424")]
    public void ParsesTheRecordedAnswers(string version, string product)
    {
        var info = RecoveryProtocol.ParseImageInfo(File.ReadAllText(Fixture(version + "-answer.txt")));
        Assert.Equal(product, info.Product);
        Assert.Equal("BaseSystem.dmg", info.ImageName);
        Assert.Equal("BaseSystem.chunklist", info.ChunklistName);
        Assert.Equal("oscdn.apple.com", info.ImageUrl.Host);
        Assert.Equal("REDACTED-ASSET-TOKEN", info.ImageToken);
        var a = RecoveryProtocol.AssetRequest(info.ChunklistUrl, info.ChunklistToken);
        Assert.Equal("AssetToken=REDACTED-ASSET-TOKEN", string.Join(";", a.Headers.GetValues("Cookie")));
    }

    [Theory]
    [InlineData("AP: 1\nAU: http://oscdn.apple.com/a/BaseSystem.dmg\nAT: t\nCU: http://oscdn.apple.com/a/BaseSystem.chunklist\n")]   // no CT
    [InlineData("AP: 1\nAU: http://evil.example/a/BaseSystem.dmg\nAT: t\nCU: http://oscdn.apple.com/a/BaseSystem.chunklist\nCT: t")]
    [InlineData("AP: 1\nAU: http://oscdn.apple.com.evil.example/BaseSystem.dmg\nAT: t\nCU: http://oscdn.apple.com/a/BaseSystem.chunklist\nCT: t")]
    [InlineData("AP: 1\nAU: file:///C:/x/BaseSystem.dmg\nAT: t\nCU: http://oscdn.apple.com/a/BaseSystem.chunklist\nCT: t")]
    [InlineData("AP: 1\nAU: http://oscdn.apple.com/a/..%5C..%5Cevil.dmg\nAT: t\nCU: http://oscdn.apple.com/a/BaseSystem.chunklist\nCT: t")]
    [InlineData("AP: 1\nAU: http://oscdn.apple.com/a/BaseSystem.dmg\nAT: t\nCU: http://oscdn.apple.com/a/Other.chunklist\nCT: t")]
    [InlineData("<html>403 Forbidden</html>")]
    public void RefusesBadAnswers(string text) => Assert.Throws<RecoveryException>(() => RecoveryProtocol.ParseImageInfo(text));

    // ---- the chunklist ----

    [Theory]
    [InlineData("tahoe", 92, 960_530_321L)]
    [InlineData("sequoia", 85, 884_317_790L)]
    public void ApplesRealChunklistsVerify(string version, int chunks, long size)
    {
        var list = Chunklist.Parse(File.ReadAllBytes(Fixture(version + "-BaseSystem.chunklist")));
        Assert.Equal(chunks, list.Chunks.Count);
        Assert.Equal(size, list.ImageSize);
        Assert.Equal(1, list.SignatureMethod);
        Assert.All(list.Chunks.SkipLast(1), c => Assert.Equal(10u << 20, c.Size));
    }

    [Fact]
    public void AChangedByteBreaksApplesSignature()
    {
        var bytes = File.ReadAllBytes(Fixture("tahoe-BaseSystem.chunklist"));
        foreach (int at in new[] { 0x30, 0x24 + 36 * 50 + 10, bytes.Length - 1 })
        {
            var copy = (byte[])bytes.Clone();
            copy[at] ^= 0x01;
            Assert.Throws<ChunklistException>(() => Chunklist.Parse(copy));
        }
        Assert.Throws<ChunklistException>(() => Chunklist.Parse(bytes.AsSpan(0, bytes.Length - 1)));
        Assert.Throws<ChunklistException>(() => Chunklist.Parse([.. bytes, 0]));
        // signed by another key
        using var other = RSA.Create(2048);
        Assert.Throws<ChunklistException>(() => Chunklist.Parse(bytes, other.ExportParameters(false)));
    }

    static (byte[] Image, byte[] List, RSA Key) Signed(int size = 300_000, int chunk = 65536, byte method = 1)
    {
        var image = new byte[size];
        new Random(7).NextBytes(image);
        var key = RSA.Create(2048);
        return (image, Chunklist.Build(image, chunk, key, method), key);
    }

    [Fact]
    public void RefusesAnUnsignedChunklist()
    {
        var (_, list, key) = Signed(method: 2);
        var e = Assert.Throws<ChunklistException>(() => Chunklist.Parse(list, key.ExportParameters(false)));
        Assert.Contains("not signed", e.Message);
    }

    [Fact]
    public void ChecksEveryPiece()
    {
        var (image, listBytes, key) = Signed();
        var list = Chunklist.Parse(listBytes, key.ExportParameters(false));
        Assert.Equal(5, list.Chunks.Count);
        Assert.Equal(image.Length, list.ImageSize);

        // fed in odd sizes: fine
        var v = new Chunklist.Verifier(list);
        for (int o = 0; o < image.Length; o += 7777) v.Feed(image.AsSpan(o, Math.Min(7777, image.Length - o)));
        v.Finish();
        Assert.Equal(5, v.ChunksVerified);

        // one changed byte in piece 3: refused at that piece
        var bad = (byte[])image.Clone();
        bad[65536 * 2 + 100] ^= 0xFF;
        var v2 = new Chunklist.Verifier(list);
        var e = Assert.Throws<ChunklistException>(() => v2.Feed(bad));
        Assert.Contains("chunk 3 of 5", e.Message);

        // short and long images
        var v3 = new Chunklist.Verifier(list);
        v3.Feed(image.AsSpan(0, image.Length - 1));
        Assert.Throws<ChunklistException>(v3.Finish);
        var v4 = new Chunklist.Verifier(list);
        Assert.Throws<ChunklistException>(() => v4.Feed([.. image, 0]));
    }

    [Fact]
    public void ChecksAFile()
    {
        var (image, listBytes, key) = Signed();
        var list = Chunklist.Parse(listBytes, key.ExportParameters(false));
        var dir = Directory.CreateTempSubdirectory("ixe-cl-").FullName;
        try
        {
            var path = Path.Combine(dir, "BaseSystem.dmg");
            File.WriteAllBytes(path, image);
            long seen = 0;
            list.VerifyFile(path, b => seen = b);
            Assert.Equal(image.Length, seen);
            image[^1] ^= 1;
            File.WriteAllBytes(path, image);
            Assert.Throws<ChunklistException>(() => list.VerifyFile(path));
            File.WriteAllBytes(path, image[..^1]);
            Assert.Throws<ChunklistException>(() => list.VerifyFile(path));
        }
        finally { Directory.Delete(dir, true); }
    }

    // ---- the client against a fake server ----

    sealed class FakeApple(byte[] image, byte[] list, Func<byte[], byte[]>? serveImage = null) : HttpMessageHandler
    {
        public readonly List<(HttpMethod Method, string Url, string Cookie, string Body)> Requests = [];

        protected override async Task<HttpResponseMessage> SendAsync(HttpRequestMessage r, CancellationToken cancel)
        {
            string cookie = r.Headers.TryGetValues("Cookie", out var c) ? string.Join(";", c) : "";
            string body = r.Content is null ? "" : await r.Content.ReadAsStringAsync(cancel);
            Requests.Add((r.Method, r.RequestUri!.AbsoluteUri, cookie, body));
            Assert.Equal("InternetRecovery/1.0", string.Join(" ", r.Headers.GetValues("User-Agent")));
            var url = r.RequestUri.AbsoluteUri;
            if (url == "http://osrecovery.apple.com/")
            {
                var resp = new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent([]) };
                resp.Headers.Add("Set-Cookie", "session=S1; Domain=osrecovery.apple.com; Path=/; HttpOnly");
                return resp;
            }
            if (url.EndsWith("/RecoveryImage", StringComparison.Ordinal))
            {
                if (cookie != "session=S1") return new HttpResponseMessage(HttpStatusCode.Forbidden);
                return new HttpResponseMessage(HttpStatusCode.OK)
                {
                    Content = new StringContent("AP: 000-00000\nAU: http://oscdn.apple.com/x/RecoveryImage/BaseSystem.dmg\nAH: 00\nAT: IMG\n" +
                                                "CU: http://oscdn.apple.com/x/RecoveryImage/BaseSystem.chunklist\nCH: 00\nCT: LIST\n"),
                };
            }
            if (url.EndsWith("BaseSystem.chunklist", StringComparison.Ordinal) && cookie == "AssetToken=LIST")
                return new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(list) };
            if (url.EndsWith("BaseSystem.dmg", StringComparison.Ordinal) && cookie == "AssetToken=IMG")
                return new HttpResponseMessage(HttpStatusCode.OK) { Content = new ByteArrayContent(serveImage?.Invoke(image) ?? image) };
            return new HttpResponseMessage(HttpStatusCode.NotFound);
        }
    }

    static RecoveryVersion Tahoe => RecoveryData.Embedded.Find("tahoe");

    [Fact]
    public async Task DownloadsAndChecks()
    {
        var (image, list, key) = Signed();
        var fake = new FakeApple(image, list);
        var dir = Directory.CreateTempSubdirectory("ixe-dl-").FullName;
        try
        {
            using var client = new RecoveryClient(fake, key.ExportParameters(false));
            var stages = new List<string>();
            var files = await client.DownloadAsync(Tahoe, dir, new SyncProgress(p => stages.Add(p.Stage)));
            Assert.Equal(Path.Combine(dir, "com.apple.recovery.boot", "BaseSystem.dmg"), files.Image);
            Assert.Equal(image, File.ReadAllBytes(files.Image));
            Assert.Equal(list, File.ReadAllBytes(files.ChunklistFile));
            Assert.Equal("000-00000", files.Product);
            Assert.Contains("done", stages);
            // the protocol's order: session, image info (with the zero MLB and the Tahoe board), chunklist, then image
            Assert.Equal(4, fake.Requests.Count);
            Assert.Equal(HttpMethod.Get, fake.Requests[0].Method);
            Assert.Contains("sn=00000000000000000\nbid=Mac-CFF7D910A743CAAF", fake.Requests[1].Body);
            Assert.EndsWith("\nos=latest", fake.Requests[1].Body);
            Assert.EndsWith("BaseSystem.chunklist", fake.Requests[2].Url);
            Assert.EndsWith("BaseSystem.dmg", fake.Requests[3].Url);
            // found again later, checked against the chunklist's signature and size
            var again = RecoveryFiles.Find(dir, key.ExportParameters(false));
            Assert.NotNull(again);
            Assert.Equal(image.Length, again!.Size);
            Assert.Null(RecoveryFiles.Find(dir));   // Apple's key does not verify our test list
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public async Task ABadPieceLeavesNoImage()
    {
        var (image, list, key) = Signed();
        var fake = new FakeApple(image, list, img => { var b = (byte[])img.Clone(); b[200_000] ^= 1; return b; });
        var dir = Directory.CreateTempSubdirectory("ixe-dl-").FullName;
        try
        {
            using var client = new RecoveryClient(fake, key.ExportParameters(false));
            var e = await Assert.ThrowsAsync<ChunklistException>(() => client.DownloadAsync(Tahoe, dir));
            Assert.Contains("chunk 4 of 5", e.Message);
            var boot = Path.Combine(dir, "com.apple.recovery.boot");
            Assert.False(File.Exists(Path.Combine(boot, "BaseSystem.dmg")));
            Assert.False(File.Exists(Path.Combine(boot, "BaseSystem.dmg.part")));
            Assert.Null(RecoveryFiles.Find(dir, key.ExportParameters(false)));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public async Task AnUnsignedChunklistStopsBeforeTheImage()
    {
        var (image, list, key) = Signed(method: 2);
        var fake = new FakeApple(image, list);
        var dir = Directory.CreateTempSubdirectory("ixe-dl-").FullName;
        try
        {
            using var client = new RecoveryClient(fake, key.ExportParameters(false));
            await Assert.ThrowsAsync<ChunklistException>(() => client.DownloadAsync(Tahoe, dir));
            Assert.DoesNotContain(fake.Requests, r => r.Url.EndsWith(".dmg", StringComparison.Ordinal));
        }
        finally { Directory.Delete(dir, true); }
    }

    [Fact]
    public async Task AForbiddenAnswerIsAnError()
    {
        var handler = new StatusHandler(HttpStatusCode.Forbidden);
        using var client = new RecoveryClient(handler);
        var e = await Assert.ThrowsAsync<RecoveryException>(() => client.GetInfoAsync(Tahoe));
        Assert.Contains("403", e.Message);
    }

    sealed class StatusHandler(HttpStatusCode code) : HttpMessageHandler
    {
        protected override Task<HttpResponseMessage> SendAsync(HttpRequestMessage r, CancellationToken c) => Task.FromResult(new HttpResponseMessage(code));
    }

    sealed class SyncProgress(Action<RecoveryProgress> a) : IProgress<RecoveryProgress>
    {
        public void Report(RecoveryProgress value) => a(value);
    }
}
