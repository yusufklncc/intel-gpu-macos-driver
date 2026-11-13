#!/bin/bash
# SPDX-License-Identifier: MIT
# night_icbc.sh: night phase C (branch icbc) - compute ICBs whose kernels' buffers a shader sets, through
# VK_EXT_device_generated_commands on the B580 (docs/design/DESIGN-icb-dgc.md section 10: the kernel read buffer 0 through
# the CPU's descriptor; fixed by lowering ICB pipelines with push constants as driver values, and a plan guard).
#
#   source night_icbc.sh; phase_icbc [stage dir]     from a night script that defines O (output dir) and say
#   bash night_icbc.sh [stage dir]                   alone: O=~/night-icbc-<time>, say = echo with the time
#
# Stage dir (default ~/b580/icbc): NVMTLDriver.bundle, nvmtl_interpose.dylib and the x86_64 tests mtl_gpu_icb2,
# mtl_icb_kept, mtl_gpu_icb3, mtl_gpu_icb. The staged plugin is loaded by the test processes only
# (NVMTL_BUNDLE + DYLD_INSERT_LIBRARIES, exported inside a subshell); nothing is installed.
#
# Expected (B580):
#   C1 mtl_gpu_icb2 NVMTL_ICB_DGC=1: 12 PASS, exit=0; diagnostic scratch[0]=0
#   C2 mtl_icb_kept NVMTL_ICB_DGC=1: K0 K1 K2 PASS (K1 is the refusal: out 0 and scratch1 0), exit=0
#   C3 the same with NVMTL_ICB_DGC_LOWER=0 (the old lowering, with the new guard): mtl_gpu_icb2 C0-C2 FAIL as refusals
#      (scratch 0, "NOT run" lines), mtl_icb_kept K0 FAIL (refused, scratch0 0), K1 K2 PASS
#   C4 regressions: mtl_gpu_icb3 knob on 16 PASS; mtl_gpu_icb off / on 6/6; mtl_gpu_icb2 and mtl_icb_kept knob off:
#      REFUSED, exit=2
#
# Timeouts come from tmo, a bash function (a perl or env wrapper would be a protected binary that strips the DYLD_*
# variables, and the test would quietly run the installed plugin). Each run is time-boxed to 60 s.

type tmo >/dev/null 2>&1 || tmo() {   # tmo <seconds> <command...>: run it, kill it after <seconds>; its exit code (137 = killed)
    local s=$1; shift
    "$@" & local p=$!
    ( sleep "$s"; kill -9 "$p" 2>/dev/null ) & local w=$!
    wait "$p"; local rc=$?
    pkill -P "$w" sleep 2>/dev/null; kill "$w" 2>/dev/null; wait "$w" 2>/dev/null   # the watcher and its sleep
    return $rc
}
type resets >/dev/null 2>&1 || resets() { sysctl -n debug.intelxe.stats 2>/dev/null | sed -nE 's/.*GuC context resets ([0-9]+); GT resets ([0-9]+).*/\1 \2/p'; }

