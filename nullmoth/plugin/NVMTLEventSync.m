/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

void nvmtl_texture_demote(NVMTLTexture *t);   // NVMTLObjects.m (rb7f)
#include "nvmtl_plock.h"

// v2allAhead (NVMTL_LOCK_V2): every part of its command buffer has gone ahead, the pass no longer needs to look at it
@interface NVMTLQueueReservation : NSObject { @public __weak NVMTLCommandBuffer *buffer; int v2allAhead; } @end
@implementation NVMTLQueueReservation @end
@interface NVMTLSubmissionPart : NSObject {
@public
    nvk_cmdbuf commands;
    NSSet *resources, *surfaces;
    NSArray *scratch;
    id waitEvent, signalEvent;
    uint64_t value;
    unsigned encoders;
    NSArray *signals;
    nvmtl_inflight *inflight;
    int aheadFailed;
    BOOL waitFolded;
    BOOL aheadEmpty;
    BOOL claimed;
    NSArray<NVMTLBuffer *> *copyInputs;
    BOOL copyInputsPrepared, hasHostCopies;
    // the surfaces' write sequence numbers (surface-out order), and the surface-outs folded into this part's own commands
    // (nvmtl_surfout_fold): their imports stay busy until the part is done
    __unsafe_unretained NVMTLTexture *seqTex[16]; uint32_t seq[16]; unsigned nSeq;
    __unsafe_unretained NVMTLTexture *fold[8]; void *foldImp[8], *foldScan[8]; unsigned nFold;
    BOOL foldP4[8];   // gpuperf phase 4: that folded copy skipped the IOSurface's pages (published with a newer mark)
    // implicit IOSurface sync (nvmtl_vk_isync), outside WindowServer: the folded surfaces are published when the part is
    // submitted (their fences are attached, the seed moves), and the command buffer is scheduled only after that
    __unsafe_unretained NVMTLCommandBuffer *owner; BOOL isyncPublish, foldPublished;
    // B580 VRAM-shared surfaces this part writes (nvmtl_vk_anv_vram): no copy; their fences are attached and they are
    // marked newest (a CPU lock pages them off, readers see the new VRAM tag) when the part is submitted
    __unsafe_unretained NVMTLTexture *vpub[8]; unsigned nVpub; BOOL vpubDone;
    int v2state;   // NVMTL_LOCK_V2: the pass / worker hand-off (NVMTL_PART_*, nvmtl_plock.h) that `claimed` was under the class lock
}
@end
static BOOL nvmtl_async_commit_on(void);
/* NVMTL_LOCK_V2 (nvmtl_plock.h): commit and the execution worker without the process-wide class lock. On by default for
   applications, off for WindowServer and the system's interface; NVMTL_LOCK_V2=0/1 decides for one process, from its
   own environment only (never the conf file: WindowServer keeps the class lock unless its own environment asks),
   and only with the asynchronous worker (NVMTL_ASYNC_COMMIT=0 keeps the synchronous pump, under the class lock). */
int nvmtl_lock_v2(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_LOCK_V2");
        int v = nvmtl_plock_rule(e, getenv("NVMTL_CONF_SET"));
        if (v < 0) v = !nvmtl_plock_system_ui(getprogname());   // the default: applications only
        if (v && !nvmtl_async_commit_on()) v = 0;
        on = v;
        nvlog("commit: locking %s (NVMTL_LOCK_V2=%s, build default %d)", on ? "V2 - state lock, submit-ahead combiner, per-part hand-off; no class lock"
              : "the command-buffer class lock", e ? e : "unset", NVMTL_LOCK_V2_DEFAULT);
    }
    return on;
}
/* SCHEDCENSUS (gpuperf, root- or user-owned /tmp/nvmtl-schedcensus): why a commit is scheduled late. Each refusal of the
   submit-ahead pass for the first part it cannot submit is counted by reason; waitUntilScheduled waits are timed and
   split by who scheduled the command buffer (the commit-time pass or the execution worker after it advanced to it); the
   worker's time is split into waiting for part fences (gsub_finish) and surface publication. Logged every 3 s. */
enum { SC_CAP, SC_CLAIMED, SC_EVENT, SC_NOCMDS, SC_HOSTCOPY, SC_MARKING, SC_NOTCOMMITTED, SC_INFLIGHTNEW, SC_SUBMITFAIL, SC_N };
static const char *const sc_name[SC_N] = { "in-flight cap", "claimed by worker", "event wait not yet promised", "no commands", "host copies held", "commit marking", "not committed/done", "inflight alloc", "submit failed" };
static _Atomic uint64_t sc_refuse[SC_N], sc_waits, sc_wait_ns, sc_wait_max, sc_by_worker, sc_by_commit, sc_w_fence_ns, sc_w_pub_ns, sc_w_parts;
static __thread int sc_on_worker;
static int nvmtl_sc_on(void) {
    // V2: the flag file's answer is kept for its 250 ms here, without nvmtl_diag_flag's lock and name search on every
    // scheduled command buffer
    if (nvmtl_lock_v2()) {
        static _Atomic uint64_t next; static _Atomic int on;
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now >= atomic_load_explicit(&next, memory_order_relaxed)) {
            atomic_store_explicit(&next, now + 250000000ull, memory_order_relaxed);
            atomic_store_explicit(&on, nvmtl_diag_flag("schedcensus"), memory_order_relaxed);
        }
        return atomic_load_explicit(&on, memory_order_relaxed);
    }
    return nvmtl_diag_flag("schedcensus");
}
static void nvmtl_sc_refuse(int why) { if (nvmtl_sc_on()) atomic_fetch_add_explicit(&sc_refuse[why], 1, memory_order_relaxed); }
static void nvmtl_sc_report(void) {
    static _Atomic uint64_t last; const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), l = atomic_load(&last);
    if (!l) { atomic_store(&last, now); return; }
    if (now - l < 3000000000ull || !atomic_compare_exchange_strong(&last, &(uint64_t){ l }, now)) return;
    char b[900]; int k = 0;
    for (int i = 0; i < SC_N; i++) { const uint64_t n = atomic_exchange(&sc_refuse[i], 0); if (n) k += snprintf(b + k, sizeof b - (size_t)k, " %s %llu,", sc_name[i], (unsigned long long)n); }
    const uint64_t w = atomic_exchange(&sc_waits, 0), wn = atomic_exchange(&sc_wait_ns, 0), wm = atomic_exchange(&sc_wait_max, 0);
    const uint64_t bw = atomic_exchange(&sc_by_worker, 0), bc = atomic_exchange(&sc_by_commit, 0);
    const uint64_t fn = atomic_exchange(&sc_w_fence_ns, 0), pn = atomic_exchange(&sc_w_pub_ns, 0), wp = atomic_exchange(&sc_w_parts, 0);
    const double sec = (now - l) / 1e9;
    nvlog_keep("SCHEDCENSUS %.1f s: waitUntilScheduled %llu waits, %.1f ms/s waiting (avg %.3f ms, max %.2f ms); scheduled at commit %llu, by the worker %llu; "
               "worker: %llu parts, %.1f ms/s waiting part fences, %.1f ms/s publishing surfaces; submit-ahead refusals:%s",
               sec, (unsigned long long)w, wn / 1e6 / sec, w ? wn / 1e6 / (double)w : 0.0, wm / 1e6, (unsigned long long)bc, (unsigned long long)bw,
               (unsigned long long)wp, fn / 1e6 / sec, pn / 1e6 / sec, k ? b : " none");
}
void nvmtl_sc_wait_done(uint64_t ns) {
    if (!nvmtl_sc_on()) return;
    atomic_fetch_add(&sc_waits, 1); atomic_fetch_add(&sc_wait_ns, ns);
    uint64_t m = atomic_load(&sc_wait_max); while (ns > m && !atomic_compare_exchange_weak(&sc_wait_max, &m, ns)) {}
    nvmtl_sc_report();
}
static _Atomic unsigned gsub_inflight;
static _Atomic unsigned long long gsub_ahead, gsub_peak;
static NSMutableArray *nvmtl_quarantined_parts;
/* A part the GPU did not finish (nvmtl_vk_submit_finish / nvmtl_vk_lost): the error Apple's drivers give after a GPU
   restart (IOAccel command buffer callback errors), so clients (CoreAnimation, WindowServer) see what they know. */
