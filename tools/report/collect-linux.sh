#!/bin/sh
# SPDX-License-Identifier: MIT
# collect-linux.sh: Linux reference capture for the Intel GPU macOS driver project (v3).
#
# Run it from a Linux live USB on the same machine that runs macOS with our driver. It records what
# the Linux xe/i915 driver finds and programs on this exact hardware: PCI setup, firmware versions,
# GuC/HuC state, display and link state, the VBT and the EDIDs. We compare our driver against it.
#
# Read-only. POSIX sh; uses only what a live system has (lspci, modinfo, sysfs/debugfs reads).
# Without root it still collects PCI, sysfs and EDID data; with root (or passwordless sudo, as on
# most live USBs) it adds debugfs, the full PCI capability dump, dmesg and intel_reg.
# Writes xe-report-XR-xxxxxxxx.tar.gz to the current directory (or --out DIR).
#
#   sh collect-linux.sh [--out DIR] [--bench] [--acpi]      (recommended: sudo sh collect-linux.sh)
#   --bench   also time tools/clbench on each OpenCL platform that has the card (about a minute each)
#   --acpi    also the DSDT and SSDT tables (root): the GPU's ACPI device, backlight and _DSM methods. Never the MSDM
#             or SLIC table (they hold the Windows licence); the privacy check still runs over the tables
# For a caller that packs more files around this report (tools/probe-stick):
#   --id XR-XXXXXXXX     use this report ID instead of a new one
#   --time SECONDS       the time budget (default 60, 300 with --bench)
#   --literals-out FILE  also write the private values found (kind<TAB>value, mode 600) to FILE. It holds exactly
#                        what the report leaves out: keep it in RAM and out of every archive
#   --redact FILE PATH...  collect nothing: apply this script's redaction, with the values in FILE (as --literals-out
#                        writes them), to the text files under each PATH in place (.bin files stay as they are), then
#                        run the privacy check over PATH; exit 3 if a value or a product key is still there

COLLECTOR_VERSION=3
PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
export PATH
LC_ALL=C
export LC_ALL
umask 077

OUTDIR=$(pwd)
BENCH=0
ACPI=0
ID=""
TIME=""
LITOUT=""
LITIN=""
while [ $# -gt 0 ]; do
	case "$1" in
	--out) OUTDIR=$2; shift ;;
	--out=*) OUTDIR=${1#--out=} ;;
	--bench) BENCH=1 ;;
	--acpi) ACPI=1 ;;
	--id) ID=$2; shift ;;
	--time) TIME=$2; shift ;;
	--literals-out) LITOUT=$2; shift ;;
	--redact) LITIN=$2; shift 2; break ;;
	-h|--help) sed -n '3,31p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "unknown option: $1 (see --help)" >&2; exit 2 ;;
	esac
	shift
done
case "$ID" in ''|XR-[0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F][0-9A-F]) ;; *) echo "--id: XR- and 8 hex digits" >&2; exit 2 ;; esac
case "$TIME" in ''|[1-9]|[1-9][0-9]|[1-9][0-9][0-9]|[1-9][0-9][0-9][0-9]) ;; *) echo "--time: seconds" >&2; exit 2 ;; esac

