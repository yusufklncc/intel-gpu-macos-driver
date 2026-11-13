# Release signing keys

IntelXe Manager installs a downloaded package only when the release's list of hashes (`VERSION.sha256`) carries a
signature by one of the Ed25519 keys built into the app (`app/Sources/Signing.swift`, `releaseSigningKeys`). The
signature is `VERSION.sha256.sig` (one line, base64), made by `release/package.sh` with `SIGN_KEY=<private key file>`;
`release/release.sh` refuses to publish a release whose signature the app would not accept.

IntelXe Manager for Windows checks the same list and signature before it downloads the Xe probe stick's image
(`xe-probe-stick-VERSION.img.zip`, listed in the same `VERSION.sha256`), with its own copy of the keys:
`windows/app/IntelXe.Core/Release/ReleaseSigning.cs`, `Keys`. A key change below changes both lists in the same commit;
`ReleaseSigningTests` (`dotnet test windows/app/IntelXe.Core.Tests`) fails while they differ.

The private key never goes into a repository. Keep it in two places: the maintainer's machine (`keys/`, ignored by
git) and an offline backup (a password manager entry or an encrypted USB stick). Without it no later release can be
installed by the app; without the backup a lost disk is a lost key.

## Changing the key (planned)

A key is changed in three releases, so that every installed app keeps accepting updates:

1. **Make the new key.** `swift release/signing.swift keygen keys/intelxe-release-ed25519-2.key` prints its public key;
   back the file up as above.
2. **Release N (transition), signed by both keys.** Add the new public key to `releaseSigningKeys` and to the Windows
   app's `ReleaseSigning.Keys` (keep the old one),
   then package with both:
   `SIGN_KEY=keys/<old>.key SIGN_KEY_NEXT=keys/<new>.key bash release/package.sh ...`.
   That writes `VERSION.sha256.sig` (old key: what apps from before release N read) and `VERSION.sha256.sigs` (one line
   per key: what apps from release N on read; `release.sh` checks every line). Users update the app with release N.
3. **Release N+1 and later, signed by the new key only.** `SIGN_KEY=keys/<new>.key`. Apps older than N refuse these
   releases; their users install release N first (from the release page, as a manual download) or download N+1's
   package by hand.
4. **Retire the old key.** In a later release remove the old public key from `releaseSigningKeys` and `ReleaseSigning.Keys`; destroy the old
   private key and its backups once no supported app version needs it.

## If the private key is lost or exposed

- **Lost:** no new release can be installed by existing apps. Make a new key, add it to the app, publish the app and
  packages signed by the new key, and tell users (README, release notes) to download that app once by hand.
- **Exposed:** anyone holding it can sign a release the app accepts, if they can also publish on the release page.
  Make a new key at once, publish an app release that accepts only the new key (signed with the new key, so users
  install it by hand), and say so in the release notes and the README. Rotate the GitHub account's credentials too.

## Checking a signature by hand

```sh
swift release/signing.swift verify <public key> intel-gpu-macos-driver-VERSION.sha256 intel-gpu-macos-driver-VERSION.sha256.sig
shasum -a 256 -c intel-gpu-macos-driver-VERSION.sha256
```

The current public key is in `app/Sources/Signing.swift` and the README.
