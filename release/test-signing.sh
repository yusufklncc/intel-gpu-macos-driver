#!/bin/bash
# SPDX-License-Identifier: MIT
# test-signing.sh: the release signing keys' change (docs/RELEASE-KEYS.md) end to end, with throwaway keys made in a
# temporary folder (never the project's key): signing.swift's keygen/sign/verify; the app's real check (app/Sources/
# Signing.swift, compiled with a small driver) as an app before the change, during it and after the old key is retired;
# release-keys.sh add/retire/check on copies of both apps' key lists; release-keys.sh release-check (release.sh's
# signature checks) on made-up releases; and package.sh's key checks, which run before it reads any input.
#
#   bash release/test-signing.sh        (macOS, swiftc; about a minute)
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d -t ixesign)
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok()  { pass=$((pass + 1)); printf '  ok    %s\n' "$*"; }
bad() { fail=$((fail + 1)); printf '  FAIL  %s\n' "$*"; }
expect() { # expect 0|1 "what" command...: the command's exit status is 0 (1: not 0)
    local want=$1 what=$2; shift 2
    if "$@" >"$T/last.out" 2>&1; then got=0; else got=1; fi
    [ $got = "$want" ] && ok "$what" || { bad "$what (exit $got)"; sed 's/^/        /' "$T/last.out"; }
}

echo "== building signing.swift and the app's check"
swiftc -O "$R/release/signing.swift" -o "$T/signing" 2>"$T/build.log" || { cat "$T/build.log"; exit 1; }
SIGN=$T/signing
# The driver: the app's releaseSignatureValid with the keys given (or the list built in), reading the signature the way
# an app of that generation does: "old" reads .sig only (0.1.4 and before), "new" prefers .sigs (Model.swift since the
# key change was planned; ProbeDownload.cs the same).
cat > "$T/main.swift" <<'EOF'
import Foundation
let a = CommandLine.arguments   // KEYS|default MODE LIST SIG SIGS
let keys = a[1] == "default" ? releaseSigningKeys : a[1].split(separator: ",").map(String.init)
let sigsPath = a[5]
let path = (a[2] == "new" && FileManager.default.fileExists(atPath: sigsPath)) ? sigsPath : a[4]
let list = FileManager.default.contents(atPath: a[3]) ?? Data()
let text = (FileManager.default.contents(atPath: path)).map { String(decoding: $0, as: UTF8.self) } ?? ""
exit(releaseSignatureValid(list: list, signature: text, keys: keys) ? 0 : 1)
EOF
swiftc "$R/app/Sources/Signing.swift" "$T/main.swift" -o "$T/app" 2>"$T/build.log" || { cat "$T/build.log"; exit 1; }
PROJECT=$(bash "$R/release/release-keys.sh" list | head -1)
grep -a -q -F "$PROJECT" "$T/app" && ok "a key of Signing.swift is in the compiled binary as text (package.sh's check of the app finds it)" ||
    bad "the compiled app does not carry $PROJECT as text: package.sh's APP_DIR key check would refuse every app"

