// SPDX-License-Identifier: MIT
// mtl_tile: tile pipelines (MTLTileRenderPipelineDescriptor, dispatchThreadsPerTile:, implicit and explicit imageblocks,
// tile-stage bindings, threadgroup memory in a render pass, MSAA colours, the Metal 4 descriptor and encoder). On the B580
// the driver splits the render pass at each tile dispatch (nullmoth/plugin/NVMTLTile.m, docs/design/DESIGN-tile-shading.md);
// Apple GPUs (families 4+) run the same cases in tile memory and are the reference (--any on an Apple silicon Mac).
//
//   1. deferred lighting: a G-buffer pass writes albedo (RGBA8) and normal (RGBA16F), a tile kernel reads both through the
//      implicit imageblock and writes the lit colour (BGRA8) with a light direction from setTileBytes; a draw after the
//      dispatch blends on top (the pass resumes with its attachments loaded). 40x24 target, 16x16 tiles (partial tiles);
//   2. an explicit imageblock with a custom struct (float, uint, half2): each thread fills its cell, a barrier, then reads
//      its right neighbour's cell and writes it to a tile texture (setTileTexture) at its grid position;
//   3. threadgroup memory: setThreadgroupMemoryLength:offset:atIndex: and a per-tile average of the red channel written to
//      green, scaled by setTileBuffer data;
//   4. MSAA 4x: a triangle edge across the target, the tile kernel swaps red and blue of every colour (get_num_colors,
//      colour-rate reads, coverage-mask writes); the resolve must equal the swapped scene drawn directly;
//   5. integer and float attachments (R32Uint + RGBA32Float) in one imageblock, get_imageblock_width, a pass with no draw
//      before the dispatch (its clear is applied first);
//   6. three dispatches with a draw between the second and third (planes kept, then copied back and reloaded);
//   7. Metal 4: MTL4TileRenderPipelineDescriptor through an MTL4Compiler, an MTL4RenderCommandEncoder with a tile-stage
//      argument table (SKIP without MTLGPUFamilyMetal4).
//
//   NVMTL_ENABLE=1 ./mtl_tile          (B580; NVMTL_METAL4=1 for case 7)
//   ./mtl_tile --any                   the first Metal device when no B580 is present
//
// Exit: 0 pass, 1 no device, 2 setup/compile failure, 3 GPU error, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <math.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs_full(uint v [[vertex_id]]) { float2 q = float2((v << 1) & 2, v & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
     // quad over the left half (two triangles)
     "vertex V vs_left(uint v [[vertex_id]]) { const float2 c[6] = { {-1,-1}, {0,-1}, {-1,1}, {0,-1}, {0,1}, {-1,1} }; V o; o.p = float4(c[v], 0, 1); return o; }\n"
     // a triangle whose hypotenuse crosses the target diagonally (partially covered pixels)
     "vertex V vs_tri(uint v [[vertex_id]]) { const float2 c[3] = { {-1,-1}, {1.0,-1}, {-1,0.9} }; V o; o.p = float4(c[v], 0, 1); return o; }\n"
     "struct G { half4 albedo [[color(0)]]; half4 normal [[color(1)]]; };\n"
     "fragment G fs_gbuf(V in [[stage_in]]) {\n"
     "    uint2 p = uint2(in.p.xy); G g;\n"
     "    g.albedo = half4(half(p.x % 8) / 7.0h, half(p.y % 6) / 5.0h, 0.5h, 1.0h);\n"
     "    float3 n = normalize(float3(float(p.x) - 20.0, float(p.y) - 12.0, 16.0));\n"
     "    g.normal = half4(half3(n * 0.5 + 0.5), 1.0h); return g; }\n"
     "struct Deferred { half4 albedo [[color(0)]]; half4 normal [[color(1)]]; half4 lit [[color(2)]]; };\n"
     "kernel void tile_light(imageblock<Deferred, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]],\n"
     "                       constant float4 &L [[buffer(0)]]) {\n"
     "    Deferred d = ib.read(tid);\n"
     "    float ndl = max(dot(float3(d.normal.xyz) * 2.0 - 1.0, L.xyz), 0.0);\n"
     "    d.lit = half4(half3(float3(d.albedo.rgb) * ndl), 1.0h); ib.write(d, tid); }\n"
     "struct Lit { half4 lit [[color(2)]]; };\n"
     "fragment Lit fs_add(V in [[stage_in]]) { Lit o; o.lit = half4(0.0h, 0.0h, 0.25h, 0.0h); return o; }\n"
     "struct Cell { float lum; uint idx; half2 pad; };\n"
     "kernel void tile_explicit(imageblock<Cell, imageblock_layout_explicit> blk, ushort2 tid [[thread_position_in_threadgroup]],\n"
     "                          ushort2 tsz [[threads_per_threadgroup]], uint2 gid [[thread_position_in_grid]],\n"
     "                          texture2d<float, access::write> out [[texture(0)]]) {\n"
     "    threadgroup_imageblock Cell *c = blk.data(tid);\n"
     "    c->lum = float(gid.x * 3 + gid.y); c->idx = uint(tid.x) + uint(tid.y) * 100; c->pad = half2(0.5h);\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup_imageblock);\n"
     "    threadgroup_imageblock Cell *n = blk.data(ushort2((tid.x + 1) % tsz.x, tid.y));\n"
     "    out.write(float4(n->lum, float(n->idx), float(n->pad.x), 1.0), gid); }\n"
     "struct C1 { half4 c [[color(0)]]; };\n"
     "fragment half4 fs_ramp(V in [[stage_in]]) { uint2 p = uint2(in.p.xy); return half4(half(p.x) / 31.0h, 0.0h, 0.0h, 1.0h); }\n"
     "kernel void tile_tg(imageblock<C1, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]],\n"
     "                    ushort2 tsz [[threads_per_threadgroup]], threadgroup float *acc [[threadgroup(0)]], constant float *k [[buffer(1)]]) {\n"
     "    uint li = tid.x + tid.y * tsz.x; C1 v = ib.read(tid); acc[li] = float(v.c.r);\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
     "    if (li == 0) { float s = 0; for (uint i = 0; i < uint(tsz.x * tsz.y); i++) s += acc[i]; acc[0] = s / float(tsz.x * tsz.y); }\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
     "    v.c.g = half(acc[0] * k[0]); ib.write(v, tid); }\n"
     "fragment half4 fs_red(V in [[stage_in]]) { return half4(1, 0, 0, 1); }\n"
     "fragment half4 fs_blue(V in [[stage_in]]) { return half4(0, 0, 1, 1); }\n"
     "kernel void tile_ms(imageblock<C1, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]]) {\n"
     "    ushort n = ib.get_num_colors(tid);\n"
     "    for (ushort i = 0; i < n; i++) { C1 v = ib.read(tid, i, imageblock_data_rate::color); v.c = v.c.bgra; ib.write(v, tid, ib.get_color_coverage_mask(tid, i)); } }\n"
     "struct FI { float4 c [[color(0)]]; uint id [[color(1)]]; };\n"
     "kernel void tile_fi(imageblock<FI, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]], uint2 gid [[thread_position_in_grid]]) {\n"
     "    FI v = ib.read(tid); v.c = v.c.bgra + float4(0, 0, 0, 0.5); v.id = v.id + gid.x + 1000 * gid.y + ib.get_width(); ib.write(v, tid); }\n"
     "kernel void tile_add(imageblock<C1, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]]) {\n"
     "    C1 v = ib.read(tid); v.c.g += 0.25h; ib.write(v, tid); }\n"
     "fragment half4 fs_setb(V in [[stage_in]]) { return half4(0, 0, 1, 0); }\n"
     "kernel void tile_scale(imageblock<C1, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]], constant float *s [[buffer(0)]]) {\n"
     "    C1 v = ib.read(tid); v.c.rgb *= half(s[0]); ib.write(v, tid); }\n";

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0;
    IORegistryEntryGetRegistryEntryID(s, &id);
    IOObjectRelease(s);
    return id;
}
static int npass, nfail, nskip;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
static void skip(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void skip(const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("SKIP "); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    nskip++;
}
static const char *say(NSError *e) { return e ? [NSString stringWithFormat:@"%@ %ld: %@", e.domain, (long)e.code, e.localizedDescription].UTF8String : "(no error)"; }
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLLibrary> lib;
static int gpu_error;
static void finish(id<MTLCommandBuffer> cb)
{
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String ?: "?"); gpu_error = 1; }
}
static id<MTLTexture> tex(MTLPixelFormat f, NSUInteger w, NSUInteger h, NSUInteger samples, MTLTextureUsage u)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:f width:w height:h mipmapped:NO];
    td.usage = u;
    if (samples > 1) { td.textureType = MTLTextureType2DMultisample; td.sampleCount = samples; td.storageMode = MTLStorageModePrivate; }
    else td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}
