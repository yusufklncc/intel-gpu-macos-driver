#!/bin/bash
# SPDX-License-Identifier: MIT
# zinkgap-kit.sh: measure zink's GPU work per frame on the B580 and A/B every knob of docs/design/DESIGN-zink-gpu-gap.md.
# Runs ON the B580 machine, from the staged kit (zink-gl/bench/stage.sh OUT, copied there as $ZG_KIT, default
# ~/b580/zinkgap): OUT/lib holds the zink build under test, so nothing installed is used or changed; Minecraft's knobs
# go through /tmp/mesa-options-java (command "mc"). Every program runs under tmo, a bash timer, never perl/env/sh, so
# DYLD_* survive where they are needed (the cgl step); env knobs are exported in a subshell.
# NVMTL_PROCESS_ENV=1 is exported for every run: the release Metal plugin, which a window (and Apple's GL) loads into
# the process, otherwise unsets every inherited INTEL_*, MESA_*, ANV_* variable before ANV reads it (nvmtl_env_knobs.h):
# on 13 Nov that silently dropped INTEL_DEBUG (diag counters all zero, ref-* configs not applied) and INTEL_MEASURE.
# A stage with lib/anv (stage.sh ZG_ANV=..., e.g. ANV with mesa/patches/0108) runs zink on that ANV through
# VK_DRIVER_FILES; ZG_ANV=installed uses the installed one anyway. `check` says which.
#   zinkgap-kit.sh check            renderer, stage hashes, one short run in each mode (<= 60 s)
#   zinkgap-kit.sh ab1 CONFIG [WORKLOAD]   one A/B run (<= 60 s), appended to the results
#   zinkgap-kit.sh repeat CONFIG WORKLOAD [N]   N such runs (image hash determinism)
#   zinkgap-kit.sh ab [ROUNDS]      the whole matrix, ROUNDS times interleaved (night: ~5 min per round)
#   zinkgap-kit.sh diag [CONFIG]    where the GPU time goes: INTEL_DEBUG=perf warnings, INTEL_DEBUG=pc PIPE_CONTROLs and
#                                   stalls per frame, INTEL_MEASURE=draw per event type with GPU idle time (<= 60 s each)
#   zinkgap-kit.sh cgl              libcglzink's IOSurface copy-back: Apple's GL, then zink with CGLZINK_SURF_WRITEBACK
#                                   unset (= dirty, the default since 13 Nov)/flush/present/flush,present; layertest
#   zinkgap-kit.sh mc CONFIG|off    Minecraft: append CONFIG's knobs to /tmp/mesa-options-java (after zink_ab.sh arm)
#   zinkgap-kit.sh table            medians per config and workload, delta against base, image hash check
#   zinkgap-kit.sh night            check, diag base/rtfix/rtoff, ab 2, repeat persist/orphan, table, cgl, expected
#   zinkgap-kit.sh expected         the 13 Nov results and what a run of this kit should show against them
#   zinkgap-kit.sh configs          the configurations and workloads
# Environment: ZG_KIT (kit dir), ZG_TAG (run name, default today), ZG_MODE window|pbuffer (default window),
#   ZG_SECONDS (per run, default 8), ZG_SIZE (default "-W 1920 -H 1080").
set -uo pipefail
export LC_ALL=C PATH=/usr/bin:/bin:/usr/sbin:/sbin
K=${ZG_KIT:-$HOME/b580/zinkgap}
TAG=${ZG_TAG:-$(date +%Y%m%d)}
case "$TAG" in *[!0-9a-zA-Z_-]*|'') echo "bad ZG_TAG"; exit 2;; esac
R=$K/run-$TAG; mkdir -p "$R"
MODE=${ZG_MODE:-window}
SECS=${ZG_SECONDS:-8}
SIZE=${ZG_SIZE:--W 1920 -H 1080}
BIN=$K/bin; LIB=$K/lib
TSV=$R/results.tsv
export NVMTL_PROCESS_ENV=1
if [ -f "$LIB/anv/intel_icd.json" ] && [ "${ZG_ANV:-staged}" != installed ]; then
    export VK_DRIVER_FILES=$LIB/anv/intel_icd.json; ANV_USED="staged lib/anv"