# ---------------------------------------------------------------- redaction (the archive, and --redact)
# The private values this machine has (kind<TAB>value, longest first) become <kind-redacted>; then these fixed rules,
# one ERE<TAB>replacement per line. The sed script uses \001 as its delimiter, so a value or rule may hold a '/'.
RULES='((root|resume)=(PART)?UUID=)[0-9A-Fa-f-]+	\1<redacted>
(Device Serial Number|Serial Number|SerialNumber|serial)([:=][[:space:]]*|[[:space:]]+)[0-9A-Za-z][-0-9A-Za-z:._]{3,}	\1\2<redacted>
/home/[^/[:space:]"'"'"']+	/home/USER
([0-9A-Fa-f]{2}:){5}[0-9A-Fa-f]{2}	xx:xx:xx:xx:xx:xx
[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}	XXXXX-XXXXX-XXXXX-XXXXX-XXXXX'
PKEY='[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}-[A-Z0-9]{5}'
# make_sed LITERALS DIR: DIR/redact.sed, DIR/values (the values alone) and DIR/rules (the rules' patterns alone)
make_sed() {
	_d=$(printf '\001')
	: > "$2/values"
	{
		awk -F'\t' '{ print length($2) "\t" $1 "\t" $2 }' "$1" | sort -rn | cut -f2- | while IFS='	' read -r kind v; do
			printf '%s\n' "$v" >> "$2/values"
			e=$(printf '%s' "$v" | sed 's/[][\\.*+?(){}|^$]/\\&/g')
			printf 's%s%s%s<%s-redacted>%sg\n' "$_d" "$e" "$_d" "$kind" "$_d"
		done
		printf '%s\n' "$RULES" | while IFS='	' read -r p r; do printf 's%s%s%s%s%sg\n' "$_d" "$p" "$_d" "$r" "$_d"; done
	} > "$2/redact.sed"
	printf '%s\n' "$RULES" | cut -f1 > "$2/rules"
}
# redact_files DIR: the file names on stdin, each text file rewritten in place. A file in which no value and no rule
# matches is left alone (sed would not change it); a file sed fails on is dropped. Prints what it dropped.
redact_files() {
	while IFS= read -r f; do
		case "$f" in *.bin) continue ;; esac
		[ -f "$f" ] || continue
		if { [ -s "$1/values" ] && grep -qF -f "$1/values" "$f" 2>/dev/null; } || grep -qE -f "$1/rules" "$f" 2>/dev/null; then
			sed -E -f "$1/redact.sed" "$f" > "$f.red" 2>/dev/null && mv "$f.red" "$f" || { rm -f "$f.red" "$f"; printf 'dropped (redaction failed): %s\n' "$f"; }
		fi
	done
}
# privacy_check LITERALS DIR PATH...: 0 when no value and nothing shaped like a product key is left under PATH
privacy_check() {
	_l=$1; _v=$2/values; shift 2
	_r=0
	if [ -s "$_v" ] && grep -rqF -f "$_v" "$@" 2>/dev/null; then
		while IFS='	' read -r kind v; do
			_w=$(grep -rlF -- "$v" "$@" 2>/dev/null | head -3 | tr '\n' ' ')
			[ -n "$_w" ] && { printf 'privacy check: a %s value is still present in %s\n' "$kind" "$_w" >&2; _r=1; }
		done < "$_l"
	fi
	if grep -rhoaE "$PKEY" "$@" 2>/dev/null | grep -qv '^XXXXX-XXXXX-XXXXX-XXXXX-XXXXX$'; then   # not the redaction's own mark
		printf '%s\n' "privacy check: something that looks like a product key is present" >&2
		_r=1
	fi
	return $_r
}
if [ -n "$LITIN" ]; then
	[ -r "$LITIN" ] && [ $# -gt 0 ] || { echo "--redact FILE PATH...: FILE must be readable and at least one PATH given" >&2; exit 2; }
	RT=$(mktemp -d "${TMPDIR:-/tmp}/xe-redact.XXXXXX") || exit 1
	trap 'rm -rf "$RT"' EXIT
	make_sed "$LITIN" "$RT"
	find "$@" -type f | redact_files "$RT" >&2
	privacy_check "$LITIN" "$RT" "$@" || exit 3
	exit 0
fi
[ -d "$OUTDIR" ] && [ -w "$OUTDIR" ] || { echo "cannot write to $OUTDIR" >&2; exit 2; }
OUTDIR=$(cd "$OUTDIR" && pwd)

START=$(date +%s)
DEADLINE=$((START + 60))
[ "$BENCH" = 1 ] && DEADLINE=$((START + 300))
[ -n "$TIME" ] && DEADLINE=$((START + TIME))
[ -n "$ID" ] || ID="XR-$(od -An -N4 -tx1 /dev/urandom | tr -d ' \n' | tr 'a-f' 'A-F')"
NAME="xe-report-$ID"
TMPBASE=$(mktemp -d "${TMPDIR:-/tmp}/xe-report.XXXXXX") || { echo "mktemp failed" >&2; exit 1; }
W="$TMPBASE/$NAME"
SECRETS="$TMPBASE/.literals"   # values to remove; never part of the archive
LOG="$W/collector.txt"
mkdir -p "$W/system" "$W/gpu" "$W/display" "$W/driver" "$W/debugfs" "$W/logs" || exit 1
: > "$SECRETS"
: > "$TMPBASE/.skipped"
cleanup() { rm -rf "${TMPBASE:?}"; }
trap cleanup EXIT
trap 'exit 130' INT TERM

say() { printf '%s\n' "$*" >&2; }
note() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*" >> "$LOG"; }
skip() { printf '%s\n' "$*" >> "$TMPBASE/.skipped"; note "skipped: $*"; }
left() { echo $((DEADLINE - $(date +%s))); }
have() { command -v "$1" >/dev/null 2>&1; }

# run_to SECONDS cmd...: time-limited run (coreutils timeout); 124 on timeout
run_to() {
	_t=$1; shift
	_l=$(left); [ "$_l" -lt "$_t" ] && _t=$_l; [ "$_t" -lt 2 ] && _t=2
	if have timeout; then timeout -k 2 "$_t" "$@" </dev/null; else "$@" </dev/null; fi
}
cmd() {
	_f=$1; _s=$2; shift 2
	printf '$ %s\n' "$*" >> "$_f"
	run_to "$_s" "$@" >> "$_f" 2>&1
	_r=$?
	[ $_r = 124 ] && { printf '[timed out after %ss]\n' "$_s" >> "$_f"; note "timeout: $*"; }
	printf '\n' >> "$_f"
	return $_r
}
if [ "$(id -u)" = 0 ]; then SUDO=""; ROOT=1
elif have sudo && sudo -n true >/dev/null 2>&1; then SUDO="sudo -n"; ROOT=1
else SUDO=""; ROOT=0; fi
rcmd() { _f=$1; _s=$2; shift 2; cmd "$_f" "$_s" $SUDO "$@"; }   # root when available
rcat() { run_to 5 $SUDO cat "$1" 2>/dev/null; }

lit() {
	[ -n "$2" ] || return 0
	[ ${#2} -ge 4 ] || return 0
	case "$2" in *'
'*) return 0 ;; esac
	case "$2" in "To be filled by O.E.M."|"Default string"|"System Serial Number"|"Not Specified"|"None"|"0123456789"|"Not Applicable") return 0 ;; esac
	printf '%s\t%s\n' "$1" "$2" >> "$SECRETS"
}
upper() { printf '%s' "$1" | tr 'a-f' 'A-F'; }

say "Intel GPU: Linux reference capture ($ID), about a minute..."
{
	echo "collector: collect-linux.sh v$COLLECTOR_VERSION"
	echo "report id: $ID"
	echo "started: $(date '+%Y-%m-%d %H:%M:%S %z')"
	echo "uid: $(id -u)  root data: $([ $ROOT = 1 ] && echo yes${SUDO:+ (sudo -n)} || echo no)"
	echo
} > "$LOG"
[ $ROOT = 1 ] || skip "root-only data (debugfs, PCI capabilities incl. Resizable BAR, dmesg if restricted, intel_reg): not root and no passwordless sudo. Run: sudo sh collect-linux.sh"

# ---------------------------------------------------------------- private values (never written)
for f in product_serial board_serial chassis_serial product_uuid; do
	v=$(rcat "/sys/class/dmi/id/$f" | tr -d '\n'); lit serial "$v"
	case "$f" in product_uuid) lit uuid "$(upper "$v")" ;; esac
done
lit machine-id "$(cat /etc/machine-id 2>/dev/null)"
for a in /sys/class/net/*/address; do
	m=$(cat "$a" 2>/dev/null)
	case "$m" in ''|00:00:00:00:00:00) continue ;; esac
	lit mac "$m"; lit mac "$(upper "$m")"
	h=$(printf '%s' "$m" | tr -d ':'); case "$h" in *[a-f]*) lit mac "$h"; lit mac "$(upper "$h")" ;; esac
done
# the names a live system gives everyone: removing them would only blank words like "root" all over the report
user_lit() { case "$1" in ''|root|ubuntu|fedora|liveuser|mint|user|kali|manjaro|archiso|live|guest|test|admin) ;; *) lit user "$1" ;; esac; }
for u in "$(id -un 2>/dev/null)" "$SUDO_USER" "$(basename "$HOME")"; do user_lit "$u"; done
user_lit "$(getent passwd "${SUDO_USER:-$(id -un)}" 2>/dev/null | cut -d: -f5 | cut -d, -f1)"   # the full name
for h in "$(hostname 2>/dev/null)" "$(cat /etc/hostname 2>/dev/null)" "$(uname -n)"; do
	case "$h" in localhost|ubuntu|fedora|debian|archiso|archlinux|kali|mint|linux|'(none)') ;; *) lit host "$h" ;; esac
done
# disk and USB device serial numbers: a full kernel log names them (the probe stick keeps one)
for f in /sys/block/*/device/serial /sys/class/nvme/*/serial /sys/bus/usb/devices/*/serial /sys/block/*/device/wwid; do
	[ -r "$f" ] || continue
	case "$f" in /sys/bus/usb/devices/usb[0-9]*/serial) continue ;; esac   # a root hub: its controller's device name
	v=$(head -c 256 "$f" 2>/dev/null | tr -d '\000' | tr -s ' \t\n' ' ' | sed 's/^ //; s/ $//')
	case "$v" in [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f].[0-7]) continue ;; esac   # a root hub: its PCI address
	lit serial "$v"
	case "$f" in */wwid) lit serial "${v##* }" ;; esac   # t10.ATA <model> <serial>: the serial alone too
