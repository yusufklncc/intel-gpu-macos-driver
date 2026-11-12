/* NVMTLSparse.m - Metal sparse textures, tier 1, over Vulkan sparse image residency (NVMTL_SPARSE_TEXTURES=1).
   Imported into NVMTLDevice.m's translation unit after NVMTLObjects.m; the Vulkan half is nvmtl_sparse_vk.c and the
   design note docs/design/DESIGN-sparse-textures.md.

   What maps to what:
     MTLHeapTypeSparse heap          one VkDeviceMemory of whole tiles (a page pool), a free stack of its pages
     texture made on that heap       a VkImage with SPARSE_BINDING | SPARSE_RESIDENCY, nothing bound: every tile reads 0
     placement sparse texture (M4)   the same image; its tiles come from an MTLHeapTypePlacement heap at offsets the app
                                     names (MTL4CommandQueue updateTextureMappings:heap:operations:count:)
     updateTextureMapping(s)         per tile: a page taken from (or given back to) the heap's pool, and a
                                     VkSparseImageMemoryBind (a mip-tail page: a VkSparseMemoryBind of the tail's opaque
                                     range), collected in the command buffer's recording and bound when that recording
                                     is submitted, after everything submitted before it (nvmtl_sparse_vk.c)
     moveTextureMappingsFromTexture  the source's pages handed to the destination's unmapped tiles (an unbind and a bind)
     sparse_color in shaders         OpImageSparse* + OpImageSparseTexelsResident (translator: images/residency.rs)
   Pages are taken and given back when the mapping is ENCODED, in encoding order, as the bind they produce goes to the GPU
   in the same order; a command buffer that is encoded and never committed leaves its pages taken. */
#include "nvmtl_sparse.h"

@interface NVMTLCommandBuffer (NVMTLSparseSeal)
- (void)nvmtlSealPartWithSignal:(id)event value:(uint64_t)value restart:(BOOL)restart;
@end

#define NVMTL_SP_NONE UINT32_MAX

// A sparse heap's tile pool: the memory and a stack of free pages. The lock also guards the page tables of the
// textures made on the heap, since their entries are this pool's pages.
@interface NVMTLSparsePool : NSObject { @public void *_mem; size_t _bytes; uint32_t _tileBytes, _pages, _nfree, *_free; os_unfair_lock _lk; } @end
@implementation NVMTLSparsePool
- (void)dealloc { nvmtl_vk_sparse_pool_free(_mem, _bytes); free(_free); }
@end
static uint32_t nvmtl_sp_take(NVMTLSparsePool *p) { return p->_nfree ? p->_free[--p->_nfree] : NVMTL_SP_NONE; }
static void nvmtl_sp_give(NVMTLSparsePool *p, uint32_t pg) { if (pg < p->_pages && p->_nfree < p->_pages) p->_free[p->_nfree++] = pg; }

// A sparse texture's state: its image, layout, and which page backs each tile (NVMTL_SP_NONE: unmapped). Tiles are
// numbered per Vulkan layer (2D array slice, cube face; a 3D texture has one) and mip level below the tail, x fastest,
// then the tail pages of each layer (or one tail for all, VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT).
@interface NVMTLSparseTex : NSObject { @public
    NVMTLSparsePool *_pool;     // made on a sparse heap: pages come from this pool (nil: placement sparse)
    void *_mem; uint32_t _memtype;   // placement sparse: the heap memory its tiles were mapped from (the first map fixes it)
    nvmtl_sparse_layout _lay;
    void *_img;
    uint32_t _w, _h, _d, _mips, _layers, _type;
    uint32_t _mipBase[32], _tx[32], _ty[32], _tz[32];
    uint32_t _perLayer, _tailPages, _tailBase, _n;
    uint32_t *_page;
    os_unfair_lock _lk;         // placement sparse textures' own lock
} @end
@implementation NVMTLSparseTex
- (void)dealloc {
    // the image is gone already (nvmtl_vk_image_destroy destroys a sparse image at once, which unbinds it): the pages
    // go back to the pool
    if (_pool && _page) {
        os_unfair_lock_lock(&_pool->_lk);
        for (uint32_t i = 0; i < _n; i++) if (_page[i] != NVMTL_SP_NONE) nvmtl_sp_give(_pool, _page[i]);
        os_unfair_lock_unlock(&_pool->_lk);
    }
    free(_page);
}
@end
static os_unfair_lock *nvmtl_sp_lock(NVMTLSparseTex *st) { return st->_pool ? &st->_pool->_lk : &st->_lk; }

static NVMTLSparseTex *nvmtl_sp_state(id<MTLTexture> t)
{
    if (![(id)t isKindOfClass:[NVMTLTexture class]]) return nil;
    NVMTLTexture *r = ((NVMTLTexture *)t)->_parent ?: (NVMTLTexture *)t;
    return [r->_sparse isKindOfClass:[NVMTLSparseTex class]] ? (NVMTLSparseTex *)r->_sparse : nil;
}

