#!/bin/bash
# SPDX-License-Identifier: MIT
# release-keys.sh: the release signing keys built into both apps (docs/RELEASE-KEYS.md), in one place.
#
#   release/release-keys.sh list                 the macOS app's keys, one per line (app/Sources/Signing.swift)
#   release/release-keys.sh check                the macOS app's and the Windows app's lists are the same (exit 1 if not)
#   release/release-keys.sh add PUBLIC           a successor key into both lists, after the others (key change, step 2)
#   release/release-keys.sh retire PUBLIC        an old key out of both lists (key change, step 4; never the last one)
#   release/release-keys.sh release-check LIST SIG [SIGS] [PREV_KEYS]
#                                                the signature checks of release.sh for a release's list of hashes:
#                                                SIG (.sha256.sig, what every app reads before the key change) verifies
#                                                with a key of the previous release's app (PREV_KEYS: its keys, spaces
#                                                between them; release.sh takes them from the last v* tag) and with one
#                                                of the current app's keys; every line of SIGS (.sha256.sigs, "" when
#                                                there is none) verifies with a current key, and every key the current
#                                                app adds since the previous release has a line there. Prints ok/warn/FAIL
#                                                lines; exit 1 on a FAIL. KEY_RESET=1 (a lost or exposed key) lets a SIG
#                                                that the previous app cannot verify through as a warning.
#
# PUBLIC is base64 of a raw 32-byte Ed25519 public key (swift release/signing.swift public KEYFILE prints it).
# KEYS_ROOT=<dir> works on <dir>/app/Sources/Signing.swift and <dir>/windows/app/IntelXe.Core/Release/ReleaseSigning.cs
# instead of this repository's (release/test-signing.sh runs on copies).
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
ROOT=${KEYS_ROOT:-$R}
SWIFT_FILE=$ROOT/app/Sources/Signing.swift
CS_FILE=$ROOT/windows/app/IntelXe.Core/Release/ReleaseSigning.cs
SWIFT_RE='^let releaseSigningKeys = \[\(.*\)\]$'
CS_RE='^\( *public static readonly IReadOnlyList<string> Keys = \)\[\(.*\)\];$'

die() { echo "release-keys: $*" >&2; exit 1; }
keys_of() { tr -d '" ' | tr ',' '\n' | sed '/^$/d'; }   # '"a", "b"' -> one key per line
swift_keys() { [ "$(grep -c "$SWIFT_RE" "$SWIFT_FILE")" = 1 ] || die "no single releaseSigningKeys line in $SWIFT_FILE"
               sed -n "s/$SWIFT_RE/\1/p" "$SWIFT_FILE" | keys_of; }
cs_keys() { [ "$(grep -c "$CS_RE" "$CS_FILE")" = 1 ] || die "no single Keys line in $CS_FILE"
            sed -n "s/$CS_RE/\2/p" "$CS_FILE" | keys_of; }
valid_key() { # base64 of exactly 32 bytes, and only base64's characters (the lists are edited with sed)
    [[ $1 =~ ^[A-Za-z0-9+/]{43}=$ ]] && [ "$(printf '%s' "$1" | base64 -D 2>/dev/null | wc -c | tr -d ' ')" = 32 ]
}
write_lists() { # the keys on stdin -> both files
    local list; list=$(sed 's/.*/"&"/' | paste -sd, - | sed 's/,/, /g')
    sed -i '' "s#$SWIFT_RE#let releaseSigningKeys = [$list]#" "$SWIFT_FILE"
    sed -i '' "s#$CS_RE#\1[$list];#" "$CS_FILE"
}
check() {
    local s c; s=$(swift_keys) || exit 1; c=$(cs_keys) || exit 1
    [ -n "$s" ] || die "app/Sources/Signing.swift lists no key"
    [ "$s" = "$c" ] || { echo "release-keys: the lists differ: Signing.swift [$(paste -sd' ' - <<< "$s")], ReleaseSigning.cs [$(paste -sd' ' - <<< "$c")]" >&2; return 1; }
    while IFS= read -r k; do valid_key "$k" || die "not an Ed25519 public key in the lists: $k"; done <<< "$s"
    [ "$(sort <<< "$s" | uniq -d)" = "" ] || die "a key is listed twice"
}
verify() { # PUBLIC FILE SIG; SIGNING=<signing.swift compiled> saves the script's start-up (release/test-signing.sh)
    if [ -n "${SIGNING:-}" ]; then "$SIGNING" verify "$1" "$2" "$3" >/dev/null 2>&1
    else swift "$R/release/signing.swift" verify "$1" "$2" "$3" >/dev/null 2>&1; fi
}

