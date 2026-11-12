#!/bin/sh
# SPDX-License-Identifier: MIT
# kconfig-check.sh VMLINUZ MODULES.BUILTIN CONFIG-OUT: check the configuration of the kernel the stick ships.
#
# The configuration is read from the kernel image itself (CONFIG_IKCONFIG: scripts/extract-ikconfig's method): the
# compressed vmlinux inside the bzImage is found by its zstd, xz or gzip magic, unpacked, and the gzip blob after
# "IKCFG_ST" is the .config. Then every option the stick depends on is checked, and modules.builtin is searched for a
# network, Wi-Fi or Bluetooth driver built into the kernel (the image deletes modules, it cannot delete a built-in one).
# Prints one line per check; exits 1 if any fails. Needs GNU grep (-b with -P), zstd, xz and gzip.
set -u
export LC_ALL=C
V=$1; B=$2; C=$3
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT

found=""
for magic in '\x28\xb5\x2f\xfd:zstd -dc' '\xfd\x37\x7a\x58\x5a\x00:xz -dc' '\x1f\x8b\x08:gzip -dc'; do
	for off in $(grep -aboP "${magic%%:*}" "$V" | cut -d: -f1 | head -8); do
		tail -c +$((off + 1)) "$V" | ${magic#*:} > "$T/vmlinux" 2>/dev/null
		o=$(grep -abo 'IKCFG_ST' "$T/vmlinux" 2>/dev/null | head -1 | cut -d: -f1)
		[ -n "$o" ] || continue
		tail -c +$((o + 9)) "$T/vmlinux" | gzip -dc > "$C" 2>/dev/null
		grep -q '^CONFIG_' "$C" && { found=${magic#*:}; break 2; }
	done
done
[ -n "$found" ] || { echo "FAIL could not read the configuration from $V (no CONFIG_IKCONFIG?)"; exit 1; }
echo "configuration: $(sed -n 's/^# Linux\/\(.*\) Configuration$/\1/p' "$C" | head -1), read from the kernel image (${found%% *})"

FAIL=0
val() { v=$(sed -n "s/^CONFIG_$1=//p" "$C" | head -1); [ -n "$v" ] || v=n; printf '%s' "$v"; }
need() { # SYMBOL ALLOWED(y|m|n|y/m) WHY
	v=$(val "$1")
	case "/$2/" in *"/$v/"*) r=ok ;; *) r=FAIL; FAIL=1 ;; esac
	printf '%-4s CONFIG_%s=%s (want %s): %s\n' "$r" "$1" "$v" "$2" "$3"
}
need MMIOTRACE y "the trace step records the GPU driver's register accesses"
need HOTPLUG_CPU y "mmiotrace takes all CPUs but one offline while it runs"
need TRACING y "tracefs, which mmiotrace writes to"
need DRM_XE m "xe as a module: bound per device, under the trace"
need DRM_XE_DISPLAY y "xe drives the display (VBT, DMC, connectors)"
need DRM_I915 m "i915 as a module, for the GPUs xe does not drive"
need DEBUG_FS y "debugfs: GuC/HuC state, VBT, display info (collect-linux.sh)"
need EFIVAR_FS y/m "efivarfs, mounted read-only (Secure Boot state)"
need EFI_STUB y "booted by GRUB on UEFI"
need BLK_DEV_INITRD y "the whole system is the initramfs"
need RD_ZSTD y "the initramfs is zstd-compressed"
need DEVTMPFS y "device nodes without udev"
need TMPFS y "/run and /tmp in RAM"
need FW_LOADER y "GPU firmware from /lib/firmware"
need FW_LOADER_COMPRESS_ZSTD y "the firmware files stay .zst as linux-firmware packs them"
need FW_LOADER_USER_HELPER n "the firmware loads without a user-space helper"
need MAGIC_SYSRQ y "the dead-man's fallback power-off"
need SOFTLOCKUP_DETECTOR y "softlockup_panic=1 turns a stuck CPU into a reboot"
need DETECT_HUNG_TASK y "hung_task_panic=1 turns a stuck driver bind into a reboot"
need IKCONFIG y "this check reads the configuration back from the kernel image"
need MSDOS_PARTITION y "the stick's MBR partition table"
need USB_XHCI_HCD y/m "USB 3 host (the stick)"
need USB_STORAGE y/m "the stick"
need BLK_DEV_SD y/m "the stick"
need VFAT_FS y/m "the stick's FAT32"
need MODULE_SIG_FORCE n "the image's modules are unpacked copies of the signed ones; nothing else may be required"
need LOCK_DOWN_KERNEL_FORCE_INTEGRITY n "lockdown would forbid mmiotrace"
need LOCK_DOWN_KERNEL_FORCE_CONFIDENTIALITY n "lockdown would forbid mmiotrace and debugfs"
need EFI_VARS_PSTORE_DEFAULT_DISABLE y "a panic does not write the kernel log into firmware variables"
# no network, Wi-Fi or Bluetooth stack built in (modules are deleted by the image build)
for s in CFG80211 MAC80211 BT E1000 E1000E IGB IGC IXGBE R8169 VIRTIO_NET USB_USBNET USB_RTL8152 THUNDERBOLT_NET \
	IWLWIFI ATH9K ATH10K_PCI ATH11K ATH12K RTW88 RTW89 MT76_CORE BRCMFMAC; do
	v=$(val "$s"); case "$v" in y) need "$s" n/m "no network driver built in" ;; esac
done
# modules.builtin lists every driver compiled into the kernel that could have been a module
NET=$(grep -E '^kernel/(drivers/net/(ethernet|wireless|usb|phy|wwan|ppp|can|ieee802154|hyperv|xen-netfront|vmxnet3)|drivers/bluetooth|net/(wireless|mac80211|bluetooth|nfc|can|ieee802154))/' "$B" || true)
if [ -n "$NET" ]; then
	echo "FAIL built-in network/radio drivers: $(echo "$NET" | tr '\n' ' ')"; FAIL=1
else
	echo "ok   no network, Wi-Fi or Bluetooth driver in modules.builtin (built in: $(grep -c '^kernel/drivers/net/' "$B" || true) virtual device driver(s) under drivers/net: $(grep '^kernel/drivers/net/' "$B" | sed 's#.*/##; s#\.ko$##' | tr '\n' ' '))"
fi
exit $FAIL
