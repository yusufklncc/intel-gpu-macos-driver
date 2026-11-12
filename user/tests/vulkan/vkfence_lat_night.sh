#!/bin/sh
# SPDX-License-Identifier: MIT
# vkfence_lat_night.sh <dir>: the user-fence completion A/B (mesa/patches/0104) on the hackintosh, with the staged ANV
# loaded directly by vkfence_lat (nothing installed). <dir> holds libvulkan_intel.dylib and vkfence_lat (x86_64, built
# on the build Mac). Each run is 2000 tiny submissions (well under 60 s); ANV_UFENCE alternates 0/1/0/1 so a drift in
# clocks shows up as a difference between the two runs of one setting. Output: one block per run, plus the kext's
# direct-submission switch (jobs with a user fence leave direct submission).
D=${1:-$HOME/anv-out}
L=$D/libvulkan_intel.dylib
B=$D/vkfence_lat
[ -x "$B" ] && [ -f "$L" ] || { echo "need $B and $L"; exit 1; }
echo "debug.intelxe.direct=$(sysctl -n debug.intelxe.direct 2>/dev/null || echo '?')  $(date)"
for mode in fill timeline behind empty fillsys; do
	for k in 0 1 0 1; do
		ANV_UFENCE=$k "$B" "$L" "$mode" 2000 || echo "  exit $? (2 = value mismatches)"
	done
done
# the poll budget: 0 = only the check before the kernel wait, 25 us, the default 100 us
for s in 0 25 100; do ANV_UFENCE=1 ANV_UFENCE_SPIN_US=$s "$B" "$L" fill 2000; done
# a compute queue (family 1 on ANV with the B580's CCS: check the printed family list if this errors)
for k in 0 1; do ANV_UFENCE=$k "$B" "$L" fill 2000 1; done
