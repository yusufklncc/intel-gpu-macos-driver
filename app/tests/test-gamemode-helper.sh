#!/bin/bash
# SPDX-License-Identifier: MIT
# test-gamemode-helper.sh: app/Resources/gamemode-helper.sh (game mode's helper) against a throwaway root, without root,
# with sysctl and tmutil stubbed: the lowest GPU clock raised while a game is in front and put back after it, when the
# app ends, on "quit" and on a signal; a helper killed with -9 and --restore; the clamp to the user's highest clock;
# a floor that is not above the current one; Time Machine stopped only in a game and only when asked; one helper at a
# time; bad arguments; the pipe's permissions. Nothing outside the throwaway root is touched.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
H=$HERE/../Resources/gamemode-helper.sh
ROOT=$(mktemp -d -t ixegm)
APPS=""
trap 'for p in $APPS; do kill "$p" 2>/dev/null; done; rm -rf -- "$ROOT"' EXIT
export GM_ROOT=$ROOT
fails=0
pass() { echo "ok    $*"; }
fail() { echo "FAIL  $*"; fails=$((fails + 1)); }
check_eq() { if [ "$2" = "$3" ]; then pass "$1: $2"; else fail "$1: '$2', expected '$3'"; fi; }

mkdir -p "$ROOT/stub" "$ROOT/sysctl"
# sysctl: values in $ROOT/sysctl/<name>; every write is also appended to $ROOT/sysctl.log
cat > "$ROOT/stub/sysctl" <<'S'
#!/bin/bash
d=$(dirname "$0")/../sysctl
case "$1" in
    -n) [ -f "$d/$2" ] && cat "$d/$2" || { echo "sysctl: unknown oid '$2'" >&2; exit 1; };;
    -w) [ -f "$d/${2%%=*}" ] || { echo "sysctl: unknown oid '${2%%=*}'" >&2; exit 1; }; printf '%s\n' "${2#*=}" > "$d/${2%%=*}"; echo "$2" >> "$d/../sysctl.log";;
esac
S
# tmutil: "Running = 1" while $ROOT/tm-running exists; stopbackup removes it and counts
cat > "$ROOT/stub/tmutil" <<'S'
#!/bin/bash
d=$(dirname "$0")/..
case "$1" in
    status) [ -f "$d/tm-running" ] && echo '    Running = 1;' || echo '    Running = 0;';;
    stopbackup) rm -f "$d/tm-running"; echo stop >> "$d/tm-stops";;
esac
S
chmod 755 "$ROOT/stub/"*
setsys() { printf '%s\n' "$2" > "$ROOT/sysctl/$1"; }
getsys() { cat "$ROOT/sysctl/$1"; }
D=$ROOT/var/run/intelxe-gamemode
newapp() { sleep 120 & APP=$!; disown "$APP"; APPS="$APPS $APP"; }
start() { # start FLOOR TM: a helper for a new app process, waits for its pipe
    newapp
    "$H" --run "$APP" "$(id -u)" "$1" "$2" > "$ROOT/helper.log" 2>&1 &
    HP=$!; disown "$HP"
    # its own pipe, not one a killed helper left: the state names this helper
    local i=0; while ! { [ -p "$D/ctl" ] && grep -q "^pid=$HP\$" "$D/state" 2>/dev/null; } && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
    [ -p "$D/ctl" ] || { fail "no pipe after start"; cat "$ROOT/helper.log"; }
}
say() { echo "$1" > "$D/ctl"; sleep 0.5; }
gone() { local i=0; while kill -0 "$HP" 2>/dev/null && [ $i -lt 60 ]; do sleep 0.1; i=$((i + 1)); done; ! kill -0 "$HP" 2>/dev/null; }

setsys debug.intelxe.slpc_min 1200; setsys debug.intelxe.slpc_max 0; setsys debug.intelxe.step 0

# --- bad arguments and no driver
newapp
"$H" --run "$APP" "$(id -u)" 9000 0 > /dev/null 2>&1; check_eq "a clock above 3000 MHz is refused" $? 2
"$H" --run "$APP" "$(id -u)" 2400 yes > /dev/null 2>&1; check_eq "STOP_BACKUPS other than 0/1 is refused" $? 2
"$H" --run 999999 "$(id -u)" 2400 0 > /dev/null 2>&1; check_eq "a process that does not exist is refused" $? 2
"$H" --run "$APP" root 2400 0 > /dev/null 2>&1; check_eq "a user that is not a number is refused" $? 2
mv "$ROOT/sysctl/debug.intelxe.slpc_min" "$ROOT/sysctl/off"
"$H" --run "$APP" "$(id -u)" 2400 0 > /dev/null 2>&1; check_eq "without the driver's sysctl it does not start" $? 3
mv "$ROOT/sysctl/off" "$ROOT/sysctl/debug.intelxe.slpc_min"
env -u GM_ROOT "$H" --restore > "$ROOT/real.log" 2>&1
grep -q "needs root" "$ROOT/real.log" && pass "without root and outside the test root it refuses" || fail "--restore outside the test root: $(cat "$ROOT/real.log")"

