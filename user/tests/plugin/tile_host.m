// SPDX-License-Identifier: MIT
// tile_host: the plugin's tile pipelines (nullmoth/plugin/NVMTLTile.m) without a GPU. The plugin binary is loaded into
// this process (x86_64; under Rosetta on an Apple silicon Mac) and fed the AIR of user/tests/plugin/tile/ (tile functions
// compiled by Metal's compiler and read by the plugin's metallib reader, the same modules the translator's tile_lower tests
// use). For each, nvmtl_tile_pipeline lowers the function into a kernel with the plugin's translator and asks a stand-in
// device for the compute pipeline; the test checks what the stand-in was handed:
//   - the lowered AIR has no implicit imageblock access left and binds each attachment's plane at texture 120 + n and the
//     tile parameters at buffer 30;
//   - its SPIR-V (the real translator, with the plugin's capabilities) binds a storage image per plane at 480 + 120 + n;
//   - the pipeline's read and written attachment masks;
//   - the library hands out every tile function, also one whose MSAA forms do not translate as a plain kernel (tile_ms:
//     get_num_colors, coverage-mask writes; on the B580 the library answered nil for it and the tile pipeline had no
//     function), with and without constant values, and the tile pipeline is made from that function;
//   - refusals: an attachment the function uses with no pixel format, an integer value on a float attachment, linked
//     functions; each MTLDeviceErrorDomain code 1 naming the tile function.
//
//   tile_host <NVMTLDriver.bundle or its NVMTLDriver binary> <user/tests/plugin/tile>
//   build: clang -arch x86_64 -fobjc-arc -I user/tests/metal user/tests/plugin/tile_host.m -framework Foundation -framework Metal
// It makes no Metal device (plugin_image.h). Exit: 0 pass, 2 setup failure, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
#include <stdarg.h>
#include "plugin_image.h"

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
    Ivar iv = o ? class_getInstanceVariable(object_getClass(o), name) : NULL;
    return iv ? object_getIvar(o, iv) : nil;
}
static uint32_t get_u32(id o, const char *name)
{
    Ivar iv = o ? class_getInstanceVariable(object_getClass(o), name) : NULL;
    return iv ? *(uint32_t *)((uint8_t *)(__bridge void *)o + ivar_getOffset(iv)) : 0xffffffffu;
}

// A stand-in for the device: it keeps the function it is asked to make a compute pipeline of and answers an object.
@interface THDevice : NSObject { @public id last; } @end
@implementation THDevice
- (id)newComputePipelineStateWithFunction:(id)f error:(NSError **)e { last = f; return [NSObject new]; }
@end

// descriptor bindings of a SPIR-V module in the storage-texture band at the planes' texture indices (480 + 120 + n);
// -1 if it is not SPIR-V
static int storage_arrays(NSData *spv)
{
    const uint32_t *w = spv.bytes; size_t n = spv.length / 4; int k = 0;
    if (n < 5 || w[0] != 0x07230203u) return -1;
    for (size_t i = 5; i < n;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 3] >= 600 && w[i + 3] < 608) k++;   // OpDecorate Binding
        i += wc;
    }
    return k;
}

typedef id (*tile_fn)(id, MTLTileRenderPipelineDescriptor *, MTLPipelineOption, id *, NSError **);