echo "== keys and signatures (signing.swift)"
mkdir -p "$T/keys"
OLD=$("$SIGN" keygen "$T/keys/old.key") NEW=$("$SIGN" keygen "$T/keys/new.key") EVIL=$("$SIGN" keygen "$T/keys/evil.key")
[ "$("$SIGN" public "$T/keys/old.key")" = "$OLD" ] && ok "keygen prints the key file's public key" || bad "public differs from keygen"
expect 1 "keygen does not overwrite a key file" "$SIGN" keygen "$T/keys/old.key"
[ "$(stat -f %Lp "$T/keys/old.key")" = 600 ] && ok "a new key file is mode 600" || bad "key file mode $(stat -f %Lp "$T/keys/old.key")"
# release N (the transition): package.sh with SIGN_KEY=old SIGN_KEY_NEXT=new writes .sig (old) and .sigs (old, new)
N=$T/N; mkdir -p "$N"
printf '%064d  intel-gpu-macos-driver-9.0.0-tahoe.zip\n' 1 > "$N/list"
"$SIGN" sign "$T/keys/old.key" "$N/list" && mv "$N/list.sig" "$N/sig"
for k in old new; do "$SIGN" sign "$T/keys/$k.key" "$N/list" "$N/sigs"; done
[ "$(wc -l < "$N/sigs" | tr -d ' ')" = 2 ] && ok ".sigs has one line per key" || bad ".sigs has $(wc -l < "$N/sigs") lines"
expect 0 "signing.swift verify: the old key's .sig" "$SIGN" verify "$OLD" "$N/list" "$N/sig"
expect 0 "signing.swift verify: the new key's line of .sigs" "$SIGN" verify "$NEW" "$N/list" "$N/sigs"
expect 1 "signing.swift verify: the new key is not the .sig's" "$SIGN" verify "$NEW" "$N/list" "$N/sig"
# release N+1, signed by the new key only; a release signed by someone else; a changed list
N1=$T/N1; mkdir -p "$N1"; cp "$N/list" "$N1/list"; echo "$(printf '%064d' 2)  intel-gpu-macos-driver-9.0.1-efi.zip" >> "$N1/list"
"$SIGN" sign "$T/keys/new.key" "$N1/list" && mv "$N1/list.sig" "$N1/sig"
E=$T/E; mkdir -p "$E"; cp "$N/list" "$E/list"
"$SIGN" sign "$T/keys/evil.key" "$E/list" && mv "$E/list.sig" "$E/sig"
"$SIGN" sign "$T/keys/evil.key" "$E/list" "$E/sigs"
X=$T/X; mkdir -p "$X"; sed 's/^0/f/' "$N/list" > "$X/list"; cp "$N/sig" "$N/sigs" "$X/"
B=$T/B; mkdir -p "$B"; cp "$N/list" "$B/"; sed 's/^./A/' "$N/sig" > "$B/sig"; [ "$(cat "$B/sig")" != "$(cat "$N/sig")" ] || sed 's/^./B/' "$N/sig" > "$B/sig"

echo "== the app's check (Signing.swift) across the key change"
app() { "$T/app" "$1" "$2" "$3/list" "$3/sig" "$3/sigs"; }
expect 0 "an app before the change ([old], reads .sig) accepts release N" app "$OLD" old "$N"
expect 1 "an app before the change refuses release N+1 (new key only)" app "$OLD" old "$N1"
expect 0 "an app of release N ([old,new], reads .sigs) accepts release N" app "$OLD,$NEW" new "$N"
expect 0 "an app of release N accepts release N+1 (no .sigs: reads .sig)" app "$OLD,$NEW" new "$N1"
expect 0 "an app knowing only the new key accepts release N through its .sigs" app "$NEW" new "$N"
expect 1 "an app with the old key retired ([new]) refuses a release signed by the old key only" app "$NEW" old "$N"
expect 0 "an app with the old key retired accepts release N+1" app "$NEW" new "$N1"
expect 1 "a release signed by another key is refused (.sig)" app "$OLD,$NEW" old "$E"
expect 1 "a release signed by another key is refused (.sigs)" app "$OLD,$NEW" new "$E"
expect 1 "a changed list is refused (.sig)" app "$OLD,$NEW" old "$X"
expect 1 "a changed list is refused (.sigs)" app "$OLD,$NEW" new "$X"
expect 1 "a changed signature is refused" app "$OLD,$NEW" old "$B"
expect 1 "the built-in project key refuses a throwaway-signed release" app default old "$N"

