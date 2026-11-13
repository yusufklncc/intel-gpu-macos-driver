#!/bin/sh
# SPDX-License-Identifier: MIT
# mkstreams.sh: the recorded VP9 streams of this folder (how they were made; they are committed, so the tests do not
# need an encoder). Sources are FFmpeg's testsrc2 / mandelbrot patterns; the encoder is libvpx 1.15 through FFmpeg
# 9.0.2 (libvpx-vp9), IVF output. Then mkgolden.py for each.
#   mkstreams.sh [ffmpeg]
set -eu
FF=${1:-ffmpeg}
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
src() { "$FF" -hide_banner -nostdin -v error -f lavfi -i "$1" -frames:v "$2" -pix_fmt "$3" -strict -1 -f yuv4mpegpipe -y "$4"; }
src testsrc2=size=176x144:rate=30 48 yuv420p "$T/qcif.y4m"
src testsrc2=size=176x144:rate=30 30 yuv420p10le "$T/qcif10.y4m"
src mandelbrot=size=352x288:rate=30 60 yuv420p "$T/cif.y4m"
src testsrc2=size=201x121:rate=30 24 yuv420p "$T/odd.y4m"
src testsrc2=size=1024x144:rate=30 12 yuv420p "$T/wide.y4m"
vpx() { in=$1 out=$2; shift 2; "$FF" -hide_banner -nostdin -v error -i "$in" -c:v libvpx-vp9 "$@" -f ivf -y "$out"; }
# two passes (libvpx makes alt-ref frames only with first-pass statistics)
vpx2() { in=$1 out=$2; shift 2
  "$FF" -hide_banner -nostdin -v error -i "$in" -c:v libvpx-vp9 "$@" -pass 1 -passlogfile "$T/pass" -f null - &&
  "$FF" -hide_banner -nostdin -v error -i "$in" -c:v libvpx-vp9 "$@" -pass 2 -passlogfile "$T/pass" -f ivf -y "$out"; }
# good quality with alt-refs: hidden ARF frames in superframes, golden / alt-ref references, sign bias, frame contexts
vpx2 "$T/qcif.y4m" lv-arf.ivf -b:v 300k -auto-alt-ref 1 -lag-in-frames 16 -arnr-maxframes 5 -g 24 -cpu-used 2
# real time with cyclic refresh (aq-mode 3): segmentation map and data on every frame, temporal map prediction
vpx "$T/cif.y4m" lv-aq3-rt.ivf -b:v 300k -aq-mode 3 -deadline realtime -cpu-used 8 -lag-in-frames 0 -g 30
# the same with the data left out where it is unchanged (segedit.py): libavcodec's Vulkan hwaccel passes stale
# segmentation probabilities on these frames ("lavc" lines); libavcodec's software decoder and libvpx decode it
# exactly as lv-aq3-rt.ivf
python3 segedit.py lv-aq3-rt.ivf seg-mapnodata.ivf
# error resilient, variance AQ (segmentation features from the key frame), sharpness, frame-parallel decoding mode
vpx "$T/qcif.y4m" lv-er-aq1.ivf -b:v 200k -error-resilient 1 -aq-mode 1 -sharpness 5 -frame-parallel 1 -lag-in-frames 0 -g 16
# 4 tile columns, 2 tile rows (1024x144: 16 superblocks wide), frame-parallel off
vpx "$T/wide.y4m" lv-tiles.ivf -b:v 600k -tile-columns 2 -tile-rows 1 -frame-parallel 0 -g 12 -cpu-used 4
# Profile 2, 10-bit, with alt-refs
vpx2 "$T/qcif10.y4m" lv-10bit.ivf -b:v 300k -profile:v 2 -auto-alt-ref 1 -lag-in-frames 8 -g 30
# lossless (base_q_idx 0, no deltas), 201x121 (odd sizes, partial superblocks)
vpx "$T/odd.y4m" lv-lossless-odd.ivf -lossless 1 -g 8
for f in *.ivf; do python3 mkgolden.py "$f" "$FF" > "${f%.ivf}.txt"; done
