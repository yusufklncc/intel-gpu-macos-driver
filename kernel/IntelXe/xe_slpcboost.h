// SPDX-License-Identifier: MIT
// slpcboost: debug.intelxe.slpc_boost, a ten-second lease on the GT's lowest clock that any user may ask for
// (docs/design/DESIGN-game-mode.md, section 2), and since 14 Nov the kernel's own short display lease on the same floor
// (dispboost, XeDispLease below; docs/design/DESIGN-display-boost.md). The pure parts, so that tests/slpcboost_test.cpp builds them on the host;
// the glue (the sysctl, the timer, the H2G) is in xe_core.cpp next to slpcParams.
//
// Linux (v7.2 xe_guc_pc.c) has one minimum: xe_guc_pc_set_min_freq (sysfs freq0/min_freq, root) checks RPn..RP0,
// returns -EAGAIN while the PC is not ready ("might be in the middle of a gt reset"), and pc_set_min_freq sends
// SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY (min < RPe) and then SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ. The lease uses
// the same two parameters; what it adds is who may ask and for how long:
//  - a write of N MHz asks for a floor of N for kXeBoostLeaseNs after the write; a write before the end renews it (and
//    may change N); 0 ends it. One lease for the whole GPU, not one per requester: the latest write wins. Two users
//    share it: B's write replaces A's floor and end, and a release by either ends it for both (the other's next
//    renewal, every few seconds in the app, takes a new one). No user can hold the clock above the administrator's
//    ceiling or for longer than 10 s after the last write, so sharing costs at most power, never a lower floor;
//  - the floor is clamped to [RPn, the ceiling], the ceiling being debug.intelxe.slpc_max (0 = RP0) clamped to RPn..RP0
//    as slpcParams clamps it, so the lease never lifts the clock above what the administrator allowed. The minimum the
//    GuC is given while a lease runs is the higher of the lease's floor and the administrator's own (slpc_min): a lease
//    never lowers the floor;
//  - at most kXeBoostRateMax requests (N > 0) per kXeBoostRateWindowNs, whoever makes them (EBUSY beyond): a renewal
//    every few seconds needs one. Releases (0) are always taken and not counted;
//  - nothing while the GT is suspended, in a reset, wedged or without SLPC (EAGAIN, as Linux), except a release;
//  - system sleep ends the lease (the GuC is reset; the resume sends the administrator's floor).
//
// Live range (sysman2, 13 Nov): debug.intelxe.slpc_min and slpc_max are the administrator's floor and ceiling, root
// only, as Linux's freq0/min_freq and max_freq (xe_gt_freq.c min_freq_store / max_freq_store, mode 0644): a write is
// sent to the GuC at once while SLPC runs (xeSlpcKnobWrite: MAX = the new ceiling, then the minimum the lease rules
// give with it), and kept for every later SLPC start (bring-up, GT reset, wake: slpcParams), as Linux keeps
// user_requested_min/max and re-sends them from pc_adjust_requested_freq. Where Linux answers a value outside RPn..RP0
// with EINVAL, the knob takes it clamped to RPn..RP0 (xeSlpcKnobValue; 0 = the default: 1200 MHz floor, RP0
// ceiling); a negative value is EINVAL. Linux sends min > max as asked ("min becomes a fixed request"); so does this.
// With the lease: a lease still never goes above the ceiling (lowering slpc_max below a running lease lowers the
// minimum the GuC has at once) and never lowers the floor (raising slpc_min above a lease's floor raises the minimum);
// only who may write differs: slpc_boost any user for 10 s, slpc_min/max root until changed.
#pragma once
#include <stdint.h>

static constexpr uint64_t kXeBoostLeaseNs = 10000000000ull;      // 10 s
static constexpr uint64_t kXeBoostRateWindowNs = 1000000000ull;  // 1 s
static constexpr uint32_t kXeBoostRateMax = 10;                  // writes per window
static constexpr uint64_t kXeBoostRetryNs = 100000000ull;        // a send the GT was not ready for: again in 100 ms
static constexpr uint32_t kXeSlpcDefaultMinMhz = 1200;           // BMG_MIN_FREQ (primary GT, every BMG), slpcParams' default

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

