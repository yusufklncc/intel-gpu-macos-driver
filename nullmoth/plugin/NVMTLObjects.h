/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOSurface/IOSurface.h>
#include "nvmtl_vk.h"
#include "nvmtl_counter_heap.h"   /* nm29: Metal 4 counter heaps (NVMTLMetal4.m) */
#include "nvmtl_caps_query.h"     /* nm29: capability answers from the Vulkan bits */
#include "nvmtl_cmd_refusal.h"

static inline NSMutableData *nvmtl_fc_storage(NSUInteger count) {
    const NSUInteger stride = 2 * sizeof(uint32_t) + sizeof(void *) + 16;
    if (!count || count > NSUIntegerMax / stride) return nil;
    return [NSMutableData dataWithLength:count * stride];
}
@class NVMTLHeap;
@class NVMTLSamplerState;
@interface NVMTLBuffer : NSObject <MTLBuffer> { @public nvk_buffer _b; MTLStorageMode _storage; NVMTLHeap *_heap; NSUInteger _heapOffset;
    void *_hostPtr; size_t _hostLen; id _hostDealloc;
    NVMTLHeap *_subHeap; NSUInteger _subCharge; BOOL _aliased;
    nvk_buffer _shadow; int _syncOut; int _zshadow;
    NSUInteger _dirtyLo, _dirtyHi;
    uint32_t _gpuUse, _gpuUseSynced;   /* command buffers that took it, and that count at its last synchronizeResource */
    uint8_t _gpuSynced;   /* synchronizeResource copied it at least once */
    uint64_t _syncEpoch;   /* the resource set of the command buffer that last synchronized it, until it binds it again */ } @end
