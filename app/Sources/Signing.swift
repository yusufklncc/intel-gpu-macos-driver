// SPDX-License-Identifier: MIT
// Release signatures: a release's list of hashes (VERSION.sha256) is signed with the project's Ed25519 key
// (release/signing.swift, VERSION.sha256.sig beside it). The app installs a downloaded package only when that list is
// signed by a key below and the zip's hash is in it, so a release that only came from the same GitHub page (an account
// taken over, an asset replaced) is refused.
import CryptoKit
import Foundation

/// Ed25519 public keys whose signatures the app accepts (base64 of the raw 32 bytes). A new key goes here in an app
/// release signed with an old one before the old one is retired (docs/RELEASE-KEYS.md): release/release-keys.sh adds
/// and retires keys here and in the Windows app's list (ReleaseSigning.cs) together; keep this on one line, as it reads it.
let releaseSigningKeys = ["PZGslAxn39LtMGgkWHbn9MzK8nc+IeM2TrvJpbifZXA="]

/// Whether `signature` holds a signature of `list` by one of releaseSigningKeys: the .sig file's text (base64, one line)
/// or the .sigs file's (one signature per line, one line per key during a key change: docs/RELEASE-KEYS.md). `keys` is
/// for tests (release/test-signing.sh: throwaway keys).
func releaseSignatureValid(list: Data, signature: String, keys: [String] = releaseSigningKeys) -> Bool {
    let lines = signature.split(whereSeparator: \.isNewline).map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
    return lines.contains { line in
        guard let sig = Data(base64Encoded: line) else { return false }
        return keys.contains { key in
            guard let raw = Data(base64Encoded: key), let publicKey = try? Curve25519.Signing.PublicKey(rawRepresentation: raw) else { return false }
            return publicKey.isValidSignature(sig, for: list)
        }
    }
}
