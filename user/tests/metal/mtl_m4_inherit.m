// SPDX-License-Identifier: MIT
// mtl_m4_inherit: the plugin's device class must leave Metal's own Metal 4 entry points in place unless its Metal 4 layer
// is switched on. NVMTLDevice is a subclass of Metal's MTLIOAccelDevice, whose superclass _MTLDevice answers
// newMTL4CommandQueue, newCompilerWithDescriptor:error:, newCounterHeapWithDescriptor:error:, queryTimestampFrequency and
// the rest itself. The nullmoth 2.9 merge defined them on NVMTLDevice (returning nil while the layer was off), and
// WindowServer on macOS 26 drew the desktop white with icons as empty boxes. Needs no GPU: the bundle is loaded and the
// class's methods are compared with Metal's (x86_64; runs under Rosetta on an Apple silicon Mac).
//
//   ./mtl_m4_inherit <path to NVMTLDriver.bundle>      exit 0: every entry point is Metal's own
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
    @autoreleasepool {
        if (argc < 2) { fprintf(stderr, "usage: %s <NVMTLDriver.bundle>\n", argv[0]); return 2; }
        unsetenv("NVMTL_METAL4");
        MTLCopyAllDevices();   // loads Metal's classes
        NSBundle *b = [NSBundle bundleWithPath:@(argv[1])];
        NSError *e = nil;
        if (![b loadAndReturnError:&e]) { printf("FAIL: cannot load %s: %s\n", argv[1], e.localizedDescription.UTF8String); return 2; }
        Class dev = NSClassFromString(@"NVMTLDevice"), base = NSClassFromString(@"_MTLDevice");
        if (!dev || !base) { printf("FAIL: no NVMTLDevice (%p) or _MTLDevice (%p)\n", (__bridge void *)dev, (__bridge void *)base); return 2; }
        const char *sels[] = { "newMTL4CommandQueue", "newMTL4CommandQueueWithDescriptor:error:", "newCommandAllocator",
                               "newCommandAllocatorWithDescriptor:error:", "newCommandBuffer", "newArgumentTableWithDescriptor:error:",
                               "newCompilerWithDescriptor:error:", "newArchiveWithURL:error:", "newCounterHeapWithDescriptor:error:",
                               "sizeOfCounterHeapEntry:", "queryTimestampFrequency", "newPipelineDataSetSerializerWithDescriptor:" };
        int bad = 0, checked = 0;
        for (size_t i = 0; i < sizeof sels / sizeof sels[0]; i++) {
            SEL s = sel_registerName(sels[i]);
            Method mb = class_getInstanceMethod(base, s);
            if (!mb) { printf("  skip %-42s (this macOS has no such Metal method)\n", sels[i]); continue; }
            checked++;
            const BOOL same = class_getMethodImplementation(dev, s) == method_getImplementation(mb);
            printf("  %s %-42s %s\n", same ? "PASS" : "FAIL", sels[i], same ? "Metal's own" : "replaced by the plugin");
            bad += !same;
        }
        printf("%s (%d of %d entry point(s) replaced)\n", bad ? "FAIL" : "PASS", bad, checked);
        return bad ? 4 : 0;
    }
}
