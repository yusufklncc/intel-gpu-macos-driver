# SPDX-License-Identifier: MIT
# trace-filter.awk: mmiotrace text in, the same text out without the accesses that fall inside the given address
# ranges: the GPU's VRAM or aperture BARs, where the console's frame-buffer writes would bury the register traffic.
#   awk -v ranges="0xSTART 0xEND [0xSTART 0xEND ...]" -v stats=FILE -f trace-filter.awk RAW > KEPT
# mmiotrace's line formats (kernel/trace/trace_mmiotrace.c): "R|W width secs.usecs map-id phys value pc pid",
# "MAP secs map-id phys virt len pc pid", "UNMAP secs map-id virt pid", "MARK secs text", "PCIDEV ...", "VERSION ...".
# MAP, UNMAP, MARK and PCIDEV lines are always kept. Addresses compare as zero-padded strings, so there is no 64-bit
# arithmetic in awk (busybox awk's numbers are doubles).
function pad(h) { h = tolower(h); sub(/^0x/, "", h); return "x" substr("0000000000000000", 1, 16 - length(h)) h }
BEGIN {
	n = split(ranges, r, " ")
	m = 0
	for (i = 1; i + 1 <= n; i += 2) { m++; lo[m] = pad(r[i]); hi[m] = pad(r[i + 1]) }
}
($1 == "R" || $1 == "W") && NF >= 5 {
	rw++
	a = pad($5)
	for (i = 1; i <= m; i++) if (a >= lo[i] && a <= hi[i]) { dropped++; next }
}
{ kept++; print }
END { if (stats != "") printf "lines %d\nkept %d\naccesses %d\ndropped %d\n", NR, kept, rw, dropped > stats }