case "${1:-}" in
list) swift_keys ;;
check) check && echo "release keys: $(swift_keys | wc -l | tr -d ' ') key(s), the same in both apps" ;;
add)
    [ $# = 2 ] || die "usage: add PUBLIC"
    valid_key "$2" || die "not base64 of a 32-byte Ed25519 public key: $2"
    check || exit 1
    swift_keys | grep -qxF "$2" && die "$2 is listed already"
    { swift_keys; echo "$2"; } | write_lists
    check && echo "added $2: $(swift_keys | wc -l | tr -d ' ') keys in Signing.swift and ReleaseSigning.cs" ;;
retire)
    [ $# = 2 ] || die "usage: retire PUBLIC"
    check || exit 1
    swift_keys | grep -qxF "$2" || die "$2 is not listed"
    [ "$(swift_keys | grep -cvxF "$2")" -ge 1 ] || die "$2 is the last key: an app without a key accepts no release"
    swift_keys | grep -vxF "$2" | write_lists
    check && echo "retired $2: $(swift_keys | wc -l | tr -d ' ') key(s) left" ;;
release-check)
    [ $# -ge 3 ] || die "usage: release-check LIST SIG [SIGS] [PREV_KEYS]"
    LIST=$2 SIG=$3 SIGS=${4:-} PREV=${5:-}
    fails=0
    ok()  { printf '  ok    %s\n' "$*"; }
    bad() { printf '  FAIL  %s\n' "$*"; fails=$((fails + 1)); }
    warn(){ printf '  warn  %s\n' "$*"; }
    if out=$(check 2>&1); then ok "the macOS and Windows apps list the same $(swift_keys | wc -l | tr -d ' ') release key(s)"
    else bad "${out#release-keys: }"; fi
    CUR=$(swift_keys)
    name=$(basename "$SIG")
    if [ ! -f "$SIG" ]; then bad "no $name: IntelXe Manager refuses unsigned releases (package with SIGN_KEY=<key file>)"
    else
        good=no; for k in $CUR; do verify "$k" "$LIST" "$SIG" && good=yes; done
        [ $good = yes ] && ok "$name verifies with a key of this release's app" || bad "$name does not verify with app/Sources/Signing.swift's keys"
        if [ -z "$PREV" ]; then warn "no previous release's keys: $name not checked against the apps already installed"
        else
            good=no; for k in $PREV; do verify "$k" "$LIST" "$SIG" && good=yes; done
            if [ $good = yes ]; then ok "$name verifies with a key of the previous release's app (what installed apps read)"
            elif [ "${KEY_RESET:-0}" = 1 ]; then warn "$name does not verify with the previous release's keys (KEY_RESET=1: installed apps refuse this release; users download it by hand)"
            else bad "$name does not verify with the previous release's keys: installed apps would refuse it (sign .sig with the old key, SIGN_KEY; KEY_RESET=1 for a lost or exposed key)"; fi
        fi
    fi
    NEW=$(for k in $CUR; do grep -qxF -- "$k" <<< "$(tr ' ' '\n' <<< "$PREV")" || echo "$k"; done)
    [ -n "$PREV" ] || NEW=
    if [ -n "$SIGS" ] && [ -f "$SIGS" ]; then
        t=$(mktemp -d); n=0; g=0; signed=" "
        while IFS= read -r line; do
            [ -n "$line" ] || continue; n=$((n + 1)); printf '%s\n' "$line" > "$t/line"
            for k in $CUR; do verify "$k" "$LIST" "$t/line" && { g=$((g + 1)); signed="$signed$k "; break; }; done
        done < "$SIGS"
        rm -rf "$t"
        [ $n -gt 0 ] && [ $g = $n ] && ok "$(basename "$SIGS"): $n signature(s), each from a key the app accepts" || bad "$(basename "$SIGS"): $g of $n lines verify with the app's keys"
        for k in $NEW; do
            [[ $signed == *" $k "* ]] && ok "$(basename "$SIGS") has a line by the new key $k" || bad "$(basename "$SIGS") has no line by the key this release adds ($k): package with SIGN_KEY_NEXT=<its key file>"
        done
    else
        for k in $NEW; do warn "this release adds the key $k but has no .sha256.sigs: package with SIGN_KEY_NEXT=<its key file> to try the new key in this release"; done
    fi
    [ $fails = 0 ] ;;
*) sed -n '3,22p' "$0"; exit 2 ;;
esac
