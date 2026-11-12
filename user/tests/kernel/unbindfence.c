// SPDX-License-Identifier: MIT
// unbindfence: the rb8d kernel hardening of IntelXe, through the uAPI only (docs/design/DESIGN-cat-fault.md, "rb8d").
//   fences   (item 1) VM_BIND out-fences of calls that bind a fresh VRAM BO (a copy-engine clear) and unbind the previous
//            one (a TLB invalidation), N times:
//              - right after the ioctl: is the binary out-syncobj still pending? (rb8d: often, the invalidation is a G2H
//                round trip; before rb8d only while the clear ran)
//              - the timeline point transferred into a binary syncobj, that one waited for: the timeline point must read
//                signaled then (SYNCOBJ_TRANSFER carries every fence of the point: the clear and the invalidation)
//              - a VM_BIND without ops (out-syncobj only) right after: once it has signaled, the unbinding call's own
//                out-fence must have too (it signals the VM's last bind fence)
//              - the unbound range reused at once: after waiting on the unbind's out-fence a new system BO with a fresh
//                pattern is bound there and a render job (in-fence: that bind's out-fence) copies it out: it must read
//                the new pattern
//              - an unbind with a user fence: rb8e writes it asynchronously (as xe's user_fence_worker), so it is
//                polled in memory without any ioctl of this process for up to 1 s: it must arrive (counted: there at
//                return, later, never; the longest wait), then WAIT_USER_FENCE on it must answer at once
//   queues   (item 2) exec queues created and destroyed N times (render and copy in turn), each running one store job
//            that is checked: every new queue reuses a seqno slot and its GGTT window (GGTT invalidation through the
//            GuC, waited for)
//   hang     (item 4, OPTIONAL, resets the render engine and maybe the GT: run it last, with nothing else on the GPU
//            that matters) queue A runs a batch that loops on itself without an arbitration point; queue B (render
//            too) submits a store job: B's job must complete (the GuC resets A's context at its preemption timeout, or
//            the watchdog resets the GT: within ~2.5 s of the reset if the engine stays stuck, 5.5 s if the job never
//            started); A must read as banned. Prints how long B took.
//   bindq    (rb8e item 4) two bind queues (DRM_XE_ENGINE_CLASS_VM_BIND) A and B, N rounds: A maps a system BO and
//            unbinds the previous one (its out-fence waits for the invalidation), B maps a system BO right after (nothing
//            to wait for): B's out-fence must not wait for A's invalidation (before rb8e every bind was ordered behind
//            the VM's last); a no-op VM_BIND and an EXEC without batches on A must not signal before A's unbind did; A's
//            unbinds carry a user fence (item 2: polled in memory without ioctls, then WAIT_USER_FENCE). Also: VM_BIND
//            naming a render queue answers EINVAL, an unknown queue ENOENT (xe_vm_bind_ioctl).
//   windows  (rb8e item 1, root) the migrate windows: debug.intelxe.dispcheck reads every display buffer twice through
//            the copy engine (svCopyBo), debug.intelxefb_thumb may read the scan-out through it (xeVramRead, when it
//            lies outside the BAR window): the counters "migrate windows (rb8e)" must move, every seal awaited, no pages
//            kept. A BO move (vramCopyToSys) is exercised by ./bomove evict.
// Then the kext's rb8d and rb8e counters (debug.intelxe.stats) as deltas. "RESULT: PASS" / "RESULT: FAIL (...)", exit
// 0 / 1.
//   clang -arch x86_64 -O2 -I../../../kernel/IntelXe/uapi -o unbindfence unbindfence.c -framework IOKit
//   ./unbindfence [fences=N] [queues=N] [bindq=N] [windows=N] [hang]
//            (defaults: fences=500 queues=200 bindq=300 windows=0 (needs root: sudo ... windows=4), no hang)
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <immintrin.h>
#include <mach/mach.h>
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
	if (IOConnectCallScalarMethod(conn, 0, in, 2, &out, &n)) { errno = EIO; return -1; }
	errno = (int)out;
	return out ? -1 : 0;
}
static mach_timebase_info_data_t tb;
static uint64_t ns(void) { return mach_absolute_time() * tb.numer / tb.denom; }
static void flush(volatile void *p, size_t len) {
	for (size_t o = 0; o < len; o += 64) _mm_clflush((const char *)p + o);
	_mm_mfence();
}
static int fails;
#define FAIL(...) do { fails++; printf("FAIL: " __VA_ARGS__); printf("\n"); } while (0)