static id<MTLRenderPipelineState> render(NSString *vs, NSString *fs, const MTLPixelFormat *fmts, NSUInteger n, NSUInteger samples, BOOL blend)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:vs]; d.fragmentFunction = [lib newFunctionWithName:fs]; d.rasterSampleCount = samples;
    for (NSUInteger i = 0; i < n; i++) {
        d.colorAttachments[i].pixelFormat = fmts[i];
        if (blend) { d.colorAttachments[i].blendingEnabled = YES; d.colorAttachments[i].sourceRGBBlendFactor = MTLBlendFactorOne;
                     d.colorAttachments[i].destinationRGBBlendFactor = MTLBlendFactorOne; d.colorAttachments[i].sourceAlphaBlendFactor = MTLBlendFactorZero;
                     d.colorAttachments[i].destinationAlphaBlendFactor = MTLBlendFactorOne; }
    }
    NSError *e = nil; id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!ps) fprintf(stderr, "render pipeline %s/%s: %s\n", vs.UTF8String, fs.UTF8String, say(e));
    return ps;
}
static id<MTLRenderPipelineState> tile(NSString *fn, const MTLPixelFormat *fmts, NSUInteger n, NSUInteger samples, NSError **err)
{
    MTLTileRenderPipelineDescriptor *d = [MTLTileRenderPipelineDescriptor new];
    d.tileFunction = [lib newFunctionWithName:fn]; d.rasterSampleCount = samples; d.threadgroupSizeMatchesTileSize = YES;
    for (NSUInteger i = 0; i < n; i++) d.colorAttachments[i].pixelFormat = fmts[i];
    return [dev newRenderPipelineStateWithTileDescriptor:d options:MTLPipelineOptionNone reflection:nil error:err];
}
static MTLRenderPassColorAttachmentDescriptor *att(MTLRenderPassDescriptor *p, NSUInteger i, id<MTLTexture> t, MTLLoadAction l, MTLClearColor c)
{
    MTLRenderPassColorAttachmentDescriptor *a = p.colorAttachments[i];
    a.texture = t; a.loadAction = l; a.clearColor = c; a.storeAction = MTLStoreActionStore;
    return a;
}
static float h2f(uint16_t h)
{
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    float v = e == 0 ? ldexpf((float)m, -24) : e == 31 ? INFINITY : ldexpf((float)(m | 1024), (int)e - 25);
    return s ? -v : v;
}
static int close8(int a, int b, int tol) { return abs(a - b) <= tol; }

