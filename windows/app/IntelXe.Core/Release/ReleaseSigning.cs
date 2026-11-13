// SPDX-License-Identifier: MIT
// Release signatures, as the macOS app checks them (app/Sources/Signing.swift): a release's list of hashes
// (intel-gpu-macos-driver-VERSION.sha256) is signed with the project's Ed25519 key (release/signing.swift,
// VERSION.sha256.sig beside it; VERSION.sha256.sigs with one line per key during a key change: docs/RELEASE-KEYS.md).
// The app takes a downloaded release file only when that list is signed by a key below and the file's SHA-256 is in it,
// so a file that only came from the same GitHub page (an account taken over, an asset replaced) is refused.
namespace IntelXe.Core.Release;

public static class ReleaseSigning
{
    /// Ed25519 public keys whose signatures the app accepts (base64 of the raw 32 bytes): the same list as the macOS app's
    /// releaseSigningKeys (a test holds the two together). A new key goes here in a release signed with an old one before
    /// the old one is retired (docs/RELEASE-KEYS.md).
    public static readonly IReadOnlyList<string> Keys = ["PZGslAxn39LtMGgkWHbn9MzK8nc+IeM2TrvJpbifZXA="];

    /// Whether signature (the .sig file's text: base64, one line; or the .sigs file's: one signature per line) holds a
    /// signature of list by one of keys (Keys when null).
    public static bool SignatureValid(ReadOnlySpan<byte> list, string signature, IReadOnlyList<string>? keys = null)
    {
        var raw = new List<byte[]>();
        foreach (var k in keys ?? Keys)
            if (FromBase64(k) is { Length: 32 } b) raw.Add(b);
        foreach (var line in signature.Split('\n'))
        {
            if (FromBase64(line.Trim()) is not { Length: 64 } sig) continue;
            foreach (var key in raw)
                if (Ed25519.Verify(key, list, sig)) return true;
        }
        return false;
    }

    static byte[]? FromBase64(string s)
    {
        if (s.Length == 0) return null;
        try { return Convert.FromBase64String(s); }
        catch (FormatException) { return null; }
    }

    /// The SHA-256 a `shasum -a 256` list gives for this file name (lower-case hex), or null: lines "HASH  NAME" or
    /// "HASH *NAME", the name compared exactly.
    public static string? ListedHash(string list, string name)
    {
        foreach (var raw in list.Split('\n'))
        {
            var line = raw.TrimEnd('\r');
            int sp = line.IndexOf(' ');
            if (sp != 64) continue;
            var hash = line[..64];
            if (!hash.All(Uri.IsHexDigit)) continue;
            var rest = line[65..];
            if (rest.StartsWith(' ') || rest.StartsWith('*')) rest = rest[1..];
            if (rest == name) return hash.ToLowerInvariant();
        }
        return null;
    }
}
