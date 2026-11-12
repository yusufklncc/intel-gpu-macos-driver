// SPDX-License-Identifier: MIT
// lr: long-running VMs (DRM_XE_VM_CREATE_FLAG_LR_MODE) on IntelXe.kext, as Linux xe has them (xe_vm.c, xe_exec.c,
// xe_sync.c, xe_guc_submit.c, xe_preempt_fence.c) and Intel's compute runtime uses them. No Mesa, no Metal plugin.
//   ./lr args    the uAPI rules: LR_MODE accepted, FAULT_MODE refused (EINVAL, no page faults here); on an LR VM an
//                exec or a VM_BIND with a syncobj or timeline syncobj as out-sync is EOPNOTSUPP, a syncobj in-sync and a
//                user fence are fine; an exec without batch does nothing; the same syncobj out-sync works on a dma-fence VM
//   ./lr long    a job longer than the job timeout (debug.intelxe.job_timeout_ms, + 2 s): the batch marks itself
//                started, writes a VRAM BO outside the BAR window, then polls a semaphore the CPU holds. Meanwhile:
//                the CPU maps that BO (the kext moves it into the window: its LR VM's queue is preempted, the BO
//                copied and remapped, the queue resumed), and an unbind frees memory the VM mapped (one preemption cycle
//                before the free). The user fence must not be written and the queue must not be banned; after the CPU
//                releases the semaphore the batch writes the moved BO through its rewritten PTEs and the fence lands.
//                debug.intelxe.stats: no TDR check, no watchdog ban, no GT reset; the "long-running VMs" line counts
//                the job past the timeout, the move's hold and the cycle.
//   ./lr ds      direct submission on an LR queue (only with sysctl debug.intelxe.direct=1): back-to-back execs keep a
//                slot armed; a BO move in the middle preempts the queue, which leaves direct mode first (counted as an
//                "other" exit), and the execs after it arm again; every fence lands, nothing is banned
//   ./lr         args and long (ds when direct submission is on)
// Each run stays under 60 s (the default 10 s timeout: about 13 s). Exit status 0 when every check passed.
//   clang -arch x86_64 -O2 -I../../../kernel/IntelXe/uapi -o lr lr.c -framework IOKit
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <immintrin.h>
#include <mach/mach_time.h>
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
static int xerr(unsigned long req, void *arg) { return xioctl(req, arg) ? errno : 0; }
static mach_timebase_info_data_t tb;
static uint64_t ns(void) { return mach_absolute_time() * tb.numer / tb.denom; }
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  FAIL line %d: ", __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

static uint32_t gem(uint64_t size, uint32_t placement, uint16_t caching) {
	struct drm_xe_gem_create g = {.size = size, .placement = placement, .cpu_caching = caching};
	if (xioctl(DRM_IOCTL_XE_GEM_CREATE, &g)) { fprintf(stderr, "GEM_CREATE %d\n", errno); exit(1); }
	return g.handle;
}
static void *map(uint32_t handle, uint64_t size) {
	struct drm_xe_gem_mmap_offset m = {.handle = handle};
	if (xioctl(DRM_IOCTL_XE_GEM_MMAP_OFFSET, &m)) { printf("  MMAP_OFFSET %d\n", errno); return NULL; }
	mach_vm_address_t addr = 0; mach_vm_size_t sz = size;
	if (IOConnectMapMemory64(conn, (uint32_t)(m.offset >> 12), mach_task_self(), &addr, &sz, kIOMapAnywhere)) return NULL;
	return (void *)(uintptr_t)addr;
}
static int bindOp(uint32_t vm, uint32_t op, uint32_t bo, uint64_t addr, uint64_t range, struct drm_xe_sync *s, uint32_t n) {
	struct drm_xe_vm_bind b = {.vm_id = vm, .num_binds = 1, .num_syncs = n, .syncs = (uint64_t)(uintptr_t)s};
	b.bind.obj = bo; b.bind.range = range; b.bind.addr = addr; b.bind.op = op; b.bind.pat_index = 2;
	return xerr(DRM_IOCTL_XE_VM_BIND, &b);
}
static void flush(volatile void *p, size_t len) {
	for (size_t o = 0; o < len; o += 64) _mm_clflush((const char *)p + o);
	_mm_mfence();
}
static uint32_t vmNew(uint32_t flags, int *err) {
	struct drm_xe_vm_create vc = {.flags = flags};
	*err = xerr(DRM_IOCTL_XE_VM_CREATE, &vc);
	return *err ? 0 : vc.vm_id;
}
static void vmDel(uint32_t vm) {
	struct drm_xe_vm_destroy d = {.vm_id = vm};
	xioctl(DRM_IOCTL_XE_VM_DESTROY, &d);
}
static uint32_t queueNew(uint32_t vm, uint16_t cls) {
	struct drm_xe_engine_class_instance ei = {.engine_class = cls};
	struct drm_xe_exec_queue_create q = {.width = 1, .num_placements = 1, .vm_id = vm, .instances = (uint64_t)(uintptr_t)&ei};
	return xioctl(DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &q) ? 0 : q.exec_queue_id;
}
static void queueDel(uint32_t q) {
	struct drm_xe_exec_queue_destroy d = {.exec_queue_id = q};
	xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d);
}
static uint64_t banned(uint32_t q) {
	struct drm_xe_exec_queue_get_property gp = {.exec_queue_id = q, .property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN};
	return xioctl(DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY, &gp) ? ~0ull : gp.value;
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
static int waitUf(volatile void *addr, uint64_t value, int64_t timeout, uint32_t q) {
	struct drm_xe_wait_user_fence w = {.addr = (uint64_t)(uintptr_t)addr, .op = DRM_XE_UFENCE_WAIT_OP_EQ, .value = value,
		.mask = ~0ull, .timeout = timeout, .exec_queue_id = q};
	return xerr(DRM_IOCTL_XE_WAIT_USER_FENCE, &w);
}
static uint32_t syncNew(int signaled) {
	struct drm_syncobj_create sc = {.flags = signaled ? DRM_SYNCOBJ_CREATE_SIGNALED : 0};
	return xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc) ? 0 : sc.handle;
}

