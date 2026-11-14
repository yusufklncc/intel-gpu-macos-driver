// SPDX-License-Identifier: MIT
// mtl_mesh_object: Metal mesh pipelines drawing known patterns, each cell of the target checked against colours computed
// on the CPU (exact 0/255 values, so every device gives the same answer). Run it on the B580 (the plugin's compute
// emulation of the object and mesh stages, nullmoth/plugin/NVMTLLibraryLoad.m "batch 42a", lowered by the translator's
// mesh_lower.rs) and on any Apple GPU as the reference (--any).
//
// Cases:
//   object-rows    object + mesh + fragment: 4 object threadgroups of 2 threads; object threadgroup o writes its payload
//                  (a colour and its row) and launches o mesh threadgroups (0, 1, 2, and 3 as a 1 x 3 grid); mesh
//                  threadgroup j draws a quad in cell (j, row) coloured payload x setMeshBytes tint + a mesh buffer entry
//                  read at j. Cells past an object's grid stay clear (a grid of 0 launches nothing).
//   object-redraw  the same pipeline drawn twice in one encoder over the same cells, the second time with other object
//                  bytes, mesh bytes and a mesh buffer offset (setMeshBufferOffset): every cell shows the second draw's.
//   mesh-grid      a mesh pipeline without an object stage, 4 x 2 mesh threadgroups, one quad each (every threadgroup
//                  writes its own output; with the lowering bug of 13 Nov they all wrote the first one's).
//   mesh-indirect  the same pipeline drawn with drawMeshThreadgroupsWithIndirectBuffer: (3 x 1 x 1 at offset 16).
//
//   NVMTL_ENABLE=1 ./mtl_mesh_object [case...]     the B580 (NVMTLDevice, or the device nvmtl_interpose.dylib gives)
//   ./mtl_mesh_object --any [case...]              the first Metal device when there is no B580 (the reference run)
// A pipeline the device refuses is a FAIL here (mtl_desc_paths "mesh" reports it as SKIP).
// Exit: 0 all passed, 1 no device, 2 library failure, 3 + n: n cases failed.
// Build: xcrun clang -fobjc-arc -O1 -o mtl_mesh_object mtl_mesh_object.m -framework Foundation -framework Metal -framework IOKit
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { W = 64, H = 64, G = 4 };

static NSString *const kSrc = @R"(
#include <metal_stdlib>
using namespace metal;
struct Payload { float4 col; uint row; uint pad[3]; };
struct MV { float4 pos [[position]]; float4 col; };
using M = metal::mesh<MV, void, 4, 2, topology::triangle>;

// object threadgroup o: payload (colour, row) and o mesh threadgroups; the last one as a 1 x 3 grid
[[object]] void obj_rows(object_data Payload &pl [[payload]], mesh_grid_properties mgp, constant float4 *ocol [[buffer(0)]],
                         uint3 otg [[threadgroup_position_in_grid]], uint tid [[thread_index_in_threadgroup]]) {
    if (tid == 0) {
        pl.col = ocol[otg.x]; pl.row = otg.x;
        mgp.set_threadgroups_per_grid(otg.x == 3 ? uint3(1, 3, 1) : uint3(otg.x, 1, 1));
    }
}
// mesh threadgroup (x, y) of its object's grid: a quad in cell (x + y, row)
[[mesh]] void mesh_rows(M m, const object_data Payload &pl [[payload]], constant float4 &tint [[buffer(0)]],
                        device const float4 *add [[buffer(1)]], uint3 mtg [[threadgroup_position_in_grid]],
                        uint tid [[thread_index_in_threadgroup]]) {
    const uint cx = mtg.x + mtg.y, cy = pl.row;
    if (tid < 4) {
        float2 c = float2(tid & 1, tid >> 1);
        float2 p = (float2(cx, cy) + 0.125 + c * 0.75) / 4.0;
        MV v; v.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0); v.col = pl.col * tint + add[cx];
        m.set_vertex(tid, v);
    }
    if (tid < 6) { const uchar idx[6] = { 0, 1, 2, 1, 3, 2 }; m.set_index(tid, idx[tid]); }
    if (tid == 0) m.set_primitive_count(2);
}
// without an object stage: mesh threadgroup (x, y) draws cell (x, y), coloured from its position and setMeshBytes
[[mesh]] void mesh_grid(M m, constant float4 &base [[buffer(0)]], uint3 mtg [[threadgroup_position_in_grid]],
                        uint tid [[thread_index_in_threadgroup]]) {
    if (tid < 4) {
        float2 c = float2(tid & 1, tid >> 1);
        float2 p = (float2(mtg.xy) + 0.125 + c * 0.75) / 4.0;
        MV v; v.pos = float4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);
        v.col = base + float4(mtg.x & 1, mtg.y & 1, (mtg.x >> 1) & 1, 0);
        m.set_vertex(tid, v);
    }
    if (tid < 6) { const uchar idx[6] = { 0, 1, 2, 1, 3, 2 }; m.set_index(tid, idx[tid]); }
    if (tid == 0) m.set_primitive_count(2);
}
fragment float4 fs_mesh(MV in [[stage_in]]) { return in.col; }
)";

