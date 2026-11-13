#!/bin/bash
# SPDX-License-Identifier: MIT
# night_desc.sh: night phase D - the Metal plugin's two descriptor models side by side on the B580
# (docs/design/DESCRIPTOR-MODEL.md sections 13 and 14). A staged plugin is loaded by the test processes only
# (NVMTL_BUNDLE + DYLD_INSERT_LIBRARIES=nvmtl_interpose.dylib); nothing is installed.
#
#   source night_desc.sh; phase_desc [stage dir]     from a night script that defines O (output dir) and say
#   bash night_desc.sh [stage dir]                   alone: O=~/night-desc-<time>, say = echo with the time
#
# Stage dir (default ~/b580/descm): NVMTLDriver.bundle (branch descmodel), nvmtl_interpose.dylib, and the x86_64 tests
# mtl_desc_paths, mtl_desc_cost, mtl_bind, mtl_tests, mtl_gpu_icb2; gl_bind is used when present.
# DESC_GB=1 also runs Geekbench 7 Metal once per model (~/b580/gb_compute_profile.sh with GB_BUNDLE/GB_INTERPOSE).
#
# Timeouts come from tmo, a bash function (a perl or env wrapper would be a protected binary that strips the DYLD_*
# variables, and the test would quietly run the installed plugin). Each run is time-boxed to 60 s (Geekbench: 15 min).
# Results: $O/desc/summary.txt (the say lines) and one directory per run with its output and plugin log.

type tmo >/dev/null 2>&1 || tmo() {   # tmo <seconds> <command...>: run it, kill it after <seconds>; its exit code (137 = killed)
    local s=$1; shift
    "$@" & local p=$!
    ( sleep "$s"; kill -9 "$p" 2>/dev/null ) & local w=$!
    wait "$p"; local rc=$?
    pkill -P "$w" sleep 2>/dev/null; kill "$w" 2>/dev/null; wait "$w" 2>/dev/null   # the watcher and its sleep
    return $rc
}
type resets >/dev/null 2>&1 || resets() { sysctl -n debug.intelxe.stats 2>/dev/null | sed -nE 's/.*GuC context resets ([0-9]+); GT resets ([0-9]+).*/\1 \2/p'; }

