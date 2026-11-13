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

## Changing the key

A key is changed in three releases, so that every installed app keeps accepting updates. `release/release-keys.sh`
edits both apps' lists together, `package.sh` and `release.sh` check each step, and `release/test-signing.sh` runs the
whole change with throwaway keys (below).

1. **Make the new key.** `swift release/signing.swift keygen keys/intelxe-release-ed25519-2.key` prints its public key;
   back the file up as above.
2. **Release N (transition), signed by both keys.** Add the new public key to both apps (it goes after the old one, which
   stays):
   `bash release/release-keys.sh add <new public key>` (it edits `releaseSigningKeys` in `app/Sources/Signing.swift`
   and `ReleaseSigning.Keys` in `windows/app/IntelXe.Core/Release/ReleaseSigning.cs`, and refuses a malformed key, a
   key listed already, or lists that differ). Commit, build both apps from that commit, then package with both keys:
   `SIGN_KEY=keys/<old>.key SIGN_KEY_NEXT=keys/<new>.key bash release/package.sh ...`.
   That writes `VERSION.sha256.sig` (old key: what apps from before release N read) and `VERSION.sha256.sigs` (one line
   per key: what apps from release N on read). Users update the app with release N.
3. **Release N+1 and later, signed by the new key only.** `SIGN_KEY=keys/<new>.key`. Apps older than N refuse these
   releases; their users install release N first (from the release page, as a manual download) or download N+1's
   package by hand.
4. **Retire the old key.** In a later release `bash release/release-keys.sh retire <old public key>` (it never removes
   the last key); destroy the old private key and its backups once no supported app version needs it.

`bash release/release-keys.sh check` says whether the two apps list the same keys; `list` prints them.

### What the scripts check

- `package.sh`, before it writes anything: the two apps' lists are the same; `SIGN_KEY`'s public key is in them (or
  the release could not be installed by its own app); with `SIGN_KEY_NEXT`, that key is in them too and is not
  `SIGN_KEY` (the transition release must ship apps that know the key of the next one), and `SIGN_KEY_NEXT` is refused
  without `SIGN_KEY`. With `APP_DIR`, the built IntelXe Manager must carry every key of the list (its executable holds
  each as text: an app built from an older tree is refused); with `WINAPP_ZIP`, `IntelXe.Core.dll` must carry each
  (as UTF-16).
- `release.sh` (`release-keys.sh release-check`): the two apps list the same keys; `.sha256.sig` verifies with a key of
  this release's app **and** with a key of the previous release's app (`app/Sources/Signing.swift` at the last `v*`
  tag before this version: what installed apps read); every `.sha256.sigs` line verifies with a key of this release's
  app; a key this release adds must have a line in `.sha256.sigs` (without a `.sigs` file it only warns: the new key
  then signs nothing until N+1). For a lost or exposed key (below), `KEY_RESET=1` turns "the previous app cannot verify
  `.sig`" into a warning.
- The apps: the macOS app reads `.sha256.sigs` when the release has it, else `.sha256.sig` (`Model.swift`), and accepts
  any line signed by one of its keys (`releaseSignatureValid`, whose `keys` parameter the tests use); IntelXe Manager for
  Windows the same (`ProbeDownload.cs`, `ReleaseSigning.SignatureValid`). Apps up to 0.1.4 read `.sha256.sig` only,
  which is why `.sig` keeps the old key's single line during the change.

### Tests

- `bash release/test-signing.sh` (macOS, about 15 s): makes throwaway keys in a temporary folder and runs the change:
  `signing.swift` keygen/sign/verify; the app's real `Signing.swift`, compiled with a small driver, as an app before the
  change (old key, reads `.sig`), of release N (both keys, prefers `.sigs`) and after retiring (new key only), against
  releases N and N+1, one signed by another key, a changed list and a changed signature; `release-keys.sh`
  add/retire/check on copies of both lists (and an app built from the edited copy); `release-check` on each step and its
  failures; `package.sh`'s key checks. It never reads `keys/`.
- `dotnet test windows/app/IntelXe.Core.Tests --filter "FullyQualifiedName~ReleaseSigning"`: the Windows check
  (RFC 8032 vectors, a key change with two keys) and `TheKeysAreTheMacAppsKeys`, which fails while the two lists
  differ.
- The macOS app's `--selftest`: the project's key, a `.sigs` file, a key added and a key removed.

## If the private key is lost or exposed

- **Lost:** no new release can be installed by existing apps. Make a new key, `release-keys.sh add` it and `retire`
  the lost one, publish the app and packages signed by the new key (`release.sh` with `KEY_RESET=1`), and tell users
  (README, release notes) to download that app once by hand.
- **Exposed:** anyone holding it can sign a release the app accepts, if they can also publish on the release page.
  Make a new key at once, publish an app release that accepts only the new key (`release-keys.sh add` the new one,
  `retire` the exposed one; signed with the new key and checked with `KEY_RESET=1`, so users install it by hand), and
  say so in the release notes and the README. Rotate the GitHub account's credentials too.

## Checking a signature by hand

```sh
swift release/signing.swift verify <public key> intel-gpu-macos-driver-VERSION.sha256 intel-gpu-macos-driver-VERSION.sha256.sig
shasum -a 256 -c intel-gpu-macos-driver-VERSION.sha256
```

The current public key is in `app/Sources/Signing.swift` and the README.
