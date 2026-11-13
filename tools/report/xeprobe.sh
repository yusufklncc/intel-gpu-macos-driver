#!/bin/sh
# SPDX-License-Identifier: MIT
# xeprobe.sh: run XeProbe's stages from Terminal, one at a time, and keep the same record IntelXe Manager's Staged probe
# page keeps (docs/PROBE-MODE.md). For testers of an Intel GPU the driver does not support yet.
#
#   sh xeprobe.sh                 what XeProbe sees this start (debug.xeprobe.info) and the results so far
#   sh xeprobe.sh 1               run stage 1 (then 2, 3, 4, 5; 9 releases); asks for your password (sudo)
#   sh xeprobe.sh 3 --crumb       the same with the NVRAM breadcrumb on (only when looking for a freeze: it wears flash)
#   sh xeprobe.sh --report        make the diagnostics report with the results and the record (collect-macos.sh --probe)
#
# Each run is appended to ~/Library/Application Support/IntelXe Manager/xeprobe-history.txt with the results after it,
# so the results of a start that hung are still in the report. Stages 3 and 4 can freeze or restart the Mac: save your
# work first, keep a working EFI on a USB stick, and restart when you are done.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
HERE=$(cd "$(dirname "$0")" && pwd)
HIST="$HOME/Library/Application Support/IntelXe Manager/xeprobe-history.txt"

usage() { sed -n '3,15p' "$0" | sed 's/^# \{0,1\}//'; exit 2; }
attached() { sysctl -n debug.xeprobe.info > /dev/null 2>&1; }

CRUMB=0 STAGE="" REPORT=0
for a in "$@"; do
	case $a in
		1|2|3|4|5|9) STAGE=$a ;;
		--crumb) CRUMB=1 ;;
		--report) REPORT=1 ;;
		*) usage ;;
	esac
done

if [ $REPORT = 1 ]; then
	C="$HERE/collect-macos.sh"
	[ -f "$C" ] || { echo "collect-macos.sh is not next to this script ($HERE)"; exit 1; }
	if [ -f "$HIST" ]; then XE_PROBE_HISTORY=$HIST sh "$C" --probe; else sh "$C" --probe; fi
	exit $?
fi

if ! attached; then
	echo "XeProbe is not attached this start."
	case " $(sysctl -n kern.bootargs 2>/dev/null) " in
		*" xeprobe="*) echo "The boot-args name a device, so IntelXe.kext (with XeProbe) is not loaded, or the id is not the GPU's." ;;
		*) echo "Add the boot-arg xeprobe=0xDEVICE (IntelXe Manager's Staged probe page, or efi.sh --xeprobe-apply) and restart." ;;
	esac
	echo "The GPUs here (the boot-arg takes the Device ID as it is, e.g. xeprobe=0x64a0):"
	system_profiler SPDisplaysDataType 2>/dev/null | grep -E 'Chipset Model|Vendor|Device ID' | sed 's/^ */  /'
	exit 1
fi

if [ -z "$STAGE" ]; then
	sysctl -n debug.xeprobe.info
	echo
	sysctl -n debug.xeprobe.results
	exit 0
fi

case $STAGE in
	3|4) printf 'Stage %s can freeze or restart the Mac. Save your work first. Run it? [y/N] ' "$STAGE"
	     read -r ok; case $ok in y|Y|yes) ;; *) echo "Not run."; exit 1 ;; esac ;;
esac
when=$(date -u +%Y-%m-%dT%H:%M:%SZ)
boot=$(sysctl -n kern.boottime | sed -E 's/.*sec = ([0-9]+).*/\1/')
boot=$(date -u -r "$boot" +%Y-%m-%dT%H:%M:%SZ 2>/dev/null || echo unknown)
sudo sysctl -w debug.xeprobe.nvram=$CRUMB > /dev/null
sudo sysctl -w debug.xeprobe.stage="$STAGE"
rc=$?
results=$(sysctl -n debug.xeprobe.results)
mkdir -p "$(dirname "$HIST")"
[ -f "$HIST" ] || echo "# XeProbe stage runs by IntelXe Manager, newest last (docs/PROBE-MODE.md)" > "$HIST"
printf '== %s boot %s stage %s exit %s\n%s\n' "$when" "$boot" "$STAGE" "$rc" "$results" >> "$HIST"
echo
echo "$results"
echo
[ $rc = 0 ] && echo "Stage $STAGE passed." || echo "Stage $STAGE did not pass (exit $rc): sysctl debug.xeprobe.log says why."
exit $rc
