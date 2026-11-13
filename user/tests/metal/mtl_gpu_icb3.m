// SPDX-License-Identifier: MIT
// mtl_gpu_icb3: indirect draws a vertex shader encodes into ICB slots the CPU gave state but no draw - the shape of
// WebKit's newer render-bundle helpers (vsICBIndirectDraw / vsICBIndirectIndexedUint / vsICBIndirectIndexedUshort, WebKit
// main, not yet in Safari 26.7), written here from scratch: a render pipeline with rasterization off draws one point per
// slot, and its vertex function reads the draw from an indirect-arguments buffer and encodes it with render_command into
// the slot the CPU named in a small constant buffer. The index pointer is an entry buffer parameter plus two dynamic
// element offsets (`ib + slotData[2] + args.indexStart`: two GEPs in the AIR), the primitive type comes from data.
//
// ICB (inheritPipelineState NO, inheritBuffers NO): the CPU encodes the pipeline P (triangle class), the vertex buffer and
// the fragment buffer into every slot; only slot 5 also gets a CPU draw. A 128x64 target, eight 32x32 cells, cell k drawn
// by slot k with colour k (baseInstance k):
//     0  GPU indexed uint32 triangle list, index pointer ib32 + 1 + 2 (indexStart)
//     1  GPU indexed uint16 triangle list, ib16 + 0 + 1
//     2  GPU plain triangle list (vertexStart 12)
//     3  GPU indexed uint32 triangle strip with a base vertex (4 indices)
//     4  GPU plain triangle strip (4 vertices)
//     5  CPU draw (the control: no GPU encode)
//     6, 7 empty slots (nothing encoded, nothing drawn)
// Executed in two command buffers; the encode runs in the first only (WebKit encodes a bundle's slot once and executes it
// every frame while its buffers are unchanged), so the second shows that the slots keep what the shader wrote.
//
// On the B580: NVMTL_ICB_DGC=1 (with NVMTL_PROCESS_ENV=1) executes through device-generated commands, which take an index
// buffer by address. Without it cells 0, 1 and 3 stay black (an index buffer no CPU command named: refused) and 4 too
// (the classic execute draws a slot without a CPU draw as its class's list). With it, the strips (3, 4) are drawn from the
// second execute on (the execute learns the topology a shader drew; NVMTL_ICB_DGC_STRIPS=1 draws both from the first).
//
//   NVMTL_ENABLE=1 ./mtl_gpu_icb3          (the B580)
//   ./mtl_gpu_icb3 --any                   the first Metal device when no B580 is present (reference run)
//
// Exit: 0 pass, 1 no device, 2 compile/pipeline failure, 3 GPU error, 4 wrong results.
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
     "struct Container { device uint *flag [[id(0)]]; command_buffer cb [[id(1)]]; };\n"
     // slot[0] = ICB slot, slot[1] = primitive type, slot[2] = index element offset
     "[[vertex]] void encDraw(device const MTLDrawPrimitivesIndirectArguments &a [[buffer(0)]], device Container *c [[buffer(1)]],\n"
     "                        const constant uint *slot [[buffer(2)]]) {\n"
     "    render_command cmd(c->cb, slot[0]);\n"
     "    cmd.draw_primitives(static_cast<primitive_type>(slot[1]), a.vertexStart, a.vertexCount, a.instanceCount, a.baseInstance);\n"
     "}\n"
     "[[vertex]] void encIdx32(device const MTLDrawIndexedPrimitivesIndirectArguments &a [[buffer(0)]], device Container *c [[buffer(1)]],\n"
     "                         device uint *ib [[buffer(2)]], const constant uint *slot [[buffer(3)]]) {\n"
     "    render_command cmd(c->cb, slot[0]);\n"
     "    device uint *base = ib + slot[2] + a.indexStart;\n"
     "    cmd.draw_indexed_primitives(static_cast<primitive_type>(slot[1]), a.indexCount, base, a.instanceCount, a.baseVertex, a.baseInstance);\n"
     "}\n"
     "[[vertex]] void encIdx16(device const MTLDrawIndexedPrimitivesIndirectArguments &a [[buffer(0)]], device Container *c [[buffer(1)]],\n"
     "                         device ushort *ib [[buffer(2)]], const constant uint *slot [[buffer(3)]]) {\n"
     "    render_command cmd(c->cb, slot[0]);\n"
     "    device ushort *base = ib + slot[2] + a.indexStart;\n"
     "    cmd.draw_indexed_primitives(static_cast<primitive_type>(slot[1]), a.indexCount, base, a.instanceCount, a.baseVertex, a.baseInstance);\n"
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

