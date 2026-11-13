#!/bin/bash
# SPDX-License-Identifier: MIT
# osupdate-guard.sh: IntelXe Manager's macOS update guard (docs/design/DESIGN-os-update-guard.md).
#
#   osupdate-guard.sh --check            the stack as it is now, one "key=value" per line. No root, changes nothing.
#   osupdate-guard.sh --status           the preparation and the last result, "key=value". No root, changes nothing.
#   sudo osupdate-guard.sh --prepare PACKAGE [TARGET] [--no-restart]
#                                        before a macOS update: record what is installed, keep a root-owned copy of
#                                        PACKAGE (the installed release's package) and install a one-shot LaunchDaemon
#                                        that runs --finish at each start until the update has happened
#   sudo osupdate-guard.sh --finish      the LaunchDaemon's run. Same build as recorded: wait (for 14 days at most).
#                                        A new build: check the kexts, the Metal driver, the system-volume copy and the
#                                        boot guard; when the package supports the new macOS and something is missing,
#                                        install the kept package again (install.sh --apply) and restart once; at most
#                                        three tries. A boot guard the update's restarts set is cleared once. Then it
#                                        removes itself. It never touches the EFI.
#   sudo osupdate-guard.sh --cancel      remove the LaunchDaemon, the record and the kept package (log and result stay)
#
# Everything goes to /Library/Application Support/intel-gpu-macos-driver/osupdate/: state (the record), result (the
# last outcome), osupdate.log (the diagnostics report collects it), package/ (the kept copy), tries.
set -uo pipefail

# Tests (app/tests/test-osupdate-guard.sh) run it as a normal user against a throwaway root: OSG_ROOT is honoured only
# then, never when the script runs as root, so a root run always works on the real system with the real tools.
R="" STUB=""
if [ -n "${OSG_ROOT:-}" ] && [ "$(id -u)" != 0 ]; then R=$OSG_ROOT; STUB=$R/stub/; fi
KMUTIL=${STUB:-/usr/bin/}kmutil
NVRAM=${STUB:-/usr/sbin/}nvram
SHUTDOWN=${STUB:-/sbin/}shutdown
WAIT=${OSG_WAIT:-90}; [ -n "$R" ] || WAIT=90   # seconds the kexts get to appear after a start

BK="$R/Library/Application Support/intel-gpu-macos-driver"
G="$BK/osupdate"
STATE="$G/state" RESULT="$G/result" LOG="$G/osupdate.log" TRIES="$G/tries" PKG="$G/package"
LABEL=io.github.yusufklncc.intelxe.osupdate
PLIST="$R/Library/LaunchDaemons/$LABEL.plist"
SV="$R/System/Library/CoreServices/SystemVersion.plist"
EXT="$R/Library/Extensions" GB="$R/Library/GPUBundles"
SSV="$R/System/Library/Extensions/NVMTLDriver.bundle"
AUXKC="$R/Library/KernelCollections/AuxiliaryKernelExtensions.kc"
MAX_TRIES=3 EXPIRE_DAYS=14
# the /Library kexts install.sh puts in the Auxiliary Kernel Collection, as "file bundle-id"; IntelXe.kext comes from the EFI
KEXTS="IntelXeFB:io.github.yusufklncc.IntelXeFB NMXAccel:com.nullmoth.NMXAccel NMXAGDC:com.nullmoth.NMXAGDC"

say() { printf '%s %s\n' "$(date '+%Y-%m-%d %H:%M:%S')" "$*" | tee -a "$LOG"; }
need_root() { [ -n "$R" ] || [ "$(id -u)" = 0 ] || { echo "$1 needs root: sudo $0 $1"; exit 1; }; }
own() { [ -n "$R" ] || chown "$@"; }
plist_get() { /usr/libexec/PlistBuddy -c "Print :$2" "$1" 2>/dev/null; }
os_version() { plist_get "$SV" ProductVersion; }
os_build() { plist_get "$SV" ProductBuildVersion; }
kv() { sed -n "s/^$1=//p" "$2" 2>/dev/null | head -1; }   # kv KEY FILE