static NSError *nvmtl_part_error(int result, NSString *other) {
    NSInteger code = MTLCommandBufferErrorInternal; NSString *what = other;
    switch (result) {
    case -3: code = 3;   /* kIOAccelCommandBufferCallbackErrorHang (MTLCommandBufferErrorPageFault's value) */
        what = @"Caused GPU Hang Error (00000003:kIOAccelCommandBufferCallbackErrorHang)"; break;
    case -4: code = MTLCommandBufferErrorInternal;
        what = @"Discarded (victim of GPU error/recovery) (00000005:kIOAccelCommandBufferCallbackErrorInnocentVictim)"; break;
    case -5: code = MTLCommandBufferErrorTimeout;
        what = @"Caused GPU Timeout Error (00000002:kIOAccelCommandBufferCallbackErrorTimeout)"; break;
    default: break;
    }
    return [NSError errorWithDomain:MTLCommandBufferErrorDomain code:code userInfo:@{ NSLocalizedDescriptionKey: what }];
}
static int nvmtl_gsub_finish(NVMTLSubmissionPart *part) {
    nvmtl_inflight *const f = part->inflight;
    part->inflight = NULL;
    const int r = nvmtl_vk_submit_finish(f);
    atomic_fetch_sub(&gsub_inflight, 1);
    if (r == 0) nvmtl_vk_inflight_free(f);
    return r;
}
@interface NVMTLPendingSurfaceWrite : NSObject {
@public __weak NVMTLCommandBuffer *buffer; __weak NVMTLTexture *texture; uint32_t seed;
}
@end
@implementation NVMTLPendingSurfaceWrite @end
// Queue keys and command-buffer references are weak. An abandoned recording is
// never registered; completion removes committed entries without retaining VRAM.
static NSMapTable *nvmtl_surface_writers;
// surface id and plane in one tagged NSNumber: no allocation on the per-bind path (it was a formatted string)
static id nvmtl_surface_writer_key(NVMTLTexture *t) {
    return @(((unsigned long long)IOSurfaceGetID(t->_surf) << 8) | ((unsigned long long)t->_plane & 0xff));
}
// NVMTL_LOCK_V2: the map has a lock of its own (recursive, as the class lock was: a writer released by the lookup may
// take it again from its dealloc), so the worker's per-command-buffer finish no longer queues behind a commit's submit
static pthread_mutex_t nvmtl_surface_writers_lock = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
static BOOL nvmtl_surface_pending_write_locked(NVMTLCommandBuffer *cb, NVMTLTexture *t);
static BOOL nvmtl_surface_pending_write(NVMTLCommandBuffer *cb, NVMTLTexture *t) {
    if (!cb || !t || !t->_surf) return NO;
    if (nvmtl_lock_v2()) {
        pthread_mutex_lock(&nvmtl_surface_writers_lock);
        const BOOL r = nvmtl_surface_pending_write_locked(cb, t);
        pthread_mutex_unlock(&nvmtl_surface_writers_lock);
        return r;
    }
    @synchronized ([NVMTLCommandBuffer class]) { return nvmtl_surface_pending_write_locked(cb, t); }
}
static BOOL nvmtl_surface_pending_write_locked(NVMTLCommandBuffer *cb, NVMTLTexture *t) {
    {
        id key = nvmtl_surface_writer_key(t);
        for (id queue in nvmtl_surface_writers.keyEnumerator) {
            NSDictionary *writes = [nvmtl_surface_writers objectForKey:queue];
            NVMTLPendingSurfaceWrite *write = writes[key];
            NVMTLCommandBuffer *writer = write ? write->buffer : nil;
            NVMTLTexture *backing = write ? write->texture : nil;
            const BOOL sameBacking = backing && (backing == t || (backing->_vramOn && t->_vramOn));
            if (writer && writer != cb && sameBacking && write->seed == IOSurfaceGetSeed(t->_surf)) return YES;
        }
        return NO;
    }
}
static void nvmtl_surface_register_writes_locked(NVMTLCommandBuffer *cb);
static void nvmtl_surface_register_writes(NVMTLCommandBuffer *cb) {
    if (nvmtl_lock_v2()) {
        BOOL any = NO;   // most command buffers write no IOSurface: no lock for them
        for (NVMTLSubmissionPart *part in cb->_parts)
            if ([part isKindOfClass:[NVMTLSubmissionPart class]] && part->surfaces.count) { any = YES; break; }
        if (!any) return;
        pthread_mutex_lock(&nvmtl_surface_writers_lock);
        nvmtl_surface_register_writes_locked(cb);
        pthread_mutex_unlock(&nvmtl_surface_writers_lock);
        return;
    }
    // The same lock as pending_write and finish_writes, taken here too: the map is shared with the completion thread, and
    // its one caller holding this (recursive) lock today must not be what keeps WindowServer's heap intact.
    @synchronized ([NVMTLCommandBuffer class]) { nvmtl_surface_register_writes_locked(cb); }
}
static void nvmtl_surface_register_writes_locked(NVMTLCommandBuffer *cb) {
    {
        if (!nvmtl_surface_writers) nvmtl_surface_writers = [NSMapTable weakToStrongObjectsMapTable];
        NSMutableDictionary *writes = [nvmtl_surface_writers objectForKey:cb->_queue];
        for (NVMTLSubmissionPart *part in cb->_parts) for (NVMTLTexture *t in part->surfaces) {
            if (!t->_surf) continue;
            if (!writes) { writes = [NSMutableDictionary new]; [nvmtl_surface_writers setObject:writes forKey:cb->_queue]; }
            NVMTLPendingSurfaceWrite *write = [NVMTLPendingSurfaceWrite new];
            write->buffer = cb; write->texture = t; write->seed = IOSurfaceGetSeed(t->_surf);
            writes[nvmtl_surface_writer_key(t)] = write;
        }
    }
}
// WindowServer frame completion, with NVAccel's nvSurfGpuEscape: each surface a WindowServer command buffer renders is
// marked pending when it commits and ready when the GPU has finished it, and the swap waits for that in the kernel, off
// WindowServer's thread (NVIDIA completes flips on the display's flip event, never in the compositor). The marks go
// through NVK's surface escape because WindowServer's sandbox denies sysctl writes. A mark the kernel does not take (an
// older NVAccel, debug.nvaccel_surface_gpu_on=0) keeps WindowServer's own wait; that answer is re-read every second.
static _Atomic int nvmtl_ws_marks_on = -1;
static _Atomic uint64_t nvmtl_ws_marks_checked;
static BOOL nvmtl_ws_kernel_waits(void) {
    static int ws = -1;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer") && !getenv("NVMTL_WS_WAIT_COMPLETION");
    if (!ws) return NO;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (atomic_load(&nvmtl_ws_marks_on) < 0 || now - atomic_load(&nvmtl_ws_marks_checked) > 1000000000ull) {
        const int on = nvmtl_vk_surface_gpu_mark(0, 0) == 0;   // surface 0: a probe the kernel answers and ignores
        if (on != atomic_exchange(&nvmtl_ws_marks_on, on))
            nvlog("WindowServer frame completion: %s", on ? "the swap waits in NVAccel (no composite wait here)" : "waiting for each composite here");
        atomic_store(&nvmtl_ws_marks_checked, now);
    }
    return atomic_load(&nvmtl_ws_marks_on) == 1;
}
static const void *const nvmtl_ws_marks_key = &nvmtl_ws_marks_key;
static void nvmtl_ws_mark(uint32_t sid, BOOL pending) { (void)nvmtl_vk_surface_gpu_mark(sid, pending); }
// Marking WindowServer's surfaces newest in VRAM at commit is only safe when NVAccel's page-off waits for a pending
// surface (NVAccelSurfaceWait). Without that wait a CPU lock or a copied present pages the surface off while the GPU
// is still drawing it: blank window panes, a torn band, a Dock without its icons. Such an accelerator (a package that
// pairs this plugin with an older one) keeps the ordering it was built for: the surface is marked newest after the GPU
// has finished, and the pending marks are released when the whole command buffer has. Read once per process.
static BOOL nvmtl_ws_surface_wait(void) {
    static int on = -1;
    if (on < 0) {
        on = (nvmtl_vk_accel_surface_wait() || nvmtl_vk_accel_surface_wait_all()) ? 1 : 0;
        nvlog("WindowServer surfaces: %s", on ? "marks released as each part lands, before publication (the accelerator's page-off waits for them)"
                                              : "marked newest after the GPU finishes (the accelerator's page-off does not wait)");
    }
    return on == 1;
}
// What one WindowServer command buffer holds: the surfaces it marked pending in the kernel, and the textures whose shared
// VRAM its commit marked newest. early is fixed at commit (nvmtl_ws_surface_wait): the marks are released part by part.
@interface NVMTLSurfaceHolds : NSObject { @public NSMutableSet *ids; NSSet *dirtied; BOOL early; } @end
@implementation NVMTLSurfaceHolds @end
// A texture whose part has landed while its surface is still held, published by the holder that releases it last.
// dirtied: the commit marked its VRAM newest, so publication is bookkeeping only (nvmtlSurfaceOutDirtied:).
@interface NVMTLDeferredPublication : NSObject { @public NVMTLTexture *texture; BOOL dirtied, folded, p4; __weak NVMTLCommandBuffer *owner; } @end
@implementation NVMTLDeferredPublication @end
// The kernel keeps one pending bit per surface, with no owner. WindowServer commits overlapping command buffers that
// write the same surface, so the holders are counted here and the kernel mark is released only when the last of them has
// finished with it; an older command buffer finishing first would otherwise release a surface the newer one still writes.
// With the early ordering nothing is published while its surface is still held. A dirtied texture's bookkeeping would
// call the surface current with another write in flight; a full publication (a private copy, or a texture whose commit
// mark was refused) locks the surface, and that lock's page-off would wait for a mark only this completion thread can
// release. Those textures wait in nvmtl_ws_deferred for the holder that releases the surface last.
// The lock is its own, never the command-buffer class lock, and nothing that can wait for the accelerator runs under it:
// a page-off waiting for a mark holds the accelerator lock, and the release it waits for must always get through.
static NSCountedSet *nvmtl_ws_pending_holds;
static NSMutableDictionary<NSNumber *, NSMutableArray<NVMTLDeferredPublication *> *> *nvmtl_ws_deferred;
static NSObject *nvmtl_ws_lock(void) {
    static NSObject *lock; static dispatch_once_t once;
    dispatch_once(&once, ^{ lock = [NSObject new]; });
    return lock;
}
static void nvmtl_ws_defer(NVMTLCommandBuffer *owner, NVMTLTexture *t, BOOL dirtied, BOOL folded, BOOL p4, uint32_t sid) {
    @synchronized (nvmtl_ws_lock()) {
        if (!nvmtl_ws_deferred) nvmtl_ws_deferred = [NSMutableDictionary new];
        NSMutableArray *waiting = nvmtl_ws_deferred[@(sid)];
        if (!waiting) nvmtl_ws_deferred[@(sid)] = waiting = [NSMutableArray new];
        // a texture written again keeps only its latest place in the order: published at its earlier place, a private
        // copy written between the two would be copied out after it and leave the older pixels current
        for (NSUInteger i = 0; i < waiting.count; i++) if (((NVMTLDeferredPublication *)waiting[i])->texture == t) { [waiting removeObjectAtIndex:i]; break; }
        NVMTLDeferredPublication *d = [NVMTLDeferredPublication new];
        d->texture = t; d->dirtied = dirtied; d->folded = folded; d->p4 = p4; d->owner = owner;
        [waiting addObject:d];
    }
}
// One holder lets go of a surface. The last one releases the kernel mark and moves the textures waiting for it to settled.
static void nvmtl_ws_release(NVMTLSurfaceHolds *holds, uint32_t sid, NSMutableArray *settled) {
    NSNumber *n = @(sid);
    @synchronized (nvmtl_ws_lock()) {
        if (![holds->ids containsObject:n]) return;
        [holds->ids removeObject:n];
        [nvmtl_ws_pending_holds removeObject:n];
        if ([nvmtl_ws_pending_holds countForObject:n]) return;
        nvmtl_ws_mark(sid, NO);
        NSArray *waiting = nvmtl_ws_deferred[n];
        if (waiting) { [settled addObjectsFromArray:waiting]; [nvmtl_ws_deferred removeObjectForKey:n]; }
    }
}
// Publishes what was waiting, after the marks are released. Returns NO when one of cb's own textures could not be
// published (its command buffer fails, as before); another command buffer's has already completed, so that is logged.
static BOOL nvmtl_ws_publish_settled(NSArray *settled, NVMTLCommandBuffer *cb) {
    BOOL ok = YES;
    for (NVMTLDeferredPublication *d in settled) {
        if (d->folded) { [d->texture nvmtlSurfaceOutFoldedP4:d->p4]; continue; }   // its pixels came with the writing part
        if (d->dirtied ? [d->texture nvmtlSurfaceOutDirtied:YES] : [d->texture nvmtlSurfaceOut]) continue;
        if (cb && d->owner == cb) ok = NO;
        else nvlog("WindowServer surface %u: publication after its last write refused", (unsigned)IOSurfaceGetID(d->texture->_surf));
    }
    return ok;
}
// Commit, OUTSIDE the command-buffer class lock and before the pending-write records are stamped: the surfaces this
// command buffer writes are marked pending in the kernel before commit returns, so WindowServer's swap that follows finds
// the marks; with the early ordering their shared VRAM is first marked newest.
// A normal IOSurfaceLock waits for GPU work still in flight on the surface (only kIOSurfaceLockAvoidSync skips that),
// and WindowServer relies on it: a window capture locks the surface it is still drawing (SkyLight's
// do_finish_and_clear_capture_state). The VRAM copy was marked newest only after the GPU finished, so a lock that came
// first found the system copy current and read zeros (black screenshots, blank Dock thumbnails, black lock screen).
// Marked here, the lock pages the surface off, and the page-off waits for the pending mark to clear.
// The VRAM mark takes the accelerator lock, which a page-off waiting for a pending surface holds until the completion
// thread releases that surface. So it runs outside the class lock (the completion thread takes that one between command
// buffers), and before this command buffer's own pending marks: with those in first, a page-off of a surface this
// command buffer writes would wait for it, and it cannot complete while its commit waits here. Either way only the
// kernel's 100 ms bound would end the wait, and the page-off would then copy unfinished pixels.
// The VRAM mark moves the IOSurface seed, so the pending-write records are stamped after it
// (nvmtl_surface_register_writes): a record stamped before the move matched nothing, every frame's read of the surface
// still in flight took the CPU path (page-off, wait, 8 MB upload) on the compositor's thread, and a 144 Hz desktop ran
// at 90 frames a second.
static void nvmtl_ws_mark_pending(NVMTLCommandBuffer *cb) {
    if (!nvmtl_ws_kernel_waits()) return;
    NSMutableSet *ids = nil;
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        for (NVMTLTexture *t in part->surfaces) if (t->_surf) {
            if (!ids) ids = [NSMutableSet new];
            [ids addObject:@(IOSurfaceGetID(t->_surf))];
        }
    }
    if (!ids) return;
    NVMTLSurfaceHolds *holds = [NVMTLSurfaceHolds new];
    holds->ids = ids; holds->early = nvmtl_ws_surface_wait();
    if (holds->early) {
        NSMutableSet *dirtied = [NSMutableSet new];
        for (NVMTLSubmissionPart *part in cb->_parts) {
            if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
            for (NVMTLTexture *t in part->surfaces)
                if (t->_surf && (t->_vramOn || t->_svDemoted)) {
                    const int m = nvmtl_vk_surface_dirty((uint32_t)IOSurfaceGetID(t->_surf), (uint32_t)t->_plane);
                    if (!m) [dirtied addObject:t];
                    else if (m == -2) nvmtl_texture_demote(t);   // rb7f: CPU-read surface, copied out at surface-out now
                }
        }
        holds->dirtied = dirtied;
    }
    @synchronized (nvmtl_ws_lock()) {
        if (!nvmtl_ws_pending_holds) nvmtl_ws_pending_holds = [NSCountedSet new];
        for (NSNumber *n in ids) { [nvmtl_ws_pending_holds addObject:n]; nvmtl_ws_mark(n.unsignedIntValue, YES); }
    }
    objc_setAssociatedObject(cb, nvmtl_ws_marks_key, holds, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
// WindowServer on NMXAccel: SkyLight copies a capture (screenshots, window and Dock snapshots, the minimize animation's
// window image, screen recording) out with the CPU straight after the compositor's flush (do_finish_and_clear_capture_state:
// commitAndWaitUntilSubmitted, then IOSurfaceLock read-only and memcpy) and relies on IOSurfaceLock to wait for the GPU, as
// Apple's drivers make it. Here nothing in the lock can: the accelerator's page-off runs only for a surface that has an
// accelerator resource, and this plugin's surfaces have none (their pages are imported into the B580's own VM), so the
// pending marks never reach a CPU lock (fix18 relied on them and Dock previews of minimized windows got their noise band
// back). So commitAndWaitUntilSubmitted waits for the GPU whenever the command buffer writes a surface the display pipe
// does not present: the kernel knows that exactly (nmxScanOp op 0, kNmxScNotPresented), and a presented composite is
// never read back by the CPU, so the compositor's own frames keep returning at submission. A surface the kernel cannot
// answer for (flips off) waits too. WAS a guess per texture (its first 16 flushes, then 64 when unknown).
// NVMTL_CAPTURE_WAIT=0 turns it off, =1 applies it in any process (tests).
static int nvmtl_capture_wait_mode(void) {
    static int mode = -1;
    if (mode < 0) {
        const char *e = getenv("NVMTL_CAPTURE_WAIT");
        const BOOL ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        mode = (e && e[0] == '0') ? 0 : (e && e[0] == '1') ? 2 : (ws && nvmtl_vk_vendor() != 0x10de) ? 1 : 0;
        if (ws || mode) nvlog("captures: %s (NVMTL_CAPTURE_WAIT=%s)", mode ? "a flush that writes a surface the display does not present waits for its pixels" : "not waited for",
                              e ? e : "unset");
    }
    return mode;
}
// Whether the display pipe presents t's surface. A yes is kept for a second (one kernel call per surface per second for
// the compositor's frames); a no is asked again at the next flush, so a new composite stops waiting once it is shown.
static BOOL nvmtl_ws_presented(NVMTLTexture *t) {
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return NO;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (r->_wsShownAt && now - r->_wsShownAt < NSEC_PER_SEC) return YES;
    const int st = nvmtl_vk_scanout_op(0, (uint32_t)IOSurfaceGetID(r->_surf), 0, 0, 0, 0, 0, NULL);
    const BOOL shown = st == 0 || st == 3;   // presented (3: at another geometry than the mode's)
    r->_wsShownAt = shown ? now : 0;
    if (st != 0 && st != 2 && st != 3) { static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 4)
        nvlog("captures: the display pipe cannot say whether surface %u is presented (%d) - its flushes wait", (unsigned)IOSurfaceGetID(r->_surf), st); }
    return shown;
}
// At commit, before cb is pending (its parts are stable): does it write a capture? commitAndWaitUntilSubmitted reads it.
// Not a surface the GPU itself streams: in a scaled (HiDPI) mode WindowServer composites into a
// surface of the mode's size, which the display pipe does not present, and a second pass samples it to scale it to the
// panel. Waiting for that composite at every flush held the compositor to every other refresh (90 of 180 Hz in every
// scaled mode; 180 with the wait off). The composite is written and sampled at display rate; a capture is written once
// per capture and read by the CPU. So a surface written and sampled at least 20 times each within 500 ms is an
// intermediate and its flushes do not wait; any other keeps waiting. (Two looser rules were measured first: "sampled
// after every write" missed composites written by several command buffers per frame; "sampled 8 times within 250 ms"
// also let pooled screenshot surfaces through after their preview animation sampled them, and screenshots tore.)
// A screenshot of a scaled mode reads the composite itself with the CPU: that is safe only once the surface has an
// accelerator resource, whose page-off makes the CPU lock wait for the GPU (nvmtl_ws_kernel_resource).
static BOOL nvmtl_ws_gpu_intermediate(NVMTLTexture *t) {
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    return r->_surf && nvmtl_ws_written_streaming((uint32_t)IOSurfaceGetID(r->_surf), 20) && nvmtl_ws_kernel_resource(r);
}
static void nvmtl_ws_note_captures(NVMTLCommandBuffer *cb) {
    cb->_wsCaptureWait = NO;
    const int mode = nvmtl_capture_wait_mode();
    if (!mode || (mode == 1 && !nvmtl_ws_kernel_waits())) return;   // without the kernel's wait every flush waits already
    for (NVMTLSubmissionPart *part in cb->_parts) {
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) continue;
        if (part->waitEvent) return;   // waiting here for an event a later commit signals would deadlock
        for (NVMTLTexture *t in part->surfaces) if (t->_surf && !t->_vramOn && !nvmtl_ws_presented(t)) {
            if (nvmtl_ws_gpu_intermediate(t)) {
                static _Atomic unsigned told; if (atomic_fetch_add(&told, 1) < 4)
                    nvlog("captures: surface %u %zux%zu is written and sampled by the GPU at display rate - an intermediate, its flush does not wait",
                          IOSurfaceGetID(t->_surf), IOSurfaceGetWidth(t->_surf), IOSurfaceGetHeight(t->_surf));
                continue;
            }
            cb->_wsCaptureWait = YES;
            static _Atomic unsigned said; const unsigned n = atomic_fetch_add(&said, 1);
            if (n < 6 || (n & 1023) == 0) nvlog("captures: surface %u %zux%zu is not on the display - its flush waits for the pixels (%u so far)",
                                                 IOSurfaceGetID(t->_surf), IOSurfaceGetWidth(t->_surf), IOSurfaceGetHeight(t->_surf), n + 1);
            return;
        }
    }
}
// The GPU has finished part index of cb. Without the early ordering, returns every texture of the part for nvmtlSurfaceOut
// and keeps the marks until the end of the command buffer (nvmtl_ws_mark_ready), as before. With it, returns nothing:
// this command buffer's mark on a surface is released after the last of its parts that writes the surface, and settled
// receives what the releases freed, to be published after them (a CPU lock waiting in the page-off for a mark holds the
// surface's lock, which publication takes). Released earlier, the swap or a capture would read the surface with a later
// part's write still in flight.
static BOOL nvmtl_part_folded(NVMTLSubmissionPart *part, NVMTLTexture *t);
static BOOL nvmtl_part_folded_p4(NVMTLSubmissionPart *part, NVMTLTexture *t);
static NSArray *nvmtl_ws_part_landed(NVMTLCommandBuffer *cb, NSUInteger index, NSMutableArray *settled) {
    NVMTLSubmissionPart *part = cb->_parts[index];
    NVMTLSurfaceHolds *holds = objc_getAssociatedObject(cb, nvmtl_ws_marks_key);
    NSMutableArray *publish = [NSMutableArray new];
    if (!holds || !holds->early) { for (NVMTLTexture *t in part->surfaces) [publish addObject:t]; return publish; }
    NSMutableArray *last = [NSMutableArray new];
    for (NVMTLTexture *t in part->surfaces) {
        if (!t->_surf) { [publish addObject:t]; continue; }
        const uint32_t sid = (uint32_t)IOSurfaceGetID(t->_surf);
        // deferred before the release below, so the release that turns out to be the last one publishes it
        nvmtl_ws_defer(cb, t, [holds->dirtied containsObject:t], nvmtl_part_folded(part, t), nvmtl_part_folded_p4(part, t), sid);
        BOOL later = NO;
        for (NSUInteger i = index + 1; i < cb->_parts.count && !later; i++) {
            NVMTLSubmissionPart *next = cb->_parts[i];
            if (![next isKindOfClass:[NVMTLSubmissionPart class]]) continue;
            for (NVMTLTexture *other in next->surfaces) if (other->_surf && (uint32_t)IOSurfaceGetID(other->_surf) == sid) { later = YES; break; }
        }
        if (!later) [last addObject:@(sid)];
    }
    for (NSNumber *n in last) nvmtl_ws_release(holds, n.unsignedIntValue, settled);
    return publish;
}
// The command buffer has finished (or failed): every hold it still has is released, and what was waiting is published.
static void nvmtl_ws_mark_ready(NVMTLCommandBuffer *cb) {
    NVMTLSurfaceHolds *holds = objc_getAssociatedObject(cb, nvmtl_ws_marks_key);
    if (!holds) return;
    NSArray *ids; @synchronized (nvmtl_ws_lock()) { ids = holds->ids.allObjects; }
    NSMutableArray *settled = [NSMutableArray new];
    for (NSNumber *n in ids) nvmtl_ws_release(holds, n.unsignedIntValue, settled);
    objc_setAssociatedObject(cb, nvmtl_ws_marks_key, nil, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    (void)nvmtl_ws_publish_settled(settled, nil);
}
static void nvmtl_surface_finish_writes_locked(NVMTLCommandBuffer *cb) {
    NSMutableDictionary *writes = [nvmtl_surface_writers objectForKey:cb->_queue];
    for (id key in [writes.allKeys copy]) {
        NVMTLPendingSurfaceWrite *write = writes[key];
        if (!write->buffer || write->buffer == cb) [writes removeObjectForKey:key];
    }
    if (!writes.count) [nvmtl_surface_writers removeObjectForKey:cb->_queue];
}
static void nvmtl_surface_finish_writes(NVMTLCommandBuffer *cb) {
    nvmtl_ws_mark_ready(cb);
    if (nvmtl_lock_v2()) {
        pthread_mutex_lock(&nvmtl_surface_writers_lock);
        if (nvmtl_surface_writers.count) nvmtl_surface_finish_writes_locked(cb);
        pthread_mutex_unlock(&nvmtl_surface_writers_lock);
        return;
    }
    @synchronized ([NVMTLCommandBuffer class]) { nvmtl_surface_finish_writes_locked(cb); }
}

// NVMTL_SURFOUT_FOLD=0: an app's IOSurface write is copied out after its part finishes, in a submission of its own that
// the completion thread waits for (as before). Default (Intel, outside WindowServer): the copy into the IOSurface's
// imported pages is recorded at the end of the part that wrote it. The separate copy queued behind every part submitted
// ahead (6-12 ms under Minecraft's load, on the thread that notices completions), and what it copied was the image as
// those later parts left it: a newer frame's partial write when they drew into the same surface.
static bool nvmtl_surfout_fold(void) {
    static int on = -1;
    // WindowServer folds too once the accelerator's page-off waits for every surface (NMXSurfaceWaitAll): its marks are then
    // released per part, before publication, and a folded surface's pixels are in its pages by then
    if (on < 0) { const char *e = getenv("NVMTL_SURFOUT_FOLD"); const bool ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        on = (!ws || nvmtl_vk_accel_surface_wait_all()) && nvmtl_surfout_direct() && !(e && e[0] == '0');
        nvlog("surface-out: %s (NVMTL_SURFOUT_FOLD=%s)", on ? "folded into the writing part's own commands" : "a submission of its own after the part", e ? e : "unset"); }
    return on;
}
@interface NVMTLCommandBuffer (EventSubmissionScheduled)
- (void)nvmtlMarkScheduled;
@end
static BOOL nvmtl_is_ws(void) {
    static int ws = -1;
    if (ws < 0) ws = getprogname() && !strcmp(getprogname(), "WindowServer");
    return ws == 1;
}
// A multi-plane IOSurface has one seed. Private planes copied by one part are
// published together; shared VRAM and scan-out retain their existing publication.
static void nvmtl_publish_folded_part(NVMTLSubmissionPart *part) {
    unsigned used = 0;
    for (unsigned i = 0; i < part->nFold; i++) {
        if (used & (1u << i)) continue;
        used |= 1u << i;
        NVMTLTexture *r = part->fold[i];
        NSMutableArray<NVMTLTexture *> *siblings = nil;
        if (r->_surf && !r->_vramOn && !part->foldP4[i] && IOSurfaceGetPlaneCount(r->_surf) > 1) {
            for (unsigned j = i + 1; j < part->nFold; j++) {
                NVMTLTexture *s = part->fold[j];
                if (!(used & (1u << j)) && s->_surf == r->_surf && !s->_vramOn && !part->foldP4[j]) {
                    if (!siblings) siblings = [NSMutableArray new];
                    [siblings addObject:s]; used |= 1u << j;
                }
            }
        }
        [r nvmtlSurfaceOutFoldedWithSiblings:siblings p4:part->foldP4[i]];
    }
}
// Implicit sync, right after the part's submit attached its fence (nvmtl_vk_submit_begin_at, submitting thread): its
// folded surfaces are published now (the seed moves; a reader's upload waits for the fence on the GPU), then the command
// buffer is scheduled, so whoever hands the surface on after waitUntilScheduled hands on a surface that says it changed.
static void nvmtl_part_published_at_submit(void *ctx) {
    NVMTLSubmissionPart *part = (__bridge NVMTLSubmissionPart *)ctx;
    nvmtl_publish_folded_part(part);
    part->foldPublished = YES;
    // rb7g: a demoted surface's folded copy is marked too (the kernel answers "demoted" and sets the device-newer bit), so
    // a CPU lock reaches NMXAccel, which waits for this part's fence (attached already) instead of reading early
    for (unsigned i = 0; i < part->nFold; i++) {
        NVMTLTexture *t = part->fold[i]; NVMTLTexture *r = t->_parent ? t->_parent : t;
        if (r->_surf && r->_svDemoted)
            (void)(nvmtl_is_ws() ? nvmtl_vk_surface_dirty_try((uint32_t)IOSurfaceGetID(r->_surf), (uint32_t)r->_plane)
                                 : nvmtl_vk_surface_dirty((uint32_t)IOSurfaceGetID(r->_surf), (uint32_t)r->_plane));
    }
    // _vramOn: mark + the VRAM tag, no copy. WindowServer tries (its submitting thread never waits for the accelerator lock
    // a CPU page-off may hold while waiting for a mark only this thread releases; a busy lock is retried off this thread).
    // rb7g: other processes wait for the lock: a page-off holding it waits only for GPU fences, and a mark dropped here
    // let a CPU lock right after waitUntilScheduled read the pages before the page-off (cpureader: ~1 frame in 240).
    for (unsigned i = 0; i < part->nVpub; i++) (void)[part->vpub[i] nvmtlSurfaceOutDirtied:nvmtl_is_ws() ? YES : NO];
    part->vpubDone = YES;
    [part->owner nvmtlMarkScheduled];
}
// which surface writes were not folded, and why (they are copied out at completion: no fence for their readers)
static void nvmtl_fold_missed(NVMTLTexture *r, const char *why) {
    static _Atomic unsigned n; const unsigned k = atomic_fetch_add(&n, 1);
    if (k < 8 || (k & 4095) == 0) nvlog("surface-out: surface %u %ux%u NOT folded (%s) - copied out after its part (%u so far)",
                                        r->_surf ? (unsigned)IOSurfaceGetID(r->_surf) : 0u, r->_i.w, r->_i.h, why, k + 1);
}
static void nvmtl_fold_surface_out(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    // B580 VRAM-shared: the part's job writes the one copy every process uses; it waits for the surface's readers and
    // writers on the GPU and becomes its newest writer, and it is published at submission (nvmtl_part_published_at_submit)
    if (r->_surf && r->_vramOn && nvmtl_vk_anv_vram()) {
        for (unsigned i = 0; i < part->nVpub; i++) if (part->vpub[i] == r) return;
        if (part->nVpub < 8 && part->commands.cb && !nvmtl_vk_cmd_surface_use(&part->commands, (uint32_t)IOSurfaceGetID(r->_surf), 1)) {
            part->vpub[part->nVpub++] = r; part->isyncPublish = YES;
        }
        return;   // not published here: the completion path publishes it as before
    }
    if (!r->_surf || r->_vramOn || !nvmtl_surfout_fold()) return;
    if (part->nFold >= 8) { nvmtl_fold_missed(r, "8 folded already"); return; }
    if (!part->commands.cb || part->commands.in_rp) { nvmtl_fold_missed(r, part->commands.cb ? "inside a render pass" : "no commands"); return; }
    if (![r nvmtlSurfaceCopyable]) { nvmtl_fold_missed(r, "format not copyable"); return; }
    void *base = IOSurfaceGetBaseAddressOfPlane(r->_surf, (uint32_t)r->_plane);
    const size_t row = IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane);
    if (!base || !row) { nvmtl_fold_missed(r, "no base address"); return; }
    nvmtl_surfimp *imp = nvmtl_surfimp_get(r->_surf, true);
    if (!imp) { nvmtl_fold_missed(r, "no writable page import"); return; }
    // a surface WindowServer presents through a scan-out buffer (stage 3) gets that copy in the same place
    nvmtl_scanout *so = nvmtl_scanout_get(r);
    // gpuperf phase 4 (WindowServer, nvmtl_scanout_p4.h): a bound composite whose pages have the scan-out layout is not
    // read back; at publication the kernel marks it newer, and a CPU lock or a copied frame pages it off from the
    // scan-out buffer (NMXAccel nmxScanPageoff). Its seed moves with the mark (no CPU lock, no page-off per frame).
    // A Linux compositor flips its buffer and copies only for a capture client that asks; here the page-off is that copy,
    // on demand. The default whenever the composite is bound (NVMTL_SCANOUT_READBACK: full / off for measuring).
    // WAS: also only at debug.intelxe.surfvram >= 2 and with the readback switch on: without the intelxe_surfvram boot-arg
    // every frame went back to its pages over PCIe (19.8 MB, 12.8 ms of GPU per frame on a B580 at 3440x1440).
    const nvmtl_p4_decision dec = so ? nvmtl_scanout_p4_decide_locked(so, r, row, (size_t)((uint8_t *)base - (uint8_t *)imp->base))
                                     : (nvmtl_p4_decision){ 0, 1, NVMTL_P4_UNBOUND };
    const int p4 = dec.p4;
    r->_p4 = p4 ? YES : NO;
    const int rb = dec.readback;
    const int rc = nvmtl_vk_cmd_image_to_host_scan(&part->commands, &r->_i, rb ? &imp->hb : NULL, (size_t)((uint8_t *)base - (uint8_t *)imp->base), row,
                                                    so ? &so->sb->buf : NULL, so ? so->pitch : 0);
    nvmtl_scanbuf *held = rc ? NULL : nvmtl_scanbuf_hold_locked(so);   // the copy runs later: its buffer lives until the part has finished
    if (!rc && so) { g_scanout_copies++; if (g_scanout_copies <= 3 || g_scanout_copies % 5000 == 0) nvlog("scan-out: %llu frames copied into scan-out buffers (folded)", g_scanout_copies); }
    nvmtl_scanout_put(so);
    if (rc) { nvmtl_surfimp_put(imp); return; }
    part->fold[part->nFold] = t; part->foldScan[part->nFold] = held; part->foldP4[part->nFold] = p4 ? YES : NO; part->foldImp[part->nFold++] = imp;
    nvmtl_surfcensus(r, 1);
    // implicit sync: the part's job waits for the surface's readers and writers on the GPU and becomes its newest writer
    // A WindowServer-created backing may be demoted after repeated CPU reads.
    // Keep its device cache marked before commit (above) and publish/schedule only
    // after this folded copy's fence is attached, just as for other demoted writers.
    if (!nvmtl_vk_cmd_surface_use(&part->commands, (uint32_t)IOSurfaceGetID(r->_surf), 1)
        && (!nvmtl_is_ws() || r->_svDemoted)) part->isyncPublish = YES;
    static _Atomic unsigned nf; const unsigned k = atomic_fetch_add(&nf, 1);
    if (k < 6 || (k & 8191) == 0) nvlog("surface-out: surface %u %ux%u folded into its part%s (%u so far)", (unsigned)IOSurfaceGetID(r->_surf), r->_i.w, r->_i.h,
                                        part->isyncPublish ? ", its fence attached and published at submission" : nvmtl_vk_isync() ? ", its fence attached" : "", k + 1);
}
static void nvmtl_fold_release(NVMTLSubmissionPart *part) {
    for (unsigned i = 0; i < part->nFold; i++) {
        nvmtl_surfimp_put((nvmtl_surfimp *)part->foldImp[i]); part->foldImp[i] = NULL;
        nvmtl_scanbuf_release((nvmtl_scanbuf *)part->foldScan[i]); part->foldScan[i] = NULL;
    }
    part->nFold = 0;
}
static BOOL nvmtl_part_vpub(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    for (unsigned i = 0; i < part->nVpub; i++) if (part->vpub[i] == r) return YES;
    return NO;
}
static BOOL nvmtl_part_folded(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    for (unsigned i = 0; i < part->nFold; i++) if (part->fold[i] == t) return YES;
    return NO;
}
static BOOL nvmtl_part_folded_p4(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    for (unsigned i = 0; i < part->nFold; i++) if (part->fold[i] == t) return part->foldP4[i];
    return NO;
}
// NVMTL_HITCH_MS: a surface published while a newer write of it is already sealed ("staleout": the separate copy can pick
// up that newer write), or after a newer write of it was published ("outoforder": the older pixels overwrite newer ones)
static void nvmtl_surface_out_order(NVMTLSubmissionPart *part, NVMTLTexture *t) {
    if (!nvmtl_hitch_ms()) return;
    for (unsigned i = 0; i < part->nSeq; i++) if (part->seqTex[i] == t) {
        const uint32_t s = part->seq[i];
        if (!nvmtl_part_folded(part, t) && __atomic_load_n(&t->_surfWSeq, __ATOMIC_ACQUIRE) > s) nvmtl_hitch_add_ns(NVH_STALEOUT, 0);
        uint32_t o = __atomic_load_n(&t->_surfOutSeq, __ATOMIC_ACQUIRE);
        if (o > s) nvmtl_hitch_add_ns(NVH_OOO, 0);
        while (o < s && !__atomic_compare_exchange_n(&t->_surfOutSeq, &o, s, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {}
        return;
    }
}
static void nvmtl_fold_release(NVMTLSubmissionPart *part);
@implementation NVMTLSubmissionPart
- (void)dealloc {
    if (inflight) {
        nvlog("commit: G-SUB BUG - a part submitted ahead reached dealloc unfinished; finishing it now");
        if (nvmtl_gsub_finish(self) != 0) {
            (void)CFBridgingRetain(resources); (void)CFBridgingRetain(surfaces); (void)CFBridgingRetain(scratch);
            memset(&commands, 0, sizeof commands);
            return;   // the folded copies' imports stay busy too: the GPU may still write them
        }
    }
    nvmtl_fold_release(self);
    if (commands.cb) nvmtl_vk_cmd_abandon(&commands);
}
@end

static NSMutableArray<NVMTLCommandBuffer *> *nvmtl_pending_buffers;
static BOOL nvmtl_pumping;
static void nvmtl_pump_buffers(void);
/* NVMTL_LOCK_V2 (nvmtl_plock.h). The state lock guards nvmtl_pending_buffers, nvmtl_pumping, nvmtl_pump_generation, every
   queue's _reservations and each command buffer's _reservation and _v2pending. It is a leaf: nothing under it submits,
   waits, takes another of these locks, or lets go of the last reference to a command buffer (their dealloc takes it):
   command buffers read from a weak reservation under it are __autoreleasing, released when an outer pool drains. The
   submit-ahead pass runs under the combiner (gV2Ahead), which nobody waits for. */
static os_unfair_lock gV2State = OS_UNFAIR_LOCK_INIT;
static nvmtl_combiner gV2Ahead = NVMTL_COMBINER_INIT;
static BOOL nvmtl_async_commit_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_ASYNC_COMMIT"); on = !(e && e[0] == '0');
        nvlog("commit: %s (NVMTL_ASYNC_COMMIT=%s)", on ? "ASYNC (default) - execution worker, submit ahead, queue capacity enforced" : "synchronous (NVMTL_ASYNC_COMMIT=0)", e ? e : "unset"); }
    return on;
}
static _Atomic unsigned long long gSkipEmpty;
static int nvmtl_skip_empty_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SKIP_EMPTY"); on = !(e && e[0] == '0');
        nvlog("commit: empty submission parts %s (NVMTL_SKIP_EMPTY=%s)", on ? "are NOT submitted" : "are submitted as before b78", e ? e : "unset"); }
    return on;
}