echo "== release-keys.sh on copies of both apps' lists"
C=$T/root; mkdir -p "$C/app/Sources" "$C/windows/app/IntelXe.Core/Release"
cp "$R/app/Sources/Signing.swift" "$C/app/Sources/"; cp "$R/windows/app/IntelXe.Core/Release/ReleaseSigning.cs" "$C/windows/app/IntelXe.Core/Release/"
# the copies start with the old throwaway key only, as if it were the project's
sed -i '' "s#^let releaseSigningKeys = \[.*\]\$#let releaseSigningKeys = [\"$OLD\"]#" "$C/app/Sources/Signing.swift"
sed -i '' "s#^\( *public static readonly IReadOnlyList<string> Keys = \)\[.*\];\$#\1[\"$OLD\"];#" "$C/windows/app/IntelXe.Core/Release/ReleaseSigning.cs"
keys() { KEYS_ROOT=$C bash "$R/release/release-keys.sh" "$@"; }
expect 0 "check: one key, the same in both" keys check
expect 1 "add refuses a key that is not 32 bytes of base64" keys add "AAAA"
expect 1 "add refuses a malformed key" keys add "${NEW%?}#"
expect 1 "add refuses a key listed already" keys add "$OLD"
expect 0 "add the successor key" keys add "$NEW"
[ "$(keys list | paste -sd' ' -)" = "$OLD $NEW" ] && ok "Signing.swift lists old, new" || bad "Signing.swift lists $(keys list | paste -sd' ' -)"
grep -qF "Keys = [\"$OLD\", \"$NEW\"];" "$C/windows/app/IntelXe.Core/Release/ReleaseSigning.cs" && ok "ReleaseSigning.cs lists old, new" || bad "ReleaseSigning.cs: $(grep 'Keys =' "$C/windows/app/IntelXe.Core/Release/ReleaseSigning.cs")"
# an app built from the edited list (the default keys) is the transition app
swiftc "$C/app/Sources/Signing.swift" "$T/main.swift" -o "$T/app-n" 2>"$T/build.log" || { cat "$T/build.log"; exit 1; }
expect 0 "the app built from the edited Signing.swift accepts release N" "$T/app-n" default new "$N/list" "$N/sig" "$N/sigs"
expect 0 "the app built from the edited Signing.swift accepts release N+1" "$T/app-n" default new "$N1/list" "$N1/sig" "$N1/sigs"
grep -a -q -F "$NEW" "$T/app-n" && ok "that app's binary carries the new key (package.sh's APP_DIR check)" || bad "the new key is not in the built app"
cp -R "$C" "$T/root-n"   # the transition release's tree, for release-check below
expect 1 "retire refuses a key that is not listed" keys retire "$EVIL"
expect 0 "retire the old key" keys retire "$OLD"
[ "$(keys list)" = "$NEW" ] && ok "Signing.swift lists new only" || bad "Signing.swift lists $(keys list | paste -sd' ' -)"
expect 1 "retire refuses the last key" keys retire "$NEW"
expect 0 "check after retiring" keys check
cp -R "$C" "$T/root-r"
sed -i '' "s#Keys = \[\"$NEW\"\];#Keys = [\"$OLD\"];#" "$C/windows/app/IntelXe.Core/Release/ReleaseSigning.cs"
expect 1 "check fails when the Windows app's list differs" keys check
expect 1 "add refuses to edit lists that differ" keys add "$EVIL"

