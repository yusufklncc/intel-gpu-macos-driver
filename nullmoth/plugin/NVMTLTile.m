// NVMTLTile.m - tile pipelines on a GPU without tile memory: render-pass splitting (docs/design/DESIGN-tile-shading.md).
//
// Imported into NVMTLDevice.m's translation unit after NVMTLLibraryLoad.m. On Apple GPUs a tile function runs between the
// draws of a render pass over the tile memory that holds the pass's colour attachments (implicit imageblocks) and any
// explicit imageblock; the Arc B580 renders straight to memory. Here:
//   - a tile pipeline is the tile function's AIR rewritten by the translator (metal2vulkan::tile_lower) into a kernel that
//     reads and writes one "plane" per colour attachment (a 2D-array storage texture, a layer per sample) at the tile
//     origin, and made into an ordinary compute pipeline;
//   - dispatchThreadsPerTile: ends the Vulkan render pass (its attachments are stored), copies each attachment the
//     function uses into its plane (a compute copy that reads the attachment as a sampled texture: sRGB decodes, MSAA
//     reads per sample), runs the kernel with one threadgroup per tile, and leaves the written planes to be copied back;
//   - the copy back is a full-screen draw per written attachment (per-sample shading for MSAA, sRGB encodes), recorded
//     when the pass resumes (the next render pipeline bound) or ends; the pass then begins again with every attachment
//     loaded, so later draws, store and resolve actions see what the tile function wrote. Consecutive tile dispatches with
//     no draw between them reuse the planes.
// The internal encoders bind through the command buffer's binding table: both stages' slots are saved before and given
// back after, so the app's own bindings stay what it set.

#import <objc/message.h>

Class gNVMTLTilePSClass;

// colour attachment pixel format -> 0 none, 1 float/normalized, 2 uint, 3 int
static uint8_t nvmtl_tile_class_of(MTLPixelFormat f)
{
    switch (f) {
        case MTLPixelFormatInvalid: return 0;
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatR32Uint:
        case MTLPixelFormatRG16Uint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint:
            return 2;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatR32Sint:
        case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA32Sint:
            return 3;
        default: return 1;
    }
}
// The plane format: 32-bit integers for integer attachments; half for 8-bit normalized ones (exact round trip), float
// otherwise.
static MTLPixelFormat nvmtl_tile_plane_format(MTLPixelFormat f)
{
    switch (nvmtl_tile_class_of(f)) {
        case 2: return MTLPixelFormatRGBA32Uint;
        case 3: return MTLPixelFormatRGBA32Sint;
        default: break;
    }
    switch (f) {
        case MTLPixelFormatA8Unorm: case MTLPixelFormatR8Unorm: case MTLPixelFormatR8Unorm_sRGB: case MTLPixelFormatR8Snorm:
        case MTLPixelFormatRG8Unorm: case MTLPixelFormatRG8Unorm_sRGB: case MTLPixelFormatRG8Snorm:
        case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatRGBA8Unorm_sRGB: case MTLPixelFormatRGBA8Snorm:
        case MTLPixelFormatBGRA8Unorm: case MTLPixelFormatBGRA8Unorm_sRGB:
        case MTLPixelFormatB5G6R5Unorm: case MTLPixelFormatA1BGR5Unorm: case MTLPixelFormatABGR4Unorm: case MTLPixelFormatBGR5A1Unorm:
            return MTLPixelFormatRGBA16Float;
        default: return MTLPixelFormatRGBA32Float;
    }
}

// ---- pipeline ------------------------------------------------------------------------------------------------------------
@interface NVMTLTileRenderPipelineState : NVMTLRenderPipelineState {
  @public NVMTLComputePipelineState *_kernel; MTLTileRenderPipelineDescriptor *_tdesc; NSString *_tlabel;
  uint32_t _reads, _writes, _samples, _explicit; uint8_t _classes[NVMTL_NCOL];
} @end
@implementation NVMTLTileRenderPipelineState
+ (void)load { gNVMTLTilePSClass = self; }
- (NSString *)label { return _tlabel; }
- (NSUInteger)maxTotalThreadsPerThreadgroup {
    const NSUInteger k = _kernel ? _kernel.maxTotalThreadsPerThreadgroup : 1024, d = _tdesc.maxTotalThreadsPerThreadgroup;
    return d && d < k ? d : k;
}
- (NSUInteger)threadExecutionWidth { return _kernel ? _kernel.threadExecutionWidth : 32; }
- (BOOL)threadgroupSizeMatchesTileSize { return _tdesc.threadgroupSizeMatchesTileSize; }
- (MTLSize)requiredThreadsPerTileThreadgroup { return nvmtl_desc_size(_tdesc, NSSelectorFromString(@"requiredThreadsPerThreadgroup")); }
- (BOOL)supportIndirectCommandBuffers { return NO; }
- (MTLResourceID)gpuResourceID { MTLResourceID r; r._impl = 0; return r; }
// The colour attachments' bytes per sample as Metal lays an implicit imageblock out (4 per 8-bit, 8 per 16-bit, 16 per
// 32-bit channel group, rounded to the plane): what the app sizes imageblockSampleLength by; nothing is allocated from it.
- (NSUInteger)imageblockSampleLength {
    NSUInteger n = 0;
    for (NSUInteger i = 0; i < NVMTL_NCOL; i++) {
        MTLPixelFormat f = _tdesc.colorAttachments[i].pixelFormat;
        if (f == MTLPixelFormatInvalid) continue;
        n += nvmtl_tile_plane_format(f) == MTLPixelFormatRGBA16Float ? 8 : 16;
    }
    return n;
}
- (NSUInteger)imageblockMemoryLengthForDimensions:(MTLSize)d { return self.imageblockSampleLength * d.width * d.height * (_samples ? _samples : 1); }
@end

