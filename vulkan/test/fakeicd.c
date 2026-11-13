// SPDX-License-Identifier: MIT
// fakeicd: a stand-in for ANV with the same ICD exports, for the provider's offline test (test/run.sh). It offers
// VK_KHR_surface and VK_EXT_metal_surface only and, like Mesa, refuses any other extension and any create flag, so a
// provider that passed VK_MVK_macos_surface or VK_KHR_portability_enumeration through would fail here as on ANV.
// Devices: VK_KHR_swapchain only (VK_KHR_portability_subset is refused, as by ANV); vkQueuePresentKHR counts calls.
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#define EXPORT __attribute__((visibility("default")))

struct fake_instance { int metal; };
// what the test reads back
EXPORT struct { char exts[8][64]; uint32_t n, flags; const void *layer; int negotiated; char dexts[4][64]; uint32_t dn; int presents; } fake_seen;

static VkResult VKAPI_CALL eie(const char *layer, uint32_t *count, VkExtensionProperties *p) {
    static const char *names[] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
    if (layer) return VK_ERROR_LAYER_NOT_PRESENT;
    if (!p) { *count = 2; return VK_SUCCESS; }
    uint32_t c = *count < 2 ? *count : 2;
    for (uint32_t i = 0; i < c; i++) { memset(&p[i], 0, sizeof p[i]); strcpy(p[i].extensionName, names[i]); p[i].specVersion = 1; }
    *count = c;
    return c < 2 ? VK_INCOMPLETE : VK_SUCCESS;
}
static VkResult VKAPI_CALL elp(uint32_t *count, VkLayerProperties *p) { (void)p; *count = 0; return VK_SUCCESS; }
static VkResult VKAPI_CALL eiv(uint32_t *v) { *v = VK_API_VERSION_1_4; return VK_SUCCESS; }
static VkResult VKAPI_CALL create(const VkInstanceCreateInfo *ci, const VkAllocationCallbacks *a, VkInstance *out) {
    (void)a;
    struct fake_instance *fi = calloc(1, sizeof *fi);
    fake_seen.n = ci->enabledExtensionCount; fake_seen.flags = ci->flags;
    for (uint32_t i = 0; i < ci->enabledExtensionCount && i < 8; i++) {
        const char *e = ci->ppEnabledExtensionNames[i];
        strncpy(fake_seen.exts[i], e, 63);
        if (!strcmp(e, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) fi->metal = 1;
        else if (strcmp(e, VK_KHR_SURFACE_EXTENSION_NAME)) { free(fi); return VK_ERROR_EXTENSION_NOT_PRESENT; }
    }
    if (ci->flags) { free(fi); return VK_ERROR_INITIALIZATION_FAILED; }
    *out = (VkInstance)fi;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL metal(VkInstance i, const VkMetalSurfaceCreateInfoEXT *ci, const VkAllocationCallbacks *a, VkSurfaceKHR *s) {
    (void)i; (void)a;
    fake_seen.layer = ci->pLayer;
    *s = (VkSurfaceKHR)(uintptr_t)0x5eaf;
    return VK_SUCCESS;
}
static void VKAPI_CALL destroy_surface(VkInstance i, VkSurfaceKHR s, const VkAllocationCallbacks *a) { (void)i; (void)s; (void)a; }
EXPORT void fake_marker(void) {}
EXPORT VkResult VKAPI_CALL fake_present(VkQueue q, const VkPresentInfoKHR *pi) { (void)q; (void)pi; fake_seen.presents++; return VK_SUCCESS; }
static PFN_vkVoidFunction VKAPI_CALL gdpa(VkDevice d, const char *n) {
    (void)d;
    if (!strcmp(n, "vkQueuePresentKHR")) return (PFN_vkVoidFunction)fake_present;
    return !strcmp(n, "vkMarker") ? (PFN_vkVoidFunction)fake_marker : NULL;
}
static VkResult VKAPI_CALL ede(VkPhysicalDevice pd, const char *layer, uint32_t *count, VkExtensionProperties *p) {
    (void)pd; (void)layer;
    if (!p) { *count = 1; return VK_SUCCESS; }
    if (*count < 1) return VK_INCOMPLETE;
    memset(p, 0, sizeof *p); strcpy(p->extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME); p->specVersion = 70;
    *count = 1;
    return VK_SUCCESS;
}
static VkResult VKAPI_CALL create_device(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *a, VkDevice *out) {
    (void)pd; (void)a;
    fake_seen.dn = ci->enabledExtensionCount;
    for (uint32_t i = 0; i < ci->enabledExtensionCount; i++) {
        if (i < 4) strncpy(fake_seen.dexts[i], ci->ppEnabledExtensionNames[i], 63);
        if (strcmp(ci->ppEnabledExtensionNames[i], VK_KHR_SWAPCHAIN_EXTENSION_NAME)) return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
    *out = (VkDevice)(uintptr_t)0xde7;
    return VK_SUCCESS;
}

EXPORT PFN_vkVoidFunction VKAPI_CALL vk_icdGetInstanceProcAddr(VkInstance inst, const char *n) {
#define R(s, f) if (!strcmp(n, s)) return (PFN_vkVoidFunction)f
    R("vkEnumerateInstanceExtensionProperties", eie);
    R("vkEnumerateInstanceLayerProperties", elp);
    R("vkEnumerateInstanceVersion", eiv);
    R("vkCreateInstance", create);
    R("vkGetInstanceProcAddr", vk_icdGetInstanceProcAddr);
    if (!inst) return NULL;
    R("vkGetDeviceProcAddr", gdpa);
    R("vkDestroySurfaceKHR", destroy_surface);
    R("vkCreateDevice", create_device);
    R("vkEnumerateDeviceExtensionProperties", ede);
    R("vkQueuePresentKHR", fake_present);   // Mesa returns a trampoline for device functions here
    if (((struct fake_instance *)inst)->metal) R("vkCreateMetalSurfaceEXT", metal);
    return NULL;
#undef R
}
EXPORT VkResult VKAPI_CALL vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *v) { fake_seen.negotiated = *v; if (*v > 7) *v = 7; return VK_SUCCESS; }
