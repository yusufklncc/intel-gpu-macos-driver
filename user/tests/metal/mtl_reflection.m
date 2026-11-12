// SPDX-License-Identifier: MIT
// mtl_reflection: pipeline reflection on the Arc B580 (nullmoth/plugin: nvmtl_arguments_for_function, NVMTLMetal4.m).
//
// One kernel with every kind of binding Metal reflects (a device pointer, a constant struct with a scalar and an array
// member, a buffer the kernel never touches, a read texture, a write-only uint texture, a depth texture, a sampler and
// two [[threadgroup(N)]] pointers), and a vertex/fragment pair with a stage-in vertex buffer:
//   1. Metal 3: newComputePipelineStateWithFunction:options:(BindingInfo | BufferTypeInfo):reflection: - every binding
//      by type and index, with its name, access, used flag, data type, size and alignment, struct members, texture
//      type, component type and depth flag, and the threadgroup memory size and alignment;
//   2. Metal 3: the render pipeline's vertex and fragment bindings (the stage-in buffer is no argument);
//   3. a Metal 3 pipeline's -reflection is nil (macOS 26: only MTL4PipelineOptions.shaderReflection fills it);
//   4. Metal 4 (when the device has a Metal 4 compiler): MTL4Compiler pipelines carry -reflection exactly when
//      shaderReflection asks, with the same bindings as 1 and 2.
// The expected answers are what Apple's driver gives (run with --any on an Apple GPU Mac as the reference).
// Each check prints PASS or FAIL (SKIP: not offered); the last line counts them.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1 ./mtl_reflection   (B580; without NVMTL_METAL4 part 4 is skipped)
//   ./mtl_reflection --any                                                the first Metal device when no B580 is present
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "mtl_reflection_expect.h"