done
for f in /sys/block/*/device/vpd_pg80; do   # SCSI/ATA unit serial page: 4 header bytes, then the serial
	[ -r "$f" ] || continue
	lit serial "$(tail -c +5 "$f" 2>/dev/null | tr -d '\000' | sed 's/^[[:space:]]*//; s/[[:space:]]*$//')"
done
if have nmcli; then
	run_to 5 nmcli -t -f NAME,TYPE connection show 2>/dev/null | sed -n 's/:802-11-wireless$//p' | while IFS= read -r s; do
		[ ${#s} -ge 3 ] && printf 'ssid\t%s\n' "$s" >> "$SECRETS"
	done
fi
if have iw; then
	iw dev 2>/dev/null | sed -n 's/^[[:space:]]*ssid //p' | while IFS= read -r s; do [ ${#s} -ge 3 ] && printf 'ssid\t%s\n' "$s" >> "$SECRETS"; done
fi

# ---------------------------------------------------------------- system
note "system"
F="$W/system/os.txt"
cmd "$F" 5 uname -a
cat /etc/os-release >> "$F" 2>/dev/null
printf '\n$ cat /proc/cmdline\n%s\n\n' "$(cat /proc/cmdline)" >> "$F"
cmd "$F" 5 uptime
grep -m1 'model name' /proc/cpuinfo >> "$F"
printf 'cpus: %s\n' "$(grep -c '^processor' /proc/cpuinfo)" >> "$F"
grep -E 'MemTotal' /proc/meminfo >> "$F"
F="$W/system/dmi.txt"
for f in sys_vendor product_name product_version product_family board_vendor board_name board_version bios_vendor bios_version bios_date bios_release; do
	[ -r "/sys/class/dmi/id/$f" ] && printf '%-16s %s\n' "$f" "$(cat "/sys/class/dmi/id/$f")" >> "$F"
done
F="$W/system/boot.txt"
if [ -d /sys/firmware/efi ]; then
	echo "boot mode: UEFI" >> "$F"
	sb=$(od -An -tu1 /sys/firmware/efi/efivars/SecureBoot-8be4df61-93ca-11d2-aa0d-00e098032b8c 2>/dev/null | awk '{ print $NF }')
	echo "secure boot: $(case "$sb" in 1) echo enabled ;; 0) echo disabled ;; *) echo unknown ;; esac)" >> "$F"
else
	echo "boot mode: legacy BIOS" >> "$F"
fi
have mokutil && cmd "$F" 5 mokutil --sb-state

# ---------------------------------------------------------------- GPUs
note "pci"
have lspci || skip "lspci (pciutils) is not installed: PCI details come from sysfs only"
: > "$TMPBASE/.gpus"
for d in /sys/bus/pci/devices/*; do
	c=$(cat "$d/class" 2>/dev/null)
	case "$c" in 0x03*) basename "$d" >> "$TMPBASE/.gpus" ;; esac
done
F="$W/gpu/pci.txt"
have lspci && cmd "$F" 5 lspci -nn
have lspci && cmd "$F" 5 lspci -tv
hx_awk='function hx(s,   i, v) { v = 0; s = tolower(s); sub(/^0x/, "", s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }'
: > "$TMPBASE/.gpusum"
while read -r slot; do
	d=/sys/bus/pci/devices/$slot
	G="$W/gpu/$slot.txt"
	have lspci && rcmd "$G" 8 lspci -nnvvv -s "$slot"
	{
		echo "== sysfs"
		for f in vendor device subsystem_vendor subsystem_device revision class current_link_speed current_link_width max_link_speed max_link_width boot_vga enable power_state numa_node; do
			[ -r "$d/$f" ] && printf '%-20s %s\n' "$f" "$(cat "$d/$f")"
		done
		printf 'driver               %s\n' "$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)"
		echo "== resource (start end flags)"; cat "$d/resource" 2>/dev/null
		for r in "$d"/resource*_resize; do [ -r "$r" ] && printf '%s: %s\n' "$(basename "$r")" "$(cat "$r")"; done
	} >> "$G"
	bars=$(awk "$hx_awk"' { s = hx($1); e = hx($2); if (e > s) { z = e - s + 1; printf "%s%s", (n++ ? ", " : ""), (z >= 1048576 ? int(z / 1048576) " MB" : int(z / 1024) " KB") } }' "$d/resource" 2>/dev/null)
	printf '%s %s:%s sub %s:%s rev %s driver %s link %s x%s BARs %s\n' "$slot" \
		"$(sed 's/^0x//' "$d/vendor")" "$(sed 's/^0x//' "$d/device")" "$(sed 's/^0x//' "$d/subsystem_vendor")" \
		"$(sed 's/^0x//' "$d/subsystem_device")" "$(sed 's/^0x//' "$d/revision")" \
		"$(basename "$(readlink "$d/driver" 2>/dev/null)" 2>/dev/null)" "$(cat "$d/current_link_speed" 2>/dev/null)" \
		"$(cat "$d/current_link_width" 2>/dev/null)" "$bars" >> "$TMPBASE/.gpusum"
	if have lspci && grep -q 'Resizable BAR' "$G"; then
		grep -A8 'Resizable BAR' "$G" | grep -E 'BAR [0-9]: current size' | sed "s/^[[:space:]]*/    ReBAR /" >> "$TMPBASE/.gpusum"
	fi
	# the GPU's other functions (HD audio) and the bridges above it
	up=$(readlink -f "$d/.." 2>/dev/null)
	while [ -n "$up" ] && [ -e "$up/class" ]; do
		have lspci && rcmd "$W/gpu/$slot-upstream.txt" 5 lspci -nnvv -s "$(basename "$up")"
		up=$(readlink -f "$up/.." 2>/dev/null)
	done
	# xe / i915 frequency and tile info in sysfs
	for card in /sys/class/drm/card[0-9]*; do
		case "$card" in *-*) continue ;; esac
		[ "$(readlink -f "$card/device")" = "$(readlink -f "$d")" ] || continue
		{
			echo "== $(basename "$card")"
			find "$card/device/" -maxdepth 4 -path '*tile*/gt*/freq*/*' -type f 2>/dev/null | sort | while read -r f; do
				printf '%s = %s\n' "${f#$card/device/}" "$(cat "$f" 2>/dev/null | head -c 200)"
			done
			for f in "$card"/gt_*_freq_mhz "$card"/gt/gt*/rps_*_freq_mhz; do [ -r "$f" ] && printf '%s = %s\n' "${f#$card/}" "$(cat "$f")"; done
		} >> "$W/gpu/$slot-sysfs-freq.txt" 2>/dev/null
		echo "$(basename "$card") $slot" >> "$TMPBASE/.cards"
	done
done < "$TMPBASE/.gpus"

# ---------------------------------------------------------------- driver, firmware, kernel log
note "driver"
F="$W/driver/modules.txt"
lsmod 2>/dev/null | grep -E '^(xe|i915|drm|drm_[a-z_]+|ttm|mei[a-z_]*|intel_vsec|snd_hda_intel) ' > "$F"
for m in xe i915; do
	have modinfo && cmd "$W/driver/modinfo-$m.txt" 5 modinfo "$m"
done
[ -d /sys/module/xe/parameters ] && for p in /sys/module/xe/parameters/*; do printf 'xe.%s = %s\n' "$(basename "$p")" "$(cat "$p" 2>/dev/null)"; done >> "$W/driver/params.txt"
[ -d /sys/module/i915/parameters ] && for p in /sys/module/i915/parameters/*; do printf 'i915.%s = %s\n' "$(basename "$p")" "$(cat "$p" 2>/dev/null)"; done >> "$W/driver/params.txt"
ls -la /lib/firmware/xe /lib/firmware/i915 /usr/lib/firmware/xe /usr/lib/firmware/i915 2>/dev/null |
	grep -iE 'bmg|lnl|mtl|arl|ptl|dg2|tgl|adl|rkl|dg1|ehl|icl|xe|i915|:$' > "$W/driver/firmware-files.txt"
KRE='(^|[^a-z])xe([^a-z]|$)|i915|drm|guc|huc|dmc|gsc|vbt|edid|resizable|rebar'
for s in $(cat "$TMPBASE/.gpus"); do KRE="$KRE|$s"; done
if dmesg >/dev/null 2>&1; then
	dmesg 2>/dev/null | grep -iE "$KRE" | tail -n 4000 > "$W/logs/dmesg-gpu.txt"
elif [ $ROOT = 1 ]; then
	$SUDO dmesg 2>/dev/null | grep -iE "$KRE" | tail -n 4000 > "$W/logs/dmesg-gpu.txt"
elif have journalctl; then
	run_to 15 journalctl -k -b --no-pager 2>/dev/null | grep -iE "$KRE" | tail -n 4000 > "$W/logs/dmesg-gpu.txt"
fi
[ -s "$W/logs/dmesg-gpu.txt" ] || skip "kernel log: dmesg is restricted and journalctl gave nothing"

# ---------------------------------------------------------------- debugfs (root)
note "debugfs"
DBG=/sys/kernel/debug/dri
if [ $ROOT = 1 ] && run_to 5 $SUDO test -d "$DBG"; then
	run_to 10 $SUDO find "$DBG" -maxdepth 4 2>/dev/null | sort > "$W/debugfs/list.txt"
	# Read-only state files, as Linux names them for xe and i915. Never: forcewake, i915_gpu_info / error
	# capture, wedged, drop_caches or anything that acts when opened.
	ALLOW='/(info|name|gtidle/[a-z_]+|i915_capabilities|i915_display_info|i915_display_capabilities|i915_vbt|i915_dmc_info|i915_frequency_info|i915_power_domain_info|i915_edp_psr_status|i915_dp_mst_info|i915_shared_dplls_info|i915_ddb_info|i915_lpsp_status|i915_fbc_status|i915_gt_info|i915_rps_boost_info|i915_runtime_pm_status|i915_engine_info|i915_guc_info|i915_huc_load_status|i915_guc_load_status|i915_gsc_info|i915_sseu_status|i915_wa_registers|i915_dsb|vram[0-9]*_mm|stolen_mm|gtt_mm|sriov_info|gt[0-9]+/(hw_engines|hwconfig|topology|steering|pat|mocs|workarounds|register-save-restore|freq_info|stats|default_lrc_[a-z]+|uc/(guc_info|huc_info|guc_pc|guc_ctb|gsc_info|guc_log))|gt[0-9]+/uc/guc_log_level|[A-Za-z]+-[A-Za-z0-9-]+/(i915_dpcd|i915_panel_timings|i915_dsc_fec_support|i915_dsc_bpc|vrr_range|output_bpc|i915_psr_status|i915_hdcp_sink_capability|i915_lpsp_capability))$'
	grep -E "$ALLOW" "$W/debugfs/list.txt" | while read -r f; do
		[ "$(left)" -gt 3 ] || break
		rel=${f#$DBG/}
		out="$W/debugfs/$(printf '%s' "$rel" | tr '/' '_')"
		case "$rel" in
		*i915_vbt) run_to 5 $SUDO cat "$f" > "$out.bin" 2>/dev/null; [ -s "$out.bin" ] || rm -f "$out.bin" ;;
		*guc_log) run_to 5 $SUDO cat "$f" 2>/dev/null | head -c 262144 > "$out.txt" ;;
		*default_lrc_*) run_to 5 $SUDO cat "$f" 2>/dev/null | head -c 131072 > "$out.txt" ;;
		*) run_to 5 $SUDO cat "$f" 2>/dev/null | head -c 1048576 > "$out.txt" ;;
		esac
		[ -f "$out.txt" ] && [ ! -s "$out.txt" ] && rm -f "$out.txt"
	done
	[ "$(left)" -gt 3 ] || skip "part of debugfs: time budget used up"
else
	[ $ROOT = 1 ] && skip "debugfs: $DBG is not available (mount -t debugfs none /sys/kernel/debug)"
fi
if have intel_reg && [ $ROOT = 1 ]; then
	note "intel_reg"
	while read -r slot; do
		[ "$(sed 's/^0x//' /sys/bus/pci/devices/$slot/vendor)" = 8086 ] || continue
		run_to 20 $SUDO intel_reg --device "pci:slot=$slot" dump > "$W/driver/intel_reg-$slot.txt" 2>&1 ||
			run_to 20 $SUDO intel_reg dump > "$W/driver/intel_reg-$slot.txt" 2>&1
		head -c 3000000 "$W/driver/intel_reg-$slot.txt" > "$W/driver/.r" && mv "$W/driver/.r" "$W/driver/intel_reg-$slot.txt"
	done < "$TMPBASE/.gpus"
else
	skip "intel_reg dump: intel-gpu-tools not installed or not root (optional)"
fi

# ---------------------------------------------------------------- displays
note "display"
EDID_AWK='
function hx(s,   i, v) { v = 0; s = tolower(s); for (i = 1; i <= length(s); i++) v = v * 16 + index("0123456789abcdef", substr(s, i, 1)) - 1; return v }
function byte(e, i) { return hx(substr(e, 2 * i + 1, 2)) }
function setb(e, i, v) { return substr(e, 1, 2 * i) sprintf("%02x", v) substr(e, 2 * i + 3) }
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
F="$W/display/connectors.txt"
: > "$TMPBASE/.displays"
for c in /sys/class/drm/card*-*; do
	[ -d "$c" ] || continue
	n=$(basename "$c")
	{
		printf '== %s  status %s  enabled %s  dpms %s\n' "$n" "$(cat "$c/status" 2>/dev/null)" "$(cat "$c/enabled" 2>/dev/null)" "$(cat "$c/dpms" 2>/dev/null)"
		[ -s "$c/modes" ] && { echo "modes:"; head -n 40 "$c/modes" | sed 's/^/  /'; }
	} >> "$F"
	e=$(od -An -v -tx1 "$c/edid" 2>/dev/null | tr -d ' \n')
	[ -n "$e" ] || continue
	e=$(printf '%s\n' "$e" | awk "$EDID_AWK"'{ out = ""; s = $0; while (length(s) >= 256) { out = out (out == "" ? edid_clean(substr(s, 1, 256)) : substr(s, 1, 256)); s = substr(s, 257) } print out s }')
	printf '%s\n' "$e" > "$W/display/edid-$n.hex"
	printf '%s: %s\n' "$n" "$(printf '%s\n' "$e" | awk "$EDID_AWK"'{ print edid_desc($0) }')" >> "$TMPBASE/.displays"
done

# Laptop panels: the backlight interfaces (which one the GPU drives, its range). macOS needs the same maximum.
for b in /sys/class/backlight/*; do
	[ -d "$b" ] || continue
	{ printf '== %s (device %s)\n' "${b##*/}" "$(basename "$(readlink -f "$b/device" 2>/dev/null)")"
	  for k in type max_brightness brightness actual_brightness scale; do [ -r "$b/$k" ] && printf '%s = %s\n' "$k" "$(cat "$b/$k" 2>/dev/null)"; done
	  echo; } >> "$W/display/backlight.txt"
done
# --acpi: DSDT and SSDTs only, never MSDM/SLIC (the Windows licence) or anything else; .bin, so the text redaction
# leaves the tables intact, while the final privacy check (serials, MACs, product-key shapes) still reads them.
if [ "$ACPI" = 1 ]; then
	if [ $ROOT = 1 ]; then
		mkdir -p "$W/acpi"
		for t in /sys/firmware/acpi/tables/DSDT /sys/firmware/acpi/tables/SSDT*; do
			[ -e "$t" ] || continue
			case "${t##*/}" in DSDT|SSDT|SSDT[0-9]*) run_to 5 $SUDO cat "$t" > "$W/acpi/${t##*/}.bin" 2>/dev/null ;; esac
			[ -s "$W/acpi/${t##*/}.bin" ] || rm -f "$W/acpi/${t##*/}.bin"
		done
		ls -l /sys/firmware/acpi/tables 2>/dev/null | awk '{ print $NF }' | grep -vE '^(total|MSDM|SLIC)$' > "$W/acpi/table-names.txt"
	else
		skip "--acpi: the ACPI tables need root"
	fi
