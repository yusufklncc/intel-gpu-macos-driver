// SPDX-License-Identifier: MIT
// bindstress: VM_BIND churn against running jobs, for the GuC MEMORY_CAT_ERROR hunt (docs/design/DESIGN-cat-fault.md).
// uAPI only, no Mesa, no Metal plugin. One scratch-page VM (as Mesa makes them), several exec queues (render, compute,
// with and without DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT; direct submission arms whichever the kext lets arm), and:
//   binder thread: over and over picks a slot and binds it (userptr from a host pool, pairs of userptr imports that share
//     their boundary page as the Metal plugin's unaligned imports do, system BOs, VRAM BOs in and outside the BAR window,
//     2 MB VRAM BOs at 2 MB-aligned VAs that become 2 MB leaves) or unbinds it (whole, or first a 64 KB hole in a 2 MB
//     leaf, which splits it). Slots sit in their own 2 MB (one level-0 table each), 1 GB (level 1) and 512 GB (level 2)
//     regions, so every whole unbind empties and frees page tables. VRAM BO handles are closed right after the bind:
//     the binding holds the BO, and the unbind frees its VRAM (window VRAM is shared with the page-table pages).
//   one submitter thread per queue: batches of MI_COPY_MEM_MEM reading random dwords of random slots (bound or not:
//     the scratch page answers an unbound one) and writing some into slots, plus copies of two never-unbound check
//     buffers (system memory and VRAM) into a result area the CPU verifies after the job; two jobs in flight per
//     queue, each a few ms, so binds and unbinds land while they run.
// It reports, against debug.intelxe.stats at the start: GuC context resets (CAT errors), GT resets, page faults, bans,
// TLB invalidations, deferred frees and those held by running jobs (rb8c), page-table pages / splits / detached /
// leaked; check-copy mismatches; exec errors (a banned queue is replaced and counted). A status line every 30 s, and
// "RESULT: PASS" or "RESULT: FAIL (...)" at the end (exit status 0 / 1).
//   clang -arch x86_64 -O2 -I../../../kernel/IntelXe/uapi -o bindstress bindstress.c -framework IOKit
//   ./bindstress [minutes=10] [queues=r,rl,c,cl] [huge|nohuge] [seed=N] [bindfence]
//     queues: r render, rl render + low-latency hint, c compute, cl compute + low-latency hint (comma separated)
//     huge (default): include the 2 MB leaf slots; nohuge: leave them out
//     bindfence: as Mesa, every VM_BIND signals the next point of one timeline syncobj and every EXEC waits for the last
//       point a bind has returned with; since rb8d an unbind's point holds its TLB invalidation, so EXECs depend on
//       invalidations in flight (the path debug.intelxe.tlb_inring changes; DESIGN-rb8-cost.md "tlbinring")
// Background for the night: nohup ./bindstress 60 > bindstress.log 2>&1 &
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <immintrin.h>
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
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
static void flush(volatile void *p, size_t len) {   // CPU writes out to memory before the GPU reads them
	for (size_t o = 0; o < len; o += 64) _mm_clflush((const char *)p + o);
	_mm_mfence();
}
static uint64_t rng(uint64_t *s) { *s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17; return *s; }