NSInteger nvmtl_sparse_texture_tier(id t) { return nvmtl_sp_state(t) ? 1 /* MTLTextureSparseTier1 */ : 0; }
NSUInteger nvmtl_sparse_heap_used(id heap)
{
    NVMTLSparsePool *p = [((NVMTLHeap *)heap)->_sparse isKindOfClass:[NVMTLSparsePool class]] ? ((NVMTLHeap *)heap)->_sparse : nil;
    if (!p) return ((NVMTLHeap *)heap)->_used;
    os_unfair_lock_lock(&p->_lk); const NSUInteger used = (NSUInteger)(p->_pages - p->_nfree) * p->_tileBytes; os_unfair_lock_unlock(&p->_lk);
    return used;
}

int nvmtl_sparse_heap_init(id heap_, MTLHeapDescriptor *d)
{
    if (d.type != MTLHeapTypeSparse || !nvmtl_sparse_knob()) return 0;
    if (!nvmtl_vk_sparse_tex()) { static int said; if (!said++) nvlog("sparse heap: NVMTL_SPARSE_TEXTURES=1 but the device has no image residency - a full heap as before"); return 0; }
    NVMTLHeap *heap = (NVMTLHeap *)heap_;
    const uint32_t tb = nvmtl_vk_sparse_tile_bytes();
    const size_t pages = d.size / tb;
    if (!pages || pages > UINT32_MAX - 1) { nvlog("sparse heap: %lu bytes is not one tile of %u", (unsigned long)d.size, tb); return -1; }
    if (@available(macOS 13.0, *)) {
        const NSInteger ps = d.sparsePageSize;   // MTLSparsePageSize16/64/256 = 101/102/103
        const uint32_t want = ps == MTLSparsePageSize16 ? 16384u : ps == MTLSparsePageSize64 ? 65536u : ps == MTLSparsePageSize256 ? 262144u : 0u;
        if (want && want != tb) {
            static int said; if (said++ < 4) nvlog("sparse heap: asked for sparse page size %ld; tiles here are %u bytes (the Vulkan sparse block) - "
                                                   "sparseTileSizeWithTextureType answers for those", (long)d.sparsePageSize, tb);
        }
    }
    NVMTLSparsePool *p = [NVMTLSparsePool new];
    p->_tileBytes = tb; p->_pages = (uint32_t)pages; p->_bytes = pages * tb; p->_lk = OS_UNFAIR_LOCK_INIT;
    p->_free = malloc(sizeof(uint32_t) * pages);
    if (!p->_free || nvmtl_vk_sparse_pool_create(p->_bytes, &p->_mem)) { nvlog("sparse heap: %zu tiles FAILED", pages); return -1; }
    for (uint32_t i = 0; i < p->_pages; i++) p->_free[i] = p->_pages - 1 - i;   // page 0 is taken first
    p->_nfree = p->_pages;
    heap->_sparse = p;
    nvlog("sparse heap: %lu bytes -> %u tiles of %u bytes -> %p", (unsigned long)d.size, p->_pages, tb, (__bridge void *)heap);
    return 1;
}

// The page table of a new sparse texture (dimensions are Vulkan's: layers 1 for 3D, depth in _d).
static int nvmtl_sp_table(NVMTLSparseTex *st)
{
    const nvmtl_sparse_layout *l = &st->_lay;
    uint32_t n = 0;
    for (uint32_t m = 0; m < l->tail_first && m < 32; m++) {
        const uint32_t w = MAX(1u, st->_w >> m), h = MAX(1u, st->_h >> m), d = MAX(1u, st->_d >> m);
        st->_tx[m] = (w + l->tw - 1) / l->tw; st->_ty[m] = (h + l->th - 1) / l->th; st->_tz[m] = (d + l->td - 1) / l->td;
        st->_mipBase[m] = n; n += st->_tx[m] * st->_ty[m] * st->_tz[m];
    }
    st->_perLayer = n;
    st->_tailPages = l->tail_first < st->_mips ? (uint32_t)(l->tail_size / l->tile_bytes) : 0;
    st->_tailBase = st->_perLayer * st->_layers;
    const uint64_t total = (uint64_t)st->_tailBase + (uint64_t)st->_tailPages * (l->single_tail ? 1 : st->_layers);
    if (total > (64u << 20)) return -1;
    st->_n = (uint32_t)total;
    st->_page = malloc(sizeof(uint32_t) * (st->_n ? st->_n : 1));
    if (!st->_page) return -1;
    for (uint32_t i = 0; i < st->_n; i++) st->_page[i] = NVMTL_SP_NONE;
    return 0;
}

