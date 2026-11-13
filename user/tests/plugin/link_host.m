// SPDX-License-Identifier: MIT
// link_host: the plugin's binary functions and dynamic linking without a GPU. The plugin binary is loaded into this
// process (x86_64; under Rosetta on an Apple silicon Mac) and fed the AIR of user/tests/plugin/link/ (the shaders of
// user/tests/metal/mtl4_linking.m):
//   - nvmtl_link_stage links a kernel and a fragment function that call through a visible function table with
//     MTLLinkedFunctions.binaryFunctions, alone and after functions; the SPIR-V must hold a switch with a case per
//     function, and the handles a pipeline answers must be the functions' places in that list (a function added later
//     goes last, the others keep theirs);
//   - functionHandleWithName: names IR-linked functions only;
//   - a binary function without AIR on the table path is an error that names it;
//   - MTL4Compiler: a dynamic linking descriptor becomes the Metal 3 descriptor's binaryFunctions / preloadedLibraries /
//     call stack depth (a stand-in device records what the compiler asks it to build), binary functions wrap the
//     library's functions, a Metal 4 tile descriptor becomes the Metal 3 one, and the refusals (tile/object/mesh linking, machine learning pipelines,
//     pipelines scripts) are MTLDeviceErrorDomain code 1 with the feature in the message;
//   - MTL4BinaryFunctionOptionPipelineIndependent: the function's handle is one value (>= 0x40000000) in two pipelines
//     that list it at different places, and each linked switch has that case; the other functions keep their places.
//
//   link_host <NVMTLDriver.bundle or its NVMTLDriver binary> <user/tests/plugin/link>
//   link_host --make <NVMTLDriver.bundle> <dir>     writes the fixtures (needs an Apple GPU: Metal compiles the source)
//   build: clang -arch x86_64 -fobjc-arc -I user/tests/metal user/tests/plugin/link_host.m -framework Foundation -framework Metal
// It makes no Metal device when checking (plugin_image.h). Exit: 0 pass, 2 setup failure, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#import <objc/message.h>
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include "plugin_image.h"

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
     "struct V { float4 p [[position]]; };\n"
     "fragment float4 fs_tab(V in [[stage_in]], visible_function_table<float4(float2)> t [[buffer(0)]], constant uint &slot [[buffer(1)]]) {\n"
     "    return t[slot](in.p.xy); }\n";
static NSString *const kNames[] = { @"apply", @"fs_tab", @"op_add", @"op_mul", @"op_neg", @"col_red", @"col_green" };

static int npass, nfail;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
static void set_ivar(id o, const char *name, id v)
{
    Ivar iv = class_getInstanceVariable(object_getClass(o), name);
    if (!iv) { printf("setup: %s has no ivar %s\n", object_getClassName(o), name); exit(2); }
    object_setIvarWithStrongDefault(o, iv, v);
}
static id get_ivar(id o, const char *name)
{
    Ivar iv = class_getInstanceVariable(object_getClass(o), name);
    return iv ? object_getIvar(o, iv) : nil;
}
// a handle's ID (its gpuResourceID, read from the plugin's NVMTLFunctionHandle so the test also builds for macOS 15); 0 for nil
static unsigned long long hid(id h)
{
    Ivar iv = h ? class_getInstanceVariable(object_getClass(h), "_id") : NULL;
    return iv ? *(uint64_t *)((uint8_t *)(__bridge void *)h + ivar_getOffset(iv)) : 0;
}
static void set_bool(id o, const char *name, BOOL v)
{
    Ivar iv = class_getInstanceVariable(object_getClass(o), name);
    if (!iv) { printf("setup: %s has no ivar %s\n", object_getClassName(o), name); exit(2); }
    *(BOOL *)((uint8_t *)(__bridge void *)o + ivar_getOffset(iv)) = v;
}