static NSError *nvmtl_tile_err(NSString *what)
{
    return nvmtl_unsupported_err([@"tile render pipeline: " stringByAppendingString:what]);
}

id nvmtl_tile_pipeline(id dev, MTLTileRenderPipelineDescriptor *d, MTLPipelineOption o, MTLAutoreleasedRenderPipelineReflection *r, NSError **err)
{
    if (r) *r = nil;
    NVMTLFunction *f = (NVMTLFunction *)d.tileFunction;
    NSString *name = d.tileFunction.name ?: @"(none)";
#define TILE_FAIL(...) do { NSError *e_ = nvmtl_tile_err([NSString stringWithFormat:__VA_ARGS__]); if (err) *err = e_; return nil; } while (0)
    if (![f isKindOfClass:[NVMTLFunction class]] || !f->_air.length)
        TILE_FAIL(@"tile function \"%@\" is not a function of this device with AIR", name);
    MTLLinkedFunctions *lf = d.linkedFunctions;
    if (lf.functions.count || lf.binaryFunctions.count || lf.privateFunctions.count || lf.groups.count || d.preloadedLibraries.count)
        TILE_FAIL(@"tile function \"%@\": linking functions into a tile function is not supported by this driver", name);
    const NSUInteger samples = d.rasterSampleCount ? d.rasterSampleCount : 1;
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8)
        TILE_FAIL(@"tile function \"%@\": raster sample count %lu", name, (unsigned long)samples);
    if (!nvmtl_xlate_ready() || !xlate_lower_tile)
        TILE_FAIL(@"tile function \"%@\": this translator cannot lower tile functions (no nvmtl_lower_tile)", name);
    uint8_t classes[NVMTL_NCOL];
    for (NSUInteger i = 0; i < NVMTL_NCOL; i++) classes[i] = nvmtl_tile_class_of(d.colorAttachments[i].pixelFormat);
    uint8_t *lo = NULL; size_t ln = 0; uint32_t masks[3] = { 0, 0, 0 }; char e[1024] = { 0 };
    if (xlate_lower_tile(f->_air.UTF8String, f->_fname.UTF8String, (uint32_t)samples, classes, &lo, &ln, masks, e, sizeof e) || !lo)
        TILE_FAIL(@"tile function \"%@\" did not lower: %s", name, e[0] ? e : "no reason given");
    NSString *kll = nvmtl_mesh_text(lo, ln);
    if (!kll) TILE_FAIL(@"tile function \"%@\": lowered AIR is not UTF-8", name);
    if (getenv("NVMTL_TILE_DUMP")) {
        NSString *dir = @(getenv("NVMTL_TILE_DUMP"));
        [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:NULL];
        [kll writeToFile:[dir stringByAppendingFormat:@"/%@.tile.ll", f->_fname] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
    }
    NSString *why = nil;
    NSData *spv = nvmtl_mesh_translate_fc(kll, @"kernel", f->_fc, &why);
    if (!spv) TILE_FAIL(@"tile function \"%@\": the lowered kernel did not translate: %@", name, why ?: @"?");
    NVMTLFunction *kf = [NVMTLFunction new];
    kf->_spirv = spv; kf->_fname = f->_fname; kf->_stage = @"kernel"; kf->_air = kll; kf->_lib = f->_lib; kf->_fc = f->_fc;
    NSError *pe = nil;
    NVMTLComputePipelineState *cps = (NVMTLComputePipelineState *)[dev newComputePipelineStateWithFunction:kf error:&pe];
    if (!cps) TILE_FAIL(@"tile function \"%@\": no compute pipeline for the lowered kernel (%@)", name, pe.localizedDescription ?: @"?");
#undef TILE_FAIL
    NVMTLTileRenderPipelineState *ps = [NVMTLTileRenderPipelineState new];
    ps->_kernel = cps; ps->_tdesc = [d copy]; ps->_tlabel = [d.label copy];
    ps->_reads = masks[0]; ps->_writes = masks[1]; ps->_explicit = masks[2]; ps->_samples = (uint32_t)samples;
    memcpy(ps->_classes, classes, sizeof classes);
    if (r && (o & MTLPipelineOptionBindingInfo)) {
        NVMTLRenderPipelineReflection *rr = [NVMTLRenderPipelineReflection new];
        rr->_t = nvmtl_arguments_for_function(f);
        *r = (MTLAutoreleasedRenderPipelineReflection)rr;
    }
    nvlog("tile pipeline: \"%s\" -> %lu B kernel SPIR-V, %lu sample(s), attachments read 0x%x written 0x%x%s",
          f->_fname.UTF8String, (unsigned long)spv.length, (unsigned long)samples, masks[0], masks[1],
          masks[2] ? ", explicit imageblock (per-threadgroup memory: threads per tile must cover the tile)" : "");
    return ps;
}