// dispboost (docs/design/DESIGN-display-boost.md): the kernel's own short lease on the same floor, taken when a frame's GPU
// work was not done at the vblank it was due (NMXAccel's present worker tells IntelXe, "intelxeDispBoost"), the
// counterpart of i915's intel_display_rps_boost_after_vblank -> intel_rps_boost -> slpc_boost_work (min = boost_freq,
// RP0 by default). Rules:
//  - a miss while a display lease runs renews it (until = miss + lease length): it lasts only while misses keep coming;
//  - with no lease, a miss counts towards a streak (misses less than one lease length apart); the lease starts at the
//    streak's misses-th miss (debug.intelxe.disp_boost_misses; 1 = every miss, as i915), so one late frame of an
//    otherwise idle desktop does not lift the clock;
//  - at most kXeDispGrantMax new leases per kXeDispGrantWindowNs (renewals are not counted: they send nothing);
//  - the floor is debug.intelxe.disp_boost_mhz (0 = the ceiling: i915's boost_freq is RP0), clamped to [RPn, the
//    ceiling] like slpc_boost's, so never above slpc_max; it only ever raises the minimum the GuC gets;
//  - nothing while switched off, while the clock is under manual control (freq_manual), with the power-saving SLPC
//    profile (i915 does no waitboost there either), or while the GT cannot take a new minimum; each counted;
//  - system sleep and switching it off end the lease at once.
static constexpr uint32_t kXeDispCeiling = 0xffffffffu;           // disp.mhz meaning "the ceiling" (clamped where used)
static constexpr uint32_t kXeDispGrantMax = 5;                     // new display leases per window
static constexpr uint64_t kXeDispGrantWindowNs = 1000000000ull;    // 1 s

// Why a miss may not boost now (the glue decides; the pure part counts).
enum XeDispGate { kXeDispGateOk, kXeDispGateOff, kXeDispGateManual, kXeDispGatePowerSaving, kXeDispGateNotReady };
enum XeDispVerdict { kXeDispGranted, kXeDispRenewed, kXeDispStreak, kXeDispRateLimited, kXeDispIgnored };

struct XeDispLease {
	uint32_t mhz;          // the floor asked while the lease runs (0 = no lease; kXeDispCeiling = the ceiling)
	uint64_t untilNs;      // when it ends unless a miss renews it
	uint64_t startNs;      // when it began (the time boosted is counted when it ends)
	uint32_t streak;       // misses towards a new lease, each less than a lease length after the one before
	uint64_t lastMissNs;
	uint64_t windowNs;     // the grant rate limit's current window
	uint32_t windowGrants;
	// counters (debug.intelxe.disp_boost_stats)
	uint64_t misses;       // misses taken (whatever came of them)
	uint64_t grants, renewals, streakMisses, rateLimited, off, manual, powerSaving, notReady;
	uint64_t expiries, sleepEnds, knobEnds;
	uint64_t boostedNs;    // time with a display lease, the running one not included
};

struct XeSlpcBoost {
	uint32_t askedMhz;     // the active lease's request as written (0 = no lease); clamped where it is used
	uint64_t untilNs;      // when it ends
	uint32_t sentMhz;      // the minimum the GuC was last given (slpcParams or the lease); 0 = unknown, nothing to restore
	uint32_t sentMaxMhz;   // the maximum the GuC was last given (slpcParams or a slpc_max write); 0 = unknown
	uint64_t windowNs;     // the rate limit's current window
	uint32_t windowWrites;
	// counters (debug.intelxe.stats "slpc boost")
	uint32_t grants, renewals, releases, expiries, sleepEnds, rateLimited, notReady, clamped, sends;
	uint32_t rangeWrites, maxSends;   // slpc_min/slpc_max writes, MAX sends they made
	XeDispLease disp;      // dispboost: the display lease (its own counters)
};

enum XeBoostVerdict { kXeBoostGranted, kXeBoostRenewed, kXeBoostReleased, kXeBoostNoLease, kXeBoostRateLimited, kXeBoostNotReady };

static inline bool xeBoostActive(const XeSlpcBoost &s, uint64_t now) { return s.askedMhz && now < s.untilNs; }

// The lease's floor now (0 = none), clamped to the current bounds (slpc_max may have changed since the write).
static inline uint32_t xeBoostFloor(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	return xeBoostActive(s, now) ? xeBoostClamp(s.askedMhz, b) : 0;
}

