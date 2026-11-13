// SPDX-License-Identifier: MIT
// slpcboost_test: offline check of debug.intelxe.slpc_boost's lease (xe_slpcboost.h).
//   1. bounds: slpcParams' ceiling and floor, the request clamped to [RPn, min(RP0, slpc_max)]
//   2. a lease: granted, the effective minimum raised (never lowered below slpc_min), the GuC told once
//   3. renew: a write before the end moves the end and may change the floor (the latest write wins); expiry after 10 s
//      without a write, the administrator's floor owed to the GuC again
//   4. release: 0 ends it at once; 0 without a lease changes nothing
//   5. rate limit: 10 requests a second whoever makes them, the 11th refused and not counted, a new second takes writes;
//      a release always taken and never counted (also right after EBUSY); two users share the one lease
//   6. GT not ready (reset, sleep): a request refused, a release taken; the owed send retried, nothing owed while unknown
//   7. sleep: the lease ends, nothing is sent until slpcParams runs again
//   8. a lease clamped again when slpc_max is lowered during it
//   9. live range: slpc_min/max writes clamped to RPn..RP0 (0 the default, negative refused), MAX and MIN owed after a
//      write, the lease capped by the new ceiling at once, min above max sent as asked, sleep and the resume's floor
// c++ -std=c++17 -Wall -Wextra -I.. -o slpcboost_test slpcboost_test.cpp && ./slpcboost_test
#include <stdio.h>
#include "xe_slpcboost.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static constexpr uint64_t s = 1000000000ull, ms = 1000000ull;

// The kernel's glue (xe_core.cpp boostSync) as far as the GuC is concerned: send what is owed when ready.
static uint32_t sync(XeSlpcBoost &b, const XeSlpcBounds &bd, uint64_t now, bool ready) {
	const uint32_t m = xeBoostToSend(b, bd, now);
	if (m && ready) {
		b.sentMhz = m;
		b.sends++;
		return m;
	}
	return 0;
}

