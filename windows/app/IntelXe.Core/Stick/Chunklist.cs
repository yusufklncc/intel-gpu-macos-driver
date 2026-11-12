// SPDX-License-Identifier: MIT
// Apple's chunklist: the list of SHA-256 hashes of a recovery image's pieces, signed with Apple's EFI ROM key, as
// OpenCorePkg's Utilities/macrecovery/macrecovery.py reads it (BSD-3-Clause, Copyright (c) 2019, vit9696; this is our
// own C# from its description of the format, not a copy):
//
//   header (0x24 bytes, little endian): "CNKL", header size 0x24, file version 1, chunk method 1, signature method
//   (1 = RSA-2048 signature, 2 = a bare SHA-256 without a signature), a pad byte, chunk count, chunk offset (0x24) and
//   signature offset (0x24 + 0x24 * count);
//   chunks (0x24 bytes each): the piece's size (uint32) and its SHA-256;
//   signature: 256 bytes, an RSA PKCS#1 v1.5 SHA-256 signature of the header and the chunks, stored little endian.
//
// A chunklist without a signature (method 2) is refused, as macrecovery.py does ("Chunklist missing digital
// signature"): its hashes could come from anyone. Every piece of the image is checked against its hash, and the image
// must end where the last piece ends.
using System.Buffers.Binary;
using System.Security.Cryptography;

namespace IntelXe.Core.Stick;

public sealed class ChunklistException(string message) : Exception(message);

public sealed record Chunk(uint Size, byte[] Sha256);

public sealed class Chunklist
{
    public const int HeaderSize = 0x24, ChunkSize = 0x24, SignatureSize = 256;
    public IReadOnlyList<Chunk> Chunks { get; }
    /// the SHA-256 of the header and the chunks, which the signature covers
    public byte[] Digest { get; }
    public int SignatureMethod { get; }

    /// The image's size: the sum of its pieces.
    public long ImageSize => Chunks.Sum(c => (long)c.Size);

    Chunklist(IReadOnlyList<Chunk> chunks, byte[] digest, int method)
    {
        Chunks = chunks;
        Digest = digest;
        SignatureMethod = method;
    }

    /// Apple's EFI ROM public key 1 (RSA-2048, exponent 65537), the key macrecovery.py checks recovery chunklists with.
    public static readonly RSAParameters AppleKey = new()
    {
        Modulus = Convert.FromHexString(
            "C3E748CAD9CD384329E10E25A91E43E1A762FF529ADE578C935BDDF9B13F2179D4855E6FC89E9E29CA12517D17DFA1EDCE0BEBF0EA7B461FFE61D94E2BDF72C1" +
            "96F89ACD3536B644064014DAE25A15DB6BB0852ECBD120916318D1CCDEA3C84C92ED743FC176D0BACA920D3FCF3158AFF731F88CE0623182A8ED67E650515F75" +
            "745909F07D415F55FC15A35654D118C55A462D37A3ACDA08612F3F3F6571761EFCCBCC299AEE99B3A4FD6212CCFFF5EF37A2C334E871191F7E1C31960E010A54" +
            "E86FA3F62E6D6905E1CD57732410A3EB0C6B4DEFDABE9F59BF1618758C751CD56CEF851D1C0EAA1C558E37AC108DA9089863D20E2E7E4BF475EC66FE6B3EFDCF"),
        Exponent = [0x01, 0x00, 0x01],
    };

    /// Reads a chunklist and checks its signature with Apple's key (or the key given, for tests). Throws
    /// ChunklistException for anything that is not a signed chunklist of this format.
    public static Chunklist Parse(ReadOnlySpan<byte> data, RSAParameters? key = null)
    {
        if (data.Length < HeaderSize) throw new ChunklistException("the chunklist is shorter than its header");
        var h = data[..HeaderSize];
        if (!h[..4].SequenceEqual("CNKL"u8)) throw new ChunklistException("not a chunklist (no CNKL magic)");
        uint headerSize = BinaryPrimitives.ReadUInt32LittleEndian(h[4..]);
        byte version = h[8], chunkMethod = h[9], sigMethod = h[10];
        ulong count = BinaryPrimitives.ReadUInt64LittleEndian(h[12..]);
        ulong chunkOffset = BinaryPrimitives.ReadUInt64LittleEndian(h[20..]);
        ulong sigOffset = BinaryPrimitives.ReadUInt64LittleEndian(h[28..]);
        if (headerSize != HeaderSize) throw new ChunklistException($"unexpected header size {headerSize}");
        if (version != 1) throw new ChunklistException($"unexpected chunklist version {version}");
        if (chunkMethod != 1) throw new ChunklistException($"unexpected chunk method {chunkMethod}");
        if (sigMethod == 2) throw new ChunklistException("the chunklist is not signed (signature method 2): refused");
        if (sigMethod != 1) throw new ChunklistException($"unknown signature method {sigMethod}: refused");
        if (count == 0 || count > 1_000_000) throw new ChunklistException($"unexpected chunk count {count}");
        if (chunkOffset != HeaderSize) throw new ChunklistException($"unexpected chunk offset {chunkOffset}");
        if (sigOffset != chunkOffset + ChunkSize * count) throw new ChunklistException("the signature offset does not follow the chunks");
        if ((ulong)data.Length != sigOffset + SignatureSize)
            throw new ChunklistException($"the chunklist is {data.Length} bytes, not {sigOffset + SignatureSize}");

        var chunks = new List<Chunk>((int)count);
        for (int i = 0; i < (int)count; i++)
        {
            var c = data.Slice(HeaderSize + i * ChunkSize, ChunkSize);
            uint size = BinaryPrimitives.ReadUInt32LittleEndian(c);
            if (size == 0) throw new ChunklistException($"chunk {i + 1} has size 0");
            chunks.Add(new Chunk(size, c[4..].ToArray()));
        }
        var digest = SHA256.HashData(data[..(int)sigOffset]);
        var signature = data.Slice((int)sigOffset, SignatureSize).ToArray();
        Array.Reverse(signature);   // stored little endian; RSA takes it big endian
        using var rsa = RSA.Create();
        rsa.ImportParameters(key ?? AppleKey);
        if (!rsa.VerifyHash(digest, signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1))
            throw new ChunklistException("the chunklist's signature does not verify with Apple's key: refused");
        return new Chunklist(chunks, digest, sigMethod);
    }

