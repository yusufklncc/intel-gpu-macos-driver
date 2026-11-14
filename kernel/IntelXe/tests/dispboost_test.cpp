// SPDX-License-Identifier: MIT
// dispboost_test: offline check of the display boost lease (xe_slpcboost.h, XeDispLease; docs/design/DESIGN-display-boost.md).
//   1. the streak: one late frame does not boost; the second within a lease length does; misses far apart never do;
//      disp_boost_misses 1 boosts on the first (i915's rule)
//   2. the floor: the ceiling (RP0, or slpc_max) or disp_boost_mhz clamped to it; never below slpc_min; the higher of the
//      display lease and slpc_boost's; the GuC told once, and back to the administrator's floor at the end
//   3. renewal: misses during the lease move its end; it ends a lease length after the last miss; the time boosted
//   4. rate limit: 5 new leases a second, renewals not counted; a new second takes them again
//   5. gates: off, freq_manual, the power-saving profile, a GT that cannot take it: counted, nothing changes
//   6. sleep and switching off end the lease at once (time counted to then); sleep leaves nothing to send
//   7. slpc_max lowered during a lease: the floor follows it at once
//   8. the timer late: a miss after the lease's end but before its timer ran starts a new streak, the old lease's time
//      counted to its end; xeBoostNextNs names the earlier of the two leases' ends
// c++ -std=c++17 -Wall -Wextra -I.. -o dispboost_test dispboost_test.cpp && ./dispboost_test
#include <stdio.h>
#include "xe_slpcboost.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static constexpr uint64_t s = 1000000000ull, ms = 1000000ull;
static constexpr uint64_t L = 50 * ms;   // debug.intelxe.disp_boost_ms default

// xe_core.cpp boostSync as far as the GuC is concerned: send what is owed.
static uint32_t sync(XeSlpcBoost &b, const XeSlpcBounds &bd, uint64_t now) {
	const uint32_t m = xeBoostToSend(b, bd, now);
	if (m) {
		b.sentMhz = m;
		b.sends++;
	}
	return m;
}
// A booted GT: slpcParams sent the administrator's floor.
static XeSlpcBoost fresh(const XeSlpcBounds &bd) {
	XeSlpcBoost b = {};
	b.sentMhz = xeSlpcUserFloor(bd);
	b.sentMaxMhz = xeSlpcCeiling(bd);
	return b;
}
static XeDispVerdict miss(XeSlpcBoost &b, uint64_t now, uint32_t misses = 2, uint32_t mhz = 0, XeDispGate g = kXeDispGateOk) {
	xeBoostExpire(b, now);   // the glue (dispRun) runs the timer's work first
	return xeDispMiss(b.disp, now, L, misses, mhz, g);
}

