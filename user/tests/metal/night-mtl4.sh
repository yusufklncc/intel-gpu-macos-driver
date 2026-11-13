# SPDX-License-Identifier: MIT
# night phase R (sourced by the night script, which defines tmo, say and $O): pipeline reflection, MTL4Compiler and
# placement sparse buffers (branch mtl4). ~/b580/mtl4: the branch's staged NVMTLDriver.bundle and nvmtl_interpose.dylib,
# and its tests (nullmoth/build/build-intelxe.sh plugin tests: mtl_reflection, mtl4_compiler, mtl_sparse_buffer,
# mtl_sparse, mtl_m4_inherit, refl_host, refl/). The plugin is used by the test processes only; nothing is installed.
# Expected: R1 PASS (12 entry points Metal's own); R2 20 PASS, exit=0 (exit=2 with "another NVMTLDriver": not a score); R3 Metal 3 parts PASS, Metal 4 SKIP, exit 0; R4 42 PASS;
# R5 exit 5; R6 14 PASS; R7 exit 5 (knobs off); R8 27 PASS; R9 mtl_sparse 106/106 still (textures, same batches).
# tmo is the night script's bash timeout function: the DYLD_* variables reach the test binary (no perl/env wrapper,
# which a protected binary would strip them through).
# R1/R2 load the staged bundle into their own process. They make no Metal device (13 Dec: refl_host made one first, Metal
# loaded the installed plugin, "Class NVMTLDevice is implemented in both", and it scored the installed plugin's 12/19),
# take the classes and entry points from the staged binary, and stop with exit=2 and "another NVMTLDriver is loaded"
# if a second NVMTLDriver image is in the process anyway. They run under $S too, so a device made there comes from the
# staged bundle (nvmtl_interpose), not the installed one.
phase_mtl4() {
  local D=~/b580/mtl4 R="$O/mtl4"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  local M4="NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1" SP="NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1 NVMTL_METAL4=1"
  say "R: reflection / MTL4Compiler / sparse buffers - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && export $S; tmo 30 ./mtl_m4_inherit "$D/NVMTLDriver.bundle" > "$R/1-inherit.txt" 2>&1; echo "exit=$?" >> "$R/1-inherit.txt" )
  say "R1 mtl_m4_inherit (no GPU; want PASS): $(grep -E '^(PASS|FAIL) \(|another NVMTLDriver' "$R/1-inherit.txt" | head -2 | cut -c1-200 | tr '\n' ' ') $(tail -1 "$R/1-inherit.txt")"
  ( cd "$D" && export $S; tmo 30 ./refl_host "$D/NVMTLDriver.bundle" "$D/refl" > "$R/2-refl-host.txt" 2>&1; echo "exit=$?" >> "$R/2-refl-host.txt" )
  say "R2 refl_host (no GPU; want 20 PASS, exit=0): $(grep -m1 'another NVMTLDriver\|setup:' "$R/2-refl-host.txt" | cut -c1-200) $(tail -2 "$R/2-refl-host.txt" | tr '\n' ' ') $(grep -c 'implemented in both' "$R/2-refl-host.txt") objc duplicate-class lines"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/3.log"; tmo 60 ./mtl_reflection > "$R/3-refl-off.txt" 2>&1; echo "exit=$?" >> "$R/3-refl-off.txt" )
  say "R3 mtl_reflection, Metal 4 off (want 21 PASS 1 SKIP, exit=0): $(tail -2 "$R/3-refl-off.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl_reflection > "$R/4-refl-on.txt" 2>&1; echo "exit=$?" >> "$R/4-refl-on.txt" )
  say "R4 mtl_reflection, NVMTL_METAL4=1 (want 42 PASS): $(tail -2 "$R/4-refl-on.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S; tmo 60 ./mtl4_compiler > "$R/5-comp-off.txt" 2>&1; echo "exit=$?" >> "$R/5-comp-off.txt" )
  say "R5 mtl4_compiler, Metal 4 off (want SKIP, exit=5): $(tail -2 "$R/5-comp-off.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/6.log"; tmo 60 ./mtl4_compiler > "$R/6-comp-on.txt" 2>&1; echo "exit=$?" >> "$R/6-comp-on.txt" )
  say "R6 mtl4_compiler, NVMTL_METAL4=1 (want 14 PASS): $(tail -2 "$R/6-comp-on.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S; tmo 60 ./mtl_sparse_buffer > "$R/7-spbuf-off.txt" 2>&1; echo "exit=$?" >> "$R/7-spbuf-off.txt" )
  say "R7 mtl_sparse_buffer, knobs off (want SKIP, exit=5): $(tail -2 "$R/7-spbuf-off.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $SP NVMTL_LOG_PATH="$R/8.log"; tmo 60 ./mtl_sparse_buffer > "$R/8-spbuf-on.txt" 2>&1; echo "exit=$?" >> "$R/8-spbuf-on.txt" )
  say "R8 mtl_sparse_buffer, NVMTL_SPARSE_TEXTURES=1 NVMTL_METAL4=1 (want 27 PASS): $(tail -2 "$R/8-spbuf-on.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/8-spbuf-on.txt")"
  say "R8 notes: $(grep -hE 'sparse buffers|sparse buffer:|vkQueueBindSparse|updateBufferMappings|copyBufferMappings' "$R/8.log" 2>/dev/null | head -6 | tr '\n' ';')"
  ( cd "$D" && export $S $SP NVMTL_LOG_PATH="$R/9.log"; tmo 60 ./mtl_sparse > "$R/9-sparse-tex.txt" 2>&1; echo "exit=$?" >> "$R/9-sparse-tex.txt" )
  say "R9 mtl_sparse (textures, want 106/106): $(grep -E 'PASS, |exit=' "$R/9-sparse-tex.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/9-sparse-tex.txt")"
  say "R: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}

