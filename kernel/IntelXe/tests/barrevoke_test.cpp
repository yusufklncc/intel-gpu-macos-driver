// SPDX-License-Identifier: MIT
// barrevoke_test: offline check of the BAR-window revocation policy and state machine (xe_barrevoke.h,
// debug.intelxe.bar_revoke; docs/design/DESIGN-rebar-window.md section 8).
//   1. xeRevokeCheck: every reason a window BO is not taken, in its order, and the hot window (forced path: none)
//   2. xeRevokePick / xeRevokeCutoff: least recently touched first, ties larger first then by id; just enough to cover
//      the need; the cut-off of a pass over several clients takes the same BOs as one global pick
//   3. xeRevokeNext: every path through the state machine, enumerated, keeps the safety rules:
//      - the old VRAM is freed only after the verification, and never while the GPU is sent back to it;
//      - the CPU mapping never stays blocked: every path that blocked it unblocks it (Unblock or the redirect);
//      - the GPU PTEs are back on the VRAM in every final state but Done;
//      - the new pages are freed only where nothing can name them, never after a redirect that touched the mapping;
//      - a step that reported success without its effect latches the feature off;
//      - a verification that cannot run (no memory for its mapping) frees neither memory and latches off;
//      - barwin2: the descriptor whose pager the user's VM object faults through (the new pages' physical descriptor
//        after a transpose, the old one before or after a failed one) is never released while the BO lives;
//   4. the release rules alone (xeRevokeReleasable / xeRevokePagerOwners), the barwin release reproduced as a failure
//      - every (state, event) pair the caller can produce is valid, and no other is
// c++ -std=c++17 -Wall -Wextra -I.. -o barrevoke_test barrevoke_test.cpp && ./barrevoke_test
#include <stdio.h>
#include <string.h>
#include "xe_barrevoke.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static XeRevokeFacts good() {
	XeRevokeFacts f = {};
	f.size = 2ull << 20;
	f.touchNs = 1000000000ull;
	f.inWindow = f.mapped = f.contig = true;
	f.liveMaps = 1;
	return f;
}