# the kexts' state, one word each: loaded, installed (the file is there, the kernel did not load it) or absent
kext_states() {
    local loaded k f id
    loaded=$("$KMUTIL" showloaded --list-only 2>/dev/null || true)
    if [[ "$loaded" == *"io.github.yusufklncc.IntelXe ("* ]]; then echo "kext_IntelXe=loaded"; else echo "kext_IntelXe=absent"; fi
    for k in $KEXTS; do
        f=${k%%:*} id=${k#*:}
        if [[ "$loaded" == *"$id ("* ]]; then echo "kext_$f=loaded"
        elif [ -d "$EXT/$f.kext" ]; then echo "kext_$f=installed"
        else echo "kext_$f=absent"; fi
    done
}

# the boot guard as IntelXeFB left it: ok, armed:<stage> (a boot that has not run its desktop for a minute yet) or held:<why>
guard_state() {
    local off g
    off=$("$NVRAM" intelxe-accel-off 2>/dev/null | cut -f2- || true)
    g=$("$NVRAM" intelxe-guard 2>/dev/null | cut -f2- || true)
    if [ -n "$off" ] && [ "$off" != ok ]; then echo "held:$off"
    elif [ -n "$g" ] && [ "$g" != ok ]; then echo "armed:$g"
    else echo ok; fi
}

check() {
    local ks
    ks=$(kext_states)
    echo "macos_version=$(os_version)"
    echo "macos_build=$(os_build)"
    echo "$ks"
    echo "installed_version=$(cat "$BK/installed-version" 2>/dev/null || echo none)"
    if [ ! -f "$AUXKC" ]; then echo "aux_kc=missing"
    elif grep -aq io.github.yusufklncc.IntelXeFB "$AUXKC" 2>/dev/null; then echo "aux_kc=ours"
    else echo "aux_kc=without-ours"; fi
    [ -d "$GB/NVMTLDriverLib.bundle" ] && [ -d "$GB/nvmtl" ] && echo "plugin=present" || echo "plugin=missing"
    [ -f "$SSV/Contents/MacOS/NVMTLDriver" ] && echo "ssv=present" || echo "ssv=absent"
    [ -f "$GB/nvmtl/ssv-component.plist" ] && echo "ssv_record=yes" || echo "ssv_record=no"
    echo "boot_guard=$(guard_state)"
    # kexts that are installed and not loaded: the collection was not rebuilt for this macOS, or not approved
    echo "not_loaded=$(echo "$ks" | sed -n 's/^kext_\(.*\)=installed$/\1/p' | tr '\n' ',' | sed 's/,$//')"
}

# support VERSION PACKAGE_DIR: ok, untested or no. The package's install.sh says (--supports-macos, from 0.1.5); an
# earlier package is for its own major version only (install.sh --status: package_macos)
support() {
    local v=$1 p=$2 major
    if grep -q -- '--supports-macos)' "$p/install.sh" 2>/dev/null; then "$p/install.sh" --supports-macos "$v"; return; fi
    major=$("$p/install.sh" --status 2>/dev/null | sed -n 's/^package_macos=//p')
    [ -n "$major" ] && [ "${v%%.*}" = "$major" ] && echo ok || echo no
}

package_ok() { (cd "$1" && shasum -a 256 -c SHA256SUMS >/dev/null 2>&1); }

remove_daemon() { rm -f "$PLIST"; }

# write_result RESULT MESSAGE: the outcome for the Manager (its Overview page and the check at login); EXTRA holds more
# "key=value" lines
EXTRA=""
write_result() {
    {
        echo "result=$1"
        echo "when=$(date +%s)"
        echo "from=$(kv from_version "$STATE") ($(kv from_build "$STATE"))"
        echo "to=$(os_version) ($(os_build))"
        echo "tries=$(cat "$TRIES" 2>/dev/null || echo 0)"
        echo "package=$(kv package_version "$STATE")"
        [ -n "$EXTRA" ] && printf '%s\n' "$EXTRA"
        echo "message=$2"
    } > "$RESULT.new" && mv -f "$RESULT.new" "$RESULT"
    chmod 644 "$RESULT"
    say "result: $1: $2"
}

# finish_with RESULT MESSAGE: the outcome, then the daemon, the record and the kept package go; nothing stays to run again
finish_with() {
    write_result "$1" "$2"
    remove_daemon
    rm -rf "$PKG" "$STATE" "$TRIES"
    say "removed $PLIST and the kept package"
}

prepare() {
    local src=${1:-} target="" restart=yes a
    shift || true
    for a in "$@"; do case "$a" in --no-restart) restart=no;; *) target=$a;; esac; done
    [ -n "$src" ] && [ -x "$src/install.sh" ] && [ -f "$src/SHA256SUMS" ] || { echo "not a package (no install.sh and SHA256SUMS): ${src:-none given}"; exit 1; }
    src=$(cd "$src" && pwd)
    package_ok "$src" || { echo "the package's files differ from its SHA256SUMS: nothing prepared"; exit 1; }
    local st pv pm iv
    st=$("$src/install.sh" --status 2>/dev/null)
    pv=$(echo "$st" | sed -n 's/^package_version=//p'); pm=$(echo "$st" | sed -n 's/^package_macos=//p')
    iv=$(cat "$BK/installed-version" 2>/dev/null || echo none)
    [ "$iv" != none ] || { echo "the driver is not installed (no installed-version): nothing to guard"; exit 1; }
    [ "$iv" = "$pv" ] || { echo "the installed driver is $iv, the package is $pv: choose the installed release's package"; exit 1; }
    mkdir -p "$R/Library/LaunchDaemons"
    say "== preparing for a macOS update (from $(os_version) $(os_build)${target:+ to $target})"
    rm -rf "$PKG.new"
    ditto "$src" "$PKG.new" || { rm -rf "$PKG.new"; say "could not copy the package: nothing prepared"; exit 1; }
    xattr -dr com.apple.quarantine "$PKG.new" 2>/dev/null || true
    own -R root:wheel "$PKG.new"; chmod -R go-w "$PKG.new"
    rm -rf "$PKG"; mv "$PKG.new" "$PKG"
    package_ok "$PKG" || { rm -rf "$PKG"; say "the kept copy differs from SHA256SUMS: nothing prepared"; exit 1; }
    # the copy that runs: a root-owned one, not the app bundle's (the app may move, or be replaced by a user)
    install -m 755 "$0" "$G/osupdate-guard.sh.new" 2>/dev/null || { cp "$0" "$G/osupdate-guard.sh.new"; chmod 755 "$G/osupdate-guard.sh.new"; }
    own root:wheel "$G/osupdate-guard.sh.new"; mv -f "$G/osupdate-guard.sh.new" "$G/osupdate-guard.sh"
    {
        echo "prepared_at=$(date +%s)"
        echo "from_version=$(os_version)"
        echo "from_build=$(os_build)"
        echo "target=$target"
        echo "package_version=$pv"
        echo "package_macos=$pm"
        echo "restart=$restart"
        check | sed 's/^/before_/'
    } > "$STATE.new" && mv -f "$STATE.new" "$STATE"
    rm -f "$TRIES" "$RESULT"
    cat > "$PLIST.new" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key><string>$LABEL</string>
    <key>ProgramArguments</key><array><string>/bin/bash</string><string>$G/osupdate-guard.sh</string><string>--finish</string></array>
    <key>RunAtLoad</key><true/>
