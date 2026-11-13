#!/bin/sh
# SPDX-License-Identifier: MIT
# vtdirect-kit.sh <out> <conformance dir>: the folder the night A/B (vtdirect-ab.sh) runs from on the B580 machine:
# the decoder test build and vtdec (build.sh), vtdirect-ab.sh, and conf/: the JVT H.264 conformance streams of
# <conformance dir> (tests/h264/run.sh --conformance downloads them) remuxed into MP4 (ffmpeg -c copy), those the
# direct path covers (its parser agrees with libavcodec on them here) and three it hands to libavcodec at the start
# (fields, MBAFF, frame_num gaps). clips/: what the timing part decodes: the mediakit's H.264 clips
# ($MEDIAKIT, default ~/b580/mediakit: clips/dec/dec_h264_*.mp4 and their reference md5s clips/ref/dec_h264_*.md5),
# carried in the kit because the machine need not have the mediakit (13 Dec: it had none, the timing never ran); with
# no mediakit here, a 1080p60 High profile clip made by ffmpeg (libx264) instead; with neither, the kit is not made.
# SHA256SUMS last. Copy the folder to the machine as it is.
#   FFMPEG_INSTALL=... MESA_INCLUDE=... [MEDIAKIT=...] vtdirect-kit.sh <out> <conformance dir> [ffmpeg]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: vtdirect-kit.sh <out> <conformance dir> [ffmpeg]}; CONF=${2:?conformance dir}; FF=${3:-ffmpeg}
mkdir -p "$OUT/conf"
OUT=$(cd "$OUT" && pwd)
OUT="$OUT" sh "$HERE/../build.sh"
rm -f "$OUT/libavcodec.63.dylib" "$OUT/libavutil.61.dylib"   # the installed ones are used (this change does not touch FFmpeg)
cp "$HERE/vtdirect-ab.sh" "$OUT/"
MK=${MEDIAKIT:-$HOME/b580/mediakit}; mkdir -p "$OUT/clips/dec" "$OUT/clips/ref"; m=0
for c in "$MK"/clips/dec/dec_h264_*.mp4; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4); cp "$c" "$OUT/clips/dec/"; m=$((m + 1))
  if [ -f "$MK/clips/ref/$b.md5" ]; then cp "$MK/clips/ref/$b.md5" "$OUT/clips/ref/"; fi
done
if [ $m = 0 ]; then
  echo "vtdirect-kit: no H.264 clips in $MK/clips/dec: generating a 1080p60 clip with $FF" >&2
  "$FF" -hide_banner -v error -f lavfi -i testsrc2=size=1920x1080:rate=60:duration=10 -c:v libx264 -profile:v high \
    -preset veryfast -pix_fmt yuv420p -g 120 -bf 2 -y "$OUT/clips/dec/dec_h264_gen1080p60.mp4" < /dev/null
  m=1
fi
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
cc -O1 -I"$MESA_INCLUDE" "$HERE/h264parse_test.c" "$HERE/../vtdirect_h264.c" -o "$T/p"
n=0
for f in "$CONF"/*; do
  b=$(basename "$f"); b=${b%.*}
  if "$T/p" "$f" > /dev/null 2>&1; then :; else
    case "$b" in
      CVFI1_SVA_C|CAMA1_TOSHIBA_B|MR4_TANDBERG_C) ;;   # not covered: libavcodec from StartSession on
      *) continue;;
    esac
  fi
  "$FF" -hide_banner -v error -i "$f" -c copy -y "$OUT/conf/$b.mp4" && n=$((n + 1))
done
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "kit: $OUT ($n conformance streams, $m timing clips)"
