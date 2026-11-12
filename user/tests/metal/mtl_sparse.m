// SPDX-License-Identifier: MIT
// mtl_sparse: Metal sparse textures (tier 1) on the Arc B580 (NVMTL_SPARSE_TEXTURES=1, nullmoth/plugin/NVMTLSparse.m).
//
// A sparse heap of 8 tiles and an RGBA8 sparse texture of 4x4 tiles on it (tile shape from
// sparseTileSizeWithTextureType:), then:
//   1. a resource state encoder maps tiles (0,0) (1,0) and (3,3); a blit uploads a distinct pattern into every tile of
//      the texture (the unmapped tiles' writes must vanish); a compute kernel reads each tile's centre with sparse_read
//      and sparse_sample(level(0)) and reports resident(); a render pass samples the texture with sparse_sample in a
//      fragment shader (magenta where not resident); the texture is copied back. Checks: mapped tiles hold their
//      pattern, unmapped tiles read zero, residency is 1 exactly where mapped, in compute and fragment;
//   2. a second command buffer unmaps (1,0) and maps (2,2) (it takes the page (1,0) gave back): residency follows;
//      heap.usedSize stays at three tiles;
//   3. a mipmapped sparse texture: firstMipmapInTail / tailSizeInBytes, the tail mapped at firstMipmapInTail and a
//      1x1 level written and read back.
// Each check prints PASS or FAIL; the last line counts them.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1 ./mtl_sparse    (B580: the plugin's knob, kept for this process)
//   ./mtl_sparse --any                    the first Metal device when no B580 is present (reference run on a Mac)
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 3 GPU error, 4 wrong results, 5 sparse textures not offered.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     // one thread per tile: the tile's centre by sparse_read and by sparse_sample at level 0
     "kernel void probe(texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], device uint4 *o [[buffer(0)]],\n"
     "                  constant uint4 &p [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {\n"
     "    uint2 c = g * p.xy + p.xy / 2;\n"
     "    sparse_color<float4> r = t.sparse_read(c);\n"
     "    sparse_color<float4> q = t.sparse_sample(s, (float2(c) + 0.5) / float2(t.get_width(), t.get_height()), level(0));\n"
     "    o[g.y * p.z + g.x] = uint4(r.resident() ? 1 : 0, pack_float_to_unorm4x8(r.value()), q.resident() ? 1 : 0,\n"
     "                               pack_float_to_unorm4x8(q.value()));\n"
     "}\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint vid [[vertex_id]]) { float2 q = float2((vid << 1) & 2, vid & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
     "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) {\n"
     "    sparse_color<float4> c = t.sparse_sample(s, in.p.xy / float2(t.get_width(), t.get_height()));\n"
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
static int mapped[TX * TY];   // which tiles the test has mapped
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
// RGBA8 bytes R,G,B,A = t*13+1, 0x55, 0xAA, 0xFF, as a little-endian word
static uint32_t pattern(uint32_t t) { return (uint32_t)((t * 13 + 1) & 0xff) | 0x55u << 8 | 0xAAu << 16 | 0xFFu << 24; }
static const uint32_t kMagenta = 0xFFFF00FFu;

