#!/bin/bash
# SPDX-License-Identifier: MIT
# test-osupdate-guard.sh: app/Resources/osupdate-guard.sh (the macOS update guard) through preparation and the runs after
# an update, against a throwaway root, without root: a minor update with everything in place, one that lost the
# Auxiliary Kernel Collection and the system-volume copy, a major update the package does not support, IntelXe.kext
# missing from the EFI, a boot guard the update's restarts set (with and without a panic report), a damaged kept
# package, an install that keeps failing, the bounded tries, the expiry, cancel, and an 0.1.4 package without
# --supports-macos. Also install.sh --supports-macos. The package is made from release/templates/install.sh with its
# paths under the throwaway root and root, the checks and kmutil stubbed, as release/test-install-flow.sh does;
# kmutil, nvram and shutdown are stubs. Nothing outside the throwaway root is touched.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
GUARD=$HERE/../Resources/osupdate-guard.sh
ROOT=$(mktemp -d -t ixeosg)
trap '[ -n "$ROOT" ] && chmod -R u+w "$ROOT" 2>/dev/null; rm -rf -- "$ROOT"' EXIT
export OSG_ROOT=$ROOT OSG_WAIT=0
fails=0
pass() { echo "ok    $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
check_eq() { if [ "$2" = "$3" ]; then pass "$1: $2"; else fail "$1: '$2', expected '$3'"; fi; }

# --- stubs: kmutil lists $ROOT/loaded (one bundle id per line), nvram keeps variables in $ROOT/nvram/, shutdown counts
mkdir -p "$ROOT/stub" "$ROOT/nvram" "$ROOT/System/Library/CoreServices" "$ROOT/Library/Extensions" "$ROOT/Library/GPUBundles" \
    "$ROOT/Library/Application Support/intel-gpu-macos-driver" "$ROOT/Library/KernelCollections" "$ROOT/Library/Logs/DiagnosticReports"
cat > "$ROOT/stub/kmutil" <<'S'
#!/bin/bash
while read -r id; do [ -n "$id" ] && echo "  101    0 0 0 0 $id (0.1.5) 00000000-0000-0000-0000-000000000000 <>"; done < "$(dirname "$0")/../loaded" 2>/dev/null
exit 0
S
cat > "$ROOT/stub/nvram" <<'S'
#!/bin/bash
d=$(dirname "$0")/../nvram
for a in "$@"; do
    case "$a" in
        *=*) printf '%s' "${a#*=}" > "$d/${a%%=*}";;
        *) if [ -f "$d/$a" ]; then printf '%s\t%s\n' "$a" "$(cat "$d/$a")"; else echo "nvram: Error getting variable - '$a'" >&2; exit 1; fi;;
    esac
done
S
cat > "$ROOT/stub/shutdown" <<'S'
#!/bin/bash
echo "shutdown $*" >> "$(dirname "$0")/../shutdowns"
S
chmod 755 "$ROOT/stub/"*
loaded() { printf '%s\n' "$@" > "$ROOT/loaded"; }
ALL="io.github.yusufklncc.IntelXe io.github.yusufklncc.IntelXeFB com.nullmoth.NMXAccel com.nullmoth.NMXAGDC"
setver() { # setver VERSION BUILD: the macOS the fake root runs
    printf '<?xml version="1.0" encoding="UTF-8"?>\n<plist version="1.0"><dict><key>ProductBuildVersion</key><string>%s</string><key>ProductVersion</key><string>%s</string></dict></plist>\n' \
        "$2" "$1" > "$ROOT/System/Library/CoreServices/SystemVersion.plist"
}
shutdowns() { grep -c . "$ROOT/shutdowns" 2>/dev/null || echo 0; }

