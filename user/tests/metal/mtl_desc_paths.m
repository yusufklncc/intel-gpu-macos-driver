// SPDX-License-Identifier: MIT
// mtl_desc_paths: every Metal binding path the plugin's two descriptor models (NVMTL_DESC_MODEL=classic|buffer,
// docs/design/DESCRIPTOR-MODEL.md) must carry, checked case by case against values computed on the CPU, PASS/FAIL/SKIP
// per case. Run it in both models on the B580 and on any Apple GPU as the reference (--any): the expected values are
// exact (0/255 colours, integers), so every device must give the same answers.
//
// Cases:
//   vtx-buf-bytes    vertex buffer + setVertexBufferOffset + setVertexBytes
//   frag-bytes       setFragmentBytes per draw (inline uniform data, the bytes ring)
//   bytes-4k         a 4096-byte setFragmentBytes block, its last element read
//   frag-tex-samp    setFragmentTexture / setFragmentSamplerState per draw
//   high-slots       texture 30, sampler 15, buffer 30 in the fragment stage
//   vtx-tex          a texture and sampler read in the vertex stage
//   tex-views        texture views: a mip level, an array slice, a pixel-format reinterpretation, a swizzle
//   pso-switch       pipelines with different bindings alternating while bindings change in between (stage regions,
//                    their reuse and the slots a pipeline does not read)
//   churn            256 draws, random binds of textures, samplers, bytes, buffer offsets and four pipelines between draws,
//                    each draw's colour computed from the bindings it ran with
//   indirect         drawPrimitives:indirectBuffer: and dispatchThreadgroupsWithIndirectBuffer: with per-call bindings
//   cs-buf-bytes     compute: setBuffer + offset + setBytes
//   cs-tex           compute: a sampled texture + sampler, and a storage texture written
//   cs-stex-high     compute: a storage texture at index 70 (bindings past storage texture 63)
//   texel-buf        texture buffers: read in a kernel, and written by one
//   argbuf-t1        an argument buffer filled by MTLArgumentEncoder (texture, sampler, buffer), fragment and kernel
//   argbuf-t2        tier 2: an argument buffer written by the CPU (gpuAddress, gpuResourceID of a texture and a sampler)
//   argbuf-array     tier 2: an array of 8 textures in an argument buffer indexed per draw (bindless heap)
//   argbuf-samplers  tier 2: two samplers in an argument buffer (clamp vs repeat), chosen per thread
//   argbuf-storage   tier 2: a writable texture in an argument buffer (heap storage images)
//   argbuf-texbuf    tier 2: a texture buffer in an argument buffer (heap texel buffers)
//   use-heap         tier 2: textures placed in an MTLHeap, reached only through an argument buffer, useHeap:
//   use-resource     tier 2: a buffer written only through an argument-buffer pointer, useResource:usage:Write
//   icb-render       a CPU-encoded indirect command buffer with per-command pipeline, vertex and fragment buffers
//   icb-compute      a CPU-encoded compute ICB with per-command kernel buffers
//   mesh             object + mesh + fragment with setObjectBytes / setMeshBytes / setMeshBuffer per draw
//   tile             a tile function with setTileBytes (Apple GPU family 4 and up; SKIP elsewhere)
//   tess             tessellation: a post-tessellation vertex function with setVertexBytes + setVertexBufferOffset per
//                    draw and setFragmentBytes, one triangle patch per draw (on the B580 the evaluation stage reads its
//                    buffers through the address table: the vertex table set, set 4, since branch dbuftess)
//   tess-mix         tessellation pipelines (one also reading a texture in the post-tessellation function) and a plain
//                    vertex pipeline alternating on the same vertex slots
//   cs-chain         a Geekbench-like chain: three kernels in turn, 24 dispatches, each with setBuffer at new offsets for
//                    its input and output and setBytes, one reading a texture that changes every 4 dispatches; every
//                    stage of the chain checked. Run it on the B580 also with NVMTL_DB_CS_TABSET=3 (and table-pc off):
//                    the table-set path of a kernel whose module names set 1
//   cs-bufoff        storage buffers read at offsets that cross 256-byte windows (dbufgeek: the buffer model keeps a
//                    kernel's buffer descriptor at its offset's window and pushes the rest): uchar, ushort, uint4,
//                    packed_uint3 (stride 12: left as before) and a 64-bit index, 40 dispatches, every output checked;
//                    uchar at odd offsets and ushort at even ones (below the 4-byte descriptor alignment).
//                    Run it on the B580 also with NVMTL_DB_BUFOFF=0
//
//   NVMTL_ENABLE=1 ./mtl_desc_paths [case...]      the B580 (NVMTLDevice, or the device nvmtl_interpose.dylib gives)
//   ./mtl_desc_paths --any [case...]               the first Metal device when there is no B580 (the reference run)
// Exit: 0 all passed (SKIP allowed), 1 no device, 2 library failure, 3 + n: n cases failed (at most 99).
// Build: xcrun clang -fobjc-arc -O1 -o mtl_desc_paths mtl_desc_paths.m -framework Foundation -framework Metal -framework IOKit
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* managed textures (replaceRegion:) are what a discrete GPU wants; the macOS 27 SDK deprecates the mode on Apple silicon */
#pragma clang diagnostic ignored "-Wdeprecated-declarations"

