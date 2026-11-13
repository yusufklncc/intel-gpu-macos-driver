# SPDX-License-Identifier: MIT
# night-vtdirect.sh: the night phases of the direct Vulkan Video decode path against libavcodec (sourced by a night
# runner, night-b.sh style: it provides O (the output folder), say (a log line) and tmo (bash's time box: never
# perl/env/sh around DYLD_* variables), and calls phase_vtdirect / phase_vtdirect_hevc / phase_vtdirect_av1). The kit is what
# tests/vtdirect-kit.sh made (decoder test build loaded by vtdec --lib; nothing installed), copied to VTDIRECT_KIT
# (default ~/b580/vtdirect).
#   phase_vtdirect:      H.264 (VTDIRECT_CODECS=h264): the JVT conformance streams and the mediakit H.264 clips
#   phase_vtdirect_hevc: HEVC (VTDIRECT_CODECS=hevc): the JCT-VC conformance streams and the mediakit HEVC 8/10-bit clips
#   phase_vtdirect_av1:  AV1 (VTDIRECT_CODECS=av1): libaom's test vectors, FFmpeg's FATE AV1 samples, the recorded SVT-AV1
#                        streams (film grain, superres, tiles, switch frames, ...) and the mediakit AV1 8/10-bit clips
# Each: the kit's sums, a sanity decode, vtdirect-ab.sh (identity, then timing), the GPU reset counters before and after.
vtdirect_run() {   # vtdirect_run <codec> <result folder name> <sanity clip> <frames>
  local K=${VTDIRECT_KIT:-$HOME/b580/vtdirect} R="$O/$2" c=$1
  mkdir -p "$R"
  ( cd "$K" && shasum -a 256 -c SHA256SUMS > "$R/sums.txt" 2>&1 ); say "V$c: sums $(grep -c ': OK' "$R/sums.txt") OK, $(grep -vc ': OK' "$R/sums.txt") not"
  say "V$c: stats before: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
  ( cd "$K" && xattr -dr com.apple.quarantine . 2>/dev/null
    export NVMTL_PROCESS_ENV=1 INTELXEDEC_DIRECT=$c
    tmo 60 ./vtdec "$3" "$4" --lib ./libintelxeav1dec.dylib --md5 --quiet > "$R/sanity.txt" 2>&1 )
  say "V$c: sanity: $(grep -E '^DECODER|^RESULT' "$R/sanity.txt" | cut -c1-240 | tr '\n' ' ')"
  ( export VTDIRECT_OUT="$R/out" VTDIRECT_CODECS=$c; tmo 2400 bash "$K/vtdirect-ab.sh" "$K" > "$R/ab.log" 2>&1 )
  say "V$c: $(grep '^SUMMARY identity:' "$R/ab.log") $(grep '^SUMMARY reference:' "$R/ab.log") | timing rows $(grep -c '^T ' "$R/out/timing.txt" 2>/dev/null)"
  grep -E '^ID .*(DIFFER|WRONG)|^SUMMARY' "$R/ab.log" | head -60 | sed 's/^/    /' >> "$O/log.txt"
  say "V$c: stats after: $(sysctl -n debug.intelxe.stats | grep -oE 'GuC context resets [0-9]+; GT resets [0-9]+')"
}
phase_vtdirect() { vtdirect_run h264 vtdirect conf/MR2_TANDBERG_E.mp4 300; }
phase_vtdirect_hevc() { vtdirect_run hevc vtdirect-hevc clips/dec/dec_hevc8_1080p30.mp4 120; }
phase_vtdirect_av1() {   # the sanity clip: the mediakit's 1080p30 8-bit one, else the one the kit generated
  local K=${VTDIRECT_KIT:-$HOME/b580/vtdirect} c=clips/dec/dec_av1_8_1080p30.mp4
  [ -f "$K/$c" ] || c=$(cd "$K" && ls clips/dec/dec_av1_*.mp4 2>/dev/null | head -1)
  vtdirect_run av1 vtdirect-av1 "${c:-conf-av1/svt-grain10.mp4}" 120
}