phase_desc() {
    local D=${1:-${DESC_STAGE:-$HOME/b580/descm}} R="$O/desc"
    mkdir -p "$R"
    local B="$D/NVMTLDriver.bundle" I="$D/nvmtl_interpose.dylib"
    [ -x "$B/Contents/MacOS/NVMTLDriver" ] && [ -f "$I" ] || { say "D: no staged plugin in $D - phase skipped"; return 0; }
    dsay() { say "$@"; echo "$*" >> "$R/summary.txt"; }
    dsay "D: descriptor models on $(shasum -a 256 "$B/Contents/MacOS/NVMTLDriver" | cut -c1-12), resets before: $(resets)"
    # run <tag> <log name> <env...> -- <command...>: in a subshell with the staged plugin and the given knobs
    run() {
        local tag=$1; shift; local envs=()
        while [ "$1" != "--" ]; do envs+=("$1"); shift; done; shift
        mkdir -p "$R/$tag"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" DYLD_INSERT_LIBRARIES="$I" NVMTL_LOG_PATH="$R/$tag/nvmtl.log" "${envs[@]}"
          cd "$D" && tmo 60 "$@" ) > "$R/$tag/out.txt" 2>&1
        echo "exit=$?" >> "$R/$tag/out.txt"
    }
    # what the plugin log says about the model and its cost
    logsum() {
        local L="$R/$1/nvmtl.log"
        printf 'model %s; legacy %s; table sets %s; %s' "$(grep -m1 -oE 'descriptor model (BUFFER|CLASSIC)[^;]*' "$L" | cut -c1-60)" \
            "$(grep -c 'LEGACY' "$L" 2>/dev/null)" "$(grep -c 'table set (set' "$L" 2>/dev/null)" \
            "$(grep -h 'DESCSTAT-TOTAL' "$L" | tail -1 | grep -oE '[0-9]+ draws \+ [0-9]+ dispatches bound, [0-9]+ ns each|per bind: .*' | tr '\n' ' ')"
    }
    local M

    # D1 binding paths: every case PASS in both models (SKIP: tile, mesh where the device has none)
    for M in classic buffer; do
        run "paths-$M" NVMTL_DESC_MODEL=$M NVMTL_DESC_STATS=1 -- ./mtl_desc_paths
        dsay "D1 mtl_desc_paths $M: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-$M/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-$M/out.txt" | cut -c6-22 | tr '\n' ' ')"
        dsay "D1   $(logsum "paths-$M")"
    done
    run "paths-buffer-nokeep" NVMTL_DESC_MODEL=buffer NVMTL_DB_KEEP=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer NVMTL_DB_KEEP=0: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-nokeep/out.txt" | tr '\n' ' ')"
    # the table set (13 Nov, branch dbuftable): with table-pc off every stage that uses a buffer reads the address table,
    # so every case runs through a table set; NVMTL_DB_TABSET=0 is the table in the stage region (before)
    run "paths-buffer-nopc" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer, table-pc off: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-nopc/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-buffer-nopc/out.txt" | cut -c6-22 | tr '\n' ' ')"
    dsay "D1   $(logsum paths-buffer-nopc)"
    run "paths-buffer-intable" NVMTL_DESC_MODEL=buffer NVMTL_DB_TABSET=0 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer, table-pc off, NVMTL_DB_TABSET=0: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-intable/out.txt" | tr '\n' ' ')"

    # D2 CPU cost per draw: two rounds, models alternating; then the buffer model without its 12 Nov parts
    local k
    for k in 1 2; do for M in classic buffer; do
        run "cost-$M-$k" NVMTL_DESC_MODEL=$M NVMTL_DESC_STATS=1 -- ./mtl_desc_cost -f 30
    done; done
    run "cost-buffer-nokeep" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_DB_KEEP=0 -- ./mtl_desc_cost -f 30
    run "cost-buffer-nopc" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_cost -f 30
    run "cost-classic-nopc" NVMTL_DESC_MODEL=classic NVMTL_DESC_STATS=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_cost -f 30
    {
        printf '%-8s %9s %9s %9s %9s %7s %9s %9s %s\n' mode classic buffer nokeep buf-nopc delta cpu-cls cpu-buf image
        for m in same small bufoff tex4 heavy argbuf pso unread compute geek; do
            local c b n p cc cb hc hb
            c=$(cat "$R"/cost-classic-[12]/out.txt | awk -v m=$m '$2==m {print $10}' | sort -n | head -1)
            b=$(cat "$R"/cost-buffer-[12]/out.txt | awk -v m=$m '$2==m {print $10}' | sort -n | head -1)
            n=$(awk -v m=$m '$2==m {print $10}' "$R/cost-buffer-nokeep/out.txt"); p=$(awk -v m=$m '$2==m {print $10}' "$R/cost-buffer-nopc/out.txt")
            cc=$(cat "$R"/cost-classic-[12]/out.txt | awk -v m=$m '$2==m {for(i=1;i<NF;i++) if($i=="CPU") print $(i+1)}' | sort -n | head -1)
            cb=$(cat "$R"/cost-buffer-[12]/out.txt | awk -v m=$m '$2==m {for(i=1;i<NF;i++) if($i=="CPU") print $(i+1)}' | sort -n | head -1)
            hc=$(awk -v m=$m '$2==m {print $NF}' "$R/cost-classic-1/out.txt"); hb=$(awk -v m=$m '$2==m {print $NF}' "$R/cost-buffer-1/out.txt")
            local dl="-"; [ -n "$c" ] && [ -n "$b" ] && [ "$c" != 0 ] && dl=$(awk -v c="$c" -v b="$b" 'BEGIN{printf "%+.0f%%", (b-c)*100/c}')
            # geek: the last field is the output checksum, compared like an image hash
            local im=SAME; [ "$m" = compute ] && im="-"; [ "$m" != compute ] && [ "$hc" != "$hb" ] && im="DIFF($hc/$hb)"
            printf '%-8s %9s %9s %9s %9s %7s %9s %9s %s\n' $m "${c:--}" "${b:--}" "${n:--}" "${p:--}" "$dl" "${cc:--}" "${cb:--}" "$im"
        done
    } > "$R/cost-table.txt"
    dsay "D2 mtl_desc_cost, encode ns/draw p50 (best of 2), process CPU ns/draw:"
    while read -r line; do dsay "D2   $line"; done < "$R/cost-table.txt"
    for M in classic buffer buffer-nokeep buffer-nopc classic-nopc; do
        local t=cost-$M; [ -d "$R/$t" ] || t=cost-$M-1
        dsay "D2   $M: $(logsum "$t")"
    done
    # D2b the Geekbench-like compute chain alone, so its DESCSTAT totals are its own: regions per bind must be near 0 in
    # the buffer model with keep (13 Nov: every recent region survives a change it does not touch); NVMTL_DB_KEEP=cur is
    # the 12 Nov rule (only the current region), which wrote a region per dispatch
    # -nopc: table-pc off, so all three kernels read the address table (the "4 of 4" column of dbuf_model geek): with the
    # table set (13 Nov) regions per bind must stay near 0 and tables per bind near 1; -intable (NVMTL_DB_TABSET=0) is the
    # table in the stage region, a region per dispatch
    for M in classic buffer buffer-keepcur classic-nopc buffer-nopc buffer-nopc-intable; do
        local mm=${M%%-*} kp= ex=()
        [ "$M" = buffer-keepcur ] && kp=cur
        case $M in *-nopc*) ex+=(NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0);; esac
        case $M in *-intable) ex+=(NVMTL_DB_TABSET=0);; esac
        run "geek-$M" NVMTL_DESC_MODEL=$mm NVMTL_DESC_STATS=1 ${kp:+NVMTL_DB_KEEP=$kp} "${ex[@]}" -- ./mtl_desc_cost -f 30 -n 4000 geek
        dsay "D2b geek $M: $(grep '^cost geek' "$R/geek-$M/out.txt" | awk '{for(i=1;i<NF;i++) if($i=="p50") print $(i+1) " ns/dispatch, checksum " $NF}'); $(logsum "geek-$M")"
    done

    # D3 mtl_bind: every mode's image byte-identical between the models (it loads NVMTL_BUNDLE itself); buffer-nopc is the
    # buffer model with table-pc off (every table read through a table set)
    for M in classic buffer buffer-nopc; do
        mkdir -p "$R/bind-$M/img"
        local nopc=0; [ "$M" = buffer-nopc ] && nopc=1
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_DESC_MODEL=${M%%-*} NVMTL_LOG_PATH="$R/bind-$M/nvmtl.log"
          [ $nopc = 1 ] && export NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0
          cd "$D" && tmo 60 ./mtl_bind -f 20 -o "$R/bind-$M/img" ) > "$R/bind-$M/out.txt" 2>&1
    done
    local same=0 diff=0 dl="" V
    for V in buffer buffer-nopc; do
        same=0; diff=0; dl=""
        for f in "$R"/bind-classic/img/*; do
            [ -f "$f" ] || continue
            if cmp -s "$f" "$R/bind-$V/img/$(basename "$f")"; then same=$((same + 1)); else diff=$((diff + 1)); dl="$dl $(basename "$f")"; fi
        done
        dsay "D3 mtl_bind images classic vs $V: $same same, $diff different${dl:+ ($dl)}"
    done
    dsay "D3   ns/draw classic: $(awk '/enc\/draw/{printf "%s %s ", $1, $4}' "$R/bind-classic/out.txt")"
    dsay "D3   ns/draw buffer:  $(awk '/enc\/draw/{printf "%s %s ", $1, $4}' "$R/bind-buffer/out.txt")"

    # D4 mtl_tests, D5 GPU-encoded ICBs through DGC (now offered in the buffer model too)
    for M in classic buffer; do
        mkdir -p "$R/tests-$M"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_DESC_MODEL=$M NVMTL_LOG_PATH="$R/tests-$M/nvmtl.log"
          cd "$D" && tmo 60 ./mtl_tests ) > "$R/tests-$M/out.txt" 2>&1
        dsay "D4 mtl_tests $M: $(grep -c 'FAIL' "$R/tests-$M/out.txt") FAIL lines; $(tail -1 "$R/tests-$M/out.txt")"
        run "icb-$M" NVMTL_DESC_MODEL=$M NVMTL_ICB_DGC=1 -- ./mtl_gpu_icb2
        dsay "D5 mtl_gpu_icb2 NVMTL_ICB_DGC=1 $M: $(grep -c '^PASS' "$R/icb-$M/out.txt") PASS, $(grep -c '^FAIL' "$R/icb-$M/out.txt") FAIL, $(tail -1 "$R/icb-$M/out.txt"); dgc: $(grep -c 'icb-dgc: render token layout\|icb-dgc: compute token layout' "$R/icb-$M/nvmtl.log")"
    done

    # D6 Apple GL over the plugin (the Minecraft path), when gl_bind is staged
    if [ -x "$D/gl_bind" ]; then
        for M in classic buffer; do
            mkdir -p "$R/gl-$M/img"
            run "gl-$M" NVMTL_DESC_MODEL=$M NVMTL_DESC_STATS=1 -- ./gl_bind -f 20 -o "$R/gl-$M/img"
            dsay "D6 gl_bind $M: $(grep '^gl-' "$R/gl-$M/out.txt" | cut -c1-90 | tr '\n' ';') $(logsum "gl-$M")"
        done
        same=0; diff=0
        for f in "$R"/gl-classic/img/*; do [ -f "$f" ] || continue; cmp -s "$f" "$R/gl-buffer/img/$(basename "$f")" && same=$((same + 1)) || diff=$((diff + 1)); done
        dsay "D6 gl_bind images classic vs buffer: $same same, $diff different"
    fi

    # D7 Geekbench 7 Metal per model (DESC_GB=1): score and the plugin's descriptor totals
    if [ "${DESC_GB:-0}" = 1 ] && [ -x "$HOME/b580/gb_compute_profile.sh" ]; then
        for M in classic buffer; do
            local G="$R/gb-$M"; rm -rf "$G"
            GB_BUNDLE="$B" GB_INTERPOSE="$I" GB_ENV="NVMTL_PROCESS_ENV=1 NVMTL_ENABLE=1 NVMTL_DESC_MODEL=$M NVMTL_DESC_STATS=1" \
                "$HOME/b580/gb_compute_profile.sh" start Metal "$G" plain > /dev/null 2>&1
            local n=0; while kill -0 "$(cat "$G/pid" 2>/dev/null)" 2>/dev/null && [ $n -lt 180 ]; do sleep 5; n=$((n + 1)); done
            kill -0 "$(cat "$G/pid" 2>/dev/null)" 2>/dev/null && { kill -9 "$(cat "$G/pid")"; dsay "D7 Geekbench $M: KILLED after 15 min"; }
            dsay "D7 Geekbench Metal $M: $(grep -m1 'GPU Score' "$G/gb.txt" | awk '{print $3}'); $(logsum "gb-$M" | cut -c1-200)"
        done
    fi
    dsay "D: resets after: $(resets) (before: see the first D line)"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    O=${O:-$HOME/night-desc-$(date +%m%d-%H%M)}; mkdir -p "$O"
    type say >/dev/null 2>&1 || say() { echo "$(date +%H:%M:%S) $*"; }
    phase_desc "$@"
fi
