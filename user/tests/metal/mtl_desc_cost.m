// SPDX-License-Identifier: MIT
// mtl_desc_cost: CPU cost per draw of Metal binding in the plugin's two descriptor models (NVMTL_DESC_MODEL=classic|buffer,
// docs/design/DESCRIPTOR-MODEL.md): many small binds between small draws, the pattern of GL-on-Metal games and of
// WindowServer. Per mode: frames of one render (or compute) encoder each; the encode time per draw (p50 and min over the
// frames), draws per second encoding only and with commit + wait, and the process CPU time per draw (getrusage: user +
// system of every thread, so the plugin's submission threads count too). The image hash must be the same in both models.
//
// Modes (per draw, D draws per frame, one 4x4-px quad each on a 256x256 target):
//   same     nothing bound between draws (the per-draw floor)
//   small    setVertexBytes + setFragmentBytes + setFragmentTexture (1 of 8) + setFragmentSamplerState (1 of 2)
//   bufoff   setVertexBufferOffset + setFragmentBufferOffset into one uniform buffer (a GL uniform ring)
//   tex4     4 fragment textures from 16
//   heavy    4 buffers (offsets), 8 textures, 4 samplers and the bytes, all rebound
//   argbuf   setFragmentBuffer of one of 64 argument buffers (texture + sampler + constant; tier 2 only)
//   pso      three pipelines with different bindings in turn, plus the bytes
//   unread   setFragmentBytes into a slot the pipeline does not read, and setVertexBytes (stage regions kept or not)
//   compute  dispatches: setBuffer offset + setBytes + setTexture, 64 threads each
//   geek     a Geekbench-like compute chain: three kernels in turn (k_ga reads a buffer and writes another, k_gb also
//            samples a texture, k_gc reduces through threadgroup memory), each dispatch with setBuffer at new offsets
//            for its input and output and setBytes for its constants; the output checksum must match across models
//
//   NVMTL_ENABLE=1 ./mtl_desc_cost [-n draws] [-f frames] [--any] [mode...]
//   (the night run: NVMTL_BUNDLE + DYLD_INSERT_LIBRARIES=nvmtl_interpose.dylib, once per model, NVMTL_DESC_STATS=1)
// Output: "cost <mode>: ..." lines and a final "cost total" line. Exit 0, 1 no device, 2 library/pipeline failure, 3 GPU error.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   /* managed textures (the macOS 27 SDK deprecates them) */