// ---- helper shaders: attachment -> plane (compute), plane -> attachment (full-screen draw) --------------------------------
// The plane is texture 120 named nvt_plane, as in a lowered tile kernel: the translator then gives an integer plane its
// exact RGBA32 format (wrapper formatless_options).
static NSString *const kNVMTLTileHelpers =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "#define NVT_IN(N, T) \\\n"
     "kernel void nvt_in_##N(texture2d<T, access::read> s [[texture(0)]], texture2d_array<T, access::write> nvt_plane [[texture(120)]], uint2 p [[thread_position_in_grid]]) { \\\n"
     "    if (p.x < nvt_plane.get_width() && p.y < nvt_plane.get_height()) nvt_plane.write(s.read(p), p, 0); } \\\n"
     "kernel void nvt_in_##N##_ms(texture2d_ms<T, access::read> s [[texture(0)]], texture2d_array<T, access::write> nvt_plane [[texture(120)]], uint2 p [[thread_position_in_grid]]) { \\\n"
     "    if (p.x >= nvt_plane.get_width() || p.y >= nvt_plane.get_height()) return; \\\n"
     "    for (uint i = 0; i < s.get_num_samples(); i++) nvt_plane.write(s.read(p, i), p, i); }\n"
     "NVT_IN(f, float) NVT_IN(u, uint) NVT_IN(i, int)\n"
     "struct NvtV { float4 p [[position]]; };\n"
     "vertex NvtV nvt_vs(uint v [[vertex_id]]) { NvtV o; float2 u = float2((v << 1) & 2, v & 2); o.p = float4(u * 2.0 - 1.0, 0.0, 1.0); return o; }\n"
     "#define NVT_OUT(N, T) \\\n"
     "fragment vec<T, 4> nvt_out_##N(NvtV v [[stage_in]], texture2d_array<T, access::read> nvt_plane [[texture(120)]]) { return nvt_plane.read(uint2(v.p.xy), 0); } \\\n"
     "fragment vec<T, 4> nvt_out_##N##_ms(NvtV v [[stage_in]], uint k [[sample_id]], texture2d_array<T, access::read> nvt_plane [[texture(120)]]) { return nvt_plane.read(uint2(v.p.xy), k); }\n"
     "NVT_OUT(f, float) NVT_OUT(u, uint) NVT_OUT(i, int)\n";

