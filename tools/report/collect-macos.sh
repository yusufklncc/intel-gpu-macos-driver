#!/bin/sh
# collect-macos.sh: diagnostics report for the Intel GPU macOS driver stack (v2).
#
# Read-only. Uses only tools that ship with macOS (no Command Line Tools, no python3).
# Needs no root; root-only driver counters are added when `sudo -n` works without a password.
# Writes xe-report-XR-xxxxxxxx.tar.gz to the current directory (or --out DIR).
# Names, serial numbers, UUIDs, MAC addresses, host names and Wi-Fi network names are removed
# before the archive is written; the run stops without an archive if the final check still finds one.
#
#   sh collect-macos.sh [--days N] [--tests] [--no-mount] [--sudo-mount] [--probe] [--out DIR]
#
#   --days N     panic and crash reports from the last N days (default 7)
#   --tests      also run the quick GPU self-tests (vk_smoke, mtl_tests) if they are installed
#   --no-mount   do not try to mount an unmounted EFI partition (read-only) to read config.plist
#   --sudo-mount mount the EFI partition the machine booted from with sudo (read-only), if sudo needs no password
#   --probe      also collect the staged-probe results (XeProbe) and the previous boot's NVRAM breadcrumb
#   --acpi       also the DSDT and SSDT tables as macOS sees them (after OpenCore's patches and added SSDTs), from the
#                I/O Registry; never MSDM or SLIC (the Windows licence)
#   --out DIR    where to write the archive (default: the current directory)

COLLECTOR_VERSION=3
PATH=/usr/bin:/bin:/usr/sbin:/sbin:/usr/local/bin:/opt/homebrew/bin
export PATH
LC_ALL=C
LANG=C
export LC_ALL LANG
COPYFILE_DISABLE=1   # no AppleDouble ._ files in the tar
export COPYFILE_DISABLE
# Started through AppleScript's "with administrator privileges" (IntelXe Manager's administrator mode) the shell gets
# SIGTERM (and others) ignored, and a non-interactive shell cannot trap a signal it got ignored: run_to's watchdog then
# never stopped, so every command waited its full time limit (a report took minutes, 10 Nov). Start again once with the
# signals back to their defaults; perl is part of macOS.
if [ -z "${XE_SIGRESET:-}" ] && [ -x /usr/bin/perl ]; then
	XE_SIGRESET=1; export XE_SIGRESET
	exec /usr/bin/perl -e '$SIG{$_} = "DEFAULT" for qw(TERM INT HUP QUIT CHLD PIPE ALRM USR1 USR2); exec @ARGV or exit 127' /bin/sh "$0" "$@"
fi
umask 077

DAYS=7
TESTS=0
MOUNT=1
SUDOMOUNT=0
PROBE=0
ACPI=0
OUTDIR=$(pwd)
while [ $# -gt 0 ]; do
	case "$1" in
	--days) DAYS=$2; shift ;;
	--days=*) DAYS=${1#--days=} ;;
	--tests) TESTS=1 ;;
	--no-mount) MOUNT=0 ;;
	--sudo-mount) SUDOMOUNT=1 ;;
	--probe) PROBE=1 ;;
	--acpi) ACPI=1 ;;
	--out) OUTDIR=$2; shift ;;
	--out=*) OUTDIR=${1#--out=} ;;
	-h|--help) sed -n '2,21p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
case "$DAYS" in ''|*[!0-9]*) echo "--days needs a number" >&2; exit 2 ;; esac
[ -d "$OUTDIR" ] && [ -w "$OUTDIR" ] || { echo "cannot write to $OUTDIR" >&2; exit 2; }
OUTDIR=$(cd "$OUTDIR" && pwd)

START=$(date +%s)
BUDGET=60
[ "$TESTS" = 1 ] && BUDGET=150
DEADLINE=$((START + BUDGET))

ID="XR-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n' | tr 'a-f' 'A-F')"
NAME="xe-report-$ID"
TMPBASE=$(mktemp -d "${TMPDIR:-/tmp}/xe-report.XXXXXX") || { echo "mktemp failed" >&2; exit 1; }
W="$TMPBASE/$NAME"
SECRETS="$TMPBASE/.literals"   # values to remove; never part of the archive
SEDF="$TMPBASE/.redact.sed"
LOG="$W/collector.txt"
MOUNTED_BY_US=""
mkdir -p "$W/system" "$W/gpu" "$W/display" "$W/driver" "$W/logs" "$W/opencore" || exit 1
: > "$SECRETS"
: > "$TMPBASE/.skipped"

cleanup() {
	for d in $MOUNTED_BY_US; do diskutil unmount "$d" >/dev/null 2>&1; done
	rm -rf "${TMPBASE:?}"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

say() { printf '%s\n' "$*" >&2; }
note() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
# step NAME: a section starts; "== step N/TOTAL: NAME" on stdout is what IntelXe Manager's progress bar reads (the nine
# sections every report has, plus the optional probe, ACPI and self-test ones)
STEPN=0
step() {
	STEPN=$((STEPN + 1))
	printf '== step %s/%s: %s\n' "$STEPN" $((9 + PROBE + ACPI + TESTS)) "$1"
	note "$1"
}
skip() { printf '%s\n' "$*" >> "$TMPBASE/.skipped"; note "skipped: $*"; }
left() { echo $((DEADLINE - $(date +%s))); }
have() { command -v "$1" >/dev/null 2>&1; }

# run_to SECONDS cmd args...: run with a time limit. Returns 124 on timeout.
# RUN_TO_FIRST=name: on timeout stop the child process with that name first (so a pipeline's
# reader still sees end-of-file and writes what it has).
TO_N=0
run_to() {
	_t=$1; shift
	_l=$(left)
	[ "$_l" -lt "$_t" ] && _t=$_l
	[ "$_t" -lt 2 ] && _t=2
	TO_N=$((TO_N + 1))
	_mark="$TMPBASE/.to$TO_N"
	"$@" &
	_p=$!
	(
		trap 'kill $_s 2>/dev/null; exit 0' TERM
		sleep "$_t" & _s=$!
		wait $_s
		: > "$_mark"
		if [ -n "$RUN_TO_FIRST" ]; then pkill -TERM -P $_p "$RUN_TO_FIRST"; sleep 2; fi
		pkill -TERM -P $_p; kill -TERM $_p
		sleep 2
		pkill -KILL -P $_p; kill -KILL $_p
	) </dev/null >/dev/null 2>&1 &
	_w=$!
	wait $_p
	_rc=$?
	# the watchdog and its sleep with KILL, which cannot be ignored (TERM can be, see the top)
	pkill -KILL -P $_w 2>/dev/null; kill -KILL $_w 2>/dev/null
	wait $_w 2>/dev/null
	if [ -e "$_mark" ]; then rm -f "$_mark"; _rc=124; fi
	return $_rc
}

# cmd FILE SECONDS cmd args...: append "$ cmd" and its output (stdout+stderr) to FILE
cmd() {
	_f=$1; _s=$2; shift 2
	printf '$ %s\n' "$*" >> "$_f"
	run_to "$_s" "$@" >> "$_f" 2>&1
	_r=$?
	[ $_r = 124 ] && { printf '[timed out after %ss]\n' "$_s" >> "$_f"; note "timeout: $*"; }
	printf '\n' >> "$_f"
	return $_r
}

# Literal values to remove from every file: kind<TAB>value
lit() {
	[ -n "$2" ] || return 0
	[ ${#2} -ge 4 ] || return 0
	case "$2" in *'
'*) return 0 ;; esac
	printf '%s\t%s\n' "$1" "$2" >> "$SECRETS"
}
hexof() { printf '%s' "$1" | od -An -tx1 | tr -d ' \n'; }
b64hex() { printf '%s' "$1" | { base64 -D 2>/dev/null || base64 -d 2>/dev/null; } | od -An -tx1 | tr -d ' \n'; }
upper() { printf '%s' "$1" | tr 'a-f' 'A-F'; }

ARCH=$(uname -m)
say "Intel GPU macOS driver: collecting diagnostics ($ID), about a minute..."
{
	echo "collector: collect-macos.sh v$COLLECTOR_VERSION"
	echo "report id: $ID"
	echo "started: $(date '+%Y-%m-%d %H:%M:%S %z')"
	echo "options: days=$DAYS tests=$TESTS mount=$MOUNT sudo-mount=$SUDOMOUNT probe=$PROBE acpi=$ACPI"
	echo "uid: $(id -u)"
	echo
} > "$LOG"

# ---------------------------------------------------------------- private values (never written)
PE=$(ioreg -r -d1 -c IOPlatformExpertDevice 2>/dev/null)
qv() { printf '%s\n' "$PE" | sed -n "s/.*\"$1\" = \"\\([^\"]*\\)\".*/\\1/p" | head -1; }
SERIAL=$(qv IOPlatformSerialNumber)
PUUID=$(qv IOPlatformUUID)
lit serial "$SERIAL"
[ -n "$SERIAL" ] && lit serial "$(hexof "$SERIAL")"
lit uuid "$PUUID"
[ -n "$PUUID" ] && lit uuid "$(printf '%s' "$PUUID" | tr 'A-F' 'a-f')"
OCG=4D1FDA02-38C7-4A6A-9CC6-4BCCA8B30102
MLB=$(nvram "$OCG:MLB" 2>/dev/null | cut -f2-)
lit mlb "$MLB"
MLB2=$(nvram MLB 2>/dev/null | cut -f2-)
lit mlb "$MLB2"
for k in "$OCG:ROM" ROM; do
	r=$(nvram -x "$k" 2>/dev/null | tr -d '\n\t ' | sed -n 's/.*<data>\([^<]*\)<\/data>.*/\1/p')
	[ -n "$r" ] || continue
	lit rom "$r"
	h=$(b64hex "$r"); lit rom "$h"; lit rom "$(upper "$h")"
done
for m in $(ifconfig -a 2>/dev/null | awk '/[[:space:]]ether /{print $2}' | sort -u); do
	lit mac "$m"; lit mac "$(upper "$m")"
	h=$(printf '%s' "$m" | tr -d ':')
	case "$h" in *[a-f]*) lit mac "$h"; lit mac "$(upper "$h")" ;; esac   # all-digit forms would hit plain numbers
done
USERNAME=$(id -un 2>/dev/null)
# XE_REPORT_USER: the user whose report this is when it runs as root without sudo (IntelXe Manager's administrator mode)
for u in "$USERNAME" "$SUDO_USER" "$(basename "$HOME")" "$XE_REPORT_USER"; do
	case "$u" in ''|root|admin|user|guest|test|Shared|staff) ;; *) lit user "$u" ;; esac
done
FULLNAME=$(id -F 2>/dev/null)
lit user "$FULLNAME"
[ -n "$XE_REPORT_USER" ] && lit user "$(id -F "$XE_REPORT_USER" 2>/dev/null)"
for h in "$(scutil --get ComputerName 2>/dev/null)" "$(scutil --get LocalHostName 2>/dev/null)" \
	"$(scutil --get HostName 2>/dev/null)" "$(hostname 2>/dev/null)" "$(hostname -s 2>/dev/null)"; do
	case "$(sysctl -n hw.model 2>/dev/null)" in *"$h"*) ;; *) lit host "$h" ;; esac   # "MacBookPro" as a host name is the model