extern void nvmtl_texture_desc_swizzle(NVMTLTexture *t, MTLTextureDescriptor *d);
id nvmtl_sparse_new_texture(id heap_, MTLTextureDescriptor *d)
{
    NVMTLHeap *heap = (NVMTLHeap *)heap_;
    NVMTLSparsePool *pool = heap && [heap->_sparse isKindOfClass:[NVMTLSparsePool class]] ? heap->_sparse : nil;
    if (heap && !pool) return nil;
    if (!heap) {
        // a placement sparse texture: Metal 4 heap offsets count tiles of its page size, which must be the Vulkan tile
        if (@available(macOS 26.0, *)) {
            const NSInteger ps = d.placementSparsePageSize;
            const uint32_t want = ps == MTLSparsePageSize16 ? 16384u : ps == MTLSparsePageSize64 ? 65536u : ps == MTLSparsePageSize256 ? 262144u : 0u;
            if (want != nvmtl_vk_sparse_tile_bytes()) {
                nvlog("sparse texture: placementSparsePageSize %ld is %u bytes; tiles here are %u bytes - REFUSED (nil)", (long)ps, want, nvmtl_vk_sparse_tile_bytes());
                return nil; }
        } else return nil;
    }
    uint32_t vkf = 0, bpp = 0; int a8 = 0;
    if (nvmtl_pixfmt_public(d.pixelFormat, &vkf, &bpp, &a8)) {
        nvlog("sparse texture: pixel format %lu cannot be sparse here (colour formats only) -> nil", (unsigned long)d.pixelFormat); return nil; }
    const uint32_t type = (uint32_t)d.textureType;
    const uint32_t layers = nvmtl_vk_layers_for_desc(type, (uint32_t)d.arrayLength, (uint32_t)d.depth);
    const uint32_t mips = (uint32_t)(d.mipmapLevelCount ? d.mipmapLevelCount : 1);
    NVMTLSparseTex *st = [NVMTLSparseTex new];
    st->_lk = OS_UNFAIR_LOCK_INIT;
    NVMTLTexture *t = [NVMTLTexture new];
    if (nvmtl_vk_image_create_sparse((uint32_t)d.width, (uint32_t)d.height, vkf, bpp, a8, mips, (d.usage & MTLTextureUsageShaderWrite) != 0,
                                     type, layers, &t->_i, &st->_lay)) return nil;
    st->_pool = pool; st->_img = t->_i.img; st->_type = type; st->_mips = mips;
    st->_w = (uint32_t)d.width; st->_h = (uint32_t)d.height; st->_d = type == 7 ? (uint32_t)d.depth : 1; st->_layers = type == 7 ? 1 : layers;
    if (st->_lay.tail_first > 32 || nvmtl_sp_table(st)) { nvlog("sparse texture: page table too large -> nil"); nvmtl_vk_image_destroy(&t->_i); return nil; }
    t->_ropt = (MTLResourceOptions)d.cpuCacheMode | ((MTLResourceOptions)d.hazardTrackingMode << MTLResourceHazardTrackingModeShift)
             | ((MTLResourceOptions)MTLStorageModePrivate << MTLResourceStorageModeShift);
    t->_fmt = d.pixelFormat; t->_q = nvmtl_device_queue(); t->_stex = t->_i.storage == 1; t->_usage = d.usage;
    nvmtl_texture_desc_swizzle(t, d);
    t->_sparse = st;
    if (heap) { t->_subHeap = heap; t->_subCharge = 0; }
    nvlog("sparse texture: %lux%lux%lu type %u, %u mips, %u layers on %s -> %p", (unsigned long)d.width, (unsigned long)d.height,
          (unsigned long)d.depth, type, mips, layers, heap ? "a sparse heap" : "placement (Metal 4 mappings)", (__bridge void *)t);
    return t;
}

