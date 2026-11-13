#!/bin/sh
# SPDX-License-Identifier: MIT
# efi-warning-test.sh: write-stick.sh's EFI warning on real partition tables and file systems, without a USB stick.
#
#   sh tools/probe-stick/test/efi-warning-test.sh           on macOS: disk images (hdiutil, diskutil); then, when Docker
#                                                          runs, the same on Linux loop devices in the pinned Debian image
#   sh tools/probe-stick/test/efi-warning-test.sh --inside DIR    (in that container, as root) the Linux half
#
# The layouts (made on macOS with diskutil, as sparse files): esp-oc (GPT: an EFI system partition holding OpenCore,
# and an empty FAT32 volume), installer (FAT32 with EFI/BOOT/BOOTX64.EFI), probe (FAT32 with the probe stick's marker
# and its BOOTX64.EFI), plain (an empty FAT32 volume). Checked: --efi names what each holds (unmounted volumes looked at
# through a temporary read-only mount, and nothing left mounted); the write warns and asks for ERASE EFI only on esp-oc
# and installer, stops before dd on any other answer, writes after ERASE EFI or with --erase-efi, and writes plain and
# probe after the disk's name alone. The writes run a copy of write-stick.sh in which only the disk rules (they refuse a
# disk image as virtual) and sudo are left out; dd writes into the disk image, which is then compared with the image.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
PATH=/usr/sbin:/usr/bin:/sbin:/bin:$PATH
fails=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fails=$((fails + 1)); }

# the copy of write-stick.sh for the writes: every disk passes the rules, no sudo, dd announces itself
stub() { # stub OUT
	sed -e 's/why "\$1" "\$(rules "\$bus" "\$rem" "\$whole" "\$sys" "\${SIZE:-0}" "\${MIN:-0}" "\$LARGE")"/why "$1" ""/' \
	    -e 's/why "\$1" "\$(rules "\$bus" "\$rem" 1 "\$sys" "\$SIZE" "\${MIN:-0}" "\$LARGE")"/why "$1" ""/' \
	    -e 's/sudo dd /echo DD-RAN >\&2; dd /' -e 's/sudo head /head /' -e 's/sudo umount /umount /' \
	    "$HERE/write-stick.sh" > "$1"
	[ "$(grep -c 'why "\$1" ""' "$1")" = 2 ] && grep -q DD-RAN "$1" || { echo "the stub did not take: write-stick.sh changed"; exit 1; }
}