# --- a game in front, then the desktop
start 2400 0
check_eq "the pipe is the user's alone" "$(stat -f %Lp "$D/ctl")" 600
check_eq "started: the clock is untouched" "$(getsys debug.intelxe.slpc_min)" 1200
say game
check_eq "a game: lowest clock" "$(getsys debug.intelxe.slpc_min)" 2400
check_eq "a game: sent to the GuC (step 17)" "$(getsys debug.intelxe.step)" 17
check_eq "--status in a game" "$("$H" --status | sed -n 's/^phase=//p')" game
say game
check_eq "a second 'game' writes nothing" "$(grep -c slpc_min=2400 "$ROOT/sysctl.log")" 1
say desktop
check_eq "the desktop: lowest clock back" "$(getsys debug.intelxe.slpc_min)" 1200
say nonsense
check_eq "an unknown word changes nothing" "$(getsys debug.intelxe.slpc_min)" 1200
# --- one helper at a time
"$H" --run "$APP" "$(id -u)" 2400 0 > "$ROOT/second.log" 2>&1; check_eq "a second helper is refused" $? 1
# --- quit in a game
say game
say quit
gone && pass "quit: the helper ended" || fail "quit: still running"
check_eq "quit in a game: lowest clock back" "$(getsys debug.intelxe.slpc_min)" 1200
[ -e "$D/ctl" ] || [ -e "$D/state" ] && fail "quit: the pipe or state is left" || pass "quit: pipe and state removed"
check_eq "--status after quit" "$("$H" --status)" running=no

# --- the app ends while a game is in front
start 2400 0
say game
kill "$APP"
gone && pass "app ended: the helper ended" || fail "app ended: still running"
check_eq "app ended in a game: lowest clock back" "$(getsys debug.intelxe.slpc_min)" 1200

# --- a signal
start 2400 0
say game
kill -TERM "$HP"
gone && pass "TERM: the helper ended" || fail "TERM: still running"
check_eq "TERM in a game: lowest clock back" "$(getsys debug.intelxe.slpc_min)" 1200

# --- kill -9: the clock stays raised until --restore (or the next start, or the next helper)
start 2400 0
say game
kill -9 "$HP"; sleep 0.3
check_eq "kill -9: the clock stays raised" "$(getsys debug.intelxe.slpc_min)" 2400
check_eq "--status after kill -9" "$("$H" --status | sed -n 's/^running=//p')" no
"$H" --restore > /dev/null 2>&1
check_eq "--restore: lowest clock back" "$(getsys debug.intelxe.slpc_min)" 1200
[ -e "$D/state" ] && fail "--restore left the state" || pass "--restore removed the state"
start 2400 0; say game; kill -9 "$HP"; sleep 0.3
start 2000 0
check_eq "a new helper puts back what a killed one left" "$(sed -n 's/^orig_min=//p' "$D/state")" 1200
say quit; gone

# --- the user's highest clock (Quiet: 1800) caps the floor; a floor not above the current one does nothing
setsys debug.intelxe.slpc_max 1800
start 2400 0; say game
check_eq "capped at the highest clock" "$(getsys debug.intelxe.slpc_min)" 1800
say quit; gone
setsys debug.intelxe.slpc_max 0; setsys debug.intelxe.slpc_min 2400; : > "$ROOT/sysctl.log"
start 2000 0; say game; say desktop
check_eq "a floor below the current minimum writes nothing" "$(grep -c . "$ROOT/sysctl.log")" 0
say quit; gone
setsys debug.intelxe.slpc_min 1200

# --- Time Machine: stopped only in a game, only when asked
: > "$ROOT/tm-running"
start 2400 0; say game; sleep 2.5
[ -e "$ROOT/tm-stops" ] && fail "a backup was stopped without STOP_BACKUPS" || pass "STOP_BACKUPS=0: backups run"
say quit; gone
start 2400 1; sleep 2.5
[ -e "$ROOT/tm-stops" ] && fail "a backup was stopped on the desktop" || pass "STOP_BACKUPS=1, no game: backups run"
say game
check_eq "STOP_BACKUPS=1 in a game: the backup stopped" "$(grep -c . "$ROOT/tm-stops" 2>/dev/null)" 1
: > "$ROOT/tm-running"; sleep 10.5
check_eq "STOP_BACKUPS=1: a backup that starts during the game is stopped too" "$(grep -c . "$ROOT/tm-stops" 2>/dev/null)" 2
say quit; gone

echo "== $fails failure(s)"
exit $fails
