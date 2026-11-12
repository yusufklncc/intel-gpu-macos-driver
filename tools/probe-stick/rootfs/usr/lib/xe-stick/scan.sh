#!/bin/sh
# SPDX-License-Identifier: MIT
# scan.sh: the Xe probe stick's scan. /init (PID 1) runs it once and turns the computer off afterwards.
#
# The order, and why:
#  1. Every block device that cannot be the stick (not USB, not removable) is set read-only in the kernel (BLKROSET,
#     busybox `blockdev --setro`) before anything is mounted. Once the stick is known, every other device is too,
#     USB drives included, and a background loop does the same for devices that appear later. The image has no NVMe
#     driver at all; SATA and virtio disks are built into the kernel, so they appear, and are locked.
#  2. The stick is found by its FAT label among USB/removable devices only, mounted read-only, checked for its marker
#     file, and only then remounted read-write. No other file system is ever mounted.
#  3. collect-linux.sh (tools/report) with no GPU driver loaded: "before".
#  4. Per Intel display-class PCI device: mmiotrace while xe (or i915) binds. Each bind is a step: a line in
#     REPORTS/xe-stick-state.txt on the stick says it is running, so if the computer freezes there, the next boot
#     skips it; and a dead-man process powers the computer off if the step does not finish.
#  5. collect-linux.sh again, with the drivers bound: "after".
#  6. Everything is redacted with the private values both runs found (collect-linux.sh --redact), checked again, and
#     written as one .tar.gz, its .txt summary and a .sha256 seal to REPORTS/ on the stick.
#
# Kernel command line switches: xestick.mode=full|safe|shell (default full; the GRUB menu sets it),
# xestick.wait=SECONDS before the power-off (default 30), xestick.trace=full (keep the accesses to the VRAM/aperture
# BARs in the trace), xestick.retry=1 (try the steps that froze earlier runs on this computer again).
# For tests only: xestick.testhang=STEP hangs in STEP (collect-before, collect-after, trace:DRIVER:BDF) under a 15 s
# dead-man, as a frozen driver would (test/qemu-test.sh).
#
# POSIX sh on busybox, by choice: the same language as collect-linux.sh, every tool it needs is a busybox applet
# (blockdev for BLKROSET, sha256sum, tar, gzip, awk, sed) and the image needs no interpreter besides.
export PATH=/usr/sbin:/usr/bin:/sbin:/bin LC_ALL=C
umask 022

LIB=/usr/lib/xe-stick
RUN=/run/xs
MNT=/mnt/stick
LABEL=XEPROBE
MARKER=xe-probe-stick.marker
TR=/sys/kernel/tracing
VERSION=$(cat /etc/xe-stick/version 2>/dev/null)
KVER=$(uname -r)
SETTLE=8                         # seconds the trace keeps running after the bind returns (display, HPD, fbdev)
TRACE_BUDGET=$((20 * 1048576))   # compressed trace bytes per report: the issue form takes files up to 25 MB

MODE=full WAIT=30 TRACEALL=0 RETRY=0 TESTHANG=""
for w in $(cat /proc/cmdline); do
	case "$w" in
	xestick.mode=*) MODE=${w#*=} ;;
	xestick.wait=*) WAIT=${w#*=} ;;
	xestick.trace=full) TRACEALL=1 ;;
	xestick.retry=1) RETRY=1 ;;
	xestick.testhang=*) TESTHANG=${w#*=} ;;
	esac
done
case "$MODE" in full|safe|shell) ;; *) MODE=full ;; esac
case "$WAIT" in ''|*[!0-9]*) WAIT=30 ;; esac

ID="XR-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n' | tr 'a-f' 'A-F')"
NAME="xe-stick-$ID"
REP=$RUN/$NAME      # the report tree, in RAM until it is packed
S=$REP/stick
mkdir -p "$S" "$REP/trace"
: > "$RUN/results"
STICK="" STICK_DISK="" MKEY="" LOCKER="" STEP=""
STATE=$MNT/REPORTS/xe-stick-state.txt