@implementation NVMTLCommandBuffer (EventSubmission)
- (void)nvmtlReserveQueue {
    if (_reservation) return;
    NVMTLQueueReservation *r = [NVMTLQueueReservation new]; r->buffer = self;
    _reservation = r;
    if (!_queue->_reservations) _queue->_reservations = [NSMutableArray new];
    [_queue->_reservations addObject:r];
}
- (void)nvmtlReleaseReservation {
    if (_reservation) [_queue->_reservations removeObjectIdenticalTo:_reservation];
    _reservation = nil;
}
- (void)nvmtlSealPartWithSignal:(id)event value:(uint64_t)value restart:(BOOL)restart {
    if (!_parts) _parts = [NSMutableArray new];
    nvmtl_g79_flush(self);
    NVMTLSubmissionPart *part = [NVMTLSubmissionPart new];
    // Draws or dispatches this recording could not take (nvk_cmdbuf.skipped: no descriptor set even after waiting for
    // in-flight work to give sets back, an address table, an unsupported dispatch): the command buffer ends with an
    // error, as a Metal driver reports what it could not do, and none of its parts is submitted (nvmtl_vk_submit_begin_at
    // refuses the recording too). WAS: committed without them, so writes were lost for good - Minecraft's texture atlas
    // uploads, black blocks for the whole session (2026-10-29). NVMTL_SUBMIT_PARTIAL=1: the old partial commit.
    if (_c.skipped && !nvmtl_vk_submit_partial_on())
        [self nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
            code:_c.skip_oom ? MTLCommandBufferErrorOutOfMemory : MTLCommandBufferErrorInternal
            userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"%u draws/dispatches could not be recorded (%u for want of GPU memory)",
                                                  _c.skipped, _c.skip_oom]}]];
    part->commands = _c;
    memset(&_c, 0, sizeof(_c));
    part->resources = [(NVMTLResSet *)_resources nvmtlTakeAll]; part->surfaces = [_surfDirty copy];
    part->owner = self;
    for (NVMTLTexture *s in part->surfaces) {
        if (part->nSeq < 16) { part->seqTex[part->nSeq] = s; part->seq[part->nSeq++] = __atomic_add_fetch(&s->_surfWSeq, 1, __ATOMIC_ACQ_REL); }
        nvmtl_fold_surface_out(part, s);
    }
    if (part->isyncPublish) { part->commands.on_submit = nvmtl_part_published_at_submit; part->commands.on_submit_ctx = (__bridge void *)part; }
    // Another process may read an IOSurface once its writer is scheduled: CoreAnimation and Apple's GL (AMOGL, Minecraft)
    // hand a surface to WindowServer after waitUntilScheduled, and Apple's drivers order the compositor's GPU read after
    // the writer's GPU write in the kernel. Nothing here can: WindowServer's surface-in reads the surface's pages, which
    // the folded copy fills only when the part finishes, and the seed moves only at publication. Handed over at
    // submission, the compositor either kept its older copy (the seed had not moved: a frame from the past) or read pages
    // the copy was still writing (a partly written frame). So a command buffer that writes an IOSurface other than its own
    // drawable (a drawable is presented after completion anyway) reports scheduled only once its surfaces are published.
    // WindowServer's own writes are exempt (the display flips wait for its marks; captures: nvmtl_ws_note_captures).
    // NVMTL_SCHEDULE_AFTER_SURFACE_WRITES=0 restores scheduled-at-submission.
    static int after = -1;
    if (after < 0) { const char *e = getenv("NVMTL_SCHEDULE_AFTER_SURFACE_WRITES"); const bool ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        after = e ? e[0] == '1' : !ws;
        nvlog("surface writes: scheduled %s (NVMTL_SCHEDULE_AFTER_SURFACE_WRITES=%s)", after ? "after the surfaces are published (another process may read them then)" : "at submission", e ? e : "unset"); }
    NVMTLTexture *drawable = _present ? (_present->_parent ?: _present) : nil;
    // with implicit sync a folded surface is published at submission instead (nvmtl_part_published_at_submit), and so is a
    // B580 VRAM-shared one (vpub): its readers in other processes wait for this part's fence on the GPU (surface fences)
    // and a CPU lock pages it off only after the kernel has waited for the same fence, so it may be handed over as soon as
    // it is submitted. WAS: scheduled only after the part completed, which made Apple GL's two surface-writing flushes per
    // frame (Minecraft's blit into its default framebuffer, the swap blit) wait for the GPU in waitUntilScheduled:
    // SCHEDCENSUS 2026-10-29, 2 of the ~10 commits per frame scheduled by the worker after their fence, ~0.95 ms each.
    for (NVMTLTexture *s in part->surfaces)
        if (after && s != drawable && !(part->isyncPublish && (nvmtl_part_folded(part, s) || nvmtl_part_vpub(part, s)))) _scheduleAfterPublish = YES;
    part->scratch = [_scratch copy]; part->signalEvent = event; part->value = value;
    part->encoders = _nEnc - _nEncSealed; _nEncSealed = _nEnc;
    if (_g79Sig.count) { part->signals = [_g79Sig copy]; [_g79Sig removeAllObjects]; }
    [_parts addObject:part];
    [_surfDirty removeAllObjects]; [_scratch removeAllObjects];
    // The repeated-setBytes cache points at ring chunks without owning them; the part now owns those chunks and frees
    // them when it completes, which can happen while this command buffer is still being encoded. A cache entry that
    // outlived its chunk was bound and retained again by the next identical setBytes, and the compute encoder's binding
    // table later released a freed object (mediaanalysisd under MPS convolution, endEncoding / .cxx_destruct).
    memset(_g79BRing, 0, sizeof _g79BRing); memset(_g79BLen, 0, sizeof _g79BLen);
    // Recording goes on after this seal, and the encoders' binding tables still name these resources by raw Vulkan
    // handle: a later dispatch snapshots them again without rebinding. The command buffer keeps holding them until it
    // completes, as Metal's do. WAS: only the part held them, so a texture the app had released died when that part
    // completed and the next dispatch wrote its destroyed image view (NVK abort in get_sampled_image_view_desc under
    // nvk_UpdateDescriptorSets: mediaanalysisd, Safari, wallpapers).
    if (restart) for (id o in (NVMTLResSet *)part->resources) [(NVMTLResSet *)_resources addObject:o];
    if (restart && nvmtl_vk_cmd_begin(&_queue->_q, &_c)) {
        _cbError = [NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorOutOfMemory
            userInfo:@{NSLocalizedDescriptionKey:@"Could not allocate recording after event boundary"}];
    }
}
- (void)nvmtlRecordWait:(id)event value:(uint64_t)value {
    if (!event || _committed) return;
    if (nvmtl_g79_on(NVMTL_G79_EV)) {
        BOOL self_ = NO;
        for (NSArray *p in _g79Promised) if (p[0] == event && [p[1] unsignedLongLongValue] >= value) { self_ = YES; break; }
        BOOL held = !self_ && [event respondsToSelector:@selector(signaledValue)]
            && ((uint64_t(*)(id,SEL))objc_msgSend)(event, @selector(signaledValue)) >= value;
        if (self_ || held) {
            if (_g79C2C && nvmtl_g79_on(NVMTL_G79_WFOLD)) { _g79Owed = YES; nvmtl_g79_count(G79C_WAITFOLD); }
            else if (_c.cb) { nvmtl_vk_cmd_barrier(&_c); _g79Owed = NO; _g79C2C = NO; }
            nvmtl_g79_count(self_ ? G79C_WAITSELF : G79C_WAITHELD);
            return;
        }
        nvmtl_g79_count(G79C_WAITSPLIT);
    }
    [self nvmtlSealPartWithSignal:nil value:0 restart:YES];
    NVMTLSubmissionPart *part = [NVMTLSubmissionPart new];
    part->waitEvent = event; part->value = value; [_parts addObject:part];
}
- (void)nvmtlRecordSignal:(id)event value:(uint64_t)value {
    if (!event || _committed) return;
    if (nvmtl_g79_on(NVMTL_G79_EV)) {
        if (!_g79Sig) _g79Sig = [NSMutableArray new];
        if (!_g79Promised) _g79Promised = [NSMutableArray new];
        [_g79Sig addObject:@[event, @(value)]]; [_g79Promised addObject:@[event, @(value)]];
        nvmtl_g79_count(G79C_SIGRIDE);
        return;
    }
    [self nvmtlSealPartWithSignal:event value:value restart:YES];
}
- (void)nvmtlDispatchHandlers:(NSArray *)handlers scheduled:(BOOL)scheduled {
    if (!handlers.count) return;
    dispatch_queue_t order;
    dispatch_queue_t target;
    // the class, not the queue: apps may hold @synchronized(queue) while they wait for this command buffer
    @synchronized ([NVMTLCommandQueue class]) {
        if (!_queue->_callbackOrderQueue)
            _queue->_callbackOrderQueue = dispatch_queue_create("com.nvmtl.command-callbacks", DISPATCH_QUEUE_SERIAL);
        order = _queue->_callbackOrderQueue;
        target = _queue->_completionQueue ?: dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    }
    for (NSUInteger i = 0; i < handlers.count; ++i) {
        dispatch_group_enter(_callbackGroup);
        if (scheduled) dispatch_group_enter(_scheduledGroup);
    }
    dispatch_async(order, ^{
        dispatch_sync(target, ^{
            for (MTLCommandBufferHandler handler in handlers) {
                handler(self);
                if (scheduled) dispatch_group_leave(self->_scheduledGroup);
                dispatch_group_leave(self->_callbackGroup);
            }
        });
    });
}
- (void)nvmtlMarkScheduled {
    if (_scheduleAfterPublish) return;
    [_completionCondition lock];
    if (!_wasScheduled) {
        if (nvmtl_sc_on()) atomic_fetch_add(sc_on_worker ? &sc_by_worker : &sc_by_commit, 1);
        _wasScheduled = YES; _cbstatus = MTLCommandBufferStatusScheduled;
        NSArray *handlers = [_scheduledHandlers copy]; [_scheduledHandlers removeAllObjects];
        [self nvmtlDispatchHandlers:handlers scheduled:YES];
        [_completionCondition broadcast];
    }
    [_completionCondition unlock];
}
- (void)nvmtlCollectDeviceTiming:(nvk_cmdbuf *)commands {
    if (!commands->timing.valid) { _gpuTimingInvalid = YES; return; }
    if (!_gpuTimingSeen) { _gpuTiming = commands->timing; _gpuTimingSeen = YES; }
    else {
        _gpuTiming.end_tick = commands->timing.end_tick;
        _gpuTiming.host_after_ns = commands->timing.host_after_ns;
    }
}
- (void)nvmtlFinishExecution:(NSError *)error {
    // V2: no submit-ahead pass is inside this command buffer from here on (its parts are released below)
    if (nvmtl_lock_v2()) nvmtl_cb_finish_gate(&_v2busy, &_v2fin);
    if (_scheduleAfterPublish) { _scheduleAfterPublish = NO; [self nvmtlMarkScheduled]; }
    for (NSUInteger i = _nextPart; i < _parts.count; i++) {
        NVMTLSubmissionPart *p = _parts[i];
        if (![p isKindOfClass:[NVMTLSubmissionPart class]] || !p->inflight) continue;
        int result = nvmtl_gsub_finish(p);
        if (!result) [self nvmtlCollectDeviceTiming:&p->commands];
        else _gpuTimingInvalid = YES;
        if (result <= -2) {   // lost (-2 backend down, -3/-4/-5 GPU recovery): the GPU may have held them
            if (!nvmtl_quarantined_parts) nvmtl_quarantined_parts = [NSMutableArray new];
            [nvmtl_quarantined_parts addObject:p];
        }
    }
    if (!error && _presentations.count) {
        // Present on a serial queue of its own, in order, not on the execution queue: -[CAMetalDrawable present] takes the
        // layer's lock, which -[CAMetalLayer nextDrawable] holds on the app's thread, and a hook inside nextDrawable (the
        // Metal HUD creating its font texture) waits for a command buffer queued behind this one. WAS: inline here, a
        // deadlock (No Man's Sky with MetalForceHudEnabled: white window, main thread in waitUntilCompleted, the execution
        // queue in present). NVMTL_PRESENT_INLINE=1: the old inline presents.
        static dispatch_queue_t presentQ; static int inl = -1; static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *e = getenv("NVMTL_PRESENT_INLINE"); inl = e && *e && *e != '0';
            presentQ = dispatch_queue_create("nvmtl.present", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0));
        });
        if (inl) { for (dispatch_block_t action in _presentations) action(); }
        else { NSArray *actions = [_presentations copy]; dispatch_async(presentQ, ^{ for (dispatch_block_t action in actions) action(); }); }
    }
    [_presentations removeAllObjects]; _present = nil;
    nvmtl_surface_finish_writes(self);
    [_completionCondition lock];
    // GPUStartTime/GPUEndTime convert the ticks when first asked (most command buffers are never asked)
    _gpuStart = 0; _gpuEnd = 0; _gpuTimesPending = !error && _gpuTimingSeen && !_gpuTimingInvalid;
    _legacyKernelEnd = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
    _cbError = error; _cbstatus = error ? MTLCommandBufferStatusError : MTLCommandBufferStatusCompleted;
    _executionDone = YES;
    [_completionCondition unlock];
    [_parts removeAllObjects];
    // A finished command buffer lets go of everything only its GPU work needed: the setBytes ring and the scratch
    // buffers it filled, and any resources still listed on it. Apple's command buffers do the same. An app's
    // autorelease pool can keep thousands of finished command buffers alive during a long loop (Geekbench 7 does),
    // and holding these until the command buffer itself died filled VRAM until allocations were refused.
    [_scratch removeAllObjects]; _e78Ring = nil; _e78RingOff = 0; memset(_g79BRing, 0, sizeof _g79BRing);
    [(NVMTLResSet *)_resources removeAllObjects]; [_stTwins removeAllObjects];
    // Applications may retain completed command buffers. Pending overwrite
    // bookkeeping must not keep their IOSurfaces and VRAM alive indefinitely.
    [_surfOverwrites removeAllObjects]; _surfOverwrites = nil;
    if (!error) nvmtl_res2_cb_done(self);
    [self nvmtlReleaseCapacity];
}
// glcpu3: the completed handlers as the command buffer's entry vector, run in order on the same queues as
// nvmtlDispatchHandlers; the vector and the arena chunks the entries point into are let go after the last one ran
- (void)nvmtlDispatchHandlerVector:(void **)v count:(unsigned)n chunks:(void *)chunks first:(NSArray *)first {
    const NSUInteger total = first.count + n;
    if (!total) { free(v); nvmtl_hchunks_put((nvmtl_hchunk *)chunks); return; }
    dispatch_queue_t order;
    dispatch_queue_t target;
    @synchronized ([NVMTLCommandQueue class]) {
        if (!_queue->_callbackOrderQueue)
            _queue->_callbackOrderQueue = dispatch_queue_create("com.nvmtl.command-callbacks", DISPATCH_QUEUE_SERIAL);
        order = _queue->_callbackOrderQueue;
        target = _queue->_completionQueue ?: dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0);
    }
    for (NSUInteger i = 0; i < total; ++i) dispatch_group_enter(_callbackGroup);
    dispatch_async(order, ^{
        dispatch_sync(target, ^{
            for (MTLCommandBufferHandler handler in first) {
                handler(self);
                dispatch_group_leave(self->_callbackGroup);
            }
            for (unsigned k = 0; k < n; k++) {
                nvmtl_handler_entry_call(v[k], self);
                nvmtl_handler_entry_drop(v[k]);
                dispatch_group_leave(self->_callbackGroup);
            }
        });
        free(v); nvmtl_hchunks_put((nvmtl_hchunk *)chunks);
    });
}
/* NVMTL_DONE_SPLIT=1 (measurement): where the time between a command buffer's fence and its waiter goes - the worker's
   fence return -> its broadcast (bookkeeping on the worker), and the broadcast -> the waiter awake (the wake-up).
   Averages logged every 3 s. */
