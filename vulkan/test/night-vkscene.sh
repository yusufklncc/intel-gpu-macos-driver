# SPDX-License-Identifier: MIT
# night-vkscene.sh: the native Vulkan renderer-path phase for the test machine's night script (sourced; defines
# phase_vkscene). Nothing is installed: the staged provider is reached through DYLD_LIBRARY_PATH (a bash subshell with
# export, never perl/env/sh, which drop DYLD_* at their start), ANV directly with --icd, the Khronos loader with
# VK_DRIVER_FILES. Every step is time-boxed with tmo (60 s at most).
#   phase_vkscene [STAGE]   STAGE: the vulkan/ folder with build.sh's output in it (lib/ bin/ icd.d/ test/),
#                           default ~/b580/vkgames
# Uses the night script's say, tmo and $O when it defines them (standalone: prints, a bash timeout, ./vkscene-out).
# Results: $O/vkscene/*.txt, PNGs of the read-back frames, frame logs; one "say" line per step.
type say >/dev/null 2>&1 || say() { echo "$(date +%H:%M:%S) $*"; }
type tmo >/dev/null 2>&1 || tmo() { local s=$1; shift; "$@" & local p=$!; ( trap 'kill $sp 2>/dev/null; exit 0' TERM; sleep "$s" & sp=$!; wait $sp; kill -9 $p 2>/dev/null ) >/dev/null 2>&1 & local w=$!; wait $p; local r=$?; kill $w 2>/dev/null; wait $w 2>/dev/null; return $r; }

