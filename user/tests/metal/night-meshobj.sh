# SPDX-License-Identifier: MIT
# night phase M (sourced by the night script, which defines tmo, say and $O; branch fix016): Metal mesh pipelines with an
# object stage, built again after the translator fix (mesh_lower.rs rebase_handle: the mesh handle spelled "captures(none)"
# by macOS 26's Metal compiler; docs/design/DESIGN-mesh-object.md). ~/b580/fix016 (or $MESHOBJ_STAGE): the staged
# NVMTLDriver.bundle, nvmtl_interpose.dylib and the x86_64 tests mtl_mesh_object, mtl_desc_paths, mtl_smoke. The plugin is
# used by the test processes only; nothing is installed.
#   source night-meshobj.sh; phase_meshobj [stage dir]    from a night script that defines O, say and tmo
#   bash night-meshobj.sh [stage dir]                     alone: O=~/night-meshobj-<time>, say = echo with the time
# Expected:
#   M1 mtl_mesh_object (default model): 4 PASS, exit=0; no "GPU RECOVERY", no "slowest submit" past a second
#   M2 mtl_mesh_object NVMTL_DESC_MODEL=classic and =buffer: 4 PASS, exit=0 each
#   M3 mtl_desc_paths mesh, both models: PASS mesh (it was SKIP: refused in 0.1.5)
#   M4 mtl_mesh_object NVMTL_MESH_OBJECT=0: object-rows and object-redraw FAIL "REFUSED ... (NVMTL_MESH_OBJECT=0)",
#      mesh-grid and mesh-indirect PASS, exit=5 (the 0.1.5 behaviour, kept as the switch)
#   M5 mtl_smoke as on main
#   GuC context resets / GT resets unchanged before and after (13 Nov: the old path took a GuC context reset).
# The log lines to read when a case fails: "mesh pipeline:" (mode, layout, cap), "mesh:" (per draw: threadgroups, scratch).
# A FAIL with an image map ("drawn ##../") says which cells came out: all in one cell = the threadgroups share one block.
# tmo is the night script's bash timeout function: the DYLD_* variables are exported in the subshell and reach the test
# binary directly (no perl/env/sh wrapper, which a protected binary strips them through).
type tmo >/dev/null 2>&1 || tmo() {   # tmo <seconds> <command...>: run it, kill it after <seconds>; its exit code (137 = killed)
    local s=$1; shift
    "$@" & local p=$!
    ( sleep "$s"; kill -9 "$p" 2>/dev/null ) & local w=$!
    wait "$p"; local rc=$?
    pkill -P "$w" sleep 2>/dev/null; kill "$w" 2>/dev/null; wait "$w" 2>/dev/null
    return $rc
}
type say >/dev/null 2>&1 || say() { echo "$(date +%H:%M:%S) $*" | tee -a "$O/summary.txt"; }

meshobj_st() { sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+'; }
meshobj_notes() { grep -hE 'mesh pipeline:|mesh: |REFUSED|GPU RECOVERY|slowest submit' "$1" 2>/dev/null | head -8 | cut -c1-200 | tr '\n' ';'; }

phase_meshobj() {
  local D=${1:-${MESHOBJ_STAGE:-$HOME/b580/fix016}} R="$O/meshobj"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  say "M: mesh object stage ($D) - $(meshobj_st)"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/1.log"; tmo 60 ./mtl_mesh_object > "$R/1-mesh.txt" 2>&1; echo "exit=$?" >> "$R/1-mesh.txt" )
  say "M1 mtl_mesh_object (want 4 PASS, exit=0): $(tail -2 "$R/1-mesh.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/1-mesh.txt" | cut -c1-260 | tr '\n' ';')"
  say "M1 notes: $(meshobj_notes "$R/1.log") - $(meshobj_st)"
  local m
  for m in classic buffer; do
    ( cd "$D" && export $S NVMTL_DESC_MODEL=$m NVMTL_LOG_PATH="$R/2-$m.log"; tmo 60 ./mtl_mesh_object > "$R/2-mesh-$m.txt" 2>&1; echo "exit=$?" >> "$R/2-mesh-$m.txt" )
    say "M2 mtl_mesh_object $m (want 4 PASS, exit=0): $(tail -2 "$R/2-mesh-$m.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/2-mesh-$m.txt" | cut -c1-200 | tr '\n' ';')"
    ( cd "$D" && export $S NVMTL_DESC_MODEL=$m NVMTL_LOG_PATH="$R/3-$m.log"; tmo 60 ./mtl_desc_paths mesh > "$R/3-paths-$m.txt" 2>&1; echo "exit=$?" >> "$R/3-paths-$m.txt" )
    say "M3 mtl_desc_paths mesh $m (want PASS mesh): $(grep -E '^(PASS|FAIL|SKIP) mesh' "$R/3-paths-$m.txt" | cut -c1-200) $(tail -1 "$R/3-paths-$m.txt")"
  done
  say "M2/M3 notes: $(meshobj_notes "$R/2-buffer.log") - $(meshobj_st)"
  ( cd "$D" && export $S NVMTL_MESH_OBJECT=0 NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl_mesh_object > "$R/4-off.txt" 2>&1; echo "exit=$?" >> "$R/4-off.txt" )
  say "M4 NVMTL_MESH_OBJECT=0 (want 2 PASS 2 FAIL REFUSED, exit=5): $(tail -2 "$R/4-off.txt" | tr '\n' ' ') $(grep -c 'NVMTL_MESH_OBJECT=0' "$R/4-off.txt") refusal(s)"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/5.log"; tmo 60 ./mtl_smoke > "$R/5-smoke.txt" 2>&1; echo "exit=$?" >> "$R/5-smoke.txt" )
  say "M5 mtl_smoke: $(tail -2 "$R/5-smoke.txt" | tr '\n' ' ')"
  say "M: after: $(meshobj_st)"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
  set -uo pipefail
  O=${O:-$HOME/night-meshobj-$(date +%Y%m%d-%H%M%S)}; mkdir -p "$O"
  phase_meshobj "$@"
fi
