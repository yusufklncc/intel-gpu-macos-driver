#!/bin/sh
# SPDX-License-Identifier: MIT
# fetch.sh: download the Battlemage GuC and HuC firmware from linux-firmware into this directory and check them.
#
#   firmware/fetch.sh            fetch bmg_guc_70.bin and bmg_huc.bin (skips files that are already present and good)
#   LINUX_FIRMWARE_REF=<tag> firmware/fetch.sh    fetch from a linux-firmware tag or commit instead of main
#   firmware/fetch.sh --probe [STEM...]   also the GuC images XeProbe builds in for stage 3 on other Intel GPUs
#                                (probe-guc.sha256: tgl adlp dg1 dg2 mtl lnl ptl, or only the stems named, e.g. lnl),
#                                from the linux-firmware tag that file pins (LINUX_FIRMWARE_PROBE_REF overrides it)
#
# The blobs are Intel's redistributable binaries under LICENSE.xe (linux-firmware LICENSES/LICENSE.xe, copied here).
# They are not committed to this repository. kernel/IntelXe/build.py builds them into IntelXe.kext and checks the same
# sha256 values; a newer linux-firmware may carry newer firmware, in which case the check fails on purpose: the kext
# was brought up and tested with exactly these versions (GuC 70.72.1, HuC 8.2.10).
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REF=${LINUX_FIRMWARE_REF:-main}
PROBE=0 STEMS=""
while [ $# -gt 0 ]; do
	case $1 in
		--probe) PROBE=1;;
		-*) sed -n '3,12p' "$0" >&2; exit 2;;
		*) [ $PROBE = 1 ] || { sed -n '3,12p' "$0" >&2; exit 2; }; STEMS="$STEMS $1";;
	esac
	shift
done
PINS=$HERE/probe-guc.sha256
for s in $STEMS; do
	grep -qE "^[0-9a-f]{64}  [a-z0-9]+/${s}_guc_70\.bin " "$PINS" || { echo "no GuC image for '$s' in $PINS" >&2; exit 2; }
done

sha() { shasum -a 256 "$1" | cut -d' ' -f1; }

fetch() { # name sha256 [path in linux-firmware [ref]]
	name=$1; want=$2; path=${3:-xe/$1}; ref=${4:-$REF}; dst=$HERE/$name
	if [ -f "$dst" ] && [ "$(sha "$dst")" = "$want" ]; then echo "ok      $name"; return 0; fi
	for base in "https://gitlab.com/kernel-firmware/linux-firmware/-/raw/$ref" \
		"https://git.kernel.org/pub/scm/linux/kernel/git/firmware/linux-firmware.git/plain"; do
		url=$base/$path
		[ "$base" = "${base#https://git.kernel.org}" ] || url=$url?h=$ref
		if curl -fsSL -o "$dst.part" "$url"; then
			got=$(sha "$dst.part")
			if [ "$got" = "$want" ]; then mv "$dst.part" "$dst"; echo "fetched $name ($url)"; return 0; fi
			echo "sha256 mismatch for $name from $url: $got" >&2
		fi
		rm -f "$dst.part"
	done
	echo "could not fetch $name with sha256 $want" >&2
	return 1
}

# Values from the files the driver was tested with.
fetch bmg_guc_70.bin de81c75f46a127c33cd59f604d800e9ffc7ed3495967ba0d8767cd6985ab398b   # GuC 70.72.1
fetch bmg_huc.bin    747452aa8c4ed7760c68a80f3d913eafde9304d6f4db481e3f2aebb0a818bf15   # HuC 8.2.10

# XeProbe's GuC images (--probe): the list, the paths and the pinned tag are in probe-guc.sha256
[ $PROBE = 1 ] || exit 0
PREF=${LINUX_FIRMWARE_PROBE_REF:-$(sed -nE 's/.*linux-firmware tag ([0-9A-Za-z._-]+).*/\1/p' "$PINS" | head -1)}
[ -n "$PREF" ] || { echo "$PINS names no linux-firmware tag" >&2; exit 1; }
bad=0
while read -r want path ver; do
	case $want in ''|'#'*) continue;; esac
	name=${path##*/}
	if [ -n "$STEMS" ]; then
		case " $STEMS " in *" ${name%_guc_70.bin} "*) ;; *) continue;; esac
	fi
	fetch "$name" "$want" "$path" "$PREF" || bad=1
	[ -f "$HERE/$name" ] && echo "        $name: GuC $ver, for XeProbe stage 3 (linux-firmware $PREF $path)"
done < "$PINS"
exit $bad