done
WIFI_IF=$(networksetup -listallhardwareports 2>/dev/null | awk '/^Hardware Port: (Wi-Fi|AirPort)/{f=1;next} f&&/^Device:/{print $2;exit}')
if [ -n "$WIFI_IF" ]; then
	run_to 6 networksetup -listpreferredwirelessnetworks "$WIFI_IF" 2>/dev/null | sed -n '2,$s/^[[:space:]]*//p' > "$TMPBASE/.ssids"
	run_to 4 networksetup -getairportnetwork "$WIFI_IF" 2>/dev/null | sed -n 's/^Current Wi-Fi Network: //p' >> "$TMPBASE/.ssids"
	while IFS= read -r s; do [ ${#s} -ge 3 ] && printf 'ssid\t%s\n' "$s" >> "$SECRETS"; done < "$TMPBASE/.ssids"
	rm -f "$TMPBASE/.ssids"
fi

# ---------------------------------------------------------------- system
step "system"
F="$W/system/os.txt"
cmd "$F" 5 sw_vers
cmd "$F" 5 uname -mrv
cmd "$F" 5 sysctl kern.osversion kern.boottime kern.bootargs hw.model hw.ncpu hw.memsize machdep.cpu.brand_string machdep.cpu.family machdep.cpu.model machdep.cpu.stepping hw.optional.arm64
cmd "$F" 5 uptime
cmd "$F" 5 csrutil status
BOOTTIME=$(sysctl -n kern.boottime 2>/dev/null | sed -n 's/^{ sec = \([0-9]*\),.*/\1/p')
printf '%s\n' "$PE" | grep -E '"(model|board-id|manufacturer|product-name|target-type|compatible)" =' > "$W/system/platform.txt"
F="$W/system/nvram.txt"
for k in "$OCG:opencore-version" "$OCG:oem-vendor" "$OCG:oem-product" "$OCG:oem-board" boot-args csr-active-config \
	"$OCG:boot-path" bootercfg; do
	v=$(nvram "$k" 2>/dev/null) && printf '%s\n' "$v" >> "$F"
done
# The unified-log queries are the slow part (they scan the whole log store). Start them now, in the background, so
# they get most of the time budget while everything else is collected. On a long uptime WindowServer's errors come
# from the last 2 hours only; the kernel lines always from the start of this boot (that is where bring-up is).
ulog() { # since(boot|date) predicate head tail headfile tailfile
	if [ "$1" = boot ]; then _r1=--last; _r2=boot; else _r1=--start; _r2=$1; fi
	shift
	/usr/bin/log show $_r1 "$_r2" --style compact --predicate "$1" 2>&1 |
		awk -v H="$2" -v T="$3" -v hf="$4" -v tf="$5" '
		NR <= H { print > hf; n = NR; next }
		{ r[NR % T] = $0; n = NR }
		END { s = n - T + 1; if (s <= H) s = H + 1; for (i = s; i <= n; i++) print r[i % T] > tf; print (n > 0 ? n - 1 : 0) > (hf ".count") }'
}
KPRED='processID == 0 AND (senderImagePath CONTAINS[c] "IntelXe" OR senderImagePath CONTAINS[c] "NMXA" OR senderImagePath CONTAINS[c] "Lilu" OR senderImagePath CONTAINS[c] "WhateverGreen" OR senderImagePath CONTAINS[c] "WhateverXe" OR senderImagePath CONTAINS[c] "IOGraphics" OR senderImagePath CONTAINS[c] "XeProbe")'
WPRED='(process == "WindowServer" AND messageType >= 16) OR senderImagePath CONTAINS[c] "NVMTLDriver" OR senderImagePath CONTAINS[c] "libvulkan_intel" OR senderImagePath CONTAINS[c] "libintelxe"'
WSINCE=boot
NOW=$(date +%s)
[ -n "$BOOTTIME" ] && [ $((NOW - BOOTTIME)) -gt 7200 ] && WSINCE=$(date -r $((NOW - 7200)) '+%Y-%m-%d %H:%M:%S')
LOGT=$(($(left) - 12))
[ $LOGT -gt 45 ] && LOGT=45
( TO_N=1000; RUN_TO_FIRST=log; run_to $LOGT ulog boot "$KPRED" 2000 3000 "$W/logs/kernel-ours-boot.txt" "$W/logs/kernel-ours-latest.txt"
  echo $? > "$TMPBASE/.kernel-ours.rc" ) &
KLOG_PID=$!
( TO_N=2000; RUN_TO_FIRST=log; run_to $LOGT ulog "$WSINCE" "$WPRED" 500 2000 "$W/logs/windowserver-errors-boot.txt" "$W/logs/windowserver-errors-latest.txt"
  echo $? > "$TMPBASE/.windowserver-errors.rc" ) &
WLOG_PID=$!

cmd "$W/system/pmset.txt" 6 pmset -g
pmlog() { pmset -g log 2>/dev/null | grep -E '[[:space:]](Sleep|Wake|DarkWake|Failure|Assertions)[[:space:]]' | tail -n 80; }
RUN_TO_FIRST=pmset run_to 10 pmlog >> "$W/system/pmset.txt" 2>&1
RUN_TO_FIRST=
ls -la /Library/LaunchDaemons /Library/LaunchAgents 2>/dev/null | grep -iE 'intelxe|nullmoth|nmx|xe|intel|hackintosh' > "$W/system/launchd.txt"

# ---------------------------------------------------------------- GPUs (PCI)
step "pci"
IOPCI="$TMPBASE/.pci"
run_to 10 ioreg -r -d1 -l -w0 -c IOPCIDevice > "$IOPCI" 2>/dev/null
# LE ioreg data <86800000> -> 8086 ; class <00000300> -> 030000. pcidebug is "bus:dev:fn" plus "(secondary:subordinate)"
# on bridges, which gives each GPU's path up to its root port. A discrete Arc card sits behind its own switch whose
# downstream port and the GPU function report a nominal Gen1 x1: the real link is the one into the root port.
awk -v SUM="$W/gpu/pci-all.txt" -v DISP="$W/gpu/pci-display.txt" -v GPUS="$TMPBASE/.gpus" -v LINKS="$W/gpu/pcie-links.txt" '
function le16(s) { s = substr(s, 2); return substr(s, 3, 2) substr(s, 1, 2) }
function le24(s) { s = substr(s, 2); return substr(s, 5, 2) substr(s, 3, 2) substr(s, 1, 2) }
function val(l) { sub(/^[^=]*= /, "", l); return l }
function link(v) { v = v + 0; return (v % 16 && int(v / 16) % 64) ? "Gen" (v % 16) " x" int(v / 16) % 64 : "" }
function hop(i,   s, c) { s = link(lst[i]); c = link(cap[i]); return loc[i] " " (s != "" ? s : "no link") (c != "" && c != s ? " (capable " c ")" : "") }
function flush(   bars, n, a, k, v) {
	if (name == "") return
	N++; nm[N] = name; loc[N] = l; bus[N] = l; sub(/:.*/, "", bus[N]); sec[N] = sc; lst[N] = lk; cap[N] = lc
	disp[N] = (substr(cls, 1, 2) == "03" || cls == "ffffff")
	bars = ""
	n = split(mem, a, /"length"=/)
	for (k = 2; k <= n; k++) { v = a[k] + 0; bars = bars (bars == "" ? "" : ", ") (v >= 1048576 ? int(v / 1048576) " MB" : int(v / 1024) " KB") }
	line[N] = sprintf("%-10s %s:%s sub %s:%s rev %s class %s %-24s %s%s", l, ven, dev, sven, sdev, rev, cls, name, (model != "" ? "\"" model "\" " : ""), (bars != "" ? "BARs " bars : ""))
	print line[N] (link(lk) != "" ? " link " link(lk) : "") > SUM
	if (disp[N]) printf "%s\n\n", block > DISP
	name = ""; block = ""
}
/^\+-o / { flush(); name = $2; ven = dev = sven = sdev = rev = cls = l = sc = model = mem = lk = lc = ""; block = $0; next }
{ block = block "\n" $0 }
/"vendor-id" = </ { ven = le16(val($0)) }
/"device-id" = </ { dev = le16(val($0)) }
/"subsystem-vendor-id" = </ { sven = le16(val($0)) }
/"subsystem-id" = </ { sdev = le16(val($0)) }
/"revision-id" = </ { rev = substr(val($0), 2, 2) }
/"class-code" = </ { cls = le24(val($0)) }
/"pcidebug" = / { l = val($0); gsub(/"/, "", l); sc = ""; if (l ~ /\(/) { sc = l; sub(/^[^(]*\(/, "", sc); sub(/:.*/, "", sc) } sub(/\(.*/, "", l) }
/"model" = </ { model = val($0); gsub(/^<"|">$/, "", model) }
/"IODeviceMemory" = / { mem = val($0) }
/"IOPCIExpressLinkStatus" = / { lk = val($0) }
/"IOPCIExpressLinkCapabilities" = / { lc = val($0) }
END {
	flush()
	for (i = 1; i <= N; i++) {
		if (!disp[i]) continue
		# walk up: the bridge whose secondary bus is the bus of this device, up to bus 0 (the root port)
		path = hop(i); b = bus[i]; top = i; root = 0; guard = 0
		while (b != "0" && guard++ < 16) {
			p = 0
			for (j = 1; j <= N; j++) if (sec[j] != "" && sec[j] == b) { p = j; break }
			if (!p) break
			path = hop(p) " > " path
			if (bus[p] == "0") { root = p; break }
			top = p; b = bus[p]
		}
		if (root) {
			# the link into the root port, as both ends see it
			card = link(lst[top]); rp = link(lst[root])
			summ = "PCIe " (card != "" ? card : rp) " at root port " loc[root] (link(cap[root]) != "" ? " (port capable " link(cap[root]) ")" : "") \
				(top != i ? ", through the card'"'"'s switch" : "")
		} else summ = (bus[i] == "0" ? "integrated (no PCIe link)" : "PCIe path not found")
		print line[i] "  " summ > GPUS
		print loc[i] " " nm[i] ": " summ "\n  path: " path > LINKS
	}
}
' "$IOPCI"
rm -f "$IOPCI"
cmd "$W/gpu/accelerators.txt" 8 ioreg -r -d1 -l -w0 -c IOAccelerator
for c in IntelXe NMXAccel NMXAGDC; do cmd "$W/gpu/our-services.txt" 5 ioreg -r -d1 -l -w0 -c "$c"; done

# ---------------------------------------------------------------- displays
step "display"
EDID_AWK='
function hx(s,   i, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
function byte(e, i) { return hx(substr(e, 2 * i + 1, 2)) }
function setb(e, i, v) { return substr(e, 1, 2 * i) sprintf("%02x", v) substr(e, 2 * i + 3) }
# zero the serial number field and the serial-number text descriptor, fix the checksum
function edid_clean(e,   i, d, j, sum) {
	if (length(e) < 256) return e
	for (i = 12; i <= 15; i++) e = setb(e, i, 0)
	for (d = 54; d <= 108; d += 18)
		if (byte(e, d) == 0 && byte(e, d + 1) == 0 && byte(e, d + 3) == 255) {
			e = setb(e, d + 5, 10); for (j = d + 6; j <= d + 17; j++) e = setb(e, j, 32)
		}
	sum = 0; for (i = 0; i < 127; i++) sum += byte(e, i)
	return setb(e, 127, (256 - sum % 256) % 256)
}
function edid_desc(e,   m, nm, d, j, c, pc, w, h, ht, vt, hz) {
	if (length(e) < 256) return "short EDID (" length(e) / 2 " bytes)"
	m = byte(e, 8) * 256 + byte(e, 9)
	nm = ""
	for (d = 54; d <= 108; d += 18)
		if (byte(e, d) == 0 && byte(e, d + 1) == 0 && byte(e, d + 3) == 252)
			for (j = d + 5; j <= d + 17; j++) { c = byte(e, j); if (c == 10) break; nm = nm sprintf("%c", c) }
	sub(/ +$/, "", nm)
	pc = (byte(e, 54) + byte(e, 55) * 256) * 10000
	w = byte(e, 56) + int(byte(e, 58) / 16) * 256; ht = w + byte(e, 57) + (byte(e, 58) % 16) * 256
	h = byte(e, 59) + int(byte(e, 61) / 16) * 256; vt = h + byte(e, 60) + (byte(e, 61) % 16) * 256
	hz = (ht * vt > 0) ? pc / (ht * vt) : 0
	return sprintf("%c%c%c %04x \"%s\" preferred %dx%d@%.2f, EDID %d.%d, %d extension block(s)", \
		int(m / 1024) % 32 + 64, int(m / 32) % 32 + 64, m % 32 + 64, byte(e, 11) * 256 + byte(e, 10), nm, w, h, hz, \
		byte(e, 18), byte(e, 19), byte(e, 126))
}'
# sanitize EDIDs inside a text file in place
edid_fix() {
	awk "$EDID_AWK"'
{ out = ""; s = $0
  while (match(s, /00ffffffffffff00[0-9a-f]+/)) { out = out substr(s, 1, RSTART - 1) edid_clean(substr(s, RSTART, RLENGTH)); s = substr(s, RSTART + RLENGTH) }
  print out s }' "$1" > "$1.tmp" && mv "$1.tmp" "$1"
}
if [ "$ARCH" = x86_64 ]; then
	cmd "$W/display/framebuffers.txt" 8 ioreg -r -d1 -l -w0 -c IOFramebuffer
	cmd "$W/display/displays.txt" 8 ioreg -r -d1 -l -w0 -c IODisplay
	edid_fix "$W/display/framebuffers.txt"
	edid_fix "$W/display/displays.txt"
	n=0
	grep -oE '"IODisplayEDID" = <[0-9a-f]+>' "$W/display/displays.txt" | sed 's/.*<\(.*\)>/\1/' | while read -r e; do
		n=$((n + 1))
		printf '%s\n' "$e" > "$W/display/edid-$n.hex"
		printf 'display %d: %s\n' "$n" "$(echo "$e" | awk "$EDID_AWK"'{ print edid_desc($0) }')" >> "$TMPBASE/.displays"
	done
else
	skip "framebuffer/EDID registry dump: Apple Silicon (the driver stack does not apply)"
fi
cmd "$W/gpu/system_profiler-displays.txt" 30 system_profiler -detailLevel mini SPDisplaysDataType

# ---------------------------------------------------------------- driver state
step "driver"
F="$W/driver/kexts-loaded.txt"
run_to 10 kmutil showloaded --list-only > "$TMPBASE/.kexts" 2>/dev/null
{
	echo "# ours, Lilu and its plugins"
	grep -iE 'intelxe|nullmoth|nmx|lilu|whatevergreen|whateverxe|hybridpm|cputopology|cpufriend|virtualsmc|smcprocessor' "$TMPBASE/.kexts"
	echo
	echo "# graphics kexts"
	grep -iE 'graphics|framebuffer|accelerat|gpu|iondrv|amdradeon|nvda|geforce|paravirt' "$TMPBASE/.kexts" | grep -viE 'intelxe|nmx'
} > "$F"
INTELXE_SYSCTLS="state stats sensors vram surfvram_stats display_irqs step irq video hugepages range_inval unbind_async rc6 gucrc
fw_hold slpc_min slpc_max slpc_strategy slpc_profile slpc_ignore_eff media_slpc ctx_policies rc_yield job_timeout_ms
surfvram surfvram_mb freq freq_manual gt_reset dumpexec"
INTELXEFB_SYSCTLS="intelxefb_dpms intelxefb_hwcursor intelxefb_vbl_precise intelxefb_vrr_lfc intelxefb_vrr_vbl intelxefb_vrr_tick_us
intelxefb_vrr_floor_hz intelxefb_vrr_pace_lo_pct intelxefb_vrr_pace_hi_pct intelxefb_vrrlog_on"
NMX_SYSCTLS="nmxaccel_headless nmxaccelfb nmxaccel_iop nmxaccel_async nmxaccel_flip nmxaccel_gamma nmxaccel_gamma_loads
nmxaccel_gamma_fail nmxaccel_color_unhandled nmxaccel_scanout_copies nmxaccel_copy_forwards nmxaccel_mark_wait_max_us
nmxaccel_iop_ok nmxaccel_iop_fail nmxaccel_iop_empty nmxaccel_iop_blank nmxaccel_iop_copy_us nmxaccel_flips
nmxaccel_flip_late nmxaccel_flip_refused nmxaccel_async_frames nmxaccel_async_drop nmxaccel_async_lost
nmxaccel_marks nmxaccel_mark_waits nmxaccel_mark_timeouts nmxaccel_vrr_tx nmxaccel_vrr_bad"
# root-only, read side has no effect (never: intelxefb_thumb/crop = screen contents, peek/poke/dpcd, execlog)
ROOT_SYSCTLS="debug.intelxe.g2h debug.intelxefb_mode debug.intelxefb_vrr debug.intelxefb_irq debug.intelxefb_vrr_modes
debug.intelxefb_link debug.intelxefb_vrrinfo debug.intelxefb_linklog"
sysget() { # file prefix names...
	_f=$1; _pre=$2; shift 2
	for _n in "$@"; do
		_v=$(run_to 4 sysctl -n "$_pre$_n" 2>/dev/null) || continue
		case "$_v" in *'
'*) printf '== %s\n%s\n\n' "$_pre$_n" "$_v" >> "$_f" ;;
		*) printf '%s = %s\n' "$_pre$_n" "$_v" >> "$_f" ;;
		esac
	done
}
INTELXE_PRESENT=0
sysctl -n debug.intelxe.state >/dev/null 2>&1 && INTELXE_PRESENT=1
if [ $INTELXE_PRESENT = 1 ]; then
	# debug.intelxe.dump (newer kernels): every read-only value in one call; otherwise the list above, one by one
	if run_to 5 sysctl -n debug.intelxe.dump > "$W/driver/intelxe-dump.txt" 2>/dev/null && [ -s "$W/driver/intelxe-dump.txt" ]; then
		note "debug.intelxe.dump: $(wc -c < "$W/driver/intelxe-dump.txt" | tr -d ' ') bytes"
	else
		rm -f "$W/driver/intelxe-dump.txt"
		sysget "$W/driver/sysctl-intelxe.txt" debug.intelxe. $INTELXE_SYSCTLS
	fi
	run_to 4 sysctl -b debug.intelxe.hwconfig > "$W/driver/intelxe-hwconfig.bin" 2>/dev/null || rm -f "$W/driver/intelxe-hwconfig.bin"
else
	skip "debug.intelxe.* sysctls: IntelXe.kext is not loaded"
fi
sysget "$W/driver/sysctl-intelxefb.txt" debug. $INTELXEFB_SYSCTLS
sysget "$W/driver/sysctl-nmxaccel.txt" debug. $NMX_SYSCTLS
if [ $INTELXE_PRESENT = 1 ]; then
	if [ "$(id -u)" = 0 ]; then SUDO=""; elif sudo -n true >/dev/null 2>&1; then SUDO="sudo -n"; else SUDO=none; fi
	if [ "$SUDO" = none ]; then
		skip "root-only driver state (debug.intelxe.g2h, debug.intelxefb_mode/vrr/irq/link/vrrinfo/linklog): sudo needs a password"
	else
		for n in $ROOT_SYSCTLS; do
			v=$(run_to 4 $SUDO sysctl -n "$n" 2>/dev/null) && printf '== %s\n%s\n\n' "$n" "$v" >> "$W/driver/sysctl-root.txt"
		done
	fi
fi

# installed components: version + sha256
F="$W/driver/installed.txt"
pget() { plutil -extract "$2" raw -o - "$1" 2>/dev/null || defaults read "${1%.plist}" "$2" 2>/dev/null; }
bundle_info() { # path-to-.kext-or-.bundle
	_b=$1
	[ -d "$_b" ] || return 0
	_ip="$_b/Contents/Info.plist"
	_ex=$(pget "$_ip" CFBundleExecutable)
	printf '%s\n  id %s  version %s (%s)\n' "$_b" "$(pget "$_ip" CFBundleIdentifier)" \
		"$(pget "$_ip" CFBundleShortVersionString)" "$(pget "$_ip" CFBundleVersion)" >> "$F"
	if [ -n "$_ex" ] && [ -f "$_b/Contents/MacOS/$_ex" ]; then
		printf '  %s  %s bytes  %s\n' "$(shasum -a 256 "$_b/Contents/MacOS/$_ex" | cut -d' ' -f1)" \
			"$(stat -f %z "$_b/Contents/MacOS/$_ex")" "$(stat -f %Sm "$_b/Contents/MacOS/$_ex")" >> "$F"
	fi
}
file_info() {
	[ -f "$1" ] || return 0
	printf '%s\n  %s  %s bytes  %s\n' "$1" "$(shasum -a 256 "$1" | cut -d' ' -f1)" "$(stat -f %z "$1")" "$(stat -f %Sm "$1")" >> "$F"
}
for k in IntelXe IntelXeFB NMXAccel NMXAGDC Lilu WhateverGreen WhateverXe; do
	bundle_info "/Library/Extensions/$k.kext"
done
# the Metal plugin: the system-volume copy (system-volume/ssv-install.sh), the /Library copy (install.sh) and the name
# installs before 0.1.2 used
for b in /System/Library/Extensions/NVMTLDriver.bundle /Library/GPUBundles/NVMTLDriverLib.bundle /Library/GPUBundles/NVMTLDriver.bundle; do
	bundle_info "$b"
	for f in "$b/Contents/MacOS/libnvmtl_translate.dylib" "$b/Contents/Frameworks/nvmtl/libvulkan_intel.dylib"; do file_info "$f"; done
done
for a in "/Applications/IntelXe Manager.app" "$HOME/Applications/IntelXe Manager.app"; do bundle_info "$a"; done
for f in /Library/GPUBundles/nvmtl/*.dylib /Library/GPUBundles/nvmtl/*.json /usr/local/lib/libvulkan*.dylib; do
	[ -L "$f" ] && { printf '%s -> %s\n' "$f" "$(readlink "$f")" >> "$F"; continue; }
	file_info "$f"
done
for f in /Library/IntelXe/*.sh; do file_info "$f"; done
ANV=/Library/GPUBundles/nvmtl/libvulkan_intel.dylib
if [ -f "$ANV" ]; then
	printf 'Mesa: %s\n' "$(grep -a -o -m1 -E 'Mesa [0-9]+\.[0-9]+\.[0-9]+[-a-z0-9.]*( \(git-[0-9a-f]+\))?' "$ANV" | head -1)" >> "$F"
fi
[ -f /Library/GPUBundles/nvmtl/intel_icd.json ] && cp /Library/GPUBundles/nvmtl/intel_icd.json "$W/driver/intel_icd.json"
[ -f /Library/GPUBundles/nvmtl-allow.txt ] && cp /Library/GPUBundles/nvmtl-allow.txt "$W/driver/nvmtl-allow.txt"
ls -la /Library/GPUBundles /Library/GPUBundles/nvmtl /Library/Extensions 2>/dev/null > "$W/driver/ls-gpubundles-extensions.txt"
[ -r /var/log/intelxefb-load.log ] && tail -n 200 /var/log/intelxefb-load.log > "$W/logs/intelxefb-load.log"
# IntelXe Manager's macOS update guard (app/Resources/osupdate-guard.sh): its preparation, last result and log; and
# game mode's helper state while it runs (app/Resources/gamemode-helper.sh)
OSG='/Library/Application Support/intel-gpu-macos-driver/osupdate'
if [ -d "$OSG" ]; then
	{ echo "== state"; cat "$OSG/state" 2>/dev/null; echo "== result"; cat "$OSG/result" 2>/dev/null; echo "== tries: $(cat "$OSG/tries" 2>/dev/null || echo 0)"
	  echo "== daemon: $([ -f /Library/LaunchDaemons/io.github.yusufklncc.intelxe.osupdate.plist ] && echo installed || echo absent)"; } > "$W/driver/osupdate-state.txt"
	[ -r "$OSG/osupdate.log" ] && tail -n 400 "$OSG/osupdate.log" > "$W/logs/osupdate.log"
fi
[ -r /var/run/intelxe-gamemode/state ] && cp /var/run/intelxe-gamemode/state "$W/driver/gamemode-state.txt"
# ---------------------------------------------------------------- stack state (collector v3)
# What decides how the stack behaves on this boot: which Metal plugin copy the accelerator names, the boot guard (an
# unhealthy boot keeps acceleration off on later boots until intelxe-accel-off is cleared), the HiDPI switch, the
# package and app versions, and the CPU power-management kexts (a CPU held at a low clock looks like a slow GPU).
step "stack"
IOREGL="$TMPBASE/.ioregl"
run_to 15 ioreg -lw0 > "$IOREGL" 2>/dev/null
F="$W/driver/stack.txt"
nvk() { if _v=$(nvram "$1" 2>/dev/null); then printf '%s = %s\n' "$1" "$(printf '%s' "$_v" | cut -f2-)"; else printf '%s: not set\n' "$1"; fi; }
{
	echo "== Metal plugin named by the accelerator (MetalPluginName; absent = withheld, WindowServer unaccelerated)"
	grep -E '"(MetalPluginName|MetalPluginClassName|MetalStatisticsName)" =' "$IOREGL" | sed 's/^[ |+-]*//' | sort | uniq -c
	echo
	echo "== plugin copies"
	for b in /System/Library/Extensions/NVMTLDriver.bundle /Library/GPUBundles/NVMTLDriverLib.bundle /Library/GPUBundles/NVMTLDriver.bundle; do
		if [ -f "$b/Contents/MacOS/NVMTLDriver" ]; then
			printf '%s: NVMTLDriver %s\n' "$b" "$(shasum -a 256 "$b/Contents/MacOS/NVMTLDriver" | cut -c1-16)"
		else
			printf '%s: absent\n' "$b"
		fi
	done
	printf 'NVMTLDriver.use-library (NMXAccel uses the /Library copy even with a system-volume copy): %s\n' \
		"$([ -e /Library/GPUBundles/NVMTLDriver.use-library ] && echo present || echo absent)"
	printf 'NVMTLDriver.no-low-latency (no GuC low-latency hint for apps: IntelXe Manager Fast GPU response off): %s\n' \
		"$([ -e /Library/GPUBundles/NVMTLDriver.no-low-latency ] && echo present || echo absent)"
	if [ -f /Library/GPUBundles/nvmtl/ssv-component.plist ]; then
		echo "system-volume copy record (nvmtl/ssv-component.plist):"
		plutil -p /Library/GPUBundles/nvmtl/ssv-component.plist 2>/dev/null | sed 's/^/  /'
	else
		echo "system-volume copy record: none (no system-volume copy installed by ssv-install.sh)"
	fi
	echo
	echo "== boot guard and driver NVRAM keys"
	for k in intelxe-accel-off intelxe-guard intelxe-guard-cleared intelxe-shutdown intelxefb-shutdown intelxe-fb-armed \
		intelxefb-hidpi intelxefb-vrr-modes intelxe-rename; do nvk "$k"; done
	grep -E '"IntelXeFBGuard(Prev)?" =' "$IOREGL" | sed 's/^[ |+-]*//'
	echo
	echo "== HiDPI switch"
	printf 'IntelXeFB knows intelxefb-hidpi: %s\n' "$(grep -qa intelxefb-hidpi /Library/Extensions/IntelXeFB.kext/Contents/MacOS/IntelXeFB 2>/dev/null && echo yes || echo no)"
	printf 'framebuffers offering scaled modes (IOFBScalerInfo): %s\n' "$(grep -c '"IOFBScalerInfo" =' "$IOREGL")"
	printf 'WindowServer DisplayResolutionEnabled: %s\n' "$(defaults read /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled 2>/dev/null || echo 'not set')"
	echo "display overrides (/Library/Displays/Contents/Resources/Overrides):"
	find /Library/Displays/Contents/Resources/Overrides -type f 2>/dev/null | head -n 20 | while read -r o; do
		printf '  %s%s\n' "${o#/Library/Displays/Contents/Resources/Overrides/}" \
			"$(/usr/libexec/PlistBuddy -c 'Print :IntelXeHiDPI' "$o" >/dev/null 2>&1 && echo ' (written by hidpi.sh)')"
	done
	echo
	echo "== package and app"
	printf 'installed package version: %s\n' "$(cat '/Library/Application Support/intel-gpu-macos-driver/installed-version' 2>/dev/null || echo 'none recorded')"
	ls -1 '/Library/Application Support/intel-gpu-macos-driver' 2>/dev/null | grep '^backup-' | tail -n 5 | sed 's/^/  install backup: /'
	for a in "/Applications/IntelXe Manager.app" "$HOME/Applications/IntelXe Manager.app"; do
		[ -f "$a/Contents/Info.plist" ] && printf 'IntelXe Manager %s: %s\n' "$(pget "$a/Contents/Info.plist" CFBundleShortVersionString)" "$a"
	done
	printf 'Zink GL (/Library/IntelXe/GL): %s\n' "$([ -d /Library/IntelXe/GL ] && echo installed || echo 'not installed')"
	[ -f /Library/LaunchAgents/io.github.yusufklncc.intelxe.zink-gl.plist ] && echo "Zink GL for every LWJGL 3 application: on (LaunchAgent)"
	echo
	echo "== CPU power management"
	sysctl hw.physicalcpu hw.logicalcpu hw.cpufrequency hw.cpufrequency_max machdep.cpu.core_count machdep.cpu.thread_count 2>/dev/null
	grep -iE 'hybridpm|cputopology|cpufriend' "$TMPBASE/.kexts" 2>/dev/null | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /\./ && $i !~ /^0x/) { print "loaded: " $i; break } }'
	sysctl debug.hybridpm.enabled debug.hybridpm.route debug.hybridpm.why 2>/dev/null
	case " $(nvram boot-args 2>/dev/null | cut -f2-) " in *ctrsmt*) echo "boot-args set ctrsmt (CpuTopologyRebuild)" ;; esac
} > "$F" 2>&1
# The CPU's clock right now, when root can ask powermetrics (it reads the package's counters for one second).
if [ "$(id -u)" = 0 ] || sudo -n true >/dev/null 2>&1; then
	_su=""; [ "$(id -u)" = 0 ] || _su="sudo -n"
	cmd "$W/driver/cpu-clock.txt" 8 $_su powermetrics -n 1 -i 1000 --samplers cpu_power
else
	skip "the CPU clock (powermetrics): sudo needs a password"
fi
# Frames the display took in two seconds while this report ran (WindowServer redrawing at full rate with nothing
# moving, as the login window did in 0.1.3, takes GPU time from everything else).
f0=$(sysctl -n debug.nmxaccel_async_frames 2>/dev/null)
if [ -n "$f0" ]; then
	sleep 2; f1=$(sysctl -n debug.nmxaccel_async_frames 2>/dev/null)
	FPS_IDLE=$(( (f1 - f0) / 2 ))
	echo "frames presented per second during the report: $FPS_IDLE (the report's own Terminal output counts)" >> "$F"
fi
# The Metal plugin's own log (non-release builds write one; release builds write failures to the system log, which
# the WindowServer query above takes). WindowServer's is /tmp/nvmtl.log, an application's is in its TMPDIR.
mkdir -p "$W/logs/plugin"
for lf in /tmp/nvmtl.log /private/tmp/nvmtl.log "${TMPDIR:-/nonexistent}/nvmtl.log"; do
	[ -f "$lf" ] && [ -r "$lf" ] || continue
	n=$(printf '%s' "$lf" | sed 's#^/private##; s#[/ ]#_#g; s#^_##')
	[ -f "$W/logs/plugin/$n" ] && continue
	{ echo "# $lf, $(wc -l < "$lf" | tr -d ' ') lines, the last 3000"; tail -n 3000 "$lf"; } > "$W/logs/plugin/$n"
done
rmdir "$W/logs/plugin" 2>/dev/null

if have vulkaninfo; then
	cmd "$W/driver/vulkaninfo-summary.txt" 20 vulkaninfo --summary
else
	skip "vulkaninfo --summary: vulkaninfo is not installed"
fi

# ---------------------------------------------------------------- logs
step "logs"
# the two log queries were started in the background at the beginning; wait for them
[ -n "$KLOG_PID" ] && wait "$KLOG_PID"
[ -n "$WLOG_PID" ] && wait "$WLOG_PID"
for q in kernel-ours windowserver-errors; do
	rc=$(cat "$TMPBASE/.$q.rc" 2>/dev/null)
	case "$q" in kernel-ours) since=boot ;; *) since=$WSINCE ;; esac
	c="$W/logs/$q-boot.txt.count"
	[ -f "$c" ] && { printf '%s log lines since %s: %s%s\n' "$q" "$since" "$(cat "$c")" "$([ "$rc" = 124 ] && echo " (log show stopped at its time limit: $q-latest.txt ends there)")" >> "$W/logs/counts.txt"; rm -f "$c"; }
	[ "$rc" = 124 ] && skip "part of the $q log: log show reached its time limit"