up() { cut -d' ' -f1 /proc/uptime; }
log() { printf '%8s %s\n' "$(up)" "$*" >> "$S/run.txt"; }
say() { printf '%s\n' "$*"; log "$*"; }
result() { printf '%s\n' "$*" >> "$RUN/results"; log "result: $*"; }
dmi() { cat "/sys/class/dmi/id/$1" 2>/dev/null; }

# ---------------------------------------------------------------- dead-man
# deadman SECONDS WHAT: a background process that turns the computer off unless it is killed first (disarm). It
# covers a step that can wedge the machine: a soft hang leaves this shell stuck, but not the dead-man. A hard hang of
# the kernel cannot be helped from user space; the kernel's panic switches (GRUB's command line) cover that.
deadman() {
	[ -n "$TESTHANG" ] && [ "$STEP" = "$TESTHANG" ] && set -- 15 "$2"
	(
		sleep "$1"
		echo
		echo "xe-stick: $2 did not finish within $1 s. Turning the computer off."
		echo "xe-stick: the next boot of this stick on this computer leaves that step out."
		sync
		sleep 3
		poweroff -f
		echo 1 > /proc/sys/kernel/sysrq
		echo o > /proc/sysrq-trigger
	) </dev/null &
	DM=$!
	log "dead-man armed: $2, $1 s"
}
disarm() { kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null; log "dead-man disarmed"; }

