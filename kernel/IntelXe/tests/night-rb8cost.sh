# night phase rb8cost (sourced by the night scripts after night-kernel.sh, which defines gbrun; tmo, say and $O come from
# the night driver). Needs the rb8cost kext installed. Per knob (docs/design/DESIGN-rb8-cost.md): Geekbench 6 OpenCL
# import x3 with the knob 0 vs 1, alternating, then all four off vs on; the "rb8 cost" stats line before and after each
# run goes to $O/rb8cost-counters.txt, scores to $O/gb-results.txt (gbrun) and $O/rb8cost.txt.
RB8COST_KNOBS="bind_tlb_fence exec_tlb_dep defer_jobs ggtt_inval_wait"
rb8cost_set() { for k in $1; do sudo -n sysctl debug.intelxe.$k=$2 >/dev/null 2>&1; done; }
rb8cost_line() { sysctl -n debug.intelxe.stats | grep -o 'rb8 cost.*'; sysctl -n debug.intelxe.stats | grep -oE 'direct submission[^;]*;[^\n]*' | cut -c1-220; }
rb8cost_one() {   # knobs value tag
  rb8cost_set "$1" "$2"
  { echo "== $3 ($1=$2) before"; rb8cost_line; } >> "$O/rb8cost-counters.txt"
  gbrun OpenCL import "$3"
  { echo "== $3 after"; rb8cost_line; } >> "$O/rb8cost-counters.txt"
  echo "$3 $(tail -1 "$O/gb-results.txt" | awk '{print $3}') [$1]=$2" >> "$O/rb8cost.txt"   # tag score knobs
}
phase_rb8cost() {
  if ! sysctl -n debug.intelxe.bind_tlb_fence >/dev/null 2>&1; then say "RB8COST: kext without the knobs, skipped"; return 0; fi
  rb8cost_set "$RB8COST_KNOBS" 1
  say "RB8COST start: $(rb8cost_line | head -1 | cut -c1-300)"
  for k in $RB8COST_KNOBS; do
    for i in 1 2 3; do rb8cost_one "$k" 0 "rb8cost-$k-off$i"; rb8cost_one "$k" 1 "rb8cost-$k-on$i"; done
    say "RB8COST $k: off $(grep -E "^rb8cost-$k-off" "$O/rb8cost.txt" | awk '{print $2}' | tr '\n' ' ') on $(grep -E "^rb8cost-$k-on" "$O/rb8cost.txt" | awk '{print $2}' | tr '\n' ' ')"
  done
  for i in 1 2 3; do rb8cost_one "$RB8COST_KNOBS" 0 "rb8cost-all-off$i"; rb8cost_one "$RB8COST_KNOBS" 1 "rb8cost-all-on$i"; done
  rb8cost_set "$RB8COST_KNOBS" 1
  say "RB8COST all: off $(grep -E '^rb8cost-all-off' "$O/rb8cost.txt" | awk '{print $2}' | tr '\n' ' ') on $(grep -E '^rb8cost-all-on' "$O/rb8cost.txt" | awk '{print $2}' | tr '\n' ' ')"
  say "RB8COST end: $(rb8cost_line | head -1 | cut -c1-300); resets $(resets)"
}
