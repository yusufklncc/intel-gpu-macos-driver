#!/bin/bash
# SPDX-License-Identifier: MIT
# run.sh [--conformance DIR] [--sanitize]: builds tests/vp9parse_test for this Mac (no GPU, no FFmpeg libraries) and
# checks the direct path's VP9 parser against the recorded streams here (golden files from FFmpeg 9.0.2, mkgolden.py).
#   MESA_INCLUDE=<mesa>/include (the Vulkan Video Std headers)
# --conformance DIR: also libvpx's VP9 test vectors (conformance.list: name, SHA-256 of the stream and of its published
# per-frame MD5 file), downloaded into DIR when missing (they are not in this repository), checked, remuxed to IVF
# (-copyinkf: vp90-2-16-intra-only starts without a key frame) and compared with golden files made there and then
# (needs ffmpeg 9.x with trace_headers and libvpx). A stream the direct path does not cover (profile 1 / 3, 12-bit)
# stops at its first frame and is counted apart; one whose golden file cannot be made is counted apart too.
# --sanitize: the test built with AddressSanitizer and UndefinedBehaviorSanitizer, and every stream also fuzzed
# (vp9parse_test --fuzz: damaged samples, 200 rounds per recorded stream, 20 per conformance stream).
set -u -o pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
: "${MESA_INCLUDE:?MESA_INCLUDE=<mesa>/include}"
CONF="" SAN=0
while [ $# -gt 0 ]; do
  case "$1" in --conformance) CONF=${2:?--conformance DIR}; shift 2;; --sanitize) SAN=1; shift;; *) echo "usage: run.sh [--conformance DIR] [--sanitize]"; exit 2;; esac
done
B=${TMPDIR:-/tmp}/vp9parse_test.$$
FL=""; [ $SAN = 1 ] && FL="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
cc -O1 -g -Wall -Wextra $FL -I"$MESA_INCLUDE" "$HERE/../vp9parse_test.c" "$HERE/../../vtdirect_vp9.c" -o "$B" || exit 2
trap 'rm -rf "$B" "$B.dSYM" "$B.g" "$B.e" "$B.e.list" "$B.ivf"' EXIT
fail=0
for f in "$HERE"/*.ivf; do
  "$B" "$f" "${f%.ivf}.txt" || fail=1
  if [ $SAN = 1 ]; then "$B" --fuzz 1 200 "$f" | sed "s|^|$(basename "$f"): |" || fail=1; fi
done
if [ -n "$CONF" ]; then
  mkdir -p "$CONF"
  url=https://storage.googleapis.com/downloads.webmproject.org/test_data/libvpx
  grep -v '^#' "$HERE/conformance.list" | while read -r n sum msum; do
    for x in "$n" "$n.md5"; do
      if [ ! -s "$CONF/$x" ]; then curl -sfL --max-time 300 -o "$CONF/$x" "$url/$x" || { echo "DOWNLOAD $x: failed"; rm -f "$CONF/$x"; }; fi
    done
    [ -s "$CONF/$n" ] && [ -s "$CONF/$n.md5" ] || continue
    [ "$(shasum -a 256 "$CONF/$n" | cut -d' ' -f1)" = "$sum" ] || { echo "SHA256 $n: differs from conformance.list"; continue; }
    [ "$(shasum -a 256 "$CONF/$n.md5" | cut -d' ' -f1)" = "$msum" ] || { echo "SHA256 $n.md5: differs from conformance.list"; continue; }
    f=$CONF/$n
    case "$n" in *.ivf) ;; *) ffmpeg -hide_banner -nostdin -v error -i "$f" -c copy -copyinkf -f ivf -y "$B.ivf" < /dev/null || { echo "NOGOLDEN $n: remux failed"; continue; }; f=$B.ivf;; esac
    r=$("$B" "$f" 2>&1)
    case "$r" in *"sample: -1 "*) echo "UNSUPPORTED $n: $(echo "$r" | sed -n 's/.*sample: -1 \(.*\)/\1/p' | head -1)"; continue;; esac
    if ! python3 "$HERE/mkgolden.py" "$f" > "$B.g" 2> "$B.e"; then
      echo "NOGOLDEN $n: $(head -c 300 "$B.e" | tr '\n' ' ')"; continue
    fi
    r=$("$B" "$f" "$B.g" 2>&1)
    case "$r" in
      *"identical to"*) echo "SAME $n";;
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