// 1.
static void case_deferred(void)
{
    enum { W = 40, H = 24 };
    const MTLPixelFormat gf[3] = { MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA16Float, MTLPixelFormatBGRA8Unorm };
    NSError *err = nil;
    id<MTLRenderPipelineState> gp = render(@"vs_full", @"fs_gbuf", gf, 2, 1, NO), tp = tile(@"tile_light", gf, 3, 1, &err);
    MTLRenderPipelineDescriptor *ad = [MTLRenderPipelineDescriptor new];
    ad.vertexFunction = [lib newFunctionWithName:@"vs_left"]; ad.fragmentFunction = [lib newFunctionWithName:@"fs_add"];
    for (int i = 0; i < 3; i++) ad.colorAttachments[i].pixelFormat = gf[i];
    ad.colorAttachments[2].blendingEnabled = YES; ad.colorAttachments[2].sourceRGBBlendFactor = MTLBlendFactorOne; ad.colorAttachments[2].destinationRGBBlendFactor = MTLBlendFactorOne;
    ad.colorAttachments[2].sourceAlphaBlendFactor = MTLBlendFactorZero; ad.colorAttachments[2].destinationAlphaBlendFactor = MTLBlendFactorOne;
    id<MTLRenderPipelineState> ap = [dev newRenderPipelineStateWithDescriptor:ad error:NULL];
    if (!gp || !tp || !ap) { check(0, "1. deferred lighting: pipelines G-buffer %p tile %p add %p: %s", (__bridge void *)gp, (__bridge void *)tp, (__bridge void *)ap, say(err)); return; }
    id<MTLTexture> t0 = tex(gf[0], W, H, 1, MTLTextureUsageRenderTarget), t1 = tex(gf[1], W, H, 1, MTLTextureUsageRenderTarget), t2 = tex(gf[2], W, H, 1, MTLTextureUsageRenderTarget);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    att(p, 0, t0, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 0)); att(p, 1, t1, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 0));
    att(p, 2, t2, MTLLoadActionClear, MTLClearColorMake(0, 1, 0, 1));
    p.tileWidth = 16; p.tileHeight = 16;
    const float L[4] = { 0.267261f, 0.534522f, 0.801784f, 0 };   // normalize(1, 2, 3)
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:gp]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re setRenderPipelineState:tp]; [re setTileBytes:L length:sizeof L atIndex:0];
    [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re setRenderPipelineState:ap]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [re endEncoding];
    finish(cb);
    uint8_t a[W * H * 4], lit[W * H * 4]; uint16_t n[W * H * 4];
    [t0 getBytes:a bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    [t1 getBytes:n bytesPerRow:W * 8 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    [t2 getBytes:lit bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, first = -1; int want[4] = { 0 }, got[4] = { 0 };
    for (int i = 0; i < W * H; i++) {
        float nx = h2f(n[4 * i]) * 2 - 1, ny = h2f(n[4 * i + 1]) * 2 - 1, nz = h2f(n[4 * i + 2]) * 2 - 1;
        float ndl = fmaxf(nx * L[0] + ny * L[1] + nz * L[2], 0);
        const int x = i % W;
        int w[4] = { (int)lrintf(a[4 * i] / 255.0f * ndl * 255), (int)lrintf(a[4 * i + 1] / 255.0f * ndl * 255), (int)lrintf(a[4 * i + 2] / 255.0f * ndl * 255), 255 };
        if (x < W / 2) w[2] = w[2] + 64 > 255 ? 255 : w[2] + 64;   // fs_add: +0.25 blue on the left half, after the dispatch
        // BGRA8 bytes: b, g, r, a
        int g[4] = { lit[4 * i + 2], lit[4 * i + 1], lit[4 * i], lit[4 * i + 3] };
        int ok = 1; for (int k = 0; k < 4; k++) ok &= close8(g[k], w[k], 3);
        if (!ok) { if (first < 0) { first = i; memcpy(want, w, sizeof want); memcpy(got, g, sizeof got); } bad++; }
    }
    int ga = a[4 * (5 + 3 * W)] == (int)lrintf(5 / 7.0f * 255) || close8(a[4 * (5 + 3 * W)], (int)lrintf(5 / 7.0f * 255), 1);
    check(!bad && ga, "1. deferred lighting (G-buffer draw, tile kernel over 3 attachments, setTileBytes, a blended draw after): %d of %d pixels wrong%s",
          bad, W * H, first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d): rgba %d %d %d %d, want %d %d %d %d)", first % W, first / W,
                                        got[0], got[1], got[2], got[3], want[0], want[1], want[2], want[3]].UTF8String);
}

