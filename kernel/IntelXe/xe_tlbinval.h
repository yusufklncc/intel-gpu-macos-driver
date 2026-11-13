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

// ─── rb8d: fences, and a VM_BIND's out-fence ────────────────────────────────────────────────────────────────────────
// A fence is one seqno on one in-order timeline: an exec queue's seqno slot (slot, generation), or the TLB invalidation
// sequence (slot kXeTlbSlot, seqno = the GuC TLB_INVALIDATION seqno: done once every GuC it went to has answered
// TLB_INVALIDATION_DONE up to it, which they do in order, as xe_tlb_inval_done_handler completes xe_tlb_inval fences in
// seqno order). slot kXeFenceDone: signaled. point: its timeline point while it sits in a timeline syncobj.
struct XeFence { uint64_t point; uint32_t slot, gen, seqno; };
// rb8f: kXeBindSlot, a queued VM_BIND's out-fence (xe_bindq.h: seqno = the call's serial, gen = its bind queue)
static constexpr uint32_t kXeFenceDone = ~0u, kXeTlbSlot = ~1u, kXeBindSlot = ~2u;
static inline XeFence xeTlbFence(uint32_t seq) { return XeFence{0, kXeTlbSlot, 0, seq}; }
static inline bool xeSeqDone(uint32_t done, uint32_t seq) { return (int32_t)(done - seq) >= 0; }

// What a VM_BIND's out-fences stand for: the copy-engine clears of the BOs it bound, and the TLB invalidation of what it
// unbound. xe_pt.c xe_pt_update_ops_run: "tlb invalidation must be done before signaling unbind/rebind": when an update
// needs an invalidation the fence it returns (and xe_vm.c vm_bind_ioctl_ops_fini signals into every out-sync and makes
// the bind queue's last fence) is the invalidation job's fence, which runs after the update job (xe_tlb_inval_job_push
// with the update's fence as dependency); the update job itself waits for the BOs' kernel fences (their clears). Before
// rb8d an unbind's out-fence here was the clear alone and signaled while the GPU could still translate through the
// entries the unbind removed. The fences of one bind queue signal in order (each bind job depends on the queue's last
// fence, xe_sched_job_last_fence_add_dep), so a bind's fence also covers the binds before it: next() folds the
// previous out-fence of the same bind queue in. rb8e: that is the queue the call names (exec_queue_id of a
// DRM_XE_ENGINE_CLASS_VM_BIND queue), else the VM's default bind queue (xe_vm.c to_wait_exec_queue: q ?: vm->q[0];
// xe_exec_queue.c xe_exec_queue_last_fence_get / _set keep one last fence per exec queue): xeBindLast. WAS the VM's
// for every call, so a bind on one queue waited for another queue's invalidation.
struct XeBindFence { XeFence clear, tlb; };
// sig(fence): signaled now. clear: the latest clear among the BOs this call bound; unbound: the call removed or replaced
// entries and issued invalidation tlbSeq after its page-table writes. The clears all run on the kernel's migrate queue
// (one timeline, XeVm::kern), so the later of two stands for both; invalidation seqnos only grow.
template <typename Sig>
static inline XeBindFence xeBindFenceNext(const XeBindFence &prev, const XeFence &clear, bool unbound, uint32_t tlbSeq, Sig sig) {
	const XeFence done = {0, kXeFenceDone, 0, 0};
	XeBindFence n;
	const bool pc = sig(prev.clear), cc = sig(clear);
	if (cc) n.clear = pc ? done : prev.clear;
	else if (pc || prev.clear.slot != clear.slot || prev.clear.gen != clear.gen) n.clear = clear;
	else n.clear = xeSeqDone(clear.seqno, prev.clear.seqno) ? clear : prev.clear;
	if (unbound) n.tlb = xeTlbFence(tlbSeq);
	else n.tlb = sig(prev.tlb) ? done : prev.tlb;
	return n;
}
// debug.intelxe.bind_tlb_fence (rb8cost, DESIGN-rb8-cost.md): 1 (the default) keeps the invalidation in the out-fence
// above; 0 drops it (rb8b) and is honoured only when the kext was booted with -intelxe_unsafe_bindfence. Measured on the
// B580 (night of 13 Nov, Geekbench 6 OpenCL import, 3 runs each way): no speed difference, and its first run with 0 had
// 3 GuC context resets and a GT reset, so a plain root write must not be able to turn it off. Only 0 and 1 are taken.
enum XeKnobWrite : uint32_t { kXeKnobTaken, kXeKnobRange, kXeKnobLocked };
static inline XeKnobWrite xeBindTlbFenceWrite(int v, bool unlocked) {
	if (v != 0 && v != 1) return kXeKnobRange;
	if (!v && !unlocked) return kXeKnobLocked;
	return kXeKnobTaken;
}

