// SPDX-License-Identifier: MIT
// Ed25519 signature verification (RFC 8032, section 5.1.7), for the release's signed list of hashes. .NET has no
// Ed25519, and the app takes no cryptography package for one check of one signature per download: this is the RFC's
// arithmetic on System.Numerics.BigInteger (a few milliseconds; the inputs are public, so it need not be constant time).
// It accepts what CryptoKit (release/signing.swift, the macOS app) and BoringSSL accept: a public key that decodes to a
// point, S below the group order, and [S]B - [k]A encoding to exactly the signature's R (cofactorless, compared as bytes).
// Tested with RFC 8032's vectors and with signatures CryptoKit made (IntelXe.Core.Tests, ReleaseSigningTests).
using System.Numerics;
using System.Security.Cryptography;

namespace IntelXe.Core.Release;

public static class Ed25519
{
    static readonly BigInteger P = BigInteger.Pow(2, 255) - 19;
    static readonly BigInteger L = BigInteger.Pow(2, 252) + BigInteger.Parse("27742317777372353535851937790883648493");
    static readonly BigInteger D = Mod(-121665 * Inv(121666));
    static readonly BigInteger D2 = Mod(2 * D);
    static readonly BigInteger SqrtM1 = BigInteger.ModPow(2, (P - 1) / 4, P);
    static readonly Point B = BasePoint();

    /// Whether signature (64 bytes) is publicKey's (32 bytes) signature of message. False for any malformed input.
    public static bool Verify(ReadOnlySpan<byte> publicKey, ReadOnlySpan<byte> message, ReadOnlySpan<byte> signature)
    {
        if (publicKey.Length != 32 || signature.Length != 64) return false;
        if (Decode(publicKey) is not { } a) return false;
        var s = new BigInteger(signature[32..], isUnsigned: true, isBigEndian: false);
        if (s >= L) return false;
        var buf = new byte[64 + message.Length];
        signature[..32].CopyTo(buf);
        publicKey.CopyTo(buf.AsSpan(32));
        message.CopyTo(buf.AsSpan(64));
        var k = new BigInteger(SHA512.HashData(buf), isUnsigned: true, isBigEndian: false) % L;
        // [S]B + [k](-A) must be R
        var r = Add(Multiply(B, s), Multiply(Negate(a), k));
        return Encode(r).AsSpan().SequenceEqual(signature[..32]);
    }

    // ---- the field and the curve: extended coordinates (X:Y:Z:T), x = X/Z, y = Y/Z, x*y = T/Z ----

    readonly record struct Point(BigInteger X, BigInteger Y, BigInteger Z, BigInteger T);

    static BigInteger Mod(BigInteger v) { var m = v % P; return m.Sign < 0 ? m + P : m; }
    static BigInteger Inv(BigInteger v) => BigInteger.ModPow(Mod(v), P - 2, P);

    static readonly Point Zero = new(0, 1, 1, 0);

    /// RFC 8032 5.1.4: the complete addition law of the twisted Edwards curve with a = -1 (also used for doubling).
    static Point Add(Point p, Point q)
    {
        var a = Mod((p.Y - p.X) * (q.Y - q.X));
        var b = Mod((p.Y + p.X) * (q.Y + q.X));
        var c = Mod(p.T * D2 * q.T);
        var d = Mod(2 * p.Z * q.Z);
        BigInteger e = b - a, f = d - c, g = d + c, h = b + a;
        return new Point(Mod(e * f), Mod(g * h), Mod(f * g), Mod(e * h));
    }

    static Point Negate(Point p) => new(Mod(-p.X), p.Y, p.Z, Mod(-p.T));

    static Point Multiply(Point p, BigInteger n)
    {
        var r = Zero;
        for (int i = (int)n.GetBitLength() - 1; i >= 0; i--)
        {
            r = Add(r, r);
            if (!(n >> i).IsEven) r = Add(r, p);
        }
        return r;
    }

    static byte[] Encode(Point p)
    {
        var zi = Inv(p.Z);
        var x = Mod(p.X * zi);
        var y = Mod(p.Y * zi);
        var b = new byte[32];
        y.TryWriteBytes(b, out _, isUnsigned: true, isBigEndian: false);
        if (!x.IsEven) b[31] |= 0x80;
        return b;
    }

    /// RFC 8032 5.1.3; null when the bytes are not a point (y not below p, no square root, or x = 0 with its sign bit set).
    static Point? Decode(ReadOnlySpan<byte> s)
    {
        var bytes = s.ToArray();
        bool sign = (bytes[31] & 0x80) != 0;
        bytes[31] &= 0x7F;
        var y = new BigInteger(bytes, isUnsigned: true, isBigEndian: false);
        if (y >= P) return null;
        var x = RecoverX(y, sign);
        return x is { } xv ? new Point(xv, y, 1, Mod(xv * y)) : null;
    }

    static BigInteger? RecoverX(BigInteger y, bool sign)
    {
        var y2 = Mod(y * y);
        var u = Mod(y2 - 1);
        var v = Mod(D * y2 + 1);
        // x = u v^3 (u v^7)^((p-5)/8)
        var v3 = Mod(v * v * v);
        var x = Mod(u * v3 * BigInteger.ModPow(Mod(u * v3 * v3 * v), (P - 5) / 8, P));
        var vx2 = Mod(v * x * x);
        if (vx2 != u)
        {
            if (vx2 != Mod(-u)) return null;
            x = Mod(x * SqrtM1);
        }
        if (x.IsZero && sign) return null;
        if (!x.IsEven != sign) x = P - x;
        return x;
    }

    static Point BasePoint()
    {
        var y = Mod(4 * Inv(5));
        var x = RecoverX(y, false)!.Value;
        return new Point(x, y, 1, Mod(x * y));
    }
}
