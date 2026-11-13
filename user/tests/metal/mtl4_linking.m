// SPDX-License-Identifier: MIT
// mtl4_linking: binary functions, dynamic linking, and the pipeline kinds the Arc B580 driver refuses
// (nullmoth/plugin/NVMTLLibraryLoad.m, NVMTLMetal4.m; docs/design/DESIGN-mtl4-compiler.md).
//
// Each pipeline that is made runs, so a case checks what it computes or draws:
//   Metal 3 (always):
//     1. a [[visible]] function made with MTLFunctionOptionCompileToBinary answers that option;
//     2. a kernel calling through a visible function table whose functions are the pipeline's binaryFunctions;
//     3. newComputePipelineStateWithAdditionalBinaryFunctions: a third function added, the table now three wide;
//     4. a fragment function calling through a fragment visible function table of binary functions, and the pipeline
//        with one more fragment binary function added;
//     5. a tile pipeline: made and run where the device has tile shading (Apple GPU families 4+) and on the B580 driver
//        (render-pass splitting, docs/design/DESIGN-tile-shading.md), refused with an error everywhere else.
//   Metal 4 (MTLGPUFamilyMetal4 and an MTL4Compiler; SKIP otherwise):
//     6. MTL4BinaryFunction from a function descriptor;
//     7. a compute pipeline with a dynamic linking descriptor's binary functions, its handles from
//        functionHandleWithBinaryFunction:; a function it does not link has no handle;
//     8. newComputePipelineStateWithBinaryFunctions: a third function;
//     9. functionHandleWithName: names a statically linked function, not a binary one;
//    10. a render pipeline whose fragment linking descriptor gives the table's functions;
//    11. a dynamic linking descriptor's preloadedLibraries: a kernel calling a function of an MTLDynamicLibrary;
//    12. MTL4TileRenderPipelineDescriptor (as 5);
//    13. MTL4BinaryFunctionOptionPipelineIndependent: made (where supported; refused with an error otherwise), one handle
//        dispatching the function from tables of two pipelines that list it at different places;
//    14. a machine-learning pipeline from an empty descriptor: nil and an error (Apple's M1 too);
//    15. a pipelines script from a serializer: Apple's JSON, or nil and an error where the device has none;
//    16. archives keep binary functions: a recorded one is found, another is not.
// Expected answers are Apple's (run with --any on an Apple GPU Mac as the reference). Each case prints PASS, FAIL or SKIP.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1 ./mtl4_linking   (B580; without NVMTL_METAL4=1 the Metal 4 cases SKIP)
//   ./mtl4_linking --any                                                the first Metal device when no B580 is present
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 3 GPU error, 4 wrong results.
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
     "[[visible]] float op_add(float x) { return x + 1.0f; }\n"
     "[[visible]] float op_mul(float x) { return x * 3.0f; }\n"
     "[[visible]] float op_neg(float x) { return -x; }\n"
     "kernel void apply(device float *o [[buffer(0)]], visible_function_table<float(float)> t [[buffer(1)]], constant uint &n [[buffer(2)]],\n"
     "                  uint i [[thread_position_in_grid]]) { o[i] = t[i % n](o[i]); }\n"
     "[[visible]] float4 col_red(float2 p) { return float4(1, 0, 0, 1); }\n"
     "[[visible]] float4 col_green(float2 p) { return float4(0, 1, 0, 1); }\n"
     "[[visible]] float4 col_blue(float2 p) { return float4(0, 0, 1, 1); }\n"
     "struct V { float4 p [[position]]; };\n"
     "vertex V vs(uint vid [[vertex_id]]) { float2 q = float2((vid << 1) & 2, vid & 2); V o; o.p = float4(q * 2 - 1, 0, 1); return o; }\n"
     "fragment float4 fs_tab(V in [[stage_in]], visible_function_table<float4(float2)> t [[buffer(0)]], constant uint &slot [[buffer(1)]]) {\n"
     "    return t[slot](in.p.xy); }\n";
// A tile function: every pixel of the tile's colour attachment gets green 1 (red stays).
static NSString *const kTileSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "struct TileColor { half4 c [[color(0)]]; };\n"
     "kernel void tile_green(imageblock<TileColor, imageblock_layout_implicit> ib, ushort2 tid [[thread_position_in_threadgroup]]) {\n"
     "    TileColor v = ib.read(tid); v.c.g = 1.0h; ib.write(v, tid); }\n";
static NSString *const kDylibSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "float dyl_scale(float x) { return x * 5.0f + 0.25f; }\n";
static NSString *const kDylibUser =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "extern float dyl_scale(float x);\n"
     "kernel void use_dylib(device float *o [[buffer(0)]], uint i [[thread_position_in_grid]]) { o[i] = dyl_scale(o[i]); }\n";

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

