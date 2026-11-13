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
// It makes no Metal device: on a machine with the plugin installed, a device would load the installed plugin into the
// process too, and its classes and symbols could answer instead of the build's (plugin_image.h). A second NVMTLDriver
// image is a setup failure, not a score. Exit: 0 pass, 2 setup failure, 4 wrong results.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <dlfcn.h>
#include <string.h>
#include "mtl_reflection_expect.h"
#include "plugin_image.h"

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
        // no MTLCopyAllDevices() or other device here (see above); Metal.framework, linked, has the classes the plugin's subclass
        NSString *bin = plugin_image_binary(argv[1]);
        BOOL isDir = NO;
        [[NSFileManager defaultManager] fileExistsAtPath:@(argv[1]) isDirectory:&isDir];
        NSBundle *bundle = isDir ? [NSBundle bundleWithPath:@(argv[1])] : nil;
        if (bundle) {
            NSError *e = nil;
            if (![bundle loadAndReturnError:&e]) { printf("setup: cannot load %s: %s\n", argv[1], e.localizedDescription.UTF8String); return 2; }
        } else if (!dlopen(argv[1], RTLD_NOW | RTLD_GLOBAL)) { printf("setup: dlopen %s: %s\n", argv[1], dlerror()); return 2; }
        void *h = bin ? dlopen(bin.fileSystemRepresentation, RTLD_NOW | RTLD_NOLOAD) : NULL;   // the build's own image
        if (!h) { printf("setup: %s is not loaded as %s\n", argv[1], bin ? bin.UTF8String : "(no executable)"); return 2; }
        id (*compute_reflection)(id) = (id (*)(id))dlsym(h, "nvmtl_compute_reflection");
        id (*render_reflection)(id) = (id (*)(id))dlsym(h, "nvmtl_render_reflection");
        Class fcls = plugin_image_class(bin.fileSystemRepresentation, "NVMTLFunction");
        if (!compute_reflection || !render_reflection || !fcls) {
            printf("setup: the plugin does not export nvmtl_compute_reflection (%p) / nvmtl_render_reflection (%p) / NVMTLFunction (%p)\n",
                   (void *)compute_reflection, (void *)render_reflection, (__bridge void *)fcls);
            return 2; }
        if (plugin_image_foreign(bin.fileSystemRepresentation, (const void *)compute_reflection, fcls, "setup")) return 2;
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
        if (plugin_image_foreign(bin.fileSystemRepresentation, NULL, Nil, "setup")) {   // something made a device meanwhile
            printf("%d PASS, %d FAIL (not counted: another NVMTLDriver was loaded during the checks)\n", npass, nfail); return 2; }
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return nfail ? 4 : 0;
    }
}
