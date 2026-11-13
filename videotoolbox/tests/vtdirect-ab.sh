#!/bin/bash
# SPDX-License-Identifier: MIT
# vtdirect-ab.sh <kit> [clip.mp4 ...]: the direct Vulkan Video path (INTELXEDEC_DIRECT=h264 / hevc / av1) against the
# libavcodec path, on the B580 machine, with a test build loaded in-process by vtdec --lib (nothing installed; libavcodec
# and libavutil are the installed ones, which this change does not touch). <kit> is what vtdirect-kit.sh made:
# libintelxeav1dec.dylib, vtdec, conf/*.mp4 (the JVT H.264 conformance streams the direct path covers, remuxed),
# conf-hevc/*.mp4 (the JCT-VC HEVC ones), conf-av1/*.mp4 (libaom's test vectors, FFmpeg's FATE AV1 samples and the
# recorded SVT-AV1 streams of tests/av1), and clips/: the mediakit's H.264, HEVC and AV1 clips (dec/dec_h264_*.mp4,
# dec/dec_hevc8_*.mp4, dec/dec_hevc10_*.mp4, dec/dec_av1_8_*.mp4, dec/dec_av1_10_*.mp4, reference md5s in ref/), or the
# clips given here.
#   VTDIRECT_CODECS: the codecs to run, "h264 hevc av1" by default (a night phase runs one: "h264", "hevc" or "av1"). A
#             clip is H.264, HEVC or AV1 by its name (dec_h264_*, dec_hevc*, dec_av1_*, gen_*, under conf/, conf-hevc/
#             or conf-av1/), a clip given here by its sample description ('hvc1' / 'hev1': HEVC, 'av01': AV1).
#   clips:    the kit's clips/, else $MEDIAKIT/clips, else ~/b580/mediakit/clips; with none of those for a codec, a 1080p60
#             clip made by ffmpeg (libx264 High / libx265 Main) if this machine has one, else the three largest
#             conformance streams the direct path decodes (small pictures: the timing then says so). Never no timing at
#             all: no clip, or no run that printed a RESULT line, is "SUMMARY FAILED timing" and exit 4 / 5.
#   identity: vtdec --md5 on both paths, every picture's checksum compared in output (decode) order; for the mediakit
#             clips also sorted by presentation time against the software reference (libavcodec on the CPU);
#   timing:   vtdec --async --quiet on the clips above, libavcodec / direct alternated three times (the first session
#             of a process pays for the device): RESULT (wall ms per picture, the process's CPU %) and the decoder's own
#             line (fps, CPU ms of the thread that calls DecodeFrame, until-emitted latency); each run's output kept as
#             out/<clip>.<path>.run<i>.timing.txt, the T lines in out/timing.txt (made empty first: it always exists).
# Every vtdec run is time-boxed (tmo: a bash timer, not perl/env: those would drop DYLD_* if a run ever needs it).
# bash 3.2 (macOS's /bin/bash): no associative arrays.
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
O=${VTDIRECT_OUT:-$PWD/out}; mkdir -p "$O"
# the timing table exists from the start, whatever happens later (the 13 Nov night: it was only made by the timing
# loop, which had no clip, and the summary's awk failed on the missing file)
: > "$O/timing.txt"; : > "$O/identity.txt"
CODECS=${VTDIRECT_CODECS:-h264 hevc av1}
shopt -s nullglob
# the mediakit clips: MEDIAKIT, else the kit's own copy (vtdirect-kit.sh puts them in clips/), else ~/b580/mediakit.
if [ -n "${MEDIAKIT:-}" ]; then MK=$MEDIAKIT
elif [ -n "$(echo clips/dec/dec_h264_*.mp4 clips/dec/dec_hevc*.mp4 clips/dec/dec_av1_*.mp4)" ]; then MK=$PWD
else MK=$HOME/b580/mediakit; fi
if "$V" /dev/null 1 --lib "$L" --quiet 2>&1 | grep -q '^dlopen'; then   # the build must load (its FFmpeg libraries)
  echo "SUMMARY vtdec cannot load the decoder build:"; "$V" /dev/null 1 --lib "$L" 2>&1 | head -2; exit 3