// The path walker: applies events from s and checks the rules on the way. Returns the number of final states reached.
struct Walk {
	XeRevokeState s;
	bool blocked, gpuSys, vramFreed, vramKept, sysFreed, sysKept, latched, redirected, unblocked;
	uint16_t acts;
	char path[160];
};
static int finals;
static void walk(Walk w) {
	if (xeRevokeFinal(w.s)) {
		finals++;
		CHECK(!w.blocked, "%s: final with the CPU blocked", w.path);
		const bool onSys = w.s == kRsDone || w.s == kRsUndecided;
		CHECK(onSys ? w.gpuSys : !w.gpuSys, "%s: GPU on %s in final state %d", w.path, w.gpuSys ? "sys" : "VRAM", w.s);
		CHECK(w.vramFreed == (w.s == kRsDone), "%s: VRAM %s in state %d", w.path, w.vramFreed ? "freed" : "kept", w.s);
		CHECK(w.s != kRsUndecided || (w.vramKept && w.latched && !w.sysFreed), "%s: undecided must keep both", w.path);
		CHECK(!(w.sysFreed && w.sysKept), "%s: pages freed and kept", w.path);
		CHECK(!(w.sysFreed && w.redirected), "%s: pages freed after a redirect", w.path);
		CHECK(w.s != kRsDone || (!w.sysFreed && !w.sysKept), "%s: Done must keep the pages as the BO's", w.path);
		CHECK(w.s != kRsBroken || (w.latched && w.sysKept), "%s: Broken must latch off and keep the pages", w.path);
		CHECK(w.s != kRsRolledBack || w.sysFreed || w.sysKept, "%s: rolled back without deciding about the pages", w.path);
		// barwin2: the descriptor whose pager the user's object faults through is never released while the BO lives
		// (B580 14 Nov: released after the check, every later fault on a revoked BO was a SIGBUS)
		const uint8_t owners = xeRevokePagerOwners(w.s, w.acts), rel = xeRevokeReleasable(w.acts);
		CHECK(!(owners & rel), "%s: pager owner(s) %#x released (%#x)", w.path, owners, rel);
		CHECK(!(rel & kRdOld) || !(owners & kRdOld), "%s: old descriptor released while it may own the pager", w.path);
		CHECK((rel & kRdKmap) != 0, "%s: the kernel mapping must go", w.path);
		CHECK(!(w.acts & kRaAdoptDesc) || !(rel & kRdBridge), "%s: the map's bridge released", w.path);
		CHECK((w.acts & kRaAdoptDesc) || (rel & (kRdBridge | kRdPhys)) == (kRdBridge | kRdPhys),
			"%s: no redirect reached the map, but descriptors are kept", w.path);
		return;
	}
	for (int e = kReBlockOk; e <= kReVerifyUnknown; e++) {
		const XeRevokeStep st = xeRevokeNext(w.s, (XeRevokeEvent)e);
		if (!st.valid) {
			CHECK(st.next == w.s && !st.actions, "invalid step changes state or acts");
			continue;
		}
		Walk n = w;
		snprintf(n.path + strlen(n.path), sizeof(n.path) - strlen(n.path), ">%d", e);
		const uint16_t a = st.actions;
		if (e == kReBlockOk) n.blocked = true;
		if (e == kReRedirectOk || e == kReRedirectRefused) {
			CHECK(w.blocked, "%s: redirect while not blocked", n.path);
			n.blocked = false, n.redirected = true;
		}
		if (a & kRaUnblock) {
			CHECK(w.blocked, "%s: unblock while not blocked", n.path);
			n.blocked = false, n.unblocked = true;
		}
		if (e == kReRemapOk) n.gpuSys = true;
		if (a & kRaGpuBack) n.gpuSys = false;
		if (a & kRaFreeVram) {
			CHECK(!(a & kRaGpuBack), "%s: VRAM freed while the GPU goes back to it", n.path);
			CHECK(w.s == kRsRedirected && e == kReVerifyOk, "%s: VRAM freed before the verification", n.path);
			CHECK(!(a & kRaKeepVram), "%s: VRAM freed and kept", n.path);
			n.vramFreed = true;
		}
		if (a & kRaFreeSys) n.sysFreed = true;
		if (a & kRaKeepSys) n.sysKept = true;
		if (a & kRaKeepVram) n.vramKept = true;
		if (a & kRaLatchOff) n.latched = true;
		if (a & kRaAdopt) CHECK(st.next == kRsDone || st.next == kRsUndecided, "%s: adopted outside Done", n.path);
		if (a & kRaAdoptDesc) CHECK(w.s == kRsRedirected, "%s: descriptor adopted without a redirect", n.path);
		if (a & kRaAdoptDesc) CHECK(a & kRaKeepPagers, "%s: a redirect that reached the map must keep the pagers", n.path);
		// the GPU must still be on the VRAM while the CPU is blocked and the copy is not done
		if (xeRevokeCpuBlocked(st.next) && st.next != kRsGpuMoved) CHECK(!n.gpuSys, "%s: GPU on sys before the copy", n.path);
		CHECK(xeRevokeCpuBlocked(st.next) == n.blocked, "%s: blocked state %d vs walker %d", n.path, st.next, n.blocked);
		CHECK(xeRevokeGpuOnSys(st.next) == n.gpuSys || st.next == kRsRolledBack || st.next == kRsBroken,
			"%s: GPU state %d vs walker", n.path, st.next);
		n.s = st.next;
		n.acts |= a;
		walk(n);
	}
}