# --- a fake package: kexts, the Metal driver, and install.sh from the template (or from an earlier release's template)
make_pkg() { # make_pkg DIR VERSION TEMPLATE
    local d=$1 k
    mkdir -p "$d/Library/GPUBundles/NVMTLDriverLib.bundle/Contents/MacOS" "$d/Library/GPUBundles/nvmtl"
    for k in IntelXeFB NMXAccel NMXAGDC; do
        mkdir -p "$d/Library/Extensions/$k.kext/Contents/MacOS"; echo "$k $2" > "$d/Library/Extensions/$k.kext/Contents/MacOS/$k"
    done
    echo "plugin $2" > "$d/Library/GPUBundles/NVMTLDriverLib.bundle/Contents/MacOS/NVMTLDriver"
    echo "mesa $2" > "$d/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
    echo "!*" > "$d/Library/GPUBundles/nvmtl-allow.txt"
    sed -e "s/@VER@/$2/g" -e "s/@MAJOR@/26/g" -e "s/@MACOS@/macOS 26 Tahoe/g" "$3" \
        | sed -E -e "s#([^H])/Library/#\1$ROOT/Library/#g" -e "s#SSV=/System/#SSV=$ROOT/System/#" > "$d/install.sh"
    perl -0pi -e 's/^set -euo pipefail\n/set -euo pipefail\nid() { echo 0; }\nchown() { :; }\ninstall() { local a=(); while [ \$# -gt 0 ]; do case "\$1" in -o|-g|-m) shift 2;; *) a+=("\$1"); shift;; esac; done; cp "\${a[@]}"; }\n/m; s/^check\(\) \{\n/check() { return 0; }\ncheck_real() {\n/m; s/^rebuild_auxkc\(\) \{\n/rebuild_auxkc() { echo "  ok    (kernel collection step skipped in this test)"; : > KCMARKPATH; }\nrebuild_real() {\n/m' "$d/install.sh"
    sed -i '' "s#KCMARKPATH#'$ROOT/kc-rebuilt'#" "$d/install.sh"
    chmod 755 "$d/install.sh"
    (cd "$d" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | LC_ALL=C sort | while IFS= read -r f; do shasum -a 256 "$f"; done > SHA256SUMS)
}
P=$ROOT/pkg-0.1.5; make_pkg "$P" 0.1.5 "$REPO/release/templates/install.sh"
OLD=$ROOT/pkg-0.1.4
git -C "$REPO" show 7ed533b:release/templates/install.sh > "$ROOT/install-0.1.4.sh" 2>/dev/null && make_pkg "$OLD" 0.1.4 "$ROOT/install-0.1.4.sh"
B="$ROOT/Library/Application Support/intel-gpu-macos-driver"
G="$B/osupdate"
PL="$ROOT/Library/LaunchDaemons/io.github.yusufklncc.intelxe.osupdate.plist"
st() { "$GUARD" --status | sed -n "s/^$1=//p" | head -1; }
ck() { "$GUARD" --check | sed -n "s/^$1=//p" | head -1; }
run() { "$GUARD" "$@" > "$ROOT/last.log" 2>&1; }

# --- install.sh --supports-macos
for v in 26.7:ok 26.7.1:ok 26.7.9:ok 26.8:untested 26.0.1:untested 27.0:no 15.7:no 2.6:no; do
    check_eq "install.sh --supports-macos ${v%%:*}" "$("$P/install.sh" --supports-macos "${v%%:*}")" "${v#*:}"
done
"$P/install.sh" --status | grep -q '^macos_support=' && pass "install.sh --status says macos_support" || fail "no macos_support in --status"

# --- nothing installed
setver 26.7.1 25H1; loaded io.github.yusufklncc.IntelXe
check_eq "--check with nothing installed: plugin" "$(ck plugin)" missing
check_eq "--check with nothing installed: IntelXeFB" "$(ck kext_IntelXeFB)" absent
check_eq "--check with nothing installed: IntelXe from the EFI" "$(ck kext_IntelXe)" loaded
run --prepare "$P" && fail "prepare with no driver installed went ahead" || pass "prepare refuses when the driver is not installed"
[ -e "$PL" ] && fail "a daemon after a refused prepare" || pass "no daemon after a refused prepare"

