#!/bin/bash
# SPDX-License-Identifier: MIT
# test-package-probe.sh: package.sh with and without the Xe probe stick image (PROBE_IMG, PROBE_SKIP), on made-up inputs
# in a copy of the packager (package.sh writes its filled copies beside itself: never into this repository). It needs
# no real binaries, no Docker and no release key (a key is made for the test). Checks: no PROBE_IMG refuses before
# anything is written; PROBE_SKIP=1 packages without it; an image of another version, one whose .img.sha256 does not
# match and one without the stick's layout are refused; with PROBE_IMG, xe-probe-stick-SVER.img.zip is written, holds
# the image and its .img.sha256, is in the signed list, and the list's signature verifies; release.sh's check of the
# zip (zip-image.py verify) passes on it and fails on a damaged one.
set -uo pipefail
R=$(cd "$(dirname "$0")/.." && pwd)
W=$(mktemp -d -t ixeprobepkg)
trap 'rm -rf "$W"' EXIT
fails=0
ok()  { echo "ok    $*"; }
bad() { echo "FAIL  $*"; fails=$((fails + 1)); }
VER=0.0.0-test
N=intel-gpu-macos-driver-$VER
SVER=$(cat "$R/tools/probe-stick/VERSION")

# the packager and what it reads from the repository, in a throwaway tree
C=$W/repo
mkdir -p "$C/tools/probe-stick" "$C/kernel/HybridPM" "$C/windows"
ditto "$R/release" "$C/release"
cp "$R/tools/probe-stick/zip-image.py" "$R/tools/probe-stick/VERSION" "$C/tools/probe-stick/"
cp "$R/kernel/HybridPM/LICENSE.Lilu" "$C/kernel/HybridPM/"
cp "$R/windows/IntelXe-EFI.ps1" "$R/windows/README.md" "$C/windows/"

# made-up Tahoe and Sequoia inputs: the package layout with text files (no Mach-O, so nothing to sign)
for x in tahoe sequoia; do
	i=$W/in-$x
	for f in EFI/OC/Kexts/IntelXe.kext/Contents/MacOS/IntelXe EFI/OC/Kexts/IntelXe.kext/Contents/Info.plist \
	         EFI/OC/Kexts/HybridPM.kext/Contents/MacOS/HybridPM EFI/OC/Kexts/HybridPM.kext/Contents/Info.plist \
	         EFI/OC/Kexts/HybridPM.kext/Contents/Resources/LICENSE-Lilu.txt EFI/OC/ACPI/SSDT-ARCOFF.aml EFI/OC/ACPI/SSDT-ARCOFF.dsl \
	         hpmstat sigcheck SOURCES.md LICENSES/MIT.txt Library/GPUBundles/nvmtl/x.txt system-volume/x.txt; do
		mkdir -p "$i/$(dirname "$f")"; echo "$f" > "$i/$f"
	done
	(cd "$i/system-volume" && shasum -a 256 x.txt > SHA256SUMS)
done