// debug.intelxe.stats: the number after `key`, in the line that holds `line` (NULL: anywhere); ~0 if not there
static char gStats[16384];
static void statsRead(void) {
	size_t l = sizeof(gStats);
	if (sysctlbyname("debug.intelxe.stats", gStats, &l, NULL, 0)) gStats[0] = 0;
}
static uint64_t statNum(const char *line, const char *key) {
	const char *p = gStats;
	if (line && !(p = strstr(gStats, line))) return ~0ull;
	const char *e = strchr(p, '\n');
	const char *k = strstr(p, key);
	if (!k || (e && k > e)) return ~0ull;
	return strtoull(k + strlen(key), NULL, 10);
}
static void statsLine(const char *line) {
	const char *p = strstr(gStats, line);
	if (!p) { printf("  (no \"%s\" line in debug.intelxe.stats: an older kext?)\n", line); return; }
	const char *e = strchr(p, '\n');
	printf("  %.*s\n", e ? (int)(e - p) : (int)strlen(p), p);
}

#define MI_SDI (0x20u << 23 | 2)                                   // MI_STORE_DATA_IMM, PPGTT, one dword
#define MI_BBE (0x0au << 23)                                       // MI_BATCH_BUFFER_END
#define MI_SEMA_GTE_POLL (0x1cu << 23 | 1u << 15 | 1u << 12 | 3)   // MI_SEMAPHORE_WAIT, polling, SAD >= SDD, PPGTT
enum { BATCH = 0x100000, FENCE = 0x200000, SEM = 0x300000, TARGET = 0x400000, SPARE = 0x500000 };
static const uint64_t VICTIM = 1ull << 32, VICTIM_SIZE = 16ull << 20;