# --- installed and running
"$P/install.sh" --apply > "$ROOT/apply.log" 2>&1 || { fail "the fake install"; tail -5 "$ROOT/apply.log"; }
loaded $ALL
check_eq "--check installed: not loaded" "$(ck not_loaded)" ""
check_eq "--check installed: plugin" "$(ck plugin)" present
check_eq "--check installed: boot guard" "$(ck boot_guard)" ok
echo "accel2" > "$ROOT/nvram/intelxe-guard"; check_eq "--check: an armed guard" "$(ck boot_guard)" armed:accel2
printf 'ok' > "$ROOT/nvram/intelxe-guard"
check_eq "status before any preparation" "$(st prepared)" no

run --prepare && fail "prepare without a package went ahead" || pass "prepare refuses without a package"
[ -n "${OLD:-}" ] && { run --prepare "$OLD" && fail "prepare with another release's package went ahead" || pass "prepare refuses another release's package (0.1.4 for 0.1.5)"; }
cp -R "$P" "$ROOT/damaged"; echo x >> "$ROOT/damaged/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
run --prepare "$ROOT/damaged" && fail "prepare with a damaged package went ahead" || pass "prepare refuses a package that differs from its SHA256SUMS"
env -u OSG_ROOT "$GUARD" --prepare "$P" > "$ROOT/real.log" 2>&1 && fail "a run without root and without the test root went ahead" \
    || { grep -q "needs root" "$ROOT/real.log" && pass "without root and outside the test root it refuses before writing anything" || fail "unexpected: $(cat "$ROOT/real.log")"; }

prepare() { run --prepare "$P" "$@" || { fail "prepare $*"; cat "$ROOT/last.log"; }; }
prepare 26.7.2
check_eq "prepared" "$(st prepared)" yes
check_eq "daemon installed" "$(st daemon)" yes
plutil -lint "$PL" >/dev/null 2>&1 && pass "the daemon's plist is valid" || fail "the daemon's plist"
grep -q "$G/osupdate-guard.sh" "$PL" && [ -x "$G/osupdate-guard.sh" ] && pass "the daemon runs the kept copy of the script" || fail "the daemon's script"
(cd "$G/package" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1) && pass "the kept package matches its SHA256SUMS" || fail "the kept package"
check_eq "recorded from" "$(st state_from_version)" 26.7.1
check_eq "recorded target" "$(st state_target)" 26.7.2
check_eq "recorded before: plugin" "$(st state_before_plugin)" present

# --- the starts before the update: it waits
run --finish
check_eq "same build: still prepared" "$(st prepared)" yes
check_eq "same build: no result yet" "$(st last_result)" ""
check_eq "same build: no try counted" "$(st tries)" 0
# 15 days later, still not updated: the preparation expires
sed -i '' "s/^prepared_at=.*/prepared_at=$(( $(date +%s) - 15 * 86400 ))/" "$G/state"
run --finish
check_eq "not updated in 14 days" "$(st last_result)" expired
check_eq "expired: daemon removed" "$(st daemon)" no
[ -d "$G/package" ] && fail "expired: the kept package is still there" || pass "expired: the kept package is removed"

# --- a minor update with everything in place
prepare 26.7.2
setver 26.7.2 25H2; rm -f "$ROOT/kc-rebuilt"
run --finish
check_eq "minor update, all loaded" "$(st last_result)" ok
check_eq "minor update: support" "$(st last_support)" ok
check_eq "minor update: daemon removed" "$(st daemon)" no
check_eq "minor update: record removed" "$(st prepared)" no
[ -e "$ROOT/kc-rebuilt" ] && fail "minor update: the collection was rebuilt though nothing was missing" || pass "minor update: nothing installed again"
grep -q "macOS was updated: 26.7.1 (25H1) -> 26.7.2 (25H2)" "$G/osupdate.log" && pass "the log names both versions" || fail "the log"