static pthread_mutex_t g_tile_lock = PTHREAD_MUTEX_INITIALIZER;
static id<MTLLibrary> g_tile_lib; static BOOL g_tile_lib_tried;
static NSMutableDictionary *g_tile_pipes;   // "in_f" / "out_<format>_<samples>" -> pipeline
static id<MTLLibrary> nvmtl_tile_helpers_locked(void)
{
    if (!g_tile_lib_tried) {
        g_tile_lib_tried = YES;
        NSError *e = nil;
        g_tile_lib = [(id<MTLDevice>)gNVMTLMainDevice newLibraryWithSource:kNVMTLTileHelpers options:nil error:&e];
        if (!g_tile_lib) nvlog("tile pipelines: the attachment copy shaders did not build: %s - tile dispatches are dropped", e.description.UTF8String ?: "?");
    }
    return g_tile_lib;
}
static const char *nvmtl_tile_cls_letter(uint8_t c) { return c == 2 ? "u" : c == 3 ? "i" : "f"; }
static id<MTLComputePipelineState> nvmtl_tile_copy_in(uint8_t cls, BOOL ms)
{
    NSString *key = [NSString stringWithFormat:@"nvt_in_%s%s", nvmtl_tile_cls_letter(cls), ms ? "_ms" : ""];
    pthread_mutex_lock(&g_tile_lock);
    id ps = g_tile_pipes[key];
    if (!ps) {
        id<MTLLibrary> lib = nvmtl_tile_helpers_locked();
        id<MTLFunction> fn = [lib newFunctionWithName:key];
        NSError *e = nil;
        ps = fn ? [(id<MTLDevice>)gNVMTLMainDevice newComputePipelineStateWithFunction:fn error:&e] : nil;
        if (!ps) nvlog("tile pipelines: copy kernel %s: %s", key.UTF8String, e.description.UTF8String ?: "not built");
        else { if (!g_tile_pipes) g_tile_pipes = [NSMutableDictionary new]; g_tile_pipes[key] = ps; }
    }
    pthread_mutex_unlock(&g_tile_lock);
    return ps;
}
static id<MTLRenderPipelineState> nvmtl_tile_copy_out(MTLPixelFormat f, NSUInteger samples)
{
    NSString *key = [NSString stringWithFormat:@"out_%lu_%lu", (unsigned long)f, (unsigned long)samples];
    pthread_mutex_lock(&g_tile_lock);
    id ps = g_tile_pipes[key];
    if (!ps) {
        id<MTLLibrary> lib = nvmtl_tile_helpers_locked();
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.label = @"tile plane copy";
        d.vertexFunction = [lib newFunctionWithName:@"nvt_vs"];
        d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithFormat:@"nvt_out_%s%s", nvmtl_tile_cls_letter(nvmtl_tile_class_of(f)), samples > 1 ? "_ms" : ""]];
        d.colorAttachments[0].pixelFormat = f; d.rasterSampleCount = samples;
        NSError *e = nil;
        ps = d.vertexFunction && d.fragmentFunction ? [(id<MTLDevice>)gNVMTLMainDevice newRenderPipelineStateWithDescriptor:d error:&e] : nil;
        if (!ps) nvlog("tile pipelines: copy-back pipeline for format %lu x%lu: %s", (unsigned long)f, (unsigned long)samples, e.description.UTF8String ?: "not built");
        else { if (!g_tile_pipes) g_tile_pipes = [NSMutableDictionary new]; g_tile_pipes[key] = ps; }
    }
    pthread_mutex_unlock(&g_tile_lock);
    return ps;
}

// ---- planes: a small pool, lent to a command buffer until it completes ------------------------------------------------
static NSMutableArray *g_tile_pool;
static id<MTLTexture> nvmtl_tile_plane_take(MTLPixelFormat pf, NSUInteger w, NSUInteger h, NSUInteger layers)
{
    pthread_mutex_lock(&g_tile_lock);
    for (NSUInteger i = 0; i < g_tile_pool.count; i++) {
        id<MTLTexture> t = g_tile_pool[i];
        if (t.pixelFormat == pf && t.width == w && t.height == h && t.arrayLength == layers) {
            [g_tile_pool removeObjectAtIndex:i]; pthread_mutex_unlock(&g_tile_lock); return t; }
    }
    pthread_mutex_unlock(&g_tile_lock);
    MTLTextureDescriptor *td = [MTLTextureDescriptor new];
    td.textureType = MTLTextureType2DArray; td.pixelFormat = pf; td.width = w; td.height = h; td.arrayLength = layers;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> t = [(id<MTLDevice>)gNVMTLMainDevice newTextureWithDescriptor:td];
    if (t) t.label = @"tile plane";
    return t;
}
static void nvmtl_tile_plane_give(NSArray *planes)
{
    pthread_mutex_lock(&g_tile_lock);
    if (!g_tile_pool) g_tile_pool = [NSMutableArray new];
    [g_tile_pool addObjectsFromArray:planes];
    while (g_tile_pool.count > 16) [g_tile_pool removeObjectAtIndex:0];
    pthread_mutex_unlock(&g_tile_lock);
}

// ---- encoder state -----------------------------------------------------------------------------------------------------
@interface NVMTLTileState : NSObject { @public
  NVMTLTileRenderPipelineState *_ps;
  NSMutableDictionary<NSNumber *, NSArray *> *_bufs;      // index -> @[buffer, offset] or @[bytes]
  NSMutableDictionary<NSNumber *, id> *_texs;             // index -> texture
  NSMutableDictionary<NSNumber *, NSArray *> *_samps;     // index -> @[sampler] or @[sampler, lodMin, lodMax]
  NSMutableDictionary<NSNumber *, NSArray *> *_tables;    // index -> @[selector name, object] (AS, function tables)
  NSMutableDictionary<NSNumber *, NSNumber *> *_tg;       // threadgroup index -> length
  id<MTLTexture> _plane[NVMTL_NCOL];
  uint32_t _valid, _dirty; BOOL _settled;
  NSMutableArray *_lent;
} @end
@implementation NVMTLTileState
@end