static id<MTLDevice> g_dev;
static id<MTLCommandQueue> g_q;
static id<MTLLibrary> g_lib;
static BOOL g_b580;
static int g_pass, g_fail;
static int g_argc; static char **g_argv;

static void report(const char *name, int ok, const char *fmt, ...)
{
    char b[600] = ""; va_list a; va_start(a, fmt); vsnprintf(b, sizeof b, fmt, a); va_end(a);
    printf("%s %-14s %s\n", ok ? "PASS" : "FAIL", name, b);
    if (ok) g_pass++; else g_fail++;
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
static id<MTLBuffer> buf(const void *p, size_t n)
{
    return p ? [g_dev newBufferWithBytes:p length:n options:MTLResourceStorageModeShared]
             : [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
}
static id<MTLRenderPipelineState> mesh_pso(NSString *obj, NSString *mesh, const char *name)
{
    MTLMeshRenderPipelineDescriptor *d = [MTLMeshRenderPipelineDescriptor new];
    if (obj) d.objectFunction = [g_lib newFunctionWithName:obj];
    d.meshFunction = [g_lib newFunctionWithName:mesh]; d.fragmentFunction = [g_lib newFunctionWithName:@"fs_mesh"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (obj) { d.payloadMemoryLength = 32; d.maxTotalThreadsPerObjectThreadgroup = 2; }
    d.maxTotalThreadsPerMeshThreadgroup = 8; d.maxTotalThreadgroupsPerMeshGrid = 8;
    NSError *e = nil;
    id<MTLRenderPipelineState> p = [g_dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
    if (!p) report(name, 0, "pipeline: %s", e.localizedDescription.UTF8String ?: "(no error)");
    return p;
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
/* commits cb with a readback of rt into img (W*H*4); 0 = ok */
static int finish(id<MTLCommandBuffer> cb, id<MTLTexture> rt, uint8_t *img, const char *name)
{
    id<MTLBuffer> rb = buf(NULL, W * H * 4);
    id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder];
    [bl copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
               toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
    [bl endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        report(name, 0, "command buffer status %ld: %s", (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        return -1;
    }
    memcpy(img, rb.contents, W * H * 4);
    return 0;
}
static uint8_t u8(float v) { return v <= 0 ? 0 : v >= 1 ? 255 : (uint8_t)(v * 255.0f + 0.5f); }
/* every cell's centre against want (G x G cells, row-major); one report line */
static void check(const char *name, const uint8_t *img, const uint8_t want[G * G][4], const char *what)
{
    uint32_t bad = 0, first = 0;
    for (uint32_t k = 0; k < G * G; k++) {
        const uint32_t cw = W / G, x = (k % G) * cw + cw / 2, y = (k / G) * cw + cw / 2;
        const uint8_t *p = img + ((size_t)y * W + x) * 4;
        for (int i = 0; i < 4; i++) if (abs((int)p[i] - (int)want[k][i]) > 1) { if (!bad++) first = k; break; }
    }
    if (!bad) { report(name, 1, "%d cells (%s)", G * G, what); return; }
    const uint32_t cw = W / G, x = (first % G) * cw + cw / 2, y = (first / G) * cw + cw / 2;
    const uint8_t *p = img + ((size_t)y * W + x) * 4;
    char map[G * G + G + 1]; uint32_t n = 0;   // which cells are drawn (#) and which are clear (.), row by row
    for (uint32_t k = 0; k < G * G; k++) {
        const uint32_t xx = (k % G) * cw + cw / 2, yy = (k / G) * cw + cw / 2;
        map[n++] = img[((size_t)yy * W + xx) * 4 + 3] ? '#' : '.';
        if (k % G == G - 1) map[n++] = '/';
    }
    map[n] = 0;
    report(name, 0, "%u of %d cells wrong; cell (%u,%u) is %u,%u,%u,%u, want %u,%u,%u,%u; drawn %s (%s)", bad, G * G, first % G, first / G,
           p[0], p[1], p[2], p[3], want[first][0], want[first][1], want[first][2], want[first][3], map, what);
}

/* the object-rows pattern: object threadgroup o launches o mesh threadgroups (the last as 1 x 3) in row o */
static void want_rows(uint8_t want[G * G][4], const float ocol[G][4], const float tint[4], const float add[G][4], uint32_t rows_from, uint32_t rows_to)
{
    for (uint32_t cy = rows_from; cy < rows_to; cy++)
        for (uint32_t cx = 0; cx < G; cx++) {
            uint8_t *w = want[cy * G + cx];
            if (cx >= cy) { memset(w, 0, 4); continue; }
            for (int i = 0; i < 4; i++) w[i] = u8(ocol[cy][i] * tint[i] + add[cx][i]);
        }
}

static void case_object_rows(void)
{
    id<MTLRenderPipelineState> p = mesh_pso(@"obj_rows", @"mesh_rows", "object-rows");
    if (!p) return;
    const float ocol[G][4] = { { 1, 1, 1, 1 }, { 1, 0, 0, 1 }, { 0, 1, 0, 1 }, { 0, 0, 1, 1 } }, tint[4] = { 1, 1, 1, 1 };
    const float add[G][4] = { { 0, 0, 0, 0 }, { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 0, 0 } };
    id<MTLBuffer> ab = buf(add, sizeof add);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4], want[G * G][4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setObjectBytes:ocol length:sizeof ocol atIndex:0];
    [e setMeshBytes:tint length:sizeof tint atIndex:0]; [e setMeshBuffer:ab offset:0 atIndex:1];
    [e drawMeshThreadgroups:MTLSizeMake(G, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(2, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
    [e endEncoding];
    if (finish(cb, rt, img, "object-rows")) return;
    want_rows(want, ocol, tint, add, 0, G);
    check("object-rows", img, want, "per-object grids of 0, 1, 2 and 1 x 3; payload colour and row; mesh bytes and buffer");
}

static void case_object_redraw(void)
{
    id<MTLRenderPipelineState> p = mesh_pso(@"obj_rows", @"mesh_rows", "object-redraw");
    if (!p) return;
    // draw 1 fills rows 0-3 with colours a; draw 2 (other object colours, the mesh buffer at an offset of one entry and a
    // tint that drops green) draws over the same cells: the result must be draw 2's everywhere it draws
    const float ca[G][4] = { { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 }, { 1, 1, 1, 1 } };
    const float cb2[G][4] = { { 0, 0, 0, 1 }, { 0, 1, 1, 1 }, { 1, 1, 0, 1 }, { 1, 0, 1, 1 } }, t1[4] = { 1, 1, 1, 1 }, t2[4] = { 1, 0, 1, 1 };
    const float add[G + 1][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 0, 0, 1, 0 }, { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
    id<MTLBuffer> ab = buf(add, sizeof add);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4], want[G * G][4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setObjectBytes:ca length:sizeof ca atIndex:0];
    [e setMeshBytes:t1 length:sizeof t1 atIndex:0]; [e setMeshBuffer:ab offset:0 atIndex:1];
    [e drawMeshThreadgroups:MTLSizeMake(G, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(2, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
    [e setObjectBytes:cb2 length:sizeof cb2 atIndex:0];
    [e setMeshBytes:t2 length:sizeof t2 atIndex:0]; [e setMeshBufferOffset:16 atIndex:1];
    [e drawMeshThreadgroups:MTLSizeMake(G, 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(2, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
    [e endEncoding];
    if (finish(cb, rt, img, "object-redraw")) return;
    want_rows(want, cb2, t2, (const float (*)[4])add[1], 0, G);
    check("object-redraw", img, want, "the second draw's object bytes, mesh bytes and mesh buffer offset over the first's");
}

static void want_grid(uint8_t want[G * G][4], const float base[4], uint32_t gx, uint32_t gy)
{
    for (uint32_t cy = 0; cy < G; cy++)
        for (uint32_t cx = 0; cx < G; cx++) {
            uint8_t *w = want[cy * G + cx];
            if (cx >= gx || cy >= gy) { memset(w, 0, 4); continue; }
            const float c[4] = { base[0] + (cx & 1), base[1] + (cy & 1), base[2] + ((cx >> 1) & 1), base[3] };
            for (int i = 0; i < 4; i++) w[i] = u8(c[i]);
        }
}

static void case_mesh_grid(void)
{
    id<MTLRenderPipelineState> p = mesh_pso(nil, @"mesh_grid", "mesh-grid");
    if (!p) return;
    const float base[4] = { 0, 0, 0, 1 };
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4], want[G * G][4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setMeshBytes:base length:sizeof base atIndex:0];
    [e drawMeshThreadgroups:MTLSizeMake(4, 2, 1) threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
    [e endEncoding];
    if (finish(cb, rt, img, "mesh-grid")) return;
    want_grid(want, base, 4, 2);
    check("mesh-grid", img, want, "4 x 2 mesh threadgroups, each its own quad");
}

static void case_mesh_indirect(void)
{
    id<MTLRenderPipelineState> p = mesh_pso(nil, @"mesh_grid", "mesh-indirect");
    if (!p) return;
    const float base[4] = { 0, 0, 0, 1 };
    const uint32_t args[8] = { 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 0xdeadbeef, 3, 1, 1, 0 };
    id<MTLBuffer> ib = buf(args, sizeof args);
    id<MTLTexture> rt = newRT(); uint8_t img[W * H * 4], want[G * G][4];
    id<MTLCommandBuffer> cb = [g_q commandBuffer];
    id<MTLRenderCommandEncoder> e = [cb renderCommandEncoderWithDescriptor:pass(rt)];
    [e setRenderPipelineState:p];
    [e setMeshBytes:base length:sizeof base atIndex:0];
    [e drawMeshThreadgroupsWithIndirectBuffer:ib indirectBufferOffset:16 threadsPerObjectThreadgroup:MTLSizeMake(1, 1, 1)
                    threadsPerMeshThreadgroup:MTLSizeMake(8, 1, 1)];
    [e endEncoding];
    if (finish(cb, rt, img, "mesh-indirect")) return;
    want_grid(want, base, 3, 1);
    check("mesh-indirect", img, want, "3 x 1 x 1 mesh threadgroups read from the buffer at offset 16");
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
        printf("device: %s (%s)%s\n", g_dev.name.UTF8String, NSStringFromClass([g_dev class]).UTF8String, g_b580 ? "" : " (reference)");
        if (![g_dev supportsFamily:MTLGPUFamilyMetal3]) { fprintf(stderr, "FAIL: not a Metal 3 device (mesh shaders)\n"); return 1; }
        g_q = [g_dev newCommandQueue];
        NSError *err = nil;
        g_lib = [g_dev newLibraryWithSource:kSrc options:[MTLCompileOptions new] error:&err];
        if (!g_lib) { fprintf(stderr, "FAIL: library: %s\n", err.localizedDescription.UTF8String); return 2; }
        struct { const char *name; void (*f)(void); } cases[] = {
            { "object-rows", case_object_rows }, { "object-redraw", case_object_redraw },
            { "mesh-grid", case_mesh_grid }, { "mesh-indirect", case_mesh_indirect } };
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            if (!wanted(cases[i].name)) continue;
            @autoreleasepool { cases[i].f(); }
        }
        printf("mtl_mesh_object: %d PASS, %d FAIL\n", g_pass, g_fail);
        return g_fail ? 3 + (g_fail > 99 ? 99 : g_fail) : 0;
    }
}