static NSString *const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float4 col; };
struct U { uint cell; uint pad0, pad1, pad2; float4 col; };
constant float2 kQ[6] = { float2(0,0), float2(1,0), float2(0,1), float2(1,0), float2(1,1), float2(0,1) };
static float4 cellpos(uint cell, float2 c) {
    float2 o = float2(cell % 64u, (cell / 64u) % 64u) / 64.0;
    float2 p = o + (0.125 + c * 0.75) / 64.0;
    return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
vertex VOut vs_b(uint vid [[vertex_id]], constant U &u [[buffer(0)]]) { VOut o; o.pos = cellpos(u.cell, kQ[vid]); o.col = u.col; return o; }
vertex VOut vs_o(uint vid [[vertex_id]], constant U &u [[buffer(1)]]) { VOut o; o.pos = cellpos(u.cell, kQ[vid]); o.col = u.col; return o; }
fragment float4 fs_small(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 &k [[buffer(0)]]) {
    return t.sample(s, float2(0.5)) * k * in.col;
}
fragment float4 fs_off(VOut in [[stage_in]], constant float4 &k [[buffer(1)]]) { return k * in.col; }
fragment float4 fs_tex4(VOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]],
                        texture2d<float> t2 [[texture(2)]], texture2d<float> t3 [[texture(3)]], sampler s [[sampler(0)]]) {
    float2 c = float2(0.5);
    return (t0.sample(s, c) + t1.sample(s, c) + t2.sample(s, c) + t3.sample(s, c)) * 0.25 * in.col;
}
fragment float4 fs_heavy(VOut in [[stage_in]], array<texture2d<float>, 8> t [[texture(0)]], sampler s0 [[sampler(0)]], sampler s1 [[sampler(1)]],
                         sampler s2 [[sampler(2)]], sampler s3 [[sampler(3)]], constant float4 &k [[buffer(0)]], constant float4 &b1 [[buffer(1)]],
                         constant float4 &b2 [[buffer(2)]], constant float4 &b3 [[buffer(3)]], constant float4 &b4 [[buffer(4)]]) {
    float2 c = float2(0.5); float4 a = 0;
    for (uint i = 0; i < 8; i++) a += t[i].sample(i & 1 ? s1 : s0, c);
    a += t[0].sample(s2, c) + t[1].sample(s3, c);
    return a * 0.1 * k * (b1 + b2 + b3 + b4) * 0.25 * in.col;
}
struct Args { texture2d<float> t; sampler s; constant float4 *k; };
fragment float4 fs_arg(VOut in [[stage_in]], constant Args &a [[buffer(5)]]) { return a.t.sample(a.s, float2(0.5)) * a.k[0] * in.col; }
fragment float4 fs_p1(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, float2(0.5)) * in.col; }
fragment float4 fs_p2(VOut in [[stage_in]], texture2d<float> t [[texture(1)]], sampler s [[sampler(1)]], constant float4 &k [[buffer(0)]]) {
    return t.sample(s, float2(0.5)) * k * in.col;
}
fragment float4 fs_p3(VOut in [[stage_in]], constant float4 &k [[buffer(0)]], constant float4 &b [[buffer(2)]]) { return k * b * in.col; }
fragment float4 fs_col(VOut in [[stage_in]]) { return in.col; }
kernel void k_cost(device float4 *out [[buffer(0)]], constant float4 &k [[buffer(1)]], texture2d<float> t [[texture(0)]],
                   uint i [[thread_position_in_grid]]) {
    out[i] = k * t.read(uint2(0, 0)) + float(i);
}
kernel void k_ga(device const float4 *in [[buffer(0)]], device float4 *out [[buffer(1)]], constant float4 &k [[buffer(2)]],
                 uint i [[thread_position_in_grid]]) {
    out[i] = in[i] * k.x + k.y;
}
kernel void k_gb(device const float4 *in [[buffer(0)]], device float4 *out [[buffer(1)]], constant float4 &k [[buffer(2)]],
                 texture2d<float> t [[texture(0)]], uint i [[thread_position_in_grid]]) {
    out[i] = in[i] + k * t.read(uint2(0, 0));
}
kernel void k_gc(device const float4 *in [[buffer(0)]], device float4 *out [[buffer(1)]], constant float4 &k [[buffer(2)]],
                 threadgroup float4 *tg [[threadgroup(0)]], uint i [[thread_position_in_grid]], uint l [[thread_position_in_threadgroup]]) {
    tg[l] = in[i];
    threadgroup_barrier(mem_flags::mem_threadgroup);
    out[i] = (tg[l] + tg[63 - l]) * k.z;
}
)";