// ---- mapping -----------------------------------------------------------------------------------------------------------
// One mapping update of `region` (in tiles) of level `mip`, slice `slice`. page0 != NVMTL_SP_NONE: Metal 4's explicit
// pages (page0, page0 + 1, ... in X, Y, Z order) of memory `mem`; otherwise pages from the texture's pool. Lock held.
// Returns the binds recorded, or -1 when the pool ran dry (the tiles before that are mapped).
static int nvmtl_sp_update(NVMTLSparseTex *st, nvmtl_sparse_batch *b, BOOL map, MTLRegion region, NSUInteger mip, NSUInteger slice,
                           uint32_t page0, void *mem)
{
    const nvmtl_sparse_layout *l = &st->_lay;
    const uint32_t layer = st->_type == 7 ? 0 : (uint32_t)slice;
    if (mip >= st->_mips || layer >= st->_layers) return 0;
    if (map && page0 == NVMTL_SP_NONE && !st->_pool) return 0;   // a placement sparse texture names its pages (Metal 4)
    void *pmem = st->_pool ? st->_pool->_mem : mem;
    uint32_t k = 0; int made = 0;
    if (mip >= l->tail_first) {
        // the mip tail: Metal addresses its pages as a row at the first tail level (origin.y 0, height 1); a deeper level
        // names the whole tail
        if (!st->_tailPages) return 0;
        const uint32_t t = l->single_tail ? 0 : layer;
        uint32_t j0 = 0, j1 = st->_tailPages;
        if (mip == l->tail_first) { j0 = (uint32_t)MIN(region.origin.x, (NSUInteger)st->_tailPages); j1 = (uint32_t)MIN(region.origin.x + region.size.width, (NSUInteger)st->_tailPages); }
        for (uint32_t j = j0; j < j1; j++, k++) {
            uint32_t *e = &st->_page[st->_tailBase + t * st->_tailPages + j];
            const uint64_t roff = l->tail_offset + (l->single_tail ? 0 : (uint64_t)layer * l->tail_stride) + (uint64_t)j * l->tile_bytes;
            if (map) {
                uint32_t pg = page0 != NVMTL_SP_NONE ? page0 + k : *e;
                if (pg == NVMTL_SP_NONE && (pg = nvmtl_sp_take(st->_pool)) == NVMTL_SP_NONE) return -1;
                if (pg == *e && page0 == NVMTL_SP_NONE) continue;   // mapped already
                if (st->_pool && *e != NVMTL_SP_NONE && *e != pg) nvmtl_sp_give(st->_pool, *e);
                *e = pg;
                nvmtl_sparse_batch_opaque(b, st->_img, roff, l->tile_bytes, pmem, (uint64_t)pg * l->tile_bytes); made++;
            } else if (*e != NVMTL_SP_NONE) {
                if (st->_pool) nvmtl_sp_give(st->_pool, *e);
                *e = NVMTL_SP_NONE;
                nvmtl_sparse_batch_opaque(b, st->_img, roff, l->tile_bytes, NULL, 0); made++;
            }
        }
        return made;
    }
    const uint32_t mw = MAX(1u, st->_w >> mip), mh = MAX(1u, st->_h >> mip), md = MAX(1u, st->_d >> mip);
    const uint32_t x1 = (uint32_t)MIN(region.origin.x + region.size.width, (NSUInteger)st->_tx[mip]);
    const uint32_t y1 = (uint32_t)MIN(region.origin.y + region.size.height, (NSUInteger)st->_ty[mip]);
    const uint32_t z1 = (uint32_t)MIN(region.origin.z + MAX(region.size.depth, (NSUInteger)1), (NSUInteger)st->_tz[mip]);
    for (uint32_t z = (uint32_t)region.origin.z; z < z1; z++)
        for (uint32_t y = (uint32_t)region.origin.y; y < y1; y++)
            for (uint32_t x = (uint32_t)region.origin.x; x < x1; x++, k++) {
                uint32_t *e = &st->_page[layer * st->_perLayer + st->_mipBase[mip] + (z * st->_ty[mip] + y) * st->_tx[mip] + x];
                // Vulkan: an extent that is not whole tiles must reach the edge of the subresource (edge tiles)
                const uint32_t px = x * l->tw, py = y * l->th, pz = z * l->td;
                const uint32_t ew = MIN(l->tw, mw - px), eh = MIN(l->th, mh - py), ed = MIN(l->td, md - pz);
                if (map) {
                    uint32_t pg = page0 != NVMTL_SP_NONE ? page0 + k : *e;
                    if (pg == NVMTL_SP_NONE && (pg = nvmtl_sp_take(st->_pool)) == NVMTL_SP_NONE) return -1;
                    if (pg == *e && page0 == NVMTL_SP_NONE) continue;
                    if (st->_pool && *e != NVMTL_SP_NONE && *e != pg) nvmtl_sp_give(st->_pool, *e);
                    *e = pg;
                    nvmtl_sparse_batch_tile(b, st->_img, (uint32_t)mip, layer, px, py, pz, ew, eh, ed, pmem, (uint64_t)pg * l->tile_bytes); made++;
                } else if (*e != NVMTL_SP_NONE) {
                    if (st->_pool) nvmtl_sp_give(st->_pool, *e);
                    *e = NVMTL_SP_NONE;
                    nvmtl_sparse_batch_tile(b, st->_img, (uint32_t)mip, layer, px, py, pz, ew, eh, ed, NULL, 0); made++;
                }
            }
    return made;
}

// The entry of tile (x, y, z) of `mip`/`slice` in the page table, NULL outside the texture (tail levels: x is the
// tail page, as nvmtl_sp_update reads it)
static uint32_t *nvmtl_sp_entry(NVMTLSparseTex *st, NSUInteger mip, NSUInteger slice, uint32_t x, uint32_t y, uint32_t z)
{
    const uint32_t layer = st->_type == 7 ? 0 : (uint32_t)slice;
    if (mip >= st->_mips || layer >= st->_layers) return NULL;
    if (mip >= st->_lay.tail_first) return x < st->_tailPages && !y && !z ? &st->_page[st->_tailBase + (st->_lay.single_tail ? 0 : layer) * st->_tailPages + x] : NULL;
    if (x >= st->_tx[mip] || y >= st->_ty[mip] || z >= st->_tz[mip]) return NULL;
    return &st->_page[layer * st->_perLayer + st->_mipBase[mip] + (z * st->_ty[mip] + y) * st->_tx[mip] + x];
}

// ---- the resource state encoder ------------------------------------------------------------------------------------------
@interface NVMTLResourceStateEncoder : NSObject <MTLResourceStateCommandEncoder> { @public NVMTLCommandBuffer *_cb; NSString *_label; BOOL _dry, _m4; } @end