# a made-up image with the stick's layout (2 MiB) and its .img.sha256, as build.sh writes them
img() { # img DIR NAME [label]
	mkdir -p "$1"
	python3 -I - "$1/$2" "${3:-XEPROBE}" <<'PY'
import hashlib, os, struct, sys
p, label = sys.argv[1], sys.argv[2]
b = bytearray(2 << 20)
b[446 + 4] = 0x0C
struct.pack_into("<II", b, 446 + 8, 2048, len(b) // 512 - 2048)
b[510:512] = b"\x55\xaa"
bs = 2048 * 512
b[bs + 71:bs + 82] = label.ljust(11).encode()
b[bs + 82:bs + 90] = b"FAT32   "
b[bs + 510:bs + 512] = b"\x55\xaa"
b[bs + 4096:bs + 4096 + 32] = hashlib.sha256(p.encode()).digest()
open(p, "wb").write(b)
open(p + ".sha256", "w").write(hashlib.sha256(b).hexdigest() + "  " + os.path.basename(p) + "\n")
PY
	echo "$1/$2"
}
IMG=$(img "$W/img" "xe-probe-stick-$SVER.img")
pkg() { # pkg OUT [VAR=value ...]: package.sh's output in $W/log
	local out=$1; shift
	env WINAPP_SKIP=1 "$@" bash "$C/release/package.sh" "$VER" "$W/in-tahoe" "$W/in-sequoia" "$out" > "$W/log" 2>&1
}

# no PROBE_IMG: refused before anything is written
if pkg "$W/out0"; then bad "packaged without PROBE_IMG"; else
	grep -q "no PROBE_IMG" "$W/log" && ok "no PROBE_IMG: refused" || bad "no PROBE_IMG: refused without saying why: $(tail -1 "$W/log")"
fi
[ ! -e "$W/out0" ] && ok "no PROBE_IMG: nothing written" || bad "no PROBE_IMG: $W/out0 was written"

# PROBE_SKIP=1: a release without it
if pkg "$W/out1" PROBE_SKIP=1; then
	ok "PROBE_SKIP=1 packages"
	grep -q xe-probe-stick "$W/out1/$N.sha256" && bad "PROBE_SKIP=1: the list names a probe image" || ok "PROBE_SKIP=1: no probe image in the list"
	ls "$W/out1" | grep -q xe-probe-stick && bad "PROBE_SKIP=1: a probe file was written" || ok "PROBE_SKIP=1: no probe file"
else bad "PROBE_SKIP=1 failed: $(tail -3 "$W/log")"; fi

# refused images
OTHER=$(img "$W/img2" "xe-probe-stick-9.9.9.img")
pkg "$W/out2" PROBE_IMG="$OTHER" && bad "an image of another version was packaged" || { grep -q "not $SVER" "$W/log" && ok "another version: refused" || bad "another version: $(tail -2 "$W/log")"; }
DMG=$(img "$W/img3" "xe-probe-stick-$SVER.img")
printf 'x' | dd of="$DMG" bs=1 seek=1500000 conv=notrunc 2>/dev/null
pkg "$W/out3" PROBE_IMG="$DMG" && bad "a damaged image was packaged" || { grep -q "not its .img.sha256" "$W/log" && ok "a damaged image: refused" || bad "a damaged image: $(tail -2 "$W/log")"; }
LBL=$(img "$W/img4" "xe-probe-stick-$SVER.img" MACOS)
pkg "$W/out4" PROBE_IMG="$LBL" && bad "an image labelled MACOS was packaged" || { grep -q "not XEPROBE" "$W/log" && ok "not the stick's layout: refused" || bad "the layout: $(tail -2 "$W/log")"; }
mv "$IMG.sha256" "$IMG.sha256.bak"
pkg "$W/out5" PROBE_IMG="$IMG" && bad "an image without its .img.sha256 was packaged" || { grep -q "no xe-probe-stick-$SVER.img.sha256" "$W/log" && ok "no .img.sha256: refused" || bad "no .img.sha256: $(tail -2 "$W/log")"; }
mv "$IMG.sha256.bak" "$IMG.sha256"
for o in out2 out3 out4 out5; do [ ! -e "$W/$o" ] || bad "$o was written for a refused image"; done

# with PROBE_IMG and a key: the zip, in the signed list
PUB=$(swift "$R/release/signing.swift" keygen "$W/test.key")
if pkg "$W/out" PROBE_IMG="$IMG" SIGN_KEY="$W/test.key"; then
	ok "packaged with PROBE_IMG"
	Z=xe-probe-stick-$SVER.img.zip
	[ -f "$W/out/$Z" ] && ok "$Z written" || bad "no $Z"
	grep -q "  $Z\$" "$W/out/$N.sha256" && ok "$Z is in $N.sha256" || bad "$Z is not in $N.sha256"
	(cd "$W/out" && shasum -a 256 -c "$N.sha256" >/dev/null) && ok "$N.sha256 matches the files" || bad "$N.sha256 does not match"
	swift "$R/release/signing.swift" verify "$PUB" "$W/out/$N.sha256" "$W/out/$N.sha256.sig" >/dev/null && ok "the list's signature verifies" || bad "the list's signature does not verify"
	[ "$(unzip -Z1 "$W/out/$Z" | sort | tr '\n' ' ')" = "xe-probe-stick-$SVER.img xe-probe-stick-$SVER.img.sha256 " ] && ok "$Z holds the image and its .img.sha256" || bad "$Z holds $(unzip -Z1 "$W/out/$Z" | tr '\n' ' ')"
	unzip -p "$W/out/$Z" "xe-probe-stick-$SVER.img" | cmp -s - "$IMG" && ok "the image in $Z is PROBE_IMG" || bad "the image in $Z differs from PROBE_IMG"
	unzip -p "$W/out/$Z" "xe-probe-stick-$SVER.img.sha256" | cmp -s - "$IMG.sha256" && ok "its .img.sha256 is PROBE_IMG's" || bad "its .img.sha256 differs"
	python3 -I "$R/tools/probe-stick/zip-image.py" verify "$W/out/$Z" --version "$SVER" >/dev/null && ok "release.sh's check passes" || bad "release.sh's check (zip-image.py verify) fails on a good zip"
	ls "$W/out" | grep -q '\.sha256$' && [ "$(ls "$W/out" | grep -c '\.sha256$')" = 1 ] && ok "one .sha256 asset (the list): the macOS app finds it by its ending" || bad "more than one .sha256 asset: $(ls "$W/out" | grep '\.sha256$' | tr '\n' ' ')"
	python3 -I - "$W/out/$Z" "$W/xe-probe-stick-$SVER.img.zip" "xe-probe-stick-$SVER.img" <<'PY'
import sys, zipfile
src, dst, name = sys.argv[1:4]
with zipfile.ZipFile(src) as a, zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as b:
    for i in a.infolist():
        data = bytearray(a.read(i.filename))
        if i.filename == name: data[1500000] ^= 1
        b.writestr(i, bytes(data))
PY
	python3 -I "$R/tools/probe-stick/zip-image.py" verify "$W/xe-probe-stick-$SVER.img.zip" >/dev/null 2>&1 && bad "release.sh's check passes a zip whose image is changed" || ok "release.sh's check refuses a zip whose image is changed"
	# package again without PROBE_IMG (PROBE_SKIP=1) into the same folder: the old zip goes
	pkg "$W/out" PROBE_SKIP=1 && [ ! -f "$W/out/$Z" ] && ok "packaging again with PROBE_SKIP=1 removes the earlier zip" || bad "an earlier $Z stayed in the folder"
else bad "packaging with PROBE_IMG failed: $(tail -5 "$W/log")"; fi

echo "== $fails failure(s)"
exit $fails