</dict>
</plist>
PL
    # a file launchd cannot read would never run: then nothing is prepared
    plutil -lint "$PLIST.new" >/dev/null 2>&1 || { rm -f "$PLIST.new" "$STATE"; rm -rf "$PKG"; say "the LaunchDaemon file is not valid: nothing prepared"; exit 1; }
    own root:wheel "$PLIST.new"; chmod 644 "$PLIST.new"; mv -f "$PLIST.new" "$PLIST"
    say "kept $pv's package in $PKG; $PLIST runs the check at the first start of the new macOS"
    case "$(support "${target:-$(os_version)}" "$PKG")" in
        no) say "note: $pv does not support macOS ${target:-$(os_version)}: after that update the guard only reports, it installs nothing";;
        untested) say "note: $pv was not tested on macOS ${target:-$(os_version)}; the guard installs it again if needed";;
    esac
    echo "== prepared"
}

finish() {
    [ -f "$STATE" ] || { remove_daemon; say "nothing prepared: removed the LaunchDaemon"; exit 0; }
    local fb nb now age tries
    fb=$(kv from_build "$STATE"); nb=$(os_build); now=$(date +%s)
    if [ "$nb" = "$fb" ]; then
        age=$(( (now - $(kv prepared_at "$STATE")) / 86400 ))
        if [ "$age" -ge "$EXPIRE_DAYS" ]; then
            finish_with expired "prepared $age days ago and still on $(os_version) ($nb): the preparation is removed; prepare again before the next update"
        else say "still on $(os_version) ($nb): waiting for the update (prepared $age day(s) ago)"; fi
        exit 0
    fi
    tries=$(( $(cat "$TRIES" 2>/dev/null || echo 0) + 1 )); echo "$tries" > "$TRIES"
    say "== macOS was updated: $(kv from_version "$STATE") ($fb) -> $(os_version) ($nb), check $tries of $MAX_TRIES"
    # the kexts of the Auxiliary Kernel Collection load a little after the daemons start
    local c i=0
    while :; do
        c=$(check)
        echo "$c" | grep -q '^not_loaded=$' && break
        [ "$i" -ge "$WAIT" ] && break
        sleep 5; i=$((i + 5))
    done
    echo "$c" | sed 's/^/  /' | tee -a "$LOG" >/dev/null
    local sup notl plugin ssv guard ssvnote efi
    sup=$(support "$(os_version)" "$PKG"); notl=$(echo "$c" | sed -n 's/^not_loaded=//p')
    plugin=$(echo "$c" | sed -n 's/^plugin=//p'); ssv=$(echo "$c" | sed -n 's/^ssv=//p'); guard=$(echo "$c" | sed -n 's/^boot_guard=//p')
    efi=$(echo "$c" | sed -n 's/^kext_IntelXe=//p')
    # the system-volume copy: an update replaces the system volume, and the copy with it; putting it back needs the
    # user (Apple Secure Boot off, a new snapshot): the Manager offers it
    if [ "$(kv before_ssv "$STATE")" = present ] && [ "$ssv" != present ]; then ssvnote=gone; else ssvnote=$ssv; fi
    EXTRA=$(printf 'support=%s\nssv=%s\nboot_guard=%s\nefi_kext=%s\nnot_loaded=%s\nplugin=%s' "$sup" "$ssvnote" "$guard" "$efi" "$notl" "$plugin")
    local from; from=$(kv from_version "$STATE")
    if [ "$sup" = no ]; then
        finish_with unsupported "release $(kv package_version "$STATE") does not support macOS $(os_version): nothing was installed. Install a release that supports it, or go back to macOS $from (Time Machine)."
        exit 0
    fi
    # IntelXe.kext comes from OpenCore, not from /Library: without it the /Library kexts have nothing to attach to, and
    # installing them again would not help
    if [ "$efi" != loaded ]; then
        finish_with efi-kext "IntelXe.kext from the EFI did not load on macOS $(os_version): check the EFI (its Kernel > Add entry, MinKernel and MaxKernel); nothing was installed"
        exit 0
    fi
    # the boot guard holding acceleration off after the update, when it was clear before and no kernel panic was recorded
    # since the preparation: the update's own restarts inside the first minute of a boot count as failed boots. Cleared
    # once (the first check after the update); a panic report, or a second time, leaves it to the user
    local clear=no panics
    panics=$(find "$R/Library/Logs/DiagnosticReports" -name '*.panic' -newer "$STATE" 2>/dev/null | head -3 | tr '\n' ' ')
    case "$guard" in held:*)
        if [ "$(kv before_boot_guard "$STATE")" = ok ] && [ -z "$panics" ] && [ "$tries" = 1 ]; then clear=yes
        else say "the boot guard holds acceleration off (${guard#held:})${panics:+; panic reports since the preparation: $panics}: left to you (the Manager's Clear the boot guard, or Reset NVRAM)"; fi;;
    esac
    local inst repaired=no
    inst=$("$PKG/install.sh" --status 2>/dev/null | sed -n 's/^installed_state=//p')
    if [ -n "$notl" ] || [ "$plugin" != present ] || [ "$inst" != same ]; then
        say "to repair: ${notl:+not loaded: $notl; }Metal driver $plugin; installed files: ${inst:-unknown}"
        if [ "$tries" -gt "$MAX_TRIES" ]; then
            finish_with gave-up "$MAX_TRIES tries did not bring the driver back on macOS $(os_version): see this log; install the package again from the Driver page"
            exit 0
        fi
        if ! package_ok "$PKG"; then
            finish_with refused "the kept package's files differ from its SHA256SUMS: nothing was installed"
            exit 0
        fi
        say "installing release $(kv package_version "$STATE") again: install.sh --apply"
        if "$PKG/install.sh" --apply >> "$LOG" 2>&1; then
            repaired=yes
        elif [ "$tries" -ge "$MAX_TRIES" ]; then
            finish_with gave-up "install.sh --apply failed $tries times on macOS $(os_version): see this log"
            exit 0
        else
            write_result retrying "install.sh --apply failed (try $tries of $MAX_TRIES); the next start tries again"
            exit 0
        fi
    fi
    if [ "$clear" = yes ]; then
        # both values: a held boot on the firmware framebuffer leaves the count to intelxe-accel-off (intelxe-guard=ok), but
        # only in the kernel's copy until a clean restart (IntelXe.kext's bootGuardDecide)
        "$NVRAM" intelxe-accel-off=ok intelxe-guard=ok && say "cleared the boot guard (intelxe-accel-off=ok, intelxe-guard=ok): it was clear before the update and no panic was recorded since"
    fi
    if [ "$repaired" = no ] && [ "$clear" = no ]; then
        local note=""
        [ "$ssvnote" = gone ] && note="; the system-volume copy (Safari on the GPU) is gone: install it again from the Driver page"
        case "$guard" in held:*) note="$note; the boot guard holds acceleration off: clear it on the Overview page once you know why";; esac
        finish_with ok "the driver works on macOS $(os_version)$note"
        exit 0
    fi
    # something changed: the next start checks it (the daemon stays); restart once by itself when that was allowed
    if [ "$repaired" = yes ]; then
        write_result repaired "installed release $(kv package_version "$STATE") again for macOS $(os_version)$([ "$clear" = yes ] && echo " and cleared the boot guard"); the next start checks that it loads"
    else
        write_result guard-cleared "cleared the boot guard, which the update's restarts had set; the next start checks the desktop"
    fi
    if [ "$tries" = 1 ] && [ "$(kv restart "$STATE")" = yes ]; then
        say "restarting once by itself, in one minute"
        "$SHUTDOWN" -r +1 >> "$LOG" 2>&1 || say "could not restart: restart by hand"
    fi
}