static uint32_t gem(uint64_t size, uint32_t placement, uint32_t flags, uint16_t caching) {
	struct drm_xe_gem_create g = {.size = size, .placement = placement, .flags = flags, .cpu_caching = caching};
	return xioctl(DRM_IOCTL_XE_GEM_CREATE, &g) ? 0 : g.handle;
}
static void gemClose(uint32_t h) { struct drm_gem_close c = {.handle = h}; xioctl(DRM_IOCTL_GEM_CLOSE, &c); }
static void *map(uint32_t handle, uint64_t size, uint32_t *type) {
	struct drm_xe_gem_mmap_offset m = {.handle = handle};
	if (xioctl(DRM_IOCTL_XE_GEM_MMAP_OFFSET, &m)) return NULL;
	mach_vm_address_t addr = 0; mach_vm_size_t sz = size;
	if (IOConnectMapMemory64(conn, (uint32_t)(m.offset >> 12), mach_task_self(), &addr, &sz, kIOMapAnywhere)) return NULL;
	if (type) *type = (uint32_t)(m.offset >> 12);
	return (void *)(uintptr_t)addr;
}
static uint32_t gVm;
// bindfence: the bind timeline (0: off), the last point a VM_BIND took and the last one a VM_BIND has returned with
// (what EXECs wait for; binds come from one thread at a time: main before the threads start, then the binder)
static uint32_t gBindTl;
static _Atomic uint64_t gBindPt, gBindDone;
static int bindOp(uint32_t op, uint32_t bo, uint64_t objOff, uint64_t addr, uint64_t range, uint16_t pat) {
	struct drm_xe_vm_bind b = {.vm_id = gVm, .num_binds = 1};
	b.bind.obj = bo; b.bind.obj_offset = objOff; b.bind.range = range; b.bind.addr = addr; b.bind.op = op;
	b.bind.pat_index = pat;
	struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = gBindTl};
	if (gBindTl) {
		s.timeline_value = atomic_fetch_add(&gBindPt, 1) + 1;
		b.num_syncs = 1;
		b.syncs = (uint64_t)(uintptr_t)&s;
	}
	const int r = xioctl(DRM_IOCTL_XE_VM_BIND, &b);
	if (gBindTl) {
		if (r) {   // the point was not signaled: signal it, so the timeline has no hole for later waits
			struct drm_syncobj_timeline_array ta = {.handles = (uint64_t)(uintptr_t)&gBindTl,
				.points = (uint64_t)(uintptr_t)&s.timeline_value, .count_handles = 1};
			const int e = errno;
			xioctl(DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL, &ta);
			errno = e;
		}
		atomic_store(&gBindDone, s.timeline_value);
	}
	return r;
}

// ─── debug.intelxe.stats ────────────────────────────────────────────────────────────────────────────────────────
enum { S_CTXRESET, S_GTRESET, S_PF, S_BANS, S_TLB, S_DEFER, S_PEND, S_HELD, S_PTPAGES, S_SPLITS, S_DETACH, S_LEAK, S_URO, S_N };
static const char *const kStatKey[S_N] = {"GuC context resets ", "GT resets ", "page faults ", "bans ", "TLB invalidations ",
	"deferred frees ", "pending ", "held by jobs ", "PT pages ", "splits ", "detached ", "leaked ", "userptr read-only "};
static void statsRead(long long *v) {
	static char b[16384];
	size_t len = sizeof(b) - 1;
	for (int i = 0; i < S_N; i++) v[i] = -1;
	if (sysctlbyname("debug.intelxe.stats", b, &len, NULL, 0)) return;
	b[len] = 0;
	// the page-table counters after "PT pages" ("leaked" is in the queue line too), pending after "deferred frees"
	const char *pt = strstr(b, "PT pages "), *df = strstr(b, "deferred frees ");
	for (int i = 0; i < S_N; i++) {
		const char *from = i >= S_PTPAGES ? pt : i == S_PEND || i == S_HELD ? df : b;
		const char *p = from ? strstr(from, kStatKey[i]) : NULL;
		if (p) v[i] = strtoll(p + strlen(kStatKey[i]), NULL, 10);
	}
}
static int sysInt(const char *name) {
	int v = -1;
	size_t l = sizeof v;
	return sysctlbyname(name, &v, &l, NULL, 0) ? -1 : v;
}

// ─── layout ─────────────────────────────────────────────────────────────────────────────────────────────────────
#define MI_SDI (0x20u << 23 | 2)          // MI_STORE_DATA_IMM, PPGTT, one dword
#define MI_CMM (0x2eu << 23 | 3)          // MI_COPY_MEM_MEM, PPGTT both (dst at dword 1, src at dword 3)
#define MI_BBE (0x0au << 23)              // MI_BATCH_BUFFER_END
enum { MAXQ = 4, NB = 2, BATCH_BYTES = 0x10000, OPS = 2000, CHK = 128, RES_BYTES = 0x1000 };
static const uint64_t BATCH_VA = 0x100000000ull, RES_VA = 0x200000000ull, CHECK_VA = 0x300000000ull,
	CHECKV_VA = 0x300100000ull, CHECK_BYTES = 0x10000;