// 2.
static void case_explicit(void)
{
    enum { W = 32, H = 32 };
    const MTLPixelFormat f[1] = { MTLPixelFormatRGBA8Unorm };
    NSError *err = nil; id<MTLRenderPipelineState> tp = tile(@"tile_explicit", f, 1, 1, &err);
    if (!tp) { check(0, "2. explicit imageblock: tile pipeline: %s", say(err)); return; }
    id<MTLTexture> rt = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget), out = tex(MTLPixelFormatRGBA32Float, W, H, 1, MTLTextureUsageShaderWrite | MTLTextureUsageShaderRead);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    att(p, 0, rt, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 1));
    p.tileWidth = 16; p.tileHeight = 16; p.imageblockSampleLength = MAX(tp.imageblockSampleLength, 16);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:tp]; [re setTileTexture:out atIndex:0];
    [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re endEncoding];
    finish(cb);
    float px[W * H * 4];
    [out getBytes:px bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, first = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const int tx = x % 16, ty = y % 16, nx = (tx + 1) % 16, gx = x - tx + nx;
        const float want[4] = { (float)(gx * 3 + y), (float)(nx + ty * 100), 0.5f, 1 };
        const float *g = &px[4 * (y * W + x)];
        if (g[0] != want[0] || g[1] != want[1] || g[2] != want[2] || g[3] != want[3]) { if (first < 0) first = y * W + x; bad++; }
    }
    check(!bad, "2. explicit imageblock (struct float/uint/half2, barrier, a neighbour's cell, setTileTexture): %d of %d texels wrong%s", bad, W * H,
          first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d) = %g %g %g)", first % W, first / W, px[4 * first], px[4 * first + 1], px[4 * first + 2]].UTF8String);
}