static uint32_t gem(uint64_t size, uint32_t placement, uint32_t flags, uint16_t caching) {
	struct drm_xe_gem_create g = {.size = size, .placement = placement, .flags = flags, .cpu_caching = caching};
	return xioctl(DRM_IOCTL_XE_GEM_CREATE, &g) ? 0 : g.handle;
}
static void gemClose(uint32_t h) { struct drm_gem_close c = {.handle = h}; xioctl(DRM_IOCTL_GEM_CLOSE, &c); }
static void *map(uint32_t handle, uint64_t size) {
	struct drm_xe_gem_mmap_offset m = {.handle = handle};
	if (xioctl(DRM_IOCTL_XE_GEM_MMAP_OFFSET, &m)) return NULL;
	mach_vm_address_t addr = 0; mach_vm_size_t sz = size;
	if (IOConnectMapMemory64(conn, (uint32_t)(m.offset >> 12), mach_task_self(), &addr, &sz, kIOMapAnywhere)) return NULL;
	return (void *)(uintptr_t)addr;
}
static uint32_t gVm;
// one VM_BIND of nOps ops with the given syncs
static int bind(struct drm_xe_vm_bind_op *ops, uint32_t nOps, struct drm_xe_sync *syncs, uint32_t nSyncs) {
	struct drm_xe_vm_bind b = {.vm_id = gVm, .num_binds = nOps, .num_syncs = nSyncs, .syncs = (uint64_t)(uintptr_t)syncs};
	if (nOps == 1) b.bind = ops[0];
	else if (nOps > 1) b.vector_of_binds = (uint64_t)(uintptr_t)ops;
	return xioctl(DRM_IOCTL_XE_VM_BIND, &b) ? errno : 0;
}
static struct drm_xe_vm_bind_op opMap(uint32_t bo, uint64_t va, uint64_t len) {
	return (struct drm_xe_vm_bind_op){.obj = bo, .addr = va, .range = len, .op = DRM_XE_VM_BIND_OP_MAP, .pat_index = 2};
}
static struct drm_xe_vm_bind_op opUnmap(uint64_t va, uint64_t len) {
	return (struct drm_xe_vm_bind_op){.addr = va, .range = len, .op = DRM_XE_VM_BIND_OP_UNMAP};
}
static uint32_t syncNew(void) {
	struct drm_syncobj_create c = {0};
	return xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &c) ? 0 : c.handle;
}
static void syncReset(uint32_t h) {
	struct drm_syncobj_array a = {.handles = (uint64_t)(uintptr_t)&h, .count_handles = 1};
	xioctl(DRM_IOCTL_SYNCOBJ_RESET, &a);
}
// 0 signaled, ETIME not (timeoutNs 0: a poll)
static int syncWait(uint32_t h, uint64_t timeoutNs) {
	struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&h, .count_handles = 1,
		.timeout_nsec = timeoutNs ? (int64_t)(ns() + timeoutNs) : 0};
	return xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w) ? errno : 0;
}
static int tlWait(uint32_t h, uint64_t point, uint64_t timeoutNs) {
	struct drm_syncobj_timeline_wait w = {.handles = (uint64_t)(uintptr_t)&h, .points = (uint64_t)(uintptr_t)&point,
		.count_handles = 1, .timeout_nsec = timeoutNs ? (int64_t)(ns() + timeoutNs) : 0};
	return xioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &w) ? errno : 0;
}

// debug.intelxe.stats numbers after a key (first occurrence after `after`, if given); -1 if missing
static char gStatBuf[32768];
static void statsLoad(void) {
	size_t len = sizeof gStatBuf - 1;
	if (sysctlbyname("debug.intelxe.stats", gStatBuf, &len, NULL, 0)) len = 0;
	gStatBuf[len] = 0;
}
static long long statNum(const char *after, const char *key) {
	const char *p = after ? strstr(gStatBuf, after) : gStatBuf;
	p = p ? strstr(p, key) : NULL;
	return p ? strtoll(p + strlen(key), NULL, 10) : -1;
}
enum { S_UNBINDS, S_PEND, S_NOOPS, S_GGUC, S_GMMIO, S_GWAIT, S_GTO, S_TLBFORCED, S_CAT, S_DOMRESET, S_STUCK, S_NOTSTARTED,
	S_GTRESET, S_CTXRESET,
	// rb8e
	S_UF_DIRECT, S_UF_ASYNC, S_UF_PINNED, S_UF_DROPPED, S_UF_WAITED, S_BQ_MADE, S_BQ_BINDS, S_BQ_EXECS, S_MW_MOVE, S_MW_SURF,
	S_MW_READ, S_MW_AWAITED, S_MW_KEPT_MOVE, S_MW_KEPT_SURF, S_MW_KEPT_READ, S_TLB_TIMEOUT_MS, S_N };
