#!/bin/bash
# SPDX-License-Identifier: MIT
# install.sh: the /Library part of intel-gpu-macos-driver 0.1.5 for macOS 26 Tahoe (Intel Arc B580, 8086:e20b).
#
#   ./install.sh               check only (the default): macOS version, SIP, AMFI boot-arg, the card, IntelXe.kext,
#                              the package's checksums and code signatures. Changes nothing.
#   sudo ./install.sh --apply  back up what it replaces, copy the kexts and the Metal driver into /Library, rebuild the
#                              Auxiliary Kernel Collection. Reboot afterwards.
#   sudo ./install.sh --undo   put the last backup back and rebuild the Auxiliary Kernel Collection. Reboot afterwards.
#   ./install.sh --status      what is installed compared with this package, one "key=value" per line. Changes nothing.
#   sudo ./install.sh --repair put back installed files that differ from this package; the last backup stays as it is.
#   sudo ./install.sh --uninstall  remove the kexts and the Metal driver from /Library, rebuild the Auxiliary Kernel
#                              Collection. Backups stay. Remove IntelXe.kext from your EFI before you reboot.
#   ./install.sh --supports-macos VERSION  ok, untested or no: whether this package is for that macOS. Changes nothing.
#
# Not done here, because they live in your OpenCore EFI (see README-INSTALL.md): EFI/OC/Kexts/IntelXe.kext and
# EFI/OC/ACPI/SSDT-ARCOFF.aml with their config.plist entries, SIP and the AMFI boot-arg. The optional system-volume copy
# (Safari WebGL) is system-volume/ssv-install.sh. Backups go to /Library/Application Support/intel-gpu-macos-driver/.
#
# Upgrading from a build with the earlier names (the stack was renamed from B580 to IntelXe in 0.1.0): --apply
# backs up and removes /Library/Extensions/B580FB.kext (two framebuffer kexts must never both be in the Auxiliary
# Kernel Collection), and --undo puts it back. Swap B580.kext for IntelXe.kext in your EFI before you reboot.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
VER=0.1.5
MACOS_MAJOR=26   # the macOS this package is built for (NMXAccel differs: 26 Tahoe, 15 Sequoia)
BK="/Library/Application Support/intel-gpu-macos-driver"
KEXTS=(IntelXeFB NMXAccel NMXAGDC)
OLD_KEXTS=(B580FB)   # the same kexts under their earlier names: removed by --apply, restored by --undo
# the cards this package supports: "PCI device id (vendor 8086), name (its spaces as _)[, untested]"; the same list as
# data/platforms.json "supported" (tools/parity/devices.py checks every copy in the repository)
CARDS=("e20b B580" "e20c B570 untested" "e211 Pro_B60 untested" "e212 Pro_B50 untested")
GB=/Library/GPUBundles
SSV=/System/Library/Extensions/NVMTLDriver.bundle   # system-volume/ssv-install.sh's copy
USELIB=$GB/NVMTLDriver.use-library                  # NMXAccel: use the /Library copy even when $SSV exists
GUARD_LABEL=io.github.yusufklncc.intelxe.guard
GUARD_PLIST=/Library/LaunchDaemons/$GUARD_LABEL.plist
GUARD_SH="$BK/guard-clear.sh"
FAILS=0
ok()  { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; FAILS=$((FAILS + 1)); }
warn(){ printf '  warn  %s\n' "$*"; }

# the system-volume copy is installed and its Mesa is not this package's (another release)
ssv_stale() {
    [ -f "$SSV/Contents/MacOS/NVMTLDriver" ] || return 1
    ! cmp -s "$SSV/Contents/Frameworks/nvmtl/libvulkan_intel.dylib" "$H/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
}

# macos_support VERSION: whether this package supports that macOS version: ok, untested (its major, another minor) or no.
# check() and --supports-macos (IntelXe Manager's macOS update guard asks it before and after an update) both use it.
macos_support() {
    case "$MACOS_MAJOR:$1" in
        26:26.7|26:26.7.*) echo ok;;
        26:26.*) echo untested;;
        15:15.*) echo ok;;
        *) echo no;;
    esac
}