else ANV_USED="installed"; fi

# tmo SECONDS CMD...: CMD in the background, TERM after SECONDS, KILL 2 s later; returns CMD's status (124 on timeout)
tmo() {
    local t=$1; shift
    "$@" & local p=$!
    ( sleep "$t"; kill -TERM "$p" 2>/dev/null && { sleep 2; kill -KILL "$p" 2>/dev/null; } ) & local w=$!
    wait "$p"; local r=$?
    kill "$w" 2>/dev/null; wait "$w" 2>/dev/null
    [ $r -eq 143 ] && r=124
    return $r
}
health() { echo "gt_reset=$(sysctl -n debug.intelxe.gt_reset 2>/dev/null) $(sysctl -n debug.intelxe.stats 2>/dev/null | grep -E '^jobs:|^TDR:' | tr '\n' ' ' | cut -c1-200)"; }

# ---- configurations: name -> knobs (KEY=VALUE, space separated). base has none. ref-* are references, not fixes.
cfg_env() {
    case "$1" in
    base)      echo "" ;;
    lazy)      echo "ZINK_DESCRIPTORS=lazy" ;;                     # classic descriptor sets: binding tables + prefetch
    rp)        echo "ZINK_DEBUG=rp" ;;                             # zink's renderpass tracking (loads/stores, reorder)
    barsys)    echo "ZINK_BAR_BUFFERS=sysmem" ;;                   # 0107: host-memory buffers mapped, no staging copy
    barall)    echo "ZINK_BAR_BUFFERS=all" ;;                      # 0107: every buffer mapped (as resizable BAR)
    rtfix)     echo "anv_state_cache_perf_fix=true" ;;             # ANV: no RT-BTI-change flush at each renderpass/blorp
    rtoff)     echo "anv_state_cache_perf_fix=false" ;;            # the opt-out (0108 makes rtfix zink's default)
    combo)     echo "ZINK_BAR_BUFFERS=sysmem anv_state_cache_perf_fix=true" ;;
    ref-nofc)  echo "INTEL_DEBUG=nofc" ;;                          # what fast clears give
    ref-noccs) echo "INTEL_DEBUG=noccs" ;;                         # what compression gives
    ref-nohiz) echo "INTEL_DEBUG=nohiz" ;;                         # what HiZ gives
    ref-norb)  echo "INTEL_DEBUG=no-resource-barrier" ;;           # how much Xe2 RESOURCE_BARRIER is used now
    *) return 1 ;;
    esac
}
CONFIGS="base lazy rp barsys barall rtfix rtoff combo ref-nofc ref-noccs ref-nohiz ref-norb"
wl_args() {
    case "$1" in
    def)     echo "-p 8 -m 1 -u subdata -U 2048" ;;   # shader-pack-like: 8 composite passes, a mip chain, 2 MiB/frame
    nomip)   echo "-p 8 -m 0 -u none" ;;              # passes and barriers only
    mapinv)  echo "-p 4 -m 0 -u mapinv -U 4096" ;;    # glMapBufferRange(INVALIDATE_RANGE) streaming
    orphan)  echo "-p 4 -m 0 -u orphan -U 1024" ;;    # glBufferData orphaning each frame
    persist) echo "-p 4 -m 0 -u persist -U 4096" ;;   # persistent staging + glCopyBufferSubData (Sodium-like)
    *) return 1 ;;
    esac
}
# the matrix: workload -> configs
matrix() {
    echo "def $CONFIGS"
    echo "nomip base lazy rp rtfix rtoff ref-norb"
    echo "mapinv base barsys barall"
    echo "orphan base barsys barall"
    echo "persist base barsys barall"
}