// The fences an out-sync of the bind gets: the parts not signaled yet (a binary syncobj holds both, as a sync file's
// several; a timeline point gets both at the same point, reached when both are), or one signaled fence.
template <typename Sig>
static inline uint32_t xeBindFenceParts(const XeBindFence &f, XeFence out[2], Sig sig) {
	uint32_t n = 0;
	if (!sig(f.clear)) out[n++] = f.clear;
	if (!sig(f.tlb)) out[n++] = f.tlb;
	if (!n) out[n++] = XeFence{0, kXeFenceDone, 0, 0};
	return n;
}

// The invalidations the TLB fence watchdog completes without an answer (xe_tlb_inval.c xe_tlb_inval_fence_timeout: a
// fence pending longer than the timeout is signaled with -ETIME, and the done seqno moves past it): every seqno from
// the oldest pending (done + 1) up to issued whose issue time (a ring of the last 64, older ones read as the newest
// that overwrote them, which only delays) is at least timeoutNs old. Returns the new done value (done: none).
static inline uint32_t xeTlbTimedOut(uint32_t done, uint32_t issued, const uint64_t issueNs[64], uint64_t now, uint64_t timeoutNs) {
	uint32_t s = done;
	while (s != issued && (int64_t)(now - issueNs[(s + 1) & 63]) >= (int64_t)timeoutNs) s++;
	return s;
}

// rb8e: the TLB invalidation fence timeout. xe_guc_tlb_inval.c tlb_inval_timeout_delay: hw_tlb_timeout = HZ / 4 ("what
// HW/GuC needs to process TLB inv request") plus twice xe_guc_ct_queue_proc_time_jiffies ("the actual delay caused by
// the CTB transport"), which xe_guc_ct.c computes as (CTB_H2G_BUFFER_SIZE / SZ_4K) * HZ: one second per 4 KB of H2G
// ring ("a 4KiB buffer full of commands takes a little over a second to process"). xe_tlb_inval.c
// xe_tlb_inval_fence_timeout signals a fence once msecs_to_jiffies(now - inval_time) >= that delay. With Linux's 4 KB
// H2G ring (IntelXe's is the same size) 250 ms + 2 s = 2.25 s. WAS a fixed 1 s.
static constexpr uint64_t xeTlbTimeoutNs(uint32_t h2gBytes) {
	return 250000000ull + 2ull * (h2gBytes / 4096u) * 1000000000ull;
}

// rb8e: the bind queue whose last fence a VM_BIND folds in and replaces (xe_vm.c to_wait_exec_queue): the named bind
// queue's, or with exec_queue_id 0 the VM's default queue's.
static inline XeBindFence *xeBindLast(XeBindFence *namedQueue, XeBindFence *vmDefault) {
	return namedQueue ? namedQueue : vmDefault;
}

// rb8d: the operation dword of a GuC TLB_INVALIDATION action (xe_guc_tlb_inval.c MAKE_INVAL_OP: the type at
// XE_GUC_TLB_INVAL_TYPE_SHIFT 0, XE_GUC_TLB_INVAL_MODE_HEAVY (0) at XE_GUC_TLB_INVAL_MODE_SHIFT 8, and
// XE_GUC_TLB_INVAL_FLUSH_CACHE, bit 31). Types (abi/guc_actions_abi.h): FULL 0, PAGE_SELECTIVE 1, GUC 3 (the GGTT,
// send_tlb_inval_ggtt).
enum : uint32_t { kXeTlbInvalFull = 0, kXeTlbInvalPageSelective = 1, kXeTlbInvalGuc = 3 };
static inline uint32_t xeTlbInvalOp(uint32_t type) { return type << 0 | 0u << 8 | 1u << 31; }

// rb8d: a timeline syncobj's value (drm_syncobj_timeline: the highest point reached, i.e. it and every point before it
// have signaled), from its entries in the order they were added. A point that holds several fences (a bind's clear and
// invalidation, a transferred point of several queues) is reached once all of them are: the signaled entries at the
// point of the first unsignaled one do not count. WAS the last signaled entry before the first unsignaled one, which
// reported a two-fence point reached as soon as its first fence had signaled. *counted: how many leading entries
// count (signaled history; the caller keeps the last of them, carrying the value, and may drop the rest).
template <typename Sig>
static inline uint64_t xeTimelineValue(const XeFence *f, uint32_t n, Sig sig, uint32_t *counted) {
	uint32_t i = 0;
	while (i < n && sig(f[i])) i++;
	uint32_t last = i;
	while (last && i < n && f[last - 1].point >= f[i].point) last--;
	uint64_t v = 0;
	for (uint32_t k = 0; k < last; k++)
		if (f[k].point > v) v = f[k].point;
	*counted = last;
	return v;
}
