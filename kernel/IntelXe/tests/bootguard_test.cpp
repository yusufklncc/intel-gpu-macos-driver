// SPDX-License-Identifier: MIT
// bootguard_test: offline check of the boot guard's rules (uapi/intelxe_bootguard.h), which IntelXe.kext (bootGuardDecide,
// the firmware framebuffer on a held boot) and IntelXeFB.kext (guardBegin) share.
//   1. parsing intelxe-guard and the "ok means absent" rule
//   2. decide() against the rules IntelXeFB's guardBegin had before the shared header (written out below as
//      oldGuardBegin), over every combination of the guard, intelxe-accel-off and the two boot-args; and against
//      NMXAccel's nmxGuardHolds (nvrm-accel.cpp), which reads the same values
//   3. what a held boot shows (intelxe-held-display, boot-arg intelxe_helddisplay)
//   4. the state machine over several boots: crashes to safe and held, a held boot on the firmware framebuffer and what
//      it writes, clearing from that boot, a power cut that loses the kernel's writes, the old one-variable clear
// c++ -std=c++17 -Wall -Wextra -I.. -o bootguard_test bootguard_test.cpp && ./bootguard_test
#include <stdio.h>
#include <string.h>
#include <string>
#include "uapi/intelxe_bootguard.h"

using namespace intelxeguard;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// IntelXeFB guardBegin before intelxe_bootguard.h (0.1.4), with one deliberate change: an empty intelxe-accel-off counts
// as absent, as NMXAccel always read it (guardBegin read "" as set, nmxGuardHolds as clear: the two kexts disagreed).
struct Old { bool allowed, safePublished, wroteOff; unsigned accelFails, fbFails; int heldBy; };
static Old oldGuardBegin(const char *prev, const char *offIn, bool noaccel, bool safeMode)
{
	Old o = {};
	const bool had = prev && strcmp(prev, "ok") && prev[0];
	bool accelOff = offIn && offIn[0] && strcmp(offIn, "ok");
	bool byCount = false, fAccelSafe = false;
	if (had && !strncmp(prev, "accel", 5)) {
		unsigned n = 0;
		for (const char *c = prev + 5; *c >= '0' && *c <= '9' && n < 1000; c++) n = n * 10 + (unsigned)(*c - '0');
		o.accelFails = n ? n : 1;
		if (o.accelFails >= 4) { o.wroteOff = true; byCount = !accelOff; accelOff = true; }
		else if (o.accelFails >= 2) fAccelSafe = true;
	}
	if (had && prev[0] == 'f' && prev[1] == 'b') {
		unsigned f = 0;
		for (const char *c = prev + 2; *c >= '0' && *c <= '9' && f < 1000; c++) f = f * 10 + (unsigned)(*c - '0');
		o.fbFails = f ? f : 1;
	}
	o.heldBy = noaccel ? kHeldByBootArg : safeMode ? kHeldBySafeMode : accelOff ? (byCount ? kHeldByCount : kHeldByAccelOff) : kNotHeld;
	o.allowed = o.heldBy == kNotHeld;
	o.safePublished = o.allowed && fAccelSafe;
	return o;
}

// nvrm-accel.cpp nmxGuardHolds: what NMXAccel reads at its start
static bool nmxGuardHolds(const char *off, const char *guard)
{
	if (off && off[0] && strcmp(off, "ok")) return true;
	if (guard && !strncmp(guard, "accel", 5)) {
		unsigned n = 0;
		for (const char *c = guard + 5; *c >= '0' && *c <= '9' && n < 1000; c++) n = n * 10 + (unsigned)(*c - '0');
		if (n >= 4) return true;
	}
	return false;
}

