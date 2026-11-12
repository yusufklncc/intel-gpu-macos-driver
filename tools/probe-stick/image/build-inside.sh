#!/bin/sh
# SPDX-License-Identifier: MIT
# build-inside.sh: the Xe probe stick image build. build.sh runs it in the pinned Alpine image as linux/amd64, with the
# repository read-only at /src and tools/probe-stick/out at /out.
#
#   sh build-inside.sh build   /out/xe-probe-stick-VERSION.img (raw USB image), .manifest.txt, .img.sha256, /out/usb/
#   sh build-inside.sh lock    print the apk and tool lines of sources.lock for the package files Alpine has now
#
# Nothing ships that is not pinned in sources.lock: the kernel and firmware by URL and SHA-256 (Arch Linux Archive),
# the user space by Alpine package file and SHA-256, GRUB likewise. The packages are unpacked, never installed, so no
# package script runs. The build tools themselves (kmod's depmod, cpio, zstd, mtools, ...) come from the image's
# repositories; their versions go into the manifest.
set -eu
export LC_ALL=C
SRC=/src
PS=$SRC/tools/probe-stick
LOCK=$PS/sources.lock
OUT=/out
CACHE=$OUT/cache
VERSION=$(cat "$PS/VERSION")
NAME=xe-probe-stick-$VERSION
LABEL=XEPROBE
MARKER=xe-probe-stick.marker
W=/tmp/xs            # work area (container-local)
R=$W/root            # the root file system that becomes the initramfs
K=$W/kernel
F=$W/firmware
U=$OUT/usb           # the stick's files
# the user space of the image: busybox, kmod (modprobe/modinfo/lsmod with module blacklists), lspci with names
IMAGE_APKS="busybox musl kmod kmod-libs libcrypto3 xz-libs zlib zstd-libs pciutils pciutils-libs hwdata-pci"
TOOL_APKS="grub grub-efi"
# the modules the stick loads by name; modprobe --show-depends adds what they need
ROOT_MODULES="xe i915 usb-storage uas vfat nls_iso8859_1 nls_utf8"
GRUB_MODULES="part_msdos part_gpt fat search search_fs_file search_label normal linux echo all_video efi_gop efi_uga
	configfile test sleep halt reboot"

say() { printf '\n== %s\n' "$*"; }
die() { printf 'error: %s\n' "$*" >&2; exit 1; }
sha() { sha256sum "$1" | cut -d' ' -f1; }

lock() {
	apk update -q >/dev/null && apk add -q curl >/dev/null
	mkdir -p /tmp/lock
	for kind in apk tool; do
		if [ $kind = apk ]; then pk=$IMAGE_APKS; else pk=$TOOL_APKS; fi
		for p in $pk; do
			url=$(apk fetch --simulate --url --arch x86_64 "$p" 2>/dev/null | grep -E '^https?://' | head -1)
			[ -n "$url" ] || die "no package $p"
			curl -fsSL -o "/tmp/lock/${url##*/}" "$url"
			printf '%-8s %-14s %s %s\n' "$kind" "$p" "$(sha "/tmp/lock/${url##*/}")" "$url"
		done
	done
}

fetch() { # every pinned file into the cache, checked
	mkdir -p "$CACHE"
	grep -E '^(kernel|firmware|apk|tool|license) ' "$LOCK" | while read -r kind name want url; do
		f=$CACHE/${url##*/}
		if [ -f "$f" ] && [ "$(sha "$f")" = "$want" ]; then continue; fi
		echo "  fetch ${url##*/}"
		curl -fsSL --retry 3 -o "$f.part" "$url" || die "download failed: $url (an Alpine package Alpine replaced? build.sh lock)"
		got=$(sha "$f.part")
		[ "$got" = "$want" ] || { rm -f "$f.part"; die "SHA-256 of ${url##*/} is $got, sources.lock says $want"; }
		mv "$f.part" "$f"
	done
}
cached() { f=$CACHE/$(awk -v k="$1" -v n="$2" '$1 == k && $2 == n { u = $4; sub(/.*\//, "", u); print u }' "$LOCK"); [ -f "$f" ] || die "$1 $2 is not in sources.lock"; echo "$f"; }

case "${1:-build}" in
lock) lock; exit 0 ;;
build) ;;
*) die "usage: build-inside.sh build|lock" ;;
esac

