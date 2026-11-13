// SPDX-License-Identifier: MIT
// plugin_image.h: for the tests that load an NVMTLDriver build into their own process with no GPU (refl_host,
// mtl_m4_inherit). They must check that build, not the installed plugin. Metal loads the installed plugin (the
// /Library/GPUBundles copy or the system volume's) as soon as a device is made in the process; two images then define
// NVMTLDevice, NVMTLFunction and the rest ("Class ... is implemented in both"), and NSClassFromString and
// dlsym(RTLD_DEFAULT) answer with whichever loaded first. On 13 Dec refl_host made a device before loading the staged
// bundle and scored the installed plugin (12/19) instead of the staged build (20/20). So the tests make no device,
// take classes and symbols from the build's own image, and check with plugin_image_foreign() after loading it and
// again at the end.
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The NVMTLDriver binary of <path>: the bundle's executable if <path> is a bundle directory, else <path> itself.
__attribute__((unused)) static NSString *plugin_image_binary(const char *path)
{
    BOOL isDir = NO;
    if ([[NSFileManager defaultManager] fileExistsAtPath:@(path) isDirectory:&isDir] && isDir)
        return [NSBundle bundleWithPath:@(path)].executablePath;
    return @(path);
}

// Class <name> of the loaded binary <bin> itself (its exported OBJC_CLASS_$_ symbol), whatever else defines that name;
// NSClassFromString's answer if the binary does not export it (plugin_image_foreign then checks where it came from).
__attribute__((unused)) static Class plugin_image_class(const char *bin, const char *name)
{
    void *h = bin ? dlopen(bin, RTLD_NOW | RTLD_NOLOAD) : NULL;
    char sym[256];
    snprintf(sym, sizeof sym, "OBJC_CLASS_$_%s", name);
    void *c = h ? dlsym(h, sym) : NULL;
    return c ? (__bridge Class)c : NSClassFromString(@(name));
}

// 0 when the only NVMTLDriver image in the process is <want> (the binary under test) and <sym> and <cls> (either may be
// NULL) come from it; otherwise prints why ("<tag>: ...") and returns 1.
static int plugin_image_foreign(const char *want, const void *sym, Class cls, const char *tag)
{
    char w[PATH_MAX], p[PATH_MAX];
    if (!want || !realpath(want, w)) { printf("%s: cannot resolve the plugin binary %s\n", tag, want ? want : "(none)"); return 1; }
    int bad = 0;
    for (uint32_t i = 0; i < _dyld_image_count(); i++) {
        const char *n = _dyld_get_image_name(i), *b = n ? strrchr(n, '/') : NULL;
        if (!b || strcmp(b + 1, "NVMTLDriver") != 0 || (realpath(n, p) && !strcmp(p, w))) continue;
        printf("%s: another NVMTLDriver is loaded in this process: %s (Metal loads the installed plugin when a device is made)\n", tag, n);
        bad = 1;
    }
    Dl_info di;
    if (sym && (!dladdr(sym, &di) || !realpath(di.dli_fname, p) || strcmp(p, w))) {
        printf("%s: the plugin's entry point comes from %s, not %s\n", tag, dladdr(sym, &di) ? di.dli_fname : "(unknown)", w);
        bad = 1;
    }
    const char *im = cls ? class_getImageName(cls) : NULL;
    if (cls && (!im || !realpath(im, p) || strcmp(p, w))) {
        printf("%s: class %s comes from %s, not %s\n", tag, class_getName(cls), im ? im : "(unknown)", w);
        bad = 1;
    }
    if (bad)
        printf("%s: the results would not be the build under test's. Make no Metal device before loading it, or run with "
               "NVMTL_BUNDLE=<that bundle> DYLD_INSERT_LIBRARIES=nvmtl_interpose.dylib so a device comes from it\n", tag);
    return bad;
}
