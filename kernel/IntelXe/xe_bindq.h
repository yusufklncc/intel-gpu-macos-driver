// SPDX-License-Identifier: MIT
// rb8f: the pure parts of asynchronous binds and of bind user fences, so that tests/bindq_test.cpp builds them on the
// host (no GPU, no IOKit):
//  - which way a VM_BIND goes (xeBindPath): Linux applies a bind's page-table update on the CPU at once only when it has
//    no unsignaled in-fence and its bind queue is idle (xe_pt.c xe_pt_vm_dependencies: "if (!job &&
//    !no_in_syncs(...)) return -ETIME; if (!job && !xe_exec_queue_is_idle(q)) return -ETIME"; xe_migrate.c
//    xe_migrate_update_pgtables: -ETIME from the CPU path means "a job is needed", __xe_migrate_update_pgtables then
//    builds a migrate job that the scheduler runs once its dependencies have signaled). Here the "job" is the bind call
//    itself, queued on its bind queue and applied (CPU page-table writes, as before) by a worker when its in-fences
//    have signaled and every earlier call of the same bind queue has been applied;
//  - the out-fence of a queued call (kXeBindSlot): one record per queued call in a ring of kXeBjMax (xeBjAlloc), whose
//    fence has signaled once the call was applied and the bind fence it then produced (clears, invalidation:
//    XeBindFence, rb8d) has signaled. A record is reused only after that, so a fence whose record holds another serial
//    is an old one that signaled;
//  - the order the worker applies queued calls in (xeBindPick): per bind queue in submission order (a drm_sched entity
//    runs its jobs in order; each bind job also depends on the queue's last fence, xe_sched_job_last_fence_add_dep),
//    calls of different bind queues in whatever order their in-fences allow (Linux orders them only where their
//    page-table updates overlap, the VM's range fences; here every apply is one critical section under the client lock);
//  - xe_vm.c check_ufence: an op that removes or splits a VMA (UNMAP, the unmapping half of a REMAP, which a MAP over
//    existing VMAs makes as well; op_lock_and_prep) answers EBUSY while the user fence of the bind that made the VMA
//    (vma_add_ufence) has not signaled; xeUfBusy;
//  - what becomes of a bind user fence still pending when its client closes (xeUfOrphanAction): Linux's
//    user_fence_worker writes it as long as the process's address space has users (mmget_not_zero), else drops it.
#pragma once
#include <stdint.h>
#include "xe_tlbinval.h"

// ─── which way a VM_BIND goes ───────────────────────────────────────────────────────────────────────────────────────
enum XeBindPathKind : uint8_t {
	kXeBindNow,    // applied in the ioctl (no in-fence pending, the bind queue idle)
	kXeBindQueue,  // queued on its bind queue, applied when its in-fences have signaled
	kXeBindWait,   // the pre-rb8f way: the ioctl waits for the in-fences (and the queue's earlier calls), then applies
};
// asyncOn: debug.intelxe.bind_async. inPending: an in-fence has not signaled. queuePending: calls of this bind queue are
// queued and not applied yet. room: a record and the memory for a queued call are there. A call behind queued calls of
// its queue is queued as well whatever the switch says (it may have been turned off while some were queued), so that
// the queue's order holds; without room it waits in the ioctl.
static inline XeBindPathKind xeBindPath(bool asyncOn, bool inPending, uint32_t queuePending, bool room) {
	if (!inPending && !queuePending) return kXeBindNow;
	if (room && (asyncOn || queuePending)) return kXeBindQueue;
	return kXeBindWait;
}

// ─── out-fence records of queued calls ──────────────────────────────────────────────────────────────────────────────
// XeFence {0, kXeBindSlot, tl, serial}: tl names the bind queue (its own id, never 0), so two fences of one queue
// compare like the seqnos of one exec queue (fenceMerge, surfAdd keep the later one: a later call's bind fence folds in
// the earlier calls', xeBindFenceNext), and fences of two queues are kept apart.
static constexpr uint32_t kXeBjMax = 1024;   // queued calls at most (a power of two: serial % kXeBjMax survives the wrap)
struct XeBjRec {
	volatile uint32_t serial;    // the call that owns it, 0 never used
	volatile uint8_t applied;    // its ops are in the page tables (or it was dropped): res is valid
	XeBindFence res;             // the bind fence the apply produced (the queue's last fence after it)
};
static inline XeFence xeBindJobFence(uint32_t tl, uint32_t serial) { return XeFence{0, kXeBindSlot, tl, serial}; }
template <typename Sig>
static inline bool xeBjSignaled(const XeBjRec *tab, const XeFence &f, Sig sig) {
	const XeBjRec &r = tab[f.seqno % kXeBjMax];
	if (r.serial != f.seqno) return true;   // reused: only after the call it held had signaled
	if (!__atomic_load_n(&r.applied, __ATOMIC_ACQUIRE)) return false;
	return sig(r.res.clear) && sig(r.res.tlb);
}
template <typename Sig>
static inline bool xeBjFree(const XeBjRec &r, Sig sig) {
	return !r.serial || (__atomic_load_n(&r.applied, __ATOMIC_ACQUIRE) && sig(r.res.clear) && sig(r.res.tlb));
}
// A record for a new queued call: its serial, or 0 when the ring is full (the record kXeBjMax calls back has not
// signaled yet). *next: the serial counter (0 is skipped).
template <typename Sig>
static inline uint32_t xeBjAlloc(XeBjRec *tab, uint32_t *next, Sig sig) {
	uint32_t s = *next ? *next : 1;
	XeBjRec &r = tab[s % kXeBjMax];
	if (!xeBjFree(r, sig)) return 0;
	__atomic_store_n(&r.applied, 0, __ATOMIC_RELEASE);
	__atomic_store_n(&r.serial, s, __ATOMIC_RELEASE);
	*next = s + 1;
	return s;
}
// The call was applied (or dropped): its bind fence, then the flag readers test first.
static inline void xeBjPublish(XeBjRec *tab, uint32_t serial, const XeBindFence &res) {
	XeBjRec &r = tab[serial % kXeBjMax];
	r.res = res;
	__atomic_store_n(&r.applied, 1, __ATOMIC_RELEASE);
}