int main() {
	const XeSlpcBounds def = {400, 2850, 0, 0};   // the B580: RPn 400, RP0 2850; slpc_min/max at their defaults
	const uint64_t t = 7 * s;

	// 1.
	{
		XeSlpcBoost b = fresh(def);
		CHECK(miss(b, t) == kXeDispStreak && !xeDispActive(b.disp, t) && sync(b, def, t) == 0, "one late frame: nothing");
		CHECK(miss(b, t + 11 * ms) == kXeDispGranted && xeDispActive(b.disp, t + 11 * ms), "the second, 11 ms later: lease");
		CHECK(b.disp.grants == 1 && b.disp.streakMisses == 1 && b.disp.misses == 2, "counted");
		XeSlpcBoost f = fresh(def);
		for (int i = 0; i < 10; i++) CHECK(miss(f, t + (uint64_t)i * 200 * ms) == kXeDispStreak, "misses 200 ms apart (%d)", i);
		CHECK(!f.disp.grants && f.disp.streakMisses == 10, "never a lease");
		CHECK(miss(f, t + 1800 * ms + L - 1) == kXeDispGranted, "but one within a lease length of the last: lease");
		XeSlpcBoost one = fresh(def);
		CHECK(miss(one, t, 1) == kXeDispGranted, "disp_boost_misses 1: the first miss boosts");
		XeSlpcBoost three = fresh(def);
		CHECK(miss(three, t, 3) == kXeDispStreak && miss(three, t + 5 * ms, 3) == kXeDispStreak &&
			miss(three, t + 10 * ms, 3) == kXeDispGranted, "disp_boost_misses 3: the third");
	}
	// 2.
	{
		XeSlpcBoost b = fresh(def);
		miss(b, t);
		miss(b, t + 5 * ms);
		CHECK(xeDispFloor(b.disp, def, t + 5 * ms) == 2850 && xeBoostEffectiveMin(b, def, t + 5 * ms) == 2850, "floor RP0");
		CHECK(sync(b, def, t + 5 * ms) == 2850 && sync(b, def, t + 6 * ms) == 0, "the GuC told once");
		const uint64_t end = t + 5 * ms + L;
		CHECK(xeBoostNextNs(b, def, t + 6 * ms) == end, "the timer at the lease's end");
		CHECK(xeBoostExpire(b, end) && b.disp.expiries == 1 && !b.disp.mhz, "expired");
		CHECK(sync(b, def, end) == 1200 && xeBoostNextNs(b, def, end) == 0, "back to the default floor, nothing more");

		const XeSlpcBounds capped = {400, 2850, 0, 2400};   // slpc_max 2400
		XeSlpcBoost c = fresh(capped);
		miss(c, t, 1);
		CHECK(xeBoostEffectiveMin(c, capped, t) == 2400, "never above slpc_max");
		XeSlpcBoost m = fresh(def);
		miss(m, t, 1, 2000);
		CHECK(xeBoostEffectiveMin(m, def, t) == 2000, "disp_boost_mhz 2000");
		XeSlpcBoost m2 = fresh(capped);
		miss(m2, t, 1, 2600);
		CHECK(xeBoostEffectiveMin(m2, capped, t) == 2400, "disp_boost_mhz above slpc_max: slpc_max");
		const XeSlpcBounds high = {400, 2850, 2600, 0};   // slpc_min 2600
		XeSlpcBoost h = fresh(high);
		miss(h, t, 1, 2000);
		CHECK(xeBoostEffectiveMin(h, high, t) == 2600 && sync(h, high, t) == 0, "never below slpc_min, nothing to send");
		XeSlpcBoost u = fresh(def);   // slpc_boost's lease of 2000 for 10 s and a display lease
		CHECK(xeBoostWrite(u, t, 2000, def, true) == kXeBoostGranted && sync(u, def, t) == 2000, "slpc_boost 2000");
		miss(u, t + 1 * ms, 1);
		CHECK(xeBoostEffectiveMin(u, def, t + 1 * ms) == 2850 && sync(u, def, t + 1 * ms) == 2850, "the higher: display");
		CHECK(xeBoostNextNs(u, def, t + 2 * ms) == t + 1 * ms + L, "the timer at the display lease's end (the earlier)");
		xeBoostExpire(u, t + 1 * ms + L);
		CHECK(sync(u, def, t + 1 * ms + L) == 2000 && xeBoostNextNs(u, def, t + 1 * ms + L) == t + 10 * s,
			"back to slpc_boost's floor, then its end");
		CHECK(u.expiries == 0 && u.disp.expiries == 1, "only the display lease expired");
	}
	// 3.
	{
		XeSlpcBoost b = fresh(def);
		miss(b, t, 1);
		uint64_t last = t;
		for (int i = 1; i <= 20; i++) {   // late frames every 11 ms (90 Hz) for 220 ms
			last = t + (uint64_t)i * 11 * ms;
			CHECK(miss(b, last, 1) == kXeDispRenewed, "renewed (%d)", i);
		}
		CHECK(b.disp.grants == 1 && b.disp.renewals == 20 && sync(b, def, last) == 2850 && b.sends == 1, "one lease, one send");
		CHECK(xeDispActive(b.disp, last + L - 1) && !xeDispActive(b.disp, last + L), "ends a lease length after the last");
		CHECK(xeDispBoostedNs(b.disp, last) == 220 * ms, "boosted 220 ms so far");
		xeBoostExpire(b, last + L + 3 * ms);   // the timer a little late
		CHECK(b.disp.boostedNs == 220 * ms + L, "boosted until its end, not until the timer ran (%llu)",
			(unsigned long long)b.disp.boostedNs);
	}
	// 4.
	{
		XeSlpcBoost b = fresh(def);
		uint64_t now = t;
		for (int i = 0; i < 5; i++) {   // five leases, each ended by its timer before the next miss
			CHECK(miss(b, now, 1) == kXeDispGranted, "lease %d", i);
			CHECK(miss(b, now + 10 * ms, 1) == kXeDispRenewed, "renewal %d (not counted)", i);
			now += 10 * ms + L + 1 * ms;
			xeBoostExpire(b, now);
		}
		CHECK(now < t + s && miss(b, now, 1) == kXeDispRateLimited && b.disp.rateLimited == 1 && !xeDispActive(b.disp, now),
			"the sixth in that second refused");
		CHECK(miss(b, t + s, 1) == kXeDispGranted, "a new second");
	}
	// 5.
	{
		XeSlpcBoost b = fresh(def);
		CHECK(miss(b, t, 1, 0, kXeDispGateOff) == kXeDispIgnored && b.disp.off == 1, "off");
		CHECK(miss(b, t, 1, 0, kXeDispGateManual) == kXeDispIgnored && b.disp.manual == 1, "freq_manual");
		CHECK(miss(b, t, 1, 0, kXeDispGatePowerSaving) == kXeDispIgnored && b.disp.powerSaving == 1, "power saving");
		CHECK(miss(b, t, 1, 0, kXeDispGateNotReady) == kXeDispIgnored && b.disp.notReady == 1, "not ready");
		CHECK(b.disp.misses == 4 && !b.disp.grants && !b.disp.mhz && sync(b, def, t) == 0, "nothing changed");
		CHECK(miss(b, t + 1 * ms, 2) == kXeDispStreak, "a gated miss is no part of a streak");
	}
	// 6.
	{
		XeSlpcBoost b = fresh(def);
		miss(b, t, 1);
		sync(b, def, t);
		xeBoostSleep(b, t + 20 * ms);
		CHECK(b.disp.sleepEnds == 1 && !b.disp.mhz && b.disp.boostedNs == 20 * ms, "sleep ends it, 20 ms boosted");
		CHECK(xeBoostToSend(b, def, t + 21 * ms) == 0 && xeBoostNextNs(b, def, t + 21 * ms) == 0, "nothing to send after sleep");
		XeSlpcBoost o = fresh(def);
		miss(o, t, 1);
		sync(o, def, t);
		CHECK(xeDispSwitchOff(o.disp, t + 30 * ms) && o.disp.knobEnds == 1 && o.disp.boostedNs == 30 * ms, "switched off");
		CHECK(sync(o, def, t + 30 * ms) == 1200, "the floor back at once");
		CHECK(!xeDispSwitchOff(o.disp, t + 31 * ms) && o.disp.knobEnds == 1, "again: nothing");
		XeSlpcBoost z = fresh(def);
		xeBoostSleep(z);
		CHECK(!z.disp.sleepEnds, "sleep without a lease");
	}
	// 7.
	{
		XeSlpcBoost b = fresh(def);
		miss(b, t, 1);
		CHECK(sync(b, def, t) == 2850, "RP0");
		const XeSlpcBounds lowered = {400, 2850, 0, 2000};
		CHECK(xeBoostEffectiveMin(b, lowered, t + 1 * ms) == 2000 && sync(b, lowered, t + 1 * ms) == 2000,
			"slpc_max 2000 during it: 2000 at once");
	}
	// 8.
	{
		XeSlpcBoost b = fresh(def);
		miss(b, t, 1);
		const uint64_t late = t + L + 5 * ms;   // past the end, the timer has not run
		CHECK(xeDispMiss(b.disp, late, L, 2, 0, kXeDispGateOk) == kXeDispStreak && !b.disp.mhz && b.disp.boostedNs == L,
			"a new streak; the old lease counted to its end");
		CHECK(b.disp.expiries == 1, "counted as expired (grants = expiries + sleep ends + switch-offs + the running one)");
		CHECK(xeDispMiss(b.disp, late + 4 * ms, L, 2, 0, kXeDispGateOk) == kXeDispGranted, "and the next starts a lease");
	}

	printf("dispboost_test: %d checks, %d failed\n", checks, fails);
	return fails != 0;
}