phase_icbc() {
    local D=${1:-${ICBC_STAGE:-$HOME/b580/icbc}} R="$O/icbc"
    mkdir -p "$R"
    local B="$D/NVMTLDriver.bundle" I="$D/nvmtl_interpose.dylib"
    [ -x "$B/Contents/MacOS/NVMTLDriver" ] && [ -f "$I" ] || { say "C: no staged plugin in $D - phase skipped"; return 0; }
    csay() { say "$@"; echo "$*" >> "$R/summary.txt"; }
    csay "C: compute ICBs through DGC on $(shasum -a 256 "$B/Contents/MacOS/NVMTLDriver" | cut -c1-12), resets before: $(resets)"
    # run <tag> <env...> -- <command...>: in a subshell with the staged plugin and the given knobs
    run() {
        local tag=$1; shift; local envs=()
        while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
        mkdir -p "$R/$tag"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" DYLD_INSERT_LIBRARIES="$I" NVMTL_LOG_PATH="$R/$tag/nvmtl.log" "${envs[@]}"
          cd "$D" && tmo 60 "$@" ) > "$R/$tag/out.txt" 2>&1
        echo "exit=$?" >> "$R/$tag/out.txt"
    }
    res() { printf '%s PASS; FAIL: %s %s' "$(grep -c '^PASS' "$R/$1/out.txt")" "$(grep '^FAIL' "$R/$1/out.txt" | cut -c6-8 | tr '\n' ' ')" "$(tail -1 "$R/$1/out.txt")"; }
    notes() { grep -hE 'kernels of GPU-encodable ICBs|compute token layout|NOT run|after the execute|cannot express' "$R/$1/nvmtl.log" 2>/dev/null \
              | sed 's/^pid [0-9]* NVMTL: //' | cut -c1-200 | head -${2:-6} | tr '\n' ';'; }

    run icb2-on NVMTL_ICB_DGC=1 -- ./mtl_gpu_icb2
    csay "C1 mtl_gpu_icb2 NVMTL_ICB_DGC=1 (want 12 PASS, exit=0, scratch 0): $(res icb2-on) | $(grep -m1 '^diagnostic' "$R/icb2-on/out.txt" | cut -c13-50)"
    csay "C1   $(grep -E '^(PASS|FAIL) C' "$R/icb2-on/out.txt" | cut -c1-7 | tr '\n' ';')"
    csay "C1   log: $(notes icb2-on 8)"
    run kept-on NVMTL_ICB_DGC=1 -- ./mtl_icb_kept
    csay "C2 mtl_icb_kept NVMTL_ICB_DGC=1 (want K0 K1 K2 PASS, K1 refused): $(res kept-on)"
    csay "C2   $(grep -E '^pass 1' "$R/kept-on/out.txt")"
    csay "C2   log: $(notes kept-on 6)"
    run icb2-oldlower NVMTL_ICB_DGC=1 NVMTL_ICB_DGC_LOWER=0 -- ./mtl_gpu_icb2
    csay "C3 mtl_gpu_icb2 NVMTL_ICB_DGC_LOWER=0 (want C0-C2 FAIL as refusals, scratch 0): $(res icb2-oldlower) | $(grep -m1 '^diagnostic' "$R/icb2-oldlower/out.txt" | cut -c13-50)"
    run kept-oldlower NVMTL_ICB_DGC=1 NVMTL_ICB_DGC_LOWER=0 -- ./mtl_icb_kept
    csay "C3 mtl_icb_kept NVMTL_ICB_DGC_LOWER=0 (want K0 FAIL refused, K1 K2 PASS): $(res kept-oldlower) | $(grep -E '^pass 1' "$R/kept-oldlower/out.txt")"
    csay "C3   log: $(notes kept-oldlower 4)"
    run icb3-on NVMTL_ICB_DGC=1 -- ./mtl_gpu_icb3
    csay "C4 mtl_gpu_icb3 NVMTL_ICB_DGC=1 (want 16 PASS): $(res icb3-on)"
    run icb-off -- ./mtl_gpu_icb
    run icb-on NVMTL_ICB_DGC=1 -- ./mtl_gpu_icb
    csay "C4 mtl_gpu_icb off / on (want 6/6 each): $(grep -c '^PASS' "$R/icb-off/out.txt") / $(grep -c '^PASS' "$R/icb-on/out.txt") PASS"
    run icb2-off -- ./mtl_gpu_icb2
    run kept-off -- ./mtl_icb_kept
    csay "C4 knob off (want REFUSED exit=2 each): mtl_gpu_icb2 $(tail -1 "$R/icb2-off/out.txt"), mtl_icb_kept $(tail -1 "$R/kept-off/out.txt")"
    csay "C: after: resets $(resets)"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    O=${O:-$HOME/night-icbc-$(date +%m%d-%H%M)}; mkdir -p "$O"
    type say >/dev/null 2>&1 || say() { echo "$(date +%H:%M:%S) $*" | tee -a "$O/say.txt"; }
    phase_icbc "$@"
fi