static NSString *const kCore = @R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float4 col; };
struct CellU { uint cell; uint grid; uint pad0, pad1; float4 col; };
constant float2 kQ[6] = { float2(0,0), float2(1,0), float2(0,1), float2(1,0), float2(1,1), float2(0,1) };
static float4 cellpos(uint cell, uint grid, float2 c) {
    float2 o = float2(cell % grid, cell / grid) / float(grid);
    float2 p = o + (0.125 + c * 0.75) / float(grid);
    return float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
}
vertex VOut vs_cell(uint vid [[vertex_id]], constant CellU &u [[buffer(0)]]) {
    VOut o; o.pos = cellpos(u.cell, u.grid, kQ[vid]); o.col = u.col; return o;
}
// colour = the bytes' colour times a vertex buffer element (setVertexBufferOffset picks it)
vertex VOut vs_vbuf(uint vid [[vertex_id]], constant CellU &u [[buffer(0)]], device const float4 *v [[buffer(1)]]) {
    VOut o; o.pos = cellpos(u.cell, u.grid, kQ[vid]); o.col = u.col * v[0]; return o;
}
vertex VOut vs_tex(uint vid [[vertex_id]], constant CellU &u [[buffer(0)]], texture2d<float> t [[texture(3)]], sampler s [[sampler(2)]]) {
    VOut o; o.pos = cellpos(u.cell, u.grid, kQ[vid]); o.col = t.sample(s, float2(0.5), level(0)); return o;
}
fragment float4 fs_col(VOut in [[stage_in]]) { return in.col; }
fragment float4 fs_bytes(VOut in [[stage_in]], constant float4 &k [[buffer(0)]]) { return k; }
struct Big { float4 v[256]; };
fragment float4 fs_big(VOut in [[stage_in]], constant Big &b [[buffer(1)]]) { return b.v[255]; }
fragment float4 fs_tex(VOut in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { return t.sample(s, float2(0.5)); }
fragment float4 fs_hi(VOut in [[stage_in]], texture2d<float> t [[texture(30)]], sampler s [[sampler(15)]], constant float4 &k [[buffer(30)]]) {
    return t.sample(s, float2(0.5)) * k;
}
fragment float4 fs_arr(VOut in [[stage_in]], texture2d_array<float> t [[texture(0)]], sampler s [[sampler(0)]], constant uint &slice [[buffer(0)]]) {
    return t.sample(s, float2(0.5), slice);
}
// churn pipelines: every channel is 0 or 1 (exact in RGBA8)
fragment float4 fs_ca(VOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], sampler s0 [[sampler(0)]], constant float4 &k [[buffer(0)]]) {
    return float4(t0.sample(s0, float2(0.5)).r, k.g, in.col.b, 1.0);
}
fragment float4 fs_cb(VOut in [[stage_in]], texture2d<float> t0 [[texture(0)]], texture2d<float> t1 [[texture(1)]], sampler s0 [[sampler(0)]],
                      device const float4 *b [[buffer(2)]]) {
    return float4(t1.sample(s0, float2(0.5)).r, b[0].g, t0.sample(s0, float2(0.5)).b, 1.0);
}
fragment float4 fs_cc(VOut in [[stage_in]], texture2d<float> t5 [[texture(5)]], sampler s1 [[sampler(1)]], constant float4 &k [[buffer(0)]],
                      device const float4 *b3 [[buffer(3)]]) {
    return float4(k.r, t5.sample(s1, float2(0.5)).g, b3[0].b, 1.0);
}
fragment float4 fs_cd(VOut in [[stage_in]]) { return float4(in.col.rgb, 1.0); }
// compute
kernel void k_buf(device uint *out [[buffer(0)]], device const uint *in [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    out[i] = in[i] * k + i;
}
kernel void k_tex(texture2d<float, access::sample> src [[texture(0)]], sampler s [[sampler(0)]], texture2d<float, access::write> dst [[texture(1)]],
                  uint2 g [[thread_position_in_grid]]) {
    dst.write(src.sample(s, (float2(g) + 0.5) / float2(dst.get_width(), dst.get_height()), level(0)), g);
}
kernel void k_stexhi(texture2d<float, access::write> dst [[texture(70)]], constant float4 &k [[buffer(0)]], uint2 g [[thread_position_in_grid]]) {
    dst.write(k, g);
}
kernel void k_texbuf(texture_buffer<float, access::read> tb [[texture(2)]], device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    out[i] = tb.read(i);
}
kernel void k_texbuf_w(texture_buffer<float, access::write> tb [[texture(3)]], constant float4 &k [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    tb.write(k * float(i + 1) / 8.0, i);
}
kernel void k_tag(device uint *out [[buffer(0)]], constant uint &tag [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    out[i] = tag * 1000 + i;
}
// argument buffers, tier 1 style (an encoder fills it)
struct ArgsT1 { texture2d<float> t [[id(0)]]; sampler s [[id(1)]]; device const float4 *k [[id(2)]]; };
fragment float4 fs_arg1(VOut in [[stage_in]], constant ArgsT1 &a [[buffer(5)]]) { return a.t.sample(a.s, float2(0.5)) * a.k[0]; }
kernel void k_arg1(constant ArgsT1 &a [[buffer(5)]], device float4 *out [[buffer(0)]], uint i [[thread_position_in_grid]]) {
    out[i] = a.t.sample(a.s, float2(0.5), level(0)) * a.k[0];
}
// ICB
fragment float4 fs_icb(VOut in [[stage_in]], device const float4 *k [[buffer(0)]]) { return k[0]; }
// tessellation: one triangle patch per cell, its corners at (0,0), (1,0), (0.5,1) of the cell (the centre is inside)
[[patch(triangle, 3)]]
vertex VOut vs_tess(uint pid [[patch_id]], float3 bc [[position_in_patch]], constant CellU &u [[buffer(0)]], device const float4 *v [[buffer(1)]]) {
    VOut o; o.pos = cellpos(u.cell, u.grid, bc.x * float2(0, 0) + bc.y * float2(1, 0) + bc.z * float2(0.5, 1)); o.col = u.col * v[0]; return o;
}
[[patch(triangle, 3)]]
vertex VOut vs_tess_tex(uint pid [[patch_id]], float3 bc [[position_in_patch]], constant CellU &u [[buffer(0)]], device const float4 *v [[buffer(1)]],
                        texture2d<float> t [[texture(3)]], sampler s [[sampler(2)]]) {
    VOut o; o.pos = cellpos(u.cell, u.grid, bc.x * float2(0, 0) + bc.y * float2(1, 0) + bc.z * float2(0.5, 1));
    o.col = u.col * v[0] * t.sample(s, float2(0.5), level(0)); return o;
}
fragment float4 fs_mul(VOut in [[stage_in]], constant float4 &k [[buffer(0)]]) { return in.col * k; }
// cs-bufoff: kernels whose input is read at offsets that move across 256-byte windows
kernel void k_bo_u8(device const uchar *in [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    out[i] = uint(in[i]) * 3u + k;
}
kernel void k_bo_u16(device const ushort *in [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    out[i] = uint(in[i]) + k;
}
kernel void k_bo_u4(device const uint4 *in [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    const uint4 v = in[i]; out[i] = v.x ^ (v.y >> 1) ^ (v.z << 1) ^ v.w ^ k;
}
kernel void k_bo_p3(device const packed_uint3 *in [[buffer(0)]], device uint *out [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    const packed_uint3 v = in[i]; out[i] = v.x + v.y * 2u + v.z * 3u + k;
}
kernel void k_bo_u64(device const uint *in [[buffer(0)]], device uint *out [[buffer(1)]], constant ulong &base [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    out[i] = in[base + ulong(i)] + 7u;
}
// cs-chain: a chain of kernels, each reading the segment the one before wrote
kernel void k_ga(device uint *out [[buffer(0)]], device const uint *in [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]]) {
    out[i] = in[i] * 3u + k;
}
kernel void k_gb(device uint *out [[buffer(0)]], device const uint *in [[buffer(1)]], constant uint &k [[buffer(2)]], texture2d<uint> t [[texture(0)]],
                 uint i [[thread_position_in_grid]]) {
    out[i] = in[i] + t.read(uint2(i & 3u, 0)).r + k;
}
kernel void k_gc(device uint *out [[buffer(0)]], device const uint *in [[buffer(1)]], constant uint &k [[buffer(2)]], uint i [[thread_position_in_grid]],
                 uint l [[thread_position_in_threadgroup]]) {
    threadgroup uint sh[64]; sh[l] = in[i]; threadgroup_barrier(mem_flags::mem_threadgroup); out[i] = sh[63u - l] ^ k;
}
)";

static NSString *const kTier2 = @R"(
#include <metal_stdlib>
using namespace metal;
struct VOut { float4 pos [[position]]; float4 col; };
struct ArgsT2 { device float4 *p; texture2d<float> t; sampler s; };
fragment float4 fs_arg2(VOut in [[stage_in]], constant ArgsT2 &a [[buffer(6)]]) { return a.t.sample(a.s, float2(0.5)) * a.p[1]; }
struct Bindless { array<texture2d<float>, 8> t; };
fragment float4 fs_bindless(VOut in [[stage_in]], constant Bindless &b [[buffer(7)]], constant uint &idx [[buffer(8)]], sampler s [[sampler(0)]]) {
    return b.t[idx].sample(s, float2(0.5));
}
kernel void k_bindless(constant Bindless &b [[buffer(0)]], sampler s [[sampler(0)]], device float4 *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    out[i] = b.t[i].sample(s, float2(0.5), level(0));
}
struct SampArgs { texture2d<float> t; array<sampler, 2> s; };
kernel void k_argsamp(constant SampArgs &a [[buffer(0)]], device float4 *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    out[i] = a.t.sample(a.s[i], float2(1.25, 0.5), level(0));
}
struct StoreArgs { texture2d<float, access::write> t; };
kernel void k_argstore(constant StoreArgs &a [[buffer(0)]], constant float4 &k [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    a.t.write(k, g);
}
struct TBArgs { texture_buffer<float, access::read> tb; };
kernel void k_argtb(constant TBArgs &a [[buffer(0)]], device float4 *out [[buffer(1)]], uint i [[thread_position_in_grid]]) {
    out[i] = a.tb.read(i);
}
struct PtrArgs { device uint *out; };
kernel void k_argptr(constant PtrArgs &a [[buffer(0)]], uint i [[thread_position_in_grid]]) { a.out[i] = i * 3 + 1; }
)";

static NSString *const kMesh = @R"(
#include <metal_stdlib>
using namespace metal;
struct Payload { float4 col; uint cell; };
struct MV { float4 pos [[position]]; float4 col; };
using M = metal::mesh<MV, void, 4, 2, topology::triangle>;
[[object]] void obj_main(object_data Payload &pl [[payload]], mesh_grid_properties mgp, constant float4 &ocol [[buffer(0)]],
                         constant uint &cell [[buffer(1)]], uint tid [[thread_index_in_threadgroup]]) {
    if (tid == 0) { pl.col = ocol; pl.cell = cell; mgp.set_threadgroups_per_grid(uint3(1, 1, 1)); }
}
[[mesh]] void mesh_main(M m, const object_data Payload &pl [[payload]], constant float4 &scale [[buffer(0)]],
                        device const float4 *add [[buffer(1)]], uint tid [[thread_index_in_threadgroup]]) {
    if (tid < 4) {
        float2 c = float2(tid & 1, tid >> 1);
        float2 o = float2(pl.cell % 4, pl.cell / 4) / 4.0;
        float2 p = o + (0.125 + c * 0.75) / 4.0;
        MV v; v.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); v.col = pl.col * scale + add[0];
        m.set_vertex(tid, v);
    }
    if (tid < 6) { const uchar idx[6] = { 0, 1, 2, 1, 3, 2 }; m.set_index(tid, idx[tid]); }
    if (tid == 0) m.set_primitive_count(2);
}
fragment float4 fs_mesh(MV in [[stage_in]]) { return in.col; }
)";

static NSString *const kTile = @R"(
#include <metal_stdlib>
using namespace metal;
struct TileCol { half4 c [[color(0)]]; };
kernel void tile_fill(imageblock<TileCol, imageblock_layout_implicit> ib, constant float4 &k [[buffer(0)]],
                      ushort2 lid [[thread_position_in_threadgroup]]) {
    TileCol o; o.c = half4(k); ib.write(o, lid);
}
)";

enum { W = 64, H = 64 };
typedef struct { uint32_t cell, grid, pad0, pad1; float col[4]; } CellU;

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLLibrary> g_lib, g_lib2;
static BOOL g_b580, g_tier2;
static int g_pass, g_fail, g_skip;
static int g_argc; static char **g_argv;

static void report(const char *name, int ok, const char *fmt, ...)
{
    char b[600] = ""; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    printf("%s %-16s %s\n", ok > 0 ? "PASS" : ok == 0 ? "FAIL" : "SKIP", name, b);
    if (ok > 0) g_pass++; else if (ok == 0) g_fail++; else g_skip++;
    fflush(stdout);
}
static BOOL wanted(const char *name)
{
    int any = 0;
    for (int i = 1; i < g_argc; i++) { if (g_argv[i][0] == '-') continue; any = 1; if (!strcmp(g_argv[i], name)) return YES; }
    return !any;
}

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0; IORegistryEntryGetRegistryEntryID(s, &id); IOObjectRelease(s);
    return id;
}

/* ---- helpers ------------------------------------------------------------------------------------------------------ */

static id<MTLFunction> fn(id<MTLLibrary> lib, NSString *n)
{
    id<MTLFunction> f = [lib newFunctionWithName:n];
    if (!f) fprintf(stderr, "no function %s\n", n.UTF8String);
    return f;
}
static id<MTLRenderPipelineState> rpso(id<MTLLibrary> lib, NSString *vs, NSString *fs, BOOL icb)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = fn(g_lib, vs); d.fragmentFunction = fn(lib, fs);
    if (!d.vertexFunction || !d.fragmentFunction) return nil;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.supportIndirectCommandBuffers = icb;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) fprintf(stderr, "render pipeline %s/%s: %s\n", vs.UTF8String, fs.UTF8String, e.localizedDescription.UTF8String);
    return p;
}
static id<MTLComputePipelineState> cpso(id<MTLLibrary> lib, NSString *k, BOOL icb)
{
    id<MTLFunction> f = fn(lib, k); if (!f) return nil;
    MTLComputePipelineDescriptor *d = [MTLComputePipelineDescriptor new];
    d.computeFunction = f; d.supportIndirectCommandBuffers = icb;
    NSError *e = nil;
    id<MTLComputePipelineState> p = [g_dev newComputePipelineStateWithDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
    if (!p) fprintf(stderr, "compute pipeline %s: %s\n", k.UTF8String, e.localizedDescription.UTF8String);
    return p;
}
static id<MTLBuffer> buf(const void *p, size_t n)
{
    return p ? [g_dev newBufferWithBytes:p length:n options:MTLResourceStorageModeShared]
             : [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
}
/* a w x h RGBA8 texture filled with one colour (0..255 each), shared via replaceRegion */
static id<MTLTexture> solid(uint32_t w, uint32_t h, const uint8_t c[4], MTLTextureUsage extra)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | extra; d.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    uint8_t *px = malloc((size_t)w * h * 4);
    for (uint32_t i = 0; i < w * h; i++) memcpy(px + 4 * i, c, 4);
    [t replaceRegion:MTLRegionMake2D(0, 0, w, h) mipmapLevel:0 withBytes:px bytesPerRow:w * 4];
    free(px);
    return t;
}
static id<MTLSamplerState> samp(MTLSamplerAddressMode am, BOOL argbuf)
{
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.minFilter = d.magFilter = MTLSamplerMinMagFilterNearest;
    d.sAddressMode = d.tAddressMode = am;
    d.supportArgumentBuffers = argbuf;
    return [g_dev newSamplerStateWithDescriptor:d];
}
static id<MTLTexture> newRT(void)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:d];
}
static MTLRenderPassDescriptor *pass(id<MTLTexture> rt)
{
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    return rp;
}
/* commits cb with a readback of rt (if any) into img (W*H*4); 0 = ok */
static int finish(id<MTLCommandBuffer> cb, id<MTLTexture> rt, uint8_t *img, const char *name)
{
    id<MTLBuffer> rb = nil;
    if (rt) {
        rb = buf(NULL, W * H * 4);
        id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
        [bl copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                   toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
        [bl endEncoding];
    }
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        report(name, 0, "command buffer status %ld: %s", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return -1;
    }
    if (rt) memcpy(img, rb.contents, W * H * 4);
    return 0;
}
/* the centre pixel of cell k in a grid x grid layout */
static const uint8_t *cellpx(const uint8_t *img, uint32_t k, uint32_t grid)
{
    const uint32_t cw = W / grid, x = (k % grid) * cw + cw / 2, y = (k / grid) * cw + cw / 2;
    return img + ((size_t)y * W + x) * 4;
}
static CellU cellu(uint32_t cell, uint32_t grid, float r, float g, float b, float a)
{
    CellU u = { cell, grid, 0, 0, { r, g, b, a } };
    return u;
}
static int near(const uint8_t *px, const uint8_t want[4]) { for (int i = 0; i < 4; i++) if (abs((int)px[i] - (int)want[i]) > 1) return 0; return 1; }
/* compares n cells against want[k] (RGBA8); reports one line */
static void check_cells(const char *name, const uint8_t *img, uint32_t grid, uint32_t n, const uint8_t (*want)[4], const char *what)
{
    uint32_t bad = 0, first = 0;
    for (uint32_t k = 0; k < n; k++) if (!near(cellpx(img, k, grid), want[k])) { if (!bad++) first = k; }
    if (!bad) { report(name, 1, "%u cells (%s)", n, what); return; }
    const uint8_t *p = cellpx(img, first, grid);
    report(name, 0, "%u of %u cells wrong; cell %u is %u,%u,%u,%u, want %u,%u,%u,%u (%s)", bad, n, first, p[0], p[1], p[2], p[3],
           want[first][0], want[first][1], want[first][2], want[first][3], what);
}
static const uint8_t R_[4] = { 255, 0, 0, 255 }, G_[4] = { 0, 255, 0, 255 }, B_[4] = { 0, 0, 255, 255 }, Wt[4] = { 255, 255, 255, 255 },
                     K_[4] = { 0, 0, 0, 255 };

/* ---- render cases ---------------------------------------------------------------------------------------------------- */

static void case_vtx_buf_bytes(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_vbuf", @"fs_col", NO);
    if (!p) { report("vtx-buf-bytes", 0, "pipeline"); return; }
    const float v[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
    id<MTLBuffer> vb = buf(v, sizeof v);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setVertexBuffer:vb offset:0 atIndex:1];
    uint8_t want[16][4];
    for (uint32_t k = 0; k < 16; k++) {
        const uint32_t sel = (k * 7) % 4; const float s = (k & 1) ? 1.0f : 0.0f;   // bytes colour: white or (1,1,1) masked
        CellU u = cellu(k, 4, 1, s > 0 ? 1 : 0, 1, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setVertexBufferOffset:sel * 16 atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        for (int c = 0; c < 4; c++) want[k][c] = (uint8_t)(255 * v[sel][c] * u.col[c]);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "vtx-buf-bytes")) return;
    check_cells("vtx-buf-bytes", img, 4, 16, want, "setVertexBytes + setVertexBufferOffset per draw");
}

static void case_frag_bytes(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_bytes", NO), pb = rpso(g_lib, @"vs_cell", @"fs_big", NO);
    if (!p || !pb) { report("frag-bytes", 0, "pipeline"); return; }
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    uint8_t want[16][4];
    for (uint32_t k = 0; k < 16; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        const float c[4] = { (float)(k & 1), (float)((k >> 1) & 1), (float)((k >> 2) & 1), 1 };
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentBytes:c length:sizeof c atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        for (int i = 0; i < 4; i++) want[k][i] = (uint8_t)(255 * c[i]);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "frag-bytes")) return;
    check_cells("frag-bytes", img, 4, 16, want, "16 draws, setFragmentBytes each");
    if (!wanted("bytes-4k")) return;
    // a 4 KB block: only the last element differs per draw
    rt = newRT(); cb = [g_q commandBuffer]; e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:pb];
    static float big[256][4];
    for (uint32_t k = 0; k < 4; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        for (int i = 0; i < 255; i++) for (int c = 0; c < 4; c++) big[i][c] = 0.5f;
        const uint8_t *w = k == 0 ? R_ : k == 1 ? G_ : k == 2 ? B_ : Wt;
        for (int c = 0; c < 4; c++) big[255][c] = w[c] / 255.0f;
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentBytes:big length:sizeof big atIndex:1];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    [e endEncoding];
    if (finish(cb, rt, img, "bytes-4k")) return;
    const uint8_t want4[4][4] = { { 255, 0, 0, 255 }, { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 255, 255, 255, 255 } };
    check_cells("bytes-4k", img, 4, 4, want4, "4096-byte setFragmentBytes, element 255");
}