// 3.
static void case_threadgroup(void)
{
    enum { W = 32, H = 32 };
    const MTLPixelFormat f[1] = { MTLPixelFormatRGBA8Unorm };
    NSError *err = nil;
    id<MTLRenderPipelineState> rp = render(@"vs_full", @"fs_ramp", f, 1, 1, NO), tp = tile(@"tile_tg", f, 1, 1, &err);
    if (!rp || !tp) { check(0, "3. threadgroup memory: pipelines: %s", say(err)); return; }
    id<MTLTexture> rt = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget);
    id<MTLBuffer> k = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; ((float *)k.contents)[0] = 0.5f;
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    att(p, 0, rt, MTLLoadActionClear, MTLClearColorMake(0, 0, 0, 1));
    p.tileWidth = 16; p.tileHeight = 16; p.threadgroupMemoryLength = 16 * 16 * 4;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:rp]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [re setRenderPipelineState:tp]; [re setThreadgroupMemoryLength:16 * 16 * 4 offset:0 atIndex:0]; [re setTileBuffer:k offset:0 atIndex:1];
    [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re endEncoding];
    finish(cb);
    uint8_t px[W * H * 4];
    [rt getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, first = -1, want0 = 0;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float s = 0; const int x0 = x - x % 16;
        for (int j = 0; j < 16; j++) s += px[4 * (y * W + x0 + j)] / 255.0f;   // the red the kernel read (rows of a tile are equal)
        const int want = (int)lrintf(s / 16 * 0.5f * 255);
        if (!close8(px[4 * (y * W + x) + 1], want, 2)) { if (first < 0) { first = y * W + x; want0 = want; } bad++; }
    }
    check(!bad, "3. threadgroup memory (setThreadgroupMemoryLength:offset:atIndex:, per-tile average, setTileBuffer): %d of %d pixels wrong%s", bad, W * H,
          first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d): g %d, want %d)", first % W, first / W, px[4 * first + 1], want0].UTF8String);
}