fi

# ---------------------------------------------------------------- user-space graphics
note "userspace"
if have vulkaninfo; then cmd "$W/driver/vulkaninfo-summary.txt" 20 vulkaninfo --summary
else skip "vulkaninfo --summary: vulkan-tools is not installed (optional)"; fi
have glxinfo && cmd "$W/driver/glxinfo-B.txt" 10 glxinfo -B
# Video: what the media driver offers on this generation (VA-API profiles and entrypoints)
if have vainfo; then cmd "$W/driver/vainfo.txt" 15 vainfo --display drm
else skip "vainfo: not installed (optional; package vainfo)"; fi
# OpenCL: Linux can run two on the same card, Mesa's rusticl (Mesa's compiler, the one the macOS driver uses) and
# Intel's compute runtime (Intel's IGC, as on Windows). Which are installed, and their versions.
if have clinfo; then
	cmd "$W/driver/clinfo.txt" 15 clinfo -l
	RUSTICL_ENABLE=iris cmd "$W/driver/clinfo.txt" 20 sh -c "clinfo 2>/dev/null | grep -E '^  (Platform Name|Platform Version)|^  Device (Name|Version)|^  Driver Version|^  Max compute units|^  Max clock frequency'"
else
	skip "clinfo: not installed (optional; package clinfo)"