check() {
    echo "== intel-gpu-macos-driver $VER: checks"
    local v; v=$(sw_vers -productVersion)
    case "$MACOS_MAJOR:$(macos_support "$v")" in
        26:ok) ok "macOS $v";;
        26:untested) warn "macOS $v: this package was tested on 26.7.1 only";;
        15:ok) ok "macOS $v (note: the Sequoia package was built from the same sources but not re-tested on Sequoia)";;
        *) bad "macOS $v: this package is for macOS $MACOS_MAJOR (use the other package)";;
    esac
    [ "$(uname -m)" = x86_64 ] && ok "x86_64" || bad "not an x86_64 Mac"
    local sip; sip=$(csrutil status 2>/dev/null || true)
    case "$sip" in *disabled*|*Custom\ Configuration*) ok "SIP relaxed ($(echo "$sip" | head -1))";; *) bad "SIP is enabled: csr-active-config 0x803 (AwgAAA==) in OpenCore, see README-INSTALL.md, stage 2";; esac
    local ba; ba=$(nvram boot-args 2>/dev/null | cut -f2- || true)
    case "$ba" in *amfi_get_out_of_my_way=1*|*amfi_get_out_of_my_way=0x1*|*amfi=0x80*|*amfi=128*) ok "AMFI relaxed in boot-args";;
        *) bad "no AMFI boot-arg (amfi_get_out_of_my_way=0x1): WindowServer would refuse the unsigned Metal driver";; esac
    local old; old=$(printf '%s\n' $ba | grep -E '^-?b580' | tr '\n' ' ' || true)   # intelxe-rename: keep
    [ -n "$old" ] && warn "boot-args with their earlier names: ${old}- they still work in this release; rename them (README-INSTALL.md, Upgrading)"
    local pci loaded c id name untested seen="" ids="" names=""; pci=$(ioreg -r -c IOPCIDevice -l 2>/dev/null || true)
    for c in "${CARDS[@]}"; do   # the first card found, in list order; ioreg shows the id little-endian: e20b = <0be20000>
        read -r id name untested <<< "$c"; name=${name//_/ }
        ids+=${ids:+/}$id; names+=${names:+/}$name
        [ -z "$seen" ] && [[ "$pci" == *"\"device-id\" = <${id:2:2}${id:0:2}0000>"* ]] || continue
        seen=1; ok "Arc $name (8086:$id) on the PCI bus${untested:+ (supported; not yet tested by the project)}"
    done
    [ -n "$seen" ] || bad "no Arc $names (8086:$ids) found"
    loaded=$(kmutil showloaded 2>/dev/null || true)
    if [[ "$loaded" == *"io.github.yusufklncc.IntelXe ("* ]]; then
        ok "IntelXe.kext is loaded (from OpenCore)"
    elif [[ "$loaded" == *"org.hackintosh.B580 ("* ]]; then   # intelxe-rename: keep
        warn "B580.kext (the earlier name) is loaded: replace it with EFI/OC/Kexts/IntelXe.kext in your OpenCore EFI before you reboot (README-INSTALL.md, Upgrading); the new /Library part does not work with it"
    else warn "IntelXe.kext is not loaded: expected on a first install (it goes into the EFI in stage 4 of README-INSTALL.md, after --apply and before the reboot)"; fi
    for k in "${OLD_KEXTS[@]}"; do
        [ -e /Library/Extensions/$k.kext ] && warn "/Library/Extensions/$k.kext (earlier name of ${KEXTS[0]}.kext): --apply backs it up and removes it"
    done
    if ssv_stale; then
        warn "the system-volume copy ($SSV) is from another release: --apply makes NMXAccel use the /Library copy ($USELIB) until you install this package's system-volume copy (README-INSTALL.md, step 3.4)"
    fi
    if xattr -r "$H" 2>/dev/null | grep -q com.apple.quarantine; then
        if [ "$(id -u)" = 0 ]; then xattr -dr com.apple.quarantine "$H" && ok "removed the downloaded-file mark (com.apple.quarantine) from the package"
        else warn "the package carries the downloaded-file mark (com.apple.quarantine): --apply removes it; or run: xattr -dr com.apple.quarantine \"$H\""; fi
    fi
    if (cd "$H" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1); then ok "package files match SHA256SUMS"
    else bad "package files differ from SHA256SUMS (damaged or changed download)"; fi
    local m n=0
    while IFS= read -r m; do "$H/sigcheck" "$m" >/dev/null 2>&1 || { bad "not signed: ${m#$H/}"; n=$((n + 1)); }; done < <(
        find "$H/Library" -type f -perm -u+x \( -path '*/MacOS/*' -o -name '*.dylib' \) )
    [ $n = 0 ] && ok "code signatures present (ad-hoc; OpenCore's IntelXe.kext needs none)"
    echo "== $FAILS problem(s)"
    return $FAILS
}

