// SPDX-License-Identifier: MIT
// recovery_test: offline check of the job watchdog's recovery rules after a GuC context reset (xe_recovery.h, rb8d).
//   1. xeNotStartedLimitMs: Linux's 5 s TDR (xe_hw_engine.c job_timeout_ms) for a job that never started, the
//      configured timeout when it is lower, off when the watchdog is off
//   2. xeResetDomain: render and compute share a reset domain, copy and video do not
//   3. xeStuckAfterReset: a never-started job is stuck only 2 s after both the reset and its own submission, within
//      the 30 s window; a started job never is; and through a model of the 12 Nov fault (CAT reset at t, the next
//      queue's job submitted 1 ms later, watchdog every 500 ms) the GT reset comes 2-2.5 s after the fault, not 10 s
// c++ -std=c++17 -Wall -Wextra -I.. -o recovery_test recovery_test.cpp && ./recovery_test
#include <stdio.h>
#include "xe_recovery.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

int main() {
	// 1.
	CHECK(xeNotStartedLimitMs(10000) == 5000, "10 s timeout: %llu", (unsigned long long)xeNotStartedLimitMs(10000));
	CHECK(xeNotStartedLimitMs(2000) == 2000, "lower timeout kept");
	CHECK(xeNotStartedLimitMs(0) == 0, "watchdog off");
	CHECK(xeNotStartedLimitMs(5000) == 5000, "equal");

	// 2.
	CHECK(xeResetDomain(0) == xeResetDomain(4) && xeResetDomain(0) >= 0, "render and compute");
	CHECK(xeResetDomain(1) != xeResetDomain(0) && xeResetDomain(2) != xeResetDomain(3) && xeResetDomain(3) != xeResetDomain(0),
		"copy, video decode, video enhance: their own");
	CHECK(xeResetDomain(9) == -1, "unknown class");

	// 3.
	const uint64_t s = 1000000000ull, t = 100 * s;
	CHECK(!xeStuckAfterReset(t + 3 * s, 0, t, false), "no reset");
	CHECK(!xeStuckAfterReset(t + 3 * s, t, t, true), "started");
	CHECK(!xeStuckAfterReset(t + s, t, t - 5 * s, false), "1 s after the reset");
	CHECK(xeStuckAfterReset(t + 2 * s, t, t - 5 * s, false), "2 s after the reset, outstanding from before");
	CHECK(!xeStuckAfterReset(t + 3 * s, t, t + 2 * s, false), "submitted 1 s ago");
	CHECK(xeStuckAfterReset(t + 4 * s, t, t + 2 * s, false), "submitted 2 s ago");
	CHECK(!xeStuckAfterReset(t + 31 * s, t, t + 28 * s, false), "outside the window");
	CHECK(!xeStuckAfterReset(t - s, t, t - 5 * s, false), "reset in the future (clock read before it)");
	// the 12 Nov sequence: CAT at t, the next job submitted at t + 1 ms; the watchdog first sees it at its next run
	// (every 500 ms) and checks again every 500 ms; a GT reset at the first check that finds it stuck
	{
		const uint64_t submit = t + 1000000, phase = 137000000;   // the watchdog's runs are at phase + k * 500 ms
		uint64_t since = 0, resetAt = 0;
		for (uint64_t now = t - s + phase; now < t + 15 * s && !resetAt; now += 500000000ull) {
			if (now < submit) continue;
			if (!since) {
				since = now;
				continue;
			}
			if (xeStuckAfterReset(now, t, since, false)) resetAt = now;
			else if (now - since >= xeNotStartedLimitMs(10000) * 1000000ull) resetAt = now;   // the plain not-started limit
		}
		CHECK(resetAt && resetAt - t >= 2 * s && resetAt - t <= 3 * s, "GT reset %.2f s after the fault",
			resetAt ? (double)(resetAt - t) / s : -1.0);
	}

	printf("recovery_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
