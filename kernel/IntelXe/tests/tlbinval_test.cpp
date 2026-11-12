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

	printf("tlbinval_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
