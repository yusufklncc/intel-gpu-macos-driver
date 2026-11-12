#!/bin/sh
# SPDX-License-Identifier: MIT
# mkstreams.sh: the recorded H.264 streams of this folder (how they were made; they are committed, so the tests do not
# need x264). Sources are FFmpeg's testsrc2 pattern; encoders x264 0.165 and, for apple-vt, Apple's VideoToolbox H.264
# encoder through FFmpeg (made on an M1 Pro, macOS 27.0.1). Then mkgolden.py for each (FFmpeg 9.0.2).
#   mkstreams.sh [ffmpeg] [x264]
set -eu
FF=${1:-ffmpeg} X=${2:-x264}
cd "$(dirname "$0")"
src() { "$FF" -hide_banner -v error -f lavfi -i "testsrc2=size=$1:rate=30" -frames:v "$2" -f yuv4mpegpipe -y "$3"; }
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
src 176x144 60 "$T/qcif.y4m"
src 64x64 300 "$T/tiny.y4m"
# High: 3 B frames in a pyramid (MMCO 1 for the B references), 4 references, weighted P, an IDR every 15 frames
"$X" --quiet --crf 30 --profile high --bframes 3 --b-pyramid normal --ref 4 --weightp 2 --keyint 15 --min-keyint 15 --scenecut 0 -o bpyramid.264 "$T/qcif.y4m"
# Baseline: CAVLC, no B frames (POC type 2), 3 references, 3 slices per picture
"$X" --quiet --crf 30 --profile baseline --ref 3 --slices 3 --keyint 25 -o baseline-slices.264 "$T/qcif.y4m"
# High: custom 8x8 intra/inter scaling lists in the PPS, temporal direct, implicit weighted B, a strict pyramid, 5 references
"$X" --quiet --crf 30 --profile high --bframes 3 --b-pyramid strict --direct temporal --weightb --ref 5 --keyint 30 \
  --cqm8i "$(python3 -c "print(','.join(str(6+(i%8)+(i//8)*2) for i in range(64)))")" \
  --cqm8p "$(python3 -c "print(','.join(str(9+(i%8)+(i//8)) for i in range(64)))")" -o cqm-temporal.264 "$T/qcif.y4m"
# Main: frame_num and POC LSB wrap-around (no IDR after the first, 300 pictures), one B frame, 2 references
"$X" --quiet --crf 30 --profile main --bframes 1 --ref 2 --keyint infinite -o wrap.264 "$T/tiny.y4m"
# Main: CAVLC, one reference, one B frame, an IDR every 4 pictures (idr_pic_id changes)
"$X" --quiet --crf 30 --profile main --no-cabac --bframes 1 --ref 1 --keyint 4 --min-keyint 4 --scenecut 0 -o idr4-cavlc.264 "$T/qcif.y4m"
# Apple's encoder: High with frame reordering
"$FF" -hide_banner -v error -i "$T/qcif.y4m" -c:v h264_videotoolbox -profile:v high -b:v 150k -g 30 -bf 2 -f h264 -y apple-vt.264
for f in *.264; do python3 mkgolden.py "$f" "$FF" > "${f%.264}.txt"; done
