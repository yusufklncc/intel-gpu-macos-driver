// SPDX-License-Identifier: MIT
// mtl4_compiler: Metal 4's MTL4Compiler on the Arc B580 (nullmoth/plugin/NVMTLMetal4.m, NVMTL_METAL4=1).
//
// Pipelines made by the compiler run on an ordinary Metal command queue, so each case checks what the pipeline computes
// or draws, not only that it was made:
//   1. a library from source (MTL4LibraryDescriptor) and a compute pipeline from a library function descriptor;
//   2. a specialized function (function constants) through MTL4SpecializedFunctionDescriptor;
//   3. maxTotalThreadsPerThreadgroup and requiredThreadsPerThreadgroup of the descriptor;
//   4. the asynchronous form: a compiler task, its status and the completion handler;
//   5. static linking (MTL4StaticLinkingDescriptor): a kernel calling a [[visible]] function declared only, defined in
//      another library;
//   6. a render pipeline (vertex + fragment, a specialized fragment) drawing into a 4x4 texture, and a pipeline made by
//      specialization from one with unspecialized blending;
//   7. archives: a pipeline data set serializer records the pipelines a compiler builds, the archive it writes is read
//      back, and a lookup of a pipeline in it works while one that is not there fails; a missing archive is an error.
// Expected answers are Apple's (run with --any on an Apple GPU Mac as the reference). Each check prints PASS or FAIL.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1 ./mtl4_compiler   (B580)
//   ./mtl4_compiler --any                                                the first Metal device when no B580 is present
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 3 GPU error, 4 wrong results, 5 no MTL4Compiler (Metal 4 off).
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void add(device float *o [[buffer(0)]], const device float *a [[buffer(1)]], const device float *b [[buffer(2)]],\n"
     "                uint i [[thread_position_in_grid]]) { o[i] = a[i] + b[i]; }\n"
     "constant float K [[function_constant(0)]];\n"
     "kernel void axpy(device float *o [[buffer(0)]], const device float *a [[buffer(1)]], const device float *b [[buffer(2)]],\n"
     "                 uint i [[thread_position_in_grid]]) { o[i] = a[i] + b[i] * K; }\n"
     "kernel void ids(device uint *o [[buffer(0)]], uint i [[thread_position_in_grid]], uint n [[threads_per_threadgroup]]) { o[i] = i * 1000 + n; }\n"
     "[[visible]] float twice(float x);\n"
     "kernel void linked(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = twice(o[i]); }\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint vid [[vertex_id]]) { float2 q = float2((vid << 1) & 2, vid & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
     "constant float4 COLOR [[function_constant(1)]];\n"
     "fragment float4 fs() { return COLOR; }\n";
static NSString *const kHelper =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "[[visible]] float twice(float x) { return 2.0f * x + 0.5f; }\n";

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0;
    IORegistryEntryGetRegistryEntryID(s, &id);
    IOObjectRelease(s);
    return id;
}

static int npass, nfail;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}

enum { N = 256 };
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static int gpu_error;

// Runs `ps` over N threads (threadgroups of tg) with buffers bound at 0.., and waits.
static void run(id<MTLComputePipelineState> ps, NSArray<id<MTLBuffer>> *bufs, NSUInteger tg)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps];
    for (NSUInteger i = 0; i < bufs.count; i++) [e setBuffer:bufs[i] offset:0 atIndex:i];
    [e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(tg, 1, 1)];
    [e endEncoding];
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String ?: "?"); gpu_error = 1; }
}
static id<MTLBuffer> floats(float (^f)(int))
{
    id<MTLBuffer> b = [dev newBufferWithLength:N * sizeof(float) options:MTLResourceStorageModeShared];
    float *p = b.contents; for (int i = 0; i < N; i++) p[i] = f ? f(i) : 0;
    return b;
}
// o == a + b * k for every element
static int axpy_ok(id<MTLBuffer> o, float k, int *firstBad)
{
    const float *p = o.contents;
    for (int i = 0; i < N; i++) if (p[i] != (float)i + (float)(N - i) * k) { *firstBad = i; return 0; }
    return 1;
}

