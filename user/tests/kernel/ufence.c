// SPDX-License-Identifier: MIT
// ufence: user fences through the kext, as Linux xe has them (xe_sync.c, xe_wait_user_fence.c, xe_ring_ops.c), the way
// Intel's compute runtime uses them: DRM_XE_SYNC_TYPE_USER_FENCE as the signal of an exec (the job's ring stores the
// 64-bit value at a GPU address after the batch) and of a VM_BIND (the kernel writes it at a CPU address once the bind
// is done), and DRM_IOCTL_XE_WAIT_USER_FENCE (sleep until *addr OP value under a mask, timeout, exec queue).
// No Mesa, no Metal plugin. Checks, in order:
//   1. argument errors: exec/bind sync parsing (wait-only fence, unaligned, two in one bind, none allowed without binds,
//      unreadable bind pointer), WAIT_USER_FENCE (unaligned, bad op/flags/pad, unknown queue, unreadable address)
//   2. exec user fence on every engine the device reports (render, copy, compute, video): written, wait returns 0
//   3. a wait that really sleeps: the batch blocks on a semaphore; a 50 ms wait times out (time left 0), the CPU
//      releases the batch, a 2 s wait returns as the interrupt arrives (latency printed)
//   4. every OP with a passing and a failing value, and the mask (u8/u16/u32/u64 views of one fence)
//   5. timeouts: relative (time left), absolute (field intact), negative = forever (met at once)
//   6. bind user fences: MAP and UNMAP write their value before the ioctl returns
//   7. Linux's quirks kept: an exec's last user fence wins; an exec without batch writes none
//   8. a wait bound to an exec queue returns EIO when that queue is destroyed meanwhile
//   9. back-to-back compute execs, half of them with a user fence (run once with sysctl debug.intelxe.direct=1 too:
//      jobs with a fence leave direct mode, the others use the armed slot)
//  10. an exec fence at an unmapped GPU address of a SCRATCH_PAGE VM: the write is dropped (wait times out), the queue
//      is not banned
// Then the "user fences" line of debug.intelxe.stats. Exit status 0 when every check passed.
//   clang -arch x86_64 -O2 -I../../../kernel/IntelXe/uapi -o ufence ufence.c -framework IOKit
//   ./ufence
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <immintrin.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>
#include "xe_drm.h"

static io_connect_t conn;
static int xioctl(unsigned long req, void *arg) {
	uint64_t in[2] = {req, (uint64_t)(uintptr_t)arg}, out = 0;
	uint32_t n = 1;
	if (IOConnectCallScalarMethod(conn, 0, in, 2, &out, &n)) return -1;
	errno = (int)out;
	return out ? -1 : 0;
}
// the errno of one ioctl (0 = success)
static int xerr(unsigned long req, void *arg) { return xioctl(req, arg) ? errno : 0; }
static mach_timebase_info_data_t tb;
static uint64_t ns(void) { return mach_absolute_time() * tb.numer / tb.denom; }   // the kext's clock (ABSTIME)
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint32_t gem(uint64_t size) {
	struct drm_xe_gem_create g = {.size = size, .placement = 1, .cpu_caching = DRM_XE_GEM_CPU_CACHING_WB};
	if (xioctl(DRM_IOCTL_XE_GEM_CREATE, &g)) { fprintf(stderr, "GEM_CREATE %d\n", errno); exit(1); }
	return g.handle;
}
static void *map(uint32_t handle, uint64_t size) {
	struct drm_xe_gem_mmap_offset m = {.handle = handle};
	if (xioctl(DRM_IOCTL_XE_GEM_MMAP_OFFSET, &m)) return NULL;
	mach_vm_address_t addr = 0; mach_vm_size_t sz = size;
	if (IOConnectMapMemory64(conn, (uint32_t)(m.offset >> 12), mach_task_self(), &addr, &sz, kIOMapAnywhere)) return NULL;
	return (void *)(uintptr_t)addr;
}
static int bindOp(uint32_t vm, uint32_t op, uint32_t bo, uint64_t addr, uint64_t range, struct drm_xe_sync *s, uint32_t ns_) {
	struct drm_xe_vm_bind b = {.vm_id = vm, .num_binds = 1, .num_syncs = ns_, .syncs = (uint64_t)(uintptr_t)s};
	b.bind.obj = bo; b.bind.range = range; b.bind.addr = addr; b.bind.op = op; b.bind.pat_index = 2;
	return xerr(DRM_IOCTL_XE_VM_BIND, &b);
}
static void flush(volatile void *p, size_t len) {   // the CPU's writes out to memory before the GPU reads them
	for (size_t o = 0; o < len; o += 64) _mm_clflush((const char *)p + o);
	_mm_mfence();
}
static uint32_t queueNew(uint32_t vm, uint16_t cls, uint16_t inst, uint16_t gt) {
	struct drm_xe_engine_class_instance ei = {.engine_class = cls, .engine_instance = inst, .gt_id = gt};
	struct drm_xe_exec_queue_create q = {.width = 1, .num_placements = 1, .vm_id = vm, .instances = (uint64_t)(uintptr_t)&ei};
	return xioctl(DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &q) ? 0 : q.exec_queue_id;
}
static struct drm_xe_sync ufSync(uint64_t addr, uint64_t value) {
	struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .addr = addr,
		.timeline_value = value};
	return s;
}
static int execS(uint32_t q, uint64_t batch, struct drm_xe_sync *s, uint32_t n) {
	struct drm_xe_exec x = {.exec_queue_id = q, .num_syncs = n, .syncs = (uint64_t)(uintptr_t)s, .address = batch,
		.num_batch_buffer = batch ? 1 : 0};
	return xerr(DRM_IOCTL_XE_EXEC, &x);
}
// WAIT_USER_FENCE; *left: the timeout field afterwards
static int waitUf(volatile void *addr, uint16_t op, uint64_t value, uint64_t mask, int64_t timeout, uint16_t flags, uint32_t q,
	int64_t *left) {
	struct drm_xe_wait_user_fence w = {.addr = (uint64_t)(uintptr_t)addr, .op = op, .flags = flags, .value = value, .mask = mask,
		.timeout = timeout, .exec_queue_id = q};
	const int e = xerr(DRM_IOCTL_XE_WAIT_USER_FENCE, &w);
	if (left) *left = w.timeout;
	return e;
}
static int syncWait(uint32_t h) {
	struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&h, .count_handles = 1, .timeout_nsec = (int64_t)(ns() + 2000000000ull)};
	return xerr(DRM_IOCTL_SYNCOBJ_WAIT, &w);
}

