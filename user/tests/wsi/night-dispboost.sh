# SPDX-License-Identifier: MIT
# night phase dispboost (sourced by the night scripts, or run with bash on its own: tmo, say and $O are defined here when
# the night driver has not). The display boost (docs/design/DESIGN-display-boost.md) on the B580 test machine: IntelXe
# raises the GT's minimum clock for a short lease while NMXAccel sees frames still on the GPU at the vblank they were
# due at. A/B by debug.intelxe.disp_boost (root, sudo -n), interleaved ABBA:
#   1. DB_N (5) runs per setting: wait until the GT is idle (debug.intelxe.sensors "act" at or below DB_IDLE_MHZ for
#      DB_IDLE_S seconds; the clock SLPC drops to with nothing to do), then p4check smooth -t DB_SECS -v (a 640x360 pt
#      CAMetalLayer at the display's rate) with the GT clock sampled every DB_SAMPLE_MS: late frames in the first 500 ms
#      and overall, fps, the clock 0.5 s / 1 s after the launch and when it first reached DB_HIGH_MHZ, and the counters
#      (debug.nmxaccel_vbl_miss, the boost's misses / leases / renewals / time boosted)
#   2. DB_IDLE_SECS (20) s of idle desktop per setting: misses, leases, time boosted, mean clock, the card's average
#      power from its energy counter (debug.intelxe.sensors "card energy", PMT)
#   3. optional, the same while a video plays: VIDEO_CMD (a bash command line that plays for at least ~50 s; it and its
#      children are killed after) or VIDEO (a file, opened with VIDEO_APP, default QuickTime Player, looped through
#      osascript, which needs the Automation permission once); skipped when neither is given
# Results: $O/dispboost-runs.txt (one line per run), $O/dispboost-samples-*.txt (the clock), $O/dispboost-summary.txt
# (the table) and say lines. Needs: the console user logged in (the GUI part runs as a launchd agent in that session),
# p4check next to this script (or P4CHECK), sudo -n for the switch (without it only the current setting is measured).
# The switch goes back to its value at the start. Every external command runs under tmo (a bash timer; < 60 s each).
# Do not run while a game, a benchmark or another test runs. bash 3.2 (macOS /bin/bash) is enough.
DB_DIR=${DB_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
INTELXE_HOME=${INTELXE_HOME:-$HOME/b580}
P4CHECK=${P4CHECK:-$DB_DIR/p4check}
DB_N=${DB_N:-5}
DB_SECS=${DB_SECS:-3}
DB_IDLE_MHZ=${DB_IDLE_MHZ:-1250}   # act at or below this (0 = RC6) counts as idle; SLPC's default floor is 1200
DB_IDLE_S=${DB_IDLE_S:-3}
DB_HIGH_MHZ=${DB_HIGH_MHZ:-2500}
DB_SAMPLE_MS=${DB_SAMPLE_MS:-50}
DB_IDLE_SECS=${DB_IDLE_SECS:-20}
VIDEO=${VIDEO:-}
VIDEO_APP=${VIDEO_APP:-QuickTime Player}
VIDEO_CMD=${VIDEO_CMD:-}
O=${O:-$INTELXE_HOME/dispboost/$(date +%m%d-%H%M)}
mkdir -p "$O/dispboost"
if ! declare -F tmo >/dev/null; then
tmo() {   # tmo <seconds> <command...>: run it, kill it after <seconds>; its exit code (137 = killed)
	local s=$1; shift
	"$@" & local p=$!
	( trap 'kill $sp 2>/dev/null; exit 0' TERM; sleep "$s" & sp=$!; wait $sp; kill -9 $p 2>/dev/null ) > /dev/null 2>&1 &
	local w=$!
	wait $p; local r=$?
	kill $w 2>/dev/null; wait $w 2>/dev/null
	return $r
}
fi
declare -F say >/dev/null || say() { echo "$(date +%H:%M:%S) $*" | tee -a "$O/say.txt"; }

db_k() { tmo 5 sysctl -n "$1" 2>/dev/null || echo "?"; }
db_now() { tmo 5 /usr/bin/perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
db_act() { db_k debug.intelxe.sensors | sed -n 's/^act \([0-9]*\) MHz.*/\1/p'; }
db_energy() { db_k debug.intelxe.sensors | sed -n 's/.*card energy \([0-9.]*\) J.*/\1/p'; }
# one line of name value pairs: the boost's stats (IntelXe) and NMXAccel's vblank counters
db_counters() {
	local d; d=$(db_k debug.intelxe.disp_boost_stats)
	[ "$d" = "?" ] && d="disp_boost ?"
	echo "$d vbl_miss $(db_k debug.nmxaccel_vbl_miss) vbl_due $(db_k debug.nmxaccel_vbl_due) vbl_nophase $(db_k debug.nmxaccel_vbl_nophase)" \
	     "vbl_unknown $(db_k debug.nmxaccel_vbl_unknown) boost_fail $(db_k debug.nmxaccel_vbl_boost_fail) flips $(db_k debug.nmxaccel_flips)"
}
db_get() {   # db_get "line" NAME: the value after NAME
	local -a a; read -ra a <<< "$1"; local i
	for ((i = 0; i + 1 < ${#a[@]}; i++)); do [ "${a[i]}" = "$2" ] && { echo "${a[i+1]}"; return 0; }; done
	echo "?"
}
db_d() {   # db_d "before" "after" NAME: after - before ("?" when either is not a number)
	local a b; a=$(db_get "$1" "$3"); b=$(db_get "$2" "$3")
	[[ $a =~ ^[0-9]+$ && $b =~ ^[0-9]+$ ]] && echo $((b - a)) || echo "?"
}
db_root=0
db_set() {   # db_set 0|1: debug.intelxe.disp_boost
	[ $db_root = 1 ] || return 1
	tmo 10 sudo -n sysctl -w debug.intelxe.disp_boost="$1" > /dev/null 2>&1 && [ "$(db_k debug.intelxe.disp_boost)" = "$1" ]
}
# db_wait_idle: until act <= DB_IDLE_MHZ for DB_IDLE_S s in a row (4 samples a second), at most 30 s. Prints the last
# act; status 1 when the GT did not go idle.
db_wait_idle() {
	local need=$((DB_IDLE_S * 4)) run=0 i a=""
	for ((i = 0; i < 120; i++)); do
		a=$(db_act)
		if [[ $a =~ ^[0-9]+$ ]] && [ "$a" -le "$DB_IDLE_MHZ" ]; then run=$((run + 1)); else run=0; fi
		[ $run -ge $need ] && { echo "$a"; return 0; }
		sleep 0.25
	done
	echo "${a:-?}"
	return 1
}
# db_sampler FILE SECS: "epoch act_MHz req_MHz lease_active" every DB_SAMPLE_MS for SECS s (one perl process; each line
# one sysctl call for the sensors and one for the boost's stats)
db_sampler() {
	tmo $(($2 + 10)) /usr/bin/perl -MTime::HiRes=time,sleep -e '
		my ($secs, $ms) = @ARGV; my $t0 = time; $| = 1;
		while (time - $t0 < $secs) {
			my $s = `/usr/sbin/sysctl -n debug.intelxe.sensors 2>/dev/null`;
			my $d = `/usr/sbin/sysctl -n debug.intelxe.disp_boost_stats 2>/dev/null`;
			my ($act) = $s =~ /^act (\d+) MHz/; my ($req) = $s =~ /req (\d+) MHz/; my ($l) = $d =~ /\bactive (\d+)/;
			printf "%.3f %s %s %s\n", time, defined $act ? $act : "?", defined $req ? $req : "?", defined $l ? $l : "?";
			sleep($ms / 1000);
		}' "$2" "$DB_SAMPLE_MS" > "$1" 2>/dev/null
}
# db_gui TAG SECONDS ARGS...: p4check ARGS in the console user's GUI session (a launchd agent: ssh is a Background
# session; ProcessType Interactive, or the agent is throttled), output to $O/dispboost/TAG.out; waits until it ends.
db_gui() {
	local tag=$1 secs=$2; shift 2
	local L=org.hackintosh.dispboost.$tag P=$O/dispboost/$tag.plist out=$O/dispboost/$tag.out a args=""
	for a in "$P4CHECK" "$@"; do args="$args<string>$a</string>"; done
	cat > "$P" <<EOP
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict><key>Label</key><string>$L</string><key>ProgramArguments</key><array>$args</array>
<key>StandardOutPath</key><string>$out</string><key>StandardErrorPath</key><string>$out</string>
<key>ProcessType</key><string>Interactive</string><key>LimitLoadToSessionType</key><string>Aqua</string><key>AbandonProcessGroup</key><true/></dict></plist>
EOP
	tmo 10 launchctl bootout "gui/$(id -u)/$L" 2>/dev/null
	rm -f "$out"
	tmo 10 launchctl bootstrap "gui/$(id -u)" "$P" && tmo 10 launchctl kickstart "gui/$(id -u)/$L" || { echo "launch of $tag failed" > "$out"; return 1; }
	local i; for ((i = 0; i < secs * 4; i++)); do sleep 0.25; tmo 5 launchctl print "gui/$(id -u)/$L" 2>/dev/null | grep -q "state = running" || break; done
	tmo 10 launchctl bootout "gui/$(id -u)/$L" 2>/dev/null
	return 0
}

# 1: one smoothness run from idle. db_run SETTING RUN: one line in $O/dispboost-runs.txt
db_run() {
	local v=$1 n=$2 tag="smooth-b$1-$2" idle idleok=1 c0 c1 t0 sp out S late late500 fps ramp a05 a10 m15
	if [ $db_root = 1 ]; then db_set "$v" || { say "DISPBOOST: could not set disp_boost=$v, run $n skipped"; return 0; }; fi
	idle=$(db_wait_idle) || idleok=0
	c0=$(db_counters)
	S=$O/dispboost-samples-$tag.txt
	t0=$(db_now)
	db_sampler "$S" $((DB_SECS + 3)) & sp=$!
	db_gui "$tag" $((DB_SECS + 12)) smooth -t "$DB_SECS" -v
	wait $sp 2>/dev/null
	c1=$(db_counters)
	out=$O/dispboost/$tag.out
	# "smooth: 180 Hz display ..., 538 frames in 2.99 s = 179.6 fps, 3 of 537 late (>= 1.5 refreshes), gap mean ..."
	fps=$(sed -n 's/^smooth:.* = \([0-9.]*\) fps.*/\1/p' "$out" | head -1)
	late=$(sed -n 's/^smooth:.* fps, \([0-9]*\) of \([0-9]*\) late.*/\1\/\2/p' "$out" | head -1)
	late500=$(awk '/^late: at / { if ($3 + 0 < 500) n++ } END { print n + 0 }' "$out" 2>/dev/null)
	# the clock after the launch: at +0.5 s, +1 s, the mean over the first 1.5 s, when it first reached DB_HIGH_MHZ. act
	# reads 0 while the GT sits in RC6 (between frames): the requested clock stands in for it then
	read -r a05 a10 m15 ramp <<< "$(awk -v t0="$t0" -v hi="$DB_HIGH_MHZ" '
		$2 ~ /^[0-9]+$/ { c = $2; if (c == 0 && $3 ~ /^[0-9]+$/) c = $3; t = $1 - t0
			if (t >= 0.5 && a05 == "") a05 = c; if (t >= 1.0 && a10 == "") a10 = c
			if (t <= 1.5) { s += c; k++ } if (c >= hi && ramp == "") ramp = sprintf("%.2f", t) }
		END { printf "%s %s %s %s\n", (a05 == "" ? "?" : a05), (a10 == "" ? "?" : a10), (k ? int(s / k) : "?"), (ramp == "" ? "never" : ramp) }' "$S" 2>/dev/null)"
	[ -n "$fps" ] || { fps="?"; late="NO RESULT ($(head -1 "$out" 2>/dev/null))"; }
	printf '%s %s idle %s(%s) fps %s late %s late500 %s act05 %s act10 %s act15mean %s high_at %s vbl_miss %s misses %s grants %s renewals %s streak %s ratelimited %s boosted_ms %s\n' \
		"b$v" "$n" "$idleok" "$idle" "$fps" "$late" "$late500" "$a05" "$a10" "$m15" "$ramp" "$(db_d "$c0" "$c1" vbl_miss)" \
		"$(db_d "$c0" "$c1" misses)" "$(db_d "$c0" "$c1" grants)" "$(db_d "$c0" "$c1" renewals)" "$(db_d "$c0" "$c1" streak)" \
		"$(db_d "$c0" "$c1" ratelimited)" "$(db_d "$c0" "$c1" boosted_ms)" >> "$O/dispboost-runs.txt"
	say "DISPBOOST run $n boost $v: $(tail -1 "$O/dispboost-runs.txt" | cut -d' ' -f3-)"
}

# 2 and 3: a quiet sample. db_quiet TAG SETTING: misses, leases, time boosted, mean clock, average card power
db_quiet() {
	local tag=$1 v=$2 c0 c1 e0 e1 t0 t1 S mean pw
	if [ $db_root = 1 ]; then db_set "$v" || { say "DISPBOOST $tag: could not set disp_boost=$v, skipped"; return 0; }; fi
	sleep 2
	S=$O/dispboost-samples-$tag-b$v.txt
	c0=$(db_counters); e0=$(db_energy); t0=$(db_now)
	DB_SAMPLE_MS=250 db_sampler "$S" "$DB_IDLE_SECS"
	c1=$(db_counters); e1=$(db_energy); t1=$(db_now)
	mean=$(awk '$2 ~ /^[0-9]+$/ { s += $2; k++ } END { print (k ? int(s / k) : "?") }' "$S" 2>/dev/null)
	pw=$(awk -v a="$e0" -v b="$e1" -v t="$t0" -v u="$t1" 'BEGIN { if (a != "" && b != "" && u > t && b >= a) printf "%.2f", (b - a) / (u - t); else print "n/a" }')
	printf '%-6s b%s %5.1f s | vbl_miss %s misses %s grants %s renewals %s ratelimited %s boosted_ms %s | act mean %s MHz | card %s W\n' \
		"$tag" "$v" "$(awk -v t="$t0" -v u="$t1" 'BEGIN { print u - t }')" "$(db_d "$c0" "$c1" vbl_miss)" "$(db_d "$c0" "$c1" misses)" \
		"$(db_d "$c0" "$c1" grants)" "$(db_d "$c0" "$c1" renewals)" "$(db_d "$c0" "$c1" ratelimited)" "$(db_d "$c0" "$c1" boosted_ms)" \
		"$mean" "$pw" >> "$O/dispboost-quiet.txt"
	say "DISPBOOST $(tail -1 "$O/dispboost-quiet.txt")"
}
db_video_pid="" db_video_app=0
db_video_start() {   # 0: something plays
	if [ -n "$VIDEO_CMD" ]; then
		bash -c "$VIDEO_CMD" > "$O/dispboost/video-cmd.out" 2>&1 & db_video_pid=$!
		sleep 3; kill -0 "$db_video_pid" 2>/dev/null && return 0
		say "DISPBOOST video: VIDEO_CMD exited at once: $(head -2 "$O/dispboost/video-cmd.out" | tr '\n' ' ')"; return 1
	fi
	[ -n "$VIDEO" ] && [ -f "$VIDEO" ] || return 1
	[ -d "/Applications/$VIDEO_APP.app" ] || [ -d "/System/Applications/$VIDEO_APP.app" ] || { say "DISPBOOST video: no $VIDEO_APP"; return 1; }
	pgrep -xq "$VIDEO_APP" || db_video_app=1
	tmo 15 open -g -a "$VIDEO_APP" "$VIDEO" || return 1
	sleep 3
	if [ "$VIDEO_APP" = "QuickTime Player" ]; then
		tmo 15 osascript -e 'tell application "QuickTime Player" to tell document 1 to set looping to true' -e \
			'tell application "QuickTime Player" to play document 1' > "$O/dispboost/video-osa.out" 2>&1 ||
			{ say "DISPBOOST video: osascript could not start playback ($(head -1 "$O/dispboost/video-osa.out")): Automation permission?"; db_video_stop; return 1; }
	fi
	sleep 2
	return 0
}
db_video_stop() {
	if [ -n "$db_video_pid" ]; then   # VIDEO_CMD's bash and what it started
		tmo 5 pkill -P "$db_video_pid" 2>/dev/null; kill "$db_video_pid" 2>/dev/null; wait "$db_video_pid" 2>/dev/null; db_video_pid=""; return 0
	fi
	[ "$VIDEO_APP" = "QuickTime Player" ] && tmo 10 osascript -e 'tell application "QuickTime Player" to close every document saving no' > /dev/null 2>&1
	[ $db_video_app = 1 ] && tmo 10 osascript -e "tell application \"$VIDEO_APP\" to quit" > /dev/null 2>&1
	db_video_app=0
	return 0
}

db_summary() {
	{
		echo "== dispboost $(date '+%Y-%m-%d %H:%M'), $(db_k debug.intelxe.disp_boost_stats | cut -d' ' -f1-8)"
		echo "-- runs from idle (p4check smooth -t $DB_SECS -v), one per line"
		cat "$O/dispboost-runs.txt" 2>/dev/null
		echo "-- per setting: runs, late frames in the first 500 ms (sum, max), late overall (sum), mean fps, act +0.5 s (mean), reached $DB_HIGH_MHZ MHz at (mean s), vblank misses, leases, ms boosted"
		awk '{ m = $1; r[m]++; for (i = 3; i < NF; i++) {
				if ($i == "late500") { l5[m] += $(i+1); if ($(i+1) > mx[m]) mx[m] = $(i+1) }
				if ($i == "late") { split($(i+1), q, "/"); lt[m] += q[1] }
				if ($i == "fps" && $(i+1) ~ /^[0-9.]+$/) { f[m] += $(i+1); fk[m]++ }
				if ($i == "act05" && $(i+1) ~ /^[0-9]+$/) { a[m] += $(i+1); ak[m]++ }
				if ($i == "high_at" && $(i+1) ~ /^[0-9.]+$/) { h[m] += $(i+1); hk[m]++ }
				if ($i == "vbl_miss" && $(i+1) ~ /^[0-9]+$/) vm[m] += $(i+1)
				if ($i == "grants" && $(i+1) ~ /^[0-9]+$/) g[m] += $(i+1)
				if ($i == "boosted_ms" && $(i+1) ~ /^[0-9]+$/) bm[m] += $(i+1) } }
			END { for (m in r) printf "%-3s runs %d | late500 sum %d max %d | late sum %d | fps %.1f | act+0.5s %s | %s | vbl_miss %d | leases %d | boosted %d ms\n",
				m, r[m], l5[m], mx[m], lt[m], fk[m] ? f[m] / fk[m] : 0, ak[m] ? int(a[m] / ak[m]) " MHz" : "?",
				hk[m] ? sprintf("high at %.2f s (%d of %d runs)", h[m] / hk[m], hk[m], r[m]) : "never high", vm[m], g[m], bm[m] }' \
			"$O/dispboost-runs.txt" 2>/dev/null | sort
		echo "-- quiet samples ($DB_IDLE_SECS s each)"
		cat "$O/dispboost-quiet.txt" 2>/dev/null
	} > "$O/dispboost-summary.txt"
}

phase_dispboost() {
	local ws0 ws1 v0 i m order
	[ -x "$P4CHECK" ] || { say "DISPBOOST: no p4check at $P4CHECK, skipped"; return 0; }
	[ "$(stat -f %Su /dev/console)" = "$(id -un)" ] || { say "DISPBOOST: $(id -un) is not logged in at the console (login window?), skipped"; return 0; }
	if [ -x "$INTELXE_HOME/wsitest/gpu_busy.sh" ] && b=$("$INTELXE_HOME/wsitest/gpu_busy.sh"); then say "DISPBOOST: GPU busy ($b), skipped"; return 0; fi
	v0=$(db_k debug.intelxe.disp_boost)
	if ! [[ $v0 =~ ^[01]$ ]]; then say "DISPBOOST: this IntelXe.kext has no debug.intelxe.disp_boost ($v0), skipped"; return 0; fi
	[ "$(db_k debug.nmxaccel_vbl_miss)" = "?" ] && say "DISPBOOST: WARNING: NMXAccel has no debug.nmxaccel_vbl_miss: the installed NMXAccel reports no misses (boost never fires)"
	tmo 10 sudo -n true 2>/dev/null && db_root=1
	ws0=$(pgrep -x WindowServer)
	say "DISPBOOST start: WindowServer $ws0, sudo $db_root, disp_boost $v0, $(db_counters)"
	say "DISPBOOST: $(db_k debug.intelxe.sensors | cut -c1-100) | slpc_min $(db_k debug.intelxe.slpc_min) slpc_max $(db_k debug.intelxe.slpc_max) profile $(db_k debug.intelxe.slpc_profile) freq_manual $(db_k debug.intelxe.freq_manual)"
	rm -f "$O/dispboost-runs.txt" "$O/dispboost-quiet.txt"
	if [ $db_root = 1 ]; then
		for ((i = 1; i <= DB_N; i++)); do
			if [ $((i % 2)) = 1 ]; then order="0 1"; else order="1 0"; fi
			for m in $order; do db_run "$m" "$i"; done
		done
		for m in 0 1; do db_quiet idle "$m"; done
		if db_video_start; then
			for m in 0 1; do db_quiet video "$m"; done
			db_video_stop
		else
			say "DISPBOOST: video sample skipped (set VIDEO_CMD, or VIDEO=<file> with $VIDEO_APP)"
		fi
		db_set "$v0" || say "DISPBOOST: WARNING: could not put disp_boost back to $v0"
	else
		say "DISPBOOST: no sudo -n: only the current setting (disp_boost $v0) is measured"
		for ((i = 1; i <= DB_N; i++)); do db_run "$v0" "$i"; done
		db_quiet idle "$v0"
	fi
	ws1=$(pgrep -x WindowServer)
	say "DISPBOOST end: WindowServer $ws0 -> $ws1$( [ "$ws0" != "$ws1" ] && echo ' (RESTARTED)'), disp_boost $(db_k debug.intelxe.disp_boost), $(db_counters)"
	db_summary
	cat "$O/dispboost-summary.txt"
}
# run on its own: bash night-dispboost.sh
if [ "${BASH_SOURCE[0]}" = "$0" ]; then set -o pipefail; phase_dispboost; fi
