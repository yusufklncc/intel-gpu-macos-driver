// SPDX-License-Identifier: MIT
// tlbinring: an EXEC whose only pending in-fences are TLB invalidations (a bind's out-fence since rb8d) waits for them on
// the GPU, at the head of its job, instead of in the FIFO; the pure parts, so that tests/tlbinring_test.cpp builds them
// on the host. docs/design/DESIGN-rb8-cost.md, "tlbinring".
//
// Linux (v6.18): xe_pt.c xe_pt_update_ops_run makes an unbind's fence the TLB invalidation job's fence ("tlb
// invalidation must be done before signaling unbind/rebind"; xe_tlb_inval_job.c xe_tlb_inval_job_push, run by its
// dep scheduler once the page-table update has signaled); vm_bind_ioctl_ops_fini signals it into the out-syncs. An exec
// that names the bind's syncobj gets it as a drm_sched dependency (xe_exec.c xe_exec_ioctl -> xe_sync.c
// xe_sync_entry_add_deps -> drm_sched_job_add_dependency), so the job's run_job (the ring write) happens only after
// xe_tlb_inval.c xe_tlb_inval_done_handler (or xe_tlb_inval_fence_timeout) signaled it. The job's own ring then
// invalidates the engine's TLB before the batch when the VM's tlb_flush_seqno moved (xe_sched_job.c xe_sched_job_arm,
// xe_ring_ops.c emit_pipe_invalidate(..., ring_ops_flush_tlb)).
//
// Here: the same order, kept on the GPU. The job goes to the ring (an armed direct-submission slot, or with
// debug.intelxe.tlb_inring 2 the normal ring path) with an MI_SEMAPHORE_WAIT at its head, in the preparser-disabled
// part before the invalidating PIPE_CONTROL: poll the TLB word (a GGTT dword that the CPU sets to the invalidation
// done seqno whenever it moves: TLB_INVALIDATION_DONE, the fence timeout, a GT reset or resume) until it is >= the
// newest invalidation among the in-fences. Nothing of the job (the in-ring TLB flush, the batch, the user fence, the
// seqno) runs before every invalidation it depends on has completed on every GuC it went to, which is what Linux's
// dependency gives; only the place of the wait moves from the CPU to the command streamer. Freed memory is not
// involved: deferred frees wait for the CPU-side done seqno and the VM's jobs (xeDeferReady), as before.
#pragma once
#include <stdint.h>
#include "xe_tlbinval.h"

// The value the TLB word gets: done on every GuC an invalidation goes to (the primary, and the media GuC while it is
// up), i.e. the one behind. tlbSeqDone(s) == xeSeqDone(xeTlbDoneBoth(...), s).
static inline uint32_t xeTlbDoneBoth(uint32_t primary, uint32_t media, bool mediaOn) {
	return !mediaOn || xeSeqDone(media, primary) ? primary : media;
}

// Whether the word moves from cur to v: forward in seqno order only (racing publishers, a late one with an older value,
// never move it back; the word is always a value that was done).
static inline bool xeTlbWordAdvances(uint32_t cur, uint32_t v) { return (int32_t)(v - cur) > 0; }

// MI_SEMAPHORE_WAIT compares unsigned (COMPARE_OP_SAD_GTE_SDD: word >= target), the seqnos wrap at 2^32. A target is
// taken only when the unsigned order of word and target is their seqno order (word below it, or past it without a wrap
// in between: the wait then passes at once) and the target is nonzero and below kXeTlbWordGuard, so the word reaches
// it before it can wrap: stepping from below the target past 0xffffffff in one publish would need 2^20 invalidations
// issued and unanswered, while tlbStallCheck counts every invalidation done 2.25 s after its issue and each one is a
// VM_BIND, a queue create/destroy or a migrate window. Targets from the guard to the wrap (one in 4096 seqnos) take the
// old path. A job stranded anyway (beyond that bound) waits in the ring, not started, until the job watchdog's
// never-started rule resets the GT (twice, then bans the queue): it never runs early.
static constexpr uint32_t kXeTlbWordGuard = 0xfff00000u;

// What an EXEC's pending in-fences allow (deps: the unsignaled ones, its own queue's left out).
enum XeTlbInring : uint32_t {
	kXeTlbInringNone,    // no TLB-invalidation fence among them (nothing for this path)
	kXeTlbInringWait,    // only TLB-invalidation fences: *target, the newest, is what the job waits for in the ring
	kXeTlbInringOther,   // a TLB-invalidation fence and another kind (a job, a clear, a queued bind): the FIFO
	kXeTlbInringWrap,    // only TLB fences, but the target is not usable with an unsigned compare against word
};
static inline XeTlbInring xeTlbInringPlan(const XeFence *f, uint32_t n, uint32_t word, uint32_t *target) {
	bool tlb = false, other = false;
	uint32_t t = 0;
	for (uint32_t i = 0; i < n; i++) {
		if (f[i].slot != kXeTlbSlot) {
			other = true;
			continue;
		}
		if (!tlb || !xeSeqDone(t, f[i].seqno)) t = f[i].seqno;   // invalidations complete in order: the newest covers all
		tlb = true;
	}
	*target = t;
	if (!tlb) return kXeTlbInringNone;
	if (other) return kXeTlbInringOther;
	if (!t || t >= kXeTlbWordGuard || (word <= t) != ((int32_t)(t - word) >= 0)) return kXeTlbInringWrap;
	return kXeTlbInringWait;
}

// Where such an EXEC goes. knob: debug.intelxe.tlb_inring (0 off, 1 armed queues, 2 any queue); haveWord: the TLB word
// is mapped (the direct-submission page exists); armed: the queue is in direct submission; free: nothing else holds
// the job back (FIFO empty, no TDR, no LR hold, no GT reset or sleep), the conditions under which an EXEC without
// in-fences goes to the ring at once.
enum XeTlbPath : uint32_t {
	kXeTlbPathOld,      // as before: the FIFO and the submit worker (an armed queue leaves direct submission first)
	kXeTlbPathDirect,   // the armed slot, the wait in the job's stub: the queue stays armed
	kXeTlbPathRing,     // the normal ring path, the wait in the ring (knob 2)
};
static inline XeTlbPath xeTlbInringPath(int knob, XeTlbInring plan, bool haveWord, bool armed, bool free) {
	if (knob <= 0 || plan != kXeTlbInringWait || !haveWord || !free) return kXeTlbPathOld;
	if (armed) return kXeTlbPathDirect;
	return knob >= 2 ? kXeTlbPathRing : kXeTlbPathOld;
}

// The wait itself, 5 dwords (xe_mi_commands.h MI_SEMAPHORE_WAIT, XE_INSTR_NUM_DW(5)): MI_SEMW_GGTT, MI_SEMW_POLL,
// COMPARE_OP_SAD_GTE_SDD; data, address low, high, token. The armed direct-submission slot polls its mailbox the same way.
static inline uint32_t xeSemWaitGte(uint32_t *dw, uint32_t n, uint32_t ggtt, uint32_t value) {
	dw[n++] = 0x1cu << 23 | 1u << 22 | 1u << 15 | 1u << 12 | 3;
	dw[n++] = value;
	dw[n++] = ggtt;
	dw[n++] = 0;
	dw[n++] = 0;
	return n;
}
