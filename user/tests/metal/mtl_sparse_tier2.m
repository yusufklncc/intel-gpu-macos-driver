// SPDX-License-Identifier: MIT
// mtl_sparse_tier2: Metal sparse textures, tier 2 (per-tile access counters), on the Arc B580
// (NVMTL_SPARSE_TEXTURES=1 NVMTL_SPARSE_TIER2=1; nullmoth/plugin/NVMTLSparse.m, translator src/sparse_counters.rs).
//
// Tier 2 adds one thing to tier 1 (Metal headers, MTLTextureSparseTier2: "Obtain per-tile activity counters"): the blit
// encoder's getTextureAccessCounters:region:mipLevel:slice:resetCounters:countersBuffer:countersBufferOffset: copies the
// counters of a region of tiles (one uint32 per tile, x fastest, then y) into a buffer, and
// resetTextureAccessCounters:region:mipLevel:slice: clears them. A counter is nonzero once a shader read the tile.
// Values are not specified beyond that, so the checks here are "touched > 0, untouched == 0".
//
// A sparse heap and a mipmapped RGBA8 sparse texture of 4x4 tiles (and a 2-slice array texture), tiles (0,0), (1,0)
// and (3,3) of level 0 mapped, then:
//   1. after a reset of every counter, nothing reads: level 0 and 1 counters are all zero;
//   2. a compute kernel reads (sparse_read) tile (0,0) [mapped] and tile (2,1) [unmapped] of level 0 and tile (1,1) of
//      level 1: exactly those counters are nonzero (a read of an unmapped tile counts: it is what a streaming app
//      maps next); the counters land at countersBufferOffset, the bytes before it are untouched;
//   3. a sub-region (1..2, 0..2) of level 0 comes back as 2x3 counters, row by row;
//   4. resetTextureAccessCounters of tile (0,0) only: (0,0) reads zero, (2,1) keeps its count;
//   5. getTextureAccessCounters with resetCounters:YES returns the counts, and the next read is zero;
//   6. a compute sparse_sample at level(1) counts on level 1, not level 0;
//   7. a fragment shader's sparse_sample over a 512x512 target (implicit LOD = level 0) touches every level-0 tile and
//      no level-1 tile; over a 256x256 target the same draw counts level 1;
//   8. the array texture: a read of slice 1 tile (1,2) counts on slice 1 only;
//   9. a plain sample() (no residency read) counts as well (an M1 Pro counts it): tile (3,0) only.
// Each check prints PASS or FAIL; the last line counts them.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1 NVMTL_SPARSE_TIER2=1 ./mtl_sparse_tier2   (B580)
//   ./mtl_sparse_tier2 --any                the first Metal device when no B580 is present (reference run on a Mac)
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 3 GPU error, 4 wrong results, 5 tier 2 not offered (SKIP:
// the texture answers Tier1, or the blit encoder has no access counters).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // the counters are deprecated since macOS 26.4

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     // one point per threadgroup: p[i] = (x, y, level, slice) in texels of that level; 64 threads read it
     "kernel void reads(texture2d<float> t [[texture(0)]], device const uint4 *p [[buffer(0)]], device uint *o [[buffer(1)]],\n"
     "                  uint g [[threadgroup_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {\n"
     "    uint4 q = p[g];\n"
     "    sparse_color<float4> c = t.sparse_read(q.xy + uint2(l & 7, l >> 3), q.z);\n"
     "    if (l == 0) o[g] = c.resident() ? 1 : 0;\n"
     "}\n"
     "kernel void reads_array(texture2d_array<float> t [[texture(0)]], device const uint4 *p [[buffer(0)]], device uint *o [[buffer(1)]],\n"
     "                  uint g [[threadgroup_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {\n"
     "    uint4 q = p[g];\n"
     "    sparse_color<float4> c = t.sparse_read(q.xy + uint2(l & 7, l >> 3), q.w, q.z);\n"
     "    if (l == 0) o[g] = c.resident() ? 1 : 0;\n"
     "}\n"
     // a normalized sample at an explicit level
     "kernel void samples(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device const float4 *p [[buffer(0)]],\n"
     "                    device uint *o [[buffer(1)]], uint g [[threadgroup_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {\n"
     "    float4 q = p[g];\n"
     "    sparse_color<float4> c = t.sparse_sample(s, q.xy, level(q.z));\n"
     "    if (l == 0) o[g] = c.resident() ? 1 : 0;\n"
     "}\n"
     "kernel void plain(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device float4 *o [[buffer(1)]],\n"
     "                  uint g [[thread_position_in_grid]]) {\n"
     "    o[g] = t.sample(s, float2(0.9, 0.1), level(0));\n"
     "}\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint vid [[vertex_id]]) { float2 q = float2((vid << 1) & 2, vid & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
     // the target's pixel centres map onto the whole texture: a 512x512 target samples level 0 of a 512x512 texture
     "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float2 &rt [[buffer(0)]]) {\n"
     "    sparse_color<float4> c = t.sparse_sample(s, in.p.xy / rt);\n"
     "    return c.resident() ? c.value() : float4(1, 0, 1, 1);\n"
     "}\n";

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0;
    IORegistryEntryGetRegistryEntryID(s, &id);
    IOObjectRelease(s);
    return id;
}