enum { W = 128, H = 64, CELL = 32, CELLS = 8, SLOTS = 8, STRIP0 = CELLS * 6 };

static void cell_rect(unsigned k, float *x0, float *y0, float *x1, float *y1)
{
    const float px0 = (float)(k % 4) * CELL + 4, py0 = (float)(k / 4) * CELL + 4, px1 = px0 + CELL - 8, py1 = py0 + CELL - 8;
    *x0 = px0 / W * 2 - 1; *x1 = px1 / W * 2 - 1;
    *y0 = 1 - py0 / H * 2; *y1 = 1 - py1 / H * 2;
}

// the indirect arguments Metal's MTLDraw(Indexed)PrimitivesIndirectArguments describe
typedef struct { uint32_t vertexCount, instanceCount, vertexStart, baseInstance; } DrawArgs;
typedef struct { uint32_t indexCount, instanceCount, indexStart; int32_t baseVertex; uint32_t baseInstance; } IdxArgs;

static id<MTLRenderPipelineState> encoder_pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name)
{
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.label = name;
    d.vertexFunction = [lib newFunctionWithName:name];
    d.rasterizationEnabled = NO;
    d.inputPrimitiveTopology = MTLPrimitiveTopologyClassPoint;
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    NSError *err = nil;
    id<MTLRenderPipelineState> p = d.vertexFunction ? [dev newRenderPipelineStateWithDescriptor:d error:&err] : nil;
    if (!p) fprintf(stderr, "FAIL: encoder pipeline %s: %s\n", name.UTF8String, err.localizedDescription.UTF8String ?: "no function");
    return p;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
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
        const char *ke = getenv("NVMTL_ICB_DGC"), *se = getenv("NVMTL_ICB_DGC_STRIPS");
        const BOOL dgc = ke && !strcmp(ke, "1"), strips = se && se[0] == '1';
        printf("device: %s (%s)%s\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String,
               !b580 ? " - expecting Apple's behaviour" : dgc ? (strips ? " - NVMTL_ICB_DGC=1, NVMTL_ICB_DGC_STRIPS=1: every cell from the first execute"
                                                                         : " - NVMTL_ICB_DGC=1: the strips from the second execute")
                                                          : " - no NVMTL_ICB_DGC: the indexed draws and the strip refused");

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:[MTLCompileOptions new] error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.label = @"P";
        rd.vertexFunction = [lib newFunctionWithName:@"vmain"];
        rd.fragmentFunction = [lib newFunctionWithName:@"fmain"];
        rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        rd.inputPrimitiveTopology = MTLPrimitiveTopologyClassTriangle;
        rd.supportIndirectCommandBuffers = YES;
        id<MTLRenderPipelineState> P = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
        if (!P) { fprintf(stderr, "FAIL: render pipeline P: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLRenderPipelineState> eDraw = encoder_pipeline(dev, lib, @"encDraw"), e32 = encoder_pipeline(dev, lib, @"encIdx32"),
                                   e16 = encoder_pipeline(dev, lib, @"encIdx16");
        if (!eDraw || !e32 || !e16) return 2;

        // geometry: cell k as a triangle list at pos[6k], and as a strip at pos[STRIP0 + 4k]
        float pos[STRIP0 + CELLS * 4][2];
        for (unsigned k = 0; k < CELLS; k++) {
            float x0, y0, x1, y1; cell_rect(k, &x0, &y0, &x1, &y1);
            const float l[6][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x0, y1 }, { x1, y0 }, { x1, y1 } };
            const float s[4][2] = { { x0, y0 }, { x1, y0 }, { x0, y1 }, { x1, y1 } };
            memcpy(pos[6 * k], l, sizeof l);
            memcpy(pos[STRIP0 + 4 * k], s, sizeof s);
        }
        float col[CELLS][4];
        for (unsigned k = 0; k < CELLS; k++) {
            const float c[4] = { (k & 1) ? 1.0f : 0.25f, (k & 2) ? 1.0f : 0.25f, (k & 4) ? 1.0f : 0.25f, 1 };
            memcpy(col[k], c, sizeof c);
        }
        // uint32: junk, cell 0's list at [3..8], junk, cell 3's strip (0..3 + base vertex) at [20..23]
        uint32_t i32[24];
        for (unsigned i = 0; i < 24; i++) i32[i] = 1000;
        for (unsigned i = 0; i < 6; i++) i32[3 + i] = i;
        for (unsigned i = 0; i < 4; i++) i32[20 + i] = i;
        // uint16: junk, cell 1's list at [1..6]
        const uint16_t i16[8] = { 999, 6, 7, 8, 9, 10, 11, 999 };
        id<MTLBuffer> posb = [dev newBufferWithBytes:pos length:sizeof pos options:MTLResourceStorageModeShared];
        id<MTLBuffer> colb = [dev newBufferWithBytes:col length:sizeof col options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib32 = [dev newBufferWithBytes:i32 length:sizeof i32 options:MTLResourceStorageModeShared];
        id<MTLBuffer> ib16 = [dev newBufferWithBytes:i16 length:sizeof i16 options:MTLResourceStorageModeShared];
        // the indirect arguments, 32 bytes apart
        uint8_t args[5 * 32]; memset(args, 0, sizeof args);
        const IdxArgs a0 = { 6, 1, 2, 0, 0 }, a1 = { 6, 1, 1, 0, 1 }, a3 = { 4, 1, 4, STRIP0 + 4 * 3, 3 };
        const DrawArgs a2 = { 6, 1, 12, 2 }, a4 = { 4, 1, STRIP0 + 4 * 4, 4 };
        memcpy(args + 0 * 32, &a0, sizeof a0); memcpy(args + 1 * 32, &a1, sizeof a1); memcpy(args + 2 * 32, &a2, sizeof a2);
        memcpy(args + 3 * 32, &a3, sizeof a3); memcpy(args + 4 * 32, &a4, sizeof a4);
        id<MTLBuffer> argb = [dev newBufferWithBytes:args length:sizeof args options:MTLResourceStorageModeShared];

        MTLIndirectCommandBufferDescriptor *bd = [MTLIndirectCommandBufferDescriptor new];
        bd.commandTypes = MTLIndirectCommandTypeDraw | MTLIndirectCommandTypeDrawIndexed;
        bd.inheritPipelineState = NO; bd.inheritBuffers = NO;
        bd.maxVertexBufferBindCount = 1; bd.maxFragmentBufferBindCount = 1;
        id<MTLIndirectCommandBuffer> icb = [dev newIndirectCommandBufferWithDescriptor:bd maxCommandCount:SLOTS options:0];
        if (!icb) { fprintf(stderr, "FAIL: newIndirectCommandBufferWithDescriptor\n"); return 2; }
        for (unsigned k = 0; k < 6; k++) {
            id<MTLIndirectRenderCommand> c = [icb indirectRenderCommandAtIndex:k];
            [c setRenderPipelineState:P];
            [c setVertexBuffer:posb offset:0 atIndex:0];
            [c setFragmentBuffer:colb offset:0 atIndex:0];
        }
        [[icb indirectRenderCommandAtIndex:5] drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:30 vertexCount:6 instanceCount:1 baseInstance:5];

        // the container, as WebKit makes it: an argument encoder of the encoder function's buffer 1
        id<MTLFunction> encFn = [lib newFunctionWithName:@"encIdx32"];
        id<MTLArgumentEncoder> ae = [encFn newArgumentEncoderWithBufferIndex:1];
        id<MTLBuffer> flag = [dev newBufferWithLength:16 options:MTLResourceStorageModeShared];
        id<MTLBuffer> cont = [dev newBufferWithLength:ae.encodedLength options:MTLResourceStorageModeShared];
        [ae setArgumentBuffer:cont offset:0];
        [ae setBuffer:flag offset:0 atIndex:0];
        [ae setIndirectCommandBuffer:icb atIndex:1];

        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLBuffer> px[2] = { [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared],
                                [dev newBufferWithLength:W * H * 4 options:MTLResourceStorageModeShared] };
        id<MTLCommandQueue> q = [dev newCommandQueue];

        for (int pass = 0; pass < 2; pass++) {
            id<MTLCommandBuffer> cmd = [q commandBuffer];
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
            id<MTLRenderCommandEncoder> re = [cmd renderCommandEncoderWithDescriptor:rp];
            if (pass == 0) {
                // one point per slot, each with its own encoder pipeline and slot data (WebKit's phase C)
                struct { id<MTLRenderPipelineState> p; unsigned slot, prim, off; id<MTLBuffer> ib; } enc[5] = {
                    { e32, 0, MTLPrimitiveTypeTriangle, 1, ib32 }, { e16, 1, MTLPrimitiveTypeTriangle, 0, ib16 },
                    { eDraw, 2, MTLPrimitiveTypeTriangle, 0, nil }, { e32, 3, MTLPrimitiveTypeTriangleStrip, 16, ib32 },
                    { eDraw, 4, MTLPrimitiveTypeTriangleStrip, 0, nil } };
                for (unsigned e = 0; e < 5; e++) {
                    const uint32_t slot[3] = { enc[e].slot, enc[e].prim, enc[e].off };
                    [re setRenderPipelineState:enc[e].p];
                    [re setVertexBuffer:argb offset:32 * enc[e].slot atIndex:0];
                    [re setVertexBuffer:cont offset:0 atIndex:1];
                    if (enc[e].ib) {
                        [re setVertexBuffer:enc[e].ib offset:0 atIndex:2];
                        [re useResource:enc[e].ib usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
                        [re setVertexBytes:slot length:sizeof slot atIndex:3];
                    } else
                        [re setVertexBytes:slot length:sizeof slot atIndex:2];
                    [re useResource:icb usage:MTLResourceUsageRead | MTLResourceUsageWrite stages:MTLRenderStageVertex];
                    [re useResource:flag usage:MTLResourceUsageWrite stages:MTLRenderStageVertex];
                    [re drawPrimitives:MTLPrimitiveTypePoint vertexStart:0 vertexCount:1];
                }
                [re memoryBarrierWithScope:MTLBarrierScopeBuffers afterStages:MTLRenderStageVertex beforeStages:MTLRenderStageVertex];
            }
            for (id<MTLBuffer> u in @[ posb, colb, ib32, ib16 ])
                [re useResource:u usage:MTLResourceUsageRead stages:MTLRenderStageVertex | MTLRenderStageFragment];
            [re useResource:icb usage:MTLResourceUsageRead stages:MTLRenderStageVertex];
            [re executeCommandsInBuffer:icb withRange:NSMakeRange(0, SLOTS)];
            [re endEncoding];
            id<MTLBlitCommandEncoder> be = [cmd blitCommandEncoder];
            [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                       toBuffer:px[pass] destinationOffset:0 destinationBytesPerRow:W * 4 destinationBytesPerImage:W * H * 4];
            [be endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if (cmd.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "FAIL: command buffer %d status %ld: %s\n", pass, (long)cmd.status, cmd.error.localizedDescription.UTF8String ?: "");
                return 3;
            }
        }

        const char *what[CELLS] = { "0 GPU indexed u32 list, ib + k + indexStart", "1 GPU indexed u16 list, ib + k + indexStart",
                                    "2 GPU plain list", "3 GPU indexed u32 strip, base vertex", "4 GPU plain strip", "5 CPU draw (control)",
                                    "6 empty slot", "7 empty slot" };
        int bad = 0;
        for (int pass = 0; pass < 2; pass++) {
            const uint8_t *p8 = (const uint8_t *)px[pass].contents;
            for (unsigned k = 0; k < CELLS; k++) {
                const BOOL indexed = k == 0 || k == 1 || k == 3, strip = k == 3 || k == 4;
                BOOL drawn = k < 6, either = NO;
                const char *why = "";
                if (b580 && !dgc && k < 5 && (indexed || strip)) { drawn = NO; why = ", refused without NVMTL_ICB_DGC=1"; }
                else if (b580 && dgc && strip && !strips && pass == 0) { either = YES; why = ", the B580 learns the strip in this execute"; }
                uint8_t want[4] = { 0, 0, 0, 255 };
                if (drawn) for (int c = 0; c < 3; c++) want[c] = (uint8_t)(col[k][c] * 255 + 0.5f);
                const unsigned x = (k % 4) * CELL + CELL / 2, y = (k / 4) * CELL + CELL / 2;
                const uint8_t *pp = p8 + (y * W + x) * 4;
                const BOOL match = abs((int)pp[0] - want[0]) <= 2 && abs((int)pp[1] - want[1]) <= 2 && abs((int)pp[2] - want[2]) <= 2;
                const BOOL black = pp[0] <= 2 && pp[1] <= 2 && pp[2] <= 2;
                const BOOL ok = match || (either && black);
                printf("%s pass %d %-44s centre %3u %3u %3u (want %3u %3u %3u%s)\n", ok ? "PASS" : "FAIL", pass, what[k], pp[0], pp[1], pp[2],
                       want[0], want[1], want[2], why);
                bad += !ok;
            }
        }
        return bad ? 4 : 0;
    }
}
