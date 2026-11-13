#!/bin/sh
# SPDX-License-Identifier: MIT
# write-stick.sh: write the Xe probe stick image to a USB stick, on macOS or Linux. Everything on the stick is erased.
#
#   sh write-stick.sh --list                    the external disks it would accept
#   sh write-stick.sh IMAGE DISK                write IMAGE to DISK (macOS: disk4; Linux: sdb), after checks and after you
#                                               type the disk's name to confirm; then read it back and compare
#   --allow-large                               accept a disk larger than 128 GB (refused by default: likely a backup disk)
#   --erase-efi                                 erase a stick that holds an EFI without asking for ERASE EFI (for scripts)
#   sh write-stick.sh --efi DISK                only look for an EFI on DISK (read-only) and say what it found
#
# It refuses: a disk that is not on USB (macOS: BusProtocol; Linux: the device path) or not removable media (a USB hard
# disk or SSD rather than a stick), a partition instead of a whole disk, the disk the running system boots from, a disk
# image or other virtual disk, a disk smaller than the image or larger than 128 GB, and an IMAGE whose SHA-256 does
# not match the .img.sha256 next to it. These are the rules IntelXe Manager for Windows applies (StickRules), held to
# the same answers by stick-disk-vectors.json (test/disk-rules-test.sh). It needs sudo for the write and the read-back.
# A stick that holds an EFI (an EFI system partition, by its type; or a FAT volume with EFI/OC/OpenCore.efi, or with an
# EFI/BOOT/BOOTx64.efi that is not a probe stick's own) is not refused, since it is often the stick to rewrite, but it
# may be someone's OpenCore recovery stick: the script names what it found and asks for ERASE EFI besides the disk's
# name (or takes --erase-efi). It looks read-only: mounted volumes where they are, an unmounted FAT volume through a
# temporary read-only mount (macOS: diskutil mount readOnly; Linux: mount -o ro, with sudo), unmounted again at once.
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
# The EFI rule: from the facts about one partition, the codes of the EFI on it, one per line; nothing when erasing it
# loses no EFI. The same rule as IntelXe Manager's StickRules.HoldsEfi and VolumeHoldsEfi; the answers both must give
# are stick-disk-vectors.json's "efiVectors".
#   efi_rules ESP OPENCORE BOOTX64 PROBE
#   ESP: an EFI system partition by its type (GPT C12A7328-..., MBR 0xEF); OPENCORE: EFI/OC/OpenCore.efi on it; BOOTX64:
#   EFI/BOOT/BOOTx64.efi on it; PROBE: it is a probe stick (xe-probe-stick.marker), whose BOOTx64.efi is its own GRUB.
#   Each 1 or 0.
efi_rules() {
	if [ "$1" = 1 ]; then echo esp; fi
	if [ "$2" = 1 ]; then echo opencore
	elif [ "$3" = 1 ] && [ "$4" != 1 ]; then echo boot-loader
	fi
	return 0
}
efi_text() { # the codes efi_rules printed, as words
	printf '%s\n' "$1" | awk '
		$0 == "esp" { w[n++] = "an EFI system partition" }
		$0 == "opencore" { w[n++] = "OpenCore (EFI/OC/OpenCore.efi)" }
		$0 == "boot-loader" { w[n++] = "an EFI boot loader (EFI/BOOT/BOOTx64.efi: an installer or another system'"'"'s boot stick)" }
		END { for (i = 0; i < n; i++) printf "%s%s", (i ? ", with " : ""), w[i]; print "" }'
}
if [ "${1:-}" = --rules ]; then shift; [ $# = 7 ] || die "--rules BUS REMOVABLE WHOLE SYSTEM SIZE MINIMUM LARGE"; rules "$@"; exit 0; fi
if [ "${1:-}" = --efi-rules ]; then shift; [ $# = 4 ] || die "--efi-rules ESP OPENCORE BOOTX64 PROBE"; efi_rules "$@"; exit 0; fi

LARGE=0 LIST=0 ERASE_EFI=0 EFI_ONLY=0
set -- "$@" --end
while [ "$1" != --end ]; do
	case "$1" in
	--allow-large) LARGE=1 ;;
	--erase-efi) ERASE_EFI=1 ;;
	--efi) EFI_ONLY=1 ;;
	--list) LIST=1 ;;
	-h|--help) sed -n '3,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