enum { N = 96 };
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static int gpu_error;

static void finish(id<MTLCommandBuffer> cb)
{
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { fprintf(stderr, "GPU error: %s\n", cb.error.localizedDescription.UTF8String ?: "?"); gpu_error = 1; }
}
static id<MTLBuffer> iota(void)
{
    id<MTLBuffer> b = [dev newBufferWithLength:N * sizeof(float) options:MTLResourceStorageModeShared];
    float *p = b.contents; for (int i = 0; i < N; i++) p[i] = (float)i;
    return b;
}
// The table holds `handles` (n of them); `ps` runs `apply` over o = 0..N-1.
static id<MTLBuffer> run_apply(id<MTLComputePipelineState> ps, NSArray<id<MTLFunctionHandle>> *handles)
{
    MTLVisibleFunctionTableDescriptor *td = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
    td.functionCount = handles.count;
    id<MTLVisibleFunctionTable> t = [ps newVisibleFunctionTableWithDescriptor:td];
    if (!t) return nil;
    for (NSUInteger k = 0; k < handles.count; k++) [t setFunction:handles[k] atIndex:k];
    id<MTLBuffer> o = iota();
    const uint32_t n = (uint32_t)handles.count;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:ps]; [e setBuffer:o offset:0 atIndex:0]; [e setVisibleFunctionTable:t atBufferIndex:1]; [e setBytes:&n length:4 atIndex:2];
    [e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)];
    [e endEncoding];
    finish(cb);
    return o;
}
// op k of the ops "add, mul, neg" applied to x
static float op(int k, float x) { return k == 0 ? x + 1.0f : k == 1 ? x * 3.0f : -x; }
// o[i] == ops[i % n](i); *bad = the first index that is not
static int apply_ok(id<MTLBuffer> o, const int *ops, int n, int *bad)
{
    if (!o) { *bad = -1; return 0; }
    const float *p = o.contents;
    for (int i = 0; i < N; i++) if (p[i] != op(ops[i % n], (float)i)) { *bad = i; return 0; }
    return 1;
}
static id<MTLTexture> target(void)
{
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];
    td.usage = MTLTextureUsageRenderTarget; td.storageMode = MTLStorageModeShared;
    return [dev newTextureWithDescriptor:td];
}
// draws fs_tab with the fragment table holding `handles`, reading slot `slot`; answers the pixel at (3, 3), all pixels equal
static uint32_t draw_tab(id<MTLRenderPipelineState> rp, NSArray<id<MTLFunctionHandle>> *handles, uint32_t slot, int *same)
{
    MTLVisibleFunctionTableDescriptor *td = [MTLVisibleFunctionTableDescriptor visibleFunctionTableDescriptor];
    td.functionCount = handles.count;
    id<MTLVisibleFunctionTable> t = [rp newVisibleFunctionTableWithDescriptor:td stage:MTLRenderStageFragment];
    *same = 0;
    if (!t) return 0;
    for (NSUInteger k = 0; k < handles.count; k++) [t setFunction:handles[k] atIndex:k];
    id<MTLTexture> rt = target();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = rt; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
    [re setRenderPipelineState:rp]; [re setFragmentVisibleFunctionTable:t atBufferIndex:0]; [re setFragmentBytes:&slot length:4 atIndex:1];
    [re drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [re endEncoding];
    finish(cb);
    uint32_t px[64] = { 0 };
    [rt getBytes:px bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    *same = 1; for (int i = 0; i < 64; i++) *same &= px[i] == px[0];
    return px[27];
}
// a tile pipeline (Metal 3 or Metal 4) over an 8x8 target cleared to red: answers the pixel at (3, 3), all pixels equal
static uint32_t run_tile(id<MTLRenderPipelineState> tp, int *same)
{
    id<MTLTexture> rt = target();
    MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = rt; pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(1, 0, 0, 1); pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.tileWidth = 16; pass.tileHeight = 16;
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:pass];
    [re setRenderPipelineState:tp]; [re dispatchThreadsPerTile:MTLSizeMake(re.tileWidth, re.tileHeight, 1)]; [re endEncoding];
    finish(cb);
    uint32_t px[64] = { 0 };
    [rt getBytes:px bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0];
    *same = 1; for (int i = 0; i < 64; i++) *same &= px[i] == px[0];
    return px[27];
}
// A refusal is clean when the object is nil and the error names what is missing.
static int refused(id obj, NSError *e, NSString *word) { return !obj && e && [e.localizedDescription rangeOfString:word options:NSCaseInsensitiveSearch].location != NSNotFound; }

static id<MTLFunction> binary_fn(id<MTLLibrary> lib, NSString *name)
{
    MTLFunctionDescriptor *d = [MTLFunctionDescriptor functionDescriptor];
    d.name = name; d.options = MTLFunctionOptionCompileToBinary;
    NSError *e = nil; id<MTLFunction> f = [lib newFunctionWithDescriptor:d error:&e];
    if (!f) fprintf(stderr, "newFunctionWithDescriptor %s: %s\n", name.UTF8String, say(e));
    return f;
}

static int metal3(id<MTLLibrary> lib)
{
    NSError *err = nil;
    // 1. CompileToBinary
    id<MTLFunction> add = binary_fn(lib, @"op_add"), mul = binary_fn(lib, @"op_mul"), neg = binary_fn(lib, @"op_neg");
    if (!add || !mul || !neg) return 2;
    check((add.options & MTLFunctionOptionCompileToBinary) && add.functionType == MTLFunctionTypeVisible,
          "Metal 3: [[visible]] op_add made with MTLFunctionOptionCompileToBinary: options 0x%lx, type %lu", (unsigned long)add.options, (unsigned long)add.functionType);

    // 2. binaryFunctions through a visible function table
    MTLComputePipelineDescriptor *cd = [MTLComputePipelineDescriptor new];
    cd.computeFunction = [lib newFunctionWithName:@"apply"];
    MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.binaryFunctions = @[add, mul];
    cd.linkedFunctions = lf; cd.supportAddingBinaryFunctions = YES;
    id<MTLComputePipelineState> p2 = [dev newComputePipelineStateWithDescriptor:cd options:MTLPipelineOptionNone reflection:nil error:&err];
    id<MTLFunctionHandle> ha = [p2 functionHandleWithFunction:add], hm = [p2 functionHandleWithFunction:mul];
    int bad = -1; const int am[] = { 0, 1 }, amn[] = { 0, 1, 2 };
    id<MTLBuffer> o = (p2 && ha && hm) ? run_apply(p2, @[ha, hm]) : nil;
    check(p2 && ha && hm && apply_ok(o, am, 2, &bad), "Metal 3: kernel through a table of binaryFunctions [add, mul]: pipeline %p, handles %p %p, first wrong element %d%s%s",
          (__bridge void *)p2, (__bridge void *)ha, (__bridge void *)hm, bad, p2 ? "" : " - ", p2 ? "" : say(err));

    // 3. a function added to the pipeline
    id<MTLComputePipelineState> p3 = p2 ? [p2 newComputePipelineStateWithAdditionalBinaryFunctions:@[neg] error:&err] : nil;
    id<MTLFunctionHandle> ha3 = [p3 functionHandleWithFunction:add], hm3 = [p3 functionHandleWithFunction:mul], hn3 = [p3 functionHandleWithFunction:neg];
    o = (p3 && ha3 && hm3 && hn3) ? run_apply(p3, @[ha3, hm3, hn3]) : nil;
    check(p3 && hn3 && apply_ok(o, amn, 3, &bad), "Metal 3: newComputePipelineStateWithAdditionalBinaryFunctions [neg] -> %p, three-wide table, first wrong element %d%s%s",
          (__bridge void *)p3, bad, p3 ? "" : " - ", p3 ? "" : say(err));

    // 4. fragment binary functions
    id<MTLFunction> red = binary_fn(lib, @"col_red"), green = binary_fn(lib, @"col_green"), blue = binary_fn(lib, @"col_blue");
    MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
    rd.vertexFunction = [lib newFunctionWithName:@"vs"]; rd.fragmentFunction = [lib newFunctionWithName:@"fs_tab"];
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    MTLLinkedFunctions *fl = [MTLLinkedFunctions linkedFunctions]; fl.binaryFunctions = @[red, green];
    rd.fragmentLinkedFunctions = fl; rd.supportAddingFragmentBinaryFunctions = YES;
    id<MTLRenderPipelineState> rp = [dev newRenderPipelineStateWithDescriptor:rd error:&err];
    id<MTLFunctionHandle> hr = [rp functionHandleWithFunction:red stage:MTLRenderStageFragment], hg = [rp functionHandleWithFunction:green stage:MTLRenderStageFragment];
    int same = 0; uint32_t px = (rp && hr && hg) ? draw_tab(rp, @[hr, hg], 1, &same) : 0;
    check(rp && same && px == 0xff00ff00u, "Metal 3: fragment table of binaryFunctions [red, green], slot 1: 0x%08x everywhere %d (want 0xff00ff00)%s%s",
          px, same, rp ? "" : " - ", rp ? "" : say(err));
    MTLRenderPipelineFunctionsDescriptor *add4 = [MTLRenderPipelineFunctionsDescriptor new]; add4.fragmentAdditionalBinaryFunctions = @[blue];
    id<MTLRenderPipelineState> rp2 = rp ? [rp newRenderPipelineStateWithAdditionalBinaryFunctions:add4 error:&err] : nil;
    id<MTLFunctionHandle> hr2 = [rp2 functionHandleWithFunction:red stage:MTLRenderStageFragment], hg2 = [rp2 functionHandleWithFunction:green stage:MTLRenderStageFragment],
                          hb2 = [rp2 functionHandleWithFunction:blue stage:MTLRenderStageFragment];
    px = (rp2 && hr2 && hg2 && hb2) ? draw_tab(rp2, @[hr2, hg2, hb2], 2, &same) : 0;
    check(rp2 && hb2 && same && px == 0xffff0000u, "Metal 3: newRenderPipelineStateWithAdditionalBinaryFunctions [blue], slot 2: 0x%08x (want 0xffff0000)%s%s",
          px, rp2 ? "" : " - ", rp2 ? "" : say(err));

    // 5. tile pipeline
    const BOOL tiles = [dev supportsFamily:MTLGPUFamilyApple4] || [NSStringFromClass([dev class]) isEqualToString:@"NVMTLDevice"];
    id<MTLLibrary> tl = [dev newLibraryWithSource:kTileSource options:nil error:&err];
    if (!tl) { check(0, "Metal 3: tile library: %s", say(err)); return 0; }
    MTLTileRenderPipelineDescriptor *td = [MTLTileRenderPipelineDescriptor new];
    td.tileFunction = [tl newFunctionWithName:@"tile_green"]; td.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; td.threadgroupSizeMatchesTileSize = YES;
    err = nil;
    id<MTLRenderPipelineState> tp = [dev newRenderPipelineStateWithTileDescriptor:td options:MTLPipelineOptionNone reflection:nil error:&err];
    if (tiles) {
        px = tp ? run_tile(tp, &same) : 0;
        check(tp && same && px == 0xff00ffffu, "Metal 3: tile pipeline (tile shading or render-pass splitting): tile_green over red -> 0x%08x everywhere %d (want 0xff00ffff)%s%s",
              px, same, tp ? "" : " - ", tp ? "" : say(err));
    } else
        check(refused(tp, err, @"tile"), "Metal 3: tile pipeline refused (no Apple4 family, no tile shading): %p, %s", (__bridge void *)tp, say(err));
    return 0;
}

API_AVAILABLE(macos(26.0))
static MTL4LibraryFunctionDescriptor *fn(id<MTLLibrary> lib, NSString *name)
{
    MTL4LibraryFunctionDescriptor *d = [MTL4LibraryFunctionDescriptor new]; d.library = lib; d.name = name; return d;
}
API_AVAILABLE(macos(26.0))
static id<MTL4BinaryFunction> bin(id<MTL4Compiler> comp, id<MTLLibrary> lib, NSString *name, MTL4BinaryFunctionOptions opts, NSError **err)
{
    MTL4BinaryFunctionDescriptor *d = [MTL4BinaryFunctionDescriptor new];
    d.name = [@"bin_" stringByAppendingString:name]; d.functionDescriptor = fn(lib, name); d.options = opts;
    return [comp newBinaryFunctionWithDescriptor:d compilerTaskOptions:nil error:err];
}

API_AVAILABLE(macos(26.0))
static int metal4(void)
{
    NSError *err = nil;
    if (![dev supportsFamily:MTLGPUFamilyMetal4]) { skip("Metal 4 cases: no MTLGPUFamilyMetal4 (B580: NVMTL_PROCESS_ENV=1 NVMTL_METAL4=1)"); return 0; }
    id<MTL4Compiler> comp = [dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    if (!comp) { skip("Metal 4 cases: no MTL4Compiler (%s)", say(err)); return 0; }
    MTL4LibraryDescriptor *ld = [MTL4LibraryDescriptor new]; ld.source = kSource; ld.name = @"mtl4_linking";
    id<MTLLibrary> lib = [comp newLibraryWithDescriptor:ld error:&err];
    if (!lib) { fprintf(stderr, "FAIL: MTL4 newLibraryWithDescriptor: %s\n", say(err)); return 2; }

    // 6. binary functions
    id<MTL4BinaryFunction> add = bin(comp, lib, @"op_add", 0, &err), mul = bin(comp, lib, @"op_mul", 0, &err), neg = bin(comp, lib, @"op_neg", 0, &err);
    check(add && mul && neg, "Metal 4: newBinaryFunctionWithDescriptor op_add/op_mul/op_neg: %p %p %p (%s, name %s)%s%s", (__bridge void *)add, (__bridge void *)mul,
          (__bridge void *)neg, add ? NSStringFromClass([(id)add class]).UTF8String : "-", add.name.UTF8String ?: "(nil)", add ? "" : " - ", add ? "" : say(err));
    if (!add || !mul || !neg) return 0;

    // 7. dynamic linking: the binary functions in the kernel's table
    MTL4ComputePipelineDescriptor *cd = [MTL4ComputePipelineDescriptor new];
    cd.computeFunctionDescriptor = fn(lib, @"apply"); cd.supportBinaryLinking = YES;
    MTL4PipelineStageDynamicLinkingDescriptor *dl = [MTL4PipelineStageDynamicLinkingDescriptor new]; dl.binaryLinkedFunctions = @[add, mul];
    id<MTLComputePipelineState> p7 = [comp newComputePipelineStateWithDescriptor:cd dynamicLinkingDescriptor:dl compilerTaskOptions:nil error:&err];
    id<MTLFunctionHandle> ha = [p7 functionHandleWithBinaryFunction:add], hm = [p7 functionHandleWithBinaryFunction:mul], hx = [p7 functionHandleWithBinaryFunction:neg];
    int bad = -1; const int am[] = { 0, 1 }, amn[] = { 0, 1, 2 };
    id<MTLBuffer> o = (p7 && ha && hm) ? run_apply(p7, @[ha, hm]) : nil;
    check(p7 && ha && hm && apply_ok(o, am, 2, &bad), "Metal 4: dynamic linking [add, mul] into a kernel's table: pipeline %p, handles %p %p, first wrong element %d%s%s",
          (__bridge void *)p7, (__bridge void *)ha, (__bridge void *)hm, bad, p7 ? "" : " - ", p7 ? "" : say(err));
    check(p7 && !hx, "Metal 4: functionHandleWithBinaryFunction: of a function the pipeline does not link -> %p (want nil)", (__bridge void *)hx);

    // 8. a binary function added
    id<MTLComputePipelineState> p8 = p7 ? [p7 newComputePipelineStateWithBinaryFunctions:@[neg] error:&err] : nil;
    id<MTLFunctionHandle> ha8 = [p8 functionHandleWithBinaryFunction:add], hm8 = [p8 functionHandleWithBinaryFunction:mul], hn8 = [p8 functionHandleWithBinaryFunction:neg];
    o = (p8 && ha8 && hm8 && hn8) ? run_apply(p8, @[ha8, hm8, hn8]) : nil;
    check(p8 && hn8 && apply_ok(o, amn, 3, &bad), "Metal 4: newComputePipelineStateWithBinaryFunctions [neg] -> %p, three-wide table, first wrong element %d%s%s",
          (__bridge void *)p8, bad, p8 ? "" : " - ", p8 ? "" : say(err));

    // 9. functionHandleWithName: a statically linked function has one by name, a binary function does not
    MTL4ComputePipelineDescriptor *sd = [MTL4ComputePipelineDescriptor new]; sd.computeFunctionDescriptor = fn(lib, @"apply"); sd.supportBinaryLinking = YES;
    MTL4StaticLinkingDescriptor *sl = [MTL4StaticLinkingDescriptor new]; sl.functionDescriptors = @[fn(lib, @"op_neg")]; sd.staticLinkingDescriptor = sl;
    MTL4PipelineStageDynamicLinkingDescriptor *dl9 = [MTL4PipelineStageDynamicLinkingDescriptor new]; dl9.binaryLinkedFunctions = @[add];
    id<MTLComputePipelineState> p9 = [comp newComputePipelineStateWithDescriptor:sd dynamicLinkingDescriptor:dl9 compilerTaskOptions:nil error:&err];
    id<MTLFunctionHandle> hn = [p9 functionHandleWithName:@"op_neg"], hb = [p9 functionHandleWithName:@"op_add"], hbb = [p9 functionHandleWithName:add.name ?: @"bin_op_add"];
    id<MTLFunctionHandle> ha9 = [p9 functionHandleWithBinaryFunction:add];
    const int na[] = { 2, 0 };
    o = (p9 && hn && ha9) ? run_apply(p9, @[hn, ha9]) : nil;
    check(p9 && hn && !hb && !hbb && ha9 && apply_ok(o, na, 2, &bad),
          "Metal 4: functionHandleWithName: static op_neg %p, binary op_add by name %p / %p (want nil), by binary function %p; table [neg, add] first wrong element %d%s%s",
          (__bridge void *)hn, (__bridge void *)hb, (__bridge void *)hbb, (__bridge void *)ha9, bad, p9 ? "" : " - ", p9 ? "" : say(err));

    // 10. render: the fragment linking descriptor's binary functions
    id<MTL4BinaryFunction> red = bin(comp, lib, @"col_red", 0, &err), green = bin(comp, lib, @"col_green", 0, &err);
    MTL4RenderPipelineDescriptor *rd = [MTL4RenderPipelineDescriptor new];
    rd.vertexFunctionDescriptor = fn(lib, @"vs"); rd.fragmentFunctionDescriptor = fn(lib, @"fs_tab"); rd.supportFragmentBinaryLinking = YES;
    rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    MTL4RenderPipelineDynamicLinkingDescriptor *rdl = [MTL4RenderPipelineDynamicLinkingDescriptor new];
    if (red && green) rdl.fragmentLinkingDescriptor.binaryLinkedFunctions = @[red, green];
    id<MTLRenderPipelineState> rp = (red && green) ? [comp newRenderPipelineStateWithDescriptor:rd dynamicLinkingDescriptor:rdl compilerTaskOptions:nil error:&err] : nil;
    id<MTLFunctionHandle> hr = [rp functionHandleWithBinaryFunction:red stage:MTLRenderStageFragment], hg = [rp functionHandleWithBinaryFunction:green stage:MTLRenderStageFragment];
    int same = 0; uint32_t px = (rp && hr && hg) ? draw_tab(rp, @[hr, hg], 1, &same) : 0;
    check(rp && same && px == 0xff00ff00u, "Metal 4: render pipeline, fragment dynamic linking [red, green], slot 1: 0x%08x everywhere %d (want 0xff00ff00)%s%s",
          px, same, rp ? "" : " - ", rp ? "" : say(err));

    // 11. preloaded libraries
    if (![dev supportsDynamicLibraries]) skip("Metal 4: preloadedLibraries: the device has no dynamic libraries");
    else {
        // Apple's M1 resolves a dynamic library's functions only once it is at its install name (serialized there), and
        // a library that calls one must name it at compile time (MTLCompileOptions.libraries); the pipeline then gets it
        // again as a preloaded library.
        NSString *path = [NSString stringWithFormat:@"%@/mtl4_linking-%d-dyl.metallib", NSTemporaryDirectory(), getpid()];
        MTLCompileOptions *co = [MTLCompileOptions new]; co.libraryType = MTLLibraryTypeDynamic; co.installName = path;
        MTL4LibraryDescriptor *dld = [MTL4LibraryDescriptor new]; dld.source = kDylibSource; dld.options = co; dld.name = @"dyl";
        id<MTLLibrary> dsrc = [comp newLibraryWithDescriptor:dld error:&err];
        id<MTLDynamicLibrary> dyl = dsrc ? [comp newDynamicLibrary:dsrc error:&err] : nil;
        if (dyl && ![dyl serializeToURL:[NSURL fileURLWithPath:path] error:&err]) dyl = nil;
        MTLCompileOptions *uo = [MTLCompileOptions new]; if (dyl) uo.libraries = @[dyl];
        MTL4LibraryDescriptor *uld = [MTL4LibraryDescriptor new]; uld.source = kDylibUser; uld.name = @"dyl_user"; uld.options = uo;
        id<MTLLibrary> ul = dyl ? [comp newLibraryWithDescriptor:uld error:&err] : nil;
        MTL4ComputePipelineDescriptor *ud = [MTL4ComputePipelineDescriptor new]; ud.computeFunctionDescriptor = fn(ul, @"use_dylib");
        MTL4PipelineStageDynamicLinkingDescriptor *udl = [MTL4PipelineStageDynamicLinkingDescriptor new]; if (dyl) udl.preloadedLibraries = @[dyl];
        id<MTLComputePipelineState> p11 = ul ? [comp newComputePipelineStateWithDescriptor:ud dynamicLinkingDescriptor:udl compilerTaskOptions:nil error:&err] : nil;
        id<MTLBuffer> b = iota();
        if (p11) {
            id<MTLCommandBuffer> cb = [queue commandBuffer]; id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
            [e setComputePipelineState:p11]; [e setBuffer:b offset:0 atIndex:0];
            [e dispatchThreads:MTLSizeMake(N, 1, 1) threadsPerThreadgroup:MTLSizeMake(32, 1, 1)]; [e endEncoding]; finish(cb);
        }
        const float *bp = b.contents; int ok = p11 != nil;
        for (int i = 0; ok && i < N; i++) ok = bp[i] == (float)i * 5.0f + 0.25f;
        check(ok, "Metal 4: dynamic linking preloadedLibraries: use_dylib calls dyl_scale of an MTLDynamicLibrary (%p): o[7] = %g (want 35.25)%s%s",
              (__bridge void *)dyl, bp[7], ok ? "" : " - ", ok ? "" : say(err));
        [[NSFileManager defaultManager] removeItemAtPath:path error:NULL];
    }

    // 12. tile pipeline through the compiler
    const BOOL tiles = [dev supportsFamily:MTLGPUFamilyApple4] || [NSStringFromClass([dev class]) isEqualToString:@"NVMTLDevice"];
    MTL4LibraryDescriptor *tld = [MTL4LibraryDescriptor new]; tld.source = kTileSource;
    id<MTLLibrary> tl = [comp newLibraryWithDescriptor:tld error:&err];
    MTL4TileRenderPipelineDescriptor *td = [MTL4TileRenderPipelineDescriptor new];
    td.tileFunctionDescriptor = fn(tl, @"tile_green"); td.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm; td.threadgroupSizeMatchesTileSize = YES;
    err = nil;
    id<MTLRenderPipelineState> tp = tl ? [comp newRenderPipelineStateWithDescriptor:td compilerTaskOptions:nil error:&err] : nil;
    if (tiles) {
        px = tp ? run_tile(tp, &same) : 0;
        check(tp && same && px == 0xff00ffffu, "Metal 4: MTL4TileRenderPipelineDescriptor (tile shading or render-pass splitting): 0x%08x everywhere %d (want 0xff00ffff)%s%s",
              px, same, tp ? "" : " - ", tp ? "" : say(err));
    } else
        check(refused(tp, err, @"tile"), "Metal 4: MTL4TileRenderPipelineDescriptor refused (no tile shading): %p, %s", (__bridge void *)tp, say(err));

    // 13. pipeline-independent binary functions
    err = nil;
    id<MTL4BinaryFunction> pi = bin(comp, lib, @"op_mul", MTL4BinaryFunctionOptionPipelineIndependent, &err);
    if (pi) {
        MTL4PipelineStageDynamicLinkingDescriptor *a = [MTL4PipelineStageDynamicLinkingDescriptor new]; a.binaryLinkedFunctions = @[pi];
        MTL4PipelineStageDynamicLinkingDescriptor *b2 = [MTL4PipelineStageDynamicLinkingDescriptor new]; b2.binaryLinkedFunctions = @[add, pi];
        id<MTLComputePipelineState> pa = [comp newComputePipelineStateWithDescriptor:cd dynamicLinkingDescriptor:a compilerTaskOptions:nil error:&err];
        id<MTLComputePipelineState> pb = [comp newComputePipelineStateWithDescriptor:cd dynamicLinkingDescriptor:b2 compilerTaskOptions:nil error:&err];
        id<MTLFunctionHandle> h1 = [pa functionHandleWithBinaryFunction:pi], h2 = [pb functionHandleWithBinaryFunction:pi];
        // the handle of one pipeline in the table another pipeline reads
        const int mm[] = { 1 };
        o = (pa && pb && h1) ? run_apply(pb, @[h1]) : nil;
        int bad2 = -1;
        id<MTLBuffer> o2 = (pa && pb && h2) ? run_apply(pa, @[h2]) : nil;
        check(pa && pb && h1 && h2 && apply_ok(o, mm, 1, &bad) && apply_ok(o2, mm, 1, &bad2),
              "Metal 4: MTL4BinaryFunctionOptionPipelineIndependent: made; a handle from one pipeline (%llx) dispatches in another (%llx) and back; first wrong element %d / %d",
              h1.gpuResourceID._impl, h2.gpuResourceID._impl, bad, bad2);
    } else
        check(refused(pi, err, @"PipelineIndependent"), "Metal 4: MTL4BinaryFunctionOptionPipelineIndependent refused: %s", say(err));

    // 14. machine-learning pipeline
    err = nil;
    id mlp = [comp newMachineLearningPipelineStateWithDescriptor:[MTL4MachineLearningPipelineDescriptor new] error:&err];
    check(!mlp && err, "Metal 4: newMachineLearningPipelineStateWithDescriptor (empty descriptor): %p, %s", (__bridge void *)mlp, say(err));

    // 15. pipelines script
    MTL4PipelineDataSetSerializerDescriptor *sdd = [MTL4PipelineDataSetSerializerDescriptor new];
    sdd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureDescriptors;
    id<MTL4PipelineDataSetSerializer> ser = [dev newPipelineDataSetSerializerWithDescriptor:sdd];
    MTL4CompilerDescriptor *ccd = [MTL4CompilerDescriptor new]; ccd.pipelineDataSetSerializer = ser;
    id<MTL4Compiler> rec = ser ? [dev newCompilerWithDescriptor:ccd error:&err] : nil;
    MTL4LibraryDescriptor *pld = [MTL4LibraryDescriptor new]; pld.source = kSource;
    id<MTLLibrary> plib = rec ? [rec newLibraryWithDescriptor:pld error:&err] : nil;
    MTL4ComputePipelineDescriptor *pcd = [MTL4ComputePipelineDescriptor new]; pcd.computeFunctionDescriptor = fn(plib, @"apply");
    MTL4PipelineStageDynamicLinkingDescriptor *pdl = [MTL4PipelineStageDynamicLinkingDescriptor new];
    id<MTL4BinaryFunction> padd = plib ? bin(rec, plib, @"op_add", 0, &err) : nil;
    if (padd) pdl.binaryLinkedFunctions = @[padd];
    id<MTLComputePipelineState> pp = padd ? [rec newComputePipelineStateWithDescriptor:pcd dynamicLinkingDescriptor:pdl compilerTaskOptions:nil error:&err] : nil;
    err = nil;
    NSData *script = pp ? [ser serializeAsPipelinesScriptWithError:&err] : nil;
    id json = script ? [NSJSONSerialization JSONObjectWithData:script options:0 error:NULL] : nil;
    if (script)
        check([json isKindOfClass:[NSDictionary class]] && json[@"pipeline_descriptors"], "Metal 4: pipelines script: %lu B of JSON with pipeline_descriptors", (unsigned long)script.length);
    else
        check(pp && refused(script, err, @"script"), "Metal 4: pipelines script refused: %s", say(err));

    // 16. archives keep binary functions
    MTL4PipelineDataSetSerializerDescriptor *bsd = [MTL4PipelineDataSetSerializerDescriptor new];
    bsd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    id<MTL4PipelineDataSetSerializer> bser = [dev newPipelineDataSetSerializerWithDescriptor:bsd];
    MTL4CompilerDescriptor *bcd = [MTL4CompilerDescriptor new]; bcd.pipelineDataSetSerializer = bser;
    id<MTL4Compiler> brec = bser ? [dev newCompilerWithDescriptor:bcd error:&err] : nil;
    id<MTL4BinaryFunction> radd = brec ? bin(brec, lib, @"op_add", 0, &err) : nil;
    NSURL *url = [NSURL fileURLWithPath:[NSString stringWithFormat:@"%@/mtl4_linking-%d.mtl4archive", NSTemporaryDirectory(), getpid()]];
    BOOL wrote = radd && [bser serializeAsArchiveAndFlushToURL:url error:&err];
    id<MTL4Archive> ar = wrote ? [dev newArchiveWithURL:url error:&err] : nil;
    MTL4BinaryFunctionDescriptor *hit = [MTL4BinaryFunctionDescriptor new]; hit.name = @"bin_op_add"; hit.functionDescriptor = fn(lib, @"op_add");
    MTL4BinaryFunctionDescriptor *miss = [MTL4BinaryFunctionDescriptor new]; miss.name = @"bin_op_neg"; miss.functionDescriptor = fn(lib, @"op_neg");
    NSError *he = nil, *me = nil;
    id<MTL4BinaryFunction> got = [ar newBinaryFunctionWithDescriptor:hit error:&he], none = [ar newBinaryFunctionWithDescriptor:miss error:&me];
    check(ar && got && !none && me, "Metal 4: archive of a recorded binary function: written %d, found %p (%s), one not recorded %p (%s)",
          wrote, (__bridge void *)got, got ? "-" : say(he), (__bridge void *)none, say(me));
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
        printf("device: %s (%s), function pointers %d, dynamic libraries %d, Apple4 (tile shading) %d\n", dev.name.UTF8String,
               NSStringFromClass([dev class]).UTF8String, dev.supportsFunctionPointers, dev.supportsDynamicLibraries, [dev supportsFamily:MTLGPUFamilyApple4]);
        queue = [dev newCommandQueue];
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
        if (!lib) { fprintf(stderr, "FAIL: library: %s\n", say(err)); return 2; }
        int rc = metal3(lib);
        if (rc) return rc;
        if (@available(macOS 26.0, *)) rc = metal4();
        else skip("Metal 4 cases need macOS 26");
        if (rc) return rc;
        printf("%d PASS, %d FAIL, %d SKIP\n", npass, nfail, nskip);
        return gpu_error ? 3 : nfail ? 4 : 0;
    }
}
