// SPDX-License-Identifier: MIT
// slpcboost: debug.intelxe.slpc_boost, a ten-second lease on the GT's lowest clock that any user may ask for
// (docs/design/DESIGN-game-mode.md, section 2). The pure parts, so that tests/slpcboost_test.cpp builds them on the host;
// the glue (the sysctl, the timer, the H2G) is in xe_core.cpp next to slpcParams.
//
// Linux (v7.2 xe_guc_pc.c) has one minimum: xe_guc_pc_set_min_freq (sysfs freq0/min_freq, root) checks RPn..RP0,
// returns -EAGAIN while the PC is not ready ("might be in the middle of a gt reset"), and pc_set_min_freq sends
// SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY (min < RPe) and then SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ. The lease uses
// the same two parameters; what it adds is who may ask and for how long:
//  - a write of N MHz asks for a floor of N for kXeBoostLeaseNs after the write; a write before the end renews it (and
//    may change N); 0 ends it. One lease for the whole GPU: the latest write wins;
//  - the floor is clamped to [RPn, the ceiling], the ceiling being debug.intelxe.slpc_max (0 = RP0) clamped to RPn..RP0
//    as slpcParams clamps it, so the lease never lifts the clock above what the administrator allowed. The minimum the
//    GuC is given while a lease runs is the higher of the lease's floor and the administrator's own (slpc_min): a lease
//    never lowers the floor;
//  - at most kXeBoostRateMax writes per kXeBoostRateWindowNs, whoever makes them (EBUSY beyond): a renewal every few
//    seconds needs one;
//  - nothing while the GT is suspended, in a reset, wedged or without SLPC (EAGAIN, as Linux), except a release;
//  - system sleep ends the lease (the GuC is reset; the resume sends the administrator's floor).
#pragma once
#include <stdint.h>

static constexpr uint64_t kXeBoostLeaseNs = 10000000000ull;      // 10 s
static constexpr uint64_t kXeBoostRateWindowNs = 1000000000ull;  // 1 s
static constexpr uint32_t kXeBoostRateMax = 10;                  // writes per window
static constexpr uint64_t kXeBoostRetryNs = 100000000ull;        // a send the GT was not ready for: again in 100 ms
static constexpr uint32_t kXeSlpcDefaultMinMhz = 1200;           // BMG_MIN_FREQ (Wa_14022085890), slpcParams' default

// The GT's frequency limits (from RP_STATE_CAP, MHz) and the administrator's knobs as the sysctls hold them.
struct XeSlpcBounds {
	uint32_t rpn, rp0;
	int userMin;   // debug.intelxe.slpc_min (<= 0: the default floor)
	int userMax;   // debug.intelxe.slpc_max (<= 0: RP0)
};

// slpcParams' ceiling: slpc_max or RP0, within RPn..RP0.
static inline uint32_t xeSlpcCeiling(const XeSlpcBounds &b) {
	uint32_t max = b.userMax > 0 ? (uint32_t)b.userMax : b.rp0;
	if (max > b.rp0) max = b.rp0;
	if (max < b.rpn) max = b.rpn;
	return max;
}

// slpcParams' floor without a lease: slpc_min or 1200, within RPn..RP0.
static inline uint32_t xeSlpcUserFloor(const XeSlpcBounds &b) {
	uint32_t min = b.userMin > 0 ? (uint32_t)b.userMin : kXeSlpcDefaultMinMhz;
	if (min < b.rpn) min = b.rpn;
	if (min > b.rp0) min = b.rp0;
	return min;
}

// A requested floor clamped to [RPn, the ceiling].
static inline uint32_t xeBoostClamp(uint32_t mhz, const XeSlpcBounds &b) {
	const uint32_t ceil = xeSlpcCeiling(b);
	if (mhz > ceil) mhz = ceil;
	if (mhz < b.rpn) mhz = b.rpn;
	return mhz;
}

