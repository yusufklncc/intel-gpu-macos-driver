// SPDX-License-Identifier: MIT
// libintelxe-vk: a Vulkan provider that hands ANV (Mesa's Intel Vulkan driver, /Library/GPUBundles/nvmtl/
// libvulkan_intel.dylib) to programs that open a Vulkan library by name and call it without the Khronos loader:
// Wine's macOS Vulkan driver above all. Installed as libMoltenVK.dylib and libvulkan.1.dylib in a folder that
// DYLD_LIBRARY_PATH names for one process (vulkan/intelxe-vk run), so Wine's
// dlopen(SONAME_LIBVULKAN) finds it instead of MoltenVK or Wine's bundled loader.
//
// What Wine needs, from its source (docs/design/DESIGN-native-vulkan.md has the lines):
//   - dlsym: vkGetInstanceProcAddr and vkGetDeviceProcAddr (win32u/vulkan.c vulkan_init_once); everything else it
//     reaches through them, global commands with a NULL instance.
//   - dlsym: vkCreateMetalSurfaceEXT (Wine 11: its presence picks VK_EXT_metal_surface over VK_MVK_macos_surface;
//     Wine 10: both it and vkCreateMacOSSurfaceMVK must resolve or winemac.drv refuses Vulkan).
//   - no MoltenVK private entry points (vkGetMTLDeviceMVK, vkSetMTLTextureMVK, ...): Wine calls none.
// ANV exports only the ICD interface (vk_icdGetInstanceProcAddr, vk_icdNegotiateLoaderICDInterfaceVersion,
// vk_icdGetPhysicalDeviceProcAddr), so this library is the small part of a loader that a single driver needs:
// the exported names, VK_KHR_portability_enumeration (a loader extension: accepted and dropped), and
// VK_MVK_macos_surface for older callers, turned into VK_EXT_metal_surface. Everything else goes to ANV untouched;
// no device function is wrapped, so there is no cost per call.
//
// Environment: INTELXE_VK_ICD=<path> loads another ANV build; INTELXE_VK_DEBUG=1 logs to stderr.
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#import <AppKit/AppKit.h>
#import <QuartzCore/CAMetalLayer.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_MACOS_MVK
#include <vulkan/vulkan.h>

#define EXPORT __attribute__((visibility("default")))
#define ANV_DEFAULT "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
#define ANV_LEAF "/nvmtl/libvulkan_intel.dylib"

// a version string the helper finds with grep -a (no nm on the test machine)
EXPORT const char intelxe_vk_version[] = "intelxe-vk-provider 1 (VK_EXT_metal_surface, VK_MVK_macos_surface alias)";

typedef VkResult (VKAPI_PTR *PFN_negotiate)(uint32_t *);

static pthread_once_t once = PTHREAD_ONCE_INIT;
static PFN_vkGetInstanceProcAddr icd_gipa;
static PFN_vkGetDeviceProcAddr anv_gdpa;   // ANV's, cached at the first vkCreateInstance
static int dbg;
static char anv_path[1024];

static void logf_(const char *fmt, ...) {
    if (!dbg) return;
    va_list ap; va_start(ap, fmt);
    fputs("intelxe-vk: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap);
}

// One ANV per process: if one is already loaded (the Metal plugin loads its own, from /Library or the system-volume
// copy), take that image, so the process does not run two copies of the driver with separate global state.
static const char *pick_anv(void) {
    const char *e = getenv("INTELXE_VK_ICD");
    if (e && *e) return e;
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) {
        const char *p = _dyld_get_image_name(i);
        size_t l = p ? strlen(p) : 0, k = strlen(ANV_LEAF);
        if (l > k && !strcmp(p + l - k, ANV_LEAF)) return p;
    }
    return ANV_DEFAULT;
}

static void load(void) {
    const char *d = getenv("INTELXE_VK_DEBUG");
    dbg = d && *d && *d != '0';
    snprintf(anv_path, sizeof anv_path, "%s", pick_anv());
    void *h = dlopen(anv_path, RTLD_NOW | RTLD_LOCAL);
    if (!h) { fprintf(stderr, "intelxe-vk: cannot load ANV (%s): %s\n", anv_path, dlerror()); return; }
    icd_gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!icd_gipa) { fprintf(stderr, "intelxe-vk: %s has no vk_icdGetInstanceProcAddr\n", anv_path); return; }
    // Interface 7, Mesa's own default: the driver owns VkSurfaceKHR objects (v3+), which is what lets a caller
    // without a loader pass ANV's surfaces straight back to it.
    PFN_negotiate neg = (PFN_negotiate)dlsym(h, "vk_icdNegotiateLoaderICDInterfaceVersion");
    uint32_t v = 7;
    if (neg) neg(&v);
    logf_("ANV %s (ICD interface %u)", anv_path, v);
}