static void case_frag_tex_samp(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_tex", NO);
    if (!p) { report("frag-tex-samp", 0, "pipeline"); return; }
    id<MTLTexture> t[4] = { solid(4, 4, R_, 0), solid(4, 4, G_, 0), solid(4, 4, B_, 0), solid(4, 4, Wt, 0) };
    id<MTLSamplerState> s[2] = { samp(MTLSamplerAddressModeClampToEdge, NO), samp(MTLSamplerAddressModeRepeat, NO) };
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    uint8_t want[16][4];
    for (uint32_t k = 0; k < 16; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:t[(k * 3) % 4] atIndex:0];
        [e setFragmentSamplerState:s[k & 1] atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        const uint8_t *w = (k * 3) % 4 == 0 ? R_ : (k * 3) % 4 == 1 ? G_ : (k * 3) % 4 == 2 ? B_ : Wt;
        memcpy(want[k], w, 4);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "frag-tex-samp")) return;
    check_cells("frag-tex-samp", img, 4, 16, want, "texture and sampler per draw");
}

static void case_high_slots(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_hi", NO);
    if (!p) { report("high-slots", 0, "pipeline"); return; }
    id<MTLTexture> t[2] = { solid(1, 1, Wt, 0), solid(1, 1, G_, 0) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setFragmentSamplerState:s atIndex:15];
    const float k0[4] = { 1, 0, 1, 1 }, k1[4] = { 1, 1, 1, 1 };
    id<MTLBuffer> kb = buf(NULL, 256); memcpy(kb.contents, k0, 16); memcpy((uint8_t *)kb.contents + 128, k1, 16);
    uint8_t want[4][4];
    for (uint32_t k = 0; k < 4; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:t[k >> 1] atIndex:30];
        [e setFragmentBuffer:kb offset:(k & 1) ? 128 : 0 atIndex:30];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        const uint8_t *tc = (k >> 1) ? G_ : Wt; const float *kk = (k & 1) ? k1 : k0;
        for (int c = 0; c < 4; c++) want[k][c] = (uint8_t)(tc[c] * kk[c]);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "high-slots")) return;
    check_cells("high-slots", img, 4, 4, want, "texture 30, sampler 15, buffer 30 + offset");
}

static void case_vtx_tex(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_tex", @"fs_col", NO);
    if (!p) { report("vtx-tex", 0, "pipeline"); return; }
    id<MTLTexture> t[3] = { solid(2, 2, R_, 0), solid(2, 2, G_, 0), solid(2, 2, B_, 0) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setVertexSamplerState:s atIndex:2];
    uint8_t want[6][4];
    for (uint32_t k = 0; k < 6; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setVertexTexture:t[k % 3] atIndex:3];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        memcpy(want[k], k % 3 == 0 ? R_ : k % 3 == 1 ? G_ : B_, 4);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "vtx-tex")) return;
    check_cells("vtx-tex", img, 4, 6, want, "setVertexTexture + setVertexSamplerState");
}