enum { RT = 256 };
typedef struct { uint32_t cell, pad0, pad1, pad2; float col[4]; } U;
static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLLibrary> g_lib;
static uint32_t g_draws = 2000, g_frames = 30;
static double g_tick_ns = 1.0;

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0; IORegistryEntryGetRegistryEntryID(s, &id); IOObjectRelease(s);
    return id;
}
static id<MTLRenderPipelineState> rp(NSString *vs, NSString *fs)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [g_lib newFunctionWithName:vs]; d.fragmentFunction = [g_lib newFunctionWithName:fs];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.colorAttachments[0].blendingEnabled = YES;
    d.colorAttachments[0].sourceRGBBlendFactor = MTLBlendFactorOne; d.colorAttachments[0].destinationRGBBlendFactor = MTLBlendFactorOne;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = d.vertexFunction && d.fragmentFunction ? [g_dev newRenderPipelineStateWithDescriptor:d error:&e] : nil;
    if (!p) fprintf(stderr, "pipeline %s/%s: %s\n", vs.UTF8String, fs.UTF8String, e.localizedDescription.UTF8String ?: "no function");
    return p;
}
static id<MTLTexture> solid(uint8_t r, uint8_t g, uint8_t b)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    uint8_t px[16]; for (int i = 0; i < 4; i++) { px[4 * i] = r; px[4 * i + 1] = g; px[4 * i + 2] = b; px[4 * i + 3] = 255; }
    [t replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 withBytes:px bytesPerRow:8];
    return t;
}
static double cpu_ns(void)
{
    struct rusage ru; getrusage(RUSAGE_SELF, &ru);
    return (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1e9 + (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e3;
}
static int cmp_d(const void *a, const void *b) { const double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }

typedef void (^enc_fn)(id<MTLRenderCommandEncoder> e, uint32_t d);
static id<MTLTexture> g_tex[16]; static id<MTLSamplerState> g_samp[4]; static id<MTLBuffer> g_ubuf;
static double g_tot_draws, g_tot_cpu;
static const char *g_modes[16]; static int g_nm;
static BOOL want(const char *m) { if (!g_nm) return YES; for (int i = 0; i < g_nm; i++) if (!strcmp(g_modes[i], m)) return YES; return NO; }

/* one render mode: setup binds what stays, per(e, d) binds before draw d */
static int run_mode(const char *name, id<MTLRenderPipelineState> p0, void (^setup)(id<MTLRenderCommandEncoder>), enc_fn per)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:RT height:RT mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
    id<MTLTexture> rt = [g_dev newTextureWithDescriptor:td];
    id<MTLBuffer> rb = [g_dev newBufferWithLength:RT * RT * 4 options:MTLResourceStorageModeShared];
    double *enc = calloc(g_frames, sizeof *enc), cpu0 = 0, t0 = 0;
    const uint32_t warm = 3;
    for (uint32_t f = 0; f < g_frames + warm; f++) @autoreleasepool {
        if (f == warm) { cpu0 = cpu_ns(); t0 = (double)mach_absolute_time(); }
        MTLRenderPassDescriptor *rpd = [MTLRenderPassDescriptor renderPassDescriptor];
        rpd.colorAttachments[0].texture = rt; rpd.colorAttachments[0].loadAction = MTLLoadActionClear;
        rpd.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1); rpd.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        const uint64_t a = mach_absolute_time();
        id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rpd];
        [e setRenderPipelineState:p0];
        if (setup) setup(e);
        for (uint32_t d = 0; d < g_draws; d++) {
            per(e, d);
            [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        }
        [e endEncoding];
        const uint64_t b = mach_absolute_time();
        if (f >= warm) enc[f - warm] = (double)(b - a) * g_tick_ns / g_draws;
        if (f == g_frames + warm - 1) {
            id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
            [bl copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(RT, RT, 1)
                       toBuffer:rb destinationOffset:0 destinationBytesPerRow:RT * 4 destinationBytesPerImage:RT * RT * 4];
            [bl endEncoding];
        }
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "cost %s: command buffer status %ld\n", name, (long)cb.status); free(enc); return 3; }
    }
    const double wall = ((double)mach_absolute_time() - t0) * g_tick_ns, cpu = cpu_ns() - cpu0, n = (double)g_frames * g_draws;
    qsort(enc, g_frames, sizeof *enc, cmp_d);
    uint32_t h = 2166136261u; const uint8_t *px = rb.contents;
    for (size_t i = 0; i < (size_t)RT * RT * 4; i++) { h ^= px[i]; h *= 16777619u; }
    printf("cost %-8s %u draws x %u frames: encode p50 %.0f ns/draw (min %.0f) = %.2f M draws/s; with commit+wait %.2f M draws/s; "
           "process CPU %.0f ns/draw; image %08x\n", name, g_draws, g_frames, enc[g_frames / 2], enc[0], 1e3 / enc[g_frames / 2],
           n / wall * 1e3, cpu / n, h);
    fflush(stdout);
    g_tot_draws += n; g_tot_cpu += cpu;
    free(enc);
    return 0;
}

static int run_compute(void)
{
    NSError *e = nil;
    id<MTLComputePipelineState> k = [g_dev newComputePipelineStateWithFunction:[g_lib newFunctionWithName:@"k_cost"] error:&e];
    if (!k) { fprintf(stderr, "compute pipeline: %s\n", e.localizedDescription.UTF8String); return 2; }
    id<MTLBuffer> out = [g_dev newBufferWithLength:64 * 16 * 64 options:MTLResourceStorageModePrivate];
    double *enc = calloc(g_frames, sizeof *enc), cpu0 = 0, t0 = 0;
    const uint32_t warm = 3, n = g_draws / 2;
    for (uint32_t f = 0; f < g_frames + warm; f++) @autoreleasepool {
        if (f == warm) { cpu0 = cpu_ns(); t0 = (double)mach_absolute_time(); }
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        const uint64_t a = mach_absolute_time();
        id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
        [c setComputePipelineState:k];
        for (uint32_t d = 0; d < n; d++) {
            const float kv[4] = { (float)(d & 7), 1, 2, 3 };
            [c setBuffer:out offset:(d % 64) * 16 * 64 atIndex:0];
            [c setBytes:kv length:16 atIndex:1];
            [c setTexture:g_tex[d % 16] atIndex:0];
            [c dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
        [c endEncoding];
        const uint64_t b = mach_absolute_time();
        if (f >= warm) enc[f - warm] = (double)(b - a) * g_tick_ns / n;
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "cost compute: command buffer status %ld\n", (long)cb.status); free(enc); return 3; }
    }
    const double wall = ((double)mach_absolute_time() - t0) * g_tick_ns, cpu = cpu_ns() - cpu0, tot = (double)g_frames * n;
    qsort(enc, g_frames, sizeof *enc, cmp_d);
    printf("cost %-8s %u dispatches x %u frames: encode p50 %.0f ns/dispatch (min %.0f) = %.2f M dispatches/s; with commit+wait %.2f M/s; "
           "process CPU %.0f ns/dispatch\n", "compute", n, g_frames, enc[g_frames / 2], enc[0], 1e3 / enc[g_frames / 2], tot / wall * 1e3, cpu / tot);
    g_tot_draws += tot; g_tot_cpu += cpu;
    free(enc);
    return 0;
}