@interface NVMTLRenderCommandEncoder (NVMTLTilePriv)
- (void)nvmtlCloseVisibility;
@end

static NVMTLTileState *nvmtl_tile_state(NVMTLRenderCommandEncoder *e)
{
    if (!e->_tile) e->_tile = [NVMTLTileState new];
    return e->_tile;
}
void nvmtl_tile_bind(NVMTLRenderCommandEncoder *e, id ps)
{
    nvmtl_tile_state(e)->_ps = (NVMTLTileRenderPipelineState *)ps;
    nvmtl_retain_resource(e->_cb, ps);
}

// The texture a copy reads: the attachment itself, or a 2D (multisample) view of the level and slice the pass renders.
static id<MTLTexture> nvmtl_tile_attachment_view(NVMTLRenderCommandEncoder *e, uint32_t a)
{
    NVMTLTexture *tx = e->_colTex[a];
    const NSUInteger level = e->_pass.col_level[a] - tx->_baseLevel, slice = e->_pass.col_layer[a] - tx->_baseSlice;
    const MTLTextureType want = tx.sampleCount > 1 ? MTLTextureType2DMultisample : MTLTextureType2D;
    if (tx.textureType == want && !level && !slice && tx.mipmapLevelCount == 1) return tx;
    return [tx newTextureViewWithPixelFormat:tx.pixelFormat textureType:want levels:NSMakeRange(level, 1) slices:NSMakeRange(slice, 1)];
}

void nvmtl_tile_flush(NVMTLRenderCommandEncoder *e)
{
    NVMTLTileState *t = e->_tile;
    if (!t) return;
    const uint32_t dirty = t->_dirty;
    t->_valid = 0; t->_dirty = 0;   // the pass resumes (or ends): the attachments are what counts from here on
    if (!dirty) return;
    NVMTLCommandBuffer *cb = e->_cb;
    void *saved = nvmtl_vk_bind_save(&cb->_c); void *dsv = cb->_c.depth_view;
    nvmtl_vk_cmd_barrier(&cb->_c);
    for (uint32_t a = 0; a < NVMTL_NCOL; a++) {
        if (!(dirty & (1u << a)) || !e->_colTex[a] || !t->_plane[a]) continue;
        NVMTLTexture *tx = e->_colTex[a];
        id<MTLRenderPipelineState> rp = nvmtl_tile_copy_out(tx.pixelFormat, tx.sampleCount);
        if (!rp) continue;
        MTLRenderPassDescriptor *d = [MTLRenderPassDescriptor renderPassDescriptor];
        d.colorAttachments[0].texture = tx;
        d.colorAttachments[0].level = e->_pass.col_level[a] - tx->_baseLevel;
        d.colorAttachments[0].slice = e->_pass.col_layer[a] - tx->_baseSlice;
        d.colorAttachments[0].loadAction = MTLLoadActionLoad; d.colorAttachments[0].storeAction = MTLStoreActionStore;
        NVMTLRenderCommandEncoder *re = [NVMTLRenderCommandEncoder new];
        re->_cb = cb; cb->_nEnc++;
        if (![re nvmtlBuildPass:d]) { nvlog("tile dispatch: colour %u: the copy back into the attachment has no pass - its tile writes are LOST", a); re->_refused = YES; [re endEncoding]; continue; }
        [re setRenderPipelineState:rp];
        [re setFragmentTexture:t->_plane[a] atIndex:120];
        [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [re endEncoding];
    }
    nvmtl_vk_cmd_barrier(&cb->_c);
    nvmtl_vk_bind_restore(&cb->_c, saved); cb->_c.depth_view = dsv;
}

void nvmtl_tile_end(NVMTLRenderCommandEncoder *e)
{
    NVMTLTileState *t = e->_tile;
    if (!t) return;
    if (!e->_begun) nvmtl_tile_flush(e);
    if (t->_lent.count) {
        NSArray *lent = [t->_lent copy];
        [(id<MTLCommandBuffer>)e->_cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; nvmtl_tile_plane_give(lent); }];
    }
    e->_tile = nil;
}