// 4.
static void case_msaa(void)
{
    enum { W = 16, H = 16 };
    const MTLPixelFormat f[1] = { MTLPixelFormatRGBA8Unorm };
    NSError *err = nil;
    id<MTLRenderPipelineState> red = render(@"vs_tri", @"fs_red", f, 1, 4, NO), blue = render(@"vs_tri", @"fs_blue", f, 1, 4, NO), tp = tile(@"tile_ms", f, 1, 4, &err);
    if (!red || !blue || !tp) { check(0, "4. MSAA: pipelines red %p blue %p tile %p: %s", (__bridge void *)red, (__bridge void *)blue, (__bridge void *)tp, say(err)); return; }
    id<MTLTexture> ms = tex(f[0], W, H, 4, MTLTextureUsageRenderTarget), a = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget), b = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget);
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    for (int pass = 0; pass < 2; pass++) {
        MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
        MTLRenderPassColorAttachmentDescriptor *c = att(p, 0, ms, MTLLoadActionClear, pass ? MTLClearColorMake(1, 0, 0, 1) : MTLClearColorMake(0, 0, 1, 1));
        c.storeAction = MTLStoreActionMultisampleResolve; c.resolveTexture = pass ? b : a;
        p.tileWidth = 16; p.tileHeight = 16;
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
        [re setRenderPipelineState:pass ? blue : red]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        if (!pass) { [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)]; }
        [re endEncoding];
    }
    finish(cb);
    uint8_t pa[W * H * 4], pb[W * H * 4];
    [a getBytes:pa bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    [b getBytes:pb bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, edge = 0, first = -1;
    for (int i = 0; i < W * H; i++) {
        int ok = 1; for (int k = 0; k < 4; k++) ok &= close8(pa[4 * i + k], pb[4 * i + k], 2);
        if (pb[4 * i] > 8 && pb[4 * i] < 247) edge++;
        if (!ok) { if (first < 0) first = i; bad++; }
    }
    check(!bad && edge > 0, "4. MSAA 4x (per-colour reads, coverage-mask writes, resolve): %d of %d pixels differ from the swapped scene, %d edge pixels%s",
          bad, W * H, edge, first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d): %d %d %d %d, want %d %d %d %d)", first % W, first / W,
          pa[4 * first], pa[4 * first + 1], pa[4 * first + 2], pa[4 * first + 3], pb[4 * first], pb[4 * first + 1], pb[4 * first + 2], pb[4 * first + 3]].UTF8String);
}

// 5.
static void case_int_float(void)
{
    enum { W = 24, H = 20 };
    const MTLPixelFormat f[2] = { MTLPixelFormatRGBA32Float, MTLPixelFormatR32Uint };
    NSError *err = nil; id<MTLRenderPipelineState> tp = tile(@"tile_fi", f, 2, 1, &err);
    if (!tp) { check(0, "5. integer and float attachments: tile pipeline: %s", say(err)); return; }
    id<MTLTexture> t0 = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget), t1 = tex(f[1], W, H, 1, MTLTextureUsageRenderTarget);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    att(p, 0, t0, MTLLoadActionClear, MTLClearColorMake(0.125, 0.25, 0.75, 0.25)); att(p, 1, t1, MTLLoadActionClear, MTLClearColorMake(7, 0, 0, 0));
    p.tileWidth = 16; p.tileHeight = 16;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re endEncoding];
    finish(cb);
    float c[W * H * 4]; uint32_t id_[W * H];
    [t0 getBytes:c bytesPerRow:W * 16 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    [t1 getBytes:id_ bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, first = -1;
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        const int i = y * W + x;
        const int ok = c[4 * i] == 0.75f && c[4 * i + 1] == 0.25f && c[4 * i + 2] == 0.125f && c[4 * i + 3] == 0.75f && id_[i] == (uint32_t)(7 + x + 1000 * y + 16);
        if (!ok) { if (first < 0) first = i; bad++; }
    }
    check(!bad, "5. RGBA32Float + R32Uint, get_imageblock_width, a dispatch with no draw before it: %d of %d pixels wrong%s", bad, W * H,
          first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d): %g %g %g %g id %u)", first % W, first / W, c[4 * first], c[4 * first + 1],
                                                      c[4 * first + 2], c[4 * first + 3], id_[first]].UTF8String);
}