# ---------------------------------------------------------------- disks
whole() { # sdb1 -> sdb, nvme0n1p2 -> nvme0n1; a whole disk names itself
	if [ -e "/sys/class/block/$1/partition" ]; then basename "$(dirname "$(readlink -f "/sys/class/block/$1")")"; else echo "$1"; fi
}
removable() { # on USB, or flagged removable
	case "$(readlink -f "/sys/class/block/$1")" in */usb[0-9]*/*) return 0 ;; esac
	[ "$(cat "/sys/class/block/$1/removable" 2>/dev/null)" = 1 ]
}
# lock_disks fixed|all: BLKROSET on block devices. fixed: every device that cannot be the stick; all: every device
# except the stick's own disk. A device that refuses is retried on the next pass and reported once.
lock_disks() {
	for b in /sys/class/block/*; do
		[ -e "$b" ] || continue
		n=${b##*/}
		case "$n" in loop*|ram*|zram*) continue ;; esac
		grep -qxF "$n" "$RUN/locked" 2>/dev/null && continue
		d=$(whole "$n")
		[ -n "$STICK_DISK" ] && [ "$d" = "$STICK_DISK" ] && continue
		if [ "$1" = fixed ] && removable "$d"; then continue; fi
		if blockdev --setro "/dev/$n" 2>/dev/null && [ "$(blockdev --getro "/dev/$n" 2>/dev/null)" = 1 ]; then
			echo "$n" >> "$RUN/locked"
			if removable "$d"; then k="USB/removable"; else k=fixed; fi
			printf '%8s %-10s read-only  (%s, %s sectors)\n' "$(up)" "$n" "$k" "$(cat "$b/size" 2>/dev/null)" >> "$S/locks.txt"
		elif ! grep -qxF "$n" "$RUN/lockwarn" 2>/dev/null; then
			echo "$n" >> "$RUN/lockwarn"
			printf '%8s %-10s WARNING: could not be set read-only yet (retried every 2 s)\n' "$(up)" "$n" >> "$S/locks.txt"
		fi
	done
}
# find_stick: the FAT volume labelled $LABEL with the marker file, on a USB/removable device. Mounted read-only to
# look (a read-write FAT mount writes the volume's dirty flag), read-write only once it is known to be the stick.
find_stick() {
	i=0
	while [ $i -lt 60 ]; do
		lock_disks fixed
		for b in /sys/class/block/*; do
			[ -e "$b" ] || continue
			n=${b##*/}
			case "$n" in loop*|ram*|zram*) continue ;; esac
			d=$(whole "$n")
			removable "$d" || continue
			[ "$(blkid "/dev/$n" 2>/dev/null | sed -n 's/.* LABEL="\([^"]*\)".*/\1/p')" = "$LABEL" ] || continue
			mount -t vfat -o ro,nosuid,nodev,noexec "/dev/$n" "$MNT" 2>/dev/null || continue
			if [ -f "$MNT/$MARKER" ] && mount -o remount,rw,noatime,flush "$MNT" 2>/dev/null; then
				STICK=$n STICK_DISK=$d
				return 0
			fi
			umount "$MNT" 2>/dev/null
		done
		sleep 0.5
		i=$((i + 1))
	done
	return 1
}

# ---------------------------------------------------------------- steps that froze a computer before
machine_key() { # a hash of the machine's model names and its Intel GPU IDs: no serial number, nothing personal
	printf '%s|%s|%s|%s|%s|%s' "$(dmi sys_vendor)" "$(dmi product_name)" "$(dmi board_vendor)" "$(dmi board_name)" \
		"$(dmi bios_version)" "$(for b in $(intel_gpus); do cat "/sys/bus/pci/devices/$b/device"; done | tr '\n' ' ')" |
		sha256sum | cut -c1-12
}
state_init() {
	mkdir -p "$MNT/REPORTS"
	if [ ! -f "$STATE" ]; then
		{
			echo "# Xe probe stick: the steps that froze a computer. 'running KEY STEP' is written before a step and"
			echo "# removed after it; one that is still here at the next boot froze, and becomes 'skip KEY STEP': that step"
			echo "# is left out on that computer (KEY: a hash of its model names) from then on. Delete this file, or boot"
			echo "# with xestick.retry=1 on the kernel command line, to try every step again."
		} > "$STATE"
	fi
	awk -v k="$MKEY" '$1 == "running" && $2 == k { print $3 }' "$STATE" > "$RUN/froze"
	{
		grep -v "^running $MKEY " "$STATE"
		[ "$RETRY" = 1 ] || sed "s/^/skip $MKEY /" "$RUN/froze"
	} | { if [ "$RETRY" = 1 ]; then grep -v "^skip $MKEY "; else cat; fi; } | awk '!seen[$0]++' > "$RUN/state.new"
	cat "$RUN/state.new" > "$STATE"
	sync
	awk -v k="$MKEY" '$1 == "skip" && $2 == k { print $3 }' "$STATE" > "$RUN/skiplist"
}
step_begin() { # STEP: 1 when the step froze this computer before (skip it)
	if grep -qxF "$1" "$RUN/skiplist" 2>/dev/null; then
		echo "$1" >> "$RUN/skipped"
		log "skip $1: it froze an earlier run on this computer"
		return 1
	fi
	echo "running $MKEY $1" >> "$STATE"
	sync
	STEP=$1
	log "begin $1"
}
test_hang() { # the test switch: hang here, as a frozen driver bind would, until the dead-man fires
	[ -n "$TESTHANG" ] && [ "$STEP" = "$TESTHANG" ] || return 0
	say "test: xestick.testhang=$STEP: hanging here until the dead-man turns the computer off"
	while :; do sleep 60; done
}
step_end() {
	grep -vxF "running $MKEY $1" "$STATE" > "$RUN/state.new"
	cat "$RUN/state.new" > "$STATE"
	sync
	STEP=""
	log "end $1"
}

# ---------------------------------------------------------------- collect-linux.sh
collect() { # before|after
	step_begin "collect-$1" || { result "collect-$1: skipped (it froze an earlier run on this computer)"; return 1; }
	deadman 420 "collecting ($1)"
	dm=$DM
	test_hang
	mkdir -p "$RUN/c-$1"
	sh "$LIB/collect-linux.sh" --out "$RUN/c-$1" --id "$ID" --acpi --time 150 --literals-out "$RUN/lit-$1" > "$S/collect-$1.txt" 2>&1
	rc=$?
	disarm "$dm"
	step_end "collect-$1"
	f=$RUN/c-$1/xe-report-$ID.tar.gz
	if [ -f "$f" ] && tar -xzf "$f" -C "$RUN/c-$1" && mv "$RUN/c-$1/xe-report-$ID" "$REP/$1"; then
		rm -f "$f" "$REP/$1/manifest.txt"   # the report's SEAL.sha256 replaces it
		log "collect-$1: done"
	else
		result "collect-$1: no report (collect-linux.sh exit $rc; stick/collect-$1.txt)"
	fi
}

# ---------------------------------------------------------------- mmiotrace
intel_gpus() {
	for d in /sys/bus/pci/devices/*; do
		[ "$(cat "$d/vendor" 2>/dev/null)" = 0x8086 ] || continue
		case "$(cat "$d/class" 2>/dev/null)" in 0x03*) echo "${d##*/}" ;; esac
	done
}
trace_sizes() { # the ring buffer per CPU and the raw-trace cap, from the RAM this computer has
	memkb=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)
	ncpu=$(awk -F, '{ n = 0; for (i = 1; i <= NF; i++) { k = split($i, a, "-"); n += (k == 2 ? a[2] - a[1] + 1 : 1) } print n }' /sys/devices/system/cpu/possible 2>/dev/null)
	[ "${ncpu:-0}" -gt 0 ] || ncpu=1
	BUFKB=$((memkb / 16 / ncpu))
	[ $BUFKB -gt 65536 ] && BUFKB=65536
	[ $BUFKB -lt 4096 ] && BUFKB=4096
	RAWCAP=$((memkb / 10 * 1024))
	[ $RAWCAP -gt $((768 * 1048576)) ] && RAWCAP=$((768 * 1048576))
	log "trace sizes: $memkb KB RAM, $ncpu possible CPUs: ring buffer $BUFKB KB per CPU, raw cap $RAWCAP bytes"
}
trace_start() { # TAG: mmiotrace on, its pipe read into RAM (a raw-size cap stops the reader)
	TERR=""
	[ -w "$TR/current_tracer" ] || { TERR="no tracefs at $TR"; return 1; }
	grep -qw mmiotrace "$TR/available_tracers" 2>/dev/null || { TERR="this kernel has no mmiotrace tracer"; return 1; }
	echo nop > "$TR/current_tracer"
	echo "$BUFKB" > "$TR/buffer_size_kb" 2>/dev/null || log "could not set buffer_size_kb to $BUFKB"
	RAW=$RUN/$1.raw
	: > "$RAW"
	echo mmiotrace > "$TR/current_tracer" 2>/dev/null || { TERR="could not start mmiotrace (kernel lockdown?)"; return 1; }
	cat "$TR/trace_pipe" >> "$RAW" &
	TPID=$!
	(
		while sleep 1; do
			[ "$(stat -c %s "$RAW")" -lt "$RAWCAP" ] && continue
			kill "$TPID" 2>/dev/null
			echo "the raw trace reached $RAWCAP bytes; what followed was not kept" > "$RAW.cap"
			exit 0
		done
	) &
	TCAP=$!
	log "mmiotrace on"
}
trace_stop() {
	s0=-1 i=0
	while [ $i -lt 10 ]; do   # let the reader drain the buffer: until the file stops growing, at most 5 s
		s=$(stat -c %s "$RAW")
		[ "$s" = "$s0" ] && break
		s0=$s
		sleep 0.5
		i=$((i + 1))
	done
	# the pipe closes before the tracer changes: an open trace_pipe keeps the tracer busy
	kill "$TPID" "$TCAP" 2>/dev/null
	wait "$TPID" "$TCAP" 2>/dev/null
	OVERRUN=$(cat "$TR"/per_cpu/cpu*/stats 2>/dev/null | awk '/^overrun:/ { s += $2 } END { print s + 0 }')
	echo nop > "$TR/current_tracer" 2>/dev/null || log "WARNING: could not switch mmiotrace off"
	log "mmiotrace off: $(stat -c %s "$RAW") bytes, overrun $OVERRUN"
}
# trace_keep TAG BDF: the raw trace without the accesses to the device's BARs other than BAR0 (registers and GGTT
# stay; VRAM and the aperture go, unless xestick.trace=full), into the report as text (redacted and compressed later)
trace_keep() {
	ranges=""
	[ "$TRACEALL" = 1 ] || ranges=$(awk 'NR >= 2 && NR <= 6 && $1 != $2 && $1 !~ /^0x0+$/ { printf "%s %s ", $1, $2 }' "/sys/bus/pci/devices/$2/resource" 2>/dev/null)
	awk -v ranges="$ranges" -v stats="$RUN/$1.stats" -f "$LIB/trace-filter.awk" "$RAW" > "$REP/trace/$1.mmio"
	rawsize=$(stat -c %s "$RAW")
	rm -f "$RAW"
	{
		echo "raw bytes:    $rawsize$( [ -f "$RAW.cap" ] && printf ' (%s)' "$(cat "$RAW.cap")")"
		echo "overrun:      $OVERRUN (events the ring buffer lost)"
		echo "left out:     ${ranges:-nothing} (BAR ranges other than BAR0)"
		sed 's/^/filter:       /' "$RUN/$1.stats"
	} >> "$REP/trace/$1.txt"
	rm -f "$RAW.cap"
}
# trace_one BDF DRIVER: 0 when DRIVER ended up bound to BDF, 2 when the step was skipped
trace_one() {
	bdf=$1 drv=$2
	tag="$drv-$(printf '%s' "$bdf" | tr ':.' '--')"
	step="trace:$drv:$bdf"
	if ! step_begin "$step"; then
		result "$bdf $drv: skipped (it froze an earlier run on this computer)"
		return 2
	fi
	say "  $bdf: $drv starts now, recorded by mmiotrace. The screen may go black or flicker for a minute."
	n0=$(dmesg | wc -l)
	traced=1
	trace_start "$tag" || { traced=0; log "no trace for $tag: $TERR"; }
	deadman 180 "starting $drv on $bdf"
	dm=$DM
	test_hang
	[ $traced = 1 ] && echo "xe-stick: bind $drv $bdf" > "$TR/trace_marker"
	t0=$(up)
	echo "$bdf" > "/sys/bus/pci/drivers/$drv/bind" 2> "$RUN/bind.err"
	rc=$?
	t1=$(up)
	sleep $SETTLE
	if [ $traced = 1 ]; then
		echo "xe-stick: end $drv $bdf" > "$TR/trace_marker"
		trace_stop
	fi
	disarm "$dm"
	step_end "$step"
	now=$(basename "$(readlink "/sys/bus/pci/devices/$bdf/driver" 2>/dev/null)" 2>/dev/null)
	dmesg | tail -n +$((n0 + 1)) > "$REP/trace/$tag.dmesg.txt"
	{
		echo "device:       $bdf $(cat "/sys/bus/pci/devices/$bdf/vendor"):$(cat "/sys/bus/pci/devices/$bdf/device") class $(cat "/sys/bus/pci/devices/$bdf/class") boot_vga $(cat "/sys/bus/pci/devices/$bdf/boot_vga" 2>/dev/null)"
		echo "driver:       $drv (bind write: exit $rc$( [ -s "$RUN/bind.err" ] && printf ', %s' "$(tr '\n' ' ' < "$RUN/bind.err")"))"
		echo "bound now:    ${now:-nothing}"
		echo "bind took:    $(awk -v a="$t0" -v b="$t1" 'BEGIN { printf "%.2f s", b - a }'), then $SETTLE s more recorded"
		echo "kernel log:   $tag.dmesg.txt ($(wc -l < "$REP/trace/$tag.dmesg.txt") lines)"
		[ $traced = 1 ] || echo "trace:        none: $TERR"
	} > "$REP/trace/$tag.txt"
	if [ $traced = 1 ]; then
		trace_keep "$tag" "$bdf"
		k=$(awk '$1 == "kept" { print $2 }' "$RUN/$tag.stats")
		x=$(awk '$1 == "dropped" { print $2 }' "$RUN/$tag.stats")
		tr_note="trace $k lines, $x VRAM/aperture accesses left out, overrun $OVERRUN"
	else
		tr_note="no trace: $TERR"
	fi
	if [ "$now" = "$drv" ]; then result "$bdf $drv: bound; $tr_note"; else result "$bdf $drv: not bound (bind exit $rc); $tr_note"; fi
	[ "$now" = "$drv" ]
}
trace_all() {
	gpus=$(intel_gpus)
	if [ -z "$gpus" ]; then
		result "trace: skipped: no Intel display-class PCI device (vendor 8086, class 03) in this computer"
		say "  No Intel display device in this computer: the trace step is skipped."
		return 0
	fi
	# the drivers load with automatic binding off, so that each device binds alone, under its own trace
	echo 0 > /sys/bus/pci/drivers_autoprobe
	modprobe -a xe i915 > "$S/modprobe.txt" 2>&1 || log "modprobe xe i915: exit $? (stick/modprobe.txt)"
	net_snap drivers
	trace_sizes
	for b in $gpus; do
		mods=" $(modprobe -R "$(cat "/sys/bus/pci/devices/$b/modalias")" 2>/dev/null | tr '\n' ' ') "
		log "$b: drivers that know it: $mods"
		cur=$(basename "$(readlink "/sys/bus/pci/devices/$b/driver" 2>/dev/null)" 2>/dev/null)
		if [ -n "$cur" ]; then result "$b: already driven by $cur before the trace step: not traced"; continue; fi
		tried=0
		for drv in xe i915; do
			case "$mods" in *" $drv "*) ;; *) continue ;; esac
			[ -d "/sys/bus/pci/drivers/$drv" ] || { result "$b $drv: the driver did not load (stick/modprobe.txt)"; continue; }
			tried=1
			trace_one "$b" "$drv" && break
		done
		[ $tried = 1 ] || result "$b 8086:$(sed 's/^0x//' "/sys/bus/pci/devices/$b/device"): xe and i915 of Linux $KVER do not know this device ID: no driver to trace"
	done
	echo 1 > /sys/bus/pci/drivers_autoprobe
}

