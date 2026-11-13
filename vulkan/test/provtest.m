// SPDX-License-Identifier: MIT
// provtest: the provider's offline test against fakeicd (test/run.sh sets it up). Opens the provider the way Wine
// does (dlopen of a bare name through DYLD_LIBRARY_PATH, dlsym of the names Wine 10 and 11 resolve) and checks the
// loader-side work: extension list, VK_MVK_macos_surface -> VK_EXT_metal_surface, VK_KHR_portability_enumeration
// dropped, surfaces from a CAMetalLayer and from an NSView, vkGetDeviceProcAddr forwarding, VK_KHR_portability_subset
// dropped at vkCreateDevice, and vkQueuePresentKHR: the driver's own without INTELXE_VK_FRAMELOG, the logging wrapper
// with it (test/run.sh checks the log file afterwards).
//   provtest NAME FAKEICD      e.g. provtest libMoltenVK.dylib /path/libfakeicd.dylib
#include <dlfcn.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_MACOS_MVK
#include <vulkan/vulkan.h>

typedef struct { char exts[8][64]; uint32_t n, flags; const void *layer; int negotiated; char dexts[4][64]; uint32_t dn; int presents; } seen_t;
static int fails;
#define T(c, ...) do { if (c) printf("ok    " __VA_ARGS__); else { printf("FAIL  " __VA_ARGS__); fails++; } printf("\n"); } while (0)