# --- a minor update that lost the Auxiliary Kernel Collection and the system-volume copy
mkdir -p "$ROOT/System/Library/Extensions/NVMTLDriver.bundle/Contents/MacOS"; : > "$ROOT/System/Library/Extensions/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver"
prepare 26.7.3
check_eq "recorded before: system-volume copy" "$(st state_before_ssv)" present
rm -rf "$ROOT/System/Library/Extensions/NVMTLDriver.bundle"
setver 26.7.3 25H3; loaded io.github.yusufklncc.IntelXe; rm -f "$ROOT/kc-rebuilt" "$ROOT/shutdowns"
run --finish
check_eq "kexts not loaded after the update" "$(st last_result)" repaired
[ -e "$ROOT/kc-rebuilt" ] && pass "install.sh --apply ran (the collection was rebuilt)" || fail "install.sh --apply did not run"
check_eq "restarted once by itself" "$(shutdowns)" 1
check_eq "the daemon stays for the check after the restart" "$(st daemon)" yes
loaded $ALL
run --finish
check_eq "after the restart: all loaded" "$(st last_result)" ok
check_eq "after the restart: system-volume copy" "$(st last_ssv)" gone
st last_message | grep -q "system-volume copy" && pass "the result says the system-volume copy is gone" || fail "no word on the system-volume copy"
check_eq "after the restart: no second restart" "$(shutdowns)" 1
check_eq "after the restart: daemon removed" "$(st daemon)" no
check_eq "after the restart: tries" "$(st last_tries)" 2

# --- --no-restart: repaired, no restart
prepare 26.7.4 --no-restart
setver 26.7.4 25H4; loaded io.github.yusufklncc.IntelXe; rm -f "$ROOT/shutdowns"
run --finish
check_eq "--no-restart: repaired" "$(st last_result)" repaired
check_eq "--no-restart: no restart" "$(shutdowns)" 0
loaded $ALL; run --finish
check_eq "--no-restart: ok after a restart by hand" "$(st last_result)" ok

# --- the kexts never load: three tries, one restart, then it gives up
prepare 26.7.5
setver 26.7.5 25H5; loaded io.github.yusufklncc.IntelXe; rm -f "$ROOT/shutdowns"
for i in 1 2 3; do run --finish; done
check_eq "three failed starts: still trying" "$(st last_result)" repaired
check_eq "three failed starts: one restart only" "$(shutdowns)" 1
run --finish
check_eq "the fourth start gives up" "$(st last_result)" gave-up
check_eq "given up: daemon removed" "$(st daemon)" no

# --- a major update the package does not support: it reports and installs nothing
loaded $ALL; prepare 27.0
grep -q "does not support macOS 27.0" "$G/osupdate.log" && pass "prepare notes that 0.1.5 does not support 27.0" || fail "no note about 27.0"
setver 27.0 26A1; loaded io.github.yusufklncc.IntelXe; rm -f "$ROOT/kc-rebuilt" "$ROOT/shutdowns"
run --finish
check_eq "major update" "$(st last_result)" unsupported
check_eq "major update: support" "$(st last_support)" no
[ -e "$ROOT/kc-rebuilt" ] && fail "major update: install.sh ran" || pass "major update: nothing installed"
check_eq "major update: no restart" "$(shutdowns)" 0
check_eq "major update: daemon removed" "$(st daemon)" no

# --- IntelXe.kext (from the EFI) missing after the update
setver 26.7.5 25H5; loaded $ALL; prepare
setver 26.7.6 25H6; loaded; rm -f "$ROOT/kc-rebuilt"
run --finish
check_eq "IntelXe.kext not loaded" "$(st last_result)" efi-kext
[ -e "$ROOT/kc-rebuilt" ] && fail "efi-kext: install.sh ran" || pass "efi-kext: nothing installed"

