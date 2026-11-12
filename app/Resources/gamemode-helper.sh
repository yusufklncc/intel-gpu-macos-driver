#!/bin/bash
# SPDX-License-Identifier: MIT
# gamemode-helper.sh: IntelXe Manager's game mode, for one session of the app (docs/design/DESIGN-game-mode.md).
#
#   sudo gamemode-helper.sh --run APP_PID OWNER_UID FLOOR_MHZ STOP_BACKUPS
#                          the app starts it once, in the background, when game mode is turned on. It reads one word
#                          per line from a pipe only OWNER_UID can write: "game" raises the GPU's lowest clock to
#                          FLOOR_MHZ (debug.intelxe.slpc_min, sent to the GuC with debug.intelxe.step=17) and, with
#                          STOP_BACKUPS=1, stops Time Machine backups that run while the game stays in front;
#                          "desktop" puts the lowest clock back as it was; "quit" ends it. It ends too when APP_PID
#                          ends or on a signal, and puts the clock back first. Nothing is written to disk but its pipe
#                          and state in /var/run/intelxe-gamemode (gone at the next start of macOS, as the clock is).
#   gamemode-helper.sh --status    the state a helper holds, "key=value" (no root)
#   sudo gamemode-helper.sh --restore
#                          put back the lowest clock a helper that was killed left raised (from its state)
set -uo pipefail

# Tests (app/tests/test-gamemode-helper.sh) run it as a normal user against a throwaway root, with sysctl and tmutil
# stubbed: GM_ROOT is honoured only then, never when the script runs as root.
R="" STUB=""
if [ -n "${GM_ROOT:-}" ] && [ "$(id -u)" != 0 ]; then R=$GM_ROOT; STUB=$R/stub/; fi
SYSCTL=${STUB:-/usr/sbin/}sysctl
TMUTIL=${STUB:-/usr/bin/}tmutil
D="$R/var/run/intelxe-gamemode"
CTL="$D/ctl" STATE="$D/state"

say() { printf '%s gamemode: %s\n' "$(date '+%H:%M:%S')" "$*"; }
need_root() { [ -n "$R" ] || [ "$(id -u)" = 0 ] || { echo "$1 needs root"; exit 1; }; }
kv() { sed -n "s/^$1=//p" "$STATE" 2>/dev/null | head -1; }
num() { case "$1" in ''|*[!0-9]*) return 1;; esac; }
alive() { num "$1" && kill -0 "$1" 2>/dev/null; }

# set_min MHZ: the GuC's lowest GT clock, now (until the next start of macOS or the next change)
set_min() {
    "$SYSCTL" -w "debug.intelxe.slpc_min=$1" >/dev/null && "$SYSCTL" -w debug.intelxe.step=17 >/dev/null
}

write_state() {
    printf 'pid=%s\napp=%s\norig_min=%s\nfloor=%s\nphase=%s\nstop_backups=%s\n' "$$" "$APP" "$ORIG" "$FLOOR" "$PHASE" "$TM" > "$STATE.new" \
        && chmod 644 "$STATE.new" && mv -f "$STATE.new" "$STATE"
}

# a helper that was killed (kill -9, a crash) cannot put the clock back itself: its state still says "game"
restore_stale() {
    local pid orig
    [ -f "$STATE" ] || return 0
    pid=$(kv pid); orig=$(kv orig_min)
    alive "$pid" && return 0
    if [ "$(kv phase)" = game ] && num "$orig"; then
        set_min "$orig" && say "put back the lowest GPU clock a stopped helper left raised: $orig MHz"
    fi
    rm -f "$STATE" "$CTL"
}

tm_stop() {
    "$TMUTIL" status 2>/dev/null | grep -q 'Running = 1' || return 0
    "$TMUTIL" stopbackup >/dev/null 2>&1 && say "stopped a Time Machine backup (the next scheduled one runs as usual)"
}

game() {
    [ "$PHASE" = game ] && return 0
    PHASE=game; write_state
    if [ "$FLOOR" -gt "$ORIG" ]; then set_min "$FLOOR" && say "a game is in front: lowest GPU clock $FLOOR MHz (was $ORIG)"
    else say "a game is in front: the lowest GPU clock is already $ORIG MHz"; fi
    [ "$TM" = 1 ] && tm_stop
}

desktop() {
    [ "$PHASE" = game ] || return 0
    PHASE=desktop; write_state
    [ "$FLOOR" -gt "$ORIG" ] && set_min "$ORIG" && say "no game in front: lowest GPU clock back to $ORIG MHz"
}

cleanup() {
    desktop
    rm -f "$CTL" "$STATE"
    say "ended: everything is as it was"
}

run() {
    need_root --run
    APP=${1:-} OWNER=${2:-} FLOOR=${3:-} TM=${4:-0} PHASE=desktop
    alive "$APP" || { echo "no such app process: $APP"; exit 2; }
    num "$OWNER" || { echo "not a user id: $OWNER"; exit 2; }
    num "$FLOOR" && [ "$FLOOR" -ge 300 ] && [ "$FLOOR" -le 3000 ] || { echo "the clock must be 300-3000 MHz: $FLOOR"; exit 2; }
    case "$TM" in 0|1) ;; *) echo "STOP_BACKUPS is 0 or 1: $TM"; exit 2;; esac
    mkdir -p "$D"; chmod 755 "$D"
    if [ -f "$STATE" ] && alive "$(kv pid)"; then echo "game mode already runs (pid $(kv pid))"; exit 1; fi
    restore_stale
    ORIG=$("$SYSCTL" -n debug.intelxe.slpc_min 2>/dev/null)
    num "$ORIG" || { echo "IntelXe.kext does not answer (debug.intelxe.slpc_min): game mode needs the driver"; exit 3; }
    # never above the highest clock the user chose (the Quiet setting's 1800 MHz, intelxe-gpu-max-mhz)
    local max; max=$("$SYSCTL" -n debug.intelxe.slpc_max 2>/dev/null)
    if num "$max" && [ "$max" -gt 0 ] && [ "$FLOOR" -gt "$max" ]; then FLOOR=$max; fi
    rm -f "$CTL"; mkfifo -m 600 "$CTL" || { echo "could not make $CTL"; exit 1; }
    [ -n "$R" ] || chown "$OWNER" "$CTL"
    write_state
    trap cleanup EXIT
    trap 'exit 0' TERM INT
    trap '' HUP
    exec 3<>"$CTL"
    say "on for app $APP: lowest GPU clock $FLOOR MHz while a game is in front (now $ORIG MHz)$([ "$TM" = 1 ] && echo ", Time Machine backups stopped then")"
    local line n=0
    while alive "$APP"; do
        if read -r -t 2 -u 3 line; then
            case "$line" in
                game) game;;
                desktop) desktop;;
                quit) break;;
            esac
        fi
        n=$((n + 1))
        [ "$PHASE" = game ] && [ "$TM" = 1 ] && [ $((n % 5)) = 1 ] && tm_stop
    done
    exit 0
}

case "${1:-}" in
    --run) shift; run "$@" ;;
    --status)
        if [ -f "$STATE" ]; then cat "$STATE"; alive "$(kv pid)" && echo "running=yes" || echo "running=no"; else echo "running=no"; fi ;;
    --restore) need_root --restore; restore_stale ;;
    *) sed -n '3,15p' "$0"; exit 2 ;;
esac
