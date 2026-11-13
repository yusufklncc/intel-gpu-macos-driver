// SPDX-License-Identifier: MIT
/* The boot guard's rules, shared by IntelXe.kext (IntelXe::bootGuardDecide), IntelXeFB.kext (guardBegin) and the host test
 * kernel/IntelXe/tests/bootguard_test.cpp. No kernel or libc calls: plain parsing of the NVRAM values.
 *
 * NVRAM (all "ok" or empty = absent; nvram -d does not delete on macOS 26, so "ok" is how a value is cleared):
 *   intelxe-guard        what the previous boot reached. "accel<n>" (plain "accel" = 1): n accelerated boots in a row
 *                        ended inside their first minute. "fb<n>": n boots in a row ended before the unaccelerated
 *                        framebuffer desktop ran for a minute (logged only). Written by IntelXeFB.
 *   intelxe-accel-off    set (any value but "ok"): acceleration is held on every boot until it is cleared. Written
 *                        at the first boot that sees accel<n> with n >= kGuardAccelHold; the value is the reason.
 *   intelxe-held-display what a held boot shows: absent, "ok", "fw" or "firmware" = the firmware framebuffer (Apple's
 *                        IONDRVFramebuffer on the GOP's surface, as the macOS installer runs on this card); "fb" or
 *                        "framebuffer" = IntelXeFB unaccelerated (the behaviour before this file: on macOS 26 the
 *                        Apple logo stays, WindowServer does not draw on IntelXeFB without the accelerator). The
 *                        boot-arg intelxe_helddisplay=0 (firmware) / =1 (IntelXeFB) wins over it.
 *
 * A boot's state:
 *   normal  nothing recorded;
 *   retry   one accelerated boot ended inside its first minute (maybe a power cut): this one tries again as usual;
 *   safe    kGuardAccelSafe or more in a row: NMXAccel arms with its most conservative settings (IntelXeFBAccelSafe);
 *   held    kGuardAccelHold or more in a row, intelxe-accel-off set, -intelxe_noaccel or safe mode (-x): no acceleration.
 * NMXAccel (nullmoth/kexts/NVRM/accel/nvrm-accel.cpp nmxGuardHolds) reads the same two values with the same rule. */
#ifndef INTELXE_BOOTGUARD_H
#define INTELXE_BOOTGUARD_H