struct XeSlpcBoost {
	uint32_t askedMhz;     // the active lease's request as written (0 = no lease); clamped where it is used
	uint64_t untilNs;      // when it ends
	uint32_t sentMhz;      // the minimum the GuC was last given (slpcParams or the lease); 0 = unknown, nothing to restore
	uint64_t windowNs;     // the rate limit's current window
	uint32_t windowWrites;
	// counters (debug.intelxe.stats "slpc boost")
	uint32_t grants, renewals, releases, expiries, sleepEnds, rateLimited, notReady, clamped, sends;
};

enum XeBoostVerdict { kXeBoostGranted, kXeBoostRenewed, kXeBoostReleased, kXeBoostNoLease, kXeBoostRateLimited, kXeBoostNotReady };

static inline bool xeBoostActive(const XeSlpcBoost &s, uint64_t now) { return s.askedMhz && now < s.untilNs; }

// The lease's floor now (0 = none), clamped to the current bounds (slpc_max may have changed since the write).
static inline uint32_t xeBoostFloor(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	return xeBoostActive(s, now) ? xeBoostClamp(s.askedMhz, b) : 0;
}

// The minimum the GuC should have now: the administrator's floor, raised to the lease's.
static inline uint32_t xeBoostEffectiveMin(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	const uint32_t user = xeSlpcUserFloor(b), lease = xeBoostFloor(s, b, now);
	return lease > user ? lease : user;
}

// ms left on the lease (0 = none).
static inline uint64_t xeBoostLeftMs(const XeSlpcBoost &s, uint64_t now) {
	return xeBoostActive(s, now) ? (s.untilNs - now + 999999) / 1000000 : 0;
}

// A write of mhz (0 = release) at now. ready: the GT can take a new minimum (up, not suspended, not in reset, SLPC running).
static inline XeBoostVerdict xeBoostWrite(XeSlpcBoost &s, uint64_t now, uint32_t mhz, const XeSlpcBounds &b, bool ready) {
	if (!s.windowNs || now - s.windowNs >= kXeBoostRateWindowNs || now < s.windowNs) {
		s.windowNs = now ? now : 1;
		s.windowWrites = 0;
	}
	if (s.windowWrites >= kXeBoostRateMax) {
		s.rateLimited++;
		return kXeBoostRateLimited;
	}
	s.windowWrites++;
	const bool active = xeBoostActive(s, now);
	if (!mhz) {
		if (!active) {
			s.askedMhz = 0;
			return kXeBoostNoLease;
		}
		s.askedMhz = 0;
		s.untilNs = 0;
		s.releases++;
		return kXeBoostReleased;
	}
	if (!ready) {
		s.notReady++;
		return kXeBoostNotReady;
	}
	if (xeBoostClamp(mhz, b) != mhz) s.clamped++;
	s.askedMhz = mhz;
	s.untilNs = now + kXeBoostLeaseNs;
	if (active) s.renewals++;
	else s.grants++;
	return active ? kXeBoostRenewed : kXeBoostGranted;
}

// The timer: a lease past its end is over. True when one ended now.
static inline bool xeBoostExpire(XeSlpcBoost &s, uint64_t now) {
	if (!s.askedMhz || now < s.untilNs) return false;
	s.askedMhz = 0;
	s.untilNs = 0;
	s.expiries++;
	return true;
}

// System sleep: the lease ends, and the GuC forgets what it was given (the resume's slpcParams sends the administrator's
// floor and sets sentMhz again).
static inline void xeBoostSleep(XeSlpcBoost &s) {
	if (s.askedMhz) s.sleepEnds++;
	s.askedMhz = 0;
	s.untilNs = 0;
	s.sentMhz = 0;
}

// What to send now: the effective minimum when it differs from what the GuC has (0: nothing). Nothing while sentMhz is
// unknown (before SLPC started, during sleep): slpcParams sends the whole set then.
static inline uint32_t xeBoostToSend(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	if (!s.sentMhz) return 0;
	const uint32_t want = xeBoostEffectiveMin(s, b, now);
	return want != s.sentMhz ? want : 0;
}

// When the timer should run next (0 = not needed): the lease's end, or soon when a send is still owed.
static inline uint64_t xeBoostNextNs(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	if (xeBoostToSend(s, b, now)) return now + kXeBoostRetryNs;
	return s.askedMhz ? s.untilNs : 0;
}
