#!/bin/bash
# SPDX-License-Identifier: MIT
# test-install-flow.sh PACKAGE_DIR: runs install.sh's whole flow against a throwaway root, without root and without kmutil:
# install, the same again, a damaged file, repair, an edited allow-list, an earlier release's allow-list, undo (with the version it puts back), uninstall;
# then NVMTLDriver.use-library with a system-volume copy of another release, with system-volume/ssv-install.sh (stubbed
# mount and bless). Each step's --status is checked.
set -uo pipefail
PKG=$(cd "$1" && pwd)
ROOT=$(mktemp -d -t ixeflow)
T=$ROOT/pkg
ditto "$PKG" "$T"
# the same script with its paths under the throwaway root, and what needs root or a real Mac stubbed
sed -E -e "s#([^H])/Library/#\1$ROOT/Library/#g" -e "s#SSV=/System/#SSV=$ROOT/System/#" "$PKG/install.sh" > "$T/install.sh"
perl -0pi -e 's/^set -euo pipefail\n/set -euo pipefail\nid() { echo 0; }\nchown() { :; }\ninstall() { local a=(); while [ \$# -gt 0 ]; do case "\$1" in -o|-g|-m) shift 2;; *) a+=("\$1"); shift;; esac; done; cp "\${a[@]}"; }\n/m; s/^check\(\) \{\n/check() { return 0; }\ncheck_real() {\n/m; s/^rebuild_auxkc\(\) \{\n/rebuild_auxkc() { echo "  ok    (kernel collection step skipped in this test)"; }\nrebuild_real() {\n/m' "$T/install.sh"
chmod +x "$T/install.sh"
mkdir -p "$ROOT/Library/Extensions" "$ROOT/Library/GPUBundles" "$ROOT/Library/Application Support/intel-gpu-macos-driver"
fails=0
state() { "$T/install.sh" --status | sed -n "s/^$1=//p"; }
expect() { # expect WHAT KEY VALUE
    local got; got=$(state "$2")
    if [ "$got" = "$3" ]; then echo "ok    $1: $2=$got"; else echo "FAIL  $1: $2=$got, expected $3"; fails=$((fails + 1)); fi
}
step() { "$T/install.sh" "$1" > "$ROOT/last.log" 2>&1 || { echo "FAIL  $1 exited with an error:"; tail -5 "$ROOT/last.log"; fails=$((fails + 1)); }; }

expect "nothing installed" installed_state absent
expect "nothing installed" installed_version none
step --apply
expect "after --apply" installed_state same
expect "after --apply" installed_version "$(state package_version)"
[ -f "$ROOT/Library/GPUBundles/nvmtl-allow.txt" ] && echo "ok    allow-list installed" || { echo "FAIL  no allow-list"; fails=$((fails + 1)); }
expect "after --apply" guard_daemon yes
plutil -lint "$ROOT/Library/LaunchDaemons/io.github.yusufklncc.intelxe.guard.plist" >/dev/null && echo "ok    guard daemon plist is valid" || { echo "FAIL  guard daemon plist"; fails=$((fails + 1)); }
sh -n "$ROOT/Library/Application Support/intel-gpu-macos-driver/guard-clear.sh" && echo "ok    guard script parses" || { echo "FAIL  guard script"; fails=$((fails + 1)); }

K="$ROOT/Library/Extensions/IntelXeFB.kext/Contents/MacOS/IntelXeFB"
echo damaged >> "$K"
expect "a kext damaged" installed_state different
expect "a kext damaged" items_different 1
step --repair
expect "after --repair" installed_state same
grep -q "kernel collection step" "$ROOT/last.log" && echo "ok    repair of a kext rebuilds the collection" || { echo "FAIL  repair did not rebuild the collection"; fails=$((fails + 1)); }

M=$(find "$ROOT/Library/GPUBundles/nvmtl" -type f | head -1)
echo damaged >> "$M"
step --repair
expect "after a Metal file repair" installed_state same
grep -q "kernel collection step" "$ROOT/last.log" && { echo "FAIL  repair of a Metal file rebuilt the collection"; fails=$((fails + 1)); } || echo "ok    repair of a Metal file leaves the collection"