__thread uint64_t nvmtl_done_split_fence_ns;
static _Atomic uint64_t ds_pub_ns, ds_pub_n, ds_wake_ns, ds_wake_n;
int nvmtl_done_split_on(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_DONE_SPLIT"); on = e && e[0] == '1'; } return on;
}
void nvmtl_done_split_woke(uint64_t published_ns) {
    if (!published_ns) return;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_fetch_add(&ds_wake_ns, now - published_ns); atomic_fetch_add(&ds_wake_n, 1);
    static _Atomic uint64_t last; const uint64_t l = atomic_load(&last);
    if (now - l < 3000000000ull || !atomic_compare_exchange_strong(&last, &(uint64_t){ l }, now)) return;
    const uint64_t pn = atomic_exchange(&ds_pub_n, 0), pt = atomic_exchange(&ds_pub_ns, 0);
    const uint64_t wn = atomic_exchange(&ds_wake_n, 0), wt = atomic_exchange(&ds_wake_ns, 0);
    nvlog_keep("DONESPLIT: fence return -> broadcast %.2f us (%llu), broadcast -> waiter awake %.2f us (%llu)",
               pn ? pt / 1e3 / (double)pn : 0.0, (unsigned long long)pn, wn ? wt / 1e3 / (double)wn : 0.0, (unsigned long long)wn);
}
- (void)nvmtlFinishCallbacks {
    if (nvmtl_done_split_on()) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (nvmtl_done_split_fence_ns && now > nvmtl_done_split_fence_ns) {
            atomic_fetch_add(&ds_pub_ns, now - nvmtl_done_split_fence_ns); atomic_fetch_add(&ds_pub_n, 1); }
        nvmtl_done_split_fence_ns = 0;
        _doneSplitNs = now;
    }
    [_completionCondition lock];
    if (nvmtl_handler_arena_on()) {
        NSArray *first = _handlers.count ? [_handlers copy] : nil;
        [_handlers removeAllObjects];
        void **v = _hv; const unsigned n = _hn; void *chunks = _hchunks;
        _hv = NULL; _hn = _hcap = 0; _hchunks = NULL;
        [self nvmtlDispatchHandlerVector:v count:n chunks:chunks first:first];
        _completionReady = YES;
        [_completionCondition broadcast]; [_completionCondition unlock];
        return;
    }
    NSMutableArray *handlers = [NSMutableArray arrayWithCapacity:_handlers.count + _hn];
    if (_handlers.count) [handlers addObjectsFromArray:_handlers];
    [_handlers removeAllObjects];
    for (unsigned k = 0; k < _hn; k++) [handlers addObject:(__bridge_transfer id)_hv[k]];
    _hn = 0;
    [self nvmtlDispatchHandlers:handlers scheduled:NO];
    _completionReady = YES;
    [_completionCondition broadcast]; [_completionCondition unlock];
}
static unsigned nvmtl_gsub_cap(void) {
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("NVMTL_GSUB_INFLIGHT"); int v = e ? atoi(e) : 16;
        cap = v < 1 ? 1 : v > 32 ? 32 : v;
        nvlog("commit: G-SUB submit-ahead %s, at most %d parts in flight (NVMTL_GSUB_INFLIGHT=%s)",
              nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) ? "ON" : "off", cap, e ? e : "unset"); }
    return (unsigned)cap;
}
static BOOL nvmtl_gsub_ahead_on(void) { return nvmtl_async_commit_on() && nvmtl_g79_on(NVMTL_G79_SUB) && nvmtl_gsub_cap(); }
static NSMapTable *nvmtl_evfold_submitted;
static _Atomic unsigned long long gEvFold, gEvFoldEmpty;
static BOOL nvmtl_evfold_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_EVFOLD"); on = !(e && e[0] == '0');
        nvlog("commit: sub1 EVFOLD %s - an event wait whose signal is already in the channel %s (NVMTL_EVFOLD=%s)",
              on ? "ON" : "off", on ? "is ordered by the channel, no CPU hold" : "is held until the CPU reads the signal", e ? e : "unset"); }
    return on;
}
static uint64_t nvmtl_evfold_promised(id event) {
    return event ? [[nvmtl_evfold_submitted objectForKey:event] unsignedLongLongValue] : 0;
}
static void nvmtl_evfold_promise(id event, uint64_t value) {
    if (!event || !nvmtl_evfold_on()) return;
    if (!nvmtl_evfold_submitted) nvmtl_evfold_submitted = [NSMapTable weakToStrongObjectsMapTable];
    if (value > nvmtl_evfold_promised(event)) [nvmtl_evfold_submitted setObject:@(value) forKey:event];
}
static void nvmtl_evfold_promise_part(NVMTLSubmissionPart *part) {
    if (part->signalEvent) nvmtl_evfold_promise(part->signalEvent, part->value);
    for (NSArray *s in part->signals) nvmtl_evfold_promise(s[0], [s[1] unsignedLongLongValue]);
}
static void nvmtl_evfold_census(_Atomic unsigned long long *c, const char *what) {
    const unsigned long long k = atomic_fetch_add(c, 1) + 1;
    if (!(k & (k - 1))) nvlog("commit: sub1 EVFOLD %llu %s", k, what);
}
static void nvmtl_gsub_census(void) {
    const unsigned long long k = atomic_load(&gsub_ahead);
    if (k & (k - 1)) return;
    uint64_t s[9]; nvmtl_vk_gsub_nvk_stats(s, 9);
    nvlog("commit: G-SUB %llu parts submitted ahead, peak %llu in flight | NVK %s submits %llu acquires %llu cpuwaits %llu slotwaits %llu fifowaits %llu deferred %llu reaped %llu waits %llu",
          k, (unsigned long long)atomic_load(&gsub_peak), s[0] == ~0ull ? "G-SUB ABSENT" : s[0] ? "async" : "SYNC (NVRM_ASYNC=0)",
          (unsigned long long)s[1], (unsigned long long)s[2], (unsigned long long)s[3], (unsigned long long)s[4],
          (unsigned long long)s[5], (unsigned long long)s[6], (unsigned long long)s[7], (unsigned long long)s[8]);
}
// A texture over a copy-backed buffer must refresh the same bytes as a direct
// buffer binding. Keep direct managed-buffer writeback and copy each backing once.
/* glperf1: the backings are collected in an array, each once per call by a call-unique stamp on the buffer, instead of an
   NSMutableSet built and rehashed for every submission part (a part of Minecraft's GL terrain pass holds ~2000 resources:
   the set's rehash, adds, -allObjects and dealloc were ~2.6% of the render thread). Two calls at once on different threads
   can only list a buffer twice, never miss it: a repeated copy of the same bytes. */
