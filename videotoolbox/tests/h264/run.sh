#!/bin/sh
# SPDX-License-Identifier: MIT
# run.sh [--conformance DIR]: builds tests/h264parse_test for this Mac (no GPU, no FFmpeg libraries) and checks the
# direct path's H.264 parser against the recorded streams here (golden files from libavcodec, mkgolden.py).
#   MESA_INCLUDE=<mesa>/include (the Vulkan Video Std headers)
# --conformance DIR: also the JVT conformance streams of FFmpeg's FATE suite (conformance.list), downloaded into DIR
# from https://fate-suite.ffmpeg.org/h264-conformance/ when missing (they are not in this repository) and compared
# with golden files made there and then (needs ffmpeg 9.x with the trace_headers filter). A stream the direct path
# does not cover (fields/MBAFF, 4:2:2/4:4:4/10-bit, frame_num gaps) stops at its first picture and is counted apart.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_INCLUDE:?MESA_INCLUDE=<mesa>/include}"
B=${TMPDIR:-/tmp}/h264parse_test.$$
cc -O1 -g -Wall -Wextra -I"$MESA_INCLUDE" "$HERE/../h264parse_test.c" "$HERE/../../vtdirect_h264.c" -o "$B" || exit 2
trap 'rm -rf "$B" "$B.dSYM" "$B.g"' EXIT
fail=0
for f in "$HERE"/*.264; do "$B" "$f" "${f%.264}.txt" || fail=1; done
if [ "${1:-}" = --conformance ]; then
  D=${2:?--conformance DIR}; mkdir -p "$D"
  same=0 unsup=0 diff=0
  while read -r n; do
    o=$(echo "$n" | tr / _)
    [ -s "$D/$o" ] || curl -sf --max-time 120 -o "$D/$o" "https://fate-suite.ffmpeg.org/h264-conformance/$n" || { echo "$n: download failed"; continue; }
    python3 "$HERE/mkgolden.py" "$D/$o" > "$B.g" 2>/dev/null
    r=$("$B" "$D/$o" "$B.g" 2>&1)
    case "$r" in
      *"identical to"*) same=$((same + 1));;
      *"sample: -1 "*) unsup=$((unsup + 1)); echo "$n: not covered: $(echo "$r" | sed -n 's/.*sample: -1 \(.*\)/\1/p' | head -1)";;
      *) diff=$((diff + 1)); fail=1; echo "$n: DIFFERS"; echo "$r" | head -4;;
    esac
  done < "$HERE/conformance.list"
  echo "conformance: $same identical to libavcodec, $unsup not covered (libavcodec decodes them), $diff differ"
fi
exit $fail