static id make(tile_fn tp, THDevice *dev, id f, const MTLPixelFormat *fmts, NSUInteger n, NSUInteger samples, NSError **e)
{
    MTLTileRenderPipelineDescriptor *d = [MTLTileRenderPipelineDescriptor new];
    d.tileFunction = f; d.rasterSampleCount = samples; d.threadgroupSizeMatchesTileSize = YES;
    for (NSUInteger i = 0; i < n; i++) d.colorAttachments[i].pixelFormat = fmts[i];
    dev->last = nil;
    return tp((id)dev, d, MTLPipelineOptionNone, NULL, e);
}
static BOOL refusal(id obj, NSError *e, NSString *word)
{
    return !obj && [e.domain isEqualToString:@"MTLDeviceErrorDomain"] && e.code == 1 && [e.localizedDescription rangeOfString:word].location != NSNotFound;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
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
        tile_fn tp = h ? (tile_fn)dlsym(h, "nvmtl_tile_pipeline") : NULL;
        Class fcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLFunction");
        Class tcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLTileRenderPipelineState");
        if (!tp || !fcls || !tcls) { printf("setup: the plugin does not export nvmtl_tile_pipeline (%p) or its classes\n", (void *)tp); return 2; }
        if (plugin_image_foreign(bin.fileSystemRepresentation, (const void *)tp, fcls, "setup")) return 2;

        NSString *dir = @(argv[2]);
        id (^fn)(NSString *) = ^id(NSString *name) {
            NSString *ll = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".ll"]] encoding:NSUTF8StringEncoding error:NULL];
            if (!ll.length) { printf("setup: %s.ll missing in %s\n", name.UTF8String, dir.UTF8String); exit(2); }
            id f = [fcls new];
            set_ivar(f, "_air", ll); set_ivar(f, "_fname", name); set_ivar(f, "_stage", @"kernel");
            return f;
        };
        THDevice *dev = [THDevice new];
        const MTLPixelFormat F8 = MTLPixelFormatRGBA8Unorm, F16 = MTLPixelFormatRGBA16Float, F32 = MTLPixelFormatRGBA32Float, U32 = MTLPixelFormatR32Uint;
        struct { const char *name; MTLPixelFormat f[3]; NSUInteger n, samples; uint32_t reads, writes; int planes; } cases[] = {
            { "tile_light", { F8, F16, MTLPixelFormatBGRA8Unorm_sRGB }, 3, 1, 0x3, 0x7, 3 },
            { "tile_ms", { F8 }, 1, 4, 0x1, 0x1, 1 },
            { "tile_f32", { F32, U32 }, 2, 1, 0x3, 0x3, 2 },
            { "tile_tg", { F8 }, 1, 1, 0x1, 0x1, 1 },
            { "tile_explicit", { F8 }, 1, 1, 0, 0, 0 },
            { "t_misc", { F16, MTLPixelFormatRG32Float, MTLPixelFormatR16Sint }, 3, 2, 0x7, 0x7, 3 },
        };
        for (size_t c = 0; c < sizeof cases / sizeof cases[0]; c++) {
            NSError *e = nil;
            id ps = make(tp, dev, fn(@(cases[c].name)), cases[c].f, cases[c].n, cases[c].samples, &e);
            id kf = dev->last;
            NSString *kll = get_ivar(kf, "_air"); NSData *spv = get_ivar(kf, "_spirv");
            const uint32_t rd = get_u32(ps, "_reads"), wr = get_u32(ps, "_writes");
            int planeMeta = 0;
            for (int a = 0; a < 8; a++) if ([kll containsString:[NSString stringWithFormat:@"!\"air.location_index\", i32 %d, i32 1, !\"air.read_write\"", 120 + a]]) planeMeta++;
            const int ok = ps && [ps isKindOfClass:tcls] && kll && ![kll containsString:@"implicit_imageblock"] &&
                           [kll containsString:@"!\"air.location_index\", i32 30, i32 1, !\"air.read\""] && planeMeta == cases[c].planes &&
                           storage_arrays(spv) == cases[c].planes && rd == cases[c].reads && wr == cases[c].writes;
            check(ok, "%s: %lu attachment(s), %lu sample(s) -> %s, %lu B SPIR-V with %d plane image(s) (want %d), reads 0x%x writes 0x%x (want 0x%x 0x%x)%s%s",
                  cases[c].name, (unsigned long)cases[c].n, (unsigned long)cases[c].samples, ps ? object_getClassName(ps) : "nil",
                  (unsigned long)spv.length, storage_arrays(spv), cases[c].planes, rd, wr, cases[c].reads, cases[c].writes,
                  ps ? "" : " - ", ps ? "" : e.localizedDescription.UTF8String);
        }
        // the library path: functions as a library made from a metallib hands them out (AIR bitcode slot and AIR text)
        Class lcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLLibrary");
        if (!lcls) { printf("setup: no NVMTLLibrary class\n"); return 2; }
        {
            NSArray *names = @[ @"tile_light", @"tile_ms", @"tile_f32", @"tile_tg", @"tile_explicit", @"t_misc" ];
            NSMutableDictionary *airs = [NSMutableDictionary new], *fdata = [NSMutableDictionary new], *stages = [NSMutableDictionary new];
            for (NSString *n in names) {
                NSString *ll = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:[n stringByAppendingString:@".ll"]] encoding:NSUTF8StringEncoding error:NULL];
                if (!ll.length) { printf("setup: %s.ll missing\n", n.UTF8String); return 2; }
                airs[n] = ll; fdata[n] = [ll dataUsingEncoding:NSUTF8StringEncoding]; stages[n] = @"kernel";
            }
            id lib = [lcls new]; set_ivar(lib, "_airs", airs); set_ivar(lib, "_stages", stages); set_ivar(lib, "_fns", fdata);
            int got = 0; NSMutableString *missing = [NSMutableString new];
            for (NSString *n in names) {
                id f = [(id<MTLLibrary>)lib newFunctionWithName:n];
                if (f && [get_ivar(f, "_air") length]) got++; else [missing appendFormat:@" %@", n];
            }
            check(got == (int)names.count, "library: newFunctionWithName: hands out %d of %lu tile functions (missing:%s)", got,
                  (unsigned long)names.count, missing.length ? missing.UTF8String : " none");
            id ms = [(id<MTLLibrary>)lib newFunctionWithName:@"tile_ms"];
            const MTLPixelFormat f1[1] = { F8 };
            NSError *e1 = nil; id p1 = ms ? make(tp, dev, ms, f1, 1, 4, &e1) : nil;
            check(p1 && [p1 isKindOfClass:tcls] && get_u32(p1, "_samples") == 4,
                  "library: tile_ms (MSAA forms, no plain-kernel translation) -> function %p, 4-sample tile pipeline %s%s%s",
                  (__bridge void *)ms, p1 ? object_getClassName(p1) : "nil", p1 ? "" : " - ", p1 ? "" : e1.localizedDescription.UTF8String);
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
            NSError *e2 = nil; id mc = [(id<MTLLibrary>)lib newFunctionWithName:@"tile_ms" constantValues:cv error:&e2];
            check(mc && [get_ivar(mc, "_air") length], "library: tile_ms through newFunctionWithName:constantValues:error: -> %p %s",
                  (__bridge void *)mc, e2 ? e2.localizedDescription.UTF8String : "");
        }
        // refusals
        NSError *e = nil;
        const MTLPixelFormat one[1] = { F32 };
        id r1 = make(tp, dev, fn(@"tile_f32"), one, 1, 1, &e);
        check(refusal(r1, e, @"colour attachment 1") && !dev->last, "tile_f32 without a format for colour 1: refused: %s", e.localizedDescription.UTF8String ?: "(no error)");
        e = nil; const MTLPixelFormat ints[2] = { MTLPixelFormatRGBA32Uint, U32 };
        id r2 = make(tp, dev, fn(@"tile_f32"), ints, 2, 1, &e);
        check(refusal(r2, e, @"tile_f32"), "tile_f32 with a float value on an integer attachment: refused: %s", e.localizedDescription.UTF8String ?: "(no error)");
        e = nil;
        MTLTileRenderPipelineDescriptor *d = [MTLTileRenderPipelineDescriptor new];
        d.tileFunction = fn(@"tile_tg"); d.colorAttachments[0].pixelFormat = F8;
        MTLLinkedFunctions *lf = [MTLLinkedFunctions linkedFunctions]; lf.functions = @[fn(@"tile_ms")]; d.linkedFunctions = lf;
        id r3 = tp((id)dev, d, MTLPipelineOptionNone, NULL, &e);
        check(refusal(r3, e, @"linking"), "tile_tg with linked functions: refused: %s", e.localizedDescription.UTF8String ?: "(no error)");
        if (plugin_image_foreign(bin.fileSystemRepresentation, (const void *)tp, fcls, "end")) return 2;
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