# allow_earlier: the installed nvmtl-allow.txt is one an earlier release put there (0.1.0 and 0.1.1; 0.1.1's with the
# -NotificationCenter line its guide asked for; the repository's before 0.1.2), not one the user wrote: replace it
allow_earlier() {
    case "$(shasum -a 256 < $GB/nvmtl-allow.txt | cut -c1-64)" in
        bda5a52913d4d38b737677181ae3a6600518a5273d054b7cf3a198f192bd8469|11dd4cd6811a4cda28f9558db412b9d55ae8f9da5c5262e3790388e9ca475b61|\
        9461be74412922ae50689f8dd1cc72e59e78b937c23c7f77948e3105bf1a6c04|4046773b7550de5840d9c8b8d76fa340e574e80e4b1f64cdea52487f628f3ef3) return 0;;
    esac
    return 1
}

rebuild_auxkc() {
    local KC=/Library/KernelCollections/AuxiliaryKernelExtensions.kc K=/System/Library/Kernels/kernel KA=(--allow-missing-kdk) log
    [ -f $K ] && KA+=(--kernel $K)
    log=$(mktemp -t auxkc)
    echo "== rebuilding the Auxiliary Kernel Collection (log: $log)"
    if kmutil create -n aux --volume-root / "${KA[@]}" -B /System/Library/KernelCollections/BootKernelExtensions.kc \
         -S /System/Library/KernelCollections/SystemKernelExtensions.kc --repository /Library/Extensions -A "$KC.new" -z >"$log" 2>&1 \
       && [ -s "$KC.new" ]; then
        mv -f "$KC.new" "$KC"; echo "  ok    $KC"
    else
        rm -f "$KC.new"; echo "  FAIL  kmutil failed; the previous collection is unchanged. See $log"; return 1
    fi
}

put() { # put SRC DST: replace DST with a root-owned copy of SRC
    rm -rf "$2.new"; ditto "$1" "$2.new"; xattr -dr com.apple.quarantine "$2.new" 2>/dev/null || true
    chown -R root:wheel "$2.new"; chmod -R go-w "$2.new"
    rm -rf "$2"; mv "$2.new" "$2"
}

# The boot guard's "healthy" mark reaches flash from here. IntelXeFB clears intelxe-guard about a minute into each boot,
# but on x86 macOS (AppleEFINVRAM) a value the kernel writes after boot stays in memory until a clean shutdown: after a
# power cut the next boot read the guard as set and held acceleration (5 Nov, test builds before 0.1.2). A write through nvram(8)
# from userspace does go to flash. Once per boot, this waits for IntelXeFB to say its guard cleared, then writes it.
guard_install() {
    mkdir -p "$BK" /Library/LaunchDaemons
    cat > "$GUARD_SH" <<'GUARD'
#!/bin/sh
# intel-gpu-macos-driver: writes the boot guard's "cleared" to NVRAM from userspace (see install.sh, guard_install)
i=0
while [ $i -lt 40 ]; do
    # a held boot on the firmware framebuffer (IntelXe.kext 0.1.5 and later): IntelXeFB does not start, nothing to clear;
    # the hold stays in intelxe-accel-off until it is cleared (IntelXe Manager, or nvram intelxe-accel-off=ok intelxe-guard=ok)
    if /usr/sbin/sysctl -n debug.intelxe.bootguard 2>/dev/null | /usr/bin/grep -qx 'display=firmware'; then
        /usr/bin/logger -t intelxe-guard "held boot on the firmware framebuffer: nothing to clear ($(/usr/sbin/sysctl -n debug.intelxe.bootguard 2>/dev/null | /usr/bin/grep -E '^(held_by|reason)=' | /usr/bin/tr '\n' ' '))"
        exit 0
    fi
    g=$(/usr/sbin/ioreg -rd1 -c IntelXeFramebuffer | /usr/bin/sed -n 's/.*"IntelXeFBGuard" = "\(.*\)"/\1/p' | /usr/bin/head -1)
    case "$g" in
        cleared*)
            /usr/sbin/nvram intelxe-guard=ok
            /usr/sbin/nvram intelxe-guard-cleared="$(/bin/date +%Y%m%d-%H%M%S)"   # a new value each boot, so a write happens
            /usr/bin/logger -t intelxe-guard "boot guard cleared in NVRAM ($g)"
            exit 0;;
    esac
    /bin/sleep 15
    i=$((i + 1))
