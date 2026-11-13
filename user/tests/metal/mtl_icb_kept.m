// SPDX-License-Identifier: MIT
// mtl_icb_kept: a compute ICB whose kernels' buffers a shader sets, written to tell apart the two ways mtl_gpu_icb2's
// C0..C2 can read zeros on the B580 with NVMTL_ICB_DGC=1 (docs/design/DESIGN-icb-dgc.md section 10):
//   - the buffer the shader set never reaches the kernel because the kernel reads that buffer through a descriptor (the
//     address lowering kept it), so the kernel writes the CPU's buffer: the CPU's scratch buffers are checked as well;
//   - or the device-generated dispatch takes nothing from the record (push constants / dispatch lost): then nothing at all
//     is written, scratch included.
//
// ICB (concurrent dispatch, inheritPipelineState NO, inheritBuffers NO), slots encoded by the CPU (pipeline, scratch
// buffers, a 1x32 dispatch), then rewritten by a kernel (enc) in the same command buffer, executed twice:
//   K0 kernel kt: o[t] = tag * 1000 + t, t = thread_position_in_grid (the pushed thread base is in the index: before
//      13 Nov this kept o's descriptor). The shader sets o = out + 0, tag = 1, 2 groups of 32  -> out[0..63] = 1000 + i,
//      scratch0 untouched
//   K1 kernel ki: o[idx[t]] = 7000 + t (an index read from memory: o keeps its descriptor on the B580, by design). The shader
//      sets o = out + 64, idx = the identity                                   -> Apple: out[64..127] = 7000 + i;
//      B580: the slot is refused (out[64..127] = 0, scratch1 untouched), never written into the CPU's buffer
//   K2 kernel kt with only the grid and the tag rewritten (o stays the CPU's scratch2): 2 groups -> scratch2[0..63] =
//      3000 + i (the CPU's buffer through the record's dispatch: the device-generated path itself works)
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_ICB_DGC=1 ./mtl_icb_kept      (the B580)
//   ./mtl_icb_kept --any                                                    (the first Metal device, reference run)
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
     "kernel void kt(device uint *o [[buffer(0)]], constant uint &tag [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[t] = tag * 1000 + t; }\n"
     "kernel void ki(device uint *o [[buffer(0)]], const device uint *idx [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[idx[t]] = 7000 + t; }\n"
     "struct Args { command_buffer cb [[id(0)]]; };\n"
     "struct Ptrs { device uint *out; device uint *tag1; device uint *idx; device uint *tag3; };\n"
     "kernel void enc(device Args *a [[buffer(0)]], const device Ptrs *p [[buffer(1)]], uint t [[thread_position_in_grid]]) {\n"
     "    if (t != 0) return;\n"
     "    compute_command c0(a->cb, 0); c0.set_kernel_buffer(p->out, 0); c0.set_kernel_buffer(p->tag1, 1);\n"
     "    c0.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
     "    compute_command c1(a->cb, 1); c1.set_kernel_buffer(p->out + 64, 0); c1.set_kernel_buffer(p->idx, 1);\n"
     "    c1.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
     "    compute_command c2(a->cb, 2); c2.set_kernel_buffer(p->tag3, 1);\n"
     "    c2.concurrent_dispatch_threadgroups(uint3(2, 1, 1), uint3(32, 1, 1));\n"
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

static id<MTLComputePipelineState> pipeline(id<MTLDevice> dev, id<MTLLibrary> lib, NSString *name, BOOL icb)
{
    id<MTLFunction> fn = [lib newFunctionWithName:name];
    if (!fn) { fprintf(stderr, "FAIL: function %s: newFunctionWithName returned nil\n", name.UTF8String); return nil; }
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = fn;
    cd.supportIndirectCommandBuffers = icb;
    NSError *err = nil;
    id<MTLComputePipelineState> p = [dev newComputePipelineStateWithDescriptor:cd options:0 reflection:nil error:&err];
    if (!p) fprintf(stderr, "FAIL: compute pipeline %s: %s\n", name.UTF8String, err.localizedDescription.UTF8String);
    return p;
}

