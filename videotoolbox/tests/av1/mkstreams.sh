#!/bin/sh
# SPDX-License-Identifier: MIT
# mkstreams.sh: the recorded AV1 streams of this folder (how they were made; they are committed, so the tests do not
# need an encoder). Sources are FFmpeg's testsrc2 / mandelbrot patterns; the encoder is SVT-AV1 4.2 through FFmpeg
# 9.0.2 (libsvtav1), IVF output. Then mkgolden.py for each.
#   mkstreams.sh [ffmpeg]
set -eu
FF=${1:-ffmpeg}
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
src() { "$FF" -hide_banner -nostdin -v error -f lavfi -i "$1" -frames:v "$2" -pix_fmt "$3" -strict -1 -f yuv4mpegpipe -y "$4"; }
src testsrc2=size=176x144:rate=30 48 yuv420p "$T/qcif.y4m"
src testsrc2=size=176x144:rate=30 30 yuv420p10le "$T/qcif10.y4m"
src mandelbrot=size=352x288:rate=30 24 yuv420p "$T/cif.y4m"
src testsrc2=size=200x120:rate=30 24 yuv420p "$T/odd.y4m"
svt() { in=$1 out=$2; shift 2; "$FF" -hide_banner -nostdin -v error -i "$in" -c:v libsvtav1 -svtav1-params "$*" -f ivf -y "$out" 2>/dev/null; }
# random access, 4 temporal layers: hidden alt-ref frames, show_existing_frame, skip mode, compound references
svt "$T/qcif.y4m" svt-ra.ivf "preset=6:keyint=24:hierarchical-levels=3:enable-tf=1:irefresh-type=2:scm=0"
# open GOP (forward key frames): a hidden key frame shown later with show_existing_frame (it refreshes every slot)
svt "$T/qcif.y4m" svt-fwdkf.ivf "preset=8:keyint=16:hierarchical-levels=2:irefresh-type=1:scm=0"
# 10-bit with film grain (grain parameters loaded from a reference: update_grain = 0), loop restoration
svt "$T/qcif10.y4m" svt-grain10.ivf "preset=6:keyint=30:film-grain=12:enable-restoration=1:scm=0"
# 2x2 tiles, 352x288 (non-uniform last tile), quantisation matrices, segmentation (variance AQ), CDEF
svt "$T/cif.y4m" svt-tiles.ivf "preset=7:keyint=12:tile-columns=1:tile-rows=1:enable-qm=1:aq-mode=1:scm=0"
# super-resolution (coded width 3/4 of the upscaled one on inter frames, 10/8 on key frames)
svt "$T/qcif.y4m" svt-superres.ivf "preset=8:keyint=24:superres-mode=1:superres-denom=12:superres-kf-denom=10:scm=0"
# low delay with switch frames (error resilient, frame sizes, ref_order_hint coded)
svt "$T/qcif.y4m" svt-sframe.ivf "preset=8:keyint=48:pred-struct=1:sframe-dist=8:sframe-mode=1:scm=0"
# screen content tools on: intra block copy and palettes; 200x120 (partial superblocks)
svt "$T/odd.y4m" svt-scm.ivf "preset=8:keyint=12:scm=1:enable-intrabc=1"
# lossless (coded lossless: no loop filter / CDEF / restoration, 4x4 transforms)
svt "$T/odd.y4m" svt-lossless.ivf "preset=10:keyint=8:lossless=1:scm=0"
# reference scaling: frames smaller than the sequence's maximum size (the direct path hands such streams to libavcodec)
svt "$T/qcif.y4m" svt-resize.ivf "preset=8:keyint=24:resize-mode=1:resize-denom=12:resize-kf-denom=8:scm=0"
# a still picture (reduced_still_picture_header)
"$FF" -hide_banner -nostdin -v error -i "$T/cif.y4m" -frames:v 1 -c:v libsvtav1 -svtav1-params "avif=1:preset=8" -f ivf -y svt-still.ivf 2>/dev/null
for f in *.ivf; do python3 mkgolden.py "$f" "$FF" > "${f%.ivf}.txt"; done