done
/usr/bin/logger -t intelxe-guard "the boot guard did not clear within 10 minutes (IntelXeFBGuard: ${g:-none})"
GUARD
    chmod 755 "$GUARD_SH"
    cat > "$GUARD_PLIST" <<PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$GUARD_LABEL</string>
    <key>ProgramArguments</key><array><string>/bin/sh</string><string>$GUARD_SH</string></array>
    <key>RunAtLoad</key><true/>
</dict>
</plist>
PLIST
    chown root:wheel "$GUARD_PLIST" "$GUARD_SH"; chmod 644 "$GUARD_PLIST"
    echo "  ok    $GUARD_PLIST (writes the boot guard's result to NVRAM once per boot)"
}
guard_remove() {
    [ -e "$GUARD_PLIST" ] || [ -e "$GUARD_SH" ] || return 0
    launchctl bootout system/$GUARD_LABEL 2>/dev/null || true
    rm -f "$GUARD_PLIST" "$GUARD_SH"
    echo "  ok    removed $GUARD_PLIST"
}

apply() {
    [ "$(id -u)" = 0 ] || { echo "--apply needs root: sudo $0 --apply"; exit 1; }
    check || { echo "checks failed: nothing changed"; exit 1; }
    local B; B="$BK/backup-$(date +%Y%m%d-%H%M%S)"
    mkdir -p "$B/Extensions" "$B/GPUBundles"
    for k in "${KEXTS[@]}" "${OLD_KEXTS[@]}"; do [ -e /Library/Extensions/$k.kext ] && ditto /Library/Extensions/$k.kext "$B/Extensions/$k.kext"; done
    for g in NVMTLDriverLib.bundle NVMTLDriver.bundle nvmtl nvmtl-allow.txt; do [ -e $GB/$g ] && ditto $GB/$g "$B/GPUBundles/$g"; done
    echo "$B" > "$BK/last-backup"
    [ -f "$BK/installed-version" ] && cp "$BK/installed-version" "$B/installed-version"   # --undo puts it back
    echo "== backup: $B"
    for k in "${KEXTS[@]}"; do put "$H/Library/Extensions/$k.kext" /Library/Extensions/$k.kext; echo "  ok    /Library/Extensions/$k.kext"; done
    # the earlier names of these kexts: two framebuffer kexts must never both be in the Auxiliary Kernel Collection
    for k in "${OLD_KEXTS[@]}"; do
        if [ -e /Library/Extensions/$k.kext ]; then rm -rf /Library/Extensions/$k.kext; echo "  ok    removed /Library/Extensions/$k.kext (backed up)"; fi
    done
    mkdir -p $GB
    put "$H/Library/GPUBundles/NVMTLDriverLib.bundle" $GB/NVMTLDriverLib.bundle; echo "  ok    $GB/NVMTLDriverLib.bundle"
    # a /Library/GPUBundles/NVMTLDriver.bundle would shadow the system-volume copy (Metal takes the first bundle of that name)
    if [ -e $GB/NVMTLDriver.bundle ]; then rm -rf $GB/NVMTLDriver.bundle; echo "  ok    removed $GB/NVMTLDriver.bundle (backed up)"; fi
    # nvmtl/ssv-component.plist is system-volume/ssv-install.sh's record of an installed system-volume copy: keep it
    local mark=$GB/nvmtl/ssv-component.plist keep=""
    [ -f "$mark" ] && { keep=$(mktemp -t ssvmark); cp -p "$mark" "$keep"; }
    put "$H/Library/GPUBundles/nvmtl" $GB/nvmtl; echo "  ok    $GB/nvmtl"
    [ -n "$keep" ] && { cp -p "$keep" "$mark"; rm -f "$keep"; echo "  keep  $mark (system-volume copy record)"; }
    if [ -e $GB/nvmtl-allow.txt ] && ! cmp -s $GB/nvmtl-allow.txt "$H/Library/GPUBundles/nvmtl-allow.txt" && ! allow_earlier; then
        echo "  keep  $GB/nvmtl-allow.txt (yours differs from the package's; the package's is in $H/Library/GPUBundles)"
    else install -o root -g wheel -m 644 "$H/Library/GPUBundles/nvmtl-allow.txt" $GB/nvmtl-allow.txt; echo "  ok    $GB/nvmtl-allow.txt"; fi
    # a system-volume copy of another release cannot drive these kexts: NMXAccel takes the /Library copy until this
    # package's system-volume copy is installed (system-volume/ssv-install.sh --apply, then remove $USELIB)
    if ssv_stale && [ ! -e "$USELIB" ]; then
        : > "$USELIB"; : > "$B/created-use-library"
        echo "  ok    $USELIB: the system-volume copy is from another release, the /Library copy is used until it is updated"
    fi
    [ -e "$GUARD_PLIST" ] && : > "$B/had-guard-daemon"
    guard_install
    for k in "${OLD_KEXTS[@]}"; do [ -e /Library/Extensions/$k.kext ] && { echo "  FAIL  /Library/Extensions/$k.kext is still there"; exit 1; }; done
    rebuild_auxkc
    echo "$VER" > "$BK/installed-version"
    echo "== done. Reboot (if you are upgrading: with IntelXe.kext in your EFI in place of B580.kext). To go back: sudo $0 --undo"
}