static void statsRead(long long *v) {
	statsLoad();
	v[S_UNBINDS] = statNum("bind out-fences (rb8d)", "unbinds ");
	v[S_PEND] = statNum("bind out-fences (rb8d)", "pending at return ");
	v[S_NOOPS] = statNum("bind out-fences (rb8d)", "calls without ops ");
	v[S_GGUC] = statNum("GGTT invalidations", "GuC ");
	v[S_GMMIO] = statNum("GGTT invalidations", "MMIO ");
	v[S_GWAIT] = statNum("GGTT invalidations", "waited ");
	v[S_GTO] = statNum("GGTT invalidations", "timed out ");
	v[S_TLBFORCED] = statNum("bind out-fences (rb8d)", "counted done unanswered ");
	v[S_CAT] = statNum("recovery (rb8d)", "CAT errors ");
	v[S_DOMRESET] = statNum("recovery (rb8d)", "primary engine ");
	v[S_STUCK] = statNum("recovery (rb8d)", "stuck after one ");
	v[S_NOTSTARTED] = statNum("never started in ", "ms ");
	v[S_GTRESET] = statNum("TDR:", "GT resets ");
	v[S_CTXRESET] = statNum("TDR:", "GuC context resets ");
	v[S_UF_DIRECT] = statNum("bind user fences (rb8e)", "signaled at return ");
	v[S_UF_ASYNC] = statNum("bind user fences (rb8e)", "written asynchronously ");
	v[S_UF_PINNED] = statNum("written asynchronously", " of ");
	v[S_UF_DROPPED] = statNum("bind user fences (rb8e)", "dropped at close ");
	v[S_UF_WAITED] = statNum("bind user fences (rb8e)", "waited in the ioctl ");
	v[S_BQ_MADE] = statNum("bind queues (rb8e)", "made ");
	v[S_BQ_BINDS] = statNum("bind queues (rb8e)", "binds on one ");
	v[S_BQ_EXECS] = statNum("bind queues (rb8e)", "EXECs on one ");
	v[S_MW_MOVE] = statNum("migrate windows (rb8e)", "sealed move ");
	v[S_MW_SURF] = statNum("migrate windows (rb8e)", "surface ");
	v[S_MW_READ] = statNum("migrate windows (rb8e)", "VRAM read ");
	v[S_MW_AWAITED] = statNum("migrate windows (rb8e)", "awaited ");
	v[S_MW_KEPT_MOVE] = statNum("migrate windows (rb8e)", "pages kept move ");
	v[S_MW_KEPT_SURF] = statNum("pages kept move", "surface ");
	v[S_MW_KEPT_READ] = statNum("pages kept move", "VRAM read ");
	v[S_TLB_TIMEOUT_MS] = statNum("migrate windows (rb8e)", "TLB fence timeout ");
}

#define MI_SDI (0x20u << 23 | 2)   // MI_STORE_DATA_IMM, PPGTT, one dword
#define MI_CMM (0x2eu << 23 | 3)   // MI_COPY_MEM_MEM, PPGTT
#define MI_BBE (0x0au << 23)
#define MI_BBS (0x31u << 23 | 1u << 8 | 1)   // MI_BATCH_BUFFER_START, PPGTT, 48-bit address
static const uint64_t BATCH_VA = 0x100000000ull, RES_VA = 0x200000000ull, CHURN_VA = 0x4000000000ull;
static volatile uint32_t *batch, *res;

static uint32_t queueNew(uint16_t cls) {
	struct drm_xe_engine_class_instance ei = {.engine_class = cls};
	struct drm_xe_exec_queue_create c = {.width = 1, .num_placements = 1, .vm_id = gVm, .instances = (uint64_t)(uintptr_t)&ei};
	return xioctl(DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &c) ? 0 : c.exec_queue_id;
}
static void queueDel(uint32_t q) { struct drm_xe_exec_queue_destroy d = {.exec_queue_id = q}; xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d); }
static int exec(uint32_t q, uint64_t bbVa, struct drm_xe_sync *s, uint32_t n) {
	struct drm_xe_exec x = {.exec_queue_id = q, .num_syncs = n, .syncs = (uint64_t)(uintptr_t)s, .address = bbVa, .num_batch_buffer = 1};
	return xioctl(DRM_IOCTL_XE_EXEC, &x) ? errno : 0;
}

// rb8e: a bind user fence the kernel writes asynchronously: polled in memory without any ioctl of this process for up to
// 1 s. Returns 0 there at return, 1 written later, 2 never; *us: how long it took
static int ufPoll(volatile uint64_t *uf, uint64_t want, uint64_t *us) {
	const uint64_t t0 = ns();
	if (*uf == want) { *us = 0; return 0; }
	while (ns() - t0 < 1000000000ull) {
		_mm_pause();
		if (*uf == want) { *us = (ns() - t0) / 1000; return 1; }
	}
	*us = (ns() - t0) / 1000;
	return 2;
}
static int ufWait(volatile uint64_t *uf, uint64_t want) {   // WAIT_USER_FENCE, 1 s
	struct drm_xe_wait_user_fence w = {.addr = (uint64_t)(uintptr_t)uf, .op = DRM_XE_UFENCE_WAIT_OP_EQ, .value = want,
		.mask = ~0ull, .timeout = 1000000000};
	return xioctl(DRM_IOCTL_XE_WAIT_USER_FENCE, &w) ? errno : 0;
}
static int gUfAtReturn, gUfLater, gUfNever, gUfWaitErr;
static uint64_t gUfMaxUs;
static void ufCheck(volatile uint64_t *uf, uint64_t want) {
	uint64_t us = 0;
	const int r = ufPoll(uf, want, &us);
	if (r == 0) gUfAtReturn++;
	else if (r == 1) gUfLater++;
	else gUfNever++;
	if (us > gUfMaxUs) gUfMaxUs = us;
	if (ufWait(uf, want)) gUfWaitErr++;
}

