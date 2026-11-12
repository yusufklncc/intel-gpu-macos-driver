// SPDX-License-Identifier: MIT
// Long-running (LR) VMs, DRM_XE_VM_CREATE_FLAG_LR_MODE without FAULT_MODE (docs/design/DESIGN-lr-mode.md): the pure
// part, so that tests/lr_test.cpp builds it on the host. The rest is in xe_core.cpp ("long-running VMs").
//
// Linux (drivers/gpu/drm/xe, dl/linux-xe): xe_vm_in_preempt_fence_mode(vm) = LR and not fault mode. Such a VM's exec
// queues have no job timeout (xe_guc_submit.c guc_exec_queue_init: MAX_SCHEDULE_TIMEOUT for q->vm in LR mode), its
// execs take only user fences as out-syncs (xe_exec.c xe_exec_ioctl -> xe_sync.c xe_sync_entry_parse with
// SYNC_PARSE_FLAG_LR_MODE: a syncobj signal is EOPNOTSUPP; VM_BIND passes the same flag, xe_vm.c xe_vm_bind_ioctl), and
// its memory is protected by preempt fences instead of job fences: every exec queue of the VM has one in the dma-resv
// of the VM and of its external BOs (xe_vm.c xe_vm_add_compute_exec_queue, BOOKKEEP), so whatever waits for the BOs to
// be idle (a move, an eviction, a free) enables the fence, which suspends the queue (xe_preempt_fence.c
// preempt_fence_enable_signaling -> q->ops->suspend: xe_guc_submit.c __guc_exec_queue_process_msg_suspend, a GuC
// schedule disable after the queue ran min_run_period_lr_ms since its last resume, xe_device.c: 5 ms) and signals once
// the GuC confirms (guc_exec_queue_suspend_wait, 5 s, then guc_exec_queue_suspend_timeout_ban). The rebind worker then
// rebinds what moved, waits for those rebinds, and resumes the queues (xe_vm.c preempt_rebind_work_func ->
// resume_and_reinstall_preempt_fences -> q->ops->resume: __guc_exec_queue_process_msg_resume, a schedule enable).
// Here a move holds the VM's queues off itself (it rewrites the PTEs on the CPU, so "rebind" is the move's own remap
// plus its TLB invalidation, waited for before the resume), and a deferred free of an LR VM waits for one preemption
// cycle of the VM's queues instead of their jobs (which may never end).
#pragma once
#include <stdint.h>

// xe_device.c: xe->min_run_period_lr_ms = 5 (vm->preempt.min_run_period_ms of an LR VM without fault mode)
static constexpr uint64_t kXeLrMinRunNs = 5000000ull;
// guc_exec_queue_wait_suspend_done: HZ * 5
static constexpr uint64_t kXeLrConfirmNs = 5000000000ull;

// What one exec queue of an LR VM being preempted needs next.
//   counts:     the queue can still run anything (not banned, not closed, not being taken off by the TDR path)
//   sent:       this preemption sent its schedule disable (sentNs: when)
//   offByOther: a schedule disable sent by someone else (teardown) is in flight or done
//   confirmed:  SCHED_CONTEXT_MODE_DONE (disable) arrived for the queue
//   ringIdle:   every job written to its ring has completed (the hold keeps new ones out of the ring)
//   gucDown:    the queue's GuC cannot take H2G now (GT reset in progress, system sleep)
//   onNs:       when the preemption last resumed it (0: never preempted)
enum XeLrStep : uint8_t {
	kXeLrIsOff,            // nothing of it can run: preempted (confirmed), idle, or not counted
	kXeLrWaitConfirm,      // the disable is out, its confirmation is not in yet
	kXeLrWaitMinRun,       // resumed less than kXeLrMinRunNs ago: the disable waits (xe: xe_sleep_relaxed_ms(wait_ms))
	kXeLrWaitGuc,          // the GuC is not reachable now; tried again later
	kXeLrSendDisable,      // send the schedule disable now
	kXeLrConfirmTimeout,   // not confirmed within kXeLrConfirmNs: ban (guc_exec_queue_suspend_timeout_ban), GT reset
};
struct XeLrQueueState {
	bool counts, sent, offByOther, confirmed, ringIdle, gucDown;
	uint64_t sentNs, onNs;
};
static inline XeLrStep xeLrQueueStep(const XeLrQueueState &s, uint64_t now, uint64_t minRunNs = kXeLrMinRunNs,
	uint64_t confirmNs = kXeLrConfirmNs) {
	if (!s.counts) return kXeLrIsOff;
	if (s.sent || s.offByOther) {
		if (s.confirmed) return kXeLrIsOff;
		return s.sent && now - s.sentNs > confirmNs ? kXeLrConfirmTimeout : kXeLrWaitConfirm;
	}
	// __guc_exec_queue_process_msg_suspend only disables an enabled context; one with nothing in its ring and nothing
	// allowed into it (the hold) touches no memory, so it needs no round trip to the GuC
	if (s.ringIdle) return kXeLrIsOff;
	if (s.gucDown) return kXeLrWaitGuc;
	if (s.onNs && now - s.onNs < minRunNs) return kXeLrWaitMinRun;
	return kXeLrSendDisable;
}

// A deferred free sealed when the VM had asked for cycle `to` may go once the VM completed cycle `done` (wrapping
// counters: xe's preempt fences of one exec queue share a dma_fence context whose seqnos only grow, q->lr.seqno).
static inline bool xeLrCycleCovers(uint32_t done, uint32_t to) { return (int32_t)(done - to) >= 0; }

// The resume of a preempted queue (resume_and_reinstall_preempt_fences -> __guc_exec_queue_process_msg_resume): a
// schedule enable only for a queue whose disable this preemption sent and that can still run; never while another
// hold or a requested cycle is still on the VM (xe: suspend_count, the last resume enables).
static inline bool xeLrResumeNow(uint32_t holds, uint32_t req, uint32_t done) { return !holds && req == done; }
static inline bool xeLrSendEnable(bool sent, bool counts, bool gucDown) { return sent && counts && !gucDown; }

// xe_exec.c xe_exec_ioctl on an LR VM: an exec without batch buffers does nothing at all (no in-fence wait, no
// out-sync: `if (!xe_vm_in_lr_mode(vm))` around xe_sync_in_fence_get / xe_sync_entry_signal), and the job timeout does
// not apply to the queue (xe_exec_queue_is_lr: q->vm in LR mode and not a VM bind queue).
static inline bool xeLrExecNoop(bool lr, uint32_t numBatch) { return lr && !numBatch; }
static inline bool xeLrJobTimeoutApplies(bool lr) { return !lr; }