static void args(void) {
	printf("1. uAPI rules\n");
	int err;
	uint32_t vm = vmNew(DRM_XE_VM_CREATE_FLAG_LR_MODE, &err);
	CHECK(vm && !err, "VM_CREATE LR_MODE: %d (an older kext refuses it with EINVAL)", err);
	if (!vm) return;
	uint32_t v2 = vmNew(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE, &err);
	CHECK(!v2 && err == EINVAL, "LR_MODE | FAULT_MODE: %d, want EINVAL (no page-fault mode)", err);
	v2 = vmNew(DRM_XE_VM_CREATE_FLAG_FAULT_MODE, &err);
	CHECK(!v2 && err == EINVAL, "FAULT_MODE alone: %d, want EINVAL", err);
	v2 = vmNew(DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE, &err);
	CHECK(v2 && !err, "LR_MODE | SCRATCH_PAGE: %d", err);
	if (v2) vmDel(v2);

	const uint32_t bb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB), fb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB);
	volatile uint32_t *batch = map(bb, 0x10000);
	volatile uint64_t *fence = map(fb, 0x10000);
	CHECK(batch && fence, "maps");
	if (!batch || !fence) return;
	batch[0] = MI_BBE; batch[1] = 0;
	flush(batch, 64);
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, bb, BATCH, 0x10000, NULL, 0), "bind batch");
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, fb, FENCE, 0x10000, NULL, 0), "bind fence page");
	const uint32_t q = queueNew(vm, DRM_XE_ENGINE_CLASS_COMPUTE);
	CHECK(q, "exec queue on the LR VM: %d", errno);
	if (!q) return;
	const uint32_t so = syncNew(0), sig = syncNew(1);
	struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = so};
	CHECK(execS(q, BATCH, &s, 1) == EOPNOTSUPP, "exec, syncobj out-sync: %d, want EOPNOTSUPP", errno);
	struct drm_xe_sync t = {.type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = so,
		.timeline_value = 1};
	CHECK(execS(q, BATCH, &t, 1) == EOPNOTSUPP, "exec, timeline out-sync: %d, want EOPNOTSUPP", errno);
	CHECK(bindOp(vm, DRM_XE_VM_BIND_OP_MAP, fb, SPARE, 0x10000, &s, 1) == EOPNOTSUPP, "VM_BIND, syncobj out-sync: %d",
		errno);
	struct drm_xe_sync two[2] = {{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .handle = sig}, ufSync(FENCE, 5)};
	CHECK(execS(q, BATCH, two, 2) == 0, "exec, syncobj in-sync + user fence: %d", errno);
	CHECK(waitUf(fence, 5, 2000000000, q) == 0, "its user fence: %d", errno);
	struct drm_xe_sync u = ufSync(FENCE + 8, 6);
	CHECK(execS(q, 0, &u, 1) == 0, "exec without batch: %d", errno);
	usleep(20000);
	CHECK(fence[1] == 0, "exec without batch on an LR VM writes nothing (xe_exec_ioctl): %llu", (unsigned long long)fence[1]);
	queueDel(q);
	vmDel(vm);

	uint32_t dvm = vmNew(0, &err);   // the same syncobj out-sync on a dma-fence VM
	CHECK(dvm && !bindOp(dvm, DRM_XE_VM_BIND_OP_MAP, bb, BATCH, 0x10000, NULL, 0), "dma-fence VM");
	const uint32_t dq = dvm ? queueNew(dvm, DRM_XE_ENGINE_CLASS_COMPUTE) : 0;
	if (dq) {
		CHECK(execS(dq, BATCH, &s, 1) == 0, "dma-fence VM, syncobj out-sync: %d", errno);
		struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&so, .count_handles = 1,
			.timeout_nsec = (int64_t)(ns() + 2000000000ull)};
		CHECK(!xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w), "its syncobj: %d", errno);
		queueDel(dq);
	}
	if (dvm) vmDel(dvm);
}