// ─── item 1 ──────────────────────────────────────────────────────────────────────────────────────────────────────
static void fences(int n, uint32_t rq) {
	const uint32_t tl = syncNew(), bin = syncNew(), xfer = syncNew(), z = syncNew(), rb = syncNew(), done = syncNew();
	if (!tl || !bin || !xfer || !z || !rb || !done) { FAIL("syncobjs"); return; }
	volatile uint64_t *uf = (volatile uint64_t *)(res + 0x800);   // a user-fence word in the result BO (CPU address)
	int pendBin = 0, badXfer = 0, badNoOp = 0, pendNoOp = 0, badData = 0, errs = 0;
	uint64_t prevVa = 0, prevLen = 0;
	const uint64_t t0 = ns();
	for (int i = 1; i <= n; i++) {
		// a fresh VRAM BO (its clear runs on the copy engine) bound, the previous one unbound, in one call
		const uint64_t len = 0x10000ull * (uint64_t)(1 + i % 8), va = CHURN_VA + (uint64_t)(i % 64) * 0x200000;
		const uint32_t bo = gem(len, 2, 0, DRM_XE_GEM_CPU_CACHING_WC);
		if (!bo) { errs++; continue; }
		struct drm_xe_vm_bind_op ops[2] = {opMap(bo, va, len), opUnmap(prevVa, prevLen)};
		syncReset(bin);
		struct drm_xe_sync s[2] = {
			{.type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = tl, .timeline_value = (uint64_t)i},
			{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = bin}};
		const int e = bind(ops, prevLen ? 2 : 1, s, 2);
		gemClose(bo);
		if (e) { errs++; if (errs < 5) printf("bind errno %d\n", e); continue; }
		if (syncWait(bin, 0) == ETIME) pendBin++;
		// a call without ops: once it has signaled, the unbinding call's fence has
		syncReset(z);
		struct drm_xe_sync zs = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = z};
		if (bind(NULL, 0, &zs, 1)) errs++;
		if (syncWait(z, 0) == ETIME) pendNoOp++;
		if (syncWait(z, 2000000000ull)) errs++;
		else if (syncWait(bin, 0)) badNoOp++;
		// the timeline point through a binary syncobj
		syncReset(xfer);
		struct drm_syncobj_transfer t = {.src_handle = tl, .dst_handle = xfer, .src_point = (uint64_t)i};
		if (xioctl(DRM_IOCTL_SYNCOBJ_TRANSFER, &t)) errs++;
		else if (syncWait(xfer, 2000000000ull)) errs++;
		else if (tlWait(tl, (uint64_t)i, 0)) badXfer++;
		if (syncWait(bin, 2000000000ull)) errs++;
		// reuse the range just unbound at once: a system BO with this round's pattern, copied out by a render job
		if (prevLen) {
			const uint32_t sb = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);
			volatile uint32_t *sp = sb ? map(sb, 0x10000) : NULL;
			if (!sp) { errs++; } else {
				for (int k = 0; k < 16; k++) sp[k * 1024] = 0xb0000000u | (uint32_t)i << 4 | (uint32_t)k;
				flush(sp, 0x10000);
				syncReset(rb);
				struct drm_xe_vm_bind_op m = opMap(sb, prevVa, 0x10000);
				struct drm_xe_sync rs = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = rb};
				if (bind(&m, 1, &rs, 1)) errs++;
				for (int k = 0; k < 16; k++) res[k] = 0;
				flush(res, 64);
				uint32_t w = 0;
				for (int k = 0; k < 16; k++) {
					const uint64_t src = prevVa + (uint64_t)k * 4096, dst = RES_VA + (uint64_t)k * 4;
					batch[w++] = MI_CMM; batch[w++] = (uint32_t)dst; batch[w++] = (uint32_t)(dst >> 32);
					batch[w++] = (uint32_t)src; batch[w++] = (uint32_t)(src >> 32);
				}
				batch[w++] = MI_BBE;
				if (w & 1) batch[w++] = 0;
				flush(batch, w * 4);
				syncReset(done);
				struct drm_xe_sync xs[2] = {{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .handle = rb},
					{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = done}};
				if (exec(rq, BATCH_VA, xs, 2) || syncWait(done, 5000000000ull)) errs++;
				else {
					flush(res, 64);
					for (int k = 0; k < 16; k++)
						if (res[k] != (0xb0000000u | (uint32_t)i << 4 | (uint32_t)k)) {
							if (badData++ < 3) printf("reuse round %d word %d: 0x%08x\n", i, k, res[k]);
							break;
						}
				}
				// unbind it again with a user fence: written by the time VM_BIND returns
				*uf = 0;
				flush((void *)uf, 8);
				struct drm_xe_vm_bind_op u = opUnmap(prevVa, 0x10000);
				struct drm_xe_sync us = {.type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL,
					.addr = (uint64_t)(uintptr_t)uf, .timeline_value = 0x1000u + (uint64_t)i};
				if (bind(&u, 1, &us, 1)) errs++;
				else ufCheck(uf, 0x1000u + (uint64_t)i);   // rb8e: written asynchronously
				gemClose(sb);
			}
		}
		prevVa = va;
		prevLen = len;
	}
	printf("fences: %d rounds in %.1f s: out-fence pending at return %d, no-op call pending at return %d; transferred point "
		"signaled before the timeline %d, no-op fence before the unbind's %d, reused range read stale %d, errors %d\n", n,
		(ns() - t0) / 1e9, pendBin, pendNoOp, badXfer, badNoOp, badData, errs);
	if (badXfer || badNoOp || badData || errs) FAIL("fences (see the line above)");
	if (!pendBin) printf("note: no out-fence was pending at return (a kext before rb8d, or every invalidation answered at once)\n");
}