// The MTLRenderCommandEncoder protocol's tile methods, implemented in this category of the class that adopts it.
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wobjc-protocol-method-implementation"
@implementation NVMTLRenderCommandEncoder (NVMTLTile)
// ---- tile-stage bindings (kept on the encoder, bound on the tile kernel at each dispatch) ----
- (void)setTileBuffer:(id<MTLBuffer>)b offset:(NSUInteger)o atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_bufs) t->_bufs = [NSMutableDictionary new];
    if (b) { nvmtl_retain_resource(_cb, b); t->_bufs[@(i)] = @[b, @(o)]; } else [t->_bufs removeObjectForKey:@(i)];
}
- (void)setTileBufferOffset:(NSUInteger)o atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self); NSArray *v = t->_bufs[@(i)];
    if (v.count == 2) t->_bufs[@(i)] = @[v[0], @(o)];
}
- (void)setTileBytes:(const void *)p length:(NSUInteger)n atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_bufs) t->_bufs = [NSMutableDictionary new];
    if (p && n) t->_bufs[@(i)] = @[[NSData dataWithBytes:p length:n]];
}
- (void)setTileBuffers:(const id<MTLBuffer> __unsafe_unretained [])b offsets:(const NSUInteger *)o withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileBuffer:b[k] offset:o ? o[k] : 0 atIndex:r.location + k];
}
- (void)setTileTexture:(id<MTLTexture>)x atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_texs) t->_texs = [NSMutableDictionary new];
    if (x) { nvmtl_retain_resource(_cb, x); t->_texs[@(i)] = x; } else [t->_texs removeObjectForKey:@(i)];
}
- (void)setTileTextures:(const id<MTLTexture> __unsafe_unretained [])x withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileTexture:x[k] atIndex:r.location + k];
}
- (void)setTileSamplerState:(id<MTLSamplerState>)s atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_samps) t->_samps = [NSMutableDictionary new];
    if (s) { nvmtl_retain_resource(_cb, s); t->_samps[@(i)] = @[s]; } else [t->_samps removeObjectForKey:@(i)];
}
- (void)setTileSamplerState:(id<MTLSamplerState>)s lodMinClamp:(float)lo lodMaxClamp:(float)hi atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_samps) t->_samps = [NSMutableDictionary new];
    if (s) { nvmtl_retain_resource(_cb, s); t->_samps[@(i)] = @[s, @(lo), @(hi)]; } else [t->_samps removeObjectForKey:@(i)];
}
- (void)setTileSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileSamplerState:s[k] atIndex:r.location + k];
}
- (void)setTileSamplerStates:(const id<MTLSamplerState> __unsafe_unretained [])s lodMinClamps:(const float *)lo lodMaxClamps:(const float *)hi withRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileSamplerState:s[k] lodMinClamp:lo[k] lodMaxClamp:hi[k] atIndex:r.location + k];
}
- (void)nvmtlTileTable:(id)o selector:(NSString *)sel atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_tables) t->_tables = [NSMutableDictionary new];
    if (o) { nvmtl_retain_resource(_cb, o); t->_tables[@(i)] = @[sel, o]; } else [t->_tables removeObjectForKey:@(i)];
}
- (void)setTileAccelerationStructure:(id<MTLAccelerationStructure>)a atBufferIndex:(NSUInteger)i { [self nvmtlTileTable:a selector:@"setAccelerationStructure:atBufferIndex:" atIndex:i]; }
- (void)setTileVisibleFunctionTable:(id<MTLVisibleFunctionTable>)f atBufferIndex:(NSUInteger)i { [self nvmtlTileTable:f selector:@"setVisibleFunctionTable:atBufferIndex:" atIndex:i]; }
- (void)setTileIntersectionFunctionTable:(id<MTLIntersectionFunctionTable>)f atBufferIndex:(NSUInteger)i { [self nvmtlTileTable:f selector:@"setIntersectionFunctionTable:atBufferIndex:" atIndex:i]; }
- (void)setTileVisibleFunctionTables:(const id<MTLVisibleFunctionTable> __unsafe_unretained [])f withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileVisibleFunctionTable:f[k] atBufferIndex:r.location + k];
}
- (void)setTileIntersectionFunctionTables:(const id<MTLIntersectionFunctionTable> __unsafe_unretained [])f withBufferRange:(NSRange)r {
    for (NSUInteger k = 0; k < r.length; k++) [self setTileIntersectionFunctionTable:f[k] atBufferIndex:r.location + k];
}
// Threadgroup memory of a tile function. On Apple it is tile memory that lives for the whole pass; here it is the kernel's
// workgroup memory for one dispatch (an offset into it is not carried: each index is its own variable).
- (void)setThreadgroupMemoryLength:(NSUInteger)len offset:(NSUInteger)off atIndex:(NSUInteger)i {
    NVMTLTileState *t = nvmtl_tile_state(self);
    if (!t->_tg) t->_tg = [NSMutableDictionary new];
    if (off) { static int said; if (said++ < 4) nvlog("tile dispatch: threadgroup memory index %lu at offset %lu - the offset is not carried (each index is its own memory)", (unsigned long)i, (unsigned long)off); }
    if (len) t->_tg[@(i)] = @(len); else [t->_tg removeObjectForKey:@(i)];
}