fi
# GT frequencies as xe reports them (read-only sysfs, no root): the limits and the current/actual clock at rest.
for g in /sys/class/drm/card*/device/tile*/gt*/freq*; do
	[ -d "$g" ] || continue
	for k in act_freq cur_freq min_freq max_freq rp0_freq rpe_freq rpa_freq rpn_freq throttle/status; do
		[ -r "$g/$k" ] && printf '%s/%s = %s\n' "${g#/sys/class/drm/}" "$k" "$(cat "$g/$k" 2>/dev/null)"
	done >> "$W/driver/gt-freq.txt"
done
# --bench: tools/clbench (the same OpenCL kernels as on macOS and Windows) on every OpenCL platform that has the card.
if [ "$BENCH" = 1 ]; then
	SD="$(cd "$(dirname "$0")" && pwd)"; CB="$SD/clbench-linux"
	if [ ! -x "$CB" ] && [ -f "$SD/clbench.c" ] && have cc; then   # build it here: one C file, OpenCL loaded at run time
		cc -O2 -o "$TMPBASE/clbench-linux" "$SD/clbench.c" -ldl -lm >/dev/null 2>&1 && CB="$TMPBASE/clbench-linux"
	fi
	if [ -x "$CB" ]; then
		mkdir -p "$W/bench"
		n=$(clinfo -l 2>/dev/null | grep -c '^Platform')
		i=0
		while [ "$i" -lt "${n:-1}" ]; do
			( cd "$W/bench" && RUSTICL_ENABLE=iris CLBENCH_PLATFORM=$i run_to 90 "$CB" ) > "$W/bench/clbench-platform$i.txt" 2>&1
			i=$((i + 1))
		done
		rm -f "$W/bench"/clbench-*.bin
	else
		skip "--bench: no clbench-linux next to this script, and no clbench.c and C compiler to build it"
	fi
