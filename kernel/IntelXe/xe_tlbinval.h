// SPDX-License-Identifier: MIT
// The pure parts of the PPGTT TLB invalidation policy (rb8c), so that tests/tlbinval_test.cpp builds them on the host:
//  - the interval one VM_BIND's page-table changes must invalidate (XeInvalInterval, was BindCtx::touch), and the span a
//    page-table entry at a level stands for (xePtSpan): a change of an entry invalidates the whole span the entry maps,
//    as xe_pt.c xe_pt_update_ops_rfence_interval aligns an update's range to the span of the highest-level entry it
//    wrote. Splitting a live huge leaf (2 MB at level 1, 1 GB at level 2) is such a change of the parent entry;
//  - the range a PAGE_SELECTIVE invalidation is widened to (xe_guc_tlb_inval.c send_tlb_inval_ppgtt: a power of two,
//    aligned to itself, at least 4 KB, and at least 16 MB from 2 MB up: "Minimum invalidation size for a 2MB page that
//    the hardware expects is 16MB");
//  - when a deferred free may go: its TLB invalidation has completed AND every job the VM's queues had been handed
//    when the unbind was sealed has completed. xe frees page-table BOs and VM-private BO memory through TTM, whose
//    ttm_bo_release defers the release until every fence of the (VM-shared) dma-resv has signaled, BOOKKEEP included:
//    the exec jobs (xe_exec.c drm_gpuvm_resv_add_fence(..., DMA_RESV_USAGE_BOOKKEEP)) and the bind and TLB invalidation
//    fences (xe_pt.c xe_pt_update_ops_run). Before rb8c only the invalidation was waited for.
#pragma once
#include <stdint.h>

// Bytes one entry of a level-`level` table maps (xe_pt_shift: 12 + 9 * level).
static inline uint64_t xePtSpan(int level) { return 1ull << (12 + 9 * level); }

// The VA interval one bind call's page-table changes cover (unions every touched range).
struct XeInvalInterval {
	bool any;
	uint64_t lo, hi;
	void touch(uint64_t a, uint64_t b) {
		if (!any) {
			any = true;
			lo = a;
			hi = b;
			return;
		}
		if (a < lo) lo = a;
		if (b > hi) hi = b;
	}
};

// send_tlb_inval_ppgtt's range for [start, end): false = a full invalidation instead (the length is beyond what a
// range invalidation encodes); else *outStart and *outLog2 (log2 of the length, >= 12).
static inline bool xeTlbInvalRange(uint64_t start, uint64_t end, uint64_t *outStart, uint32_t *outLog2) {
	if (end <= start || end - start > (1ull << 46)) return false;
	const uint64_t orig = start;
	uint64_t len = end - start < 0x1000 ? 0x1000 : end - start;
	uint64_t align = len <= 1 ? 1 : 1ull << (64 - __builtin_clzll(len - 1));   // roundup_pow_of_two
	start &= ~(align - 1);
	end = (end + align - 1) & ~(align - 1);
	len = align;
	while (start + len < end) {
		len <<= 1;
		start = orig & ~(len - 1);
	}
	if (len >= (2ull << 20)) {
		if (len < (16ull << 20)) len = 16ull << 20;
		start = orig & ~(len - 1);
	}
	*outStart = start;
	*outLog2 = (uint32_t)(63 - __builtin_clzll(len));
	return true;
}

// A deferred free's state: sealed (its bind call issued the invalidation it waits for), that invalidation done, the
// VM's jobs of that moment done. Only then may the memory be reused.
static inline bool xeDeferReady(bool sealed, bool tlbDone, bool jobsDone) { return sealed && tlbDone && jobsDone; }
