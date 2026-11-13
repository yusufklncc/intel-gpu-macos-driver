#!/bin/bash
# SPDX-License-Identifier: MIT
# bench-test.sh: "intelxe-vk bench" against a stand-in Wine (a script that checks the variables each mode must set and
# writes what the provider, Mesa's WSI and DXVK would write), on any Mac, no GPU: the modes' environments, the
# interleaving, the stop after --seconds, result parsing, the report's medians and ratio.
set -euo pipefail
D=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d -t ixvk-bench); trap 'rm -rf "$T"' EXIT
mkdir -p "$T/root/provider" "$T/root/loader" "$T/root/icd.d" "$T/wine/bin" "$T/wine/lib" "$T/prefix/drive_c/windows/system32"
: > "$T/root/provider/libMoltenVK.dylib"; : > "$T/root/provider/libvulkan.1.dylib"; : > "$T/root/loader/libMoltenVK.dylib"
: > "$T/root/icd.d/intel_icd.x86_64.json"; : > "$T/anv.dylib"; : > "$T/wine/lib/libMoltenVK.dylib"; : > "$T/prefix/system.reg"
cat > "$T/wine/bin/wineserver" <<'EOF'
#!/bin/bash
exit 0
EOF
cat > "$T/wine/bin/wine" <<'EOF'
#!/bin/bash
[ "${1:-}" = --version ] && { echo wine-fake-1.0; exit 0; }
m=${INTELXE_VK_BENCH_MODE:-?}
echo "fake wine: mode $m exe $1 args ${*:2}"
bad() { echo "BAD ENV for $m: $*"; exit 3; }
[ "${NVMTL_PROCESS_ENV:-}" = 1 ] || bad "no NVMTL_PROCESS_ENV=1 (the Metal plugin would unset INTELXE*/MESA_*)"
case $m in
    anv) [ -n "${VK_DRIVER_FILES:-}" ] && [ -z "${INTELXE_VK_ICD:-}" ] && [ -n "${INTELXE_VK_FRAMELOG:-}" ] || bad anv; iv=5 ;;
    mvk) [ -z "${VK_DRIVER_FILES:-}" ] && [ -L "${INTELXE_VK_ICD:-}" ] || bad mvk
         [ "$(basename "$INTELXE_VK_ICD")" != libMoltenVK.dylib ] || bad "INTELXE_VK_ICD has the provider's leaf name"; iv=10 ;;
    wine) [ -z "${INTELXE_VK_FRAMELOG:-}${VK_DRIVER_FILES:-}${INTELXE_VK_ICD:-}" ] || bad wine; iv=20 ;;
    *) bad "no mode" ;;
esac
[ "${FAKE_HANG:-}" = 1 ] && exec sleep 30
if [ -n "${INTELXE_VK_FRAMELOG:-}" ]; then
    f=${INTELXE_VK_FRAMELOG//%p/$$}; : > "${INTELXE_VK_FRAMELOG//%p/1}"   # a second process that never presents
    { echo "# intelxe-vk framelog 1: fake"; t=0
      for ((i = 0; i < 2000; i++)); do v=$iv; [ $i = 0 ] && v=0; [ $((i % 500)) = 499 ] && v=$((iv * 4)); echo "$i $t $v 0.1 0 1"; t=$((t + v)); done; } > "$f"
fi
if [ -n "${MESA_VK_WSI_FPS_LOG:-}" ]; then
    for t in 100 102 104 106 108 110; do s=59.5; [ $t = 100 ] && s=10.0
        echo "$t.000 pid $$ 1280x720 FIFO 3 images (shared VRAM, Tile4): 60.0 presents/s, $s shown/s, 0.5 dropped/s, acquire 0.100 ms/frame"; done > "$MESA_VK_WSI_FPS_LOG"
fi
printf 'info:  Game: game.exe\ninfo:  Fake GPU (FAKE):\ninfo:    Driver : Fake driver 1.0\n' > "$DXVK_LOG_PATH/game_d3d11.log"
echo "RESULT d3d11 iters=16 1280x720 sync=0 avg_fps=$((1000 / iv)).0 (steady 9 s) best=1.0 frames=100"
EOF
chmod +x "$T/wine/bin/wine" "$T/wine/bin/wineserver"
export INTELXE_VK_ROOT=$T/root INTELXE_VK_ANV=$T/anv.dylib
fails=0
ok() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

"$D/intelxe-vk" bench --wine "$T/wine" --exe 'C:\game.exe' --prefix "$T/prefix" --runs 2 --out "$T/out" -- -w 1280 > "$T/report.txt" 2> "$T/err.txt"
R=$T/out/results.tsv
cat "$T/report.txt" | sed 's/^/      /'
ok "4 runs, interleaved anv mvk anv mvk" '[ "$(cut -f2 "$R" | tail -n +2 | tr "\n" " ")" = "anv mvk anv mvk " ]'
ok "every run exited 0" '[ "$(cut -f3 "$R" | tail -n +2 | sort -u)" = 0 ]'
ok "anv: the presenting process's log, 5 s warmup skipped: 1002 frames, 198.2 fps, max 20 ms" '[ "$(awk -F"\t" "\$2 == \"anv\" { print \$5, \$6, \$9 }" "$R" | sort -u)" = "1002 198.2 20.000" ]'
ok "mvk: 99.4 fps, p50 10 ms, max = the 40 ms hitch, 1% low 60.9" '[ "$(awk -F"\t" "\$2 == \"mvk\" { print \$6, \$7, \$9, \$10 }" "$R" | sort -u)" = "99.4 10.000 40.000 60.9" ]'
ok "anv: Mesa WSI shown/s after the warmup 59.5" '[ "$(awk -F"\t" "\$2 == \"anv\" { print \$11 }" "$R" | sort -u)" = 59.5 ]'
ok "score from the RESULT line (avg_fps=)" '[ "$(awk -F"\t" "NR > 1 { print \$2 \"=\" \$12 }" "$R" | sort -u | tr "\n" " ")" = "anv=200.0 mvk=100.0 " ]'
ok "DXVK's adapter from its log" '[ "$(cut -f13 "$R" | tail -n +2 | sort -u)" = "Fake GPU (FAKE)" ]'
ok "report: anv/mvk fps 1.99x, score 2.00x" 'grep -q "anv/mvk: fps 1.99x, score 2.00x" "$T/report.txt"'
ok "the program got its arguments" 'grep -q "args -w 1280" "$T/out/01-anv/out.log"'
ok "MoltenVK linked under another leaf name" '[ -L "$T/out/.mvk/libMoltenVK-icd.dylib" ]'

"$D/intelxe-vk" bench --wine "$T/wine/bin/wine" --exe 'C:\game.exe' --prefix "$T/prefix" --modes wine --runs 1 --seconds 2 \
    --env FAKE_HANG=1 --out "$T/out2" > /dev/null 2>&1
ok "--seconds 2 stops a program that keeps running" '[ "$(cut -f4 "$T/out2/results.tsv" | tail -1)" = stopped ]'
ok "wine mode: no provider, no frame log" '[ ! -e "$T/out2/01-wine/frames-1.txt" ] && ! grep -q INTELXE_VK_FRAMELOG "$T/out2/01-wine/env.txt"'
"$D/intelxe-vk" bench --wine "$T/wine" --exe x --prefix "$T/prefix" --modes nope --out "$T/out3" > /dev/null 2>&1 && st=0 || st=$?
ok "an unknown mode is refused" '[ $st != 0 ]'
echo "$([ $fails = 0 ] && echo PASS || echo FAIL) ($fails failure(s))"
[ $fails = 0 ]