# night phase Rb (sourced like phase R; branch mtl4more): binary functions, dynamic linking, and the pipeline kinds the
# plugin refuses (tile, machine learning, pipelines scripts, pipeline-independent binary functions). ~/b580/mtl4b: the
# branch's staged NVMTLDriver.bundle and nvmtl_interpose.dylib, and its tests (build-intelxe.sh plugin tests:
# mtl4_linking, mtl4_compiler, mtl_reflection, mtl_m4_inherit, link_host + link/, refl_host + refl/). Nothing is installed.
# Expected: Rb1 17 PASS, exit=0; Rb2 6 PASS 1 SKIP, exit=0 (Metal 3 binary functions + the tile refusal; Metal 4 off);
# Rb3 18 PASS, exit=0; Rb4 14 PASS; Rb5 42 PASS; Rb6 PASS (0 of 12 replaced); Rb7 20 PASS.
# Same rules as phase R: tmo (bash) and the DYLD_* variables exported in the subshell, no perl/env wrapper; link_host and
# refl_host make no device and stop with exit=2 "another NVMTLDriver" if one is loaded anyway.
phase_mtl4b() {
  local D=~/b580/mtl4b R="$O/mtl4b"; mkdir -p "$R"
  local S="NVMTL_ENABLE=1 NVMTL_BUNDLE=$D/NVMTLDriver.bundle DYLD_INSERT_LIBRARIES=$D/nvmtl_interpose.dylib"
  local M4="NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1"
  say "Rb: binary functions / dynamic linking / refusals - $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/1.log"; tmo 60 ./link_host "$D/NVMTLDriver.bundle" "$D/link" > "$R/1-link-host.txt" 2>&1; echo "exit=$?" >> "$R/1-link-host.txt" )
  say "Rb1 link_host (no GPU; want 17 PASS, exit=0): $(grep -m1 'another NVMTLDriver\|setup:' "$R/1-link-host.txt" | cut -c1-200) $(tail -2 "$R/1-link-host.txt" | tr '\n' ' ') FAIL lines $(grep -c '^FAIL' "$R/1-link-host.txt")"
  ( cd "$D" && export $S NVMTL_LOG_PATH="$R/2.log"; tmo 60 ./mtl4_linking > "$R/2-linking-off.txt" 2>&1; echo "exit=$?" >> "$R/2-linking-off.txt" )
  say "Rb2 mtl4_linking, Metal 4 off (want 6 PASS 1 SKIP, exit=0): $(tail -2 "$R/2-linking-off.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/2-linking-off.txt" | cut -c1-160 | head -3 | tr '\n' ';')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/3.log"; tmo 90 ./mtl4_linking > "$R/3-linking-on.txt" 2>&1; echo "exit=$?" >> "$R/3-linking-on.txt" )
  say "Rb3 mtl4_linking, NVMTL_METAL4=1 (want 18 PASS, exit=0): $(tail -2 "$R/3-linking-on.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/3-linking-on.txt" | cut -c1-160 | head -4 | tr '\n' ';')"
  say "Rb3 notes: $(grep -hE 'link: |link FAILED|REFUSED|AdditionalBinaryFunctions|newDynamicLibrary|dylib' "$R/3.log" 2>/dev/null | cut -c1-200 | head -8 | tr '\n' ';')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/4.log"; tmo 60 ./mtl4_compiler > "$R/4-comp-on.txt" 2>&1; echo "exit=$?" >> "$R/4-comp-on.txt" )
  say "Rb4 mtl4_compiler, NVMTL_METAL4=1 (want 14 PASS): $(tail -2 "$R/4-comp-on.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S $M4 NVMTL_LOG_PATH="$R/5.log"; tmo 60 ./mtl_reflection > "$R/5-refl-on.txt" 2>&1; echo "exit=$?" >> "$R/5-refl-on.txt" )
  say "Rb5 mtl_reflection, NVMTL_METAL4=1 (want 42 PASS): $(tail -2 "$R/5-refl-on.txt" | tr '\n' ' ')"
  ( cd "$D" && export $S; tmo 30 ./mtl_m4_inherit "$D/NVMTLDriver.bundle" > "$R/6-inherit.txt" 2>&1; echo "exit=$?" >> "$R/6-inherit.txt" )
  say "Rb6 mtl_m4_inherit (no GPU; want PASS): $(grep -E '^(PASS|FAIL) \(|another NVMTLDriver' "$R/6-inherit.txt" | head -2 | cut -c1-200 | tr '\n' ' ') $(tail -1 "$R/6-inherit.txt")"
  ( cd "$D" && export $S; tmo 30 ./refl_host "$D/NVMTLDriver.bundle" "$D/refl" > "$R/7-refl-host.txt" 2>&1; echo "exit=$?" >> "$R/7-refl-host.txt" )
  say "Rb7 refl_host (no GPU; want 20 PASS, exit=0): $(tail -2 "$R/7-refl-host.txt" | tr '\n' ' ')"
  say "Rb: after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