static void longJob(void) {
	printf("2. a job longer than the job timeout on an LR VM\n");
	int timeoutMs = 0;
	size_t l = sizeof(timeoutMs);
	if (sysctlbyname("debug.intelxe.job_timeout_ms", &timeoutMs, &l, NULL, 0)) timeoutMs = 10000;
	uint64_t runMs = (timeoutMs > 0 ? (uint64_t)timeoutMs : 1000) + 2000;
	if (runMs > 45000) runMs = 45000;   // night commands stay under 60 s
	printf("  debug.intelxe.job_timeout_ms %d: the job runs %llu ms\n", timeoutMs, (unsigned long long)runMs);
	int err;
	const uint32_t vm = vmNew(DRM_XE_VM_CREATE_FLAG_LR_MODE, &err);
	CHECK(vm, "LR VM: %d", err);
	if (!vm) return;
	const uint32_t bb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB), fb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB),
		sb = gem(0x1000, 1, DRM_XE_GEM_CPU_CACHING_WB), tb_ = gem(0x1000, 1, DRM_XE_GEM_CPU_CACHING_WB),
		vb = gem(VICTIM_SIZE, 2, DRM_XE_GEM_CPU_CACHING_WC), spare = gem(0x10000, 2, DRM_XE_GEM_CPU_CACHING_WC);
	volatile uint32_t *batch = map(bb, 0x10000), *sem = map(sb, 0x1000), *target = map(tb_, 0x1000);
	volatile uint64_t *fence = map(fb, 0x10000);
	CHECK(batch && fence && sem && target, "maps");
	if (!batch || !fence || !sem || !target) return;
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, bb, BATCH, 0x10000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, fb, FENCE, 0x10000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, sb, SEM, 0x1000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, tb_, TARGET, 0x1000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, vb, VICTIM, VICTIM_SIZE, NULL, 0), "binds: %d", errno);
	uint32_t n = 0;
	batch[n++] = MI_SDI; batch[n++] = TARGET + 4; batch[n++] = 0; batch[n++] = 1;   // started
	batch[n++] = MI_SDI; batch[n++] = (uint32_t)VICTIM; batch[n++] = (uint32_t)(VICTIM >> 32); batch[n++] = 0x1111;
	batch[n++] = MI_SEMA_GTE_POLL; batch[n++] = 1; batch[n++] = SEM; batch[n++] = 0; batch[n++] = 0;
	batch[n++] = MI_SDI; batch[n++] = (uint32_t)(VICTIM + 4); batch[n++] = (uint32_t)(VICTIM >> 32); batch[n++] = 0x2222;
	batch[n++] = MI_SDI; batch[n++] = TARGET; batch[n++] = 0; batch[n++] = 0xcafe;
	batch[n++] = MI_BBE; batch[n++] = 0;
	sem[0] = 0; target[0] = target[1] = 0; fence[0] = 0;
	flush(batch, n * 4); flush(sem, 64); flush(target, 64); flush(fence, 64);
	const uint32_t q = queueNew(vm, DRM_XE_ENGINE_CLASS_COMPUTE);
	CHECK(q, "queue: %d", errno);
	if (!q) return;
	statsRead();
	const uint64_t tdr0 = statNum("TDR:", "checks "), wd0 = statNum("jobs:", "(watchdog "), gtr0 = statNum("TDR:", "GT resets "),
		past0 = statNum("long-running VMs", "past the job timeout "), holds0 = statNum("long-running VMs", "move holds "),
		dis0 = statNum("long-running VMs", "disables "), cyc0 = statNum("long-running VMs", ", done ");
	if (past0 == ~0ull) printf("  this kext has no \"long-running VMs\" stats line\n");
	struct drm_xe_sync u = ufSync(FENCE, 0x77);
	CHECK(execS(q, BATCH, &u, 1) == 0, "exec: %d", errno);
	const uint64_t t0 = ns();
	while (target[1] != 1 && ns() - t0 < 2000000000ull) usleep(1000);
	CHECK(target[1] == 1, "the job started");

	// a. the CPU maps the VRAM BO the running job writes: a move into the BAR window, under preemption
	usleep(100000);
	const uint64_t m0 = ns();
	volatile uint32_t *victim = map(vb, VICTIM_SIZE);
	const uint64_t m1 = ns();
	CHECK(victim != NULL, "map of the BO outside the window (the move preempts the LR queue)");
	if (victim) {
		printf("  map (move into the window) took %.1f ms\n", (m1 - m0) / 1e6);
		CHECK(victim[0] == 0x1111, "the job's write before the move came along: 0x%x", victim[0]);
	}
	// b. an unbind of memory the VM mapped: its free waits for one preemption cycle
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, spare, SPARE, 0x10000, NULL, 0), "bind spare");
	usleep(10000);   // past min_run_period_lr_ms after the move's resume
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_UNMAP, 0, SPARE, 0x10000, NULL, 0), "unbind spare: %d", errno);
	struct drm_gem_close gc = {.handle = spare};
	xioctl(DRM_IOCTL_GEM_CLOSE, &gc);

	// c. past the job timeout: no fence, no ban
	int midWait = -1;
	while (ns() - t0 < runMs * 1000000ull) {
		if (midWait < 0 && ns() - t0 > runMs * 500000ull) midWait = waitUf(fence, 0x77, 100000000, q);
		usleep(100000);
	}
	CHECK(midWait == ETIME, "a wait during the job: %d, want ETIME", midWait);
	CHECK(fence[0] == 0, "the user fence was written while the job was blocked: 0x%llx", (unsigned long long)fence[0]);
	CHECK(banned(q) == 0, "queue banned after %llu ms (the job timeout must not apply)", (unsigned long long)runMs);
	statsRead();
	CHECK(statNum("TDR:", "checks ") == tdr0, "a TDR check ran: %llu -> %llu", (unsigned long long)tdr0,
		(unsigned long long)statNum("TDR:", "checks "));
	CHECK(statNum("jobs:", "(watchdog ") == wd0, "a watchdog ban");
	CHECK(statNum("TDR:", "GT resets ") == gtr0, "a GT reset");
	if (past0 != ~0ull && timeoutMs > 0)
		CHECK(statNum("long-running VMs", "past the job timeout ") > past0, "the job is not counted past the job timeout");
	if (holds0 != ~0ull) {
		CHECK(statNum("long-running VMs", "move holds ") > holds0, "the move held no LR VM");
		CHECK(statNum("long-running VMs", "disables ") > dis0, "no preemption (schedule disable) was sent");
		CHECK(statNum("long-running VMs", ", done ") > cyc0, "no preemption cycle for the unbind's free");
	}

	// d. release: the job writes the moved BO through its rewritten PTEs, then its user fence
	sem[0] = 1;
	flush(sem, 64);
	const uint64_t r0 = ns();
	const int w = waitUf(fence, 0x77, 2000000000, q);
	CHECK(w == 0, "user fence after the release: %d", w);
	printf("  release -> fence %.1f us\n", (ns() - r0) / 1e3);
	CHECK(target[0] == 0xcafe, "the job's last store: 0x%x", target[0]);
	if (victim) CHECK(victim[1] == 0x2222, "the job's write after the move landed in the moved BO: 0x%x", victim[1]);
	CHECK(banned(q) == 0, "queue banned at the end");
	statsRead();
	statsLine("long-running VMs");
	statsLine("BO moves");
	queueDel(q);
	vmDel(vm);
}

