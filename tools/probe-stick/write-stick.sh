#!/bin/sh
# SPDX-License-Identifier: MIT
# write-stick.sh: write the Xe probe stick image to a USB stick, on macOS or Linux. Everything on the stick is erased.
#
#   sh write-stick.sh --list                    the external disks it would accept
#   sh write-stick.sh IMAGE DISK                write IMAGE to DISK (macOS: disk4; Linux: sdb), after checks and after you
#                                               type the disk's name to confirm; then read it back and compare
#   --allow-large                               accept a disk larger than 128 GB (refused by default: likely a backup disk)
#
# It refuses: a disk that is not on USB (macOS: BusProtocol; Linux: the device path) or not removable media (a USB hard
# disk or SSD rather than a stick), a partition instead of a whole disk, the disk the running system boots from, a disk
# image or other virtual disk, a disk smaller than the image or larger than 128 GB, and an IMAGE whose SHA-256 does
# not match the .img.sha256 next to it. These are the rules IntelXe Manager for Windows applies (StickRules), held to
# the same answers by stick-disk-vectors.json (test/disk-rules-test.sh). It needs sudo for the write and the read-back.
set -eu
LC_ALL=C
PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH   # diskutil and plutil live in /usr/sbin and /usr/bin
export LC_ALL PATH
MAXGB=128

die() { printf 'error: %s\n' "$*" >&2; exit 1; }
gb() { awk -v b="$1" 'BEGIN { printf "%.1f GB", b / 1e9 }'; }
sha() { if command -v shasum >/dev/null 2>&1; then shasum -a 256 | cut -d' ' -f1; else sha256sum | cut -d' ' -f1; fi; }

# The shared rules: from the facts about one disk, the codes of the reasons to refuse it, one per line; nothing when it
# may be written. The same function as IntelXe Manager's StickRules (windows/app/IntelXe.Core/Stick/StickDisk.cs); the
# answers both must give are stick-disk-vectors.json.
#   rules BUS REMOVABLE WHOLE SYSTEM SIZE MINIMUM LARGE
#   BUS usb, virtual, or anything else (internal, nvme, sata, thunderbolt, sd ...); REMOVABLE, WHOLE, SYSTEM (holds the
#   running system), LARGE (--allow-large): 1 or 0; SIZE and MINIMUM in bytes
rules() {
	if [ "$3" != 1 ]; then echo partition; fi
	case "$1" in usb) ;; virtual) echo virtual ;; *) echo not-usb ;; esac
	if [ "$2" != 1 ]; then echo not-removable; fi
	if [ "$4" = 1 ]; then echo system; fi
	if [ "$5" -lt "$6" ]; then echo too-small; fi
	if [ "$7" != 1 ] && [ "$5" -gt $((MAXGB * 1000000000)) ]; then echo too-large; fi
	return 0
}
# why: the first reason as a sentence (WHY), from the codes rules printed; returns 1 when there is one
why() {
	code=$(printf '%s\n' "$2" | sed -n 1p)
	case "$code" in
	'') WHY=""; return 0 ;;
	partition) WHY="$1 is a partition: name the whole disk (see --list)" ;;
	virtual) WHY="$1 is a virtual disk (a disk image or an APFS container)" ;;
	not-usb) WHY="$1 is not a USB disk (${BUS:-internal})" ;;
	not-removable) WHY="$1 is not removable media: a USB hard disk or SSD rather than a stick" ;;
	system) WHY="$1 holds the running system" ;;
	too-small) WHY="$1 ($(gb "$SIZE")) is smaller than the image" ;;
	too-large) WHY="$1 is $(gb "$SIZE"), larger than $MAXGB GB (--allow-large if you mean it)" ;;
	*) WHY="$1: $code" ;;
	esac
	return 1
}
if [ "${1:-}" = --rules ]; then shift; [ $# = 7 ] || die "--rules BUS REMOVABLE WHOLE SYSTEM SIZE MINIMUM LARGE"; rules "$@"; exit 0; fi

LARGE=0 LIST=0
set -- "$@" --end
while [ "$1" != --end ]; do
	case "$1" in
	--allow-large) LARGE=1 ;;
	--list) LIST=1 ;;
	-h|--help) sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) set -- "$@" "$1" ;;
	esac
	shift
done
shift