/* geek: per dispatch, kernel d % 3 with its input at slice (d + 7) % 64 and its output at slice d % 64 of one 64 x 64
   float4 buffer, and its constants by setBytes. The checksum of the buffer after the last frame is printed: it must
   be the same in both models (and on Apple, up to float rounding). */
static int run_geek(void)
{
    NSError *e = nil;
    id<MTLComputePipelineState> k[3];
    const char *names[3] = { "k_ga", "k_gb", "k_gc" };
    for (int j = 0; j < 3; j++) {
        k[j] = [g_dev newComputePipelineStateWithFunction:[g_lib newFunctionWithName:@(names[j])] error:&e];
        if (!k[j]) { fprintf(stderr, "geek pipeline %s: %s\n", names[j], e.localizedDescription.UTF8String); return 2; }
    }
    const size_t slice = 64 * 16;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:64 * slice options:MTLResourceStorageModeShared];
    double *enc = calloc(g_frames, sizeof *enc), cpu0 = 0, t0 = 0;
    const uint32_t warm = 3, n = g_draws / 2;
    for (uint32_t f = 0; f < g_frames + warm; f++) @autoreleasepool {
        if (f == warm) { cpu0 = cpu_ns(); t0 = (double)mach_absolute_time(); }
        float *init = (float *)buf.contents;
        for (size_t q = 0; q < 64 * slice / 4; q++) init[q] = (float)(q % 13) * 0.25f;
        id<MTLCommandBuffer> cb = [g_q commandBuffer];
        const uint64_t a = mach_absolute_time();
        id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
        for (uint32_t d = 0; d < n; d++) {
            const uint32_t j = d % 3;
            const float kv[4] = { 0.5f, (float)(d & 3) * 0.125f, 0.25f, 1 };
            [c setComputePipelineState:k[j]];
            [c setBuffer:buf offset:((d + 7) % 64) * slice atIndex:0];
            [c setBuffer:buf offset:(d % 64) * slice atIndex:1];
            [c setBytes:kv length:16 atIndex:2];
            if (j == 1) [c setTexture:g_tex[d % 16] atIndex:0];
            if (j == 2) [c setThreadgroupMemoryLength:64 * 16 atIndex:0];
            [c dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        }
        [c endEncoding];
        const uint64_t b = mach_absolute_time();
        if (f >= warm) enc[f - warm] = (double)(b - a) * g_tick_ns / n;
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "cost geek: command buffer status %ld\n", (long)cb.status); free(enc); return 3; }
    }
    const double wall = ((double)mach_absolute_time() - t0) * g_tick_ns, cpu = cpu_ns() - cpu0, tot = (double)g_frames * n;
    double sum = 0; const float *r = (const float *)buf.contents;
    for (size_t q = 0; q < 64 * slice / 4; q++) sum += (double)r[q] * (double)((q % 7) + 1);
    qsort(enc, g_frames, sizeof *enc, cmp_d);
    printf("cost %-8s %u dispatches x %u frames: encode p50 %.0f ns/dispatch (min %.0f) = %.2f M dispatches/s; with commit+wait %.2f M/s; "
           "process CPU %.0f ns/dispatch; checksum %.6g\n", "geek", n, g_frames, enc[g_frames / 2], enc[0], 1e3 / enc[g_frames / 2],
           tot / wall * 1e3, cpu / tot, sum);
    g_tot_draws += tot; g_tot_cpu += cpu;
    free(enc);
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        BOOL any = NO;
        for (int i = 1; i < argc; i++) {
            if (!strcmp(argv[i], "--any")) any = YES;
            else if (!strcmp(argv[i], "-n") && i + 1 < argc) g_draws = (uint32_t)atoi(argv[++i]);
            else if (!strcmp(argv[i], "-f") && i + 1 < argc) g_frames = (uint32_t)atoi(argv[++i]);
            else if (g_nm < 16) g_modes[g_nm++] = argv[i];
        }
        if (g_draws < 16) g_draws = 16;
        if (g_frames < 1) g_frames = 1;
        mach_timebase_info_data_t tb; mach_timebase_info(&tb); g_tick_ns = (double)tb.numer / tb.denom;
        const uint64_t nmx = nmx_registry_id();
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL ours = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (ours || (any && !g_dev)) g_dev = d;
            if (ours) break;
        }
        if (!g_dev && any) g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { fprintf(stderr, "no B580 Metal device (--any: the first device)\n"); return 1; }
        const char *model = getenv("NVMTL_DESC_MODEL");
        printf("device: %s (%s), descriptor model %s\n", g_dev.name.UTF8String, NSStringFromClass([g_dev class]).UTF8String, model && *model ? model : "default");
        g_q = [g_dev newCommandQueue];
        NSError *err = nil;
        g_lib = [g_dev newLibraryWithSource:kSrc options:[MTLCompileOptions new] error:&err];
        if (!g_lib) { fprintf(stderr, "library: %s\n", err.localizedDescription.UTF8String); return 2; }
        for (int i = 0; i < 16; i++) g_tex[i] = solid((uint8_t)(i * 16), (uint8_t)(255 - i * 16), (uint8_t)((i & 3) * 64));
        for (int i = 0; i < 4; i++) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new];
            sd.minFilter = sd.magFilter = (i & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = (i & 2) ? MTLSamplerAddressModeRepeat : MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
            g_samp[i] = [g_dev newSamplerStateWithDescriptor:sd];
        }
        // the uniform ring: 4096 slots of 256 bytes, (cell, colour) per slot
        g_ubuf = [g_dev newBufferWithLength:4096 * 256 options:MTLResourceStorageModeShared];
        for (uint32_t i = 0; i < 4096; i++) {
            U u = { i, 0, 0, 0, { 0.02f, 0.03f * (i & 3), 0.05f, 1 } };
            memcpy((uint8_t *)g_ubuf.contents + i * 256, &u, sizeof u);
            const float k[4] = { 0.5f, 0.6f, 0.7f, 1 }; memcpy((uint8_t *)g_ubuf.contents + i * 256 + 64, k, 16);
        }
        id<MTLRenderPipelineState> pb = rp(@"vs_b", @"fs_small"), po = rp(@"vs_o", @"fs_off"), p4 = rp(@"vs_b", @"fs_tex4"),
            ph = rp(@"vs_b", @"fs_heavy"), pa = rp(@"vs_b", @"fs_arg"), p1 = rp(@"vs_b", @"fs_p1"), p2 = rp(@"vs_b", @"fs_p2"),
            p3 = rp(@"vs_b", @"fs_p3"), pc = rp(@"vs_b", @"fs_col");
        if (!pb || !po || !p4 || !ph || !p1 || !p2 || !p3 || !pc) return 2;
        const BOOL tier2 = g_dev.argumentBuffersSupport == MTLArgumentBuffersTier2 && pa;
        // 64 argument buffers (tier 2 layout: texture id, sampler id, constant pointer)
        id<MTLBuffer> args = nil;
        if (tier2) {
            args = [g_dev newBufferWithLength:64 * 256 options:MTLResourceStorageModeShared];
            for (uint32_t i = 0; i < 64; i++) {
                struct { MTLResourceID t, s; uint64_t k; } a = { g_tex[i % 16].gpuResourceID, g_samp[i % 4].gpuResourceID, g_ubuf.gpuAddress + (i * 256) + 64 };
                memcpy((uint8_t *)args.contents + i * 256, &a, sizeof a);
            }
        }
        int rc = 0;