id nvmtl_sparse_encoder(id cb_)
{
    NVMTLCommandBuffer *cb = (NVMTLCommandBuffer *)cb_;
    if (!nvmtl_vk_sparse_tex() || !cb || cb->_committed) return nil;
    nvmtl_g79_flush(cb);
    // the mappings must land between the encoders before this one and the ones after it: what is recorded so far
    // becomes a submission of its own, and the binds ride the recording that follows (nvmtl_vk_submit_begin_at)
    if (cb->_nEnc > cb->_nEncSealed && cb->_c.cb) [cb nvmtlSealPartWithSignal:nil value:0 restart:YES];
    NVMTLResourceStateEncoder *e = [NVMTLResourceStateEncoder new];
    e->_cb = cb; cb->_nEnc++;   // counted, so the part carrying the binds is never retired as empty
    return e;
}

@implementation NVMTLResourceStateEncoder
- (id<MTLDevice>)device { return (id<MTLDevice>)gNVMTLMainDevice; }
- (NSString *)label { return _label; }
- (void)setLabel:(NSString *)l { _label = [l copy]; }
- (void)endEncoding { if (_cb) { _cb->_nEnd++; _cb = nil; } }
- (void)insertDebugSignpost:(NSString *)s {}
- (void)pushDebugGroup:(NSString *)g { nvtrace("GROUP %s", g.UTF8String ?: "?"); }
- (void)popDebugGroup {}
// binds are ordered against every earlier and later submission of the device's one queue, so fences add nothing
- (void)updateFence:(id<MTLFence>)f {}
- (void)waitForFence:(id<MTLFence>)f {}
- (void)barrierAfterQueueStages:(MTLStages)after beforeStages:(MTLStages)before API_AVAILABLE(macos(26.0)) {}
- (nvmtl_sparse_batch *)nvmtlBatchFor:(id<MTLTexture>)t state:(NVMTLSparseTex **)st {
    if (!_cb) { nvlog("resource state encoder: used after endEncoding - ignored"); return NULL; }
    *st = nvmtl_sp_state(t);
    if (!*st) { static int said; if (said++ < 4) nvlog("resource state encoder: %p is not a sparse texture - mapping ignored", (__bridge void *)t); return NULL; }
    if (!(*st)->_pool && !_m4) {
        static int said; if (said++ < 4) nvlog("resource state encoder: %p is a placement sparse texture, mapped by MTL4CommandQueue "
                                               "updateTextureMappings:heap: only - ignored", (__bridge void *)t);
        return NULL; }
    nvmtl_retain_resource(_cb, t);   // alive (and its image) until the command buffer completes
    nvmtl_sparse_batch *b = nvmtl_sparse_batch_of(&_cb->_c);
    if (!b) nvlog("resource state encoder: out of memory - mapping ignored");
    return b;
}
- (void)nvmtlUpdate:(id<MTLTexture>)t mode:(MTLSparseTextureMappingMode)mode regions:(const MTLRegion *)r mipLevels:(const NSUInteger *)m
             slices:(const NSUInteger *)s count:(NSUInteger)n page0:(uint32_t)page0 heapMemory:(void *)mem {
    NVMTLSparseTex *st = nil; nvmtl_sparse_batch *b = [self nvmtlBatchFor:t state:&st];
    if (!b) return;
    os_unfair_lock_lock(nvmtl_sp_lock(st));
    for (NSUInteger i = 0; i < n; i++)
        if (nvmtl_sp_update(st, b, mode == MTLSparseTextureMappingModeMap, r[i], m[i], s[i], page0, mem) < 0) {
            if (!_dry) nvlog("resource state encoder: the sparse heap has no free tile - the rest of the mapping is not made (%u tiles)", st->_pool ? st->_pool->_pages : 0);
            _dry = YES; break;
        }
    os_unfair_lock_unlock(nvmtl_sp_lock(st));
}
- (void)updateTextureMappings:(id<MTLTexture>)t mode:(const MTLSparseTextureMappingMode)mode regions:(const MTLRegion[])r
                    mipLevels:(const NSUInteger[])m slices:(const NSUInteger[])s numRegions:(NSUInteger)n {
    if (!r || !m || !s) return;
    [self nvmtlUpdate:t mode:mode regions:r mipLevels:m slices:s count:n page0:NVMTL_SP_NONE heapMemory:NULL];
}
- (void)updateTextureMapping:(id<MTLTexture>)t mode:(const MTLSparseTextureMappingMode)mode region:(const MTLRegion)r
                    mipLevel:(const NSUInteger)m slice:(const NSUInteger)s {
    [self nvmtlUpdate:t mode:mode regions:&r mipLevels:&m slices:&s count:1 page0:NVMTL_SP_NONE heapMemory:NULL];
}
// The arguments are GPU-written (MTLMapIndirectBufferFormat: uint32 count, then MTLMapIndirectArguments): read when the
// binds run, after the work submitted before them has finished. A buffer the CPU cannot read (private) is refused.
typedef struct { CFTypeRef tex, buf; NSUInteger off; BOOL map; } nvmtl_sp_indirect;
static void nvmtl_sp_indirect_run(nvmtl_sparse_batch *b, void *ctx)
{
    nvmtl_sp_indirect *in = (nvmtl_sp_indirect *)ctx;
    NVMTLSparseTex *st = nvmtl_sp_state((__bridge id<MTLTexture>)in->tex);
    NVMTLBuffer *buf = (__bridge NVMTLBuffer *)in->buf;
    const uint8_t *p = (const uint8_t *)nvmtl_buf_cpu(buf);
    const size_t len = buf->_b.size;
    if (!st || !p || in->off + 4 > len) return;
    uint32_t n; memcpy(&n, p + in->off, 4);
    n = (uint32_t)MIN((size_t)n, (len - in->off - 4) / sizeof(MTLMapIndirectArguments));
    os_unfair_lock_lock(nvmtl_sp_lock(st));
    for (uint32_t i = 0; i < n; i++) {
        MTLMapIndirectArguments a; memcpy(&a, p + in->off + 4 + i * sizeof a, sizeof a);
        const MTLRegion r = MTLRegionMake3D(a.regionOriginX, a.regionOriginY, a.regionOriginZ, a.regionSizeWidth, a.regionSizeHeight, a.regionSizeDepth);
        if (nvmtl_sp_update(st, b, in->map, r, a.mipMapLevel, a.sliceId, NVMTL_SP_NONE, NULL) < 0) { nvlog("indirect mapping: the sparse heap has no free tile - the rest is not made"); break; }
    }
    os_unfair_lock_unlock(nvmtl_sp_lock(st));
}
static void nvmtl_sp_indirect_drop(void *ctx)
{
    nvmtl_sp_indirect *in = (nvmtl_sp_indirect *)ctx;
    if (in->tex) CFRelease(in->tex);
    if (in->buf) CFRelease(in->buf);
    free(in);
}
- (void)updateTextureMapping:(id<MTLTexture>)t mode:(const MTLSparseTextureMappingMode)mode indirectBuffer:(id<MTLBuffer>)ib
        indirectBufferOffset:(NSUInteger)off {
    NVMTLSparseTex *st = nil; nvmtl_sparse_batch *b = [self nvmtlBatchFor:t state:&st];
    if (!b) return;
    if (![(id)ib isKindOfClass:[NVMTLBuffer class]] || !nvmtl_buf_cpu((NVMTLBuffer *)ib)) {
        nvlog("updateTextureMapping:indirectBuffer: the arguments are in a buffer the CPU cannot read (private storage) - REFUSED, nothing mapped"); return; }
    nvmtl_retain_resource(_cb, ib);
    nvmtl_sp_indirect *in = calloc(1, sizeof *in);
    if (!in) return;
    in->tex = CFBridgingRetain(t); in->buf = CFBridgingRetain(ib); in->off = off; in->map = mode == MTLSparseTextureMappingModeMap;
    nvmtl_sparse_batch_defer(b, nvmtl_sp_indirect_run, in, nvmtl_sp_indirect_drop);
}
// Pages of the source region go to the destination's tiles that are unmapped (both on the same sparse heap; the
// header: matching format, type, sample count, usage and options, which the caller vouches for).
- (void)moveTextureMappingsFromTexture:(id<MTLTexture>)src sourceSlice:(NSUInteger)ss sourceLevel:(NSUInteger)sl sourceOrigin:(MTLOrigin)so
                            sourceSize:(MTLSize)sz toTexture:(id<MTLTexture>)dst destinationSlice:(NSUInteger)ds
                      destinationLevel:(NSUInteger)dl destinationOrigin:(MTLOrigin)dorg {
    NVMTLSparseTex *a = nil, *d = nil;
    nvmtl_sparse_batch *b = [self nvmtlBatchFor:src state:&a];
    if (!b || ![self nvmtlBatchFor:dst state:&d]) return;
    if (!a->_pool || a->_pool != d->_pool) { nvlog("moveTextureMappingsFromTexture: the textures are not on the same sparse heap - nothing moved"); return; }
    os_unfair_lock_lock(&a->_pool->_lk);
    for (uint32_t z = 0; z < MAX(sz.depth, (NSUInteger)1); z++)
        for (uint32_t y = 0; y < sz.height; y++)
            for (uint32_t x = 0; x < sz.width; x++) {
                uint32_t *se = nvmtl_sp_entry(a, sl, ss, (uint32_t)so.x + x, (uint32_t)so.y + y, (uint32_t)so.z + z);
                uint32_t *de = nvmtl_sp_entry(d, dl, ds, (uint32_t)dorg.x + x, (uint32_t)dorg.y + y, (uint32_t)dorg.z + z);
                if (!se || !de || *se == NVMTL_SP_NONE || *de != NVMTL_SP_NONE) continue;
                const uint32_t pg = *se;
                const MTLRegion one = MTLRegionMake3D((NSUInteger)so.x + x, (NSUInteger)so.y + y, (NSUInteger)so.z + z, 1, 1, 1);
                const MTLRegion done = MTLRegionMake3D((NSUInteger)dorg.x + x, (NSUInteger)dorg.y + y, (NSUInteger)dorg.z + z, 1, 1, 1);
                nvmtl_sp_update(a, b, NO, one, sl, ss, NVMTL_SP_NONE, NULL);   // gives pg back to the pool...
                nvmtl_sp_update(d, b, YES, done, dl, ds, pg, a->_pool->_mem);   // ...and the destination takes exactly pg
                // pg went onto the free stack and was named explicitly: take it off again
                for (uint32_t i = a->_pool->_nfree; i-- > 0;) if (a->_pool->_free[i] == pg) { a->_pool->_free[i] = a->_pool->_free[--a->_pool->_nfree]; break; }
            }
    os_unfair_lock_unlock(&a->_pool->_lk);
}
@end

