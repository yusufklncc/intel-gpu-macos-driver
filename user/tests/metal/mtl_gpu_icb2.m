// SPDX-License-Identifier: MIT
// mtl_gpu_icb2: the indirect-command-buffer commands a shader encodes beyond draws (NVMTL_ICB_DGC=1 on the Arc B580,
// docs/design/DESIGN-icb-dgc.md), checked case by case, PASS/FAIL per case.
//
// Render (one pass, a 128x64 target cut into eight 32x32 cells, cell k drawn by one slot with colour k):
//   ICB B (inheritPipelineState NO, inheritBuffers NO), slots 0..5, all written by a kernel:
//     0  set_render_pipeline_state(P), set_vertex_buffer(pos + 0), set_fragment_buffer(col), draw           -> col[0]
//     1  the same with pos + 6 and set_fragment_buffer(col2) (per-slot buffers in one run)              -> col2[1]
//     2  set_render_pipeline_state(Q), another pipeline than the execute binds                           -> B580: refused (black); Apple: col[2]
//     3  set_barrier (from an earlier kernel, encBar), draw                                             -> B580: refused (black); Apple: col[3]
//     4  set_barrier then clear_barrier (encBar), draw                                                   -> col[4]
//        (the B580 refuses a render barrier through the sticky word's bit 1, which clear_barrier clears)
//        (Apple's M1 compiler refuses set_barrier_render_command: there encBar is skipped and 3/4 just draw)
//     5  draw_indexed_primitives through an index buffer no CPU command named (uint16)                  -> col[5]
//   ICB A (inheritPipelineState YES, inheritBuffers YES), slots 0..1, the CPU encodes nothing:
//     6  draw_indexed_primitives (uint32) by address                                                     -> col[6]
//     7  draw_primitives into a slot the CPU never touched (the identity index buffer on the B580)       -> col[7]
// Compute (ICB C: inheritPipelineState NO, inheritBuffers NO, concurrent dispatch), executed twice (two command buffers:
// the B580 learns a threadgroup size a shader asked for in a slot it could not dispatch the first time):
//     C0 the CPU encodes pipeline W, a scratch buffer and a 1x32 dispatch; the kernel sets buffer 0 to out + 0, buffer 1 to
//        the tag 1 and dispatches 2 groups of 32                                                           -> out[0..63] = 1000 + i
//     C1 only the kernel: set_compute_pipeline_state(W), buffers out + 64 and tag 2, 2 groups of 32       -> out[64..127] = 2000 + i
//     C2 only the kernel: concurrent_dispatch_threads(64, 32) (exact), set_barrier, constant tag 3         -> out[128..191] = 3000 + i
//     C3 only the kernel: set_compute_pipeline_state(W2), another pipeline                               -> B580: refused (0); Apple: 9000 + i
//
//   NVMTL_ENABLE=1 ./mtl_gpu_icb2         (the B580; with NVMTL_PROCESS_ENV=1 NVMTL_ICB_DGC=1 for the knob)
//   ./mtl_gpu_icb2 --any                  run on the first Metal device when no B580 is present (reference run)
//
// Exit: 0 pass, 1 no B580 device, 2 compile/pipeline failure (on the B580 without the knob the state commands are refused),
// 3 GPU error, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct VOut { float4 pos [[position]]; uint iid [[flat]]; };\n"
     "vertex VOut vmain(const device float2 *pos [[buffer(0)]], uint vid [[vertex_id]], uint iid [[instance_id]]) {\n"
     "    VOut o; o.pos = float4(pos[vid], 0.0, 1.0); o.iid = iid; return o;\n"
     "}\n"
     "fragment float4 fmain(VOut in [[stage_in]], const device float4 *col [[buffer(0)]]) { return col[in.iid]; }\n"
     "struct RArgs { command_buffer cb [[id(0)]]; render_pipeline_state p [[id(1)]]; render_pipeline_state q [[id(2)]]; };\n"
     "struct AArgs { command_buffer cb [[id(0)]]; };\n"
     // pointers loaded from memory (gpuAddress), as WebKit's helpers load theirs
     "struct Ptrs { device float2 *pos; device float4 *col; device float4 *col2; device ushort *ib16; device uint *ib32; device float2 *pos5; };\n"
     "kernel void encB(device RArgs *a [[buffer(0)]], const device Ptrs *p [[buffer(1)]], uint t [[thread_position_in_grid]]) {\n"
     "    if (t != 0) return;\n"
     "    render_command c0(a->cb, 0); c0.set_render_pipeline_state(a->p); c0.set_vertex_buffer(p->pos, 0); c0.set_fragment_buffer(p->col, 0);\n"
     "    c0.draw_primitives(primitive_type::triangle, 0, 6, 1, 0);\n"
     "    render_command c1(a->cb, 1); c1.set_render_pipeline_state(a->p); c1.set_vertex_buffer(p->pos + 6, 0); c1.set_fragment_buffer(p->col2, 0);\n"
     "    c1.draw_primitives(primitive_type::triangle, 0, 6, 1, 1);\n"
     "    render_command c2(a->cb, 2); c2.set_render_pipeline_state(a->q); c2.set_vertex_buffer(p->pos + 12, 0); c2.set_fragment_buffer(p->col, 0);\n"
     "    c2.draw_primitives(primitive_type::triangle, 0, 6, 1, 2);\n"
     "    render_command c3(a->cb, 3); c3.set_render_pipeline_state(a->p); c3.set_vertex_buffer(p->pos + 18, 0); c3.set_fragment_buffer(p->col, 0);\n"
     "    c3.draw_primitives(primitive_type::triangle, 0, 6, 1, 3);\n"
     "    render_command c4(a->cb, 4); c4.set_render_pipeline_state(a->p); c4.set_vertex_buffer(p->pos + 24, 0); c4.set_fragment_buffer(p->col, 0);\n"
     "    c4.draw_primitives(primitive_type::triangle, 0, 6, 1, 4);\n"
     "    render_command c5(a->cb, 5); c5.set_render_pipeline_state(a->p); c5.set_vertex_buffer(p->pos5, 0); c5.set_fragment_buffer(p->col, 0);\n"
     "    c5.draw_indexed_primitives(primitive_type::triangle, 6, p->ib16, 1, 0, 5);\n"
     "}\n"
     // render barriers in their own kernel, run first: Apple's G13 (M1) compiler refuses set_barrier_render_command
     "kernel void encBar(device RArgs *a [[buffer(0)]], uint t [[thread_position_in_grid]]) {\n"
     "    if (t != 0) return;\n"
     "    render_command c3(a->cb, 3); c3.set_barrier();\n"
     "    render_command c4(a->cb, 4); c4.set_barrier(); c4.clear_barrier();\n"
     "}\n"
     "kernel void encA(device AArgs *a [[buffer(0)]], const device Ptrs *p [[buffer(1)]], uint t [[thread_position_in_grid]]) {\n"
     "    if (t != 0) return;\n"
     "    render_command c0(a->cb, 0); c0.draw_indexed_primitives(primitive_type::triangle, 6, p->ib32 + 6, 1, 0, 6);\n"
     "    render_command c1(a->cb, 1); c1.draw_primitives(primitive_type::triangle, 42, 6, 1, 7);\n"
     "}\n"
     // compute: the writers and the encoder
     "kernel void w(device uint *o [[buffer(0)]], constant uint &tag [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[t] = tag * 1000 + t; }\n"
     "kernel void w2(device uint *o [[buffer(0)]], uint t [[thread_position_in_grid]]) { o[t] = 9000 + t; }\n"
     "struct CArgs { command_buffer cb [[id(0)]]; compute_pipeline_state w [[id(1)]]; compute_pipeline_state w2 [[id(2)]]; };\n"
     "struct CPtrs { device uint *out; device uint *tag1; device uint *tag2; constant uint *tag3; };\n"
     "kernel void encC(device CArgs *a [[buffer(0)]], const device CPtrs *p [[buffer(1)]], uint t [[thread_position_in_grid]]) {\n"
     "    if (t != 0) return;\n"
     "    compute_command c0(a->cb, 0); c0.set_kernel_buffer(p->out, 0); c0.set_kernel_buffer(p->tag1, 1);\n"
     "    c0.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
     "    compute_command c1(a->cb, 1); c1.set_compute_pipeline_state(a->w); c1.set_kernel_buffer(p->out + 64, 0); c1.set_kernel_buffer(p->tag2, 1);\n"
     "    c1.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
     "    compute_command c2(a->cb, 2); c2.set_compute_pipeline_state(a->w); c2.set_kernel_buffer(p->out + 128, 0); c2.set_kernel_buffer(p->tag3, 1);\n"
     "    c2.set_barrier(); c2.concurrent_dispatch_threads(uint3(64, 1, 1), uint3(32, 1, 1));\n"
     "    compute_command c3(a->cb, 3); c3.set_compute_pipeline_state(a->w2); c3.set_kernel_buffer(p->out + 192, 0);\n"
     "    c3.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
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