# run CONFIG WORKLOAD [extra zgbench args]: one zgbench run with the knobs exported; appends a TSV row
run() {
    local cfg=$1 wl=$2; shift 2
    local envs args out err rc line
    envs=$(cfg_env "$cfg") || { echo "unknown config $cfg"; return 2; }
    args=$(wl_args "$wl") || { echo "unknown workload $wl"; return 2; }
    out=$R/$cfg-$wl-$(date +%H%M%S).txt; err=${out%.txt}.err
    (
        for kv in $envs; do export "$kv"; done
        export ZGB_GLFW=$LIB/libglfw.3.dylib ZGB_EGL=$LIB/libEGL.1.dylib
        local m=""; [ "$MODE" = pbuffer ] && m=-e
        # shellcheck disable=SC2086
        tmo $((SECS + 25)) "$BIN/zgbench" $m $SIZE -s "$SECS" -q 1 $args "$@" > "$out" 2> "$err"
    )
    rc=$?
    line=$(grep '^zgbench: fps' "$out" | tail -1)
    local fps gpu img
    fps=$(echo "$line" | sed -n 's/.*fps \([0-9.]*\).*/\1/p'); gpu=$(echo "$line" | sed -n 's/.* gpu \([0-9.]*\) ms.*/\1/p')
    img=$(echo "$line" | sed -n 's/.*image \([0-9a-f]*\).*/\1/p')
    printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$(date +%H:%M:%S)" "$cfg" "$wl" "$MODE" "${fps:-NA}" "${gpu:-NA}" "${img:-NA}" "$rc" >> "$TSV"
    printf '%-10s %-8s fps %-8s gpu %-7s ms image %s rc %s\n' "$cfg" "$wl" "${fps:-NA}" "${gpu:-NA}" "${img:-NA}" "$rc"
    [ $rc -ne 0 ] && tail -3 "$err"
    return 0
}

table() {
    [ -s "$TSV" ] || { echo "no results in $TSV"; return 1; }
    awk -F'\t' '
    function median(s,   n, a, i, j, t) { n = split(s, a, " "); for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (a[j] + 0 < a[i] + 0) { t = a[i]; a[i] = a[j]; a[j] = t }
        return n ? (n % 2 ? a[(n + 1) / 2] : (a[n / 2] + a[n / 2 + 1]) / 2) : "NA" }
    { k = $3 SUBSEP $2; if (!($3 in seen)) { seen[$3] = 1; wls[++nw] = $3 } if (!($2 in cs)) { cs[$2] = 1; cfgs[++nc] = $2 }
      if ($5 == "NA" || $8 != 0) { failed[k]++; any[k] = 1; next }
      any[k] = 1; fps[k] = fps[k] " " $5; gpu[k] = gpu[k] " " $6; n[k]++; if (!(k in img)) img[k] = $7; else if (img[k] != $7) img[k] = "DIFFERS" }
    END {
        printf "%-8s %-10s %4s %9s %9s %8s %8s  %s\n", "workload", "config", "n", "fps", "gpu ms", "fps %", "gpu %", "image (failed runs)"
        for (w = 1; w <= nw; w++) { bk = wls[w] SUBSEP "base"; bf = median(fps[bk]); bg = median(gpu[bk])
            for (c = 1; c <= nc; c++) { k = wls[w] SUBSEP cfgs[c]; if (!(k in any)) continue
                if (!(k in n)) { printf "%-8s %-10s %4d %9s %9s %8s %8s  FAILED %d\n", wls[w], cfgs[c], 0, "-", "-", "-", "-", failed[k]; continue }
                f = median(fps[k]); g = median(gpu[k])
                df = (bf + 0 > 0) ? sprintf("%+.1f", (f - bf) * 100 / bf) : "-"; dg = (bg + 0 > 0 && g != "") ? sprintf("%+.1f", (g - bg) * 100 / bg) : "-"
                # VARIES: this config gave more than one image; base VARIES: base did (so "same" means nothing)
                same = (img[k] == "DIFFERS") ? "VARIES" : (!(bk in img) ? "no base" : (img[bk] == "DIFFERS" ? "base VARIES" : (img[k] == img[bk] ? "same" : "DIFFERENT")))
                printf "%-8s %-10s %4d %9s %9s %8s %8s  %s%s\n", wls[w], cfgs[c], n[k], f, g, df, dg, same, (k in failed) ? " (" failed[k] " failed)" : "" } } }' "$TSV"
}