// --make: the fixtures, from Apple's compiler and the plugin's own metallib reader
static int make_fixtures(const char *bundlePath, const char *outDir)
{
    NSBundle *b = [NSBundle bundleWithPath:@(bundlePath)]; NSError *e = nil;
    if (![b loadAndReturnError:&e]) { printf("setup: %s\n", e.localizedDescription.UTF8String); return 2; }
    void *h = dlopen(b.executablePath.fileSystemRepresentation, RTLD_NOW | RTLD_NOLOAD);
    NSDictionary *(*xm)(NSData *, NSDictionary **, NSDictionary **) = h ? dlsym(h, "nvmtl_translate_metallib") : NULL;
    if (!xm) { printf("setup: no nvmtl_translate_metallib\n"); return 2; }
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
    id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&e];
    SEL s = sel_registerName("libraryDataContents");
    NSData *mtlb = [(id)lib respondsToSelector:s] ? ((id (*)(id, SEL))objc_msgSend)(lib, s) : nil;
    if (!mtlb) { printf("setup: no library container (%s)\n", e.localizedDescription.UTF8String ?: "libraryDataContents"); return 2; }
    NSDictionary *stages = nil, *airs = nil;
    xm(mtlb, &stages, &airs);
    NSMutableDictionary *st = [NSMutableDictionary new];
    for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
        NSString *ll = airs[kNames[i]];
        if (!ll) { printf("setup: no AIR for %s\n", kNames[i].UTF8String); return 2; }
        [ll writeToFile:[@(outDir) stringByAppendingPathComponent:[kNames[i] stringByAppendingString:@".ll"]] atomically:YES encoding:NSUTF8StringEncoding error:NULL];
        st[kNames[i]] = stages[kNames[i]] ?: @"";
    }
    [[NSJSONSerialization dataWithJSONObject:st options:NSJSONWritingSortedKeys | NSJSONWritingPrettyPrinted error:NULL]
        writeToFile:[@(outDir) stringByAppendingPathComponent:@"stages.json"] atomically:YES];
    printf("wrote %zu modules and stages.json to %s\n", sizeof kNames / sizeof kNames[0], outDir);
    return 0;
}

// A stand-in for the device under MTL4Compiler: it records the Metal 3 descriptors it is asked to build and answers an
// object, so the conversion of Metal 4 descriptors can be checked without a GPU.
@interface LHDevice : NSObject { @public id last; } @end
@implementation LHDevice
- (id)newComputePipelineStateWithDescriptor:(id)d options:(MTLPipelineOption)o reflection:(id *)r error:(NSError **)e { last = [d copy]; return [NSObject new]; }
- (id)newRenderPipelineStateWithDescriptor:(id)d error:(NSError **)e { last = [d copy]; return [NSObject new]; }
- (id)newRenderPipelineStateWithDescriptor:(id)d options:(MTLPipelineOption)o reflection:(id *)r error:(NSError **)e { last = [d copy]; return [NSObject new]; }
- (id)newRenderPipelineStateWithMeshDescriptor:(id)d options:(MTLPipelineOption)o reflection:(id *)r error:(NSError **)e { last = [d copy]; return [NSObject new]; }
- (id)newRenderPipelineStateWithTileDescriptor:(id)d options:(MTLPipelineOption)o reflection:(id *)r error:(NSError **)e { last = [d copy]; return [NSObject new]; }
@end

// OpSwitch (opcode 251) case counts of a SPIR-V module
static NSArray<NSNumber *> *switch_cases(NSData *spv)
{
    NSMutableArray *out = [NSMutableArray new];
    const uint32_t *w = spv.bytes; size_t n = spv.length / 4;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc) break;
        if (op == 251 && wc >= 3) [out addObject:@((wc - 3) / 2)];
        i += wc;
    }
    return out;
}
// the case literals of the module's OpSwitch instructions (a 32-bit selector: one word per literal)
static NSArray<NSNumber *> *switch_values(NSData *spv)
{
    NSMutableArray *out = [NSMutableArray new];
    const uint32_t *w = spv.bytes; size_t n = spv.length / 4;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (op == 251) for (uint32_t k = 3; k + 1 < wc; k += 2) [out addObject:@(w[i + k])];
        i += wc;
    }
    return out;
}
static BOOL has_switch_of(NSData *spv, NSUInteger cases) { for (NSNumber *c in switch_cases(spv)) if (c.unsignedIntegerValue >= cases) return YES; return NO; }

static BOOL refusal(id obj, NSError *e, NSString *word)
{
    return !obj && [e.domain isEqualToString:@"MTLDeviceErrorDomain"] && e.code == 1 && [e.localizedDescription rangeOfString:word options:NSCaseInsensitiveSearch].location != NSNotFound;
}