// ─── item 2 ──────────────────────────────────────────────────────────────────────────────────────────────────────
static void queues(int n) {
	const uint32_t done = syncNew();
	int bad = 0, errs = 0;
	const uint64_t t0 = ns();
	for (int i = 0; i < n; i++) {
		const uint16_t cls = i & 1 ? DRM_XE_ENGINE_CLASS_COPY : DRM_XE_ENGINE_CLASS_RENDER;
		const uint32_t q = queueNew(cls);
		if (!q) { errs++; continue; }
		const uint64_t dst = RES_VA + 0x100;
		res[0x40] = 0;
		flush(res, 0x1000);
		batch[0] = MI_SDI; batch[1] = (uint32_t)dst; batch[2] = (uint32_t)(dst >> 32); batch[3] = 0xc0000000u + (uint32_t)i;
		batch[4] = MI_BBE; batch[5] = 0;
		flush(batch, 64);
		syncReset(done);
		struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = done};
		if (exec(q, BATCH_VA, &s, 1) || syncWait(done, 5000000000ull)) errs++;
		else {
			flush(res, 0x1000);
			if (res[0x40] != 0xc0000000u + (uint32_t)i && bad++ < 3) printf("queue %d (%s): read 0x%08x\n", i, cls ? "copy" : "render", res[0x40]);
		}
		queueDel(q);
	}
	printf("queues: %d created, run and destroyed in %.1f s: wrong results %d, errors %d\n", n, (ns() - t0) / 1e9, bad, errs);
	if (bad || errs) FAIL("queues");
}

