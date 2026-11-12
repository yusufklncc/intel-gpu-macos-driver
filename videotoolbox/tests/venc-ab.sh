#!/bin/sh
# SPDX-License-Identifier: MIT
# venc-ab.sh <dir>: the encoder upload A/B on the B580 machine, with a libintelxevenc.dylib and vtencq from encoder/build.sh
# in <dir> (nothing installed; libavcodec/libavutil and the AV1 decoder are the installed ones in /Library/GPUBundles/nvmtl).
# For every codec at 1080p and 2160p:
#   q:     quality and identity, pool buffers filled per frame as a client does: --upload copy,import (PSNR, checksums)
#   speed: encoder throughput, --ring 8 --nodecode: copy,import,copy,import (order effect) and import:4, import:8
#          (libavcodec's encode depth through INTELXEVENC_ASYNC)
# HEVC also in Main10 (P010). Every vtencq run is short (seconds); run the whole script in the background:
#   nohup sh venc-ab.sh <dir> > venc-ab.log 2>&1 &
# then: grep -E '^(copy|import)|^UPLOAD|^RESULT|^vtencq|^==' venc-ab.log
set -u
D=${1:?usage: venc-ab.sh <dir with libintelxevenc.dylib and vtencq>}
L=$D/libintelxevenc.dylib Q=$D/vtencq
DEC=/Library/GPUBundles/nvmtl/libintelxeav1dec.dylib
export NVMTL_PROCESS_ENV=1   # the Metal plug-in, if anything loads it, keeps INTELXEVENC_* then
run() { echo "== $*"; "$Q" --lib "$L" "$@"; echo "== exit $?"; }
for size in 1920x1080 3840x2160; do
  for c in hevc h264 av1 hevc10; do
    codec=$c extra=""
    [ "$c" = hevc10 ] && codec=hevc extra="--10bit"
    [ "$c" = av1 ] && extra="--declib $DEC --decoder any"
    nq=120 ns=600; [ "$size" = 3840x2160 ] && nq=60 ns=300
    run --codec "$codec" $extra --size "$size" --frames "$nq" --upload copy,import
    run --codec "$codec" $extra --size "$size" --frames "$ns" --ring 8 --nodecode --patterns noise,scroll \
        --upload copy,import,copy,import,import:4,import:8
  done
done
echo "== done"
