#!/bin/bash
# SPDX-License-Identifier: MIT
# dpbindex.sh <ffmpeg-9.0.2 source> <build dir> <stream.264|.bit ...>: libavcodec's own DPB array index of every new
# picture (H.264 find_unused_picture(), HEVC alloc_frame()), the number FFmpeg 9.0.2's Vulkan hwaccels passed as the
# DPB slotIndex before ffmpeg-9.0.2-vulkan-decode-dpb-slots-below-maxDpbSlots.patch. Builds, on this Mac (no GPU), a
# copy of the source with one fprintf at each of the two places, as a minimal software-only ffmpeg (H.264/HEVC
# decoders and raw demuxers), and decodes each stream single-threaded (as the hwaccel decodes). The source is copied,
# not changed. One line per stream: "<name> <pictures> <max index> <pictures at or above 16> <at or above 17>"
# (ANV's H.265 / H.264 decode slots).
# 13 Nov: HEVC_v1 (147 streams): only RPS_B_qualcomm_5 / RPS_C_ericsson_5 / RPS_E_qualcomm_5 reach 16 (4 / 1 / 14
# pictures; tests/hevc/dpbslots.py's replay gives the same); JVT H.264 (185): HCBP2_HHI_A and HCMP1_HHI_A reach 19 (14
# pictures at 17 or above each), MR3_TANDBERG_B 16, every other stream at most 15.
set -eu -o pipefail
SRC=$(cd "${1:?usage: dpbindex.sh <ffmpeg-9.0.2 source> <build dir> <streams...>}" && pwd)
B=${2:?build dir}; shift 2
mkdir -p "$B"; B=$(cd "$B" && pwd)
if [ ! -x "$B/ffmpeg" ]; then
  rm -rf "$B/src"; cp -R "$SRC" "$B/src"
  python3 -I - "$B/src" <<'EOF'
import sys
root = sys.argv[1]
def patch(path, anchor, line):
    p = root + '/' + path
    s = open(p).read()
    assert s.count(anchor) == 1, (path, anchor)
    open(p, 'w').write(s.replace(anchor, anchor + line))
patch('libavcodec/h264_slice.c', '    i = find_unused_picture(h);\n', '    fprintf(stderr, "DPBIDX h264 %d\\n", i);\n')
patch('libavcodec/hevc/refs.c', 'frame->pps = av_refstruct_ref_c(s->pps);\n', '        fprintf(stderr, "DPBIDX hevc %d\\n", i);\n')
EOF
  mkdir -p "$B/build"
  ( cd "$B/build" && ../src/configure --disable-everything --enable-decoder=h264,hevc --enable-parser=h264,hevc \
      --enable-demuxer=h264,hevc --enable-protocol=file --enable-muxer=null --enable-encoder=wrapped_avframe \
      --enable-filter=null,format,aformat,anull,aresample --disable-doc --disable-autodetect --disable-network \
      --disable-asm > configure.log 2>&1 && make -j8 ffmpeg > make.log 2>&1 )
  cp "$B/build/ffmpeg" "$B/ffmpeg"
fi
for f in "$@"; do
  case "$f" in *.bit|*.bin|*.265|*.hevc) fmt=hevc;; *) fmt=h264;; esac
  n=$(basename "$f"); n=${n%.*}
  "$B/ffmpeg" -nostdin -hide_banner -v quiet -threads 1 -f $fmt -i "$f" -f null - 2>&1 < /dev/null |
    awk -v s="$n" '/^DPBIDX/ { n++; if ($3 > m) m = $3; if ($3 >= 16) a++; if ($3 >= 17) b++ } END { print s, n + 0, m + 0, a + 0, b + 0 }'
done
