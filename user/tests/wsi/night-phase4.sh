# SPDX-License-Identifier: MIT
# night phase phase4 (sourced by the night scripts, or run with bash on its own: tmo, say and $O are defined here when
# the night driver has not). gpuperf phase 4 on the B580 test machine (its 2560x1440 180 Hz monitor): WindowServer's
# composite scanned out without the per-frame readback into its system-memory pages (nullmoth/plugin/nvmtl_scanout_p4.h),
# A/B/C against the readback modes, switched at run time by root-owned diag flags (non-release plugin, sudo -n):
#   full  /tmp/nvmtl-scanout-readback-full    every frame read back (what 0.1.5 did at debug.intelxe.surfvram < 2)
#   off   /tmp/nvmtl-no-scanout-readback      no readback, no marks (td016's default; CPU readers may see old frames)
#   p4    neither flag                        phase 4 (this build's default)
# For each mode:
#   1. WindowServer GPU time: the plugin's GPUTIME (/tmp/nvmtl-gputime, root-owned) for ~6 s while p4check smooth runs
#      at the display's rate: WindowServer's "GPU busy in this process" % of wall and ms per command buffer (its
#      composites) from its plugin log
#   2. smoothness: p4check smooth -t 6 (IntelXe Manager's smoothness test): fps, frames a refresh or more late
#   3. screenshots: p4check show (a 300x300 pt patch that holds colour k until the capturer acks) + screencapture -R of
#      the patch per colour, 12 colours: each picture must show the colour on screen when it was taken
#   4. screen recording: p4check show -free 200 (a colour every 200 ms) + screencapture -v -V 9; the patch's colour per
#      video frame (ffmpeg crop, 1x1) must never step back to an older colour and must end on the last one
#   5. p4 only, copy-instead-of-flip: debug.nmxaccel_flip=0 for the screenshot test (frames the kernel copies must be
#      current: they page off from the scan-out buffer first), then flips back on
#   and the counters around each step: debug.nmxaccel_p4_* (marks, pageoffs, noresource, failed, busy), flips, copies.
# Results: $O/phase4-*.txt, pictures/recordings in $O/phase4/, the summary table in $O/phase4-summary.txt and say lines.
# Needs: the console user logged in (the GUI parts run as launchd agents in that session), p4check next to this script
# (or P4CHECK), sudo -n for the flags (without it only the installed default is measured), ffmpeg for step 4 (FFMPEG,
# default $INTELXE_HOME/video/ffmpeg; without it the recording is kept but not checked). The flags are removed at the
# end (default = phase 4). Every external command runs under tmo (a bash timer: no perl / env / sh wrapper).
# Do not run while a game or benchmark runs.
PHASE4_DIR=${PHASE4_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
INTELXE_HOME=${INTELXE_HOME:-$HOME/b580}
P4CHECK=${P4CHECK:-$PHASE4_DIR/p4check}
FFMPEG=${FFMPEG:-$INTELXE_HOME/video/ffmpeg}
PHASE4_MODES=${PHASE4_MODES:-"full off p4"}
O=${O:-$INTELXE_HOME/phase4/$(date +%m%d-%H%M)}
mkdir -p "$O/phase4"
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

p4_k() { tmo 5 sysctl -n "$1" 2>/dev/null || echo "?"; }
p4_counters() {   # one line: the kernel's phase 4 and flip counters
	echo "marks $(p4_k debug.nmxaccel_p4_marks) noresource $(p4_k debug.nmxaccel_p4_noresource) pageoffs $(p4_k debug.nmxaccel_p4_pageoffs)" \
	     "failed $(p4_k debug.nmxaccel_p4_failed) busy $(p4_k debug.nmxaccel_p4_busy) flips $(p4_k debug.nmxaccel_flips)" \
	     "copies $(p4_k debug.nmxaccel_scanout_copies) refused $(p4_k debug.nmxaccel_flip_refused) l2fail $(p4_k debug.nmxaccel_flip_l2fail)"
}
p4_delta() {   # p4_delta "before" "after": the counters that moved, "name +n"
	local -a a b; read -ra a <<< "$1"; read -ra b <<< "$2"; local i out=""
	for ((i = 0; i + 1 < ${#a[@]}; i += 2)); do
		[[ ${a[i+1]} =~ ^[0-9]+$ && ${b[i+1]} =~ ^[0-9]+$ ]] || continue
		out="$out ${a[i]} +$((b[i+1] - a[i+1]))"
	done
	echo "${out# }"
}
p4_root=0
p4_flag() {   # p4_flag MODE: the root-owned diag flags for that readback mode
	[ $p4_root = 1 ] || return 1
	tmo 10 sudo -n rm -f /tmp/nvmtl-no-scanout-readback /tmp/nvmtl-scanout-readback-full
	case $1 in
		full) tmo 10 sudo -n touch /tmp/nvmtl-scanout-readback-full ;;
		off) tmo 10 sudo -n touch /tmp/nvmtl-no-scanout-readback ;;
	esac
	sleep 1.5   # the plugin polls its flags every 250 ms; a few composites in the new mode
}
p4_wslog=""
p4_find_wslog() {   # the plugin log WindowServer writes (/tmp/nvmtl.log, or its own TMPDIR when that one is not its)
	local ws f; ws=$(pgrep -x WindowServer)
	for f in /tmp/nvmtl.log; do
		[ -f "$f" ] && tmo 10 grep -aq "^pid $ws NVMTL" "$f" && { p4_wslog=$f; return 0; }
	done
	[ $p4_root = 1 ] || return 1
	f=$(tmo 20 sudo -n find /private/var/folders -maxdepth 4 -name nvmtl.log -user _windowserver 2>/dev/null | head -1)
	[ -n "$f" ] && { p4_wslog=$f; return 0; }
	return 1
}
p4_wslog_size() { if [ -z "$p4_wslog" ]; then echo 0; elif [ -r "$p4_wslog" ]; then stat -f %z "$p4_wslog"; else tmo 5 sudo -n stat -f %z "$p4_wslog"; fi; }
p4_wslog_since() {   # p4_wslog_since OFFSET: WindowServer's lines appended since
	local ws; ws=$(pgrep -x WindowServer)
	[ -n "$p4_wslog" ] || return 0
	if [ -r "$p4_wslog" ]; then tail -c +$(($1 + 1)) "$p4_wslog"; else tmo 10 sudo -n tail -c +$(($1 + 1)) "$p4_wslog"; fi | grep -a "^pid $ws NVMTL"
}
# p4_gui TAG SECONDS ARGS...: p4check ARGS in the console user's GUI session (a launchd agent: ssh is a Background
# session), output to $O/phase4/TAG.out. P4_GUI_WAIT=0: started, not waited for (p4_gui_stop TAG ends it).
p4_gui() {
	local tag=$1 secs=$2; shift 2
	local L=org.hackintosh.p4check.$tag P=$O/phase4/$tag.plist out=$O/phase4/$tag.out a args=""
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
	[ "${P4_GUI_WAIT:-1}" = 0 ] && return 0
	local i; for ((i = 0; i < secs * 4; i++)); do sleep 0.25; tmo 5 launchctl print "gui/$(id -u)/$L" 2>/dev/null | grep -q "state = running" || break; done
	p4_gui_stop "$tag"
}
p4_gui_stop() { tmo 10 launchctl bootout "gui/$(id -u)/org.hackintosh.p4check.$1" 2>/dev/null; return 0; }

# 1 + 2: GPUTIME of WindowServer while the smoothness test runs
p4_gputime_smooth() {   # MODE
	local m=$1 off made=0
	off=$(p4_wslog_size)
	if [ $p4_root = 1 ]; then tmo 10 sudo -n touch /tmp/nvmtl-gputime && made=1; fi
	p4_gui "smooth-$m" 15 smooth -t 6
	[ $made = 1 ] && tmo 10 sudo -n rm -f /tmp/nvmtl-gputime
	sleep 0.5
	p4_wslog_since "$off" > "$O/phase4-$m-wslog.txt"
	grep -a "GPUTIME [0-9.]* s:" "$O/phase4-$m-wslog.txt" > "$O/phase4-$m-gputime.txt"
	# "GPUTIME 3.00 s: 0 frames (0.0 fps), 540 cbs (540.0 total), GPU busy in this process 12.3% of wall = 369.000 ms total; ..."
	# (busy is "ms total" when the process presented nothing, as WindowServer, else "ms/frame": times the frames)
	P4_GT=$(awk '{ f = 0; for (i = 1; i <= NF; i++) { if ($i == "frames") f = $(i-1); if ($i == "cbs") cbs += $(i-1); if ($i == "s:") wall += $(i-1)
		if ($i == "wall") { b = $(i+2); if ($(i+3) == "ms/frame") b *= f; busy += b } } }
		END { if (cbs > 0 && wall > 0) printf "busy %.1f%% of wall, %.3f ms/cb, %.0f cbs/s", 100 * busy / (wall * 1000), busy / cbs, cbs / wall; else print "no GPUTIME lines" }' \
		"$O/phase4-$m-gputime.txt")
	P4_SMOOTH=$(grep -a "^smooth:" "$O/phase4/smooth-$m.out" | head -1 | sed 's/^smooth: //')
	say "PHASE4 $m: WindowServer GPU $P4_GT | smooth ${P4_SMOOTH:-NO RESULT}"
}
# 3: screenshots of a held colour
p4_shots() {   # TAG [N]
	local tag=$1 n=${2:-12} D=$O/phase4/show-$1 k st x y w h pass=0 fail=0 last=-1 t0 f
	mkdir -p "$D"; rm -f "$D/state" "$D/ack" "$D/done"
	P4_GUI_WAIT=0 p4_gui "show-$tag" 0 show "$D" -n "$n"
	t0=$SECONDS
	while [ $((SECONDS - t0)) -lt $((n * 6 + 15)) ] && [ ! -e "$D/done" ]; do
		st=$(cat "$D/state" 2>/dev/null); read -r k x y w h _ <<< "$st"
		if [[ ! $k =~ ^[0-9]+$ ]] || [ "$k" = "$last" ]; then sleep 0.05; continue; fi
		f=$D/shot-$k.png
		if ! tmo 15 /usr/sbin/screencapture -x -R "$x,$y,$w,$h" -t png "$f" 2>/dev/null || [ ! -s "$f" ]; then
			echo "probe $k screencapture-failed" >> "$D/probe.txt"; fail=$((fail + 1))
		elif tmo 10 "$P4CHECK" probe "$f" "$k" >> "$D/probe.txt" 2>&1; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
		last=$k; echo "$k" > "$D/ack"
	done
	p4_gui_stop "show-$tag"
	P4_SHOTS="$pass/$((pass + fail)) current"
	[ $fail -gt 0 ] && P4_SHOTS="$P4_SHOTS, $fail STALE or failed: $(awk '$3 != "got" || $2 % 6 != $4' "$D/probe.txt" | head -3 | tr '\n' ' ')"
	[ $((pass + fail)) = 0 ] && P4_SHOTS="NO SHOTS (screencapture refused, or the show agent did not start: $(head -2 "$O/phase4/show-$tag.out" 2>/dev/null | tr '\n' ' '))"
}
# 4: a screen recording of colours that change every 200 ms
p4_record() {   # TAG
	local tag=$1 D=$O/phase4/rec-$1 st k x y w h sc W H mov rc
	mkdir -p "$D"; rm -f "$D/state" "$D/done"
	P4_GUI_WAIT=0 p4_gui "rec-$tag" 0 show "$D" -n 30 -free 200
	local i; for ((i = 0; i < 40; i++)); do [ -s "$D/state" ] && break; sleep 0.1; done
	read -r k x y w h sc W H <<< "$(cat "$D/state" 2>/dev/null)"
	mov=$D/rec.mov
	tmo 40 /usr/sbin/screencapture -x -v -V 9 "$mov" > "$D/screencapture.txt" 2>&1; rc=$?
	for ((i = 0; i < 50; i++)); do [ -e "$D/done" ] && break; sleep 0.2; done
	p4_gui_stop "rec-$tag"
	if [ ! -s "$mov" ]; then P4_REC="NO RECORDING (screencapture -v exit $rc: $(head -1 "$D/screencapture.txt"))"; return; fi
	if [ ! -x "$FFMPEG" ] || [[ ! $W =~ ^[0-9]+$ ]] || [ "$W" = 0 ]; then P4_REC="recorded $(stat -f %z "$mov") bytes, not checked (no ffmpeg or no patch position)"; return; fi
	tmo 60 "$FFMPEG" -nostdin -loglevel error -i "$mov" -vf "crop=iw*$w/$W:ih*$h/$H:iw*$x/$W:ih*$y/$H,scale=1:1:flags=area" -f rawvideo -pix_fmt rgb24 -y "$D/patch.rgb"
	tmo 20 "$P4CHECK" rawseq "$D/patch.rgb" > "$D/rawseq.txt" 2>&1; rc=$?
	local seq want; seq=$(sed -n 's/.*runs (\([0-9]*\)).*/\1/p' "$D/rawseq.txt"); want=$(((30 - 1) % 6))
	P4_REC="$(grep '^rawseq: [0-9]' "$D/rawseq.txt" | sed 's/^rawseq: //')"
	[ $rc != 0 ] && P4_REC="STALE FRAMES: $P4_REC"
	[ "${seq: -1}" != "$want" ] && P4_REC="$P4_REC; ENDS ON ${seq: -1}, not the last colour $want"
}
p4_mode() {   # MODE: one readback mode, all checks
	local m=$1 c0 c1 c2 c3
	p4_flag "$m" || { [ "$m" = p4 ] || { say "PHASE4 $m: skipped (no sudo -n for the flags)"; return 0; }; }
	c0=$(p4_counters); p4_gputime_smooth "$m"; c1=$(p4_counters)
	p4_shots "$m"; c2=$(p4_counters)
	p4_record "$m"; c3=$(p4_counters)
	{ echo "== $m"; echo "gputime+smooth: $(p4_delta "$c0" "$c1")"; echo "shots: $(p4_delta "$c1" "$c2")"; echo "recording: $(p4_delta "$c2" "$c3")"; } >> "$O/phase4-counters.txt"
	say "PHASE4 $m: shots $P4_SHOTS | counters $(p4_delta "$c1" "$c2")"
	say "PHASE4 $m: recording $P4_REC | counters $(p4_delta "$c2" "$c3")"
	printf '%-5s | %-48s | %-70s | %s | %s\n' "$m" "$P4_GT" "${P4_SMOOTH:-none}" "$P4_SHOTS" "$P4_REC" >> "$O/phase4-summary.txt"
	if [ "$m" = p4 ] && [ $p4_root = 1 ] && [ "$(p4_k debug.nmxaccel_flip)" = 1 ]; then   # 5: copy instead of flip
		tmo 10 sudo -n sysctl -w debug.nmxaccel_flip=0 > /dev/null; sleep 1
		c0=$(p4_counters); p4_shots p4-flip0 8; c1=$(p4_counters)
		tmo 10 sudo -n sysctl -w debug.nmxaccel_flip=1 > /dev/null
		say "PHASE4 p4 with flips off (frames copied): shots $P4_SHOTS | counters $(p4_delta "$c0" "$c1") | flip back to $(p4_k debug.nmxaccel_flip)"
		printf '%-5s | %-48s | %-70s | %s | %s\n' "p4/f0" "-" "-" "$P4_SHOTS" "-" >> "$O/phase4-summary.txt"
	fi
}
phase_phase4() {
	local ws0 ws1 m
	[ -x "$P4CHECK" ] || { say "PHASE4: no p4check at $P4CHECK, skipped"; return 0; }
	[ "$(stat -f %Su /dev/console)" = "$(id -un)" ] || { say "PHASE4: $(id -un) is not logged in at the console (login window?), skipped"; return 0; }
	if [ -x "$INTELXE_HOME/wsitest/gpu_busy.sh" ] && b=$("$INTELXE_HOME/wsitest/gpu_busy.sh"); then say "PHASE4: GPU busy ($b), skipped"; return 0; fi
	tmo 10 sudo -n true 2>/dev/null && p4_root=1
	p4_find_wslog
	ws0=$(pgrep -x WindowServer)
	say "PHASE4 start: WindowServer $ws0, plugin log ${p4_wslog:-NOT FOUND (no GPU time)}, sudo $p4_root, surfvram $(p4_k debug.intelxe.surfvram), $(p4_counters)"
	[ -n "$p4_wslog" ] && p4_wslog_since 0 | grep -a "scan-out: bound composites\|phase 4: surface\|item 7 (B580)" | tail -4 | sed 's/^/  /' | tee -a "$O/say.txt"
	[ -n "$p4_wslog" ] && ! p4_wslog_since 0 | grep -aq "scan-out: bound composites" && say "PHASE4: WARNING: WindowServer's plugin has no 'scan-out: bound composites' line - the phase 4 build is probably not the one installed"
	printf '%-5s | %-48s | %-70s | %s | %s\n' mode "WindowServer GPU (GPUTIME)" "smoothness (p4check smooth)" "screenshots" "recording (patch colour per frame)" > "$O/phase4-summary.txt"
	for m in $PHASE4_MODES; do p4_mode "$m"; done
	p4_flag p4   # back to the default
	[ $p4_root = 1 ] && tmo 10 sudo -n rm -f /tmp/nvmtl-gputime
	ws1=$(pgrep -x WindowServer)
	[ -n "$p4_wslog" ] && p4_wslog_since 0 | grep -a "phase 4 summary\|phase 4: surface\|mark refused" | tail -6 > "$O/phase4-plugin-lines.txt"
	say "PHASE4 end: WindowServer $ws0 -> $ws1$( [ "$ws0" != "$ws1" ] && echo ' (RESTARTED)'), $(p4_counters)"
	cat "$O/phase4-summary.txt"
}
# run on its own: bash night-phase4.sh
if [ "${BASH_SOURCE[0]}" = "$0" ]; then set -o pipefail; phase_phase4; fi