# diag CONFIG: three short runs of the default workload with ANV's own instruments. INTEL_DEBUG=pc and =perf print
# through mesa_log (stderr, "MESA: info: pc : emit PC=( +rt_flush ... ) reason: ..."); MESA_LOG/MESA_LOG_LEVEL are set
# so that an inherited setting cannot send them elsewhere. NVMTL_PROCESS_ENV=1 (exported above) keeps the Metal plugin
# from unsetting INTEL_* first. A run whose instrument printed nothing says so instead of counting zeros.
diag() {
    local cfg=${1:-base} envs
    envs=$(cfg_env "$cfg") || { echo "unknown config $cfg"; return 2; }
    local d=$R/diag-$cfg; mkdir -p "$d"
    local m=""; [ "$MODE" = pbuffer ] && m=-e
    (
        for kv in $envs; do export "$kv"; done
        export ZGB_GLFW=$LIB/libglfw.3.dylib ZGB_EGL=$LIB/libEGL.1.dylib MESA_LOG=file MESA_LOG_LEVEL=info
        unset MESA_LOG_FILE
        # 1. perf warnings (slow clears, disabled aux, resolves), counted by text
        INTEL_DEBUG=perf${INTEL_DEBUG:+,$INTEL_DEBUG} tmo 40 "$BIN/zgbench" $m $SIZE -s 3 -q 0 $(wl_args def) > "$d/perf.txt" 2> "$d/perf.err"
        # 2. every PIPE_CONTROL with its bits and reason (stderr is large: 1 s only)
        INTEL_DEBUG=pc${INTEL_DEBUG:+,$INTEL_DEBUG} tmo 50 "$BIN/zgbench" $m $SIZE -s 1 -q 0 $(wl_args def) > "$d/pc.txt" 2> "$d/pc.err"
        # 3. GPU time and idle time per event (draw, blit, copy, clear, resolve) from GPU timestamps; the file is made
        # at ANV's first print (a gather at a swapchain acquire or a command buffer reset)
        rm -f "$d/measure.csv"
        INTEL_MEASURE=draw,file=$d/measure.csv tmo 40 "$BIN/zgbench" $m $SIZE -s 3 -q 0 $(wl_args def) > "$d/measure.txt" 2> "$d/measure.err"
    )
    echo "== diag $cfg ($MODE, ANV $ANV_USED): $(grep '^zgbench: fps' "$d/perf.txt" | sed 's/.*fps \([0-9.]*\).*/fps \1 under INTEL_DEBUG=perf/')"
    echo "-- perf warnings (count, text)"
    grep -v '^zgbench' "$d/perf.err" | sed -E 's/0x[0-9a-f]+/0x_/g; s/[0-9]+x[0-9]+/WxH/g' | sort | uniq -c | sort -rn | head -12
    local frames npc
    frames=$(sed -n 's/^zgbench: fps .* drawn \([0-9]*\).*/\1/p' "$d/pc.txt"); frames=${frames:-0}
    npc=$(grep -c 'emit PC=' "$d/pc.err")
    echo "-- PIPE_CONTROLs: totals over all $frames frames drawn (warm-up included); per frame in brackets"
    if [ "${npc:-0}" -eq 0 ]; then
        echo "NO PIPE_CONTROL lines: INTEL_DEBUG=pc did not reach ANV or its log went elsewhere ($(wc -l < "$d/pc.err") stderr lines; first:)"
        head -3 "$d/pc.err"
    else
        awk -v f="$frames" '/emit PC=/ { pc++; if (/cs_stall/) cs++; if (/rt-bti-change/) bti++; if (/rt_flush/) rt++
                r = $0; sub(/.*reason: /, "", r); reason[r]++ }
            /rb : Type=/ { rb++ } /Ensure flushes done before invalidate/ { eop++ }
            END { if (f < 1) f = 1
                  printf "PIPE_CONTROL %d [%.1f], with CS stall %d [%.1f], RT flush %d [%.1f], rt-bti-change %d [%.1f], RESOURCE_BARRIER %d [%.1f], end-of-pipe syncs added %d [%.1f]\n", pc, pc / f, cs, cs / f, rt, rt / f, bti, bti / f, rb, rb / f, eop, eop / f; fflush()
                  for (k in reason) printf "%8d [%6.1f]  %s\n", reason[k], reason[k] / f, k | "sort -rn | head -16" }' "$d/pc.err"
    fi
    echo "-- INTEL_MEASURE=draw: per frame, by event type (GPU us busy, GPU us idle before the event)"
    if [ -s "$d/measure.csv" ]; then
        local mframes
        mframes=$(sed -n 's/^zgbench: fps .* drawn \([0-9]*\).*/\1/p' "$d/measure.txt")
        # frames counted by ANV at each swapchain acquire; a pbuffer run has none, so the drawn frames divide
        awk -F, -v drawn="${mframes:-0}" 'NR > 1 && $9 != "" { t = $9; n[t]++; busy[t] += $20; idle[t] += $19; fr[$3] = 1 }
            END { nf = 0; for (k in fr) nf++; if (nf <= 1 && drawn > 0) nf = drawn; if (!nf) nf = 1; printf "frames %d\n", nf
                  for (t in n) printf "%-24s %7.1f events %9.1f us busy %9.1f us idle\n", t, n[t] / nf, busy[t] / nf, idle[t] / nf | "sort -k2 -rn" }' "$d/measure.csv"
    else
        echo "NO INTEL_MEASURE output ($d/measure.csv missing or empty); its stderr:"
        grep -i 'measure' "$d/measure.err" | head -5
    fi
}