static int ready(void) { pthread_once(&once, load); return icd_gipa != NULL; }

static int has_ext(const char *name) {
    PFN_vkEnumerateInstanceExtensionProperties f =
        (PFN_vkEnumerateInstanceExtensionProperties)icd_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    if (!f || f(NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *p = calloc(n, sizeof *p);
    int found = 0;
    if (p && f(NULL, &n, p) >= 0)
        for (uint32_t i = 0; i < n && !found; i++) found = !strcmp(p[i].extensionName, name);
    free(p);
    return found;
}

// ANV's list plus the two extensions this library implements itself.
static VkResult VKAPI_CALL ixe_EnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                     VkExtensionProperties *props) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceExtensionProperties f =
        (PFN_vkEnumerateInstanceExtensionProperties)icd_gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    if (layer) return f(layer, count, props);
    uint32_t n = 0;
    VkResult r = f(NULL, &n, NULL);
    if (r != VK_SUCCESS) return r;
    VkExtensionProperties *all = calloc(n + 2, sizeof *all);
    if (!all) return VK_ERROR_OUT_OF_HOST_MEMORY;
    if ((r = f(NULL, &n, all)) < 0) { free(all); return r; }
    int metal = 0, mvk = 0, port = 0;
    for (uint32_t i = 0; i < n; i++) {
        metal |= !strcmp(all[i].extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        mvk |= !strcmp(all[i].extensionName, VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
        port |= !strcmp(all[i].extensionName, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    if (metal && !mvk) {
        snprintf(all[n].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
        all[n++].specVersion = VK_MVK_MACOS_SURFACE_SPEC_VERSION;
    }
    if (!port) {
        snprintf(all[n].extensionName, VK_MAX_EXTENSION_NAME_SIZE, "%s", VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
        all[n++].specVersion = VK_KHR_PORTABILITY_ENUMERATION_SPEC_VERSION;
    }
    if (!props) { *count = n; free(all); return VK_SUCCESS; }
    uint32_t c = *count < n ? *count : n;
    memcpy(props, all, c * sizeof *props);
    *count = c;
    free(all);
    return c < n ? VK_INCOMPLETE : VK_SUCCESS;
}

// The extension list ANV gets: VK_MVK_macos_surface becomes VK_EXT_metal_surface, VK_KHR_portability_enumeration
// (and its create flag) is dropped as the Khronos loader drops it for a driver that is not a portability driver.
static VkResult VKAPI_CALL ixe_CreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                              VkInstance *out) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateInstance f = (PFN_vkCreateInstance)icd_gipa(NULL, "vkCreateInstance");
    VkInstanceCreateInfo c = *ci;
    const char **names = calloc(ci->enabledExtensionCount + 1, sizeof *names);
    if (!names) return VK_ERROR_OUT_OF_HOST_MEMORY;
    uint32_t n = 0;
    int metal = 0, anv_mvk = -1, anv_port = -1;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        if (!strcmp(e, VK_MVK_MACOS_SURFACE_EXTENSION_NAME)) {
            if (anv_mvk < 0) anv_mvk = has_ext(e);
            if (!anv_mvk) { e = VK_EXT_METAL_SURFACE_EXTENSION_NAME; logf_("VK_MVK_macos_surface -> VK_EXT_metal_surface"); }
        } else if (!strcmp(e, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
            if (anv_port < 0) anv_port = has_ext(e);
            if (!anv_port) { c.flags &= ~VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR; continue; }
        }
        if (!strcmp(e, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) { if (metal++) continue; }
        names[n++] = e;
    }
    c.enabledExtensionCount = n;
    c.ppEnabledExtensionNames = names;
    VkResult r = f(&c, alloc, out);
    free(names);
    if (r == VK_SUCCESS && !anv_gdpa)   // the same function for every ANV instance and device
        anv_gdpa = (PFN_vkGetDeviceProcAddr)icd_gipa(*out, "vkGetDeviceProcAddr");
    if (dbg) {
        const VkApplicationInfo *ai = ci->pApplicationInfo;
        logf_("vkCreateInstance(%s, api %u.%u, %u extensions) = %d", ai && ai->pApplicationName ? ai->pApplicationName : "-",
              ai ? VK_API_VERSION_MAJOR(ai->apiVersion) : 1, ai ? VK_API_VERSION_MINOR(ai->apiVersion) : 0, n, r);
    }
    return r;
}

// pView may be a CAMetalLayer (what Wine passes) or an NSView; a view gets a CAMetalLayer if it has none, as MoltenVK
// does. Layer and view are touched on the main thread only.
static CAMetalLayer *layer_of(const void *obj) {
    __block CAMetalLayer *l = nil;
    id o = (__bridge id)obj;
    if ([o isKindOfClass:[CAMetalLayer class]]) return (CAMetalLayer *)o;
    if (![o isKindOfClass:[NSView class]]) return nil;
    void (^get)(void) = ^{
        NSView *v = (NSView *)o;
        if (![v.layer isKindOfClass:[CAMetalLayer class]]) {
            v.wantsLayer = YES;
            v.layer = [CAMetalLayer layer];
        }
        l = (CAMetalLayer *)v.layer;
    };
    if ([NSThread isMainThread]) get(); else dispatch_sync(dispatch_get_main_queue(), get);
    return l;
}

static VkResult VKAPI_CALL ixe_CreateMacOSSurfaceMVK(VkInstance inst, const VkMacOSSurfaceCreateInfoMVK *ci,
                                                     const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateMetalSurfaceEXT f = (PFN_vkCreateMetalSurfaceEXT)icd_gipa(inst, "vkCreateMetalSurfaceEXT");
    if (!f) return VK_ERROR_EXTENSION_NOT_PRESENT;
    CAMetalLayer *l = layer_of(ci->pView);
    if (!l) { logf_("vkCreateMacOSSurfaceMVK: pView is neither a CAMetalLayer nor an NSView"); return VK_ERROR_INITIALIZATION_FAILED; }
    VkMetalSurfaceCreateInfoEXT m = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, l};
    return f(inst, &m, alloc, out);
}

// ---- the exported names

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name);

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                             VkExtensionProperties *props) {
    return ixe_EnumerateInstanceExtensionProperties(layer, count, props);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *props) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    return ((PFN_vkEnumerateInstanceLayerProperties)icd_gipa(NULL, "vkEnumerateInstanceLayerProperties"))(count, props);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkEnumerateInstanceVersion f = (PFN_vkEnumerateInstanceVersion)icd_gipa(NULL, "vkEnumerateInstanceVersion");
    if (!f) { *version = VK_API_VERSION_1_0; return VK_SUCCESS; }
    return f(version);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *alloc,
                                                      VkInstance *out) {
    return ixe_CreateInstance(ci, alloc, out);
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateMetalSurfaceEXT(VkInstance inst, const VkMetalSurfaceCreateInfoEXT *ci,
                                                             const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    if (!ready()) return VK_ERROR_INITIALIZATION_FAILED;
    PFN_vkCreateMetalSurfaceEXT f = (PFN_vkCreateMetalSurfaceEXT)icd_gipa(inst, "vkCreateMetalSurfaceEXT");
    return f ? f(inst, ci, alloc, out) : VK_ERROR_EXTENSION_NOT_PRESENT;
}

EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkCreateMacOSSurfaceMVK(VkInstance inst, const VkMacOSSurfaceCreateInfoMVK *ci,
                                                             const VkAllocationCallbacks *alloc, VkSurfaceKHR *out) {
    return ixe_CreateMacOSSurfaceMVK(inst, ci, alloc, out);
}

EXPORT VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance inst, VkSurfaceKHR s, const VkAllocationCallbacks *alloc) {
    if (!ready()) return;
    PFN_vkDestroySurfaceKHR f = (PFN_vkDestroySurfaceKHR)icd_gipa(inst, "vkDestroySurfaceKHR");
    if (f) f(inst, s, alloc);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice dev, const char *name) {
    if (!ready() || !anv_gdpa) return NULL;
    return anv_gdpa(dev, name);
}

EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    if (!name || !ready()) return NULL;
#define OWN(n, fn) if (!strcmp(name, n)) return (PFN_vkVoidFunction)fn
    OWN("vkGetInstanceProcAddr", vkGetInstanceProcAddr);
    OWN("vkCreateInstance", ixe_CreateInstance);
    OWN("vkEnumerateInstanceExtensionProperties", ixe_EnumerateInstanceExtensionProperties);
    if (inst) {
        OWN("vkGetDeviceProcAddr", vkGetDeviceProcAddr);
        if (!strcmp(name, "vkCreateMacOSSurfaceMVK"))
            return icd_gipa(inst, "vkCreateMacOSSurfaceMVK") ?: (icd_gipa(inst, "vkCreateMetalSurfaceEXT")
                   ? (PFN_vkVoidFunction)ixe_CreateMacOSSurfaceMVK : NULL);
    }
#undef OWN
    return icd_gipa(inst, name);
}

// ---- for vksurf and the helper's check: which ANV this process uses

EXPORT const char *intelxe_vk_anv_path(void) { return ready() ? anv_path : NULL; }