static void testParse()
{
	CHECK(!isSet(nullptr) && !isSet("") && !isSet("ok") && isSet("o") && isSet("okay") && isSet("test") && isSet("accel1"), "isSet");
	struct { const char *v; char kind; unsigned n; } t[] = {
		{"", 0, 0}, {"ok", 0, 0}, {"accel", 'a', 1}, {"accel1", 'a', 1}, {"accel3", 'a', 3}, {"accel12", 'a', 12},
		{"accel0", 'a', 1}, {"fb", 'f', 1}, {"fb2", 'f', 2}, {"junk", '?', 0}, {"f", '?', 0},
	};
	for (const auto &c : t) {
		const Prev p = parsePrev(c.v);
		CHECK(p.kind == c.kind && p.n == c.n, "parsePrev(\"%s\") = %c%u, want %c%u", c.v, p.kind ? p.kind : '0', p.n, c.kind ? c.kind : '0', c.n);
	}
	CHECK(digitsAfter("99999") >= 1000 && digitsAfter("99999") < 100000, "digits are bounded");
}

static void testAgainstOld()
{
	const char *guards[] = {"", "ok", "accel", "accel1", "accel2", "accel3", "accel4", "accel5", "accel6", "accel10", "fb", "fb1", "fb3", "junk"};
	const char *offs[] = {"", "ok", "test", "4 accelerated boots in a row ended inside their first minute"};
	int n = 0;
	for (const char *g : guards)
		for (const char *o : offs)
			for (int na = 0; na < 2; na++)
				for (int sm = 0; sm < 2; sm++) {
					const Decision d = decide(g, o, na, sm);
					const Old r = oldGuardBegin(g, o, na, sm);
					n++;
					CHECK(d.accelAllowed == r.allowed && (d.accelAllowed && d.safe) == r.safePublished && d.writeAccelOff == r.wroteOff
					      && d.accelFails == r.accelFails && d.fbFails == r.fbFails && d.heldBy == r.heldBy,
					      "guard \"%s\" off \"%s\" noaccel %d -x %d: allowed %d/%d safe %d/%d write %d/%d accel %u/%u fb %u/%u heldBy %d/%d",
					      g, o, na, sm, d.accelAllowed, r.allowed, d.accelAllowed && d.safe, r.safePublished, d.writeAccelOff, r.wroteOff,
					      d.accelFails, r.accelFails, d.fbFails, r.fbFails, d.heldBy, r.heldBy);
					// the state follows from the rest
					const int want = !d.accelAllowed ? kHeld : d.safe ? kSafe : d.accelFails ? kRetry : kNormal;
					CHECK(d.state == want, "state %d, want %d", d.state, want);
					// NMXAccel holds exactly when the guard does by its NVRAM values (boot-args are its own checks)
					if (!na && !sm) CHECK(nmxGuardHolds(o, g) == !d.accelAllowed, "NMXAccel and the guard disagree on \"%s\" / \"%s\"", g, o);
				}
	CHECK(n == 14 * 4 * 4, "%d combinations", n);
	// the named cases
	CHECK(decide("accel1", "", false, false).state == kRetry, "one crash: try again");
	CHECK(decide("accel2", "", false, false).state == kSafe && decide("accel3", "ok", false, false).state == kSafe, "two or three: safe settings");
	CHECK(decide("accel4", "", false, false).state == kHeld && decide("accel4", "", false, false).heldBy == kHeldByCount, "four: held by the count");
	CHECK(decide("ok", "anything", false, false).heldBy == kHeldByAccelOff, "intelxe-accel-off holds");
	CHECK(decide("accel2", "", true, false).heldBy == kHeldByBootArg && !decide("accel2", "", true, false).safe, "a boot-arg hold has no safe settings");
}

static void testHeldDisplay()
{
	CHECK(heldDisplay(nullptr, -1) == kDisplayFirmware && heldDisplay("", -1) == kDisplayFirmware && heldDisplay("ok", -1) == kDisplayFirmware,
	      "default: the firmware framebuffer");
	CHECK(heldDisplay("fw", -1) == kDisplayFirmware && heldDisplay("firmware", -1) == kDisplayFirmware && heldDisplay("x", -1) == kDisplayFirmware,
	      "fw, firmware, anything unknown: firmware");
	CHECK(heldDisplay("fb", -1) == kDisplayFramebuffer && heldDisplay("framebuffer", -1) == kDisplayFramebuffer && heldDisplay("1", -1) == kDisplayFramebuffer,
	      "fb, framebuffer, 1: IntelXeFB");
	CHECK(heldDisplay("fb", 0) == kDisplayFirmware && heldDisplay("", 1) == kDisplayFramebuffer && heldDisplay("fw", 1) == kDisplayFramebuffer,
	      "the boot-arg wins");
	CHECK(heldDisplay("", 7) == kDisplayFirmware, "a boot-arg value other than 0/1 is ignored");
}