// 6.
static void case_sequence(void)
{
    enum { W = 32, H = 16 };
    const MTLPixelFormat f[1] = { MTLPixelFormatRGBA8Unorm };
    NSError *err = nil;
    id<MTLRenderPipelineState> tp = tile(@"tile_add", f, 1, 1, &err), bp = render(@"vs_left", @"fs_setb", f, 1, 1, YES);
    if (!tp || !bp) { check(0, "6. dispatch sequence: pipelines: %s", say(err)); return; }
    id<MTLTexture> rt = tex(f[0], W, H, 1, MTLTextureUsageRenderTarget);
    MTLRenderPassDescriptor *p = [MTLRenderPassDescriptor renderPassDescriptor];
    att(p, 0, rt, MTLLoadActionClear, MTLClearColorMake(0.5, 0, 0, 1));
    p.tileWidth = 16; p.tileHeight = 16;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re setRenderPipelineState:bp]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re endEncoding];
    finish(cb);
    uint8_t px[W * H * 4];
    [rt getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0, first = -1;
    for (int i = 0; i < W * H; i++) {
        const int x = i % W, wb = x < W / 2 ? 255 : 0;
        if (!close8(px[4 * i], 128, 1) || !close8(px[4 * i + 1], 191, 1) || px[4 * i + 2] != wb || px[4 * i + 3] != 255) { if (first < 0) first = i; bad++; }
    }
    check(!bad, "6. two dispatches, a draw, a third dispatch (green += 0.25 each, blue drawn on the left): %d of %d pixels wrong%s", bad, W * H,
          first < 0 ? "" : [NSString stringWithFormat:@" (first (%d,%d): %d %d %d %d, want 128 191 %d 255)", first % W, first / W, px[4 * first],
                                                      px[4 * first + 1], px[4 * first + 2], px[4 * first + 3], (first % W) < W / 2 ? 255 : 0].UTF8String);
}