EPOCH=$(awk '$1 == "epoch" { print $4 }' "$LOCK")
export SOURCE_DATE_EPOCH=$EPOCH
rm -rf "$W" "$U" "$OUT/$NAME".*
mkdir -p "$W" "$R" "$K" "$F" "$U"

say "build tools"
apk add -q kmod zstd cpio coreutils grep mtools dosfstools sfdisk curl tar >/dev/null
fetch
apk add -q --allow-untrusted $(for p in $TOOL_APKS; do cached tool "$p"; done) >/dev/null
TOOLS=$(apk list -I 2>/dev/null | grep -E '^(kmod|zstd|cpio|coreutils|mtools|dosfstools|sfdisk|grub|grub-efi|tar)-[0-9]' | cut -d' ' -f1 | tr '\n' ' ')
echo "  $TOOLS"

say "kernel"
tar --zstd -xf "$(cached kernel linux)" -C "$K"
KVER=$(ls "$K/usr/lib/modules")
[ "$(echo "$KVER" | wc -w)" = 1 ] || die "expected one kernel version in the package, got: $KVER"
[ -f "$K/usr/lib/modules/$KVER/vmlinuz" ] || die "no vmlinuz in the kernel package"
mkdir -p "$K/lib" && mv "$K/usr/lib/modules" "$K/lib/modules"
KM=$K/lib/modules/$KVER
echo "  $KVER"
sh "$PS/image/kconfig-check.sh" "$KM/vmlinuz" "$KM/modules.builtin" "$W/config" > "$W/kconfig.txt" || { cat "$W/kconfig.txt"; die "kernel configuration check failed"; }
cat "$W/kconfig.txt"

