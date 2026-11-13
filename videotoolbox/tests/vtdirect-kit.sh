#!/bin/sh
# SPDX-License-Identifier: MIT
# vtdirect-kit.sh <out> <H.264 conformance dir> [ffmpeg]: the folder the night A/B (vtdirect-ab.sh) runs from on the B580
# machine: the decoder test build and vtdec (build.sh), vtdirect-ab.sh, and
#   conf/: the JVT H.264 conformance streams of <H.264 conformance dir> (tests/h264/run.sh --conformance downloads them)
#          remuxed into MP4 (ffmpeg -c copy), those the direct path covers (its parser agrees with libavcodec on them
#          here) and three it hands to libavcodec at the start (fields, MBAFF, frame_num gaps);
#   conf-hevc/: with HEVC_CONF=<dir> (tests/hevc/run.sh --conformance downloads the JCT-VC streams there, <name>.bit),
#          the HEVC conformance streams the direct path covers (hevcparse_test parses them to the end; with
#          HEVC_GOLDEN=1 only those whose golden file mkgolden.py makes and the parser matches) as MP4 (none to
#          hand to libavcodec: the one HEVC_v1 stream the path does not cover, TSUNEQBD_A_MAIN10_Technicolor_2 with
#          luma and chroma bit depths that differ, libavcodec does not decode either). The parameter sets stay in band:
#          FFmpeg's MP4 muxer drops every in-band VPS/SPS/PPS from an 'hvc1' track (the sample entry is to hold them
#          all), so the 13 streams of the kit that send other parameter sets mid-stream (INITQP_*, SLIST_*, TILES_*,
#          DELTAQP_*, DBLK_A/B, TSKIP_A: mostly a PPS per picture) lost them, and both paths decoded those pictures with
#          the first PPS (the 13 Nov B580 run: 12 of them wrong on both paths, 5 of those reported as DIFFER). Muxed as
#          'hev1' (which keeps them), then the sample entry renamed 'hvc1' (hevc/inband-hvc1.py): VideoToolbox has no
#          'hev1' decoder of its own (Apple's: kVTCouldNotFindVideoDecoderErr), and an 'hvc1' track with the parameter
#          sets also in band decodes as published with Apple's decoder too (checked 13 Nov on an Apple-silicon Mac:
#          INITQP_A, SLIST_B/D, TILES_B, DELTAQP_A, TSKIP_A, RPS_B, MAXBINS_C every picture right).
#          A stream whose packets come out with one timestamp (MAXBINS_C_TI_5: all 0, a track of zero duration that
#          AVFoundation reads no sample from) is muxed again with the packets numbered in decode order (setts).
#          conf-hevc/ref/<name>.md5: the software reference, libavcodec's HEVC decoder on the CPU over the .bit, one MD5
#          per picture of the NV12 / P010 planes (vtdec --md5's format), sorted (vtdec lists decode order); the picture
#          is the visible size taken at the top left of the decoded one, as VideoToolbox decoders hand it over (Apple's
#          and ours: CONFWIN_A_Sony_1's left/top conformance window offsets are not applied to the pixels). Made only
#          where libavcodec's whole output matches the yuv MD5 published in <name>.zip (or the zip has none): 13 Nov,
#          142 of 144; not SAODBLK_A/B_MainConcept_4, where both B580 paths and Apple's decoder agree with each other
#          and not with libavcodec. vtdirect-ab.sh compares both paths with it;
#   conf-av1/: the recorded SVT-AV1 streams of tests/av1 and, with AV1_CONF=<dir> (tests/av1/run.sh --conformance
#          downloads libaom's test vectors and FFmpeg's FATE AV1 samples there), those the direct path's parser takes to
#          the end (with AV1_GOLDEN=1 only those whose golden file mkgolden.py makes and the parser matches), as MP4
#          ('av01', av1C); left out: monochrome and spatial-layer streams (the plug-in refuses them at StartSession on
#          both paths) and svt-sframe (a 16384x8704 maximum frame size: libavcodec's pictures would be that large);
#          kept: the streams with frames of another size, which the direct path hands to libavcodec mid-stream;
#   conf-vp9/: the recorded libvpx streams of tests/vp9 (seg-mapnodata: the segment map without the data, which
#          libavcodec's Vulkan hwaccel decodes with stale probabilities) and, with VP9_CONF=<dir> (tests/vp9/run.sh
#          --conformance downloads libvpx's test vectors there with their published per-frame MD5s), those the direct
#          path's parser takes to the end (with VP9_GOLDEN=1 only those whose golden file mkgolden.py makes and the
#          parser matches), as MP4 ('vp09', vpcC; -copyinkf: vp90-2-16-intra-only starts without a key frame; the
#          packets numbered with a duration each and no edit list: FFmpeg's MP4 muxer gave WebM packets without
#          durations an edit list that ends before the last sample, which AVAssetReader then does not read - the 13 Nov
#          kit lost the last picture of vp90-2-10-show-existing-frame and vp90-2-20-big_superframe-01/02 on both paths,
#          and the only one of vp90-2-15-segkey); left out: profiles 1 / 3 and 12-bit (the plug-in refuses them at
#          StartSession on both paths), frames wider or taller than 16384 (vp90-2-13-largescaling, 20400x120: refused
#          as larger than the B580 decodes) and samples that show more than one frame (vp90-2-22-svc_1280x720_3: three
#          spatial layers per superframe, a VideoToolbox decoder answers a sample with one picture); kept: the resize
#          streams (the direct path decodes them in one session; libavcodec makes a new one at each size change).
#          conf-vp9/ref/<name>.md5: the software reference, libvpx's decoder (libvpx-vp9 through ffmpeg), one MD5 per
#          picture of the NV12 / P010 planes (sorted), made only where its yuv output equals libvpx's published MD5 for
#          every frame (conformance) or libavcodec's software decode (recorded streams). (libvpx rather than libavcodec:
#          libavcodec's software VP9 decoder differs from the published MD5 after the key frame that grows
#          vp90-2-14-resize-fp-tiles-* to their largest size, 11 streams);
#   clips/: what the timing part decodes: the mediakit's H.264, HEVC and AV1 clips ($MEDIAKIT, default ~/b580/mediakit:
#          clips/dec/dec_h264_*.mp4, dec_hevc8_*, dec_hevc10_*, dec_av1_8_*, dec_av1_10_*, dec_vp9_8_* and dec_vp9_10_* but not the 4320p ones, and their reference md5s
#          clips/ref/<clip>.md5), carried in the kit because the machine need not have the mediakit (13 Nov: it had none,
#          the timing never ran); with no mediakit here, 1080p60 clips made by ffmpeg (libx264 High, libx265 Main)
#          (libsvtav1 for AV1, libvpx-vp9 for VP9) instead; with neither, the kit is not made.
# SHA256SUMS last. Copy the folder to the machine as it is.
#   FFMPEG_INSTALL=... MESA_INCLUDE=... [MEDIAKIT=...] [HEVC_CONF=...] [AV1_CONF=...] [VP9_CONF=...] vtdirect-kit.sh <out> <conformance dir> [ffmpeg]
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=${1:?usage: vtdirect-kit.sh <out> <conformance dir> [ffmpeg]}; CONF=${2:?conformance dir}; FF=${3:-ffmpeg}
mkdir -p "$OUT/conf" "$OUT/conf-hevc" "$OUT/conf-av1" "$OUT/conf-vp9/ref"
OUT=$(cd "$OUT" && pwd)
OUT="$OUT" sh "$HERE/../build.sh"
rm -f "$OUT/libavcodec.63.dylib" "$OUT/libavutil.61.dylib"   # the installed ones are used (this change does not touch FFmpeg)
cp "$HERE/vtdirect-ab.sh" "$OUT/"
MK=${MEDIAKIT:-$HOME/b580/mediakit}; mkdir -p "$OUT/clips/dec" "$OUT/clips/ref"; m=0 mh=0 ma=0 mv=0
for c in "$MK"/clips/dec/dec_h264_*.mp4 "$MK"/clips/dec/dec_hevc8_*.mp4 "$MK"/clips/dec/dec_hevc10_*.mp4 "$MK"/clips/dec/dec_av1_8_*.mp4 "$MK"/clips/dec/dec_av1_10_*.mp4 \
         "$MK"/clips/dec/dec_vp9_8_*.mp4 "$MK"/clips/dec/dec_vp9_10_*.mp4; do
  [ -f "$c" ] || continue
  b=$(basename "$c" .mp4)
  case "$b" in *_4320p*) continue;; dec_hevc*) mh=$((mh + 1));; dec_av1*) ma=$((ma + 1));; dec_vp9*) mv=$((mv + 1));; *) m=$((m + 1));; esac
  cp "$c" "$OUT/clips/dec/"
  if [ -f "$MK/clips/ref/$b.md5" ]; then cp "$MK/clips/ref/$b.md5" "$OUT/clips/ref/"; fi
