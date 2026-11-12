#!/bin/sh
# SPDX-License-Identifier: MIT
# unit-test.sh: the stick's quick checks, without QEMU: trace-filter.awk, collect-linux.sh --redact on trace text, and
# collect-linux.sh as the standalone live-USB script. They run twice: in the pinned Alpine image (busybox sh and tools,
# as on the stick) and in the pinned Debian image (dash and GNU tools, as on a live USB). Needs Docker.
set -eu
HERE=$(cd "$(dirname "$0")/.." && pwd)
REPO=$(cd "$HERE/../.." && pwd)
RC=0
for name in alpine qemu; do
	img=$(awk -v n=$name '$1 == "image" && $2 == n { print $4 }' "$HERE/sources.lock")
	echo "#### $img"
	docker run --rm -v "$REPO":/src:ro "$img" sh /src/tools/probe-stick/test/unit-inside.sh || RC=1
done
exit $RC