fi

# ---------------------------------------------------------------- summary
note "summary"
S="$W/summary.txt"
dmi() { cat "/sys/class/dmi/id/$1" 2>/dev/null; }
fwline() { # GuC/HuC/DMC versions as the kernel reported them
	grep -hiE 'guc firmware|huc firmware|dmc firmware|gsc firmware|using guc|guc: .*version|huc: .*(version|authenticated)|finished loading dmc' "$W/logs/dmesg-gpu.txt" 2>/dev/null | sed 's/^\[[^]]*\] //' | sort -u | head -n 12
	grep -hE '(found|wanted|version).*[0-9]+\.[0-9]+\.[0-9]+|status:' "$W"/debugfs/*uc_guc_info.txt "$W"/debugfs/*uc_huc_info.txt 2>/dev/null | sort -u | head -n 12
}
{
	echo "Intel GPU: Linux reference capture $ID"
	echo "Created $(date '+%Y-%m-%d %H:%M %z') by collect-linux.sh v$COLLECTOR_VERSION"
	echo
	echo "OS           $(. /etc/os-release 2>/dev/null; echo "$PRETTY_NAME")"
	echo "Kernel       $(uname -r) $(uname -m)"
	echo "Machine      $(dmi sys_vendor) $(dmi product_name) $(dmi product_version)"
	echo "Board        $(dmi board_vendor) $(dmi board_name) $(dmi board_version)"
	echo "BIOS         $(dmi bios_vendor) $(dmi bios_version) ($(dmi bios_date))"
	echo "CPU          $(grep -m1 'model name' /proc/cpuinfo | sed 's/.*: //'), $(grep -c '^processor' /proc/cpuinfo) threads"
	echo "Firmware     $(tr '\n' ' ' < "$W/system/boot.txt" | sed 's/\$ mokutil --sb-state//')"
	echo "Root data    $([ $ROOT = 1 ] && echo "yes (debugfs, PCI capabilities)" || echo "no: run with sudo for debugfs, ReBAR and firmware state")"
	echo
	echo "GPUs"
	if [ -s "$TMPBASE/.gpusum" ]; then sed 's/^/  /' "$TMPBASE/.gpusum"; else echo "  no display-class PCI device found"; fi
	have lspci && while read -r slot; do printf '  %s\n' "$(lspci -s "$slot" 2>/dev/null)"; done < "$TMPBASE/.gpus"
	echo
	echo "Firmware as loaded by Linux"
	fwline | sed 's/^/  /'
	echo
	echo "Displays"
	if [ -s "$TMPBASE/.displays" ]; then sed 's/^/  /' "$TMPBASE/.displays"; else echo "  no EDID found in /sys/class/drm"; fi
	grep -h '^== ' "$W/display/connectors.txt" 2>/dev/null | grep -w connected | sed 's/^== /  /'
	if [ -s "$W/driver/vulkaninfo-summary.txt" ]; then
		echo
		echo "Vulkan"
		grep -E 'deviceName|driverName|driverInfo|apiVersion|conformanceVersion' "$W/driver/vulkaninfo-summary.txt" | sed 's/^[[:space:]]*/  /' | head -n 12
	fi
	if [ -s "$W/driver/clinfo.txt" ]; then
		echo
		echo "OpenCL"
		grep -E '^Platform|Device #' "$W/driver/clinfo.txt" | sed 's/^/  /' | head -n 8
	fi
	if [ -s "$W/driver/gt-freq.txt" ]; then
		echo
		echo "GT clock (MHz)"
		grep -E '/(act_freq|min_freq|max_freq|rp0_freq) ' "$W/driver/gt-freq.txt" | sed 's/^/  /' | head -n 8
	fi
	for b in "$W"/bench/clbench-platform*.txt; do
		[ -f "$b" ] || continue
		echo
		echo "clbench ($(basename "$b" .txt))"
		grep -E '^== clbench|GFLOP|GB/s|^conv|^dw|^blur|^hist|^tiny|^scale' "$b" | sed 's/^/  /'
	done
	echo
	echo "Collected: $(find "$W/debugfs" -type f ! -name list.txt 2>/dev/null | wc -l | tr -d ' ') debugfs files, $(ls "$W"/display/edid-*.hex 2>/dev/null | wc -l | tr -d ' ') EDID(s), $(cat "$W/logs/dmesg-gpu.txt" 2>/dev/null | grep -c .) kernel log lines"
	[ -n "$(ls "$W"/debugfs/*i915_vbt.bin 2>/dev/null)" ] && echo "VBT: included (debugfs i915_vbt)"
	if [ -s "$TMPBASE/.skipped" ]; then
		echo
		echo "Not collected"
		awk '/^ /{ print "  " $0; next } { print "  - " $0 }' "$TMPBASE/.skipped"
	fi
	echo
	echo "Removed before packing"
	echo "  Serial numbers (machine, board, disks, USB devices, monitors), UUIDs, machine-id, MAC addresses,"
	echo "  user and host names, Wi-Fi network names."
	if [ "$ACPI" = 1 ]; then echo "  ACPI: the DSDT and SSDT tables only (--acpi), never MSDM or SLIC."; else echo "  No ACPI tables are read."; fi
} > "$S" 2>/dev/null

