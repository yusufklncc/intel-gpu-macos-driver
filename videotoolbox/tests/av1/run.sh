#!/bin/bash
# SPDX-License-Identifier: MIT
# run.sh [--conformance DIR] [--sanitize]: builds tests/av1parse_test for this Mac (no GPU, no FFmpeg libraries) and
# checks the direct path's AV1 parser against the recorded streams here (golden files from FFmpeg 9.0.2, mkgolden.py).
#   MESA_INCLUDE=<mesa>/include (the Vulkan Video Std headers)
# --conformance DIR: also libaom's AV1 test vectors and FFmpeg's FATE AV1 samples (conformance.list: name and SHA-256),
# downloaded into DIR when missing (they are not in this repository), checked, a Matroska one remuxed to IVF, and
# compared with golden files made there and then (needs ffmpeg 9.x with trace_headers and libdav1d). A stream the direct
# path does not cover (not Main 4:2:0 8/10-bit, several spatial layers, ...) stops at its sequence header and is
# counted apart; one whose golden file cannot be made (mkgolden.py fails) is counted apart too, with its reason.
# --sanitize: the test built with AddressSanitizer and UndefinedBehaviorSanitizer, and every stream also fuzzed
# (av1parse_test --fuzz: damaged samples, 200 rounds per recorded stream, 20 per conformance stream).
set -u -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_INCLUDE:?MESA_INCLUDE=<mesa>/include}"
CONF="" SAN=0
while [ $# -gt 0 ]; do
  case "$1" in --conformance) CONF=${2:?--conformance DIR}; shift 2;; --sanitize) SAN=1; shift;; *) echo "usage: run.sh [--conformance DIR] [--sanitize]"; exit 2;; esac
done
B=${TMPDIR:-/tmp}/av1parse_test.$$
FL=""; [ $SAN = 1 ] && FL="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -O1 -g -Wall -Wextra $FL -I"$MESA_INCLUDE" "$HERE/../av1parse_test.c" "$HERE/../../vtdirect_av1.c" -o "$B" || exit 2
trap 'rm -rf "$B" "$B.dSYM" "$B.g" "$B.e" "$B.e.list" "$B.ivf"' EXIT
fail=0
for f in "$HERE"/*.ivf; do
  "$B" "$f" "${f%.ivf}.txt" || fail=1
  if [ $SAN = 1 ]; then "$B" --fuzz 1 200 "$f" | sed "s|^|$(basename "$f"): |" || fail=1; fi
done
if [ -n "$CONF" ]; then
  mkdir -p "$CONF"
  grep -v '^#' "$HERE/conformance.list" | while read -r src n sum; do
    case $src in aom) url=https://storage.googleapis.com/aom-test-data/$n;; fate) url=https://fate-suite.ffmpeg.org/av1/$n;; *) continue;; esac
    if [ ! -s "$CONF/$n" ]; then curl -sfL --max-time 300 -o "$CONF/$n" "$url" || { echo "DOWNLOAD $n: failed"; rm -f "$CONF/$n"; continue; }; fi
    [ "$(shasum -a 256 "$CONF/$n" | cut -d' ' -f1)" = "$sum" ] || { echo "SHA256 $n: differs from conformance.list"; continue; }
    f=$CONF/$n
    case "$n" in *.ivf) ;; *) ffmpeg -hide_banner -nostdin -v error -i "$f" -c copy -f ivf -y "$B.ivf" < /dev/null || { echo "NOGOLDEN $n: remux failed"; continue; }; f=$B.ivf;; esac
    if ! python3 "$HERE/mkgolden.py" "$f" > "$B.g" 2> "$B.e"; then
      echo "NOGOLDEN $n: $(head -c 300 "$B.e" | tr '\n' ' ')"; continue
    fi
    r=$("$B" "$f" "$B.g" 2>&1)
    case "$r" in
      *"identical to"*) echo "SAME $n";;
      *"sample: -1 "*) echo "UNSUPPORTED $n: $(echo "$r" | sed -n 's/.*sample: -1 \(.*\)/\1/p' | head -1)";;
      *) echo "DIFFERS $n"; echo "$r" | head -4;;
    esac
    if [ $SAN = 1 ]; then "$B" --fuzz 2 20 "$f" > "$B.e" 2>&1 || { echo "FUZZ $n: $(tail -3 "$B.e" | tr '\n' ' ')"; }; fi
  done > "$B.e.list"
  cat "$B.e.list"
  same=$(grep -c '^SAME ' "$B.e.list"); unsup=$(grep -c '^UNSUPPORTED ' "$B.e.list"); diff=$(grep -c '^DIFFERS ' "$B.e.list")
  nogold=$(grep -c '^NOGOLDEN ' "$B.e.list"); other=$(grep -cE '^(DOWNLOAD|SHA256|FUZZ) ' "$B.e.list")
  [ "$diff" = 0 ] && [ "$other" = 0 ] || fail=1
  echo "conformance: $same identical to the golden files, $unsup not covered (libavcodec decodes them), $nogold without a golden file, $diff differ, $other download/hash/fuzz failures"
fi
exit $fail