#include <sched.h>
static inline void nvmtl_buf_settle(NVMTLBuffer *b) {
    if (!__atomic_load_n(&b->_zshadow, __ATOMIC_ACQUIRE)) return;
    int owed = 1;
    if (__atomic_compare_exchange_n(&b->_zshadow, &owed, 2, 0, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        if (b->_shadow.map) memset(b->_shadow.map, 0, b->_shadow.size);
        __atomic_store_n(&b->_zshadow, 0, __ATOMIC_RELEASE); return; }
    while (__atomic_load_n(&b->_zshadow, __ATOMIC_ACQUIRE) == 2) sched_yield();
}
static inline void *nvmtl_buf_cpu(NVMTLBuffer *b) { nvmtl_buf_settle(b); return b->_shadow.map ? b->_shadow.map : b->_b.map; }
@interface NVMTLTexture : NSObject <MTLTexture> {
  @public nvk_image _i; nvk_queue *_q; IOSurfaceRef _surf; MTLPixelFormat _fmt; NSUInteger _plane;
  NVMTLHeap *_heap; NSUInteger _heapOffset;
  NVMTLHeap *_subHeap; NSUInteger _subCharge;
  BOOL _aliased;
  NVMTLTexture *_parent;
  void *_sampledDepthView;
  void *_ownView;
  uint32_t _mips;
  NVMTLTexture *_viewParent;
  uint32_t _viewType, _viewSlices, _relativeLevel, _relativeSlice;
  uint32_t _baseSlice, _baseLevel;
  uint32_t _surfSeed, _surfGen; BOOL _surfSynced, _surfOwned;
  nvk_buffer _vramBuf; BOOL _vramOn; BOOL _svDemoted;   /* rb7f/g: the kernel demoted its VRAM-shared surface (copy-out + marks for CPU-lock waits) */
  BOOL _p4;   /* gpuperf phase 4: this composite's last frame skipped its readback (scan-out buffer newer than the pages) */
  BOOL _stex;
  uint64_t _resid;
  NSUInteger _usage;
  NVMTLBuffer *_backBuf; NSUInteger _backOff, _backBPR;
  uint64_t _wsShownAt;   // nvmtl_ws_presented: when the display pipe last said it presents this surface
  uint32_t _surfWSeq, _surfOutSeq;   // surface writes sealed into parts, and the newest one published (surface-out order)
}
- (nvk_image *)nvi;
- (void *)nvview;
- (void *)nvsampleview;
- (void)nvmtlSurfaceIn;
- (BOOL)nvmtlSurfaceOut;
- (void)nvmtlSurfaceOutFolded;
- (void)nvmtlSurfaceOutFoldedWithSiblings:(NSArray<NVMTLTexture *> *)siblings;
- (BOOL)nvmtlSurfaceOutDirtied:(BOOL)dirtied;
@end
@interface NVMTLFunction : NSObject <MTLFunction> { @public NSData *_spirv; NSString *_fname; NSString *_stage; NSString *_air; NSDictionary *_fc; NSArray *_embedded; unsigned char _bcHash[32]; BOOL _bcHashed;
  NSDictionary *_refl; NSDictionary *_airNames; NSArray *_arguments;
  id _lib; id _fcv; NSString *_specName; BOOL _needsLink;
  BOOL _fcSpec;   // _spirv is the specialization-constant module with this function's values (nvmtl_fc_spec_variant)
} @end
NSDictionary *nvmtl_function_reflection(NVMTLFunction *function);
NSDictionary *nvmtl_air_names(NVMTLFunction *function);
uint64_t nvmtl_resid_for_texture(id t);
void nvmtl_resid_texture_gone(uint64_t token);
uint64_t nvmtl_resid_for_sampler(id s);
@interface NVMTLType : NSObject { @public MTLDataType _dataType; } @end
@interface NVMTLVertexAttribute : NSObject { @public NSString *_name; NSUInteger _index; MTLDataType _dataType; } @end
@interface NVMTLTextureReferenceType : NVMTLType { @public MTLDataType _texDataType; MTLTextureType _texType; MTLBindingAccess _access; BOOL _depth; } @end
@interface NVMTLStructType : NVMTLType { @public NSArray *_members; } @end
@interface NVMTLPointerType : NVMTLType { @public MTLDataType _elem; MTLBindingAccess _access; NSUInteger _align, _size; BOOL _isArgBuf; NVMTLStructType *_elemStruct; } @end
// an array member of a reflected struct (MTLArrayType): element type, length and stride from the AIR struct_type_info
@interface NVMTLArrayType : NVMTLType { @public MTLDataType _elem; NSUInteger _len, _stride; } @end
@interface NVMTLStructMember : NSObject { @public NSString *_name; NSUInteger _offset; MTLDataType _dataType; NSUInteger _argIndex; NVMTLArrayType *_arr;
  NVMTLStructType *_struct; NVMTLTextureReferenceType *_texRef; NVMTLPointerType *_ptr; } @end
@interface NVMTLArgument : NSObject { @public NSString *_name; MTLArgumentType _type; MTLBindingAccess _access; NSUInteger _index; BOOL _active;
  NSUInteger _arrayLength, _bufAlign, _bufSize; MTLDataType _bufDataType; NVMTLStructType *_bufStruct; NVMTLPointerType *_bufPtr;
  MTLTextureType _texType; MTLDataType _texDataType; BOOL _depth; } @end
@interface NVMTLRenderPipelineReflection : NSObject { @public NSArray *_v, *_f, *_o, *_m; } @end
@interface NVMTLComputePipelineReflection : NSObject { @public NSArray *_a; } @end
NSArray<NVMTLArgument *> *nvmtl_arguments_for_function(NVMTLFunction *fn);
NVMTLArgument *nvmtl_argument_for_buffer(NVMTLFunction *fn, NSUInteger index);
id nvmtl_render_reflection(MTLRenderPipelineDescriptor *d);
id nvmtl_compute_reflection(id<MTLFunction> fn);
id<MTLFunction> nvmtl_link_kernel(MTLComputePipelineDescriptor *d, NSError **err);
NVMTLFunction *nvmtl_kernel_with_stage_in(NVMTLFunction *fn, MTLStageInputOutputDescriptor *sd, NSError **err);
id<MTLFunction> nvmtl_link_stage(id<MTLFunction> k, MTLLinkedFunctions *lf, NSArray *dylibs, NSError **err);
void nvmtl_render_keep_linked(id ps, id<MTLFunction> vf, id<MTLFunction> ff);
// a pipeline's own function-handle list per stage (0 compute, 1 vertex, 2 fragment) and its descriptor (NVMTLLibraryLoad.m)
void nvmtl_pipe_keep_handles(id ps, int stage, MTLLinkedFunctions *lf);
void nvmtl_pipe_keep_descriptor(id ps, id d);
void nvmtl_note_compile_dylibs(id lib, NSArray *libraries);
id nvmtl_new_dylib(id<MTLLibrary> lib, NSError **err);
id nvmtl_new_dylib_url(NSURL *url, NSError **err);
id nvmtl_new_binary_archive(MTLBinaryArchiveDescriptor *d, NSError **err);
NSString *nvmtl_archive_refusal(NSArray *archives, NSString *key);
NSString *nvmtl_archive_key_compute(id<MTLFunction> f);
NSString *nvmtl_archive_key_render(MTLRenderPipelineDescriptor *d);
static inline uint32_t nvmtl_vk_layers_for_desc(uint32_t t, uint32_t n, uint32_t depth) { return t == 7 ? (depth ? depth : 1u) : nvmtl_vk_layers_for_type(t, n); }

@interface NVMTLArgumentEncoder : NSObject <MTLArgumentEncoder> {
  @public NSArray<NSDictionary *> *_fields; NSDictionary *_fieldByIndex;
  int64_t *_offAt; NSUInteger _offN;
  NVMTLBuffer *_dst; NSUInteger _dstOffset; NSString *_label;
  NSUInteger _airSize, _airAlign;
} @end
extern id gNVMTLMainDevice;
@interface NVMTLLibrary : NSObject <MTLLibrary> { @public NSDictionary<NSString *, NSData *> *_fns; NSMutableDictionary<NSString *, NSData *> *_compiledFns; NSDictionary<NSString *, NSString *> *_stages, *_airs; NSArray<NSString *> *_externs;
  NSData *_raw; id _appleTwin; BOOL _twinTried; NSString *_rawPath; } @end
@interface NVMTLRenderPipelineState : NSObject <MTLRenderPipelineState> { @public nvk_pipeline _p;
  NSString *_vname, *_fname, *_blend, *_vdesc; MTLRenderPipelineDescriptor *_descriptor; NSMutableDictionary *_samplerVariants;
  NVMTLFunction *_linkedVert, *_linkedFrag; uint32_t _a8mask; } @end
@interface NVMTLCommandQueue : NSObject <MTLCommandQueue> {
  // _resSets: the residency sets this queue was given. A set retains its allocations, so holding the
  // set is what keeps them alive; without this nobody did, and an app that handed its only reference
  // to the queue had every resource in the set freed under work the queue had not finished.
  @public nvk_queue _q; id _dev; id _completionQueue; dispatch_queue_t _callbackOrderQueue; NSMutableArray *_reservations; dispatch_semaphore_t _capacity; NSUInteger _capacityCount; NSMutableArray *_resSets;
} @end
@interface NVMTLCommandBuffer : NSObject <MTLCommandBuffer> {
  NSError *_cbError;
  NSMutableDictionary *_userDictionary;
  @public nvk_cmdbuf _c; NVMTLCommandQueue *_queue; BOOL _committed, _commitMarking; BOOL _capacityHeld; MTLCommandBufferStatus _cbstatus;
  NVMTLTexture *_present;
  NSMutableArray *_presentations;
  NSMutableArray *_handlers;
  // perf1: completed handlers as retained block pointers (Apple's GL adds one per draw), made an array at completion
  void **_hv; unsigned _hn, _hcap;
  void *_hchunks;   // glcpu3: nvmtl_hchunk list holding this command buffer's arena copies of _hv entries tagged NVMTL_HTAG
  NSMutableArray *_scratch;
  NSMutableSet *_resources;
  NSMutableSet *_surfDirty;
  NSMutableSet *_surfOverwrites; // contents defined by writes recorded in this command buffer
  unsigned _nEnc, _nEnd, _nHandlers, _nStatus, _nEnqueue, _nLabel, _nPresent, _nWait;
  unsigned _nEncSealed;
  CFTimeInterval _gpuStart, _gpuEnd;
  CFTimeInterval _legacyKernelStart, _legacyKernelEnd;
  nvmtl_gpu_interval _gpuTiming; BOOL _gpuTimingSeen, _gpuTimingInvalid, _gpuTimesPending;   // pending: converted on the first GPUStartTime/EndTime
  NSMutableArray *_parts, *_scheduledHandlers;
  NSUInteger _nextPart;
  NSUInteger _nextSubmit;
  id _reservation;
  // NVMTL_LOCK_V2 (nvmtl_plock.h): the submit-ahead pass is inside this command buffer (busy) / it has finished or is
  // finishing (fin); it is in nvmtl_pending_buffers (pending, under the state lock)
  int _v2busy, _v2fin; BOOL _v2pending;
  NSCondition *_completionCondition;
  dispatch_group_t _callbackGroup, _scheduledGroup;
  BOOL _wasScheduled, _executionDone, _completionReady, _scheduleAfterPublish;
  uint64_t _doneSplitNs;   // NVMTL_DONE_SPLIT: when the worker published the completion (CLOCK_UPTIME_RAW)
  BOOL _wsCaptureWait;   // WindowServer: it writes a surface the display does not present (nvmtl_ws_note_captures)
  // per set (vertex/compute 0, fragment 1): the sampler the app bound at each slot, and the slots a constexpr sampler took
  // since (nvmtl_bind_embedded gives them back to the app's sampler when a pipeline without one there draws)
  // unretained: every sampler stored here is in the command buffer's resource set (the setter adds it) for the command
  // buffer's life, so the slot needs no strong reference; the retain/release pair was the first atomic after each draw's
  // write-combined batch writes and paid their drain (~100 ns a draw, stage 1d)
  __unsafe_unretained NVMTLSamplerState *_appSamp[2][16]; uint16_t _staticSamp[2];
  NSMutableArray *_signals;
  NSMutableArray *_waits;
}
- (void)nvmtlReleaseCapacity;
- (void)nvmtlRecordEncodingError:(NSError *)error;
@end
// implemented in NVMTLEventSync.m
@interface NVMTLCommandBuffer (EventSubmission)
- (void)nvmtlReserveQueue;
- (void)nvmtlReleaseReservation;
- (void)nvmtlRecordWait:(id)event value:(uint64_t)value;
- (void)nvmtlRecordSignal:(id)event value:(uint64_t)value;
- (void)nvmtlSubmitWithEvents;
- (BOOL)nvmtlAdvancePart;
- (BOOL)nvmtlSubmitAhead;
- (BOOL)nvmtlSubmitAheadV2;
- (void)nvmtlFinishCallbacks;
@end
@interface NVMTLDepthStencilState : NSObject <MTLDepthStencilState> { @public int _write; int _compare;
  int _stencil; uint32_t _sf[6], _sb[6]; } @end
@interface NVMTLSamplerState : NSObject <MTLSamplerState> { @public nvk_sampler _s; NSDictionary *_pixelState; uint64_t _resid; } @end
@interface NVMTLFence : NSObject <MTLFence> { @public id<MTLDevice> _dev; NSString *_label; } @end
@interface NVMTLResidencySet : NSObject <MTLResidencySet> { @public id<MTLDevice> _dev; NSString *_label; NSMutableArray *_allocs; } @end
@interface NVMTLEvent : NSObject <MTLEvent> { @public id<MTLDevice> _dev; NSString *_label; uint64_t _value; NSCondition *_condition; } @end
@interface NVMTLSharedEvent : NSObject <MTLSharedEvent> { @public id<MTLDevice> _dev; NSString *_label; uint64_t _value; NSCondition *_condition; NSMutableArray *_notifications; } @end
@interface NVMTLHeap : NSObject <MTLHeap> {
  @public id<MTLDevice> _dev; NSUInteger _size; NSUInteger _used;
  MTLStorageMode _storage; MTLCPUCacheMode _cache; NSString *_label;
  nvk_heap _hm; MTLHeapType _type;
}
- (void)nvmtlReleaseSubAllocation:(NSUInteger)bytes;
- (void)nvmtlGiveRange:(NSUInteger)off length:(NSUInteger)len;
@end
@interface NVMTLComputePipelineState : NSObject <MTLComputePipelineState> { @public nvk_pipeline _p; NVMTLFunction *_function; NSMutableDictionary *_samplerVariants;
    BOOL _icb;
} @end
@interface NVMTLIndirectCommand : NSObject <MTLIndirectRenderCommand, MTLIndirectComputeCommand> {
  @public id _ps; NSMutableArray *_vbufs; NSMutableArray *_fbufs;
  MTLPrimitiveType _type; NSUInteger _start, _count, _instances, _baseInstance;
  id _ibuf; NSUInteger _ioff; MTLIndexType _itype; NSInteger _baseVertex;
  BOOL _indexed, _armed;
  MTLCullMode _cull; BOOL _hasCull;
  MTLWinding _winding; BOOL _hasWinding;
  MTLTriangleFillMode _fill; BOOL _hasFill;
  MTLDepthClipMode _clip; BOOL _hasClip;
  id _ds; float _bias, _slope, _clamp; BOOL _hasBias;
  id _cps; NSMutableArray *_kbufs; MTLSize _grid, _tg; BOOL _isCompute;
} @end
@interface NVMTLIndirectCommandBuffer : NSObject <MTLIndirectCommandBuffer> {
  @public NSMutableArray<NVMTLIndirectCommand *> *_cmds; id<MTLDevice> _dev; NSString *_label;
} @end
#ifndef NVMTL_NBUFAT
#define NVMTL_NBUFAT 32   /* buffer argument indices an encoder tracks (Metal has 31) */
#endif
@interface NVMTLComputeCommandEncoder : NSObject <MTLComputeCommandEncoder> {
  @public NVMTLCommandBuffer *_cb; NVMTLComputePipelineState *_ps; NSMutableArray *_scratch; NSMutableDictionary *_samplers;
  uint64_t _cpixMask;   // sampler indices (< 64) whose _samplers entry exists: a plain sampler bound elsewhere skips the dictionary
  NSMutableDictionary *_tgLen;
  // The binding table, by Metal buffer index: the buffer, its offset and, for setBytes, the inline length and base.
  // Plain arrays: WAS an NSMutableDictionary of NSArrays, three allocations on every setBuffer (a large part of the
  // ~2.5 us of host time per dispatch against Apple's ~0.3 us). _baN: 0 unbound, 2 = buffer+offset, 4 = setBytes form.
  __strong id _baObj[NVMTL_NBUFAT]; NSUInteger _baOff[NVMTL_NBUFAT], _baLen[NVMTL_NBUFAT], _baBase[NVMTL_NBUFAT];
  uint8_t _baN[NVMTL_NBUFAT];
  NSMutableSet *_resources;
  NSMutableDictionary *_sassTextures, *_sassSamplers;   // by Metal index, for kernels run from machine code
} @end
@interface NVMTLBlitCommandEncoder : NSObject <MTLBlitCommandEncoder> { @public NVMTLCommandBuffer *_cb; __weak NVMTLCommandBuffer *_owner; } @end
/* rtopt4: _b = [NVMTL_AS_HEADER: the Metal header (nvmtl_rt_layout.h)][the Vulkan structure]. _tabObj its table
   (geometry records of a primitive structure, instance records of an instance structure), _datObj the build's own
   copies of curve control points / radii / indices and primitive data (Metal lets those buffers go after the build).
   Both are new per build and never written after it: copies share them. _hdr the header last recorded.
   rtopt5: a primitive structure (when nvmtl_vk_rt_wrap()) is [header][_wrap: one-instance top-level structure, NVMTL_AS_WRAP
   bytes][_as: the bottom-level one]; _wrapAux holds the wrapper's instance (64 B at 0) and its build scratch (at 256),
   both rewritten by every wrapper build (nvmtlWrap). */
@interface NVMTLAccelerationStructure : NVMTLBuffer <MTLAccelerationStructure> { @public nvk_accel _as; nvk_buffer _uid; uint32_t _iftAdd;
    NVMTLBuffer *_tabObj, *_datObj; nvmtl_as_header _hdr; nvk_accel _wrap; NVMTLBuffer *_wrapAux; } @end
@interface NVMTLAccelerationStructureCommandEncoder : NSObject <MTLAccelerationStructureCommandEncoder> { @public NVMTLCommandBuffer *_cb; __weak NVMTLCommandBuffer *_owner; } @end
/* *out: a malloc'd array of the descriptor's geometries (the caller frees it; NULL for instance descriptors) */
uint32_t nvmtl_geoms_from_descriptor(id desc, nvmtl_geom **out, int *instance, uint32_t *instance_count, NSMutableSet *track, const char **refused);
@interface NVMTLRenderCommandEncoder : NSObject <MTLRenderCommandEncoder> {
  @public NVMTLCommandBuffer *_cb; NVMTLTexture *_target; float _clear[4]; BOOL _begun;
  NVMTLTexture *_resolve; uint32_t _passSamples;
  int _cull; uint32_t _passFmt; int _passDepth;
  uint32_t _load; BOOL _hasVP, _hasSC; float _vp[6]; uint32_t _sc[4];
  NSMutableDictionary *_vertexSamplers, *_fragmentSamplers;
  NSData *_pixelSamplerKey; // sampler specialization depends on values, not the draw or pipeline
  NSMutableArray *_pixelSamplerKeys; // at most eight recent value snapshots, owned by this encoder
  // the buffer and offset bound at each vertex/fragment index, read by embedded argument binding; plain arrays because
  // Apple's GL rebinds buffers for every draw, and a dictionary of (buffer, offset) arrays allocated on each one
  // unretained: every buffer stored here is held by the command buffer (its resource set, or _scratch for setBytes data)
  // for longer than this encoder lives, and a strong store cost a retain/release pair per bind (mtl Minecraft sample)
  __unsafe_unretained NVMTLBuffer *_vbufAt[NVMTL_NBUFAT], *_fbufAt[NVMTL_NBUFAT]; NSUInteger _vbufOff[NVMTL_NBUFAT], _fbufOff[NVMTL_NBUFAT];
  NVMTLRenderPipelineState *_ps;
  NSMutableArray *_scratch;
  NVMTLDepthStencilState *_ds;
  NVMTLBuffer *_tfb; NSUInteger _tfbOff, _tfbStride; float _tfScale; BOOL _tfScaleSet;
  NVMTLBuffer *_visBuf;
  uint32_t _visSlot; NSUInteger _visOffset;
  BOOL _visActive, _visPending;
  NSMutableArray *_visRecords;
  nvk_pass _pass; nvk_rtsig _passSig; BOOL _hasRef, _refused;
} @end
@interface NVMTLRenderCommandEncoder ()
- (BOOL)nvmtlBuildPass:(MTLRenderPassDescriptor *)d;
- (BOOL)nvmtlBeginPassLoad:(uint32_t)load;
- (void)nvmtlApplyDepthStencil;
@end

NSArray<NSDictionary *> *nvmtl_embedded_bindings(NVMTLFunction *function);
void nvmtl_bind_embedded(__unsafe_unretained NVMTLCommandBuffer *cb, uint32_t set, __unsafe_unretained NVMTLFunction *fn, NVMTLBuffer *__unsafe_unretained const *bufAt, const NSUInteger *offAt);
void nvmtl_bind_embedded_enc(NVMTLCommandBuffer *cb, uint32_t set, NVMTLFunction *fn, NVMTLComputeCommandEncoder *e);
// the NSArray form of one binding ({buffer, offset} or {buffer, offset, length, base}), for the few readers that want it
static inline NSArray *nvmtl_bufat(NVMTLComputeCommandEncoder *e, NSUInteger i) {
    if (!e || i >= NVMTL_NBUFAT || !e->_baN[i] || !e->_baObj[i]) return nil;
    return e->_baN[i] == 4 ? @[e->_baObj[i], @(e->_baOff[i]), @(e->_baLen[i]), @(e->_baBase[i])] : @[e->_baObj[i], @(e->_baOff[i])];
}
uint64_t nvmtl_resid_for_texture(id t);
uint64_t nvmtl_resid_for_sampler(id s);

@interface NVMTLParallelRenderCommandEncoder : NSObject <MTLParallelRenderCommandEncoder> { @public NVMTLCommandBuffer *_cb; MTLRenderPassDescriptor *_desc;
  NSMutableArray *_subs; NSLock *_lock; BOOL _ended; NSString *_label; } @end

@interface NVMTLCounter : NSObject <MTLCounter> @end
@interface NVMTLCounterSet : NSObject <MTLCounterSet> { @public NSArray *_counters; } @end
@interface NVMTLCounterSampleBuffer : NSObject <MTLCounterSampleBuffer> {
  @public void *_pool; NSUInteger _count; MTLStorageMode _storage; NSString *_label; id<MTLDevice> _dev; } @end
id<MTLCounterSet> nvmtl_timestamp_counter_set(void);

@interface NVMTLBuffer () { @public NSString *_label; MTLResourceOptions _ropt;
  int _resRefs, _rstate, _lruReg, _wantOn; uint64_t _lastUse;
  uint64_t _useEp;
  BOOL _texSeen;
} @end
@interface NVMTLTexture () { @public NSString *_label; MTLResourceOptions _ropt; BOOL _shareable; BOOL _fbo;
  MTLTextureSwizzleChannels _swz; BOOL _hasSwz; void *_swzView;
  void *_tbView, *_tbAlias; BOOL _tbStorage;
  int _resRefs;
  int _nviews;
  int _rstate;
  uint64_t _lastUse;
  int _lruReg, _wantOn;
  uint64_t _useEp;
} @end
void nvmtl_texture_desc_swizzle(NVMTLTexture *t, MTLTextureDescriptor *d);
#include <os/lock.h>
@interface NVMTLResSet : NSMutableSet { @public NSMutableSet *_s; os_unfair_lock _lk; const void *_mru[8]; unsigned _mruNext; uint64_t _epoch; }
- (NVMTLResSet *)nvmtlTakeAll;
@end
@interface NVMTLFunction () { @public NSString *_label; } @end
// machine code compiled ahead of time for this function (NVMTLSass.m), nil otherwise
@interface NVMTLFunction () { @public id _sassKernel; } @end
// the MTLFunctionDescriptor options it was made with (MTLFunctionOptionCompileToBinary for a binary function); answered by -options
@interface NVMTLFunction () { @public MTLFunctionOptions _opts; } @end
@interface NVMTLLibrary () { @public NSString *_label; } @end
@interface NVMTLCommandQueue () { @public NSString *_label; } @end
@interface NVMTLCommandBuffer () { @public NSString *_label; uint64_t _rep; } @end
@interface NVMTLCommandBuffer () { @public NSMutableSet *_stTwins; } @end
@interface NVMTLTexture () { @public nvk_image _stTwin; BOOL _stTwinTried; BOOL _mipsWritten; } @end
@interface NVMTLDepthStencilState () { @public NSString *_label; } @end
@interface NVMTLSamplerState () { @public NSString *_label; } @end
@interface NVMTLComputePipelineState () { @public NSString *_label; } @end
@interface NVMTLComputePipelineState () { @public uint64_t _resid; NSUInteger _attrT, _askT; } @end
// _staticTG: the function's own threadgroup variables in bytes (staticThreadgroupMemoryLength), from its AIR
@interface NVMTLComputePipelineState () { @public NSUInteger _staticTG; } @end
// _sassKernel: machine code that runs before the Vulkan pipeline; _sassOnly: there is no Vulkan pipeline
@interface NVMTLComputePipelineState () { @public id _sassKernel; BOOL _sassOnly; } @end
@interface NVMTLRenderPipelineState () { @public uint64_t _resid; } @end
// macOS 26 required threadgroup sizes, as the pipeline descriptor gave them (0x0x0 = not set). Apps read them back from
// the pipeline; without the getters the read is an unrecognized selector and the app stops.
@interface NVMTLRenderPipelineState () { @public MTLSize _reqObj, _reqMesh; } @end
@interface NVMTLComputePipelineState () { @public MTLSize _reqTG; } @end
// A descriptor's MTLSize property that exists only on macOS 26 and later: read by key so the plugin, built for macOS 15,
// needs no availability check and gets 0x0x0 on an older system that has no such property.
static inline MTLSize nvmtl_desc_size(id d, SEL sel) {
    MTLSize s = { 0, 0, 0 };
    if (!d || ![d respondsToSelector:sel]) return s;
    id v = [d valueForKey:NSStringFromSelector(sel)];
    if ([v isKindOfClass:[NSValue class]] && !strcmp([(NSValue *)v objCType], @encode(MTLSize))) [(NSValue *)v getValue:&s size:sizeof s];
    return s;
}
@interface NVMTLIndirectCommandBuffer () { @public uint64_t _resid; } @end
#include "nvmtl_icb_layout.h"
#include "nvmtl_icb_dgc.h"
/* GPU-encodable ICBs (nvmtl_icb_layout.h): _rec is the slot record buffer, made when the gpuResourceID is first taken
   (the address a shader's command_buffer holds); nil for an ICB only the CPU encodes, which keeps the replay path.
   _rec lives in VRAM when it can (the execute's indirect draws read it with one MI_LOAD_REGISTER_MEM per parameter: from
   system RAM that was ~5 us per draw on the B580); _shadow is the CPU's copy of what the CPU wrote, so the CPU never
   reads the records back through the BAR. _refusedChecks counts the completed executes the refusal diagnostic read. */
/* NVMTL_ICB_DGC=1 (nvmtl_icb_dgc.h): _stride the record size (NVMTL_ICB_RECORD_BYTES, or NVMTL_ICB_DGC_RECORD_BYTES with the
   knob; 0 before the records exist); what the descriptor said about inheritance (a shader may set buffers only in an ICB
   that does not inherit them); _dgcMode word 20 of every record; _learnTg the threadgroup size a shader asked for in a
   compute slot whose dispatch was refused, read back after completion (3 words a slot, 0 = none) */
@interface NVMTLIndirectCommandBuffer () { @public uint32_t _stride; BOOL _inheritBuffers, _inheritPipeline; uint32_t _dgcMode; uint32_t *_learnTg; } @end
/* record i of a record array with the ICB's stride (the classic 64-byte records, or the 1024-byte ones with the knob) */
static inline nvmtl_icb_record *nvmtl_icb_at(const void *base, uint32_t stride, NSUInteger i) {
    return (nvmtl_icb_record *)((uint8_t *)base + i * (size_t)(stride ? stride : NVMTL_ICB_RECORD_BYTES)); }
@interface NVMTLIndirectCommandBuffer () { @public NVMTLBuffer *_rec; nvmtl_icb_record *_shadow; uint32_t _refusedSaid, _refusedChecks; }
- (nvmtl_icb_record *)nvmtlRecords;      /* the CPU's copy */
- (nvmtl_icb_record *)nvmtlGPURecords;   /* the record buffer's mapping (what a shader wrote): read sparingly */
- (void)nvmtlSetExecPrim:(NSUInteger)i prim:(uint32_t)prim;
- (void)nvmtlWriteRecord:(NSUInteger)i;
- (void)nvmtlWriteDGCRecord:(NSUInteger)i head:(const nvmtl_icb_record *)v;   /* NVMTL_ICB_DGC=1 */
- (void)nvmtlSetWord:(NSUInteger)i word:(uint32_t)w value:(uint32_t)v;           /* a CPU-owned word of record i, both copies */
@end
@interface NVMTLIndirectCommand () { @public __unsafe_unretained NVMTLIndirectCommandBuffer *_owner; NSUInteger _slot; } @end
@interface NVMTLIndirectCommand () - (void)nvmtlCopyFrom:(NVMTLIndirectCommand *)o; @end
NSUInteger nvmtl_air_max_tg(NVMTLFunction *fn);
NSUInteger nvmtl_air_static_tg(NVMTLFunction *fn);
BOOL nvmtl_air_grid_only(NVMTLFunction *fn);
// _gridOnly: 0 not yet asked, 1 the kernel's result does not depend on its threadgroup shape, -1 it may
@interface NVMTLComputePipelineState () { @public int8_t _gridOnly; } @end
@interface NVMTLRenderCommandEncoder () { @public NSString *_label; } @end
@interface NVMTLRenderCommandEncoder () { @public BOOL _dbOn; float _dbLo, _dbHi; } @end   // setDepthTestMinBound:maxBound:
@interface NVMTLRenderCommandEncoder () { @public uint32_t _vstride[NVMTL_NVIN]; uint32_t _vstrideSet; } @end
@interface NVMTLComputeCommandEncoder () { @public NSString *_label; } @end
@interface NVMTLBlitCommandEncoder () { @public NSString *_label; } @end
@interface NVMTLAccelerationStructureCommandEncoder () { @public NSString *_label; } @end
// stage-boundary GPU timestamps: the end-of-pass samples of the pass descriptor's sampleBufferAttachments, written at
// endEncoding (sample buffer, index pairs; nil without any)
@interface NVMTLRenderCommandEncoder () { @public NSArray *_tsEnd; } @end
@interface NVMTLComputeCommandEncoder () { @public NSArray *_tsEnd; } @end
@interface NVMTLBlitCommandEncoder () { @public NSArray *_tsEnd; } @end
@interface NVMTLAccelerationStructureCommandEncoder () { @public NSArray *_tsEnd; } @end
@interface NVMTLHeap () { @public MTLHazardTrackingMode _hazard; } @end
@interface NVMTLHeap () { @public id _sparse; } @end      // a sparse heap's tile pool (NVMTLSparse.m, NVMTL_SPARSE_TEXTURES)
@interface NVMTLTexture () { @public id _sparse; } @end   // a sparse texture's image layout and page table (NVMTLSparse.m)
@interface NVMTLBuffer () { @public id _sparse; } @end    // a placement sparse buffer's page table (NVMTLSparse.m)
@interface NVMTLIndirectCommandBuffer () { @public MTLResourceOptions _opts; } @end
@interface NVMTLBuffer () { @public uint32_t _purge; } @end
@interface NVMTLTexture () { @public uint32_t _purge; } @end
@interface NVMTLHeap () { @public uint32_t _purge; } @end
#include <os/lock.h>
@interface NVMTLHeap () { @public NSUInteger *_fr; uint32_t _nfr, _cfr; os_unfair_lock _fl; BOOL _backed; } @end
@interface NVMTLBuffer () { @public BOOL _subPlaced; } @end
typedef struct { nvk_image img; uint32_t key[12]; NSUInteger off, len; } nvmtl_park_t;
@interface NVMTLTexture () { @public BOOL _subPlaced, _parkable; uint32_t _pkey[12]; NSUInteger _pkLen; } @end
@interface NVMTLHeap () { @public nvmtl_park_t *_pk; uint32_t _npk; } @end
// Lazy backing: _lzc = slot size (0 = the heap owns one _hm of its full size). _lzo[slot] = 1 + index into _lzm/_lzb (0 = no
// memory yet); one backing may cover several slots (a resource larger than a slot gets one backing for all of them).
@interface NVMTLHeap () { @public size_t _lzc; uint32_t _lzn, _lzcap, _lzslots; nvk_heap *_lzm; size_t *_lzb; uint16_t *_lzo;
                         os_unfair_lock _lzl; } @end
@interface NVMTLBuffer () { @public nvmtl_park_t *_bpk; uint32_t _nbpk; os_unfair_lock _bpl; } @end
// What this buffer last put in the Metal 4 address table: base, end, and the table epoch at the time. -gpuAddress
// skips the table (one global lock) while all three still hold - see nvmtl_m4_note_current.
@interface NVMTLBuffer () { @public _Atomic uint64_t _m4note[3]; } @end
@interface NVMTLTexture () { @public BOOL _bparkable; } @end
static void nvmtl_heap_park(NVMTLHeap *h, nvk_image *img, const uint32_t *key, NSUInteger off, NSUInteger len);
void nvmtl_heap_back(NVMTLHeap *h, NSUInteger size, int shared);
@interface NVMTLIndirectCommandBuffer () { @public uint32_t _purge; } @end
static inline MTLPurgeableState nvmtl_purge(uint32_t *st, MTLPurgeableState s) {
    if (s < MTLPurgeableStateKeepCurrent || s > MTLPurgeableStateEmpty) {
        static int said; if (!said++) nvlog("setPurgeableState: %lu is no MTLPurgeableState - answered as KeepCurrent", (unsigned long)s);
        s = MTLPurgeableStateKeepCurrent;
    }
    uint32_t prev = s == MTLPurgeableStateKeepCurrent ? __atomic_load_n(st, __ATOMIC_ACQUIRE) : __atomic_exchange_n(st, (uint32_t)s, __ATOMIC_ACQ_REL);
    return prev ? (MTLPurgeableState)prev : MTLPurgeableStateNonVolatile;
}
static inline kern_return_t nvmtl_owner_check(task_id_token_t tok) {
    mach_port_t p = MACH_PORT_NULL;
    if (task_identity_token_get_task_port(tok, TASK_FLAVOR_NAME, &p) != KERN_SUCCESS) return kIOReturnBadArgument;
    mach_port_deallocate(mach_task_self(), p);
    return KERN_SUCCESS;
}
static inline MTLCPUCacheMode nvmtl_res_cache(NVMTLHeap *hp, MTLResourceOptions o) { return hp ? hp->_cache : (MTLCPUCacheMode)(o & 0xF); }
static inline MTLHazardTrackingMode nvmtl_res_hazard(NVMTLHeap *hp, MTLResourceOptions o) {
    if (hp) return hp->_hazard == MTLHazardTrackingModeTracked ? MTLHazardTrackingModeTracked : MTLHazardTrackingModeUntracked;
    return ((o >> MTLResourceHazardTrackingModeShift) & 0x3) == MTLHazardTrackingModeUntracked ? MTLHazardTrackingModeUntracked : MTLHazardTrackingModeTracked;
}
extern unsigned nvmtl_apps1_resolve_miss, nvmtl_apps1_resolve_extra, nvmtl_apps1_ds_resolve_miss;
@interface MTLSharedTextureHandle (NVMTLSPI)
- (instancetype)initWithIOSurface:(IOSurfaceRef)surface label:(NSString *)label;
- (IOSurfaceRef)ioSurface;
@end
@interface MTLIOAccelDevice (NVMTLApps1)
- (id<MTLTexture>)newSharedTextureWithHandle:(MTLSharedTextureHandle *)handle;
@end

NSDictionary *nvmtl_index_argument_fields(NSArray *fields);
/* Binding front end (DESCRIPTOR-MODEL.md, stage 1): per-draw state the encoder used to look up through associated
   objects, dictionaries keyed by NSString and a monotonic clock read per call.
   - NVMTLFunction: its constexpr samplers and embedded argument-buffer plan, built once (NVMTLEmbedded.m) and read
     without a lock after _bindReady is published (release/acquire).
   - NVMTLRenderPipelineState: its NVMTLFunction objects (the descriptor's property getters retain/autorelease on every
     call), published the same way.
   - NVMTLRenderCommandEncoder: _pixMask[set] has bit i set while _vertexSamplers/_fragmentSamplers hold an entry for
     sampler index i, so binding a sampler without a pixel-coordinate state (all but unnormalized ones) touches neither
     dictionary. */
@interface NVMTLFunction () { @public NSArray *_bindSS, *_bindEmb; uint16_t _bindTook; int _bindReady; } @end
@interface NVMTLRenderPipelineState () { @public NVMTLFunction *_bindVF, *_bindFF; int _bindFnReady; } @end
@interface NVMTLRenderCommandEncoder () { @public uint16_t _pixMask[2]; } @end
#include <mach/mach_time.h>
/* The clock of the 250 ms diagnostic-flag pollers (trace, dumps, static-linear ...), read on every bind and draw: the
   commpage's approximate time (a load), not CLOCK_MONOTONIC_RAW (mach_continuous_time + timebase conversion, ~5% of a
   GL-like draw's CPU time in mtl_bind gl, 2026-10-29). Only "has 250 ms passed" depends on it. */
static inline uint64_t nvmtl_poll_ns(void)
{
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    const uint64_t t = mach_approximate_time();
    // x86 Macs tick in ns (1/1): no 64-bit multiply and divide on each of the ~10 flag polls a draw makes
    return tb.numer == tb.denom ? t : t * tb.numer / tb.denom;
}
/* Stage 1b (DESCRIPTOR-MODEL.md): a resource remembers the command buffer resource set it was last added to, as that set's
   epoch (NVMTLResSet._epoch: unique per set and renewed whenever anything leaves the set). A bind of a resource whose tag
   equals the set's current epoch is already a member: no lock, no hash lookup (nvmtl_retain_resource). */
@interface NVMTLTexture () { @public uint64_t _rsTag; } @end
@interface NVMTLBuffer () { @public uint64_t _rsTag; uint64_t _cpStamp; } @end
@interface NVMTLSamplerState () { @public uint64_t _rsTag; } @end
@interface NVMTLRenderPipelineState () { @public uint64_t _rsTag; } @end