static const uint64_t POOL_BYTES = 64ull << 20;

enum { K_USER, K_PAIR, K_SYS, K_VRAM, K_VRAMHI, K_HUGE };
static const char *const kKind[] = {"userptr", "pair", "sys", "vram", "vram-hi", "huge"};
typedef struct {
	uint64_t va, span;   // region base and the bytes reserved there
	int kind;
	int bound;           // 0 no, 1 yes, 2 huge with its hole already unbound
	uint64_t at, len;    // what is bound now (a pair: two ranges, at and at + span / 2)
} Slot;
enum { NSTD = 48, NFAR1G = 8, NFAR512G = 8, NHUGE = 8, NSLOT = NSTD + NFAR1G + NFAR512G + NHUGE };
static Slot slots[NSLOT];
static int nSlots;
static uint8_t *pool;
static pthread_mutex_t slotLock = PTHREAD_MUTEX_INITIALIZER;
static atomic_int stop;
static atomic_ullong nBinds, nUnbinds, nHoles, nBindFails, nUnbindFails, nJobs, nMismatch, nExecErr, nWaitTimeouts,
	nQueueRedo;

static void slotsInit(int huge) {
	int k = 0;
	for (int i = 0; i < NSTD; i++, k++)   // own level-0 table each
		slots[k] = (Slot){.va = 0x1000000000ull + (uint64_t)i * 0x200000, .span = 0x200000,
			.kind = (int[]){K_USER, K_PAIR, K_SYS, K_VRAM, K_VRAMHI}[i % 5]};
	for (int i = 0; i < NFAR1G; i++, k++)  // own level-1 table each
		slots[k] = (Slot){.va = 0x10000000000ull + (uint64_t)i * 0x40000000ull, .span = 0x200000, .kind = i & 1 ? K_VRAM : K_USER};
	for (int i = 0; i < NFAR512G; i++, k++)   // own level-2 table each
		slots[k] = (Slot){.va = 0x200000000000ull + (uint64_t)i * (512ull << 30), .span = 0x200000, .kind = i & 1 ? K_PAIR : K_VRAMHI};
	if (huge)
		for (int i = 0; i < NHUGE; i++, k++)   // 2 MB leaves (VRAM, 2 MB at a 2 MB-aligned VA)
			slots[k] = (Slot){.va = 0x2000000000ull + (uint64_t)i * 0x400000, .span = 0x200000, .kind = K_HUGE};
	nSlots = k;
}

// Binds slot s (slotLock not held: binds may wait). Returns 0 or errno.
static int slotBind(Slot *s, uint64_t *seed) {
	const uint64_t r = rng(seed);
	if (s->kind == K_USER || s->kind == K_PAIR) {
		const uint64_t len = (((r >> 8) % 64) + 1) * 0x1000;                       // 4 KB .. 256 KB
		const uint64_t off = ((r >> 20) % ((POOL_BYTES - 2 * len) / 0x1000)) * 0x1000;   // anywhere in the pool: pages shared
		// inside the region's first 512 KB: a pair's second import goes at + 1 MB, clear of the first (<= 256 KB long)
		const uint64_t at = s->va + ((r >> 40) % 8) * 0x10000;
		if (s->kind == K_USER) {
			if (bindOp(DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, (uint64_t)(uintptr_t)(pool + off), at, len, 1)) return errno;
		} else {   // two imports whose ranges share their boundary page, as an unaligned host pointer imported whole
			if (bindOp(DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, (uint64_t)(uintptr_t)(pool + off), at, len, 1)) return errno;
			if (bindOp(DRM_XE_VM_BIND_OP_MAP_USERPTR, 0, (uint64_t)(uintptr_t)(pool + off + len - 0x1000), s->va + 0x100000,
				len, 1)) {
				const int e = errno;
				bindOp(DRM_XE_VM_BIND_OP_UNMAP, 0, 0, at, len, 0);
				return e;
			}
		}
		s->at = at;
		s->len = len;
		return 0;
	}
	uint64_t len;
	uint32_t place = 2, flags = 0;
	uint16_t caching = DRM_XE_GEM_CPU_CACHING_WC;
	if (s->kind == K_HUGE) len = 0x200000;
	else len = (((r >> 8) % 16) + 1) * 0x10000;   // 64 KB .. 1 MB
	if (s->kind == K_SYS) { place = 1; caching = DRM_XE_GEM_CPU_CACHING_WB; }
	if (s->kind == K_VRAMHI) flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;   // in the BAR window, with the page tables
	const uint32_t h = gem(len, place, flags, caching);
	if (!h) return errno;
	const uint64_t at = s->kind == K_HUGE ? s->va : s->va + ((r >> 40) % 16) * 0x10000;
	const int err = bindOp(DRM_XE_VM_BIND_OP_MAP, h, 0, at, len, 2) ? errno : 0;
	gemClose(h);   // the binding holds it: the unbind frees its memory
	if (err) return err;
	s->at = at;
	s->len = len;
	return 0;
}