enum { TX = 4, TY = 4 };
static int npass, nfail;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
static int finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "FAIL: %s: command buffer status %ld: %s\n", what, (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return 3;
    }
    return 0;
}
// "n0 n1 n2 ..." of `n` counters, for the log
static const char *row(const uint32_t *c, unsigned n)
{
    static char b[4][512]; static unsigned k; char *s = b[k++ & 3]; size_t at = 0; s[0] = 0;
    for (unsigned i = 0; i < n && at + 12 < sizeof b[0]; i++) at += (size_t)snprintf(s + at, sizeof b[0] - at, "%s%u", i ? " " : "", c[i]);
    return s;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const BOOL any = argc > 1 && !strcmp(argv[1], "--any");
        const uint64_t nmx = nmx_registry_id();
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL b580 = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (b580 || (any && !dev)) dev = d;
            if (b580) break;
        }
        if (!dev) { fprintf(stderr, "FAIL: no B580 Metal device (--any runs on the first device)\n"); return 1; }
        printf("device: %s (%s)\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String);

        const MTLSparsePageSize ps = MTLSparsePageSize64;
        const NSUInteger tileBytes = [dev sparseTileSizeInBytesForSparsePageSize:ps];
        const MTLSize ts = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatRGBA8Unorm sampleCount:1 sparsePageSize:ps];
        if (!tileBytes || !ts.width || !ts.height || ts.width * ts.height * 4 != tileBytes) {
            printf("SKIP: this device reports no usable sparse tile (B580: NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1?)\n"); return 5; }
        const NSUInteger W = ts.width * TX, H = ts.height * TY;
        printf("RGBA8 tile %lux%lu (%lu bytes); texture %lux%lu\n", (unsigned long)ts.width, (unsigned long)ts.height, (unsigned long)tileBytes,
               (unsigned long)W, (unsigned long)H);

        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypeSparse; hd.storageMode = MTLStorageModePrivate; hd.size = tileBytes * 16; hd.sparsePageSize = ps;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        if (!heap) { printf("SKIP: newHeapWithDescriptor(MTLHeapTypeSparse) -> nil\n"); return 5; }
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:YES];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> tex = [heap newTextureWithDescriptor:td];
        if (!tex) { fprintf(stderr, "FAIL: sparse heap newTextureWithDescriptor %lux%lu mipmapped -> nil\n", (unsigned long)W, (unsigned long)H); return 2; }
        MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        ad.textureType = MTLTextureType2DArray; ad.arrayLength = 2; ad.storageMode = MTLStorageModePrivate; ad.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> arr = [heap newTextureWithDescriptor:ad];
        if (!arr) { fprintf(stderr, "FAIL: sparse 2D array texture -> nil\n"); return 2; }
        long tier = 0, atier = 0;
        if (@available(macOS 26.0, *)) { tier = (long)tex.sparseTextureTier; atier = (long)arr.sparseTextureTier; }
        printf("sparseTextureTier %ld (array texture %ld); %lu levels, firstMipmapInTail %lu\n", tier, atier,
               (unsigned long)tex.mipmapLevelCount, (unsigned long)tex.firstMipmapInTail);
        if (tier < 2 /* MTLTextureSparseTier2 */) {
            printf("SKIP: sparseTextureTier %ld: no access counters (B580: NVMTL_SPARSE_TIER2=1 with NVMTL_SPARSE_TEXTURES=1)\n", tier); return 5; }
        check(atier == tier, "the array texture answers the same tier (%ld)", atier);

        id<MTLCommandQueue> q = [dev newCommandQueue];
        {   id<MTLCommandBuffer> cb = [q commandBuffer];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            const BOOL get = [be respondsToSelector:@selector(getTextureAccessCounters:region:mipLevel:slice:resetCounters:countersBuffer:countersBufferOffset:)];
            const BOOL reset = [be respondsToSelector:@selector(resetTextureAccessCounters:region:mipLevel:slice:)];
            [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
            if (!get || !reset) { printf("SKIP: Tier2 texture but the blit encoder has %s%s\n", get ? "" : "no getTextureAccessCounters ", reset ? "" : "no resetTextureAccessCounters"); return 5; }
            check(1, "the blit encoder has getTextureAccessCounters and resetTextureAccessCounters");
        }

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLComputePipelineState> preads = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"reads"] error:&err];
        id<MTLComputePipelineState> preadsA = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"reads_array"] error:&err];
        id<MTLComputePipelineState> psamples = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"samples"] error:&err];
        id<MTLComputePipelineState> pplain = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"plain"] error:&err];
        if (!preads || !preadsA || !psamples || !pplain) { fprintf(stderr, "FAIL: compute pipelines: %s\n", err.localizedDescription.UTF8String); return 2; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> draw = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!draw) { fprintf(stderr, "FAIL: render pipeline: %s\n", err.localizedDescription.UTF8String); return 2; }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
        sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest; sd.mipFilter = MTLSamplerMipFilterNearest;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        id<MTLBuffer> pts = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> res = [dev newBufferWithLength:256 options:MTLResourceStorageModeShared];
        id<MTLBuffer> cnt = [dev newBufferWithLength:4096 options:MTLResourceStorageModeShared];
        const NSUInteger tx1 = (W / 2 + ts.width - 1) / ts.width, ty1 = (H / 2 + ts.height - 1) / ts.height;   // level-1 tiles

        // map (0,0) (1,0) (3,3) of level 0, the array texture's tile (1,2) of slice 1; reset every counter
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLResourceStateCommandEncoder> rs = [cb resourceStateCommandEncoder];
        if (!rs) { printf("SKIP: resourceStateCommandEncoder -> nil\n"); return 5; }
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(0, 0, 2, 1) mipLevel:0 slice:0];
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(3, 3, 1, 1) mipLevel:0 slice:0];
        [rs updateTextureMapping:arr mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(1, 2, 1, 1) mipLevel:0 slice:1];
        [rs endEncoding];
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be resetTextureAccessCounters:tex region:MTLRegionMake2D(0, 0, TX, TY) mipLevel:0 slice:0];
        [be resetTextureAccessCounters:tex region:MTLRegionMake2D(0, 0, tx1, ty1) mipLevel:1 slice:0];
        [be resetTextureAccessCounters:arr region:MTLRegionMake2D(0, 0, TX, TY) mipLevel:0 slice:0];
        [be resetTextureAccessCounters:arr region:MTLRegionMake2D(0, 0, TX, TY) mipLevel:0 slice:1];
        [be endEncoding];
        if (finish(cb, "map + reset")) return 3;

        // get(level, slice, region) -> cnt at `off` bytes (the bytes before it are 0xEE)
        uint32_t *(^get)(id<MTLTexture>, NSUInteger, NSUInteger, MTLRegion, NSUInteger, BOOL) =
            ^uint32_t *(id<MTLTexture> t, NSUInteger level, NSUInteger slice, MTLRegion r, NSUInteger off, BOOL reset) {
            memset(cnt.contents, 0xEE, cnt.length);
            id<MTLCommandBuffer> c = [q commandBuffer];
            id<MTLBlitCommandEncoder> b = [c blitCommandEncoder];
            [b getTextureAccessCounters:t region:r mipLevel:level slice:slice resetCounters:reset countersBuffer:cnt countersBufferOffset:off];
            [b endEncoding];
            if (finish(c, "getTextureAccessCounters")) return NULL;
            return (uint32_t *)((uint8_t *)cnt.contents + off);
        };
        // run `pipe` over the points (64 threads each)
        int (^reads)(id<MTLComputePipelineState>, id<MTLTexture>, const void *, size_t, NSUInteger) =
            ^int(id<MTLComputePipelineState> pipe, id<MTLTexture> t, const void *p, size_t bytes, NSUInteger n) {
            memcpy(pts.contents, p, bytes); memset(res.contents, 0xEE, res.length);
            id<MTLCommandBuffer> c = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
            [ce setComputePipelineState:pipe]; [ce setTexture:t atIndex:0]; [ce setSamplerState:smp atIndex:0];
            [ce setBuffer:pts offset:0 atIndex:0]; [ce setBuffer:res offset:0 atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(n, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            [ce endEncoding];
            return finish(c, "reads");
        };
        const MTLRegion all0 = MTLRegionMake2D(0, 0, TX, TY), all1 = MTLRegionMake2D(0, 0, tx1, ty1);

        // 1. nothing read since the reset
        uint32_t *c0 = get(tex, 0, 0, all0, 0, NO); if (!c0) return 3;
        uint32_t z = 0; for (int i = 0; i < TX * TY; i++) z |= c0[i];
        check(z == 0, "after a reset, level 0 counters are zero (%s)", row(c0, TX * TY));
        uint32_t *c1 = get(tex, 1, 0, all1, 0, NO); if (!c1) return 3;
        z = 0; for (NSUInteger i = 0; i < tx1 * ty1; i++) z |= c1[i];
        check(z == 0, "after a reset, level 1 counters are zero (%s)", row(c1, (unsigned)(tx1 * ty1)));

        // 2. reads of tile (0,0) [mapped] and (2,1) [unmapped] of level 0 and (1,1) of level 1
        const uint32_t p2[3][4] = { { 3, 5, 0, 0 }, { (uint32_t)(2 * ts.width + 9), (uint32_t)(ts.height + 17), 0, 0 },
                                    { (uint32_t)(ts.width + 3), (uint32_t)(ts.height + 3), 1, 0 } };
        if (reads(preads, tex, p2, sizeof p2, 3)) return 3;
        const uint32_t *rr = (const uint32_t *)res.contents;
        check(rr[0] == 1 && rr[1] == 0, "compute sparse_read: tile (0,0) resident %u, tile (2,1) resident %u (want 1, 0)", rr[0], rr[1]);
        c0 = get(tex, 0, 0, all0, 64, NO); if (!c0) return 3;
        uint32_t lvl0[TX * TY]; memcpy(lvl0, c0, sizeof lvl0);
        int others = 0; for (int i = 0; i < TX * TY; i++) if (i != 0 && i != 1 * TX + 2) others |= lvl0[i] != 0;
        check(lvl0[0] > 0, "level 0 tile (0,0) [mapped, read] counted %u", lvl0[0]);
        check(lvl0[1 * TX + 2] > 0, "level 0 tile (2,1) [unmapped, read] counted %u", lvl0[1 * TX + 2]);
        check(!others, "level 0 tiles nobody read are zero (%s)", row(lvl0, TX * TY));
        { const uint8_t *b = (const uint8_t *)cnt.contents; int ok = 1; for (int i = 0; i < 64; i++) ok &= b[i] == 0xEE;
          check(ok, "countersBufferOffset 64: the 64 bytes before it are untouched"); }
        c1 = get(tex, 1, 0, all1, 0, NO); if (!c1) return 3;
        others = 0; for (NSUInteger i = 0; i < tx1 * ty1; i++) if (i != 1 * tx1 + 1) others |= c1[i] != 0;
        check(c1[1 * tx1 + 1] > 0 && !others, "level 1: tile (1,1) counted %u, the others zero (%s)", c1[1 * tx1 + 1], row(c1, (unsigned)(tx1 * ty1)));

        // 3. a sub-region: tiles x 1..2, y 0..2 as 2x3 counters, row by row
        uint32_t *cs = get(tex, 0, 0, MTLRegionMake2D(1, 0, 2, 3), 0, NO); if (!cs) return 3;
        int sub = 1; for (int y = 0; y < 3; y++) for (int x = 0; x < 2; x++) sub &= cs[y * 2 + x] == lvl0[y * TX + 1 + x];
        check(sub && cs[1 * 2 + 1] > 0, "sub-region (1,0)+2x3 of level 0: %s (row by row; tile (2,1) at [3])", row(cs, 6));
        check(cs[6] == 0xEEEEEEEEu, "the sub-region wrote 6 counters, no more (word 6 0x%08x)", cs[6]);

        // 4. reset tile (0,0) only
        cb = [q commandBuffer]; be = [cb blitCommandEncoder];
        [be resetTextureAccessCounters:tex region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:0 slice:0];
        [be endEncoding];
        if (finish(cb, "reset (0,0)")) return 3;
        c0 = get(tex, 0, 0, all0, 0, NO); if (!c0) return 3;
        check(c0[0] == 0 && c0[1 * TX + 2] == lvl0[1 * TX + 2], "reset of tile (0,0): (0,0) %u (want 0), (2,1) %u (want %u, kept)",
              c0[0], c0[1 * TX + 2], lvl0[1 * TX + 2]);

        // 5. resetCounters:YES returns the counts and clears them
        c0 = get(tex, 0, 0, all0, 0, YES); if (!c0) return 3;
        check(c0[1 * TX + 2] == lvl0[1 * TX + 2], "get with resetCounters:YES returns (2,1) = %u", c0[1 * TX + 2]);
        c0 = get(tex, 0, 0, all0, 0, NO); if (!c0) return 3;
        z = 0; for (int i = 0; i < TX * TY; i++) z |= c0[i];
        check(z == 0, "after get with resetCounters:YES, level 0 reads zero (%s)", row(c0, TX * TY));
        cb = [q commandBuffer]; be = [cb blitCommandEncoder];
        [be resetTextureAccessCounters:tex region:all1 mipLevel:1 slice:0];
        [be endEncoding];
        if (finish(cb, "reset level 1")) return 3;

        // 6. compute sparse_sample at level(1), the centre of level-1 tile (0,1)
        const float p6[1][4] = { { (0.5f * ts.width) / (W / 2), (1.5f * ts.height) / (H / 2), 1.0f, 0 } };
        if (reads(psamples, tex, p6, sizeof p6, 1)) return 3;
        c0 = get(tex, 0, 0, all0, 0, NO); if (!c0) return 3;
        z = 0; for (int i = 0; i < TX * TY; i++) z |= c0[i];
        c1 = get(tex, 1, 0, all1, 0, YES); if (!c1) return 3;
        others = 0; for (NSUInteger i = 0; i < tx1 * ty1; i++) if (i != 1 * tx1 + 0) others |= c1[i] != 0;
        check(z == 0 && c1[1 * tx1] > 0 && !others, "compute sparse_sample level(1) at tile (0,1) of level 1: level 1 %s, level 0 untouched (%s)",
              row(c1, (unsigned)(tx1 * ty1)), z ? "NOT" : "yes");

        // 7. fragment sparse_sample, implicit LOD: a 512x512 target samples level 0, a 256x256 one level 1
        for (int pass = 0; pass < 2; pass++) {
            const NSUInteger rw = pass ? W / 2 : W, rh = pass ? H / 2 : H;
            MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:rw height:rh mipmapped:NO];
            rtd.storageMode = MTLStorageModePrivate; rtd.usage = MTLTextureUsageRenderTarget;
            id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
            cb = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            const float rsz[2] = { (float)rw, (float)rh };
            [re setRenderPipelineState:draw]; [re setFragmentTexture:tex atIndex:0]; [re setFragmentSamplerState:smp atIndex:0];
            [re setFragmentBytes:rsz length:sizeof rsz atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            if (finish(cb, "fragment draw")) return 3;
            uint32_t a0[TX * TY], a1[64];
            c0 = get(tex, 0, 0, all0, 0, YES); if (!c0) return 3; memcpy(a0, c0, sizeof a0);
            c1 = get(tex, 1, 0, all1, 0, YES); if (!c1) return 3; memcpy(a1, c1, sizeof(uint32_t) * tx1 * ty1);
            int every0 = 1, none0 = 1, every1 = 1, none1 = 1;
            for (int i = 0; i < TX * TY; i++) { every0 &= a0[i] > 0; none0 &= a0[i] == 0; }
            for (NSUInteger i = 0; i < tx1 * ty1; i++) { every1 &= a1[i] > 0; none1 &= a1[i] == 0; }
            if (!pass) check(every0 && none1, "fragment sparse_sample on a %lux%lu target: every level-0 tile counted (%s), no level-1 tile (%s)",
                             (unsigned long)rw, (unsigned long)rh, row(a0, TX * TY), row(a1, (unsigned)(tx1 * ty1)));
            else check(every1 && none0, "fragment sparse_sample on a %lux%lu target: every level-1 tile counted (%s), no level-0 tile (%s)",
                       (unsigned long)rw, (unsigned long)rh, row(a1, (unsigned)(tx1 * ty1)), row(a0, TX * TY));
        }

        // 8. the array texture: slice 1 tile (1,2)
        const uint32_t p8[1][4] = { { (uint32_t)(ts.width + 1), (uint32_t)(2 * ts.height + 1), 0, 1 } };
        if (reads(preadsA, arr, p8, sizeof p8, 1)) return 3;
        check(((const uint32_t *)res.contents)[0] == 1, "array texture: slice 1 tile (1,2) is resident (%u)", ((const uint32_t *)res.contents)[0]);
        uint32_t s0[TX * TY];
        c0 = get(arr, 0, 0, all0, 0, NO); if (!c0) return 3; memcpy(s0, c0, sizeof s0);
        c1 = get(arr, 0, 1, all0, 0, NO); if (!c1) return 3;
        z = 0; for (int i = 0; i < TX * TY; i++) z |= s0[i];
        others = 0; for (int i = 0; i < TX * TY; i++) if (i != 2 * TX + 1) others |= c1[i] != 0;
        check(z == 0 && c1[2 * TX + 1] > 0 && !others, "array texture: slice 1 tile (1,2) counted %u, slice 1 others zero, slice 0 all zero (%s)",
              c1[2 * TX + 1], row(s0, TX * TY));

        // 9. a plain sample() of tile (3,0)
        {   memset(res.contents, 0, res.length);
            id<MTLCommandBuffer> c = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
            [ce setComputePipelineState:pplain]; [ce setTexture:tex atIndex:0]; [ce setSamplerState:smp atIndex:0]; [ce setBuffer:res offset:0 atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(4, 1, 1)];
            [ce endEncoding];
            if (finish(c, "plain sample")) return 3;
            c0 = get(tex, 0, 0, all0, 0, YES); if (!c0) return 3;
            others = 0; for (int i = 0; i < TX * TY; i++) if (i != 3) others |= c0[i] != 0;
            check(c0[3] > 0 && !others, "plain sample() (no residency read) of tile (3,0) counted: level 0 %s", row(c0, TX * TY));
        }

        printf("mtl_sparse_tier2: %d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