// 7.
API_AVAILABLE(macos(26.0))
static void case_metal4(void)
{
    if (![dev supportsFamily:MTLGPUFamilyMetal4]) { skip("7. Metal 4: no MTLGPUFamilyMetal4 (B580: NVMTL_METAL4=1)"); return; }
    enum { W = 16, H = 16 };
    NSError *err = nil;
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTL4LibraryFunctionDescriptor *fd = [MTL4LibraryFunctionDescriptor new]; fd.name = @"tile_scale"; fd.library = lib;
    MTL4TileRenderPipelineDescriptor *td = [MTL4TileRenderPipelineDescriptor new];
    td.tileFunctionDescriptor = fd; td.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; td.threadgroupSizeMatchesTileSize = YES;
    id<MTLRenderPipelineState> tp = comp ? [comp newRenderPipelineStateWithDescriptor:td compilerTaskOptions:nil error:&err] : nil;
    if (!tp) { check(0, "7. Metal 4: MTL4TileRenderPipelineDescriptor: %s", say(err)); return; }
    id<MTLTexture> rt = tex(MTLPixelFormatRGBA8Unorm, W, H, 1, MTLTextureUsageRenderTarget);
    id<MTLBuffer> s = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared]; ((float *)s.contents)[0] = 0.5f;
    MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
    id<MTLResidencySet> set = [dev newResidencySetWithDescriptor:rd error:&err];
    [set addAllocation:rt]; [set addAllocation:s]; [set commit];
    id<MTL4CommandQueue> q4 = [dev newMTL4CommandQueue];
    id<MTL4CommandAllocator> al = [dev newCommandAllocator];
    id<MTL4CommandBuffer> cb = [dev newCommandBuffer];
    MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1;
    id<MTL4ArgumentTable> at = [dev newArgumentTableWithDescriptor:ad error:&err];
    if (!set || !q4 || !al || !cb || !at) { check(0, "7. Metal 4: setup (residency set %p queue %p allocator %p table %p): %s", (__bridge void *)set,
                                                  (__bridge void *)q4, (__bridge void *)al, (__bridge void *)at, say(err)); return; }
    [at setAddress:s.gpuAddress atIndex:0];
    [cb beginCommandBufferWithAllocator:al];
    [cb useResidencySet:set];
    MTL4RenderPassDescriptor *p = [MTL4RenderPassDescriptor new];
    p.colorAttachments[0].texture = rt; p.colorAttachments[0].loadAction = MTLLoadActionClear;
    p.colorAttachments[0].clearColor = MTLClearColorMake(1, 0.5, 0.25, 1); p.colorAttachments[0].storeAction = MTLStoreActionStore;
    p.tileWidth = 16; p.tileHeight = 16;
    id<MTL4RenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:p];
    [re setArgumentTable:at atStages:MTLRenderStageTile];
    [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [re endEncoding];
    [cb endCommandBuffer];
    id<MTLSharedEvent> ev = [dev newSharedEvent];
    id<MTL4CommandBuffer> list[1] = { cb };
    [q4 commit:list count:1]; [q4 signalEvent:ev value:1];
    const BOOL done = [ev waitUntilSignaledValue:1 timeoutMS:10000];
    uint8_t px[W * H * 4];
    [rt getBytes:px bytesPerRow:W * 4 fromRegion:MTLRegionMake2D(0, 0, W, H) mipmapLevel:0];
    int bad = 0;
    for (int i = 0; i < W * H; i++) bad += !(close8(px[4 * i], 128, 1) && close8(px[4 * i + 1], 64, 1) && close8(px[4 * i + 2], 32, 1) && px[4 * i + 3] == 255);
    check(done && !bad, "7. Metal 4: MTL4Compiler tile pipeline, MTL4RenderCommandEncoder, tile-stage argument table: %d of %d pixels wrong (pixel 0: %d %d %d %d, want 128 64 32 255)%s",
          bad, W * H, px[0], px[1], px[2], px[3], done ? "" : " - TIMED OUT");
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const BOOL any = argc > 1 && !strcmp(argv[1], "--any");
        const uint64_t nmx = nmx_registry_id();
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL b580 = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (b580 || (any && !dev)) dev = d;
            if (b580) break;
        }
        if (!dev) { fprintf(stderr, "FAIL: no B580 Metal device (--any runs on the first device)\n"); return 1; }
        printf("device: %s (%s), Apple4 (tile shading) %d\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String,
               [dev supportsFamily:MTLGPUFamilyApple4]);
        queue = [dev newCommandQueue];
        NSError *err = nil;
        lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "FAIL: library: %s\n", say(err)); return 2; }
        case_deferred();
        case_explicit();
        case_threadgroup();
        case_msaa();
        case_int_float();
        case_sequence();
        if (@available(macOS 26.0, *)) case_metal4();
        else skip("7. Metal 4 needs macOS 26");
        printf("%d PASS, %d FAIL, %d SKIP\n", npass, nfail, nskip);
        return gpu_error ? 3 : nfail ? 4 : 0;
    }
}