undo() {
    [ "$(id -u)" = 0 ] || { echo "--undo needs root: sudo $0 --undo"; exit 1; }
    local B; B=$(cat "$BK/last-backup" 2>/dev/null || true)
    [ -n "$B" ] && [ -d "$B" ] || { echo "no backup recorded in $BK/last-backup"; exit 1; }
    echo "== restoring $B"
    for k in "${KEXTS[@]}" "${OLD_KEXTS[@]}"; do
        if [ -e "$B/Extensions/$k.kext" ]; then put "$B/Extensions/$k.kext" /Library/Extensions/$k.kext; echo "  ok    restored $k.kext"
        elif [ -e /Library/Extensions/$k.kext ]; then rm -rf /Library/Extensions/$k.kext; echo "  ok    removed $k.kext (there was none before)"; fi
    done
    for g in NVMTLDriverLib.bundle NVMTLDriver.bundle nvmtl nvmtl-allow.txt; do
        if [ -e "$B/GPUBundles/$g" ]; then rm -rf $GB/$g; ditto "$B/GPUBundles/$g" $GB/$g; chown -R root:wheel $GB/$g; echo "  ok    restored $GB/$g"
        else rm -rf $GB/$g; echo "  ok    removed $GB/$g (there was none before)"; fi
    done
    if [ -e "$B/created-use-library" ]; then rm -f "$USELIB"; echo "  ok    removed $USELIB (--apply made it)"; fi
    [ -e "$B/had-guard-daemon" ] || guard_remove
    if [ -f "$B/installed-version" ]; then cp "$B/installed-version" "$BK/installed-version"; else rm -f "$BK/installed-version"; fi
    rebuild_auxkc
    for k in "${OLD_KEXTS[@]}"; do
        [ -e "$B/Extensions/$k.kext" ] && echo "== $k.kext is back: put B580.kext back into your EFI in place of IntelXe.kext before you reboot"
    done
    echo "== done. Reboot."
}

# the installed items that come from this package, as "package path|installed path"; nvmtl-allow.txt is left out because
# it may be edited, and nvmtl/ssv-component.plist is excluded where they are compared because only an install writes it
package_items() {
    for k in "${KEXTS[@]}"; do echo "Library/Extensions/$k.kext|/Library/Extensions/$k.kext"; done
    echo "Library/GPUBundles/NVMTLDriverLib.bundle|$GB/NVMTLDriverLib.bundle"
    echo "Library/GPUBundles/nvmtl|$GB/nvmtl"
}

differs() { ! diff -rq -x ssv-component.plist "$1" "$2" >/dev/null 2>&1; }

