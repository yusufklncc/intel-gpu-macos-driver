// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// nmx_vbldue_test: offline check of nmx_vbldue.h (when a frame the present worker shows was due on the panel), no GPU.
// c++ -std=c++17 -Wall -Wextra -I.. -o nmx_vbldue_test nmx_vbldue_test.cpp && ./nmx_vbldue_test
#include <stdio.h>
#include "nmx_vbldue.h"

static int fails, checks;
#define EQ(a, b) do { checks++; const unsigned long long x_ = (a), y_ = (b); if (x_ != y_) { fails++; \
	printf("FAIL %s:%d: %s = %llu, want %llu\n", __FILE__, __LINE__, #a, x_, y_); } } while (0)

int main()
{
	const uint64_t f180 = 5555555;   // 180 Hz
	// NVRMFlipWaitUs is 5/4 of a frame: 180 Hz publishes 6944 us; no property (50000) and 0 are unknown
	EQ(nmxFrameNsFromFlipWait(6944), 5555200ull);
	EQ(nmxFrameNsFromFlipWait(50000), 0);
	EQ(nmxFrameNsFromFlipWait(0), 0);
	EQ(nmxFrameNsFromFlipWait(20833), 16666400ull);   // 60 Hz

	// no frame length: unknown; no phase: one frame after t0
	EQ(nmxVblDueNs(1000000, 0, 0), 0);
	EQ(nmxVblDueNs(1000000, 0, f180), 1000000 + f180);

	// the phase known, the last vblank before t0: the next one after t0
	const uint64_t v = 100000000;   // a start of vblank at 100 ms
	EQ(nmxVblDueNs(v + 1000000, v, f180), v + f180);            // presented 1 ms after it
	EQ(nmxVblDueNs(v + f180 - 1, v, f180), v + f180);           // just before the next
	EQ(nmxVblDueNs(v, v, f180), v + f180);                      // exactly at a vblank: due at the next one
	EQ(nmxVblDueNs(v + f180, v, f180), v + 2 * f180);
	EQ(nmxVblDueNs(v + 10 * f180 + 7, v, f180), v + 11 * f180); // the phase from 10 frames back
	// the last vblank after t0 (the query ran after the frame was taken): the earliest vblank after t0
	EQ(nmxVblDueNs(v - 1000000, v, f180), v);
	EQ(nmxVblDueNs(v - f180 - 1, v, f180), v - f180);
	EQ(nmxVblDueNs(v - f180, v, f180), v);                      // t0 on a vblank: the one after it
	EQ(nmxVblDueNs(v - 3 * f180 + 5, v, f180), v - 2 * f180);

	// op 4's answer: ~0 (not live), longer ago than two frames, or before the clock's start = no phase
	EQ(nmxLastVblNs(v, 2000000, f180), v - 2000000);
	EQ(nmxLastVblNs(v, ~0ull, f180), 0);
	EQ(nmxLastVblNs(v, 2 * f180 + 1, f180), 0);
	EQ(nmxLastVblNs(1000, 2000, f180), 0);
	EQ(nmxLastVblNs(v, 1000, 0), 0);

	// end to end: the worker took a frame 3 ms after a vblank; the query 1 ms later said 4 ms ago: due 2.56 ms later
	const uint64_t t0 = v + 3000000, q = t0 + 1000000;
	EQ(nmxVblDueNs(t0, nmxLastVblNs(q, 4000000, f180), f180), v + f180);

	printf("nmx_vbldue_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
