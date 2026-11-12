// SPDX-License-Identifier: MIT
// tlbinval_test: offline check of the TLB invalidation policy's pure parts (xe_tlbinval.h, rb8c), no GPU needed.
//   1. xeTlbInvalRange equals Linux xe_guc_tlb_inval.c send_tlb_inval_ppgtt (v6.18, written out below as it is there)
//      on edge cases and 200000 random ranges, and its result has the properties the xe_gt_asserts there check: a power
//      of two of at least 4 KB, start aligned to it, [start, end) covered, no length strictly between 2 MB and 16 MB
//   2. a 4 KB unbind inside a 2 MB leaf: the bind's interval with the split's touch (ptSplit) yields an invalidation
//      that covers the whole 2 MB leaf with at least 16 MB, the minimum the hardware wants for 2 MB pages; the touch of
//      the changed 4 KB alone (before rb8c) does not
//   3. XeInvalInterval: a first range at VA 0, unions, single ranges
//   4. the deferred-free rule (xeDeferReady) through a model of deferReap: an entry goes only once it is sealed, its
//      invalidation has completed and every job snapshotted with it has completed; a job snapshot shared by the entries
//      of one seal is released with the last of them
//   5. rb8d, a VM_BIND's out-fence (xeBindFenceNext / xeBindFenceParts) through a model of a VM: random calls that bind
//      (a clear on the migrate queue) and/or unbind (an invalidation), clears and invalidations completing in order at
//      random: whenever the parts a call's out-sync got have all signaled, that call's clear and invalidation have
//      completed (xe_pt_update_ops_run: "tlb invalidation must be done before signaling unbind/rebind") and so have
//      those of every earlier call (one bind queue's fences signal in order); the clear alone (before rb8d) signals
//      with the invalidation still pending
//   6. rb8d, xeTlbTimedOut (xe_tlb_inval_fence_timeout): only invalidations issued at least the timeout ago count as
//      done, in order, never past the last one issued, across the seqno wrap and with more than 64 pending
//   7. rb8d, xeTlbInvalOp: MAKE_INVAL_OP of xe_guc_tlb_inval.c for the GGTT (XE_GUC_TLB_INVAL_GUC), full and
//      page-selective invalidations, equal to the dwords the PPGTT paths have always sent
//   8. rb8d, xeTimelineValue: a timeline point holding a bind's clear and invalidation is reached only once both have
//      signaled, whatever their order; the value never counts a point with an unsignaled fence, against a brute-force
//      definition on random timelines (several fences per point, random signaled sets)
// c++ -std=c++17 -Wall -Wextra -I.. -o tlbinval_test tlbinval_test.cpp && ./tlbinval_test
#include <stdio.h>
#include <stdlib.h>
#include <vector>
#include "xe_tlbinval.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// send_tlb_inval_ppgtt's range part, xe_guc_tlb_inval.c (v6.18), with the kernel helpers it uses
static uint64_t roundup_pow_of_two(uint64_t x) { uint64_t p = 1; while (p < x) p <<= 1; return p; }
static uint64_t ALIGN_DOWN(uint64_t x, uint64_t a) { return x & ~(a - 1); }
static uint64_t ALIGN(uint64_t x, uint64_t a) { return (x + a - 1) & ~(a - 1); }
static uint32_t ilog2(uint64_t x) { return 63 - __builtin_clzll(x); }
static void linuxRange(uint64_t start, uint64_t end, uint64_t *outStart, uint32_t *outLog2) {
	uint64_t length = end - start;
	uint64_t orig_start = start, align;
	if (length < 0x1000) length = 0x1000;
	align = roundup_pow_of_two(length);
	start = ALIGN_DOWN(start, align);
	end = ALIGN(end, align);
	length = align;
	while (start + length < end) {
		length <<= 1;
		start = ALIGN_DOWN(orig_start, length);
	}
	if (length >= (2ull << 20)) {
		length = length > (16ull << 20) ? length : (16ull << 20);
		start = ALIGN_DOWN(orig_start, length);
	}
	*outStart = start;
	*outLog2 = ilog2(length);
}

