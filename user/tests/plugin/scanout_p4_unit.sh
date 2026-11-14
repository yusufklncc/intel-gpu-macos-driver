#!/bin/bash
# SPDX-License-Identifier: MIT
# scanout_p4_unit.sh: build and run tests/plugin/scanout_p4_unit.c on the host (no GPU): gpuperf phase 4's per-frame
# decision for bound WindowServer composites (nullmoth/plugin/nvmtl_scanout_p4.h)
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd); O=${TMPDIR:-/tmp}/scanout_p4_unit.$$
clang -std=gnu11 -O1 -Wall -Wextra -Werror -o "$O" "$H/scanout_p4_unit.c" -framework IOSurface -framework CoreFoundation
rc=0; "$O" || rc=$?; rm -f "$O"; exit $rc