# ---------------------------------------------------------------- redaction
note "redaction"
[ -n "$LITOUT" ] && { cp "$SECRETS" "$LITOUT" && chmod 600 "$LITOUT"; }
make_sed "$SECRETS" "$TMPBASE"
find "$W" -type f | redact_files "$TMPBASE" | while IFS= read -r l; do note "${l%%: *}: ${l#*: $W/}"; done
if ! privacy_check "$SECRETS" "$TMPBASE" "$W"; then
	say "No report was written. Please open an issue and say that the privacy check failed (no file needed)."
	exit 3
fi

# ---------------------------------------------------------------- pack
END=$(date +%s)
{ echo; echo "finished: $(date '+%Y-%m-%d %H:%M:%S %z') ($((END - START)) s)"; echo "privacy check: passed"; } >> "$LOG"
(cd "$W" && find . -type f ! -name manifest.txt | sort | while read -r f; do sha256sum "$f"; done) > "$W/manifest.txt"
OUT="$OUTDIR/$NAME.tar.gz"
tar -czf "$OUT" -C "$TMPBASE" "$NAME" || { say "could not write $OUT"; exit 1; }
chmod 644 "$OUT"
[ -n "$SUDO_USER" ] && chown "$SUDO_USER" "$OUT" 2>/dev/null
echo
echo "Report ID: $ID"
echo "File:      $OUT"
echo "Took $((END - START)) s. Nothing was uploaded. Copy the file somewhere that survives the reboot"
echo "(a USB stick or your Windows/macOS disk), attach it to your GitHub issue and put the report ID in the form."