static inline bool xeDispActive(const XeDispLease &d, uint64_t now) { return d.mhz && now < d.untilNs; }

// The display lease's floor now (0 = none), clamped to the current bounds.
static inline uint32_t xeDispFloor(const XeDispLease &d, const XeSlpcBounds &b, uint64_t now) {
	return xeDispActive(d, now) ? xeBoostClamp(d.mhz, b) : 0;
}

// The minimum the GuC should have now: the administrator's floor, raised to the leases' (slpc_boost's, the display's).
static inline uint32_t xeBoostEffectiveMin(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	const uint32_t user = xeSlpcUserFloor(b), lease = xeBoostFloor(s, b, now), disp = xeDispFloor(s.disp, b, now);
	const uint32_t m = lease > user ? lease : user;
	return disp > m ? disp : m;
}

// ms left on the lease (0 = none).
static inline uint64_t xeBoostLeftMs(const XeSlpcBoost &s, uint64_t now) {
	return xeBoostActive(s, now) ? (s.untilNs - now + 999999) / 1000000 : 0;
}

// A write of mhz (0 = release) at now. ready: the GT can take a new minimum (up, not suspended, not in reset, SLPC running).
// A release (0) is taken before the rate limit and does not count against it: it can only lower the clock back to the
// administrator's floor, and a client that hit EBUSY must still be able to end its lease (night of 13 Nov: eleven writes
// in a second, then the release was refused and the lease ran its 10 s).
static inline XeBoostVerdict xeBoostWrite(XeSlpcBoost &s, uint64_t now, uint32_t mhz, const XeSlpcBounds &b, bool ready) {
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
	if (!s.windowNs || now - s.windowNs >= kXeBoostRateWindowNs || now < s.windowNs) {
		s.windowNs = now ? now : 1;
		s.windowWrites = 0;
	}
	if (s.windowWrites >= kXeBoostRateMax) {
		s.rateLimited++;
		return kXeBoostRateLimited;
	}
	s.windowWrites++;
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

// Ends the display lease at now (or at its own end, if that came first), adding its time to boostedNs. False: none ran.
static inline bool xeDispEnd(XeDispLease &d, uint64_t now) {
	if (!d.mhz) return false;
	const uint64_t end = now < d.untilNs ? now : d.untilNs;
	if (end > d.startNs) d.boostedNs += end - d.startNs;
	d.mhz = 0;
	d.untilNs = 0;
	d.streak = 0;
	return true;
}

// The time boosted so far, the running lease's part included.
static inline uint64_t xeDispBoostedNs(const XeDispLease &d, uint64_t now) {
	if (!d.mhz) return d.boostedNs;
	const uint64_t end = now < d.untilNs ? now : d.untilNs;
	return d.boostedNs + (end > d.startNs ? end - d.startNs : 0);
}

// The timer: a lease past its end is over. True when one ended now (slpc_boost's or the display's).
static inline bool xeBoostExpire(XeSlpcBoost &s, uint64_t now) {
	bool ended = false;
	if (s.disp.mhz && now >= s.disp.untilNs) {
		xeDispEnd(s.disp, now);
		s.disp.expiries++;
		ended = true;
	}
	if (!s.askedMhz || now < s.untilNs) return ended;
	s.askedMhz = 0;
	s.untilNs = 0;
	s.expiries++;
	return true;
}

// System sleep: the leases end, and the GuC forgets what it was given (the resume's slpcParams sends the administrator's
// floor and sets sentMhz again). now: for the display lease's time boosted (0: counted to its end).
static inline void xeBoostSleep(XeSlpcBoost &s, uint64_t now = 0) {
	if (s.askedMhz) s.sleepEnds++;
	s.askedMhz = 0;
	s.untilNs = 0;
	if (xeDispEnd(s.disp, now ? now : s.disp.untilNs)) s.disp.sleepEnds++;
	s.disp.streak = 0;
	s.sentMhz = 0;
	s.sentMaxMhz = 0;
}

// A missed vblank at now (dispboost). leaseNs: the lease length (debug.intelxe.disp_boost_ms); misses: the streak that
// starts a lease (>= 1); mhz: the floor to ask (0 = the ceiling). The glue then sends what is owed (xeBoostToSend).
static inline XeDispVerdict xeDispMiss(XeDispLease &d, uint64_t now, uint64_t leaseNs, uint32_t misses, uint32_t mhz,
                                       XeDispGate gate) {
	d.misses++;
	switch (gate) {
	case kXeDispGateOk: break;
	case kXeDispGateOff: d.off++; return kXeDispIgnored;
	case kXeDispGateManual: d.manual++; return kXeDispIgnored;
	case kXeDispGatePowerSaving: d.powerSaving++; return kXeDispIgnored;
	case kXeDispGateNotReady: d.notReady++; return kXeDispIgnored;
	}
	if (!misses) misses = 1;
	if (xeDispActive(d, now)) {   // still late with the floor raised: keep it while the misses go on
		if (now + leaseNs > d.untilNs) d.untilNs = now + leaseNs;   // never shortens a longer (idle-exit) lease
		d.lastMissNs = now;
		d.renewals++;
		return kXeDispRenewed;
	}
	if (d.mhz && xeDispEnd(d, now)) d.expiries++;   // ended, the timer not run yet: its time counted to its end
	if (d.streak && d.lastMissNs && now >= d.lastMissNs && now - d.lastMissNs < leaseNs) d.streak++;
	else d.streak = 1;
	d.lastMissNs = now;
	if (d.streak < misses) {
		d.streakMisses++;
		return kXeDispStreak;
	}
	if (!d.windowNs || now < d.windowNs || now - d.windowNs >= kXeDispGrantWindowNs) {
		d.windowNs = now ? now : 1;
		d.windowGrants = 0;
	}
	if (d.windowGrants >= kXeDispGrantMax) {
		d.rateLimited++;
		return kXeDispRateLimited;   // the streak stays: the next miss past the window starts the lease
	}
	d.windowGrants++;
	d.mhz = mhz ? mhz : kXeDispCeiling;
	d.startNs = now;
	d.untilNs = now + leaseNs;
	d.streak = 0;
	d.grants++;
	return kXeDispGranted;
}

// debug.intelxe.disp_boost written 0: the lease ends now. True when one ran.
static inline bool xeDispSwitchOff(XeDispLease &d, uint64_t now) {
	d.streak = 0;
	if (!xeDispEnd(d, now)) return false;
	d.knobEnds++;
	return true;
}

// What to send now: the effective minimum when it differs from what the GuC has (0: nothing). Nothing while sentMhz is
// unknown (before SLPC started, during sleep): slpcParams sends the whole set then.
static inline uint32_t xeBoostToSend(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	if (!s.sentMhz) return 0;
	const uint32_t want = xeBoostEffectiveMin(s, b, now);
	return want != s.sentMhz ? want : 0;
}

// When the timer should run next (0 = not needed): the end of the lease that ends first, or soon when a send is still
// owed.
static inline uint64_t xeBoostNextNs(const XeSlpcBoost &s, const XeSlpcBounds &b, uint64_t now) {
	if (xeBoostToSend(s, b, now)) return now + kXeBoostRetryNs;
	uint64_t next = s.askedMhz ? s.untilNs : 0;
	if (s.disp.mhz && (!next || s.disp.untilNs < next)) next = s.disp.untilNs;
	return next;
}

// A slpc_min / slpc_max write: false (EINVAL) for a negative value; else what the knob keeps: 0 (the default) as is, a
// value clamped to [RPn, RP0] once the GT's limits are known (rp0 != 0; before SLPC's first start slpcParams clamps).
static inline bool xeSlpcKnobValue(int mhz, uint32_t rpn, uint32_t rp0, int *out) {
	if (mhz < 0) return false;
	uint32_t v = (uint32_t)mhz;
	if (v && rp0) {
		if (v > rp0) v = rp0;
		if (v < rpn) v = rpn;
	}
	*out = (int)v;
	return true;
}

// The ceiling to send after a knob write (0: the GuC has it already, or what it has is not known).
static inline uint32_t xeSlpcMaxToSend(const XeSlpcBoost &s, const XeSlpcBounds &b) {
	if (!s.sentMaxMhz) return 0;
	const uint32_t want = xeSlpcCeiling(b);
	return want != s.sentMaxMhz ? want : 0;
}
