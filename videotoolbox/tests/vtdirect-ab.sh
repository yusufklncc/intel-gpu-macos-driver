#!/bin/bash
# SPDX-License-Identifier: MIT
# vtdirect-ab.sh <kit> [clip.mp4 ...]: the direct Vulkan Video path (INTELXEDEC_DIRECT=h264) against the libavcodec
# path, on the B580 machine, with a test build loaded in-process by vtdec --lib (nothing installed; libavcodec and
# libavutil are the installed ones, which this change does not touch). <kit> is what vtdirect-kit.sh made:
# libintelxeav1dec.dylib, vtdec, conf/*.mp4 (the JVT conformance streams the direct path covers, remuxed) and, if
# given, the clips (default: the H.264 clips of ~/b580/mediakit/clips/dec, with their reference md5s from clips/ref).
#   identity: vtdec --md5 on both paths, every picture's checksum compared in output (decode) order; for the mediakit
#             clips also sorted by presentation time against the software reference (libavcodec on the CPU);
#   timing:   vtdec --async --quiet, libavcodec / direct alternated three times (the first session of a process pays
#             for the device): RESULT (wall ms per picture, the process's CPU %) and the decoder's own line (fps, CPU ms
#             of the thread that calls DecodeFrame, until-emitted latency).
# Every vtdec run is time-boxed (tmo: a bash timer, not perl/env: those would drop DYLD_* if a run ever needs it).
# Output lines to grep: "^ID " (one per clip: identical / DIFFER), "^T " (timing), "^SUMMARY".
K=${1:?usage: vtdirect-ab.sh <kit> [clip.mp4 ...]}; shift
args=(); for c in "$@"; do case "$c" in /*) args+=("$c");; *) args+=("$PWD/$c");; esac; done   # before the cd
cd "$K" || exit 2
# (the timer's sleep is killed with it and holds no pipe: inside $(...) a stray sleep kept the substitution open)
tmo() {
  local s=$1; shift
  "$@" & local p=$!
  ( trap 'kill $sp 2>/dev/null; exit 0' TERM; sleep "$s" & sp=$!; wait $sp; kill -9 $p 2>/dev/null ) > /dev/null 2>&1 &
  local w=$!
  wait $p; local r=$?
  kill $w 2>/dev/null; wait $w 2>/dev/null
  return $r
}
# the Metal plug-in, if VideoToolbox loads it here, keeps INTELXEDEC_* and stays out of the decoder registration
export NVMTL_PROCESS_ENV=1 NVMTL_NO_INTELXEDEC=1
# VTDIRECT_LIBS=<dir>: libavcodec/libavutil from there instead of the installed ones (exported here, inside bash: a
# DYLD_* variable set before a protected binary such as bash itself is dropped when it starts)
[ -n "${VTDIRECT_LIBS:-}" ] && export DYLD_LIBRARY_PATH="$VTDIRECT_LIBS"
L=$PWD/libintelxeav1dec.dylib V=$PWD/vtdec
O=${VTDIRECT_OUT:-$PWD/out}; mkdir -p "$O"; rm -f "$O/timing.txt"
MK=${MEDIAKIT:-$HOME/b580/mediakit}
if "$V" /dev/null 1 --lib "$L" --quiet 2>&1 | grep -q '^dlopen'; then   # the build must load (its FFmpeg libraries)
  echo "SUMMARY vtdec cannot load the decoder build:"; "$V" /dev/null 1 --lib "$L" 2>&1 | head -2; exit 3
fi
clips=("${args[@]}")
[ ${#args[@]} -eq 0 ] && clips=(conf/*.mp4 "$MK"/clips/dec/dec_h264_*.mp4)
same=0 differ=0 both_failed=0
for c in "${clips[@]}"; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4)
  for p in avc direct; do
    d=0; [ $p = direct ] && d=h264
    INTELXEDEC_DIRECT=$d tmo 120 "$V" "$c" 1000000 --lib "$L" --md5 --quiet > "$O/$b.$p.txt" 2>&1
    echo "exit $?" >> "$O/$b.$p.txt"
  done
  grep -E '^ ?frame ' "$O/$b.avc.txt" | awk '{print $NF}' > "$O/$b.avc.md5"
  grep -E '^ ?frame ' "$O/$b.direct.txt" | awk '{print $NF}' > "$O/$b.direct.md5"
  na=$(wc -l < "$O/$b.avc.md5" | tr -d ' '); nd=$(wc -l < "$O/$b.direct.md5" | tr -d ' ')
  path=$(grep -m1 '^DECODER' "$O/$b.direct.txt" | grep -oE 'path=[^ ]+|fallback=.*' | tr '\n' ' ')
  if [ "$na" = 0 ] && [ "$nd" = 0 ]; then both_failed=$((both_failed + 1)); st="no pictures on either path"
  elif cmp -s "$O/$b.avc.md5" "$O/$b.direct.md5"; then same=$((same + 1)); st="identical"
  else differ=$((differ + 1)); st="DIFFER (first at picture $(paste -d' ' "$O/$b.avc.md5" "$O/$b.direct.md5" | awk '$1 != $2 {print NR; exit}'))"; fi
  ref="$MK/clips/ref/$b.md5"
  if [ -f "$ref" ]; then   # the reference is in presentation order: sort by pts
    grep -E '^ ?frame ' "$O/$b.direct.txt" | awk '{print $4, $NF}' | sort -n | awk '{print $2}' > "$O/$b.direct.pts.md5"
    bad=$(paste -d' ' "$O/$b.direct.pts.md5" "$ref" | awk '$1 != $2 {n++} END {print n+0}')
    st="$st, direct vs software reference: $bad of $(wc -l < "$ref" | tr -d ' ') differ"
  fi
  echo "ID $b: libavcodec $na pictures, direct $nd: $st [$path]"
done
echo "SUMMARY identity: $same identical, $differ differ, $both_failed decoded by neither"
# timing: the mediakit clips (or the clips given), each path three times, alternated
tclips=("${args[@]}"); [ ${#args[@]} -eq 0 ] && tclips=("$MK"/clips/dec/dec_h264_*.mp4)
for c in "${tclips[@]}"; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4)
  for i in 1 2 3; do
    for p in avc direct; do
      d=0; [ $p = direct ] && d=h264
      r=$(INTELXEDEC_DIRECT=$d tmo 120 "$V" "$c" 1000000 --lib "$L" --async --quiet 2>&1)
      echo "T $b $p run$i $(echo "$r" | grep -m1 '^RESULT' | cut -d' ' -f2-) | $(echo "$r" | grep -m1 '^DECODER' | grep -oE '(path|fps|decode_ms|cpu_ms|copy_submit_ms|latency_ms|parse_ms|record_ms|submit_ms)=[^ ]+' | tr '\n' ' ')" | tee -a "$O/timing.txt"
    done
  done
done
# per clip and path, the median of the runs: ms per picture (wall), process CPU %, the decoding thread's CPU ms per
# DecodeFrame, the decoder's fps
awk '$1 == "T" {
  k = $2 " " $3; n[k]++
  for (i = 5; i <= NF; i++) {
    if ($i == "ms_per_frame") m[k, n[k]] = $(i + 1)
    if ($i == "cpu_pct") c[k, n[k]] = $(i + 1)
    if ($i ~ /^cpu_ms=/) { split($i, a, "="); t[k, n[k]] = a[2] }
    if ($i ~ /^fps=/) { split($i, a, "="); f[k, n[k]] = a[2] }
  }
}
function med(arr, k, cnt,   i, j, v, x) { for (i = 1; i <= cnt; i++) v[i] = arr[k, i] + 0
  for (i = 1; i <= cnt; i++) for (j = i + 1; j <= cnt; j++) if (v[j] < v[i]) { x = v[i]; v[i] = v[j]; v[j] = x }
  return v[int((cnt + 1) / 2)] }
END { for (k in n) printf "SUMMARY timing %s: ms_per_frame %.3f cpu_pct %.0f decode_thread_cpu_ms %.3f decoder_fps %.1f (median of %d)\n",
        k, med(m, k, n[k]), med(c, k, n[k]), med(t, k, n[k]), med(f, k, n[k]), n[k] }' "$O/timing.txt" | sort
echo "SUMMARY done"