API_AVAILABLE(macos(26.0))
static void metal4_checks(const char *bin, id lib, NSDictionary *fns)
{
    Class ccls = plugin_image_class(bin, "NVMTL4Compiler"), scls = plugin_image_class(bin, "NVMTL4Serializer"), pcls = plugin_image_class(bin, "NVMTLComputePipelineState");
    if (!ccls || !scls) { check(0, "MTL4: the plugin has no NVMTL4Compiler / NVMTL4Serializer"); return; }
    id<MTL4Compiler> comp = [ccls new]; LHDevice *dev = [LHDevice new]; set_ivar(comp, "_dev", dev);
    MTL4LibraryFunctionDescriptor *(^fd)(NSString *) = ^(NSString *n) { MTL4LibraryFunctionDescriptor *d = [MTL4LibraryFunctionDescriptor new]; d.library = lib; d.name = n; return d; };
    id<MTL4BinaryFunction> (^binf)(NSString *, MTL4BinaryFunctionOptions, NSError **) = ^(NSString *n, MTL4BinaryFunctionOptions o, NSError **e) {
        MTL4BinaryFunctionDescriptor *d = [MTL4BinaryFunctionDescriptor new]; d.name = [@"bin_" stringByAppendingString:n]; d.functionDescriptor = fd(n); d.options = o;
        return [comp newBinaryFunctionWithDescriptor:d compilerTaskOptions:nil error:e]; };
    NSError *err = nil;
    id<MTL4BinaryFunction> add = binf(@"op_add", 0, &err), mul = binf(@"op_mul", 0, &err);
    check(add && mul && [add.name isEqualToString:@"bin_op_add"] && add.functionType == MTLFunctionTypeVisible,
          "MTL4: newBinaryFunctionWithDescriptor: %s, name %s, type %lu", add ? NSStringFromClass([(id)add class]).UTF8String : "nil", add.name.UTF8String ?: "-", (unsigned long)add.functionType);

    // compute: dynamic linking into the Metal 3 descriptor
    MTL4ComputePipelineDescriptor *cd = [MTL4ComputePipelineDescriptor new]; cd.computeFunctionDescriptor = fd(@"apply"); cd.supportBinaryLinking = YES;
    MTL4StaticLinkingDescriptor *sl = [MTL4StaticLinkingDescriptor new]; sl.functionDescriptors = @[fd(@"op_neg")]; cd.staticLinkingDescriptor = sl;
    MTL4PipelineStageDynamicLinkingDescriptor *dl = [MTL4PipelineStageDynamicLinkingDescriptor new];
    dl.binaryLinkedFunctions = @[add, mul]; dl.maxCallStackDepth = 3;
    id ps = [comp newComputePipelineStateWithDescriptor:cd dynamicLinkingDescriptor:dl compilerTaskOptions:nil error:&err];
    MTLComputePipelineDescriptor *m3 = dev->last;
    NSArray *bf = m3.linkedFunctions.binaryFunctions, *lf = m3.linkedFunctions.functions;
    check(ps && bf.count == 2 && [[bf[0] name] isEqualToString:@"op_add"] && [[bf[1] name] isEqualToString:@"op_mul"] && lf.count == 1 &&
          [[lf[0] name] isEqualToString:@"op_neg"] && m3.maxCallStackDepth == 3 && m3.supportAddingBinaryFunctions,
          "MTL4: compute dynamic linking -> Metal 3 linkedFunctions functions [%s] binaryFunctions [%s, %s], call stack depth %lu, supportAddingBinaryFunctions %d%s%s",
          [lf.firstObject name].UTF8String ?: "-", bf.count > 0 ? [bf[0] name].UTF8String : "-", bf.count > 1 ? [bf[1] name].UTF8String : "-",
          (unsigned long)m3.maxCallStackDepth, m3.supportAddingBinaryFunctions, ps ? "" : " - ", ps ? "" : err.localizedDescription.UTF8String);

    // handles of binary functions through the pipeline-state category
    if (pcls) {
        id p = [pcls new]; set_ivar(p, "_function", fns[@"apply"]);
        void (*keep)(id, int, MTLLinkedFunctions *) = dlsym(dlopen(bin, RTLD_NOW | RTLD_NOLOAD), "nvmtl_pipe_keep_handles");
        if (keep) keep(p, 0, m3.linkedFunctions);
        id<MTLFunctionHandle> ha = [(id<MTLComputePipelineState>)p functionHandleWithBinaryFunction:add], hn = [(id<MTLComputePipelineState>)p functionHandleWithName:@"op_neg"],
                              hb = [(id<MTLComputePipelineState>)p functionHandleWithName:@"op_add"];
        check(keep && hid(ha) == 2 && hid(hn) == 1 && !hb,
              "MTL4: functionHandleWithBinaryFunction: op_add -> id %llu (want 2, after the static op_neg = %llu); functionHandleWithName: op_add (binary) %p (want nil)",
              hid(ha), hid(hn), (__bridge void *)hb);
    }

    // render: fragment binary functions and preloaded libraries; tile / object / mesh linking refused
    MTL4RenderPipelineDescriptor *rd = [MTL4RenderPipelineDescriptor new];
    rd.vertexFunctionDescriptor = fd(@"op_add"); rd.fragmentFunctionDescriptor = fd(@"fs_tab"); rd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    id<MTL4BinaryFunction> red = binf(@"col_red", 0, &err), green = binf(@"col_green", 0, &err);
    MTL4RenderPipelineDynamicLinkingDescriptor *rdl = [MTL4RenderPipelineDynamicLinkingDescriptor new];
    rdl.fragmentLinkingDescriptor.binaryLinkedFunctions = @[red, green]; rdl.fragmentLinkingDescriptor.maxCallStackDepth = 2;
    dev->last = nil;
    id rp = [comp newRenderPipelineStateWithDescriptor:rd dynamicLinkingDescriptor:rdl compilerTaskOptions:nil error:&err];
    MTLRenderPipelineDescriptor *r3 = dev->last;
    check(rp && r3.fragmentLinkedFunctions.binaryFunctions.count == 2 && r3.maxFragmentCallStackDepth == 2 && !r3.vertexLinkedFunctions.binaryFunctions.count,
          "MTL4: render fragment dynamic linking -> fragmentLinkedFunctions.binaryFunctions %lu, max fragment call stack depth %lu%s%s",
          (unsigned long)r3.fragmentLinkedFunctions.binaryFunctions.count, (unsigned long)r3.maxFragmentCallStackDepth, rp ? "" : " - ", rp ? "" : err.localizedDescription.UTF8String);
    MTL4RenderPipelineDynamicLinkingDescriptor *tdl = [MTL4RenderPipelineDynamicLinkingDescriptor new]; tdl.tileLinkingDescriptor.binaryLinkedFunctions = @[red];
    err = nil; id tp = [comp newRenderPipelineStateWithDescriptor:rd dynamicLinkingDescriptor:tdl compilerTaskOptions:nil error:&err];
    check(refusal(tp, err, @"tile"), "MTL4: linking into tile functions refused: %s", err.localizedDescription.UTF8String ?: "(no error)");
    MTL4RenderPipelineDynamicLinkingDescriptor *odl = [MTL4RenderPipelineDynamicLinkingDescriptor new]; odl.objectLinkingDescriptor.binaryLinkedFunctions = @[red];
    err = nil; id op = [comp newRenderPipelineStateWithDescriptor:rd dynamicLinkingDescriptor:odl compilerTaskOptions:nil error:&err];
    check(refusal(op, err, @"object"), "MTL4: linking into object functions refused: %s", err.localizedDescription.UTF8String ?: "(no error)");
    MTL4MeshRenderPipelineDescriptor *md = [MTL4MeshRenderPipelineDescriptor new]; md.meshFunctionDescriptor = fd(@"apply");
    MTL4StaticLinkingDescriptor *msl = [MTL4StaticLinkingDescriptor new]; msl.functionDescriptors = @[fd(@"op_neg")]; md.meshStaticLinkingDescriptor = msl;
    err = nil; id mp = [comp newRenderPipelineStateWithDescriptor:md compilerTaskOptions:nil error:&err];
    check(refusal(mp, err, @"mesh"), "MTL4: static linking into a mesh function refused (it was dropped before): %s", err.localizedDescription.UTF8String ?: "(no error)");

    // tile pipelines (render-pass splitting): the Metal 4 descriptor becomes the Metal 3 tile descriptor; linking refused
    MTL4TileRenderPipelineDescriptor *td = [MTL4TileRenderPipelineDescriptor new]; td.tileFunctionDescriptor = fd(@"apply");
    td.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA16Float; td.rasterSampleCount = 4; td.threadgroupSizeMatchesTileSize = YES;
    err = nil; dev->last = nil; id tile = [comp newRenderPipelineStateWithDescriptor:td compilerTaskOptions:nil error:&err];
    MTLTileRenderPipelineDescriptor *t3 = [dev->last isKindOfClass:[MTLTileRenderPipelineDescriptor class]] ? dev->last : nil;
    check(tile && [t3.tileFunction.name isEqualToString:@"apply"] && t3.colorAttachments[0].pixelFormat == MTLPixelFormatRGBA16Float &&
          t3.rasterSampleCount == 4 && t3.threadgroupSizeMatchesTileSize,
          "MTL4: MTL4TileRenderPipelineDescriptor -> Metal 3 tile descriptor: function %s, format %lu, %lu samples%s%s", t3.tileFunction.name.UTF8String ?: "-",
          (unsigned long)t3.colorAttachments[0].pixelFormat, (unsigned long)t3.rasterSampleCount, tile ? "" : " - ", tile ? "" : err.localizedDescription.UTF8String);
    MTL4StaticLinkingDescriptor *tsl = [MTL4StaticLinkingDescriptor new]; tsl.functionDescriptors = @[fd(@"op_neg")]; td.staticLinkingDescriptor = tsl;
    err = nil; id tlink = [comp newRenderPipelineStateWithDescriptor:td compilerTaskOptions:nil error:&err];
    check(refusal(tlink, err, @"tile"), "MTL4: static linking into a tile function refused: %s", err.localizedDescription.UTF8String ?: "(no error)");
    // refusals
    err = nil; id ml = [comp newMachineLearningPipelineStateWithDescriptor:[MTL4MachineLearningPipelineDescriptor new] error:&err];
    check(refusal(ml, err, @"machine learning"), "MTL4: machine learning pipeline refused: %s", err.localizedDescription.UTF8String ?: "(no error)");
    // pipeline-independent binary functions: one handle value in every pipeline, the switch case of each link
    err = nil; id<MTL4BinaryFunction> pi = binf(@"op_mul", MTL4BinaryFunctionOptionPipelineIndependent, &err);
    id pif = pi ? get_ivar(pi, "_fn") : nil;
    id (*link)(id, MTLLinkedFunctions *, NSArray *, NSError **) = dlsym(dlopen(bin, RTLD_NOW | RTLD_NOLOAD), "nvmtl_link_stage");
    void (*keep)(id, int, MTLLinkedFunctions *) = dlsym(dlopen(bin, RTLD_NOW | RTLD_NOLOAD), "nvmtl_pipe_keep_handles");
    if (pi && pif && link && keep && pcls) {
        MTLLinkedFunctions *la = [MTLLinkedFunctions linkedFunctions]; la.binaryFunctions = @[pif];
        MTLLinkedFunctions *lb = [MTLLinkedFunctions linkedFunctions]; lb.binaryFunctions = @[fns[@"op_add"], fns[@"op_neg"], pif];
        NSError *ea = nil, *eb = nil;
        id ka = link(fns[@"apply"], la, nil, &ea), kb = link(fns[@"apply"], lb, nil, &eb);
        id pa = [pcls new]; set_ivar(pa, "_function", ka); keep(pa, 0, la);
        id pb = [pcls new]; set_ivar(pb, "_function", kb); keep(pb, 0, lb);
        id<MTLFunctionHandle> ha = [(id<MTLComputePipelineState>)pa functionHandleWithBinaryFunction:pi], hb = [(id<MTLComputePipelineState>)pb functionHandleWithBinaryFunction:pi],
                              hadd = [(id<MTLComputePipelineState>)pb functionHandleWithFunction:fns[@"op_add"]], hneg = [(id<MTLComputePipelineState>)pb functionHandleWithFunction:fns[@"op_neg"]];
        NSArray *ca = ka ? switch_values(get_ivar(ka, "_spirv")) : @[], *cb = kb ? switch_values(get_ivar(kb, "_spirv")) : @[];
        const unsigned long long v = hid(ha);
        check(ka && kb && v >= 0x40000000ull && hid(hb) == v && [ca containsObject:@(v)] && [cb containsObject:@(v)] && [cb containsObject:@1] && [cb containsObject:@2] &&
              hid(hadd) == 1 && hid(hneg) == 2,
              "MTL4: MTL4BinaryFunctionOptionPipelineIndependent: handle %#llx in a pipeline linking [pi] and %#llx in one linking [add, neg, pi] (want the same, >= 0x40000000); "
              "switch cases [%s] and [%s]; add/neg keep places %llu/%llu%s%s%s%s", v, hid(hb), [[ca componentsJoinedByString:@","] UTF8String] ?: "-",
              [[cb componentsJoinedByString:@","] UTF8String] ?: "-", hid(hadd), hid(hneg), ka ? "" : " - ", ka ? "" : ea.localizedDescription.UTF8String,
              kb ? "" : " - ", kb ? "" : eb.localizedDescription.UTF8String);
        // a second pipeline-independent function gets another value; a plain binary function of the same function none
        id<MTL4BinaryFunction> pi2 = binf(@"op_mul", MTL4BinaryFunctionOptionPipelineIndependent, &err), plain = binf(@"op_mul", 0, &err);
        id pf = [pcls new]; set_ivar(pf, "_function", ka);
        MTLLinkedFunctions *lc = [MTLLinkedFunctions linkedFunctions]; lc.binaryFunctions = @[get_ivar(plain, "_fn"), get_ivar(pi2, "_fn")]; keep(pf, 0, lc);
        id<MTLFunctionHandle> h2 = [(id<MTLComputePipelineState>)pf functionHandleWithBinaryFunction:pi2], hp = [(id<MTLComputePipelineState>)pf functionHandleWithBinaryFunction:plain];
        check(hid(h2) >= 0x40000000ull && hid(h2) != v && hid(hp) == 1, "MTL4: another pipeline-independent function %#llx (want a new value), a plain one of the same function %llu (want its place 1)",
              hid(h2), hid(hp));
    } else
        check(0, "MTL4: MTL4BinaryFunctionOptionPipelineIndependent: binary function %p (%s), plugin entry points %p/%p", (__bridge void *)pi,
              err.localizedDescription.UTF8String ?: "-", (void *)link, (void *)keep);
    id<MTL4PipelineDataSetSerializer> ser = [scls new];
    err = nil; NSData *script = [ser serializeAsPipelinesScriptWithError:&err];
    check(refusal(script, err, @"script"), "MTL4: pipelines script refused: %s", err.localizedDescription.UTF8String ?: "(no error)");
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (argc == 4 && !strcmp(argv[1], "--make")) return make_fixtures(argv[2], argv[3]);
        if (argc < 3) { fprintf(stderr, "usage: %s <NVMTLDriver.bundle | NVMTLDriver> <fixture dir>\n", argv[0]); return 2; }
        NSString *bin = plugin_image_binary(argv[1]);
        BOOL isDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:@(argv[1]) isDirectory:&isDir];
        NSBundle *bundle = isDir ? [NSBundle bundleWithPath:@(argv[1])] : nil;
        if (bundle) {
            NSError *e = nil;
            if (![bundle loadAndReturnError:&e]) { printf("setup: cannot load %s: %s\n", argv[1], e.localizedDescription.UTF8String); return 2; }
        } else if (!dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL)) { printf("setup: dlopen %s: %s\n", argv[1], dlerror()); return 2; }
        void *h = bin ? dlopen(bin.fileSystemRepresentation, RTLD_NOW | RTLD_NOLOAD) : NULL;
        if (!h) { printf("setup: %s is not loaded as %s\n", argv[1], bin ? bin.UTF8String : "(no executable)"); return 2; }
        id (*link)(id, MTLLinkedFunctions *, NSArray *, NSError **) = dlsym(h, "nvmtl_link_stage");
        void (*keep)(id, int, MTLLinkedFunctions *) = dlsym(h, "nvmtl_pipe_keep_handles");
        Class fcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLFunction"), pcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLComputePipelineState");
        Class lcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLLibrary");
        if (!link || !keep || !fcls || !pcls || !lcls) {
            printf("setup: the plugin does not export nvmtl_link_stage (%p) / nvmtl_pipe_keep_handles (%p) / its classes\n", (void *)link, (void *)keep); return 2; }
        if (plugin_image_foreign(bin.fileSystemRepresentation, (const void *)link, fcls, "setup")) return 2;

        NSString *dir = @(argv[2]);
        NSDictionary *stages = [NSJSONSerialization JSONObjectWithData:[NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:@"stages.json"]] options:0 error:NULL];
        NSMutableDictionary *fns = [NSMutableDictionary new], *airs = [NSMutableDictionary new];
        for (size_t i = 0; i < sizeof kNames / sizeof kNames[0]; i++) {
            NSString *ll = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:[kNames[i] stringByAppendingString:@".ll"]] encoding:NSUTF8StringEncoding error:NULL];
            if (!ll.length || !stages[kNames[i]]) { printf("setup: %s.ll or its stage missing in %s\n", kNames[i].UTF8String, dir.UTF8String); return 2; }
            airs[kNames[i]] = ll;
        }
        // the library the Metal 4 checks take their functions from, as a library load makes it (AIR only: translation on demand)
        id lib = [lcls new]; set_ivar(lib, "_airs", airs); set_ivar(lib, "_stages", stages); set_ivar(lib, "_fns", @{});
        for (NSString *n in airs) {
            id f = [fcls new];
            set_ivar(f, "_air", airs[n]); set_ivar(f, "_fname", n); set_ivar(f, "_stage", stages[n]); set_ivar(f, "_lib", lib);
            if ([n isEqualToString:@"apply"] || [n isEqualToString:@"fs_tab"]) set_bool(f, "_needsLink", YES);   // they read a table
            fns[n] = f;
        }
        id apply = fns[@"apply"], fs = fns[@"fs_tab"], add = fns[@"op_add"], mul = fns[@"op_mul"], neg = fns[@"op_neg"];

        // 1. kernel, binaryFunctions only; handles from the pipeline's list and from the linked function's
        NSError *e = nil;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.binaryFunctions = @[add, mul];
        id k1 = link(apply, lf, nil, &e);
        NSData *spv = get_ivar(k1, "_spirv");
        check(k1 && spv.length && has_switch_of(spv, 2), "link: kernel through a table of binaryFunctions [add, mul]: %lu B SPIR-V, switches with %s case(s)%s%s",
              (unsigned long)spv.length, [[switch_cases(spv) componentsJoinedByString:@","] UTF8String] ?: "-", k1 ? "" : " - ", k1 ? "" : e.localizedDescription.UTF8String);
        if (getenv("LINK_HOST_DUMP") && spv) [spv writeToFile:[@(getenv("LINK_HOST_DUMP")) stringByAppendingPathComponent:@"apply-add-mul.spv"] atomically:YES];
        id p1 = [pcls new]; set_ivar(p1, "_function", k1);
        id<MTLFunctionHandle> fa = [(id<MTLComputePipelineState>)p1 functionHandleWithFunction:add], fm = [(id<MTLComputePipelineState>)p1 functionHandleWithFunction:mul];
        keep(p1, 0, lf);
        id<MTLFunctionHandle> ka = [(id<MTLComputePipelineState>)p1 functionHandleWithFunction:add], km = [(id<MTLComputePipelineState>)p1 functionHandleWithFunction:mul],
                              kn = [(id<MTLComputePipelineState>)p1 functionHandleWithFunction:neg];
        check(hid(fa) == 1 && hid(fm) == 2 && hid(ka) == 1 && hid(km) == 2 && !kn,
              "link: handles add/mul = %llu/%llu from the linked function's list, %llu/%llu from the pipeline's (want 1/2); neg (not linked) %p",
              hid(fa), hid(fm), hid(ka), hid(km), (__bridge void *)kn);

        // 2. a function added later goes last; the others keep their places
        MTLLinkedFunctions *lf3 = [MTLLinkedFunctions linkedFunctions]; lf3.binaryFunctions = @[add, mul, neg];
        id k3 = link(apply, lf3, nil, &e);
        NSData *spv3 = get_ivar(k3, "_spirv");
        id p3 = [pcls new]; set_ivar(p3, "_function", k3); keep(p3, 0, lf3);
        id<MTLFunctionHandle> a3 = [(id<MTLComputePipelineState>)p3 functionHandleWithFunction:add], m3 = [(id<MTLComputePipelineState>)p3 functionHandleWithFunction:mul],
                              n3 = [(id<MTLComputePipelineState>)p3 functionHandleWithFunction:neg];
        check(k3 && has_switch_of(spv3, 3) && hid(a3) == 1 && hid(m3) == 2 && hid(n3) == 3,
              "link: [add, mul] + neg: switches with %s case(s) (want one >= 3), handles %llu/%llu/%llu (want 1/2/3)%s%s",
              [[switch_cases(spv3) componentsJoinedByString:@","] UTF8String] ?: "-", hid(a3), hid(m3), hid(n3),
              k3 ? "" : " - ", k3 ? "" : e.localizedDescription.UTF8String);

        // 3. functions, then binary functions; functionHandleWithName: names only the IR-linked one
        MTLLinkedFunctions *lfm = [MTLLinkedFunctions linkedFunctions]; lfm.functions = @[neg]; lfm.binaryFunctions = @[add];
        id km2 = link(apply, lfm, nil, &e);
        id pm = [pcls new]; set_ivar(pm, "_function", km2); keep(pm, 0, lfm);
        id<MTLFunctionHandle> mn = [(id<MTLComputePipelineState>)pm functionHandleWithFunction:neg], ma = [(id<MTLComputePipelineState>)pm functionHandleWithFunction:add];
        id<MTLFunctionHandle> byN = nil, byA = nil;
        if (@available(macOS 26.0, *)) { byN = [(id<MTLComputePipelineState>)pm functionHandleWithName:@"op_neg"]; byA = [(id<MTLComputePipelineState>)pm functionHandleWithName:@"op_add"]; }
        check(km2 && hid(mn) == 1 && hid(ma) == 2 && hid(byN) == 1 && !byA,
              "link: functions [neg] + binaryFunctions [add]: handles %llu/%llu (want 1/2); by name op_neg %llu (want 1), op_add %p (want nil)%s%s",
              hid(mn), hid(ma), hid(byN), (__bridge void *)byA, km2 ? "" : " - ", km2 ? "" : e.localizedDescription.UTF8String);

        // 4. fragment function, binaryFunctions
        MTLLinkedFunctions *lff = [MTLLinkedFunctions linkedFunctions]; lff.binaryFunctions = @[fns[@"col_red"], fns[@"col_green"]];
        id f4 = link(fs, lff, nil, &e);
        NSData *spv4 = get_ivar(f4, "_spirv");
        check(f4 && spv4.length && has_switch_of(spv4, 2), "link: fragment through a table of binaryFunctions [red, green]: %lu B SPIR-V, switches with %s case(s)%s%s",
              (unsigned long)spv4.length, [[switch_cases(spv4) componentsJoinedByString:@","] UTF8String] ?: "-", f4 ? "" : " - ", f4 ? "" : e.localizedDescription.UTF8String);
        if (getenv("LINK_HOST_DUMP") && spv4) [spv4 writeToFile:[@(getenv("LINK_HOST_DUMP")) stringByAppendingPathComponent:@"fs_tab-red-green.spv"] atomically:YES];

        // 5. a binary function with no AIR on the table path
        id hollow = [fcls new]; set_ivar(hollow, "_fname", @"hollow_fn"); set_ivar(hollow, "_stage", @"visible");
        MTLLinkedFunctions *lfh = [MTLLinkedFunctions linkedFunctions]; lfh.binaryFunctions = @[add, hollow];
        e = nil; id k5 = link(apply, lfh, nil, &e);
        check(!k5 && [e.localizedDescription rangeOfString:@"hollow_fn"].location != NSNotFound, "link: a binary function without AIR is an error naming it: %s",
              e.localizedDescription.UTF8String ?: "(no error)");

        if (@available(macOS 26.0, *)) metal4_checks(bin.fileSystemRepresentation, lib, fns);
        else printf("SKIP MTL4 checks: macOS 26 needed\n");

        if (plugin_image_foreign(bin.fileSystemRepresentation, NULL, Nil, "setup")) {
            printf("%d PASS, %d FAIL (not counted: another NVMTLDriver was loaded during the checks)\n", npass, nfail); return 2; }
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