echo "+ my.app" >> "$ROOT/Library/GPUBundles/nvmtl-allow.txt"
touch "$ROOT/Library/GPUBundles/nvmtl/ssv-component.plist"
expect "an edited allow-list and the record of a system-volume copy are not differences" installed_state same
step --repair
grep -q "my.app" "$ROOT/Library/GPUBundles/nvmtl-allow.txt" && echo "ok    repair keeps an edited allow-list" || { echo "FAIL  repair lost the allow-list"; fails=$((fails + 1)); }
rm "$ROOT/Library/GPUBundles/nvmtl/ssv-component.plist"

backup=$(state backup)
step --repair
[ "$(state backup)" = "$backup" ] && echo "ok    repair leaves the last backup" || { echo "FAIL  repair changed the backup"; fails=$((fails + 1)); }

step --undo
expect "after --undo (there was nothing before)" installed_state absent
expect "after --undo" installed_version none
expect "after --undo (there was no guard daemon before)" guard_daemon no
step --apply
# undo puts back the version recorded with its backup (here an earlier version, written by hand)
echo 0.0.9-test > "$ROOT/Library/Application Support/intel-gpu-macos-driver/installed-version"
step --apply
expect "after --apply over 0.0.9-test" installed_version "$(state package_version)"
step --undo
expect "after --undo of that install" installed_version 0.0.9-test
expect "after --undo of that install" installed_state same
step --apply
step --uninstall
expect "after --uninstall" installed_state absent
expect "after --uninstall" guard_daemon no
expect "after --uninstall" installed_version none
[ -d "$backup" ] && echo "ok    uninstall keeps backups" || { echo "FAIL  backups are gone"; fails=$((fails + 1)); }

# the allow-list an earlier release installed is replaced, one the user edited is kept
A="$ROOT/Library/GPUBundles/nvmtl-allow.txt"; mkdir -p "$ROOT/Library/GPUBundles"
printf '%s\n' "# B580: which processes get the B580 Metal device (-Name denies, !Name = only when armed; NVMTL_ENABLE=1 bypasses)" \
    "# 4 Nov Tahoe: Safari's GPU process restarted 16 times in 2 min on the plugin (black page content): kept off until fixed" \
    -com.apple.WebKit.GPU -com.apple.dock.extra mtl_smoke '!WindowServer' '!*' > "$A"
step --apply
cmp -s "$A" "$T/Library/GPUBundles/nvmtl-allow.txt" && echo "ok    apply replaces 0.1.1's allow-list" || { echo "FAIL  0.1.1's allow-list was kept"; fails=$((fails + 1)); }
echo "+ my.app" >> "$A"
step --apply
grep -q "my.app" "$A" && echo "ok    apply keeps an edited allow-list" || { echo "FAIL  apply replaced an edited allow-list"; fails=$((fails + 1)); }
step --uninstall
grep -q "remove IntelXe.kext" "$ROOT/last.log" && echo "ok    uninstall says to remove the kext from the EFI" || { echo "FAIL  no EFI note"; fails=$((fails + 1)); }

