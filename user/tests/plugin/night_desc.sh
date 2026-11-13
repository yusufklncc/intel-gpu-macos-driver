#!/bin/bash
# SPDX-License-Identifier: MIT
# night_desc.sh: night phase D - the Metal plugin's two descriptor models side by side on the B580
# (docs/design/DESCRIPTOR-MODEL.md sections 13 and 14; the dbuftess checks: tessellation and set-1 kernels in table sets;
# dbufgeek: kernels' storage buffers with pushed offsets, NVMTL_DB_BUFOFF; dbufgb: literal words no longer refuse table-pc
# or bufoff, NVMTL_PC_EXACT; buffer-only regions survive a destroyed view or sampler, NVMTL_DB_OBJKEEP; dbufdef: bufoff
# off by default, NVMTL_DB_BUFOFF=1|fit, the plugin built with the buffer model as its default (DESC_DEFAULT=buffer), so a
# run without NVMTL_DESC_MODEL is the buffer model and classic is NVMTL_DESC_MODEL=classic; D8 runs every staged Metal test
# and regress.sh's Metal stages in both models). A staged plugin is loaded by the test processes only
# (NVMTL_BUNDLE + DYLD_INSERT_LIBRARIES=nvmtl_interpose.dylib); nothing is installed.
#
#   source night_desc.sh; phase_desc [stage dir]     from a night script that defines O (output dir) and say
#   bash night_desc.sh [stage dir]                   alone: O=~/night-desc-<time>, say = echo with the time
#
# Stage dir (default ~/b580/descm): NVMTLDriver.bundle (branch descmodel), nvmtl_interpose.dylib, and the x86_64 tests
# mtl_desc_paths, mtl_desc_cost, mtl_bind, mtl_tests, mtl_gpu_icb2; gl_bind is used when present.
# DESC_GB=1 also runs Geekbench 7 Metal (~/b580/gb_compute_profile.sh with GB_BUNDLE/GB_INTERPOSE): DESC_GB_ROUNDS rounds
# (default 3) of DESC_GB_VARIANTS (default "classic buffer buffer-fit"), alternating, and the median of each.
# D8 also takes the other x86_64 tests of build-intelxe.sh tests when they are staged (mtl_smoke, mtl_cube_read, mtl_props,
# mtl_f32filter, mtl_timestamps, mtl_gpu_icb, mtl_gpu_icb3, mtl_icb_cost, mtl_lod_pull, mtl_reflection, mtl_sparse,
# mtl_sparse_buffer, mtl_sparse_tier2, mtl4_compiler, mtl4_linking, mtl_m4_inherit) and regress.sh (its parsers).
# Safe under bash 3.2 with set -u (empty arrays are expanded as ${a[@]+"${a[@]}"}).
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
    # run <tag> <log name> <env...> -- <command...>: in a subshell with the staged plugin and the given knobs. NODYLD=1
    # among the knobs: no DYLD_INSERT_LIBRARIES (a test that loads the bundle itself, as mtl_tests and mtl_m4_inherit do)
    run() {
        local tag=$1; shift; local envs=() dy="$I"
        while [ "$1" != "--" ]; do if [ "$1" = NODYLD=1 ]; then dy=""; else envs+=("$1"); fi; shift; done; shift
        mkdir -p "$R/$tag"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_LOG_PATH="$R/$tag/nvmtl.log" ${envs[@]+"${envs[@]}"}
          [ -n "$dy" ] && export DYLD_INSERT_LIBRARIES="$dy"
          cd "$D" && tmo "${RUN_TMO:-60}" "$@" ) > "$R/$tag/out.txt" 2>&1
        echo "exit=$?" >> "$R/$tag/out.txt"
    }
    # what the plugin log says about the model and its cost. legacy: the buffer model's own LEGACY lines (until dbufgeek
    # this counted any line with LEGACY, and the heap pin's "NVMTL_HEAP_PIN_LEGACY" made every run say 1); bufoff: the
    # process total of DESCSTAT-KERNELS (dbufgb; written at exit with NVMTL_DESC_STATS=1), else the last "converted so far"
    # count (logged for the first kernels and at powers of two only: the 13 Nov night's "0 of 16" was Geekbench's first 16)
    logsum() {
        local L="$R/$1/nvmtl.log" bo
        bo=$(grep -h 'DESCSTAT-KERNELS' "$L" 2>/dev/null | tail -1 | grep -oE 'bufoff [0-9]+ of [0-9]+' | cut -c8-)
        [ -n "$bo" ] || bo=$(grep -oE 'bufoff: .*\(([0-9]+ of [0-9]+) kernels converted' "$L" 2>/dev/null | tail -1 | grep -oE '[0-9]+ of [0-9]+' || echo -)
        printf 'model %s; legacy %s; table sets %s; tess tables in set 4 %s; free-set kernels %s; bufoff %s; %s' \
            "$(grep -m1 -oE 'descriptor model (BUFFER|CLASSIC)[^;]*' "$L" | cut -c1-60)" \
            "$(grep -c 'descriptor buffer: LEGACY' "$L" 2>/dev/null)" "$(grep -c 'table set (set' "$L" 2>/dev/null)" \
            "$(grep -c "post-tessellation function's address table in set 4" "$L" 2>/dev/null)" \
            "$(grep -cE 'a kernel (that names set 1|under NVMTL_DB_CS_TABSET): its address table \(from set' "$L" 2>/dev/null)" \
            "$bo" \
            "$(grep -h 'DESCSTAT-TOTAL' "$L" | tail -1 | grep -oE '[0-9]+ draws \+ [0-9]+ dispatches bound, [0-9]+ ns each|per bind: .*' | tr '\n' ' ')"
    }
    local M

    # D0 the switch (dbufdef): this bundle is built with DESC_DEFAULT=buffer, so a process that names no model starts in
    # the buffer model ("asked by default (a build with DESC_DEFAULT=buffer)"), and NVMTL_DESC_MODEL=classic still gives
    # classic. A bundle built without it says CLASSIC (default) here.
    run "paths-default" NVMTL_DESC_STATS=1 -- ./mtl_desc_paths
    dsay "D0 default model: $(grep -m1 -oE 'descriptor model (BUFFER asked by [^;]*|CLASSIC \([^)]*\))' "$R/paths-default/nvmtl.log" | cut -c1-90); mtl_desc_paths $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-default/out.txt" | tr '\n' ' ')"
    # D1 binding paths: every case PASS in both models (SKIP: tile, mesh where the device has none)
    for M in classic buffer; do
        run "paths-$M" NVMTL_DESC_MODEL=$M NVMTL_DESC_STATS=1 -- ./mtl_desc_paths
        dsay "D1 mtl_desc_paths $M: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-$M/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-$M/out.txt" | cut -c6-22 | tr '\n' ' ')"
        dsay "D1   $(logsum "paths-$M")"
    done
    run "paths-buffer-nokeep" NVMTL_DESC_MODEL=buffer NVMTL_DB_KEEP=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer NVMTL_DB_KEEP=0: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-nokeep/out.txt" | tr '\n' ' ')"
    # dbufgeek/dbufdef: bufoff is off by default now; on (=1, every kernel the pass takes) and fit (only kernels whose push
    # data ANV reads from memory already) must pass as well (cs-bufoff and cs-chain either way)
    for M in bufoff fit; do
        local bv=1; [ $M = fit ] && bv=fit
        run "paths-buffer-$M" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_DB_BUFOFF=$bv -- ./mtl_desc_paths
        dsay "D1 mtl_desc_paths buffer NVMTL_DB_BUFOFF=$bv: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-$M/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-buffer-$M/out.txt" | cut -c6-22 | tr '\n' ' ')"
        dsay "D1   $(logsum "paths-buffer-$M")"
    done
    # dbufgb: the rules before 13 Nov night (literal words refuse table-pc, every destroy drops every region); both must pass
    run "paths-buffer-oldrules" NVMTL_DESC_MODEL=buffer NVMTL_PC_EXACT=0 NVMTL_DB_OBJKEEP=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer NVMTL_PC_EXACT=0 NVMTL_DB_OBJKEEP=0: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-oldrules/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-buffer-oldrules/out.txt" | cut -c6-22 | tr '\n' ' ')"
    # the table set (13 Nov, branch dbuftable): with table-pc off every stage that uses a buffer reads the address table,
    # so every case runs through a table set; NVMTL_DB_TABSET=0 is the table in the stage region (before)
    run "paths-buffer-nopc" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer, table-pc off: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-nopc/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-buffer-nopc/out.txt" | cut -c6-22 | tr '\n' ' ')"
    dsay "D1   $(logsum paths-buffer-nopc)"
    run "paths-buffer-intable" NVMTL_DESC_MODEL=buffer NVMTL_DB_TABSET=0 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_desc_paths
    dsay "D1 mtl_desc_paths buffer, table-pc off, NVMTL_DB_TABSET=0: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-intable/out.txt" | tr '\n' ' ')"
    # dbuftess: tess and tess-mix put the post-tessellation function's table in set 4 (every run above); cstab sends every
    # table kernel to a free set from 3, the path of a kernel whose module names set 1 (no translated kernel does), and
    # cstab7 to set 7, the last one ANV has; base is the rule before (tessellation and set-1 kernels: table in the region)
    for M in cstab cstab7 base; do
        local ex=(NVMTL_DB_CS_TABSET=3); [ $M = cstab7 ] && ex=(NVMTL_DB_CS_TABSET=7); [ $M = base ] && ex=(NVMTL_DB_TABSET=base)
        run "paths-buffer-$M" NVMTL_DESC_MODEL=buffer NVMTL_DESC_STATS=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 ${ex[@]+"${ex[@]}"} -- ./mtl_desc_paths
        dsay "D1 mtl_desc_paths buffer, table-pc off, ${ex[*]}: $(grep -E '^mtl_desc_paths:|^exit=' "$R/paths-buffer-$M/out.txt" | tr '\n' ' ') FAIL: $(grep '^FAIL' "$R/paths-buffer-$M/out.txt" | cut -c6-22 | tr '\n' ' ')"
        dsay "D1   $(logsum "paths-buffer-$M")"
    done

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
    # -cstab (dbuftess): every kernel's table set at set 3, as for a kernel whose module names set 1: regions per bind near
    # 0 and tables per bind near 1 as with -nopc, the same checksum
    # dbufgeek: the 13 Nov night showed 1.00 regions per bind in every buffer variant above: the three kernels keep their
    # input and output as storage-buffer descriptors (the translator's choice for a plainly indexed buffer), and those
    # held the bound offset. With bufoff the descriptors hold the buffer from its offset's 256-byte window and the rest is
    # pushed. dbufdef: bufoff is off by default (Geekbench lost 1.5% to it on the GPU), so buffer, -nopc, -nopc-cstab are
    # back at 1.00 regions per bind and 128 B; -bufoff (NVMTL_DB_BUFOFF=1) is the 13 Nov evening default, regions per
    # bind near 0 (3 per command buffer of 2000 dispatches); -fit (NVMTL_DB_BUFOFF=fit): k_ga and k_gc keep their push
    # data in the inline registers (7 dwords: a dispatch value, a table entry, the set offset, the base workgroup), so fit
    # leaves them; k_gb (13 dwords, from memory already) is converted: "1 of 3", about 0.67 regions per bind. bufoff_isa:
    # k_ga 267 -> 455 brw cycles with the pass, k_gb 470 -> 472. -keepcur stays near 1. "bufoff": converted of those made
    for M in classic buffer buffer-bufoff buffer-fit buffer-keepcur classic-nopc buffer-nopc buffer-nopc-intable buffer-nopc-cstab; do
        local mm=${M%%-*} kp= ex=()
        [ "$M" = buffer-keepcur ] && kp=cur
        [ "$M" = buffer-bufoff ] && ex+=(NVMTL_DB_BUFOFF=1)
        [ "$M" = buffer-fit ] && ex+=(NVMTL_DB_BUFOFF=fit)
        case $M in *-nopc*) ex+=(NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0);; esac
        case $M in *-intable) ex+=(NVMTL_DB_TABSET=0);; esac
        case $M in *-cstab) ex+=(NVMTL_DB_CS_TABSET=3);; esac
        run "geek-$M" NVMTL_DESC_MODEL=$mm NVMTL_DESC_STATS=1 ${kp:+NVMTL_DB_KEEP=$kp} ${ex[@]+"${ex[@]}"} -- ./mtl_desc_cost -f 30 -n 4000 geek
        dsay "D2b geek $M: $(grep '^cost geek' "$R/geek-$M/out.txt" | awk '{for(i=1;i<NF;i++) if($i=="p50") print $(i+1) " ns/dispatch, checksum " $NF}'); $(logsum "geek-$M")"
    done

    # D3 mtl_bind: every mode's image byte-identical between the models (it loads NVMTL_BUNDLE itself); buffer-nopc is the
    # buffer model with table-pc off (every table read through a table set); buffer-cstab the same with every kernel's
    # table set at set 3 (dbuftess)
    for M in classic buffer buffer-nopc buffer-cstab; do
        mkdir -p "$R/bind-$M/img"
        local nopc=0; [ "$M" != classic ] && [ "$M" != buffer ] && nopc=1
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_DESC_MODEL=${M%%-*} NVMTL_LOG_PATH="$R/bind-$M/nvmtl.log"
          [ $nopc = 1 ] && export NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0
          [ "$M" = buffer-cstab ] && export NVMTL_DB_CS_TABSET=3
          cd "$D" && tmo 60 ./mtl_bind -f 20 -o "$R/bind-$M/img" ) > "$R/bind-$M/out.txt" 2>&1
    done
    local same=0 diff=0 dl="" V
    for V in buffer buffer-nopc buffer-cstab; do
        same=0; diff=0; dl=""
        for f in "$R"/bind-classic/img/*; do
            [ -f "$f" ] || continue
            if cmp -s "$f" "$R/bind-$V/img/$(basename "$f")"; then same=$((same + 1)); else diff=$((diff + 1)); dl="$dl $(basename "$f")"; fi
        done
        dsay "D3 mtl_bind images classic vs $V: $same same, $diff different${dl:+ ($dl)}"
    done
    dsay "D3   ns/draw classic: $(awk '/enc\/draw/{printf "%s %s ", $1, $4}' "$R/bind-classic/out.txt")"
    dsay "D3   ns/draw buffer:  $(awk '/enc\/draw/{printf "%s %s ", $1, $4}' "$R/bind-buffer/out.txt")"
    # D3b mtl_bind tess alone (dbuftess), so its DESCSTAT is its own: every draw has new vertex bytes, which the
    # post-tessellation function reads through the address table. Buffer: the table in set 4, about one region per bind
    # (the fragment stage's: setFragmentTexture every draw) and one table; base (the rule before): about two, a new
    # vertex-stage region per draw on top. The image hash must be the same in all three.
    for M in classic buffer buffer-base; do
        mkdir -p "$R/tess-$M"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_DESC_MODEL=${M%%-*} NVMTL_DESC_STATS=1 NVMTL_LOG_PATH="$R/tess-$M/nvmtl.log"
          [ "$M" = buffer-base ] && export NVMTL_DB_TABSET=base
          cd "$D" && tmo 60 ./mtl_bind -f 20 tess ) > "$R/tess-$M/out.txt" 2>&1
        dsay "D3b mtl_bind tess $M: $(awk '/^tess/{print "enc/draw p50", $4, "hash", $NF}' "$R/tess-$M/out.txt"); $(logsum "tess-$M")"
    done

    # D4 mtl_tests, D5 GPU-encoded ICBs through DGC (now offered in the buffer model too). D5 names its failing cases
    # (C0-C2 are the known compute ones)
    icbfail() { grep '^FAIL' "$R/$1/out.txt" | cut -c6-40 | sed 's/  */ /g' | tr '\n' ';'; }
    for M in classic buffer; do
        mkdir -p "$R/tests-$M"
        ( export NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_BUNDLE="$B" NVMTL_DESC_MODEL=$M NVMTL_LOG_PATH="$R/tests-$M/nvmtl.log"
          cd "$D" && tmo 60 ./mtl_tests ) > "$R/tests-$M/out.txt" 2>&1
        dsay "D4 mtl_tests $M: $(grep -c 'FAIL' "$R/tests-$M/out.txt") FAIL lines; $(tail -1 "$R/tests-$M/out.txt")"
        run "icb-$M" NVMTL_DESC_MODEL=$M NVMTL_ICB_DGC=1 -- ./mtl_gpu_icb2
        dsay "D5 mtl_gpu_icb2 NVMTL_ICB_DGC=1 $M: $(grep -c '^PASS' "$R/icb-$M/out.txt") PASS, $(grep -c '^FAIL' "$R/icb-$M/out.txt") FAIL, $(tail -1 "$R/icb-$M/out.txt"); dgc: $(grep -c 'icb-dgc: render token layout\|icb-dgc: compute token layout' "$R/icb-$M/nvmtl.log"); FAIL: $(icbfail "icb-$M")"
    done
    # dbuftess: GPU-encoded compute through DGC with the kernels' table sets at set 3 (a pipeline layout of 4 sets).
    # dbufgeek: the 13 Nov run of this line also had table-pc off (NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0) and gave 5 PASS,
    # 7 FAIL against 9/3. Table-pc off is what breaks it, not the table set: a GPU-encoded render command's
    # set_vertex_buffer / set_fragment_buffer reach the shader only as DGC push-constant tokens over pushed table entries
    # (nvmtl_dgc_takes), so without graphics table-pc the four render cases that set buffers (0, 1, 4, 5) draw nothing; and
    # without cs-table-pc no kernel gets a compute DGC layout at all (no push slot), so the run never put a set-3 kernel
    # through DGC. Now: -nopc (table-pc off, no table set: the control, expected 5/7 with render cases 0 1 4 5 failing)
    # and -cstab (NVMTL_DB_CS_TABSET=3 alone, table-pc on: expected the PASS count of the buffer run, 9/3)
    run "icb-buffer-nopc" NVMTL_DESC_MODEL=buffer NVMTL_ICB_DGC=1 NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0 -- ./mtl_gpu_icb2
    dsay "D5 mtl_gpu_icb2 NVMTL_ICB_DGC=1 buffer, table-pc off: $(grep -c '^PASS' "$R/icb-buffer-nopc/out.txt") PASS, $(grep -c '^FAIL' "$R/icb-buffer-nopc/out.txt") FAIL, $(tail -1 "$R/icb-buffer-nopc/out.txt"); FAIL: $(icbfail icb-buffer-nopc)"
    run "icb-buffer-cstab" NVMTL_DESC_MODEL=buffer NVMTL_ICB_DGC=1 NVMTL_DB_CS_TABSET=3 -- ./mtl_gpu_icb2
    dsay "D5 mtl_gpu_icb2 NVMTL_ICB_DGC=1 buffer, NVMTL_DB_CS_TABSET=3: $(grep -c '^PASS' "$R/icb-buffer-cstab/out.txt") PASS, $(grep -c '^FAIL' "$R/icb-buffer-cstab/out.txt") FAIL, $(tail -1 "$R/icb-buffer-cstab/out.txt"); free-set kernels $(grep -cE 'a kernel under NVMTL_DB_CS_TABSET: its address table \(from set' "$R/icb-buffer-cstab/nvmtl.log" 2>/dev/null); FAIL: $(icbfail icb-buffer-cstab)"

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

    # D8 every staged Metal test in both models (dbufdef): classic (NVMTL_DESC_MODEL=classic) and the build's default
    # (buffer, nothing named; D0 says which). Each test's exit code, PASS/FAIL line counts and first FAIL; the verdict is
    # "same" when both models give the same exit code and counts (a test that fails on the B580 in classic too, as
    # mtl_gpu_icb2's C0-C2, must fail the same way). Tests that need the bundle path get it as their argument.
    local t a ex v1 v2 nsame=0 ndiff=0 dlist=""
    for t in mtl_smoke mtl_cube_read mtl_props mtl_f32filter mtl_timestamps mtl_gpu_icb mtl_gpu_icb2 mtl_gpu_icb3 mtl_icb_cost \
             mtl_lod_pull mtl_reflection mtl_sparse mtl_sparse_buffer mtl_sparse_tier2 mtl4_compiler mtl4_linking mtl_m4_inherit \
             mtl_tests mtl_desc_paths; do
        [ -x "$D/$t" ] || continue
        a=(); ex=(); [ $t = mtl_m4_inherit ] && a=("$B"); case $t in mtl_m4_inherit|mtl_tests) ex=(NODYLD=1);; esac
        for M in classic default; do
            if [ $M = classic ]; then run "all-$t-$M" NVMTL_DESC_MODEL=classic ${ex[@]+"${ex[@]}"} -- ./$t ${a[@]+"${a[@]}"}
            else run "all-$t-$M" NVMTL_DESC_STATS=1 ${ex[@]+"${ex[@]}"} -- ./$t ${a[@]+"${a[@]}"}; fi
        done
        v1="$(grep -m1 '^exit=' "$R/all-$t-classic/out.txt") pass $(grep -c 'PASS' "$R/all-$t-classic/out.txt") fail $(grep -c 'FAIL' "$R/all-$t-classic/out.txt")"
        v2="$(grep -m1 '^exit=' "$R/all-$t-default/out.txt") pass $(grep -c 'PASS' "$R/all-$t-default/out.txt") fail $(grep -c 'FAIL' "$R/all-$t-default/out.txt")"
        if [ "$v1" = "$v2" ]; then nsame=$((nsame + 1)); else ndiff=$((ndiff + 1)); dlist="$dlist $t"; fi
        dsay "D8 $t: classic $v1 | default $v2 $([ "$v1" = "$v2" ] && echo same || echo DIFFERENT) $(grep -m1 'FAIL' "$R/all-$t-default/out.txt" | cut -c1-80); legacy $(grep -c 'descriptor buffer: LEGACY' "$R/all-$t-default/nvmtl.log" 2>/dev/null)"
    done
    dsay "D8 summary: $nsame tests the same in both models, $ndiff different${dlist:+ ($dlist)}"
    # D8r regress.sh's Metal stages with its own parsers (check_*), on the staged plugin in both models: regress.sh itself
    # runs the installed plugin through a perl time box, which would drop DYLD_INSERT_LIBRARIES, so only its verdicts are
    # used here
    if [ -f "$D/regress.sh" ]; then
        ( REGRESS_LIB=1; set +u; . "$D/regress.sh"
          for M in classic default; do
              for st in smoke:mtl_smoke cube:mtl_cube_read props:mtl_props f32filter:mtl_f32filter timestamps:mtl_timestamps gpu_icb:mtl_gpu_icb; do
                  local f=$R/all-${st#*:}-$M/out.txt
                  [ -f "$f" ] || continue
                  local rc; rc=$(sed -n 's/^exit=//p' "$f" | tail -1)
                  grep -v '^exit=' "$f" > "$f.body"
                  echo "D8r regress $M ${st%%:*}: $(check_${st%%:*} "$f.body" "${rc:-1}" | tr '\t' ' ')"
              done
          done ) > "$R/regress-verdicts.txt" 2>&1
        while read -r line; do dsay "$line"; done < "$R/regress-verdicts.txt"
    fi

    # D7 Geekbench 7 Metal (DESC_GB=1): DESC_GB_ROUNDS rounds of DESC_GB_VARIANTS, alternating, then the median of each.
    # classic = NVMTL_DESC_MODEL=classic; buffer = the build's default (D0: the buffer model, bufoff off); buffer-fit =
    # NVMTL_DB_BUFOFF=fit; buffer-bufoff = NVMTL_DB_BUFOFF=1 (13 Nov 19:21-19:42, three rounds: classic 92.3k, buffer
    # with bufoff 92.3k, buffer without 93.7k). D7k: the kernels behind the totals (bufoff converted, and why the rest
    # were left). Each run is boxed to 15 min.
    if [ "${DESC_GB:-0}" = 1 ] && [ -x "$HOME/b580/gb_compute_profile.sh" ]; then
        local rnd V G gx
        for rnd in $(seq 1 "${DESC_GB_ROUNDS:-3}"); do
            for V in ${DESC_GB_VARIANTS:-classic buffer buffer-fit}; do
                G="$R/gb-$V-$rnd"; gx=""; rm -rf "$G"
                case $V in
                    classic) gx=" NVMTL_DESC_MODEL=classic";;
                    buffer) gx="";;
                    buffer-fit) gx=" NVMTL_DB_BUFOFF=fit";;
                    buffer-bufoff) gx=" NVMTL_DB_BUFOFF=1";;
                    *) dsay "D7 unknown variant $V"; continue;;
                esac
                GB_BUNDLE="$B" GB_INTERPOSE="$I" GB_ENV="NVMTL_PROCESS_ENV=1 NVMTL_ENABLE=1 NVMTL_DESC_STATS=1$gx" \
                    "$HOME/b580/gb_compute_profile.sh" start Metal "$G" plain > /dev/null 2>&1
                local n=0; while kill -0 "$(cat "$G/pid" 2>/dev/null)" 2>/dev/null && [ $n -lt 180 ]; do sleep 5; n=$((n + 1)); done
                kill -0 "$(cat "$G/pid" 2>/dev/null)" 2>/dev/null && { kill -9 "$(cat "$G/pid")"; dsay "D7 Geekbench $V round $rnd: KILLED after 15 min"; }
                dsay "D7 Geekbench Metal $V round $rnd: $(grep -m1 'GPU Score' "$G/gb.txt" 2>/dev/null | awk '{print $3}'); $(grep -m1 -oE 'descriptor model (BUFFER|CLASSIC)' "$G/nvmtl.log" 2>/dev/null); $(logsum "gb-$V-$rnd" | grep -oE 'bufoff [^;]*; .*' | cut -c1-400)"
                [ "$V" != classic ] && dsay "D7k $V round $rnd: $(grep -h 'DESCSTAT-KERNELS' "$G/nvmtl.log" 2>/dev/null | tail -1 | sed 's/.*DESCSTAT-KERNELS //' | cut -c1-300)"
            done
        done
        for V in ${DESC_GB_VARIANTS:-classic buffer buffer-fit}; do
            dsay "D7 median $V: $(for G in "$R"/gb-$V-*; do grep -m1 'GPU Score' "$G/gb.txt" 2>/dev/null | awk '{print $3}'; done | sort -n | awk '{v[NR]=$1} END {if (NR) printf "%s of %d runs (%s)", v[int((NR+1)/2)], NR, v[1] ".." v[NR]; else printf "no score"}')"
        done
    fi
    dsay "D: resets after: $(resets) (before: see the first D line)"
}

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    O=${O:-$HOME/night-desc-$(date +%m%d-%H%M)}; mkdir -p "$O"
    type say >/dev/null 2>&1 || say() { echo "$(date +%H:%M:%S) $*"; }
    phase_desc "$@"
fi