// ─── rb8e item 4: bind queues ────────────────────────────────────────────────────────────────────────────────────────
static int bindOn(uint32_t q, struct drm_xe_vm_bind_op *ops, uint32_t nOps, struct drm_xe_sync *syncs, uint32_t nSyncs) {
	struct drm_xe_vm_bind b = {.vm_id = gVm, .exec_queue_id = q, .num_binds = nOps, .num_syncs = nSyncs,
		.syncs = (uint64_t)(uintptr_t)syncs};
	if (nOps == 1) b.bind = ops[0];
	return xioctl(DRM_IOCTL_XE_VM_BIND, &b) ? errno : 0;
}
static const uint64_t BQ_A_VA = 0x6000000000ull, BQ_B_VA = 0x7000000000ull;
static void bindq(int n, uint32_t rq) {
	const uint32_t qa = queueNew(DRM_XE_ENGINE_CLASS_VM_BIND), qb = queueNew(DRM_XE_ENGINE_CLASS_VM_BIND);
	const uint32_t sa = syncNew(), sb = syncNew(), z = syncNew(), e = syncNew();
	if (!qa || !qb || !sa || !sb || !z || !e) { FAIL("bindq: bind queues %d", errno); return; }
	// xe_vm_bind_ioctl: a render queue is not a bind queue (EINVAL), an unknown id ENOENT
	struct drm_xe_vm_bind_op none = opUnmap(BQ_A_VA, 0x10000);
	const int eRender = bindOn(rq, &none, 1, NULL, 0), eUnknown = bindOn(0xfff0, &none, 1, NULL, 0);
	if (eRender != EINVAL || eUnknown != ENOENT)
		FAIL("bindq: VM_BIND on a render queue %d (want EINVAL %d), on an unknown queue %d (want ENOENT %d)", eRender, EINVAL,
			eUnknown, ENOENT);
	volatile uint64_t *uf = (volatile uint64_t *)(res + 0x900);
	uint32_t bBos[512];
	int nb = 0, aPend = 0, bPendWhileA = 0, noOpEarly = 0, execEarly = 0, errs = 0;
	uint64_t prevA = 0;
	const uint64_t t0 = ns();
	for (int i = 1; i <= n; i++) {
		// A: map a fresh system BO, unbind the previous one: its out-fence waits for the invalidation
		const uint32_t abo = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);
		const uint64_t va = BQ_A_VA + (uint64_t)(i % 32) * 0x100000;
		struct drm_xe_vm_bind_op aops[2] = {opMap(abo, va, 0x10000), opUnmap(prevA, 0x10000)};
		syncReset(sa);
		*uf = 0;
		flush((void *)uf, 8);
		struct drm_xe_sync as[2] = {{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sa},
			{.type = DRM_XE_SYNC_TYPE_USER_FENCE, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .addr = (uint64_t)(uintptr_t)uf,
			 .timeline_value = 0x2000u + (uint64_t)i}};
		struct drm_xe_vm_bind ab = {.vm_id = gVm, .exec_queue_id = qa, .num_binds = prevA ? 2 : 1, .num_syncs = 2,
			.syncs = (uint64_t)(uintptr_t)as};
		if (prevA) ab.vector_of_binds = (uint64_t)(uintptr_t)aops; else ab.bind = aops[0];
		const int ea = xioctl(DRM_IOCTL_XE_VM_BIND, &ab) ? errno : 0;
		gemClose(abo);
		if (ea) { if (errs++ < 3) printf("bindq: A bind %d\n", ea); continue; }
		const int aPending = prevA && syncWait(sa, 0) == ETIME;
		// B right after: a system BO at its own address, nothing of its own to wait for
		if (nb < (int)(sizeof bBos / sizeof bBos[0])) {
			const uint32_t bbo = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);
			struct drm_xe_vm_bind_op bop = opMap(bbo, BQ_B_VA + (uint64_t)nb * 0x10000, 0x10000);
			syncReset(sb);
			struct drm_xe_sync bs = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sb};
			if (!bbo || bindOn(qb, &bop, 1, &bs, 1)) errs++;
			else {
				bBos[nb++] = bbo;
				const int bPending = syncWait(sb, 0) == ETIME;
				if (aPending) aPend++;
				if (aPending && bPending && syncWait(sa, 0) == ETIME) bPendWhileA++;
			}
		}
		// a no-op call and an EXEC without batches on A: never signaled before A's unbind
		syncReset(z);
		struct drm_xe_sync zs = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = z};
		if (bindOn(qa, NULL, 0, &zs, 1)) errs++;
		syncReset(e);
		struct drm_xe_sync es = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = e};
		struct drm_xe_exec x = {.exec_queue_id = qa, .num_syncs = 1, .syncs = (uint64_t)(uintptr_t)&es};
		if (xioctl(DRM_IOCTL_XE_EXEC, &x)) errs++;
		const int zDone = syncWait(z, 0) == 0, eDone = syncWait(e, 0) == 0, aDone = syncWait(sa, 0) == 0;
		if (zDone && !aDone) noOpEarly++;
		if (eDone && !aDone) execEarly++;
		if (syncWait(sa, 3000000000ull) || syncWait(z, 3000000000ull) || syncWait(e, 3000000000ull)) errs++;
		ufCheck(uf, 0x2000u + (uint64_t)i);
		prevA = va;
	}
	struct drm_xe_vm_bind_op last = opUnmap(prevA, 0x10000), ball = opUnmap(BQ_B_VA, (uint64_t)nb * 0x10000);
	if (prevA && bindOn(qa, &last, 1, NULL, 0)) errs++;
	if (nb && bindOn(qb, &ball, 1, NULL, 0)) errs++;
	for (int k = 0; k < nb; k++) gemClose(bBos[k]);
	queueDel(qa);
	queueDel(qb);
	printf("bindq: %d rounds in %.1f s: A's unbind pending at return %d, B's plain bind pending meanwhile %d (rb8e: 0; "
		"before, about every one), A's no-op call signaled before A's unbind %d, A's EXEC without batches %d, errors %d\n",
		n, (ns() - t0) / 1e9, aPend, bPendWhileA, noOpEarly, execEarly, errs);
	if (bPendWhileA || noOpEarly || execEarly || errs) FAIL("bindq (see the line above)");
	if (!aPend) printf("note: no unbind on A was pending at return (every invalidation answered at once): B unchecked\n");
}

// ─── rb8e item 1: the migrate windows (root) ───────────────────────────────────────────────────────────────────────────
static int gDispBufs;   // display buffers dispcheck copied (each twice through the surface window)
static void windows(int n) {
	static char buf[65536];
	int dc = 0, th = 0, eperm = 0;
	for (int i = 0; i < n; i++) {
		size_t len = sizeof buf - 1;
		if (!sysctlbyname("debug.intelxe.dispcheck", buf, &len, NULL, 0)) {
			dc++;
			buf[len < sizeof buf ? len : sizeof buf - 1] = 0;
			const char *p = strstr(buf, "display buffers: ");
			if (p) gDispBufs += atoi(p + 17);
		} else if (errno == EPERM) eperm++;
		len = sizeof buf;
		if (!sysctlbyname("debug.intelxefb_thumb", buf, &len, NULL, 0)) th++;
		else if (errno == EPERM) eperm++;
	}
	printf("windows: dispcheck read %d of %d (%d display buffers copied, twice each), intelxefb_thumb read %d of %d%s\n",
		dc, n, gDispBufs, th, n, eperm ? " (EPERM: run as root)" : "");
	if (eperm) FAIL("windows: needs root");
}