static uint64_t rnd64() { return (uint64_t)random() << 33 ^ (uint64_t)random() << 11 ^ (uint64_t)random(); }

static void checkRange(uint64_t a, uint64_t b) {
	uint64_t s = 0, ls = 0;
	uint32_t lg = 0, llg = 0;
	if (!xeTlbInvalRange(a, b, &s, &lg)) {
		CHECK(b <= a || b - a > (1ull << 46), "[0x%llx, 0x%llx): full invalidation for a range that fits", a, b);
		return;
	}
	linuxRange(a, b, &ls, &llg);
	CHECK(s == ls && lg == llg, "[0x%llx, 0x%llx): 0x%llx/2^%u, Linux 0x%llx/2^%u", a, b, s, lg, ls, llg);
	const uint64_t len = 1ull << lg;
	CHECK(lg >= 12, "[0x%llx, 0x%llx): length 2^%u below 4 KB", a, b, lg);
	CHECK(!(s & (len - 1)), "[0x%llx, 0x%llx): start 0x%llx not aligned to 2^%u", a, b, s, lg);
	CHECK(s <= a && s + len >= b, "[0x%llx, 0x%llx): 0x%llx + 2^%u does not cover it", a, b, s, lg);
	CHECK(!(lg > 21 && lg < 24), "[0x%llx, 0x%llx): length 2^%u between 2 MB and 16 MB", a, b, lg);
}

