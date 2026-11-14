#!/bin/bash
# SPDX-License-Identifier: MIT
# ssv-install.sh: the OPTIONAL system-volume component of the IntelXe Metal plugin, so WebKit's GPU process (Safari WebGL)
# can load it. The default install (/Library/GPUBundles/NVMTLDriver.bundle) stays as it is.
#
# Why: com.apple.WebKit.GPUProcess.sb lets that process map code only from /System/Library/{Extensions,...}, /usr/lib and
# the cryptexes; the kernel logs "deny(1) file-map-executable /Library/GPUBundles/NVMTLDriver.bundle/...", Metal gets no
# device and Safari's WebGL contexts have no backend. Apple's own Metal drivers are bundles in /System/Library/Extensions
# that carry their libraries inside: ./system-layout/NVMTLDriver.bundle is that layout (Mesa and the translator in
# Contents/Frameworks/nvmtl, found there by the plugin itself).
#
# How: as OpenCore Legacy Patcher's root patches on macOS 11+ - mount the system volume (not its sealed snapshot)
# writable, copy, bless a new boot snapshot; uninstall = boot the last sealed (Apple) snapshot again.
#
#   ssv-install.sh [--preflight]   read-only checks (the default; no root needed for most of it)
#   sudo ssv-install.sh --apply    install ./system-layout/NVMTLDriver.bundle into the system volume, new boot snapshot
#   sudo ssv-install.sh --uninstall   revert to the last sealed snapshot (removes EVERY root change, ours and any other)
#   ssv-install.sh --check         after a macOS update: is the component missing from the booted snapshot?
# Every change needs a reboot to take effect. Nothing is done without --apply / --uninstall.
# ssv6: ssv5 with Mesa 5b2fba33 (d8860b17 = be1cb088 + 0044 HEVC RDOQ, then + 0045 H.264 BRC slice states, 0051 blorp
# compute clear bounds, 0046 AV1 BRC GOP model, 0047 AV1 two PAK passes); bundle re-signed ad-hoc for the new Frameworks
# hash (plugin code unchanged: eba1965f unsigned-layout build of sc6dn = speccst6 + the two webkit patches), and translator aa356713 in Contents/Frameworks/nvmtl. With the component installed the
# /Library copy is /Library/GPUBundles/NVMTLDriverLib.bundle (NMXAccel nmx-ssv2's fallback); NVMTLDriver.bundle in
# /Library would shadow the system copy (Metal takes the first bundle of that name, /Library/GPUBundles first). The Vulkan loader and the ICD json are
# not needed: the plugin opens Mesa directly and uses them only when that fails.
# Not checkable from here: OpenCore must boot with Misc/Security/SecureBootModel = Disabled (Apple Secure Boot refuses a
# broken seal); the repo's USB and DAILY configs have it, with csr-active-config AwgAAA== (0x803).
set -uo pipefail
H=$(cd "$(dirname "$0")" && pwd)
SRC=$H/system-layout/NVMTLDriver.bundle
NAME=NVMTLDriver.bundle
MARK=/Library/GPUBundles/nvmtl/ssv-component.plist   # data volume: survives snapshots and updates
MNT=/System/Volumes/Update/mnt1                       # where OCLP mounts the system volume
# The package's install.sh --apply makes USELIB when the system-volume copy is from another release (NMXAccel then loads
# the /Library copy) and records that it did in USELIB_REC (0.1.5 and earlier: created-use-library in its last backup).
# --apply removes both once the copy it installed is this package's, so the next boot uses it; a USELIB the user made
# is left alone.
USELIB=/Library/GPUBundles/NVMTLDriver.use-library
BK="/Library/Application Support/intel-gpu-macos-driver"
USELIB_REC="$BK/use-library-by-install"
MODE=${1:---preflight}
red() { printf '\033[31m%s\033[0m\n' "$*"; }; ok() { printf '  ok    %s\n' "$*"; }; bad() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }
warn() { printf '  warn  %s\n' "$*"; }
FAILS=0