# the image to write: 2 MiB with the stick's layout, and its .img.sha256
probe_image() { # probe_image DIR
	python3 -I - "$1/xe-probe-stick-0.0.1.img" <<'PY'
import hashlib, os, struct, sys
p = sys.argv[1]
b = bytearray(2 << 20)
b[446 + 4] = 0x0C
struct.pack_into("<II", b, 446 + 8, 2048, len(b) // 512 - 2048)
b[510:512] = b"\x55\xaa"
bs = 2048 * 512
b[bs + 71:bs + 82] = b"XEPROBE    "
b[bs + 82:bs + 90] = b"FAT32   "
b[bs + 510:bs + 512] = b"\x55\xaa"
b[bs + 8192:bs + 8224] = hashlib.sha256(b"probe").digest()
open(p, "wb").write(b)
open(p + ".sha256", "w").write(hashlib.sha256(b).hexdigest() + "  " + os.path.basename(p) + "\n")
PY
}

# ---- the cases, the same on both systems (attach FILE prints the disk's name; detach DISK; still_mounted DISK)
scan_case() { # scan_case LAYOUT EXPECTED-TEXT|none
	f=$W/$1-scan.img
	cp_sparse "$W/$1.img" "$f"
	d=$(attach "$f")
	out=$(sh "$HERE/write-stick.sh" --efi "$d" 2>&1) || true
	mounted=$(still_mounted "$d")
	detach "$d"
	rm -f "$f"
	if [ "$2" = none ]; then
		printf '%s\n' "$out" | grep -q "no EFI found" && ok "$1: --efi finds no EFI" || bad "$1: --efi says: $out"
	else
		printf '%s\n' "$out" | grep -qF "$2" && ok "$1: --efi names $2" || bad "$1: --efi says: $out"
	fi
	[ -z "$mounted" ] && ok "$1: nothing left mounted" || bad "$1: left mounted: $mounted"
}
write_case() { # write_case LAYOUT INPUT EXPECT(written|stopped) [OPTION]; DISK in INPUT is the attached disk's name
	f=$W/$1-write.img
	cp_sparse "$W/$1.img" "$f"
	d=$(attach "$f")
	input=$(printf "$2" | sed "s/^DISK\$/$d/")
	out=$(printf '%s\n' "$input" | sh "$W/stub.sh" ${4:-} "$W/img/xe-probe-stick-0.0.1.img" "$d" 2>&1) && rc=0 || rc=$?
	detach "$d"
	what="$1, answers '$(printf '%s' "$2" | sed 's/\\n/|/g')'${4:+, $4}"
	written=no
	head -c 2097152 "$f" | cmp -s - "$W/img/xe-probe-stick-0.0.1.img" && written=yes
	rm -f "$f"
	if [ "$3" = written ]; then
		[ $rc = 0 ] && [ $written = yes ] && ok "$what: written and read back" || bad "$what: exit $rc, written $written: $(printf '%s' "$out" | tail -3)"
	elif [ $rc != 0 ] && [ $written = no ] && ! printf '%s' "$out" | grep -q DD-RAN; then ok "$what: stopped before dd"
	else bad "$what: exit $rc, written $written: $(printf '%s' "$out" | tail -3)"; fi
	case "$1" in
	esp-oc|installer)
		printf '%s' "$out" | grep -q "^WARNING: $d holds an EFI" && ok "$what: warned" || bad "$what: no warning"
		case "$2" in DISK*)
			if [ -n "${4:-}" ]; then
				printf '%s' "$out" | grep -q -e "--erase-efi: the EFI on $d is erased" && ok "$what: no question, --erase-efi said so" || bad "$what: --erase-efi not taken"
			else
				printf '%s' "$out" | grep -q "Type ERASE EFI" && ok "$what: asked for ERASE EFI" || bad "$what: did not ask for ERASE EFI"
			fi ;;
		esac ;;
	*)
		printf '%s' "$out" | grep -q "WARNING\|ERASE EFI" && bad "$what: warned about an EFI it does not hold" || ok "$what: no EFI warning" ;;
	esac
}
cases() {
	stub "$W/stub.sh"
	scan_case esp-oc "an EFI system partition, with OpenCore (EFI/OC/OpenCore.efi)"
	scan_case installer "an EFI boot loader (EFI/BOOT/BOOTx64.efi"
	scan_case probe none
	scan_case plain none
	write_case esp-oc 'DISK\n' stopped
	write_case esp-oc 'DISK\nerase efi\n' stopped
	write_case esp-oc 'DISK\nERASE EFI\n' written
	write_case esp-oc 'DISK\n' written --erase-efi
	write_case esp-oc 'disk0\nERASE EFI\n' stopped
	write_case installer 'DISK\n' stopped
	write_case installer 'DISK\nERASE EFI\n' written
	write_case plain 'DISK\n' written
	write_case probe 'DISK\n' written
}