#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // MTLArgument-era names some drivers still answer through

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct Params { float4 scale; uint count; float pad[3]; };\n"
     "kernel void kref(device float4 *out [[buffer(0)]], constant Params &p [[buffer(1)]], const device uint *unused [[buffer(5)]],\n"
     "                 texture2d<float, access::read> src [[texture(0)]], texture2d<uint, access::write> dst [[texture(3)]],\n"
     "                 depth2d<float> dep [[texture(4)]], sampler s [[sampler(2)]], threadgroup float4 *tg [[threadgroup(0)]],\n"
     "                 threadgroup uint *tg2 [[threadgroup(1)]], uint i [[thread_position_in_grid]], uint l [[thread_index_in_threadgroup]]) {\n"
     "    tg[l] = src.read(uint2(i, 0)) * p.scale + dep.sample(s, float2(0.5));\n"
     "    tg2[l] = i;\n"
     "    threadgroup_barrier(mem_flags::mem_threadgroup);\n"
     "    out[i] = tg[(l + 1) % p.count] + float(tg2[0]);\n"
     "    dst.write(uint4(i), uint2(i, 0));\n"
     "}\n"
     "struct VO { float4 pos [[position]]; float2 uv; };\n"
     "struct VIn { float2 pos [[attribute(0)]]; float2 uv [[attribute(1)]]; };\n"
     "vertex VO vs(VIn in [[stage_in]], constant float4x4 &mvp [[buffer(1)]]) {\n"
     "    VO o; o.pos = mvp * float4(in.pos, 0, 1); o.uv = in.uv; return o;\n"
     "}\n"
     "fragment float4 fs(VO in [[stage_in]], texture2d<half> t [[texture(1)]], sampler s [[sampler(0)]], constant float4 &tint [[buffer(2)]]) {\n"
     "    return float4(t.sample(s, in.uv)) * tint;\n"
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

        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
        id<MTLFunction> kf = [lib newFunctionWithName:@"kref"], vf = [lib newFunctionWithName:@"vs"], ff = [lib newFunctionWithName:@"fs"];
        if (!kf || !vf || !ff) { fprintf(stderr, "FAIL: functions kref %p vs %p fs %p\n", (__bridge void *)kf, (__bridge void *)vf, (__bridge void *)ff); return 2; }
        const MTLPipelineOption opts = MTLPipelineOptionBindingInfo | MTLPipelineOptionBufferTypeInfo;

        // 1. Metal 3 compute
        MTLAutoreleasedComputePipelineReflection cr = nil;
        id<MTLComputePipelineState> cps = [dev newComputePipelineStateWithFunction:kf options:opts reflection:&cr error:&err];
        if (!cps) { fprintf(stderr, "FAIL: compute pipeline: %s\n", err.localizedDescription.UTF8String); return 2; }
        check(cr != nil, "Metal 3 compute: reflection with BindingInfo | BufferTypeInfo (%s)", cr ? NSStringFromClass([cr class]).UTF8String : "nil");
        if (cr) check_bindings("Metal 3 compute", cr.bindings, kKernel, sizeof kKernel / sizeof kKernel[0]);

        // 2. Metal 3 render, with a vertex descriptor (the stage-in buffer is reflected as vertexBuffer.0)
        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = vf; rd.fragmentFunction = ff; rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
        vd.layouts[0].stride = 16; rd.vertexDescriptor = vd;
        MTLAutoreleasedRenderPipelineReflection rr = nil;
        id<MTLRenderPipelineState> rps = [dev newRenderPipelineStateWithDescriptor:rd options:opts reflection:&rr error:&err];
        if (!rps) { fprintf(stderr, "FAIL: render pipeline: %s\n", err.localizedDescription.UTF8String); return 2; }
        check(rr != nil, "Metal 3 render: reflection with BindingInfo | BufferTypeInfo");
        if (rr) {
            check_bindings("Metal 3 vertex", rr.vertexBindings, kVertex, sizeof kVertex / sizeof kVertex[0]);
            check_bindings("Metal 3 fragment", rr.fragmentBindings, kFragment, sizeof kFragment / sizeof kFragment[0]);
        }

        // 3. and 4.
        if (@available(macOS 26.0, *)) {
            check(cps.reflection == nil && rps.reflection == nil, "a Metal 3 pipeline's -reflection is nil (compute %p, render %p)",
                  (__bridge void *)cps.reflection, (__bridge void *)rps.reflection);
            // a device without the Metal 4 family (the B580 without NVMTL_METAL4=1) is not asked for a compiler: apps check first
            id<MTL4Compiler> comp = [dev supportsFamily:MTLGPUFamilyMetal4] ? [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err] : nil;
            if (!comp) skip("Metal 4: no MTLGPUFamilyMetal4 or no MTL4Compiler (B580: NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1)");
            else {
                MTL4LibraryFunctionDescriptor *kd = [MTL4LibraryFunctionDescriptor new]; kd.library = lib; kd.name = @"kref";
                MTL4ComputePipelineDescriptor *cd = [MTL4ComputePipelineDescriptor new]; cd.computeFunctionDescriptor = kd;
                cd.options = [MTL4PipelineOptions new];
                cd.options.shaderReflection = MTL4ShaderReflectionNone;
                id<MTLComputePipelineState> p0 = [comp newComputePipelineStateWithDescriptor:cd compilerTaskOptions:nil error:&err];
                check(p0 && !p0.reflection, "Metal 4 compute, shaderReflection None: pipeline %p, reflection %p", (__bridge void *)p0, (__bridge void *)p0.reflection);
                cd.options.shaderReflection = MTL4ShaderReflectionBindingInfo | MTL4ShaderReflectionBufferTypeInfo;
                id<MTLComputePipelineState> p1 = [comp newComputePipelineStateWithDescriptor:cd compilerTaskOptions:nil error:&err];
                check(p1 && p1.reflection, "Metal 4 compute, shaderReflection BindingInfo | BufferTypeInfo: pipeline %p, reflection %p",
                      (__bridge void *)p1, (__bridge void *)p1.reflection);
                if (p1.reflection) check_bindings("Metal 4 compute", p1.reflection.bindings, kKernel, sizeof kKernel / sizeof kKernel[0]);

                MTL4LibraryFunctionDescriptor *v4 = [MTL4LibraryFunctionDescriptor new]; v4.library = lib; v4.name = @"vs";
                MTL4LibraryFunctionDescriptor *f4 = [MTL4LibraryFunctionDescriptor new]; f4.library = lib; f4.name = @"fs";
                MTL4RenderPipelineDescriptor *r4 = [MTL4RenderPipelineDescriptor new];
                r4.vertexFunctionDescriptor = v4; r4.fragmentFunctionDescriptor = f4; r4.vertexDescriptor = vd;
                r4.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
                r4.options = [MTL4PipelineOptions new]; r4.options.shaderReflection = MTL4ShaderReflectionBindingInfo;
                id<MTLRenderPipelineState> q = [comp newRenderPipelineStateWithDescriptor:r4 compilerTaskOptions:nil error:&err];
                check(q && q.reflection, "Metal 4 render, shaderReflection BindingInfo: pipeline %p, reflection %p (%s)", (__bridge void *)q,
                      (__bridge void *)q.reflection, q ? "" : err.localizedDescription.UTF8String);
                if (q.reflection) {
                    check_bindings("Metal 4 vertex", q.reflection.vertexBindings, kVertex, sizeof kVertex / sizeof kVertex[0]);
                    check_bindings("Metal 4 fragment", q.reflection.fragmentBindings, kFragment, sizeof kFragment / sizeof kFragment[0]);
                }
            }
        } else skip("macOS 26 pipeline -reflection and MTL4Compiler (older OS)");

        printf("%d PASS, %d FAIL, %d SKIP\n", npass, nfail, nskip);
        return nfail ? 4 : 0;
    }
}