static void case_tex_views(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_tex", NO), pa = rpso(g_lib, @"vs_cell", @"fs_arr", NO);
    if (!p || !pa) { report("tex-views", 0, "pipeline"); return; }
    // a 4x4 texture with 3 mips: level 0 red, level 1 green, level 2 blue; viewable (pixel format view)
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:YES];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModeManaged;
    id<MTLTexture> mip = [g_dev newTextureWithDescriptor:d];
    uint8_t px[64];
    const uint8_t *lc[3] = { R_, G_, B_ };
    for (int l = 0; l < 3; l++) { const int w = 4 >> l; for (int i = 0; i < w * w; i++) memcpy(px + 4 * i, lc[l], 4);
        [mip replaceRegion:MTLRegionMake2D(0, 0, w, w) mipmapLevel:l withBytes:px bytesPerRow:w * 4]; }
    // a 2-slice 2D array: slice 0 green, slice 1 white
    MTLTextureDescriptor *ad = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:2 mipmapped:NO];
    ad.textureType = MTLTextureType2DArray; ad.arrayLength = 2; ad.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    ad.storageMode = MTLStorageModeManaged;
    id<MTLTexture> arr = [g_dev newTextureWithDescriptor:ad];
    for (int i = 0; i < 4; i++) memcpy(px + 4 * i, G_, 4);
    [arr replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:0 withBytes:px bytesPerRow:8 bytesPerImage:16];
    for (int i = 0; i < 4; i++) memcpy(px + 4 * i, Wt, 4);
    [arr replaceRegion:MTLRegionMake2D(0, 0, 2, 2) mipmapLevel:0 slice:1 withBytes:px bytesPerRow:8 bytesPerImage:16];
    id<MTLTexture> v[6] = {
        [mip newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)],
        [mip newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(2, 1) slices:NSMakeRange(0, 1)],
        [mip newTextureViewWithPixelFormat:MTLPixelFormatBGRA8Unorm],                         // red bytes read as BGRA: blue
        [mip newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1)
                                   swizzle:MTLTextureSwizzleChannelsMake(MTLTextureSwizzleBlue, MTLTextureSwizzleGreen, MTLTextureSwizzleRed, MTLTextureSwizzleAlpha)],
        [arr newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(1, 1)],
        [arr newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm textureType:MTLTextureType2DArray levels:NSMakeRange(0, 1) slices:NSMakeRange(1, 1)] };
    for (int i = 0; i < 6; i++) if (!v[i]) { report("tex-views", 0, "view %d not made", i); return; }
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setFragmentSamplerState:s atIndex:0];
    for (uint32_t k = 0; k < 5; k++) {
        CellU u = cellu(k, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentTexture:v[k] atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    // the whole mip texture next to its views (the same image through two descriptors in one pass)
    { CellU u = cellu(5, 4, 0, 0, 0, 1); [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentTexture:mip atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; }
    [e setRenderPipelineState:pa];
    { CellU u = cellu(6, 4, 0, 0, 0, 1); const uint32_t sl = 0; [e setVertexBytes:&u length:sizeof u atIndex:0];
      [e setFragmentTexture:v[5] atIndex:0]; [e setFragmentBytes:&sl length:4 atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; }
    { CellU u = cellu(7, 4, 0, 0, 0, 1); const uint32_t sl = 0; [e setVertexBytes:&u length:sizeof u atIndex:0];
      [e setFragmentTexture:arr atIndex:0]; [e setFragmentBytes:&sl length:4 atIndex:0];
      [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; }
    [e endEncoding];
    if (finish(cb, rt, img, "tex-views")) return;
    // level 0 of a 4x4 at a 16x16-px quad: the sampler has no mip filter, so level 0 (red) for the whole texture
    const uint8_t want[8][4] = { { 0, 255, 0, 255 }, { 0, 0, 255, 255 }, { 0, 0, 255, 255 }, { 0, 0, 255, 255 },
                                 { 255, 255, 255, 255 }, { 255, 0, 0, 255 }, { 255, 255, 255, 255 }, { 0, 255, 0, 255 } };
    check_cells("tex-views", img, 4, 8, want, "mip 1, mip 2, BGRA view, swizzle view, slice view, the base, array view, the base array");
}

/* pipelines with different bindings alternate while bindings change in between */
static void case_pso_switch(void)
{
    id<MTLRenderPipelineState> pa = rpso(g_lib, @"vs_cell", @"fs_ca", NO), pb = rpso(g_lib, @"vs_cell", @"fs_cb", NO),
                               pc = rpso(g_lib, @"vs_cell", @"fs_cc", NO);
    if (!pa || !pb || !pc) { report("pso-switch", 0, "pipeline"); return; }
    id<MTLTexture> tR = solid(1, 1, R_, 0), tB = solid(1, 1, B_, 0), tW = solid(1, 1, Wt, 0), tK = solid(1, 1, K_, 0), tG = solid(1, 1, G_, 0);
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    const float bv[2][4] = { { 0, 1, 0, 1 }, { 0, 0, 1, 1 } };
    id<MTLBuffer> b = buf(bv, sizeof bv);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setFragmentSamplerState:s atIndex:0]; [e setFragmentSamplerState:s atIndex:1];
    const float k1[4] = { 1, 1, 0, 1 }, k0[4] = { 0, 0, 0, 1 };
    uint8_t want[16][4]; uint32_t n = 0;
#define DRAWC(P, R, G, Bc) do { CellU u = cellu(n, 4, 0, 0, 1, 1); [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setRenderPipelineState:P]; \
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6]; want[n][0] = R; want[n][1] = G; want[n][2] = Bc; want[n][3] = 255; n++; } while (0)
    [e setFragmentTexture:tR atIndex:0]; [e setFragmentTexture:tB atIndex:1]; [e setFragmentBytes:k1 length:16 atIndex:0];
    [e setFragmentBuffer:b offset:0 atIndex:2]; [e setFragmentTexture:tG atIndex:5]; [e setFragmentBuffer:b offset:16 atIndex:3];
    DRAWC(pa, 255, 255, 255);                        // A: t0.r=1, k.g=1, in.col.b=1
    [e setFragmentTexture:tW atIndex:1];             // a slot A does not read
    DRAWC(pa, 255, 255, 255);
    DRAWC(pb, 255, 255, 0);                          // B: t1.r (white) 1, b[0].g 1, t0.b (red) 0
    [e setFragmentBytes:k0 length:16 atIndex:0];     // B does not read buffer 0
    DRAWC(pb, 255, 255, 0);
    DRAWC(pa, 255, 0, 255);                          // A again, with the new bytes: g 0
    [e setFragmentTexture:tB atIndex:0];             // A and B read texture 0
    DRAWC(pb, 255, 255, 255);                        // t0 blue: b 1
    DRAWC(pa, 0, 0, 255);
    DRAWC(pc, 0, 255, 255);                          // C: k.r 0, t5 green g 1, b3 (offset 16) b 1
    [e setFragmentBuffer:b offset:0 atIndex:2]; [e setFragmentBufferOffset:0 atIndex:3];
    DRAWC(pc, 0, 255, 0);                            // b3 now element 0: b 0
    DRAWC(pb, 255, 255, 255);
    [e setFragmentBufferOffset:16 atIndex:2];        // B's b[0] is now element 1: g 0
    DRAWC(pb, 255, 0, 255);
    [e setFragmentTexture:tK atIndex:5];
    DRAWC(pc, 0, 0, 0);
    DRAWC(pa, 0, 0, 255);
    [e setFragmentBytes:k1 length:16 atIndex:0];
    DRAWC(pc, 255, 0, 0);
    DRAWC(pa, 0, 255, 255);
    DRAWC(pb, 255, 0, 255);
#undef DRAWC
    [e endEncoding];
    if (finish(cb, rt, img, "pso-switch")) return;
    check_cells("pso-switch", img, 4, n, want, "3 pipelines, 16 draws, bindings changed between them");
}

/* 256 draws with random binds between them; every channel 0 or 1 */
static void case_churn(void)
{
    id<MTLRenderPipelineState> ps[4] = { rpso(g_lib, @"vs_cell", @"fs_ca", NO), rpso(g_lib, @"vs_cell", @"fs_cb", NO),
                                         rpso(g_lib, @"vs_cell", @"fs_cc", NO), rpso(g_lib, @"vs_vbuf", @"fs_cd", NO) };
    for (int i = 0; i < 4; i++) if (!ps[i]) { report("churn", 0, "pipeline %d", i); return; }
    // 8 textures: every combination of r,g,b in {0,1}
    id<MTLTexture> tex[8]; uint8_t tc[8][4];
    for (int i = 0; i < 8; i++) { tc[i][0] = (i & 1) ? 255 : 0; tc[i][1] = (i & 2) ? 255 : 0; tc[i][2] = (i & 4) ? 255 : 0; tc[i][3] = 255; tex[i] = solid(1, 1, tc[i], 0); }
    id<MTLSamplerState> sm[2] = { samp(MTLSamplerAddressModeClampToEdge, NO), samp(MTLSamplerAddressModeRepeat, NO) };
    float bv[8][4]; for (int i = 0; i < 8; i++) { bv[i][0] = (float)(i & 1); bv[i][1] = (float)((i >> 1) & 1); bv[i][2] = (float)((i >> 2) & 1); bv[i][3] = 1; }
    id<MTLBuffer> b = buf(bv, sizeof bv);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    uint32_t rng = 12345; uint8_t want[256][4];
#define RND() (rng = rng * 1664525u + 1013904223u, rng >> 8)
    int t0 = 0, t1 = 0, t5 = 0, k = 0, o2 = 0, o3 = 0, vo = 0, pi = 0;
    [e setVertexBuffer:b offset:0 atIndex:1]; [e setFragmentBuffer:b offset:0 atIndex:2]; [e setFragmentBuffer:b offset:0 atIndex:3];
    [e setFragmentTexture:tex[0] atIndex:0]; [e setFragmentTexture:tex[0] atIndex:1]; [e setFragmentTexture:tex[0] atIndex:5];
    [e setFragmentSamplerState:sm[0] atIndex:0]; [e setFragmentSamplerState:sm[0] atIndex:1];
    { const float kv[4] = { 0, 0, 0, 1 }; [e setFragmentBytes:kv length:16 atIndex:0]; }
    for (uint32_t d = 0; d < 256; d++) {
        const uint32_t nops = RND() % 4;
        for (uint32_t o = 0; o < nops; o++) switch (RND() % 9) {
            case 0: t0 = RND() % 8; [e setFragmentTexture:tex[t0] atIndex:0]; break;
            case 1: t1 = RND() % 8; [e setFragmentTexture:tex[t1] atIndex:1]; break;
            case 2: t5 = RND() % 8; [e setFragmentTexture:tex[t5] atIndex:5]; break;
            case 3: k = RND() % 8; [e setFragmentBytes:bv[k] length:16 atIndex:0]; break;
            case 4: o2 = RND() % 8; [e setFragmentBufferOffset:o2 * 16 atIndex:2]; break;
            case 5: o3 = RND() % 8; [e setFragmentBuffer:b offset:o3 * 16 atIndex:3]; break;
            case 6: vo = RND() % 8; [e setVertexBufferOffset:vo * 16 atIndex:1]; break;
            case 7: { const uint32_t si = RND() % 2, at = RND() % 2; [e setFragmentSamplerState:sm[si] atIndex:at]; break; }
            default: pi = RND() % 4; break;
        }
        // the vertex colour: bytes (1, 1, b) times the vertex buffer element vo
        const float vb = (float)(RND() % 2);
        CellU u = cellu(d, 16, 1, 1, vb, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setRenderPipelineState:ps[pi]];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        float r, g, bl;
        const float inb = pi == 3 ? vb * bv[vo][2] : vb;
        switch (pi) {
            case 0: r = tc[t0][0] / 255.0f; g = bv[k][1]; bl = inb; break;
            case 1: r = tc[t1][0] / 255.0f; g = bv[o2][1]; bl = tc[t0][2] / 255.0f; break;
            case 2: r = bv[k][0]; g = tc[t5][1] / 255.0f; bl = bv[o3][2]; break;
            default: r = bv[vo][0]; g = bv[vo][1]; bl = inb; break;
        }
        want[d][0] = (uint8_t)(255 * r); want[d][1] = (uint8_t)(255 * g); want[d][2] = (uint8_t)(255 * bl); want[d][3] = 255;
    }
#undef RND
    [e endEncoding];
    if (finish(cb, rt, img, "churn")) return;
    check_cells("churn", img, 16, 256, want, "random binds between 256 draws, 4 pipelines");
}

static void case_indirect(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_bytes", NO);
    id<MTLComputePipelineState> k = cpso(g_lib, @"k_tag", NO);
    if (!p || !k) { report("indirect", 0, "pipeline"); return; }
    const MTLDrawPrimitivesIndirectArguments da = { 6, 1, 0, 0 };
    const MTLDispatchThreadgroupsIndirectArguments ga = { { 2, 1, 1 } };
    id<MTLBuffer> ib = buf(NULL, 64); memcpy(ib.contents, &da, sizeof da); memcpy((uint8_t *)ib.contents + 32, &ga, sizeof ga);
    id<MTLBuffer> out = buf(NULL, 4 * 64 * 2);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k];
    for (uint32_t t = 0; t < 2; t++) {
        const uint32_t tag = 7 + t;
        [c setBuffer:out offset:t * 4 * 64 atIndex:0]; [c setBytes:&tag length:4 atIndex:1];
        [c dispatchThreadgroupsWithIndirectBuffer:ib indirectBufferOffset:32 threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
    [c endEncoding];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    uint8_t want[4][4];
    for (uint32_t i = 0; i < 4; i++) {
        CellU u = cellu(i, 4, 0, 0, 0, 1); const uint8_t *w = i == 0 ? R_ : i == 1 ? G_ : i == 2 ? B_ : Wt;
        const float col[4] = { w[0] / 255.0f, w[1] / 255.0f, w[2] / 255.0f, 1 };
        [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:col length:16 atIndex:0];
        [e drawPrimitives:MTLPrimitiveTypeTriangle indirectBuffer:ib indirectBufferOffset:0];
        memcpy(want[i], w, 4);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "indirect")) return;
    const uint32_t *o = out.contents; uint32_t bad = 0;
    for (uint32_t i = 0; i < 128; i++) if (o[i] != (7 + i / 64) * 1000 + i % 64) bad++;
    uint32_t cbad = 0; for (uint32_t i = 0; i < 4; i++) if (!near(cellpx(img, i, 4), want[i])) cbad++;
    report("indirect", !bad && !cbad, "indirect dispatch: %u of 128 wrong; indirect draws: %u of 4 cells wrong", bad, cbad);
}

static void case_icb_render(void)
{
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_icb", YES), p2 = rpso(g_lib, @"vs_cell", @"fs_icb", YES);
    if (!p || !p2) { report("icb-render", 0, "pipeline"); return; }
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeDraw; d.inheritPipelineState = NO; d.inheritBuffers = NO;
    d.maxVertexBufferBindCount = 1; d.maxFragmentBufferBindCount = 1;
    id<MTLIndirectCommandBuffer> icb = [g_dev newIndirectCommandBufferWithDescriptor:d maxCommandCount:8 options:0];
    if (!icb) { report("icb-render", 0, "no ICB"); return; }
    CellU cells[8]; float cols[8][4]; uint8_t want[8][4];
    for (uint32_t k = 0; k < 8; k++) {
        cells[k] = cellu(k, 4, 0, 0, 0, 1);
        cols[k][0] = (float)(k & 1); cols[k][1] = (float)((k >> 1) & 1); cols[k][2] = (float)((k >> 2) & 1); cols[k][3] = 1;
        for (int c = 0; c < 4; c++) want[k][c] = (uint8_t)(255 * cols[k][c]);
    }
    id<MTLBuffer> cb_ = buf(cells, sizeof cells), colb = buf(cols, sizeof cols);
    for (uint32_t k = 0; k < 8; k++) {
        id<MTLIndirectRenderCommand> c = [icb indirectRenderCommandAtIndex:k];
        [c setRenderPipelineState:(k & 1) ? p2 : p];
        [c setVertexBuffer:cb_ offset:k * sizeof(CellU) atIndex:0];
        [c setFragmentBuffer:colb offset:k * 16 atIndex:0];
        [c drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6 instanceCount:1 baseInstance:0];
    }
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e useResource:cb_ usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
    [e useResource:colb usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [e executeCommandsInBuffer:icb withRange:NSMakeRange(0, 8)];
    [e endEncoding];
    if (finish(cb, rt, img, "icb-render")) return;
    check_cells("icb-render", img, 4, 8, want, "8 CPU-encoded commands, own pipeline and buffers each");
}

/* ---- compute cases --------------------------------------------------------------------------------------------------- */

static void case_cs_buf_bytes(void)
{
    id<MTLComputePipelineState> k = cpso(g_lib, @"k_buf", NO);
    if (!k) { report("cs-buf-bytes", 0, "pipeline"); return; }
    uint32_t in[256]; for (int i = 0; i < 256; i++) in[i] = (uint32_t)i * 2 + 1;
    id<MTLBuffer> ib = buf(in, sizeof in), ob = buf(NULL, 4 * 256);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k];
    for (uint32_t j = 0; j < 4; j++) {   // four dispatches of 64: input offset j*64 elements, multiplier j+2
        const uint32_t m = j + 2;
        [c setBuffer:ob offset:j * 64 * 4 atIndex:0]; [c setBuffer:ib offset:j * 64 * 4 atIndex:1]; [c setBytes:&m length:4 atIndex:2];
        [c dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
    [c endEncoding];
    if (finish(cb, nil, NULL, "cs-buf-bytes")) return;
    const uint32_t *o = ob.contents; uint32_t bad = 0, first = 0;
    for (uint32_t i = 0; i < 256; i++) { const uint32_t want = in[i] * (i / 64 + 2) + i % 64; if (o[i] != want && !bad++) first = i; }
    report("cs-buf-bytes", !bad, "%u of 256 wrong%s", bad, bad ? [NSString stringWithFormat:@" (first %u: %u)", first, o[first]].UTF8String : "");
}

static int read_tex(id<MTLTexture> t, uint32_t w, uint32_t h, uint8_t *out, const char *name)
{
    id<MTLBuffer> rb = buf(NULL, (size_t)w * h * 4);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
               toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
    [bl endEncoding];
    if (finish(cb, nil, NULL, name)) return -1;
    memcpy(out, rb.contents, (size_t)w * h * 4);
    return 0;
}
static id<MTLTexture> storage_tex(uint32_t w, uint32_t h)
{
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite; d.storageMode = MTLStorageModePrivate;
    return [g_dev newTextureWithDescriptor:d];
}

static void case_cs_tex(void)
{
    id<MTLComputePipelineState> k = cpso(g_lib, @"k_tex", NO), kh = cpso(g_lib, @"k_stexhi", NO);
    if (!k) { report("cs-tex", 0, "pipeline"); return; }
    id<MTLTexture> src[2] = { solid(4, 4, G_, 0), solid(4, 4, B_, 0) };
    id<MTLTexture> dst[2] = { storage_tex(8, 8), storage_tex(8, 8) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setSamplerState:s atIndex:0];
    for (int i = 0; i < 2; i++) {
        [c setTexture:src[i] atIndex:0]; [c setTexture:dst[i] atIndex:1];
        [c dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    }
    [c endEncoding];
    if (finish(cb, nil, NULL, "cs-tex")) return;
    uint8_t px[8 * 8 * 4]; int bad = 0;
    for (int i = 0; i < 2 && !bad; i++) {
        if (read_tex(dst[i], 8, 8, px, "cs-tex")) return;
        for (int j = 0; j < 64; j++) if (!near(px + 4 * j, i ? B_ : G_)) { bad = 1 + i; break; }
    }
    report("cs-tex", !bad, bad ? "storage texture %d wrong" : "sampled -> storage, two dispatches", bad - 1);
    if (!wanted("cs-stex-high")) return;
    if (!kh) { report("cs-stex-high", 0, "pipeline (texture 70)"); return; }
    id<MTLTexture> hi = storage_tex(8, 8);
    cb = [g_q commandBuffer]; c = [cb computeCommandEncoder];
    const float kv[4] = { 1, 0, 1, 1 };
    [c setComputePipelineState:kh]; [c setTexture:hi atIndex:70]; [c setBytes:kv length:16 atIndex:0];
    [c dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "cs-stex-high") || read_tex(hi, 8, 8, px, "cs-stex-high")) return;
    const uint8_t want[4] = { 255, 0, 255, 255 }; bad = 0;
    for (int j = 0; j < 64; j++) if (!near(px + 4 * j, want)) bad++;
    report("cs-stex-high", !bad, "storage texture at index 70: %d of 64 wrong", bad);
}

static void case_texel_buf(void)
{
    id<MTLComputePipelineState> kr = cpso(g_lib, @"k_texbuf", NO), kw = cpso(g_lib, @"k_texbuf_w", NO);
    if (!kr || !kw) { report("texel-buf", 0, "pipeline"); return; }
    float v[16][4]; for (int i = 0; i < 16; i++) for (int c = 0; c < 4; c++) v[i][c] = (float)(i * 4 + c);
    id<MTLBuffer> b = buf(v, sizeof v), wb = buf(NULL, 16 * 16), out = buf(NULL, sizeof v);
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:16
                                                                           resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    id<MTLTexture> tb = [b newTextureWithDescriptor:d offset:0 bytesPerRow:16 * 16];
    MTLTextureDescriptor *dw = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8
                                                                            resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderWrite];
    id<MTLTexture> tw = [wb newTextureWithDescriptor:dw offset:0 bytesPerRow:8 * 16];
    if (!tb || !tw) { report("texel-buf", 0, "texture buffer not made"); return; }
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:kr]; [c setTexture:tb atIndex:2]; [c setBuffer:out offset:0 atIndex:0];
    [c dispatchThreads:MTLSizeMake(16, 1, 1) threadsPerThreadgroup:MTLSizeMake(16, 1, 1)];
    const float kv[4] = { 1, 2, 3, 4 };
    [c setComputePipelineState:kw]; [c setTexture:tw atIndex:3]; [c setBytes:kv length:16 atIndex:0];
    [c dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "texel-buf")) return;
    const float *o = out.contents, *w = wb.contents; int bad = 0, badw = 0;
    for (int i = 0; i < 64; i++) if (o[i] != (float)i) bad++;
    for (int i = 0; i < 8; i++) for (int c2 = 0; c2 < 4; c2++) if (w[i * 4 + c2] != kv[c2] * (float)(i + 1) / 8.0f) badw++;
    report("texel-buf", !bad && !badw, "read %d of 64 wrong, written %d of 32 wrong", bad, badw);
}