# ---------------------------------------------------------------- an EFI on the disk (read-only)
# efi_scan DISK sets EFI_FOUND (one line per partition that holds an EFI: "PART (LABEL): what") and EFI_NOTE (FAT
# volumes it could not look inside). Mounted volumes are looked at where they are; an unmounted FAT volume (the only
# kind a PC's firmware boots from) is mounted read-only in a temporary folder and unmounted at once. Nothing is written.
ET="" EFI_DEV="" EFI_MNT=""
as_root() { if [ "$(id -u)" = 0 ]; then "$@"; else sudo "$@"; fi; }
efi_unmount() {
	[ -n "$EFI_DEV" ] || return 0
	if [ "$OS" = Darwin ]; then diskutil unmount "$EFI_DEV" >/dev/null 2>&1 || diskutil unmount force "$EFI_DEV" >/dev/null 2>&1 || true
	else as_root umount "$EFI_MNT" 2>/dev/null || true; fi
	EFI_DEV=""
}
cleanup() {
	efi_unmount
	if [ -n "$ET" ]; then rm -f "$ET"/*.plist; rmdir "$ET/m" "$ET" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
efi_mount() { # efi_mount DEV: DEV mounted read-only at EFI_MNT, or 1
	[ -n "$ET" ] || ET=$(mktemp -d)
	EFI_MNT=$ET/m
	mkdir -p "$EFI_MNT"
	if [ "$OS" = Darwin ]; then
		diskutil mount readOnly -mountPoint "$EFI_MNT" "$1" >/dev/null 2>&1 || return 1
	else
		echo "looking for an EFI on $1: mounting it read-only (mount -o ro, as root)" >&2
		as_root mount -o ro,nosuid,nodev,noexec -t vfat "$1" "$EFI_MNT" 2>/dev/null || return 1
	fi
	EFI_DEV=$1
}
vol_look() { # vol_look DIR: OC, BOOT and PROBE (1 or 0) for the volume at DIR; FAT ignores case, so names are compared in lower case
	OC=0 BOOT=0 PROBE=0
	files=$(cd "$1" 2>/dev/null && find . -maxdepth 3 -type f 2>/dev/null | tr 'A-Z' 'a-z') || files=""
	printf '%s\n' "$files" | grep -qx './efi/oc/opencore.efi' && OC=1
	printf '%s\n' "$files" | grep -qx './efi/boot/bootx64.efi' && BOOT=1
	printf '%s\n' "$files" | grep -qx './xe-probe-stick.marker' && PROBE=1
	return 0
}
efi_part() { # efi_part DEV ESP FS MOUNTPOINT LABEL: one partition (FS: fat or ""), added to EFI_FOUND when it holds an EFI
	OC=0 BOOT=0 PROBE=0
	if [ -n "$4" ]; then vol_look "$4"
	elif [ "$3" = fat ]; then
		if efi_mount "$1"; then vol_look "$EFI_MNT"; efi_unmount
		elif [ "$2" != 1 ]; then EFI_NOTE="$EFI_NOTE$1 is a FAT volume the script could not mount read-only to look for an EFI
"
		fi
	fi
	codes=$(efi_rules "$2" "$OC" "$BOOT" "$PROBE")
	[ -z "$codes" ] || EFI_FOUND="$EFI_FOUND$1${5:+ ($5)}: $(efi_text "$codes")
"
}
mac_efi() {
	[ -n "$ET" ] || ET=$(mktemp -d)
	diskutil list -plist "$1" > "$ET/l.plist" 2>/dev/null || return 0
	parts="" n=0
	while :; do
		p=$(mac_pget "$ET/l.plist" "AllDisksAndPartitions.0.Partitions.$n.DeviceIdentifier"); [ -n "$p" ] || break
		parts="$parts $p"
		n=$((n + 1))
	done
	[ -n "$parts" ] || parts=$1   # a file system on the whole disk, without a partition table
	for p in $parts; do
		diskutil info -plist "$p" > "$ET/p.plist" 2>/dev/null || continue
		esp=0
		case "$(mac_pget "$ET/p.plist" Content | tr 'a-z' 'A-Z')" in EFI|C12A7328-F81F-11D2-BA4B-00A0C93EC93B|0XEF) esp=1 ;; esac
		fs=""
		[ "$(mac_pget "$ET/p.plist" FilesystemType)" = msdos ] && fs=fat
		efi_part "$p" "$esp" "$fs" "$(mac_pget "$ET/p.plist" MountPoint)" "$(mac_pget "$ET/p.plist" VolumeName)"
	done
}
lin_efi() {
	parts=""
	for p in /dev/"$1"[0-9]* /dev/"$1"p[0-9]*; do [ -b "$p" ] && parts="$parts $p"; done
	[ -n "$parts" ] || parts=/dev/$1   # a file system on the whole disk, without a partition table
	for p in $parts; do
		pt=$(lsblk -dno PARTTYPE "$p" 2>/dev/null | tr -d ' ' || true)
		[ -n "$pt" ] || pt=$(blkid -p -o value -s PART_ENTRY_TYPE "$p" 2>/dev/null || true)
		esp=0
		case "$(printf '%s' "$pt" | tr 'A-Z' 'a-z')" in c12a7328-f81f-11d2-ba4b-00a0c93ec93b|0xef) esp=1 ;; esac
		ft=$(lsblk -dno FSTYPE "$p" 2>/dev/null | tr -d ' ' || true)
		[ -n "$ft" ] || ft=$(blkid -p -o value -s TYPE "$p" 2>/dev/null || true)
		fs=""
		[ "$ft" = vfat ] && fs=fat
		lb=$(lsblk -dno LABEL "$p" 2>/dev/null || true)
		[ -n "$lb" ] || lb=$(blkid -p -o value -s LABEL "$p" 2>/dev/null || true)
		efi_part "$p" "$esp" "$fs" "$(lsblk -dno MOUNTPOINT "$p" 2>/dev/null | sed -n 1p || true)" "$lb"
	done
}
efi_scan() {
	EFI_FOUND="" EFI_NOTE=""
	if [ "$OS" = Darwin ]; then mac_efi "$1"; else lin_efi "$1"; fi
}
efi_report() { # what efi_scan found on DISK, as a warning
	if [ -n "$EFI_FOUND" ]; then
		echo "WARNING: $1 holds an EFI, and writing the stick erases it:"
		printf '%s' "$EFI_FOUND" | sed 's/^/  - /'
		echo "If a computer boots from this stick (an OpenCore recovery or boot stick, an installer), it will not boot from it any"
		echo "more: copy its EFI folder somewhere safe first."
	fi
	[ -z "$EFI_NOTE" ] || printf '%s' "$EFI_NOTE" | sed 's/^/note: /'
	return 0
}
disk_name() { # the DISK argument, checked: a whole disk's name
	D=${1#/dev/}
	if [ "$OS" = Darwin ]; then
		case "$D" in disk[0-9]|disk[0-9][0-9]) ;; *) die "name a whole disk such as disk4 (see --list)" ;; esac
	else
		case "$D" in */*|'') die "name a disk such as sdb (see --list)" ;; esac
	fi
}

