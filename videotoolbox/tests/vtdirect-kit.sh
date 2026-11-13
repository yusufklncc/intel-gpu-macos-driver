#!/bin/sh
# SPDX-License-Identifier: MIT
# vtdirect-kit.sh <out> <H.264 conformance dir> [ffmpeg]: the folder the night A/B (vtdirect-ab.sh) runs from on the B580
# machine: the decoder test build and vtdec (build.sh), vtdirect-ab.sh, and
#   conf/: the JVT H.264 conformance streams of <H.264 conformance dir> (tests/h264/run.sh --conformance downloads them)
#          remuxed into MP4 (ffmpeg -c copy), those the direct path covers (its parser agrees with libavcodec on them
#          here) and three it hands to libavcodec at the start (fields, MBAFF, frame_num gaps);
#   conf-hevc/: with HEVC_CONF=<dir> (tests/hevc/run.sh --conformance downloads the JCT-VC streams there, <name>.bit),
#          the HEVC conformance streams the direct path covers (hevcparse_test parses them to the end; with
#          HEVC_GOLDEN=1 only those whose golden file mkgolden.py makes and the parser matches) as 'hvc1' MP4 (none to
#          hand to libavcodec: the one HEVC_v1 stream the path does not cover, TSUNEQBD_A_MAIN10_Technicolor_2 with
#          luma and chroma bit depths that differ, libavcodec does not decode either);
#   clips/: what the timing part decodes: the mediakit's H.264 and HEVC clips ($MEDIAKIT, default ~/b580/mediakit:
#          clips/dec/dec_h264_*.mp4, dec_hevc8_* and dec_hevc10_* but not the 4320p ones, and their reference md5s
#          clips/ref/<clip>.md5), carried in the kit because the machine need not have the mediakit (13 Nov: it had none,
#          the timing never ran); with no mediakit here, 1080p60 clips made by ffmpeg (libx264 High, libx265 Main)
#          instead; with neither, the kit is not made.
# SHA256SUMS last. Copy the folder to the machine as it is.
#   FFMPEG_INSTALL=... MESA_INCLUDE=... [MEDIAKIT=...] [HEVC_CONF=...] vtdirect-kit.sh <out> <conformance dir> [ffmpeg]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: vtdirect-kit.sh <out> <conformance dir> [ffmpeg]}; CONF=${2:?conformance dir}; FF=${3:-ffmpeg}
mkdir -p "$OUT/conf" "$OUT/conf-hevc"
OUT=$(cd "$OUT" && pwd)
OUT="$OUT" sh "$HERE/../build.sh"
rm -f "$OUT/libavcodec.63.dylib" "$OUT/libavutil.61.dylib"   # the installed ones are used (this change does not touch FFmpeg)
cp "$HERE/vtdirect-ab.sh" "$OUT/"
MK=${MEDIAKIT:-$HOME/b580/mediakit}; mkdir -p "$OUT/clips/dec" "$OUT/clips/ref"; m=0 mh=0
for c in "$MK"/clips/dec/dec_h264_*.mp4 "$MK"/clips/dec/dec_hevc8_*.mp4 "$MK"/clips/dec/dec_hevc10_*.mp4; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4)
  case "$b" in *_4320p*) continue;; dec_hevc*) mh=$((mh + 1));; *) m=$((m + 1));; esac
  cp "$c" "$OUT/clips/dec/"
  if [ -f "$MK/clips/ref/$b.md5" ]; then cp "$MK/clips/ref/$b.md5" "$OUT/clips/ref/"; fi
done
gen() {   # gen <name> <ffmpeg encoder options...>: a 1080p60 clip when the mediakit has none
  n=$1; shift
  echo "vtdirect-kit: no $n clips in $MK/clips/dec: generating a 1080p60 clip with $FF" >&2
  "$FF" -hide_banner -v error -f lavfi -i testsrc2=size=1920x1080:rate=60:duration=10 "$@" \
    -preset veryfast -pix_fmt yuv420p -g 120 -bf 2 -y "$OUT/clips/dec/dec_${n}_gen1080p60.mp4" < /dev/null
}
if [ $m = 0 ]; then gen h264 -c:v libx264 -profile:v high; m=1; fi
if [ $mh = 0 ]; then gen hevc8 -c:v libx265 -tag:v hvc1; mh=1; fi
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
nh=0
if [ -n "${HEVC_CONF:-}" ]; then
  cc -O1 -I"$MESA_INCLUDE" "$HERE/hevcparse_test.c" "$HERE/../vtdirect_hevc.c" -o "$T/ph"
  for f in "$HEVC_CONF"/*.bit; do
    b=$(basename "$f" .bit)
    "$T/ph" "$f" > /dev/null 2>&1 || continue
    if [ "${HEVC_GOLDEN:-0}" = 1 ]; then
      python3 "$HERE/hevc/mkgolden.py" "$f" "$FF" > "$T/g" 2> /dev/null || continue
      "$T/ph" "$f" "$T/g" > /dev/null 2>&1 || continue
    fi
    if "$FF" -hide_banner -v error -i "$f" -c copy -tag:v hvc1 -y "$OUT/conf-hevc/$b.mp4" < /dev/null; then nh=$((nh + 1))
    else rm -f "$OUT/conf-hevc/$b.mp4"; fi
  done
fi
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "kit: $OUT ($n H.264 and $nh HEVC conformance streams, $m H.264 and $mh HEVC timing clips)"