int main() {
	// 1.
	const uint64_t now = 10000000000ull, hot = 1000000000ull, max = 16ull << 20;
	{
		XeRevokeFacts f = good();
		CHECK(xeRevokeCheck(f, max, now, hot) == kRvOk, "good BO: %s", xeRevokeWhyName(xeRevokeCheck(f, max, now, hot)));
		struct { void (*set)(XeRevokeFacts &); XeRevokeWhy want; } cases[] = {
			{[](XeRevokeFacts &x) { x.inWindow = false; }, kRvNotWindow},
			{[](XeRevokeFacts &x) { x.mapped = false; x.liveMaps = 0; }, kRvUnmapped},
			{[](XeRevokeFacts &x) { x.pinned = true; }, kRvPinned},
			{[](XeRevokeFacts &x) { x.shared = true; }, kRvShared},
			{[](XeRevokeFacts &x) { x.purgeable = true; }, kRvPurgeable},
			{[](XeRevokeFacts &x) { x.compressed = true; }, kRvCompressed},
			{[](XeRevokeFacts &x) { x.contig = false; }, kRvNotContig},
			{[](XeRevokeFacts &x) { x.size = 32ull << 20; }, kRvTooBig},
			{[](XeRevokeFacts &x) { x.size = (2ull << 20) + 1; }, kRvTooBig},
			{[](XeRevokeFacts &x) { x.size = 0; }, kRvTooBig},
			{[](XeRevokeFacts &x) { x.moving = true; }, kRvMoving},
			{[](XeRevokeFacts &x) { x.unusedMaps = 1; }, kRvInFlight},
			{[](XeRevokeFacts &x) { x.descBusy = true; }, kRvInFlight},
			{[](XeRevokeFacts &x) { x.liveMaps = 2; }, kRvMaps},
			{[](XeRevokeFacts &x) { x.liveMaps = 0; }, kRvMaps},   // mapped (sticky) but its mapping is gone
			{[](XeRevokeFacts &x) { x.otherMaps = 1; }, kRvMaps},
			{[](XeRevokeFacts &x) { x.refsUnaccounted = true; }, kRvRefs},
			{[](XeRevokeFacts &x) { x.clearPending = true; }, kRvClear},
			{[](XeRevokeFacts &x) { x.vmBusy = true; }, kRvBusy},
			{[](XeRevokeFacts &x) { x.touchNs = now - hot / 2; }, kRvHot},
			{[](XeRevokeFacts &x) { x.touchNs = now + 5; }, kRvHot},
		};
		for (auto &cs : cases) {
			XeRevokeFacts x = good();
			cs.set(x);
			const XeRevokeWhy w = xeRevokeCheck(x, max, now, hot);
			CHECK(w == cs.want, "want %s, got %s", xeRevokeWhyName(cs.want), xeRevokeWhyName(w));
		}
		XeRevokeFacts x = good();
		x.touchNs = now - hot / 2;
		CHECK(xeRevokeCheck(x, max, now, 0) == kRvOk, "forced path ignores the hot window");
		x.touchNs = now - hot;
		CHECK(xeRevokeCheck(x, max, now, hot) == kRvOk, "exactly hotNs ago is cold");
		// order: a pinned BO that is also busy reports pinned (the cheaper, permanent reason first)
		x = good();
		x.pinned = x.vmBusy = true;
		CHECK(xeRevokeCheck(x, max, now, hot) == kRvPinned, "first reason wins");
		CHECK(!strcmp(xeRevokeWhyName(kRvHot), "hot") && !strcmp(xeRevokeWhyName((XeRevokeWhy)99), "?"), "names");
	}

	// 2.
	{
		XeRevokeCand c[6] = {{1 << 20, 500, 1}, {4 << 20, 100, 2}, {2 << 20, 300, 3}, {2 << 20, 100, 4}, {8 << 20, 900, 5},
			{4 << 20, 100, 6}};
		const uint32_t k = xeRevokePick(c, 6, 9 << 20);
		// order: t=100 (4 MB id 2, 4 MB id 6, 2 MB id 4), t=300, t=500, t=900
		CHECK(c[0].id == 2 && c[1].id == 6 && c[2].id == 4 && c[3].id == 3 && c[4].id == 1 && c[5].id == 5, "order %u %u %u %u %u %u",
			c[0].id, c[1].id, c[2].id, c[3].id, c[4].id, c[5].id);
		CHECK(k == 3, "9 MB needs the first three (4+4+2): %u", k);
		CHECK(xeRevokePick(c, 6, 0) == 0, "nothing needed");
		CHECK(xeRevokePick(c, 6, 100ull << 20) == 6, "more than all: all");
		CHECK(xeRevokePick(c, 0, 1) == 0, "none");
		CHECK(xeRevokePick(c, 6, 1) == 1, "one byte: one BO");
		// the cut-off over two clients equals a global pick
		XeRevokeCand a[3] = {{2 << 20, 50, 1}, {2 << 20, 700, 2}, {2 << 20, 200, 3}};   // client A
		XeRevokeCand b[3] = {{2 << 20, 100, 4}, {2 << 20, 800, 5}, {2 << 20, 150, 6}};  // client B
		XeRevokeCand all[6];
		memcpy(all, a, sizeof(a));
		memcpy(all + 3, b, sizeof(b));
		const uint64_t cut = xeRevokeCutoff(all, 6, 7 << 20);   // four BOs: 50, 100, 150, 200
		CHECK(cut == 200, "cut-off %llu", (unsigned long long)cut);
		uint32_t taken = 0;
		for (auto &x : a) taken += x.touchNs <= cut;
		for (auto &x : b) taken += x.touchNs <= cut;
		CHECK(taken == 4, "per-client pass at the cut-off takes %u", taken);
		CHECK(xeRevokeCutoff(all, 6, 0) == 0, "no need, no cut-off");
	}

	// 3.
	{
		Walk w = {};
		w.s = kRsStart;
		walk(w);
		CHECK(finals == 8, "paths to a final state: %d", finals);   // BlockFail, CopyFail, CopyLate, RemapFail, Refused,
		                                                              // VerifyFail, VerifyUnknown, Done
		// the expected outcomes, path by path
		struct { XeRevokeEvent ev[6]; int n; XeRevokeState end; uint16_t must, mustNot; } paths[] = {
			{{kReBlockFail}, 1, kRsRolledBack, kRaFreeSys, kRaUnblock | kRaFreeVram},
			{{kReBlockOk, kReCopyFail}, 2, kRsRolledBack, kRaUnblock | kRaFreeSys, kRaFreeVram | kRaGpuBack},
			{{kReBlockOk, kReCopyLate}, 2, kRsRolledBack, kRaUnblock | kRaKeepSys | kRaKeepVram, kRaFreeSys | kRaFreeVram},
			{{kReBlockOk, kReCopyOk, kReRemapFail}, 3, kRsRolledBack, kRaUnblock | kRaGpuBack | kRaFreeSys, kRaFreeVram},
			{{kReBlockOk, kReCopyOk, kReRemapOk, kReRedirectRefused}, 4, kRsRolledBack, kRaGpuBack | kRaKeepSys | kRaLatchOff,
				kRaUnblock | kRaFreeSys | kRaFreeVram},
			{{kReBlockOk, kReCopyOk, kReRemapOk, kReRedirectOk, kReVerifyFail}, 5, kRsBroken,
				kRaGpuBack | kRaKeepSys | kRaLatchOff | kRaAdoptDesc, kRaFreeVram | kRaFreeSys | kRaUnblock},
			{{kReBlockOk, kReCopyOk, kReRemapOk, kReRedirectOk, kReVerifyUnknown}, 5, kRsUndecided,
				kRaKeepVram | kRaLatchOff | kRaAdoptDesc | kRaKeepPagers, kRaFreeVram | kRaFreeSys | kRaGpuBack | kRaUnblock},
			{{kReBlockOk, kReCopyOk, kReRemapOk, kReRedirectOk, kReVerifyOk}, 5, kRsDone,
				kRaFreeVram | kRaAdopt | kRaAdoptDesc | kRaKeepPagers,
				kRaGpuBack | kRaFreeSys | kRaKeepSys | kRaLatchOff},
		};
		for (auto &p : paths) {
			XeRevokeState s = kRsStart;
			uint16_t acts = 0;
			bool ok = true;
			for (int i = 0; i < p.n; i++) {
				const XeRevokeStep st = xeRevokeNext(s, p.ev[i]);
				ok &= st.valid;
				acts |= st.actions;
				s = st.next;
			}
			CHECK(ok && s == p.end, "path of %d events ends in %d (want %d)", p.n, s, p.end);
			CHECK((acts & p.must) == p.must, "path of %d events: actions %#x lack %#x", p.n, acts, p.must);
			CHECK(!(acts & p.mustNot), "path of %d events: actions %#x include %#x", p.n, acts, acts & p.mustNot);
		}
		// events out of order are refused
		CHECK(!xeRevokeNext(kRsStart, kReVerifyOk).valid, "verify before anything");
		CHECK(!xeRevokeNext(kRsBlocked, kReRedirectOk).valid, "redirect before the copy");
		CHECK(!xeRevokeNext(kRsCopied, kReRedirectOk).valid, "redirect before the GPU moved");
		CHECK(!xeRevokeNext(kRsDone, kReBlockOk).valid, "nothing after Done");
		CHECK(!xeRevokeNext(kRsRolledBack, kReBlockOk).valid, "nothing after a rollback");
	}

	// 4.
	{
		const uint16_t done = kRaFreeVram | kRaAdopt | kRaAdoptDesc | kRaKeepPagers;
		CHECK(xeRevokeReleasable(done) == kRdKmap, "Done releases only the kernel mapping: %#x", xeRevokeReleasable(done));
		CHECK(xeRevokePagerOwners(kRsDone, done) == kRdPhys, "Done: the user's object faults through phys's pager");
		// barwin's release (bridge to the map, old descriptor and phys dropped): phys owned the pager -> SIGBUS
		const uint16_t barwin = kRaFreeVram | kRaAdopt | kRaAdoptDesc;
		CHECK(xeRevokeReleasable(barwin) & xeRevokePagerOwners(kRsDone, barwin) & kRdPhys,
			"the barwin rule released the pager owner (the B580 SIGBUS), so this check must see it");
		CHECK(xeRevokeReleasable(kRaUnblock | kRaFreeSys) == (kRdKmap | kRdBridge | kRdPhys), "copy failure releases all set-up");
		CHECK(xeRevokePagerOwners(kRsRolledBack, kRaGpuBack | kRaKeepSys | kRaLatchOff) == kRdOld, "refused: old owns it");
	}

	printf("%s: %d of %d checks failed\n", fails ? "FAILED" : "PASSED", fails, checks);
	return fails != 0;
}