fi
codec_of() {   # h264 / hevc / av1 for a clip
  case "${1##*/}" in dec_h264_*|gen_h264_*) echo h264; return;; dec_hevc*|gen_hevc*) echo hevc; return;; dec_av1_*|gen_av1_*) echo av1; return;; esac
  case "$1" in */conf-hevc/*|conf-hevc/*) echo hevc; return;; */conf-av1/*|conf-av1/*) echo av1; return;; */conf/*|conf/*) echo h264; return;; esac
  if LC_ALL=C grep -q -a 'av01' "$1" 2>/dev/null; then echo av1
  elif LC_ALL=C grep -q -a -E 'hvc1|hev1' "$1" 2>/dev/null; then echo hevc; else echo h264; fi
}
mk_of() {
  case $1 in
    h264) echo "$MK"/clips/dec/dec_h264_*.mp4;;
    hevc) echo "$MK"/clips/dec/dec_hevc8_*.mp4 "$MK"/clips/dec/dec_hevc10_*.mp4;;
    av1) for c in "$MK"/clips/dec/dec_av1_8_*.mp4 "$MK"/clips/dec/dec_av1_10_*.mp4; do case "$c" in *_4320p*) ;; *) echo "$c";; esac; done;;
  esac
}
conf_of() { case $1 in h264) echo conf/*.mp4;; hevc) echo conf-hevc/*.mp4;; av1) echo conf-av1/*.mp4;; esac; }
# the conformance streams the kit adds that the direct path must hand to libavcodec (H.264: at the start; AV1: frames of
# another size, mid-stream, with the samples since the key frame replayed): not for timing
FALLBACK_RE='/(CVFI1_SVA_C|CAMA1_TOSHIBA_B|MR4_TANDBERG_C|svt-resize|av1-1-b8-03-size(up|down)|switch_frame)\.mp4$'
clips=() tclips=()
for codec in $CODECS; do
  mkc=($(mk_of $codec)) confs=($(conf_of $codec)) gen=() t=()
  if [ ${#args[@]} -gt 0 ]; then for c in "${args[@]}"; do [ "$(codec_of "$c")" = $codec ] && t+=("$c"); done; tsrc="the clips given"
  elif [ ${#mkc[@]} -gt 0 ]; then t=("${mkc[@]}"); tsrc="the mediakit clips of $MK"
  else
    ff=${VTDIRECT_FFMPEG:-$(command -v ffmpeg)} g=$O/gen_${codec}_1080p60.mp4
    case $codec in
      h264) enc="-c:v libx264 -profile:v high -preset veryfast -bf 2";; hevc) enc="-c:v libx265 -tag:v hvc1 -preset veryfast -bf 2";;
      av1) enc="-c:v libsvtav1 -preset 10";;
    esac
    if [ -n "$ff" ] && tmo 300 "$ff" -hide_banner -v error -f lavfi -i testsrc2=size=1920x1080:rate=60:duration=10 \
         $enc -pix_fmt yuv420p -g 120 -y "$g" < /dev/null && [ -s "$g" ]; then
      gen=("$g"); t=("$g"); tsrc="no mediakit clips in $MK: a 1080p60 clip generated by $ff"
    else   # the largest by size, not those the direct path hands to libavcodec
      [ ${#confs[@]} -gt 0 ] && while read -r f; do t+=("$f"); done < <(ls -S "${confs[@]}" | grep -vE "$FALLBACK_RE" | head -3)
      tsrc="NO mediakit clips in $MK and no ffmpeg encoder here: the three largest conformance streams (small pictures, not representative)"
    fi
  fi
  echo "SUMMARY timing clips $codec ($tsrc): ${t[*]##*/}"
  [ ${#t[@]} -gt 0 ] && tclips+=("${t[@]}")
  if [ ${#args[@]} -gt 0 ]; then for c in "${args[@]}"; do [ "$(codec_of "$c")" = $codec ] && clips+=("$c"); done
  else for c in "${confs[@]}" "${mkc[@]}" "${gen[@]}"; do clips+=("$c"); done; fi
done
same=0 differ=0 both_failed=0
for c in "${clips[@]}"; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4) codec=$(codec_of "$c")
  for p in avc direct; do
    d=0; [ $p = direct ] && d=$codec
    INTELXEDEC_DIRECT=$d tmo 120 "$V" "$c" 1000000 --lib "$L" --md5 --quiet > "$O/$b.$p.txt" 2>&1
    echo "exit $?" >> "$O/$b.$p.txt"
  done
  grep -E '^ ?frame ' "$O/$b.avc.txt" | awk '{print $NF}' > "$O/$b.avc.md5"
  grep -E '^ ?frame ' "$O/$b.direct.txt" | awk '{print $NF}' > "$O/$b.direct.md5"
  na=$(wc -l < "$O/$b.avc.md5" | tr -d ' '); nd=$(wc -l < "$O/$b.direct.md5" | tr -d ' ')
  path=$(grep -m1 '^DECODER' "$O/$b.direct.txt" | grep -oE 'path=[^ ]+|skipped=[^ ]+|fallback=.*' | tr '\n' ' ')
  if [ "$na" = 0 ] && [ "$nd" = 0 ]; then both_failed=$((both_failed + 1)); st="no pictures on either path"
  elif cmp -s "$O/$b.avc.md5" "$O/$b.direct.md5"; then same=$((same + 1)); st="identical"
  else differ=$((differ + 1)); st="DIFFER (first at picture $(paste -d' ' "$O/$b.avc.md5" "$O/$b.direct.md5" | awk '$1 != $2 {print NR; exit}'))"; fi
  ref="$MK/clips/ref/$b.md5"
  if [ -f "$ref" ]; then   # the reference is in presentation order: sort by pts
    grep -E '^ ?frame ' "$O/$b.direct.txt" | awk '{print $4, $NF}' | sort -n | awk '{print $2}' > "$O/$b.direct.pts.md5"
    bad=$(paste -d' ' "$O/$b.direct.pts.md5" "$ref" | awk '$1 != $2 {n++} END {print n+0}')
    st="$st, direct vs software reference: $bad of $(wc -l < "$ref" | tr -d ' ') differ"
  fi
  echo "ID $codec $b: libavcodec $na pictures, direct $nd: $st [$path]" | tee -a "$O/identity.txt"
done
for codec in $CODECS; do
  awk -v c=$codec '$2 == c { n++; if (/: identical/) s++; else if (/DIFFER/) d++; else f++ }
    END { printf "SUMMARY identity %s: %d identical, %d differ, %d decoded by neither\n", c, s, d, f }' "$O/identity.txt" 2>/dev/null ||
    echo "SUMMARY identity $codec: 0 identical, 0 differ, 0 decoded by neither"
done
echo "SUMMARY identity: $same identical, $differ differ, $both_failed decoded by neither"
# timing: each clip, each path three times, alternated
[ ${#tclips[@]} -eq 0 ] && { echo "SUMMARY FAILED timing: no clip to time (no mediakit clips, no ffmpeg, no conformance streams)"; exit 4; }
for c in "${tclips[@]}"; do
  [ -f "$c" ] || { echo "SUMMARY FAILED timing: $c is missing"; continue; }
  b=$(basename "$c" .mp4) codec=$(codec_of "$c")
  for i in 1 2 3; do
    for p in avc direct; do
      d=0; [ $p = direct ] && d=$codec
      t=$O/$b.$p.run$i.timing.txt
      INTELXEDEC_DIRECT=$d tmo 120 "$V" "$c" 1000000 --lib "$L" --async --quiet > "$t" 2>&1; echo "exit $?" >> "$t"
      echo "T $b $p run$i $(grep -m1 '^RESULT' "$t" | cut -d' ' -f2-) | $(grep -m1 '^DECODER' "$t" | grep -oE '(path|fps|decode_ms|cpu_ms|copy_submit_ms|latency_ms|parse_ms|record_ms|submit_ms)=[^ ]+' | tr '\n' ' ')$(grep -q '^RESULT' "$t" || echo "NO RESULT ($(tail -1 "$t"); see ${t##*/})")" | tee -a "$O/timing.txt"
    done
  done
done
nres=$(grep -c ' ms_per_frame ' "$O/timing.txt"); nrun=$(grep -c '^T ' "$O/timing.txt")
if [ "$nres" = 0 ]; then echo "SUMMARY FAILED timing: none of $nrun runs printed a RESULT line (see $O/*.timing.txt)"; exit 5; fi
[ "$nres" != "$nrun" ] && echo "SUMMARY timing: only $nres of $nrun runs printed a RESULT line (the medians use those)"
# per clip and path, the median of the runs: ms per picture (wall), process CPU %, the decoding thread's CPU ms per
# DecodeFrame, the decoder's fps
awk '$1 == "T" && / ms_per_frame / {
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