#define U_(d) U u = { (d) % 4096, 0, 0, 0, { 0.02f, 0.03f, 0.05f * ((d) & 1), 1 } }
        if (!rc && want("same")) rc = run_mode("same", pc, ^(id<MTLRenderCommandEncoder> e) { U_(7); [e setVertexBytes:&u length:sizeof u atIndex:0]; },
                                              ^(id<MTLRenderCommandEncoder> e, uint32_t d) { (void)e; (void)d; });
        if (!rc && want("small")) rc = run_mode("small", pb, nil, ^(id<MTLRenderCommandEncoder> e, uint32_t d) {
            U_(d); const float k[4] = { 1, 0.5f, 0.25f, 1 };
            [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:16 atIndex:0];
            [e setFragmentTexture:g_tex[d & 7] atIndex:0]; [e setFragmentSamplerState:g_samp[d & 1] atIndex:0]; });
        if (!rc && want("bufoff")) rc = run_mode("bufoff", po, ^(id<MTLRenderCommandEncoder> e) {
            [e setVertexBuffer:g_ubuf offset:0 atIndex:1]; [e setFragmentBuffer:g_ubuf offset:64 atIndex:1]; },
            ^(id<MTLRenderCommandEncoder> e, uint32_t d) { [e setVertexBufferOffset:(d % 4096) * 256 atIndex:1]; [e setFragmentBufferOffset:(d % 4096) * 256 + 64 atIndex:1]; });
        if (!rc && want("tex4")) rc = run_mode("tex4", p4, ^(id<MTLRenderCommandEncoder> e) { U_(3); [e setVertexBytes:&u length:sizeof u atIndex:0];
            [e setFragmentSamplerState:g_samp[0] atIndex:0]; },
            ^(id<MTLRenderCommandEncoder> e, uint32_t d) { U_(d); [e setVertexBytes:&u length:sizeof u atIndex:0];
                for (uint32_t i = 0; i < 4; i++) [e setFragmentTexture:g_tex[(d + i * 5) & 15] atIndex:i]; });
        if (!rc && want("heavy")) rc = run_mode("heavy", ph, nil, ^(id<MTLRenderCommandEncoder> e, uint32_t d) {
            U_(d); const float k[4] = { 1, 1, 1, 1 };
            [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:16 atIndex:0];
            for (uint32_t i = 1; i <= 4; i++) [e setFragmentBuffer:g_ubuf offset:((d + i) % 4096) * 256 + 64 atIndex:i];
            for (uint32_t i = 0; i < 8; i++) [e setFragmentTexture:g_tex[(d + i) & 15] atIndex:i];
            for (uint32_t i = 0; i < 4; i++) [e setFragmentSamplerState:g_samp[(d + i) & 3] atIndex:i]; });
        if (!rc && want("argbuf")) {
            if (!tier2) printf("cost argbuf   SKIP (argument buffers tier 2 needed)\n");
            else rc = run_mode("argbuf", pa, ^(id<MTLRenderCommandEncoder> e) {
                for (int i = 0; i < 16; i++) [e useResource:g_tex[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
                [e useResource:g_ubuf usage:MTLResourceUsageRead stages:MTLRenderStageFragment]; },
                ^(id<MTLRenderCommandEncoder> e, uint32_t d) { U_(d); [e setVertexBytes:&u length:sizeof u atIndex:0];
                    [e setFragmentBuffer:args offset:(d & 63) * 256 atIndex:5]; });
        }
        if (!rc && want("pso")) rc = run_mode("pso", p1, ^(id<MTLRenderCommandEncoder> e) {
            [e setFragmentTexture:g_tex[1] atIndex:0]; [e setFragmentTexture:g_tex[2] atIndex:1];
            [e setFragmentSamplerState:g_samp[0] atIndex:0]; [e setFragmentSamplerState:g_samp[1] atIndex:1];
            [e setFragmentBuffer:g_ubuf offset:64 atIndex:2]; },
            ^(id<MTLRenderCommandEncoder> e, uint32_t d) { U_(d); const float k[4] = { 1, 0.5f, 0.5f, 1 };
                [e setRenderPipelineState:d % 3 == 0 ? p1 : d % 3 == 1 ? p2 : p3];
                [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:16 atIndex:0]; });
        if (!rc && want("unread")) rc = run_mode("unread", p1, ^(id<MTLRenderCommandEncoder> e) {
            [e setFragmentTexture:g_tex[3] atIndex:0]; [e setFragmentSamplerState:g_samp[0] atIndex:0]; },
            ^(id<MTLRenderCommandEncoder> e, uint32_t d) { U_(d); const float k[4] = { (float)(d & 1), 0, 0, 1 };
                [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:k length:16 atIndex:4]; });
        if (!rc && want("compute")) rc = run_compute();
        if (!rc && want("geek")) rc = run_geek();
#undef U_
        if (!rc) printf("cost total: %.0f draws/dispatches, process CPU %.0f ns each\n", g_tot_draws, g_tot_cpu / (g_tot_draws ? g_tot_draws : 1));
        return rc;
    }
}