// End the Vulkan render pass (or apply the clears of one no draw began) and mark every attachment loaded, so the pass
// that follows continues from the stored contents.
- (void)nvmtlTileSuspend:(NVMTLTileState *)t {
    if (_begun) {
        const BOOL vis = _visActive || _visPending;
        [self nvmtlCloseVisibility];
        nvmtl_vk_cmd_end_render(&_cb->_c);
        _begun = NO; _visPending = vis;
    } else if (!t->_settled) {
        switch (nvmtl_vk_pass_kind(&_pass)) {
            case 1: break;
            case 2: if (nvmtl_vk_cmd_empty_pass(&_cb->_c, &_pass)) nvlog("tile dispatch: the clears of the pass before it FAILED"); break;
            default: if (!(_load & 1u) && _target && nvmtl_vk_cmd_clear_image(&_cb->_c, [_target nvi], _clear)) nvlog("tile dispatch: the clear of the pass before it FAILED"); break;
        }
    }
    t->_settled = YES;
    uint32_t keep = 0;
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) if (_pass.col[i]) keep |= NVMTL_LOAD_COLOUR(i);
    if (_pass.ds) keep |= NVMTL_LOAD_DEPTH | NVMTL_LOAD_STENCIL;
    _pass.load |= keep; _load |= keep;
}