done

# panic reports: names of all, the panic string of those from the last $DAYS days
DR=/Library/Logs/DiagnosticReports
UDR="$HOME/Library/Logs/DiagnosticReports"
[ -n "$XE_REPORT_USER" ] && [ -d "/Users/$XE_REPORT_USER" ] && UDR="/Users/$XE_REPORT_USER/Library/Logs/DiagnosticReports"   # root, for that user
mkdir -p "$W/logs/panics"
PANIC_AWK='
{ buf = buf $0 "\n" }
END {
	i = index(buf, "\"macOSPanicString\""); if (!i) i = index(buf, "\"panicString\"")
	if (!i) { printf "%s", substr(buf, 1, 65536); exit }
	s = substr(buf, i); s = substr(s, index(s, ":") + 1); s = substr(s, index(s, "\"") + 1)
	out = ""
	while (length(out) < 65536 && match(s, /[\\"]/)) {
		out = out substr(s, 1, RSTART - 1); c = substr(s, RSTART, 1)
		if (c == "\"") break
		d = substr(s, RSTART + 1, 1); s = substr(s, RSTART + 2)
		if (d == "n") out = out "\n"; else if (d == "t") out = out "\t"; else if (d == "u") { out = out "?"; s = substr(s, 5) } else out = out d
	}
	print out
}'
ls -lT "$DR" "$DR/Retired" 2>/dev/null | grep -E '\.panic$' > "$W/logs/panics-all.txt"
find "$DR" "$DR/Retired" -maxdepth 1 -type f -name '*.panic' ! -name '.*' -mtime "-$DAYS" 2>/dev/null > "$TMPBASE/.panics"
NPANIC=$(grep -c . "$TMPBASE/.panics")
if [ "$NPANIC" -gt 0 ]; then
	ls -t $(cat "$TMPBASE/.panics") 2>/dev/null | head -n 15 | while read -r p; do
		awk "$PANIC_AWK" "$p" > "$W/logs/panics/$(basename "$p" .panic).txt" 2>/dev/null
	done
fi
[ -r "$DR" ] || skip "system diagnostic reports: $DR is not readable"

# crash / GPU restart / hang reports that involve the stack
mkdir -p "$W/logs/crashes"
OURS='NVMTLDriver|libvulkan_intel|nvmtl|libintelxe|xemac|IntelXe|NMXAccel|NMXAGDC|"procName" : "WindowServer"|"app_name":"WindowServer"|VTDecoderXPCService|VTEncoderXPCService'
find "$DR" "$UDR" -maxdepth 1 -type f \( -name '*.ips' -o -name '*.crash' -o -name '*.gpuRestart' -o -name '*.spin' -o -name '*.hang' \) \
	-mtime "-$DAYS" 2>/dev/null > "$TMPBASE/.crashes"
: > "$TMPBASE/.ours"
while read -r f; do
	case "$f" in */JetsamEvent*|*/ExcUserFault_*) continue ;; esac   # these list every process
	case "$f" in *.gpuRestart) echo "$f" >> "$TMPBASE/.ours"; continue ;; esac
	head -c 2000000 "$f" 2>/dev/null | grep -qE "$OURS" && echo "$f" >> "$TMPBASE/.ours"
