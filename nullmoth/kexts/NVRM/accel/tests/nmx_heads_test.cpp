// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// nmx_heads_test: offline check of nmx_heads.h (which IntelXeFB heads NMXAccel arms, and when), no GPU needed.
// c++ -std=c++17 -Wall -Wextra -I.. -o nmx_heads_test nmx_heads_test.cpp && ./nmx_heads_test
#include <stdio.h>
#include "nmx_heads.h"

static int fails, checks;
#define EQ(a, b) do { checks++; if ((a) != (b)) { fails++; printf("FAIL %s:%d: %s = %s, want %s\n", __FILE__, __LINE__, #a, \
	nmxHeadVerdictName(a), nmxHeadVerdictName(b)); } } while (0)
#define TRUE(c) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

int main()
{
	const NmxHeadIn none = {}, pub = { true, false, false, true, false }, pubNoScan = { true, false, false, false, false };
	const NmxHeadIn piped = { true, true, true, true, false }, regNoPipe = { true, true, false, true, false };
	const NmxHeadIn gone = { false, false, false, false, true };

	// one monitor, knob off, armed at boot: head 0 piped, nothing else; the knob changes nothing about it
	NmxAccelIn a = { false, true, false, 0x1 };
	EQ(nmxHeadVerdict(0, piped, a), kNmxHeadPiped);
	EQ(nmxHeadVerdict(1, none, a), kNmxHeadAbsent);
	a.knob = true;
	EQ(nmxHeadVerdict(0, piped, a), kNmxHeadPiped);
	EQ(nmxHeadVerdict(1, none, a), kNmxHeadAbsent);

	// head 1 published after the arm: knob off = today's manual step; knob on = registered now (or once it scans out)
	a = { false, true, false, 0x3 };
	EQ(nmxHeadVerdict(1, pub, a), kNmxHeadManual);
	a.knob = true;
	EQ(nmxHeadVerdict(1, pub, a), kNmxHeadLateArm);
	EQ(nmxHeadVerdict(1, pubNoScan, a), kNmxHeadNotReady);
	EQ(nmxHeadVerdict(1, piped, a), kNmxHeadPiped);
	EQ(nmxHeadVerdict(1, regNoPipe, a), kNmxHeadRegisteredNoPipe);

	// the boot arm is pending (waiting for IntelXeFB, or arming): it takes every published head, knob or not
	a = { true, false, true, 0x3 };
	EQ(nmxHeadVerdict(0, pub, a), kNmxHeadBootArm);
	EQ(nmxHeadVerdict(1, pub, a), kNmxHeadBootArm);
	a.armed = true;   // levels 1/3 written, the arm not finished yet: still the boot arm's
	EQ(nmxHeadVerdict(1, pub, a), kNmxHeadBootArm);

	// not armed at all (held by IntelXeFB's guard, -intelxe_noaccel, arm failed): nothing registers a head on its own
	a = { true, false, false, 0x3 };
	EQ(nmxHeadVerdict(1, pub, a), kNmxHeadNotArmed);
	EQ(nmxHeadVerdict(0, pub, a), kNmxHeadNotArmed);

	// head 0 is never the late arm's, even with the knob (a manual or boot registration only)
	a = { true, true, false, 0x1 };
	EQ(nmxHeadVerdict(0, pub, a), kNmxHeadManual);

	// a hole: head 2 without head 1 is never reached by the registration walk
	a = { true, true, false, 0x5 };
	EQ(nmxHeadVerdict(2, pub, a), kNmxHeadBlocked);
	a.publishedMask = 0x7;
	EQ(nmxHeadVerdict(2, pub, a), kNmxHeadLateArm);

	// a terminated head frees its slot; a new framebuffer there is a new publish
	a = { true, true, false, 0x1 };
	EQ(nmxHeadVerdict(1, gone, a), kNmxHeadTerminated);
	const NmxHeadIn back = { true, false, false, true, true };
	a.publishedMask = 0x3;
	EQ(nmxHeadVerdict(1, back, a), kNmxHeadLateArm);

	// capabilities: never skipped on head 0, nor with the knob off; head 1 without a capability is skipped with the knob
	TRUE(!nmxHeadLacks(true, 0, 0, kNmxCapFlip));
	TRUE(!nmxHeadLacks(false, 1, 0, kNmxCapFlip));
	TRUE(nmxHeadLacks(true, 1, kNmxCapPipeFB | kNmxCapTiming, kNmxCapFlip));
	TRUE(nmxHeadLacks(true, 1, kNmxCapPipeFB | kNmxCapTiming, kNmxCapGamma));
	TRUE(!nmxHeadLacks(true, 1, kNmxCapFlip | kNmxCapGamma, kNmxCapGamma));

	for (int v = 0; v < kNmxHeadVerdictCount; v++) TRUE(nmxHeadVerdictName(v)[0] != '?');
	printf("%s: %d checks, %d failed\n", fails ? "FAIL" : "ok", checks, fails);
	return fails ? 1 : 0;
}
