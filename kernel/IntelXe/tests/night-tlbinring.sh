# night phase tlbinring (sourced by the night scripts after night-kernel.sh, which defines gbrun and resets; tmo, say and
# $O come from the night driver). Needs the tlbinring kext (branch tlbinring: debug.intelxe.tlb_inring) installed.
# docs/design/DESIGN-rb8-cost.md "tlbinring": with tlb_inring 1 an EXEC whose only pending in-fences are TLB
# invalidations waits for them in the ring (MI_SEMAPHORE_WAIT on the TLB word) and an armed queue stays in direct
# submission; 0 (default) is the rb8f behaviour (FIFO, dsExit).
#  1. Geekbench 6 OpenCL import x3, knob 0 vs 1 alternating (0 first, then 1 first, then 0 first); the "rb8 cost" and
#     "direct submission" stats lines before and after each run -> $O/tlbinring-counters.txt, scores -> $O/tlbinring.txt
#  2. cl_gbmicro (chain, churn, import, mixed; the plugin's unaligned import on) x3, knob 0 vs 1 alternating ->
#     $O/tlbinring-gbcl/<knob>-<run>.log, RESULT lines in $O/tlbinring-gbcl/results.txt
#  3. CAT stress with the knob on: bindstress 10 min, all four queue kinds, EXECs waiting on the bind timeline as Mesa's
#     (bindfence; the binary next to this script, built from branch tlbinring, since ~/b580/cat/bindstress has no
#     bindfence and its EXECs have no in-fences, so they would never take the new path) -> $O/bs-tlbinring.log
# The knob goes back to 0 at the end. TLBINRING_ON=2 runs the "on" side with the normal-ring path as well.
TLBINRING_DIR=${TLBINRING_DIR:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)}
TLBINRING_ON=${TLBINRING_ON:-1}
tlbinring_set() { sudo -n sysctl debug.intelxe.tlb_inring=$1 >/dev/null 2>&1; }
tlbinring_line() {
  local s; s=$(sysctl -n debug.intelxe.stats)
  echo "$s" | grep -o 'rb8 cost.*'
  echo "$s" | grep -o 'direct submission.*' | head -1
  echo "$s" | grep -oE 'jobs: direct [0-9]+, via worker [0-9]+'
}
# the tlbinring counters in one short line: armed slot, normal ring, word already there; fallbacks
tlbinring_short() {
  sysctl -n debug.intelxe.stats | grep -o 'rb8 cost.*' | sed -nE 's/.*(tlb_inring [0-9]+).*(EXEC in-fences on an invalidation [0-9]+).*(EXECs to the worker for in-fences [0-9]+ \(left direct mode [0-9]+\)).*(armed slot [0-9]+, normal ring [0-9]+ \(word already there [0-9]+\)); old path: ([^;]*);.*/\1; \2; \3; \4; fallback \5/p'
}
tlbinring_gb() {   # knob tag
  tlbinring_set "$1"
  { echo "== $2 (tlb_inring=$1) before"; tlbinring_line; } >> "$O/tlbinring-counters.txt"
  gbrun OpenCL import "$2"
  { echo "== $2 after"; tlbinring_line; } >> "$O/tlbinring-counters.txt"
  echo "$2 $(tail -1 "$O/gb-results.txt" | awk '{print $3}') tlb_inring=$1" >> "$O/tlbinring.txt"
  say "TLBINRING $2: $(tlbinring_short)"
}
tlbinring_gbcl() {   # knob run
  local d="$O/tlbinring-gbcl" g="$TLBINRING_DIR/cl_gbmicro" tag="k$1-$2" r0 r1 g0 t0 g1 t1
  [ -x "$g" ] || { say "TLBINRING gbcl: no binary at $g"; return 0; }
  mkdir -p "$d"; tlbinring_set "$1"; r0=$(resets)
  ( export NVMTL_PROCESS_ENV=1 NVMTL_ENABLE=1 NVMTL_IMPORT_UNALIGNED=1
    tmo $((15 * 4 * 3 + 60)) "$g" -q -t 15 -m chain,churn,import,mixed ) > "$d/$tag.log" 2>&1
  r1=$(resets); read -r g0 t0 <<< "${r0:-0 0}"; read -r g1 t1 <<< "${r1:-0 0}"
  sed -n "s/^RESULT /RESULT knob=$1 run=$2 /p" "$d/$tag.log" >> "$d/results.txt"
  grep -qE '^(CHECK|FAIL)' "$d/$tag.log" && say "TLBINRING gbcl $tag: $(grep -E '^(CHECK|FAIL)' "$d/$tag.log" | head -2 | tr '\n' ' ')"
  [ $((g1 - g0 + t1 - t0)) -gt 0 ] && say "TLBINRING gbcl $tag: guc+$((g1 - g0)) gt+$((t1 - t0))"
  say "TLBINRING gbcl $tag: $(grep '^RESULT' "$d/$tag.log" | sed -nE 's/RESULT mode=([a-z]+) .*ops_s=([0-9.]+) p50_us=([0-9.]+).*/\1 \2\/s p50 \3/p' | tr '\n' ' ')"
}
phase_tlbinring() {
  if ! sysctl -n debug.intelxe.tlb_inring >/dev/null 2>&1; then say "TLBINRING: kext without debug.intelxe.tlb_inring, skipped"; return 0; fi
  say "TLBINRING start: exec_tlb_dep $(sysctl -n debug.intelxe.exec_tlb_dep 2>&1) (want 1), direct $(sysctl -n debug.intelxe.direct), on = $TLBINRING_ON; resets $(resets)"
  sudo -n sysctl debug.intelxe.exec_tlb_dep=1 >/dev/null 2>&1
  : > "$O/tlbinring.txt"
  # 1. Geekbench OpenCL import, alternating which side goes first
  for i in 1 2 3; do
    if [ $((i % 2)) = 1 ]; then tlbinring_gb 0 "tlbinring-off$i"; tlbinring_gb "$TLBINRING_ON" "tlbinring-on$i"
    else tlbinring_gb "$TLBINRING_ON" "tlbinring-on$i"; tlbinring_gb 0 "tlbinring-off$i"; fi
  done
  say "TLBINRING GB OpenCL import: off $(grep -E '^tlbinring-off' "$O/tlbinring.txt" | awk '{print $2}' | tr '\n' ' ')on $(grep -E '^tlbinring-on' "$O/tlbinring.txt" | awk '{print $2}' | tr '\n' ' ')"
  # 2. cl_gbmicro
  mkdir -p "$O/tlbinring-gbcl"; : > "$O/tlbinring-gbcl/results.txt"
  for i in 1 2 3; do
    if [ $((i % 2)) = 1 ]; then tlbinring_gbcl 0 "$i"; tlbinring_gbcl "$TLBINRING_ON" "$i"
    else tlbinring_gbcl "$TLBINRING_ON" "$i"; tlbinring_gbcl 0 "$i"; fi
  done
  # 3. CAT stress, knob on
  local bs="$TLBINRING_DIR/bindstress-tlb" mode=bindfence
  [ -x "$bs" ] || { bs=~/b580/cat/bindstress; mode=""; say "TLBINRING bindstress: no bindstress-tlb next to this script, ~/b580/cat/bindstress without bindfence (the new path is not exercised)"; }
  tlbinring_set "$TLBINRING_ON"
  { echo "== bindstress (tlb_inring=$TLBINRING_ON) before"; tlbinring_line; } >> "$O/tlbinring-counters.txt"
  ( cd ~/b580/cat && tmo 800 "$bs" 10 r,rl,c,cl $mode > "$O/bs-tlbinring.log" 2>&1 )
  { echo "== bindstress after"; tlbinring_line; } >> "$O/tlbinring-counters.txt"
  say "TLBINRING bindstress 10 min $mode tlb_inring=$TLBINRING_ON: $(tail -1 "$O/bs-tlbinring.log"); $(grep -m1 '^kext deltas' "$O/bs-tlbinring.log" | cut -c1-160)"
  say "TLBINRING bindstress counters: $(tlbinring_short)"
  grep -q 'RESULT: PASS' "$O/bs-tlbinring.log" || sudo -n sysctl -n debug.intelxe.g2h | tail -60 > "$O/g2h-tlbinring-bs.txt" 2>&1
  tlbinring_set 0
  say "TLBINRING end: tlb_inring $(sysctl -n debug.intelxe.tlb_inring); resets $(resets)"
}