API_AVAILABLE(macos(26.0))
static MTL4LibraryFunctionDescriptor *fn(id<MTLLibrary> lib, NSString *name)
{
    MTL4LibraryFunctionDescriptor *d = [MTL4LibraryFunctionDescriptor new]; d.library = lib; d.name = name; return d;
}
API_AVAILABLE(macos(26.0))
static MTL4ComputePipelineDescriptor *compute(MTL4FunctionDescriptor *f)
{
    MTL4ComputePipelineDescriptor *d = [MTL4ComputePipelineDescriptor new]; d.computeFunctionDescriptor = f; return d;
}

API_AVAILABLE(macos(26.0))
static int body(void)
{
    NSError *err = nil;
    if (![dev supportsFamily:MTLGPUFamilyMetal4]) { printf("SKIP: no MTLGPUFamilyMetal4 (B580: NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1)\n"); return 5; }
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    if (!comp) { printf("SKIP: no MTL4Compiler (%s; B580: NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1)\n", err.localizedDescription.UTF8String ?: "nil"); return 5; }
    check(comp.device == dev, "MTL4Compiler %s, device is the one that made it", NSStringFromClass([(id)comp class]).UTF8String);

    // 1. library and compute pipeline
    MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new]; ld.source = kSource; ld.name = @"mtl4_compiler";
    id<MTLLibrary> lib = [comp newLibraryWithDescriptor:ld error:&err];
    if (!lib) { fprintf(stderr, "FAIL: newLibraryWithDescriptor: %s\n", err.localizedDescription.UTF8String); return 2; }
    check([lib.functionNames containsObject:@"add"] && [lib.functionNames containsObject:@"fs"], "library from a MTL4LibraryDescriptor: %lu functions",
          (unsigned long)lib.functionNames.count);
    id<MTLBuffer> a = floats(^float(int i) { return (float)i; }), b = floats(^float(int i) { return (float)(N - i); }), o = floats(nil);
    id<MTLComputePipelineState> p1 = [comp newComputePipelineStateWithDescriptor:compute(fn(lib, @"add")) compilerTaskOptions:nil error:&err];
    int bad = -1;
    if (p1) run(p1, @[o, a, b], 64);
    check(p1 && axpy_ok(o, 1.0f, &bad), "compute pipeline (library function): o = a + b%s", p1 ? "" : err.localizedDescription.UTF8String);

    // 2. specialized function
    MTL4SpecializedFunctionDescriptor *sf = [MTL4SpecializedFunctionDescriptor new];
    sf.functionDescriptor = fn(lib, @"axpy");
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; const float k = 3.0f;
    [cv setConstantValue:&k type:MTLDataTypeFloat atIndex:0];
    sf.constantValues = cv;
    id<MTLComputePipelineState> p2 = [comp newComputePipelineStateWithDescriptor:compute(sf) compilerTaskOptions:nil error:&err];
    if (p2) run(p2, @[o, a, b], 64);
    check(p2 && axpy_ok(o, 3.0f, &bad), "compute pipeline (specialized, K = 3): o = a + 3b%s%s", p2 ? "" : " - ", p2 ? "" : err.localizedDescription.UTF8String);

    // 3. threadgroup limits of the descriptor
    MTL4ComputePipelineDescriptor *cd = compute(fn(lib, @"ids"));
    cd.maxTotalThreadsPerThreadgroup = 64;
    id<MTLComputePipelineState> p3 = [comp newComputePipelineStateWithDescriptor:cd compilerTaskOptions:nil error:&err];
    check(p3 && p3.maxTotalThreadsPerThreadgroup >= 1 && p3.maxTotalThreadsPerThreadgroup <= 64, "maxTotalThreadsPerThreadgroup 64 -> the pipeline answers %lu",
          (unsigned long)p3.maxTotalThreadsPerThreadgroup);
    cd = compute(fn(lib, @"ids"));
    cd.requiredThreadsPerThreadgroup = MTLSizeMake(32, 1, 1);
    id<MTLComputePipelineState> p3b = [comp newComputePipelineStateWithDescriptor:cd compilerTaskOptions:nil error:&err];
    id<MTLBuffer> u = [dev newBufferWithLength:N * 4 options:MTLResourceStorageModeShared];
    if (p3b) run(p3b, @[u], 32);
    const uint32_t *up = u.contents;
    check(p3b && up[0] == 32 && up[N - 1] == (N - 1) * 1000 + 32, "requiredThreadsPerThreadgroup 32: pipeline %p, ids[0] %u, ids[%d] %u",
          (__bridge void *)p3b, up[0], N - 1, up[N - 1]);

    // 4. asynchronous build
    __block id<MTLComputePipelineState> p4 = nil; __block NSError *e4 = nil; __block int called = 0;
    id<MTL4CompilerTask> task = [comp newComputePipelineStateWithDescriptor:compute(fn(lib, @"add")) compilerTaskOptions:nil
                                                        completionHandler:^(id<MTLComputePipelineState> ps, NSError *e) { p4 = ps; e4 = e; called++; }];
    [task waitUntilCompleted];
    for (int t = 0; t < 200 && !called; t++) usleep(10000);   // the handler may run just after the task completes
    check(task && task.compiler == comp && task.status == MTL4CompilerTaskStatusFinished && called == 1 && p4,
          "asynchronous compute pipeline: task status %ld, handler called %d time(s), pipeline %p %s", (long)task.status, called, (__bridge void *)p4,
          e4 ? e4.localizedDescription.UTF8String : "");

    // 5. static linking of an extern function from another library
    id<MTLLibrary> helper = [comp newLibraryWithDescriptor:({ MTL4LibraryDescriptor *h = [MTL4LibraryDescriptor new]; h.source = kHelper; h; }) error:&err];
    if (!helper) check(0, "helper library: %s", err.localizedDescription.UTF8String);
    else {
        cd = compute(fn(lib, @"linked"));
        MTL4StaticLinkingDescriptor *sl = [MTL4StaticLinkingDescriptor new]; sl.functionDescriptors = @[fn(helper, @"twice")];
        cd.staticLinkingDescriptor = sl;
        id<MTLComputePipelineState> p5 = [comp newComputePipelineStateWithDescriptor:cd compilerTaskOptions:nil error:&err];
        id<MTLBuffer> l = floats(^float(int i) { return (float)i; });
        if (p5) run(p5, @[l], 64);
        const float *lp = l.contents;
        check(p5 && lp[0] == 0.5f && lp[10] == 20.5f && lp[N - 1] == 2.0f * (N - 1) + 0.5f, "static linking: twice() from a second library: o[10] = %g (want 20.5)%s%s",
              lp[10], p5 ? "" : " - ", p5 ? "" : err.localizedDescription.UTF8String);
    }

    // 6. render pipeline: a specialized fragment colour into a 4x4 texture
    MTL4SpecializedFunctionDescriptor *sfs = [MTL4SpecializedFunctionDescriptor new];
    sfs.functionDescriptor = fn(lib, @"fs");
    MTLFunctionConstantValues *fcv = [MTLFunctionConstantValues new]; const float col[4] = { 1.0f, 0.5f, 0.25f, 1.0f };
    [fcv setConstantValue:col type:MTLDataTypeFloat4 atIndex:1];
    sfs.constantValues = fcv;
    MTL4RenderPipelineDescriptor *rd = [MTL4RenderPipelineDescriptor new];
    rd.vertexFunctionDescriptor = fn(lib, @"vs"); rd.fragmentFunctionDescriptor = sfs;
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTLRenderPipelineState> rp = [comp newRenderPipelineStateWithDescriptor:rd compilerTaskOptions:nil error:&err];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:4 height:4 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
    uint32_t px[16] = { 0 };
    if (rp) {
        MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = rt; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLCommandBuffer> cb = [queue commandBuffer];
        id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
        [re setRenderPipelineState:rp]; [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
        [cb commit]; [cb waitUntilCompleted];
        if (cb.status != MTLCommandBufferStatusCompleted) gpu_error = 1;
        [rt getBytes:px bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
    }
    int same = 1; for (int i = 0; i < 16; i++) same &= px[i] == px[0];
    // 0.5 and 0.25 round to 0x80 and 0x40 on every GPU we know of; accept one step either way
    const uint32_t r = px[0] & 0xff, g = (px[0] >> 8) & 0xff, bl = (px[0] >> 16) & 0xff, al = px[0] >> 24;
    check(rp && same && r == 0xff && (g == 0x7f || g == 0x80) && (bl == 0x3f || bl == 0x40) && al == 0xff,
          "render pipeline (specialized fragment colour) draws 0x%08x everywhere (want ~0xff4080ff)%s%s", px[0], rp ? "" : " - ", rp ? "" : err.localizedDescription.UTF8String);
    rd.colorAttachments[0].blendingState = MTL4BlendStateUnspecialized;
    id<MTLRenderPipelineState> un = [comp newRenderPipelineStateWithDescriptor:rd compilerTaskOptions:nil error:&err];
    rd.colorAttachments[0].blendingState = MTL4BlendStateEnabled;
    id<MTLRenderPipelineState> sp = un ? [comp newRenderPipelineStateBySpecializationWithDescriptor:rd pipeline:un error:&err] : nil;
    check(un && sp, "render pipeline by specialization (unspecialized blending, then enabled): %p -> %p", (__bridge void *)un, (__bridge void *)sp);

    // 7. archives
    NSURL *missing = [NSURL fileURLWithPath:[NSString stringWithFormat:@"%@/mtl4_compiler-missing-%d.mtl4archive", NSTemporaryDirectory(), getpid()]];
    err = nil;
    id<MTL4Archive> none = [dev newArchiveWithURL:missing error:&err];
    check(!none && err, "newArchiveWithURL on a missing file: nil, error \"%s\"", err.localizedDescription.UTF8String ?: "(none)");
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new];
    sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    id<MTL4PipelineDataSetSerializer> ser = [dev newPipelineDataSetSerializerWithDescriptor:sd];
    MTL4CompilerDescriptor *ccd = [MTL4CompilerDescriptor new]; ccd.pipelineDataSetSerializer = ser;
    id<MTL4Compiler> rec = ser ? [dev newCompilerWithDescriptor:ccd error:&err] : nil;
    id<MTLComputePipelineState> pr = rec ? [rec newComputePipelineStateWithDescriptor:compute(fn(lib, @"add")) compilerTaskOptions:nil error:&err] : nil;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithFormat:@"%@/mtl4_compiler-%d.mtl4archive", NSTemporaryDirectory(), getpid()]];
    BOOL wrote = pr && [ser serializeAsArchiveAndFlushToURL:url error:&err];
    check(ser && rec && pr && wrote, "serializer %p, recording compiler %p, pipeline %p, archive written %d%s%s", (__bridge void *)ser, (__bridge void *)rec,
          (__bridge void *)pr, wrote, wrote ? "" : " - ", wrote ? "" : err.localizedDescription.UTF8String);
    id<MTL4Archive> ar = wrote ? [dev newArchiveWithURL:url error:&err] : nil;
    id<MTLComputePipelineState> hit = ar ? [ar newComputePipelineStateWithDescriptor:compute(fn(lib, @"add")) error:&err] : nil;
    if (hit) { o = floats(nil); run(hit, @[o, a, b], 64); }
    check(ar && hit && axpy_ok(o, 1.0f, &bad), "archive read back (%p): the recorded pipeline is found and computes o = a + b", (__bridge void *)ar);
    NSError *me = nil;
    id<MTLComputePipelineState> miss = ar ? [ar newComputePipelineStateWithDescriptor:compute(fn(lib, @"ids")) error:&me] : nil;
    check(ar && !miss, "archive lookup of a pipeline it does not hold: %p, error \"%s\"", (__bridge void *)miss, me.localizedDescription.UTF8String ?: "(none)");
    if (wrote) [[NSFileManager defaultManager] removeItemAtURL:url error:NULL];
    return 0;
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
        printf("device: %s (%s)\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String);
        queue = [dev newCommandQueue];
        int rc = 0;
        if (@available(macOS 26.0, *)) rc = body();
        else { printf("SKIP: MTL4Compiler needs macOS 26\n"); return 5; }
        if (rc) return rc;
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return gpu_error ? 3 : nfail ? 4 : 0;
    }
}