status() {
    local installed=none state=absent present=0 changed=0 i p d differing="" missing=""
    [ -f "$BK/installed-version" ] && installed=$(cat "$BK/installed-version")
    while IFS='|' read -r p d; do
        [ -e "$d" ] || { missing="$missing${missing:+,}$d"; continue; }
        present=$((present + 1))
        differs "$H/$p" "$d" && { changed=$((changed + 1)); differing="$differing${differing:+,}$d"; }
    done < <(package_items)
    i=$(package_items | wc -l | tr -d ' ')
    if [ "$present" = 0 ]; then state=absent
    elif [ "$present" != "$i" ]; then state=partial
    elif [ "$changed" = 0 ]; then state=same
    else state=different; fi
    echo "package_version=$VER"
    echo "package_macos=$MACOS_MAJOR"
    echo "installed_version=$installed"
    echo "installed_state=$state"
    echo "items_present=$present/$i"
    echo "items_different=$changed"
    echo "items_differing=$differing"   # installed paths, comma-separated (the app lists them)
    echo "items_missing=$missing"
    [ -s "$BK/last-backup" ] && [ -d "$(cat "$BK/last-backup")" ] && echo "backup=$(cat "$BK/last-backup")" || echo "backup=none"
    [ -e "$USELIB" ] && echo "use_library=yes" || echo "use_library=no"
    [ -f "$SSV/Contents/MacOS/NVMTLDriver" ] && echo "system_volume_copy=yes" || echo "system_volume_copy=no"
    [ -e "$GUARD_PLIST" ] && echo "guard_daemon=yes" || echo "guard_daemon=no"
    echo "macos_support=$(macos_support "$(sw_vers -productVersion)")"
}

repair() {
    [ "$(id -u)" = 0 ] || { echo "--repair needs root: sudo $0 --repair"; exit 1; }
    check || { echo "checks failed: nothing changed"; exit 1; }
    local p d keep kexts=0 n=0 mark=$GB/nvmtl/ssv-component.plist
    mkdir -p $GB /Library/Extensions
    while IFS='|' read -r p d; do
        differs "$H/$p" "$d" || { echo "  same  $d"; continue; }
        keep=""
        [ "$d" = $GB/nvmtl ] && [ -f "$mark" ] && { keep=$(mktemp -t ssvmark); cp -p "$mark" "$keep"; }
        put "$H/$p" "$d"; echo "  ok    $d"
        [ -n "$keep" ] && { cp -p "$keep" "$mark"; rm -f "$keep"; }
        n=$((n + 1))
        case "$d" in /Library/Extensions/*) kexts=$((kexts + 1));; esac
    done < <(package_items)
    [ -e $GB/nvmtl-allow.txt ] || { install -o root -g wheel -m 644 "$H/Library/GPUBundles/nvmtl-allow.txt" $GB/nvmtl-allow.txt; echo "  ok    $GB/nvmtl-allow.txt"; n=$((n + 1)); }
    [ -e "$GUARD_PLIST" ] && [ -x "$GUARD_SH" ] || { guard_install; n=$((n + 1)); }
    [ "$kexts" = 0 ] || rebuild_auxkc
    mkdir -p "$BK"; echo "$VER" > "$BK/installed-version"
    if [ "$n" = 0 ]; then echo "== nothing to repair"; else echo "== done: $n item(s) put back. Reboot."; fi
}

uninstall() {
    [ "$(id -u)" = 0 ] || { echo "--uninstall needs root: sudo $0 --uninstall"; exit 1; }
    local k g
    for k in "${KEXTS[@]}" "${OLD_KEXTS[@]}"; do
        if [ -e /Library/Extensions/$k.kext ]; then rm -rf /Library/Extensions/$k.kext; echo "  ok    removed /Library/Extensions/$k.kext"; fi
    done
    for g in NVMTLDriverLib.bundle NVMTLDriver.bundle nvmtl nvmtl-allow.txt NVMTLDriver.use-library NVMTLDriver.no-low-latency NVMTLDriver.low-latency; do
        if [ -e $GB/$g ]; then rm -rf $GB/$g; echo "  ok    removed $GB/$g"; fi
    done
    guard_remove
    rm -f "$BK/installed-version"
    rebuild_auxkc
    echo "== done. Backups stay in $BK."
    echo "== before you reboot: remove IntelXe.kext (and SSDT-ARCOFF.aml if you want macOS's display driver back) from your EFI and config.plist."
    [ -f "$SSV/Contents/MacOS/NVMTLDriver" ] && echo "== the system-volume copy is still installed: sudo system-volume/ssv-install.sh --uninstall reverts to the last sealed snapshot (it removes EVERY root change, ours and any other)."
    echo "== then reboot."
}

case "${1:-}" in
    ""|--check) check ;;
    --apply) apply ;;
    --undo) undo ;;
    --status) status ;;
    --repair) repair ;;
    --uninstall) uninstall ;;
    --supports-macos) [ -n "${2:-}" ] || { echo "--supports-macos needs a version (26.7.2)"; exit 2; }; macos_support "$2" ;;
    *) sed -n '3,14p' "$0"; exit 2 ;;
esac
