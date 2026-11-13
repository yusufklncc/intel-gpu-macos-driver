# SPDX-License-Identifier: MIT
# night phase barrevoke (sourced by the night scripts, or run with bash on its own: tmo, say and $O are defined here when
# the night driver has not). Needs the barwin kext (debug.intelxe.bar_revoke) and user/tests/kernel/barrevoke built next
# to this script (or BARREVOKE_BIN). docs/design/DESIGN-rebar-window.md section 8: CPU-mapped BOs leave the BAR window
# with their mappings redirected to system memory.
#  1. forced, knob 0: barrevoke now 4 (sudo: writes debug.intelxe.bar_revoke_now) -> $O/barrevoke-now4.log
#  2. knob 1: barrevoke natural 4 (GEM_CREATE's eviction revokes the oldest) -> $O/barrevoke-natural4.log
#  3. knob 1: barrevoke now 16 (the size limit's edge) -> $O/barrevoke-now16.log
#  4. checks after each: WindowServer pid unchanged, no new GuC / GT resets, no CAT error; the bar_revoke line of
#     debug.intelxe.vram before and after -> $O/barrevoke-vram.txt; on a latched or broken result the g2h log is saved
# The knob goes back to 0 at the end. Every binary runs under tmo (a bash timer: no perl / env / sh wrapper).
# Do not run while a game or benchmark runs: the test takes the whole window for a few seconds.
BARREVOKE_DIR=${BARREVOKE_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
BARREVOKE_BIN=${BARREVOKE_BIN:-$BARREVOKE_DIR/barrevoke}
O=${O:-$HOME/b580/barrevoke/$(date +%m%d-%H%M)}
mkdir -p "$O"
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
declare -F resets >/dev/null || resets() {
	sysctl -n debug.intelxe.stats 2>/dev/null | sed -nE 's/.*GuC context resets ([0-9]+); GT resets ([0-9]+).*/\1 \2/p'
}
barrevoke_line() { sysctl -n debug.intelxe.vram 2>/dev/null | grep -A1 '^bar_revoke '; }
barrevoke_set() { sudo -n sysctl debug.intelxe.bar_revoke="$1" >/dev/null 2>&1; }
barrevoke_run() {   # tag args...
	local tag=$1 ws0 ws1 r0 r1 c0 c1 rc; shift
	ws0=$(pgrep -x WindowServer); r0=$(resets)
	c0=$(sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'CAT errors [0-9]+' | head -1)
	{ echo "== $tag before (bar_revoke $(sysctl -n debug.intelxe.bar_revoke))"; barrevoke_line; } >> "$O/barrevoke-vram.txt"
	if [ "${1:-}" = now ]; then tmo 120 sudo -n "$BARREVOKE_BIN" "$@" > "$O/barrevoke-$tag.log" 2>&1
	else tmo 120 "$BARREVOKE_BIN" "$@" > "$O/barrevoke-$tag.log" 2>&1; fi
	rc=$?
	{ echo "== $tag after (exit $rc)"; barrevoke_line; } >> "$O/barrevoke-vram.txt"
	ws1=$(pgrep -x WindowServer); r1=$(resets)
	c1=$(sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'CAT errors [0-9]+' | head -1)
	local ok=PASS
	grep -q '^PASSED' "$O/barrevoke-$tag.log" || ok=FAIL
	grep -q '^SKIP' "$O/barrevoke-$tag.log" && ok=SKIP
	[ "$ws0" = "$ws1" ] && [ "$r0" = "$r1" ] && [ "$c0" = "$c1" ] || ok="$ok+SYSTEM"
	say "BARREVOKE $tag $ok (exit $rc): $(grep -E '^(revoked|FAIL|SKIP|PASSED|FAILED)' "$O/barrevoke-$tag.log" | head -4 | tr '\n' ' ')" \
		"| WindowServer $ws0->$ws1, resets $r0->$r1, $c0->$c1"
	if sysctl -n debug.intelxe.vram | grep -qE 'LATCHED OFF|broken [1-9]|undecided [1-9]'; then
		sudo -n sysctl -n debug.intelxe.g2h 2>/dev/null | tail -80 > "$O/g2h-barrevoke-$tag.txt"
		say "BARREVOKE $tag: latched / broken, g2h saved"
	fi
}
phase_barrevoke() {
	if ! sysctl -n debug.intelxe.bar_revoke >/dev/null 2>&1; then say "BARREVOKE: kext without debug.intelxe.bar_revoke, skipped"; return 0; fi
	[ -x "$BARREVOKE_BIN" ] || { say "BARREVOKE: no binary at $BARREVOKE_BIN, skipped"; return 0; }
	if [ -x "$HOME/b580/wsitest/gpu_busy.sh" ] && b=$("$HOME/b580/wsitest/gpu_busy.sh"); then say "BARREVOKE: GPU busy ($b), skipped"; return 0; fi
	say "BARREVOKE start: window $(sysctl -n debug.intelxe.vram | head -1 | cut -c1-120); resets $(resets)"
	barrevoke_set 0
	barrevoke_run now4 now 4
	barrevoke_set 1
	barrevoke_run natural4 natural 4
	barrevoke_run now16 now 16
	barrevoke_set 0
	say "BARREVOKE end: bar_revoke $(sysctl -n debug.intelxe.bar_revoke); $(barrevoke_line | head -1 | cut -c1-200)"
}
# run on its own: bash night-barrevoke.sh
if [ "${BASH_SOURCE[0]}" = "$0" ]; then set -o pipefail; phase_barrevoke; fi
