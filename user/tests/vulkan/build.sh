#!/bin/sh
# SPDX-License-Identifier: MIT
# Builds the x86_64 tests next to this script (run them under Rosetta on Apple Silicon).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
MESA=$HERE/../../../mesa
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vk_smoke" "$HERE/vk_smoke.c"
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vk_videocaps" "$HERE/vk_videocaps.c"
clang -arch x86_64 -mmacosx-version-min=14.4 -std=c11 -O1 -g -Wall -I"$MESA/include" \
	-o "$HERE/vksparse" "$HERE/vksparse.c"
