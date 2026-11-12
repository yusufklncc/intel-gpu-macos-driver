#!/bin/sh
# SPDX-License-Identifier: MIT
# qemu-test.sh: the stick's end-to-end test. Boots out/xe-probe-stick-VERSION.img four times on an emulated UEFI PC (QEMU
# and OVMF in a Debian container, pinned in sources.lock; QEMU emulates x86_64 in software: about 30 s a boot here)
# and judges what it wrote (check-report.py). It fails if:
#   a planted serial number, the UUID or the MAC address survives anywhere in a report;
#   a network interface other than lo appears;
#   the emulated SATA disk (which carries a decoy volume with the stick's label) changes by one byte;
#   a report is missing, or its seal does not verify;
#   the trace step does not skip cleanly (QEMU has no Intel GPU);
#   boot 2's step that hangs on purpose (xestick.testhang) is not ended by its dead-man powering the PC off, or boot 3
#   does not leave that step out, or a freeze planted for another computer changes anything;
#   the safe scan (boot 4: the menu's second entry) does not write its report.
#   tools/probe-stick/test/qemu-test.sh          (after tools/probe-stick/build.sh)
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(cat "$HERE/VERSION")
IMG=$HERE/out/xe-probe-stick-$VERSION.img
[ -f "$IMG" ] || { echo "error: build the image first: tools/probe-stick/build.sh" >&2; exit 1; }
BASE=$(awk '$1 == "image" && $2 == "qemu" { print $4 }' "$HERE/sources.lock")
TAG=xe-probe-stick-qemu:$(printf '%s' "$BASE" | shasum -a 256 | cut -c1-12)
if ! docker image inspect "$TAG" >/dev/null 2>&1; then
	printf 'FROM %s\nRUN apt-get update && apt-get install -y --no-install-recommends qemu-system-x86 ovmf mtools && rm -rf /var/lib/apt/lists/*\n' "$BASE" |
		docker build -q -t "$TAG" -
fi
T=$HERE/out/test
rm -rf "$T"
mkdir -p "$T"
cp "$IMG" "$T/xe-probe-stick.img"
# the planted values (none of them is a real one)
SERIAL_SYS=XSSYSSERIAL0042 SERIAL_BOARD=XSBOARDSERIAL0099 SERIAL_STICK=XSSTICKSERIAL0815 SERIAL_DISK=XSDISKSERIAL4711
UUID=5ec2e7aa-1111-2222-3333-444455556666 MAC=$(printf '%s:' 52 54 00 5e c4)e7   # spelled in parts: no address in git
docker run --rm -v "$T":/t -v "$HERE/test/qemu-inside.sh":/qemu-inside.sh:ro \
	-e SERIAL_SYS=$SERIAL_SYS -e SERIAL_BOARD=$SERIAL_BOARD -e SERIAL_STICK=$SERIAL_STICK -e SERIAL_DISK=$SERIAL_DISK \
	-e UUID=$UUID -e MAC=$MAC -e LIMIT="${LIMIT:-1500}" "$TAG" sh /qemu-inside.sh
SECRETS="$SERIAL_SYS $SERIAL_BOARD $SERIAL_STICK $SERIAL_DISK $UUID $MAC" python3 -I "$HERE/test/check-report.py" "$T"