# The system-volume copy and NVMTLDriver.use-library (0.1.6): --apply makes the file for a copy of another release and
# records that it did; system-volume/ssv-install.sh --apply removes it once it has installed this package's copy (a file
# the user made stays); a later --apply does not make it again while the new copy waits for the restart.
SV=$(sed -n 's/^SSV=\([^ ]*\).*/\1/p' "$T/install.sh")   # the booted snapshot's copy, where the test's install.sh looks
U=$ROOT/Library/GPUBundles/NVMTLDriver.use-library
BKD="$ROOT/Library/Application Support/intel-gpu-macos-driver"
LAYOUT=$PKG/system-volume/system-layout/NVMTLDriver.bundle
old_copy() { rm -rf "$SV"; ditto "$LAYOUT" "$SV"; echo "another release" >> "$SV/Contents/Frameworks/nvmtl/libvulkan_intel.dylib"; }
check_file() { # check_file WHAT present|absent PATH
    if { [ "$2" = present ] && [ -e "$3" ]; } || { [ "$2" = absent ] && [ ! -e "$3" ]; }; then echo "ok    $1: ${3##*/} $2"
    else echo "FAIL  $1: ${3##*/} not $2"; fails=$((fails + 1)); fi
}
if [ -f "$PKG/system-volume/ssv-install.sh" ] && [ -d "$LAYOUT" ]; then
    # ssv-install.sh with its paths under the throwaway root; the mount, bless, the root checks and preflight stubbed.
    # The copy goes to $ROOT/mnt (the system volume behind a new snapshot), not to the booted copy: a restart moves it.
    sed -E -e "s#^(MARK|USELIB)=/Library/#\1=$ROOT/Library/#" -e "s#^BK=\"/Library/#BK=\"$ROOT/Library/#" \
        -e "s#local L=/Library/#local L=$ROOT/Library/#" -e "s#^MNT=.*#MNT=$ROOT/mnt#" "$PKG/system-volume/ssv-install.sh" > "$T/system-volume/ssv-install.sh"
    perl -0pi -e 's/^set -uo pipefail\n/set -uo pipefail\nid() { echo 0; }\nchown() { :; }\nmount() { :; }\numount() { :; }\nbless() { :; }\n/m; s/^preflight\(\) \{\n/preflight() { return 0; }\npreflight_real() {\n/m; s/^root_device\(\) \{\n/root_device() { SYSDEV=disk9s9; }\nroot_device_real() {\n/m' "$T/system-volume/ssv-install.sh"
    mkdir -p "$ROOT/mnt/System/Library/Extensions"
    ssv() { "$T/system-volume/ssv-install.sh" --apply > "$ROOT/ssv.log" 2>&1 || { echo "FAIL  ssv-install.sh --apply exited with an error:"; tail -5 "$ROOT/ssv.log"; fails=$((fails + 1)); }; }
    restart() { rm -rf "$SV"; ditto "$ROOT/mnt/System/Library/Extensions/NVMTLDriver.bundle" "$SV"; }

    step --apply
    expect "no system-volume copy: nothing changes for it" use_library no
    expect "no system-volume copy" system_volume_state absent
    step --uninstall

    old_copy
    expect "a copy of another release" system_volume_state other
    step --apply
    expect "--apply over a copy of another release" use_library yes
    expect "--apply over a copy of another release" use_library_by install
    ssv
    check_file "ssv-install.sh --apply of this package's copy" absent "$U"
    check_file "ssv-install.sh --apply of this package's copy" absent "$BKD/use-library-by-install"
    expect "this package's copy in the new snapshot, before the restart" system_volume_state pending
    expect "before the restart" use_library_by none
    step --apply
    check_file "--apply while the new copy waits for the restart" absent "$U"
    restart
    expect "after the restart" system_volume_state same
    expect "after the restart" use_library no

    sleep 1; touch "$U"
    expect "a use-library file made by hand" use_library_by user
    step --apply
    check_file "--apply leaves the file made by hand" present "$U"
    ssv
    check_file "ssv-install.sh leaves the file made by hand" present "$U"
    grep -q "not made by install.sh" "$ROOT/ssv.log" && echo "ok    ssv-install.sh says why it kept it" || { echo "FAIL  ssv-install.sh did not say why"; fails=$((fails + 1)); }
    rm -f "$U"; step --uninstall

    # 0.1.5's install.sh recorded the file only in its backup (created-use-library); this release's --apply takes it over
    old_copy; step --apply; rm -f "$BKD/use-library-by-install"
    expect "a use-library file of 0.1.5's install.sh" use_library_by install
    step --apply
    expect "--apply over 0.1.5's file" use_library_by install
    check_file "--apply over 0.1.5's file" present "$BKD/use-library-by-install"
    ssv
    check_file "ssv-install.sh after 0.1.5's install.sh" absent "$U"

    # an earlier --apply's file while the copy is this package's already (installed by an earlier ssv-install.sh):
    # --apply removes it, --undo puts it back
    restart; : > "$U"; : > "$BKD/use-library-by-install"
    step --apply
    check_file "--apply with this package's copy installed" absent "$U"
    step --undo
    check_file "--undo of that" present "$U"
    expect "--undo of that" use_library_by install
    step --uninstall
    check_file "--uninstall" absent "$U"
    check_file "--uninstall" absent "$BKD/use-library-by-install"

    # --undo of the --apply that made it removes it and its record
    old_copy; step --apply; step --undo
    check_file "--undo of the --apply that made it" absent "$U"
    check_file "--undo of the --apply that made it" absent "$BKD/use-library-by-install"
    rm -rf "$SV"
else
    echo "skip  the system-volume steps: the package has no system-volume/ssv-install.sh and system-layout"
fi

[ -n "$ROOT" ] && rm -rf -- "$ROOT"
echo "== $fails failure(s)"
exit $fails
