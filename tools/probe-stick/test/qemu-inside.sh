#!/bin/sh
# SPDX-License-Identifier: MIT
# qemu-inside.sh: boot the stick image on an emulated UEFI PC, four times. qemu-test.sh runs it in a Debian container
# (QEMU, OVMF, mtools) with the test directory at /t; it judges the results afterwards (check-report.py).
#
# The PC is loaded with what must not come out: SMBIOS system and board serials and a UUID, a USB serial on the stick,
# a SATA disk with a serial, and the same serial written inside the disk, which also carries a decoy FAT volume with the
# stick's own label and marker (the worst case for the stick finder). An e1000e network card sits on the PCI bus; the
# image has no driver for it. The disk's SHA-256 is taken before and after each boot.
#   boot 1: a clean scan.
#   boot 2: xestick.testhang=collect-after: the scan hangs in that step, as a frozen driver bind would; the step's
#           dead-man must turn the PC off, leaving the step marked running on the stick.
#   boot 3: a clean boot again, with a line from another computer planted in the state file: the scan must leave
#           collect-after out on this computer and say so, and the other computer's line must change nothing.
#   boot 4: the menu's second entry, the safe scan (nomodeset, no GPU driver): one collection, a report.
set -eu
T=/t
LIMIT=${LIMIT:-1500}
CODE=/usr/share/OVMF/OVMF_CODE_4M.fd
VARS=/usr/share/OVMF/OVMF_VARS_4M.fd
P=1M   # the stick's partition starts at 1 MiB
export MTOOLS_SKIP_CHECK=1
sha() { sha256sum "$1" | cut -d' ' -f1; }

cp "$T/xe-probe-stick.img" "$T/stick.img"
mcopy -n -i "$T/stick.img@@$P" ::/boot/grub/grub.cfg "$T/grub.cfg.orig"
grub() { # EXTRA [ENTRY]: the stick's own menu, entry ENTRY (default 0: the scan) as it is, with a serial console, no
	# menu delay, no wait before the power-off, and EXTRA
	sed -e 's/^set timeout=.*/set timeout=0/' -e "s/^set default=.*/set default=${2:-0}/" -e "s#^\\(\tlinux /boot/vmlinuz .*\\)\$#\\1 console=ttyS0,115200 xestick.wait=0 $1#" \
		"$T/grub.cfg.orig" > "$T/grub.cfg"
	grep -q "xestick.mode=safe console=ttyS0,115200 xestick.wait=0 $1" "$T/grub.cfg" || { echo "error: could not patch grub.cfg"; exit 1; }
	mcopy -o -i "$T/stick.img@@$P" "$T/grub.cfg" ::/boot/grub/grub.cfg
}

# the tester's disk: a decoy FAT volume labelled like the stick, with the marker and the disk's serial inside
truncate -s 64M "$T/disk.img"
mformat -i "$T/disk.img" -v XEPROBE -F ::
echo "decoy" > "$T/xe-probe-stick.marker"
printf 'serial of this disk: %s\n' "$SERIAL_DISK" > "$T/serial.txt"
mcopy -i "$T/disk.img" "$T/xe-probe-stick.marker" "$T/serial.txt" ::
printf '%s' "$SERIAL_DISK" | dd of="$T/disk.img" bs=1 seek=$((48 * 1048576)) conv=notrunc status=none
cp "$VARS" "$T/vars.fd"

boot() { # N
	echo "== boot $1 (limit ${LIMIT}s)"
	sha "$T/disk.img" > "$T/disk-before-$1.sha256"
	t0=$(date +%s)
	timeout "$LIMIT" qemu-system-x86_64 -machine q35 -cpu max -smp 2 -m 2048 \
		-drive if=pflash,format=raw,readonly=on,file="$CODE" -drive if=pflash,format=raw,file="$T/vars.fd" \
		-device qemu-xhci,id=xhci \
		-drive if=none,id=stick,format=raw,file="$T/stick.img" \
		-device usb-storage,bus=xhci.0,drive=stick,serial="$SERIAL_STICK",removable=on \
		-drive if=none,id=d0,format=raw,file="$T/disk.img" -device ide-hd,drive=d0,bus=ide.0,serial="$SERIAL_DISK" \
		-netdev user,id=n0,restrict=on -device e1000e,netdev=n0,mac="$MAC" \
		-smbios type=0,vendor=XsBios,version=XS-BIOS-1 \
		-smbios type=1,manufacturer=XsTest,product=QEMU-XS,serial="$SERIAL_SYS",uuid="$UUID" \
		-smbios type=2,manufacturer=XsBoards,product=XS-BOARD,serial="$SERIAL_BOARD" \
		-display none -monitor none -serial file:"$T/serial-$1.log" -no-reboot
	rc=$?
	echo "QEMU exit $rc after $(( $(date +%s) - t0 )) s"
	echo "$rc $(( $(date +%s) - t0 ))" > "$T/qemu-$1.rc"
	sha "$T/disk.img" > "$T/disk-after-$1.sha256"
	mkdir -p "$T/reports-$1"
	mcopy -n -s -i "$T/stick.img@@$P" "::/REPORTS/*" "$T/reports-$1/" 2>/dev/null || true
	ls -la "$T/reports-$1"
}

set +e
grub ""
boot 1
grub "xestick.testhang=collect-after"
boot 2
# another computer froze during collect-before: that must change nothing here
mcopy -n -i "$T/stick.img@@$P" ::/REPORTS/xe-stick-state.txt "$T/state.txt"
printf 'running 000000000000 collect-before\n' >> "$T/state.txt"
mcopy -o -i "$T/stick.img@@$P" "$T/state.txt" ::/REPORTS/xe-stick-state.txt
grub ""
boot 3
grub "" 1
boot 4
printf 'XsTest|QEMU-XS|XsBoards|XS-BOARD|XS-BIOS-1|' | sha256sum | cut -c1-12 > "$T/key.txt"