enum { W = 128, H = 64, CELL = 32, CELLS = 8, OUTN = 256 };

static void cell_rect(unsigned k, float *x0, float *y0, float *x1, float *y1)
{
    const float px0 = (float)(k % 4) * CELL + 4, py0 = (float)(k / 4) * CELL + 4, px1 = px0 + CELL - 8, py1 = py0 + CELL - 8;
    *x0 = px0 / W * 2 - 1; *x1 = px1 / W * 2 - 1;
    *y0 = 1 - py0 / H * 2; *y1 = 1 - py1 / H * 2;
}

// Q differs from P by a blend that changes nothing (one, zero): another pipeline object drawing the same colours
static id<MTLRenderPipelineState> render_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, BOOL blend, NSString *label)
{
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.label = label;
    rd.vertexFunction = [lib newFunctionWithName:@"vmain"];
    rd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    if (blend) {
        rd.colorAttachments[0].blendingEnabled = YES;
        rd.colorAttachments[0].sourceRGBBlendFactor = rd.colorAttachments[0].sourceAlphaBlendFactor = MTLBlendFactorOne;
        rd.colorAttachments[0].destinationRGBBlendFactor = rd.colorAttachments[0].destinationAlphaBlendFactor = MTLBlendFactorZero;
    }
    rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
    rd.supportIndirectCommandBuffers = YES;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
    if (!p) fprintf(stderr, "FAIL: render pipeline %s: %s\n", label.UTF8String, err.localizedDescription.UTF8String);
    return p;
}
static id<MTLComputePipelineState> compute_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name, BOOL icb)
{
    // never hand Metal a nil function (its descriptor asserts): a plugin from before 13 Nov returned nil for an encoder
    // kernel the translator refused without NVMTL_ICB_DGC=1; now it returns the function and the pipeline fails
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) {
        fprintf(stderr, "%s compute function %s: newFunctionWithName returned nil\n", [name isEqualToString:@"encBar"] ? "note:" : "FAIL:",
                name.UTF8String);
        return nil;
    }
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = fn;
    cd.supportIndirectCommandBuffers = icb;
    NSError *err = nil;
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
    if (!p) fprintf(stderr, "%s compute pipeline %s: %s\n", [name isEqualToString:@"encBar"] ? "note:" : "FAIL:", name.UTF8String,
                    err.localizedDescription.UTF8String);
    return p;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);   // the lines before an abort reach the log in order
    @autoreleasepool {
        const BOOL any = argc > 1 && !strcmp(argv[1], "--any");
        const uint64_t nmx = nmx_registry_id();
        id<MTLDevice> dev = nil;
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL ours = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (ours || (any && !dev)) dev = d;
            if (ours) break;
        }
        if (!dev) { fprintf(stderr, "FAIL: no B580 Metal device (--any runs on the first device)\n"); return 1; }
        const BOOL b580 = [NSStringFromClass([dev class]) isEqualToString:@"NVMTLDevice"] || (nmx && dev.registryID == nmx);
        printf("device: %s (%s)%s\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String,
               b580 ? " - expecting the B580's refusals (pipeline switch, render barrier)" : " - expecting Apple's behaviour");

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:[MTLCompileOptions new] error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLRenderPipelineState> P = render_pipeline(dev, lib, NO, @"P");
        id<MTLRenderPipelineState> Q = render_pipeline(dev, lib, YES, @"Q");
        id<MTLFunction> encBFn = [lib newFunctionWithName:@"encB"], encAFn = [lib newFunctionWithName:@"encA"], encCFn = [lib newFunctionWithName:@"encC"];
        id<MTLComputePipelineState> encB = compute_pipeline(dev, lib, @"encB", NO), encA = compute_pipeline(dev, lib, @"encA", NO);
        id<MTLComputePipelineState> encC = compute_pipeline(dev, lib, @"encC", NO);
        id<MTLComputePipelineState> encBar = compute_pipeline(dev, lib, @"encBar", NO);   // nil where render barriers are unsupported
        if (!encBar) printf("render barriers: this device refuses set_barrier_render_command - B3/B4 run without them\n");
        id<MTLComputePipelineState> Wp = compute_pipeline(dev, lib, @"w", YES), W2p = compute_pipeline(dev, lib, @"w2", YES);
        if (!P || !Q || !encB || !encA || !encC || !Wp || !W2p) {
            if (b580) printf("REFUSED: the B580 cannot build the encoder kernels (expected without NVMTL_ICB_DGC=1: the state commands "
                             "have no classic lowering, docs/design/DESIGN-icb-dgc.md) - exit 2\n");
            return 2;
        }

        // geometry: cell k is the triangle list pos[6k .. 6k + 5]
        float pos[CELLS * 6][2];
        for (unsigned k = 0; k < CELLS; k++) {
            float x0, y0, x1, y1; cell_rect(k, &x0, &y0, &x1, &y1);
            const float l[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x0, y1 }, { x1, y0 }, { x1, y1 } };
            memcpy(pos[6 * k], l, sizeof l);
        }
        float col[CELLS][4], col2[CELLS][4];
        for (unsigned k = 0; k < CELLS; k++) {
            const float c[4] = { (k & 1) ? 1.0f : 0.25f, (k & 2) ? 1.0f : 0.25f, (k & 4) ? 1.0f : 0.25f, 1 };
            memcpy(col[k], c, sizeof c);
            const float d[4] = { 1 - c[0] + 0.25f, 0.5f, 1 - c[2] + 0.25f, 1 };
            memcpy(col2[k], d, sizeof d);
        }
        // uint16 indices of cell 5 relative to pos5 (= pos + 30), uint32 junk then cell 6
        const uint16_t i16[6] = { 0, 1, 2, 3, 4, 5 };
        const uint32_t i32[12] = { 7, 7, 7, 7, 7, 7, 36, 37, 38, 39, 40, 41 };
        id<MTLBuffer> posb = [dev newBufferWithBytes:pos length:sizeof pos options:MTLResourceStorageModeShared];
        id<MTLBuffer> colb = [dev newBufferWithBytes:col length:sizeof col options:MTLResourceStorageModeShared];
        id<MTLBuffer> col2b = [dev newBufferWithBytes:col2 length:sizeof col2 options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib16 = [dev newBufferWithBytes:i16 length:sizeof i16 options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib32 = [dev newBufferWithBytes:i32 length:sizeof i32 options:MTLResourceStorageModeShared];
        const uint64_t ptrs[6] = { posb.gpuAddress, colb.gpuAddress, col2b.gpuAddress, ib16.gpuAddress, ib32.gpuAddress, posb.gpuAddress + 30 * 8 };
        id<MTLBuffer> pb = [dev newBufferWithBytes:ptrs length:sizeof ptrs options:MTLResourceStorageModeShared];

        MTLIndirectCommandBufferDescriptor *bd = [MTLIndirectCommandBufferDescriptor new];
        bd.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
        bd.inheritPipelineState = NO; bd.inheritBuffers = NO;
        bd.maxVertexBufferBindCount = 1; bd.maxFragmentBufferBindCount = 1;
        id<MTLIndirectCommandBuffer> icbB = [dev newIndirectCommandBufferWithDescriptor:bd maxCommandCount:6 options:0];
        MTLIndirectCommandBufferDescriptor *ad = [MTLIndirectCommandBufferDescriptor new];
        ad.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
        ad.inheritPipelineState = YES; ad.inheritBuffers = YES;
        id<MTLIndirectCommandBuffer> icbA = [dev newIndirectCommandBufferWithDescriptor:ad maxCommandCount:2 options:0];
        MTLIndirectCommandBufferDescriptor *cdsc = [MTLIndirectCommandBufferDescriptor new];
        cdsc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        cdsc.inheritPipelineState = NO; cdsc.inheritBuffers = NO;
        cdsc.maxKernelBufferBindCount = 2;
        id<MTLIndirectCommandBuffer> icbC = [dev newIndirectCommandBufferWithDescriptor:cdsc maxCommandCount:4 options:0];
        if (!icbA || !icbB || !icbC) { fprintf(stderr, "FAIL: newIndirectCommandBufferWithDescriptor\n"); return 2; }

        id<MTLArgumentEncoder> aeB = [encBFn newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> argB = [dev newBufferWithLength:aeB.encodedLength options:MTLResourceStorageModeShared];
        [aeB setArgumentBuffer:argB offset:0];
        [aeB setIndirectCommandBuffer:icbB atIndex:0];
        [aeB setRenderPipelineState:P atIndex:1];
        [aeB setRenderPipelineState:Q atIndex:2];
        id<MTLArgumentEncoder> aeA = [encAFn newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> argA = [dev newBufferWithLength:aeA.encodedLength options:MTLResourceStorageModeShared];
        [aeA setArgumentBuffer:argA offset:0];
        [aeA setIndirectCommandBuffer:icbA atIndex:0];
        id<MTLArgumentEncoder> aeC = [encCFn newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> argC = [dev newBufferWithLength:aeC.encodedLength options:MTLResourceStorageModeShared];
        [aeC setArgumentBuffer:argC offset:0];
        [aeC setIndirectCommandBuffer:icbC atIndex:0];
        [aeC setComputePipelineState:Wp atIndex:1];
        [aeC setComputePipelineState:W2p atIndex:2];

        // compute: C0 is CPU-encoded first (the kernel then rewrites its buffers and grid)
        id<MTLBuffer> out = [dev newBufferWithLength:OUTN * 4 options:MTLResourceStorageModeShared];
        id<MTLBuffer> scratch = [dev newBufferWithLength:64 * 4 options:MTLResourceStorageModeShared];
        const uint32_t tags[3] = { 1, 2, 3 };
        id<MTLBuffer> tagb = [dev newBufferWithBytes:tags length:sizeof tags options:MTLResourceStorageModeShared];
        const uint64_t cptrs[4] = { out.gpuAddress, tagb.gpuAddress, tagb.gpuAddress + 4, tagb.gpuAddress + 8 };
        id<MTLBuffer> cpb = [dev newBufferWithBytes:cptrs length:sizeof cptrs options:MTLResourceStorageModeShared];
        id<MTLIndirectComputeCommand> cc0 = [icbC indirectComputeCommandAtIndex:0];
        [cc0 setComputePipelineState:Wp];
        [cc0 setKernelBuffer:scratch offset:0 atIndex:0];
        [cc0 setKernelBuffer:tagb offset:0 atIndex:1];
        [cc0 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLBuffer> px = [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandQueue> q = [dev newCommandQueue];

        for (int pass = 0; pass < 2; pass++) {
            memset(out.contents, 0, OUTN * 4);
            id<MTLCommandBuffer> cmd = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
            if (encBar) {
                [ce setComputePipelineState:encBar];
                [ce setBuffer:argB offset:0 atIndex:0];
                [ce useResource:icbB usage:MTLResourceUsageWrite];
                [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            }
            [ce setComputePipelineState:encB];
            [ce setBuffer:argB offset:0 atIndex:0]; [ce setBuffer:pb offset:0 atIndex:1];
            [ce useResource:icbB usage:MTLResourceUsageWrite];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce setComputePipelineState:encA];
            [ce setBuffer:argA offset:0 atIndex:0];
            [ce useResource:icbA usage:MTLResourceUsageWrite];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce setComputePipelineState:encC];
            [ce setBuffer:argC offset:0 atIndex:0]; [ce setBuffer:cpb offset:0 atIndex:1];
            [ce useResource:icbC usage:MTLResourceUsageWrite];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];

            // the compute ICB, in its own encoder after the one that wrote it
            id<MTLComputeCommandEncoder> xe = [cmd computeCommandEncoder];
            [xe setComputePipelineState:Wp];
            [xe useResource:out usage:MTLResourceUsageWrite];
            [xe useResource:tagb usage:MTLResourceUsageRead];
            [xe useResource:scratch usage:MTLResourceUsageWrite];
            [xe useResource:icbC usage:MTLResourceUsageRead];
            [xe executeCommandsInBuffer:icbC withRange:NSMakeRange(0, 4)];
            [xe endEncoding];

            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> re = [cmd renderCommandEncoderWithDescriptor:rp];
            [re setRenderPipelineState:P];   // the pipeline the execute binds for ICB B's slots (and ICB A inherits)
            for (id<MTLBuffer> u in @[ posb, colb, col2b, ib16, ib32 ])
                [re useResource:u usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            [re useResource:icbB usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [re useResource:icbA usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [re executeCommandsInBuffer:icbB withRange:NSMakeRange(0, 6)];
            [re setRenderPipelineState:P];
            [re setVertexBuffer:posb offset:0 atIndex:0];
            [re setFragmentBuffer:colb offset:0 atIndex:0];
            [re executeCommandsInBuffer:icbA withRange:NSMakeRange(0, 2)];
            [re endEncoding];

            id<MTLBlitCommandEncoder> be = [cmd blitCommandEncoder];
            [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                       toBuffer:px destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if (cmd.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "FAIL: command buffer %d status %ld: %s\n", pass, (long)cmd.status, cmd.error.localizedDescription.UTF8String ?: "");
                return 3;
            }
            if (pass == 0) {
                const uint32_t *o = (const uint32_t *)out.contents;
                printf("pass 0 (before any threadgroup size was learned): out[0]=%u out[64]=%u out[128]=%u out[192]=%u\n", o[0], o[64], o[128], o[192]);
            }
        }

        int bad = 0;
        if (b580 && !encBar) { printf("FAIL encBar (render set_barrier / clear_barrier) did not translate on the B580\n"); bad++; }
        // render cells
        const uint8_t *p8 = (const uint8_t *)px.contents;
        const char *rwhat[CELLS] = { "B0 GPU pipeline(P) + vertex buffer + fragment buffer", "B1 per-slot buffers in one run (pos+6, col2)",
                                     "B2 GPU pipeline(Q), not the one the execute binds", "B3 GPU set_barrier (earlier kernel) then draw",
                                     "B4 GPU set_barrier + clear_barrier then draw", "B5 GPU indexed u16 by address, GPU buffers",
                                     "A0 GPU indexed u32 by address (untouched slot)", "A1 GPU plain draw (untouched slot)" };
        for (unsigned k = 0; k < CELLS; k++) {
            const float *c = (k == 1) ? col2[k] : col[k];
            uint8_t want[4] = { (uint8_t)(c[0] * 255 + 0.5f), (uint8_t)(c[1] * 255 + 0.5f), (uint8_t)(c[2] * 255 + 0.5f), 255 };
            const BOOL refused = b580 && (k == 2 || (k == 3 && encBar));
            if (refused) { want[0] = want[1] = want[2] = 0; }
            const unsigned x = (k % 4) * CELL + CELL / 2, y = (k / 4) * CELL + CELL / 2;
            const uint8_t *pp = p8 + (y * W + x) * 4;
            const int ok = abs((int)pp[0] - want[0]) <= 2 && abs((int)pp[1] - want[1]) <= 2 && abs((int)pp[2] - want[2]) <= 2;
            printf("%s %-56s centre %3u %3u %3u (want %3u %3u %3u%s)\n", ok ? "PASS" : "FAIL", rwhat[k], pp[0], pp[1], pp[2],
                   want[0], want[1], want[2], refused ? ", refused on the B580" : "");
            bad += !ok;
        }
        // compute ranges
        const uint32_t *o = (const uint32_t *)out.contents;
        const char *cwhat[4] = { "C0 CPU slot, GPU buffers + grid", "C1 GPU pipeline(W) + buffers + dispatch (learned threadgroup)",
                                 "C2 GPU concurrent_dispatch_threads + constant buffer + set_barrier", "C3 GPU pipeline(W2), not the one the execute binds" };
        for (unsigned s = 0; s < 4; s++) {
            int ok = 1; uint32_t first = o[64 * s];
            for (unsigned i = 0; i < 64; i++) {
                uint32_t want = (s + 1) * 1000 + i;
                if (s == 3) want = b580 ? 0 : 9000 + i;
                if (o[64 * s + i] != want) ok = 0;
            }
            printf("%s %-64s out[%3u] = %u%s\n", ok ? "PASS" : "FAIL", cwhat[s], 64 * s, first, s == 3 && b580 ? " (want 0: refused on the B580)" : "");
            bad += !ok;
        }
        return bad ? 4 : 0;
    }
}