phase_vkscene() {
  local P=${1:-$HOME/b580/vkgames} R="${O:-$PWD/vkscene-out}/vkscene" ICDJ X L M f
  set -o pipefail
  mkdir -p "$R"
  X=$P/bin/vkscene; ICDJ=$P/icd.d/intel_icd.x86_64.json
  [ -x "$X" ] || { say "VS: no $X"; return 1; }
  xattr -dr com.apple.quarantine "$P" 2>/dev/null
  # the staged provider under both names Wine and games open (not the installed one)
  L=$P/provider-stage; rm -rf "$L"; mkdir -p "$L"
  ln -s ../lib/libintelxe-vk.dylib "$L/libvulkan.1.dylib"; ln -s ../lib/libintelxe-vk.dylib "$L/libMoltenVK.dylib"
  stats() { sysctl -n debug.intelxe.stats 2>/dev/null | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+' || echo '?'; }
  last() { grep -E '^(RESULT|PASS|FAIL)' "$1" | tail -2 | tr '\n' ' '; }
  say "VS: vkscene (MSAA, depth, bindless, timelines) - stats before: $(stats)"

  # 1. ANV directly (ICD interface), FIFO, then IMMEDIATE for throughput
  tmo 60 "$X" --frames 300 --png "$R/1-anv.png" > "$R/1-anv.txt" 2>&1; say "VS1 ANV direct FIFO: $(last "$R/1-anv.txt")"
  tmo 60 "$X" --frames 600 --immediate > "$R/1b-anv-immediate.txt" 2>&1; say "VS1b ANV direct IMMEDIATE: $(last "$R/1b-anv-immediate.txt")"
  # 2. through the provider by name, with the frame log (and its own debug log of the extensions)
  ( export DYLD_LIBRARY_PATH=$L INTELXE_VK_FRAMELOG="$R/ft-provider-%p.txt" INTELXE_VK_DEBUG=1
    tmo 60 "$X" --lib libvulkan.1.dylib --frames 300 --png "$R/2-provider.png" ) > "$R/2-provider.txt" 2>&1
  say "VS2 provider FIFO: $(last "$R/2-provider.txt")"
  ( export DYLD_LIBRARY_PATH=$L INTELXE_VK_FRAMELOG="$R/ft-provider-imm-%p.txt"
    tmo 60 "$X" --lib libMoltenVK.dylib --frames 600 --immediate --mtl-device ) > "$R/2b-provider-immediate.txt" 2>&1
  say "VS2b provider IMMEDIATE (as libMoltenVK.dylib, Wine-like layer with a Metal device): $(last "$R/2b-provider-immediate.txt")"
  ( export DYLD_LIBRARY_PATH=$L; tmo 60 "$X" --lib libvulkan.1.dylib --frames 200 --resize ) > "$R/2c-provider-resize.txt" 2>&1
  say "VS2c provider + resize: $(last "$R/2c-provider-resize.txt")"
  # 3. the Khronos loader with ANV's manifest
  if [ -e /usr/local/lib/libvulkan.1.dylib ]; then
    ( export VK_DRIVER_FILES=$ICDJ; tmo 60 "$X" --lib /usr/local/lib/libvulkan.1.dylib --frames 300 ) > "$R/3-loader.txt" 2>&1
    say "VS3 loader FIFO: $(last "$R/3-loader.txt")"
  else say "VS3 loader: no /usr/local/lib/libvulkan.1.dylib, skipped"; fi
  # 4. the same frame on three paths to the same driver must give the same pixels
  local sums; sums=$(grep -h -o 'scene checksum 0x[0-9a-f]*' "$R/1-anv.txt" "$R/2-provider.txt" "$R/3-loader.txt" 2>/dev/null | sort | uniq -c | tr '\n' ' ')
  say "VS4 checksums (ANV, provider, loader; one value expected): $sums"
  # 5. the frame log against vkscene's own intervals
  for f in "$R"/ft-provider-[0-9]*.txt "$R"/ft-provider-imm-[0-9]*.txt; do
    [ -f "$f" ] || continue
    say "VS5 $(basename "$f"): $("$P/intelxe-vk" frametimes "$f" --skip 1 | grep '^fps')"
  done
  say "VS5 vkscene's own: FIFO $(grep -o 'fps=[0-9.]*' "$R/2-provider.txt" | head -1), IMMEDIATE $(grep -o 'fps=[0-9.]*' "$R/2b-provider-immediate.txt" | head -1)"
  # 6. A/B: the same front end over MoltenVK, if an x86_64 MoltenVK is on the machine (Wine builds, LWJGL's natives)
  M=""
  while IFS= read -r f; do case "$(file -b "$f")" in *x86_64*) M=$f; break;; esac; done < <(
    find /Applications "$HOME/Applications" "$HOME/Library/Application Support/minecraft" /usr/local/lib -name libMoltenVK.dylib 2>/dev/null | head -20)
  if [ -n "$M" ]; then
    mkdir -p "$R/mvk"; ln -sf "$M" "$R/mvk/libMoltenVK-icd.dylib"   # another leaf name: DYLD_LIBRARY_PATH holds the provider's
    ( export DYLD_LIBRARY_PATH=$L INTELXE_VK_ICD="$R/mvk/libMoltenVK-icd.dylib" INTELXE_VK_FRAMELOG="$R/ft-mvk-%p.txt"
      tmo 60 "$X" --lib libvulkan.1.dylib --frames 600 --immediate ) > "$R/6-mvk-immediate.txt" 2>&1
    say "VS6 provider -> MoltenVK ($M) IMMEDIATE: $(last "$R/6-mvk-immediate.txt")"
  else say "VS6 MoltenVK A/B: no x86_64 libMoltenVK.dylib found, skipped"; fi
  # 7. offline checks of the stage (no GPU): the provider against a stand-in driver, the bench harness, Minecraft notes
  tmo 60 sh "$P/test/run.sh" "$P" > "$R/7-provtest.txt" 2>&1; say "VS7 provider offline test: $(grep -c '^ok' "$R/7-provtest.txt") ok, $(grep -c '^FAIL' "$R/7-provtest.txt") FAIL"
  tmo 60 "$P/test/bench-test.sh" > "$R/7-benchtest.txt" 2>&1; say "VS7 bench harness test: $(tail -1 "$R/7-benchtest.txt")"
  INTELXE_VK_ROOT=$P "$P/intelxe-vk" minecraft-args > "$R/7-minecraft.txt" 2>&1; say "VS7 minecraft-args: $(grep -c 'library validation off\|lets it load\|no hardened runtime: loads' "$R/7-minecraft.txt") Java runtime(s) able to load it"
  say "VS: stats after: $(stats)"
}