uselib_ours() {   # the same test as install.sh's: USELIB is there and install.sh made it (its record is not older)
    [ -e "$USELIB" ] || return 1
    local r="" lb
    if [ -e "$USELIB_REC" ]; then r=$USELIB_REC
    else lb=$(cat "$BK/last-backup" 2>/dev/null); [ -n "$lb" ] && [ -e "$lb/created-use-library" ] && r=$lb/created-use-library; fi
    [ -n "$r" ] && ! [ "$USELIB" -nt "$r" ]
}
# after the copy: remove USELIB when install.sh made it and the copy now on the system volume ($1) is this package's,
# with the Mesa of the /Library part (preflight refused another); otherwise say what stays
uselib_after_apply() {
    [ -e "$USELIB" ] || return 0
    local L=/Library/GPUBundles/nvmtl/libvulkan_intel.dylib
    if ! cmp -s "$1/Contents/MacOS/NVMTLDriver" "$SRC/Contents/MacOS/NVMTLDriver" ||
       ! cmp -s "$1/Contents/Frameworks/nvmtl/libvulkan_intel.dylib" "$L"; then
        warn "$USELIB kept: the copy on the system volume does not match this package and the /Library part"
    elif uselib_ours; then
        rm -f "$USELIB" "$USELIB_REC" && ok "removed $USELIB (install.sh --apply made it for the older copy): after the restart Metal loads the system-volume copy"
    else
        warn "$USELIB kept: it was not made by install.sh. While it is there NMXAccel loads the /Library copy; to use the system-volume copy: sudo rm $USELIB, then restart"
    fi
}
root_device() {   # the booted snapshot's device (diskXsYs1) and its system volume (diskXsY)
    SNAPDEV=$(diskutil info / | awk -F': *' '/Device Node/{print $2; exit}'); SNAPDEV=${SNAPDEV#/dev/}
    SYSDEV=$(printf '%s' "$SNAPDEV" | sed -E 's/s[0-9]+$//')
}
preflight() {
    echo "== preflight ($(sw_vers -productVersion) $(sw_vers -buildVersion), $(uname -m))"
    [ "$(uname -m)" = x86_64 ] || bad "not x86_64"
    # SIP: CSR_ALLOW_UNAUTHENTICATED_ROOT (0x800) is what lets a modified system volume boot
    local csr; csr=$(nvram csr-active-config 2>/dev/null | awk '{print $2}')
    local v=0; if [ -n "$csr" ]; then v=$(printf '%s' "$csr" | perl -ne 's/%([0-9a-f]{2})/chr(hex $1)/ge; my @b=unpack("C4",$_."\0\0\0\0"); print $b[0]|($b[1]<<8)|($b[2]<<16)|($b[3]<<24)'); fi
    printf '        csr-active-config 0x%x (%s)\n' "$v" "$(csrutil status 2>/dev/null | head -1)"
    [ $((v & 0x800)) -ne 0 ] && ok "authenticated-root disabled (0x800)" || bad "authenticated-root is ENABLED (csr 0x800 not set): a modified system volume would not boot"
    # AMFI: our ad-hoc signed code in a system path, loaded by Apple processes with library validation
    local ba; ba=$(sysctl -n kern.bootargs 2>/dev/null)
    case "$ba" in *amfi_get_out_of_my_way=1*|*amfi_get_out_of_my_way=0x1*|*amfi=0x80*|*amfi=128*) ok "AMFI relaxed in boot-args ($ba)";;
        *) bad "boot-args have no amfi_get_out_of_my_way=1 / amfi=0x80: Apple processes would refuse the ad-hoc signed plugin";; esac
    # FileVault: OCLP refuses root patching with it on (the system volume cannot be mounted writable then)
    fdesetup status 2>/dev/null | grep -q "FileVault is On" && bad "FileVault is on" || ok "FileVault off"
    # the boot volume, its snapshot, the rollback point
    root_device
    local sealed; sealed=$(diskutil info / | awk -F': *' '/Sealed/{print $2; exit}')
    echo "        booted snapshot device $SNAPDEV, system volume $SYSDEV, sealed: ${sealed:-?}"
    [ -n "$SNAPDEV" ] && [ "$SNAPDEV" != "$SYSDEV" ] && ok "booted from a snapshot of $SYSDEV" || bad "cannot find the system volume behind / ($SNAPDEV)"
    case "$sealed" in Yes*) ok "the booted snapshot is Apple's sealed one (the rollback point is the one booted now)";;
        Broken*) warn "the booted snapshot is already modified (another root patch?): --uninstall reverts those changes too";;
        *) warn "seal state unknown";; esac
    echo "        snapshots of /:"; diskutil apfs listSnapshots / 2>/dev/null | grep -E "Name:|XID:|Purgeable" | sed 's/^/          /'
    bless --info --getBoot 2>/dev/null | sed 's/^/        bless boot: /'
    # space: the bundle (~40 MB) plus a snapshot's metadata
    local free; free=$(df -k / | awk 'NR==2{print int($4/1024)}')
    [ "${free:-0}" -ge 1024 ] && ok "free space ${free} MB in the container" || bad "less than 1 GB free (${free} MB)"
    # the component itself
    [ -d "$SRC" ] && ok "component present: $SRC" || bad "no $SRC"
    if [ -d "$SRC" ]; then
        (cd "$H" && shasum -a 256 -c SHA256SUMS 2>/dev/null | grep "system-layout" | grep -v ': OK$') && bad "system-layout files differ from SHA256SUMS" || ok "system-layout matches SHA256SUMS"
        for m in "$SRC/Contents/MacOS/NVMTLDriver" "$SRC/Contents/MacOS/libnvmtl_translate.dylib" "$SRC"/Contents/Frameworks/nvmtl/*.dylib; do
            "$H/sigcheck" "$m" >/dev/null 2>&1 || bad "not signed: $m"; done
        ok "code signatures present"
        # the two Mesa copies (system-layout Frameworks and /Library/GPUBundles/nvmtl) must be the same file
        local lm sm; lm=$(shasum -a 256 /Library/GPUBundles/nvmtl/libvulkan_intel.dylib 2>/dev/null | cut -c1-64)
        sm=$(shasum -a 256 "$SRC/Contents/Frameworks/nvmtl/libvulkan_intel.dylib" | cut -c1-64)
        [ "$lm" = "$sm" ] && ok "/Library Mesa = system-layout Mesa (${sm:0:12})" || bad "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib (${lm:0:12}) differs from the system-layout Mesa (${sm:0:12}): install the /Library part first"
    fi
    # the kernel side: NMXAccel (nmx-ssv) probes /System/Library/Extensions/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver
    # at start and publishes where Metal loads the plugin from (NMXMetalPluginFrom)
    local mp from; mp=$(ioreg -r -c NMXAccel -d 1 -w0 2>/dev/null | awk -F'= ' '/"MetalPluginName"/{print $2; exit}')
    from=$(ioreg -r -c NMXAccel -d 1 -w0 2>/dev/null | awk -F'= ' '/"NMXMetalPluginFrom"/{print $2; exit}')
    echo "        NMXAccel MetalPluginName = $mp, NMXMetalPluginFrom = ${from:-(absent)}"
    if [ -z "$from" ]; then bad "this NMXAccel does not probe /System/Library/Extensions (no NMXMetalPluginFrom): Metal would keep loading the /Library copy"
    else ok "NMXAccel probes /System/Library/Extensions at start (now: $from; after --apply and a reboot it reads \"system volume\")"; fi
    # Metal's bundle lookup (IOGPU +[IOGPUMetalDevice registerAcceleratorService:]) takes the FIRST <name>.bundle it finds,
    # /Library/GPUBundles before /System/Library/Extensions, and does not fall back when it cannot load it: with both
    # present every process (WindowServer too) loads the /Library copy and WebKit's GPU process fails on it
    # ("CFBundle: Error loading /Library/GPUBundles/NVMTLDriver.bundle ... dlopen", 2026-10-30 03:16)
    [ -e /Library/GPUBundles/$NAME ] && bad "/Library/GPUBundles/$NAME exists: it would shadow the system copy (install the /Library copy as NVMTLDriverLib.bundle first)"
    [ -d /Library/GPUBundles/NVMTLDriverLib.bundle ] && ok "/Library fallback NVMTLDriverLib.bundle present" || warn "no /Library/GPUBundles/NVMTLDriverLib.bundle: NMXAccel (nmx-ssv2) has no fallback if the system copy goes"
    # already installed?
    [ -e "/System/Library/Extensions/$NAME" ] && warn "/System/Library/Extensions/$NAME exists in the booted snapshot" || ok "not installed in the booted snapshot"
    echo "== preflight: $FAILS failure(s)"
    return $FAILS
}
apply() {
    [ "$(id -u)" = 0 ] || { red "--apply needs root (sudo)"; exit 1; }
    preflight || { red "preflight failed: nothing changed"; exit 1; }
    root_device
    echo "== rollback point (write it down): the last sealed snapshot of $SYSDEV; revert with:"
    echo "     sudo $0 --uninstall      (= mount $SYSDEV at $MNT; bless --mount $MNT --bootefi --last-sealed-snapshot; reboot)"
    mkdir -p "$MNT"
    mount -o nobrowse -t apfs "/dev/$SYSDEV" "$MNT" || { red "mount of /dev/$SYSDEV failed: nothing changed"; exit 1; }
    trap 'umount "$MNT" 2>/dev/null' EXIT
    local dst="$MNT/System/Library/Extensions/$NAME"
    rm -rf "$dst.new" && ditto "$SRC" "$dst.new" || { red "copy failed"; rm -rf "$dst.new"; exit 1; }
    chown -R root:wheel "$dst.new"; find "$dst.new" -type d -exec chmod 755 {} +; find "$dst.new" -type f -exec chmod 644 {} +
    find "$dst.new" -type f \( -path '*/Contents/MacOS/*' -o -name '*.dylib' \) -exec chmod 755 {} +
    rm -rf "$dst.old"; [ -e "$dst" ] && mv "$dst" "$dst.old"; mv "$dst.new" "$dst"; rm -rf "$dst.old"
    # caches: a GPU bundle is dlopen'd by Metal by path; it is in no kernel collection and not in the dyld shared cache, so
    # nothing is rebuilt (OCLP runs kmutil only when it adds kexts)
    bless --folder "$MNT/System/Library/CoreServices" --bootefi --create-snapshot || { red "bless --create-snapshot FAILED: the change is NOT bootable as a snapshot; run --uninstall"; exit 1; }
    /usr/bin/defaults write "${MARK%.plist}" build "$(sw_vers -buildVersion)"
    /usr/bin/defaults write "${MARK%.plist}" sha "$(shasum -a 256 "$SRC/Contents/MacOS/NVMTLDriver" | cut -c1-64)"
    /usr/bin/defaults write "${MARK%.plist}" date "$(date +%FT%T)"
    # its Mesa: install.sh --status tells "installed, waiting for the restart" from "another release" by it
    /usr/bin/defaults write "${MARK%.plist}" mesa "$(shasum -a 256 "$dst/Contents/Frameworks/nvmtl/libvulkan_intel.dylib" | cut -c1-64)"
    chmod 644 "$MARK"   # defaults writes it 0600 as root; --check runs as the user
    uselib_after_apply "$dst"
    echo "== installed: $dst, new boot snapshot blessed; reboot to use it. Marker $MARK"
}
uninstall() {
    [ "$(id -u)" = 0 ] || { red "--uninstall needs root (sudo)"; exit 1; }
    # Before the system copy goes, a /Library copy must be there under BOTH names NMXAccel may fall back to:
    # NVMTLDriverLib (nmx-ssv2) and NVMTLDriver (earlier NMXAccel builds). After the revert nothing is shadowed any more.
    local L=/Library/GPUBundles
    if [ ! -d "$L/NVMTLDriverLib.bundle" ]; then
        [ -d "$L/NVMTLDriver.bundle" ] || { red "no /Library fallback (neither NVMTLDriverLib.bundle nor NVMTLDriver.bundle): refusing to remove the system copy"; exit 1; }
        ditto "$L/NVMTLDriver.bundle" "$L/NVMTLDriverLib.bundle" && chown -R root:wheel "$L/NVMTLDriverLib.bundle"
    fi
    if [ ! -d "$L/NVMTLDriver.bundle" ]; then ditto "$L/NVMTLDriverLib.bundle" "$L/NVMTLDriver.bundle" && chown -R root:wheel "$L/NVMTLDriver.bundle"; fi
    echo "== /Library fallback: NVMTLDriverLib.bundle and NVMTLDriver.bundle ($(shasum -a 256 "$L/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver" | cut -c1-12))"
    root_device
    mkdir -p "$MNT"
    mount -o nobrowse -t apfs "/dev/$SYSDEV" "$MNT" || { red "mount of /dev/$SYSDEV failed"; exit 1; }
    trap 'umount "$MNT" 2>/dev/null' EXIT
    bless --mount "$MNT" --bootefi --last-sealed-snapshot || { red "bless --last-sealed-snapshot FAILED"; exit 1; }
    rm -f "$MARK"
    echo "== the last sealed snapshot is blessed: reboot to boot it (every root change is gone, ours and any other)"
}
check() {   # after a macOS update the new sealed snapshot does not have the component
    if [ ! -f "$MARK" ]; then echo "component not installed (no $MARK)"; return 0; fi
    local b; b=$(/usr/bin/defaults read "${MARK%.plist}" build 2>/dev/null)
    if [ ! -e "/System/Library/Extensions/$NAME" ]; then
        echo "NEEDS RE-APPLY: installed for build $b, now $(sw_vers -buildVersion) and /System/Library/Extensions/$NAME is gone (a macOS update replaced the snapshot): sudo $0 --apply"
        return 2
    fi
    local cur; cur=$(shasum -a 256 "/System/Library/Extensions/$NAME/Contents/MacOS/NVMTLDriver" | cut -c1-64)
    local want; want=$(/usr/bin/defaults read "${MARK%.plist}" sha 2>/dev/null)
    [ "$cur" = "$want" ] || { echo "component present but not the one recorded (${cur:0:12} vs ${want:0:12})"; return 3; }
    local from; from=$(ioreg -r -c NMXAccel -d 1 -w0 2>/dev/null | awk -F'= ' '/"NMXMetalPluginFrom"/{print $2; exit}')
    echo "component present, build $b, plugin ${cur:0:12}; NMXAccel loads Metal's plugin from: ${from:-?}"
    if [ -e "$USELIB" ]; then
        if uselib_ours; then echo "NOT USED: $USELIB (install.sh made it for an older copy) makes NMXAccel load the /Library copy: sudo $0 --apply installs this package's copy and removes it"
        else echo "NOT USED: $USELIB (made by hand) makes NMXAccel load the /Library copy: sudo rm $USELIB, then restart"; fi
        return 5
    fi
    if [ -e "/Library/GPUBundles/$NAME" ]; then echo "SHADOWED: /Library/GPUBundles/$NAME exists - Metal loads it before the system copy (WebKit's GPU process cannot)"; return 4; fi
}
case "$MODE" in
    --preflight) preflight;;
    --apply) apply;;
    --uninstall) uninstall;;
    --check) check;;
    *) echo "usage: $0 [--preflight|--apply|--uninstall|--check]"; exit 2;;
esac