case "$OS" in Darwin|Linux) ;; *) die "macOS or Linux only (Windows: IntelXe Manager, Report page, Probe stick)" ;; esac
if [ "$LIST" = 1 ]; then
	echo "Disks:"
	if [ "$OS" = Darwin ]; then mac_list; else lin_list; fi
	exit 0
fi
if [ "$EFI_ONLY" = 1 ]; then
	[ $# = 1 ] || die "usage: write-stick.sh --efi DISK"
	disk_name "$1"
	efi_scan "$D"
	if [ -n "$EFI_FOUND" ]; then efi_report "$D"; else echo "$D: no EFI found"; efi_report "$D"; fi
	exit 0
fi
[ $# = 2 ] || die "usage: write-stick.sh IMAGE DISK (or --list); see --help"
IMG=$1
disk_name "$2"
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

check_disk() { if [ "$OS" = Darwin ]; then mac_check "$D" || die "$WHY"; else lin_check "$D" || die "$WHY"; fi; }
check_disk
if [ "$OS" = Darwin ]; then RAW=/dev/r$D; else RAW=/dev/$D; fi
echo
echo "disk:  $D  $(gb "$SIZE")  $BUS  $MODEL"
if [ "$OS" = Darwin ]; then diskutil list "$D" | sed '1d; s/^/       /'; else lsblk -o NAME,SIZE,FSTYPE,LABEL,MOUNTPOINT "/dev/$D" 2>/dev/null | sed 's/^/       /'; fi
efi_scan "$D"
echo
efi_report "$D"
echo "Everything on $D will be erased. Type its name ($D) to go on, anything else to stop:"
printf '> '
read -r answer || answer=""
[ "$answer" = "$D" ] || die "stopped: nothing was written"
if [ -n "$EFI_FOUND" ]; then
	if [ "$ERASE_EFI" = 1 ]; then
		echo "--erase-efi: the EFI on $D is erased with the rest"
	else
		echo "$D holds an EFI (above). Type ERASE EFI to erase it too, anything else to stop:"
		printf '> '
		read -r answer || answer=""
		[ "$answer" = "ERASE EFI" ] || die "stopped: nothing was written ($D holds an EFI; --erase-efi to erase it without this question)"
	fi
fi
# still the same disk, holding the same: a stick may have been swapped while you typed
size0=$SIZE model0=$MODEL found0=$EFI_FOUND
check_disk
[ "$SIZE" = "$size0" ] && [ "$MODEL" = "$model0" ] || die "$D changed while you typed (now $(gb "$SIZE") $MODEL): nothing was written; run it again"
efi_scan "$D"
[ "$EFI_FOUND" = "$found0" ] || die "what $D holds changed while you typed: nothing was written; run it again"

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
