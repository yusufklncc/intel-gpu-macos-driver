#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the x86_64 tests next to this script (run them under Rosetta on Apple Silicon).
# MESA: a Mesa source tree whose include/ has vulkan/vulkan.h (this repository's mesa/ holds only patches), e.g.
#   MESA=~/mesa sh build.sh
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MESA=${MESA:-$HERE/../../../mesa}
[ -f "$MESA/include/vulkan/vulkan.h" ] || { echo "no $MESA/include/vulkan/vulkan.h: set MESA to a Mesa source tree" >&2; exit 1; }
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vk_smoke" "$HERE/vk_smoke.c"
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vk_videocaps" "$HERE/vk_videocaps.c"
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vksparse" "$HERE/vksparse.c"
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vk_dbuf_empty_set" "$HERE/vk_dbuf_empty_set.c"
