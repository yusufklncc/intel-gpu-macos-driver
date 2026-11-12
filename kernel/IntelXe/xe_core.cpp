// SPDX-License-Identifier: MIT
// The xe device behind IntelXeClient: bring-up, buffer objects, VMs with PPGTT, exec queues on GuC, syncobjs and the
// xe/DRM ioctls Mesa's ANV issues. Semantics follow Linux drivers/gpu/drm/xe (xe_query.c, xe_bo.c, xe_vm.c, xe_pt.c,
// xe_exec_queue.c, xe_exec.c, xe_ring_ops.c, xe_guc_submit.c, xe_guc_ct.c, xe_guc_id_mgr.c, xe_irq.c) and
// drivers/gpu/drm/drm_syncobj.c, cut down:
//  - binds are CPU writes of the page tables through BAR2 (in-fences of a bind are waited for on the CPU: Mesa only
//    passes them for sparse binds);
//  - an exec returns at once: its job waits in the queue's FIFO for its in-fences, as drm_sched's entity does, and the
//    submit worker (a thread call kicked by the interrupt) puts it in the ring when they have signaled;
//  - G2H is read by the interrupt handler (both GuCs), which completes TLB invalidations, context deregistration and
//    resets and wakes the waiters; waiters sleep on one wake generation and fall back to polling only if an interrupt
//    is missed (counted in debug.intelxe.stats);
//  - job completion is a seqno the ring writes after the batch.
//
// Locks, outer to inner (an inner lock is never held while taking an outer one):
//   client lock (XeClientState::lock: the client's tables, VMs, syncobjs; dropped while a client waits)
//   gClientsLock (client list; xeScanoutPin takes it before a client lock: see xeScanoutPin)
//   gDevLock (bring-up, media bring-up, debug steps)
//   gSchedLock (queue rings, job FIFOs, guc_id -> queue map) -> gResLock (allocators, deferred frees, reaper list)
//   leaves: gTlbLock -> gH2gLock[gt]; gG2hLock[gt], gFwLock, gWaitLock, gSayLock, gDispLock
//   rb7s: gNameLock (GEM_FLINK names, a BO's last handle) -> gBoLock (a spin lock: every BO's cpuMaps list and purgeable
//   counters, which several clients reach once a BO is shared). A thread that holds a client lock may take another
//   client's lock only with a try-lock (evictWindow, purgeAll, moveSetBegin); a BO move that rewrites other clients' VMs
//   holds their locks for the whole move (it then does not drop its own either: xeSuspend locks every client in turn).
//   rb7t: gSyncLock (leaf: the contents of a syncobj that was exported to an fd, which several clients' handles then
//   reach, each under its own client lock; a syncobj never exported keeps the client lock alone) and xe_fd.cpp's gFdLock
//   (leaf: the object fd records; taken under client locks and gClientsLock, nothing but reference increments inside).
#include "IntelXe.h"
#include "lrc.h"
#include "xe_vram.h"
#include <IOKit/IOUserClient.h>
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <kern/thread.h>   // rb7q: current_thread (who is making a backing)
#include <libkern/OSAtomic.h>
#include <sys/errno.h>
#include <sys/proc.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include "uapi/xe_drm.h"
#include "uapi/intelxe_drm.h"
#include "uapi/intelxe_surfvram_kabi.h"
#include "xe_fd.h"
#include "xe_uapi_args.h"
#include "xe_tlbinval.h"
#include "xe_recovery.h"

static IOLock *gResLock, *gSchedLock, *gClientsLock, *gTlbLock, *gWaitLock;
static IOLock *gFaultLock;   // rb7j: gFaultVms, every XeVm::faults, gFaultRing (taken alone, never with another lock)
static IOLock *gNameLock;    // rb7s: gNames and XeBo::name, a BO's handle count reaching 0 (drm_device object_name_lock)
static IOSimpleLock *gBoLock;   // rb7s: XeBo::cpuMaps lists, XeBo purgeable counters and state (short, no calls inside)
static IOLock *gSyncLock;       // rb7t: the contents of shared (fd-exported) syncobjs (XeSyncobj::shared)
static thread_call_t gWorkCall, gReapCall, gWatchCall;
static bool gWatchArmed;   // gSchedLock
// GT reset (xeGtReset): while set, nothing is put in a ring or sent to the GuC; requested from G2H (GuC exception, CT
// broken) and run by the watchdog thread call.
static volatile bool gResetting, gResetReq;
static volatile bool gSuspended;   // system sleep: user submission held (xeSuspend / xeResume)
static volatile bool gWedged;      // xe_device_declare_wedged: the GPU could not be brought back; execs fail
static const char *gResetWhy;
static void requestReset(const char *why) {
	if (gResetReq) return;
	gResetWhy = why;
	gResetReq = true;
	if (gWatchCall) thread_call_enter(gWatchCall);
}
static volatile uint32_t gWakeGen;
IntelXeStats gStats;

// ─── small growable handle table (handle = index + 1) ─────────────────────────────────────────────────────────────
struct Table {
	void **p {nullptr};
	uint32_t n {0};
	uint32_t add(void *o) {
		for (uint32_t i = 0; i < n; i++)
			if (!p[i]) {
				p[i] = o;
				return i + 1;
			}
		uint32_t nn = n ? n * 2 : 64;
		auto **q = static_cast<void **>(IOMallocZero(nn * sizeof(void *)));
		if (!q) return 0;
		if (p) {
			memcpy(q, p, n * sizeof(void *));
			IOFree(p, n * sizeof(void *));
		}
		p = q;
		q[n] = o;
		uint32_t h = n + 1;
		n = nn;
		return h;
	}
	void *get(uint32_t h) { return h && h <= n ? p[h - 1] : nullptr; }
	uint32_t find(const void *o) const {   // the handle of o, 0 if none (drm_prime_lookup_buf_handle's question)
		for (uint32_t i = 0; i < n; i++)
			if (p[i] == o) return i + 1;
		return 0;
	}
	void *take(uint32_t h) {
		void *o = get(h);
		if (o) p[h - 1] = nullptr;
		return o;
	}
	void free() {
		if (p) IOFree(p, n * sizeof(void *));
		p = nullptr;
		n = 0;
	}
};

// ─── ID allocators (xe_guc_id_mgr.c: a bitmap per GuC; ASIDs: xe_vm.c xa_alloc_cyclic over [1, XE_MAX_ASID)) ──────
// Bits [lo, hi) of a bitmap, next-fit from *next when cyclic (ASIDs, so a freed ASID is not handed out again at once),
// lowest-fit otherwise. gResLock held.
static uint32_t idAlloc(uint64_t *bits, uint32_t lo, uint32_t hi, uint32_t *next) {
	const uint32_t span = hi - lo, start = next && *next >= lo && *next < hi ? *next : lo;
	for (uint32_t k = 0; k < span; k++) {
		const uint32_t id = lo + (start - lo + k) % span;
		if (bits[id / 64] & 1ull << (id % 64)) continue;
		bits[id / 64] |= 1ull << (id % 64);
		if (next) *next = id + 1;
		return id;
	}
	return ~0u;
}
static void idFree(uint64_t *bits, uint32_t id) { bits[id / 64] &= ~(1ull << (id % 64)); }

// guc_ids per GuC: 0-15 stay for the bring-up steps' test contexts. Slots: an exec queue's 32 KB LRC window in the GGTT
// (lrcPoolG) and its seqno qword (seqnoG) share one index. ASIDs: 1 is step 9's, kernel VMs use 0xffff0 and up.
static constexpr uint32_t kMaxGucIds = 4096, kGucIdFirst = 16, kSlots = 4096, kAsidFirst = 2, kAsidEnd = 65536;
static uint64_t gGucIdBits[2][kMaxGucIds / 64], gSlotBits[kSlots / 64], gAsidBits[kAsidEnd / 64];
static uint32_t gAsidNext = kAsidFirst;
static volatile uint32_t gSlotGen[kSlots];
// The ban floor (queueBan): a banned queue's slot seqno is set to the last seqno it handed out, but the ring may still
// write an older one afterwards (a job that was on the hardware when the ban came). Linux's fences are software
// objects and cannot go back to unsignaled; here every fence of that queue generation at or below the floor reads as
// signaled whatever the seqno memory says, so a ban can never strand a waiter.
static volatile uint32_t gSlotBanGen[kSlots], gSlotBanSeq[kSlots];
// G2H state per guc_id (written by the G2H reader, read by the queue teardown): schedule-disable done, deregister done.
static volatile uint8_t gGucState[2][kMaxGucIds];
static constexpr uint8_t kSchedOff = 1, kDeregDone = 2;
// A schedule disable the job watchdog waits for (XeQueue::tdr): SCHED_CONTEXT_MODE_DONE runs the watchdog at once
// instead of at its next 500 ms tick. Its own array: gGucState is written by the G2H reader without gSchedLock.
static volatile uint8_t gTdrWait[2][kMaxGucIds];
// Ban requests from G2H (CONTEXT_RESET_NOTIFICATION, MEMORY_CAT_ERROR), applied by the submit worker.
static volatile uint32_t gBanReq[2][kMaxGucIds / 32];
// rb8d (xe_recovery.h): when the GuC last reset a context of each primary-GT reset domain (xeResetDomain; 0: not since the
// last GT reset), and debug.intelxe.stats' "recovery" counters: CAT errors, GuC context resets taken into a domain,
// GT resets for a job stuck behind a reset context (xeStuckAfterReset), queues taken off instead (third time),
// GT resets for a job that never started within Linux's 5 s (xeNotStartedLimitMs)
static uint64_t gDomResetNs[16];
static struct { uint32_t catErrors, domResets, stuckResets, stuckOff, notStartedResets; } gRecSt;
static inline uint32_t gtIdx(uint32_t gt) { return gt ? 1 : 0; }

// ─── device-wide state ─────────────────────────────────────────────────────────────────────────────────────────────
static uint64_t vramVisible = 256ull << 20;   // the BAR2 window, set from its length at bring-up (ResizeAppleGpuBars 0 = 256 MB, 10 = 1 GB)
static constexpr uint64_t vramUserStart = 32ull << 20;   // below: firmware framebuffer, IntelXeFB's cursor (31 MB)
// End of the VRAM BOs may use: below flat CCS storage, GSM and DSM (xe_vram.c tile_vram_size), set at bring-up.
static uint64_t vramEnd = 0x2c0000000ull;
static int scratchInit();
static int recordGolden();
static void vramClearInit();

// VRAM for BOs: two buddies (xe_vram.cpp), the CPU-visible window [vramUserStart, vramVisible) and the rest
// [vramVisible, vramEnd); gResLock. Below vramUserStart: firmware framebuffer, IntelXeFB's cursor image (DPA 0x1F00000 -
// 0x1F3FFFF). Page tables come from the window too (ptPageAlloc).
static VramBuddy gVis, gHi;
static bool gHiOn;                  // gHi only once the copy engine can clear it (vramClearInit)
// The window is shared by user BOs and the kernel's own page tables (ptPageAlloc), as visible VRAM is in xe, where
// page tables are pinned kernel BOs with XE_BO_FLAG_NEEDS_CPU_ACCESS (xe_pt_create -> xe_bo_create_pin_map). xe can
// always make room for them: TTM evicts user BOs from the visible range (to TT: xe_evict_flags), CPU mmaps included,
// since a CPU fault re-validates the BO. Here a BO the CPU has mapped cannot move (an IOKit user mapping of a physical
// range cannot be pointed elsewhere: IOMemoryMap::redirect needs a single-entry pageable object, upl_transpose), so a
// window full of mapped BOs left nothing for page tables: every VM_BIND then failed with ENOMEM, a new descriptor pool
// or BO in any process came back VK_ERROR_OUT_OF_DEVICE_MEMORY (Minecraft 2026-10-29: draws skipped, textures black).
// So user placements in the window keep kWinReserve free for page tables (a 64 KB block of them maps 64 MB of 4 KB
// pages, 32 GB of 2 MB ones); the display's buffers (xeDispVramAlloc) may go down to kDispFloor. A user BO that does
// not fit goes beyond the window or to system memory instead (GEM_CREATE), as xe spills NEEDS_VISIBLE_VRAM BOs that
// allow system memory.
static constexpr uint64_t kWinReserve = 32ull << 20, kDispFloor = 8ull << 20;
static VBlock *visAlloc(uint64_t size, bool topdown, bool contiguous, uint64_t floor) {   // gResLock
	if (gVis.freeBytes < floor || gVis.freeBytes - floor < ((size + kVChunk - 1) & ~(kVChunk - 1))) {
		gStats.reserveHits++;
		return nullptr;
	}
	return buddyAlloc(&gVis, size, topdown, contiguous);
}
static uint64_t gVramUsed, gVisibleUsed;
static volatile int64_t gSysUsed;   // system-memory BOs (DRM_XE_MEM_REGION_CLASS_SYSMEM used, as TTM's TT manager counts)

static uint64_t gSysTotal;
static uint32_t gRefClock, gGeomDss, gCompDss, gEuMask, gFfMode;
static uint64_t gL3Mask;
// kSlots 16-byte slots at GGTT seqnoG: dword 0 the completed seqno, 1 the seqno of the job that started last, 2 the
// context's CTX_TIMESTAMP when it started (Linux keeps these in the LRC: seqno, start_seqno, ctx_job_timestamp),
// rb7o: 3 on the media GT the ENGINE_ID register of the engine the last job started on (xe's utilization WA BB stores
// it in the PPHWSP: "we do not know which instance the context is running on until it is scheduled"), 0 before any.
static IOBufferMemoryDescriptor *gSeqnoMem;
static constexpr uint32_t kSlotBytes = 16;
static volatile uint32_t gTlbSeq;
char gG2hLog[4096];
uint32_t gG2hLen;

static uint64_t nowNs() {
	uint64_t abs, ns;
	clock_get_uptime(&abs);
	absolutetime_to_nanoseconds(abs, &ns);
	return ns;
}

// Every waiter sleeps on one generation counter; anything that may complete a wait bumps it (interrupt, G2H, syncobj
// changes, queue teardown). The waiter reads the generation before testing its condition and sleeps only if it has not
// moved, under gWaitLock, so no wakeup is lost between the test and the sleep.
static void wakeAll() {
	IOLockLock(gWaitLock);
	gWakeGen++;
	IOLockWakeup(gWaitLock, const_cast<uint32_t *>(&gWakeGen), false);
	IOLockUnlock(gWaitLock);
}

static void g2hLog(const char *fmt, ...) __printflike(1, 2);
static void faultQueue(const uint32_t *dw);   // rb7j: GuC page faults (after vmCreate)
static void faultsDrain();
static void g2hLog(const char *fmt, ...) {
	char line[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof(line), fmt, ap);
	va_end(ap);
	IOLockLock(gSayLock);
	if (gG2hLen >= 3500) gG2hLen = 0;
	gG2hLen += (uint32_t)snprintf(gG2hLog + gG2hLen, sizeof(gG2hLog) - gG2hLen, "%s", line);
	IOLockUnlock(gSayLock);
	LOG("%s", line);
}

static volatile uint64_t *vram64(uint64_t dpa) {
	return reinterpret_cast<volatile uint64_t *>(gDevVram()->getVirtualAddress() + dpa);
}

// ─── objects ───────────────────────────────────────────────────────────────────────────────────────────────────────
// XeFence (xe_tlbinval.h): slot == ~0u signaled, kXeTlbSlot (rb8d) a TLB invalidation, else an exec queue's seqno slot
struct XeBo;
// A CPU map handed out by GEM_MMAP_OFFSET (see xeClientMemory).
struct XeClientState;
struct XeCpuMap {
	XeBo *bo;          // its BO; held (a reference) from the first mapping on, closed handle or not (see boMapsPrune)
	IOMemoryDescriptor *md;
	uint32_t idx;      // in owner->maps
	bool used;         // handed to clientMemoryForType: a mapping exists or is being made
	XeCpuMap *next;    // bo->cpuMaps (nullptr once detached); the list itself is gBoLock's
	bool held;         // holds a BO reference (drm_gem_vm_open: an mmap keeps its GEM object)
	bool detached;     // the client's last handle of it was closed: off bo->cpuMaps, freed when the mapping goes
	                   // (clientMapsPrune)
	bool byHandle;     // made by IOConnectMapMemory64 with a GEM handle instead of a cookie (older user space)
	// rb7s: the client whose maps table holds it (whose lock guards used/held/detached). One BO's list holds the maps of
	// every client that has a handle of it (drm_vma_node_allow is per file): a client prunes and detaches only its own.
	XeClientState *owner;
};

struct XeBo {
	uint64_t size;
	bool vram;
	bool contig;                // its VRAM is one physical run starting at dpa (scan-out, single-range CPU maps)
	bool hi;                    // blocks from gHi (outside the BAR window)
	uint64_t dpa;               // first block's DPA
	VBlock *blocks;             // VRAM blocks in BO offset order (linked by VBlock::link)
	XeFence clear;              // the copy-engine clear of its fresh VRAM (xe_bo's kernel fence)
	IOBufferMemoryDescriptor *sys;
	IOMemoryDescriptor *user;   // MAP_USERPTR: the caller's pages, wired until the last binding goes
	uint16_t caching;
	volatile SInt32 refs;       // handle, bindings, deferred frees, scan-out pins (atomic: pins come from NMXAccel)
	volatile SInt32 pins;       // scan-out pins (xeScanoutPin): the display reads its DPA, so it never moves
	bool mapped;                // sticky: the CPU was given a mapping (IOKit user mappings cannot be revoked), never moves
	XeCpuMap *cpuMaps;          // its CPU maps (GEM_MMAP_OFFSET), each with its own memory descriptor (XeCpuMap)
	uint32_t gemFlags;          // DRM_XE_GEM_CREATE_FLAG_* it was created with (debug.intelxe.vram)
	uint32_t placement;         // and its placement mask
	// rb7s: handles, every client's together (drm_gem_object handle_count; GEM_CREATE, surface imports, GEM_OPEN, each
	// SURFACE_VRAM import without SAME_HANDLE is a handle of its own). WAS `closed`, one flag for the BO: a client closing
	// its handle of a shared BO made another client's GEM_MMAP_OFFSET of it answer ENOENT. Atomic (several client locks).
	volatile SInt32 handles;
	// rb7s: binding records of it in every VM, live or destroyed (each XeBinding naming it): a move rewrites the PTEs of
	// every one (xe_bo_move_notify -> every vm_bo of the object), so it is refused while some sit in a VM it cannot reach
	// (destroyed, its queues still being torn down). Atomic: the records of a shared BO change under several client locks.
	volatile SInt32 vmRecs;
	// rb7s: owner: the first client that got a handle of it (compared, never dereferenced); shared (sticky): a handle went
	// to another client (GEM_OPEN, a SURFACE_VRAM import from a second process). Moves of a shared BO rewrite the VMs of
	// every client that binds it (see moveSetBegin); the flag and `migrating` are a Dekker pair (boHandleGet).
	void *owner;
	volatile bool shared;
	uint32_t name;              // rb7s: its DRM_IOCTL_GEM_FLINK name (gNameLock), 0 none
	volatile bool migrating;    // a move is in progress (the moving client's lock may be dropped while it waits)
	bool scanout;               // DRM_XE_GEM_CREATE_FLAG_SCANOUT: one physical run (NMXAccel pins it by DPA), never moves
	bool noComp;                // DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION: no compressed PAT may map it
	bool compBound;             // sticky: bound with a compressed PAT at least once, so its VRAM may hold compressed data
	                            // (copies out of it read through the compressed identity map: kIdentCompVa)
	bool compWarned;
	bool noMove;                // its binding records could not be split (no memory): they do not describe its PTEs exactly
	uint32_t pmGen;             // suspend: saved in this round already
	uint64_t pmVa;              // suspend: its backup's address in the migrate VM
	IOBufferMemoryDescriptor *pmBackup;   // suspend: its VRAM contents while the device is off
	DmaMap pmDma;               // suspend: the backup's device addresses
	DmaMap dma;                 // system BO / userptr: its pages' device addresses (IOVAs under VT-d)
	// rb7r: purgeable state (xe_bo_types.h bo->purgeable, DRM_IOCTL_XE_MADVISE DRM_XE_VMA_ATTR_PURGEABLE_STATE). Only BOs
	// made by GEM_CREATE are tracked (purgeTrack): the shared surfvram backings are this driver's imported objects and,
	// as drm_gem_is_imported BOs in xe, never change state; userptr bindings are not BOs to xe. Client lock (xe: the BO's
	// dma-resv). purgeState: XE_MADV_PURGEABLE_WILLNEED / DONTNEED / PURGED (terminal); vmaCount: binding records of it
	// in VMs that still have a handle; willneedCount: those whose own state is WILLNEED (xe also counts dma-buf exports,
	// which do not exist here). sysRefs: bo->sys's retain count with no CPU mapping (each mapping of a system BO retains
	// it), to tell whether one is still there. purgeScan: the purge pass that last looked at it.
	bool purgeTrack;
	uint8_t purgeState;
	uint32_t vmaCount, willneedCount, sysRefs, purgeScan;
	// rb7t: object fds (PRIME). vmPrivate: GEM_CREATE with a vm_id (xe's bo->vm: never exported, xe_gem_prime_export
	// EPERM). dmabufRefs / dmabufCached (gNameLock): the references of what Linux has as the BO's one dma-buf: one per BO
	// fd (minor) and one for the handle cache while the BO has handles (obj->dma_buf, dropped with its last handle,
	// drm_gem_object_exported_dma_buf_free); while there are any the BO is held WILLNEED (xe_gem_prime_export ->
	// xe_dma_buf_release). exported: sticky, for debug.intelxe.fd_stats' count of such BOs alive.
	bool vmPrivate;
	bool dmabufCached;
	bool exported;
	uint32_t dmabufRefs;
};

// pat/flags: to rewrite the PTEs when the BO moves. handle (rb7l): the client's GEM handle the MAP named (0: userptr, NULL
// and kernel bindings). UNMAP_ALL drops the bindings made through its handle only: here one BO can sit behind several
// handles of one client (each SURFACE_VRAM import of a shared IOSurface backing is a handle of its own), where Linux has
// one handle per object per file (drm_gem_prime_fd_to_handle returns the existing one), so in Linux "every VMA of the
// object" and "every VMA of the handle" are the same set, and Mesa ANV, which unbinds a BO it releases with UNMAP_ALL of
// its handle (xe_vm_unbind_bo), relies on exactly that.
// purge (rb7r): the record's own purgeable state (xe_vma attr.purgeable_state: WILLNEED on a new MAP, inherited by the
// parts a partial unbind leaves).
struct XeBinding { uint64_t addr, range; XeBo *bo; uint64_t off; uint16_t pat; uint32_t flags; uint32_t handle; uint8_t purge; };

struct PtNode;
struct XeQueue;
struct XeVm {
	uint64_t root;              // DPA of the level-4 table (CTX_PDP0)
	PtNode *top;                // its shadow
	uint32_t asid;              // global (gAsidBits), so two processes' VMs never share TLB tags
	volatile SInt32 refs;       // the client's handle and each exec queue on it (xe_exec_queue holds its VM)
	volatile uint32_t ptGen;    // bumped after every page-table change (xe_vm tlb_flush_seqno): see jobDwords
	XeBinding *binds;
	uint32_t nBinds, capBinds;
	// The latest kernel operation on its BOs (the copy-engine clear of fresh VRAM): the VM dma-resv KERNEL slot, which
	// every exec on the VM waits behind (xe_exec.c: xe_sched_job_add_deps(job, obj->resv, DMA_RESV_USAGE_KERNEL) for the
	// VM's resv and each external BO). Clears all run on the one migrate queue, so the latest covers the earlier ones.
	XeFence kern;
	// rb7j: DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE (xe_vm_create_scratch): empty entries are NULL leaves (reads 0, writes
	// dropped) only when the VM was created with it; otherwise they are 0 and an access faults (the GuC page fault
	// queue then reports it, see gFaults). Kernel VMs always have scratch.
	bool scratch;
	// rb7j: the faults the GuC reported for this VM (xe_vm_add_fault_entry_pf: at most MAX_FAULTS_SAVED_PER_VM = 50,
	// kept until the VM goes), read by DRM_IOCTL_XE_VM_GET_PROPERTY / DRM_XE_VM_GET_PROPERTY_FAULTS; and the list of
	// live VMs the fault worker looks a fault's ASID up in (xe->usm.asid_to_vm). Both under gFaultLock.
	struct Fault { uint64_t addr; uint8_t access, type, level, srcid; } faults[50];
	uint32_t nFaults;
	XeVm *faultNext;
	// rb8b: for the CAT-fault dump (engineDump): when PTEs of it were last removed or replaced (a VM_BIND unbind or
	// remap over live entries, a purge; nowNs, 0 never), how often, and the TLB invalidation that removal issued; when
	// a BO move last rewrote its PTEs (boRemap); when it was last bound (VM_BIND with ops). Written under the client
	// lock, read racily by the dump.
	uint64_t unbindNs, remapNs, bindNs;
	uint32_t unbinds, unbindTlb;
	// rb8c: its exec queues (XeQueue::vmNext; gSchedLock), from queueCreate until queueTeardown takes the guc_id back:
	// whose last job a deferred free of this VM waits for (deferSeal), as xe's frees wait for the VM's dma-resv
	XeQueue *queues;
	uint32_t jobHeldSeals;      // rb8c, engineDump: unbinds whose frees had to wait for jobs of this VM (client lock)
	// rb8d: the out-fence of its last VM_BIND, the binds before folded in (xe_exec_queue_last_fence of the bind queue):
	// every VM_BIND's out-fence covers it, and a VM_BIND without ops signals it (client lock). rb8e: the VM's default
	// bind queue's (vm->q[0], calls with exec_queue_id 0) only; a named bind queue has its own (XeBindQueue)
	XeBindFence bindOut;
};

// A job waiting in its queue's FIFO (drm_sched_job): the batch, its seqno (reserved at exec time, so out-fences exist
// at once) and the fences it waits for.
// DRM_XE_SYNC_TYPE_USER_FENCE of an exec (xe_sched_job.h job->user_fence): the job's ring stores value, 64 bits, at the
// GPU address addr in the queue's VM after the batch and before its seqno and interrupt (xe_ring_ops.c
// emit_store_imm_ppgtt_posted).
struct XeUfence { uint64_t addr, value; bool used; };

struct XeJob {
	XeJob *next;
	uint64_t batch;             // 0: ring-only job (signals its seqno after everything before it)
	XeUfence uf;                // the exec's user fence (uf.used)
	uint32_t seqno, nDeps, capDeps;
	XeFence *deps() { return reinterpret_cast<XeFence *>(this + 1); }
};

struct XeQueue {
	XeVm *vm;
	// rb7o: eng is the first placement (xe's q->hwe: GT, GuC class, the LRC's register offsets, which xe also writes at
	// that engine's base with MI_LRI_LRM_CS_MMIO, so they hold on any engine of the class); logicalMask is every engine
	// the queue may run on (xe q->logical_mask, logical ids of eng's class on eng's GT), registered with the GuC as the
	// context's engine_submit_mask: the GuC puts the context on whichever of them is free. One bit for every queue but a
	// video one created with several placements (and debug.intelxe.vcs_single 0).
	const XeEngine *eng;
	uint32_t logicalMask;
	uint16_t cls, inst;
	uint32_t gucId, slot, gen;
	// The LRC (ring at 0, PPHWSP at ringSize, context registers at ringSize + 0x1000), lrcLen bytes at GGTT lrcGgtt.
	// rb7: in VRAM like xe on dGFX (xe_lrc_init: XE_BO_FLAG_VRAM_IF_DGFX, user contexts FORCE_USER_VRAM, pinned and
	// CPU-mapped, so contiguous and CPU-visible): one 64 KB block of the BAR window (lrcBlk), written by the CPU
	// write-combined through BAR2 (lrcCpu). Else (debug.intelxe.lrc_vram 0, boot-arg -intelxe_lrcsys, or no window block)
	// system memory as before (lrc). lrcSave: a VRAM LRC's contents across system sleep.
	IOBufferMemoryDescriptor *lrc;
	VBlock *lrcBlk;
	uint8_t *lrcCpu, *lrcSave;
	uint32_t lrcLen;
	uint32_t lrcGgtt;
	// gSchedLock: ring tail (bytes), last seqno handed out to a fence, last seqno written to the ring, ring tail after
	// each job (seqno % 256) for ring space, the job FIFO and the scheduler list link.
	uint32_t tail, seqno, submitted;
	uint32_t flushedGen;        // vm->ptGen this queue's last TLB-invalidating job saw (xe_exec_queue tlb_flush_seqno)
	uint32_t ringPos[256];
	XeJob *jobHead, *jobTail;
	uint32_t jobCount;          // jobs in jobHead's FIFO (xe's job_cnt counts the running ones too)
	XeQueue *schedNext;
	bool onSched, closed;
	volatile bool banned;
	bool schedOffSent;          // SCHED_CONTEXT_MODE_SET(disable) sent (watchdog or teardown), gSchedLock
	bool kernel;                // a kernel queue (migrate, golden context recording): never banned by the watchdog
	// rb7j: from DRM_IOCTL_XE_EXEC_QUEUE_CREATE (init_policies / xe_lrc_init): the uAPI priority (XE_EXEC_QUEUE_PRIORITY_*,
	// NORMAL by default), the timeslice in us (1 ms default), DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT, and
	// DRM_XE_EXEC_QUEUE_SET_DISABLE_STATE_CACHE_PERF_FIX (render: COMMON_SLICE_CHICKEN3 bit 13 set in the ring at init)
	uint8_t prio;
	bool lowLatency, stateCacheFix;
	uint32_t timesliceUs;
	uint32_t wdSeq;             // job watchdog (gSchedLock): completed seqno when last seen moving, and since when
	uint64_t wdSince;
	uint8_t tdr;                // timeout check in progress (gSchedLock): 1 schedule disable sent to sample the context's
	uint64_t tdrSince;          // run time, 2 being taken off the engine, banned once the GuC confirms (queueKickOff);
	                            // submission to the ring is held
	uint32_t tdrSample;         // check_timeout's job->sample_timestamp: the saved CTX_TIMESTAMP at the last timeout check
	int pid;                    // the process that created it (0: kernel), and its name: for the reset/ban log lines
	char name[16];
	uint32_t karmaSeq;          // xe_sched_invalidate_job: the job (seqno) the GT resets and timeouts below count against,
	uint8_t karma;              // and how many there were (more than 2: banned)
	XeQueue *reapNext;
	// debug.intelxe.direct (gSchedLock): a slot is armed in the ring (direct submission, see dsEnter); the mailbox and
	// stub entry in gDsMem + 1 (0 none; kept until the guc_id goes); the seqno of the last slot of the last direct
	// stretch; when the queue last submitted (eligible queues, while the switch is on)
	bool dsArmed;
	uint8_t dsEntry;
	uint32_t dsEndSeq;
	uint64_t dsLastNs;
	XeQueue *vmNext;            // rb8c: vm->queues (gSchedLock)
};

// guc_id -> queue (g2h_exec_queue_lookup), per GuC. gSchedLock.
static XeQueue *gQueueByGuc[2][kMaxGucIds];
static XeQueue *gSchedHead;     // queues with jobs waiting (gSchedLock)
static XeQueue *gReapHead;      // closed queues to tear down (gResLock)
static XeQueue *gZombies;       // torn-down queues the GuC did not confirm off: kept with their VM (gResLock)

// VM_BIND-class exec queues (DRM_XE_ENGINE_CLASS_VM_BIND): binds are synchronous, so a bind queue is only what orders
// its binds' out-fences, its last fence (rb8e: xe_exec_queue.c xe_exec_queue_last_fence_get / _set, one per exec
// queue; xe_vm.c vm_bind_ioctl_ops_fini sets it, vm_bind_ioctl_signal_fences signals it for a call without ops). WAS a
// marker (the constant 1) and every bind ordered behind the VM's last one. Its table slot holds the pointer with bit 0
// set (isBindQ), so it is never taken for an XeQueue; client lock.
struct XeBindQueue { XeBindFence last; };
static inline bool isBindQ(const void *q) { return (reinterpret_cast<uintptr_t>(q) & 1) != 0; }
static inline XeBindQueue *bindQOf(const void *q) {
	return isBindQ(q) ? reinterpret_cast<XeBindQueue *>(reinterpret_cast<uintptr_t>(q) & ~(uintptr_t)1) : nullptr;
}
static inline void *bindQTag(XeBindQueue *b) { return reinterpret_cast<void *>(reinterpret_cast<uintptr_t>(b) | 1); }
// rb8e, debug.intelxe.stats: bind queues made and gone, VM_BINDs on a named bind queue, EXECs on one (IntelXe's
// extension) that waited for its last fence
static struct { uint32_t made, gone, binds, execs; } gBindQSt;

struct XeSyncobj {
	XeFence *f;
	uint32_t n, cap;
	// rb7t: references: every client's handles and the object fds (drm_syncobj refcount); it goes with the last. shared
	// (sticky): exported to an fd (SYNCOBJ_HANDLE_TO_FD), set under the exporting client's lock before any other client
	// can reach it; from then on f/n/cap are read and written under gSyncLock as well (SyncGuard), since each client's
	// handle is used under its own client lock. A syncobj never exported (every one WindowServer uses) costs one byte
	// test per access.
	volatile SInt32 refs;
	volatile bool shared;
};

struct XeClientState {
	Table bos, vms, queues, syncs;
	void *task;              // the task that opened the connection: intelxeScanoutPin looks a task's VMs up by it
	XeClientState *next;     // gClients (gClientsLock)
	IOLock *lock;
	uint32_t busy;           // threads inside xeIoctl/xeClientMemory (lock)
	volatile uint32_t lastReq;   // the last ioctl a thread entered (diagnostics: a teardown or wait that does not end)
	bool dying;              // xeClientDestroy started: waits return EINTR, new calls ENXIO
	uint32_t migrating;      // BO moves in progress: EXEC and VM_BIND wait (the reservation of its VMs, see boMove)
	Table maps;              // CPU maps by cookie (XeCpuMap)
	uint32_t orphanMaps;     // CPU maps whose BO handle was closed while the mapping was still there
	int pid;                 // the process that opened the connection, and its name (debug.intelxe.vram)
	char name[24];
	// Bind user fences whose bind fence had not signaled when VM_BIND returned and whose page could not be wired for the
	// asynchronous writer (rb8e, ufAsyncAdd; bindUfenceWait): written by this client's next ioctl or WAIT_USER_FENCE poll
	// once it has (lateUfFlush), from the client's own task.
	struct { uint64_t addr, value; XeBindFence f; } lateUf[8];   // rb8d: the whole bind fence (clear, invalidation)
	uint32_t nLateUf;
};
static XeClientState *gClients;
// INTELXE_CAP_SURFACE_VRAM_LAYOUT: the WindowServer client that imported with SURFACE_VRAM2 (its plugin reads tiled
// backings); cleared when that client goes (gClientsLock)
static XeClientState *gSVLayoutReader;

static uint32_t slotWord(uint32_t slot, uint32_t i) {
	auto *p = static_cast<volatile uint32_t *>(gSeqnoMem->getBytesNoCopy()) + slot * (kSlotBytes / 4);
	flushRange(const_cast<uint32_t *>(p), kSlotBytes);
	return p[i];
}
static uint32_t seqnoOf(uint32_t slot) { return slotWord(slot, 0); }

// rb7o: dword 3 (the ENGINE_ID a media job stored when it started) back to 0, for a new queue in the slot
static void slotClearEngine(uint32_t slot) {
	auto *p = static_cast<volatile uint32_t *>(gSeqnoMem->getBytesNoCopy()) + slot * (kSlotBytes / 4);
	p[3] = 0;
	flushRange(const_cast<uint32_t *>(p), kSlotBytes);
}

// rb7o: "VCS2" from a stored ENGINE_ID (ENGINE_CLASS_ID 2:0, ENGINE_INSTANCE_ID 9:4; xe_lrc.c engine_id_to_hwe), "-"
// before the queue's first job started
static const char *engineIdName(uint32_t id, char *b, size_t n) {
	static const char *const kCls[8] = {"RCS", "VCS", "VECS", "BCS", "OTHER", "CCS", "?", "?"};
	if (!id) snprintf(b, n, "-");
	else snprintf(b, n, "%s%u", kCls[id & 7], id >> 4 & 0x3f);
	return b;
}

// rb7o: for the hang and ban log lines of a media queue: its logical mask and the engine its last job started on
static const char *queueWhere(XeQueue *q, char *b, size_t n) {
	if (!q->eng->gt) {
		b[0] = 0;
		return b;
	}
	char e[16];
	snprintf(b, n, ", mask 0x%x, last job on %s", q->logicalMask, engineIdName(slotWord(q->slot, 3), e, sizeof(e)));
	return b;
}

static void seqnoSet(uint32_t slot, uint32_t v) {
	auto *p = static_cast<volatile uint32_t *>(gSeqnoMem->getBytesNoCopy()) + slot * (kSlotBytes / 4);
	p[0] = v;
	p[1] = v;
	p[2] = 0;
	flushRange(const_cast<uint32_t *>(p), kSlotBytes);
}

static bool tlbSeqDone(uint32_t seq);
static bool fenceSignaled(const XeFence &f) {
	if (f.slot == ~0u) return true;
	if (f.slot == kXeTlbSlot) return !gDev || tlbSeqDone(f.seqno);   // rb8d: an unbind's invalidation (VM_BIND out-fence)
	if (gSlotGen[f.slot] != f.gen) return true;   // queue torn down: everything it had was completed or dropped
	if (gSlotBanGen[f.slot] == f.gen && (int32_t)(gSlotBanSeq[f.slot] - f.seqno) >= 0) return true;   // banned
	return (int32_t)(seqnoOf(f.slot) - f.seqno) >= 0;
}
static bool bindFenceSignaled(const XeBindFence &f) { return fenceSignaled(f.clear) && fenceSignaled(f.tlb); }   // rb8d

// A kernel operation (a BO clear) the VM's later execs must wait behind: kept if it is the latest (client lock).
static void vmKernAdd(XeVm *vm, const XeFence &f) {
	if (!vm || fenceSignaled(f)) return;
	if (fenceSignaled(vm->kern) || vm->kern.slot != f.slot || vm->kern.gen != f.gen || (int32_t)(f.seqno - vm->kern.seqno) > 0)
		vm->kern = f;
}

// A BO's VRAM: outside the window from the top down if the CPU will not map it (DRM_BUDDY_TOPDOWN_ALLOCATION, as
// xe_ttm_vram_mgr does for !XE_BO_FLAG_NEEDS_CPU_ACCESS), else in the window, contiguous when it can be (a scan-out
// candidate, and one physical range for the CPU map), any blocks otherwise.
// contiguous: one physical run wherever it goes (scan-out: NMXAccel's pin gives the display one DPA range).
static bool vramAllocBo(XeBo *bo, uint64_t size, bool visible, bool contiguous = false) {
	VBlock *bl = nullptr;
	bool hi = false;
	IOLockLock(gResLock);
	if (!visible && gHiOn) hi = (bl = buddyAlloc(&gHi, size, true, contiguous)) != nullptr;
	if (!bl) bl = visAlloc(size, !visible, visible || contiguous, kWinReserve);
	if (!bl && visible && !contiguous) bl = visAlloc(size, false, false, kWinReserve);
	uint64_t total = 0;
	bool contig = true;
	for (VBlock *b = bl; b; b = b->link) {
		if (b->link && b->off + vblockSize(b) != b->link->off) contig = false;
		total += vblockSize(b);
	}
	if (bl) {
		gVramUsed += total;
		if (!hi) gVisibleUsed += total;
	}
	IOLockUnlock(gResLock);
	if (!bl) return false;
	bo->vram = true;
	bo->hi = hi;
	bo->blocks = bl;
	bo->dpa = bl->off;
	bo->contig = contig;
	bo->clear = {0, ~0u, 0, 0};
	return true;
}

static void vramFree(XeBo *bo) {
	uint64_t total = 0;
	IOLockLock(gResLock);
	for (VBlock *b = bo->blocks; b; b = b->link) total += vblockSize(b);
	buddyFree(bo->hi ? &gHi : &gVis, bo->blocks);
	gVramUsed -= total;
	if (!bo->hi) gVisibleUsed -= total;
	IOLockUnlock(gResLock);
	bo->blocks = nullptr;
}

static bool boVisible(const XeBo *bo) { return bo->vram && !bo->hi; }

// DPA of BO offset `off` and the bytes left in that block.
static uint64_t boDpa(const XeBo *bo, uint64_t off, uint64_t *runLeft = nullptr) {
	for (VBlock *b = bo->blocks; b; b = b->link) {
		if (off < vblockSize(b)) {
			if (runLeft) *runLeft = vblockSize(b) - off;
			return b->off + off;
		}
		off -= vblockSize(b);
	}
	return 0;
}

static void boGet(XeBo *bo) { OSIncrementAtomic(&bo->refs); }

static volatile SInt32 gDontneedBos;
static volatile SInt32 gExportedBoLive;   // rb7t: BOs that were PRIME-exported, alive
static void boPut(XeBo *bo) {
	if (!bo || OSDecrementAtomic(&bo->refs) != 1) return;
	if (bo->purgeTrack && bo->purgeState == 1) OSDecrementAtomic(&gDontneedBos);   // rb7r: freed while DONTNEED
	if (bo->exported) OSDecrementAtomic(&gExportedBoLive);   // rb7t (debug.intelxe.fd_stats)
	if (bo->vram) vramFree(bo);
	if (bo->sys) OSAddAtomic64(-(SInt64)bo->size, const_cast<SInt64 *>(&gSysUsed));
	bo->dma.unmap();   // the GPU is done with it: its last binding's invalidation has completed
	OSSafeReleaseNULL(bo->sys);
	if (bo->user) { bo->user->complete(); OSSafeReleaseNULL(bo->user); }
	IOFree(bo, sizeof(*bo));
}

// ─── rb7r: purgeable state bookkeeping (xe_bo.h xe_bo_willneed_get/put_locked, xe_bo_vma_count_inc/dec_locked) ─────
// The client lock stands for the BO's dma-resv. A BO starts WILLNEED with no holders; it becomes DONTNEED when its last
// WILLNEED binding goes DONTNEED while it still has bindings, WILLNEED again when one becomes WILLNEED; PURGED is
// terminal. Untracked BOs (not GEM_CREATE's: xe's imported BOs) never change state.
enum : uint8_t { kMadvWillneed = 0, kMadvDontneed = 1, kMadvPurged = 2 };   // enum xe_madv_purgeable_state
// gDontneedBos (above boPut): tracked BOs now DONTNEED, all clients; a purge pass with none is skipped
// xe_bo_set_purgeable_state
// rb7s: the counters and the state under gBoLock: a shared BO (GEM_OPEN, a surface import) has bindings, and so holders,
// in several clients' VMs, each changed under its own client's lock (xe: one dma-resv for the BO).
static void boSetPurgeLocked(XeBo *bo, uint8_t st) {
	if (bo->purgeState == st) return;
	if (st == kMadvDontneed) OSIncrementAtomic(&gDontneedBos);
	else if (bo->purgeState == kMadvDontneed) OSDecrementAtomic(&gDontneedBos);
	bo->purgeState = st;
}
static void boSetPurge(XeBo *bo, uint8_t st) {
	IOSimpleLockLock(gBoLock);
	boSetPurgeLocked(bo, st);
	IOSimpleLockUnlock(gBoLock);
}
static void boWillneedGetLocked(XeBo *bo) {
	if (bo->willneedCount++ == 0 && bo->purgeState == kMadvDontneed) boSetPurgeLocked(bo, kMadvWillneed);
}
static void boWillneedPutLocked(XeBo *bo) {
	if (!bo->willneedCount) return;
	if (--bo->willneedCount == 0 && bo->vmaCount > 0 && bo->purgeState != kMadvPurged) boSetPurgeLocked(bo, kMadvDontneed);
}
static void boWillneedGet(XeBo *bo) {
	if (!bo || !bo->purgeTrack) return;
	IOSimpleLockLock(gBoLock);
	boWillneedGetLocked(bo);
	IOSimpleLockUnlock(gBoLock);
}
static void boWillneedPut(XeBo *bo) {
	if (!bo || !bo->purgeTrack) return;
	IOSimpleLockLock(gBoLock);
	boWillneedPutLocked(bo);
	IOSimpleLockUnlock(gBoLock);
}
// a binding record of bo appears (xe_vma_create) / goes (xe_vma_destroy: the count first, then the holder)
static void bindingCounted(XeBo *bo, uint8_t purge) {
	if (!bo || !bo->purgeTrack) return;
	IOSimpleLockLock(gBoLock);
	bo->vmaCount++;
	if (purge == kMadvWillneed) boWillneedGetLocked(bo);
	IOSimpleLockUnlock(gBoLock);
}
static void bindingUncounted(XeBo *bo, uint8_t purge) {
	if (!bo || !bo->purgeTrack) return;
	IOSimpleLockLock(gBoLock);
	if (bo->vmaCount) {
		bo->vmaCount--;
		if (purge == kMadvWillneed) boWillneedPutLocked(bo);
	}
	IOSimpleLockUnlock(gBoLock);
}

// ─── rb7s: GEM handles across clients (drm_gem.c: handle_count, drm_gem_handle_create_tail / drm_gem_object_release_handle,
// drm_gem_flink_ioctl / drm_gem_open_ioctl) ────────────────────────────────────────────────────────────────────────
// A BO's handles are counted over every client (bo->handles). A client's own state for the BO is its handles in its
// table and its CPU maps on bo->cpuMaps (XeCpuMap::owner): closing one client's handle touches nothing of another's.
// GEM_FLINK names (gNames, gNameLock) live while the BO has a handle anywhere, as in drm (the name goes with the last
// handle, drm_gem_object_handle_free). A named BO is this driver's cross-process export, so the name holds the BO
// WILLNEED as a dma-buf export does in xe (xe_gem_prime_export: xe_bo_willneed_get_locked, DONTNEED EBUSY, purged
// EINVAL): a BO other processes may bind never becomes purgeable behind their backs.
static Table gNames;   // GEM_FLINK name -> BO (name = index + 1), gNameLock
int gFlink = 0;        // debug.intelxe.flink: GEM_FLINK / GEM_OPEN allowed (1); 0: EACCES, as a render node answers them

// A handle of bo is being made in c (c->lock held; gNameLock for GEM_OPEN). Marks the BO shared when the handle goes to
// a client other than its first, then reads `migrating`: true means a move that did not see `shared` is running, and the
// caller must wait for it before the handle is used (the Dekker pair with moveSetBegin).
static bool boHandleGet(XeClientState *c, XeBo *bo) {
	OSIncrementAtomic(&bo->handles);
	if (!bo->owner && OSCompareAndSwapPtr(nullptr, c, reinterpret_cast<void *volatile *>(&bo->owner))) return false;   // its first client
	if (bo->owner != c && !bo->shared) {
		bo->shared = true;
		__sync_synchronize();
		return bo->migrating;
	}
	return false;
}

// A handle of bo is gone (taken out of a client's table, that client's lock held). The last one anywhere takes the BO's
// name and the name's WILLNEED hold with it. true: it was the last handle anywhere.
static void dmabufPutLocked(XeBo *bo);
static bool boHandleRelease(XeBo *bo) {
	if (OSDecrementAtomic(&bo->handles) != 1) return false;
	if (bo->name || bo->dmabufCached) {
		IOLockLock(gNameLock);
		if (!bo->handles && bo->name) {   // no GEM_OPEN found it meanwhile (that takes a handle under gNameLock)
			gNames.take(bo->name);
			bo->name = 0;
			boWillneedPut(bo);
		}
		// rb7t: the handle cache's reference of its dma-buf goes with the last handle (a PRIME import takes a handle under
		// gNameLock too)
		if (!bo->handles && bo->dmabufCached) {
			bo->dmabufCached = false;
			dmabufPutLocked(bo);
		}
		IOLockUnlock(gNameLock);
	}
	return true;
}

// ─── rb7t: a BO's "dma-buf" (PRIME, xe_dma_buf.c / drm_prime.c) ─────────────────────────────────────────────────────
// Linux has one dma-buf per exported object, alive while an fd of it or the object's handle cache (obj->dma_buf, until
// the last handle) holds it; xe holds the BO WILLNEED for that long (xe_gem_prime_export -> xe_dma_buf_release). Here
// every export is a minor of its own: dmabufRefs counts the minors and the cache, all under gNameLock.
// Export: xe_gem_prime_export's checks when no dma-buf exists yet (DONTNEED EBUSY, purged EINVAL, then the WILLNEED
// hold), the minor's reference, and the cache's while the BO has handles (export_and_register_object).
static int dmabufExportLocked(XeBo *bo) {
	if (!bo->dmabufRefs) {
		if (bo->purgeTrack && bo->purgeState != kMadvWillneed) return bo->purgeState == kMadvDontneed ? EBUSY : EINVAL;
		boWillneedGet(bo);
		if (!bo->exported) {
			bo->exported = true;
			OSIncrementAtomic(&gExportedBoLive);
		}
	}
	bo->dmabufRefs++;
	if (!bo->dmabufCached && bo->handles) {
		bo->dmabufCached = true;
		bo->dmabufRefs++;
	}
	return 0;
}
static void dmabufPutLocked(XeBo *bo) {
	if (bo->dmabufRefs && !--bo->dmabufRefs) boWillneedPut(bo);   // xe_dma_buf_release
}
static void dmabufPut(XeBo *bo) {
	IOLockLock(gNameLock);
	dmabufPutLocked(bo);
	IOLockUnlock(gNameLock);
}

// ─── syncobj (drm_syncobj.c): a binary syncobj keeps one fence at point 0, a timeline a list of (point, fence) ────
static void syncAdd(XeSyncobj *s, XeFence f) {
	if (s->n == s->cap) {
		uint32_t nc = s->cap ? s->cap * 2 : 4;
		auto *nf = static_cast<XeFence *>(IOMallocZero(nc * sizeof(XeFence)));
		if (!nf) return;
		if (s->f) {
			memcpy(nf, s->f, s->n * sizeof(XeFence));
			IOFree(s->f, s->cap * sizeof(XeFence));
		}
		s->f = nf;
		s->cap = nc;
	}
	s->f[s->n++] = f;
}

static void syncReplace(XeSyncobj *s, XeFence f) {   // binary: drm_syncobj_replace_fence
	s->n = 0;
	f.point = 0;
	syncAdd(s, f);
}

// Timeline value: highest point such that it and every lower point are signaled (rb8d: every fence at it, xe_tlbinval.h
// xeTimelineValue). Prunes signaled history: of the entries that count, the last stays, carrying the value.
static uint64_t syncValue(XeSyncobj *s) {
	uint32_t counted = 0;
	const uint64_t v = xeTimelineValue(s->f, s->n, fenceSignaled, &counted);
	if (counted > 1) {
		const uint32_t drop = counted - 1;
		memmove(s->f, s->f + drop, (s->n - drop) * sizeof(XeFence));
		s->n -= drop;
		s->f[0].point = v;   // signaled: only its point matters any more
	}
	return v;
}

static uint64_t syncLastSubmitted(XeSyncobj *s) {
	uint64_t v = 0;
	for (uint32_t i = 0; i < s->n; i++)
		if (s->f[i].point > v) v = s->f[i].point;
	return v;
}

// Fence for "point": binary -> the one fence; timeline -> first fence at or above point (drm_syncobj_find_fence).
static bool syncFind(XeSyncobj *s, uint64_t point, XeFence *out) {
	for (uint32_t i = 0; i < s->n; i++)
		if (s->f[i].point >= point || point == 0) {
			*out = s->f[i];
			return true;
		}
	return false;
}

static volatile SInt32 gSyncLive, gSharedSyncLive;   // rb7t: syncobjs alive, of them exported to an fd (fd_stats)

static XeSyncobj *syncNew() {
	auto *s = static_cast<XeSyncobj *>(IOMallocZero(sizeof(XeSyncobj)));
	if (!s) return nullptr;
	s->refs = 1;
	OSIncrementAtomic(&gSyncLive);
	return s;
}

static void syncFree(XeSyncobj *s) {
	if (s->shared) OSDecrementAtomic(&gSharedSyncLive);
	OSDecrementAtomic(&gSyncLive);
	if (s->f) IOFree(s->f, s->cap * sizeof(XeFence));
	IOFree(s, sizeof(*s));
}

// rb7t: a handle or an object fd of it is gone (drm_syncobj_put); the last frees it. Nobody else reaches it then.
static void syncPut(XeSyncobj *s) {
	if (s && OSDecrementAtomic(&s->refs) == 1) syncFree(s);
}

// rb7t: f/n/cap of a shared syncobj under gSyncLock, for one syncobj at a time (never two nested: gSyncLock is one lock)
// and never across a wait (waitUntil drops the client lock; the guard is taken inside the condition instead).
struct SyncGuard {
	XeSyncobj *s;
	explicit SyncGuard(XeSyncobj *x) : s(x && x->shared ? x : nullptr) {
		if (s) IOLockLock(gSyncLock);
	}
	~SyncGuard() {
		if (s) IOLockUnlock(gSyncLock);
	}
	SyncGuard(const SyncGuard &) = delete;
	SyncGuard &operator=(const SyncGuard &) = delete;
};

// rb7t: the entries a binary wait (point 0) stands for: the first, and the point-0 entries after it. Only a sync file of
// several fences imported with IMPORT_SYNC_FILE (syncReplaceN; Linux: one dma_fence_array / chain object) makes more
// than one; everywhere else this is the one entry the code used before.
static uint32_t syncBinSpan(const XeSyncobj *s) {
	uint32_t k = s->n ? 1 : 0;
	while (k < s->n && !s->f[k].point) k++;
	return k;
}
static bool syncBinSignaled(XeSyncobj *s) {
	const uint32_t span = syncBinSpan(s);
	for (uint32_t k = 0; k < span; k++)
		if (!fenceSignaled(s->f[k])) return false;
	return span != 0;
}
// rb7t: a sync file's fences: the unsignaled ones, one per queue generation (a queue's seqnos complete in order, so its
// latest stands for the earlier ones, as dma_fence_unwrap_merge keeps one fence per context). false: no room.
static constexpr uint32_t kSyncFileMax = 16;
static bool fenceMerge(XeFence *set, uint32_t *n, uint32_t cap, const XeFence &f) {
	if (fenceSignaled(f)) return true;
	for (uint32_t i = 0; i < *n; i++)
		if (set[i].slot == f.slot && set[i].gen == f.gen) {
			if ((int32_t)(f.seqno - set[i].seqno) > 0) set[i].seqno = f.seqno;
			return true;
		}
	if (*n == cap) return false;
	set[(*n)++] = f;
	return true;
}
// binary: the fences of a sync file replace the syncobj's (drm_syncobj_replace_fence with the file's fence); none = a
// signaled one (a sync file whose fences had all signaled when it was made)
static void syncReplaceN(XeSyncobj *s, const XeFence *f, uint32_t n) {
	if (!n) {
		syncReplace(s, XeFence{0, ~0u, 0, 0});
		return;
	}
	s->n = 0;
	for (uint32_t i = 0; i < n; i++) {
		XeFence x = f[i];
		x.point = 0;
		syncAdd(s, x);
	}
}

// rb7t: a sync_file (drivers/dma-buf/sync_file.c): what a syncobj's fence was when it was exported
// (DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE), a snapshot later signals and resets of the syncobj do not change.
// Fences are device-global values here (slot, generation, seqno), so the snapshot is a copy: the unsignaled ones, one
// per queue (fenceMerge); none left = signaled. Held by its minor only (and briefly by an ioctl reading it).
struct XeSyncFile {
	volatile SInt32 refs;
	uint32_t n;
	XeFence f[kSyncFileMax];
};
static volatile SInt32 gSyncFileLive;
// drm_syncobj_export_sync_file -> drm_syncobj_find_fence(point 0): the syncobj's fence. A binary syncobj has one (or a
// sync file's several); a timeline's is the last point's chain node, which signals when every point has: every entry.
// No fence (reset, or never signaled nor submitted): EINVAL. More queues than kSyncFileMax: ENOMEM.
static XeSyncFile *syncFileNew(XeSyncobj *so, int *err) {
	auto *sf = static_cast<XeSyncFile *>(IOMallocZero(sizeof(XeSyncFile)));
	if (!sf) {
		*err = ENOMEM;
		return nullptr;
	}
	sf->refs = 1;
	*err = 0;
	{
		SyncGuard g(so);
		if (!so->n) *err = EINVAL;
		for (uint32_t k = 0; k < so->n && !*err; k++)
			if (!fenceMerge(sf->f, &sf->n, kSyncFileMax, so->f[k])) *err = ENOMEM;
	}
	if (*err) {
		IOFree(sf, sizeof(*sf));
		return nullptr;
	}
	OSIncrementAtomic(&gSyncFileLive);
	return sf;
}
static void syncFilePut(XeSyncFile *sf) {
	if (sf && OSDecrementAtomic(&sf->refs) == 1) {
		OSDecrementAtomic(&gSyncFileLive);
		IOFree(sf, sizeof(*sf));
	}
}
static bool syncFileSignaled(const XeSyncFile *sf) {
	for (uint32_t i = 0; i < sf->n; i++)
		if (!fenceSignaled(sf->f[i])) return false;
	return true;
}

// ─── waiting ──────────────────────────────────────────────────────────────────────────────────────────────────────
static void schedKick();

// What a wait is for, for the fallback's statistics (IntelXeStats::missedBy).
enum WaitKind : uint8_t {
	kWaitSync,       // SYNCOBJ_WAIT / TIMELINE_WAIT
	kWaitClear,      // a CPU map waiting for the BO's VRAM clear
	kWaitTlb,        // a GuC TLB invalidation (G2H)
	kWaitTeardown,   // queue teardown: jobs, schedule disable, deregistration (G2H)
	kWaitRing,       // ring space for a kernel job
	kWaitSubmit,     // an in-fence point to be materialized by another thread's submit
	kWaitDeps,       // in-fences of a bind or a bind-queue exec, waited on the CPU
	kWaitKernel,     // bring-up jobs, display VRAM clears
	kWaitSurface,    // SURFACE_FENCE_WAIT
	kWaitMigrate,    // a BO migration copy
	kWaitKinds
};

// timeoutAbs: ns of the mach_absolute_time clock; 0 = poll; INT64_MAX = forever. c: the client whose lock the caller
// holds (dropped while asleep, EINTR once the client is being destroyed), or nullptr (kernel waits: nothing dropped).
// With interrupts the sleep is woken by them; a 2 ms slice catches a missed one (G2H is then read here). A slice that
// runs out just as the condition comes true while its interrupt is still pending or being handled is a race with the
// wakeup, not a missed interrupt (long jobs hit it in ~1-3% of waits: the slice expires within the 20-80 us between the
// seqno landing and the wakeup), so the hardware and the handler are looked at first; only a completion that neither an
// interrupt on its way nor G2H read here explains is counted as missed (debug.intelxe.stats). Without interrupts: poll
// every 5 us for ~0.5 ms, then every 1 ms.
static void tlbStallCheck();
template <typename F> static int waitUntil(XeClientState *c, int64_t timeoutAbs, WaitKind kind, F done) {
	for (uint32_t i = 0;; i++) {
		const uint32_t gen = gWakeGen;
		__asm__ volatile("" : : : "memory");
		if (done()) return 0;
		if (c && c->dying) return EINTR;
		const uint64_t now = nowNs();
		if (timeoutAbs <= 0 || (uint64_t)timeoutAbs <= now) return ETIME;
		if (c) IOLockUnlock(c->lock);
		int r = THREAD_AWAKENED;
		if (gDev->irqOn) {
			uint64_t until = now + 2000000ull, abs;
			if ((uint64_t)timeoutAbs < until) until = (uint64_t)timeoutAbs;
			nanoseconds_to_absolutetime(until, &abs);
			IOLockLock(gWaitLock);
			if (gWakeGen == gen) r = IOLockSleepDeadline(gWaitLock, const_cast<uint32_t *>(&gWakeGen), abs, THREAD_ABORTSAFE);
			IOLockUnlock(gWaitLock);
		} else if (i < 100) {
			IODelay(5);
		} else {   // without interrupts: 1 ms sleeps, interruptible like the interrupt path (IOSleep is THREAD_UNINT, and
			// a signalled process then never leaves a wait whose end only its own teardown would bring)
			uint64_t abs;
			nanoseconds_to_absolutetime(now + 1000000ull, &abs);
			IOLockLock(gWaitLock);
			if (gWakeGen == gen) r = IOLockSleepDeadline(gWaitLock, const_cast<uint32_t *>(&gWakeGen), abs, THREAD_ABORTSAFE);
			IOLockUnlock(gWaitLock);
		}
		if (c) IOLockLock(c->lock);
		if (r == THREAD_INTERRUPTED) return EINTR;
		if (c && c->dying) return EINTR;
		if (r == THREAD_TIMED_OUT || !gDev->irqOn) {
			// on its way: a wakeup that came as the deadline fired (the sleep then reports a timeout), or an interrupt
			// pending in the hardware or being handled
			const bool inflight = gDev->irqOn && (gWakeGen != gen || gDev->irqActive || gDev->irqPending());
			const uint32_t g2h = gStats.g2h[0] + gStats.g2h[1];
			gDev->gucPoll();
			tlbStallCheck();   // rb8d: an invalidation the GuC never answered
			if (gSchedHead) schedKick();
			if (gDev->irqOn) {
				gStats.fallbackWakes++;
				if (done()) {
					if (inflight) gStats.raced++;
					else if (gStats.g2h[0] + gStats.g2h[1] != g2h) gStats.g2hFound++;
					else {
						gStats.rescued++;
						gStats.missedBy[kind]++;
					}
					return 0;
				}
			}
		}
	}
}

// ─── GT power management knobs (debug.intelxe.slpc_*, gucrc, rc6, fw_hold; steps 17-20) ─────────────────────────
// SLPC parameters (Linux v7.2 abi/guc_actions_slpc_abi.h, xe_guc_pc.c). Defaults = what Linux does on BMG G21:
// min 1200 MHz (BMG_MIN_FREQ, pc_adjust_freq_bounds), max RP0, IGNORE_EFFICIENT_FREQUENCY = (min < RPe)
// (pc_set_min_freq), STRATEGIES = SLPC_OPTIMIZED_STRATEGY_COMPUTE, POWER_PROFILE = base. Linux also caps min/max at
// 2133 MHz (MERT_FREQ_CAP, Wa_22019338487) only until probe ends, and max at 2600 during L2 flushes; not done here.
static constexpr uint32_t slpcG = 0x1580000;
static IOBufferMemoryDescriptor *gSlpcMem;
static bool gSlpcUp;
int gSlpcMin = 1200;        // MHz; SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ (6)
int gSlpcMax = 0;           // MHz, 0 = RP0; SLPC_PARAM_GLOBAL_MAX_GT_UNSLICE_FREQ_MHZ (7)
int gSlpcIgnoreEff = -1;    // SLPC_PARAM_IGNORE_EFFICIENT_FREQUENCY (28): -1 = Linux rule (min < RPe), else 0/1
int gSlpcStrategy = 1;      // SLPC_PARAM_STRATEGIES (26): bit 0 = compute-optimized
int gSlpcProfile = 0;       // SLPC_PARAM_POWER_PROFILE (27): 0 base, 1 power saving; -1 = do not send
int gSlpcParamId = -1, gSlpcParamVal;   // step 17 also sends this one raw parameter (val -1 = PARAMETER_UNSET)
int gGucRc = 1;             // GUC_ACTION_HOST2GUC_SETUP_PC_GUCRC (0x3004): 1 FIRMWARE_CONTROL (default: what Linux
                            // xe_guc_rc_enable sends), 0 HOST_CONTROL, -1 not sent (the behaviour before 2026-10-28 20:40).
                            // Measured: without it the GuC still entered RC6 after ~50 us idle, and a job submitted to an
                            // idle GT took 240-270 us to start; with it, 70-90 us (rtt). CPU reads of VRAM after >=100 us
                            // idle still cost 130-190 us in both (only a GT kept out of RC6 avoids that).
int gRc6Ctl = -1;           // RC_CONTROL: -1 untouched (current), 0 RC6 off (xe_gt_idle_disable_c6),
                            // 1 host RC6 on with Linux's 5 s hysteresis (xe_gt_idle_enable_c6)
int gFwHold;                // a second GT forcewake requester (FORCEWAKE_MT bit 1): 1 keeps the GT out of RC6
int gMmioAddr;              // step 20 reads this BAR0 offset (read only)

static uint32_t mhzOf(uint32_t raw) { return (raw * 50 + 1) / 3; }

void IntelXe::slpcSet(uint32_t id, uint32_t value) {
	uint32_t set[4] = {0x3003, 6u << 8 | 2, id, value};   // SLPC_EVENT(PARAMETER_SET, 2)
	ctSend(set, 4);
}

void IntelXe::slpcParams() {
	const uint32_t cap = rd(0x138000), rp0 = mhzOf(cap & 0x1ff), rpn = mhzOf(cap >> 16 & 0x1ff),
		rpe = mhzOf(rd(0x13800c) & 0x1ff);
	uint32_t max = gSlpcMax > 0 ? (uint32_t)gSlpcMax : rp0, min = gSlpcMin > 0 ? (uint32_t)gSlpcMin : 1200;
	if (max > rp0) max = rp0;
	if (max < rpn) max = rpn;
	if (min < rpn) min = rpn;
	if (min > rp0) min = rp0;
	const uint32_t ignoreEff = gSlpcIgnoreEff >= 0 ? (uint32_t)(gSlpcIgnoreEff != 0) : (uint32_t)(min < rpe);
	slpcSet(7, max);
	slpcSet(28, ignoreEff);   // before MIN, as pc_set_min_freq does
	slpcSet(6, min);
	slpcSet(26, (uint32_t)gSlpcStrategy);
	if (gSlpcProfile >= 0) slpcSet(27, (uint32_t)gSlpcProfile);
	say("SLPC params: min %u max %u MHz (RPn %u RPe %u RP0 %u), ignore-efficient %u, strategies 0x%x, profile %d\n", min,
		max, rpn, rpe, rp0, ignoreEff, gSlpcStrategy, gSlpcProfile);
}

void IntelXe::gucRcSetup(uint32_t mode) {
	uint32_t act[2] = {0x3004, mode};   // GUC_ACTION_HOST2GUC_SETUP_PC_GUCRC
	ctSend(act, 2);
	say("GUCRC setup: %s\n", mode ? "FIRMWARE_CONTROL" : "HOST_CONTROL");
}

// xe_gt_idle_enable_c6 / xe_gt_idle_disable_c6 (needs the GT awake: called with forcewake held, or takes it).
void IntelXe::rc6Control(int on) {
	bool fw = forcewake(true);
	if (on) {
		wr(0xa0ac, 0x3B9ACA);                           // RC_IDLE_HYSTERSIS: 1.28 us units, 5 s
		wr(0xa090, 1u << 31 | 1u << 28 | 1u << 18);     // RC_CONTROL: HW_ENABLE | TO_MODE | RC6_ENABLE
	} else {
		wr(0xa090, 0);                                  // RC_CONTROL
		wr(0xa094, 0);                                  // RC_STATE
	}
	if (fw) forcewake(false);
	say("RC_CONTROL %s: now 0x%08x\n", on ? "host RC6 on" : "RC6 off", rd(0xa090));
}

// FORCEWAKE_MT has one bit per requester (FORCEWAKE_KERNEL bit 0 is what forcewake() uses); bit 1 is ours alone, so
// forcewake(false) elsewhere does not drop the hold.
bool IntelXe::fwHold(bool on) {
	if (!mapBar0()) return false;
	// the GT and the render domain (RC6 is the render well's power-down), requester bit 1 in both, as forcewake() takes
	// bit 0; released in the reverse order
	uint32_t ack = 0, rack = 0;
	bool ok;
	if (on) {
		wr(FORCEWAKE_GT, 0x20002);
		ok = waitFor(FORCEWAKE_ACK_GT_MTL, 2, 2, 50000, &ack);
		wr(FORCEWAKE_RENDER, 0x20002);
		ok = waitFor(FORCEWAKE_ACK_RENDER, 2, 2, 50000, &rack) && ok;
	} else {
		wr(FORCEWAKE_RENDER, 0x20000);
		ok = waitFor(FORCEWAKE_ACK_RENDER, 2, 0, 50000, &rack);
		wr(FORCEWAKE_GT, 0x20000);
		ok = waitFor(FORCEWAKE_ACK_GT_MTL, 2, 0, 50000, &ack) && ok;
	}
	LOG("GT+render forcewake hold %s: %s (ack 0x%08x / 0x%08x)", on ? "on" : "off", ok ? "ok" : "NO ACK", ack, rack);
	return ok;
}

// Step 17: SLPC parameters now (from the sysctls), the raw parameter if any, then QUERY_TASK_STATE.
int IntelXe::slpcStep() {
	if (!up || !gSlpcUp || !gSlpcMem) { say("SLPC not running (bring the GPU up first)\n"); return ENXIO; }
	slpcParams();
	if (gSlpcParamId >= 0) {
		if (gSlpcParamVal == -1) {
			uint32_t unset[3] = {0x3003, 7u << 8 | 1, (uint32_t)gSlpcParamId};   // SLPC_EVENT(PARAMETER_UNSET, 1)
			ctSend(unset, 3);
			say("SLPC param %d unset\n", gSlpcParamId);
		} else {
			slpcSet((uint32_t)gSlpcParamId, (uint32_t)gSlpcParamVal);
			say("SLPC param %d = %d\n", gSlpcParamId, gSlpcParamVal);
		}
	}
	uint32_t q[4] = {0x3003, 5u << 8 | 2, slpcG, 0};   // SLPC_EVENT(QUERY_TASK_STATE, 2)
	ctSend(q, 4);
	IODelay(5000);
	auto *sd = static_cast<volatile uint32_t *>(gSlpcMem->getBytesNoCopy());
	flushRange(gSlpcMem->getBytesNoCopy(), 256);
	const uint32_t st = sd[32], fr = sd[33];   // task_state_data at +128: status, freq (units of 50/3 MHz)
	say("SLPC global state %u; task status 0x%08x; unslice min %u max %u MHz (task state, may lag one query)\n", sd[1], st,
		mhzOf(fr >> 8 & 0xff), mhzOf(fr & 0xff));
	return 0;
}

// Step 18: what the GT's power state machinery is doing. First without forcewake (C-state and residency counters
// are readable while the GT sleeps: xe_guc_pc_c_status, xe_guc_pc_rc6_residency), then the RC6/power-gate controls.
int IntelXe::powerReport() {
	if (!mapBar0()) return ENXIO;
	const uint32_t wp1 = rd(0xc60), rc6a = rd(0x138108), mc6a = rd(0x138048), core = rd(0x138060);
	IOSleep(100);   // gDevLock only: GPU clients do not wait for it
	const uint32_t rc6b = rd(0x138108), mc6b = rd(0x138048), wp1b = rd(0xc60);
	const uint32_t cc = wp1 >> 9 & 0xf, ccb = wp1b >> 9 & 0xf;
	say("GT C-state (MTL_MIRROR_TARGET_WP1 12:9) %u then %u (0 = C0, 6 = C6, 15 = deeper/reset), CAGF %u MHz\n", cc, ccb,
		mhzOf(wp1 & 0x1ff));
	say("RC6 residency %u%% of the last 100 ms (GT_GFX_RC6 %u -> %u, 1.28 us units); media MC6 %u%%; GT_CORE_STATUS 0x%08x\n",
		(uint32_t)((uint64_t)(rc6b - rc6a) * 128 / 100000), rc6a, rc6b, (uint32_t)((uint64_t)(mc6b - mc6a) * 128 / 100000),
		core);
	say("forcewake ack GT 0x%08x (bit 0 kernel, bit 1 hold %s)\n", rd(FORCEWAKE_ACK_GT_MTL), gFwHold ? "on" : "off");
	if (!forcewake(true)) return ETIMEDOUT;
	say("RC_CONTROL 0x%08x RC_STATE 0x%08x RC_IDLE_HYSTERSIS 0x%08x POWERGATE_ENABLE 0x%08x; sysctls gucrc %d rc6 %d\n",
		rd(0xa090), rd(0xa094), rd(0xa0ac), rd(0xa210), gGucRc, gRc6Ctl);
	forcewake(false);
	return 0;
}

// xe_vram.c tile_vram_size / get_flat_ccs_offset (graphics >= 20): what BOs may use ends where flat CCS storage
// starts: XE2_FLAT_CCS_BASE_RANGE_LOWER/UPPER give it in the hardware view (bits 39:32 and 31:6), times the enabled
// L3 nodes (MIRROR_FUSE3 31:16) for the software view, rounded down to 4 KB; it must leave room for the CCS (tile/512)
// below GSM. DSM (stolen, DSMBASE) and GSM lie above it; the minimum of all three is taken, and an implausible value
// falls back to the 11 GB used before. Needs forcewake.
uint64_t IntelXe::vramUsableEnd() {
	const uint32_t range = rd(SG_TILE_ADDR_RANGE0);
	const uint64_t tile = (uint64_t)(range >> 8 & 0x3ff) << 30, tileOff = (uint64_t)(range >> 1 & 0x7f) << 30;
	const uint64_t gsm = ((uint64_t)rd(GSMBASE + 4) << 32 | rd(GSMBASE)) & ~0xfffffull;
	const uint64_t dsm = ((uint64_t)rd(DSMBASE + 4) << 32 | rd(DSMBASE)) & ~0xfffffull;   // BDSM_MASK 63:20
	const uint32_t lo = rd(0x8800), hi = rd(0x8804), nodes = (uint32_t)__builtin_popcount(rd(0x9118) >> 16);
	uint64_t ccs = (((uint64_t)(hi & 0xff) << 32) | (lo & 0xffffffc0u)) * nodes;
	ccs &= ~0xfffull;
	const uint64_t ccsSize = tile / 512;
	uint64_t end = ccs;
	const bool ccsOk = (lo & 1) && tile && ccs > tile / 2 && ccs + ccsSize <= gsm && ccs <= tile;
	if (!ccsOk) end = gsm > ccsSize ? gsm - ccsSize : 0;
	if (dsm && dsm < end) end = dsm;
	if (gsm && gsm < end) end = gsm;
	end = (end - tileOff) & ~0xffffull;
	const bool sane = tile && end >= (1ull << 30) && end <= tile;
	say("VRAM: tile %llu MB at %llu GB, flat CCS base 0x%llx (lower 0x%08x upper 0x%08x, %u L3 nodes)%s, DSM 0x%llx, GSM "
		"0x%llx -> BOs end at 0x%llx%s\n", tile >> 20, tileOff >> 30, ccs, lo, hi, nodes, ccsOk ? "" : " IMPLAUSIBLE",
		dsm, gsm, sane ? end : 0x2c0000000ull, sane ? "" : " (fallback 11 GB)");
	return sane ? end : 0x2c0000000ull;
}

// xe_guc_pc_start: SLPC (GuC frequency and power management) with an 8 KB shared page, RESET, wait for RUNNING, then the
// parameters of slpcParams (Linux v7.2 xe_guc_pc.c: max = RP0, BMG's 1200 MHz min from pc_adjust_freq_bounds,
// IGNORE_EFFICIENT_FREQUENCY = min < RPe from pc_set_min_freq, the compute-optimized strategy and power profile "base"),
// each overridable at runtime with the debug.intelxe.slpc_* sysctls and step 17. If it does not come up, request RP0 directly
// as pc_set_cur_freq does, or the GT stays at RPn (400 MHz). Then GuC RC (xe_guc_rc_enable). Forcewake held by the caller.
// guc_init_global_schedule_policy (xe_guc_submit_enable, after every load of the primary GuC): UPDATE_SCHEDULING_
// POLICIES_KLV with RENDER_COMPUTE_YIELD (80 ms quantum, 20% to the CCS), since the GT has compute engines. Without it
// the GuC prefers RCS whenever RCS and CCS hold work of different address spaces, and a compute job can wait behind a
// busy render engine without bound (the job watchdog would then see a job the GuC never started). debug.intelxe.rc_yield.
int gRcYield = 1;
void IntelXe::schedPolicyInit() {
	if (!gRcYield) return;
	uint32_t a[4] = {0x509, 0x1001u << 16 | 2, 80, 20};
	ctSend(a, 4, 0, true);
}

void IntelXe::pcStart() {
	schedPolicyInit();   // pcStart follows every primary GuC load (bring-up, GT reset, resume)
	const uint32_t rp0Raw = rd(0x138000) & 0x1ff;
	if (!gSlpcMem) gSlpcMem = alloc(0x2000, true);
	IOBufferMemoryDescriptor *slpc = gSlpcMem;
	bool slpcUp = false;
	if (slpc && !ggttMap(slpc, slpcG)) {
		auto *sd = static_cast<volatile uint32_t *>(slpc->getBytesNoCopy());
		bzero(slpc->getBytesNoCopy(), 0x2000);
		sd[0] = 0x2000;   // header.size
		flushRange(slpc->getBytesNoCopy(), 0x2000);
		uint32_t reset[4] = {0x3003, 0u << 8 | 2, slpcG, 0};   // SLPC_EVENT(RESET, 2)
		ctSend(reset, 4);
		for (int i = 0; i < 1000 && !slpcUp; i++) {
			flushRange(slpc->getBytesNoCopy(), 64);
			slpcUp = sd[1] == 3;   // header.global_state == SLPC_GLOBAL_STATE_RUNNING
			if (!slpcUp) IODelay(1000);
		}
		gSlpcUp = slpcUp;
		if (slpcUp) slpcParams();
		say("SLPC %s (global state %u)\n", slpcUp ? "running" : "did NOT start", sd[1]);
	}
	if (slpcUp && gGucRc >= 0) gucRcSetup((uint32_t)gGucRc);   // default 1: GuC RC firmware control (xe_guc_rc_enable)
	if (gRc6Ctl >= 0) rc6Control(gRc6Ctl);                      // default -1: RC_CONTROL untouched, as before
	if (!slpcUp) {
		wr(0xa024, 2u << 9);
		wr(0xa008, rp0Raw << 23);
		IODelay(300);
		wr(0xa024, 0);
	}
	// xe_gt_idle_enable_pg for the primary GT (no video engines on it): RENDER_POWERGATE_ENABLE in POWERGATE_ENABLE, from
	// the host as Linux does at GT init and resume; GuC RC alone leaves power gating off. The hysteresis is SLPC's.
	wr(0xa210, 1u << 0);
}

// ─── bring-up: engine state, GuC with the copy class, CT, VRAM and the shared pages (gDevLock) ──────────────────
int IntelXe::bringUp() {
	if (up) return 0;
	say("bring-up\n");
	int err = firstBatchSetupOnly();
	if (err) return err;
	if (!bar2) bar2 = pci->mapDeviceMemoryWithRegister(0x18, kIOMapWriteCombineCache);
	if (!bar2) return ENXIO;
	if (!forcewake(true)) return ETIMEDOUT;
	vramEnd = vramUsableEnd();
	// rb7j: the GuC's UM queues at the top stay out of the BO allocator (set here too, in case no full GuC load came
	// first; the GuC loads that follow use the same range)
	if (gUsm && !gUmDpa) gUmDpa = umDpaFor(vramEnd);
	if (gUsm && gUmDpa && gUmDpa < vramEnd) vramEnd = gUmDpa;
	vramVisible = bar2->getLength() < vramEnd ? bar2->getLength() & ~0xffffull : vramEnd;
	say("BAR2 window %llu MB, VRAM for BOs %llu MB\n", vramVisible >> 20, vramEnd >> 20);
	wr(0x4194, rd(0x4194) | 1u << 6);   // Wa_16023588340: XE2_GAMREQSTRM_CTRL CG_DIS_CNTLBUS
	wr(0xb194 + 3 * 8, 0xf);             //                 XEHPC_L3CLOS_MASK(3)
	gGeomDss = rd(XELP_GT_GEOMETRY_DSS_ENABLE);
	gCompDss = rd(XEHP_GT_COMPUTE_DSS_ENABLE);
	gEuMask = rd(XELP_EU_ENABLE);
	uint32_t fuse3 = rd(0x9118);         // MIRROR_FUSE3: node enable 31:16, L3 mode pattern 7:4
	gL3Mask = 0;
	for (uint32_t b = 0; b < 16; b++)
		if (fuse3 >> 16 & 1u << b) gL3Mask |= (uint64_t)((fuse3 >> 4) & 0xf) << (b * 4);
	// FF_MODE is not masked: keep the power-on value, set DIS_MESH_PARTIAL_AUTOSTRIP | DIS_MESH_AUTOSTRIP and
	// VS_HIT_MAX_VALUE 25:20 = 0x3f (Wa_14021490052, lrc tuning)
	gFfMode = (rd(0x6210) & ~(0x3fu << 20)) | 1u << 16 | 1u << 15 | 0x3fu << 20;
	uint32_t c0 = rd(0xd00);             // RPM_CONFIG0 (xe_gt_clock_init)
	static const uint32_t freq[] = {24000000, 19200000, 38400000, 25000000};
	gRefClock = ((c0 >> 3) & 7) < 4 ? freq[(c0 >> 3) & 7] >> (3 - ((c0 >> 1) & 3)) : 0;
	pcStart();
	forcewake(false);
	devId = pci->configRead16(kIOPCIConfigDeviceID);
	revId = pci->configRead8(kIOPCIConfigRevisionID);
	size_t sz = sizeof(gSysTotal);
	sysctlbyname("hw.memsize", &gSysTotal, &sz, nullptr, 0);

	IOLockLock(gResLock);
	const bool vramOk = buddyInit(&gVis, vramUserStart, vramVisible) && buddyInit(&gHi, vramVisible, vramEnd);
	IOLockUnlock(gResLock);
	if (!vramOk) return ENOMEM;
	if (scratchInit()) return ENOMEM;
	if (!gSeqnoMem) gSeqnoMem = alloc(kSlots * kSlotBytes, false);
	if (!gSeqnoMem || ggttMap(gSeqnoMem, seqnoG)) return ENOMEM;
	say("up: DSS 0x%x/0x%x EU 0x%x L3 0x%llx refclk %u, %llu MB RAM\n", gGeomDss, gCompDss, gEuMask, gL3Mask, gRefClock,
		gSysTotal >> 20);
	if (gIrqWanted && !irqOn) irqInit();
	if (irqOn) irqGtEnable();   // also when IntelXeFB's display handler set the MSI up first
	recordGolden();
	vramClearInit();
	// xe_device_probe brings both GTs up: the media GT's GuC, HuC and SLPC now, so its VCS engines are listed from the
	// first query (debug.intelxe.video=0 or -intelxe_novideo: not; a failure is retried by mediaUp, at most every 10 s).
	if (gVideoWanted) {
		const int merr = mediaGucLoad();
		say("media GT at bring-up: %s (%d)\n", merr ? "FAILED" : "up", merr);
	}
	up = true;
	return 0;
}

// ─── G2H (xe_guc_ct.c g2h_read / parse_g2h_msg / process_g2h_msg) ─────────────────────────────────────────────────
// G2H of both GuCs (the media one once its CT is up). Called by the interrupt handler for the GuC that raised
// GUC2HOST, and by waiters that time out (a missed interrupt) or run without interrupts.
void IntelXe::gucPoll() {
	gucPollGt(0);
	if (mCtOn) gucPollGt(MEDIA_GT_GSI_OFFSET);
}

// The last H2G messages per GuC by fence, for naming the action a FAST_REQUEST failure reply refers to (fast_req_report).
H2gHistory gH2gHist[2];

static const char *h2gActionOf(uint32_t g, uint32_t fence) {
	static char buf[2][16];
	for (uint32_t i = 0; i < 64; i++)
		if (gH2gHist[g].fence[i] == (fence & 0xffff) && gH2gHist[g].action[i]) {
			snprintf(buf[g], sizeof(buf[g]), "0x%04x", gH2gHist[g].action[i]);
			return buf[g];
		}
	return "unknown";
}

void IntelXe::gucPollGt(uint32_t gt) {
	const uint32_t g = gtIdx(gt);
	IOLockLock(gG2hLock[g]);
	IOBufferMemoryDescriptor *mem = gt ? mCtMem : ctMem;
	uint32_t &head = gt ? mG2hHead : g2hHead;
	if (!mem || ctBroken[g]) {
		IOLockUnlock(gG2hLock[g]);
		return;
	}
	auto *base = static_cast<uint8_t *>(mem->getBytesNoCopy());
	auto *desc = reinterpret_cast<volatile uint32_t *>(base + 0x800);   // head, tail, status
	auto *cmds = reinterpret_cast<volatile uint32_t *>(base + 0x2000);
	const uint32_t size = 0x8000;
	flushRange(base + 0x800, 64);
	const uint32_t tail = desc[1], status = desc[2];
	bool wake = false, kick = false, watch = false;
	static uint32_t lastStatus[2];
	if (status != lastStatus[g]) {   // g2h_read: OVERFLOW/UNDERFLOW/MISMATCH mean corruption, DISABLED a stopped GuC
		lastStatus[g] = status;
		if (status) g2hLog("G2H%s descriptor status 0x%x\n", gt ? " media" : "", status);
	}
	if (tail >= size || head >= size) {
		// Offsets out of range: reading on would index outside the ring. The channel is broken (CT_DEAD in Linux, which
		// then resets the GT; here it is reported and reading stops until the next CT setup).
		ctBroken[g] = true;
		g2hLog("G2H%s channel broken: status 0x%x head %u tail %u\n", gt ? " media" : "", status, head, tail);
		if (!gt) requestReset("G2H channel broken");   // CT_DEAD -> xe_gt_reset
		IOLockUnlock(gG2hLock[g]);
		return;
	}
	while (head != tail) {
		flushRing(cmds, head, size);
		const uint32_t avail = (tail - head + size) % size, hdr = cmds[head], len = (hdr & 0xff) + 1;
		if (len < 2 || len > avail || len > 256) {   // GUC_CTB_MSG_MAX_LEN
			ctBroken[g] = true;
			g2hLog("G2H%s message length %u with %u dwords available (header 0x%08x): channel broken\n", gt ? " media" : "",
				len, avail, hdr);
			if (!gt) requestReset("G2H channel broken");   // g2h_read "reset required": CT_DEAD -> kick_reset
			break;
		}
		const uint32_t hxg = cmds[(head + 1) % size], type = hxg >> 28 & 7, action = hxg & 0xffff;
		const uint32_t d0 = len > 2 ? cmds[(head + 2) % size] : 0, d1 = len > 3 ? cmds[(head + 3) % size] : 0;
		gStats.g2h[g]++;
		if (!(hxg >> 31)) {
			g2hLog("G2H%s 0x%08x: origin is not the GuC\n", gt ? " media" : "", hxg);
			if (!gt) {   // parse_g2h_msg: CT_DEAD(PARSE_G2H_ORIGIN), then xe_guc_ct.c kick_reset
				ctBroken[g] = true;
				requestReset("G2H message not from the GuC");
				break;
			}
		} else if (type == 1) {   // GUC_HXG_TYPE_EVENT
			switch (action) {
				case 0x7001:   // TLB_INVALIDATION_DONE {seqno}: completions arrive in order (xe_tlb_inval_done_handler)
					if ((int32_t)(d0 - (gt ? mTlbDone : tlbDone)) > 0) (gt ? mTlbDone : tlbDone) = d0;
					wake = kick = true;
					break;
				case 0x1002:   // SCHED_CONTEXT_MODE_DONE {guc_id, runnable}: only a disable is waited for
					if (d0 < kMaxGucIds && !d1) {
						gGucState[g][d0] |= kSchedOff;
						if (gTdrWait[g][d0]) watch = true;   // the job watchdog waits for it
					}
					wake = true;
					break;
				case 0x4600:   // DEREGISTER_CONTEXT_DONE {guc_id}
					if (d0 < kMaxGucIds) gGucState[g][d0] |= kDeregDone;
					wake = true;
					break;
				case 0x1008:   // CONTEXT_RESET_NOTIFICATION {guc_id}
				case 0x6000:   // NOTIFY_MEMORY_CAT_ERROR {guc_id, type}
					if (action == 0x6000) gRecSt.catErrors++;
					// The GuC stopped that context: as xe_guc_exec_queue_reset_trigger_cleanup, the submit worker takes
					// the queue off the engine and the watchdog bans it once the GuC confirms (queueKickOff): what it had
					// pending completes, waiters return and Mesa reads the ban as a lost device.
					if (d0 < kMaxGucIds) OSBitOrAtomic(1u << (d0 % 32), const_cast<uint32_t *>(&gBanReq[g][d0 / 32]));
					gStats.gucCtxResets++;
					// rb8c: the CAT error's type dword only when the GuC sent one (0xdeadbeef XE_GUC_CAT_ERR_TYPE_INVALID: none)
					g2hLog("G2H%s %s for guc_id %u (%s0x%x): queue to be banned\n", gt ? " media" : "",
						action == 0x1008 ? "CONTEXT_RESET" : "MEMORY_CAT_ERROR", d0, len > 3 ? "type " : "no type, ",
						len > 3 ? d1 : 0xdeadbeefu);
					wake = kick = true;
					break;
				case 0x6002:   // REPORT_PAGE_FAULT_REQ_DESC: struct xe_guc_pagefault_desc, 4 dwords (rb7j)
					// xe_guc_pagefault_handler: parsed and answered by the submit worker (faultsDrain), never here under
					// this GuC's G2H lock; until the answer the GuC keeps the faulting context stalled
					if (!gt && len == 6) {
						uint32_t dw[4];
						for (uint32_t k = 0; k < 4; k++) dw[k] = cmds[(head + 2 + k) % size];
						faultQueue(dw);
						kick = true;
					} else {
						g2hLog("G2H%s page fault with %u dwords: ignored\n", gt ? " media" : "", len - 2);
					}
					break;
				case 0x1009:   // ENGINE_FAILURE_NOTIFICATION {guc_class, instance, reason}
					// xe_guc_exec_queue_reset_failure_handler: the GuC could not reset the engine -> xe_gt_reset_async
					g2hLog("G2H%s ENGINE_FAILURE class %u instance %u reason 0x%x\n", gt ? " media" : "", d0, d1,
						len > 4 ? cmds[(head + 4) % size] : 0);
					if (!gt) requestReset("GuC engine reset failed");
					break;
				case 0x8004: case 0x8005:   // NOTIFY_CRASH_DUMP_POSTED, NOTIFY_EXCEPTION: the GuC firmware died
					g2hLog("G2H%s GuC %s (0x%x)\n", gt ? " media" : "", action == 0x8005 ? "EXCEPTION" : "CRASH DUMP", d0);
					if (!gt) requestReset("GuC exception");   // a dead GuC means a GT reset
					break;
				default:
					g2hLog("G2H%s event 0x%08x %08x %08x\n", gt ? " media" : "", hxg, d0, d1);
			}
		} else if (type == 6 || type == 5 || type == 3) {   // RESPONSE_FAILURE, NO_RESPONSE_RETRY/BUSY
			// to a FAST_REQUEST (parse_g2h_response): the GuC rejected an H2G nobody waits for
			gStats.h2gFailed++;
			g2hLog("G2H%s H2G fence 0x%x (action %s) %s: error 0x%x hint %u\n", gt ? " media" : "", hdr >> 16,
				h2gActionOf(g, hdr >> 16), type == 6 ? "FAILED" : type == 5 ? "retry" : "busy", hxg & 0xffff, hxg >> 16 & 0xfff);
		} else if (type != 7) {   // RESPONSE_SUCCESS: replies to REQUESTs (HuC authentication), not waited for here
			g2hLog("G2H%s unexpected type %u: 0x%08x\n", gt ? " media" : "", type, hxg);
			if (!gt) {   // parse_g2h_msg: CT_DEAD(PARSE_G2H_TYPE), then kick_reset
				ctBroken[g] = true;
				requestReset("G2H message of unknown type");
				break;
			}
		}
		head = (head + len) % size;
	}
	desc[0] = head;
	flushRange(base + 0x800, 64);
	IOLockUnlock(gG2hLock[g]);
	if (wake) wakeAll();
	if (kick) schedKick();
	if (watch && gWatchCall) thread_call_enter(gWatchCall);
}

// ─── TLB invalidation (xe_guc_tlb_inval.c): the full heavy invalidation with cache flush, on every GuC whose engines
// may hold the VM's translations (media included). gTlbLock keeps seqnos and the order the GuCs see them the same, so
// "done up to N" means every earlier one is done too.
static uint64_t gTlbIssueNs;   // rb8b: when the last TLB invalidation went to the GuC (engineDump)
static uint64_t gTlbIssueAt[64];   // rb8d: when seqno s was issued, at [s & 63] (tlbStallCheck); gTlbLock
static uint32_t tlbInvalidateIssue() {
	gTlbIssueNs = nowNs();
	IOLockLock(gTlbLock);
	const uint32_t seq = ++gTlbSeq;
	gDev->tlbSeqno = seq;
	gTlbIssueAt[seq & 63] = gTlbIssueNs;
	uint32_t a[3] = {0x7000, seq, 0x80000000u};   // XE_GUC_TLB_INVAL_FULL, heavy, flush cache
	// A primary GT reset empties its TLBs and counts each invalidation issued meanwhile as done; the media GT is not
	// reset with it, so its TLBs are still invalidated.
	if (!gResetting) gDev->guc(a, 3);
	if (gDev->mediaOn()) gDev->guc(a, 3, MEDIA_GT_GSI_OFFSET);
	IOLockUnlock(gTlbLock);
	gStats.tlbInvals++;
	return seq;
}
// send_tlb_inval_ppgtt with XE_GUC_TLB_INVAL_PAGE_SELECTIVE (graphics_xe2 has_range_tlb_inval, no context
// invalidation): {action, seqno, PAGE_SELECTIVE | HEAVY | FLUSH_CACHE, ASID, start low, start high, log2(length) - 12},
// the range widened as normalize_invalidation_range does (a power of two aligned to itself, at least 16 MB from 2 MB up).
// debug.intelxe.range_inval=0: always the full invalidation.
int gRangeInval = 1;
// rb7k: debug.intelxe.tlb_every_batch=1: every batch of a queue on a scratch VM invalidates the TLB before it runs, as Linux xe
// does for a VM with scratch pages (xe_vm.c: vm->batch_invalidate_tlb for XE_VM_FLAG_SCRATCH_PAGE; xe_sched_job_arm hands it
// to the ring ops). 0 (default): only the first batch of a queue after the VM's page tables changed (rb7j and before).
int gTlbEveryBatch = 0;
static uint32_t tlbInvalidateRange(uint32_t asid, uint64_t start, uint64_t end) {
	uint32_t lg = 0;
	if (!gRangeInval || !xeTlbInvalRange(start, end, &start, &lg)) return tlbInvalidateIssue();   // xe_tlbinval.h
	gTlbIssueNs = nowNs();
	IOLockLock(gTlbLock);
	const uint32_t seq = ++gTlbSeq;
	gDev->tlbSeqno = seq;
	gTlbIssueAt[seq & 63] = gTlbIssueNs;
	uint32_t a[7] = {0x7000, seq, 1u | 1u << 31, asid, (uint32_t)start, (uint32_t)(start >> 32), lg - 12};
	if (!gResetting) gDev->guc(a, 7);   // as tlbInvalidateIssue
	if (gDev->mediaOn()) gDev->guc(a, 7, MEDIA_GT_GSI_OFFSET);
	IOLockUnlock(gTlbLock);
	gStats.tlbInvals++;
	gStats.tlbRanged++;
	return seq;
}

static bool tlbSeqDone(uint32_t seq) {
	return (int32_t)(gDev->tlbDone - seq) >= 0 && (!gDev->mediaOn() || (int32_t)(gDev->mTlbDone - seq) >= 0);
}

// rb8d: TLB invalidations the watchdog counted as done (tlbStallCheck), and how often it did
static struct { uint32_t forced, forcedEvents; } gTlbSt;

// rb8d: an invalidation the GuC never answers. VM_BIND out-fences wait for invalidations now, and completions are counted
// in order, so one lost TLB_INVALIDATION_DONE would hold every bind fence after it. As xe_tlb_inval.c
// xe_tlb_inval_fence_timeout signals (with -ETIME) every pending invalidation fence issued longer ago than its
// timeout_delay and moves past it, an invalidation still pending kTlbTimeoutNs after it was issued counts as done
// (logged and counted). rb8e: that delay is Linux's (xe_tlbinval.h xeTlbTimeoutNs, xe_guc_tlb_inval.c
// tlb_inval_timeout_delay: HZ / 4 plus twice the CT's queue time, 2.25 s with the 4 KB H2G ring); WAS a fixed 1 s.
// Not during a GT reset or sleep, whose end counts every invalidation done anyway. From the submit worker and from
// waits' 2 ms fallback checks.
static constexpr uint64_t kTlbTimeoutNs = xeTlbTimeoutNs(kH2gBytes);
// rb8e: how long a caller waits for an invalidation fence (tlbWait, xeGgttInvalWait): the fence's own timeout and a
// margin, so the watchdog above ends a lost one first (Linux waits for the fence, xe_tlb_inval_fence_wait, which its
// timeout work signals); WAS 1 s, shorter than the timeout
static constexpr uint64_t kTlbWaitNs = kTlbTimeoutNs + 100000000ull;
static void tlbStallCheck() {
	if (!gDev || !gDev->up || gResetting || gSuspended || tlbSeqDone(gTlbSeq)) return;
	const bool media = gDev->mediaOn();
	const uint32_t t = gDev->tlbDone, m = gDev->mTlbDone, done = !media || xeSeqDone(m, t) ? t : m;   // the one behind
	uint64_t at[64];
	IOLockLock(gTlbLock);
	memcpy(at, gTlbIssueAt, sizeof(at));
	const uint32_t issued = gTlbSeq;
	IOLockUnlock(gTlbLock);
	const uint32_t to = xeTlbTimedOut(done, issued, at, nowNs(), kTlbTimeoutNs);
	if (to == done) return;
	IOLockLock(gTlbLock);
	if (!xeSeqDone(gDev->tlbDone, to)) gDev->tlbDone = to;
	if (media && !xeSeqDone(gDev->mTlbDone, to)) gDev->mTlbDone = to;
	IOLockUnlock(gTlbLock);
	gTlbSt.forced += to - done;
	gTlbSt.forcedEvents++;
	g2hLog("TLB invalidation %u..%u: no TLB_INVALIDATION_DONE within %llu ms of issue (done %u/%u): counted as done "
		"(xe_tlb_inval_fence_timeout)\n", done + 1, to, kTlbTimeoutNs / 1000000ull, t, m);
	wakeAll();
	schedKick();
}

// rb8d: the GGTT TLBs (xe_ggtt.c xe_ggtt_invalidate: each GT of the tile caches GGTT lookups, so ggtt_invalidate_gt_tlb on
// the primary and the media GT -> xe_tlb_inval_ggtt -> xe_guc_tlb_inval.c send_tlb_inval_ggtt). With the CT up, the GuC
// action TLB_INVALIDATION {seqno, XE_GUC_TLB_INVAL_GUC | MODE_HEAVY | FLUSH_CACHE} in the seqno space of the PPGTT
// invalidations, whose fence xe_tlb_inval_ggtt waits for (xeGgttInvalWait); without it (bring-up, GT reset, resume:
// the GuC is being loaded) the MMIO write PVC_GUC_TLB_INV_DESC1/DESC0 under forcewake, which Linux does not wait for
// either (send_tlb_inval_ggtt returns -ECANCELED, the fence signals at once). WAS the MMIO write always, on the one GT
// the range was mapped for, never waited for: a new queue's first job could run through the GGTT translation of the
// LRC that had used its slot's window before. Returns the seqno to wait for, 0 for none.
static struct { uint32_t guc, mmio, waits, timeouts, maxWaitUs; } gGgttSt;   // debug.intelxe.stats
uint32_t xeGgttInvalidate(uint32_t gt) {
	if (!gDev) return 0;   // set in IntelXe::start before any GGTT mapping
	const bool viaGuc = gDev->up && gTlbLock && !gResetting && !gSuspended && !gDev->suspended && !gWedged &&
		!gDev->h2gDead[0];
	if (!viaGuc) {
		gDev->ggttInvalidateMmio(gt);
		gGgttSt.mmio++;
		return 0;
	}
	const bool media = gDev->mediaOn();
	if (gt && !media) gDev->ggttInvalidateMmio(gt);   // a media GT whose GuC is being loaded: its MMIO path
	gTlbIssueNs = nowNs();
	IOLockLock(gTlbLock);
	const uint32_t seq = ++gTlbSeq;
	gDev->tlbSeqno = seq;
	gTlbIssueAt[seq & 63] = gTlbIssueNs;
	uint32_t a[3] = {0x7000, seq, xeTlbInvalOp(kXeTlbInvalGuc)};   // XE_GUC_ACTION_TLB_INVALIDATION, the GGTT
	const int err = gDev->guc(a, 3);
	if (media) gDev->guc(a, 3, MEDIA_GT_GSI_OFFSET);
	IOLockUnlock(gTlbLock);
	if (err) {   // the H2G did not go out: the MMIO path for this one; a later completion covers the seqno
		gDev->ggttInvalidateMmio(0);
		gGgttSt.mmio++;
		return 0;
	}
	gGgttSt.guc++;
	return seq;
}

// rb8d: xe_tlb_inval_ggtt's xe_tlb_inval_fence_wait, bounded (rb8e: kTlbWaitNs, just past the fence's own timeout,
// tlbStallCheck, which ends it first; WAS 1 s). Not with gResLock held: the callers wait before the window's slot or
// memory is handed out again.
bool xeGgttInvalWait(uint32_t seq) {
	if (!seq) return true;
	gGgttSt.waits++;
	const uint64_t t0 = nowNs();
	const int err = waitUntil(nullptr, (int64_t)(t0 + kTlbWaitNs), kWaitTlb, [&] { return tlbSeqDone(seq); });
	const uint64_t us = (nowNs() - t0) / 1000;
	if (us > gGgttSt.maxWaitUs) gGgttSt.maxWaitUs = (uint32_t)(us > 0xffffffffull ? 0xffffffffull : us);
	if (err == ETIME) {
		gGgttSt.timeouts++;
		g2hLog("GGTT invalidation %u: no completion within %llu ms (done %u/%u)\n", seq, kTlbWaitNs / 1000000ull,
			gDev->tlbDone, gDev->mTlbDone);
		return false;
	}
	return !err;
}

static bool tlbWait(XeClientState *c, uint32_t seq) {
	const int err = waitUntil(c, (int64_t)(nowNs() + kTlbWaitNs), kWaitTlb, [&] { return tlbSeqDone(seq); });
	if (err == ETIME) {
		gStats.tlbTimeouts++;
		g2hLog("TLB invalidation %u: no completion within %llu ms (done %u/%u)\n", seq, kTlbWaitNs / 1000000ull,
			gDev->tlbDone, gDev->mTlbDone);
	}
	return !err;
}

// ─── deferred frees (xe_pt.c: an unbind's TLB invalidation is a fence its pages wait for) ────────────────────────
// A BO whose last binding went away keeps its pages (wired userptr pages, a VRAM range) until the invalidation issued
// after the unbind has completed, and (rb8c) the jobs the VM had in flight then (deferSeal); a VM's page-table pages and bindings likewise once its last reference is gone. Entries
// with a nonzero owner belong to a VM_BIND still running (its invalidation is issued after all its ops); gResLock.
enum : uint8_t { kDefBo, kDefVm, kDefPt, kDefVram, kDefSys };
struct VramList { VBlock *blocks; bool hi; };   // kDefVram: the old VRAM of a BO that moved (or was purged)
struct SysList { IOBufferMemoryDescriptor *sys; DmaMap dma; };   // kDefSys (rb7r): a purged system BO's pages
static void vramListFree(VramList *l);
static void sysListFree(SysList *l) {
	l->dma.unmap();
	OSSafeReleaseNULL(l->sys);
	IOFree(l, sizeof(*l));
}
// rb8c: what an entry waits for besides its invalidation: the last job each queue of the VM had been handed when the
// bind sealed it, those not complete then (xe_tlbinval.h: xe frees page tables and BO memory only once the VM's dma-resv
// is idle, the in-flight execs included). One per seal, shared by the entries it covers (refs, gResLock); done caches
// the answer once every fence has signaled.
struct DeferJobs { uint32_t refs, n, cap; bool done; XeFence f[1]; };
static size_t deferJobsSize(uint32_t cap) { return sizeof(DeferJobs) + (cap - 1) * sizeof(XeFence); }
// heldBy: the entry's invalidation had completed while its jobs had not (counted once per entry: gStats.deferJobHeld)
struct XeDeferred { void *obj; uint32_t seq; uint8_t kind; bool heldBy; const void *owner; DeferJobs *jobs; };
static XeDeferred *gDef;
static uint32_t gNDef, gCapDef;
static void vmFreeNow(XeVm *vm);
static void ptTreeFree(PtNode *n);

static bool deferAdd(void *obj, uint8_t kind, uint32_t seq, const void *owner) {
	IOLockLock(gResLock);
	if (gNDef == gCapDef) {
		uint32_t nc = gCapDef ? gCapDef * 2 : 64;
		auto *n = static_cast<XeDeferred *>(IOMallocZero(nc * sizeof(XeDeferred)));
		if (!n) {
			IOLockUnlock(gResLock);
			return false;
		}
		if (gDef) {
			memcpy(n, gDef, gNDef * sizeof(XeDeferred));
			IOFree(gDef, gCapDef * sizeof(XeDeferred));
		}
		gDef = n;
		gCapDef = nc;
	}
	gDef[gNDef++] = {obj, seq, kind, false, owner, nullptr};
	IOLockUnlock(gResLock);
	return true;
}

// rb8c: the not yet completed last jobs of every queue of vm, into *out (nullptr: none to wait for). False: no memory.
static bool deferJobsSnap(const XeVm *vm, DeferJobs **out) {
	*out = nullptr;
	for (uint32_t cap = 8;;) {
		auto *dj = static_cast<DeferJobs *>(IOMalloc(deferJobsSize(cap)));
		if (!dj) return false;
		uint32_t n = 0, need = 0;
		IOLockLock(gSchedLock);
		for (const XeQueue *q = vm->queues; q; q = q->vmNext) {
			const XeFence f = {0, q->slot, q->gen, q->seqno};   // seqno: the last one handed out (FIFO jobs included)
			if (fenceSignaled(f)) continue;
			if (n < cap) dj->f[n++] = f;
			need++;
		}
		IOLockUnlock(gSchedLock);
		if (need <= cap) {
			if (!n) {
				IOFree(dj, deferJobsSize(cap));
				return true;
			}
			dj->refs = 0;
			dj->n = n;
			dj->cap = cap;
			dj->done = false;
			*out = dj;
			return true;
		}
		IOFree(dj, deferJobsSize(cap));
		cap = need + 4;   // queues came meanwhile: once more, larger
	}
}

static bool deferJobsDone(DeferJobs *dj) {   // gResLock
	if (!dj || dj->done) return true;
	for (uint32_t i = 0; i < dj->n; i++)
		if (!fenceSignaled(dj->f[i])) return false;
	dj->done = true;
	return true;
}

// The entries of one bind call (owner) wait for seq, the invalidation issued after all its ops, and (rb8c, vm given) for
// the jobs vm's queues had been handed by now: xe_pt.c puts the page tables an update releases with xe_bo_put_deferred
// and ttm_bo_release keeps their memory until the VM's dma-resv has signaled, BOOKKEEP (every exec) included; a VM-private
// BO shares that resv and an external one gets every exec's fence too (drm_gpuvm_resv_add_fence). WAS the invalidation
// alone: a page-table page or BO page could be reused (as another table, an LRC, a BO) while a batch that had translated
// through it was still running. Without memory for the snapshot the caller waits here for those jobs (5 s at most).
static void deferSeal(const void *owner, uint32_t seq, XeVm *vm = nullptr) {
	DeferJobs *dj = nullptr;
	if (vm && !deferJobsSnap(vm, &dj))
		waitUntil(nullptr, (int64_t)(nowNs() + 5000000000ull), kWaitDeps, [&] {
			bool idle = true;
			IOLockLock(gSchedLock);
			for (const XeQueue *q = vm->queues; q && idle; q = q->vmNext)
				idle = fenceSignaled(XeFence{0, q->slot, q->gen, q->seqno});
			IOLockUnlock(gSchedLock);
			return idle;
		});
	uint32_t sealed = 0;
	IOLockLock(gResLock);
	for (uint32_t i = 0; i < gNDef; i++)
		if (gDef[i].owner == owner) {
			gDef[i].owner = nullptr;
			gDef[i].seq = seq;
			gDef[i].jobs = dj;
			sealed++;
		}
	if (dj) dj->refs += sealed;
	IOLockUnlock(gResLock);
	if (dj && !sealed) IOFree(dj, deferJobsSize(dj->cap));
	if (vm && dj && sealed) vm->jobHeldSeals++;   // rb8b/c: engineDump
}

// CPU maps of a client that is gone whose mapping IOKit has not torn down yet (md still referenced by its IOMemoryMap):
// the BO stays until it has. gResLock.
struct XeLinger { IOMemoryDescriptor *md; XeBo *bo; XeLinger *next; };
static XeLinger *gLinger;
static void lingerAdd(IOMemoryDescriptor *md, XeBo *bo) {
	if (bo && md->getRetainCount() > 1) {
		auto *l = static_cast<XeLinger *>(IOMalloc(sizeof(XeLinger)));
		if (!l) return;   // no memory for the list: the BO is leaked rather than freed under a mapping
		IOLockLock(gResLock);
		*l = {md, bo, gLinger};
		gLinger = l;
		IOLockUnlock(gResLock);
		return;
	}
	md->release();
	boPut(bo);
}
static void lingerReap() {
	XeLinger *done = nullptr;
	IOLockLock(gResLock);
	for (XeLinger **pp = &gLinger; *pp;) {
		XeLinger *l = *pp;
		if (l->md->getRetainCount() > 1) {
			pp = &l->next;
			continue;
		}
		*pp = l->next;
		l->next = done;
		done = l;
	}
	IOLockUnlock(gResLock);
	while (XeLinger *l = done) {
		done = l->next;
		l->md->release();
		boPut(l->bo);
		IOFree(l, sizeof(*l));
	}
}

// Frees what has become safe. Called at every ioctl and by the submit worker after a TLB completion.
static void deferReap() {
	if (gLinger) lingerReap();
	if (!gNDef) return;
	for (;;) {
		XeDeferred ready[32];
		uint32_t n = 0, keep = 0;
		IOLockLock(gResLock);
		DeferJobs *drop[32];
		uint32_t nDrop = 0;
		for (uint32_t i = 0; i < gNDef; i++) {
			XeDeferred &d = gDef[i];
			const bool tlb = !d.owner && tlbSeqDone(d.seq), jobs = tlb && deferJobsDone(d.jobs);
			if (n < 32 && xeDeferReady(!d.owner, tlb, jobs)) {
				ready[n++] = d;
				if (d.jobs && !--d.jobs->refs) drop[nDrop++] = d.jobs;
				continue;
			}
			if (tlb && !d.heldBy) {   // rb8c: the invalidation is done, a job of the VM still runs
				d.heldBy = true;
				gStats.deferJobHeld++;
			}
			gDef[keep++] = d;
		}
		gNDef = keep;
		IOLockUnlock(gResLock);
		for (uint32_t i = 0; i < nDrop; i++) IOFree(drop[i], deferJobsSize(drop[i]->cap));
		for (uint32_t i = 0; i < n; i++) {
			if (ready[i].kind == kDefVm) vmFreeNow(static_cast<XeVm *>(ready[i].obj));
			else if (ready[i].kind == kDefPt) ptTreeFree(static_cast<PtNode *>(ready[i].obj));
			else if (ready[i].kind == kDefVram) vramListFree(static_cast<VramList *>(ready[i].obj));
			else if (ready[i].kind == kDefSys) sysListFree(static_cast<SysList *>(ready[i].obj));
			else boPut(static_cast<XeBo *>(ready[i].obj));
		}
		gStats.deferredFrees += n;
		if (n < 32) return;
	}
}

// ─── page tables (xe_pt.c): root at level 4, 9 bits per level ────────────────────────────────────────────────────
// Every table page has a shadow in CPU memory (PtNode): lookups and splits read the shadow, the GPU copy in VRAM is
// only written (through BAR2), read back only by the CAT-fault dump (rb8c). Table pages are 4 KB pieces of 64 KB buddy
// blocks in the BAR window (ptPageAlloc), allocated as needed; a table whose entries are all empty again goes back after
// the TLB invalidation of the bind that emptied it and (rb8c) the jobs its VM had in flight then (xe_pt's page tables
// are BOs on the VM's dma-resv, released by TTM once every fence there has signaled).
// Leaves: 1 GB at level 2 and 2 MB at level 1 where the VA and the physical run allow (XE_PDPE_PS_1G, XE_PDE_PS_2M),
// 4 KB at level 0 with the XE_PTE_PS64 hint on 64 KB-aligned 64 KB-contiguous runs (xe_pt_is_pte_ps64K).
struct PtNode {
	uint64_t dpa;
	uint64_t e[512];            // shadow of the entries
	PtNode **child;             // levels 1-4: the tables below (allocated with the first one)
	uint16_t used;              // entries that are not this level's empty entry
	uint8_t level;
	bool scratch;               // its VM's (XeVm::scratch): what an empty entry is
};

struct PtSlab { uint64_t dpa; VBlock *blk; uint16_t mask; PtSlab *next; };   // 16 table pages in one 64 KB block
static PtSlab *gPtSlabs;
static uint32_t gPtPages;

static bool ptPageAlloc(uint64_t *dpa) {
	IOLockLock(gResLock);
	PtSlab *sl = gPtSlabs;
	while (sl && sl->mask == 0xffff) sl = sl->next;
	if (!sl) {
		VBlock *b = buddyAlloc(&gVis, kVChunk, false, true);   // the whole window: kWinReserve is kept for this
		sl = b ? static_cast<PtSlab *>(IOMallocZero(sizeof(PtSlab))) : nullptr;
		if (!sl) {
			gStats.ptFails++;
			if (b) buddyFree(&gVis, b);
			IOLockUnlock(gResLock);
			return false;
		}
		sl->dpa = b->off;
		sl->blk = b;
		sl->next = gPtSlabs;
		gPtSlabs = sl;
		gVramUsed += kVChunk;
		gVisibleUsed += kVChunk;
	}
	const uint32_t i = (uint32_t)__builtin_ctz(~(uint32_t)sl->mask);
	sl->mask |= (uint16_t)(1u << i);
	gPtPages++;
	*dpa = sl->dpa + i * 0x1000ull;
	IOLockUnlock(gResLock);
	return true;
}

static void ptPageFree(uint64_t dpa) {
	IOLockLock(gResLock);
	for (PtSlab **pp = &gPtSlabs; *pp; pp = &(*pp)->next) {
		PtSlab *sl = *pp;
		if ((dpa & ~(kVChunk - 1)) != sl->dpa) continue;
		sl->mask &= (uint16_t)~(1u << ((dpa - sl->dpa) / 0x1000));
		gPtPages--;
		if (!sl->mask) {   // the whole 64 KB block back to the buddy
			*pp = sl->next;
			buddyFree(&gVis, sl->blk);
			gVramUsed -= kVChunk;
			gVisibleUsed -= kVChunk;
			IOFree(sl, sizeof(*sl));
		}
		break;
	}
	IOLockUnlock(gResLock);
}

static uint64_t pdeOf(uint64_t dpa) { return dpa | 1 | 2 | 1ull << 3 | 1ull << 4; }   // PAT 3: VRAM page tables

// pte_encode_pat_index: PAT0 bit 3, PAT1 bit 4, PAT2 bit 7 on a 4 KB PTE / bit 12 on a huge leaf, PAT3 62, PAT4 61
static uint64_t patBits(uint16_t pat, int level = 0) {
	uint64_t b = 0;
	if (pat & 1) b |= 1ull << 3;
	if (pat & 2) b |= 1ull << 4;
	if (pat & 4) b |= level ? 1ull << 12 : 1ull << 7;
	if (pat & 8) b |= 1ull << 62;
	if (pat & 16) b |= 1ull << 61;
	return b;
}

// Scratch (xe_vm_create_scratch, __xe_pt_empty_pte): an empty entry at levels 0-2 is a NULL PTE (reads 0, writes
// dropped; 2 MB / 1 GB huge at levels 1 / 2, DM and PAT WB as pte_encode_addr builds it), at levels 3 and 4 a PDE to a
// shared scratch table of the level below. rb7j: only in a VM created with DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE (and the
// kernel's), as xe_vm_create does it only for XE_VM_FLAG_SCRATCH_PAGE; without it an empty entry is 0 (__xe_pt_empty_pte
// with no scratch). Mesa ANV asks for scratch by default (driconf anv_enable_scratch_page, default true), so the
// Metal plugin's and the codecs' VMs keep it.
static uint64_t gScratchL2, gScratchL3;

// A NULL leaf (XE_PTE_NULL, as DRM_XE_VM_BIND_FLAG_NULL binds write): huge (PS) at levels 1 and 2.
static uint64_t nullLeaf(int level) { return 1ull << 9 | 1ull << 11 | 1ull << 4 | 2 | 1 | (level ? 1ull << 7 : 0); }

static uint64_t emptyEntry(int level, bool scratch) {
	if (!scratch) return 0;
	if (level == 4) return pdeOf(gScratchL3);
	if (level == 3) return pdeOf(gScratchL2);
	return nullLeaf(level);
}

static int scratchInit() {
	for (int lvl = 2; lvl <= 3; lvl++) {
		uint64_t &t = lvl == 2 ? gScratchL2 : gScratchL3;
		if (!ptPageAlloc(&t)) return ENOMEM;
		const uint64_t v = emptyEntry(lvl, true);
		volatile uint64_t *p = vram64(t);
		for (int i = 0; i < 512; i++) p[i] = v;
	}
	return 0;
}

static bool isLeaf(uint64_t v, int level) { return (v & 1) && (!level || (v & 1ull << 7)); }

// The entry `i` of a table that replaces huge leaf v at `level` (xe_pt's split of a huge entry): same flags, the
// address advanced by the child's span; NULL leaves stay NULL; PS stays above level 0, where PAT2 moves to bit 7.
static uint64_t leafChild(uint64_t v, int level, uint32_t i) {
	const uint64_t addrMask = ((1ull << 52) - 1) & ~((1ull << (12 + 9 * level)) - 1);
	uint64_t flags = v & ~addrMask;
	if (level == 1) {
		flags &= ~(1ull << 7);
		if (flags & 1ull << 12) flags = (flags & ~(1ull << 12)) | 1ull << 7;
	}
	if (v & 1ull << 9) return flags;
	return ((v & addrMask) + (uint64_t)i * (1ull << (12 + 9 * (level - 1)))) | flags;
}

static void ptSet(PtNode *n, uint32_t idx, uint64_t v) {
	const uint64_t empty = emptyEntry(n->level, n->scratch), old = n->e[idx];
	if (old == empty && v != empty) n->used++;
	else if (old != empty && v == empty) n->used--;
	n->e[idx] = v;
	vram64(n->dpa)[idx] = v;
}

// A table at `level` whose entries stand for parent entry `from` (a huge leaf to split, or empty).
static PtNode *ptNodeNew(int level, uint64_t from, bool scratch) {
	auto *n = static_cast<PtNode *>(IOMalloc(sizeof(PtNode)));
	if (!n) return nullptr;
	n->child = nullptr;
	n->level = (uint8_t)level;
	n->scratch = scratch;
	n->used = 0;
	if (level && !(n->child = static_cast<PtNode **>(IOMallocZero(512 * sizeof(PtNode *))))) {
		IOFree(n, sizeof(*n));
		return nullptr;
	}
	if (!ptPageAlloc(&n->dpa)) {
		if (n->child) IOFree(n->child, 512 * sizeof(PtNode *));
		IOFree(n, sizeof(*n));
		return nullptr;
	}
	const bool split = level < 4 && isLeaf(from, level + 1) && from != emptyEntry(level + 1, scratch);
	const uint64_t empty = emptyEntry(level, scratch);
	volatile uint64_t *gpu = vram64(n->dpa);
	for (uint32_t i = 0; i < 512; i++) {
		n->e[i] = split ? leafChild(from, level + 1, i) : empty;
		if (n->e[i] != empty) n->used++;
		gpu[i] = n->e[i];
	}
	// rb8c: every entry out of the write-combining buffers before the caller writes the entry that links the table: WC
	// stores to different lines may become visible in any order, and a walk by a running batch that met the new parent
	// entry first would read the page's previous contents (an older table, or a BO's data: a CAT error). xe writes a new
	// table well before the update that links it (xe_pt_stage_bind, then the commit), with locked operations between;
	// here the two are back to back.
	asm volatile("sfence" : : : "memory");
	return n;
}

static void ptTreeFree(PtNode *n) {
	if (n->child) {
		for (uint32_t i = 0; i < 512; i++)
			if (n->child[i]) ptTreeFree(n->child[i]);
		IOFree(n->child, 512 * sizeof(PtNode *));
	}
	ptPageFree(n->dpa);
	IOFree(n, sizeof(*n));
}

// One VM_BIND call: the BOs and page tables its unbinds release wait for the invalidation issued after all its ops
// (owner = this), which covers [lo, hi) of the VM; the call's out-fence is the latest clear of a BO it maps (xe_vm_bind
// waits for the BO's kernel fences).
struct BindCtx {
	bool parked, unmapped;
	uint64_t lo, hi;
	XeFence clear;
	void touch(uint64_t a, uint64_t b) {   // xe_tlbinval.h XeInvalInterval (unmapped: one was touched)
		XeInvalInterval iv = {unmapped, lo, hi};
		iv.touch(a, b);
		unmapped = true;
		lo = iv.lo;
		hi = iv.hi;
	}
};

// rb8c (engineDump): page-table changes that matter to a CAT fault: huge leaves split into a table (last VA, level, when),
// tables detached by an unbind (freed after their invalidation and the VM's jobs), tables leaked for want of memory.
static struct { uint32_t splits, splitLevel, detached, leaked; uint64_t splitVa, splitNs, detachNs, detachDpa; } gPtSt;

// Detach table i of n (it goes after the bind's invalidation and, rb8c, the VM's jobs of that moment) and set the entry.
static void ptReplace(PtNode *n, uint32_t i, uint64_t v, uint64_t va, uint64_t span, BindCtx *ctx) {
	if (n->child && n->child[i]) {
		PtNode *c = n->child[i];
		n->child[i] = nullptr;
		gPtSt.detached++;
		gPtSt.detachNs = nowNs();
		gPtSt.detachDpa = c->dpa;
		// no memory for the list: rb8c leaked (WAS freed at once, before the invalidation that covers it: a walk could
		// then still reach a page already handed out again)
		if (!deferAdd(c, kDefPt, 0, ctx)) gPtSt.leaked++;
		ctx->touch(va, va + span);
	}
	if (isLeaf(n->e[i], n->level) && n->e[i] != emptyEntry(n->level, n->scratch) && n->e[i] != v) ctx->touch(va, va + span);
	ptSet(n, i, v);
}

// A table for entry i of n (an empty entry, or a huge leaf it splits), written, then linked. rb8c: splitting a live huge
// leaf changes how its whole span translates (a TLB may hold it as one 2 MB / 1 GB entry), so the bind invalidates all
// of it: xe_pt_update_ops_rfence_interval aligns an update's range to the span of the highest-level entry it wrote, and
// for a 2 MB entry the TLB wants an invalidation of at least 16 MB (send_tlb_inval_ppgtt), which the 4 KB pages the bind
// changes underneath would not give. WAS not touched: only the sub-range was invalidated.
static PtNode *ptSplit(PtNode *n, uint32_t i, uint64_t base, BindCtx *ctx) {
	const uint64_t old = n->e[i];
	PtNode *c = ptNodeNew(n->level - 1, old, n->scratch);
	if (!c) return nullptr;
	n->child[i] = c;
	ptSet(n, i, pdeOf(c->dpa));
	if (isLeaf(old, n->level) && old != emptyEntry(n->level, n->scratch)) {
		ctx->touch(base, base + xePtSpan(n->level));
		gPtSt.splits++;
		gPtSt.splitLevel = n->level;
		gPtSt.splitVa = base;
		gPtSt.splitNs = nowNs();
	}
	return c;
}

struct MapReq {
	XeBo *bo;                    // nullptr: unmap, or a NULL bind (null), which in a scratch VM equals the empty entry
	uint64_t off0, va0, len;
	uint16_t pat;
	uint32_t flags;
	BindCtx *ctx;
	uint64_t cOff, cPa, cRun;    // physical-run cursor
	bool huge;                   // 2 MB / 1 GB leaves and the PS64 hint allowed (gHugePages; always for the migrate VM)
	bool null;                   // DRM_XE_VM_BIND_FLAG_NULL: NULL leaves (written as such in a VM without scratch)
	// Physical address of BO offset o and the bytes physically contiguous from it.
	bool phys(uint64_t o, uint64_t *pa, uint64_t *run) {
		if (o >= cOff && o < cOff + cRun) {
			*pa = cPa + (o - cOff);
			*run = cRun - (o - cOff);
			return true;
		}
		uint64_t p, r = 0;
		if (bo->vram) {   // DPA 0 is a valid address (the migrate VM's identity map starts there): run 0 = beyond the BO
			p = boDpa(bo, o, &r);
			if (!r) return false;
		} else {
			if (!bo->dma.lookup(o, &p, &r)) return false;   // device address: IOVA under VT-d, physical without
		}
		if ((!p && !bo->vram) || !r) return false;
		cOff = o, cPa = p, cRun = r;
		*pa = p;
		*run = r;
		return true;
	}
	uint64_t leaf(uint64_t pa, int level) const {
		uint64_t v = pa | 1 | patBits(pat, level) | 1ull << 10;   // AE (XE_USM_PPGTT_PTE_AE: device atomics)
		if (!(flags & DRM_XE_VM_BIND_FLAG_READONLY)) v |= 2;
		if (bo->vram) v |= 1ull << 11;   // DM
		if (level) v |= 1ull << 7;       // PS
		return v;
	}
};

static int ptRange(MapReq &r, PtNode *n, uint64_t va, uint64_t end) {
	const int lvl = n->level;
	const uint64_t span = 1ull << (12 + 9 * lvl), empty = emptyEntry(lvl, n->scratch);
	// a NULL bind is an unmap where the empty entry is a NULL leaf (scratch); without scratch it writes NULL leaves
	const bool nullMap = !r.bo && r.null && !n->scratch;
	while (va < end) {
		const uint32_t i = (uint32_t)((va >> (12 + 9 * lvl)) & 511);
		const uint64_t base = va & ~(span - 1), eEnd = end < base + span ? end : base + span;
		const bool full = va == base && eEnd == base + span;
		PtNode *c = n->child ? n->child[i] : nullptr;
		if (!r.bo && !nullMap) {   // unmap
			if (full) {
				if (c || n->e[i] != empty) ptReplace(n, i, empty, base, span, r.ctx);
			} else if (c || (isLeaf(n->e[i], lvl) && n->e[i] != empty)) {
				if (!c && !(c = ptSplit(n, i, base, r.ctx))) return ENOMEM;   // split the huge leaf to unmap part of it
				int err = ptRange(r, c, va, eEnd);
				if (err) return err;
				if (!c->used) ptReplace(n, i, empty, base, span, r.ctx);
			}
		} else if (nullMap && (!lvl || (full && lvl <= 2))) {   // NULL leaf (huge where a whole entry is covered)
			ptReplace(n, i, nullLeaf(lvl), base, span, r.ctx);
		} else if (nullMap) {
			if (!c && !(c = ptSplit(n, i, base, r.ctx))) return ENOMEM;
			int err = ptRange(r, c, va, eEnd);
			if (err) return err;
		} else if (!lvl) {
			const uint64_t o = r.off0 + (va - r.va0);
			uint64_t pa, run;
			if (!r.phys(o, &pa, &run)) return EFAULT;
			uint64_t v = r.leaf(pa, 0);
			// XE_PTE_PS64: this PTE is part of a 64 KB-aligned run the bind maps contiguously
			const uint64_t c64 = va & ~0xffffull, o64 = r.off0 + (c64 - r.va0);
			uint64_t pa64, run64;
			if (r.huge && c64 >= r.va0 && c64 + 0x10000 <= r.va0 + r.len && r.phys(o64, &pa64, &run64) && !(pa64 & 0xffff) &&
				run64 >= 0x10000)
				v |= 1ull << 8;
			if (n->e[i] != empty && n->e[i] != v) r.ctx->touch(base, base + span);
			ptSet(n, i, v);
		} else {
			const uint64_t o = r.off0 + (base - r.va0);
			uint64_t pa = 0, run = 0;
			const bool huge = r.huge && full && lvl <= 2 && (lvl == 1 || r.bo->vram) && r.phys(o, &pa, &run) && !(pa & (span - 1)) &&
				run >= span;
			if (huge) {
				ptReplace(n, i, r.leaf(pa, lvl), base, span, r.ctx);
			} else {
				if (!c && !(c = ptSplit(n, i, base, r.ctx))) return ENOMEM;
				int err = ptRange(r, c, va, eEnd);
				if (err) return err;
			}
		}
		va = eEnd;
	}
	return 0;
}

static XeVm *gFaultVms;   // every live VM, for the ASID of a GuC page fault (gFaultLock)

// ─── GPU page faults (xe_guc_pagefault.c, xe_pagefault.c; rb7j) ─────────────────────────────────────────────────────
// With the UM queues in the ADS (gUsm) the GuC reports a page fault as G2H REPORT_PAGE_FAULT_REQ_DESC and stalls the
// context until the KMD answers PAGE_FAULT_RES_DESC. No VM here is in fault mode (DRM_XE_VM_CREATE_FLAG_FAULT_MODE is
// refused), so, as xe_pagefault_service for a VM not in fault mode, every fault is unsuccessful: it goes on its VM's list
// (xe_pagefault_save_to_vm: unless a prefetch), is printed (xe_guc_pagefault_print) and answered with PFR_SUCCESS set
// (guc_ack_fault sets it to !!err), whereupon the GuC resets the context (CONTEXT_RESET / CAT error G2H: the queue is
// banned as before). The descriptors wait in a ring the submit worker drains (xe_pagefault_queue_work).
struct FaultDesc { uint32_t dw[4]; };
static FaultDesc gFaultRing[256];
static uint32_t gFaultHead, gFaultTail;   // gFaultLock

static void faultQueue(const uint32_t *dw) {
	IOLockLock(gFaultLock);
	if (gFaultTail - gFaultHead < 256) {
		memcpy(gFaultRing[gFaultTail % 256].dw, dw, 16);
		gFaultTail++;
	} else {
		gStats.pageFaultsLost++;   // the GuC waits for an answer that does not come: the watchdog bans the queue
	}
	IOLockUnlock(gFaultLock);
}

static void faultsDrain() {
	for (;;) {
		FaultDesc d;
		IOLockLock(gFaultLock);
		if (gFaultHead == gFaultTail) {
			IOLockUnlock(gFaultLock);
			return;
		}
		d = gFaultRing[gFaultHead % 256];
		gFaultHead++;
		// xe_guc_pagefault_handler's fields (xe_guc_fwif.h PFD_*)
		const uint32_t level = d.dw[0] & 7, srcid = d.dw[0] >> 3 & 0xff, trva = d.dw[0] >> 18 & 1,
			engInst = d.dw[0] >> 19 & 0x3f, engClass = d.dw[0] >> 25 & 7, pdata = d.dw[0] >> 28 | (d.dw[1] & 0xfff) << 4,
			asid = d.dw[1] >> 12, access = d.dw[2] & 3, type = d.dw[2] >> 2 & 3, vfid = d.dw[2] >> 4 & 0x3f,
			prefetch = d.dw[2] >> 11 & 1;
		const uint64_t addr = (uint64_t)d.dw[3] << 32 | (d.dw[2] & 0xfffff000u);
		// fault_type_level: XE_PAGEFAULT_TYPE_LEVEL_NACK (0xff) for a TRVA fault, else level | type << 3
		const uint8_t tl = trva ? 0xff : (uint8_t)(level | type << 3);
		bool saved = false;
		if (!prefetch) {   // xe_pagefault_save_to_vm -> xe_vm_add_fault_entry_pf (at most 50 per VM)
			for (XeVm *vm = gFaultVms; vm; vm = vm->faultNext)
				if (vm->asid == asid) {
					if (vm->nFaults < 50) {
						vm->faults[vm->nFaults++] = {addr, (uint8_t)access, (uint8_t)(tl >> 3 & 0xf), (uint8_t)(tl & 7),
							(uint8_t)srcid};
						saved = true;
					}
					break;
				}
		}
		IOLockUnlock(gFaultLock);
		gStats.pageFaults++;
		g2hLog("page fault: ASID %u address 0x%llx type %u access %u level %u engine %u.%u srcid 0x%02x%s%s\n", asid, addr,
			type, access, level, engClass, engInst, srcid, trva ? " (TRVA)" : "", saved ? "" : prefetch ? " (prefetch)" :
			" (no VM / list full)");
		// guc_ack_fault: PFR_VALID, PFR_SUCCESS = !!err (1: not serviced), PFR_REPLY = PFR_ACCESS, PFR_DESC_TYPE =
		// FAULT_RESPONSE_DESC, PFR_ASID; PFR_VFID, PFR_PREFETCH (err ? prefetch : 0), engine instance and class, PDATA
		const uint32_t res[3] = {0x6003, 1u | 1u << 1 | 0u << 2 | 1u << 10 | asid << 12,
			vfid | prefetch << 6 | engInst << 7 | engClass << 13 | pdata << 16};
		if (!gResetting) gDev->guc(res, 3);   // a GT reset reloads the GuC: the stalled context is gone with it
	}
}

static XeVm *vmCreate(uint32_t asid, bool scratch = true) {
	auto *vm = static_cast<XeVm *>(IOMallocZero(sizeof(XeVm)));
	if (!vm) return nullptr;
	if (!asid) {
		IOLockLock(gResLock);
		asid = idAlloc(gAsidBits, kAsidFirst, kAsidEnd, &gAsidNext);
		IOLockUnlock(gResLock);
		if (asid == ~0u) {
			IOFree(vm, sizeof(*vm));
			return nullptr;
		}
	}
	vm->asid = asid;
	vm->refs = 1;
	vm->kern = {0, ~0u, 0, 0};
	vm->bindOut = {{0, ~0u, 0, 0}, {0, ~0u, 0, 0}};
	vm->scratch = scratch;
	if (!(vm->top = ptNodeNew(4, 0, scratch))) {
		vmFreeNow(vm);
		return nullptr;
	}
	vm->root = vm->top->dpa;
	IOLockLock(gFaultLock);
	vm->faultNext = gFaultVms;
	gFaultVms = vm;
	IOLockUnlock(gFaultLock);
	return vm;
}

// debug.intelxe.hugepages (boot-arg -intelxe_nohugepages sets 0): 0 = binds made from now on use 4 KB leaves only, without the
// PS64 hint, to rule huge pages out of a GPU fault without a rebuild. Existing mappings keep what they have.
int gHugePages = 1;
// debug.intelxe.ctx_policies: queues created from now on get Linux's GuC scheduling policies (init_policies); 0 = none.
int gCtxPolicies = 1;
int gVcsSingle = 0;   // rb7o debug.intelxe.vcs_single: 1 = video queues on their first placement (before rb7o)
// debug.intelxe.job_timeout_ms: the job watchdog (drm_sched's job timeout, xe_guc_submit.c guc_exec_queue_timedout_job;
// watchRun): a queue whose oldest submitted job has not completed for this long is taken off the engine (schedule
// disable) and the context's own run time is read from its saved CTX_TIMESTAMP (check_timeout, with the 5% margin): at
// or over this limit the queue is banned (its fences complete; the 640 ms preemption timeout resets the engine if it
// does not yield), under it (a job queued behind others) it goes back on and its clock restarts. A job that never
// started is not judged by this limit: rb8d resets the GT for it at Linux's 5 s (xeNotStartedLimitMs, lower if this is
// lower), or 2 s after a GuC context reset of its domain (xeStuckAfterReset). Linux's TDR fires at job_timeout_ms, 5 s
// by default (xe_hw_engine.c hw_engine_init_early), and check_timeout compares the run time with the same 5 s; this
// driver keeps 10 s (Linux's DRM_XE_JOB_TIMEOUT_MAX, Kconfig.profile) for both, giving a started job twice Linux's run
// time. 0 = off. (rb8e: WAS described as having no run-time check, which the TDR has had since 0.1.0.)
int gJobTimeoutMs = 10000;

static int vmMap(XeVm *vm, XeBo *bo, uint64_t off, uint64_t range, uint64_t addr, uint16_t pat, uint32_t flags,
	BindCtx *ctx, bool kernelHuge = false) {
	MapReq r = {(flags & DRM_XE_VM_BIND_FLAG_NULL) ? nullptr : bo, off, addr, range, pat, flags, ctx, 0, 0, 0,
		kernelHuge || gHugePages != 0, (flags & DRM_XE_VM_BIND_FLAG_NULL) != 0};
	return ptRange(r, vm->top, addr, addr + range);
}

// The level-0 table that maps va, or nullptr (empty, or a huge leaf above).
static PtNode *ptLeafTable(XeVm *vm, uint64_t va) {
	PtNode *n = vm->top;
	while (n && n->level > 0) n = n->child ? n->child[(va >> (12 + 9 * n->level)) & 511] : nullptr;
	return n;
}

// XE_PTE_PS64 tells the TLB that the 16 PTEs of a 64 KB-aligned group map one contiguous 64 KB run, so it may cache the
// group as one 64 KB translation (xe_pt_is_pte_ps64K: set only when the whole group is one VMA's contiguous run). A
// bind or unbind that changes part of a group breaks that: left in place on the other PTEs, a 64 KB entry filled from
// them would still translate the changed pages to the old run, VRAM that may already belong to another process. Linux
// never gets there, since a split VMA's remainder is bound again (drm_gpuvm remap) and its PTEs recomputed; here the
// hint goes from every PTE of a group the range only partly covers, and the group is invalidated with the bind.
static void ptPs64Break(XeVm *vm, uint64_t va, BindCtx *ctx) {
	PtNode *n = ptLeafTable(vm, va);
	if (!n) return;
	const uint32_t first = (uint32_t)((va >> 12) & 511) & ~15u;
	bool any = false;
	for (uint32_t i = first; i < first + 16; i++)
		if (n->e[i] & 1ull << 8) {
			ptSet(n, i, n->e[i] & ~(1ull << 8));
			any = true;
		}
	if (any) ctx->touch(va & ~0xffffull, (va & ~0xffffull) + 0x10000);
}

static void ptPs64Edges(XeVm *vm, uint64_t addr, uint64_t end, BindCtx *ctx) {
	if (addr & 0xffff) ptPs64Break(vm, addr, ctx);
	if (end & 0xffff) ptPs64Break(vm, end - 1, ctx);   // the same group again finds nothing left to clear
}

// Returns ENOMEM if a huge leaf the range cuts could not be split: the PTEs before that point are cleared, the rest
// are as they were.
static int vmUnmap(XeVm *vm, uint64_t addr, uint64_t range, BindCtx *ctx) {
	MapReq r = {nullptr, 0, addr, range, 0, 0, ctx, 0, 0, 0, false, false};
	const int err = ptRange(r, vm->top, addr, addr + range);
	ctx->touch(addr, addr + range);
	ptPs64Edges(vm, addr, addr + range, ctx);
	return err;
}

static bool vmAddBinding(XeVm *vm, uint64_t addr, uint64_t range, XeBo *bo, uint64_t off, uint16_t pat, uint32_t flags,
	uint32_t handle, uint8_t purge = kMadvWillneed) {
	if (vm->nBinds == vm->capBinds) {
		uint32_t nc = vm->capBinds ? vm->capBinds * 2 : 32;
		auto *nb = static_cast<XeBinding *>(IOMallocZero(nc * sizeof(XeBinding)));
		if (!nb) return false;
		if (vm->binds) {
			memcpy(nb, vm->binds, vm->nBinds * sizeof(XeBinding));
			IOFree(vm->binds, vm->capBinds * sizeof(XeBinding));
		}
		vm->binds = nb;
		vm->capBinds = nc;
	}
	if (bo) {
		boGet(bo);
		OSIncrementAtomic(&bo->vmRecs);   // rb7s: every record a move must rewrite
	}
	vm->binds[vm->nBinds++] = {addr, range, bo, off, pat, flags, handle, purge};
	bindingCounted(bo, purge);
	return true;
}

// Unbind [addr, addr+range) from the records (only: every binding of that BO made through handle onlyHandle, unmapping
// it too; see XeBinding::handle): a binding fully inside
// goes and drops its BO reference (after the bind's TLB invalidation); one that overlaps it in part keeps the parts
// outside, split in two around a hole, as xe splits a VMA (drm_gpuvm remap), so the records always describe exactly what
// the page tables map (a BO move rewrites PTEs from them). Without memory to split, the record keeps covering the hole
// and its BO is marked never to move.
static void vmDropBindings(XeVm *vm, uint64_t addr, uint64_t range, XeBo *only, BindCtx *ctx, uint32_t onlyHandle = 0) {
	const uint64_t end = addr + range;
	for (uint32_t i = 0; i < vm->nBinds;) {
		const XeBinding b = vm->binds[i];
		const uint64_t bEnd = b.addr + b.range;
		const bool hit = only ? b.bo == only && b.handle == onlyHandle : (b.addr >= addr && bEnd <= end);
		if (hit) {
			if (only && vmUnmap(vm, b.addr, b.range, ctx)) {   // PTEs may still point at it: the binding (and BO) stays
				if (b.bo) b.bo->noMove = true;
				i++;
				continue;
			}
			ctx->touch(b.addr, bEnd);
			bindingUncounted(b.bo, b.purge);
			if (b.bo) {
				OSDecrementAtomic(&b.bo->vmRecs);   // rb7s: no record names it here any more (its PTEs are gone)
				if (gDeferAsync && deferAdd(b.bo, kDefBo, 0, ctx)) ctx->parked = true;
				else {   // the synchronous way (debug.intelxe.unbind_async=0, or no memory for the list)
					asm volatile("sfence" : : : "memory");
					tlbWait(nullptr, tlbInvalidateIssue());
					boPut(b.bo);
				}
			}
			vm->binds[i] = vm->binds[--vm->nBinds];
			continue;
		}
		if (!only && b.addr < end && addr < bEnd) {
			if (b.addr < addr && bEnd > end) {   // the range is a hole inside it: the part above becomes its own record
				if (vmAddBinding(vm, end, bEnd - end, b.bo, b.off + (end - b.addr), b.pat, b.flags, b.handle, b.purge))
					vm->binds[i].range = addr - b.addr;
				else if (b.bo) b.bo->noMove = true;
			} else if (b.addr < addr) {          // overlaps its end
				vm->binds[i].range = addr - b.addr;
			} else {                             // overlaps its start
				vm->binds[i].off += end - b.addr;
				vm->binds[i].addr = end;
				vm->binds[i].range = bEnd - end;
			}
		}
		i++;
	}
}

static void vmFreeNow(XeVm *vm) {
	IOLockLock(gFaultLock);   // no fault is recorded into it after this (a later one for its ASID finds no VM)
	for (XeVm **pp = &gFaultVms; *pp; pp = &(*pp)->faultNext)
		if (*pp == vm) {
			*pp = vm->faultNext;
			break;
		}
	IOLockUnlock(gFaultLock);
	for (uint32_t i = 0; i < vm->nBinds; i++)
		if (XeBo *bo = vm->binds[i].bo) {
			OSDecrementAtomic(&bo->vmRecs);   // rb7s
			boPut(bo);
		}
	if (vm->binds) IOFree(vm->binds, vm->capBinds * sizeof(XeBinding));
	if (vm->top) ptTreeFree(vm->top);
	if (vm->asid >= kAsidFirst && vm->asid < kAsidEnd) {
		IOLockLock(gResLock);
		idFree(gAsidBits, vm->asid);
		IOLockUnlock(gResLock);
	}
	IOFree(vm, sizeof(*vm));
}

static void vmGet(XeVm *vm) { OSIncrementAtomic(&vm->refs); }

// Last reference (the client's handle and its queues are gone): no batch can use the VM any more, but TLBs may still
// hold its translations, so its page tables and pages go once an invalidation issued now has completed.
static void vmPut(XeVm *vm) {
	if (OSDecrementAtomic(&vm->refs) != 1) return;
	if (!deferAdd(vm, kDefVm, tlbInvalidateIssue(), nullptr)) {
		tlbWait(nullptr, gTlbSeq);
		vmFreeNow(vm);
	}
}

// ─── LRC memory (rb7) ──────────────────────────────────────────────────────────────────────────────────────────────
int gLrcVram = 1;               // debug.intelxe.lrc_vram: new queues' LRCs in VRAM
static uint32_t gLrcVramN, gLrcSysN, gLrcVramFallbacks;   // LRCs now in VRAM / system memory; VRAM asked but not had

// The LRC's memory: a 64 KB block of the BAR window (the page-table pool's way: the whole window, kWinReserve is the
// kernel's), else system memory (zeroed). len <= the 32 KB GGTT window per slot.
static bool lrcAlloc(XeQueue *q, uint32_t len) {
	q->lrcLen = len;
	if (gLrcVram && gDevVram()) {
		IOLockLock(gResLock);
		VBlock *b = buddyAlloc(&gVis, kVChunk, false, true);
		if (b) {
			gVramUsed += kVChunk;
			gVisibleUsed += kVChunk;
			gLrcVramN++;
		} else {
			gLrcVramFallbacks++;
		}
		IOLockUnlock(gResLock);
		if (b) {
			q->lrcBlk = b;
			q->lrcCpu = reinterpret_cast<uint8_t *>(gDevVram()->getVirtualAddress() + b->off);
			return true;   // queueCreate writes all len bytes
		}
	}
	q->lrc = gDev->allocSys(len, false);
	if (!q->lrc) return false;
	q->lrcCpu = static_cast<uint8_t *>(q->lrc->getBytesNoCopy());
	IOLockLock(gResLock);
	gLrcSysN++;
	IOLockUnlock(gResLock);
	return true;
}

static void lrcFree(XeQueue *q) {
	if (q->lrcBlk) {
		IOLockLock(gResLock);
		buddyFree(&gVis, q->lrcBlk);
		gVramUsed -= kVChunk;
		gVisibleUsed -= kVChunk;
		gLrcVramN--;
		IOLockUnlock(gResLock);
		q->lrcBlk = nullptr;
	} else if (q->lrc) {
		OSSafeReleaseNULL(q->lrc);
		IOLockLock(gResLock);
		gLrcSysN--;
		IOLockUnlock(gResLock);
	}
	if (q->lrcSave) {
		IOFree(q->lrcSave, q->lrcLen);
		q->lrcSave = nullptr;
	}
	q->lrcCpu = nullptr;
}

static uint32_t *lrcRegs(XeQueue *q) { return reinterpret_cast<uint32_t *>(q->lrcCpu + ringSize + 0x1000); }

// CPU writes of [p, p + len) done; the GPU or the GuC may read them after what follows (a tail register, an H2G):
// system memory: cache lines out (clflush + mfence, the GPU reads system memory coherently only from memory here);
// VRAM: xe_device_wmb (sfence + posted MMIO write), as xe_lrc_write_ring / h2g_write.
static void lrcSync(XeQueue *q, const void *p, size_t len) {
	if (q->lrcBlk) gDev->deviceWmb();
	else flushRange(const_cast<void *>(p), len);
}

// Before a CPU read of what the GPU wrote into the LRC (context save, PPHWSP): system memory: the stale lines go;
// VRAM: nothing to do, BAR2 reads are uncached and see the device's view.
static void lrcReadPrep(XeQueue *q, const void *p, size_t len) {
	if (!q->lrcBlk) flushRange(const_cast<void *>(p), len);
}

static int lrcGgttMap(XeQueue *q, uint32_t *inval) {
	return q->lrcBlk ? gDev->ggttMapLrc(q->lrcBlk->off, q->lrcLen, q->lrcGgtt, q->eng->gt, inval)
	                 : gDev->ggttMapMem(q->lrc, q->lrcGgtt, q->eng->gt, inval);
}

// ─── exec queues: LRC, registered and enabled on GuC at creation ─────────────────────────────────────────────────
static const XeEngine *engineFor(uint16_t cls, uint16_t inst) {
	for (uint32_t i = 0; i < gNumEngines; i++)
		if (gEngines[i].drmClass == cls && gEngines[i].logical == inst) return &gEngines[i];
	// the media GT's engines: video decode, and (rb7j) video enhance, each class with its own logical ids
	if ((cls == DRM_XE_ENGINE_CLASS_VIDEO_DECODE || cls == DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE) && gVideoWanted && gDev->mediaUp())
		for (uint32_t i = 0; i < gNumMediaEngines; i++)
			if (gMediaEngines[i].drmClass == cls && gMediaEngines[i].logical == inst) return &gMediaEngines[i];
	return nullptr;
}

// register_exec_queue + init_policies + enable_scheduling: REGISTER_CONTEXT with the LRC descriptor, the GuC
// scheduling policies (debug.intelxe.ctx_policies) and SCHED_CONTEXT_MODE_SET(enable) unless `enable` is false. At
// creation, and again for every live queue after a GT reset.
// rb7o: engine_submit_mask = the queue's logical mask (xe_guc_submit.c register_exec_queue:
// xe_hwe_guc_logical_to_submit_mask(q->hwe, q->logical_mask), which is the mask itself when there is no GuC PAGING
// class, NVL-S and later); the GuC maps logical ids to engines with the ADS mapping table (guc_mapping_table_init).
static void queueRegister(XeQueue *q, bool enable) {
	const XeEngine *eng = q->eng;
	const uint32_t desc = (q->lrcGgtt + ringSize) | 1u | 3u << 3 | 1u << 8;
	uint32_t reg[12] = {0x4502, 1, q->gucId, eng->gucClass, q->logicalMask, 0, 0, 0, 0, 0, desc, 0};
	gDev->guc(reg, 12, eng->gt);
	if (gCtxPolicies) {
		// init_policies: XE_GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES with the xe_hw_engine defaults, priority
		// KMD_NORMAL (KMD_HIGH for kernel queues), execution quantum (timeslice) 1 ms, preemption timeout 640 ms
		// (CONFIG_DRM_XE_PREEMPT_TIMEOUT), no SLPC frequency request. Without them the GuC's own defaults apply, and a
		// context that does not yield is never preempted off the engine.
		// rb7j: priority from the queue (xe_exec_queue_prio_to_guc: LOW -> NORMAL 3, NORMAL -> KMD_NORMAL 2, HIGH -> HIGH 1,
		// kernel -> KMD_HIGH 0), its timeslice, and for DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT the SLPM_GT_FREQUENCY KLV's
		// SLPC_CTX_FREQ_REQ_IS_COMPUTE (bit 28): SLPC then treats the context like compute work (frequency held up).
		static const uint32_t kGucPrio[3] = {3, 2, 1};
		const uint32_t prio = q->kernel ? 0u : kGucPrio[q->prio < 3 ? q->prio : 1];
		uint32_t pol[10] = {0x100b, q->gucId, 0x2003u << 16 | 1, prio, 0x2001u << 16 | 1, q->timesliceUs ? q->timesliceUs : 1000,
			0x2002u << 16 | 1, 640000, 0x2005u << 16 | 1, q->lowLatency ? 1u << 28 : 0u};
		gDev->guc(pol, 10, eng->gt);
	}
	if (enable) {
		uint32_t en[3] = {0x1001, q->gucId, 1};
		gDev->guc(en, 3, eng->gt);
	}
}

// rb7j: what DRM_IOCTL_XE_EXEC_QUEUE_CREATE's flags and SET_PROPERTY extensions ask for (queueProps).
struct QueueProps {
	uint8_t prio = 1;              // XE_EXEC_QUEUE_PRIORITY_NORMAL
	uint32_t timesliceUs = 1000;   // the engine class default (1 ms)
	bool lowLatency = false, stateCacheFix = false;
};

// logicalMask: the logical ids of cls on inst's GT the queue may run on (calc_validate_logical_mask, checked by the
// caller); 0 = inst alone.
static int queueCreate(XeVm *vm, uint16_t cls, uint16_t inst, XeQueue **out, const QueueProps *props = nullptr,
	uint32_t logicalMask = 0) {
	if (gWedged) return ECANCELED;   // xe_device_declare_wedged: nothing goes to the GPU any more
	const XeEngine *eng = engineFor(cls, inst);
	if (!eng) return EINVAL;
	if (!logicalMask) logicalMask = 1u << eng->logical;
	if (!(logicalMask & 1u << eng->logical)) return EINVAL;
	const uint32_t g = gtIdx(eng->gt);
	auto *q = static_cast<XeQueue *>(IOMallocZero(sizeof(XeQueue)));
	if (!q) return ENOMEM;
	q->vm = vm;
	q->eng = eng;
	q->logicalMask = logicalMask;
	q->cls = cls;
	q->inst = inst;
	const QueueProps defaults;
	if (!props) props = &defaults;
	q->prio = props->prio;
	q->timesliceUs = props->timesliceUs;
	q->lowLatency = props->lowLatency;
	q->stateCacheFix = props->stateCacheFix && eng->drmClass == DRM_XE_ENGINE_CLASS_RENDER;
	IOLockLock(gResLock);
	q->slot = idAlloc(gSlotBits, 0, kSlots, nullptr);
	q->gucId = idAlloc(gGucIdBits[g], kGucIdFirst, kMaxGucIds, nullptr);
	if (q->slot != ~0u) q->gen = ++gSlotGen[q->slot];
	IOLockUnlock(gResLock);
	static_assert(ringSize + 0x4000 <= 0x8000, "an LRC fits its 32 KB GGTT window");
	if (q->slot == ~0u || q->gucId == ~0u || ringSize + eng->lrcSize > 0x8000 || !lrcAlloc(q, ringSize + eng->lrcSize)) {
		IOLockLock(gResLock);
		if (q->slot != ~0u) idFree(gSlotBits, q->slot);
		if (q->gucId != ~0u) idFree(gGucIdBits[g], q->gucId);
		IOLockUnlock(gResLock);
		IOFree(q, sizeof(*q));
		return ENOMEM;
	}
	q->lrcGgtt = lrcPoolG + q->slot * 0x8000;
	seqnoSet(q->slot, 0);
	slotClearEngine(q->slot);   // rb7o: no engine yet (not the slot's previous queue's)

	// built in system memory and copied in one go: the context image is read-modify-written field by field, which a
	// write-combined BAR2 mapping would turn into uncached reads
	auto *l = static_cast<uint8_t *>(IOMallocZero(q->lrcLen));
	if (!l) {
		lrcFree(q);
		IOLockLock(gResLock);
		idFree(gSlotBits, q->slot);
		idFree(gGucIdBits[g], q->gucId);
		IOLockUnlock(gResLock);
		IOFree(q, sizeof(*q));
		return ENOMEM;
	}
	auto *ring = reinterpret_cast<uint32_t *>(l);
	auto *regs = reinterpret_cast<uint32_t *>(l + ringSize + 0x1000);
	lrcSetOffsets(regs, eng->drmClass == 0 ? xe2_rcs_offsets : eng->drmClass == 1 ? xe2_bcs_offsets : xe2_xcs_offsets,
		eng->base);
	regs[CTX_CONTEXT_CONTROL] = (1u << 3 | 1u) << 16 | (1u << 3 | 1u);
	regs[CTX_RING_START] = q->lrcGgtt;
	regs[CTX_RING_CTL] = (ringSize - 0x1000) | 1;
	regs[CTX_PDP0_UDW] = (uint32_t)(pdeOf(vm->root) >> 32);
	regs[CTX_PDP0_LDW] = (uint32_t)pdeOf(vm->root);
	regs[CTX_ASID] = vm->asid;
	uint32_t n = 0;
	ring[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	if (eng->drmClass == 0) {
		// xe_wa.c lrc_was / xe_tuning.c lrc_tunings for graphics 20.01 render: context registers, set once in the
		// ring (Linux records them into the default LRC with emit_wa_job).
		const uint32_t lri[][2] = {
			{0x7044, (1u << 5 | 1u << 10) * 0x10001},   // XEHP_PSS_CHICKEN FD_END_COLLECT, FLSH_IGNORES_PSD
			{0x83a8, (1u << 13) * 0x10001},             // VF_SCRATCHPAD XE2_VFG_TED_CREDIT_INTERFACE_DISABLE
			{0x7010, (1u << 14) * 0x10001},             // XEHP_COMMON_SLICE_CHICKEN1 DISABLE_BOTTOM_CLIP_RECTANGLE_TEST
			{0x6204, (1u << 6) * 0x10001},              // CHICKEN_RASTER_1 DIS_CLIP_NEGATIVE_BOUNDING_BOX
			{0x6208, (1u << 5) * 0x10001},              // CHICKEN_RASTER_2 TBIMR_FAST_CLIP
			{0x5588, (1u << 10) * 0x10001},             // WM_CHICKEN3 HIZ_PLANE_COMPRESSION_DIS
			{0x62a8, (1u << 9 | 1u << 6) * 0x10001},    // VFLSKPD DIS_PARTIAL_AUTOSTRIP, DIS_AUTOSTRIP
			{0x7300, (1u << 12) * 0x10001},             // XEHP_COMMON_SLICE_CHICKEN4 SBE_PUSH_CONSTANT_BEHIND_FIX_ENABLE
			{0x6210, gFfMode},                          // FF_MODE: mesh autostrip off, VS hit max 0x3f
		};
		ring[n++] = MI_LOAD_REGISTER_IMM | MI_LRI_NUM_REGS(9);
		for (auto &r : lri) {
			ring[n++] = r[0];
			ring[n++] = r[1];
		}
		if (q->stateCacheFix) {   // xe_lrc_init XE_LRC_DISABLE_STATE_CACHE_PERF_FIX
			ring[n++] = MI_LOAD_REGISTER_IMM | MI_LRI_NUM_REGS(1);
			ring[n++] = 0x7304;                  // COMMON_SLICE_CHICKEN3
			ring[n++] = (1u << 13) * 0x10001;    // DISABLE_STATE_CACHE_PERF_FIX (masked)
		}
	}
	if (n & 1) ring[n++] = MI_NOOP;
	q->tail = n * 4;
	regs[CTX_RING_TAIL] = q->tail;
	for (uint32_t i = 0; i < 256; i++) q->ringPos[i] = q->tail;
	memcpy(q->lrcCpu, l, q->lrcLen);
	IOFree(l, q->lrcLen);
	lrcSync(q, q->lrcCpu, q->lrcLen);
	uint32_t inval = 0;
	IOLockLock(gResLock);
	int err = lrcGgttMap(q, &inval);   // a media queue's LRC: the media GT's GGTT TLB
	IOLockUnlock(gResLock);
	// rb8d: the GGTT TLBs no longer translate this slot's window to the LRC of the queue that had it before, before the
	// GuC is told about this one (xe_lrc_init -> xe_bo_create_pin_map with XE_BO_FLAG_GGTT_INVALIDATE: the insert waits)
	xeGgttInvalWait(inval);
	if (err) {
		lrcFree(q);
		IOLockLock(gResLock);
		idFree(gSlotBits, q->slot);
		idFree(gGucIdBits[g], q->gucId);
		IOLockUnlock(gResLock);
		IOFree(q, sizeof(*q));
		return err;
	}
	vmGet(vm);
	q->flushedGen = vm->ptGen - 1;   // the first batch invalidates: the ASID may have been another VM's before
	gGucState[g][q->gucId] = 0;
	gTdrWait[g][q->gucId] = 0;
	IOLockLock(gSchedLock);
	gQueueByGuc[g][q->gucId] = q;
	q->vmNext = vm->queues;   // rb8c: deferSeal waits for its jobs
	vm->queues = q;
	IOLockUnlock(gSchedLock);
	q->kernel = vm->asid >= 0xffff0;
	if (gResetting && !eng->gt)   // registered once the GuC is back
		waitUntil(nullptr, (int64_t)(nowNs() + 10000000000ull), kWaitKernel, [] { return !gResetting; });
	queueRegister(q, true);
	*out = q;
	return 0;
}

// PIPE_CONTROL bits (instructions/xe_gpu_commands.h)
static constexpr uint32_t PC_CMD_INV = 1u << 29, PC_TILE_FLUSH = 1u << 28, PC_STORE_INDEX = 1u << 21,
	PC_CS_STALL = 1u << 20, PC_TLB_INV = 1u << 18, PC_QW_WRITE = 1u << 14, PC_RT_FLUSH = 1u << 12, PC_INSTR_INV = 1u << 11,
	PC_TEX_INV = 1u << 10, PC_FLUSH_EN = 1u << 7, PC_DC_FLUSH = 1u << 5, PC_VF_INV = 1u << 4, PC_CONST_INV = 1u << 3,
	PC_STATE_INV = 1u << 2, PC_DEPTH_FLUSH = 1u << 0, PC0_UNTYPED_DP_FLUSH = 1u << 11, PC0_L3_RO_INV = 1u << 10,
	PC0_HDC_FLUSH = 1u << 9;
// PIPE_CONTROL_3D_ENGINE_FLAGS (the ones our sequences use): not on compute queues
static constexpr uint32_t PC_3D_ENGINE = PC_RT_FLUSH | PC_DEPTH_FLUSH | PC_TILE_FLUSH | PC_VF_INV | 1u << 13 | 1u << 1 |
	1u << 17 | 1u << 25 | 1u << 19;
static constexpr uint32_t MI_INVALIDATE_TLB = 1u << 18;
// A GGTT dword the ring-prologue MI_FLUSH_DW stores to (Linux: the LRC's start seqno): inside the misc test page.
static constexpr uint32_t kFlushScratchG = miscG + targetOff + 0x800;

static uint32_t pipeControl(uint32_t *dw, uint32_t n, uint32_t f0, uint32_t f1, uint32_t addr, uint32_t value) {
	dw[n++] = 3u << 29 | 3u << 27 | 2u << 24 | (6 - 2) | f0;   // GFX_OP_PIPE_CONTROL(6)
	dw[n++] = f1;
	dw[n++] = addr;
	dw[n++] = 0;
	dw[n++] = value;
	dw[n++] = 0;
	return n;
}

// The ring commands of one job (xe_ring_ops.c __emit_job_gen12_simple / _video / _render_compute).
// TLB invalidation before the batch (ring_ops_flush_tlb): MI_FLUSH_DW with MI_INVALIDATE_TLB on copy and video,
// PIPE_CONTROL_TLB_INVALIDATE on render and compute. xe_sched_job_arm sets it when the VM is batch_invalidate_tlb (a
// scratch VM) or its tlb_flush_seqno moved since the queue's last flush. Every VM here has scratch (NULL) entries a TLB
// may hold (kept even when Mesa does not ask: an out-of-bounds read by a translated Metal shader then reads zeros
// instead of banning WindowServer's queue), but instead of every batch only the first batch of each queue after the
// VM's page tables changed invalidates (flush): an entry a TLB caches between two binds is still current, so this is
// as safe as Linux's per-batch rule and costs what its non-scratch rule costs.
// Completion: render/compute flush their caches with a CS-stalling PIPE_CONTROL, then the command streamer writes the
// seqno itself (MI_STORE_DATA_IMM) before MI_USER_INTERRUPT. With Linux's PIPE_CONTROL post-sync write instead, 1-2% of
// render/compute interrupts were handled before the seqno (system memory, as Linux master keeps it since 3954313b39e4)
// was visible, and those waiters slept a whole 2 ms safety slice (Phase 1 gate, 2026-10-29); copy and video, whose
// seqno MI_FLUSH_DW writes from the command streamer, never did.
// ─── job timing (debug.intelxe.jobtime, off by default) ──────────────────────────────────────────────────────────
// Where a job's round trip goes, in the engine's own RING_TIMESTAMP clock: the ring stores the timestamp at the job's
// start, after the cache invalidation, when the batch returns and after the end-of-job flush (MI_STORE_REGISTER_MEM into
// a 16-byte slot per queue), and the CPU reads the same register at the exec ioctl, just before the GuC is told, when
// the interrupt is handled and when a syncobj waiter wakes. A job is measured at the queue's next submit when it has
// completed by then (one job at a time, as mtl_lat split runs them). The CPU reads need the GT awake: run with
// debug.intelxe.fw_hold=1 (RC6 changes nothing measurable here, 2026-11-09). Writing jobtime restarts the statistics.
int gJobTime = 0;
static IOBufferMemoryDescriptor *gJtMem;
static uint32_t gJtG;
struct JtSlot { uint32_t seq, gen, execTs, subTs; bool armed; };
static JtSlot *gJtSlots;
static volatile uint32_t gJtEngBase, gJtExecTs, gJtIrqTs, gJtWakeTs;
static uint64_t gJtN, gJtSkipped, gJtSum[7], gJtMax[7];
static uint32_t jtNow(uint32_t base) { return gDev->mmio(base + 0x358); }   // RING_TIMESTAMP (low dword)
static uint32_t jtTicksToNs(uint32_t t) { return gRefClock ? (uint32_t)((uint64_t)t * 1000000000ull / gRefClock) : 0; }   // gRefClock: Hz
static void jtSrm(uint32_t *dw, uint32_t &n, uint32_t addr) {
	dw[n++] = MI_INSTR(0x24) | 1u << 22 | 1u << 19 | 2;   // MI_STORE_REGISTER_MEM | USE_GGTT | ADD_CS_OFFSET
	dw[n++] = 0x358;                                      // RING_TIMESTAMP(0)
	dw[n++] = addr;
	dw[n++] = 0;
}
// at the queue's next submit (gSchedLock): the previous job's timestamps, if it has completed
static void jtCollect(XeQueue *q) {
	JtSlot &s = gJtSlots[q->slot];
	if (!s.armed) return;
	s.armed = false;
	if (s.gen != gSlotGen[q->slot] || (int32_t)(seqnoOf(q->slot) - s.seq) < 0) { gJtSkipped++; return; }
	auto *t = static_cast<volatile uint32_t *>(gJtMem->getBytesNoCopy()) + q->slot * 4;
	flushRange(const_cast<uint32_t *>(t), 16);
	const uint32_t a = t[0], b = t[1], c = t[2], d = t[3], irq = gJtIrqTs, wake = gJtWakeTs;
	// exec ioctl -> GuC told, -> ring start, invalidation, batch, end flush, -> interrupt handled, -> waiter awake
	const uint32_t v[7] = { s.subTs - s.execTs, a - s.subTs, b - a, c - b, d - c, irq - d, wake - irq };
	for (uint32_t i = 0; i < 7; i++) if (v[i] > 0x7fffffff) { gJtSkipped++; return; }   // out of order: not this job's
	gJtN++;
	for (uint32_t i = 0; i < 7; i++) { gJtSum[i] += v[i]; if (v[i] > gJtMax[i]) gJtMax[i] = v[i]; }
}
int xeJobTimeSet(int on) {
	if (!gDev || !gDev->up) return ENXIO;
	if (on && !gJtMem) {
		IOBufferMemoryDescriptor *m = gDev->allocSys(kSlots * 16, false);
		uint32_t g = 0;
		IOLockLock(gResLock);
		int err = m ? ggttRangeAlloc(kSlots * 16, 0x1000, false, &g) : ENOMEM;
		IOLockUnlock(gResLock);
		if (!err) err = gDev->ggttMapMem(m, g);
		JtSlot *sl = err ? nullptr : static_cast<JtSlot *>(IOMalloc(kSlots * sizeof(JtSlot)));
		if (sl) bzero(sl, kSlots * sizeof(JtSlot));
		if (err || !sl) { OSSafeReleaseNULL(m); return err ? err : ENOMEM; }
		gJtSlots = sl; gJtG = g;
		__asm__ volatile("" : : : "memory");
		gJtMem = m;
	}
	IOLockLock(gSchedLock);
	gJobTime = on ? 1 : 0;
	gJtN = gJtSkipped = 0;
	bzero(gJtSum, sizeof gJtSum); bzero(gJtMax, sizeof gJtMax);
	if (gJtSlots) for (uint32_t i = 0; i < kSlots; i++) gJtSlots[i].armed = false;
	IOLockUnlock(gSchedLock);
	return 0;
}
int xeJobTimeString(char *b, size_t len, int fwHold) {
	static const char *const nm[7] = { "exec->guc", "guc->ring start", "invalidate", "batch", "end flush", "->irq handled", "irq->waiter" };
	IOLockLock(gSchedLock);
	int k = snprintf(b, len, "jobtime %d, refclk %u Hz, jobs %llu, skipped %llu%s; average (max) us:", gJobTime, gRefClock,
		gJtN, gJtSkipped, fwHold ? "" : " (fw_hold is 0: CPU-side reads may be wrong)");
	for (uint32_t i = 0; i < 7 && k > 0 && (size_t)k < len; i++) {
		const uint64_t avg = gJtN ? jtTicksToNs((uint32_t)(gJtSum[i] / gJtN)) : 0, mx = jtTicksToNs((uint32_t)gJtMax[i]);
		k += snprintf(b + k, len - (size_t)k, " %s %llu.%llu (%llu.%llu)", nm[i], avg / 1000, avg % 1000 / 100, mx / 1000, mx % 1000 / 100);
	}
	IOLockUnlock(gSchedLock);
	return k;
}

// The job-timing slot of q, 0 if the job is not timed (job timing: render GT only).
static uint32_t jobTimeAddr(const XeQueue *q) { return gJobTime && gJtMem && !q->eng->gt ? gJtG + q->slot * 16 : 0; }

// A job's commands up to and including the jump to its batch (jobDwords; direct submission puts them in the stub).
static uint32_t jobPre(const XeQueue *q, uint64_t batch, uint32_t seq, bool flush, uint32_t jt, uint32_t *dw, uint32_t n) {
	const uint32_t seqAddr = seqnoG + q->slot * kSlotBytes;
	const bool rc = q->eng->drmClass == 0 || q->eng->drmClass == 4, compute = q->eng->drmClass == 4;
	if (jt) jtSrm(dw, n, jt);
	if (rc) {
		dw[n++] = MI_INSTR(0x5) | 1u << 8 | 1;   // preparser disable
		uint32_t f1 = PC_CMD_INV | PC_INSTR_INV | PC_TEX_INV | PC_VF_INV | PC_CONST_INV | PC_STATE_INV | PC_QW_WRITE |
			PC_STORE_INDEX | PC_CS_STALL | (flush ? PC_TLB_INV : 0);
		if (compute) f1 &= ~PC_3D_ENGINE;
		n = pipeControl(dw, n, f1 & PC_VF_INV ? PC0_L3_RO_INV : 0, f1, 0x34 * 4, 0);
		dw[n++] = MI_INSTR(0x5) | 1u << 8;       // preparser enable
	} else if (flush) {
		dw[n++] = MI_INSTR(0x5) | 1u << 8 | 1;
		dw[n++] = MI_INSTR(0x26) | 1u << 14 | 2 | MI_INVALIDATE_TLB;   // MI_FLUSH_DW, OP_STOREDW, IMM_DW
		dw[n++] = kFlushScratchG | 1u << 2;                             // MI_FLUSH_DW_USE_GTT
		dw[n++] = 0;
		dw[n++] = seq;
		dw[n++] = MI_INSTR(0x5) | 1u << 8;
	}
	if (jt) jtSrm(dw, n, jt + 4);
	// rb7o: a media queue may run on any engine of its logical mask: the engine this job starts on, as xe's
	// setup_utilization_wa stores it (ENGINE_ID read relative to the executing engine, MI_SRM_ADD_CS_OFFSET), before the
	// start seqno so a reader that sees the seqno sees this job's engine or a later one's
	if (q->eng->gt) {
		dw[n++] = MI_INSTR(0x24) | 1u << 22 | 1u << 19 | 2;   // MI_STORE_REGISTER_MEM | USE_GGTT | ADD_CS_OFFSET
		dw[n++] = 0x8c;                                       // ENGINE_ID(0)
		dw[n++] = seqAddr + 12;
		dw[n++] = 0;
	}
	// emit_copy_timestamp and the start seqno (xe_lrc_start_seqno): what the job watchdog reads to tell whether the job
	// started and how long the context has run it
	dw[n++] = MI_INSTR(0x24) | 1u << 22 | 1u << 19 | 2;   // MI_STORE_REGISTER_MEM | USE_GGTT | ADD_CS_OFFSET
	dw[n++] = 0x3a8;                                      // RING_CTX_TIMESTAMP(0)
	dw[n++] = seqAddr + 8;
	dw[n++] = 0;
	dw[n++] = MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1);
	dw[n++] = seqAddr + 4;
	dw[n++] = 0;
	dw[n++] = seq;
	if (batch) {   // batch 0: a ring-only job (golden context recording, an exec without batch that has in-fences)
		dw[n++] = MI_INSTR(0x31) | 1u << 8 | 1;   // MI_BATCH_BUFFER_START, PPGTT, XE_INSTR_NUM_DW(3)
		dw[n++] = (uint32_t)batch;
		dw[n++] = (uint32_t)(batch >> 32);
	}
	return n;
}

// A job's commands after its batch returns: kernel inline commands, the cache flush, the user fence, the seqno, the
// interrupt (jobDwords; direct submission puts them in the armed slot, never with a user fence: see queueExec).
// User fence (uf, xe_ring_ops.c): render/compute store it after the cache flush (__emit_job_gen12_render_compute),
// copy and video after a plain MI_FLUSH_DW (__emit_job_gen12_simple / _video: emit_flush_dw), both with
// MI_STORE_DATA_IMM of one qword to the PPGTT address (emit_store_imm_ppgtt_posted). The seqno store and
// MI_USER_INTERRUPT come after it, so a waiter woken by the interrupt (WAIT_USER_FENCE) reads the value.
static uint32_t jobPost(const XeQueue *q, const uint32_t *inl, uint32_t nInl, uint32_t seq, uint32_t jt, uint32_t *dw,
	uint32_t n, const XeUfence *uf = nullptr) {
	auto ufence = [&] {
		dw[n++] = MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1);
		dw[n++] = (uint32_t)uf->addr;
		dw[n++] = (uint32_t)(uf->addr >> 32);
		dw[n++] = (uint32_t)uf->value;
		dw[n++] = (uint32_t)(uf->value >> 32);
	};
	const bool hasUf = uf && uf->used;
	const uint32_t seqAddr = seqnoG + q->slot * kSlotBytes;
	const bool rc = q->eng->drmClass == 0 || q->eng->drmClass == 4, compute = q->eng->drmClass == 4;
	for (uint32_t i = 0; i < nInl; i++) dw[n++] = inl[i];   // kernel commands in the ring itself (VRAM clears)
	if (jt) jtSrm(dw, n, jt + 8);
	dw[n++] = MI_ARB_ON_OFF;                   // MI_ARB_DISABLE
	if (rc) {
		// emit_render_cache_flush: on Xe2 also the untyped dataport (LSC) flush, which HDC_PIPELINE_FLUSH no longer
		// implies from MTL on; CS_STALL holds the command streamer until the flush has completed
		uint32_t f1 = PC_TILE_FLUSH | PC_RT_FLUSH | PC_DEPTH_FLUSH | PC_DC_FLUSH | PC_FLUSH_EN | PC_CS_STALL;
		if (compute) f1 &= ~PC_3D_ENGINE;
		n = pipeControl(dw, n, PC0_HDC_FLUSH | PC0_UNTYPED_DP_FLUSH, f1, 0, 0);
		if (jt) jtSrm(dw, n, jt + 12);
		if (hasUf) ufence();
		dw[n++] = MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1);   // emit_store_imm_ggtt
		dw[n++] = seqAddr;
		dw[n++] = 0;
		dw[n++] = seq;
	} else {
		if (hasUf) {
			dw[n++] = MI_INSTR(0x26) | 2;          // emit_flush_dw: MI_FLUSH_DW | MI_FLUSH_IMM_DW
			dw[n++] = 0;
			dw[n++] = 0;
			dw[n++] = 0;
			ufence();
		}
		dw[n++] = MI_INSTR(0x26) | 1u << 14 | 2;  // MI_FLUSH_DW, OP_STOREDW, IMM_DW
		dw[n++] = seqAddr | 1u << 2;
		dw[n++] = 0;
		dw[n++] = seq;
	}
	dw[n++] = MI_INSTR(0x2);                   // MI_USER_INTERRUPT
	dw[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	dw[n++] = MI_INSTR(0x5);                   // MI_ARB_CHECK
	return n;
}

static uint32_t jobDwords(const XeQueue *q, uint64_t batch, const uint32_t *inl, uint32_t nInl, uint32_t seq, bool flush,
	uint32_t *dw, const XeUfence *uf = nullptr) {
	const uint32_t jt = jobTimeAddr(q);
	uint32_t n = jobPre(q, batch, seq, flush, jt, dw, 0);
	n = jobPost(q, inl, nInl, seq, jt, dw, n, uf);
	if (n & 1) dw[n++] = MI_NOOP;
	return n;
}

// Ring space: the head is at least where the last completed job ended. gSchedLock.
static bool ringRoom(const XeQueue *q, uint32_t n) {
	const uint32_t done = seqnoOf(q->slot);
	if ((int32_t)(done - q->submitted) > 0) return true;   // a ban completed everything
	const uint32_t head = done == q->submitted ? q->tail : q->ringPos[done % 256];
	const uint32_t used = (q->tail - head + ringSize) % ringSize;
	return ringSize - used > n * 4 + 64 && q->submitted - done < 255;
}

// Writes the job into the ring, moves the context's tail and tells the GuC (XE_GUC_ACTION_SCHED_CONTEXT).
// gSchedLock; the caller checked ringRoom.
// The job watchdog (watchRun) in 500 ms, unless it is due already. gSchedLock.
static void watchArm() {
	if (gWatchArmed || !gWatchCall) return;
	gWatchArmed = true;
	uint64_t deadline;
	clock_interval_to_deadline(500, kMillisecondScale, &deadline);
	thread_call_enter_delayed(gWatchCall, deadline);
}

// Job timing: job seq goes to the engine now (the previous job's timestamps are collected). gSchedLock.
static void jtSubmit(XeQueue *q, uint32_t seq) {
	if (gJobTime && gJtMem && !q->eng->gt) {
		jtCollect(q);
		JtSlot &s = gJtSlots[q->slot];
		gJtEngBase = q->eng->base;
		s.seq = seq; s.gen = gSlotGen[q->slot]; s.execTs = gJtExecTs; s.subTs = jtNow(q->eng->base); s.armed = true;
	}
}

// The dwords into the ring at the tail, which moves past them (not yet in the LRC). gSchedLock.
static void ringCopy(XeQueue *q, const uint32_t *dw, uint32_t n) {
	auto *ring = reinterpret_cast<uint32_t *>(q->lrcCpu);
	const uint32_t start = q->tail / 4, room = ringSize / 4 - start, first = n < room ? n : room;
	memcpy(ring + start, dw, first * 4);
	if (!q->lrcBlk) flushRange(ring + start, first * 4);
	if (first < n) {
		memcpy(ring, dw + first, (n - first) * 4);
		if (!q->lrcBlk) flushRange(ring, (n - first) * 4);
	}
	// VRAM: the ring contents are out before the tail that publishes them (xe_lrc_write_ring's xe_device_wmb)
	if (q->lrcBlk) gDev->deviceWmb();
	q->tail = (q->tail + n * 4) % ringSize;
}

static void ringWrite(XeQueue *q, const uint32_t *dw, uint32_t n, uint32_t seq) {
	auto *regs = lrcRegs(q);
	ringCopy(q, dw, n);
	q->submitted = seq;
	if (gJobTimeoutMs > 0) watchArm();   // the job watchdog looks while anything is outstanding
	q->ringPos[seq % 256] = q->tail;
	regs[CTX_RING_TAIL] = q->tail;
	lrcSync(q, regs + CTX_RING_TAIL, 4);
	if (gResetting && !q->eng->gt) return;   // the context is scheduled when it is registered again
	uint32_t act[2] = {0x1000, q->gucId};
	jtSubmit(q, seq);
	gDev->guc(act, 2, q->eng->gt);
}

// Whether the job that goes to the ring now invalidates the TLB first (the VM's page tables changed since this queue's
// last invalidation).
// rb7o: a queue with more than one engine in its mask invalidates the TLB before every batch (xe_sched_job_arm's
// batch_invalidate_tlb, Linux's rule for scratch VMs). The once-per-page-table-change rule assumes the batches after
// the first run on the engine that first one invalidated; the GuC may run the next one on another engine, whose TLB
// can still hold an entry (a scratch one) from before the change.
static bool tlbFlushDue(const XeQueue *q, uint32_t gen) {
	return gen != q->flushedGen || (gTlbEveryBatch && q->vm->scratch) ||   // rb7k: Linux's scratch-VM rule on demand
		(q->logicalMask & (q->logicalMask - 1));
}

// ─── direct submission (debug.intelxe.direct, off by default; docs/design/DESIGN-direct-submission.md) ────────────
// Linux xe has none: every job is a ring write and SCHED_CONTEXT, and the GuC puts the context on the engine for it
// (29 of a 56 us round trip here). Intel's compute runtime keeps a context on the engine polling a mailbox with
// MI_SEMAPHORE_WAIT (ULLS), in user space. This does it in the ring of a primary-GT compute queue that has
// DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT, so fences, the watchdog, bans and GT-reset replay keep working. While the queue is
// armed its ring ends with one slot for the next seqno a, and the LRC tail right after it:
//   preparser off, MI_SEMAPHORE_WAIT (poll, GGTT mailbox >= a), preparser on   (the context waits on the engine here)
//   MI_BATCH_BUFFER_START (first level, GGTT) to stub[a % 256]
//   jobPost: flush, seqno a, MI_USER_INTERRUPT, MI_ARB_CHECK
// Job a goes in through its stub (jobPre: the invalidating PIPE_CONTROL, with TLB invalidation when the page tables
// changed, CTX_TIMESTAMP and the start seqno, then a chained MI_BATCH_BUFFER_START, PPGTT, to the batch, whose
// MI_BATCH_BUFFER_END returns to the ring) and then mailbox = a: the job starts. Then, off the latency path, the slot
// for a + 1 and SCHED_CONTEXT for the new tail (the GuC moves the tail of the running context, as for any job submitted
// while the previous one runs). A job submitted while the previous one runs takes the slot that one's submit armed, so
// one slot is armed at a time and back-to-back jobs need no fallback.
// Leaving (dsExit: idle for debug.intelxe.direct_idle_us, another queue on the engine class, an exec that waits in the
// FIFO, suspend, GT reset, close, the switch off): stub = MI_BATCH_BUFFER_END and the slot's seqno is consumed like an
// empty kernel job, with nothing armed after it; the ring runs dry and the GuC switches the context out. A job that
// finds no ring room for the next slot leaves the same way (it is the stretch's last). A ban only drops the state: the
// context stays off. The next job takes the normal path; one that follows it within the idle time arms again (dsEnter).
// Invariants (gSchedLock): armed => the FIFO is empty and seqno == submitted (the slot's seqno is submitted + 1 and no
// fence has it yet); at most one queue armed (gDsQ); stub[x % 256] is written for job x only after x - 256 completed
// (at most 255 jobs outstanding, as ringRoom); a stretch starts only when the last one's slots have all run, so the
// mailbox can be set to a - 1; a is never 0 (the >= would pass on the previous value 0xffffffff). The mailbox only grows
// within a stretch, so a GT reset replays every slot left in the ring without waiting.
int gDirect = 0, gDirectIdleUs = 2000;
static constexpr uint32_t kDsEntries = 32, kDsStubBytes = 128, kDsEntryBytes = 256 * kDsStubBytes;
// Page 0: a mailbox per entry, 64 bytes apart; then per entry 256 stubs of 32 dwords. System memory, GGTT-mapped by the
// first debug.intelxe.direct=1, never freed (as gJtMem).
static IOBufferMemoryDescriptor *gDsMem;
static uint32_t gDsG, gDsBits;   // its GGTT address; entries in use (gResLock)
static thread_call_t gDsCall;    // dsIdleRun
static XeQueue *gDsQ;            // the armed queue (gSchedLock)
static const XeQueue *gDsLastQ[2];  // per class (render, compute) of the primary GT: the last queue that submitted (only
static uint64_t gDsLastNs[2];       // compared) and when (gSchedLock)

// WindowServer and the processes that draw the system's interface never arm (the Metal driver keeps them on its old paths
// too, nvmtl_plock_system_ui): what they submit runs whenever the screen changes, and they must never wait behind a
// polling context. q->name is the process name cut to 15 characters.
static bool dsSystemUi(const XeQueue *q) {
	static const char *const ui[] = { "WindowServer", "loginwindow", "Dock", "com.apple.dock.", "SystemUIServer",
		"ControlCenter", "NotificationCen", "WindowManager", "ScreenSaverEngi", "legacyScreenSav" };
	for (const char *u : ui) if (!strncmp(q->name, u, 15)) return true;
	return false;
}

static void queueBan(XeQueue *q);

static uint64_t dsIdleNs() {
	const int us = gDirectIdleUs;
	return (uint64_t)(us < 20 ? 20 : us > 1000000 ? 1000000 : us) * 1000;
}
static uint32_t dsMboxG(const XeQueue *q) { return gDsG + (q->dsEntry - 1u) * 64; }
static uint32_t dsStubG(const XeQueue *q, uint32_t s) {
	return gDsG + 0x1000 + (q->dsEntry - 1u) * kDsEntryBytes + s % 256 * kDsStubBytes;
}
static uint32_t *dsCpu(uint32_t g) {
	return reinterpret_cast<uint32_t *>(static_cast<uint8_t *>(gDsMem->getBytesNoCopy()) + (g - gDsG));
}

// The mailbox, out of the CPU cache (the semaphore polls memory) after everything written before it.
static void dsMboxSet(const XeQueue *q, uint32_t v) {
	volatile uint32_t *m = dsCpu(dsMboxG(q));
	*m = v;
	flushRange(const_cast<uint32_t *>(m), 4);
}

// The stub of job s: jobPre (at most 27 dwords) and the jump to the batch, or MI_BATCH_BUFFER_END after it for a job
// without batch, or MI_BATCH_BUFFER_END alone for the no-op that leaves direct mode.
static void dsStubWrite(const XeQueue *q, uint32_t s, uint64_t batch, bool flush, bool noop) {
	uint32_t *p = dsCpu(dsStubG(q, s));
	uint32_t n = 0;
	if (!noop) n = jobPre(q, batch, s, flush, jobTimeAddr(q), p, 0);
	if (noop || !batch) p[n++] = MI_BATCH_BUFFER_END;
	flushRange(p, n * 4);
}

// The armed slot for seqno a (at most 32 dwords, even). The preparser is off across the wait, so nothing after it (the
// stub) is fetched before the mailbox moves.
static uint32_t dsSlot(const XeQueue *q, uint32_t a, uint32_t *dw) {
	uint32_t n = 0;
	dw[n++] = MI_INSTR(0x5) | 1u << 8 | 1;   // preparser disable
	// MI_SEMAPHORE_WAIT (xe_mi_commands.h, XE_INSTR_NUM_DW(5)): MI_SEMW_GGTT, MI_SEMW_POLL, COMPARE_OP_SAD_GTE_SDD;
	// semaphore data, address, token
	dw[n++] = MI_INSTR(0x1c) | 1u << 22 | 1u << 15 | 1u << 12 | 3;
	dw[n++] = a;
	dw[n++] = dsMboxG(q);
	dw[n++] = 0;
	dw[n++] = 0;
	dw[n++] = MI_INSTR(0x5) | 1u << 8;       // preparser enable
	dw[n++] = MI_INSTR(0x31) | 1;            // MI_BATCH_BUFFER_START, first level, GGTT (privileged, as the ring)
	dw[n++] = dsStubG(q, a);
	dw[n++] = 0;
	n = jobPost(q, nullptr, 0, a, jobTimeAddr(q), dw, n);
	if (n & 1) dw[n++] = MI_NOOP;
	return n;
}

// Room for n more dwords while armed: the engine is at or after where job `done` ended (the armed slot's start when
// every job has completed). ringRoom's q->tail case does not hold here: the armed slot is never consumed.
static bool dsRoom(const XeQueue *q, uint32_t n, uint32_t done) {
	const uint32_t used = (q->tail - q->ringPos[done % 256] + ringSize) % ringSize;
	return ringSize - used > n * 4 + 64;
}

static bool dsEntryGet(XeQueue *q) {
	IOLockLock(gResLock);
	if (!q->dsEntry && ~gDsBits) {
		const uint32_t e = (uint32_t)__builtin_ctz(~gDsBits);
		gDsBits |= 1u << e;
		q->dsEntry = (uint8_t)(e + 1);
	}
	IOLockUnlock(gResLock);
	return q->dsEntry != 0;
}

// The entry goes with the guc_id, once the context is off the hardware (its slots may run until then). gResLock.
static void dsEntryPut(XeQueue *q) {
	if (!q->dsEntry) return;
	gDsBits &= ~(1u << (q->dsEntry - 1));
	q->dsEntry = 0;
}

static void dsDisarm(XeQueue *q) {
	q->dsArmed = false;
	q->dsEndSeq = q->submitted;
	gDsQ = nullptr;
}

// Leaves direct mode: the armed slot runs a no-op whose seqno is consumed like an empty kernel job, and nothing is
// armed after it. No H2G: the slot is inside the tail already. why: the counter. gSchedLock.
static void dsExit(XeQueue *q, uint32_t *why) {
	if (!q->dsArmed) return;
	if (q->seqno != q->submitted || q->jobHead) {   // never: a fence has the slot's seqno and must not signal early
		g2hLog("direct: queue guc_id %u armed with seqno %u handed out, %u submitted: banned\n", q->gucId, q->seqno,
			q->submitted);
		// the armed slot runs a no-op first, so the context stops polling and can leave the engine (the ban floor keeps
		// the fences right whatever seqno the slot stores)
		dsStubWrite(q, q->submitted + 1, 0, false, true);
		dsMboxSet(q, q->submitted + 1);
		queueBan(q);
		return;
	}
	const uint32_t s = q->submitted + 1;
	dsStubWrite(q, s, 0, false, true);
	dsMboxSet(q, s);
	q->submitted = q->seqno = s;
	if (gJobTimeoutMs > 0) watchArm();
	dsDisarm(q);
	(*why)++;
}

// Another queue's job on the armed queue's engine class: the armed one leaves, so the GuC does not timeslice the new
// job against a context that polls. gSchedLock.
static void dsContend(const XeQueue *q) {
	XeQueue *a = gDsQ;
	if (a->eng->gt == q->eng->gt && a->eng->drmClass == q->eng->drmClass) dsExit(a, &gStats.dsContendExits);
}

// Job seq (submitted + 1, the armed slot's seqno) into the armed slot: its stub, then the mailbox; then the slot for
// seq + 1, or none (the queue leaves direct mode with this job) when the switch is off, the queue is held, a reset or
// suspend is on, or the ring or the 256 ringPos entries have no room. Always emits. gSchedLock.
static bool dsSubmit(XeQueue *q, uint64_t batch, uint32_t seq) {
	const uint32_t gen = q->vm->ptGen;
	const bool flush = tlbFlushDue(q, gen);
	dsStubWrite(q, seq, batch, flush, false);
	if (flush) {
		q->flushedGen = gen;
		gStats.tlbRingFlushes++;
	}
	jtSubmit(q, seq);
	dsMboxSet(q, seq);   // the job starts: the latency path ends here
	q->submitted = seq;
	if (gJobTimeoutMs > 0) watchArm();
	gStats.dsSubmits++;
	const int ci = q->eng->drmClass == DRM_XE_ENGINE_CLASS_COMPUTE;
	gDsLastQ[ci] = q;
	gDsLastNs[ci] = q->dsLastNs = nowNs();
	const uint32_t done = seqnoOf(q->slot);
	uint32_t sdw[40];
	const uint32_t m = dsSlot(q, seq + 1, sdw);
	if (!gDirect || seq + 1 == 0 || q->tdr || gResetting || gSuspended || gWedged || seq - done >= 255 ||
		!dsRoom(q, m, done)) {
		dsDisarm(q);
		gStats.dsOtherExits++;
		return true;
	}
	ringCopy(q, sdw, m);
	q->ringPos[(seq + 1) % 256] = q->tail;
	auto *regs = lrcRegs(q);
	regs[CTX_RING_TAIL] = q->tail;
	lrcSync(q, regs + CTX_RING_TAIL, 4);
	uint32_t act[2] = {0x1000, q->gucId};
	gDev->guc(act, 2, 0);
	return true;
}

// After job seq on the normal path (dw, n: its ring dwords, ringRoom checked): a low-latency compute queue of the primary
// GT whose previous job was less than the idle time ago, and that shared the engine class with no other queue in that
// time, gets the slot for seq + 1 behind the job, in the same ring write and SCHED_CONTEXT. Only on the exec path with
// an empty FIFO (seq is the last seqno handed out). False: not armed, nothing written. gSchedLock, the switch on.
static bool dsEnter(XeQueue *q, const uint32_t *dw, uint32_t n, uint32_t seq, uint32_t nInl) {
	if (q->eng->gt || (q->eng->drmClass != DRM_XE_ENGINE_CLASS_COMPUTE && q->eng->drmClass != DRM_XE_ENGINE_CLASS_RENDER))
		return false;
	const int ci = q->eng->drmClass == DRM_XE_ENGINE_CLASS_COMPUTE;
	const uint64_t now = nowNs(), idle = dsIdleNs();
	const bool alone = gDsLastQ[ci] == q || now - gDsLastNs[ci] >= idle, recent = q->dsLastNs && now - q->dsLastNs < idle;
	gDsLastQ[ci] = q;
	gDsLastNs[ci] = q->dsLastNs = now;
	// 9 Nov: any application queue, not only those with the low-latency hint (off by default now), and render too, where
	// Metal and OpenCL apps run their work; WindowServer's next frame on the render class makes an armed queue leave
	if (q->kernel || dsSystemUi(q) || nInl || !alone || !recent || !gDsMem || !gDsCall) return false;
	// one armed queue in the whole device: since the render class may arm too, a compute and a render queue could
	// otherwise both arm, and the one gDsQ no longer names would keep a slot nobody releases (review 9 Nov)
	if (gDsQ && gDsQ != q) return false;
	if (q->banned || q->closed || q->tdr || gResetting || gSuspended || gWedged) return false;
	if (q->jobHead || seq != q->seqno + 1 || seq + 1 == 0) return false;
	const uint32_t done = seqnoOf(q->slot);
	if (seq - done >= 255 || (q->dsEntry && (int32_t)(done - q->dsEndSeq) < 0)) return false;
	if (!dsEntryGet(q)) return false;
	uint32_t sdw[40];
	const uint32_t m = dsSlot(q, seq + 1, sdw);
	if (!ringRoom(q, n + m)) return false;
	dsMboxSet(q, seq);   // the slot waits for its job
	ringCopy(q, dw, n);
	const uint32_t jobEnd = q->tail;
	ringWrite(q, sdw, m, seq);   // the tail after the slot, SCHED_CONTEXT
	q->ringPos[seq % 256] = jobEnd;
	q->ringPos[(seq + 1) % 256] = q->tail;
	q->dsArmed = true;
	gDsQ = q;
	gStats.dsArms++;
	uint64_t deadline;
	nanoseconds_to_absolutetime(now + idle, &deadline);
	thread_call_enter_delayed(gDsCall, deadline);
	return true;
}

// The idle exit: the armed queue leaves once it has submitted nothing for debug.intelxe.direct_idle_us (at once when the
// switch went off), so nothing polls and the engine can idle. Thread call.
static void dsIdleRun(thread_call_param_t, thread_call_param_t) {
	IOLockLock(gSchedLock);
	if (XeQueue *q = gDsQ) {
		const uint64_t now = nowNs(), idle = dsIdleNs();
		if (!gDirect) {
			dsExit(q, &gStats.dsOtherExits);
		} else if (now - q->dsLastNs >= idle) {
			dsExit(q, &gStats.dsIdleExits);
		} else {
			uint64_t deadline;
			nanoseconds_to_absolutetime(q->dsLastNs + idle, &deadline);
			thread_call_enter_delayed(gDsCall, deadline);
		}
	}
	IOLockUnlock(gSchedLock);
}

// debug.intelxe.direct: 1 allocates and maps the mailboxes and stubs the first time (kept); 0 makes the armed queue leave.
int xeDirectSet(int on) {
	if (on && !gDsMem) {
		if (!gDev || !gDev->up) return ENXIO;
		int err = 0;
		IOLockLock(gDevLock);
		if (!gDsMem) {
			const uint32_t len = 0x1000 + kDsEntries * kDsEntryBytes;
			thread_call_t c = thread_call_allocate_with_options(dsIdleRun, nullptr, THREAD_CALL_PRIORITY_KERNEL, 0);
			IOBufferMemoryDescriptor *m = c ? gDev->allocSys(len, false) : nullptr;
			uint32_t g = 0;
			if (m) {
				bzero(m->getBytesNoCopy(), len);
				flushRange(m->getBytesNoCopy(), len);
			}
			IOLockLock(gResLock);
			err = m ? ggttRangeAlloc(len, 0x1000, false, &g) : ENOMEM;
			IOLockUnlock(gResLock);
			if (!err && (err = gDev->ggttMapMem(m, g))) {
				IOLockLock(gResLock);
				ggttRangeFree(g);
				IOLockUnlock(gResLock);
			}
			if (err) {
				OSSafeReleaseNULL(m);
				if (c) thread_call_free(c);
			} else {
				gDsG = g;
				gDsCall = c;
				__asm__ volatile("" : : : "memory");
				gDsMem = m;
			}
		}
		IOLockUnlock(gDevLock);
		if (err) return err;
	}
	IOLockLock(gSchedLock);
	gDirect = on ? 1 : 0;
	IOLockUnlock(gSchedLock);
	if (!on && gDsCall) thread_call_enter(gDsCall);
	return 0;
}

// Emits the job if the ring has room. gSchedLock. The VM's page-table generation is read here, when the job goes to
// the ring: binds bump it after their PTE writes and before signaling their out-fences, so a job that depends on a
// bind sees it.
// Direct submission: an armed queue's job goes into its armed slot (dsSubmit), another queue's job on the armed one's
// engine class makes that one leave first (dsContend), and a compute queue may arm behind its job (dsEnter).
// uf: the job's user fence (an exec's); an armed queue never gets one here (queueExec takes it out of direct mode first).
static bool queueEmitEx(XeQueue *q, uint64_t batch, const uint32_t *inl, uint32_t nInl, uint32_t seq, uint32_t *dw,
	const XeUfence *uf = nullptr) {
	if (gDsQ) {   // debug.intelxe.direct: a queue is armed
		if (gDsQ == q) return dsSubmit(q, batch, seq);
		dsContend(q);
	}
	const uint32_t gen = q->vm->ptGen;
	const bool flush = tlbFlushDue(q, gen);
	const uint32_t n = jobDwords(q, batch, inl, nInl, seq, flush, dw, uf);
	if (!ringRoom(q, n)) return false;
	if (!gDirect || !dsEnter(q, dw, n, seq, nInl)) ringWrite(q, dw, n, seq);
	if (flush) {
		q->flushedGen = gen;
		gStats.tlbRingFlushes++;
	}
	return true;
}

static bool queueEmit(XeQueue *q, uint64_t batch, uint32_t seq, const XeUfence *uf = nullptr) {
	uint32_t dw[64];   // jobDwords: at most 55 (render with job timing and a user fence)
	return queueEmitEx(q, batch, nullptr, 0, seq, dw, uf);
}

static void jobFree(XeJob *j) { IOFree(j, sizeof(XeJob) + j->capDeps * sizeof(XeFence)); }

static void queueDropJobs(XeQueue *q) {
	for (XeJob *j = q->jobHead, *nx; j; j = nx) {
		nx = j->next;
		jobFree(j);
	}
	q->jobHead = q->jobTail = nullptr;
	q->jobCount = 0;
}

// xe_guc_exec_queue_reset_trigger_cleanup: the queue is banned, its waiting jobs are dropped and its seqno jumps to
// the last one handed out, so every fence it gave completes. gSchedLock.
// INTELXE_CAP_STATUS_PAGE: the page shared read-only with every client (made at the first request); ban_epoch moves after
// every ban (the queue is marked first, so a client that sees the new epoch and asks gets the ban).
static IOBufferMemoryDescriptor *gStatusPage;
static volatile uint64_t *gBanEpoch;
static constexpr uint32_t kStatusPageType = (1u << 30) - 1;   // just below kMapCookie: no BO handle gets this far
static IOMemoryDescriptor *statusPage() {
	if (!gStatusPage) {
		IOBufferMemoryDescriptor *m = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryKernelUserShared,
			4096, 4096);
		if (!m) return nullptr;
		bzero(m->getBytesNoCopy(), 4096);
		if (OSCompareAndSwapPtr(nullptr, m, (void *volatile *)&gStatusPage))
			gBanEpoch = static_cast<volatile uint64_t *>(m->getBytesNoCopy());
		else
			m->release();
	}
	gStatusPage->retain();
	return gStatusPage;
}
static void queueBan(XeQueue *q) {
	if (q->banned) return;
	q->banned = true;
	if (q->dsArmed) {   // direct submission: the armed slot stays in the ring of a context that stays off
		dsDisarm(q);
		gStats.dsOtherExits++;
	}
	if (gBanEpoch) { __sync_synchronize(); OSIncrementAtomic64(reinterpret_cast<volatile SInt64 *>(gBanEpoch)); }
	queueDropJobs(q);
	q->submitted = q->seqno;
	gSlotBanSeq[q->slot] = q->seqno;   // the ban floor (fenceSignaled), before the generation that enables it
	__sync_synchronize();
	gSlotBanGen[q->slot] = q->gen;
	seqnoSet(q->slot, q->seqno);
	gStats.bans++;
}

// A kernel job without in-fences (golden contexts, VRAM clears, whose commands go in the ring: inl): waits for ring
// space if needed.
static int queueSubmitKernel(XeQueue *q, uint64_t batch, XeFence *out, const uint32_t *inl = nullptr, uint32_t nInl = 0) {
	int err = 0;
	const uint32_t cap = 64 + nInl;
	auto *dw = static_cast<uint32_t *>(IOMalloc(cap * 4));
	if (!dw) return ENOMEM;
	IOLockLock(gSchedLock);
	const uint32_t seq = q->seqno + 1;
	if (!queueEmitEx(q, batch, inl, nInl, seq, dw)) {
		IOLockUnlock(gSchedLock);
		err = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitRing, [&] { return ringRoom(q, cap); }) ? EBUSY : 0;
		IOLockLock(gSchedLock);
		if (!err && !queueEmitEx(q, batch, inl, nInl, seq, dw)) err = EBUSY;
	}
	IOFree(dw, cap * 4);
	if (!err) {
		q->seqno = seq;
		*out = {0, q->slot, q->gen, seq};
	}
	IOLockUnlock(gSchedLock);
	return err;
}

// Schedule disable (XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET, runnable 0): the GuC preempts the context off its engine and
// confirms with SCHED_CONTEXT_MODE_DONE (gGucState kSchedOff). Sent once per queue. gSchedLock.
static void queueSchedOff(XeQueue *q) {
	if (q->schedOffSent) return;
	q->schedOffSent = true;
	gGucState[gtIdx(q->eng->gt)][q->gucId] = 0;
	uint32_t dis[3] = {0x1001, q->gucId, 0};
	gDev->guc(dis, 3, q->eng->gt);
}

// set_min_preemption_timeout (disable_scheduling with immediate): UPDATE_CONTEXT_POLICIES with only the preemption
// timeout, 1 us, so the schedule disable that follows takes a context that does not yield off the engine at once (an
// engine reset) instead of after the 640 ms of init_policies. gSchedLock.
static void queueMinPreempt(XeQueue *q) {
	uint32_t pol[4] = {0x100b, q->gucId, 0x2002u << 16 | 1, 1};
	gDev->guc(pol, 4, q->eng->gt);
}

// guc_exec_queue_timedout_job for a queue that is to be banned without a timeout check (skip_timeout_check: the GuC
// reset it, CONTEXT_RESET_NOTIFICATION or NOTIFY_MEMORY_CAT_ERROR, or the watchdog gave up on it): submission to it is
// held (tdr 2), the GuC takes it off the engine at once (disable_scheduling(immediate)) and the watchdog bans it when
// SCHED_CONTEXT_MODE_DONE confirms it is off: "Must wait for scheduling to be disabled before signalling any fences".
// Not confirmed within 5 s: a GT reset. A closed queue is left to its teardown, which disables it the same way.
// gSchedLock.
static void queueKickOff(XeQueue *q) {
	if (q->banned || q->tdr == 2) return;
	q->tdr = 2;
	q->tdrSince = nowNs();
	if (!q->schedOffSent) {
		queueMinPreempt(q);
		queueSchedOff(q);
	}
	gTdrWait[gtIdx(q->eng->gt)][q->gucId] = 1;
	watchArm();
}

// xe_sched_invalidate_job(job, 2): counts a timeout or a GT reset against the queue's first unfinished job (seqno);
// returns the new count (more than 2: that job is banned rather than tried again).
static uint32_t jobKarma(XeQueue *q, uint32_t seq) {
	if (q->karmaSeq != seq) {
		q->karmaSeq = seq;
		q->karma = 0;
	}
	if (q->karma < 255) q->karma++;
	return q->karma;
}

// rb8a: what the render and compute engines (one reset domain) were doing, written to the G2H log when the GuC resets a
// context (CAT error, CONTEXT_RESET) and when a job the GuC never started is about to reset the GT: the GT fault
// register (XEHP_RING_FAULT_REG: valid, type, source, engine; the address in XEHP_FAULT_TLB_DATA0/1), then per engine
// ring head and tail, ACTHD, the context on it (RING_CURRENT_LRCA, matched to the queue whose LRC holds it), the
// execlist status, MI_MODE, ESR and EIR, IPEHR and INSTDONE; then every primary-GT queue with work pending: completed,
// started and submitted seqno and the GuC state the driver knows. gSchedLock.
// rb8b (2026-11-10: two MEMORY_CAT_ERRORs of a Geekbench OpenCL run after a long idle, no page-fault descriptor: a VM
// without fault mode touched an unmapped VA): then what could have taken a page from under the job. A BAR-window BO move
// or an unbind racing the job or its TLB invalidation shows as a recent move / remap / unbind, a queue that last
// invalidated at an older page-table generation than its VM's, or an invalidation still in flight. Lines:
//   moves: BO moves into / out of the BAR window, to system memory (maps the window could not take), purges, eviction
//          scans; the last move, its kind and how long ago
//   tlb:   invalidations issued and completed on each GuC (pending when done < issued), the last one's age, deferred
//          frees still waiting for one
//   vm:    the faulting queue's VM (fq): ASID, scratch, ptGen against the generation the queue's last invalidating job
//          saw (flushedGen), its bindings, its last unbind (and whether that unbind's invalidation completed), remap
//          by a move and bind, as ages
// gSchedLock (the queue and its VM stay); the VM fields are read without its client's lock.
static struct { uint64_t ns; char kind; } gLastMove;   // rb8b: the last BO move: i/o window, s system memory, p purge
static void lastMoveSet(char kind) {
	gLastMove.ns = nowNs();
	gLastMove.kind = kind;
}
static bool tlbSeqDone(uint32_t seq);
static uint32_t purgedCount();
static const char *msAgo(uint64_t ns, uint64_t now, char *b, size_t n) {
	if (!ns) snprintf(b, n, "never");
	else snprintf(b, n, "%llu ms ago", now > ns ? (now - ns) / 1000000 : 0);
	return b;
}
static void engineDumpMm(const XeQueue *fq) {   // g2hLog lines are at most 159 characters
	const uint64_t now = nowNs();
	char a[24], b[24], d[24];
	g2hLog("  moves: in %u out %u sys %u purge %u fail %u busy %u evict %u; last %c %s\n", gStats.movesIn, gStats.movesOut,
		gStats.mapSpills, purgedCount(), gStats.moveFails, gStats.moveBusy, gStats.evictScans,
		gLastMove.ns ? gLastMove.kind : '-', msAgo(gLastMove.ns, now, a, sizeof(a)));
	const uint32_t issued = gTlbSeq, done = gDev ? gDev->tlbDone : 0, mdone = gDev ? gDev->mTlbDone : 0;
	g2hLog("  tlb: issued %u done %u/%u%s, last %s; deferred frees %u\n", issued, done, mdone,
		gDev && !tlbSeqDone(issued) ? " PENDING" : "", msAgo(gTlbIssueNs, now, a, sizeof(a)), gNDef);
	if (!fq || isBindQ(fq) || !fq->vm) return;
	const XeVm *vm = fq->vm;
	g2hLog("  vm asid %u%s: ptGen %u, queue flushed at %u%s; binds %u\n", vm->asid, vm->scratch ? " scratch" : "",
		vm->ptGen, fq->flushedGen, vm->ptGen != fq->flushedGen ? " (changed since)" : "", vm->nBinds);
	g2hLog("  vm unbind %s (n %u, tlb %u %s), remap %s, bind %s\n", msAgo(vm->unbindNs, now, a, sizeof(a)), vm->unbinds,
		vm->unbindTlb, !vm->unbindTlb ? "-" : tlbSeqDone(vm->unbindTlb) ? "done" : "PENDING",
		msAgo(vm->remapNs, now, b, sizeof(b)), msAgo(vm->bindNs, now, d, sizeof(d)));
	g2hLog("  vm unbinds whose frees waited for its jobs %u\n", vm->jobHeldSeals);
}

// rb8c (2026-11-12 CAT fault: fault register 0, scratch VM, an unbind 21 ms before): what tells the page-table
// hypotheses of docs/design/DESIGN-cat-fault.md apart. Page-table state, device-wide:
//   pt: pages in use, huge leaves split (last one's VA, level, age), tables detached by unbinds (last one's DPA and age),
//       tables leaked for want of memory; deferred frees still waiting and how many waited for a running job
static void ptStatDump() {
	const uint64_t now = nowNs();
	char a[24], b[24];
	g2hLog("  pt: pages %u, splits %u (last L%u va 0x%llx %s), detached %u (last dpa 0x%llx %s), leaked %u; deferred %u, "
		"held by jobs %u; userptr read-only %u\n", gPtPages, gPtSt.splits, gPtSt.splitLevel, gPtSt.splitVa,
		msAgo(gPtSt.splitNs, now, a, sizeof(a)), gPtSt.detached, gPtSt.detachDpa, msAgo(gPtSt.detachNs, now, b, sizeof(b)),
		gPtSt.leaked, gNDef, gStats.deferJobHeld, gStats.userptrRo);
}

// The path of va through vm's tables: the index at each level, then the entry it ends at, and every place where the
// copy in VRAM (read back through BAR2, uncached) differs from the shadow, a PDE names another table than the shadow's
// child, or a leaf points outside VRAM (DM) / at 0 (system). A difference means something other than ptSet wrote the
// table page (a stale write into a reused page) or a write that never landed. Read racily (gSchedLock only): a bind
// running at the same moment can show as a difference.
static void ptWalkDump(const XeVm *vm, uint64_t va, const char *what) {
	char b[160];
	int k = snprintf(b, sizeof(b), "  walk %s 0x%llx:", what, va);
	const PtNode *n = vm->top;
	while (n && k > 0 && (size_t)k < sizeof(b)) {
		const uint32_t lvl = n->level, i = (uint32_t)((va >> (12 + 9 * lvl)) & 511);
		const uint64_t sh = n->e[i], gpu = vram64(n->dpa)[i];
		const PtNode *c = n->child ? n->child[i] : nullptr;
		k += snprintf(b + k, sizeof(b) - (size_t)k, " L%u[%u]", lvl, i);
		if (sh != gpu) g2hLog("  walk %s: L%u[%u] of table 0x%llx: shadow 0x%llx, VRAM 0x%llx DIFFER\n", what, lvl, i, n->dpa, sh, gpu);
		if (c && sh != pdeOf(c->dpa))
			g2hLog("  walk %s: L%u[%u] 0x%llx does not name its table 0x%llx\n", what, lvl, i, sh, c->dpa);
		if (!c) {
			const uint64_t pa = sh & ((1ull << 52) - 1) & ~0xfffull;
			const bool bad = isLeaf(sh, (int)lvl) && !(sh & 1ull << 9) && ((sh & 1ull << 11) ? pa >= vramEnd : !pa);
			if ((size_t)k < sizeof(b))
				snprintf(b + k, sizeof(b) - (size_t)k, " = 0x%llx%s%s", sh, !(sh & 1) ? " (not present)" : (sh & 1ull << 9) ?
					" (null)" : !isLeaf(sh, (int)lvl) ? " (PDE, no table)" : "", bad ? " OUT OF RANGE" : "");
			break;
		}
		n = c;
	}
	g2hLog("%s\n", b);
}

// Every table of vm (at most 256 pages: each costs 512 uncached reads, ~0.5 ms): VRAM copy against the shadow, and each
// child's PDE. Prints the count and the first few differences.
static void ptVerifyNode(const PtNode *n, uint64_t base, uint32_t *pages, uint32_t *diffs) {
	if (*pages >= 256) return;
	(*pages)++;
	volatile uint64_t *gpu = vram64(n->dpa);
	const uint64_t span = xePtSpan(n->level);
	for (uint32_t i = 0; i < 512; i++) {
		const uint64_t sh = n->e[i], g = gpu[i];
		const PtNode *c = n->child ? n->child[i] : nullptr;
		if (sh != g || (c && sh != pdeOf(c->dpa))) {
			if (++*diffs <= 4)
				g2hLog("  table L%u 0x%llx [%u] va 0x%llx: shadow 0x%llx VRAM 0x%llx%s\n", n->level, n->dpa, i, base + i * span,
					sh, g, c && sh != pdeOf(c->dpa) ? " (not its child's PDE)" : "");
		}
		if (c) ptVerifyNode(c, base + i * span, pages, diffs);
	}
}
static void ptVerifyDump(const XeVm *vm) {
	static uint64_t last;   // at most every 10 s: a reset storm would otherwise spend its time here
	const uint64_t now = nowNs();
	if (last && now - last < 10000000000ull) return;
	last = now;
	uint32_t pages = 0, diffs = 0;
	ptVerifyNode(vm->top, 0, &pages, &diffs);
	g2hLog("  tables: %u checked%s, %u entries differ from the shadow\n", pages, pages >= 256 ? " (limit)" : "", diffs);
}

static void engineDump(const char *why, const XeQueue *fq = nullptr) {
	if (!gDev || !gDev->fwOn(true)) {
		g2hLog("  dump (%s): no forcewake\n", why);
		engineDumpMm(fq);
		return;
	}
	bool walked = false;
	uint64_t bbVa = 0, acthdVa = 0;
	const uint32_t f = gDev->mmioRead(0xcec4), d0 = gDev->mmioRead(0xceb8), d1 = gDev->mmioRead(0xcebc);
	g2hLog("  dump (%s): fault 0x%08x va 0x%llx\n", why, f, (uint64_t)(d1 & 0xf) << 44 | (uint64_t)d0 << 12);
	for (uint32_t i = 0; i < gNumEngines; i++) {
		const XeEngine &e = gEngines[i];
		if (e.gt || (e.drmClass != 0 && e.drmClass != 4)) continue;
		const uint32_t b = e.base, lrca = gDev->mmioRead(b + 0x240);
		const XeQueue *on = nullptr;
		for (uint32_t k = 0; k < kMaxGucIds && !on; k++) {
			const XeQueue *q = gQueueByGuc[0][k];
			if (q && !isBindQ(q) && (lrca & ~0xfffu) - q->lrcGgtt < q->lrcLen) on = q;
		}
		g2hLog("  %s%u h %x t %x acthd %x lrca %x (%s %d) el %x:%x mi %x esr %x eir %x ipehr %x idone %x\n",
			e.drmClass ? "ccs" : "rcs", e.phys, gDev->mmioRead(b + 0x34), gDev->mmioRead(b + 0x30), gDev->mmioRead(b + 0x74), lrca,
			on ? (on->kernel ? "kernel" : on->name) : "-", on ? (int)on->gucId : -1, gDev->mmioRead(b + 0x238), gDev->mmioRead(b + 0x234),
			gDev->mmioRead(b + 0x9c), gDev->mmioRead(b + 0xb8), gDev->mmioRead(b + 0xb0), gDev->mmioRead(b + 0x68), gDev->mmioRead(b + 0x6c));
		// rb8c: the faulting context's batch: RING_BBADDR (the batch the command streamer runs) and the whole ACTHD
		if (fq && on == fq && !walked) {
			walked = true;
			bbVa = (uint64_t)gDev->mmioRead(b + 0x168) << 32 | gDev->mmioRead(b + 0x140);
			acthdVa = (uint64_t)gDev->mmioRead(b + 0x5c) << 32 | gDev->mmioRead(b + 0x74);
		}
	}
	gDev->fwOn(false);
	uint32_t n = 0;
	for (uint32_t k = 0; k < kMaxGucIds && n < 6; k++) {
		const XeQueue *q = gQueueByGuc[0][k];
		if (!q || isBindQ(q) || q->closed) continue;
		const uint32_t done = seqnoOf(q->slot);
		if ((int32_t)(done - q->submitted) >= 0 && !q->tdr) continue;
		n++;
		g2hLog("  q %u %s class %u: done %u started %u submitted %u guc 0x%x tdr %u%s%s\n", q->gucId,
			q->kernel ? "kernel" : q->name, q->cls, done, slotWord(q->slot, 1), q->submitted, gGucState[0][q->gucId], q->tdr,
			q->banned ? " banned" : "", q->schedOffSent ? " off-sent" : "");
	}
	engineDumpMm(fq);
	ptStatDump();
	if (!fq || isBindQ(fq) || !fq->vm || fq->kernel) return;
	// rb8c: the context's page-table root and ASID as its LRC holds them (a corrupted context image shows here), the
	// walks of the batch addresses the engine reported, and every table of the VM against its shadow
	auto *q = const_cast<XeQueue *>(fq);
	if (q->lrcCpu) {
		const uint32_t *regs = lrcRegs(q);
		lrcReadPrep(q, regs, 0x200);
		const uint64_t pdp = (uint64_t)regs[CTX_PDP0_UDW] << 32 | regs[CTX_PDP0_LDW];
		g2hLog("  ctx pdp0 0x%llx (vm root 0x%llx: %s), asid %u (vm %u)\n", pdp, pdeOf(fq->vm->root),
			pdp == pdeOf(fq->vm->root) ? "same" : "DIFFERENT", regs[CTX_ASID], fq->vm->asid);
	}
	if (walked) {
		ptWalkDump(fq->vm, bbVa & ~0xfffull, "bb");
		if ((acthdVa >> 12) != (bbVa >> 12)) ptWalkDump(fq->vm, acthdVa & ~0xfffull, "acthd");
	}
	ptVerifyDump(fq->vm);
}

// rb8e: bind user fences waiting for their fence (ufAsyncAdd); the worker writes them (ufAsyncRun)
static volatile uint32_t gUfaN;
static void ufAsyncRun(const XeClientState *closing = nullptr);

// ─── the submit worker (drm_sched's run_job work): ban requests from G2H, then every queue's FIFO in order ─────────
static void workRun(thread_call_param_t, thread_call_param_t) {
	gStats.workerRuns++;
	if (!gDev->irqOn) gDev->gucPoll();
	faultsDrain();
	IOLockLock(gSchedLock);
	for (uint32_t g = 0; g < 2; g++)
		for (uint32_t w = 0; w < kMaxGucIds / 32; w++) {
			// during a GT reset the primary GuC is being loaded again: its requests wait (the reset bans those queues
			// itself if their job had started)
			if (!gBanReq[g][w] || (gResetting && !g)) continue;
			uint32_t bits;
			do bits = gBanReq[g][w]; while (!OSCompareAndSwap(bits, 0, const_cast<uint32_t *>(&gBanReq[g][w])));
			// xe_guc_exec_queue_reset_trigger_cleanup -> guc_exec_queue_timedout_job: off the engine, then banned; a
			// kernel queue the GuC had to reset means a GT reset (timeout_needs_gt_reset)
			for (; bits; bits &= bits - 1)
				if (XeQueue *q = gQueueByGuc[g][w * 32 + __builtin_ctz(bits)]) {
					char wh[48];
					g2hLog("  guc_id %u%s reset by the GuC: %s (pid %d), class %u, seqno %u done %u%s\n", q->gucId,
						g ? " media" : "", q->kernel ? "kernel" : q->name, q->pid, q->cls, q->submitted, seqnoOf(q->slot),
						queueWhere(q, wh, sizeof(wh)));
					if (!g) engineDump("GuC reset", q);
					// rb8d: the engine's reset domain was just reset by the GuC: a job there that does not start soon after
					// means the engine kept this context (watchRun, xeStuckAfterReset)
					if (const int d = g ? -1 : xeResetDomain(q->cls); d >= 0) {
						gDomResetNs[d] = nowNs();
						gRecSt.domResets++;
					}
					if (q->kernel && !g) requestReset("kernel queue reset by the GuC");
					else queueKickOff(q);
				}
		}
	for (XeQueue **pp = &gSchedHead; *pp;) {
		XeQueue *q = *pp;
		while (q->jobHead && !q->tdr && !(gResetting && !q->eng->gt) && !(gSuspended && !q->kernel)) {
			XeJob *j = q->jobHead;
			uint32_t keep = 0;   // drop the deps that have signaled, so later checks are shorter
			for (uint32_t i = 0; i < j->nDeps; i++)
				if (!fenceSignaled(j->deps()[i])) j->deps()[keep++] = j->deps()[i];
			j->nDeps = keep;
			if (keep || !queueEmit(q, j->batch, j->seqno, &j->uf)) break;
			q->jobHead = j->next;
			if (!q->jobHead) q->jobTail = nullptr;
			q->jobCount--;
			jobFree(j);
			gStats.jobsDeferred++;
		}
		if (!q->jobHead) {
			*pp = q->schedNext;
			q->schedNext = nullptr;
			q->onSched = false;
		} else {
			pp = &q->schedNext;
		}
	}
	const bool pending = gSchedHead != nullptr;
	IOLockUnlock(gSchedLock);
	tlbStallCheck();   // rb8d: jobs waiting on a bind fence whose invalidation the GuC never answered
	deferReap();
	ufAsyncRun();      // rb8e: bind user fences whose fence has signaled
	// Waiting jobs are looked at again when something they can wait for completes: every job completion interrupts and
	// kicks this worker (xeIrqEvent), as do G2H (TLB, bans), queue teardown, the watchdog and resume, the way drm_sched
	// re-runs on dma-fence callbacks. A 100 ms timer remains only as a net for a lost interrupt (1 ms polling without
	// interrupts).
	if (pending || gUfaN) {   // rb8e: pending bind user fences keep the net as well
		uint64_t deadline;
		clock_interval_to_deadline(gDev->irqOn ? 100 : 1, kMillisecondScale, &deadline);
		thread_call_enter_delayed(gWorkCall, deadline);
	}
}

static void schedKick() {
	if (gWorkCall) thread_call_enter(gWorkCall);
}

// How long the context has run the job that started last, in ms: its CTX_TIMESTAMP as the last context save left it in
// the LRC minus the value the ring stored when the job started (check_timeout, xe_gt_clock_interval_to_ms).
static uint32_t ctxTimestamp(XeQueue *q) {
	auto *regs = reinterpret_cast<volatile uint32_t *>(lrcRegs(q));
	lrcReadPrep(q, const_cast<uint32_t *>(regs + CTX_TIMESTAMP), 4);
	return regs[CTX_TIMESTAMP];
}
static uint64_t tdrRunMs(XeQueue *q) {
	const uint32_t ticks = ctxTimestamp(q) - slotWord(q->slot, 2);
	return gRefClock ? (uint64_t)ticks * 1000 / gRefClock : ~0ull;
}

static void xeGtReset(const char *why);
static void zombieFree();

// The job watchdog and timeout handler, every 500 ms while jobs are outstanding or a queue is being taken off the
// engine (drm_sched's job timeout and guc_exec_queue_timedout_job). A queue whose oldest submitted job has not
// completed for debug.intelxe.job_timeout_ms:
//  - if that job never started (its start seqno was not stored), the GuC never scheduled it: that is the GuC's fault,
//    not the queue's, so the GT is reset and the job runs again (timeout_needs_gt_reset); the next time the same job
//    times out (xe_sched_invalidate_job(job, 2)) the queue is taken off the engine and banned instead. A media queue is
//    banned at once: a reset of the primary GT would not help it;
//  - otherwise submission to it is held and its scheduling disabled, which makes the GuC save the context; once that is
//    confirmed, the run time is its saved CTX_TIMESTAMP minus the one stored at the job's start (check_timeout, with
//    the 5% margin). Under the timeout (it was waiting behind other contexts) scheduling is enabled again and the clock
//    restarts; at or over it the queue is banned and stays off the engine.
// A queue being taken off the engine (tdr 2: queueKickOff) is banned once the GuC confirms the schedule disable.
// A schedule disable the GuC does not confirm within 5 s, a kernel job that times out, a GuC exception, a GuC engine
// reset failure or a broken CT channel resets the GT (xe_gt_reset_async); on the media GT, which this driver cannot
// reset on its own, the unconfirmed queue is banned (the ban floor keeps its fences signaled).
// Kernel queues are never banned: a clear that did not run must not read as done.
static void watchRun(thread_call_param_t, thread_call_param_t) {
	const uint64_t now = nowNs(), limitMs = gJobTimeoutMs > 0 ? (uint64_t)gJobTimeoutMs : 0, limit = limitMs * 1000000ull;
	bool outstanding = false, banned = false, kick = false;
	const char *reset = gResetReq ? gResetWhy : nullptr;
	IOLockLock(gSchedLock);
	gWatchArmed = false;
	for (uint32_t g = 0; g < 2 && !reset && !gResetting; g++)
		for (uint32_t i = 0; i < kMaxGucIds; i++) {
			XeQueue *q = gQueueByGuc[g][i];
			if (!q || q->closed) continue;
			const uint32_t done = seqnoOf(q->slot);
			char wh[48];   // rb7o: a media queue's mask and the engine its last job started on, for the lines below
			if (q->tdr) {
				outstanding = true;
				if (gGucState[g][q->gucId] & kSchedOff) {
					gTdrWait[g][q->gucId] = 0;
					if (q->tdr == 1) {
						gStats.tdrChecks++;
						const bool pending = (int32_t)(done - q->submitted) < 0;
						const uint64_t ran = pending ? tdrRunMs(q) : 0;
						// check_timeout: a context whose timestamp has not moved since the last check is not running the
						// job at all (a semaphore it waits on, or never put back on the engine): timed out, like one that
						// ran too long (xe_sched_invalidate_job(job, 0)); otherwise it would be put back on forever
						const uint32_t ts = pending ? ctxTimestamp(q) : 0;
						const bool stuck = pending && q->tdrSample && ts == q->tdrSample;
						q->tdrSample = ts;
						if (stuck) {
							g2hLog("job timeout: queue guc_id %u%s (class %u) seqno %u: context timestamp stuck: banned%s\n",
								q->gucId, g ? " media" : "", q->cls, done + 1, queueWhere(q, wh, sizeof(wh)));
							queueBan(q);
							gStats.watchdogBans++;
							banned = true;
						} else if (!limitMs || ran * 105 / 100 < limitMs) {   // completed meanwhile, or not run that long: back on
							uint32_t en[3] = {0x1001, q->gucId, 1};
							gDev->guc(en, 3, q->eng->gt);
							q->schedOffSent = false;
							q->wdSince = now;
							q->wdSeq = done;
							gStats.tdrRearms++;
							kick = true;
						} else {
							g2hLog("job timeout: queue guc_id %u%s (class %u) ran seqno %u for %llu ms: banned%s\n", q->gucId,
								g ? " media" : "", q->cls, done + 1, ran, queueWhere(q, wh, sizeof(wh)));
							queueBan(q);
							gStats.watchdogBans++;
							banned = true;
						}
					} else {   // tdr 2: off the engine, so its fences may signal now
						g2hLog("queue guc_id %u%s (class %u) off the engine after %llu ms: banned%s\n", q->gucId,
							g ? " media" : "", q->cls, (now - q->tdrSince) / 1000000, queueWhere(q, wh, sizeof(wh)));
						queueBan(q);
						banned = true;
					}
					q->tdr = 0;
				} else if (now - q->tdrSince > 5000000000ull) {
					if (g) {
						g2hLog("queue guc_id %u media: schedule disable not confirmed in 5 s: banned (no media GT reset)%s\n",
							q->gucId, queueWhere(q, wh, sizeof(wh)));
						gTdrWait[g][q->gucId] = 0;
						queueBan(q);
						banned = true;
						q->tdr = 0;
					} else {
						reset = "schedule disable not confirmed";
					}
				}
				continue;
			}
			if (q->banned) continue;
			if ((int32_t)(done - q->submitted) >= 0) {
				q->wdSince = 0;
				continue;
			}
			outstanding = true;
			if (!limit) continue;
			if (!q->wdSince || done != q->wdSeq) {
				q->wdSeq = done;
				q->wdSince = now;
				q->tdrSample = 0;   // a new job: its own samples (job->sample_timestamp)
				continue;
			}
			// rb8d (xe_recovery.h): a job that never started is judged at Linux's 5 s TDR (the run-time check that makes
			// the started ones' limit 10 s here cannot apply to it), and 2 s after the GuC reset a context of its reset
			// domain it is taken to be stuck behind that context: a GT reset now rather than after the timeout. WAS the
			// 10 s limit for both: the queue after the 12 Nov CAT fault never started and waited 10 s for its reset.
			const bool started = slotWord(q->slot, 1) == done + 1;
			const int dom = g ? -1 : xeResetDomain(q->cls);
			const bool stuck = dom >= 0 && xeStuckAfterReset(now, gDomResetNs[dom], q->wdSince, started);
			if (!stuck && now - q->wdSince < (started ? limit : xeNotStartedLimitMs(limitMs) * 1000000ull)) continue;
			if (q->kernel && started) {
				reset = "kernel job timed out";
				continue;
			}
			if (!started) {   // "GuC never scheduled this job"
				gStats.tdrNotStarted++;
				if (!g && jobKarma(q, done + 1) <= 2) {   // timeout_needs_gt_reset, xe_sched_invalidate_job(job, 2)
					g2hLog("job timeout: queue guc_id %u (class %u) seqno %u never started in %llu ms%s: GT reset\n", q->gucId,
						q->cls, done + 1, (now - q->wdSince) / 1000000, stuck ? " after a GuC context reset of its engine" : "");
					engineDump(stuck ? "stuck after a GuC reset" : "never started", q);
					reset = stuck ? "a job stuck behind a context the GuC reset" : "a job the GuC never started";
					if (stuck) gRecSt.stuckResets++;
					else gRecSt.notStartedResets++;
					continue;
				}
				if (q->kernel) {   // a kernel job is never banned: Linux declares the device wedged; reset once more
					reset = "kernel job never started";
					continue;
				}
				if (stuck) gRecSt.stuckOff++;
				g2hLog("job timeout: queue guc_id %u%s (class %u) seqno %u never started in %llu ms: taken off%s\n", q->gucId,
					g ? " media" : "", q->cls, done + 1, (now - q->wdSince) / 1000000, queueWhere(q, wh, sizeof(wh)));
				gStats.watchdogBans++;
				queueKickOff(q);
				continue;
			}
			q->tdrSince = now;
			queueSchedOff(q);
			gTdrWait[g][q->gucId] = 1;
			q->tdr = 1;
		}
	if (outstanding) watchArm();
	IOLockUnlock(gSchedLock);
	if (banned) wakeAll();
	if (banned || kick) schedKick();
	if (reset) xeGtReset(reset);
}

// xe_gt_reset (gt_reset_worker: xe_uc_stop -> xe_guc_submit_stop, do_gt_reset, do_gt_restart -> xe_guc_submit_start).
// Submission is held, the GT is reset and its GuC loaded again (IntelXe::gtReset); with the engines stopped the seqnos the
// rings wrote are final, and then each user queue of the primary GT is judged as guc_exec_queue_stop does: one whose
// first unfinished job had started is banned, as is one whose job has now been through more than two resets or
// timeouts (xe_sched_invalidate_job(job, 2)) and one that was being taken off the engine (tdr 2). The others keep
// their jobs, which run again from the first unfinished one (guc_exec_queue_start + xe_sched_resubmit_jobs: here the
// ring head goes back to where that job begins), as the kernel queues' clears and copies always do. Then every live
// queue is registered again: banned and closing ones left disabled for their teardown. A failed restart leaves the
// GPU stopped (xe_device_declare_wedged). Not while the device sleeps (a request then is dropped).
static void xeGtReset(const char *why) {
	if (!gDev || !gDev->up) return;
	if (gSuspended || gDev->suspended || gWedged) {
		g2hLog("GT reset (%s) not done: device %s\n", why, gWedged ? "wedged" : "suspended");
		gResetReq = false;
		return;
	}
	g2hLog("GT reset: %s\n", why);
	xePowerEvent(3);
	IOLockLock(gSchedLock);
	gResetting = true;
	// direct submission: the armed slot runs a no-op; the replay then passes every slot (the mailbox is at the last one)
	if (gDsQ) dsExit(gDsQ, &gStats.dsOtherExits);
	IOLockUnlock(gSchedLock);
	IOLockLock(gDevLock);
	const int err = gDev->gtReset();
	IOLockUnlock(gDevLock);
	IOLockLock(gSchedLock);
	uint32_t nBan = 0, nReplay = 0;
	for (uint32_t i = 0; i < kMaxGucIds; i++) {
		XeQueue *q = gQueueByGuc[0][i];
		if (!q) continue;
		const uint32_t done = seqnoOf(q->slot);
		const bool pending = (int32_t)(done - q->submitted) < 0;
		if (!q->kernel && !q->banned && (q->tdr == 2 || (pending && (slotWord(q->slot, 1) == done + 1 ||
			jobKarma(q, done + 1) > 2)))) {
			g2hLog("GT reset: queue guc_id %u of %s (pid %d, class %u) %s seqno %u: banned\n", q->gucId, q->name, q->pid, q->cls,
				q->tdr == 2 ? "being taken off at" : slotWord(q->slot, 1) == done + 1 ? "was running" : "reset 3 times at",
				done + 1);
			queueBan(q);
			nBan++;
		}
		gTdrWait[0][q->gucId] = 0;
		q->tdr = 0;
		q->wdSince = 0;
		if (err) continue;
		auto *regs = lrcRegs(q);
		const bool off = q->banned || q->closed, replay = !off && pending;
		regs[CTX_RING_HEAD] = replay ? q->ringPos[done % 256] : q->tail;
		regs[CTX_RING_TAIL] = q->tail;
		lrcSync(q, regs, 0x100);
		gGucState[0][q->gucId] = off ? kSchedOff : 0;
		q->schedOffSent = off;
		queueRegister(q, !off);
		if (replay) {
			uint32_t act[2] = {0x1000, q->gucId};
			gDev->guc(act, 2, 0);
			if (!q->kernel) nReplay++;
		}
	}
	if (!err) gResetting = false;   // else wedged: nothing more goes to the hardware
	gResetReq = false;
	if (err) {
		gWedged = true;
		for (uint32_t g = 0; g < 2; g++)
			for (uint32_t i = 0; i < kMaxGucIds; i++)
				if (XeQueue *q = gQueueByGuc[g][i]) queueBan(q);
	}
	if (!err) watchArm();   // the replayed jobs are watched again
	IOLockUnlock(gSchedLock);
	gDev->tlbDone = gDev->tlbSeqno;
	memset(gDomResetNs, 0, sizeof(gDomResetNs));   // rb8d: every engine starts afresh (xeStuckAfterReset)
	gStats.gtResets++;
	gStats.resetBans += nBan;
	gStats.resetReplays += nReplay;
	if (!err) zombieFree();
	g2hLog("GT reset: %s (%u queues banned, %u replayed)\n", err ? "FAILED, the GPU stays stopped" : "done", nBan, nReplay);
	xePowerEvent(4);
	wakeAll();
	schedKick();
}

// Power-off and restart (xe_device_shutdown -> xe_gt_shutdown): no new work, up to 500 ms for what runs to finish, then
// a full GT reset so no engine or GuC touches memory while the platform goes down.
// debug.intelxe.gt_reset=1 (root): a GT reset now, as a hang would cause one.
void xeRequestGtReset() { requestReset("requested (debug.intelxe.gt_reset)"); }
void xeRequestGtResetFor(const char *why) { requestReset(why); }

void xeShutdown() {
	if (!gDev || !gDev->up || !gSchedLock) return;
	IOLockLock(gSchedLock);
	gResetting = true;
	IOLockUnlock(gSchedLock);
	waitUntil(nullptr, (int64_t)(nowNs() + 500000000ull), kWaitKernel, [] {
		bool idle = true;
		IOLockLock(gSchedLock);
		for (uint32_t g = 0; g < 2 && idle; g++)
			for (uint32_t i = 0; i < kMaxGucIds && idle; i++)
				if (XeQueue *q = gQueueByGuc[g][i]) idle = q->banned || (int32_t)(seqnoOf(q->slot) - q->submitted) >= 0;
		IOLockUnlock(gSchedLock);
		return idle;
	});
	intelxeCrumb("2 submissions stopped, GPU idle or 500 ms passed");
	IOLockLock(gDevLock);
	gDev->gtStop();
	IOLockUnlock(gDevLock);
}

// Called by the interrupt handler after GT interrupts: runs the worker if jobs wait, frees wait for a TLB completion or
// (rb8e) bind user fences wait for a clear.
void xeIrqEvent(bool gtEvent) {
	if (!gtEvent) return;
	if (gJobTime && gJtEngBase) gJtIrqTs = jtNow(gJtEngBase);
	wakeAll();
	if (gSchedHead || gNDef || gUfaN) schedKick();
}

// ─── queue teardown (guc_exec_queue_fini / __guc_exec_queue_destroy): closed queues go to the reaper thread call,
// which lets submitted jobs finish (2 s), disables scheduling and waits for SCHED_DONE, deregisters and waits for
// DEREGISTER_DONE, and only then frees the LRC, the guc_id and the slot. A context the GuC does not confirm is off the
// hardware keeps its LRC and ids (leaked rather than reused under a running engine).
static void queueTeardown(XeQueue *q) {
	const uint32_t g = gtIdx(q->eng->gt), id = q->gucId;
	if (gResetting && !g) waitUntil(nullptr, (int64_t)(nowNs() + 10000000000ull), kWaitTeardown, [] { return !gResetting; });
	waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitTeardown, [&] {
		return q->banned || (int32_t)(seqnoOf(q->slot) - q->submitted) >= 0;
	});
	IOLockLock(gSchedLock);
	// guc_exec_queue_kill -> disable_scheduling(immediate): a queue that still has unfinished work after the 2 s (a hung
	// application killed or quitting) is taken off the engine at once, not after the 640 ms preemption timeout
	if (!q->schedOffSent && !q->banned && (int32_t)(seqnoOf(q->slot) - q->submitted) < 0) queueMinPreempt(q);
	queueSchedOff(q);   // unless the watchdog already did
	gTdrWait[g][id] = 0;
	IOLockUnlock(gSchedLock);
	// 5 s for each confirmation, as guc_exec_queue_timedout_job and enable/disable_scheduling wait (HZ * 5): a context
	// not confirmed off becomes a zombie and costs a GT reset, so a busy GuC is given Linux's time
	bool off = !waitUntil(nullptr, (int64_t)(nowNs() + 5000000000ull), kWaitTeardown, [&] { return gGucState[g][id] & kSchedOff; });
	bool dereg = false;
	if (off) {
		uint32_t d[2] = {0x4503, id};
		gDev->guc(d, 2, q->eng->gt);
		dereg = !waitUntil(nullptr, (int64_t)(nowNs() + 5000000000ull), kWaitTeardown, [&] { return gGucState[g][id] & kDeregDone; });
	}
	IOLockLock(gSchedLock);
	gQueueByGuc[g][id] = nullptr;
	for (XeQueue **pp = &q->vm->queues; *pp; pp = &(*pp)->vmNext)   // rb8c: off; gSlotGen moves next (fences signaled)
		if (*pp == q) {
			*pp = q->vmNext;
			break;
		}
	IOLockUnlock(gSchedLock);
	IOLockLock(gResLock);
	gSlotGen[q->slot]++;   // every fence the queue handed out now reads as signaled
	IOLockUnlock(gResLock);
	// rb8d: the window's PTEs cleared and the GGTT TLBs invalidated, waited for (ggttClear), while the slot (whose GGTT
	// window this is) is still this queue's: not under gResLock. WAS under it, with the unawaited MMIO invalidation.
	if (dereg) gDev->ggttClear(q->lrcGgtt, q->lrcLen, q->eng->gt);
	IOLockLock(gResLock);
	if (dereg) {
		idFree(gSlotBits, q->slot);
		idFree(gGucIdBits[g], id);
		dsEntryPut(q);
	}
	IOLockUnlock(gResLock);
	if (dereg) {
		lrcFree(q);
		gStats.queuesFreed++;
	} else {
		// The GuC may still run the context: its LRC, ids AND its VM stay (page tables and the BOs they map), or the
		// engine would go on through freed page-table pages and write VRAM that by then belongs to another process.
		// Linux keeps an exec queue (and its VM reference) until DEREGISTER_CONTEXT_DONE, and a GuC that does not answer
		// gets a GT reset; after a GT reset nothing of it can run any more, so xeGtReset frees it (zombieFree).
		gStats.queuesLeaked++;
		g2hLog("queue guc_id %u%s: GuC did not confirm %s; LRC, ids and VM kept%s\n", id, g ? " media" : "",
			off ? "deregistration" : "schedule disable", g ? "" : " until a GT reset");
		IOLockLock(gResLock);
		q->reapNext = gZombies;
		gZombies = q;
		IOLockUnlock(gResLock);
		if (!g) requestReset("a context the GuC did not confirm off");
		wakeAll();
		schedKick();
		return;
	}
	wakeAll();
	schedKick();
	vmPut(q->vm);
	IOFree(q, sizeof(*q));
}

// After a GT reset: the primary GT's zombies (queueTeardown) are off the hardware for good; their LRC, ids and VM go.
static void zombieFree() {
	XeQueue *list = nullptr;
	IOLockLock(gResLock);
	for (XeQueue **pp = &gZombies; *pp;) {
		XeQueue *q = *pp;
		if (q->eng->gt) {
			pp = &q->reapNext;
			continue;
		}
		*pp = q->reapNext;
		q->reapNext = list;
		list = q;
	}
	IOLockUnlock(gResLock);
	// rb8d: each window cleared and invalidated (waited for, ggttClear) outside gResLock, then the ids go
	for (XeQueue *q = list; q; q = q->reapNext) gDev->ggttClear(q->lrcGgtt, q->lrcLen, 0);
	IOLockLock(gResLock);
	for (XeQueue *q = list; q; q = q->reapNext) {
		idFree(gSlotBits, q->slot);
		idFree(gGucIdBits[0], q->gucId);
		dsEntryPut(q);
	}
	IOLockUnlock(gResLock);
	while (XeQueue *q = list) {
		list = q->reapNext;
		lrcFree(q);
		vmPut(q->vm);
		IOFree(q, sizeof(*q));
		gStats.queuesLeaked--;
		gStats.queuesFreed++;
	}
}

static void reapRun(thread_call_param_t, thread_call_param_t) {
	for (;;) {
		IOLockLock(gResLock);
		XeQueue *q = gReapHead;
		if (q) gReapHead = q->reapNext;
		IOLockUnlock(gResLock);
		if (!q) return;
		queueTeardown(q);
	}
}

// EXEC_QUEUE_DESTROY (xe_exec_queue_kill): no new jobs; jobs still waiting for in-fences are dropped; the rest of the
// teardown runs on the reaper so the ioctl does not wait for the GuC.
static void queueClose(XeQueue *q) {
	IOLockLock(gSchedLock);
	q->closed = true;
	if (q->dsArmed) dsExit(q, &gStats.dsOtherExits);   // direct submission: out first; the teardown waits for the no-op
	queueDropJobs(q);
	for (XeQueue **pp = &gSchedHead; *pp; pp = &(*pp)->schedNext)
		if (*pp == q) {
			*pp = q->schedNext;
			break;
		}
	q->onSched = false;
	IOLockUnlock(gSchedLock);
	IOLockLock(gResLock);
	q->reapNext = gReapHead;
	gReapHead = q;
	IOLockUnlock(gResLock);
	thread_call_enter(gReapCall);
}

// ─── debugging: debug.intelxe.dumpexec=1 records the first dwords of each batch and of one chained batch (debug builds) ──
#if INTELXE_DEBUG
int gDumpExec;
char gExecLog[16384];
static uint32_t gExecLen;

static const uint32_t *vaToCpu(XeVm *vm, uint64_t va, uint32_t *avail) {
	va &= 0xffffffffffffull;   // canonical -> 48 bits
	for (uint32_t i = 0; i < vm->nBinds; i++) {
		const XeBinding &b = vm->binds[i];
		if (!b.bo || va < b.addr || va >= b.addr + b.range) continue;
		uint64_t off = b.off + (va - b.addr);
		*avail = (uint32_t)(0x1000 - (off & 0xfff)) / 4;
		if (b.bo->vram) return boVisible(b.bo) ? reinterpret_cast<const uint32_t *>(const_cast<uint64_t *>(vram64(boDpa(b.bo, off)))) : nullptr;
		if (!b.bo->sys) return nullptr;   // userptr: the caller's pages have no kernel mapping here
		flushRange(static_cast<uint8_t *>(b.bo->sys->getBytesNoCopy()) + off, 0x1000 - (off & 0xfff));
		return reinterpret_cast<const uint32_t *>(static_cast<uint8_t *>(b.bo->sys->getBytesNoCopy()) + off);
	}
	return nullptr;
}

static void logDwords(XeVm *vm, uint64_t va, uint32_t max) {
	uint32_t avail = 0;
	const uint32_t *p = vaToCpu(vm, va, &avail);
	if (gExecLen > sizeof(gExecLog) - 2048) return;
	gExecLen += (uint32_t)snprintf(gExecLog + gExecLen, sizeof(gExecLog) - gExecLen, "  @%llx%s:", va, p ? "" : " (unmapped)");
	for (uint32_t i = 0; p && i < max && i < avail && gExecLen < sizeof(gExecLog) - 16; i++)
		gExecLen += (uint32_t)snprintf(gExecLog + gExecLen, sizeof(gExecLog) - gExecLen, "%s%08x", i % 8 ? " " : "\n   ", p[i]);
	gExecLen += (uint32_t)snprintf(gExecLog + gExecLen, sizeof(gExecLog) - gExecLen, "\n");
}

static void dumpBatch(XeVm *vm, uint64_t va, uint16_t cls) {
	if (gExecLen > sizeof(gExecLog) - 2048) return;
	gExecLen += (uint32_t)snprintf(gExecLog + gExecLen, sizeof(gExecLog) - gExecLen, "exec class %u batch %llx\n", cls, va);
	logDwords(vm, va, 64);
	uint32_t avail = 0;
	const uint32_t *p = vaToCpu(vm, va, &avail);
	for (uint32_t i = 0; p && i + 2 < avail && i < 64; i++)   // first MI_BATCH_BUFFER_START (0x31 << 23)
		if ((p[i] >> 23) == 0x31) {
			logDwords(vm, (uint64_t)p[i + 2] << 32 | p[i + 1], 128);
			break;
		}
}
#endif

// ─── the migrate VM (xe_migrate.c): a kernel VM on the copy engine whose page tables map all of VRAM once at bring-up,
// identity-style with 1 GB pages at kIdentVa + DPA (xe_migrate_prepare_vm / xe_migrate_program_identity), so a clear
// or copy is only commands in the copy queue's ring: no per-job mapping, no TLB invalidation. Clears (xe_migrate_clear,
// emit_clear_link_copy): PVC_MEM_SET (Xe2 blitter, matrix form, 4 KB rows, 32 MB per command, uncached MOCS index 3)
// over each block; asynchronous, the BO keeps the job's fence (its kernel fence in Linux), which binds of the BO hand
// on as their out-fence and CPU maps wait for.
static XeVm *gMigVm;
static XeQueue *gMigQ;
static bool gClearOk;
static constexpr uint64_t kIdentVa = 1ull << 40;   // 1 TB, a level-2 table of its own
// The same identity once more with PAT 12, XE_CACHE_NONE_COMPRESSION (xe_migrate_prepare_vm: "Identity map the entire
// vram for compressed pat_index for xe2+ if flat ccs is enabled"). A copy that reads VRAM through it gets the data
// decompressed (xe_migrate_copy use_comp_pat, for VRAM to system memory); writes stay on kIdentVa (uncompressed: the
// lines' CCS state becomes uncompressed, which a later compressed-PAT read takes as plain data).
static constexpr uint64_t kIdentCompVa = kIdentVa + (512ull << 30);   // 1.5 TB, its own level-2 table
static constexpr uint16_t kPatCompUC = 12;
// The address a copy reads VRAM at: decompressing for a BO that may hold compressed data. Linux uses the compressed
// identity for user BOs (ttm_bo_type_device) only; here, for BOs ever bound with a compressed PAT, so kernel and
// display memory (whose CCS state a CPU write may have left stale) are copied as they are.
static uint64_t identRead(bool comp, uint64_t dpa) { return (comp ? kIdentCompVa : kIdentVa) + dpa; }
static constexpr uint16_t kPatUC = 3;              // Xe2 PAT 3: uncached

static IOLock *gMoveLock;   // the migrate VM's kMoveVa window (boMoveSys)

// rb8c: a window of the migrate VM (kMoveVa, kSurfVa, kSurfTiledVa) unmapped after its copy completed: the PTE writes out
// (xe_device_wmb), the next migrate job invalidates in its ring, and the window's page tables go once a ranged
// invalidation issued now has completed, as every unbind's. WAS sealed with the last invalidation already issued
// (gTlbSeq), often complete before the unmap: the copy engine's page-walk caches could still name tables that were
// handed to another VM by then. rb8d: returns that invalidation (0: nothing was unmapped); jobsOf: the tables wait for
// the migrate VM's jobs of this moment too (deferSeal), for a caller whose copies may not all have completed (rb8e: every
// caller passes gMigVm, and the window's pages wait for the invalidation as well: migWindowAwait).
static uint32_t migWindowSeal(BindCtx *ctx, XeVm *jobsOf = nullptr) {
	gDev->deviceWmb();
	OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gMigVm->ptGen));
	const uint32_t seq = ctx->unmapped ? tlbInvalidateRange(gMigVm->asid, ctx->lo, ctx->hi) : 0;
	deferSeal(ctx, seq ? seq : gTlbSeq, jobsOf);
	return seq;
}

// rb8e: the backing of a sealed migrate window (the pages the window mapped and their DMA mapping) goes only once the
// window's invalidation (migWindowSeal) has completed; the copies through it were waited for before the unmap. Linux:
// xe_migrate.c xe_migrate_access_memory waits for the last migrate job's fence (dma_fence_wait) before
// xe_migrate_dma_unmap; a BO's pages go once its dma-resv, which holds the migrate job's fence, is idle (TTM); and the
// window's PTEs are only reached through a TLB that the next migrate job invalidates first (xe_ring_ops.c
// emit_migration_job_gen12: emit_flush_invalidate between the PTE update batch and the copy, the job's
// migrate_flush_flags MI_INVALIDATE_TLB, xe_migrate_vram). Here the window is unmapped after the copy, so the copy
// engine's TLB may still name its pages until the ranged invalidation of the seal completes. WAS: svCopyBo and
// xeVramRead unmapped the DMA and gave the pages back (the caller's IOSurface pages, a kernel buffer) right after
// issuing it, and a failed BO move freed its new pages the same way. Bounded (kTlbWaitNs, past the fence's own
// timeout, after which tlbStallCheck counts it done as Linux's xe_tlb_inval_fence_timeout signals it); false: not
// complete then, and the caller keeps the pages and the mapping (leaked, as for a copy that did not complete). Counted
// per window (debug.intelxe.stats "migrate windows (rb8e)"). seq 0: nothing was unmapped.
enum : uint32_t { kMigWinMove, kMigWinSurf, kMigWinRead, kMigWinN };
static struct { uint32_t sealed[kMigWinN], awaited, kept[kMigWinN], maxUs; uint64_t us; } gMigWinSt;
static bool migWindowAwait(uint32_t seq, uint32_t kind) {
	gMigWinSt.sealed[kind]++;
	if (!seq) return true;
	const uint64_t t0 = nowNs();
	const bool done = tlbSeqDone(seq) || tlbWait(nullptr, seq);
	const uint64_t us = (nowNs() - t0) / 1000;
	gMigWinSt.awaited++;
	gMigWinSt.us += us;
	if (us > gMigWinSt.maxUs) gMigWinSt.maxUs = (uint32_t)(us > 0xffffffffull ? 0xffffffffull : us);
	if (!done) {
		gMigWinSt.kept[kind]++;
		g2hLog("migrate window %u: invalidation %u not complete after %llu us: its pages are kept\n", kind, seq, us);
	}
	return done;
}
static int migrateInit() {
	if (!gMoveLock) gMoveLock = IOLockAlloc();
	if (gMigVm) return 0;
	XeVm *vm = vmCreate(0xffff1);
	if (!vm) return ENOMEM;
	const uint64_t tile = (uint64_t)(gDev->mmio(SG_TILE_ADDR_RANGE0) >> 8 & 0x3ff) << 30;
	// a BO standing for all of VRAM: one 1 GB-aligned physical run, so ptRange writes 1 GB leaves (XE_PDPE_PS_1G)
	VBlock all = {};
	all.order = kVMaxOrder;
	XeBo id = {};
	id.vram = true;
	id.blocks = &all;
	id.size = tile;
	int err = tile ? 0 : ENODEV;
	BindCtx ctx = {};
	for (uint64_t gb = 0; !err && gb < (tile >> 30); gb++) {
		all.off = gb << 30;
		err = vmMap(vm, &id, 0, 1ull << 30, kIdentVa + (gb << 30), kPatUC, 0, &ctx, true);
	}
	for (uint64_t gb = 0; !err && gb < (tile >> 30); gb++) {
		all.off = gb << 30;
		err = vmMap(vm, &id, 0, 1ull << 30, kIdentCompVa + (gb << 30), kPatCompUC, 0, &ctx, true);
	}
	if (err) {
		vmPut(vm);
		return err;
	}
	asm volatile("sfence" : : : "memory");
	vm->ptGen++;
	gMigVm = vm;
	return 0;
}

// Clear the BO's VRAM; bo->clear is the fence of the (last) job. Commands are split into jobs of at most 1536 dwords.
static int vramClear(XeBo *bo) {
	if (!gMigQ) return ENXIO;
	static constexpr uint32_t kMax = 1536;
	auto *cmd = static_cast<uint32_t *>(IOMalloc(kMax * 4));
	if (!cmd) return ENOMEM;
	int err = 0;
	uint32_t n = 0;
	for (VBlock *b = bo->blocks; b && !err; b = b->link) {
		for (uint64_t off = 0; off < vblockSize(b) && !err;) {
			const uint64_t chunk = vblockSize(b) - off > (32ull << 20) ? (32ull << 20) : vblockSize(b) - off;
			const uint64_t va = kIdentVa + b->off + off;
			cmd[n++] = (2u << 29 | 0x5bu << 22) | 1u << 17 | (7 - 2);   // PVC_MEM_SET_CMD | MATRIX | len
			cmd[n++] = 0x1000 - 1;                                     // width (bytes) - 1
			cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;                 // rows - 1
			cmd[n++] = 0x1000 - 1;                                     // pitch - 1
			cmd[n++] = (uint32_t)va;
			cmd[n++] = (uint32_t)(va >> 32);
			cmd[n++] = (uint32_t)gXePlat->mocsUcIndex << 3;           // XE2_MEM_SET_MOCS_INDEX: uc_index
			off += chunk;
			if (n + 7 > kMax || (!b->link && off >= vblockSize(b))) {
				err = queueSubmitKernel(gMigQ, 0, &bo->clear, cmd, n);
				n = 0;
			}
		}
	}
	IOFree(cmd, kMax * 4);
	if (!err) gStats.clears++;
	return err;
}

// The copy queue on the migrate VM, then a self-test inside the BAR window: CPU fill, GPU clear, CPU check. Until it
// passes, nothing is placed outside the window and visible BOs are cleared by the CPU.
static void vramClearInit() {
	if (gClearOk) return;
	if (migrateInit()) return;
	if (!gMigQ && queueCreate(gMigVm, DRM_XE_ENGINE_CLASS_COPY, 0, &gMigQ)) { gMigQ = nullptr; return; }
	XeBo t = {};
	t.refs = 1;
	if (!vramAllocBo(&t, 0x20000, true)) return;
	volatile uint64_t *p = vram64(t.dpa);
	for (uint32_t i = 0; i < 0x20000 / 8; i++) p[i] = 0xaaaaaaaaaaaaaaaaull;
	int err = vramClear(&t);
	if (!err) err = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(t.clear); });
	uint32_t bad = 0;
	for (uint32_t i = 0; i < 0x20000 / 8; i++) if (p[i]) bad++;
	vramFree(&t);
	gClearOk = !err && !bad && t.contig;
	IOLockLock(gResLock);
	gHiOn = gClearOk;
	IOLockUnlock(gResLock);
	say("vram clear self-test (migrate VM, 1 GB identity pages): err %d, %u of %u qwords left non-zero -> %s\n", err, bad,
		0x20000 / 8, gClearOk ? "BOs beyond the BAR window ON, clears on the copy engine" : "window only, CPU clears");
}

// ─── BO moves (xe_bo_migrate / ttm_bo_validate + xe_migrate_copy, TTM eviction): BOs the CPU never maps live outside
// the BAR window; one that the CPU then maps moves into it, and when the window is full, BOs in it that the CPU never
// mapped move out to make room. Rules:
//  - a BO the CPU was given a mapping of (sticky XeBo::mapped: an IOKit user mapping cannot be revoked) or that the
//    display scans out (pins) never moves;
//  - it moves only when idle: every queue on the VMs it is bound in has completed everything it was handed (the VM's
//    dma-resv fences in xe); while a move of its BOs is in progress the client's EXEC and VM_BIND wait (the VM
//    reservation xe takes, held across the wait with the client lock dropped);
//  - the copy is the migrate queue's MEM_COPY through the identity map (emit_mem_copy, page mode, 8 MB per command:
//    MAX_PREEMPTDISABLE_TRANSFER), so it runs after the BO's clear on the same queue;
//  - then the block list is swapped, the PTEs of every binding rewritten (ptGen moves, so each queue's next batch
//    invalidates in the ring), a ranged GuC invalidation issued per VM, and the old blocks freed after it completes.
static VBlock *vramBlocksAlloc(uint64_t size, bool hi, bool *contig) {
	VBlock *bl = nullptr;
	IOLockLock(gResLock);
	if (hi) bl = gHiOn ? buddyAlloc(&gHi, size, true, false) : nullptr;
	else if (!(bl = visAlloc(size, false, true, kWinReserve))) bl = visAlloc(size, false, false, kWinReserve);
	uint64_t total = 0;
	bool ct = true;
	for (VBlock *b = bl; b; b = b->link) {
		if (b->link && b->off + vblockSize(b) != b->link->off) ct = false;
		total += vblockSize(b);
	}
	if (bl) {
		gVramUsed += total;
		if (!hi) gVisibleUsed += total;
	}
	IOLockUnlock(gResLock);
	*contig = ct;
	return bl;
}

static void vramBlocksFree(VBlock *bl, bool hi) {
	uint64_t total = 0;
	IOLockLock(gResLock);
	for (VBlock *b = bl; b; b = b->link) total += vblockSize(b);
	buddyFree(hi ? &gHi : &gVis, bl);
	gVramUsed -= total;
	if (!hi) gVisibleUsed -= total;
	IOLockUnlock(gResLock);
}

static void vramListFree(VramList *l) {
	vramBlocksFree(l->blocks, l->hi);
	IOFree(l, sizeof(*l));
}

// MEM_COPY of `size` bytes (64 KB granular, as the blocks are) from the blocks of src to those of dst. *out: the fence
// of the last job.
static int vramCopy(VBlock *src, VBlock *dst, uint64_t size, XeFence *out, bool comp = false) {
	if (!gMigQ) return ENXIO;
	static constexpr uint32_t kMax = 1500, kChunk = 8u << 20;
	auto *cmd = static_cast<uint32_t *>(IOMalloc(kMax * 4));
	if (!cmd) return ENOMEM;
	int err = 0;
	uint32_t n = 0;
	uint64_t left = (size + kVChunk - 1) & ~(kVChunk - 1), so = 0, dof = 0;
	for (VBlock *s = src, *d = dst; left && s && d && !err;) {
		uint64_t chunk = vblockSize(s) - so;
		if (vblockSize(d) - dof < chunk) chunk = vblockSize(d) - dof;
		if (chunk > kChunk) chunk = kChunk;
		if (chunk > left) chunk = left;
		// a compressed BO's data comes out decompressed and lands uncompressed (is_vram_resolve), so the moved BO reads
		// the same through any PAT: a raw copy would carry the bytes without their CCS state
		const uint64_t sva = identRead(comp, s->off + so), dva = kIdentVa + d->off + dof;
		cmd[n++] = (2u << 29 | 0x5au << 22) | 1u << 19 | (10 - 2);   // MEM_COPY_CMD | MEM_COPY_PAGE_COPY_MODE, linear
		cmd[n++] = (uint32_t)(chunk / 256) - 1;                         // width: 256-byte pages - 1
		cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;                      // ignored in page mode
		cmd[n++] = 0x1000 - 1;
		cmd[n++] = 0x1000 - 1;
		cmd[n++] = (uint32_t)sva;
		cmd[n++] = (uint32_t)(sva >> 32);
		cmd[n++] = (uint32_t)dva;
		cmd[n++] = (uint32_t)(dva >> 32);
		cmd[n++] = (uint32_t)gXePlat->mocsUcIndex << 28 | (uint32_t)gXePlat->mocsUcIndex << 3;   // src, dst MOCS: uc_index
		left -= chunk;
		if ((so += chunk) == vblockSize(s)) s = s->link, so = 0;
		if ((dof += chunk) == vblockSize(d)) d = d->link, dof = 0;
		if (n + 10 > kMax || !left) {
			err = queueSubmitKernel(gMigQ, 0, out, cmd, n);
			n = 0;
		}
	}
	IOFree(cmd, kMax * 4);
	if (!err && left) err = EINVAL;   // a block list shorter than size
	return err;
}

// Every queue on vm has completed every job it was handed.
static bool vmIdle(const XeVm *vm) {
	bool idle = true;
	IOLockLock(gSchedLock);
	for (uint32_t g = 0; g < 2 && idle; g++)
		for (uint32_t i = 0; i < kMaxGucIds; i++) {
			const XeQueue *q = gQueueByGuc[g][i];
			if (q && q->vm == vm && !q->banned && (int32_t)(seqnoOf(q->slot) - q->seqno) < 0) {
				idle = false;
				break;
			}
		}
	IOLockUnlock(gSchedLock);
	return idle;
}

// The client's VMs that hold a binding of bo, appended to out[*n] (at most max in all; *n keeps counting past max so the
// caller sees the overflow); *recs += the binding records of bo in them. c->lock held.
static void boVmsAdd(XeClientState *c, const XeBo *bo, XeVm **out, uint32_t max, uint32_t *n, uint32_t *recs) {
	for (uint32_t h = 1; h <= c->vms.n; h++) {
		auto *vm = static_cast<XeVm *>(c->vms.get(h));
		uint32_t k0 = 0;
		for (uint32_t k = 0; vm && k < vm->nBinds; k++) k0 += vm->binds[k].bo == bo;
		if (!k0) continue;
		*recs += k0;
		if (*n < max) out[*n] = vm;
		(*n)++;
	}
}
// The client's VMs that hold a binding of bo (at most max; returns the count, max + 1 if there are more).
static uint32_t boVms(XeClientState *c, const XeBo *bo, XeVm **out, uint32_t max) {
	uint32_t n = 0, recs = 0;
	boVmsAdd(c, bo, out, max, &n, &recs);
	return n > max ? max + 1 : n;
}

// ─── rb7s: the VMs a BO move rewrites (xe_bo_move -> xe_bo_move_notify -> xe_bo_trigger_rebind: every vm_bo of the
// object, whichever file's VM it is) ────────────────────────────────────────────────────────────────────────────────
// WAS: c's VMs only (boVms(c, bo)): a BO bound in another client's VM kept PTEs there that pointed at the blocks the
// move freed. The set is c's VMs and, for a shared BO (a handle went to another client), the VMs of every client that
// binds it. Those clients' locks are held for the whole move: their page tables change only under them (VM_BIND,
// purges, their own moves), and their EXEC and VM_BIND wait meanwhile, as xe's do behind the BO's dma-resv. The move
// then keeps its own lock too (waits without dropping it): xeSuspend takes every client lock in turn, and a mover that
// held another client's lock while waiting for its own back could deadlock with it. Taking another client's lock while
// holding one is a try-lock (lock order); a busy one answers EAGAIN, which boMakeVisible retries.
// A binding of the BO that no client's VM table reaches (a destroyed VM whose queues are still being torn down) cannot
// be rewritten: the move is refused (EAGAIN) until that VM is freed, which bo->vmRecs tells (before rb7s such a VM kept
// PTEs on the freed blocks).
struct MoveSet {
	XeVm *vms[8];
	uint32_t nv;
	XeClientState *locked[8];   // other clients whose lock the move holds (their VMs bind the BO)
	uint32_t nl;
	bool shared;                // counted in gSharedMoving
};
// moves of shared BOs in progress: VM_BIND looks for a MAP of a migrating BO only while there is one (it reads the ops
// once more for that), so a bind costs nothing more when no shared BO moves
static volatile SInt32 gSharedMoving;
// what the mover holds besides its own client's lock: another client's (evictWindow's caller while it evicts another
// client's BOs) and gClientsLock (evictWindow's other clients, debug.intelxe.evict)
struct MoveHeld { XeClientState *other; bool clients; };
static struct { uint32_t moves, foreignVms, lockBusy, unaccounted, opens, flinks, names, mapWaits, bindWaits; } gShareSt;

// Starts a move of bo for client c (its lock held): marks the BO migrating and collects the VMs to rewrite (each held,
// vmGet). unmapped: the move needs a BO the CPU has no mapping of (out of the window, to system memory). 0, or EAGAIN
// (a lock was busy, or bindings in a VM no client reaches yet), EBUSY (mapped meanwhile, more than 8 VMs or other
// clients): then nothing is held and the BO is not migrating.
static int moveSetBegin(XeClientState *c, XeBo *bo, MoveSet *ms, const MoveHeld &held, bool unmapped) {
	ms->nv = ms->nl = 0;
	ms->shared = false;
	bo->migrating = true;
	// Dekker pairs: boHandleGet stores shared and then reads migrating, the CPU-map paths store mapped and then read
	// migrating; here migrating is stored and then shared and mapped are read. A handle given to another client, or a
	// CPU map made, while this move starts is either seen below or waits for the move to end.
	__sync_synchronize();
	uint32_t n = 0, recs = 0;
	int err = unmapped && (bo->mapped || bo->cpuMaps) ? EBUSY : 0;
	if (!err) boVmsAdd(c, bo, ms->vms, 8, &n, &recs);
	if (!err && bo->shared) {
		ms->shared = true;
		OSIncrementAtomic(&gSharedMoving);   // before any other client's lock is taken (and released) below
		if (held.other && held.other != c) boVmsAdd(held.other, bo, ms->vms, 8, &n, &recs);
		const bool took = !held.clients && IOLockTryLock(gClientsLock);
		if (!held.clients && !took) err = EAGAIN;
		for (XeClientState *o = err ? nullptr : gClients; o && !err; o = o->next) {
			if (o == c || o == held.other) continue;
			if (!IOLockTryLock(o->lock)) {
				err = EAGAIN;
				break;
			}
			const uint32_t before = n;
			boVmsAdd(o, bo, ms->vms, 8, &n, &recs);
			if (n == before) IOLockUnlock(o->lock);   // binds nothing of it: it saw `migrating` (a bind of it waits)
			else if (ms->nl < 8) ms->locked[ms->nl++] = o;
			else {
				IOLockUnlock(o->lock);
				err = EBUSY;
			}
		}
		if (took) IOLockUnlock(gClientsLock);
	}
	if (!err && n > 8) err = EBUSY;
	bool unreached = false;
	if (!err && (SInt32)recs != bo->vmRecs) {   // a binding in a VM no client table reaches: EAGAIN, as such a VM is
		err = EAGAIN;                           // normally freed within moments (its queues' teardown, an invalidation)
		unreached = true;
		gShareSt.unaccounted++;
	}
	if (err) {
		for (uint32_t i = 0; i < ms->nl; i++) IOLockUnlock(ms->locked[i]->lock);
		ms->nl = ms->nv = 0;
		bo->migrating = false;
		if (ms->shared) OSDecrementAtomic(&gSharedMoving);
		ms->shared = false;
		if (err == EAGAIN && !unreached) gShareSt.lockBusy++;
		wakeAll();   // a map or bind of it that saw `migrating` is waiting for it to clear
		return err;
	}
	ms->nv = n;
	for (uint32_t v = 0; v < n; v++) vmGet(ms->vms[v]);
	if (bo->shared) {   // the VMs of other clients, for debug.intelxe.vram
		uint32_t foreign = 0;
		for (uint32_t v = 0; v < n; v++) {
			bool own = false;
			for (uint32_t h = 1; h <= c->vms.n && !own; h++) own = c->vms.get(h) == ms->vms[v];
			foreign += !own;
		}
		if (foreign) {
			gShareSt.moves++;
			gShareSt.foreignVms += foreign;
		}
	}
	return 0;
}

// Ends it: the VM references and the other clients' locks go, the BO stops migrating, its waiters wake.
static void moveSetEnd(XeBo *bo, MoveSet *ms) {
	for (uint32_t v = 0; v < ms->nv; v++) vmPut(ms->vms[v]);
	bo->migrating = false;
	if (ms->shared) OSDecrementAtomic(&gSharedMoving);
	for (uint32_t i = 0; i < ms->nl; i++) IOLockUnlock(ms->locked[i]->lock);
	ms->nv = ms->nl = 0;
	ms->shared = false;
	wakeAll();   // EXEC / VM_BIND / map waiters
}

// A move that met another client's lock busy (EAGAIN) is tried again after 1 ms with c's lock dropped, until `until`
// (a CPU fault on a BO whose dma-resv another process holds waits for it in xe). false: give up.
static bool moveRetry(XeClientState *c, int err, uint64_t until) {
	if (err != EAGAIN || nowNs() >= until) return false;
	IOLockUnlock(c->lock);
	IOSleep(1);
	IOLockLock(c->lock);
	return !c->dying;
}

// Rewrite the PTEs of every binding of bo in vms after its blocks changed. Returns ENOMEM if a page-table page could
// not be allocated (some entries may then still point at the old blocks: the caller moves the BO back).
static int boRemap(XeVm **vms, uint32_t nv, XeBo *bo) {
	int err = 0;
	for (uint32_t v = 0; v < nv; v++) {
		XeVm *vm = vms[v];
		BindCtx ctx = {};
		ctx.clear = {0, ~0u, 0, 0};
		for (uint32_t k = 0; k < vm->nBinds; k++) {
			const XeBinding &b = vm->binds[k];
			if (b.bo == bo && vmMap(vm, bo, b.off, b.range, b.addr, b.pat, b.flags, &ctx)) err = ENOMEM;
		}
		gDev->deviceWmb();   // rb8c: xe_device_wmb, as xe_migrate_update_pgtables_cpu after its PTE writes
		OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&vm->ptGen));
		vm->remapNs = nowNs();
		if (ctx.unmapped) deferSeal(&ctx, tlbInvalidateRange(vm->asid, ctx.lo, ctx.hi), vm);
	}
	return err;
}

static bool evictWindow(XeClientState *self, uint64_t need);

// Move bo into the BAR window (toWindow) or out of it. c: the client moving it (it has a handle), whose lock is held.
// mayWait: wait for the BO's VMs to go idle (dropping the client lock unless other clients' locks are held; the
// client's EXEC and VM_BIND wait meanwhile); otherwise EBUSY if they are not idle now (eviction). held: see MoveHeld.
static int boMove(XeClientState *c, XeBo *bo, bool toWindow, bool mayWait, const MoveHeld &held = {nullptr, false}) {
	if (!bo->vram || bo->hi != toWindow) return 0;   // nothing to do: already on that side
	if (bo->pins || bo->scanout || bo->migrating || bo->noMove || (!toWindow && (bo->mapped || bo->cpuMaps))) return EBUSY;
	bool contig;
	VBlock *nb = vramBlocksAlloc(bo->size, !toWindow, &contig);
	if (!nb && toWindow && evictWindow(c, bo->size)) nb = vramBlocksAlloc(bo->size, false, &contig);
	if (!nb) {
		gStats.moveFails++;
		return ENOSPC;
	}
	// rb7s: every VM that binds it, other clients' included (moveSetBegin); bound in more VMs than tracked here, or
	// where no client reaches: it stays where it is
	MoveSet ms;
	if (int e = moveSetBegin(c, bo, &ms, held, !toWindow)) {
		vramBlocksFree(nb, !toWindow);
		return e;
	}
	XeVm **const vms = ms.vms;
	const uint32_t nv = ms.nv;
	c->migrating++;
	boGet(bo);
	auto idle = [&] {
		for (uint32_t v = 0; v < nv; v++)
			if (!vmIdle(vms[v])) return false;
		return true;
	};
	// with other clients' locks held, c's is kept as well while it waits (see MoveSet)
	int err = mayWait ? waitUntil(ms.nl ? nullptr : c, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, idle) : idle() ? 0 : EBUSY;
	bool copied = false;
	XeFence f = {0, ~0u, 0, 0};
	if (!err && !(err = vramCopy(bo->blocks, nb, bo->size, &f, bo->compBound))) {
		// a kernel wait (the lock stays held): the copy is bounded, and nothing may touch the BO until the swap
		err = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitMigrate, [&] { return fenceSignaled(f); });
		copied = !err;
		if (err) {
			g2hLog("BO move: copy of %llu KB did not complete within 2 s; destination leaked\n", bo->size >> 10);
			nb = nullptr;
			f = {0, ~0u, 0, 0};
		}
	}
	if (copied) {
		VBlock *old = bo->blocks;
		const bool oldHi = bo->hi, oldContig = bo->contig;
		bo->blocks = nb;
		bo->dpa = nb->off;
		bo->contig = contig;
		bo->hi = !toWindow;
		bo->clear = {0, ~0u, 0, 0};
		if (boRemap(vms, nv, bo)) {   // back to where it was (its old layout's tables are mostly still there)
			bo->blocks = old;
			bo->dpa = old->off;
			bo->contig = oldContig;
			bo->hi = oldHi;
			boRemap(vms, nv, bo);
			old = nb;
			err = ENOMEM;
		}
		// The old blocks go once every invalidation issued so far has completed: those of this remap, and those of
		// unbinds of the BO elsewhere whose TLB entries may still point at them.
		auto *l = static_cast<VramList *>(IOMalloc(sizeof(VramList)));
		if (l) {
			*l = {old, old == nb ? !toWindow : oldHi};
			if (!deferAdd(l, kDefVram, gTlbSeq, nullptr)) {
				tlbWait(nullptr, gTlbSeq);
				vramListFree(l);
			}
		} else {
			tlbWait(nullptr, gTlbSeq);
			vramBlocksFree(old, old == nb ? !toWindow : oldHi);
		}
		if (!err) {
			(toWindow ? gStats.movesIn : gStats.movesOut)++;
			gStats.movedBytes += bo->size;
			lastMoveSet(toWindow ? 'i' : 'o');
		}
	} else if (!nb) {
	} else if (f.slot == ~0u ||
		!waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitMigrate, [&] { return fenceSignaled(f); })) {
		vramBlocksFree(nb, !toWindow);   // no copy into it is running (any part that was submitted has completed)
	}
	if (err && err != EBUSY) gStats.moveFails++;
	if (err == EBUSY) gStats.moveBusy++;
	c->migrating--;
	moveSetEnd(bo, &ms);   // the VMs, the other clients' locks; wakes the EXEC / VM_BIND / map waiters
	boPut(bo);
	return err;
}

// The CPU is about to map bo: it must be in the BAR window. Waits for a move already in progress.
static int boMakeVisible(XeClientState *c, XeBo *bo) {
	boGet(bo);
	int err = 0;
	const uint64_t until = nowNs() + 5000000000ull;
	while (bo->vram && bo->hi && !err) {
		if (bo->migrating) err = waitUntil(c, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
		else if (bo->pins) err = EBUSY;
		else if ((err = boMove(c, bo, true, true)) && moveRetry(c, err, until)) err = 0;   // rb7s: another client's lock was busy
	}
	boPut(bo);
	return err;
}

// ─── VRAM -> system memory for a CPU map the window cannot take (xe_bo_fault_migrate / xe_bo_migrate to XE_PL_TT) ──
// The CPU must map a BO that lives beyond the BAR window and the window has no room: every BO in it is CPU-mapped and
// cannot move (see XeCpuMap), so evicting frees nothing. xe would evict mapped BOs too (to TT; their mmaps fault back
// in), so there the map always succeeds; here GEM_MMAP_OFFSET returned ENOSPC and the app's vkMapMemory failed
// (bomove evict: 16 x 64 MB BOs mapped one after another against the ~960 MB the window has for them; 3 maps refused,
// 2026-10-29 - no contents were lost, the maps were). Instead the BO moves to system memory, where xe also puts a BO
// that may not stay in VRAM (xe_evict_flags -> tt_placement; a dGPU reads system memory coherently over PCIe), and the
// CPU maps its pages. It stays there, as GEM_CREATE's spill does.
// The copy is the migrate queue's MEM_COPY from the VRAM identity map to the new pages, mapped for the duration at
// kMoveVa in the migrate VM (as the suspend backups are at kSysVa); the rest follows boMove: the BO's VMs idle with the
// client's EXEC/VM_BIND held (c->migrating), bindings remapped, the old blocks freed after the invalidation.
static constexpr uint64_t kMoveVa = 3ull << 40;   // 3 TB: the migrate VM's window onto a BO's new system pages

// A copy in a migrate-VM window (kMoveVa, kSurfVa) whose job did not complete in time may still run, until the kernel
// job watchdog resets the GT. Linux never frees memory a migrate job may still touch (the job's fence sits in the BOs'
// dma-resv); here the memory it reads and writes is kept (leaked: the BO and the pages), its window stays mapped, and
// the next copy through a window first waits for that job (EBUSY if it still runs).
static XeFence gMigLate = {0, ~0u, 0, 0};
static bool migLateDone() {
	return fenceSignaled(gMigLate) ||
		!waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitMigrate, [] { return fenceSignaled(gMigLate); });
}

// *keep (rb8e): the copy may still run or the window's invalidation did not complete: the caller must not unmap or free
// sys (it is the BO's backing on success anyway)
static int vramCopyToSys(XeBo *bo, IOBufferMemoryDescriptor *sys, const DmaMap &dma, bool *keep) {
	IOLockLock(gMoveLock);
	if (!migLateDone()) {
		IOLockUnlock(gMoveLock);
		return EBUSY;
	}
	BindCtx ctx = {};
	ctx.clear = {0, ~0u, 0, 0};
	XeBo tmp = {};
	tmp.size = bo->size;
	tmp.sys = sys;
	tmp.dma = dma;   // borrowed: the caller keeps the mapping
	int err = vmMap(gMigVm, &tmp, 0, bo->size, kMoveVa, 2, 0, &ctx, true);   // PAT 2: WB, coherent (XE_CACHE_WB)
	asm volatile("sfence" : : : "memory");
	OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gMigVm->ptGen));   // the next migrate job invalidates its TLB
	static constexpr uint32_t kMax = 1500, kChunk = 8u << 20;
	auto *cmd = err ? nullptr : static_cast<uint32_t *>(IOMalloc(kMax * 4));
	if (!err && !cmd) err = ENOMEM;
	XeFence last = {0, ~0u, 0, 0};
	uint32_t n = 0;
	uint64_t off = 0;
	for (VBlock *b = bo->blocks; b && !err && off < bo->size; b = b->link)
		for (uint64_t o = 0; o < vblockSize(b) && off < bo->size && !err;) {
			uint64_t chunk = vblockSize(b) - o;
			if (chunk > kChunk) chunk = kChunk;
			if (chunk > bo->size - off) chunk = bo->size - off;
			const uint64_t src = identRead(bo->compBound, b->off + o), dst = kMoveVa + off;   // decompressed (use_comp_pat)
			cmd[n++] = (2u << 29 | 0x5au << 22) | 1u << 19 | (10 - 2);   // MEM_COPY, page mode (as vramCopy, pmCopyAll)
			cmd[n++] = (uint32_t)(chunk / 256) - 1;
			cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;
			cmd[n++] = 0x1000 - 1;
			cmd[n++] = 0x1000 - 1;
			cmd[n++] = (uint32_t)src;
			cmd[n++] = (uint32_t)(src >> 32);
			cmd[n++] = (uint32_t)dst;
			cmd[n++] = (uint32_t)(dst >> 32);
			cmd[n++] = 3u << 28 | 3u << 3;
			o += chunk;
			off += chunk;
			if (n + 10 > kMax) {
				err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
				n = 0;
			}
		}
	if (!err && off < bo->size) err = EINVAL;   // a block list shorter than the BO
	if (!err && n) err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
	if (cmd) IOFree(cmd, kMax * 4);
	// waited for even after a failed submit: nothing may still write the pages when they are unmapped (or freed)
	if (last.slot != ~0u &&
		waitUntil(nullptr, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, [&] { return fenceSignaled(last); })) {
		g2hLog("BO move to system memory: copy of %llu KB did not complete within 5 s: its memory is kept\n",
			bo->size >> 10);
		*keep = true;    // never released: the job may still write these pages (rb8e: nor their DMA mapping unmapped;
		                 // WAS sys->retain() here and the caller's dma.unmap() all the same)
		boGet(bo);       // nor is the VRAM it reads freed
		gMigLate = last;
		IOLockUnlock(gMoveLock);
		return ETIME;    // kMoveVa stays mapped to the kept pages
	}
	vmUnmap(gMigVm, kMoveVa, bo->size, &ctx);
	// rb8e: sealed with the migrate queue's jobs too (as pmCopyAll), and awaited before a failed move lets the pages go
	if (!migWindowAwait(migWindowSeal(&ctx, gMigVm), kMigWinMove)) *keep = true;
	IOLockUnlock(gMoveLock);
	return err;
}

// Move bo (VRAM, never CPU-mapped) to system memory. c: its client, whose lock is held (dropped while its VMs go idle).
static int boMoveSys(XeClientState *c, XeBo *bo) {
	if (!bo->vram) return 0;
	if (bo->pins || bo->scanout || bo->migrating || bo->noMove || bo->mapped || bo->cpuMaps) return EBUSY;
	if (!gMigQ || !gMigVm || !gMoveLock) return ENXIO;
	IOBufferMemoryDescriptor *sys = gDev->allocSys(bo->size, false);
	if (!sys) return ENOMEM;
	DmaMap dma;
	if (!dma.map(sys)) {   // xe_tt_map_sg
		dma.unmap();
		sys->release();
		return ENOMEM;
	}
	MoveSet ms;   // rb7s: every VM that binds it (see boMove)
	if (int e = moveSetBegin(c, bo, &ms, {nullptr, false}, true)) {
		dma.unmap();
		sys->release();
		return e;
	}
	XeVm **const vms = ms.vms;
	const uint32_t nv = ms.nv;
	c->migrating++;
	boGet(bo);
	int err = waitUntil(ms.nl ? nullptr : c, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, [&] {
		for (uint32_t v = 0; v < nv; v++)
			if (!vmIdle(vms[v])) return false;
		return true;
	});
	// its fresh-VRAM clear first: the copy must read the cleared contents (the same queue orders it, waited all the same)
	if (!err && !fenceSignaled(bo->clear)) {
		const XeFence clr = bo->clear;
		err = waitUntil(nullptr, (int64_t)(nowNs() + 5000000000ull), kWaitClear, [&] { return fenceSignaled(clr); });
	}
	bool keepSys = false;   // rb8e: vramCopyToSys could not prove the copy engine is done with sys
	if (!err) err = vramCopyToSys(bo, sys, dma, &keepSys);
	if (!err) {
		VBlock *old = bo->blocks;
		const bool oldHi = bo->hi, oldContig = bo->contig;
		const uint64_t oldDpa = bo->dpa;
		bo->vram = false;
		bo->hi = false;
		bo->blocks = nullptr;
		bo->dpa = 0;
		bo->contig = false;
		bo->sys = sys;
		bo->dma = dma;
		bo->clear = {0, ~0u, 0, 0};
		if (boRemap(vms, nv, bo)) {   // back to its VRAM (its old layout's tables are mostly still there)
			bo->vram = true;
			bo->hi = oldHi;
			bo->blocks = old;
			bo->dpa = oldDpa;
			bo->contig = oldContig;
			bo->sys = nullptr;
			memset(static_cast<void *>(&bo->dma), 0, sizeof(bo->dma));
			boRemap(vms, nv, bo);
			err = ENOMEM;
		} else {
			memset(static_cast<void *>(&dma), 0, sizeof(dma));   // the BO owns the mapping and the pages now
			sys = nullptr;
			bo->sysRefs = (uint32_t)bo->sys->getRetainCount();   // rb7r: no CPU mapping of the new pages yet
			OSAddAtomic64((SInt64)bo->size, const_cast<SInt64 *>(&gSysUsed));
			// the old blocks go once every invalidation issued so far has completed (this remap's, and earlier unbinds')
			auto *l = static_cast<VramList *>(IOMalloc(sizeof(VramList)));
			if (l) {
				*l = {old, oldHi};
				if (!deferAdd(l, kDefVram, gTlbSeq, nullptr)) {
					tlbWait(nullptr, gTlbSeq);
					vramListFree(l);
				}
			} else {
				tlbWait(nullptr, gTlbSeq);
				vramBlocksFree(old, oldHi);
			}
			gStats.movedBytes += bo->size;
		}
	}
	if (sys && !keepSys) {   // not moved: the new pages go
		dma.unmap();
		sys->release();
	}
	if (err) gStats.mapSpillFails++;
	else {
		gStats.mapSpills++;
		lastMoveSet('s');
	}
	c->migrating--;
	moveSetEnd(bo, &ms);
	boPut(bo);
	return err;
}

// ─── rb7r: purging DONTNEED BOs (xe_bo.c xe_ttm_bo_purge, xe_bo_move's evict path, xe_shrinker.c) ──────────────────
// A BO user space marked DONTNEED through every binding it has (DRM_IOCTL_XE_MADVISE, DRM_XE_VMA_ATTR_PURGEABLE_STATE)
// loses its backing when memory is short, instead of being moved: in xe, TTM eviction of a DONTNEED BO purges it
// (xe_bo_move -> xe_ttm_bo_purge) and the shrinker purges DONTNEED system BOs first. Here that happens where this driver
// runs short: a GEM_CREATE whose VRAM (or BAR window) allocation fails, the BAR-window eviction (evictWindow), a failed
// system-memory allocation, and debug.intelxe.purge. The state becomes PURGED for good.
// The GPU mappings go first (xe_bo_move_notify -> rebind): every binding's PTEs become its VM's empty entry, which is a
// NULL (scratch) PTE reading zeros in a VM created with SCRATCH_PAGE and a non-present entry (an access faults) in one
// without - what xe writes for a purged BO (xe_pt.c xe_pt_stage_bind: XE_PTE_NULL on scratch VMs, clear_pt otherwise).
// The records stay, as the VMAs do in xe. The VRAM or pages are freed only after the TLB invalidation of those PTEs.
// Memory safety, where this differs from xe:
//  - a CPU mapping cannot be revoked here (IOKit user mappings; xe zaps them and later faults SIGBUS), so a BO with a
//    mapping still there is not purged (it keeps its backing until the mapping goes);
//  - the BO must be idle: every VM it is bound in has completed all its work (xe waits for the BO's fences, or skips
//    it in no-wait contexts), its clear done, and every reference accounted for: its handle and its bindings in VMs
//    with a handle. Bindings in a destroyed VM whose queues may still run, an unbind's deferred free, a scan-out pin or
//    a dead client's lingering map hold it, so it is not purged until they are gone.
static struct {
	uint32_t calls, toDontneed, toWillneed, retainedZero, bindBusy, bindPurged, mmapRefused;
	uint32_t purged, purgedVram, purgedSys, notIdle, cpuMapped, held, failed, passes;
	uint64_t purgedBytes;
} gPurgeSt;
static uint32_t purgedCount() { return gPurgeSt.purged; }
static volatile SInt32 gPurgeScan;
static uint32_t boMapsPrune(XeClientState *c, XeBo *bo, bool closing);

// 0 if DONTNEED bo can be purged now (vms/nv: the VMs it is bound in), else why not.
static int purgeBlocked(XeClientState *c, XeBo *bo, XeVm **vms, uint32_t *nv) {
	if (!bo->purgeTrack || bo->purgeState != kMadvDontneed || (!bo->vram && !bo->sys)) return EINVAL;
	if (bo->pins || bo->migrating || bo->noMove || !fenceSignaled(bo->clear)) return EBUSY;
	boMapsPrune(c, bo, false);   // drops the maps whose mappings are gone
	bool live = false;           // rb7s: any client's map with its mapping still there (c's left are all live)
	IOSimpleLockLock(gBoLock);
	for (XeCpuMap *m = bo->cpuMaps; m && !live; m = m->next) live = m->used && m->md->getRetainCount() > 1;
	IOSimpleLockUnlock(gBoLock);
	if (live) return EPERM;
	if (bo->sys && (uint32_t)bo->sys->getRetainCount() > bo->sysRefs) return EPERM;   // a system BO's mapping retains it
	uint32_t recs = 0;
	for (uint32_t h = 1; h <= c->vms.n; h++)
		if (auto *vm = static_cast<XeVm *>(c->vms.get(h)))
			for (uint32_t k = 0; k < vm->nBinds; k++) recs += vm->binds[k].bo == bo;
	// every reference accounted for: the handles (every client's: rb7s, WAS 1 unless `closed`) and c's bindings; one held
	// by another client's binding, a map, a pin or a deferred free keeps it
	if ((uint32_t)bo->refs != (uint32_t)bo->handles + recs) return EAGAIN;
	if ((*nv = boVms(c, bo, vms, 8)) > 8) return EAGAIN;
	for (uint32_t v = 0; v < *nv; v++)
		if (!vmIdle(vms[v])) return EBUSY;
	return 0;
}

// xe_ttm_bo_purge for one DONTNEED BO of client c (its lock held). 0: purged.
static int boPurge(XeClientState *c, XeBo *bo) {
	XeVm *vms[8];
	uint32_t nv = 0;
	int err = purgeBlocked(c, bo, vms, &nv);
	if (err) {
		if (err == EBUSY) gPurgeSt.notIdle++;
		else if (err == EPERM) gPurgeSt.cpuMapped++;
		else if (err == EAGAIN) gPurgeSt.held++;
		return err;
	}
	BindCtx ctx[8];
	for (uint32_t v = 0; v < nv; v++) {
		ctx[v] = {};
		ctx[v].clear = {0, ~0u, 0, 0};
		for (uint32_t k = 0; k < vms[v]->nBinds; k++) {
			const XeBinding &b = vms[v]->binds[k];
			if (b.bo == bo && vmUnmap(vms[v], b.addr, b.range, &ctx[v])) err = ENOMEM;
		}
	}
	gDev->deviceWmb();   // PTE writes (BAR2, write-combined) before the invalidation (rb8c: xe_device_wmb)
	for (uint32_t v = 0; v < nv; v++) {
		OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&vms[v]->ptGen));
		if (ctx[v].unmapped || ctx[v].parked) {
			const uint32_t seq = tlbInvalidateRange(vms[v]->asid, ctx[v].lo, ctx[v].hi);
			deferSeal(&ctx[v], seq, vms[v]);
			vms[v]->unbindNs = nowNs();   // rb8b
			vms[v]->unbinds++;
			vms[v]->unbindTlb = seq;
		}
	}
	if (err) {   // a huge leaf could not be split: its PTEs back to the backing, the BO stays DONTNEED (xe keeps the state)
		boRemap(vms, nv, bo);
		gPurgeSt.failed++;
		return err;
	}
	// the backing goes once the invalidations issued so far (these, and earlier unbinds of it elsewhere) have completed
	const uint32_t seq = gTlbSeq;
	const uint64_t size = bo->size;
	if (bo->vram) {
		auto *l = static_cast<VramList *>(IOMalloc(sizeof(VramList)));
		if (l) *l = {bo->blocks, bo->hi};
		if (!l || !deferAdd(l, kDefVram, seq, nullptr)) {
			if (tlbWait(nullptr, seq)) {
				if (l) vramListFree(l);
				else vramBlocksFree(bo->blocks, bo->hi);
			} else if (l) {
				IOFree(l, sizeof(*l));   // the invalidation did not complete: leaked rather than reused under a TLB entry
			}
		}
		bo->blocks = nullptr;
		bo->vram = bo->hi = bo->contig = false;
		bo->dpa = 0;
		gPurgeSt.purgedVram++;
	} else {
		auto *l = static_cast<SysList *>(IOMalloc(sizeof(SysList)));
		if (l) {
			l->sys = bo->sys;
			l->dma = bo->dma;
			if (!deferAdd(l, kDefSys, seq, nullptr)) {
				if (tlbWait(nullptr, seq)) sysListFree(l);
				else IOFree(l, sizeof(*l));   // leaked (see above)
			}
		} else if (tlbWait(nullptr, seq)) {
			bo->dma.unmap();
			bo->sys->release();
		}
		bo->sys = nullptr;
		memset(static_cast<void *>(&bo->dma), 0, sizeof(bo->dma));
		OSAddAtomic64(-(SInt64)size, const_cast<SInt64 *>(&gSysUsed));
		gPurgeSt.purgedSys++;
	}
	bo->clear = {0, ~0u, 0, 0};
	boSetPurge(bo, kMadvPurged);   // xe_bo_set_purgeable_state(bo, XE_MADV_PURGEABLE_PURGED)
	boMapsPrune(c, bo, true);       // the maps never used: their descriptors name the freed VRAM
	gPurgeSt.purged++;
	gPurgeSt.purgedBytes += size;
	lastMoveSet('p');
	return 0;
}

// What a purge pass may take: VRAM anywhere, VRAM in the BAR window, system memory.
enum : uint32_t { kPurgeVram = 1, kPurgeWindow = 2, kPurgeSys = 4, kPurgeAny = 7 };

// One pass over client c (its lock held): its DONTNEED BOs of the kinds asked, by handle, then those only bindings
// hold (handle closed), until `need` bytes are freed (0: every one). Returns the bytes freed.
static uint64_t purgeClient(XeClientState *c, uint64_t need, uint32_t what) {
	if (c->dying || c->migrating) return 0;
	const uint32_t scan = (uint32_t)OSIncrementAtomic(&gPurgeScan) + 1;
	uint64_t freed = 0;
	auto one = [&](XeBo *bo) {
		if (!bo || !bo->purgeTrack || bo->purgeState != kMadvDontneed || bo->purgeScan == scan) return;
		bo->purgeScan = scan;
		const bool want = bo->vram ? (what & kPurgeVram) || ((what & kPurgeWindow) && !bo->hi) : bo->sys && (what & kPurgeSys);
		const uint64_t size = bo->size;
		if (want && !boPurge(c, bo)) freed += size;
	};
	for (uint32_t h = 1; h <= c->bos.n && (!need || freed < need); h++) one(static_cast<XeBo *>(c->bos.get(h)));
	for (uint32_t h = 1; h <= c->vms.n && (!need || freed < need); h++)
		if (auto *vm = static_cast<XeVm *>(c->vms.get(h)))
			for (uint32_t k = 0; k < vm->nBinds && (!need || freed < need); k++) one(vm->binds[k].bo);
	return freed;
}

// A purge pass over every client: self (its lock held) first, then the others whose lock is free now (a try-lock, as
// evictWindow); self == nullptr (debug.intelxe.purge, no client lock held): every client, waiting for its lock (the
// order gClientsLock -> client lock, as xeScanoutPin). Afterwards the freed memory is waited for, so a retried
// allocation finds it. Returns the bytes freed.
static uint64_t purgeAll(XeClientState *self, uint64_t need, uint32_t what) {
	if (!gDev || !gDev->up || gSuspended || gDontneedBos <= 0) return 0;
	gPurgeSt.passes++;
	uint64_t freed = self ? purgeClient(self, need, what) : 0;
	if ((!need || freed < need) && (self ? IOLockTryLock(gClientsLock) : (IOLockLock(gClientsLock), true))) {
		for (XeClientState *o = gClients; o && (!need || freed < need); o = o->next) {
			if (o == self) continue;
			if (self) {
				if (!IOLockTryLock(o->lock)) continue;
			} else {
				IOLockLock(o->lock);
			}
			freed += purgeClient(o, need ? need - freed : 0, what);
			IOLockUnlock(o->lock);
		}
		IOLockUnlock(gClientsLock);
	}
	if (freed) {
		tlbWait(nullptr, gTlbSeq);
		deferReap();
	}
	return freed;
}

// debug.intelxe.purge: write 1 = purge every DONTNEED BO that can be now (memory pressure on demand, for tests); the
// count purged by this call is logged. xePurgeStats: debug.intelxe.purge_stats.
int xePurgeNow() {
	const uint32_t before = gPurgeSt.purged;
	const uint64_t freed = purgeAll(nullptr, 0, kPurgeAny);
	say("purge: %u BO(s), %llu KB freed on request\n", gPurgeSt.purged - before, freed >> 10);
	return 0;
}
int xePurgeStats(char *out, size_t len) {
	const auto &p = gPurgeSt;
	return snprintf(out, len,
		"madvise purgeable: calls %u, bindings to DONTNEED %u, to WILLNEED %u, retained 0 answered %u; DONTNEED BOs now %d\n"
		"refused: VM_BIND/PREFETCH of a DONTNEED BO %u, of a purged BO %u; CPU maps of a DONTNEED/purged BO %u\n"
		"purge passes %u: purged %u BOs (VRAM %u, system %u, %llu KB); skipped: not idle %u, CPU-mapped %u, held %u, "
		"failed %u\n",
		p.calls, p.toDontneed, p.toWillneed, p.retainedZero, (int)gDontneedBos, p.bindBusy, p.bindPurged, p.mmapRefused, p.passes, p.purged,
		p.purgedVram, p.purgedSys, p.purgedBytes >> 10, p.notIdle, p.cpuMapped, p.held, p.failed);
}

// Window BOs of client c that may move out now: VRAM in the window, never CPU-mapped, not pinned, cleared, and not bound
// in a VM that has work outstanding (each VM's state is taken once per scan). Returns the bytes moved. Handle order, not
// LRU as TTM's eviction is.
static bool evictable(const XeBo *bo) {
	return bo && bo->vram && !bo->hi && ((!bo->mapped && !bo->cpuMaps && !bo->pins && !bo->scanout && !bo->migrating &&
		!bo->noMove) || (bo->purgeTrack && bo->purgeState == kMadvDontneed));   // rb7r: a DONTNEED one is purged instead
}

static uint64_t evictClient(XeClientState *c, uint64_t need, const MoveHeld &held = {nullptr, false}) {
	// Most window BOs are CPU-mapped (Mesa maps its state pools, descriptor pools and batch buffers for their whole life):
	// look for a candidate before taking every VM's idle state (vmIdle walks all guc_ids under gSchedLock), which each
	// GEM_CREATE of a visible BO did here while the window stayed full (2026-10-29: 928 scans, nothing ever movable).
	bool any = false;
	for (uint32_t h = 1; h <= c->bos.n && !any; h++) any = evictable(static_cast<XeBo *>(c->bos.get(h)));
	if (!any) return 0;
	XeVm *busy[8];
	uint32_t nBusy = 0;
	for (uint32_t h = 1; h <= c->vms.n; h++) {
		auto *vm = static_cast<XeVm *>(c->vms.get(h));
		if (vm && !vmIdle(vm)) {
			if (nBusy == 8) return 0;
			busy[nBusy++] = vm;
		}
	}
	uint64_t freed = 0;
	for (uint32_t h = 1; h <= c->bos.n && freed < need; h++) {
		auto *bo = static_cast<XeBo *>(c->bos.get(h));
		if (!evictable(bo)) continue;
		if (bo->purgeTrack && bo->purgeState == kMadvDontneed) {   // rb7r: xe_bo_move purges an evicted DONTNEED BO
			const uint64_t size = bo->size;
			if (!boPurge(c, bo)) freed += size;
			continue;
		}
		if (!fenceSignaled(bo->clear)) continue;
		bool inBusy = false;
		for (uint32_t v = 0; v < nBusy && !inBusy; v++)
			for (uint32_t k = 0; k < busy[v]->nBinds && !inBusy; k++) inBusy = busy[v]->binds[k].bo == bo;
		if (!inBusy && !boMove(c, bo, false, false, held)) freed += bo->size;
	}
	return freed;
}

// TTM eviction for the BAR window: the caller's own BOs first, then other clients' whose lock is free right now (a
// try-lock: the caller already holds its own, and xeScanoutPin takes gClientsLock before a client lock). The old blocks
// come back once the remaps' invalidation has completed, which is waited for here.
static bool evictWindow(XeClientState *self, uint64_t need) {
	if (!gHiOn) return false;
	gStats.evictScans++;
	uint64_t freed = self ? evictClient(self, need) : 0;
	if (freed < need && IOLockTryLock(gClientsLock)) {
		for (XeClientState *o = gClients; o && freed < need; o = o->next) {
			if (o == self || !IOLockTryLock(o->lock)) continue;
			// rb7s: a shared BO's move then try-locks the remaining clients (it holds self's and gClientsLock already)
			if (!o->dying && !o->migrating) freed += evictClient(o, need - freed, {self, true});
			IOLockUnlock(o->lock);
		}
		IOLockUnlock(gClientsLock);
	}
	if (!freed) return false;
	tlbWait(nullptr, gTlbSeq);
	deferReap();
	return true;
}

// rb7s: debug.intelxe.evict: write a pid = move every BAR-window BO of that process's connections that was never CPU-mapped
// (and is idle) out of the window now, through the eviction GEM_CREATE uses (evictClient -> boMove), the bindings other
// clients have of them rewritten too (moveSetBegin); 0 = every process's. For tests: a move on demand. gClientsLock,
// then each client's lock in turn (purgeAll's order). Returns the bytes moved.
uint64_t xeEvictNow(int pid) {
	if (!gDev || !gDev->up || gSuspended || !gHiOn) return 0;
	uint64_t freed = 0;
	IOLockLock(gClientsLock);
	for (XeClientState *o = gClients; o; o = o->next) {
		if (pid && o->pid != pid) continue;
		IOLockLock(o->lock);
		if (!o->dying && !o->migrating) freed += evictClient(o, ~0ull, {nullptr, true});
		IOLockUnlock(o->lock);
	}
	IOLockUnlock(gClientsLock);
	if (freed) {
		tlbWait(nullptr, gTlbSeq);
		deferReap();
	}
	say("evict: %llu KB moved out of the BAR window on request (pid %d)\n", freed >> 10, pid);
	return freed;
}

// xe_gt_record_default_lrcs: per engine class run a job on one context, switch to a second context so the first is
// saved, and keep its image (PPHWSP + context registers) as the class's golden context, where the ADS already points
// (guc_golden_lrc_populate). The render context carries our LRC workarounds from its ring prologue.
static int recordGolden() {
	XeVm *vm = vmCreate(0xffff0);
	if (!vm) return ENOMEM;
	int err = 0;
	bool done[5] = {};
	for (uint32_t i = 0; i < gNumEngines && !err; i++) {
		const XeEngine &e = gEngines[i];
		if (done[e.gucClass]) continue;
		done[e.gucClass] = true;
		XeQueue *a = nullptr, *b = nullptr;
		XeFence fa, fb;
		if ((err = queueCreate(vm, e.drmClass, e.logical, &a)) || (err = queueSubmitKernel(a, 0, &fa))) break;
		err = waitUntil(nullptr, (int64_t)(nowNs() + 1000000000ull), kWaitKernel, [&] { return fenceSignaled(fa); });
		if (!err && !(err = queueCreate(vm, e.drmClass, e.logical, &b)) && !(err = queueSubmitKernel(b, 0, &fb)))
			err = waitUntil(nullptr, (int64_t)(nowNs() + 1000000000ull), kWaitKernel, [&] { return fenceSignaled(fb); });
		if (!err) {
			uint8_t *src = a->lrcCpu + ringSize;
			uint8_t *dst = gDev->miscBytes() + (e.gucClass == 0 ? goldenRcsOff : e.gucClass == 4 ? goldenCcsOff : goldenOff);
			if (a->lrcBlk) gDev->l2Flush();   // the context save went through the GPU's L2
			lrcReadPrep(a, src, e.lrcSize);
			memcpy(dst, src, e.lrcSize);
			flushRange(dst, e.lrcSize);
		}
		say("golden context class %u: %s\n", e.drmClass, err ? "FAILED" : "recorded");
		if (b) queueTeardown(b);
		if (a) queueTeardown(a);
	}
	vmPut(vm);
	return err;
}

// ─── ioctls ────────────────────────────────────────────────────────────────────────────────────────────────────────
static int putUser(uint64_t uaddr, const void *src, size_t len) { return copyout(src, (user_addr_t)uaddr, len) ? EFAULT : 0; }
static int getUser(uint64_t uaddr, void *dst, size_t len) { return copyin((user_addr_t)uaddr, dst, len) ? EFAULT : 0; }

template <typename T> static int answer(drm_xe_device_query *q, const T *data, uint32_t size) {
	if (q->size == 0) {
		q->size = size;
		return 0;
	}
	if (q->size != size) return EINVAL;
	return putUser(q->data, data, size);
}

static int deviceQuery(drm_xe_device_query *q) {
	if (q->extensions || q->reserved[0] || q->reserved[1]) return EINVAL;   // xe_query_ioctl
	switch (q->query) {
		case DRM_XE_DEVICE_QUERY_ENGINES: {
			struct { drm_xe_query_engines h; drm_xe_engine e[12]; } r = {};
			r.h.num_engines = gNumEngines;
			for (uint32_t i = 0; i < gNumEngines; i++) r.e[i].instance = {gEngines[i].drmClass, gEngines[i].logical, 0, 0};
			if (gVideoWanted && gDev->mediaUp())   // video decode engines on the media GT, gt_id 1
				for (uint32_t i = 0; i < gNumMediaEngines; i++)
					r.e[r.h.num_engines++].instance = {gMediaEngines[i].drmClass, gMediaEngines[i].logical, 1, 0};
			return answer(q, &r, (uint32_t)(sizeof(r.h) + r.h.num_engines * sizeof(drm_xe_engine)));
		}
		case DRM_XE_DEVICE_QUERY_MEM_REGIONS: {
			// xe_query.c query_mem_regions: system memory used = GPU BOs in system memory (TTM TT usage); VRAM total =
			// the usable size, used including the kernel's own reservation below vramUserStart.
			struct { drm_xe_query_mem_regions h; drm_xe_mem_region m[2]; } r = {};
			r.h.num_mem_regions = 2;
			r.m[0] = {};
			r.m[0].mem_class = DRM_XE_MEM_REGION_CLASS_SYSMEM;
			r.m[0].instance = 0;
			r.m[0].min_page_size = 0x1000;
			r.m[0].total_size = gSysTotal;
			r.m[0].used = gSysUsed > 0 ? (uint64_t)gSysUsed : 0;
			r.m[1].mem_class = DRM_XE_MEM_REGION_CLASS_VRAM;
			r.m[1].instance = 1;
			r.m[1].min_page_size = 0x1000;
			r.m[1].total_size = vramEnd;   // includes the kernel's reservation below vramUserStart, counted as used
			IOLockLock(gResLock);
			r.m[1].used = gVramUsed + vramUserStart;
			r.m[1].cpu_visible_used = gVisibleUsed + vramUserStart;
			IOLockUnlock(gResLock);
			r.m[1].cpu_visible_size = vramVisible;
			return answer(q, &r, sizeof(r));
		}
		case DRM_XE_DEVICE_QUERY_CONFIG: {
			struct { drm_xe_query_config h; uint64_t info[5]; } r = {};
			r.h.num_params = 5;
			r.info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] = gDev->devId | (uint32_t)gDev->revId << 16;
			// rb7j: + HAS_LOW_LATENCY (DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT -> SLPC_CTX_FREQ_REQ_IS_COMPUTE) and
			// HAS_DISABLE_STATE_CACHE_PERF_FIX (the render queue property), as xe reports them. rb7r: + HAS_PURGING_SUPPORT,
			// which xe sets unconditionally (xe_query.c query_config): DRM_IOCTL_XE_MADVISE purgeable state (vmMadvise).
			// Not HAS_CPU_ADDR_MIRROR (no SVM / fault mode).
			r.info[DRM_XE_QUERY_CONFIG_FLAGS] = (gXePlat->isDgfx ? DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM : 0) |
				DRM_XE_QUERY_CONFIG_FLAG_HAS_NO_COMPRESSION_HINT |
				DRM_XE_QUERY_CONFIG_FLAG_HAS_LOW_LATENCY | DRM_XE_QUERY_CONFIG_FLAG_HAS_DISABLE_STATE_CACHE_PERF_FIX |
				DRM_XE_QUERY_CONFIG_FLAG_HAS_PURGING_SUPPORT;
			r.info[DRM_XE_QUERY_CONFIG_MIN_ALIGNMENT] = 0x1000;
			r.info[DRM_XE_QUERY_CONFIG_VA_BITS] = gXePlat->vaBits;
			r.info[DRM_XE_QUERY_CONFIG_MAX_EXEC_QUEUE_PRIORITY] = 1;
			return answer(q, &r, sizeof(r));
		}
		case DRM_XE_DEVICE_QUERY_GT_LIST: {
			struct { drm_xe_query_gt_list h; drm_xe_gt g[2]; } r = {};
			r.h.num_gt = 1;
			r.g[0].type = DRM_XE_QUERY_GT_TYPE_MAIN;
			r.g[0].reference_clock = gRefClock;
			r.g[0].near_mem_regions = 1u << 1;
			r.g[0].far_mem_regions = 1u << 0;
			r.g[0].ip_ver_major = gXePlat->graphicsVerx100 / 100;   // xe reads GMD_ID; the descriptor's IP (20.01)
			r.g[0].ip_ver_minor = gXePlat->graphicsVerx100 % 100;
			if (gVideoWanted && gDev->mediaUp()) {   // the standalone media GT (xe_query.c query_gt_list), 13.01
				r.g[1] = r.g[0];
				r.g[1].type = DRM_XE_QUERY_GT_TYPE_MEDIA;
				r.g[1].gt_id = 1;
				r.g[1].ip_ver_major = gXePlat->mediaVerx100 / 100;
				r.g[1].ip_ver_minor = gXePlat->mediaVerx100 % 100;
				r.g[1].ip_ver_rev = 1;
				r.h.num_gt = 2;
			}
			return answer(q, &r, (uint32_t)(sizeof(r.h) + r.h.num_gt * sizeof(drm_xe_gt)));
		}
		case DRM_XE_DEVICE_QUERY_HWCONFIG:
			return answer(q, gHwconfig, gHwconfigLen);
		case DRM_XE_DEVICE_QUERY_GT_TOPOLOGY: {
			// calc_topo_query_size: geometry and compute DSS bitmaps (128 bits), L3 banks (64), EU per DSS (32 bits in
			// an unsigned long bitmap: 8 bytes). Xe2 EUs are SIMD16.
			struct __attribute__((packed)) {
				drm_xe_query_topology_mask g; uint8_t gm[16];
				drm_xe_query_topology_mask c; uint8_t cm[16];
				drm_xe_query_topology_mask l; uint8_t lm[8];
				drm_xe_query_topology_mask e; uint8_t em[8];
			} r = {};
			r.g = {0, DRM_XE_TOPO_DSS_GEOMETRY, 16};
			memcpy(r.gm, &gGeomDss, 4);
			r.c = {0, DRM_XE_TOPO_DSS_COMPUTE, 16};
			memcpy(r.cm, &gCompDss, 4);
			r.l = {0, DRM_XE_TOPO_L3_BANK, 8};
			memcpy(r.lm, &gL3Mask, 8);
			r.e = {0, DRM_XE_TOPO_SIMD16_EU_PER_DSS, 8};
			memcpy(r.em, &gEuMask, 4);
			return answer(q, &r, sizeof(r));
		}
		case DRM_XE_DEVICE_QUERY_ENGINE_CYCLES: {
			drm_xe_query_engine_cycles r;
			if (q->size != sizeof(r)) {
				if (q->size == 0) {
					q->size = sizeof(r);
					return 0;
				}
				return EINVAL;
			}
			int err = getUser(q->data, &r, sizeof(r));
			if (err) return err;
			const XeEngine *eng = engineFor(r.eci.engine_class, r.eci.engine_instance);
			if (!eng) return EINVAL;
			if (eng->gt ? !gDev->mediaWake(true) : !gDev->fwOn(true)) return EIO;
			uint64_t t0 = nowNs();
			uint32_t hi, lo;
			do {   // RING_TIMESTAMP / _UDW
				hi = gDev->mmio(eng->base + 0x35c);
				lo = gDev->mmio(eng->base + 0x358);
			} while (hi != gDev->mmio(eng->base + 0x35c));
			uint64_t t1 = nowNs();
			if (eng->gt) gDev->mediaWake(false);
			else gDev->fwOn(false);
			r.engine_cycles = (uint64_t)hi << 32 | lo;
			r.cpu_timestamp = t0;
			r.cpu_delta = t1 - t0;
			r.width = 64;
			return putUser(q->data, &r, sizeof(r));
		}
		case DRM_XE_DEVICE_QUERY_UC_FW_VERSION: {
			drm_xe_query_uc_fw_version r;
			if (q->size == 0) {
				q->size = sizeof(r);
				return 0;
			}
			if (q->size != sizeof(r)) return EINVAL;
			int err = getUser(q->data, &r, sizeof(r));
			if (err) return err;
			if (r.uc_type == XE_QUERY_UC_TYPE_HUC) {
				// query_uc_fw_version: the media GT's HuC release version, only while it is RUNNING (authenticated);
				// Mesa's xe_gem_supports_huc reads success as has_huc (H.265 decode). Video-gated like the engines.
				if (!gVideoWanted || !gDev->mediaUp() || !gDev->hucRunning()) return ENODEV;
				r.branch_ver = 0;
				r.major_ver = gDev->hucVer >> 16 & 0xff;   // CSS sw_version (bmg_huc.bin: 8.2.10)
				r.minor_ver = gDev->hucVer >> 8 & 0xff;
				r.patch_ver = gDev->hucVer & 0xff;
				return putUser(q->data, &r, sizeof(r));
			}
			if (r.uc_type != XE_QUERY_UC_TYPE_GUC_SUBMISSION) return ENODEV;
			r.branch_ver = 0;
			r.major_ver = 1;
			r.minor_ver = 38;
			r.patch_ver = 1;
			return putUser(q->data, &r, sizeof(r));
		}
		case DRM_XE_DEVICE_QUERY_PXP_STATUS:
			// rb7r: xe_query.c query_pxp_status: size 0 asks the size, another size than the struct EINVAL, then
			// xe_pxp_get_readiness_status, which is ENODEV without PXP (Battlemage: bmg_desc has no has_pxp). Was the
			// default's EINVAL; Mesa (xe_gem_supports_protected_exec_queue) takes either as "no protected content".
			if (!q->size) {
				q->size = sizeof(drm_xe_query_pxp_status);
				return 0;
			}
			return q->size != sizeof(drm_xe_query_pxp_status) ? EINVAL : ENODEV;
		default:
			return EINVAL;
	}
}

// The drm_xe_sync array of an exec or bind, copied in once (xe_sync_entry_parse; DRM_XE_MAX_SYNCS = 1024 in Linux).
// flags: kXeSyncParse* (xe_uapi_args.h). ufence: index of the user fence that counts, -1 none: an exec's last one
// (xe_exec_ioctl calls xe_sched_job_init_user_fence for each, the last wins; it never counts them, since an exec's sync
// has no sync->ufence for xe_sync_is_ufence), a bind's only one (xe_vm_bind_ioctl: more than one EINVAL).
struct SyncList {
	drm_xe_sync local[16];
	drm_xe_sync *s {local};
	uint32_t n {0};
	int32_t ufence {-1};
	int load(uint64_t uaddr, uint32_t num, uint32_t flags) {
		if (num > DRM_XE_MAX_SYNCS) return EINVAL;
		if (num > 16 && !(s = static_cast<drm_xe_sync *>(IOMalloc(num * sizeof(drm_xe_sync))))) {
			s = local;
			return ENOMEM;
		}
		n = num;
		if (!num) return 0;
		if (int err = getUser(uaddr, s, num * sizeof(drm_xe_sync))) return err;
		// xe_sync_entry_parse, for every entry before anything is done (xeSyncEntryCheck): only the SIGNAL flag, reserved
		// zero, a 32-bit syncobj handle (it shares a union with addr), a nonzero timeline point, a known type; a user
		// fence only as a signal, 8-byte aligned, and not in a VM_BIND without binds. A bind's user fence is a user pointer
		// read once now (user_fence_create's get_user: EFAULT); an exec's is a GPU address in the VM, not read.
		uint32_t nUf = 0;
		for (uint32_t i = 0; i < num; i++) {
			const drm_xe_sync &e = s[i];
			if (int err = xeSyncEntryCheck(e, flags)) return err;
			if (e.type != DRM_XE_SYNC_TYPE_USER_FENCE) continue;
			if (!(flags & kXeSyncParseExec)) {
				uint64_t v;
				if (getUser(e.addr, &v, sizeof(v))) return EFAULT;
				nUf++;
			}
			ufence = (int32_t)i;
		}
		return nUf > 1 ? EINVAL : 0;
	}
	~SyncList() {
		if (s != local) IOFree(s, n * sizeof(drm_xe_sync));
	}
};

// A wait sync's syncobj and point; ENOENT if the handle is gone (it may be destroyed while the caller waits).
static int syncWaitTarget(XeClientState *c, const drm_xe_sync &s, XeSyncobj **so, uint64_t *point) {
	if (s.type == DRM_XE_SYNC_TYPE_USER_FENCE) return EINVAL;
	if (s.type != DRM_XE_SYNC_TYPE_SYNCOBJ && s.type != DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ) return EINVAL;
	*so = static_cast<XeSyncobj *>(c->syncs.get(s.handle));
	*point = s.type == DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ ? s.timeline_value : 0;
	return *so ? 0 : ENOENT;
}

// A point nobody has submitted yet would block forever. Linux answers it with EINVAL at once; this driver gives a
// submit racing on another thread 10 s to materialize it (only the materialization is waited for, not the fence).
static int waitMaterialized(XeClientState *c, const drm_xe_sync &s) {
	bool gone = false;
	const int err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitSubmit, [&] {
		XeSyncobj *so;
		uint64_t point;
		if (syncWaitTarget(c, s, &so, &point)) return gone = true;
		SyncGuard g(so);
		XeFence f;
		return syncFind(so, point, &f) || (point && syncValue(so) >= point);
	});
	if (gone) return ENOENT;
	if (err == ETIME) {
		say("exec: in-fence %u point %llu never materialized\n", s.handle, s.timeline_value);
		return EINVAL;
	}
	return err;
}

// The unsignaled fences a wait sync stands for (drm_syncobj_find_fence + dma_fence_chain: a timeline point is done when
// its fence and every earlier point's fence are). Growable list; gone handles and type errors are reported.
struct DepList {
	XeFence local[16];
	XeFence *f {local};
	uint32_t n {0}, cap {16};
	bool add(const XeFence &x) {
		if (n == cap) {
			auto *nf = static_cast<XeFence *>(IOMalloc(cap * 2 * sizeof(XeFence)));
			if (!nf) return false;
			memcpy(nf, f, n * sizeof(XeFence));
			if (f != local) IOFree(f, cap * sizeof(XeFence));
			f = nf;
			cap *= 2;
		}
		f[n++] = x;
		return true;
	}
	~DepList() {
		if (f != local) IOFree(f, cap * sizeof(XeFence));
	}
};

static int collectDeps(XeClientState *c, const SyncList &sl, DepList &deps) {
	for (uint32_t i = 0; i < sl.n; i++) {
		const drm_xe_sync &s = sl.s[i];
		if (s.flags & DRM_XE_SYNC_FLAG_SIGNAL) continue;
		int err = waitMaterialized(c, s);
		if (err) return err;
		XeSyncobj *so;
		uint64_t point;
		if ((err = syncWaitTarget(c, s, &so, &point))) return err;
		SyncGuard g(so);
		syncValue(so);   // prune signaled history
		XeFence found;
		if (!syncFind(so, point, &found)) continue;   // every point up to it signaled and pruned
		const uint32_t span = point ? 0 : syncBinSpan(so);   // rb7t: binary: its entry (or a sync file's entries)
		for (uint32_t k = 0; k < so->n; k++) {
			const XeFence &f = so->f[k];
			if (point && f.point > found.point) continue;
			if (!point && k >= span) break;
			if (!fenceSignaled(f) && !deps.add(f)) return ENOMEM;
		}
	}
	return 0;
}

// In-fences of a bind or of a bind-queue exec: waited for on the CPU (binds are CPU page-table writes here).
static int waitSyncsIn(XeClientState *c, const SyncList &sl) {
	DepList deps;
	int err = collectDeps(c, sl, deps);
	if (err) return err;
	return waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitDeps, [&] {
		for (uint32_t i = 0; i < deps.n; i++)
			if (!fenceSignaled(deps.f[i])) return false;
		return true;
	});
}

// User fences (DRM_XE_SYNC_TYPE_USER_FENCE, DRM_IOCTL_XE_WAIT_USER_FENCE), for debug.intelxe.stats: execs with one; bind
// fences written, whose write faulted, whose bind fence had not signaled in time (not written); waits, met, timed
// out, read faults, ended by a reset of their exec queue.
static struct { uint32_t execs, binds, bindFaults, bindLate, waits, met, timeouts, faults, resets; } gUfSt;
// rb8d, debug.intelxe.stats: VM_BINDs that unbound (their out-fence includes the invalidation), whose out-fence still
// waited for an invalidation when the ioctl returned, of those carrying an earlier call's, calls without ops
static struct { uint32_t unbinds, tlbPending, tlbCarried, noOps; } gBindSt;

// A bind's user fence (xe_sync.c xe_sync_entry_signal -> user_fence_cb -> user_fence_worker): the value goes to the user
// pointer once the bind's fence has signaled, then the waiters are woken (wake_up_all(&xe->ufence_wq)). Binds are
// synchronous here: the PTEs are in when VM_BIND gets here, and the bind's fence (f) is the copy-engine clear of the BOs
// it bound and (rb8d) the TLB invalidation of what it unbound (XeBindFence, xe_pt_update_ops_run). Signaled already:
// written now (copyout from the client's task). Not yet (rb8e): written asynchronously by the kernel (ufAsyncAdd); the
// bounded wait below is the fallback when the page cannot be wired. The write that faults is no error of the ioctl (xe:
// XE_WARN_ON "Copy to user failed").
static void ufWrite(uint64_t addr, uint64_t v) {
	if (putUser(addr, &v, sizeof(v))) gUfSt.bindFaults++;
	else gUfSt.binds++;
	wakeAll();
}

// The late bind fences whose fence has signaled meanwhile: written, in the order they came. Only from the task that
// opened the client (the user pointers are its addresses). Client lock held.
static void lateUfFlush(XeClientState *c) {
	if (!c->nLateUf || c->task != current_task()) return;
	uint32_t k = 0;
	for (uint32_t i = 0; i < c->nLateUf; i++) {
		if (bindFenceSignaled(c->lateUf[i].f)) ufWrite(c->lateUf[i].addr, c->lateUf[i].value);
		else c->lateUf[k++] = c->lateUf[i];
	}
	c->nLateUf = k;
}

// rb8e: bind user fences written asynchronously, as xe's user_fence_worker: xe_sync_entry_parse takes a reference on the
// caller's mm (mmgrab), xe_sync_entry_signal adds user_fence_cb to the bind's fence, the callback queues the worker, and
// the worker writes the value into that mm (mmget_not_zero, kthread_use_mm, copy_to_user of the u64), then wakes every
// waiter (wake_up_all(&xe->ufence_wq)). Here, when VM_BIND returns with its fence still pending, the user page is wired
// and mapped into the kernel (IOMemoryDescriptor::withAddressRange on the calling task, prepare(), createMappingInTask
// on kernel_task: the page stays resident at one address whatever the task does with it), and the completion path
// stores the value through that mapping (one aligned 64-bit store, as copy_to_user of a u64) once the whole bind fence
// (clears and invalidation) has signaled, then wakes the waiters. The completion path is the submit worker (kicked by
// TLB_INVALIDATION_DONE, by the job completions that end a clear, by the TLB fence watchdog; a 100 ms timer while any is
// pending) and every ioctl of any client, WAIT_USER_FENCE's checks included. Order: as the binds returned (a later
// bind of the same bind queue folds the earlier ones' fences into its own, so it cannot be ready first), and a fence
// already signaled at return is written only after the ready ones before it. The client's close writes those ready
// and drops the rest (xe writes as long as the mm has users; this driver's client lives as long as the process's
// connection). WAS: the ioctl waited for the fence (up to 2 s, then the client's late list), now the fallback
// (bindUfenceWait) when the page cannot be wired or kUfAsyncMax are pending. Counted (debug.intelxe.stats "bind user
// fences (rb8e)").
struct XeUfAsync {
	XeUfAsync *next;
	const XeClientState *c;
	IOMemoryDescriptor *md;
	IOMemoryMap *map;
	volatile uint64_t *kva;
	uint64_t value, t0;
	XeBindFence f;
};
static XeUfAsync *gUfaHead, **gUfaTail = &gUfaHead;
static IOLock *gUfaLock;
static constexpr uint32_t kUfAsyncMax = 1024;
static struct { uint32_t direct, pinned, written, dropped, pinFails, full, waited, maxPending, maxUs; uint64_t us; } gUfaSt;

static void ufAsyncFree(XeUfAsync *e) {
	OSSafeReleaseNULL(e->map);
	e->md->complete();
	e->md->release();
	IOFree(e, sizeof(*e));
}
// Writes every pending fence that has signaled, in order. closing: that client's connection is going: its unsignaled
// ones are dropped as well. Any thread context, no lock of this driver held but possibly a client lock.
static void ufAsyncRun(const XeClientState *closing) {
	if (!gUfaN || !gUfaLock) return;
	XeUfAsync *gone = nullptr;
	uint32_t wrote = 0;
	const uint64_t now = nowNs();
	IOLockLock(gUfaLock);
	for (XeUfAsync **pp = &gUfaHead; *pp;) {
		XeUfAsync *e = *pp;
		const bool ready = bindFenceSignaled(e->f);
		if (!ready && !(closing && e->c == closing)) {
			pp = &e->next;
			continue;
		}
		if (ready) {
			*e->kva = e->value;   // one 64-bit store (the address is 8-byte aligned: xe_sync_entry_parse)
			wrote++;
			const uint64_t us = (now - e->t0) / 1000;
			gUfaSt.us += us;
			if (us > gUfaSt.maxUs) gUfaSt.maxUs = (uint32_t)(us > 0xffffffffull ? 0xffffffffull : us);
		} else {
			gUfaSt.dropped++;
		}
		*pp = e->next;
		if (gUfaTail == &e->next) gUfaTail = pp;
		e->next = gone;
		gone = e;
		gUfaN--;
	}
	gUfaSt.written += wrote;
	IOLockUnlock(gUfaLock);
	if (wrote) wakeAll();
	while (gone) {
		XeUfAsync *e = gone;
		gone = e->next;
		ufAsyncFree(e);
	}
}
// Wire and map the page of s.addr in the calling task and queue the write. false: not possible (the fallback waits).
static bool ufAsyncAdd(XeClientState *c, const drm_xe_sync &s, const XeBindFence &f) {
	if (!gUfaLock) return false;
	if (gUfaN >= kUfAsyncMax) {
		gUfaSt.full++;
		return false;
	}
	IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(s.addr & ~0xfffull, 0x1000, kIODirectionInOut, current_task());
	if (md && md->prepare(kIODirectionInOut) != kIOReturnSuccess) OSSafeReleaseNULL(md);
	IOMemoryMap *map = md ? md->createMappingInTask(kernel_task, 0, kIOMapAnywhere) : nullptr;
	auto *e = map ? static_cast<XeUfAsync *>(IOMallocZero(sizeof(XeUfAsync))) : nullptr;
	if (!e) {
		OSSafeReleaseNULL(map);
		if (md) {
			md->complete();
			md->release();
		}
		gUfaSt.pinFails++;
		return false;
	}
	e->c = c;
	e->md = md;
	e->map = map;
	e->kva = reinterpret_cast<volatile uint64_t *>(map->getVirtualAddress() + (s.addr & 0xfff));
	e->value = s.timeline_value;
	e->t0 = nowNs();
	e->f = f;
	IOLockLock(gUfaLock);
	*gUfaTail = e;
	gUfaTail = &e->next;
	const uint32_t n = ++gUfaN;
	if (n > gUfaSt.maxPending) gUfaSt.maxPending = n;
	gUfaSt.pinned++;
	IOLockUnlock(gUfaLock);
	schedKick();   // it may have signaled meanwhile
	return true;
}

// The fallback (rb8d's path): waited for up to 2 s (interrupted sleeps are taken again, since the bind itself is done and
// its fence must still be written; an abort that interrupts every sleep at once - process exit - ends the wait). A
// fence still not signaled then (a GT reset or suspend holding the copy engine) is not dropped: it goes on the client's
// late list and its value is written once it signals, by the client's next ioctl or by the WAIT_USER_FENCE that waits
// for it (lateUfFlush). Client lock held (dropped while asleep).
static void bindUfenceWait(XeClientState *c, const drm_xe_sync &s, const XeBindFence &f) {
	gUfaSt.waited++;
	const int64_t until = (int64_t)(nowNs() + 2000000000ull);
	uint32_t intr = 0;
	while (!bindFenceSignaled(f) && !c->dying && (int64_t)nowNs() < until)
		if (waitUntil(c, until, kWaitClear, [&] { return bindFenceSignaled(f); }) == EINTR && ++intr >= 8) break;
	if (c->dying) return;
	if (!bindFenceSignaled(f)) {
		gUfSt.bindLate++;
		if (c->nLateUf < sizeof(c->lateUf) / sizeof(c->lateUf[0])) {
			c->lateUf[c->nLateUf++] = {s.addr, s.timeline_value, f};
			g2hLog("VM_BIND: user fence at 0x%llx late: written once the bind's clear and invalidation complete\n", s.addr);
		} else {
			g2hLog("VM_BIND: user fence at 0x%llx not written: the bind's clear or invalidation did not complete within "
				"2 s and %u late fences are pending\n", s.addr, c->nLateUf);
		}
		return;
	}
	ufAsyncRun();
	lateUfFlush(c);   // earlier late ones first: their values come before this one on a timeline
	ufWrite(s.addr, s.timeline_value);
}

static void bindUfenceSignal(XeClientState *c, const drm_xe_sync &s, const XeBindFence &f) {
	if (bindFenceSignaled(f)) {   // rb8e: signaled at return: written now, after the ready ones queued before it
		gUfaSt.direct++;
		ufAsyncRun();
		lateUfFlush(c);
		ufWrite(s.addr, s.timeline_value);
		return;
	}
	if (!ufAsyncAdd(c, s, f)) bindUfenceWait(c, s, f);
}

// f[0..n): the fences the out-syncs stand for (a bind's clear and invalidation, rb8d; one otherwise): a binary syncobj gets
// them all (as a sync file's several, syncReplaceN), a timeline point gets each at that point (reached when all are)
static int signalSyncsOut(XeClientState *c, const SyncList &sl, const XeFence *f, uint32_t n) {
	int err = 0;
	for (uint32_t i = 0; i < sl.n && !err; i++) {
		const drm_xe_sync &s = sl.s[i];
		if (!(s.flags & DRM_XE_SYNC_FLAG_SIGNAL)) continue;
		if (s.type == DRM_XE_SYNC_TYPE_USER_FENCE) continue;   // the job's ring (exec) or bindUfenceSignal (bind) writes it
		auto *so = static_cast<XeSyncobj *>(c->syncs.get(s.handle));
		if (!so) { err = ENOENT; break; }
		SyncGuard g(so);
		if (s.type == DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ) {
			for (uint32_t k = 0; k < n; k++) {
				XeFence t = f[k];
				t.point = s.timeline_value;
				syncAdd(so, t);
			}
		} else if (n == 1) {
			syncReplace(so, f[0]);
		} else {
			syncReplaceN(so, f, n);
		}
	}
	wakeAll();   // waiters for submission (WAIT_FOR_SUBMIT, materialization)
	return err;
}
static int signalSyncsOut(XeClientState *c, const SyncList &sl, XeFence f) { return signalSyncsOut(c, sl, &f, 1); }

// xe_exec_ioctl -> drm_sched: the job takes the queue's next seqno now (its out-fences exist when the ioctl returns) and
// goes to the ring at once if nothing is ahead of it and its in-fences have signaled; otherwise it waits in the FIFO
// for the submit worker. An exec without batch: its fence is the queue's last one, or, with in-fences, a ring-only job.
static constexpr uint32_t kMaxJobsPerQueue = 1000;   // XE_MAX_JOB_COUNT_PER_EXEC_QUEUE

// uf: the exec's user fence (xe_sched_job_init_user_fence), only with a batch: xe_exec_ioctl's path without batch signals
// the syncs with the queue's last fence (xe_sync_entry_signal), which does nothing for an exec's user fence.
static int queueExec(XeQueue *q, uint64_t batch, const DepList &deps, XeFence *out, const XeUfence &uf = XeUfence{}) {
	XeJob *j = nullptr;
	if (batch || deps.n) {
		const uint32_t cap = deps.n;
		j = static_cast<XeJob *>(IOMalloc(sizeof(XeJob) + cap * sizeof(XeFence)));
		if (!j) return ENOMEM;
		j->next = nullptr;
		j->batch = batch;
		j->nDeps = deps.n;
		j->capDeps = cap;
		j->uf = batch ? uf : XeUfence{};
		memcpy(j->deps(), deps.f, deps.n * sizeof(XeFence));
	}
	bool kick = false;
	int err = 0;
	IOLockLock(gSchedLock);
	// direct submission: a job that is to wait in the FIFO takes the queue out of direct mode first (an armed queue's
	// FIFO stays empty: the armed slot's seqno is the next one handed out); so does one with a user fence, whose store
	// the armed slot (jobPost written when it was armed) does not have
	if (q->dsArmed && j && (deps.n || j->uf.used || q->tdr || gResetting || gSuspended)) dsExit(q, &gStats.dsOtherExits);
	if (q->banned || q->closed || gWedged) {
		err = ECANCELED;
	} else if (j && q->jobCount >= kMaxJobsPerQueue) {
		err = EAGAIN;   // xe_exec_ioctl: job_cnt >= XE_MAX_JOB_COUNT_PER_EXEC_QUEUE (WAS unbounded: kernel memory per job)
	} else if (!j) {
		*out = {0, q->slot, q->gen, q->seqno};
	} else if (!q->jobHead && !deps.n && !q->tdr && !gResetting && !gSuspended && queueEmit(q, batch, q->seqno + 1, &j->uf)) {
		*out = {0, q->slot, q->gen, ++q->seqno};
		gStats.jobsDirect++;
	} else {
		j->seqno = ++q->seqno;
		q->jobCount++;
		if (q->jobTail) q->jobTail->next = j;
		else q->jobHead = j;
		q->jobTail = j;
		if (!q->onSched) {
			q->onSched = true;
			q->schedNext = gSchedHead;
			gSchedHead = q;
		}
		*out = {0, q->slot, q->gen, j->seqno};
		j = nullptr;
		kick = true;
	}
	IOLockUnlock(gSchedLock);
	if (j) jobFree(j);
	if (kick) schedKick();
	return err;
}

// MAP and MAP_USERPTR. xe_vm_bind prepares the page tables before the old VMAs go, so a failed bind leaves the VM as it
// was. Here the old bindings of the range are dropped (their BOs freed after the bind's invalidation) only once the new
// PTEs are all in: dropped first, as before, a map that then failed (no page-table page) left the old PTEs pointing at
// BOs about to be freed, VRAM that would then belong to someone else. A failed map is undone by unmapping the range,
// after which the old BOs may go; if even that fails, the old bindings stay and the new BO is recorded over the range as
// well, so nothing a PTE may still point at is ever freed.
static int vmBindRange(XeVm *vm, XeBo *bo, uint64_t off, uint64_t addr, uint64_t range, uint16_t pat, uint32_t flags,
	BindCtx *ctx, uint32_t handle = 0) {
	// an overwrite touches what it replaces (ptRange: a changed PTE, a replaced table; vmDropBindings: the bindings it
	// drops), so a bind over scratch still needs no invalidation of its own
	int err = vmMap(vm, bo, off, range, addr, pat, flags, ctx);
	if (!err) {
		vmDropBindings(vm, addr, range, nullptr, ctx);
		ptPs64Edges(vm, addr, addr + range, ctx);
		if (vmAddBinding(vm, addr, range, bo, off, pat, flags, handle)) return 0;
		err = ENOMEM;
	}
	if (!vmUnmap(vm, addr, range, ctx)) {
		vmDropBindings(vm, addr, range, nullptr, ctx);
	} else if (bo) {
		boGet(bo);   // never put: a PTE may still point at it (leaked rather than reused under the GPU)
		bo->noMove = true;
	}
	return err;
}

// rb7j: the per-op part of xe_vm.c vm_bind_ioctl_check_args that concerns flags and operation shape (SUPPORTED_FLAGS:
// READONLY, IMMEDIATE, NULL, DUMPABLE, CHECK_PXP, CPU_ADDR_MIRROR, MADVISE_AUTORESET, DECOMPRESS). Accepted and what they
// do here: READONLY (no RW bit), IMMEDIATE (binds are synchronous anyway), NULL (NULL leaves), DUMPABLE (kept in the
// binding's flags, as XE_VMA_DUMPABLE; nothing is captured: there is no devcoredump), CHECK_PXP (no BO here is
// PXP-protected, so there is no invalid key to refuse). CPU_ADDR_MIRROR and MADVISE_AUTORESET need a fault-mode VM, which
// is refused at VM_CREATE, so they are invalid as in xe for such a VM; DECOMPRESS (an in-place resolve at bind) is not
// implemented: EOPNOTSUPP rather than a silent no-op. rb7s: the PAT checks moved here from vmBindOp (patCheck).
// rb7s: the PAT part of vm_bind_ioctl_check_args, for every op (xe checks each bind op's pat_index whatever the op): the
// index must be below xe->pat.n_entries, which is 28 on Battlemage (xe_pat.c xe_pat_init_early: graphics 20.01,
// Wa_16023588340 disables CLOS3, entries 28-31), and its coh_mode nonzero (16-19 are not in xe2_pat_table: hardware
// reserved); a DECOMPRESS bind may not use a compressed index; MAP_USERPTR needs a coherent (1-way or 2-way) index and no
// compression. WAS: MAP only, 16-19 and 32+ refused, so 28-31 reached the PTEs, entries this driver (as xe) never
// programs (IntelXe::programPatMocs writes 28). Mesa uses 0, 1, 6, 9 and 11 (intel_device_info XE2_PAT_ENTRIES; host
// pointers: 1, cached-coherent), the same index for an unbind as for its bind.
static constexpr uint32_t kPatComp = 0x0220de00u;                 // XE2_COMP_EN: 9-12, 14, 15, 21, 25
static constexpr uint32_t kPatReserved = 0x000f0000u;             // 16-19: coh_mode 0
static constexpr uint32_t kPatCoherent = 0x0cc000b6u;             // coh_mode 1-way or 2-way: 1, 2, 4, 5, 7, 22, 23, 26, 27
// n_entries: gXePlat->patEntries. The three masks are xe2_pat_table's (graphics 20).
static int patCheck(const drm_xe_vm_bind_op &op) {
	const uint32_t bit = op.pat_index < 32 ? 1u << op.pat_index : 0;
	if (op.pat_index >= gXePlat->patEntries || (bit & kPatReserved)) return EINVAL;
	const bool comp = bit & kPatComp;
	if ((op.flags & DRM_XE_VM_BIND_FLAG_DECOMPRESS) && comp) return EINVAL;
	if (op.op == DRM_XE_VM_BIND_OP_MAP_USERPTR && (!(bit & kPatCoherent) || comp)) return EINVAL;   // XE_COH_NONE, comp_en
	return 0;
}

// VM_BIND ops one call may carry (64 bytes each: 4 MB). xe has no fixed limit, only what kvmalloc gives (ENOBUFS).
static constexpr uint32_t kMaxBindOps = 65536;

static int bindOpCheck(const drm_xe_vm_bind_op &op) {
	constexpr uint32_t kSupported = DRM_XE_VM_BIND_FLAG_READONLY | DRM_XE_VM_BIND_FLAG_IMMEDIATE | DRM_XE_VM_BIND_FLAG_NULL |
		DRM_XE_VM_BIND_FLAG_DUMPABLE | DRM_XE_VM_BIND_FLAG_CHECK_PXP | DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR |
		DRM_XE_VM_BIND_FLAG_MADVISE_AUTORESET | DRM_XE_VM_BIND_FLAG_DECOMPRESS;
	const bool isNull = op.flags & DRM_XE_VM_BIND_FLAG_NULL, mirror = op.flags & DRM_XE_VM_BIND_FLAG_CPU_ADDR_MIRROR,
		decomp = op.flags & DRM_XE_VM_BIND_FLAG_DECOMPRESS;
	if (mirror) return EINVAL;   // CPU_ADDR_MIRROR needs a fault-mode VM: xe tests it first
	if (int err = patCheck(op)) return err;   // rb7s
	if (op.op > DRM_XE_VM_BIND_OP_PREFETCH || (op.flags & ~kSupported)) return EINVAL;
	if (op.flags & DRM_XE_VM_BIND_FLAG_MADVISE_AUTORESET) return EINVAL;   // no fault-mode VM
	if ((op.obj && isNull) || (op.obj_offset && isNull)) return EINVAL;
	if (op.op != DRM_XE_VM_BIND_OP_MAP && (decomp || isNull)) return EINVAL;
	if (!op.obj && op.op == DRM_XE_VM_BIND_OP_MAP && !isNull) return EINVAL;
	if (op.op == DRM_XE_VM_BIND_OP_UNMAP_ALL && (!op.obj || op.addr || op.range)) return EINVAL;
	if (op.obj && (op.op == DRM_XE_VM_BIND_OP_MAP_USERPTR || op.op == DRM_XE_VM_BIND_OP_PREFETCH ||
		op.op == DRM_XE_VM_BIND_OP_UNMAP))
		return EINVAL;
	if (op.prefetch_mem_region_instance && op.op != DRM_XE_VM_BIND_OP_PREFETCH) return EINVAL;
	// xe vm_bind_ioctl_check_args: every offset, address and size is whole pages (obj_offset shares its union with
	// userptr), and only UNMAP_ALL may have no range. WAS missing: the low 12 bits of an unaligned address or offset
	// reached the PTE, where they are control bits (present, RW, PAT, DM).
	if ((op.obj_offset & 0xfff) || (op.addr & 0xfff) || (op.range & 0xfff) ||
		(!op.range && op.op != DRM_XE_VM_BIND_OP_UNMAP_ALL))
		return EINVAL;
	// xe_vm_bind_ioctl: range > vm->size || addr > vm->size - range (vm->size = 1 << va_bits, 48 as the query says).
	// WAS missing: the page walk masks each level's index, so an address at or above 2^48 aliased a low one.
	const uint64_t vmSize = xePlatformVmSize(gXePlat);
	if (op.range > vmSize || op.addr > vmSize - op.range) return EINVAL;
	if (decomp) return EOPNOTSUPP;
	return 0;
}

static int vmBindOp(XeClientState *c, XeVm *vm, const drm_xe_vm_bind_op &op, BindCtx *ctx) {
	switch (op.op) {
		case DRM_XE_VM_BIND_OP_MAP: {
			XeBo *bo = nullptr;
			// the index was checked with the op (patCheck: below 28, not 16-19); compression is on for 9-12, 14, 15, 21, 25
			const bool comp = (kPatComp >> op.pat_index) & 1;
			if (!(op.flags & DRM_XE_VM_BIND_FLAG_NULL)) {
				bo = static_cast<XeBo *>(c->bos.get(op.obj));
				// xe_vm_bind_ioctl_validate_bo, in its overflow-safe form (WAS obj_offset + range > size, which wraps)
				if (!bo || op.range > bo->size || op.obj_offset > bo->size - op.range) return EINVAL;
				if (bo->noComp && comp) return EINVAL;   // xe_vm_bind_ioctl_validate_bo
				// xe_vm_bind_ioctl_validate_bo: a BO the CPU caches (WB) needs a coherent PAT index (XE_COH_NONE refused)
				if (bo->caching == DRM_XE_GEM_CPU_CACHING_WB && !((kPatCoherent >> op.pat_index) & 1)) return EINVAL;
				// rb7r: xe_vma_create: a new mapping (WILLNEED) of a DONTNEED BO EBUSY, of a purged one EINVAL
				if (bo->purgeTrack && bo->purgeState != kMadvWillneed) {
					if (bo->purgeState == kMadvDontneed) gPurgeSt.bindBusy++;
					else gPurgeSt.bindPurged++;
					return bo->purgeState == kMadvDontneed ? EBUSY : EINVAL;
				}
				if (comp && bo->vram) bo->compBound = true;
				if (bo->scanout && comp && !bo->compWarned) {
					// legal (xe's display decompresses CCS modifiers), but a plane that reads the surface linear shows the
					// compressed bytes: say so once per BO
					bo->compWarned = true;
					say("scan-out BO of %llu KB bound with compressed PAT %u\n", bo->size >> 10, op.pat_index);
				}
			}
			int err = vmBindRange(vm, bo, op.obj_offset, op.addr, op.range, op.pat_index, op.flags, ctx, bo ? op.obj : 0);
			// all clears are on the one migrate queue: the highest seqno covers the others
			if (!err && bo && bo->vram && !fenceSignaled(bo->clear) &&
				(ctx->clear.slot == ~0u || (int32_t)(bo->clear.seqno - ctx->clear.seqno) > 0))
				ctx->clear = bo->clear;
			return err;
		}
		case DRM_XE_VM_BIND_OP_MAP_USERPTR: {
			// VK_EXT_external_memory_host (the Metal plugin imports IOSurface pages this way, WindowServer included):
			// wire the calling task's pages and map them as system memory, like a system BO (xe_vm.c userptr, minus
			// the MMU notifier: the pages stay wired while bound, so they cannot move under the GPU).
			if ((op.userptr & 0xfff) || (op.range & 0xfff) || !op.range) return EINVAL;
			// Failures answer ENOMEM, not EFAULT: Mesa takes any other errno from a bind as a lost device and the Metal
			// plugin then shut its whole backend down (WindowServer aborted); out of memory makes the import fail alone.
			// Pages the task cannot write (WindowServer's view of client CoreAnimation surfaces is r--) are wired for reading
			// and bound read-only, as xe does for a READONLY vma: the PTE loses RW, so the GPU cannot write them either.
			uint32_t flags = op.flags;
			IODirection dir = (flags & DRM_XE_VM_BIND_FLAG_READONLY) ? kIODirectionOut : kIODirectionInOut;
			IOMemoryDescriptor *md = IOMemoryDescriptor::withAddressRange(op.userptr, op.range, dir, current_task());
			if (!md) return ENOMEM;
			IOReturn pr = md->prepare(dir);
			if (pr && dir == kIODirectionInOut) {
				md->release();
				dir = kIODirectionOut;
				flags |= DRM_XE_VM_BIND_FLAG_READONLY;
				md = IOMemoryDescriptor::withAddressRange(op.userptr, op.range, dir, current_task());
				if (!md) return ENOMEM;
				pr = md->prepare(dir);
				if (!pr) {
					say("userptr 0x%llx +%llu: not writable, bound read-only\n", op.userptr, op.range);
					gStats.userptrRo++;   // rb8c: engineDump, debug.intelxe.stats (say's report fills up early)
				}
			}
			if (pr) {
				say("userptr 0x%llx +%llu: prepare 0x%x\n", op.userptr, op.range, pr);
				md->release();
				return ENOMEM;
			}
			auto *bo = static_cast<XeBo *>(IOMallocZero(sizeof(XeBo)));
			if (!bo) { md->complete(); md->release(); return ENOMEM; }
			bo->size = op.range; bo->user = md; bo->caching = DRM_XE_GEM_CPU_CACHING_WB; bo->refs = 1;
			if (!bo->dma.map(md)) {   // dma_map_sgtable of the userptr pages
				say("userptr 0x%llx +%llu: no DMA mapping\n", op.userptr, op.range);
				boPut(bo);
				return ENOMEM;
			}
			int err = vmBindRange(vm, bo, 0, op.addr, op.range, op.pat_index, flags, ctx);   // the binding holds the only reference
			if (err) {
				say("userptr 0x%llx +%llu -> va 0x%llx: map %d\n", op.userptr, op.range, op.addr, err);
				err = ENOMEM;
			}
			boPut(bo);
			return err;
		}
		case DRM_XE_VM_BIND_OP_UNMAP:
			// a range whose PTEs could not all be cleared keeps its bindings, so no BO a PTE still points at is freed
			if (vmUnmap(vm, op.addr, op.range, ctx)) return ENOMEM;
			vmDropBindings(vm, op.addr, op.range, nullptr, ctx);
			return 0;
		case DRM_XE_VM_BIND_OP_UNMAP_ALL: {
			auto *bo = static_cast<XeBo *>(c->bos.get(op.obj));
			if (!bo) return EINVAL;
			// rb7l: the bindings made through this handle (xe: the object's VMAs, one handle per object there). WAS every
			// binding of the BO: WindowServer's second import of an app's IOSurface backing (a new wrapper made before the
			// old one went) lost its PTEs when the first import was released, and the window read the scratch page's zeros
			// from then on (2026-10-31 svdiag8 T1: one of Minecraft's three buffers black in all 182 probes, image and
			// buffer alike; T3, a fresh import every frame: black nearly always).
			vmDropBindings(vm, 0, 0, bo, ctx, op.obj);
			return 0;
		}
		case DRM_XE_VM_BIND_OP_PREFETCH:
			// rb7r: no migration is done, but PREFETCH is the op xe gates on the purge state of the BOs in its range
			// (vma_lock_and_validate check_purged): DONTNEED EBUSY, purged EINVAL
			for (uint32_t k = 0; k < vm->nBinds; k++) {
				const XeBinding &b = vm->binds[k];
				if (!b.bo || !b.bo->purgeTrack || b.addr >= op.addr + op.range || op.addr >= b.addr + b.range) continue;
				if (b.bo->purgeState == kMadvDontneed) { gPurgeSt.bindBusy++; return EBUSY; }
				if (b.bo->purgeState == kMadvPurged) { gPurgeSt.bindPurged++; return EINVAL; }
			}
			return 0;
		default:
			return EINVAL;
	}
}

XeClientState *xeClientCreate(void *task) {
	// rb7t: boot-arg intelxe_fds=1: object fds on from the first client (the device node needs devfs, which may not have
	// been up when IntelXe started); tried again with the next client if the node could not be made
	if (gFdsBoot == 1 && !xeFdEnable()) {
		gFds = 1;
		gFdsBoot = 2;
	}
	auto *c = static_cast<XeClientState *>(IOMallocZero(sizeof(XeClientState)));
	if (!c) return nullptr;
	c->task = task;
	c->pid = proc_selfpid();   // IOServiceOpen runs in the opening process
	proc_selfname(c->name, sizeof(c->name));
	c->lock = IOLockAlloc();
	if (!c->lock) {
		IOFree(c, sizeof(*c));
		return nullptr;
	}
	IOLockLock(gClientsLock);
	c->next = gClients;
	gClients = c;
	IOLockUnlock(gClientsLock);
	return c;
}

// The user client counts every thread that uses the state (xeClientEnter/Leave around xeIoctl and xeClientMemory);
// the destroy waits until none is left, waking any that sleep in a wait (they return EINTR once dying is set).
void xeClientEnter(XeClientState *c) {
	IOLockLock(c->lock);
	c->busy++;
	IOLockUnlock(c->lock);
}

void xeClientLeave(XeClientState *c) {
	IOLockLock(c->lock);
	if (!--c->busy && c->dying) IOLockWakeup(c->lock, &c->busy, false);
	IOLockUnlock(c->lock);
}

void xeClientDestroy(XeClientState *c) {
	IOLockLock(gClientsLock);
	for (XeClientState **pp = &gClients; *pp; pp = &(*pp)->next)
		if (*pp == c) {
			*pp = c->next;
			break;
		}
	if (gSVLayoutReader == c) gSVLayoutReader = nullptr;
	IOLockUnlock(gClientsLock);
	IOLockLock(c->lock);
	c->dying = true;
	const uint64_t t0 = nowNs();
	uint64_t said = t0;
	while (c->busy) {   // wake its sleeping waiters (they see dying and leave); look again every 10 ms
		IOLockUnlock(c->lock);
		wakeAll();
		IOLockLock(c->lock);
		if (!c->busy) break;
		if (nowNs() - said > 5000000000ull) {   // a thread that does not leave keeps the process from exiting: say which
			said = nowNs();
			g2hLog("client %s (pid %d) teardown: %u thread(s) still inside for %llu s, last ioctl 0x%08x\n", c->name, c->pid,
				c->busy, (said - t0) / 1000000000ull, c->lastReq);
		}
		uint64_t deadline;
		clock_interval_to_deadline(10, kMillisecondScale, &deadline);
		IOLockSleepDeadline(c->lock, &c->busy, deadline, THREAD_UNINT);
	}
	ufAsyncRun(c);   // rb8e: its bind user fences: the ready ones written, the rest dropped (their pages unwired)
	for (uint32_t i = 1; i <= c->queues.n; i++)
		if (void *q = c->queues.take(i); XeBindQueue *bq = bindQOf(q)) {
			IOFree(bq, sizeof(*bq));
			gBindQSt.gone++;
		} else if (q) {
			queueClose(static_cast<XeQueue *>(q));
		}
	for (uint32_t i = 1; i <= c->vms.n; i++)
		if (auto *vm = static_cast<XeVm *>(c->vms.take(i))) vmPut(vm);
	// rb7s: the maps first, each off its BO's list (WAS left on it and freed: a BO another client still holds kept pointers
	// to the freed maps, which its next prune, purge or by-handle map walked), while the handles still hold the BOs
	for (uint32_t i = 1; i <= c->maps.n; i++)   // the task's mappings go with the user client (IOUserClient::free)
		if (auto *m = static_cast<XeCpuMap *>(c->maps.take(i))) {
			if (!m->detached && m->bo) {
				IOSimpleLockLock(gBoLock);
				for (XeCpuMap **pp = &m->bo->cpuMaps; *pp; pp = &(*pp)->next)
					if (*pp == m) {
						*pp = m->next;
						break;
					}
				IOSimpleLockUnlock(gBoLock);
			}
			// a BO a mapping may still reach waits for the mapping to go (IOKit drops the user client's mappings after
			// this); the rest goes now
			lingerAdd(m->md, m->held ? m->bo : nullptr);
			IOFree(m, sizeof(*m));
			gStats.cpuMapsGone++;   // counted as gone too, so made - gone is the maps alive (leak check)
		}
	for (uint32_t i = 1; i <= c->bos.n; i++)
		if (auto *bo = static_cast<XeBo *>(c->bos.take(i))) {
			boHandleRelease(bo);   // rb7s: boHandlePut without the table scan (the table is going)
			boPut(bo);
		}
	for (uint32_t i = 1; i <= c->syncs.n; i++)
		if (auto *s = static_cast<XeSyncobj *>(c->syncs.take(i))) syncPut(s);   // rb7t: other holders keep it
	c->queues.free();
	c->vms.free();
	c->bos.free();
	c->syncs.free();
	c->maps.free();
	IOLockUnlock(c->lock);
	IOLockFree(c->lock);
	IOFree(c, sizeof(*c));
}

// ─── CPU maps (xe_gem_mmap_offset, drm_vma_offset_node) ─────────────────────────────────────────────────────────────
// GEM_MMAP_OFFSET of a VRAM BO hands out a cookie per call, not the handle: the cookie names a memory descriptor of the
// BO's VRAM made for that one mapping, which IOConnectMapMemory64 maps (clientMemoryForType). IOConnectUnmapMemory64
// asks clientMemoryForType for the descriptor of the same type again and looks the mapping up among that descriptor's
// own mappings (is_io_connect_unmap_memory_from_task: createMappingInTask with kIOMapReference). With a new descriptor
// per call, as before, the lookup found nothing and the unmap failed unseen (xemac_munmap ignores the result): every
// VRAM mapping stayed in its process until the connection closed, still pointing at VRAM that went back to the
// allocator, and to other processes' BOs, as soon as the handle was closed. One descriptor per map also keeps two maps
// of one BO apart: a descriptor reuses a compatible kIOMapAnywhere mapping of the same task, so the first unmap would
// have taken both down.
// A map is gone when its descriptor's only reference is the map entry's own again (IOMemoryMap::free releases
// fMemory); one never handed out (GEM_MMAP_OFFSET without an mmap) goes with the handle. A map still there when its
// handle is closed stays, without the BO, until it is unmapped (ANV unmaps before it closes; a Linux mmap would keep the
// GEM object, which is not done here so that a mapping IOKit kept longer than expected cannot hold VRAM).
// The BO's `mapped` stays sticky: a BO that was moved out of the window after its maps went could not always come back
// (a full window), and xe would then place it in system memory, which a move here does not do.
static constexpr uint32_t kMapCookie = 1u << 30;   // IOConnectMapMemory64 types from here on are cookies, below handles

// The BAR2 range(s) behind bo's VRAM in the window: one range per block (xe_ttm_vram_mgr: a BO is a list of blocks).
static IOMemoryDescriptor *boVramDesc(const XeBo *bo) {
	const uint64_t bar = gDevVram()->getPhysicalAddress();
	if (bo->contig) return IOMemoryDescriptor::withPhysicalAddress(bar + bo->dpa, bo->size, kIODirectionInOut);
	uint32_t n = 0;
	for (VBlock *b = bo->blocks; b; b = b->link) n++;
	auto *r = static_cast<IOAddressRange *>(IOMalloc(n * sizeof(IOAddressRange)));
	if (!r) return nullptr;
	uint32_t i = 0;
	uint64_t left = bo->size;
	for (VBlock *b = bo->blocks; b && left; b = b->link, i++) {
		r[i].address = bar + b->off;
		r[i].length = vblockSize(b) < left ? vblockSize(b) : left;
		left -= r[i].length;
	}
	IOMemoryDescriptor *m = IOMemoryDescriptor::withAddressRanges(r, i, kIODirectionInOut | kIOMemoryTypePhysical64, nullptr);
	IOFree(r, n * sizeof(IOAddressRange));
	return m;
}

// The mapping is gone (or never made), and the map is off its BO's list: the map and its BO reference go.
static void mapFree(XeClientState *c, XeCpuMap *m) {
	c->maps.take(m->idx);
	m->md->release();
	XeBo *bo = m->held ? m->bo : nullptr;
	IOFree(m, sizeof(*m));
	gStats.cpuMapsGone++;
	boPut(bo);
}

// c->lock. Drops c's maps of bo that are gone; closing: c's last handle of bo is being closed, so c's maps never handed
// out go too and its others are detached from the BO. Returns c's maps left (detached ones if closing).
// rb7s: c's own maps only. WAS every map on bo->cpuMaps, freed through c's table: with a BO shared by two clients, the
// other client's maps were taken out of c's table by index (another map of c's, or nothing) and freed under it. The
// list is gBoLock's (other clients add to it under their own locks); the maps are unlinked under it and freed after.
static uint32_t boMapsPrune(XeClientState *c, XeBo *bo, bool closing) {
	uint32_t left = 0;
	XeCpuMap *drop = nullptr;
	IOSimpleLockLock(gBoLock);
	for (XeCpuMap **pp = &bo->cpuMaps; *pp;) {
		XeCpuMap *m = *pp;
		if (m->owner != c) {
			pp = &m->next;
			continue;
		}
		if (m->used ? m->md->getRetainCount() > 1 : !closing) {
			left++;
			if (closing) {   // still mapped: detached from the handle, the map keeps the BO (and its VRAM) alive
				*pp = m->next;
				m->detached = true;
				m->next = nullptr;
			} else {
				pp = &m->next;
			}
			continue;
		}
		*pp = m->next;
		m->next = drop;
		drop = m;
	}
	IOSimpleLockUnlock(gBoLock);
	while (XeCpuMap *m = drop) {
		drop = m->next;
		mapFree(c, m);
	}
	return left;
}

// c->lock. Detached maps (closed handles) whose mappings have gone since.
static void clientMapsPrune(XeClientState *c) {
	if (!c->orphanMaps) return;
	uint32_t left = 0;
	for (uint32_t i = 1; i <= c->maps.n; i++) {
		auto *m = static_cast<XeCpuMap *>(c->maps.get(i));
		if (!m || !m->detached) continue;
		if (m->md->getRetainCount() > 1) left++;
		else mapFree(c, m);
	}
	c->orphanMaps = left;
}

// c->lock. A new CPU map of bo (in the window, cleared): its cookie, 0 if no memory.
static uint32_t boMapNew(XeClientState *c, XeBo *bo) {
	IOMemoryDescriptor *md = boVramDesc(bo);
	auto *m = md ? static_cast<XeCpuMap *>(IOMallocZero(sizeof(XeCpuMap))) : nullptr;
	const uint32_t idx = m ? c->maps.add(m) : 0;
	if (!idx || idx >= kMapCookie) {
		if (idx) c->maps.take(idx);
		if (m) IOFree(m, sizeof(*m));
		OSSafeReleaseNULL(md);
		return 0;
	}
	*m = {bo, md, idx, false, nullptr, false, false, false, c};
	IOSimpleLockLock(gBoLock);
	m->next = bo->cpuMaps;
	bo->cpuMaps = m;
	IOSimpleLockUnlock(gBoLock);
	gStats.cpuMapsMade++;
	return kMapCookie | idx;
}

// IOConnectMapMemory64 type = a GEM_MMAP_OFFSET cookie (VRAM BOs: write-combined, *wc, as xe maps VRAM:
// ttm_write_combined; the default for device memory is uncached, and Mesa's descriptor and state pools in VRAM were
// written at 56 MB/s, which held the accelerated desktop at 20-50 fps, 2026-10-28), or a GEM handle: system BOs map
// their own pages; a VRAM BO by handle (no cookie: older user space) gets a descriptor of its own that no unmap finds.
// rb7r: a BO that is not WILLNEED gets no new mapping (xe_gem_object_mmap: DONTNEED EBUSY, purged EINVAL); *why tells
// clientMemoryForType which (kIOReturnBusy / kIOReturnBadArgument). IOConnectUnmapMemory64 asks clientMemoryForType for
// the same type again to find the mapping it removes, so a request while a mapping of it is still there (live) is
// answered as before: refusing it would leave that mapping in place for good. A purged BO has none (purgeBlocked).
static bool mapRefused(const XeBo *bo, bool live, IOReturn *why) {
	if (!bo || !bo->purgeTrack || bo->purgeState == kMadvWillneed || live) return false;
	*why = bo->purgeState == kMadvDontneed ? kIOReturnBusy : kIOReturnBadArgument;
	gPurgeSt.mmapRefused++;
	return true;
}
static bool boMappedNow(const XeBo *bo) {   // a CPU mapping of bo is (or may still be) there, any client's
	bool live = false;
	IOSimpleLockLock(gBoLock);
	for (const XeCpuMap *m = bo->cpuMaps; m && !live; m = m->next) live = m->used && m->md->getRetainCount() > 1;
	IOSimpleLockUnlock(gBoLock);
	return live || (bo->sys && (uint32_t)bo->sys->getRetainCount() > bo->sysRefs);
}

IOMemoryDescriptor *xeClientMemory(XeClientState *c, uint32_t handle, bool *wc, IOReturn *why) {
	if (handle == kStatusPageType) return statusPage();   // INTELXE_CAP_STATUS_PAGE (the caller maps it read-only)
	if (gSuspended) waitUntil(nullptr, INT64_MAX, kWaitKernel, [] { return !gSuspended; });
	IOMemoryDescriptor *m = nullptr;
	IOLockLock(c->lock);
	if (handle >= kMapCookie) {
		auto *cm = c->dying ? nullptr : static_cast<XeCpuMap *>(c->maps.get(handle - kMapCookie));
		if (cm && mapRefused(cm->bo, cm->used && cm->md->getRetainCount() > 1, why)) cm = nullptr;
		if (cm) {
			if (!cm->held && cm->bo && !cm->detached) {   // the mapping holds the BO (its VRAM) until it is unmapped
				boGet(cm->bo);
				cm->held = true;
			}
			cm->used = true;
			cm->md->retain();
			m = cm->md;
			*wc = true;
		}
		IOLockUnlock(c->lock);
		return m;
	}
	auto *bo = c->dying ? nullptr : static_cast<XeBo *>(c->bos.get(handle));
	if (bo && mapRefused(bo, boMappedNow(bo), why)) bo = nullptr;
	while (bo && bo->vram) {
		if (bo->hi)   // looked up again afterwards: the lock is dropped while it moves
			bo = boMakeVisible(c, bo) || c->dying ? nullptr : static_cast<XeBo *>(c->bos.get(handle));
		if (!bo || !bo->vram || !boVisible(bo)) break;
		bo->mapped = true;   // sticky: this mapping is never found again, so the BO never moves again
		__sync_synchronize();   // rb7s: the Dekker pair with moveSetBegin, as in GEM_MMAP_OFFSET
		if (!bo->migrating) break;
		gShareSt.mapWaits++;
		boGet(bo);
		const int werr = waitUntil(c, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
		boPut(bo);
		bo = werr || c->dying ? nullptr : static_cast<XeBo *>(c->bos.get(handle));
	}
	if (bo && bo->vram && boVisible(bo)) {
		// The CPU must not see the VRAM before its clear has landed (a CPU fault waits for the BO's kernel fences).
		boGet(bo);
		const XeFence clr = bo->clear;
		if (waitUntil(c, (int64_t)(nowNs() + 5000000000ull), kWaitClear, [&] { return fenceSignaled(clr); })) {
			IOLockUnlock(c->lock);
			boPut(bo);
			return nullptr;
		}
		// one tracked map per BO for this path, so its unmap (which asks for the same type again) finds the mapping, and
		// the mapping holds the BO like a cookie map does
		XeCpuMap *hm = nullptr;   // rb7s: c's own (the list holds every client's)
		IOSimpleLockLock(gBoLock);
		for (XeCpuMap *x = bo->cpuMaps; x && !hm; x = x->next)
			if (x->byHandle && x->owner == c) hm = x;
		IOSimpleLockUnlock(gBoLock);
		if (!hm) {
			const uint32_t cookie = boMapNew(c, bo);
			hm = cookie ? static_cast<XeCpuMap *>(c->maps.get(cookie - kMapCookie)) : nullptr;
			if (hm) hm->byHandle = true;
		}
		if (hm) {
			if (!hm->held) {
				boGet(bo);
				hm->held = true;
			}
			hm->used = true;
			hm->md->retain();
			m = hm->md;
			*wc = true;
		}
		IOLockUnlock(c->lock);
		boPut(bo);
		return m;
	}
	if (bo && !bo->vram && bo->sys) {
		*wc = false;
		bo->sys->retain();
		m = bo->sys;
	}
	IOLockUnlock(c->lock);
	return m;
}

// The scan-out pinned BOs, for system sleep: a pin outlives the handle and the binding it was taken through (ANV may free
// the buffer, or WindowServer exit, while the plane still scans it), and xeSuspend found BOs only through handles and
// bindings, so such a BO came back from a sleep with whatever D3cold left in its VRAM. xe keeps pinned user BOs on a
// list of their own and saves and restores them like the rest (xe_bo_pin_external -> pinned.late.external,
// xe_bo_evict_all / xe_bo_restore_late). The count changes under gResLock, so a pin racing the last unpin cannot leave
// the table without its BO. gPinOverflow: pins the table had no room for (such a BO is saved only through a handle).
static XeBo *gPinBos[32];
static uint32_t gPinOverflow;
static void pinChange(XeBo *bo, SInt32 d) {
	IOLockLock(gResLock);
	const SInt32 was = OSAddAtomic(d, &bo->pins);
	if (d > 0 && was == 0) {
		bool in = false;
		for (XeBo *&p : gPinBos)
			if (!p) {
				p = bo;
				in = true;
				break;
			}
		if (!in) gPinOverflow++;
	} else if (d < 0 && was == 1) {
		for (XeBo *&p : gPinBos)
			if (p == bo) {
				p = nullptr;
				break;
			}
	}
	IOLockUnlock(gResLock);
}

// Stage 3 of the frame path (NMXAccel flips the plane to a WindowServer frame instead of copying it): the VRAM behind
// a GPU virtual address in one of the task's VMs. The Metal plugin knows its scan-out buffer only by the address
// vkGetBufferDeviceAddress gave it (ANV's GEM handles live in ANV's connection, not in the plugin's), so the bindings of
// every connection the task has open are searched, as xe's own VMA lookup walks a VM's tree (xe_vm_find_overlapping_vma).
// The range must be one VRAM run: a VRAM BO is a single contiguous DPA range here (vramAlloc), unlike xe's TTM buddy
// blocks, which is what lets the display scan it out through a contiguous GGTT range (Linux needs XE_BO_FLAG_SCANOUT /
// DRM_XE_GEM_CREATE_FLAG_SCANOUT to get one). The BO gains a reference so its VRAM stays allocated until unpinned, even
// if ANV frees it or the task dies while the plane still scans it. gClientsLock keeps the client alive (its destroy
// unlinks it under that lock first); each client's lock guards its VMs.
// runs (v2, Linux's way: the display maps the BO page by page through the GGTT, so it need not be one physical run): the
// DPA runs behind [va, va + size) in order, adjacent blocks merged, at most maxRuns; E2BIG with *nRuns = the number
// needed (nothing pinned). runs == nullptr (v1): the range must be one physical run (*dpa).
int xeScanoutPin(void *task, uint64_t va, uint64_t size, uint64_t *dpa, void **pin, IntelXeDpaRun *runs, uint32_t maxRuns,
	uint32_t *nRuns) {
	va &= xePlatformVmSize(gXePlat) - 1;   // vkGetBufferDeviceAddress is canonical (intel_canonical_address); binds are not
	if (!task || !size || (va & 0xfff)) return EINVAL;
	int err = ENOENT;
	IOLockLock(gClientsLock);
	for (XeClientState *c = gClients; c && err == ENOENT; c = c->next) {
		if (c->task != task) continue;
		IOLockLock(c->lock);
		for (uint32_t i = 1; i <= c->vms.n && err == ENOENT; i++) {
			auto *vm = static_cast<XeVm *>(c->vms.get(i));
			for (uint32_t k = 0; vm && k < vm->nBinds; k++) {
				const XeBinding &b = vm->binds[k];
				if (va < b.addr || va + size > b.addr + b.range) continue;
				XeBo *bo = b.bo;
				const uint64_t off = b.off + (va - b.addr);
				if (!bo || !bo->vram || bo->user || (!runs && !bo->contig)) err = ENOTSUP;   // system memory, or scattered for v1
				else if (off + size > bo->size || ((bo->dpa + off) & 0xfff) || (size & 0xfff)) err = EINVAL;
				else if (bo->migrating) err = EBUSY;
				else if (runs) {   // walk the blocks over [off, off + size)
					uint32_t n = 0;
					uint64_t at = 0, left = size, o = off;
					for (VBlock *bl = bo->blocks; bl && left; at += vblockSize(bl), bl = bl->link) {
						if (o >= at + vblockSize(bl)) continue;
						const uint64_t from = bl->off + (o - at), len = at + vblockSize(bl) - o < left ? at + vblockSize(bl) - o : left;
						if (n && n <= maxRuns && runs[n - 1].dpa + runs[n - 1].len == from) runs[n - 1].len += len;
						else {
							if (n < maxRuns) runs[n] = {from, len};
							n++;
						}
						o += len;
						left -= len;
					}
					*nRuns = n;
					if (left) err = EINVAL;
					else if (n > maxRuns) err = E2BIG;
					else {
						boGet(bo);
						pinChange(bo, 1);
						*dpa = runs[0].dpa;
						*pin = bo;
						err = 0;
					}
				} else {
					boGet(bo);
					pinChange(bo, 1);
					*dpa = bo->dpa + off;
					*pin = bo;
					err = 0;
				}
				break;
			}
		}
		IOLockUnlock(c->lock);
	}
	IOLockUnlock(gClientsLock);
	return err;
}

void xeScanoutUnpin(void *pin) {
	auto *bo = static_cast<XeBo *>(pin);
	pinChange(bo, -1);
	boPut(bo);
}


// ─── system sleep (xe_pm_suspend / xe_pm_resume) ──────────────────────────────────────────────────────────────────
// Suspend: user submission held (jobs stay in their FIFOs, fences unsignaled), up to 2 s for what runs to finish (what
// does not is banned), then every VRAM BO copied to system memory by the migrate queue (xe_bo_evict_all: the backups are
// mapped into the migrate VM at kSysVa and MEM_COPY writes them), then the GuC soft reset and the interrupts off.
// "Every VRAM BO": the clients' handles and bindings, the display's buffers, the VRAM-shared IOSurface backings and
// (rb6d) the scan-out pinned BOs, which xe keeps on pinned.late.external. Flat CCS: as xe on Xe2 dGFX
// (xe_migrate_needs_ccs_emit false), no CCS copy; saved through the compressed identity (decompressed), restored with
// uncompressed writes, which leave the lines' CCS state uncompressed. The GGTT is IntelXe's records (ggttMap, rewritten by
// IntelXe::gpuResume) plus IntelXeFB's own ranges (the firmware surface, scan-out slots, cursor), which IntelXeFB rewrites.
// Page tables are not copied: every table has a CPU shadow (PtNode::e), written back on resume. Resume: the device level
// (IntelXe::gpuResume), page tables, queues registered again, VRAM copied back, submission released.
static constexpr uint64_t kSysVa = 2ull << 40;   // 2 TB: the migrate VM's window onto the backups
static XeBo **gPmBos;
static uint32_t gPmN, gPmCap;
static XeBo *gDispBos[64];                       // display VRAM (xeDispVramAlloc), saved like the clients' BOs
static uint32_t gPmGen = 1;
// The last system sleep, for debug.intelxe.stats (the g2h log lines scroll away): BOs saved (of them scan-out pinned
// ones), MB, BOs without a backup, the copy results, and whether the device came back.
static struct { uint32_t suspends, resumes, bos, pinned, lost, restored; uint64_t mb; int saveErr, restoreErr, devErr;
	uint32_t sealWaits, sealTimeouts; } gPmSt;   // rb8d: pmCopyAll windows sealed with an awaited invalidation, timed out

// VRAM-shared IOSurfaces (the registry; its code is after the surface fences)
int gSurfVram = 0;           // debug.intelxe.surfvram: 0 off, 1 apps, 2 apps + WindowServer
int gSurfVramMb = 1024;      // debug.intelxe.surfvram_mb: VRAM all backings together may take
struct SurfVram {
	SurfVram *next;
	uint32_t sid, plane, refs, gen;
	XeBo *bo;
	uint64_t cookie;
	// SURFACE_VRAM2: the layout its creator renders with (modifier 0 = linear, as every backing before it)
	uint64_t modifier;
	uint32_t pitch, cpp, width, height, linPitch;
	bool layoutSet;
	// rb7q diagnostics (debug.intelxe.surfvram_list): who made the backing and when, whether it holds at least one scan-out
	// frame ("big": a WindowServer composite is one), and what happened to it since
	int cpid;
	char cname[24];
	uint64_t bornMs;           // uptime, ms
	bool big;
	uint32_t imports, dOk, dFail, dDemoted, lastFailSt, pOff, pOn, waits, copyErr;
};
static SurfVram *gSV;
static IOLock *gSVLock, *gSVCopyLock;
static uint64_t gSVBytes, gSVCookie;
static uint32_t gSVGen;
static void pmAdd(XeBo *bo) {
	if (!bo || !bo->vram || bo->pmGen == gPmGen) return;
	if (gPmN == gPmCap) {
		const uint32_t nc = gPmCap ? gPmCap * 2 : 256;
		auto **n = static_cast<XeBo **>(IOMalloc(nc * sizeof(XeBo *)));
		if (!n) return;
		if (gPmBos) {
			memcpy(n, gPmBos, gPmN * sizeof(XeBo *));
			IOFree(gPmBos, gPmCap * sizeof(XeBo *));
		}
		gPmBos = n;
		gPmCap = nc;
	}
	bo->pmGen = gPmGen;
	boGet(bo);
	gPmBos[gPmN++] = bo;
}

// MEM_COPY of each saved BO between its VRAM (identity map) and its backup (kSysVa + its offset in the window), toSys:
// VRAM -> backup. The backups are mapped for the duration (the next job of the migrate queue invalidates its TLB).
static int pmCopyAll(bool toSys) {
	if (!gMigQ || !gMigVm) return ENXIO;
	BindCtx ctx = {};
	ctx.clear = {0, ~0u, 0, 0};
	uint64_t va = kSysVa;
	int err = 0;
	for (uint32_t i = 0; i < gPmN && !err; i++) {
		XeBo *bo = gPmBos[i];
		if (!bo->pmBackup) continue;
		if (!bo->pmDma.n && !bo->pmDma.map(bo->pmBackup)) {
			err = ENOMEM;
			break;
		}
		XeBo sys = {};
		sys.size = bo->pmBackup->getLength();
		sys.sys = bo->pmBackup;
		sys.dma = bo->pmDma;   // borrowed: the backup keeps the mapping
		err = vmMap(gMigVm, &sys, 0, sys.size, va, 2, 0, &ctx, true);   // PAT 2: WB, coherent (XE_CACHE_WB)
		bo->pmVa = va;
		va += (sys.size + (2ull << 20) - 1) & ~((2ull << 20) - 1);
	}
	gDev->deviceWmb();   // rb8d: xe_device_wmb as every page-table update (rb8c D4); WAS sfence
	OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gMigVm->ptGen));   // the copies' ring invalidates first
	static constexpr uint32_t kMax = 1500, kChunk = 8u << 20;
	auto *cmd = err ? nullptr : static_cast<uint32_t *>(IOMalloc(kMax * 4));
	if (!err && !cmd) err = ENOMEM;
	XeFence last = {0, ~0u, 0, 0};
	uint32_t n = 0;
	// rb7c: before its contents come back, every restored BO's VRAM is cleared with PVC_MEM_SET, the way a new BO's is
	// (vramClear, xe_migrate_clear on allocation): after a real D3cold the VRAM's flat-CCS state is undefined, and a
	// MEM_COPY write does not put it right, while a MEM_SET does. Seen 30 Oct 15:24: after a 2-minute sleep the cursor
	// buffer, restored by MEM_COPY only, showed one fixed pattern on the cursor plane whatever the CPU stored into it
	// afterwards, while the firmware framebuffer, MEM_SET at resume (rb7) and then written by the CPU the same way,
	// displayed correctly. Same queue, own jobs: the copies below start after these clears have completed.
	for (uint32_t i = 0; i < gPmN && !err && !toSys; i++) {
		XeBo *bo = gPmBos[i];
		if (!bo->pmBackup) continue;
		uint64_t off = 0;
		for (VBlock *b = bo->blocks; b && !err && off < bo->pmBackup->getLength(); b = b->link)
			for (uint64_t o = 0; o < vblockSize(b) && off < bo->pmBackup->getLength() && !err;) {
				uint64_t chunk = vblockSize(b) - o;
				if (chunk > kChunk) chunk = kChunk;
				if (chunk > bo->pmBackup->getLength() - off) chunk = bo->pmBackup->getLength() - off;
				const uint64_t sva = kIdentVa + b->off + o;
				cmd[n++] = (2u << 29 | 0x5bu << 22) | 1u << 17 | (7 - 2);   // PVC_MEM_SET_CMD | MATRIX | len (as vramClear)
				cmd[n++] = 0x1000 - 1;                                     // width (bytes) - 1
				cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;                 // rows - 1
				cmd[n++] = 0x1000 - 1;                                     // pitch - 1
				cmd[n++] = (uint32_t)sva;
				cmd[n++] = (uint32_t)(sva >> 32);
				cmd[n++] = (uint32_t)gXePlat->mocsUcIndex << 3;           // XE2_MEM_SET_MOCS_INDEX: uc_index
				o += chunk;
				off += chunk;
				if (n + 7 > kMax) {
					err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
					n = 0;
				}
			}
	}
	if (!err && n) {
		err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
		n = 0;
	}
	for (uint32_t i = 0; i < gPmN && !err; i++) {
		XeBo *bo = gPmBos[i];
		if (!bo->pmBackup) continue;
		uint64_t off = 0;
		for (VBlock *b = bo->blocks; b && !err && off < bo->pmBackup->getLength(); b = b->link)
			for (uint64_t o = 0; o < vblockSize(b) && off < bo->pmBackup->getLength() && !err;) {
				uint64_t chunk = vblockSize(b) - o;
				if (chunk > kChunk) chunk = kChunk;
				if (chunk > bo->pmBackup->getLength() - off) chunk = bo->pmBackup->getLength() - off;
				// saved decompressed (xe_bo_evict_all -> xe_migrate_copy use_comp_pat), restored uncompressed: a raw save of
				// a compressed BO came back as its compressed bytes with the CCS state of whatever had been written last
				const uint64_t vram = toSys ? identRead(bo->compBound, b->off + o) : kIdentVa + b->off + o;
				const uint64_t sys = bo->pmVa + off, src = toSys ? vram : sys, dst = toSys ? sys : vram;
				cmd[n++] = (2u << 29 | 0x5au << 22) | 1u << 19 | (10 - 2);   // MEM_COPY, page mode
				cmd[n++] = (uint32_t)(chunk / 256) - 1;
				cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;
				cmd[n++] = 0x1000 - 1;
				cmd[n++] = 0x1000 - 1;
				cmd[n++] = (uint32_t)src;
				cmd[n++] = (uint32_t)(src >> 32);
				cmd[n++] = (uint32_t)dst;
				cmd[n++] = (uint32_t)(dst >> 32);
				cmd[n++] = 3u << 28 | 3u << 3;
				o += chunk;
				off += chunk;
				if (n + 10 > kMax) {
					err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
					n = 0;
				}
			}
	}
	if (!err && n) err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
	if (cmd) IOFree(cmd, kMax * 4);
	if (!err && last.slot != ~0u)
		err = waitUntil(nullptr, (int64_t)(nowNs() + 20000000000ull), kWaitKernel, [&] { return fenceSignaled(last); });
	// rb8d: the window sealed as every other migrate window (migWindowSeal): a ranged invalidation issued after the unmap,
	// which the window's tables wait for along with the migrate queue's jobs (a copy that timed out may still run), and
	// which is waited for here (bounded, tlbWait) before the caller lets the backups' pages go (resume frees them at
	// once). WAS deferSeal(gTlbSeq): the last invalidation already issued, complete before the unmap, so the tables went
	// back while the copy engine's page-walk caches could still name them and the backups were freed with the window's
	// translations still in its TLB (xe_pt.c: an unbind's page tables and pages wait for its invalidation and the VM's
	// jobs; xe_bo_evict_all's copies are xe_migrate jobs whose window PTEs live in the job's own batch).
	vmUnmap(gMigVm, kSysVa, va - kSysVa, &ctx);
	if (const uint32_t seq = migWindowSeal(&ctx, gMigVm)) {
		if (!tlbWait(nullptr, seq)) gPmSt.sealTimeouts++;
		gPmSt.sealWaits++;
	}
	return err;
}

static void ptRewrite(const PtNode *n) {
	volatile uint64_t *gpu = vram64(n->dpa);
	for (uint32_t i = 0; i < 512; i++) gpu[i] = n->e[i];
	if (n->child)
		for (uint32_t i = 0; i < 512; i++)
			if (n->child[i]) ptRewrite(n->child[i]);
}

static bool queuesIdle(bool userOnly) {
	bool idle = true;
	IOLockLock(gSchedLock);
	for (uint32_t g = 0; g < 2 && idle; g++)
		for (uint32_t i = 0; i < kMaxGucIds && idle; i++)
			if (XeQueue *q = gQueueByGuc[g][i])
				if (!(userOnly && q->kernel)) idle = q->banned || (int32_t)(seqnoOf(q->slot) - q->submitted) >= 0;
	IOLockUnlock(gSchedLock);
	return idle;
}

// LRCs in VRAM across system sleep (rb7). xe keeps user LRCs on pinned.late (XE_BO_FLAG_PINNED_LATE_RESTORE, saved and
// restored by the copy engine) and kernel ones on pinned.early (restored by the CPU before the GT is resumed,
// xe_bo_restore_early), because the migrate queue's own LRC must be back before it can copy anything. Here every LRC
// is small (at most 32 KB) and in the BAR window, so all of them go the early way: copied out by the CPU after the
// GuC stopped (no context save can follow), back in before any queue is registered again. The ring contents come
// along; the queues were idle, and resume sets HEAD = TAIL anyway. gSchedLock for the table.
static uint32_t gLrcSaved, gLrcSaveFails;
static void lrcSuspendSave() {
	gLrcSaved = gLrcSaveFails = 0;
	if (gLrcVramN && !gDev->l2Flush()) g2hLog("suspend: L2 flush before the LRC copy timed out\n");   // context saves
	IOLockLock(gSchedLock);
	for (uint32_t k = 0; k < 2 * kMaxGucIds; k++) {
		XeQueue *q = gQueueByGuc[k / kMaxGucIds][k % kMaxGucIds];
		if (!q || !q->lrcBlk) continue;
		if (!q->lrcSave) q->lrcSave = static_cast<uint8_t *>(IOMalloc(q->lrcLen));
		if (q->lrcSave) {
			memcpy(q->lrcSave, q->lrcCpu, q->lrcLen);
			gLrcSaved++;
		} else {
			gLrcSaveFails++;
		}
	}
	IOLockUnlock(gSchedLock);
	if (gLrcSaved || gLrcSaveFails) g2hLog("suspend: %u LRCs in VRAM saved, %u without memory\n", gLrcSaved, gLrcSaveFails);
}

// Before page tables and queue registration: every saved LRC back into its VRAM block. A VRAM LRC without a copy
// (no memory at suspend, or made while the device was going down) lost its context image with D3cold: its queue is
// banned (the process sees a lost device), as xe declares a context whose state it could not restore lost.
static void lrcResumeRestore() {
	uint32_t back = 0, lost = 0;
	IOLockLock(gSchedLock);
	for (uint32_t k = 0; k < 2 * kMaxGucIds; k++) {
		XeQueue *q = gQueueByGuc[k / kMaxGucIds][k % kMaxGucIds];
		if (!q || !q->lrcBlk) continue;
		if (q->lrcSave) {
			memcpy(q->lrcCpu, q->lrcSave, q->lrcLen);
			IOFree(q->lrcSave, q->lrcLen);
			q->lrcSave = nullptr;
			back++;
		} else if (!q->banned) {
			queueBan(q);
			lost++;
		}
	}
	IOLockUnlock(gSchedLock);
	if (back || lost) {
		gDev->deviceWmb();
		g2hLog("resume: %u LRCs back in VRAM, %u lost (queue banned)\n", back, lost);
	}
}

int xeSuspend() {
	if (!gDev || !gDev->up) return 0;
	IOLockLock(gSchedLock);
	gSuspended = true;
	if (gDsQ) dsExit(gDsQ, &gStats.dsOtherExits);   // direct submission: out, so the queue drains below
	IOLockUnlock(gSchedLock);
	if (waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [] { return queuesIdle(true); })) {
		IOLockLock(gSchedLock);   // what still runs after 2 s will not finish: banned (xe_guc_submit_stop)
		for (uint32_t g = 0; g < 2; g++)
			for (uint32_t i = 0; i < kMaxGucIds; i++)
				if (XeQueue *q = gQueueByGuc[g][i])
					if (!q->kernel && !q->banned && (int32_t)(seqnoOf(q->slot) - q->submitted) < 0) queueBan(q);
		IOLockUnlock(gSchedLock);
		wakeAll();
	}
	// The media GT's queues are idle now as well (or banned above): they are registered again with the media GuC at
	// resume (xe_guc_submit_start on every GT), not banned; only a media GuC that does not come back bans them.
	// every VRAM BO: the clients' handles and bindings, under every client's lock (no ioctl runs while they are held)
	IOLockLock(gClientsLock);
	for (XeClientState *c = gClients; c; c = c->next) IOLockLock(c->lock);
	gPmGen++;
	gPmN = 0;
	for (XeClientState *c = gClients; c; c = c->next) {
		for (uint32_t h = 1; h <= c->bos.n; h++) pmAdd(static_cast<XeBo *>(c->bos.get(h)));
		for (uint32_t h = 1; h <= c->vms.n; h++)
			if (auto *vm = static_cast<XeVm *>(c->vms.get(h)))
				for (uint32_t k = 0; k < vm->nBinds; k++) pmAdd(vm->binds[k].bo);
	}
	uint32_t pinned = 0;
	IOLockLock(gResLock);   // inside the client locks and gClientsLock: the lock order allows it
	for (XeBo *bo : gDispBos) pmAdd(bo);
	for (XeBo *bo : gPinBos)   // scan-out pins, also those whose handle and binding are gone (pinned.late.external)
		if (bo) {
			const uint32_t before = gPmN;
			pmAdd(bo);
			pinned += gPmN - before;
		}
	IOLockUnlock(gResLock);
	if (gSVLock) { IOLockLock(gSVLock); for (SurfVram *e = gSV; e; e = e->next) pmAdd(e->bo); IOLockUnlock(gSVLock); }
	// rb7t: BOs an object fd holds (a PRIME export whose handles may all be closed, or another process's import still to
	// come): xe saves every BO (xe_bo_evict_all walks the device's BO list); here the record table (gFdLock, a leaf)
	const uint32_t beforeFd = gPmN;
	xeFdForEach(kFdBo, [](void *bo, void *) { pmAdd(static_cast<XeBo *>(bo)); }, nullptr);
	const uint32_t fdOnly = gPmN - beforeFd;
	uint64_t bytes = 0;
	uint32_t lost = 0;
	for (uint32_t i = 0; i < gPmN; i++) {
		XeBo *bo = gPmBos[i];
		bo->pmBackup = gDev->allocSys((bo->size + 0xffff) & ~0xffffull, false);
		if (bo->pmBackup) bytes += bo->pmBackup->getLength();
		else lost++;
	}
	const int err = pmCopyAll(true);
	for (XeClientState *c = gClients; c; c = c->next) IOLockUnlock(c->lock);
	IOLockUnlock(gClientsLock);
	g2hLog("suspend: %u VRAM BOs (%u only through a scan-out pin, %u only through an object fd), %llu MB saved, %u without "
		"memory for a backup, copy %d\n", gPmN, pinned, fdOnly, bytes >> 20, lost, err);
	gPmSt.suspends++;
	gPmSt.bos = gPmN;
	gPmSt.pinned = pinned;
	gPmSt.mb = bytes >> 20;
	gPmSt.lost = lost;
	gPmSt.saveErr = err;
	gPmSt.restored = 0;
	gPmSt.restoreErr = gPmSt.devErr = 0;
	IOLockLock(gSchedLock);
	gResetting = true;   // nothing more goes to the GuC until resume registers the queues again
	IOLockUnlock(gSchedLock);
	IOLockLock(gDevLock);
	gDev->gpuSuspend();
	gDev->mediaDown();
	IOLockUnlock(gDevLock);
	lrcSuspendSave();   // after the GuC soft reset: no context save can follow
	return err;
}

int xeResume() {
	IOLockLock(gDevLock);
	int err = gDev->gpuResume();
	IOLockUnlock(gDevLock);
	gPmSt.resumes++;
	gPmSt.devErr = err;
	if (err) {   // xe_device_declare_wedged: every fence completes, every exec fails, nothing waits for the GPU
		g2hLog("resume: device %d, the GPU stays stopped\n", err);
		IOLockLock(gSchedLock);
		gWedged = true;
		for (uint32_t g = 0; g < 2; g++)
			for (uint32_t i = 0; i < kMaxGucIds; i++)
				if (XeQueue *q = gQueueByGuc[g][i]) queueBan(q);
		gSuspended = false;
		IOLockUnlock(gSchedLock);
		for (uint32_t i = 0; i < gPmN; i++) {
			gPmBos[i]->pmDma.unmap();
			OSSafeReleaseNULL(gPmBos[i]->pmBackup);
			boPut(gPmBos[i]);
		}
		gPmN = 0;
		wakeAll();
		return err;
	}
	lrcResumeRestore();   // before any queue is registered again (and before the migrate queue copies VRAM back)
	// page tables from their shadows: the scratch tables, the migrate VM, every client VM
	for (int lvl = 2; lvl <= 3; lvl++) {
		volatile uint64_t *p = vram64(lvl == 2 ? gScratchL2 : gScratchL3);
		for (int i = 0; i < 512; i++) p[i] = emptyEntry(lvl, true);
	}
	if (gMigVm) ptRewrite(gMigVm->top);
	IOLockLock(gClientsLock);
	for (XeClientState *c = gClients; c; c = c->next) {
		IOLockLock(c->lock);
		for (uint32_t h = 1; h <= c->vms.n; h++)
			if (auto *vm = static_cast<XeVm *>(c->vms.get(h))) ptRewrite(vm->top);
		IOLockUnlock(c->lock);
	}
	IOLockUnlock(gClientsLock);
	asm volatile("sfence" : : : "memory");
	// every queue registered again (idle at suspend: head at tail), banned ones left off: the primary GT's, and the media
	// GT's once its GuC is back (IntelXe::gpuResume loads it when video is on; else its queues cannot run: banned)
	const bool mediaBack = gDev->mediaOn();
	if (mediaBack) gDev->mTlbDone = gDev->tlbSeqno;   // its TLBs start empty too
	IOLockLock(gSchedLock);
	uint32_t mediaRegistered = 0, mediaBanned = 0;
	for (uint32_t k = 0; k < 2 * kMaxGucIds; k++) {
		const uint32_t g = k / kMaxGucIds, i = k % kMaxGucIds;
		XeQueue *q = gQueueByGuc[g][i];
		if (!q) continue;
		if (g && !mediaBack) {
			if (!q->banned) mediaBanned++;
			queueBan(q);
			continue;
		}
		if (g) mediaRegistered++;
		auto *regs = lrcRegs(q);
		regs[CTX_RING_HEAD] = regs[CTX_RING_TAIL] = q->tail;
		lrcSync(q, regs, 0x100);
		const bool off = q->banned || q->closed;
		gGucState[g][q->gucId] = off ? kSchedOff : 0;
		gTdrWait[g][q->gucId] = 0;
		q->schedOffSent = off;
		q->tdr = 0;
		q->wdSince = 0;
		q->flushedGen = q->vm->ptGen - 1;   // the TLBs start empty, but the first batch invalidates anyway
		queueRegister(q, !off);
	}
	gResetting = false;
	IOLockUnlock(gSchedLock);
	if (mediaRegistered || mediaBanned)
		g2hLog("resume: media queues registered again %u, banned %u (media GuC %s)\n", mediaRegistered, mediaBanned,
			mediaBack ? "back" : "not loaded");
	gDev->tlbDone = gDev->tlbSeqno;
	// VRAM back from the backups
	err = pmCopyAll(false);
	// [0, vramUserStart): the GOP's framebuffer and IntelXeFB's fixed cursor range, which belong to no BO and were not
	// saved. D3cold left random data there and random flat-CCS metadata for it, and IntelXeFB writes these ranges only
	// with the CPU through BAR2, which leaves the CCS state as it is: after the 14:49 wake the cursor read back
	// pixelated whatever image it was given. At a cold boot the GOP initializes them; on resume nothing did. A copy-
	// engine clear (PVC_MEM_SET, an uncompressed write, as xe_migrate_clear on Xe2 dGFX, which emits no CCS clear)
	// makes data and CCS state defined before IntelXeFB is told (event 2) and redraws them.
	{
		static_assert((kVChunk << 9) == vramUserStart, "the low range is one 32 MB buddy block");
		VBlock low = {};
		low.order = 9;
		XeBo t = {};
		t.refs = 1;
		t.vram = true;
		t.blocks = &low;
		int cerr = vramClear(&t);
		if (!cerr)
			cerr = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(t.clear); });
		g2hLog("resume: DPA 0-%llu MB (firmware framebuffer, cursor range) cleared by the copy engine: %d\n", vramUserStart >> 20, cerr);
	}
	uint32_t n = gPmN;
	for (uint32_t i = 0; i < n; i++) {
		XeBo *bo = gPmBos[i];
		bo->pmDma.unmap();
		OSSafeReleaseNULL(bo->pmBackup);
		boPut(bo);
	}
	gPmN = 0;
	deferReap();
	IOLockLock(gSchedLock);
	gSuspended = false;
	IOLockUnlock(gSchedLock);
	g2hLog("resume: %u VRAM BOs restored, copy %d\n", n, err);
	gPmSt.restored = n;
	gPmSt.restoreErr = err;
	wakeAll();
	schedKick();
	return err;
}

// ─── display memory for IntelXeFB ("intelxeDispVramAlloc"/"Free", "intelxeGgttAlloc"/"Free", IntelXeClient.cpp) ──────────────────
// Scan-out and cursor buffers: contiguous VRAM inside the BAR window (the display reads it through GGTT PTEs IntelXeFB
// writes, the CPU through BAR2), held by a kernel BO until freed; zeroed by the copy engine if asked (waited for).
int xeDispVramAlloc(uint64_t size, uint32_t flags, uint64_t *dpa, void **cookie) {
	if (!gDev->up) return ENXIO;
	if (!size || size > (1ull << 30)) return EINVAL;
	auto *bo = static_cast<XeBo *>(IOMallocZero(sizeof(XeBo)));
	if (!bo) return ENOMEM;
	bo->size = (size + 0xfff) & ~0xfffull;
	bo->refs = 1;
	IOLockLock(gResLock);
	VBlock *bl = visAlloc(bo->size, false, true, kDispFloor);
	uint64_t total = 0;
	for (VBlock *b = bl; b; b = b->link) total += vblockSize(b);
	gVramUsed += total;
	gVisibleUsed += total;
	IOLockUnlock(gResLock);
	if (!bl && evictWindow(nullptr, bo->size)) {   // the window is full: clients' never-mapped idle BOs make room
		IOLockLock(gResLock);
		if ((bl = visAlloc(bo->size, false, true, kDispFloor)))
			for (VBlock *b = bl; b; b = b->link) {
				gVramUsed += vblockSize(b);
				gVisibleUsed += vblockSize(b);
			}
		IOLockUnlock(gResLock);
	}
	if (!bl) {
		IOFree(bo, sizeof(*bo));
		return ENOSPC;
	}
	bo->vram = bo->contig = true;
	bo->blocks = bl;
	bo->dpa = bl->off;
	bo->clear = {0, ~0u, 0, 0};
	if (flags & 1) {
		int err = gClearOk ? vramClear(bo) : ENXIO;
		if (!err) err = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(bo->clear); });
		if (err) {   // CPU instead
			volatile uint64_t *p = vram64(bo->dpa);
			for (uint64_t i = 0; i < bo->size / 8; i++) p[i] = 0;
		}
	}
	*dpa = bo->dpa;
	*cookie = bo;
	IOLockLock(gResLock);
	for (XeBo *&d : gDispBos)
		if (!d) {
			d = bo;
			break;
		}
	IOLockUnlock(gResLock);
	return 0;
}

void xeDispVramFree(void *cookie) {
	IOLockLock(gResLock);
	for (XeBo *&d : gDispBos)
		if (d == cookie) d = nullptr;
	IOLockUnlock(gResLock);
	boPut(static_cast<XeBo *>(cookie));
}

int xeDispGgttAlloc(uint64_t size, uint64_t align, bool fixed, uint32_t *ggtt) {
	IOLockLock(gResLock);
	const int err = ggttRangeAlloc(size, align, fixed, ggtt);
	IOLockUnlock(gResLock);
	return err;
}

int xeDispGgttFree(uint32_t ggtt) {
	IOLockLock(gResLock);
	const int err = ggttRangeFree(ggtt);
	IOLockUnlock(gResLock);
	return err;
}

// ─── surface fences (uapi/intelxe_drm.h): implicit synchronization of IOSurfaces on the GPU, as dma-resv does it for
// dma-bufs. Per IOSurface ID, the fences of the jobs that use it with their usage (dma_resv_usage: WRITE < READ). Adding
// follows dma_resv_add_fence: an entry that has signaled, or that comes from the same exec queue (fence context) with an
// equal or weaker usage and is not later than the new fence, is replaced in place; otherwise the fence is appended. A
// consumer takes the fences up to a usage (dma_resv_iter: READER = writes, WRITER = writes and reads) into one of its own
// timeline syncobjs, so its exec waits for them in the job FIFO. Signaled fences are pruned whenever a surface is
// touched and a surface left without fences is dropped, so the 1024 entries only ever hold live fences: a full table
// is ENOSPC, never an eviction of something live. gSurfLock is a leaf (fenceSignaled reads memory only).
static constexpr uint32_t kSurfEntries = 1024, kSurfBuckets = 256;
static constexpr uint32_t kSurfFences = INTELXE_SURFACE_FENCE_MAX_WRITE + INTELXE_SURFACE_FENCE_MAX_READ;
enum : uint8_t { kUsageWrite = 1, kUsageRead = 2 };
struct SurfFence { uint32_t slot, gen, seqno; uint8_t usage; };
struct SurfEntry {
	uint32_t id;                // 0: free
	uint16_t next;              // hash chain or free list: index + 1, 0 = end
	uint8_t n;
	SurfFence f[kSurfFences];
};
static SurfEntry *gSurf;
static uint16_t gSurfHash[kSurfBuckets], gSurfFree;
static uint32_t gSurfCount;
static bool gSurfInit;
static IOLock *gSurfLock;

static bool surfSignaled(const SurfFence &f) { return fenceSignaled(XeFence{0, f.slot, f.gen, f.seqno}); }
static uint32_t surfBucket(uint32_t id) { return (id * 2654435761u) >> 24; }   // 8 bits

static void surfPrune(SurfEntry *e) {
	uint8_t keep = 0;
	for (uint8_t i = 0; i < e->n; i++)
		if (!surfSignaled(e->f[i])) e->f[keep++] = e->f[i];
	e->n = keep;
}

// Unlink entry index i from its chain and put it on the free list. gSurfLock.
static void surfDrop(uint32_t i) {
	for (uint16_t *pp = &gSurfHash[surfBucket(gSurf[i].id)]; *pp; pp = &gSurf[*pp - 1].next)
		if (*pp == i + 1) {
			*pp = gSurf[i].next;
			break;
		}
	gSurf[i].id = 0;
	gSurf[i].n = 0;
	gSurf[i].next = gSurfFree;
	gSurfFree = (uint16_t)(i + 1);
	gSurfCount--;
}

// The entry of `id` with its signaled fences pruned (dropped and nullptr if none are left and !create); a new one if
// create (nullptr when the table is full of surfaces with live fences). gSurfLock.
static SurfEntry *surfGet(uint32_t id, bool create) {
	if (!gSurfInit) {
		for (uint32_t i = 0; i < kSurfEntries; i++) gSurf[i].next = (uint16_t)(i + 2 <= kSurfEntries ? i + 2 : 0);
		gSurfFree = 1;
		gSurfInit = true;
	}
	for (uint16_t k = gSurfHash[surfBucket(id)]; k; k = gSurf[k - 1].next) {
		SurfEntry *e = &gSurf[k - 1];
		if (e->id != id) continue;
		surfPrune(e);
		if (!e->n && !create) {
			surfDrop(k - 1);
			return nullptr;
		}
		return e;
	}
	if (!create) return nullptr;
	if (!gSurfFree)   // full: drop every surface whose fences have all signaled
		for (uint32_t i = 0; i < kSurfEntries; i++)
			if (gSurf[i].id) {
				surfPrune(&gSurf[i]);
				if (!gSurf[i].n) surfDrop(i);
			}
	if (!gSurfFree) return nullptr;
	const uint32_t i = gSurfFree - 1u;
	gSurfFree = gSurf[i].next;
	SurfEntry *e = &gSurf[i];
	e->id = id;
	e->n = 0;
	e->next = gSurfHash[surfBucket(id)];
	gSurfHash[surfBucket(id)] = (uint16_t)(i + 1);
	gSurfCount++;
	return e;
}

// dma_resv_add_fence for one fence. gSurfLock; e pruned.
static int surfAdd(SurfEntry *e, const XeFence &x, uint8_t usage) {
	if (x.slot == ~0u) return 0;
	const SurfFence nf = {x.slot, x.gen, x.seqno, usage};
	int at = -1;
	for (uint8_t i = 0; i < e->n;) {
		const SurfFence &o = e->f[i];
		if (o.slot == x.slot && o.gen == x.gen && o.usage >= usage && (int32_t)(x.seqno - o.seqno) >= 0) {
			if (at < 0) {   // replaced in place
				e->f[i] = nf;
				at = i++;
			} else {        // a further entry of the same queue the new fence also supersedes
				e->f[i] = e->f[--e->n];
			}
			continue;
		}
		i++;
	}
	if (at >= 0) return 0;
	uint32_t writes = 0;
	for (uint8_t i = 0; i < e->n; i++) writes += e->f[i].usage == kUsageWrite;
	if (usage == kUsageWrite ? writes >= INTELXE_SURFACE_FENCE_MAX_WRITE : e->n - writes >= INTELXE_SURFACE_FENCE_MAX_READ)
		return ENOSPC;
	e->f[e->n++] = nf;
	return 0;
}

// The fences of `id` up to `usage` (unsignaled), copied out. gSurfLock taken here.
static uint32_t surfCollect(uint32_t id, uint8_t usage, SurfFence *out) {
	IOLockLock(gSurfLock);
	uint32_t n = 0;
	if (SurfEntry *e = surfGet(id, false))
		for (uint8_t i = 0; i < e->n; i++)
			if (e->f[i].usage <= usage) out[n++] = e->f[i];
	IOLockUnlock(gSurfLock);
	return n;
}

// The unsignaled fences a wait on (so, point) stands for (collectDeps without waiting for materialization): EAGAIN if
// the point has no fence yet.
static int syncPointFences(XeSyncobj *so, uint64_t point, DepList &deps) {
	SyncGuard g(so);
	syncValue(so);   // prune signaled history
	XeFence found;
	if (!syncFind(so, point, &found)) return point && syncValue(so) >= point ? 0 : EAGAIN;
	const uint32_t span = point ? 0 : syncBinSpan(so);   // rb7t: see collectDeps
	for (uint32_t k = 0; k < so->n; k++) {
		const XeFence &f = so->f[k];
		if (point && f.point > found.point) continue;
		if (!point && k >= span) break;
		if (!fenceSignaled(f) && !deps.add(f)) return ENOMEM;
	}
	return 0;
}

static int surfaceAttach(XeClientState *c, const drm_intelxe_surface_fence_attach &a) {
	if (!a.surface_id || a.pad || (a.flags != INTELXE_SURFACE_FENCE_WRITE && a.flags != INTELXE_SURFACE_FENCE_READ)) return EINVAL;
	auto *so = static_cast<XeSyncobj *>(c->syncs.get(a.syncobj));
	if (!so) return ENOENT;
	DepList fl;
	int err = syncPointFences(so, a.point, fl);
	if (err || !fl.n) return err;
	const uint8_t usage = a.flags == INTELXE_SURFACE_FENCE_WRITE ? kUsageWrite : kUsageRead;
	IOLockLock(gSurfLock);
	SurfEntry *e = surfGet(a.surface_id, true);
	if (!e) err = ENOSPC;
	for (uint32_t i = 0; e && i < fl.n && !err; i++) err = surfAdd(e, fl.f[i], usage);
	if (e && !e->n) surfDrop((uint32_t)(e - gSurf));
	IOLockUnlock(gSurfLock);
	if (err == ENOSPC) gStats.surfFull++;
	else gStats.surfAttach++;
	return err;
}

static int surfaceImport(XeClientState *c, drm_intelxe_surface_fence_import &m) {
	if (m.flags != INTELXE_SURFACE_FENCE_READER && m.flags != INTELXE_SURFACE_FENCE_WRITER) return EINVAL;
	auto *so = static_cast<XeSyncobj *>(c->syncs.get(m.syncobj));
	if (!so) return ENOENT;
	SurfFence f[kSurfFences];
	const uint32_t n = surfCollect(m.surface_id, m.flags == INTELXE_SURFACE_FENCE_READER ? kUsageWrite : kUsageRead, f);
	SyncGuard g(so);
	const uint64_t base = syncLastSubmitted(so);
	for (uint32_t i = 0; i < n; i++) {
		const uint32_t before = so->n;
		syncAdd(so, XeFence{base + 1 + i, f[i].slot, f[i].gen, f[i].seqno});
		if (so->n == before) return ENOMEM;
	}
	m.count_out = n;
	m.point_out = n ? base + n : 0;
	if (n) {
		gStats.surfImport++;
		wakeAll();   // waiters for the materialization of these points
	}
	return 0;
}

static int surfaceWait(XeClientState *c, const drm_intelxe_surface_fence_wait &w) {
	if (w.flags != INTELXE_SURFACE_FENCE_READER && w.flags != INTELXE_SURFACE_FENCE_WRITER) return EINVAL;
	SurfFence f[kSurfFences];
	const uint32_t n = surfCollect(w.surface_id, w.flags == INTELXE_SURFACE_FENCE_READER ? kUsageWrite : kUsageRead, f);
	return waitUntil(c, w.timeout_ns, kWaitSurface, [&] {
		for (uint32_t i = 0; i < n; i++)
			if (!surfSignaled(f[i])) return false;
		return true;
	});
}

// ─── VRAM-shared IOSurfaces (gpuperf phase 1; uapi/intelxe_drm.h SURFACE_VRAM, uapi/intelxe_surfvram_kabi.h) ─────────────
// One VRAM backing per (IOSurface ID, plane), shared by every client that imports it: a BO of pinned (never moved, never
// evicted), uncompressed VRAM outside the CPU window, never CPU-mapped. The accelerator family owns the association: the
// surface's NMXAccel vidmem adopts the entry ("intelxeSurfAdopt" from NVVidMemory::allocPhysical, family lock held), so the
// family's CPU-lock path (pageoffIfNeeded -> NVResource::pageoff) reaches "intelxeSurfCopy", which copies the VRAM into the
// surface's pages on the copy engine after the surface's GPU writers. References: the vidmem's (one), each lookup's
// (dropped by the ioctl once the client has its handle); the BO itself is referenced by the entry and by client handles,
// so it outlives the entry while a client still holds it. gSVLock is a leaf apart from the VRAM allocator (gResLock).
// (gSurfVram, SurfVram, gSV, gSVLock ...: declared before the suspend code, which saves the backings too)
struct SVStats { uint32_t adopts, refused, budget, imports, importMiss, dirty, dirtyFail, pageoffs, pageons, copyFail, copyTimeouts, pinPageoffs; uint64_t copyUs;
	uint32_t v2Imports, layoutBusy, pageoffsT4, pageonsT4;   // SURFACE_VRAM2 imports, layout EBUSY refusals, Tile4 copies
	uint32_t demotedMarks;      // rb7f: SURFACE_DIRTY marks answered "demoted" (the writer copies out itself)
	uint32_t sameHandle; };     // rb7m: imports answered with the client's existing handle (SAME_HANDLE)
static SVStats gSVSt;
// ─── rb7q: boot-time surfvram diagnostics and the WindowServer full-screen rule ────────────────────────────────────────
// debug.intelxe.surfvram=2 from boot corrupted the screen from the lock screen on (2026-11-03), never when it was set after
// login. Leading hypothesis (docs/reports/b580-surfvram-boot-20261103.md, to be confirmed by surfvram_list): the difference
// is WindowServer's surfaces made while it starts. Its display composites (2560x1440 BGRA, the first textures it makes,
// before anything presents or CPU-locks them) are wrapped as writers with the switch already at 2, and NMXAccel's guard
// (an already-wired vidmem answers NMX_SV_OTHER_BACKING: "existing scan-out allocations stay private") does not hold for
// a vidmem nobody has wired yet, so they get VRAM-shared backings - which stay with the surface for its life
// (NVVidMemory::fIntelXe). After login the composites are wired or cached as private images when the switch goes to 2.
//   debug.intelxe.surfvram_wsfull (boot-arg intelxe_svwsfull=N), default 1: a SURFACE_VRAM(2) create from WindowServer for a
//     surface of at least one scan-out frame (IntelXeFB's mode, width x 4 x height) is made an import: an existing backing
//     (a full-screen app's own drawable) is still shared, none is made. 0: as rb7p (WindowServer makes them).
//   debug.intelxe.surfvram_list: every live backing with its creator, age, size class and counters, and the event ring.
int gSurfVramWsFull = 1;
struct SvCreator { thread_t th; int pid; char name[24]; };
static SvCreator gSVCreating[8];                 // gSVLock: the SURFACE_VRAM creates in flight (xeSurfAdopt names them)
static struct { uint32_t wsCreates, wsBig, wsBigImportOnly, wsBigImported, wsBigPrivate, wsBigMade, frameUnknown;
	uint32_t dirtyBy[11];                          // SURFACE_DIRTY failures by NMX_SV_* status; [10]: the call itself failed
	uint32_t bigDirtyFail, bigDemoted, bigPageoffs;
	uint32_t adoptNoDev, adoptClearFail, copyNoMig, copyLate;
	uint64_t wsFirstMs, firstAdoptMs; } gSVQ;
static uint32_t gSVFrameW, gSVFrameH, gSVFramePitch;
static uint64_t gSVFrameBytes, gSVFrameAtNs;
// event ring (debug.intelxe.surfvram_list): creates, adoptions, releases, and per backing the 1st, 2nd, 4th, 8th ... failed
// mark, demoted mark and page-off, so a boot's first minute can be read back after login
struct SvEv { uint64_t ms; uint32_t sid, kb, a, b; int pid; char kind; char name[15]; };
static SvEv gSVEv[256], gSVEvFirst[64];   // the ring, and the boot's first 64 events (never overwritten)
static uint32_t gSVEvN;
static IOSimpleLock *gSVEvLock;
static bool svPow2(uint32_t n) { return n && !(n & (n - 1)); }
static uint64_t svMs();
static void svEv(char kind, uint32_t sid, uint32_t kb, uint32_t a, uint32_t b, int pid, const char *name) {
	if (!gSVEvLock) return;
	IOSimpleLockLock(gSVEvLock);
	const uint32_t i = gSVEvN++;
	SvEv &e = gSVEv[i & 255];
	e.ms = svMs(); e.sid = sid; e.kb = kb; e.a = a; e.b = b; e.pid = pid; e.kind = kind;
	strlcpy(e.name, name ? name : "", sizeof(e.name));
	if (i < 64) gSVEvFirst[i] = e;
	IOSimpleLockUnlock(gSVEvLock);
}
// 4 TB. It was 3 TB, kMoveVa's address too, under another lock (gSVCopyLock, gMoveLock): a surface copy and a move to
// system memory at the same time rewrote each other's PTEs, and either copy could then land in the other's pages.
static constexpr uint64_t kSurfVa = 4ull << 40;   // the migrate VM's window onto a surface's pages during a copy

static uint64_t svMs() { return nowNs() / 1000000ull; }
static bool svInit() {
	if (!gSVEvLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSVEvLock)) IOSimpleLockFree(l); }
	if (!gSVLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSVLock)) IOLockFree(l); }
	if (!gSVCopyLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSVCopyLock)) IOLockFree(l); }
	return gSVLock && gSVCopyLock;
}
static void svPutLocked(SurfVram *e, XeBo **drop) {   // gSVLock: the last reference unlinks it; its BO ref goes after unlock
	if (--e->refs) return;
	for (SurfVram **pp = &gSV; *pp; pp = &(*pp)->next)
		if (*pp == e) { *pp = e->next; break; }
	gSVBytes -= e->bo->size;
	*drop = e->bo;
	svEv('R', e->sid, (uint32_t)(e->bo->size >> 10), (uint32_t)(svMs() - e->bornMs), e->dFail, e->cpid, e->cname);
	IOFree(e, sizeof(*e));
}
// NMXAccel -> IntelXe: the entry of (sid, plane), made if absent (pinned VRAM, cleared); one reference for the caller.
int xeSurfAdopt(IntelXeSurfAdopt *a) {
	if (!a || a->version != INTELXE_SV_KABI_VERSION || !a->sid || !a->size || !svInit()) return EINVAL;
	if (!gDev || !gDev->up || !gClearOk) { gSVQ.adoptNoDev++; svEv('X', a->sid, (uint32_t)(a->size >> 10), ENXIO, 0, 0, "no device"); return ENXIO; }
	IOLockLock(gSVLock);
	for (SurfVram *e = gSV; e; e = e->next)
		if (e->sid == a->sid && e->plane == a->plane) {
			if (e->bo->size < a->size) break;   // grown: refused (the family makes a new resource for a new size)
			e->refs++;
			a->entry = e; a->bytes = e->bo->size; a->cookie = e->cookie;
			IOLockUnlock(gSVLock);
			return 0;
		}
	const uint64_t size = (a->size + 0xffff) & ~0xffffull;
	if (gSVBytes + size > (uint64_t)(gSurfVramMb > 0 ? gSurfVramMb : 0) << 20) {
		gSVSt.budget++;
		IOLockUnlock(gSVLock);
		return ENOSPC;
	}
	gSVBytes += size;   // reserved while the VRAM is made (the lock is dropped)
	IOLockUnlock(gSVLock);
	auto *bo = static_cast<XeBo *>(IOMallocZero(sizeof(XeBo)));
	auto *e = static_cast<SurfVram *>(IOMallocZero(sizeof(SurfVram)));
	int err = (bo && e) ? 0 : ENOMEM;
	if (!err) {
		bo->size = size; bo->refs = 1; bo->noComp = true; bo->noMove = true; bo->pins = 1; bo->placement = 2;
		bo->caching = DRM_XE_GEM_CPU_CACHING_WC;
		if (!vramAllocBo(bo, size, false, true) && !vramAllocBo(bo, size, false, false)) err = ENOSPC;
		else if (bo->vram && !bo->hi) { vramFree(bo); bo->vram = false; err = ENOSPC; }   // never in the CPU window
	}
	if (!err && (err = vramClear(bo)) == 0)   // a backing never shows another process's old VRAM
		err = waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(bo->clear); });
	IOLockLock(gSVLock);
	if (err) {
		gSVBytes -= size;
		gSVSt.refused++;
		if (err != ENOMEM && err != ENOSPC) gSVQ.adoptClearFail++;
		IOLockUnlock(gSVLock);
		if (bo) { if (bo->vram) vramFree(bo); IOFree(bo, sizeof(*bo)); }
		if (e) IOFree(e, sizeof(*e));
		return err;
	}
	e->sid = a->sid; e->plane = a->plane; e->refs = 1; e->bo = bo; e->gen = ++gSVGen; e->cookie = ++gSVCookie | 1ull << 63;
	e->bornMs = svMs(); e->cpid = -1; strlcpy(e->cname, "?", sizeof(e->cname));
	{   // rb7q: the SURFACE_VRAM create this adoption runs under (NMXAccel calls back on the ioctl's thread)
		const thread_t th = current_thread();
		for (SvCreator &k : gSVCreating) if (k.th == th) { e->cpid = k.pid; strlcpy(e->cname, k.name, sizeof(e->cname)); break; }
	}
	e->big = gSVFrameBytes && size >= gSVFrameBytes;
	if (!gSVQ.firstAdoptMs) gSVQ.firstAdoptMs = e->bornMs;
	e->next = gSV; gSV = e;
	gSVSt.adopts++;
	if (e->big && !strncmp(e->cname, "WindowServer", sizeof(e->cname))) gSVQ.wsBigMade++;
	IOLockUnlock(gSVLock);
	svEv('A', e->sid, (uint32_t)(size >> 10), e->big, gSVGen, e->cpid, e->cname);
	a->entry = e; a->bytes = size; a->cookie = e->cookie;
	say("surfvram: surface %u plane %u -> %llu KB of VRAM at DPA 0x%llx%s (%llu MB in %s)\n", a->sid, a->plane, size >> 10,
		bo->dpa, bo->contig ? " (one run)" : "", gSVBytes >> 20, "backings");
	return 0;
}
static bool svKnown(const void *entry) {   // gSVLock
	for (SurfVram *e = gSV; e; e = e->next) if (e == entry) return true;
	return false;
}
int xeSurfRef(IntelXeSurfAdopt *a, bool get) {
	if (!a || a->version != INTELXE_SV_KABI_VERSION || !a->entry || !svInit()) return EINVAL;
	XeBo *drop = nullptr;
	IOLockLock(gSVLock);
	if (!svKnown(a->entry)) { IOLockUnlock(gSVLock); return ENOENT; }
	auto *e = static_cast<SurfVram *>(a->entry);
	if (get) { e->refs++; a->bytes = e->bo->size; a->cookie = e->cookie; }
	else svPutLocked(e, &drop);
	IOLockUnlock(gSVLock);
	if (drop) boPut(drop);
	return 0;
}
// Copy-engine copy between the entry's VRAM and md (the surface's pages), after the surface's GPU users have finished:
// towards the pages the writers (READER), towards VRAM writers and readers (WRITER). gSVCopyLock serializes the window.
// The copy between bo's VRAM and md's pages (both from offset 0, len bytes): after the surface's GPU users when sid is
// nonzero (towards the pages its writers, towards VRAM writers and readers), on the migrate queue, waited for.
// A Tile4 backing (SurfVram layout): the copy detiles (towards the pages) or tiles (towards VRAM) on the copy engine.
struct SvLayout { uint64_t modifier; uint32_t pitch, cpp, width, height, linPitch; };
static constexpr uint64_t kSurfTiledVa = 5ull << 40;   // the migrate VM's window onto a tiled backing during a copy
// XY_BLOCK_COPY_BLT (Xe2, 22 dwords; Mesa's genxml xe2.xml, blorp_xy_block_copy_blt): the whole width x height image
// between a Tile4 surface (pitch in dwords) and a linear one (pitch in bytes). Both 2D, one level, HALIGN_32 (128 bytes
// at 32 bpp, isl_gfx125_choose_image_alignment_el for CCS-capable and linear color) / HALIGN_16 at 64 bpp, VALIGN_4,
// MOCS index 3 (the migrate queue's, as MEM_COPY uses), uncompressed. src/dst: GPU addresses in the migrate VM.
static uint32_t svBlockCopy(uint32_t *cmd, const SvLayout &l, bool toSys, uint64_t tiled, uint64_t linear) {
	const uint32_t bpp = l.cpp == 8 ? 3u : 2u;                // XY_BPP_64_BIT / XY_BPP_32_BIT
	const uint32_t halign = l.cpp == 8 ? 0u : 1u;             // 16 or 32 elements: 128 bytes
	const uint32_t tPitch = l.pitch / 4 - 1, lPitch = l.linPitch - 1;
	const uint32_t dstPitch = toSys ? lPitch : tPitch, srcPitch = toSys ? tPitch : lPitch;
	const uint32_t dstTile = toSys ? 0u : 2u, srcTile = toSys ? 2u : 0u;   // XY_TILE_LINEAR / XY_TILE_4
	const uint64_t dst = toSys ? linear : tiled, src = toSys ? tiled : linear;
	const uint32_t dstMem = toSys ? 1u : 0u, srcMem = toSys ? 0u : 1u;      // XY_MEM_SYSTEM / XY_MEM_LOCAL
	const uint32_t dims = 1u << 29 | (l.width - 1) << 14 | (l.height - 1);  // 2D, width - 1, height - 1
	const uint32_t align = halign | 1u << 3;                                // VALIGN_4
	uint32_t n = 0;
	cmd[n++] = 2u << 29 | 0x41u << 22 | bpp << 19 | (22 - 2);
	cmd[n++] = dstTile << 30 | (uint32_t)gXePlat->mocsUcIndex << 24 | dstPitch;   // MOCS index: uc_index
	cmd[n++] = 0;                                     // dst X1, Y1
	cmd[n++] = l.height << 16 | l.width;              // dst X2, Y2 (exclusive)
	cmd[n++] = (uint32_t)dst; cmd[n++] = (uint32_t)(dst >> 32);
	cmd[n++] = dstMem << 31;                          // dst X/Y offset 0, target memory
	cmd[n++] = 0;                                     // src X1, Y1
	cmd[n++] = srcTile << 30 | (uint32_t)gXePlat->mocsUcIndex << 24 | srcPitch;
	cmd[n++] = (uint32_t)src; cmd[n++] = (uint32_t)(src >> 32);
	cmd[n++] = srcMem << 31;
	cmd[n++] = 0; cmd[n++] = 0;                       // src compression format / clear address: none
	cmd[n++] = 0; cmd[n++] = 0;                       // dst compression format / clear address: none
	cmd[n++] = dims;                                  // dst surface: type, width, height
	cmd[n++] = 0;                                     // dst LOD 0, qpitch, depth 1
	cmd[n++] = align;                                 // dst alignment, array index 0
	cmd[n++] = dims;                                  // src surface
	cmd[n++] = 0;
	cmd[n++] = align;
	return n;
}
static int svCopyBo(XeBo *bo, uint32_t sid, IOMemoryDescriptor *md, uint64_t reqLen, bool toSys, uint32_t timeoutMs, uint64_t *usec,
	const SvLayout *layout = nullptr, bool compSrc = false) {
	if (!gMigQ || !gMigVm) { gSVQ.copyNoMig++; return ENXIO; }
	const bool tiled = layout && layout->modifier == I915_FORMAT_MOD_4_TILED;
	if (tiled) {   // the pages hold the linear image: linear_pitch x height bytes of them
		const uint64_t need = (uint64_t)layout->linPitch * layout->height;
		if (need > md->getLength()) return EINVAL;
		reqLen = need;
	}
	const uint64_t t0 = nowNs();
	uint64_t len = tiled ? (reqLen + 0xfff) & ~0xfffull : reqLen & ~0xfffull;
	if (len > md->getLength()) len = md->getLength() & ~0xfffull;
	if (!tiled && len > bo->size) len = bo->size;
	const int64_t deadline = (int64_t)(t0 + (uint64_t)(timeoutMs ? timeoutMs : 2000) * 1000000ull);
	SurfFence f[kSurfFences];
	const uint32_t nf = sid ? surfCollect(sid, toSys ? kUsageWrite : kUsageRead, f) : 0;
	int err = waitUntil(nullptr, deadline, kWaitSurface, [&] {
		for (uint32_t i = 0; i < nf; i++) if (!surfSignaled(f[i])) return false;
		return true;
	});
	if (err == ETIME) { gSVSt.copyTimeouts++; err = ETIMEDOUT; }
	IOLockLock(gSVCopyLock);
	if (!err && !migLateDone()) { err = EBUSY; gSVQ.copyLate++; }
	XeBo sys = {};
	BindCtx ctx = {};
	ctx.clear = {0, ~0u, 0, 0};
	bool mapped = false, late = false, tmapped = false;
	if (!err && len) {
		sys.size = len;
		if (!sys.dma.map(md)) err = ENOMEM;
		else if ((err = vmMap(gMigVm, &sys, 0, len, kSurfVa, 2, 0, &ctx, true)) == 0) mapped = true;   // PAT 2: WB, coherent
	}
	if (!err && len && tiled) {   // the backing as one range (it may be in runs): PAT 0, WB, uncompressed
		if ((err = vmMap(gMigVm, bo, 0, bo->size, kSurfTiledVa, 0, 0, &ctx, true)) == 0) tmapped = true;
	}
	if (!err && len) {
		asm volatile("sfence" : : : "memory");
		OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gMigVm->ptGen));   // the next migrate job invalidates first
		static constexpr uint32_t kMax = 1500, kChunk = 8u << 20;
		XeFence last = {0, ~0u, 0, 0};
		uint32_t n = 0;
		uint64_t off = 0;
		uint32_t tcmd[24];
		if (tiled) {   // one XY_BLOCK_COPY_BLT of the whole image, detiling towards the pages, tiling towards VRAM
			n = svBlockCopy(tcmd, *layout, toSys, kSurfTiledVa, kSurfVa);
			err = queueSubmitKernel(gMigQ, 0, &last, tcmd, n);
			n = 0;
			off = len;
		}
		auto *cmd = tiled ? nullptr : static_cast<uint32_t *>(IOMalloc(kMax * 4));
		if (!tiled && !cmd) err = ENOMEM;
		for (VBlock *b = bo->blocks; cmd && b && !err && off < len; b = b->link)
			for (uint64_t o = 0; o < vblockSize(b) && off < len && !err;) {
				uint64_t chunk = vblockSize(b) - o;
				if (chunk > kChunk) chunk = kChunk;
				if (chunk > len - off) chunk = len - off;
				// compSrc (debug.intelxe.dispcheck only): read VRAM through the compressed identity, i.e. decompressed
				const uint64_t vram = (toSys && compSrc ? kIdentCompVa : kIdentVa) + b->off + o, pg = kSurfVa + off,
					src = toSys ? vram : pg, dst = toSys ? pg : vram;
				cmd[n++] = (2u << 29 | 0x5au << 22) | 1u << 19 | (10 - 2);   // MEM_COPY, page mode (256-byte units)
				cmd[n++] = (uint32_t)(chunk / 256) - 1;
				cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;
				cmd[n++] = 0x1000 - 1;
				cmd[n++] = 0x1000 - 1;
				cmd[n++] = (uint32_t)src;
				cmd[n++] = (uint32_t)(src >> 32);
				cmd[n++] = (uint32_t)dst;
				cmd[n++] = (uint32_t)(dst >> 32);
				cmd[n++] = 3u << 28 | 3u << 3;
				o += chunk;
				off += chunk;
				if (n + 10 > kMax || off >= len) {
					err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
					n = 0;
				}
			}
		if (cmd) IOFree(cmd, kMax * 4);
		// waited for even after a failed submit: nothing may still use the pages or the VRAM when they are let go
		if (last.slot != ~0u &&
			waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(last); })) {
			gSVSt.copyTimeouts++;
			err = ETIMEDOUT;
			late = true;
			md->retain();   // never released (with the leaked IOVA mapping below): the job may still use the pages
			boGet(bo);      // and the VRAM
			gMigLate = last;
			g2hLog("surfvram copy of %llu KB did not complete within 2 s: its memory is kept\n", len >> 10);
		}
	}
	if (tmapped && !late) vmUnmap(gMigVm, kSurfTiledVa, bo->size, &ctx);   // a late copy keeps it, as the pages
	if (mapped && !late) vmUnmap(gMigVm, kSurfVa, len, &ctx);
	// rb8e: one seal for both windows (the tables wait for the migrate queue's jobs too, as pmCopyAll's), and the pages
	// and the backing's VRAM are let go only once its invalidation has completed (migWindowAwait). WAS sealed without
	// waiting and the DMA unmapped at once, the caller then free to release the pages (NMXAccel's page-offs and page-ons)
	if ((mapped || tmapped) && !late && !migWindowAwait(migWindowSeal(&ctx, gMigVm), kMigWinSurf)) {
		late = true;    // kept as for a copy that timed out: the pages, their IOVA mapping and the VRAM
		md->retain();
		boGet(bo);
	}
	// a copy that timed out may still run: its pages stay mapped for the device (the IOVA mapping is leaked, not freed)
	if (!late) sys.dma.unmap();
	IOLockUnlock(gSVCopyLock);
	*usec = (nowNs() - t0) / 1000;
	return err;
}
// debug.intelxe.dispcheck (rb7d, diagnostics): every display buffer (gDispBos, e.g. IntelXeFB's cursor image) read back four
// ways and compared, to tell where an image the display shows wrong went wrong:
//   bar  the CPU through BAR2 (the host path, the way IntelXeFB writes it)
//   gpu  the copy engine through the plain identity (PAT 3: the bytes as stored, CCS state ignored)
//   dec  the copy engine through the compressed identity (PAT 12: decompressed per the lines' flat-CCS state)
//   bar2 the CPU through BAR2 again after an L2 flush (XE2_GLOBAL_INVAL)
// Per way: a word-wise FNV-1a checksum (IntelXeFB's pminfo prints the same checksum of the last image it wrote) and the
// first 4 words; then how many 256-byte blocks differ between gpu and dec (lines whose CCS state reads as compressed)
// and between bar and gpu. Reads only; small copy-engine jobs on the migrate queue.
static uint32_t fnvWords(const uint32_t *w, size_t n) {
	uint32_t h = 2166136261u;
	for (size_t i = 0; i < n; i++) h = (h ^ w[i]) * 16777619u;
	return h;
}
int xeDispCheck(char *out, size_t len) {
	size_t n = 0;
#define dput(...) do { if (n < len) { const int k_ = snprintf(out + n, len - n, __VA_ARGS__); if (k_ > 0) n = n + (size_t)k_ < len ? n + (size_t)k_ : len - 1; } } while (0)
	if (!gDev || !gDev->up || gDev->suspended || !gMigQ || !svInit()) { dput("device not up\n"); return (int)n; }
	XeBo *list[64];
	uint32_t nb = 0;
	IOLockLock(gResLock);
	for (XeBo *d : gDispBos)
		if (d && nb < 64) {
			boGet(d);
			list[nb++] = d;
		}
	IOLockUnlock(gResLock);
	dput("display buffers: %u\n", nb);
	for (uint32_t i = 0; i < nb; i++) {
		XeBo *bo = list[i];
		const uint64_t sz = bo->size > (1u << 20) ? (1u << 20) : bo->size;   // the first MB is enough (cursor: 256 KB)
		const size_t words = (size_t)(sz / 4);
		IOBufferMemoryDescriptor *m[4] = {};
		for (auto &x : m) x = gDev->allocSys(sz, false);
		if (!m[0] || !m[1] || !m[2] || !m[3]) {
			dput("bo %u: no memory\n", i);
		} else {
			auto *w0 = static_cast<uint32_t *>(m[0]->getBytesNoCopy()), *w1 = static_cast<uint32_t *>(m[1]->getBytesNoCopy());
			auto *w2 = static_cast<uint32_t *>(m[2]->getBytesNoCopy()), *w3 = static_cast<uint32_t *>(m[3]->getBytesNoCopy());
			const bool vis = boVisible(bo) && bo->contig;
			if (vis) {
				volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(vram64(bo->dpa));
				for (size_t k = 0; k < words; k++) w0[k] = v[k];
			}
			uint64_t us = 0;
			const int e1 = svCopyBo(bo, 0, m[1], sz, true, 500, &us);
			flushRange(w1, sz);
			const int e2 = svCopyBo(bo, 0, m[2], sz, true, 500, &us, nullptr, true);
			flushRange(w2, sz);
			const bool fl = gDev->l2Flush();
			if (vis) {
				volatile uint32_t *v = reinterpret_cast<volatile uint32_t *>(vram64(bo->dpa));
				for (size_t k = 0; k < words; k++) w3[k] = v[k];
			}
			uint32_t ccs = 0, hostDiff = 0;
			for (size_t blk = 0; blk < words / 64; blk++) {
				if (memcmp(w1 + blk * 64, w2 + blk * 64, 256)) ccs++;
				if (vis && memcmp(w0 + blk * 64, w1 + blk * 64, 256)) hostDiff++;
			}
			dput("bo %u: DPA 0x%llx size %llu KB%s, checked %llu KB; copy errs %d/%d, L2 flush %s\n", i, bo->dpa, bo->size >> 10,
				vis ? "" : " (not CPU-visible)", sz >> 10, e1, e2, fl ? "ok" : "TIMEOUT");
			const char *nm[4] = {"bar ", "gpu ", "dec ", "bar2"};
			uint32_t *ws[4] = {w0, w1, w2, w3};
			for (int k = 0; k < 4; k++) {
				if ((k == 0 || k == 3) && !vis) continue;
				dput("  %s fnv 0x%08x  %08x %08x %08x %08x\n", nm[k], fnvWords(ws[k], words), ws[k][0], ws[k][1], ws[k][2], ws[k][3]);
			}
			dput("  256-byte blocks: gpu != dec (CCS says compressed) %u, bar != gpu %u, of %zu\n", ccs, hostDiff, words / 64);
		}
		for (auto &x : m) OSSafeReleaseNULL(x);
		boPut(bo);
	}
#undef dput
	return (int)n;
}

// "intelxeVramRead" (rb7e, diagnostics for IntelXeFB's debug.intelxefb_thumb/_crop): len bytes of VRAM at dpa (4 KB aligned)
// copied by the copy engine into dst (kernel memory), for scan-out buffers outside the CPU's BAR2 window (the Metal
// plugin's scan-out BOs live above it). Raw bytes through the plain identity; the surface-copy window and lock.
int xeVramRead(uint64_t dpa, uint64_t len, void *dst) {
	if (!dst || !len || ((dpa | len) & 0xfff) || len > (64ull << 20) || dpa + len > vramEnd) return EINVAL;
	if (!gDev || !gDev->up || gDev->suspended || !gMigQ || !gMigVm || !svInit()) return ENXIO;
	IOBufferMemoryDescriptor *md = gDev->allocSys(len, false);
	if (!md) return ENOMEM;
	IOLockLock(gSVCopyLock);
	int err = migLateDone() ? 0 : EBUSY;
	XeBo sys = {};
	BindCtx ctx = {};
	ctx.clear = {0, ~0u, 0, 0};
	bool mapped = false, late = false;
	if (!err) {
		sys.size = len;
		if (!sys.dma.map(md)) err = ENOMEM;
		else if ((err = vmMap(gMigVm, &sys, 0, len, kSurfVa, 2, 0, &ctx, true)) == 0) mapped = true;   // PAT 2: WB, coherent
	}
	if (!err) {
		asm volatile("sfence" : : : "memory");
		OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&gMigVm->ptGen));   // the next migrate job invalidates first
		static constexpr uint32_t kMax = 1500, kChunk = 8u << 20;
		auto *cmd = static_cast<uint32_t *>(IOMalloc(kMax * 4));
		XeFence last = {0, ~0u, 0, 0};
		uint32_t n = 0;
		if (!cmd) err = ENOMEM;
		for (uint64_t off = 0; cmd && off < len && !err;) {
			const uint64_t chunk = len - off > kChunk ? kChunk : len - off;
			const uint64_t src = kIdentVa + dpa + off, dstVa = kSurfVa + off;
			cmd[n++] = (2u << 29 | 0x5au << 22) | 1u << 19 | (10 - 2);   // MEM_COPY, page mode (256-byte units)
			cmd[n++] = (uint32_t)(chunk / 256) - 1;
			cmd[n++] = (uint32_t)(chunk / 0x1000) - 1;
			cmd[n++] = 0x1000 - 1;
			cmd[n++] = 0x1000 - 1;
			cmd[n++] = (uint32_t)src;
			cmd[n++] = (uint32_t)(src >> 32);
			cmd[n++] = (uint32_t)dstVa;
			cmd[n++] = (uint32_t)(dstVa >> 32);
			cmd[n++] = 3u << 28 | 3u << 3;
			off += chunk;
			if (n + 10 > kMax || off >= len) {
				err = queueSubmitKernel(gMigQ, 0, &last, cmd, n);
				n = 0;
			}
		}
		if (cmd) IOFree(cmd, kMax * 4);
		if (last.slot != ~0u &&
			waitUntil(nullptr, (int64_t)(nowNs() + 2000000000ull), kWaitKernel, [&] { return fenceSignaled(last); })) {
			err = ETIMEDOUT;
			late = true;
			md->retain();   // never released: the job may still write the pages (as svCopyBo)
			gMigLate = last;
			g2hLog("VRAM read of %llu KB did not complete within 2 s: its memory is kept\n", len >> 10);
		}
	}
	// rb8e: the pages (freed below) only once the window's invalidation has completed, as svCopyBo
	if (mapped && !late) {
		vmUnmap(gMigVm, kSurfVa, len, &ctx);
		if (!migWindowAwait(migWindowSeal(&ctx, gMigVm), kMigWinRead)) {
			late = true;
			md->retain();   // never released, with its IOVA mapping (the copy itself completed: the data is good)
		}
	}
	if (!late) sys.dma.unmap();
	IOLockUnlock(gSVCopyLock);
	if (!err) {
		flushRange(md->getBytesNoCopy(), len);
		memcpy(dst, md->getBytesNoCopy(), len);
	}
	md->release();
	return err;
}

// rb7g: a CPU lock of a surface whose pages its GPU writers fill (NMXAccel: a demoted backing) waits for those writes,
// as IOSurfaceLock does on Apple's drivers and a CPU access to a dma-buf waits for its exclusive fences on Linux
// (dma_resv_wait_timeout(DMA_RESV_USAGE_WRITE)). Bounded; the timeout is logged once in a while.
static uint32_t gSvCpuWaits, gSvCpuWaitTimeouts; static uint64_t gSvCpuWaitUs, gSvCpuWaitMaxUs;
int xeSurfWaitWriters(IntelXeSurfWait *w) {
	if (!w || w->version != INTELXE_SV_KABI_VERSION || !w->sid) { if (w) w->err = EINVAL; return EINVAL; }
	SurfFence f[kSurfFences];
	const uint32_t n = surfCollect(w->sid, kUsageWrite, f);
	w->fences = n; w->usec = 0; w->err = 0;
	if (!n) return 0;
	const uint64_t t0 = nowNs();
	const int64_t deadline = (int64_t)(t0 + (uint64_t)(w->timeoutMs ? w->timeoutMs : 1000) * 1000000ull);
	int err = waitUntil(nullptr, deadline, kWaitSurface, [&] {
		for (uint32_t i = 0; i < n; i++) if (!surfSignaled(f[i])) return false;
		return true;
	});
	const uint64_t us = (nowNs() - t0) / 1000;
	w->usec = us;
	gSvCpuWaits++; gSvCpuWaitUs += us; if (us > gSvCpuWaitMaxUs) gSvCpuWaitMaxUs = us;
	if (gSVLock) {   // rb7q: per backing
		IOLockLock(gSVLock);
		for (SurfVram *e = gSV; e; e = e->next) if (e->sid == w->sid) { e->waits++; break; }
		IOLockUnlock(gSVLock);
	}
	if (err == ETIME) {
		err = ETIMEDOUT;
		if (gSvCpuWaitTimeouts++ < 16) g2hLog("CPU lock of surface %u: its GPU writers did not finish in %u ms - read as it is\n", w->sid,
		                                      w->timeoutMs ? w->timeoutMs : 1000);
	}
	w->err = err;
	return err;
}

int xeSurfCopy(IntelXeSurfCopy *q) {
	if (!q || q->version != INTELXE_SV_KABI_VERSION || !q->entry || !q->md || !svInit()) return EINVAL;
	XeBo *bo = nullptr; uint32_t sid = 0;
	SvLayout layout = {};
	IOLockLock(gSVLock);
	if (svKnown(q->entry)) {
		auto *e = static_cast<SurfVram *>(q->entry); bo = e->bo; sid = e->sid; boGet(bo);
		layout = {e->modifier, e->pitch, e->cpp, e->width, e->height, e->linPitch};
	}
	IOLockUnlock(gSVLock);
	if (!bo) return ENOENT;
	uint64_t us = 0;
	const int err = svCopyBo(bo, sid, static_cast<IOMemoryDescriptor *>(q->md), q->len, q->toSys != 0, q->timeoutMs, &us, &layout);
	boPut(bo);
	{   // rb7q: per backing (the entry may have gone meanwhile)
		uint32_t n = 0, kb = 0; bool big = false; int pid = 0; char nm[24] = "";
		IOLockLock(gSVLock);
		if (svKnown(q->entry)) {
			auto *e = static_cast<SurfVram *>(q->entry);
			if (err) e->copyErr++;
			else if (q->toSys) n = ++e->pOff; else e->pOn++;
			big = e->big; kb = (uint32_t)(e->bo->size >> 10); pid = e->cpid; strlcpy(nm, e->cname, sizeof(nm));
			if (big && q->toSys && !err) gSVQ.bigPageoffs++;
		}
		IOLockUnlock(gSVLock);
		if (err || (n && svPow2(n) && (big || n <= 4))) svEv(err ? 'E' : 'P', sid, kb, err ? (uint32_t)err : n, q->toSys, pid, nm);
	}
	q->usec = us;
	q->err = err;
	if (err) gSVSt.copyFail++;
	else {
		if (q->toSys) gSVSt.pageoffs++; else gSVSt.pageons++;
		if (layout.modifier == I915_FORMAT_MOD_4_TILED) { if (q->toSys) gSVSt.pageoffsT4++; else gSVSt.pageonsT4++; }
		gSVSt.copyUs += us;
	}
	return err;
}
// gpuperf phase 4 ("intelxeCopyPinToPages"): a scan-out pin's VRAM (the BO xeScanoutPin pinned; the caller holds the pin, or
// a reference from "intelxePinGet") into an IOSurface's pages, for WindowServer composites whose system-memory readback
// was dropped: NMXAccel pages them off when the CPU locks them or a frame is copied instead of flipped. The caller has
// waited for the composite's GPU work (its mark).
int xeCopyPinToPages(IntelXePinCopy *q) {
	if (!q || q->version != INTELXE_SV_KABI_VERSION || !q->pin || !q->md || !svInit()) return EINVAL;
	uint64_t us = 0;
	const int err = svCopyBo(static_cast<XeBo *>(q->pin), 0, static_cast<IOMemoryDescriptor *>(q->md), q->len, true, q->timeoutMs, &us);
	q->usec = us; q->err = err;
	if (err) gSVSt.copyFail++; else { gSVSt.pinPageoffs++; gSVSt.copyUs += us; }
	return err;
}
void xePinGet(void *pin) { boGet(static_cast<XeBo *>(pin)); }
void xePinPut(void *pin) { boPut(static_cast<XeBo *>(pin)); }
static IOService *svAccel() {
	static IOService *a;
	if (a && !a->isInactive()) return a;
	if (OSDictionary *m = IOService::serviceMatching("NMXAccel")) {
		IOService *n = nullptr;
		if (OSIterator *it = IOService::getMatchingServices(m)) {
			if ((n = OSDynamicCast(IOService, it->getNextObject()))) n->retain();
			it->release();
		}
		m->release();
		if (n && !OSCompareAndSwapPtr(nullptr, n, (void *volatile *)&a)) n->release();
	}
	return a;
}
// rb7q: IntelXeFB's scan-out mode ("nvScanoutInfo", the NVRMVramRequest of nullmoth's nvrm_vram_abi.h version 2, which
// NMXAccel's askFramebufferForScanout uses too): one frame is width x 4 x height bytes. Asked at most once a second
// (a mode change is seen within it); 0 when there is no framebuffer yet (then no create is called full-screen).
struct SvScanoutInfo {   // nvrm_vram_abi.h struct NVRMVramRequest, same layout
	unsigned int version, flags;
	unsigned long long size, actualSize, phys;
	void *kva, *handle;
	unsigned long long mappedTotal;
	unsigned int width, height, pitch;
	unsigned long long allocationCookie;
};
static IOService *svFb() {
	static IOService *f;
	if (f && !f->isInactive()) return f;
	if (OSDictionary *m = IOService::serviceMatching("IntelXeFramebuffer")) {
		IOService *n = nullptr;
		if (OSIterator *it = IOService::getMatchingServices(m)) {
			if ((n = OSDynamicCast(IOService, it->getNextObject()))) n->retain();
			it->release();
		}
		m->release();
		if (n && !OSCompareAndSwapPtr(nullptr, n, (void *volatile *)&f)) n->release();
	}
	return f;
}
static uint64_t svFrameBytes() {
	const uint64_t now = nowNs();
	if (gSVFrameAtNs && now - gSVFrameAtNs < 1000000000ull) return gSVFrameBytes;
	SvScanoutInfo r = {};
	r.version = 2;
	IOService *f = svFb();
	const OSSymbol *sym = f ? OSSymbol::withCString("nvScanoutInfo") : nullptr;
	const IOReturn ret = sym ? f->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnNotAttached;
	if (sym) sym->release();
	if (ret == kIOReturnSuccess && r.width && r.height && r.width <= 16384 && r.height <= 16384) {
		gSVFrameW = r.width; gSVFrameH = r.height; gSVFramePitch = r.pitch;
		gSVFrameBytes = (uint64_t)r.width * 4 * r.height;
	} else gSVFrameBytes = 0;
	gSVFrameAtNs = now;
	return gSVFrameBytes;
}
static IOReturn svAccelCall(const char *fn, NMXSurfVramReq *r) {
	IOService *a = svAccel();
	const OSSymbol *sym = OSSymbol::withCString(fn);
	IOReturn ret = (a && sym) ? a->callPlatformFunction(sym, false, r, nullptr, nullptr, nullptr) : kIOReturnNotAttached;
	if (sym) sym->release();
	return ret;
}
static int svStatusErr(uint32_t st) {
	switch (st) {
		case NMX_SV_OK: return 0;
		case NMX_SV_NO_SURFACE: case NMX_SV_NOT_FOUND: return ENOENT;
		case NMX_SV_BUSY: case NMX_SV_OTHER_BACKING: return EBUSY;
		case NMX_SV_NO_VRAM: return ENOSPC;
		case NMX_SV_BAD: return EINVAL;
		default: return EOPNOTSUPP;
	}
}
static bool svAllowed(XeClientState *c) {
	if (gSurfVram <= 0) return false;
	return gSurfVram >= 2 || strncmp(c->name, "WindowServer", sizeof(c->name));
}
// SURFACE_VRAM / SURFACE_DIRTY: the accelerator is called without the client lock (it takes the family lock and calls
// back into the registry); the handle is made under it afterwards.
static int surfVramIoctl(XeClientState *c, uint32_t request, uint8_t *buf) {
	// SURFACE_DIRTY is not gated by the switch: a surface that got its VRAM backing while it was on still needs its marks
	// after it goes off (else a CPU lock reads stale pages and the plugin falls back to a synchronous copy-out per frame).
	// NMXAccel answers NOT_FOUND for a surface without a registry backing, so this reaches nothing else.
	if (request != DRM_IOCTL_INTELXE_SURFACE_DIRTY && !svAllowed(c)) return EOPNOTSUPP;
	const bool v2 = request == DRM_IOCTL_INTELXE_SURFACE_VRAM2;
	if (request == DRM_IOCTL_INTELXE_SURFACE_DIRTY && !gSVLock) return EOPNOTSUPP;   // no backing was ever made
	if (request == DRM_IOCTL_INTELXE_SURFACE_DIRTY) {
		auto *d = reinterpret_cast<drm_intelxe_surface_dirty *>(buf);
		if (!d->surface_id || d->pad || d->plane || (d->flags & ~(INTELXE_SURFACE_DIRTY_TRYLOCK | INTELXE_SURFACE_DIRTY_DEMOTE_OK)))
			return EINVAL;
		NMXSurfVramReq r = {};
		r.version = INTELXE_SV_KABI_VERSION; r.sid = d->surface_id; r.plane = d->plane; r.task = c->task;
		r.flags = ((d->flags & INTELXE_SURFACE_DIRTY_TRYLOCK) ? NMX_SV_TRYLOCK : 0) |
			((d->flags & INTELXE_SURFACE_DIRTY_DEMOTE_OK) ? NMX_SV_DEMOTE_OK : 0);
		r.status = NMX_SV_OFF;
		const IOReturn ret = svAccelCall("nmxSurfDirty", &r);
		const bool demoted = !ret && r.status == NMX_SV_DEMOTED;
		const int err = demoted ? ETIME : ret ? EOPNOTSUPP : svStatusErr(r.status);
		{   // rb7q: per backing, failures by status
			const uint32_t st = ret ? 10u : (r.status < 10 ? r.status : 8u);
			uint32_t n = 0, kb = 0; bool big = false;
			IOLockLock(gSVLock);
			for (SurfVram *e = gSV; e; e = e->next)
				if (e->sid == d->surface_id && e->plane == d->plane) {
					big = e->big; kb = (uint32_t)(e->bo->size >> 10);
					if (demoted) n = ++e->dDemoted;
					else if (err) { n = ++e->dFail; e->lastFailSt = st; }
					else e->dOk++;
					break;
				}
			if (!demoted && err) { gSVQ.dirtyBy[st]++; if (big) gSVQ.bigDirtyFail++; }
			if (demoted && big && n == 1) gSVQ.bigDemoted++;
			IOLockUnlock(gSVLock);
			// a mark refused for a surface without a backing (n 0) is logged too, the first 32 times
			static uint32_t noEntry;
			if ((n && svPow2(n)) || (!n && !demoted && err && noEntry++ < 32))
				svEv(demoted ? 'M' : 'D', d->surface_id, kb, st, n, c->pid, c->name);
		}
		// demoted (rb7f): the caller writes the pages itself from now on; not a failed mark
		if (demoted) { gSVSt.demotedMarks++; return ETIME; }
		if (err) gSVSt.dirtyFail++; else gSVSt.dirty++;
		return err;
	}
	auto *v = reinterpret_cast<drm_intelxe_surface_vram *>(buf);   // SURFACE_VRAM2 starts the same way
	auto *v2l = v2 ? reinterpret_cast<drm_intelxe_surface_vram2 *>(buf) : nullptr;
	if (!v->surface_id || v->pad || (v->flags & ~(INTELXE_SURFACE_VRAM_CREATE | INTELXE_SURFACE_VRAM_SAME_HANDLE)) || v->plane)
		return EINVAL;   // plane 0 only (phase 1)
	if (v2l && v2l->pad2) return EINVAL;
	if (!svInit()) return ENOMEM;
	// rb7q: WindowServer making a backing for a surface of at least one scan-out frame (its display composites at start)
	// only imports one (debug.intelxe.surfvram_wsfull); the user's flags are left as they were
	uint32_t flags = v->flags;
	const bool ws = !strncmp(c->name, "WindowServer", sizeof(c->name));
	const uint64_t frame = (flags & INTELXE_SURFACE_VRAM_CREATE) ? svFrameBytes() : 0;
	const bool big = frame && v->size >= frame;
	bool stripped = false;
	if (ws && (flags & INTELXE_SURFACE_VRAM_CREATE)) {
		if (!gSVQ.wsFirstMs) gSVQ.wsFirstMs = svMs();
		gSVQ.wsCreates++;
		if (!frame) gSVQ.frameUnknown++;
		if (big) {
			gSVQ.wsBig++;
			if (gSurfVramWsFull > 0) { flags &= ~INTELXE_SURFACE_VRAM_CREATE; stripped = true; gSVQ.wsBigImportOnly++; }
		}
	}
	const bool create = flags & INTELXE_SURFACE_VRAM_CREATE;
	SvLayout want = {DRM_FORMAT_MOD_LINEAR, 0, 0, 0, 0, 0};
	if (v2l && create) {   // the layout the creator renders with (Linux: drm_mode_fb_cmd2 modifier + pitch, checked)
		want = {v2l->modifier, v2l->pitch, v2l->cpp, v2l->width, v2l->height, v2l->linear_pitch};
		if (want.modifier != DRM_FORMAT_MOD_LINEAR && want.modifier != I915_FORMAT_MOD_4_TILED) return EINVAL;
		if (want.modifier == I915_FORMAT_MOD_4_TILED) {
			if ((want.cpp != 4 && want.cpp != 8) || !want.width || !want.height || want.width > 16384 || want.height > 16384 ||
				!want.pitch || want.pitch % 128 || want.pitch / 4 > (1u << 18) || (uint64_t)want.width * want.cpp > want.pitch ||
				want.linPitch < want.width * want.cpp || want.linPitch > (1u << 18) ||
				(uint64_t)want.pitch * ((want.height + 31) & ~31u) > v->size)
				return EINVAL;
		}
	}
	NMXSurfVramReq r = {};
	r.version = INTELXE_SV_KABI_VERSION; r.sid = v->surface_id; r.plane = v->plane; r.task = c->task; r.size = v->size;
	r.flags = create ? NMX_SV_CREATE : 0;
	r.status = NMX_SV_OFF;
	int slot = -1;   // rb7q: xeSurfAdopt names the creator by this thread
	if (create) {
		IOLockLock(gSVLock);
		for (int i = 0; i < 8; i++) if (!gSVCreating[i].th) {
			slot = i; gSVCreating[i].th = current_thread(); gSVCreating[i].pid = c->pid;
			strlcpy(gSVCreating[i].name, c->name, sizeof(gSVCreating[i].name));
			break;
		}
		IOLockUnlock(gSVLock);
	}
	const IOReturn ret = svAccelCall("nmxSurfVram", &r);
	if (slot >= 0) { IOLockLock(gSVLock); gSVCreating[slot].th = nullptr; IOLockUnlock(gSVLock); }
	int err = ret ? EOPNOTSUPP : svStatusErr(r.status);
	if (!err && !r.entry) err = ENOENT;
	if (ws && (v->flags & INTELXE_SURFACE_VRAM_CREATE) && (big || svMs() - gSVQ.wsFirstMs < 120000)) {
		// every WindowServer create in its first two minutes and every full-screen one: what it asked, what it got
		svEv(stripped ? 'I' : 'C', v->surface_id, (uint32_t)(v->size >> 10), ret ? 0xffffu : r.status, (big ? 1u : 0u) | (stripped ? 2u : 0u),
			c->pid, c->name);
		if (stripped) { if (err) gSVQ.wsBigPrivate++; else gSVQ.wsBigImported++; }
	}
	if (err) { gSVSt.importMiss++; return err; }
	IntelXeSurfAdopt a = {};
	a.version = INTELXE_SV_KABI_VERSION; a.entry = r.entry;
	XeBo *bo = nullptr;
	int layoutErr = 0;
	IOLockLock(gSVLock);
	if (svKnown(r.entry)) {
		auto *e = static_cast<SurfVram *>(r.entry);
		bo = e->bo; boGet(bo); v->gen = e->gen;
		e->imports++;
		if (create && !e->layoutSet) {   // the first creator's layout (SURFACE_VRAM: linear)
			e->modifier = want.modifier; e->pitch = want.pitch; e->cpp = want.cpp; e->width = want.width;
			e->height = want.height; e->linPitch = want.linPitch; e->layoutSet = true;
		} else if (create && v2l && (e->modifier != want.modifier ||
			(want.modifier != DRM_FORMAT_MOD_LINEAR && (e->pitch != want.pitch || e->cpp != want.cpp || e->width != want.width ||
			                                             e->height != want.height || e->linPitch != want.linPitch)))) {
			layoutErr = EBUSY;   // another creator renders it with another layout
		}
		if (!v2l && e->modifier != DRM_FORMAT_MOD_LINEAR) layoutErr = EBUSY;   // a caller that only reads linear
		if (v2l) {
			v2l->modifier = e->modifier; v2l->pitch = e->pitch; v2l->cpp = e->cpp; v2l->width = e->width;
			v2l->height = e->height; v2l->linear_pitch = e->linPitch;
		}
	}
	IOLockUnlock(gSVLock);
	xeSurfRef(&a, false);   // the lookup's reference: the handle holds the BO from here
	if (!bo) return ENOENT;
	if (layoutErr) { boPut(bo); gSVSt.importMiss++; gSVSt.layoutBusy++; return layoutErr; }
	if (v2l && !strncmp(c->name, "WindowServer", sizeof(c->name))) {   // its plugin reads layouts: creators may tile
		IOLockLock(gClientsLock);
		gSVLayoutReader = c;
		IOLockUnlock(gClientsLock);
	}
	IOLockLock(c->lock);
	err = c->dying ? ENXIO : 0;
	// rb7m: SAME_HANDLE returns the handle the client already has for this backing (drm_gem_prime_fd_to_handle); the
	// lookup's BO reference then goes, the existing handle holds one
	uint32_t same = !err && (v->flags & INTELXE_SURFACE_VRAM_SAME_HANDLE) ? c->bos.find(bo) : 0;
	if (same) v->handle = same;
	else if (!err && !(v->handle = c->bos.add(bo))) err = ENOMEM;
	// rb7s: counted (and shared, from a second process on). A backing never moves (noMove, pinned), so no move can be
	// running that this would have to wait for: the hot path stays one atomic add and a compare
	if (!err && !same) boHandleGet(c, bo);
	IOLockUnlock(c->lock);
	if (err || same) boPut(bo);
	if (err) return err;
	if (same) gSVSt.sameHandle++;
	v->size_out = bo->size;
	gSVSt.imports++;
	if (v2) gSVSt.v2Imports++;
	return 0;
}
// debug.intelxe.surfvram_stats
int xeSurfVramStats(char *out, size_t len) {
	uint32_t n = 0, nT4 = 0;
	uint64_t bytesT4 = 0;
	if (gSVLock) {
		IOLockLock(gSVLock);
		for (SurfVram *e = gSV; e; e = e->next) {
			n++;
			if (e->modifier == I915_FORMAT_MOD_4_TILED) { nT4++; bytesT4 += e->bo->size; }
		}
		IOLockUnlock(gSVLock);
	}
	int readerPid = 0;   // the WindowServer client that imported with SURFACE_VRAM2 (INTELXE_CAP_SURFACE_VRAM_LAYOUT)
	if (gClientsLock) {
		IOLockLock(gClientsLock);
		if (gSVLayoutReader) readerPid = gSVLayoutReader->pid;
		IOLockUnlock(gClientsLock);
	}
	const SVStats &s = gSVSt;
	const int k = snprintf(out, len, "surfvram %d (budget %d MB): %u backings, %llu MB; adopted %u, refused %u (budget %u); imports %u, "
		"misses %u; dirty %u (failed %u); page-offs %u, page-ons %u, scan-out page-offs %u, copy failures %u (timeouts %u), %llu us copying\n"
		"tile4: layout cap %d (VRAM2 reader pid %d); VRAM2 imports %u, layout refusals (EBUSY) %u; live Tile4 backings %u, %llu MB; "
		"page-offs Tile4 %u / linear %u, page-ons Tile4 %u / linear %u\n"
		"demotion: %u marks answered \"demoted\" (CPU-read surfaces the writer now copies out; NMXAccel debug.nmxaccel_sv_demoted); "
		"CPU locks that waited for GPU writers %u (%llu us, max %llu us, timeouts %u); imports given an existing handle %u\n",
		gSurfVram, gSurfVramMb, n, gSVBytes >> 20, s.adopts, s.refused, s.budget, s.imports, s.importMiss, s.dirty, s.dirtyFail,
		s.pageoffs, s.pageons, s.pinPageoffs, s.copyFail, s.copyTimeouts, s.copyUs,
		gSurfVram >= 2 && readerPid ? 1 : 0, readerPid, s.v2Imports, s.layoutBusy, nT4, bytesT4 >> 20,
		s.pageoffsT4, s.pageoffs - s.pageoffsT4, s.pageonsT4, s.pageons - s.pageonsT4, s.demotedMarks,
		gSvCpuWaits, gSvCpuWaitUs, gSvCpuWaitMaxUs, gSvCpuWaitTimeouts, s.sameHandle);
	if (k < 0 || (size_t)k >= len) return k;
	// rb7q: the boot-time line (debug.intelxe.surfvram_list has the backings one by one)
	const auto &q = gSVQ;
	const int k2 = snprintf(out + k, len - (size_t)k,
		"rb7q: wsfull %d, frame %ux%u (%llu KB); WindowServer creates %u (first at %llu ms), full-screen %u -> import-only %u "
		"(imported %u, private %u), full-screen made by WindowServer %u, frame unknown %u; dirty failures busy %u no-cache %u "
		"not-found %u no-surface %u off %u no-vram %u other %u call %u, of full-screen backings %u; full-screen demoted %u, "
		"page-offs %u; adopt no-device %u clear-fail %u (first adopt %llu ms); copy no-migrate %u late-busy %u; uptime %llu ms\n",
		gSurfVramWsFull, gSVFrameW, gSVFrameH, gSVFrameBytes >> 10, q.wsCreates, q.wsFirstMs, q.wsBig, q.wsBigImportOnly,
		q.wsBigImported, q.wsBigPrivate, q.wsBigMade, q.frameUnknown, q.dirtyBy[NMX_SV_BUSY], q.dirtyBy[NMX_SV_NO_CACHE],
		q.dirtyBy[NMX_SV_NOT_FOUND], q.dirtyBy[NMX_SV_NO_SURFACE], q.dirtyBy[NMX_SV_OFF], q.dirtyBy[NMX_SV_NO_VRAM],
		q.dirtyBy[NMX_SV_OTHER_BACKING] + q.dirtyBy[NMX_SV_BAD] + q.dirtyBy[NMX_SV_OK] + q.dirtyBy[NMX_SV_DEMOTED], q.dirtyBy[10],
		q.bigDirtyFail, q.bigDemoted, q.bigPageoffs, q.adoptNoDev, q.adoptClearFail, q.firstAdoptMs, q.copyNoMig, q.copyLate, svMs());
	return k2 < 0 ? k : k + k2;
}
// rb7q: debug.intelxe.surfvram_list - the live backings (creator, age, full-screen or not, counters), then the event ring:
//   C/I  a WindowServer create (I: made an import by surfvram_wsfull); a = NMX_SV_* status, b = 1 full-screen | 2 stripped
//   A    a backing made (a = full-screen, b = generation)      R  one released (a = its life in ms, b = failed marks)
//   D/M  a failed / demoted mark (a = NMX_SV_* or 10: call failed, b = the backing's count: 1, 2, 4, 8 ...)
//   P/E  a page-off (b = 1) or page-on (b = 0) of the backing (a = its count), E a failed copy (a = errno)
//   X    an adoption refused before the device was up
int xeSurfVramList(char *out, size_t len) {
	size_t n = 0;
#define lput(...) do { if (n < len) { const int k_ = snprintf(out + n, len - n, __VA_ARGS__); if (k_ > 0) n = n + (size_t)k_ < len ? n + (size_t)k_ : len - 1; } } while (0)
	lput("uptime %llu ms; wsfull %d; frame %ux%u pitch %u\n", svMs(), gSurfVramWsFull, gSVFrameW, gSVFrameH, gSVFramePitch);
	if (gSVLock) {
		IOLockLock(gSVLock);
		for (SurfVram *e = gSV; e; e = e->next)
			lput("sid %u p%u %llu KB%s%s by %s (%d) at %llu ms, refs %u | imports %u, marks ok %u failed %u (last %u) demoted %u | "
				"page-offs %u page-ons %u copy errors %u, CPU waits %u\n", e->sid, e->plane, e->bo->size >> 10, e->big ? " FULL" : "",
				e->modifier == I915_FORMAT_MOD_4_TILED ? " T4" : "", e->cname, e->cpid, e->bornMs, e->refs, e->imports, e->dOk,
				e->dFail, e->lastFailSt, e->dDemoted, e->pOff, e->pOn, e->copyErr, e->waits);
		IOLockUnlock(gSVLock);
	}
	if (gSVEvLock) {
		static SvEv copy[256], first[64];   // copied under gSVEvLock, formatted under listLock
		static IOLock *listLock;
		if (!listLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&listLock)) IOLockFree(l); }
		if (listLock) {
			IOLockLock(listLock);
			IOSimpleLockLock(gSVEvLock);
			const uint32_t total = gSVEvN;
			memcpy(copy, gSVEv, sizeof(copy));
			memcpy(first, gSVEvFirst, sizeof(first));
			IOSimpleLockUnlock(gSVEvLock);
			const uint32_t from = total > 256 ? total - 256 : 0;
			if (from) {   // the ring went round: the boot's first events first
				lput("first 64 events:\n");
				for (uint32_t i = 0; i < 64; i++) {
					const SvEv &e = first[i];
					lput("%8llu %c sid %u %u KB a %u b %u %s(%d)\n", e.ms, e.kind, e.sid, e.kb, e.a, e.b, e.name, e.pid);
				}
			}
			lput("events %u (the last %u):\n", total, total - from);
			for (uint32_t i = from; i < total; i++) {
				const SvEv &e = copy[i & 255];
				lput("%8llu %c sid %u %u KB a %u b %u %s(%d)\n", e.ms, e.kind, e.sid, e.kb, e.a, e.b, e.name, e.pid);
			}
			IOLockUnlock(listLock);
		}
	}
#undef lput
	return (int)n;
}

// The state of a client's exec queues, for a wait that does not end (c->lock).
static void clientQueueReport(XeClientState *c, uint32_t round) {
	g2hLog("client %s (pid %d): syncobj wait still going after %u s\n", c->name, c->pid, (round + 1) * 30);
	IOLockLock(gSchedLock);
	for (uint32_t i = 1; i <= c->queues.n; i++) {
		auto *q = static_cast<XeQueue *>(c->queues.get(i));
		if (!q || isBindQ(q)) continue;
		uint32_t fifo = 0, deps = 0;
		for (XeJob *j = q->jobHead; j; j = j->next) fifo++;
		if (q->jobHead)
			for (uint32_t k = 0; k < q->jobHead->nDeps; k++) deps += !fenceSignaled(q->jobHead->deps()[k]);
		char wh[48];
		g2hLog("  queue guc_id %u class %u: seqno %u, in ring %u, done %u, started %u; FIFO %u (first waits on %u); tdr %u%s%s\n",
			q->gucId, q->cls, q->seqno, q->submitted, seqnoOf(q->slot), slotWord(q->slot, 1), fifo, deps, q->tdr,
			q->banned ? ", banned" : "", queueWhere(q, wh, sizeof(wh)));
	}
	IOLockUnlock(gSchedLock);
}

static int syncobjWait(XeClientState *c, uint64_t handles, uint64_t points, uint32_t count, uint32_t flags,
	int64_t timeout, uint32_t *first) {
	if (count == 0 || count > 64) return EINVAL;
	uint32_t h[64];
	uint64_t pt[64];
	int err = getUser(handles, h, count * 4);
	if (err) return err;
	for (uint32_t i = 0; i < count; i++) {
		pt[i] = 0;
		if (points && (err = getUser(points + i * 8, &pt[i], 8))) return err;
		if (!c->syncs.get(h[i])) return ENOENT;
	}
	const bool all = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, submit = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT,
		avail = flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE;
	if (!submit)
		for (uint32_t i = 0; i < count; i++) {
			auto *so = static_cast<XeSyncobj *>(c->syncs.get(h[i]));
			SyncGuard g(so);
			XeFence f;
			if (!syncFind(so, pt[i], &f) && (points ? syncValue(so) < pt[i] : true)) return EINVAL;
		}
	bool gone = false;
	// in 30 s pieces: a wait still going after one says, in the kernel log, what the client's queues are doing (a fence
	// that never signals: a job still in a FIFO behind its in-fences, one on the hardware, a ban)
	const int64_t end = timeout;
	for (uint32_t round = 0;; round++) {
	const int64_t piece = (int64_t)(nowNs() + 30000000000ull);
	timeout = end <= 0 || end < piece ? end : piece;
	err = waitUntil(c, timeout, kWaitSync, [&] {
		uint32_t ok = 0;
		for (uint32_t i = 0; i < count; i++) {
			auto *so = static_cast<XeSyncobj *>(c->syncs.get(h[i]));   // looked up again: the lock was dropped
			if (!so) return gone = true;
			bool sig;
			{
				SyncGuard g(so);
				if (points)
					sig = avail ? syncLastSubmitted(so) >= pt[i] : syncValue(so) >= pt[i];
				else   // rb7t: every entry of a binary wait (one, or a sync file's several)
					sig = so->n && (avail || syncBinSignaled(so));
			}
			if (sig) {
				if (!all) {
					*first = i;
					return true;
				}
				ok++;
			}
		}
		*first = 0;
		return all && ok == count;
	});
	if (!err && gJobTime && gJtEngBase) gJtWakeTs = jtNow(gJtEngBase);
	if (err != ETIME || gone || timeout == end) break;
	clientQueueReport(c, round);
	}
	return gone ? ENOENT : err;
}

static int xeIoctlLocked(XeClientState *c, uint32_t request, uint8_t *buf);

int xeIoctl(XeClientState *c, uint32_t request, uint64_t arg) {
	c->lastReq = request;
	if (gSuspended)   // system sleep: the device is off or going off; callers wait for resume (xe_pm_runtime_get)
		waitUntil(nullptr, INT64_MAX, kWaitKernel, [] { return !gSuspended; });
	uint8_t buf[256];
	uint32_t len = IOCPARM_LEN(request);
	if (len > sizeof(buf)) return EINVAL;
	bzero(buf, sizeof(buf));
	if ((request & IOC_IN) && getUser(arg, buf, len)) return EFAULT;
	if (request == DRM_IOCTL_INTELXE_SURFACE_VRAM || request == DRM_IOCTL_INTELXE_SURFACE_VRAM2 ||
		request == DRM_IOCTL_INTELXE_SURFACE_DIRTY) {   // no client lock: see surfVramIoctl
		int err = gDev->up ? surfVramIoctl(c, request, buf) : ENXIO;
		if (!err && (request & IOC_OUT) && putUser(arg, buf, len)) err = EFAULT;
		return err;
	}
	IOLockLock(c->lock);
	int err = c->dying ? ENXIO : 0;
	if (!err && !gDev->up) {
		IOLockLock(gDevLock);
		if (!gDev->up) err = gDev->bringUp();
		IOLockUnlock(gDevLock);
	}
	if (!err) {
		deferReap();
		ufAsyncRun();   // rb8e: before this client's late ones, which may come after them on a timeline
		lateUfFlush(c);
		err = xeIoctlLocked(c, request, buf);
	}
	IOLockUnlock(c->lock);
	// WAIT_USER_FENCE: the time left goes out on a timeout or an interrupted wait too (drm_ioctl copies the struct out
	// whatever the handler answered; the other ioctls keep this driver's rule: only on success)
	if ((!err || request == DRM_IOCTL_XE_WAIT_USER_FENCE) && (request & IOC_OUT) && putUser(arg, buf, len)) err = EFAULT;
	return err;
}

// rb7j: DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY chain of EXEC_QUEUE_CREATE (xe_exec_queue.c
// __exec_queue_user_extensions, exec_queue_user_ext_set_property): at most 16 extensions, base.pad and pad zero, each
// property at most once. PRIORITY: up to HIGH (2), above the device maximum (NORMAL without CAP_SYS_NICE, which nothing
// here grants) EPERM. TIMESLICE: 1 us .. 10 s (the limits enforced without CAP_SYS_NICE). PXP_TYPE: NONE accepted, HWDRM
// ENODEV (no PXP here), others EINVAL. DISABLE_STATE_CACHE_PERF_FIX: render only (EOPNOTSUPP elsewhere). MULTI_GROUP:
// ENODEV (no multi-queue on Xe2), MULTI_QUEUE_PRIORITY: EINVAL (not a multi-queue), HANG_REPLAY_STATE: EOPNOTSUPP (the
// replay of a captured context is not implemented).
static int queueProps(uint64_t ext, uint16_t cls, QueueProps *p) {
	uint32_t seen = 0;
	for (uint32_t n = 0; ext; n++) {
		if (n >= 16) return E2BIG;
		drm_xe_ext_set_property sp;
		int err = getUser(ext, &sp, sizeof(sp));
		if (err) return err;
		if (sp.base.pad || sp.base.name != DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY) return EINVAL;
		if (sp.property > DRM_XE_EXEC_QUEUE_SET_DISABLE_STATE_CACHE_PERF_FIX || sp.pad) return EINVAL;
		if (seen & 1u << sp.property) return EINVAL;
		seen |= 1u << sp.property;
		switch (sp.property) {
			case DRM_XE_EXEC_QUEUE_SET_PROPERTY_PRIORITY:
				if (sp.value > 2) return EINVAL;
				if (sp.value > 1) return EPERM;
				p->prio = (uint8_t)sp.value;
				break;
			case DRM_XE_EXEC_QUEUE_SET_PROPERTY_TIMESLICE:
				if (sp.value < 1 || sp.value > 10000000) return EINVAL;
				p->timesliceUs = (uint32_t)sp.value;
				break;
			case DRM_XE_EXEC_QUEUE_SET_PROPERTY_PXP_TYPE:
				if (sp.value == DRM_XE_PXP_TYPE_NONE) break;
				return sp.value == DRM_XE_PXP_TYPE_HWDRM ? ENODEV : EINVAL;
			case DRM_XE_EXEC_QUEUE_SET_HANG_REPLAY_STATE:
				return EOPNOTSUPP;
			case DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_GROUP:
				return ENODEV;
			case DRM_XE_EXEC_QUEUE_SET_PROPERTY_MULTI_QUEUE_PRIORITY:
				return EINVAL;
			case DRM_XE_EXEC_QUEUE_SET_DISABLE_STATE_CACHE_PERF_FIX:
				if (cls != DRM_XE_ENGINE_CLASS_RENDER) return EOPNOTSUPP;
				p->stateCacheFix = sp.value != 0;
				break;
		}
		ext = sp.base.next_extension;
	}
	return 0;
}

// rb7r: the GEM_CREATE extension chain, as xe_bo.c gem_create_user_extensions / gem_create_user_ext_set_property /
// gem_create_set_pxp_type: at most 16 extensions (the 17th E2BIG); each read as drm_xe_user_extension (EFAULT), whose
// pad must be 0 and name DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY (EINVAL), then as drm_xe_ext_set_property (EFAULT),
// whose property must be DRM_XE_GEM_CREATE_SET_PROPERTY_PXP_TYPE and pad 0 (EINVAL; reserved is not looked at). PXP_TYPE:
// NONE accepted, HWDRM -> xe_pxp_key_assign, which answers ENODEV when xe->pxp is NULL - always on Battlemage (bmg_desc
// has no has_pxp, so xe_pxp_init leaves it NULL; DEVICE_QUERY_PXP_STATUS and the exec queue PXP_TYPE answer the same);
// any other value EINVAL. Mesa ANV only chains it for protected memory, which it offers only when the PXP status query
// succeeds, so on Linux B580 ANV never sends it, and neither does it here.
// DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY chain (xe_uapi_args.cpp xeGemCreateExtensions: gem_create_user_extensions,
// gem_create_user_ext_set_property, gem_create_set_pxp_type without PXP), read with copyin
static int userRead(void *, uint64_t addr, void *dst, size_t len) { return getUser(addr, dst, len); }
static int gemCreateExts(uint64_t ext) { return xeGemCreateExtensions(ext, userRead, nullptr); }

// DRM_IOCTL_XE_WAIT_USER_FENCE (xe_wait_user_fence.c xe_wait_user_fence_ioctl): the argument checks (xeUfenceArgsCheck),
// the exec queue if one is named (xe_exec_queue_lookup: ENOENT), then until the deadline (xeUfenceDeadline,
// to_jiffies_timeout): read the 64-bit value at addr (do_compare's copy_from_user: EFAULT), compare it under the mask
// (xeUfenceCompare: done when it holds), and with an exec queue give up with EIO once that queue was reset
// (q->ops->reset_status: banned, killed - destroyed meanwhile here - or the device wedged). addr is a user pointer, a CPU
// address in the caller's task (xe_drm.h): a BO mapped with GEM_MMAP_OFFSET, the caller's own memory; an exec's fence is
// written by the GPU at a GPU address of the same memory, a bind's by the kernel at this kind of pointer. Waiters sleep
// on the device's wait generation, which every GT interrupt moves (xe_hw_engine_handle_irq wakes ufence_wq), as do
// bans (xe_guc_submit wakes it on a reset) and bind fences (bindUfenceSignal, rb8e ufAsyncRun). The time left goes back to the caller
// whatever the outcome (xeUfenceTimeoutLeft; xeIoctl copies this struct out on errors too, as drm_ioctl does).
static int waitUserFence(XeClientState *c, drm_xe_wait_user_fence &a) {
	if (int err = xeUfenceArgsCheck(a)) return err;
	XeQueue *q = nullptr;
	uint32_t qgen = 0;
	if (a.exec_queue_id) {
		q = static_cast<XeQueue *>(c->queues.get(a.exec_queue_id));
		if (!q) return ENOENT;
		if (!isBindQ(q)) qgen = q->gen;
	}
	gUfSt.waits++;
	const uint64_t start = nowNs();
	const int64_t until = xeUfenceDeadline(a, start);
	int res = 1;
	int err = waitUntil(c, until, kWaitSync, [&] {
		ufAsyncRun();     // rb8e: a bind fence this wait may be for (ufAsyncAdd), should the worker not have run yet
		lateUfFlush(c);   // a late bind fence this wait may be for (bindUfenceWait)
		uint64_t v;
		if (getUser(a.addr, &v, sizeof(v))) {
			res = EFAULT;
			return true;
		}
		if ((res = xeUfenceCompare(v, a.value, a.mask, a.op)) != 1) return true;   // met (0) or EINVAL
		if (q) {   // exec_queue_reset_status; looked up again: the client lock was dropped while asleep
			auto *cur = static_cast<XeQueue *>(c->queues.get(a.exec_queue_id));
			if (cur != q || gWedged || (!isBindQ(q) && (q->gen != qgen || q->banned))) {
				res = EIO;
				return true;
			}
		}
		return false;
	});
	if (!err) err = res;
	if (err == 0) gUfSt.met++;
	else if (err == ETIME) gUfSt.timeouts++;
	else if (err == EFAULT) gUfSt.faults++;
	else if (err == EIO) gUfSt.resets++;
	xeUfenceTimeoutLeft(a, start, nowNs());
	return err;
}

// rb7r: DRM_IOCTL_XE_MADVISE (xe_vm_madvise.c xe_vm_madvise_ioctl), in xe's order: the VM (EINVAL), madvise_args_are_sane
// (start and range 4 KB aligned, range >= 4 KB; per type: PREFERRED_LOC devmem_fd >= -1, region_instance 0 unless
// devmem_fd > 0, migration_policy <= ONLY_SYSTEM_PAGES, reserved 0; ATOMIC val <= CPU, pad and reserved 0; PAT index
// below the device's n_entries (28 on BMG, graphics 20.01: Wa_16023588340 drops CLOS3) with a coherency mode (16-19 have
// none), pad and reserved 0; PURGEABLE_STATE val WILLNEED or DONTNEED, pad 0; other types EINVAL; reserved[] 0), then
// a nonzero retained_ptr must point at a readable 0 (EFAULT / EINVAL). PREFERRED_LOC with a devmem fd: ENOENT, what
// xe_drm_pagemap_from_fd answers without device-memory pagemaps; then the range must lie in the 48-bit VM (the
// drm_gpuvm_range_valid of xe_vm_alloc_madvise_vma: EINVAL). The bindings it acts on are every one overlapping the range
// (BO-backed VMAs are never split by madvise: drm_gpuvm_madvise_ops_create skips VAs with an object); none: 0.
//  - PURGEABLE_STATE (madvise_purgeable): per binding of a BO (NULL and userptr bindings are not BOs), a purged BO
//    sets "has purged"; otherwise WILLNEED/DONTNEED change the binding's state and its BO's holder count (the BO turns
//    DONTNEED when no WILLNEED binding is left, WILLNEED with the first one). No PTE changes (madvise_range_needs_
//    invalidation: the PTEs stay until a purge). *retained_ptr = 1 unless a purged BO was met.
//  - PREFERRED_LOC without a fd: what xe does to BO VMAs (only CPU-address-mirror VMAs take it, and there are none
//    here): nothing, 0.
//  - ATOMIC and PAT: EOPNOTSUPP once the arguments passed xe's checks and the range holds bindings. xe changes the
//    VMAs' attributes and zaps their PTEs for the GPU fault handler to rebuild; there is no fault-mode VM here.
static int vmMadvise(XeClientState *c, const drm_xe_madvise &a) {
	auto *vm = static_cast<XeVm *>(c->vms.get(a.vm_id));
	if (!vm) return EINVAL;
	if ((a.start & 0xfff) || (a.range & 0xfff) || a.range < 0x1000) return EINVAL;
	// ATOMIC and PAT: xe's checks (each on one line with its error, so tools/parity/uapi.py reads them as refused)
	if (a.type == DRM_XE_MEM_RANGE_ATTR_ATOMIC && (a.atomic.val > DRM_XE_ATOMIC_CPU || a.atomic.pad || a.atomic.reserved)) return EINVAL;
	if (a.type == DRM_XE_MEM_RANGE_ATTR_PAT && (a.pat_index.val >= gXePlat->patEntries || (a.pat_index.val >= 16 && a.pat_index.val <= 19))) return EINVAL;
	if (a.type == DRM_XE_MEM_RANGE_ATTR_PAT && (a.pat_index.pad || a.pat_index.reserved)) return EINVAL;
	switch (a.type) {
		case DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC: {
			const int32_t fd = (int32_t)a.preferred_mem_loc.devmem_fd;
			const bool ok = fd >= DRM_XE_PREFERRED_LOC_DEFAULT_SYSTEM && (fd > DRM_XE_PREFERRED_LOC_DEFAULT_DEVICE ||
				!a.preferred_mem_loc.region_instance) && a.preferred_mem_loc.migration_policy <= DRM_XE_MIGRATE_ONLY_SYSTEM_PAGES &&
				!a.preferred_mem_loc.reserved;
			if (!ok) return EINVAL;
			break;
		}
		case DRM_XE_VMA_ATTR_PURGEABLE_STATE: {
			const uint32_t v = a.purge_state_val.val;
			const bool ok = v == DRM_XE_VMA_PURGEABLE_STATE_WILLNEED || v == DRM_XE_VMA_PURGEABLE_STATE_DONTNEED;
			if (!ok || a.purge_state_val.pad) return EINVAL;
			break;
		}
		default:   // ATOMIC and PAT were checked above; other types do not exist
			if (a.type > 3) return EINVAL;
			break;
	}
	if (a.reserved[0] || a.reserved[1]) return EINVAL;
	const bool purgeable = a.type == DRM_XE_VMA_ATTR_PURGEABLE_STATE;
	const uint64_t retainedPtr = purgeable ? a.purge_state_val.retained_ptr : 0;
	if (retainedPtr) {
		uint32_t r;
		if (getUser(retainedPtr, &r, sizeof(r))) return EFAULT;
		if (r) return EINVAL;
	}
	if (a.type == DRM_XE_MEM_RANGE_ATTR_PREFERRED_LOC && (int32_t)a.preferred_mem_loc.devmem_fd > 0) return ENOENT;
	const uint64_t vmSize = xePlatformVmSize(gXePlat);
	const uint64_t start = a.start & (vmSize - 1), end = start + a.range;   // xe_device_uncanonicalize_addr
	if (end < start || end > vmSize) return EINVAL;
	bool any = false, purged = false;
	for (uint32_t k = 0; k < vm->nBinds; k++) {
		XeBinding &b = vm->binds[k];
		if (b.addr >= end || start >= b.addr + b.range) continue;
		any = true;
		if (!purgeable) continue;
		XeBo *bo = b.bo;
		if (!bo || bo->user) continue;
		if (bo->purgeTrack && bo->purgeState == kMadvPurged) {   // once purged, always purged
			purged = true;
			continue;
		}
		if (a.purge_state_val.val == DRM_XE_VMA_PURGEABLE_STATE_WILLNEED) {
			if (b.purge == kMadvDontneed) {
				b.purge = kMadvWillneed;
				boWillneedGet(bo);
				gPurgeSt.toWillneed++;
			}
		} else if (b.purge == kMadvWillneed) {
			b.purge = kMadvDontneed;
			boWillneedPut(bo);
			gPurgeSt.toDontneed++;
		}
	}
	if (purgeable) gPurgeSt.calls++;
	if (any && (a.type == DRM_XE_MEM_RANGE_ATTR_ATOMIC || a.type == DRM_XE_MEM_RANGE_ATTR_PAT)) return EOPNOTSUPP;
	if (retainedPtr) {   // written last, as xe does after dropping its locks
		const uint32_t retained = !purged;
		if (!retained) gPurgeSt.retainedZero++;
		if (putUser(retainedPtr, &retained, sizeof(retained))) return EFAULT;
	}
	return 0;
}

// ─── rb7t: object fds (uapi/intelxe_drm.h; the clone device and the fd resolution: xe_fd.cpp) ─────────────────────────
// Counters for debug.intelxe.fd_stats / debug.intelxe.vram: exports and imports per kind, PRIME imports by a client other
// than the BO's first (noMove from then on), sync files imported.
static struct { uint32_t primeExports, primeImports, primeSame, foreign, syncExports, syncImports, fileExports, fileImports; } gFdShare;

// DRM_CLOEXEC / DRM_RDWR as user space defines them on macOS (uapi/drm.h: O_CLOEXEC, O_RDWR)
static constexpr uint32_t kDrmCloexec = 0x01000000u, kDrmRdwr = 0x0002u;

// drm_gem_prime_fd_to_handle with xe_gem_prime_import of its own dma-buf (the same object): the handle c has for it
// already (drm_prime_lookup_buf_handle: one handle per object per file), else a new one, which keeps the caller's
// reference. A BO imported by a client other than its first stops moving (noMove: Linux pins a dma-buf an importer
// maps; here a move of a shared BO would also hold every importer's client lock, moveSetBegin); one made for CPU access
// that an eviction took out of the window goes back in first, so CPU maps of it keep working.
static int primeImport(XeClientState *c, XeBo *bo, uint32_t *handle) {
	if (uint32_t h = c->bos.find(bo)) {
		*handle = h;
		boPut(bo);
		gFdShare.primeSame++;
		return 0;
	}
	IOLockLock(gNameLock);
	const bool moving = boHandleGet(c, bo);
	if (!bo->dmabufCached) {   // obj->dma_buf again (drm_gem_prime_fd_to_handle): the handle cache holds the dma-buf
		bo->dmabufCached = true;
		bo->dmabufRefs++;
	}
	IOLockUnlock(gNameLock);
	int err = 0;
	// a move that started before the BO was shared rewrites only its first client's VMs: let it end (as GEM_OPEN)
	if (moving) err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
	if (!err && c->dying) err = ENXIO;
	uint32_t h = 0;
	if (!err && (h = c->bos.find(bo))) {   // another thread of c imported it while the lock was dropped
		boHandleRelease(bo);
		boPut(bo);
		*handle = h;
		gFdShare.primeSame++;
		return 0;
	}
	if (!err && !(h = c->bos.add(bo))) err = ENOMEM;
	if (err) {
		boHandleRelease(bo);   // no handle was made in c
		boPut(bo);
		return err;
	}
	*handle = h;
	gFdShare.primeImports++;
	if (bo->owner != c && !bo->noMove) {
		if (bo->vram && bo->hi && (bo->gemFlags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM)) {
			boGet(bo);
			boMakeVisible(c, bo);   // best effort (c's lock may be dropped meanwhile)
			bo->noMove = true;
			boPut(bo);
		} else {
			bo->noMove = true;
		}
		gFdShare.foreign++;
	}
	return 0;
}

static int objFdIoctl(XeClientState *c, uint32_t request, uint8_t *buf) {
	if (!gFds) return ENOTTY;   // switched off: as before rb7t
	int err = 0;
	uint32_t kind = kFdNone;
	void *obj = nullptr;
	switch (request) {
		case DRM_IOCTL_PRIME_HANDLE_TO_FD: {   // drm_prime_handle_to_fd_ioctl -> drm_gem_prime_handle_to_dmabuf, xe_gem_prime_export
			auto *p = reinterpret_cast<drm_prime_handle *>(buf);
			if (p->flags & ~(kDrmCloexec | kDrmRdwr)) return EINVAL;
			auto *bo = static_cast<XeBo *>(c->bos.get(p->handle));
			if (!bo) return ENOENT;
			if (bo->vmPrivate) return EPERM;   // xe_gem_prime_export: bo->vm
			IOLockLock(gNameLock);
			err = dmabufExportLocked(bo);
			IOLockUnlock(gNameLock);
			if (err) return err;
			boGet(bo);   // the minor's reference
			if ((err = xeFdBind(p->fd, kFdBo, bo))) {
				dmabufPut(bo);
				boPut(bo);
				return err;
			}
			gFdShare.primeExports++;
			return 0;   // p->fd stays the caller's (an input here: uapi/intelxe_drm.h)
		}
		case DRM_IOCTL_PRIME_FD_TO_HANDLE: {   // drm_prime_fd_to_handle_ioctl
			auto *p = reinterpret_cast<drm_prime_handle *>(buf);
			if ((err = xeFdTake(p->fd, &kind, &obj))) return err;   // dma_buf_get: EBADF (no such fd), EINVAL
			if (kind != kFdBo) {
				if (obj) xeFdObjPut(kind, obj);
				return EINVAL;   // not a dma-buf
			}
			return primeImport(c, static_cast<XeBo *>(obj), &p->handle);
		}
		case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD: {   // drm_syncobj_handle_to_fd_ioctl
			auto *a = reinterpret_cast<drm_syncobj_handle *>(buf);
			if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) return EINVAL;
			auto *so = static_cast<XeSyncobj *>(c->syncs.get(a->handle));
			if (!(a->flags & DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE)) {
				if (!so) return EINVAL;   // drm_syncobj_handle_to_fd
				if (!so->shared) {   // under c's lock, before any other client can reach it (see XeSyncobj)
					so->shared = true;
					OSIncrementAtomic(&gSharedSyncLive);
				}
				OSIncrementAtomic(&so->refs);   // the minor's (drm_syncobj_get_fd)
				if ((err = xeFdBind(a->fd, kFdSyncobj, so))) syncPut(so);
				else gFdShare.syncExports++;
				return err;
			}
			if (!so) return ENOENT;   // drm_syncobj_find_fence
			XeSyncFile *sf = syncFileNew(so, &err);
			if (!sf) return err;
			if ((err = xeFdBind(a->fd, kFdSyncFile, sf))) syncFilePut(sf);
			else gFdShare.fileExports++;
			return err;
		}
		case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE: {   // drm_syncobj_fd_to_handle_ioctl
			auto *a = reinterpret_cast<drm_syncobj_handle *>(buf);
			if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE)) return EINVAL;
			// drm_syncobj_fd_to_handle (fd_empty) and sync_file_get_fence (sync_file_fdget) answer EINVAL for an fd
			// that is not open as well: unlike dma_buf_get, no EBADF here
			if (xeFdTake(a->fd, &kind, &obj)) return EINVAL;
			if (a->flags & DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE) {   // drm_syncobj_import_sync_file_fence
				auto *so = static_cast<XeSyncobj *>(c->syncs.get(a->handle));
				if (kind != kFdSyncFile) err = EINVAL;
				else if (!so) err = ENOENT;
				else {
					auto *sf = static_cast<XeSyncFile *>(obj);
					{
						SyncGuard g(so);
						syncReplaceN(so, sf->f, sf->n);   // drm_syncobj_replace_fence
					}
					gFdShare.fileImports++;
					wakeAll();   // waiters for submission, in every client that holds the syncobj
				}
				if (obj) xeFdObjPut(kind, obj);
				return err;
			}
			if (kind != kFdSyncobj) {
				if (obj) xeFdObjPut(kind, obj);
				return EINVAL;
			}
			// a new handle every time (drm_syncobj_fd_to_handle: xa_alloc, no lookup), keeping the reference just taken
			if (!(a->handle = c->syncs.add(obj))) {
				syncPut(static_cast<XeSyncobj *>(obj));
				return ENOMEM;
			}
			gFdShare.syncImports++;
			return 0;
		}
		case DRM_IOCTL_INTELXE_OBJ_INFO: {
			auto *o = reinterpret_cast<drm_intelxe_obj_info *>(buf);
			if (o->pad) return EINVAL;
			if ((err = xeFdTake(o->fd, &kind, &obj))) return err;
			o->kind = kind;
			o->size = kind == kFdBo ? static_cast<XeBo *>(obj)->size : 0;
			o->status = kind == kFdSyncFile && syncFileSignaled(static_cast<XeSyncFile *>(obj)) ? INTELXE_OBJ_SIGNALED : 0;
			if (obj) xeFdObjPut(kind, obj);
			return 0;
		}
	}
	return ENOTTY;
}

void xeFdObjGet(uint32_t kind, void *obj) {
	if (kind == kFdBo) boGet(static_cast<XeBo *>(obj));
	else if (kind == kFdSyncobj) OSIncrementAtomic(&static_cast<XeSyncobj *>(obj)->refs);
	else if (kind == kFdSyncFile) OSIncrementAtomic(&static_cast<XeSyncFile *>(obj)->refs);
}
void xeFdObjPut(uint32_t kind, void *obj) {
	if (kind == kFdBo) boPut(static_cast<XeBo *>(obj));
	else if (kind == kFdSyncobj) syncPut(static_cast<XeSyncobj *>(obj));
	else if (kind == kFdSyncFile) syncFilePut(static_cast<XeSyncFile *>(obj));
}
// the minor's last close: its reference (a BO's also from its dma-buf)
void xeFdObjRelease(uint32_t kind, void *obj) {
	if (kind == kFdBo) dmabufPut(static_cast<XeBo *>(obj));
	xeFdObjPut(kind, obj);
}

static int xeIoctlLocked(XeClientState *c, uint32_t request, uint8_t *buf) {
	int err = 0;
	switch (request) {
		case DRM_IOCTL_VERSION: {
			auto *v = reinterpret_cast<drm_version *>(buf);
			static const char name[] = "xe", date[] = "20260101", desc[] = "Intel Xe2 (IntelXe.kext)";
			v->version_major = 1;
			v->version_minor = 1;
			auto put = [&](uint64_t dst, __kernel_size_t &l, const char *s, size_t sl) {
				if (dst && l) putUser(dst, s, l < sl ? l : sl);
				l = sl;
			};
			put((uint64_t)(uintptr_t)v->name, v->name_len, name, sizeof(name) - 1);
			put((uint64_t)(uintptr_t)v->date, v->date_len, date, sizeof(date) - 1);
			put((uint64_t)(uintptr_t)v->desc, v->desc_len, desc, sizeof(desc) - 1);
			break;
		}
		case DRM_IOCTL_GET_CAP: {
			auto *g = reinterpret_cast<drm_get_cap *>(buf);
			g->value = g->capability == DRM_CAP_SYNCOBJ || g->capability == DRM_CAP_SYNCOBJ_TIMELINE ||
				g->capability == INTELXE_CAP_SURFACE_FENCES;
			if (g->capability == INTELXE_CAP_SURFACE_VRAM) g->value = gSurfVram > 0 ? (uint64_t)gSurfVram : 0;
			// WindowServer reads tiled backings (SURFACE_VRAM2). A plain read: this runs under the client lock, which
			// gClientsLock comes before (lock order), and the answer is a hint taken again at each swapchain.
			if (g->capability == INTELXE_CAP_STATUS_PAGE) g->value = kStatusPageType;
			if (g->capability == INTELXE_CAP_SURFACE_VRAM_LAYOUT)
				g->value = gSurfVram >= 2 && *static_cast<XeClientState *volatile *>(&gSVLayoutReader) != nullptr;
			// rb7t: object fds, only while debug.intelxe.fds is 1 (0: as before, the caps read 0 and the ioctls ENOTTY)
			if (g->capability == INTELXE_CAP_FD_OBJECTS) g->value = gFds ? 1 : 0;
			if (g->capability == DRM_CAP_PRIME) g->value = gFds ? DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT : 0;
			break;
		}
		case DRM_IOCTL_XE_DEVICE_QUERY:
			err = deviceQuery(reinterpret_cast<drm_xe_device_query *>(buf));
			break;
		case DRM_IOCTL_XE_MADVISE:   // rb7r
			err = vmMadvise(c, *reinterpret_cast<const drm_xe_madvise *>(buf));
			break;
		case DRM_IOCTL_XE_WAIT_USER_FENCE:
			err = waitUserFence(c, *reinterpret_cast<drm_xe_wait_user_fence *>(buf));
			break;
		case DRM_IOCTL_XE_GEM_CREATE: {
			auto *g = reinterpret_cast<drm_xe_gem_create *>(buf);
			// rb7s: xe_gem_create_ioctl's argument checks, in its order and with its answers (xe_bo.c). WAS only size,
			// 4 KB alignment and a nonzero placement: pad, reserved, unknown flags, a handle, unknown placement bits, a
			// zero or unknown cpu_caching, WB for a BO that may live in VRAM or is a scan-out, NEEDS_VISIBLE_VRAM without
			// VRAM and an unknown vm_id were all accepted. Mesa ANV and iris never send those (anv xe_gem_create: VRAM
			// placements always WC, WB only for NO_LOCAL_MEM system BOs; xemac's fake device has made the same checks).
			constexpr uint32_t kMemRegionMask = 3;   // xe->info.mem_region_mask: system (bit 0) and tile 0's VRAM (bit 1)
			constexpr uint32_t kFlags = DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING | DRM_XE_GEM_CREATE_FLAG_SCANOUT |
				DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM | DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION;
			if (g->pad[0] || g->pad[1] || g->pad[2] || g->reserved[0] || g->reserved[1]) return EINVAL;
			if ((g->placement & ~kMemRegionMask) || !g->placement) return EINVAL;   // at least one valid placement
			if (g->flags & ~kFlags) return EINVAL;
			if (g->handle) return EINVAL;
			if (!g->size) return EINVAL;
			if (g->size & 0xfff) return EINVAL;   // ~PAGE_MASK (size > SIZE_MAX cannot happen with a 64-bit size_t)
			// NO_COMPRESSION: EOPNOTSUPP below graphics 20 only (Battlemage is 20.01). DEFER_BACKING: the backing is
			// made at once all the same (what it defers is an allocation xe would make at the first bind or CPU fault).
			// Scan-out + VRAM + 64 KB-aligned size gets XE_BO_FLAG_NEEDS_64K in xe; Battlemage has no
			// XE_VRAM_FLAGS_NEED64K, so xe_bo_init_locked refuses no size here (every VRAM block here is 64 KB anyway).
			const bool vramPl = g->placement & 2;   // bo_flags & XE_BO_FLAG_VRAM_MASK
			if ((g->flags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM) && !vramPl) return EINVAL;
			if (!g->cpu_caching || g->cpu_caching > DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
			if (vramPl && g->cpu_caching != DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
			if ((g->flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT) && g->cpu_caching == DRM_XE_GEM_CPU_CACHING_WB)
				return EINVAL;   // XE_BO_FLAG_FORCE_WC: display scan-out is never CPU-cache coherent
			if (g->vm_id && !c->vms.get(g->vm_id)) return ENOENT;   // xe_vm_lookup
			// rb7r: the extension chain (DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY). xe_gem_create_ioctl reads it after the
			// BO is made and frees the BO on an error; here it is read first, so a refused chain never frees VRAM whose
			// copy-engine clear is still queued. The answers are the same unless the allocation itself fails.
			if (g->extensions && (err = gemCreateExts(g->extensions))) return err;
			auto *bo = static_cast<XeBo *>(IOMallocZero(sizeof(XeBo)));
			if (!bo) return ENOMEM;
			bo->size = g->size;
			bo->refs = 1;
			bo->caching = g->cpu_caching;
			bo->gemFlags = g->flags;
			bo->placement = g->placement;
			bo->vmPrivate = g->vm_id != 0;   // rb7t: xe's bo->vm (never PRIME-exported)
			clientMapsPrune(c);
			if (g->placement & 2) {   // VRAM instance 1
				const bool visible = g->flags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
				// Scan-out: one physical run (above the window unless the CPU needs it), so NMXAccel's pin can hand the
				// display a single DPA range. Linux needs no physical run (the display pins the BO and writes one GGTT
				// PTE per page); here the pin interface takes one range.
				const bool scanout = g->flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT;
				bo->scanout = scanout;
				bo->noComp = g->flags & DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION;
				if (!vramAllocBo(bo, g->size, visible, scanout) && gNDef) {   // VRAM unbinds still hold until their invalidation
					tlbWait(c, gTlbSeq);
					deferReap();
					vramAllocBo(bo, g->size, visible, scanout);
				}
				// rb7r: DONTNEED BOs give their VRAM up first (xe_bo_move purges an evicted DONTNEED BO instead of moving it)
				if (!bo->vram && purgeAll(c, g->size, visible ? kPurgeWindow : kPurgeVram)) vramAllocBo(bo, g->size, visible, scanout);
				// the window is full: move never-mapped idle BOs out of it (TTM evicts to make room)
				if (!bo->vram && visible && evictWindow(c, g->size)) vramAllocBo(bo, g->size, visible, scanout);
				// VRAM full: the BO lives in system memory instead, as TTM evicts xe BOs to sys_placement. A dGPU's
				// system memory accesses are CPU-coherent (xe_bo.c xe_ttm_tt_create), so it only costs speed; ENOSPC
				// made Mesa fail the allocation and WindowServer or the app abort.
				static uint32_t spills;
				if (!bo->vram) {
					if (visible) {
						gStats.winSpills++;
						gStats.winSpillBytes += g->size;
					} else {
						gStats.vramSpills++;
					}
				}
				if (!bo->vram && !(g->placement & 1) && !(spills++ % 64))
					say("vram%s full: %llu KB BO placed in system memory (%u so far)\n", visible ? " window" : "",
						g->size >> 10, spills);
			}
			if (!bo->vram) {
				bo->sys = gDev->allocSys(g->size, false);
				// rb7r: out of system memory: DONTNEED system BOs go first (xe_shrinker purges before it backs up)
				if (!bo->sys && purgeAll(c, g->size, kPurgeSys)) bo->sys = gDev->allocSys(g->size, false);
				if (!bo->sys) {
					IOFree(bo, sizeof(*bo));
					return ENOMEM;
				}
				OSAddAtomic64((SInt64)g->size, const_cast<SInt64 *>(&gSysUsed));
				if (!bo->dma.map(bo->sys)) {   // xe_tt_map_sg
					boPut(bo);
					return ENOMEM;
				}
				flushRange(bo->sys->getBytesNoCopy(), g->size);
				bo->sysRefs = (uint32_t)bo->sys->getRetainCount();   // rb7r: no CPU mapping yet (purgeBlocked)
			} else if (!gClearOk || vramClear(bo)) {   // fresh VRAM reads as zeros, like a new TTM BO
				if (!boVisible(bo)) {   // the CPU cannot reach it, and the copy engine failed
					boPut(bo);
					return ENOMEM;
				}
				for (VBlock *b = bo->blocks; b; b = b->link) {
					volatile uint64_t *p = vram64(b->off);
					for (uint64_t i = 0; i < vblockSize(b) / 8; i++) p[i] = 0;
				}
			}
			// rb7r: purgeable state is tracked from here (xe_bo_init_locked: WILLNEED)
			bo->purgeTrack = true;
			bo->purgeState = kMadvWillneed;
			// a BO private to a VM (vm_id: it shares the VM's dma-resv in xe) puts its clear in the VM's KERNEL slot at once
			if (g->vm_id && bo->vram) vmKernAdd(static_cast<XeVm *>(c->vms.get(g->vm_id)), bo->clear);
			g->handle = c->bos.add(bo);
			if (!g->handle) {
				boPut(bo);
				return ENOMEM;
			}
			boHandleGet(c, bo);   // rb7s: its first handle (c becomes its first client)
			break;
		}
		case DRM_IOCTL_GEM_CLOSE: {
			auto *g = reinterpret_cast<drm_gem_close *>(buf);
			auto *bo = static_cast<XeBo *>(c->bos.take(g->handle));
			if (!bo) return EINVAL;   // rb7s: drm_gem_handle_delete (drm_gem.c) answers EINVAL for an unknown handle (WAS ENOENT)
			// rb7s: per client. WAS bo->closed = true and every map of the BO pruned: another client's handle of a shared
			// BO then answered ENOENT to GEM_MMAP_OFFSET, and its maps were freed through this client's table. Now c's maps
			// go (or detach, if still mapped) only when c's last handle of it goes (drm_vma_node_revoke is per file).
			// c's maps are on bo->cpuMaps (added under c's lock, so none of c's is missed when it reads empty): a BO nobody
			// mapped (every surfvram backing: WindowServer closes those often) costs no scan of c's table
			const bool last = boHandleRelease(bo);
			if (bo->cpuMaps && (last || !c->bos.find(bo)))
				c->orphanMaps += boMapsPrune(c, bo, true);   // still mapped: stay (without the BO) for their unmap
			boPut(bo);
			clientMapsPrune(c);
			break;
		}
		case DRM_IOCTL_GEM_FLINK: {   // rb7s: drm_gem_flink_ioctl (DRM_AUTH: debug.intelxe.flink 0 answers as a render node)
			auto *f = reinterpret_cast<drm_gem_flink *>(buf);
			if (!gFlink) return EACCES;   // drm_ioctl_permit: a render client and a DRM_AUTH-only ioctl
			auto *bo = static_cast<XeBo *>(c->bos.get(f->handle));
			if (!bo) return ENOENT;
			IOLockLock(gNameLock);
			if (!bo->name) {
				// the name is the cross-process export here: xe_gem_prime_export's purgeable rules and WILLNEED hold
				if (bo->purgeTrack && bo->purgeState != kMadvWillneed) err = bo->purgeState == kMadvDontneed ? EBUSY : EINVAL;
				else if (!(bo->name = gNames.add(bo))) err = ENOMEM;
				else {
					boWillneedGet(bo);
					gShareSt.names++;
				}
			}
			if (!err) f->name = bo->name;
			IOLockUnlock(gNameLock);
			if (!err) gShareSt.flinks++;
			break;
		}
		case DRM_IOCTL_GEM_OPEN: {    // rb7s: drm_gem_open_ioctl: a new handle of the named BO in this client, its size
			auto *o = reinterpret_cast<drm_gem_open *>(buf);
			if (!gFlink) return EACCES;
			IOLockLock(gNameLock);
			auto *bo = static_cast<XeBo *>(gNames.get(o->name));
			if (!bo || !bo->handles) {   // its last handle is going (the name with it)
				IOLockUnlock(gNameLock);
				return ENOENT;
			}
			boGet(bo);
			const bool moving = boHandleGet(c, bo);
			IOLockUnlock(gNameLock);
			// a move of it that started before it was shared rewrites only its first client's VMs: let it end before this
			// client can bind it (moves started from now on see `shared`)
			if (moving) err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
			if (!err && !c->dying && !(o->handle = c->bos.add(bo))) err = ENOMEM;
			if (!err && c->dying) err = ENXIO;
			if (err) {
				boHandleRelease(bo);   // no handle was made in c: none of c's maps hangs on it
				boPut(bo);
				return err;
			}
			o->size = bo->size;
			gShareSt.opens++;
			break;
		}
		case DRM_IOCTL_XE_GEM_MMAP_OFFSET: {
			auto *m = reinterpret_cast<drm_xe_gem_mmap_offset *>(buf);
			// xe_gem_mmap_offset_ioctl: extensions and reserved zero; the PCI barrier page (its only flag) is not offered
			if (m->extensions || m->reserved[0] || m->reserved[1] || m->flags) return EINVAL;
			auto *bo = static_cast<XeBo *>(c->bos.get(m->handle));
			if (!bo) return ENOENT;
			// rb7r: a DONTNEED or purged BO: the offset is handed out as xe does (xe_gem_mmap_offset_ioctl does not look at
			// the state), but nothing moves and no map is made; the mapping itself is refused while it is not WILLNEED
			// (xe_gem_object_mmap: DONTNEED EBUSY, purged EINVAL; see xeClientMemory)
			if (bo->purgeTrack && bo->purgeState != kMadvWillneed) {
				m->offset = (uint64_t)m->handle << 12;
				break;
			}
			// outside the BAR window: it moves in now (xe_bo_migrate on a CPU fault); from here on it stays (mapped). A
			// window with no room left (every BO in it CPU-mapped): to system memory instead (boMoveSys).
			for (;;) {
				if (bo->vram && (err = boMakeVisible(c, bo))) {
					if (err != ENOSPC) return err == EBUSY || err == EAGAIN ? EINVAL : err;
					if (!(bo = static_cast<XeBo *>(c->bos.get(m->handle)))) return ENOENT;
					const uint64_t until = nowNs() + 5000000000ull;
					while (bo->vram && bo->hi && (err = boMoveSys(c, bo))) {   // rb7s: EAGAIN (a busy lock) tried again
						if (!moveRetry(c, err, until)) return err == EBUSY || err == EAGAIN ? EINVAL : err;
						if (!(bo = static_cast<XeBo *>(c->bos.get(m->handle)))) return ENOENT;
					}
				}
				if (!(bo = static_cast<XeBo *>(c->bos.get(m->handle)))) return ENOENT;   // looked up again: the lock was dropped
				bo->mapped = true;
				// rb7s: Dekker pair with moveSetBegin (it stores migrating, then reads mapped): a move of this BO by another
				// client that started before it could see `mapped` is seen here, and waited for; then the BO is where that
				// move left it (back to the top: it may be outside the window now)
				__sync_synchronize();
				if (!bo->migrating) break;
				gShareSt.mapWaits++;
				boGet(bo);
				err = waitUntil(c, (int64_t)(nowNs() + 5000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
				boPut(bo);
				if (err) return err;
				if (!(bo = static_cast<XeBo *>(c->bos.get(m->handle)))) return ENOENT;
			}
			if (!bo->vram) {   // system memory: its own pages, one descriptor (bo->sys) for every map, found by handle
				m->offset = (uint64_t)m->handle << 12;
				break;
			}
			// The CPU must not see the VRAM before its clear has landed (a CPU fault waits for the BO's kernel fences).
			boGet(bo);
			const XeFence clr = bo->clear;
			err = waitUntil(c, (int64_t)(nowNs() + 5000000000ull), kWaitClear, [&] { return fenceSignaled(clr); });
			// rb7s: this client closed its handle(s) of it while the lock was dropped (WAS bo->closed: any client's close)
			const bool closed = c->bos.get(m->handle) != bo && !c->bos.find(bo);
			const uint32_t cookie = err || closed || !boVisible(bo) ? 0 : boMapNew(c, bo);
			boPut(bo);
			if (err) return err;
			if (!cookie) return closed ? ENOENT : ENOMEM;
			// Bit 63 tells xemac_mmap to ask for a write-combined mapping: IOConnectMapMemory64's caller picks the cache
			// mode (kIOMapUserOptionsMask covers it), and BAR2 defaults to uncached. The cookie (XeCpuMap) is bits 43:12,
			// the IOConnectMapMemory64 type.
			m->offset = (uint64_t)cookie << 12 | 1ull << 63;
			break;
		}
		case DRM_IOCTL_XE_VM_CREATE: {
			auto *v = reinterpret_cast<drm_xe_vm_create *>(buf);
			// rb7j: xe_vm_create_ioctl's checks: extensions and reserved must be zero, only ALL_DRM_XE_VM_CREATE_FLAGS,
			// FAULT_MODE only with LR_MODE, NO_VM_OVERCOMMIT only with FAULT_MODE. Long-running and fault mode are not
			// implemented here (preempt fences, the GPU page fault service): refused, so the remaining flag is SCRATCH_PAGE,
			// now honoured (Wa_22014953428, which forces it, is DG2-G10 only).
			constexpr uint32_t kAll = DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE | DRM_XE_VM_CREATE_FLAG_LR_MODE |
				DRM_XE_VM_CREATE_FLAG_FAULT_MODE | DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT;
			if (v->extensions || v->reserved[0] || v->reserved[1] || (v->flags & ~kAll)) return EINVAL;
			if ((v->flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE) && !(v->flags & DRM_XE_VM_CREATE_FLAG_LR_MODE)) return EINVAL;
			if ((v->flags & DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT) && !(v->flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE))
				return EINVAL;
			if (v->flags & (DRM_XE_VM_CREATE_FLAG_LR_MODE | DRM_XE_VM_CREATE_FLAG_FAULT_MODE)) return EINVAL;
			XeVm *vm = vmCreate(0, v->flags & DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE);
			if (!vm) return ENOMEM;
			v->vm_id = c->vms.add(vm);
			if (!v->vm_id) {
				vmPut(vm);
				return ENOMEM;
			}
			break;
		}
		case DRM_IOCTL_XE_VM_DESTROY: {
			auto *v = reinterpret_cast<drm_xe_vm_destroy *>(buf);
			if (v->pad || v->reserved[0] || v->reserved[1]) return EINVAL;   // xe_vm_destroy_ioctl
			auto *vm = static_cast<XeVm *>(c->vms.take(v->vm_id));
			if (!vm) return ENOENT;
			// rb7r: its bindings stop counting for their BOs' purgeable state now (xe_vm_close_and_put destroys the VMAs);
			// the records stay until the VM is freed, and while they do they keep those BOs from being purged
			for (uint32_t k = 0; k < vm->nBinds; k++) bindingUncounted(vm->binds[k].bo, vm->binds[k].purge);
			for (uint32_t i = 1; i <= c->queues.n; i++) {
				auto *q = static_cast<XeQueue *>(c->queues.get(i));
				if (q && !isBindQ(q) && q->vm == vm) queueClose(static_cast<XeQueue *>(c->queues.take(i)));
			}
			vmPut(vm);   // freed after its queues' teardown and a TLB invalidation (vmPut, deferReap)
			break;
		}
		case DRM_IOCTL_XE_VM_BIND: {
			auto *b = reinterpret_cast<drm_xe_vm_bind *>(buf);
			// rb7j: vm_bind_ioctl_check_args, before anything is done: unknown bits and fields that must be zero
			if (b->pad || b->pad2 || b->reserved[0] || b->reserved[1] || b->extensions) return EINVAL;
			// xe vm_bind_ioctl_check_args: the ops are copied from the caller once, and the checks and the binds use that
			// copy. WAS read from user memory again for each pass, so ops changed after the checks were bound unchecked.
			struct OpsCopy {
				drm_xe_vm_bind_op *p = nullptr; size_t size = 0;
				~OpsCopy() { if (p) IOFree(p, size); }
			} copy;
			const drm_xe_vm_bind_op *ops = &b->bind;
			if (b->num_binds > 1) {
				if (b->num_binds > kMaxBindOps) return ENOBUFS;   // xe answers ENOBUFS when it cannot allocate the array
				copy.size = (size_t)b->num_binds * sizeof(drm_xe_vm_bind_op);
				copy.p = static_cast<drm_xe_vm_bind_op *>(IOMalloc(copy.size));
				if (!copy.p) { copy.size = 0; return ENOBUFS; }
				if ((err = getUser(b->vector_of_binds, copy.p, copy.size))) return err;
				ops = copy.p;
			}
			for (uint32_t i = 0; i < b->num_binds; i++)
				if ((err = bindOpCheck(ops[i]))) return err;
			// rb8e: xe_vm_bind_ioctl: a named exec queue must exist (xe_exec_queue_lookup: ENOENT) and be a bind queue
			// (EXEC_QUEUE_FLAG_VM: EINVAL). WAS ignored: every call went to the VM's one timeline.
			if (b->exec_queue_id) {
				void *nq = c->queues.get(b->exec_queue_id);
				if (!nq) return ENOENT;
				if (!isBindQ(nq)) return EINVAL;
			}
			SyncList sl;   // xe_vm_bind_ioctl: SYNC_PARSE_FLAG_DISALLOW_USER_FENCE without binds
			if ((err = sl.load(b->syncs, b->num_syncs, b->num_binds ? 0 : kXeSyncParseDisallowUfence)) ||
				(err = waitSyncsIn(c, sl)))
				return err;
			if (c->migrating && (err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !c->migrating; })))
				return err;   // a BO of this client is moving: its bindings must stay as they are
			// rb7s: a MAP of a shared BO another client is moving waits for that move (new PTEs would point at the blocks
			// it frees). Checked under this lock, which is then held until the ops are done: a move of a shared BO
			// try-locks every client, this one included, so none can start in between. Only while such a move runs
			// (gSharedMoving, raised before the mover takes or releases any other client's lock).
			for (bool again = true; again && !err;) {
				again = false;
				if (c->migrating) {
					err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !c->migrating; });
					again = true;
					continue;
				}
				for (uint32_t i = 0; gSharedMoving > 0 && i < b->num_binds && !err && !again; i++) {
					const drm_xe_vm_bind_op &op = ops[i];
					if (op.op != DRM_XE_VM_BIND_OP_MAP || !op.obj) continue;
					auto *bo = static_cast<XeBo *>(c->bos.get(op.obj));
					if (!bo || !bo->migrating) continue;
					gShareSt.bindWaits++;
					boGet(bo);
					err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !bo->migrating; });
					boPut(bo);
					again = true;
				}
			}
			if (err) return err;
			auto *vm = static_cast<XeVm *>(c->vms.get(b->vm_id));   // after the waits: the lock was dropped
			if (!vm) return EINVAL;
			XeBindQueue *bq = nullptr;   // rb8e: looked up again likewise (destroyed meanwhile: ENOENT, nothing done)
			if (b->exec_queue_id && !(bq = bindQOf(c->queues.get(b->exec_queue_id)))) return ENOENT;
			BindCtx ctx = {};
			ctx.clear = {0, ~0u, 0, 0};
			for (uint32_t i = 0; i < b->num_binds && !err; i++) err = vmBindOp(c, vm, ops[i], &ctx);
			// PTE writes (BAR2, write-combined) out before anything uses them. rb8c: xe_device_wmb (sfence and a posted
			// register write), as xe_migrate_update_pgtables_cpu ends its CPU page-table update; WAS sfence alone, which
			// drains the CPU's write-combining buffers but, Linux notes for dGFX, does not order them for the GPU
			gDev->deviceWmb();
			OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(&vm->ptGen));   // next batch of each queue invalidates
			// A bind over scratch entries needs no invalidation here: the next batch's ring prologue invalidates the TLB
			// (batch_invalidate_tlb). What an unbind removed may still be in a TLB: the BOs it released are freed only
			// once this invalidation has completed.
			const bool unbound = ctx.unmapped || ctx.parked;
			uint32_t seq = 0;
			if (unbound) {
				seq = tlbInvalidateRange(vm->asid, ctx.lo, ctx.hi);
				deferSeal(&ctx, seq, vm);   // rb8c: and the VM's jobs of this moment
				vm->unbindNs = nowNs();   // rb8b: engineDump
				vm->unbinds++;
				vm->unbindTlb = seq;
			}
			if (b->num_binds) vm->bindNs = nowNs();
			vmKernAdd(vm, ctx.clear);   // the clears of the BOs just bound: later execs wait for them even without this out-fence
			// rb8d: the out-fence is the clears AND the invalidation of what this call unbound, the bind queue's earlier
			// binds folded in (xe_pt_update_ops_run: "tlb invalidation must be done before signaling unbind/rebind"; the
			// bind queue's fences signal in order). WAS ctx.clear alone: an unbind's out-fence signaled while the GPU
			// could still translate through the removed entries, so a range reused at once after waiting on it could be
			// reached through a stale TLB entry. A call without ops (xe_vm_bind_ioctl -ENODATA ->
			// vm_bind_ioctl_signal_fences) signals the last out-fence (xe_sync_in_fence_get: the queue's last fence; its
			// in-fences were waited for above). Nothing waits here: the fence signals when the G2H completes it.
			// rb8e: the bind queue is the one the call names, else the VM's default queue (xeBindLast,
			// to_wait_exec_queue); WAS the VM's for every call. Binds of other queues are not ordered behind it (each
			// queue invalidates in its ring before its next batch after any page-table change, ptGen, and unbound
			// memory waits for its own invalidation and the VM's jobs, deferSeal).
			XeBindFence *last = xeBindLast(bq ? &bq->last : nullptr, &vm->bindOut);
			const XeBindFence out = xeBindFenceNext(*last, ctx.clear, unbound, seq, fenceSignaled);
			*last = out;
			if (bq) gBindQSt.binds++;
			XeFence parts[2];
			const uint32_t nParts = xeBindFenceParts(out, parts, fenceSignaled);
			if (unbound) gBindSt.unbinds++;
			if (!fenceSignaled(out.tlb)) {
				gBindSt.tlbPending++;   // the out-fence still waits for an invalidation as the ioctl returns
				if (!unbound) gBindSt.tlbCarried++;   // an earlier unbind's, carried by a later call (zero-op calls too)
			}
			if (!b->num_binds) gBindSt.noOps++;
			// Signal the out-fences even when an op failed: Mesa allocates a bind-timeline point per bind and every
			// later exec waits on it; a failed userptr import (ENOMEM) left the point unmaterialized and WindowServer's
			// next exec waited for it forever (watchdog kill, 2026-10-28).
			if (int serr = signalSyncsOut(c, sl, parts, nParts); !err) err = serr;
			// the user fence only after a bind that succeeded, out-fences included: xe parses every sync before it binds,
			// unwinds a failed bind and signals nothing
			if (!err && sl.ufence >= 0) bindUfenceSignal(c, sl.s[sl.ufence], out);
			break;
		}
		case DRM_IOCTL_XE_EXEC_QUEUE_CREATE: {
			auto *e = reinterpret_cast<drm_xe_exec_queue_create *>(buf);
			// rb7j: xe_exec_queue_create_ioctl's checks: only DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT, reserved zero,
			// 1 <= width * num_placements <= XE_HW_ENGINE_MAX_INSTANCE (9); parallel (width > 1) submission is not
			// implemented (xe allows it on video classes): EOPNOTSUPP, as xe answers for classes without multi-LRC.
			// Placements (rb7o): on the media GT's classes (video decode, video enhance) every placement goes into the
			// queue's logical mask as xe_exec_queue_create_ioctl -> calc_validate_logical_mask builds it (each one an
			// engine that exists, all of the first one's class and GT, BIT(engine_instance) of each), and the GuC runs
			// the context on whichever of those engines is free. Mesa ANV asks for every engine of the queue family's
			// class. debug.intelxe.vcs_single 1: the first placement only, as before rb7o. Render, copy and compute queues
			// keep using the first placement only (their behaviour is unchanged).
			if ((e->flags & ~(uint32_t)DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT) || e->reserved[0] || e->reserved[1]) return EINVAL;
			const uint64_t len = (uint64_t)e->width * e->num_placements;
			if (!len || len > 9) return EINVAL;
			drm_xe_engine_class_instance eci, place[9];
			if ((err = getUser(e->instances, &eci, sizeof(eci)))) return err;
			if (eci.gt_id > 1) return EINVAL;   // xe_device_get_gt
			if (eci.engine_class == DRM_XE_ENGINE_CLASS_VM_BIND) {
				if (e->width != 1 || e->num_placements != 1 || eci.engine_instance) return EINVAL;
				if (!c->vms.get(e->vm_id)) return ENOENT;
				QueueProps props;   // a bind queue's properties are checked, then have nothing to act on
				if ((err = queueProps(e->extensions, eci.engine_class, &props))) return err;
				// binds are synchronous: the queue is its last fence (rb8e), signaled to begin with
				auto *bq = static_cast<XeBindQueue *>(IOMallocZero(sizeof(XeBindQueue)));
				if (!bq) return ENOMEM;
				bq->last = {{0, ~0u, 0, 0}, {0, ~0u, 0, 0}};
				e->exec_queue_id = c->queues.add(bindQTag(bq));
				if (!e->exec_queue_id) {
					IOFree(bq, sizeof(*bq));
					return ENOMEM;
				}
				gBindQSt.made++;
				break;
			}
			if (e->width != 1) return EOPNOTSUPP;
			const bool media = eci.engine_class == DRM_XE_ENGINE_CLASS_VIDEO_DECODE ||
				eci.engine_class == DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE;
			uint32_t mask = 0;   // 0: the first placement alone
			if (media && e->num_placements > 1) {
				if ((err = getUser(e->instances, place, (size_t)len * sizeof(place[0])))) return err;
				if (memcmp(&place[0], &eci, sizeof(eci))) return EINVAL;   // read twice: the first must not have changed
				for (uint32_t i = 0; i < len; i++) {
					// calc_validate_logical_mask: xe_hw_engine_lookup must find it (engineFor also brings the media GT
					// up), same gt_id and class as the first; the mask is in logical ids (the uAPI's engine_instance)
					if (place[i].gt_id != eci.gt_id || place[i].engine_class != eci.engine_class || place[i].engine_instance >= 32 ||
						!engineFor(place[i].engine_class, place[i].engine_instance)) return EINVAL;
					mask |= 1u << place[i].engine_instance;
				}
				if (gVcsSingle) mask = 0;
			}
			QueueProps props;
			props.lowLatency = e->flags & DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT;
			if ((err = queueProps(e->extensions, eci.engine_class, &props))) return err;
			auto *vm = static_cast<XeVm *>(c->vms.get(e->vm_id));
			if (!vm) return ENOENT;
			XeQueue *q;
			if ((err = queueCreate(vm, eci.engine_class, eci.engine_instance, &q, &props, mask))) return err;
			q->pid = c->pid;
			strlcpy(q->name, c->name, sizeof(q->name));
			if (media) {
				const bool multi = q->logicalMask & (q->logicalMask - 1);
				OSIncrementAtomic(reinterpret_cast<volatile SInt32 *>(multi ? &gStats.vqMulti : &gStats.vqSingle));
				LOG("video queue guc_id %u (%s, pid %d): class %u, %u placement%s, logical mask 0x%x%s", q->gucId, q->name,
					q->pid, q->cls, e->num_placements, e->num_placements == 1 ? "" : "s", q->logicalMask,
					gVcsSingle && e->num_placements > 1 ? " (debug.intelxe.vcs_single)" : "");
			}
			e->exec_queue_id = c->queues.add(q);
			if (!e->exec_queue_id) {
				queueClose(q);
				return ENOMEM;
			}
			break;
		}
		case DRM_IOCTL_XE_EXEC_QUEUE_DESTROY: {
			auto *e = reinterpret_cast<drm_xe_exec_queue_destroy *>(buf);
			if (e->pad || e->reserved[0] || e->reserved[1]) return EINVAL;   // xe_exec_queue_destroy_ioctl
			void *q = c->queues.take(e->exec_queue_id);
			if (!q) return ENOENT;
			if (XeBindQueue *bq = bindQOf(q)) {   // rb8e: nothing runs on it; its last fence lives on in the syncobjs
				IOFree(bq, sizeof(*bq));
				gBindQSt.gone++;
			} else {
				queueClose(static_cast<XeQueue *>(q));
			}
			break;
		}
		case DRM_IOCTL_XE_VM_GET_PROPERTY: {
			// rb7j: xe_vm_get_property_ioctl. FAULTS: size 0 asks for the size of the VM's fault list (struct
			// xe_vm_fault each); otherwise size must be a multiple of it and at most that, and the first size / 48
			// entries are copied, addresses canonical (xe_device_canonicalize_addr, 48-bit VA). Mesa probes support
			// with vm_id -1 and wants ENOENT.
			auto *g = reinterpret_cast<drm_xe_vm_get_property *>(buf);
			if (g->reserved[0] || g->reserved[1] || g->reserved[2] || g->extensions || g->pad) return EINVAL;
			auto *vm = static_cast<XeVm *>(c->vms.get(g->vm_id));
			if (!vm) return ENOENT;
			if (g->property != DRM_XE_VM_GET_PROPERTY_FAULTS) return EINVAL;
			IOLockLock(gFaultLock);
			const uint32_t n = vm->nFaults;
			IOLockUnlock(gFaultLock);
			const uint32_t size = n * (uint32_t)sizeof(xe_vm_fault);
			if (!g->size) {
				g->size = size;
				break;
			}
			if (g->size > size || g->size % sizeof(xe_vm_fault)) return EINVAL;
			const uint32_t count = g->size / (uint32_t)sizeof(xe_vm_fault);
			auto *out = static_cast<xe_vm_fault *>(IOMallocZero(g->size));
			if (!out) return ENOMEM;
			IOLockLock(gFaultLock);   // the list only grows while the VM lives (the client lock keeps it alive)
			for (uint32_t i = 0; i < count; i++) {
				const XeVm::Fault &f = vm->faults[i];
				out[i].address = (uint64_t)((int64_t)(f.addr << 16) >> 16);
				out[i].address_precision = 0x1000;   // SZ_4K, as xe for now
				out[i].access_type = f.access;
				out[i].fault_type = f.type;
				out[i].fault_level = f.level;
				out[i].srcid = f.srcid;
			}
			IOLockUnlock(gFaultLock);
			err = putUser(g->data, out, g->size);
			IOFree(out, g->size);
			if (err) return err;
			break;
		}
		case DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY: {
			auto *p = reinterpret_cast<drm_xe_exec_queue_get_property *>(buf);
			if (p->reserved[0] || p->reserved[1]) return EINVAL;   // xe_exec_queue_get_property_ioctl
			auto *q = static_cast<XeQueue *>(c->queues.get(p->exec_queue_id));
			if (!q) return ENOENT;
			if (p->property != DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN) return EINVAL;
			p->value = !isBindQ(q) && q->banned;
			break;
		}
		case DRM_IOCTL_XE_EXEC: {
			if (gJobTime && gJtEngBase) gJtExecTs = jtNow(gJtEngBase);
			auto *x = reinterpret_cast<drm_xe_exec *>(buf);
			// xe_exec_ioctl: extensions, pad and reserved zero (WAS: not checked)
			if (x->extensions || x->pad[0] || x->pad[1] || x->pad[2] || x->reserved[0] || x->reserved[1]) return EINVAL;
			if (x->num_batch_buffer > 1) return EINVAL;
			SyncList sl;
			if ((err = sl.load(x->syncs, x->num_syncs, kXeSyncParseExec))) return err;
			if (isBindQ(c->queues.get(x->exec_queue_id))) {   // bind queue: binds are synchronous
				if (sl.ufence >= 0) return EINVAL;   // a GPU address without a job to write it (xe: no exec on a VM queue)
				if ((err = waitSyncsIn(c, sl))) return err;
				// rb8e: its out-syncs get the queue's last bind fence, as an EXEC without batches does on any exec queue
				// (xe_exec.c: xe_sync_in_fence_get(syncs, num_syncs, q, vm), the in-fences waited for above). WAS signaled
				// at once, before an unbind on the queue had its invalidation. Looked up again: the lock was dropped.
				XeBindQueue *bq = bindQOf(c->queues.get(x->exec_queue_id));
				if (!bq) return EINVAL;
				XeFence parts[2];
				const uint32_t nParts = xeBindFenceParts(bq->last, parts, fenceSignaled);
				gBindQSt.execs++;
				err = signalSyncsOut(c, sl, parts, nParts);
				break;
			}
			DepList deps;
			if ((err = collectDeps(c, sl, deps))) return err;
			if (c->migrating && (err = waitUntil(c, (int64_t)(nowNs() + 10000000000ull), kWaitMigrate, [&] { return !c->migrating; })))
				return err;   // a BO of this client is moving: its VMs are reserved
			auto *q = static_cast<XeQueue *>(c->queues.get(x->exec_queue_id));   // after the waits: the lock was dropped
			if (!q || isBindQ(q)) return EINVAL;
			if (q->banned) return ECANCELED;
			// behind the kernel's operations on the VM's BOs (their clears), as xe_exec waits on the VM resv KERNEL slot:
			// an exec that did not wait on its bind's out-fence ran before a fresh BO's clear, which then zeroed what the
			// batch wrote (bomove, 2026-10-29). Mesa always waited (its bind timeline), so it never saw this.
			if (!fenceSignaled(q->vm->kern) && !deps.add(q->vm->kern)) return ENOMEM;
			// drm_sched_entity_add_dependency_cb: a fence of this queue itself is already ordered before the job (the
			// ring runs jobs in order, each ending in a CS-stalling flush), so it is not waited for; the job then goes
			// to the ring at once instead of waiting in the FIFO for the previous job's completion.
			uint32_t keep = 0;
			for (uint32_t i = 0; i < deps.n; i++) {
				if (deps.f[i].slot == q->slot && deps.f[i].gen == q->gen) gStats.sameQueueDeps++;
				else deps.f[keep++] = deps.f[i];
			}
			deps.n = keep;
#if INTELXE_DEBUG
			if (x->num_batch_buffer && gDumpExec) dumpBatch(q->vm, x->address, q->eng->drmClass);
#endif
			XeFence f;
			XeUfence uf = {};
			if (sl.ufence >= 0) {   // xe_sched_job_init_user_fence: addr, and the value in timeline_value
				uf = {sl.s[sl.ufence].addr, sl.s[sl.ufence].timeline_value, true};
				if (x->num_batch_buffer) gUfSt.execs++;
			}
			if ((err = queueExec(q, x->num_batch_buffer ? x->address : 0, deps, &f, uf))) return err;
			err = signalSyncsOut(c, sl, f);
			break;
		}
		case DRM_IOCTL_SYNCOBJ_CREATE: {
			auto *s = reinterpret_cast<drm_syncobj_create *>(buf);
			if (s->flags & ~(uint32_t)DRM_SYNCOBJ_CREATE_SIGNALED) return EINVAL;   // drm_syncobj_create_ioctl
			auto *so = syncNew();   // rb7t: one reference, the handle's
			if (!so) return ENOMEM;
			if (s->flags & DRM_SYNCOBJ_CREATE_SIGNALED) syncReplace(so, XeFence{0, ~0u, 0, 0});
			s->handle = c->syncs.add(so);
			if (!s->handle) {
				syncPut(so);
				return ENOMEM;
			}
			break;
		}
		case DRM_IOCTL_SYNCOBJ_DESTROY: {
			auto *s = reinterpret_cast<drm_syncobj_destroy *>(buf);
			if (s->pad) return EINVAL;   // drm_syncobj_destroy_ioctl
			auto *so = static_cast<XeSyncobj *>(c->syncs.take(s->handle));
			if (!so) return ENOENT;
			syncPut(so);   // rb7t: the handle's reference; an fd or another client's handle may keep it
			break;
		}
		case DRM_IOCTL_SYNCOBJ_WAIT: {
			auto *w = reinterpret_cast<drm_syncobj_wait *>(buf);
			err = syncobjWait(c, w->handles, 0, w->count_handles, w->flags, w->timeout_nsec, &w->first_signaled);
			break;
		}
		case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
			auto *w = reinterpret_cast<drm_syncobj_timeline_wait *>(buf);
			err = syncobjWait(c, w->handles, w->points, w->count_handles, w->flags, w->timeout_nsec, &w->first_signaled);
			break;
		}
		case DRM_IOCTL_SYNCOBJ_RESET:
		case DRM_IOCTL_SYNCOBJ_SIGNAL: {
			auto *a = reinterpret_cast<drm_syncobj_array *>(buf);
			if (a->pad || !a->count_handles) return EINVAL;   // drm_syncobj_reset_ioctl / drm_syncobj_signal_ioctl
			for (uint32_t i = 0; i < a->count_handles && !err; i++) {
				uint32_t h;
				if ((err = getUser(a->handles + i * 4, &h, 4))) break;
				auto *so = static_cast<XeSyncobj *>(c->syncs.get(h));
				if (!so) {
					err = ENOENT;
					continue;
				}
				SyncGuard g(so);
				if (request == DRM_IOCTL_SYNCOBJ_RESET) so->n = 0;
				else syncReplace(so, XeFence{0, ~0u, 0, 0});
			}
			wakeAll();
			break;
		}
		case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
		case DRM_IOCTL_SYNCOBJ_QUERY: {
			auto *a = reinterpret_cast<drm_syncobj_timeline_array *>(buf);
			// drm_syncobj_timeline_signal_ioctl: no flags; drm_syncobj_query_ioctl: LAST_SUBMITTED only; both: handles
			if (!a->count_handles) return EINVAL;
			if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL ? a->flags != 0 : (a->flags & ~(uint32_t)DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED))
				return EINVAL;
			for (uint32_t i = 0; i < a->count_handles && !err; i++) {
				uint32_t h;
				uint64_t p;
				if ((err = getUser(a->handles + i * 4, &h, 4))) break;
				auto *so = static_cast<XeSyncobj *>(c->syncs.get(h));
				if (!so) {
					err = ENOENT;
					break;
				}
				if (request == DRM_IOCTL_SYNCOBJ_QUERY) {
					{
						SyncGuard g(so);
						p = a->flags & DRM_SYNCOBJ_QUERY_FLAGS_LAST_SUBMITTED ? syncLastSubmitted(so) : syncValue(so);
					}
					err = putUser(a->points + i * 8, &p, 8);
				} else if (!(err = getUser(a->points + i * 8, &p, 8))) {
					SyncGuard g(so);
					if (p) syncAdd(so, XeFence{p, ~0u, 0, 0});
					else syncReplace(so, XeFence{0, ~0u, 0, 0});
				}
			}
			if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL) wakeAll();
			break;
		}
		case DRM_IOCTL_SYNCOBJ_TRANSFER: {
			auto *t = reinterpret_cast<drm_syncobj_transfer *>(buf);
			if (t->pad) return EINVAL;   // drm_syncobj_transfer_ioctl
			auto *src = static_cast<XeSyncobj *>(c->syncs.get(t->src_handle));
			auto *dst = static_cast<XeSyncobj *>(c->syncs.get(t->dst_handle));
			XeFence f;
			if (!src || !dst) return ENOENT;
			// rb7t: src and dst one after the other (each may be shared: gSyncLock is never nested). A binary src with a
			// sync file's several fences (syncReplaceN): those, as the one fence Linux has there. rb8d: a timeline src
			// point likewise stands for every unsignaled fence at or below it (drm_syncobj_find_fence gives the
			// dma_fence_chain node, which signals once it and every earlier point have; a bind's clear and invalidation
			// share one point). WAS the first fence at or above the point alone: the others were dropped.
			XeFence many[kSyncFileMax];
			uint32_t nMany = 0;
			{
				SyncGuard g(src);
				if (t->src_point) syncValue(src);   // prune signaled history
				if (!syncFind(src, t->src_point, &f)) {
					if (t->src_point && syncValue(src) >= t->src_point) f = XeFence{0, ~0u, 0, 0};
					else return EINVAL;
				} else if (t->src_point || syncBinSpan(src) > 1) {
					const uint32_t span = t->src_point ? src->n : syncBinSpan(src);
					for (uint32_t k = 0; k < span; k++)   // at most kSyncFileMax queues (a sync file's limit as well)
						if (!t->src_point || src->f[k].point <= f.point) fenceMerge(many, &nMany, kSyncFileMax, src->f[k]);
					if (nMany == 1) f = many[0];
					else if (!nMany) f = XeFence{0, ~0u, 0, 0};
					if (nMany < 2) nMany = 0;
				}
			}
			SyncGuard g(dst);
			if (nMany && !t->dst_point) {
				syncReplaceN(dst, many, nMany);
			} else if (nMany) {   // a timeline point of several fences: each at that point (it is reached when all are)
				for (uint32_t k = 0; k < nMany; k++) syncAdd(dst, XeFence{t->dst_point, many[k].slot, many[k].gen, many[k].seqno});
			} else if (t->dst_point) {
				f.point = t->dst_point;
				syncAdd(dst, f);
			} else {
				syncReplace(dst, f);
			}
			wakeAll();
			break;
		}
		case DRM_IOCTL_INTELXE_SURFACE_FENCE_ATTACH:
			err = surfaceAttach(c, *reinterpret_cast<drm_intelxe_surface_fence_attach *>(buf));
			break;
		case DRM_IOCTL_INTELXE_SURFACE_FENCE_IMPORT:
			err = surfaceImport(c, *reinterpret_cast<drm_intelxe_surface_fence_import *>(buf));
			break;
		case DRM_IOCTL_INTELXE_SURFACE_FENCE_WAIT:
			err = surfaceWait(c, *reinterpret_cast<drm_intelxe_surface_fence_wait *>(buf));
			break;
		case DRM_IOCTL_PRIME_HANDLE_TO_FD:
		case DRM_IOCTL_PRIME_FD_TO_HANDLE:
		case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
		case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
		case DRM_IOCTL_INTELXE_OBJ_INFO:   // rb7t
			err = objFdIoctl(c, request, buf);
			break;
		default:
			return ENOTTY;
	}
	return err;
}

// ─── module state: locks and thread calls (IntelXe_start / IntelXe_stop), statistics ──────────────────────────────────
// debug.intelxe.fd_stats: the device's records (xe_fd.cpp) and the objects behind them, for leak checks: after every
// holder has closed, "minors open" and the alive counts of fd-only objects return to what they were.
int xeFdStats(char *out, size_t len) {
	int n = xeFdStatsString(out, len);
	if (n < 0 || (size_t)n >= len) return n;
	return n + snprintf(out + n, len - (size_t)n,
		"objects: syncobjs alive %d (exported to an fd %d), sync files alive %d, exported BOs alive %d\n"
		"PRIME exports %u, imports %u (same handle again %u, by another client: noMove %u); syncobj exports %u, imports %u; "
		"sync file exports %u, imports %u\n",
		(int)gSyncLive, (int)gSharedSyncLive, (int)gSyncFileLive, (int)gExportedBoLive, gFdShare.primeExports,
		gFdShare.primeImports, gFdShare.primeSame, gFdShare.foreign, gFdShare.syncExports, gFdShare.syncImports,
		gFdShare.fileExports, gFdShare.fileImports);
}

bool xeCoreInit() {
	gSyncLock = IOLockAlloc();   // rb7t
	gResLock = IOLockAlloc();
	gSchedLock = IOLockAlloc();
	gClientsLock = IOLockAlloc();
	gTlbLock = IOLockAlloc();
	gWaitLock = IOLockAlloc();
	gUfaLock = IOLockAlloc();   // rb8e
	gWorkCall = thread_call_allocate_with_options(workRun, nullptr, THREAD_CALL_PRIORITY_KERNEL_HIGH, THREAD_CALL_OPTIONS_ONCE);
	gReapCall = thread_call_allocate_with_options(reapRun, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	gWatchCall = thread_call_allocate_with_options(watchRun, nullptr, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	gSurfLock = IOLockAlloc();
	gFaultLock = IOLockAlloc();
	gNameLock = IOLockAlloc();
	gBoLock = IOSimpleLockAlloc();
	gSurf = static_cast<SurfEntry *>(IOMallocZero(kSurfEntries * sizeof(SurfEntry)));
	return gResLock && gSchedLock && gClientsLock && gTlbLock && gWaitLock && gUfaLock && gWorkCall && gReapCall && gWatchCall && gSurfLock &&
		gFaultLock && gNameLock && gBoLock && gSurf && gSyncLock && xeFdInit();
}

void xeCoreFini() {
	thread_call_t *calls[] = {&gWorkCall, &gReapCall, &gWatchCall, &gDsCall};
	for (thread_call_t *t : calls)
		if (*t) {
			thread_call_cancel_wait(*t);
			thread_call_free(*t);
			*t = nullptr;
		}
	if (gSurf) IOFree(gSurf, kSurfEntries * sizeof(SurfEntry));
	gSurf = nullptr;
	while (XeUfAsync *e = gUfaHead) {   // rb8e: none once every client has closed; unwired all the same
		gUfaHead = e->next;
		ufAsyncFree(e);
	}
	gUfaTail = &gUfaHead;
	gUfaN = 0;
	IOLock **locks[] = {&gResLock, &gSchedLock, &gClientsLock, &gTlbLock, &gWaitLock, &gSurfLock, &gMoveLock, &gFaultLock, &gNameLock,
		&gSyncLock, &gUfaLock};
	for (IOLock **l : locks)
		if (*l) {
			IOLockFree(*l);
			*l = nullptr;
		}
	if (gBoLock) IOSimpleLockFree(gBoLock);
	gBoLock = nullptr;
	gNames.free();
}

int xeStatsString(char *out, size_t len) {
	uint32_t live = 0, waiting = 0, jobs = 0;
	IOLockLock(gSchedLock);
	const int dsArmedId = gDsQ ? (int)gDsQ->gucId : -1;
	for (uint32_t g = 0; g < 2; g++)
		for (uint32_t i = 0; i < kMaxGucIds; i++)
			if (gQueueByGuc[g][i]) live++;
	for (XeQueue *q = gSchedHead; q; q = q->schedNext) {
		waiting++;
		for (XeJob *j = q->jobHead; j; j = j->next) jobs++;
	}
	IOLockUnlock(gSchedLock);
	uint32_t ggttRanges = 0;
	uint64_t ggttRangeBytes = 0;
	IOLockLock(gResLock);
	ggttRangeStats(&ggttRanges, &ggttRangeBytes);
	IOLockUnlock(gResLock);
	const IntelXeStats &st = gStats;
	const uint32_t *m = st.missedBy;
	uint32_t pinsNow = 0;
	IOLockLock(gResLock);
	for (XeBo *bo : gPinBos) pinsNow += bo ? 1 : 0;
	IOLockUnlock(gResLock);
	const int head = snprintf(out, len,
		"system sleep: suspends %u, resumes %u; last: %u VRAM BOs saved (%u only through a scan-out pin), %llu MB, %u "
		"without a backup, save copy %d; device resume %d, %u restored, restore copy %d; scan-out pins now %u (table full %u); "
		"copy windows sealed with an awaited invalidation %u, timed out %u\n"
		"LRCs: %u in VRAM, %u in system memory (debug.intelxe.lrc_vram %d), VRAM asked but not had %u; last sleep: %u saved, %u "
		"without memory\n",
		gPmSt.suspends, gPmSt.resumes, gPmSt.bos, gPmSt.pinned, gPmSt.mb, gPmSt.lost, gPmSt.saveErr, gPmSt.devErr,
		gPmSt.restored, gPmSt.restoreErr, pinsNow, gPinOverflow, gPmSt.sealWaits, gPmSt.sealTimeouts, gLrcVramN, gLrcSysN, gLrcVram, gLrcVramFallbacks, gLrcSaved,
		gLrcSaveFails);
	if (head < 0 || (size_t)head >= len) return head;
	out += head;
	len -= (size_t)head;
	// rb7o: video queues by logical mask and the user interrupts of each video engine (debug.intelxe.vcs has the details)
	const uint32_t *vi = gStats.mediaUserIrqs[0];
	const int video = snprintf(out, len, "video queues: multi-engine %u, single-engine %u (vcs_single %d); user interrupts "
		"VCS0 %u VCS1 %u VCS2 %u VCS3 %u\n", gStats.vqMulti, gStats.vqSingle, gVcsSingle, vi[0], vi[1], vi[2], vi[3]);
	if (video < 0 || (size_t)video >= len) return head + video;
	out += video;
	len -= (size_t)video;
	const int uf = snprintf(out, len, "user fences: execs %u; binds written %u, faulted %u, bind fence late %u; waits %u: met "
		"%u, timed out %u, faulted %u, queue reset %u\n"
		"bind out-fences (rb8d): unbinds %u (fence includes the invalidation), invalidation pending at return %u (an "
		"earlier call's %u), calls without ops %u; GGTT invalidations: GuC %u, MMIO %u, waited %u (max %u us), timed out %u; "
		"TLB invalidations counted done unanswered %u (%u times)\n"
		"recovery (rb8d): CAT errors %u, GuC context resets of a primary engine %u; GT resets for a job stuck after one %u "
		"(taken off instead %u), for a job never started in %llu ms %u\n"
		"bind user fences (rb8e): signaled at return %u; written asynchronously %u of %u (pending %u, at most %u, mean %llu "
		"us, max %u us), dropped at close %u; waited in the ioctl %u (page not wired %u, list full %u)\n"
		"bind queues (rb8e): made %u, gone %u; binds on one %u, EXECs on one %u\n"
		"migrate windows (rb8e): sealed move %u, surface %u, VRAM read %u; invalidation awaited %u (mean %llu us, max %u "
		"us); pages kept move %u, surface %u, VRAM read %u; TLB fence timeout %llu ms\n", gUfSt.execs, gUfSt.binds, gUfSt.bindFaults, gUfSt.bindLate,
		gUfSt.waits, gUfSt.met, gUfSt.timeouts, gUfSt.faults, gUfSt.resets,
		gBindSt.unbinds, gBindSt.tlbPending, gBindSt.tlbCarried, gBindSt.noOps, gGgttSt.guc, gGgttSt.mmio, gGgttSt.waits,
		gGgttSt.maxWaitUs, gGgttSt.timeouts, gTlbSt.forced, gTlbSt.forcedEvents, gRecSt.catErrors, gRecSt.domResets,
		gRecSt.stuckResets, gRecSt.stuckOff, (unsigned long long)xeNotStartedLimitMs(gJobTimeoutMs > 0 ? (uint64_t)gJobTimeoutMs : 0),
		gRecSt.notStartedResets,
		gUfaSt.direct, gUfaSt.written, gUfaSt.pinned, (uint32_t)gUfaN, gUfaSt.maxPending,
		(unsigned long long)(gUfaSt.written ? gUfaSt.us / gUfaSt.written : 0), gUfaSt.maxUs, gUfaSt.dropped, gUfaSt.waited,
		gUfaSt.pinFails, gUfaSt.full,
		gBindQSt.made, gBindQSt.gone, gBindQSt.binds, gBindQSt.execs,
		gMigWinSt.sealed[kMigWinMove], gMigWinSt.sealed[kMigWinSurf], gMigWinSt.sealed[kMigWinRead], gMigWinSt.awaited,
		(unsigned long long)(gMigWinSt.awaited ? gMigWinSt.us / gMigWinSt.awaited : 0), gMigWinSt.maxUs,
		gMigWinSt.kept[kMigWinMove], gMigWinSt.kept[kMigWinSurf], gMigWinSt.kept[kMigWinRead],
		(unsigned long long)(kTlbTimeoutNs / 1000000ull));
	if (uf < 0 || (size_t)uf >= len) return head + video + uf;
	out += uf;
	len -= (size_t)uf;
	return head + video + uf + snprintf(out, len,
		"irqs %u (GT %u, display %u); G2H %u + media %u, FAST_REQUEST failures %u\n"
		"waits: fallback wakeups %u, completed by the fallback (missed interrupt) %u; raced the wakeup %u, G2H read by the "
		"fallback %u; interrupt identities not valid %u\n"
		"missed by wait: sync %u clear %u tlb %u teardown %u ring %u submit %u deps %u kernel %u surface %u migrate %u\n"
		"jobs: direct %u, via worker %u, now waiting %u in %u queues; worker runs %u; bans %u (watchdog %u); same-queue "
		"in-fences skipped %u\n"
		"direct submission (debug.intelxe.direct %d, idle %d us): submits %u, arms %u; exits idle %u, contention %u, other "
		"%u; armed guc_id %d\n"
		"TDR: checks %u, re-enabled %u, never started %u; GuC context resets %u; GT resets %u%s (queues banned %u, "
		"replayed %u); H2G full %u, dead %u; page faults %u (lost %u)%s\n"
		"queues: live %u, freed %u, leaked %u; GGTT: system mappings %u, display ranges %u/64 (%llu MB)\n"
		"TLB invalidations %u (ranged %u, done %u/%u), timeouts %u, in-ring %u; deferred frees %u, pending %u, held by jobs "
		"%u; PT pages %u, splits %u, detached %u, leaked %u; userptr read-only %u\n"
		"VRAM end 0x%llx, used %llu MB (window %llu MB), free %llu + %llu MB; clears %u; system BOs %lld MB\n"
		"surface fences: attached %u, imported %u, ENOSPC %u, surfaces tracked %u\n"
		"BO moves: into the window %u, out of it %u (%llu MB), failed %u, busy %u; eviction scans %u\n"
		"window: refusals %u, page-table allocation failures %u; spills to system memory: window %u (%llu MB), VRAM %u; "
		"CPU maps made %u, gone %u (debug.intelxe.vram); maps of BOs the full window could not take: %u moved to system memory, %u failed\n",
		st.irqs, st.gtIrqs, st.displayIrqs, st.g2h[0], st.g2h[1], st.h2gFailed,
		st.fallbackWakes, st.rescued, st.raced, st.g2hFound, st.identTimeouts,
		m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9],
		st.jobsDirect, st.jobsDeferred, jobs, waiting, st.workerRuns, st.bans, st.watchdogBans, st.sameQueueDeps,
		gDirect, gDirectIdleUs, st.dsSubmits, st.dsArms, st.dsIdleExits, st.dsContendExits, st.dsOtherExits, dsArmedId,
		st.tdrChecks, st.tdrRearms, st.tdrNotStarted, st.gucCtxResets, st.gtResets, gResetting ? " (stopped)" : "",
		st.resetBans, st.resetReplays, st.h2gFull, st.h2gDead, st.pageFaults, st.pageFaultsLost, gWedged ? "; WEDGED" : "",
		live, st.queuesFreed, st.queuesLeaked, ggttRecCount(), ggttRanges, ggttRangeBytes >> 20,
		st.tlbInvals, st.tlbRanged, gDev ? gDev->tlbDone : 0, gDev ? gDev->mTlbDone : 0, st.tlbTimeouts, st.tlbRingFlushes,
		st.deferredFrees, gNDef, st.deferJobHeld, gPtPages, gPtSt.splits, gPtSt.detached, gPtSt.leaked, st.userptrRo,
		vramEnd, gVramUsed >> 20, gVisibleUsed >> 20, gVis.freeBytes >> 20, gHi.freeBytes >> 20, st.clears,
		(long long)(gSysUsed >> 20),
		st.surfAttach, st.surfImport, st.surfFull, gSurfCount,
		st.movesIn, st.movesOut, st.movedBytes >> 20, st.moveFails, st.moveBusy, st.evictScans,
		st.reserveHits, st.ptFails, st.winSpills, st.winSpillBytes >> 20, st.vramSpills, st.cpuMapsMade, st.cpuMapsGone,
		st.mapSpills, st.mapSpillFails);
}

// ─── rb7o: video engines ─────────────────────────────────────────────────────────────────────────────────────────
// debug.intelxe.vcs: what the driver knows without touching the GPU: the media GT's video engines (logical id, physical
// instance, user interrupts each raised: every job ends with MI_USER_INTERRUPT on the engine that ran it), and every
// live media queue with its logical mask and the engine its last job started on (seqno slot dword 3, ENGINE_ID).
int xeVcsString(char *out, size_t len) {
	size_t n = 0;
	auto put = [&](const char *fmt, ...) {
		if (n >= len) return;
		va_list ap;
		va_start(ap, fmt);
		const int k = vsnprintf(out + n, len - n, fmt, ap);
		va_end(ap);
		if (k > 0) n = n + (size_t)k < len ? n + (size_t)k : len - 1;
	};
	put("vcs_single %d; video queues created: multi-engine %u, single-engine %u; media GT %s\n", gVcsSingle,
		gStats.vqMulti, gStats.vqSingle, gDev->mediaOn() ? "up" : "not up");
	for (uint32_t i = 0; i < gNumMediaEngines; i++) {
		const XeEngine &e = gMediaEngines[i];
		const bool vcs = e.drmClass == DRM_XE_ENGINE_CLASS_VIDEO_DECODE;
		put("%s%u: logical %u, mmio 0x%x, user interrupts %u\n", vcs ? "VCS" : "VECS", e.phys, e.logical, e.base,
			e.phys < 8 ? gStats.mediaUserIrqs[vcs ? 0 : 1][e.phys] : 0);
	}
	struct Row { uint32_t gucId, mask, done, submitted, eid; int pid; uint16_t cls; bool banned, kernel; char name[16]; };
	constexpr uint32_t kRows = 24;
	Row rows[kRows];
	uint32_t nr = 0, more = 0;
	IOLockLock(gSchedLock);
	for (uint32_t i = 0; i < kMaxGucIds; i++) {
		XeQueue *q = gQueueByGuc[1][i];
		if (!q) continue;
		if (nr == kRows) {
			more++;
			continue;
		}
		Row &r = rows[nr++];
		r = {q->gucId, q->logicalMask, seqnoOf(q->slot), q->submitted, slotWord(q->slot, 3), q->pid, q->cls, q->banned, q->kernel, {}};
		strlcpy(r.name, q->name, sizeof(r.name));
	}
	IOLockUnlock(gSchedLock);
	put("live media queues: %u%s\n", nr + more, nr + more ? " (guc_id, process, class, logical mask, seqno done/in ring, "
		"engine the last job started on)" : "");
	for (uint32_t i = 0; i < nr; i++) {
		const Row &r = rows[i];
		char e[16];
		put("  %u %s (pid %d) class %u mask 0x%x %u/%u %s%s\n", r.gucId, r.kernel ? "kernel" : r.name, r.pid, r.cls, r.mask,
			r.done, r.submitted, engineIdName(r.eid, e, sizeof(e)), r.banned ? " banned" : "");
	}
	if (more) put("  ... %u more\n", more);
	return (int)n;
}

// debug.intelxe.vcs_busy (root): samples every media engine for 250 ms (about every 20 us) with the media GT and its
// engines awake (mediaWake, as ENGINE_CYCLES): RING_CURRENT_LRCA valid (bit 0: a context is on the engine) and
// RING_HEAD != RING_TAIL (the loaded context's ring has commands left), and which contexts were seen loaded (by LRCA,
// named by guc_id). Also RING_IDLEDLY / RING_PWRCTX_MAXCNT of each engine (Wa_16023105232 / Wa_14025941587:
// xe_hw_engine.c adjust_idledly wants IDLE_DELAY below the wait time and INHIBIT_SWITCH_UNTIL_PREEMPTED clear).
// A sampling estimate (the GuC's own engine busyness, xe_guc_engine_activity.c, is not set up here).
int xeVcsBusyString(char *out, size_t len) {
	if (!gDev->mediaOn()) return snprintf(out, len, "media GT not up (no video queue since boot or wake)\n");
	if (gSuspended || gDev->suspended) return snprintf(out, len, "suspended\n");
	struct Eng { uint32_t base, phys; bool vcs; uint32_t idledly, maxcnt, loaded, ring, nl, lrca[6], cnt[6], otherCtx; };
	Eng e[4] = {};
	const uint32_t ne = gNumMediaEngines < 4 ? gNumMediaEngines : 4;
	for (uint32_t i = 0; i < ne; i++)
		e[i] = {gMediaEngines[i].base, gMediaEngines[i].phys, gMediaEngines[i].drmClass == DRM_XE_ENGINE_CLASS_VIDEO_DECODE,
			0, 0, 0, 0, 0, {}, {}, 0};
	if (!gDev->mediaWake(true)) return snprintf(out, len, "media forcewake failed\n");
	for (uint32_t i = 0; i < ne; i++) {
		e[i].idledly = gDev->mmio(e[i].base + 0x23c);   // RING_IDLEDLY
		e[i].maxcnt = gDev->mmio(e[i].base + 0x54);     // RING_PWRCTX_MAXCNT
	}
	const uint64_t t0 = nowNs();
	uint32_t samples = 0;
	do {
		for (uint32_t i = 0; i < ne; i++) {
			Eng &x = e[i];
			const uint32_t lrca = gDev->mmio(x.base + 0x240);            // RING_CURRENT_LRCA
			const uint32_t head = gDev->mmio(x.base + 0x34) & 0x1ffffc;  // RING_HEAD HEAD_ADDR
			const uint32_t tail = gDev->mmio(x.base + 0x30) & 0x1ffff8;  // RING_TAIL TAIL_ADDR
			if (head != tail) x.ring++;
			if (!(lrca & 1)) continue;
			x.loaded++;
			uint32_t k = 0;
			while (k < x.nl && x.lrca[k] != (lrca & ~0xfffu)) k++;
			if (k == x.nl && x.nl < 6) x.lrca[x.nl++] = lrca & ~0xfffu;
			if (k < x.nl) x.cnt[k]++;
			else x.otherCtx++;
		}
		samples++;
		IODelay(20);
	} while (nowNs() - t0 < 250000000ull && samples < 50000);
	const uint64_t ns = nowNs() - t0;
	gDev->mediaWake(false);
	// the contexts seen, by guc_id (an LRCA no live media queue has: "?")
	uint32_t ids[4][6] = {};
	IOLockLock(gSchedLock);
	for (uint32_t g = 0; g < kMaxGucIds; g++)
		if (XeQueue *q = gQueueByGuc[1][g])
			for (uint32_t i = 0; i < ne; i++)
				for (uint32_t k = 0; k < e[i].nl; k++)
					if (e[i].lrca[k] == q->lrcGgtt + ringSize) ids[i][k] = q->gucId;
	IOLockUnlock(gSchedLock);
	size_t n = 0;
	auto put = [&](const char *fmt, ...) {
		if (n >= len) return;
		va_list ap;
		va_start(ap, fmt);
		const int k = vsnprintf(out + n, len - n, fmt, ap);
		va_end(ap);
		if (k > 0) n = n + (size_t)k < len ? n + (size_t)k : len - 1;
	};
	put("%u samples in %llu ms\n", samples, ns / 1000000);
	for (uint32_t i = 0; i < ne; i++) {
		const Eng &x = e[i];
		const uint32_t s = samples ? samples : 1;
		put("%s%u: context loaded %u.%u%%, ring not empty %u.%u%%; IDLEDLY 0x%08x MAXCNT 0x%08x", x.vcs ? "VCS" : "VECS", x.phys,
			x.loaded * 100 / s, x.loaded * 1000 / s % 10, x.ring * 100 / s, x.ring * 1000 / s % 10, x.idledly, x.maxcnt);
		for (uint32_t k = 0; k < x.nl; k++) {
			if (ids[i][k]) put("%s guc_id %u %u.%u%%", k ? "," : ";", ids[i][k], x.cnt[k] * 100 / s, x.cnt[k] * 1000 / s % 10);
			else put("%s LRCA 0x%x %u.%u%%", k ? "," : ";", x.lrca[k], x.cnt[k] * 100 / s, x.cnt[k] * 1000 / s % 10);
		}
		if (x.otherCtx) put(", others %u.%u%%", x.otherCtx * 100 / s, x.otherCtx * 1000 / s % 10);
		put("\n");
	}
	return (int)n;
}

// debug.intelxe.vram: who holds VRAM and the BAR window. Kernel: page-table pages (64 KB slabs, always in the window) and
// the display's buffers; then per connection (process id and name; a process may have several, one per VkDevice):
// window BOs the CPU has mapped (mapped: cannot move, see XeCpuMap) and the others, BOs beyond the window, system BOs,
// CPU maps alive, window bytes by BO size, and its largest window BOs with their GEM_CREATE flags
// (v NEEDS_VISIBLE_VRAM, s SCANOUT, +sys system memory allowed, m mapped, p scan-out pinned).
int xeVramString(char *out, size_t len) {
	size_t n = 0;
#define put(...)                                                                     \
	do {                                                                             \
		if (n < len) {                                                               \
			const int k_ = snprintf(out + n, len - n, __VA_ARGS__);                  \
			if (k_ > 0) n = n + (size_t)k_ < len ? n + (size_t)k_ : len - 1;         \
		}                                                                            \
	} while (0)
	uint64_t ptBytes = 0, dispBytes = 0, visFree, hiFree, visUsed, used;
	IOLockLock(gResLock);
	for (PtSlab *sl = gPtSlabs; sl; sl = sl->next) ptBytes += kVChunk;
	for (XeBo *d : gDispBos)
		if (d) dispBytes += d->size;
	visFree = gVis.freeBytes;
	hiFree = gHi.freeBytes;
	visUsed = gVisibleUsed;
	used = gVramUsed;
	IOLockUnlock(gResLock);
	const IntelXeStats &st = gStats;
	put("window %llu MB (BOs from %llu MB): used %llu MB, free %llu MB, of which %llu MB kept for page tables; beyond "
		"the window: used %llu MB, free %llu MB; system BOs %lld MB\n",
		vramVisible >> 20, vramUserStart >> 20, visUsed >> 20, visFree >> 20, kWinReserve >> 20, (used - visUsed) >> 20,
		hiFree >> 20, (long long)(gSysUsed >> 20));
	put("kernel: page tables %llu KB (%u pages), display %llu KB; window refusals %u, page-table allocation failures %u; "
		"spilled to system memory: %u window BOs (%llu MB), %u others; CPU maps made %u, gone %u\n",
		ptBytes >> 10, gPtPages, dispBytes >> 10, st.reserveHits, st.ptFails, st.winSpills, st.winSpillBytes >> 20,
		st.vramSpills, st.cpuMapsMade, st.cpuMapsGone);
	// rb7s: BOs shared between clients (GEM_OPEN, surface imports from a second process)
	put("shared BOs: moves rewriting other clients' VMs %u (their VMs %u); moves refused: a client lock busy %u, bindings "
		"in a VM no client reaches %u; maps / binds that waited for another client's move %u / %u; GEM names made %u, "
		"FLINK %u, OPEN %u (debug.intelxe.flink %d)\n",
		gShareSt.moves, gShareSt.foreignVms, gShareSt.lockBusy, gShareSt.unaccounted, gShareSt.mapWaits, gShareSt.bindWaits,
		gShareSt.names, gShareSt.flinks, gShareSt.opens, gFlink);
	// rb7t: BOs held by object fds (PRIME exports), which no client table shows
	struct FdV { uint32_t n, orphans; uint64_t bytes, orphanBytes; } fv = {};
	xeFdForEach(kFdBo, [](void *o, void *ctx) {
		auto *bo = static_cast<XeBo *>(o);
		auto *v = static_cast<FdV *>(ctx);
		v->n++;
		v->bytes += bo->size;
		if (!bo->handles) v->orphans++, v->orphanBytes += bo->size;
	}, &fv);
	put("object fds (debug.intelxe.fds %d): BO fds %u (%llu MB), of them of BOs with no handle left %u (%llu MB); PRIME "
		"exports %u, imports %u (same handle again %u, by another client: noMove %u); exported BOs alive %d\n",
		gFds, fv.n, fv.bytes >> 20, fv.orphans, fv.orphanBytes >> 20, gFdShare.primeExports, gFdShare.primeImports,
		gFdShare.primeSame, gFdShare.foreign, (int)gExportedBoLive);
	static const uint64_t kClass[] = {256ull << 10, 2ull << 20, 16ull << 20, ~0ull};
	if (!IOLockTryLock(gClientsLock)) {
		put("(client list busy)\n");
		return (int)n;
	}
	for (XeClientState *c = gClients; c; c = c->next) {
		if (!IOLockTryLock(c->lock)) {
			put("pid %d %s: busy\n", c->pid, c->name);
			continue;
		}
		uint64_t winMap = 0, winOther = 0, hi = 0, sys = 0, cls[4] = {};
		uint32_t nWinMap = 0, nWinOther = 0, nHi = 0, nSys = 0, maps = 0;
		XeBo *top[6] = {};
		for (uint32_t h = 1; h <= c->bos.n; h++) {
			auto *bo = static_cast<XeBo *>(c->bos.get(h));
			if (!bo) continue;
			if (bo->vram && !bo->hi) {
				if (bo->mapped || bo->cpuMaps) winMap += bo->size, nWinMap++;
				else winOther += bo->size, nWinOther++;
				uint32_t k = 0;
				while (bo->size > kClass[k]) k++;
				cls[k] += bo->size;
				for (uint32_t i = 0; i < 6; i++)
					if (!top[i] || bo->size > top[i]->size) {
						for (uint32_t j = 5; j > i; j--) top[j] = top[j - 1];
						top[i] = bo;
						break;
					}
			} else if (bo->vram) {
				hi += bo->size, nHi++;
			} else if (bo->sys) {
				sys += bo->size, nSys++;
			}
		}
		for (uint32_t i = 1; i <= c->maps.n; i++)
			if (auto *m = static_cast<XeCpuMap *>(c->maps.get(i)); m && m->used) maps++;
		put("pid %d %s: window %llu MB mapped (%u BOs) + %llu MB not (%u); beyond %llu MB (%u); system %llu MB (%u); "
			"CPU maps %u, of closed handles %u; window by BO size <=256K %llu MB, <=2M %llu MB, <=16M %llu MB, more %llu MB\n"
			"  largest window BOs:",
			c->pid, c->name, winMap >> 20, nWinMap, winOther >> 20, nWinOther, hi >> 20, nHi, sys >> 20, nSys, maps,
			c->orphanMaps, cls[0] >> 20, cls[1] >> 20, cls[2] >> 20, cls[3] >> 20);
		for (XeBo *bo : top)
			if (bo)
				put(" %llu KB %s%s%s%s%s%s", bo->size >> 10, bo->gemFlags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM ? "v" : "",
					bo->gemFlags & DRM_XE_GEM_CREATE_FLAG_SCANOUT ? "s" : "", bo->placement & 1 ? "+sys" : "",
					bo->mapped || bo->cpuMaps ? "m" : "", bo->pins ? "p" : "", bo->shared ? "S" : "");
		put("\n");
		IOLockUnlock(c->lock);
	}
	IOLockUnlock(gClientsLock);
	return (int)n;
}
#undef put