static int slotUnbind(Slot *s, uint64_t *seed) {
	if (s->kind == K_HUGE && s->bound == 1 && (rng(seed) & 1)) {   // a 64 KB hole first: the 2 MB leaf splits
		const uint64_t hole = s->at + ((rng(seed) % 30) + 1) * 0x10000;
		if (bindOp(DRM_XE_VM_BIND_OP_UNMAP, 0, 0, hole, 0x10000, 0)) return errno;
		nHoles++;
		return -1;   // still bound (the rest)
	}
	if (bindOp(DRM_XE_VM_BIND_OP_UNMAP, 0, 0, s->at, s->len, 0)) return errno;
	if (s->kind == K_PAIR && bindOp(DRM_XE_VM_BIND_OP_UNMAP, 0, 0, s->va + 0x100000, s->len, 0)) return errno;
	return 0;
}

static void *binder(void *arg) {
	uint64_t seed = (uint64_t)(uintptr_t)arg | 1;
	while (!atomic_load(&stop)) {
		Slot *s = &slots[rng(&seed) % (uint64_t)nSlots];
		pthread_mutex_lock(&slotLock);
		const int bound = s->bound;
		pthread_mutex_unlock(&slotLock);
		// bind/unbind under no lock: the submitters only read the slot table to pick addresses
		if (!bound) {
			const int e = slotBind(s, &seed);
			pthread_mutex_lock(&slotLock);
			if (!e) { s->bound = 1; nBinds++; }
			else if (nBindFails++ < 10) printf("bind %s at 0x%llx: errno %d\n", kKind[s->kind], s->va, e);
			pthread_mutex_unlock(&slotLock);
		} else {
			const int e = slotUnbind(s, &seed);
			pthread_mutex_lock(&slotLock);
			if (e == -1) s->bound = 2;
			else if (!e) { s->bound = 0; nUnbinds++; }
			else if (nUnbindFails++ < 10) printf("unbind %s at 0x%llx: errno %d\n", kKind[s->kind], s->at, e);
			pthread_mutex_unlock(&slotLock);
		}
		if ((rng(&seed) & 7) == 0) usleep(200);   // some quiet moments: queues may arm and leave
	}
	return NULL;
}

// ─── submitters ─────────────────────────────────────────────────────────────────────────────────────────────────
typedef struct {
	int idx;
	uint16_t cls;
	int lowLatency;
	const char *name;
	uint32_t queue, sync[NB];
	volatile uint32_t *batch, *res;
	uint64_t jobs;
} Q;
static Q qs[MAXQ];
static int nq;
static volatile uint32_t *checkSys, *checkVram;

static int queueMake(Q *q) {
	struct drm_xe_engine_class_instance ei = {.engine_class = q->cls};
	struct drm_xe_exec_queue_create c = {.width = 1, .num_placements = 1, .vm_id = gVm, .instances = (uint64_t)(uintptr_t)&ei,
		.flags = q->lowLatency ? DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT : 0};
	if (xioctl(DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &c)) return errno;
	q->queue = c.exec_queue_id;
	return 0;
}
static void queueDrop(Q *q) { struct drm_xe_exec_queue_destroy d = {.exec_queue_id = q->queue}; xioctl(DRM_IOCTL_XE_EXEC_QUEUE_DESTROY, &d); }

static uint32_t chkWord(int i, int vram) { return 0x5a000000u ^ (uint32_t)i * 2654435761u ^ (vram ? 0xffu : 0); }

