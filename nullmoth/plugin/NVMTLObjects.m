/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#import <QuartzCore/QuartzCore.h>
#include "nvmtl_format_twin.h"
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

void nvlog(const char *fmt, ...);

#import "NVMTLObjects.h"
#import <objc/runtime.h>
#import <objc/message.h>
/* perf2: the trace check every bind and draw makes (64 call sites) is an inline load and compare while tracing is off and
   the 250 ms poll is not due (WAS: a call that read the clock and an env cache each time: nvtrace_floats alone was 19% of
   setVertexBuffer in the gl_bind profile, 2026-10-30). The slow path keeps the old answer and refreshes these two. */
static int g_trace_hot = 1;            // 1: tracing may be on (env, request flag): ask the slow path every time
static uint64_t g_trace_recheck;      // the slow path's next poll (nvmtl_poll_ns)
static int nvmtl_trace_on_slow(void);
static inline int nvmtl_trace_on(void) {
    if (__builtin_expect(!g_trace_hot && nvmtl_poll_ns() < g_trace_recheck, 1)) return 0;
    return nvmtl_trace_on_slow();
}
static void nvtrace_emit(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
// Guard the call site: converting NSString arguments on every draw is expensive
// even when the trace writer immediately returns without emitting anything.
#define nvtrace(...) do { if (nvmtl_trace_on()) nvtrace_emit(__VA_ARGS__); } while (0)
// A private lock, not @synchronized on the object: the object is the app's (a command buffer, an encoder), and an app that
// holds @synchronized on it while it waits for the scheduled handler deadlocked against the Metal HUD, which asks for the ID
// from that handler (Unity games with MetalForceHudEnabled: Soulstone Survivors hung on a black screen at start).
static unsigned long long nvmtl_global_trace_id(id o) {
    static char key; static unsigned long long next = 1; static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&lock);
    NSNumber *n = objc_getAssociatedObject(o, &key);
    if (!n) { n = @(__atomic_fetch_add(&next, 1, __ATOMIC_RELAXED)); objc_setAssociatedObject(o, &key, n, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    const unsigned long long id = n.unsignedLongLongValue;
    os_unfair_lock_unlock(&lock);
    return id;
}
@protocol NVMTLFunctionConstantInit
- (instancetype)initWithName:(NSString *)name type:(NSUInteger)type index:(NSUInteger)index required:(BOOL)required;
@end
static MTLDataType nvmtl_air_fc_type(NSString *t) {
    static NSDictionary *base; static dispatch_once_t once;
    dispatch_once(&once, ^{ base = @{ @"float": @(MTLDataTypeFloat), @"half": @(MTLDataTypeHalf), @"bfloat": @(MTLDataTypeBFloat),
        @"int": @(MTLDataTypeInt), @"uint": @(MTLDataTypeUInt), @"short": @(MTLDataTypeShort), @"ushort": @(MTLDataTypeUShort),
        @"char": @(MTLDataTypeChar), @"uchar": @(MTLDataTypeUChar), @"bool": @(MTLDataTypeBool),
        @"long": @(MTLDataTypeLong), @"ulong": @(MTLDataTypeULong) }; });
    NSNumber *b = base[t];
    if (b) return (MTLDataType)b.unsignedIntegerValue;
    unichar last = t.length > 1 ? [t characterAtIndex:t.length - 1] : 0;
    if (last >= '2' && last <= '4' && (b = base[[t substringToIndex:t.length - 1]]))
        return (MTLDataType)(b.unsignedIntegerValue + (NSUInteger)(last - '1'));
    return MTLDataTypeNone;
}
#include <CommonCrypto/CommonDigest.h>

#define NVMTL_SET_VERTEX   0u
#define NVMTL_SET_FRAGMENT 1u

@class NVMTLBuffer;
static void nvmtl_buffer_sync_in(NVMTLBuffer *b);
@class NVMTLCommandBuffer;
static void nvmtl_buffer_sync_out(NVMTLBuffer *b);
static void nvbt(const char *tag, int *count, int max) {
    if (*count >= max) return; (*count)++;
    extern const char *nvmtl_log_path(void);
    FILE *f = fopen(nvmtl_log_path(), "a"); if (!f) return;
    NSArray *syms = [NSThread callStackSymbols];
    fprintf(f, "pid %d NVMTL: BT %s #%d (%lu frames)\n", getpid(), tag, *count, (unsigned long)syms.count);
    for (NSUInteger i = 1; i < syms.count && i < 14; i++) fprintf(f, "pid %d NVMTL:    %s\n", getpid(), [syms[i] UTF8String]);
    fclose(f);
}
static int g_bt_status, g_bt_enqueue, g_bt_handler, g_bt_release, g_bt_commit;

#pragma mark - buffer
static NSArray *nvmtl_vertex_attributes_for(NVMTLFunction *fn);
#include <os/lock.h>
static os_unfair_lock gManagedLock = OS_UNFAIR_LOCK_INIT;
static NSHashTable *gManagedDirty;
static _Atomic int gManagedAny;
static __thread int gManagedInFlush;
static os_unfair_lock gPurgeLock = OS_UNFAIR_LOCK_INIT;
static NSHashTable *gPurgeVol;
static _Atomic uint64_t gPurgeAsks;
extern void (*nvmtl_purge_report_hook)(const char *why);
static void nvmtl_purge_report(const char *why);

static _Atomic uint64_t gRes2Tick;
static void nvmtl_res2_note_use(NVMTLTexture *t);
static uint64_t nvmtl_res2_evict(uint64_t want);
extern uint64_t (*nvmtl_res2_evict_hook)(uint64_t want);
#include "nvmtl_env_knobs.h"
__attribute__((constructor)) static void nvmtl_conf_knobs(void)
{
#if NVMTL_RELEASE
    // Inherited driver knobs are forgotten unless this process set NVMTL_PROCESS_ENV=1 (nvmtl_env_knobs.h has the rule).
    { extern char **environ; nvmtl_release_forget_env(environ); }
#else
    // The packages ship this diagnostic build, so the rule for a privileged process must hold here too (2.7.2).
    if (nvmtl_env_privileged()) { extern char **environ; nvmtl_release_forget_env(environ); }
#endif
    unsigned forgot = 0;
    { const char *rec = getenv("NVMTL_CONF_SET");
      if (rec && *rec) { char *dup = strdup(rec), *save = NULL;
          for (char *kv = dup ? strtok_r(dup, ";", &save) : NULL; kv; kv = strtok_r(NULL, ";", &save)) {
              char *eq = strchr(kv, '='); if (!eq) continue; *eq = 0;
              const char *now = getenv(kv);
              if (now && !strcmp(now, eq + 1)) { unsetenv(kv); forgot++; }
          }
          free(dup); }
      unsetenv("NVMTL_CONF_SET"); }
    char recbuf[2048]; size_t recn = 0; recbuf[0] = 0;
    FILE *f = fopen("/Library/GPUBundles/nvmtl/nvrm610.conf", "r");
    if (!f) { if (forgot) nvlog("conf: forgot %u inherited knob(s); no conf file", forgot); return; }
    char line[512];
    unsigned took = 0;
    // The last line for a key wins: the installer keeps the user's own block ("my settings, kept on update") at the end
    // of the file, and the first match used to win, so a shipped value of the same key above it silently beat the
    // user's (#399). Keys set by an earlier line of this file may be replaced; the real environment still wins.
    char fromFile[64][48]; unsigned nFromFile = 0;
    while (fgets(line, sizeof line, f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || !*p) continue;
        char *eq = strchr(p, '=');
        if (!eq) continue;
        *eq = 0;
        char *key = p, *val = eq + 1;
        char *end = key + strlen(key);
        while (end > key && (end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        end = val + strlen(val);
        while (end > val && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ' || end[-1] == '\t')) *--end = 0;
        if (strncmp(key, "NVMTL_", 6) && strncmp(key, "NVK_", 4) && strncmp(key, "NVRM_", 5)) continue;
        bool ours = false;
        for (unsigned i = 0; i < nFromFile; i++) if (!strcmp(fromFile[i], key)) { ours = true; break; }
        if (getenv(key) && !ours) continue;
        if (setenv(key, val, 1) == 0) { if (ours) { nvlog("conf: %s=%s (a later line of the file replaces an earlier one)", key, val); continue; }
            took++;
            if (nFromFile < 64 && strlen(key) < sizeof fromFile[0]) strcpy(fromFile[nFromFile++], key); }
    }
    fclose(f);
    // NVMTL_CONF_SET records the values this file set, as they stand after every line, so a child process forgets
    // exactly these (an override recorded at its first value would no longer match and be kept as if user-set)
    for (unsigned i = 0; i < nFromFile; i++) {
        const char *v = getenv(fromFile[i]); if (!v) continue;
        int k = snprintf(recbuf + recn, sizeof recbuf - recn, "%s%s=%s", recn ? ";" : "", fromFile[i], v);
        if (k > 0 && (size_t)k < sizeof recbuf - recn) recn += (size_t)k; else { recbuf[recn] = 0; break; }
    }
    if (recn) setenv("NVMTL_CONF_SET", recbuf, 1);
    if (took || forgot) nvlog("conf: %u knob(s) read from /Library/GPUBundles/nvmtl/nvrm610.conf, %u inherited conf knob(s) forgotten first (the environment still wins)", took, forgot);
}
static int nvmtl_dontcare_clears(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_DONTCARE_CLEARS"); on = (e && *e && *e != '0') ? 1 : 0; }
    return on;
}
static inline int nvmtl_load_keeps(MTLLoadAction a) {
    return a == MTLLoadActionLoad || (a == MTLLoadActionDontCare && !nvmtl_dontcare_clears());
}
static void nvmtl_res3_note_use(NVMTLBuffer *b);
static uint64_t nvmtl_ep_now(void);
static inline void nvmtl_res3_wait_move(int *rs) { while (__atomic_load_n(rs, __ATOMIC_ACQUIRE) == 2) sched_yield(); }
static void nvmtl_res2_kick(int scan);
static _Atomic int gRes2Scan;
static Class gNVMTLTexClass;
/* glcpu4 NVMTL_RESREF_BORROW (glcpu1's change, now switchable): the residency count of a resource the caller already holds
   (a set member, an owned old set being emptied, the object being added or removed; a texture view strongly owns its
   parent) is updated through borrowed references. prod7's strong locals made an objc_retain/objc_release pair per new
   member, and isKindOfClass: walked the class chain for every buffer: 20 + 13 + 16 of 3595 render-thread samples in
   nvmtl_resref (prod7 Minecraft profile). The exact buffer class is tested first, textures (and their subclasses) after. */
NVMTL_G4_SWITCH(nvmtl_resref_borrow_on, "NVMTL_RESREF_BORROW", "resource-set residency counts through borrowed references (no ARC pair per new member)")
static void nvmtl_resref_prod7(id o, int d);
static inline void nvmtl_resref(__unsafe_unretained id o, int d) {
    static Class BC, TC; static dispatch_once_t once;
    dispatch_once(&once, ^{ BC = [NVMTLBuffer class]; TC = [NVMTLTexture class]; });
    // a command buffer took a buffer (any binding path ends here): synchronizeResource skips a buffer none took since its last one
    if (d > 0 && object_getClass(o) == BC) { __unsafe_unretained NVMTLBuffer *ub = o; __atomic_add_fetch(&ub->_gpuUse, 1, __ATOMIC_RELAXED); }
    if (!nvmtl_resref_borrow_on()) { nvmtl_resref_prod7(o, d); return; }
    if (object_getClass(o) == BC) {
        __unsafe_unretained NVMTLBuffer *b = o;
        const int v = __atomic_add_fetch(&b->_resRefs, d, __ATOMIC_ACQ_REL);
        if (d > 0) { __atomic_store_n(&b->_useEp, nvmtl_ep_now(), __ATOMIC_RELEASE); nvmtl_res3_wait_move(&b->_rstate); nvmtl_res3_note_use(b); }
        else if (v == 0 && __atomic_load_n(&b->_wantOn, __ATOMIC_ACQUIRE)) nvmtl_res2_kick(1);
        return;
    }
    if (![o isKindOfClass:TC]) return;
    __unsafe_unretained NVMTLTexture *t = o; if (t->_parent) t = t->_parent;
    const int v = __atomic_add_fetch(&t->_resRefs, d, __ATOMIC_ACQ_REL);
    if (d > 0) { __atomic_store_n(&t->_useEp, nvmtl_ep_now(), __ATOMIC_RELEASE); nvmtl_res3_wait_move(&t->_rstate);
                 nvmtl_res2_note_use(t); }
    else if (v == 0 && __atomic_load_n(&t->_wantOn, __ATOMIC_ACQUIRE)) nvmtl_res2_kick(1);
}
static void nvmtl_resref_prod7(id o, int d) {
    if (!gNVMTLTexClass) gNVMTLTexClass = [NVMTLTexture class];
    if (![o isKindOfClass:gNVMTLTexClass]) {
        static Class BC; if (!BC) BC = [NVMTLBuffer class];
        if (object_getClass(o) != BC) return;
        NVMTLBuffer *b = o;
        const int v = __atomic_add_fetch(&b->_resRefs, d, __ATOMIC_ACQ_REL);
        if (d > 0) { __atomic_store_n(&b->_useEp, nvmtl_ep_now(), __ATOMIC_RELEASE); nvmtl_res3_wait_move(&b->_rstate); nvmtl_res3_note_use(b); }
        else if (v == 0 && __atomic_load_n(&b->_wantOn, __ATOMIC_ACQUIRE)) nvmtl_res2_kick(1);
        return;
    }
    NVMTLTexture *t = o; if (t->_parent) t = t->_parent;
    const int v = __atomic_add_fetch(&t->_resRefs, d, __ATOMIC_ACQ_REL);
    if (d > 0) { __atomic_store_n(&t->_useEp, nvmtl_ep_now(), __ATOMIC_RELEASE); nvmtl_res3_wait_move(&t->_rstate);
                 nvmtl_res2_note_use(t); }
    else if (v == 0 && __atomic_load_n(&t->_wantOn, __ATOMIC_ACQUIRE)) nvmtl_res2_kick(1);
}
// 0aacf15: a Metal object is equal only to itself, so the set may hash and compare pointers: NSMutableSet sends -hash and
// -isEqual: to every object it probes, two message sends per lookup (upstream: the top plugin leaf on a game's
// render-encoding threads). CFRetain/CFRelease keep the set's ownership unchanged.
// B580: OFF unless NVMTL_RESSET_IDENTITY=1. The fork's sets already skip most lookups (epoch tags, glcpu4), its
// contract test (tools/b580/resource-set-check.m) requires -isEqual: semantics (4 failures with identity on), and the
// same test measured no gain (CPU set add p50 574 vs 504 ns/op, x86_64 ASan under Rosetta).
static NSMutableSet *nvmtl_identity_set(NSUInteger cap) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_RESSET_IDENTITY"); on = e && e[0] == '1'; }
    if (!on) return cap ? [[NSMutableSet alloc] initWithCapacity:cap] : [NSMutableSet new];
    CFSetCallBacks cb = kCFTypeSetCallBacks; cb.equal = NULL; cb.hash = NULL;
    return (__bridge_transfer NSMutableSet *)CFSetCreateMutable(kCFAllocatorDefault, (CFIndex)cap, &cb);
}
/* the set epochs: unique in the process, never 0 (a resource's tag starts at 0) */
static _Atomic uint64_t gResSetEpoch;
static uint64_t nvmtl_resset_next_epoch(void) { return atomic_fetch_add(&gResSetEpoch, 1) + 1; }
/* glperf1: a new set starts at the size the last emptied set had. Apple GL binds a new vertex buffer for almost every draw
   (Minecraft: ~1800 chunk buffers per frame in ~8 command buffers), and a set grown from empty rehashed again and again:
   CFSet's __rehashs was ~1.6% of the game's render thread (sample, 2026-10-31). Relaxed: only a size hint. */
static _Atomic unsigned gResSetHint;
static NSUInteger nvmtl_resset_hint(void) { return atomic_load_explicit(&gResSetHint, memory_order_relaxed); }
static void nvmtl_resset_note(NSUInteger n) { if (n > 65536) n = 65536; atomic_store_explicit(&gResSetHint, (unsigned)n, memory_order_relaxed); }
/* A command buffer binds a buffer its own synchronizeResource made a member: the set has it already, so no new reference
   is counted, yet a command after the copy may write it. That bind counts as a use (synchronizeResource then copies again). */
static inline void nvmtl_sync_touch(__unsafe_unretained id o, uint64_t epoch) {
    static Class BC; if (!BC) BC = [NVMTLBuffer class];
    if (object_getClass(o) != BC) return;
    __unsafe_unretained NVMTLBuffer *b = o;
    if (__atomic_load_n(&b->_syncEpoch, __ATOMIC_ACQUIRE) == epoch) { __atomic_store_n(&b->_syncEpoch, 0, __ATOMIC_RELEASE); __atomic_add_fetch(&b->_gpuUse, 1, __ATOMIC_RELAXED); }
}
@implementation NVMTLResSet
- (instancetype)init { if ((self = [super init])) { const NSUInteger h = nvmtl_resset_hint();
    _s = nvmtl_identity_set(h); _epoch = nvmtl_resset_next_epoch(); } return self; }
- (instancetype)initWithCapacity:(NSUInteger)n { if ((self = [super init])) { _s = nvmtl_identity_set(n); _epoch = nvmtl_resset_next_epoch(); } return self; }
/* -addObject: that also tags the resource with this set's epoch, under the set's lock: the tag can only equal the current
   epoch while the resource is a member (every removal renews the epoch, also under the lock) */
// glcpu4 NVMTL_RESREF_BORROW: o is borrowed (the caller binds it and holds it; the set retains it when added) and the member
// is only tested for nil (_s keeps it): no retain of o at entry, no retain of the answer. Off: prod7's body, strong o.
static void nvmtl_resset_add_prod7(__unsafe_unretained NVMTLResSet *s, id o, uint64_t *tag) { os_unfair_lock_lock(&s->_lk);
    id member = [s->_s member:o]; BOOL had = member != nil;
    if (!had) [s->_s addObject:o]; else nvmtl_sync_touch(o, s->_epoch);
    __atomic_store_n(tag, s->_epoch, __ATOMIC_RELEASE);
    os_unfair_lock_unlock(&s->_lk); if (!had) nvmtl_resref(o, 1); }
- (void)nvmtlAdd:(__unsafe_unretained id)o tag:(uint64_t *)tag { if (!o) return;
    if (!nvmtl_resref_borrow_on()) { nvmtl_resset_add_prod7(self, o, tag); return; }
    os_unfair_lock_lock(&_lk);
    __unsafe_unretained id m = [_s member:o]; const BOOL had = m != nil;
    if (!had) [_s addObject:o]; else nvmtl_sync_touch(o, _epoch);
    __atomic_store_n(tag, _epoch, __ATOMIC_RELEASE);
    os_unfair_lock_unlock(&_lk); if (!had) nvmtl_resref(o, 1); }
// 10-07 (a user's crash report, RTX 5060 Ti): Hackintool died in -[__NSSetM member:] under -addObject:, called
// from setFragmentTexture on CoreAnimation's async-render workqueue thread. NSMutableSet is not thread-safe and nothing
// here serialised it, so every operation now holds _lk. Enumeration hands out a snapshot, never the live set.
- (NSUInteger)count { os_unfair_lock_lock(&_lk); NSUInteger n = _s.count; os_unfair_lock_unlock(&_lk); return n; }
- (id)member:(id)o { os_unfair_lock_lock(&_lk); id m = [_s member:o]; os_unfair_lock_unlock(&_lk); return m; }
- (NSArray *)nvmtlSnapshot { os_unfair_lock_lock(&_lk); NSArray *a = _s.allObjects; os_unfair_lock_unlock(&_lk); return a; }
- (NSEnumerator *)objectEnumerator { return [[self nvmtlSnapshot] objectEnumerator]; }
// for-in runs only over a command buffer part's own copy (one thread), never over a set still being encoded into
- (NSUInteger)countByEnumeratingWithState:(NSFastEnumerationState *)st objects:(id __unsafe_unretained [])b count:(NSUInteger)n {
    return [_s countByEnumeratingWithState:st objects:b count:n]; }
// _mru: objects added lately, each in the set until it is removed (the set retains it, so its address cannot be reused
// while it is listed). Encoders add the same few buffers, pipelines and samplers draw after draw (Apple's GL rebinds its
// vertex, index and uniform buffers for every draw), and a hit skips the set's hash lookup.
- (void)addObject:(id)o { if (!o) return; const void *p = (__bridge const void *)o; os_unfair_lock_lock(&_lk);
    for (unsigned k = 0; k < 8; k++) if (_mru[k] == p) { nvmtl_sync_touch(o, _epoch); os_unfair_lock_unlock(&_lk); return; }
    id member = [_s member:o]; BOOL had = member != nil;
    if (!had) { [_s addObject:o]; member = o; } else nvmtl_sync_touch(o, _epoch);
    // An equal object need not be the object retained by NSMutableSet. Cache only the retained member's address.
    _mru[_mruNext++ & 7] = (__bridge const void *)member;
    os_unfair_lock_unlock(&_lk); if (!had) nvmtl_resref(o, 1); }
- (void)removeObject:(id)o { if (!o) return; os_unfair_lock_lock(&_lk); id member = [_s member:o];
    if (member) { [_s removeObject:member]; __atomic_store_n(&_epoch, nvmtl_resset_next_epoch(), __ATOMIC_RELEASE); }
    for (unsigned k = 0; k < 8; k++) if (_mru[k] == (__bridge const void *)member) _mru[k] = NULL;
    os_unfair_lock_unlock(&_lk); if (member) nvmtl_resref(member, -1); }
// Clearing, copying and sealing never go through -allObjects. That array is autoreleased into the CALLER's pool, and
// these run on the app's own commit thread: Geekbench 7 never drains that pool, so every resource a command buffer had
// ever touched stayed alive, and Super Resolution filled VRAM with 9 MB MPS buffers until allocations were refused.
// The sets below are owned (+1) and released at the end of scope.
- (void)removeAllObjects { NSMutableSet *old; const NSUInteger h = nvmtl_resset_hint();
    NSMutableSet *fresh = nvmtl_identity_set(h);
    os_unfair_lock_lock(&_lk); old = _s; _s = fresh; memset(_mru, 0, sizeof _mru); nvmtl_resset_note(old.count);
    __atomic_store_n(&_epoch, nvmtl_resset_next_epoch(), __ATOMIC_RELEASE); os_unfair_lock_unlock(&_lk);
    for (id o in old) nvmtl_resref(o, -1); }
- (id)copyWithZone:(NSZone *)z { atomic_fetch_add(&gRes2Tick, 1);
    NVMTLResSet *c = [NVMTLResSet new];
    os_unfair_lock_lock(&_lk); NSMutableSet *m = [_s mutableCopy]; os_unfair_lock_unlock(&_lk);
    c->_s = m;
    for (id o in m) nvmtl_resref(o, 1);
    return c; }
- (id)mutableCopyWithZone:(NSZone *)z { return [self copyWithZone:z]; }
// Moves every member into a new set and empties this one in a single locked step. Sealing a submission part used to
// copy and then clear in two steps, so a resource another thread bound in between was released by the clear without
// being in the part: the command buffer stopped holding it while the GPU work still used it. Membership moves to one
// new owner, so each resource's residency count is unchanged; the new set's -dealloc gives it back.
- (NVMTLResSet *)nvmtlTakeAll { atomic_fetch_add(&gRes2Tick, 1);
    NVMTLResSet *taken = [NVMTLResSet new]; NSMutableSet *empty = taken->_s;
    os_unfair_lock_lock(&_lk); nvmtl_resset_note(_s.count); taken->_s = _s; _s = empty; memset(_mru, 0, sizeof _mru);
    __atomic_store_n(&_epoch, nvmtl_resset_next_epoch(), __ATOMIC_RELEASE); os_unfair_lock_unlock(&_lk);
    return taken; }
- (void)dealloc { for (id o in _s) nvmtl_resref(o, -1); }
@end

static BOOL nvmtl_purge_eligible(NVMTLTexture *t) {
    return !t->_parent && !t->_viewParent && !t->_backBuf && !t->_heap && !t->_subHeap && !t->_surf && !t->_vramOn && !t->_ownView
        && !t->_shareable && !t->_resid && !__atomic_load_n(&t->_nviews, __ATOMIC_ACQUIRE) && t->_i.img && !t->_i.placed
        && t->_i.usage && !t->_i.vfl && t->_i.samples <= 1 && !(t->_i.usage & 0x20u )
        // a swizzled sampling view (MTLTextureDescriptor.swizzle) is made against this VkImage; re-homing the memory
        // recreates the image and would leave that view pointing at the destroyed one (the depth-only view likewise)
        && !t->_swzView && !t->_sampledDepthView;
}
static uint64_t nvmtl_purge_reclaim(uint64_t need) {
    NSArray *all = nil;
    os_unfair_lock_lock(&gPurgeLock); all = gPurgeVol.count ? gPurgeVol.allObjects : nil; os_unfair_lock_unlock(&gPurgeLock);
    if (!all.count) return 0;
    uint64_t freed = 0; unsigned n = 0, busy = 0, inel = 0, fail = 0;
    for (int pass = 0; pass < 2 && freed < need; pass++)
        for (id o in all) {
            if (freed >= need) break;
            if (![o isKindOfClass:[NVMTLTexture class]]) continue;
            NVMTLTexture *t = o;
            uint32_t st = pass ? MTLPurgeableStateVolatile : MTLPurgeableStateEmpty;
            if (__atomic_load_n(&t->_purge, __ATOMIC_ACQUIRE) != st || t->_i.sysmem) continue;
            if (!nvmtl_purge_eligible(t)) { inel++; continue; }
            int z = 0; if (!__atomic_compare_exchange_n(&t->_rstate, &z, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) continue;
            uint32_t want = st;
            if (!__atomic_compare_exchange_n(&t->_purge, &want, (uint32_t)MTLPurgeableStateEmpty, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&t->_rstate, 0, __ATOMIC_RELEASE); continue; }
            if (__atomic_load_n(&t->_resRefs, __ATOMIC_ACQUIRE)) {
                busy++; uint32_t e = MTLPurgeableStateEmpty;
                __atomic_compare_exchange_n(&t->_purge, &e, st, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                __atomic_store_n(&t->_rstate, 0, __ATOMIC_RELEASE); continue; }
            uint64_t was = t->_i.alloc;
            if (nvmtl_vk_image_reback(&t->_i, 1, t->_fmt == MTLPixelFormatA8Unorm) == 0) {
                freed += was; n++; __atomic_store_n(&t->_rstate, 1, __ATOMIC_RELEASE);
            } else {
                fail++; uint32_t e = MTLPurgeableStateEmpty;
                __atomic_compare_exchange_n(&t->_purge, &e, st, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
                __atomic_store_n(&t->_rstate, 0, __ATOMIC_RELEASE); }
        }
    uint64_t s[6]; nvmtl_vk_res1_stats(s);
    nvlog("residency (res1): the wall is %llu MB short -> purged %u textures, %llu MB of VRAM given back (Empty/Volatile -> system memory, "
          "contents discarded as Metal allows); %u in use by a command buffer, %u not movable, %u rebuild failed; ever: %llu purged "
          "(%llu MB), %llu paged back on (%llu MB), %llu page-ons refused, %llu wall calls",
          (unsigned long long)(need >> 20), n, (unsigned long long)(freed >> 20), busy, inel, fail,
          (unsigned long long)s[0], (unsigned long long)s[3], (unsigned long long)s[1], (unsigned long long)s[4],
          (unsigned long long)s[2], (unsigned long long)s[5]);
    return freed;
}
extern uint64_t (*nvmtl_reclaim_hook)(uint64_t need);
static int nvmtl_res1_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_NO_PURGE_RECLAIM"); on = !(e && e[0] == '1');
        nvlog("residency (res1): purge under VRAM pressure %s (NVMTL_NO_PURGE_RECLAIM=%s)", on ? "ON" : "OFF", e ? e : "unset"); }
    return on;
}

#define NVMTL_EPN 256
static _Atomic uint64_t gRes2Epoch = 1, gRes2Drained;
static _Atomic int gRes2EpLive[NVMTL_EPN];
static uint64_t nvmtl_ep_enter(void) { const uint64_t e = atomic_load(&gRes2Epoch); atomic_fetch_add(&gRes2EpLive[e % NVMTL_EPN], 1); return e; }
static _Atomic unsigned gGraveN;
static void nvmtl_res2_cb_done(NVMTLCommandBuffer *cb) {
    const uint64_t e = __atomic_exchange_n(&cb->_rep, 0, __ATOMIC_ACQ_REL);
    if (!e) return;
    atomic_fetch_sub(&gRes2EpLive[e % NVMTL_EPN], 1);
    if (atomic_load(&gGraveN) || atomic_load(&gRes2Scan)) nvmtl_res2_kick(0);
}
static void nvmtl_ep_drain(void) {
    const uint64_t cur = atomic_load(&gRes2Epoch); uint64_t d = atomic_load(&gRes2Drained);
    while (d + 1 < cur && atomic_load(&gRes2EpLive[(d + 1) % NVMTL_EPN]) == 0) d++;
    atomic_store(&gRes2Drained, d);
}
static uint64_t nvmtl_ep_now(void) { return atomic_load(&gRes2Epoch); }
static BOOL nvmtl_ep_room(void) { return atomic_load(&gRes2Epoch) + 2 - atomic_load(&gRes2Drained) < NVMTL_EPN; }
typedef struct { nvk_image img; uint64_t ep; } nvmtl_grave;
static os_unfair_lock gGraveLock = OS_UNFAIR_LOCK_INIT;
static nvmtl_grave *gGrave; static unsigned gGraveCap; static uint64_t gGraveBytes, gGraveVram, gGraveFreed;
static void nvmtl_res2_reap(void) {
    nvmtl_ep_drain();
    const uint64_t d = atomic_load(&gRes2Drained);
    os_unfair_lock_lock(&gGraveLock);
    unsigned n = atomic_load(&gGraveN), k = 0;
    for (unsigned j = 0; j < n; j++) {
        if (gGrave[j].ep <= d) {
            gGraveBytes -= gGrave[j].img.alloc; if (!gGrave[j].img.sysmem) gGraveVram -= gGrave[j].img.alloc;
            nvmtl_vk_image_free_backing(&gGrave[j].img); gGraveFreed++;
        } else gGrave[k++] = gGrave[j];
    }
    atomic_store(&gGraveN, k);
    os_unfair_lock_unlock(&gGraveLock);
}
static void nvmtl_grave_put(const nvk_image *old) {
    os_unfair_lock_lock(&gGraveLock);
    unsigned n = atomic_load(&gGraveN);
    if (n == gGraveCap) { unsigned c = gGraveCap ? gGraveCap * 2 : 64; nvmtl_grave *g = realloc(gGrave, c * sizeof *g); if (g) { gGrave = g; gGraveCap = c; } }
    if (n < gGraveCap) {
        gGrave[n].img = *old; gGrave[n].ep = atomic_load(&gRes2Epoch);
        gGraveBytes += old->alloc; if (!old->sysmem) gGraveVram += old->alloc;
        atomic_store(&gGraveN, n + 1);
    } else { nvk_image leak = *old; (void)leak;
        nvlog("residency (res2): graveyard realloc FAILED - an old %llu MB backing is LEAKED, not freed in use", (unsigned long long)(old->alloc >> 20)); }
    os_unfair_lock_unlock(&gGraveLock);
}
static int gRes2On = -1; static uint64_t gRes2Cold, gRes2Hot, gRes2Margin;
static int nvmtl_res2_on(void) {
    if (gRes2On < 0) {
        const char *e = getenv("NVMTL_NO_RES2"), *c = getenv("NVMTL_RES2_COLD"), *h = getenv("NVMTL_RES2_HOT"), *m = getenv("NVMTL_RES2_MARGIN_MB");
        gRes2Cold = c && *c ? strtoull(c, NULL, 10) : 600;
        gRes2Hot = h && *h ? strtoull(h, NULL, 10) : 120;
        gRes2Margin = (m && *m ? strtoull(m, NULL, 10) : 256) << 20;
        gRes2On = !(e && e[0] == '1');
        nvlog("residency (res2): LRU page-off/page-on %s (NVMTL_NO_RES2=%s) - cold %llu ticks, hot %llu ticks, margin %llu MB",
              gRes2On ? "ON" : "OFF", e ? e : "unset", (unsigned long long)gRes2Cold, (unsigned long long)gRes2Hot, (unsigned long long)(gRes2Margin >> 20));
    }
    return gRes2On;
}
static BOOL nvmtl_res2_movable(NVMTLTexture *t) {
    return !t->_parent && !t->_viewParent && !t->_backBuf && !t->_heap && !t->_subHeap && !t->_surf && !t->_vramOn && !t->_ownView
        && !t->_shareable && !__atomic_load_n(&t->_nviews, __ATOMIC_ACQUIRE) && t->_i.img && !t->_i.placed && t->_i.usage
        && !t->_i.vfl && t->_i.samples <= 1 && !(t->_i.usage & 0x20u )
        // the move rebuilds _i.view and the slot only: a swizzled or depth-only view of the old image would outlive it,
        // and the slot rewrite would drop the swizzle (tools/check_rehome_views.py)
        && !t->_swzView && !t->_sampledDepthView;
}
static os_unfair_lock gRes2RegLock = OS_UNFAIR_LOCK_INIT;
static NSHashTable *gRes2Reg;
extern void (*nvmtl_res2_pressure_hook)(uint64_t want);
static _Atomic uint64_t gRes2Want;
static void nvmtl_res2_pressure(uint64_t want) {
    uint64_t w = atomic_load(&gRes2Want); while (want > w && !atomic_compare_exchange_weak(&gRes2Want, &w, want)) {}
    nvmtl_res2_kick(1);
}
static void nvmtl_res2_note_use(NVMTLTexture *t) {
    __atomic_store_n(&t->_lastUse, atomic_load(&gRes2Tick), __ATOMIC_RELAXED);
    if (!__atomic_load_n(&t->_lruReg, __ATOMIC_ACQUIRE)) {
        int z = 0;
        if (__atomic_compare_exchange_n(&t->_lruReg, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && nvmtl_res2_on() && nvmtl_res2_movable(t)) {
            os_unfair_lock_lock(&gRes2RegLock);
            if (!gRes2Reg) gRes2Reg = [NSHashTable weakObjectsHashTable];
            [gRes2Reg addObject:t];
            os_unfair_lock_unlock(&gRes2RegLock);
            if (!nvmtl_res2_pressure_hook) nvmtl_res2_pressure_hook = nvmtl_res2_pressure;
            if (!nvmtl_res2_evict_hook) nvmtl_res2_evict_hook = nvmtl_res2_evict;
        }
    }
    if (t->_i.sysmem && !__atomic_load_n(&t->_rstate, __ATOMIC_ACQUIRE) && !__atomic_load_n(&t->_wantOn, __ATOMIC_ACQUIRE)
        && __atomic_load_n(&t->_lruReg, __ATOMIC_ACQUIRE) && nvmtl_res2_on() && nvmtl_res2_movable(t))
        __atomic_store_n(&t->_wantOn, 1, __ATOMIC_RELEASE);
}
static dispatch_queue_t gRes2Q; static _Atomic int gRes2Pending, gRes2Scan; static _Atomic uint64_t gRes2LastNs;
static void nvmtl_res2_pass(void);
static os_unfair_lock gRes2PassLock = OS_UNFAIR_LOCK_INIT;
static __thread int gRes2InPass;
static uint64_t gRes2EvictCalls, gRes2EvictBytes;
static _Atomic int gRes2Force;
static void nvmtl_res2_pass_guarded(int from_worker) {
    if (gRes2InPass) return;
    if (!os_unfair_lock_trylock(&gRes2PassLock)) {
        if (from_worker) { nvmtl_res2_kick(1); return; }
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); int got = 0;
        while (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0 < 30000000ull) { usleep(200); if (os_unfair_lock_trylock(&gRes2PassLock)) { got = 1; break; } }
        if (!got) { static unsigned said; if (said++ < 4) nvlog("residency (res2): allocating thread waited 30 ms for the pager and gave up - this allocation spills"); return; }
    }
    gRes2InPass = 1;
    nvmtl_res2_pass();
    gRes2InPass = 0;
    os_unfair_lock_unlock(&gRes2PassLock);
}
static uint64_t nvmtl_res2_evict(uint64_t want) {
    const int force = (int)(want >> 63); want &= ~(1ull << 63);
    if (gRes2On != 1 || !want || gRes2InPass) return 0;
    if (force) atomic_store(&gRes2Force, 1);
    const uint64_t before = nvmtl_vk_vram_free_now();
    uint64_t w = atomic_load(&gRes2Want);
    do { if (want <= w) break; } while (!atomic_compare_exchange_weak(&gRes2Want, &w, want));
    atomic_store(&gRes2Scan, 1);
    @autoreleasepool { nvmtl_res2_pass_guarded(0); }
    uint64_t after = nvmtl_vk_vram_free_now();
    static _Atomic uint64_t blindUntil;
    if (after <= before && gGraveVram && clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) >= atomic_load(&blindUntil)) {
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        while (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0 < 16000000ull) {
            usleep(1000); nvmtl_res2_reap(); after = nvmtl_vk_vram_free_now();
            if (after > before) break;
        }
        if (after <= before) { atomic_store(&blindUntil, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) + 250000000ull);
            static unsigned said; if (said++ < 4 || said % 256 == 0)
                nvlog("residency (res2): waited 16 ms for %llu MB of evicted VRAM to drain and it did not - this allocation spills (%u)",
                      (unsigned long long)(gGraveVram >> 20), said); }
    }
    const uint64_t freed = after > before ? after - before : 0;
    gRes2EvictCalls++; gRes2EvictBytes += freed;
    return freed;
}
static void nvmtl_res2_kick(int scan) {
    if (gRes2On != 1) return;
    if (scan) atomic_store(&gRes2Scan, 1);
    int z = 0; if (!atomic_compare_exchange_strong(&gRes2Pending, &z, 1)) return;
    static dispatch_once_t once; dispatch_once(&once, ^{ gRes2Q = dispatch_queue_create("nvmtl.residency", DISPATCH_QUEUE_SERIAL); });
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), last = atomic_load(&gRes2LastNs);
    const int64_t wait = last && now - last < 16000000ull ? (int64_t)(16000000ull - (now - last)) : 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, wait), gRes2Q, ^{
        atomic_store(&gRes2Pending, 0);
        @autoreleasepool { nvmtl_res2_pass_guarded(1); }
        atomic_store(&gRes2LastNs, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
    });
}
extern int nvmtl_vk_mig_batch_begin(void); extern int nvmtl_vk_mig_batch_end(void);
static int gRes2Batching;
typedef struct { __unsafe_unretained NVMTLTexture *t; nvk_image old; } nvmtl_pend_t;
static nvmtl_pend_t *gPend; static unsigned gPendN, gPendCap; static uint64_t gBatchN, gBatchMoves;
static void nvmtl_grave_put(const nvk_image *old);
static void nvmtl_res2_batch_end(void);
static void nvmtl_res2_pend(NVMTLTexture *t, const nvk_image *old) {
    if (gPendN == gPendCap) { unsigned c = gPendCap ? gPendCap * 2 : 128; nvmtl_pend_t *n = realloc(gPend, c * sizeof *n); if (n) { gPend = n; gPendCap = c; } }
    if (gPendN < gPendCap) { gPend[gPendN].t = t; gPend[gPendN].old = *old; gPendN++; }
    else { nvmtl_res2_batch_end(); nvmtl_grave_put(old); __atomic_store_n(&t->_rstate, 0, __ATOMIC_RELEASE); }
    if (gPendN >= 128) { nvmtl_res2_batch_end(); nvmtl_vk_mig_batch_begin(); }
}
static void nvmtl_res2_batch_end(void) {
    (void)nvmtl_vk_mig_batch_end();
    for (unsigned k = 0; k < gPendN; k++) { nvmtl_grave_put(&gPend[k].old); __atomic_store_n(&gPend[k].t->_rstate, 0, __ATOMIC_RELEASE); }
    if (gPendN) { gBatchN++; gBatchMoves += gPendN;
        static unsigned said; if (said++ < 4 || said % 256 == 0) nvlog("residency (res2): evict-batch %llu: %u textures copied in ONE submission (%llu moves in %llu batches)",
            (unsigned long long)gBatchN, gPendN, (unsigned long long)gBatchMoves, (unsigned long long)gBatchN); }
    gPendN = 0;
}
static int nvmtl_res2_move(NVMTLTexture *t, int to_sys) {
    int z = 0; if (!__atomic_compare_exchange_n(&t->_rstate, &z, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    int r = 1, held = 0;
    @synchronized (t) {
        if (!__atomic_load_n(&t->_resRefs, __ATOMIC_ACQUIRE) && __atomic_load_n(&t->_useEp, __ATOMIC_ACQUIRE) <= atomic_load(&gRes2Drained)) {
            nvk_image old; memset(&old, 0, sizeof old);
            r = nvmtl_vk_image_migrate(&t->_i, to_sys, t->_fmt == MTLPixelFormatA8Unorm, &old);
            if (r == 0) {
                if (t->_resid & (1ull << 62)) {
                    // the slot's sampled view follows the new image, and so must its storage descriptor (binding 2):
                    // the old image is freed below, and a kernel writing through a stale binding 2 stored into freed memory
                    const uint32_t slot = (uint32_t)(t->_resid & 0xffffffffu);
                    nvmtl_vk_bindless_rewrite(slot, t->_i.view);
                    nvmtl_vk_bindless_put_storage(slot, t->_stex ? [t nvview] : NULL);
                }
                if (gRes2Batching) { nvmtl_res2_pend(t, &old); held = 1; }
                else nvmtl_grave_put(&old);
            }
        }
    }
    if (!held) __atomic_store_n(&t->_rstate, 0, __ATOMIC_RELEASE);
    return r;
}
static Class gNVMTLBufClass;
static BOOL nvmtl_res3_movable(NVMTLBuffer *b) {
    if (!gNVMTLBufClass) gNVMTLBufClass = [NVMTLBuffer class];
    return object_getClass(b) == gNVMTLBufClass && b->_storage == MTLStorageModePrivate && b->_b.sparse && b->_b.buf && b->_b.mem
        && !b->_b.map && !b->_heap && !b->_subHeap && !b->_hostPtr && !b->_shadow.buf && !b->_texSeen && !b->_nbpk && !b->_subPlaced
        && !b->_b.pool && !b->_b.placed && !b->_b.imported && !b->_b.bar1 && !b->_b.moff;
}
static void nvmtl_res3_note_use(NVMTLBuffer *b) {
    __atomic_store_n(&b->_lastUse, atomic_load(&gRes2Tick), __ATOMIC_RELAXED);
    if (!b->_b.sparse) return;
    if (!__atomic_load_n(&b->_lruReg, __ATOMIC_ACQUIRE)) {
        int z = 0;
        if (__atomic_compare_exchange_n(&b->_lruReg, &z, 1, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE) && nvmtl_res2_on() && nvmtl_res3_movable(b)) {
            os_unfair_lock_lock(&gRes2RegLock);
            if (!gRes2Reg) gRes2Reg = [NSHashTable weakObjectsHashTable];
            [gRes2Reg addObject:b];
            os_unfair_lock_unlock(&gRes2RegLock);
            if (!nvmtl_res2_pressure_hook) nvmtl_res2_pressure_hook = nvmtl_res2_pressure;
            if (!nvmtl_res2_evict_hook) nvmtl_res2_evict_hook = nvmtl_res2_evict;
        }
    }
    if (b->_b.sysmem && !__atomic_load_n(&b->_rstate, __ATOMIC_ACQUIRE) && !__atomic_load_n(&b->_wantOn, __ATOMIC_ACQUIRE)
        && __atomic_load_n(&b->_lruReg, __ATOMIC_ACQUIRE) && nvmtl_res2_on() && nvmtl_res3_movable(b))
        __atomic_store_n(&b->_wantOn, 1, __ATOMIC_RELEASE);
}
static int nvmtl_res3_move(NVMTLBuffer *b, int to_sys) {
    int z = 0; if (!__atomic_compare_exchange_n(&b->_rstate, &z, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return 1;
    int r = 1;
    if (!__atomic_load_n(&b->_resRefs, __ATOMIC_ACQUIRE) && __atomic_load_n(&b->_useEp, __ATOMIC_ACQUIRE) <= atomic_load(&gRes2Drained))
        r = nvmtl_vk_buffer_migrate(&b->_b, to_sys);
    __atomic_store_n(&b->_rstate, 0, __ATOMIC_RELEASE);
    return r;
}
static inline BOOL r2_tex(id o) { return [o isKindOfClass:gNVMTLTexClass]; }
static inline int *r2_rst(id o)  { if (r2_tex(o)) { NVMTLTexture *t = o; return &t->_rstate; }  NVMTLBuffer *b = o; return &b->_rstate; }
static inline int *r2_refs(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return &t->_resRefs; } NVMTLBuffer *b = o; return &b->_resRefs; }
static inline int *r2_want(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return &t->_wantOn; }  NVMTLBuffer *b = o; return &b->_wantOn; }
static inline uint64_t *r2_useEp(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return &t->_useEp; } NVMTLBuffer *b = o; return &b->_useEp; }
static inline uint64_t *r2_last(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return &t->_lastUse; } NVMTLBuffer *b = o; return &b->_lastUse; }
static inline uint32_t r2_purge(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return __atomic_load_n(&t->_purge, __ATOMIC_ACQUIRE); }
                                        NVMTLBuffer *b = o; return __atomic_load_n(&b->_purge, __ATOMIC_ACQUIRE); }
static inline int r2_sys(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return t->_i.sysmem != 0; } NVMTLBuffer *b = o; return b->_b.sysmem != 0; }
static inline uint64_t r2_alloc(id o) { if (r2_tex(o)) { NVMTLTexture *t = o; return t->_i.alloc; } NVMTLBuffer *b = o; return b->_b.alloc; }
static inline BOOL r2_movable(id o) { return r2_tex(o) ? (((NVMTLTexture *)o)->_i.img && nvmtl_res2_movable(o)) : nvmtl_res3_movable(o); }
static inline int r2_move(id o, int to_sys) { return r2_tex(o) ? nvmtl_res2_move(o, to_sys) : nvmtl_res3_move(o, to_sys); }
static void nvmtl_res2_pass(void) {
    nvmtl_res2_reap();
    if (!atomic_exchange(&gRes2Scan, 0)) return;
    NSArray *all = nil;
    os_unfair_lock_lock(&gRes2RegLock); all = gRes2Reg.count ? gRes2Reg.allObjects : nil; os_unfair_lock_unlock(&gRes2RegLock);
    const uint64_t want = atomic_exchange(&gRes2Want, 0);
    if (!all.count) return;
    if (!gNVMTLTexClass) gNVMTLTexClass = [NVMTLTexture class];
    if (!nvmtl_ep_room()) { nvlog("residency (res2): PAUSED - a command buffer from %llu epochs ago has not completed; nothing moves until it does",
                                  (unsigned long long)(atomic_load(&gRes2Epoch) - atomic_load(&gRes2Drained))); return; }
    const uint64_t tick = atomic_load(&gRes2Tick);
    NSMutableArray *cold = [NSMutableArray new], *hot = [NSMutableArray new];
    const uint64_t drained = atomic_load(&gRes2Drained); unsigned undrained = 0;
    for (id o in all) {
        if (__atomic_load_n(r2_rst(o), __ATOMIC_ACQUIRE) || !r2_movable(o)) continue;
        if (__atomic_load_n(r2_useEp(o), __ATOMIC_ACQUIRE) > drained) { undrained++; continue; }
        const uint32_t ps = r2_purge(o);
        const uint64_t lu = __atomic_load_n(r2_last(o), __ATOMIC_RELAXED);
        if (!r2_sys(o)) {
            if (lu + (want ? 0 : gRes2Cold) <= tick && (ps == 0 || ps == MTLPurgeableStateNonVolatile) && !__atomic_load_n(r2_refs(o), __ATOMIC_ACQUIRE)) [cold addObject:o];
        } else if (__atomic_load_n(r2_want(o), __ATOMIC_ACQUIRE) || lu + gRes2Hot >= tick) [hot addObject:o];
    }
    [cold sortUsingComparator:^NSComparisonResult(id a, id b) { uint64_t x = *r2_last(a), y = *r2_last(b); return x < y ? NSOrderedAscending : x > y ? NSOrderedDescending : NSOrderedSame; }];
    [hot sortUsingComparator:^NSComparisonResult(id a, id b) { uint64_t x = *r2_last(a), y = *r2_last(b); return x > y ? NSOrderedAscending : x < y ? NSOrderedDescending : NSOrderedSame; }];
    unsigned on = 0, off = 0, onB = 0, offB = 0, busy = 0, noroom = 0; uint64_t onM = 0, offM = 0;
    const uint64_t coldest = cold.count ? tick - *r2_last(cold[0]) : 0;
    NSUInteger ci = 0;
    for (id t in (want ? @[] : hot)) {
        if (__atomic_load_n(r2_refs(t), __ATOMIC_ACQUIRE)) { busy++; continue; }
        const uint64_t need = r2_alloc(t);
        if (!nvmtl_vk_vram_room(need)) {
            uint64_t pending = 0;
            while (ci < cold.count && pending < need + gRes2Margin) {
                id v = cold[ci++];
                if (*r2_last(v) + gRes2Cold > *r2_last(t)) continue;
                const uint64_t a = r2_alloc(v); const BOOL vb = !r2_tex(v);
                if (r2_move(v, 1) == 0) { off++; offB += vb; offM += a; pending += a; }
            }
            noroom++;
            continue;
        }
        const uint64_t a = r2_alloc(t); const BOOL tb = !r2_tex(t);
        if (r2_move(t, 0) == 0) { on++; onB += tb; onM += a; __atomic_store_n(r2_want(t), 0, __ATOMIC_RELEASE); }
    }
    if (want) {
        const uint64_t target = nvmtl_vk_vram_headroom() + gRes2Margin + want;
        if (nvmtl_vk_mig_batch_begin() == 0) gRes2Batching = 1;
        { static unsigned said; if (said++ < 16 || said % 1024 == 0)
            nvlog("residency (res2): pressure pass wants %llu MB - registry %lu, victims left %lu of %lu, "
                  "stranded %lu, undrained %u, in use %u, free %llu MB + graveyard %llu MB vs target %llu MB",
                  (unsigned long long)(want >> 20), (unsigned long)all.count,
                  (unsigned long)(cold.count > ci ? cold.count - ci : 0), (unsigned long)cold.count,
                  (unsigned long)hot.count, undrained, busy,
                  (unsigned long long)(nvmtl_vk_vram_free_now() >> 20),
                  (unsigned long long)(gGraveVram >> 20), (unsigned long long)(target >> 20)); }
        const int force = atomic_exchange(&gRes2Force, 0);
        const uint64_t f0 = nvmtl_vk_vram_free_now() + gGraveVram, cap = force ? want + gRes2Margin : (target > f0 ? target - f0 : 0); uint64_t movedM = 0;
        while (ci < cold.count && movedM < cap && (force || nvmtl_vk_vram_free_now() + gGraveVram < target)) {
            id v = cold[ci++]; const uint64_t a = r2_alloc(v); const BOOL vb = !r2_tex(v);
            if (r2_move(v, 1) == 0) { off++; offB += vb; offM += a; movedM += a; }
        }
        if (gRes2Batching) { nvmtl_res2_batch_end(); gRes2Batching = 0; }
    }
    if (on || off) {
        if (nvmtl_ep_room()) atomic_fetch_add(&gRes2Epoch, 1);
        uint64_t s[5], s3[6]; nvmtl_vk_res2_stats(s); nvmtl_vk_res3_stats(s3);
        nvlog("residency (res2): paged ON %u (%llu MB, %u buffers), paged OFF %u (%llu MB, %u buffers, coldest idle %llu ticks); %u in use, %u waiting for room; "
              "graveyard %u (%llu MB, %llu MB VRAM) freed %llu; free VRAM %llu MB; ever: textures off %llu (%llu MB) on %llu (%llu MB), %llu copy failures; "
              "buffers off %llu (%llu MB) on %llu (%llu MB), %llu stable-address buffers made (%llu in system memory at birth)",
              on, (unsigned long long)(onM >> 20), onB, off, (unsigned long long)(offM >> 20), offB, (unsigned long long)coldest, busy, noroom,
              atomic_load(&gGraveN), (unsigned long long)(gGraveBytes >> 20), (unsigned long long)(gGraveVram >> 20), (unsigned long long)gGraveFreed,
              (unsigned long long)(nvmtl_vk_vram_free_now() >> 20),
              (unsigned long long)s[0], (unsigned long long)s[2], (unsigned long long)s[1], (unsigned long long)s[3], (unsigned long long)s[4],
              (unsigned long long)s3[2], (unsigned long long)s3[4], (unsigned long long)s3[3], (unsigned long long)s3[5],
              (unsigned long long)s3[0], (unsigned long long)s3[1]);
    }
    if (noroom) { atomic_store(&gRes2Scan, 1); if (off) nvmtl_res2_kick(1); }
    if (undrained) { if (!(on || off) && nvmtl_ep_room()) atomic_fetch_add(&gRes2Epoch, 1); atomic_store(&gRes2Scan, 1); }
}

static void nvmtl_purge_track(id r, MTLPurgeableState s) {
    if (s != MTLPurgeableStateVolatile && s != MTLPurgeableStateEmpty && s != MTLPurgeableStateNonVolatile) return;
    os_unfair_lock_lock(&gPurgeLock);
    if (s == MTLPurgeableStateNonVolatile) { [gPurgeVol removeObject:r]; }
    else {
        if (!gPurgeVol) gPurgeVol = [NSHashTable weakObjectsHashTable];
        [gPurgeVol addObject:r];
        atomic_fetch_add(&gPurgeAsks, 1);
        nvmtl_purge_report_hook = nvmtl_purge_report;
        if (!nvmtl_reclaim_hook && nvmtl_res1_on()) nvmtl_reclaim_hook = nvmtl_purge_reclaim;
    }
    os_unfair_lock_unlock(&gPurgeLock);
}

static void nvmtl_purge_report(const char *why) {
    uint64_t nvol = 0, nemp = 0, bvol = 0, bemp = 0;
    NSArray *all = nil;
    os_unfair_lock_lock(&gPurgeLock);
    all = gPurgeVol.allObjects;
    os_unfair_lock_unlock(&gPurgeLock);
    for (id r in all) {
        uint32_t st = 0; uint64_t bytes = 0;
        if ([r isKindOfClass:[NVMTLTexture class]]) { NVMTLTexture *t = r; st = t->_purge; bytes = t->_i.alloc; }
        else if ([r isKindOfClass:[NVMTLBuffer class]]) { NVMTLBuffer *b = r; st = b->_purge; bytes = b->_b.alloc; }
        else continue;
        if (st == MTLPurgeableStateEmpty) { nemp++; bemp += bytes; }
        else if (st == MTLPurgeableStateVolatile) { nvol++; bvol += bytes; }
    }
    nvlog("purgeable at the %s spill: %llu Volatile holding %llu MB, %llu Empty holding %llu MB, of %llu asks ever"
          " - still marked after the res1 consumer ran (buffers, views, bindless or in-use textures): %llu MB Empty",
          why, (unsigned long long)nvol, (unsigned long long)(bvol >> 20),
          (unsigned long long)nemp, (unsigned long long)(bemp >> 20),
          (unsigned long long)atomic_load(&gPurgeAsks), (unsigned long long)(bemp >> 20));
}
static void nvmtl_pre_release(void *held) { @autoreleasepool { (void)CFBridgingRelease(held); } }
static void nvmtl_zero_flush(void);
static void nvmtl_managed_flush(void) {
    if (!atomic_load(&gManagedAny) || gManagedInFlush) return;
    gManagedInFlush = 1;
    NSMutableArray *todo = [NSMutableArray new];
    os_unfair_lock_lock(&gManagedLock);
    for (NVMTLBuffer *b in gManagedDirty.allObjects) {
        if (b->_dirtyHi > b->_dirtyLo) [todo addObject:@[b, @(b->_dirtyLo), @(b->_dirtyHi)]];
        b->_dirtyLo = b->_dirtyHi = 0;
    }
    [gManagedDirty removeAllObjects]; atomic_store(&gManagedAny, 0);
    os_unfair_lock_unlock(&gManagedLock);
    // A managed buffer can be created and dirtied after presubmit's first zero
    // snapshot. Claim its initial clear before uploading this dirty snapshot;
    // otherwise the next submission can clear data already uploaded here.
    if (todo.count) nvmtl_zero_flush();
    if (todo.count && nvmtl_vk_pre_open()) {
        for (NSArray *t in todo) {
            NVMTLBuffer *b = t[0]; NSUInteger lo = [t[1] unsignedIntegerValue], n = [t[2] unsignedIntegerValue] - lo;
            if (nvmtl_vk_pre_copy(&b->_shadow, lo, &b->_b, lo, n))
                nvlog("managed: upload of %lu bytes at %lu FAILED - the GPU keeps the old bytes", (unsigned long)n, (unsigned long)lo);
        }
        nvmtl_pre_release_hook = nvmtl_pre_release;
        nvmtl_vk_pre_hold((__bridge_retained void *)todo);
    } else
    for (NSArray *t in todo) {
        NVMTLBuffer *b = t[0]; NSUInteger lo = [t[1] unsignedIntegerValue], n = [t[2] unsignedIntegerValue] - lo;
        if (nvmtl_vk_copy_buffer(nvmtl_device_queue(), &b->_shadow, lo, &b->_b, lo, n))
            nvlog("managed: upload of %lu bytes at %lu FAILED - the GPU keeps the old bytes", (unsigned long)n, (unsigned long)lo);
    }
    gManagedInFlush = 0;
}
static void nvmtl_presubmit(void);
static void nvmtl_managed_mark(NVMTLBuffer *b, NSUInteger lo, NSUInteger hi) {
    os_unfair_lock_lock(&gManagedLock);
    if (!gManagedDirty) gManagedDirty = [NSHashTable weakObjectsHashTable];
    if (b->_dirtyHi <= b->_dirtyLo) { b->_dirtyLo = lo; b->_dirtyHi = hi; }
    else { b->_dirtyLo = MIN(b->_dirtyLo, lo); b->_dirtyHi = MAX(b->_dirtyHi, hi); }
    @autoreleasepool { [gManagedDirty addObject:b]; } atomic_store(&gManagedAny, 1);
    nvmtl_pre_submit_hook = nvmtl_presubmit;
    os_unfair_lock_unlock(&gManagedLock);
}
static os_unfair_lock gZeroLock = OS_UNFAIR_LOCK_INIT;
static NSHashTable *gZeroOwed;
static _Atomic int gZeroAny;
static __thread int gZeroInFlush;
static _Atomic unsigned long long gZeroHostN, gZeroHostB, gZeroGpuN, gZeroGpuB, gZeroTail;
static int nvmtl_zero_mode(void) {
    static int m = -1;
    if (m < 0) { const char *e = getenv("NVMTL_ZERO_FILL");
        m = !e ? 2 : (e[0] == '0' ? 0 : (strcmp(e, "host") == 0 ? 1 : 2));
        nvlog("zero-fill: new buffers %s (NVMTL_ZERO_FILL=%s)", m == 2 ? "are cleared - CPU-mapped by memset, Private by a GPU fill before the next submission"
              : m == 1 ? "are cleared only where the CPU maps them - Private keeps what VRAM held" : "are NOT cleared - they hold what the memory held", e ? e : "unset"); }
    return m;
}
static void nvmtl_zero_census(void) {
    nvlog("zero-fill census: %llu CPU-mapped buffers (%llu bytes) cleared by memset, %llu Private (%llu bytes) by the GPU, %llu tail bytes left (length not a multiple of 4)",
          (unsigned long long)gZeroHostN, (unsigned long long)gZeroHostB, (unsigned long long)gZeroGpuN, (unsigned long long)gZeroGpuB, (unsigned long long)gZeroTail);
}
static void nvmtl_zero_flush(void) {
    if (!atomic_load(&gZeroAny) || gZeroInFlush) return;
    gZeroInFlush = 1;
    os_unfair_lock_lock(&gZeroLock);
    NSArray *todo = gZeroOwed.allObjects; [gZeroOwed removeAllObjects]; atomic_store(&gZeroAny, 0);
    os_unfair_lock_unlock(&gZeroLock);
    nvk_cmdbuf c; unsigned n = 0; size_t bytes = 0;
    if (todo.count && nvmtl_vk_pre_open()) {
        for (NVMTLBuffer *b in todo) { const size_t w = b->_b.size & ~(size_t)3;
            if (b->_b.buf && w && nvmtl_vk_pre_fill(&b->_b, 0, w, 0) == 0) { n++; bytes += w; gZeroTail += b->_b.size - w; } }
        nvmtl_pre_release_hook = nvmtl_pre_release;
        nvmtl_vk_pre_hold((__bridge_retained void *)todo);
        const unsigned long long k = atomic_fetch_add(&gZeroGpuN, n); gZeroGpuB += bytes;
        if (!k || (k >> 12) != ((k + n) >> 12)) nvmtl_zero_census();
    } else if (todo.count && nvmtl_vk_cmd_begin(nvmtl_device_queue(), &c) == 0) {
        for (NVMTLBuffer *b in todo) { const size_t w = b->_b.size & ~(size_t)3;
            if (b->_b.buf && w && nvmtl_vk_cmd_fill_buffer(&c, &b->_b, 0, w, 0) == 0) { n++; bytes += w; gZeroTail += b->_b.size - w; } }
        if (nvmtl_vk_submit_wait(&c)) nvlog("zero-fill: GPU clear of %u Private buffers (%zu bytes) FAILED - they hold what VRAM held", n, bytes);
        else { const unsigned long long k = atomic_fetch_add(&gZeroGpuN, n); gZeroGpuB += bytes;
               if (!k || (k >> 12) != ((k + n) >> 12)) nvmtl_zero_census(); }
    } else if (todo.count) nvlog("zero-fill: no command buffer for %lu Private clears - they hold what VRAM held", (unsigned long)todo.count);
    gZeroInFlush = 0;
}
static void nvmtl_presubmit(void) { @autoreleasepool { nvmtl_zero_flush(); } @autoreleasepool { nvmtl_managed_flush(); } }
static int nvmtl_mzero_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_MANAGED_EAGER_ZERO"); on = !(e && e[0] == '1');
        nvlog("zero-fill: managed buffers %s (NVMTL_MANAGED_EAGER_ZERO=%s)", on ? "clear their VRAM copy on the GPU before the next submit and their CPU copy on first CPU touch"
              : "memset their CPU copy at creation and upload it (old)", e ? e : "unset"); }
    return on;
}
static void nvmtl_zero_new_buffer(NVMTLBuffer *b, NSUInteger len) {
    const int m = nvmtl_zero_mode();
    if (!m || !b || !len) return;
    const int mz = b->_shadow.map && m >= 2 && b->_b.buf && !b->_b.map && nvmtl_mzero_on();
    if (mz) __atomic_store_n(&b->_zshadow, 1, __ATOMIC_RELEASE);
    if (b->_shadow.map && !mz) {
        memset(b->_shadow.map, 0, len); nvmtl_managed_mark(b, 0, MIN((NSUInteger)b->_b.size, len));
    } else if (b->_b.map && !mz) {
        memset(b->_b.map, 0, len);
    } else {
        if (m < 2 || !b->_b.buf) return;
        os_unfair_lock_lock(&gZeroLock);
        if (!gZeroOwed) gZeroOwed = [NSHashTable weakObjectsHashTable];
        @autoreleasepool { [gZeroOwed addObject:b]; }
        atomic_store(&gZeroAny, 1);
        nvmtl_pre_submit_hook = nvmtl_presubmit;
        os_unfair_lock_unlock(&gZeroLock);
        return;
    }
    const unsigned long long k = atomic_fetch_add(&gZeroHostN, 1); gZeroHostB += len;
    if (!k || ((k + 1) & 0xFFF) == 0) nvmtl_zero_census();
}
#define NVMTL_BUFPARK 4
static _Atomic uint64_t gBTNew, gBTRecycled, gBTParked, gBTEvicted;
static void nvmtl_buftex_census(const char *what) {
    const uint64_t n = atomic_load(&gBTNew) + atomic_load(&gBTRecycled);
    if (n & (n - 1)) return;
    nvlog("buffer texture (%s): %llu new, %llu recycled, %llu parked, %llu evicted", what,
          (unsigned long long)atomic_load(&gBTNew), (unsigned long long)atomic_load(&gBTRecycled),
          (unsigned long long)atomic_load(&gBTParked), (unsigned long long)atomic_load(&gBTEvicted));
}
static int nvmtl_bpark_take(NVMTLBuffer *b, const uint32_t *key, nvk_image *out) {
    int got = -1;
    os_unfair_lock_lock(&b->_bpl);
    for (uint32_t k = b->_nbpk; k-- > 0; ) {
        nvmtl_park_t *p = &b->_bpk[k];
        if (memcmp(p->key, key, sizeof p->key)) continue;
        *out = p->img; memmove(p, p + 1, (size_t)(b->_nbpk - k - 1) * sizeof *p); b->_nbpk--; got = 0; break;
    }
    os_unfair_lock_unlock(&b->_bpl);
    return got;
}
static void nvmtl_bpark_put(NVMTLBuffer *b, nvk_image *img, const uint32_t *key) {
    nvk_image ev; int evict = 0, parked = 0;
    os_unfair_lock_lock(&b->_bpl);
    if (!b->_bpk) b->_bpk = calloc(NVMTL_BUFPARK, sizeof *b->_bpk);
    if (b->_bpk) {
        if (b->_nbpk == NVMTL_BUFPARK) { ev = b->_bpk[0].img; evict = 1;
            memmove(&b->_bpk[0], &b->_bpk[1], (NVMTL_BUFPARK - 1) * sizeof *b->_bpk); b->_nbpk--; }
        nvmtl_park_t *p = &b->_bpk[b->_nbpk++]; p->img = *img; memcpy(p->key, key, sizeof p->key); p->off = p->len = 0;
        parked = 1;
    }
    os_unfair_lock_unlock(&b->_bpl);
    if (parked) { memset(img, 0, sizeof *img); atomic_fetch_add(&gBTParked, 1); }
    if (evict) { nvmtl_vk_image_destroy(&ev); atomic_fetch_add(&gBTEvicted, 1); }
}
static NVMTLTexture *nvmtl_texbuf_make(NVMTLBuffer *b, MTLTextureDescriptor *d, NSUInteger offset)
{
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    extern nvk_queue *nvmtl_device_queue(void);
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (!b || !d || !d.width || nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8) || a8 || !bpp) {
        nvlog("texture buffer: format %lu width %lu is not carried -> nil", (unsigned long)d.pixelFormat, (unsigned long)d.width); return nil; }
    const size_t range = (size_t)d.width * bpp;
    NVMTLTexture *t = [NVMTLTexture new]; int st = 0;
    if (nvmtl_vk_texel_view_create(&b->_b, offset, range, vkf, (d.usage & MTLTextureUsageShaderWrite) != 0, &t->_tbAlias, &t->_tbView, &st)) {
        nvlog("texture buffer: %lu x fmt %lu at offset %lu of a %zu-byte buffer -> nil (reason above)", (unsigned long)d.width,
              (unsigned long)d.pixelFormat, (unsigned long)offset, b->_b.size); return nil; }
    t->_tbStorage = st != 0;
    t->_i.w = (uint32_t)d.width; t->_i.h = 1; t->_i.fmt = vkf; t->_i.bpp = bpp; t->_i.mtl_type = 9; t->_i.mips = 1; t->_i.layers = 1; t->_i.samples = 1;
    t->_q = nvmtl_device_queue(); t->_fmt = d.pixelFormat; t->_mips = 1; t->_usage = d.usage; t->_stex = NO;
    t->_backBuf = b; t->_backOff = offset; t->_backBPR = range;
    b->_texSeen = YES;
    nvlog("texture buffer: %lu texels fmt %lu (VkFormat %u, %s) at offset %lu -> %p", (unsigned long)d.width, (unsigned long)d.pixelFormat, vkf,
          st ? "read/write" : "read", (unsigned long)offset, (__bridge void *)t);
    return t;
}
@implementation NVMTLBuffer
- (IOSurfaceRef)iosurface { return NULL; }
- (id<MTLTexture>)newTiledTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr {
    id<MTLTexture> t = [self newTextureWithDescriptor:d offset:offset bytesPerRow:bpr];
    if (t || !d) return t;
    void *base = [self contents];
    if (!base || d.textureType != MTLTextureType2D || offset + bpr * d.height > _b.size) {
        nvlog("newTiledTextureWithDescriptor: %lux%lu bpr %lu off %lu: no linear view and no CPU copy (buffer %lu B) -> nil",
              (unsigned long)d.width, (unsigned long)d.height, (unsigned long)bpr, (unsigned long)offset, (unsigned long)_b.size);
        return nil; }
    MTLTextureDescriptor *cd = [d copy]; cd.storageMode = MTLStorageModeShared;
    id<MTLTexture> c = [[self device] newTextureWithDescriptor:cd];
    if (c) [c replaceRegion:MTLRegionMake2D(0, 0, d.width, d.height) mipmapLevel:0 withBytes:(char *)base + offset bytesPerRow:bpr];
    { static int said; if (!said++) nvlog("newTiledTextureWithDescriptor: linear view refused -> copied texture (%lux%lu bpr %lu; said once)",
                                          (unsigned long)d.width, (unsigned long)d.height, (unsigned long)bpr); }
    return c;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)offset bytesPerRow:(NSUInteger)bpr {
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    if (!d) return nil;
    if (d.textureType == MTLTextureTypeTextureBuffer) return nvmtl_texbuf_make(self, d, offset);
    size_t w = d.width, h = d.height;
    if (d.textureType != MTLTextureType2D || d.mipmapLevelCount > 1 || d.sampleCount > 1 || d.arrayLength > 1 || d.depth > 1
        || !w || !h || w > UINT32_MAX || h > UINT32_MAX) {
        nvlog("buffer newTextureWithDescriptor:offset:%lu bytesPerRow:%lu: type %lu %zux%zu (mips %lu, samples %lu, array %lu, depth %lu) "
              "is not built - only a 2D single-level linear view -> nil", (unsigned long)offset, (unsigned long)bpr, (unsigned long)d.textureType,
              w, h, (unsigned long)d.mipmapLevelCount, (unsigned long)d.sampleCount, (unsigned long)d.arrayLength, (unsigned long)d.depth);
        return nil; }
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8)) {
        nvlog("buffer newTextureWithDescriptor: pixelFormat %lu has no VkFormat -> nil", (unsigned long)d.pixelFormat); return nil; }
    NVMTLTexture *t = [NVMTLTexture new];
    static int noRecycle = -1; if (noRecycle < 0) { noRecycle = getenv("NVMTL_NO_BUFTEX_RECYCLE") != NULL;
        nvlog("buffer texture: dres R3 recycle %s", noRecycle ? "OFF (NVMTL_NO_BUFTEX_RECYCLE)" : "ON"); }
    const uint32_t bkey[12] = { 2, (uint32_t)w, (uint32_t)h, vkf, bpp, (uint32_t)a8, (d.usage & MTLTextureUsageShaderWrite) != 0,
        (d.usage & MTLTextureUsageRenderTarget) != 0, (uint32_t)offset, (uint32_t)((uint64_t)offset >> 32), (uint32_t)bpr, (uint32_t)((uint64_t)bpr >> 32) };
    const BOOL brec = !noRecycle && _nbpk && nvmtl_bpark_take(self, bkey, &t->_i) == 0;
    if (!brec && nvmtl_vk_image_create_buffer_alias((uint32_t)w, (uint32_t)h, vkf, bpp, a8, (d.usage & MTLTextureUsageShaderWrite) != 0,
                                           (d.usage & MTLTextureUsageRenderTarget) != 0, &_b, offset, bpr, &t->_i)) {
        nvlog("buffer newTextureWithDescriptor:offset:%lu bytesPerRow:%lu (%zux%zu fmt %lu, buffer %lu B, storage %lu) -> nil (reason above)",
              (unsigned long)offset, (unsigned long)bpr, w, h, (unsigned long)d.pixelFormat, (unsigned long)_b.size, (unsigned long)_storage);
        return nil; }
    t->_q = nvmtl_device_queue();
    t->_fmt = d.pixelFormat; t->_mips = 1;
    t->_stex = t->_i.storage == 1;
    t->_ropt |= (MTLResourceOptions)d.storageMode << MTLResourceStorageModeShift;
    t->_usage = d.usage;
    _texSeen = YES;
    t->_backBuf = self; t->_backOff = offset; t->_backBPR = bpr;
    t->_bparkable = !noRecycle; memcpy(t->_pkey, bkey, sizeof bkey);
    atomic_fetch_add(brec ? &gBTRecycled : &gBTNew, 1); nvmtl_buftex_census(brec ? "recycled" : "new");
    nvmtl_texture_desc_swizzle(t, d);
    nvlog("buffer %p newTextureWithDescriptor:offset:%lu bytesPerRow:%lu -> %p (%zux%zu fmt %lu usage 0x%lx, storage %lu)", (__bridge void *)self,
          (unsigned long)offset, (unsigned long)bpr, (__bridge void *)t, w, h, (unsigned long)d.pixelFormat, (unsigned long)d.usage, (unsigned long)_storage);
    return t; }
- (void)setResponsibleProcess:(int)pid { static int said; if (!said++) nvlog("setResponsibleProcess: pid %d on a buffer - per-process attribution is not kept; nothing to set (said once)", pid); }
- (instancetype)initStandinWithDevice:(id)device bytesNoCopy:(void *)bytes length:(NSUInteger)length deallocator:(void (^)(void *, NSUInteger))deallocator {
    id d = device ? device : (id)gNVMTLMainDevice;
    id buf = [d newBufferWithBytesNoCopy:bytes length:length options:MTLResourceStorageModeShared deallocator:deallocator];
    { static int said; if (!said++) nvlog("initStandin: %lu-byte client-storage stand-in -> %s (said once)", (unsigned long)length,
                                          buf ? "a no-copy buffer over the caller's pages" : "nil (newBufferWithBytesNoCopy refused, reason above)"); }
    return buf;
}
- (NSUInteger)allocatedSize { return (NSUInteger)_b.size; }
// No buffer here is sparse in Metal's sense: every one is backed in full when it is made, and the plugin has neither
// placement-sparse buffers nor sparse page mapping (supportsPlacementSparse NO).
- (MTLBufferSparseTier)sparseBufferTier API_AVAILABLE(macos(26.0)) { return MTLBufferSparseTierNone; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (MTLCPUCacheMode)cpuCacheMode { NVMTLBuffer *r = self; return nvmtl_res_cache(r->_heap ? r->_heap : r->_subHeap, r->_ropt); }
- (MTLHazardTrackingMode)hazardTrackingMode { NVMTLBuffer *r = self; return nvmtl_res_hazard(r->_heap ? r->_heap : r->_subHeap, r->_ropt); }
- (MTLResourceOptions)resourceOptions { return ((MTLResourceOptions)[self storageMode] << MTLResourceStorageModeShift) | (MTLResourceOptions)[self cpuCacheMode]
                                            | ((MTLResourceOptions)[self hazardTrackingMode] << MTLResourceHazardTrackingModeShift); }
- (BOOL)isAliasable { @synchronized (self) { return _aliased; } }
- (void)makeAliasable {
    NVMTLHeap *h = nil; NSUInteger c = 0, o = 0; BOOL p = NO;
    @synchronized (self) {
        if (!_subHeap || _aliased) return;
        h = _subHeap; c = _subCharge; _subCharge = 0; _aliased = YES;
        p = _subPlaced; o = _heapOffset; _subPlaced = NO;
    }
    if (c) { if (p) [h nvmtlGiveRange:o length:c]; [h nvmtlReleaseSubAllocation:c]; }
}
- (id<MTLBuffer>)remoteStorageBuffer { return nil; }
- (id<MTLBuffer>)newRemoteBufferViewForDevice:(id<MTLDevice>)d { return nil; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)t { return nvmtl_owner_check(t); }
- (uint64_t)gpuAddress {   // a Metal 4 argument table resolves addresses back to buffers (only while Metal 4 is on)
    uint64_t a = nvmtl_vk_buffer_address(&_b);
    if (nvmtl_m4_enabled()) {
        const uint64_t len = [self length];
        if (!nvmtl_m4_note_current(_m4note, a, len)) nvmtl_m4_note_buffer_cached(self, a, len, _m4note);
    }
    return a;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s {
    if (_heap || _subHeap || _hostPtr) return MTLPurgeableStateNonVolatile;
    MTLPurgeableState prev = nvmtl_purge(&_purge, s);
    nvmtl_purge_track(self, s);
    return prev;
}
- (void)addDebugMarker:(NSString *)m range:(NSRange)r {}
- (void)removeAllDebugMarkers {}
void nvmtl_vk_buffer_retire(nvk_buffer *b);
- (void)dealloc {
    if (_b.buf && nvmtl_trace_on()) nvtrace("BUFFREE 0x%llx len %zu", (unsigned long long)nvmtl_vk_buffer_address(&_b), _b.size);
    for (uint32_t k = 0; k < _nbpk; k++) nvmtl_vk_image_destroy(&_bpk[k].img);
    free(_bpk); _bpk = NULL; _nbpk = 0;
    nvmtl_vk_buffer_retire(&_b); if (_shadow.buf) nvmtl_vk_buffer_retire(&_shadow);
    if (_subHeap) { NVMTLHeap *h = _subHeap; NSUInteger c = _subCharge; _subHeap = nil; _subCharge = 0;
                    if (_subPlaced && c) { _subPlaced = NO; [h nvmtlGiveRange:_heapOffset length:c]; }
                    [h nvmtlReleaseSubAllocation:c]; }
    if (_hostDealloc) { void (^d)(void *, NSUInteger) = _hostDealloc; _hostDealloc = nil;
                        d(_hostPtr ?: _b.map, (NSUInteger)(_hostLen ?: _b.size)); } }
- (MTLStorageMode)storageMode { return _storage; }
- (void *)contents { nvmtl_buf_settle(self);
    { static int dbg = -1; if (dbg < 0) dbg = getenv("NVMTL_CONTENTS_TRACE") != NULL;
      if (dbg && _b.imported) { static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 64) nvlog("contents: imported %zu-byte buffer %p (storage %d)", _b.size, (__bridge void *)self, (int)_storage); } }
    return _shadow.map ? _shadow.map : (_hostPtr ? _hostPtr : _b.map); }
- (id<MTLHeap>)heap { return _heap ?: _subHeap; }
- (NSUInteger)heapOffset { return _heapOffset; }
- (NSUInteger)length { return _b.size; }
- (void)didModifyRange:(NSRange)r {
    nvmtl_buf_settle(self);
    if (_shadow.map) {
        if (r.location >= _b.size) return;
        NSUInteger n = MIN(r.length, _b.size - r.location);
        if (n) nvmtl_managed_mark(self, r.location, r.location + n);
        return;
    }
    nvmtl_buffer_sync_in(self); }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
@end

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
static void nvmtl_surf_dirty(NVMTLCommandBuffer *cb, NVMTLTexture *t) {
    if (!cb || !t) return;
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    if (!cb->_surfDirty) cb->_surfDirty = [NSMutableSet new];
    [cb->_surfDirty addObject:r];
}
// A texture a kernel writes through a storage binding although NVK cannot make it a storage image - an IOSurface in a
// pitch-linear layout (ImageIO's RGBA16Unorm HDR / gain-map output), or a format NVK refuses linear storage for. The
// binding fell back to the dummy image and every write vanished: HDR photos decoded black. The kernel now writes a
// storage twin of the same VkFormat in optimal tiling, filled from the texture when this encoder first binds it and
// copied back after every dispatch (nvmtl_storage_twins_out), the IOSurface marked dirty as for a direct write.
static void nvmtl_retain_resource(__unsafe_unretained NVMTLCommandBuffer *cb, __unsafe_unretained id resource);
static void nvmtl_heap_register(NVMTLHeap *heap, id resource);
static void nvmtl_use_heap(NVMTLCommandBuffer *cb, id<MTLHeap> heap);
// `writes`: the shader declares a storage binding there. A texture made with MTLTextureUsageUnknown (0) may be used
// any way Metal allows - ImageIO makes its HDR output IOSurface textures that way and writes them.
static void *nvmtl_storage_view_for(NVMTLCommandBuffer *cb, NVMTLTexture *t, BOOL writes) {
    if (!t) return NULL;
    if (t->_stex) return [t nvview];
    const MTLTextureUsage usage = (MTLTextureUsage)t->_usage;   // not -usage, which answers ShaderRead for Unknown
    if (!writes || !cb || cb->_c.in_rp || t->_parent || (usage && !(usage & MTLTextureUsageShaderWrite))) return NULL;
    nvk_image *im = [t nvi];
    if (!im || !im->img || !im->w || !im->h || (im->mips ? im->mips : 1) > 1 || (im->layers ? im->layers : 1) > 1
        || im->mtl_type != 2 || (im->samples ? im->samples : 1) > 1 || im->bw > 1) return NULL;
    // Created once per texture; two command buffers may encode the same texture on different threads at once.
    @synchronized (t) {
        if (!t->_stTwin.img) {
            if (t->_stTwinTried) return NULL;
            t->_stTwinTried = YES;
            // An sRGB format cannot be a storage image in Vulkan at all. Its twin is half float, filled and written back by
            // blits that decode and encode sRGB, so a kernel writes linear values and the texture ends up sRGB-encoded,
            // as a write to an sRGB texture does on Apple GPUs (Blender's edit-mode overlay wrote RGBA8Unorm_sRGB).
            const int srgb = nvmtl_vk_srgb_format(im->fmt);
            if (nvmtl_vk_image_create_typed(im->w, im->h, srgb ? 97u : im->fmt, srgb ? 8u : im->bpp, 0, 1, 1, 2, 1, &t->_stTwin) || !t->_stTwin.storage) {
                if (t->_stTwin.img) nvmtl_vk_image_destroy(&t->_stTwin);
                memset(&t->_stTwin, 0, sizeof t->_stTwin);
                nvlog("storage twin: VkFormat %u %ux%u cannot be a storage image in optimal tiling either - shader writes to it are LOST", im->fmt, im->w, im->h);
                return NULL;
            }
            static int said; if (said++ < 4) nvlog("storage twin: %ux%u VkFormat %u%s is written through an optimal-tiled storage copy", im->w, im->h, im->fmt, t->_surf ? " (IOSurface)" : "");
        }
    }
    if (!cb->_stTwins) cb->_stTwins = [NSMutableSet new];
    if (![cb->_stTwins containsObject:t]) {
        if (t->_stTwin.fmt != im->fmt ? nvmtl_vk_cmd_blit_image_2d(&cb->_c, im, &t->_stTwin, im->w, im->h)
                                      : nvmtl_vk_cmd_copy_image_sub(&cb->_c, im, 0, 0, 0, 0, &t->_stTwin, 0, 0, 0, 0, im->w, im->h)) return NULL;
        [cb->_stTwins addObject:t];
        nvmtl_retain_resource(cb, t);
    }
    nvmtl_vk_cmd_prepare_sampled_image(&cb->_c, &t->_stTwin, 1);
    return t->_stTwin.view;
}
NSDictionary *nvmtl_function_reflection(NVMTLFunction *function);
static BOOL nvmtl_function_writes_texture(NVMTLFunction *fn, NSUInteger index) {
    NSArray *bindings = fn ? nvmtl_function_reflection(fn)[@"bindings"] : nil;
    if (![bindings isKindOfClass:[NSArray class]]) return NO;
    for (NSDictionary *b in bindings) {
        if (![b isKindOfClass:[NSDictionary class]] || [b[@"metal_index"] unsignedLongValue] != index) continue;
        NSString *kind = b[@"kind"], *access = b[@"access"];
        if (![kind isKindOfClass:[NSString class]] || [kind hasPrefix:@"Embedded"] || [kind isEqualToString:@"Buffer"] || [kind isEqualToString:@"Sampler"]
            || [kind isEqualToString:@"StaticSampler"]) continue;
        if ([kind isEqualToString:@"StorageImage"] || ([access isKindOfClass:[NSString class]] && ([access isEqualToString:@"Storage"] || [access isEqualToString:@"ReadWrite"] || [access isEqualToString:@"WriteOnly"]))) return YES;
    }
    return NO;
}
static void nvmtl_storage_twins_out(NVMTLCommandBuffer *cb) {
    for (NVMTLTexture *t in cb->_stTwins) {
        nvk_image *im = [t nvi];
        if (t->_stTwin.fmt != im->fmt ? nvmtl_vk_cmd_blit_image_2d(&cb->_c, &t->_stTwin, im, im->w, im->h)
                                      : nvmtl_vk_cmd_copy_image_sub(&cb->_c, &t->_stTwin, 0, 0, 0, 0, im, 0, 0, 0, 0, im->w, im->h)) {
            nvlog("storage twin: copy back to the %ux%u texture FAILED", im->w, im->h); continue; }
        nvmtl_vk_cmd_prepare_sampled_image(&cb->_c, &t->_stTwin, 1);
        nvmtl_surf_dirty(cb, t);
    }
}
static int g_trace_lines;
static int g_trace_bigonly, g_trace_mute;
static int nvmtl_trace_on_slow(void) {
#if NVMTL_RELEASE
    g_trace_hot = 0; g_trace_recheck = UINT64_MAX;
    return 0;   // the request files are refused in release builds (nvmtl_rel_access), so tracing never turns on
#endif
    static uint64_t next; static int on;
    uint64_t now = nvmtl_poll_ns();
    static int cap = 2500;
    // NVMTL_TRACE=<lines>: this process traces from the start (per-draw bindings, samplers ...) to
    // /tmp/nvmtl-trace-<pid>.txt, without the /tmp flag that every process (WindowServer too) watches
    static int envcap = -1;
    if (envcap < 0) { const char *e = getenv("NVMTL_TRACE"); envcap = e && *e ? (atoi(e) > 0 ? atoi(e) : 20000) : 0; }
    if (envcap) return !g_trace_mute && g_trace_lines < envcap;   // (g_trace_hot stays 1)
    if (now >= next) { next = now + 250000000ull; int was = on; on = nvmtl_diag_flag("trace-request");
        g_trace_bigonly = on && nvmtl_diag_flag("trace-big"); if (!g_trace_bigonly) g_trace_mute = 0;
        if (on && !was) { g_trace_lines = 0; cap = nvmtl_diag_flag("trace-huge") ? 600000 : nvmtl_diag_flag("trace-long") ? 20000 : 2500; } }
    g_trace_recheck = next; g_trace_hot = on;
    return on && !g_trace_mute && g_trace_lines < cap;
}
static int nvtrace_scope(uint32_t w, uint32_t h) { int was = g_trace_mute; g_trace_mute = 0; (void)nvmtl_trace_on();
    g_trace_mute = g_trace_bigonly && (w < 1000 || h < 100); return was; }
static void nvtrace_emit(const char *fmt, ...) {
    if (!nvmtl_trace_on()) return;
    char p[96]; snprintf(p, sizeof p, "/tmp/nvmtl-trace-%d.txt", (int)getpid());
    int fd = nvmtl_diag_open(p, 1);
    if (fd < 0) { static int said; if (!said++) nvlog("G15 trace: cannot open %s (errno %d)", p, errno); return; }
    char b[700]; va_list a; va_start(a, fmt); int n = vsnprintf(b, sizeof b - 1, fmt, a); va_end(a);
    if (n < 0) n = 0; if (n > (int)sizeof b - 2) n = (int)sizeof b - 2;
    b[n++] = '\n'; if (write(fd, b, (size_t)n) != n) { static int said2; if (!said2++) nvlog("G15 trace: short write (errno %d)", errno); }
    close(fd); g_trace_lines++;
}
static void nvtrace_floats(const char *what, unsigned long idx, unsigned long off, const void *map, int nf) {
    if (!nvmtl_trace_on()) return;
    if (!map) { nvtrace("%s[%lu] off %lu (not CPU-visible)", what, idx, off); return; }
    const float *f = (const float *)((const char *)map + off); char s[420]; int k = 0;
    for (int i = 0; i < nf && k < (int)sizeof s - 16; i++) k += snprintf(s + k, sizeof s - (size_t)k, " %.3g", f[i]);
    nvtrace("%s[%lu] off %lu:%s", what, idx, off, s);
}
static void nvmtl_nonzero(const void *base, size_t row, size_t rowValid, uint32_t h, unsigned *nz, unsigned *n) {
    *nz = *n = 0; if (!base || !rowValid || !h) return;
    size_t take = rowValid < 256 ? rowValid : 256; uint32_t step = h > 16 ? h / 16 : 1;
    for (uint32_t y = 0; y < h; y += step) { const unsigned char *p = (const unsigned char *)base + (size_t)y * row;
        for (size_t x = 0; x < take; x++) { *nz += p[x] != 0; (*n)++; } }
}
// A full overwrite of a single 8-bit RGBA/BGRA subresource cannot consume the previous CPU contents.
// Keep all partial updates, views, arrays, volumes, compressed and multisample images
// on SurfaceIn: their untouched bytes still have to be preserved.
static BOOL nvmtl_surface_full_overwrite(NVMTLTexture *t, MTLRegion r, NSUInteger level, NSUInteger slice) {
    if (!t || !t->_surf || t->_parent || t->_viewParent || t->_tbView || t->_baseLevel || t->_baseSlice || level || slice)
        return NO;
    const nvk_image *i = &t->_i;
    if (i->bpp != 4 || (t->_fmt != MTLPixelFormatBGRA8Unorm && t->_fmt != MTLPixelFormatBGRA8Unorm_sRGB
        && t->_fmt != MTLPixelFormatRGBA8Unorm && t->_fmt != MTLPixelFormatRGBA8Unorm_sRGB)) return NO;
    return i->mtl_type == MTLTextureType2D && i->mips == 1 && i->layers == 1 && i->samples <= 1
        && i->bw <= 1 && i->bh <= 1 && !r.origin.x && !r.origin.y && !r.origin.z
        && r.size.width == i->w && r.size.height == i->h && r.size.depth == 1;
}
// A recorded overwrite only defines contents for subsequent commands in this
// command buffer. Do not promote global Seed/generation state before it executes:
// an abandoned command buffer must leave SurfaceIn able to restore CPU contents.
static void nvmtl_surface_overwrite_recorded(NVMTLCommandBuffer *cb, NVMTLTexture *t) {
    if (!cb || !t) return;
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    if (!cb->_surfOverwrites) cb->_surfOverwrites = [NSMutableSet new];
    [cb->_surfOverwrites addObject:r];
}
/* SURFCENSUS (/tmp/nvmtl-surfcensus, gpuperf): which IOSurfaces this process copies out to system memory, how often, and
   whether its own GPU reads them again (sampled, blitted from, loaded as an attachment), with what the IOSurface says of
   itself (use counts, in use by any process, its values). Logged every 3 s while the flag is there. */
#if !NVMTL_RELEASE
static struct { uint32_t sid, w, h, fmt; uint64_t outs, reads, inuse, usecnt; char vals[160]; } g_sc[32];
static unsigned g_nsc; static uint64_t g_sc_t0; static os_unfair_lock g_sc_lk = OS_UNFAIR_LOCK_INIT;
static void nvmtl_surfcensus(NVMTLTexture *r, int out) {
    if (!r || !r->_surf || !nvmtl_diag_flag("surfcensus")) return;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    os_unfair_lock_lock(&g_sc_lk);
    if (!g_sc_t0) g_sc_t0 = now;
    unsigned i = 0; while (i < g_nsc && g_sc[i].sid != sid) i++;
    if (i == g_nsc && g_nsc < 32) { memset(&g_sc[i], 0, sizeof g_sc[i]); g_sc[i].sid = sid; g_sc[i].w = (uint32_t)IOSurfaceGetWidth(r->_surf);
        g_sc[i].h = (uint32_t)IOSurfaceGetHeight(r->_surf); g_sc[i].fmt = (uint32_t)IOSurfaceGetPixelFormat(r->_surf); g_nsc++;
        CFDictionaryRef d = IOSurfaceCopyAllValues(r->_surf);
        if (d) { const CFIndex n = CFDictionaryGetCount(d); const void *k[16]; CFIndex m = n < 16 ? n : 16; CFDictionaryGetKeysAndValues(d, k, NULL);
            int o = 0; for (CFIndex j = 0; j < m && o < 150; j++) { char b[64] = "?"; if (CFGetTypeID(k[j]) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)k[j], b, sizeof b, kCFStringEncodingUTF8);
                o += snprintf(g_sc[i].vals + o, sizeof g_sc[i].vals - (size_t)o, "%s%s", o ? "," : "", b); }
            CFRelease(d); } }
    if (i < g_nsc) {
        if (out) { g_sc[i].outs++; g_sc[i].inuse += IOSurfaceIsInUse(r->_surf) ? 1 : 0; g_sc[i].usecnt += (uint64_t)IOSurfaceGetUseCount(r->_surf); }
        else g_sc[i].reads++;
    }
    if (now - g_sc_t0 >= 3000000000ull) {
        const double sec = (now - g_sc_t0) / 1e9;
        for (unsigned j = 0; j < g_nsc; j++)
            nvlog_keep("SURFCENSUS %.1f s: surface %u %ux%u fmt '%c%c%c%c': copied out %.1f/s, read by this GPU %.1f/s, in use (any process) at %.0f%% of copy-outs, local use count %.2f avg, values [%s]",
                       sec, g_sc[j].sid, g_sc[j].w, g_sc[j].h, (char)(g_sc[j].fmt >> 24), (char)(g_sc[j].fmt >> 16), (char)(g_sc[j].fmt >> 8), (char)g_sc[j].fmt,
                       g_sc[j].outs / sec, g_sc[j].reads / sec, g_sc[j].outs ? 100.0 * g_sc[j].inuse / g_sc[j].outs : 0.0,
                       g_sc[j].outs ? (double)g_sc[j].usecnt / g_sc[j].outs : 0.0, g_sc[j].vals);
        g_nsc = 0; g_sc_t0 = now;
    }
    os_unfair_lock_unlock(&g_sc_lk);
}
#else
static inline void nvmtl_surfcensus(NVMTLTexture *r, int out) { (void)r, (void)out; }
#endif
static BOOL nvmtl_surface_pending_write(NVMTLCommandBuffer *cb, NVMTLTexture *t);
// The GPU reads t's surface into another resource (a sampled or read-only binding, a texture-to-texture copy source):
// see nvmtl_ws_note_captures. Not for render-target loads or copy destinations (writes) or copies into a buffer (read back
// for the CPU).
// Kept per IOSurface, not per texture: WindowServer writes a surface through one MTLTexture and samples it through
// another. A small table; a collision only costs a surface its trust (it waits, as before).
enum { kWsSurfSlots = 64 };
// per surface: GPU reads and writes counted in 500 ms windows (the last full window and the current one), and the
// accelerator resource made for it (kres, below)
static struct { uint32_t id; uint32_t reads, writes, prevReads, prevWrites; uint64_t t0, lastWrite; CFTypeRef kres; } g_ws_surf[kWsSurfSlots];
static os_unfair_lock g_ws_surf_lk = OS_UNFAIR_LOCK_INIT;
#define NVMTL_WS_WINDOW (500 * NSEC_PER_MSEC)
static __typeof__(g_ws_surf[0]) *nvmtl_ws_slot(uint32_t id, uint64_t now, CFTypeRef *drop) {   // g_ws_surf_lk held
    __typeof__(g_ws_surf[0]) *e = &g_ws_surf[id % kWsSurfSlots];
    if (e->id != id) { if (e->kres && drop) *drop = e->kres; memset(e, 0, sizeof *e); e->id = id; e->t0 = now; }
    if (now - e->t0 >= NVMTL_WS_WINDOW) {
        const BOOL stale = now - e->t0 >= 2 * NVMTL_WS_WINDOW;   // a whole window without use: nothing carries over
        e->prevReads = stale ? 0 : e->reads; e->prevWrites = stale ? 0 : e->writes;
        e->reads = e->writes = 0; e->t0 = now;
    }
    return e;
}
static inline void nvmtl_ws_sampled(__unsafe_unretained NVMTLTexture *t) {
    if (!t) return;
    __unsafe_unretained NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    const uint32_t id = (uint32_t)IOSurfaceGetID(r->_surf);
    CFTypeRef drop = NULL;
    os_unfair_lock_lock(&g_ws_surf_lk);
    __typeof__(g_ws_surf[0]) *e = nvmtl_ws_slot(id, clock_gettime_nsec_np(CLOCK_UPTIME_RAW), &drop);
    e->reads++;
    os_unfair_lock_unlock(&g_ws_surf_lk);
    if (drop) CFRelease(drop);
}
// A write of surface id (counted); whether the GPU writes and samples it at display rate: at least `n` of each within
// one 500 ms window (this one or the last)
static BOOL nvmtl_ws_written_streaming(uint32_t id, uint32_t n) {
    CFTypeRef drop = NULL;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    os_unfair_lock_lock(&g_ws_surf_lk);
    __typeof__(g_ws_surf[0]) *e = nvmtl_ws_slot(id, now, &drop);
    e->writes++; e->lastWrite = now;
    const BOOL ok = (e->reads >= n && e->writes >= n) || (e->prevReads >= n && e->prevWrites >= n);
    os_unfair_lock_unlock(&g_ws_surf_lk);
    if (drop) CFRelease(drop);
    return ok;
}
// The same test without counting a write, and whether its accelerator resource exists (nvmtl_scanout_get)
static BOOL nvmtl_ws_is_streaming_with_resource(uint32_t id, uint32_t n) {
    os_unfair_lock_lock(&g_ws_surf_lk);
    const __typeof__(g_ws_surf[0]) e = g_ws_surf[id % kWsSurfSlots];
    os_unfair_lock_unlock(&g_ws_surf_lk);
    return e.id == id && e.kres && ((e.reads >= n && e.writes >= n) || (e.prevReads >= n && e.prevWrites >= n));
}
// The accelerator resource of t's surface, made once (Metal's own MTLIOAccelTexture, as Apple's drivers make one for
// every IOSurface texture). With it a CPU lock of the surface - a screenshot of a scaled mode reads the composite itself -
// reaches NVResource::pageoff, which waits for the pending mark this plugin keeps on the surface while the GPU writes it
// (NMXSurfaceWaitAll), so the flush need not wait. Without one (it could not be made) the flush keeps waiting. The
// resource holds the surface, so one not written for 2 s is released.
static BOOL nvmtl_ws_kernel_resource(__unsafe_unretained NVMTLTexture *t) {
    __unsafe_unretained NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return NO;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    CFTypeRef idle[kWsSurfSlots]; unsigned nIdle = 0; BOOL have;
    os_unfair_lock_lock(&g_ws_surf_lk);
    have = g_ws_surf[sid % kWsSurfSlots].id == sid && g_ws_surf[sid % kWsSurfSlots].kres;
    if (!have)   // a new one is about to be made: let the idle ones go
        for (unsigned k = 0; k < kWsSurfSlots; k++)
            if (g_ws_surf[k].kres && g_ws_surf[k].id != sid && now - g_ws_surf[k].lastWrite > 2 * NSEC_PER_SEC) { idle[nIdle++] = g_ws_surf[k].kres; g_ws_surf[k].kres = NULL; }
    os_unfair_lock_unlock(&g_ws_surf_lk);
    for (unsigned k = 0; k < nIdle; k++) CFRelease(idle[k]);
    if (have) return YES;
    static Class T; static SEL sel; static dispatch_once_t once;
    dispatch_once(&once, ^{ T = objc_getClass("MTLIOAccelTexture"); sel = NSSelectorFromString(@"initWithDevice:descriptor:iosurface:plane:field:args:argsSize:"); });
    id dev = gNVMTLMainDevice;
    if (!T || !dev || ![T instancesRespondToSelector:sel]) return NO;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:r.pixelFormat width:IOSurfaceGetWidthOfPlane(r->_surf, r->_plane)
                                                                                height:IOSurfaceGetHeightOfPlane(r->_surf, r->_plane) mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
    static uint8_t args[2440];   // zero: no vendor data (the IOSurface is the resource's whole description)
    id (*mk)(id, SEL, id, id, IOSurfaceRef, unsigned, unsigned, void *, unsigned) = (void *)objc_msgSend;
    id k = mk([T alloc], sel, dev, d, r->_surf, (unsigned)r->_plane, 0, args, (unsigned)sizeof args);
    static _Atomic unsigned said;
    if (atomic_fetch_add(&said, 1) < 6) nvlog("captures: surface %u %zux%zu: accelerator resource %s", sid, IOSurfaceGetWidth(r->_surf), IOSurfaceGetHeight(r->_surf), k ? "made (a CPU lock waits for its GPU writes)" : "could NOT be made - its flushes keep waiting");
    if (!k) return NO;
    CFTypeRef keep = CFBridgingRetain(k), drop = NULL;
    os_unfair_lock_lock(&g_ws_surf_lk);
    __typeof__(g_ws_surf[0]) *e = nvmtl_ws_slot(sid, now, &drop);
    if (e->kres) { if (!drop) drop = keep; else CFRelease(keep); } else e->kres = keep;
    os_unfair_lock_unlock(&g_ws_surf_lk);
    if (drop) CFRelease(drop);
    return YES;
}
static void nvmtl_surface_in_for_cb(__unsafe_unretained NVMTLCommandBuffer *cb, __unsafe_unretained NVMTLTexture *t) {
    if (!t) return;
    __unsafe_unretained NVMTLTexture *r = t->_parent ? t->_parent : t;
    // everything below is for a texture on an IOSurface (VRAM sharing, the census, overwrites, pending writes, the copy in):
    // a texture of the process's own returns here (WAS: each step tested _surf itself, after retains and a message send)
    if (!r->_surf) return;
    // B580 VRAM-shared: this command buffer reads the one copy another process may be writing: it waits for the surface's
    // writers on the GPU and is attached as a reader (a writer that comes later waits for it)
    if (cb && r->_surf && r->_vramOn && nvmtl_vk_anv_vram()) (void)nvmtl_vk_cmd_surface_use(&cb->_c, (uint32_t)IOSurfaceGetID(r->_surf), 0);
    nvmtl_surfcensus(r, 0);
    if (cb && r->_surf && [cb->_surfOverwrites containsObject:r]) return;
    // A prior committed buffer defines this GPU backing's contents.
    // Importing the old CPU surface here would run ahead of this buffer and
    // overwrite that earlier GPU write. Queue order or the application's event supplies the dependency.
    if (nvmtl_surface_pending_write(cb, r)) return;
    [t nvmtlSurfaceIn];
}
static unsigned nvmtl_surf_id(NVMTLTexture *t) { NVMTLTexture *r = (t && t->_parent) ? t->_parent : t; return (r && r->_surf) ? (unsigned)IOSurfaceGetID(r->_surf) : 0u; }
static int nvmtl_noseed(void) { static uint64_t next; static int on; uint64_t now = nvmtl_poll_ns();
    if (now >= next) { next = now + 250000000ull; int was = on; on = nvmtl_diag_flag("noseed");
        if (on && !was) nvlog("G17 EXPERIMENT ON: surface-out writes under a read-only lock, the IOSurface seed does not move");
        if (!on && was) nvlog("G17 experiment off: surface-out bumps the seed again"); }
    return on; }
static void nvmtl_census(const char *what, uint32_t w, uint32_t h, int surf) {
    static struct { const char *what; uint32_t w, h; int surf; } seen[48]; static int n; static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;
    if (__atomic_load_n(&n, __ATOMIC_RELAXED) == 48) return;   // full: every later pass skips the lock
    os_unfair_lock_lock(&lk);
    for (int i = 0; i < n; i++) if (seen[i].what == what && seen[i].w == w && seen[i].h == h && seen[i].surf == surf) { os_unfair_lock_unlock(&lk); return; }
    if (n == 48) { os_unfair_lock_unlock(&lk); return; }
    seen[n].what = what; seen[n].w = w; seen[n].h = h; seen[n].surf = surf; __atomic_store_n(&n, n + 1, __ATOMIC_RELAXED);
    os_unfair_lock_unlock(&lk);
    nvlog("G14 census: %s %ux%u iosurface=%d (first time in this process)", what, w, h, surf);
}
static void nvmtl_frame_dump(const char *tag, const void *base, size_t row, uint32_t w, uint32_t h);
/* perf2: the dump-request flag, polled every 250 ms, shared by the per-bind dump hooks (each kept a poll of its own: three
   clock reads and compares per texture bind) */
static int nvmtl_dump_request_on(void) {
    static uint64_t next; static int on;
    const uint64_t now = nvmtl_poll_ns();
    if (__builtin_expect(now >= next, 0)) { next = now + 250000000ull; on = nvmtl_diag_flag("dump-request"); }
    return on;
}
static void nvmtl_surface_cpu_dump(__unsafe_unretained NVMTLTexture *rt) {
    if (!nvmtl_dump_request_on() || !rt || !rt->_surf || IOSurfaceGetBytesPerElement(rt->_surf) != 4 || IOSurfaceGetPlaneCount(rt->_surf) > 1) return;
    if (IOSurfaceLock(rt->_surf, kIOSurfaceLockReadOnly, NULL) != kIOReturnSuccess) { static int said; if (!said++) nvlog("G16 cpu dump: IOSurfaceLock failed"); return; }
    char tag[24]; snprintf(tag, sizeof tag, "cpu%u-", (unsigned)IOSurfaceGetID(rt->_surf));
    nvmtl_frame_dump(tag, IOSurfaceGetBaseAddress(rt->_surf), IOSurfaceGetBytesPerRow(rt->_surf), (uint32_t)IOSurfaceGetWidth(rt->_surf), (uint32_t)IOSurfaceGetHeight(rt->_surf));
    IOSurfaceUnlock(rt->_surf, kIOSurfaceLockReadOnly, NULL);
}
static void nvmtl_frame_dump(const char *tag, const void *base, size_t row, uint32_t w, uint32_t h) {
    if ((uint64_t)w * h < 1024 || !nvmtl_diag_flag("dump-request")) return;
    static struct { char tag[24]; uint32_t w, h; uint64_t last; } slot[32]; static int nslot;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW); int k, go = 0;
    @synchronized([NSObject class]) {
        for (k = 0; k < nslot; k++) if (slot[k].w == w && slot[k].h == h && !strncmp(slot[k].tag, tag, sizeof slot[k].tag - 1)) break;
        if (k == nslot && nslot < 32) { snprintf(slot[k].tag, sizeof slot[k].tag, "%s", tag); slot[k].w = w; slot[k].h = h; slot[k].last = 0; nslot++; }
        if (k < nslot && (!slot[k].last || now - slot[k].last >= 2000000000ull)) { slot[k].last = now; go = 1; }
    }
    if (!go) return;
    char dst[400], tmp[416]; const char *dir = "/tmp"; int fd = -1;
    for (int pass = 0; pass < 2 && fd < 0; pass++) {
        if (pass) { const char *t = getenv("TMPDIR"); if (!t || !*t || (errno != EPERM && errno != EACCES)) break; dir = t; }
        size_t dl = strlen(dir); const char *sep = (dl && dir[dl - 1] == '/') ? "" : "/";
        snprintf(dst, sizeof dst, "%s%snvmtl-frame-%d-%.23s%ux%u.bgra", dir, sep, (int)getpid(), tag, w, h); snprintf(tmp, sizeof tmp, "%s.tmp", dst);
        fd = nvmtl_diag_open(tmp, 0);
    }
    if (fd < 0) { static int said; if (!said++) nvlog("G13 frame dump: cannot open %s (errno %d)", tmp, errno); return; }
    uint32_t hdr[4] = { 0x3146564e , w, h, (uint32_t)row };
    int ok = write(fd, hdr, sizeof hdr) == (ssize_t)sizeof hdr && write(fd, base, row * h) == (ssize_t)(row * h);
    close(fd);
    if (ok && rename(tmp, dst) == 0) { static int told; if (!told++) nvlog("G13 frame dump: %ux%u written to %s", w, h, dst); }
    else { unlink(tmp); static int said2; if (!said2++) nvlog("G13 frame dump: write FAILED (errno %d)", errno); }
}
// Diagnostic (Dock icons point-sampled, 2026-10-28): with /tmp/nvmtl-dump-request, a mipmapped 8-bit texture of 256 px or
// less that a draw samples has its levels 0 and 1 written out (nvmtl-frame-<pid>-lv<L>-<id>-<w>x<h>.bgra, each texture
// once): a level 1 that is a 2:1 point decimation of level 0 rather than its 2x2 average shows who wrote the levels.
static void nvmtl_mip_dump(__unsafe_unretained NVMTLTexture *t) {
    if (!nvmtl_dump_request_on()) return;   // perf2: (WAS: -nvi asked first, on every texture bind)
    nvk_image *i = [t nvi];
    if (t->_parent || t->_surf || !i || i->mips < 2 || i->bpp != 4 || i->w > 256 || i->h > 256 || i->w < 16 || i->mtl_type != 2) return;
    static NSHashTable *done; static os_unfair_lock dl = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&dl); if (!done) done = [NSHashTable weakObjectsHashTable];
    const BOOL fresh = ![done containsObject:t] && done.count < 12; if (fresh) [done addObject:t]; os_unfair_lock_unlock(&dl);
    if (!fresh) return;
    for (NSUInteger l = 0; l < 2; l++) {
        const uint32_t w = i->w >> l, h = i->h >> l; const size_t row = (size_t)w * 4; void *px = calloc(row, h);
        if (!px) return;
        [t getBytes:px bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:l];
        char tag[24]; snprintf(tag, sizeof tag, "lv%lu-%05lx-", (unsigned long)l, (unsigned long)(((uintptr_t)(__bridge void *)t >> 4) & 0xfffff));
        nvmtl_frame_dump(tag, px, row, w, h); free(px);
    }
    nvlog("mip dump: %ux%u texture %p (%u levels, mipsWritten %d) levels 0 and 1 written", i->w, i->h, (__bridge void *)t, i->mips, (int)t->_mipsWritten);
}
// Diagnostic (Dock icons): with /tmp/nvmtl-dump-request, the next pipeline that draws after a 128x128 mipmapped texture is
// bound at fragment index 3 has its fragment module, function constants and constexpr sampler slots written to
// /tmp/nvmtl-psodump-<pid>-<n>.spv / .json (up to 6 pipelines; its own files, not the frame-dump table).
static __thread int t_pso_dump;
static NSArray<NSArray *> *nvmtl_static_samplers(NVMTLFunction *fn);   // NVMTLEmbedded.m
static int nvmtl_pso_dump_on(void) { return nvmtl_dump_request_on(); }
static void nvmtl_pso_dump(id ps, NVMTLFunction *ff, NVMTLFunction *used) {
    static void *seen[6]; static int n; static os_unfair_lock l = OS_UNFAIR_LOCK_INIT;
    os_unfair_lock_lock(&l); int k = -1; BOOL dup = NO;
    for (int i = 0; i < n; i++) if (seen[i] == (__bridge void *)ps) dup = YES;
    if (!dup && n < 6) { seen[n] = (__bridge void *)ps; k = n++; }
    os_unfair_lock_unlock(&l);
    if (k < 0 || !ff) return;
    char p[128]; snprintf(p, sizeof p, "/tmp/nvmtl-psodump-%d-%d", (int)getpid(), k);
    NSData *spv = used && used->_spirv ? used->_spirv : ff->_spirv;
    [spv writeToFile:[NSString stringWithFormat:@"%s.spv", p] atomically:YES];
    NSMutableArray *ss = [NSMutableArray new];
    for (NSArray *pair in nvmtl_static_samplers(ff)) { NVMTLSamplerState *s = pair[1];
        [ss addObject:@{ @"slot": pair[0], @"min": @(s->_s.desc.min_filter), @"mag": @(s->_s.desc.mag_filter), @"mip": @(s->_s.desc.mip_filter), @"address_s": @(s->_s.desc.address_s) }]; }
    NSMutableDictionary *fc = [NSMutableDictionary new];
    for (id key in ([ff->_fc isKindOfClass:[NSDictionary class]] ? ff->_fc : @{})) fc[[key description]] = [ff->_fc[key] description];
    NSDictionary *j = @{ @"function": ff->_fname ?: @"?", @"spec_name": ff->_specName ?: @"", @"variant": @(used != nil && used != ff), @"spirv_bytes": @(spv.length),
                         @"function_constants": fc, @"static_samplers": ss };
    [[NSJSONSerialization dataWithJSONObject:j options:NSJSONWritingPrettyPrinted error:NULL] writeToFile:[NSString stringWithFormat:@"%s.json", p] atomically:YES];
    nvlog("pso dump: %s (%lu B of SPIR-V, %lu function constants) -> %s.spv/.json", ff->_fname.UTF8String, (unsigned long)spv.length, (unsigned long)fc.count, p);
}
// The app bound sampler ns at slot i of set: remembered, so a slot a constexpr sampler takes for one pipeline can be given
// back (nvmtl_bind_embedded). The app's binding is current again, whatever a constexpr sampler did to the slot before.
static inline void nvmtl_note_app_sampler(__unsafe_unretained NVMTLCommandBuffer *cb, uint32_t set, NSUInteger i, __unsafe_unretained NVMTLSamplerState *ns) {
    if (!cb || set > 1 || i >= 16) return;
    cb->_appSamp[set][i] = ns; cb->_staticSamp[set] &= (uint16_t)~(1u << i);
}
static void nvmtl_blit_dump(NVMTLTexture *d) {
    nvk_image *i = [d nvi]; if (!i || i->w < 1000 || i->h < 100 || i->bpp != 4) return;
    static uint64_t last; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (!nvmtl_diag_flag("dump-request") || (last && now - last < 2000000000ull)) return;
    last = now; size_t row = (size_t)i->w * 4; void *px = malloc(row * i->h);
    if (!px) { nvlog("G17 blit dump: malloc FAILED"); return; }
    if (nvmtl_vk_image_read(d->_q, i, px, row)) nvlog("G17 blit dump: readback FAILED");
    else { char tag[24]; snprintf(tag, sizeof tag, "blit%u-", nvmtl_surf_id(d)); nvmtl_frame_dump(tag, px, row, i->w, i->h); }
    free(px);
}
#pragma mark - texture
static _Atomic unsigned long long nvmtl_tex_objs_made, nvmtl_tex_objs_gone;
unsigned long long nvmtl_tex_objs_live(void) {
    unsigned long long made = nvmtl_tex_objs_made, gone = nvmtl_tex_objs_gone;
    return made > gone ? made - gone : 0;
}
unsigned long long nvmtl_tex_objs_released(void) { return nvmtl_tex_objs_gone; }
@implementation NVMTLTexture
+ (instancetype)alloc { nvmtl_tex_objs_made++; return [super alloc]; }
- (void)setResponsibleProcess:(int)pid { static int said; if (!said++) nvlog("setResponsibleProcess: pid %d on a texture - per-process attribution is not kept; nothing to set (said once)", pid); }
- (unsigned long long)protectionOptions { return 0; }
- (NSUInteger)allocatedSize { nvk_image *i = [self nvi]; if (i->alloc) return (NSUInteger)i->alloc;
    NSUInteger l = i->layers ? i->layers : 1, m = i->mips ? i->mips : 1, bpp = i->bpp ? i->bpp : 4, bw = i->bw ? i->bw : 1, bh = i->bh ? i->bh : 1, tot = 0;
    for (NSUInteger k = 0; k < m; k++) { NSUInteger w = i->w >> k ? i->w >> k : 1, h = i->h >> k ? i->h >> k : 1; tot += ((w + bw - 1) / bw) * ((h + bh - 1) / bh) * bpp; }
    return tot * l; }
- (MTLResourceID)gpuResourceID {
    nvmtl_heap_register((NVMTLHeap *)self.heap, self);
    @synchronized (self) { if (!_resid) _resid = nvmtl_resid_for_texture(self); }
    MTLResourceID rid; rid._impl = _resid; return rid;
}
extern int nvmtl_depthfmt_public(MTLPixelFormat, uint32_t *, uint32_t *);
- (nvk_image *)nvi { return _parent ? [_parent nvi] : &_i; }
- (void *)nvview {
    nvk_image *image = [self nvi];
    if (image->fmt == VK_FORMAT_D16_UNORM_S8_UINT || image->fmt == VK_FORMAT_D24_UNORM_S8_UINT || image->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT) {
        uint32_t format = 0, aspect = 0;
        nvmtl_depthfmt_public(self.pixelFormat, &format, &aspect);
        if (aspect != VK_IMAGE_ASPECT_STENCIL_BIT) {
            @synchronized(self) {
                if (!_sampledDepthView && nvmtl_vk_image_view_create_range_aspect(image, image->fmt, 0, VK_IMAGE_ASPECT_DEPTH_BIT, _baseLevel, (uint32_t)self.mipmapLevelCount, _baseSlice, _parent ? _viewSlices : image->layers, (uint32_t)self.textureType, &_sampledDepthView)) return NULL;
                return _sampledDepthView;
            }
        }
    }
    return _ownView ? _ownView : image->view;
}
- (void *)nvsampleview { return _swzView ? _swzView : [self nvview]; }
/* perf2: the views the texture binds take, without a message send (setFragmentTexture spent ~30% of its time in
   objc_msgSend on -nvi / -nvview / -nvsampleview / -usage, gl_bind profile 2026-10-30). The same answers as those methods;
   a depth+stencil image (its depth-only sampled view is made on first use under a lock) still asks -nvview. */
static inline nvk_image *nvmtl_tex_nvi(__unsafe_unretained NVMTLTexture *t) {
    while (t->_parent) t = t->_parent;
    return &t->_i;
}
static inline void *nvmtl_tex_view(__unsafe_unretained NVMTLTexture *t) {
    nvk_image *image = nvmtl_tex_nvi(t);
    if (image->fmt == VK_FORMAT_D16_UNORM_S8_UINT || image->fmt == VK_FORMAT_D24_UNORM_S8_UINT || image->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
        return [t nvview];
    return t->_ownView ? t->_ownView : image->view;
}
static inline void *nvmtl_tex_sampleview(__unsafe_unretained NVMTLTexture *t) { return t->_swzView ? t->_swzView : nvmtl_tex_view(t); }
static inline MTLTextureUsage nvmtl_tex_usage(__unsafe_unretained NVMTLTexture *t) { return t->_usage ? (MTLTextureUsage)t->_usage : t.usage; }
/* MTLTextureDescriptor.swizzle applies when the texture is sampled or read, not to attachments or shader writes, so it
 * gets a view of its own for the sampled binds and the identity view stays for everything else. IconServices hands
 * gray + alpha image layers over as RG8 with swizzle (r, r, r, g); with the swizzle dropped, alpha landed in green. */
void nvmtl_texture_desc_swizzle(NVMTLTexture *t, MTLTextureDescriptor *d) {
    if (!t || !d || ![d respondsToSelector:@selector(swizzle)]) return;
    const MTLTextureSwizzleChannels sw = d.swizzle;
    if (sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha) return;
    t->_swz = sw; t->_hasSwz = YES;
    if (d.textureType == MTLTextureTypeTextureBuffer || d.sampleCount > 1 || t->_tbView) {
        nvlog("texture swizzle %u,%u,%u,%u on texture type %lu (samples %lu) is not applied", sw.red, sw.green, sw.blue, sw.alpha,
              (unsigned long)d.textureType, (unsigned long)d.sampleCount); return; }
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    const uint8_t s4[4] = { (uint8_t)sw.red, (uint8_t)sw.green, (uint8_t)sw.blue, (uint8_t)sw.alpha };
    if (nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8) ||
        nvmtl_vk_image_view_create_range_swz([t nvi], vkf, a8, 0, 0, 0, 0, (uint32_t)d.textureType, s4, &t->_swzView)) {
        t->_swzView = NULL;
        nvlog("texture swizzle FAILED: no swizzled view for format %lu - samples keep the identity mapping", (unsigned long)d.pixelFormat); return; }
    { static int said; if (said++ < 8) nvlog("texture swizzle: format %lu samples r,g,b,a <- %u,%u,%u,%u", (unsigned long)d.pixelFormat, s4[0], s4[1], s4[2], s4[3]); }
}
- (id<MTLHeap>)heap { return _heap ?: _subHeap; }
- (NSUInteger)heapOffset { return _heapOffset; }
- (void)dealloc {
    if (_sampledDepthView) nvmtl_vk_image_view_destroy(_sampledDepthView);
    nvmtl_tex_objs_gone++;
    if (_tbView || _tbAlias) { nvmtl_vk_texel_view_destroy(_tbAlias, _tbView); _tbView = _tbAlias = NULL; }
    if (_resid) nvmtl_resid_texture_gone(_resid);
    if (_ownView) nvmtl_vk_image_view_destroy(_ownView);
    if (_swzView) nvmtl_vk_image_view_destroy(_swzView);
    if (!_parent && _parkable && _subHeap && _i.img) nvmtl_heap_park(_subHeap, &_i, _pkey, _heapOffset, _pkLen);
    if (!_parent && _bparkable && _backBuf && _i.img) nvmtl_bpark_put(_backBuf, &_i, _pkey);
    // svfix2: a VRAM-shared wrapper's image and imported backing live until every submission made so far has finished
    if (!_parent && _vramOn && _i.img && nvmtl_vk_defer_image_free(&_i, &_vramBuf)) _vramOn = NO;
    if (!_parent) nvmtl_vk_image_destroy(&_i);
    if (_stTwin.img) nvmtl_vk_image_destroy(&_stTwin);
    if (!_parent && _vramOn) { nvmtl_vk_buffer_destroy(&_vramBuf); _vramOn = NO; }
    if (_surfOwned && _surf) CFRelease(_surf);
    if (_subHeap) { NVMTLHeap *h = _subHeap; NSUInteger c = _subCharge; _subHeap = nil; _subCharge = 0;
                    if (_subPlaced && c) { _subPlaced = NO; [h nvmtlGiveRange:_heapOffset length:c]; }
                    [h nvmtlReleaseSubAllocation:c]; }
}
- (BOOL)nvmtlSurfaceCopyable {
    size_t rowBytes = (size_t)_i.w * _i.bpp;
    size_t surfaceBytes = IOSurfaceGetWidthOfPlane(_surf, (uint32_t)_plane)
                        * IOSurfaceGetBytesPerElementOfPlane(_surf, (uint32_t)_plane);
    // The copy must also end inside the surface's allocation. Two Tahoe machines crashed in the row copy at a page
    // boundary while width, height and bytes per row all passed; the plane's rows can describe more memory than the
    // surface owns.
    const size_t bpr = IOSurfaceGetBytesPerRowOfPlane(_surf, (uint32_t)_plane);
    const uintptr_t b0 = (uintptr_t)IOSurfaceGetBaseAddress(_surf), bp = (uintptr_t)IOSurfaceGetBaseAddressOfPlane(_surf, (uint32_t)_plane);
    const size_t alloc = IOSurfaceGetAllocSize(_surf);
    const BOOL inAlloc = b0 && bp >= b0 && _i.h && bp - b0 <= alloc
        && (size_t)(_i.h - 1) <= (alloc - (bp - b0) - MIN(rowBytes, alloc - (bp - b0))) / (bpr ? bpr : 1)
        && rowBytes <= alloc - (bp - b0);
    if (_i.bpp && rowBytes <= surfaceBytes
        && IOSurfaceGetHeightOfPlane(_surf, (uint32_t)_plane) >= _i.h
        && bpr >= rowBytes && inAlloc) return YES;
    if (_i.bpp && !inAlloc) {
        static int saidAlloc; if (saidAlloc++ < 4) nvlog("G13 surface %p plane %lu NOT synced: %u rows of %zu B from plane offset %zu pass the surface's %zu-byte allocation",
            (void *)_surf, (unsigned long)_plane, _i.h, bpr, (size_t)(bp - b0), alloc);
        return NO;
    }
    static int said; if (said++ < 4) nvlog("G13 surface %p plane %lu NOT synced: image %ux%u bpp %u, surface bytesPerElement %zu — its pixels stay on one side",
        (void *)_surf, (unsigned long)_plane, _i.w, _i.h, _i.bpp, IOSurfaceGetBytesPerElementOfPlane(_surf, (uint32_t)_plane));
    return NO;
}
// IOSurface locking wires memory and updates its seed; it does not serialize
// this process's GPU readback and CPU import. The seed belongs to the entire
// IOSurface, so every plane and wrapper must use the same monitor.
static id nvmtl_surface_sync(NVMTLTexture *t) {
    static char key;
    static NSMapTable *monitors;
    @synchronized ([NVMTLTexture class]) {
        id monitor = objc_getAssociatedObject(t, &key);
        if (!monitor) {
            if (!monitors) monitors = [NSMapTable strongToWeakObjectsMapTable];
            NSNumber *surface = @(IOSurfaceGetID(t->_surf));
            monitor = [monitors objectForKey:surface];
            if (!monitor) { monitor = [NSObject new]; [monitors setObject:monitor forKey:surface]; }
            objc_setAssociatedObject(t, &key, monitor, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        }
        return monitor;
    }
}
// rb7f: the kernel demoted this texture's VRAM-shared surface (the CPU keeps reading its pages): from now on its writes
// are copied out into the pages after each GPU write (the system-memory placement), not marked newest in VRAM.
void nvmtl_texture_demote(NVMTLTexture *t) {
    NVMTLTexture *r = t->_parent ? t->_parent : t;
    if (!r->_surf) return;
    @synchronized (nvmtl_surface_sync(r)) { r->_vramOn = NO; r->_svDemoted = YES; }
    static unsigned n; if (n++ < 8 || n % 1000 == 0) nvlog("surface %u (%ux%u): demoted - copy-out path (%u so far)", (unsigned)IOSurfaceGetID(r->_surf), r->_i.w, r->_i.h, n);
}
// Per-IOSurface host-pointer imports (VK_EXT_external_memory_host; the B580 kernel turns one into
// DRM_XE_VM_BIND_OP_MAP_USERPTR and wires the pages, kernel/IntelXe/xe_core.cpp). An import costs a VM_BIND plus wiring every page
// (3600 for a 2560x1440 frame), so it is made once per surface and kept: surface-out copies the image straight into the
// surface's pages through it, and surface-in uploads from them.
// - Key: the IOSurfaceRef, its ID, IOSurfaceGetBaseAddress and the allocation size. A key that no longer matches (the
//   surface was mapped elsewhere, or resized) drops the entry before a new import is made.
// - The entry RETAINS the surface. That keeps the surface, and so its mapping in this task, alive while its pages are wired
//   and bound: the address range cannot be freed and handed to other memory under the GPU's binding. An entry goes
//   once it has been idle NVMTL_SURFIMP_IDLE_NS, or idle a quarter of a second while our reference is the last one in this
//   task (the surface was let go), or for room (NVMTL_SURFIMP_SLOTS entries, NVMTL_SURFIMP_BYTES wired). Never while
//   busy (a copy holds it between nvmtl_surfimp_get and nvmtl_surfimp_put), so no buffer is freed under a running copy.
// - writable: every page of the range is writable in this task (protection and max protection). The kernel binds pages it
//   cannot wire for writing READ-ONLY and says nothing to user space (WindowServer's view of client CoreAnimation surfaces
//   is r--), and a GPU write through a read-only PTE is a page fault, not a dropped write; only a writable entry may be a
//   copy destination.
// - failed: the import was refused (or the surface is not writable for a write). Kept like any entry so that the next
//   frames take the old path at once instead of asking the kernel again every frame.
#include <mach/mach_vm.h>
#define NVMTL_SURFIMP_SLOTS 16
#define NVMTL_SURFIMP_IDLE_NS (2ull * NSEC_PER_SEC)
#define NVMTL_SURFIMP_LAST_NS (NSEC_PER_SEC / 4)
#define NVMTL_SURFIMP_BYTES ((size_t)768 << 20)
typedef struct { IOSurfaceRef surf; uint32_t id; void *base; size_t bytes; nvk_buffer hb; int busy; uint64_t used; bool writable, failed; } nvmtl_surfimp;
static nvmtl_surfimp g_surfimp[NVMTL_SURFIMP_SLOTS];
static os_unfair_lock g_surfimp_lock = OS_UNFAIR_LOCK_INIT;
static unsigned long long g_surfimp_made, g_surfimp_hits, g_surfimp_dropped, g_surfout_direct, g_surfout_staged;
static bool nvmtl_range_writable(const void *p, size_t n) {
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)p; const mach_vm_address_t end = a + n;
    while (a < end) {
        mach_vm_address_t ra = a; mach_vm_size_t rs = 0; mach_port_t obj = MACH_PORT_NULL;
        vm_region_basic_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
        if (mach_vm_region(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&info, &cnt, &obj) != KERN_SUCCESS) return false;
        if (obj != MACH_PORT_NULL) mach_port_deallocate(mach_task_self(), obj);
        // ra > a: a hole (mach_vm_region answers the next region up)
        if (ra > a || !rs || !(info.protection & VM_PROT_WRITE) || !(info.max_protection & VM_PROT_WRITE)) return false;
        a = ra + rs;
    }
    return true;
}
// Takes the entry out (the caller frees it after dropping the lock: unbinding and CFRelease do not belong under a spin lock).
static void nvmtl_surfimp_take(nvmtl_surfimp *e, nvmtl_surfimp *dead, int *nd) { dead[(*nd)++] = *e; memset(e, 0, sizeof *e); g_surfimp_dropped++; }
static void nvmtl_surfimp_free(nvmtl_surfimp *dead, int nd) {
    for (int i = 0; i < nd; i++) { if (dead[i].hb.buf) nvmtl_vk_buffer_destroy(&dead[i].hb); if (dead[i].surf) CFRelease(dead[i].surf); }
}
// The import of s's whole allocation, busy until nvmtl_surfimp_put; NULL: none (the caller copies the old way). Made on
// first use. want_write: the copy writes the pages (surface-out), so only a writable entry serves it.
static nvmtl_surfimp *nvmtl_surfimp_get(IOSurfaceRef s, bool want_write) {
    nvmtl_vk_asyncup_reap();   // landed implicit-sync uploads give their imports back first
    const size_t align = nvmtl_vk_host_import_align();
    void *const base = IOSurfaceGetBaseAddress(s);
    size_t bytes = IOSurfaceGetAllocSize(s);
    if (!align || !base || !bytes || ((uintptr_t)base & (align - 1))) return NULL;
    bytes = (bytes + align - 1) & ~(align - 1);
    const uint32_t id = (uint32_t)IOSurfaceGetID(s);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    nvmtl_surfimp dead[NVMTL_SURFIMP_SLOTS + 1]; int nd = 0;
    nvmtl_surfimp *hit = NULL;
    os_unfair_lock_lock(&g_surfimp_lock);
    for (unsigned i = 0; i < NVMTL_SURFIMP_SLOTS; i++) {
        nvmtl_surfimp *e = &g_surfimp[i];
        if (!e->surf || e->busy) { if (e->surf && e->surf == s && e->id == id && e->base == base && e->bytes == bytes) hit = e; continue; }
        const bool same = e->surf == s && e->id == id;
        const uint64_t idle = now - e->used;
        if ((same && (e->base != base || e->bytes != bytes))
            || (!same && (idle > NVMTL_SURFIMP_IDLE_NS || (idle > NVMTL_SURFIMP_LAST_NS && CFGetRetainCount(e->surf) <= 1)))) {
            nvmtl_surfimp_take(e, dead, &nd); continue; }
        if (same) hit = e;
    }
    if (hit) {
        const bool usable = !hit->failed && (!want_write || hit->writable);
        if (usable) { hit->busy++; g_surfimp_hits++; } hit->used = now;
        os_unfair_lock_unlock(&g_surfimp_lock);
        nvmtl_surfimp_free(dead, nd);
        return usable ? hit : NULL;
    }
    os_unfair_lock_unlock(&g_surfimp_lock);
    nvmtl_surfimp_free(dead, nd); nd = 0;
    // a miss: import outside the lock (VM_BIND + wiring takes a while), then publish
    nvmtl_surfimp n = { .surf = s, .id = id, .base = base, .bytes = bytes, .used = now };
    n.writable = nvmtl_range_writable(base, bytes);
    n.failed = (want_write && !n.writable) || nvmtl_vk_buffer_import_host(base, bytes, &n.hb) != 0;
    if (n.failed) memset(&n.hb, 0, sizeof n.hb);
    { static int said; if (said++ < 8) nvlog("surface import: IOSurface %u, %zu bytes at %p, %s%s", id, bytes, base,
        n.failed ? "NOT imported" : "imported", n.failed ? (want_write && !n.writable ? " (its pages are not writable here) - it is copied by the CPU" : " - it is copied by the CPU")
                                                       : n.writable ? " (writable)" : " (read-only: uploads only)"); }
    CFRetain(s);
    os_unfair_lock_lock(&g_surfimp_lock);
    nvmtl_surfimp *slot = NULL, *lru = NULL;
    size_t wired = n.bytes;
    for (unsigned i = 0; i < NVMTL_SURFIMP_SLOTS; i++) {
        nvmtl_surfimp *e = &g_surfimp[i];
        if (e->surf && e->surf == s && e->id == id && e->base == base && e->bytes == bytes) hit = e;   // another thread was first
        if (!e->surf) { if (!slot) slot = e; continue; }
        if (e->hb.buf) wired += e->bytes;
        if (!e->busy && (!lru || e->used < lru->used)) lru = e;
    }
    if (hit) {   // keep the published one, free ours
        const bool usable = !hit->failed && (!want_write || hit->writable);
        if (usable) hit->busy++; hit->used = now;
        os_unfair_lock_unlock(&g_surfimp_lock);
        dead[0] = n; nvmtl_surfimp_free(dead, 1);
        return usable ? hit : NULL;
    }
    if (!slot && lru) { if (lru->hb.buf) wired -= lru->bytes; nvmtl_surfimp_take(lru, dead, &nd); slot = lru; }
    // room in wired bytes: drop the least recently used idle entries
    while (slot && n.hb.buf && wired > NVMTL_SURFIMP_BYTES) {
        nvmtl_surfimp *v = NULL;
        for (unsigned i = 0; i < NVMTL_SURFIMP_SLOTS; i++) { nvmtl_surfimp *e = &g_surfimp[i];
            if (e->surf && e->hb.buf && !e->busy && (!v || e->used < v->used)) v = e; }
        if (!v) break;
        wired -= v->bytes; nvmtl_surfimp_take(v, dead, &nd);
    }
    nvmtl_surfimp *ret = NULL;
    if (slot) {
        *slot = n; g_surfimp_made++;
        if (!n.failed && (!want_write || n.writable)) { slot->busy = 1; ret = slot; }
    } else dead[nd++] = n;   // every slot busy: this one import is used once and freed
    os_unfair_lock_unlock(&g_surfimp_lock);
    nvmtl_surfimp_free(dead, nd);
    static unsigned long long nm; if (++nm <= 6 || nm % 500 == 0) nvlog("surface import cache: %llu made, %llu hits, %llu dropped", g_surfimp_made, g_surfimp_hits, g_surfimp_dropped);
    return ret;
}
static void nvmtl_surfimp_put(nvmtl_surfimp *e);
static void nvmtl_surfin_release(void *imp) { nvmtl_surfimp_put((nvmtl_surfimp *)imp); }
static void nvmtl_surfimp_put(nvmtl_surfimp *e) {
    if (!e) return;
    os_unfair_lock_lock(&g_surfimp_lock);
    e->busy--; e->used = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    os_unfair_lock_unlock(&g_surfimp_lock);
}
// NVMTL_SURFIN_UPLOAD: how surface-in reaches the IOSurface's pages.
//   cache    the copy engine reads them through the surface's cached import (nvmtl_surfimp_get); the default on Intel,
//            whose kernel (IntelXe.kext xe_core.cpp MAP_USERPTR) is the one whose import semantics the cache was written against
//   import   an import made for each upload and released after it (the default before the cache, and on other GPUs)
//   cpu      the CPU copies them into a staging buffer (nvmtl_vk_image_write)
static int nvmtl_surfin_mode(void) {
    static int mode = -1;
    if (mode < 0) { const char *u = getenv("NVMTL_SURFIN_UPLOAD");
        mode = (u && !strcmp(u, "cpu")) ? 0 : (u && !strcmp(u, "import")) ? 1 : (u && !strcmp(u, "cache")) ? 2 : nvmtl_vk_vendor() == 0x8086 ? 2 : 1;
        nvlog("surface-in: %s (NVMTL_SURFIN_UPLOAD=%s)", mode == 2 ? "copy engine reads the IOSurface's pages through a cached import"
              : mode == 1 ? "copy engine reads the IOSurface's pages, imported for each upload" : "CPU copy through a staging buffer", u ? u : "unset"); }
    return mode;
}
// NVMTL_SURFOUT_DIRECT=0: surface-out copies the image into a staging buffer and memcpys that into the IOSurface
// (nvmtl_vk_image_read), as before. Default on Intel (=1 elsewhere): the copy engine writes the IOSurface's own pages
// (nvmtl_vk_image_read_host).
static bool nvmtl_surfout_direct(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SURFOUT_DIRECT"); on = (e && *e) ? *e != '0' : nvmtl_vk_vendor() == 0x8086;
        nvlog("surface-out: %s (NVMTL_SURFOUT_DIRECT=%s)", on ? "the copy engine writes the IOSurface's own pages (cached import); staged copy when a surface cannot be imported"
              : "GPU copy to a staging buffer, then a CPU memcpy into the IOSurface", e ? e : "unset"); }
    return on;
}
// Frame path stage 3 (B580): the scan-out buffer of a WindowServer surface that the display pipe presents at the panel's
// mode. NMXAccel flips the plane to it instead of copying the frame (nvrm-accel.cpp nmxScanOp / nmxPresent); each GPU
// write of the surface is copied into it, VRAM to VRAM, in the surface-out's own submission (nvmtl_vk_image_read_host_scan),
// which ends before the surface's completion mark is released, and the kernel's present waits for that mark. A surface
// qualifies once the kernel says so (op 0: presented at the mode's size, debug.nmxaccel_flip on); a refusal is asked again
// after 500 ms, a binding re-checked every second (a mode change or the switch going off unbinds it). An entry holds its
// IOSurface, so the ID cannot be reused while bound; one idle for 10 s is unbound unless the plane still shows it (the
// kernel answers busy and it stays). NVMTL_SCANOUT=0: never bound. Only the plugin's GPU writes reach the buffer: a CPU
// write to a bound surface would not be on the panel until its next GPU write (none on WindowServer's composites seen).
#define NVMTL_SCANOUT_SLOTS 6
// The buffer is counted: a copy folded into a part (nvmtl_fold_surface_out) holds it until the part has finished on the GPU,
// so an entry dropped meanwhile (another size, no longer shown, idle) frees it only after that copy. refs: g_scanout_mx.
typedef struct { nvk_buffer buf; unsigned refs; } nvmtl_scanbuf;
typedef struct { IOSurfaceRef surf; uint32_t sid, w, h, pitch; nvmtl_scanbuf *sb; uint64_t va, tried, used, checked; int state, readback; } nvmtl_scanout;   // state 1 bound, -1 refused; readback: op 3, not displayed
static nvmtl_scanout g_scanout[NVMTL_SCANOUT_SLOTS];
static pthread_mutex_t g_scanout_mx = PTHREAD_MUTEX_INITIALIZER;
static void nvmtl_scanbuf_unref_locked(nvmtl_scanbuf *b) {
    if (b && !--b->refs) { nvmtl_vk_buffer_destroy(&b->buf); free(b); }
}
// A folded copy's hold, taken with the entry returned by nvmtl_scanout_get (g_scanout_mx held), released after its part.
static nvmtl_scanbuf *nvmtl_scanbuf_hold_locked(nvmtl_scanout *e) { if (e && e->sb) e->sb->refs++; return e ? e->sb : NULL; }
static void nvmtl_scanbuf_release(nvmtl_scanbuf *b) {
    if (!b) return;
    pthread_mutex_lock(&g_scanout_mx); nvmtl_scanbuf_unref_locked(b); pthread_mutex_unlock(&g_scanout_mx);
}
static unsigned long long g_scanout_copies, g_scanout_binds;
static bool nvmtl_scanout_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SCANOUT"); const bool ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        on = !(e && e[0] == '0') && ws && nvmtl_vk_vendor() == 0x8086;
        if (ws) nvlog("scan-out buffers: %s (NVMTL_SCANOUT=%s)", on ? "bound while NMXAccel flips (debug.nmxaccel_flip)" : "off", e ? e : "unset"); }
    return on;
}
// g_scanout_mx held. Unbinds and frees an entry; false when the plane still shows it (it stays bound).
static bool nvmtl_scanout_drop(nvmtl_scanout *e) {
    // phase 4: pages that may be older than the scan-out buffer get its frame before the binding goes
    if (e->state == 1 && e->sid && nvmtl_vk_anv_level() >= 2) (void)nvmtl_vk_scan_newer(e->sid, 0);
    if (e->state == 1) {
        if (nvmtl_vk_scanout_op(2, e->sid, 0, 0, 0, 0, 0, NULL) == 4) return false;
        nvmtl_scanbuf_unref_locked(e->sb);
    }
    if (e->surf) CFRelease(e->surf);
    memset(e, 0, sizeof *e);
    return true;
}
// The bound entry of r's surface with g_scanout_mx held (release it with nvmtl_scanout_put), or NULL.
static nvmtl_scanout *nvmtl_scanout_get(NVMTLTexture *r) {
    if (!nvmtl_scanout_on() || !r->_surf || r->_plane || r->_i.bpp != 4 || r->_i.samples > 1 || r->_i.layers > 1 || r->_i.mips > 1 ||
        r->_i.mtl_type != 2 || (r->_fmt != MTLPixelFormatBGRA8Unorm && r->_fmt != MTLPixelFormatBGRA8Unorm_sRGB) ||
        IOSurfaceGetPixelFormat(r->_surf) != 0x42475241u || r->_i.w < 640 || r->_i.h < 480) return NULL;
    const uint32_t sid = (uint32_t)IOSurfaceGetID(r->_surf), w = r->_i.w, h = r->_i.h;
    uint32_t pitch = (w * 4 + 63) & ~63u;
    size_t bytes = (size_t)pitch * h;
    // a readback binding (NMXAccel op 3) keeps the surface's own row, so the kernel's page-off copies it 1:1 into the pages
    const uint32_t srow = (uint32_t)IOSurfaceGetBytesPerRowOfPlane(r->_surf, 0);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_scanout_mx);
    nvmtl_scanout *e = NULL;
    for (int i = 0; i < NVMTL_SCANOUT_SLOTS; i++) {
        nvmtl_scanout *x = &g_scanout[i];
        if (x->sid == sid) e = x;
        else if (x->state == 1 && now - x->used > 10 * NSEC_PER_SEC) (void)nvmtl_scanout_drop(x);
    }
    if (e && e->state == 1 && e->surf == r->_surf && e->w == w && e->h == h) {
        // a readback binding stays while the surface is not presented (op 0 answers 2) and still streams; presented now
        // (a mode change), it goes and the surface is bound for the display below
        const int recheck = now - e->checked >= NSEC_PER_SEC;
        const int st0 = recheck ? nvmtl_vk_scanout_op(0, sid, 0, e->readback ? (size_t)((w * 4 + 63) & ~63u) * h : bytes, w, h,
                                                      e->readback ? ((w * 4 + 63) & ~63u) : pitch, NULL) : 0;
        if (!recheck || (e->readback ? st0 == 2 && nvmtl_ws_is_streaming_with_resource(sid, 20) : !st0)) {
            if (recheck) e->checked = now;
            e->used = now;
            return e;
        }
    }
    if (e && e->state == 1 && !nvmtl_scanout_drop(e)) { pthread_mutex_unlock(&g_scanout_mx); return NULL; }   // still shown
    if (e && e->state == 1) e = NULL;
    if (e && e->state < 0 && now - e->tried < NSEC_PER_SEC / 2) { pthread_mutex_unlock(&g_scanout_mx); return NULL; }
    const int ask = nvmtl_vk_scanout_op(0, sid, 0, bytes, w, h, pitch, NULL);
    if (!e) {
        for (int i = 0; i < NVMTL_SCANOUT_SLOTS && !e; i++) if (!g_scanout[i].sid) e = &g_scanout[i];
        if (!e)   // else the refusal asked longest ago; bound entries are never displaced
            for (int i = 0; i < NVMTL_SCANOUT_SLOTS; i++) if (g_scanout[i].state < 0 && (!e || g_scanout[i].tried < e->tried)) e = &g_scanout[i];
        if (e) memset(e, 0, sizeof *e);
    }
    if (!e) { pthread_mutex_unlock(&g_scanout_mx); return NULL; }
    // not presented, but a composite the GPU streams and that has an accelerator resource: a readback binding (op 3)
    const int readback = ask == 2 && srow >= w * 4 && !(srow & 3) && nvmtl_ws_is_streaming_with_resource(sid, 20);
    if (readback) { pitch = srow; bytes = (size_t)pitch * h; }
    e->sid = sid; e->w = w; e->h = h; e->pitch = pitch; e->state = -1; e->tried = now; e->readback = readback;
    if (ask && !readback) { pthread_mutex_unlock(&g_scanout_mx); return NULL; }   // not presented yet, another size, flips off: asked again later
    nvk_buffer b; uint64_t va = 0; uint32_t ggtt = 0;
    // outside the BAR window first (nvmtl_vk_scanout_buffer); a kernel that cannot pin such a buffer (5, kNmxScPin: not
    // one physical run) gets window memory from then on. NVMTL_SCANOUT_VISIBLE=1: window memory always.
    static int nonvis = -1;
    if (nonvis < 0) { const char *e = getenv("NVMTL_SCANOUT_VISIBLE"); nonvis = !(e && e[0] == '1'); }
    int vis = !nonvis;
    if (nvmtl_vk_scanout_buffer(bytes, &b, &va, vis) && (vis || (vis = 1, nvmtl_vk_scanout_buffer(bytes, &b, &va, 1)))) {
        nvlog("scan-out: no buffer for surface %u (%ux%u)", sid, w, h); pthread_mutex_unlock(&g_scanout_mx); return NULL; }
    const unsigned bindOp = readback ? 3u : 1u;
    int st = nvmtl_vk_scanout_op(bindOp, sid, va, bytes, w, h, pitch, &ggtt);
    if (st == 5 && !vis) {
        nvmtl_vk_buffer_destroy(&b); nonvis = 0;
        nvlog("scan-out: the kernel cannot pin a scan-out buffer outside the BAR window - inside it from now on");
        if (nvmtl_vk_scanout_buffer(bytes, &b, &va, 1)) { nvlog("scan-out: no buffer for surface %u (%ux%u)", sid, w, h); pthread_mutex_unlock(&g_scanout_mx); return NULL; }
        vis = 1; st = nvmtl_vk_scanout_op(bindOp, sid, va, bytes, w, h, pitch, &ggtt);
    }
    if (!st) { static int said; if (!said++) nvlog("scan-out: buffers in %s", vis ? "the CPU-visible BAR window" : "VRAM outside the BAR window (scan-out BOs)"); }
    if (st) {
        nvmtl_vk_buffer_destroy(&b);
        static unsigned nr; if (nr++ < 8) nvlog("scan-out: surface %u %ux%u: bind refused (%d) - copied as before", sid, w, h, st);
        pthread_mutex_unlock(&g_scanout_mx); return NULL;
    }
    nvmtl_scanbuf *sb = calloc(1, sizeof *sb);
    if (!sb) { (void)nvmtl_vk_scanout_op(2, sid, 0, 0, 0, 0, 0, NULL); nvmtl_vk_buffer_destroy(&b); pthread_mutex_unlock(&g_scanout_mx); return NULL; }
    sb->buf = b; sb->refs = 1;
    e->surf = (IOSurfaceRef)CFRetain(r->_surf); e->sb = sb; e->va = va; e->state = 1; e->used = e->checked = now;
    g_scanout_binds++;
    if (readback) nvlog("scan-out: surface %u %ux%u pitch %u: readback buffer at GPU 0x%llx (not displayed; a CPU lock pages it off) (%llu bound)", sid, w, h, pitch, (unsigned long long)va, g_scanout_binds);
    else nvlog("scan-out: surface %u %ux%u pitch %u: VRAM buffer at GPU 0x%llx is GGTT 0x%x (%llu bound)", sid, w, h, pitch, (unsigned long long)va, ggtt, g_scanout_binds);
    return e;
}
static void nvmtl_scanout_put(nvmtl_scanout *e) { if (e) pthread_mutex_unlock(&g_scanout_mx); }
// NVMTL_SCANOUT_READBACK=0 (experiment): a bound surface's frames go only into its scan-out buffer, not back to the
// IOSurface's pages, which takes the 14.75 MB GPU-to-system-memory copy off the frame. Its system memory then keeps an
// older frame: CPU readers of the composite, and any frame the kernel copies instead of flipping (L2 flush failure,
// a refused flip), see that older frame. Default 1: both copies, in one submission.
// gpuperf phase 4 experiment: the root-owned diag flag /tmp/nvmtl-no-scanout-readback (WindowServer reads only root's
// flags; polled every 250 ms) drops the readback at run time, to measure what it costs per composite. CPU readers of a
// bound composite (and a frame the kernel copies instead of flipping) then see an older frame until the flag goes.
static bool nvmtl_scanout_readback(void) {
    static int on = -1, was = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SCANOUT_READBACK"); on = !(e && e[0] == '0');
        if (nvmtl_scanout_on()) nvlog("scan-out: bound surfaces are %s (NVMTL_SCANOUT_READBACK=%s)", on ? "also copied to their IOSurface" : "NOT copied to their IOSurface", e ? e : "unset"); }
    const int skip = on && nvmtl_diag_flag("no-scanout-readback");
    if (skip != was) { if (was >= 0 || skip) nvlog("scan-out: readback of bound surfaces %s (/tmp/nvmtl-no-scanout-readback)", skip ? "OFF (experiment)" : "on again"); was = skip; }
    return on && !skip;
}
- (void)nvmtlSurfaceIn {   // NVMTL_HITCH_MS: timed into the hitch log
    const uint64_t t = nvmtl_hitch_t0(); [self nvmtlSurfaceInNow]; nvmtl_hitch_add(NVH_SURFIN, t, 0);
}
- (void)nvmtlSurfaceInNow {
    NVMTLTexture *r = _parent ? _parent : self;
    if (!r->_surf) return;
    @synchronized (nvmtl_surface_sync(r)) {
    const uint32_t vgen = r->_vramOn ? 0 : nvmtl_vram_gen(r->_surf, r->_plane);
    if (r->_surfSynced && IOSurfaceGetSeed(r->_surf) == r->_surfSeed && (r->_vramOn || vgen == r->_surfGen)) return;
    if (![r nvmtlSurfaceCopyable]) return;
    { uint32_t now = IOSurfaceGetSeed(r->_surf);
      if (r->_vramOn && nvmtl_vram_tag_current(r->_surf, r->_plane, now)) {
          r->_surfSeed = now; r->_surfSynced = YES; g_surfin_skip++;
          static unsigned ns; if (ns++ < 6 || ns % 2000 == 0) nvlog("item 7: surface-in SKIPPED (shared VRAM current at seed %u): %llu skipped, %llu copied", now, g_surfin_skip, g_surfin_copy);
          return; } }
    uint32_t seed = 0;
    if (IOSurfaceLock(r->_surf, kIOSurfaceLockReadOnly, &seed) != kIOReturnSuccess) { nvlog("G13 surface-in: IOSurfaceLock FAILED"); return; }
    void *base = IOSurfaceGetBaseAddressOfPlane(r->_surf, (uint32_t)r->_plane);
    size_t row = IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane);
    int rc = -1;
    const int mode = nvmtl_surfin_mode();
    const uint32_t isid = (uint32_t)IOSurfaceGetID(r->_surf);
    if (base && row && mode == 2) {
        // implicit sync: the copy waits on the GPU for the surface's writers in other processes and stays in flight; the
        // pages' import is released when it lands (nvmtl_surfin_release). Without it: a synchronous copy, as before.
        nvmtl_surfimp *imp = nvmtl_surfimp_get(r->_surf, false);
        if (imp) rc = nvmtl_vk_image_write_host_buf_isync(r->_q, &r->_i, &imp->hb, (size_t)((uint8_t *)base - (uint8_t *)imp->base), row, isid,
                                                          nvmtl_surfin_release, imp);
        static _Atomic unsigned long long nimp, nnoimp, nfail;
        const unsigned long long k = atomic_fetch_add(rc == 0 ? &nimp : imp ? &nfail : &nnoimp, 1) + 1;
        if (rc && k <= 6) nvlog("surface-in: surface %u %ux%u %s - copied by the CPU", isid, r->_i.w, r->_i.h, imp ? "upload from its imported pages FAILED" : "has no page import (no free slot, or not importable)");
        if ((atomic_load(&nimp) + atomic_load(&nnoimp) + atomic_load(&nfail)) % 4000 == 0)
            nvlog("surface-in paths: %llu from imported pages, %llu with no import, %llu failed uploads (CPU copies)", atomic_load(&nimp), atomic_load(&nnoimp), atomic_load(&nfail));
    } else if (base && row && mode == 1) {
        (void)nvmtl_vk_surface_cpu_wait(isid, 0, 100);   // the CPU reads the pages: their writers on the GPU first
        const size_t off = (size_t)((uint8_t *)base - (uint8_t *)IOSurfaceGetBaseAddress(r->_surf));
        rc = nvmtl_vk_image_write_host(r->_q, &r->_i, base, row, IOSurfaceGetAllocSize(r->_surf) - off);
    }
    if (rc && base && row) (void)nvmtl_vk_surface_cpu_wait(isid, 0, 100);
    if (rc) rc = (base && row) ? nvmtl_vk_image_write(r->_q, &r->_i, base, row) : -1;
    if (!rc && r->_i.bpp == 4) { char tag[24]; snprintf(tag, sizeof tag, "in%u-", (unsigned)IOSurfaceGetID(r->_surf)); nvmtl_frame_dump(tag, base, row, r->_i.w, r->_i.h); }
    IOSurfaceUnlock(r->_surf, kIOSurfaceLockReadOnly, NULL);
    if (rc) { nvlog("G13 surface-in: upload FAILED (%ux%u) — retried at the next use", r->_i.w, r->_i.h); return; }
    r->_surfSeed = seed; r->_surfSynced = YES; r->_surfGen = vgen;
    g_surfin_copy++; if (r->_vramOn) nvmtl_vram_tag_set(r->_surf, r->_plane, seed);
    static unsigned n; if (n++ < 6 || n % 2000 == 0) nvlog("G13 surface-in #%u: %ux%u surface %p seed %u uploaded", n, r->_i.w, r->_i.h, (void *)r->_surf, seed);
    }
}
- (BOOL)nvmtlSurfaceOut { return [self nvmtlSurfaceOutDirtied:NO]; }
// The copy ran at the end of the writing part (nvmtl_fold_surface_out): what is left is the bookkeeping of a surface-out,
// a lock and unlock that moves the surface's seed (CPU readers see new contents) and that seed recorded as ours.
static unsigned long long g_surfout_folded;
- (void)nvmtlSurfaceOutFolded { [self nvmtlSurfaceOutFoldedWithSiblings:nil]; }
- (void)nvmtlSurfaceOutFoldedWithSiblings:(NSArray<NVMTLTexture *> *)siblings {
    NVMTLTexture *r = _parent ? _parent : self;
    const uint64_t t = nvmtl_hitch_t0();
    @synchronized (nvmtl_surface_sync(r)) {
        // phase 4: no CPU lock at all. The kernel's mark makes the composite's device cache current
        // (IOSurfaceDeviceCache::setCurrent), which is itself the seed move readers see (IOSurface::set_current_device_cache
        // increments the seed); a seed-bump lock here would sync the device caches first - the page-off this skips - and
        // with kIOSurfaceLockAvoidSync it fails while a device cache is current. The seed after the mark is recorded as
        // ours, so the next frame's load of the composite does not count it as a foreign write (no surface-in: no CPU lock,
        // page-off and 28 MB upload per frame, 2026-10-29 level-3 capture). Refused (no family resource, not bound any
        // more): the pages get this frame the old way.
        if (r->_p4) {
            const int st = nvmtl_vk_scan_newer((uint32_t)IOSurfaceGetID(r->_surf), 1);
            static _Atomic unsigned long long nok, nref;
            const unsigned long long k = atomic_fetch_add(st == 0 ? &nok : &nref, 1) + 1;
            if (st == 0) {
                r->_surfSeed = IOSurfaceGetSeed(r->_surf); r->_surfSynced = YES; r->_surfGen = nvmtl_vram_gen(r->_surf, r->_plane);
                if (k <= 3 || (k & 8191) == 0) nvlog("phase 4: surface %u composite not read back, scan-out buffer newer (%llu so far)",
                                                     (unsigned)IOSurfaceGetID(r->_surf), k);
                return;
            }
            r->_p4 = NO;
            if (k <= 8 || (k & 1023) == 0) nvlog("phase 4: surface %u newer mark refused (%d) - read back now (%llu refused, %llu marked)",
                                                 (unsigned)IOSurfaceGetID(r->_surf), st, k, atomic_load(&nok));
            (void)[r nvmtlSurfaceOutDirtiedNow:NO];
            return;
        }
        const uint32_t lockopt = nvmtl_noseed() ? kIOSurfaceLockReadOnly : 0; uint32_t seed = 0;
        if (IOSurfaceLock(r->_surf, lockopt, NULL) != kIOReturnSuccess) { nvlog("surface-out (folded): IOSurfaceLock FAILED"); return; }
        IOSurfaceUnlock(r->_surf, lockopt, &seed); if (lockopt) seed = IOSurfaceGetSeed(r->_surf);
        r->_surfSeed = seed; r->_surfSynced = YES; r->_surfGen = nvmtl_vram_gen(r->_surf, r->_plane);
        // All these planes were copied by the same submission, under the same fence.
        // Publish once and record that one seed on every plane. Publishing planes
        // separately left earlier planes at an old seed and re-uploaded them next use.
        for (NVMTLTexture *s in siblings) if (s != r && s->_surf == r->_surf && !s->_vramOn && !s->_p4) {
            s->_surfSeed = seed; s->_surfSynced = YES; s->_surfGen = nvmtl_vram_gen(s->_surf, s->_plane);
        }
    }
    const unsigned long long n = ++g_surfout_folded;
    if (n <= 3 || n % 5000 == 0) nvlog("surface-out (folded) #%llu: %ux%u surface %u - copied by its writer's own commands", n, r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf));
    nvmtl_hitch_add(NVH_SURFFOLD, t, 0);
}
// A re-mark after the commit's (nvmtlSurfaceOutDirtied:) that found the accelerator lock taken is retried here, off the
// completion thread: a CPU lock that paged the surface off between the commit's two marks would otherwise leave its
// unfinished copy current until the surface is drawn again. Each retry is again a try that never waits for the lock,
// right after checking that the seed has not moved (a CPU write, or a newer commit marked it again): a mark that waited
// for the lock could land after a CPU write that came in meanwhile and make the older VRAM current over it. 50 tries,
// 2 ms apart; the texture then follows the seed the mark left.
static void nvmtl_surface_remark_later(NVMTLTexture *r, uint32_t seed) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("nvmtl.surface-remark", DISPATCH_QUEUE_SERIAL); });
    dispatch_async(queue, ^{
        int marked = 1;
        for (int i = 0; i < 50 && marked > 0; i++) {
            if (i) usleep(2000);
            if (IOSurfaceGetSeed(r->_surf) != seed) return;
            marked = nvmtl_vk_surface_dirty_try((uint32_t)IOSurfaceGetID(r->_surf), (uint32_t)r->_plane);
        }
        // demoted meanwhile: the commit's mark stands (the page-off that demoted it copied this frame); the next writes copy out
        if (marked == -2) { nvmtl_texture_demote(r); return; }
        if (marked) return;
        @synchronized (nvmtl_surface_sync(r)) {
            if (r->_surfSeed != seed) return;
            const uint32_t now = IOSurfaceGetSeed(r->_surf);
            r->_surfSeed = now; r->_surfGen = nvmtl_vram_tag_bump(r->_surf, r->_plane, now);
        }
    });
}
/* glcpu4 NVMTL_VRAM_TAG_ASYNC: the generation bump of a surface-out (IOSurfaceCopyValue + IOSurfaceSetValue of the
   "NVMTLVramAt" tag) is done on a serial queue instead of the committing thread. IOSurfaceSetValue serialises the value
   (IOCFSerialize) and calls into the kernel: 48 + 5 of 3595 render-thread samples in prod7's Minecraft profile, once or
   twice per frame. The tag tells private (copying) wrappers in any process that the shared VRAM changed under an unchanged
   seed. A reader that looks before the queued bump lands sees the previous generation, the same answer it got had it
   looked just before this commit; the bump still lands in commit order (one serial queue), under the surface's monitor
   like every other tag write here, with the seed taken at surface-out. _surfGen of a shared-VRAM texture is not compared
   (nvmtlSurfaceInNow); it is still recorded when the bump lands, unless a newer seed was recorded meanwhile. */
NVMTL_G4_SWITCH(nvmtl_vram_tag_async_on, "NVMTL_VRAM_TAG_ASYNC", "surface-out VRAM generation tags written off the committing thread")
static void nvmtl_vram_tag_bump_later(NVMTLTexture *r, uint32_t seed) {
    static dispatch_queue_t queue; static dispatch_once_t once;
    dispatch_once(&once, ^{ queue = dispatch_queue_create("nvmtl.vram-tag", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0)); });
    dispatch_async(queue, ^{
        @synchronized (nvmtl_surface_sync(r)) {
            const uint32_t g = nvmtl_vram_tag_bump(r->_surf, r->_plane, seed);
            if (r->_surfSeed == seed) r->_surfGen = g;
        }
    });
}
// dirtied: the commit already marked this surface's shared VRAM newest in the kernel (WindowServer, with an accelerator
// whose page-off waits for the GPU). It is marked again in case a CPU lock paged it off since, but only if the accelerator
// lock is free: a page-off waiting for another surface's pending GPU write holds that lock until the write is released,
// by the same completion thread that would be blocked here. Taken, the commit's mark stands and the re-mark is retried
// off this thread (nvmtl_surface_remark_later).
- (BOOL)nvmtlSurfaceOutDirtied:(BOOL)dirtied {   // NVMTL_HITCH_MS: timed into the hitch log
    nvmtl_surfcensus(_parent ? _parent : self, 1);
    const uint64_t t = nvmtl_hitch_t0(); const BOOL r = [self nvmtlSurfaceOutDirtiedNow:dirtied]; nvmtl_hitch_add(NVH_SURFOUT, t, 0); return r;
}
- (BOOL)nvmtlSurfaceOutDirtiedNow:(BOOL)dirtied {
    NVMTLTexture *r = _parent ? _parent : self;
    if (!r->_surf) return YES;
    @synchronized (nvmtl_surface_sync(r)) {
    if (![r nvmtlSurfaceCopyable]) { nvtrace("SURFOUT surf %u %ux%u NOT copyable", (unsigned)IOSurfaceGetID(r->_surf), r->_i.w, r->_i.h); return NO; }
    nvtrace("SURFOUT surf %u %ux%u vramOn %d t=%llu", (unsigned)IOSurfaceGetID(r->_surf), r->_i.w, r->_i.h, (int)r->_vramOn,
            (unsigned long long)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000));
    const int marked = !r->_vramOn ? -1 : dirtied ? nvmtl_vk_surface_dirty_try((uint32_t)IOSurfaceGetID(r->_surf), (uint32_t)r->_plane)
                                                   : nvmtl_vk_surface_dirty((uint32_t)IOSurfaceGetID(r->_surf), (uint32_t)r->_plane);
    if (marked == -2) { r->_vramOn = NO; r->_svDemoted = YES; static unsigned nd; if (nd++ < 8 || nd % 1000 == 0) nvlog("surface %u (%ux%u): demoted at surface-out - copied out from now on (%u so far)", (unsigned)IOSurfaceGetID(r->_surf), r->_i.w, r->_i.h, nd); }
    if (r->_vramOn && (dirtied || !marked)) {
        uint32_t seed = IOSurfaceGetSeed(r->_surf);
        r->_surfSeed = seed; r->_surfSynced = YES;
        if (nvmtl_vram_tag_async_on()) nvmtl_vram_tag_bump_later(r, seed);   // glcpu4
        else r->_surfGen = nvmtl_vram_tag_bump(r->_surf, r->_plane, seed);
        if (dirtied && marked > 0) nvmtl_surface_remark_later(r, seed);
        static unsigned nd; if (nd++ < 6 || nd % 5000 == 0) nvlog("surface-out -> dirty, no copy (%ux%u surface %u seed %u, #%u)", r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf), seed, nd);
        return YES;
    }
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    uint32_t lockopt = nvmtl_noseed() ? kIOSurfaceLockReadOnly : 0;
    if (IOSurfaceLock(r->_surf, lockopt, NULL) != kIOReturnSuccess) { nvlog("G13 surface-out: IOSurfaceLock FAILED"); return NO; }
    void *base = IOSurfaceGetBaseAddressOfPlane(r->_surf, (uint32_t)r->_plane);
    size_t row = IOSurfaceGetBytesPerRowOfPlane(r->_surf, (uint32_t)r->_plane);
    // Direct: the copy engine writes the surface's pages through its cached import (no staging buffer, no CPU memcpy).
    // Shared-VRAM surfaces (_vramOn, NVIDIA item 7) are left alone: the kernel pages their memory on and off.
    int rc = -1; bool direct = false, scanned = false;
    // implicit sync: other processes' GPU readers and writers of the pages finish before this copy overwrites them
    if (base && row && !r->_vramOn) (void)nvmtl_vk_surface_cpu_wait((uint32_t)IOSurfaceGetID(r->_surf), 1, 100);
    nvmtl_scanout *so = (base && row && !r->_vramOn) ? nvmtl_scanout_get(r) : NULL;   // stage 3: also into the scan-out buffer
    if (so && !nvmtl_scanout_readback()) {
        rc = nvmtl_vk_image_read_host_scan(r->_q, &r->_i, NULL, 0, 0, &so->sb->buf, so->pitch);
        scanned = direct = !rc;
    }
    if (!direct && base && row && !r->_vramOn && nvmtl_surfout_direct()) {
        nvmtl_surfimp *imp = nvmtl_surfimp_get(r->_surf, true);
        if (imp) {
            rc = nvmtl_vk_image_read_host_scan(r->_q, &r->_i, &imp->hb, (size_t)((uint8_t *)base - (uint8_t *)imp->base), row,
                                               so ? &so->sb->buf : NULL, so ? so->pitch : 0);
            scanned = so && !rc;
            nvmtl_surfimp_put(imp);
            if (rc == -2) { static int sf; if (sf++ < 4) nvlog("surface-out: the copy into the imported pages FAILED (%ux%u surface %u) - staged copy instead", r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf)); }
            direct = !rc;
        }
        if (!direct) { static int sn; if (sn++ < 4) nvlog("surface-out: %ux%u surface %u (row %zu, bpp %u, block %ux%u) is NOT copied directly - staged copy",
                                                          r->_i.w, r->_i.h, (unsigned)IOSurfaceGetID(r->_surf), row, r->_i.bpp, r->_i.bw, r->_i.bh); }
    }
    if (!direct) rc = (base && row) ? nvmtl_vk_image_read(r->_q, &r->_i, base, row) : -1;
    if (so && !scanned && !rc) scanned = !nvmtl_vk_image_read_host_scan(r->_q, &r->_i, NULL, 0, 0, &so->sb->buf, so->pitch);
    if (so && !scanned && !nvmtl_scanout_drop(so))   // its buffer is stale now: unbound, so this surface is copied
        nvlog("scan-out: surface %u: copy into its scan-out buffer FAILED while the plane shows it", (unsigned)IOSurfaceGetID(r->_surf));
    if (scanned) { g_scanout_copies++;
        if (g_scanout_copies <= 3 || g_scanout_copies % 5000 == 0) nvlog("scan-out: %llu frames copied into scan-out buffers", g_scanout_copies); }
    nvmtl_scanout_put(so);
    g_surfout_direct += direct; g_surfout_staged += !direct;
    if (!rc && r->_i.bpp == 4) { char tag[24]; snprintf(tag, sizeof tag, "out%u-", (unsigned)IOSurfaceGetID(r->_surf)); nvmtl_frame_dump(tag, base, row, r->_i.w, r->_i.h); }
    uint32_t seed = 0; IOReturn unlocked = IOSurfaceUnlock(r->_surf, lockopt, &seed); if (lockopt) seed = IOSurfaceGetSeed(r->_surf);
    if (rc || unlocked != kIOReturnSuccess) { nvlog("G13 surface-out: readback FAILED (%ux%u)", r->_i.w, r->_i.h); return NO; }
    r->_surfSeed = seed; r->_surfSynced = YES;
    r->_surfGen = r->_vramOn ? r->_surfGen : nvmtl_vram_gen(r->_surf, r->_plane);
    if (r->_vramOn && !lockopt) nvmtl_vram_tag_set(r->_surf, r->_plane, seed);
    static unsigned n; if (n++ < 6 || n % 2000 == 0) nvlog("G13 surface-out #%u: %ux%u surface %p seed %u, %.2f ms, %s (%llu direct, %llu staged)", n, r->_i.w, r->_i.h,
        (void *)r->_surf, seed, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6, direct ? "direct" : "staged", g_surfout_direct, g_surfout_staged);
    return YES;
    }
}
- (NSUInteger)width  { return MAX(1u, [self nvi]->w >> _baseLevel); }
- (NSUInteger)height { return MAX(1u, [self nvi]->h >> _baseLevel); }
- (NSUInteger)depth  { nvk_image *i = [self nvi]; return i->mtl_type == 7 ? MAX(1u, i->layers >> _baseLevel) : 1; }
- (NSUInteger)mipmapLevelCount { if (_parent) return _mips ? _mips : 1; uint32_t m = [self nvi]->mips; return m ? m : 1; }
- (id<MTLTexture>)parentTexture { return _viewParent; }
- (id<MTLResource>)rootResource { return _viewParent ? (id<MTLResource>)_viewParent : (id<MTLResource>)_backBuf; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)t { return nvmtl_owner_check(t); }
- (NSUInteger)parentRelativeLevel { return _relativeLevel; }
- (NSUInteger)parentRelativeSlice { return _relativeSlice; }
- (NSUInteger)arrayLength { uint32_t l = _parent ? _viewSlices : ([self nvi]->layers ?: 1), t = (uint32_t)[self textureType]; return t == 6 ? l / 6 : (t == 1 || t == 3 || t == 8) ? l : 1; }
- (NSUInteger)sampleCount { return [self nvi]->samples > 1 ? [self nvi]->samples : 1; }
- (MTLTextureType)textureType { return (MTLTextureType)(_parent ? _viewType : [self nvi]->mtl_type); }
- (MTLPixelFormat)pixelFormat { return _fmt ? _fmt : MTLPixelFormatRGBA8Unorm; }
- (IOSurfaceRef)iosurface { return _shareable ? NULL : _surf; }
- (NSUInteger)iosurfacePlane { return _plane; }
- (MTLStorageMode)storageMode { NVMTLTexture *r = _parent ? _parent : self; if (r->_shareable) return MTLStorageModePrivate;
    NVMTLHeap *hp = r->_heap ? r->_heap : r->_subHeap; return hp ? hp->_storage : (MTLStorageMode)((r->_ropt >> MTLResourceStorageModeShift) & 0xF); }
- (BOOL)isFramebufferOnly { NVMTLTexture *r = _parent ? _parent : self; return r->_fbo; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (MTLCPUCacheMode)cpuCacheMode { NVMTLTexture *r = _parent ? _parent : self; return nvmtl_res_cache(r->_heap ? r->_heap : r->_subHeap, r->_ropt); }
- (MTLHazardTrackingMode)hazardTrackingMode { NVMTLTexture *r = _parent ? _parent : self; return nvmtl_res_hazard(r->_heap ? r->_heap : r->_subHeap, r->_ropt); }
- (MTLResourceOptions)resourceOptions { return ((MTLResourceOptions)[self storageMode] << MTLResourceStorageModeShift) | (MTLResourceOptions)[self cpuCacheMode]
                                            | ((MTLResourceOptions)[self hazardTrackingMode] << MTLResourceHazardTrackingModeShift); }
- (BOOL)isAliasable { @synchronized (self) { return _aliased; } }
- (void)makeAliasable {
    NVMTLHeap *h = nil; NSUInteger c = 0, o = 0; BOOL p = NO;
    @synchronized (self) {
        if (!_subHeap || _aliased) return;
        h = _subHeap; c = _subCharge; _subCharge = 0; _aliased = YES;
        p = _subPlaced; o = _heapOffset; _subPlaced = NO;
    }
    if (c) { if (p) [h nvmtlGiveRange:o length:c]; [h nvmtlReleaseSubAllocation:c]; }
}
- (MTLTextureUsage)usage { if (_usage) return (MTLTextureUsage)_usage; if (_parent) return [_parent usage]; return MTLTextureUsageShaderRead; }
- (id<MTLBuffer>)buffer { return _backBuf ? _backBuf : _parent ? [_parent buffer] : nil; }
- (NSUInteger)bufferOffset { return _backBuf ? _backOff : _parent ? [_parent bufferOffset] : 0; }
- (NSUInteger)bufferBytesPerRow { return _backBuf ? _backBPR : _parent ? [_parent bufferBytesPerRow] : 0; }
- (BOOL)isShareable { return _parent ? NO : _shareable; }
- (MTLTextureSwizzleChannels)swizzle { return _hasSwz ? _swz : MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha); }
- (BOOL)isSparse { NVMTLTexture *r = _parent ? _parent : self; return (r->_heap && r->_heap->_type == MTLHeapTypeSparse) || (r->_subHeap && r->_subHeap->_type == MTLHeapTypeSparse); }
// None even for a texture on a sparse heap (isSparse YES): such a heap is backed in full here and nothing maps tiles
// (resourceStateCommandEncoder is nil), so none of tier 1's partial backing, defined unbacked reads or residency
// feedback exists.
- (MTLTextureSparseTier)sparseTextureTier API_AVAILABLE(macos(26.0)) { return MTLTextureSparseTierNone; }
- (uint32_t)swizzleKey { MTLTextureSwizzleChannels c = [self swizzle]; return (uint32_t)c.red | (uint32_t)c.green << 8 | (uint32_t)c.blue << 16 | (uint32_t)c.alpha << 24; }
- (BOOL)allowGPUOptimizedContents { return YES; }
- (MTLTextureCompressionType)compressionType { return MTLTextureCompressionTypeLossless; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s {
    if (_parent || _viewParent || _backBuf || _heap || _subHeap) return MTLPurgeableStateNonVolatile;
    MTLPurgeableState prev = nvmtl_purge(&_purge, s);
    nvmtl_purge_track(self, s);
    if (s == MTLPurgeableStateNonVolatile) {
        int one = 1;
        if (__atomic_compare_exchange_n(&_rstate, &one, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            int r = __atomic_load_n(&_resRefs, __ATOMIC_ACQUIRE) ? 1 : nvmtl_vk_image_reback(&_i, 0, _fmt == MTLPixelFormatA8Unorm);
            nvlog("residency (res1): a purged %ux%u texture was made NonVolatile -> %s", _i.w, _i.h,
                  r == 0 ? "paged back onto the card" : r == 1 ? "stays in system memory (the wall or a command buffer said no)" : "rebuild FAILED - stays in system memory");
            __atomic_store_n(&_rstate, 0, __ATOMIC_RELEASE);
        }
    }
    return prev;
}
- (void)addDebugMarker:(NSString *)m range:(NSRange)r {}
- (void)removeAllDebugMarkers {}
- (id)newSharedTextureHandle {
    if (_parent || !_shareable || !_surf) return nil;
    if (![MTLSharedTextureHandle instancesRespondToSelector:@selector(initWithIOSurface:label:)]) {
        static int said; if (!said++) nvlog("newSharedTextureHandle: MTLSharedTextureHandle has no -initWithIOSurface:label: on this OS - nil (apps1)");
        return nil; }
    MTLSharedTextureHandle *h = [[MTLSharedTextureHandle alloc] initWithIOSurface:_surf label:_label];
    if (!h) { static int said; if (said++ < 4) nvlog("newSharedTextureHandle: Metal refused surface %u (its kMetalRegistryID names no live device) - nil (apps1)",
                                                   IOSurfaceGetID(_surf)); }
    return h;
}
- (id<MTLTexture>)remoteStorageTexture { return nil; }
- (id<MTLTexture>)newRemoteTextureViewForDevice:(id<MTLDevice>)d {
    return nil; }
// An app's region, level and slice are 64-bit; the copy path below takes 32-bit. A value that does not fit
// describes no texture, and narrowing it produces a DIFFERENT region that then passes every bounds check that
// follows: an origin of 2^32 reads as 0, so the copy lands at the start of the texture instead of being
// refused. The texture-buffer paths also add origin and width together, which wraps for an origin near 2^64
// and would memcpy far outside the buffer. Apple raises an exception on a region like this; refuse it here,
// before anything is narrowed.
static BOOL nvmtl_region_fits(MTLRegion r, NSUInteger level, NSUInteger slice) {
    return r.origin.x <= UINT32_MAX && r.origin.y <= UINT32_MAX && r.origin.z <= UINT32_MAX
        && r.size.width <= UINT32_MAX && r.size.height <= UINT32_MAX && r.size.depth <= UINT32_MAX
        && level <= UINT32_MAX && slice <= UINT32_MAX;
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level {
    if (!nvmtl_region_fits(region, level, 0)) { nvlog("getBytes: the region does not fit the copy path — REFUSED"); return; }
    if (level >= [self mipmapLevelCount]) { nvlog("getBytes: mip level %lu — this texture has %lu", (unsigned long)level, (unsigned long)[self mipmapLevelCount]); return; }
    [self nvmtlSurfaceIn];
    nvmtl_census("getBytes", (uint32_t)region.size.width, (uint32_t)region.size.height, _surf != NULL);
    if (nvmtl_vk_image_read_region_level_layer(_q, [self nvi], (uint8_t *)bytes, row, (uint32_t)region.origin.x, (uint32_t)region.origin.y,
                                               (uint32_t)region.size.width, (uint32_t)region.size.height, (uint32_t)level + _baseLevel, _baseSlice))
        nvlog("texture getBytes: readback FAILED");
}
- (void)getBytes:(void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)img
      fromRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice {
    if (!nvmtl_region_fits(region, level, slice)) { nvlog("getBytes:slice: the region does not fit the copy path — REFUSED"); return; }
    if (_tbView) {
        uint8_t *src = (uint8_t *)[(id<MTLBuffer>)_backBuf contents];
        if (!src || region.origin.x > _i.w || region.size.width > _i.w - region.origin.x) { nvlog("getBytes: texture buffer is not CPU-visible or the region is outside it"); return; }
        memcpy(bytes, src + _backOff + region.origin.x * _i.bpp, region.size.width * _i.bpp); return; }
    if (level >= [self mipmapLevelCount]) { nvlog("getBytes: mip level %lu — this texture has %lu", (unsigned long)level, (unsigned long)[self mipmapLevelCount]); return; }
    [self nvmtlSurfaceIn];
    nvk_image *im = [self nvi]; BOOL vol = im->mtl_type == 7; NSUInteger nz = vol ? (region.size.depth ? region.size.depth : 1) : 1;
    NSUInteger per = img ? img : row * region.size.height; int rc = 0;
    for (NSUInteger zi = 0; zi < nz && !rc; zi++)
        rc = nvmtl_vk_image_read_region_level_layer(_q, im, (uint8_t *)bytes + zi * per, row, (uint32_t)region.origin.x, (uint32_t)region.origin.y,
                                                    (uint32_t)region.size.width, (uint32_t)region.size.height, (uint32_t)level + _baseLevel, (uint32_t)(vol ? region.origin.z + zi : slice + _baseSlice));
    if (rc) nvlog("texture getBytes:slice: readback FAILED");
}
// CoreAnimation uploads only level 0 of the mipmapped texture it draws a Dock badge digit into and samples it trilinear,
// so the levels nobody wrote need filling; a write to a higher level or generateMipmaps sets _mipsWritten and ends that.
// CA does this for every image it minifies with kCAFilterTrilinear (it asks for a mipmapped linear texture from the
// image's buffer first, which is refused, then writes level 0 of a plain one and never calls generateMipmaps).
static void nvmtl_mip_autofill(NVMTLTexture *t, const void *bytes, size_t row, MTLRegion region, NSUInteger level) {
    NVMTLTexture *r = t->_parent ?: t;
    if (level + t->_baseLevel > 0) { r->_mipsWritten = YES; return; }
    NSUInteger mips = t.mipmapLevelCount; if (mips <= 1 || r->_mipsWritten || t->_parent || !bytes || !row) return;
    nvk_image *im = [t nvi]; if (!im) return; uint32_t w = im->w, h = im->h;
    if (im->mtl_type != 2 || im->bw > 1 || (im->layers ? im->layers : 1) > 1) return;
    if (region.origin.x || region.origin.y || region.size.width != w || region.size.height != h) return;
    // On the GPU when the format blits: every format and size, and sRGB levels averaged in linear light as Apple's are.
    // WAS 8-bit formats up to 2048 only, on the CPU: a 16-bit or half-float image (CA keeps wide-colour images in
    // RGBA16Float) kept levels nobody wrote, and sRGB levels were averaged in gamma space (too dark).
    if (nvmtl_vk_format_mip_blit(im->fmt)) {
        if (nvmtl_vk_image_gen_mipmaps(t->_q, im)) nvlog("mip autofill: GPU generation FAILED (%ux%u, VkFormat %u)", w, h, im->fmt);
        return;
    }
    if (w > 2048 || h > 2048) return;
    unsigned bpp; switch (t.pixelFormat) {
        case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: bpp = 1; break;
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: bpp = 2; break;
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB: bpp = 4; break;
        default: return; }
    if (bpp != im->bpp) return;
    size_t cap = (size_t)((w + 1) / 2) * ((h + 1) / 2) * bpp; uint8_t *cur = malloc(cap), *nxt = malloc(cap);
    if (!cur || !nxt) { free(cur); free(nxt); return; }
    const uint8_t *src = bytes; size_t srow = row; uint32_t sw = w, sh = h;
    for (NSUInteger l = 1; l < mips; l++) {
        uint32_t dw = sw > 1 ? sw / 2 : 1, dh = sh > 1 ? sh / 2 : 1;
        for (uint32_t y = 0; y < dh; y++) for (uint32_t x = 0; x < dw; x++) {
            uint32_t x0 = 2 * x, x1 = 2 * x + 1 < sw ? 2 * x + 1 : x0, y0 = 2 * y, y1 = 2 * y + 1 < sh ? 2 * y + 1 : y0;
            const uint8_t *p00 = src + (size_t)y0 * srow + (size_t)x0 * bpp, *p01 = src + (size_t)y0 * srow + (size_t)x1 * bpp;
            const uint8_t *p10 = src + (size_t)y1 * srow + (size_t)x0 * bpp, *p11 = src + (size_t)y1 * srow + (size_t)x1 * bpp;
            uint8_t *d = nxt + ((size_t)y * dw + x) * bpp;
            for (unsigned c = 0; c < bpp; c++) d[c] = (uint8_t)((p00[c] + p01[c] + p10[c] + p11[c] + 2) / 4); }
        if (nvmtl_vk_image_write_region_level_layer(t->_q, im, nxt, (size_t)dw * bpp, 0, 0, dw, dh, (uint32_t)l, t->_baseSlice)) break;
        uint8_t *tmp = cur; cur = nxt; nxt = tmp; src = cur; srow = (size_t)dw * bpp; sw = dw; sh = dh;
    }
    free(cur); free(nxt);
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row {
    if (!nvmtl_region_fits(region, level, 0)) { nvlog("replaceRegion: the region does not fit the copy path — REFUSED"); return; }
    if (level >= [self mipmapLevelCount]) { nvlog("replaceRegion: mip level %lu — this texture has %lu", (unsigned long)level, (unsigned long)[self mipmapLevelCount]); return; }
    if (region.size.depth > 1 || region.origin.z) { [self replaceRegion:region mipmapLevel:level slice:0 withBytes:bytes bytesPerRow:row bytesPerImage:row * region.size.height]; return; }
    { int was = nvtrace_scope([self nvi]->w, [self nvi]->h);
      if (nvmtl_trace_on()) { unsigned nz, nn; size_t valid = (size_t)region.size.width * [self nvi]->bpp; if (row && valid > row) valid = row;
        nvmtl_nonzero(bytes, row, valid, (uint32_t)region.size.height, &nz, &nn);
        nvtrace("REPLACE %lux%lu at %lu,%lu level %lu row %lu -> tex %ux%u surf %u | nonzero %u/%u sampled bytes", (unsigned long)region.size.width,
            (unsigned long)region.size.height, (unsigned long)region.origin.x, (unsigned long)region.origin.y, (unsigned long)level, (unsigned long)row,
            [self nvi]->w, [self nvi]->h, nvmtl_surf_id(self), nz, nn); }
      g_trace_mute = was; }
    if (!bytes || row < (size_t)_i.w * (_i.bpp ? _i.bpp : 4) || !nvmtl_surface_full_overwrite(self, region, level, 0)) [self nvmtlSurfaceIn];
    if (nvmtl_vk_image_write_region_level_layer(_q, [self nvi], (const uint8_t *)bytes, row, (uint32_t)region.origin.x, (uint32_t)region.origin.y,
                                                (uint32_t)region.size.width, (uint32_t)region.size.height, (uint32_t)level + _baseLevel, _baseSlice))
        nvlog("replaceRegion: upload FAILED");
    else { [self nvmtlSurfaceOut]; nvmtl_mip_autofill(self, bytes, row, region, level); }
}
- (void)replaceRegion:(MTLRegion)region mipmapLevel:(NSUInteger)level slice:(NSUInteger)slice
            withBytes:(const void *)bytes bytesPerRow:(NSUInteger)row bytesPerImage:(NSUInteger)img {
    if (!nvmtl_region_fits(region, level, slice)) { nvlog("replaceRegion:slice: the region does not fit the copy path — REFUSED"); return; }
    if (_tbView) {
        id<MTLBuffer> bb = (id<MTLBuffer>)_backBuf; uint8_t *dst = (uint8_t *)[bb contents];
        if (!dst || region.origin.x > _i.w || region.size.width > _i.w - region.origin.x) { nvlog("replaceRegion: texture buffer is not CPU-visible or the region is outside it"); return; }
        const NSUInteger at = _backOff + region.origin.x * _i.bpp, len = region.size.width * _i.bpp; memcpy(dst + at, bytes, len);
        if (bb.storageMode == MTLStorageModeManaged) [bb didModifyRange:NSMakeRange(at, len)];
        return; }
    if (level >= [self mipmapLevelCount]) { nvlog("replaceRegion: mip level %lu — this texture has %lu", (unsigned long)level, (unsigned long)[self mipmapLevelCount]); return; }
    if (!bytes || row < (size_t)_i.w * (_i.bpp ? _i.bpp : 4) || !nvmtl_surface_full_overwrite(self, region, level, slice)) [self nvmtlSurfaceIn];
    nvk_image *im = [self nvi]; BOOL vol = im->mtl_type == 7; NSUInteger nz = vol ? (region.size.depth ? region.size.depth : 1) : 1;
    if (!vol && (region.size.depth > 1 || region.origin.z)) { nvlog("replaceRegion:slice: depth %lu@z%lu on a non-3D texture — REFUSED", (unsigned long)region.size.depth, (unsigned long)region.origin.z); return; }
    NSUInteger per = img ? img : row * region.size.height; int rc = 0;
    for (NSUInteger zi = 0; zi < nz && !rc; zi++)
        rc = nvmtl_vk_image_write_region_level_layer(_q, im, (const uint8_t *)bytes + zi * per, row, (uint32_t)region.origin.x, (uint32_t)region.origin.y,
                                                     (uint32_t)region.size.width, (uint32_t)region.size.height, (uint32_t)level + _baseLevel, (uint32_t)(vol ? region.origin.z + zi : slice + _baseSlice));
    if (rc) nvlog("replaceRegion:slice: upload FAILED");
    else { [self nvmtlSurfaceOut]; if (!vol) nvmtl_mip_autofill(self, bytes, row, region, level); }
}
// A view of a texture buffer is another texel-buffer view of the same bytes. Apple's GL asks for one whenever a buffer
// texture is sampled through a sampler of another type (Minecraft 26's R8Sint/R32Float buffer textures), every draw
// until it gets one.
static NVMTLTexture *nvmtl_texbuf_view(NVMTLTexture *t, MTLPixelFormat f) {
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (!t->_backBuf || nvmtl_pixfmt_public(f, &vkf, &bpp, &a8) || a8 || !bpp || !t->_backBPR || t->_backBPR % bpp) {
        nvlog("newTextureView: texture-buffer view as format %lu is not carried -> nil", (unsigned long)f); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:f width:t->_backBPR / bpp
                                                                            resourceOptions:[t->_backBuf resourceOptions] usage:t->_usage];
    return nvmtl_texbuf_make(t->_backBuf, d, t->_backOff);
}
- (id<MTLTexture>)nvmtlViewWithFormat:(MTLPixelFormat)f levels:(NSRange)levels {
    if (!levels.length || levels.location >= self.mipmapLevelCount || levels.length > self.mipmapLevelCount - levels.location) return nil;
    if ([self nvi]->mtl_type == 9) return nvmtl_texbuf_view(self, f);
    NVMTLTexture *v = [NVMTLTexture new];
    { NVMTLTexture *vp_ = _parent ? _parent : self; __atomic_fetch_add(&vp_->_nviews, 1, __ATOMIC_ACQ_REL); }  v->_parent = _parent ? _parent : self; v->_usage = [self usage];
    v->_q = _q; v->_fmt = f; v->_surf = _surf; v->_plane = _plane;
    v->_mips = (uint32_t)levels.length;
    v->_viewParent = self; v->_relativeLevel = (uint32_t)levels.location;
    v->_viewType = (uint32_t)self.textureType;
    v->_viewSlices = _parent ? _viewSlices : (v->_viewType == 7 ? 1 : ([self nvi]->layers ?: 1));
    v->_baseSlice = _baseSlice; v->_baseLevel = _baseLevel + (uint32_t)levels.location;
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    extern int nvmtl_depthfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *aspect);
    uint32_t dvk = 0, dasp = 0;
    if (nvmtl_depthfmt_public(f, &dvk, &dasp) == 0) {
        if (nvmtl_vk_image_view_create_range_aspect([v nvi], dvk, 0, dasp, v->_baseLevel, v->_mips, v->_baseSlice, v->_viewSlices, v->_viewType, &v->_ownView)) { nvlog("newTextureView: depth/stencil view FAILED (format %lu)", (unsigned long)f); return nil; }
        v->_stex = NO;
        nvlog("newTextureView: depth/stencil view format %lu aspect %u -> %p", (unsigned long)f, dasp, (__bridge void *)v);
        return v;
    }
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_pixfmt_public(f, &vkf, &bpp, &a8)) return nil;
    if (vkf != [v nvi]->fmt && bpp != [v nvi]->bpp) {
        nvlog("newTextureView: pixel format %lu is %u B per element, its texture's is %u B - nil", (unsigned long)f, bpp, [v nvi]->bpp); return nil; }
    if (nvmtl_vk_image_view_create_range([v nvi], vkf, a8, v->_baseLevel, v->_mips, v->_baseSlice, v->_viewSlices, v->_viewType, &v->_ownView)) {
        nvlog("newTextureView: view create FAILED"); return nil;
    }
    v->_stex = [v nvi]->storage && !a8 && nvmtl_vk_format_is_storage(vkf);
    return v;
}
int nvmtl_vk_cmd_attachment_view_fmt(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, uint32_t vkfmt, int a8,
                                     void **out, uint32_t *w, uint32_t *h);
static uint32_t nvmtl_tex_attach_vkfmt(NVMTLTexture *t, int *a8) {
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    uint32_t vk = 0, bpp = 0; *a8 = 0;
    if (!t->_fmt || nvmtl_pixfmt_public(t->_fmt, &vk, &bpp, a8)) { *a8 = 0; return 0; }
    return vk;
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)f {
    return [self nvmtlViewWithFormat:f levels:NSMakeRange(0, [self mipmapLevelCount])];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)tt
                                         levels:(NSRange)levels slices:(NSRange)slices {
    NSUInteger availableSlices = self.textureType == MTLTextureTypeCube ? 6 : self.textureType == MTLTextureTypeCubeArray ? self.arrayLength * 6 : self.arrayLength;
    if (!levels.length || levels.location >= self.mipmapLevelCount || levels.length > self.mipmapLevelCount - levels.location ||
        !slices.length || slices.location >= availableSlices || slices.length > availableSlices - slices.location) return nil;
    if (tt == MTLTextureType2D && !slices.location && slices.length <= 1 && [self nvi]->mtl_type == 2) return [self nvmtlViewWithFormat:f levels:levels];
    if (tt == MTLTextureType2DMultisample && !slices.location && slices.length <= 1 && [self nvi]->mtl_type == 4) return [self nvmtlViewWithFormat:f levels:levels];
    if (tt == MTLTextureTypeTextureBuffer || [self nvi]->mtl_type == 9)
        return tt == MTLTextureTypeTextureBuffer && [self nvi]->mtl_type == 9 ? nvmtl_texbuf_view(self, f) : nil;
    if (tt == MTLTextureType3D && [self nvi]->mtl_type == 7 && !slices.location) slices = NSMakeRange(0, 1);
    if (((tt == MTLTextureType1D || tt == MTLTextureType1DArray) && [self nvi]->mtl_type != (uint32_t)tt) || (tt == MTLTextureType2DMultisample && ([self nvi]->mtl_type != 4 && [self nvi]->mtl_type != 8)) || (tt == MTLTextureType2DMultisampleArray && [self nvi]->mtl_type != 8) || (([self nvi]->mtl_type == 4 || [self nvi]->mtl_type == 8) && tt != MTLTextureType2DMultisample && tt != MTLTextureType2DMultisampleArray) || (tt == MTLTextureType3D && [self nvi]->mtl_type != 7)) { nvlog("newTextureView: textureType %lu of a %u-typed texture — not carried", (unsigned long)tt, [self nvi]->mtl_type); return nil; }
    NVMTLTexture *v = [NVMTLTexture new];
    { NVMTLTexture *vp_ = _parent ? _parent : self; __atomic_fetch_add(&vp_->_nviews, 1, __ATOMIC_ACQ_REL); }  v->_parent = _parent ? _parent : self; v->_usage = [self usage];  v->_q = _q; v->_fmt = f; v->_surf = _surf; v->_plane = _plane; v->_mips = (uint32_t)levels.length;
    v->_viewParent = self; v->_relativeLevel = (uint32_t)levels.location; v->_relativeSlice = (uint32_t)slices.location;
    v->_viewType = (uint32_t)tt; v->_viewSlices = (uint32_t)slices.length;
    v->_baseSlice = _baseSlice + (uint32_t)slices.location; v->_baseLevel = _baseLevel + (uint32_t)levels.location;
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    extern int nvmtl_depthfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *aspect);
    uint32_t depthFormat = 0, depthAspect = 0;
    if (!nvmtl_depthfmt_public(f, &depthFormat, &depthAspect)) {
        if (depthFormat != [v nvi]->fmt || nvmtl_vk_image_view_create_range_aspect([v nvi], depthFormat, 0, depthAspect, v->_baseLevel, v->_mips, v->_baseSlice, v->_viewSlices, (uint32_t)tt, &v->_ownView)) return nil;
        v->_stex = NO;
        return v;
    }
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_pixfmt_public(f, &vkf, &bpp, &a8)) return nil;
    if (vkf != [v nvi]->fmt && bpp != [v nvi]->bpp) {
        nvlog("newTextureView: pixel format %lu is %u B per element, its texture's is %u B - nil", (unsigned long)f, bpp, [v nvi]->bpp); return nil; }
    uint32_t nslices = (uint32_t)slices.length;
    if (nvmtl_vk_image_view_create_range([v nvi], vkf, a8, v->_baseLevel, v->_mips, v->_baseSlice, nslices, (uint32_t)tt, &v->_ownView)) { nvlog("newTextureView: typed view FAILED"); return nil; }
    v->_stex = [v nvi]->storage && !a8 && nvmtl_vk_format_is_storage(vkf);
    return v;
}
// macOS 26: the same view as newTextureViewWithPixelFormat:textureType:levels:slices:swizzle:, described by an
// MTLTextureViewDescriptor. The descriptor is read by key because the plugin is built for macOS 15, where the class does
// not exist. A field left at zero means "as the texture is": no pixel format keeps the texture's, an empty level or slice
// range covers every level or slice, and an all-zero swizzle is the identity.
- (id<MTLTexture>)newTextureViewWithDescriptor:(id)d {
    if (!d) return nil;
    MTLPixelFormat f = [[d valueForKey:@"pixelFormat"] unsignedIntegerValue];
    MTLTextureType tt = (MTLTextureType)[[d valueForKey:@"textureType"] unsignedIntegerValue];
    NSRange levels = [[d valueForKey:@"levelRange"] rangeValue], slices = [[d valueForKey:@"sliceRange"] rangeValue];
    MTLTextureSwizzleChannels sw = { 0, 0, 0, 0 };
    { id v = [d valueForKey:@"swizzle"];
      if ([v isKindOfClass:[NSValue class]] && !strcmp([(NSValue *)v objCType], @encode(MTLTextureSwizzleChannels))) [(NSValue *)v getValue:&sw size:sizeof sw]; }
    if (f == MTLPixelFormatInvalid) f = self.pixelFormat;
    if (!levels.length) levels = NSMakeRange(0, self.mipmapLevelCount);
    if (!slices.length) slices = NSMakeRange(0, self.textureType == MTLTextureTypeCube ? 6 : self.textureType == MTLTextureTypeCubeArray ? self.arrayLength * 6 : self.arrayLength);
    if (!sw.red && !sw.green && !sw.blue && !sw.alpha) sw = MTLTextureSwizzleChannelsMake(MTLTextureSwizzleRed, MTLTextureSwizzleGreen, MTLTextureSwizzleBlue, MTLTextureSwizzleAlpha);
    return [self newTextureViewWithPixelFormat:f textureType:tt levels:levels slices:slices swizzle:sw];
}
- (id<MTLTexture>)newTextureViewWithPixelFormat:(MTLPixelFormat)f textureType:(MTLTextureType)tt
                                         levels:(NSRange)levels slices:(NSRange)slices
                                        swizzle:(MTLTextureSwizzleChannels)sw {
    const BOOL identity = sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
    if (!identity && [self nvi]->mtl_type == 9) {   // a Vulkan texel buffer has no component mapping
        static int said; if (!said++) nvlog("newTextureView:swizzle: a texture buffer cannot be swizzled -> nil (said once)");
        return nil; }
    NVMTLTexture *v = (NVMTLTexture *)[self newTextureViewWithPixelFormat:f textureType:tt levels:levels slices:slices];
    if (!v || identity) return v;
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    uint32_t vkf = 0, bpp = 0; int a8 = 0; void *nv = NULL;
    const uint8_t s4[4] = { (uint8_t)sw.red, (uint8_t)sw.green, (uint8_t)sw.blue, (uint8_t)sw.alpha };
    if (nvmtl_pixfmt_public(f, &vkf, &bpp, &a8) ||
        nvmtl_vk_image_view_create_range_swz([v nvi], vkf, a8, v->_baseLevel, v->_mips, v->_baseSlice, v->_viewSlices, v->_viewType, s4, &nv)) {
        nvlog("newTextureView:swizzle: par1 the swizzled view could not be built (format %lu) — the view keeps the identity mapping", (unsigned long)f);
        return v; }
    if (v->_ownView) nvmtl_vk_image_view_destroy(v->_ownView);
    v->_ownView = nv; v->_stex = NO;
    v->_swz = sw; v->_hasSwz = YES;
    nvlog("newTextureView:swizzle: par1 view %p maps r,g,b,a <- %u,%u,%u,%u", (__bridge void *)v, s4[0], s4[1], s4[2], s4[3]);
    return v;
}
@end

NSArray<NSDictionary *> *nvmtl_argbuf_fields(NVMTLFunction *function, NSUInteger ownerBufferIndex);
NSArray<NSDictionary *> *nvmtl_air_argbuf_layout(NVMTLFunction *fn, NSUInteger bufferIndex, NSUInteger *size, NSUInteger *align);
BOOL nvmtl_needs_visible_link(NSString *air, NSString *why);
BOOL nvmtl_needs_visible_link_once(NSString *air);
void nvmtl_fc_expect_link(int on);
NSData *nvmtl_translate_with_fc(NSString *air, NSString *stage, const uint32_t *idx, const uint32_t *sizes,
                                const uint8_t *const *payloads, size_t n, NSString **err);
NSData *nvmtl_translate_with_fc_cached(NSString *air, NSString *stage, NSString *name, NSDictionary *constants,
                                       const uint32_t *idx, const uint32_t *sizes,
                                       const uint8_t *const *payloads, size_t n, NSString **err);
int nvmtl_fc_spec_wanted(NSString *stage);
NSData *nvmtl_translate_fc_spec_cached(NSString *ll, NSString *stage, NSString *name, const uint32_t *idx,
                                       const uint32_t *sizes, const uint8_t *const *payloads, size_t n, NSString **err);
NSData *nvmtl_fc_spec_variant(NSData *spec, const uint32_t *idx, const uint32_t *sizes, const uint8_t *const *payloads, size_t n);

#pragma mark - library / function
id gNVMTLMainDevice = nil;

extern NSData *nvmtl_translate_samplers(NVMTLFunction *, NSDictionary *, uint32_t, NSString **);
extern NSData *nvmtl_translate_variant(NVMTLFunction *, NSDictionary *, NSDictionary *, NSString **);
@interface NVMTLCIBinding : NSObject { @public NSString *_name; NVMTLType *_t; } @end
@implementation NVMTLCIBinding
- (NSString *)name { return _name ?: @""; }
- (NSUInteger)type { return 18; }
- (MTLBindingAccess)access { return MTLBindingAccessReadOnly; }
- (NSUInteger)index { return 0; }
- (BOOL)isUsed { return YES; }
- (BOOL)isActive { return YES; }
- (BOOL)isArgument { return NO; }
- (NSUInteger)arrayLength { return 1; }
- (id)dataTypeDescription { return _t; }
- (MTLDataType)dataType { return _t ? _t->_dataType : MTLDataTypeNone; }
- (NSString *)description { return [NSString stringWithFormat:@"<CI binding %@ dataType %lu>", _name, (unsigned long)self.dataType]; }
@end
@interface NVMTLCIStructType : NVMTLStructType { @public NSString *_typeName; } @end
@implementation NVMTLCIStructType
- (NSString *)typeName { return _typeName ?: @""; }
@end
static NSString *nvmtl_ci_trim(NSString *t) { return [t stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"!\" "]]; }
static NSString *nvmtl_ci_wtrim(NSString *t) { return [t stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]]; }
@interface NVMTLCIStitchBinding : NVMTLCIBinding { @public NSUInteger _bt; } @end
@implementation NVMTLCIStitchBinding
- (NSUInteger)type { return _bt; }
- (BOOL)isUsed { return NO; }
- (BOOL)isActive { return NO; }
@end
static MTLDataType nvmtl_datatype_for_air_name(NSString *t);
static char nvmtl_ci_key;
static NSString *nvmtl_ci_body(NSDictionary *nodes, NSString *ref) { return [ref isKindOfClass:[NSString class]] ? nodes[ref] : nil; }
static NSString *nvmtl_ci_quoted(NSString *body, NSString *tag) {
    NSString *needle = [NSString stringWithFormat:@"!\"%@\", !\"", tag];
    NSRange r = [body rangeOfString:needle]; if (r.location == NSNotFound) return nil;
    NSUInteger from = NSMaxRange(r);
    NSRange q = [body rangeOfString:@"\"" options:0 range:NSMakeRange(from, body.length - from)];
    return q.location == NSNotFound ? nil : [body substringWithRange:NSMakeRange(from, q.location - from)];
}
static NSArray *nvmtl_ci_refs(NSString *body) {
    NSMutableArray *out = [NSMutableArray new];
    for (NSString *p in [body componentsSeparatedByString:@","]) {
        NSString *t = [p stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
        if ([t hasPrefix:@"!"] && ![t hasPrefix:@"!\""]) [out addObject:t];
    }
    return out;
}
static NSDictionary *nvmtl_ci_entry(NVMTLFunction *fn)
{
    if (!fn || ![fn->_stage isEqualToString:@"visible"]) return nil;
    @synchronized (fn) {
        id have = objc_getAssociatedObject(fn, &nvmtl_ci_key);
        if (have) return have == [NSNull null] ? nil : have;
        NSDictionary *found = nil;
        NSString *ll = fn->_air, *fname = fn->_fname;
        if (ll.length && fname.length && [ll rangeOfString:@"\n!air.ci = "].location != NSNotFound) {
            NSMutableDictionary *nodes = [NSMutableDictionary new]; NSString *list = nil;
            for (NSString *line in [ll componentsSeparatedByString:@"\n"]) {
                if (![line hasPrefix:@"!"]) continue;
                NSRange eq = [line rangeOfString:@" = !{"]; if (eq.location == NSNotFound || ![line hasSuffix:@"}"]) continue;
                NSString *body = [line substringWithRange:NSMakeRange(NSMaxRange(eq), line.length - NSMaxRange(eq) - 1)];
                NSString *key = [line substringToIndex:eq.location];
                if ([key isEqualToString:@"!air.ci"]) list = body; else nodes[key] = body;
            }
            NSString *head = [NSString stringWithFormat:@"ptr @%@,", fname];
            for (NSString *ref in nvmtl_ci_refs(list ?: @"")) {
                NSString *entry = nodes[ref]; if (![entry hasPrefix:head]) continue;
                NSArray *parts = nvmtl_ci_refs([entry substringFromIndex:head.length]);
                if (parts.count < 2) break;
                NSString *retBody = nvmtl_ci_body(nodes, parts[0]);
                if (retBody && !nvmtl_ci_quoted(retBody, @"air.arg_type_name")) retBody = nvmtl_ci_body(nodes, nvmtl_ci_refs(retBody).firstObject);
                NVMTLType *ret = [NVMTLType new];
                NSString *rtn = retBody ? nvmtl_ci_quoted(retBody, @"air.arg_type_name") : nil;
                ret->_dataType = rtn ? nvmtl_datatype_for_air_name(rtn) : MTLDataTypeFloat4;
                NSMutableArray *args = [NSMutableArray new]; NSMutableString *say = [NSMutableString new];
                for (NSString *aref in nvmtl_ci_refs(nvmtl_ci_body(nodes, parts[1]) ?: @"")) {
                    NSString *ab = nodes[aref]; if (!ab) continue;
                    NSString *tn = nvmtl_ci_quoted(ab, @"air.arg_type_name") ?: @"";
                    BOOL builtin = [ab rangeOfString:@"!\"air.ci_builtin\""].location != NSNotFound;
                    NVMTLCIBinding *b = [NVMTLCIBinding new]; b->_t = [NVMTLType new];
                    b->_name = nvmtl_ci_quoted(ab, @"air.arg_name") ?: [NSString stringWithFormat:@"arg%lu", (unsigned long)args.count];
                    MTLDataType dt = nvmtl_datatype_for_air_name(tn);
                    if (!builtin && dt != MTLDataTypeTexture && dt != MTLDataTypeSampler) dt = MTLDataTypePointer;
                    b->_t->_dataType = dt;
                    [args addObject:b]; [say appendFormat:@" %@:%@=%lu", b->_name, tn, (unsigned long)dt];
                }
                found = @{ @"args": args, @"ret": ret };
                nvlog("CI REFL %s: functionType 4, returns %lu, %lu argument(s)%s", fname.UTF8String, (unsigned long)ret->_dataType, (unsigned long)args.count, say.UTF8String);
                break;
            }
        }
        objc_setAssociatedObject(fn, &nvmtl_ci_key, found ?: [NSNull null], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return found;
    }
}
static MTLDataType nvmtl_ci_air_dtype(NSDictionary *nodes, NSString *ref) {
    NSString *b = ref ? nodes[ref] : nil; if (!b) return MTLDataTypeNone;
    NSArray *p = [b componentsSeparatedByString:@", "];
    NSUInteger n = 1; NSString *el = b;
    if ([b hasPrefix:@"!\"air.vector_type\""] && p.count >= 6) { n = (NSUInteger)[[p[5] stringByReplacingOccurrencesOfString:@"i32 " withString:@""] integerValue]; el = nodes[nvmtl_ci_wtrim(p[4])] ?: @""; }
    NSArray *q = [el componentsSeparatedByString:@", "];
    NSInteger sz = q.count > 1 ? [[q[1] stringByReplacingOccurrencesOfString:@"i32 " withString:@""] integerValue] : 0;
    if (n < 1 || n > 4) return MTLDataTypeNone;
    if ([el hasPrefix:@"!\"air.float_type\""]) return (MTLDataType)((sz == 2 ? MTLDataTypeHalf : MTLDataTypeFloat) + n - 1);
    return MTLDataTypeNone;
}
static char nvmtl_ci_stitch_key;
static NSDictionary *nvmtl_ci_stitch_entry(NVMTLFunction *fn)
{
    if (!fn || ![fn->_stage isEqualToString:@"visible"]) return nil;
    @synchronized (fn) {
        id have = objc_getAssociatedObject(fn, &nvmtl_ci_stitch_key);
        if (have) return have == [NSNull null] ? nil : have;
        NSDictionary *found = nil;
        NSString *ll = fn->_air, *fname = fn->_fname;
        if (ll.length && fname.length && [ll rangeOfString:@"\n!air.visible = "].location != NSNotFound
            && [ll rangeOfString:@"!\"air.stitching_info\""].location != NSNotFound) {
            NSMutableDictionary *nodes = [NSMutableDictionary new]; NSString *list = nil;
            for (NSString *line in [ll componentsSeparatedByString:@"\n"]) {
                if (![line hasPrefix:@"!"]) continue;
                NSRange eq = [line rangeOfString:@" = !{"]; if (eq.location == NSNotFound || ![line hasSuffix:@"}"]) continue;
                NSString *body = [line substringWithRange:NSMakeRange(NSMaxRange(eq), line.length - NSMaxRange(eq) - 1)];
                NSString *key = [line substringToIndex:eq.location];
                if ([key isEqualToString:@"!air.visible"]) list = body; else nodes[key] = body;
            }
            NSString *head = [NSString stringWithFormat:@"ptr @%@,", fname];
            for (NSString *ref in nvmtl_ci_refs(list ?: @"")) {
                NSString *entry = nodes[ref]; if (![entry hasPrefix:head]) continue;
                NSArray *parts = nvmtl_ci_refs([entry substringFromIndex:head.length]);
                if (parts.count < 3) break;
                NSString *outs = nvmtl_ci_body(nodes, parts[0]), *ins = nvmtl_ci_body(nodes, parts[1]), *si = nvmtl_ci_body(nodes, parts[2]);
                if (![si hasPrefix:@"!\"air.stitching_info\""]) break;
                NSString *ob = nvmtl_ci_body(nodes, nvmtl_ci_refs(outs ?: @"").firstObject);
                NVMTLType *ret = [NVMTLType new];
                NSString *rtn = ob ? nvmtl_ci_quoted(ob, @"air.arg_type_name") : nil;
                ret->_dataType = rtn ? nvmtl_datatype_for_air_name(rtn) : MTLDataTypeFloat4;
                NSArray *inRefs = nvmtl_ci_refs(ins ?: @""), *siRefs = nvmtl_ci_refs(si);
                NSMutableArray *args = [NSMutableArray new]; NSMutableString *say = [NSMutableString new];
                for (NSUInteger k = 1; k < siRefs.count; k++) {
                    NSString *sa = nvmtl_ci_body(nodes, siRefs[k]);
                    if (![sa hasPrefix:@"!\"air.stitching_argument\""]) continue;
                    NSArray *sp = [sa componentsSeparatedByString:@", "];
                    NSString *nm = sp.count >= 3 ? [sp[2] stringByTrimmingCharactersInSet:[NSCharacterSet characterSetWithCharactersInString:@"!\" "]] : nil;
                    NSString *st = nvmtl_ci_body(nodes, sp.count >= 2 ? [sp[1] stringByTrimmingCharactersInSet:[NSCharacterSet whitespaceCharacterSet]] : nil);
                    NSString *tt = nvmtl_ci_body(nodes, nvmtl_ci_refs(st ?: @"").firstObject);
                    NVMTLCIStitchBinding *b = [NVMTLCIStitchBinding new]; b->_t = [NVMTLType new];
                    b->_name = nm.length ? nm : [NSString stringWithFormat:@"arg%lu", (unsigned long)args.count];
                    if ([tt hasPrefix:@"!\"air.struct_type\""]) {
                        b->_bt = 29;
                        NVMTLCIStructType *T = [NVMTLCIStructType new]; T->_dataType = MTLDataTypeStruct;
                        NSArray *tp = [tt componentsSeparatedByString:@", "];
                        T->_typeName = tp.count >= 5 ? nvmtl_ci_trim(tp[4]) : @"";
                        NSString *fl = tp.count >= 6 ? nvmtl_ci_body(nodes, nvmtl_ci_wtrim(tp[5])) : nil;
                        NSMutableArray *ms = [NSMutableArray array];
                        for (NSString *fr in nvmtl_ci_refs(fl ?: @"")) {
                            NSString *fb = nvmtl_ci_body(nodes, fr); if (![fb hasPrefix:@"!\"air.record_field\""]) continue;
                            NSArray *fp = [fb componentsSeparatedByString:@", "];
                            NVMTLStructMember *m = [NVMTLStructMember new];
                            m->_offset = fp.count > 1 ? (NSUInteger)[[fp[1] stringByReplacingOccurrencesOfString:@"i32 " withString:@""] integerValue] : 0;
                            m->_name = fp.count > 4 ? nvmtl_ci_trim(fp[4]) : @"";
                            m->_dataType = nvmtl_ci_air_dtype(nodes, fp.count > 3 ? nvmtl_ci_wtrim(fp[3]) : nil);
                            m->_argIndex = ms.count; [ms addObject:m];
                        }
                        T->_members = ms; b->_t = T;
                    }
                    else {
                        NSUInteger pos = args.count;
                        NSString *ib = pos < inRefs.count ? nvmtl_ci_body(nodes, inRefs[pos]) : nil;
                        NSString *tn = ib ? nvmtl_ci_quoted(ib, @"air.arg_type_name") : nil;
                        b->_bt = 18; b->_t->_dataType = tn ? nvmtl_datatype_for_air_name(tn) : MTLDataTypeNone;
                    }
                    [args addObject:b]; [say appendFormat:@" %@:%lu/%lu", b->_name, (unsigned long)b->_bt, (unsigned long)b->_t->_dataType];
                }
                found = @{ @"args": args, @"ret": ret };
                nvlog("CI STITCH REFL %s: functionType 5, returns %lu, %lu argument(s)%s", fname.UTF8String, (unsigned long)ret->_dataType, (unsigned long)args.count, say.UTF8String);
                break;
            }
        }
        objc_setAssociatedObject(fn, &nvmtl_ci_stitch_key, found ?: [NSNull null], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        return found;
    }
}
@implementation NVMTLFunction
- (NSDictionary<NSString *, MTLFunctionConstant *> *)functionConstantsDictionary {
    static char key;
    NSDictionary *cached = objc_getAssociatedObject(self, &key);
    if (cached) return cached;
    NSMutableDictionary *d = [NSMutableDictionary new];
    Class ci = NSClassFromString(@"MTLFunctionConstantInternal");
    if (!_air.length) {
        static int said; if (!said++) nvlog("functionConstantsDictionary: \"%s\" has no retained AIR - answering EMPTY (said once)", _fname.UTF8String ?: "?");
    } else if (!ci || ![ci instancesRespondToSelector:@selector(initWithName:type:index:required:)]) {
        static int said; if (!said++) nvlog("functionConstantsDictionary: this Metal has no MTLFunctionConstantInternal initWithName:type:index:required: - answering EMPTY (said once)");
    } else {
        static NSRegularExpression *row, *any; static dispatch_once_t once;
        dispatch_once(&once, ^{
            row = [NSRegularExpression regularExpressionWithPattern:@"^![0-9]+ = !\\{[^\\n]*\\.MTL_FC_INIT_[^\\n]*, !\"([^\"]+)\", !\"([^\"]+)\", i32 ([0-9]+), i1 (true|false)\\}$" options:NSRegularExpressionAnchorsMatchLines error:NULL];
            any = [NSRegularExpression regularExpressionWithPattern:@"^![0-9]+ = !\\{[^\\n]*\\.MTL_FC_INIT_" options:NSRegularExpressionAnchorsMatchLines error:NULL];
        });
        NSRange all = NSMakeRange(0, _air.length);
        NSArray<NSTextCheckingResult *> *ms = [row matchesInString:_air options:0 range:all];
        NSUInteger rows = [any numberOfMatchesInString:_air options:0 range:all];
        if (rows != ms.count) { static int said; if (!said++) nvlog("functionConstantsDictionary: \"%s\" has %lu MTL_FC_INIT rows, %lu in the measured shape - the AIR row moved; the rest are NOT answered (said once)", _fname.UTF8String ?: "?", (unsigned long)rows, (unsigned long)ms.count); }
        for (NSTextCheckingResult *r in ms) {
            NSString *ty = [_air substringWithRange:[r rangeAtIndex:1]], *nm = [_air substringWithRange:[r rangeAtIndex:2]];
            NSUInteger ix = (NSUInteger)[[_air substringWithRange:[r rangeAtIndex:3]] longLongValue];
            BOOL req = [[_air substringWithRange:[r rangeAtIndex:4]] isEqualToString:@"true"];
            MTLDataType dt = nvmtl_air_fc_type(ty);
            if (dt == MTLDataTypeNone) { static int said; if (!said++) nvlog("functionConstantsDictionary: AIR type \"%s\" (constant %s) has no MTLDataType mapping - answered as None (said once)", ty.UTF8String, nm.UTF8String); }
            id c = [(id<NVMTLFunctionConstantInit>)[ci alloc] initWithName:nm type:(NSUInteger)dt index:ix required:req];
            if (c) d[nm] = c;
        }
    }
    NSDictionary *out = [d copy];
    objc_setAssociatedObject(self, &key, out, OBJC_ASSOCIATION_RETAIN);
    return out;
}
- (MTLFunctionOptions)options { return MTLFunctionOptionNone; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (const void *)bitCodeHash {
    if (!_bcHashed) {
        NSMutableData *d = [NSMutableData data];
        [d appendData:[(_fname ?: @"") dataUsingEncoding:NSUTF8StringEncoding]];
        if (_air.length)         [d appendData:[_air dataUsingEncoding:NSUTF8StringEncoding]];
        else if (_spirv.length)  [d appendData:_spirv];
        if (_fc.count)           [d appendData:[[_fc description] dataUsingEncoding:NSUTF8StringEncoding]];
        CC_SHA256(d.bytes, (CC_LONG)d.length, _bcHash);
        _bcHashed = YES;
    }
    return _bcHash;
}
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index {
    // The AIR layout names every member (constants and nested structs too) with Metal's [[id]] and offset; the
    // translator's fields add any resource the AIR walk did not reach.
    NSUInteger airSize = 0, airAlign = 0;
    NSMutableArray *fields = [nvmtl_air_argbuf_layout(self, index, &airSize, &airAlign) mutableCopy];
    NSMutableSet *ids = [NSMutableSet set]; for (NSDictionary *f in fields) [ids addObject:f[@"argument_index"]];
    for (NSDictionary *f in nvmtl_argbuf_fields(self, index)) if (![ids containsObject:@([f[@"argument_index"] unsignedLongValue])]) [fields addObject:f];
    if (!fields.count && !airSize) { nvlog("newArgumentEncoderWithBufferIndex:%lu — %s declares no argument buffer there", (unsigned long)index, _fname.UTF8String); return nil; }
    NVMTLArgumentEncoder *e = [NVMTLArgumentEncoder new];
    e->_fields = fields; e->_fieldByIndex = nvmtl_index_argument_fields(fields);
    e->_airSize = airSize; e->_airAlign = airAlign;
    return e;
}
- (NSArray<MTLVertexAttribute *> *)vertexAttributes { return (NSArray<MTLVertexAttribute *> *)nvmtl_vertex_attributes_for(self); }
- (NSArray<MTLAttribute *> *)stageInputAttributes { return (NSArray<MTLAttribute *> *)nvmtl_vertex_attributes_for(self); }
- (MTLPatchType)patchType { uint32_t pt = 0, n = 0; return nvmtl_spirv_tess_info(_spirv.bytes, _spirv.length, &pt, &n) ? (MTLPatchType)pt : MTLPatchTypeNone; }
- (NSInteger)patchControlPointCount {
    uint32_t pt = 0, n = 0; if (!nvmtl_spirv_tess_info(_spirv.bytes, _spirv.length, &pt, &n)) return -1;
    NSDictionary *tr = nvmtl_function_reflection(self)[@"tessellation"];
    NSNumber *k = [tr isKindOfClass:[NSDictionary class]] && [tr[@"control_point_count"] isKindOfClass:[NSNumber class]] ? tr[@"control_point_count"] : nil;
    return k.integerValue > 0 ? k.integerValue : (NSInteger)n;
}
- (id<MTLArgumentEncoder>)newArgumentEncoderWithBufferIndex:(NSUInteger)index reflection:(MTLAutoreleasedArgument *)reflection {
    if (reflection) *reflection = (MTLArgument *)nvmtl_argument_for_buffer(self, index);
    id<MTLArgumentEncoder> e = [self newArgumentEncoderWithBufferIndex:index];
    nvlog("newArgumentEncoderWithBufferIndex:%lu reflection: -> %p (%s)", (unsigned long)index, (__bridge void *)e, _fname.UTF8String);
    return e;
}
- (NSString *)name { return _specName.length ? _specName : _fname; }
- (MTLFunctionType)functionType {
    if ([_stage isEqualToString:@"visible"] && _air.length && [_air rangeOfString:@"!air.intersection = "].location != NSNotFound) {
        __block BOOL isect = NO; NSString *key = [NSString stringWithFormat:@"ptr @%@,", _fname];
        [_air enumerateLinesUsingBlock:^(NSString *l, BOOL *stop) {
            if ([l hasPrefix:@"!"] && [l rangeOfString:key].location != NSNotFound &&
                ([l rangeOfString:@"!\"air.bounding_box\""].location != NSNotFound || [l rangeOfString:@"!\"air.triangle\""].location != NSNotFound)) { isect = YES; *stop = YES; } }];
        if (isect) return MTLFunctionTypeIntersection;
    }
    if ([_stage isEqualToString:@"visible"]) return nvmtl_ci_entry(self) ? (MTLFunctionType)4 : MTLFunctionTypeVisible;
    if ([_stage isEqualToString:@"kernel"] || [_stage isEqualToString:@"compute"]) return MTLFunctionTypeKernel;
    if ([_stage isEqualToString:@"fragment"]) return MTLFunctionTypeFragment;
    if ([_stage isEqualToString:@"mesh"]) return MTLFunctionTypeMesh;
    if ([_stage isEqualToString:@"object"]) return MTLFunctionTypeObject;
    return MTLFunctionTypeVertex;
}
- (NSArray *)arguments { return (nvmtl_ci_entry(self) ?: nvmtl_ci_stitch_entry(self))[@"args"]; }
- (id)returnType { return (nvmtl_ci_entry(self) ?: nvmtl_ci_stitch_entry(self))[@"ret"]; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
@end
static MTLDataType nvmtl_datatype_for_air_name(NSString *t);
@implementation NVMTLType
- (MTLDataType)dataType { return _dataType; }
@end
@implementation NVMTLVertexAttribute
- (NSString *)name { return _name ?: @""; }
- (NSUInteger)attributeIndex { return _index; }
- (MTLDataType)attributeType { return _dataType; }
- (BOOL)isActive { return YES; }
- (BOOL)isPatchData { return NO; }
- (BOOL)isPatchControlPointData { return NO; }
@end
static NSArray *nvmtl_vertex_attributes_for(NVMTLFunction *fn) {
    NSDictionary *refl = nvmtl_function_reflection(fn);
    NSMutableArray *out = [NSMutableArray new];
    for (NSDictionary *a in refl[@"vertex_attributes"]) {
        if (![a isKindOfClass:[NSDictionary class]]) continue;
        NVMTLVertexAttribute *v = [NVMTLVertexAttribute new];
        v->_index = [a[@"location"] unsignedLongValue];
        v->_name = [a[@"name"] isKindOfClass:[NSString class]] ? a[@"name"] : [NSString stringWithFormat:@"attribute%lu", (unsigned long)v->_index];
        v->_dataType = [a[@"type_name"] isKindOfClass:[NSString class]] ? nvmtl_datatype_for_air_name(a[@"type_name"]) : MTLDataTypeFloat4;
        [out addObject:v];
    }
    return out;
}
@implementation NVMTLTextureReferenceType
- (MTLDataType)textureDataType { return _texDataType; }
- (MTLTextureType)textureType { return _texType; }
- (MTLBindingAccess)access { return _access; }
- (BOOL)isDepthTexture { return _depth; }
@end
@implementation NVMTLStructType
- (NSArray *)members { return _members ?: @[]; }
- (id)memberByName:(NSString *)n { for (NVMTLStructMember *m in _members) if ([m->_name isEqualToString:n]) return m; return nil; }
@end
@implementation NVMTLArrayType
- (MTLDataType)elementType { return _elem; }
- (NSUInteger)arrayLength { return _len; }
- (NSUInteger)stride { return _stride; }
- (NSUInteger)argumentIndexStride { return 0; }
- (id)elementStructType { return nil; }
- (id)elementArrayType { return nil; }
- (id)elementTextureReferenceType { return nil; }
- (id)elementPointerType { return nil; }
@end
@implementation NVMTLPointerType
- (MTLDataType)elementType { return _elem; }
- (MTLBindingAccess)access { return _access; }
- (NSUInteger)alignment { return _align ?: 16; }
- (NSUInteger)dataSize { return _size; }
- (BOOL)elementIsArgumentBuffer { return _isArgBuf; }
- (id)elementStructType { return _elemStruct; }
- (id)elementArrayType { return nil; }
@end
@implementation NVMTLStructMember
- (NSString *)name { return _name ?: @""; }
- (NSUInteger)offset { return _offset; }
- (MTLDataType)dataType { return _dataType; }
- (NSUInteger)argumentIndex { return _argIndex; }
- (id)structType { return _struct; }
- (id)arrayType { return _arr; }
- (id)textureReferenceType { return _texRef; }
- (id)pointerType { return _ptr; }
- (NSString *)description { return [NSString stringWithFormat:@"<member %@ id %lu off %lu type %lu>", _name, (unsigned long)_argIndex, (unsigned long)_offset, (unsigned long)_dataType]; }
@end
@implementation NVMTLArgument
- (NSString *)name { return _name ?: @""; }
- (MTLArgumentType)type { return _type; }
- (MTLBindingAccess)access { return _access; }
- (NSUInteger)index { return _index; }
- (BOOL)isActive { return _active; }
- (BOOL)isUsed { return _active; }
- (BOOL)isArgument { return YES; }
- (NSUInteger)arrayLength { return _arrayLength ?: 1; }
- (NSUInteger)bufferAlignment { return _bufAlign ?: 16; }
- (NSUInteger)bufferDataSize { return _bufSize; }
- (MTLDataType)bufferDataType { return _bufDataType; }
- (id)bufferStructType { return _bufStruct; }
- (id)bufferPointerType { return _bufPtr; }
- (MTLTextureType)textureType { return _texType; }
- (MTLDataType)textureDataType { return _texDataType; }
- (BOOL)isDepthTexture { return _depth; }
- (NSUInteger)threadgroupMemoryAlignment { return 16; }
- (NSUInteger)threadgroupMemoryDataSize { return 0; }
- (NSString *)description { return [NSString stringWithFormat:@"<arg %@ type %lu index %lu size %lu members %lu>", _name, (unsigned long)_type, (unsigned long)_index, (unsigned long)_bufSize, (unsigned long)_bufStruct->_members.count]; }
@end
// NVMTLArgument implements every getter of both binding protocols
@interface NVMTLBufferArgument : NVMTLArgument <MTLBufferBinding> @end
@implementation NVMTLBufferArgument
@dynamic name, type, access, index, used, argument;
@dynamic bufferAlignment, bufferDataSize, bufferDataType, bufferStructType, bufferPointerType;
@end
@interface NVMTLTextureArgument : NVMTLArgument <MTLTextureBinding> @end
@implementation NVMTLTextureArgument
@dynamic name, type, access, index, used, argument;
@dynamic textureType, textureDataType, depthTexture, arrayLength;
@end
@interface NVMTLStageInArgument : NVMTLBufferArgument @end
@implementation NVMTLStageInArgument
- (BOOL)isArgument { return NO; }
@end
@implementation NVMTLRenderPipelineReflection
- (NSArray *)vertexArguments { return _v ?: @[]; }
- (NSArray *)fragmentArguments { return _f ?: @[]; }
- (NSArray *)tileArguments { return @[]; }
- (NSArray *)vertexBindings { return _v ?: @[]; }
- (NSArray *)fragmentBindings { return _f ?: @[]; }
- (NSArray *)tileBindings { return @[]; }
- (NSArray *)objectBindings { return _o ?: @[]; }
- (NSArray *)meshBindings { return _m ?: @[]; }
@end
@implementation NVMTLComputePipelineReflection
- (NSArray *)arguments { return _a ?: @[]; }
- (NSArray *)bindings { return _a ?: @[]; }
@end

static MTLDataType nvmtl_datatype_for_air_name(NSString *t) {
    if (!t.length) return MTLDataTypeNone;
    if ([t hasPrefix:@"texture"] || [t hasPrefix:@"depth"]) return MTLDataTypeTexture;
    if ([t isEqualToString:@"sampler"]) return MTLDataTypeSampler;
    static NSDictionary *tab; static dispatch_once_t once;
    dispatch_once(&once, ^{ tab = @{
        @"float": @(MTLDataTypeFloat), @"float2": @(MTLDataTypeFloat2), @"float3": @(MTLDataTypeFloat3), @"float4": @(MTLDataTypeFloat4),
        @"float2x2": @(MTLDataTypeFloat2x2), @"float3x3": @(MTLDataTypeFloat3x3), @"float4x4": @(MTLDataTypeFloat4x4), @"float3x4": @(MTLDataTypeFloat3x4), @"float4x3": @(MTLDataTypeFloat4x3),
        @"half": @(MTLDataTypeHalf), @"half2": @(MTLDataTypeHalf2), @"half3": @(MTLDataTypeHalf3), @"half4": @(MTLDataTypeHalf4),
        @"int": @(MTLDataTypeInt), @"int2": @(MTLDataTypeInt2), @"int3": @(MTLDataTypeInt3), @"int4": @(MTLDataTypeInt4),
        @"uint": @(MTLDataTypeUInt), @"uint2": @(MTLDataTypeUInt2), @"uint3": @(MTLDataTypeUInt3), @"uint4": @(MTLDataTypeUInt4),
        @"short": @(MTLDataTypeShort), @"ushort": @(MTLDataTypeUShort), @"char": @(MTLDataTypeChar), @"uchar": @(MTLDataTypeUChar), @"bool": @(MTLDataTypeBool),
        @"long": @(MTLDataTypeLong), @"ulong": @(MTLDataTypeULong) }; });
    NSNumber *n = tab[t];
    if (n) return (MTLDataType)n.integerValue;
    if ([t hasSuffix:@"*"] || [t hasPrefix:@"device "] || [t hasPrefix:@"constant "]) return MTLDataTypePointer;
    return MTLDataTypeStruct;
}
static MTLBindingAccess nvmtl_access_for(id a) {
    NSString *s = [a isKindOfClass:[NSString class]] ? a : nil;
    if ([s isEqualToString:@"ReadWrite"] || [s isEqualToString:@"Storage"]) return MTLBindingAccessReadWrite;
    if ([s isEqualToString:@"WriteOnly"]) return MTLBindingAccessWriteOnly;
    return MTLBindingAccessReadOnly;
}
static MTLTextureType nvmtl_textype_for_shape(id shape) {
    NSString *s = [shape isKindOfClass:[NSString class]] ? shape : [shape description];
    if (!s.length) return MTLTextureType2D;
    if ([s rangeOfString:@"CubeArray"].location != NSNotFound) return MTLTextureTypeCubeArray;
    if ([s rangeOfString:@"Cube"].location != NSNotFound) return MTLTextureTypeCube;
    if ([s rangeOfString:@"3D"].location != NSNotFound || [s rangeOfString:@"3d"].location != NSNotFound || [s rangeOfString:@"Three"].location != NSNotFound) return MTLTextureType3D;
    if ([s rangeOfString:@"1D"].location != NSNotFound || [s rangeOfString:@"1d"].location != NSNotFound || [s rangeOfString:@"One"].location != NSNotFound)
        return [s rangeOfString:@"Array"].location != NSNotFound ? MTLTextureType1DArray : MTLTextureType1D;
    if ([s rangeOfString:@"Multisample"].location != NSNotFound || [s rangeOfString:@"MS"].location != NSNotFound) return MTLTextureType2DMultisample;
    return [s rangeOfString:@"Array"].location != NSNotFound ? MTLTextureType2DArray : MTLTextureType2D;
}
// texture2d<uint, sample> -> UInt, <int ...> -> Int, <half ...> -> Half, anything else (float, depth, unknown) -> Float
static MTLDataType nvmtl_texdatatype_for_air_name(NSString *tn) {
    if (![tn isKindOfClass:[NSString class]]) return MTLDataTypeFloat;
    if ([tn rangeOfString:@"<uint"].location != NSNotFound || [tn rangeOfString:@"<ushort"].location != NSNotFound) return MTLDataTypeUInt;
    if ([tn rangeOfString:@"<int"].location != NSNotFound || [tn rangeOfString:@"<short"].location != NSNotFound) return MTLDataTypeInt;
    if ([tn rangeOfString:@"<half"].location != NSNotFound) return MTLDataTypeHalf;
    return MTLDataTypeFloat;
}
static BOOL nvmtl_binding_active(NSDictionary *b) { id acc = b[@"access"]; return !([acc isKindOfClass:[NSString class]] && [acc isEqualToString:@"Unused"]); }
static NVMTLArgument *nvmtl_build_buffer_argument(NVMTLFunction *fn, NSUInteger index, NSDictionary *binding, NSDictionary *refl, NSDictionary *names) {
    NVMTLArgument *a = [NVMTLBufferArgument new];
    a->_type = MTLArgumentTypeBuffer; a->_index = index; a->_active = nvmtl_binding_active(binding); a->_arrayLength = 1; a->_bufAlign = 16;
    NSString *key = [NSString stringWithFormat:@"buffer:%lu", (unsigned long)index];
    a->_name = names[@"args"][key] ?: [NSString stringWithFormat:@"buffer%lu", (unsigned long)index];
    a->_access = nvmtl_access_for(binding[@"access"]);
    id ds = binding[@"declared_size"]; a->_bufSize = [ds isKindOfClass:[NSNumber class]] ? [ds unsignedLongValue] : 0;
    NSMutableArray *members = [NSMutableArray new];
    NSDictionary *memberNames = names[@"members"], *types = names[@"types"];
    NSString *bufKeyPrefix = [NSString stringWithFormat:@"%lu:", (unsigned long)index];
    NSMutableDictionary *kindAt = [NSMutableDictionary new];
    for (NSDictionary *b in refl[@"bindings"]) {
        NSDictionary *src = b[@"embedded_source"]; if (![src isKindOfClass:[NSDictionary class]]) continue;
        if ([src[@"buffer_index"] unsignedLongValue] != index) continue;
        kindAt[src[@"field_offset"]] = [b[@"kind"] isEqualToString:@"EmbeddedArgBufferSampler"] ? @"sampler" : @"texture";
    }
    NSUInteger end = 0;
    for (NSDictionary *f0 in refl[@"argument_buffer_fields"]) {
        if ([f0[@"buffer_index"] unsignedLongValue] != index) continue;
        NVMTLStructMember *m = [NVMTLStructMember new];
        m->_offset = [f0[@"field_offset"] unsignedLongValue]; m->_argIndex = [f0[@"argument_index"] unsignedLongValue];
        NSString *mk = [bufKeyPrefix stringByAppendingFormat:@"%lu", (unsigned long)m->_argIndex];
        m->_name = memberNames[mk] ?: [NSString stringWithFormat:@"field%lu", (unsigned long)m->_argIndex];
        NSString *tn = types[mk]; NSString *kind = kindAt[f0[@"field_offset"]];
        if ([kind isEqualToString:@"sampler"]) m->_dataType = MTLDataTypeSampler;
        else if ([kind isEqualToString:@"texture"]) m->_dataType = MTLDataTypeTexture;
        else if ([f0[@"resource_buffer_index"] isKindOfClass:[NSNumber class]]) m->_dataType = MTLDataTypePointer;
        else m->_dataType = tn ? nvmtl_datatype_for_air_name(tn) : MTLDataTypePointer;
        if (m->_dataType == MTLDataTypeTexture) { m->_texRef = [NVMTLTextureReferenceType new]; m->_texRef->_dataType = MTLDataTypeTexture; m->_texRef->_texDataType = MTLDataTypeFloat;
            m->_texRef->_texType = [tn rangeOfString:@"cube"].location != NSNotFound ? MTLTextureTypeCube : [tn rangeOfString:@"3d"].location != NSNotFound ? MTLTextureType3D
                                 : [tn rangeOfString:@"2d_array"].location != NSNotFound ? MTLTextureType2DArray : MTLTextureType2D;
            m->_texRef->_depth = [tn hasPrefix:@"depth"]; }
        if (m->_dataType == MTLDataTypePointer) { m->_ptr = [NVMTLPointerType new]; m->_ptr->_dataType = MTLDataTypePointer; m->_ptr->_elem = MTLDataTypeStruct; m->_ptr->_align = 16; }
        NSUInteger e = m->_offset + (m->_dataType == MTLDataTypeFloat4x4 ? 64 : m->_dataType == MTLDataTypeFloat3x3 ? 48 : 8);
        if (e > end) end = e;
        [members addObject:m];
    }
    NSArray *airPlain = [names[@"plain"] isKindOfClass:[NSDictionary class]] ? names[@"plain"][@(index)] : nil;
    if (!members.count && [airPlain isKindOfClass:[NSArray class]] && airPlain.count) {
        NSUInteger i = 0;
        for (NSDictionary *am in airPlain) {
            NVMTLStructMember *m = [NVMTLStructMember new];
            m->_offset = [am[@"off"] unsignedLongValue]; m->_argIndex = i++; m->_name = am[@"name"];
            m->_dataType = nvmtl_datatype_for_air_name(am[@"type"]);
            if (m->_dataType == MTLDataTypeNone) m->_dataType = MTLDataTypeStruct;
            // struct_type_info gives the element size and an element count (0 = not an array). An array is reported as
            // Apple does, MTLDataTypeArray with its MTLArrayType: engines size their uniform copies from it. WAS: the element
            // alone, so Hades (The Forge) copied one float4x4 of its float4x4[2] and every screen-space sprite read zeros.
            NSUInteger cnt = [am[@"count"] unsignedLongValue], sz = [am[@"size"] unsignedLongValue];
            if (cnt > 1) { NVMTLArrayType *at = [NVMTLArrayType new]; at->_dataType = MTLDataTypeArray; at->_elem = m->_dataType; at->_len = cnt; at->_stride = sz;
                m->_arr = at; m->_dataType = MTLDataTypeArray; }
            NSUInteger e = m->_offset + sz * (cnt > 1 ? cnt : 1); if (e > end) end = e;
            [members addObject:m];
        }
    }
    if (!members.count && [binding[@"type_layout"] isKindOfClass:[NSDictionary class]]) {
        NSArray *ms = binding[@"type_layout"][@"Struct"];
        NSUInteger i = 0;
        for (NSDictionary *am in ms) {
            if (![am isKindOfClass:[NSDictionary class]]) continue;
            NVMTLStructMember *m = [NVMTLStructMember new];
            m->_offset = [am[@"offset"] unsignedLongValue]; m->_argIndex = i;
            NSString *mk = [bufKeyPrefix stringByAppendingFormat:@"%lu", (unsigned long)i];
            m->_name = memberNames[mk] ?: [NSString stringWithFormat:@"m%lu", (unsigned long)i];
            NSString *tn = types[mk]; m->_dataType = tn ? nvmtl_datatype_for_air_name(tn) : MTLDataTypeFloat;
            [members addObject:m]; i++;
        }
    }
    if (members.count) {
        [members sortUsingComparator:^NSComparisonResult(NVMTLStructMember *x, NVMTLStructMember *y) { return x->_offset < y->_offset ? NSOrderedAscending : x->_offset > y->_offset ? NSOrderedDescending : NSOrderedSame; }];
        a->_bufDataType = MTLDataTypeStruct;
        a->_bufStruct = [NVMTLStructType new]; a->_bufStruct->_dataType = MTLDataTypeStruct; a->_bufStruct->_members = members;
        if (!a->_bufSize) a->_bufSize = end;
    } else {
        NSString *tn = names[@"types"][key];
        a->_bufDataType = tn ? nvmtl_datatype_for_air_name(tn) : MTLDataTypeStruct;
        if (a->_bufDataType == MTLDataTypeTexture || a->_bufDataType == MTLDataTypeSampler) a->_bufDataType = MTLDataTypeStruct;
    }
    a->_bufPtr = [NVMTLPointerType new]; a->_bufPtr->_dataType = MTLDataTypePointer; a->_bufPtr->_elem = a->_bufDataType; a->_bufPtr->_access = a->_access;
    a->_bufPtr->_align = 16; a->_bufPtr->_size = a->_bufSize; a->_bufPtr->_isArgBuf = kindAt.count > 0 || [refl[@"argument_buffer_fields"] count] > 0; a->_bufPtr->_elemStruct = a->_bufStruct;
    return a;
}
NSArray<NVMTLArgument *> *nvmtl_arguments_for_function(NVMTLFunction *fn)
{
    if (!fn) return @[];
    @synchronized (fn) {
        if (fn->_arguments) return fn->_arguments;
        NSDictionary *refl = nvmtl_function_reflection(fn);
        NSDictionary *names = nvmtl_air_names(fn) ?: @{ @"args": @{}, @"members": @{}, @"types": @{} };
        NSMutableArray *out = [NSMutableArray new];
        NSMutableSet *seenBuf = [NSMutableSet new];
        for (NSDictionary *b in refl[@"bindings"]) {
            if (![b isKindOfClass:[NSDictionary class]]) continue;
            if (![b[@"param_index"] isKindOfClass:[NSNumber class]]) continue;
            NSString *kind = b[@"kind"]; NSUInteger idx = [b[@"metal_index"] unsignedLongValue];
            if ([kind isEqualToString:@"Buffer"]) {
                if ([seenBuf containsObject:@(idx)]) continue;
                [seenBuf addObject:@(idx)];
                [out addObject:nvmtl_build_buffer_argument(fn, idx, b, refl, names)];
            } else if ([kind isEqualToString:@"Texture"] || [kind isEqualToString:@"TextureArray"] || [kind isEqualToString:@"StorageImage"]) {
                NVMTLArgument *a = [NVMTLTextureArgument new]; a->_type = MTLArgumentTypeTexture; a->_index = idx; a->_active = nvmtl_binding_active(b);
                a->_name = names[@"args"][[NSString stringWithFormat:@"texture:%lu", (unsigned long)idx]] ?: [NSString stringWithFormat:@"texture%lu", (unsigned long)idx];
                a->_access = [kind isEqualToString:@"StorageImage"] ? MTLBindingAccessReadWrite : nvmtl_access_for(b[@"access"]);
                a->_texType = nvmtl_textype_for_shape(b[@"texture_shape"]);
                // The AIR type name says what MTLArgument does: depth2d/depthcube/... is a depth texture, <uint>/<int>/<half>
                // the component type. Apple's GL picks a texture unit's sampler type from these two: with isDepthTexture NO
                // a sampler2DShadow unit counts as Float and its shader-side compare swizzle (texInfo) stays all Zero, so
                // every shadow lookup returned 0 (Unity's screen-space shadows, Northgard's lit ground: black).
                NSString *ttn = names[@"types"][[NSString stringWithFormat:@"texture:%lu", (unsigned long)idx]];
                a->_depth = [ttn isKindOfClass:[NSString class]] && [ttn rangeOfString:@"depth"].location != NSNotFound;
                a->_texDataType = nvmtl_texdatatype_for_air_name(ttn);
                id loc = b[@"descriptor"]; a->_arrayLength = [loc isKindOfClass:[NSDictionary class]] && [loc[@"count"] unsignedLongValue] > 1 ? [loc[@"count"] unsignedLongValue] : 1;
                [out addObject:a];
            } else if ([kind isEqualToString:@"Sampler"]) {
                NVMTLArgument *a = [NVMTLArgument new]; a->_type = MTLArgumentTypeSampler; a->_index = idx; a->_active = nvmtl_binding_active(b); a->_arrayLength = 1;
                a->_name = names[@"args"][[NSString stringWithFormat:@"sampler:%lu", (unsigned long)idx]] ?: [NSString stringWithFormat:@"sampler%lu", (unsigned long)idx];
                [out addObject:a];
            }
        }
        [out sortUsingComparator:^NSComparisonResult(NVMTLArgument *x, NVMTLArgument *y) {
            if (x->_type != y->_type) return x->_type < y->_type ? NSOrderedAscending : NSOrderedDescending;
            return x->_index < y->_index ? NSOrderedAscending : x->_index > y->_index ? NSOrderedDescending : NSOrderedSame; }];
        NSMutableString *s = [NSMutableString new];
        for (NVMTLArgument *a in out) [s appendFormat:@" %@(%lu)=%@%@", a->_type == MTLArgumentTypeBuffer ? @"buffer" : a->_type == MTLArgumentTypeTexture ? @"texture" : @"sampler",
                                       (unsigned long)a->_index, a->_name, a->_bufStruct ? [NSString stringWithFormat:@"[%lu members, %lu B]", (unsigned long)a->_bufStruct->_members.count, (unsigned long)a->_bufSize] : @""];
        nvlog("REFL %s (%s): %lu argument(s)%s", fn->_fname.UTF8String ?: "?", fn->_stage.UTF8String ?: "?", (unsigned long)out.count, s.UTF8String);
        fn->_arguments = out;
        return out;
    }
}
NVMTLArgument *nvmtl_argument_for_buffer(NVMTLFunction *fn, NSUInteger index)
{
    for (NVMTLArgument *a in nvmtl_arguments_for_function(fn)) if (a->_type == MTLArgumentTypeBuffer && a->_index == index) return a;
    return nil;
}
static NSArray *nvmtl_stage_in_arguments(MTLRenderPipelineDescriptor *d)
{
    MTLVertexDescriptor *vd = d.vertexDescriptor; if (!vd || !d.vertexFunction) return @[];
    NSMutableSet *used = [NSMutableSet set];
    for (NSDictionary *va in nvmtl_function_reflection((NVMTLFunction *)d.vertexFunction)[@"vertex_attributes"])
        if ([va isKindOfClass:[NSDictionary class]] && [va[@"location"] isKindOfClass:[NSNumber class]]) [used addObject:va[@"location"]];
    NSMutableIndexSet *bufs = [NSMutableIndexSet indexSet], *live = [NSMutableIndexSet indexSet];
    for (NSUInteger i = 0; i < 31; i++) {
        MTLVertexAttributeDescriptor *at = vd.attributes[i]; if (at.format == MTLVertexFormatInvalid) continue;
        [bufs addIndex:at.bufferIndex]; if ([used containsObject:@(i)]) [live addIndex:at.bufferIndex];
    }
    NSMutableArray *out = [NSMutableArray array]; __block NSUInteger n = 0;
    [bufs enumerateIndexesUsingBlock:^(NSUInteger bi, BOOL *stop) {
        NVMTLStageInArgument *a = [NVMTLStageInArgument new];
        a->_type = MTLArgumentTypeBuffer; a->_index = bi; a->_active = [live containsIndex:bi]; a->_arrayLength = 1; a->_bufAlign = 4;
        a->_access = MTLBindingAccessReadOnly; a->_bufSize = vd.layouts[bi].stride; a->_bufDataType = MTLDataTypeStruct;
        a->_name = [NSString stringWithFormat:@"vertexBuffer.%lu", (unsigned long)n++];
        [out addObject:a];
    }];
    return out;
}
id nvmtl_render_reflection(MTLRenderPipelineDescriptor *d)
{
    NVMTLRenderPipelineReflection *r = [NVMTLRenderPipelineReflection new];
    r->_v = [nvmtl_stage_in_arguments(d) arrayByAddingObjectsFromArray:nvmtl_arguments_for_function((NVMTLFunction *)d.vertexFunction)];
    r->_f = nvmtl_arguments_for_function((NVMTLFunction *)d.fragmentFunction);
    return r;
}
void nvmtl_gl_tag_compute_reflection(id r, id fn);
id nvmtl_compute_reflection(id<MTLFunction> fn)
{
    NVMTLComputePipelineReflection *r = [NVMTLComputePipelineReflection new];
    r->_a = nvmtl_arguments_for_function((NVMTLFunction *)fn);
    nvmtl_gl_tag_compute_reflection(r, fn);
    return r;
}

NSDictionary *nvmtl_index_argument_fields(NSArray *fields) {
    NSMutableDictionary *index = [NSMutableDictionary new];
    for (NSDictionary *field in fields) {
        NSNumber *key = @([field[@"argument_index"] unsignedLongValue]);
        if (!index[key]) index[key] = field;
    }
    return [index copy];
}
@implementation NVMTLArgumentEncoder
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSUInteger)encodedLength {
    NSUInteger end = 0;
    for (NSDictionary *f in _fields) {
        NSUInteger e = [f[@"field_offset"] unsignedLongValue] + 8;
        if (e > end) end = e;
    }
    return MAX(end, _airSize) ?: 8;
}
- (NSUInteger)alignment { return _airAlign ?: 8; }
- (void)setArgumentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)off { _dst = (NVMTLBuffer *)b; _dstOffset = off; nvtrace("AE argbuf %p off %lu (%lu fields)", (__bridge void *)b, (unsigned long)off, (unsigned long)_fields.count); }
// The encoder of the argument buffer that the pointer member [[id(index)]] points at. Metal answers nil for a member that is not
// such a pointer (measured on an Apple GPU: tools/argbuf_oracle/nested_oracle.m, outer 16/8, nested 16/8, member 1 nil).
- (id<MTLArgumentEncoder>)newArgumentEncoderForBufferAtIndex:(NSUInteger)index {
    NSDictionary *f = [self nvmtlFieldAt:index];
    NSArray *nested = f[@"nested_fields"];
    if (![nested isKindOfClass:[NSArray class]]) { nvlog("newArgumentEncoderForBufferAtIndex:%lu - member %lu is not a pointer to an argument buffer - nil", (unsigned long)index, (unsigned long)index); return nil; }
    NVMTLArgumentEncoder *e = [NVMTLArgumentEncoder new];
    e->_fields = nested; e->_fieldByIndex = nvmtl_index_argument_fields(nested);
    e->_airSize = [f[@"nested_size"] unsignedLongValue]; e->_airAlign = [f[@"nested_align"] unsignedLongValue];
    nvlog("newArgumentEncoderForBufferAtIndex:%lu -> %lu field(s), %lu bytes", (unsigned long)index, (unsigned long)nested.count, (unsigned long)e->_airSize);
    return e;
}
- (void)setArgumentBuffer:(id<MTLBuffer>)b startOffset:(NSUInteger)off arrayElement:(NSUInteger)i {
    [self setArgumentBuffer:b offset:off + i * [self encodedLength]];
}
- (void)dealloc { free(_offAt); _offAt = NULL; }
- (NSDictionary *)nvmtlFieldAt:(NSUInteger)index {
    return _fieldByIndex[@(index)];
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)index {
    NVMTLBuffer *nb = (NVMTLBuffer *)b;
    if (!_dst || !nvmtl_buf_cpu(_dst)) { nvlog("argument encoder: no argument buffer set (or it is not host-visible)"); return; }
    NSDictionary *f = [self nvmtlFieldAt:index];
    if (!f) { nvlog("argument encoder: this function has no [[id(%lu)]] field — REFUSED", (unsigned long)index); return; }
    // The binding path checks the offset against the buffer length (nvmtl_vk_bind_buffer); this path
    // computes an address the shader will dereference, so it has to check the same thing.
    if (nb && off > nb->_b.size) {
        nvlog("argument encoder: offset %lu is past the %zu-byte buffer at [[id(%lu)]] - REFUSED",
              (unsigned long)off, nb->_b.size, (unsigned long)index);
        return;
    }
    uint64_t addr = nb ? nvmtl_vk_buffer_address(&nb->_b) + off : 0;
    const NSUInteger fieldOffset = [f[@"field_offset"] unsignedLongValue];
    const size_t size = _dst->_b.size;
    if (_dstOffset > size || fieldOffset > size - _dstOffset || 8 > size - _dstOffset - fieldOffset) {
        nvlog("argument encoder: field offset %lu+%lu does not fit %zu bytes — REFUSED", (unsigned long)_dstOffset, (unsigned long)fieldOffset, size); return;
    }
    NSUInteger at = _dstOffset + fieldOffset;
    if (at + 8 > _dst->_b.size) { nvlog("argument encoder: field at %lu is past the argument buffer (%zu bytes)", (unsigned long)at, _dst->_b.size); return; }
    memcpy((uint8_t *)nvmtl_buf_cpu(_dst) + at, &addr, 8);
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)as atIndex:(NSUInteger)index { [self setBuffer:(id<MTLBuffer>)as offset:0 atIndex:index]; }
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])bs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:bs[k] offset:offs ? offs[k] : 0 atIndex:r.location + k];
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)index {
    if (!_dst || !nvmtl_buf_cpu(_dst)) { nvlog("argument encoder: setTexture: no argument buffer set (or it is not host-visible)"); return; }
    NSDictionary *f = [self nvmtlFieldAt:index];
    if (!f) { nvlog("argument encoder: setTexture:atIndex:%lu — this function has no [[id(%lu)]] field — REFUSED", (unsigned long)index, (unsigned long)index); return; }
    uint64_t handle = t ? [t gpuResourceID]._impl : 0;
    const NSUInteger fieldOffset = [f[@"field_offset"] unsignedLongValue];
    const size_t size = _dst->_b.size;
    if (_dstOffset > size || fieldOffset > size - _dstOffset || 8 > size - _dstOffset - fieldOffset) {
        nvlog("argument encoder: field offset %lu+%lu does not fit %zu bytes — REFUSED", (unsigned long)_dstOffset, (unsigned long)fieldOffset, size); return;
    }
    NSUInteger at = _dstOffset + fieldOffset;
    if (at + 8 > _dst->_b.size) { nvlog("argument encoder: texture field at %lu is past the argument buffer (%zu bytes)", (unsigned long)at, _dst->_b.size); return; }
    memcpy((uint8_t *)nvmtl_buf_cpu(_dst) + at, &handle, 8);
    nvtrace("AE tex id %lu -> %p+%lu handle 0x%llx", (unsigned long)index, (__bridge void *)_dst, (unsigned long)at, (unsigned long long)handle);
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setTexture:t ? t[k] : nil atIndex:r.location + k]; }
- (void)setSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)index {
    if (!_dst || !nvmtl_buf_cpu(_dst)) { nvlog("argument encoder: setSamplerState: no argument buffer set (or it is not host-visible)"); return; }
    NSDictionary *f = [self nvmtlFieldAt:index];
    if (!f) { nvlog("argument encoder: setSamplerState:atIndex:%lu — this function has no [[id(%lu)]] field — REFUSED", (unsigned long)index, (unsigned long)index); return; }
    uint64_t handle = s ? [s gpuResourceID]._impl : 0;
    const NSUInteger fieldOffset = [f[@"field_offset"] unsignedLongValue];
    const size_t size = _dst->_b.size;
    if (_dstOffset > size || fieldOffset > size - _dstOffset || 8 > size - _dstOffset - fieldOffset) {
        nvlog("argument encoder: field offset %lu+%lu does not fit %zu bytes — REFUSED", (unsigned long)_dstOffset, (unsigned long)fieldOffset, size); return;
    }
    NSUInteger at = _dstOffset + fieldOffset;
    if (at + 8 > _dst->_b.size) { nvlog("argument encoder: sampler field at %lu is past the argument buffer (%zu bytes)", (unsigned long)at, _dst->_b.size); return; }
    memcpy((uint8_t *)nvmtl_buf_cpu(_dst) + at, &handle, 8);
    nvtrace("AE samp id %lu -> %p+%lu handle 0x%llx", (unsigned long)index, (__bridge void *)_dst, (unsigned long)at, (unsigned long long)handle);
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:s ? s[k] : nil atIndex:r.location + k]; }
- (void)nvmtlPutHandle:(uint64_t)h atIndex:(NSUInteger)index what:(const char *)what {
    if (!_dst || !nvmtl_buf_cpu(_dst)) { nvlog("argument encoder: set%s: no argument buffer set (or it is not host-visible)", what); return; }
    NSDictionary *f = [self nvmtlFieldAt:index];
    if (!f) { nvlog("argument encoder: set%s:atIndex:%lu — this function has no [[id(%lu)]] field — REFUSED", what, (unsigned long)index, (unsigned long)index); return; }
    const size_t size = _dst->_b.size; const NSUInteger fo = [f[@"field_offset"] unsignedLongValue];
    if (_dstOffset > size || fo > size - _dstOffset || 8 > size - _dstOffset - fo) {
        nvlog("argument encoder: %s field at %lu+%lu is past the argument buffer (%zu bytes)", what, (unsigned long)_dstOffset, (unsigned long)fo, size); return; }
    memcpy((uint8_t *)nvmtl_buf_cpu(_dst) + _dstOffset + fo, &h, 8);
    nvtrace("AE %s id %lu -> %p+%lu handle 0x%llx", what, (unsigned long)index, (__bridge void *)_dst, (unsigned long)(_dstOffset + fo), (unsigned long long)h);
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p atIndex:(NSUInteger)i { [self nvmtlPutHandle:p ? p.gpuResourceID._impl : 0 atIndex:i what:"RenderPipelineState"]; }
- (void)setRenderPipelineStates:(const id<MTLRenderPipelineState> __unsafe_unretained [])p withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setRenderPipelineState:p ? p[k] : nil atIndex:r.location + k]; }
- (void)setComputePipelineState:(id<MTLComputePipelineState>)p atIndex:(NSUInteger)i { [self nvmtlPutHandle:p ? p.gpuResourceID._impl : 0 atIndex:i what:"ComputePipelineState"]; }
- (void)setComputePipelineStates:(const id<MTLComputePipelineState> __unsafe_unretained [])p withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setComputePipelineState:p ? p[k] : nil atIndex:r.location + k]; }
- (void)setIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)icb atIndex:(NSUInteger)index {
    { static int said; if (icb && !said++) nvlog("argument encoder: setIndirectCommandBuffer: writes the ICB's slot record address; shaders "
                                             "may encode draw/drawIndexed/reset into it, other render_command calls are refused at translation (said once)"); }
    [self nvmtlPutHandle:icb ? icb.gpuResourceID._impl : 0 atIndex:index what:"IndirectCommandBuffer"];
}
- (void)setIndirectCommandBuffers:(const id<MTLIndirectCommandBuffer> __unsafe_unretained [])b withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setIndirectCommandBuffer:b ? b[k] : nil atIndex:r.location + k]; }
- (void *)constantDataAtIndex:(NSUInteger)index {
    if (!_offAt) {
        NSUInteger mx = 0; for (NSDictionary *fd in _fields) { NSUInteger a = [fd[@"argument_index"] unsignedLongValue]; if (a > mx) mx = a; }
        int64_t *o = (int64_t *)malloc((mx + 1) * sizeof *o);
        if (o) { for (NSUInteger k = 0; k <= mx; k++) o[k] = -1;
                 for (NSDictionary *fd in _fields) { NSUInteger a = [fd[@"argument_index"] unsignedLongValue]; if (o[a] < 0) o[a] = (int64_t)[fd[@"field_offset"] unsignedLongValue]; }
                 _offN = mx + 1; _offAt = o; }
    }
    if (!_offAt || index >= _offN || _offAt[index] < 0 || !_dst || !nvmtl_buf_cpu(_dst)) return NULL;
    const NSUInteger fieldOffset = (NSUInteger)_offAt[index];
    const size_t size = _dst->_b.size;
    if (_dstOffset >= size || fieldOffset >= size - _dstOffset) return NULL;
    return (uint8_t *)nvmtl_buf_cpu(_dst) + _dstOffset + fieldOffset;
}
@end

extern NSData *nvmtl_compile_library_entry(NSData *, NSString *, NSString *, NSString *, NSString **);
@implementation NVMTLLibrary
- (MTLLibraryType)type { return MTLLibraryTypeExecutable; }
- (NSString *)installName { return nil; }
- (NSData *)nvmtlEntrySpirv:(NSString *)name error:(NSError **)error {
    @synchronized(self) {
        NSData *cached = _compiledFns[name];
        if (cached) return cached;
    }

    NSData *air = _fns[name];
    NSString *ll = _airs[name], *stage = _stages[name];
    NSString *why = nil;
    NSData *spv = nvmtl_compile_library_entry(air, ll, stage, name, &why);
    if (!spv) {
        if (error) *error = [NSError errorWithDomain:@"NVMTL" code:3
                                           userInfo:@{NSLocalizedDescriptionKey:why ?: @"entry translation failed"}];
        return nil;
    }

    @synchronized(self) {
        NSData *won = _compiledFns[name];
        if (won) return won;
        if (!_compiledFns) _compiledFns = [NSMutableDictionary new];
        _compiledFns[name] = spv;
    }
    return spv;
}
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
// NVMTL_AIR_DUMP=<dir>: write each function's AIR text, once, as <dir>/<name>.ll when an app first asks for it, so an
// app that builds its libraries from memory can be studied offline kernel by kernel. Nothing is written when unset.
static void nvmtl_air_dump(NSString *name, NSString *air)
{
    static const char *dir; static dispatch_once_t once;
    dispatch_once(&once, ^{ dir = getenv("NVMTL_AIR_DUMP"); if (dir && !*dir) dir = NULL; });
    if (!dir || !air.length || !name.length || [name containsString:@"/"]) return;
    NSString *path = [[@(dir) stringByAppendingPathComponent:name] stringByAppendingPathExtension:@"ll"];
    if ([[NSFileManager defaultManager] fileExistsAtPath:path]) return;
    [[NSFileManager defaultManager] createDirectoryAtPath:@(dir) withIntermediateDirectories:YES attributes:nil error:nil];
    [air writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil];
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name {
    NSData *spv = [_fns objectForKey:name];
    if (!spv && !_airs[name]) { nvlog("library: no function \"%s\" (this library holds %lu, externs %lu)", name.UTF8String, (unsigned long)(_fns.count + _airs.count), (unsigned long)_externs.count); return nil; }
    nvmtl_air_dump(name, _airs[name]);
    if (spv && ([_stages[name] isEqualToString:@"mesh"] || [_stages[name] isEqualToString:@"object"])) {
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fname = name; f->_stage = _stages[name];
        f->_air = _airs[name]; f->_lib = self; return f;
    }
    if (spv && [_airs[name] rangeOfString:@"@air.dyld_lib_table"].location != NSNotFound) {
        nvlog("library: \"%s\" was compiled against a dynamic library - DEFERRED to pipeline creation", name.UTF8String);
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fname = name; f->_stage = (_stages[name] ?: @"");
        f->_air = _airs[name]; f->_lib = self; f->_needsLink = YES; return f;
    }
    if (spv && [_airs[name] rangeOfString:@"!\"air.intersection_function_table\""].location != NSNotFound) {
        nvlog("library: \"%s\" takes an intersection function table - DEFERRED to pipeline creation", name.UTF8String);
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fname = name; f->_stage = (_stages[name] ?: @"");
        f->_air = _airs[name]; f->_lib = self; f->_needsLink = YES; return f;
    }
    if (spv) { NSError *te = nil; NSData *es = [self nvmtlEntrySpirv:name error:&te];
        if (!es && _airs[name] && nvmtl_needs_visible_link(_airs[name], te.localizedDescription)) {
            nvlog("library: \"%s\" calls a visible function (by reference or table) - DEFERRED to pipeline creation", name.UTF8String);
            NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fname = name; f->_stage = (_stages[name] ?: @"");
            f->_air = _airs[name]; f->_lib = self; f->_needsLink = YES; return f;
        }
        if (!es && _airs[name] && ([te.localizedDescription rangeOfString:@"`air.constant`"].location != NSNotFound || [name hasPrefix:@"__entry_"])) {
            nvlog("library: \"%s\" has a by-value (air.constant) parameter - DEFERRED to newFunctionWithPluginData (OpenCL via AMOGL)", name.UTF8String);
            NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fname = name; f->_stage = (_stages[name] ?: @"kernel");
            f->_air = _airs[name]; f->_lib = self; return f;
        }
        spv = es; if (!spv) return nil; }
    NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = spv; f->_fname = name; f->_stage = (_stages[name] ?: @""); f->_air = _airs[name]; f->_lib = self; return f;
}
static size_t nvmtl_fc_width(NSUInteger dataType)
{
    const struct { NSUInteger base; size_t bytes; } families[] = {
        {MTLDataTypeFloat,4},{MTLDataTypeHalf,2},{MTLDataTypeInt,4},{MTLDataTypeUInt,4},
        {MTLDataTypeShort,2},{MTLDataTypeUShort,2},{MTLDataTypeChar,1},{MTLDataTypeUChar,1},
        {MTLDataTypeBool,1},{MTLDataTypeLong,8},{MTLDataTypeULong,8}};
    for(size_t i=0;i<sizeof(families)/sizeof(families[0]);i++)
        if(dataType>=families[i].base && dataType<=families[i].base+3)
            return families[i].bytes*(dataType-families[i].base+1);
    switch (dataType) {
        case MTLDataTypeBool:  case MTLDataTypeChar:  case MTLDataTypeUChar:  return 1;
        case MTLDataTypeShort: case MTLDataTypeUShort: case MTLDataTypeHalf:  return 2;
        case MTLDataTypeInt:   case MTLDataTypeUInt:  case MTLDataTypeFloat:  return 4;
        case MTLDataTypeLong:  case MTLDataTypeULong:                          return 8;
        default: return 0;
    }
}
- (id<MTLFunction>)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)cv error:(NSError **)error {
    if (error) *error = nil;
    NSData *spv = [_fns objectForKey:name];
    nvmtl_air_dump(name, _airs[name]);
    if (!spv && _airs[name]) {
        if (cv) nvlog("library: visible function \"%s\" asked with constant values — carried on the function",
                      name.UTF8String);
        id<MTLFunction> vf = [self newFunctionWithName:name];
        if (cv && [(id)vf isKindOfClass:[NVMTLFunction class]]) ((NVMTLFunction *)vf)->_fcv = cv;
        return vf;
    }
    if (!spv) { nvlog("library: no function \"%s\" (this library holds %lu, externs %lu)", name.UTF8String,
                      (unsigned long)(_fns.count + _airs.count), (unsigned long)_externs.count);
        if (error) *error = [NSError errorWithDomain:@"NVMTL" code:1 userInfo:@{NSLocalizedDescriptionKey: @"no such function"}];
        return nil; }
    NSArray *vals = nil;
    SEL sel = NSSelectorFromString(@"newIndexedConstantArray");
    if (cv && [cv respondsToSelector:sel]) vals = ((id (*)(id, SEL))objc_msgSend)(cv, sel);
    SEL namedSel=NSSelectorFromString(@"newNamedConstantArray");
    NSArray *named=(cv && [cv respondsToSelector:namedSel]) ? ((id(*)(id,SEL))objc_msgSend)(cv,namedSel) : nil;
    if(named.count) {
        NSString *air=_airs[name];
        if(!air){if(error)*error=[NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey:@"named constants require retained AIR"}];return nil;}
        NSRegularExpression *re=[NSRegularExpression regularExpressionWithPattern:@"^![0-9]+ = !\\{[^\\n]*\\.MTL_FC_INIT_[^\\n]*, !\"[^\"]+\", !\"([^\"]+)\", i32 ([0-9]+), i1 (?:true|false)\\}$" options:NSRegularExpressionAnchorsMatchLines error:NULL];
        NSMutableDictionary *indices=[NSMutableDictionary new];
        for(NSTextCheckingResult *m in [re matchesInString:air options:0 range:NSMakeRange(0,air.length)])
            indices[[air substringWithRange:[m rangeAtIndex:1]]]=@([[air substringWithRange:[m rangeAtIndex:2]] longLongValue]);
        MTLFunctionConstantValues *merged=[MTLFunctionConstantValues new];
        NSUInteger applied=0;
        for(id v in named) {
            Ivar ni=class_getInstanceVariable([v class],"_name"),ti=class_getInstanceVariable([v class],"_dataType"),di=class_getInstanceVariable([v class],"_data");
            if(!ni || !ti || !di){if(error)*error=[NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey:@"unrecognized named-constant representation"}];return nil;}
            NSString *key=object_getIvar(v,ni);NSNumber *index=indices[key];
            if(!index)continue;
            const uint8_t *base=(const uint8_t *)(__bridge void *)v;
            NSUInteger type=*(const NSUInteger *)(base+ivar_getOffset(ti));
            const void *data=*(const void *const *)(base+ivar_getOffset(di));
            if(!data || !nvmtl_fc_width(type)){if(error)*error=[NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey:@"unsupported named-constant type"}];return nil;}
            [merged setConstantValue:data type:type atIndex:index.unsignedIntegerValue];applied++;
        }
        for(id v in vals) {
            Ivar ii=class_getInstanceVariable([v class],"_index"),ti=class_getInstanceVariable([v class],"_dataType"),di=class_getInstanceVariable([v class],"_data");
            if(!ii || !ti || !di){if(error)*error=[NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey:@"unrecognized indexed-constant representation"}];return nil;}
            const uint8_t *base=(const uint8_t *)(__bridge void *)v;
            [merged setConstantValue:*(const void *const *)(base+ivar_getOffset(di)) type:*(const NSUInteger *)(base+ivar_getOffset(ti)) atIndex:*(const NSUInteger *)(base+ivar_getOffset(ii))];
        }
        vals=((id(*)(id,SEL))objc_msgSend)(merged,sel);
        nvlog("named constants: %s applied %lu of %lu, merged %lu",name.UTF8String,(unsigned long)applied,(unsigned long)named.count,(unsigned long)vals.count);
    }
    if (vals.count == 0 && ([_stages[name] isEqualToString:@"mesh"] || [_stages[name] isEqualToString:@"object"]) && _airs[name]) {
        nvlog("newFunctionWithName:constantValues: \"%s\" is a %s function - %zu constant(s) DEFERRED to the mesh pipeline",
              name.UTF8String, [_stages[name] UTF8String], (size_t)0);
        if (error) *error = nil;
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fc = nil; f->_fname = name; f->_stage = _stages[name];
        f->_air = _airs[name]; f->_lib = self; f->_fcv = cv; return f;
    }
    if (vals.count == 0) {
        spv = [self nvmtlEntrySpirv:name error:error]; if (!spv) return nil;
        NVMTLFunction *f0 = [NVMTLFunction new];
        f0->_spirv = spv; f0->_fname = name; f0->_stage = (_stages[name] ?: @""); f0->_air = _airs[name]; f0->_lib = self; f0->_fcv = cv;
        nvlog("newFunctionWithName:constantValues: \"%s\" - no constants, unspecialized module (%lu B)",
              name.UTF8String, (unsigned long)spv.length);
        return f0;
    }
    NSUInteger capacity = vals.count;
        __attribute__((objc_precise_lifetime)) NSMutableData *packed = nvmtl_fc_storage(capacity);
        uint32_t *idx = packed.mutableBytes, *sz = idx ? idx + capacity : NULL;
        const uint8_t **pay = sz ? (const uint8_t **)(sz + capacity) : NULL;
        (void)pay; size_t n = 0;
    if (!packed) { if(error)*error=[NSError errorWithDomain:@"NVMTL" code:4 userInfo:@{NSLocalizedDescriptionKey:@"function constant storage allocation failed"}]; return nil; }
    for (id v in vals) {
        Ivar ii = class_getInstanceVariable([v class], "_index");
        Ivar it = class_getInstanceVariable([v class], "_dataType");
        Ivar id_ = class_getInstanceVariable([v class], "_data");
        if (!ii || !it || !id_) { nvlog("function constants: MTLIndexedConstantValue has no _index/_dataType/_data"); vals = nil; break; }
        const uint8_t *base = (const uint8_t *)(__bridge void *)v;
        NSUInteger index = *(const NSUInteger *)(base + ivar_getOffset(ii));
        NSUInteger dtype = *(const NSUInteger *)(base + ivar_getOffset(it));
        const uint8_t *data = *(const uint8_t *const *)(base + ivar_getOffset(id_));
        size_t w = nvmtl_fc_width(dtype);
        if (!w || !data) { nvlog("function constants: index %lu has data type %lu, which this driver does not encode", (unsigned long)index, (unsigned long)dtype);
            if (error) *error = [NSError errorWithDomain:@"NVMTL" code:2 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"function constant %lu: unsupported data type %lu", (unsigned long)index, (unsigned long)dtype]}];
            return nil; }
        idx[n] = (uint32_t)index; sz[n] = (uint32_t)w; pay[n] = data; n++;
    }
    NSString *e = nil;
    NSMutableDictionary *constants=[NSMutableDictionary new];
    for(size_t k=0;k<n;k++){NSMutableArray *bytes=[NSMutableArray new];for(uint32_t j=0;j<sz[k];j++)[bytes addObject:@(pay[k][j])];constants[[@(idx[k]) stringValue]]=bytes;}
    if (([_stages[name] isEqualToString:@"mesh"] || [_stages[name] isEqualToString:@"object"]) && _airs[name]) {
        nvlog("newFunctionWithName:constantValues: \"%s\" is a %s function - %zu constant(s) DEFERRED to the mesh pipeline",
              name.UTF8String, [_stages[name] UTF8String], n);
        if (error) *error = nil;
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fc = [constants copy]; f->_fname = name; f->_stage = _stages[name];
        f->_air = _airs[name]; f->_lib = self; f->_fcv = cv; return f;
    }
    // A function that names visible functions or a function table is only translated once its pipeline links them
    // (nvmtl_link_stage). Translating it here can only fail, and the failure is not cached: MPS's depthwise
    // weight-gradient kernels paid seconds of failed translation on the encoding thread in every Geekbench run.
    // The reference query is cheap, so defer at once. NVMTL_EAGER_LINK_PROBE=1 restores the translate-first order.
    // B580: the answer is cached per AIR string (nvmtl_needs_visible_link_once) - fc-spec callers such as WindowServer's
    // uber shader ask here for every variant, and the query parses the whole module; the fc-spec translation is skipped
    // for such a function as well, since it fails the same way.
    static int eagerProbe = -1;
    if (eagerProbe < 0) { const char *ep = getenv("NVMTL_EAGER_LINK_PROBE"); eagerProbe = ep && ep[0] == '1'; }
    const BOOL linkFirst = !eagerProbe && _airs[name] && nvmtl_needs_visible_link_once(_airs[name]);
    // one translation for every set of values when the function's constants can be specialization constants
    if (!linkFirst && _airs[name] && nvmtl_fc_spec_wanted(_stages[name])) {
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        NSData *gen = nvmtl_translate_fc_spec_cached(_airs[name], _stages[name], name, idx, sz, pay, n, NULL);
        NSData *sv = gen ? nvmtl_fc_spec_variant(gen, idx, sz, pay, n) : nil;
        if (sv && getenv("NVMTL_FC_SPEC_DUMP")) {   // NVMTL_FC_SPEC_DUMP=<dir>: each variant module (and below, each baked one)
            static _Atomic unsigned seq; NSString *dir = @(getenv("NVMTL_FC_SPEC_DUMP"));
            [sv writeToFile:[dir stringByAppendingFormat:@"/%@-spec-%u.spv", name, atomic_fetch_add(&seq, 1)] atomically:NO];
        }
        if (sv) {
            static _Atomic unsigned told; if (atomic_fetch_add(&told, 1) < 16)
                nvlog("newFunctionWithName:constantValues: \"%s\" - %zu constant(s) as specialization constants (%.2f ms)",
                      name.UTF8String, n, (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0) / 1e6);
            NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = sv; f->_fc = [constants copy]; f->_fname = name;
            f->_stage = (_stages[name] ?: @""); f->_air = _airs[name]; f->_lib = self; f->_fcv = cv; f->_fcSpec = YES;
            return f;
        }
    }
    NSData *spec = nil;
    if (!linkFirst) {
        nvmtl_fc_expect_link(1);
        spec = nvmtl_translate_with_fc_cached(_airs[name], _stages[name], name, constants, idx, sz, pay, n, &e);
        nvmtl_fc_expect_link(0);
    }
    if (!spec && _airs[name] && (linkFirst || nvmtl_needs_visible_link(_airs[name], e))) {
        nvlog("newFunctionWithName:constantValues: \"%s\" has visible-function references - DEFERRED to pipeline creation (%zu constant(s))", name.UTF8String, n);
        if (error) *error = nil;
        NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = nil; f->_fc = [constants copy]; f->_fname = name; f->_stage = (_stages[name] ?: @"");
        f->_air = _airs[name]; f->_lib = self; f->_fcv = cv; f->_needsLink = YES; return f;
    }
    if (!spec) {
        nvlog("newFunctionWithName:constantValues: \"%s\" — %s", name.UTF8String, e.UTF8String ?: "specialization failed");
        if (error) *error = [NSError errorWithDomain:@"NVMTL" code:3 userInfo:@{NSLocalizedDescriptionKey: e ?: @"specialization failed"}];
        return nil;
    }
    if (getenv("NVMTL_FC_SPEC_DUMP")) {
        static _Atomic unsigned bseq; NSString *dir = @(getenv("NVMTL_FC_SPEC_DUMP"));
        [spec writeToFile:[dir stringByAppendingFormat:@"/%@-bake-%u.spv", name, atomic_fetch_add(&bseq, 1)] atomically:NO];
    }
    nvlog("newFunctionWithName:constantValues: \"%s\" specialized with %zu constant(s)", name.UTF8String, n);
    NVMTLFunction *f = [NVMTLFunction new]; f->_spirv = spec; f->_fc = [constants copy]; f->_fname = name; f->_stage = (_stages[name] ?: @""); f->_air = _airs[name]; f->_lib = self; f->_fcv = cv; return f;
}
- (void)newFunctionWithName:(NSString *)name constantValues:(MTLFunctionConstantValues *)cv
          completionHandler:(void (^)(id<MTLFunction>, NSError *))handler {
    if (!handler) return;
    NSError *err = nil;
    id<MTLFunction> f = [self newFunctionWithName:name constantValues:cv error:&err];
    handler(f, err);
}
- (id<MTLFunction>)newFunctionWithDescriptor:(id)desc error:(NSError **)error {
    if (error) *error = nil;
    if (!desc) {
        if (error) *error = [NSError errorWithDomain:@"NVMTL" code:1
                             userInfo:@{NSLocalizedDescriptionKey: @"newFunctionWithDescriptor: nil descriptor"}];
        nvlog("newFunctionWithDescriptor: nil descriptor");
        return nil;
    }
    NSString *name = [desc respondsToSelector:@selector(name)] ? [desc name] : nil;
    NSString *spec = [desc respondsToSelector:@selector(specializedName)] ? [desc specializedName] : nil;
    id cv = [desc respondsToSelector:@selector(constantValues)] ? [desc constantValues] : nil;
    if (![name isKindOfClass:[NSString class]] || !name.length) {
        if (error) *error = [NSError errorWithDomain:@"NVMTL" code:2
                             userInfo:@{NSLocalizedDescriptionKey: @"newFunctionWithDescriptor: descriptor has no name"}];
        nvlog("newFunctionWithDescriptor: %s has no usable name", object_getClassName(desc));
        return nil;
    }
    nvlog("newFunctionWithDescriptor: \"%s\"%s%s%s", name.UTF8String,
          spec.length ? " specializedName \"" : "", spec.length ? spec.UTF8String : "", spec.length ? "\"" : "");
    id<MTLFunction> f = cv ? [self newFunctionWithName:name constantValues:(MTLFunctionConstantValues *)cv error:error]
                           : [self newFunctionWithName:name];
    if (spec.length && [(id)f isKindOfClass:[NVMTLFunction class]]) ((NVMTLFunction *)f)->_specName = [spec copy];
    if (cv) return f;
    if (!f && error)
        *error = [NSError errorWithDomain:@"NVMTL" code:3
                  userInfo:@{NSLocalizedDescriptionKey:
                             [NSString stringWithFormat:@"this library has no function named %@", name]}];
    return f;
}
- (void)newFunctionWithDescriptor:(id)desc completionHandler:(void (^)(id<MTLFunction>, NSError *))handler {
    if (!handler) return;
    NSError *err = nil;
    id<MTLFunction> f = [self newFunctionWithDescriptor:desc error:&err];
    handler(f, err);
}
- (NSArray<NSString *> *)externFunctionNames {
    NSSet<NSString *> *x = nvmtl_mtlb_extern_names(_raw);
    return x.count ? x.allObjects : (_externs ?: @[]);
}

- (NSArray<NSString *> *)functionNames {
    NSMutableSet<NSString *> *s = [NSMutableSet setWithArray:_fns.allKeys];
    [s addObjectsFromArray:_airs.allKeys];
    [s minusSet:nvmtl_mtlb_extern_names(_raw)];
    return s.allObjects;
}

- (id)newExternFunctionWithName:(NSString *)name {
    if (![nvmtl_mtlb_extern_names(_raw) containsObject:name]) {
        nvlog("newExternFunctionWithName: \"%s\" is not an extern function of this library (%lu are)", name.UTF8String,
              (unsigned long)nvmtl_mtlb_extern_names(_raw).count);
        return nil;
    }
    id twin;
    @synchronized (self) { twin = [(id)gNVMTLMainDevice nvmtlAppleTwinOf:self]; }
    id f = [twin respondsToSelector:@selector(newExternFunctionWithName:)] ? [twin newExternFunctionWithName:name] : nil;
    if (!f) nvlog("newExternFunctionWithName: \"%s\" - the Apple twin %s it; answering nil (CI reports the kernel missing)",
                  name.UTF8String, twin ? "refused" : "could not be built for");
    return f;
}
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
@end

#define NVMTL_RESID_PIPE (1ull << 61)
static uint64_t nvmtl_resid_pipe(id o, uint64_t *slot) {
    static uint64_t seq;
    @synchronized (o) { if (!*slot) *slot = __atomic_add_fetch(&seq, 1, __ATOMIC_RELAXED) | NVMTL_RESID_PIPE; }
    return *slot;
}

#pragma mark - pipeline state
@implementation NVMTLRenderPipelineState
- (void)dealloc { nvmtl_vk_pipeline_destroy(&_p); }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSUInteger)maxTotalThreadsPerThreadgroup { return 1024; }
- (NSUInteger)threadExecutionWidth { return 32; }
- (BOOL)threadgroupSizeMatchesTileSize { return NO; }
- (MTLSize)requiredThreadsPerTileThreadgroup { return MTLSizeMake(0, 0, 0); }   // no tile pipelines here
- (MTLSize)requiredThreadsPerObjectThreadgroup { return _reqObj; }
- (MTLSize)requiredThreadsPerMeshThreadgroup { return _reqMesh; }
- (NSString *)label { return _descriptor.label; }
- (BOOL)supportIndirectCommandBuffers { return _descriptor.supportIndirectCommandBuffers; }
- (MTLResourceID)gpuResourceID {
    MTLResourceID r; r._impl = _descriptor.supportIndirectCommandBuffers ? nvmtl_resid_pipe(self, &_resid) : 0; return r; }
// The mesh and object stage limits are answered by the AppsPSOMesh category (NVMTLLibraryLoad.m) from the pipeline's
// mesh info; declared here so the compiler knows they exist.
@dynamic objectThreadExecutionWidth, meshThreadExecutionWidth, maxTotalThreadsPerObjectThreadgroup,
         maxTotalThreadsPerMeshThreadgroup, maxTotalThreadgroupsPerMeshGrid;
// Metal's header: the property is nil for a pipeline made by an MTLDevice; only an MTL4Compiler asked for shader
// reflection fills it, and this plugin has no MTL4Compiler. Reflection is handed out through the reflection: arguments.
- (MTLRenderPipelineReflection *)reflection API_AVAILABLE(macos(26.0)) { return nil; }
// The memory a residency set has to keep for the pipeline: none that Metal sees. The shader code lives in the Vulkan
// driver's own memory, whose size Vulkan does not report (the indirect command buffer answers 0 the same way).
- (NSUInteger)allocatedSize { return 0; }
- (NSUInteger)imageblockSampleLength { return 0; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { return 0; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
@end

#pragma mark - encoder
/* The command buffer holds every resource a command reads until it completes. The resources bound draw after draw
   (textures, buffers, samplers, pipelines) carry their set's epoch: when it matches, they are already held and the bind
   costs two loads (WAS: the set's lock, its 8-entry MRU and often a hash lookup, plus retain/release, for every bind:
   ~14% of a GL-like draw, mtl_bind gl 2026-10-29). Anything else, or a mismatch, takes the set's -addObject: path. */
static Class gRsSet, gRsTex, gRsBuf, gRsSamp, gRsPipe;
static void nvmtl_retain_resource(__unsafe_unretained NVMTLCommandBuffer *cb, __unsafe_unretained id resource) {
    if (!resource) return;
    if (!cb->_resources) cb->_resources = [NVMTLResSet new];
    static dispatch_once_t once;
    dispatch_once(&once, ^{ gRsSet = [NVMTLResSet class]; gRsTex = [NVMTLTexture class]; gRsBuf = [NVMTLBuffer class];
                            gRsSamp = [NVMTLSamplerState class]; gRsPipe = [NVMTLRenderPipelineState class]; });
    if (object_getClass(cb->_resources) != gRsSet) { [cb->_resources addObject:resource]; return; }   // (always a resource set today)
    __unsafe_unretained NVMTLResSet *s = (NVMTLResSet *)cb->_resources;
    const Class k = object_getClass(resource);
    uint64_t *tag = k == gRsTex ? &((NVMTLTexture *)resource)->_rsTag : k == gRsBuf ? &((NVMTLBuffer *)resource)->_rsTag
                  : k == gRsSamp ? &((NVMTLSamplerState *)resource)->_rsTag : k == gRsPipe ? &((NVMTLRenderPipelineState *)resource)->_rsTag : NULL;
    if (!tag) { [s addObject:resource]; return; }
    if (__atomic_load_n(tag, __ATOMIC_ACQUIRE) == __atomic_load_n(&s->_epoch, __ATOMIC_ACQUIRE)) return;
    [s nvmtlAdd:resource tag:tag];
}
// An app that declares a HEAP is not required to declare the resources inside it: useHeap: says the
// encoder may reach any of them indirectly, through an argument buffer or a bindless slot, and Metal
// keeps the heap alive and resident for the command buffer's lifetime. Every useHeap:/useHeaps:
// implementation here used to do nothing, so such a heap was retained by nobody: released after
// encoding, -[NVMTLHeap dealloc] ran nvmtl_vk_heap_destroy -> vkFreeMemory on the whole block while
// submitted work still read placed resources inside it, and the range lost its page table entry -
// the next access is an MMU fault on a valid-looking address. Cyberpunk 2077 references all four
// useHeap variants and creates heaps of 3 GiB and 384 MiB, so the window is large.
// The command buffer's resource set is class-checked everywhere it is read, so a heap sitting in it
// is skipped by the readers and only its lifetime matters.
static void nvmtl_retain_heaps(NVMTLCommandBuffer *cb, const id<MTLHeap> __unsafe_unretained heaps[], NSUInteger n) {
    if (!cb || !heaps) return;
    for (NSUInteger i = 0; i < n; i++) nvmtl_retain_resource(cb, heaps[i]);
}

// A heap in use: Metal's contract for useHeap: is that the encoder may reach any resource placed in the heap, through an
// argument buffer or a bindless slot, without naming it in useResource:. Every heap member is therefore held and made
// resident for the command buffer the way a used resource is. A pin is an immutable snapshot of the members at one
// version of the heap, shared by every command buffer that used the heap at that version, and it holds residency until
// the last submission that references it finishes. A delta pin covers a heap that only gained members since its last
// pin: it keeps that pin alive through _base and takes residency only on the new members (_own), so the members are not
// walked again on every use. The chain is collapsed by a full pin every 16 links so members the app frees are let go.
// _members and _copyCandidates are this link's only; -allMembers / -allCopyCandidates join the chain once and keep it.
//
// Two costs used to dominate a game's driver CPU time here (Cyberpunk 2077, a 3 GiB heap with thousands of members:
// 2249 full pins walking 11.9 million members in 32768 uses). First, every full pin took a residency reference
// (nvmtl_resref) on every member. Second, a pin died as soon as the last submission holding it finished, so a heap used
// once per frame was walked again from its weak membership table (one weak load under the runtime's weak lock per
// member) on nearly every frame even when nothing in it had changed.
//
// The residency reference is dead weight on a heap member: nvmtl_res2_movable / nvmtl_res3_movable and
// nvmtl_purge_eligible refuse anything with _heap, _subHeap or (textures) _backBuf, so the residency mover and the
// purger never page such a resource and never read its _resRefs, _useEp or _lastUse, and its _rstate never becomes 2,
// so the move wait is a no-op too. Members that are not heap-backed (or NVMTL_HEAP_PIN_LEGACY=1) still take it. _own is
// exactly the members that took one, so the release in -dealloc always matches the take.
// The one visible difference: the private isComplete / waitUntilComplete selectors on a heap member now report only
// the submissions that named it directly, not those that reached it only through useHeap:.
//
// The second cost is cut by keeping the newest pin of a heap alive for a short window after its full walk (see
// NVMTLHeapMembers), so the next frames reuse it or extend it with a delta instead of walking the heap again.
@interface NVMTLHeapPin : NSObject { @public NSArray *_members; NSArray *_copyCandidates; NSArray *_own; NVMTLHeapPin *_base; unsigned _depth;
                                     NSArray *_allMembers; NSArray *_allCopyCandidates; }
- (NSArray *)allMembers;
- (NSArray *)allCopyCandidates;
- (instancetype)initWithMembers:(NSArray *)members;
- (instancetype)initWithBase:(NVMTLHeapPin *)base added:(NSArray *)added;
- (instancetype)initFlatFromBase:(NVMTLHeapPin *)base added:(NSArray *)added;
@end
// NVMTL_HEAP_PIN_LEGACY=1 restores the pinning as it was before the kept pin, for A/B: every member takes a residency
// reference and no pin outlives its last submission.
static int nvmtl_heap_pin_legacy(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_HEAP_PIN_LEGACY"); on = e && e[0] == '1';
                  nvlog("heap pin: %s (NVMTL_HEAP_PIN_LEGACY=%s)", on ? "LEGACY - full residency refs, no kept pin" : "heap members skip residency refs, newest pin kept", e ? e : "unset"); }
    return on;
}
static _Atomic uint64_t gHeapPinRefsTaken, gHeapPinRefsSkipped;
// Does nvmtl_resref on this member do anything anyone reads? See the comment above NVMTLHeapPin for each reader.
static BOOL nvmtl_heap_pin_needs_ref(id o) {
    if (nvmtl_heap_pin_legacy()) return YES;
    if (!gNVMTLTexClass) gNVMTLTexClass = [NVMTLTexture class];
    if ([o isKindOfClass:gNVMTLTexClass]) {
        NVMTLTexture *t = o; if (t->_parent) t = t->_parent;   // nvmtl_resref counts on the owner
        return !(t->_heap || t->_subHeap || t->_backBuf);
    }
    static Class BC; if (!BC) BC = [NVMTLBuffer class];
    if (object_getClass(o) != BC) return NO;   // nvmtl_resref ignores every other class already
    NVMTLBuffer *b = o;
    return !(b->_heap || b->_subHeap);
}
@implementation NVMTLHeapPin
- (instancetype)initWithBase:(NVMTLHeapPin *)base added:(NSArray *)added {
    if ((self = [self initWithMembers:added])) { _base = base; _depth = base->_depth + 1; }
    return self;
}
- (NSArray *)allMembers {
    if (!_base) return _members;
    @synchronized (self) { if (!_allMembers) _allMembers = [[_base allMembers] arrayByAddingObjectsFromArray:_members]; return _allMembers; }
}
- (NSArray *)allCopyCandidates {
    if (!_base) return _copyCandidates;
    @synchronized (self) {
        if (!_allCopyCandidates) _allCopyCandidates = [[_base allCopyCandidates] arrayByAddingObjectsFromArray:_copyCandidates];
        return _allCopyCandidates;
    }
}
// Residency reference and host-copy classification for members this link adds; refs taken land in *own.
static void nvmtl_heap_pin_take(NSArray *members, NSMutableArray **own, NSMutableArray *copies) {
    uint64_t skipped = 0;
    for (id resource in members) {
        if (nvmtl_heap_pin_needs_ref(resource)) { nvmtl_resref(resource, 1); if (!*own) *own = [NSMutableArray new]; [*own addObject:resource]; }
        else skipped++;
        if (!copies) continue;
        NVMTLBuffer *buffer = nil;
        if ([resource isKindOfClass:[NVMTLBuffer class]]) buffer = resource;
        else if ([resource isKindOfClass:[NVMTLTexture class]]) {
            NVMTLTexture *texture = resource;
            buffer = texture->_backBuf ?: (texture->_parent ? texture->_parent->_backBuf : nil);
        }
        if (buffer && (buffer->_hostPtr || buffer->_shadow.map)) [copies addObject:resource];
    }
    atomic_fetch_add(&gHeapPinRefsTaken, members.count - skipped); atomic_fetch_add(&gHeapPinRefsSkipped, skipped);
}
- (instancetype)initWithMembers:(NSArray *)members {
    if ((self = [super init])) {
        _members = members;
        NSMutableArray *copies = [NSMutableArray new], *own = nil;
        nvmtl_heap_pin_take(members, &own, copies);
        _copyCandidates = [copies copy];
        _own = [own copy];
    }
    return self;
}
// A chain that reached 16 links collapses into one flat pin built from the chain's own strong arrays: the same members
// a walk of the weak table would find right now (every member alive is either in the chain or listed as added since),
// without one weak load per member. The flat pin takes its own references; the chain releases its own when it dies.
- (instancetype)initFlatFromBase:(NVMTLHeapPin *)base added:(NSArray *)added {
    if ((self = [super init])) {
        NSMutableArray *own = nil, *copies = [NSMutableArray new], *chainOwn = [NSMutableArray new];
        for (NVMTLHeapPin *p = base; p; p = p->_base) if (p->_own.count) [chainOwn addObjectsFromArray:p->_own];
        if (chainOwn.count) nvmtl_heap_pin_take(chainOwn, &own, nil);   // members that need a reference, again for this pin
        nvmtl_heap_pin_take(added, &own, copies);
        _members = [[base allMembers] arrayByAddingObjectsFromArray:added];
        _copyCandidates = [[base allCopyCandidates] arrayByAddingObjectsFromArray:copies];
        _own = [own copy];
    }
    return self;
}
- (void)dealloc { for (id resource in _own) nvmtl_resref(resource, -1); }   // only what this link took; _base releases the rest
@end
// The heap's membership: weak, so the heap never owns its members through this; _added lists the members registered
// since the last pin decision, for the next delta.
//
// What a use costs. Same version as the cached pin: nothing (reuse). Members added since: a delta link with just them.
// A chain of 16 links: one flat pin from the chain's strong arrays (no weak loads). Only when no pin of the heap is
// alive at all is the weak table walked, one weak load per member. Measured in Cyberpunk 2077 before these rules
// (3 GiB heap, ~12k members): 1329 full walks in 32768 uses even with a kept pin, because
//   - a pin was cached only if the heap's version had not moved while it was being built; the game registers members
//     all the time, a walk of 12k members takes long enough to see one, and the uncached pin left the next use with
//     no pin to extend, so it walked again (and again) - each walk invalidated by the next registration; and
//   - command buffers encoded on several threads that missed together each walked the same heap at once.
// Now _added is taken over at the decision (the new list collects what registers during the build), every built pin
// is installed with the version it covers, and one build runs per heap at a time: a second user waits for it
// (bounded, see kHeapPinWaitS) and then reuses or extends it.
//
// _keep holds the newest pin strongly for NVMTL_HEAP_PIN_KEEP_MS (default 200 ms, 0 = off) after a WALK, so a heap
// whose submissions drain between frames is not walked again every frame. The window is anchored to the walk: deltas
// and flat collapses move the keep to the new chain head but never extend the deadline, so once it passes the chain
// lives only as long as submissions hold it, as before, and the first drain after that lets every member the app
// released go. A walk can only happen when no pin is alive, so it never re-captures a member held only by an old pin.
// The keep is dropped by a timer at the deadline (this also breaks the heap -> membership -> kept pin -> member ->
// heap cycle when the app releases the heap) and when an automatic heap is out of room.
// Every access to the membership is under _cv's lock (a condition, so a waiting user can sleep on the build).
@interface NVMTLHeapMembers : NSObject { @public NSHashTable *_members; uint64_t _version; __weak NVMTLHeapPin *_pin; uint64_t _pinVersion; NSPointerArray *_added;
                                        NVMTLHeapPin *_keep; uint64_t _keepUntil, _keepGen; NSCondition *_cv; BOOL _building, _expired, _everPinned;
                                        // per-heap census
                                        uint64_t _uses, _reuses, _deltas, _flats, _waits, _walks, _walked, _why[4]; }
@end
@implementation NVMTLHeapMembers
- (instancetype)init { if ((self = [super init])) { _members = [NSHashTable weakObjectsHashTable]; _added = [NSPointerArray weakObjectsPointerArray];
                                                     _cv = [NSCondition new]; } return self; }
@end
// Per command buffer: the version of each heap it already holds a pin for, so a heap used by every encoder of a frame
// is pinned once per command buffer.
@interface NVMTLHeapUseCache : NSObject { @public NSLock *_lock; NSMutableDictionary *_versions; }
@end
@implementation NVMTLHeapUseCache
- (instancetype)init { if ((self = [super init])) { _lock = [NSLock new]; _versions = [NSMutableDictionary new]; } return self; }
@end
static const char kHeapMembers, kHeapUseCache;
static NVMTLHeapMembers *nvmtl_heap_members(NVMTLHeap *heap) {
    if (!heap) return nil;
    @synchronized (heap) {
        NVMTLHeapMembers *m = objc_getAssociatedObject(heap, &kHeapMembers);
        if (!m) { m = [NVMTLHeapMembers new]; objc_setAssociatedObject(heap, &kHeapMembers, m, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
        return m;
    }
}
static uint64_t nvmtl_heap_pin_keep_ns(void) {
    static int64_t ns = -1;
    if (ns < 0) { const char *e = getenv("NVMTL_HEAP_PIN_KEEP_MS"); long long ms = e && *e ? strtoll(e, NULL, 10) : 200;
                  if (ms < 0 || ms > 10000) { nvlog("heap pin: NVMTL_HEAP_PIN_KEEP_MS=%s out of range 0..10000 - using 200", e); ms = 200; }
                  ns = nvmtl_heap_pin_legacy() ? 0 : (int64_t)ms * 1000000;
                  nvlog("heap pin: newest pin kept %lld ms after a walk (NVMTL_HEAP_PIN_KEEP_MS=%s)", (long long)(ns / 1000000), e ? e : "unset"); }
    return (uint64_t)ns;
}
// A use that finds another thread building the heap's pin waits at most this long, then walks for itself without
// installing (correct, just the old cost) and says so: a build that never finishes must not hang the encoder.
static const double kHeapPinWaitS = 2.0;
static _Atomic uint64_t gHeapPinKeepDrops[2];   // [0] expired (timer or use), [1] heap out of room
// Takes the kept pin out under the lock; the caller lets it go OUTSIDE it: the last release of a pin can free members,
// whose dealloc returns heap ranges and destroys Vulkan objects.
// Returned retained, never autoreleased: an autoreleased pin would live until the app's pool drains, and the
// out-of-room retry would see no space come back.
static NVMTLHeapPin *nvmtl_heap_unkeep_locked(NVMTLHeapMembers *m, int why) NS_RETURNS_RETAINED;
static NVMTLHeapPin *nvmtl_heap_unkeep_locked(NVMTLHeapMembers *m, int why) {
    NVMTLHeapPin *k = m->_keep; m->_keep = nil; m->_keepUntil = 0;
    if (k) { atomic_fetch_add(&gHeapPinKeepDrops[why], 1); if (why == 0) m->_expired = YES; }
    return k;
}
// An automatic heap that cannot fit an allocation: let the kept pin go first, so members the app already released
// (and no submission holds) are freed and their space returns. YES when a pin was let go, so the caller retries once.
static BOOL nvmtl_heap_unkeep(NVMTLHeap *heap) {
    NVMTLHeapMembers *m = objc_getAssociatedObject(heap, &kHeapMembers);
    if (!m) return NO;
    BOOL had;
    @autoreleasepool {   // anything the last release of the pin autoreleases goes now, not at the app's next drain
        NVMTLHeapPin *k;
        [m->_cv lock]; k = nvmtl_heap_unkeep_locked(m, 1); [m->_cv unlock];
        had = k != nil;
        k = nil; (void)k;
    }
    return had;
}
static void nvmtl_heap_register(NVMTLHeap *heap, id resource) {
    if (!heap || !resource) return;
    NVMTLHeapMembers *m = nvmtl_heap_members(heap);
    [m->_cv lock];
    if (![m->_members containsObject:resource]) { [m->_members addObject:resource]; [m->_added addPointer:(__bridge void *)resource]; m->_version++; }
    [m->_cv unlock];
}
// NVMTL_HEAP_PIN_LEGACY=1: the use path exactly as it was before the kept pin and the single build, for A/B.
static void nvmtl_use_heap_legacy(NVMTLCommandBuffer *cb, NVMTLHeap *heap, NVMTLHeapUseCache *cache) {
    NVMTLHeapMembers *members = nvmtl_heap_members(heap);
    [cache->_lock lock];
    NSArray *resources = nil, *added = nil; NVMTLHeapPin *pin = nil, *base = nil; uint64_t version;
    NSValue *key = [NSValue valueWithNonretainedObject:heap];
    [members->_cv lock]; {
        version = members->_version;
        NSNumber *prior = cache->_versions[key];
        if (!prior || prior.unsignedLongLongValue != version) {
            if (members->_pinVersion == version) pin = members->_pin;
            if (!pin) {
                base = members->_pin;   // the last pin, if a submission still holds it
                if (base && base->_depth < 16) { [members->_added compact]; added = members->_added.allObjects; }
                else { base = nil; resources = members->_members.allObjects; }
            }
        }
        { static _Atomic uint64_t calls, rebuilds, walked, deltas; const uint64_t c = atomic_fetch_add(&calls, 1) + 1;
          if (resources) { atomic_fetch_add(&rebuilds, 1); atomic_fetch_add(&walked, resources.count); }
          if (added) atomic_fetch_add(&deltas, 1);
          if ((c & 4095) == 0) nvlog("heap pin (legacy): %llu uses, %llu full pins walking %llu members, %llu delta pins", (unsigned long long)c,
                                     (unsigned long long)atomic_load(&rebuilds), (unsigned long long)atomic_load(&walked), (unsigned long long)atomic_load(&deltas)); }
    } [members->_cv unlock];
    // Residency can wait inside allocator locks: build the pin outside the membership monitor.
    if (resources || added) {
        pin = added ? [[NVMTLHeapPin alloc] initWithBase:base added:added] : [[NVMTLHeapPin alloc] initWithMembers:resources];
        [members->_cv lock]; {
            // The added list is cleared only when this pin is the one cached: if the heap gained members meanwhile, the
            // members just consumed stay listed, or the next delta would miss them (an unpinned member is a GPU fault).
            if (members->_version == version) { members->_pin = pin; members->_pinVersion = version;
                                                 members->_added = [NSPointerArray weakObjectsPointerArray]; }
        } [members->_cv unlock];
    }
    if (pin) {
        nvmtl_retain_resource(cb, heap);
        nvmtl_retain_resource(cb, pin);
        cache->_versions[key] = @(version);
    }
    [cache->_lock unlock];
}

enum { kHeapWalkFirst, kHeapWalkExpired, kHeapWalkDied, kHeapWalkTimeout };   // why a use had to walk the weak table
static void nvmtl_use_heap(NVMTLCommandBuffer *cb, id<MTLHeap> value) {
    NVMTLHeap *heap = (NVMTLHeap *)value;
    if (!cb || !heap || ![heap isKindOfClass:[NVMTLHeap class]]) return;
    NVMTLHeapUseCache *cache;
    // A lock object of its own: the command buffer's monitor may be held by the application.
    @synchronized ([NVMTLHeapUseCache class]) {
        cache = objc_getAssociatedObject(cb, &kHeapUseCache);
        if (!cache) { cache = [NVMTLHeapUseCache new]; objc_setAssociatedObject(cb, &kHeapUseCache, cache, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    }
    if (nvmtl_heap_pin_legacy()) { nvmtl_use_heap_legacy(cb, heap, cache); return; }
    NVMTLHeapMembers *members = nvmtl_heap_members(heap);
    [cache->_lock lock];
    NSValue *key = [NSValue valueWithNonretainedObject:heap];
    NSNumber *prior = cache->_versions[key];
    const uint64_t keepNs = nvmtl_heap_pin_keep_ns();
    enum { kNone, kReuse, kDelta, kFlat, kWalk } kind = kNone;
    NSArray *resources = nil, *added = nil; NVMTLHeapPin *pin = nil, *base = nil; uint64_t version = 0;
    NSMutableArray *dropped = nil;   // kept pins that expired while this use looked; released after the lock
    BOOL install = NO, waited = NO; int why = -1;
    NSDate *waitUntil = nil;
    [members->_cv lock];
    members->_uses++;
    for (;;) {
        if (members->_keep && clock_gettime_nsec_np(CLOCK_UPTIME_RAW) >= members->_keepUntil) {
            NVMTLHeapPin *d = nvmtl_heap_unkeep_locked(members, 0);
            if (!dropped) dropped = [NSMutableArray new];
            [dropped addObject:d];
        }
        version = members->_version;
        if (prior && prior.unsignedLongLongValue == version) break;   // this command buffer already holds a pin at this version
        NVMTLHeapPin *cur = members->_pin;
        if (cur && members->_pinVersion == version) { pin = cur; kind = kReuse; members->_reuses++; break; }
        if (members->_building) {
            if (!waitUntil) { waitUntil = [NSDate dateWithTimeIntervalSinceNow:kHeapPinWaitS]; members->_waits++; waited = YES; }
            if ([members->_cv waitUntilDate:waitUntil]) continue;
            // the build did not finish in time: walk privately, install nothing, leave the builder alone
            nvlog("heap pin: waited %.0f s for another thread's pin build of heap %p - walking %lu members privately (not installed)",
                  kHeapPinWaitS, (__bridge void *)heap, (unsigned long)members->_members.count);
            resources = members->_members.allObjects; kind = kWalk; why = kHeapWalkTimeout; break;
        }
        // This thread builds. _added is taken over now: members that register during the build land in the new list and
        // move the version, so the next use extends this pin with exactly them.
        [members->_added compact];
        if (cur && cur->_depth < 16) { base = cur; added = members->_added.allObjects; kind = kDelta; }
        else if (cur) { base = cur; added = members->_added.allObjects; kind = kFlat; }
        else { resources = members->_members.allObjects; kind = kWalk;
               why = !members->_everPinned ? kHeapWalkFirst : members->_expired ? kHeapWalkExpired : kHeapWalkDied; }
        members->_added = [NSPointerArray weakObjectsPointerArray];
        members->_building = YES; install = YES;
        break;
    }
    if (kind == kDelta) members->_deltas++;
    if (kind == kFlat) members->_flats++;
    if (kind == kWalk) { members->_walks++; members->_walked += resources.count; members->_why[why]++; }
    const BOOL sayHeap = kind == kWalk && (members->_walks <= 4 || (members->_walks & 63) == 0);
    uint64_t hc[11] = { members->_uses, members->_reuses, members->_deltas, members->_flats, members->_waits, members->_walks, members->_walked,
                        members->_why[0], members->_why[1], members->_why[2], members->_why[3] };
    const NSUInteger hcount = sayHeap ? members->_members.count : 0;
    [members->_cv unlock];
    dropped = nil; (void)dropped;   // outside the lock
    if (sayHeap)
        nvlog("heap pin: heap %p (%llu MB, ~%lu members): %llu uses, %llu reuses, %llu deltas, %llu flat collapses, %llu waits; "
              "%llu walks over %llu members (first %llu, after keep expiry %llu, pin died unkept %llu, wait timeout %llu)",
              (__bridge void *)heap, (unsigned long long)(heap->_size >> 20), (unsigned long)hcount,
              (unsigned long long)hc[0], (unsigned long long)hc[1], (unsigned long long)hc[2], (unsigned long long)hc[3], (unsigned long long)hc[4],
              (unsigned long long)hc[5], (unsigned long long)hc[6], (unsigned long long)hc[7], (unsigned long long)hc[8], (unsigned long long)hc[9],
              (unsigned long long)hc[10]);
    { static _Atomic uint64_t calls, walks, walked, deltas, flats, reuses, waits;
      const uint64_t c = atomic_fetch_add(&calls, 1) + 1;
      if (kind == kWalk) { atomic_fetch_add(&walks, 1); atomic_fetch_add(&walked, resources.count); }
      if (kind == kDelta) atomic_fetch_add(&deltas, 1);
      if (kind == kFlat) atomic_fetch_add(&flats, 1);
      if (kind == kReuse) atomic_fetch_add(&reuses, 1);
      if (waited) atomic_fetch_add(&waits, 1);
      if ((c & 4095) == 0) nvlog("heap pin: %llu uses (all heaps), %llu full pins walking %llu members, %llu delta pins, %llu flat collapses, "
                                 "%llu pin reuses, %llu waits on another build; member refs %llu taken / %llu skipped (heap-backed); "
                                 "keep dropped %llu expired, %llu for room",
                                 (unsigned long long)c, (unsigned long long)atomic_load(&walks), (unsigned long long)atomic_load(&walked),
                                 (unsigned long long)atomic_load(&deltas), (unsigned long long)atomic_load(&flats), (unsigned long long)atomic_load(&reuses),
                                 (unsigned long long)atomic_load(&waits),
                                 (unsigned long long)atomic_load(&gHeapPinRefsTaken), (unsigned long long)atomic_load(&gHeapPinRefsSkipped),
                                 (unsigned long long)atomic_load(&gHeapPinKeepDrops[0]), (unsigned long long)atomic_load(&gHeapPinKeepDrops[1])); }
    // Residency can wait inside allocator locks: build the pin outside the membership lock.
    if (kind == kDelta || kind == kFlat || kind == kWalk) {
        pin = kind == kDelta ? [[NVMTLHeapPin alloc] initWithBase:base added:added]
            : kind == kFlat  ? [[NVMTLHeapPin alloc] initFlatFromBase:base added:added]
            :                  [[NVMTLHeapPin alloc] initWithMembers:resources];
        if (install) {
            NVMTLHeapPin *replaced = nil; uint64_t keepGen = 0;
            [members->_cv lock];
            members->_pin = pin; members->_pinVersion = version; members->_everPinned = YES; members->_building = NO;
            // A walk opens a keep window; a delta or collapse inside an open window moves the keep to the new chain head
            // without extending the deadline; one after it closed is not kept.
            if (keepNs && kind == kWalk) { replaced = members->_keep; members->_keep = pin; members->_expired = NO;
                                           members->_keepUntil = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + keepNs; keepGen = ++members->_keepGen; }
            else if (members->_keep) { replaced = members->_keep; members->_keep = pin; }
            [members->_cv broadcast];
            [members->_cv unlock];
            replaced = nil; (void)replaced;   // outside the lock
            if (keepGen) {
                __weak NVMTLHeapMembers *wm = members;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)keepNs), dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    NVMTLHeapMembers *m = wm; if (!m) return;
                    NVMTLHeapPin *k = nil;
                    [m->_cv lock]; if (m->_keepGen == keepGen) k = nvmtl_heap_unkeep_locked(m, 0); [m->_cv unlock];
                    k = nil; (void)k;
                });
            }
        }
    }
    base = nil; (void)base;
    if (pin) {
        nvmtl_retain_resource(cb, heap);
        nvmtl_retain_resource(cb, pin);
        cache->_versions[key] = @(version);
    }
    [cache->_lock unlock];
}

@implementation NVMTLCounter
- (NSString *)name { return MTLCommonCounterTimestamp; }
@end
@implementation NVMTLCounterSet
- (NSString *)name { return MTLCommonCounterSetTimestamp; }
- (NSArray *)counters { return _counters; }
@end
id<MTLCounterSet> nvmtl_timestamp_counter_set(void)
{
    static NVMTLCounterSet *set; static dispatch_once_t once;
    dispatch_once(&once, ^{ NVMTLCounterSet *s = [NVMTLCounterSet new]; s->_counters = @[[NVMTLCounter new]]; set = s; });
    return set;
}
@implementation NVMTLCounterSampleBuffer
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label ?: @""; }
- (NSUInteger)sampleCount { return _count; }
- (NSData *)resolveCounterRange:(NSRange)r {
    if (!r.length || r.location > _count || r.length > _count - r.location) {
        nvlog("resolveCounterRange: %lu+%lu is outside this buffer's %lu samples - nil", (unsigned long)r.location, (unsigned long)r.length, (unsigned long)_count);
        return nil;
    }
    NSMutableData *d = [NSMutableData dataWithLength:r.length * 8];
    if (nvmtl_vk_ts_read(_pool, (uint32_t)r.location, (uint32_t)r.length, d.mutableBytes)) { nvlog("resolveCounterRange: read FAILED - nil"); return nil; }
    return d;
}
- (void)dealloc { nvmtl_vk_ts_pool_destroy(_pool); _pool = NULL; }
@end
static void nvmtl_counter_recording_failed(NVMTLCommandBuffer *cb)
{
    cb->_c.counter_error = 1;
    [cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
        code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey:@"Counter recording failed"}]];
}
static void nvmtl_sample_counter(NVMTLCommandBuffer *cb, id sb, NSUInteger i, id who, BOOL barrier)
{
    static int said_foreign, said_range, said_fail;
    if (![sb isKindOfClass:[NVMTLCounterSampleBuffer class]]) {
        if (said_foreign++ < 4) nvlog("%s sampleCountersInBuffer: %s is not a sample buffer this device made - not sampled", object_getClassName(who), sb ? object_getClassName(sb) : "nil");
        nvmtl_counter_recording_failed(cb); return;
    }
    NVMTLCounterSampleBuffer *b = sb;
    if (i >= b->_count) {
        if (said_range++ < 4) nvlog("%s sampleCountersInBuffer: index %lu is past the buffer's %lu samples - not sampled", object_getClassName(who), (unsigned long)i, (unsigned long)b->_count);
        nvmtl_counter_recording_failed(cb); return;
    }
    if (!cb->_resources) cb->_resources = [NVMTLResSet new];
    [cb->_resources addObject:b];
    if (nvmtl_vk_cmd_ts_write(&cb->_c, b->_pool, (uint32_t)i, barrier)) {
        nvmtl_counter_recording_failed(cb);
        if (said_fail++ < 4) nvlog("%s sampleCountersInBuffer: index %lu - recording failed", object_getClassName(who), (unsigned long)i);
    }
}

// Stage-boundary sampling: a pass descriptor's sampleBufferAttachments (up to 4 sample buffers). The start indices are
// sampled when the encoder is made, before any of its commands; the end indices when it ends, after all of them (for
// a render pass after the Vulkan render pass and its stores and resolves). Vertex and fragment stages are not separate
// phases here: both starts are sampled at the start, both ends at the end. Bottom-of-pipe writes, so a pass's start is
// when the work before it has finished. WebKit's WebGPU timestampWrites arrive this way.
#define NVMTL_TS_ATTACHMENTS 4
static NSArray *nvmtl_ts_pass_begin(NVMTLCommandBuffer *cb, id who, id attachments, BOOL render)
{
    NSMutableArray *ends = nil;
    for (NSUInteger a = 0; attachments && a < NVMTL_TS_ATTACHMENTS; a++) {
        id at = [attachments objectAtIndexedSubscript:a];
        id sb = [at sampleBuffer];
        if (!sb) continue;
        NSUInteger s0, s1, e0, e1;
        if (render) {
            MTLRenderPassSampleBufferAttachmentDescriptor *r = at;
            s0 = r.startOfVertexSampleIndex; s1 = r.startOfFragmentSampleIndex; e0 = r.endOfVertexSampleIndex; e1 = r.endOfFragmentSampleIndex;
        } else {
            s0 = [at startOfEncoderSampleIndex]; e0 = [at endOfEncoderSampleIndex]; s1 = e1 = MTLCounterDontSample;
        }
        if (s0 != MTLCounterDontSample) nvmtl_sample_counter(cb, sb, s0, who, YES);
        if (s1 != MTLCounterDontSample && s1 != s0) nvmtl_sample_counter(cb, sb, s1, who, YES);
        if (e0 != MTLCounterDontSample || e1 != MTLCounterDontSample) {
            if (!ends) ends = [NSMutableArray new];
            if (e0 != MTLCounterDontSample) { [ends addObject:sb]; [ends addObject:@(e0)]; }
            if (e1 != MTLCounterDontSample && e1 != e0) { [ends addObject:sb]; [ends addObject:@(e1)]; }
        }
    }
    return ends;
}
static void nvmtl_ts_pass_end(NVMTLCommandBuffer *cb, id who, NSArray *ends)
{
    for (NSUInteger i = 0; cb && i + 1 < ends.count; i += 2)
        nvmtl_sample_counter(cb, ends[i], [ends[i + 1] unsignedIntegerValue], who, YES);
}

unsigned nvmtl_apps1_resolve_miss, nvmtl_apps1_resolve_extra, nvmtl_apps1_ds_resolve_miss;
@interface NVMTLRenderCommandEncoder () { unsigned _groupDepth, _dumpDepth; NSString *_dumpName; BOOL _dumpPso; }
- (void)nvmtlDumpBoundFragmentBuffers;
@end
@implementation NVMTLRenderCommandEncoder
- (NSUInteger)tileWidth { return 0; }
- (NSUInteger)tileHeight { return 0; }
- (unsigned long long)globalTraceObjectID { return nvmtl_global_trace_id(self); }
static NSMutableDictionary *g_sampvar; static pthread_mutex_t g_sampvar_lock = PTHREAD_MUTEX_INITIALIZER;
extern unsigned char *CC_SHA256(const void *data, uint32_t len, unsigned char *md);
static char kNVMTLSampvarAir;
static NSString *nvmtl_air_digest(NVMTLFunction *f) {
    if (!f) return @"-";
    NSString *h = objc_getAssociatedObject(f, &kNVMTLSampvarAir); if (h) return h;
    NSData *u = [f->_air dataUsingEncoding:NSUTF8StringEncoding]; unsigned char md[32];
    CC_SHA256(u.bytes, (uint32_t)u.length, md);
    NSMutableString *x = [NSMutableString stringWithCapacity:64]; for (int i = 0; i < 32; i++) [x appendFormat:@"%02x", md[i]];
    objc_setAssociatedObject(f, &kNVMTLSampvarAir, x, OBJC_ASSOCIATION_RETAIN_NONATOMIC); return x;
}
static NSString *nvmtl_sampvar_key(MTLRenderPipelineDescriptor *d, NVMTLFunction *vf, NVMTLFunction *ff, NSData *skey) {
    static int off = -1; if (off < 0) off = getenv("NVMTL_NO_SAMPVAR") != NULL;
    if (off || !d || !vf || !vf->_air || (ff && !ff->_air)) return nil;
    MTLRenderPipelineDescriptor *c = [d copy]; c.vertexFunction = nil; c.fragmentFunction = nil; c.label = @"";
    for (MTLLinkedFunctions *lf in @[c.vertexLinkedFunctions ?: (id)[NSNull null], c.fragmentLinkedFunctions ?: (id)[NSNull null]]) {
        if ((id)lf == [NSNull null]) continue;
        if (lf.functions.count || lf.binaryFunctions.count || lf.groups.count || lf.privateFunctions.count) return nil;
    }
    NSString *ds = c.description; NSRange nl = [ds rangeOfString:@"\n"];
    if (nl.location != NSNotFound) ds = [ds substringFromIndex:nl.location];
    static NSRegularExpression *addr; static dispatch_once_t once;
    dispatch_once(&once, ^{ addr = [NSRegularExpression regularExpressionWithPattern:@"0x[0-9a-fA-F]+" options:0 error:NULL]; });
    ds = [addr stringByReplacingMatchesInString:ds options:0 range:NSMakeRange(0, ds.length) withTemplate:@"@"];
    NSString *sk = [[NSString alloc] initWithData:skey encoding:NSUTF8StringEncoding] ?: @"";
    return [NSString stringWithFormat:@"%@|%@|%@|%@||%@|%@|%@|%@||%@||%@", nvmtl_air_digest(vf), vf->_fname, vf->_stage, vf->_fc ?: @{},
            nvmtl_air_digest(ff), ff ? ff->_fname : @"", ff ? ff->_stage : @"", ff ? (ff->_fc ?: @{}) : @{}, sk, ds];
}
static id nvmtl_sampvar_get(NSString *k) {
    if (!k) return nil;
    pthread_mutex_lock(&g_sampvar_lock); id e = g_sampvar[k]; pthread_mutex_unlock(&g_sampvar_lock);
    if (e) nvlog("sampvar: device cache HIT - a new pipeline state of the same shaders + samplers + descriptor reuses its variant");
    return e;
}
static void nvmtl_sampvar_put(NSString *k, id ps, NVMTLFunction *vf, NVMTLFunction *ff) {
    if (!k || !ps) return;
    pthread_mutex_lock(&g_sampvar_lock);
    if (!g_sampvar) g_sampvar = [NSMutableDictionary new];
    if (g_sampvar.count >= 1024) { [g_sampvar removeAllObjects]; nvlog("sampvar: device cache reached 1024 variants - cleared"); }
    g_sampvar[k] = ps;
    pthread_mutex_unlock(&g_sampvar_lock);
}
static NSDictionary *nvmtl_pixel_sampler_norm(NSDictionary *m) {
    static int off = -1; if (off < 0) off = getenv("NVMTL_NO_SAMPKEY") != NULL;
    if (off || !m.count) return m ?: @{};
    NSMutableDictionary *o = [NSMutableDictionary dictionaryWithCapacity:m.count];
    for (NSString *k in m) { NSMutableDictionary *st = [m[k] mutableCopy]; st[@"lod_max_clamp"] = @65504.0f; o[k] = st; }
    return o;
}
static BOOL nvmtl_pixel_sampler_update(NSMutableDictionary *states, NSString *index, NSDictionary *state) {
    NSDictionary *old = states[index];
    if (old == state || [old isEqual:state]) return NO;
    if (state) states[index] = state; else [states removeObjectForKey:index];
    return YES;
}
- (NVMTLRenderPipelineState *)nvmtlSamplerPipeline {
    NVMTLRenderPipelineState *selected=_ps;
    {
        NSDictionary *vS=nil,*fS=nil;
        if (!_pixelSamplerKey) {
            NSDictionary *vertex=_vertexSamplers ?: @{}, *fragment=_fragmentSamplers ?: @{};
            for (NSArray *entry in _pixelSamplerKeys) {
                if ([entry[0] isEqual:vertex] && [entry[1] isEqual:fragment]) { _pixelSamplerKey=entry[2]; break; }
            }
            if (!_pixelSamplerKey) {
                vS=nvmtl_pixel_sampler_norm(vertex); fS=nvmtl_pixel_sampler_norm(fragment);
                _pixelSamplerKey=[NSJSONSerialization dataWithJSONObject:@[vS,fS] options:NSJSONWritingSortedKeys error:NULL];
                if (!_pixelSamplerKeys) _pixelSamplerKeys=[NSMutableArray new];
                if (_pixelSamplerKeys.count == 8) [_pixelSamplerKeys removeObjectAtIndex:0];
                [_pixelSamplerKeys addObject:@[[vertex copy],[fragment copy],_pixelSamplerKey]];
            }
        }
        NSData *key=_pixelSamplerKey;
        @synchronized(_ps) {
            if(!_ps->_samplerVariants) _ps->_samplerVariants=[NSMutableDictionary new];
            id cached=_ps->_samplerVariants[key];
            if(cached==[NSNull null]) return nil;
            selected=cached;
            if(!selected) {
                // Reusing the key across pipelines still performs each pipeline's own variant lookup.
                if (!vS) vS=nvmtl_pixel_sampler_norm(_vertexSamplers);
                if (!fS) fS=nvmtl_pixel_sampler_norm(_fragmentSamplers);
                NVMTLFunction *vf=_ps->_linkedVert?:(id)_ps->_descriptor.vertexFunction,*ff=_ps->_linkedFrag?:(id)_ps->_descriptor.fragmentFunction;
                NSString *why=nil;
                NSString *gk=nvmtl_sampvar_key(_ps->_descriptor,vf,ff,key); selected=nvmtl_sampvar_get(gk);
                NSData *v=selected?nil:nvmtl_translate_samplers(vf,vS,0,&why),*f=(selected||!v)?nil:(ff?nvmtl_translate_samplers(ff,fS,_ps->_a8mask,&why):[NSData data]);
                if(v && f) {
                    NVMTLFunction *vs=[NVMTLFunction new],*fs=ff?[NVMTLFunction new]:nil;
                    vs->_spirv=v;vs->_fname=vf->_fname;vs->_stage=vf->_stage;
                    if(fs){fs->_spirv=f;fs->_fname=ff->_fname;fs->_stage=ff->_stage;}
                    MTLRenderPipelineDescriptor *desc=[_ps->_descriptor copy];desc.vertexFunction=vs;desc.fragmentFunction=fs;
                    NSError *error=nil;selected=(id)[(id<MTLDevice>)gNVMTLMainDevice newRenderPipelineStateWithDescriptor:desc error:&error];
                    if(!selected)why=error.description;
                    else nvmtl_sampvar_put(gk,selected,vf,ff);
                }
                if(!selected){nvlog("pixel sampler specialization REFUSED %s: %s",_ps->_fname.UTF8String,why.UTF8String);_ps->_samplerVariants[key]=[NSNull null];return nil;}
                _ps->_samplerVariants[key]=selected;
                nvlog("pixel sampler variant built: %s / %s",_ps->_vname.UTF8String,_ps->_fname.UTF8String);
            }
        }
    }
    return selected;
}
// the pipeline's vertex and fragment NVMTLFunction, read once from its (immutable) descriptor copy
__attribute__((noinline)) static void nvmtl_ps_bindfns_slow(__unsafe_unretained NVMTLRenderPipelineState *ps) {
    NVMTLFunction *vf = (NVMTLFunction *)ps->_descriptor.vertexFunction, *ff = (NVMTLFunction *)ps->_descriptor.fragmentFunction;
    @synchronized (ps) {
        if (!__atomic_load_n(&ps->_bindFnReady, __ATOMIC_RELAXED)) {
            ps->_bindVF = vf; ps->_bindFF = ff;
            __atomic_store_n(&ps->_bindFnReady, 1, __ATOMIC_RELEASE);
        }
    }
}
- (BOOL)nvmtlBindSamplerPipeline {
    if (!_ps) return NO;
    // _ps is held by this encoder, a sampler variant by _ps->_samplerVariants: no retain per draw
    __unsafe_unretained NVMTLRenderPipelineState *selected=_ps;
    if (_pixMask[0] | _pixMask[1]) {   // = _vertexSamplers.count || _fragmentSamplers.count (stage 1: entries only while a bit is set)
        selected=[self nvmtlSamplerPipeline];
        if (!selected) return NO;
    }
    nvmtl_retain_resource(_cb, selected);
    if (t_pso_dump) { t_pso_dump = 0; nvmtl_pso_dump(selected, (NVMTLFunction *)_ps->_descriptor.fragmentFunction,
                                                     (NVMTLFunction *)selected->_descriptor.fragmentFunction); }
    nvmtl_vk_cmd_bind_pipeline(&_cb->_c,&selected->_p);
    if (!__atomic_load_n(&_ps->_bindFnReady, __ATOMIC_ACQUIRE)) nvmtl_ps_bindfns_slow(_ps);
    // _ps (held by this encoder and its command buffer) keeps both functions alive: no retain per draw. Read as plain
    // pointers: with ARC locals the compiler still retained the fragment function across the vertex call, and that
    // release was the first atomic after the descriptor-ring (write-combined VRAM) writes - ~65 ns a draw (stage 1d)
    void *vfp = (__bridge void *)_ps->_bindVF, *ffp = (__bridge void *)_ps->_bindFF;
    nvmtl_bind_embedded(_cb, NVMTL_SET_VERTEX,   (__bridge NVMTLFunction *)vfp, _vbufAt, _vbufOff);
    nvmtl_bind_embedded(_cb, NVMTL_SET_FRAGMENT, (__bridge NVMTLFunction *)ffp, _fbufAt, _fbufOff);
    nvmtl_vk_cmd_bind_set(&_cb->_c,&selected->_p);
    return YES;
}
- (BOOL)nvmtlSamplePositionError:(NSString *)message {
    _refused = YES;
    nvlog("%s", message.UTF8String);
    [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
        code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey:message}]];
    return NO;
}
- (BOOL)nvmtlBuildPass:(MTLRenderPassDescriptor *)d {
    NVMTLCommandBuffer *cb = _cb;
    memset(&_pass, 0, sizeof _pass); cb->_c.depth_view = NULL;
    MTLSamplePosition sample[8];
    NSUInteger sampleCount = [d getSamplePositions:NULL count:0];
    if (sampleCount && sampleCount <= 8) [d getSamplePositions:sample count:sampleCount];
    if (sampleCount == 1) return [self nvmtlSamplePositionError:@"sample positions: custom single-sample pattern is not a Metal descriptor contract"];
    if (sampleCount > 8) return [self nvmtlSamplePositionError:@"sample positions: more than eight positions; pass refused"];
    if (sampleCount) {
        _pass.sample_pattern.count = (uint32_t)sampleCount;
        for (NSUInteger i = 0; i < sampleCount; ++i)
            _pass.sample_pattern.position[i] = (nvmtl_sample_position){ sample[i].x, sample[i].y };
        if (!nvmtl_sample_pattern_valid(&_pass.sample_pattern)) return [self nvmtlSamplePositionError:@"sample positions: invalid positions; pass refused"];
    }
    _pass.depth_store_options = (uint32_t)d.depthAttachment.storeActionOptions;
    if (d.depthAttachment.storeAction == MTLStoreActionCustomSampleDepthStore)
        _pass.depth_store_options |= MTLStoreActionOptionCustomSamplePositions;
    _pass.stencil_store_options = (uint32_t)d.stencilAttachment.storeActionOptions;
    if (d.stencilAttachment.texture && _pass.stencil_store_options)
        return [self nvmtlSamplePositionError:@"sample positions: stencil store options are illegal"];
    if ((_pass.depth_store_options | _pass.stencil_store_options) & ~MTLStoreActionOptionCustomSamplePositions)
        return [self nvmtlSamplePositionError:@"sample positions: unknown depth/stencil store options"];

    if (!cb->_resources) cb->_resources = [NVMTLResSet new];
    uint32_t load = 0, w = UINT32_MAX, h = UINT32_MAX; BOOL any = NO;
    const uint32_t NL = d.renderTargetArrayLength > 1 ? (uint32_t)d.renderTargetArrayLength : 1u;
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
        MTLRenderPassColorAttachmentDescriptor *ca = d.colorAttachments[i];
        id<MTLTexture> tx = ca.texture, rx = ca.resolveTexture;
        if (!tx) continue;
        if (![(id)tx isKindOfClass:[NVMTLTexture class]]) { nvlog("encoder: apps2 colour %u is a %s, not our texture — pass REFUSED", i, object_getClassName(tx)); return NO; }
        NVMTLTexture *t = (NVMTLTexture *)tx; uint32_t cw = 0, ch = 0;
        uint32_t zslice = [t nvi]->mtl_type == 7 ? (uint32_t)ca.depthPlane : (uint32_t)ca.slice;
        if (ca.depthPlane && [t nvi]->mtl_type != 7) { static int said;
            if (said++ < 4) nvlog("encoder: apps2 colour %u sets depthPlane %lu on a texture of type %u, which is not 3D — depthPlane IGNORED, its slice %lu is used", i, (unsigned long)ca.depthPlane, [t nvi]->mtl_type, (unsigned long)ca.slice); }
        int ta8 = 0; uint32_t tvk = nvmtl_tex_attach_vkfmt(t, &ta8);
        if (nvmtl_vk_cmd_attachment_view_n(&cb->_c, [t nvi], 0, t->_baseLevel + (uint32_t)ca.level, t->_baseSlice + zslice, NL, tvk, ta8, &_pass.colview[i], &cw, &ch)) return NO;
        _pass.col[i] = [t nvi]; _pass.colw[i] = cw; _pass.colh[i] = ch;
        _pass.col_level[i] = t->_baseLevel + (uint32_t)ca.level; _pass.col_layer[i] = t->_baseSlice + zslice;
        if (_pass.col_level[i]) (t->_parent ?: t)->_mipsWritten = YES;
        _pass.colfmt[i] = tvk;
        MTLClearColor cc = ca.clearColor;   // an A8 target keeps Metal's alpha in its red lane, its clear too
        _pass.clear[i][0] = ta8 ? (float)cc.alpha : (float)cc.red; _pass.clear[i][1] = (float)cc.green; _pass.clear[i][2] = (float)cc.blue; _pass.clear[i][3] = (float)cc.alpha;
        if (ta8 == 2) { _pass.clear[i][0] = (float)cc.alpha; _pass.clear[i][1] = (float)cc.red; _pass.clear[i][2] = (float)cc.green; _pass.clear[i][3] = (float)cc.blue; }   // ARGB8 stored as RGBA8
        if (nvmtl_load_keeps(ca.loadAction)) load |= NVMTL_LOAD_COLOUR(i);
        if (cw < w) w = cw;
        if (ch < h) h = ch;
        any = YES;
        if (i) {
            [cb->_resources addObject:t];
            NVMTLTexture *sr = t->_parent ? t->_parent : t;
            if (sr->_surf) {
                if (ca.loadAction == MTLLoadActionClear) nvmtl_surface_overwrite_recorded(cb, sr);
                else nvmtl_surface_in_for_cb(cb, sr);
                nvmtl_surf_dirty(cb, sr); }
        }
        MTLStoreAction sa = ca.storeAction;
        if ((sa == MTLStoreActionMultisampleResolve || sa == MTLStoreActionStoreAndMultisampleResolve || sa == MTLStoreActionUnknown) && rx) {
            if (![(id)rx isKindOfClass:[NVMTLTexture class]]) { nvlog("encoder: apps2 resolve %u is a %s, not our texture — pass REFUSED", i, object_getClassName(rx)); return NO; }
            NVMTLTexture *r = (NVMTLTexture *)rx; uint32_t rw = 0, rh = 0;
            int ra8 = 0; uint32_t rvk = nvmtl_tex_attach_vkfmt(r, &ra8);
            if (nvmtl_vk_cmd_attachment_view_n(&cb->_c, [r nvi], 0, r->_baseLevel + (uint32_t)ca.resolveLevel, r->_baseSlice + (uint32_t)ca.resolveSlice, NL, rvk, ra8, &_pass.resview[i], &rw, &rh)) return NO;
            _pass.res[i] = [r nvi]; _pass.resw[i] = rw; _pass.resh[i] = rh;
            _pass.resfmt[i] = rvk;
            if (i) { [cb->_resources addObject:r]; NVMTLTexture *rs = r->_parent ? r->_parent : r; if (rs->_surf) nvmtl_surf_dirty(cb, rs); }
        }
    }
    MTLRenderPassDepthAttachmentDescriptor *da = d.depthAttachment; MTLRenderPassStencilAttachmentDescriptor *sa = d.stencilAttachment;
    id<MTLTexture> dtx = da.texture ? da.texture : sa.texture;
    if (dtx) {
        if (![(id)dtx isKindOfClass:[NVMTLTexture class]]) { nvlog("encoder: apps2 depth/stencil is a %s, not our texture — pass REFUSED", object_getClassName(dtx)); return NO; }
        if (da.texture && sa.texture && da.texture != sa.texture) { static int said; if (!said++) nvlog("encoder: apps2 separate depth and stencil textures — the stencil texture is NOT attached (one depth/stencil attachment)"); }
        MTLRenderPassAttachmentDescriptor *which = da.texture ? (MTLRenderPassAttachmentDescriptor *)da : (MTLRenderPassAttachmentDescriptor *)sa;
        NVMTLTexture *dt = (NVMTLTexture *)dtx; uint32_t dw = 0, dh = 0;
        if (nvmtl_vk_cmd_attachment_view_n(&cb->_c, [dt nvi], 1, dt->_baseLevel + (uint32_t)which.level, dt->_baseSlice + (uint32_t)which.slice, NL, 0, 0, &_pass.dsview, &dw, &dh)) return NO;
        {
            MTLStoreAction dsa = da.storeAction, ssa = sa.storeAction;
            BOOL dr = da.texture && da.resolveTexture && (dsa == MTLStoreActionMultisampleResolve || dsa == MTLStoreActionStoreAndMultisampleResolve || dsa == MTLStoreActionUnknown);
            BOOL sr = sa.texture && sa.resolveTexture && (ssa == MTLStoreActionMultisampleResolve || ssa == MTLStoreActionStoreAndMultisampleResolve || ssa == MTLStoreActionUnknown);
            id<MTLTexture> drx = dr ? da.resolveTexture : sr ? sa.resolveTexture : nil;
            if (drx && [(id)drx isKindOfClass:[NVMTLTexture class]]) {
                NVMTLTexture *r = (NVMTLTexture *)drx; uint32_t rw = 0, rh = 0;
                MTLRenderPassAttachmentDescriptor *rw_ = dr ? (MTLRenderPassAttachmentDescriptor *)da : (MTLRenderPassAttachmentDescriptor *)sa;
                if (nvmtl_vk_cmd_attachment_view_n(&cb->_c, [r nvi], 1, r->_baseLevel + (uint32_t)rw_.resolveLevel, r->_baseSlice + (uint32_t)rw_.resolveSlice, NL, 0, 0, &_pass.dresview, &rw, &rh)) return NO;
                _pass.dres = [r nvi];
                _pass.dresmode = dr ? 1u + (uint32_t)da.depthResolveFilter : 0u;
                _pass.sresmode = sr ? 1u + (uint32_t)sa.stencilResolveFilter : 0u;
                [cb->_resources addObject:r];
            } else if (drx) { nvlog("encoder: par1 depth/stencil resolve texture is a %s, not our texture — pass REFUSED", object_getClassName(drx)); return NO; }
        }
        _pass.ds = [dt nvi];
        _pass.ds_level = dt->_baseLevel + (uint32_t)which.level;
        _pass.ds_layer = dt->_baseSlice + (uint32_t)which.slice;
        if (sa.texture) [cb->_resources addObject:sa.texture];
        if (dw < w) w = dw;
        if (dh < h) h = dh;
        any = YES;
        if (da.texture && nvmtl_load_keeps(da.loadAction)) load |= NVMTL_LOAD_DEPTH;
        if (sa.texture && nvmtl_load_keeps(sa.loadAction)) load |= NVMTL_LOAD_STENCIL;
    }
    _pass.clear_depth = (float)da.clearDepth; _pass.clear_stencil = sa.clearStencil;
    if (!any) {
        // A pass with no attachments: the fragment shader writes through images or buffers only, sized by the descriptor's
        // renderTargetWidth/Height (Blender Eevee's shadow pages, 8192x8192 with image atomics). Vulkan draws it as a render
        // pass with no attachments and a framebuffer of that size. Refusing it crashed Blender on its first shadowed frame
        // (#152). Metal requires the size for such a pass, so a pass without one is still refused.
        if (!d.renderTargetWidth || !d.renderTargetHeight) {
            nvlog("encoder: apps2 a render pass with no attachments and no renderTargetWidth/Height — pass REFUSED"); return NO; }
        w = (uint32_t)d.renderTargetWidth; h = (uint32_t)d.renderTargetHeight;
        static int said; if (said++ < 4) nvlog("encoder: render pass with no attachments, %ux%u (writes through images/buffers only)", w, h);
    }
    if (d.renderTargetWidth && d.renderTargetWidth < w) w = (uint32_t)d.renderTargetWidth;
    if (d.renderTargetHeight && d.renderTargetHeight < h) h = (uint32_t)d.renderTargetHeight;
    _pass.load = _load = load; _pass.w = w; _pass.h = h; _pass.layers = NL;
    { static uint32_t seen[16]; static unsigned nseen;
      const uint32_t shape = (uint32_t)d.colorAttachments[0].loadAction
                           | ((uint32_t)(da.texture ? da.loadAction + 1 : 0) << 4)
                           | ((uint32_t)(sa.texture ? sa.loadAction + 1 : 0) << 8)
                           | (load << 12);
      unsigned k = 0; for (; k < nseen; k++) if (seen[k] == shape) break;
      if (k == nseen && nseen < 16) {
        seen[nseen++] = shape;
        nvlog("encoder: dontcare pass shape — colour0 load action %lu, depth %s action %lu, stencil %s action %lu, load bits 0x%x (DontCare %s)",
              (unsigned long)d.colorAttachments[0].loadAction,
              da.texture ? "present" : "absent", (unsigned long)da.loadAction,
              sa.texture ? "present" : "absent", (unsigned long)sa.loadAction,
              load, nvmtl_dontcare_clears() ? "CLEARS (old behaviour)" : "keeps, as on Apple");
      } }
    cb->_c.depth_view = _pass.dsview;
    return YES;
}
- (BOOL)nvmtlBeginPassLoad:(uint32_t)load {
    if (_refused || !_ps) return NO;
    uint32_t keep = _pass.load; _pass.load = load;
    int rc = nvmtl_vk_cmd_begin_pass(&_cb->_c, &_pass, &_ps->_p);
    _pass.load = keep;
    return rc == 0;
}
- (void)nvmtlApplyDepthStencil {
    if (!_begun || !_ps || !_ps->_p.has_depth) return;
    nvmtl_vk_cmd_set_depth(&_cb->_c, _ds ? 1 : 0, _ds ? _ds->_write : 0, _ds ? _ds->_compare : 7);
    // Every encoder starts with the depth bounds test off, as on Apple; the pipeline holds it as dynamic state, which
    // must be set before the first draw, so it is written here at pass begin even when the app never asks for it.
    if (nvmtl_vk_cmd_set_depth_bounds(&_cb->_c, _dbOn, _dbLo, _dbHi) && _dbOn) {
        static int said; if (!said++) nvlog("setDepthTestMinBound:maxBound: REFUSED - this device has no depthBounds feature, the test is not applied"); }
    if (!_ps->_p.has_stencil) return;
    static const uint32_t off[6] = { 7, 0, 0, 0, 0xff, 0xff };
    nvmtl_vk_cmd_set_stencil(&_cb->_c, _ds && _ds->_stencil, _ds ? _ds->_sf : off, _ds ? _ds->_sb : off);
    if (!_hasRef) { nvmtl_vk_cmd_set_stencil_ref(&_cb->_c, 0, 0); _hasRef = YES; }
}
// Metal draws through the ATTACHMENT's format, not the pipeline's: a pipeline declared BGRA8 that renders into an RGBA8
// texture (or the reverse) writes the colour the shader returned into the texture's own channel order and reports no error
// (measured on Apple M1 Pro: a BGRA8 pipeline into an RGBA8 texture stores 255,128,64,255 for float4(1,.5,.25,1), exactly as
// an RGBA8 pipeline does). Vulkan builds a pass in the pipeline's format, so the pass was refused and the draws dropped.
// Cyberpunk with frame generation on asks for an RGBA8 back buffer while its pipelines are BGRA8. The pipeline is rebuilt
// from its own descriptor with the channel-swapped format the attachment really has, once per pipeline.
static uint32_t nvmtl_swizzle_twin_vk(uint32_t f) { return nvmtl_format_twin_vk(f); }
static MTLPixelFormat nvmtl_swizzle_twin_mtl(MTLPixelFormat f) { return (MTLPixelFormat)nvmtl_format_twin_mtl((uint32_t)f); }
- (NVMTLRenderPipelineState *)nvmtlColourTwinPipeline:(NVMTLRenderPipelineState *)p {
    // NVMTL_FORMAT_TWIN=0 restores the old refusal of a pass whose attachment format is not the pipeline's (an A/B switch).
    static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_FORMAT_TWIN"); on = !(e && e[0] == '0'); if (!on) nvlog("pipelines are NOT rebuilt for an attachment's channel order (NVMTL_FORMAT_TWIN=0)"); }
    if (!on || !p || !p->_descriptor) return p;
    const uint32_t n = p->_p.ncol ? p->_p.ncol : (p->_p.cfmt ? 1u : 0u);
    uint32_t swap = 0;
    for (uint32_t i = 0; i < n && i < NVMTL_NCOL; i++) {
        const uint32_t pf = p->_p.ncol ? p->_p.cfmts[i] : p->_p.cfmt;
        if (!pf || !_pass.col[i]) continue;
        const uint32_t af = _pass.colfmt[i] ? _pass.colfmt[i] : _pass.col[i]->fmt;
        if (af != pf && nvmtl_swizzle_twin_vk(pf) == af) swap |= 1u << i;
    }
    if (!swap) return p;
    NSString *key = [NSString stringWithFormat:@"colour-twin:%x", swap];
    @synchronized (p) {
        if (!p->_samplerVariants) p->_samplerVariants = [NSMutableDictionary new];
        id got = p->_samplerVariants[key];
        if (got == [NSNull null]) return p;
        if (got) return got;
        MTLRenderPipelineDescriptor *desc = [p->_descriptor copy];
        NVMTLFunction *vf = p->_linkedVert ?: (id)p->_descriptor.vertexFunction, *ff = p->_linkedFrag ?: (id)p->_descriptor.fragmentFunction;
        desc.vertexFunction = vf; desc.fragmentFunction = ff;
        for (uint32_t i = 0; i < n && i < NVMTL_NCOL; i++) if (swap & (1u << i)) {
            const MTLPixelFormat t = nvmtl_swizzle_twin_mtl(desc.colorAttachments[i].pixelFormat);
            if (t == MTLPixelFormatInvalid) { p->_samplerVariants[key] = [NSNull null]; return p; }
            desc.colorAttachments[i].pixelFormat = t;
        }
        NSError *error = nil;
        NVMTLRenderPipelineState *v = (NVMTLRenderPipelineState *)[(id<MTLDevice>)gNVMTLMainDevice newRenderPipelineStateWithDescriptor:desc error:&error];
        if (!v) { nvlog("pipeline %s / %s: no variant for the attachment's channel order (%s)", p->_vname.UTF8String ?: "?", p->_fname.UTF8String ?: "?", error.description.UTF8String ?: "?");
                  p->_samplerVariants[key] = [NSNull null]; return p; }
        p->_samplerVariants[key] = v;
        static _Atomic unsigned long made; const unsigned long k = atomic_fetch_add(&made, 1) + 1;
        if (!(k & (k - 1))) nvlog("pipeline %s / %s rebuilt for the attachment's channel order (colour mask 0x%x) [%lu made]", p->_vname.UTF8String ?: "?", p->_fname.UTF8String ?: "?", swap, k);
        return v;
    }
}
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)ps {
    ps = (id<MTLRenderPipelineState>)[self nvmtlColourTwinPipeline:(NVMTLRenderPipelineState *)ps];
    // The same pipeline again inside a begun pass changes nothing: Apple's OpenGL renderer sets it before every draw, so
    // GL games paid a full bind per draw (the GL-on-Metal per-draw overhead). Not inside an open pass-time pool.
    // NVMTL_PSO_REBIND=1 takes the full path every time (an A/B switch).
    if (ps && (NVMTLRenderPipelineState *)ps == _ps && _begun && !(_cb->_c.pt_open && _cb->_c.pt_pool)) {
        static int rebind = -1; if (rebind < 0) { const char *e = getenv("NVMTL_PSO_REBIND"); rebind = e && e[0] == '1'; }
        if (!rebind) return;
    }
    // NVMTL_DUMP_PSO=<text>: the first encoder that sets a pipeline whose label contains it has the fragment buffers bound
    // at its end dumped, as NVMTL_DUMP_GROUP does, for apps that label pipelines but push no debug groups (diagnostic)
    // Only asked for when the switch is set: this runs on every pipeline bind, and an unconditional `ps.label` was a message
    // send plus a string retain/release per draw call in profiles of a game that never sets NVMTL_DUMP_PSO.
    { static const char *want; static int once; static _Atomic int armed; if (!once) { once = 1; want = getenv("NVMTL_DUMP_PSO"); }
      NSString *lbl = want ? ps.label : nil;
      if (want && !strcmp(want, "?") && lbl) { static NSMutableSet *seen; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
          pthread_mutex_lock(&mu); if (!seen) seen = [NSMutableSet new]; BOOL n = ![seen containsObject:lbl]; if (n) [seen addObject:lbl];
          pthread_mutex_unlock(&mu); if (n) nvlog("NVMTL_DUMP_PSO: pipeline label %s", lbl.UTF8String); }
      if (want && *want && lbl && strstr(lbl.UTF8String ?: "", want) && !atomic_exchange(&armed, 1)) { _dumpPso = YES; _dumpName = [lbl copy]; } }
    { NVMTLRenderPipelineState *p_ = (NVMTLRenderPipelineState *)ps;
      if (p_ && nvmtl_trace_on()) {
          extern int nvmtl_vk_pipeline_stats(void *pipe, char *out, size_t n);
          uint32_t hv = 0, hf = 0;
          for (int s = 0; s < 2; s++) {
              id f = s ? p_->_descriptor.fragmentFunction : p_->_descriptor.vertexFunction;
              if (![f isKindOfClass:[NVMTLFunction class]]) continue;
              NSData *d = ((NVMTLFunction *)f)->_spirv; if (!d.length) continue;
              uint32_t h = 2166136261u; const uint8_t *b = d.bytes; for (NSUInteger i = 0; i < d.length; i++) { h ^= b[i]; h *= 16777619u; }
              if (s) hf = h; else hv = h;
              static NSMutableSet *saved; static os_unfair_lock sl = OS_UNFAIR_LOCK_INIT; BOOL fresh = NO;
              os_unfair_lock_lock(&sl); if (!saved) saved = [NSMutableSet new]; if (![saved containsObject:@(h)]) { [saved addObject:@(h)]; fresh = YES; } os_unfair_lock_unlock(&sl);
              if (fresh) { NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/nvmtl/pstat"];
                  [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
                  NSString *pth = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%08x.%s.spv", h, s ? "frag" : "vert"]];
                  if (![[NSFileManager defaultManager] fileExistsAtPath:pth]) [d writeToFile:pth atomically:YES]; }
          }
          char st[600]; nvmtl_vk_pipeline_stats(p_->_p.pipe, st, sizeof st);
          nvtrace("PSO %p %s / %s vs %08x fs %08x | %s", (__bridge void *)ps, p_->_vname.UTF8String ?: "?", p_->_fname.UTF8String ?: "?", hv, hf, st);
      } }
    NVMTLRenderPipelineState *p = (NVMTLRenderPipelineState *)ps;
    if (!p) { nvlog("encoder: nil render pipeline refused"); return; }
    if (_begun && !nvmtl_vk_pipeline_compatible(&_passSig, &p->_p)) {
        static int refuse = -1;   // NVMTL_NO_PASS_TRANSITION=1: the old refusal instead of the transition below (a split switch, 84e655e)
        if (refuse < 0) { const char *e = getenv("NVMTL_NO_PASS_TRANSITION"); refuse = e && e[0] == '1';
            if (refuse) nvlog("encoder: an incompatible render pipeline is REFUSED, not transitioned (NVMTL_NO_PASS_TRANSITION=1)"); }
        if (refuse) { nvlog("encoder: incompatible render pipeline refused before changing draw state"); return; }
        // Metal can omit unused attachments in a later pipeline. Vulkan requires a new compatible pass: end this one,
        // keep every attachment an earlier segment used, preserve the encoder's dynamic state and begin again below.
        // Refusing the pipeline instead dropped every draw until the next compatible one: Cyberpunk's parallel render
        // encoders replay thousands of such binds per frame (4,096+ refusals logged per run) and the frame lost them.
        uint32_t keep = 0;
        for (uint32_t i = 0; i < _passSig.ncol; i++)
            if (_passSig.cfmts[i]) keep |= NVMTL_LOAD_COLOUR(i);
        if (_passSig.has_depth) keep |= NVMTL_LOAD_DEPTH;
        if (_ps && _ps->_p.has_stencil) keep |= NVMTL_LOAD_STENCIL;
        BOOL visibility = _visActive || _visPending;
        [self nvmtlCloseVisibility];
        nvmtl_vk_cmd_end_render(&_cb->_c);
        _begun = NO; _pass.load |= keep; _load |= keep; _visPending = visibility;
        static _Atomic unsigned long long transitions; const unsigned long long k = atomic_fetch_add(&transitions, 1) + 1;
        if (!(k & (k - 1))) nvlog("encoder: pipeline attachment transition #%llu preserves load mask 0x%x (pass %u colour, depth %u, %u samples -> pipeline %s / %s)",
                                 k, keep, _passSig.ncol, _passSig.has_depth, _passSig.samples, p->_vname.UTF8String ?: "?", p->_fname.UTF8String ?: "?");
    }
    _ps = p;
    nvmtl_retain_resource(_cb, p);
    if (!_begun) {
        if (_refused) return;
        if (nvmtl_vk_cmd_begin_pass(&_cb->_c, &_pass, &p->_p)) {
            nvlog("encoder: begin render FAILED — nothing is recorded for this pipeline");
            if (_pass.sample_pattern.count) [self nvmtlSamplePositionError:@"sample positions: cannot begin requested render pass"];
            return; }
        _begun = YES; [self nvmtlBeginVisibility]; _passFmt = p->_p.cfmt; _passDepth = p->_p.has_depth; _passSamples = p->_p.samples; nvmtl_vk_pipeline_sig(&p->_p, &_passSig);
        if (_resolve) { NVMTLTexture *rs = _resolve->_parent ? _resolve->_parent : _resolve; if (rs->_surf) nvmtl_surf_dirty(_cb, rs); }
        nvmtl_vk_cmd_set_cull(&_cb->_c, _cull);
        if (_hasVP) nvmtl_vk_cmd_set_viewport(&_cb->_c, _vp[0], _vp[1], _vp[2], _vp[3], _vp[4], _vp[5]);
        if (_hasSC) nvmtl_vk_cmd_set_scissor(&_cb->_c, (int32_t)_sc[0], (int32_t)_sc[1], _sc[2], _sc[3]);
        [self nvmtlApplyDepthStencil];
    } else if (!nvmtl_vk_pipeline_compatible(&_passSig, &p->_p)) {
        static int said; if (said++ < 8) nvlog("G12 setRenderPipelineState: pipeline (fmt %u depth %d) does NOT match the pass in progress "
            "(fmt %u depth %d) — NOT bound, the earlier pipeline keeps drawing", p->_p.cfmt, p->_p.has_depth, _passFmt, _passDepth);
    } else {
        nvmtl_vk_cmd_bind_pipeline(&_cb->_c, &p->_p);
        static int told; if (!told++) nvlog("G12 a second pipeline was bound inside one encoder (first time in this process)");
    }
    extern int nvmtl_gpt_on_cb(void *cb);
    const int gptOn = nvmtl_gpt_on_cb(_cb->_c.cb);
    if ((_cb->_c.pt_open && _cb->_c.pt_pool) || gptOn) {
        static const char kPtHash = 0; NSNumber *hn = objc_getAssociatedObject(p, &kPtHash);
        if (!hn) {
            uint32_t hv = 0, hf = 0;
            for (int s = 0; s < 2; s++) {
                id f = s ? p->_descriptor.fragmentFunction : p->_descriptor.vertexFunction;
                if (![f isKindOfClass:[NVMTLFunction class]]) continue;
                NSData *d = ((NVMTLFunction *)f)->_spirv; if (!d.length) continue;
                uint32_t h = 2166136261u; const uint8_t *b = d.bytes; for (NSUInteger i = 0; i < d.length; i++) { h ^= b[i]; h *= 16777619u; }
                if (s) hf = h; else hv = h;
                NSString *dir = [NSHomeDirectory() stringByAppendingPathComponent:@"Library/Caches/nvmtl/pstat"];
                NSString *pth = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"%08x.%s.spv", h, s ? "frag" : "vert"]];
                if (![[NSFileManager defaultManager] fileExistsAtPath:pth]) {
                    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
                    [d writeToFile:pth atomically:YES]; }
            }
            hn = @(((uint64_t)hv << 32) | hf);
            objc_setAssociatedObject(p, &kPtHash, hn, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
            extern int nvmtl_vk_pipeline_stats(void *pipe, char *out, size_t n);
            char st[600]; nvmtl_vk_pipeline_stats(p->_p.pipe, st, sizeof st);
            nvlog("ptpso: vs %08x fs %08x | %s", hv, hf, st);
        }
        extern void nvmtl_vk_pt_note(nvk_cmdbuf *c, uint32_t fs);
        nvmtl_vk_pt_note(&_cb->_c, (uint32_t)(hn.unsignedLongLongValue & 0xffffffffu));
        extern void nvmtl_gpt_fs(void *cb, uint32_t fs);
        if (gptOn) nvmtl_gpt_fs(_cb->_c.cb, (uint32_t)(hn.unsignedLongLongValue & 0xffffffffu));
    }
}
- (void)setVertexBuffer:(__unsafe_unretained id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLBuffer *nb = (NVMTLBuffer *)b;   // the caller holds it; the command buffer's set from here on
    if (!nb) { nvlog("setVertexBuffer: nil at index %lu", (unsigned long)i); return; }
    nvmtl_retain_resource(_cb, nb);
    if (i < NVMTL_NBUFAT) { _vbufAt[i] = nb; _vbufOff[i] = off; }
    nvtrace_floats("VB", (unsigned long)i, (unsigned long)off, nb->_b.map, 24);
    if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, &nb->_b, off)) nvlog("setVertexBuffer: bind FAILED");
}
- (void)setVertexAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)i { [self setVertexBuffer:(id<MTLBuffer>)as offset:0 atIndex:i];
    NVMTLAccelerationStructure *a = (NVMTLAccelerationStructure *)as;
    if (a && [(id)a isKindOfClass:[NVMTLAccelerationStructure class]] && a->_uid.buf && _cb) nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 31, &a->_uid, 0); }
- (void)setFragmentAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)i { [self setFragmentBuffer:(id<MTLBuffer>)as offset:0 atIndex:i];
    NVMTLAccelerationStructure *a = (NVMTLAccelerationStructure *)as;
    if (a && [(id)a isKindOfClass:[NVMTLAccelerationStructure class]] && a->_uid.buf && _cb) nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_FRAGMENT, 31, &a->_uid, 0); }
- (void)setFragmentBuffer:(__unsafe_unretained id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLBuffer *nb = (NVMTLBuffer *)b;   // the caller holds it; the command buffer's set from here on
    if (!nb) { nvlog("setFragmentBuffer: nil at index %lu", (unsigned long)i); return; }
    nvmtl_retain_resource(_cb, nb);
    if (i < NVMTL_NBUFAT) { _fbufAt[i] = nb; _fbufOff[i] = off; }
    nvtrace_floats("FB", (unsigned long)i, (unsigned long)off, nb->_b.map, 16);
    if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, &nb->_b, off)) nvlog("setFragmentBuffer: bind FAILED");
}
static BOOL nvmtl_bytes_ring_at(__unsafe_unretained NVMTLCommandBuffer *cb, NSUInteger len, size_t *off, __unsafe_unretained NVMTLBuffer **out);
- (void)nvmtlBindBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i set:(uint32_t)set {
    size_t roff = 0;
    // the ring chunk is held by the command buffer (_scratch): no retain per call; a buffer of its own is held by `own`
    __unsafe_unretained NVMTLBuffer *nb = nil; NVMTLBuffer *own = nil;
    const BOOL ring = nvmtl_bytes_ring_at(_cb, len, &roff, &nb);
    if (!ring) {
        nb = own = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(len < 16 ? 16 : len, 1, &nb->_b)) { nvlog("setBytes: buffer create FAILED"); return; }
    }
    if (nb->_b.map) memcpy((uint8_t *)nb->_b.map + roff, bytes, len);
    if (nvmtl_vk_bind_buffer(&_cb->_c, set, (uint32_t)i, &nb->_b, roff)) nvlog("setBytes: bind FAILED");
    nvtrace_floats(set == NVMTL_SET_VERTEX ? "VBYTES" : "FBYTES", (unsigned long)i, 0, len >= 4 ? bytes : NULL, (int)(len / 4 > 24 ? 24 : len / 4));
    if (!ring) {
        if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
        [_cb->_scratch addObject:own];
    }
    // the same ring chunk as last time at this index (the usual case): no store, so no retain/release pair
    if (i < NVMTL_NBUFAT && set == NVMTL_SET_VERTEX)        { if (_vbufAt[i] != nb) _vbufAt[i] = nb; _vbufOff[i] = roff; }
    else if (i < NVMTL_NBUFAT && set == NVMTL_SET_FRAGMENT) { if (_fbufAt[i] != nb) _fbufAt[i] = nb; _fbufOff[i] = roff; }
}
- (void)setVertexBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self nvmtlBindBytes:b length:l atIndex:i set:NVMTL_SET_VERTEX]; }
- (void)setFragmentBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self nvmtlBindBytes:b length:l atIndex:i set:NVMTL_SET_FRAGMENT]; }
- (void)setVertexTexture:(__unsafe_unretained id<MTLTexture>)t atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLTexture *nt = (NVMTLTexture *)t;   // the caller holds it; the command buffer's set from here on
    if (!nt) { nvlog("setVertexTexture: nil at index %lu", (unsigned long)i); return; }
    nvmtl_retain_resource(_cb, nt);
    nvmtl_surface_in_for_cb(_cb, nt); nvmtl_ws_sampled(nt);
    if (nt->_tbView) nvmtl_vk_bind_texel_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nt->_tbView, nt->_tbStorage);
    else {
    if (nvmtl_vk_bind_texture_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nvmtl_tex_sampleview(nt))) nvlog("setVertexTexture: bind FAILED");
    nvmtl_vk_bind_storage_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nt->_stex ? nvmtl_tex_view(nt) : NULL);
    }
}
- (void)setFragmentSamplerState:(__unsafe_unretained id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLSamplerState *ns = (NVMTLSamplerState *)s;   // held by the caller, then by the command buffer
    if (i >= 16 || nvmtl_vk_bind_sampler(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, ns ? &ns->_s : NULL)) {
        nvlog("setFragmentSamplerState: invalid index or bind failure"); return;
    }
    nvmtl_note_app_sampler(_cb, NVMTL_SET_FRAGMENT, i, ns);
    // only a pixel-coordinate sampler (or one replacing it) changes the specialization key: the rest skip the dictionary,
    // whose NSString key cost a GL-like draw ~25% of its CPU time (mtl_bind gl, 2026-10-29)
    NSDictionary *pix = ns ? ns->_pixelState : nil;
    if (pix || ((_pixMask[1] >> i) & 1u)) {
        if (!_fragmentSamplers) _fragmentSamplers=[NSMutableDictionary new];
        NSString *key=[@(i) stringValue];
        if (nvmtl_pixel_sampler_update(_fragmentSamplers,key,pix)) _pixelSamplerKey=nil;
        if (pix) _pixMask[1] |= (uint16_t)(1u << i); else _pixMask[1] &= (uint16_t)~(1u << i);
    }
    nvtrace("FRAGMENT SAMPLER[%lu] pixel=%d bound=%d | min %u mag %u mip %u lod %g..%g aniso %u full %d",(unsigned long)i,ns && ns->_pixelState!=nil,ns!=nil,
            ns ? ns->_s.desc.min_filter : 0u, ns ? ns->_s.desc.mag_filter : 0u, ns ? ns->_s.desc.mip_filter : 0u, ns ? ns->_s.desc.lod_min : 0.f,
            ns ? ns->_s.desc.lod_max : 0.f, ns ? ns->_s.desc.max_anisotropy : 0u, ns ? ns->_s.full_state : -1);
    if (ns) nvmtl_retain_resource(_cb, ns);
}
- (void)setVertexSamplerState:(__unsafe_unretained id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLSamplerState *ns = (NVMTLSamplerState *)s;   // held by the caller, then by the command buffer
    if (i >= 16 || nvmtl_vk_bind_sampler(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, ns ? &ns->_s : NULL)) {
        nvlog("setVertexSamplerState: invalid index or bind failure"); return;
    }
    nvmtl_note_app_sampler(_cb, NVMTL_SET_VERTEX, i, ns);
    NSDictionary *pix = ns ? ns->_pixelState : nil;
    if (pix || ((_pixMask[0] >> i) & 1u)) {
        if (!_vertexSamplers) _vertexSamplers=[NSMutableDictionary new];
        NSString *key=[@(i) stringValue];
        if (nvmtl_pixel_sampler_update(_vertexSamplers,key,pix)) _pixelSamplerKey=nil;
        if (pix) _pixMask[0] |= (uint16_t)(1u << i); else _pixMask[0] &= (uint16_t)~(1u << i);
    }
    nvtrace("VERTEX SAMPLER[%lu] pixel=%d bound=%d",(unsigned long)i,ns && ns->_pixelState!=nil,ns!=nil);
    if (ns) nvmtl_retain_resource(_cb, ns);
}
- (void)setFragmentTexture:(__unsafe_unretained id<MTLTexture>)t atIndex:(NSUInteger)i {
    __unsafe_unretained NVMTLTexture *nt = (NVMTLTexture *)t;   // the caller holds it; the command buffer's set from here on
    if (!nt) { nvlog("setFragmentTexture: nil at index %lu", (unsigned long)i); return; }
    nvmtl_retain_resource(_cb, nt);
    if (__builtin_expect(nvmtl_dump_request_on(), 0)) {   // perf2: the dump hooks only when /tmp/nvmtl-dump-request is up
        nvmtl_surface_cpu_dump(nt->_parent ? nt->_parent : nt);
        nvmtl_mip_dump(nt);
        if (i == 3 && !nt->_parent && !nt->_surf && nt->_i.mips > 1 && nt->_i.w == 128 && nt->_i.h == 128) t_pso_dump = 1;
    }
    nvmtl_surface_in_for_cb(_cb, nt); nvmtl_ws_sampled(nt);
    if (__builtin_expect(nvmtl_trace_on(), 0)) { __unsafe_unretained NVMTLTexture *rt = nt->_parent ? nt->_parent : nt; nvtrace("FTEX[%lu] %ux%u surf %u view %d | %p fmt %lu levels %lu (image %u, base %u) mipsWritten %d buf %d", (unsigned long)i, [nt nvi]->w, [nt nvi]->h,
        rt->_surf ? (unsigned)IOSurfaceGetID(rt->_surf) : 0u, nt->_parent != nil, (__bridge void *)nt, (unsigned long)nt->_fmt, (unsigned long)nt.mipmapLevelCount,
        [nt nvi]->mips, nt->_baseLevel, (int)rt->_mipsWritten, nt->_backBuf != nil || nt->_tbView != NULL); }
    if (nt->_tbView) nvmtl_vk_bind_texel_view(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, nt->_tbView, nt->_tbStorage);
    else {
    if (nvmtl_vk_bind_texture_view(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, nvmtl_tex_sampleview(nt))) nvlog("setFragmentTexture: bind FAILED");
    nvmtl_vk_bind_storage_view(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, nt->_stex ? nvmtl_tex_view(nt) : NULL);
    if (!nt->_stex && (nvmtl_tex_usage(nt) & MTLTextureUsageShaderWrite)) {
        // a fragment shader writing a texture that has no storage view writes the dummy: say which texture, once per shape
        // OpenGL rebinds every texture before each draw, and its textures carry ShaderWrite: this runs thousands of times
        // a frame, so the shape is a packed integer in a small table, with no string or object made per bind.
        static uint64_t seen[64]; static int nseen; static os_unfair_lock sl = OS_UNFAIR_LOCK_INIT;
        const uint64_t k = ((uint64_t)nt.pixelFormat << 40) ^ ((uint64_t)nt.usage << 8) ^ (uint64_t)nt.textureType ^ 0x8000000000000000ull;
        BOOL fresh = YES;
        os_unfair_lock_lock(&sl);
        for (int q = 0; q < nseen; q++) if (seen[q] == k) { fresh = NO; break; }
        if (fresh && nseen < 64) seen[nseen++] = k; else if (fresh) fresh = NO;   // a full table says nothing more
        os_unfair_lock_unlock(&sl);
        if (fresh) nvlog("setFragmentTexture: texture %lu is ShaderWrite but has no storage view (pixelFormat %lu, usage 0x%lx, type %lu, image storage %u) - a fragment write to it goes to the dummy",
                         (unsigned long)i, (unsigned long)nt.pixelFormat, (unsigned long)nt.usage, (unsigned long)nt.textureType, [nt nvi]->storage);
    }
    }
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)start vertexCount:(NSUInteger)count
         instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)baseInstance {
    if (!_begun) { nvlog("encoder: draw with no pipeline set"); return; }
    if (![self nvmtlBindSamplerPipeline]) return;
    nvtrace("DRAW type %u | vs %s | fs %s | blend %s", (unsigned)type, _ps ? _ps->_vname.UTF8String : "-", _ps ? _ps->_fname.UTF8String : "-", _ps ? _ps->_blend.UTF8String : "-");
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_); nvtrace("  BUFS%s", bufs_); }
    if (nvmtl_vk_cmd_set_topology(&_cb->_c, (unsigned)type)) return;
    if ([self nvmtlVertexInputs]) return;
    nvtrace("  direct start %lu count %lu instances %lu", (unsigned long)start, (unsigned long)count, (unsigned long)instances);
    nvmtl_vk_cmd_draw(&_cb->_c, (uint32_t)start, (uint32_t)count, (uint32_t)instances, (uint32_t)baseInstance);
}
- (int)nvmtlVertexInputs {
    if (_ps && _ps->_p.tess) { static int said; if (said++ < 8) nvlog("tess: a primitive draw on a tessellation pipeline (%s) — NOT drawn "
        "(Metal requires drawPatches:)", _ps->_vname.UTF8String ?: "?"); return -1; }
    if (!_ps || !_ps->_p.vin_mask) return 0;
    nvtrace("  VIN%s", _ps->_vdesc.UTF8String ?: " ?");
    int r = nvmtl_vk_cmd_vertex_inputs(&_cb->_c, &_ps->_p, _vstride, _vstrideSet);
    if (r) nvtrace("  NOT DRAWN: a vertex buffer the descriptor names is not bound, or its dynamic stride is unset or too large");
    return r;
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c instanceCount:(NSUInteger)i {
    [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:i baseInstance:0];
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c {
    [self drawPrimitives:t vertexStart:s vertexCount:c instanceCount:1 baseInstance:0];
}
- (void)setTessellationFactorBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset instanceStride:(NSUInteger)instanceStride {
    NVMTLBuffer *nb = (NVMTLBuffer *)buffer;
    if (nb && ![(id)nb isKindOfClass:[NVMTLBuffer class]]) { nvlog("setTessellationFactorBuffer: not our buffer (%s) — ignored", object_getClassName(buffer)); nb = nil; }
    if (nb) nvmtl_retain_resource(_cb, nb);
    _tfb = nb; _tfbOff = offset; _tfbStride = instanceStride;
}
- (void)setTessellationFactorScale:(float)scale { _tfScale = scale; _tfScaleSet = YES; }
- (BOOL)nvmtlTessAddress:(id<MTLBuffer>)buf offset:(NSUInteger)off align:(uint64_t)al what:(const char *)what out:(uint64_t *)out {
    *out = 0; if (!buf) return YES;
    if (![(id)buf isKindOfClass:[NVMTLBuffer class]]) { nvlog("tess draw: the %s is not our buffer (%s) — NOT drawn", what, object_getClassName(buf)); return NO; }
    NVMTLBuffer *nb = (NVMTLBuffer *)buf;
    if (off >= nb->_b.size) { nvlog("tess draw: %s offset %lu is outside its %zu bytes — NOT drawn", what, (unsigned long)off, nb->_b.size); return NO; }
    nvmtl_retain_resource(_cb, nb);
    uint64_t a = nvmtl_vk_buffer_address(&nb->_b);
    if (!a) { nvlog("tess draw: the %s has no device address — NOT drawn", what); return NO; }
    a += off;
    if (a & (al - 1)) { nvlog("tess draw: the %s at offset %lu is not %llu-byte aligned — NOT drawn", what, (unsigned long)off, (unsigned long long)al); return NO; }
    *out = a; return YES;
}
- (void)nvmtlPatches:(NSUInteger)n start:(NSUInteger)start count:(NSUInteger)count pib:(id<MTLBuffer>)pib pibOff:(NSUInteger)pibOff
                cpib:(id<MTLBuffer>)cpib cpibOff:(NSUInteger)cpibOff indexed:(BOOL)indexed instances:(NSUInteger)instances
                base:(NSUInteger)base indirect:(id<MTLBuffer>)ind indOff:(NSUInteger)indOff {
    if (!_begun) { nvlog("encoder: patch draw with no pipeline set"); return; }
    if (!_ps || !_ps->_p.tess) { nvlog("tess draw: the pipeline (%s) has no post-tessellation vertex function — NOT drawn", _ps ? _ps->_vname.UTF8String : "-"); return; }
    if (n != _ps->_p.tess_n) { nvlog("tess draw: %lu control points per patch, %s takes %u — NOT drawn", (unsigned long)n, _ps->_vname.UTF8String ?: "?", _ps->_p.tess_n); return; }
    if (!_tfb) { nvlog("tess draw: no tessellation factor buffer is set — NOT drawn"); return; }
    if (indexed != (_ps->_p.tess_cpidx != 0)) { nvlog("tess draw: a%s draw on a pipeline whose tessellationControlPointIndexType is %u — NOT drawn",
        indexed ? "n indexed" : " non-indexed", _ps->_p.tess_cpidx); return; }
    if (indexed && !cpib) { nvlog("tess draw: drawIndexedPatches: with no control-point index buffer — NOT drawn"); return; }
    if (_tfbStride & 3) { nvlog("tess draw: factor instanceStride %lu is not a multiple of 4 — NOT drawn", (unsigned long)_tfbStride); return; }
    uint64_t fac = 0, cpx = 0, pix = 0, ia = 0;
    if (![self nvmtlTessAddress:_tfb offset:_tfbOff align:4 what:"tessellation factor buffer" out:&fac] ||
        ![self nvmtlTessAddress:pib offset:pibOff align:4 what:"patch index buffer" out:&pix] ||
        ![self nvmtlTessAddress:cpib offset:cpibOff align:(_ps->_p.tess_cpidx == 1 ? 2 : 4) what:"control-point index buffer" out:&cpx] ||
        ![self nvmtlTessAddress:ind offset:indOff align:4 what:"indirect patch-draw buffer" out:&ia]) return;
    if (![self nvmtlBindSamplerPipeline]) return;
    nvtrace("DRAW patches %lu cp | vs %s | fs %s | blend %s", (unsigned long)n, _ps->_vname.UTF8String, _ps->_fname.UTF8String, _ps->_blend.UTF8String);
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_); nvtrace("  BUFS%s", bufs_); }
    if (ind) nvtrace("  indirect patches @%lu, pib %s", (unsigned long)indOff, pib ? "yes" : "no");
    else nvtrace("  patches start %lu count %lu instances %lu base %lu, pib %s", (unsigned long)start, (unsigned long)count,
                 (unsigned long)instances, (unsigned long)base, pib ? "yes" : "no");
    if (nvmtl_vk_cmd_draw_patches(&_cb->_c, &_ps->_p, fac, (uint32_t)_tfbStride, _tfScaleSet ? _tfScale : 1.0f, cpx, pix, (uint32_t)start,
                                  (uint32_t)count, (uint32_t)instances, (uint32_t)base, ind ? &((NVMTLBuffer *)ind)->_b : NULL, indOff))
        nvtrace("  NOT DRAWN: a vertex buffer the tessellation pipeline pulls is not bound");
}
- (void)drawPatches:(NSUInteger)n patchStart:(NSUInteger)start patchCount:(NSUInteger)count patchIndexBuffer:(id<MTLBuffer>)pib
    patchIndexBufferOffset:(NSUInteger)pibOff instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base {
    [self nvmtlPatches:n start:start count:count pib:pib pibOff:pibOff cpib:nil cpibOff:0 indexed:NO instances:instances base:base indirect:nil indOff:0];
}
- (void)drawPatches:(NSUInteger)n patchIndexBuffer:(id<MTLBuffer>)pib patchIndexBufferOffset:(NSUInteger)pibOff
     indirectBuffer:(id<MTLBuffer>)ind indirectBufferOffset:(NSUInteger)indOff {
    if (!ind) { nvlog("drawPatches:indirectBuffer: nil"); return; }
    [self nvmtlPatches:n start:0 count:0 pib:pib pibOff:pibOff cpib:nil cpibOff:0 indexed:NO instances:0 base:0 indirect:ind indOff:indOff];
}
- (void)drawIndexedPatches:(NSUInteger)n patchStart:(NSUInteger)start patchCount:(NSUInteger)count patchIndexBuffer:(id<MTLBuffer>)pib
    patchIndexBufferOffset:(NSUInteger)pibOff controlPointIndexBuffer:(id<MTLBuffer>)cpib controlPointIndexBufferOffset:(NSUInteger)cpibOff
             instanceCount:(NSUInteger)instances baseInstance:(NSUInteger)base {
    [self nvmtlPatches:n start:start count:count pib:pib pibOff:pibOff cpib:cpib cpibOff:cpibOff indexed:YES instances:instances base:base indirect:nil indOff:0];
}
- (void)drawIndexedPatches:(NSUInteger)n patchIndexBuffer:(id<MTLBuffer>)pib patchIndexBufferOffset:(NSUInteger)pibOff
    controlPointIndexBuffer:(id<MTLBuffer>)cpib controlPointIndexBufferOffset:(NSUInteger)cpibOff
             indirectBuffer:(id<MTLBuffer>)ind indirectBufferOffset:(NSUInteger)indOff {
    if (!ind) { nvlog("drawIndexedPatches:indirectBuffer: nil"); return; }
    [self nvmtlPatches:n start:0 count:0 pib:pib pibOff:pibOff cpib:cpib cpibOff:cpibOff indexed:YES instances:0 base:0 indirect:ind indOff:indOff];
}
- (void)nvmtlVisibilityError:(NSString *)message {
    nvlog("%s", message.UTF8String);
    [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
        code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey: message}]];
}
- (void)nvmtlCloseVisibility {
    if (_visActive) {
        nvmtl_vk_cmd_end_occlusion(&_cb->_c, _visSlot);
        if (!_visRecords) _visRecords = [NSMutableArray new];
        [_visRecords addObject:@[@(_visSlot), @(_visOffset)]];
        _visActive = NO;
    } else if (_visPending) {
        if (!_visRecords) _visRecords = [NSMutableArray new];
        [_visRecords addObject:@[@(UINT32_MAX), @(_visOffset)]];
    }
    _visPending = NO;
}
- (void)nvmtlBeginVisibility {
    if (!_visPending || !_begun) return;
    uint32_t query;
    if (nvmtl_vk_cmd_alloc_occlusion(&_cb->_c, &query) ||
        nvmtl_vk_cmd_begin_occlusion(&_cb->_c, query)) {
        [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: query allocation failed at offset %zu - pass refused", (size_t)_visOffset]];
        _refused = YES; return;
    }
    _visSlot = query; _visActive = YES; _visPending = NO;
}
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)mode offset:(NSUInteger)offset {
    [self nvmtlCloseVisibility];
    if (mode == MTLVisibilityResultModeDisabled) return;
    if (!_visBuf || (offset & 7) || offset > _visBuf->_b.size ||
        _visBuf->_b.size - offset < sizeof(uint64_t) || offset > 262144) {
        [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: invalid byte offset %zu", (size_t)offset]]; return;
    }
    _visOffset = offset; _visPending = YES;
    [self nvmtlBeginVisibility];
}
- (void)endEncoding {
    [self nvmtlEndPass];
    if (_tsEnd) { nvmtl_ts_pass_end(_cb, self, _tsEnd); _tsEnd = nil; }
}
- (void)nvmtlEndPass {
    _cb->_nEnd++;
    if (_dumpPso) { _dumpPso = NO; [self nvmtlDumpBoundFragmentBuffers]; }
    [self nvmtlCloseVisibility];
    if (_begun) nvmtl_vk_cmd_end_render(&_cb->_c);
    NSMutableDictionary *groups = [NSMutableDictionary new];
    BOOL needScratch = NO;
    for (NSArray *record in _visRecords) {
        NSNumber *offset = record[1];
        NSMutableArray *queries = groups[offset];
        if (!queries) { queries = [NSMutableArray new]; groups[offset] = queries; }
        [queries addObject:record[0]];
        if (queries.count > 1) needScratch = YES;
    }
    NVMTLBuffer *scratch = nil;
    if (needScratch) {
        scratch = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(_visRecords.count * 8, 0, &scratch->_b)) {
            [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: accumulation scratch allocation failed"]]; scratch = nil;
        } else {
            if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
            [_cb->_scratch addObject:scratch];
        }
    }
    size_t cursor = 0;
    for (NSNumber *key in groups) {
        NSArray *queries = groups[key]; size_t offset = key.unsignedLongLongValue;
        if (queries.count > 1 && !scratch) continue;
        nvk_buffer *dst = queries.count > 1 ? &scratch->_b : &_visBuf->_b;
        size_t first = cursor;
        for (NSNumber *value in queries) {
            uint32_t query = value.unsignedIntValue;
            size_t out = queries.count > 1 ? cursor++ * 8 : offset;
            if (query == UINT32_MAX) {
                if (nvmtl_vk_cmd_write_word(&_cb->_c, dst, out, 0) ||
                    nvmtl_vk_cmd_write_word(&_cb->_c, dst, out + 4, 0))
                    [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: empty result write failed at offset %zu", out]];
            } else if (nvmtl_vk_cmd_copy_occlusion(&_cb->_c, query, 1, dst, out))
                [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: result copy failed at offset %zu", out]];
        }
        if (queries.count > 1 && nvmtl_vk_cmd_sum_occlusion(&_cb->_c, &scratch->_b, first,
                                                          (uint32_t)queries.count, &_visBuf->_b, offset))
            [self nvmtlVisibilityError:[NSString stringWithFormat:@"visibility: accumulation failed at offset %zu", offset]];
    }
    _visRecords = nil;
    if (_begun) return;
    if (_refused) return;
    switch (nvmtl_vk_pass_kind(&_pass)) {
        case 1: { static int said; if (!said++) nvlog("encoder: apps2 a pass with no draws loads everything and resolves nothing — left untouched"); return; }
        case 2:
            if (nvmtl_vk_cmd_empty_pass(&_cb->_c, &_pass)) nvlog("encoder: apps2 empty pass FAILED — its clears and resolves are NOT applied");
            else { static int said; if (said++ < 4) nvlog("encoder: apps2 empty pass (the clears/resolves of a pass with no draws)"); }
            if (_resolve) { NVMTLTexture *rs = _resolve->_parent ? _resolve->_parent : _resolve; if (rs->_surf) nvmtl_surf_dirty(_cb, rs); }
            return;
        default: break;
    }
    if (_load & 1u) { static int said; if (!said++) nvlog("encoder: a Load pass with no draws — target left untouched"); return; }
    if (_target && nvmtl_vk_cmd_clear_image(&_cb->_c, [_target nvi], _clear))
        nvlog("encoder: clear-only pass FAILED");
    else if (_target)
        nvlog("encoder: clear-only pass (%.2f %.2f %.2f %.2f)", _clear[0], _clear[1], _clear[2], _clear[3]);
}
- (void)setViewport:(MTLViewport)v {
    nvtrace("VP %.4g %.4g %.4g %.4g z %.3g %.3g", v.originX, v.originY, v.width, v.height, v.znear, v.zfar);
    _vp[0] = (float)v.originX; _vp[1] = (float)v.originY; _vp[2] = (float)v.width; _vp[3] = (float)v.height; _vp[4] = (float)v.znear; _vp[5] = (float)v.zfar; _hasVP = YES;
    if (_begun) nvmtl_vk_cmd_set_viewport(&_cb->_c, _vp[0], _vp[1], _vp[2], _vp[3], _vp[4], _vp[5]);
}
- (void)setViewports:(const MTLViewport *)v count:(NSUInteger)n { if (v && n) [self setViewport:v[0]]; }
- (void)setScissorRects:(const MTLScissorRect *)r count:(NSUInteger)n { if (r && n) [self setScissorRect:r[0]]; }
- (void)setScissorRect:(MTLScissorRect)r {
    nvtrace("SC %lu %lu %lu %lu", (unsigned long)r.x, (unsigned long)r.y, (unsigned long)r.width, (unsigned long)r.height);
    _sc[0] = (uint32_t)r.x; _sc[1] = (uint32_t)r.y; _sc[2] = (uint32_t)r.width; _sc[3] = (uint32_t)r.height; _hasSC = YES;
    if (!_begun) return;
    nvmtl_vk_cmd_set_scissor(&_cb->_c, (int32_t)r.x, (int32_t)r.y, (uint32_t)r.width, (uint32_t)r.height);
}
- (void)setCullMode:(MTLCullMode)mode {
    _cull = (_cull & 4) | (mode == MTLCullModeFront ? 1 : mode == MTLCullModeBack ? 2 : 0);
    if (_begun) nvmtl_vk_cmd_set_cull(&_cb->_c, _cull);
}
- (void)setFrontFacingWinding:(MTLWinding)w {
    _cull = (_cull & 3) | (w == MTLWindingCounterClockwise ? 4 : 0);
    if (_begun) nvmtl_vk_cmd_set_cull(&_cb->_c, _cull);
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)icb withRange:(NSRange)r {
    NVMTLIndirectCommandBuffer *b = (NVMTLIndirectCommandBuffer *)icb;
    if (!b) { nvlog("executeCommandsInBuffer: nil"); return; }
    if (b->_rec) { [self nvmtlExecuteRecords:b range:r]; return; }
    NSUInteger n = 0;
    for (NSUInteger i = r.location; i < r.location + r.length && i < b->_cmds.count; i++) {
        NVMTLIndirectCommand *c = b->_cmds[i];
        if (!c->_armed || c->_isCompute) continue;
        if (c->_ps) [self setRenderPipelineState:c->_ps];
        if (c->_hasCull) [self setCullMode:c->_cull];
        if (c->_hasWinding) [self setFrontFacingWinding:c->_winding];
        if (c->_hasFill) [self setTriangleFillMode:c->_fill];
        if (c->_hasClip) [self setDepthClipMode:c->_clip];
        if (c->_ds) [self setDepthStencilState:c->_ds];
        if (c->_hasBias) [self setDepthBias:c->_bias slopeScale:c->_slope clamp:c->_clamp];
        for (NSArray *e in c->_vbufs) {
            if (e.count > 3) [self setVertexBuffer:e[0] offset:[e[1] unsignedLongValue] attributeStride:[e[3] unsignedLongValue] atIndex:[e[2] unsignedLongValue]];
            else [self setVertexBuffer:e[0] offset:[e[1] unsignedLongValue] atIndex:[e[2] unsignedLongValue]]; }
        for (NSArray *e in c->_fbufs) [self setFragmentBuffer:e[0] offset:[e[1] unsignedLongValue] atIndex:[e[2] unsignedLongValue]];
        if (c->_indexed)
            [self drawIndexedPrimitives:c->_type indexCount:c->_count indexType:c->_itype indexBuffer:c->_ibuf
                      indexBufferOffset:c->_ioff instanceCount:c->_instances baseVertex:c->_baseVertex baseInstance:c->_baseInstance];
        else
            [self drawPrimitives:c->_type vertexStart:c->_start vertexCount:c->_count
                   instanceCount:c->_instances baseInstance:c->_baseInstance];
        n++;
    }
    nvlog("executeCommandsInBuffer: replayed %lu of %lu slot(s)", (unsigned long)n, (unsigned long)r.length);
}
// A GPU-encodable ICB (nvmtl_icb_layout.h): each slot's state is replayed as the CPU encoded it, and its draw comes from
// its record, which the CPU (indirectRenderCommandAtIndex:) or a shader (render_command) wrote last: an indexed indirect
// draw when the slot has an index buffer, else a plain indirect draw.
// The app orders the shader's writes before this with a barrier or an encoder boundary, as Metal requires
// (memoryBarrierWithScope: and the pass boundary are ALL_COMMANDS MEMORY_WRITE -> MEMORY_READ, which covers the
// indirect-argument read).
// Slot b joins a run that slot a started when replaying b changes nothing a's replay set: the same state fields (the
// same objects, the same buffer bindings), the same CPU draw form and primitive type, and for an indexed slot the same
// index buffer, offset and index size. A run is one vkCmdDraw*Indirect with drawCount > 1 over consecutive records
// (64 bytes apart), which ANV can execute without one register load per argument (nvmtl_icb_layout.h).
static BOOL nvmtl_icb_same_slot(NVMTLIndirectCommand *a, NVMTLIndirectCommand *b) {
    if (a->_isCompute || b->_isCompute || a->_ps != b->_ps || a->_ds != b->_ds) return NO;
    if (a->_hasCull != b->_hasCull || (a->_hasCull && a->_cull != b->_cull)) return NO;
    if (a->_hasWinding != b->_hasWinding || (a->_hasWinding && a->_winding != b->_winding)) return NO;
    if (a->_hasFill != b->_hasFill || (a->_hasFill && a->_fill != b->_fill)) return NO;
    if (a->_hasClip != b->_hasClip || (a->_hasClip && a->_clip != b->_clip)) return NO;
    if (a->_hasBias != b->_hasBias || (a->_hasBias && (a->_bias != b->_bias || a->_slope != b->_slope || a->_clamp != b->_clamp))) return NO;
    if (!(a->_vbufs.count == 0 && b->_vbufs.count == 0) && ![a->_vbufs isEqualToArray:b->_vbufs]) return NO;
    if (!(a->_fbufs.count == 0 && b->_fbufs.count == 0) && ![a->_fbufs isEqualToArray:b->_fbufs]) return NO;
    if (a->_armed != b->_armed || (a->_armed && (a->_type != b->_type || a->_indexed != b->_indexed))) return NO;
    if (a->_armed && a->_indexed && (a->_ibuf != b->_ibuf || a->_ioff != b->_ioff || a->_itype != b->_itype)) return NO;
    return YES;
}

- (void)nvmtlExecuteRecords:(NVMTLIndirectCommandBuffer *)b range:(NSRange)r {
    nvmtl_icb_record *rec = [b nvmtlRecords];
    nvmtl_retain_resource(_cb, b); nvmtl_retain_resource(_cb, b->_rec);
    NSUInteger n = 0, runs = 0;
    const NSUInteger end = MIN(r.location + r.length, b->_cmds.count);
    for (NSUInteger i = r.location; i < end; i++) {
        NVMTLIndirectCommand *c = b->_cmds[i];
        if (c->_isCompute) continue;
        if (c->_ps) [self setRenderPipelineState:c->_ps];
        if (c->_hasCull) [self setCullMode:c->_cull];
        if (c->_hasWinding) [self setFrontFacingWinding:c->_winding];
        if (c->_hasFill) [self setTriangleFillMode:c->_fill];
        if (c->_hasClip) [self setDepthClipMode:c->_clip];
        if (c->_ds) [self setDepthStencilState:c->_ds];
        if (c->_hasBias) [self setDepthBias:c->_bias slopeScale:c->_slope clamp:c->_clamp];
        for (NSArray *e in c->_vbufs) {
            if (e.count > 3) [self setVertexBuffer:e[0] offset:[e[1] unsignedLongValue] attributeStride:[e[3] unsignedLongValue] atIndex:[e[2] unsignedLongValue]];
            else [self setVertexBuffer:e[0] offset:[e[1] unsignedLongValue] atIndex:[e[2] unsignedLongValue]]; }
        for (NSArray *e in c->_fbufs) [self setFragmentBuffer:e[0] offset:[e[1] unsignedLongValue] atIndex:[e[2] unsignedLongValue]];
        if (!_begun) { nvlog("executeCommandsInBuffer: slot %lu with no pipeline set", (unsigned long)i); continue; }
        // the topology: the CPU draw's, else the pipeline's class; written back so a later GPU encode is checked against it
        const uint32_t prim = c->_armed ? (uint32_t)c->_type
                                        : nvmtl_icb_prim_for_class(_ps ? (unsigned long)_ps->_descriptor.inputPrimitiveTopology : 0);
        if (rec[i].execPrim != prim) [b nvmtlSetExecPrim:i prim:prim];
        if (![self nvmtlBindSamplerPipeline]) continue;
        if (nvmtl_vk_cmd_set_topology(&_cb->_c, prim)) continue;
        if ([self nvmtlVertexInputs]) continue;
        const size_t at = i * (size_t)NVMTL_ICB_RECORD_BYTES;
        NVMTLBuffer *ib = c->_armed && c->_indexed && [(id)c->_ibuf isKindOfClass:[NVMTLBuffer class]] ? (NVMTLBuffer *)c->_ibuf : nil;
        // the slots after this one that replay the same state (nvmtl_icb_same_slot) and have the same draw form go with it
        const uint32_t form = ib ? rec[i].ibShift : 0;
        NSUInteger j = i + 1;
        while (j < end && nvmtl_icb_same_slot(c, b->_cmds[j]) && (ib ? rec[j].ibShift : 0) == form) {
            if (rec[j].execPrim != prim) [b nvmtlSetExecPrim:j prim:prim];
            j++;
        }
        const uint32_t run = (uint32_t)(j - i);
        // the indexed form when the slot has an index buffer (the translator refuses a plain draw a shader writes there),
        // else the plain form
        if (ib && form) {
            nvmtl_retain_resource(_cb, ib);
            nvmtl_vk_cmd_draw_indexed_indirect_n(&_cb->_c, &ib->_b, c->_ioff & ((1u << form) - 1u), form == 2,
                                                 &b->_rec->_b, at + NVMTL_ICB_W_IDX * 4, run, NVMTL_ICB_RECORD_BYTES);
        } else nvmtl_vk_cmd_draw_indirect_n(&_cb->_c, &b->_rec->_b, at + NVMTL_ICB_W_DRAW * 4, run, NVMTL_ICB_RECORD_BYTES);
        n += run; runs++;
        i = j - 1;
    }
    { static int said; if (said++ < 8) nvlog("executeCommandsInBuffer: %lu of %lu slot(s) drawn from their records in %lu indirect draw(s) (GPU-encodable ICB)", (unsigned long)n, (unsigned long)r.length, (unsigned long)runs); }
    // diagnostic: a GPU-written draw the execute could not express (nvmtl_icb_layout.h, kind REFUSED), said up to 8 times per
    // ICB and looked for in its first 32 executes only (the records may be in VRAM: each read crosses the BAR)
    if (b->_refusedSaid < 8 && b->_refusedChecks < 32 && end > r.location) {
        NVMTLIndirectCommandBuffer *held = b; const NSUInteger lo = r.location;
        b->_refusedChecks++;
        [_cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            nvmtl_icb_record *rr = [held nvmtlGPURecords];
            if (!rr || done.status != MTLCommandBufferStatusCompleted) return;
            for (NSUInteger k = lo; k < end && held->_refusedSaid < 8; k++) if (rr[k].kind == NVMTL_ICB_REFUSED) {
                held->_refusedSaid++;
                nvlog("ICB %p slot %lu: a shader encoded a draw the execute cannot express (primitive %u, executed as %u; index shift bound %u) - NOT drawn",
                      (__bridge void *)held, (unsigned long)k, rr[k].prim, rr[k].execPrim, rr[k].ibShift);
            }
        }];
    }
}
- (void)setDepthClipMode:(MTLDepthClipMode)mode {
    if (nvmtl_vk_cmd_set_depth_clip(&_cb->_c, mode == MTLDepthClipModeClip))
        nvlog("setDepthClipMode:%lu REFUSED — VK_EXT_depth_clip_enable is not enabled on this device",
              (unsigned long)mode);
}
- (void)setTriangleFillMode:(MTLTriangleFillMode)mode {
    if (nvmtl_vk_cmd_set_polygon_mode(&_cb->_c, mode == MTLTriangleFillModeLines))
        nvlog("setTriangleFillMode:%lu REFUSED — VK_EXT_extended_dynamic_state3 is not enabled on this device",
              (unsigned long)mode);
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)s {
    _ds = (NVMTLDepthStencilState *)s;
    [self nvmtlApplyDepthStencil];
}
// macOS 26: the depth bounds test. Setting a range turns the test on for the rest of the encoder; fragments whose STORED
// attachment depth is outside [min, max] are discarded. Apple applies no test until this is called.
- (void)setDepthTestMinBound:(float)lo maxBound:(float)hi {
    _dbOn = YES; _dbLo = lo; _dbHi = hi;
    [self nvmtlApplyDepthStencil];
}
// macOS 26: a logical-to-physical colour attachment map. Measured on Apple GPUs, a Metal 3 encoder accepts it and nothing
// changes, because a Metal 3 render pipeline always uses the identity mapping (only Metal 4 pipelines inherit the map).
// Without the method the call is an unrecognized selector and the app stops.
- (void)setColorAttachmentMap:(id)map {
    static int said;
    if (map && !said++) nvlog("render encoder setColorAttachmentMap: a Metal 3 pipeline uses the identity mapping, so the map has no effect, as on Apple GPUs (said once)");
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)count indexType:(MTLIndexType)it
                  indexBuffer:(__unsafe_unretained id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off
                instanceCount:(NSUInteger)instances baseVertex:(NSInteger)baseVertex baseInstance:(NSUInteger)baseInstance {
    if (!_begun) { nvlog("drawIndexedPrimitives with no pipeline set"); return; }
    // the caller holds the index buffer, then the command buffer: no retain/release pair per draw (the release of a buffer
    // whose cache line the completion thread had touched was 5% of Minecraft's GL render thread, 2026-10-29)
    __unsafe_unretained NVMTLBuffer *b = (NVMTLBuffer *)ib;
    if (!b) { nvlog("drawIndexedPrimitives with no index buffer"); return; }
    nvmtl_retain_resource(_cb, b);
    if (![self nvmtlBindSamplerPipeline]) return;
    nvtrace("DRAW type %u | vs %s | fs %s | blend %s", (unsigned)type, _ps ? _ps->_vname.UTF8String : "-", _ps ? _ps->_fname.UTF8String : "-", _ps ? _ps->_blend.UTF8String : "-");
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_); nvtrace("  BUFS%s", bufs_); }
    if (nvmtl_vk_cmd_set_topology(&_cb->_c, (unsigned)type)) return;
    if ([self nvmtlVertexInputs]) return;
    nvtrace("  indexed count %lu u32 %d off %lu instances %lu baseVertex %ld", (unsigned long)count, it == MTLIndexTypeUInt32, (unsigned long)off, (unsigned long)instances, (long)baseVertex);
    nvmtl_vk_cmd_draw_indexed(&_cb->_c, &b->_b, off, (uint32_t)count, it == MTLIndexTypeUInt32,
                              (uint32_t)instances, (int32_t)baseVertex, (uint32_t)baseInstance);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off instanceCount:(NSUInteger)i {
    [self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:ib indexBufferOffset:off
                  instanceCount:i baseVertex:0 baseInstance:0];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)off {
    [self drawIndexedPrimitives:t indexCount:n indexType:it indexBuffer:ib indexBufferOffset:off
                  instanceCount:1 baseVertex:0 baseInstance:0];
}

- (void)setStencilReferenceValue:(uint32_t)ref { nvmtl_vk_cmd_set_stencil_ref(&_cb->_c, ref, ref); _hasRef = YES; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { nvmtl_vk_cmd_set_stencil_ref(&_cb->_c, f, b); _hasRef = YES; }
- (void)setDepthBias:(float)bias slopeScale:(float)slope clamp:(float)clamp { nvmtl_vk_cmd_set_depth_bias(&_cb->_c, bias, slope, clamp); }
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    float rgba[4] = { r, g, b, a }; nvmtl_vk_cmd_set_blend_color(&_cb->_c, rgba);
}
- (void)setVertexBufferOffset:(NSUInteger)off atIndex:(NSUInteger)i {
    nvtrace("VBOFF[%lu] -> %lu", (unsigned long)i, (unsigned long)off);
    if (i < NVMTL_NBUFAT && _vbufAt[i]) _vbufOff[i] = off;
    if (nvmtl_vk_bind_buffer_offset(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, off))
        nvlog("setVertexBufferOffset: nothing bound at vertex index %lu", (unsigned long)i);
}
- (void)nvmtlVertexStride:(NSUInteger)st atIndex:(NSUInteger)i {
    if (i >= NVMTL_NVIN) { nvlog("apps-dyn: attributeStride at vertex buffer index %lu is outside [0,%d) — ignored", (unsigned long)i, NVMTL_NVIN); return; }
    if (st == MTLAttributeStrideStatic) { _vstrideSet &= ~(1u << i); return; }
    if (st > UINT32_MAX) { nvlog("apps-dyn: attributeStride %lu at vertex buffer index %lu does not fit — the index keeps NO stride", (unsigned long)st, (unsigned long)i);
        _vstrideSet &= ~(1u << i); return; }
    _vstride[i] = (uint32_t)st; _vstrideSet |= 1u << i;
    nvtrace("VBSTRIDE[%lu] -> %lu", (unsigned long)i, (unsigned long)st);
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    [self setVertexBuffer:b offset:off atIndex:i]; [self nvmtlVertexStride:st atIndex:i]; }
- (void)setVertexBufferOffset:(NSUInteger)off attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    [self setVertexBufferOffset:off atIndex:i]; [self nvmtlVertexStride:st atIndex:i]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])bufs offsets:(const NSUInteger *)offs
        attributeStrides:(const NSUInteger *)sts withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (bufs[k])
        [self setVertexBuffer:bufs[k] offset:offs ? offs[k] : 0 attributeStride:sts ? sts[k] : MTLAttributeStrideStatic atIndex:r.location + k]; }
- (void)setVertexBytes:(const void *)b length:(NSUInteger)l attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    [self setVertexBytes:b length:l atIndex:i]; [self nvmtlVertexStride:st atIndex:i]; }
- (void)setFragmentBufferOffset:(NSUInteger)off atIndex:(NSUInteger)i {
    nvtrace("FBOFF[%lu] -> %lu", (unsigned long)i, (unsigned long)off);
    if (i < NVMTL_NBUFAT && _fbufAt[i]) _fbufOff[i] = off;
    if (nvmtl_vk_bind_buffer_offset(&_cb->_c, NVMTL_SET_FRAGMENT, (uint32_t)i, off))
        nvlog("setFragmentBufferOffset: nothing bound at fragment index %lu", (unsigned long)i);
}
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])bufs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (bufs[k]) [self setVertexBuffer:bufs[k] offset:offs ? offs[k] : 0 atIndex:r.location + k];
}
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])bufs offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (bufs[k]) [self setFragmentBuffer:bufs[k] offset:offs ? offs[k] : 0 atIndex:r.location + k];
}
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (t[k]) [self setVertexTexture:t[k] atIndex:r.location + k];
}
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (t[k]) [self setFragmentTexture:t[k] atIndex:r.location + k];
}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])ss withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setVertexSamplerState:ss[k] atIndex:r.location + k];
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])ss withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setFragmentSamplerState:ss[k] atIndex:r.location + k];
}
- (void)setFragmentSamplerState:(id<MTLSamplerState>)ss lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self setFragmentSamplerState:ss atIndex:i];
    if (i >= 16 || !((_pixMask[1] >> i) & 1u)) return;   // no pixel-coordinate state at i: nothing to clamp in the key
    NSString *key=[@(i) stringValue];
    if(_fragmentSamplers[key]) {
        NSMutableDictionary *state=[_fragmentSamplers[key] mutableCopy];
        state[@"lod_min_clamp"]=@(lo);state[@"lod_max_clamp"]=@(hi);
        if (nvmtl_pixel_sampler_update(_fragmentSamplers,key,state)) _pixelSamplerKey=nil;
    }
}
- (void)setVertexSamplerState:(id<MTLSamplerState>)ss lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self setVertexSamplerState:ss atIndex:i];
    if (i >= 16 || !((_pixMask[0] >> i) & 1u)) return;
    NSString *key=[@(i) stringValue];
    if(_vertexSamplers[key]) {
        NSMutableDictionary *state=[_vertexSamplers[key] mutableCopy];
        state[@"lod_min_clamp"]=@(lo);state[@"lod_max_clamp"]=@(hi);
        if (nvmtl_pixel_sampler_update(_vertexSamplers,key,state)) _pixelSamplerKey=nil;
    }
}
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])ss lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    for(NSUInteger k=0;k<r.length;k++) [self setFragmentSamplerState:ss[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location+k];
}
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])ss lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    for(NSUInteger k=0;k<r.length;k++) [self setVertexSamplerState:ss[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location+k];
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { nvmtl_retain_resource(_cb, r); }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])h count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) nvmtl_use_heap(_cb, h[i]); }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])h count:(NSUInteger)n stages:(MTLRenderStages)st { nvtrace("USEHEAPS %lu stages %lu", (unsigned long)n, (unsigned long)st); for (NSUInteger i = 0; i < n; i++) [self useHeap:h[i] stages:st]; }
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)st { nvmtl_retain_resource(_cb, r); nvtrace("USERES %s %p usage %lu stages %lu", object_getClassName(r), (__bridge void *)r, (unsigned long)u, (unsigned long)st); }
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u { for (NSUInteger i=0;i<n;i++) nvmtl_retain_resource(_cb,r[i]); }
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u stages:(MTLRenderStages)st { for (NSUInteger i=0;i<n;i++) nvmtl_retain_resource(_cb,r[i]); nvtrace("USERES x%lu usage %lu stages %lu", (unsigned long)n, (unsigned long)u, (unsigned long)st); }
- (void)useHeap:(id<MTLHeap>)h { nvmtl_use_heap(_cb, h); }
- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)st { (void)st; nvmtl_use_heap(_cb, h); }
- (void)sampleCountersInBuffer:(id)sb atSampleIndex:(NSUInteger)i withBarrier:(BOOL)b { nvmtl_sample_counter(_cb, sb, i, self, b); }
- (void)textureBarrier { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)updateFence:(id<MTLFence>)f afterStages:(MTLRenderStages)st {}
- (void)waitForFence:(id<MTLFence>)f beforeStages:(MTLRenderStages)st { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n afterStages:(MTLRenderStages)a beforeStages:(MTLRenderStages)b { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)drawPrimitives:(MTLPrimitiveType)type indirectBuffer:(id<MTLBuffer>)buf indirectBufferOffset:(NSUInteger)off {
    NVMTLBuffer *nb = (NVMTLBuffer *)buf;
    if (!nb) { nvlog("drawPrimitives:indirectBuffer: nil"); return; }
    nvmtl_retain_resource(_cb, nb);
    if (!_begun) { nvlog("drawPrimitives:indirectBuffer: no render pass"); return; }
    if (![self nvmtlBindSamplerPipeline]) return;
    nvtrace("DRAW type %u | vs %s | fs %s | blend %s", (unsigned)type, _ps ? _ps->_vname.UTF8String : "-", _ps ? _ps->_fname.UTF8String : "-", _ps ? _ps->_blend.UTF8String : "-");
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_); nvtrace("  BUFS%s", bufs_); }
    if (nvmtl_vk_cmd_set_topology(&_cb->_c, (unsigned)type)) return;
    if ([self nvmtlVertexInputs]) return;
    nvmtl_vk_cmd_draw_indirect(&_cb->_c, &nb->_b, off);
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexType:(MTLIndexType)it indexBuffer:(id<MTLBuffer>)idx
          indexBufferOffset:(NSUInteger)ioff indirectBuffer:(id<MTLBuffer>)buf indirectBufferOffset:(NSUInteger)off {
    NVMTLBuffer *ni = (NVMTLBuffer *)idx, *nb = (NVMTLBuffer *)buf;
    if (!ni || !nb) { nvlog("drawIndexedPrimitives:indirectBuffer: nil"); return; }
    nvmtl_retain_resource(_cb, ni);
    nvmtl_retain_resource(_cb, nb);
    if (!_begun) { nvlog("drawIndexedPrimitives:indirectBuffer: no render pass"); return; }
    if (![self nvmtlBindSamplerPipeline]) return;
    nvtrace("DRAW type %u | vs %s | fs %s | blend %s", (unsigned)type, _ps ? _ps->_vname.UTF8String : "-", _ps ? _ps->_fname.UTF8String : "-", _ps ? _ps->_blend.UTF8String : "-");
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_); nvtrace("  BUFS%s", bufs_); }
    if (nvmtl_vk_cmd_set_topology(&_cb->_c, (unsigned)type)) return;
    if ([self nvmtlVertexInputs]) return;
    nvmtl_vk_cmd_draw_indexed_indirect(&_cb->_c, &ni->_b, ioff, it == MTLIndexTypeUInt32, &nb->_b, off);
}
- (void)pushDebugGroup:(NSString *)g {
    nvtrace("GROUP %s", g.UTF8String ?: "?");
    // NVMTL_SKIP_GROUP=<text>: draws inside a debug group whose name contains it are not recorded (finds a hanging shader)
    static const char *skip; static int once; if (!once) { once = 1; skip = getenv("NVMTL_SKIP_GROUP"); }
    _groupDepth++;
    if (skip && *skip && !_cb->_c.skip_group && strstr(g.UTF8String ?: "", skip)) { _cb->_c.skip_group = _groupDepth; nvlog("NVMTL_SKIP_GROUP: skipping %s", g.UTF8String); }
    // NVMTL_DUMP_GROUP=<text>: the first time a debug group containing it opens, the fragment buffers bound at that moment
    // are copied out after this command buffer completes, up to 64 KB each, to $TMPDIR/dump-f<index>.bin (diagnostic)
    static const char *dump; static int donce; static _Atomic int dumped; if (!donce) { donce = 1; dump = getenv("NVMTL_DUMP_GROUP"); }
    if (dump && *dump && strstr(g.UTF8String ?: "", dump) && !atomic_exchange(&dumped, 1)) { _dumpDepth = _groupDepth; _dumpName = [g copy]; }
}
- (void)nvmtlDumpBoundFragmentBuffers {
    {
        NSMutableDictionary *bound = [NSMutableDictionary new]; NSString *name = _dumpName;
        for (NSUInteger i = 0; i < NVMTL_NBUFAT; i++) if (_fbufAt[i]) bound[@(i)] = @[_fbufAt[i], @(_fbufOff[i])];
        [_cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
            if (done.status == MTLCommandBufferStatusNotEnqueued) return;   // glcpu4: released uncommitted, nothing to dump
            dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                id<MTLDevice> dev = done.device; id<MTLCommandQueue> q = [dev newCommandQueue]; id<MTLCommandBuffer> cb = [q commandBuffer];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder]; NSMutableDictionary *outs = [NSMutableDictionary new];
                for (NSNumber *k in bound) { NVMTLBuffer *b = bound[k][0]; NSUInteger off = [bound[k][1] unsignedLongValue];
                    if (off >= b.length) continue; NSUInteger n = MIN(b.length - off, (NSUInteger)65536);
                    id<MTLBuffer> o = [dev newBufferWithLength:n options:MTLResourceStorageModeShared];
                    [be copyFromBuffer:b sourceOffset:off toBuffer:o destinationOffset:0 size:n]; outs[k] = o; }
                [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
                for (NSNumber *k in outs) { id<MTLBuffer> o = outs[k];
                    NSString *path = [NSTemporaryDirectory() stringByAppendingFormat:@"dump-f%@.bin", k];
                    [[NSData dataWithBytes:o.contents length:o.length] writeToFile:path atomically:YES]; }
                nvlog("NVMTL_DUMP_GROUP: %lu fragment buffer(s) of '%s' written to %s (command buffer status %ld)", (unsigned long)outs.count,
                      name.UTF8String, NSTemporaryDirectory().UTF8String, (long)done.status);
            });
        }];
    }
}
- (void)popDebugGroup { if (_dumpDepth && _dumpDepth == _groupDepth) { [self nvmtlDumpBoundFragmentBuffers]; _dumpDepth = 0; }
    if (_cb->_c.skip_group == _groupDepth) _cb->_c.skip_group = 0; if (_groupDepth) _groupDepth--; }
- (void)insertDebugSignpost:(NSString *)s {}
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { id<MTLDevice> d = _cb ? [_cb device] : nil; return d ? d : (id<MTLDevice>)gNVMTLMainDevice; }
// -commandBuffer is SPI on Apple's encoders (_MTLCommandEncoder), not in the MTLCommandEncoder protocol, but apps call it:
// Hades asks every render encoder for its command buffer each frame; the forwarding stub answered nil and messages to the
// nil command buffer did nothing. The encoder keeps _cb after endEncoding, so it answers then too, as Apple's does.
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)_cb; }
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i {
    BOOL resolve = a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve;
    BOOL planned = i < NVMTL_NCOL && _pass.res[i] != NULL;
    if (resolve && !planned) { nvmtl_apps1_resolve_miss++; static int said; if (said++ < 4)
        nvlog("encoder: apps1 setColorStoreAction:%lu atIndex:%lu — the pass began with no resolve texture on that slot: resolve NOT applied (apps3)", (unsigned long)a, (unsigned long)i); }
    else if (!resolve && planned) { nvmtl_apps1_resolve_extra++; static int said; if (said++ < 4)
        nvlog("encoder: apps1 setColorStoreAction:%lu atIndex:%lu — the resolve planned for MTLStoreActionUnknown stays: the resolve texture IS written", (unsigned long)a, (unsigned long)i); }
}
- (void)setDepthStoreAction:(MTLStoreAction)a {
    if (a == MTLStoreActionCustomSampleDepthStore) _pass.depth_store_options |= MTLStoreActionOptionCustomSamplePositions;
    if (a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve) { nvmtl_apps1_ds_resolve_miss++; static int said;
        if (said++ < 4) nvlog("encoder: apps1 setDepthStoreAction:%lu — depth resolve is not built: depth is stored, NOT resolved (apps3)", (unsigned long)a); }
}
- (void)setStencilStoreAction:(MTLStoreAction)a {
    if (a == MTLStoreActionMultisampleResolve || a == MTLStoreActionStoreAndMultisampleResolve) { nvmtl_apps1_ds_resolve_miss++; static int said;
        if (said++ < 4) nvlog("encoder: apps1 setStencilStoreAction:%lu — stencil resolve is not built: stencil is stored, NOT resolved (apps3)", (unsigned long)a); }
}
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)x atIndex:(NSUInteger)i {
    if (x & ~MTLStoreActionOptionCustomSamplePositions) [self nvmtlSamplePositionError:@"unknown color store options"];
    (void)i;
}
- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)x {
    _pass.depth_store_options = (uint32_t)x;
    if (x & ~MTLStoreActionOptionCustomSamplePositions) [self nvmtlSamplePositionError:@"unknown depth store options"];
}
- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)x {
    _pass.stencil_store_options = (uint32_t)x;
    if (x) [self nvmtlSamplePositionError:@"stencil store options are illegal"];
}
@end

@implementation NVMTLFence
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
@end

static _Atomic uint64_t gHAPlaced, gHAGiven, gHALive, gHAFallback, gHABacked, gHABackedBytes;
static int nvmtl_heap_grow(NVMTLHeap *h) {
    if (h->_nfr < h->_cfr) return 0;
    uint32_t nc = h->_cfr ? h->_cfr * 2 : 8;
    NSUInteger *n = realloc(h->_fr, (size_t)nc * 2 * sizeof(NSUInteger));
    if (!n) return -1;
    h->_fr = n; h->_cfr = nc; return 0;
}
static NSUInteger nvmtl_heap_take(NVMTLHeap *h, NSUInteger size, NSUInteger al) {
    NSUInteger got = NSUIntegerMax;
    if (!al) al = 1;
    os_unfair_lock_lock(&h->_fl);
    for (uint32_t i = 0; i < h->_nfr; i++) {
        NSUInteger ro = h->_fr[2 * i], rl = h->_fr[2 * i + 1], o = (ro + al - 1) / al * al;
        if (o < ro || o + size < o || o + size > ro + rl) continue;
        if (h->_lzc && size) {
            // a lazily backed heap: a resource that fits a slot must not straddle two (each slot may get its own memory);
            // a larger one starts on a slot boundary so it owns whole slots
            const NSUInteger c = h->_lzc;
            if (size > c ? o % c != 0 : o / c != (o + size - 1) / c) {
                NSUInteger o2 = (o / c + 1) * c; o2 = (o2 + al - 1) / al * al;
                if (o2 < o || o2 + size < o2 || o2 + size > ro + rl) continue;
                o = o2;
            }
        }
        NSUInteger lo = o - ro, hi = ro + rl - (o + size);
        if (lo && hi) {
            if (nvmtl_heap_grow(h)) break;
            memmove(&h->_fr[2 * (i + 1)], &h->_fr[2 * i], (size_t)(h->_nfr - i) * 2 * sizeof(NSUInteger));
            h->_fr[2 * i + 1] = lo; h->_fr[2 * (i + 1)] = o + size; h->_fr[2 * (i + 1) + 1] = hi; h->_nfr++;
        } else if (lo) h->_fr[2 * i + 1] = lo;
        else if (hi) { h->_fr[2 * i] = o + size; h->_fr[2 * i + 1] = hi; }
        else { memmove(&h->_fr[2 * i], &h->_fr[2 * (i + 1)], (size_t)(h->_nfr - i - 1) * 2 * sizeof(NSUInteger)); h->_nfr--; }
        got = o; break;
    }
    os_unfair_lock_unlock(&h->_fl);
    return got;
}
static const char *nvmtl_heap_give(NVMTLHeap *h, NSUInteger o, NSUInteger n) {
    const char *why = NULL;
    os_unfair_lock_lock(&h->_fl);
    uint32_t i = 0; while (i < h->_nfr && h->_fr[2 * i] < o) i++;
    int hasP = i > 0, hasN = i < h->_nfr;
    if (!n || o + n < o || o + n > h->_size) why = "outside the heap";
    else if (hasP && h->_fr[2 * (i - 1)] + h->_fr[2 * (i - 1) + 1] > o) why = "overlaps the free range before it (a double give)";
    else if (hasN && o + n > h->_fr[2 * i]) why = "overlaps the free range after it (a double give)";
    else {
        int mP = hasP && h->_fr[2 * (i - 1)] + h->_fr[2 * (i - 1) + 1] == o, mN = hasN && o + n == h->_fr[2 * i];
        if (mP && mN) { h->_fr[2 * (i - 1) + 1] += n + h->_fr[2 * i + 1];
                        memmove(&h->_fr[2 * i], &h->_fr[2 * (i + 1)], (size_t)(h->_nfr - i - 1) * 2 * sizeof(NSUInteger)); h->_nfr--; }
        else if (mP) h->_fr[2 * (i - 1) + 1] += n;
        else if (mN) { h->_fr[2 * i] = o; h->_fr[2 * i + 1] += n; }
        else if (nvmtl_heap_grow(h)) why = "the free list could not grow (the range stays out of circulation)";
        else { memmove(&h->_fr[2 * (i + 1)], &h->_fr[2 * i], (size_t)(h->_nfr - i) * 2 * sizeof(NSUInteger));
               h->_fr[2 * i] = o; h->_fr[2 * i + 1] = n; h->_nfr++; }
    }
    os_unfair_lock_unlock(&h->_fl);
    return why;
}
static int nvmtl_heap_take_at_locked(NVMTLHeap *h, NSUInteger o, NSUInteger size) {
    if (!size || o + size < o) return -1;
    for (uint32_t i = 0; i < h->_nfr; i++) {
        NSUInteger ro = h->_fr[2 * i], rl = h->_fr[2 * i + 1];
        if (ro > o) break;
        if (o + size > ro + rl) continue;
        NSUInteger lo = o - ro, hi = ro + rl - (o + size);
        if (lo && hi) {
            if (nvmtl_heap_grow(h)) return -1;
            memmove(&h->_fr[2 * (i + 1)], &h->_fr[2 * i], (size_t)(h->_nfr - i) * 2 * sizeof(NSUInteger));
            h->_fr[2 * i + 1] = lo; h->_fr[2 * (i + 1)] = o + size; h->_fr[2 * (i + 1) + 1] = hi; h->_nfr++;
        } else if (lo) h->_fr[2 * i + 1] = lo;
        else if (hi) { h->_fr[2 * i] = o + size; h->_fr[2 * i + 1] = hi; }
        else { memmove(&h->_fr[2 * i], &h->_fr[2 * (i + 1)], (size_t)(h->_nfr - i - 1) * 2 * sizeof(NSUInteger)); h->_nfr--; }
        return 0;
    }
    return -1;
}
#define NVMTL_HEAPPARK 8
static _Atomic uint64_t gHTPlaced, gHTRecycled, gHTParked, gHTEvicted, gHTFallback, gHTMisfit;
static void nvmtl_heaptex_census(const char *what) {
    const uint64_t n = atomic_load(&gHTPlaced) + atomic_load(&gHTRecycled) + atomic_load(&gHTFallback);
    if (n & (n - 1)) return;
    nvlog("heap texture (%s): %llu placed new, %llu recycled, %llu parked, %llu evicted, %llu took the legacy fresh path, %llu misfit",
          what, (unsigned long long)atomic_load(&gHTPlaced), (unsigned long long)atomic_load(&gHTRecycled),
          (unsigned long long)atomic_load(&gHTParked), (unsigned long long)atomic_load(&gHTEvicted),
          (unsigned long long)atomic_load(&gHTFallback), (unsigned long long)atomic_load(&gHTMisfit));
}
static int nvmtl_heap_unpark(NVMTLHeap *h, const uint32_t *key, nvk_image *out, NSUInteger *off, NSUInteger *len) {
    int got = -1;
    os_unfair_lock_lock(&h->_fl);
    for (uint32_t k = h->_npk; k-- > 0; ) {
        nvmtl_park_t *p = &h->_pk[k];
        if (memcmp(p->key, key, sizeof p->key) || nvmtl_heap_take_at_locked(h, p->off, p->len)) continue;
        *out = p->img; *off = p->off; *len = p->len;
        memmove(p, p + 1, (size_t)(h->_npk - k - 1) * sizeof *p); h->_npk--; got = 0; break;
    }
    os_unfair_lock_unlock(&h->_fl);
    return got;
}
static void nvmtl_heap_park(NVMTLHeap *h, nvk_image *img, const uint32_t *key, NSUInteger off, NSUInteger len) {
    nvk_image ev; int evict = 0, parked = 0;
    os_unfair_lock_lock(&h->_fl);
    if (!h->_pk) h->_pk = calloc(NVMTL_HEAPPARK, sizeof *h->_pk);
    if (h->_pk) {
        if (h->_npk == NVMTL_HEAPPARK) { ev = h->_pk[0].img; evict = 1;
            memmove(&h->_pk[0], &h->_pk[1], (NVMTL_HEAPPARK - 1) * sizeof *h->_pk); h->_npk--; }
        nvmtl_park_t *p = &h->_pk[h->_npk++]; p->img = *img; memcpy(p->key, key, sizeof p->key); p->off = off; p->len = len;
        parked = 1;
    }
    os_unfair_lock_unlock(&h->_fl);
    if (parked) { memset(img, 0, sizeof *img); atomic_fetch_add(&gHTParked, 1); }
    if (evict) { nvmtl_vk_image_destroy(&ev); atomic_fetch_add(&gHTEvicted, 1); }
}
static void nvmtl_heap_census(const char *what) {
    nvlog("heap alias (%s): %llu placed, %llu given back, %llu live = %llu MB bound in %llu backed heaps of %llu MB;"
          " %llu took the legacy fresh path", what,
          (unsigned long long)atomic_load(&gHAPlaced), (unsigned long long)atomic_load(&gHAGiven),
          (unsigned long long)(atomic_load(&gHAPlaced) - atomic_load(&gHAGiven)), (unsigned long long)(atomic_load(&gHALive) >> 20),
          (unsigned long long)atomic_load(&gHABacked), (unsigned long long)(atomic_load(&gHABackedBytes) >> 20),
          (unsigned long long)atomic_load(&gHAFallback));
}
// nm29 (nullmoth a968812): automatic heaps of 256 MB and up take device memory in 64 MB slots as resources land
static const size_t kLazySlot = 64u << 20, kLazyMin = 256u << 20;
static _Atomic uint64_t gHLazyHeaps, gHLazyReserved, gHLazyBacked, gHLazyMixed;
static int nvmtl_heap_lazy_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_HEAP_LAZY"); on = !(e && e[0] == '0');
                  nvlog("heap lazy backing: %s (automatic heaps of %zu MB and up take memory in %zu MB slots as resources land)",
                        on ? "ON" : "OFF (NVMTL_HEAP_LAZY=0)", kLazyMin >> 20, kLazySlot >> 20); }
    return on;
}
/* The memory and the offset inside it for a resource at heap offset off, length len. A non-lazy heap answers its one
 * _hm. A lazy heap answers the backing that owns every slot the range touches, creating one when none of them has memory
 * yet; NULL when the slots belong to different backings (the caller gives the range back and takes the legacy path). */
static nvk_heap *nvmtl_heap_mem_for(NVMTLHeap *h, NSUInteger off, NSUInteger len, NSUInteger *rel) {
    if (!h->_lzc) { *rel = off; return &h->_hm; }
    if (!len || off + len < off || off + len > h->_size) return NULL;
    const uint32_t s0 = (uint32_t)(off / h->_lzc), s1 = (uint32_t)((off + len - 1) / h->_lzc);
    nvk_heap *got = NULL;
    os_unfair_lock_lock(&h->_lzl);
    uint16_t own = h->_lzo[s0]; int same = 1;
    for (uint32_t s = s0 + 1; s <= s1; s++) if (h->_lzo[s] != own) { same = 0; break; }
    if (same && own) { got = &h->_lzm[own - 1]; *rel = off - h->_lzb[own - 1]; }
    else if (same && !own && h->_lzn < 65535) {
        if (h->_lzn == h->_lzcap) {
            const uint32_t nc = h->_lzcap ? h->_lzcap * 2 : 8;
            nvk_heap *nm = realloc(h->_lzm, nc * sizeof *nm); if (nm) h->_lzm = nm;
            size_t *nb = realloc(h->_lzb, nc * sizeof *nb); if (nb) h->_lzb = nb;
            if (nm && nb) h->_lzcap = nc;
        }
        if (h->_lzn < h->_lzcap) {
            const size_t base = (size_t)s0 * h->_lzc, end = (size_t)(s1 + 1) * h->_lzc, top = end < h->_size ? end : h->_size;
            nvk_heap *m = &h->_lzm[h->_lzn]; memset(m, 0, sizeof *m);
            if (!nvmtl_vk_heap_create(top - base, h->_storage == MTLStorageModeShared, m)) {
                nvmtl_vk_heap_zero(m, h->_storage == MTLStorageModeShared ? "shared slot" : "private slot");
                h->_lzb[h->_lzn] = base; h->_lzn++;
                for (uint32_t s = s0; s <= s1; s++) h->_lzo[s] = (uint16_t)h->_lzn;
                atomic_fetch_add(&gHLazyBacked, top - base);
                got = m; *rel = off - base;
            } else nvlog("heap lazy backing: %zu bytes for slots %u-%u of a %lu-byte heap REFUSED - legacy path", top - base, s0, s1, (unsigned long)h->_size);
        }
    } else if (!same) {
        const uint64_t n = atomic_fetch_add(&gHLazyMixed, 1);
        if (n < 4) nvlog("heap lazy backing: %lu+%lu spans slots %u-%u with different memories - legacy path", (unsigned long)off, (unsigned long)len, s0, s1);
    }
    os_unfair_lock_unlock(&h->_lzl);
    return got;
}
void nvmtl_heap_back(NVMTLHeap *h, NSUInteger size, int shared) {
    if (size >= kLazyMin && nvmtl_heap_lazy_on()) {
        h->_lzslots = (uint32_t)((size + kLazySlot - 1) / kLazySlot);
        h->_lzo = calloc(h->_lzslots, sizeof *h->_lzo);
        if (h->_lzo) {
            h->_lzc = kLazySlot; h->_lzl = OS_UNFAIR_LOCK_INIT; memset(&h->_hm, 0, sizeof h->_hm);
            h->_fl = OS_UNFAIR_LOCK_INIT; h->_nfr = 0; h->_cfr = 0; h->_fr = NULL;
            if (nvmtl_heap_grow(h)) { free(h->_lzo); h->_lzo = NULL; h->_lzc = 0; nvlog("heap alias: no free list for a %lu-byte heap - legacy path", (unsigned long)size); return; }
            h->_fr[0] = 0; h->_fr[1] = size; h->_nfr = 1; h->_backed = YES;
            atomic_fetch_add(&gHABacked, 1); atomic_fetch_add(&gHLazyHeaps, 1); atomic_fetch_add(&gHLazyReserved, size);
            nvlog("heap lazy backing: %s heap of %lu bytes = %u slots, memory on first use", shared ? "shared" : "private", (unsigned long)size, h->_lzslots);
            return;
        }
    }
    if (nvmtl_vk_heap_create(size, shared, &h->_hm)) {
        nvlog("heap alias: an automatic %s heap of %lu bytes got no memory of its own - its resources take the legacy fresh path",
              shared ? "shared" : "private", (unsigned long)size);
        return;
    }
    nvmtl_vk_heap_zero(&h->_hm, shared ? "shared" : "private");
    h->_fl = OS_UNFAIR_LOCK_INIT; h->_nfr = 0; h->_cfr = 0; h->_fr = NULL;
    if (nvmtl_heap_grow(h)) { nvmtl_vk_heap_destroy(&h->_hm); nvlog("heap alias: no free list for a %lu-byte heap - legacy path", (unsigned long)size); return; }
    h->_fr[0] = 0; h->_fr[1] = size; h->_nfr = 1; h->_backed = YES;
    atomic_fetch_add(&gHABacked, 1); atomic_fetch_add(&gHABackedBytes, size);
}
@implementation NVMTLHeap
- (void)nvmtlGiveRange:(NSUInteger)off length:(NSUInteger)len {
    const char *why = nvmtl_heap_give(self, off, len);
    if (why) { nvlog("heap alias: range %lu+%lu NOT given back - %s", (unsigned long)off, (unsigned long)len, why); return; }
    atomic_fetch_add(&gHAGiven, 1); atomic_fetch_sub(&gHALive, len);
}
- (id<MTLBuffer>)nvmtlPlacedBuffer:(NSUInteger)len options:(MTLResourceOptions)opt charge:(NSUInteger)charge {
    MTLStorageMode sm = (MTLStorageMode)((opt >> MTLResourceStorageModeShift) & 0xF);
    if (sm != _storage) {
        static int said; if (said++ < 2) nvlog("heap alias: a storage-%d buffer from a storage-%d heap - legacy path", (int)sm, (int)_storage);
        return nil;
    }
    NSUInteger al = [_dev heapBufferSizeAndAlignWithLength:len options:opt].align;
    NSUInteger off = nvmtl_heap_take(self, charge, al);
    if (off == NSUIntegerMax) {
        nvlog("heap alias: no free range of %lu (align %lu) in a %lu-byte heap with %lu charged - legacy path",
              (unsigned long)charge, (unsigned long)al, (unsigned long)_size, (unsigned long)_used);
        return nil;
    }
    NVMTLBuffer *b = [NVMTLBuffer new];
    b->_storage = sm;
    b->_ropt = opt & ((MTLResourceOptions)0xF | ((MTLResourceOptions)0x3 << MTLResourceHazardTrackingModeShift));
    NSUInteger rel = off; nvk_heap *mem = nvmtl_heap_mem_for(self, off, charge, &rel);
    if (!mem || nvmtl_vk_buffer_create_placed(len, mem, rel, &b->_b)) {
        const char *why = nvmtl_heap_give(self, off, charge);
        nvlog("heap alias: bind of %lu at %lu FAILED - legacy path%s%s", (unsigned long)len, (unsigned long)off, why ? "; range kept out: " : "", why ? why : "");
        return nil;
    }
    b->_subHeap = self; b->_subCharge = charge; b->_subPlaced = YES; b->_heapOffset = off; _used += charge;
    nvmtl_heap_register(self, b);
    atomic_fetch_add(&gHAPlaced, 1); atomic_fetch_add(&gHALive, charge);
    nvmtl_heap_census("placed");
    return b;
}
- (unsigned long long)protectionOptions { return 0; }
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSUInteger)size { return _size; }
- (NSUInteger)usedSize { return _used; }
- (NSUInteger)currentAllocatedSize { return _size; }
- (MTLStorageMode)storageMode { return _storage; }
- (MTLCPUCacheMode)cpuCacheMode { return _cache; }
- (MTLHeapType)type { return _type; }
- (MTLHazardTrackingMode)hazardTrackingMode { return _hazard == MTLHazardTrackingModeTracked ? MTLHazardTrackingModeTracked : MTLHazardTrackingModeUntracked; }
- (MTLResourceOptions)resourceOptions { return ((MTLResourceOptions)_storage << MTLResourceStorageModeShift) | (MTLResourceOptions)_cache
                                            | ((MTLResourceOptions)[self hazardTrackingMode] << MTLResourceHazardTrackingModeShift); }
- (NSUInteger)allocatedSize { return _size; }
- (void)dealloc { for (uint32_t k = 0; k < _npk; k++) nvmtl_vk_image_destroy(&_pk[k].img);
                  free(_pk); _pk = NULL; _npk = 0;
                  nvmtl_vk_heap_destroy(&_hm); free(_fr); _fr = NULL;
                  for (uint32_t k = 0; k < _lzn; k++) nvmtl_vk_heap_destroy(&_lzm[k]);
                  free(_lzm); free(_lzb); free(_lzo); _lzm = NULL; _lzb = NULL; _lzo = NULL; _lzn = 0; }
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)opt offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { nvlog("heap newBufferWithLength:offset: on a non-placement heap"); return nil; }
    NVMTLBuffer *b = [NVMTLBuffer new];
    b->_storage = (MTLStorageMode)((opt >> MTLResourceStorageModeShift) & 0xF);
    if (nvmtl_vk_buffer_create_placed(len, &_hm, off, &b->_b)) { nvlog("heap newBufferWithLength:%lu offset:%lu FAILED", (unsigned long)len, (unsigned long)off); return nil; }
    b->_heap = self; b->_heapOffset = off; if (off + len > _used) _used = off + len;
    nvmtl_heap_register(self, b);
    nvlog("heap newBufferWithLength:%lu offset:%lu -> %p", (unsigned long)len, (unsigned long)off, (__bridge void *)b);
    return b;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d offset:(NSUInteger)off {
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    extern int nvmtl_depthfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *aspect);
    extern nvk_queue *nvmtl_device_queue(void);
    if (_type != MTLHeapTypePlacement) { nvlog("heap newTextureWithDescriptor:offset: on a non-placement heap"); return nil; }
    if (!d) return nil;
    NVMTLTexture *t = [NVMTLTexture new];
    t->_fmt = d.pixelFormat; t->_mips = (uint32_t)(d.mipmapLevelCount ? d.mipmapLevelCount : 1);
    t->_usage = d.usage;
    uint32_t dvk = 0, dasp = 0;
    if (nvmtl_depthfmt_public(d.pixelFormat, &dvk, &dasp) == 0) {
        if (nvmtl_vk_depth_create_ex_placed((uint32_t)d.width, (uint32_t)d.height, dvk, dasp, &_hm, off, &t->_i)) { nvlog("heap depth texture FAILED (format %lu offset %lu)", (unsigned long)d.pixelFormat, (unsigned long)off); return nil; }
        t->_q = nvmtl_device_queue(); t->_heap = self; t->_heapOffset = off;
        nvmtl_heap_register(self, t);
        nvlog("heap newTextureWithDescriptor: %lux%lu DEPTH offset:%lu -> %p", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off, (__bridge void *)t);
        return t;
    }
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8)) return nil;
    int wantStex = (d.usage & MTLTextureUsageShaderWrite) != 0;
    if (nvmtl_vk_image_create_typed_placed((uint32_t)d.width, (uint32_t)d.height, vkf, bpp, a8, (uint32_t)d.mipmapLevelCount, wantStex,
                                            (uint32_t)d.textureType, nvmtl_vk_layers_for_desc((uint32_t)d.textureType, (uint32_t)d.arrayLength, (uint32_t)d.depth), &_hm, off, &t->_i)) {
        nvlog("heap newTextureWithDescriptor: %lux%lu offset:%lu FAILED", (unsigned long)d.width, (unsigned long)d.height, (unsigned long)off); return nil; }
    t->_q = nvmtl_device_queue(); t->_stex = t->_i.storage == 1; t->_heap = self; t->_heapOffset = off;
    nvmtl_heap_register(self, t);
    nvmtl_texture_desc_swizzle(t, d);
    nvlog("heap newTextureWithDescriptor: %lux%lu%s offset:%lu -> %p", (unsigned long)d.width, (unsigned long)d.height, t->_stex ? " +storage" : "", (unsigned long)off, (__bridge void *)t);
    return t;
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s { return nvmtl_purge(&_purge, s); }
- (void)nvmtlReleaseSubAllocation:(NSUInteger)bytes {
    if (bytes > _used) {
        nvlog("heap release %lu but only %lu charged - accounting underflow, clamped to 0", (unsigned long)bytes, (unsigned long)_used);
        _used = 0; return;
    }
    _used -= bytes;
}
- (NSUInteger)maxAvailableSizeWithAlignment:(NSUInteger)a {
    NSUInteger left = _size > _used ? _size - _used : 0;
    return (a > 1 && left % a) ? left - (left % a) : left;
}
- (id<MTLBuffer>)newBufferWithLength:(NSUInteger)len options:(MTLResourceOptions)opt {
    NSUInteger charge = [_dev heapBufferSizeAndAlignWithLength:len options:opt].size;
    if (charge < len) charge = len;
    if (charge > [self maxAvailableSizeWithAlignment:0] && nvmtl_heap_unkeep(self)) nvlog("heap newBufferWithLength:%lu: out of room - let the kept heap pin go (%lu left now)", (unsigned long)len, (unsigned long)[self maxAvailableSizeWithAlignment:0]);
    if (charge > [self maxAvailableSizeWithAlignment:0]) { nvlog("heap newBufferWithLength:%lu (charge %lu) — only %lu left of %lu", (unsigned long)len, (unsigned long)charge, (unsigned long)(_size > _used ? _size - _used : 0), (unsigned long)_size); return nil; }
    if (_backed) { id<MTLBuffer> pb = [self nvmtlPlacedBuffer:len options:opt charge:charge]; if (pb) return pb;
                   if (nvmtl_heap_unkeep(self) && (pb = [self nvmtlPlacedBuffer:len options:opt charge:charge])) return pb;
                   atomic_fetch_add(&gHAFallback, 1); nvmtl_heap_census("fallback"); }
    gZeroSkip = 1; id<MTLBuffer> b = [_dev newBufferWithLength:len options:opt]; gZeroSkip = 0;
    if (!b) return nil;
    if (![(id)b isKindOfClass:[NVMTLBuffer class]]) {
        static int said; if (said++ < 2) nvlog("heap sub-allocation is a %s, not ours — UNCHARGED, usedSize will under-report", [NSStringFromClass([(id)b class]) UTF8String]);
        return b;
    }
    NVMTLBuffer *nb = (NVMTLBuffer *)b; nb->_subHeap = self; nb->_subCharge = charge; _used += charge;
    nvmtl_heap_register(self, nb);
    return b;
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size {
    NSUInteger charge = [_dev heapAccelerationStructureSizeAndAlignWithSize:size].size;
    if (charge < size) charge = size;
    if (charge > [self maxAvailableSizeWithAlignment:0]) nvmtl_heap_unkeep(self);
    if (charge > [self maxAvailableSizeWithAlignment:0]) { nvlog("heap newAccelerationStructureWithSize:%lu (charge %lu) - only %lu left of %lu", (unsigned long)size, (unsigned long)charge, (unsigned long)(_size > _used ? _size - _used : 0), (unsigned long)_size); return nil; }
    id<MTLAccelerationStructure> as = [_dev newAccelerationStructureWithSize:size];
    if (!as) return nil;
    if (![(id)as isKindOfClass:[NVMTLAccelerationStructure class]]) {
        static int said; if (said++ < 2) nvlog("heap structure is a %s, not ours - UNCHARGED, usedSize will under-report", [NSStringFromClass([(id)as class]) UTF8String]);
        return as;
    }
    NVMTLAccelerationStructure *na = (NVMTLAccelerationStructure *)as; na->_subHeap = self; na->_subCharge = charge; _used += charge;
    nvmtl_heap_register(self, na);
    nvlog("heap newAccelerationStructureWithSize:%lu (charge %lu) -> %p", (unsigned long)size, (unsigned long)charge, (__bridge void *)na);
    return as;
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)desc {
    MTLAccelerationStructureSizes s = [_dev accelerationStructureSizesWithDescriptor:desc];
    return s.accelerationStructureSize ? [self newAccelerationStructureWithSize:s.accelerationStructureSize] : nil;
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithSize:(NSUInteger)size offset:(NSUInteger)off {
    if (_type != MTLHeapTypePlacement) { nvlog("heap newAccelerationStructureWithSize:offset: on a non-placement heap"); return nil; }
    if (nvmtl_vk_init() || !nvmtl_vk_rt_available()) { nvlog("heap newAccelerationStructureWithSize:offset: no ray tracing on this NVK"); return nil; }
    if (off & 255) { nvlog("heap newAccelerationStructureWithSize:%lu offset:%lu - offset is not 256-aligned (heapAccelerationStructureSizeAndAlign says 256) - REFUSED", (unsigned long)size, (unsigned long)off); return nil; }
    const size_t len = (size < 256 ? 256 : size) + NVMTL_AS_HEADER;   // the Metal header, then the Vulkan structure
    NVMTLAccelerationStructure *a = [NVMTLAccelerationStructure new];
    a->_storage = MTLStorageModePrivate;
    if (nvmtl_vk_buffer_create_placed(len, &_hm, off, &a->_b)) { nvlog("heap newAccelerationStructureWithSize:%lu offset:%lu FAILED", (unsigned long)size, (unsigned long)off); return nil; }
    a->_heap = self; a->_heapOffset = off; if (off + len > _used) _used = off + len;
    nvmtl_heap_register(self, a);
    nvlog("heap newAccelerationStructureWithSize:%lu offset:%lu -> %p", (unsigned long)size, (unsigned long)off, (__bridge void *)a);
    return a;
}
- (id<MTLAccelerationStructure>)newAccelerationStructureWithDescriptor:(MTLAccelerationStructureDescriptor *)desc offset:(NSUInteger)off {
    MTLAccelerationStructureSizes s = [_dev accelerationStructureSizesWithDescriptor:desc];
    return s.accelerationStructureSize ? [self newAccelerationStructureWithSize:s.accelerationStructureSize offset:off] : nil;
}
- (id<MTLTexture>)nvmtlPlacedTexture:(MTLTextureDescriptor *)d charge:(NSUInteger)charge align:(NSUInteger)al {
    extern int nvmtl_pixfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *bpp, int *a8);
    extern int nvmtl_depthfmt_public(MTLPixelFormat f, uint32_t *vk, uint32_t *aspect);
    extern nvk_queue *nvmtl_device_queue(void);
    static int noPlace = -1, noRecycle = -1;
    if (noPlace < 0) { noPlace = getenv("NVMTL_NO_HEAPTEX_PLACE") != NULL; noRecycle = getenv("NVMTL_NO_HEAPTEX_RECYCLE") != NULL;
        nvlog("heap texture: dres R1 placement %s, recycle %s", noPlace ? "OFF (NVMTL_NO_HEAPTEX_PLACE)" : "ON",
              noRecycle ? "OFF (NVMTL_NO_HEAPTEX_RECYCLE)" : "ON"); }
    if (noPlace) return nil;
    if (_storage != MTLStorageModePrivate || d.storageMode != MTLStorageModePrivate
        || d.textureType == MTLTextureType2DMultisample || d.sampleCount > 1
        || !d.width || !d.height || d.width > UINT32_MAX || d.height > UINT32_MAX) return nil;
    uint32_t dvk = 0, dasp = 0, vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_depthfmt_public(d.pixelFormat, &dvk, &dasp) == 0 || nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8)) return nil;
    const uint32_t w = (uint32_t)d.width, hh = (uint32_t)d.height, mips = (uint32_t)(d.mipmapLevelCount ? d.mipmapLevelCount : 1);
    const uint32_t type = (uint32_t)d.textureType, layers = nvmtl_vk_layers_for_desc(type, (uint32_t)d.arrayLength, (uint32_t)d.depth);
    const int wantStex = (d.usage & MTLTextureUsageShaderWrite) != 0;
    const uint32_t key[12] = { 1, w, hh, vkf, bpp, (uint32_t)a8, mips, (uint32_t)wantStex, type, layers, 0, 0 };
    NVMTLTexture *t = [NVMTLTexture new];
    NSUInteger off = 0, len = charge; BOOL rec = NO;
    if (!noRecycle && _npk && nvmtl_heap_unpark(self, key, &t->_i, &off, &len) == 0) {
        rec = YES; t->_i.layout = 0;
    } else {
        off = nvmtl_heap_take(self, charge, al);
        if (off == NSUIntegerMax) {
            static int said; if (said++ < 4) nvlog("heap texture: no free range of %lu (align %lu) in a %lu-byte heap with %lu charged - legacy path",
                                                   (unsigned long)charge, (unsigned long)al, (unsigned long)_size, (unsigned long)_used);
            return nil; }
        NSUInteger rel = off; nvk_heap *mem = nvmtl_heap_mem_for(self, off, charge, &rel);
        if (!mem || nvmtl_vk_image_create_typed_placed(w, hh, vkf, bpp, a8, mips, wantStex, type, layers, mem, rel, &t->_i)
            || !nvmtl_vk_image_placed_fits(&t->_i, charge)) {
            if (t->_i.img) { nvmtl_vk_image_destroy(&t->_i); atomic_fetch_add(&gHTMisfit, 1); }
            const char *why = nvmtl_heap_give(self, off, charge);
            nvlog("heap texture: %ux%u fmt %lu at %lu NOT placed - legacy path%s%s", w, hh, (unsigned long)d.pixelFormat, (unsigned long)off,
                  why ? "; range kept out: " : "", why ? why : "");
            return nil; }
    }
    t->_ropt = (MTLResourceOptions)d.cpuCacheMode | ((MTLResourceOptions)d.hazardTrackingMode << MTLResourceHazardTrackingModeShift)
             | ((MTLResourceOptions)d.storageMode << MTLResourceStorageModeShift);
    t->_fmt = d.pixelFormat; t->_mips = mips; t->_usage = d.usage;
    t->_q = nvmtl_device_queue(); t->_stex = t->_i.storage == 1;
    t->_subHeap = self; t->_subCharge = len; t->_subPlaced = YES; t->_heapOffset = off;
    nvmtl_heap_register(self, t);
    t->_parkable = !noRecycle; memcpy(t->_pkey, key, sizeof key); t->_pkLen = len;
    _used += len;
    atomic_fetch_add(&gHAPlaced, 1); atomic_fetch_add(&gHALive, len);
    atomic_fetch_add(rec ? &gHTRecycled : &gHTPlaced, 1); nvmtl_heaptex_census(rec ? "recycled" : "placed");
    // automatic heaps land here, not in -newTextureWithDescriptor:offset:; without this the descriptor's swizzle was lost
    nvmtl_texture_desc_swizzle(t, d);
    return t;
}
- (id<MTLTexture>)newTextureWithDescriptor:(MTLTextureDescriptor *)d {
    if (!d) return nil;
    MTLSizeAndAlign sa = [_dev heapTextureSizeAndAlignWithDescriptor:d];
    NSUInteger charge = sa.size;
    if (_backed && charge) { id<MTLTexture> pt = [self nvmtlPlacedTexture:d charge:charge align:sa.align]; if (pt) return pt;
                             if (nvmtl_heap_unkeep(self) && (pt = [self nvmtlPlacedTexture:d charge:charge align:sa.align])) return pt;
                             atomic_fetch_add(&gHTFallback, 1); nvmtl_heaptex_census("fallback"); }
    if (!charge) charge = d.width * d.height * 4;
    id<MTLTexture> t = [_dev newTextureWithDescriptor:d];
    if (!t) return nil;
    if (![(id)t isKindOfClass:[NVMTLTexture class]]) {
        static int said; if (said++ < 2) nvlog("heap sub-allocation is a %s, not ours — UNCHARGED, usedSize will under-report", [NSStringFromClass([(id)t class]) UTF8String]);
        return t;
    }
    NVMTLTexture *nt = (NVMTLTexture *)t; nt->_subHeap = self; nt->_subCharge = charge; _used += charge;
    nvmtl_heap_register(self, nt);
    return t;
}
@end

@implementation NVMTLDepthStencilState
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (void)setLabel:(NSString *)l {}
- (BOOL)readsDepth { return _compare != 7 || _write; }
- (BOOL)writesDepth { return _write != 0; }
- (BOOL)readsStencil { return _stencil != 0; }
- (BOOL)writesStencil { if (!_stencil) return NO; for (int k = 1; k <= 3; k++) if ((_sf[k] && _sf[5]) || (_sb[k] && _sb[5])) return YES; return NO; }
// No handle: a depth-stencil state reaches a draw only as the object itself (the encoder, and an indirect command's
// setDepthStencilState:, keep the object); nothing here reads a depth-stencil handle out of an argument buffer, so a
// token would name a state no GPU-side path could find. 0 is what the forwarding net answered before.
- (MTLResourceID)gpuResourceID API_AVAILABLE(macos(26.0)) { MTLResourceID r; r._impl = 0; return r; }
@end

@implementation NVMTLSamplerState
- (MTLResourceID)gpuResourceID {
    @synchronized (self) { if (!_resid) _resid = nvmtl_resid_for_sampler(self); }
    MTLResourceID rid; rid._impl = _resid; return rid;
}
- (void)dealloc { nvmtl_vk_sampler_destroy(&_s); }
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
@end

#pragma mark - compute / blit
@implementation NVMTLComputePipelineState
- (void)dealloc { nvmtl_vk_pipeline_destroy(&_p); }
- (BOOL)supportIndirectCommandBuffers { return _icb; }
- (MTLResourceID)gpuResourceID {
    MTLResourceID r; r._impl = _icb ? nvmtl_resid_pipe(self, &_resid) : 0; return r; }
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { return 0; }
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (MTLShaderValidation)shaderValidation { return MTLShaderValidationDisabled; }
// nil and 0 for the reasons given at NVMTLRenderPipelineState: no MTL4Compiler to ask for reflection, and the kernel's
// code lives in the Vulkan driver's memory, whose size Vulkan does not report.
- (MTLComputePipelineReflection *)reflection API_AVAILABLE(macos(26.0)) { return nil; }
- (NSUInteger)allocatedSize { return 0; }
static NSUInteger nvmtl_env_uint(NSString *k, NSUInteger dflt) {
    NSString *s = [[NSProcessInfo processInfo] environment][k];
    return s.length ? (NSUInteger)s.integerValue : dflt;
}
static NSUInteger nvmtl_compute_maxt(void) { static NSUInteger v; if (!v) v = nvmtl_env_uint(@"NVMTL_MAXT", 1024); return v; }
// A required threadgroup size fixes the total too: Apple answers 64 for a pipeline that requires 64x1x1.
- (NSUInteger)maxTotalThreadsPerThreadgroup { NSUInteger v = nvmtl_compute_maxt(), n = _attrT ?: _askT;
    NSUInteger rq = _reqTG.width * _reqTG.height * _reqTG.depth; if (rq && (!n || rq < n)) n = rq;
    return n && n < v ? n : v; }
- (MTLSize)requiredThreadsPerThreadgroup { return _reqTG; }
- (NSUInteger)threadExecutionWidth { static NSUInteger v; if (!v) { const uint32_t f = nvmtl_vk_cs_simd(); v = f ? f : nvmtl_env_uint(@"NVMTL_TEW", 32); } return v; }
// Static only, as Apple's: the function's own threadgroup variables, not setThreadgroupMemoryLength:atIndex:'s memory.
- (NSUInteger)staticThreadgroupMemoryLength { return _staticTG; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l {}
@end

@implementation NVMTLIndirectCommand
- (void)nvmtlRecordChanged { if (_owner && _owner->_rec) [_owner nvmtlWriteRecord:_slot]; }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)ps { _ps = ps; if (!_armed) [self nvmtlRecordChanged]; }
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (!_vbufs) _vbufs = [NSMutableArray new];
    if (b) [_vbufs addObject:@[b, @(o), @(i)]];
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    if (!_vbufs) _vbufs = [NSMutableArray new];
    if (b) [_vbufs addObject:@[b, @(o), @(i), @(st)]];
}
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (!_fbufs) _fbufs = [NSMutableArray new];
    if (b) [_fbufs addObject:@[b, @(o), @(i)]];
}
- (void)drawPrimitives:(MTLPrimitiveType)type vertexStart:(NSUInteger)s vertexCount:(NSUInteger)c
         instanceCount:(NSUInteger)n baseInstance:(NSUInteger)bi {
    _type = type; _start = s; _count = c; _instances = n; _baseInstance = bi; _indexed = NO; _armed = YES;
    [self nvmtlRecordChanged];
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)type indexCount:(NSUInteger)c indexType:(MTLIndexType)it
                  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)io
                instanceCount:(NSUInteger)n baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    _type = type; _count = c; _itype = it; _ibuf = ib; _ioff = io;
    _instances = n; _baseVertex = bv; _baseInstance = bi; _indexed = YES; _armed = YES;
    [self nvmtlRecordChanged];
}
- (void)setComputePipelineState:(id<MTLComputePipelineState>)ps { _cps = ps; _isCompute = YES; }
- (void)setKernelBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    if (!_kbufs) _kbufs = [NSMutableArray new];
    if (b) [_kbufs addObject:@[b, @(o), @(i)]];
}
- (void)concurrentDispatchThreadgroups:(MTLSize)grid threadsPerThreadgroup:(MTLSize)tg {
    _grid = grid; _tg = tg; _isCompute = YES; _armed = YES;
}
- (void)concurrentDispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tg {
    MTLSize g = { (threads.width + tg.width - 1) / tg.width,
                  (threads.height + tg.height - 1) / tg.height,
                  (threads.depth + tg.depth - 1) / tg.depth };
    [self concurrentDispatchThreadgroups:g threadsPerThreadgroup:tg];
}
- (void)setBarrier {}
- (void)clearBarrier {}
- (void)setThreadgroupMemoryLength:(NSUInteger)len atIndex:(NSUInteger)i {
    if (len) nvlog("indirect command setThreadgroupMemoryLength:%lu — dynamic threadgroup memory is not wired; IGNORED", (unsigned long)len);
}
- (void)setStageInRegion:(MTLRegion)r {}
- (void)setCullMode:(MTLCullMode)m { _cull = m; _hasCull = YES; }
- (void)setFrontFacingWinding:(MTLWinding)w { _winding = w; _hasWinding = YES; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)f { _fill = f; _hasFill = YES; }
- (void)setDepthClipMode:(MTLDepthClipMode)c { _clip = c; _hasClip = YES; }
- (void)setDepthStencilState:(id<MTLDepthStencilState>)d { _ds = d; }
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)c { _bias = b; _slope = s; _clamp = c; _hasBias = YES; }
- (void)nvmtlCopyFrom:(NVMTLIndirectCommand *)o {
    if (!o || o == self) return;
    _ps = o->_ps; _vbufs = [o->_vbufs mutableCopy]; _fbufs = [o->_fbufs mutableCopy];
    _type = o->_type; _start = o->_start; _count = o->_count; _instances = o->_instances; _baseInstance = o->_baseInstance;
    _ibuf = o->_ibuf; _ioff = o->_ioff; _itype = o->_itype; _baseVertex = o->_baseVertex; _indexed = o->_indexed; _armed = o->_armed;
    _cull = o->_cull; _hasCull = o->_hasCull; _winding = o->_winding; _hasWinding = o->_hasWinding; _fill = o->_fill; _hasFill = o->_hasFill;
    _clip = o->_clip; _hasClip = o->_hasClip; _ds = o->_ds; _bias = o->_bias; _slope = o->_slope; _clamp = o->_clamp; _hasBias = o->_hasBias;
    _cps = o->_cps; _kbufs = [o->_kbufs mutableCopy]; _grid = o->_grid; _tg = o->_tg; _isCompute = o->_isCompute;
    [self nvmtlRecordChanged];
}
- (void)reset {
    _ps = nil; [_vbufs removeAllObjects]; [_fbufs removeAllObjects]; _ibuf = nil; _armed = NO;
    _hasCull = _hasWinding = _hasFill = _hasClip = _hasBias = NO; _ds = nil;
    _cps = nil; [_kbufs removeAllObjects]; _isCompute = NO;
    [self nvmtlRecordChanged];
}
@end

@implementation NVMTLIndirectCommandBuffer
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSUInteger)size { return _cmds.count; }
// A shader's command_buffer is this address (nvmtl_icb_layout.h): taking the ID makes the ICB GPU-encodable. Without a
// record buffer (its allocation refused, logged once) the old opaque ID stays and the ICB keeps the CPU replay; a shader
// that encodes into such an ICB writes through an address no buffer holds.
- (MTLResourceID)gpuResourceID { MTLResourceID r; const uint64_t a = [self nvmtlRecordAddress]; r._impl = a ? a : nvmtl_resid_pipe(self, &_resid); return r; }
- (uint64_t)nvmtlRecordAddress {
    @synchronized (self) {
        if (!_rec && _cmds.count) {
            NVMTLBuffer *b = [NVMTLBuffer new];
            const size_t bytes = _cmds.count * (size_t)NVMTL_ICB_RECORD_BYTES;
            // VRAM through the BAR (host_visible 3) when there is room, else system RAM: each indirect draw of the execute
            // loads its parameters from here one register at a time, and a system-RAM read costs a PCIe round trip each
            // (mtl_icb_cost: ~5 us per indirect draw from system RAM). NVMTL_ICB_SYSRAM=1 keeps them in system RAM.
            static int sysram = -1; if (sysram < 0) { const char *e = getenv("NVMTL_ICB_SYSRAM"); sysram = e && e[0] == '1'; }
            nvmtl_icb_record *shadow = calloc(_cmds.count, sizeof *shadow);
            if (!shadow || nvmtl_vk_buffer_create(bytes, sysram ? 1 : 3, &b->_b) || !b->_b.map) {
                free(shadow);
                static int said; if (!said++) nvlog("indirect command buffer: the %zu-byte slot record buffer was REFUSED - GPU-side encoding into it is unavailable, CPU replay only", bytes);
                return 0;
            }
            b->_storage = MTLStorageModeShared;
            for (NSUInteger i = 0; i < _cmds.count; i++) shadow[i].execPrim = NVMTL_ICB_NO_PRIM;
            memcpy(b->_b.map, shadow, bytes);
            _shadow = shadow;
            _rec = b;
            for (NSUInteger i = 0; i < _cmds.count; i++) [self nvmtlWriteRecord:i];
            nvlog("indirect command buffer %p: %lu slot record(s) at 0x%llx - GPU-encodable, executed from the records (vkCmdDraw*Indirect)",
                  (__bridge void *)self, (unsigned long)_cmds.count, (unsigned long long)nvmtl_vk_buffer_address(&b->_b));
        }
    }
    return _rec ? nvmtl_vk_buffer_address(&_rec->_b) : 0;
}
- (nvmtl_icb_record *)nvmtlRecords { return _rec ? _shadow : NULL; }
- (nvmtl_icb_record *)nvmtlGPURecords { return _rec ? (nvmtl_icb_record *)_rec->_b.map : NULL; }
// the execute's topology for slot i, in the CPU's copy and in the record a shader checks its draw against (word 11)
- (void)nvmtlSetExecPrim:(NSUInteger)i prim:(uint32_t)prim {
    if (!_rec || i >= _cmds.count) return;
    _shadow[i].execPrim = prim;
    ((nvmtl_icb_record *)_rec->_b.map)[i].execPrim = prim;
}
- (void)dealloc { free(_shadow); }
- (MTLStorageMode)storageMode { return (MTLStorageMode)((_opts >> MTLResourceStorageModeShift) & 0xF); }
// The record of slot i from what the CPU encoded there (indirectRenderCommandAtIndex:): the draw, its topology and its
// index buffer. A slot without a CPU draw keeps the topology an earlier execute or its pipeline chose, so a shader that
// encodes into it before the next execute finds one (nvmtl_icb_layout.h word 11).
- (void)nvmtlWriteRecord:(NSUInteger)i {
    nvmtl_icb_record *rec = [self nvmtlRecords];
    if (!rec || i >= _cmds.count) return;
    NVMTLIndirectCommand *c = _cmds[i];
    nvmtl_icb_record v; memset(&v, 0, sizeof v);
    v.execPrim = rec[i].execPrim;
    if (c->_ps && [(id)c->_ps isKindOfClass:[NVMTLRenderPipelineState class]])
        v.execPrim = nvmtl_icb_prim_for_class((unsigned long)((NVMTLRenderPipelineState *)c->_ps)->_descriptor.inputPrimitiveTopology);
    if (c->_armed && !c->_isCompute) {
        v.prim = v.execPrim = (uint32_t)c->_type;
        if (c->_indexed) {
            NVMTLBuffer *ib = [(id)c->_ibuf isKindOfClass:[NVMTLBuffer class]] ? (NVMTLBuffer *)c->_ibuf : nil;
            const uint32_t sh = c->_itype == MTLIndexTypeUInt32 ? 2 : 1;
            const size_t bindOff = c->_ioff & ((1u << sh) - 1u);   // Metal offsets are index-aligned: 0 in practice
            const uint64_t addr = ib ? nvmtl_vk_buffer_address(&ib->_b) : 0;
            if (addr && c->_ioff <= ib->_b.size) {
                const uint64_t base = addr + bindOff, room = ib->_b.size - bindOff;
                v.kind = NVMTL_ICB_INDEXED;
                v.idx[0] = (uint32_t)c->_count; v.idx[1] = (uint32_t)c->_instances; v.idx[2] = (uint32_t)((c->_ioff - bindOff) >> sh);
                v.idx[3] = (uint32_t)(int32_t)c->_baseVertex; v.idx[4] = (uint32_t)c->_baseInstance;
                v.ibLo = (uint32_t)base; v.ibHi = (uint32_t)(base >> 32); v.ibLimit = room > 0xffffffffull ? 0xffffffffu : (uint32_t)room; v.ibShift = sh;
            }
        } else {
            v.kind = NVMTL_ICB_DRAW;
            v.draw[0] = (uint32_t)c->_count; v.draw[1] = (uint32_t)c->_instances; v.draw[2] = (uint32_t)c->_start; v.draw[3] = (uint32_t)c->_baseInstance;
        }
    }
    rec[i] = v;
    memcpy(&((nvmtl_icb_record *)_rec->_b.map)[i], &v, sizeof v);
}
- (MTLCPUCacheMode)cpuCacheMode { return (MTLCPUCacheMode)(_opts & 0xF); }
- (MTLHazardTrackingMode)hazardTrackingMode { return nvmtl_res_hazard(nil, _opts); }
- (MTLResourceOptions)resourceOptions { return ((MTLResourceOptions)[self storageMode] << MTLResourceStorageModeShift) | (MTLResourceOptions)[self cpuCacheMode]
                                            | ((MTLResourceOptions)[self hazardTrackingMode] << MTLResourceHazardTrackingModeShift); }
- (id<MTLHeap>)heap { return nil; }
- (NSUInteger)heapOffset { return 0; }
- (BOOL)isAliasable { return NO; }
- (void)makeAliasable {}
- (NSUInteger)allocatedSize { return 0; }
- (kern_return_t)setOwnerWithIdentity:(task_id_token_t)t { return nvmtl_owner_check(t); }
- (id<MTLIndirectComputeCommand>)indirectComputeCommandAtIndex:(NSUInteger)i {
    if (i >= _cmds.count) { nvlog("indirectComputeCommandAtIndex:%lu — this buffer holds %lu", (unsigned long)i, (unsigned long)_cmds.count); return nil; }
    return _cmds[i];
}
- (id<MTLIndirectRenderCommand>)indirectRenderCommandAtIndex:(NSUInteger)i {
    if (i >= _cmds.count) { nvlog("indirectRenderCommandAtIndex:%lu — this buffer holds %lu", (unsigned long)i, (unsigned long)_cmds.count); return nil; }
    return _cmds[i];
}
- (void)resetWithRange:(NSRange)r {
    for (NSUInteger i = r.location; i < r.location + r.length && i < _cmds.count; i++) [_cmds[i] reset];
}
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s { return nvmtl_purge(&_purge, s); }
@end

enum { NVMTL_E78_H3 = 1, NVMTL_E78_H4 = 2, NVMTL_E78_H5 = 4, NVMTL_E78_P1 = 8, NVMTL_E78_H7 = 16 };
enum { E78C_ENC, E78C_DISP, E78C_FULL, E78C_NARROW, E78C_NONE, E78C_ENDFULL, E78C_SETNEW, E78C_SETREUSE, E78C_WRITES,
       E78C_RING, E78C_RINGBYTES, E78C_RINGCHUNK, E78C_BYTESOLD, E78C_VARHIT, E78C_VARMISS, E78C_CONC, E78C_N };
static uint64_t g_e78c[E78C_N];
static int g_e78_off = -1, g_e78_census = -1;
static int nvmtl_enc78_on(int row) {
    int off = __atomic_load_n(&g_e78_off, __ATOMIC_RELAXED);
    if (off < 0) {
        const char *e = getenv("NVMTL_ENC78_OFF"); off = 0;
        if (e) {
            if (strstr(e, "all")) off = 31;
            if (strstr(e, "h3")) off |= NVMTL_E78_H3;
            if (strstr(e, "h4")) off |= NVMTL_E78_H4;
            if (strstr(e, "h5")) off |= NVMTL_E78_H5;
            if (strstr(e, "p1")) off |= NVMTL_E78_P1;
            if (strstr(e, "h7")) off |= NVMTL_E78_H7;
            nvlog("denc: NVMTL_ENC78_OFF=%s -> rows off 0x%x", e, off);
        }
        __atomic_store_n(&g_e78_off, off, __ATOMIC_RELAXED);
    }
    return !(off & row);
}
// counted only with NVMTL_ENC78_CENSUS set (the counters are printed nowhere else): a locked add per snapshot, setBytes and
// dispatch was the first atomic after each draw's write-combined batch writes and paid their drain (stage 1d)
static void nvmtl_enc78_count(int k, uint64_t n) {
    int c = __atomic_load_n(&g_e78_census, __ATOMIC_RELAXED);
    if (c < 0) { const char *e = getenv("NVMTL_ENC78_CENSUS"); c = e ? atoi(e) : 0; __atomic_store_n(&g_e78_census, c, __ATOMIC_RELAXED); }
    if (c && k >= 0 && k < E78C_N) __atomic_fetch_add(&g_e78c[k], n, __ATOMIC_RELAXED);
}
static void nvmtl_enc78_census(void) {
    if (g_e78_census < 0) { const char *e = getenv("NVMTL_ENC78_CENSUS"); g_e78_census = e ? atoi(e) : 0; }
    uint64_t n = __atomic_add_fetch(&g_e78c[E78C_ENC], 1, __ATOMIC_RELAXED);
    if (!g_e78_census || (g_e78_census < 2 && (n & (n - 1)))) return;
    uint64_t v[E78C_N]; for (int k = 0; k < E78C_N; k++) v[k] = __atomic_load_n(&g_e78c[k], __ATOMIC_RELAXED);
    fprintf(stderr, "ENC78 enc %llu disp %llu full %llu narrow %llu none %llu endfull %llu setnew %llu setreuse %llu writes %llu "
            "ring %llu ringbytes %llu ringchunk %llu bytesold %llu varhit %llu varmiss %llu conc %llu rowsoff 0x%x\n",
            v[E78C_ENC], v[E78C_DISP], v[E78C_FULL], v[E78C_NARROW], v[E78C_NONE], v[E78C_ENDFULL], v[E78C_SETNEW],
            v[E78C_SETREUSE], v[E78C_WRITES], v[E78C_RING], v[E78C_RINGBYTES], v[E78C_RINGCHUNK], v[E78C_BYTESOLD],
            v[E78C_VARHIT], v[E78C_VARMISS], v[E78C_CONC], g_e78_off < 0 ? 0 : g_e78_off);
}
void nvmtl_vk_cmd_barrier_c2c(nvk_cmdbuf *c);
@interface NVMTLCommandBuffer () { @public NVMTLBuffer *_e78Ring; size_t _e78RingOff; } @end
@interface NVMTLCommandBuffer () { @public BOOL _g79Owed, _g79C2C; NSMutableArray *_g79Sig, *_g79Promised; } @end
enum { NVMTL_G79_DUPIDX = 32, NVMTL_G79_DUPMAX = 64 };
@interface NVMTLCommandBuffer () { @public __unsafe_unretained NVMTLBuffer *_g79BRing[NVMTL_G79_DUPIDX]; size_t _g79BOff[NVMTL_G79_DUPIDX];
    uint32_t _g79BLen[NVMTL_G79_DUPIDX]; uint8_t _g79B[NVMTL_G79_DUPIDX][NVMTL_G79_DUPMAX]; } @end
enum { NVMTL_G79_BAR = 1, NVMTL_G79_ENC = 2, NVMTL_G79_EV = 4, NVMTL_G79_DUP = 8, NVMTL_G79_WFOLD = 16, NVMTL_G79_SUB = 32 };
enum { G79C_ENCNARROW, G79C_ENCFULL, G79C_BARLAZY, G79C_BARFOLD, G79C_OWEDPAID, G79C_SIGRIDE, G79C_WAITSELF, G79C_WAITHELD, G79C_WAITSPLIT, G79C_BYTESDUP, G79C_WAITFOLD, G79C_ENDS, G79C_N };
static uint64_t g_g79c[G79C_N];
static int g_g79_off = -1;
static int nvmtl_g79_on(int row) {
    int off = __atomic_load_n(&g_g79_off, __ATOMIC_RELAXED);
    if (off < 0) {
        const char *e = getenv("NVMTL_G79_OFF"); off = 0;
        if (e) {
            if (strstr(e, "all")) off = 63;
            if (strstr(e, "bar")) off |= NVMTL_G79_BAR;
            if (strstr(e, "enc")) off |= NVMTL_G79_ENC;
            if (strstr(e, "ev")) off |= NVMTL_G79_EV;
            if (strstr(e, "dup")) off |= NVMTL_G79_DUP;
            if (strstr(e, "wfold")) off |= NVMTL_G79_WFOLD;
            if (strstr(e, "sub")) off |= NVMTL_G79_SUB;
            nvlog("gsub: NVMTL_G79_OFF=%s -> rows off 0x%x", e, off);
        }
        __atomic_store_n(&g_g79_off, off, __ATOMIC_RELAXED);
    }
    return !(off & row);
}
static void nvmtl_g79_count(int k) { __atomic_fetch_add(&g_g79c[k], 1, __ATOMIC_RELAXED); }
static void nvmtl_g79_census(void) {
    static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_G79_CENSUS"); on = e ? atoi(e) : 0; }
    uint64_t n = __atomic_add_fetch(&g_g79c[G79C_ENDS], 1, __ATOMIC_RELAXED);
    if (!on || (on < 2 && (n & (n - 1)))) return;
    fprintf(stderr, "G79 ends %llu encnarrow %llu encfull %llu barlazy %llu barfold %llu owedpaid %llu sigride %llu waitself %llu "
            "waitheld %llu waitsplit %llu bytesdup %llu waitfold %llu rowsoff 0x%x\n",
            n, g_g79c[G79C_ENCNARROW], g_g79c[G79C_ENCFULL], g_g79c[G79C_BARLAZY], g_g79c[G79C_BARFOLD], g_g79c[G79C_OWEDPAID],
            g_g79c[G79C_SIGRIDE], g_g79c[G79C_WAITSELF], g_g79c[G79C_WAITHELD], g_g79c[G79C_WAITSPLIT],
            g_g79c[G79C_BYTESDUP], g_g79c[G79C_WAITFOLD], g_g79_off < 0 ? 0 : g_g79_off);
}
static void nvmtl_g79_flush(NVMTLCommandBuffer *cb) {
    if (!cb) return;
    if (cb->_g79Owed) { nvmtl_vk_cmd_barrier(&cb->_c); cb->_g79Owed = NO; nvmtl_g79_count(G79C_OWEDPAID); }
    cb->_g79C2C = NO;
}
@interface NVMTLComputeCommandEncoder () {
@public
    // Textures declared with useResource:usage:, keyed by their Vulkan image (a view and its root share one), latest
    // usage wins. An indirect dispatch reaches them only through argument buffers, so nothing else moves them into the
    // layout a shader access needs; see nvmtlPrepareIndirectImages.
    NSMutableDictionary *_indirectImages;
    NSData *_e78Key;
    NVMTLComputePipelineState *_e78VarFor, *_e78VarSel;
    NSUInteger _e78NDisp; BOOL _e78Concurrent, _e78Loose;
    BOOL _g79Bar;
}
@end
enum { NVMTL_E78_RING = 64 * 1024, NVMTL_E78_RING_ALIGN = 256, NVMTL_E78_RING_MAX = 16 * 1024 };
/* setBytes data: a slice of the command buffer's ring chunk (*out, held by cb->_scratch; returned unretained). NO when
   the ring is off or the data too large: the caller makes a buffer of its own. */
static BOOL nvmtl_bytes_ring_at(__unsafe_unretained NVMTLCommandBuffer *cb, NSUInteger len, size_t *off, __unsafe_unretained NVMTLBuffer **out) {
    size_t need = len < 16 ? 16 : len;
    *out = nil;
    if (!cb || need > NVMTL_E78_RING_MAX || !nvmtl_enc78_on(NVMTL_E78_H4)) return NO;
    size_t at = (cb->_e78RingOff + NVMTL_E78_RING_ALIGN - 1) & ~(size_t)(NVMTL_E78_RING_ALIGN - 1);
    if (!cb->_e78Ring || at + need > cb->_e78Ring->_b.size) {
        NVMTLBuffer *r = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(NVMTL_E78_RING, 1, &r->_b) || !r->_b.map) {
            static int said; if (!said++) nvlog("denc: setBytes ring chunk create failed - per-call buffers instead");
            return NO;
        }
        if (!cb->_scratch) cb->_scratch = [NSMutableArray new];
        [cb->_scratch addObject:r];
        cb->_e78Ring = r; at = 0;
        nvmtl_enc78_count(E78C_RINGCHUNK, 1);
    }
    cb->_e78RingOff = at + need; *off = at;
    *out = (__bridge NVMTLBuffer *)(__bridge void *)cb->_e78Ring;   // held by cb->_scratch: no retain/release per setBytes
    return YES;
}
static NVMTLBuffer *nvmtl_bytes_ring(NVMTLCommandBuffer *cb, NSUInteger len, size_t *off) {
    __unsafe_unretained NVMTLBuffer *b = nil;
    return nvmtl_bytes_ring_at(cb, len, off, &b) ? b : nil;
}

#include "nvconv_lib.h"
@interface NVMTLComputeCommandEncoder () { @public uint8_t _nvcKP[528]; uint32_t _nvcKPLen; uint8_t _nvcCP[272]; uint32_t _nvcCPLen; BOOL _nvcIn; } @end
static id<MTLComputePipelineState> g_nvc_ps[7];
#define NVC_CMP 6
#define NVC_NVAR 5
static NSMutableDictionary *g_nvc_state;
static pthread_mutex_t g_nvc_lk = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long g_nvc_ran, g_nvc_verified, g_nvc_refused, g_nvc_skips;
#define NVC_SKIP(why) do { const unsigned long k_ = ++g_nvc_skips; if (k_ <= 16) nvlog("nvconv: not taken - %s (%lu)", why, k_); return 0; } while (0)
static BOOL nvmtl_nvconv_ready(id<MTLDevice> dev) {
    static int on = -1;
    if (on >= 0) return on;
    pthread_mutex_lock(&g_nvc_lk);
    if (on < 0) {
        const char *e = getenv("NVMTL_NVCONV"); on = !(e && e[0] == '0');
        if (on) {
            NSError *err = nil;
            dispatch_data_t dd = dispatch_data_create(nvconv_metallib, nvconv_metallib_len, NULL, DISPATCH_DATA_DESTRUCTOR_DEFAULT);
            id<MTLLibrary> lib = [dev newLibraryWithData:dd error:&err];
            NSString *names[7] = { @"nvconv3_b", @"nvconv3_s", @"nvconv4_b", @"nvconv4_s", @"nvconv5_b", @"nvconv5_s", @"nvconv_cmp" };
            for (int i = 0; i < 7 && on; i++) {
                id<MTLFunction> f = [lib newFunctionWithName:names[i]];
                g_nvc_ps[i] = f ? [dev newComputePipelineStateWithFunction:f error:&err] : nil;
                if (!g_nvc_ps[i]) { on = 0; nvlog("nvconv: %s did not build (%s) - MPS's conv stays everywhere", names[i].UTF8String, err.description.UTF8String); }
            }
            g_nvc_state = [NSMutableDictionary new];
        }
        nvlog("nvconv: verified MPS-conv fast path %s (NVMTL_NVCONV=%s)", on ? "ON" : "OFF", e ? e : "unset");
    }
    pthread_mutex_unlock(&g_nvc_lk);
    return on;
}
static void nvmtl_nvconv_run(NVMTLComputeCommandEncoder *e, id<MTLComputePipelineState> ps, MTLSize grid, uint32_t variant) {
    NVMTLComputePipelineState *saved = e->_ps; NSMutableDictionary *savedTG = e->_tgLen;
    e->_ps = (NVMTLComputePipelineState *)ps; e->_tgLen = nil; e->_nvcIn = YES;
    [e setBytes:&variant length:4 atIndex:30];
    [e dispatchThreadgroups:grid threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
    e->_nvcIn = NO; e->_ps = saved; e->_tgLen = savedTG;
}
static int nvmtl_nvconv_try(NVMTLComputeCommandEncoder *e, MTLSize grid, MTLSize tg) {
    if (e->_nvcIn || !e->_ps || !e->_ps->_function || ![e->_ps->_function->_fname isEqualToString:@"ndArrayConvolution2D"]) return 0;
    const uint32_t nar = e->_nvcKPLen == 368 ? 3 : e->_nvcKPLen == 448 ? 4 : e->_nvcKPLen == 528 ? 5 : 0;
    if (!nar || e->_nvcCPLen != 272) {
        char why[200]; const uint32_t *w = (const uint32_t *)e->_nvcKP;
        snprintf(why, sizeof why, "parameter blocks %u / %u bytes (first array sizes %u,%u,%u,%u, second %u,%u,%u,%u)", e->_nvcKPLen, e->_nvcCPLen,
                 w[4], w[5], w[6], w[7], w[24], w[25], w[26], w[27]);
        NVC_SKIP(why);
    }
    id<MTLDevice> dev = [e->_cb device];
    if (!nvmtl_nvconv_ready(dev)) return 0;
    const uint32_t *kp = (const uint32_t *)e->_nvcKP, *cp = (const uint32_t *)e->_nvcCP;
    const uint32_t cin = cp[32], cout = cp[33], kw = cp[34], kh = cp[35], groups = cp[36], batch = cp[37], sw = cp[38], sh = cp[39], dw = cp[40], dh = cp[41];
    if (groups != 1 || !cin || !cout || !kw || !kh || !batch || !sw || !sh || !dw || !dh) NVC_SKIP("groups/dims");
    for (uint32_t a = 0; a < nar; a++) if (kp[a * 20] | kp[a * 20 + 1] | kp[a * 20 + 2] | kp[a * 20 + 3]) NVC_SKIP("array offset");
    const uint32_t ob = (nar - 1) * 20;
    if (kp[4] != cin || kp[5] != sw || kp[6] != sh || kp[7] != batch || kp[8] != 1) NVC_SKIP("input descriptor");
    if (kp[24] != cout || kp[25] != cin || kp[26] != kw || kp[27] != kh || kp[28] != 1) NVC_SKIP("weights descriptor");
    if (kp[ob + 4] != cout || kp[ob + 5] != dw || kp[ob + 6] != dh || kp[ob + 7] != batch) NVC_SKIP("output sizes");
    if (kp[ob + 8] != 1 || (dw > 1 && kp[ob + 9] != cout) || (dh > 1 && kp[ob + 10] != cout * dw) || (batch > 1 && kp[ob + 11] != cout * dw * dh)) {
        char why[160]; snprintf(why, sizeof why, "output strides %u,%u,%u,%u for cout %u x %u x %u (sizes %u,%u,%u,%u)",
                                kp[ob + 8], kp[ob + 9], kp[ob + 10], kp[ob + 11], cout, dw, dh, kp[ob + 4], kp[ob + 5], kp[ob + 6], kp[ob + 7]);
        NVC_SKIP(why); }
    if (nar >= 4 && (kp[44] != cout || kp[48] != 1)) NVC_SKIP("bias descriptor");
    if (nar == 5 && (kp[64] != cout || kp[65] != dw || kp[66] != dh || kp[67] != batch || kp[68] != 1 || (dw > 1 && kp[69] != cout) || (dh > 1 && kp[70] != cout * dw)))
        NVC_SKIP("residual descriptor");
    NSArray *bi = nvmtl_bufat(e, 0), *bw = nvmtl_bufat(e, 1), *bo = nvmtl_bufat(e, nar - 1), *bb = nar >= 4 ? nvmtl_bufat(e, 2) : nil, *br = nar == 5 ? nvmtl_bufat(e, 3) : nil;
    if (!bi || !bw || !bo || (nar >= 4 && !bb) || (nar == 5 && !br)) NVC_SKIP("buffer slot unbound");
    #define NVC_ROOM(arr, need) ([(id<MTLBuffer>)arr[0] length] >= [arr[1] unsignedLongValue] + (uint64_t)(need) * 4)
    const uint64_t nin = (uint64_t)(cin - 1) * kp[8] + (uint64_t)(sw - 1) * kp[9] + (uint64_t)(sh - 1) * kp[10] + (uint64_t)(batch - 1) * kp[11] + 1;
    const uint64_t nwt = (uint64_t)(cout - 1) * kp[28] + (uint64_t)(cin - 1) * kp[29] + (uint64_t)(kw - 1) * kp[30] + (uint64_t)(kh - 1) * kp[31] + 1;
    const uint64_t nout = (uint64_t)cout * dw * dh * batch;
    if (!NVC_ROOM(bi, nin) || !NVC_ROOM(bw, nwt) || !NVC_ROOM(bo, nout) || (bb && !NVC_ROOM(bb, cout)) || (br && !NVC_ROOM(br, nout)) || nout > 0xffffffffull) NVC_SKIP("buffer too short for f32");
    #undef NVC_ROOM
    const uint64_t M = (uint64_t)batch * dh * dw;
    const BOOL small = ((M + 127) / 128) * ((cout + 63) / 64) < 60;
    id<MTLComputePipelineState> ours = g_nvc_ps[(nar - 3) * 2 + (small ? 1 : 0)];
    const MTLSize og = small ? MTLSizeMake((M + 63) / 64, (cout + 31) / 32, 1) : MTLSizeMake((M + 127) / 128, (cout + 63) / 64, 1);
    NSMutableData *key = [NSMutableData dataWithBytes:&e->_ps length:sizeof(void *)];
    for (uint32_t a = 0; a < nar; a++) [key appendBytes:kp + a * 20 + 4 length:8 * 4];
    [key appendBytes:cp + 32 length:18 * 4];
    pthread_mutex_lock(&g_nvc_lk); NSNumber *st = g_nvc_state[key]; if (!st) g_nvc_state[key] = @0; pthread_mutex_unlock(&g_nvc_lk);
    if (st.intValue > 0) { nvmtl_nvconv_run(e, ours, og, (uint32_t)(st.intValue - 1)); g_nvc_ran++; return 1; }
    if (st) { static _Atomic unsigned long pend; if (st.intValue == 0 && ++pend <= 8) nvlog("nvconv: still pending (%lu)", (unsigned long)pend); return 0; }
    e->_nvcIn = YES; [e dispatchThreadgroups:grid threadsPerThreadgroup:tg]; e->_nvcIn = NO;
    id<MTLBuffer> outBuf = bo[0]; const NSUInteger outOff = [bo[1] unsignedLongValue];
    id<MTLBuffer> res = [dev newBufferWithLength:32 + 48 options:MTLResourceStorageModeShared];
    memset(res.contents, 0, 32 + 48);
    const uint32_t n = (uint32_t)nout;
    for (uint32_t v = 0; v < NVC_NVAR; v++) {
        id<MTLBuffer> scratch = [dev newBufferWithLength:(NSUInteger)nout * 4 options:MTLResourceStorageModePrivate];
        [e setBuffer:scratch offset:0 atIndex:nar - 1];
        nvmtl_nvconv_run(e, ours, og, v);
        NVMTLComputePipelineState *saved = e->_ps; NSMutableDictionary *savedTG = e->_tgLen;
        e->_ps = (NVMTLComputePipelineState *)g_nvc_ps[NVC_CMP]; e->_tgLen = nil; e->_nvcIn = YES;
        [e setBuffer:outBuf offset:outOff atIndex:5]; [e setBuffer:scratch offset:0 atIndex:6]; [e setBuffer:res offset:v * 4 atIndex:7];
        [e setBytes:&n length:4 atIndex:8]; [e setBuffer:res offset:32 atIndex:9];
        [e dispatchThreads:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)];
        e->_nvcIn = NO; e->_ps = saved; e->_tgLen = savedTG;
    }
    [e setBuffer:outBuf offset:outOff atIndex:nar - 1];
    char shape[96]; snprintf(shape, sizeof shape, "%ux%ux%u -> %u k%ux%u s%u,%u form %u", sw, sh, cin, cout, kw, kh, cp[46], cp[47], nar);
    NSString *sh_ = @(shape);
    { static _Atomic unsigned long issued; const unsigned long k = ++issued; if (k <= 96) nvlog("nvconv: verifying %s (%lu issued)", shape, k); }
    [e->_cb addCompletedHandler:^(id<MTLCommandBuffer> cb) {
        // glcpu4: handlers of a command buffer released uncommitted now run (NotEnqueued): the check never ran, judge nothing
        if (cb.status == MTLCommandBufferStatusNotEnqueued) return;
        const uint32_t *r = (const uint32_t *)res.contents; int pick = 0;
        for (int v = 0; v < NVC_NVAR && !pick; v++) if (r[v] == 0) pick = v + 1;
        pthread_mutex_lock(&g_nvc_lk); g_nvc_state[key] = @(pick ? pick : -1); pthread_mutex_unlock(&g_nvc_lk);
        if (pick) { const unsigned long k = ++g_nvc_verified; if (k <= 64 || !(k & (k - 1))) nvlog("nvconv: VERIFIED %s epilogue %d (%lu so far)", sh_.UTF8String, pick - 1, k); }
        else { const unsigned long k = ++g_nvc_refused; const float *sp = (const float *)(r + 8);
            if (k <= 64 || !(k & (k - 1))) nvlog("nvconv: REFUSED %s - mismatches %u/%u/%u/%u/%u of %u, MPS keeps it (%lu so far); MPS/ours(prelu) %g/%g %g/%g %g/%g %g/%g", sh_.UTF8String,
                                                  r[0], r[1], r[2], r[3], r[4], n, k, sp[0], sp[1], sp[2], sp[3], sp[4], sp[5], sp[6], sp[7]); }
    }];
    return 1;
}

// Metal binds a compute buffer at any 4-byte offset; Vulkan storage buffers need 16. A slot at an unaligned offset is bound to
// a dummy buffer, so a shader that reads its arguments through that slot reads zeros (Photoshop's duplicate-layer kernel, slot 1
// at offset 4100, then dereferenced a null pointer). With NVMTL_MISALIGNED_BUFFER_COPY=1 a CPU-visible buffer at such an
// offset is bound as a copy of the bytes from the offset on (up to 4 MB), the way setBytes binds, which is correct for data the
// kernel only reads. Bytes the kernel writes are carried back after completion. A buffer another GPU command writes earlier in
// the same command buffer is read as it was at encode time. Off unless set.
static int nvmtl_misaligned_copy_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_MISALIGNED_BUFFER_COPY"); on = e && *e && *e != '0'; }
    return on;
}
@implementation NVMTLComputeCommandEncoder
- (unsigned long long)globalTraceObjectID { return nvmtl_global_trace_id(self); }
static void nvmtl_stride_dropped(NSUInteger st) { static BOOL said; if (!said && st) { said = YES;
    nvlog("compute: attributeStride %lu dropped (dynamic strides not implemented) - buffer bound", (unsigned long)st); } }
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    nvmtl_stride_dropped(st); [self setBuffer:b offset:o atIndex:i]; }
- (void)setBufferOffset:(NSUInteger)o attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    nvmtl_stride_dropped(st); [self setBufferOffset:o atIndex:i]; }
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])bs offsets:(const NSUInteger *)offs attributeStrides:(const NSUInteger *)sts withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setBuffer:bs ? bs[k] : nil offset:offs ? offs[k] : 0 attributeStride:sts ? sts[k] : 0 atIndex:r.location + k]; }
- (void)setBytes:(const void *)bytes length:(NSUInteger)len attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    nvmtl_stride_dropped(st); [self setBytes:bytes length:len atIndex:i]; }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])hs count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) [self useHeap:hs[i]]; }
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++)
        [self setSamplerState:s ? s[k] : nil lodMinClamp:lo ? lo[k] : 0.0f lodMaxClamp:hi ? hi[k] : FLT_MAX atIndex:r.location + k]; }
- (MTLDispatchType)dispatchType { return _e78Concurrent ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial; }
- (NVMTLComputePipelineState *)nvmtlSamplerPipeline {
    if (!_ps) return nil;
    NVMTLComputePipelineState *selected = _ps;
    if ((_samplers.count || _tgLen.count) && _e78VarSel && _e78VarFor == _ps && nvmtl_enc78_on(NVMTL_E78_H5)) {
        selected = _e78VarSel; nvmtl_enc78_count(E78C_VARHIT, 1);
    } else if (_samplers.count || _tgLen.count) {
        if (!_e78Key || !nvmtl_enc78_on(NVMTL_E78_H5))
            _e78Key = [NSJSONSerialization dataWithJSONObject:@{@"s": _samplers ?: @{}, @"tg": _tgLen ?: @{}} options:NSJSONWritingSortedKeys error:NULL];
        NSData *key = _e78Key;
        nvmtl_enc78_count(E78C_VARMISS, 1);
        @synchronized (_ps) {
            if (!_ps->_samplerVariants) _ps->_samplerVariants = [NSMutableDictionary new];
            id cached = _ps->_samplerVariants[key];
            if (cached == [NSNull null]) return nil;
            selected = cached;
            if (!selected) {
                NSString *why = nil;
                NSData *spirv = nvmtl_translate_variant(_ps->_function, _samplers, _tgLen, &why);
                if (spirv) {
                    selected = [NVMTLComputePipelineState new];
                    if (nvmtl_vk_compute_pipeline_create(spirv.bytes, spirv.length, &selected->_p)) {
                        selected = nil; why = @"Vulkan compute pipeline creation failed";
                    } else strlcpy(selected->_p.ktname, _ps->_function->_fname.UTF8String ?: "?", sizeof selected->_p.ktname);
                }
                if (!selected) {
                    nvlog("compute pixel sampler specialization REFUSED %s: %s", _ps->_function->_fname.UTF8String, why.UTF8String);
                    _ps->_samplerVariants[key] = [NSNull null];
                    return nil;
                }
                _ps->_samplerVariants[key] = selected;
                nvlog("compute pixel sampler variant built: %s", _ps->_function->_fname.UTF8String);
            }
        }
        _e78VarFor = _ps; _e78VarSel = selected;
    }
    nvmtl_retain_resource(_cb, selected);
    nvmtl_bind_embedded_enc(_cb, NVMTL_SET_VERTEX, _ps->_function, self);
    return selected;
}
static int nvmtl_zprobe_on(void);
- (void)setComputePipelineState:(id<MTLComputePipelineState>)ps { _ps = (NVMTLComputePipelineState *)ps; nvmtl_retain_resource(_cb, ps);
    if (ps && nvmtl_zprobe_on())
        nvlog("ZENC setPipeline %s  tew=%lu maxT=%lu staticTG=%lu",
              _ps && _ps->_function ? _ps->_function->_fname.UTF8String : "?",
              (unsigned long)[ps threadExecutionWidth], (unsigned long)[ps maxTotalThreadsPerThreadgroup],
              (unsigned long)[ps staticThreadgroupMemoryLength]);
}
- (void)setBuffer:(id<MTLBuffer>)b offset:(NSUInteger)off atIndex:(NSUInteger)i {
    NVMTLBuffer *nb = (NVMTLBuffer *)b;
    if (nb && (off & 15) && nvmtl_misaligned_copy_on() && nb->_b.map && nb->_b.size > off) {
        size_t n = nb->_b.size - off; if (n > ((size_t)4 << 20)) n = (size_t)4 << 20;
        static int said; if (!said++) nvlog("compute setBuffer: slot %lu at offset %lu is not 16-aligned - bound as a %lu-byte copy (NVMTL_MISALIGNED_BUFFER_COPY)", (unsigned long)i, (unsigned long)off, (unsigned long)n);
        NSData *orig = [NSData dataWithBytes:(const uint8_t *)nb->_b.map + off length:n];
        [self setBytes:orig.bytes length:n atIndex:i];
        // Bytes the kernel changed are carried back into the real buffer once the command buffer completes. Only changed
        // bytes move, so a write by anyone else to the rest of the range in the meantime is not undone.
        if (i < NVMTL_NBUFAT && _baObj[i] && ((NVMTLBuffer *)_baObj[i])->_b.map && _baLen[i] == n) {
            NVMTLBuffer *copy = (NVMTLBuffer *)_baObj[i]; size_t coff = _baOff[i]; NVMTLBuffer *home = nb; size_t hoff = off;
            [_cb addCompletedHandler:^(id<MTLCommandBuffer> done) {
                (void)done;
                if (!copy->_b.map || !home->_b.map || home->_b.size < hoff + n) return;
                const uint8_t *now = (const uint8_t *)copy->_b.map + coff, *was = orig.bytes; uint8_t *dst = (uint8_t *)home->_b.map + hoff;
                for (size_t k = 0; k < n; k++) if (now[k] != was[k]) dst[k] = now[k];
            }];
        } else {
            static int said2; if (!said2++) nvlog("compute setBuffer: the copy of an unaligned buffer could not be located - its writes are not carried back");
        }
        return;
    }
    if (!nb) {
        if (i < NVMTL_NBUFAT) { _baObj[i] = nil; _baN[i] = 0; }   // the binding table must not keep the previous resource
        size_t zoff = 0;
        NVMTLBuffer *zb = nvmtl_bytes_ring(_cb, 16, &zoff);
        const BOOL zring = zb != nil;
        if (!zring) {
            zb = [NVMTLBuffer new];
            if (nvmtl_vk_buffer_create(16, 1, &zb->_b)) { nvlog("compute setBuffer: nil at %lu - zero buffer create FAILED", (unsigned long)i); return; }
        }
        if (zb->_b.map) memset((uint8_t *)zb->_b.map + zoff, 0, 16);
        if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, &zb->_b, zoff)) nvlog("compute setBuffer: nil at %lu - bind FAILED", (unsigned long)i);
        if (!zring) {
            if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
            [_cb->_scratch addObject:zb];
        }
        if (nvmtl_zprobe_on()) nvlog("ZENC setBuffer  idx %-3lu len 0        off 0  (nil -> zero-filled)", (unsigned long)i);
        return;
    }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    // the same buffer at the same index of this encoder is already held by the command buffer's set
    if (i >= NVMTL_NBUFAT || _baObj[i] != (id)nb) [_cb->_resources addObject:nb];
    if (i < NVMTL_NBUFAT) { _baObj[i] = nb; _baOff[i] = off; _baN[i] = 2; }
    if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, &nb->_b, off)) nvlog("compute setBuffer: bind FAILED");
    if (nvmtl_zprobe_on()) {
        static const char *ZH = "0123456789abcdef";
        char hx[96]; size_t hn = 0;
        size_t cap = nb->_b.size < 32 ? nb->_b.size : 32;
        if (nb->_b.map) for (size_t k = 0; k < cap; k++) {
            uint8_t byte = ((const uint8_t *)nb->_b.map)[k];
            hx[hn++] = ZH[byte >> 4]; hx[hn++] = ZH[byte & 15];
        }
        hx[hn] = 0;
        nvlog("ZENC setBuffer  idx %-3lu len %-8lu off %lu  %s%s", (unsigned long)i,
              (unsigned long)[(id<MTLBuffer>)b length], (unsigned long)off,
              nb->_b.map ? "bytes " : "NO HOST MAP (private - bytes unreadable from here)", hx);
    }
}
- (void)setAccelerationStructure:(id<MTLAccelerationStructure>)as atBufferIndex:(NSUInteger)i {
    [self setBuffer:(id<MTLBuffer>)as offset:0 atIndex:i];
    NVMTLAccelerationStructure *a = (NVMTLAccelerationStructure *)as;
    if (a && [(id)a isKindOfClass:[NVMTLAccelerationStructure class]] && a->_uid.buf && _cb)
        nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, 31, &a->_uid, 0);
}
- (void)setBytes:(const void *)bytes length:(NSUInteger)len atIndex:(NSUInteger)i {
    if (i == 23 && len <= sizeof _nvcKP && bytes) { memcpy(_nvcKP, bytes, len); _nvcKPLen = (uint32_t)len; }
    else if (i == 29 && len == sizeof _nvcCP && bytes) { memcpy(_nvcCP, bytes, len); _nvcCPLen = (uint32_t)len; }
    size_t roff = 0;
    const BOOL dupable = _cb && bytes && len && len <= NVMTL_G79_DUPMAX && i < NVMTL_G79_DUPIDX && nvmtl_g79_on(NVMTL_G79_DUP);
    if (dupable && _cb->_g79BRing[i] && _cb->_g79BLen[i] == len && !memcmp(_cb->_g79B[i], bytes, len)) {
        if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, &_cb->_g79BRing[i]->_b, _cb->_g79BOff[i])) nvlog("compute setBytes: bind FAILED");
        // The binding table follows the binding: kernels run from compiled machine code read their buffer
        // addresses from it, and a stale entry hands the GPU an address that belongs to nothing.
        _baObj[i] = _cb->_g79BRing[i]; _baOff[i] = _baBase[i] = _cb->_g79BOff[i]; _baLen[i] = len; _baN[i] = 4;
        nvmtl_g79_count(G79C_BYTESDUP);
        return;
    }
    NVMTLBuffer *nb = nvmtl_bytes_ring(_cb, len, &roff);
    BOOL ring = nb != nil;
    if (!ring) {
        nb = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(len < 16 ? 16 : len, 1, &nb->_b)) { nvlog("compute setBytes: create FAILED"); return; }
        nvmtl_enc78_count(E78C_BYTESOLD, 1);
    } else { nvmtl_enc78_count(E78C_RING, 1); nvmtl_enc78_count(E78C_RINGBYTES, len); }
    if (nb->_b.map) memcpy((uint8_t *)nb->_b.map + roff, bytes, len);
    if (dupable) {
        _cb->_g79BRing[i] = ring && nb->_b.map ? nb : nil; _cb->_g79BOff[i] = roff; _cb->_g79BLen[i] = (uint32_t)len;
        memcpy(_cb->_g79B[i], bytes, len);
    }
    if (i < NVMTL_NBUFAT) { _baObj[i] = nb; _baOff[i] = _baBase[i] = roff; _baLen[i] = len; _baN[i] = 4; }   // buffer, offset, inline length, base
    if (nvmtl_vk_bind_buffer(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, &nb->_b, roff)) nvlog("compute setBytes: bind FAILED");
    if (nvmtl_zprobe_on()) {
        const uint32_t *zw = (const uint32_t *)bytes; char zb[220]; int zo = 0;
        for (NSUInteger k = 0; (k + 1) * 4 <= len && k < 13 && zo < 190; k++)
            zo += snprintf(zb + zo, sizeof zb - (size_t)zo, "%08x ", zw[k]);
        zb[zo] = 0;
        nvlog("ZENC setBytes   idx %-3lu len %-4lu map %s words: %s", (unsigned long)i,
              (unsigned long)len, nb->_b.map ? "yes" : "NO", zb);
    }
    if (ring) return;
    if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
    [_cb->_scratch addObject:nb];
}

#define NVMTL_ZPROBE_MAX 64
#define NVMTL_ZDISP_MAX 400
static NSMutableArray *g_zprobe;
static int nvmtl_zprobe_on(void) {
    static int on = -1;
    if (on < 0) on = NVMTL_RELEASE ? 0 : ([[NSProcessInfo processInfo] environment][@"NVMTL_ZPROBE"] ? 1 : 0);
    return on;
}
static void nvmtl_zprobe_dispatch(const char *kernel, const uint32_t *t, const uint32_t *l) {
    if (!nvmtl_zprobe_on()) return;
    static unsigned seq = 0;
    if (seq >= NVMTL_ZDISP_MAX) return;
    seq++;
    nvlog("ZDISP #%u %s threads %ux%ux%u local %ux%ux%u", seq, kernel ? kernel : "(no pipeline set)",
          t[0], t[1], t[2], l[0], l[1], l[2]);
}
static void nvmtl_zprobe_record(NVMTLTexture *t, const char *kernel, unsigned idx, unsigned stex) {
    if (!nvmtl_zprobe_on() || !t) return;
    @synchronized ([NVMTLTexture class]) {
        if (!g_zprobe) g_zprobe = [NSMutableArray new];
        if (g_zprobe.count >= NVMTL_ZPROBE_MAX) return;
        [g_zprobe addObject:@[t, @(kernel ?: "(no pipeline set)"), @(idx), @(stex)]];
    }
}
static void nvmtl_zprobe_drain(void) {
    if (!nvmtl_zprobe_on()) return;
    NSArray *snap = nil;
    @synchronized ([NVMTLTexture class]) { snap = g_zprobe; g_zprobe = nil; }
    for (NSArray *e in snap) {
        NVMTLTexture *t = e[0];
        const char *kn = [e[1] UTF8String];
        unsigned idx = [e[2] unsignedIntValue], stex = [e[3] unsignedIntValue];
        nvk_image *im = [t nvi];
        if (!im || !im->w || !im->h || !im->bpp) { nvlog("ZPROBE %s tex(%u) stex=%u: %p has no image - SKIPPED (a fact about the probe, not about the pixels)", kn, idx, stex, (__bridge void *)t); continue; }
        uint32_t rw = im->w > 256 ? 256 : im->w, rh = im->h > 256 ? 256 : im->h;
        size_t row = (size_t)rw * im->bpp;
        NSMutableData *md = [NSMutableData dataWithLength:row * rh];
        if (!md) continue;
        [t getBytes:md.mutableBytes bytesPerRow:row fromRegion:MTLRegionMake2D(0, 0, rw, rh) mipmapLevel:0];
        unsigned nz = 0, nn = 0;
        nvmtl_nonzero(md.mutableBytes, row, row, rh, &nz, &nn);
        nvlog("ZPROBE %s tex(%u) stex=%u: %p %ux%u x%u slice(s) bpp %u storage %u -> NONZERO %u/%u bytes in %ux%u of slice 0",
              kn, idx, stex, (__bridge void *)t, im->w, im->h, im->layers, im->bpp, im->storage, nz, nn, rw, rh);
    }
}
- (void)setTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i {
    NVMTLTexture *nt = (NVMTLTexture *)t;
    if (nvmtl_sass_tables_wanted()) {
        if (!_sassTextures) _sassTextures = [NSMutableDictionary new];
        if (nt) _sassTextures[@(i)] = nt; else [_sassTextures removeObjectForKey:@(i)];
    }
    if (!nt) { nvlog("compute setTexture: nil at %lu", (unsigned long)i); return; }
    if (nt->_parent && nt->_baseLevel) nt->_parent->_mipsWritten = YES;
    nvmtl_zprobe_record(nt, _ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, (unsigned)i, nt->_stex ? 1u : 0u);
    nvmtl_retain_resource(_cb, nt);
    nvmtl_surface_in_for_cb(_cb, nt); if (!(nt->_usage & MTLTextureUsageShaderWrite)) nvmtl_ws_sampled(nt);
    // Storage-image capability is also needed for read-only imageLoad. Private IOSurface images are made storage
    // capable regardless of their descriptor usage; that must not turn a ShaderRead input into a producer. In a
    // VTMTS capture, doing so copied the BGRA input back and moved its seed, forcing WindowServer to upload it again.
    // Unknown usage permits shader writes; an explicit ShaderRead texture does not.
    const MTLTextureUsage readOnly = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (nt->_stex && (!nt->_usage || (nt->_usage & ~readOnly))) nvmtl_surf_dirty(_cb, nt);
    if (nt->_tbView) nvmtl_vk_bind_texel_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nt->_tbView, nt->_tbStorage);
    else {
    if (nvmtl_vk_cmd_prepare_sampled_image(&_cb->_c, nvmtl_tex_nvi(nt), nt->_stex)) { nvlog("compute setTexture: image layout preparation refused"); return; }
    if (nvmtl_vk_bind_texture_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nvmtl_tex_sampleview(nt))) nvlog("compute setTexture: bind FAILED");
    nvmtl_vk_bind_storage_view(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, nvmtl_storage_view_for(_cb, nt, nt->_stex || nvmtl_function_writes_texture(_ps ? _ps->_function : nil, i)));
    }
}
static int nvmtl_conc_no_barrier(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_CONC_NO_BARRIER"); on = e && e[0] == '1';
        if (on) nvlog("compute: a concurrent encoder's later dispatches take NO barrier unless the app asks (NVMTL_CONC_NO_BARRIER=1)"); }
    return on;
}
- (void)nvmtlE78Barrier:(BOOL)indirect pipe:(const nvk_pipeline *)pipe {
    int kind;
    if (indirect) kind = 0;
    else if (!_e78NDisp) {
        kind = _cb->_g79C2C && nvmtl_g79_on(NVMTL_G79_ENC) ? 1 : 0;
        nvmtl_g79_count(kind ? G79C_ENCNARROW : G79C_ENCFULL);
    }
    // A concurrent compute encoder's later dispatches used to get NO barrier at all unless the app had called
    // memoryBarrierWithScope: (the H7 row: kind 2 = none). That reads Metal's concurrent dispatch type as a
    // promise that the dispatches are independent, and Cyberpunk's are not: a later dispatch read a buffer an
    // earlier one in the same encoder was still writing, so it consumed garbage - wrong lighting, and pointers
    // loaded out of that garbage faulted the channel at addresses no buffer covers (FAULT_PTE / FAULT_PDE).
    // Measured 2026-11-03 on the benchmark, every other setting held: H7 on faulted in every run that reached
    // the light pass; NVMTL_ENC78_OFF=h7 completed at 39.28 fps against 41.08 with it on, so the narrow
    // compute->compute barrier it costs is within noise. Concurrent dispatches now take that same narrow
    // barrier serial ones do (P1); NVMTL_ENC78_OFF still turns the rows off for an A/B.
    // nm271: NVMTL_CONC_NO_BARRIER=1 restores the H7 row (Metal's documented concurrent semantics: no barrier unless
    // the app asks with memoryBarrierWithScope:), for an A/B of the cost on Xe2.
    else if (_e78Concurrent && nvmtl_conc_no_barrier() && nvmtl_enc78_on(NVMTL_E78_H7)) {
        kind = _g79Bar ? 1 : 2;
        if (_g79Bar) nvmtl_g79_count(G79C_BARFOLD);
    }
    else if (nvmtl_enc78_on(NVMTL_E78_P1)) kind = 1;
    else kind = 0;
    // compute2 NVMTL_CS_HAZARD: a serial pass's compute->compute barrier only on a buffer conflict (eligible kernels);
    // the encoder's first dispatch, an explicit memory barrier and a concurrent pass keep their own rules
    if (nvmtl_vk_cs_hazard_on() && !_e78Concurrent) {
        const int serial_c2c = kind == 1 && _e78NDisp && !_g79Bar;
        if (!nvmtl_vk_cs_hazard(&_cb->_c, pipe, !serial_c2c)) kind = 2;   // counted below as E78C_NONE
    }
    // compute2 NVMTL_CS_BAR: the destination masks fit this kernel's reads only where every later dispatch gets a barrier
    // of its own (a serial pass after its first dispatch). An encoder's first barrier and a concurrent pass's folded
    // memoryBarrier also cover dispatches that follow without one: they keep prod7's masks (pipe NULL).
    const nvk_pipeline *own = (!_e78Concurrent && _e78NDisp) ? pipe : NULL;
    _g79Bar = NO;
    _e78NDisp++;
    if (kind == 0) nvmtl_vk_cmd_barrier(&_cb->_c);
    else if (kind == 1) nvmtl_vk_cmd_barrier_c2c_p(&_cb->_c, own);
    if (kind) _e78Loose = YES;
    nvmtl_enc78_count(E78C_DISP, 1); nvmtl_enc78_count(E78C_FULL + kind, 1);
    if (_e78Concurrent) nvmtl_enc78_count(E78C_CONC, 1);
}
// A threadgroup of 1024 threads fits once per SM at 40 registers (32 of 48 warps) and MPS's Gaussian ran 1.5x
// slower in 32x32 groups than in 32x4 on the RTX 5060 (gblin, identical pixels). A kernel that cannot see its
// grouping (nvmtl_air_grid_only) runs the same threads in groups of at most 128: height first, then depth, then
// width down to 32, each halved only while it stays a divisor, so the thread set and every grid position are
// unchanged. B580: OFF unless NVMTL_TG_RESHAPE=1 - the occupancy argument above is NVIDIA's (warps per SM) and was
// not measured on Xe2; =force skips the grid-only check, which only a test uses, to prove its probe sees a kernel
// that depends on its grouping.
static void nvmtl_tg_reshape(NVMTLComputePipelineState *ps, const uint32_t l[3], uint32_t out[3])
{
    out[0] = l[0]; out[1] = l[1]; out[2] = l[2];
    static int mode = -1;
    if (mode < 0) { const char *e = getenv("NVMTL_TG_RESHAPE"); mode = !e ? 0 : e[0] == '1' ? 1 : e[0] == 'f' ? 2 : 0; }
    if (!mode || !ps || l[0] * l[1] * l[2] <= 128) return;
    if (mode == 2) ps->_gridOnly = 1;
    if (!ps->_gridOnly) {
        ps->_gridOnly = nvmtl_air_grid_only(ps->_function) ? 1 : -1;
        if (ps->_gridOnly > 0) nvlog("dispatch: %s uses only its grid position - threadgroups above 128 threads are split",
                                     ps->_function ? ps->_function->_fname.UTF8String : "-");
    }
    if (ps->_gridOnly < 0) return;
    while (out[0] * out[1] * out[2] > 128 && out[1] % 2 == 0) out[1] /= 2;
    while (out[0] * out[1] * out[2] > 128 && out[2] % 2 == 0) out[2] /= 2;
    while (out[0] * out[1] * out[2] > 128 && out[0] > 32 && out[0] % 2 == 0) out[0] /= 2;
}
- (void)dispatchThreadgroups:(MTLSize)grid threadsPerThreadgroup:(MTLSize)tg {
    if (_ps && _ps->_sassKernel) _cb->_c.hz_unknown = 1;   // compute2: not tracked by NVMTL_CS_HAZARD
    if (_ps && _ps->_sassKernel) {
        int ran = nvmtl_sass_dispatch(self, grid, tg, 0, !_ps->_sassOnly);
        if (ran) { if (ran > 0 && _cb->_stTwins.count) nvmtl_storage_twins_out(_cb); return; }
    }
    if (!_nvcIn && nvmtl_nvconv_try(self, grid, tg)) { _cb->_c.hz_unknown = 1; return; }
    NVMTLComputePipelineState *selected = [self nvmtlSamplerPipeline];
    if (!selected) { nvlog("dispatch with no valid compute sampler pipeline"); return; }
    uint32_t l[3] = { (uint32_t)tg.width, (uint32_t)tg.height, (uint32_t)tg.depth };
    uint32_t t[3] = { (uint32_t)(grid.width * tg.width), (uint32_t)(grid.height * tg.height), (uint32_t)(grid.depth * tg.depth) };
    if (nvmtl_vk_tg_reject(l, t, _ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, selected->_p.lin)) return;
    nvmtl_zprobe_dispatch(_ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, t, l);
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_);
        nvtrace("DISPATCH %s | threads %ux%ux%u tg %ux%ux%u", (_ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL) ?: "-", t[0], t[1], t[2], l[0], l[1], l[2]);
        nvtrace("  BUFS%s", bufs_); }
    [self nvmtlE78Barrier:NO pipe:&selected->_p];
    if (nvmtl_vendor_dispatch_sass(selected, &_cb->_c, t, l) != 0) {
        uint32_t lr[3]; nvmtl_tg_reshape(_ps, l, lr);
        nvmtl_vk_cmd_dispatch_threads(&_cb->_c, &selected->_p, t, lr);
    }
    if (_cb->_stTwins.count) nvmtl_storage_twins_out(_cb);
}
- (void)dispatchThreads:(MTLSize)threads threadsPerThreadgroup:(MTLSize)tg {
    if (_ps && _ps->_sassKernel) _cb->_c.hz_unknown = 1;   // compute2: not tracked by NVMTL_CS_HAZARD
    if (_ps && _ps->_sassKernel) {
        int ran = nvmtl_sass_dispatch(self, threads, tg, 1, !_ps->_sassOnly);
        if (ran) { if (ran > 0 && _cb->_stTwins.count) nvmtl_storage_twins_out(_cb); return; }
    }
    NVMTLComputePipelineState *selected = [self nvmtlSamplerPipeline];
    if (!selected) { nvlog("dispatch with no valid compute sampler pipeline"); return; }
    uint32_t l[3] = { (uint32_t)tg.width, (uint32_t)tg.height, (uint32_t)tg.depth };
    uint32_t t[3] = { (uint32_t)threads.width, (uint32_t)threads.height, (uint32_t)threads.depth };
    if (nvmtl_vk_tg_reject(l, t, _ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, selected->_p.lin)) return;
    nvmtl_zprobe_dispatch(_ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, t, l);
    if (nvmtl_trace_on()) { char bufs_[600]; nvmtl_vk_trace_buffers(&_cb->_c, bufs_, sizeof bufs_);
        nvtrace("DISPATCH %s | threads %ux%ux%u tg %ux%ux%u", (_ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL) ?: "-", t[0], t[1], t[2], l[0], l[1], l[2]);
        nvtrace("  BUFS%s", bufs_); }
    [self nvmtlE78Barrier:NO pipe:&selected->_p];
    if (nvmtl_vendor_dispatch_sass(selected, &_cb->_c, t, l) != 0) {
        uint32_t lr[3]; nvmtl_tg_reshape(_ps, l, lr);
        nvmtl_vk_cmd_dispatch_threads(&_cb->_c, &selected->_p, t, lr);
    }
    if (_cb->_stTwins.count) nvmtl_storage_twins_out(_cb);
}
- (void)setBufferOffset:(NSUInteger)off atIndex:(NSUInteger)i {
    if (i < NVMTL_NBUFAT && _baN[i]) { _baOff[i] = off; _baN[i] = 2; }
    if (nvmtl_vk_bind_buffer_offset(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, off))
        nvlog("compute setBufferOffset: nothing bound at index %lu", (unsigned long)i);
}
// MPS (Lanczos scale, used by video players) sets an imageblock size on compute encoders.
// The translator keeps a kernel's imageblock cells in per-thread private memory and derives
// the block extent from the threadgroup size, so no encoder state is needed; without this
// method the call is an unrecognized selector and the app aborts.
- (void)setImageblockWidth:(NSUInteger)w height:(NSUInteger)h {
    static int logged;
    if (!logged++) nvlog("compute setImageblockWidth:%lu height:%lu (imageblock lives in thread-private memory)",
                         (unsigned long)w, (unsigned long)h);
}
- (void)setThreadgroupMemoryLength:(NSUInteger)len atIndex:(NSUInteger)i {
    if (!_tgLen) _tgLen = [NSMutableDictionary new];
    if (nvmtl_zprobe_on()) nvlog("ZENC tgMemory   idx %-3lu length %lu", (unsigned long)i, (unsigned long)len);
    NSUInteger was = [_tgLen[@(i).stringValue] unsignedIntegerValue];
    if (len) _tgLen[@(i).stringValue] = @(len);
    else     [_tgLen removeObjectForKey:@(i).stringValue];
    if (was != len) { _e78Key = nil; _e78VarSel = nil; }
}
- (void)executeCommandsInBuffer:(id<MTLIndirectCommandBuffer>)icb withRange:(NSRange)r {
    NVMTLIndirectCommandBuffer *b = (NVMTLIndirectCommandBuffer *)icb;
    if (!b) { nvlog("compute executeCommandsInBuffer: nil"); return; }
    NSUInteger n = 0;
    for (NSUInteger i = r.location; i < r.location + r.length && i < b->_cmds.count; i++) {
        NVMTLIndirectCommand *c = b->_cmds[i];
        if (!c->_armed || !c->_isCompute) continue;
        if (c->_cps) [self setComputePipelineState:c->_cps];
        for (NSArray *e in c->_kbufs) [self setBuffer:e[0] offset:[e[1] unsignedLongValue] atIndex:[e[2] unsignedLongValue]];
        [self dispatchThreadgroups:c->_grid threadsPerThreadgroup:c->_tg];
        n++;
    }
    nvlog("compute executeCommandsInBuffer: replayed %lu of %lu slot(s)", (unsigned long)n, (unsigned long)r.length);
}
- (void)setSamplerState:(id<MTLSamplerState>)ss atIndex:(NSUInteger)i {
    NVMTLSamplerState *ns = (NVMTLSamplerState *)ss;
    if (nvmtl_sass_tables_wanted()) {
        if (!_sassSamplers) _sassSamplers = [NSMutableDictionary new];
        if (ns) _sassSamplers[@(i)] = ns; else [_sassSamplers removeObjectForKey:@(i)];
    }
    // the dictionary (the variant key) changes only for a pixel-coordinate sampler or one replacing it: the rest skip the
    // NSString key per bind (as the render encoder's _pixMask)
    const uint64_t pbit = i < 64 ? 1ull << i : 0;
    if ((ns && ns->_pixelState) || !pbit || (_cpixMask & pbit)) {
        if (!_samplers) _samplers = [NSMutableDictionary new];
        NSString *key = [NSString stringWithFormat:@"%lu", (unsigned long)i];
        id was = _samplers[key], now = ns ? ns->_pixelState : nil;
        if (ns && ns->_pixelState) { _samplers[key] = ns->_pixelState; _cpixMask |= pbit; }
        else { [_samplers removeObjectForKey:key]; _cpixMask &= ~pbit; }
        if (was != now && ![was isEqual:now]) { _e78Key = nil; _e78VarSel = nil; }
    }
    nvmtl_retain_resource(_cb, ns);
    if (nvmtl_vk_bind_sampler(&_cb->_c, NVMTL_SET_VERTEX, (uint32_t)i, ns ? &ns->_s : NULL)) nvlog("compute setSamplerState: bind FAILED");
    else nvmtl_note_app_sampler(_cb, NVMTL_SET_VERTEX, i, ns);
}
- (void)setSamplerState:(id<MTLSamplerState>)ss lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    [self setSamplerState:ss atIndex:i];
}
- (void)setBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)offs withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (b[k]) [self setBuffer:b[k] offset:offs ? offs[k] : 0 atIndex:r.location + k];
}
- (void)setTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) if (t[k]) [self setTexture:t[k] atIndex:r.location + k];
}
- (void)setSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])ss withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setSamplerState:ss[k] atIndex:r.location + k];
}
- (void)dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)buf indirectBufferOffset:(NSUInteger)off
                         threadsPerThreadgroup:(MTLSize)tg {
    NVMTLBuffer *nb = (NVMTLBuffer *)buf;
    if (!nb) { nvlog("dispatchThreadgroupsWithIndirectBuffer: nil"); return; }
    nvmtl_retain_resource(_cb, nb);
    // a pipeline with only compiled machine code has no Vulkan pipeline to run an indirect dispatch on
    if (_ps && _ps->_sassOnly) {
        [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
            userInfo:@{ NSLocalizedDescriptionKey: @"Indirect dispatch is not available for this kernel" }]];
        return;
    }
    NVMTLComputePipelineState *selected = [self nvmtlSamplerPipeline];
    if (!selected) { nvlog("indirect dispatch with no valid compute sampler pipeline"); return; }
    uint32_t l[3] = { (uint32_t)tg.width, (uint32_t)tg.height, (uint32_t)tg.depth };
    if (nvmtl_vk_tg_reject(l, NULL, _ps && _ps->_function ? _ps->_function->_fname.UTF8String : NULL, selected->_p.lin)) return;
    [self nvmtlE78Barrier:YES pipe:&selected->_p];
    if (![self nvmtlPrepareIndirectImages]) return;
    nvmtl_vk_cmd_dispatch_indirect_tg(&_cb->_c, &selected->_p, &nb->_b, off, l);
    if (_cb->_stTwins.count) nvmtl_storage_twins_out(_cb);
}
// Before an indirect dispatch, every texture the encoder declared with useResource:usage: is brought in for this command
// buffer and moved to the layout the access needs: GENERAL when it may be written as a storage image, read-only otherwise.
// A texture written earlier in the frame as a render target is still in its attachment layout; on NVIDIA that layout can
// keep the surface compressed, and a storage write into it without the transition reaches memory the image does not own
// (Cyberpunk's 2048x2048 RG11B10 clear faulted in a page no image or buffer held). Direct dispatches get the same
// preparation through setTexture:atIndex:; textures reached only through an argument buffer get it here.
- (BOOL)nvmtlPrepareIndirectImages {
    for (NSValue *key in _indirectImages) {
        NSArray *entry = _indirectImages[key]; NVMTLTexture *t = entry[0];
        MTLResourceUsage usage = [entry[1] unsignedLongLongValue];
        nvmtl_surface_in_for_cb(_cb, t);
        if (usage & MTLResourceUsageWrite) nvmtl_surf_dirty(_cb, t); else nvmtl_ws_sampled(t);
        if (nvmtl_vk_cmd_prepare_sampled_image(&_cb->_c, [t nvi], (usage & MTLResourceUsageWrite) && t->_stex)) {
            [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                userInfo:@{NSLocalizedDescriptionKey:@"Cannot prepare a declared indirect texture for dispatch"}]];
            return NO;
        }
    }
    return YES;
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {
    nvmtl_retain_resource(_cb, r);
    if ([(id)r isKindOfClass:[NVMTLTexture class]]) {
        NVMTLTexture *t = (NVMTLTexture *)r;
        if (!t->_tbView) {
            if (!_indirectImages) _indirectImages = [NSMutableDictionary new];
            _indirectImages[[NSValue valueWithPointer:[t nvi]]] = @[t, @(u)];
        }
    }
}
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u { for (NSUInteger i=0;i<n;i++) [self useResource:r[i] usage:u]; }
- (void)useHeap:(id<MTLHeap>)h { nvmtl_use_heap(_cb, h); }
- (void)sampleCountersInBuffer:(id)sb atSampleIndex:(NSUInteger)i withBarrier:(BOOL)b { nvmtl_sample_counter(_cb, sb, i, self, b); }
- (void)memoryBarrierWithScope:(MTLBarrierScope)scope {
    if (nvmtl_g79_on(NVMTL_G79_BAR)) { _g79Bar = YES; nvmtl_g79_count(G79C_BARLAZY); } else nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)updateFence:(id<MTLFence>)f {}
- (void)waitForFence:(id<MTLFence>)f { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)memoryBarrierWithResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n {
    if (nvmtl_g79_on(NVMTL_G79_BAR)) { _g79Bar = YES; nvmtl_g79_count(G79C_BARLAZY); } else nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)pushDebugGroup:(NSString *)g { nvtrace("GROUP %s", g.UTF8String ?: "?"); }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)sp {}
- (void)endEncoding {
    if (nvmtl_g79_on(NVMTL_G79_ENC)) {
        if (_e78NDisp) { _cb->_g79C2C = YES; if (_e78Loose || _g79Bar) _cb->_g79Owed = YES; }
        _e78Loose = NO; _g79Bar = NO;
    } else if (_e78Loose || _g79Bar) { nvmtl_vk_cmd_barrier(&_cb->_c); _e78Loose = NO; _g79Bar = NO; nvmtl_enc78_count(E78C_ENDFULL, 1); }
    nvmtl_g79_census();
    [_cb->_stTwins removeAllObjects];   // the next encoder copies each twin in again
    _cb->_nEnd++; nvmtl_enc78_census();
    // The command buffer holds every resource until the GPU is done; the encoder's own binding tables are dead after
    // this point, as in Apple's encoders. WAS: kept until dealloc, which MPS defers to the completion handler; by then
    // Espresso had recycled the objects they named and .cxx_destruct released freed memory (mediaanalysisd, issue #107).
    for (unsigned k = 0; k < NVMTL_NBUFAT; k++) { _baObj[k] = nil; _baN[k] = 0; }
    _samplers = nil; _cpixMask = 0; _tgLen = nil; _e78Key = nil;
    _indirectImages = nil;   // 9a05b0a's declared textures: dead with the encoder's other tables
    if (_tsEnd) { nvmtl_ts_pass_end(_cb, self, _tsEnd); _tsEnd = nil; }
}
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { id<MTLDevice> d = _cb ? [_cb device] : nil; return d ? d : (id<MTLDevice>)gNVMTLMainDevice; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)_cb; }   // SPI, see the render encoder
@end

static NSUInteger nvmtl_texture_slice_count(NVMTLTexture *t) {
    MTLTextureType type = t.textureType;
    return type == MTLTextureTypeCube ? 6 : type == MTLTextureTypeCubeArray ? t.arrayLength * 6 : t.arrayLength;
}
@implementation NVMTLBlitCommandEncoder
- (unsigned long long)globalTraceObjectID { return nvmtl_global_trace_id(self); }
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so toBuffer:(id<MTLBuffer>)dst
     destinationOffset:(NSUInteger)dof size:(NSUInteger)size {
    NVMTLBuffer *s = (NVMTLBuffer *)src, *d = (NVMTLBuffer *)dst;
    if (!s || !d) { nvlog("blit copyFromBuffer: nil"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:s];
    [_cb->_resources addObject:d];
    if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &s->_b, so, &d->_b, dof, size)) nvlog("blit copyFromBuffer: FAILED");
}
- (void)fillBuffer:(id<MTLBuffer>)buf range:(NSRange)range value:(uint8_t)value {
    NVMTLBuffer *b = (NVMTLBuffer *)buf;
    if (!b) { nvlog("blit fillBuffer: nil buffer"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:b];
    if (nvmtl_vk_cmd_fill_buffer(&_cb->_c, &b->_b, range.location, range.length, value))
        nvlog("blit fillBuffer: FAILED");
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dof {
    NVMTLTexture *s = (NVMTLTexture *)src, *d = (NVMTLTexture *)dst;
    if (d && dl + d->_baseLevel > 0) (d->_parent ?: d)->_mipsWritten = YES;
    if (!s || !d) { nvlog("blit copyFromTexture: nil"); return; }
    if (sl >= s.mipmapLevelCount || dl >= d.mipmapLevelCount || ss >= nvmtl_texture_slice_count(s) || ds >= nvmtl_texture_slice_count(d)) {
        nvlog("blit copyFromTexture: subresource outside view"); return;
    }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:s];
    [_cb->_resources addObject:d];
    { int was = nvtrace_scope([d nvi]->w, [d nvi]->h);
      nvtrace("BLIT tex %ux%u surf %u (%lu,%lu %lux%lu) -> tex %ux%u surf %u (%lu,%lu)", [s nvi]->w, [s nvi]->h, nvmtl_surf_id(s), (unsigned long)so.x,
          (unsigned long)so.y, (unsigned long)size.width, (unsigned long)size.height, [d nvi]->w, [d nvi]->h, nvmtl_surf_id(d), (unsigned long)dof.x, (unsigned long)dof.y);
      g_trace_mute = was; }
    nvmtl_surface_in_for_cb(_cb, s); nvmtl_ws_sampled(s);
    const BOOL overwrite = [s nvi] != [d nvi] && (!s->_surf || !d->_surf || IOSurfaceGetID(s->_surf) != IOSurfaceGetID(d->_surf))
        && nvmtl_surface_full_overwrite(d, (MTLRegion){dof, size}, dl, ds);
    if (!overwrite) nvmtl_surface_in_for_cb(_cb, d);
    nvmtl_surf_dirty(_cb, d);
    if (nvmtl_vk_cmd_copy_image_sub3(&_cb->_c, [s nvi], (uint32_t)sl + s->_baseLevel, (uint32_t)ss + s->_baseSlice, (uint32_t)so.x, (uint32_t)so.y, (uint32_t)so.z,
                                [d nvi], (uint32_t)dl + d->_baseLevel, (uint32_t)ds + d->_baseSlice, (uint32_t)dof.x, (uint32_t)dof.y, (uint32_t)dof.z,
                                (uint32_t)size.width, (uint32_t)size.height, (uint32_t)(size.depth ? size.depth : 1)))
        nvlog("blit copyFromTexture: FAILED");
    else if (overwrite) nvmtl_surface_overwrite_recorded(_cb, d);
}
- (void)sampleCountersInBuffer:(id)sb atSampleIndex:(NSUInteger)i withBarrier:(BOOL)b { nvmtl_sample_counter(_cb, sb, i, self, b); }
- (void)resolveCounters:(id)sb inRange:(NSRange)r destinationBuffer:(id<MTLBuffer>)dst destinationOffset:(NSUInteger)off {
    NVMTLBuffer *b = (NVMTLBuffer *)dst;
    if (!b || !r.length) { nvlog("resolveCounters: no destination buffer or empty range"); nvmtl_counter_recording_failed(_cb); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:b];
    NVMTLCounterSampleBuffer *s = [sb isKindOfClass:[NVMTLCounterSampleBuffer class]] ? sb : nil;
    if (!s || r.location > s->_count || r.length > s->_count - r.location) {
        nvlog("resolveCounters: %s, range %lu+%lu of %lu samples - not resolved", s ? "range outside the buffer" : "not a sample buffer this device made",
              (unsigned long)r.location, (unsigned long)r.length, s ? (unsigned long)s->_count : 0ul);
        nvmtl_counter_recording_failed(_cb); return;
    }
    [_cb->_resources addObject:s];
    if (nvmtl_vk_cmd_ts_resolve(&_cb->_c, s->_pool, (uint32_t)r.location, (uint32_t)r.length, &b->_b, off))
        nvmtl_counter_recording_failed(_cb);
}
- (void)generateMipmapsForTexture:(id<MTLTexture>)tex {
    NVMTLTexture *t = (NVMTLTexture *)tex;
    if (!t) { nvlog("generateMipmapsForTexture: nil"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:t];
    { int was = nvtrace_scope([t nvi]->w, [t nvi]->h); nvtrace("MIPS tex %ux%u surf %u", [t nvi]->w, [t nvi]->h, nvmtl_surf_id(t)); g_trace_mute = was; }
    (t->_parent ?: t)->_mipsWritten = YES;
    nvmtl_surface_in_for_cb(_cb, t); nvmtl_surf_dirty(_cb, t);
    if (nvmtl_vk_cmd_image_gen_mipmaps(&_cb->_c, [t nvi])) nvlog("generateMipmapsForTexture: FAILED");
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl toTexture:(id<MTLTexture>)dst
       destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl sliceCount:(NSUInteger)nslices levelCount:(NSUInteger)nlevels {
    NVMTLTexture *s = (NVMTLTexture *)src, *d = (NVMTLTexture *)dst;
    if (!s || !d) { nvlog("blit copyFromTexture:sliceCount: nil"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:s]; [_cb->_resources addObject:d];
    for (NSUInteger k = 0; k < (nslices ? nslices : 1); k++)
        for (NSUInteger l = 0; l < (nlevels ? nlevels : 1); l++) {
            if (sl + l >= s.mipmapLevelCount || dl + l >= d.mipmapLevelCount) { nvlog("blit sliceCount: mip outside view"); return; }
            NSUInteger w = MAX((NSUInteger)1, s.width >> (sl + l)), h = MAX((NSUInteger)1, s.height >> (sl + l));
            NSUInteger dp = s.textureType == MTLTextureType3D ? MAX((NSUInteger)1, s.depth >> (sl + l)) : 1;
            [self copyFromTexture:s sourceSlice:ss + k sourceLevel:sl + l sourceOrigin:MTLOriginMake(0,0,0)
                      sourceSize:MTLSizeMake(w,h,dp) toTexture:d destinationSlice:ds + k destinationLevel:dl + l destinationOrigin:MTLOriginMake(0,0,0)];
        }
}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t {}
- (void)optimizeContentsForGPUAccess:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t {}
- (void)optimizeContentsForCPUAccess:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l {}
// A GPU-encodable ICB's records, slots [lo, lo + n), as the CPU state now has them, written again on the GPU timeline:
// a shader may have written a slot earlier in this command buffer (or after the CPU encoded), and a blit reset or copy
// happens where the blit is, not when it is encoded. The snapshot goes through a host-visible slice the command buffer holds.
- (void)nvmtlUploadRecords:(NVMTLIndirectCommandBuffer *)b from:(NSUInteger)lo count:(NSUInteger)n {
    nvmtl_icb_record *rec = [b nvmtlRecords];
    if (!rec || !n) return;
    const size_t bytes = n * (size_t)NVMTL_ICB_RECORD_BYTES;
    size_t off = 0; NVMTLBuffer *st = nvmtl_bytes_ring(_cb, bytes, &off);
    if (!st) {
        st = [NVMTLBuffer new]; off = 0;
        if (nvmtl_vk_buffer_create(bytes, 1, &st->_b) || !st->_b.map) { nvlog("blit: ICB record staging of %zu bytes REFUSED - the reset/copy reaches the records only from the CPU", bytes); return; }
        if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new];
        [_cb->_scratch addObject:st];
    }
    memcpy((uint8_t *)st->_b.map + off, rec + lo, bytes);
    nvmtl_retain_resource(_cb, b); nvmtl_retain_resource(_cb, b->_rec);
    if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &st->_b, off, &b->_rec->_b, lo * (size_t)NVMTL_ICB_RECORD_BYTES, bytes))
        nvlog("blit: ICB record upload FAILED");
}
- (void)resetCommandsInBuffer:(id<MTLIndirectCommandBuffer>)icb withRange:(NSRange)r {
    if ([(id)icb isKindOfClass:[NVMTLIndirectCommandBuffer class]]) {
        NVMTLIndirectCommandBuffer *b = (NVMTLIndirectCommandBuffer *)icb;
        [b resetWithRange:r];
        if (b->_rec && r.location < b->_cmds.count) [self nvmtlUploadRecords:b from:r.location count:MIN(r.length, b->_cmds.count - r.location)];
    }
    else nvlog("blit resetCommandsInBuffer: a %s is not our indirect command buffer — ignored", object_getClassName(icb));
}
- (void)copyIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)src sourceRange:(NSRange)r destination:(id<MTLIndirectCommandBuffer>)dst destinationIndex:(NSUInteger)di {
    if (![(id)src isKindOfClass:[NVMTLIndirectCommandBuffer class]] || ![(id)dst isKindOfClass:[NVMTLIndirectCommandBuffer class]]) {
        nvlog("blit copyIndirectCommandBuffer: not our indirect command buffers — ignored"); return; }
    NVMTLIndirectCommandBuffer *s = (NVMTLIndirectCommandBuffer *)src, *d = (NVMTLIndirectCommandBuffer *)dst;
    if (r.location + r.length > s->_cmds.count || di + r.length > d->_cmds.count) {
        nvlog("blit copyIndirectCommandBuffer: range [%lu,+%lu) -> %lu is outside the buffers (%lu / %lu slots) — ignored", (unsigned long)r.location,
              (unsigned long)r.length, (unsigned long)di, (unsigned long)s->_cmds.count, (unsigned long)d->_cmds.count); return; }
    for (NSUInteger k = 0; k < r.length; k++) [d->_cmds[di + k] nvmtlCopyFrom:s->_cmds[r.location + k]];
    if (s->_rec && d->_rec && r.length) {
        // what a shader wrote into the source slots travels with them, on the GPU timeline
        nvmtl_retain_resource(_cb, s); nvmtl_retain_resource(_cb, s->_rec); nvmtl_retain_resource(_cb, d); nvmtl_retain_resource(_cb, d->_rec);
        if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &s->_rec->_b, r.location * (size_t)NVMTL_ICB_RECORD_BYTES, &d->_rec->_b,
                                     di * (size_t)NVMTL_ICB_RECORD_BYTES, r.length * (size_t)NVMTL_ICB_RECORD_BYTES))
            nvlog("blit copyIndirectCommandBuffer: record copy FAILED");
    } else if (d->_rec) [self nvmtlUploadRecords:d from:di count:r.length];
    else if (s->_rec) { static int said; if (!said++) nvlog("blit copyIndirectCommandBuffer: the source is GPU-encodable and the destination is not - "
                                                          "only what the CPU encoded is copied (said once)"); }
}
- (void)optimizeIndirectCommandBuffer:(id<MTLIndirectCommandBuffer>)icb withRange:(NSRange)r {}
- (void)copyFromTexture:(id<MTLTexture>)src toTexture:(id<MTLTexture>)dst {
    NVMTLTexture *s = (NVMTLTexture *)src;
    if (!s) { nvlog("blit copyFromTexture:toTexture: nil"); return; }
    [self copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0)
               sourceSize:MTLSizeMake(s.width, s.height, s.textureType == MTLTextureType3D ? s.depth : 1) toTexture:dst
         destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)row
   sourceBytesPerImage:(NSUInteger)img sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
      destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dof {
    NVMTLBuffer *b = (NVMTLBuffer *)src; NVMTLTexture *t = (NVMTLTexture *)dst;
    if (!b || !t) { nvlog("blit copyFromBuffer:toTexture: nil"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:b];
    [_cb->_resources addObject:t];
    { nvk_image *ti = [t nvi]; uint32_t tl = (uint32_t)nvmtl_texture_slice_count(t), tm = (uint32_t)t.mipmapLevelCount; BOOL vol = ti->mtl_type == 7;
      if (ds >= tl || dl >= tm || (vol ? (dof.z + size.depth > MAX((NSUInteger)1, t.depth >> dl)) : (size.depth != 1 || dof.z))) {
          nvlog("blit copyFromBuffer:toTexture: slice %lu/%u level %lu/%u depth %lu@z%lu — outside this texture", (unsigned long)ds, tl, (unsigned long)dl, tm, (unsigned long)size.depth, (unsigned long)dof.z); return; } }
    { int was = nvtrace_scope([t nvi]->w, [t nvi]->h);
      if (nvmtl_trace_on()) { unsigned nz = 0, nn = 0; size_t valid = (size_t)size.width * [t nvi]->bpp; if (row && valid > row) valid = row;
        if (b->_b.map && size.height && so + (size_t)(size.height - 1) * row + valid <= b->_b.size)
            nvmtl_nonzero((const char *)b->_b.map + so, row, valid, (uint32_t)size.height, &nz, &nn);
        nvtrace("BLIT buf len %lu off %lu row %lu (%lux%lu) -> tex %ux%u surf %u (%lu,%lu) | nonzero %u/%u sampled bytes", (unsigned long)b->_b.size, (unsigned long)so,
            (unsigned long)row, (unsigned long)size.width, (unsigned long)size.height, [t nvi]->w, [t nvi]->h, nvmtl_surf_id(t), (unsigned long)dof.x, (unsigned long)dof.y, nz, nn); }
      g_trace_mute = was; }
    const BOOL overwrite = nvmtl_surface_full_overwrite(t, (MTLRegion){dof, size}, dl, ds);
    if (!overwrite) nvmtl_surface_in_for_cb(_cb, t);
    nvmtl_surf_dirty(_cb, t);
    { BOOL vol = [t nvi]->mtl_type == 7; NSUInteger nz = vol ? (size.depth ? size.depth : 1) : 1; NSUInteger per = img ? img : row * size.height; int rc = 0;
      for (NSUInteger zi = 0; zi < nz && !rc; zi++)
          rc = nvmtl_vk_cmd_copy_buffer_to_image_level_layer(&_cb->_c, &b->_b, so + zi * per, (uint32_t)row, [t nvi],
                                      (uint32_t)dof.x, (uint32_t)dof.y, (uint32_t)size.width, (uint32_t)size.height, (uint32_t)dl + t->_baseLevel, (uint32_t)(vol ? dof.z + zi : ds + t->_baseSlice));
      if (rc) nvlog("blit copyFromBuffer:toTexture: FAILED (level %lu slice %lu; pf %lu %lux%lu mips %lu; buffer %lu B off %lu row %lu img %lu; region (%lu,%lu) %lux%lux%lu)",
                    (unsigned long)dl, (unsigned long)ds, (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height,
                    (unsigned long)t.mipmapLevelCount, (unsigned long)b->_b.size, (unsigned long)so, (unsigned long)row, (unsigned long)img,
                    (unsigned long)dof.x, (unsigned long)dof.y, (unsigned long)size.width, (unsigned long)size.height, (unsigned long)size.depth);
      else if (overwrite) nvmtl_surface_overwrite_recorded(_cb, t);
      { NVMTLTexture *r = t->_parent ?: t;
        if (dl + t->_baseLevel > 0) r->_mipsWritten = YES;
        else if (!rc && !vol && !t->_parent && !r->_mipsWritten && t.mipmapLevelCount > 1 && !dof.x && !dof.y && size.width == [t nvi]->w && size.height == [t nvi]->h && [t nvi]->bw <= 1
                 && nvmtl_vk_cmd_image_gen_mipmaps(&_cb->_c, [t nvi])) nvlog("blit copyFromBuffer:toTexture: mip generation FAILED"); } }
}
- (void)copyFromBuffer:(id<MTLBuffer>)src sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)row
   sourceBytesPerImage:(NSUInteger)img sourceSize:(MTLSize)size toTexture:(id<MTLTexture>)dst
      destinationSlice:(NSUInteger)ds destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dof
               options:(MTLBlitOption)opt {
    const uint32_t asp = (opt & MTLBlitOptionDepthFromDepthStencil) ? 2u : (opt & MTLBlitOptionStencilFromDepthStencil) ? 4u : 0u;
    if (opt & ~(MTLBlitOptionDepthFromDepthStencil | MTLBlitOptionStencilFromDepthStencil))
        nvlog("blit copyFromBuffer:toTexture:options:%lu — only the depth/stencil options are implemented", (unsigned long)opt);
    nvmtl_vk_copy_aspect_override(asp, asp == 2u ? 4u : asp == 4u ? 1u : 0u);
    [self copyFromBuffer:src sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:size
               toTexture:dst destinationSlice:ds destinationLevel:dl destinationOrigin:dof];
    nvmtl_vk_copy_aspect_override(0, 0);
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst
      destinationOffset:(NSUInteger)dof destinationBytesPerRow:(NSUInteger)row
destinationBytesPerImage:(NSUInteger)img {
    NVMTLTexture *t = (NVMTLTexture *)src; NVMTLBuffer *b = (NVMTLBuffer *)dst;
    if (!t || !b) { nvlog("blit copyFromTexture:toBuffer: nil"); return; }
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:t];
    [_cb->_resources addObject:b];
    { nvk_image *ti = [t nvi]; uint32_t tl = (uint32_t)nvmtl_texture_slice_count(t), tm = (uint32_t)t.mipmapLevelCount; BOOL vol = ti->mtl_type == 7;
      if (ss >= tl || sl >= tm || (vol ? (so.z + size.depth > MAX((NSUInteger)1, t.depth >> sl)) : (size.depth != 1 || so.z))) {
          nvlog("blit copyFromTexture:toBuffer: slice %lu/%u level %lu/%u depth %lu@z%lu — outside this texture", (unsigned long)ss, tl, (unsigned long)sl, tm, (unsigned long)size.depth, (unsigned long)so.z); return; } }
    nvmtl_surface_in_for_cb(_cb, t);
    nvmtl_census("copy texture->buffer", [t nvi]->w, [t nvi]->h, t->_surf != NULL);
    { int was = nvtrace_scope([t nvi]->w, [t nvi]->h);
      nvtrace("BLIT tex %ux%u surf %u (%lu,%lu %lux%lu) -> buf len %lu off %lu row %lu", [t nvi]->w, [t nvi]->h, nvmtl_surf_id(t), (unsigned long)so.x, (unsigned long)so.y,
          (unsigned long)size.width, (unsigned long)size.height, (unsigned long)b->_b.size, (unsigned long)dof, (unsigned long)row);
      g_trace_mute = was; }
    { BOOL vol = [t nvi]->mtl_type == 7; NSUInteger nz = vol ? (size.depth ? size.depth : 1) : 1; NSUInteger per = img ? img : row * size.height; int rc = 0;
      for (NSUInteger zi = 0; zi < nz && !rc; zi++)
          rc = nvmtl_vk_cmd_copy_image_to_buffer_level_layer(&_cb->_c, [t nvi], (uint32_t)so.x, (uint32_t)so.y,
                                      (uint32_t)size.width, (uint32_t)size.height, &b->_b, dof + zi * per, (uint32_t)row, (uint32_t)sl + t->_baseLevel, (uint32_t)(vol ? so.z + zi : ss + t->_baseSlice));
      if (rc) nvlog("blit copyFromTexture:toBuffer: FAILED (level %lu slice %lu)", (unsigned long)sl, (unsigned long)ss); }
}
- (void)copyFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl
           sourceOrigin:(MTLOrigin)so sourceSize:(MTLSize)size toBuffer:(id<MTLBuffer>)dst
      destinationOffset:(NSUInteger)dof destinationBytesPerRow:(NSUInteger)row
destinationBytesPerImage:(NSUInteger)img options:(MTLBlitOption)opt {
    const uint32_t asp = (opt & MTLBlitOptionDepthFromDepthStencil) ? 2u : (opt & MTLBlitOptionStencilFromDepthStencil) ? 4u : 0u;
    if (opt & ~(MTLBlitOptionDepthFromDepthStencil | MTLBlitOptionStencilFromDepthStencil))
        nvlog("blit copyFromTexture:toBuffer:options:%lu — only the depth/stencil options are implemented", (unsigned long)opt);
    nvmtl_vk_copy_aspect_override(asp, asp == 2u ? 4u : asp == 4u ? 1u : 0u);
    [self copyFromTexture:src sourceSlice:ss sourceLevel:sl sourceOrigin:so sourceSize:size toBuffer:dst
        destinationOffset:dof destinationBytesPerRow:row destinationBytesPerImage:img];
    nvmtl_vk_copy_aspect_override(0, 0);
}
- (void)updateFence:(id<MTLFence>)f {}
- (void)waitForFence:(id<MTLFence>)f { nvmtl_vk_cmd_barrier(&_cb->_c); }
- (void)pushDebugGroup:(NSString *)g { nvtrace("GROUP %s", g.UTF8String ?: "?"); }
- (void)popDebugGroup {}
- (void)insertDebugSignpost:(NSString *)s {}
- (void)synchronizeResource:(id<MTLResource>)r {
    if (!r) return;
    const uint32_t use0 = [(id)r isKindOfClass:[NVMTLBuffer class]] ? __atomic_load_n(&((NVMTLBuffer *)r)->_gpuUse, __ATOMIC_RELAXED) : 0;
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    [_cb->_resources addObject:r];
    if ([(id)r isKindOfClass:[NVMTLBuffer class]] && ((NVMTLBuffer *)r)->_shadow.map) {
        NVMTLBuffer *b = (NVMTLBuffer *)r;
        // Metal copies the whole buffer back. A buffer no command buffer took since its last synchronizeResource cannot hold
        // newer GPU bytes than that copy brought back (a CPU write since was uploaded from the shadow, so both agree): the
        // copy is skipped. Not for a buffer in a heap (useHeap: reaches it without taking it), nor before its first copy,
        // nor while its shadow is still owed its zero fill. Geekbench 7 Metal: 40% of the 22 GB it synchronizes in a run.
        // NVMTL_SYNC_SKIP=0: always copy.
        static int skipOn = -1;
        if (skipOn < 0) { const char *e = getenv("NVMTL_SYNC_SKIP"); skipOn = !(e && e[0] == '0'); nvlog("synchronizeResource: unchanged managed buffers %s (NVMTL_SYNC_SKIP=%s)", skipOn ? "are not copied again" : "are copied every time", e ? e : "unset"); }
        const BOOL unchanged = skipOn && b->_gpuSynced && use0 == b->_gpuUseSynced && !b->_heap && !b->_subHeap && !b->_aliased &&
                               !__atomic_load_n(&b->_zshadow, __ATOMIC_ACQUIRE);
        b->_gpuUseSynced = __atomic_load_n(&b->_gpuUse, __ATOMIC_RELAXED);
        // later binds in this command buffer find it a member: untag it so the next one takes the set's path and counts
        if (object_getClass(_cb->_resources) == [NVMTLResSet class]) {
            __unsafe_unretained NVMTLResSet *rs = (NVMTLResSet *)_cb->_resources;
            os_unfair_lock_lock(&rs->_lk);
            __atomic_store_n(&b->_syncEpoch, rs->_epoch, __ATOMIC_RELEASE); __atomic_store_n(&b->_rsTag, 0, __ATOMIC_RELEASE);
            for (unsigned q = 0; q < 8; q++) if (rs->_mru[q] == (__bridge const void *)b) rs->_mru[q] = NULL;
            os_unfair_lock_unlock(&rs->_lk);
        } else b->_gpuUseSynced--;   // (never today) no way to see later binds: the next synchronizeResource copies
        static _Atomic unsigned long long nSync, nSkip, bSkip;
        const unsigned long long k = atomic_fetch_add(&nSync, 1) + 1;
        if (unchanged) { atomic_fetch_add(&nSkip, 1); atomic_fetch_add(&bSkip, b->_b.size); }
        if ((k & 4095) == 0) nvlog("synchronizeResource: %llu managed buffer syncs, %llu skipped as unchanged (%llu MB not copied)", k, atomic_load(&nSkip), atomic_load(&bSkip) >> 20);
        if (!unchanged) {
            __atomic_store_n(&b->_zshadow, 0, __ATOMIC_RELEASE);
            b->_gpuSynced = 1;
            if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &b->_b, 0, &b->_shadow, 0, b->_b.size))
                [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                    userInfo:@{NSLocalizedDescriptionKey:@"Cannot encode managed buffer synchronization"}]];
        }
    }
    if ([(id)r isKindOfClass:[NVMTLTexture class]]) {
        NVMTLTexture *t = (NVMTLTexture *)r;
        NVMTLBuffer *bb = t->_backBuf ? t->_backBuf : (t->_parent ? t->_parent->_backBuf : nil);
        if (bb && bb->_shadow.map) {
            [_cb->_resources addObject:bb];
            __atomic_store_n(&bb->_zshadow, 0, __ATOMIC_RELEASE);
            if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &bb->_b, 0, &bb->_shadow, 0, bb->_b.size))
                [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain code:MTLCommandBufferErrorInternal
                    userInfo:@{NSLocalizedDescriptionKey:@"Cannot encode managed buffer-texture synchronization"}]];
        }
    }
}
- (void)synchronizeTexture:(id<MTLTexture>)t slice:(NSUInteger)s level:(NSUInteger)l { [self synchronizeResource:t]; }
// The encoder lets go of its command buffer when encoding ends, as Metal's own encoders do. An app's autorelease pool
// can keep an ended encoder alive for a long time (a benchmark loop that never drains), and a strong reference here kept
// every such command buffer, and every buffer it used, alive with it: VRAM filled until allocations were refused.
- (void)endEncoding { if (_cb) { if (_tsEnd) { nvmtl_ts_pass_end(_cb, self, _tsEnd); _tsEnd = nil; } _cb->_nEnd++; _cb = nil; } }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { id<MTLDevice> d = _cb ? [_cb device] : nil; return d ? d : (id<MTLDevice>)gNVMTLMainDevice; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)(_cb ? _cb : _owner); }   // SPI, see the render encoder; _cb is gone after endEncoding
@end

#pragma mark - command buffer / queue
#include "nvmtl_handler_arena.h"
#import "NVMTLEventSync.m"
@implementation NVMTLCommandBuffer
// Never a lock on the command buffer itself: apps may hold @synchronized(commandBuffer) around -waitUntilCompleted
// (Unity 6 does), and this runs on the execution queue that completes it. Vampire Survivors hung on its first frame
// (1.14.2-1.14.4, sample from a user: the execution queue in objc_sync_enter here, Unity's thread in waitUntilCompleted).
- (void)nvmtlReleaseCapacity {
    if (__atomic_exchange_n(&_capacityHeld, NO, __ATOMIC_ACQ_REL)) dispatch_semaphore_signal(_queue->_capacity);
}
- (void)nvmtlRecordEncodingError:(NSError *)error { if (!_cbError) _cbError = error; }
- (void)setProtectionOptions:(unsigned long long)o { if (o) { static int said; if (!said++) nvlog("setProtectionOptions: 0x%llx asked - no protected-content path; protectionOptions stays 0 (said once)", o); } }
- (unsigned long long)globalTraceObjectID { return nvmtl_global_trace_id(self); }
- (BOOL)retainedReferences { return YES; }
- (MTLCommandBufferErrorOption)errorOptions { return MTLCommandBufferErrorOptionNone; }
- (id<MTLLogContainer>)logs { return nil; }
- (unsigned long long)protectionOptions { return 0; }
- (instancetype)init {
    if ((self = [super init])) {
        _completionCondition = [NSCondition new];
        // Made here, never lazily: two threads encoding into one command buffer could each find it nil and store
        // their own set, and the set that lost was freed with the resources only it held (and under the other
        // thread's addObject:).
        _resources = [NVMTLResSet new];
        _callbackGroup = dispatch_group_create(); _scheduledGroup = dispatch_group_create();
    }
    return self;
}
// as on the queue, but for this command buffer only: holding the set holds its allocations, and the
// resource set is released when the command buffer completes
- (void)useResidencySet:(id<MTLResidencySet>)r { nvmtl_retain_resource(self, r); }
- (void)useResidencySets:(const id<MTLResidencySet> __unsafe_unretained [])r count:(NSUInteger)n {
    if (!r) return;
    for (NSUInteger i = 0; i < n; i++) nvmtl_retain_resource(self, r[i]);
}
- (NSMutableDictionary *)userDictionary {
    @synchronized ([NVMTLCommandBuffer class]) { if (!_userDictionary) _userDictionary = [NSMutableDictionary new]; return _userDictionary; }
}
- (id<MTLParallelRenderCommandEncoder>)parallelRenderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    nvmtl_g79_flush(self);
    NVMTLParallelRenderCommandEncoder *p = [NVMTLParallelRenderCommandEncoder new];
    _nEnc++;
    p->_cb = self; p->_desc = [d copy]; p->_subs = [NSMutableArray new]; p->_lock = [NSLock new];
    MTLSamplePosition copiedSample[8]; NSUInteger copiedCount = [d getSamplePositions:NULL count:0];
    if (copiedCount && copiedCount <= 8) [d getSamplePositions:copiedSample count:copiedCount];
    if (copiedCount <= 8) [p->_desc setSamplePositions:copiedSample count:copiedCount];

    nvlog("parallelRenderCommandEncoderWithDescriptor: -> %p (sub-encoders record, endEncoding replays into one pass)", (__bridge void *)p);
    return p;
}
- (id<MTLRenderCommandEncoder>)renderCommandEncoderWithDescriptor:(MTLRenderPassDescriptor *)d {
    nvmtl_g79_flush(self);
    NVMTLRenderCommandEncoder *e = [NVMTLRenderCommandEncoder new];
    _nEnc++;
    e->_tsEnd = nvmtl_ts_pass_begin(self, e, d.sampleBufferAttachments, YES);
    e->_cb = self; e->_target = (NVMTLTexture *)d.colorAttachments[0].texture;
    if (!_resources) _resources = [NVMTLResSet new];
    if (e->_target) [_resources addObject:e->_target];
    if (d.depthAttachment.texture) [_resources addObject:d.depthAttachment.texture];
    { MTLStoreAction sa = d.colorAttachments[0].storeAction;
      if ((sa == MTLStoreActionMultisampleResolve || sa == MTLStoreActionStoreAndMultisampleResolve || sa == MTLStoreActionUnknown) && d.colorAttachments[0].resolveTexture) {
          e->_resolve = (NVMTLTexture *)d.colorAttachments[0].resolveTexture; [_resources addObject:e->_resolve]; } }
    if (d.visibilityResultBuffer) [_resources addObject:d.visibilityResultBuffer];
    if (e->_target) { NVMTLTexture *sr = e->_target->_parent ? e->_target->_parent : e->_target;
        nvmtl_census("render target", [sr nvi]->w, [sr nvi]->h, sr->_surf != NULL);
        (void)nvtrace_scope([sr nvi]->w, [sr nvi]->h);
        nvtrace("PASS target %ux%u surf %u view %d load %lu store %lu clear (%.3g %.3g %.3g %.3g) depth %d", [sr nvi]->w, [sr nvi]->h,
            sr->_surf ? (unsigned)IOSurfaceGetID(sr->_surf) : 0u, e->_target->_parent != nil, (unsigned long)d.colorAttachments[0].loadAction,
            (unsigned long)d.colorAttachments[0].storeAction, d.colorAttachments[0].clearColor.red, d.colorAttachments[0].clearColor.green,
            d.colorAttachments[0].clearColor.blue, d.colorAttachments[0].clearColor.alpha, d.depthAttachment.texture != nil);
        if (sr->_surf) {
            if (d.colorAttachments[0].loadAction == MTLLoadActionClear) nvmtl_surface_overwrite_recorded(self, sr);
            else nvmtl_surface_in_for_cb(self, sr);
            nvmtl_surf_dirty(self, sr); } }
    const float defaultBlend[4] = { 0, 0, 0, 0 };
    nvmtl_vk_cmd_set_blend_color(&_c, defaultBlend);
    nvmtl_vk_cmd_set_line_width(&_c, 1.0f);
    (void)nvmtl_vk_cmd_set_polygon_mode(&_c, 0);
    // Metal encoder state starts fresh even when the command buffer also contains an earlier GL render pass.
    _c.restart_off = 0;
    nvmtl_vk_cmd_set_depth_bias(&_c, 0.0f, 0.0f, 0.0f);
    nvmtl_vk_cmd_set_provoking_last(&_c, 0);
    (void)nvmtl_vk_cmd_set_depth_clip(&_c, 1);
    e->_visBuf = (NVMTLBuffer *)d.visibilityResultBuffer;
    MTLClearColor cc = d.colorAttachments[0].clearColor;
    BOOL ca8 = d.colorAttachments[0].texture && d.colorAttachments[0].texture.pixelFormat == MTLPixelFormatA8Unorm;   // alpha lives in the red lane
    e->_clear[0] = ca8 ? (float)cc.alpha : (float)cc.red; e->_clear[1] = (float)cc.green; e->_clear[2] = (float)cc.blue; e->_clear[3] = (float)cc.alpha;
    if (![e nvmtlBuildPass:d]) e->_refused = YES;
    return e;
}
- (void)nvmtlRecordDrawable:(id<MTLDrawable>)drawable presentation:(dispatch_block_t)action {
    if (!drawable || _committed) return;
    {
        extern void nvmtl_vk_submit_wait_stats(uint64_t *total_ns, uint64_t *waits);
        static int off = -1;
        if (off < 0) { const char *e = getenv("NVMTL_NO_FPS"); off = (NVMTL_RELEASE && !nvmtl_process_env_on()) ? 1 : (e && *e && *e != '0') ? 1 : 0; }
        if (!off) {
            static _Atomic uint64_t frames, t0, lastn, lastwns, lastwaits;
            const uint64_t n = atomic_fetch_add(&frames, 1) + 1;
            const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
            uint64_t t = atomic_load(&t0);
            if (!t) atomic_store(&t0, now);
            else if (now - t >= 2000000000ull) {
                uint64_t wns = 0, waits = 0;
                nvmtl_vk_submit_wait_stats(&wns, &waits);
                const uint64_t dn = n - atomic_load(&lastn);
                const uint64_t dwns = wns - atomic_load(&lastwns);
                const uint64_t dwaits = waits - atomic_load(&lastwaits);
                const double secs = (double)(now - t) / 1e9;
                nvlog("fps: %.2f over %.1f s (%llu frames, %llu total); CPU blocked on the GPU %.1f%% of wall "
                      "across %llu submits (%.2f ms each, %.1f submits per frame)",
                      (double)dn / secs, secs, (unsigned long long)dn, (unsigned long long)n,
                      100.0 * (double)dwns / (double)(now - t), (unsigned long long)dwaits,
                      dwaits ? (double)dwns / (double)dwaits / 1e6 : 0.0,
                      dn ? (double)dwaits / (double)dn : 0.0);
                {
                    static FILE *ff; static long fbytes;
                    if (!ff) { char fp[512]; const char *td = getenv("TMPDIR"); snprintf(fp, sizeof fp, "%s/nvmtl-fps-%d.log", td && *td ? td : "/tmp", (int)getpid()); ff = fopen(fp, "a"); }
                    if (ff && fbytes < (1 << 20)) {
                        time_t tt = time(NULL); struct tm lt; localtime_r(&tt, &lt);
                        fbytes += fprintf(ff, "%02d:%02d:%02d fps %.2f frames %llu gpuwait %.1f%% submits/frame %.1f\n", lt.tm_hour, lt.tm_min, lt.tm_sec,
                                          (double)dn / secs, (unsigned long long)dn, 100.0 * (double)dwns / (double)(now - t), dn ? (double)dwaits / (double)dn : 0.0);
                        fflush(ff);
                    }
                }
                atomic_store(&t0, now); atomic_store(&lastn, n);
                atomic_store(&lastwns, wns); atomic_store(&lastwaits, waits);
            }
        }
    }
    if ([drawable respondsToSelector:@selector(texture)]) {
        _present = (NVMTLTexture *)[(id<CAMetalDrawable>)drawable texture];
        nvmtl_surf_dirty(self, _present);
    }
    if (!_presentations) _presentations = [NSMutableArray new];
    [_presentations addObject:[action copy]];
    _nPresent++;
}
// NVMTL_HITCH_MS: a Metal app's frame ends when the command buffer that presents it gets to the present
- (void)presentDrawable:(id<MTLDrawable>)drawable {
    [self nvmtlRecordDrawable:drawable presentation:^{ nvmtl_hitch_frame("present"); [drawable present]; }];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable atTime:(CFTimeInterval)t {
    [self nvmtlRecordDrawable:drawable presentation:^{ nvmtl_hitch_frame("present"); [drawable presentAtTime:t]; }];
}
- (void)presentDrawable:(id<MTLDrawable>)drawable afterMinimumDuration:(CFTimeInterval)d {
    [self nvmtlRecordDrawable:drawable presentation:^{ nvmtl_hitch_frame("present"); [drawable presentAfterMinimumDuration:d]; }];
}

- (void)pushDebugGroup:(NSString *)g { nvtrace("GROUP %s", g.UTF8String ?: "?"); }
- (void)popDebugGroup {}
- (void)encodeSignalEvent:(id<MTLEvent>)e value:(uint64_t)v { [self nvmtlRecordSignal:e value:v]; }
- (void)encodeWaitForEvent:(id<MTLEvent>)e value:(uint64_t)v { [self nvmtlRecordWait:e value:v]; }
// The display-pipe swap follows this call immediately in WindowServer. NVAccel
// cannot order that swap after an NVK fence, so wait for the pixels to finish
// before allowing the surface to become scanout. Other applications keep the
// normal submission-only contract.
- (BOOL)commitAndWaitUntilSubmitted {
    static int ws = -1;
    if (ws < 0) ws = (getprogname() && !strcmp(getprogname(), "WindowServer")) ? 1 : 0;
    [self commit];
    // nvmtl_ws_kernel_waits: the swap waits for this composite in NVAccel instead (nvmtl_ws_mark_pending at commit);
    // a capture is read with the CPU as soon as this returns (nvmtl_ws_note_captures)
    if ((ws && !nvmtl_ws_kernel_waits()) || _wsCaptureWait) [self waitUntilCompleted]; else [self waitUntilScheduled];
    return self.status != MTLCommandBufferStatusError;
}
- (void)commit {
    [self nvmtlSubmitWithEvents];
    // NVMTL_DEBUG_WAIT_EACH=1: every command buffer is waited for and its completion traced, so a GPU hang is pinned to
    // the command buffer (and the debug groups traced before it) that never finished. A diagnostic: it serialises the app.
    static int each = -1; if (each < 0) { const char *e = getenv("NVMTL_DEBUG_WAIT_EACH"); each = e && e[0] == '1'; }
    if (each) { static _Atomic unsigned long n; const unsigned long k = ++n; nvtrace("CB commit #%lu %s", k, _label.UTF8String ?: "");
        [self waitUntilCompleted]; nvtrace("CB done #%lu status %ld", k, (long)self.status); }
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoder {
    NVMTLComputeCommandEncoder *e = [NVMTLComputeCommandEncoder new]; e->_cb = self; _nEnc++; return e;
}
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDispatchType:(MTLDispatchType)t {
    NVMTLComputeCommandEncoder *e = (NVMTLComputeCommandEncoder *)[self computeCommandEncoder];
    e->_e78Concurrent = t == MTLDispatchTypeConcurrent; return e; }
- (id<MTLComputeCommandEncoder>)computeCommandEncoderWithDescriptor:(MTLComputePassDescriptor *)d {
    NVMTLComputeCommandEncoder *e = (NVMTLComputeCommandEncoder *)[self computeCommandEncoderWithDispatchType:d ? d.dispatchType : MTLDispatchTypeSerial];
    if (d) e->_tsEnd = nvmtl_ts_pass_begin(self, e, d.sampleBufferAttachments, NO);
    return e; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoderWithDescriptor:(MTLBlitPassDescriptor *)d {
    NVMTLBlitCommandEncoder *e = (NVMTLBlitCommandEncoder *)[self blitCommandEncoder];
    if (d) e->_tsEnd = nvmtl_ts_pass_begin(self, e, d.sampleBufferAttachments, NO);
    return e; }
- (id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoder { return nil; }
- (id<MTLResourceStateCommandEncoder>)resourceStateCommandEncoderWithDescriptor:(MTLResourceStatePassDescriptor *)d { return nil; }
- (id<MTLBlitCommandEncoder>)blitCommandEncoder {
    nvmtl_g79_flush(self);
    NVMTLBlitCommandEncoder *e = [NVMTLBlitCommandEncoder new]; e->_cb = self; e->_owner = self; _nEnc++; return e;
}
- (id<MTLAccelerationStructureCommandEncoder>)accelerationStructureCommandEncoder {
    nvmtl_g79_flush(self);
    NVMTLAccelerationStructureCommandEncoder *e = [NVMTLAccelerationStructureCommandEncoder new]; e->_cb = self; e->_owner = self; _nEnc++; return e;
}
- (id<MTLAccelerationStructureCommandEncoder>)accelerationStructureCommandEncoderWithDescriptor:(MTLAccelerationStructurePassDescriptor *)d {
    NVMTLAccelerationStructureCommandEncoder *e = (NVMTLAccelerationStructureCommandEncoder *)[self accelerationStructureCommandEncoder];
    if (d) e->_tsEnd = nvmtl_ts_pass_begin(self, e, d.sampleBufferAttachments, NO);
    return e; }
- (void)addCompletedHandler:(MTLCommandBufferHandler)h {
    if (!h) return;
    // copied once and kept as a retained pointer: no array insertion and no extra retain/release pair per handler; the
    // array is built on the completion thread (nvmtlFinishCallbacks)
    // glcpu3: a helper-free stack block is copied into this command buffer's arena instead (nvmtl_handler_arena.h)
    if (nvmtl_handler_arena_on()) {
        [_completionCondition lock];
        if (_hn == _hcap) {
            const unsigned cap = _hcap ? _hcap * 2 : 64;
            void **g = (void **)realloc(_hv, cap * sizeof *g);
            if (!g) { [_completionCondition unlock]; nvlog("addCompletedHandler: out of memory - handler dropped"); return; }
            _hv = g; _hcap = cap;
        }
        nvmtl_hchunk *head = (nvmtl_hchunk *)_hchunks;
        void *e = nvmtl_harena_copy(&head, (__bridge const void *)h);
        _hchunks = head;
        if (e) { _hv[_hn++] = e; _nHandlers++; [_completionCondition unlock]; return; }
        [_completionCondition unlock];
    }
    void *blk = (__bridge_retained void *)[h copy];
    [_completionCondition lock];
    if (_hn == _hcap) {
        const unsigned cap = _hcap ? _hcap * 2 : 64;
        void **g = (void **)realloc(_hv, cap * sizeof *g);
        if (!g) { [_completionCondition unlock]; CFBridgingRelease(blk);
                  nvlog("addCompletedHandler: out of memory - handler dropped"); return; }
        _hv = g; _hcap = cap;
    }
    _hv[_hn++] = blk; _nHandlers++;
    [_completionCondition unlock];
}
- (void)addScheduledHandler:(MTLCommandBufferHandler)h {
    if (!h) return;
    [_completionCondition lock];
    if (!_scheduledHandlers) _scheduledHandlers = [NSMutableArray new];
    [_scheduledHandlers addObject:[h copy]];
    [_completionCondition unlock];
}
- (void)enqueue {
    if (nvmtl_lock_v2()) {   // NVMTL_LOCK_V2: the queue's place under the state lock (NVMTLEventSync.m)
        NVMTLQueueReservation *mine = _reservation ? nil : [NVMTLQueueReservation new];
        if (mine) mine->buffer = self;
        nvmtl_plock_lock(&gV2State);
        _nEnqueue++;
        if (!_reservation && mine && !_committed) {
            if (!_queue->_reservations) _queue->_reservations = [NSMutableArray new];
            [_queue->_reservations addObject:mine]; _reservation = mine;
        }
        os_unfair_lock_unlock(&gV2State);
        [_completionCondition lock];
        if (_cbstatus == MTLCommandBufferStatusNotEnqueued) _cbstatus = MTLCommandBufferStatusEnqueued;
        [_completionCondition unlock];
        return;
    }
    @synchronized ([NVMTLCommandBuffer class]) {
        _nEnqueue++; [self nvmtlReserveQueue];
        [_completionCondition lock];
        if (_cbstatus == MTLCommandBufferStatusNotEnqueued) _cbstatus = MTLCommandBufferStatusEnqueued;
        [_completionCondition unlock];
    }
}
/* glcpu4 NVMTL_UNCOMMITTED_HANDLERS: a command buffer released without ever being enqueued or committed runs its handlers
   at release, as Apple's driver does (Apple M1 Pro, arm64 and x86_64: all scheduled handlers, then all completed handlers,
   in the order added, synchronously on the releasing thread, each passed the command buffer being released, status
   MTLCommandBufferStatusNotEnqueued, error nil). Metal's documentation only says the driver calls a completed handler
   after the GPU finishes the command buffer and does not cover a buffer never committed; Apple GL frees per-draw
   resource lists in its handlers, so a handler that never runs leaked them. An enqueued but uncommitted buffer is kept by
   Apple's queue and its handlers never run; ours is not kept, and its handlers are dropped unrun, as before. */
NVMTL_G4_FIX(nvmtl_uncommitted_handlers_on, "NVMTL_UNCOMMITTED_HANDLERS", "handlers of a command buffer released uncommitted run at release (Apple's behaviour)")
- (void)nvmtlRunHandlersOfUncommitted {
    [_completionCondition lock];
    NSArray *sched = _scheduledHandlers.count ? [_scheduledHandlers copy] : nil; [_scheduledHandlers removeAllObjects];
    NSArray *first = _handlers.count ? [_handlers copy] : nil; [_handlers removeAllObjects];
    void **v = _hv; const unsigned n = _hn; void *chunks = _hchunks;
    _hv = NULL; _hn = _hcap = 0; _hchunks = NULL;
    [_completionCondition unlock];
    static _Atomic unsigned long long ncb, nh;
    const unsigned long long k = atomic_fetch_add(&ncb, 1) + 1, h = atomic_fetch_add(&nh, sched.count + first.count + n) + sched.count + first.count + n;
    if (!(k & (k - 1))) nvlog("glcpu4: %llu command buffer(s) released uncommitted, %llu handler(s) run at release", k, h);
    for (MTLCommandBufferHandler handler in sched) handler(self);
    for (MTLCommandBufferHandler handler in first) handler(self);
    for (unsigned i = 0; i < n; i++) { nvmtl_handler_entry_call(v[i], self); nvmtl_handler_entry_drop(v[i]); }
    free(v); nvmtl_hchunks_put((nvmtl_hchunk *)chunks);
}
- (void)dealloc {
    if (!_committed && _cbstatus == MTLCommandBufferStatusNotEnqueued && nvmtl_uncommitted_handlers_on()) [self nvmtlRunHandlersOfUncommitted];
    for (unsigned k = 0; k < _hn; k++) nvmtl_handler_entry_drop(_hv[k]);   // handlers of a command buffer that never completed
    free(_hv); _hv = NULL; _hn = _hcap = 0;
    nvmtl_hchunks_put((nvmtl_hchunk *)_hchunks); _hchunks = NULL;
    nvmtl_res2_cb_done(self);
    if (!_committed && _c.cb) nvmtl_vk_cmd_abandon(&_c);
    [self nvmtlReleaseCapacity];
    if (nvmtl_lock_v2()) {
        // NVMTL_LOCK_V2: a completed command buffer let go of its reservation on the worker, and nothing waits for it
        // here: no lock and no pump (both were ~20% of the worker under cbcost, where the worker frees the command
        // buffers). An enqueued one released uncommitted gives its place up and the queue moves on.
        if (_reservation) {
            nvmtl_plock_lock(&gV2State);
            NSMutableArray *res = _queue->_reservations;
            const NSUInteger at = [res indexOfObjectIdenticalTo:_reservation];
            if (at != NSNotFound) [res removeObjectAtIndex:at];
            os_unfair_lock_unlock(&gV2State);
            _reservation = nil;
            nvmtl_pump_buffers();
        }
        return;
    }
    @synchronized ([NVMTLCommandBuffer class]) {
        [self nvmtlReleaseReservation];
        nvmtl_pump_buffers();
    }
}
// _completionCondition held: the device interval in host seconds, converted once
- (void)nvmtlResolveGPUTimes {
    if (!_gpuTimesPending) return;
    _gpuTimesPending = NO;
    double a = 0, b = 0; (void)nvmtl_vk_interval_seconds(&_gpuTiming, &a, &b);
    _gpuStart = a; _gpuEnd = b;
}
- (CFTimeInterval)GPUStartTime {
    [_completionCondition lock];
    if (_executionDone && !_cbError) [self nvmtlResolveGPUTimes];
    CFTimeInterval value = _executionDone && !_cbError ? _gpuStart : 0;
    [_completionCondition unlock]; return value;
}
- (CFTimeInterval)GPUEndTime {
    [_completionCondition lock];
    if (_executionDone && !_cbError) [self nvmtlResolveGPUTimes];
    CFTimeInterval value = _executionDone && !_cbError ? _gpuEnd : 0;
    [_completionCondition unlock]; return value;
}
- (CFTimeInterval)kernelStartTime { return _legacyKernelStart; }
- (CFTimeInterval)kernelEndTime { return _legacyKernelEnd; }
- (void)waitUntilCompleted {
    _nWait++; const uint64_t t = nvmtl_hitch_t0(); [_completionCondition lock];
    while (!_completionReady) [_completionCondition wait];
    [_completionCondition unlock];
    { extern int nvmtl_done_split_on(void); extern void nvmtl_done_split_woke(uint64_t);
      if (nvmtl_done_split_on()) nvmtl_done_split_woke(_doneSplitNs); }
    dispatch_group_wait(_callbackGroup, DISPATCH_TIME_FOREVER);
    nvmtl_hitch_add(NVH_WAITCOMPLETED, t, 0);
}
- (void)waitUntilScheduled {
    _nWait++; const uint64_t t = nvmtl_hitch_t0(); [_completionCondition lock];
    extern void nvmtl_sc_wait_done(uint64_t ns);
    const uint64_t sc0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    while (!_wasScheduled && !_executionDone) [_completionCondition wait];
    [_completionCondition unlock];
    dispatch_group_wait(_scheduledGroup, DISPATCH_TIME_FOREVER);
    nvmtl_sc_wait_done(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - sc0);
    nvmtl_hitch_add(NVH_WAITSCHEDULED, t, 0);
}
- (MTLCommandBufferStatus)status { [_completionCondition lock]; MTLCommandBufferStatus s = _cbstatus; [_completionCondition unlock]; return s; }
- (NSError *)error { return _cbError; }
- (id<MTLCommandQueue>)commandQueue { return _queue; }
- (id<MTLDevice>)device { return _queue ? [_queue device] : nil; }
- (void)setLabel:(NSString *)l { _nLabel++; _label = [l copy]; }
- (NSString *)label { return _label; }
@end

@implementation NVMTLCommandQueue
- (NSUInteger)getGPUPriority { return 1; }
- (NSUInteger)getBackgroundGPUPriority { return 2; }
- (void)setSubmissionQueue:(id)q { static int said; if (!said++) nvlog("setSubmissionQueue: %s ignored - this backend has no submission queue (said once)", q ? "a queue" : "nil"); }
- (BOOL)setGPUPriority:(NSUInteger)priority {
    nvlog("queue: GPU priority hint %lu unavailable; retaining fixed backend priority", (unsigned long)priority);
    return NO;
}
- (BOOL)setBackgroundGPUPriority:(NSUInteger)priority {
    nvlog("queue: background GPU priority hint %lu unavailable; retaining fixed backend priority", (unsigned long)priority);
    return NO;
}
// Residency is implicit in this backend, so these only have to hold the set: a set retains the
// allocations added to it, so the app may hand the queue its only reference and release its own.
// They are called from any thread, hence the lock.
- (void)addResidencySet:(id<MTLResidencySet>)r {
    if (!r) return;
    @synchronized (self) { if (!_resSets) _resSets = [NSMutableArray new]; [_resSets addObject:r]; }
}
- (void)addResidencySets:(const id<MTLResidencySet> __unsafe_unretained [])r count:(NSUInteger)n {
    if (!r) return;
    for (NSUInteger i = 0; i < n; i++) [self addResidencySet:r[i]];
}
- (void)removeResidencySet:(id<MTLResidencySet>)r {
    if (!r) return;
    @synchronized (self) { [_resSets removeObject:r]; }
}
- (void)removeResidencySets:(const id<MTLResidencySet> __unsafe_unretained [])r count:(NSUInteger)n {
    if (!r) return;
    for (NSUInteger i = 0; i < n; i++) [self removeResidencySet:r[i]];
}
- (void)dealloc { nvmtl_vk_queue_destroy(&_q); }
- (id<MTLCommandBuffer>)commandBuffer {
    dispatch_semaphore_t cap = nil;
    if (nvmtl_async_commit_on()) {
        @synchronized ([NVMTLCommandQueue class]) { if (!_capacity) _capacity = dispatch_semaphore_create(_capacityCount ? _capacityCount : 64); cap = _capacity; }
        dispatch_semaphore_wait(cap, DISPATCH_TIME_FOREVER);
    }
    NVMTLCommandBuffer *cb = [NVMTLCommandBuffer new];
    if (!cb) { if (cap) dispatch_semaphore_signal(cap); return nil; }
    cb->_queue = self; cb->_capacityHeld = cap != nil;
    // The resource set exists from the start. Every encoder used to create it on first use
    // (`if (!cb->_resources) cb->_resources = [NVMTLResSet new]`), and encoders of one command buffer run on several
    // threads (parallel render encoders, CoreAnimation's async render queue): two threads both saw nil, the second
    // store released the set the first was adding to, and the app died in -[NVMTLResSet addObject:] (__rehash on a freed
    // member) or in its -dealloc - about 14 Macs from 1.15 to 2.0.1 (NotificationCenter, Music, mediaanalysisd, games).
    // The set itself takes a lock for every operation; only its creation was unguarded. The first-use checks now never fire.
    cb->_resources = [NVMTLResSet new];
    if (nvmtl_vk_cmd_begin(&_q, &cb->_c)) {
        // WindowServer asserts on this nil ("Failed to get a MTLCommandBuffer"); the cause is the "vk: command buffer
        // refused" line just above, and for a disabled backend the "disabling backend" line before it.
        nvlog("queue: command buffer begin FAILED in %s - the caller gets nil", getprogname());
        return nil;
    }
    return cb;
}
- (id<MTLCommandBuffer>)commandBufferWithUnretainedReferences { return [self commandBuffer]; }
- (id<MTLCommandBuffer>)commandBufferWithDescriptor:(MTLCommandBufferDescriptor *)d { return [self commandBuffer]; }
- (void)insertDebugCaptureBoundary {}
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (NSString *)label { return _label; }
- (id<MTLDevice>)device { return _dev; }
- (id)completionQueue { return _completionQueue; }
- (void)setCompletionQueue:(id)q { _completionQueue = q; }
- (NSUInteger)maxCommandBufferCount { return nvmtl_async_commit_on() && _capacityCount ? _capacityCount : 64; }
@end

#pragma mark - residency + events (2026-09-21, honest no-op objects)
@implementation NVMTLResidencySet
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (uint64_t)allocatedSize { uint64_t s = 0; for (id<MTLAllocation> a in _allocs) s += [a allocatedSize]; return s; }
- (void)requestResidency {}
- (void)endResidency {}
- (void)addAllocation:(id<MTLAllocation>)a { if (a) [_allocs addObject:a]; }
- (void)addAllocations:(const id<MTLAllocation> __unsafe_unretained [])a count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) if (a[i]) [_allocs addObject:a[i]]; }
- (void)removeAllocation:(id<MTLAllocation>)a { if (a) [_allocs removeObject:a]; }
- (void)removeAllocations:(const id<MTLAllocation> __unsafe_unretained [])a count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) if (a[i]) [_allocs removeObject:a[i]]; }
- (void)removeAllAllocations { [_allocs removeAllObjects]; }
- (BOOL)containsAllocation:(id<MTLAllocation>)a { return a && [_allocs containsObject:a]; }
- (NSArray<id<MTLAllocation>> *)allAllocations { return [_allocs copy]; }
- (NSUInteger)allocationCount { return _allocs.count; }
- (void)commit {}
@end
@implementation NVMTLEvent
- (instancetype)init { if ((self = [super init])) _condition = [NSCondition new]; return self; }
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (uint64_t)signaledValue { return nvmtl_event_read(_condition, &_value); }
- (void)setSignaledValue:(uint64_t)v { nvmtl_event_signal(self, _condition, &_value, nil, v); }
@end
@implementation NVMTLSharedEvent
- (instancetype)init { if ((self = [super init])) { _condition = [NSCondition new]; _notifications = [NSMutableArray new]; } return self; }
- (id<MTLDevice>)device { return _dev; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (uint64_t)signaledValue { return nvmtl_event_read(_condition, &_value); }
- (void)setSignaledValue:(uint64_t)v { nvmtl_event_signal(self, _condition, &_value, _notifications, v); }
- (MTLSharedEventHandle *)newSharedEventHandle {
    static int said; if (!said++) nvlog("newSharedEventHandle: -> nil (cross-process event sharing is not built; this event lives in pid %d only)", getpid());
    return nil; }
- (void)notifyListener:(MTLSharedEventListener *)l atValue:(uint64_t)v block:(MTLSharedEventNotificationBlock)b {
    nvmtl_event_notify(self, _condition, &_value, _notifications, l, v, b);
}
- (BOOL)waitUntilSignaledValue:(uint64_t)v timeoutMS:(uint64_t)ms {
    const uint64_t t = nvmtl_hitch_t0(); const BOOL r = nvmtl_event_wait(_condition, &_value, v, ms); nvmtl_hitch_add(NVH_EVENTWAIT, t, 0); return r;
}
@end

typedef enum : uint8_t {
    NVP_INV = 0, NVP_PIPE, NVP_DS, NVP_VBUF, NVP_FBUF, NVP_VTEX, NVP_FTEX, NVP_VSAMP, NVP_FSAMP,
    NVP_CULL, NVP_WIND, NVP_FILL, NVP_SREF, NVP_SREF2, NVP_VIEWPORT, NVP_SCISSOR, NVP_DEPTHBIAS, NVP_BLENDCOL,
    NVP_DRAWIDX, NVP_DRAWIDX5, NVP_DRAW, NVP_DRAW3, NVP_USERES, NVP_USERES3, NVP_VBYTES, NVP_FBYTES,
    NVP_VOFF, NVP_FOFF, NVP_DEPTHCLIP, NVP_VBUFST, NVP_VOFFST, NVP_USEHEAP, NVP_USEHEAP2
} nvp_kind;
typedef struct {
    nvp_kind kind;
    uint32_t i0, i1, i2;
    uint64_t u0, u1, u2, u3, u4, u5;
    double   f0, f1, f2, f3, f4, f5;
    void    *o0;
    const void *p0;
} nvp_cmd;
// _heaps: heaps this sub-encoder declared. Sub-encoders record on their own threads, so they are
// collected here under the sub's own lock and handed to the real encoder during the single-threaded
// replay in -endEncoding, which is what puts them on the command buffer (nvmtl_retain_heaps).
@interface NVMTLDeferredRenderCommandEncoder : NSProxy { @public __weak NVMTLParallelRenderCommandEncoder *_owner; NSMutableArray *_inv, *_keep, *_heaps; BOOL _ended; NSString *_label; nvp_cmd *_cmds; uint32_t _ncmd, _capcmd; }
@end
// Method signatures for the sub-encoder, by selector. The translated game asks the proxy for a signature before
// messaging it, and +instanceMethodSignatureForSelector: walks the class's protocol lists each time (class_copyProtocolList,
// a malloc, a hash probe): ~320 render-thread samples in 20 s. A signature never changes for a selector, so the first
// answer is kept. Reads take no lock: a slot's signature is published before its selector. NVMTL_PAR_SIGCACHE=0 turns it off.
#define NVP_SIG_SLOTS 512u
static struct { _Atomic(SEL) sel; void *sig; } nvp_sig_tab[NVP_SIG_SLOTS];
static os_unfair_lock nvp_sig_lock = OS_UNFAIR_LOCK_INIT;
static int nvp_sig_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_PAR_SIGCACHE"); on = !(e && e[0] == '0'); }
    return on;
}
static inline unsigned nvp_sig_slot(SEL s) { return (unsigned)(((uintptr_t)(const void *)s >> 3) * 0x9E3779B1u) & (NVP_SIG_SLOTS - 1); }
static NSMethodSignature *nvp_sig_cached(SEL s) {
    if (!s || !nvp_sig_on()) return nil;
    for (unsigned k = 0, i = nvp_sig_slot(s); k < NVP_SIG_SLOTS; k++, i = (i + 1) & (NVP_SIG_SLOTS - 1)) {
        SEL t = atomic_load_explicit(&nvp_sig_tab[i].sel, memory_order_acquire);
        if (t == s) return (__bridge NSMethodSignature *)nvp_sig_tab[i].sig;
        if (!t) return nil;
    }
    return nil;
}
static void nvp_sig_remember(SEL s, NSMethodSignature *m) {
    if (!s || !m || !nvp_sig_on()) return;
    os_unfair_lock_lock(&nvp_sig_lock);
    for (unsigned k = 0, i = nvp_sig_slot(s); k < NVP_SIG_SLOTS * 3 / 4; k++, i = (i + 1) & (NVP_SIG_SLOTS - 1)) {
        SEL t = atomic_load_explicit(&nvp_sig_tab[i].sel, memory_order_relaxed);
        if (t == s) break;
        if (!t) { nvp_sig_tab[i].sig = (__bridge_retained void *)m;   // held for the life of the process
                  atomic_store_explicit(&nvp_sig_tab[i].sel, s, memory_order_release); break; }
    }
    os_unfair_lock_unlock(&nvp_sig_lock);
}
@implementation NVMTLDeferredRenderCommandEncoder
static int nvp_fast_off(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("NVMTL_NO_PARFAST"); off = (e && *e && *e != '0') ? 1 : 0; }
    return off;
}
- (nvp_cmd *)nvpPush:(nvp_kind)k {
    if (_ncmd == _capcmd) {
        uint32_t cap = _capcmd ? _capcmd * 2 : 256;
        nvp_cmd *p = (nvp_cmd *)realloc(_cmds, (size_t)cap * sizeof(nvp_cmd));
        if (!p) return NULL;
        _cmds = p; _capcmd = cap;
    }
    nvp_cmd *c = &_cmds[_ncmd++];
    memset(c, 0, sizeof *c);
    c->kind = k;
    return c;
}
- (void)nvpKeep:(id)o {
    if (!o) return;
    if (!_keep) _keep = [NSMutableArray new];
    [_keep addObject:o];
}
#define NVP_BEGIN(K) if (nvp_fast_off()) { [(id)self nvpForwardSlow:_cmd]; return; } \
                     @synchronized (self) { nvp_cmd *c = [self nvpPush:(K)]; if (!c) return;
#define NVP_END      }
- (void)setRenderPipelineState:(id<MTLRenderPipelineState>)p {
    if (nvp_fast_off()) { [self nvpSlowObj:@selector(setRenderPipelineState:) obj:p]; return; }
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_PIPE]; if (!c) return; [self nvpKeep:p]; c->o0 = (__bridge void *)p; }
}
- (void)setDepthStencilState:(id<MTLDepthStencilState>)d {
    if (nvp_fast_off()) { [self nvpSlowObj:@selector(setDepthStencilState:) obj:d]; return; }
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DS]; if (!c) return; [self nvpKeep:d]; c->o0 = (__bridge void *)d; }
}
- (void)nvpBind:(nvp_kind)k obj:(id)o u0:(uint64_t)u0 idx:(uint64_t)i {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:k]; if (!c) return; [self nvpKeep:o]; c->o0 = (__bridge void *)o; c->u0 = u0; c->u1 = i; }
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self nvpBind:NVP_VBUF obj:b u0:o idx:i]; }
- (void)setFragmentBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i { [self nvpBind:NVP_FBUF obj:b u0:o idx:i]; }
- (void)setVertexBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { [self nvpBind:NVP_VOFF obj:nil u0:o idx:i]; }
// The strided and heap forms used to fall through to -forwardInvocation:, which costs a method-signature lookup, an
// NSInvocation, retainArguments, and an -invokeWithTarget: at replay - for a call the game makes per draw. Measured on
// Cyberpunk: ~1,100 render-thread samples in 20 s between recording and replay. NVMTL_NO_PARFAST=1 restores the old path.
- (void)nvpStrided:(nvp_kind)k obj:(id)o u0:(uint64_t)u0 stride:(uint64_t)st idx:(uint64_t)i {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:k]; if (!c) return; [self nvpKeep:o]; c->o0 = (__bridge void *)o; c->u0 = u0; c->u1 = i; c->u2 = st; }
}
- (void)setVertexBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    if (nvp_fast_off()) {
        SEL sel = @selector(setVertexBuffer:offset:attributeStride:atIndex:);
        NSMethodSignature *m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:sel]; if (!m) return;
        NSInvocation *inv = [NSInvocation invocationWithMethodSignature:m]; inv.selector = sel;
        [inv setArgument:&b atIndex:2]; [inv setArgument:&o atIndex:3]; [inv setArgument:&st atIndex:4]; [inv setArgument:&i atIndex:5];
        [self forwardInvocation:inv]; return;
    }
    [self nvpStrided:NVP_VBUFST obj:b u0:o stride:st idx:i];
}
- (void)setVertexBufferOffset:(NSUInteger)o attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    if (nvp_fast_off()) {
        SEL sel = @selector(setVertexBufferOffset:attributeStride:atIndex:);
        NSMethodSignature *m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:sel]; if (!m) return;
        NSInvocation *inv = [NSInvocation invocationWithMethodSignature:m]; inv.selector = sel;
        [inv setArgument:&o atIndex:2]; [inv setArgument:&st atIndex:3]; [inv setArgument:&i atIndex:4];
        [self forwardInvocation:inv]; return;
    }
    [self nvpStrided:NVP_VOFFST obj:nil u0:o stride:st idx:i];
}
- (void)setFragmentBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i { [self nvpBind:NVP_FOFF obj:nil u0:o idx:i]; }
- (void)setVertexTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self nvpBind:NVP_VTEX obj:t u0:0 idx:i]; }
- (void)setFragmentTexture:(id<MTLTexture>)t atIndex:(NSUInteger)i { [self nvpBind:NVP_FTEX obj:t u0:0 idx:i]; }
- (void)setVertexSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { [self nvpBind:NVP_VSAMP obj:s u0:0 idx:i]; }
- (void)setFragmentSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i { [self nvpBind:NVP_FSAMP obj:s u0:0 idx:i]; }
- (void)nvpScalar:(nvp_kind)k u0:(uint64_t)a u1:(uint64_t)b {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:k]; if (!c) return; c->u0 = a; c->u1 = b; }
}
- (void)setCullMode:(MTLCullMode)m { [self nvpScalar:NVP_CULL u0:(uint64_t)m u1:0]; }
- (void)setFrontFacingWinding:(MTLWinding)w { [self nvpScalar:NVP_WIND u0:(uint64_t)w u1:0]; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)f { [self nvpScalar:NVP_FILL u0:(uint64_t)f u1:0]; }
- (void)setDepthClipMode:(MTLDepthClipMode)m { [self nvpScalar:NVP_DEPTHCLIP u0:(uint64_t)m u1:0]; }
- (void)setStencilReferenceValue:(uint32_t)r { [self nvpScalar:NVP_SREF u0:r u1:0]; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { [self nvpScalar:NVP_SREF2 u0:f u1:b]; }
- (void)setViewport:(MTLViewport)v {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_VIEWPORT]; if (!c) return;
        c->f0 = v.originX; c->f1 = v.originY; c->f2 = v.width; c->f3 = v.height; c->f4 = v.znear; c->f5 = v.zfar; }
}
- (void)setScissorRect:(MTLScissorRect)r {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_SCISSOR]; if (!c) return;
        c->u0 = r.x; c->u1 = r.y; c->u2 = r.width; c->u3 = r.height; }
}
- (void)setDepthBias:(float)b slopeScale:(float)s clamp:(float)cl {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DEPTHBIAS]; if (!c) return; c->f0 = b; c->f1 = s; c->f2 = cl; }
}
- (void)setBlendColorRed:(float)r green:(float)g blue:(float)b alpha:(float)a {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_BLENDCOL]; if (!c) return; c->f0 = r; c->f1 = g; c->f2 = b; c->f3 = a; }
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)ibo instanceCount:(NSUInteger)ic
                   baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DRAWIDX]; if (!c) return; [self nvpKeep:ib];
        c->i0 = (uint32_t)t; c->i1 = (uint32_t)it; c->o0 = (__bridge void *)ib;
        c->u0 = n; c->u1 = ibo; c->u2 = ic; c->u3 = (uint64_t)bv; c->u4 = bi; }
}
- (void)drawIndexedPrimitives:(MTLPrimitiveType)t indexCount:(NSUInteger)n indexType:(MTLIndexType)it
                  indexBuffer:(id<MTLBuffer>)ib indexBufferOffset:(NSUInteger)ibo {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DRAWIDX5]; if (!c) return; [self nvpKeep:ib];
        c->i0 = (uint32_t)t; c->i1 = (uint32_t)it; c->o0 = (__bridge void *)ib; c->u0 = n; c->u1 = ibo; }
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n
         instanceCount:(NSUInteger)ic baseInstance:(NSUInteger)bi {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DRAW]; if (!c) return;
        c->i0 = (uint32_t)t; c->u0 = s; c->u1 = n; c->u2 = ic; c->u3 = bi; }
}
- (void)drawPrimitives:(MTLPrimitiveType)t vertexStart:(NSUInteger)s vertexCount:(NSUInteger)n {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_DRAW3]; if (!c) return; c->i0 = (uint32_t)t; c->u0 = s; c->u1 = n; }
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_USERES]; if (!c) return; [self nvpKeep:r]; c->o0 = (__bridge void *)r; c->u0 = u; }
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u stages:(MTLRenderStages)st {
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_USERES3]; if (!c) return; [self nvpKeep:r]; c->o0 = (__bridge void *)r; c->u0 = u; c->u1 = st; }
}
- (void)nvmtlRecordBytes:(SEL)sel bytes:(const void *)b length:(NSUInteger)l index:(NSUInteger)i {
    NSData *d = [NSData dataWithBytes:b length:l];
    @synchronized (self) {
        [self nvpKeep:d];
        nvp_cmd *c = [self nvpPush:(sel == @selector(setVertexBytes:length:atIndex:)) ? NVP_VBYTES : NVP_FBYTES];
        if (!c) return;
        c->p0 = d.bytes; c->u0 = l; c->u1 = i;
    }
}
- (void)setVertexBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self nvmtlRecordBytes:@selector(setVertexBytes:length:atIndex:) bytes:b length:l index:i]; }
- (void)setFragmentBytes:(const void *)b length:(NSUInteger)l atIndex:(NSUInteger)i { [self nvmtlRecordBytes:@selector(setFragmentBytes:length:atIndex:) bytes:b length:l index:i]; }
- (void)setVertexBytes:(const void *)b length:(NSUInteger)l attributeStride:(NSUInteger)st atIndex:(NSUInteger)i {
    SEL sel = @selector(setVertexBytes:length:attributeStride:atIndex:); NSData *d = [NSData dataWithBytes:b length:l];
    NSMethodSignature *m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:sel]; if (!m) return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:m]; inv.selector = sel;
    const void *p = d.bytes; [inv setArgument:&p atIndex:2]; [inv setArgument:&l atIndex:3]; [inv setArgument:&st atIndex:4]; [inv setArgument:&i atIndex:5];
    @synchronized (self) { if (!_keep) _keep = [NSMutableArray new]; [_keep addObject:d]; [_inv addObject:inv]; }
}
- (NSMethodSignature *)methodSignatureForSelector:(SEL)s {
    NSMethodSignature *m = nvp_sig_cached(s);
    if (m) return m;
    m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:s];
    if (!m) { nvlog("parallel sub-encoder: %s is not a render-encoder selector — recorded as a no-op", sel_getName(s)); m = [NSMethodSignature signatureWithObjCTypes:"v@:"]; }
    nvp_sig_remember(s, m);
    return m;
}
- (void)forwardInvocation:(NSInvocation *)inv {
    if (![NVMTLRenderCommandEncoder instancesRespondToSelector:inv.selector]) return;
    [inv retainArguments];
    @synchronized (self) {
        [_inv addObject:inv];
        nvp_cmd *c = [self nvpPush:NVP_INV]; (void)c;
        if (inv.selector == @selector(endEncoding)) _ended = YES;
    }
    { static const char *seen[48]; static unsigned n; const char *s = sel_getName(inv.selector);
      unsigned k = 0; for (; k < n; k++) if (seen[k] == s) break;
      if (k == n && n < 48) { seen[n++] = s; nvlog("parallel sub-encoder: %s still goes through NSInvocation", s); } }
}
- (void)nvpSlowObj:(SEL)sel obj:(id)o {
    NSMethodSignature *m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:sel]; if (!m) return;
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:m]; inv.selector = sel;
    [inv setArgument:&o atIndex:2];
    [self forwardInvocation:inv];
}
- (void)dealloc { free(_cmds); _cmds = NULL; }
- (BOOL)respondsToSelector:(SEL)s { return [NVMTLRenderCommandEncoder instancesRespondToSelector:s]; }
- (BOOL)conformsToProtocol:(Protocol *)p { return [NVMTLRenderCommandEncoder conformsToProtocol:p]; }
- (id<MTLDevice>)device { return gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (id<MTLCommandBuffer>)commandBuffer { NVMTLParallelRenderCommandEncoder *p = _owner; return p ? (id<MTLCommandBuffer>)p->_cb : nil; }   // SPI; answered here, not recorded for replay
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) if (b[k]) [(id)self setVertexBuffer:b[k] offset:o ? o[k] : 0 atIndex:r.location + k]; }
- (void)setVertexBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)o attributeStrides:(const NSUInteger *)s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) if (b[k]) [(id)self setVertexBuffer:b[k] offset:o ? o[k] : 0 attributeStride:s ? s[k] : MTLAttributeStrideStatic atIndex:r.location + k]; }
- (void)setFragmentBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)o withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) if (b[k]) [(id)self setFragmentBuffer:b[k] offset:o ? o[k] : 0 atIndex:r.location + k]; }
- (void)setVertexTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) if (t[k]) [(id)self setVertexTexture:t[k] atIndex:r.location + k]; }
- (void)setFragmentTextures:(const id<MTLTexture> __unsafe_unretained [])t withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) if (t[k]) [(id)self setFragmentTexture:t[k] atIndex:r.location + k]; }
- (void)setVertexSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [(id)self setVertexSamplerState:s[k] atIndex:r.location + k]; }
- (void)setFragmentSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r { for (NSUInteger k = 0; k < r.length; k++) [(id)self setFragmentSamplerState:s[k] atIndex:r.location + k]; }
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u { for (NSUInteger i = 0; i < n; i++) [(id)self useResource:r[i] usage:u]; }
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])r count:(NSUInteger)n usage:(MTLResourceUsage)u stages:(MTLRenderStages)st { for (NSUInteger i = 0; i < n; i++) [(id)self useResource:r[i] usage:u stages:st]; }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])h count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) [self useHeap:h[i]]; }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])h count:(NSUInteger)n stages:(MTLRenderStages)st { for (NSUInteger i = 0; i < n; i++) [self useHeap:h[i] stages:st]; }
- (void)useHeap:(id<MTLHeap>)h {
    if (nvp_fast_off()) { [self nvpSlowObj:@selector(useHeap:) obj:h]; return; }
    @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_USEHEAP]; if (!c) return; [self nvpKeep:h]; c->o0 = (__bridge void *)h; }
}
- (void)useHeap:(id<MTLHeap>)h stages:(MTLRenderStages)st {
    if (!nvp_fast_off()) {
        @synchronized (self) { nvp_cmd *c = [self nvpPush:NVP_USEHEAP2]; if (!c) return; [self nvpKeep:h]; c->o0 = (__bridge void *)h; c->u0 = st; }
        return;
    }
    SEL sel = @selector(useHeap:stages:);
    NSMethodSignature *m = [NVMTLRenderCommandEncoder instanceMethodSignatureForSelector:sel];
    NSInvocation *inv = [NSInvocation invocationWithMethodSignature:m]; inv.selector = sel;
    [inv setArgument:&h atIndex:2]; [inv setArgument:&st atIndex:3];
    [self forwardInvocation:inv];
}
@end
@implementation NVMTLParallelRenderCommandEncoder
- (id<MTLRenderCommandEncoder>)renderCommandEncoder {
    NVMTLDeferredRenderCommandEncoder *s = [NVMTLDeferredRenderCommandEncoder alloc];
    s->_owner = self; s->_inv = [NSMutableArray new];
    [_lock lock]; [_subs addObject:s]; [_lock unlock];
    return (id<MTLRenderCommandEncoder>)s;
}
- (void)endEncoding {
    if (_ended) { nvlog("parallel encoder: endEncoding twice"); return; }
    _ended = YES;
    [_lock lock]; NSArray *subs = [_subs copy]; [_subs removeAllObjects]; [_lock unlock];
    NVMTLRenderCommandEncoder *real = (NVMTLRenderCommandEncoder *)[_cb renderCommandEncoderWithDescriptor:_desc];
    NSUInteger n = 0, dropped = 0;
    for (NVMTLDeferredRenderCommandEncoder *s in subs) {
        if (!s->_ended) dropped++;
        @synchronized (s) {
            // straight into the command buffer's resource set, which is what keeps a heap's memory alive
            // while this submission still reads resources placed inside it (-useHeap: itself is
            // deprecated, and this is the same set it would have added them to)
            for (id<MTLHeap> heap in s->_heaps) nvmtl_retain_resource(real->_cb, heap);
            NSUInteger invi = 0;
            @autoreleasepool {
                for (uint32_t ci = 0; ci < s->_ncmd; ci++) {
                    const nvp_cmd *c = &s->_cmds[ci];
                    switch (c->kind) {
                    case NVP_INV: {
                        if (invi >= s->_inv.count) break;
                        NSInvocation *inv = s->_inv[invi++];
                        if (inv.selector == @selector(endEncoding)) break;
                        [inv invokeWithTarget:real]; inv.target = nil; n++;
                        break; }
                    case NVP_PIPE:   [real setRenderPipelineState:(__bridge id)c->o0]; n++; break;
                    case NVP_DS:     [real setDepthStencilState:(__bridge id)c->o0]; n++; break;
                    case NVP_VBUF:   [real setVertexBuffer:(__bridge id)c->o0 offset:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_FBUF:   [real setFragmentBuffer:(__bridge id)c->o0 offset:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_VOFF:   [real setVertexBufferOffset:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_FOFF:   [real setFragmentBufferOffset:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_VBUFST: [real setVertexBuffer:(__bridge id)c->o0 offset:(NSUInteger)c->u0 attributeStride:(NSUInteger)c->u2 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_VOFFST: [real setVertexBufferOffset:(NSUInteger)c->u0 attributeStride:(NSUInteger)c->u2 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_USEHEAP:  nvmtl_use_heap(real->_cb, (__bridge id)c->o0); n++; break;   // what -[real useHeap:] does
                    case NVP_USEHEAP2: [real useHeap:(__bridge id)c->o0 stages:(MTLRenderStages)c->u0]; n++; break;
                    case NVP_VTEX:   [real setVertexTexture:(__bridge id)c->o0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_FTEX:   [real setFragmentTexture:(__bridge id)c->o0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_VSAMP:  [real setVertexSamplerState:(__bridge id)c->o0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_FSAMP:  [real setFragmentSamplerState:(__bridge id)c->o0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_CULL:   [real setCullMode:(MTLCullMode)c->u0]; n++; break;
                    case NVP_WIND:   [real setFrontFacingWinding:(MTLWinding)c->u0]; n++; break;
                    case NVP_FILL:   [real setTriangleFillMode:(MTLTriangleFillMode)c->u0]; n++; break;
                    case NVP_DEPTHCLIP: [real setDepthClipMode:(MTLDepthClipMode)c->u0]; n++; break;
                    case NVP_SREF:   [real setStencilReferenceValue:(uint32_t)c->u0]; n++; break;
                    case NVP_SREF2:  [real setStencilFrontReferenceValue:(uint32_t)c->u0 backReferenceValue:(uint32_t)c->u1]; n++; break;
                    case NVP_VIEWPORT: {
                        MTLViewport v = { c->f0, c->f1, c->f2, c->f3, c->f4, c->f5 };
                        [real setViewport:v]; n++; break; }
                    case NVP_SCISSOR: {
                        MTLScissorRect r = { (NSUInteger)c->u0, (NSUInteger)c->u1, (NSUInteger)c->u2, (NSUInteger)c->u3 };
                        [real setScissorRect:r]; n++; break; }
                    case NVP_DEPTHBIAS: [real setDepthBias:(float)c->f0 slopeScale:(float)c->f1 clamp:(float)c->f2]; n++; break;
                    case NVP_BLENDCOL:  [real setBlendColorRed:(float)c->f0 green:(float)c->f1 blue:(float)c->f2 alpha:(float)c->f3]; n++; break;
                    case NVP_DRAWIDX:
                        [real drawIndexedPrimitives:(MTLPrimitiveType)c->i0 indexCount:(NSUInteger)c->u0
                                          indexType:(MTLIndexType)c->i1 indexBuffer:(__bridge id)c->o0
                                  indexBufferOffset:(NSUInteger)c->u1 instanceCount:(NSUInteger)c->u2
                                         baseVertex:(NSInteger)c->u3 baseInstance:(NSUInteger)c->u4]; n++; break;
                    case NVP_DRAWIDX5:
                        [real drawIndexedPrimitives:(MTLPrimitiveType)c->i0 indexCount:(NSUInteger)c->u0
                                          indexType:(MTLIndexType)c->i1 indexBuffer:(__bridge id)c->o0
                                  indexBufferOffset:(NSUInteger)c->u1]; n++; break;
                    case NVP_DRAW:
                        [real drawPrimitives:(MTLPrimitiveType)c->i0 vertexStart:(NSUInteger)c->u0
                                 vertexCount:(NSUInteger)c->u1 instanceCount:(NSUInteger)c->u2
                                baseInstance:(NSUInteger)c->u3]; n++; break;
                    case NVP_DRAW3:
                        [real drawPrimitives:(MTLPrimitiveType)c->i0 vertexStart:(NSUInteger)c->u0
                                 vertexCount:(NSUInteger)c->u1]; n++; break;
                    case NVP_USERES:  [real useResource:(__bridge id)c->o0 usage:(MTLResourceUsage)c->u0]; n++; break;
                    case NVP_USERES3: [real useResource:(__bridge id)c->o0 usage:(MTLResourceUsage)c->u0 stages:(MTLRenderStages)c->u1]; n++; break;
                    case NVP_VBYTES:  [real setVertexBytes:c->p0 length:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    case NVP_FBYTES:  [real setFragmentBytes:c->p0 length:(NSUInteger)c->u0 atIndex:(NSUInteger)c->u1]; n++; break;
                    }
                    if ((ci & 0xff) == 0xff) {  }
                }
            }
            free(s->_cmds); s->_cmds = NULL; s->_ncmd = s->_capcmd = 0;
            [s->_inv removeAllObjects]; [s->_keep removeAllObjects]; s->_owner = nil;
        }
    }
    [real endEncoding];
    nvlog("parallel encoder %p: replayed %lu call(s) from %lu sub-encoder(s)%s", (__bridge void *)self, (unsigned long)n, (unsigned long)subs.count,
          dropped ? " (some sub-encoders were never ended)" : "");
}
- (id<MTLDevice>)device { return gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)_cb; }   // SPI, see the render encoder
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i { _desc.colorAttachments[i].storeAction = a; }
- (void)setDepthStoreAction:(MTLStoreAction)a { _desc.depthAttachment.storeAction = a; }
- (void)setStencilStoreAction:(MTLStoreAction)a { _desc.stencilAttachment.storeAction = a; }
- (void)setColorStoreActionOptions:(MTLStoreActionOptions)o atIndex:(NSUInteger)i { _desc.colorAttachments[i].storeActionOptions = o; }
- (void)setDepthStoreActionOptions:(MTLStoreActionOptions)o { _desc.depthAttachment.storeActionOptions = o; }
- (void)setStencilStoreActionOptions:(MTLStoreActionOptions)o {
    if (o) [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
        code:MTLCommandBufferErrorInternal userInfo:@{NSLocalizedDescriptionKey:@"stencil store options are illegal"}]];
    _desc.stencilAttachment.storeActionOptions = o;
}
- (void)insertDebugSignpost:(NSString *)s {}
- (void)pushDebugGroup:(NSString *)g { nvtrace("GROUP %s", g.UTF8String ?: "?"); }
- (void)popDebugGroup {}
@end

/* *refused: why the descriptor is not carried (NULL when it is). Curve and motion geometry are refused, not dropped: this
   device reports supportsRayTracingCurves and supportsPrimitiveMotionBlur NO, the case Metal's validation layer reports as
   "Ray tracing curve primitives is not supported on this device" - a build with them fails its command buffer. */
/* The Vulkan vertex format of a Metal AS vertex format (ANV: vk_acceleration_struct_vtx_format_supported), 0 when not
   carried. Metal's 3-component normalized formats read as the 4-component Vulkan format: the build uses x, y, z (the
   fourth component is ignored), the stride is Metal's. Integer (non-normalized), signed 10:10:10:2, BGRA8 and the
   packed float formats have no Vulkan AS vertex format: those builds are refused visibly. */
static uint32_t nvmtl_as_vertex_format(MTLAttributeFormat f)
{
    switch (f) {
    case MTLAttributeFormatFloat2: return 103;  /* R32G32_SFLOAT */
    case MTLAttributeFormatFloat3: return 106;  /* R32G32B32_SFLOAT */
    case MTLAttributeFormatFloat4: return 109;  /* R32G32B32A32_SFLOAT */
    case MTLAttributeFormatHalf2:  return 83;   /* R16G16_SFLOAT */
    case MTLAttributeFormatHalf3:  return 90;   /* R16G16B16_SFLOAT */
    case MTLAttributeFormatHalf4:  return 97;   /* R16G16B16A16_SFLOAT */
    case MTLAttributeFormatShort2Normalized:  return 78;   /* R16G16_SNORM */
    case MTLAttributeFormatShort3Normalized:
    case MTLAttributeFormatShort4Normalized:  return 92;   /* R16G16B16A16_SNORM */
    case MTLAttributeFormatUShort2Normalized: return 77;   /* R16G16_UNORM */
    case MTLAttributeFormatUShort3Normalized:
    case MTLAttributeFormatUShort4Normalized: return 91;   /* R16G16B16A16_UNORM */
    case MTLAttributeFormatChar2Normalized:   return 17;   /* R8G8_SNORM */
    case MTLAttributeFormatChar3Normalized:
    case MTLAttributeFormatChar4Normalized:   return 38;   /* R8G8B8A8_SNORM */
    case MTLAttributeFormatUChar2Normalized:  return 16;   /* R8G8_UNORM */
    case MTLAttributeFormatUChar3Normalized:
    case MTLAttributeFormatUChar4Normalized:  return 37;   /* R8G8B8A8_UNORM */
    case MTLAttributeFormatUInt1010102Normalized: return 64;   /* A2B10G10R10_UNORM_PACK32: x in bits 0-9, as Metal */
    default: return 0;
    }
}

/* *refused: why the descriptor is not carried (NULL when it is). Curve and motion geometry are refused, not dropped: this
   device reports supportsRayTracingCurves and supportsPrimitiveMotionBlur NO, the case Metal's validation layer reports as
   "Ray tracing curve primitives is not supported on this device" - a build with them fails its command buffer. So are
   per-primitive data (supportsRayTracingPerPrimitiveData NO), vertex formats without a Vulkan AS format, and counts past
   the device's acceleration structure limits. */
/* rtopt4: a Metal curve geometry descriptor as its record (nvmtl_rt_layout.h): NULL, or why it is refused (formats the
   curve pass and the shaders do not read, a basis / control-point count Metal does not define, buffers it overruns) */
static const char *nvmtl_curve_record(MTLAccelerationStructureCurveGeometryDescriptor *c, nvmtl_geom_rec *r, NSMutableSet *track)
{
    NVMTLBuffer *cb = (NVMTLBuffer *)c.controlPointBuffer, *rb = (NVMTLBuffer *)c.radiusBuffer, *ib = (NVMTLBuffer *)c.indexBuffer;
    if (![(id)cb isKindOfClass:[NVMTLBuffer class]] || ![(id)rb isKindOfClass:[NVMTLBuffer class]] || ![(id)ib isKindOfClass:[NVMTLBuffer class]])
        return "curve geometry without its control point, radius or index buffer";
    const MTLAttributeFormat pf = c.controlPointFormat, rf = c.radiusFormat;
    uint32_t psz = 0, palign = 4;
    switch (pf) {
    case MTLAttributeFormatFloat2: psz = 8; break; case MTLAttributeFormatFloat3: psz = 12; break; case MTLAttributeFormatFloat4: psz = 16; break;
    case MTLAttributeFormatHalf2: psz = 4; palign = 2; break; case MTLAttributeFormatHalf3: psz = 6; palign = 2; break; case MTLAttributeFormatHalf4: psz = 8; palign = 2; break;
    default: return "curve control point format not carried (Float2/3/4 and Half2/3/4 are)";
    }
    const uint32_t rsz = rf == MTLAttributeFormatFloat ? 4 : rf == MTLAttributeFormatHalf ? 2 : 0;
    if (!rsz) return "curve radius format not carried (Float and Half are)";
    const NSUInteger seg = c.segmentCount, scp = c.segmentControlPointCount, ncp = c.controlPointCount;
    const NSUInteger pst = c.controlPointStride ? c.controlPointStride : psz, rst = c.radiusStride ? c.radiusStride : rsz;
    const MTLCurveBasis basis = c.curveBasis;
    const BOOL ok_cp = basis == MTLCurveBasisLinear ? scp == 2 : basis == MTLCurveBasisCatmullRom ? scp == 4 : (scp == 3 || scp == 4);
    if (!ok_cp) return "curve segmentControlPointCount does not fit its basis (linear 2, Bezier and B-spline 3 or 4, Catmull-Rom 4)";
    if ((NSUInteger)c.curveType > 1 || (NSUInteger)c.curveEndCaps > 2 || (NSUInteger)basis > 3) return "curve type, basis or end caps not carried";
    if (seg > UINT32_MAX || ncp > UINT32_MAX || pst > UINT32_MAX || rst > UINT32_MAX) return "curve counts or strides past 32 bits";
    const NSUInteger poff = c.controlPointBufferOffset, roff = c.radiusBufferOffset, ioff = c.indexBufferOffset;
    if ((pst % palign) || (poff % palign) || (rst % rsz) || (roff % rsz)) return "curve control points or radii not aligned to their format";
    const uint32_t isz = c.indexType == MTLIndexTypeUInt16 ? 2 : 4;
    if (ioff % isz) return "curve index buffer offset not aligned to its index type";
    if (ncp < scp) return "curve with fewer control points than a segment has";
    if (poff > cb->_b.size || (uint64_t)(ncp - 1) * pst + psz > cb->_b.size - poff) return "curve control points overrun their buffer";
    if (roff > rb->_b.size || (uint64_t)(ncp - 1) * rst + rsz > rb->_b.size - roff) return "curve radii overrun their buffer";
    if (ioff > ib->_b.size || (uint64_t)seg * isz > ib->_b.size - ioff) return "curve indices overrun their buffer";
    if (track) { [track addObject:cb]; [track addObject:rb]; [track addObject:ib]; }
    r->kind = 2;
    r->cp = nvmtl_vk_buffer_address(&cb->_b) + poff; r->radius = nvmtl_vk_buffer_address(&rb->_b) + roff; r->index = nvmtl_vk_buffer_address(&ib->_b) + ioff;
    r->cp_stride = (uint32_t)pst; r->cp_format = (uint32_t)pf; r->r_stride = (uint32_t)rst; r->r_format = (uint32_t)rf;
    r->index_type = c.indexType == MTLIndexTypeUInt16 ? 0 : 1; r->seg_cp = (uint32_t)scp; r->basis = (uint32_t)basis;
    r->ctype = (uint32_t)c.curveType; r->caps = (uint32_t)c.curveEndCaps; r->seg_count = (uint32_t)seg; r->cp_count = (uint32_t)ncp;
    if (!r->cp || !r->radius || !r->index) return "curve buffers have no device address";
    return NULL;
}

uint32_t nvmtl_geoms_from_descriptor(id desc, nvmtl_geom **outp, int *instance, uint32_t *instance_count, NSMutableSet *track, const char **refused)
{
    *instance = 0; *instance_count = 0; *outp = NULL; if (refused) *refused = NULL;
    const char *dummy; if (!refused) refused = &dummy;
    if ([desc isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]] || [desc isKindOfClass:[MTLIndirectInstanceAccelerationStructureDescriptor class]]) {
        // indirect: the build range is the maximum; instances past the count the GPU reads from instanceCountBuffer are inactive
        *instance = 1;
        *instance_count = [desc isKindOfClass:[MTLInstanceAccelerationStructureDescriptor class]]
            ? (uint32_t)MIN(((MTLInstanceAccelerationStructureDescriptor *)desc).instanceCount, (NSUInteger)UINT32_MAX)
            : (uint32_t)MIN(((MTLIndirectInstanceAccelerationStructureDescriptor *)desc).maxInstanceCount, (NSUInteger)UINT32_MAX);
        if (*instance_count > nvmtl_vk_rt_limit(1)) {
            nvlog("acceleration structure: %u instances, more than the device's %llu: REFUSED", *instance_count, (unsigned long long)nvmtl_vk_rt_limit(1));
            *refused = "more instances than this device's acceleration structure limit";
        }
        return 0;
    }
    if (![desc isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]]) {
        nvlog("acceleration structure: %s is not carried (primitive, instance and indirect instance descriptors are)", NSStringFromClass([desc class]).UTF8String);
        *refused = "acceleration structure descriptor class not carried";
        return 0;
    }
    NSArray *gs = ((MTLPrimitiveAccelerationStructureDescriptor *)desc).geometryDescriptors; uint32_t n = 0; uint64_t prims = 0;
    if (gs.count > nvmtl_vk_rt_limit(0)) {
        nvlog("acceleration structure: %lu geometries, more than the device's %llu: REFUSED", (unsigned long)gs.count, (unsigned long long)nvmtl_vk_rt_limit(0));
        *refused = "more geometries than this device's acceleration structure limit"; return 0;
    }
    nvmtl_geom *out = calloc(gs.count ? gs.count : 1, sizeof *out);
    if (!out) { *refused = "out of memory for the geometry list"; return 0; }
    for (MTLAccelerationStructureGeometryDescriptor *g in gs) {
        nvmtl_geom *o = &out[n]; memset(o, 0, sizeof *o); o->opaque = g.opaque ? 1 : 0;
        // rtopt4: the geometry's record in the structure's table (shaders read it through the structure's header)
        o->rec.ift = (uint32_t)MIN(g.intersectionFunctionTableOffset, (NSUInteger)UINT32_MAX);
        NVMTLBuffer *pdb = [g respondsToSelector:@selector(primitiveDataBuffer)] ? (NVMTLBuffer *)g.primitiveDataBuffer : nil;
        if (pdb) {
            const NSUInteger esz = g.primitiveDataElementSize, st = g.primitiveDataStride ? g.primitiveDataStride : esz, off = g.primitiveDataBufferOffset;
            if (![(id)pdb isKindOfClass:[NVMTLBuffer class]] || off > pdb->_b.size || st > UINT32_MAX || esz > UINT32_MAX) {
                nvlog("acceleration structure: geometry %u primitive data (offset %lu stride %lu element %lu): REFUSED", n, (unsigned long)off, (unsigned long)st, (unsigned long)esz);
                *refused = "primitive data buffer offset outside its buffer"; free(out); return 0;
            }
            if (track) [track addObject:pdb];
            o->rec.pd = nvmtl_vk_buffer_address(&pdb->_b) + off; o->rec.pd_stride = (uint32_t)st; o->rec.pd_size = (uint32_t)esz;
        }
        if ([g isKindOfClass:[MTLAccelerationStructureTriangleGeometryDescriptor class]]) {
            MTLAccelerationStructureTriangleGeometryDescriptor *t = (MTLAccelerationStructureTriangleGeometryDescriptor *)g;
            NVMTLBuffer *vb = (NVMTLBuffer *)t.vertexBuffer;
            if (!vb) { nvlog("acceleration structure: triangle geometry %u without a vertex buffer: REFUSED", n); *refused = "triangle geometry without a vertex buffer"; free(out); return 0; }
            if (track) [track addObject:vb];
            MTLAttributeFormat f = [t respondsToSelector:@selector(vertexFormat)] ? t.vertexFormat : MTLAttributeFormatFloat3;
            o->vertex_format = nvmtl_as_vertex_format(f);
            if (!o->vertex_format) {
                nvlog("acceleration structure: vertex format %lu has no Vulkan acceleration structure format: REFUSED", (unsigned long)f);
                *refused = "Acceleration structure vertex format is not supported on this device"; free(out); return 0;
            }
            o->vertices = nvmtl_vk_buffer_address(&vb->_b) + t.vertexBufferOffset; o->stride = (uint32_t)t.vertexStride;
            if (!o->stride) {   // tightly packed: the format's size
                static const uint8_t sz[] = { [MTLAttributeFormatFloat2] = 8, [MTLAttributeFormatFloat3] = 12, [MTLAttributeFormatFloat4] = 16,
                    [MTLAttributeFormatHalf2] = 4, [MTLAttributeFormatHalf3] = 6, [MTLAttributeFormatHalf4] = 8,
                    [MTLAttributeFormatShort2Normalized] = 4, [MTLAttributeFormatShort3Normalized] = 6, [MTLAttributeFormatShort4Normalized] = 8,
                    [MTLAttributeFormatUShort2Normalized] = 4, [MTLAttributeFormatUShort3Normalized] = 6, [MTLAttributeFormatUShort4Normalized] = 8,
                    [MTLAttributeFormatChar2Normalized] = 2, [MTLAttributeFormatChar3Normalized] = 3, [MTLAttributeFormatChar4Normalized] = 4,
                    [MTLAttributeFormatUChar2Normalized] = 2, [MTLAttributeFormatUChar3Normalized] = 3, [MTLAttributeFormatUChar4Normalized] = 4,
                    [MTLAttributeFormatUInt1010102Normalized] = 4 };
                o->stride = (NSUInteger)f < sizeof sz ? sz[f] : 12;
            }
            o->count = (uint32_t)t.triangleCount;
            NVMTLBuffer *ib = (NVMTLBuffer *)t.indexBuffer;
            if (ib) { if (track) [track addObject:ib]; o->indices = nvmtl_vk_buffer_address(&ib->_b) + t.indexBufferOffset;
                      o->index_type = t.indexType == MTLIndexTypeUInt16 ? 0 : 1; o->max_vertex = 0xFFFFFF; }
            else { o->index_type = 1000165000u ; o->max_vertex = o->count ? o->count * 3 - 1 : 0; }
            NVMTLBuffer *mb = [t respondsToSelector:@selector(transformationMatrixBuffer)] ? (NVMTLBuffer *)t.transformationMatrixBuffer : nil;
            if (mb) {
                const NSUInteger moff = t.transformationMatrixBufferOffset;
                if ((moff & 3) || moff > mb->_b.size || mb->_b.size - moff < 48) {
                    nvlog("acceleration structure: transformation matrix at %lu of a %zu-byte buffer: REFUSED", (unsigned long)moff, mb->_b.size);
                    *refused = "transformation matrix outside its buffer or not 4-aligned"; free(out); return 0;
                }
                if (track) [track addObject:mb];
                o->transform_src = &mb->_b; o->transform_off = moff;
                o->transform_rowmajor = [t respondsToSelector:@selector(transformationMatrixLayout)] && t.transformationMatrixLayout == MTLMatrixLayoutRowMajor;
                o->transform = 1;   // present (the build gives it its staged address); sizes and the update shape see it
            }
        } else if ([g isKindOfClass:[MTLAccelerationStructureBoundingBoxGeometryDescriptor class]]) {
            MTLAccelerationStructureBoundingBoxGeometryDescriptor *b = (MTLAccelerationStructureBoundingBoxGeometryDescriptor *)g;
            NVMTLBuffer *bb = (NVMTLBuffer *)b.boundingBoxBuffer;
            if (!bb) { nvlog("acceleration structure: box geometry %u without a buffer: REFUSED", n); *refused = "bounding box geometry without a buffer"; free(out); return 0; }
            if (track) [track addObject:bb];
            o->is_boxes = 1; o->boxes = nvmtl_vk_buffer_address(&bb->_b) + b.boundingBoxBufferOffset;
            o->box_stride = (uint32_t)(b.boundingBoxStride ? b.boundingBoxStride : 24); o->count = (uint32_t)b.boundingBoxCount;
            o->rec.kind = 1;
        } else if ([g isKindOfClass:[MTLAccelerationStructureCurveGeometryDescriptor class]]) {
            // rtopt4: a curve is bounding-box geometry, one box per segment (nvmtl_rt_curves.comp); shaders intersect
            // the segment (the translator's runtime) from the record
            const char *why = nvmtl_curve_record((MTLAccelerationStructureCurveGeometryDescriptor *)g, &o->rec, track);
            if (why) { nvlog("acceleration structure: curve geometry %u: %s - REFUSED", n, why); *refused = why; free(out); return 0; }
            o->is_boxes = 1; o->box_stride = 24; o->count = o->rec.seg_count; o->boxes = 0;   // the build's curve pass writes the boxes
        } else {
            const BOOL curve = [g isKindOfClass:[MTLAccelerationStructureMotionCurveGeometryDescriptor class]];   // motion curves
            const BOOL motion = [g isKindOfClass:[MTLAccelerationStructureMotionTriangleGeometryDescriptor class]] ||
                                [g isKindOfClass:[MTLAccelerationStructureMotionBoundingBoxGeometryDescriptor class]] ||
                                [g isKindOfClass:[MTLAccelerationStructureMotionCurveGeometryDescriptor class]];
            const char *why = curve ? "Acceleration descriptor is using primitive motion blur which is not supported by the device (motion curves)"
                            : motion ? "Acceleration descriptor is using primitive motion blur which is not supported by the device"
                            : "acceleration structure geometry class not carried";
            nvlog("acceleration structure: geometry %s — %s: REFUSED", NSStringFromClass([g class]).UTF8String, why);
            *refused = why; free(out);
            return 0;
        }
        prims += o->count;
        n++;
    }
    if (prims > nvmtl_vk_rt_limit(2)) {
        nvlog("acceleration structure: %llu primitives, more than the device's %llu: REFUSED", (unsigned long long)prims, (unsigned long long)nvmtl_vk_rt_limit(2));
        *refused = "more primitives than this device's acceleration structure limit"; free(out); return 0;
    }
    *outp = out;
    return n;
}

@implementation NVMTLAccelerationStructure
- (NSUInteger)size { return _b.size > NVMTL_AS_HEADER ? _b.size - NVMTL_AS_HEADER : 0; }
- (MTLPurgeableState)setPurgeableState:(MTLPurgeableState)s { return MTLPurgeableStateNonVolatile; }
/* the Metal structure's address: its header (shaders reach the Vulkan structure NVMTL_AS_HEADER after it) */
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = nvmtl_vk_buffer_address(&_b); return r; }
- (BOOL)nvmtlEnsureCreated:(int)instance {
    if (_as.as) {
        if (_as.instance != instance) { nvlog("acceleration structure: built as %s before, now as %s — REFUSED", _as.instance ? "instances" : "primitives", instance ? "instances" : "primitives"); return NO; }
        return YES;
    }
    // rtopt5: a primitive structure is traced through its one-instance top-level wrapper at + NVMTL_AS_HEADER (Vulkan
    // ray queries start at a top-level structure); the bottom-level structure follows it (nvmtl_rt_layout.h)
    size_t wscratch = 0; const uint32_t wrap = instance ? 0 : nvmtl_vk_rt_wrap(&wscratch);
    if (wrap) {
        if (_b.size <= (size_t)NVMTL_AS_HEADER + wrap + 256) { nvlog("acceleration structure: %zu bytes cannot hold the header, the top-level wrapper and a structure - create FAILED", _b.size); return NO; }
        NVMTLBuffer *aux = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(256 + ((wscratch + 255) & ~(size_t)255) + 256, 0, &aux->_b)) { nvlog("acceleration structure: wrapper scratch FAILED"); return NO; }
        if (nvmtl_vk_accel_create(&_b, NVMTL_AS_HEADER, wrap, 1, &_wrap)) { nvlog("acceleration structure: top-level wrapper create FAILED"); return NO; }
        if (nvmtl_vk_accel_create(&_b, NVMTL_AS_HEADER + wrap, _b.size - NVMTL_AS_HEADER - wrap, 0, &_as)) {
            nvmtl_vk_accel_destroy(&_wrap); nvlog("acceleration structure: create FAILED"); return NO; }
        _wrapAux = aux;
        nvlog("acceleration structure %p: primitives, %zu bytes, top-level wrapper at %llx, structure at %llx", (__bridge void *)self, _b.size,
              (unsigned long long)_wrap.addr, (unsigned long long)_as.addr);
        return YES;
    }
    if (_b.size <= NVMTL_AS_HEADER || nvmtl_vk_accel_create(&_b, NVMTL_AS_HEADER, _b.size - NVMTL_AS_HEADER, instance, &_as)) { nvlog("acceleration structure: create FAILED"); return NO; }
    nvlog("acceleration structure %p: %s, %zu bytes at %llx", (__bridge void *)self, instance ? "instances" : "primitives", _b.size, (unsigned long long)_as.addr);
    return YES;
}
- (void)dealloc {
    nvmtl_vk_accel_destroy(&_wrap);
    nvmtl_vk_accel_destroy(&_as);
    if (_uid.buf) nvmtl_vk_buffer_destroy(&_uid);
#if !__has_feature(objc_arc)
    [super dealloc];
#endif
}
@end

@implementation NVMTLAccelerationStructureCommandEncoder
- (id<MTLDevice>)device { return _cb->_queue->_dev; }
- (id<MTLCommandBuffer>)commandBuffer { return (id<MTLCommandBuffer>)(_cb ? _cb : _owner); }   // SPI, see the render encoder
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (void)endEncoding { if (_cb) { if (_tsEnd) { nvmtl_ts_pass_end(_cb, self, _tsEnd); _tsEnd = nil; } _cb->_nEnd++; _cb = nil; } }   // lets go of the command buffer, as the blit encoder does
- (void)nvmtlTrack:(id)r { if (!r) return; if (!_cb->_resources) _cb->_resources = [NVMTLResSet new]; [_cb->_resources addObject:r]; }
- (void)nvmtlHold:(NVMTLBuffer *)b { if (!b) return; if (!_cb->_scratch) _cb->_scratch = [NSMutableArray new]; [_cb->_scratch addObject:b]; }
/* A build refused after the structure exists is recorded as an empty build instead: a ray query that reaches the
   structure then misses, rather than walking memory no build wrote (a never-built structure holds whatever its buffer
   held, and the RT unit follows the node pointers it finds there). Count 0, so no geometry or instance data is read. */
/* rtopt4: the structure's header (nvmtl_rt_layout.h) at its address, in this command buffer's order */
- (BOOL)nvmtlHeader:(NVMTLAccelerationStructure *)dst kind:(uint32_t)kind count:(uint32_t)count flags:(uint32_t)flags table:(uint64_t)table {
    nvmtl_as_header h; memset(&h, 0, sizeof h);
    h.magic = NVMTL_AS_MAGIC; h.kind = kind; h.count = count; h.flags = flags; h.table = table; h.vk = dst->_as.addr;
    h.rec_bytes = kind ? (uint32_t)sizeof(nvmtl_inst_rec) : (uint32_t)sizeof(nvmtl_geom_rec);
    NVMTLBuffer *st = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create(sizeof h, 1, &st->_b) || !st->_b.map) { nvlog("build acceleration structure %p: header staging FAILED", (__bridge void *)dst); return NO; }
    [self nvmtlHold:st];
    if (nvmtl_vk_cmd_upload(&_cb->_c, &dst->_b, 0, &h, sizeof h, &st->_b)) { nvlog("build acceleration structure %p: header write FAILED", (__bridge void *)dst); return NO; }
    dst->_hdr = h;
    return YES;
}
/* rtopt5: (re)builds dst's one-instance top-level wrapper over its bottom-level structure, after every build, refit, empty
   build or copy into dst (the wrapper's bounds come from the bottom-level structure it references). empty: no instance
   (rays miss). Identity transform, custom index 0, mask 0xFF, SBT offset 0, flags 0: geometry opacity, ray flags and
   culling act as on the bottom-level structure alone. A structure without a wrapper (instances, NVMTL_RT_WRAP=0) is
   left alone. */
- (BOOL)nvmtlWrap:(NVMTLAccelerationStructure *)dst empty:(BOOL)empty {
    if (!dst || !dst->_wrap.as || !dst->_as.as || dst->_as.instance) return YES;
    NVMTLBuffer *aux = dst->_wrapAux;
    const uint64_t base = aux ? nvmtl_vk_buffer_address(&aux->_b) : 0;
    if (!base) { nvlog("build acceleration structure %p: wrapper buffer has no address - FAILED", (__bridge void *)dst); return NO; }
    if (!empty) {
        uint32_t inst[16]; memset(inst, 0, sizeof inst);
        const float one = 1.0f; memcpy(&inst[0], &one, 4); memcpy(&inst[5], &one, 4); memcpy(&inst[10], &one, 4);   // 3x4 row-major identity
        inst[12] = 0xFFu << 24;                                   // custom index 0, mask 0xFF
        inst[13] = 0;                                             // SBT offset 0, flags 0
        inst[14] = (uint32_t)dst->_as.addr; inst[15] = (uint32_t)(dst->_as.addr >> 32);
        NVMTLBuffer *st = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(sizeof inst, 1, &st->_b) || !st->_b.map) { nvlog("build acceleration structure %p: wrapper instance staging FAILED", (__bridge void *)dst); return NO; }
        [self nvmtlHold:st];
        if (nvmtl_vk_cmd_upload(&_cb->_c, &aux->_b, 0, inst, sizeof inst, &st->_b)) { nvlog("build acceleration structure %p: wrapper instance write FAILED", (__bridge void *)dst); return NO; }
    }
    nvmtl_geom g; memset(&g, 0, sizeof g);
    if (nvmtl_vk_cmd_build_accel(&_cb->_c, &dst->_wrap, &g, 0, base, empty ? 0u : 1u, (base + 256 + 255) & ~255ull, nvmtl_vk_accel_flags(0), NULL)) {
        nvlog("build acceleration structure %p: top-level wrapper build FAILED", (__bridge void *)dst); return NO; }
    [self nvmtlHold:aux];   // a later release of dst leaves the wrapper's inputs alive until this command buffer retires
    return YES;
}
/* rtopt4: a new table / data object for dst's next build; the ones it replaces live until this command buffer retires
   (earlier command buffers that read them have finished by then) */
- (void)nvmtlRetireTables:(NVMTLAccelerationStructure *)dst {
    [self nvmtlHold:dst->_tabObj]; [self nvmtlHold:dst->_datObj];
    dst->_tabObj = nil; dst->_datObj = nil;
}
static uint64_t nvmtl_align16(uint64_t v) { return (v + 15) & ~15ull; }
/* rtopt4, a primitive structure's build: its own copies of curve data and primitive data (records point there), the
   curve segments' boxes (nvmtl_rt_curves.comp; geoms[i].boxes), the geometry records. *flags: the header's. */
- (BOOL)nvmtlPrimitiveTables:(NVMTLAccelerationStructure *)dst desc:(MTLAccelerationStructureDescriptor *)desc geoms:(nvmtl_geom *)geoms n:(uint32_t)n
                     scratch:(uint64_t)scratch_addr flags:(uint32_t *)flags {
    NSArray *gd = [desc isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]] ? ((MTLPrimitiveAccelerationStructureDescriptor *)desc).geometryDescriptors : @[];
    typedef struct { NVMTLBuffer *src; uint64_t off, len, dst; uint64_t *field; } nvmtl_copy;
    nvmtl_copy *cps = calloc((size_t)n * 4 + 1, sizeof *cps); uint32_t nc = 0; uint64_t total = 0, nbox = 0;
    if (!cps) { [self nvmtlRefuse:dst scratch:scratch_addr why:"out of memory for the geometry copies" oom:YES]; return NO; }
    *flags = 0;
    for (uint32_t i = 0; i < n && i < gd.count; i++) {
        MTLAccelerationStructureGeometryDescriptor *g = gd[i]; nvmtl_geom_rec *r = &geoms[i].rec;
        if (r->pd) {
            const uint64_t cnt = geoms[i].count, len = cnt ? (cnt - 1) * (uint64_t)r->pd_stride + r->pd_size : 0;
            NVMTLBuffer *b = (NVMTLBuffer *)g.primitiveDataBuffer;
            if (len && (g.primitiveDataBufferOffset > b->_b.size || len > b->_b.size - g.primitiveDataBufferOffset)) {
                free(cps); [self nvmtlRefuse:dst scratch:scratch_addr why:"primitive data overruns its buffer" oom:NO]; return NO; }
            if (len) cps[nc++] = (nvmtl_copy){ b, g.primitiveDataBufferOffset, len, 0, &r->pd };
            *flags |= NVMTL_AS_FLAG_PRIMITIVE_DATA;
        }
        if (r->kind == 2) {
            MTLAccelerationStructureCurveGeometryDescriptor *c = (MTLAccelerationStructureCurveGeometryDescriptor *)g;
            const uint64_t psz = r->cp_format == MTLAttributeFormatFloat2 ? 8 : r->cp_format == MTLAttributeFormatFloat3 ? 12 : r->cp_format == MTLAttributeFormatFloat4 ? 16
                               : r->cp_format == MTLAttributeFormatHalf2 ? 4 : r->cp_format == MTLAttributeFormatHalf3 ? 6 : 8;
            const uint64_t rsz = r->r_format == MTLAttributeFormatHalf ? 2 : 4, isz = r->index_type ? 4 : 2;
            cps[nc++] = (nvmtl_copy){ (NVMTLBuffer *)c.controlPointBuffer, c.controlPointBufferOffset, (uint64_t)(r->cp_count - 1) * r->cp_stride + psz, 0, &r->cp };
            cps[nc++] = (nvmtl_copy){ (NVMTLBuffer *)c.radiusBuffer, c.radiusBufferOffset, (uint64_t)(r->cp_count - 1) * r->r_stride + rsz, 0, &r->radius };
            if (r->seg_count) cps[nc++] = (nvmtl_copy){ (NVMTLBuffer *)c.indexBuffer, c.indexBufferOffset, (uint64_t)r->seg_count * isz, 0, &r->index };
            nbox += geoms[i].count; *flags |= NVMTL_AS_FLAG_CURVES;
        }
    }
    for (uint32_t k = 0; k < nc; k++) { total = nvmtl_align16(total); cps[k].dst = total; total += cps[k].len; }
    [self nvmtlRetireTables:dst];
    if (total) {
        NVMTLBuffer *dat = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create(total, 0, &dat->_b)) { free(cps); [self nvmtlRefuse:dst scratch:scratch_addr why:"curve / primitive data copy allocation FAILED" oom:YES]; return NO; }
        const uint64_t base = nvmtl_vk_buffer_address(&dat->_b);
        for (uint32_t k = 0; k < nc; k++) {
            if (nvmtl_vk_cmd_copy_buffer(&_cb->_c, &cps[k].src->_b, cps[k].off, &dat->_b, cps[k].dst, cps[k].len)) {
                free(cps); [self nvmtlRefuse:dst scratch:scratch_addr why:"curve / primitive data copy could not be recorded" oom:NO]; return NO; }
            *cps[k].field = base + cps[k].dst;
        }
        dst->_datObj = dat;
    }
    free(cps);
    if (nbox) {
        NVMTLBuffer *bx = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create((size_t)nbox * 24, 0, &bx->_b)) { [self nvmtlRefuse:dst scratch:scratch_addr why:"curve bounding boxes allocation FAILED" oom:YES]; return NO; }
        [self nvmtlHold:bx];
        const uint64_t base = nvmtl_vk_buffer_address(&bx->_b); uint64_t at = 0;
        for (uint32_t i = 0; i < n; i++) {
            const nvmtl_geom_rec *r = &geoms[i].rec;
            if (r->kind != 2) continue;
            nvmtl_curve_args a = { r->cp, r->radius, r->index, base + at * 24, r->cp_stride, r->cp_format, r->r_stride, r->r_format,
                                   r->index_type, r->seg_cp, r->basis, r->ctype, r->seg_count, r->cp_count, 0, 0 };
            if (nvmtl_vk_cmd_curve_aabbs(&_cb->_c, &a)) { [self nvmtlRefuse:dst scratch:scratch_addr why:"curve bounding box pass FAILED" oom:NO]; return NO; }
            geoms[i].boxes = base + at * 24; at += geoms[i].count;
        }
    }
    const size_t tb = (size_t)(n ? n : 1) * sizeof(nvmtl_geom_rec);
    NVMTLBuffer *tab = [NVMTLBuffer new], *st = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create(tb, 0, &tab->_b) || nvmtl_vk_buffer_create(tb, 1, &st->_b) || !st->_b.map) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"geometry table allocation FAILED" oom:YES]; return NO; }
    [self nvmtlHold:st];
    nvmtl_geom_rec *recs = calloc(n ? n : 1, sizeof *recs);
    if (!recs) { [self nvmtlRefuse:dst scratch:scratch_addr why:"out of memory for the geometry table" oom:YES]; return NO; }
    for (uint32_t i = 0; i < n; i++) recs[i] = geoms[i].rec;
    const int rc = nvmtl_vk_cmd_upload(&_cb->_c, &tab->_b, 0, recs, tb, &st->_b);
    free(recs);
    if (rc) { [self nvmtlRefuse:dst scratch:scratch_addr why:"geometry table write could not be recorded" oom:NO]; return NO; }
    dst->_tabObj = tab;
    return YES;
}
- (void)nvmtlBuildEmpty:(NVMTLAccelerationStructure *)dst scratch:(uint64_t)scratch_addr why:(const char *)why {
    nvmtl_geom g; memset(&g, 0, sizeof g);
    g.vertex_format = 106u /* VK_FORMAT_R32G32B32_SFLOAT */; g.stride = 12; g.index_type = 1000165000u /* VK_INDEX_TYPE_NONE_KHR */;
    if (nvmtl_vk_cmd_build_accel(&_cb->_c, &dst->_as, &g, dst->_as.instance ? 0u : 1u, 0, 0, scratch_addr, nvmtl_vk_accel_flags(0), NULL))
        nvlog("build acceleration structure %p: %s - the empty fallback build FAILED too", (__bridge void *)dst, why);
    else nvlog("build acceleration structure %p: %s - built EMPTY instead (rays miss it)", (__bridge void *)dst, why);
    [self nvmtlWrap:dst empty:YES];   // rtopt5: what shaders trace (a primitive structure's wrapper) is empty too
    [self nvmtlRetireTables:dst];
    [self nvmtlHeader:dst kind:(uint32_t)dst->_as.instance count:0 flags:0 table:0];   // rtopt4: no records
}
/* A build Metal's contract has this device refuse (curves, motion, a descriptor it does not carry) or that cannot be
   recorded: the command buffer fails with an error naming it (status Error, as with any encoding error), and a structure
   that exists is built empty in case the work runs anyway. */
- (void)nvmtlRefuse:(NVMTLAccelerationStructure *)dst scratch:(uint64_t)scratch_addr why:(const char *)why oom:(BOOL)oom {
    nvlog("build acceleration structure %p: %s - REFUSED, the command buffer fails", (__bridge void *)dst, why);
    [_cb nvmtlRecordEncodingError:[NSError errorWithDomain:MTLCommandBufferErrorDomain
        code:oom ? MTLCommandBufferErrorOutOfMemory : MTLCommandBufferErrorInternal
        userInfo:@{NSLocalizedDescriptionKey:[NSString stringWithFormat:@"Acceleration structure build: %s", why]}]];
    if (dst && dst->_as.as) [self nvmtlBuildEmpty:dst scratch:scratch_addr why:why];
}
/* Instance descriptors are converted on the GPU (nvmtl_rt_instances.comp), right before the build in this command
   buffer: the build sees the descriptors as they are when it runs, whatever their storage mode (private, managed,
   shared) and whoever wrote them (CPU, blit, compute), and an indirect descriptor's count is read there too.
   Returns the Vulkan instance array's address, or 0 after refusing. */
- (uint64_t)nvmtlInstances:(MTLAccelerationStructureDescriptor *)desc into:(NVMTLAccelerationStructure *)dst count:(uint32_t)icount scratch:(uint64_t)scratch_addr {
    NVMTLBuffer *ib, *cntb = nil; NSUInteger ioff, istride, cntoff = 0; MTLAccelerationStructureInstanceDescriptorType itype;
    NSArray *blases = nil; uint32_t flags = 0;
    if ([desc isKindOfClass:[MTLIndirectInstanceAccelerationStructureDescriptor class]]) {
        MTLIndirectInstanceAccelerationStructureDescriptor *x = (MTLIndirectInstanceAccelerationStructureDescriptor *)desc;
        ib = (NVMTLBuffer *)x.instanceDescriptorBuffer; ioff = x.instanceDescriptorBufferOffset; istride = x.instanceDescriptorStride;
        itype = x.instanceDescriptorType; cntb = (NVMTLBuffer *)x.instanceCountBuffer; cntoff = x.instanceCountBufferOffset;
        if (x.instanceTransformationMatrixLayout == MTLMatrixLayoutRowMajor) flags |= 1u;
        if (itype != MTLAccelerationStructureInstanceDescriptorTypeIndirect) {
            [self nvmtlRefuse:dst scratch:scratch_addr why:"indirect instance descriptor type is not Indirect (motion instances are not carried)" oom:NO]; return 0; }
    } else {
        MTLInstanceAccelerationStructureDescriptor *x = (MTLInstanceAccelerationStructureDescriptor *)desc;
        ib = (NVMTLBuffer *)x.instanceDescriptorBuffer; ioff = x.instanceDescriptorBufferOffset; istride = x.instanceDescriptorStride;
        itype = x.instanceDescriptorType; blases = x.instancedAccelerationStructures;
        if (x.instanceTransformationMatrixLayout == MTLMatrixLayoutRowMajor) flags |= 1u;
        if (itype != MTLAccelerationStructureInstanceDescriptorTypeDefault && itype != MTLAccelerationStructureInstanceDescriptorTypeUserID &&
            itype != MTLAccelerationStructureInstanceDescriptorTypeIndirect) {
            [self nvmtlRefuse:dst scratch:scratch_addr why:"instance descriptor type not carried (Default, UserID and Indirect are; motion is not)" oom:NO]; return 0; }
    }
    if (!ib) { [self nvmtlRefuse:dst scratch:scratch_addr why:"nil instance descriptor buffer" oom:NO]; return 0; }
    const uint32_t natural = itype == MTLAccelerationStructureInstanceDescriptorTypeDefault ? 64 : itype == MTLAccelerationStructureInstanceDescriptorTypeUserID ? 68 : 72;
    const uint64_t stride = istride ? istride : natural;
    if (stride < natural || (stride & 3) || (ioff & 3) || stride > 0xFFFFFFFFull ||
        ioff + (uint64_t)(icount ? icount - 1 : 0) * stride + natural > ib->_b.size) {
        nvlog("build acceleration structure: %u instances of stride %llu at %lu in a %zu-byte buffer", icount, (unsigned long long)stride, (unsigned long)ioff, ib->_b.size);
        [self nvmtlRefuse:dst scratch:scratch_addr why:"instance descriptors overrun their buffer or are not 4-aligned" oom:NO]; return 0; }
    if (cntb && ((cntoff & 3) || cntoff > cntb->_b.size || cntb->_b.size - cntoff < 4)) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"instance count is outside its buffer or not 4-aligned" oom:NO]; return 0; }
    // the instanced structures: address and intersection-function base per index, written now (each structure's
    // address is fixed once it exists; one that was never built is left 0 and its instances are inactive)
    NVMTLBuffer *tab = nil; const uint32_t nas = (uint32_t)MIN(blases.count, (NSUInteger)0xFFFFFF);
    if (nas) {
        tab = [NVMTLBuffer new];
        if (nvmtl_vk_buffer_create((size_t)nas * 32, 1, &tab->_b) || !tab->_b.map) {
            [self nvmtlRefuse:dst scratch:scratch_addr why:"instanced structure table allocation FAILED" oom:YES]; return 0; }
        uint32_t *w = (uint32_t *)tab->_b.map;
        for (uint32_t k = 0; k < nas; k++) {
            NVMTLAccelerationStructure *b = blases[k];
            if ([(id)b isKindOfClass:[NVMTLAccelerationStructure class]] && b->_as.as && b->_as.instance) {
                // an instance structure of instance structures: Vulkan instances reference bottom-level structures only
                // (supportsRayTracingMultiLevelInstancing NO)
                [self nvmtlRefuse:dst scratch:scratch_addr why:"Multi-level instancing (an instanced acceleration structure that is itself an instance structure) is not supported on this device" oom:NO];
                return 0;
            }
            const BOOL is = [(id)b isKindOfClass:[NVMTLAccelerationStructure class]];
            const uint64_t addr = is ? b->_as.addr : 0, maddr = is && addr ? nvmtl_vk_buffer_address(&b->_b) : 0;   // Vulkan / Metal (header) addresses
            w[8 * k] = (uint32_t)addr; w[8 * k + 1] = (uint32_t)(addr >> 32);
            w[8 * k + 2] = is ? b->_iftAdd : 0; w[8 * k + 3] = 0;
            w[8 * k + 4] = (uint32_t)maddr; w[8 * k + 5] = (uint32_t)(maddr >> 32); w[8 * k + 6] = 0; w[8 * k + 7] = 0;
            [self nvmtlTrack:b];
        }
        [self nvmtlHold:tab];
    }
    NVMTLBuffer *vk = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create((size_t)(icount ? icount : 1) * 64, 0, &vk->_b)) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"instance conversion buffer allocation FAILED" oom:YES]; return 0; }
    [self nvmtlHold:vk];
    const size_t uidBytes = (size_t)(icount ? icount : 1) * 4;
    if (dst->_uid.size < uidBytes) {
        if (dst->_uid.buf) { NVMTLBuffer *old = [NVMTLBuffer new]; old->_b = dst->_uid; [self nvmtlHold:old]; }
        memset(&dst->_uid, 0, sizeof dst->_uid);
        if (nvmtl_vk_buffer_create(uidBytes, 0, &dst->_uid)) { nvlog("build acceleration structure: userID side table (%zu B) FAILED - user_instance_id reads 0", uidBytes); memset(&dst->_uid, 0, sizeof dst->_uid); }
    }
    // rtopt4: the instance records (Metal address of the instanced structure, 32-bit user ID, table offset), new per build
    [self nvmtlRetireTables:dst];
    NVMTLBuffer *recs = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create((size_t)(icount ? icount : 1) * sizeof(nvmtl_inst_rec), 0, &recs->_b)) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"instance record table allocation FAILED" oom:YES]; return 0; }
    dst->_tabObj = recs;
    nvmtl_inst_args a = { nvmtl_vk_buffer_address(&ib->_b) + ioff, nvmtl_vk_buffer_address(&vk->_b),
                          dst->_uid.buf ? nvmtl_vk_buffer_address(&dst->_uid) : 0, tab ? nvmtl_vk_buffer_address(&tab->_b) : 0,
                          cntb ? nvmtl_vk_buffer_address(&cntb->_b) + cntoff : 0, nvmtl_vk_buffer_address(&recs->_b),
                          (uint32_t)stride, icount, itype == MTLAccelerationStructureInstanceDescriptorTypeIndirect ? 3u : (uint32_t)itype, nas, flags,
                          nvmtl_vk_rt_wrap(NULL) };   // rtopt5: an Indirect descriptor's bottom-level structure follows its wrapper
    if (!a.src || !a.dst || !a.rec || (cntb && !a.count_addr) || (tab && !a.table)) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"instance buffers have no device address" oom:NO]; return 0; }
    [self nvmtlTrack:ib]; [self nvmtlTrack:cntb];
    if (nvmtl_vk_cmd_convert_instances(&_cb->_c, &a)) { [self nvmtlRefuse:dst scratch:scratch_addr why:"instance conversion pass FAILED" oom:NO]; return 0; }
    return a.dst;
}
/* src: a refit's source (nil for a build). A refit is a Vulkan update (MODE_UPDATE) when src was built for it
   (usage Refit -> ALLOW_UPDATE) with the same geometry counts; else the structure is rebuilt in full. */
- (void)nvmtlBuild:(id<MTLAccelerationStructure>)as descriptor:(MTLAccelerationStructureDescriptor *)desc
           scratch:(id<MTLBuffer>)scratch offset:(NSUInteger)soff refitFrom:(id<MTLAccelerationStructure>)from {
    NVMTLAccelerationStructure *dst = (NVMTLAccelerationStructure *)as, *src = (NVMTLAccelerationStructure *)from; NVMTLBuffer *sc = (NVMTLBuffer *)scratch;
    if (!dst || !sc) { nvlog("build acceleration structure: nil %s", dst ? "scratch" : "structure"); return; }
    [self nvmtlTrack:dst]; [self nvmtlTrack:sc]; [self nvmtlTrack:src];
    if (!_cb->_resources) _cb->_resources = [NVMTLResSet new];
    nvmtl_geom *geoms = NULL; int instance = 0; uint32_t icount = 0; const char *why = NULL;
    uint32_t n = nvmtl_geoms_from_descriptor(desc, &geoms, &instance, &icount, _cb->_resources, &why);
    uint64_t scratch_addr = (nvmtl_vk_buffer_address(&sc->_b) + soff + 255ull) & ~255ull;
    if (why) { [self nvmtlRefuse:dst scratch:scratch_addr why:why oom:NO]; free(geoms); return; }
    if (![dst nvmtlEnsureCreated:instance]) { [self nvmtlRefuse:dst scratch:scratch_addr why:"the structure exists with the other level (primitive / instance) or cannot be created" oom:NO]; free(geoms); return; }
    [self nvmtlBuild2:dst desc:desc src:src geoms:geoms n:n instance:instance icount:icount scratch:scratch_addr];
    free(geoms);
}
/* transformationMatrixBuffer: each geometry's matrix is copied into Vulkan's 3x4 row-major form in a staging buffer,
   in this command buffer right before the build (so it is read when the build runs, whoever wrote it) */
- (BOOL)nvmtlStageTransforms:(nvmtl_geom *)geoms n:(uint32_t)n dst:(NVMTLAccelerationStructure *)dst scratch:(uint64_t)scratch_addr {
    uint32_t nt = 0;
    for (uint32_t i = 0; i < n; i++) nt += geoms[i].transform != 0;
    if (!nt) return YES;
    NVMTLBuffer *st = [NVMTLBuffer new];
    if (nvmtl_vk_buffer_create((size_t)nt * 64, 0, &st->_b)) { [self nvmtlRefuse:dst scratch:scratch_addr why:"transformation matrix staging FAILED" oom:YES]; return NO; }
    [self nvmtlHold:st];
    const uint64_t base = nvmtl_vk_buffer_address(&st->_b); uint32_t k = 0;
    for (uint32_t i = 0; i < n; i++) {
        if (!geoms[i].transform) continue;
        if (nvmtl_vk_cmd_transform_matrix(&_cb->_c, (const nvk_buffer *)geoms[i].transform_src, geoms[i].transform_off, (int)geoms[i].transform_rowmajor, &st->_b, (uint64_t)k * 64)) {
            [self nvmtlRefuse:dst scratch:scratch_addr why:"transformation matrix copy could not be recorded" oom:NO]; return NO; }
        geoms[i].transform = base + (uint64_t)k * 64; k++;   // 16-aligned, as VkTransformMatrixKHR data must be
    }
    return YES;
}
- (void)nvmtlBuild2:(NVMTLAccelerationStructure *)dst desc:(MTLAccelerationStructureDescriptor *)desc src:(NVMTLAccelerationStructure *)src
              geoms:(nvmtl_geom *)geoms n:(uint32_t)n instance:(int)instance icount:(uint32_t)icount scratch:(uint64_t)scratch_addr {
    if (!instance && ![self nvmtlStageTransforms:geoms n:n dst:dst scratch:scratch_addr]) return;
    uint32_t hflags = 0;
    if (!instance && ![self nvmtlPrimitiveTables:dst desc:desc geoms:geoms n:n scratch:scratch_addr flags:&hflags]) return;
    if (!instance && [desc isKindOfClass:[MTLPrimitiveAccelerationStructureDescriptor class]]) {
        NSArray *gd = ((MTLPrimitiveAccelerationStructureDescriptor *)desc).geometryDescriptors; uint32_t add = 0;
        if (gd.count) {
            const NSUInteger o0 = [(MTLAccelerationStructureGeometryDescriptor *)gd[0] intersectionFunctionTableOffset]; BOOL same = YES, affine = YES;
            for (NSUInteger g = 0; g < gd.count; g++) {
                const NSUInteger o = [(MTLAccelerationStructureGeometryDescriptor *)gd[g] intersectionFunctionTableOffset];
                same = same && o == o0; affine = affine && o == o0 + g; }
            if (o0 > 0x7FFFFF || (!same && !affine))
                nvlog("build acceleration structure: geometry intersection function table offsets (first %lu, %lu geometries) are neither equal nor base + index - the slot reads the instance offset alone", (unsigned long)o0, (unsigned long)gd.count);
            else add = (uint32_t)o0 | (same ? 0u : 0x800000u);
        }
        dst->_iftAdd = add;
    }
    uint64_t inst_addr = 0;
    if (instance && !(inst_addr = [self nvmtlInstances:desc into:dst count:icount scratch:scratch_addr])) return;
    nvk_accel *upd = NULL;
    if (src) {
        if (!src->_as.as || src->_as.instance != instance || !nvmtl_vk_accel_can_update(&src->_as, geoms, n, icount))
            nvlog("refit acceleration structure %p: not built with usage Refit, or the geometry counts changed - rebuilt in full", (__bridge void *)src);
        else if (src != dst && dst->_as.size < src->_as.size)
            nvlog("refit acceleration structure %p: destination %zu B is smaller than the source %zu B - rebuilt in full", (__bridge void *)src, dst->_as.size, src->_as.size);
        else upd = &src->_as;
        if (upd && src != dst) dst->_iftAdd = src->_iftAdd;
    }
    if (nvmtl_vk_cmd_build_accel(&_cb->_c, &dst->_as, geoms, n, inst_addr, icount, scratch_addr, nvmtl_vk_accel_flags(desc.usage), upd)) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"vkCmdBuildAccelerationStructuresKHR could not be recorded" oom:NO]; return; }
    if (!instance && ![self nvmtlWrap:dst empty:NO]) {   // rtopt5: the top-level wrapper shaders trace, over the new bounds
        [self nvmtlRefuse:dst scratch:scratch_addr why:"the primitive structure's top-level wrapper could not be built" oom:NO]; return; }
    if (![self nvmtlHeader:dst kind:(uint32_t)instance count:instance ? icount : n flags:hflags
                     table:dst->_tabObj ? nvmtl_vk_buffer_address(&dst->_tabObj->_b) : 0]) {
        [self nvmtlRefuse:dst scratch:scratch_addr why:"acceleration structure header could not be recorded" oom:NO]; return; }
    nvlog("%s acceleration structure %p: %s (%u %s)", upd ? "refit (update)" : src ? "refit (rebuild)" : "build", (__bridge void *)dst,
               instance ? "instances" : "primitives", instance ? icount : n, instance ? "instances" : "geometries");
}
- (void)buildAccelerationStructure:(id<MTLAccelerationStructure>)as descriptor:(MTLAccelerationStructureDescriptor *)desc
                     scratchBuffer:(id<MTLBuffer>)scratch scratchBufferOffset:(NSUInteger)soff {
    [self nvmtlBuild:as descriptor:desc scratch:scratch offset:soff refitFrom:nil];
}
- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)src descriptor:(MTLAccelerationStructureDescriptor *)d
                       destination:(id<MTLAccelerationStructure>)dst scratchBuffer:(id<MTLBuffer>)s scratchBufferOffset:(NSUInteger)o {
    [self nvmtlBuild:(dst ? dst : src) descriptor:d scratch:s offset:o refitFrom:src];
}
- (void)refitAccelerationStructure:(id<MTLAccelerationStructure>)src descriptor:(MTLAccelerationStructureDescriptor *)d
                       destination:(id<MTLAccelerationStructure>)dst scratchBuffer:(id<MTLBuffer>)s scratchBufferOffset:(NSUInteger)o
                           options:(MTLAccelerationStructureRefitOptions)opts {
    [self nvmtlBuild:(dst ? dst : src) descriptor:d scratch:s offset:o refitFrom:src];   // every refit updates vertices and per-primitive data alike
}
- (void)nvmtlCopy:(id<MTLAccelerationStructure>)src to:(id<MTLAccelerationStructure>)dst compact:(int)compact {
    NVMTLAccelerationStructure *s = (NVMTLAccelerationStructure *)src, *d = (NVMTLAccelerationStructure *)dst;
    if (!s || !d || !s->_as.as || !s->_as.built) { nvlog("copy acceleration structure: %s", s && d ? "source never built" : "nil"); return; }
    if (![d nvmtlEnsureCreated:s->_as.instance]) return;
    [self nvmtlTrack:s]; [self nvmtlTrack:d];
    if (nvmtl_vk_cmd_copy_accel(&_cb->_c, &s->_as, &d->_as, compact)) nvlog("copy acceleration structure: FAILED");
    else if (!s->_as.instance && ![self nvmtlWrap:d empty:NO]) nvlog("copy acceleration structure: top-level wrapper FAILED");   // rtopt5: d's own, over d's copy
    d->_iftAdd = s->_iftAdd;   // a copied primitive structure keeps its geometries' intersection function base
    // rtopt4: the copy shares the source's tables (never written after their build) under its own header
    [self nvmtlRetireTables:d];
    d->_tabObj = s->_tabObj; d->_datObj = s->_datObj;
    if (s->_hdr.magic && ![self nvmtlHeader:d kind:s->_hdr.kind count:s->_hdr.count flags:s->_hdr.flags table:s->_hdr.table])
        nvlog("copy acceleration structure: header FAILED");
    if (s->_uid.buf) {
        if (d->_uid.size < s->_uid.size) {
            if (d->_uid.buf) { NVMTLBuffer *old = [NVMTLBuffer new]; old->_b = d->_uid; [self nvmtlHold:old]; }
            memset(&d->_uid, 0, sizeof d->_uid);
            if (nvmtl_vk_buffer_create(s->_uid.size, 0, &d->_uid)) { nvlog("copy acceleration structure: side table FAILED"); memset(&d->_uid, 0, sizeof d->_uid); }
        }
        if (d->_uid.buf && nvmtl_vk_cmd_copy_buffer(&_cb->_c, &s->_uid, 0, &d->_uid, 0, s->_uid.size)) nvlog("copy acceleration structure: side table copy FAILED");
    }
}
- (void)copyAccelerationStructure:(id<MTLAccelerationStructure>)src toAccelerationStructure:(id<MTLAccelerationStructure>)dst {
    [self nvmtlCopy:src to:dst compact:0];
}
- (void)copyAndCompactAccelerationStructure:(id<MTLAccelerationStructure>)src toAccelerationStructure:(id<MTLAccelerationStructure>)dst {
    [self nvmtlCopy:src to:dst compact:1];
}
- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)as toBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off {
    [self writeCompactedAccelerationStructureSize:as toBuffer:buf offset:off sizeDataType:MTLDataTypeUInt];
}
/* The size a compacting copy needs, from the build (ANV's compacted-size query, on the GPU in this command buffer's
   order). A structure with no recorded build has nothing to compact: its allocation size is written. */
- (void)writeCompactedAccelerationStructureSize:(id<MTLAccelerationStructure>)as toBuffer:(id<MTLBuffer>)buf offset:(NSUInteger)off sizeDataType:(MTLDataType)t {
    NVMTLAccelerationStructure *a = (NVMTLAccelerationStructure *)as; NVMTLBuffer *b = (NVMTLBuffer *)buf;
    if (!a || !b) { nvlog("writeCompactedAccelerationStructureSize: nil"); return; }
    const uint64_t v = a->_b.size; const size_t n = t == MTLDataTypeULong ? 8 : 4;
    if (off > b->_b.size || b->_b.size - off < n) { nvlog("writeCompactedAccelerationStructureSize: %zu bytes at %lu is past the %zu-byte buffer - REFUSED", n, (unsigned long)off, b->_b.size); return; }
    [self nvmtlTrack:a]; [self nvmtlTrack:b];
    if (a->_as.built) {
        NVMTLBuffer *st = [NVMTLBuffer new];
        if (!nvmtl_vk_buffer_create(8, 0, &st->_b)) {
            [self nvmtlHold:st];
            // rtopt5: a primitive structure's Metal size includes its top-level wrapper (sizes report it too)
            const uint32_t add = a->_wrap.as && !a->_as.instance ? (uint32_t)NVMTL_AS_WRAP : 0u;
            if (!nvmtl_vk_cmd_write_compacted_size_add(&_cb->_c, &a->_as, &b->_b, off, (uint32_t)n, &st->_b, add)) return;
        }
        nvlog("writeCompactedAccelerationStructureSize: compacted-size query FAILED - the allocation size is written");
    }
    if (off & 3) {
        if (!nvmtl_buf_cpu(b)) { nvlog("writeCompactedAccelerationStructureSize: offset %lu is not 4-aligned and the buffer is not host-visible - REFUSED", (unsigned long)off); return; }
        static int said; if (said++ < 2) nvlog("writeCompactedAccelerationStructureSize: offset %lu is not 4-aligned - written from the CPU at encode time (unordered)", (unsigned long)off);
        memcpy((uint8_t *)nvmtl_buf_cpu(b) + off, &v, n);
        if (b->_shadow.map) [b didModifyRange:NSMakeRange(off, n)];
        return;
    }
    if (nvmtl_vk_cmd_write_word(&_cb->_c, &b->_b, off, (uint32_t)v) ||
        (n == 8 && nvmtl_vk_cmd_write_word(&_cb->_c, &b->_b, off + 4, (uint32_t)(v >> 32))))
        nvlog("writeCompactedAccelerationStructureSize: GPU write at %lu FAILED", (unsigned long)off);
}
- (void)useResource:(id<MTLResource>)r usage:(MTLResourceUsage)u { [self nvmtlTrack:r]; }
- (void)useResources:(const id<MTLResource> __unsafe_unretained [])rs count:(NSUInteger)n usage:(MTLResourceUsage)u { for (NSUInteger i = 0; i < n; i++) [self nvmtlTrack:rs[i]]; }
- (void)useHeap:(id<MTLHeap>)h { nvmtl_use_heap(_cb, h); }
- (void)useHeaps:(const id<MTLHeap> __unsafe_unretained [])hs count:(NSUInteger)n { for (NSUInteger i = 0; i < n; i++) [self useHeap:hs[i]]; }
- (void)updateFence:(id<MTLFence>)f {}
- (void)waitForFence:(id<MTLFence>)f {}
- (void)insertDebugSignpost:(NSString *)s {}
- (void)pushDebugGroup:(NSString *)s {}
- (void)popDebugGroup {}
- (void)sampleCountersInBuffer:(id<MTLCounterSampleBuffer>)b atSampleIndex:(NSUInteger)i withBarrier:(BOOL)w { nvmtl_sample_counter(_cb, b, i, self, w); }
@end

static void nvmtl_buffer_sync_in(NVMTLBuffer *b)
{
    if (!b || !b->_hostPtr || !b->_b.map || !b->_hostLen) return;
    const uint64_t t0 = nvmtl_perf_now();
    memcpy(b->_b.map, b->_hostPtr, b->_hostLen);
    nvmtl_perf_note(NVP_SYNCIN, t0, b->_hostLen);
}

static void nvmtl_buffer_sync_out(NVMTLBuffer *b)
{
    if (b && b->_shadow.map) {
        if (b->_syncOut) { b->_syncOut = 0; __atomic_store_n(&b->_zshadow, 0, __ATOMIC_RELEASE);
            if (nvmtl_vk_copy_buffer(nvmtl_device_queue(), &b->_b, 0, &b->_shadow, 0, b->_b.size)) nvlog("commit: managed sync-out of %zu bytes FAILED", b->_b.size); }
        return;
    }
    if (!b || !b->_hostPtr || !b->_b.map || !b->_hostLen) return;
    const uint64_t t0 = nvmtl_perf_now();
    memcpy(b->_hostPtr, b->_b.map, b->_hostLen);
    nvmtl_perf_note(NVP_SYNCOUT, t0, b->_hostLen);
}