// ---- Metal 4: queue-level mapping of placement sparse textures --------------------------------------------------------
// MTL4CommandQueue updateTextureMappings:heap:operations:count: and copyTextureMappingsFromTexture: run through a
// command buffer of the hidden Metal 3 queue (NVMTLMetal4.m), like the queue's signals and waits, so they order against
// commits. The heap is an MTLHeapTypePlacement heap; heapOffset counts tiles of the texture's placementSparsePageSize,
// which must be the Vulkan tile here.
API_AVAILABLE(macos(26.0))
BOOL nvmtl_sparse_m4_update(id<MTLCommandBuffer> c, id<MTLTexture> t, id<MTLHeap> h, const MTL4UpdateSparseTextureMappingOperation *o, NSUInteger n)
{
    NVMTLSparseTex *st = nvmtl_sp_state(t);
    if (!st || st->_pool) { nvlog("Metal 4: updateTextureMappings on %p, which is not a placement sparse texture - ignored", (__bridge void *)t); return NO; }
    void *mem = NULL; uint32_t memtype = 0; size_t heapBytes = 0;
    if ([(id)h isKindOfClass:[NVMTLHeap class]] && ((NVMTLHeap *)h)->_type == MTLHeapTypePlacement) {
        NVMTLHeap *ph = (NVMTLHeap *)h; mem = ph->_hm.mem; memtype = ph->_hm.memtype; heapBytes = ph->_hm.size; }
    BOOL maps = NO; for (NSUInteger i = 0; i < n; i++) maps |= o[i].mode == MTLSparseTextureMappingModeMap;
    if (maps && (!mem || !nvmtl_vk_sparse_memtype_ok(&st->_lay, memtype))) {
        nvlog("Metal 4: updateTextureMappings maps from %s - REFUSED", !mem ? "no placement heap" : "a heap whose memory type sparse images cannot bind"); return NO; }
    os_unfair_lock_lock(&st->_lk);
    if (maps && st->_mem && st->_mem != mem) { os_unfair_lock_unlock(&st->_lk); nvlog("Metal 4: a placement sparse texture maps tiles from one heap at a time here - REFUSED"); return NO; }
    if (maps) { st->_mem = mem; st->_memtype = memtype; }
    os_unfair_lock_unlock(&st->_lk);
    NVMTLResourceStateEncoder *e = (NVMTLResourceStateEncoder *)[c resourceStateCommandEncoder];
    if (!e) return NO;
    e->_m4 = YES;
    if (h) nvmtl_retain_resource(e->_cb, h);
    for (NSUInteger i = 0; i < n; i++) {
        const BOOL map = o[i].mode == MTLSparseTextureMappingModeMap;
        const MTLRegion r = o[i].textureRegion;
        const uint64_t tiles = (uint64_t)r.size.width * r.size.height * MAX(r.size.depth, (NSUInteger)1);
        if (map && ((uint64_t)o[i].heapOffset + tiles) * st->_lay.tile_bytes > heapBytes) {
            nvlog("Metal 4: updateTextureMappings: tiles %lu..%llu are past the heap's end - operation %lu skipped", (unsigned long)o[i].heapOffset,
                  (unsigned long long)(o[i].heapOffset + tiles), (unsigned long)i); continue; }
        const NSUInteger m = o[i].textureLevel, s = o[i].textureSlice;
        [e nvmtlUpdate:t mode:o[i].mode regions:&r mipLevels:&m slices:&s count:1 page0:map ? (uint32_t)o[i].heapOffset : NVMTL_SP_NONE heapMemory:mem];
    }
    [e endEncoding];
    return YES;
}
API_AVAILABLE(macos(26.0))
BOOL nvmtl_sparse_m4_copy(id<MTLCommandBuffer> c, id<MTLTexture> src, id<MTLTexture> dst, const MTL4CopySparseTextureMappingOperation *o, NSUInteger n)
{
    NVMTLSparseTex *a = nvmtl_sp_state(src), *d = nvmtl_sp_state(dst);
    if (!a || !d || a->_pool || d->_pool) { nvlog("Metal 4: copyTextureMappingsFromTexture needs two placement sparse textures - ignored"); return NO; }
    if (!a->_mem || (d->_mem && d->_mem != a->_mem)) { nvlog("Metal 4: copyTextureMappingsFromTexture: %s - ignored", !a->_mem ? "the source maps nothing" : "the textures map different heaps"); return NO; }
    if (!nvmtl_vk_sparse_memtype_ok(&d->_lay, a->_memtype)) { nvlog("Metal 4: copyTextureMappingsFromTexture: the destination cannot bind the source's heap memory - ignored"); return NO; }
    NVMTLResourceStateEncoder *e = (NVMTLResourceStateEncoder *)[c resourceStateCommandEncoder];
    if (!e) return NO;
    e->_m4 = YES;
    d->_mem = a->_mem; d->_memtype = a->_memtype;
    nvmtl_sparse_batch *b = [e nvmtlBatchFor:dst state:&d];
    nvmtl_retain_resource(e->_cb, src);
    if (b) {
        NVMTLSparseTex *first = (uintptr_t)a < (uintptr_t)d ? a : d, *second = first == a ? d : a;   // one lock order
        os_unfair_lock_lock(&first->_lk);
        if (a != d) os_unfair_lock_lock(&second->_lk);
        for (NSUInteger i = 0; i < n; i++) {
            const MTLRegion r = o[i].sourceRegion;
            for (uint32_t z = 0; z < MAX(r.size.depth, (NSUInteger)1); z++)
                for (uint32_t y = 0; y < r.size.height; y++)
                    for (uint32_t x = 0; x < r.size.width; x++) {
                        uint32_t *se = nvmtl_sp_entry(a, o[i].sourceLevel, o[i].sourceSlice, (uint32_t)r.origin.x + x, (uint32_t)r.origin.y + y, (uint32_t)r.origin.z + z);
                        if (!se) continue;
                        const MTLOrigin dor = o[i].destinationOrigin;
                        const MTLRegion one = MTLRegionMake3D((NSUInteger)dor.x + x, (NSUInteger)dor.y + y, (NSUInteger)dor.z + z, 1, 1, 1);
                        nvmtl_sp_update(d, b, *se != NVMTL_SP_NONE, one, o[i].destinationLevel, o[i].destinationSlice,
                                        *se != NVMTL_SP_NONE ? *se : NVMTL_SP_NONE, a->_mem);
                    }
        }
        if (a != d) os_unfair_lock_unlock(&second->_lk);
        os_unfair_lock_unlock(&first->_lk);
    }
    [e endEncoding];
    return YES;
}