static int finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "FAIL: %s: command buffer status %ld: %s\n", what, (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return 3;
    }
    return 0;
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

        // the tile: 64 KiB pages (the B580's only size; Apple GPUs have it too)
        const MTLSparsePageSize ps = MTLSparsePageSize64;
        const NSUInteger tileBytes = [dev sparseTileSizeInBytesForSparsePageSize:ps];
        const MTLSize ts = [dev sparseTileSizeWithTextureType:MTLTextureType2D pixelFormat:MTLPixelFormatRGBA8Unorm sampleCount:1 sparsePageSize:ps];
        printf("sparseTileSizeInBytes %lu, for 64 KiB pages %lu; RGBA8 2D tile %lux%lux%lu\n", (unsigned long)dev.sparseTileSizeInBytes,
               (unsigned long)tileBytes, (unsigned long)ts.width, (unsigned long)ts.height, (unsigned long)ts.depth);
        if (!tileBytes || !ts.width || !ts.height || ts.width * ts.height * 4 != tileBytes) {
            printf("SKIP: this device reports no usable sparse tile (B580: NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1?)\n"); return 5; }
        check(ts.depth == 1, "the 2D tile is %lux%lu texels, %lu bytes", (unsigned long)ts.width, (unsigned long)ts.height, (unsigned long)tileBytes);
        const NSUInteger W = ts.width * TX, H = ts.height * TY;

        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypeSparse; hd.storageMode = MTLStorageModePrivate; hd.size = tileBytes * 8; hd.sparsePageSize = ps;
        id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
        if (!heap) { printf("SKIP: newHeapWithDescriptor(MTLHeapTypeSparse) -> nil\n"); return 5; }
        check(heap.type == MTLHeapTypeSparse, "sparse heap of %lu bytes (8 tiles), usedSize %lu", (unsigned long)heap.size, (unsigned long)heap.usedSize);
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.storageMode = MTLStorageModePrivate; td.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        id<MTLTexture> tex = [heap newTextureWithDescriptor:td];
        if (!tex) { fprintf(stderr, "FAIL: sparse heap newTextureWithDescriptor %lux%lu -> nil\n", (unsigned long)W, (unsigned long)H); return 2; }
        check(tex.isSparse, "the texture is sparse (%lux%lu, %ux%u tiles)", (unsigned long)W, (unsigned long)H, TX, TY);
        if (@available(macOS 26.0, *)) check(tex.sparseTextureTier >= MTLTextureSparseTier1, "sparseTextureTier %ld (at least Tier1 = 1; the B580 answers 1, an M1 2)", (long)tex.sparseTextureTier);

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLComputePipelineState> probe = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&err];
        if (!probe) { fprintf(stderr, "FAIL: probe pipeline: %s\n", err.localizedDescription.UTF8String); return 2; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        id<MTLRenderPipelineState> draw = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!draw) { fprintf(stderr, "FAIL: render pipeline: %s\n", err.localizedDescription.UTF8String); return 2; }
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
        id<MTLSamplerState> smp = [dev newSamplerStateWithDescriptor:sd];
        MTLTextureDescriptor *rtd = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        rtd.storageMode = MTLStorageModePrivate; rtd.usage = MTLTextureUsageRenderTarget;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:rtd];
        id<MTLBuffer> up = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> down = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> rdown = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> out = [dev newBufferWithLength:TX * TY * 16 options:MTLResourceStorageModeShared];
        uint32_t params[4] = { (uint32_t)ts.width, (uint32_t)ts.height, TX, 0 };
        id<MTLBuffer> pb = [dev newBufferWithBytes:params length:sizeof params options:MTLResourceStorageModeShared];
        uint32_t *u = (uint32_t *)up.contents;
        for (NSUInteger y = 0; y < H; y++) for (NSUInteger x = 0; x < W; x++) u[y * W + x] = pattern((uint32_t)((y / ts.height) * TX + x / ts.width));
        id<MTLCommandQueue> q = [dev newCommandQueue];

        // the readback pass: copy, residency kernel, fragment sampling, all after the mappings of the command buffer
        void (^readback)(id<MTLCommandBuffer>) = ^(id<MTLCommandBuffer> cb) {
            memset(out.contents, 0xEE, out.length); memset(down.contents, 0xEE, down.length); memset(rdown.contents, 0xEE, rdown.length);
            id<MTLComputeCommandEncoder> ce = [cb computeCommandEncoder];
            [ce setComputePipelineState:probe]; [ce setTexture:tex atIndex:0]; [ce setSamplerState:smp atIndex:0];
            [ce setBuffer:out offset:0 atIndex:0]; [ce setBuffer:pb offset:0 atIndex:1];
            [ce dispatchThreadgroups:MTLSizeMake(TX, TY, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:draw]; [re setFragmentTexture:tex atIndex:0]; [re setFragmentSamplerState:smp atIndex:0];
            [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [re endEncoding];
            id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
            [be copyFromTexture:tex sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                       toBuffer:down destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                       toBuffer:rdown destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding];
        };
        void (^verify)(const char *, BOOL) = ^(const char *when, BOOL contents) {
            const uint32_t *d = (const uint32_t *)down.contents, *r = (const uint32_t *)rdown.contents, *o = (const uint32_t *)out.contents;
            for (uint32_t t = 0; t < TX * TY; t++) {
                const NSUInteger tx = t % TX, ty = t / TX, cx = tx * ts.width + ts.width / 2, cy = ty * ts.height + ts.height / 2;
                const uint32_t want = mapped[t] ? pattern(t) : 0;
                if (contents || !mapped[t]) {
                    NSUInteger bad = 0;
                    for (NSUInteger y = ty * ts.height; y < (ty + 1) * ts.height; y++) for (NSUInteger x = tx * ts.width; x < (tx + 1) * ts.width; x++) bad += d[y * W + x] != want;
                    check(!bad, "%s: tile (%lu,%lu) %s: blit readback 0x%08x in every texel (%lu differ)", when, (unsigned long)tx, (unsigned long)ty,
                          mapped[t] ? "mapped" : "unmapped", want, (unsigned long)bad);
                }
                const uint32_t *e = o + t * 4;
                check(e[0] == (uint32_t)mapped[t] && e[2] == (uint32_t)mapped[t], "%s: tile (%lu,%lu) compute sparse_read resident=%u, sparse_sample resident=%u (want %d)",
                      when, (unsigned long)tx, (unsigned long)ty, e[0], e[2], mapped[t]);
                if (contents && mapped[t]) check(e[1] == want && e[3] == want, "%s: tile (%lu,%lu) compute texels 0x%08x / 0x%08x (want 0x%08x)", when,
                                                 (unsigned long)tx, (unsigned long)ty, e[1], e[3], want);
                const uint32_t f = r[cy * W + cx];
                if (contents || !mapped[t])
                    check(f == (mapped[t] ? want : kMagenta), "%s: tile (%lu,%lu) fragment sparse_sample 0x%08x (want 0x%08x: %s)", when, (unsigned long)tx,
                          (unsigned long)ty, f, mapped[t] ? want : kMagenta, mapped[t] ? "the texel" : "magenta, not resident");
                else check(f != kMagenta, "%s: tile (%lu,%lu) fragment sparse_sample resident (0x%08x)", when, (unsigned long)tx, (unsigned long)ty, f);
            }
        };

        // 1. map (0,0) (1,0) as one region and (3,3); upload over everything; read back
        id<MTLCommandBuffer> cb = [q commandBuffer];
        id<MTLResourceStateCommandEncoder> rs = [cb resourceStateCommandEncoder];
        if (!rs) { printf("SKIP: resourceStateCommandEncoder -> nil (B580: NVMTL_SPARSE_TEXTURES=1?)\n"); return 5; }
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(0, 0, 2, 1) mipLevel:0 slice:0];
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(3, 3, 1, 1) mipLevel:0 slice:0];
        [rs endEncoding];
        mapped[0] = mapped[1] = mapped[15] = 1;
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:up sourceOffset:0 sourceBytesPerRow:W * 4 sourceBytesPerImage:W * H * 4 sourceSize:MTLSizeMake(W, H, 1)
                 toTexture:tex destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding];
        readback(cb);
        if (finish(cb, "map + upload + readback")) return 3;
        check(heap.usedSize == 3 * tileBytes, "heap.usedSize %lu after mapping 3 tiles (want %lu)", (unsigned long)heap.usedSize, (unsigned long)(3 * tileBytes));
        verify("mapped", YES);

        // 2. unmap (1,0), map (2,2): residency follows (the new tile's contents are undefined, as on any Metal device)
        cb = [q commandBuffer];
        rs = [cb resourceStateCommandEncoder];
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeUnmap region:MTLRegionMake2D(1, 0, 1, 1) mipLevel:0 slice:0];
        [rs updateTextureMapping:tex mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(2, 2, 1, 1) mipLevel:0 slice:0];
        [rs endEncoding];
        mapped[1] = 0; mapped[10] = 1;
        readback(cb);
        if (finish(cb, "remap + readback")) return 3;
        check(heap.usedSize == 3 * tileBytes, "heap.usedSize %lu after unmap + map (want %lu)", (unsigned long)heap.usedSize, (unsigned long)(3 * tileBytes));
        verify("remapped", NO);
        { const uint32_t *d = (const uint32_t *)down.contents;   // the tiles that kept their page keep their data
          check(d[(ts.height / 2) * W + ts.width / 2] == pattern(0) && d[(3 * ts.height + 1) * W + 3 * ts.width + 1] == pattern(15),
                "remapped: tiles (0,0) and (3,3) kept their data (0x%08x, 0x%08x)", d[(ts.height / 2) * W + ts.width / 2], d[(3 * ts.height + 1) * W + 3 * ts.width + 1]); }

        // 3. a mipmapped sparse texture: the mip tail
        MTLTextureDescriptor *md = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:YES];
        md.storageMode = MTLStorageModePrivate; md.usage = MTLTextureUsageShaderRead;
        id<MTLTexture> mt = [heap newTextureWithDescriptor:md];
        if (!mt) { check(0, "mipmapped sparse texture %lux%lu, %lu levels -> nil", (unsigned long)W, (unsigned long)H, (unsigned long)md.mipmapLevelCount); }
        else {
            const NSUInteger tail = mt.firstMipmapInTail, last = mt.mipmapLevelCount - 1;
            check(tail > 0 && tail <= last && mt.tailSizeInBytes > 0, "mipmapped: %lu levels, firstMipmapInTail %lu, tailSizeInBytes %lu",
                  (unsigned long)mt.mipmapLevelCount, (unsigned long)tail, (unsigned long)mt.tailSizeInBytes);
            if (tail > 0 && tail <= last) {
                const NSUInteger before = heap.usedSize;
                cb = [q commandBuffer];
                rs = [cb resourceStateCommandEncoder];
                [rs updateTextureMapping:mt mode:MTLSparseTextureMappingModeMap region:MTLRegionMake2D(0, 0, 1, 1) mipLevel:tail slice:0];
                [rs endEncoding];
                const uint32_t px = 0x11223344u;
                id<MTLBuffer> one = [dev newBufferWithBytes:&px length:4 options:MTLResourceStorageModeShared];
                be = [cb blitCommandEncoder];
                [be copyFromBuffer:one sourceOffset:0 sourceBytesPerRow:4 sourceBytesPerImage:4 sourceSize:MTLSizeMake(1, 1, 1)
                         toTexture:mt destinationSlice:0 destinationLevel:last destinationOrigin:MTLOriginMake(0, 0, 0)];
                memset(down.contents, 0, 4);
                [be copyFromTexture:mt sourceSlice:0 sourceLevel:last sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(1, 1, 1)
                           toBuffer:down destinationOffset:0 destinationBytesPerRow:4 destinationBytesPerImage:4];
                [be endEncoding];
                if (finish(cb, "mip tail")) return 3;
                check(((const uint32_t *)down.contents)[0] == px, "mip tail: level %lu (1x1) reads back 0x%08x (want 0x%08x)", (unsigned long)last,
                      ((const uint32_t *)down.contents)[0], px);
                check(heap.usedSize == before + mt.tailSizeInBytes, "heap.usedSize %lu after mapping the tail (want %lu)",
                                (unsigned long)heap.usedSize, (unsigned long)(before + mt.tailSizeInBytes));
            }
        }
        printf("mtl_sparse: %d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