// Writes batch b of q: OPS copies over the slots (reads; every 8th a write into a slot), then CHK copies of each check
// buffer into the result area, a store of the job number, the end.
static void batchWrite(Q *q, int b, uint64_t *seed, uint32_t job) {
	volatile uint32_t *p = q->batch + (size_t)b * (BATCH_BYTES / 4);
	const uint64_t res = RES_VA + (uint64_t)q->idx * 0x100000 + (uint64_t)b * RES_BYTES;
	uint32_t n = 0;
	pthread_mutex_lock(&slotLock);
	for (int i = 0; i < OPS; i++) {
		const Slot *s = &slots[rng(seed) % (uint64_t)nSlots];
		const uint64_t a = s->va + (rng(seed) % (s->span / 4)) * 4;   // anywhere in the region, bound or not
		const uint64_t scratch = res + 0x800 + (i % 64) * 4;          // reads land in the second half of the result area
		const int write = (i & 7) == 7;
		p[n++] = MI_CMM;
		p[n++] = (uint32_t)(write ? a : scratch); p[n++] = (uint32_t)((write ? a : scratch) >> 32);
		p[n++] = (uint32_t)(write ? scratch : a); p[n++] = (uint32_t)((write ? scratch : a) >> 32);
	}
	pthread_mutex_unlock(&slotLock);
	for (int i = 0; i < CHK; i++) {
		const int v = i & 1;
		const uint64_t src = (v ? CHECKV_VA : CHECK_VA) + (uint64_t)(rng(seed) % (CHECK_BYTES / 4)) * 4;
		const uint64_t dst = res + (uint64_t)i * 8;
		p[n++] = MI_CMM; p[n++] = (uint32_t)dst; p[n++] = (uint32_t)(dst >> 32); p[n++] = (uint32_t)src; p[n++] = (uint32_t)(src >> 32);
		p[n++] = MI_SDI; p[n++] = (uint32_t)(dst + 4); p[n++] = (uint32_t)((dst + 4) >> 32);
		p[n++] = (uint32_t)((src - (v ? CHECKV_VA : CHECK_VA)) / 4 | (uint32_t)v << 31);   // which word, for the check
	}
	p[n++] = MI_SDI; p[n++] = (uint32_t)(res + 0x7fc); p[n++] = (uint32_t)((res + 0x7fc) >> 32); p[n++] = job;
	p[n++] = MI_BBE;
	if (n & 1) p[n++] = 0;
	flush(p, n * 4);
}

static int jobCheck(Q *q, int b, uint32_t job) {
	volatile uint32_t *r = q->res + (size_t)b * (RES_BYTES / 4);
	int bad = 0;
	if (r[0x7fc / 4] != job) bad++;
	for (int i = 0; i < CHK; i++) {
		const uint32_t tag = r[i * 2 + 1], word = tag & 0x7fffffff;
		const int v = (int)(tag >> 31);
		if (word >= CHECK_BYTES / 4 || r[i * 2] != chkWord((int)word, v)) {
			if (bad++ < 3)
				printf("%s job %u: check %d: %s word %u read 0x%08x, expected 0x%08x\n", q->name, job, i, v ? "VRAM" : "system",
					word, r[i * 2], word < CHECK_BYTES / 4 ? chkWord((int)word, v) : 0);
		}
	}
	return bad;
}

static int waitSync(uint32_t h, uint64_t timeoutNs) {
	struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&h, .count_handles = 1, .timeout_nsec = (int64_t)(ns() + timeoutNs)};
	return xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w) ? errno : 0;
}