cgl() {
    local d=$R/cgl; mkdir -p "$d"
    echo "== libcglzink IOSurface copy-back: zgbench -a -i -f 3 (final pass into an IOSurface, 3 mid-frame glFlush)"
    tmo $((SECS + 25)) "$BIN/zgbench" -a -i -f 3 $SIZE -s "$SECS" -q 1 -p 4 -m 0 -u none > "$d/apple.txt" 2> "$d/apple.err"
    echo "apple      $(grep '^zgbench: fps' "$d/apple.txt" | cut -c10-)"
    local wb
    # default: CGLZINK_SURF_WRITEBACK unset, which is dirty since 13 Nov; present alone is dirty,present now
    for wb in default flush present flush,present; do
        (
            export CGLZINK_EGL=$LIB/libEGL.1.dylib CGLZINK_LOG=1
            unset CGLZINK_SURF_WRITEBACK; [ "$wb" != default ] && export CGLZINK_SURF_WRITEBACK=$wb
            export DYLD_INSERT_LIBRARIES=$LIB/libcglzink.dylib
            tmo $((SECS + 25)) "$BIN/zgbench" -a -i -f 3 $SIZE -s "$SECS" -q 1 -p 4 -m 0 -u none > "$d/zink-$wb.txt" 2> "$d/zink-$wb.err"
        )
        echo "zink $wb $(grep '^zgbench: fps' "$d/zink-$wb.txt" | cut -c10-) [$(grep -c 'copy-back skipped' "$d/zink-$wb.err") skip lines]"
    done
    echo "-- layertest through real zink (Apple's GL reads its own IOSurface texels as DIFFERing from the surface, zink as"
    echo "   equal: that line and the 'strings' lines differ for every mode; 'present' may differ in the other IOSurface lines)"
    tmo 30 "$BIN/layertest" > "$d/lt-apple.txt" 2> "$d/lt-apple.err"
    for wb in default flush present; do
        (
            export CGLZINK_EGL=$LIB/libEGL.1.dylib CGLZINK_LOG=1
            unset CGLZINK_SURF_WRITEBACK; [ "$wb" != default ] && export CGLZINK_SURF_WRITEBACK=$wb
            export DYLD_INSERT_LIBRARIES=$LIB/libcglzink.dylib
            tmo 30 "$BIN/layertest" > "$d/lt-$wb.txt" 2> "$d/lt-$wb.err"
        )
        if cmp -s "$d/lt-apple.txt" "$d/lt-$wb.txt"; then echo "layertest $wb: same as Apple's GL"
        else echo "layertest $wb: differs:"; diff "$d/lt-apple.txt" "$d/lt-$wb.txt" | head -8; fi
    done
}