- (void)dispatchThreadsPerTile:(MTLSize)tpt {
    NVMTLTileState *t = _tile; NVMTLTileRenderPipelineState *tp = t ? t->_ps : nil;
    if (!tp) { static int said; if (said++ < 4) nvlog("dispatchThreadsPerTile: no tile pipeline is set - dropped"); return; }
    if (_refused || !tpt.width || !tpt.height || !tpt.depth) return;
    const uint32_t tw = _tileW ? _tileW : 16, th = _tileH ? _tileH : 16;
    const uint32_t used = tp->_reads | tp->_writes;
    if (tpt.width * tpt.height * tpt.depth > tp.maxTotalThreadsPerThreadgroup) {
        nvlog("dispatchThreadsPerTile: %lux%lux%lu threads per tile exceed the pipeline's %lu - dropped", (unsigned long)tpt.width,
              (unsigned long)tpt.height, (unsigned long)tpt.depth, (unsigned long)tp.maxTotalThreadsPerThreadgroup);
        return;
    }
    if (_pass.layers > 1) { static int said; if (said++ < 4) nvlog("dispatchThreadsPerTile: a layered render pass (renderTargetArrayLength %u) is not carried - dropped", _pass.layers); return; }
    for (uint32_t a = 0; a < NVMTL_NCOL; a++) {
        if (!(used & (1u << a))) continue;
        NVMTLTexture *tx = _colTex[a];
        if (!tx || !_pass.col[a]) { nvlog("dispatchThreadsPerTile: the tile function uses colour %u, which this pass does not have - dropped", a); return; }
        const NSUInteger s = tx.sampleCount ? tx.sampleCount : 1;
        if (s != (tp->_samples ? tp->_samples : 1) || [tx nvi]->mtl_type == 7 || nvmtl_tile_class_of(tx.pixelFormat) != tp->_classes[a]) {
            nvlog("dispatchThreadsPerTile: colour %u (%lu samples, format %lu) does not match the tile pipeline (%u samples) - dropped", a,
                  (unsigned long)s, (unsigned long)tx.pixelFormat, tp->_samples);
            return;
        }
    }
    [self nvmtlTileSuspend:t];
    NVMTLCommandBuffer *cb = _cb;
    void *saved = nvmtl_vk_bind_save(&cb->_c); void *dsv = cb->_c.depth_view;
    nvmtl_vk_cmd_barrier(&cb->_c);
    NVMTLComputeCommandEncoder *ce = [NVMTLComputeCommandEncoder new];
    ce->_cb = cb; cb->_nEnc++;
    BOOL ok = YES;
    // planes: the attachment's current contents, unless the last tile dispatch left them there (no draw since)
    for (uint32_t a = 0; a < NVMTL_NCOL && ok; a++) {
        if (!(used & (1u << a))) continue;
        NVMTLTexture *tx = _colTex[a];
        const NSUInteger s = tx.sampleCount ? tx.sampleCount : 1;
        const MTLPixelFormat pf = nvmtl_tile_plane_format(tx.pixelFormat);
        id<MTLTexture> pl = t->_plane[a];
        if (!pl || pl.pixelFormat != pf || pl.width != _pass.colw[a] || pl.height != _pass.colh[a] || pl.arrayLength != s) {
            pl = nvmtl_tile_plane_take(pf, _pass.colw[a], _pass.colh[a], s);
            if (!pl) { nvlog("dispatchThreadsPerTile: no %ux%u plane for colour %u - dropped", _pass.colw[a], _pass.colh[a], a); ok = NO; break; }
            if (!t->_lent) t->_lent = [NSMutableArray new];
            [t->_lent addObject:pl]; t->_plane[a] = pl; t->_valid &= ~(1u << a);
        }
        if (t->_valid & (1u << a)) continue;
        id<MTLComputePipelineState> cin = nvmtl_tile_copy_in(tp->_classes[a], s > 1);
        id<MTLTexture> src = nvmtl_tile_attachment_view(self, a);
        if (!cin || !src) { ok = NO; break; }
        [ce setComputePipelineState:cin];
        [ce setTexture:src atIndex:0];
        [ce setTexture:pl atIndex:120];
        [ce dispatchThreads:MTLSizeMake(_pass.colw[a], _pass.colh[a], 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        nvmtl_vk_cmd_barrier(&cb->_c);
        t->_valid |= 1u << a;
    }
    if (ok) {
        [ce setComputePipelineState:tp->_kernel];
        for (NSNumber *k in t->_bufs) {
            NSArray *v = t->_bufs[k];
            if ([v[0] isKindOfClass:[NSData class]]) [ce setBytes:((NSData *)v[0]).bytes length:((NSData *)v[0]).length atIndex:k.unsignedIntegerValue];
            else [ce setBuffer:v[0] offset:[v[1] unsignedIntegerValue] atIndex:k.unsignedIntegerValue];
        }
        for (NSNumber *k in t->_texs) [ce setTexture:t->_texs[k] atIndex:k.unsignedIntegerValue];
        for (NSNumber *k in t->_samps) {
            NSArray *v = t->_samps[k];
            if (v.count == 3) [ce setSamplerState:v[0] lodMinClamp:[v[1] floatValue] lodMaxClamp:[v[2] floatValue] atIndex:k.unsignedIntegerValue];
            else [ce setSamplerState:v[0] atIndex:k.unsignedIntegerValue];
        }
        for (NSNumber *k in t->_tables) {
            NSArray *v = t->_tables[k]; SEL sel = NSSelectorFromString(v[0]);
            if ([ce respondsToSelector:sel]) ((void (*)(id, SEL, id, NSUInteger))objc_msgSend)(ce, sel, v[1], k.unsignedIntegerValue);
            else { static int said; if (said++ < 4) nvlog("dispatchThreadsPerTile: %s is not carried to the tile kernel", [v[0] UTF8String]); }
        }
        for (NSNumber *k in t->_tg) [ce setThreadgroupMemoryLength:t->_tg[k].unsignedIntegerValue atIndex:k.unsignedIntegerValue];
        for (uint32_t a = 0; a < NVMTL_NCOL; a++) if (used & (1u << a)) [ce setTexture:t->_plane[a] atIndex:120 + a];
        const uint32_t prm[4] = { tw, th, _pass.w, _pass.h };
        [ce setBytes:prm length:sizeof prm atIndex:30];
        [ce dispatchThreadgroups:MTLSizeMake((_pass.w + tw - 1) / tw, (_pass.h + th - 1) / th, 1) threadsPerThreadgroup:tpt];
        t->_dirty |= tp->_writes;
    }
    [ce endEncoding];
    nvmtl_vk_cmd_barrier(&cb->_c);
    // attachments only read go back to the layout the resumed pass begins them in; written ones are drawn into on flush
    for (uint32_t a = 0; a < NVMTL_NCOL; a++)
        if ((used & (1u << a)) && !(t->_dirty & (1u << a)) && _pass.col[a]) nvmtl_vk_cmd_image_rest(&cb->_c, _pass.col[a]);
    nvmtl_vk_bind_restore(&cb->_c, saved); cb->_c.depth_view = dsv;
    static _Atomic unsigned long n; const unsigned long k = atomic_fetch_add(&n, 1) + 1;
    if (!(k & (k - 1))) nvlog("dispatchThreadsPerTile #%lu: \"%s\" %ux%u tiles of %ux%u, %lux%lu threads, planes read 0x%x written 0x%x%s",
                              k, tp->_kernel->_function ? tp->_kernel->_function->_fname.UTF8String : "?", (_pass.w + tw - 1) / tw,
                              (_pass.h + th - 1) / th, tw, th, (unsigned long)tpt.width, (unsigned long)tpt.height, tp->_reads, tp->_writes,
                              ok ? "" : " - NOT run");
}
@end
#pragma clang diagnostic pop