static void case_icb_compute(void)
{
    id<MTLComputePipelineState> k = cpso(g_lib, @"k_tag", YES);
    if (!k) { report("icb-compute", 0, "pipeline"); return; }
    MTLIndirectCommandBufferDescriptor *d = [MTLIndirectCommandBufferDescriptor new];
    d.commandTypes = MTLIndirectCommandTypeConcurrentDispatch; d.inheritPipelineState = NO; d.inheritBuffers = NO; d.maxKernelBufferBindCount = 2;
    id<MTLIndirectCommandBuffer> icb = [g_dev newIndirectCommandBufferWithDescriptor:d maxCommandCount:4 options:0];
    if (!icb) { report("icb-compute", 0, "no ICB"); return; }
    id<MTLBuffer> out = buf(NULL, 4 * 64 * 4), tags = buf(NULL, 64);
    for (uint32_t j = 0; j < 4; j++) {
        ((uint32_t *)tags.contents)[j * 4] = 20 + j;
        id<MTLIndirectComputeCommand> c = [icb indirectComputeCommandAtIndex:j];
        [c setComputePipelineState:k];
        [c setKernelBuffer:out offset:j * 64 * 4 atIndex:0]; [c setKernelBuffer:tags offset:j * 16 atIndex:1];
        [c concurrentDispatchThreadgroups:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    }
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c useResource:out usage:MTLResourceUsageWrite]; [c useResource:tags usage:MTLResourceUsageRead];
    [c executeCommandsInBuffer:icb withRange:NSMakeRange(0, 4)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "icb-compute")) return;
    const uint32_t *o = out.contents; uint32_t bad = 0;
    for (uint32_t i = 0; i < 256; i++) if (o[i] != (20 + i / 64) * 1000 + i % 64) bad++;
    report("icb-compute", !bad, "4 commands with their own kernel buffers: %u of 256 wrong", bad);
}

/* ---- argument buffers -------------------------------------------------------------------------------------------- */