static void *submitter(void *arg) {
	Q *q = arg;
	uint64_t seed = 0x9e3779b97f4a7c15ull * (uint64_t)(q->idx + 1);
	uint32_t job = 0, inflight[NB] = {0};
	for (int b = 0;; b = (b + 1) % NB) {
		if (inflight[b]) {   // this slot's previous job: wait, then check what it copied
			const int e = waitSync(q->sync[b], 15000000000ull);
			if (e) { nWaitTimeouts++; printf("%s job %u: wait errno %d\n", q->name, inflight[b], e); }
			else if (jobCheck(q, b, inflight[b])) nMismatch++;
			inflight[b] = 0;
		}
		if (atomic_load(&stop)) {
			int busy = 0;
			for (int k = 0; k < NB; k++) busy |= inflight[k] != 0;
			if (!busy) break;
			continue;
		}
		volatile uint32_t *r = q->res + (size_t)b * (RES_BYTES / 4);
		for (int i = 0; i < RES_BYTES / 4; i++) r[i] = 0;
		flush(r, RES_BYTES);
		batchWrite(q, b, &seed, ++job);
		struct drm_syncobj_array rs = {.handles = (uint64_t)(uintptr_t)&q->sync[b], .count_handles = 1};
		xioctl(DRM_IOCTL_SYNCOBJ_RESET, &rs);
		struct drm_xe_sync s[2] = {{.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = q->sync[b]},
			{.type = DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, .handle = gBindTl, .timeline_value = atomic_load(&gBindDone)}};
		const uint32_t ns_ = gBindTl && s[1].timeline_value ? 2 : 1;   // bindfence: after the last bind, as Mesa
		struct drm_xe_exec x = {.exec_queue_id = q->queue, .num_syncs = ns_, .syncs = (uint64_t)(uintptr_t)s,
			.address = BATCH_VA + (uint64_t)q->idx * 0x1000000 + (uint64_t)b * BATCH_BYTES, .num_batch_buffer = 1};
		if (xioctl(DRM_IOCTL_XE_EXEC, &x)) {
			const int e = errno;
			nExecErr++;
			printf("%s: exec errno %d%s\n", q->name, e, e == ECANCELED ? " (queue banned: replaced)" : "");
			for (int k = 0; k < NB; k++)
				if (inflight[k]) { waitSync(q->sync[k], 15000000000ull); inflight[k] = 0; }
			queueDrop(q);
			if (queueMake(q)) { printf("%s: no new queue: errno %d\n", q->name, errno); break; }
			nQueueRedo++;
			continue;
		}
		inflight[b] = job;
		q->jobs++;
		nJobs++;
	}
	return NULL;
}

