// vkcaps: what the installed ANV (libvulkan_intel.dylib, loaded directly as an ICD) offers against what DXVK and
// vkd3d-proton ask for. Measurement only. clang -arch x86_64 -O1 -I<mesa>/include -o vkcaps vkcaps.c
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
#define IP(i, n) ((PFN_##n)gipa(i, #n))

static const char *want_dev[] = {
    // DXVK 2.x/3.x required
    "VK_KHR_swapchain", "VK_EXT_robustness2", "VK_EXT_transform_feedback", "VK_EXT_extended_dynamic_state3",
    "VK_KHR_pipeline_library", "VK_EXT_graphics_pipeline_library", "VK_EXT_custom_border_color", "VK_EXT_depth_clip_enable",
    "VK_EXT_vertex_attribute_divisor", "VK_KHR_maintenance5", "VK_EXT_shader_demote_to_helper_invocation",
    "VK_EXT_descriptor_buffer", "VK_EXT_memory_priority", "VK_EXT_memory_budget", "VK_KHR_present_id", "VK_KHR_present_wait",
    "VK_EXT_swapchain_maintenance1", "VK_EXT_line_rasterization", "VK_EXT_conservative_rasterization",
    "VK_EXT_non_seamless_cube_map", "VK_EXT_attachment_feedback_loop_layout", "VK_EXT_shader_module_identifier",
    "VK_KHR_external_memory_fd", "VK_KHR_external_semaphore_fd", "VK_EXT_external_memory_host",
    // vkd3d-proton
    "VK_KHR_buffer_device_address", "VK_EXT_mutable_descriptor_type", "VK_KHR_timeline_semaphore", "VK_KHR_dynamic_rendering",
    "VK_KHR_synchronization2", "VK_KHR_shader_float16_int8", "VK_EXT_shader_viewport_index_layer", "VK_KHR_fragment_shading_rate",
    "VK_EXT_mesh_shader", "VK_KHR_ray_tracing_pipeline", "VK_KHR_ray_query", "VK_KHR_acceleration_structure",
    "VK_EXT_image_view_min_lod", "VK_KHR_shader_integer_dot_product", "VK_KHR_maintenance4", "VK_EXT_device_generated_commands",
    "VK_KHR_cooperative_matrix", "VK_EXT_shader_object", "VK_KHR_present_id2", "VK_KHR_present_wait2", NULL };

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen %s: %s\n", lib, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }
    uint32_t n = 0;
    PFN_vkEnumerateInstanceExtensionProperties eie = IP(NULL, vkEnumerateInstanceExtensionProperties);
    eie(NULL, &n, NULL); VkExtensionProperties *ie = calloc(n, sizeof *ie); eie(NULL, &n, ie);
    printf("instance extensions (%u):", n); for (uint32_t i = 0; i < n; i++) printf(" %s", ie[i].extensionName); printf("\n");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkcaps", 1, NULL, 0, VK_API_VERSION_1_4 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VkInstance inst; VkResult r = IP(NULL, vkCreateInstance)(&ici, NULL, &inst);
    if (r) { printf("vkCreateInstance %d\n", r); return 1; }
    uint32_t pc = 0; IP(inst, vkEnumeratePhysicalDevices)(inst, &pc, NULL);
    VkPhysicalDevice pd[4]; pc = pc > 4 ? 4 : pc; IP(inst, vkEnumeratePhysicalDevices)(inst, &pc, pd);
    if (!pc) { printf("no physical device\n"); return 1; }
    VkPhysicalDeviceProperties pp; IP(inst, vkGetPhysicalDeviceProperties)(pd[0], &pp);
    printf("device: %s, API %u.%u.%u, driver 0x%x\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion),
           VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion), pp.driverVersion);
    PFN_vkEnumerateDeviceExtensionProperties ede = IP(inst, vkEnumerateDeviceExtensionProperties);
    ede(pd[0], NULL, &n, NULL); VkExtensionProperties *de = calloc(n, sizeof *de); ede(pd[0], NULL, &n, de);
    printf("device extensions: %u\n", n);
    for (const char **w = want_dev; *w; w++) {
        int have = 0; for (uint32_t i = 0; i < n; i++) if (!strcmp(de[i].extensionName, *w)) have = 1;
        printf("  %-48s %s\n", *w, have ? "yes" : "NO");
    }
    VkPhysicalDeviceFeatures f; IP(inst, vkGetPhysicalDeviceFeatures)(pd[0], &f);
    printf("features: geometryShader %u tessellationShader %u sparseBinding %u sparseResidencyBuffer %u sparseResidencyImage2D %u "
           "sparseResidencyImage3D %u sparseResidencyAliased %u shaderResourceResidency %u multiDrawIndirect %u depthBounds %u "
           "pipelineStatisticsQuery %u shaderFloat64 %u shaderInt64 %u textureCompressionBC %u variableMultisampleRate %u\n",
           f.geometryShader, f.tessellationShader, f.sparseBinding, f.sparseResidencyBuffer, f.sparseResidencyImage2D,
           f.sparseResidencyImage3D, f.sparseResidencyAliased, f.shaderResourceResidency, f.multiDrawIndirect, f.depthBounds,
           f.pipelineStatisticsQuery, f.shaderFloat64, f.shaderInt64, f.textureCompressionBC, f.variableMultisampleRate);
    printf("all device extensions:"); for (uint32_t i = 0; i < n; i++) printf(" %s", de[i].extensionName); printf("\n");
    return 0;
}