int main(int argc, char **argv) {
    @autoreleasepool {
    if (argc < 3) { printf("usage: provtest NAME FAKEICD\n"); return 2; }
    void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("FAIL  dlopen %s: %s\n", argv[1], dlerror()); return 1; }
    void *fk = dlopen(argv[2], RTLD_NOW | RTLD_NOLOAD);
    PFN_vkGetInstanceProcAddr gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)dlsym(h, "vkGetDeviceProcAddr");
    PFN_vkCreateMetalSurfaceEXT dl_metal = (PFN_vkCreateMetalSurfaceEXT)dlsym(h, "vkCreateMetalSurfaceEXT");
    PFN_vkCreateMacOSSurfaceMVK dl_mvk = (PFN_vkCreateMacOSSurfaceMVK)dlsym(h, "vkCreateMacOSSurfaceMVK");
    T(gipa && gdpa, "dlsym vkGetInstanceProcAddr, vkGetDeviceProcAddr (win32u vulkan_init_once)");
    T(dl_metal && dl_mvk, "dlsym vkCreateMetalSurfaceEXT, vkCreateMacOSSurfaceMVK (winemac.drv VulkanInit, Wine 10 needs both)");
    T(dlsym(h, "vkGetMTLDeviceMVK") == NULL, "no MoltenVK private API exported (Wine does not use it)");
    if (!gipa) return 1;
    // after the first call the provider has loaded the fake ICD
    PFN_vkEnumerateInstanceExtensionProperties eie = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    T(eie && eie(NULL, &n, NULL) == VK_SUCCESS && n == 4, "instance extensions: %u (ANV's 2 + MVK alias + portability enumeration)", n);
    VkExtensionProperties p[8] = {0};
    int metal = 0, mvk = 0, port = 0;
    eie(NULL, &n, p);
    for (uint32_t i = 0; i < n; i++) {
        metal |= !strcmp(p[i].extensionName, "VK_EXT_metal_surface");
        mvk |= !strcmp(p[i].extensionName, "VK_MVK_macos_surface") && p[i].specVersion == 3;
        port |= !strcmp(p[i].extensionName, "VK_KHR_portability_enumeration");
    }
    T(metal && mvk && port, "VK_EXT_metal_surface, VK_MVK_macos_surface (spec 3), VK_KHR_portability_enumeration listed");
    uint32_t two = 2;
    T(eie(NULL, &two, p) == VK_INCOMPLETE && two == 2, "a short array gets VK_INCOMPLETE");
    if (!fk) fk = dlopen(argv[2], RTLD_NOW | RTLD_NOLOAD);
    seen_t *seen = fk ? (seen_t *)dlsym(fk, "fake_seen") : NULL;
    T(seen != NULL, "the provider loaded the fake ICD (INTELXE_VK_ICD)");
    if (!seen) return 1;
    T(seen->negotiated == 7, "ICD interface negotiated at version %d", seen->negotiated);
    T((void *)gipa(NULL, "vkGetInstanceProcAddr") == (void *)gipa, "vkGetInstanceProcAddr(NULL, \"vkGetInstanceProcAddr\") is the provider's");

    const char *ex[] = {"VK_KHR_surface", "VK_MVK_macos_surface", "VK_KHR_portability_enumeration", "VK_EXT_metal_surface"};
    VkInstanceCreateInfo ci = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR, NULL, 0, NULL, 4, ex};
    VkInstance inst = VK_NULL_HANDLE;
    PFN_vkCreateInstance create = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkResult r = create(&ci, NULL, &inst);
    T(r == VK_SUCCESS, "vkCreateInstance with VK_MVK_macos_surface + portability enumeration (+ flag) = %d", r);
    T(seen->n == 2 && !strcmp(seen->exts[0], "VK_KHR_surface") && !strcmp(seen->exts[1], "VK_EXT_metal_surface") && seen->flags == 0,
      "the ICD got VK_KHR_surface, VK_EXT_metal_surface once, flags 0 (got %u: %s %s, flags %u)", seen->n, seen->exts[0], seen->exts[1], seen->flags);
    if (r) return 1;

    CAMetalLayer *layer = [CAMetalLayer layer];
    VkSurfaceKHR s = 0;
    VkMetalSurfaceCreateInfoEXT mci = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, layer};
    T(dl_metal(inst, &mci, NULL, &s) == VK_SUCCESS && s && seen->layer == (__bridge const void *)layer, "dlsym'd vkCreateMetalSurfaceEXT reaches the ICD with the layer");
    PFN_vkCreateMacOSSurfaceMVK mvkf = (PFN_vkCreateMacOSSurfaceMVK)gipa(inst, "vkCreateMacOSSurfaceMVK");
    seen->layer = NULL; s = 0;
    VkMacOSSurfaceCreateInfoMVK vci = {VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK, NULL, 0, (__bridge const void *)layer};
    T(mvkf && mvkf(inst, &vci, NULL, &s) == VK_SUCCESS && seen->layer == (__bridge const void *)layer, "vkCreateMacOSSurfaceMVK with a CAMetalLayer (what Wine passes)");
    [NSApplication sharedApplication];
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 64, 64)];
    seen->layer = NULL;
    vci.pView = (__bridge const void *)v;
    T(dl_mvk(inst, &vci, NULL, &s) == VK_SUCCESS && seen->layer && [(__bridge id)seen->layer isKindOfClass:[CAMetalLayer class]] &&
      (__bridge id)seen->layer == v.layer, "vkCreateMacOSSurfaceMVK with a plain NSView gives it a CAMetalLayer");
    vci.pView = (__bridge const void *)@"not a view";
    T(dl_mvk(inst, &vci, NULL, &s) == VK_ERROR_INITIALIZATION_FAILED, "vkCreateMacOSSurfaceMVK refuses an object that is neither");
    PFN_vkGetDeviceProcAddr gd2 = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    void *marker = dlsym(fk, "fake_marker");
    T(gd2 && (void *)gdpa((VkDevice)(uintptr_t)1, "vkMarker") == marker && (void *)gd2((VkDevice)(uintptr_t)1, "vkMarker") == marker,
      "vkGetDeviceProcAddr (exported and through vkGetInstanceProcAddr) forwards to the ICD's");
    // devices
    PFN_vkCreateDevice cdev = (PFN_vkCreateDevice)gipa(inst, "vkCreateDevice");
    const char *dex[] = {"VK_KHR_swapchain", "VK_KHR_portability_subset"};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 0, NULL, 0, NULL, 2, dex, NULL};
    VkDevice dev = VK_NULL_HANDLE;
    r = cdev ? cdev((VkPhysicalDevice)(uintptr_t)1, &dci, NULL, &dev) : VK_ERROR_INITIALIZATION_FAILED;
    T(r == VK_SUCCESS && seen->dn == 1 && !strcmp(seen->dexts[0], "VK_KHR_swapchain"),
      "vkCreateDevice with VK_KHR_portability_subset: dropped for a driver without it (= %d, driver got %u)", r, seen->dn);
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = (const char *[]){"VK_EXT_not_there"};
    T(cdev && cdev((VkPhysicalDevice)(uintptr_t)1, &dci, NULL, &dev) == VK_ERROR_EXTENSION_NOT_PRESENT, "another unknown device extension is still refused");
    void *fpres = dlsym(fk, "fake_present");
    PFN_vkQueuePresentKHR pres = (PFN_vkQueuePresentKHR)gdpa((VkDevice)(uintptr_t)0xde7, "vkQueuePresentKHR");
    PFN_vkQueuePresentKHR pres2 = (PFN_vkQueuePresentKHR)gipa(inst, "vkQueuePresentKHR");
    const char *fl = getenv("INTELXE_VK_FRAMELOG");
    if (fl && *fl)
        T(pres && pres2 && (void *)pres != fpres && (void *)pres2 != fpres, "INTELXE_VK_FRAMELOG: vkQueuePresentKHR is the logging wrapper (both ways)");
    else
        T((void *)pres == fpres && (void *)pres2 == fpres, "no INTELXE_VK_FRAMELOG: vkQueuePresentKHR is the driver's own (no cost per call)");
    VkSwapchainKHR swc = (VkSwapchainKHR)(uintptr_t)0x5c;
    uint32_t idx = 0;
    VkPresentInfoKHR pinfo = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 0, NULL, 1, &swc, &idx, NULL};
    int before = seen->presents;
    for (int i = 0; i < 5 && pres; i++) { pres((VkQueue)(uintptr_t)1, &pinfo); usleep(2000); }
    T(seen->presents == before + 5, "5 presents reach the driver (%d)", seen->presents - before);

    VkInstanceCreateInfo bad = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, NULL, 0, NULL, 1, (const char *[]){"VK_KHR_xlib_surface"}};
    VkInstance i2;
    T(create(&bad, NULL, &i2) == VK_ERROR_EXTENSION_NOT_PRESENT, "an extension neither offers is still refused");
    const char *(*anvp)(void) = (const char *(*)(void))dlsym(h, "intelxe_vk_anv_path");
    T(anvp && anvp() && strstr(anvp(), "fakeicd"), "intelxe_vk_anv_path: %s", anvp ? anvp() : "-");
    printf("%s (%d failure(s))\n", fails ? "FAIL" : "PASS", fails);
    return fails != 0;
    }
}
