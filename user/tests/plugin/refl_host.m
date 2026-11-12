// SPDX-License-Identifier: MIT
// refl_host: the plugin's pipeline reflection (nvmtl_compute_reflection, nvmtl_render_reflection: the argument lists
// built from the translator's reflection and the AIR's argument metadata) checked against Apple's answers for the same
// shaders, without a GPU. The plugin binary is loaded into this process (x86_64; under Rosetta on an Apple silicon
// Mac); its functions are fed the AIR text and the translator reflection of user/tests/plugin/refl/ (the shaders of
// user/tests/metal/mtl_reflection.m), and the reflection objects they return are checked with
// user/tests/metal/mtl_reflection_expect.h, the expectations mtl_reflection checks on a GPU.
//
//   refl_host <NVMTLDriver.bundle or its NVMTLDriver binary> <user/tests/plugin/refl>
//   build: clang -arch x86_64 -fobjc-arc -I user/tests/metal user/tests/plugin/refl_host.m -framework Foundation -framework Metal
// Exit: 0 pass, 2 setup failure, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
#include "mtl_reflection_expect.h"

// An NVMTLFunction of the loaded plugin, with the AIR text and the translator reflection a library load would give it.
static id plugin_function(Class cls, NSString *dir, NSString *name, NSString *stage)
{
    NSString *ll = [NSString stringWithContentsOfFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".ll"]] encoding:NSUTF8StringEncoding error:NULL];
    NSData *js = [NSData dataWithContentsOfFile:[dir stringByAppendingPathComponent:[name stringByAppendingString:@".json"]]];
    NSDictionary *refl = js ? [NSJSONSerialization JSONObjectWithData:js options:0 error:NULL] : nil;
    if (!ll.length || ![refl isKindOfClass:[NSDictionary class]]) { printf("setup: %s.ll / %s.json missing or unreadable in %s\n", name.UTF8String, name.UTF8String, dir.UTF8String); return nil; }
    id f = [cls new];
    const struct { const char *ivar; id value; } set[] = { { "_air", ll }, { "_fname", name }, { "_stage", stage }, { "_refl", refl } };
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) {
        Ivar iv = class_getInstanceVariable(cls, set[i].ivar);
        if (!iv) { printf("setup: NVMTLFunction has no %s\n", set[i].ivar); return nil; }
        object_setIvarWithStrongDefault(f, iv, set[i].value);
    }
    return f;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (argc < 3) { fprintf(stderr, "usage: %s <NVMTLDriver.bundle | NVMTLDriver> <fixture dir>\n", argv[0]); return 2; }
        MTLCopyAllDevices();   // Metal's classes, which the plugin's subclass
        BOOL isDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:@(argv[1]) isDirectory:&isDir];
        if (isDir) {
            NSError *e = nil;
            if (![[NSBundle bundleWithPath:@(argv[1])] loadAndReturnError:&e]) { printf("setup: cannot load %s: %s\n", argv[1], e.localizedDescription.UTF8String); return 2; }
        } else if (!dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL)) { printf("setup: dlopen %s: %s\n", argv[1], dlerror()); return 2; }
        id (*compute_reflection)(id) = (id (*)(id))dlsym(RTLD_DEFAULT, "nvmtl_compute_reflection");
        id (*render_reflection)(id) = (id (*)(id))dlsym(RTLD_DEFAULT, "nvmtl_render_reflection");
        Class fcls = NSClassFromString(@"NVMTLFunction");
        if (!compute_reflection || !render_reflection || !fcls) {
            printf("setup: the plugin does not export nvmtl_compute_reflection (%p) / nvmtl_render_reflection (%p) / NVMTLFunction (%p)\n",
                   (void *)compute_reflection, (void *)render_reflection, (__bridge void *)fcls);
            return 2; }
        NSString *dir = @(argv[2]);
        id kf = plugin_function(fcls, dir, @"kref", @"kernel"), vf = plugin_function(fcls, dir, @"vs", @"vertex"), ff = plugin_function(fcls, dir, @"fs", @"fragment");
        if (!kf || !vf || !ff) return 2;

        MTLComputePipelineReflection *cr = compute_reflection(kf);
        check(cr != nil, "plugin compute reflection (%s)", cr ? NSStringFromClass([cr class]).UTF8String : "nil");
        if (cr) check_bindings("plugin compute", cr.bindings, kKernel, sizeof kKernel / sizeof kKernel[0]);

        MTLRenderPipelineDescriptor *rd = [MTLRenderPipelineDescriptor new];
        rd.vertexFunction = vf; rd.fragmentFunction = ff; rd.colorAttachments[0].pixelFormat = MTLPixelFormatBGRA8Unorm;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor new];
        vd.attributes[0].format = MTLVertexFormatFloat2; vd.attributes[0].bufferIndex = 0;
        vd.attributes[1].format = MTLVertexFormatFloat2; vd.attributes[1].offset = 8; vd.attributes[1].bufferIndex = 0;
        vd.layouts[0].stride = 16; rd.vertexDescriptor = vd;
        MTLRenderPipelineReflection *rr = render_reflection(rd);
        check(rr != nil, "plugin render reflection");
        if (rr) {
            check_bindings("plugin vertex", rr.vertexBindings, kVertex, sizeof kVertex / sizeof kVertex[0]);
            check_bindings("plugin fragment", rr.fragmentBindings, kFragment, sizeof kFragment / sizeof kFragment[0]);
        }
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