// ─── item 4 (optional) ───────────────────────────────────────────────────────────────────────────────────────────
static void hang(void) {
	const uint32_t qa = queueNew(DRM_XE_ENGINE_CLASS_RENDER), qb = queueNew(DRM_XE_ENGINE_CLASS_RENDER);
	const uint32_t sa = syncNew(), sb = syncNew();
	if (!qa || !qb || !sa || !sb) { FAIL("hang: queues"); return; }
	// A: at BATCH_VA + 0x1000, MI_BATCH_BUFFER_START to itself (no MI_ARB_CHECK); B: a store at BATCH_VA + 0x2000
	volatile uint32_t *a = batch + 0x400, *b = batch + 0x800;
	const uint64_t aVa = BATCH_VA + 0x1000, dst = RES_VA + 0x200;
	a[0] = MI_BBS; a[1] = (uint32_t)aVa; a[2] = (uint32_t)(aVa >> 32); a[3] = 0;
	b[0] = MI_SDI; b[1] = (uint32_t)dst; b[2] = (uint32_t)(dst >> 32); b[3] = 0x600dcafe; b[4] = MI_BBE; b[5] = 0;
	res[0x80] = 0;
	flush(res, 0x1000);
	flush(batch, 0x3000);
	struct drm_xe_sync s1 = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sa};
	struct drm_xe_sync s2 = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sb};
	if (exec(qa, aVa, &s1, 1)) { FAIL("hang: exec A %d", errno); return; }
	usleep(50000);   // A on the engine first
	const uint64_t t0 = ns();
	if (exec(qb, BATCH_VA + 0x2000, &s2, 1)) { FAIL("hang: exec B %d", errno); return; }
	const int eb = syncWait(sb, 30000000000ull);
	const double bt = (ns() - t0) / 1e9;
	flush(res, 0x1000);
	const int ea = syncWait(sa, 30000000000ull);
	struct drm_xe_exec_queue_get_property p = {.exec_queue_id = qa, .property = DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN};
	const int perr = xioctl(DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY, &p) ? errno : 0;
	printf("hang: B's job %s after %.2f s (value 0x%08x); A's fence %s, A banned %s\n", eb ? "NOT done" : "done", bt, res[0x80],
		ea ? "not signaled" : "signaled", perr ? "?" : p.value ? "yes" : "no");
	if (eb || res[0x80] != 0x600dcafe || ea || perr || !p.value) FAIL("hang");
	queueDel(qa);
	queueDel(qb);
}

