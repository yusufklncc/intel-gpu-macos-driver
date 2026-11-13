# SPDX-License-Identifier: MIT
# night phase S2 (sourced by the night script, which defines tmo, say and $O): sparse textures tier 2, the per-tile access
# counters (branch sparse2; NVMTL_SPARSE_TIER2=1 with NVMTL_SPARSE_TEXTURES=1), and tier 1 with tier 2 on and off.
# The stage (~/hackintosh/work/night1214/sparse2 on the build Mac) is copied to ~/b580/sparse2 (SPARSE2_STAGE overrides):
# the branch's NVMTLDriver.bundle and nvmtl_interpose.dylib, and mtl_sparse_tier2, mtl_sparse, mtl_sparse_buffer
# (nullmoth/build/build-intelxe.sh plugin tests). The plugin is used by the test processes only; nothing is installed.
# Expected: S2.1 SKIP, exit=5 (no knobs: no sparse tile); S2.2 SKIP, exit=5 (sparse on, tier 2 off: Tier1);
# S2.3 21 PASS, exit=0 (tier 2: an M1 Pro passes the same 21); S2.4/S2.5 mtl_sparse 106/106 with tier 2 off and on;
# S2.6/S2.7 mtl_sparse_buffer 27/27 with tier 2 off and on (NVMTL_METAL4=1 for its queue).
# tmo is the night script's bash timeout function: the DYLD_* variables are exported in the subshell and reach the test
# binary (no perl/env wrapper, which a protected binary would strip them through).
phase_sparse2() {
  local D=${SPARSE2_STAGE:-~/b580/sparse2} R="$O/sparse2"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  local SP="NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1" T2="NVMTL_SPARSE_TIER2=1" M4="NVMTL_METAL4=1"
  say "S2: sparse tier 2 (access counters) - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && export $S; tmo 60 ./mtl_sparse_tier2 > "$R/1-t2-noknobs.txt" 2>&1; echo "exit=$?" >> "$R/1-t2-noknobs.txt" )
  say "S2.1 mtl_sparse_tier2, no knobs (want SKIP, exit=5): $(tail -2 "$R/1-t2-noknobs.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $SP NVMTL_LOG_PATH="$R/2.log"; tmo 60 ./mtl_sparse_tier2 > "$R/2-t2-off.txt" 2>&1; echo "exit=$?" >> "$R/2-t2-off.txt" )
  say "S2.2 mtl_sparse_tier2, sparse on, tier 2 off (want SKIP Tier1, exit=5): $(tail -2 "$R/2-t2-off.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $SP $T2 NVMTL_LOG_PATH="$R/3.log"; tmo 60 ./mtl_sparse_tier2 > "$R/3-t2-on.txt" 2>&1; echo "exit=$?" >> "$R/3-t2-on.txt" )
  say "S2.3 mtl_sparse_tier2, NVMTL_SPARSE_TIER2=1 (want 21 PASS, exit=0): $(grep -E 'PASS, |exit=' "$R/3-t2-on.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/3-t2-on.txt" | cut -c1-200 | head -4 | tr '\n' ';')"
  say "S2.3 notes: $(grep -hE 'sparse tier 2|access counters|blit (get|reset)TextureAccessCounters|pipeline REFUSED' "$R/3.log" 2>/dev/null | head -8 | cut -c1-220 | tr '\n' ';')"
  ( cd "$D" && export $S $SP NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl_sparse > "$R/4-sparse-t2off.txt" 2>&1; echo "exit=$?" >> "$R/4-sparse-t2off.txt" )
  say "S2.4 mtl_sparse, tier 2 off (want 106/106): $(grep -E 'PASS, |exit=' "$R/4-sparse-t2off.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/4-sparse-t2off.txt")"
  ( cd "$D" && export $S $SP $T2 NVMTL_LOG_PATH="$R/5.log"; tmo 60 ./mtl_sparse > "$R/5-sparse-t2on.txt" 2>&1; echo "exit=$?" >> "$R/5-sparse-t2on.txt" )
  say "S2.5 mtl_sparse, tier 2 on (want 106/106): $(grep -E 'PASS, |exit=' "$R/5-sparse-t2on.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/5-sparse-t2on.txt")"
  ( cd "$D" && export $S $SP $M4 NVMTL_LOG_PATH="$R/6.log"; tmo 60 ./mtl_sparse_buffer > "$R/6-spbuf-t2off.txt" 2>&1; echo "exit=$?" >> "$R/6-spbuf-t2off.txt" )
  say "S2.6 mtl_sparse_buffer, tier 2 off (want 27 PASS): $(tail -2 "$R/6-spbuf-t2off.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/6-spbuf-t2off.txt")"
  ( cd "$D" && export $S $SP $M4 $T2 NVMTL_LOG_PATH="$R/7.log"; tmo 60 ./mtl_sparse_buffer > "$R/7-spbuf-t2on.txt" 2>&1; echo "exit=$?" >> "$R/7-spbuf-t2on.txt" )
  say "S2.7 mtl_sparse_buffer, tier 2 on (want 27 PASS): $(tail -2 "$R/7-spbuf-t2on.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/7-spbuf-t2on.txt")"
  say "S2: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