done < "$TMPBASE/.crashes"
NCRASH=$(grep -c . "$TMPBASE/.ours")
NGPURESET=$(grep -c '\.gpuRestart$' "$TMPBASE/.ours")
if [ "$NCRASH" -gt 0 ]; then
	ls -t $(cat "$TMPBASE/.ours") 2>/dev/null | head -n 25 | while read -r f; do
		head -c 524288 "$f" > "$W/logs/crashes/$(basename "$f")" 2>/dev/null
	done
	ls -lT $(cat "$TMPBASE/.ours") 2>/dev/null > "$W/logs/crashes-list.txt"
fi

# ---------------------------------------------------------------- OpenCore
step "opencore"
cp "$W/system/nvram.txt" "$W/opencore/nvram.txt" 2>/dev/null
# OpenCore config.plist identifiers. MODE=values prints each value (one line, no spaces);
# MODE=redact prints the plist with each value replaced, element kept (values may span lines).
PLIST_AWK='
BEGIN { K = "^(SystemSerialNumber|MLB|SystemUUID|ROM|BoardSerialNumber|ChassisSerialNumber|ApECID)$" }
function tagof(s) { if (match(s, /<(string|data|integer)>/)) return substr(s, RSTART + 1, RLENGTH - 2); return "" }
function emit(   v) {
	if (MODE == "values") { v = val; gsub(/<[^>]*>/, "", v); gsub(/[[:space:]]/, "", v); if (v != "") print v; return }
	print pre (tag == "string" ? "<string>REDACTED</string>" : tag == "data" ? "<data></data>" : "<integer>0</integer>")
}
function out(l) { if (MODE == "redact") print l }
{
	line = $0
	if (st == 2) { val = val line; if (index(line, "</" tag ">")) { st = 0; emit() }; next }
	if (st == 1) {
		t = tagof(line)
		if (t == "") { if (line !~ /^[[:space:]]*$/) st = 0; out(line); next }
		pre = substr(line, 1, RSTART - 1); tag = t; val = substr(line, RSTART)
		if (index(val, "</" t ">")) { st = 0; emit() } else st = 2
		next
	}
	if (match(line, /<key>[^<]*<\/key>/) && substr(line, RSTART + 5, RLENGTH - 11) ~ K) {
		keypart = substr(line, 1, RSTART + RLENGTH - 1); rest = substr(line, RSTART + RLENGTH)
		t = tagof(rest)
		if (t == "") { out(line); st = 1; next }
		pre = keypart substr(rest, 1, RSTART - 1); tag = t; val = substr(rest, RSTART)
		if (index(val, "</" t ">")) { st = 0; emit() } else st = 2
		next
	}
	out(line)
}'
efi_collect() { # mountpoint disk-id n
	_mp=$1; _id=$2; _d="$W/opencore/efi-$3"
	mkdir -p "$_d"
	_parent=$(printf '%s' "$_id" | sed 's/s[0-9]*$//')
	{
		echo "partition: $_id  mounted at: $_mp"
		diskutil info "$_parent" 2>/dev/null | grep -E 'Device / Media Name|Protocol|Device Location|Solid State|Removable Media'
	} > "$_d/disk.txt"
	[ -n "$BOOTED_EFI" ] && [ "$_id" = "$BOOTED_EFI" ] &&
		echo "this is the EFI partition the machine booted from ($BOOTED_HOW, file ${BOOTED_FILE:-?})" >> "$_d/disk.txt"
	cp "$_mp/EFI/OC/config.plist" "$_d/config.plist"
	# values in it to remove everywhere (PlatformInfo and other identifiers)
	awk -v MODE=values "$PLIST_AWK" "$_d/config.plist" | while IFS= read -r v; do
		v=$(printf '%s' "$v" | tr -d ' \t')
		lit oc "$v"
		h=$(b64hex "$v")
		case "$v" in *=|*[!0-9A-Fa-f-]*) [ ${#h} -ge 8 ] && { lit oc "$h"; lit oc "$(upper "$h")"; } ;; esac
	done
	{
		echo "== OpenCore.efi"; for f in "$_mp/EFI/OC/OpenCore.efi" "$_mp/EFI/BOOT/BOOTx64.efi"; do
			[ -f "$f" ] && printf '%s  %s\n' "$(shasum -a 256 "$f" | cut -d' ' -f1)" "${f#$_mp/}"; done
		echo; echo "== Kexts (name, version, sha256 of the binary)"
		for k in "$_mp"/EFI/OC/Kexts/*.kext; do
			[ -d "$k" ] || continue
			_ex=$(pget "$k/Contents/Info.plist" CFBundleExecutable)
			_sha=-; [ -n "$_ex" ] && [ -f "$k/Contents/MacOS/$_ex" ] && _sha=$(shasum -a 256 "$k/Contents/MacOS/$_ex" | cut -d' ' -f1)
			printf '%s  %s  %s\n' "$(basename "$k")" "$(pget "$k/Contents/Info.plist" CFBundleVersion)" "$_sha"
		done
		echo; echo "== Drivers"; ls "$_mp/EFI/OC/Drivers" 2>/dev/null
		echo; echo "== ACPI (file names only; tables are never read)"; ls "$_mp/EFI/OC/ACPI" 2>/dev/null
		echo; echo "== Tools"; ls "$_mp/EFI/OC/Tools" 2>/dev/null
	} > "$_d/listing.txt" 2>&1
	_score=0; _why=""
	_oclog=$(ls -t "$_mp"/opencore-*.txt 2>/dev/null | head -1)
	if [ -n "$_oclog" ]; then
		head -c 1500000 "$_oclog" > "$_d/opencore-log.txt"
		printf 'newest OpenCore log: %s (%s)\n' "$(basename "$_oclog")" "$(stat -f %Sm "$_oclog")" >> "$_d/disk.txt"
		_lm=$(stat -f %m "$_oclog")
		[ -n "$BOOTTIME" ] && [ "$_lm" -le $((BOOTTIME + 120)) ] && [ "$_lm" -ge $((BOOTTIME - 900)) ] &&
			{ echo "this log was written during the current boot: this EFI is probably the one that booted" >> "$_d/disk.txt"
			  _score=$((_score + 3)); _why="$_why OpenCore log of this boot;"; }
	fi
	# how well this EFI matches the running system, for when the firmware does not say which one booted
	_ga=7C436110-AB2A-4BBB-A880-FE41995C9F82
	_cba=$(plutil -extract "NVRAM.Add.$_ga.boot-args" raw -o - "$_mp/EFI/OC/config.plist" 2>/dev/null)
	[ -n "$_cba" ] && [ "$_cba" = "$(nvram boot-args 2>/dev/null | cut -f2-)" ] && { _score=$((_score + 2)); _why="$_why same boot-args;"; }
	_ccsr=$(plutil -extract "NVRAM.Add.$_ga.csr-active-config" raw -o - "$_mp/EFI/OC/config.plist" 2>/dev/null)
	[ -n "$_ccsr" ] && [ "$_ccsr" = "$(nvram -x csr-active-config 2>/dev/null | tr -d '\n\t ' | sed -n 's/.*<data>\([^<]*\)<\/data>.*/\1/p')" ] &&
		{ _score=$((_score + 1)); _why="$_why same csr-active-config;"; }
	_ocv=$(nvram "$OCG:opencore-version" 2>/dev/null | cut -f2)
	[ -n "$_ocv" ] && grep -aqF -- "$_ocv" "$_mp/EFI/OC/OpenCore.efi" 2>/dev/null && { _score=$((_score + 1)); _why="$_why OpenCore.efi is version $_ocv;"; }
	echo "match with the running system: score $_score ($_why )" >> "$_d/disk.txt"
	printf '%s\t%s\t%s\n' "$_id" "$_score" "$_why" >> "$TMPBASE/.efiscore"
	printf '%s\n' "$_id" >> "$TMPBASE/.efis"
}
# Which EFI partition booted. The firmware's BootCurrent names the load option (Boot####) it started, and that
# option's device path holds the partition's GPT GUID and the file (\EFI\BOOT\BOOTx64.efi or \EFI\OC\OpenCore.efi).
# OpenCore's boot-path (ExposeSensitiveData bit 0) says the same in text. No root needed. Only the description, the
# partition GUID and the file path are kept: a raw load option can hold a MAC address (network boot entries).
GG=8BE4DF61-93CA-11D2-AA0D-00E098032B8C
nvhex() { nvram -x "$1" 2>/dev/null | tr -d '\n\t ' | sed -n 's/.*<data>\([^<]*\)<\/data>.*/\1/p' | base64 -D 2>/dev/null | od -An -v -tx1 | tr -d ' \n'; }
LOADOPT_AWK='
function hx(s,   i, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
function b(i) { return hx(substr(h, 2 * i + 1, 2)) }
function x(i) { return toupper(substr(h, 2 * i + 1, 2)) }
function guid(i) { return x(i+3) x(i+2) x(i+1) x(i) "-" x(i+5) x(i+4) "-" x(i+7) x(i+6) "-" x(i+8) x(i+9) "-" x(i+10) x(i+11) x(i+12) x(i+13) x(i+14) x(i+15) }
function u16(i, end,   s, c) { s = ""; for (; i + 1 < end; i += 2) { c = b(i) + b(i + 1) * 256; if (c == 0) break; s = s ((c >= 32 && c < 127) ? sprintf("%c", c) : "?") } return s }
{
	h = $0; n = length(h) / 2
	if (n < 8) exit
	fpl = b(4) + b(5) * 256
	i = 6; desc = ""
	while (i + 1 < n) { c = b(i) + b(i + 1) * 256; i += 2; if (c == 0) break; desc = desc ((c >= 32 && c < 127) ? sprintf("%c", c) : "?") }
	end = i + fpl; if (end > n) end = n
	part = ""; g = ""; path = ""
	while (i + 4 <= end) {
		t = b(i); st = b(i + 1); len = b(i + 2) + b(i + 3) * 256
		if (len < 4) break
		if (t == 4 && st == 1 && len >= 42) { part = b(i + 4) + b(i + 5) * 256; if (b(i + 41) == 2) g = guid(i + 24) }
		if (t == 4 && st == 4) path = path u16(i + 4, i + len)
		if (t == 127 && st == 255) break
		i += len
	}
	print desc "\t" part "\t" g "\t" path
}'
BOOTED_EFI=""; BOOTED_HOW=""; BOOTED_FILE=""; BGUID=""
{
	bc=$(nvhex "$GG:BootCurrent")
	if [ ${#bc} -ge 4 ]; then
		num=$(printf '%s' "$bc" | cut -c3-4)$(printf '%s' "$bc" | cut -c1-2)
		num=$(printf '%s' "$num" | tr 'a-f' 'A-F')
		echo "firmware BootCurrent: Boot$num"
		for gu in "$GG" "$OCG"; do
			lo=$(nvhex "$gu:Boot$num")
			[ -n "$lo" ] || continue
			printf '%s\n' "$lo" | awk "$LOADOPT_AWK" > "$TMPBASE/.bootopt"
			IFS='	' read -r bdesc bpart bguid bpath < "$TMPBASE/.bootopt"
			echo "load option Boot$num ($gu): \"$bdesc\" partition $bpart GUID ${bguid:-none} file ${bpath:-none}"
			[ -n "$bguid" ] && { BGUID=$bguid; BOOTED_FILE=$bpath; BOOTED_HOW="firmware Boot$num \"$bdesc\""; break; }
		done
	else
		echo "firmware BootCurrent: not readable"
	fi
	bp=$(nvram "$OCG:boot-path" 2>/dev/null | cut -f2-)
	if [ -n "$bp" ]; then
		echo "OpenCore boot-path: $(printf '%s' "$bp" | grep -oE '[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}' | head -1) $(printf '%s' "$bp" | grep -oE '\\[^)]*$')"
		if [ -z "$BGUID" ]; then
			BGUID=$(printf '%s' "$bp" | grep -oE '[0-9A-Fa-f]{8}-([0-9A-Fa-f]{4}-){3}[0-9A-Fa-f]{12}' | head -1 | tr 'a-f' 'A-F')
			BOOTED_FILE=$(printf '%s' "$bp" | grep -oE '\\[^)]*$'); BOOTED_HOW="OpenCore boot-path"
		fi
	else
		echo "OpenCore boot-path: not exposed (Misc > Security > ExposeSensitiveData bit 0)"
	fi
	if [ -n "$BGUID" ]; then
		for id in $(diskutil list 2>/dev/null | awk '/(EFI|Microsoft Basic Data|DOS_FAT|Windows_FAT)/ && $NF ~ /^disk[0-9]+s[0-9]+$/ { print $NF }'); do
			u=$(diskutil info "$id" 2>/dev/null | sed -n 's/^ *Disk \/ Partition UUID: *//p' | tr 'a-f' 'A-F')
			[ "$u" = "$BGUID" ] && { BOOTED_EFI=$id; break; }
		done
		echo "booted partition: ${BOOTED_EFI:-GUID $BGUID matches no partition here}"
		[ -n "$BOOTED_EFI" ] && diskutil info "$(printf '%s' "$BOOTED_EFI" | sed 's/s[0-9]*$//')" 2>/dev/null |
			grep -E 'Device / Media Name|Protocol|Device Location|Removable Media' | sed 's/^ */  /'
	fi
} > "$W/opencore/booted-efi.txt" 2>&1
rm -f "$TMPBASE/.bootopt"
: > "$TMPBASE/.efis"
: > "$TMPBASE/.efiscore"
NEFI=0
NOMOUNT=""
# efi_ro ID: mount an EFI partition read-only, print where. diskutil first; as root, mount_msdos directly when diskutil
# refuses: an EFI that OpenCore wrote at boot is left marked dirty, and diskutil mounts a dirty FAT read-write only
# (found on the B580 machine, 10 Nov: administrator mode collected no config.plist). The cleanup unmounts by ID.
efi_ro() {
	if run_to 10 diskutil mount readOnly "$1" >/dev/null 2>&1; then
		diskutil info "$1" 2>/dev/null | sed -n 's/^ *Mount Point: *//p'; return 0
	fi
	[ "$(id -u)" = 0 ] || return 1
	_d=$(mktemp -d "$TMPBASE/efi.XXXXXX") || return 1
	if run_to 10 mount_msdos -o rdonly "/dev/$1" "$_d" >/dev/null 2>&1; then echo "$_d"; return 0; fi
	rmdir "$_d" 2>/dev/null; return 1
}
mount | awk '$0 ~ /\((msdos|exfat)/ { id = $1; sub("/dev/", "", id); mp = $0; sub(/^[^ ]+ on /, "", mp); sub(/ \(.*$/, "", mp); print id "\t" mp }' > "$TMPBASE/.mounts"
while IFS='	' read -r id mp; do
	[ -f "$mp/EFI/OC/config.plist" ] || continue
	NEFI=$((NEFI + 1)); efi_collect "$mp" "$id" "$NEFI"
done < "$TMPBASE/.mounts"
if [ "$MOUNT" = 1 ] && [ "$ARCH" = x86_64 ]; then
	for id in $(diskutil list 2>/dev/null | awk '$2 == "EFI" { print $NF }'); do
		[ "$(left)" -gt 10 ] || break
		grep -q "^$id	" "$TMPBASE/.mounts" && continue
		mp=$(efi_ro "$id") || { NOMOUNT="$NOMOUNT $id"; continue; }
		MOUNTED_BY_US="$MOUNTED_BY_US $id"
		[ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ] && { NEFI=$((NEFI + 1)); efi_collect "$mp" "$id" "$NEFI"; }
		diskutil unmount "$id" >/dev/null 2>&1 && MOUNTED_BY_US=$(echo "$MOUNTED_BY_US" | sed "s/ $id//")
		case "$mp" in "$TMPBASE"/efi.*) rmdir "$mp" 2>/dev/null ;; esac
	done
fi
# --sudo-mount: mount with sudo (read-only, only if sudo needs no password) the booted EFI if it is known and was not
# read, or else every EFI partition a user could not mount, so they can be compared; each is unmounted again.
if [ "$SUDOMOUNT" = 1 ] && [ "$ARCH" = x86_64 ] && sudo -n true >/dev/null 2>&1; then
	if [ -n "$BOOTED_EFI" ]; then targets=$BOOTED_EFI; else targets=$NOMOUNT; fi
	for id in $targets; do
		grep -qx "$id" "$TMPBASE/.efis" && continue
		grep -q "^$id	" "$TMPBASE/.mounts" && continue
		run_to 10 sudo -n diskutil mount readOnly "$id" >/dev/null 2>&1 || continue
		MOUNTED_BY_US="$MOUNTED_BY_US $id"
		mp=$(diskutil info "$id" 2>/dev/null | sed -n 's/^ *Mount Point: *//p')
		[ -n "$mp" ] && [ -f "$mp/EFI/OC/config.plist" ] && { NEFI=$((NEFI + 1)); efi_collect "$mp" "$id" "$NEFI"; }
		sudo -n diskutil unmount "$id" >/dev/null 2>&1 && MOUNTED_BY_US=$(echo "$MOUNTED_BY_US" | sed "s/ $id//")
		NOMOUNT=$(echo "$NOMOUNT" | sed "s/ $id//")
	done
elif [ "$SUDOMOUNT" = 1 ]; then
	skip "--sudo-mount: sudo needs a password, so no EFI partition was mounted as root"
fi
# No firmware answer: take the collected EFI that matches the running system best (score >= 3: the OpenCore log of
# this boot, or the same boot-args plus one more match).
if [ -z "$BOOTED_EFI" ] && [ -s "$TMPBASE/.efiscore" ]; then
	best=$(sort -t '	' -k2,2nr "$TMPBASE/.efiscore" | head -1)
	bid=$(printf '%s' "$best" | cut -f1); bsc=$(printf '%s' "$best" | cut -f2); bwhy=$(printf '%s' "$best" | cut -f3)
	nbest=$(awk -F'\t' -v s="$bsc" '$2 == s' "$TMPBASE/.efiscore" | wc -l | tr -d ' ')
	# A copy of the same EFI on another disk matches the settings just as well, so settings alone decide only when
	# every EFI partition here was read; the OpenCore log of this boot decides on its own.
	if [ "$nbest" = 1 ] && { case "$bwhy" in *"log of this boot"*) true ;; *) [ "$bsc" -ge 3 ] && [ -z "$(echo $NOMOUNT)" ] ;; esac; }; then
		BOOTED_EFI=$bid; BOOTED_HOW="best match:$bwhy"
		{ echo "booted partition (by comparison, not from the firmware): $bid, score $bsc:$bwhy"
		  diskutil info "$(printf '%s' "$bid" | sed 's/s[0-9]*$//')" 2>/dev/null |
			grep -E 'Device / Media Name|Protocol|Device Location|Removable Media' | sed 's/^ */  /'; } >> "$W/opencore/booted-efi.txt"
		for d in "$W"/opencore/efi-*; do
			grep -q "^partition: $bid " "$d/disk.txt" 2>/dev/null && echo "this is most likely the EFI partition the machine booted from ($BOOTED_HOW)" >> "$d/disk.txt"
		done
	else
		echo "booted partition: not determined (best collected match $bid, score $bsc:$bwhy; $nbest with that score; not read:${NOMOUNT:- none})" >> "$W/opencore/booted-efi.txt"
	fi
fi
if [ -n "$BOOTED_EFI" ] && ! grep -qx "$BOOTED_EFI" "$TMPBASE/.efis"; then
	skip "the booted EFI partition ($BOOTED_EFI) was not read: mount it (or run with --sudo-mount if sudo needs no password)"
fi
if [ -n "$NOMOUNT" ] || { [ "$NEFI" = 0 ] && [ "$ARCH" = x86_64 ]; }; then
	skip "OpenCore config.plist from EFI partition(s)${NOMOUNT:- (none found)}: not mounted, and mounting needs root.
    To add it: mount the EFI partition you boot from (sudo diskutil mount diskXsY, or MountEFI / OpenCore
    Configurator), then run this script again. It reads only EFI/OC/config.plist and file names, never ACPI tables."
fi

# ---------------------------------------------------------------- staged probe (--probe)
# XeProbe (the probe personality in IntelXe.kext, docs/PROBE-MODE.md): the stage results of this boot from the registry
# and sysctl, the NVRAM breadcrumb the previous boot left (it survives a hang and the hard reset after it), and
# any panic the probe watchdog raised.
if [ "$PROBE" = 1 ]; then
	step "probe"
	mkdir -p "$W/probe"
	v=$(nvram xeprobe-crumb 2>/dev/null | cut -f2-)
	printf '%s\n' "${v:-no breadcrumb in NVRAM (the probe has not run with debug.xeprobe.nvram=1 or -xeprobecrumb)}" > "$W/probe/nvram-breadcrumb.txt"
	cmd "$W/probe/ioreg.txt" 5 ioreg -r -d1 -l -w0 -c XeProbe
	for k in results log; do
		run_to 5 sysctl -n "debug.xeprobe.$k" > "$W/probe/$k.txt" 2>/dev/null || rm -f "$W/probe/$k.txt"
	done
	grep -l 'XeProbe' "$W"/logs/panics/*.txt 2>/dev/null | sed "s#^$W/##" > "$W/probe/panics.txt"
	grep -q '<class XeProbe' "$W/probe/ioreg.txt" 2>/dev/null ||
		skip "XeProbe is not attached this boot (boot-arg xeprobe=0xDEVICE and a probe build of IntelXe.kext)"
fi

# ---------------------------------------------------------------- ACPI tables (--acpi)
# AppleACPIPlatformExpert keeps every table macOS loaded in its "ACPI Tables" property: the firmware's, as OpenCore
# patched them, plus the SSDTs OpenCore added. No root needed. Only DSDT and SSDT*; .bin files, which the text redaction
# leaves intact and the final privacy check still reads.
if [ "$ACPI" = 1 ]; then
	step "acpi"
	mkdir -p "$W/acpi"
	run_to 10 ioreg -lw0 -p IOService -r -n AppleACPIPlatformExpert -d1 2>/dev/null |
		awk -v D="$W/acpi" '/"ACPI Tables" = \{/ {
			s = $0; sub(/.*"ACPI Tables" = \{/, "", s)
			while (match(s, /"[A-Za-z0-9_-]+"=<[0-9a-fA-F]*>/)) {
				e = substr(s, RSTART, RLENGTH); s = substr(s, RSTART + RLENGTH)
				k = e; sub(/^"/, "", k); sub(/".*/, "", k)
				print k >> (D "/table-names.txt")
				if (k !~ /^(DSDT|SSDT)/) continue
				h = e; sub(/^[^<]*</, "", h); sub(/>$/, "", h)
				print h > (D "/" k ".hex"); close(D "/" k ".hex")
			}
			exit }'
	for h in "$W"/acpi/*.hex; do
		[ -f "$h" ] || continue
		xxd -r -p "$h" > "${h%.hex}.bin" 2>/dev/null && rm -f "$h"
	done
	grep -vE '^(MSDM|SLIC)$' "$W/acpi/table-names.txt" > "$W/acpi/.n" 2>/dev/null; mv "$W/acpi/.n" "$W/acpi/table-names.txt" 2>/dev/null
	if ! ls "$W"/acpi/*.bin >/dev/null 2>&1; then
		# macOS 26 no longer publishes the "ACPI Tables" property (seen on 26.7.1, as root too)
		case "$(sw_vers -productVersion 2>/dev/null)" in
		1[0-5].*|"") skip "--acpi: no ACPI tables in the I/O Registry (AppleACPIPlatformExpert)" ;;
		*) skip "--acpi: macOS 26 and later no longer publish the ACPI tables in the I/O Registry; dump them from Windows or Linux (SSDTTime) or with OpenCore's Misc > Debug > SysReport" ;;
		esac
	fi
fi

# ---------------------------------------------------------------- optional self-tests
if [ "$TESTS" = 1 ]; then
	step "tests"
	mkdir -p "$W/tests"
	SD=$(cd "$(dirname "$0")" && pwd)
	findbin() { for c in "$SD/$1" "$HOME/b580/$1" "$(command -v "$1" 2>/dev/null)"; do [ -n "$c" ] && [ -x "$c" ] && [ -f "$c" ] && { echo "$c"; return 0; }; done; return 1; }
	if VK=$(findbin vk_smoke) && [ -f "$ANV" ]; then
		cmd "$W/tests/vk_smoke.txt" 30 "$VK" "$ANV"
	else
		skip "self-test vk_smoke: not found (looked next to this script, in ~/b580 and in PATH)"
	fi
	if MT=$(findbin mtl_tests); then
		NVMTL_ENABLE=1 cmd "$W/tests/mtl_tests.txt" 45 "$MT" vadd reduce math texture blit render
	else
		skip "self-test mtl_tests: not found (looked next to this script, in ~/b580 and in PATH)"
	fi
fi

# ---------------------------------------------------------------- warnings (collector v3)
# Known ways the stack goes wrong, each checked from what was collected; the summary lists the ones that apply first.
WARN="$TMPBASE/.warn"; : > "$WARN"
warn() { _c=$1; shift; printf '%s\t%s\n' "$_c" "$*" >> "$WARN"; }   # code (for IntelXe Manager), text
_aoff=$(nvram intelxe-accel-off 2>/dev/null | cut -f2-)
[ -n "$_aoff" ] && [ "$_aoff" != ok ] &&
	warn accel-off "Acceleration is held off by the boot guard (NVRAM intelxe-accel-off = $_aoff): earlier boots did not reach a healthy accelerated desktop. Once the cause is fixed: sudo nvram intelxe-accel-off=ok, then restart."
_guard=$(nvram intelxe-guard 2>/dev/null | cut -f2-)
case "$_guard" in ''|ok) ;; *) warn guard-armed "The boot guard is armed (intelxe-guard = $_guard): this boot has not yet run its desktop for a minute, or an earlier boot ended early. Normal right after login; after several minutes it means the desktop did not come up healthy." ;; esac
if grep -qi nmxaccel "$TMPBASE/.kexts" 2>/dev/null && ! grep -q '"MetalPluginName" =' "$IOREGL" 2>/dev/null; then
	warn no-metal "NMXAccel is loaded but names no Metal plugin (MetalPluginName withheld): WindowServer runs unaccelerated. The NMXAccel lines in logs/kernel-ours-*.txt say why (no-Metal fallback)."
fi
_ssv=/System/Library/Extensions/NVMTLDriver.bundle
if [ -d "$_ssv" ] && [ ! -e /Library/GPUBundles/NVMTLDriver.use-library ]; then
	_a=$(shasum -a 256 "$_ssv/Contents/Frameworks/nvmtl/libvulkan_intel.dylib" 2>/dev/null | cut -c1-16)
	_b=$(shasum -a 256 /Library/GPUBundles/nvmtl/libvulkan_intel.dylib 2>/dev/null | cut -c1-16)
	[ -n "$_a" ] && [ -n "$_b" ] && [ "$_a" != "$_b" ] &&
		warn ssv-mesa "The system-volume copy of the Metal plugin carries another Mesa ($_a) than /Library ($_b), and WindowServer uses the system-volume copy: install the package's system-volume copy too (sudo ./system-volume/ssv-install.sh --apply) and restart."
	_rb=$(defaults read /Library/GPUBundles/nvmtl/ssv-component build 2>/dev/null)
	[ -n "$_rb" ] && [ "$_rb" != "$(sw_vers -buildVersion)" ] &&
		warn ssv-build "The system-volume copy was installed on macOS build $_rb; this is $(sw_vers -buildVersion). A macOS update can replace the system volume: check it with ./system-volume/ssv-install.sh --check."
fi
if [ -f /Library/GPUBundles/nvmtl/ssv-component.plist ] && [ ! -d "$_ssv" ]; then
	warn ssv-gone "ssv-component.plist records a system-volume copy, but $_ssv is gone (a macOS update replaces the system volume): reinstall it with ./system-volume/ssv-install.sh --apply, or use the /Library copy (sudo touch /Library/GPUBundles/NVMTLDriver.use-library) and restart."
fi
_hn=$(nvram intelxefb-hidpi 2>/dev/null | cut -f2-); [ "$_hn" = 1 ] || _hn=0
_ho=$(find /Library/Displays/Contents/Resources/Overrides -type f 2>/dev/null | while read -r o; do /usr/libexec/PlistBuddy -c 'Print :IntelXeHiDPI' "$o" >/dev/null 2>&1 && echo x; done | grep -c x)
_hw=$(defaults read /Library/Preferences/com.apple.windowserver DisplayResolutionEnabled 2>/dev/null || echo 0)
if [ "$_hn" = 1 ] && { [ "$_ho" = 0 ] || [ "$_hw" != 1 ]; }; then
	warn hidpi-half "The HiDPI switch is half on (NVRAM intelxefb-hidpi=1, display override from hidpi.sh: $_ho, DisplayResolutionEnabled: $_hw): run sudo ./hidpi.sh --on again (or the app's Display page) and restart."
elif [ "$_hn" = 0 ] && [ "$_ho" -gt 0 ]; then
	warn hidpi-stale "A HiDPI display override from hidpi.sh is present but NVRAM intelxefb-hidpi is not 1: the scaled sizes are listed without the framebuffer's scaler. Run sudo ./hidpi.sh --off or --on, then restart."
fi
_iv=$(cat '/Library/Application Support/intel-gpu-macos-driver/installed-version' 2>/dev/null)
for k in IntelXeFB NMXAccel NMXAGDC; do
	_lv=$(grep -iE "$k" "$TMPBASE/.kexts" 2>/dev/null | head -1 | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\(/) { v = $i; gsub(/[()]/, "", v); print v; exit } }')
	_fv=$(pget "/Library/Extensions/$k.kext/Contents/Info.plist" CFBundleShortVersionString)
	[ -n "$_lv" ] && [ -n "$_fv" ] && [ "$_lv" != "$_fv" ] &&
		warn kext-version "$k.kext $_lv is loaded but the installed file is $_fv: restart (if it stays, the Auxiliary Kernel Collection was not rebuilt: sudo ./install.sh --apply again)."
	[ -z "$_lv" ] && [ -n "$_fv" ] && warn kext-not-loaded "$k.kext is installed ($_fv) but not loaded: see logs/kernel-ours-*.txt and kmutil, or approve it in System Settings > Privacy & Security."
done
# 12th-14th gen Core with E-cores (HybridPM leaves Core Ultra alone; the listed desktop models have P-cores only)
case "$(sysctl -n machdep.cpu.brand_string 2>/dev/null)" in
*Ultra*|*i3-1[234]100*|*i3-12300*|*i5-12400*|*i5-12490*|*i5-12500|*i5-12500[!H]*|*i5-12600|*i5-12600[!KH]*) ;;
*i[3579]-1[2-4][0-9][0-9][0-9]*|*i[3579]-1[2-4][0-9][0-9][UP]*)
	grep -qi hybridpm "$TMPBASE/.kexts" 2>/dev/null ||
		warn cpu-hybridpm "This Intel CPU has performance and efficiency cores, and HybridPM.kext is not loaded: XCPM can hold every core at a low clock, which shows as a slow GPU (Geekbench GPU scores about a third lower on the test machine). See \"Hybrid Intel CPUs\" in the install guide." ;;
esac
[ -n "$FPS_IDLE" ] && [ "$FPS_IDLE" -gt 100 ] &&
	warn redraw "The display presented $FPS_IDLE frames a second while this report ran. If nothing on screen was moving, something redraws at full rate and takes GPU time from applications."
[ "$NPANIC" -gt 0 ] && warn panics "$NPANIC kernel panic(s) in the last $DAYS days: their text is in logs/panics/."

# ---------------------------------------------------------------- summary
step "summary"
S="$W/summary.txt"
kx() { grep -iE "$1" "$TMPBASE/.kexts" 2>/dev/null | head -1 | awk '{ for (i = 1; i <= NF; i++) if ($i ~ /^\(/) { v = $i; gsub(/[()]/, "", v); print "loaded " v; exit } }' | grep . || echo "not loaded"; }
inst() { grep -A2 "^$1\$" "$W/driver/installed.txt" 2>/dev/null | grep -E '^  [0-9a-f]{64} ' | head -1 | sed 's/^ *//' | cut -c1-16 | sed 's/^/sha256 /' | grep . || echo "not installed"; }
{
	echo "Intel GPU macOS driver: diagnostics report $ID"
	echo "Created $(date '+%Y-%m-%d %H:%M %z') by collect-macos.sh v$COLLECTOR_VERSION"
	echo
	if [ -s "$WARN" ]; then
		echo "Warnings (read first)"
		cut -f2- "$WARN" | awk '{ print "  ! " $0 }' | fold -s -w 118 | sed 's/^\([^ ]\)/      \1/'
		echo
	fi
	echo "macOS        $(sw_vers -productVersion 2>/dev/null) ($(sw_vers -buildVersion 2>/dev/null)) $ARCH"
	echo "Booted       $(date -r "${BOOTTIME:-0}" '+%Y-%m-%d %H:%M' 2>/dev/null)"
	echo "Model        $(sysctl -n hw.model 2>/dev/null)   $(sed -n 's/.*"board-id" = <"\(.*\)">.*/board-id \1/p' "$W/system/platform.txt" | head -1)"
	o=$(grep -E 'oem-(vendor|product|board)' "$W/system/nvram.txt" 2>/dev/null | cut -f2 | tr '\n' ' ')
	[ -n "$o" ] && echo "Board        $o"
	echo "CPU          $(sysctl -n machdep.cpu.brand_string 2>/dev/null), $(sysctl -n hw.ncpu) threads, $(($(sysctl -n hw.memsize) / 1073741824)) GB RAM"
	echo "SIP          $(csrutil status 2>/dev/null | head -n 1 | sed 's/System Integrity Protection status: //')  csr-active-config $(grep csr-active-config "$W/system/nvram.txt" 2>/dev/null | cut -f2)"
	echo "boot-args    $(nvram boot-args 2>/dev/null | cut -f2-)"
	echo "OpenCore     $(grep opencore-version "$W/system/nvram.txt" 2>/dev/null | cut -f2)  config.plist: $( [ "$NEFI" -gt 0 ] && echo "collected from $NEFI EFI partition(s): $(tr '\n' ' ' < "$TMPBASE/.efis")" || echo "not collected")"
	if [ -n "$BOOTED_EFI" ]; then
		echo "Booted from  $BOOTED_EFI ($(sed -n 's/^ *Device \/ Media Name: *//p' "$W/opencore/booted-efi.txt" | head -1), $(sed -n 's/^ *Protocol: *//p' "$W/opencore/booted-efi.txt" | head -1)) ${BOOTED_FILE:-} [$BOOTED_HOW]$(grep -qx "$BOOTED_EFI" "$TMPBASE/.efis" && echo ", config collected" || echo ", config NOT collected")"
	elif [ "$ARCH" = x86_64 ]; then
		echo "Booted from  unknown: firmware and OpenCore do not say$(sed -n 's/^booted partition: not determined (\(.*\))$/; \1/p' "$W/opencore/booted-efi.txt" | head -1)"
	fi
	for d in "$W"/opencore/efi-*; do [ -d "$d" ] && grep -q 'probably the one that booted' "$d/disk.txt" && echo "             $(basename "$d") has the OpenCore log of this boot"; done
	echo
	echo "GPUs (PCI)"
	if [ -s "$TMPBASE/.gpus" ]; then sed 's/^/  /' "$TMPBASE/.gpus"; else echo "  none found in the I/O Registry"; fi
	grep '^  path:' "$W/gpu/pcie-links.txt" 2>/dev/null | grep -v 'no link$' | sed 's/^  /  /'
	echo
	echo "Displays"
	if [ -s "$TMPBASE/.displays" ]; then sed 's/^/  /' "$TMPBASE/.displays"; fi
	grep -E '^ {4,8}[A-Za-z].*:$|Chipset Model|Metal|Resolution|UI Looks like|Main Display|Connection Type|Vendor:|Device ID' "$W/gpu/system_profiler-displays.txt" 2>/dev/null | sed 's/^ *//; s/^/  /' | head -n 30
	echo
	echo "Driver stack (loaded version | installed file)"
	ockext() { for l in "$W"/opencore/efi-*/listing.txt; do [ -f "$l" ] && grep -m1 "^$1 " "$l" | awk -v efi="$(basename "$(dirname "$l")")" '{ printf "in %s: %s sha256 %s\n", efi, $2, substr($3, 1, 16) }'; done | head -1; }
	printf '  %-24s %s | %s\n' "IntelXe.kext" "$(kx 'io.github.yusufklncc.IntelXe ')" "$(ockext IntelXe.kext | grep . || { [ -d /Library/Extensions/IntelXe.kext ] && inst /Library/Extensions/IntelXe.kext; } || echo "not in /Library/Extensions or a mounted EFI")"
	for k in IntelXeFB NMXAccel NMXAGDC; do
		printf '  %-24s %s | %s\n' "$k.kext" "$(kx "$k")" "$(inst "/Library/Extensions/$k.kext")"
	done
	for b in /System/Library/Extensions/NVMTLDriver.bundle /Library/GPUBundles/NVMTLDriverLib.bundle /Library/GPUBundles/NVMTLDriver.bundle; do
		[ -d "$b" ] || continue
		_where=/Library; case "$b" in /System/*) _where="system volume" ;; esac
		printf '  %-24s %s | %s\n' "$(basename "$b")" "$_where" "$(inst "$b")"
	done
	printf '  %-24s %s\n' "Metal plugin named" "$(grep -m1 '"MetalPluginName" =' "$IOREGL" 2>/dev/null | sed 's/.*= //' | grep . || echo "none (withheld or no accelerator)")"
	printf '  %-24s %s | %s\n' "Mesa ANV" "$(sed -n 's/^Mesa: //p' "$W/driver/installed.txt" 2>/dev/null | grep . || echo -)" "$(inst "$ANV")"
	printf '  %-24s %s | %s\n' "Vulkan loader" "-" "$(grep -E '^/usr/local/lib/libvulkan\.1\.dylib' "$W/driver/installed.txt" 2>/dev/null | sed 's/.* -> //' | grep . || echo "not installed")"
	for c in libintelxeav1dec libintelxevenc; do
		printf '  %-24s %s | %s\n' "$c (VideoToolbox)" "-" "$(inst "/Library/GPUBundles/nvmtl/$c.dylib")"
	done
	printf '  %-24s %s\n' "package" "$(cat '/Library/Application Support/intel-gpu-macos-driver/installed-version' 2>/dev/null || echo 'no version recorded')$(grep -m1 '^IntelXe Manager' "$W/driver/stack.txt" 2>/dev/null | sed 's/:.*//; s/^/, /')$([ -d /Library/IntelXe/GL ] && echo ', Zink GL installed')"
	printf '  %-24s %s\n' "HiDPI switch" "NVRAM $_hn, override $_ho, DisplayResolutionEnabled $_hw"
	printf '  %-24s %s\n' "boot guard" "intelxe-guard ${_guard:-not set}, intelxe-accel-off ${_aoff:-not set}"
	for k in as.vit9696.Lilu WhateverGreen WhateverXe HybridPM CpuTopologyRebuild; do
		printf '  %-24s %s\n' "${k#as.vit9696.}" "$(kx "$k")"
	done
	if [ $INTELXE_PRESENT = 1 ]; then
		echo
		echo "IntelXe state (debug.intelxe.state)"
		sysctl -n debug.intelxe.state 2>/dev/null | head -n 8 | sed 's/^/  /'
	fi
	if [ "$PROBE" = 1 ]; then
		echo
		echo "Staged probe (XeProbe)"
		echo "  previous boot's breadcrumb: $(head -1 "$W/probe/nvram-breadcrumb.txt")"
		[ -s "$W/probe/results.txt" ] && sed 's/^/  /' "$W/probe/results.txt" | head -n 12
		[ -s "$W/probe/panics.txt" ] && echo "  probe watchdog panics: $(tr '\n' ' ' < "$W/probe/panics.txt")"
	fi
	echo
	echo "Problems seen"
	echo "  kernel panics, last $DAYS days: $NPANIC (all: $(grep -c . "$W/logs/panics-all.txt" 2>/dev/null))"
	for p in $(ls "$W"/logs/panics/*.txt 2>/dev/null | sort -r | head -n 3); do
		printf '    %s: %s\n' "$(basename "$p" .txt)" "$(grep -m1 -E 'panic\(' "$p" | cut -c1-150)"
	done
	echo "  crash/hang reports involving the stack, last $DAYS days: $NCRASH (GPU restarts: $NGPURESET)"
	[ "$ACPI" = 1 ] && echo "  ACPI tables collected (--acpi): $(ls "$W"/acpi/*.bin 2>/dev/null | sed 's#.*/##; s#\.bin$##' | tr '\n' ' ')"
	[ -f "$W/logs/counts.txt" ] && sed 's/^/  /' "$W/logs/counts.txt"
	if [ -s "$TMPBASE/.skipped" ]; then
		echo
		echo "Not collected"
		awk '/^ /{ print "  " $0; next } { print "  - " $0 }' "$TMPBASE/.skipped"
	fi
	echo
	echo "Removed before packing: serial numbers, platform UUID, MLB, ROM, MAC addresses, user and host names,"
	echo "Wi-Fi network names, monitor serial numbers (EDID), OpenCore PlatformInfo identifiers."
} > "$S" 2>/dev/null

# ---------------------------------------------------------------- redaction
step "redaction"
# 1. OpenCore config.plist: identifiers keep their element, value removed
for p in "$W"/opencore/efi-*/config.plist; do
	[ -f "$p" ] || continue
	awk -v MODE=redact "$PLIST_AWK" "$p" > "$p.tmp" && mv "$p.tmp" "$p"
done
# 2. one sed script for every text file
{
	awk -F'\t' '{ print length($2) "\t" $1 "\t" $2 }' "$SECRETS" | sort -rn | cut -f2- | while IFS='	' read -r kind v; do
		e=$(printf '%s' "$v" | sed 's/[][\\/.*+?(){}|^$]/\\&/g')
		printf 's/%s/<%s-redacted>/g\n' "$e" "$kind"
	done
	printf '%s\n' 's/("(IOPlatformSerialNumber|IOPlatformUUID|serial-number|SerialNumber|DisplaySerialNumber|AlphanumericSerialNumber|kUSBSerialNumberString|USB Serial Number|iSerialNumber|board-serial-number|platform-uuid|system-id|crashReporterKey|sleepWakeUUID|IOMACAddress|mac-address|local-mac-address|MLB|ROM|ApECID|SystemSerialNumber|SystemUUID)"[[:space:]]*[=:][[:space:]]*)("[^"]*"|<[^>]*>|[0-9]+)/\1"<redacted>"/g'
	printf '%s\n' 's/^([[:space:]]*(Serial Number[^:]*|Display Serial Number|Hardware UUID|Provisioning UDID|Computer Name|User Name|Local Host Name|MAC Address|Network Name|SSID): ).*/\1<redacted>/'
	printf '%s\n' 's#/Users/[^/[:space:]"'"'"']+#/Users/USER#g'
	printf '%s\n' 's#\\/Users\\/[^\\"]+#\\/Users\\/USER#g'
	printf '%s\n' 's/([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}/xx:xx:xx:xx:xx:xx/g'
	printf '%s\n' 's/[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}/XXXXX-XXXXX-XXXXX-XXXXX-XXXXX/g'
} > "$SEDF"
find "$W" -type f ! -name '*.bin' | while read -r f; do
	sed -E -f "$SEDF" "$f" > "$f.red" 2>/dev/null && mv "$f.red" "$f" || { rm -f "$f.red" "$f"; note "dropped (redaction failed): ${f#$W/}"; }
done
# file names (crash reports carry the host name)
find "$W" -type f | while read -r f; do
	b=$(basename "$f"); nb=$(printf '%s' "$b" | sed -E -f "$SEDF" | tr '<>' '__')
	[ "$b" = "$nb" ] || mv "$f" "$(dirname "$f")/$nb"
done

# 3. final check: no private value may remain, and nothing that looks like a Windows product key
FAIL=0
while IFS='	' read -r kind v; do
	if grep -rqF -- "$v" "$W" 2>/dev/null || find "$W" -name "*$v*" 2>/dev/null | grep -q .; then
		say "privacy check: a $kind value is still present in $(grep -rlF -- "$v" "$W" 2>/dev/null | sed "s#^$W/##" | head -3 | tr '\n' ' ')"
		FAIL=1
	fi
done < "$SECRETS"
if grep -rqaE '[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}' "$W" 2>/dev/null; then
	say "privacy check: something that looks like a product key is present"
	FAIL=1
fi
for p in "$W"/opencore/efi-*/config.plist; do
	[ -f "$p" ] || continue
	if awk -v MODE=values "$PLIST_AWK" "$p" | grep -qvxE 'REDACTED|0'; then
		say "privacy check: an OpenCore PlatformInfo value is still present in ${p#$W/}"
		FAIL=1
	fi
done
if [ $FAIL = 1 ]; then
	say "No report was written. Please open an issue and say that the privacy check failed (no file needed)."
	exit 3
fi

# ---------------------------------------------------------------- pack
END=$(date +%s)
{ echo; echo "finished: $(date '+%Y-%m-%d %H:%M:%S %z') ($((END - START)) s)"; echo "privacy check: passed"; } >> "$LOG"
(cd "$W" && find . -type f ! -name manifest.txt | sort | while read -r f; do shasum -a 256 "$f"; done) > "$W/manifest.txt"
OUT="$OUTDIR/$NAME.tar.gz"
tar -czf "$OUT" -C "$TMPBASE" "$NAME" || { say "could not write $OUT"; exit 1; }
chmod 644 "$OUT"
echo
echo "Report ID: $ID"
# one line per warning for IntelXe Manager: "Warning <code>: <text>" (the codes are in the warnings section above)
[ -s "$WARN" ] && awk -F'\t' '{ print "Warning " $1 ": " $2 }' "$WARN"
echo "File:      $OUT"
echo "Took $((END - START)) s. Nothing was uploaded. Attach the file to your GitHub issue and put the"
echo "report ID in the issue form. You can look inside first: tar -xzf $NAME.tar.gz"
