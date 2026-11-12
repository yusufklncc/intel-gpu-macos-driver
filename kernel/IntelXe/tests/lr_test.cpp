// SPDX-License-Identifier: MIT
// lr_test: offline check of long-running VMs' pure logic (xe_lr.h, xeVmCreateCheck in xe_uapi_args.cpp), no GPU needed.
//   1. DRM_IOCTL_XE_VM_CREATE flags (xe_vm.c xe_vm_create_ioctl): LR_MODE accepted, FAULT_MODE refused (no fault
//      service: xe's !has_usm answer), the FAULT/LR and NO_VM_OVERCOMMIT/FAULT dependencies, unknown bits, reserved
//   2. the syncs an exec and a bind on an LR VM may carry (xe_sync.c xe_sync_entry_parse, SYNC_PARSE_FLAG_LR_MODE)
//   3. one queue's preemption step (xeLrQueueStep: __guc_exec_queue_process_msg_suspend, min_run_period_lr_ms,
//      guc_exec_queue_wait_suspend_done's 5 s), driven through a whole preemption of a few queues
//   4. cycles (preempt fences' seqnos), resume rules, exec without batch, the job timeout exemption
// c++ -std=c++17 -Wall -Wextra -I.. -o lr_test lr_test.cpp ../xe_uapi_args.cpp && ./lr_test
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include "xe_lr.h"
#include "xe_uapi_args.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static void vmCreate() {
	auto chk = [](uint32_t flags, bool *lr) {
		drm_xe_vm_create v = {};
		v.flags = flags;
		return xeVmCreateCheck(v, lr);
	};
	bool lr = true;
	CHECK(chk(0, &lr) == 0 && !lr, "plain VM");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, &lr) == 0 && !lr, "scratch VM");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE, &lr) == 0 && lr, "LR VM accepted");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, &lr) == 0 && lr, "LR scratch VM");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE, &lr) == EINVAL && !lr,
		"fault mode refused (no GPU fault service)");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_FAULT_MODE, &lr) == EINVAL, "fault mode without LR");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT, &lr) == EINVAL,
		"NO_VM_OVERCOMMIT without fault mode");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE | DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT,
		&lr) == EINVAL, "NO_VM_OVERCOMMIT with fault mode: still fault mode");
	CHECK(chk(1u << 4, &lr) == EINVAL, "unknown flag");
	CHECK(chk(DRM_XE_VM_CREATE_FLAG_LR_MODE | 1u << 31, &lr) == EINVAL && !lr, "unknown flag with LR");
	drm_xe_vm_create v = {};
	v.flags = DRM_XE_VM_CREATE_FLAG_LR_MODE;
	v.reserved[1] = 1;
	CHECK(xeVmCreateCheck(v, &lr) == EINVAL && !lr, "reserved");
	v = {};
	v.extensions = 0x1000;
	CHECK(xeVmCreateCheck(v, &lr) == EINVAL, "extensions");
}

static void syncs() {
	auto mk = [](uint32_t type, uint32_t flags, uint64_t addr, uint64_t point) {
		drm_xe_sync e = {};
		e.type = type;
		e.flags = flags;
		e.addr = addr;
		e.timeline_value = point;
		return e;
	};
	const uint32_t SIG = DRM_XE_SYNC_FLAG_SIGNAL, EXEC = kXeSyncParseExec, LR = kXeSyncParseLrMode;
	// exec on an LR VM: what xe_exec_ioctl passes (SYNC_PARSE_FLAG_EXEC | SYNC_PARSE_FLAG_LR_MODE)
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, SIG, 3, 0), EXEC | LR) == EOPNOTSUPP, "exec: syncobj out");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, SIG, 3, 4), EXEC | LR) == EOPNOTSUPP,
		"exec: timeline out");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, 0, 3, 0), EXEC | LR) == 0, "exec: syncobj in");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, 0, 3, 4), EXEC | LR) == 0, "exec: timeline in");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x2000, 1), EXEC | LR) == 0, "exec: user fence");
	// VM_BIND on an LR VM: xe_vm_bind_ioctl passes SYNC_PARSE_FLAG_LR_MODE too
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, SIG, 3, 0), LR) == EOPNOTSUPP, "bind: syncobj out");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x2000, 1), LR) == 0, "bind: user fence");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x2000, 1), LR | kXeSyncParseDisallowUfence) ==
		EOPNOTSUPP, "bind without ops: no user fence");
	// the same entries on a dma-fence VM are fine
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, SIG, 3, 0), EXEC) == 0, "dma-fence VM: syncobj out");
}

static XeLrQueueState q0() {
	XeLrQueueState s = {};
	s.counts = true;
	return s;
}