echo "== release-keys.sh release-check (release.sh's signature checks)"
rc() { local root=$1; shift; KEYS_ROOT=$root SIGNING=$SIGN bash "$R/release/release-keys.sh" release-check "$@"; }
expect 0 "release N: .sig by the old key, .sigs by both, the app adds the new key" rc "$T/root-n" "$N/list" "$N/sig" "$N/sigs" "$OLD"
cp "$N1/sig" "$T/sig-new"
expect 1 "release N with its .sig by the new key: installed apps would refuse it" rc "$T/root-n" "$N/list" "$N1/sig" "$N/sigs" "$OLD"
"$SIGN" sign "$T/keys/new.key" "$N/list" && mv "$N/list.sig" "$T/sig-n-new"
expect 1 "... the same refusal for a .sig by the new key of release N's own list" rc "$T/root-n" "$N/list" "$T/sig-n-new" "$N/sigs" "$OLD"
KEY_RESET=1 expect 0 "... let through with KEY_RESET=1 (a lost or exposed key), as a warning" rc "$T/root-n" "$N/list" "$T/sig-n-new" "$N/sigs" "$OLD"
grep -q '^  warn  .*KEY_RESET=1' "$T/last.out" && ok "KEY_RESET=1 warns" || bad "no KEY_RESET warning"
: > "$T/sigs-old"; "$SIGN" sign "$T/keys/old.key" "$N/list" "$T/sigs-old"
expect 1 "release N whose .sigs lacks the new key's line" rc "$T/root-n" "$N/list" "$N/sig" "$T/sigs-old" "$OLD"
cat "$N/sigs" "$E/sigs" > "$T/sigs-evil"
expect 1 "release N whose .sigs has a line by an unknown key" rc "$T/root-n" "$N/list" "$N/sig" "$T/sigs-evil" "$OLD"
expect 0 "release N without .sigs passes (a warning: the new key is not tried)" rc "$T/root-n" "$N/list" "$N/sig" "" "$OLD"
grep -q '^  warn  .*no .sha256.sigs' "$T/last.out" && ok "... and warns" || bad "no warning for the missing .sigs"
expect 0 "release N+1: .sig by the new key, the previous app knows both" rc "$T/root-n" "$N1/list" "$N1/sig" "" "$OLD $NEW"
expect 1 "release N+1 signed by the new key while the previous app knows only the old one" rc "$T/root-n" "$N1/list" "$N1/sig" "" "$OLD"
expect 0 "the retiring release: app [new], previous app [old,new], .sig by the new key" rc "$T/root-r" "$N1/list" "$N1/sig" "" "$OLD $NEW"
expect 1 "the retiring release signed by the old key: its own app refuses it" rc "$T/root-r" "$N/list" "$N/sig" "" "$OLD $NEW"
expect 1 "a release without a .sig" rc "$T/root-n" "$N/list" "$T/none" "" "$OLD"
expect 1 "the apps' lists differ" rc "$C" "$N1/list" "$N1/sig" "" "$NEW"
expect 1 "a changed list" rc "$T/root-n" "$X/list" "$X/sig" "$X/sigs" "$OLD"

echo "== package.sh's key checks (before any input is read)"
pkg() { local root=$1; shift; env KEYS_ROOT="$root" WINAPP_SKIP=1 PROBE_SKIP=1 "$@" bash "$R/release/package.sh" 9.0.0 "$T/no-tahoe" "$T/no-sequoia" "$T/pkg-out"; }
expect 1 "SIGN_KEY not in the app's list is refused" pkg "$T/root-n" SIGN_KEY="$T/keys/evil.key"
grep -q "is not in app/Sources/Signing.swift" "$T/last.out" && ok "... with the reason" || bad "... reason: $(tail -1 "$T/last.out")"
expect 1 "SIGN_KEY_NEXT not in the app's list is refused" pkg "$T/root-n" SIGN_KEY="$T/keys/old.key" SIGN_KEY_NEXT="$T/keys/evil.key"
grep -q "is not in app/Sources/Signing.swift" "$T/last.out" && ok "... with the reason" || bad "... reason: $(tail -1 "$T/last.out")"
expect 1 "SIGN_KEY_NEXT without SIGN_KEY is refused" pkg "$T/root-n" SIGN_KEY_NEXT="$T/keys/new.key"
expect 1 "SIGN_KEY_NEXT the same key as SIGN_KEY is refused" pkg "$T/root-n" SIGN_KEY="$T/keys/old.key" SIGN_KEY_NEXT="$T/keys/old.key"
grep -q "same key" "$T/last.out" && ok "... with the reason" || bad "... reason: $(tail -1 "$T/last.out")"
expect 1 "keys that differ between the apps are refused" pkg "$C" SIGN_KEY="$T/keys/new.key"
grep -q "release keys differ" "$T/last.out" && ok "... with the reason" || bad "... reason: $(tail -1 "$T/last.out")"
[ ! -e "$T/pkg-out" ] && ok "a refused key stops package.sh before it makes OUT_DIR" || bad "package.sh made $T/pkg-out"
expect 1 "old + new keys in both lists pass the key checks (then stop at the missing inputs)" pkg "$T/root-n" SIGN_KEY="$T/keys/old.key" SIGN_KEY_NEXT="$T/keys/new.key"
grep -q "missing in the Tahoe input" "$T/last.out" && ok "... past the key checks" || bad "... stopped at: $(tail -1 "$T/last.out")"

echo "== $pass passed, $fail failed"
[ $fail = 0 ]