int main() {
	// the B580: RPn 400, RP0 2850; slpcParams' defaults
	const XeSlpcBounds def = {400, 2850, 1200, 0};

	// 1.
	CHECK(xeSlpcCeiling(def) == 2850, "ceiling RP0: %u", xeSlpcCeiling(def));
	CHECK(xeSlpcCeiling({400, 2850, 1200, 1800}) == 1800, "ceiling slpc_max");
	CHECK(xeSlpcCeiling({400, 2850, 1200, 5000}) == 2850 && xeSlpcCeiling({400, 2850, 1200, 100}) == 400, "ceiling clamped");
	CHECK(xeSlpcUserFloor(def) == 1200 && xeSlpcUserFloor({400, 2850, 0, 0}) == 1200, "floor default");
	CHECK(xeSlpcUserFloor({400, 2850, 100, 0}) == 400 && xeSlpcUserFloor({400, 2850, 9000, 0}) == 2850, "floor clamped");
	CHECK(xeBoostClamp(2400, def) == 2400, "in range");
	CHECK(xeBoostClamp(3000, def) == 2850, "above RP0: %u", xeBoostClamp(3000, def));
	CHECK(xeBoostClamp(3000, {400, 2850, 1200, 1800}) == 1800, "never above slpc_max");
	CHECK(xeBoostClamp(100, def) == 400, "below RPn");
	CHECK(xeBoostClamp(0xffffffffu, def) == 2850, "huge");

	// 2.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 100 * s;
		CHECK(xeBoostToSend(b, def, t) == 0, "nothing sent before SLPC ran (sentMhz unknown)");
		b.sentMhz = xeBoostEffectiveMin(b, def, t);   // slpcParams at bring-up
		CHECK(b.sentMhz == 1200, "bring-up floor %u", b.sentMhz);
		CHECK(xeBoostWrite(b, t, 2400, def, true) == kXeBoostGranted && b.grants == 1, "granted");
		CHECK(xeBoostFloor(b, def, t) == 2400 && xeBoostEffectiveMin(b, def, t) == 2400, "floor 2400");
		CHECK(xeBoostLeftMs(b, t) == 10000, "10 s left: %llu", (unsigned long long)xeBoostLeftMs(b, t));
		CHECK(sync(b, def, t, true) == 2400, "sent 2400");
		CHECK(sync(b, def, t + ms, true) == 0, "sent once");
		CHECK(xeBoostNextNs(b, def, t) == t + kXeBoostLeaseNs, "timer at the end");
		// a lease below the administrator's floor does not lower it
		XeSlpcBoost l = {};
		l.sentMhz = 2000;
		const XeSlpcBounds high = {400, 2850, 2000, 0};
		CHECK(xeBoostWrite(l, t, 800, high, true) == kXeBoostGranted && xeBoostEffectiveMin(l, high, t) == 2000 &&
			xeBoostToSend(l, high, t) == 0, "a lower lease changes nothing");
	}

	// 3.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 50 * s;
		b.sentMhz = 1200;
		xeBoostWrite(b, t, 2400, def, true);
		sync(b, def, t, true);
		CHECK(xeBoostWrite(b, t + 5 * s, 2400, def, true) == kXeBoostRenewed && b.renewals == 1, "renewed");
		CHECK(xeBoostLeftMs(b, t + 5 * s) == 10000 && sync(b, def, t + 5 * s, true) == 0, "renewal: new end, nothing sent");
		CHECK(!xeBoostExpire(b, t + 12 * s) && xeBoostActive(b, t + 12 * s), "still on at 12 s (renewed at 5 s)");
		CHECK(xeBoostWrite(b, t + 13 * s, 2000, def, true) == kXeBoostRenewed && sync(b, def, t + 13 * s, true) == 2000,
			"the latest write wins: 2000 sent");
		const uint64_t end = t + 23 * s;
		CHECK(xeBoostFloor(b, def, end - 1) == 2000 && xeBoostFloor(b, def, end) == 0, "ends 10 s after the last write");
		CHECK(xeBoostExpire(b, end) && b.expiries == 1 && b.askedMhz == 0, "expired");
		CHECK(sync(b, def, end, true) == 1200, "the administrator's floor back");
		CHECK(xeBoostNextNs(b, def, end) == 0 && !xeBoostExpire(b, end + s), "nothing more to do");
		CHECK(xeBoostWrite(b, end + s, 2400, def, true) == kXeBoostGranted && b.grants == 2, "a new lease after the end");
		// the app's pattern: a write every 5 s keeps it; a client that stops leaves it for at most 10 s
		XeSlpcBoost a = {};
		a.sentMhz = 1200;
		uint64_t now = 0;
		for (int i = 0; i < 20; i++, now += 5 * s) {
			xeBoostExpire(a, now);
			xeBoostWrite(a, now, 2400, def, true);
			sync(a, def, now, true);
		}
		CHECK(a.grants == 1 && a.renewals == 19 && a.sends == 1 && a.sentMhz == 2400, "renewed every 5 s: one grant, one send");
		CHECK(xeBoostActive(a, now + 4 * s) && !xeBoostActive(a, now + 5 * s), "the client stopped: ends 10 s after its last write");
	}

	// 4.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 10 * s;
		b.sentMhz = 1200;
		CHECK(xeBoostWrite(b, t, 0, def, true) == kXeBoostNoLease && b.releases == 0 && sync(b, def, t, true) == 0,
			"release without a lease");
		xeBoostWrite(b, t, 2850, def, true);
		sync(b, def, t, true);
		CHECK(xeBoostWrite(b, t + s, 0, def, true) == kXeBoostReleased && b.releases == 1 && !xeBoostActive(b, t + s), "released");
		CHECK(sync(b, def, t + s, true) == 1200 && xeBoostNextNs(b, def, t + s) == 0, "floor back at once, no timer");
	}

	// 5.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 7 * s + 300 * ms;
		b.sentMhz = 1200;
		int ok = 0;
		for (int i = 0; i < 10; i++) ok += xeBoostWrite(b, t + i * ms, 2400, def, true) != kXeBoostRateLimited;
		CHECK(ok == 10, "10 writes in a second: %d", ok);
		CHECK(xeBoostWrite(b, t + 500 * ms, 2000, def, true) == kXeBoostRateLimited && b.rateLimited == 1 && b.askedMhz == 2400 &&
			xeBoostActive(b, t + 500 * ms), "the 11th request refused, the lease unchanged");
		CHECK(xeBoostWrite(b, t + 999 * ms, 2000, def, true) == kXeBoostRateLimited && b.rateLimited == 2 && b.askedMhz == 2400,
			"still refused in the same second");
		CHECK(xeBoostWrite(b, t + s, 2000, def, true) == kXeBoostRenewed && b.askedMhz == 2000, "a new second");
		// the night of 13 Nov: 11 writes in a second, then a release in the same second must end the lease at once
		XeSlpcBoost r = {};
		r.sentMhz = 1200;
		for (int i = 0; i < 10; i++) xeBoostWrite(r, t + i * ms, 2400, def, true);
		sync(r, def, t, true);
		CHECK(xeBoostWrite(r, t + 11 * ms, 2400, def, true) == kXeBoostRateLimited, "the 11th request refused");
		const uint32_t limited = r.rateLimited, used = r.windowWrites;
		CHECK(xeBoostWrite(r, t + 12 * ms, 0, def, true) == kXeBoostReleased && r.releases == 1 && !xeBoostActive(r, t + 12 * ms),
			"a release after EBUSY is taken");
		CHECK(r.rateLimited == limited && r.windowWrites == used, "the release not counted against the limit");
		CHECK(sync(r, def, t + 12 * ms, true) == 1200 && xeBoostNextNs(r, def, t + 12 * ms) == 0, "floor back at once, no timer");
		CHECK(xeBoostWrite(r, t + 13 * ms, 0, def, true) == kXeBoostNoLease && r.rateLimited == limited, "a second release: no lease");
		CHECK(xeBoostWrite(r, t + 14 * ms, 2400, def, true) == kXeBoostRateLimited, "a request in that second still refused");
		// releases never use up the window: 10 releases, then 10 requests all taken
		XeSlpcBoost q = {};
		q.sentMhz = 1200;
		const uint64_t u = 60 * s;
		for (int i = 0; i < 50; i++) CHECK(xeBoostWrite(q, u + i * ms, 0, def, true) == kXeBoostNoLease, "release %d", i);
		int taken = 0;
		for (int i = 0; i < 10; i++) taken += xeBoostWrite(q, u + (100 + i) * ms, 2400, def, true) != kXeBoostRateLimited;
		CHECK(taken == 10 && q.rateLimited == 0, "50 releases then 10 requests in one second: %d taken", taken);
		// a release in a reset after the limit: taken too
		CHECK(xeBoostWrite(q, u + 200 * ms, 2400, def, true) == kXeBoostRateLimited &&
			xeBoostWrite(q, u + 201 * ms, 0, def, false) == kXeBoostReleased, "a release while limited and not ready");
		// two users share one lease: B's write replaces A's floor and end, B's release ends it for A too
		XeSlpcBoost two = {};
		two.sentMhz = 1200;
		const uint64_t v = 90 * s;
		xeBoostWrite(two, v, 2400, def, true);                                       // A
		CHECK(xeBoostWrite(two, v + 3 * s, 1800, def, true) == kXeBoostRenewed &&   // B
			xeBoostFloor(two, def, v + 3 * s) == 1800 && xeBoostLeftMs(two, v + 3 * s) == 10000, "the latest write wins");
		CHECK(xeBoostWrite(two, v + 4 * s, 0, def, true) == kXeBoostReleased && !xeBoostActive(two, v + 4 * s),
			"a release by B ends the lease A asked for");
		CHECK(xeBoostWrite(two, v + 5 * s, 2400, def, true) == kXeBoostGranted, "A's next renewal takes a new lease");
		// a flood: at most 10 a second get through
		XeSlpcBoost f = {};
		f.sentMhz = 1200;
		int through = 0;
		for (uint64_t now = 0; now < 3 * s; now += ms) through += xeBoostWrite(f, now + s, 2400, def, true) != kXeBoostRateLimited;
		CHECK(through <= 31 && through >= 30, "a 1 kHz flood for 3 s: %d through", through);
	}

	// 6.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 20 * s;
		b.sentMhz = 1200;
		CHECK(xeBoostWrite(b, t, 2400, def, false) == kXeBoostNotReady && b.notReady == 1 && !xeBoostActive(b, t),
			"refused in a reset");
		xeBoostWrite(b, t + s, 2400, def, true);
		sync(b, def, t + s, true);
		CHECK(xeBoostWrite(b, t + 2 * s, 0, def, false) == kXeBoostReleased, "a release is taken during a reset");
		CHECK(sync(b, def, t + 2 * s, false) == 0 && xeBoostNextNs(b, def, t + 2 * s) == t + 2 * s + kXeBoostRetryNs,
			"not sent during the reset: retried in 100 ms");
		CHECK(sync(b, def, t + 2 * s + kXeBoostRetryNs, true) == 1200 && xeBoostNextNs(b, def, t + 3 * s) == 0,
			"sent once the GT is back");
	}

	// 7.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 30 * s;
		b.sentMhz = 1200;
		xeBoostWrite(b, t, 2400, def, true);
		sync(b, def, t, true);
		xeBoostSleep(b);
		CHECK(b.sleepEnds == 1 && !xeBoostActive(b, t + s) && b.sentMhz == 0, "sleep ends the lease");
		CHECK(xeBoostToSend(b, def, t + s) == 0 && xeBoostNextNs(b, def, t + s) == 0, "nothing sent while asleep, no timer");
		b.sentMhz = xeBoostEffectiveMin(b, def, t + 60 * s);   // the resume's slpcParams
		CHECK(b.sentMhz == 1200, "the resume sends the administrator's floor");
		xeBoostSleep(b);
		CHECK(b.sleepEnds == 1, "a sleep without a lease is not counted");
	}

	// 8.
	{
		XeSlpcBoost b = {};
		const uint64_t t = 40 * s;
		b.sentMhz = 1200;
		xeBoostWrite(b, t, 3000, def, true);
		CHECK(b.clamped == 1 && sync(b, def, t, true) == 2850, "3000 clamped to RP0");
		const XeSlpcBounds quiet = {400, 2850, 1200, 1800};   // the administrator lowers slpc_max during the lease
		CHECK(xeBoostFloor(b, quiet, t + s) == 1800 && sync(b, quiet, t + s, true) == 1800, "clamped again to the new ceiling");
	}

	// 9. live range (slpc_min / slpc_max writes, xeSlpcKnobWrite's pure part)
	{
		int v = -1;
		CHECK(!xeSlpcKnobValue(-1, 400, 2850, &v) && v == -1, "negative: EINVAL, nothing stored");
		CHECK(xeSlpcKnobValue(0, 400, 2850, &v) && v == 0, "0 = the default, kept as 0");
		CHECK(xeSlpcKnobValue(1500, 400, 2850, &v) && v == 1500, "in range");
		CHECK(xeSlpcKnobValue(9999, 400, 2850, &v) && v == 2850, "above RP0: clamped %d", v);
		CHECK(xeSlpcKnobValue(100, 400, 2850, &v) && v == 400, "below RPn: clamped %d", v);
		CHECK(xeSlpcKnobValue(9999, 0, 0, &v) && v == 9999, "limits not known yet: kept (slpcParams clamps at the start)");
		XeSlpcBoost b = {};
		XeSlpcBounds bd = def;
		const uint64_t t = 50 * s;
		CHECK(xeSlpcMaxToSend(b, bd) == 0, "nothing before SLPC ran (sentMaxMhz unknown)");
		b.sentMhz = xeBoostEffectiveMin(b, bd, t);   // slpcParams
		b.sentMaxMhz = xeSlpcCeiling(bd);
		CHECK(xeSlpcMaxToSend(b, bd) == 0 && xeBoostToSend(b, bd, t) == 0, "nothing owed after slpcParams");
		bd.userMax = 2000;   // zesFrequencySetRange(1500, 2000): max first is not needed (1500 < current max)
		bd.userMin = 1500;
		CHECK(xeSlpcMaxToSend(b, bd) == 2000, "max 2000 owed");
		b.sentMaxMhz = 2000;
		CHECK(sync(b, bd, t, true) == 1500, "min 1500 owed");
		CHECK(xeBoostWrite(b, t, 2400, bd, true) == kXeBoostGranted, "a lease under the new range");
		CHECK(sync(b, bd, t, true) == 2000, "the lease's floor capped by slpc_max 2000");
		bd.userMax = 1800;   // root lowers the ceiling during the lease: the GuC's min follows at once
		CHECK(xeSlpcMaxToSend(b, bd) == 1800, "max 1800 owed");
		b.sentMaxMhz = 1800;
		CHECK(sync(b, bd, t + s, true) == 1800, "min lowered with the ceiling");
		bd.userMin = 2500;   // a floor above the ceiling is sent as asked (Linux: min becomes a fixed request)
		xeBoostWrite(b, t + s, 0, bd, true);
		CHECK(sync(b, bd, t + s, true) == 2500, "min above max: %u", b.sentMhz);
		bd.userMin = 0;   // back to the defaults
		bd.userMax = 0;
		CHECK(xeSlpcMaxToSend(b, bd) == 2850 && (b.sentMaxMhz = 2850) && sync(b, bd, t + s, true) == 1200, "defaults");
		xeBoostSleep(b);
		CHECK(b.sentMaxMhz == 0 && xeSlpcMaxToSend(b, bd) == 0, "sleep: the GuC's max unknown until slpcParams");
		bd.userMin = 1600;   // written while asleep: kept, sent by the resume's slpcParams
		CHECK(xeBoostToSend(b, bd, t + 2 * s) == 0, "nothing sent while asleep");
		CHECK(xeBoostEffectiveMin(b, bd, t + 3 * s) == 1600, "the resume's floor is the new slpc_min");
	}

	printf("slpcboost_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