static void step() {
	const uint64_t T = 10000000000ull;   // "now"
	XeLrQueueState s = q0();
	s.ringIdle = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrIsOff, "idle ring: off without a disable");
	s = q0();
	CHECK(xeLrQueueStep(s, T) == kXeLrSendDisable, "busy, never preempted: disable now");
	s.onNs = T - 1000000;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitMinRun, "resumed 1 ms ago: waits for the 5 ms run period");
	s.onNs = T - kXeLrMinRunNs;
	CHECK(xeLrQueueStep(s, T) == kXeLrSendDisable, "resumed 5 ms ago: disable");
	s = q0();
	s.gucDown = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitGuc, "GuC down: nothing sent");
	s.ringIdle = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrIsOff, "GuC down but idle: off");
	s = q0();
	s.sent = true;
	s.sentNs = T - 1000;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitConfirm, "disable out: waits for the confirmation");
	s.ringIdle = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitConfirm, "a disable sent is waited for even if the ring went idle");
	s.confirmed = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrIsOff, "confirmed: off");
	s = q0();
	s.sent = true;
	s.sentNs = T - kXeLrConfirmNs - 1;
	CHECK(xeLrQueueStep(s, T) == kXeLrConfirmTimeout, "not confirmed in 5 s: ban");
	s.sentNs = T - kXeLrConfirmNs;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitConfirm, "exactly 5 s: still waits");
	s = q0();
	s.offByOther = true;
	s.sentNs = 0;
	CHECK(xeLrQueueStep(s, T) == kXeLrWaitConfirm, "teardown's disable: waited, never timed out here");
	s.confirmed = true;
	CHECK(xeLrQueueStep(s, T) == kXeLrIsOff, "teardown's disable confirmed: off");
	s = q0();
	s.counts = false;
	s.sent = true;
	s.sentNs = 0;
	CHECK(xeLrQueueStep(s, T) == kXeLrIsOff, "banned or closed: does not count");

	// a whole preemption of three queues: busy, idle, busy and resumed 2 ms ago; the GuC confirms one per tick
	struct Q { XeLrQueueState s; bool confirmAt; uint64_t sentAt; };
	Q qs[3] = {{q0(), false, 0}, {q0(), false, 0}, {q0(), false, 0}};
	qs[1].s.ringIdle = true;
	qs[2].s.onNs = T - 2000000;
	uint32_t disables = 0, ticks = 0;
	bool allOff = false;
	for (uint64_t now = T; ticks < 20 && !allOff; now += 1000000, ticks++) {   // a 1 ms tick
		allOff = true;
		for (Q &q : qs) {
			if (q.s.sent && now - q.sentAt >= 2000000) q.s.confirmed = true;   // the GuC answers in 2 ms
			const XeLrStep st = xeLrQueueStep(q.s, now);
			if (st == kXeLrSendDisable) {
				q.s.sent = true;
				q.s.sentNs = q.sentAt = now;
				disables++;
			}
			allOff = (st == kXeLrIsOff) && allOff;
		}
	}
	CHECK(allOff, "the preemption completes");
	CHECK(disables == 2, "two disables (the idle queue needs none): %u", disables);
	CHECK(ticks == 6, "done after the third queue's run period (3 ms) and its confirmation (2 ms): %u ticks", ticks);
}

static void cycles() {
	CHECK(xeLrCycleCovers(5, 5) && xeLrCycleCovers(6, 5) && !xeLrCycleCovers(4, 5), "cycle order");
	CHECK(xeLrCycleCovers(2, 0xfffffffeu) && !xeLrCycleCovers(0xfffffffeu, 2), "cycle counters wrap");
	CHECK(xeLrResumeNow(0, 7, 7), "no hold, no cycle pending: resume");
	CHECK(!xeLrResumeNow(1, 7, 7), "a move still holds the VM");
	CHECK(!xeLrResumeNow(0, 8, 7), "a cycle asked for meanwhile");
	CHECK(xeLrSendEnable(true, true, false), "this preemption's disable: enable");
	CHECK(!xeLrSendEnable(false, true, false), "never disabled (idle): no enable");
	CHECK(!xeLrSendEnable(true, false, false), "banned meanwhile: stays off");
	CHECK(!xeLrSendEnable(true, true, true), "GuC down: the registration enables it");
	CHECK(xeLrExecNoop(true, 0) && !xeLrExecNoop(true, 1) && !xeLrExecNoop(false, 0), "LR exec without batch: nothing");
	CHECK(!xeLrJobTimeoutApplies(true) && xeLrJobTimeoutApplies(false), "no job timeout on LR queues");
}

int main() {
	vmCreate();
	syncs();
	step();
	cycles();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
