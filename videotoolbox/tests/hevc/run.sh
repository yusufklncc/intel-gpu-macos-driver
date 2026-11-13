#!/bin/bash
# SPDX-License-Identifier: MIT
# run.sh [--conformance DIR]: builds tests/hevcparse_test for this Mac (no GPU, no FFmpeg libraries) and checks the
# direct path's HEVC parser against the recorded streams here (golden files from FFmpeg 9.0.2, mkgolden.py).
#   MESA_INCLUDE=<mesa>/include (the Vulkan Video Std headers)
# --conformance DIR: also the JCT-VC HEVC_v1 conformance bitstreams (conformance.list: the official ITU-T zips with
# their SHA-256), downloaded into DIR when missing (they are not in this repository), checked, the bitstream taken out
# as DIR/<name>.bit, and compared with golden files made there and then (needs ffmpeg 9.x: trace_headers and
# libavcodec's HEVC decoder). A stream the direct path does not cover (not Main/Main 10 4:2:0, range extensions, ...)
# stops at its first picture and is counted apart; one whose golden file cannot be made (mkgolden.py fails: the
# trace_headers filter stops at a parameter set it cannot read, or the POCs differ from libavcodec's) is counted apart
# too, with mkgolden's reason.
set -u -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_INCLUDE:?MESA_INCLUDE=<mesa>/include}"
B=${TMPDIR:-/tmp}/hevcparse_test.$$
cc -O1 -g -Wall -Wextra -I"$MESA_INCLUDE" "$HERE/../hevcparse_test.c" "$HERE/../../vtdirect_hevc.c" -o "$B" || exit 2
trap 'rm -rf "$B" "$B.dSYM" "$B.g" "$B.e" "$B.e.list"' EXIT
fail=0
for f in "$HERE"/*.265; do "$B" "$f" "${f%.265}.txt" || fail=1; done
if [ "${1:-}" = --conformance ]; then
  D=${2:?--conformance DIR}; mkdir -p "$D"
  URL=https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1
  grep -v '^#' "$HERE/conformance.list" | while read -r z sum; do
    n=${z%.zip}
    if [ ! -s "$D/$n.bit" ]; then
      [ -s "$D/$z" ] || curl -sfL --max-time 300 -o "$D/$z" "$URL/$z" || { echo "$n: download failed"; continue; }
      [ "$(shasum -a 256 "$D/$z" | cut -d' ' -f1)" = "$sum" ] || { echo "$n: SHA-256 differs from conformance.list"; rm -f "$D/$z"; continue; }
      # the zip's one bitstream (.bit / .bin), read as data (python -I: nothing is imported from DIR)
      python3 -I -c 'import sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); m = [i for i in z.namelist() if i.lower().endswith((".bit", ".bin")) and "__MACOSX" not in i]
assert len(m) == 1, m
open(sys.argv[2], "wb").write(z.read(m[0]))' "$D/$z" "$D/$n.bit" || { echo "$n: no bitstream in $z"; continue; }
    fi
    if ! python3 "$HERE/mkgolden.py" "$D/$n.bit" > "$B.g" 2> "$B.e"; then
      echo "NOGOLDEN $n: $(head -c 300 "$B.e" | tr '\n' ' ')"; continue
    fi
    r=$("$B" "$D/$n.bit" "$B.g" 2>&1)
    case "$r" in
      *"identical to"*) echo "SAME $n";;
      *"sample: -1 "*) echo "UNSUPPORTED $n: $(echo "$r" | sed -n 's/.*sample: -1 \(.*\)/\1/p' | head -1)";;
      *) echo "DIFFERS $n"; echo "$r" | head -4;;
    esac
  done > "$B.e.list"
  cat "$B.e.list"
  same=$(grep -c '^SAME ' "$B.e.list"); unsup=$(grep -c '^UNSUPPORTED ' "$B.e.list"); diff=$(grep -c '^DIFFERS ' "$B.e.list")
  nogold=$(grep -c '^NOGOLDEN ' "$B.e.list")
  rm -f "$B.e.list"
  [ "$diff" = 0 ] || fail=1
  echo "conformance: $same identical to libavcodec, $unsup not covered (libavcodec decodes them), $nogold without a golden file, $diff differ"
fi
exit $fail