static void case_argbuf_t1(void)
{
    id<MTLFunction> ff = fn(g_lib, @"fs_arg1");
    id<MTLRenderPipelineState> p = rpso(g_lib, @"vs_cell", @"fs_arg1", NO);
    id<MTLComputePipelineState> k = cpso(g_lib, @"k_arg1", NO);
    if (!ff || !p || !k) { report("argbuf-t1", 0, "pipeline"); return; }
    id<MTLArgumentEncoder> ae = [ff newArgumentEncoderWithBufferIndex:5];
    if (!ae) { report("argbuf-t1", 0, "no argument encoder"); return; }
    id<MTLTexture> t[2] = { solid(2, 2, Wt, 0), solid(2, 2, G_, 0) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, YES);
    const float kv[2][4] = { { 1, 0, 1, 1 }, { 1, 1, 1, 1 } };
    id<MTLBuffer> kb = buf(kv, sizeof kv);
    const NSUInteger stride = (ae.encodedLength + 255) & ~(NSUInteger)255;
    id<MTLBuffer> ab = buf(NULL, stride * 2);
    for (int i = 0; i < 2; i++) {
        [ae setArgumentBuffer:ab offset:stride * i];
        [ae setTexture:t[i] atIndex:0]; [ae setSamplerState:s atIndex:1]; [ae setBuffer:kb offset:16 * i atIndex:2];
    }
    id<MTLBuffer> out = buf(NULL, 2 * 16);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    for (int i = 0; i < 2; i++) { [e useResource:t[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment]; }
    [e useResource:kb usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    for (uint32_t i = 0; i < 4; i++) {
        CellU u = cellu(i, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentBuffer:ab offset:stride * (i & 1) atIndex:5];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    [e endEncoding];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k];
    for (int i = 0; i < 2; i++) [c useResource:t[i] usage:MTLResourceUsageRead];
    [c useResource:kb usage:MTLResourceUsageRead];
    for (uint32_t i = 0; i < 2; i++) {
        [c setBuffer:ab offset:stride * i atIndex:5]; [c setBuffer:out offset:16 * i atIndex:0];
        [c dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    }
    [c endEncoding];
    if (finish(cb, rt, img, "argbuf-t1")) return;
    const uint8_t want[4][4] = { { 255, 0, 255, 255 }, { 0, 255, 0, 255 }, { 255, 0, 255, 255 }, { 0, 255, 0, 255 } };
    uint32_t bad = 0; for (uint32_t i = 0; i < 4; i++) if (!near(cellpx(img, i, 4), want[i])) bad++;
    const float *o = out.contents; int cbad = 0;
    const float cw[2][4] = { { 1, 0, 1, 1 }, { 0, 1, 0, 1 } };
    for (int i = 0; i < 8; i++) if (o[i] != cw[i / 4][i % 4]) cbad++;
    report("argbuf-t1", !bad && !cbad, "encoder-filled (texture, sampler, buffer): fragment %u of 4 cells wrong, kernel %d of 8 wrong", bad, cbad);
}

typedef struct { uint64_t p; MTLResourceID t, s; } ArgsT2C;

static void case_argbuf_t2(void)
{
    if (!g_tier2) { report("argbuf-t2", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLRenderPipelineState> p = rpso(g_lib2, @"vs_cell", @"fs_arg2", NO);
    if (!p) { report("argbuf-t2", 0, "pipeline"); return; }
    id<MTLTexture> t[2] = { solid(2, 2, Wt, 0), solid(2, 2, B_, 0) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, YES);
    const float kv[4][4] = { { 0 }, { 0, 1, 1, 1 }, { 0 }, { 1, 1, 1, 1 } };
    id<MTLBuffer> kb = buf(kv, sizeof kv);
    ArgsT2C a[2] = { { kb.gpuAddress, t[0].gpuResourceID, s.gpuResourceID }, { kb.gpuAddress + 32, t[1].gpuResourceID, s.gpuResourceID } };
    id<MTLBuffer> ab = buf(a, sizeof a);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e useResource:t[0] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [e useResource:t[1] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    [e useResource:kb usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    for (uint32_t i = 0; i < 4; i++) {
        CellU u = cellu(i, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setFragmentBuffer:ab offset:sizeof(ArgsT2C) * (i & 1) atIndex:6];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    }
    [e endEncoding];
    if (finish(cb, rt, img, "argbuf-t2")) return;
    const uint8_t want[4][4] = { { 0, 255, 255, 255 }, { 0, 0, 255, 255 }, { 0, 255, 255, 255 }, { 0, 0, 255, 255 } };
    check_cells("argbuf-t2", img, 4, 4, want, "CPU-written gpuAddress + texture and sampler gpuResourceID");
}

static void case_argbuf_array(void)
{
    if (!g_tier2) { report("argbuf-array", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLRenderPipelineState> p = rpso(g_lib2, @"vs_cell", @"fs_bindless", NO);
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_bindless", NO);
    if (!p || !k) { report("argbuf-array", 0, "pipeline"); return; }
    id<MTLTexture> t[8]; uint8_t tc[8][4]; MTLResourceID ids[8];
    for (int i = 0; i < 8; i++) { tc[i][0] = (i & 1) ? 255 : 0; tc[i][1] = (i & 2) ? 255 : 0; tc[i][2] = (i & 4) ? 255 : 0; tc[i][3] = 255;
                                  t[i] = solid(1, 1, tc[i], 0); ids[i] = t[i].gpuResourceID; }
    id<MTLBuffer> ab = buf(ids, sizeof ids), out = buf(NULL, 8 * 16);
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p]; [e setFragmentBuffer:ab offset:0 atIndex:7]; [e setFragmentSamplerState:s atIndex:0];
    for (int i = 0; i < 8; i++) [e useResource:t[i] usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
    uint8_t want[16][4];
    for (uint32_t i = 0; i < 16; i++) {
        const uint32_t idx = (i * 5) % 8; CellU u = cellu(i, 4, 0, 0, 0, 1);
        [e setVertexBytes:&u length:sizeof u atIndex:0]; [e setFragmentBytes:&idx length:4 atIndex:8];
        [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        memcpy(want[i], tc[idx], 4);
    }
    [e endEncoding];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setBuffer:ab offset:0 atIndex:0]; [c setSamplerState:s atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
    for (int i = 0; i < 8; i++) [c useResource:t[i] usage:MTLResourceUsageRead];
    [c dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [c endEncoding];
    if (finish(cb, rt, img, "argbuf-array")) return;
    uint32_t bad = 0; for (uint32_t i = 0; i < 16; i++) if (!near(cellpx(img, i, 4), want[i])) bad++;
    const float *o = out.contents; int cbad = 0;
    for (int i = 0; i < 8; i++) for (int c2 = 0; c2 < 4; c2++) if (o[i * 4 + c2] != tc[i][c2] / 255.0f) { cbad++; break; }
    report("argbuf-array", !bad && !cbad, "array<texture2d, 8> by index: fragment %u of 16 cells wrong, kernel %d of 8 wrong", bad, cbad);
}

static void case_argbuf_samplers(void)
{
    if (!g_tier2) { report("argbuf-samplers", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_argsamp", NO);
    if (!k) { report("argbuf-samplers", 0, "pipeline"); return; }
    // a 2x1 texture: red | green. u = 1.25: clamp-to-edge reads green, repeat (0.25) reads red
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:2 height:1 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeManaged;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:d];
    const uint8_t px[8] = { 255, 0, 0, 255, 0, 255, 0, 255 };
    [t replaceRegion:MTLRegionMake2D(0, 0, 2, 1) mipmapLevel:0 withBytes:px bytesPerRow:8];
    id<MTLSamplerState> sc = samp(MTLSamplerAddressModeClampToEdge, YES), sr = samp(MTLSamplerAddressModeRepeat, YES);
    struct { MTLResourceID t, s[2]; } a = { t.gpuResourceID, { sc.gpuResourceID, sr.gpuResourceID } };
    id<MTLBuffer> ab = buf(&a, sizeof a), out = buf(NULL, 32);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setBuffer:ab offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
    [c useResource:t usage:MTLResourceUsageRead];
    [c dispatchThreads:MTLSizeMake(2, 1, 1) threadsPerThreadgroup:MTLSizeMake(2, 1, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "argbuf-samplers")) return;
    const float *o = out.contents;
    const int ok = o[0] == 0 && o[1] == 1 && o[4] == 1 && o[5] == 0;
    report("argbuf-samplers", ok, "clamp read %.0f,%.0f,%.0f (want green), repeat read %.0f,%.0f,%.0f (want red)", o[0], o[1], o[2], o[4], o[5], o[6]);
}

static void case_argbuf_storage(void)
{
    if (!g_tier2) { report("argbuf-storage", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_argstore", NO);
    if (!k) { report("argbuf-storage", 0, "pipeline (the B580's buffer model refused heap storage bindings before 2026-11-12)"); return; }
    id<MTLTexture> t[2] = { storage_tex(8, 8), storage_tex(8, 8) };
    const MTLResourceID ids[2] = { t[0].gpuResourceID, t[1].gpuResourceID };
    id<MTLBuffer> ab = buf(ids, sizeof ids);
    const float kv[2][4] = { { 0, 1, 1, 1 }, { 1, 1, 0, 1 } };
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k];
    for (int i = 0; i < 2; i++) {
        [c useResource:t[i] usage:MTLResourceUsageWrite];
        [c setBuffer:ab offset:8 * i atIndex:0]; [c setBytes:kv[i] length:16 atIndex:1];
        [c dispatchThreads:MTLSizeMake(8, 8, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
    }
    [c endEncoding];
    if (finish(cb, nil, NULL, "argbuf-storage")) return;
    uint8_t px[8 * 8 * 4]; int bad = 0;
    for (int i = 0; i < 2; i++) {
        if (read_tex(t[i], 8, 8, px, "argbuf-storage")) return;
        const uint8_t w[4] = { (uint8_t)(255 * kv[i][0]), (uint8_t)(255 * kv[i][1]), (uint8_t)(255 * kv[i][2]), 255 };
        for (int j = 0; j < 64; j++) if (!near(px + 4 * j, w)) { bad++; break; }
    }
    report("argbuf-storage", !bad, "writable textures through an argument buffer: %d of 2 wrong", bad);
}

static void case_argbuf_texbuf(void)
{
    if (!g_tier2) { report("argbuf-texbuf", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_argtb", NO);
    if (!k) { report("argbuf-texbuf", 0, "pipeline"); return; }
    float v[8][4]; for (int i = 0; i < 8; i++) for (int c = 0; c < 4; c++) v[i][c] = (float)(100 + i * 4 + c);
    id<MTLBuffer> b = buf(v, sizeof v), out = buf(NULL, sizeof v);
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:8
                                                                           resourceOptions:MTLResourceStorageModeShared usage:MTLTextureUsageShaderRead];
    id<MTLTexture> tb = [b newTextureWithDescriptor:d offset:0 bytesPerRow:8 * 16];
    if (!tb) { report("argbuf-texbuf", 0, "texture buffer not made"); return; }
    const MTLResourceID id0 = tb.gpuResourceID;
    id<MTLBuffer> ab = buf(&id0, sizeof id0);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setBuffer:ab offset:0 atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
    [c useResource:tb usage:MTLResourceUsageRead];
    [c dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "argbuf-texbuf")) return;
    const float *o = out.contents; int bad = 0;
    for (int i = 0; i < 32; i++) if (o[i] != (float)(100 + i)) bad++;
    report("argbuf-texbuf", !bad, "texture_buffer through an argument buffer: %d of 32 wrong", bad);
}

static void case_use_heap(void)
{
    if (!g_tier2) { report("use-heap", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_bindless", NO);
    if (!k) { report("use-heap", 0, "pipeline"); return; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    const MTLSizeAndAlign sa = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.size = (sa.size + sa.align) * 8; hd.storageMode = MTLStorageModePrivate;
    id<MTLHeap> heap = [g_dev newHeapWithDescriptor:hd];
    if (!heap) { report("use-heap", 0, "no heap"); return; }
    id<MTLTexture> t[8]; MTLResourceID ids[8]; uint8_t tc[8][4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    for (int i = 0; i < 8; i++) {
        t[i] = [heap newTextureWithDescriptor:d]; if (!t[i]) { [bl endEncoding]; report("use-heap", 0, "heap texture %d", i); return; }
        ids[i] = t[i].gpuResourceID;
        tc[i][0] = (uint8_t)(i * 30); tc[i][1] = (uint8_t)(255 - i * 30); tc[i][2] = (i & 1) ? 255 : 0; tc[i][3] = 255;
        id<MTLTexture> src = solid(4, 4, tc[i], 0);
        [bl copyFromTexture:src toTexture:t[i]];
    }
    [bl endEncoding];
    if (finish(cb, nil, NULL, "use-heap")) return;
    id<MTLBuffer> ab = buf(ids, sizeof ids), out = buf(NULL, 8 * 16);
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setBuffer:ab offset:0 atIndex:0]; [c setSamplerState:s atIndex:0]; [c setBuffer:out offset:0 atIndex:1];
    [c useHeap:heap];
    [c dispatchThreads:MTLSizeMake(8, 1, 1) threadsPerThreadgroup:MTLSizeMake(8, 1, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "use-heap")) return;
    const float *o = out.contents; int bad = 0;
    for (int i = 0; i < 8; i++) for (int c2 = 0; c2 < 4; c2++) if (fabsf(o[i * 4 + c2] - tc[i][c2] / 255.0f) > 0.002f) { bad++; break; }
    report("use-heap", !bad, "8 heap textures reached only through an argument buffer: %d of 8 wrong", bad);
}

static void case_use_resource(void)
{
    if (!g_tier2) { report("use-resource", -1, "argument buffers tier 2 not supported"); return; }
    id<MTLComputePipelineState> k = cpso(g_lib2, @"k_argptr", NO);
    if (!k) { report("use-resource", 0, "pipeline"); return; }
    id<MTLBuffer> target = [g_dev newBufferWithLength:4 * 128 options:MTLResourceStorageModeShared];
    memset(target.contents, 0xff, 4 * 128);
    const uint64_t a = target.gpuAddress + 4 * 64;
    id<MTLBuffer> ab = buf(&a, sizeof a);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    [c setComputePipelineState:k]; [c setBuffer:ab offset:0 atIndex:0];
    [c useResource:target usage:MTLResourceUsageWrite];
    [c dispatchThreads:MTLSizeMake(64, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [c endEncoding];
    if (finish(cb, nil, NULL, "use-resource")) return;
    const uint32_t *o = target.contents; int bad = 0;
    for (int i = 0; i < 64; i++) if (o[i] != 0xffffffffu) bad++;
    for (int i = 0; i < 64; i++) if (o[64 + i] != (uint32_t)i * 3 + 1) bad++;
    report("use-resource", !bad, "a buffer written through an argument-buffer pointer: %d of 128 wrong", bad);
}

/* ---- mesh and tile ----------------------------------------------------------------------------------------------------- */

static void case_mesh(void)
{
    if (@available(macOS 13.0, *)) {} else { report("mesh", -1, "macOS 13 needed"); return; }
    if (![g_dev supportsFamily:MTLGPUFamilyMetal3]) { report("mesh", -1, "not a Metal 3 device"); return; }
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kMesh options:[MTLCompileOptions new] error:&err];
    if (!lib) { report("mesh", 0, "library: %s", err.localizedDescription.UTF8String); return; }
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    d.objectFunction = fn(lib, @"obj_main"); d.meshFunction = fn(lib, @"mesh_main"); d.fragmentFunction = fn(lib, @"fs_mesh");
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.payloadMemoryLength = 32; d.maxTotalThreadsPerObjectThreadgroup = 1; d.maxTotalThreadsPerMeshThreadgroup = 8;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
    if (!p) { report("mesh", g_b580 ? -1 : 0, "pipeline: %s", err.localizedDescription.UTF8String); return; }
    const float add[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 1, 0 } };
    id<MTLBuffer> ab = buf(add, sizeof add);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    uint8_t want[4][4];
    for (uint32_t i = 0; i < 4; i++) {
        const float col[4] = { (float)(i & 1), (float)((i >> 1) & 1), 0, 1 }, scale[4] = { 1, 1, 1, 1 };
        [e setObjectBytes:col length:16 atIndex:0]; [e setObjectBytes:&i length:4 atIndex:1];
        [e setMeshBytes:scale length:16 atIndex:0]; [e setMeshBuffer:ab offset:(i >= 2) ? 16 : 0 atIndex:1];
        [e drawMeshThreadgroups:MTLSizeMake(1, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
        want[i][0] = (i & 1) ? 255 : 0; want[i][1] = ((i >> 1) & 1) ? 255 : 0; want[i][2] = i >= 2 ? 255 : 0; want[i][3] = 255;
    }
    [e endEncoding];
    if (finish(cb, rt, img, "mesh")) return;
    check_cells("mesh", img, 4, 4, want, "object bytes, mesh bytes and a mesh buffer offset per draw");
}

static void case_tile(void)
{
    if (![g_dev supportsFamily:MTLGPUFamilyApple4]) { report("tile", -1, "tile functions need Apple GPU family 4"); return; }
    NSError *err = nil;
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:kTile options:[MTLCompileOptions new] error:&err];
    if (!lib) { report("tile", 0, "library: %s", err.localizedDescription.UTF8String); return; }
    MTLTileRenderPipelineDescriptor *d = [MTLTileRenderPipelineDescriptor new];
    d.tileFunction = fn(lib, @"tile_fill"); d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; d.threadgroupSizeMatchesTileSize = YES;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithTileDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&err];
    if (!p) { report("tile", 0, "pipeline: %s", err.localizedDescription.UTF8String); return; }
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    MTLRenderPassDescriptor *rp = pass(rt); rp.tileWidth = 16; rp.tileHeight = 16;
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:rp];
    const float k[4] = { 0, 1, 1, 1 };
    [e setRenderPipelineState:p]; [e setTileBytes:k length:16 atIndex:0];
    [e dispatchThreadsPerTile:MTLSizeMake(16, 16, 1)];
    [e endEncoding];
    if (finish(cb, rt, img, "tile")) return;
    uint8_t want[16][4]; for (int i = 0; i < 16; i++) { want[i][0] = 0; want[i][1] = 255; want[i][2] = 255; want[i][3] = 255; }
    check_cells("tile", img, 4, 16, want, "setTileBytes, dispatchThreadsPerTile");
}

/* ---- tessellation and the compute chain (branch dbuftess) ----------------------------------------------------------- */

static id<MTLRenderPipelineState> tpso(NSString *vs, NSString *fs)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = fn(g_lib, vs); d.fragmentFunction = fn(g_lib, fs);
    if (!d.vertexFunction || !d.fragmentFunction) return nil;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    d.tessellationPartitionMode = MTLTessellationPartitionModeInteger; d.maxTessellationFactor = 16;
    d.tessellationFactorFormat = MTLTessellationFactorFormatHalf; d.tessellationFactorStepFunction = MTLTessellationFactorStepFunctionPerPatch;
    d.tessellationOutputWindingOrder = MTLWindingCounterClockwise; d.tessellationControlPointIndexType = MTLTessellationControlPointIndexTypeNone;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    if (!p) fprintf(stderr, "tessellation pipeline %s/%s: %s\n", vs.UTF8String, fs.UTF8String, e.localizedDescription.UTF8String);
    return p;
}
/* per-patch factors: 16 triangle patches, every factor 2 (half 2.0 = 0x4000) */
static id<MTLBuffer> tess_factors(void)
{
    uint16_t f[16][4]; for (int i = 0; i < 16; i++) for (int j = 0; j < 4; j++) f[i][j] = 0x4000;
    return buf(f, sizeof f);
}
static void case_tess(void)
{
    id<MTLRenderPipelineState> p = tpso(@"vs_tess", @"fs_mul");
    if (!p) { report("tess", 0, "pipeline"); return; }
    const float v[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
    id<MTLBuffer> vb = buf(v, sizeof v), tf = tess_factors();
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setTessellationFactorBuffer:tf offset:0 instanceStride:0];
    [e setVertexBuffer:vb offset:0 atIndex:1];
    uint8_t want[16][4];
    for (uint32_t k = 0; k < 16; k++) {
        const uint32_t sel = (k * 5) % 4;
        CellU u = cellu(k, 4, 1, (k & 1) ? 1 : 0, 1, 1);
        const float kf[4] = { 1, 1, (k & 2) ? 0.0f : 1.0f, 1 };
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setVertexBufferOffset:sel * 16 atIndex:1];
        [e setFragmentBytes:kf length:16 atIndex:0];
        [e drawPatches:3 patchStart:k patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        for (int c = 0; c < 4; c++) want[k][c] = (uint8_t)(255 * u.col[c] * v[sel][c] * kf[c]);
    }
    [e endEncoding];
    if (finish(cb, rt, img, "tess")) return;
    check_cells("tess", img, 4, 16, want, "patches with setVertexBytes + setVertexBufferOffset + setFragmentBytes per draw");
}
static void case_tess_mix(void)
{
    id<MTLRenderPipelineState> pt = tpso(@"vs_tess", @"fs_mul"), px = tpso(@"vs_tess_tex", @"fs_mul"), pv = rpso(g_lib, @"vs_vbuf", @"fs_mul", NO);
    if (!pt || !px || !pv) { report("tess-mix", 0, "pipeline"); return; }
    const float v[4][4] = { { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 }, { 1, 1, 1, 1 } };
    id<MTLBuffer> vb = buf(v, sizeof v), tf = tess_factors();
    id<MTLTexture> tex[2] = { solid(1, 1, Wt, 0), solid(1, 1, (const uint8_t[4]){ 255, 255, 0, 255 }, 0) };
    id<MTLSamplerState> s = samp(MTLSamplerAddressModeClampToEdge, NO);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setTessellationFactorBuffer:tf offset:0 instanceStride:0];
    [e setVertexBuffer:vb offset:0 atIndex:1];
    [e setVertexSamplerState:s atIndex:2];
    const float kf[4] = { 1, 1, 1, 1 };
    [e setFragmentBytes:kf length:16 atIndex:0];
    uint8_t want[16][4];
    for (uint32_t k = 0; k < 16; k++) {
        const uint32_t kind = k % 3, sel = (k * 3 + 1) % 4, ti = (k / 3) & 1;   // tess, tess + texture, plain vertex
        CellU u = cellu(k, 4, 1, 1, (k & 1) ? 0 : 1, 1);
        [e setRenderPipelineState:kind == 0 ? pt : kind == 1 ? px : pv];
        [e setVertexBytes:&u length:sizeof u atIndex:0];
        [e setVertexBufferOffset:sel * 16 atIndex:1];
        if (kind == 1) [e setVertexTexture:tex[ti] atIndex:3];
        if (kind == 2) [e drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
        else [e drawPatches:3 patchStart:k patchCount:1 patchIndexBuffer:nil patchIndexBufferOffset:0 instanceCount:1 baseInstance:0];
        const uint8_t *tc = kind == 1 ? (ti ? (const uint8_t[4]){ 255, 255, 0, 255 } : Wt) : Wt;
        for (int c = 0; c < 4; c++) want[k][c] = (uint8_t)(255 * u.col[c] * v[sel][c] * (tc[c] / 255.0f));
    }
    [e endEncoding];
    if (finish(cb, rt, img, "tess-mix")) return;
    check_cells("tess-mix", img, 4, 16, want, "tessellation (+ a vertex texture) and vertex pipelines alternating on the same slots");
}
static void case_cs_chain(void)
{
    id<MTLComputePipelineState> k[3] = { cpso(g_lib, @"k_ga", NO), cpso(g_lib, @"k_gb", NO), cpso(g_lib, @"k_gc", NO) };
    if (!k[0] || !k[1] || !k[2]) { report("cs-chain", 0, "pipeline"); return; }
    enum { N = 64, NSEG = 25 };
    static uint32_t seg[NSEG][N];
    for (uint32_t i = 0; i < N; i++) seg[0][i] = i * 2654435761u >> 8;
    id<MTLBuffer> b = buf(seg, sizeof seg);
    id<MTLTexture> tex[2];
    for (int t = 0; t < 2; t++) {   // 4x1 R32Uint, texel x = 1000 * (t + 1) + x
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatR32Uint width:4 height:1 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModeManaged;
        tex[t] = [g_dev newTextureWithDescriptor:d];
        uint32_t px[4]; for (int x = 0; x < 4; x++) px[x] = 1000u * (uint32_t)(t + 1) + (uint32_t)x;
        [tex[t] replaceRegion:MTLRegionMake2D(0, 0, 4, 1) mipmapLevel:0 withBytes:px bytesPerRow:16];
    }
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    for (uint32_t j = 0; j + 1 < NSEG; j++) {
        const uint32_t kk = j * 7u + 1u, ti = (j / 4) & 1;
        [c setComputePipelineState:k[j % 3]];
        [c setBuffer:b offset:(j + 1) * N * 4 atIndex:0]; [c setBuffer:b offset:j * N * 4 atIndex:1];
        [c setBytes:&kk length:4 atIndex:2];
        if (j % 4 == 0) [c setTexture:tex[ti] atIndex:0];
        [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(N, 1, 1)];
        for (uint32_t i = 0; i < N; i++)   // the CPU's answer
            seg[j + 1][i] = j % 3 == 0 ? seg[j][i] * 3u + kk : j % 3 == 1 ? seg[j][i] + 1000u * (ti + 1) + (i & 3u) + kk : seg[j][N - 1 - i] ^ kk;
    }
    [c endEncoding];
    if (finish(cb, nil, NULL, "cs-chain")) return;
    const uint32_t (*o)[N] = (const uint32_t (*)[N])b.contents; uint32_t bad = 0, fj = 0, fi = 0;
    for (uint32_t j = 1; j < NSEG; j++) for (uint32_t i = 0; i < N; i++) if (o[j][i] != seg[j][i] && !bad++) { fj = j; fi = i; }
    report("cs-chain", !bad, "%u of %u wrong, 24 dispatches of 3 kernels%s", bad, (NSEG - 1) * N,
           bad ? [NSString stringWithFormat:@" (first: dispatch %u thread %u: %u, want %u)", fj - 1, fi, o[fj][fi], seg[fj][fi]].UTF8String : "");
}
static void case_cs_bufoff(void)
{
    static NSString *const names[5] = { @"k_bo_u8", @"k_bo_u16", @"k_bo_u4", @"k_bo_p3", @"k_bo_u64" };
    id<MTLComputePipelineState> k[5];
    for (int j = 0; j < 5; j++) if (!(k[j] = cpso(g_lib, names[j], NO))) { report("cs-bufoff", 0, "pipeline %s", names[j].UTF8String); return; }
    enum { N = 64, ND = 40, INB = 32768, OSTEP = 272 };   // OSTEP: each output 16 bytes further into its window than the last
    static uint8_t src[INB]; static uint32_t want[ND][N];
    for (uint32_t q = 0; q < INB; q++) src[q] = (uint8_t)((q * 2654435761u) >> 13);
    id<MTLBuffer> in = buf(src, sizeof src), out = buf(NULL, (size_t)ND * OSTEP + 64);
    memset(out.contents, 0, out.length);
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLComputeCommandEncoder> c = [cb computeCommandEncoder];
    for (uint32_t d = 0; d < ND; d++) {
        const uint32_t kind = d % 5, kk = d * 11u + 5u;
        // uchar at any byte offset, ushort at even ones (their types' alignment: the plugin binds the descriptor 4-aligned
        // and the translator adds the rest), uint4 at 16, the others at 4
        const uint32_t align = kind == 0 ? 1u : kind == 1 ? 2u : kind == 2 ? 16u : 4u, inoff = (d * 1237u) % 28000u / align * align;
        const uint64_t base = d * 3u;
        [c setComputePipelineState:k[kind]];
        [c setBuffer:in offset:inoff atIndex:0]; [c setBuffer:out offset:(size_t)d * OSTEP atIndex:1];
        if (kind == 4) [c setBytes:&base length:8 atIndex:2]; else [c setBytes:&kk length:4 atIndex:2];
        [c dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(N, 1, 1)];
        const uint8_t *p = src + inoff;
        for (uint32_t i = 0; i < N; i++) {
            uint32_t u[4]; memcpy(u, p + (kind == 2 ? 16 * i : kind == 3 ? 12 * i : 4 * (base + i)), 16);
            uint16_t h; memcpy(&h, p + 2 * i, 2);
            want[d][i] = kind == 0 ? p[i] * 3u + kk : kind == 1 ? h + kk : kind == 2 ? u[0] ^ (u[1] >> 1) ^ (u[2] << 1) ^ u[3] ^ kk
                       : kind == 3 ? u[0] + u[1] * 2u + u[2] * 3u + kk : u[0] + 7u;
        }
    }
    [c endEncoding];
    if (finish(cb, nil, NULL, "cs-bufoff")) return;
    uint32_t bad = 0, fd = 0, fi = 0, got = 0;
    for (uint32_t d = 0; d < ND; d++) {
        const uint32_t *o = (const uint32_t *)((const uint8_t *)out.contents + (size_t)d * OSTEP);
        for (uint32_t i = 0; i < N; i++) if (o[i] != want[d][i] && !bad++) { fd = d; fi = i; got = o[i]; }
    }
    report("cs-bufoff", !bad, "%u of %u wrong, %u dispatches of 5 kernels at offsets across 256-byte windows%s", bad, ND * N, ND,
           bad ? [NSString stringWithFormat:@" (first: dispatch %u (%s) thread %u: %u, want %u)", fd, names[fd % 5].UTF8String, fi, got, want[fd][fi]].UTF8String : "");
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        g_argc = argc; g_argv = argv;
        BOOL any = NO; for (int i = 1; i < argc; i++) if (!strcmp(argv[i], "--any")) any = YES;
        const uint64_t nmx = nmx_registry_id();
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL ours = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (ours || (any && !g_dev)) g_dev = d;
            if (ours) break;
        }
        if (!g_dev && any) g_dev = MTLCreateSystemDefaultDevice();
        if (!g_dev) { fprintf(stderr, "FAIL: no B580 Metal device (--any runs on the first device)\n"); return 1; }
        g_b580 = [NSStringFromClass([g_dev class]) isEqualToString:@"NVMTLDevice"] || (nmx && g_dev.registryID == nmx);
        g_tier2 = g_dev.argumentBuffersSupport == MTLArgumentBuffersTier2;
        const char *model = getenv("NVMTL_DESC_MODEL");
        printf("device: %s (%s), argument buffers tier %d%s%s\n", g_dev.name.UTF8String, NSStringFromClass([g_dev class]).UTF8String,
               g_tier2 ? 2 : 1, g_b580 ? ", descriptor model " : " (reference)", g_b580 ? (model && *model ? model : "default") : "");
        g_q = [g_dev newCommandQueue];
        NSError *err = nil;
        g_lib = [g_dev newLibraryWithSource:kCore options:[MTLCompileOptions new] error:&err];
        if (!g_lib) { fprintf(stderr, "FAIL: core library: %s\n", err.localizedDescription.UTF8String); return 2; }
        if (g_tier2) {
            g_lib2 = [g_dev newLibraryWithSource:kTier2 options:[MTLCompileOptions new] error:&err];
            if (!g_lib2) { fprintf(stderr, "FAIL: tier-2 library: %s\n", err.localizedDescription.UTF8String); return 2; }
        }
        struct { const char *name; void (*f)(void); } cases[] = {
            { "vtx-buf-bytes", case_vtx_buf_bytes }, { "frag-bytes", case_frag_bytes }, { "frag-tex-samp", case_frag_tex_samp },
            { "high-slots", case_high_slots }, { "vtx-tex", case_vtx_tex }, { "tex-views", case_tex_views },
            { "pso-switch", case_pso_switch }, { "churn", case_churn }, { "indirect", case_indirect },
            { "cs-buf-bytes", case_cs_buf_bytes }, { "cs-tex", case_cs_tex }, { "texel-buf", case_texel_buf },
            { "argbuf-t1", case_argbuf_t1 }, { "argbuf-t2", case_argbuf_t2 }, { "argbuf-array", case_argbuf_array },
            { "argbuf-samplers", case_argbuf_samplers }, { "argbuf-storage", case_argbuf_storage }, { "argbuf-texbuf", case_argbuf_texbuf },
            { "use-heap", case_use_heap }, { "use-resource", case_use_resource }, { "icb-render", case_icb_render },
            { "icb-compute", case_icb_compute }, { "mesh", case_mesh }, { "tile", case_tile }, { "tess", case_tess },
            { "tess-mix", case_tess_mix }, { "cs-chain", case_cs_chain }, { "cs-bufoff", case_cs_bufoff } };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            // bytes-4k and cs-stex-high run inside frag-bytes and cs-tex
            const BOOL sub = (!strcmp(cases[i].name, "frag-bytes") && wanted("bytes-4k")) || (!strcmp(cases[i].name, "cs-tex") && wanted("cs-stex-high"));
            if (!wanted(cases[i].name) && !sub) continue;
            @autoreleasepool { cases[i].f(); }
        }
        printf("mtl_desc_paths: %d PASS, %d FAIL, %d SKIP\n", g_pass, g_fail, g_skip);
        return g_fail ? 3 + (g_fail > 99 ? 99 : g_fail) : 0;
    }
}