// ---- several boots ------------------------------------------------------------------------------------------------
// flash: what survives a power cut; mem: the running boot's NVRAM (kernel writes land here, reach flash at a clean
// shutdown); userspace nvram(8) writes both.
struct Nvram { std::string guard, off, display; };
enum Outcome { kHealthy, kPowerCut };   // a clean shutdown at the end, or the power gone (also how a panic is modelled)
struct Boot { int state, heldBy; bool firmware, safe; };

static Boot boot(Nvram &flash, Outcome how, bool noaccel = false, bool safeMode = false, bool clearFromDesktop = false,
                 bool clearOffOnly = false)
{
	Nvram mem = flash;
	const Decision d = decide(mem.guard.c_str(), mem.off.c_str(), noaccel, safeMode);
	const bool firmware = d.state == kHeld && heldDisplay(mem.display.c_str(), -1) == kDisplayFirmware;
	if (firmware) {   // IntelXe::bootGuardDecide; IntelXeFB does not start
		if (d.writeAccelOff) mem.off = "4 accelerated boots in a row ended inside their first minute";
		if (const char *g = guardAfterFirmwareHold(mem.guard.c_str(), d)) mem.guard = g;
	} else {          // IntelXeFB guardBegin, then the accelerated stage when allowed
		if (d.writeAccelOff) mem.off = "4 accelerated boots in a row ended inside their first minute";
		mem.guard = "fb" + std::to_string(d.fbFails + 1);
		if (d.accelAllowed) mem.guard = "accel" + std::to_string(d.accelFails + 1);
		if (how == kHealthy) mem.guard = "ok";   // healthy minute; guard-clear.sh writes it to flash at once
		if (how == kHealthy) flash.guard = "ok";
	}
	// the IntelXe Manager's "Clear the boot guard" from this boot's desktop (userspace: memory and flash)
	if (clearFromDesktop) { mem.off = "ok"; flash.off = "ok"; if (!clearOffOnly) { mem.guard = "ok"; flash.guard = "ok"; } }
	if (how == kHealthy) flash = mem;   // a clean shutdown flushes the kernel's writes
	return {d.state, d.heldBy, firmware, d.safe};
}