# mc CONFIG|off: Minecraft through the installed zink reads /tmp/mesa-options-java (patch 0020: NAME=VALUE lines,
# '#' comments, the first line with a name wins) when it starts. zink_ab.sh arm writes that file; this adds CONFIG's
# knobs as a "# zinkgap begin/end" block (or makes the file), and "off" removes only that block.
mc() {
    local cfg=${1:?mc CONFIG|off} opt=/tmp/mesa-options-java
    [ -f "$opt" ] && { sed '/^# zinkgap begin/,/^# zinkgap end$/d' "$opt" > "$opt.tmp"; cat "$opt.tmp" > "$opt"; rm -f "$opt.tmp"; }
    if [ "$cfg" = off ]; then
        [ -f "$opt" ] && ! grep -q '=' "$opt" && rm -f "$opt"
        echo "zinkgap knobs removed from $opt"; return 0
    fi
    local envs; envs=$(cfg_env "$cfg") || { echo "unknown config $cfg"; return 2; }
    case "$envs" in *ZINK_BAR_BUFFERS*) echo "NOTE: ZINK_BAR_BUFFERS needs the staged libgallium (0107) in /Library/IntelXe/GL; the released one ignores it";; esac
    ( umask 077; { echo "# zinkgap begin $cfg"; for kv in $envs; do echo "$kv"; done; echo "# zinkgap end"; } >> "$opt" )
    chmod go-w "$opt"
    echo "$opt now:"; cat "$opt"
    echo "Start the game now (zink_ab.sh launch), measure (zink_ab.sh sample <label>), then: $0 mc off"
}

# What the 13 Nov night measured (B580, window 1920x1080, installed ANV, 2 rounds, medians; GPU ms per frame) and what
# a run of this kit should show against it. A row that does not match is the thing to look at.
expected() {
    cat <<'EOT'
== 13 Nov reference (zinkgap-night, installed ANV without 0108; fps is present/CPU-bound near 300)
def      base 294.6 fps 2.091 ms | rtfix 298.8 / 1.866 (-10.7%) | combo 299.2 / 1.848 (-11.6%) | others within +-3.5%
nomip    base 425.8 fps 2.229 ms | rtfix 430.3 / 1.420 (-36.3%) | lazy, rp, ref-norb within 1%
mapinv   base 289.4 fps 1.321 ms | barsys -0.6%, barall +3.0% (noise)
orphan   base 275.9 fps 2.757 ms | barsys +2.0%, barall +3.2%; image varied in every config (zgbench bug, fixed)
persist  base 297.7 fps 1.259 ms | barsys -1.5% (one of 2 images differed: zgbench race, fixed), barall -4.4%
ref-*    INTEL_DEBUG was unset by the Metal plugin in window runs: those rows measured base (fixed: NVMTL_PROCESS_ENV=1)
cgl      Apple 1188.6 fps | zink flush 120.0 | dirty 391.2 | present 791.0 | dirty,present 813.8 (same image f3d461ba)
== expected now
staged ANV with 0108 (check says "staged lib/anv"): base ~= old rtfix (def ~1.87 ms, nomip ~1.42 ms), rtfix = base,
  rtoff ~= old base (def ~2.09, nomip ~2.23); with the installed ANV: base/rtoff as old base, rtfix as old rtfix
every image "same" (no VARIES, no "base VARIES"); repeat persist/orphan: one image per config
diag: PIPE_CONTROL counts not 0; rtfix/base(0108) far fewer rt-bti-change and CS stalls per frame than rtoff
ref-*: now really applied: nofc/noccs/nohiz should cost GPU time (if they do not, those paths are not in use)
cgl: default (dirty) ~390 fps, flush ~120, present (= dirty,present) ~810, flush,present ~790; one image hash
EOT
}