#define MI_BBE (0x0au << 23)                                       // MI_BATCH_BUFFER_END
#define MI_SEMA_GTE_POLL (0x1cu << 23 | 1u << 15 | 1u << 12 | 3)   // MI_SEMAPHORE_WAIT, polling, SAD >= SDD, PPGTT
enum { BATCH = 0x100000, FENCE = 0x200000, MBOX = 0x300000, UNMAPPED = 0x7000000 };
static const uint64_t M64 = ~0ull;

struct destroyArg { uint32_t q; };
static void *destroyLater(void *p) {
	usleep(100000);
	struct drm_xe_exec_queue_destroy d = {.exec_queue_id = ((struct destroyArg *)p)->q};
	xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d);
	return NULL;
}

int main(int argc, char **argv) {
	(void)argc; (void)argv;
	mach_timebase_info(&tb);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) { fprintf(stderr, "no IntelXe service\n"); return 1; }
	struct drm_xe_vm_create vc = {0};
	if (xioctl(DRM_IOCTL_XE_VM_CREATE, &vc)) { fprintf(stderr, "vm create %d\n", errno); return 1; }
	const uint32_t vm = vc.vm_id;
	const uint32_t batchBo = gem(0x10000), fenceBo = gem(0x10000), mboxBo = gem(0x1000);
	volatile uint32_t *batch = map(batchBo, 0x10000), *mbox = map(mboxBo, 0x1000);
	volatile uint64_t *fence = map(fenceBo, 0x10000);   // fence[i] at GPU FENCE + 8 i
	if (!batch || !fence || !mbox) { fprintf(stderr, "map\n"); return 1; }
	if (bindOp(vm, DRM_XE_VM_BIND_OP_MAP, batchBo, BATCH, 0x10000, NULL, 0) || bindOp(vm, DRM_XE_VM_BIND_OP_MAP, fenceBo, FENCE, 0x10000, NULL, 0) ||
		bindOp(vm, DRM_XE_VM_BIND_OP_MAP, mboxBo, MBOX, 0x1000, NULL, 0)) { fprintf(stderr, "bind %d\n", errno); return 1; }
	// batch 0: MI_BATCH_BUFFER_END; batch at +0x100: wait for mailbox >= 1, then end
	batch[0] = MI_BBE; batch[1] = 0;
	volatile uint32_t *sb = batch + 0x40;
	sb[0] = MI_SEMA_GTE_POLL; sb[1] = 1; sb[2] = MBOX; sb[3] = 0; sb[4] = 0; sb[5] = MI_BBE;
	mbox[0] = 0;
	memset((void *)fence, 0, 0x1000);
	flush(batch, 0x200); flush(mbox, 64); flush(fence, 0x1000);
	const uint32_t rq = queueNew(vm, DRM_XE_ENGINE_CLASS_RENDER, 0, 0);
	if (!rq) { fprintf(stderr, "render queue %d\n", errno); return 1; }
	int64_t left;

	printf("1. argument errors\n");
	{
		struct drm_xe_sync s = ufSync(FENCE, 1);
		s.flags = 0;
		CHECK(execS(rq, BATCH, &s, 1) == EOPNOTSUPP, "exec: a user fence as a wait: %d, want EOPNOTSUPP", execS(rq, BATCH, &s, 1));
		s = ufSync(FENCE + 4, 1);
		CHECK(execS(rq, BATCH, &s, 1) == EINVAL, "exec: unaligned user fence");
		uint64_t mem = 0;
		struct drm_xe_sync two[2] = {ufSync((uint64_t)(uintptr_t)&mem, 1), ufSync((uint64_t)(uintptr_t)&mem, 2)};
		CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, batchBo, 0x400000, 0x10000, two, 2) == EINVAL, "bind: two user fences");
		s = ufSync(0x8, 1);   // unreadable user pointer (user_fence_create's get_user)
		CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, batchBo, 0x400000, 0x10000, &s, 1) == EFAULT, "bind: unreadable user fence");
		s = ufSync((uint64_t)(uintptr_t)&mem + 4, 1);
		CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, batchBo, 0x400000, 0x10000, &s, 1) == EINVAL, "bind: unaligned user fence");
		s = ufSync((uint64_t)(uintptr_t)&mem, 1);
		struct drm_xe_vm_bind b = {.vm_id = vm, .num_binds = 0, .num_syncs = 1, .syncs = (uint64_t)(uintptr_t)&s};
		CHECK(xerr(DRM_IOCTL_XE_VM_BIND, &b) == EOPNOTSUPP, "bind without binds: user fence disallowed");
		CHECK(mem == 0, "no failed bind wrote its user fence (0x%llx)", (unsigned long long)mem);
		CHECK(waitUf((char *)fence + 4, 0, 0, M64, 0, 0, 0, NULL) == EINVAL, "wait: unaligned");
		CHECK(waitUf(fence, 6, 0, M64, 0, 0, 0, NULL) == EINVAL, "wait: op 6");
		CHECK(waitUf(fence, 0, 0, M64, 0, 2, 0, NULL) == EINVAL, "wait: unknown flag");
		CHECK(waitUf(fence, 0, 0, M64, 0, 0, 999, NULL) == ENOENT, "wait: unknown exec queue");
		CHECK(waitUf((void *)0x10, 0, 0, M64, 0, 0, 0, NULL) == EFAULT, "wait: unreadable address");
		struct drm_xe_wait_user_fence w = {.addr = (uint64_t)(uintptr_t)fence, .pad = 1};
		CHECK(xerr(DRM_IOCTL_XE_WAIT_USER_FENCE, &w) == EINVAL, "wait: pad");
	}

	printf("2. exec user fence on every engine\n");
	{
		struct drm_xe_device_query dq = {.query = DRM_XE_DEVICE_QUERY_ENGINES};
		xioctl(DRM_IOCTL_XE_DEVICE_QUERY, &dq);
		struct drm_xe_query_engines *qe = calloc(1, dq.size);
		dq.data = (uint64_t)(uintptr_t)qe;
		if (xioctl(DRM_IOCTL_XE_DEVICE_QUERY, &dq)) { fprintf(stderr, "engine query %d\n", errno); return 1; }
		static const char *const names[] = {"render", "copy", "video", "video-enh", "compute"};
		for (uint32_t i = 0; i < qe->num_engines; i++) {
			const struct drm_xe_engine_class_instance *e = &qe->engines[i].instance;
			const uint32_t q = queueNew(vm, e->engine_class, e->engine_instance, e->gt_id);
			const char *nm = e->engine_class < 5 ? names[e->engine_class] : "?";
			CHECK(q, "%s%u: queue %d", nm, e->engine_instance, errno);
			if (!q) continue;
			const uint64_t v = 0xabc0000000000000ull | (uint64_t)i << 32 | 0x5eed;
			volatile uint64_t *f = fence + 16 + i;
			struct drm_xe_sync s = ufSync(FENCE + (16 + i) * 8, v);
			const uint64_t t0 = ns();
			CHECK(execS(q, BATCH, &s, 1) == 0, "%s%u: exec %d", nm, e->engine_instance, errno);
			const int r = waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, v, M64, 2000000000, 0, q, &left);
			const uint64_t t1 = ns();
			CHECK(r == 0 && *f == v, "%s%u (gt %u): wait %d, fence 0x%llx want 0x%llx", nm, e->engine_instance, e->gt_id, r,
				(unsigned long long)*f, (unsigned long long)v);
			if (!r) printf("  %s%u (gt %u): written, exec -> wait return %.1f us\n", nm, e->engine_instance, e->gt_id, (t1 - t0) / 1e3);
			struct drm_xe_exec_queue_destroy d = {.exec_queue_id = q};
			xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d);
		}
		free(qe);
	}

	printf("3. a wait that sleeps until the interrupt\n");
	{
		volatile uint64_t *f = fence + 1;
		struct drm_xe_sync s = ufSync(FENCE + 8, 77);
		CHECK(execS(rq, BATCH + 0x100, &s, 1) == 0, "exec (semaphore batch) %d", errno);
		uint64_t t0 = ns();
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 77, M64, 50000000, 0, rq, &left) == ETIME, "50 ms wait on a blocked job: ETIME");
		const uint64_t waited = ns() - t0;
		CHECK(waited >= 49000000 && waited < 500000000, "50 ms wait took %.1f ms", waited / 1e6);
		CHECK(left == 0, "time left after a timeout: %lld, want 0", (long long)left);
		CHECK(*f == 0, "fence written while the batch was blocked");
		mbox[0] = 1;
		flush(mbox, 64);
		t0 = ns();
		const int r = waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 77, M64, 2000000000, 0, rq, &left);
		const uint64_t t1 = ns();
		CHECK(r == 0 && *f == 77, "after the release: wait %d, fence %llu", r, (unsigned long long)*f);
		CHECK(left > 0 && left < 2000000000, "time left %lld", (long long)left);
		printf("  release -> wait return %.1f us, %lld ns of 2 s left\n", (t1 - t0) / 1e3, (long long)left);
	}

	printf("4. every op, and the mask\n");
	{
		volatile uint64_t *f = fence + 2;
		const uint64_t V = 0x1122334455667788ull;
		struct drm_xe_sync s = ufSync(FENCE + 16, V);
		CHECK(execS(rq, BATCH, &s, 1) == 0, "exec %d", errno);
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, V, M64, 2000000000, 0, 0, NULL) == 0, "fence 2 written");
		struct { uint16_t op; uint64_t pass, fail; const char *nm; } ops[] = {
			{DRM_XE_UFENCE_WAIT_OP_EQ, V, V + 1, "EQ"}, {DRM_XE_UFENCE_WAIT_OP_NEQ, V + 1, V, "NEQ"},
			{DRM_XE_UFENCE_WAIT_OP_GT, V - 1, V, "GT"}, {DRM_XE_UFENCE_WAIT_OP_GTE, V, V + 1, "GTE"},
			{DRM_XE_UFENCE_WAIT_OP_LT, V + 1, V, "LT"}, {DRM_XE_UFENCE_WAIT_OP_LTE, V, V - 1, "LTE"}};
		for (unsigned i = 0; i < 6; i++) {
			CHECK(waitUf(f, ops[i].op, ops[i].pass, M64, 0, 0, 0, NULL) == 0, "%s passing value", ops[i].nm);
			CHECK(waitUf(f, ops[i].op, ops[i].fail, M64, 0, 0, 0, NULL) == ETIME, "%s failing value: ETIME at once", ops[i].nm);
		}
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 0x88, 0xff, 0, 0, 0, NULL) == 0, "u8 view");
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 0xffff7788, 0xffff, 0, 0, 0, NULL) == 0, "u16 view ignores the high bits");
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 0x55667788, 0xffffffffu, 0, 0, 0, NULL) == 0, "u32 view");
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_EQ, 0x55667788, M64, 0, 0, 0, NULL) == ETIME, "u64 view sees the high bits");
		CHECK(waitUf(f, DRM_XE_UFENCE_WAIT_OP_GT, 0x55667787, 0xffffffffu, 0, 0, 0, NULL) == 0, "u32 GT");
	}

	printf("5. timeouts\n");
	{
		volatile uint64_t *never = fence + 3;   // nobody writes it
		uint64_t t0 = ns();
		CHECK(waitUf(never, DRM_XE_UFENCE_WAIT_OP_EQ, 1, M64, 20000000, 0, 0, &left) == ETIME, "relative 20 ms: ETIME");
		CHECK(ns() - t0 >= 19000000, "relative 20 ms waited %.1f ms", (ns() - t0) / 1e6);
		CHECK(left == 0, "relative: 0 left");
		const int64_t abs = (int64_t)(ns() + 20000000);
		t0 = ns();
		CHECK(waitUf(never, DRM_XE_UFENCE_WAIT_OP_EQ, 1, M64, abs, DRM_XE_UFENCE_WAIT_FLAG_ABSTIME, 0, &left) == ETIME,
			"absolute +20 ms: ETIME");
		CHECK(ns() - t0 >= 15000000, "absolute waited %.1f ms", (ns() - t0) / 1e6);
		CHECK(left == abs, "absolute: the field stays (%lld vs %lld)", (long long)left, (long long)abs);
		CHECK(waitUf(never, DRM_XE_UFENCE_WAIT_OP_EQ, 0, M64, -1, 0, 0, &left) == 0, "negative timeout, met at once");
		CHECK(left > INT64_MAX - 1000000000LL, "forever: MAX_SCHEDULE_TIMEOUT less the time waited (%lld)", (long long)left);
		CHECK(waitUf(never, DRM_XE_UFENCE_WAIT_OP_EQ, 1, M64, 0, 0, 0, &left) == ETIME, "timeout 0: one look");
	}

	printf("6. bind user fences\n");
	{
		volatile uint64_t cpuFence = 0;
		struct drm_xe_sync s = ufSync((uint64_t)(uintptr_t)&cpuFence, 0xb1d);
		CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, batchBo, 0x400000, 0x10000, &s, 1) == 0, "MAP with a user fence %d", errno);
		CHECK(cpuFence == 0xb1d, "MAP: written before the ioctl returned (0x%llx)", (unsigned long long)cpuFence);
		CHECK(waitUf(&cpuFence, DRM_XE_UFENCE_WAIT_OP_EQ, 0xb1d, M64, 0, 0, 0, NULL) == 0, "MAP: wait met");
		s = ufSync(FENCE, 0);
		s.addr = (uint64_t)(uintptr_t)(fence + 4);   // the fence BO's CPU mapping as the bind's user pointer
		s.timeline_value = 0xdead;
		CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_UNMAP, 0, 0x400000, 0x10000, &s, 1) == 0, "UNMAP with a user fence %d", errno);
		// a bind's user fence is written when the bind is done, as xe does it (since rb8c the bind waits for its TLB
		// invalidation, so the write comes about 0.25 ms after the ioctl returns): wait for it, bounded
		CHECK(waitUf((volatile uint64_t *)(fence + 4), DRM_XE_UFENCE_WAIT_OP_EQ, 0xdead, M64, 2000000000, 0, 0, NULL) == 0 &&
		      fence[4] == 0xdead, "UNMAP: written into the mapped BO (0x%llx)", (unsigned long long)fence[4]);
		uint32_t vramBo;
		struct drm_xe_gem_create g = {.size = 0x200000, .placement = 2, .cpu_caching = DRM_XE_GEM_CPU_CACHING_WC};
		if (!xioctl(DRM_IOCTL_XE_GEM_CREATE, &g)) {   // a fresh VRAM BO: the bind's fence is its clear
			vramBo = g.handle;
			cpuFence = 0;
			s = ufSync((uint64_t)(uintptr_t)&cpuFence, 0xc1ea);
			CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, vramBo, 0x800000, 0x200000, &s, 1) == 0, "MAP of fresh VRAM %d", errno);
			CHECK(waitUf(&cpuFence, DRM_XE_UFENCE_WAIT_OP_EQ, 0xc1ea, M64, 2000000000, 0, 0, NULL) == 0 && cpuFence == 0xc1ea,
			      "fresh VRAM: written once its clear was done (0x%llx)", (unsigned long long)cpuFence);
		}
	}

	printf("7. Linux's quirks: the last user fence of an exec; none without batch\n");
	{
		struct drm_xe_sync s2[2] = {ufSync(FENCE + 5 * 8, 5), ufSync(FENCE + 6 * 8, 6)};
		CHECK(execS(rq, BATCH, s2, 2) == 0, "exec with two user fences %d", errno);
		CHECK(waitUf(fence + 6, DRM_XE_UFENCE_WAIT_OP_EQ, 6, M64, 2000000000, 0, 0, NULL) == 0, "the last one written");
		CHECK(fence[5] == 0, "the first one not written (0x%llx)", (unsigned long long)fence[5]);
		struct drm_xe_sync s = ufSync(FENCE + 7 * 8, 7);
		CHECK(execS(rq, 0, &s, 1) == 0, "exec without batch %d", errno);
		CHECK(waitUf(fence + 7, DRM_XE_UFENCE_WAIT_OP_EQ, 7, M64, 100000000, 0, 0, NULL) == ETIME, "no batch: not written");
	}

	printf("8. a wait bound to a queue that goes away: EIO\n");
	{
		const uint32_t q = queueNew(vm, DRM_XE_ENGINE_CLASS_RENDER, 0, 0);
		struct destroyArg da = {q};
		pthread_t th;
		pthread_create(&th, NULL, destroyLater, &da);
		const uint64_t t0 = ns();
		const int r = waitUf(fence + 3, DRM_XE_UFENCE_WAIT_OP_EQ, 1, M64, 3000000000LL, 0, q, NULL);
		pthread_join(th, NULL);
		CHECK(r == EIO, "wait on a destroyed queue: %d, want EIO", r);
		CHECK(ns() - t0 < 1000000000ull, "EIO came after %.1f ms", (ns() - t0) / 1e6);
	}

	printf("9. back-to-back compute execs, every other one with a user fence\n");
	{
		const uint32_t cq = queueNew(vm, DRM_XE_ENGINE_CLASS_COMPUTE, 0, 0);
		struct drm_syncobj_create sc = {0};
		xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc);
		CHECK(cq, "compute queue %d", errno);
		const int N = 2000;
		int bad = 0;
		for (int i = 1; i <= N && cq; i++) {
			struct drm_xe_sync s[2] = {ufSync(FENCE + 8 * 8, (uint64_t)i),
				{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sc.handle}};
			const int e = (i & 1) ? execS(cq, BATCH, s, 2) : execS(cq, BATCH, s + 1, 1);
			if (e) { bad++; break; }
			if ((i & 1) && waitUf(fence + 8, DRM_XE_UFENCE_WAIT_OP_GTE, (uint64_t)i, M64, 2000000000, 0, cq, NULL)) bad++;
			if (!(i & 1) && (i % 64) == 0 && syncWait(sc.handle)) bad++;
		}
		CHECK(!bad, "%d of %d jobs failed", bad, N);
		CHECK(!syncWait(sc.handle), "last job's syncobj");
		CHECK(fence[8] == (uint64_t)(N - 1), "last user fence %llu, want %d", (unsigned long long)fence[8], N - 1);
		struct drm_xe_exec_queue_destroy d = {.exec_queue_id = cq};
		xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d);
	}

	printf("10. exec fence at an unmapped GPU address of a scratch VM\n");
	{
		struct drm_xe_vm_create sv = {.flags = DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE};
		CHECK(!xioctl(DRM_IOCTL_XE_VM_CREATE, &sv), "scratch VM %d", errno);
		const uint32_t bb = gem(0x10000);
		volatile uint32_t *b2 = map(bb, 0x10000);
		b2[0] = MI_BBE; b2[1] = 0;
		flush(b2, 64);
		CHECK(!bindOp(sv.vm_id, DRM_XE_VM_BIND_OP_MAP, bb, BATCH, 0x10000, NULL, 0), "bind %d", errno);
		const uint32_t q = queueNew(sv.vm_id, DRM_XE_ENGINE_CLASS_RENDER, 0, 0);
		struct drm_syncobj_create sc = {0};
		xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc);
		struct drm_xe_sync s[2] = {ufSync(UNMAPPED, 9),
			{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sc.handle}};
		CHECK(execS(q, BATCH, s, 2) == 0, "exec %d", errno);
		CHECK(!syncWait(sc.handle), "job completed");
		struct drm_xe_exec_queue_get_property gp = {.exec_queue_id = q, .property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN};
		CHECK(!xioctl(DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY, &gp) && gp.value == 0, "queue not banned (%llu)", (unsigned long long)gp.value);
	}

	static char st[65536];
	size_t len = sizeof(st);
	if (!sysctlbyname("debug.intelxe.stats", st, &len, NULL, 0)) {
		char *l = strstr(st, "user fences:");
		if (l) { char *e = strchr(l, '\n'); if (e) *e = 0; printf("%s\n", l); }
	}
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