static _Atomic uint64_t gCopyStamp;
// One resource's host-copy backing into copies (once per call, by the call's stamp on the buffer).
static void nvmtl_submission_copy_one(__unsafe_unretained id resource, BOOL writtenOnly, uint64_t stamp, Class kBuf, Class kTex,
                                      NSMutableArray<NVMTLBuffer *> *__strong *copies) {
    __unsafe_unretained NVMTLBuffer *buffer = nil;
    const Class k = object_getClass(resource);
    if (k == kBuf || (k != kTex && [resource isKindOfClass:kBuf])) buffer = resource;
    else if (k == kTex || [resource isKindOfClass:kTex]) {
        NVMTLTexture *texture = resource;
        if (writtenOnly && texture->_usage != MTLTextureUsageUnknown &&
            !(texture->_usage & (MTLTextureUsageShaderWrite | MTLTextureUsageRenderTarget))) return;
        buffer = texture->_backBuf ?: (texture->_parent ? texture->_parent->_backBuf : nil);
    }
    if (!buffer || (!buffer->_hostPtr && !buffer->_shadow.map)) return;
    if (__atomic_load_n(&buffer->_cpStamp, __ATOMIC_RELAXED) == stamp) return;
    __atomic_store_n(&buffer->_cpStamp, stamp, __ATOMIC_RELAXED);
    if (!*copies) *copies = [NSMutableArray new];
    [*copies addObject:buffer];
}
static NSArray<NVMTLBuffer *> *nvmtl_submission_copies(NSSet *resources, BOOL writtenOnly) {
    NSMutableArray<NVMTLBuffer *> *copies = nil;
    const uint64_t stamp = atomic_fetch_add_explicit(&gCopyStamp, 1, memory_order_relaxed) + 1;
    static Class kBuf, kTex, kPin; static dispatch_once_t once;
    dispatch_once(&once, ^{ kBuf = [NVMTLBuffer class]; kTex = [NVMTLTexture class]; kPin = [NVMTLHeapPin class]; });
    for (__unsafe_unretained id resource in resources) {
        if (object_getClass(resource) == kPin) {   // 0aacf15: a used heap - its members with a host copy
            for (__unsafe_unretained id member in [(NVMTLHeapPin *)resource allCopyCandidates])
                nvmtl_submission_copy_one(member, writtenOnly, stamp, kBuf, kTex, &copies);
            continue;
        }
        nvmtl_submission_copy_one(resource, writtenOnly, stamp, kBuf, kTex, &copies);
    }
    return copies;
}
static void nvmtl_prepare_submission_copies(NVMTLSubmissionPart *part) {
    if (part->copyInputsPrepared) return;
    // A sealed part's membership and its buffers' backing modes do not change.
    // The ahead pass can retry a held part many times: discover its upload
    // backings once, but refresh their bytes only when it actually submits.
    part->copyInputs = nvmtl_submission_copies(part->resources, NO);
    for (NVMTLBuffer *buffer in part->copyInputs) {
        if (buffer->_hostPtr) { part->hasHostCopies = YES; break; }
    }
    part->copyInputsPrepared = YES;
}
- (BOOL)nvmtlSubmitAhead {
    // a commit still putting its kernel marks in (nvmtlSubmitWithEvents) must not reach the GPU before them
    if (_executionDone || _cbError || !_committed || _commitMarking) { nvmtl_sc_refuse(_commitMarking ? SC_MARKING : SC_NOTCOMMITTED); return NO; }
    if (_nextSubmit < _nextPart) _nextSubmit = _nextPart;
    while (_nextSubmit < _parts.count) {
        if (atomic_load(&gsub_inflight) >= nvmtl_gsub_cap()) { nvmtl_sc_refuse(SC_CAP); return NO; }
        NVMTLSubmissionPart *part = _parts[_nextSubmit];
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) return NO;
        if (part->claimed) { nvmtl_sc_refuse(SC_CLAIMED); return NO; }
        if (part->waitEvent) {
            if (!nvmtl_evfold_on() || nvmtl_evfold_promised(part->waitEvent) < part->value) { nvmtl_sc_refuse(SC_EVENT); return NO; }
            part->waitFolded = YES; _nextSubmit++; nvmtl_evfold_census(&gEvFold, "event waits ordered by the channel (no CPU hold)");
            continue;
        }
        if (!part->commands.cb) { nvmtl_sc_refuse(SC_NOCMDS); return NO; }
        if (!part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && nvmtl_skip_empty_on()) {
            /* An empty part is scheduled here even when it is the command buffer's last. WAS: a last empty part waited for
               the execution worker, which was itself waiting on an earlier part's fence, and held every later command
               buffer of the queue behind it: Apple's GL commits an empty command buffer for each glBufferSubData flush
               and waits until it is scheduled, so Minecraft's render thread spent 26% waiting for the GPU (2026-10-28).
               Its completion still runs in queue order on the worker. NVMTL_EMPTY_AHEAD=0: the old hold. */
            static int last_ok = -1; if (last_ok < 0) { const char *e = getenv("NVMTL_EMPTY_AHEAD"); last_ok = !(e && e[0] == '0'); }
            if (!nvmtl_evfold_on() || (!last_ok && _nextSubmit + 1 >= _parts.count)) return NO;
            [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            nvmtl_vk_cmd_abandon(&part->commands);
            part->aheadEmpty = YES; _nextSubmit++;
            nvmtl_evfold_promise_part(part);
            nvmtl_evfold_census(&gEvFoldEmpty, "empty split parts retired by the ahead pass");
            continue;
        }
        nvmtl_prepare_submission_copies(part);
        {
            static int hold = -1;
            if (hold < 0) { const char *e = getenv("NVMTL_GSUB_HOSTPTR_LIFT"); hold = (e && *e && *e != '0') ? 0 : 1; }
            if (hold && part->hasHostCopies) { nvmtl_sc_refuse(SC_HOSTCOPY); return NO; }
        }
        nvmtl_inflight *f = nvmtl_vk_inflight_new();
        if (!f) return NO;
        for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);
        if (!part->isyncPublish) [self nvmtlMarkScheduled];   // else once its surfaces are published (on_submit)
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        const int r = nvmtl_vk_submit_begin(&part->commands, f);
        if (part->isyncPublish) [self nvmtlMarkScheduled];   // a failed submit is scheduled (and fails) all the same
        _nextSubmit++;
        if (r) {
            nvmtl_vk_inflight_free(f);
            part->aheadFailed = r;
            return NO;
        }
        part->inflight = f;
        nvmtl_evfold_promise_part(part);
        const unsigned long long n = atomic_fetch_add(&gsub_inflight, 1) + 1;
        if (n > atomic_load(&gsub_peak)) atomic_store(&gsub_peak, n);
        atomic_fetch_add(&gsub_ahead, 1);
        nvmtl_gsub_census();
    }
    return YES;
}
// NVMTL_LOCK_V2: the same pass for one command buffer, under the combiner instead of the class lock. The worker may claim
// a part at any time, so each part is taken first (FREE -> SUBMITTING) and let go as AHEAD once the pass has done with it
// (or FREE if it left it as it was); the command buffer's _parts are touched only inside the gate (enter/leave), which
// nvmtlFinishExecution closes before it releases them. Returns YES when nothing of it holds back the queue's later
// command buffers: all its parts have gone ahead, or it has finished.
- (BOOL)nvmtlSubmitAheadV2Inside {
    if (_cbError || !__atomic_load_n(&_committed, __ATOMIC_ACQUIRE) || __atomic_load_n(&_commitMarking, __ATOMIC_ACQUIRE)) {
        nvmtl_sc_refuse(__atomic_load_n(&_commitMarking, __ATOMIC_RELAXED) ? SC_MARKING : SC_NOTCOMMITTED); return NO; }
    const NSUInteger done = __atomic_load_n(&_nextPart, __ATOMIC_ACQUIRE);
    if (_nextSubmit < done) _nextSubmit = done;
    while (_nextSubmit < _parts.count) {
        if (atomic_load(&gsub_inflight) >= nvmtl_gsub_cap()) { nvmtl_sc_refuse(SC_CAP); return NO; }
        NVMTLSubmissionPart *part = _parts[_nextSubmit];
        if (![part isKindOfClass:[NVMTLSubmissionPart class]]) return NO;
        const int was = nvmtl_part_take(&part->v2state);
        if (was == NVMTL_PART_AHEAD) { _nextSubmit++; continue; }
        if (was != NVMTL_PART_FREE) { nvmtl_sc_refuse(SC_CLAIMED); return NO; }   // the worker runs it
        if (part->waitEvent) {
            if (!nvmtl_evfold_on() || nvmtl_evfold_promised(part->waitEvent) < part->value) {
                nvmtl_part_release(&part->v2state, NVMTL_PART_FREE); nvmtl_sc_refuse(SC_EVENT); return NO; }
            part->waitFolded = YES; nvmtl_part_release(&part->v2state, NVMTL_PART_AHEAD);
            _nextSubmit++; nvmtl_evfold_census(&gEvFold, "event waits ordered by the channel (no CPU hold)");
            continue;
        }
        if (!part->commands.cb) { nvmtl_part_release(&part->v2state, NVMTL_PART_FREE); nvmtl_sc_refuse(SC_NOCMDS); return NO; }
        if (!part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && nvmtl_skip_empty_on()) {
            // as nvmtlSubmitAhead: an empty part is scheduled here even when it is the command buffer's last
            static int last_ok = -1; if (last_ok < 0) { const char *e = getenv("NVMTL_EMPTY_AHEAD"); last_ok = !(e && e[0] == '0'); }
            if (!nvmtl_evfold_on() || (!last_ok && _nextSubmit + 1 >= _parts.count)) { nvmtl_part_release(&part->v2state, NVMTL_PART_FREE); return NO; }
            [self nvmtlMarkScheduled];
            if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
            nvmtl_vk_cmd_abandon(&part->commands);
            part->aheadEmpty = YES;
            nvmtl_evfold_promise_part(part);
            nvmtl_part_release(&part->v2state, NVMTL_PART_AHEAD); _nextSubmit++;
            nvmtl_evfold_census(&gEvFoldEmpty, "empty split parts retired by the ahead pass");
            continue;
        }
        nvmtl_prepare_submission_copies(part);
        {
            static int hold = -1;
            if (hold < 0) { const char *e = getenv("NVMTL_GSUB_HOSTPTR_LIFT"); hold = (e && *e && *e != '0') ? 0 : 1; }
            if (hold && part->hasHostCopies) { nvmtl_part_release(&part->v2state, NVMTL_PART_FREE); nvmtl_sc_refuse(SC_HOSTCOPY); return NO; }
        }
        nvmtl_inflight *f = nvmtl_vk_inflight_new();
        if (!f) { nvmtl_part_release(&part->v2state, NVMTL_PART_FREE); return NO; }
        for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);
        if (!part->isyncPublish) [self nvmtlMarkScheduled];   // else once its surfaces are published (on_submit)
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        const int r = nvmtl_vk_submit_begin(&part->commands, f);
        if (part->isyncPublish) [self nvmtlMarkScheduled];   // a failed submit is scheduled (and fails) all the same
        _nextSubmit++;
        if (r) {
            nvmtl_vk_inflight_free(f);
            part->aheadFailed = r;
            nvmtl_part_release(&part->v2state, NVMTL_PART_AHEAD);
            return NO;
        }
        part->inflight = f;
        nvmtl_evfold_promise_part(part);   // before AHEAD: the worker may complete the part as soon as it sees it
        // counted before AHEAD too: the worker's nvmtl_gsub_finish may run as soon as it sees the part (review 9 Nov)
        const unsigned long long n = atomic_fetch_add(&gsub_inflight, 1) + 1;
        nvmtl_part_release(&part->v2state, NVMTL_PART_AHEAD);
        if (n > atomic_load(&gsub_peak)) atomic_store(&gsub_peak, n);
        atomic_fetch_add(&gsub_ahead, 1);
        nvmtl_gsub_census();
    }
    return YES;
}
- (BOOL)nvmtlSubmitAheadV2 {
    if (!nvmtl_cb_enter(&_v2busy, &_v2fin)) return YES;   // finished (or finishing): nothing left of it to hold back
    const BOOL all = [self nvmtlSubmitAheadV2Inside];
    // the reservation stays while the gate is open (the worker lets it go only after nvmtlFinishExecution)
    if (all) { NVMTLQueueReservation *r = _reservation; if (r) __atomic_store_n(&r->v2allAhead, 1, __ATOMIC_RELAXED); }
    nvmtl_cb_leave(&_v2busy);
    return all;
}
static void nvmtl_gsub_ahead_pass1(void);
static void nvmtl_gsub_ahead_pass(void) {
    for (int round = 0; round < 64; round++) {
        const unsigned long long before = atomic_load(&gEvFold);
        nvmtl_gsub_ahead_pass1();
        if (atomic_load(&gEvFold) == before) break;
    }
}
static void nvmtl_gsub_ahead_pass1(void) {
    NSMutableSet *queues = [NSMutableSet new];
    for (NVMTLCommandBuffer *cb in nvmtl_pending_buffers) {
        id q = cb->_queue;
        if (!q || [queues containsObject:q]) continue;
        [queues addObject:q];
        for (NVMTLQueueReservation *r in [cb->_queue->_reservations copy]) {
            NVMTLCommandBuffer *b = r->buffer;
            if (!b) continue;
            if (![b nvmtlSubmitAhead]) break;
        }
    }
}
// NVMTL_LOCK_V2: one pass over the queues with pending command buffers. Under the state lock only the command buffers to
// try are collected (no allocation, no submit): per queue, in reservation order, those whose parts have not all gone
// ahead yet. They are tried after it, in the same order; a queue stops at the first one that holds it back.
static void nvmtl_v2_ahead_pass1(void) { @autoreleasepool {
    enum { MAXQ = 16, MAXB = 160, PERQ = 48 };
    __unsafe_unretained NVMTLCommandBuffer *bufs[MAXB]; unsigned nb = 0;
    __unsafe_unretained NVMTLCommandQueue *qs[MAXQ]; unsigned nq = 0;
    nvmtl_plock_lock(&gV2State);
    for (__unsafe_unretained NVMTLCommandBuffer *cb in nvmtl_pending_buffers) {
        __unsafe_unretained NVMTLCommandQueue *q = cb->_queue;
        if (!q) continue;
        BOOL seen = NO;
        for (unsigned k = 0; k < nq; k++) if (qs[k] == q) { seen = YES; break; }
        if (seen) continue;
        if (nq == MAXQ || nb + 1 >= MAXB) break;
        qs[nq++] = q;
        unsigned per = 0;
        for (__unsafe_unretained NVMTLQueueReservation *r in q->_reservations) {
            if (__atomic_load_n(&r->v2allAhead, __ATOMIC_RELAXED)) continue;
            if (per == PERQ || nb + 1 >= MAXB) break;
            __autoreleasing NVMTLCommandBuffer *b = r->buffer;   // kept by this pass's pool, never released under the lock
            if (!b) continue;
            bufs[nb++] = b; per++;
        }
        bufs[nb++] = nil;   // end of this queue
    }
    os_unfair_lock_unlock(&gV2State);
    BOOL stop = NO;
    for (unsigned i = 0; i < nb; i++) {
        if (!bufs[i]) { stop = NO; continue; }
        if (!stop && ![bufs[i] nvmtlSubmitAheadV2]) stop = YES;
    }
} }
static void nvmtl_v2_ahead_pass(void *ctx) {
    (void)ctx;
    for (int round = 0; round < 64; round++) {   // as nvmtl_gsub_ahead_pass: again while event waits got folded
        const unsigned long long before = atomic_load(&gEvFold);
        nvmtl_v2_ahead_pass1();
        if (atomic_load(&gEvFold) == before) break;
    }
}
// run the pass here, or have the thread running it run it again; never waits
static void nvmtl_v2_ahead(void) { nvmtl_combine(&gV2Ahead, nvmtl_v2_ahead_pass, NULL, 8); }
- (BOOL)nvmtlAdvancePart {
    if (_executionDone) return NO;
    if (_cbError) { [self nvmtlFinishExecution:_cbError]; return YES; }
    if (_nextPart >= _parts.count) { [self nvmtlFinishExecution:nil]; return YES; }
    NVMTLSubmissionPart *part = _parts[_nextPart];
    BOOL ahead;   // claimed under the class lock: the commit-time pass stops at a claimed part
    const BOOL v2 = nvmtl_lock_v2();
    int v2took = NVMTL_PART_AHEAD;
    if (v2) {   // V2: the per-part hand-off; a part the pass is submitting right now is waited for (one vkQueueSubmit)
        v2took = nvmtl_part_claim(&part->v2state);
        ahead = part->inflight || part->aheadFailed; part->claimed = YES;
    } else
    @synchronized ([NVMTLCommandBuffer class]) { ahead = part->inflight || part->aheadFailed; part->claimed = YES; }
    if (part->waitEvent && !ahead && !part->waitFolded) {
        if (![part->waitEvent respondsToSelector:@selector(signaledValue)]) {
            [self nvmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot wait on an event from another backend"}]];
            return YES;
        }
        uint64_t have = ((uint64_t(*)(id,SEL))objc_msgSend)(part->waitEvent,@selector(signaledValue));
        if (have < part->value) {
            if (v2) { part->claimed = NO; if (v2took == NVMTL_PART_WORKER) nvmtl_part_unclaim(&part->v2state); return NO; }
            @synchronized ([NVMTLCommandBuffer class]) { part->claimed = NO; } return NO;
        }
    }
    if (!ahead && part->commands.cb && !part->encoders && !part->resources.count && !part->surfaces.count && !part->scratch.count && nvmtl_skip_empty_on()) {
        [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        nvmtl_vk_cmd_abandon(&part->commands);
        const unsigned long long k = atomic_fetch_add(&gSkipEmpty, 1);
        if (!k || ((k + 1) & 0xFFF) == 0) nvlog("commit: %llu empty submission parts NOT submitted (no encoder, no resources) - each WAS a full submit + wait", k + 1);
    }
    if (part->commands.cb || ahead) {
        if (!part->isyncPublish || part->inflight || part->aheadFailed) [self nvmtlMarkScheduled];
        if (!_legacyKernelStart) _legacyKernelStart = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
        int result;
        const uint64_t scf0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (part->inflight) result = nvmtl_gsub_finish(part);
        else if (part->aheadFailed) result = part->aheadFailed;
        else {
            nvmtl_prepare_submission_copies(part);
            for (NVMTLBuffer *buffer in part->copyInputs) nvmtl_buffer_sync_in(buffer);
            result = nvmtl_vk_submit_wait(&part->commands);
            if (part->isyncPublish) [self nvmtlMarkScheduled];   // normally done at submission (on_submit)
        }
        if (result <= -2) {   // lost (-2 backend down, -3/-4/-5 GPU recovery): the GPU may have held them
            if (!nvmtl_quarantined_parts) nvmtl_quarantined_parts = [NSMutableArray new];
            [nvmtl_quarantined_parts addObject:part];
        }
        const uint64_t scf1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (sc_on_worker && nvmtl_sc_on()) { atomic_fetch_add(&sc_w_fence_ns, scf1 - scf0); atomic_fetch_add(&sc_w_parts, 1); }
        if (result == 0) [self nvmtlCollectDeviceTiming:&part->commands];
        BOOL published = result == 0;
        if (published) {
            // the GPU has finished this part: its surfaces' kernel marks are released (nvmtl_ws_part_landed) before the
            // surface work below, which takes the surface's own lock
            NSMutableArray *settled = [NSMutableArray new];
            NSArray *landed = nvmtl_ws_part_landed(self, _nextPart, settled);
            // Without implicit-sync publication, non-WindowServer parts publish at
            // completion. All of their folded planes have landed by this point.
            if (!part->foldPublished && !nvmtl_is_ws()) {
                nvmtl_publish_folded_part(part);
                part->foldPublished = YES;
            }
            for (NVMTLTexture *texture in landed) {
                nvmtl_surface_out_order(part, texture);
                if (nvmtl_part_vpub(part, texture)) { if (!part->vpubDone) (void)[texture nvmtlSurfaceOutDirtied:YES]; }   // B580 VRAM-shared
                else if (nvmtl_part_folded(part, texture)) { if (!part->foldPublished) [texture nvmtlSurfaceOutFoldedP4:nvmtl_part_folded_p4(part, texture)]; }   // the part's own commands copied it
                else if (![texture nvmtlSurfaceOut]) published = NO;
            }
            nvmtl_fold_release(part);
            if (!nvmtl_ws_publish_settled(settled, self)) published = NO;
            if (sc_on_worker && nvmtl_sc_on()) atomic_fetch_add(&sc_w_pub_ns, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - scf1);
            for (NVMTLBuffer *buffer in nvmtl_submission_copies(part->resources, YES)) nvmtl_buffer_sync_out(buffer);
        }
        if (!published) {
            // nm29 (nullmoth f9fe87b) -3: the command buffer was made after the GPU was lost. Metal reports a removed GPU
            // as DeviceRemoved (11).
            if (result == -3) {
                [self nvmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:11
                    userInfo:@{NSLocalizedDescriptionKey:@"The GPU was lost; this command buffer was not executed"}]];
                return YES;
            }
            [self nvmtlFinishExecution:nvmtl_part_error(result, result ? @"GPU event segment submission failed" : @"Could not publish event segment writes")];
            return YES;
        }
    }
    if (part->signalEvent) {
        if (![part->signalEvent respondsToSelector:@selector(setSignaledValue:)]) {
            [self nvmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot signal an event from another backend"}]];
            return YES;
        }
        ((void(*)(id,SEL,uint64_t))objc_msgSend)(part->signalEvent,@selector(setSignaledValue:),part->value);
    }
    for (NSArray *s in part->signals) {
        id event = s[0];
        if (![event respondsToSelector:@selector(setSignaledValue:)]) {
            [self nvmtlFinishExecution:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot signal an event from another backend"}]];
            return YES;
        }
        ((void(*)(id,SEL,uint64_t))objc_msgSend)(event, @selector(setSignaledValue:), [s[1] unsignedLongLongValue]);
    }
    if (v2) { _parts[_nextPart] = [NSNull null]; __atomic_store_n(&_nextPart, _nextPart + 1, __ATOMIC_RELEASE); }   // the pass reads it
    else _parts[_nextPart++] = [NSNull null];
    if (_nextPart == _parts.count) { nvtrace("COMPLETE cb %p t=%llu", (__bridge void *)self, (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000));
        [self nvmtlFinishExecution:nil]; }
    return YES;
}
// NVMTL_LOCK_V2 commit: the same steps as below, with the class lock's two sections replaced by short state-lock ones
// (reserve + committed; marking off + pending) and nothing submitted under any lock. Until _commitMarking is cleared
// (release, after the seal and the kernel marks) the pass refuses this command buffer, as the class lock made it wait.
static void nvmtl_pump_v2(void);
- (void)nvmtlSubmitWithEventsV2 { @autoreleasepool {
    NVMTLQueueReservation *mine = nil;
    if (!_reservation) { mine = [NVMTLQueueReservation new]; mine->buffer = self; }   // made outside the lock
    nvmtl_plock_lock(&gV2State);
    if (_committed) { os_unfair_lock_unlock(&gV2State); return; }
    if (!_reservation && mine) {
        if (!_queue->_reservations) _queue->_reservations = [NSMutableArray new];
        [_queue->_reservations addObject:mine]; _reservation = mine;
    }
    __atomic_store_n(&_commitMarking, YES, __ATOMIC_RELAXED);
    __atomic_store_n(&_committed, YES, __ATOMIC_RELEASE);
    os_unfair_lock_unlock(&gV2State);
    if (!__atomic_load_n(&_rep, __ATOMIC_ACQUIRE)) __atomic_store_n(&_rep, nvmtl_ep_enter(), __ATOMIC_RELEASE);
    [_completionCondition lock]; _cbstatus = MTLCommandBufferStatusCommitted; [_completionCondition unlock];
    [self nvmtlSealPartWithSignal:nil value:0 restart:NO];
    if (_scheduleAfterPublish) for (NVMTLSubmissionPart *p in _parts)   // as below
        if ([p isKindOfClass:[NVMTLSubmissionPart class]] && p->waitEvent) {
            _scheduleAfterPublish = NO;
            static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 4)
                nvlog("surface writes: a command buffer that waits for an event is scheduled at submission (its surfaces are published at completion)");
            break;
        }
    nvmtl_ws_mark_pending(self);
    nvmtl_ws_note_captures(self);
    nvmtl_surface_register_writes(self);   // before the pass may submit it (marking still on)
    nvmtl_plock_lock(&gV2State);
    if (!nvmtl_pending_buffers) nvmtl_pending_buffers = [NSMutableArray new];
    [nvmtl_pending_buffers addObject:self]; _v2pending = YES;
    __atomic_store_n(&_commitMarking, NO, __ATOMIC_RELEASE);
    os_unfair_lock_unlock(&gV2State);
    nvmtl_pump_v2();
} }
- (void)nvmtlSubmitWithEvents {
    if (nvmtl_lock_v2()) { [self nvmtlSubmitWithEventsV2]; return; }
    @autoreleasepool {   // the committing thread may have no pool of its own (see nvmtl_pump_buffers)
    @synchronized ([NVMTLCommandBuffer class]) {
        if (_committed) return;
        [self nvmtlReserveQueue];
        _committed = YES; _commitMarking = YES;
        if (!__atomic_load_n(&_rep, __ATOMIC_ACQUIRE)) __atomic_store_n(&_rep, nvmtl_ep_enter(), __ATOMIC_RELEASE);
        [_completionCondition lock]; _cbstatus = MTLCommandBufferStatusCommitted; [_completionCondition unlock];
        [self nvmtlSealPartWithSignal:nil value:0 restart:NO];
        // scheduled-after-publish (nvmtlSealPartWithSignal) is dropped for a command buffer that waits for an event: the
        // signal may come from a command buffer or the CPU after waitUntilScheduled returns. Nothing is submitted yet.
        if (_scheduleAfterPublish) for (NVMTLSubmissionPart *p in _parts)
            if ([p isKindOfClass:[NVMTLSubmissionPart class]] && p->waitEvent) {
                _scheduleAfterPublish = NO;
                static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 4)
                    nvlog("surface writes: a command buffer that waits for an event is scheduled at submission (its surfaces are published at completion)");
                break;
            }
    }
    // the commit's kernel marks can wait for the accelerator lock, so they go in outside the class lock (see
    // nvmtl_ws_mark_pending). This command buffer is committed and holds its queue reservation, but nothing submits it
    // (_commitMarking stops the submit-ahead pass) or completes it (it is not pending yet) before its marks are in.
    nvmtl_ws_mark_pending(self);
    nvmtl_ws_note_captures(self);
    @synchronized ([NVMTLCommandBuffer class]) {
        _commitMarking = NO;
        nvmtl_surface_register_writes(self);
        if (!nvmtl_pending_buffers) nvmtl_pending_buffers = [NSMutableArray new];
        [nvmtl_pending_buffers addObject:self];
        nvmtl_pump_buffers();
    }
    }
}
@end

static void nvmtl_pump_buffers_sync(void) {
    @synchronized ([NVMTLCommandBuffer class]) {
        if (nvmtl_pumping) return;
        nvmtl_pumping = YES;
        BOOL progress;
        do {
            progress = NO;
            for (NVMTLCommandBuffer *cb in [nvmtl_pending_buffers copy]) {
                NSMutableArray *reservations = cb->_queue->_reservations;
                while (reservations.count && !((NVMTLQueueReservation *)reservations.firstObject)->buffer)
                    [reservations removeObjectAtIndex:0];
                if (reservations.firstObject != cb->_reservation) continue;
                if ([cb nvmtlAdvancePart]) progress = YES;
                if (cb->_executionDone) {
                    [cb nvmtlReleaseReservation];
                    [nvmtl_pending_buffers removeObjectIdenticalTo:cb];
                    [cb nvmtlFinishCallbacks];
                }
            }
        } while (progress);
        nvmtl_pumping = NO;
    }
}

static uint64_t nvmtl_pump_generation;
static BOOL nvmtl_commit_ahead_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_COMMIT_AHEAD"); on = !(e && e[0] == '0');
        nvlog("commit: submit-ahead %s (NVMTL_COMMIT_AHEAD=%s)", on ? "also runs at commit while the worker waits" : "runs on the worker only", e ? e : "unset"); }
    return on;
}
static void nvmtl_pump_buffers(void) {
    if (nvmtl_lock_v2()) { nvmtl_pump_v2(); return; }
    static dispatch_once_t once; static dispatch_queue_t worker;
    const BOOL async = nvmtl_async_commit_on();
    if (async) dispatch_once(&once, ^{ worker = dispatch_queue_create("nvmtl.execution", DISPATCH_QUEUE_SERIAL); });
    if (!async) { nvmtl_vk_clock_refresh(); nvmtl_pump_buffers_sync(); return; }
    @synchronized ([NVMTLCommandBuffer class]) {
        if (!nvmtl_pending_buffers.count) return;
        ++nvmtl_pump_generation;
        if (nvmtl_pumping) {
            // the worker may be blocked on an earlier part; without this the commit is scheduled only after that wait
            // in a pool of its own: the committing thread may have none (Apple's OpenCL enqueues from a plain thread), and
            // what the pass autoreleases - arrays of the part's resources - would keep those buffers alive for good
            if (nvmtl_commit_ahead_on() && nvmtl_gsub_ahead_on()) { @autoreleasepool { nvmtl_gsub_ahead_pass(); } }
            return;
        }
        nvmtl_pumping = YES;
        // the worker is idle: submit what can go ahead here, on the committing thread (a commitAndWaitUntilSubmitted then
        // does not wait for the worker to wake: 0.13 ms each, 2824 in 5 s of Minecraft loading chunks). In a pool of its
        // own: fix11 ran the pass on Apple's OpenCL enqueue thread, which has none, and the arrays it autoreleased kept
        // every buffer of every enqueue alive (Geekbench: 13.5 GB of VkDeviceMemory for 1466 live buffers, then allocation
        // failures; clchurn: CL_OUT_OF_RESOURCES at iteration 376). NVMTL_COMMIT_AHEAD_IDLE=0: the worker submits.
        { static int idle = -1; if (idle < 0) { const char *e = getenv("NVMTL_COMMIT_AHEAD_IDLE"); idle = !(e && e[0] == '0');
              nvlog("commit: an idle worker's first submit %s (NVMTL_COMMIT_AHEAD_IDLE=%s)", idle ? "is made at commit" : "waits for the worker", e ? e : "unset"); }
          if (idle && nvmtl_commit_ahead_on() && nvmtl_gsub_ahead_on()) { @autoreleasepool { nvmtl_gsub_ahead_pass(); } } }
        dispatch_async(worker, ^{
            for (;;) { @autoreleasepool {
                nvmtl_vk_clock_refresh();
                NSArray *batch; uint64_t generation;
                @synchronized ([NVMTLCommandBuffer class]) {
                    if (nvmtl_gsub_ahead_on()) nvmtl_gsub_ahead_pass();
                    batch = [nvmtl_pending_buffers copy]; generation = nvmtl_pump_generation;
                }
                BOOL progress = NO;
                for (NVMTLCommandBuffer *cb in batch) {
                    BOOL eligible;
                    @synchronized ([NVMTLCommandBuffer class]) {
                        NSMutableArray *reservations = cb->_queue->_reservations;
                        while (reservations.count && !((NVMTLQueueReservation *)reservations.firstObject)->buffer)
                            [reservations removeObjectAtIndex:0];
                        eligible = reservations.firstObject == cb->_reservation;
                    }
                    if (!eligible) continue;
                    sc_on_worker = 1;
                    if ([cb nvmtlAdvancePart]) progress = YES;
                    sc_on_worker = 0;
                    @synchronized ([NVMTLCommandBuffer class]) {
                        if (cb->_executionDone) {
                            [cb nvmtlReleaseReservation];
                            [nvmtl_pending_buffers removeObjectIdenticalTo:cb];
                            [cb nvmtlFinishCallbacks];
                        }
                    }
                }
                @synchronized ([NVMTLCommandBuffer class]) {
                    if (!progress && generation == nvmtl_pump_generation) {
                        nvmtl_pumping = NO; return;
                    }
                }
            } }
        });
    }
}

