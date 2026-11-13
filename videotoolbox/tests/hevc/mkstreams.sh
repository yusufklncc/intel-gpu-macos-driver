#!/bin/sh
# SPDX-License-Identifier: MIT
# mkstreams.sh: the recorded HEVC streams of this folder (how they were made; they are committed, so the tests do not
# need x265). Sources are FFmpeg's testsrc2 pattern; encoders libx265 (x265 4.3, through FFmpeg 9.0.2) and, for
# apple-vt*, Apple's VideoToolbox HEVC encoder through FFmpeg (made on an M1 Pro, macOS 27). Then mkgolden.py for each.
#   mkstreams.sh [ffmpeg]
set -eu
FF=${1:-ffmpeg}
cd "$(dirname "$0")"
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
src() { "$FF" -hide_banner -nostdin -v error -f lavfi -i "testsrc2=size=$1:rate=30" -frames:v "$2" -pix_fmt "$3" -strict -1 -f yuv4mpegpipe -y "$4"; }
src 176x144 60 yuv420p "$T/qcif.y4m"
src 176x144 60 yuv420p10le "$T/qcif10.y4m"
src 64x64 300 yuv420p "$T/tiny.y4m"
x265() { in=$1 out=$2; shift 2; "$FF" -hide_banner -nostdin -v error -i "$in" -c:v libx265 -x265-params "log-level=none:$*" -f hevc -y "$out"; }
# Main, open GOP: CRA pictures with RASL leading pictures, 3 B frames in a pyramid, default scaling lists, WPP
x265 "$T/qcif.y4m" x265-opengop.265 "keyint=24:min-keyint=24:scenecut=0:open-gop=1:bframes=3:b-pyramid=1:ref=3:scaling-list=default:wpp=1"
# Main, closed GOP (IDR_W_RADL with RADL pictures), 3 slices (x265 needs WPP for them), weighted P, 4 references, SAO off, AMP on
x265 "$T/qcif.y4m" x265-slices.265 "keyint=20:min-keyint=20:scenecut=0:open-gop=0:radl=2:bframes=2:slices=3:weightp=1:ref=4:wpp=1:sao=0:amp=1"
# Main 10: 4 B frames, B references, weighted B, temporal MVP off, the SPS's RPS candidates (no RPS in the headers)
x265 "$T/qcif10.y4m" x265-main10.265 "keyint=30:min-keyint=30:scenecut=0:bframes=4:b-pyramid=1:weightb=1:ref=3:tmvp=0:profile=main10"
# Main: POC LSB wrap-around (300 pictures after one IDR, log2_max_pic_order_cnt_lsb 8), 2 temporal sub-layers
x265 "$T/tiny.y4m" x265-wrap.265 "keyint=-1:scenecut=0:bframes=1:ref=2:temporal-layers=2"
# Apple's encoder, 8 and 10 bits, with frame reordering
"$FF" -hide_banner -nostdin -v error -i "$T/qcif.y4m" -c:v hevc_videotoolbox -profile:v main -b:v 150k -g 30 -bf 2 -f hevc -y apple-vt.265
"$FF" -hide_banner -nostdin -v error -i "$T/qcif10.y4m" -c:v hevc_videotoolbox -profile:v main10 -pix_fmt p010le -b:v 150k -g 30 -bf 2 -f hevc -y apple-vt-main10.265
for f in *.265; do python3 mkgolden.py "$f" "$FF" > "${f%.265}.txt"; done