static void ds(void) {
	printf("3. direct submission on an LR queue\n");
	int on = 0;
	size_t l = sizeof(on);
	if (sysctlbyname("debug.intelxe.direct", &on, &l, NULL, 0) || !on) {
		printf("  skipped: debug.intelxe.direct is 0\n");
		return;
	}
	int err;
	const uint32_t vm = vmNew(DRM_XE_VM_CREATE_FLAG_LR_MODE, &err);
	CHECK(vm, "LR VM: %d", err);
	if (!vm) return;
	const uint32_t bb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB), fb = gem(0x10000, 1, DRM_XE_GEM_CPU_CACHING_WB),
		vb = gem(4ull << 20, 2, DRM_XE_GEM_CPU_CACHING_WC);
	volatile uint32_t *batch = map(bb, 0x10000);
	volatile uint64_t *fence = map(fb, 0x10000);
	CHECK(batch && fence, "maps");
	if (!batch || !fence) return;
	batch[0] = MI_BBE; batch[1] = 0;
	flush(batch, 64);
	CHECK(!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, bb, BATCH, 0x10000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, fb, FENCE, 0x10000, NULL, 0) &&
		!bindOp(vm, DRM_XE_VM_BIND_OP_MAP, vb, VICTIM, 4ull << 20, NULL, 0), "binds: %d", errno);
	const uint32_t q = queueNew(vm, DRM_XE_ENGINE_CLASS_COMPUTE);
	CHECK(q, "queue: %d", errno);
	if (!q) return;
	statsRead();
	const uint64_t arms0 = statNum("direct submission", "arms "), other0 = statNum("direct submission", "other ");
	int bad = 0;
	for (int i = 0; i < 400; i++) {
		if (i == 200) CHECK(map(vb, 4ull << 20) != NULL, "map (a move: the LR queue is preempted) mid-stream");
		struct drm_xe_sync u = ufSync(FENCE + 8 * (i % 512), (uint64_t)i + 1);
		if (execS(q, BATCH, &u, 1) || waitUf(fence + i % 512, (uint64_t)i + 1, 2000000000, q)) bad++;
	}
	CHECK(!bad, "%d of 400 execs failed or their fence did not land", bad);
	CHECK(banned(q) == 0, "queue banned");
	statsRead();
	CHECK(statNum("direct submission", "arms ") > arms0, "the LR queue never armed");
	CHECK(statNum("direct submission", "other ") > other0, "no exit for the preemption");
	statsLine("direct submission");
	statsLine("long-running VMs");
	queueDel(q);
	vmDel(vm);
}

int main(int argc, char **argv) {
	mach_timebase_info(&tb);
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) { fprintf(stderr, "no IntelXe service\n"); return 1; }
	const char *ph = argc > 1 ? argv[1] : "all";
	if (!strcmp(ph, "args") || !strcmp(ph, "all")) args();
	if (!strcmp(ph, "long") || !strcmp(ph, "all")) longJob();
	if (!strcmp(ph, "ds") || !strcmp(ph, "all")) ds();
	printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
	return fails != 0;
}