// ─── the order queued calls are applied in ──────────────────────────────────────────────────────────────────────────
// The first call, in submission order, that is the oldest of its bind queue and whose in-fences have all signaled.
// N: a list node with `next`; key(n): its bind queue; ready(n): its in-fences have signaled. nullptr: none.
template <typename N, typename Key, typename Ready>
static inline N *xeBindPick(N *head, Key key, Ready ready) {
	for (N *j = head; j; j = j->next) {
		bool oldest = true;
		for (N *k = head; k != j && oldest; k = k->next) oldest = key(k) != key(j);
		if (oldest && ready(j)) return j;
	}
	return nullptr;
}

// Calls of two bind queues of one VM whose ranges overlap are applied in the order they were made: Linux's CPU path
// returns -ETIME while an overlapping update of the VM is pending (xe_pt_vm_dependencies: the VM's range-fence tree,
// rftree, "GPU is busy updating"), and the job it makes instead depends on that update's fence. A queued call is
// therefore an in-fence of a later call of another queue that overlaps it (xeBindSpanOverlap); an UNMAP_ALL (a BO's
// VMAs, wherever they are) and a call's span are compared as the whole address space.
struct XeBindSpan { uint64_t lo, hi; };   // [lo, hi), hi 0: empty
static inline XeBindSpan xeBindOpSpan(bool unmapAll, uint64_t addr, uint64_t range) {
	if (unmapAll) return XeBindSpan{0, ~0ull};
	return XeBindSpan{addr, range ? addr + range : addr};
}
static inline bool xeBindSpanOverlap(const XeBindSpan &a, const XeBindSpan &b) {
	return a.hi > a.lo && b.hi > b.lo && a.lo < b.hi && b.lo < a.hi;
}

// ─── user fences of VMAs (xe_vm.c check_ufence) ─────────────────────────────────────────────────────────────────────
// [a, a + alen) and [b, b + blen) share a byte.
static inline bool xeRangeOverlap(uint64_t a, uint64_t alen, uint64_t b, uint64_t blen) {
	return alen && blen && a < b + blen && b < a + alen;
}
// An op over [addr, addr + range) meets a VMA over [vaddr, vaddr + vrange) whose user fence is pending (ufPending):
// the call answers EBUSY. removes: the op removes or splits what it overlaps (MAP, MAP_USERPTR, UNMAP, UNMAP_ALL;
// PREFETCH does not). byHandle: an UNMAP_ALL, which meets the VMAs of its object (sameObject) whatever their range.
static inline bool xeUfBusy(bool removes, bool byHandle, bool sameObject, uint64_t addr, uint64_t range, uint64_t vaddr,
	uint64_t vrange, bool ufPending) {
	if (!ufPending || !removes) return false;
	return byHandle ? sameObject : xeRangeOverlap(addr, range, vaddr, vrange);
}

// ─── a bind user fence whose client closed ──────────────────────────────────────────────────────────────────────────
// xe_sync.c user_fence_worker: mmget_not_zero(ufence->mm) ? copy_to_user : "ufence wasn't signaled". The mm reference
// (mmgrab at parse) keeps the address space struct, and the write happens while the process still has it. Here the
// page stays wired and mapped into the kernel (or the BO it lies in stays held) until the write; it is written once the
// bind fence has signaled if the process still exists, dropped as soon as the process is gone, and dropped after
// limitNs pending so that a fence that never signals cannot keep a page wired for good.
enum XeUfOrphanKind : uint8_t { kXeUfKeep, kXeUfWrite, kXeUfDropGone, kXeUfDropLate };
static inline XeUfOrphanKind xeUfOrphanAction(bool ready, bool procAlive, uint64_t now, uint64_t orphanNs, uint64_t limitNs) {
	if (!procAlive) return kXeUfDropGone;
	if (ready) return kXeUfWrite;
	return now - orphanNs >= limitNs ? kXeUfDropLate : kXeUfKeep;
}
// The bound: a started job's limit (DRM_XE_JOB_TIMEOUT_MAX, 10 s) plus the TLB fence timeout (xeTlbTimeoutNs, 2.25 s
// with the 4 KB H2G ring), rounded up: every bind fence has signaled or been forced by then.
static constexpr uint64_t kXeUfOrphanNs = 15000000000ull;