/* NVMTL_LOCK_V2 execution worker. Per round: the submit-ahead pass (or a request to whoever runs it), then, under the
   state lock, the head command buffer of each queue that has a pending one (only a head can be advanced: the queue's
   first reservation); each is advanced outside any lock, and one that finished is let go under the lock (reservation,
   pending list) before its callbacks are dispatched, on this one thread, so a queue's completions keep their order.
   The worker stops when a round made no progress and nothing was committed or signalled meanwhile (the generation),
   as the class-lock worker does. Command buffers read through weak reservations are autoreleased into the round's
   pool: their last release happens after the lock, where their dealloc may take it. */
static void nvmtl_v2_worker(void) {
    for (;;) { @autoreleasepool {
        nvmtl_vk_clock_refresh();
        if (nvmtl_gsub_ahead_on()) nvmtl_v2_ahead();
        // every queue's head each round (review 9 Nov: a fixed 32 could leave a 33rd queue's ready command buffer
        // until the next commit while the first 32 wait on events); arrays on the stack up to 32 queues, else from
        // the heap, sized before the lock (a list that grew meanwhile counts as progress: the next round is larger)
        enum { MAXH = 32 };
        __unsafe_unretained NVMTLCommandBuffer *hbuf[MAXH]; __unsafe_unretained NVMTLCommandQueue *sbuf[MAXH];
        nvmtl_plock_lock(&gV2State);
        const NSUInteger want = nvmtl_pending_buffers.count;
        os_unfair_lock_unlock(&gV2State);
        const NSUInteger cap = want > MAXH ? want : MAXH;
        __unsafe_unretained NVMTLCommandBuffer **heads = hbuf; __unsafe_unretained NVMTLCommandQueue **seen = sbuf;
        if (cap > MAXH) {
            heads = (__unsafe_unretained NVMTLCommandBuffer **)calloc(cap, sizeof(void *));
            seen = (__unsafe_unretained NVMTLCommandQueue **)calloc(cap, sizeof(void *));
        }
        unsigned nh = 0, ns = 0; BOOL truncated = NO;
        uint64_t generation;
        nvmtl_plock_lock(&gV2State);
        generation = nvmtl_pump_generation;
        for (__unsafe_unretained NVMTLCommandBuffer *cb in nvmtl_pending_buffers) {
            __unsafe_unretained NVMTLCommandQueue *q = cb->_queue;
            BOOL dup = NO;
            for (unsigned k = 0; k < ns; k++) if (seen[k] == q) { dup = YES; break; }
            if (dup) continue;
            if (ns == cap) { truncated = YES; break; }   // the list grew since it was sized: the rest next round
            seen[ns++] = q;
            NSMutableArray *res = q->_reservations;
            __autoreleasing NVMTLCommandBuffer *head = nil;
            while (res.count) {   // reservations whose command buffer is gone (released uncommitted)
                __unsafe_unretained NVMTLQueueReservation *first = res.firstObject;
                head = first->buffer;
                if (head) break;
                [res removeObjectAtIndex:0];
            }
            if (head && head->_v2pending) heads[nh++] = head;
        }
        os_unfair_lock_unlock(&gV2State);
        BOOL progress = truncated;
        for (unsigned i = 0; i < nh; i++) {
            NVMTLCommandBuffer *cb = heads[i];
            sc_on_worker = 1;
            if ([cb nvmtlAdvancePart]) progress = YES;
            sc_on_worker = 0;
            if (!cb->_executionDone) continue;
            nvmtl_plock_lock(&gV2State);
            id r = cb->_reservation;
            if (r) {
                NSMutableArray *res = cb->_queue->_reservations;
                if (res.firstObject == r) [res removeObjectAtIndex:0];
                else [res removeObjectIdenticalTo:r];
            }
            const NSUInteger at = [nvmtl_pending_buffers indexOfObjectIdenticalTo:cb];
            if (at != NSNotFound) [nvmtl_pending_buffers removeObjectAtIndex:at];   // heads[] keeps it (autoreleased)
            cb->_v2pending = NO;
            os_unfair_lock_unlock(&gV2State);
            cb->_reservation = nil;   // the reservation's last reference, outside the lock (the worker alone writes it now)
            r = nil;
            [cb nvmtlFinishCallbacks];
        }
        if (heads != hbuf) { free(heads); free(seen); }
        if (progress) continue;
        nvmtl_plock_lock(&gV2State);
        const BOOL quit = generation == nvmtl_pump_generation;
        if (quit) nvmtl_pumping = NO;
        os_unfair_lock_unlock(&gV2State);
        if (quit) return;
    } }
}
static BOOL nvmtl_commit_ahead_idle_on(void) {
    static int idle = -1;
    if (idle < 0) { const char *e = getenv("NVMTL_COMMIT_AHEAD_IDLE"); idle = !(e && e[0] == '0');
        nvlog("commit: an idle worker's first submit %s (NVMTL_COMMIT_AHEAD_IDLE=%s)", idle ? "is made at commit" : "waits for the worker", e ? e : "unset"); }
    return idle;
}
/* NVMTL_LOCK_V2 pump: note the new work (generation) under the state lock, run the submit-ahead pass on this thread
   (in a pool of its own: the committing thread may have none, see nvmtl_pump_buffers), and start the worker if it is
   idle. The pass never blocks here: if another thread is running it, that thread runs it again for us. */