# --- the boot guard: set by the update's restarts (cleared once), or after a panic (left alone)
loaded $ALL; prepare
setver 26.7.7 25H7; printf '4 accelerated boots in a row ended inside their first minute' > "$ROOT/nvram/intelxe-accel-off"; rm -f "$ROOT/shutdowns"
printf 'accel4' > "$ROOT/nvram/intelxe-guard"   # a held boot whose kernel writes have not reached flash yet
run --finish
check_eq "boot guard held, no panic" "$(st last_result)" guard-cleared
check_eq "the guard is cleared" "$(cat "$ROOT/nvram/intelxe-accel-off")" ok
check_eq "the count is cleared with it" "$(cat "$ROOT/nvram/intelxe-guard")" ok
check_eq "restart after clearing the guard" "$(shutdowns)" 1
run --finish
check_eq "after the restart" "$(st last_result)" ok
prepare
setver 26.7.8 25H8; printf 'held' > "$ROOT/nvram/intelxe-accel-off"; sleep 1; : > "$ROOT/Library/Logs/DiagnosticReports/Kernel-2026-11-13-010203.panic"
run --finish
check_eq "boot guard held after a panic" "$(st last_result)" ok
check_eq "the guard stays after a panic" "$(cat "$ROOT/nvram/intelxe-accel-off")" held
check_eq "the result records the guard" "$(st last_boot_guard)" held:held
printf 'ok' > "$ROOT/nvram/intelxe-accel-off"; rm -f "$ROOT/Library/Logs/DiagnosticReports/"*.panic

# --- a damaged kept package is refused
prepare
setver 26.7.9 25H9; loaded io.github.yusufklncc.IntelXe; rm -f "$ROOT/kc-rebuilt"
chmod u+w "$G/package/Library/Extensions/IntelXeFB.kext/Contents/MacOS/IntelXeFB"; echo x >> "$G/package/Library/Extensions/IntelXeFB.kext/Contents/MacOS/IntelXeFB"
run --finish
check_eq "damaged kept package" "$(st last_result)" refused
[ -e "$ROOT/kc-rebuilt" ] && fail "refused: install.sh ran" || pass "refused: nothing installed"

# --- install.sh --apply fails each time: retried, then given up
loaded $ALL; prepare
setver 26.7.10 25H10; loaded io.github.yusufklncc.IntelXe; chmod 555 "$ROOT/Library/Extensions"
run --finish
check_eq "a failed install" "$(st last_result)" retrying
check_eq "a failed install: the daemon stays" "$(st daemon)" yes
run --finish; run --finish
check_eq "three failed installs" "$(st last_result)" gave-up
chmod 755 "$ROOT/Library/Extensions"

# --- cancel
loaded $ALL; prepare
run --cancel
check_eq "cancelled: daemon" "$(st daemon)" no
check_eq "cancelled: record" "$(st prepared)" no
[ -f "$G/osupdate.log" ] && pass "cancel keeps the log" || fail "cancel removed the log"
run --finish
check_eq "a run with nothing prepared changes nothing" "$(st daemon)" no

# --- a package from before --supports-macos (0.1.4's install.sh): its own major version only
if [ -n "${OLD:-}" ] && [ -d "$OLD" ]; then
    echo 0.1.4 > "$B/installed-version"
    run --prepare "$OLD" 27.0 && pass "prepare with an 0.1.4 package" || { fail "prepare with an 0.1.4 package"; cat "$ROOT/last.log"; }
    grep -q -- '--supports-macos)' "$G/package/install.sh" && fail "the 0.1.4 template already has --supports-macos" || pass "the 0.1.4 package has no --supports-macos"
    setver 27.0 26A1; run --finish
    check_eq "0.1.4 package, macOS 27" "$(st last_result)" unsupported
    setver 26.7.10 25H10; run --prepare "$OLD"; setver 26.7.11 25H11; run --finish
    check_eq "0.1.4 package, macOS 26.7.11" "$(st last_support)" ok
else echo "skip  no 0.1.4 template (git show 7ed533b)"; fi

echo "== $fails failure(s)"
exit $fails