done
gen() {   # gen <name> <ffmpeg encoder options...>: a 1080p60 clip when the mediakit has none
  n=$1; shift
  echo "vtdirect-kit: no $n clips in $MK/clips/dec: generating a 1080p60 clip with $FF" >&2
  "$FF" -hide_banner -v error -f lavfi -i testsrc2=size=1920x1080:rate=60:duration=10 "$@" \
    -pix_fmt yuv420p -g 120 -y "$OUT/clips/dec/dec_${n}_gen1080p60.mp4" < /dev/null
}
if [ $m = 0 ]; then gen h264 -c:v libx264 -profile:v high -preset veryfast -bf 2; m=1; fi
if [ $mh = 0 ]; then gen hevc8 -c:v libx265 -tag:v hvc1 -preset veryfast -bf 2; mh=1; fi
if [ $ma = 0 ]; then gen av1_8 -c:v libsvtav1 -preset 10; ma=1; fi
if [ $mv = 0 ]; then gen vp9_8 -c:v libvpx-vp9 -deadline realtime -cpu-used 8 -b:v 8M; mv=1; fi
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
nh=0 nr=0 nbad=0
if [ -n "${HEVC_CONF:-}" ]; then
  mkdir -p "$OUT/conf-hevc/ref"
  FP=$(dirname "$(command -v "$FF")")/ffprobe; [ -x "$FP" ] || FP=$(command -v ffprobe || true)
  cc -O1 -I"$MESA_INCLUDE" "$HERE/hevcparse_test.c" "$HERE/../vtdirect_hevc.c" -o "$T/ph"
  for f in "$HEVC_CONF"/*.bit; do
    b=$(basename "$f" .bit)
    "$T/ph" "$f" > /dev/null 2>&1 || continue
    if [ "${HEVC_GOLDEN:-0}" = 1 ]; then
      python3 "$HERE/hevc/mkgolden.py" "$f" "$FF" > "$T/g" 2> /dev/null || continue
      "$T/ph" "$f" "$T/g" > /dev/null 2>&1 || continue
    fi
    m4=$OUT/conf-hevc/$b.mp4
    if ! "$FF" -hide_banner -v error -i "$f" -c copy -tag:v hev1 -y "$m4" < /dev/null; then rm -f "$m4"; continue; fi
    if [ -n "$FP" ] && [ "$("$FP" -v error -show_entries packet=pts -of csv=p=0 "$m4" | sort -u | wc -l)" -lt 2 ] &&
       [ "$("$FP" -v error -show_entries packet=pts -of csv=p=0 "$m4" | wc -l)" -gt 1 ]; then
      "$FF" -hide_banner -v error -i "$f" -c copy -tag:v hev1 -bsf:v setts=ts=N -time_base 1/25 -y "$m4" < /dev/null || { rm -f "$m4"; continue; }
    fi
    python3 -I "$HERE/hevc/inband-hvc1.py" "$m4" || { rm -f "$m4"; continue; }
    nh=$((nh + 1))
    # the software reference, if libavcodec decodes the stream as published
    pub=$( [ -f "$HEVC_CONF/$b.zip" ] && python3 -I "$HERE/hevc/published-md5.py" "$HEVC_CONF/$b.zip" || true)
    own=$("$FF" -hide_banner -v error -threads 1 -i "$f" -f rawvideo - < /dev/null 2> /dev/null | /sbin/md5 -q || true)
    case " $pub " in "  "|*" $own "*) ;; *) nbad=$((nbad + 1)); echo "vtdirect-kit: $b: no software reference (libavcodec's yuv MD5 $own, published $pub)" >&2; continue;; esac
    # P010 for a 10-bit stream, NV12 otherwise (the kit's streams are 4:2:0, 8 or 10 bits); the visible size at the
    # top left of the decoded picture
    if "$FF" -hide_banner -i "$f" 2>&1 | grep -q 'yuv420p10'; then pf=p010le; else pf=nv12; fi
    vf=format=$pf
    if [ -n "$FP" ]; then wh=$("$FP" -v error -show_entries stream=width,height -of csv=p=0:s=x "$f" | head -1 | tr x :); vf=crop=$wh:0:0,$vf; fi
    if "$FF" -hide_banner -v error -threads 1 -apply_cropping 0 -i "$f" -vf "$vf" -fps_mode passthrough -f framemd5 - < /dev/null > "$T/r" 2> /dev/null; then
      grep -v '^#' "$T/r" | awk -F', *' '{print $NF}' | sort > "$OUT/conf-hevc/ref/$b.md5"; nr=$((nr + 1))
    else echo "vtdirect-kit: $b: the software reference failed" >&2; fi
  done
fi
na=0
cc -O1 -I"$MESA_INCLUDE" "$HERE/av1parse_test.c" "$HERE/../vtdirect_av1.c" -o "$T/pa"
for f in "$HERE"/av1/*.ivf ${AV1_CONF:+"$AV1_CONF"/*.ivf "$AV1_CONF"/*.mkv}; do
  [ -f "$f" ] || continue
  b=$(basename "$f"); b=${b%.*}
  case "$b" in svt-sframe|*monochrome*|*svc-L2T*) continue;; esac
  p=$f   # the parser and mkgolden.py read IVF: a Matroska stream is remuxed for them
  case "$f" in *.mkv) "$FF" -hide_banner -v error -i "$f" -c copy -f ivf -y "$T/x.ivf" < /dev/null || continue; p=$T/x.ivf;; esac
  "$T/pa" "$p" > "$T/o" 2>&1 || continue
  grep -q 'sample: -1 ' "$T/o" && continue   # not covered from the start (the plug-in or libavcodec decodes it)
  if [ "${AV1_GOLDEN:-0}" = 1 ]; then
    python3 "$HERE/av1/mkgolden.py" "$p" "$FF" > "$T/g" 2> /dev/null || continue
    "$T/pa" "$p" "$T/g" > /dev/null 2>&1 || continue
  fi
  if "$FF" -hide_banner -v error -i "$f" -c copy -y "$OUT/conf-av1/$b.mp4" < /dev/null; then na=$((na + 1))
  else rm -f "$OUT/conf-av1/$b.mp4"; fi
done
nv=0 nvr=0 nvbad=0
cc -O1 -I"$MESA_INCLUDE" "$HERE/vp9parse_test.c" "$HERE/../vtdirect_vp9.c" -o "$T/pv"
for f in "$HERE"/vp9/*.ivf ${VP9_CONF:+"$VP9_CONF"/*.webm "$VP9_CONF"/*.ivf}; do
  [ -f "$f" ] || continue
  b=$(basename "$f"); b=${b%.*}
  p=$f   # the parser and mkgolden.py read IVF: a WebM stream is remuxed for them
  case "$f" in *.webm) "$FF" -hide_banner -v error -i "$f" -c copy -copyinkf -f ivf -y "$T/x.ivf" < /dev/null || continue; p=$T/x.ivf;; esac
  "$T/pv" "$p" > "$T/o" 2>&1 || continue
  grep -q 'sample: -1 ' "$T/o" && continue        # profile 1 / 3, 12-bit: refused at StartSession
  grep -qE 'extra=[1-9]' "$T/o" && continue        # several pictures per sample (spatial layers)
  grep -oE 'size=[0-9]+x[0-9]+' "$T/o" | tr 'x=' '  ' | awk '$2 > 16384 || $3 > 16384 {b = 1} END {exit !b}' && continue   # larger than the B580 decodes
  if [ "${VP9_GOLDEN:-0}" = 1 ]; then
    python3 "$HERE/vp9/mkgolden.py" "$p" "$FF" > "$T/g" 2> /dev/null || continue
    "$T/pv" "$p" "$T/g" > /dev/null 2>&1 || continue
  fi
  if ! "$FF" -hide_banner -v error -i "$f" -c copy -copyinkf -bsf:v setts=ts=N:duration=1 -time_base 1/30 -use_editlist 0 \
         -y "$OUT/conf-vp9/$b.mp4" < /dev/null; then rm -f "$OUT/conf-vp9/$b.mp4"; continue; fi
  nv=$((nv + 1))
  # the software reference: libvpx's decoder, checked against the published per-frame MD5s (or, for the recorded
  # streams, against libavcodec's software decoder); every picture at its own size (the resize streams change it)
  if grep -q 'bd=10' "$T/o"; then pf=yuv420p10le npf=p010le; else pf=yuv420p npf=nv12; fi
  own=$("$FF" -hide_banner -v error -threads 1 -c:v libvpx-vp9 -i "$p" -noautoscale -pix_fmt $pf -f framemd5 - < /dev/null 2> /dev/null | grep -v '^#' | awk -F', *' '{print $NF}')
  if [ -f "$f.md5" ]; then pub=$(awk '{print $1}' "$f.md5")
  else pub=$("$FF" -hide_banner -v error -threads 1 -c:v vp9 -i "$p" -noautoscale -pix_fmt $pf -f framemd5 - < /dev/null 2> /dev/null | grep -v '^#' | awk -F', *' '{print $NF}'); fi
  if [ -z "$own" ] || [ "$own" != "$pub" ]; then nvbad=$((nvbad + 1)); echo "vtdirect-kit: $b: no software reference (libvpx's decode differs from the published MD5 / libavcodec's)" >&2; continue; fi
  if "$FF" -hide_banner -v error -threads 1 -c:v libvpx-vp9 -i "$p" -noautoscale -vf format=$npf -fps_mode passthrough -f framemd5 - < /dev/null > "$T/r" 2> /dev/null; then
    grep -v '^#' "$T/r" | awk -F', *' '{print $NF}' | sort > "$OUT/conf-vp9/ref/$b.md5"; nvr=$((nvr + 1))
  else echo "vtdirect-kit: $b: the software reference failed" >&2; fi
done
( cd "$OUT" && find . -type f ! -name SHA256SUMS | sort | xargs shasum -a 256 > SHA256SUMS )
echo "kit: $OUT ($n H.264, $nh HEVC, $na AV1 and $nv VP9 conformance streams, $nr HEVC and $nvr VP9 software references ($nbad / $nvbad without: the reference decoder differs from the published MD5), $m H.264, $mh HEVC, $ma AV1 and $mv VP9 timing clips)"