// ---- texture and device answers ----------------------------------------------------------------------------------------
@implementation NVMTLTexture (NVMTLSparse)
// the first level whose texels share the tail's tiles (mipmapLevelCount when there is no tail); 0 for a texture that
// is not sparse, as before
- (NSUInteger)firstMipmapInTail { NVMTLSparseTex *st = nvmtl_sp_state(self); return st ? st->_lay.tail_first : 0; }
- (NSUInteger)tailSizeInBytes { NVMTLSparseTex *st = nvmtl_sp_state(self); return st && st->_tailPages ? (NSUInteger)st->_lay.tail_size : 0; }
@end

#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"   // MTLDevice methods _MTLDevice implements
@implementation NVMTLDevice (NVMTLSparse)
// 16384 from _MTLDevice before (and still, with the knob off); the Vulkan sparse block with it on
- (NSUInteger)sparseTileSizeInBytes {
    if (nvmtl_vk_sparse_tex()) return nvmtl_vk_sparse_tile_bytes();
    struct objc_super sup = { self, class_getSuperclass([NVMTLDevice class]) };
    return ((NSUInteger (*)(struct objc_super *, SEL))objc_msgSendSuper)(&sup, _cmd);
}
- (NSUInteger)sparseTileSizeInBytesForSparsePageSize:(MTLSparsePageSize)ps API_AVAILABLE(macos(13.0)) {
    if (nvmtl_vk_sparse_tex()) return nvmtl_vk_sparse_tile_bytes();
    struct objc_super sup = { self, class_getSuperclass([NVMTLDevice class]) };
    return ((NSUInteger (*)(struct objc_super *, SEL, MTLSparsePageSize))objc_msgSendSuper)(&sup, _cmd, ps);
}
@end
#pragma clang diagnostic pop

// sparseTileSizeWithTextureType:... (NVMTLDevice_contract.m): the Vulkan tile shape of the format, 0x0x0 when it cannot be
// sparse; ok 0 when sparse textures are off (the caller keeps its old answer)
MTLSize nvmtl_sparse_tile_size(NSUInteger type, NSUInteger pixelFormat, NSUInteger samples, int *ok)
{
    *ok = nvmtl_vk_sparse_tex();
    if (!*ok) return MTLSizeMake(0, 0, 0);
    uint32_t vkf = 0, bpp = 0, tw = 0, th = 0, td = 0; int a8 = 0;
    if (nvmtl_pixfmt_public((MTLPixelFormat)pixelFormat, &vkf, &bpp, &a8) || samples > 1 || type == MTLTextureType1D || type == MTLTextureType1DArray
        || type == MTLTextureTypeTextureBuffer || !nvmtl_vk_sparse_tile_shape(vkf, type == MTLTextureType3D, 1, &tw, &th, &td))
        return MTLSizeMake(0, 0, 0);
    return MTLSizeMake(tw, th, td);
}
