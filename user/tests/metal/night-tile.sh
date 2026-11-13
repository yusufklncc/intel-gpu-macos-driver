# SPDX-License-Identifier: MIT
# night phase T (sourced by the night script, which defines tmo, say and $O; branch mtl4tile): tile pipelines by render-pass
# splitting (nullmoth/plugin/NVMTLTile.m, docs/design/DESIGN-tile-shading.md). ~/b580/mtl4tile: the branch's staged
# NVMTLDriver.bundle, nvmtl_interpose.dylib and its tests (build-intelxe.sh plugin tests: tile_host + tile/, link_host +
# link/, mtl_tile, mtl4_linking, mtl4_compiler, mtl_smoke). The plugin is used by the test processes only; nothing is
# installed.
# Expected:
#   T1 tile_host 9 PASS, exit=0 (no GPU)          T2 link_host 19 PASS, exit=0 (no GPU)
#   T3 mtl_tile: cases 1-6 PASS, 7 SKIP (Metal 4 off), exit=0
#   T4 mtl_tile NVMTL_METAL4=1: 7 PASS, exit=0     T5 mtl4_linking NVMTL_METAL4=1: 18 PASS (cases 5 and 12 made and run)
#   T6 mtl_smoke and mtl4_compiler NVMTL_METAL4=1 as on main (no render or compute change outside tile dispatches)
#   GuC context resets / GT resets unchanged before and after.
# The log lines to read when a case fails: "tile pipeline:" (made, masks), "dispatchThreadsPerTile" (tiles, planes),
# "tile dispatch:" / "tile pipelines:" (copy shaders, copy back).
# tmo is the night script's bash timeout function: the DYLD_* variables are exported in the subshell and reach the test
# binary directly (no perl/env/sh wrapper, which a protected binary strips them through).
phase_tile() {
  local D=~/b580/mtl4tile R="$O/mtl4tile"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  local M4="NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1"
  say "T: tile pipelines (render-pass splitting) - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/1.log"; tmo 60 ./tile_host "$D/NVMTLDriver.bundle" "$D/tile" > "$R/1-tile-host.txt" 2>&1; echo "exit=$?" >> "$R/1-tile-host.txt" )
  say "T1 tile_host (no GPU; want 9 PASS, exit=0): $(tail -2 "$R/1-tile-host.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/1-tile-host.txt" | cut -c1-200 | head -3 | tr '\n' ';')"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/2.log"; tmo 60 ./link_host "$D/NVMTLDriver.bundle" "$D/link" > "$R/2-link-host.txt" 2>&1; echo "exit=$?" >> "$R/2-link-host.txt" )
  say "T2 link_host (no GPU; want 19 PASS, exit=0): $(tail -2 "$R/2-link-host.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/2-link-host.txt" | cut -c1-160 | head -3 | tr '\n' ';')"
  ( cd "$D" && export $S NVMTL_PROCESS_ENV=1 NVMTL_LOG_PATH="$R/3.log"; tmo 60 ./mtl_tile > "$R/3-tile.txt" 2>&1; echo "exit=$?" >> "$R/3-tile.txt" )
  say "T3 mtl_tile (want 6 PASS 1 SKIP, exit=0): $(tail -2 "$R/3-tile.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/3-tile.txt" | cut -c1-220 | tr '\n' ';')"
  say "T3 notes: $(grep -hE 'tile pipeline:|dispatchThreadsPerTile|tile dispatch:|tile pipelines:|REFUSED' "$R/3.log" 2>/dev/null | head -10 | cut -c1-220 | tr '\n' ';')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl_tile > "$R/4-tile-m4.txt" 2>&1; echo "exit=$?" >> "$R/4-tile-m4.txt" )
  say "T4 mtl_tile NVMTL_METAL4=1 (want 7 PASS, exit=0): $(tail -2 "$R/4-tile-m4.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/4-tile-m4.txt" | cut -c1-220 | tr '\n' ';')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/5.log"; tmo 90 ./mtl4_linking > "$R/5-linking.txt" 2>&1; echo "exit=$?" >> "$R/5-linking.txt" )
  say "T5 mtl4_linking NVMTL_METAL4=1 (want 18 PASS, exit=0): $(tail -2 "$R/5-linking.txt" | tr '\n' ' ') tile: $(grep -iE 'tile' "$R/5-linking.txt" | cut -c1-120 | tr '\n' ';')"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/6.log"; tmo 60 ./mtl_smoke > "$R/6-smoke.txt" 2>&1; echo "exit=$?" >> "$R/6-smoke.txt" )
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/6b.log"; tmo 60 ./mtl4_compiler > "$R/6-comp.txt" 2>&1; echo "exit=$?" >> "$R/6-comp.txt" )
  say "T6 mtl_smoke: $(tail -2 "$R/6-smoke.txt" | tr '\n' ' ') mtl4_compiler (want 14 PASS): $(tail -2 "$R/6-comp.txt" | tr '\n' ' ')"
  say "T: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