static void nvmtl_pump_v2(void) {
    static dispatch_once_t once; static dispatch_queue_t worker;
    dispatch_once(&once, ^{ worker = dispatch_queue_create("nvmtl.execution", DISPATCH_QUEUE_SERIAL); });
    nvmtl_plock_lock(&gV2State);
    if (!nvmtl_pending_buffers.count) { os_unfair_lock_unlock(&gV2State); return; }
    ++nvmtl_pump_generation;
    const BOOL start = !nvmtl_pumping;
    if (start) nvmtl_pumping = YES;
    os_unfair_lock_unlock(&gV2State);
    if (nvmtl_commit_ahead_on() && nvmtl_gsub_ahead_on() && (!start || nvmtl_commit_ahead_idle_on())) { @autoreleasepool { nvmtl_v2_ahead(); } }
    if (start) dispatch_async(worker, ^{ nvmtl_v2_worker(); });
}

static uint64_t nvmtl_event_read(NSCondition *condition, uint64_t *storage) {
    [condition lock]; uint64_t value = *storage; [condition unlock]; return value;
}
static void nvmtl_event_signal(id event, NSCondition *condition, uint64_t *storage,
                              NSMutableArray *notifications, uint64_t value) {
    NSMutableArray *ready = [NSMutableArray new];
    [condition lock];
    if (value > *storage) *storage = value;
    value = *storage;
    for (NSArray *notification in notifications)
        if ([notification[1] unsignedLongLongValue] <= value) [ready addObject:notification];
    [notifications removeObjectsInArray:ready];
    [condition broadcast]; [condition unlock];
    for (NSArray *notification in ready) {
        MTLSharedEventListener *listener = notification[0];
        MTLSharedEventNotificationBlock block = notification[2];
        dispatch_async(listener.dispatchQueue, ^{ block(event, value); });
    }
    nvmtl_pump_buffers();
}
static void nvmtl_event_notify(id event, NSCondition *condition, uint64_t *storage,
                              NSMutableArray *notifications, MTLSharedEventListener *listener,
                              uint64_t wanted, MTLSharedEventNotificationBlock block) {
    if (!listener || !block) return;
    [condition lock]; uint64_t value = *storage;
    if (value < wanted) [notifications addObject:@[listener, @(wanted), [block copy]]];
    [condition unlock];
    if (value >= wanted) dispatch_async(listener.dispatchQueue, ^{ block(event, value); });
}
static BOOL nvmtl_event_wait(NSCondition *condition, uint64_t *storage, uint64_t wanted, uint64_t milliseconds) {
    NSDate *deadline = milliseconds == UINT64_MAX ? nil : [NSDate dateWithTimeIntervalSinceNow:(double)milliseconds / 1000.0];
    [condition lock];
    while (*storage < wanted) {
        if (!milliseconds) break;
        if (deadline) { if (![condition waitUntilDate:deadline]) break; }
        else [condition wait];
    }
    BOOL done = *storage >= wanted; [condition unlock]; return done;
}