    /// Checks pieces as they arrive (a download) or are read (a file): Feed bytes in any sizes; each piece is hashed
    /// when it is complete and a bad one throws at once. Finish() throws when the image is shorter.
    public sealed class Verifier(Chunklist list)
    {
        int index;
        uint filled;
        IncrementalHash hash = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        public long Bytes { get; private set; }
        public int ChunksVerified => index;
        public int ChunkCount => list.Chunks.Count;

        public void Feed(ReadOnlySpan<byte> data)
        {
            while (data.Length > 0)
            {
                if (index >= list.Chunks.Count) throw new ChunklistException("the image is longer than its chunklist: refused");
                var chunk = list.Chunks[index];
                int take = (int)Math.Min((uint)data.Length, chunk.Size - filled);
                hash.AppendData(data[..take]);
                filled += (uint)take;
                Bytes += take;
                data = data[take..];
                if (filled == chunk.Size)
                {
                    if (!CryptographicOperations.FixedTimeEquals(hash.GetHashAndReset(), chunk.Sha256))
                        throw new ChunklistException($"chunk {index + 1} of {list.Chunks.Count} does not match Apple's hash: refused");
                    index++;
                    filled = 0;
                }
            }
        }

        public void Finish()
        {
            if (index != list.Chunks.Count || filled != 0)
                throw new ChunklistException($"the image ends after {Bytes} bytes, in chunk {index + 1} of {list.Chunks.Count}: refused");
        }
    }

    /// Checks a whole image file; progress gets the bytes checked so far.
    public void VerifyFile(string path, Action<long>? progress = null, CancellationToken cancel = default)
    {
        var v = new Verifier(this);
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20, FileOptions.SequentialScan);
        if (f.Length != ImageSize) throw new ChunklistException($"{Path.GetFileName(path)} is {f.Length} bytes, the chunklist says {ImageSize}: refused");
        var buf = new byte[1 << 20];
        int n;
        while ((n = f.Read(buf, 0, buf.Length)) > 0)
        {
            cancel.ThrowIfCancellationRequested();
            v.Feed(buf.AsSpan(0, n));
            progress?.Invoke(v.Bytes);
        }
        v.Finish();
    }

    /// Builds a signed chunklist for an image with the given private key (only for tests: Apple signs the real ones).
    public static byte[] Build(byte[] image, int chunkSize, RSA key, byte signatureMethod = 1)
    {
        var chunks = new List<(uint Size, byte[] Hash)>();
        for (int o = 0; o < image.Length; o += chunkSize)
        {
            int len = Math.Min(chunkSize, image.Length - o);
            chunks.Add(((uint)len, SHA256.HashData(image.AsSpan(o, len))));
        }
        int sigOffset = HeaderSize + ChunkSize * chunks.Count;
        var body = new byte[sigOffset];
        "CNKL"u8.CopyTo(body);
        BinaryPrimitives.WriteUInt32LittleEndian(body.AsSpan(4), HeaderSize);
        body[8] = 1; body[9] = 1; body[10] = signatureMethod;
        BinaryPrimitives.WriteUInt64LittleEndian(body.AsSpan(12), (ulong)chunks.Count);
        BinaryPrimitives.WriteUInt64LittleEndian(body.AsSpan(20), HeaderSize);
        BinaryPrimitives.WriteUInt64LittleEndian(body.AsSpan(28), (ulong)sigOffset);
        for (int i = 0; i < chunks.Count; i++)
        {
            BinaryPrimitives.WriteUInt32LittleEndian(body.AsSpan(HeaderSize + i * ChunkSize), chunks[i].Size);
            chunks[i].Hash.CopyTo(body.AsSpan(HeaderSize + i * ChunkSize + 4));
        }
        byte[] tail;
        if (signatureMethod == 2) tail = SHA256.HashData(body);
        else
        {
            tail = key.SignHash(SHA256.HashData(body), HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
            Array.Reverse(tail);
        }
        return [.. body, .. tail];
    }
}