int main(int argc, char **argv) {
	mach_timebase_info(&tb);
	int nf = 500, nq = 200, nbq = 300, nw = 0, doHang = 0;
	for (int i = 1; i < argc; i++) {
		if (!strncmp(argv[i], "fences=", 7)) nf = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "queues=", 7)) nq = atoi(argv[i] + 7);
		else if (!strncmp(argv[i], "bindq=", 6)) nbq = atoi(argv[i] + 6);
		else if (!strncmp(argv[i], "windows=", 8)) nw = atoi(argv[i] + 8);
		else if (!strcmp(argv[i], "hang")) doHang = 1;
		else { fprintf(stderr, "usage: %s [fences=N] [queues=N] [bindq=N] [windows=N] [hang]\n", argv[0]); return 2; }
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) { fprintf(stderr, "no IntelXe service\n"); return 1; }
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct drm_xe_vm_create vc = {.flags = DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE};
	if (xioctl(DRM_IOCTL_XE_VM_CREATE, &vc)) { fprintf(stderr, "vm create %d\n", errno); return 1; }
	gVm = vc.vm_id;
	const uint32_t bb = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB), rbo = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);
	batch = bb ? map(bb, 0x10000) : NULL;
	res = rbo ? map(rbo, 0x10000) : NULL;
	struct drm_xe_vm_bind_op m1 = opMap(bb, BATCH_VA, 0x10000), m2 = opMap(rbo, RES_VA, 0x10000);
	if (!batch || !res || bind(&m1, 1, NULL, 0) || bind(&m2, 1, NULL, 0)) { fprintf(stderr, "buffers %d\n", errno); return 1; }
	const uint32_t rq = queueNew(DRM_XE_ENGINE_CLASS_RENDER);
	if (!rq) { fprintf(stderr, "render queue %d\n", errno); return 1; }
	long long s0[S_N], s1[S_N];
	statsRead(s0);
	if (s0[S_UNBINDS] < 0) printf("note: no rb8d counters in debug.intelxe.stats (a kext before rb8d): deltas read -1\n");
	if (nf > 0) fences(nf, rq);
	if (nq > 0) queues(nq);
	if (nbq > 0) bindq(nbq, rq);
	if (nw > 0) windows(nw);
	queueDel(rq);
	if (doHang) hang();
	usleep(200000);
	statsRead(s1);
	printf("kext deltas: bind fences: unbinds %lld, pending at return %lld, calls without ops %lld; GGTT invalidations: GuC %lld, "
		"MMIO %lld, waited %lld, timed out %lld; TLB counted done unanswered %lld; CAT errors %lld, GuC context resets %lld "
		"(of a primary domain %lld), GT resets %lld (stuck after a reset %lld, never started %lld)\n",
		s1[S_UNBINDS] - s0[S_UNBINDS], s1[S_PEND] - s0[S_PEND], s1[S_NOOPS] - s0[S_NOOPS], s1[S_GGUC] - s0[S_GGUC],
		s1[S_GMMIO] - s0[S_GMMIO], s1[S_GWAIT] - s0[S_GWAIT], s1[S_GTO] - s0[S_GTO], s1[S_TLBFORCED] - s0[S_TLBFORCED],
		s1[S_CAT] - s0[S_CAT], s1[S_CTXRESET] - s0[S_CTXRESET], s1[S_DOMRESET] - s0[S_DOMRESET], s1[S_GTRESET] - s0[S_GTRESET],
		s1[S_STUCK] - s0[S_STUCK], s1[S_NOTSTARTED] - s0[S_NOTSTARTED]);
	printf("kext deltas (rb8e): bind user fences: signaled at return %lld, written asynchronously %lld of %lld queued, dropped "
		"%lld, waited in the ioctl %lld; polled here: there at return %d, later %d (longest %llu us), never %d, "
		"WAIT_USER_FENCE errors %d; bind queues made %lld, binds on one %lld, EXECs on one %lld; migrate windows sealed: "
		"move %lld, surface %lld, VRAM read %lld, awaited %lld, pages kept %lld/%lld/%lld; TLB fence timeout %lld ms\n",
		s1[S_UF_DIRECT] - s0[S_UF_DIRECT], s1[S_UF_ASYNC] - s0[S_UF_ASYNC], s1[S_UF_PINNED] - s0[S_UF_PINNED],
		s1[S_UF_DROPPED] - s0[S_UF_DROPPED], s1[S_UF_WAITED] - s0[S_UF_WAITED], gUfAtReturn, gUfLater,
		(unsigned long long)gUfMaxUs, gUfNever, gUfWaitErr, s1[S_BQ_MADE] - s0[S_BQ_MADE], s1[S_BQ_BINDS] - s0[S_BQ_BINDS],
		s1[S_BQ_EXECS] - s0[S_BQ_EXECS], s1[S_MW_MOVE] - s0[S_MW_MOVE], s1[S_MW_SURF] - s0[S_MW_SURF],
		s1[S_MW_READ] - s0[S_MW_READ], s1[S_MW_AWAITED] - s0[S_MW_AWAITED], s1[S_MW_KEPT_MOVE] - s0[S_MW_KEPT_MOVE],
		s1[S_MW_KEPT_SURF] - s0[S_MW_KEPT_SURF], s1[S_MW_KEPT_READ] - s0[S_MW_KEPT_READ], s1[S_TLB_TIMEOUT_MS]);
	if (gUfNever || gUfWaitErr) FAIL("bind user fences never written (%d) or not met by WAIT_USER_FENCE (%d)", gUfNever, gUfWaitErr);
	if (s0[S_UF_ASYNC] >= 0) {   // a kext with rb8e
		if (s1[S_TLB_TIMEOUT_MS] != 2250) FAIL("TLB fence timeout %lld ms, want 2250 (Linux, 4 KB H2G ring)", s1[S_TLB_TIMEOUT_MS]);
		if (gUfLater && s1[S_UF_ASYNC] - s0[S_UF_ASYNC] <= 0) FAIL("user fences arrived late but none written asynchronously");
		if (s1[S_UF_DROPPED] != s0[S_UF_DROPPED]) FAIL("bind user fences dropped");
		if (nbq > 0 && s1[S_BQ_MADE] - s0[S_BQ_MADE] < 2) FAIL("bind queues not counted");
		if (s1[S_MW_KEPT_MOVE] + s1[S_MW_KEPT_SURF] + s1[S_MW_KEPT_READ] !=
			s0[S_MW_KEPT_MOVE] + s0[S_MW_KEPT_SURF] + s0[S_MW_KEPT_READ]) FAIL("migrate window pages kept");
		if (s1[S_MW_SURF] - s0[S_MW_SURF] < 2 * gDispBufs) FAIL("migrate windows: %lld surface seals for %d display "
			"buffer copies", s1[S_MW_SURF] - s0[S_MW_SURF], 2 * gDispBufs);
		if (gDispBufs && s1[S_MW_AWAITED] - s0[S_MW_AWAITED] <= 0) FAIL("migrate windows: no seal awaited");
	} else {
		printf("note: no rb8e counters in debug.intelxe.stats (a kext before rb8e)\n");
	}
	if (s0[S_UNBINDS] >= 0) {
		if (nq > 0 && s1[S_GWAIT] - s0[S_GWAIT] < nq) FAIL("GGTT invalidations waited %lld for %d queues", s1[S_GWAIT] - s0[S_GWAIT], nq);
		if (s1[S_GTO] != s0[S_GTO]) FAIL("GGTT invalidation timeouts");
		if (s1[S_TLBFORCED] != s0[S_TLBFORCED]) FAIL("TLB invalidations the GuC did not answer");
		if (!doHang && s1[S_GTRESET] != s0[S_GTRESET]) FAIL("GT resets without the hang phase");
	}
	printf("RESULT: %s\n", fails ? "FAIL (see FAIL lines)" : "PASS");
	return fails ? 1 : 0;
}