int main(int argc, char **argv) {
	mach_timebase_info(&tb);
	const double minutes = argc > 1 ? atof(argv[1]) : 10;
	const char *qspec = argc > 2 ? argv[2] : "r,rl,c,cl";
	int huge = 1, bindFence = 0;
	uint64_t seed0 = 0x12345678;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "bindfence")) bindFence = 1;
		else if (!strcmp(argv[i], "nohuge")) huge = 0;
		else if (!strcmp(argv[i], "huge")) huge = 1;
		else if (!strncmp(argv[i], "seed=", 5)) seed0 = strtoull(argv[i] + 5, NULL, 0);
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) { fprintf(stderr, "no IntelXe service\n"); return 1; }
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct drm_xe_vm_create vc = {.flags = DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE};
	if (xioctl(DRM_IOCTL_XE_VM_CREATE, &vc)) { fprintf(stderr, "vm create %d\n", errno); return 1; }
	gVm = vc.vm_id;
	if (bindFence) {
		struct drm_syncobj_create sc = {0};
		if (xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc)) { fprintf(stderr, "bind timeline %d\n", errno); return 1; }
		gBindTl = sc.handle;
	}

	// the host pool userptr slots import from (never unmapped: the kext keeps unbound pages wired until their deferred free)
	pool = mmap(NULL, POOL_BYTES, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE, -1, 0);
	if (pool == MAP_FAILED) { fprintf(stderr, "pool\n"); return 1; }
	for (uint64_t i = 0; i < POOL_BYTES / 4; i++) ((uint32_t *)pool)[i] = 0xc0de0000u ^ (uint32_t)i;

	// check buffers: system memory and VRAM in the window, a known word each, never unbound
	const uint32_t cs = gem(CHECK_BYTES, 1, 0, DRM_XE_GEM_CPU_CACHING_WB),
		cv = gem(CHECK_BYTES, 2, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM, DRM_XE_GEM_CPU_CACHING_WC);
	checkSys = cs ? map(cs, CHECK_BYTES, NULL) : NULL;
	checkVram = cv ? map(cv, CHECK_BYTES, NULL) : NULL;
	if (!checkSys || !checkVram) { fprintf(stderr, "check buffers\n"); return 1; }
	for (int i = 0; i < (int)(CHECK_BYTES / 4); i++) { checkSys[i] = chkWord(i, 0); checkVram[i] = chkWord(i, 1); }
	flush(checkSys, CHECK_BYTES);
	_mm_sfence();
	if (bindOp(DRM_XE_VM_BIND_OP_MAP, cs, 0, CHECK_VA, CHECK_BYTES, 2) || bindOp(DRM_XE_VM_BIND_OP_MAP, cv, 0, CHECKV_VA, CHECK_BYTES, 2)) {
		fprintf(stderr, "check bind %d\n", errno);
		return 1;
	}

	// queues
	char spec[64];
	strlcpy(spec, qspec, sizeof spec);
	for (char *t = strtok(spec, ","); t && nq < MAXQ; t = strtok(NULL, ",")) {
		Q *q = &qs[nq];
		q->idx = nq;
		if (!strcmp(t, "r")) { q->cls = DRM_XE_ENGINE_CLASS_RENDER; q->name = "render"; }
		else if (!strcmp(t, "rl")) { q->cls = DRM_XE_ENGINE_CLASS_RENDER; q->lowLatency = 1; q->name = "render+ll"; }
		else if (!strcmp(t, "c")) { q->cls = DRM_XE_ENGINE_CLASS_COMPUTE; q->name = "compute"; }
		else if (!strcmp(t, "cl")) { q->cls = DRM_XE_ENGINE_CLASS_COMPUTE; q->lowLatency = 1; q->name = "compute+ll"; }
		else { fprintf(stderr, "queue kind %s: r, rl, c or cl\n", t); return 1; }
		const uint32_t bb = gem(NB * BATCH_BYTES, 1, 0, DRM_XE_GEM_CPU_CACHING_WB), rb = gem(0x100000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);
		q->batch = bb ? map(bb, NB * BATCH_BYTES, NULL) : NULL;
		q->res = rb ? map(rb, 0x100000, NULL) : NULL;
		if (!q->batch || !q->res || bindOp(DRM_XE_VM_BIND_OP_MAP, bb, 0, BATCH_VA + (uint64_t)nq * 0x1000000, NB * BATCH_BYTES, 2) ||
			bindOp(DRM_XE_VM_BIND_OP_MAP, rb, 0, RES_VA + (uint64_t)nq * 0x100000, 0x100000, 2)) {
			fprintf(stderr, "%s: buffers %d\n", q->name, errno);
			return 1;
		}
		for (int k = 0; k < NB; k++) {
			struct drm_syncobj_create sc = {0};
			if (xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc)) { fprintf(stderr, "syncobj %d\n", errno); return 1; }
			q->sync[k] = sc.handle;
		}
		if (queueMake(q)) { fprintf(stderr, "%s: queue %d\n", q->name, errno); return 1; }
		nq++;
	}
	slotsInit(huge);

	long long s0[S_N], s1[S_N];
	statsRead(s0);
	printf("bindstress: %.1f min, queues %s, %d slots%s, seed 0x%llx%s\n", minutes, qspec, nSlots, huge ? " (2 MB leaves too)" : "",
		seed0, bindFence ? ", EXECs wait on the bind timeline (bindfence)" : "");
	printf("kext: direct %d, direct_idle_us %d, hugepages %d, tlb_every_batch %d, range_inval %d, unbind_async %d, "
		"exec_tlb_dep %d, tlb_inring %d%s\n",
		sysInt("debug.intelxe.direct"), sysInt("debug.intelxe.direct_idle_us"), sysInt("debug.intelxe.hugepages"),
		sysInt("debug.intelxe.tlb_every_batch"), sysInt("debug.intelxe.range_inval"), sysInt("debug.intelxe.unbind_async"),
		sysInt("debug.intelxe.exec_tlb_dep"), sysInt("debug.intelxe.tlb_inring"),
		s0[S_HELD] < 0 ? " (no 'held by jobs' counter: a kext before rb8c)" : "");

	pthread_t bt, st[MAXQ];
	pthread_create(&bt, NULL, binder, (void *)(uintptr_t)seed0);
	for (int i = 0; i < nq; i++) pthread_create(&st[i], NULL, submitter, &qs[i]);
	const uint64_t t0 = ns(), end = t0 + (uint64_t)(minutes * 60e9);
	uint64_t next = t0 + 30000000000ull;
	while (ns() < end) {
		usleep(200000);
		if (ns() < next) continue;
		next += 30000000000ull;
		statsRead(s1);
		printf("[%5.1f min] binds %llu unbinds %llu holes %llu jobs %llu | CAT/ctx resets +%lld GT resets +%lld page faults +%lld "
			"bans +%lld | TLB +%lld, deferred +%lld (pending %lld, held by jobs +%lld) | PT pages %lld splits +%lld detached +%lld "
			"leaked +%lld | mismatches %llu exec errors %llu\n", (ns() - t0) / 60e9, nBinds, nUnbinds, nHoles, nJobs,
			s1[S_CTXRESET] - s0[S_CTXRESET], s1[S_GTRESET] - s0[S_GTRESET], s1[S_PF] - s0[S_PF], s1[S_BANS] - s0[S_BANS],
			s1[S_TLB] - s0[S_TLB], s1[S_DEFER] - s0[S_DEFER], s1[S_PEND], s1[S_HELD] - s0[S_HELD], s1[S_PTPAGES],
			s1[S_SPLITS] - s0[S_SPLITS], s1[S_DETACH] - s0[S_DETACH], s1[S_LEAK] - s0[S_LEAK], nMismatch, nExecErr);
	}
	atomic_store(&stop, 1);
	pthread_join(bt, NULL);
	for (int i = 0; i < nq; i++) pthread_join(st[i], NULL);
	// everything unbound: every table of the churn regions goes back
	for (int i = 0; i < nSlots; i++) {
		uint64_t sd = 1;
		while (slots[i].bound) {
			const int e = slotUnbind(&slots[i], &sd);
			if (e > 0) break;
			slots[i].bound = e == -1 ? 2 : 0;
		}
	}
	for (int i = 0; i < nq; i++) queueDrop(&qs[i]);
	usleep(500000);
	statsRead(s1);
	const long long cat = s1[S_CTXRESET] - s0[S_CTXRESET], gtr = s1[S_GTRESET] - s0[S_GTRESET], pf = s1[S_PF] - s0[S_PF],
		bans = s1[S_BANS] - s0[S_BANS];
	printf("done: %.1f min, binds %llu, unbinds %llu (holes %llu), bind failures %llu, unbind failures %llu; jobs %llu:",
		(ns() - t0) / 60e9, nBinds, nUnbinds, nHoles, nBindFails, nUnbindFails, nJobs);
	for (int i = 0; i < nq; i++) printf(" %s %llu", qs[i].name, qs[i].jobs);
	printf("\nkext deltas: GuC context resets (CAT) %lld, GT resets %lld, page faults %lld, bans %lld, TLB invalidations %lld, "
		"deferred frees %lld (held by jobs %lld), splits %lld, detached %lld, leaked %lld, userptr read-only %lld; pending "
		"deferred now %lld, PT pages now %lld\n", cat, gtr, pf, bans, s1[S_TLB] - s0[S_TLB], s1[S_DEFER] - s0[S_DEFER],
		s1[S_HELD] - s0[S_HELD], s1[S_SPLITS] - s0[S_SPLITS], s1[S_DETACH] - s0[S_DETACH], s1[S_LEAK] - s0[S_LEAK],
		s1[S_URO] - s0[S_URO], s1[S_PEND], s1[S_PTPAGES]);
	const int fail = cat || gtr || pf || bans || nMismatch || nExecErr || nWaitTimeouts;
	if (fail)
		printf("RESULT: FAIL (CAT/context resets %lld, GT resets %lld, page faults %lld, bans %lld, check mismatches %llu, exec "
			"errors %llu, wait timeouts %llu; queues replaced %llu): see the G2H log (sudo sysctl -n debug.intelxe.g2h)\n", cat, gtr, pf, bans,
			nMismatch, nExecErr, nWaitTimeouts, nQueueRedo);
	else
		printf("RESULT: PASS\n");
	return fail;
}