static void testBoots()
{
	Nvram nv;
	// a crash in the accelerated minute: the boot's other writes are lost, but the accel<n> IntelXeFB wrote at the arm is
	// what the next boot reads (the count went up over panics and held power buttons in the 5-6 Nov tests). Model: the
	// arm's value reaches flash, nothing else.
	auto crash = [&](bool noaccel = false) {
		const Decision d = decide(nv.guard.c_str(), nv.off.c_str(), noaccel, false);
		const Boot b = boot(nv, kPowerCut, noaccel);
		if (d.accelAllowed) nv.guard = "accel" + std::to_string(d.accelFails + 1);
		return b;
	};
	Boot b = crash();
	CHECK(b.state == kNormal && nv.guard == "accel1", "boot 1 normal, crashed: %s", nv.guard.c_str());
	b = crash();
	CHECK(b.state == kRetry && nv.guard == "accel2", "boot 2 retries: %s", nv.guard.c_str());
	b = crash();
	CHECK(b.state == kSafe && b.safe && nv.guard == "accel3", "boot 3 safe settings: %s", nv.guard.c_str());
	b = crash();
	CHECK(b.state == kSafe && nv.guard == "accel4", "boot 4 safe settings: %s", nv.guard.c_str());
	// boot 5: held by the count, the firmware framebuffer shows the desktop; a clean restart without clearing
	b = boot(nv, kHealthy);
	CHECK(b.state == kHeld && b.heldBy == kHeldByCount && b.firmware, "boot 5 held on the firmware framebuffer");
	CHECK(nv.guard == "ok" && isSet(nv.off.c_str()), "boot 5 recorded the hold in intelxe-accel-off and cleared the count: %s / %s",
	      nv.guard.c_str(), nv.off.c_str());
	b = boot(nv, kHealthy);
	CHECK(b.state == kHeld && b.heldBy == kHeldByAccelOff && b.firmware, "boot 6 still held (intelxe-accel-off)");
	// boot 7: the user clears from the held desktop (both variables), restarts
	b = boot(nv, kHealthy, false, false, true);
	CHECK(b.state == kHeld, "boot 7 held, cleared from its desktop");
	b = boot(nv, kHealthy);
	CHECK(b.state == kNormal && nv.guard == "ok" && !isSet(nv.off.c_str()), "boot 8 normal and healthy");

	// a held boot whose kernel writes are lost (power cut): accel4 is still there, so the next boot is held the same way
	Nvram p; p.guard = "accel4";
	b = boot(p, kPowerCut);
	CHECK(b.firmware && p.guard == "accel4" && !isSet(p.off.c_str()), "power cut in a held boot: nothing changed in flash");
	b = boot(p, kHealthy);
	CHECK(b.state == kHeld && b.heldBy == kHeldByCount && b.firmware, "and the next boot is held again");
	// the old instruction (intelxe-accel-off=ok only) from a held boot after its writes were flushed: enough
	b = boot(p, kHealthy, false, false, true, true);
	CHECK(b.state == kHeld, "held, intelxe-accel-off=ok from its desktop");
	b = boot(p, kHealthy);
	CHECK(b.state == kNormal, "intelxe-accel-off=ok alone clears it once the held boot's own writes reached flash");
	// ... but not when those writes were lost: the Manager therefore clears both
	Nvram q; q.guard = "accel4";
	b = boot(q, kPowerCut, false, false, true, true);
	b = boot(q, kHealthy);
	CHECK(b.state == kHeld, "intelxe-accel-off=ok alone after a power cut in the held boot: held again (clear both)");
	Nvram r; r.guard = "accel4";
	b = boot(r, kPowerCut, false, false, true, false);
	b = boot(r, kHealthy);
	CHECK(b.state == kNormal, "both cleared from the held desktop survive a power cut");

	// -intelxe_noaccel and safe mode: held on the firmware framebuffer, the count below the hold left for later
	Nvram s; s.guard = "accel2";
	b = boot(s, kHealthy, true);
	CHECK(b.state == kHeld && b.heldBy == kHeldByBootArg && b.firmware && s.guard == "accel2", "boot-arg hold keeps accel2: %s", s.guard.c_str());
	b = boot(s, kHealthy, false, true);
	CHECK(b.heldBy == kHeldBySafeMode && b.firmware && s.guard == "accel2", "safe mode the same");
	b = boot(s, kHealthy);
	CHECK(b.state == kSafe && s.guard == "ok", "without them: the safe settings, then healthy");

	// intelxe-held-display=fb: IntelXeFB unaccelerated as before (it writes fb<n>, the hold stays in intelxe-accel-off)
	Nvram f; f.guard = "accel4"; f.display = "fb";
	b = boot(f, kHealthy);
	CHECK(b.state == kHeld && !b.firmware && isSet(f.off.c_str()) && f.guard == "ok", "fb: IntelXeFB held as before (%s)", f.guard.c_str());

	CHECK(guardAfterFirmwareHold("accel4", decide("accel4", "", false, false)) && !strcmp(guardAfterFirmwareHold("accel4", decide("accel4", "", false, false)), "ok"),
	      "a count that made the hold is cleared");
	CHECK(!guardAfterFirmwareHold("accel2", decide("accel2", "", true, false)) && !guardAfterFirmwareHold("fb2", decide("fb2", "x", false, false))
	      && !guardAfterFirmwareHold("accel4", decide("accel4", "", true, false).state == kHeld ? Decision{kNormal, 0, 4, 0, true, false, true} : Decision{}),
	      "anything else is left alone (a boot that is not held, a count below the hold, fb<n>)");
	CHECK(!strcmp(stateName(kHeld), "held") && !strcmp(stateName(kSafe), "safe") && !strcmp(heldByName(kHeldByCount), "count"), "names");
}

int main()
{
	testParse();
	testAgainstOld();
	testHeldDisplay();
	testBoots();
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
	return fails ? 1 : 0;
}