case "${1:-}" in
check)
    echo "kit $K run $R mode $MODE | $(sw_vers -productVersion) | WindowServer $(pgrep -x WindowServer) | $(health)"
    (cd "$K" && shasum -a 256 -c SHA256SUMS 2>&1 | grep -v ': OK$' | head -5; echo "stage hashes checked")
    [ -e /usr/local/lib/libvulkan.1.dylib ] && echo "OK    Vulkan loader" || echo "FAIL  no /usr/local/lib/libvulkan.1.dylib"
    ( export ZGB_EGL=$LIB/libEGL.1.dylib; tmo 40 "$BIN/zgbench" -e -s 2 -q 2 -W 1280 -H 720 2>&1 | grep '^zgbench' )
    ( export ZGB_GLFW=$LIB/libglfw.3.dylib; tmo 40 "$BIN/zgbench" -s 2 -q 1 -W 1280 -H 720 2>&1 | grep '^zgbench' ) ||
        echo "window mode failed: run from the logged-in session or use ZG_MODE=pbuffer"
    echo "ANV for zink: $ANV_USED${VK_DRIVER_FILES:+ ($VK_DRIVER_FILES)}"
    # do INTEL_* knobs reach ANV in a window process (the Metal plugin is loaded there)? 0 lines = they do not
    n=$( ( export ZGB_GLFW=$LIB/libglfw.3.dylib INTEL_DEBUG=pc MESA_LOG=file MESA_LOG_LEVEL=info
           tmo 40 "$BIN/zgbench" -s 1 -q 0 -W 640 -H 360 -p 2 -m 0 -u none 2>&1 >/dev/null | grep -c 'emit PC=' ) )
    [ "${n:-0}" -gt 0 ] && echo "OK    INTEL_DEBUG reaches ANV in a window ($n PIPE_CONTROL lines in ~2 s)" ||
        echo "FAIL  INTEL_DEBUG does not reach ANV in a window: diag and ref-* would measure nothing"
    echo "$(health)";;
ab1)
    run "${2:?ab1 CONFIG [WORKLOAD]}" "${3:-def}";;
ab)
    rounds=${2:-1}
    echo "ab: $rounds round(s), $MODE, $SECS s per run, $SIZE | $(health)" | tee -a "$R/ab.log"
    for ((i = 1; i <= rounds; i++)); do
        matrix | while read -r wl cfgs; do for c in $cfgs; do run "$c" "$wl"; done; done | tee -a "$R/ab.log"
    done
    echo "ab done | $(health)" | tee -a "$R/ab.log"
    table;;
repeat)
    # repeat CONFIG WORKLOAD N: N runs in a row (each <= 60 s), for an image hash that must not vary
    for ((i = 1; i <= ${4:-3}; i++)); do run "${2:?repeat CONFIG WORKLOAD [N]}" "${3:?repeat CONFIG WORKLOAD [N]}"; done;;
expected) expected;;
diag) diag "${2:-base}";;
cgl) cgl;;
mc) mc "${2:-}";;
table) table;;
configs)
    for c in $CONFIGS; do printf '%-10s %s\n' "$c" "$(cfg_env "$c")"; done
    matrix | while read -r wl cfgs; do printf 'workload %-8s %-32s configs: %s\n' "$wl" "$(wl_args "$wl")" "$cfgs"; done;;
night)
    {
        echo "== night $(date) $(health)"
        "$0" check
        "$0" diag base
        "$0" diag rtfix
        "$0" diag rtoff
        "$0" ab 2
        "$0" repeat base persist 3
        "$0" repeat barsys persist 3
        "$0" repeat base orphan 2
        "$0" table
        "$0" cgl
        "$0" expected
        echo "== end $(date) $(health)"
    } 2>&1 | tee "$R/night.log";;
*) sed -n '2,/^set -uo/p' "$0" | sed '$d'; exit 2;;
esac