enum { N = 64 };

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
        printf("device: %s (%s)%s\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String,
               b580 ? " - K1 is refused on the B580 (an index from memory keeps the descriptor)" : " - Apple's behaviour");

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:[MTLCompileOptions new] error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLComputePipelineState> KT = pipeline(dev, lib, @"kt", YES), KI = pipeline(dev, lib, @"ki", YES);
        id<MTLComputePipelineState> ENC = pipeline(dev, lib, @"enc", NO);
        if (!KT || !KI || !ENC) {
            if (b580) printf("REFUSED: no encoder kernel (expected without NVMTL_ICB_DGC=1) - exit 2\n");
            return 2;
        }

        MTLIndirectCommandBufferDescriptor *cdsc = [MTLIndirectCommandBufferDescriptor new];
        cdsc.commandTypes = MTLIndirectCommandTypeConcurrentDispatch;
        cdsc.inheritPipelineState = NO; cdsc.inheritBuffers = NO;
        cdsc.maxKernelBufferBindCount = 2;
        id<MTLIndirectCommandBuffer> icb = [dev newIndirectCommandBufferWithDescriptor:cdsc maxCommandCount:3 options:0];
        if (!icb) { fprintf(stderr, "FAIL: newIndirectCommandBufferWithDescriptor\n"); return 2; }

        const MTLResourceOptions sh = MTLResourceStorageModeShared;
        id<MTLBuffer> out = [dev newBufferWithLength:2 * N * 4 options:sh];
        id<MTLBuffer> s0 = [dev newBufferWithLength:N * 4 options:sh], s1 = [dev newBufferWithLength:N * 4 options:sh],
                      s2 = [dev newBufferWithLength:N * 4 options:sh];
        uint32_t ident[N]; for (unsigned i = 0; i < N; i++) ident[i] = i;
        uint32_t zeros[N]; memset(zeros, 0, sizeof zeros);   // the CPU's idx for K1: every thread writes element 0
        id<MTLBuffer> idx = [dev newBufferWithBytes:ident length:sizeof ident options:sh];
        id<MTLBuffer> idx0 = [dev newBufferWithBytes:zeros length:sizeof zeros options:sh];
        const uint32_t tags[4] = { 1, 9, 3, 8 };   // tag1, (cpu tag K0), tag3, (cpu tag K2)
        id<MTLBuffer> tagb = [dev newBufferWithBytes:tags length:sizeof tags options:sh];
        const uint64_t ptrs[4] = { out.gpuAddress, tagb.gpuAddress, idx.gpuAddress, tagb.gpuAddress + 8 };
        id<MTLBuffer> pb = [dev newBufferWithBytes:ptrs length:sizeof ptrs options:sh];

        // the CPU's slots: scratch buffers and a 1x32 dispatch, which the shader rewrites
        id<MTLIndirectComputeCommand> c0 = [icb indirectComputeCommandAtIndex:0];
        [c0 setComputePipelineState:KT]; [c0 setKernelBuffer:s0 offset:0 atIndex:0]; [c0 setKernelBuffer:tagb offset:4 atIndex:1];
        [c0 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        id<MTLIndirectComputeCommand> c1 = [icb indirectComputeCommandAtIndex:1];
        [c1 setComputePipelineState:KI]; [c1 setKernelBuffer:s1 offset:0 atIndex:0]; [c1 setKernelBuffer:idx0 offset:0 atIndex:1];
        [c1 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
        id<MTLIndirectComputeCommand> c2 = [icb indirectComputeCommandAtIndex:2];
        [c2 setComputePipelineState:KT]; [c2 setKernelBuffer:s2 offset:0 atIndex:0]; [c2 setKernelBuffer:tagb offset:12 atIndex:1];
        [c2 concurrentDispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];

        id<MTLFunction> encFn = [lib newFunctionWithName:@"enc"];
        id<MTLArgumentEncoder> ae = [encFn newArgumentEncoderWithBufferIndex:0];
        id<MTLBuffer> arg = [dev newBufferWithLength:ae.encodedLength options:sh];
        [ae setArgumentBuffer:arg offset:0];
        [ae setIndirectCommandBuffer:icb atIndex:0];

        id<MTLCommandQueue> q = [dev newCommandQueue];
        for (int pass = 0; pass < 2; pass++) {
            memset(out.contents, 0, 2 * N * 4);
            for (id<MTLBuffer> s in @[ s0, s1, s2 ]) memset(s.contents, 0, N * 4);
            id<MTLCommandBuffer> cmd = [q commandBuffer];
            id<MTLComputeCommandEncoder> ce = [cmd computeCommandEncoder];
            [ce setComputePipelineState:ENC];
            [ce setBuffer:arg offset:0 atIndex:0]; [ce setBuffer:pb offset:0 atIndex:1];
            [ce useResource:icb usage:MTLResourceUsageWrite];
            [ce dispatchThreads:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
            [ce endEncoding];
            id<MTLComputeCommandEncoder> xe = [cmd computeCommandEncoder];
            [xe setComputePipelineState:KT];
            for (id<MTLBuffer> u in @[ out, s0, s1, s2 ]) [xe useResource:u usage:MTLResourceUsageWrite];
            for (id<MTLBuffer> u in @[ tagb, idx, idx0 ]) [xe useResource:u usage:MTLResourceUsageRead];
            [xe useResource:icb usage:MTLResourceUsageRead];
            [xe executeCommandsInBuffer:icb withRange:NSMakeRange(0, 3)];
            [xe endEncoding];
            [cmd commit];
            [cmd waitUntilCompleted];
            if (cmd.status != MTLCommandBufferStatusCompleted) {
                fprintf(stderr, "FAIL: command buffer %d status %ld: %s\n", pass, (long)cmd.status, cmd.error.localizedDescription.UTF8String ?: "");
                return 3;
            }
            const uint32_t *o = out.contents, *a = s0.contents, *b = s1.contents, *c = s2.contents;
            printf("pass %d: out[0]=%u out[63]=%u out[64]=%u out[127]=%u | scratch0[0]=%u scratch1[0]=%u scratch2[0]=%u scratch2[63]=%u\n",
                   pass, o[0], o[63], o[64], o[127], a[0], b[0], c[0], c[63]);
        }

        const uint32_t *o = out.contents, *a = s0.contents, *b = s1.contents, *c = s2.contents;
        int bad = 0, ok;
        // K0
        ok = 1; for (unsigned i = 0; i < N; i++) if (o[i] != 1000 + i || a[i]) ok = 0;
        printf("%s K0 kt, shader-set o + tag + grid (o[thread_position_in_grid])     out[0]=%u out[63]=%u scratch0[0]=%u%s\n", ok ? "PASS" : "FAIL",
               o[0], o[63], a[0], a[0] ? " (the kernel wrote the CPU's buffer: o kept its descriptor)" : "");
        bad += !ok;
        // K1
        ok = 1; for (unsigned i = 0; i < N; i++) if (o[N + i] != (b580 ? 0 : 7000 + i) || b[i]) ok = 0;
        printf("%s K1 ki, shader-set o (o[idx[t]]: a data-driven index)               out[64]=%u out[127]=%u scratch1[0]=%u (want %s)\n",
               ok ? "PASS" : "FAIL", o[N], o[2 * N - 1], b[0], b580 ? "0, 0, 0: refused on the B580" : "7000, 7063, 0");
        bad += !ok;
        // K2
        ok = 1; for (unsigned i = 0; i < N; i++) if (c[i] != 3000 + i) ok = 0;
        printf("%s K2 kt, shader-set tag + grid, the CPU's o                          scratch2[0]=%u scratch2[63]=%u (want 3000, 3063)\n",
               ok ? "PASS" : "FAIL", c[0], c[63]);
        bad += !ok;
        printf("mtl_icb_kept: %s\n", bad ? "FAIL" : "PASS");
        return bad ? 4 : 0;
    }
}