say "modules (only what the stick loads, and what those need)"
depmod -b "$K" "$KVER"
modprobe -d "$K" -S "$KVER" --show-depends -a $ROOT_MODULES > "$W/deps.txt" || die "modprobe --show-depends failed"
M=$R/lib/modules/$KVER
mkdir -p "$M"
awk '$1 == "insmod" { print $2 }' "$W/deps.txt" | sort -u | while read -r p; do
	rel=${p#"$K"/lib/modules/"$KVER"/}
	case "$rel" in /*) die "module outside the tree: $p" ;; esac
	mkdir -p "$M/$(dirname "$rel")"
	zstd -q -d -c "$p" > "$M/${rel%.zst}"
done
cp "$KM/modules.builtin" "$KM/modules.builtin.modinfo" "$KM/modules.order" "$M/"
depmod -b "$R" "$KVER"
(cd "$M" && find kernel -name '*.ko' | sort) > "$W/modules.txt"
echo "  $(wc -l < "$W/modules.txt") modules: $(sed 's#.*/##; s#\.ko$##' "$W/modules.txt" | tr '\n' ' ')"
N=$(grep -cE '/(drivers/net|net/wireless|net/mac80211|net/bluetooth|drivers/bluetooth|drivers/usb/net|drivers/infiniband)/' "$W/modules.txt" || true)
[ "$N" = 0 ] || die "$N network or radio modules in the image"
echo "  network, Wi-Fi and Bluetooth modules in the image: 0"

say "GPU firmware (linux-firmware i915/ and xe/ only)"
tar --zstd -xf "$(cached firmware linux-firmware-intel)" -C "$F"
mkdir -p "$R/lib/firmware"
for d in i915 xe; do
	[ -d "$F/usr/lib/firmware/$d" ] || die "no $d/ in the firmware package"
	cp -a "$F/usr/lib/firmware/$d" "$R/lib/firmware/"
done
FWL=$F/usr/share/licenses/linux-firmware-intel
for l in LICENSE.i915 LICENSE.xe; do [ -f "$FWL/$l" ] || die "no $l in the firmware package"; cp "$FWL/$l" "$R/lib/firmware/"; done
# every firmware file xe and i915 name must be here (compressed: the kernel loads .zst)
: > "$W/fw-missing.txt"
for m in xe i915; do
	modinfo -b "$R" -k "$KVER" -F firmware "$m" | while read -r fw; do
		[ -e "$R/lib/firmware/$fw" ] || [ -e "$R/lib/firmware/$fw.zst" ] || echo "$m: $fw" >> "$W/fw-missing.txt"
	done
done
echo "  $(find "$R/lib/firmware" -type f | wc -l) files, $(du -sk "$R/lib/firmware" | cut -f1) KB"
[ -s "$W/fw-missing.txt" ] && { echo "  named by the drivers but not in linux-firmware $(basename "$(cached firmware linux-firmware-intel)"):"; sed 's/^/    /' "$W/fw-missing.txt"; }

say "user space (Alpine package files, unpacked)"
: > "$W/packages.txt"
for p in $IMAGE_APKS; do
	f=$(cached apk "$p")
	tar -xzf "$f" -C "$R" --exclude='.PKGINFO' --exclude='.SIGN.*' --exclude='.pre-*' --exclude='.post-*' --exclude='.trigger' 2>/dev/null
	tar -xzOf "$f" .PKGINFO 2>/dev/null | awk -v f="${f##*/}" '/^pkgname = /{ n = $3 } /^pkgver = /{ v = $3 } /^license = /{ sub(/^license = /, ""); l = $0 } /^origin = /{ o = $3 }
		END { printf "%s-%s\t%s\t%s\t%s\n", n, v, l, o, f }' >> "$W/packages.txt"
done
LD=$R/lib/ld-musl-x86_64.so.1
[ -x "$LD" ] || die "no musl loader in the image"
IMG() { "$LD" --library-path "$R/lib:$R/usr/lib" "$@"; }
# busybox applet links, from the image's own busybox; kmod's own tools stay (they read modprobe.d and the cmdline)
IMG "$R/bin/busybox" --list-full | while read -r a; do
	[ -e "$R/$a" ] || [ -L "$R/$a" ] || { mkdir -p "$R/$(dirname "$a")"; ln -s /bin/busybox "$R/$a"; }
done
# no network client or server, nothing that formats, partitions or copies raw to a drive, nothing that turns swap on
for a in wget nc netcat telnet telnetd udhcpc udhcpc6 udhcpd dhcprelay ftpget ftpput ftpd tftp tftpd httpd ntpd ping ping6 \
	traceroute traceroute6 ssl_client arping nslookup whois sendmail popmaildir inetd dnsd nameif ifenslave tunctl vconfig \
	zcip brctl ether-wake pscan route ifconfig ifup ifdown ip ipaddr iplink ipneigh iproute iprule iptunnel nbd-client \
	rdate microcom slattach ssl_client \
	dd mkfs.vfat mkdosfs mkfs.ext2 mke2fs mkswap swapon swapoff fdisk fsck shred blkdiscard fstrim fatattr fdflush losetup \
	nandwrite nanddump partprobe raidautorun hwclock rtcwake watchdog; do
	for d in bin sbin usr/bin usr/sbin; do rm -f "$R/$d/$a"; done
done
rm -rf "$R/etc/network" "$R/etc/logrotate.d" "$R/etc/busybox-paths.d" "$R/usr/share/man" "$R/usr/share/doc"
for t in busybox lspci kmod; do
	b=$(find "$R/bin" "$R/sbin" "$R/usr/bin" "$R/usr/sbin" -name "$t" \( -type f -o -type l \) 2>/dev/null | head -1)
	[ -n "$b" ] || die "$t is not in the image"
	l=$(readlink "$b" || true); case "$l" in /*) b=$R$l ;; esac
	IMG "$b" --version >/dev/null 2>&1 || IMG "$b" --help >/dev/null 2>&1 || die "does not run: ${b#"$R"}"
	echo "  runs: $t (${b#"$R"})"
done
mkdir -p "$W/bin" && ln -sf "$R/bin/kmod" "$W/bin/modinfo"   # kmod picks its tool by the name it was started as
IMG "$W/bin/modinfo" -b "$R" -k "$KVER" -F vermagic xe >/dev/null || die "the image's modinfo cannot read xe.ko"
echo "  the image's kmod reads the image's modules"

say "the stick's programs"
mkdir -p "$R/usr/lib/xe-stick" "$R/etc/xe-stick" "$R/etc/modprobe.d"
cp -R "$PS/rootfs/." "$R/"
cp "$SRC/tools/report/collect-linux.sh" "$R/usr/lib/xe-stick/collect-linux.sh"
chmod 0755 "$R/init" "$R/usr/lib/xe-stick/"*.sh
for s in "$R/init" "$R/usr/lib/xe-stick/"*.sh; do IMG "$R/bin/busybox" sh -n "$s" || die "syntax: $s"; done
printf 'root:x:0:0:root:/root:/bin/sh\n' > "$R/etc/passwd"
printf 'root:x:0:\n' > "$R/etc/group"
printf 'NAME="Xe probe stick"\nID=xe-probe-stick\nVERSION_ID=%s\nPRETTY_NAME="Xe probe stick %s (Linux %s)"\n' \
	"$VERSION" "$VERSION" "$KVER" > "$R/etc/os-release"
echo "$VERSION" > "$R/etc/xe-stick/version"
{
	echo "stick:    $NAME"
	echo "kernel:   $KVER ($(basename "$(cached kernel linux)"))"
	echo "firmware: $(basename "$(cached firmware linux-firmware-intel)")"
	echo "alpine:   $(awk '$1 == "image" && $2 == "alpine" { print $4 }' "$LOCK")"
	echo "lock:     sources.lock sha256 $(sha "$LOCK")"
} > "$R/etc/xe-stick/build.txt"
mkdir -p "$R/dev" "$R/proc" "$R/sys" "$R/run" "$R/tmp" "$R/mnt/stick" "$R/root"
rm -f "$R/dev/console" "$R/dev/null"
mknod -m 600 "$R/dev/console" c 5 1
mknod -m 666 "$R/dev/null" c 1 3

say "initramfs"
find "$R" -exec touch -h -d "@$EPOCH" {} +
(cd "$R" && find . | sort | cpio -o -H newc -R 0:0 --reproducible --quiet) > "$W/initramfs.cpio"
mkdir -p "$U/EFI/BOOT" "$U/boot/grub" "$U/REPORTS" "$U/LICENSES/linux-firmware"
zstd -q -19 --single-thread -c "$W/initramfs.cpio" > "$U/boot/initramfs.img"
cp "$KM/vmlinuz" "$U/boot/vmlinuz"
echo "  initramfs $(du -k "$U/boot/initramfs.img" | cut -f1) KB (cpio $(du -k "$W/initramfs.cpio" | cut -f1) KB), vmlinuz $(du -k "$U/boot/vmlinuz" | cut -f1) KB"

say "boot loader (GRUB, UEFI x86_64, no shim: Secure Boot must be off)"
cp "$PS/image/grub.cfg" "$U/boot/grub/grub.cfg"
grub-mkimage -O x86_64-efi -o "$U/EFI/BOOT/BOOTX64.EFI" -c "$PS/image/grub-embedded.cfg" -p /boot/grub $GRUB_MODULES
echo "  BOOTX64.EFI $(du -k "$U/EFI/BOOT/BOOTX64.EFI" | cut -f1) KB"

say "the stick's other files"
cp "$PS/stick/README.txt" "$U/README.txt"
cp "$PS/stick/REPORTS-README.txt" "$U/REPORTS/README.txt"
printf 'Xe probe stick %s. This file marks the stick; the boot loader and the scan look for it.\n' "$VERSION" > "$U/$MARKER"
cp "$FWL/LICENSE.i915" "$FWL/LICENSE.xe" "$U/LICENSES/linux-firmware/"
for l in $(awk '$1 == "license" { print $2 }' "$LOCK"); do cp "$(cached license "$l")" "$U/LICENSES/$l.txt"; done
cp "$SRC/LICENSE" "$U/LICENSES/xe-probe-stick-MIT.txt" 2>/dev/null || cp "$(cached license MIT)" "$U/LICENSES/xe-probe-stick-MIT.txt"
GRUBV=$(apk list -I 2>/dev/null | sed -n 's/^\(grub-efi-[^ ]*\) .*/\1/p')
{
	echo "THIRD-PARTY NOTICES: Xe probe stick $VERSION"
	echo
	echo "The stick's own programs (/init, scan.sh, trace-filter.awk, collect-linux.sh) are MIT licensed:"
	echo "LICENSES/xe-probe-stick-MIT.txt. Everything else is the work of others, unchanged, under the licences below;"
	echo "the licence texts are in LICENSES/. Sources: the URLs below, with the package's build recipe."
	echo
	echo "Linux kernel $KVER (GPL-2.0-only WITH Linux-syscall-note): Arch Linux package"
	echo "  $(awk '$1 == "kernel" { print $4 }' "$LOCK")"
	KPV=$(basename "$(cached kernel linux)" | sed 's/^linux-//; s/-x86_64\.pkg\.tar\.zst$//')
	echo "  build recipe: https://gitlab.archlinux.org/archlinux/packaging/packages/linux (tag $KPV); source: the"
	echo "  kernel.org release it names, with Arch's patches from https://github.com/archlinux/linux"
	echo
	echo "Intel GPU firmware (i915/, xe/; LICENSES/linux-firmware/LICENSE.i915 and LICENSE.xe): from Arch Linux's"
	echo "  $(awk '$1 == "firmware" { print $4 }' "$LOCK")"
	echo "  upstream: https://gitlab.com/kernel-firmware/linux-firmware"
	echo
	echo "GRUB ($GRUBV, GPL-3.0-or-later): EFI/BOOT/BOOTX64.EFI, made with grub-mkimage. Alpine Linux package;"
	echo "  source: https://gitlab.alpinelinux.org/alpine/aports (main/grub)"
	echo
	echo "User space in boot/initramfs.img (Alpine Linux 3.24 packages: name-version, licence, aports origin):"
	awk -F'\t' '{ printf "  %-28s %-44s https://gitlab.alpinelinux.org/alpine/aports (main/%s)\n", $1, $2, $3 }' "$W/packages.txt"
} > "$U/THIRD-PARTY-NOTICES.txt"
(cd "$U" && find . -type f ! -name SHA256SUMS | sed 's#^\./##' | sort | while read -r f; do sha256sum "$f"; done) > "$U/SHA256SUMS"

say "raw disk image (MBR, one FAT32 partition: Windows and macOS mount it, UEFI boots it)"
find "$U" -exec touch -h -d "@$EPOCH" {} +
KB=$(du -sk "$U" | cut -f1)
MB=$(( (KB / 1024 + 160 + 15) / 16 * 16 ))   # the files plus 160 MB for reports, in 16 MB steps
truncate -s $(( MB - 1 ))M "$W/part.img"
mkfs.vfat -F 32 -n "$LABEL" -i 58455052 --invariant "$W/part.img" >/dev/null
export MTOOLS_SKIP_CHECK=1
(cd "$U" && for d in $(find . -mindepth 1 -type d | sed 's#^\./##' | sort); do mmd -i "$W/part.img" "::/$d"; done
 find . -type f | sed 's#^\./##' | sort | while read -r f; do mcopy -m -i "$W/part.img" "$f" "::/$f"; done)
IMGF=$OUT/$NAME.img
truncate -s ${MB}M "$IMGF"
printf 'label: dos\nlabel-id: 0x58455052\nstart=2048, type=c, bootable\n' | sfdisk -q "$IMGF"
dd if="$W/part.img" of="$IMGF" bs=1M seek=1 conv=notrunc status=none
(cd "$OUT" && sha256sum "$NAME.img") > "$OUT/$NAME.img.sha256"

say "manifest"
{
	echo "Xe probe stick $VERSION"
	echo "image: $NAME.img  $(du -k "$IMGF" | cut -f1) KB  sha256 $(sha "$IMGF")"
	echo "files on the stick: $KB KB (boot/initramfs.img $(du -k "$U/boot/initramfs.img" | cut -f1) KB, boot/vmlinuz $(du -k "$U/boot/vmlinuz" | cut -f1) KB)"
	echo "epoch: $EPOCH"
	echo
	echo "== sources.lock (sha256 $(sha "$LOCK"))"
	grep -vE '^(#|$)' "$LOCK"
	echo
	echo "== build tools (versions from the build container's repositories)"
	echo "$TOOLS" | tr ' ' '\n' | sed '/^$/d; s/^/  /'
	echo
	echo "== kernel configuration checks"
	cat "$W/kconfig.txt"
	echo
	echo "== modules in the image ($(wc -l < "$W/modules.txt"))"
	sed 's/^/  /' "$W/modules.txt"
	echo
	echo "== firmware files in the image"
	(cd "$R/lib/firmware" && find . -type f | sed 's#^\./##' | sort | while read -r f; do printf '  %s  %s\n' "$(sha "$f")" "$f"; done)
	if [ -s "$W/fw-missing.txt" ]; then echo "  named by the drivers but missing from the package:"; sed 's/^/    /' "$W/fw-missing.txt"; fi
	echo
	echo "== packages in the image"
	sed 's/^/  /' "$W/packages.txt"
	echo
	echo "== initramfs content (sha256 of every file)"
	(cd "$R" && find . -type f | sed 's#^\./##' | sort | while read -r f; do printf '  %s  %s\n' "$(sha "$f")" "$f"; done)
	echo
	echo "== files on the stick (SHA256SUMS)"
	sed 's/^/  /' "$U/SHA256SUMS"
} > "$OUT/$NAME.manifest.txt"
chmod -R a+rX "$OUT" 2>/dev/null || true
du -sh "$U" "$IMGF"
echo "done: $IMGF"