namespace intelxeguard {

static constexpr unsigned kGuardAccelSafe = 2, kGuardAccelHold = 4;

enum State : int { kNormal = 0, kRetry = 1, kSafe = 2, kHeld = 3 };
enum HeldBy : int { kNotHeld = 0, kHeldByBootArg = 1, kHeldBySafeMode = 2, kHeldByAccelOff = 3, kHeldByCount = 4 };
enum HeldDisplay : int { kDisplayFirmware = 0, kDisplayFramebuffer = 1 };

inline bool streq(const char *a, const char *b) {
	if (!a || !b) return false;
	while (*a && *a == *b) { a++; b++; }
	return *a == *b;
}
inline bool startsWith(const char *s, const char *prefix) {
	if (!s || !prefix) return false;
	while (*prefix && *s == *prefix) { s++; prefix++; }
	return !*prefix;
}
// an NVRAM value that is there: not null, not empty, not "ok"
inline bool isSet(const char *v) { return v && v[0] && !streq(v, "ok"); }

// the decimal digits after `s` (up to 999); 0 when there are none
inline unsigned digitsAfter(const char *s) {
	unsigned n = 0;
	for (; s && *s >= '0' && *s <= '9' && n < 1000; s++) n = n * 10 + (unsigned)(*s - '0');
	return n;
}

// intelxe-guard as the previous boot left it
struct Prev { char kind; unsigned n; };   // kind 0: nothing, 'a': accel<n>, 'f': fb<n>, '?': something else
inline Prev parsePrev(const char *v) {
	Prev p = {0, 0};
	if (!isSet(v)) return p;
	if (startsWith(v, "accel")) { p.kind = 'a'; p.n = digitsAfter(v + 5); if (!p.n) p.n = 1; }
	else if (v[0] == 'f' && v[1] == 'b') { p.kind = 'f'; p.n = digitsAfter(v + 2); if (!p.n) p.n = 1; }
	else p.kind = '?';
	return p;
}

struct Decision {
	int state;              // State
	int heldBy;             // HeldBy
	unsigned accelFails;    // accelerated boots in a row before this one that ended inside their first minute
	unsigned fbFails;       // boots in a row that ended before the framebuffer desktop ran for a minute
	bool writeAccelOff;     // this boot records the hold in intelxe-accel-off (accelFails >= kGuardAccelHold)
	bool safe;              // NMXAccel arms with the safe settings (only when acceleration is allowed)
	bool accelAllowed;
};

// IntelXeFB's guardBegin, before this file: the same order of checks. The boot-arg wins over safe mode, safe mode over
// intelxe-accel-off; the hold by count is written to intelxe-accel-off even when a boot-arg holds as well.
inline Decision decide(const char *guard, const char *accelOff, bool noaccelBootArg, bool safeModeBoot) {
	Decision d = {kNormal, kNotHeld, 0, 0, false, false, true};
	const Prev p = parsePrev(guard);
	bool off = isSet(accelOff);
	bool byCount = false;
	if (p.kind == 'a') {
		d.accelFails = p.n;
		if (p.n >= kGuardAccelHold) { d.writeAccelOff = true; byCount = !off; off = true; }
		else if (p.n >= kGuardAccelSafe) d.safe = true;
	} else if (p.kind == 'f') d.fbFails = p.n;
	if (noaccelBootArg) d.heldBy = kHeldByBootArg;
	else if (safeModeBoot) d.heldBy = kHeldBySafeMode;
	else if (off) d.heldBy = byCount ? kHeldByCount : kHeldByAccelOff;
	d.accelAllowed = d.heldBy == kNotHeld;
	if (!d.accelAllowed) { d.state = kHeld; d.safe = false; }
	else if (d.safe) d.state = kSafe;
	else if (d.accelFails) d.state = kRetry;
	return d;
}

// what a held boot shows: the boot-arg intelxe_helddisplay (-1 = absent) wins over NVRAM intelxe-held-display
inline int heldDisplay(const char *nvram, int bootArg) {
	if (bootArg == 0) return kDisplayFirmware;
	if (bootArg == 1) return kDisplayFramebuffer;
	if (streq(nvram, "fb") || streq(nvram, "framebuffer") || streq(nvram, "1")) return kDisplayFramebuffer;
	return kDisplayFirmware;
}

// What intelxe-guard becomes on a held boot that shows the firmware framebuffer, where IntelXeFB does not start (it would
// have replaced the value with fb<n>). nullptr: left as it is. A count that made the hold is cleared ("ok"): the hold is
// in intelxe-accel-off from now on, so clearing intelxe-accel-off alone ends it, as with IntelXeFB. A count below the
// hold (a boot held by -intelxe_noaccel or safe mode) stays for the next boot without them, and so does fb<n>.
inline const char *guardAfterFirmwareHold(const char *guard, const Decision &d) {
	if (d.state != kHeld) return nullptr;
	const Prev p = parsePrev(guard);
	return p.kind == 'a' && p.n >= kGuardAccelHold ? "ok" : nullptr;
}

inline const char *stateName(int s) {
	return s == kRetry ? "retry" : s == kSafe ? "safe" : s == kHeld ? "held" : "normal";
}
inline const char *heldByName(int h) {
	return h == kHeldByBootArg ? "boot-arg" : h == kHeldBySafeMode ? "safe-mode" : h == kHeldByAccelOff ? "accel-off"
	     : h == kHeldByCount ? "count" : "none";
}

}   // namespace intelxeguard
#endif