int main() {
	// 1. against Linux
	const uint64_t edges[][2] = {{0, 0x1000}, {0x1000, 0x2000}, {0xfff000, 0x1001000}, {0x200000, 0x400000},
		{0x1ff000, 0x201000}, {0x3fa06000, 0x3fa07000}, {0x7fffff000000ull, 0x800000000000ull}, {0, 1ull << 46},
		{0x10000, 0x20000}, {0x40000000, 0x80000000}, {0x123456789000ull, 0x12345678a000ull}, {0x1000, 0x1000},
		{0, (1ull << 46) + 0x1000}};
	for (auto &e : edges) checkRange(e[0], e[1]);
	srandom(12);
	for (int i = 0; i < 200000; i++) {
		const uint64_t a = rnd64() & ((1ull << 48) - 1) & ~0xfffull;
		const int sh = (int)(random() % 34) + 12;
		const uint64_t len = ((rnd64() & ((1ull << sh) - 1)) | 0x1000) & ~0xfffull;
		if (a + len > (1ull << 48)) continue;
		checkRange(a, a + len);
	}

	// 2. a 4 KB unbind in the middle of a 2 MB leaf at 0x3fa00000
	{
		const uint64_t leaf = 0x3fa00000, page = leaf + 0x6000;
		XeInvalInterval old = {}, now = {};
		old.touch(page, page + 0x1000);                 // before rb8c: the cleared PTE only
		now.touch(leaf, leaf + xePtSpan(1));            // ptSplit: the whole leaf the split replaces
		now.touch(page, page + 0x1000);
		uint64_t s = 0;
		uint32_t lg = 0;
		CHECK(xeTlbInvalRange(old.lo, old.hi, &s, &lg) && lg == 12, "old: 2^%u", lg);
		CHECK(!(s <= leaf && s + (1ull << lg) >= leaf + 0x200000), "old: the 4 KB invalidation would cover the leaf");
		CHECK(xeTlbInvalRange(now.lo, now.hi, &s, &lg), "new: a range");
		CHECK(lg >= 24 && s <= leaf && s + (1ull << lg) >= leaf + 0x200000, "new: 0x%llx/2^%u does not cover the 2 MB leaf "
			"with >= 16 MB", s, lg);
		// a split 1 GB leaf: the whole GB
		XeInvalInterval g = {};
		g.touch(0x40000000, 0x40000000 + xePtSpan(2));
		CHECK(xeTlbInvalRange(g.lo, g.hi, &s, &lg) && s == 0x40000000 && lg == 30, "1 GB split: 0x%llx/2^%u", s, lg);
		CHECK(xePtSpan(0) == 0x1000 && xePtSpan(1) == 0x200000 && xePtSpan(2) == 0x40000000, "spans");
	}

	// 3. intervals
	{
		XeInvalInterval iv = {};
		CHECK(!iv.any, "empty");
		iv.touch(0, 0x1000);
		CHECK(iv.any && iv.lo == 0 && iv.hi == 0x1000, "VA 0: [0x%llx, 0x%llx)", iv.lo, iv.hi);
		iv.touch(0x5000, 0x6000);
		CHECK(iv.lo == 0 && iv.hi == 0x6000, "union: [0x%llx, 0x%llx)", iv.lo, iv.hi);
		XeInvalInterval j = {};
		j.touch(0x7000, 0x8000);
		j.touch(0x2000, 0x3000);
		CHECK(j.lo == 0x2000 && j.hi == 0x8000, "lower later: [0x%llx, 0x%llx)", j.lo, j.hi);
	}

	// 4. deferred frees: entries {sealed, tlb seq, snapshot}; fences complete one by one
	{
		CHECK(!xeDeferReady(false, true, true) && !xeDeferReady(true, false, true) && !xeDeferReady(true, true, false) &&
			xeDeferReady(true, true, true), "truth table");
		struct Snap { int refs; std::vector<int> jobs; bool freed; };
		struct Entry { bool sealed; uint32_t seq; Snap *snap; bool freed; };
		std::vector<bool> jobDone(4, false);
		uint32_t tlbDone = 0;
		Snap snap = {0, {1, 2}, false};          // a seal while jobs 1 and 2 of the VM ran
		std::vector<Entry> e = {{true, 5, &snap, false}, {true, 5, &snap, false}, {true, 3, nullptr, false},
			{false, 0, nullptr, false}};
		snap.refs = 2;
		auto reap = [&] {
			for (auto &x : e) {
				if (x.freed) continue;
				bool jobs = true;
				if (x.snap) for (int j : x.snap->jobs) jobs = jobs && jobDone[(size_t)j];
				if (xeDeferReady(x.sealed, x.sealed && (int32_t)(tlbDone - x.seq) >= 0, jobs)) {
					x.freed = true;
					if (x.snap && !--x.snap->refs) x.snap->freed = true;
				}
			}
		};
		reap();
		CHECK(!e[0].freed && !e[2].freed, "nothing before any invalidation");
		tlbDone = 5;
		reap();
		CHECK(e[2].freed && !e[0].freed && !e[1].freed && !e[3].freed, "invalidation done, jobs running: only the "
			"entry without jobs");
		jobDone[1] = true;
		reap();
		CHECK(!e[0].freed, "one of two jobs done");
		jobDone[2] = true;
		reap();
		CHECK(e[0].freed && e[1].freed && snap.freed && !e[3].freed, "all done: both entries, the snapshot; the unsealed "
			"entry stays");
	}

	// 5. bind out-fences
	{
		uint32_t clearSeq = 0, tlbSeq = 0xfffffff0u, clearDone = 0, tlbDone = 0xfffffff0u;   // tlb seqnos wrap here
		auto sig = [&](const XeFence &f) {
			if (f.slot == kXeFenceDone) return true;
			if (f.slot == kXeTlbSlot) return xeSeqDone(tlbDone, f.seqno);
			return f.slot == 7 && f.gen == 1 ? xeSeqDone(clearDone, f.seqno) : true;   // the migrate queue: slot 7 gen 1
		};
		struct Call { XeFence parts[2]; uint32_t n; bool hasClear, hasTlb; uint32_t clear, tlb; XeFence old; };
		std::vector<Call> calls;
		XeBindFence prev = {{0, kXeFenceDone, 0, 0}, {0, kXeFenceDone, 0, 0}};
		srandom(5);
		uint32_t oldEarly = 0, checked = 0;
		for (int step = 0; step < 20000; step++) {
			const int r = (int)(random() % 8);
			if (r < 4) {   // a VM_BIND
				Call c = {};
				c.hasClear = random() % 2;
				c.hasTlb = random() % 2;
				XeFence clr = {0, kXeFenceDone, 0, 0};
				if (c.hasClear) clr = {0, 7, 1, c.clear = ++clearSeq};
				if (c.hasTlb) c.tlb = ++tlbSeq;
				prev = xeBindFenceNext(prev, clr, c.hasTlb, c.tlb, sig);
				c.n = xeBindFenceParts(prev, c.parts, sig);
				c.old = clr;
				CHECK(c.n >= 1 && c.n <= 2, "parts %u", c.n);
				calls.push_back(c);
			} else if (r < 6) {
				if (clearDone != clearSeq) clearDone++;
			} else {
				if (tlbDone != tlbSeq) tlbDone++;
			}
			// every call: its parts signaled => it and everything before it complete
			bool allBefore = true;
			for (auto &c : calls) {
				const bool own = (!c.hasClear || xeSeqDone(clearDone, c.clear)) && (!c.hasTlb || xeSeqDone(tlbDone, c.tlb));
				bool s = true;
				for (uint32_t k = 0; k < c.n; k++) s = s && sig(c.parts[k]);
				if (s) {
					CHECK(own && allBefore, "step %d: out-fence signaled, own %d, earlier calls %d", step, own, allBefore);
					checked++;
				}
				if (c.hasTlb && sig(c.old) && !xeSeqDone(tlbDone, c.tlb)) oldEarly++;
				allBefore = allBefore && own;
			}
			if (calls.size() > 64) calls.erase(calls.begin(), calls.begin() + 32);
		}
		CHECK(checked > 1000, "signaled out-fences checked: %u", checked);
		CHECK(oldEarly > 0, "the clear-only fence (before rb8d) never signaled ahead of its invalidation: %u", oldEarly);
		// a call without ops carries the previous one's fence
		XeBindFence p0 = {{0, kXeFenceDone, 0, 0}, xeTlbFence(tlbSeq + 1)};
		const XeBindFence p1 = xeBindFenceNext(p0, XeFence{0, kXeFenceDone, 0, 0}, false, 0, sig);
		CHECK(p1.tlb.slot == kXeTlbSlot && p1.tlb.seqno == tlbSeq + 1, "no-op call: invalidation carried");
		XeFence parts[2];
		CHECK(xeBindFenceParts(p1, parts, sig) == 1 && parts[0].slot == kXeTlbSlot, "no-op call: one part, the invalidation");
		tlbDone = tlbSeq + 1;
		CHECK(xeBindFenceParts(p1, parts, sig) == 1 && parts[0].slot == kXeFenceDone, "all done: one signaled part");
	}

	// 6. the TLB fence watchdog
	{
		uint64_t at[64] = {};
		const uint64_t T = 1000000000ull;
		uint32_t issued = 0xffffffe0u;   // across the wrap
		const uint32_t done = issued;
		for (uint32_t i = 1; i <= 40; i++) at[(issued + i) & 63] = 1000 * T + i * (T / 10);   // one every 100 ms
		issued += 40;
		CHECK(xeTlbTimedOut(done, issued, at, 1000 * T, T) == done, "nothing a second old");
		const uint32_t to = xeTlbTimedOut(done, issued, at, 1000 * T + 15 * (T / 10), T);
		CHECK(to == done + 5, "1.5 s after the first: the first five, got %d", (int)(to - done));
		CHECK(xeTlbTimedOut(done, issued, at, 2000 * T, T) == issued, "all, never past the last issued");
		CHECK(xeTlbTimedOut(issued, issued, at, 2000 * T, T) == issued, "none pending");
		at[(done + 3) & 63] = 2000 * T;   // overwritten by a later seqno (more than 64 pending): read as newer
		CHECK(xeTlbTimedOut(done, issued, at, 1000 * T + 15 * (T / 10), T) == done + 2, "an overwritten time only delays");
		at[(done + 1) & 63] = 1000 * T + 20 * T;   // issued after "now" (a racing issue): not timed out
		CHECK(xeTlbTimedOut(done, issued, at, 1000 * T + 10 * T, T) == done, "an issue time after now");
	}

	// 7. TLB_INVALIDATION operation dwords (abi/guc_actions_abi.h: TYPE_SHIFT 0, MODE_SHIFT 8, MODE_HEAVY 0, FLUSH_CACHE
	// bit 31; FULL 0, PAGE_SELECTIVE 1, GUC 3)
	{
		auto makeInvalOp = [](uint32_t type) { return (type << 0) | (0x0u << 8) | (1u << 31); };
		CHECK(xeTlbInvalOp(kXeTlbInvalGuc) == makeInvalOp(0x3) && xeTlbInvalOp(kXeTlbInvalGuc) == 0x80000003u, "GGTT op 0x%x",
			xeTlbInvalOp(kXeTlbInvalGuc));
		CHECK(xeTlbInvalOp(kXeTlbInvalFull) == 0x80000000u, "full op (tlbInvalidateIssue)");
		CHECK(xeTlbInvalOp(kXeTlbInvalPageSelective) == (1u | 1u << 31), "page-selective op (tlbInvalidateRange)");
	}

	// 8. timeline values
	{
		std::vector<bool> done;
		auto sig = [&](const XeFence &f) { return f.slot == kXeFenceDone || done[f.seqno]; };
		uint32_t counted = 0;
		// point 5 = {clear, invalidation}, point 4 before it
		XeFence tl[3] = {{4, 1, 0, 0}, {5, 1, 0, 1}, {5, 1, 0, 2}};
		done = {true, true, false};
		CHECK(xeTimelineValue(tl, 3, sig, &counted) == 4 && counted == 1, "clear done, invalidation not: 4 (old rule gave 5)");
		done = {true, false, true};
		CHECK(xeTimelineValue(tl, 3, sig, &counted) == 4 && counted == 1, "invalidation done, clear not: 4");
		done = {true, true, true};
		CHECK(xeTimelineValue(tl, 3, sig, &counted) == 5 && counted == 3, "both: 5");
		srandom(8);
		for (int r = 0; r < 20000; r++) {
			const uint32_t n = 1 + (uint32_t)(random() % 12);
			std::vector<XeFence> f(n);
			done.assign(n, false);
			uint64_t p = (uint64_t)(random() % 3);
			for (uint32_t k = 0; k < n; k++) {
				if (random() % 2) p += 1 + (uint64_t)(random() % 2);   // several fences per point at times
				f[k] = {p, 1, 0, k};
				done[k] = random() % 3 != 0;
			}
			// brute force: the highest point q among the entries such that every entry with point <= q has signaled
			uint64_t want = 0;
			for (uint32_t k = 0; k < n; k++) {
				bool ok = true;
				for (uint32_t j = 0; j < n; j++) ok = ok && (f[j].point > f[k].point || done[j]);
				if (ok && f[k].point > want) want = f[k].point;
			}
			const uint64_t got = xeTimelineValue(f.data(), n, sig, &counted);
			CHECK(got == want, "random timeline %d: value %llu, brute force %llu", r, (unsigned long long)got,
				(unsigned long long)want);
			for (uint32_t k = 0; k < counted; k++) CHECK(done[k], "random timeline %d: entry %u counted unsignaled", r, k);
		}
	}

	printf("tlbinval_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
