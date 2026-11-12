#!/bin/sh
# SPDX-License-Identifier: MIT
# build.sh: build the Xe probe stick's USB image in Docker. The image is x86_64; on an arm64 Mac Docker emulates it
# (--platform linux/amd64). Nothing is installed on the host.
#
#   tools/probe-stick/build.sh          out/xe-probe-stick-VERSION.img, .img.sha256, .manifest.txt; out/usb/ (the files)
#   tools/probe-stick/build.sh lock     print new apk/tool lines for sources.lock, after Alpine replaced a package file
#
# Inputs are pinned in sources.lock (Docker image by digest, every downloaded file by SHA-256) and cached in out/cache.
set -eu
HERE=$(cd "$(dirname "$0")" && pwd)
REPO=$(cd "$HERE/../.." && pwd)
IMAGE=$(awk '$1 == "image" && $2 == "alpine" { print $4 }' "$HERE/sources.lock")
[ -n "$IMAGE" ] || { echo "error: no alpine image in sources.lock" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || { echo "error: needs Docker" >&2; exit 1; }
mkdir -p "$HERE/out/cache"
# --privileged is not needed: mknod for /dev/console in the image's root needs only Docker's default capabilities
exec docker run --rm --platform linux/amd64 -v "$REPO":/src:ro -v "$HERE/out":/out "$IMAGE" \
	sh /src/tools/probe-stick/image/build-inside.sh "${1:-build}"
