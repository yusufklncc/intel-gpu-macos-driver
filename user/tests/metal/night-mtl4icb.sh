# SPDX-License-Identifier: MIT
# night phase Q (sourced by the night script, which defines tmo, say and $O; branch mtl4icb): pipeline-independent binary
# functions (MTL4BinaryFunctionOptionPipelineIndependent) and WebKit-shaped GPU-encoded indirect draws into ICB slots the
# CPU never drew (vsICBIndirect*: an index buffer by address, a primitive type from data, the class's strip learned).
# ~/b580/mtl4icb: the branch's staged NVMTLDriver.bundle, nvmtl_interpose.dylib and its tests (build-intelxe.sh plugin
# tests: link_host + link/, mtl4_linking, mtl4_compiler, mtl_gpu_icb, mtl_gpu_icb2, mtl_gpu_icb3, mtl_icb_cost). The plugin
# is used by the test processes only; nothing is installed.
# Expected:
#   Q1 link_host 18 PASS, exit=0 (no GPU)            Q2 mtl4_linking NVMTL_METAL4=1: 18 PASS (case 13 made, both directions)
#   Q3 mtl_gpu_icb3 knob off: 16 PASS, exit=0 (cells 0, 1, 3, 4 black as the expected refusal)
#   Q4 mtl_gpu_icb3 NVMTL_ICB_DGC=1: 16 PASS (pass 0 strips may be black: learned; pass 1 all drawn)
#   Q5 mtl_gpu_icb3 NVMTL_ICB_DGC=1 NVMTL_ICB_DGC_STRIPS=1: 16 PASS (every cell from the first execute)
#   Q6 mtl_gpu_icb knob off / on: 6/6 each        Q7 mtl_gpu_icb2 knob on: B0-B5, A0, A1, C3 PASS; C0-C2 open (dispatch log)
#   Q8 mtl_icb_cost knob on (per-slot cost, as I5)  Q9 mtl4_compiler NVMTL_METAL4=1: 14 PASS
# tmo is the night script's bash timeout function: the DYLD_* variables are exported in the subshell and reach the test
# binary directly (no perl/env/sh wrapper, which a protected binary strips them through).
phase_mtl4icb() {
  local D=~/b580/mtl4icb R="$O/mtl4icb"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  local M4="NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1" K="NVMTL_PROCESS_ENV=1 NVMTL_ICB_DGC=1"
  say "Q: pipeline-independent binary functions / WebKit-shaped ICB draws - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/1.log"; tmo 60 ./link_host "$D/NVMTLDriver.bundle" "$D/link" > "$R/1-link-host.txt" 2>&1; echo "exit=$?" >> "$R/1-link-host.txt" )
  say "Q1 link_host (no GPU; want 18 PASS, exit=0): $(grep -m1 'another NVMTLDriver\|setup:' "$R/1-link-host.txt" | cut -c1-200) $(tail -2 "$R/1-link-host.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/1-link-host.txt" | cut -c1-160 | head -3 | tr '\n' ';')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/2.log"; tmo 90 ./mtl4_linking > "$R/2-linking-on.txt" 2>&1; echo "exit=$?" >> "$R/2-linking-on.txt" )
  say "Q2 mtl4_linking, NVMTL_METAL4=1 (want 18 PASS, exit=0): $(tail -2 "$R/2-linking-on.txt" | tr '\n' ' ') case 13: $(grep -m1 'PipelineIndependent' "$R/2-linking-on.txt" | cut -c1-200)"
  say "Q2 link notes: $(grep -hE 'pipeline-independent|nvmtl_link_runtime_ids' "$R/2.log" 2>/dev/null | head -4 | tr '\n' ';')"
  ( cd "$D" && export $S NVMTL_PROCESS_ENV=1 NVMTL_LOG_PATH="$R/3.log"; tmo 60 ./mtl_gpu_icb3 > "$R/3-icb3-off.txt" 2>&1; echo "exit=$?" >> "$R/3-icb3-off.txt" )
  say "Q3 mtl_gpu_icb3 knob off (want 16 PASS, exit=0): $(grep -c '^PASS' "$R/3-icb3-off.txt") PASS; FAIL: $(grep '^FAIL' "$R/3-icb3-off.txt" | cut -c1-60 | tr '\n' ';') $(tail -1 "$R/3-icb3-off.txt")"
  ( cd "$D" && export $S $K NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl_gpu_icb3 > "$R/4-icb3-on.txt" 2>&1; echo "exit=$?" >> "$R/4-icb3-on.txt" )
  say "Q4 mtl_gpu_icb3 NVMTL_ICB_DGC=1 (want 16 PASS): $(grep -c '^PASS' "$R/4-icb3-on.txt") PASS; FAIL: $(grep '^FAIL' "$R/4-icb3-on.txt" | cut -c1-60 | tr '\n' ';') $(tail -1 "$R/4-icb3-on.txt")"
  say "Q4 notes: $(grep -hE 'changed topology|NOT drawn|NOT run|device-generated execute|cannot express' "$R/4.log" 2>/dev/null | head -6 | tr '\n' ';')"
  ( cd "$D" && export $S $K NVMTL_ICB_DGC_STRIPS=1 NVMTL_LOG_PATH="$R/5.log"; tmo 60 ./mtl_gpu_icb3 > "$R/5-icb3-strips.txt" 2>&1; echo "exit=$?" >> "$R/5-icb3-strips.txt" )
  say "Q5 mtl_gpu_icb3 NVMTL_ICB_DGC_STRIPS=1 (want 16 PASS): $(grep -c '^PASS' "$R/5-icb3-strips.txt") PASS; FAIL: $(grep '^FAIL' "$R/5-icb3-strips.txt" | cut -c1-60 | tr '\n' ';') $(tail -1 "$R/5-icb3-strips.txt")"
  ( cd "$D" && export $S; tmo 60 ./mtl_gpu_icb > "$R/6-icb-off.txt" 2>&1 )
  ( cd "$D" && export $S $K NVMTL_LOG_PATH="$R/6.log"; tmo 60 ./mtl_gpu_icb > "$R/6-icb-on.txt" 2>&1 )
  say "Q6 mtl_gpu_icb off / on (want 6/6 each): $(tail -1 "$R/6-icb-off.txt") / $(tail -1 "$R/6-icb-on.txt")"
  ( cd "$D" && export $S $K NVMTL_LOG_PATH="$R/7.log"; tmo 60 ./mtl_gpu_icb2 > "$R/7-icb2-on.txt" 2>&1; echo "exit=$?" >> "$R/7-icb2-on.txt" )
  say "Q7 mtl_gpu_icb2 NVMTL_ICB_DGC=1 (want B0-B5 A0 A1 C3 PASS; C0-C2 open): $(grep -c '^PASS' "$R/7-icb2-on.txt") PASS; FAIL: $(grep '^FAIL' "$R/7-icb2-on.txt" | cut -c1-8 | tr '\n' ' ') $(tail -1 "$R/7-icb2-on.txt")"
  say "Q7 dispatch records: $(grep -hE 'after the execute' "$R/7.log" 2>/dev/null | sed 's/.*slot/slot/' | head -8 | tr '\n' ';')"
  ( cd "$D" && export $S $K NVMTL_LOG_PATH="$R/8.log"; tmo 60 ./mtl_icb_cost 1024 9 > "$R/8-cost-on.txt" 2>&1 )
  say "Q8 mtl_icb_cost knob on: $(grep -iE 'slot|us' "$R/8-cost-on.txt" | tail -2 | tr '\n' ' ')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/9.log"; tmo 60 ./mtl4_compiler > "$R/9-comp-on.txt" 2>&1; echo "exit=$?" >> "$R/9-comp-on.txt" )
  say "Q9 mtl4_compiler NVMTL_METAL4=1 (want 14 PASS): $(tail -2 "$R/9-comp-on.txt" | tr '\n' ' ')"
  say "Q: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