OS=$(uname -s)
# ---------------------------------------------------------------- macOS
mac_pget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || true; }
# the checks set SIZE, MODEL and BUS, and WHY when they refuse the disk (return 1); MIN is the image's size (0: --list)
mac_check() {
	T=$(mktemp -d)
	diskutil info -plist "$1" > "$T/i.plist" 2>/dev/null || { WHY="diskutil does not know $1"; rm -rf "$T"; return 1; }
	whole=0 sys=0 rem=0 bus=other
	[ "$(mac_pget "$T/i.plist" WholeDisk)" = true ] && whole=1
	[ "$(mac_pget "$T/i.plist" RemovableMedia)" = true ] && rem=1
	BUS=$(mac_pget "$T/i.plist" BusProtocol)
	if [ "$(mac_pget "$T/i.plist" VirtualOrPhysical)" = Virtual ]; then bus=virtual
	elif [ "$BUS" = USB ] && [ "$(mac_pget "$T/i.plist" Internal)" != true ]; then bus=usb
	fi
	# the disk the running macOS boots from: the physical stores of the / volume's APFS container
	diskutil info -plist / > "$T/root.plist" 2>/dev/null || true
	n=0
	while :; do
		s=$(mac_pget "$T/root.plist" "APFSPhysicalStores.$n.APFSPhysicalStore"); [ -n "$s" ] || break
		[ "$(printf '%s' "$s" | sed 's/s[0-9]*$//')" = "$1" ] && sys=1
		n=$((n + 1))
	done
	[ "$(mac_pget "$T/root.plist" ParentWholeDisk)" = "$1" ] && sys=1
	SIZE=$(mac_pget "$T/i.plist" Size)
	MODEL=$(mac_pget "$T/i.plist" MediaName)
	rm -rf "$T"
	why "$1" "$(rules "$bus" "$rem" "$whole" "$sys" "${SIZE:-0}" "${MIN:-0}" "$LARGE")"
}
mac_list() {
	for d in $(diskutil list external physical 2>/dev/null | sed -n 's#^/dev/\(disk[0-9]*\) .*#\1#p'); do
		if mac_check "$d"; then printf '  %-8s %-10s %-10s %s\n' "$d" "$(gb "$SIZE")" "$BUS" "$MODEL"
		else printf '  %-8s refused: %s\n' "$d" "$WHY"; fi
	done
}
# ---------------------------------------------------------------- Linux
lin_check() {
	[ -d "/sys/block/$1" ] || { WHY="$1 is not a whole disk (see --list)"; return 1; }
	case "$(readlink -f "/sys/block/$1")" in */virtual/*) bus=virtual ;; */usb[0-9]*/*) bus=usb ;; *) bus=other ;; esac
	rem=$(cat "/sys/block/$1/removable" 2>/dev/null || echo 0)
	SIZE=$(( $(cat "/sys/block/$1/size") * 512 ))
	MODEL=$(cat "/sys/block/$1/device/model" 2>/dev/null | sed 's/ *$//')
	case $bus in usb) BUS=USB ;; virtual) BUS=virtual ;; *) BUS="not USB" ;; esac
	# a mounted file system of the running system on it: /, /boot, /home, /usr, /var or swap
	sys=0
	for p in "/dev/$1" /dev/"$1"[0-9]* /dev/"$1"p[0-9]*; do
		[ -b "$p" ] || continue
		if awk -v p="$p" '$1 == p && ($2 == "/" || $2 ~ /^\/(boot|home|usr|var)(\/|$)/) { f = 1 } END { exit !f }' /proc/mounts; then sys=1; fi
		if grep -q "^$p " /proc/swaps 2>/dev/null; then sys=1; fi
	done
	why "$1" "$(rules "$bus" "$rem" 1 "$sys" "$SIZE" "${MIN:-0}" "$LARGE")"
}
lin_list() {
	for b in /sys/block/*; do
		d=${b##*/}
		case "$d" in loop*|ram*|zram*|sr*|dm-*|md*) continue ;; esac
		if lin_check "$d"; then printf '  %-8s %-10s %-10s %s\n' "$d" "$(gb "$SIZE")" "$BUS" "$MODEL"
		else printf '  %-8s refused: %s\n' "$d" "$WHY"; fi
	done
}

case "$OS" in Darwin|Linux) ;; *) die "macOS or Linux only (Windows: IntelXe Manager, Report page, Probe stick)" ;; esac
if [ "$LIST" = 1 ]; then
	echo "Disks:"
	if [ "$OS" = Darwin ]; then mac_list; else lin_list; fi
	exit 0
fi
[ $# = 2 ] || die "usage: write-stick.sh IMAGE DISK (or --list); see --help"
IMG=$1
D=${2#/dev/}
[ -f "$IMG" ] || die "no image $IMG (tools/probe-stick/build.sh makes one)"
ISZ=$(wc -c < "$IMG" | tr -d ' ')
MIN=$ISZ
if [ -f "$IMG.sha256" ]; then
	want=$(cut -d' ' -f1 "$IMG.sha256")
	got=$(sha < "$IMG")
	[ "$want" = "$got" ] || die "$IMG does not match $IMG.sha256: download it again"
	echo "image: $IMG, $(gb "$ISZ"), SHA-256 matches"
else
	echo "image: $IMG, $(gb "$ISZ") (no .sha256 next to it: not checked)"
fi

if [ "$OS" = Darwin ]; then
	case "$D" in disk[0-9]|disk[0-9][0-9]) ;; *) die "name a whole disk such as disk4 (see --list)" ;; esac
	mac_check "$D" || die "$WHY"
	RAW=/dev/r$D
else
	case "$D" in */*|'') die "name a disk such as sdb (see --list)" ;; esac
	lin_check "$D" || die "$WHY"
	RAW=/dev/$D
fi
echo
echo "disk:  $D  $(gb "$SIZE")  $BUS  $MODEL"
if [ "$OS" = Darwin ]; then diskutil list "$D" | sed '1d; s/^/       /'; else lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "/dev/$D" 2>/dev/null | sed 's/^/       /'; fi
echo
echo "Everything on $D will be erased. Type its name ($D) to go on, anything else to stop:"
printf '> '
read -r answer
[ "$answer" = "$D" ] || die "stopped: nothing was written"

if [ "$OS" = Darwin ]; then
	diskutil unmountDisk "/dev/$D" >/dev/null || die "could not unmount $D"
	sudo dd if="$IMG" of="$RAW" bs=4m
else
	for p in /dev/"$D"?*; do [ -b "$p" ] && grep -q "^$p " /proc/mounts && sudo umount "$p"; done
	sudo dd if="$IMG" of="$RAW" bs=4M conv=fsync
fi
sync
echo "written; reading it back..."
back=$(sudo head -c "$ISZ" "$RAW" | sha)
[ "$back" = "$(sha < "$IMG")" ] || die "the read-back differs from the image: try another stick"
echo "the stick matches the image (SHA-256 $back)"
if [ "$OS" = Darwin ]; then diskutil eject "/dev/$D" >/dev/null 2>&1 || true; fi
echo "Done. Turn Secure Boot off on the computer to scan, and boot it from this stick."