# ---------------------------------------------------------------- report
net_snap() { printf '%-10s %s\n' "$1:" "$(ls /sys/class/net 2>/dev/null | tr '\n' ' ')" >> "$S/net.txt"; }
net_only_lo() { awk '{ for (i = 2; i <= NF; i++) if ($i != "lo") bad = 1 } END { exit bad }' "$S/net.txt"; }
summary() {
	{
		echo "Xe probe stick report $ID"
		echo "Stick $VERSION, Linux $KVER, mode $MODE. The clock is the firmware's; it may be wrong."
		echo "Created $(date '+%Y-%m-%d %H:%M %z')"
		echo
		echo "Intel display devices"
		g=$(intel_gpus)
		if [ -n "$g" ]; then
			for b in $g; do
				d=/sys/bus/pci/devices/$b
				printf '  %s 8086:%s rev %s class %s%s\n' "$b" "$(sed 's/^0x//' "$d/device")" "$(cat "$d/revision")" "$(cat "$d/class")" \
					"$( [ "$(cat "$d/boot_vga" 2>/dev/null)" = 1 ] && echo ', the boot display')"
			done
		else
			echo "  none"
		fi
		echo
		echo "Steps"
		if [ -s "$RUN/results" ]; then sed 's/^/  /' "$RUN/results"; else echo "  all ran"; fi
		[ "$MODE" = safe ] && echo "  safe mode: no GPU driver was started (no trace, no 'after' report)"
		echo
		echo "Earlier runs on this computer (key $MKEY: a hash of its model names and GPU IDs, nothing personal)"
		if [ -s "$RUN/froze" ]; then echo "  the last run stopped during: $(tr '\n' ' ' < "$RUN/froze")"; else echo "  the last run, if any, finished"; fi
		if [ -s "$RUN/skiplist" ]; then echo "  left out on this computer: $(tr '\n' ' ' < "$RUN/skiplist")"; fi
		echo
		echo "Safety"
		if [ -s "$RUN/locked" ]; then echo "  set read-only before the stick was mounted, or as they appeared: $(tr '\n' ' ' < "$RUN/locked")"
		else echo "  no disk besides the stick was present (the image has no NVMe driver; SATA and virtio disks would be locked)"; fi
		[ -s "$RUN/lockwarn" ] && echo "  WARNING: could not set read-only at first: $(tr '\n' ' ' < "$RUN/lockwarn") (stick/locks.txt)"
		echo "  mounted: only the stick ($STICK)"
		if net_only_lo; then echo "  network interfaces: none besides lo, at every check"; else echo "  WARNING: a network interface appeared (stick/net.txt)"; fi
		echo
		echo "Files: before/ and after/ (collect-linux.sh, without and with the GPU driver), trace/ (mmiotrace),"
		echo "stick/ (this scan's log, kernel log, disk locks, network checks), SEAL.sha256 (SHA-256 of every file)."
		for p in after before; do
			[ -f "$REP/$p/summary.txt" ] || continue
			echo
			echo "-------- collect-linux.sh ($p) --------"
			cat "$REP/$p/summary.txt"
			break
		done
	} > "$REP/summary.txt"
}
pack_traces() { # gzip each trace within the report's trace budget; a trace over it keeps its head
	left=$TRACE_BUDGET
	for t in "$REP"/trace/*.mmio; do
		[ -f "$t" ] || continue
		gzip -9 -c "$t" > "$t.gz"
		gz=$(stat -c %s "$t.gz") sz=$(stat -c %s "$t")
		if [ "$gz" -gt "$left" ]; then
			keep=$(awk -v s="$sz" -v g="$gz" -v l="$left" 'BEGIN { printf "%d", s * l / g * 0.9 }')
			if [ "$keep" -lt 65536 ]; then
				rm -f "$t.gz"
				echo "kept:         nothing, the report's trace budget was used up" >> "${t%.mmio}.txt"
				result "$(basename "$t" .mmio): trace not kept (the report's size budget was used up)"
			else
				{ head -c "$keep" "$t"; printf '\nMARK 0.000000 xe-stick: cut here; the report keeps the first %s of %s bytes\n' "$keep" "$sz"; } | gzip -9 -c > "$t.gz"
				echo "kept:         the first $keep of $sz bytes (the report's size budget)" >> "${t%.mmio}.txt"
				result "$(basename "$t" .mmio): trace cut to its first $keep bytes (size budget)"
				gz=$(stat -c %s "$t.gz")
			fi
		fi
		[ -f "$t.gz" ] && left=$((left - gz))
		rm -f "$t"
	done
}
finish() {
	net_snap end
	[ -n "$LOCKER" ] && kill "$LOCKER" 2>/dev/null
	dmesg > "$S/dmesg.txt"
	lsmod > "$S/modules.txt" 2>&1
	cat /proc/mounts > "$S/mounts.txt"
	cat /proc/cmdline > "$S/cmdline.txt"
	cp /etc/xe-stick/build.txt "$S/build.txt"
	{
		echo "machine key: $MKEY"
		echo "stopped during, last run: $(tr '\n' ' ' < "$RUN/froze")"
		echo "left out on this computer: $(tr '\n' ' ' < "$RUN/skiplist")"
		echo "skipped this run: $(tr '\n' ' ' < "$RUN/skipped" 2>/dev/null)"
	} > "$S/stages.txt"
	summary
	# one redaction for everything, with the values both collect-linux.sh runs found
	cat "$RUN"/lit-* 2>/dev/null | sort -u > "$RUN/literals"
	{
		echo "private values found on this computer: $(grep -c . "$RUN/literals")"
		cut -f1 "$RUN/literals" | sort | uniq -c | awk '{ printf "  %s: %s\n", $2, $1 }'
		echo "(the values themselves never leave RAM)"
	} > "$S/redaction.txt"
	if ! sh "$LIB/collect-linux.sh" --redact "$RUN/literals" "$REP" > "$RUN/redact.log" 2>&1; then
		say ""
		say "The privacy check failed: a private value was still in the report after redaction, so no report was written."
		{
			echo "Xe probe stick $ID: the privacy check failed, so no report was written."
			echo "Please open an issue and say so; no file is needed. What the check found (file names only):"
			sed "s#$RUN/##g" "$RUN/redact.log"
		} > "$MNT/REPORTS/$NAME-privacy-check-failed.txt"
		sync
		return 1
	fi
	sed "s#$RUN/##g" "$RUN/redact.log" >> "$S/redaction.txt"
	pack_traces
	(cd "$REP" && find . -type f ! -name SEAL.sha256 | sed 's#^\./##' | sort | while read -r f; do sha256sum "$f"; done) > "$REP/SEAL.sha256"
	tar -czf "$RUN/$NAME.tar.gz" -C "$RUN" "$NAME" || { say "Could not pack the report."; return 1; }
	D=$MNT/REPORTS
	need=$(( $(stat -c %s "$RUN/$NAME.tar.gz") / 1024 + 256 ))
	free=$(df -k "$MNT" | awk 'NR == 2 { print $4 }')
	if [ "${free:-0}" -lt "$need" ]; then
		say "The stick is full ($free KB free, $need KB needed): move the old reports in REPORTS/ off it and scan again."
		return 1
	fi
	cp "$RUN/$NAME.tar.gz" "$D/$NAME.tar.gz.part" && mv "$D/$NAME.tar.gz.part" "$D/$NAME.tar.gz" &&
		cp "$REP/summary.txt" "$D/$NAME.txt" &&
		(cd "$D" && sha256sum "$NAME.tar.gz" "$NAME.txt") > "$D/$NAME.sha256" || { say "Could not write the report to the stick."; sync; return 1; }
	sync
	log "report written: REPORTS/$NAME.tar.gz"
	return 0
}

# ---------------------------------------------------------------- main
say ""
say "Xe probe stick $VERSION ($MODE): Linux $KVER"
say "Nothing is installed and nothing is sent: this computer has no network here, and its disks are read-only."
net_snap start
lock_disks fixed
modprobe -q -a usb-storage uas vfat nls_iso8859_1 nls_utf8 2>/dev/null
say "Looking for the stick..."
if ! find_stick; then
	say "The stick (a FAT volume labelled $LABEL with $MARKER) was not found, so nothing can be saved."
	say "Turning off in 60 s."
	sleep 60
	exit 1
fi
lock_disks all
( while :; do lock_disks all; sleep 2; done ) </dev/null &
LOCKER=$!
log "stick: /dev/$STICK (disk $STICK_DISK), every other block device read-only from now on"
MKEY=$(machine_key)
state_init
log "machine key $MKEY; stopped during last run: $(tr '\n' ' ' < "$RUN/froze"); left out: $(tr '\n' ' ' < "$RUN/skiplist")"

if [ "$MODE" = shell ]; then
	say "Shell. The stick is at $MNT; every other disk is read-only. 'poweroff -f' turns the computer off."
	say "The scan itself: sh $LIB/scan.sh is NOT run in this mode; collect-linux.sh is at $LIB."
	while :; do setsid -c sh -i </dev/tty1 >/dev/tty1 2>&1 || sh -i; sleep 1; done
fi

if [ -s "$RUN/froze" ]; then
	say ""
	say "The last scan on this computer stopped during: $(tr '\n' ' ' < "$RUN/froze")"
	say "That step is left out this time (and from now on on this computer)."
fi
if [ "$MODE" = full ]; then deadman 2400 "the scan"; else deadman 900 "the scan"; fi
GDM=$DM

say ""
say "1/3 Reading the hardware without a GPU driver (about a minute)..."
collect before
if [ "$MODE" = full ]; then
	say "2/3 Starting the GPU driver under mmiotrace, one device at a time..."
	trace_all
	net_snap traced
	say "3/3 Reading the hardware with the GPU driver running (about a minute)..."
	collect after
fi
say "Writing the report..."
if finish; then
	say ""
	say "Done. On the stick: REPORTS/$NAME.tar.gz, with $NAME.txt (a summary) and $NAME.sha256."
	say "Report ID: $ID. Attach the .tar.gz to a GitHub issue and put the ID in the form."
	sed -n '/^Steps$/,/^$/p' "$REP/summary.txt"
fi
disarm "$GDM"
say "Turning the computer off in $WAIT s."
sleep "$WAIT"
exit 0