if [ "${1:-}" = --inside ]; then
	# ---- Linux, as root in the container: loop devices and their partitions (no udev here: the nodes are made)
	W=$2
	cp_sparse() { dd if="$1" of="$2" bs=4M conv=sparse status=none; }   # the shared folder takes no hole punching
	attach() {
		l=$(losetup -fP --show "$1")
		n=${l#/dev/}
		for p in /sys/block/"$n"/"$n"p*; do
			[ -e "$p/dev" ] || continue
			rm -f "/dev/${p##*/}"   # partition numbers are given out anew at each attach
			mknod "/dev/${p##*/}" b $(tr ':' ' ' < "$p/dev")
		done
		echo "$n"
	}
	detach() { sync; losetup -d "/dev/$1"; }
	still_mounted() { grep "^/dev/$1" /proc/mounts | cut -d' ' -f1-2 || true; }
	cases
	echo "== Linux: $fails failure(s)"
	exit $fails
fi

[ "$(uname -s)" = Darwin ] || { echo "run it on macOS (it makes the disk images with diskutil); the Linux half runs in Docker from there"; exit 2; }
W=$(mktemp -d "${TMPDIR:-/tmp}/ixeefi.XXXXXX")
ATTACHED=""
trap 'for a in $ATTACHED; do hdiutil detach -force "$a" >/dev/null 2>&1 || true; done; rm -rf "$W"' EXIT
cp_sparse() { cp -c "$1" "$2" 2>/dev/null || cp "$1" "$2"; }
attach() {
	a=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount "$1" 2>/dev/null | awk 'NR == 1 { sub("/dev/", "", $1); print $1 }')
	[ -n "$a" ] || { echo "hdiutil could not attach $1" >&2; exit 1; }
	ATTACHED="$ATTACHED $a"
	echo "$a"
}
detach() { hdiutil detach -force "$1" >/dev/null 2>&1 || true; }
still_mounted() { mount | grep "^/dev/$1" | cut -d' ' -f1-3 || true; }
mountpoint_of() { diskutil info -plist "$1" | plutil -extract MountPoint raw -o - - 2>/dev/null || true; }

layout() { # layout NAME SIZE_MB
	dd if=/dev/zero of="$W/$1.img" bs=1 count=0 seek=$(($2 * 1048576)) 2>/dev/null
	d=$(attach "$W/$1.img")
	diskutil partitionDisk "$d" GPT FAT32 STICK 100% >/dev/null
	s=$(diskutil list "$d" | awk '$0 ~ / STICK / { print $NF }')
	diskutil mount "$s" >/dev/null 2>&1 || true
	vol=$(mountpoint_of "$s")
	[ -n "$vol" ] || { echo "could not mount $s"; exit 1; }
	case $1 in
	esp-oc)
		[ "$(diskutil info -plist "${d}s1" | plutil -extract Content raw -o - -)" = EFI ] || { echo "diskutil made no EFI system partition"; exit 1; }
		diskutil mount "${d}s1" >/dev/null
		e=$(mountpoint_of "${d}s1")
		mkdir -p "$e/EFI/OC" "$e/EFI/BOOT"
		echo oc > "$e/EFI/OC/OpenCore.efi"; echo oc > "$e/EFI/BOOT/BOOTx64.efi" ;;
	installer) mkdir -p "$vol/EFI/BOOT"; echo boot > "$vol/EFI/BOOT/BOOTX64.EFI" ;;
	probe) mkdir -p "$vol/EFI/BOOT"; echo grub > "$vol/EFI/BOOT/BOOTX64.EFI"; echo marker > "$vol/xe-probe-stick.marker" ;;
	esac
	diskutil unmountDisk "$d" >/dev/null
	detach "$d"
}
layout esp-oc 3072   # diskutil adds an EFI system partition to a GPT disk this large (a sparse file: about 12 MB)
layout installer 600
layout probe 600
layout plain 600
mkdir -p "$W/img"
probe_image "$W/img"
echo "== macOS (disk images)"
cases

if command -v docker >/dev/null 2>&1 && docker info >/dev/null 2>&1; then
	IMAGE=$(awk '$1 == "image" && $2 == "qemu" { print $4 }' "$HERE/sources.lock")
	echo "== Linux ($IMAGE, loop devices)"
	docker run --rm --privileged -v "$W:/work" -v "$HERE:/probe-stick:ro" "$IMAGE" sh /probe-stick/test/efi-warning-test.sh --inside /work || fails=$((fails + $?))
else
	echo "note: no Docker here: the Linux half was not run"
fi
echo "== $fails failure(s)"
exit $fails