status() {
    if [ -f "$STATE" ]; then echo "prepared=yes"; sed 's/^/state_/' "$STATE"; else echo "prepared=no"; fi
    [ -f "$PLIST" ] && echo "daemon=yes" || echo "daemon=no"
    echo "tries=$(cat "$TRIES" 2>/dev/null || echo 0)"
    [ -f "$RESULT" ] && sed 's/^/last_/' "$RESULT"
    echo "log=$LOG"
}

cancel() {
    remove_daemon
    rm -rf "$PKG" "$STATE" "$TRIES"
    say "preparation cancelled: removed $PLIST and the kept package"
}

# the guard's folder, and its log kept short (the last 2000 lines); only for the runs that write
start() {
    need_root "$1"
    mkdir -p "$G"; chmod 755 "$G"
    if [ -f "$LOG" ] && [ "$(wc -l < "$LOG")" -gt 4000 ]; then tail -n 2000 "$LOG" > "$LOG.new" && mv -f "$LOG.new" "$LOG"; fi
}
case "${1:-}" in
    --check) check ;;
    --status) status ;;
    --prepare) start --prepare; shift; prepare "$@" ;;
    --finish) start --finish; finish ;;
    --cancel) start --cancel; cancel ;;
    *) sed -n '3,20p' "$0"; exit 2 ;;
esac
