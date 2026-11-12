/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include <dlfcn.h>
#include <mach-o/loader.h>
#include <mach-o/nlist.h>
#include <mach/mach.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#define VK_NO_PROTOTYPES 1
#include <vulkan/vulkan.h>
#include "nvmtl_vk.h"
#include "nvmtl_plock.h"
#include "nvmtl_tex_upload.h"
#include "nvmtl_cmd_refusal.h"
#include "nvmtl_loss.h"
#include "nvmtl_counter_heap.h"
#include "nvmtl_copy_compat.h"
#include "nvmtl_sampler_state.h"
static int g_sampler_anisotropy, g_sampler_mirror_clamp;
static int g_scalar_layout;

void nvlog(const char *fmt, ...);
void nvlog_keep(const char *fmt, ...);

static __thread uint32_t g_copy_aspect_override, g_copy_bpp_override;
void nvmtl_vk_copy_aspect_override(uint32_t aspect, uint32_t bpp) { g_copy_aspect_override = aspect; g_copy_bpp_override = bpp; }
static VkImageAspectFlags nvmtl_copy_aspect(const nvk_image *img)
{
    if (g_copy_aspect_override) return (VkImageAspectFlags)g_copy_aspect_override;
    switch (img ? img->fmt : 0) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}
static VkImageAspectFlags nvmtl_barrier_aspect(const nvk_image *img)
{
    switch (img ? img->fmt : 0) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT: return VK_IMAGE_ASPECT_DEPTH_BIT;
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    case VK_FORMAT_S8_UINT: return VK_IMAGE_ASPECT_STENCIL_BIT;
    default: return VK_IMAGE_ASPECT_COLOR_BIT;
    }
}

#include <unistd.h>
#include <limits.h>
#include <sys/stat.h>
#include <pthread.h>
static pthread_mutex_t g_gpu_clock_lock = PTHREAD_MUTEX_INITIALIZER;
static inline const char *nvmtl_pick_vk(const char *staged, const char *dev) {
    return access(staged, R_OK) == 0 ? staged : dev;
}
#define NVMTL_LOADER nvmtl_companion("libvulkan.dylib")
/*
 * The Vulkan driver under this process's Metal device follows the GPU it runs on: NVK for NVIDIA (the NVAccel
 * accelerator), RADV for AMD and ANV for Intel (NMXAccel, the accelerator for the GPUs NMXGPU drives).
 */
#include <IOKit/IOKitLib.h>
static uint16_t g_vendor;
static uint16_t nvmtl_pci_vendor_of(io_service_t accel)
{
    io_registry_entry_t pci = 0; uint16_t v = 0;
    if (IORegistryEntryGetParentEntry(accel, kIOServicePlane, &pci) != KERN_SUCCESS) return 0;
    CFTypeRef d = IORegistryEntryCreateCFProperty(pci, CFSTR("vendor-id"), kCFAllocatorDefault, 0);
    if (d && CFGetTypeID(d) == CFDataGetTypeID() && CFDataGetLength((CFDataRef)d) >= 2) {
        const UInt8 *b = CFDataGetBytePtr((CFDataRef)d); v = (uint16_t)(b[0] | (b[1] << 8));
    }
    if (d) CFRelease(d);
    IOObjectRelease(pci);
    return v;
}
/* the vendor of the accelerator a Metal device was made on (0 if the port is neither of ours); the first one wins */
uint16_t nvmtl_vk_set_accelerator(mach_port_t accel)
{
    uint16_t v = 0;
    if (accel && IOObjectConformsTo(accel, "NMXAccel")) { v = nvmtl_pci_vendor_of(accel); if (v != 0x1002 && v != 0x8086) v = 0; }
    else if (accel && IOObjectConformsTo(accel, "NVAccel")) v = 0x10de;
    if (v && !g_vendor) g_vendor = v;
    return v;
}
/* a process that starts Vulkan before Metal makes a device: NVIDIA when its accelerator runs, else the AMD/Intel one */
uint16_t nvmtl_vk_vendor(void)
{
    if (g_vendor) return g_vendor;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccel"));
    if (s) { IOObjectRelease(s); g_vendor = 0x10de; return g_vendor; }
    s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    uint16_t v = s ? nvmtl_pci_vendor_of(s) : 0;
    if (s) IOObjectRelease(s);
    g_vendor = (v == 0x1002 || v == 0x8086) ? v : 0x10de;
    return g_vendor;
}
static const char *nvmtl_vk_icd_json(void) {
    switch (nvmtl_vk_vendor()) {
    case 0x1002: return "/Library/GPUBundles/nvmtl/radeon_icd.json";
    case 0x8086: return "/Library/GPUBundles/nvmtl/intel_icd.json";
    default:     return "/Library/GPUBundles/nvmtl/nvk_icd.json";
    }
}
#define NVMTL_ICD    nvmtl_vk_icd_json()
static inline const char *nvmtl_icd_lib_path(void) {
    const char *e = getenv("NVMTL_ICD_LIB"); if (e && access(e, R_OK) == 0 && nvmtl_code_path_allowed(e)) return e;
    switch (nvmtl_vk_vendor()) {
    case 0x1002: return nvmtl_companion("libvulkan_radeon.dylib");
    case 0x8086: return nvmtl_companion("libvulkan_intel.dylib");
    default:     return nvmtl_companion("libvulkan_nouveau.dylib");
    }
}
#define NVMTL_ICD_LIB nvmtl_icd_lib_path()

static void *g_lib;
static int g_sample_loader_unwrapped;
static VkInstance g_inst; static VkPhysicalDevice g_pdev; static VkDevice g_dev; static VkQueue g_queue;
static int g_host_query_reset, g_occlusion_precise;
static int g_atomic64_buffer, g_atomic64_shared;
static int g_coop_matrix, g_float16, g_vulkan_memory_model, g_vulkan_memory_device_scope, g_storage16, g_uniform_storage16;
static int g_bindless;
static VkPhysicalDeviceMemoryProperties g_memp;
static uint32_t g_qfam = UINT32_MAX;
static uint32_t g_timestamp_bits;
static PFN_vkGetCalibratedTimestampsEXT pvkGetCalibratedTimestampsEXT;
static nvmtl_gpu_clock g_gpu_clock;
static char g_name[256];
static VkPhysicalDeviceLimits g_lim;
static uint32_t g_sm_count;
static _Atomic int g_state;
static int g_sample_rate_shading;   /* the sampleRateShading feature, enabled when present */
int nvmtl_vk_sample_rate_shading(void) { return g_sample_rate_shading; }
#include "nvmtl_recover.h"   /* GPU reset recovery: a banned queue replaced on the same device */
/* device-loss bookkeeping (nvmtl_vk_note_loss) */
static _Atomic int g_lost_after_up;
static _Atomic uint64_t g_up_ns;
static const char *g_last_submit_who;
static _Atomic long g_slow_submit_ms;
static _Atomic uint64_t g_submit_wait_ns, g_submit_waits;
void nvmtl_vk_submit_wait_stats(uint64_t *total_ns, uint64_t *waits)
{
    if (total_ns) *total_ns = atomic_load(&g_submit_wait_ns);
    if (waits) *waits = atomic_load(&g_submit_waits);
}
static long nvmtl_gpu_wait_budget_ms(void)
{
    static long cached;
    if (cached) return cached;
    const char *e = getenv("NVRM_FLUSH_TIMEOUT_MS");
    long v = e ? atol(e) : 0;
    if (v > 0) { cached = v; return cached; }
    const char *me = getprogname();
    cached = (me && strcmp(me, "WindowServer") == 0) ? 5000 : 60000;
    return cached;
}
static size_t g_alloc_bytes;
static uint64_t g_buf_made, g_buf_gone, g_img_made, g_img_gone;
static uint64_t g_pool_blocks, g_pool_live, g_pool_bytes;
static uint64_t g_img_align, g_sys_align;
void (*nvmtl_purge_report_hook)(const char *why);
static uint64_t g_budget_delta;
static int g_budget_stale;
static void nvmtl_vk_budget_charge(uint64_t bytes)
{
    g_budget_delta += (bytes + 65535ull) & ~65535ull;
}
static uint64_t g_vram_bytes, g_sys_bytes, g_spilled;
static int g_budget_ext;
static int g_rt;
static int g_bary;  /* VK_KHR_fragment_shader_barycentric with fragmentShaderBarycentric: backs the barycentric claim */
#define NVMTL_RT_BUFFER_USAGE (g_rt ? (VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR) : 0)
static VkPhysicalDeviceAccelerationStructurePropertiesKHR g_asprops;
static PFN_vkCreateAccelerationStructureKHR pvkCreateAccelerationStructureKHR;
static PFN_vkDestroyAccelerationStructureKHR pvkDestroyAccelerationStructureKHR;
static PFN_vkGetAccelerationStructureBuildSizesKHR pvkGetAccelerationStructureBuildSizesKHR;
static PFN_vkCmdBuildAccelerationStructuresKHR pvkCmdBuildAccelerationStructuresKHR;
static PFN_vkGetAccelerationStructureDeviceAddressKHR pvkGetAccelerationStructureDeviceAddressKHR;
static PFN_vkCmdCopyAccelerationStructureKHR pvkCmdCopyAccelerationStructureKHR;
static PFN_vkCmdWriteAccelerationStructuresPropertiesKHR pvkCmdWriteAccelerationStructuresPropertiesKHR;
#define NVMTL_VRAM_NONIMAGE (2655ull << 20)
#define NVMTL_VRAM_HEADROOM_DEFAULT (1024ull << 20)
static uint64_t nvmtl_vk_headroom(void)
{
    static long long ovr = -2;
    if (ovr == -2) { const char *e = getenv("NVMTL_VRAM_HEADROOM_MB"); ovr = (e && *e) ? atoll(e) : -1; }
    return ovr >= 0 ? (uint64_t)ovr << 20 : NVMTL_VRAM_HEADROOM_DEFAULT;
}
#define NVMTL_VRAM_HEADROOM nvmtl_vk_headroom()
static int nvmtl_vk_vram_wall(uint64_t want, uint64_t *freev_out, uint64_t *bud_out, uint64_t *used_out, int *measured_out);

static uint64_t g_buf_vram_refused;
static uint64_t g_heap_vram_refused;
static _Atomic uint64_t g_cmd_begun, g_cmd_submitted, g_cmd_abandoned;   /* 33ed311: atomic (several encoding threads) */
void nvmtl_vk_cmd_counts(uint64_t *begun, uint64_t *submitted, uint64_t *abandoned)
{
    if (begun) *begun = g_cmd_begun; if (submitted) *submitted = g_cmd_submitted; if (abandoned) *abandoned = g_cmd_abandoned;
}
void nvmtl_vk_counts(uint64_t *bm, uint64_t *bg, uint64_t *im, uint64_t *ig, size_t *bytes)
{
    if (bm) *bm = g_buf_made;  if (bg) *bg = g_buf_gone;
    if (im) *im = g_img_made;  if (ig) *ig = g_img_gone;
    if (bytes) *bytes = g_alloc_bytes;
}

#define INSTANCE_FNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures) \
    X(vkEnumerateDeviceExtensionProperties) X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceMemoryProperties) X(vkGetPhysicalDeviceMemoryProperties2) X(vkGetPhysicalDeviceProperties2) X(vkCreateDevice) X(vkGetDeviceProcAddr)
#define DEVICE_FNS(X) X(vkGetDeviceQueue) X(vkCreateBuffer) X(vkGetBufferMemoryRequirements) X(vkAllocateMemory) \
    X(vkBindBufferMemory) X(vkMapMemory) X(vkUnmapMemory) X(vkCreateImage) X(vkGetImageMemoryRequirements) X(vkBindImageMemory) \
    X(vkCreateImageView) X(vkCreateRenderPass) X(vkCreateFramebuffer) X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkDestroyPipeline) X(vkDestroyRenderPass) X(vkCreateGraphicsPipelines) X(vkCreateComputePipelines) X(vkCmdDispatch) X(vkCmdPushConstants) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkCmdBeginRenderPass) X(vkCmdBindPipeline) X(vkCmdDraw) X(vkCmdEndRenderPass) X(vkEndCommandBuffer) \
    X(vkQueueSubmit) X(vkQueueBindSparse) X(vkCreateFence) X(vkWaitForFences) X(vkResetFences) X(vkDestroyFence) X(vkCmdCopyImageToBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyBuffer) \
    X(vkCmdPipelineBarrier) X(vkCmdClearColorImage) X(vkCmdClearDepthStencilImage) X(vkDeviceWaitIdle) X(vkFreeCommandBuffers) X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetPrimitiveTopology) X(vkCmdSetPrimitiveRestartEnable) X(vkCmdBindVertexBuffers) X(vkCreateDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) X(vkDestroyDescriptorSetLayout) X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCmdBindDescriptorSets) X(vkCreateSampler) X(vkDestroySampler) X(vkFreeDescriptorSets) X(vkCmdSetBlendConstants) X(vkCmdSetStencilReference) X(vkCmdSetDepthBias) X(vkCmdFillBuffer) X(vkCmdBindIndexBuffer) X(vkCmdDrawIndexed) X(vkCmdCopyImage) X(vkCmdBlitImage) X(vkGetBufferDeviceAddress) X(vkCmdWriteTimestamp) X(vkCreateQueryPool) X(vkCmdResetQueryPool) X(vkCmdBeginQuery) X(vkCmdEndQuery) X(vkCmdCopyQueryPoolResults) X(vkGetQueryPoolResults) X(vkDestroyQueryPool) X(vkResetQueryPool) X(vkCmdDrawIndirect) X(vkCmdDrawIndexedIndirect) X(vkCmdDispatchIndirect) X(vkCmdSetCullMode) X(vkCmdSetFrontFace) X(vkCmdSetDepthTestEnable) X(vkCmdSetDepthWriteEnable) X(vkCmdSetDepthCompareOp) \
    X(vkDestroyBuffer) X(vkFreeMemory) X(vkDestroyImage) X(vkDestroyImageView) X(vkDestroyFramebuffer) X(vkDestroyCommandPool) \
    X(vkCmdSetStencilTestEnable) X(vkCmdSetStencilOp) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask) X(vkCmdSetDepthBiasEnable) \
    X(vkCmdSetDepthBoundsTestEnable) X(vkCmdSetDepthBounds)
static int g_dyn3;
static int g_provoking;   /* provoking vertex is per-draw dynamic state, and the last vertex can provoke */
static int g_divisor;     /* per-instance step rates (VK_KHR_vertex_attribute_divisor), up to g_max_divisor */
static uint32_t g_max_divisor;
static PFN_vkCmdSetProvokingVertexModeEXT pvkCmdSetProvokingVertexModeEXT;
static int g_tess;
static int g_indep;
static int g_a2one;
static int g_depth_bounds;   /* the depthBounds feature: Metal's setDepthTestMinBound:maxBound: */
static int g_multidraw;      /* multiDrawIndirect: runs of GPU-encodable ICB slots as one indirect draw (drawCount > 1) */
static int g_dual;
static int g_wide;
static PFN_vkCmdSetLineWidth pvkCmdSetLineWidth;
static int g_pstat;
/* compute1: NVMTL_CS_SIMD=16|32 - every compute variant is compiled at that SIMD width (VK_EXT_subgroup_size_control,
   core 1.3 requiredSubgroupSize) and threadExecutionWidth reports it. Unset (default): the compiler picks, as prod7.
   An A/B switch for the Geekbench compute gap: brw takes SIMD32 on Xe2 unless it spills or is past its pressure
   estimate, and prod7 reports 32 whatever was picked. */
static uint32_t g_cs_simd;
static int g_sync2;   /* compute2: synchronization2 enabled; vkCmdPipelineBarrier2 loaded after the device */
static PFN_vkCmdPipelineBarrier2 pvkCmdPipelineBarrier2_;
uint32_t nvmtl_vk_cs_simd(void) { return g_cs_simd; }
static PFN_vkGetPipelineExecutablePropertiesKHR p_pstat_props;
static PFN_vkGetPipelineExecutableStatisticsKHR p_pstat_stats;
static int g_mutable;
static int g_mutable_layout;
/* The descriptor model (DESCRIPTOR-MODEL.md): 0 = classic descriptor sets; 1 = VK_EXT_descriptor_buffer (nvmtl_dbuf.c),
   chosen at device creation by NVMTL_DESC_MODEL=buffer. g_heap_ready: the bindless heap (set 2) exists, in either model. */
static int g_dbuf, g_heap_ready;
static VkPhysicalDeviceDescriptorBufferPropertiesEXT g_dbp;
static int nvmtl_db_probe(VkPhysicalDeviceDescriptorBufferFeaturesEXT *feat);
static int nvmtl_db_device_ready(int robust);
static VkPipelineCreateFlags nvmtl_db_pipeline_flags(void);
static VkDescriptorSetLayoutCreateFlags nvmtl_db_layout_flags(void);
static int nvmtl_db_heap_create_locked(void);
static void nvmtl_db_heap_write_image(uint32_t slot, const void *view);
static void nvmtl_db_heap_write_sampler(uint32_t slot, const void *s);
static void nvmtl_db_texel_note(const void *view, uint64_t addr, uint64_t range, uint32_t fmt);
static void nvmtl_db_texel_forget(const void *view);
static PFN_vkCreateBufferView pvkCreateBufferView; static PFN_vkDestroyBufferView pvkDestroyBufferView;
static void *g_dummyTexAlias, *g_dummyTexView;
static void nvmtl_spirv_texel_mask(const void *p, size_t n, uint64_t out[4]);
static int g_host_import; static size_t g_host_align;
static int g_linmod;
static int g_marker_ext; static PFN_vkCmdWriteBufferMarker2AMD pvkCmdWriteBufferMarker2AMD;   // VK_AMD_buffer_marker: the completion witness
static PFN_vkGetMemoryHostPointerPropertiesEXT pvkGetMemoryHostPointerPropertiesEXT;
static int g_sample_locations;
#include "nvmtl_sample_depth_contract.h"
static VkResult (*pnvk_sample_depth_contract)(VkDevice, uint32_t *, uint32_t *);
static VkResult (*pnvk_sample_depth_image)(VkDevice, VkImage, uint32_t *);
static uint32_t g_sample_depth_flags, g_sample_depth_counts;
static VkPhysicalDeviceSampleLocationsPropertiesEXT g_sample_props;
static PFN_vkCmdSetSampleLocationsEXT pvkCmdSetSampleLocationsEXT;
static int g_clip;
static int g_topo_any;
static PFN_vkCmdSetPolygonModeEXT pvkCmdSetPolygonModeEXT;
static PFN_vkCmdSetDepthClipEnableEXT pvkCmdSetDepthClipEnableEXT;
static PFN_vkCmdBindVertexBuffers2 pvkCmdBindVertexBuffers2;
/* glcpu4 NVMTL_VB_ADDR: set only when the piece is on and the device enabled VK_KHR_device_address_commands */
static PFN_vkCmdBindVertexBuffers3KHR pvkCmdBindVertexBuffers3KHR;
NVMTL_G4_SWITCH(nvmtl_vb_addr_on, "NVMTL_VB_ADDR", "vertex buffers bound by device address (vkCmdBindVertexBuffers3KHR)")
static PFN_vkCreateRenderPass2 pvkCreateRenderPass2;
static uint32_t g_dsr_depth_modes, g_dsr_stencil_modes, g_dsr_indep, g_dsr_indep_none;
static void *nvmtl_rp_shape_variant(nvk_pipeline *p, uint32_t load, uint32_t prm, uint32_t dmode, uint32_t smode);
#define DECL(f) static PFN_##f p##f;
static PFN_vkGetInstanceProcAddr pvkGetInstanceProcAddr; static PFN_vkCreateInstance pvkCreateInstance;
// Chrome's GPU process (and other sandboxed renderers) may not read sysctl hw.memsize, so the driver's
// sysconf(_SC_PHYS_PAGES) returns -1 there; NVK then fails "Failed to query total system memory", the device never comes
// up and the app has no Metal device (measured 10-12: Chrome on an RTX 5060 had no WebGL with its GPU sandbox on, and
// WebGL on the GPU with it off). The kernel's host_info answers the same number through a Mach call sandboxes allow.
#include "nvmtl_fd_token.h"
/* A descriptor for /dev/null where a sandbox refuses to open it: WebKit's GPU process (com.apple.WebKit.GPUProcess.sb)
   may not read /dev/null ("deny(1) file-read-data /dev/null"), but its syscall list has dup, and an XPC service starts
   with its standard descriptors on /dev/null. The B580 user-space driver (xemac_open) and the VRAM-shared surface imports
   use such a descriptor only as a handle: WAS, there, ANV's device open failed and vkEnumeratePhysicalDevices found no
   device, so Safari's WebGL had none. */
static int nvmtl_devnull_fd(int flags)
{
    int fd = open("/dev/null", flags);
    if (fd >= 0 || (errno != EPERM && errno != EACCES)) return fd;
    const int err = errno;
    for (int s = 0; s <= 2; s++) {
        struct stat st;
        if (fstat(s, &st) != 0 || !S_ISCHR(st.st_mode)) continue;   /* a character device: /dev/null for an XPC service */
        int d = dup(s);
        if (d < 0) continue;
        if (flags & O_CLOEXEC) (void)fcntl(d, F_SETFD, FD_CLOEXEC);
        static int said; if (!said++) nvlog("vk: /dev/null cannot be opened in %s (a sandbox): its descriptor %d is used instead", getprogname(), s);
        return d;
    }
    errno = err;
    return -1;
}
/* the Vulkan driver's open(): /dev/null through nvmtl_devnull_fd, everything else unchanged */
static int nvmtl_icd_open_devnull(const char *path, int flags, ...)
{
    int mode = 0;
    if (flags & O_CREAT) { va_list ap; va_start(ap, flags); mode = va_arg(ap, int); va_end(ap); }
    if (path && !strcmp(path, "/dev/null")) return nvmtl_devnull_fd(flags);
    return open(path, flags, mode);
}
static long nvmtl_sysconf(int name)
{
    long v = sysconf(name);
    if (v > 0 || name != _SC_PHYS_PAGES) return v;
    static mach_port_t host;
    if (!host) host = mach_host_self();
    host_basic_info_data_t hi; mach_msg_type_number_t n = HOST_BASIC_INFO_COUNT;
    if (host_info(host, HOST_BASIC_INFO, (host_info_t)&hi, &n) != KERN_SUCCESS || !hi.max_mem) return v;
    long page = getpagesize();
    return page > 0 ? (long)(hi.max_mem / (uint64_t)page) : v;
}
// Point one imported symbol of the image that holds `inside` at `repl` (its lazy and non-lazy pointers). Returns how
// many pointers were changed. The driver's own dylib only: nothing else in the process is touched.
static int nvmtl_rebind_import(const void *inside, const char *symbol, void *repl)
{
    Dl_info di;
    if (!dladdr(inside, &di) || !di.dli_fbase) return -1;
    const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
    if (mh->magic != MH_MAGIC_64) return -1;
    const struct segment_command_64 *text = NULL, *linkedit = NULL;
    const struct symtab_command *st = NULL; const struct dysymtab_command *dst = NULL;
    const uint8_t *lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd == LC_SEGMENT_64) {
            const struct segment_command_64 *sg = (const struct segment_command_64 *)c;
            if (!strcmp(sg->segname, SEG_TEXT)) text = sg;
            else if (!strcmp(sg->segname, SEG_LINKEDIT)) linkedit = sg;
        } else if (c->cmd == LC_SYMTAB) st = (const struct symtab_command *)c;
        else if (c->cmd == LC_DYSYMTAB) dst = (const struct dysymtab_command *)c;
    }
    if (!text || !linkedit || !st || !dst || !dst->nindirectsyms) return -1;
    const intptr_t slide = (intptr_t)mh - (intptr_t)text->vmaddr;
    const uint8_t *lebase = (const uint8_t *)(slide + linkedit->vmaddr - linkedit->fileoff);
    const struct nlist_64 *syms = (const struct nlist_64 *)(lebase + st->symoff);
    const char *strs = (const char *)(lebase + st->stroff);
    const uint32_t *ind = (const uint32_t *)(lebase + dst->indirectsymoff);
    int changed = 0;
    lc = (const uint8_t *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds; i++, lc += ((const struct load_command *)lc)->cmdsize) {
        const struct load_command *c = (const struct load_command *)lc;
        if (c->cmd != LC_SEGMENT_64) continue;
        const struct segment_command_64 *sg = (const struct segment_command_64 *)c;
        if (strncmp(sg->segname, "__DATA", 6)) continue;
        const struct section_64 *sec = (const struct section_64 *)(sg + 1);
        for (uint32_t k = 0; k < sg->nsects; k++, sec++) {
            const uint32_t type = sec->flags & SECTION_TYPE;
            if (type != S_LAZY_SYMBOL_POINTERS && type != S_NON_LAZY_SYMBOL_POINTERS) continue;
            void **slots = (void **)(slide + sec->addr);
            for (uint64_t e = 0; e < sec->size / sizeof(void *); e++) {
                if ((uint64_t)sec->reserved1 + e >= dst->nindirectsyms) break;   // a malformed table ends the section
                const uint32_t si = ind[sec->reserved1 + e];
                if (si == INDIRECT_SYMBOL_ABS || si == INDIRECT_SYMBOL_LOCAL || (si & INDIRECT_SYMBOL_ABS) || si >= st->nsyms) continue;
                if (syms[si].n_un.n_strx >= st->strsize) continue;
                if (strcmp(strs + syms[si].n_un.n_strx, symbol)) continue;
                vm_address_t page = (vm_address_t)&slots[e] & ~(vm_address_t)(getpagesize() - 1);
                // __DATA_CONST is read-only after binding: open the one page, write, close it again
                if (vm_protect(mach_task_self(), page, getpagesize(), 0, VM_PROT_READ | VM_PROT_WRITE | VM_PROT_COPY) != KERN_SUCCESS) continue;
                slots[e] = repl;
                if (!strcmp(sg->segname, "__DATA_CONST")) vm_protect(mach_task_self(), page, getpagesize(), 0, VM_PROT_READ);
                changed++;
            }
        }
    }
    return changed;
}

INSTANCE_FNS(DECL) DEVICE_FNS(DECL)
#undef DECL
/* Object generation: bumped BEFORE every image view, buffer view or sampler this process destroys, and whenever a bindless
   heap slot (a texture's gpuResourceID) is given up. A binding slot names its object by pointer; once an object is gone the
   allocator may hand the same address to a new one, and a slot comparison then calls the new object unchanged. The
   dirty-only machinery (nvmtl_dirty.h) trusts what a descriptor set or a skipped argument-buffer table holds only while the
   generation it was made at is current. Bumped before the destroy, so whoever later gets the address sees the new value. */
static _Atomic uint64_t g_objgen = 1;
static inline uint64_t nvmtl_objgen(void) { return atomic_load_explicit(&g_objgen, memory_order_acquire); }
void nvmtl_vk_objgen_bump(void) { atomic_fetch_add_explicit(&g_objgen, 1, memory_order_acq_rel); }
static PFN_vkDestroyImageView g_gen_real_destroy_view;
static VKAPI_ATTR void VKAPI_CALL nvmtl_gen_destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ if (v) nvmtl_vk_objgen_bump(); g_gen_real_destroy_view(d, v, a); }
static void nvmtl_gen_hook_destroy(void)
{ if (!g_gen_real_destroy_view && pvkDestroyImageView) { g_gen_real_destroy_view = pvkDestroyImageView; pvkDestroyImageView = nvmtl_gen_destroy_view; } }
static void nvmtl_hitch_wrap(void);   // NVMTL_HITCH_MS
static int g_hitch_ms = -1;           // NVMTL_HITCH_MS: -1 not read yet, 0 off
#include "nvmtl_gputime.h"            // GPUTIME (/tmp/nvmtl-gputime): GPU time per frame by category
#define LOAD_I(f) do { p##f = (PFN_##f)pvkGetInstanceProcAddr(g_inst, #f); if (!p##f) { nvlog("vk: missing %s", #f); return -1; } } while (0);
#define LOAD_D(f) do { p##f = (PFN_##f)pvkGetDeviceProcAddr(g_dev, #f);   if (!p##f) { nvlog("vk: missing %s", #f); return -1; } } while (0);

_Static_assert(sizeof(VkBuffer) == sizeof(void *) && _Alignof(VkBuffer) == _Alignof(void *), "VkBuffer bridge ABI");
static VkResult nvmtl_opaque_CreateBuffer(VkDevice dev, const VkBufferCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkBuffer handle = VK_NULL_HANDLE;
    VkResult result = pvkCreateBuffer(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
_Static_assert(sizeof(VkDeviceMemory) == sizeof(void *) && _Alignof(VkDeviceMemory) == _Alignof(void *), "VkDeviceMemory bridge ABI");
static VkResult nvmtl_opaque_AllocateMemory(VkDevice dev, const VkMemoryAllocateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkDeviceMemory handle = VK_NULL_HANDLE;
    VkResult result = pvkAllocateMemory(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
_Static_assert(sizeof(VkImage) == sizeof(void *) && _Alignof(VkImage) == _Alignof(void *), "VkImage bridge ABI");
static VkImageCreateFlags nvmtl_sample_depth_flags(const VkImageCreateInfo *info)
{
    if (!g_sample_locations || info->samples <= VK_SAMPLE_COUNT_1_BIT || info->tiling != VK_IMAGE_TILING_OPTIMAL ||
        !(info->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return info->flags;
    switch (info->format) {
    case VK_FORMAT_D16_UNORM: case VK_FORMAT_X8_D24_UNORM_PACK32: case VK_FORMAT_D32_SFLOAT:
    case VK_FORMAT_D16_UNORM_S8_UINT: case VK_FORMAT_D24_UNORM_S8_UINT: case VK_FORMAT_D32_SFLOAT_S8_UINT:
        return info->flags | VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT;
    default: return info->flags;
    }
}
static int nvmtl_fbfetch_on(void);
static VkResult nvmtl_typed_CreateImage(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator, VkImage *out)
{
    if (!info || !out) return VK_ERROR_UNKNOWN;
    VkImageCreateInfo adjusted = *info;
    adjusted.flags = nvmtl_sample_depth_flags(info);
    // a colour attachment is also an input attachment for [[color(n)]] framebuffer fetch (nvmtl_fbfetch_on)
    if ((adjusted.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && nvmtl_fbfetch_on()) adjusted.usage |= VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;
    VkImage handle = VK_NULL_HANDLE;
    VkResult result = pvkCreateImage(dev, &adjusted, allocator, &handle);
    if (result != VK_SUCCESS) return result;
    if (g_sample_locations &&
        (adjusted.flags & VK_IMAGE_CREATE_SAMPLE_LOCATIONS_COMPATIBLE_DEPTH_BIT_EXT)) {
        uint32_t flags = 0;
        result = pnvk_sample_depth_image ? pnvk_sample_depth_image(dev, handle, &flags) : VK_ERROR_FEATURE_NOT_PRESENT;
        if (result != VK_SUCCESS || (flags & NVMTL_SAMPLE_DEPTH_CONTRACT_REQUIRED) != NVMTL_SAMPLE_DEPTH_CONTRACT_REQUIRED) {
            pvkDestroyImage(dev, handle, allocator);
            return result != VK_SUCCESS ? result : VK_ERROR_FEATURE_NOT_PRESENT;
        }
    }
    *out = handle;
    return VK_SUCCESS;
}
static VkResult nvmtl_opaque_CreateImage(VkDevice dev, const VkImageCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkImage handle = VK_NULL_HANDLE;
    VkResult result = nvmtl_typed_CreateImage(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
_Static_assert(sizeof(VkImageView) == sizeof(void *) && _Alignof(VkImageView) == _Alignof(void *), "VkImageView bridge ABI");
static VkResult nvmtl_opaque_CreateImageView(VkDevice dev, const VkImageViewCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkImageView handle = VK_NULL_HANDLE;
    VkResult result = pvkCreateImageView(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
_Static_assert(sizeof(VkPipelineLayout) == sizeof(void *) && _Alignof(VkPipelineLayout) == _Alignof(void *), "VkPipelineLayout bridge ABI");
static VkResult nvmtl_opaque_CreatePipelineLayout(VkDevice dev, const VkPipelineLayoutCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkPipelineLayout handle = VK_NULL_HANDLE;
    VkResult result = pvkCreatePipelineLayout(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
_Static_assert(sizeof(VkCommandPool) == sizeof(void *) && _Alignof(VkCommandPool) == _Alignof(void *), "VkCommandPool bridge ABI");
static VkResult nvmtl_opaque_CreateCommandPool(VkDevice dev, const VkCommandPoolCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    VkCommandPool handle = VK_NULL_HANDLE;
    VkResult result = pvkCreateCommandPool(dev, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}
static VkResult nvmtl_opaque_CreateGraphicsPipelines(VkDevice dev, VkPipelineCache cache, uint32_t count, const VkGraphicsPipelineCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    if (count != 1) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipeline handle = VK_NULL_HANDLE;
    VkGraphicsPipelineCreateInfo pinfo;
    if (g_pstat) { pinfo = *info; pinfo.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR; info = &pinfo; }
    VkResult result = pvkCreateGraphicsPipelines(dev, cache, count, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    else if (handle) pvkDestroyPipeline(dev, handle, allocator);
    return result;
}
int nvmtl_vk_pipeline_stats(void *pipe, char *out, size_t n)
{
    if (!n) return -1; out[0] = 0;
    if (!g_pstat || !pipe || !p_pstat_props || !p_pstat_stats) return -1;
    VkPipelineInfoKHR pi = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, NULL, (VkPipeline)pipe };
    uint32_t ne = 0; if (p_pstat_props(g_dev, &pi, &ne, NULL) != VK_SUCCESS || !ne) return -1;
    VkPipelineExecutablePropertiesKHR ep[4]; if (ne > 4) ne = 4;
    for (uint32_t i = 0; i < ne; i++) ep[i] = (VkPipelineExecutablePropertiesKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
    if (p_pstat_props(g_dev, &pi, &ne, ep) < 0) return -1;
    static const char *lname[] = { "Instruction count", "Static cycle count", "Max warps/SM", "Number of GPRs", "Spills to memory",
                                   "Fills from memory", "Spills to reg", "Fills from reg", "Code size", "SLM size",
                                   /* ANV (brw) */ "Instructions", "SENDs", "Loops", "Cycles", "Spills", "Fills", "Scratch Memory Size",
                                   "GRF registers", "Max dispatch width", "Max live registers", "Workgroup Memory Size" };   // dispatch width 0: see SIMD
    static const char *sname[] = { "instr", "cyc", "warps", "gpr", "spillM", "fillM", "spillR", "fillR", "code", "slm",
                                   "instr", "sends", "loops", "cyc", "spill", "fill", "scratch", "grf", "simd", "live", "slm" };
    static const char *skip[] = { "VRT size", "VRT threads", "Push constant ranges", "Push constant registers", "Source hash" };
    size_t w = 0;
    for (uint32_t e = 0; e < ne && w + 1 < n; e++) {
        int k = snprintf(out + w, n - w, "%s%s SIMD%u:", e ? " | " : "", ep[e].name, ep[e].subgroupSize); if (k > 0) w += (size_t)k < n - w ? (size_t)k : n - w - 1;
        VkPipelineExecutableInfoKHR ei = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, NULL, (VkPipeline)pipe, e };
        VkPipelineExecutableStatisticKHR st[16]; uint32_t ns = 16;
        for (uint32_t i = 0; i < ns; i++) st[i] = (VkPipelineExecutableStatisticKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR };
        if (p_pstat_stats(g_dev, &ei, &ns, st) < 0) continue;
        for (uint32_t i = 0; i < ns && w + 1 < n; i++) {
            const char *nm = st[i].name; for (unsigned j = 0; j < sizeof lname / sizeof *lname; j++) if (!strcmp(nm, lname[j])) { nm = sname[j]; break; }
            int skipped = 0; for (unsigned j = 0; j < sizeof skip / sizeof *skip; j++) skipped |= !strcmp(nm, skip[j]);
            if (skipped) continue;
            unsigned long long v = st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? (unsigned long long)st[i].value.i64
                                 : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_FLOAT64_KHR ? (unsigned long long)st[i].value.f64
                                 : st[i].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_BOOL32_KHR ? (unsigned long long)st[i].value.b32 : st[i].value.u64;
            k = snprintf(out + w, n - w, " %s %llu", nm, v); if (k > 0) w += (size_t)k < n - w ? (size_t)k : n - w - 1;
        }
    }
    return 0;
}
static VkResult nvmtl_opaque_CreateComputePipelines(VkDevice dev, VkPipelineCache cache, uint32_t count, const VkComputePipelineCreateInfo *info, const VkAllocationCallbacks *allocator, void **out)
{
    if (count != 1) return VK_ERROR_INITIALIZATION_FAILED;
    VkPipeline handle = VK_NULL_HANDLE;
    VkComputePipelineCreateInfo pinfo;   // NVMTL_PSTAT=1: compute kernels' statistics too (KTIME rows show them)
    if (g_pstat) { pinfo = *info; pinfo.flags |= VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR; info = &pinfo; }
    VkResult result = pvkCreateComputePipelines(dev, cache, count, info, allocator, &handle);
    if (result == VK_SUCCESS) *out = handle;
    else if (handle) pvkDestroyPipeline(dev, handle, allocator);
    return result;
}
_Static_assert(sizeof(VkPipeline) == sizeof(void *) && _Alignof(VkPipeline) == _Alignof(void *), "VkPipeline bridge ABI");
_Static_assert(sizeof(VkCommandBuffer) == sizeof(void *) && _Alignof(VkCommandBuffer) == _Alignof(void *), "VkCommandBuffer bridge ABI");
static VkResult nvmtl_opaque_AllocateCommandBuffers(VkDevice dev, const VkCommandBufferAllocateInfo *info, void **out)
{
    if (info->commandBufferCount != 1) return VK_ERROR_INITIALIZATION_FAILED;
    VkCommandBuffer handle = VK_NULL_HANDLE;
    VkResult result = pvkAllocateCommandBuffers(dev, info, &handle);
    if (result == VK_SUCCESS) *out = handle;
    return result;
}

#define VKCK(x, what) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { nvlog("vk: %s -> %d", what, r_); return -1; } } while (0)
#include <fcntl.h>
typedef struct { uint32_t version, surfaceID, plane, pad; uint64_t size_B; } nvmtl_vram_rec;
static void (*pnvk_stage_import)(const nvmtl_vram_rec *);
static uint32_t (*pnvk_surface_dirty)(VkDevice, uint32_t, uint32_t);
static int (*pnvk_sparse)(void);
/* implicit IOSurface sync (Mesa ANV over IntelXe.kext, anv_nvmtl_surface.c): nvmtl_vk_isync */
static VkResult (*panv_surf_cap)(VkDevice, uint32_t *);
static VkResult (*panv_surf_attach)(VkDevice, VkFence, VkSemaphore, uint64_t, uint32_t, uint32_t);
static VkResult (*panv_surf_import)(VkDevice, VkSemaphore, uint32_t, uint32_t, uint64_t *, uint32_t *);
static VkResult (*panv_surf_wait)(VkDevice, uint32_t, uint32_t, int64_t);
/* VRAM-shared IOSurfaces on the B580 (gpuperf phase 2; Mesa 0014 + IntelXe.kext SURFACE_VRAM, behind debug.intelxe.surfvram) */
/* version 2 (Mesa 0021 with IntelXe.kext SURFACE_VRAM2) also returns the layout the backing's creator renders with: a Tile4
   swapchain image of the Vulkan WSI is read through an image of that layout, never as linear bytes */
struct nvmtl_anv_surface_vram_req { uint32_t version, surface_id, plane, flags; uint64_t size, size_out; uint32_t gen; int32_t err;
                                    uint64_t modifier; uint32_t pitch, cpp, width, height, linear_pitch, pad; };
#define NVMTL_MOD_4_TILED ((0x01ull << 56) | 9ull)   /* I915_FORMAT_MOD_4_TILED */
static VkResult (*panv_vram_cap)(VkDevice, uint32_t *);
static VkResult (*panv_vram_stage)(VkDevice, struct nvmtl_anv_surface_vram_req *);
static VkResult (*panv_surf_dirty)(VkDevice, uint32_t, uint32_t, uint32_t);
static int g_timeline;
static int g_null_desc;   /* robustness2 nullDescriptor enabled */
static int g_qsparse, g_sparse;
static int g_extfd;

int nvmtl_vk_format_block(uint32_t f, uint32_t *bw, uint32_t *bh, uint32_t *block_bytes)
{
    if (f >= 131 && f <= 146) { *bw = *bh = 4; *block_bytes = (f <= 134 || f == 139 || f == 140) ? 8 : 16; return 1; }
    *bw = *bh = 1; *block_bytes = 0; return 0;
}
static inline uint32_t nvmtl_blocks(uint32_t v, uint32_t b) { return b > 1 ? (v + b - 1) / b : v; }

static int nvmtl_spirv_bands_ok(const void *p, size_t n, const char *what);
static int nvmtl_spirv_ok(const void *p, size_t n, const char *what)
{
    if (!p || n < 20 || (n & 3u)) {
        nvlog("vk: %s is not SPIR-V (%zu bytes) — refusing to build a pipeline from it", what, n);
        return 0;
    }
    uint32_t magic = ((const uint32_t *)p)[0];
    if (magic != 0x07230203u) {
        nvlog("vk: %s is not SPIR-V (first word 0x%08x, %zu bytes) — refusing to build a pipeline from it",
              what, magic, n);
        return 0;
    }
    if (!(g_atomic64_buffer && g_atomic64_shared)) {
        const uint32_t *w = p;
        size_t words = n / sizeof *w;
        for (size_t i = 5; i < words; ) {
            uint32_t count = w[i] >> 16, op = w[i] & 0xffffu;
            if (!count || count > words - i) return 0;
            if (op == 17 && count == 2 && w[i + 1] == 12) {
                nvlog("vk: %s requires integer atomic64 features unavailable on this device", what);
                return 0;
            }
            i += count;
        }
    }
    {
        const uint32_t *w = p; size_t words = n / sizeof *w;
        for (size_t i = 5; i < words; ) {
            uint32_t count = w[i] >> 16, op = w[i] & 0xffffu;
            if (!count || count > words - i) return 0;
            if (op == 17 && count == 2) {
                uint32_t cap = w[i + 1];
                int supported = cap == 9 ? g_float16 : cap == 4433 ? g_storage16 : cap == 4434 ? g_uniform_storage16 :
                                cap == 5345 ? g_vulkan_memory_model : cap == 5346 ? g_vulkan_memory_device_scope :
                                cap == 6022 ? g_coop_matrix : 1;
                if (!supported) { nvlog("vk: %s requires unavailable enabled capability %u", what, cap); return 0; }
            }
            i += count;
        }
    }
    return nvmtl_spirv_bands_ok(p, n, what);
}

#define NVMTL_VKMSG_SITES    128
#define NVMTL_VKMSG_VERBATIM 8
#define NVMTL_VKMSG_EVERY    4096
static struct { uint64_t key, n; } g_vkmsg_sites[NVMTL_VKMSG_SITES];
static uint64_t g_vkmsg_over;
static int g_vkmsg_want;
static VkDebugUtilsMessengerEXT g_vkmsg;
static pthread_mutex_t g_vkmsg_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t nvmtl_vkmsg_key(const char *id, const char *m)
{
    uint64_t h = 1469598103934665603ull; int dig = 0;
    for (const char *s = id; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ull; }
    h ^= 0xff; h *= 1099511628211ull;
    for (const char *s = m; *s; s++) {
        int d = *s >= '0' && *s <= '9'; if (d && dig) continue; dig = d;
        h ^= d ? (unsigned char)'#' : (unsigned char)*s; h *= 1099511628211ull;
    }
    return h ? h : 1;
}
static int nvmtl_vkmsg_budget(uint64_t key, uint64_t *rep, int *full)
{
    *rep = 0; *full = 0; uint64_t n; int i = 0;
    pthread_mutex_lock(&g_vkmsg_lock);
    for (; i < NVMTL_VKMSG_SITES; i++) {
        if (g_vkmsg_sites[i].key == key) break;
        if (!g_vkmsg_sites[i].key) { g_vkmsg_sites[i].key = key; break; }
    }
    if (i < NVMTL_VKMSG_SITES) n = ++g_vkmsg_sites[i].n; else { n = ++g_vkmsg_over; *full = 1; }
    pthread_mutex_unlock(&g_vkmsg_lock);
    if (n <= NVMTL_VKMSG_VERBATIM) return 1;
    if (n % NVMTL_VKMSG_EVERY == 0) { *rep = n; return 1; }
    return 0;
}
static VkBool32 VKAPI_PTR nvmtl_vkmsg_cb(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                         const VkDebugUtilsMessengerCallbackDataEXT *d, void *user)
{
    (void)type; (void)user;
    const char *id = d && d->pMessageIdName ? d->pMessageIdName : "?", *m = d && d->pMessage ? d->pMessage : "(no message)";
    uint64_t rep; int full;
    if (!nvmtl_vkmsg_budget(nvmtl_vkmsg_key(id, m), &rep, &full)) return VK_FALSE;
    const char *b = strrchr(id, '/'); b = b ? b + 1 : id;
    char t[600]; snprintf(t, sizeof t, "nvk-reason: %s %s: %s", (sev & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) ? "E" : "W", b, m);
    for (char *c = t; *c; c++) if ((unsigned char)*c < 0x20) *c = ' ';
    extern const char *nvmtl_log_path(void);
    FILE *f = fopen(nvmtl_log_path(), "a");
    if (f) {
        if (rep) fprintf(f, "pid %d NVMTL: %s   [logged %llu times now%s]\n", getpid(), t, (unsigned long long)rep, full ? ", table full" : "");
        else     fprintf(f, "pid %d NVMTL: %s%s\n", getpid(), t, full ? "   [table full]" : "");
        fclose(f);
    }
    return VK_FALSE;
}
static int nvmtl_vkmsg_off_by_env(void) { const char *e = getenv("NVMTL_NVK_MESSAGES"); return e && !strcmp(e, "0"); }
static int nvmtl_vkmsg_listed(void)
{
    if (nvmtl_vkmsg_off_by_env()) return 0;
    PFN_vkEnumerateInstanceExtensionProperties en =
        (PFN_vkEnumerateInstanceExtensionProperties)pvkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0; if (!en || en(NULL, &n, NULL) != VK_SUCCESS || !n) return 0;
    VkExtensionProperties *p = calloc(n, sizeof *p); if (!p) return 0;
    int hit = 0; VkResult r = en(NULL, &n, p);
    if (r == VK_SUCCESS || r == VK_INCOMPLETE)
        for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME)) hit = 1;
    free(p); return hit;
}

static int nvmtl_sample_loader_diagnostic(void)
{
    if (pnvk_sample_depth_contract || pnvk_sample_depth_image) return 0;
    const char *layers = getenv("VK_INSTANCE_LAYERS");
    if (!layers || strcmp(layers, "VK_LAYER_KHRONOS_validation") ||
        getenv("VK_LOADER_LAYERS_ENABLE") || getenv("VK_LOADER_LAYERS_ALLOW") ||
        getenv("VK_LOADER_LAYERS_DISABLE")) return 0;
    PFN_vkEnumerateInstanceLayerProperties enlayer =
        (PFN_vkEnumerateInstanceLayerProperties)pvkGetInstanceProcAddr(NULL, "vkEnumerateInstanceLayerProperties");
    PFN_vkEnumerateInstanceExtensionProperties enext =
        (PFN_vkEnumerateInstanceExtensionProperties)pvkGetInstanceProcAddr(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    if (!enlayer || !enext || enlayer(&n, NULL) != VK_SUCCESS || n != 1) return 0;
    VkLayerProperties layer;
    if (enlayer(&n, &layer) != VK_SUCCESS || n != 1 ||
        strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation")) return 0;
    n = 0;
    if (enext(layer.layerName, &n, NULL) != VK_SUCCESS || !n || n > 128) return 0;
    VkExtensionProperties *ext = calloc(n, sizeof *ext);
    if (!ext) return 0;
    int settings = 0, validation = 0;
    VkResult r = enext(layer.layerName, &n, ext);
    if (r == VK_SUCCESS) for (uint32_t i = 0; i < n; ++i) {
        settings |= !strcmp(ext[i].extensionName, VK_EXT_LAYER_SETTINGS_EXTENSION_NAME);
        validation |= !strcmp(ext[i].extensionName, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }
    free(ext); return settings && validation;
}
static VkResult nvmtl_vkmsg_create_instance(VkInstanceCreateInfo *ici)
{
    VkInstanceCreateInfo original = *ici;
    const char *ext[16]; uint32_t count = original.enabledExtensionCount;
    if (count > 13) return pvkCreateInstance(ici, NULL, &g_inst);
    for (uint32_t i = 0; i < count; ++i) ext[i] = original.ppEnabledExtensionNames[i];
    g_vkmsg_want = nvmtl_vkmsg_listed();
    const uint32_t diagnostic_count = count;
    if (g_vkmsg_want) ext[count++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    g_sample_loader_unwrapped = !original.enabledLayerCount && nvmtl_sample_loader_diagnostic();
    VkBool32 wrap = VK_FALSE;
    VkLayerSettingEXT setting = { "VK_LAYER_KHRONOS_validation", "unique_handles",
        VK_LAYER_SETTING_TYPE_BOOL32_EXT, 1, &wrap };
    VkValidationFeatureEnableEXT sync = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
    VkValidationFeaturesEXT validation = { VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT,
        original.pNext, 1, &sync, 0, NULL };
    VkLayerSettingsCreateInfoEXT settings = { VK_STRUCTURE_TYPE_LAYER_SETTINGS_CREATE_INFO_EXT,
        &validation, 1, &setting };
    const char *layer = "VK_LAYER_KHRONOS_validation";
    if (g_sample_loader_unwrapped) {
        ext[count++] = VK_EXT_LAYER_SETTINGS_EXTENSION_NAME;
        ext[count++] = VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME;
        ici->pNext = &settings; ici->enabledLayerCount = 1; ici->ppEnabledLayerNames = &layer;
    }
    ici->enabledExtensionCount = count; ici->ppEnabledExtensionNames = count ? ext : NULL;
    VkResult r = pvkCreateInstance(ici, NULL, &g_inst);
    if (r != VK_SUCCESS && g_vkmsg_want) {

        for (uint32_t i = diagnostic_count; i + 1 < count; ++i) ext[i] = ext[i + 1];
        --count; g_vkmsg_want = 0;
        ici->enabledExtensionCount = count; ici->ppEnabledExtensionNames = count ? ext : NULL;
        r = pvkCreateInstance(ici, NULL, &g_inst);
    }
    if (r != VK_SUCCESS) g_sample_loader_unwrapped = 0;
    *ici = original;
    return r;
}
static void nvmtl_vkmsg_arm(void)
{
    if (!g_vkmsg_want) {
        nvlog("vk: NVK messages OFF: %s", nvmtl_vkmsg_off_by_env() ? "NVMTL_NVK_MESSAGES=0" : "the instance has no VK_EXT_debug_utils");
        return;
    }
    PFN_vkCreateDebugUtilsMessengerEXT c = (PFN_vkCreateDebugUtilsMessengerEXT)pvkGetInstanceProcAddr(g_inst, "vkCreateDebugUtilsMessengerEXT");
    VkDebugUtilsMessengerCreateInfoEXT mi = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
        VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
        nvmtl_vkmsg_cb, NULL };
    VkResult r = c ? c(g_inst, &mi, NULL, &g_vkmsg) : VK_ERROR_EXTENSION_NOT_PRESENT;
    if (r != VK_SUCCESS) { g_vkmsg = VK_NULL_HANDLE; nvlog("vk: NVK messages OFF: vkCreateDebugUtilsMessengerEXT %s %d", c ? "returned" : "is missing,", r); return; }
    nvlog("vk: NVK messages ON: its own warnings and errors land here as \"nvk-reason:\"");
    const char *e = getenv("NVMTL_NVK_MESSAGES");
    if (e && !strcmp(e, "probe")) {
        PFN_vkSubmitDebugUtilsMessageEXT s = (PFN_vkSubmitDebugUtilsMessageEXT)pvkGetInstanceProcAddr(g_inst, "vkSubmitDebugUtilsMessageEXT");
        VkDebugUtilsMessengerCallbackDataEXT cd = { VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CALLBACK_DATA_EXT, NULL, 0,
            "apps-vkmsg/probe:1", 0, "probe delivered by the ICD", 0, NULL, 0, NULL, 0, NULL };
        if (s) s(g_inst, VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT, VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT, &cd);
        else nvlog("vk: NVK messages probe: vkSubmitDebugUtilsMessageEXT is missing");
    }
}

/* The GuC's low-latency hint (Mesa's driconf force_guc_low_latency -> DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT -> the GuC
   SLPC treats the queue's work as compute and keeps the GT clock up while it runs). Without it, SLPC sees short,
   spaced-out submissions as low load and leaves the GT at its 1200 MHz floor: one tiny dispatch's round trip 94 us
   instead of 56 us, Geekbench 7 Metal 92.7k -> 96.1k and OpenCL 86.1k -> 91.1k (B580, 2026-11-09). When idle the GT is in
   RC6 either way. OFF by default since 9 Nov: the first heavy run after a long idle (Geekbench OpenCL after 40 min and
   more) was followed by a GuC MEMORY_CAT_ERROR and a GT reset in 3 of 6 runs with it, 0 of 4 without; the cause is open.
   /Library/GPUBundles/NVMTLDriver.low-latency (IntelXe Manager's switch) turns it on for every process but WindowServer
   and the system's own interface, which run whenever anything on screen moves; NVMTL_GUC_LOW_LATENCY=0/1 decides for
   one process, and a force_guc_low_latency a process was started with stands. */
static void nvmtl_guc_hint_env(void)
{
    static const char *const ui[] = { "WindowServer", "loginwindow", "Dock", "com.apple.dock.extra", "SystemUIServer",
        "ControlCenter", "NotificationCenter", "WindowManager", "ScreenSaverEngine", "legacyScreenSaver" };
    const char *me = getprogname(), *o = getenv("NVMTL_GUC_LOW_LATENCY"), *why; int on;
    int system_ui = 0;
    for (size_t i = 0; me && i < sizeof ui / sizeof *ui; i++) system_ui |= !strcmp(me, ui[i]);
    if (o && *o) { on = *o != '0'; why = "NVMTL_GUC_LOW_LATENCY"; }
    else if (system_ui) { on = 0; why = "the system's interface"; }
    else if (access("/Library/GPUBundles/NVMTLDriver.low-latency", F_OK) == 0) { on = 1; why = "turned on (NVMTLDriver.low-latency)"; }
    else if (getenv("force_guc_low_latency") && *getenv("force_guc_low_latency")) return;   // set by whoever started it
    else { on = 0; why = "default"; }
    setenv("force_guc_low_latency", on ? "true" : "false", 1);
    nvlog("vk: GuC low-latency hint %s for %s (%s)", on ? "on" : "off", me ? me : "?", why);
}
static int nvmtl_vk_initialize(void)
{
    if (g_state) return g_state == 1 ? 0 : -1;
    g_state = -1;
    nvmtl_guc_hint_env();
    if (!getenv("NVRM_FLUSH_TIMEOUT_MS")) {
        char budget[32]; snprintf(budget, sizeof budget, "%ld", nvmtl_gpu_wait_budget_ms());
        setenv("NVRM_FLUSH_TIMEOUT_MS", budget, 1);
        nvlog("vk: GPU wait budget %s ms for %s (WindowServer keeps 5000 - a frozen desktop is worse than a dropped frame)", budget, getprogname());
    }
    // A cache dir set by another process's plugin is inherited: loginwindow runs as root before login, has no writable
    // HOME and so gets root's user cache dir below, and every app it opens at login started with that path and could
    // not write it ("Failed to create ... for shader cache (Permission denied) --- disabling": NAK recompiled every
    // start). One this process cannot use is dropped, and the rules below choose again. (upstream 9b3850c, #399)
    { const char *mc = getenv("MESA_SHADER_CACHE_DIR");
      if (mc && *mc && access(mc, W_OK) != 0 && mkdir(mc, 0700) != 0) {
          nvlog("vk: %s inherited MESA_SHADER_CACHE_DIR=%s, which it cannot write - choosing its own", getprogname(), mc);
          unsetenv("MESA_SHADER_CACHE_DIR"); } }
    if (!getenv("MESA_SHADER_CACHE_DIR") && !getenv("XDG_CACHE_HOME") && !getenv("NVMTL_NO_MESA_CACHE_DIR")) {
        const char *home = getenv("HOME");
        if (!home || access(home, W_OK) != 0) {
            char ucd[PATH_MAX], dir[PATH_MAX];
            const size_t n = confstr(_CS_DARWIN_USER_CACHE_DIR, ucd, sizeof ucd);
            if (n > 0 && n <= sizeof ucd && snprintf(dir, sizeof dir, "%snvmtl-mesa", ucd) < (int)sizeof dir) {
                mkdir(dir, 0700);
                if (access(dir, W_OK) == 0) { setenv("MESA_SHADER_CACHE_DIR", dir, 1);
                    nvlog("vk: %s has no writable HOME (%s) - Mesa shader disk cache at %s", getprogname(), home ?: "unset", dir); }
                else nvlog("vk: %s: no writable Mesa cache dir (%s) - NAK recompiles every start", getprogname(), dir);
            }
        }
    }
    /* GCN 3/4 (Tonga, Fiji, Polaris) have a 16-bit ALU, but RADV reports shaderFloat16 there only with this option (one of
       its own driconf defaults). Without it every half-precision module is refused on those cards by nvmtl_spirv_ok,
       though RADV compiles them. GCN 1/2 have no 16-bit ALU and keep shaderFloat16 off: their half modules stay refused,
       which is what keeps ACO from compiling half arithmetic it cannot lower there. A value the user set wins. */
    if (nvmtl_vk_vendor() == 0x1002) setenv("radv_enable_float16_gfx8", "true", 0);
    g_lib = dlopen(NVMTL_ICD_LIB, RTLD_NOW | RTLD_LOCAL);
    if (g_lib) {
        pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
        if (!pvkGetInstanceProcAddr)
            pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vk_icdGetInstanceProcAddr");
        if (pvkGetInstanceProcAddr) {
            char vid[1200];
            nvlog("vk: driver opened directly: %s (image %s)", NVMTL_ICD_LIB, nvmtl_image_ident((const void *)pvkGetInstanceProcAddr, vid, sizeof vid));
            { int rb = nvmtl_rebind_import((const void *)pvkGetInstanceProcAddr, "_sysconf", (void *)nvmtl_sysconf);
              if (rb <= 0) nvlog("vk: memory-size fallback missing: _sysconf not rebound in %s (%d) -- sandboxed apps may have no device", NVMTL_ICD_LIB, rb); }
            if (nvmtl_vk_vendor() != 0x10de) { int rb = nvmtl_rebind_import((const void *)pvkGetInstanceProcAddr, "_open", (void *)nvmtl_icd_open_devnull);
              if (rb <= 0) nvlog("vk: /dev/null fallback missing: _open not rebound in %s (%d)", NVMTL_ICD_LIB, rb); }
            if (nvmtl_vk_vendor() == 0x10de) { int rb = nvmtl_rebind_import((const void *)pvkGetInstanceProcAddr, "_open", (void *)nvmtl_icd_open);
              if (rb <= 0) nvlog("vk: owned handle fallback missing: _open not rebound in %s (%d)", NVMTL_ICD_LIB, rb); }
            pnvk_stage_import = (void (*)(const nvmtl_vram_rec *))dlsym(g_lib, "nvmtl_nvk_stage_import");
            pnvk_surface_dirty = (uint32_t (*)(VkDevice, uint32_t, uint32_t))dlsym(g_lib, "nvmtl_nvk_surface_dirty");
            pnvk_sample_depth_contract = (VkResult (*)(VkDevice, uint32_t *, uint32_t *))dlsym(g_lib, "nvmtl_nvk_sample_depth_contract_v1");
            pnvk_sample_depth_image = (VkResult (*)(VkDevice, VkImage, uint32_t *))dlsym(g_lib, "nvmtl_nvk_sample_depth_image_v1");
            pnvk_sparse = (int (*)(void))dlsym(g_lib, "nvmtl_nvk_sparse_bind_v1");
            panv_surf_cap = (VkResult (*)(VkDevice, uint32_t *))dlsym(g_lib, "nvmtl_anv_surface_cap_v1");
            panv_surf_attach = (VkResult (*)(VkDevice, VkFence, VkSemaphore, uint64_t, uint32_t, uint32_t))dlsym(g_lib, "nvmtl_anv_surface_attach_v1");
            panv_surf_import = (VkResult (*)(VkDevice, VkSemaphore, uint32_t, uint32_t, uint64_t *, uint32_t *))dlsym(g_lib, "nvmtl_anv_surface_import_v1");
            panv_surf_wait = (VkResult (*)(VkDevice, uint32_t, uint32_t, int64_t))dlsym(g_lib, "nvmtl_anv_surface_wait_v1");
            panv_vram_cap = (VkResult (*)(VkDevice, uint32_t *))dlsym(g_lib, "nvmtl_anv_surface_vram_cap_v1");
            panv_vram_stage = (VkResult (*)(VkDevice, struct nvmtl_anv_surface_vram_req *))dlsym(g_lib, "nvmtl_anv_surface_vram_stage_v1");
            panv_surf_dirty = (VkResult (*)(VkDevice, uint32_t, uint32_t, uint32_t))dlsym(g_lib, "nvmtl_anv_surface_dirty_v1");
            panv_recover = (VkResult (*)(VkDevice, uint32_t, struct nvmtl_anv_recover_info *))dlsym(g_lib, "nvmtl_anv_device_recover_v1");
        }
        else { nvlog("vk: %s has no vkGetInstanceProcAddr", NVMTL_ICD_LIB); dlclose(g_lib); g_lib = NULL; }
    } else {
        nvlog("vk: dlopen(%s): %s", NVMTL_ICD_LIB, dlerror());
    }
    if (!pvkGetInstanceProcAddr) {
        if (!getenv("VK_ICD_FILENAMES")) setenv("VK_ICD_FILENAMES", NVMTL_ICD, 1);
        g_lib = dlopen(NVMTL_LOADER, RTLD_NOW | RTLD_LOCAL);
        if (!g_lib) { nvlog("vk: dlopen(%s): %s", NVMTL_LOADER, dlerror()); return -1; }
        pvkGetInstanceProcAddr = (PFN_vkGetInstanceProcAddr)dlsym(g_lib, "vkGetInstanceProcAddr");
        if (!pvkGetInstanceProcAddr) { nvlog("vk: no vkGetInstanceProcAddr"); return -1; }
        nvlog("vk: fell back to the loader at %s", NVMTL_LOADER);
    }
    pvkCreateInstance = (PFN_vkCreateInstance)pvkGetInstanceProcAddr(NULL, "vkCreateInstance");
    if (!pvkCreateInstance) { nvlog("vk: no vkCreateInstance"); return -1; }

    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "NVMTLDriver", 1, "NVMTL", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VKCK(nvmtl_vkmsg_create_instance(&ici), "vkCreateInstance");
    nvmtl_vkmsg_arm();
    INSTANCE_FNS(LOAD_I)

    uint32_t n = 0; VkResult er = pvkEnumeratePhysicalDevices(g_inst, &n, NULL);
    if (er != VK_SUCCESS || !n) {
        nvlog("vk: zero physical devices through this ICD — vkEnumeratePhysicalDevices -> VkResult %d, count %u, in %s (uid %u euid %u): %s",
              (int)er, n, getprogname(), (unsigned)getuid(), (unsigned)geteuid(),
              er != VK_SUCCESS ? "the ENUMERATION failed, so NVK could not reach the device (not an absent GPU)"
                               : "the enumeration SUCCEEDED and found no device through this ICD");
        return -1; }
    nvlog("vk: pdev %u physical device(s) through this ICD (VkResult %d)", n, (int)er);
    VkPhysicalDevice pds[8]; if (n > 8) n = 8; pvkEnumeratePhysicalDevices(g_inst, &n, pds); g_pdev = pds[0];
    VkPhysicalDeviceProperties pp; pvkGetPhysicalDeviceProperties(g_pdev, &pp);
    snprintf(g_name, sizeof g_name, "%s", pp.deviceName);
    g_lim = pp.limits;
    { VkPhysicalDeviceShaderSMBuiltinsPropertiesNV sm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_SM_BUILTINS_PROPERTIES_NV };
      VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &sm };
      pvkGetPhysicalDeviceProperties2(g_pdev, &p2); g_sm_count = sm.shaderSMCount;
      nvlog("vk: limits — image array layers %u, shared memory %u B, workgroup %ux%ux%u (%u invocations), SMs %u",
            g_lim.maxImageArrayLayers, g_lim.maxComputeSharedMemorySize, g_lim.maxComputeWorkGroupSize[0], g_lim.maxComputeWorkGroupSize[1],
            g_lim.maxComputeWorkGroupSize[2], g_lim.maxComputeWorkGroupInvocations, g_sm_count); }
    pvkGetPhysicalDeviceMemoryProperties(g_pdev, &g_memp);

    uint32_t qn = 0; pvkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, NULL);
    VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; pvkGetPhysicalDeviceQueueFamilyProperties(g_pdev, &qn, qf);
    for (uint32_t i = 0; i < qn; i++)
        if ((qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT)) { g_qfam = i; g_qsparse = (qf[i].queueFlags & VK_QUEUE_SPARSE_BINDING_BIT) != 0; break; }
    if (g_qfam == UINT32_MAX) { nvlog("vk: no graphics+compute queue family"); return -1; }
    g_timestamp_bits = qf[g_qfam].timestampValidBits;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, g_qfam, 1, &prio };

    static const char *kWant[20] = { "VK_EXT_extended_dynamic_state3", "VK_EXT_depth_clip_enable", "VK_EXT_memory_budget",
                                    "VK_KHR_acceleration_structure", "VK_KHR_ray_query", "VK_KHR_deferred_host_operations",
                                    "VK_EXT_external_memory_host",
                                    "VK_EXT_image_drm_format_modifier",
                                    "VK_KHR_external_memory_fd",
                                    "VK_EXT_mutable_descriptor_type", "VK_KHR_cooperative_matrix", "VK_EXT_calibrated_timestamps", "VK_EXT_sample_locations", "VK_KHR_pipeline_executable_properties",
                                    "VK_EXT_provoking_vertex", "VK_KHR_vertex_attribute_divisor", "VK_AMD_buffer_marker",
                                    "VK_EXT_robustness2", "VK_KHR_robustness2",
                                    "VK_KHR_device_address_commands" /* glcpu4 NVMTL_VB_ADDR: enabled only with that piece on */ };
    const char *exts[24]; uint32_t next = 0; int have[20] = {0};
    uint32_t na = 0;
    if (pvkEnumerateDeviceExtensionProperties &&
        pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &na, NULL) == VK_SUCCESS && na) {
        VkExtensionProperties *props = calloc(na, sizeof *props);
        if (props && pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &na, props) == VK_SUCCESS)
            for (uint32_t i = 0; i < na; i++)
                for (int w = 0; w < 20; w++)
                    if (!have[w] && !strcmp(props[i].extensionName, kWant[w])) { have[w] = 1; exts[next++] = kWant[w]; }
        free(props);
    }
    /* Single-sample rendering uses its fixed center; only MSAA needs this extension. */
    if (have[12]) {
        PFN_vkGetPhysicalDeviceProperties2 props2 = (PFN_vkGetPhysicalDeviceProperties2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2");
        g_sample_props = (VkPhysicalDeviceSampleLocationsPropertiesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLE_LOCATIONS_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &g_sample_props };
        if (props2) props2(g_pdev, &pp);
        g_sample_locations = props2 && g_lim.maxPushConstantsSize >= NVMTL_SAMPLE_POSITION_OFFSET + NVMTL_SAMPLE_POSITION_BYTES && g_sample_props.variableSampleLocations &&
            g_sample_props.maxSampleLocationGridSize.width >= 1 && g_sample_props.maxSampleLocationGridSize.height >= 1 &&
            (g_sample_props.sampleLocationSampleCounts & (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT)) == (VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_8_BIT) && g_sample_props.sampleLocationSubPixelBits == 4 &&
            g_sample_props.sampleLocationCoordinateRange[0] <= 0 && g_sample_props.sampleLocationCoordinateRange[1] >= 0.9375f;
    }
    {
        /* only asked, not enabled: the translator emits BaryCoordKHR and the claim must match what the device has */
        int bext = 0; uint32_t nb = 0;
        if (pvkEnumerateDeviceExtensionProperties && pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &nb, NULL) == VK_SUCCESS && nb) {
            VkExtensionProperties *bp = calloc(nb, sizeof *bp);
            if (bp && pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &nb, bp) == VK_SUCCESS)
                for (uint32_t i = 0; i < nb; i++)
                    if (!strcmp(bp[i].extensionName, "VK_KHR_fragment_shader_barycentric")) bext = 1;
            free(bp);
        }
        if (bext) {
            PFN_vkGetPhysicalDeviceFeatures2 gbf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
            VkPhysicalDeviceFragmentShaderBarycentricFeaturesKHR bf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_SHADER_BARYCENTRIC_FEATURES_KHR };
            VkPhysicalDeviceFeatures2 bf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &bf };
            if (gbf) gbf(g_pdev, &bf2);
            g_bary = bf.fragmentShaderBarycentric == VK_TRUE;
        }
        nvlog("vk: fragment shader barycentrics %s", g_bary ? "present - supportsShaderBarycentricCoordinates YES" : "ABSENT - not claimed");
    }
    g_dyn3 = have[0]; g_clip = have[0] && have[1]; g_budget_ext = have[2]; g_rt = have[3] && have[4] && have[5];
    g_host_import = have[6];
    g_marker_ext = have[16];
    /* Null descriptors (VK_EXT/KHR_robustness2 nullDescriptor only, not robust access): an unbound buffer, texture,
       texel buffer or storage texture slot is written as a null descriptor, which reads zero (writes are dropped),
       instead of the zero-filled 1x1 dummy image / 256 B dummy buffer. NVMTL_NULL_DESC=0: the dummies, as before. */
    static VkPhysicalDeviceRobustness2FeaturesEXT r2f;
    if ((have[17] || have[18]) && !(getenv("NVMTL_NULL_DESC") && getenv("NVMTL_NULL_DESC")[0] == '0')) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceRobustness2FeaturesEXT q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT };
        VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
        if (gpf) gpf(g_pdev, &qf2);
        g_null_desc = q.nullDescriptor == VK_TRUE;
        if (!g_null_desc) {   /* not used after all: leave the extension out */
            for (uint32_t k = 0; k < next; k++) if (exts[k] == kWant[17] || exts[k] == kWant[18]) { exts[k] = exts[--next]; k--; }
        } else if (have[17] && have[18]) {   /* one name is enough */
            for (uint32_t k = 0; k < next; k++) if (exts[k] == kWant[18]) { exts[k] = exts[--next]; break; }
        }
    } else {
        for (uint32_t k = 0; k < next; k++) if (exts[k] == kWant[17] || exts[k] == kWant[18]) { exts[k] = exts[--next]; k--; }
    }
    /* glcpu4 NVMTL_VB_ADDR: VK_KHR_device_address_commands for vkCmdBindVertexBuffers3KHR (its deviceAddressCommands feature
       is enabled below); with the piece off the device is created exactly as prod7 created it */
    static VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR dacf;
    int want_dac = 0;
    if (have[19] && nvmtl_vb_addr_on()) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR };
        VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
        if (gpf) gpf(g_pdev, &qf2);
        want_dac = q.deviceAddressCommands == VK_TRUE;
    }
    if (!want_dac) for (uint32_t k = 0; k < next; k++) if (exts[k] == kWant[19]) { exts[k] = exts[--next]; break; }
    nvlog("vk: glcpu4 VK_KHR_device_address_commands %s", want_dac ? "enabled (vertex buffers bound by address)" : have[19] ? "present, not enabled" : "ABSENT");
    nvlog("vk: null descriptors %s", g_null_desc ? "ON (robustness2 nullDescriptor: unbound slots read zero, no dummy resources)" : "off (dummy image/buffer in unbound slots)");
    g_linmod = have[7];
    g_extfd = have[8];
    { const char *e = getenv("NVMTL_PSTAT"); g_pstat = have[13] && e && *e == '1'; }
    g_mutable = have[9] && !getenv("NVMTL_NO_TEXBUF");
    nvlog("vk: par3 VK_EXT_mutable_descriptor_type %s — texture buffers %s", have[9] ? "present" : "ABSENT",
          g_mutable ? "ON (mutable texture/storage bands)" : have[9] ? "OFF (NVMTL_NO_TEXBUF)" : "OFF");
    nvlog("vk: VK_EXT_image_drm_format_modifier %s - a buffer texture's row pitch is %s", have[7] ? "present" : "ABSENT",
          have[7] ? "the app's own bytesPerRow (explicit DRM linear layout)" : "what LINEAR tiling picks; any other bytesPerRow is refused");
    nvlog("vk: VK_EXT_memory_budget %s — VRAM budget is %s", have[2] ? "present" : "ABSENT", have[2] ? "MEASURED" : "estimated at 70 %% of the card");
    nvlog("vk: %s %s · %s %s", kWant[0], have[0] ? "present" : "ABSENT", kWant[1], have[1] ? "present" : "ABSENT");
    VkPhysicalDeviceRayQueryFeaturesKHR rqf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR, NULL, VK_TRUE };
    VkPhysicalDeviceAccelerationStructureFeaturesKHR asf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rqf,
                                                            VK_TRUE, VK_FALSE, VK_FALSE, VK_FALSE, VK_TRUE };
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, g_rt ? (void *)&asf : NULL };
    v12.bufferDeviceAddress = VK_TRUE;
    VkPhysicalDeviceVulkan11Features v11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &v12 };
    if (g_null_desc) { r2f = (VkPhysicalDeviceRobustness2FeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, v12.pNext, VK_FALSE, VK_FALSE, VK_TRUE }; v12.pNext = &r2f; }
    VkPhysicalDeviceCooperativeMatrixFeaturesKHR cmf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
    if (want_dac) { dacf = (VkPhysicalDeviceDeviceAddressCommandsFeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_ADDRESS_COMMANDS_FEATURES_KHR, v12.pNext, VK_TRUE }; v12.pNext = &dacf; }
    static VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT mdf;
    if (g_mutable) { mdf = (VkPhysicalDeviceMutableDescriptorTypeFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MUTABLE_DESCRIPTOR_TYPE_FEATURES_EXT, v12.pNext, VK_TRUE }; v12.pNext = &mdf; }
    static VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf;
    g_dbuf = nvmtl_db_probe(&dbf);
    if (g_dbuf) { dbf.pNext = v12.pNext; v12.pNext = &dbf; exts[next++] = VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME; }
    static VkPhysicalDeviceSubgroupSizeControlFeatures sscf;
    { const char *e = getenv("NVMTL_CS_SIMD"); const uint32_t want = e ? (uint32_t)atoi(e) : 0;
      if (want == 16 || want == 32) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        PFN_vkGetPhysicalDeviceProperties2 gpp = (PFN_vkGetPhysicalDeviceProperties2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceSubgroupSizeControlFeatures qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES };
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qf };
        VkPhysicalDeviceSubgroupSizeControlProperties qp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_PROPERTIES };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &qp };
        if (gpf && gpp) { gpf(g_pdev, &f2); gpp(g_pdev, &p2); }
        if (qf.subgroupSizeControl && (qp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) && want >= qp.minSubgroupSize && want <= qp.maxSubgroupSize) {
            g_cs_simd = want; sscf = (VkPhysicalDeviceSubgroupSizeControlFeatures){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_SIZE_CONTROL_FEATURES, v12.pNext, VK_TRUE, VK_FALSE };
            v12.pNext = &sscf;
        }
        nvlog("vk: compute SIMD width %s (NVMTL_CS_SIMD=%s; device subgroup sizes %u..%u, compute stage %s, at most %u subgroups per workgroup)",
              g_cs_simd ? "FORCED" : "NOT forced - unsupported here", e, qp.minSubgroupSize, qp.maxSubgroupSize,
              (qp.requiredSubgroupSizeStages & VK_SHADER_STAGE_COMPUTE_BIT) ? "yes" : "no", qp.maxComputeWorkgroupSubgroups);
      } }
    /* compute2: synchronization2 for the narrow compute->compute barrier (VK_ACCESS_2_SHADER_STORAGE_READ/WRITE have no
       synchronization-1 equivalent: VK_ACCESS_SHADER_READ also invalidates ANV's texture and constant caches) */
    static VkPhysicalDeviceSynchronization2Features s2f;
    { PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
      VkPhysicalDeviceSynchronization2Features q = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES };
      VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q };
      if (gpf) gpf(g_pdev, &qf2);
      if (q.synchronization2) { s2f = (VkPhysicalDeviceSynchronization2Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES, v12.pNext, VK_TRUE };
                                v12.pNext = &s2f; g_sync2 = 1; } }
    static VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef;
    if (g_pstat) { pef = (VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, v12.pNext, VK_TRUE }; v12.pNext = &pef; }
    VkPhysicalDeviceDepthClipEnableFeaturesEXT clipf = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, NULL, VK_TRUE };
    if (g_clip) clipf.pNext = &v11;
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT dyn3 = {
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, g_clip ? (void *)&clipf : (void *)&v11 };
    dyn3.extendedDynamicState3PolygonMode = VK_TRUE;
    static VkPhysicalDeviceProvokingVertexFeaturesEXT pvf;
    if (g_dyn3 && have[14]) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceProvokingVertexFeaturesEXT qpv = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT };
        VkPhysicalDeviceExtendedDynamicState3FeaturesEXT q3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, &qpv };
        VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q3 };
        if (gpf) gpf(g_pdev, &qf2);
        g_provoking = qpv.provokingVertexLast && q3.extendedDynamicState3ProvokingVertexMode;
        if (g_provoking) {
            dyn3.extendedDynamicState3ProvokingVertexMode = VK_TRUE;
            pvf = (VkPhysicalDeviceProvokingVertexFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, dyn3.pNext, VK_TRUE, VK_FALSE };
            dyn3.pNext = &pvf;
        }
    }
    if (g_clip) dyn3.extendedDynamicState3DepthClipEnable = VK_TRUE;
    /* MTLVertexStepFunctionPerInstance with a stepRate other than 1 is a binding divisor; Metal's base instance is
       Vulkan's firstInstance, so non-zero first instances must work with it too. */
    static VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR divf;
    if (have[15]) {
        PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        PFN_vkGetPhysicalDeviceProperties2 gpp = (PFN_vkGetPhysicalDeviceProperties2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR qd = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR };
        VkPhysicalDeviceFeatures2 qf2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qd };
        VkPhysicalDeviceVertexAttributeDivisorPropertiesKHR dp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_PROPERTIES_KHR };
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dp };
        if (gpf && gpp) { gpf(g_pdev, &qf2); gpp(g_pdev, &pp); }
        g_divisor = qd.vertexAttributeInstanceRateDivisor && dp.supportsNonZeroFirstInstance;
        g_max_divisor = dp.maxVertexAttribDivisor;
        if (g_divisor) {
            divf = (VkPhysicalDeviceVertexAttributeDivisorFeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_ATTRIBUTE_DIVISOR_FEATURES_KHR,
                                                                        v12.pNext, VK_TRUE, VK_FALSE };
            v12.pNext = &divf;
        }
    }
    nvlog("vk: vertex step rates %s", g_divisor ? "ON (binding divisors)" : "OFF - per-instance data steps once per instance");
    {
        PFN_vkGetPhysicalDeviceFeatures2 gpf2 = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
        VkPhysicalDeviceVulkan12Features q12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
        VkPhysicalDeviceCooperativeMatrixFeaturesKHR qcm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_FEATURES_KHR };
        VkPhysicalDeviceVulkan11Features q11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, have[10] ? (void *)&qcm : NULL };
        q12.pNext = &q11;
        VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &q12 };
        if (gpf2) gpf2(g_pdev, &f2);
        v12.shaderFloat16 = q12.shaderFloat16;
        v12.shaderInt8 = q12.shaderInt8;
        v12.vulkanMemoryModel = q12.vulkanMemoryModel;
        v12.vulkanMemoryModelDeviceScope = q12.vulkanMemoryModelDeviceScope;
        v11.storageBuffer16BitAccess = q11.storageBuffer16BitAccess;
        v11.uniformAndStorageBuffer16BitAccess = q11.uniformAndStorageBuffer16BitAccess;
        v11.shaderDrawParameters = q11.shaderDrawParameters;  /* [[base_vertex]] / [[base_instance]] (Blender 4.2) */
        g_float16 = q12.shaderFloat16 == VK_TRUE;
        g_vulkan_memory_model = q12.vulkanMemoryModel == VK_TRUE;
        g_vulkan_memory_device_scope = q12.vulkanMemoryModelDeviceScope == VK_TRUE;
        g_storage16 = q11.storageBuffer16BitAccess == VK_TRUE;
        g_uniform_storage16 = q11.uniformAndStorageBuffer16BitAccess == VK_TRUE;
        unsigned tile_mask = 0;
        VkPhysicalDeviceCooperativeMatrixPropertiesKHR cp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_COOPERATIVE_MATRIX_PROPERTIES_KHR };
        if (have[10] && pvkGetPhysicalDeviceProperties2) {
            VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &cp };
            pvkGetPhysicalDeviceProperties2(g_pdev, &p2);
            PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR get_tiles =
                (PFN_vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceCooperativeMatrixPropertiesKHR");
            uint32_t count = 0;
            if (get_tiles && get_tiles(g_pdev, &count, NULL) == VK_SUCCESS && count) {
                VkCooperativeMatrixPropertiesKHR *tiles = calloc(count, sizeof *tiles);
                if (tiles) {
                    for (uint32_t i = 0; i < count; ++i) tiles[i].sType = VK_STRUCTURE_TYPE_COOPERATIVE_MATRIX_PROPERTIES_KHR;
                    if (get_tiles(g_pdev, &count, tiles) == VK_SUCCESS)
                        for (uint32_t i = 0; i < count; ++i) {
                            const VkCooperativeMatrixPropertiesKHR *t = &tiles[i];
                            if (t->MSize != 16 || t->NSize != 8 || (t->KSize != 8 && t->KSize != 16) ||
                                t->AType != VK_COMPONENT_TYPE_FLOAT16_KHR || t->BType != VK_COMPONENT_TYPE_FLOAT16_KHR ||
                                t->CType != t->ResultType || (t->CType != VK_COMPONENT_TYPE_FLOAT16_KHR && t->CType != VK_COMPONENT_TYPE_FLOAT32_KHR) ||
                                t->scope != VK_SCOPE_SUBGROUP_KHR || t->saturatingAccumulation) continue;
                            unsigned bit = (t->KSize == 16 ? 2u : 0u) + (t->CType == VK_COMPONENT_TYPE_FLOAT32_KHR ? 1u : 0u);
                            tile_mask |= 1u << bit;
                        }
                    free(tiles);
                }
            }
        }
        g_coop_matrix = have[10] && qcm.cooperativeMatrix && g_float16 && g_vulkan_memory_model &&
                        (cp.cooperativeMatrixSupportedStages & VK_SHADER_STAGE_COMPUTE_BIT) && tile_mask == 15;
        if (g_coop_matrix) { cmf.cooperativeMatrix = VK_TRUE; cmf.pNext = v12.pNext; v12.pNext = &cmf; }
        nvlog("vk: cooperative matrix %d, half arithmetic %d, storage16 %d/%d, Vulkan memory model %d/%d, tiles 0x%x",
              g_coop_matrix, g_float16, g_storage16, g_uniform_storage16, g_vulkan_memory_model, g_vulkan_memory_device_scope, tile_mask);
        v12.shaderBufferInt64Atomics = q12.shaderBufferInt64Atomics;
        v12.shaderSharedInt64Atomics = q12.shaderSharedInt64Atomics;
        g_atomic64_buffer = q12.shaderBufferInt64Atomics == VK_TRUE;
        g_atomic64_shared = q12.shaderSharedInt64Atomics == VK_TRUE;
        nvlog("vk: integer atomic64 buffer %d, shared %d", g_atomic64_buffer, g_atomic64_shared);
        v12.hostQueryReset = q12.hostQueryReset;
        g_host_query_reset = q12.hostQueryReset == VK_TRUE;
        v12.timelineSemaphore = q12.timelineSemaphore; g_timeline = q12.timelineSemaphore == VK_TRUE;   /* implicit IOSurface sync imports */
        v12.samplerMirrorClampToEdge = q12.samplerMirrorClampToEdge;
        g_sampler_mirror_clamp = q12.samplerMirrorClampToEdge == VK_TRUE;
        v12.scalarBlockLayout = q12.scalarBlockLayout; g_scalar_layout = q12.scalarBlockLayout == VK_TRUE;
        v12.shaderOutputLayer = q12.shaderOutputLayer; v12.shaderOutputViewportIndex = q12.shaderOutputViewportIndex;
        nvlog("vk: par1 shaderOutputLayer %d, shaderOutputViewportIndex %d", q12.shaderOutputLayer, q12.shaderOutputViewportIndex);
        const char *off = getenv("NVMTL_NO_BINDLESS");
        g_bindless = gpf2 && !(off && *off == '1') && q12.descriptorIndexing && q12.runtimeDescriptorArray &&
                     q12.shaderSampledImageArrayNonUniformIndexing && q12.descriptorBindingPartiallyBound &&
                     q12.descriptorBindingSampledImageUpdateAfterBind && q12.descriptorBindingUpdateUnusedWhilePending &&
                     q12.shaderStorageImageArrayNonUniformIndexing &&
                     q12.shaderUniformTexelBufferArrayNonUniformIndexing && q12.shaderStorageTexelBufferArrayNonUniformIndexing &&
                     q12.descriptorBindingStorageImageUpdateAfterBind &&
                     q12.descriptorBindingUniformTexelBufferUpdateAfterBind && q12.descriptorBindingStorageTexelBufferUpdateAfterBind;
        if (g_bindless) {
            v12.descriptorIndexing = v12.runtimeDescriptorArray = v12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
            v12.descriptorBindingPartiallyBound = v12.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
            v12.descriptorBindingUpdateUnusedWhilePending = VK_TRUE;
            // The heap has five bands: sampled images, samplers, storage images, uniform texel buffers and storage texel
            // buffers. Every band is update-after-bind, and translated storage-image indexes are nonuniform. A layout
            // whose storage bindings carry UPDATE_AFTER_BIND without these device features is invalid, so the five-binding
            // heap was refused and the two-binding heap took its place.
            v12.shaderStorageImageArrayNonUniformIndexing = VK_TRUE;
            v12.shaderUniformTexelBufferArrayNonUniformIndexing = VK_TRUE;
            v12.shaderStorageTexelBufferArrayNonUniformIndexing = VK_TRUE;
            v12.descriptorBindingStorageImageUpdateAfterBind = VK_TRUE;
            v12.descriptorBindingUniformTexelBufferUpdateAfterBind = VK_TRUE;
            v12.descriptorBindingStorageTexelBufferUpdateAfterBind = VK_TRUE;
            nvlog("vk: bindless storage images and texel buffers: nonuniform indexing and update-after-bind enabled");
        }
        nvlog("vk: descriptor indexing %s - bindless textures %s", g_bindless ? "present" : (off && *off == '1') ? "DISABLED by NVMTL_NO_BINDLESS" : "ABSENT",
              g_bindless ? "ON (set 2)" : "OFF");
    }
    VkPhysicalDeviceFeatures supported; pvkGetPhysicalDeviceFeatures(g_pdev, &supported);
    if (!supported.shaderInt64) { nvlog("vk: 64-bit shader addresses unavailable"); return -1; }
    VkPhysicalDeviceFeatures enabled = {0}; enabled.shaderInt64 = VK_TRUE;
    enabled.sampleRateShading = supported.sampleRateShading;
    g_sample_rate_shading = supported.sampleRateShading == VK_TRUE;   /* nm29: -supportsPullModelInterpolation */
    enabled.occlusionQueryPrecise = supported.occlusionQueryPrecise;
    g_occlusion_precise = supported.occlusionQueryPrecise == VK_TRUE;
    enabled.samplerAnisotropy = supported.samplerAnisotropy;
    enabled.imageCubeArray = supported.imageCubeArray;   // texturecube_array storage writes / sampled cube arrays (ImageCubeArray, SampledCubeArray)
    // Metal apps read past buffer ends and Apple GPUs tolerate it; on NVK an unchecked load off the end of a
    // mapping is an MMU FAULT_PTE that kills the channel (WoW at the login screen). Bounds-checked buffer and
    // vertex-fetch access turns those reads into zeros instead.
    enabled.robustBufferAccess = supported.robustBufferAccess;
    nvlog("vk: robustBufferAccess %s", supported.robustBufferAccess ? "ON (out-of-range buffer reads return zeros)" : "ABSENT - out-of-range buffer reads can fault the channel");
    g_sampler_anisotropy = supported.samplerAnisotropy == VK_TRUE;
    nvlog("vk: sampler anisotropy %d (max %g), mirror-clamp-to-edge %d", g_sampler_anisotropy,
          (double)g_lim.maxSamplerAnisotropy, g_sampler_mirror_clamp);
    enabled.tessellationShader = supported.tessellationShader; g_tess = supported.tessellationShader == VK_TRUE;
    enabled.vertexPipelineStoresAndAtomics = supported.vertexPipelineStoresAndAtomics;
    enabled.independentBlend = supported.independentBlend; g_indep = supported.independentBlend == VK_TRUE;
    enabled.alphaToOne = supported.alphaToOne; g_a2one = supported.alphaToOne == VK_TRUE;
    enabled.depthBounds = supported.depthBounds; g_depth_bounds = supported.depthBounds == VK_TRUE;
    nvlog("vk: depthBounds %s", g_depth_bounds ? "AVAILABLE" : "ABSENT - setDepthTestMinBound:maxBound: is refused");
    enabled.dualSrcBlend = supported.dualSrcBlend; g_dual = supported.dualSrcBlend == VK_TRUE;
    enabled.wideLines = supported.wideLines; g_wide = supported.wideLines == VK_TRUE;
    enabled.multiDrawIndirect = supported.multiDrawIndirect; g_multidraw = supported.multiDrawIndirect == VK_TRUE;
    enabled.drawIndirectFirstInstance = supported.drawIndirectFirstInstance;   // an ICB record's base instance (Metal allows any)
    nvlog("vk: sel1 wideLines %s (range %g..%g)", g_wide ? "AVAILABLE" : "ABSENT", (double)g_lim.lineWidthRange[0], (double)g_lim.lineWidthRange[1]);
    nvlog("vk: par2 dualSrcBlend %s", g_dual ? "AVAILABLE" : "ABSENT");
    if (pnvk_sparse && pnvk_sparse() == 1 && supported.sparseBinding) { enabled.sparseBinding = VK_TRUE; g_sparse = 1; }
    nvlog("vk: alphaToCoverage always, alphaToOne %s", g_a2one ? "AVAILABLE" : "ABSENT");
    nvlog("vk: tessellation %s, vertex-stage stores %s", g_tess ? "AVAILABLE" : "ABSENT", supported.vertexPipelineStoresAndAtomics ? "on" : "off");
    nvmtl_vk_icb_dgc_probe(exts, &next, sizeof exts / sizeof exts[0], &v12.pNext);   /* NVMTL_ICB_DGC=1 only (nvmtl_icb_dgc.c) */
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, g_dyn3 ? (const void *)&dyn3 : (const void *)&v11, 0,
                               1, &qci, 0, NULL, next, next ? exts : NULL, &enabled };
    VKCK(pvkCreateDevice(g_pdev, &dci, NULL, &g_dev), "vkCreateDevice");
    DEVICE_FNS(LOAD_D)
    if (g_dbuf && nvmtl_db_device_ready(enabled.robustBufferAccess == VK_TRUE)) g_dbuf = 0;
    nvmtl_vk_icb_dgc_device_ready();
    nvmtl_hitch_wrap();
    nvmtl_gpt_wrap();
    if (g_sync2 && !(pvkCmdPipelineBarrier2_ = (PFN_vkCmdPipelineBarrier2)pvkGetDeviceProcAddr(g_dev, "vkCmdPipelineBarrier2"))) g_sync2 = 0;
    nvlog("vk: synchronization2 %s", g_sync2 ? "ON (compute2 narrow compute barriers possible)" : "ABSENT - compute barriers stay synchronization-1");
    if (g_pstat) { p_pstat_props = (PFN_vkGetPipelineExecutablePropertiesKHR)pvkGetDeviceProcAddr(g_dev, "vkGetPipelineExecutablePropertiesKHR");
                   p_pstat_stats = (PFN_vkGetPipelineExecutableStatisticsKHR)pvkGetDeviceProcAddr(g_dev, "vkGetPipelineExecutableStatisticsKHR");
                   if (!p_pstat_props || !p_pstat_stats) g_pstat = 0;
                   nvlog("pstat: per-pipeline compile statistics %s", g_pstat ? "ON (NVMTL_PSTAT=1)" : "unavailable"); }
    nvmtl_gen_hook_destroy();   // before the bindless hook, which then calls through it
    { extern void nvmtl_bl_hook_destroy(void); nvmtl_bl_hook_destroy(); }
    if (g_sample_locations) {
        pvkCmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT)pvkGetDeviceProcAddr(g_dev, "vkCmdSetSampleLocationsEXT");
        if (!pvkCmdSetSampleLocationsEXT) g_sample_locations = 0;
    }
    if (g_sample_loader_unwrapped) {
        pnvk_sample_depth_contract = (VkResult (*)(VkDevice, uint32_t *, uint32_t *))
            pvkGetDeviceProcAddr(g_dev, "vkNVMTLGetSampleDepthContractV1");
        pnvk_sample_depth_image = (VkResult (*)(VkDevice, VkImage, uint32_t *))
            pvkGetDeviceProcAddr(g_dev, "vkNVMTLVerifySampleDepthImageV1");
        nvlog("vk: sample-depth loader diagnostic unique_handles=false; synchronization/core validation retained");
    }

    if (g_sample_locations && (!pnvk_sample_depth_contract || !pnvk_sample_depth_image ||
        pnvk_sample_depth_contract(g_dev, &g_sample_depth_flags, &g_sample_depth_counts) != VK_SUCCESS ||
        (g_sample_depth_flags & NVMTL_SAMPLE_DEPTH_CONTRACT_REQUIRED) != NVMTL_SAMPLE_DEPTH_CONTRACT_REQUIRED ||
        (g_sample_depth_counts & 0xf) != 0xf)) g_sample_locations = 0;
    nvlog("vk: sample locations backend %d; Metal capability pending native qualification", g_sample_locations);
    if (have[11] && g_timestamp_bits && g_timestamp_bits <= 64 &&
        isfinite(g_lim.timestampPeriod) && g_lim.timestampPeriod > 0) {
        PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT domains =
            (PFN_vkGetPhysicalDeviceCalibrateableTimeDomainsEXT)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceCalibrateableTimeDomainsEXT");
        uint32_t n = 0;
        if (domains && domains(g_pdev, &n, NULL) == VK_SUCCESS && n && n <= 16) {
            VkTimeDomainEXT list[16];
            if (domains(g_pdev, &n, list) == VK_SUCCESS) {
                for (uint32_t i = 0; i < n; ++i) if (list[i] == VK_TIME_DOMAIN_DEVICE_EXT)
                    pvkGetCalibratedTimestampsEXT = (PFN_vkGetCalibratedTimestampsEXT)
                        pvkGetDeviceProcAddr(g_dev, "vkGetCalibratedTimestampsEXT");
            }
        }
    }
    if (g_marker_ext) pvkCmdWriteBufferMarker2AMD = (PFN_vkCmdWriteBufferMarker2AMD)pvkGetDeviceProcAddr(g_dev, "vkCmdWriteBufferMarker2AMD");
    nvlog("vk: completion witness %s", pvkCmdWriteBufferMarker2AMD ? "written with vkCmdWriteBufferMarker2AMD after all commands (no barriers)"
                                                                  : "written with vkCmdFillBuffer between two barriers (VK_AMD_buffer_marker absent)");
    pvkCmdSetLineWidth = (PFN_vkCmdSetLineWidth)pvkGetDeviceProcAddr(g_dev, "vkCmdSetLineWidth");
    if (g_wide && !pvkCmdSetLineWidth) { nvlog("vk: sel1 vkCmdSetLineWidth missing - wide lines OFF"); g_wide = 0; }
    if (g_rt) {
#define LOAD_RT(f) p##f = (PFN_##f)pvkGetDeviceProcAddr(g_dev, #f);
        LOAD_RT(vkCreateAccelerationStructureKHR) LOAD_RT(vkDestroyAccelerationStructureKHR) LOAD_RT(vkGetAccelerationStructureBuildSizesKHR)
        LOAD_RT(vkCmdBuildAccelerationStructuresKHR) LOAD_RT(vkGetAccelerationStructureDeviceAddressKHR) LOAD_RT(vkCmdCopyAccelerationStructureKHR)
        LOAD_RT(vkCmdWriteAccelerationStructuresPropertiesKHR)
        if (!pvkCreateAccelerationStructureKHR || !pvkGetAccelerationStructureBuildSizesKHR || !pvkCmdBuildAccelerationStructuresKHR ||
            !pvkGetAccelerationStructureDeviceAddressKHR) { nvlog("vk: ray tracing entry points missing — ray tracing OFF"); g_rt = 0; }
    }
    if (g_rt) {
        PFN_vkGetPhysicalDeviceProperties2 gp2 = (PFN_vkGetPhysicalDeviceProperties2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceProperties2");
        VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &g_asprops };
        g_asprops.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_PROPERTIES_KHR;
        if (gp2) gp2(g_pdev, &pp);
        nvlog("vk: acceleration structure limits: %llu geometries, %llu instances, %llu primitives per structure",
              (unsigned long long)g_asprops.maxGeometryCount, (unsigned long long)g_asprops.maxInstanceCount, (unsigned long long)g_asprops.maxPrimitiveCount);
    }
    nvlog("vk: ray tracing %s", g_rt ? "AVAILABLE (acceleration structures + ray queries)" : "ABSENT on this NVK");
    if (g_host_import) {
        pvkGetMemoryHostPointerPropertiesEXT = (PFN_vkGetMemoryHostPointerPropertiesEXT)
            pvkGetDeviceProcAddr(g_dev, "vkGetMemoryHostPointerPropertiesEXT");
        VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT, NULL, 0 };
        VkPhysicalDeviceProperties2 hp2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &hp };
        pvkGetPhysicalDeviceProperties2(g_pdev, &hp2);
        g_host_align = (size_t)hp.minImportedHostPointerAlignment;
        if (!pvkGetMemoryHostPointerPropertiesEXT || !g_host_align || (g_host_align & (g_host_align - 1))) {
            nvlog("vk: host-pointer import OFF — entry point %s, alignment %zu (must be a power of two)",
                  pvkGetMemoryHostPointerPropertiesEXT ? "present" : "MISSING", g_host_align);
            g_host_import = 0; g_host_align = 0;
        }
    }
    nvlog("vk: newBufferWithBytesNoCopy: %s", g_host_import
          ? "imports the caller's pages (VK_EXT_external_memory_host)" : "must COPY — no host-pointer import on this device");
    if (g_dyn3) {
        VkPhysicalDeviceExtendedDynamicState3PropertiesEXT p3 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_PROPERTIES_EXT };
        VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &p3 };
        pvkGetPhysicalDeviceProperties2(g_pdev, &p2); g_topo_any = p3.dynamicPrimitiveTopologyUnrestricted ? 1 : 0;
    }
    nvlog("vk: G14 primitive topology is dynamic; points/lines on a triangle-class pipeline: %s", g_topo_any ? "yes" : "NO");
    if (g_dyn3) {
        pvkCmdSetPolygonModeEXT = (PFN_vkCmdSetPolygonModeEXT)pvkGetDeviceProcAddr(g_dev, "vkCmdSetPolygonModeEXT");
        if (!pvkCmdSetPolygonModeEXT) { nvlog("vk: the extension is enabled but vkCmdSetPolygonModeEXT is missing — fill mode stays static"); g_dyn3 = 0; }
    }
    if (g_provoking) {
        pvkCmdSetProvokingVertexModeEXT = (PFN_vkCmdSetProvokingVertexModeEXT)pvkGetDeviceProcAddr(g_dev, "vkCmdSetProvokingVertexModeEXT");
        if (!pvkCmdSetProvokingVertexModeEXT) g_provoking = 0;
    }
    nvlog("vk: provoking vertex %s", g_provoking ? "dynamic, first or last (Apple GL's setProvokingVertexMode:)" : "first only");
    if (g_clip) {
        pvkCmdSetDepthClipEnableEXT = (PFN_vkCmdSetDepthClipEnableEXT)pvkGetDeviceProcAddr(g_dev, "vkCmdSetDepthClipEnableEXT");
        if (!pvkCmdSetDepthClipEnableEXT) { nvlog("vk: vkCmdSetDepthClipEnableEXT is missing — depth clip mode stays static"); g_clip = 0; }
    }
    pvkCmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)pvkGetDeviceProcAddr(g_dev, "vkCmdBindVertexBuffers2");
    if (want_dac) pvkCmdBindVertexBuffers3KHR = (PFN_vkCmdBindVertexBuffers3KHR)pvkGetDeviceProcAddr(g_dev, "vkCmdBindVertexBuffers3KHR");
    if (want_dac) nvlog("vk: glcpu4 vkCmdBindVertexBuffers3KHR %s", pvkCmdBindVertexBuffers3KHR ? "loaded - vertex buffers are bound by address" : "MISSING - bound by handle");
    pvkCreateRenderPass2 = (PFN_vkCreateRenderPass2)pvkGetDeviceProcAddr(g_dev, "vkCreateRenderPass2");
    pvkCreateBufferView = (PFN_vkCreateBufferView)pvkGetDeviceProcAddr(g_dev, "vkCreateBufferView");
    pvkDestroyBufferView = (PFN_vkDestroyBufferView)pvkGetDeviceProcAddr(g_dev, "vkDestroyBufferView");
    { VkPhysicalDeviceDepthStencilResolveProperties dsp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES };
      VkPhysicalDeviceProperties2 pp2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &dsp };
      pvkGetPhysicalDeviceProperties2(g_pdev, &pp2);
      g_dsr_depth_modes = dsp.supportedDepthResolveModes; g_dsr_stencil_modes = dsp.supportedStencilResolveModes;
      g_dsr_indep = dsp.independentResolve; g_dsr_indep_none = dsp.independentResolveNone;
      nvlog("vk: par1 vkCreateRenderPass2 %s; depth resolve modes 0x%x, stencil 0x%x, independent %u/%u", pvkCreateRenderPass2 ? "loaded" : "ABSENT",
            g_dsr_depth_modes, g_dsr_stencil_modes, g_dsr_indep, g_dsr_indep_none); }
    nvlog("vk: apps-dyn dynamic vertex strides (vkCmdBindVertexBuffers2): %s", pvkCmdBindVertexBuffers2 ? "yes" : "NO — MTLBufferLayoutStrideDynamic pipelines are refused");
    pvkGetDeviceQueue(g_dev, g_qfam, 0, &g_queue);

    atomic_store(&g_up_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW));
    g_state = 1;
    nvlog("vk: GPU reset recovery %s", !nvmtl_recover_on() ? "OFF (NVMTL_GPU_RECOVERY=0) - a lost device disables the backend"
          : panv_recover ? "ON - a banned queue is replaced on the same device (nvmtl_anv_device_recover_v1)"
          : "unavailable - the Vulkan driver has no nvmtl_anv_device_recover_v1; a lost device disables the backend");
    nvlog("vk: UP in this Metal process — \"%s\" api %u.%u.%u, queue family %u", g_name,
          VK_VERSION_MAJOR(pp.apiVersion), VK_VERSION_MINOR(pp.apiVersion), VK_VERSION_PATCH(pp.apiVersion), g_qfam);
    return 0;
}
static pthread_once_t g_vk_once = PTHREAD_ONCE_INIT;
static void nvmtl_vk_initialize_once(void) { (void)nvmtl_vk_initialize(); }
/* 1 once the backend is down in this process: a failed submit or fence wait disabled it, or it never started. */
int nvmtl_vk_backend_lost(void) { return atomic_load(&g_state) == -1; }   /* (upstream: nvmtl_vk_lost; that name is our recovery path's) */
/* The device was up and then lost, as opposed to never starting. After a loss the Vulkan queue is dead for the rest of the
   process, but the caller still gets command buffers: they record nothing and complete with a "device removed" error,
   as Metal reports a removed GPU, instead of nil (WindowServer asserts on nil and the session goes). */
static char g_dead_cb_token;
static void nvmtl_noop_cmd(void) {}
/* Recording calls made on a dead command buffer, and on one that was being recorded when the loss was noticed, do
   nothing: a void function taking any arguments is the same machine code as this stub on x86-64. */
static void nvmtl_vk_stub_recording(void)
{
#define STUB_CMD(f) if (!strncmp(#f, "vkCmd", 5)) p##f = (PFN_##f)nvmtl_noop_cmd;
    DEVICE_FNS(STUB_CMD)
#undef STUB_CMD
    if (pvkCmdSetLineWidth) pvkCmdSetLineWidth = (PFN_vkCmdSetLineWidth)nvmtl_noop_cmd;
    if (pvkCmdBindVertexBuffers2) pvkCmdBindVertexBuffers2 = (PFN_vkCmdBindVertexBuffers2)nvmtl_noop_cmd;
    if (pvkCmdSetSampleLocationsEXT) pvkCmdSetSampleLocationsEXT = (PFN_vkCmdSetSampleLocationsEXT)nvmtl_noop_cmd;
    if (pvkCmdSetPolygonModeEXT) pvkCmdSetPolygonModeEXT = (PFN_vkCmdSetPolygonModeEXT)nvmtl_noop_cmd;
    if (pvkCmdSetProvokingVertexModeEXT) pvkCmdSetProvokingVertexModeEXT = (PFN_vkCmdSetProvokingVertexModeEXT)nvmtl_noop_cmd;
    if (pvkCmdSetDepthClipEnableEXT) pvkCmdSetDepthClipEnableEXT = (PFN_vkCmdSetDepthClipEnableEXT)nvmtl_noop_cmd;
    if (pvkCmdBuildAccelerationStructuresKHR) pvkCmdBuildAccelerationStructuresKHR = (PFN_vkCmdBuildAccelerationStructuresKHR)nvmtl_noop_cmd;
    if (pvkCmdCopyAccelerationStructureKHR) pvkCmdCopyAccelerationStructureKHR = (PFN_vkCmdCopyAccelerationStructureKHR)nvmtl_noop_cmd;
    /* IntelXe: the command entry points this copy loads outside DEVICE_FNS */
    if (pvkCmdPipelineBarrier2_) pvkCmdPipelineBarrier2_ = (PFN_vkCmdPipelineBarrier2)nvmtl_noop_cmd;
    if (pvkCmdWriteBufferMarker2AMD) pvkCmdWriteBufferMarker2AMD = (PFN_vkCmdWriteBufferMarker2AMD)nvmtl_noop_cmd;
    if (pvkCmdBindVertexBuffers3KHR) pvkCmdBindVertexBuffers3KHR = (PFN_vkCmdBindVertexBuffers3KHR)nvmtl_noop_cmd;
    if (pvkCmdWriteAccelerationStructuresPropertiesKHR) pvkCmdWriteAccelerationStructuresPropertiesKHR = (PFN_vkCmdWriteAccelerationStructuresPropertiesKHR)nvmtl_noop_cmd;
}
/* Called once, by whichever thread first sees the device go: names the cause in the log and turns recording into no-ops. */
static void nvmtl_vk_note_loss(const char *where, int vkresult, long waited_ms)
{
    if (atomic_exchange(&g_lost_after_up, 1)) return;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), up = atomic_load(&g_up_ns);
    char cause[512];
    nvmtl_loss_cause(cause, sizeof cause, where, vkresult, waited_ms, g_last_submit_who, up ? (long)((now - up) / 1000000000ull) : -1, getprogname());
    nvlog("vk: %s", cause);
    nvlog("vk: every command buffer made in %s from now on records nothing and completes with a device-removed error; objects made before the loss stay inert", getprogname());
    nvmtl_vk_stub_recording();
}
int nvmtl_vk_init(void) {
    pthread_once(&g_vk_once, nvmtl_vk_initialize_once);
    return g_state == 1 ? 0 : -1;
}
const char *nvmtl_vk_device_name(void) { return g_state == 1 ? g_name : "(vulkan down)"; }
void nvmtl_vk_limits(uint32_t *layers, uint32_t *shared_bytes, uint32_t wg[3], uint32_t *wg_inv, uint32_t *sm) {
    int up = g_state == 1;
    if (layers) *layers = up ? g_lim.maxImageArrayLayers : 0;
    if (shared_bytes) *shared_bytes = up ? g_lim.maxComputeSharedMemorySize : 0;
    if (wg) { wg[0] = up ? g_lim.maxComputeWorkGroupSize[0] : 0; wg[1] = up ? g_lim.maxComputeWorkGroupSize[1] : 0; wg[2] = up ? g_lim.maxComputeWorkGroupSize[2] : 0; }
    if (wg_inv) *wg_inv = up ? g_lim.maxComputeWorkGroupInvocations : 0;
    if (sm) *sm = up ? g_sm_count : 0;
}

void nvmtl_vk_pubcaps(uint32_t *color_samples, uint32_t *depth_samples, uint64_t *max_storage_range, uint64_t *linear_align) {
    int up = g_state == 1;
    if (color_samples) *color_samples = up ? (uint32_t)(g_lim.framebufferColorSampleCounts & g_lim.sampledImageColorSampleCounts) : 0;
    if (depth_samples) *depth_samples = up ? (uint32_t)(g_lim.framebufferDepthSampleCounts & g_lim.sampledImageDepthSampleCounts) : 0;
    if (max_storage_range) *max_storage_range = up ? (uint64_t)g_lim.maxStorageBufferRange : 0;
    if (linear_align) {
        uint64_t a = up ? (uint64_t)g_lim.optimalBufferCopyRowPitchAlignment : 0;
        uint64_t t = up ? (uint64_t)g_lim.minTexelBufferOffsetAlignment : 0;
        *linear_align = a > t ? a : t;
    }
}

static int memtype(uint32_t bits, VkMemoryPropertyFlags want)
{
    if (want & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++)
            if ((bits & (1u << i)) && (g_memp.memoryTypes[i].propertyFlags & want) == want
                && !(g_memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return (int)i;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (g_memp.memoryTypes[i].propertyFlags & want) == want) return (int)i;
    return -1;
}

/* VRAM for a Private/Managed buffer. ANV on Xe2 lists a compressed DEVICE_LOCAL type first (flat CCS); a linear image never
   accepts it, so a buffer there could not back newTextureWithDescriptor:offset:bytesPerRow: (nil, and MPS stopped: Geekbench 7
   "Background Blur", 2026-10-28). Buffers take the first DEVICE_LOCAL type a linear image also takes - the uncompressed one -
   when there is such a type; NVMTL_BUFFER_CCS=1 keeps the compressed one (no buffer textures in VRAM then). */
static uint32_t g_linear_types;   /* memory types a linear image may use: never a compressed (Xe2 flat CCS) type */
static int nvmtl_vk_buffer_vram_type(uint32_t bits)
{
    static int ccs = -1; static uint32_t linear;
    if (ccs < 0) {
        const char *e = getenv("NVMTL_BUFFER_CCS");
        // made as nvmtl_vk_image_create_buffer_alias makes its images: the explicit DRM linear layout when there is one
        VkSubresourceLayout pl = { 0, 0, 256, 0, 0 };
        VkImageDrmFormatModifierExplicitCreateInfoEXT eci = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL, 0, 1, &pl };
        VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, g_linmod ? (void *)&eci : NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM,
            { 64, 64, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, g_linmod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
            VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_PREINITIALIZED };
        VkImage img = VK_NULL_HANDLE; uint32_t got = 0;
        if (pvkCreateImage(g_dev, &ici, NULL, &img) == VK_SUCCESS) {
            VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, img, &mr); got = mr.memoryTypeBits; pvkDestroyImage(g_dev, img, NULL); }
        linear = got; g_linear_types = got; ccs = (e && e[0] == '1') || !got;
        nvlog("vk: VRAM buffers in %s (linear images take memory types 0x%x; NVMTL_BUFFER_CCS=%s)",
              ccs ? "the first DEVICE_LOCAL type" : "a DEVICE_LOCAL type a linear texture can view", got, e ? e : "unset");
    }
    const int mt = ccs ? -1 : memtype(bits & linear, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    return mt >= 0 ? mt : memtype(bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
}
#define NVMTL_POOL_BLOCK (32u << 20)
#define NVMTL_POOL_MAX   (4u << 20)
#define NVMTL_POOL_KEEP  2
static size_t g_bar1_bytes; static int g_bar1_off;
static int bar1_on(void)
{
    static int env = -1;
    if (env < 0) { const char *e = getenv("NVMTL_SHARED_VRAM"); env = e && e[0] == '1';
        if (!env) nvlog("vk: shared-in-VRAM off - Shared stays in system RAM at every size (batch 56 default; NVMTL_SHARED_VRAM=1 opts in)"); }
    return env;
}
/* force: a small internal buffer the GPU reads from its command streamer (ICB slot records, host_visible 3) goes to VRAM
   whatever the Shared policy (bar1_on) says, within the same BAR budget */
static int bar1_memtype_for(uint32_t bits, VkDeviceSize size, int force)
{
    if (!force) {
        if (g_bar1_off) return -1;
        if (!bar1_on()) { g_bar1_off = 1; return -1; }
    }
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) {
        if (!(bits & (1u << i)) || (g_memp.memoryTypes[i].propertyFlags & want) != want) continue;
        VkDeviceSize heap = g_memp.memoryHeaps[g_memp.memoryTypes[i].heapIndex].size;
        VkDeviceSize budget = heap > (256ull << 20) ? heap - (256ull << 20) : 0;
        { static long long cap = -2; if (cap == -2) { const char *e = getenv("NVMTL_SHARED_VRAM_BUDGET_MB"); cap = (e && *e) ? atoll(e) : -1; }
          if (cap >= 0 && ((VkDeviceSize)cap << 20) < budget) budget = (VkDeviceSize)cap << 20; }
        { static int said; if (!said++) nvlog("vk: shared-in-VRAM: memory type %u flags 0x%x heap %u of %llu MB, budget %llu MB",
              i, g_memp.memoryTypes[i].propertyFlags, g_memp.memoryTypes[i].heapIndex, (unsigned long long)(heap >> 20), (unsigned long long)(budget >> 20)); }
        if (g_bar1_bytes + size > budget) { static int full; if (!full++) nvlog("vk: shared-in-VRAM budget full (%zu + %llu > %llu bytes) - system RAM from here", g_bar1_bytes, (unsigned long long)size, (unsigned long long)budget); return -1; }
        return (int)i;
    }
    { static int said; if (!said++) nvlog("vk: shared-in-VRAM: no DEVICE_LOCAL|HOST_VISIBLE|HOST_COHERENT type in bits 0x%x", bits); }
    return -1;
}
static int bar1_memtype(uint32_t bits, VkDeviceSize size) { return bar1_memtype_for(bits, size, 0); }
static int  pool_buffer_create(size_t size, nvk_buffer *out);
static int  pool_buffer_create_in(int vram, size_t size, nvk_buffer *out);
static int  vpool_on(void);
static void pool_buffer_release(nvk_buffer *b);
static int pool_buffer_create_in(int vram, size_t size, nvk_buffer *out);
static int ppool_on(void);
#define NVMTL_PPOOL_MAX   (1u << 20)   /* Private buffers below this go to the private pool (ppool_heap_create) */
/* Parked dead buffers: up to 64 of them and 768 MB, each up to 256 MB, and a create may take one up to a quarter larger
   than it asked for (1 MB and up). WAS 32 / 256 MB / 64 MB, exact size only: Geekbench 7's OpenCL run destroyed 23,662
   buffers (49 GB) at ~0.3 ms each - 7.4 s of the run - while 66 parked ones were evicted for 16 reused (2026-10-28).
   A VRAM wall still drains the parked Private ones first (bpark_drain_private). */
#define NVMTL_BUFPARK_N     64
/* Submission numbers (one Vulkan queue): each submit takes the next number under the submit lock, and its fence's wait
   marks it done. g_sub_wm: every submission up to it is done. What a submission may still read must not be handed out
   again before it is done: a parked buffer waits for every submission made before it was parked (nvmtl_bpark_ready),
   since a dead buffer's last use can be a GPU address in an argument buffer that no resource set held.
   NVMTL_REUSE_CHECK=1 logs each reuse of a descriptor set, fence or parked buffer whose last user had not finished. */
#define NVMTL_SUBRING 65536u
static uint64_t g_sub_seq;                         // the last number taken (nvmtl_submit_mutex)
static _Atomic uint64_t g_sub_wm;                  // done up to here
static uint64_t g_sub_done[NVMTL_SUBRING];         // a done number, at its slot, until the watermark passes it
static pthread_mutex_t g_sub_lock = PTHREAD_MUTEX_INITIALIZER;
static void nvmtl_bindless_drain(void);
static void nvmtl_sub_done(uint64_t s)
{
    if (!s) return;
    pthread_mutex_lock(&g_sub_lock);
    g_sub_done[s % NVMTL_SUBRING] = s;
    uint64_t wm = atomic_load(&g_sub_wm);
    while (g_sub_done[(wm + 1) % NVMTL_SUBRING] == wm + 1) wm++;
    atomic_store(&g_sub_wm, wm);
    pthread_mutex_unlock(&g_sub_lock);
    nvmtl_bindless_drain();   // svfix2: heap slots whose readers have all finished
}
static int nvmtl_sub_is_done(uint64_t s)
{
    if (!s || s <= atomic_load(&g_sub_wm)) return 1;
    pthread_mutex_lock(&g_sub_lock); const int d = g_sub_done[s % NVMTL_SUBRING] == s; pthread_mutex_unlock(&g_sub_lock);
    return d;
}
static int nvmtl_reuse_check(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_REUSE_CHECK"); on = e && e[0] == '1';
        if (on) nvlog("reuse check: ON - a descriptor set, fence or parked buffer handed out before its last user finished is logged (NVMTL_REUSE_CHECK)"); }
    return on;
}
static void nvmtl_reuse_flag(const char *what, uint64_t seq, size_t bytes)
{
    static _Atomic unsigned long long n; const unsigned long long k = atomic_fetch_add(&n, 1) + 1;
    if (k <= 32 || (k & (k - 1)) == 0)
        nvlog("reuse check: %s reused while submission #%llu that may still use it is not done (done up to #%llu, last #%llu; %zu bytes) - %llu so far",
              what, (unsigned long long)seq, (unsigned long long)atomic_load(&g_sub_wm), (unsigned long long)__atomic_load_n(&g_sub_seq, __ATOMIC_RELAXED),
              bytes, k);
}
#define NVMTL_BUFPARK_BYTES (768ull << 20)
#define NVMTL_BUFPARK_MAX1  (256ull << 20)
static pthread_mutex_t g_bpark_lock = PTHREAD_MUTEX_INITIALIZER;
static nvk_buffer g_bpark[NVMTL_BUFPARK_N];
static uint64_t g_bpark_seq[NVMTL_BUFPARK_N];   // the last submission number when it was parked
static size_t g_bpark_len[NVMTL_BUFPARK_N];   // the VkBuffer's own size, which a larger parked buffer keeps when reused smaller
static uint64_t g_bpark_ns[NVMTL_BUFPARK_N];    // when it was parked (CLOCK_UPTIME_RAW)
/* dcce9d9: parked memory unused for NVMTL_BUFPARK_AGE_MS (5000; 0 = never) is let go at the next park - system RAM behind
   it is wired. The fork's own sizes above stay (upstream went to 32 / 1 GB / 512 MB). */
static uint64_t bpark_age_ns(void)
{
    static long long ms = -1;
    if (ms < 0) { const char *e = getenv("NVMTL_BUFPARK_AGE_MS"); ms = e && *e ? atoll(e) : 5000; if (ms < 0) ms = 0; }
    return (uint64_t)ms * 1000000ull;
}
static unsigned g_bpark_n;
static uint64_t g_bpark_bytes;
static _Atomic uint64_t g_bpark_parked, g_bpark_reused, g_bpark_evicted, g_bpark_drained;
static int bpark_on(void)
{
    static int on = -1;
    if (on < 0) { on = !getenv("NVMTL_NO_BUFREUSE");
        nvlog("buffer park: %s (NVMTL_NO_BUFREUSE=%s)", on ? "a dead MTLBuffer's memory is reused by the next create of its exact size and placement"
              : "OFF - every dead buffer is freed", getenv("NVMTL_NO_BUFREUSE") ?: "unset"); }
    return on;
}
static int bpark_kind(const nvk_buffer *b) { return b->bar1 ? 2 : b->map ? 1 : 0; }
// NVMTL_BUFREUSE_EXACT=1: a parked buffer serves only a create of its exact size (fix8's up-to-25%-larger reuse off).
// NVMTL_BUFREUSE_ANYTIME=1: a parked buffer is reused without waiting for the submissions made before it was parked.
static int bpark_exact(void) { static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_BUFREUSE_EXACT"); on = e && e[0] == '1';
    if (on) nvlog("buffer park: exact sizes only (NVMTL_BUFREUSE_EXACT=1)"); } return on; }
static int bpark_anytime(void) { static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_BUFREUSE_ANYTIME"); on = e && e[0] == '1';
    nvlog("buffer park: a parked buffer is reused %s (NVMTL_BUFREUSE_ANYTIME=%s)", on ? "at once" : "once every submission made before it was parked is done", e ? e : "unset"); } return on; }
static _Atomic uint64_t g_bpark_held;
static void bpark_census(const char *what)
{
    const uint64_t n = atomic_load(&g_bpark_parked) + atomic_load(&g_bpark_reused);
    if (n & (n - 1)) return;
    nvlog("buffer park (%s): %llu parked, %llu reused, %llu evicted, %llu drained, %llu passed over until earlier submissions finished; %u held, %llu bytes", what,
          (unsigned long long)atomic_load(&g_bpark_parked), (unsigned long long)atomic_load(&g_bpark_reused),
          (unsigned long long)atomic_load(&g_bpark_evicted), (unsigned long long)atomic_load(&g_bpark_drained),
          (unsigned long long)atomic_load(&g_bpark_held), g_bpark_n, (unsigned long long)g_bpark_bytes);
}
static void bpark_free(nvk_buffer *b)
{
    pvkDestroyBuffer(g_dev, (VkBuffer)b->buf, NULL);
    if (b->map && pvkUnmapMemory) pvkUnmapMemory(g_dev, (VkDeviceMemory)b->mem);
    if (b->bar1) g_bar1_bytes -= b->alloc;
    pvkFreeMemory(g_dev, (VkDeviceMemory)b->mem, NULL); g_alloc_bytes -= b->alloc;
    memset(b, 0, sizeof *b);
}
static int bpark_take(size_t size, int host_visible, nvk_buffer *out)
{
    if (!bpark_on()) return -1;
    const int want = host_visible == 2 ? 2 : host_visible ? 1 : 0;
    int hit = -1; nvk_buffer got;
    pthread_mutex_lock(&g_bpark_lock);
    for (unsigned k = g_bpark_n; k-- > 0; ) {
        const int kind = bpark_kind(&g_bpark[k]);
        const size_t have = g_bpark_len[k];
        const int fits = have == size || (!bpark_exact() && size >= (1u << 20) && have > size && have - size <= size / 4);
        if (!fits || !(kind == want || (want == 2 && kind == 1 && g_bar1_off))) continue;
        if (g_bpark_seq[k] > atomic_load(&g_sub_wm)) {   // a submission from before its park may still use it
            if (nvmtl_reuse_check() && bpark_anytime()) nvmtl_reuse_flag("parked buffer", g_bpark_seq[k], have);
            if (!bpark_anytime()) { atomic_fetch_add(&g_bpark_held, 1); continue; }
        }
        if (hit < 0 || have < g_bpark_len[hit]) hit = (int)k; }
    if (hit >= 0) {
        got = g_bpark[hit]; g_bpark_bytes -= got.alloc; const size_t g_bpark_len_taken = g_bpark_len[hit];
        memmove(&g_bpark[hit], &g_bpark[hit + 1], (g_bpark_n - (unsigned)hit - 1) * sizeof got);
        memmove(&g_bpark_len[hit], &g_bpark_len[hit + 1], (g_bpark_n - (unsigned)hit - 1) * sizeof g_bpark_len[0]);
        memmove(&g_bpark_seq[hit], &g_bpark_seq[hit + 1], (g_bpark_n - (unsigned)hit - 1) * sizeof g_bpark_seq[0]);
        memmove(&g_bpark_ns[hit], &g_bpark_ns[hit + 1], (g_bpark_n - (unsigned)hit - 1) * sizeof g_bpark_ns[0]); g_bpark_n--;
        got.vksize = g_bpark_len_taken; got.size = size;   // what the caller asked for; the VkBuffer and its memory stay the larger ones
    }
    pthread_mutex_unlock(&g_bpark_lock);
    if (hit < 0) return -1;
    *out = got; g_buf_made++;
    atomic_fetch_add(&g_bpark_reused, 1); bpark_census("reuse");
    if (g_hitch_ms > 0) nvmtl_hitch_add_ns(NVH_REUSE, 0);
    return 0;
}
static void bpark_drain_private(void)
{
    nvk_buffer out[NVMTL_BUFPARK_N]; unsigned nout = 0;
    pthread_mutex_lock(&g_bpark_lock);
    unsigned w = 0;
    for (unsigned k = 0; k < g_bpark_n; k++) {
        if (bpark_kind(&g_bpark[k]) == 0) { g_bpark_bytes -= g_bpark[k].alloc; out[nout++] = g_bpark[k]; }
        else { g_bpark_len[w] = g_bpark_len[k]; g_bpark_seq[w] = g_bpark_seq[k]; g_bpark_ns[w] = g_bpark_ns[k]; g_bpark[w++] = g_bpark[k]; } }
    g_bpark_n = w;
    pthread_mutex_unlock(&g_bpark_lock);
    for (unsigned k = 0; k < nout; k++) bpark_free(&out[k]);
    if (nout) { atomic_fetch_add(&g_bpark_drained, nout);
        nvlog("buffer park: VRAM wall refused - drained %u parked Private buffers", nout); }
}
static int nvmtl_vk_buffer_create_body(size_t size, int host_visible, nvk_buffer *out);
static pthread_mutex_t nvmtl_submit_mutex;
static uint64_t g_res3_made, g_res3_madesys, g_res3_off, g_res3_on, g_res3_off_b, g_res3_on_b;
void nvmtl_vk_res3_stats(uint64_t out[6]) { out[0] = g_res3_made; out[1] = g_res3_madesys; out[2] = g_res3_off; out[3] = g_res3_on;
    out[4] = g_res3_off_b >> 20; out[5] = g_res3_on_b >> 20; }
static int nvmtl_vk_sparse_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_NO_SPARSE_BUF"), *r = getenv("NVMTL_NO_RES2");
        on = g_sparse && g_qsparse && !(e && e[0] == '1') && !(r && r[0] == '1');
        nvlog("residency (res3): Private buffers >= 1 MiB %s (NVK sparse bind %s, queue sparse %s, NVMTL_NO_SPARSE_BUF=%s)",
              on ? "get a STABLE GPU address whose memory pages" : "as before (fixed memory)",
              g_sparse ? "present" : "ABSENT", g_qsparse ? "yes" : "no", e ? e : "unset");
    }
    return on;
}
static int nvmtl_vk_sparse_bind(VkBuffer b, int unbind_first, VkDeviceMemory mem, VkDeviceSize size)
{
    VkSparseMemoryBind mb[2]; uint32_t n = 0;
    if (unbind_first) mb[n++] = (VkSparseMemoryBind){ 0, size, VK_NULL_HANDLE, 0, 0 };
    if (mem) mb[n++] = (VkSparseMemoryBind){ 0, size, mem, 0, 0 };
    VkSparseBufferMemoryBindInfo bbi = { b, n, mb };
    VkBindSparseInfo bsi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 1, &bbi, 0, NULL, 0, NULL, 0, NULL };
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 }; VkFence f = VK_NULL_HANDLE;
    if (pvkCreateFence(g_dev, &fci, NULL, &f) != VK_SUCCESS) return -1;
    pthread_mutex_lock(&nvmtl_submit_mutex);
    VkResult r = g_state == 1 ? pvkQueueBindSparse(g_queue, 1, &bsi, f) : VK_ERROR_DEVICE_LOST;
    pthread_mutex_unlock(&nvmtl_submit_mutex);
    if (r == VK_SUCCESS) { nvmtl_wait_enter(); r = pvkWaitForFences(g_dev, 1, &f, VK_TRUE, 5000000000ull); nvmtl_wait_leave(); }
    pvkDestroyFence(g_dev, f, NULL);
    if (r != VK_SUCCESS) nvlog("residency (res3): vkQueueBindSparse(%s%s, %llu MB) -> %d", unbind_first ? "unbind + " : "",
                               mem ? "bind" : "nothing", (unsigned long long)(size >> 20), r);
    return r == VK_SUCCESS ? 0 : -1;
}
static int nvmtl_vk_sysmem_type(uint32_t bits)
{
    for (uint32_t k = 0; k < g_memp.memoryTypeCount; k++)
        if ((bits & (1u << k)) && !(g_memp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) return (int)k;
    return -1;
}
static int nvmtl_vk_buffer_create_sparse(size_t size, nvk_buffer *out)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT, size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | NVMTL_RT_BUFFER_USAGE,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &bci, NULL, &b) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, b, &mr);
    uint64_t bfree = 0, bbud = 0, bused = 0; int bmeas = 0;
    int sys = nvmtl_vk_vram_wall(mr.size, &bfree, &bbud, &bused, &bmeas);
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, mr.size, 0 };
    VkDeviceMemory m = VK_NULL_HANDLE; VkResult ar = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!sys) { int mt = nvmtl_vk_buffer_vram_type(mr.memoryTypeBits);
                if (mt >= 0) { mai.memoryTypeIndex = (uint32_t)mt; ar = pvkAllocateMemory(g_dev, &mai, NULL, &m); } }
    if (ar != VK_SUCCESS) { int mt = nvmtl_vk_sysmem_type(mr.memoryTypeBits); sys = 1;
                            if (mt >= 0) { mai.memoryTypeIndex = (uint32_t)mt; ar = pvkAllocateMemory(g_dev, &mai, NULL, &m); } }
    if (ar != VK_SUCCESS || nvmtl_vk_sparse_bind(b, 0, m, mr.size)) {
        if (m) pvkFreeMemory(g_dev, m, NULL);
        pvkDestroyBuffer(g_dev, b, NULL);
        static unsigned n; if (n++ < 4) nvlog("residency (res3): a %zu-byte stable-address buffer was refused (alloc %d) - the old path", size, ar);
        return -1;
    }
    out->buf = b; out->mem = m; out->size = size; out->alloc = mr.size; out->sparse = 1; out->sysmem = sys; out->mtype1 = mai.memoryTypeIndex + 1;
    g_alloc_bytes += mr.size; g_buf_made++; g_res3_made++;
    if (!sys) nvmtl_vk_budget_charge(mr.size);
    else { g_res3_madesys++; g_buf_vram_refused++;
           nvlog("vk: private buffer of %zu bytes REFUSED VRAM (our soft gate) -> system memory under a stable address (res3): %llu MB free, "
                 "heapUsage %llu of budget %llu MB, %llu refused so far", size, (unsigned long long)(bfree >> 20),
                 (unsigned long long)(bused >> 20), (unsigned long long)(bbud >> 20), (unsigned long long)g_buf_vram_refused); }
    return 0;
}

int nvmtl_vk_buffer_create(size_t size, int host_visible, nvk_buffer *out)
{
    const uint64_t t0 = nvmtl_perf_now();
    nvk_buffer pending = {0};
    const int r = nvmtl_vk_buffer_create_body(size, host_visible, &pending);
    if (r) {
        if (pending.buf) pvkDestroyBuffer(g_dev, (VkBuffer)pending.buf, NULL);
        if (pending.mem) {
            if (pending.map) pvkUnmapMemory(g_dev, (VkDeviceMemory)pending.mem);
            pvkFreeMemory(g_dev, (VkDeviceMemory)pending.mem, NULL);
            if (!host_visible) g_budget_stale = 1;
        }
        memset(out, 0, sizeof *out);
    } else *out = pending;
    nvmtl_perf_note(r == 0 && out->pool ? NVP_BUFPOOL : NVP_BUFNEW, t0, size);
    return r;
}
static int nvmtl_vk_buffer_create_body(size_t size, int host_visible, nvk_buffer *out)
{
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out); out->size = size;
    if (host_visible == 2 && size <= NVMTL_POOL_MAX && vpool_on() && pool_buffer_create_in(1, size, out) == 0) return 0;
    memset(out, 0, sizeof *out); out->size = size;
    if (host_visible && host_visible != 3 && size <= NVMTL_POOL_MAX && pool_buffer_create(size, out) == 0) return 0;
    memset(out, 0, sizeof *out); out->size = size;
    if (!host_visible && size < NVMTL_PPOOL_MAX && ppool_on() && pool_buffer_create_in(2, size, out) == 0) return 0;
    memset(out, 0, sizeof *out); out->size = size;
    if (host_visible != 3 && bpark_take(size, host_visible, out) == 0) return 0;
    memset(out, 0, sizeof *out); out->size = size;
    if (!host_visible && size >= (1u << 20) && nvmtl_vk_sparse_on() && nvmtl_vk_buffer_create_sparse(size, out) == 0) return 0;
    memset(out, 0, sizeof *out); out->size = size;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | NVMTL_RT_BUFFER_USAGE,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VKCK(nvmtl_opaque_CreateBuffer(g_dev, &bci, NULL, &out->buf), "vkCreateBuffer");
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, out->buf, &mr);
    int mt = host_visible ? memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                          : nvmtl_vk_buffer_vram_type(mr.memoryTypeBits);
    if (mt < 0) { nvlog("vk: no memory type for buffer (host_visible=%d)", host_visible); return -1; }
    if ((host_visible == 2 && size > NVMTL_POOL_MAX) || host_visible == 3) { int bt = bar1_memtype_for(mr.memoryTypeBits, mr.size, host_visible == 3); if (bt >= 0) {
        VkMemoryAllocateFlagsInfo bf = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
        VkMemoryAllocateInfo bi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &bf, mr.size, (uint32_t)bt };
        VkDeviceMemory bm = VK_NULL_HANDLE; void *bp = NULL;
        VkResult r1 = pvkAllocateMemory(g_dev, &bi, NULL, &bm);
        VkResult r2 = r1 == VK_SUCCESS ? pvkMapMemory(g_dev, bm, 0, VK_WHOLE_SIZE, 0, &bp) : r1;
        VkResult r3 = r2 == VK_SUCCESS ? pvkBindBufferMemory(g_dev, out->buf, bm, 0) : r2;
        if (r3 == VK_SUCCESS) {
            out->mem = bm; out->map = bp; out->alloc = mr.size; out->bar1 = 1; out->mtype1 = (uint32_t)bt + 1; g_alloc_bytes += mr.size; g_bar1_bytes += mr.size; g_buf_made++;
            { static int said[2]; if (!said[host_visible == 3]++) nvlog("vk: %s buffer of %zu bytes placed in VRAM through BAR1 (type %d)",
                                                                     host_visible == 3 ? "command-streamer-read (ICB records)" : "shared", size, bt); }
            return 0; }
        if (bp) pvkUnmapMemory(g_dev, bm);
        if (bm) pvkFreeMemory(g_dev, bm, NULL);
        if (host_visible == 3) { static int said; if (!said++) nvlog("vk: ICB record buffer REFUSED VRAM (alloc %d, map %d, bind %d) - system RAM (said once)", r1, r2, r3); }
        else {
        g_bar1_off = 1;
        nvlog("vk: shared-in-VRAM REFUSED (alloc %d, map %d, bind %d) at %zu BAR bytes - system RAM for this process", r1, r2, r3, g_bar1_bytes); } } }
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL,
                                       VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, mr.size, (uint32_t)mt };
    if (!host_visible) {
        uint64_t bbud = 0, bused = 0, bfree = 0; int bmeas = 0;
        int bgate = nvmtl_vk_vram_wall(mr.size, &bfree, &bbud, &bused, &bmeas);
        VkResult br = bgate ? VK_ERROR_OUT_OF_DEVICE_MEMORY
                            : nvmtl_opaque_AllocateMemory(g_dev, &mai, NULL, &out->mem);
        if (br != VK_SUCCESS) {
            g_buf_vram_refused++;
            bpark_drain_private();
            nvlog("vk: private buffer of %zu bytes REFUSED VRAM (%s) -> %d: %llu MB free, %llu MB headroom, "
                  "heapUsage %llu of budget %llu MB (%s), %llu buffers live, %llu refused so far "
                  "- the caller places it in system memory",
                  size, bgate ? "our soft gate" : "RM refused it", br,
                  (unsigned long long)(bfree >> 20), (unsigned long long)(NVMTL_VRAM_HEADROOM >> 20),
                  (unsigned long long)(bused >> 20), (unsigned long long)(bbud >> 20),
                  bmeas ? "measured" : "estimated",
                  (unsigned long long)(g_buf_made - g_buf_gone),
                  (unsigned long long)g_buf_vram_refused);
            return -1;
        }
    } else VKCK(nvmtl_opaque_AllocateMemory(g_dev, &mai, NULL, &out->mem), "vkAllocateMemory(buffer)");
    out->alloc = mr.size; out->mtype1 = (uint32_t)mt + 1;
    if (!host_visible) nvmtl_vk_budget_charge(mr.size);
    VKCK(pvkBindBufferMemory(g_dev, out->buf, out->mem, 0), "vkBindBufferMemory");
    if (host_visible) { VkResult mr_ = pvkMapMemory(g_dev, out->mem, 0, VK_WHOLE_SIZE, 0, &out->map); if (mr_ != VK_SUCCESS) {
        nvlog("vk: vkMapMemory -> %d with %llu buffers live (%llu made, %llu gone), %zu bytes allocated, memory type %d", mr_,
              (unsigned long long)(g_buf_made - g_buf_gone), (unsigned long long)g_buf_made, (unsigned long long)g_buf_gone, g_alloc_bytes, mt);
        return -1; } }
    g_alloc_bytes += mr.size;
    g_buf_made++;
    if ((g_buf_made & 0xFFF) == 0) nvlog("buffer census: %llu made, %llu gone, %llu live, %zu bytes allocated; pool %llu blocks, %llu pooled live, %llu bytes in ranges",
        (unsigned long long)g_buf_made, (unsigned long long)g_buf_gone, (unsigned long long)(g_buf_made - g_buf_gone), g_alloc_bytes,
        (unsigned long long)g_pool_blocks, (unsigned long long)g_pool_live, (unsigned long long)g_pool_bytes);
    { static int said[2]; if (!said[!!host_visible]) { said[!!host_visible] = 1;
        nvlog("vk: buffers (host_visible=%d) take memory type %d flags 0x%x heap %u", host_visible, mt,
              g_memp.memoryTypes[mt].propertyFlags, g_memp.memoryTypes[mt].heapIndex); } }
    return 0;
}

size_t nvmtl_vk_host_import_align(void) { return g_host_align; }
const char *nvmtl_image_ident(const void *addr, char *out, size_t n)
{
    Dl_info di;
    if (!out || !n) return "";
    snprintf(out, n, "?");
    if (!addr || !dladdr(addr, &di) || !di.dli_fbase) return out;
    const char *path = di.dli_fname ? di.dli_fname : "?";
    const struct mach_header_64 *mh = (const struct mach_header_64 *)di.dli_fbase;
    if (mh->magic != MH_MAGIC_64) { snprintf(out, n, "not-64-bit %s", path); return out; }
    const struct load_command *lc = (const struct load_command *)(mh + 1);
    for (uint32_t i = 0; i < mh->ncmds && lc->cmdsize; i++, lc = (const struct load_command *)((const char *)lc + lc->cmdsize)) {
        if (lc->cmd != LC_UUID) continue;
        const uint8_t *u = ((const struct uuid_command *)lc)->uuid;
        snprintf(out, n, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X %s", u[0], u[1], u[2], u[3], u[4], u[5],
                 u[6], u[7], u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15], path);
        return out;
    }
    snprintf(out, n, "no-uuid %s", path);
    return out;
}

/* Host-pointer imports kept after their buffer dies, for the next import of the same pages. Apple's OpenCL wraps the
   caller's host array in a fresh no-copy buffer for each clEnqueueRead/WriteBuffer (and Managed mirrors import their
   shadow), and an import cost 0.2-0.4 ms plus 0.2-3.9 ms to free - Geekbench OpenCL spent 76% of a window in
   vkFreeMemory of imports. Until fix12 the wrappers leaked from a pool-less thread and Apple's own lookup found them
   alive; with that leak fixed they were made and freed every enqueue. An entry is reused only for the same pointer and
   length, while the address still maps the same VM object at the same offset (memory freed and mapped again at the
   same address is new pages); it is freed after 2 s unused, past 64 entries or past 1 GiB. NVMTL_IMPORT_CACHE=1 turns
   it on. OFF by default: it removed the per-enqueue imports (4585 -> 8 in 5 s of clrw) but the 8 MB write/kernel/read
   loop on a device buffer went from 2.19 to 3.2-4.1 ms per round, for a reason not yet found. */
#include <mach/mach_vm.h>
typedef struct { void *ptr; size_t size; uint32_t obj; uint64_t objoff; nvk_buffer b; uint64_t at; } nvmtl_impc;
#define NVMTL_IMPC_N 64
#define NVMTL_IMPC_BYTES (1024ull << 20)
#define NVMTL_IMPC_AGE_NS (2000000000ull)
static nvmtl_impc g_impc[NVMTL_IMPC_N]; static unsigned g_nimpc; static size_t g_impc_bytes;
static pthread_mutex_t g_impc_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long long g_impc_hit, g_impc_miss, g_impc_kept;
static int nvmtl_impc_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_IMPORT_CACHE"); on = e && e[0] == '1';   // opt-in: see the clrw note above
        nvlog("host import cache: %s (NVMTL_IMPORT_CACHE=%s)", on ? "ON - an import is kept 2 s for the next one of the same pages" : "off", e ? e : "unset"); }
    return on;
}
// the VM object (and offset in it) that maps ptr, and that the whole range [ptr, ptr+size) lies in that one mapping
static int nvmtl_vm_ident(const void *ptr, size_t size, uint32_t *obj, uint64_t *off)
{
    mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)ptr; mach_vm_size_t rs = 0; natural_t depth = 0;
    for (;;) {
        vm_region_submap_info_data_64_t info; mach_msg_type_number_t cnt = VM_REGION_SUBMAP_INFO_COUNT_64;
        mach_vm_address_t ra = a;
        if (mach_vm_region_recurse(mach_task_self(), &ra, &rs, &depth, (vm_region_recurse_info_t)&info, &cnt) != KERN_SUCCESS) return -1;
        if (ra > a) return -1;
        if (info.is_submap) { depth++; continue; }
        if (!info.object_id || a + size > ra + rs) return -1;
        *obj = info.object_id; *off = info.offset + (a - ra);
        return 0;
    }
}
static void nvmtl_impc_free(nvk_buffer *b)
{
    if (b->buf) pvkDestroyBuffer(g_dev, (VkBuffer)b->buf, NULL);
    if (b->mem) { pvkFreeMemory(g_dev, (VkDeviceMemory)b->mem, NULL); g_alloc_bytes -= b->alloc; }
    memset(b, 0, sizeof *b);
}
// takes out the entries that are too old or over the caps (the caller frees them outside the lock)
static unsigned nvmtl_impc_expire_locked(uint64_t now, nvk_buffer *dead, unsigned cap)
{
    unsigned nd = 0;
    for (unsigned i = 0; i < g_nimpc && nd < cap; ) {
        if (now - g_impc[i].at > NVMTL_IMPC_AGE_NS) { dead[nd++] = g_impc[i].b; g_impc_bytes -= g_impc[i].size; g_impc[i] = g_impc[--g_nimpc]; }
        else i++;
    }
    return nd;
}
static int nvmtl_impc_take(void *ptr, size_t size, nvk_buffer *out)
{
    if (!nvmtl_impc_on()) return -1;
    uint32_t obj = 0; uint64_t off = 0; const int known = nvmtl_vm_ident(ptr, size, &obj, &off) == 0;
    nvk_buffer dead[NVMTL_IMPC_N]; unsigned nd; int hit = -1;
    pthread_mutex_lock(&g_impc_lock);
    nd = nvmtl_impc_expire_locked(clock_gettime_nsec_np(CLOCK_UPTIME_RAW), dead, NVMTL_IMPC_N);
    for (unsigned i = 0; i < g_nimpc; i++) if (g_impc[i].ptr == ptr && g_impc[i].size == size) {
        if (known && g_impc[i].obj == obj && g_impc[i].objoff == off) { *out = g_impc[i].b; hit = (int)i; }
        else { dead[nd++] = g_impc[i].b; }   // the same address now maps other pages: the old import goes
        g_impc_bytes -= g_impc[i].size; g_impc[i] = g_impc[--g_nimpc];
        break;
    }
    pthread_mutex_unlock(&g_impc_lock);
    for (unsigned i = 0; i < nd; i++) nvmtl_impc_free(&dead[i]);
    if (hit < 0) { atomic_fetch_add(&g_impc_miss, 1); return -1; }
    g_buf_made++;
    const unsigned long long k = atomic_fetch_add(&g_impc_hit, 1) + 1;
    if (!(k & (k - 1))) nvlog("host import cache: %llu imports reused, %llu made, %llu kept", k, (unsigned long long)atomic_load(&g_impc_miss), (unsigned long long)atomic_load(&g_impc_kept));
    return 0;
}
// a dying imported buffer: kept for the next import of the same pages (1), or the caller frees it (0)
static int nvmtl_impc_put(nvk_buffer *b)
{
    if (!nvmtl_impc_on() || !b->imported || !b->map || !b->buf || !b->mem || b->size > NVMTL_IMPC_BYTES / 4) return 0;
    uint32_t obj = 0; uint64_t off = 0;
    if (nvmtl_vm_ident(b->map, b->size, &obj, &off)) return 0;
    nvk_buffer dead[NVMTL_IMPC_N + 1]; unsigned nd;
    pthread_mutex_lock(&g_impc_lock);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    nd = nvmtl_impc_expire_locked(now, dead, NVMTL_IMPC_N);
    while (g_nimpc && (g_nimpc >= NVMTL_IMPC_N || g_impc_bytes + b->size > NVMTL_IMPC_BYTES)) {   // the oldest goes
        unsigned o = 0; for (unsigned i = 1; i < g_nimpc; i++) if (g_impc[i].at < g_impc[o].at) o = i;
        dead[nd++] = g_impc[o].b; g_impc_bytes -= g_impc[o].size; g_impc[o] = g_impc[--g_nimpc];
    }
    g_impc[g_nimpc++] = (nvmtl_impc){ b->map, b->size, obj, off, *b, now }; g_impc_bytes += b->size;
    pthread_mutex_unlock(&g_impc_lock);
    for (unsigned i = 0; i < nd; i++) nvmtl_impc_free(&dead[i]);
    atomic_fetch_add(&g_impc_kept, 1); g_buf_gone++;
    memset(b, 0, sizeof *b);
    return 1;
}
int nvmtl_vk_buffer_import_host(void *ptr, size_t size, nvk_buffer *out)
{
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out);
    if (ptr && size && nvmtl_impc_take(ptr, size, out) == 0) return 0;
    memset(out, 0, sizeof *out);
    if (!g_host_import || !pvkGetMemoryHostPointerPropertiesEXT || !g_host_align || !ptr || !size) return -1;
    /* An unaligned pointer or size (Apple's GL-based OpenCL hands its cl_mem data at malloc alignment): the whole pages
       around [ptr, ptr + size) are imported and the buffer is bound at ptr's offset into them, so the GPU still sees
       only the caller's bytes. Those pages are mapped (they hold ptr's bytes); the rest of them is not reachable
       through this buffer. Otherwise the buffer is COPIED and re-synchronized at every submission.
       Opt-in (NVMTL_IMPORT_UNALIGNED=1) since 12 Nov: Geekbench 7 OpenCL 87.7k -> 89.0-89.4k, but one of six runs met the
       GuC MEMORY_CAT_ERROR that is still open (docs/ROADMAP.md), and importing adds binds and unbinds, its suspect. */
    uint8_t *const base = (uint8_t *)((uintptr_t)ptr & ~(uintptr_t)(g_host_align - 1));
    const size_t lead = (size_t)((uint8_t *)ptr - base);
    const size_t isize = (lead + size + g_host_align - 1) & ~(size_t)(g_host_align - 1);
    { static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_IMPORT_UNALIGNED"); on = e && e[0] == '1'; }
      if ((lead || isize != size) && !on) {
          static int said; if (!said++) nvlog("vk: host-pointer import refused — ptr %p / size %zu are not both a multiple of %zu; that buffer is COPIED (NVMTL_IMPORT_UNALIGNED=1 imports it)",
                                              ptr, size, g_host_align);
          return -1; } }
    VkMemoryHostPointerPropertiesEXT mhp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT, NULL, 0 };
    if (pvkGetMemoryHostPointerPropertiesEXT(g_dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &mhp) != VK_SUCCESS
        || !mhp.memoryTypeBits) { nvlog("vk: this pointer reports no importable memory type"); return -1; }
    out->size = size;
    VkExternalMemoryBufferCreateInfo emb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb, 0, size,
        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT |
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | NVMTL_RT_BUFFER_USAGE,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (nvmtl_opaque_CreateBuffer(g_dev, &bci, NULL, &out->buf) != VK_SUCCESS) {
        nvlog("vk: vkCreateBuffer refused an external-memory buffer of %zu bytes", size); out->buf = NULL; return -1; }
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, (VkBuffer)out->buf, &mr);
    if (mr.alignment && lead % mr.alignment) {
        static int said; if (!said++) nvlog("vk: host-pointer import refused — %p is %zu bytes into its page, not a multiple of the buffer alignment %llu; that buffer is COPIED",
                                            ptr, lead, (unsigned long long)mr.alignment);
        pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); out->buf = NULL; return -1; }
    int mt = memtype(mr.memoryTypeBits & mhp.memoryTypeBits,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (mt < 0) { nvlog("vk: no host-visible memory type is both importable and valid for this buffer");
                  pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); out->buf = NULL; return -1; }
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL,
                                       VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, &mafi,
                                             VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, isize, (uint32_t)mt };
    if (nvmtl_opaque_AllocateMemory(g_dev, &mai, NULL, &out->mem) != VK_SUCCESS) {
        nvlog("vk: vkAllocateMemory refused the imported host pointer %p (%zu bytes, memory type %d)", ptr, size, mt);
        pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); out->buf = NULL; out->mem = NULL; return -1; }
    if (pvkBindBufferMemory(g_dev, (VkBuffer)out->buf, (VkDeviceMemory)out->mem, lead) != VK_SUCCESS) {
        nvlog("vk: vkBindBufferMemory refused the imported allocation");
        pvkFreeMemory(g_dev, (VkDeviceMemory)out->mem, NULL); pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL);
        out->buf = NULL; out->mem = NULL; return -1; }
    out->map = ptr; out->alloc = isize; out->imported = 1; out->mtype1 = (uint32_t)mt + 1;
    out->moff = lead;   // where the buffer starts in mem: an alias (texel view, texture over the buffer) binds at moff + offset
    g_alloc_bytes += isize; g_buf_made++;
    if (lead || isize != size) { static int said; if (!said++) nvlog("vk: imported an unaligned host pointer %p (%zu bytes) through the %zu pages around it, bound %zu bytes in",
                                                                     ptr, size, isize / g_host_align, lead); }
    { static int said; if (!said++) nvlog("vk: imported the caller's pages as device memory — %zu bytes at %p, memory type %d", size, ptr, mt); }
    return 0;
}

static pthread_key_t g_stage_key; static pthread_once_t g_stage_once = PTHREAD_ONCE_INIT;
static void nvmtl_stage_free(void *p) { nvk_buffer *b = p; nvmtl_vk_buffer_destroy(b); free(b); }
static void nvmtl_stage_key_make(void) { if (pthread_key_create(&g_stage_key, nvmtl_stage_free)) nvlog("stage: pthread_key_create FAILED"); }
static int nvmtl_vk_stage(nvk_queue *q, size_t size, nvk_buffer *out)
{
    (void)q; pthread_once(&g_stage_once, nvmtl_stage_key_make);
    nvk_buffer *s = pthread_getspecific(g_stage_key);
    if (!s) { s = calloc(1, sizeof *s); if (!s) return -1; if (pthread_setspecific(g_stage_key, s)) { free(s); nvlog("stage: pthread_setspecific FAILED"); return -1; } }
    if (s->buf && s->size >= size) { *out = *s; return 0; }
    nvmtl_vk_buffer_destroy(s); memset(s, 0, sizeof *s);
    if (nvmtl_vk_buffer_create(size, 1, s)) { memset(s, 0, sizeof *s); return -1; }
    *out = *s;
    return 0;
}

int nvmtl_vk_format_is_storage(uint32_t vkfmt)
{
    static void (*fp)(VkPhysicalDevice, VkFormat, VkFormatProperties *); static int tried;
    if (nvmtl_vk_init()) return 0;
    if (!tried) { tried = 1; fp = (void (*)(VkPhysicalDevice, VkFormat, VkFormatProperties *))pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
                  if (!fp) nvlog("vk: vkGetPhysicalDeviceFormatProperties is missing — NO texture can be a storage image"); }
    if (!fp) return 0;
    VkFormatProperties fpz; memset(&fpz, 0, sizeof fpz); fp(g_pdev, (VkFormat)vkfmt, &fpz);
    return (fpz.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) != 0;
}
/* How a mip chain of this format can be made by blits: 2 = linear-filtered (a box at 2:1, in linear light for sRGB),
   1 = nearest only (integer formats), 0 = no blits at all. */
int nvmtl_vk_format_mip_blit(uint32_t vkfmt)
{
    static PFN_vkGetPhysicalDeviceFormatProperties gfp;
    if (nvmtl_vk_init()) return 0;
    if (!gfp) gfp = (PFN_vkGetPhysicalDeviceFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
    if (!gfp) return 0;
    VkFormatProperties fp; memset(&fp, 0, sizeof fp); gfp(g_pdev, (VkFormat)vkfmt, &fp);
    const VkFormatFeatureFlags blit = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT;
    if ((fp.optimalTilingFeatures & blit) != blit) return 0;
    return (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT) ? 2 : 1;
}
/* 1 when every listed format can be sampled through a linear filter at optimal tiling (the device's
   supports32BitFloatFiltering asks this of R32, RG32 and RGBA32 float); 0 otherwise, or when Vulkan is down. */
int nvmtl_vk_formats_filter_linear(const uint32_t *vkfmts, int n)
{
    static PFN_vkGetPhysicalDeviceFormatProperties gfp;
    if (nvmtl_vk_init()) return 0;
    if (!gfp) gfp = (PFN_vkGetPhysicalDeviceFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
    if (!gfp) return 0;
    for (int i = 0; i < n; i++) {
        VkFormatProperties fp; memset(&fp, 0, sizeof fp); gfp(g_pdev, (VkFormat)vkfmts[i], &fp);
        if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) return 0;
    }
    return 1;
}
/* 1 when every listed format can be a 2D optimal-tiling colour attachment that is also sampled, with the given sample
   count (VkSampleCountFlagBits; the device's supports32BitMSAA asks 4x of the 32-bit formats); 0 otherwise, or when
   Vulkan is down. */
int nvmtl_vk_formats_msaa(const uint32_t *vkfmts, int n, uint32_t samples)
{
    static PFN_vkGetPhysicalDeviceImageFormatProperties gif;
    if (nvmtl_vk_init()) return 0;
    if (!gif) gif = (PFN_vkGetPhysicalDeviceImageFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceImageFormatProperties");
    if (!gif) return 0;
    for (int i = 0; i < n; i++) {
        VkImageFormatProperties p; memset(&p, 0, sizeof p);
        if (gif(g_pdev, (VkFormat)vkfmts[i], VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
                VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, 0, &p) != VK_SUCCESS) return 0;
        if (!(p.sampleCounts & samples)) return 0;
    }
    return 1;
}
static int nvmtl_vk_cube_compat(uint32_t mtl_type, uint32_t w, uint32_t h, uint32_t layers)
{ return mtl_type == 3 && w == h && w <= 0x8000 && layers >= 6; }
static uint32_t nvmtl_vk_srgb_pair(uint32_t f)
{
    switch (f) {
        case VK_FORMAT_R8_UNORM: return VK_FORMAT_R8_SRGB;                          case VK_FORMAT_R8_SRGB: return VK_FORMAT_R8_UNORM;
        case VK_FORMAT_R8G8_UNORM: return VK_FORMAT_R8G8_SRGB;                      case VK_FORMAT_R8G8_SRGB: return VK_FORMAT_R8G8_UNORM;
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB;              case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB;              case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_A8B8G8R8_UNORM_PACK32: return VK_FORMAT_A8B8G8R8_SRGB_PACK32; case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return VK_FORMAT_A8B8G8R8_UNORM_PACK32;
        default: return 0;
    }
}

/*
 * The formats a texture may be viewed as, grouped by bytes per pixel.
 *
 * Metal allows newTextureViewWithPixelFormat: between any two formats of the same size, for a
 * buffer-backed texture as much as for an ordinary one, and rejects a format of a different size.
 * An ordinary image here already carries every such view, because it is created with
 * VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT and no format list, which permits all compatible formats. A
 * buffer alias has to name its formats instead, since a DRM-format-modifier image that is mutable
 * is required to carry a VkImageFormatListCreateInfo. Naming only the format's sRGB counterpart
 * refused every other view Metal allows, which leaves a client that views a BGRA8Unorm surface as
 * RGBA8Unorm - screen capture does exactly that - with no texture at all.
 *
 * These are the uncompressed formats this driver maps a MTLPixelFormat to. A format it does not
 * map cannot be asked for, and a buffer alias of a block-compressed format is refused earlier.
 */
static const VkFormat nvmtl_vk_view1[] = {
    VK_FORMAT_R8_UNORM, VK_FORMAT_R8_SRGB, VK_FORMAT_R8_SNORM, VK_FORMAT_R8_UINT, VK_FORMAT_R8_SINT,
};
static const VkFormat nvmtl_vk_view2[] = {
    VK_FORMAT_R16_UNORM, VK_FORMAT_R16_SNORM, VK_FORMAT_R16_UINT, VK_FORMAT_R16_SINT, VK_FORMAT_R16_SFLOAT,
    VK_FORMAT_R8G8_UNORM, VK_FORMAT_R8G8_SNORM, VK_FORMAT_R8G8_UINT, VK_FORMAT_R8G8_SINT,
};
static const VkFormat nvmtl_vk_view4[] = {
    VK_FORMAT_R32_UINT, VK_FORMAT_R32_SINT, VK_FORMAT_R32_SFLOAT,
    VK_FORMAT_R16G16_UNORM, VK_FORMAT_R16G16_SNORM, VK_FORMAT_R16G16_UINT, VK_FORMAT_R16G16_SINT,
    VK_FORMAT_R16G16_SFLOAT,
    VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_SNORM,
    VK_FORMAT_R8G8B8A8_UINT, VK_FORMAT_R8G8B8A8_SINT,
    VK_FORMAT_B8G8R8A8_UNORM, VK_FORMAT_B8G8R8A8_SRGB,
    VK_FORMAT_A2B10G10R10_UNORM_PACK32, VK_FORMAT_A2B10G10R10_UINT_PACK32,
    VK_FORMAT_A2R10G10B10_UNORM_PACK32,
    VK_FORMAT_B10G11R11_UFLOAT_PACK32, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32,
};
static const VkFormat nvmtl_vk_view8[] = {
    VK_FORMAT_R32G32_UINT, VK_FORMAT_R32G32_SINT, VK_FORMAT_R32G32_SFLOAT,
    VK_FORMAT_R16G16B16A16_UNORM, VK_FORMAT_R16G16B16A16_SNORM, VK_FORMAT_R16G16B16A16_UINT,
    VK_FORMAT_R16G16B16A16_SINT, VK_FORMAT_R16G16B16A16_SFLOAT,
};
static const VkFormat nvmtl_vk_view16[] = {
    VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_R32G32B32A32_SINT, VK_FORMAT_R32G32B32A32_SFLOAT,
};

/* The group a texture of this many bytes per pixel belongs to. The mask kept on an image is
   relative to the group its own size selects, so 32 bits is always enough for it. */
static uint32_t nvmtl_vk_view_group(uint32_t bpp, const VkFormat **out)
{
    switch (bpp) {
        case 1:  *out = nvmtl_vk_view1;  return (uint32_t)(sizeof nvmtl_vk_view1  / sizeof *nvmtl_vk_view1);
        case 2:  *out = nvmtl_vk_view2;  return (uint32_t)(sizeof nvmtl_vk_view2  / sizeof *nvmtl_vk_view2);
        case 4:  *out = nvmtl_vk_view4;  return (uint32_t)(sizeof nvmtl_vk_view4  / sizeof *nvmtl_vk_view4);
        case 8:  *out = nvmtl_vk_view8;  return (uint32_t)(sizeof nvmtl_vk_view8  / sizeof *nvmtl_vk_view8);
        case 16: *out = nvmtl_vk_view16; return (uint32_t)(sizeof nvmtl_vk_view16 / sizeof *nvmtl_vk_view16);
        default: *out = NULL; return 0;
    }
}

/* Every format named in an image's format list has to support what the image itself is used for:
   naming one that does not makes the whole vkCreateImage fail, which costs the texture rather than
   one view of it. */
static VkFormatFeatureFlags nvmtl_vk_usage_features(VkImageUsageFlags u)
{
    VkFormatFeatureFlags f = 0;
    if (u & VK_IMAGE_USAGE_SAMPLED_BIT)          f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (u & VK_IMAGE_USAGE_STORAGE_BIT)          f |= VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT;
    if (u & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (u & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)     f |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (u & VK_IMAGE_USAGE_TRANSFER_DST_BIT)     f |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return f;
}

/* Does the format list this image was created with carry vkfmt? A view of a format absent from the
   list is undefined, so a format that did not make it in stays refused. */
static int nvmtl_vk_view_listed(const nvk_image *img, uint32_t vkfmt)
{
    const VkFormat *tbl = NULL;
    const uint32_t n = nvmtl_vk_view_group(img->bpp, &tbl);
    for (uint32_t i = 0; i < n; i++)
        if ((uint32_t)tbl[i] == vkfmt) return (int)((img->vfl >> i) & 1u);
    return 0;
}
static int nvmtl_vk_3d_slice_ok(VkFormat fmt, VkImageUsageFlags usage)
{
    static PFN_vkGetPhysicalDeviceImageFormatProperties2 gif; static int tried;
    if (!tried) { tried = 1;
        gif = (PFN_vkGetPhysicalDeviceImageFormatProperties2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceImageFormatProperties2");
        if (!gif) nvlog("vk: vkGetPhysicalDeviceImageFormatProperties2 is MISSING — no 3D texture can take a per-slice attachment view"); }
    if (!gif) return 0;
    VkPhysicalDeviceImageFormatInfo2 in = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, NULL, fmt,
        VK_IMAGE_TYPE_3D, VK_IMAGE_TILING_OPTIMAL, usage,
        VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT };
    VkImageFormatProperties2 props = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
    VkResult r = gif(g_pdev, &in, &props);
    static int said;
    if (r != VK_SUCCESS && said++ < 4)
        nvlog("vk: a 3D VkFormat %u usage 0x%x cannot take 2D_ARRAY_COMPATIBLE (VkResult %d) — its depthPlane attachments stay REFUSED",
              (unsigned)fmt, (unsigned)usage, (int)r);
    return r == VK_SUCCESS;
}
static void nvmtl_vk_image_mutable(VkImageCreateInfo *ici, nvk_image *out)
{
    ici->flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (ici->imageType == VK_IMAGE_TYPE_2D && ici->samples == VK_SAMPLE_COUNT_1_BIT
        && nvmtl_vk_cube_compat(out->mtl_type, ici->extent.width, ici->extent.height, ici->arrayLayers))
        ici->flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (ici->imageType == VK_IMAGE_TYPE_3D && (ici->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        && nvmtl_vk_3d_slice_ok(ici->format, ici->usage))
        ici->flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    out->vkflags = (uint32_t)ici->flags; out->usage = (uint32_t)ici->usage;
}
static int nvmtl_vk_view_guard(const nvk_image *img, uint32_t vkfmt, int a8, VkImageViewType vt, uint32_t levelCount,
                               VkImageViewUsageCreateInfo *uci, const char *who)
{
    const uint32_t t = img->mtl_type;
    int ok;
    switch (vt) {
        case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_1D_ARRAY: ok = t <= 1; break;
        case VK_IMAGE_VIEW_TYPE_2D: case VK_IMAGE_VIEW_TYPE_2D_ARRAY:
            ok = (t >= 2 && t <= 6) || t == 8
              || (t == 7 && (img->vkflags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT) && levelCount == 1); break;
        case VK_IMAGE_VIEW_TYPE_3D: ok = t == 7; break;
        case VK_IMAGE_VIEW_TYPE_CUBE: case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY:
            ok = t == 5 || t == 6 || (img->vkflags & VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT) != 0; break;
        default: ok = 0; break;
    }
    if (!ok) { nvlog("%s: view type %d of a textureType-%u image refused (NVK asserts on it)", who, (int)vt, t); return -1; }
    if (vkfmt == img->fmt) return 0;
    if (!(img->vkflags & VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT)) {
        nvlog("%s: view format %u of a format-%u image that is not mutable (depth / buffer texture) refused", who, vkfmt, img->fmt); return -1; }
    if (img->vfl && !nvmtl_vk_view_listed(img, vkfmt)) {
        nvlog("%s: view format %u of a buffer texture (format %u, %u B) refused - its image's format list (mask 0x%x) does not carry it",
              who, vkfmt, img->fmt, img->bpp, img->vfl); return -1; }
    uint32_t bw = 1, bh = 1, bb = 0;
    if (img->bw > 1 && !nvmtl_vk_format_block(vkfmt, &bw, &bh, &bb) && levelCount != 1) {
        nvlog("%s: uncompressed view of a block-compressed image over %u levels refused (NVK allows 1)", who, levelCount); return -1; }
    if (!(img->usage & VK_IMAGE_USAGE_STORAGE_BIT) || (!a8 && nvmtl_vk_format_is_storage(vkfmt))) return 0;
    *uci = (VkImageViewUsageCreateInfo){ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
                                         (VkImageUsageFlags)((img->usage & ~(uint32_t)VK_IMAGE_USAGE_STORAGE_BIT)
                                             | ((img->usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) && nvmtl_fbfetch_on() ? VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT : 0)) };
    return 1;
}
static uint32_t nvmtl_spirv_stex_hi(const void *p, size_t n)
{
    enum { STEX0 = 480, NSTEX = 128 };
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4; uint32_t hi = 0;
    if (!p || nw < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33) {
            uint32_t b = w[i + 3];
            if (b >= STEX0 && b < STEX0 + NSTEX && b - STEX0 + 1 > hi) hi = b - STEX0 + 1;
        }
        i += wc;
    }
    return hi;
}
int nvmtl_vk_image_create_mips(uint32_t w, uint32_t h, int bgra, uint32_t mips, nvk_image *out)
{
    return nvmtl_vk_image_create_ex(w, h, bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, 4, 0, mips, out);
}
int nvmtl_vk_image_create_ex(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, nvk_image *out)
{
    return nvmtl_vk_image_create_ex2(w, h, vkfmt, bpp, a8, mips, 0, out);
}
int nvmtl_vk_image_create_ex2(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, nvk_image *out)
{
    return nvmtl_vk_image_create_typed(w, h, vkfmt, bpp, a8, mips, storage, 2, 1, out);
}
uint64_t nvmtl_vk_vram_bytes(void)
{
    uint64_t best = 0;
    for (uint32_t i = 0; i < g_memp.memoryHeapCount; i++)
        if ((g_memp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && g_memp.memoryHeaps[i].size > best) best = g_memp.memoryHeaps[i].size;
    return best;
}
void nvmtl_vk_mem_split(uint64_t *vram, uint64_t *sys) { if (vram) *vram = g_vram_bytes; if (sys) *sys = g_sys_bytes; }
static int nvmtl_vk_vram_budget(uint64_t *budget, uint64_t *used)
{
    static uint64_t cb, cu, when; static int measured;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (!when || g_budget_stale || now - when > 50000000ull) {
        g_budget_stale = 0;
        uint32_t heap = 0; uint64_t best = 0;
        for (uint32_t i = 0; i < g_memp.memoryHeapCount; i++)
            if ((g_memp.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) && g_memp.memoryHeaps[i].size > best) { best = g_memp.memoryHeaps[i].size; heap = i; }
        measured = 0; cb = best * 7 / 10; cu = g_vram_bytes;
        if (g_budget_ext && pvkGetPhysicalDeviceMemoryProperties2) {
            VkPhysicalDeviceMemoryBudgetPropertiesEXT b = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_BUDGET_PROPERTIES_EXT, NULL, { 0 }, { 0 } };
            VkPhysicalDeviceMemoryProperties2 mp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2, &b, { 0 } };
            pvkGetPhysicalDeviceMemoryProperties2(g_pdev, &mp);
            if (b.heapBudget[heap]) { cb = b.heapBudget[heap]; cu = b.heapUsage[heap]; measured = 1; }
        }
        when = now;
        g_budget_delta = 0;
    }
    *budget = cb; *used = cu; return measured;
}
static int nvmtl_vk_vram_wall0(uint64_t want, uint64_t *freev_out, uint64_t *bud_out, uint64_t *used_out, int *measured_out);
uint64_t (*nvmtl_reclaim_hook)(uint64_t need);
void (*nvmtl_res2_pressure_hook)(uint64_t want);
uint64_t (*nvmtl_res2_evict_hook)(uint64_t want);
static uint64_t g_res2_evict_calls, g_res2_evict_won;
static int nvmtl_res2_evict_off(void) {
    static int off = -1;
    if (off < 0) { const char *e = getenv("NVMTL_NO_RES2_EVICT"); off = (e && *e && *e != '0') ? 1 : 0; }
    return off;
}
static __thread int t_in_reclaim;
static uint64_t g_res1_calls, g_res1_credit, g_res1_seen_free, g_res1_admit;
static int nvmtl_vk_vram_wall(uint64_t want, uint64_t *freev_out, uint64_t *bud_out, uint64_t *used_out, int *measured_out)
{
    uint64_t fv = 0, bd = 0, us = 0; int ms = 0;
    int gate = nvmtl_vk_vram_wall0(want, &fv, &bd, &us, &ms);
    if (fv > g_res1_seen_free) { uint64_t up = fv - g_res1_seen_free; g_res1_credit = g_res1_credit > up ? g_res1_credit - up : 0; }
    g_res1_seen_free = fv;
    if (gate && nvmtl_reclaim_hook && !t_in_reclaim) {
        uint64_t need = want + NVMTL_VRAM_HEADROOM > fv + g_res1_credit ? want + NVMTL_VRAM_HEADROOM - fv - g_res1_credit : 0;
        if (need) {
            t_in_reclaim = 1; g_res1_calls++;
            g_res1_credit += nvmtl_reclaim_hook(need);
            t_in_reclaim = 0;
        }
        if (ms && want + NVMTL_VRAM_HEADROOM <= fv + g_res1_credit) {
            g_res1_credit = g_res1_credit > want ? g_res1_credit - want : 0; g_res1_admit++; gate = 0;
            { static unsigned said; if (said++ < 8 || said % 1000 == 0)
                nvlog("residency (res1): %llu MB admitted to VRAM on purged bytes RM has not shown yet (free %llu MB, credit left %llu MB, %llu admitted)",
                      (unsigned long long)(want >> 20), (unsigned long long)(fv >> 20), (unsigned long long)(g_res1_credit >> 20),
                      (unsigned long long)g_res1_admit); }
        }
    }
    if (gate && nvmtl_res2_pressure_hook && !t_in_reclaim) nvmtl_res2_pressure_hook(want);
    if (gate && nvmtl_res2_evict_hook && !t_in_reclaim && !nvmtl_res2_evict_off()) {
        t_in_reclaim = 1;
        const uint64_t freed = nvmtl_res2_evict_hook(want);
        t_in_reclaim = 0;
        if (freed) {
            g_budget_stale = 1;
            const int g2 = nvmtl_vk_vram_wall0(want, &fv, &bd, &us, &ms);
            if (!g2) { gate = 0; g_res2_evict_won++;
                { static unsigned said; if (said++ < 8 || said % 4096 == 0)
                    nvlog("residency (res2): evicted %llu MB on the allocating thread and ADMITTED a %llu MB "
                          "resource to VRAM (free now %llu MB; Apple's order - the newest wins the card; %llu admitted)",
                          (unsigned long long)(freed >> 20), (unsigned long long)(want >> 20),
                          (unsigned long long)(fv >> 20), (unsigned long long)g_res2_evict_won); } }
            g_res2_evict_calls++;
        }
    }
    if (freev_out) *freev_out = fv;
    if (bud_out) *bud_out = bd;
    if (used_out) *used_out = us;
    if (measured_out) *measured_out = ms;
    return gate;
}
static int nvmtl_vk_vram_wall0(uint64_t want, uint64_t *freev_out, uint64_t *bud_out, uint64_t *used_out, int *measured_out)
{
    uint64_t bud = 0, used = 0;
    int measured = nvmtl_vk_vram_budget(&bud, &used);
    uint64_t card = nvmtl_vk_vram_bytes();
    uint64_t inv = bud / 9 * 10;
    uint64_t freev = card;
    uint64_t head = NVMTL_VRAM_HEADROOM;
    int gate;
    if (!measured || !bud) gate = (used + g_budget_delta + want + head > bud);
    else if (inv >= card) gate = 0;
    else { freev = inv > used ? inv - used : 0;

           freev = freev > g_budget_delta ? freev - g_budget_delta : 0;
           gate = (want + head > freev); }
    if (freev_out) *freev_out = freev;
    if (bud_out) *bud_out = bud;
    if (used_out) *used_out = used;
    if (measured_out) *measured_out = measured;
    return gate;
}
uint64_t nvmtl_vk_working_set(void)
{
    uint64_t b = 0, u = 0; if (nvmtl_vk_init()) return 2ull << 30;
    { static int res2 = -1; if (res2 < 0) { const char *e = getenv("NVMTL_NO_RES2"), *w = getenv("NVMTL_RES2_WS");
          res2 = !(w && w[0] == '0') && g_sparse && !(e && e[0] == '1'); }
      if (res2 && nvmtl_vk_vram_bytes()) return nvmtl_vk_vram_bytes(); }
    nvmtl_vk_vram_budget(&b, &u);
    static long long ovr = -2; if (ovr == -2) { const char *e = getenv("NVMTL_VRAM_WS_NONIMAGE_MB"); ovr = (e && *e) ? atoll(e) : -1; }
    uint64_t nonimg = ovr >= 0 ? (uint64_t)ovr << 20 : NVMTL_VRAM_NONIMAGE;
    if (u > g_vram_bytes && u - g_vram_bytes > nonimg) nonimg = u - g_vram_bytes;
    uint64_t reserve = NVMTL_VRAM_HEADROOM + nonimg;
    uint64_t room = b > reserve ? b - reserve : 0;
    { static long long pct = -2; if (pct == -2) { const char *e = getenv("NVMTL_WS_FILL_PCT"); pct = (e && *e) ? atoll(e) : 92;
          if (pct < 50 || pct > 100) pct = 92; }
      room = room / 100 * (uint64_t)pct; }
    return room > (1ull << 30) ? room : (1ull << 30);
}
/* 931d015: what recommendedMaxWorkingSetSize answers - the working-set formula above, capped at the card less a reserve
   (29% of the card by default; NVMTL_WS_RESERVE_MB sets the reserve, 0 removes the cap). The formula moves with the memory
   budget at the moment the app asks, i.e. with the desktop's footprint at that boot; upstream measured an 8 GB card
   advertising 5.7-5.8 GB on some boots and 6.4-6.5 GB on others, and a game that sized its heaps from the larger figure
   ran out of VRAM in its first scene. The cap keeps the answer stable. Not applied when residency v2 deliberately
   advertises the whole card (NVMTL_RES2_WS: it pages out what does not fit). beaab1d's NVMTL_WS_FULL is not needed: the
   fork never took the batch's whole-budget formula, its default is the formula beaab1d restored. */
static uint64_t nvmtl_vk_ws_reserve(void)
{
    static long long mb = -2;
    if (mb == -2) { const char *e = getenv("NVMTL_WS_RESERVE_MB"); mb = (e && *e) ? atoll(e) : -1; }
    if (mb >= 0) return (uint64_t)mb << 20;
    return nvmtl_vk_vram_bytes() / 100 * 29;
}
uint64_t nvmtl_vk_recommended_working_set(void)
{
    uint64_t limit = nvmtl_vk_working_set();
    const uint64_t card = nvmtl_vk_vram_bytes();
    /* Intel (ANV): the formula above keeps back NVMTL_VRAM_NONIMAGE, 2.6 GB that NVK spent outside images on the cards
       upstream measured; ANV has no such pool, and an Arc B580 (11.2 GB to Vulkan) answered 5.1 GB, so games sized their
       textures for half the card. Here: ANV's own heap budget (VK_EXT_memory_budget, what is left after everyone's use)
       less the headroom for the desktop, never more than the card less 2 GB. NVMTL_WS_INTEL=0: the formula. */
    { static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_WS_INTEL"); on = nvmtl_vk_vendor() == 0x8086 && !(e && e[0] == '0'); }
      if (on && card > (3ull << 30)) {
          uint64_t b = 0, u = 0; nvmtl_vk_vram_budget(&b, &u);
          const uint64_t head = NVMTL_VRAM_HEADROOM, top = card - (2ull << 30);
          uint64_t w = b > head ? b - head : limit;
          if (w > top) w = top;
          if (w < limit) w = limit;
          static int said; if (!said++) nvlog("working set (Intel): budget %llu MB less %llu MB headroom, at most the card %llu MB less 2048 MB -> recommending %llu MB (the formula said %llu MB)",
              (unsigned long long)(b >> 20), (unsigned long long)(head >> 20), (unsigned long long)(card >> 20), (unsigned long long)(w >> 20), (unsigned long long)(limit >> 20));
          return w;
      } }
    if (!card || limit >= card) return limit;   // residency v2's whole-card answer, or no card size
    const uint64_t reserve = nvmtl_vk_ws_reserve();
    const uint64_t cap = card > reserve ? card - reserve : 0;
    uint64_t b = 0, u = 0; nvmtl_vk_vram_budget(&b, &u);
    static int said;
    if (!said++) nvlog("working set: formula %llu MB (budget %llu MB, used %llu MB at the query), cap %llu MB (card %llu MB less %llu MB reserve) -> recommending %llu MB",
                       (unsigned long long)(limit >> 20), (unsigned long long)(b >> 20), (unsigned long long)(u >> 20),
                       (unsigned long long)(cap >> 20), (unsigned long long)(card >> 20), (unsigned long long)(reserve >> 20),
                       (unsigned long long)((cap && limit > cap ? cap : limit) >> 20));
    if (cap && reserve && limit > cap) limit = cap;
    return limit;
}
unsigned long long nvmtl_tex_objs_live(void);
unsigned long long nvmtl_tex_objs_released(void);
static void nvmtl_vk_image_census(void)
{
    if ((g_img_made & 0xFFF) == 0)
    { uint64_t bud = 0, used = 0; int m = nvmtl_vk_vram_budget(&bud, &used);
        nvlog("image accounting: VRAM images asked %llu MB, 64K-aligned %llu MB (+%llu waste); system images %llu -> %llu MB; all our allocations %llu MB (BAR1 %llu MB); NVK charges heap0 %llu MB -> NON-IMAGE %llu MB",
              (unsigned long long)(g_vram_bytes >> 20), (unsigned long long)(g_img_align >> 20),
              (unsigned long long)((g_img_align > g_vram_bytes ? g_img_align - g_vram_bytes : 0) >> 20),
              (unsigned long long)(g_sys_bytes >> 20), (unsigned long long)(g_sys_align >> 20),
              (unsigned long long)(((uint64_t)g_alloc_bytes) >> 20), (unsigned long long)(g_bar1_bytes >> 20),
              (unsigned long long)(used >> 20),
              (unsigned long long)((used > g_img_align ? used - g_img_align : 0) >> 20));
        nvlog("image census: %llu made, %llu gone, %llu live; VRAM %llu MB, system memory %llu MB (%llu spilled); card %llu MB; budget %llu MB used %llu MB (%s); MTLTexture objects %llu live, %llu released",
              (unsigned long long)g_img_made, (unsigned long long)g_img_gone, (unsigned long long)(g_img_made - g_img_gone),
              (unsigned long long)(g_vram_bytes >> 20), (unsigned long long)(g_sys_bytes >> 20), (unsigned long long)g_spilled,
              (unsigned long long)(nvmtl_vk_vram_bytes() >> 20), (unsigned long long)(bud >> 20), (unsigned long long)(used >> 20), m ? "measured" : "estimated",
              nvmtl_tex_objs_live(), nvmtl_tex_objs_released()); }
}
static VkResult nvmtl_vk_alloc_image_mem(const VkMemoryRequirements *mr, VkDeviceMemory *mem, uint32_t *sysmem, const char *what)
{
    *sysmem = 0;
    int mt = memtype(mr->memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { nvlog("vk: no device-local memory type for the %s image", what); return VK_ERROR_OUT_OF_DEVICE_MEMORY; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr->size, (uint32_t)mt };
    uint64_t bud = 0, used = 0, freev = 0; int measured = 0;
    int gate = nvmtl_vk_vram_wall(mr->size, &freev, &bud, &used, &measured);
    VkResult r = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    if (!gate) r = pvkAllocateMemory(g_dev, &mai, NULL, mem);
    else nvlog("vk: soft VRAM gate: %llu MB free on the card, this %s image wants %llu MB + %llu MB headroom -> SYSTEM MEMORY (heapUsage %llu of budget %llu MB, %s)",
               (unsigned long long)(freev >> 20), what, (unsigned long long)(mr->size >> 20),
               (unsigned long long)(NVMTL_VRAM_HEADROOM >> 20), (unsigned long long)(used >> 20),
               (unsigned long long)(bud >> 20), measured ? "measured" : "estimated");
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY && nvmtl_res2_evict_hook && !t_in_reclaim && !nvmtl_res2_evict_off()) {
        t_in_reclaim = 1;
        const uint64_t freed = nvmtl_res2_evict_hook(mr->size | (1ull << 63));
        t_in_reclaim = 0;
        if (freed) {
            mai.memoryTypeIndex = (uint32_t)mt;
            r = pvkAllocateMemory(g_dev, &mai, NULL, mem);
            g_res2_evict_calls++; if (r == VK_SUCCESS) g_res2_evict_won++;
            { static unsigned said; if (said++ < 8 || said % 4096 == 0)
                nvlog("residency (res2): evicted %llu MB on the allocating thread for a %llu MB %s image -> %s "
                      "(%llu of %llu retries won the card)",
                      (unsigned long long)(freed >> 20), (unsigned long long)(mr->size >> 20), what,
                      r == VK_SUCCESS ? "VRAM" : "still system memory",
                      (unsigned long long)g_res2_evict_won, (unsigned long long)g_res2_evict_calls); }
        }
    }
    if (r == VK_ERROR_OUT_OF_DEVICE_MEMORY) {
        int mt2 = -1;
        for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++)
            if ((mr->memoryTypeBits & (1u << i)) && !(g_memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt2 = (int)i; break; }
        if (mt2 >= 0) { mai.memoryTypeIndex = (uint32_t)mt2; r = pvkAllocateMemory(g_dev, &mai, NULL, mem); if (r == VK_SUCCESS) *sysmem = 1; }
        if (r == VK_SUCCESS) { g_spilled++;
            if (nvmtl_purge_report_hook) nvmtl_purge_report_hook(what);
            nvlog("vk: %s image SPILLED to system memory (%s): images hold %llu MB VRAM + %llu MB system, heapUsage %llu MB, %llu MB free, %llu spilled so far",
                  what, gate ? "our soft gate" : "RM refused it",
                  (unsigned long long)(g_vram_bytes >> 20), (unsigned long long)(g_sys_bytes >> 20),
                  (unsigned long long)(used >> 20), (unsigned long long)(freev >> 20),
                  (unsigned long long)g_spilled); }
        else nvlog("vk: %s image: VRAM full and no system-memory type takes it (bits 0x%x) -> %d", what, mr->memoryTypeBits, r);
    }
    return r;
}
static VkResult nvmtl_alloc_opaque_image_mem(const VkMemoryRequirements *mr, void **out, uint32_t *sysmem, const char *what)
{
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkResult result = nvmtl_vk_alloc_image_mem(mr, &memory, sysmem, what);
    if (result == VK_SUCCESS) {
        *out = memory;
        if (!*sysmem) nvmtl_vk_budget_charge(mr->size);
    }
    return result;
}
static void nvmtl_failed_image(nvk_image *image)
{
    if (image->view) pvkDestroyImageView(g_dev, (VkImageView)image->view, NULL);
    if (image->img) pvkDestroyImage(g_dev, (VkImage)image->img, NULL);
    if (image->mem && !image->placed) {
        pvkFreeMemory(g_dev, (VkDeviceMemory)image->mem, NULL);

        if (!image->sysmem) g_budget_stale = 1;
    }
    memset(image, 0, sizeof *image);
}
static void nvmtl_commit_image(nvk_image *image)
{
    if (image->mem && !image->placed) {
        uint64_t aligned = (image->alloc + 65535ull) & ~65535ull;
        g_alloc_bytes += image->alloc;
        if (image->sysmem) { g_sys_bytes += image->alloc; g_sys_align += aligned; }
        else { g_vram_bytes += image->alloc; g_img_align += aligned; }
    }
    g_img_made++;
    nvmtl_vk_image_census();
}

static int nvmtl_vk_image_create_ms_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, uint32_t mtl_type, uint32_t layers, nvk_image *out);
int nvmtl_vk_image_create_ms(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, nvk_image *out)
{
    return nvmtl_vk_image_create_ms_full(w, h, vkfmt, bpp, samples, 4, 1, out);
}
int nvmtl_vk_image_create_ms_full(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, uint32_t mtl_type, uint32_t layers, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_image_create_ms_lifetime_body(w, h, vkfmt, bpp, samples, mtl_type, layers, &pending);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
static int nvmtl_vk_image_create_ms_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, uint32_t mtl_type, uint32_t layers, nvk_image *out)
{
    if (!w || !h || !layers || (mtl_type != 4 && mtl_type != 8) || (mtl_type == 4 && layers != 1)) return -1;
    if (samples != 2 && samples != 4 && samples != 8) { nvlog("image_create: sampleCount %u is not carried (2, 4, 8)", samples); return -1; }
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->mips = 1; out->fmt = vkfmt; out->bpp = bpp ? bpp : 4; out->mtl_type = mtl_type; out->layers = layers; out->samples = samples;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, (VkFormat)vkfmt, { w, h, 1 }, 1, layers,
        (VkSampleCountFlagBits)samples, VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    nvmtl_vk_image_mutable(&ici, out);
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage(ms)");
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, out->img, &mr);
    VKCK(nvmtl_alloc_opaque_image_mem(&mr, &out->mem, &out->sysmem, "multisample"), "vkAllocateMemory(ms)");
    out->alloc = mr.size;
    VKCK(pvkBindImageMemory(g_dev, out->img, out->mem, 0), "vkBindImageMemory(ms)");
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, mtl_type == 8 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, (VkFormat)vkfmt,
        { 0, 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers } };
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView(ms)");
    nvlog("image_create: %ux%u %u-sample colour target", w, h, samples);
    return 0;
}
static int nvmtl_vk_image_create_typed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_image *out);
int nvmtl_vk_image_create_typed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_image_create_typed_lifetime_body(w, h, vkfmt, bpp, a8, mips, storage, mtl_type, layers, &pending);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
// The a8 flag of a pixel format selects a fixed swizzle of its view: 1 = A8Unorm (alpha in the red lane), 2 = Apple's private
// ARGB8 format 84 (Apple's GL asks for it for GL_BGRA + GL_UNSIGNED_INT_8_8_8_8 uploads: bytes A,R,G,B, stored as RGBA8,
// so R<-G, G<-B, B<-A, A<-R). 0 = identity.
static VkComponentMapping nvmtl_a8_mapping(int a8)
{
    if (a8 == 1) return (VkComponentMapping){ VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
    if (a8 == 2) return (VkComponentMapping){ VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R };
    return (VkComponentMapping){ 0, 0, 0, 0 };
}
/* 33ed311 (sRGB part): an sRGB image cannot be a storage image (no typed writes for sRGB formats on Xe either), so one
   made with ShaderWrite lost the storage usage and the UNORM view an app makes to write it was never writable either.
   Such an image keeps STORAGE with EXTENDED_USAGE; its own (sRGB) views drop it, a storage-capable UNORM view keeps it.
   nvk_image.storage 2 marks that: view-only storage, never the base view (t->_stex = storage == 1). */
static uint32_t nvmtl_storage_view_twin(uint32_t f)
{
    switch (f) { case 43: return 37; case 50: return 44; case 57: return 51; default: return 0; }   // *_SRGB -> *_UNORM
}
static int nvmtl_storage_view_needed(uint32_t format, int requested, int native_storage, int a8, int compressed)
{
    static int off = -1; if (off < 0) { const char *e = getenv("NVMTL_NO_SRGB_VIEW_STORAGE"); off = e && e[0] == '1'; }
    const uint32_t twin = nvmtl_storage_view_twin(format);
    return !off && requested && !native_storage && !a8 && !compressed && twin && nvmtl_vk_format_is_storage(twin);
}
static int nvmtl_vk_image_create_typed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_image *out)
{
    if (!layers || (mtl_type < 2 && h != 1) || mtl_type == 4 || mtl_type > 7
        || ((mtl_type == 0 || mtl_type == 2) && layers != 1) || (mtl_type == 5 && layers != 6) || (mtl_type == 6 && layers % 6)) {
        nvlog("image_create: textureType %u with %u layer(s) is not carried", mtl_type, layers); return -1; }
    if (nvmtl_vk_init()) return -1;
    if (!mips) mips = 1;
    VkFormat fmt = (VkFormat)vkfmt;
    const int wantStorage = storage;
    storage = storage && !a8 && nvmtl_vk_format_is_storage(vkfmt);
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->mips = mips; out->fmt = vkfmt; out->bpp = bpp ? bpp : 4;
    out->storage = storage ? 1 : 0; out->mtl_type = mtl_type; out->layers = layers;
    { uint32_t bb = 0; if (nvmtl_vk_format_block(vkfmt, &out->bw, &out->bh, &bb)) { out->bpp = bb; out->storage = 0; storage = 0; } }
    const int viewStorage = nvmtl_storage_view_needed(vkfmt, wantStorage, storage, a8, out->bw > 1);
    if (viewStorage) out->storage = 2;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, (mtl_type == 5 || mtl_type == 6) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, mtl_type == 7 ? VK_IMAGE_TYPE_3D : mtl_type < 2 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
        fmt, { w, h, mtl_type == 7 ? layers : 1 }, mips, mtl_type == 7 ? 1 : layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        (out->bw > 1 ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | ((storage || viewStorage) ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    if (viewStorage) ici.flags |= VK_IMAGE_CREATE_EXTENDED_USAGE_BIT;
    nvmtl_vk_image_mutable(&ici, out);
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage");
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, out->img, &mr);
    VKCK(nvmtl_alloc_opaque_image_mem(&mr, &out->mem, &out->sysmem, "colour"), "vkAllocateMemory(image)");
    out->alloc = mr.size;
    VKCK(pvkBindImageMemory(g_dev, out->img, out->mem, 0), "vkBindImageMemory");
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewType viewType = mtl_type == 0 ? VK_IMAGE_VIEW_TYPE_1D : mtl_type == 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : mtl_type == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                             : mtl_type == 5 ? VK_IMAGE_VIEW_TYPE_CUBE : mtl_type == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : mtl_type == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, viewType,
        fmt, swz, { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, mtl_type == 7 ? 1 : layers } };
    // the sRGB base view of a view-storage image: everything the image has but storage (33ed311)
    VkImageViewUsageCreateInfo vu = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
        (VkImageUsageFlags)(out->usage & ~(uint32_t)VK_IMAGE_USAGE_STORAGE_BIT) };
    if (viewStorage) vci.pNext = &vu;
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView");
    { static int said; if (viewStorage && !said++) nvlog("image: an sRGB texture asked for ShaderWrite keeps storage for its UNORM views (format %u; NVMTL_NO_SRGB_VIEW_STORAGE=1 off)", vkfmt); }
    return 0;
}
#include <sys/sysctl.h>
/* A GPU with no display of its own (laptops whose screen is on the Intel or AMD GPU; the kernel sets
   debug.nvaccel_headless) hands frames to that GPU's WindowServer, which reads an IOSurface from system memory and
   cannot page our VRAM copy off. Apple's two-GPU Macs copy the discrete GPU's frames the same way. So on such a GPU,
   IOSurface textures are private per process and copied out after every GPU write. */
static int nvmtl_gpu_headless(void)
{
    static int h = -1;
    /* AMD/Intel (NMXAccel): WindowServer draws into NMXGPU's framebuffer, so this device is always an app-only GPU */
    if (h < 0 && nvmtl_vk_vendor() != 0x10de) { h = 1; nvlog("AMD/Intel GPU: IOSurfaces are copied out to system memory"); }
    if (h < 0) { int v = 0; size_t n = sizeof v; h = sysctlbyname("debug.nvaccel_headless", &v, &n, NULL, 0) == 0 && v;
                 if (h) nvlog("headless GPU: the display belongs to another GPU - IOSurfaces are copied out to system memory"); }
    return h;
}
/* B580 (ANV): IOSurfaces live in one VRAM backing per surface when Mesa has the entry points, implicit sync works
   (readers and writers of the shared VRAM wait for each other's GPU jobs through the surface fences) and the kernel's
   switch debug.intelxe.surfvram allows this process (1: apps, 2: apps and WindowServer). The switch is looked at again every
   second (one GET_CAP), so it can be turned on or off while processes run: surface textures made from then on follow it,
   existing ones keep what they have. */
uint32_t g_anv_level;
int nvmtl_vk_anv_vram(void)
{
    static int prereq = -1, on, said = -1;
    static uint64_t next;
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;
    if (!g_dev) return 0;
    if (prereq < 0) {
        /* no VK_KHR_external_memory_fd needed: ANV on macOS exposes no fd sharing, but its vkAllocateMemory still takes the
           VkImportMemoryFdInfoKHR that carries the staged request (Mesa 0014) */
        prereq = panv_vram_cap && panv_vram_stage && panv_surf_dirty && g_linmod && nvmtl_vk_isync();
        if (panv_vram_cap && !prereq) nvlog("item 7 (B580): VRAM-shared IOSurfaces unavailable (DRM linear %s, implicit sync %s)",
                                            g_linmod ? "yes" : "NO", nvmtl_vk_isync() ? "yes" : "NO");
    }
    if (!prereq) return 0;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    os_unfair_lock_lock(&lk);
    if (now >= next) {
        next = now + 1000000000ull;
        uint32_t level = 0;
        panv_vram_cap(g_dev, &level);
        g_anv_level = level;
        const int is_ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        on = level >= (is_ws ? 2u : 1u);
        if (on != said) {
            said = on;
            nvlog("item 7 (B580): debug.intelxe.surfvram %u -> IOSurface textures made from now on %s", level,
                  on ? (is_ws ? "share VRAM backings for off-screen surfaces; existing scan-out allocations stay private" : "share one VRAM backing per surface across processes")
                     : "stay private per process (copies)");
        }
    }
    const int r = on;
    os_unfair_lock_unlock(&lk);
    return r;
}
int nvmtl_vk_surface_share_on(void)
{
    static int on = -1;
    static const char *e;
    if (on < 0) {
        e = getenv("NVMTL_NO_SURFACE_SHARE");
        on = !(e && e[0] == '1') && pnvk_stage_import && g_extfd && g_linmod && !nvmtl_gpu_headless();
        nvlog("item 7: IOSurface textures %s (NVK import %s, VK_KHR_external_memory_fd %s, DRM linear %s, NVMTL_NO_SURFACE_SHARE=%s)",
              on ? "live in the surface's family VRAM (one per surface, every process)" : pnvk_stage_import ? "private per process (copies)" : "on the NVK path: off (B580: see the item 7 (B580) line)",
              pnvk_stage_import ? "present" : "ABSENT", g_extfd ? "present" : "ABSENT", g_linmod ? "present" : "ABSENT", e ?: "unset");
    }
    return on || (!(e && e[0] == '1') && !pnvk_stage_import && nvmtl_vk_anv_vram());
}
static int nvmtl_vk_surface_mark_newest(uint32_t surfaceID, uint32_t plane, uint32_t flags)
{
    /* B580: IntelXe.kext SURFACE_DIRTY -> NMXAccel's valid bits, for every VRAM-shared surface whatever the switch says now
       (a surface that got its backing keeps needing its marks) */
    if (!pnvk_stage_import && panv_surf_dirty) {
        static int off = -1;
        if (off < 0) { const char *e = getenv("NVMTL_NO_SURFACE_PAGEOFF"); off = e && e[0] == '1';
            nvlog("after a GPU write, a VRAM-shared IOSurface is %s (IntelXe.kext SURFACE_DIRTY)", off ? "NOT marked (NVMTL_NO_SURFACE_PAGEOFF)" : "marked newest; a CPU lock pages it off"); }
        if (off || !g_dev) return -1;
        /* rb7f: INTELXE_SURFACE_DIRTY_DEMOTE_OK (bit 1): this plugin copies out itself when a CPU-read surface is demoted
           (IntelXe.kext answers ETIME -> VK_TIMEOUT). An older kernel refuses the flag (EINVAL -> VK_ERROR_UNKNOWN): tried
           once more without it, and when that works the flag is never sent again. NVMTL_NO_SURFACE_DEMOTE=1 never sends it. */
        static int demote = -1;
        if (demote < 0) { const char *e = getenv("NVMTL_NO_SURFACE_DEMOTE"); demote = !(e && e[0] == '1');
            nvlog("CPU-read VRAM-shared surfaces: %s", demote ? "may be demoted to copy-out (INTELXE_SURFACE_DIRTY_DEMOTE_OK)" : "never demoted (NVMTL_NO_SURFACE_DEMOTE=1)"); }
        const uint32_t base = (flags & 0x40000000u) ? 1u : 0u;
        /* test hook (no kernel change needed): NVMTL_DEMOTE_TEST=N answers every mark after the first N as demoted, so the
           copy-out path of a VRAM-shared texture can be checked on any kernel */
        static long dtest = -2; static _Atomic long dcount;
        if (dtest == -2) { const char *e = getenv("NVMTL_DEMOTE_TEST"); dtest = e && *e ? atol(e) : -1;
            if (dtest >= 0) nvlog("NVMTL_DEMOTE_TEST=%ld: marks after the first %ld are treated as demoted", dtest, dtest); }
        if (dtest >= 0 && ++dcount > dtest) return -2;
        VkResult r = panv_surf_dirty(g_dev, surfaceID, plane, base | (demote ? 2u : 0u));
        if (r == VK_ERROR_UNKNOWN && demote) {
            r = panv_surf_dirty(g_dev, surfaceID, plane, base);
            if (r == VK_SUCCESS || r == VK_NOT_READY) { demote = 0; nvlog("CPU-read VRAM-shared surfaces: this kernel has no demotion (DEMOTE_OK refused) - never demoted"); }
        }
        if (r == VK_NOT_READY) return 1;
        if (r == VK_TIMEOUT) {   /* demoted: not marked; the caller copies out into the pages from now on */
            static unsigned nd; if (nd++ < 8 || nd % 2000 == 0) nvlog("surface %u demoted by the kernel (CPU keeps reading it): copy-out from now on (%u so far)", surfaceID, nd);
            return -2;
        }
        if (r != VK_SUCCESS) { static unsigned n; if (n++ < 8 || n % 2000 == 0) nvlog("surface %u dirty refused (%d) (%u so far)", surfaceID, r, n); }
        return r == VK_SUCCESS ? 0 : -1;
    }
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_NO_SURFACE_PAGEOFF");
        on = pnvk_surface_dirty && !(e && e[0] == '1') && !nvmtl_gpu_headless();
        nvlog("after a GPU write, IOSurface VRAM is %s (NVK dirty export %s, NVMTL_NO_SURFACE_PAGEOFF=%s)",
              on ? "marked newest; a CPU lock / the display pipe pages it off (Apple's page-off)" : "copied out eagerly (old)",
              pnvk_surface_dirty ? "present" : "ABSENT", e ?: "unset");
    }
    if (!on || !g_dev) return -1;
    uint32_t st = pnvk_surface_dirty(g_dev, surfaceID, plane | flags);
    if (st == 0x80000007u) return 1;    /* NVRM_SS_BUSY: a try that found the accelerator lock taken */
    if (st) { static unsigned n; if (n++ < 8 || n % 2000 == 0) nvlog("surface %u dirty refused (%#x) - copy-out (%u so far)", surfaceID, st, n); }
    return st ? -1 : 0;
}
int nvmtl_vk_surface_dirty(uint32_t surfaceID, uint32_t plane) { return nvmtl_vk_surface_mark_newest(surfaceID, plane, 0); }
/* the same mark, given up instead of waiting when the accelerator lock is taken (NVRM_SS_PLANE_TRYLOCK; only an NVAccel
   that publishes NVAccelSurfaceWait is asked): 0 marked, 1 the lock was taken, -1 refused */
int nvmtl_vk_surface_dirty_try(uint32_t surfaceID, uint32_t plane) { return nvmtl_vk_surface_mark_newest(surfaceID, plane, 0x40000000u); }
/* AMD/Intel (B580): NMXAccel's present client, IOAccelNMXPresent (type 'NMXP', kexts/NVRM/accel/nvrm-accel.cpp), for
   WindowServer's completion marks (frame path stage 2) and its scan-out buffers (stage 3). ANV exports no NVK surface
   escape, so this is how the marks reach the kernel there; the IOAccel class prefix is what WindowServer's sandbox
   lets it open. Opened once, in WindowServer only; 0 when there is none (an older NMXAccel answers kIOReturnBadArgument). */
static io_connect_t g_nmxp;
static pthread_once_t g_nmxp_once = PTHREAD_ONCE_INIT;
static void nvmtl_nmxp_open(void)
{
    if (nvmtl_vk_vendor() == 0x10de || !getprogname() || strcmp(getprogname(), "WindowServer")) return;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return;
    io_connect_t c = 0;
    kern_return_t kr = IOServiceOpen(s, mach_task_self(), 0x4E4D5850u, &c);
    IOObjectRelease(s);
    if (kr == KERN_SUCCESS) g_nmxp = c;
    nvlog("NMXAccel present client: %s (0x%x)", kr == KERN_SUCCESS ? "open" : "NOT available - WindowServer waits for its own frames", kr);
}
static io_connect_t nvmtl_nmxp(void) { pthread_once(&g_nmxp_once, nvmtl_nmxp_open); return g_nmxp; }
// WindowServer completion mark (NVAccel nvSurfGpuEscape), on NVK's surface escape, or on Intel through NMXAccel's present
// client (the same nvSurfMark behind it): 0 = the kernel took it
uint32_t nvmtl_vk_surface_gpu_mark(uint32_t surfaceID, int pending)
{
    if (pnvk_surface_dirty && g_dev) return pnvk_surface_dirty(g_dev, surfaceID, 0x80000000u | (pending ? 1u : 0u));
    io_connect_t c = nvmtl_nmxp();
    if (!c) return 0x80000002u;
    uint64_t in[2] = { surfaceID, pending ? 1u : 0u }, out[1] = { 0x80000002u };
    uint32_t n = 1;
    if (IOConnectCallScalarMethod(c, 0, in, 2, out, &n) != KERN_SUCCESS || n < 1) return 0x80000002u;
    return (uint32_t)out[0];
}
/* Stage 3: the scan-out binding of a WindowServer surface (NMXAccel nmxScanOp). op 0 asks whether the surface may be
   bound (it is on the display pipe at the mode's size and flips are on), 1 binds the buffer at GPU address va, 2 unbinds.
   Returns the kernel's status (0 ok, 1 off, 2 not presented, 3 geometry, 4 busy, 5 not pinnable, 6 no slot, 7 not
   bound) or -1 without a present client. */
/* gpuperf phase 4: NMXAccel selector 2. newer 1: this frame of bound composite sid was not read back into its pages (the
   kernel marks the binding and the family's valid bits; 0 = taken); newer 0: page it off now if marked. -1: no client. */
int nvmtl_vk_scan_newer(uint32_t sid, int newer)
{
    io_connect_t c = nvmtl_nmxp();
    if (!c) return -1;
    uint64_t in[2] = { sid, newer ? 1u : 0u }, out[1] = { 0 };
    uint32_t n = 1;
    if (IOConnectCallScalarMethod(c, 2, in, 2, out, &n) != KERN_SUCCESS || n < 1) return -1;
    return (int)out[0];
}
/* debug.intelxe.surfvram as last read (nvmtl_vk_anv_vram, once a second): 3 = WindowServer composites skip their readback */
int nvmtl_vk_anv_level(void) { extern uint32_t g_anv_level; (void)nvmtl_vk_anv_vram(); return (int)g_anv_level; }
int nvmtl_vk_scanout_op(uint32_t op, uint32_t sid, uint64_t va, uint64_t size, uint32_t w, uint32_t h, uint32_t pitch, uint32_t *ggtt)
{
    io_connect_t c = nvmtl_nmxp();
    if (!c) return -1;
    uint64_t in[6] = { op, sid, va, size, (uint64_t)w | (uint64_t)h << 32, pitch }, out[2] = { 0, 0 };
    uint32_t n = 2;
    if (IOConnectCallScalarMethod(c, 1, in, 6, out, &n) != KERN_SUCCESS || n < 2) return -1;
    if (ggtt) *ggtt = (uint32_t)out[1];
    return (int)out[0];
}
/* A scan-out buffer: dedicated, linear (a buffer). visible = 0: plain DEVICE_LOCAL memory, allocated as a scan-out BO
   (Mesa's WSI allocate info: ANV_BO_ALLOC_SCANOUT, so DRM_XE_GEM_CREATE_FLAG_SCANOUT and the display PAT), so it stays out
   of the 1 GB CPU-visible BAR window (six of them held ~90 MB of it). A buffer is never given a compressed type (ANV
   leaves compressed types out of buffers' memoryTypeBits), and the kernel must place a scan-out BO in one physical run
   for the display to pin it; one that cannot is refused at bind (kNmxScPin) and the caller asks again with visible = 1:
   DEVICE_LOCAL | HOST_VISIBLE, which IntelXe.kext keeps in one run inside the window (DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM),
   where debug.intelxefb_thumb can also sample it. *va: its GPU address. */
struct nvmtl_wsi_memory_allocate_info { VkStructureType sType; const void *pNext; bool implicit_sync; bool dma_buf_sync_file; };   /* vk_internal_exts.h */
int nvmtl_vk_scanout_buffer(size_t size, nvk_buffer *out, uint64_t *va, int visible)
{
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out); *va = 0;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (nvmtl_opaque_CreateBuffer(g_dev, &bci, NULL, &out->buf) != VK_SUCCESS) { out->buf = NULL; return -1; }
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, (VkBuffer)out->buf, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT | (visible ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT : 0);
    /* never a compressed type: a buffer's memoryTypeBits include Xe2's flat-CCS DEVICE_LOCAL type (anv_buffer.c), the
       first plain DEVICE_LOCAL one, and the display reads such memory as raw compressed data (fix22's corrupted lock
       screen: the scan-out buffers had taken it). Only the types a linear image may take are uncompressed. */
    (void)nvmtl_vk_buffer_vram_type(0);   /* learns g_linear_types */
    const uint32_t allowed = visible ? mr.memoryTypeBits : (mr.memoryTypeBits & g_linear_types);
    int mt = -1;
    for (uint32_t i = 0; i < g_memp.memoryTypeCount && mt < 0; i++)
        if ((allowed & (1u << i)) && (g_memp.memoryTypes[i].propertyFlags & want) == want
            && (visible || !(g_memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))) mt = (int)i;
    if (mt < 0) { nvlog("scan-out: no %s memory type for the buffer", visible ? "DEVICE_LOCAL|HOST_VISIBLE" : "plain DEVICE_LOCAL"); pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); out->buf = NULL; return -1; }
    struct nvmtl_wsi_memory_allocate_info wsi = { (VkStructureType)1000001003 /* VK_STRUCTURE_TYPE_WSI_MEMORY_ALLOCATE_INFO_MESA */, NULL, false, false };
    VkMemoryDedicatedAllocateInfo ded = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, visible ? NULL : &wsi, VK_NULL_HANDLE, (VkBuffer)out->buf };
    VkMemoryAllocateFlagsInfo fl = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, &ded, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fl, mr.size, (uint32_t)mt };
    if (nvmtl_opaque_AllocateMemory(g_dev, &mai, NULL, &out->mem) != VK_SUCCESS) {
        nvlog("scan-out: vkAllocateMemory(%llu bytes, type %d) refused", (unsigned long long)mr.size, mt);
        pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); out->buf = NULL; out->mem = NULL; return -1; }
    if (pvkBindBufferMemory(g_dev, (VkBuffer)out->buf, (VkDeviceMemory)out->mem, 0) != VK_SUCCESS) {
        pvkFreeMemory(g_dev, (VkDeviceMemory)out->mem, NULL); pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL);
        out->buf = NULL; out->mem = NULL; return -1; }
    out->size = size; out->alloc = mr.size; out->mtype1 = (uint32_t)mt + 1; g_alloc_bytes += mr.size;
    { static int said; if (said++ < 2) nvlog("scan-out: buffer of %zu bytes in memory type %d (flags 0x%x; buffer types 0x%x, uncompressed 0x%x)%s",
          size, mt, g_memp.memoryTypes[mt].propertyFlags, mr.memoryTypeBits, g_linear_types, visible ? "" : ", a scan-out BO outside the BAR window"); }
    *va = nvmtl_vk_buffer_address(out);
    if (!*va) { nvmtl_vk_buffer_destroy(out); return -1; }
    return 0;
}
/* 1 when NVAccel publishes NVAccelSurfaceWait: its page-off waits for a surface WindowServer's GPU is still writing */
// NMXAccel (B580) publishes NMXSurfaceWaitAll when NVResource::pageoff waits for the plugin's mark on every surface, not
// only on those with a VRAM copy: WindowServer's CPU locks (captures) then wait for exactly the surface's pending write.
int nvmtl_vk_accel_surface_wait_all(void)
{
    static int on = -1;
    if (on >= 0) return on;
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    int v = 0;
    if (s) {
        CFTypeRef p = IORegistryEntryCreateCFProperty(s, CFSTR("NMXSurfaceWaitAll"), kCFAllocatorDefault, 0);
        if (p && CFGetTypeID(p) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)p, kCFNumberIntType, &v);
        if (p) CFRelease(p);
        IOObjectRelease(s);
    }
    on = v == 1;
    return on;
}
int nvmtl_vk_accel_surface_wait(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NVAccel"));
    if (!s) return 0;
    CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("NVAccelSurfaceWait"), kCFAllocatorDefault, 0);
    int on = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &on);
    if (v) CFRelease(v);
    IOObjectRelease(s);
    return on == 1;
}
static int nvmtl_vk_buffer_vram_type(uint32_t bits);
/* B580: the surface's VRAM backing through Mesa's staged import (create: this process will write the surface and makes
   the backing if it has none; otherwise only an existing backing is imported). The memory type is one a linear image
   accepts (the backing is uncompressed). */
static int nvmtl_vk_surface_vram_anv(uint32_t surfaceID, uint32_t plane, size_t need, int create, nvk_buffer *out,
                                     nvmtl_vram_layout *lay)
{
    const uint32_t all = g_memp.memoryTypeCount >= 32 ? 0xffffffffu : ((1u << g_memp.memoryTypeCount) - 1u);
    (void)nvmtl_vk_buffer_vram_type(all);   /* fills g_linear_types */
    const int mt = memtype(g_linear_types ? g_linear_types : all, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return -1;
    /* version 2 when the caller reads layouts (a Mesa without it refuses the stage: version 1, linear only) */
    struct nvmtl_anv_surface_vram_req q = { lay ? 2u : 1u, surfaceID, plane, create ? 1u : 0u, (uint64_t)need, 0, 0, -1,
                                            0, 0, 0, 0, 0, 0, 0 };
    if (lay && panv_vram_stage(g_dev, &q) != VK_SUCCESS) q.version = 1;
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                    nvmtl_devnull_fd(O_RDONLY | O_CLOEXEC) };
    if (imp.fd < 0) { panv_vram_stage(g_dev, NULL); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, need, (uint32_t)mt };
    VkDeviceMemory m = VK_NULL_HANDLE;
    panv_vram_stage(g_dev, &q);
    VkResult vr = pvkAllocateMemory(g_dev, &mai, NULL, &m);
    panv_vram_stage(g_dev, NULL);
    if (vr != VK_SUCCESS) {
        close(imp.fd);
        static unsigned n; if ((q.err != ENOENT && n++ < 8) || n % 1000 == 0)
            nvlog("item 7 (B580): surface %u %s refused (%d, errno %d) - private image + copies", surfaceID, create ? "VRAM backing" : "import", (int)vr, q.err);
        return -1;
    }
    /* A Tile4 backing holds whole tiles (128 B x 32 rows, 4 KiB): pitch x align32(height) bytes, more than the caller's
       linear need = bytesPerRow x height whenever the height is not a multiple of 32 (720 -> 736 rows). Learned only from
       this import (SURFACE_VRAM2), so the memory is imported again at the tiled size, which the backing (size_out) holds.
       WAS: the memory and buffer stayed at the linear size, the Tile4 image did not fit ("Tile4 pitch 5120 for 1280x720
       does not fit the 3686400-byte backing"), and WindowServer composited a private copy that the kernel detiled at
       every CPU lock (one page-off per refresh). */
    if (lay && q.version >= 2 && q.modifier && q.pitch && q.height) {
        const uint64_t tsz = (uint64_t)q.pitch * (((uint64_t)q.height + 31u) & ~31ull);
        if (q.pitch % 128u || tsz > q.size_out) {
            static unsigned nb; if (nb++ < 8)
                nvlog("item 7 (B580): surface %u: Tile4 pitch %u x %u rows needs %llu B, the backing has %llu - private",
                      surfaceID, q.pitch, q.height, (unsigned long long)tsz, (unsigned long long)q.size_out);
            pvkFreeMemory(g_dev, m, NULL); return -1;
        }
        if (tsz > need) {
            pvkFreeMemory(g_dev, m, NULL); m = VK_NULL_HANDLE;
            const uint64_t lin = need; need = (size_t)tsz;
            q.size = tsz; q.size_out = 0; q.err = -1;
            imp.fd = nvmtl_devnull_fd(O_RDONLY | O_CLOEXEC);
            if (imp.fd < 0) return -1;
            mai.allocationSize = tsz;
            panv_vram_stage(g_dev, &q);
            vr = pvkAllocateMemory(g_dev, &mai, NULL, &m);
            panv_vram_stage(g_dev, NULL);
            if (vr != VK_SUCCESS || !q.modifier || (uint64_t)q.pitch * (((uint64_t)q.height + 31u) & ~31ull) != tsz) {
                if (vr != VK_SUCCESS) close(imp.fd); else pvkFreeMemory(g_dev, m, NULL);
                nvlog("item 7 (B580): surface %u: import at the Tile4 size %llu refused (%d, errno %d) - private", surfaceID,
                      (unsigned long long)tsz, (int)vr, q.err);
                return -1;
            }
            static unsigned nr; if (nr++ < 8 || nr % 1000 == 0)
                nvlog("item 7 (B580): surface %u: Tile4 backing imported at %llu B (pitch %u x %u rows; linear need %llu)",
                      surfaceID, (unsigned long long)tsz, q.pitch, (q.height + 31u) & ~31u, (unsigned long long)lin);
        }
    }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, need,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &bci, NULL, &b) != VK_SUCCESS || pvkBindBufferMemory(g_dev, b, m, 0) != VK_SUCCESS) {
        if (b) pvkDestroyBuffer(g_dev, b, NULL);
        pvkFreeMemory(g_dev, m, NULL);
        nvlog("item 7 (B580): surface %u: a buffer over the imported VRAM was refused - private", surfaceID);
        return -1;
    }
    out->buf = b; out->mem = m; out->size = need; out->alloc = 0; out->imported = 1; out->mtype1 = (uint32_t)mt + 1;
    g_buf_made++;
    if (lay) {
        memset(lay, 0, sizeof *lay);
        if (q.version >= 2 && q.modifier) {
            lay->modifier = q.modifier; lay->pitch = q.pitch; lay->cpp = q.cpp; lay->width = q.width; lay->height = q.height;
            static unsigned t; if (t++ < 8 || t % 1000 == 0)
                nvlog("item 7 (B580): surface %u's backing is Tile4 (%ux%u, cpp %u, pitch %u): read through a Tile4 image",
                      surfaceID, q.width, q.height, q.cpp, q.pitch);
        }
    }
    static unsigned k; if (k++ < 8 || k % 1000 == 0)
        nvlog("item 7 (B580): surface %u %s its VRAM backing (%llu KB, gen %u)", surfaceID, create ? "made/imported" : "imported", (unsigned long long)(q.size_out >> 10), q.gen);
    return 0;
}
int nvmtl_vk_surface_vram_ex(uint32_t surfaceID, uint32_t plane, size_t need, int create, nvk_buffer *out)
{
    return nvmtl_vk_surface_vram_layout(surfaceID, plane, need, create, out, NULL);
}
/* lay: the caller reads tiled backings (it gets their layout); NULL: linear backings only (IntelXe.kext refuses a tiled one) */
int nvmtl_vk_surface_vram_layout(uint32_t surfaceID, uint32_t plane, size_t need, int create, nvk_buffer *out, nvmtl_vram_layout *lay)
{
    memset(out, 0, sizeof *out);
    if (lay) memset(lay, 0, sizeof *lay);
    if (!nvmtl_vk_surface_share_on() || !need) return -1;
    /* B580 level 2 allows WindowServer to create the same shared off-screen backing as other GPU writers. Captures
       then import it instead of copying through CPU pages. Existing framebuffer/scan-out allocations are refused
       by NMX_SV_OTHER_BACKING and keep their private-image path. No diagnostic file is needed; the existing
       surface-share prerequisites and debug.intelxe.surfvram gate above still apply. */
    if (!pnvk_stage_import) return nvmtl_vk_anv_vram() ? nvmtl_vk_surface_vram_anv(surfaceID, plane, need, create, out, lay) : -1;
    return nvmtl_vk_surface_vram(surfaceID, plane, need, out);
}
int nvmtl_vk_surface_vram(uint32_t surfaceID, uint32_t plane, size_t need, nvk_buffer *out)
{
    memset(out, 0, sizeof *out);
    if (!nvmtl_vk_surface_share_on() || !need) return -1;
    if (!pnvk_stage_import) return nvmtl_vk_anv_vram() ? nvmtl_vk_surface_vram_anv(surfaceID, plane, need, 1, out, NULL) : -1;
    uint32_t all = g_memp.memoryTypeCount >= 32 ? 0xffffffffu : ((1u << g_memp.memoryTypeCount) - 1u);
    int mt = memtype(all, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) return -1;
    nvmtl_vram_rec r = { 3, surfaceID, plane, 0, (uint64_t)need };
    VkImportMemoryFdInfoKHR imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
                                    nvmtl_devnull_fd(O_RDONLY | O_CLOEXEC) };
    if (imp.fd < 0) return -1;
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, need, (uint32_t)mt };
    VkDeviceMemory m = VK_NULL_HANDLE;
    pnvk_stage_import(&r);
    VkResult vr = pvkAllocateMemory(g_dev, &mai, NULL, &m);
    pnvk_stage_import(NULL);
    if (vr != VK_SUCCESS) {
        close(imp.fd);
        static unsigned n; if (n++ < 8 || n % 1000 == 0)
            nvlog("item 7: surface %u plane %u: family VRAM import refused (%d) - private image + copies (%u so far)", surfaceID, plane, (int)vr, n);
        return -1;
    }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, need,
                               VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &bci, NULL, &b) != VK_SUCCESS || pvkBindBufferMemory(g_dev, b, m, 0) != VK_SUCCESS) {
        if (b) pvkDestroyBuffer(g_dev, b, NULL);
        pvkFreeMemory(g_dev, m, NULL);
        nvlog("item 7: surface %u: a buffer over the imported VRAM was refused - private", surfaceID);
        return -1;
    }
    out->buf = b; out->mem = m; out->size = need; out->alloc = 0; out->imported = 1; out->mtype1 = (uint32_t)mt + 1;
    g_buf_made++;
    return 0;
}
int nvmtl_vk_image_create_fmt(uint32_t w, uint32_t h, int bgra, nvk_image *out) { return nvmtl_vk_image_create_mips(w, h, bgra, 1, out); }

int nvmtl_vk_image_create(uint32_t w, uint32_t h, nvk_image *out) { return nvmtl_vk_image_create_fmt(w, h, 0, out); }

#define NVMTL_NBUF   32
#define NVMTL_NTEX   128
#define NVMTL_NSAMP  32
#define NVMTL_ADDRESS_BINDING (NVMTL_NBUF + NVMTL_NTEX + NVMTL_NSAMP)
#define NVMTL_ADDRESS_BINDING_NUMBER 640
#define NVMTL_BINDING_NUMBER(i) ((i) == NVMTL_ADDRESS_BINDING ? NVMTL_ADDRESS_BINDING_NUMBER : \
                                 (i) >= NVMTL_STEX_BASE ? NVMTL_STEX_BINDING_NUMBER + ((i) - NVMTL_STEX_BASE) : (i))
/* A storage-buffer descriptor's offset must be a multiple of the device's minStorageBufferOffsetAlignment: 16 on NVK,
   which is where this was written, and 4 on ANV. WAS a fixed 16 here, so a buffer Apple's OpenCL bound at +28 or +4 got
   the dummy descriptor on the B580 and its writes and copies were lost (clpw); 4-aligned offsets now bind as they are. */
static uint32_t nvmtl_ssbo_align(void)
{
    const uint64_t a = g_lim.minStorageBufferOffsetAlignment;
    return a && a <= 256 && !(a & (a - 1)) ? (uint32_t)a : 16u;
}
#define NVMTL_SSBO_ALIGN (nvmtl_ssbo_align())
#define NVMTL_NSTEX 128
#define NVMTL_STEX_BASE (NVMTL_ADDRESS_BINDING + 1)
#define NVMTL_STEX_BINDING_NUMBER 480
#define NVMTL_NBIND (NVMTL_STEX_BASE + NVMTL_NSTEX)
#define NVMTL_DISPATCH_PC_BYTES 48
_Static_assert(NVMTL_STEX_BINDING_NUMBER == 480 && NVMTL_NSTEX == 128, "nvmtl_spirv_stex_hi() scans for this band by value");

#define NVMTL_MAX_SETS 4096
int nvmtl_vk_dual_src(void) { return g_dual; }
int nvmtl_spirv_has_index1(const void *p, size_t n)
{
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4;
    if (!p || nw < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) return 0;
        if (op == 71 && wc == 4 && w[i + 2] == 32 && w[i + 3] == 1) return 1;
        i += wc;
    }
    return 0;
}
static int nvmtl_heap_binding_refused(const void *p, size_t n, const char *what);
// 02ba248: the highest binding a module uses in the heap set (2), or -1 when it uses none: OpDecorate DescriptorSet 2
// marks the variables, OpDecorate Binding gives their bindings. Read the decorations only; nothing else is parsed.
static int nvmtl_spirv_heap_binding_max(const void *p, size_t n)
{
    const uint32_t *w = (const uint32_t *)p; const size_t nw = n / 4;
    if (!p || nw < 5 || w[0] != 0x07230203u) return -1;
    const uint32_t bound = w[3];
    uint8_t *inheap = bound && bound < (1u << 24) ? (uint8_t *)calloc(bound, 1) : NULL;
    if (!inheap) return -1;
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 34 && w[i + 3] == 2 && w[i + 1] < bound) inheap[w[i + 1]] = 1;
        i += wc;
    }
    int hi = -1;
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 1] < bound && inheap[w[i + 1]] && (int)w[i + 3] > hi) hi = (int)w[i + 3];
        i += wc;
    }
    free(inheap);
    return hi;
}
static int nvmtl_spirv_bands_ok(const void *p, size_t n, const char *what)
{
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4;
    if (!p || nw < 5 || w[0] != 0x07230203u) return 1;
    const uint32_t bound = w[3];
    uint8_t *heap = bound && bound < (1u << 24) ? (uint8_t *)calloc(bound, 1) : NULL;
    for (size_t i = 5; heap && i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 34 && w[i + 3] == 2 && w[i + 1] < bound) heap[w[i + 1]] = 1;
        i += wc;
    }
    int ok = 1;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33) {
            uint32_t id = w[i + 1], b = w[i + 3];
            int inheap = heap && id < bound && heap[id];
            if (!inheap && !(b < NVMTL_ADDRESS_BINDING || b == NVMTL_ADDRESS_BINDING_NUMBER ||
                             (b >= NVMTL_STEX_BINDING_NUMBER && b < NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX))) {
                const char *band = b >= 192 && b < 200 ? "[[color(n)]] framebuffer fetch (programmable blending)"
                                 : b >= 200 && b < 480 ? "imageblock / tile memory" : "no band this driver builds";
                static _Atomic int said;
                if (said++ < 8)
                    nvlog("vk: %s declares descriptor binding %u — %s, a band this device's set layout does not have; built "
                          "(a read that survives constant folding still stops in NVK's descriptor lowering)", what, b, band);
                break;
            }
        }
        i += wc;
    }
    free(heap);
    return ok;
}
static int nvmtl_spirv_bmask_x(const void *p, size_t n, uint64_t out[4], int skip_fetch);
static int nvmtl_spirv_bmask(const void *p, size_t n, uint64_t out[4]) { return nvmtl_spirv_bmask_x(p, n, out, 0); }
// NVMTL_DESC_MASK (on by default, =0 off): pipelines use the masks that also leave the framebuffer-fetch set out
// (skip_fetch below). With the set counted, a pipeline with framebuffer fetch had no mask, and every draw of it wrote
// all ~180 descriptors: CoreAnimation's pipelines use framebuffer fetch, so WindowServer wrote 2.2 million descriptors
// a second (~12k sets of ~180) where ~6 per set are needed.
static int nvmtl_desc_mask_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_DESC_MASK"); on = !(e && e[0] == '0'); nvlog("descriptor masks without the framebuffer-fetch set %s (NVMTL_DESC_MASK=%s)", on ? "on" : "off", e ? e : "unset"); }
    return on;
}
// skip_fetch: the framebuffer-fetch set (3: [[color(n)]] at binding 192+n, bound on its own) is left out like the heap.
// Counted, its binding 192 is no stage binding and voided the whole mask.
static int nvmtl_spirv_bmask_x(const void *p, size_t n, uint64_t out[4], int skip_fetch)
{
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4;
    memset(out, 0, 4 * sizeof *out);
    if (!p || nw < 5 || w[0] != 0x07230203u) return 0;
    // the bindless texture heap (set 2, bound once per command buffer) has bindings of its own: its binding 0 is not
    // the stage's buffer 0, and counting it put every textured draw on the slow descriptor path
    const uint32_t bound = w[3];
    uint8_t *heap = bound && bound < (1u << 24) ? (uint8_t *)calloc(bound, 1) : NULL;
    if (!heap) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) { free(heap); return 0; }
        if (op == 71 && wc == 4 && w[i + 2] == 34 && (w[i + 3] == 2 || (skip_fetch && w[i + 3] == 3)) && w[i + 1] < bound) heap[w[i + 1]] = 1;
        i += wc;
    }
    int ok = 1;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (op == 71 && wc == 4 && w[i + 2] == 33 && !(w[i + 1] < bound && heap[w[i + 1]])) {
            uint32_t b = w[i + 3];
            if (b == NVMTL_ADDRESS_BINDING_NUMBER) out[NVMTL_ADDRESS_BINDING >> 6] |= 1ull << (NVMTL_ADDRESS_BINDING & 63);
            else if (b < NVMTL_ADDRESS_BINDING) out[b >> 6] |= 1ull << (b & 63);
            else if (!(b >= NVMTL_STEX_BINDING_NUMBER && b < NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX)) { ok = 0; break; }
        }
        i += wc;
    }
    free(heap);
    return ok;
}
/* Stage 1d: which address-table entries a module reads, so a snapshot copies those instead of the whole 1.5 KB table
   (2 x 1.5 KB of fresh memory per draw was ~6 MB of writes per 2000-draw frame). The table (binding 640) is laid out as
   NVMTL_NBUF buffer addresses, NVMTL_NBUF self addresses, NVMTL_NTEX texture entries (size or bindless heap slot), one
   qword each. Returns NVMTL_TABNEED(bufs, texs) - entries [0, bufs) and texture entries [0, texs) - when every read of the
   table is an access chain with constant indices whose result is only loaded; 0 (= the whole table) for anything else:
   a dynamic index, a self address read (a pointer into the table: the shader may read any buffer entry through it), a
   pointer passed on, copied or selected, an unknown layout. A word that merely looks like one of those ids also gives 0:
   the mistakes this scan can make cost speed, never correctness. */
#define NVMTL_TABNEED(b, t) (0x80000000u | (uint32_t)(b) | ((uint32_t)(t) << 8))
#define NVMTL_TABNEED_BUFS(n) ((n) & 0xffu)
#define NVMTL_TABNEED_TEXS(n) (((n) >> 8) & 0xffu)
static int nvmtl_tabneed_covers(uint32_t have, uint32_t need)
{
    if (!have) return 1;            /* a whole table covers anything */
    if (!need) return 0;
    return NVMTL_TABNEED_BUFS(have) >= NVMTL_TABNEED_BUFS(need) && NVMTL_TABNEED_TEXS(have) >= NVMTL_TABNEED_TEXS(need);
}
static int nvmtl_tabneed_off(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NVMTL_TABLE_FULL"); v = e && e[0] == '1'; if (v) nvlog("stage 1d: NVMTL_TABLE_FULL=1 - every snapshot copies the whole address table"); }
    return v;
}
static uint32_t nvmtl_spirv_tabneed(const void *p, size_t n)
{
    const uint32_t *w = (const uint32_t *)p; const size_t nw = n / 4;
    if (nvmtl_tabneed_off() || !p || nw < 5 || w[0] != 0x07230203u) return 0;
    const uint32_t bound = w[3];
    if (!bound || bound >= (1u << 24)) return 0;
    uint8_t *k = (uint8_t *)calloc(bound, 1);          /* 1 table variable, 2 chain into it, 4 has a constant, 8 the table's pointer type */
    uint32_t *cv = (uint32_t *)calloc(bound, sizeof *cv), *stride = (uint32_t *)calloc(bound, sizeof *stride);
    uint32_t *ptee = (uint32_t *)calloc(bound, sizeof *ptee), *mem0 = (uint32_t *)calloc(bound, sizeof *mem0);
    uint32_t r = 0, bufs = 0, texs = 0, ntab = 0, tabty = 0, moff0 = 0;
    if (!k || !cv || !stride || !ptee || !mem0) goto out;
    /* pass 1: the table variable(s), constants, types, strides */
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) goto out;
        if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 3] == NVMTL_ADDRESS_BINDING_NUMBER && w[i + 1] < bound) { k[w[i + 1]] |= 1; ntab++; }
        else if (op == 71 && wc == 4 && w[i + 2] == 6 && w[i + 1] < bound) stride[w[i + 1]] = w[i + 3];
        else if (op == 72 && wc == 5 && w[i + 2] == 0 && w[i + 3] == 35 && w[i + 1] < bound) { if (w[i + 4]) mem0[w[i + 1]] = 1; }
        else if (op == 43 && wc >= 4 && w[i + 2] < bound) { k[w[i + 2]] |= 4; cv[w[i + 2]] = w[i + 3]; if (wc > 4 && w[i + 4]) cv[w[i + 2]] = 0xffffffffu; }
        else if (op == 32 && wc == 4 && w[i + 1] < bound) ptee[w[i + 1]] = w[i + 3];
        else if (op == 30 && wc >= 3 && w[i + 1] < bound) ptee[w[i + 1]] = w[i + 2];   /* struct: its member 0 */
        else if (op == 59 && wc >= 4 && w[i + 2] < bound && (k[w[i + 2]] & 1)) { if (tabty && tabty != w[i + 1]) goto out; tabty = w[i + 1]; }
        i += wc;
    }
    if (ntab == 0) { r = NVMTL_TABNEED(0, 0); goto out; }   /* no table: nothing to copy */
    if (ntab != 1 || !tabty || tabty >= bound) goto out;
    {   /* pointer -> block struct -> member 0 (offset 0) -> array with an ArrayStride */
        const uint32_t blk = ptee[tabty]; if (!blk || blk >= bound || mem0[blk]) goto out;
        const uint32_t arr = ptee[blk]; if (!arr || arr >= bound || !stride[arr]) goto out;
        moff0 = stride[arr];
    }
    /* pass 2: the chains into the table */
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if ((op == 65 || op == 66) && wc >= 4 && w[i + 3] < bound && (k[w[i + 3]] & 1)) {
            if (wc < 6 || w[i + 2] >= bound) goto out;            /* a pointer to the whole array */
            const uint32_t i0 = w[i + 4], i1 = w[i + 5];
            if (i0 >= bound || i1 >= bound || !(k[i0] & 4) || !(k[i1] & 4) || cv[i0] != 0 || cv[i1] == 0xffffffffu) goto out;
            const uint64_t lo = (uint64_t)cv[i1] * moff0, hi = lo + moff0 - 1;
            for (uint64_t q = lo / 8; q <= hi / 8; q++) {
                if (q < NVMTL_NBUF) { if (q + 1 > bufs) bufs = (uint32_t)q + 1; }
                else if (q < 2 * NVMTL_NBUF + NVMTL_NTEX && q >= 2 * NVMTL_NBUF) { if (q - 2 * NVMTL_NBUF + 1 > texs) texs = (uint32_t)(q - 2 * NVMTL_NBUF + 1); }
                else goto out;                                   /* a self address, or past the table */
            }
            k[w[i + 2]] |= 2;
        }
        i += wc;
    }
    /* pass 3: a chain result may only be loaded; the table variable only chained into */
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        switch (op) {
        case 3: case 4: case 5: case 6: case 7: case 8: case 11: case 14: case 15: case 16: case 17: case 59:
        case 71: case 72: case 317: case 330: case 5632: case 5633: break;   /* debug, mode, decoration, variable */
        case 65: case 66: if (w[i + 3] < bound && (k[w[i + 3]] & 1)) break; /* fall through */
        default: {
            const size_t from = (op == 61 && wc >= 4 && w[i + 3] < bound && (k[w[i + 3]] & 2)) ? 4 : 1;   /* OpLoad of a chain */
            for (size_t j = from; j < wc; j++) if (w[i + j] < bound && (k[w[i + j]] & 3)) goto out;
        } }
        i += wc;
    }
    r = NVMTL_TABNEED(bufs, texs);
out:
    free(k); free(cv); free(stride); free(ptee); free(mem0);
    return r;
}
enum { NVMTL_SLOT_NONE = 0, NVMTL_SLOT_BUF, NVMTL_SLOT_TEX, NVMTL_SLOT_SAMP, NVMTL_SLOT_TEXBUF  };
/* vextra: how much larger the VkBuffer is than size (a parked buffer reused smaller: nvk_buffer.vksize), else 0; kept in
   kind's padding so a slot stays 40 bytes (the classic model walks them per snapshot) */
typedef struct { uint8_t kind; uint32_t vextra; void *h; size_t off; size_t size; uint64_t address; } nvmtl_slot;
_Static_assert(sizeof(nvmtl_slot) == 40, "nvmtl_slot stays 40 bytes");
/* chg[set]: one bit per binding index that changed (0..NVMTL_NBIND-1). WAS: 4 words for 321 indices - a storage texture
   at index 256 and up (storage texture 63 and up) set a bit past the array, in the other set's word or texgen. */
#define NVMTL_CHG_WORDS ((NVMTL_NBIND + 63) / 64)
#include "nvmtl_dirty.h"   /* the reuse / was / copy decisions of nvmtl_snapshot_set_c (host-tested: dirty_model.c) */
// Every handle a slot holds is an NVK object pointer (VkBuffer, VkImageView, VkSampler, VkBufferView), and NVK reads
// through each one in vkUpdateDescriptorSets. A value that is not an 8-byte-aligned user address cannot be a live
// object, and NVK faults on it: WindowServer died a few seconds after each start on RTX 3060 machines with a buffer
// handle of 0x01006000000748c0 and a sampler handle of 0xa3 in a descriptor write. Such a slot binds the dummy instead
// and the log says whether the handle was bad when it was bound or the slot changed after binding.
static int nvmtl_handle_plausible(const void *h)
{
    const uintptr_t v = (uintptr_t)h;
    return v >= 0x10000 && v < (1ull << 47) && !(v & 7);
}
static const nvmtl_slot *nvmtl_slot_checked(const nvmtl_slot *sl, uint32_t which, uint32_t binding)
{
    if (!sl || sl->kind == NVMTL_SLOT_NONE || !sl->h || nvmtl_handle_plausible(sl->h)) return sl;
    static int said;
    if (said < 8) {
        said++;
        nvlog("vk: binding %u of set %u (kind %u) holds handle 0x%llx after it was bound, which cannot be a live object - "
              "the dummy is written instead (said up to 8 times)", binding, which, sl->kind, (unsigned long long)(uintptr_t)sl->h);
    }
    return NULL;
}
#define NVMTL_EMB_KEYMAX 480
static int nvmtl_dirty_only_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_NO_DIRTYONLY"); on = !(e && *e && *e != '0');
        nvlog("vk: dirty-only bindings %s (NVMTL_NO_DIRTYONLY=%s)", on ? "ON - unchanged descriptors are copied, unchanged argument-buffer tables skipped" : "OFF", e ? e : "unset"); }
    return on;
}
typedef struct { nvmtl_slot s[2][NVMTL_NBIND];
                 uint8_t dirty[2]; VkDescriptorSet last[2]; unsigned last_idx[2]; uint32_t last_hi[2]; uint64_t lastw[2][4];
                 uint64_t chg[2][NVMTL_CHG_WORDS]; uint64_t texgen[2]; const void *emb_fn[2]; uint64_t emb_gen[2]; uint32_t emb_n[2];
                 uint64_t emb_key[2][NVMTL_EMB_KEYMAX];
                 /* the object generation (nvmtl_objgen) the last set was built at, and the skipped table was noted at:
                    neither is trusted once a view, sampler or heap slot was given up since (nvmtl_dirty.h) */
                 uint64_t lastgen[2], emb_og[2];
                 uint32_t lastdyn[2]; void *lasttab[2];
                 uint64_t lasttb[2][4];
                 /* tab[set]: the address table's buffer addresses (0..31) and texture entries (64..191) as the slots stand,
                    kept current by every slot write once tinit[set] (nvmtl_tab_note); a snapshot copies them instead of
                    walking 160 slots (that walk was ~5% of Minecraft's GL render thread, 2026-10-29). Entries 32..63 (the
                    table's own addresses) depend on where each snapshot lands and are written per snapshot. */
                 uint64_t tab[2][2 * NVMTL_NBUF + NVMTL_NTEX]; uint8_t tinit[2];
                 uint32_t lastneed[2];   /* the entries the last set's table holds (stage 1d; 0 = all) */ } nvmtl_bindtable;   /* the texel-buffer mask the last set was written with */
/* NVMTL_LOCK_V2 (NVMTL_BTPOOL=0 turns this piece off): binding tables (~36 KB) come from a small pool, cleared by the
   thread that retires them (the execution worker) instead of calloc's clear and fresh pages on the encoding thread at the
   first setBuffer of every command buffer (11% of cbcost's main thread, sample 2026-11-09). A pooled table is all zero,
   as calloc's is. */
#define NVMTL_BT_POOL 16
static os_unfair_lock g_bt_pool_lock = OS_UNFAIR_LOCK_INIT;
static void *g_bt_pool[NVMTL_BT_POOL]; static unsigned g_bt_npool;
static int nvmtl_bt_pool_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_BTPOOL"); on = nvmtl_lock_v2() && !(e && e[0] == '0'); }
    return on;
}
static void *nvmtl_bt_get(void)
{
    if (nvmtl_bt_pool_on()) {
        void *t = NULL;
        os_unfair_lock_lock(&g_bt_pool_lock); if (g_bt_npool) t = g_bt_pool[--g_bt_npool]; os_unfair_lock_unlock(&g_bt_pool_lock);
        if (t) return t;
    }
    return calloc(1, sizeof(nvmtl_bindtable));
}
static void nvmtl_bt_put(void *t)
{
    if (!t) return;
    if (nvmtl_bt_pool_on() && __atomic_load_n(&g_bt_npool, __ATOMIC_RELAXED) < NVMTL_BT_POOL) {
        memset(t, 0, sizeof(nvmtl_bindtable));
        os_unfair_lock_lock(&g_bt_pool_lock);
        if (g_bt_npool < NVMTL_BT_POOL) { g_bt_pool[g_bt_npool++] = t; t = NULL; }
        os_unfair_lock_unlock(&g_bt_pool_lock);
    }
    free(t);
}

static VkDescriptorSetLayout g_setLayout;
#define NVMTL_MAX_POOLS 64
static VkDescriptorPool      g_descPools[NVMTL_MAX_POOLS];
/* Retired main-layout descriptor sets, kept for the next allocation instead of freed (g_desc_lock). A recycled set keeps
   its old descriptors: every binding a pipeline reads is in its mask and written again (nvmtl_snapshot_set_md), so only
   bindings no shader of that pipeline reads keep stale contents. WAS: vkFreeDescriptorSets at retire and
   vkAllocateDescriptorSets + its bzero per draw or dispatch, with a logged VK_ERROR_OUT_OF_POOL_MEMORY each time a pool
   ran dry - Apple's GL index-buffer fix-up dispatch per draw spent 21% of Minecraft's render thread there (2026-10-28).
   NVMTL_DESC_RECYCLE=0: free at retire, as before. */
#define NVMTL_DESC_RECYCLE_MAX 16384u
typedef struct { VkDescriptorPool pool; VkDescriptorSet set; uint64_t seq; } nvmtl_recycled_set;   // seq: the submission that last used it
static nvmtl_recycled_set g_recyc[NVMTL_DESC_RECYCLE_MAX]; static unsigned g_nrecyc;
static uint64_t g_recyc_hits, g_recyc_kept;
static unsigned              g_ndescPools;
static pthread_mutex_t       g_desc_lock = PTHREAD_MUTEX_INITIALIZER;
/* Descriptor sets coming back (g_desc_lock): bumped by nvmtl_cmd_retire whenever a finished command buffer's sets are
   recycled or freed to their pool, so an allocation that found every pool full can wait for them (nvmtl_desc_reclaim). */
static pthread_cond_t        g_desc_cond = PTHREAD_COND_INITIALIZER;
static uint64_t              g_desc_gen;
static _Atomic unsigned long long g_desc_reclaims, g_desc_reclaim_fail;
static unsigned              g_peak_sets, g_peak_fbs;
static nvk_buffer            g_dummyBuf;
static nvk_image             g_dummyImg;
/* 32768, not 131072: the heap's four bindings are SLOTS surface states each (64 bytes on Xe2), host-written, so the pool sits
   in the CPU-visible VRAM window (1 GB on macOS). At 131072 each process took 32 MB of it; twenty-odd Metal processes left a
   game's mapped buffers no room and they spilled to system memory (Minecraft + Sodium, 5 Nov: 137 MB over PCIe). */
#define NVMTL_BINDLESS_SLOTS      32768
#define NVMTL_BINDLESS_QUARANTINE 1024
#define NVMTL_BINDLESS_SAMPLER_SLOTS 2048   /* the translator's reflect::BINDLESS_SAMPLER_SLOTS; slot 0 stays the dummy */
static VkDescriptorSetLayout g_heapLayout; static VkDescriptorPool g_heapPool; static VkDescriptorSet g_heapSet;
static int g_heap_sampler_binding;   /* 1 = binding 1 holds samplers (NVMTL_ARGBUF_SAMPLERS=1) */
static int g_heap_storage_binding;   /* 1 = bindings 2-4 hold storage images / uniform + storage texel buffers */
static pthread_mutex_t g_heap_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t g_heap_next = 1;
static uint32_t g_heap_nbind;        /* 02ba248: how many bindings the heap layout was created with (1, 2 or 5; either model) */
static uint32_t g_heap_fifo[NVMTL_BINDLESS_SLOTS]; static uint32_t g_heap_fifo_head, g_heap_fifo_n, g_heap_live;

static nvk_image             g_dummyStex;
static nvk_image             g_dummyStexArray;   /* 939b6b5: the heap's storage dummy, a 2D ARRAY (1x1, one layer) */
/* What a heap slot's storage binding holds when no texture fills it: a null descriptor where robustness2 gives them
   (stores dropped, loads zero - the fork's rule for unbound slots), else the 2D-array dummy (the type a kernel that stores
   through a heap handle mostly declares; a store whose declared type differs from the view's is undefined), else the 2D
   one. NULL with g_null_desc means "write a null descriptor". */
static inline void *nvmtl_heap_storage_dummy(void) { return g_null_desc ? NULL : g_dummyStexArray.view ? g_dummyStexArray.view : g_dummyStex.view; }
static inline int nvmtl_heap_storage_dummy_ok(void) { return g_null_desc || nvmtl_heap_storage_dummy() != NULL; }
static inline void *nvmtl_heap_texel_dummy(void) { return g_null_desc ? NULL : g_dummyTexView; }
static VkSampler             g_dummySampler;

#define NVMTL_SAMPLER_INTERN_CAP 4096u
#define NVMTL_SAMPLER_INTERN_BUCKETS 8192u
typedef struct { uint32_t words[16]; } nvmtl_sampler_key;
typedef struct {
    nvmtl_sampler_key key;
    VkSampler handle;
    uint64_t refs;
    uint32_t state_next, handle_next;
} nvmtl_sampler_intern_entry;
static nvmtl_sampler_intern_entry g_sampler_entries[NVMTL_SAMPLER_INTERN_CAP + 1];
static uint32_t g_sampler_state_heads[NVMTL_SAMPLER_INTERN_BUCKETS];
static uint32_t g_sampler_handle_heads[NVMTL_SAMPLER_INTERN_BUCKETS];
static uint32_t g_sampler_intern_next = 1, g_sampler_intern_free, g_sampler_intern_live;
static pthread_mutex_t g_sampler_intern_lock = PTHREAD_MUTEX_INITIALIZER;

static uint32_t nvmtl_sampler_float_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof(bits));
    return bits;
}

static int nvmtl_sampler_make_key(const VkSamplerCreateInfo *s, nvmtl_sampler_key *key)
{

    if (!s || s->sType != VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO || s->pNext)
        return -1;
    *key = (nvmtl_sampler_key){{
        s->flags, s->magFilter, s->minFilter, s->mipmapMode,
        s->addressModeU, s->addressModeV, s->addressModeW,
        nvmtl_sampler_float_bits(s->mipLodBias), s->anisotropyEnable,
        nvmtl_sampler_float_bits(s->maxAnisotropy), s->compareEnable,
        s->compareOp, nvmtl_sampler_float_bits(s->minLod),
        nvmtl_sampler_float_bits(s->maxLod), s->borderColor,
        s->unnormalizedCoordinates
    }};
    return 0;
}

static uint32_t nvmtl_sampler_key_bucket(const nvmtl_sampler_key *key)
{
    uint32_t hash = 2166136261u;
    for (size_t i = 0; i < sizeof(key->words) / sizeof(key->words[0]); ++i)
        hash = (hash ^ key->words[i]) * 16777619u;
    hash ^= hash >> 16;
    return hash & (NVMTL_SAMPLER_INTERN_BUCKETS - 1);
}

static uint32_t nvmtl_sampler_handle_bucket(VkSampler handle)
{
    uint64_t bits = (uint64_t)(uintptr_t)handle;
    bits ^= bits >> 33;
    bits *= UINT64_C(0xff51afd7ed558ccd);
    bits ^= bits >> 33;
    return (uint32_t)bits & (NVMTL_SAMPLER_INTERN_BUCKETS - 1);
}

static VkResult nvmtl_sampler_acquire(const VkSamplerCreateInfo *info, VkSampler *out)
{
    if (!out) return VK_ERROR_INITIALIZATION_FAILED;
    *out = VK_NULL_HANDLE;
    nvmtl_sampler_key key;
    if (nvmtl_sampler_make_key(info, &key)) return VK_ERROR_FEATURE_NOT_PRESENT;
    const uint32_t state_bucket = nvmtl_sampler_key_bucket(&key);
    pthread_mutex_lock(&g_sampler_intern_lock);
    for (uint32_t i = g_sampler_state_heads[state_bucket]; i; i = g_sampler_entries[i].state_next) {
        nvmtl_sampler_intern_entry *entry = &g_sampler_entries[i];
        if (memcmp(&entry->key, &key, sizeof(key)) != 0) continue;
        if (entry->refs == UINT64_MAX) {
            pthread_mutex_unlock(&g_sampler_intern_lock);
            return VK_ERROR_TOO_MANY_OBJECTS;
        }
        ++entry->refs;
        *out = entry->handle;
        pthread_mutex_unlock(&g_sampler_intern_lock);
        return VK_SUCCESS;
    }
    uint32_t capacity = g_lim.maxSamplerAllocationCount;
    if (capacity > NVMTL_SAMPLER_INTERN_CAP) capacity = NVMTL_SAMPLER_INTERN_CAP;
    if (g_sampler_intern_live >= capacity) {
        pthread_mutex_unlock(&g_sampler_intern_lock);
        return VK_ERROR_TOO_MANY_OBJECTS;
    }
    VkSampler handle = VK_NULL_HANDLE;
    VkResult result = pvkCreateSampler(g_dev, info, NULL, &handle);
    if (result != VK_SUCCESS || handle == VK_NULL_HANDLE) {
        pthread_mutex_unlock(&g_sampler_intern_lock);
        return result == VK_SUCCESS ? VK_ERROR_INITIALIZATION_FAILED : result;
    }
    uint32_t index;
    if (g_sampler_intern_free) {
        index = g_sampler_intern_free;
        g_sampler_intern_free = g_sampler_entries[index].state_next;
    } else {
        index = g_sampler_intern_next++;
    }
    uint32_t handle_bucket = nvmtl_sampler_handle_bucket(handle);
    g_sampler_entries[index] = (nvmtl_sampler_intern_entry){
        .key = key, .handle = handle, .refs = 1,
        .state_next = g_sampler_state_heads[state_bucket],
        .handle_next = g_sampler_handle_heads[handle_bucket]
    };
    g_sampler_state_heads[state_bucket] = index;
    g_sampler_handle_heads[handle_bucket] = index;
    ++g_sampler_intern_live;
    *out = handle;
    pthread_mutex_unlock(&g_sampler_intern_lock);
    return VK_SUCCESS;
}

static void nvmtl_vk_bindless_sampler_forget(VkSampler vksampler);
static void nvmtl_sampler_release(VkSampler handle)
{
    if (handle == VK_NULL_HANDLE) return;
    pthread_mutex_lock(&g_sampler_intern_lock);
    uint32_t *handle_link = &g_sampler_handle_heads[nvmtl_sampler_handle_bucket(handle)];
    while (*handle_link && g_sampler_entries[*handle_link].handle != handle)
        handle_link = &g_sampler_entries[*handle_link].handle_next;
    if (!*handle_link) {
        pthread_mutex_unlock(&g_sampler_intern_lock);
        nvlog("sampler release: handle is not owned by the sampler interner");
        return;
    }
    uint32_t index = *handle_link;
    nvmtl_sampler_intern_entry *entry = &g_sampler_entries[index];
    if (--entry->refs == 0) {
        *handle_link = entry->handle_next;
        uint32_t *state_link = &g_sampler_state_heads[nvmtl_sampler_key_bucket(&entry->key)];
        while (*state_link != index)
            state_link = &g_sampler_entries[*state_link].state_next;
        *state_link = entry->state_next;

        nvmtl_vk_bindless_sampler_forget(handle);
        nvmtl_vk_objgen_bump();   // a sampler made later may get this handle's address (nvmtl_dirty.h)
        pvkDestroySampler(g_dev, handle, NULL);
        *entry = (nvmtl_sampler_intern_entry){ .state_next = g_sampler_intern_free };
        g_sampler_intern_free = index;
        --g_sampler_intern_live;
    }
    pthread_mutex_unlock(&g_sampler_intern_lock);
}


static int nvmtl_table_ubo(void)
{
    static int v = -1;
    if (v < 0) {
        (void)nvmtl_xlate_ready();
        const char *e = getenv("NVMTL_TABLE_SSBO");
        v = g_scalar_layout && xlate_lower_ex && !(e && e[0] == '1');
        nvlog("G1: address table (binding 640) is %s (NVMTL_TABLE_SSBO=%s, scalarBlockLayout %d, translator lower_ex %s)",
              v ? "a UNIFORM_BUFFER_DYNAMIC - an NVK constant bank, read with ldc" : "a STORAGE buffer (as before)", e ? e : "unset",
              g_scalar_layout, xlate_lower_ex ? "yes" : "NO");
    }
    return v;
}
static VkDescriptorType binding_type(uint32_t i)
{
    if (i == NVMTL_ADDRESS_BINDING && nvmtl_table_ubo()) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    if (i >= NVMTL_STEX_BASE) return VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    if (i < NVMTL_NBUF || i == NVMTL_ADDRESS_BINDING) return VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    if (i < NVMTL_NBUF + NVMTL_NTEX) return VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    return VK_DESCRIPTOR_TYPE_SAMPLER;
}

/* Sets in pool k: NVMTL_FIRST_SETS << k, at most NVMTL_MAX_SETS. ANV backs a pool with one CPU-mapped descriptor BO
   sized for every descriptor of every set up front (anv_CreateDescriptorPool: 289 surface states of 64 bytes and 32
   samplers per set here), and on a discrete GPU that BO lives in the CPU-visible BAR window (1 GB on macOS), where a
   mapped BO can never move again (IntelXe.kext). A 4096-set pool took 77 MB of it in every process that drew once -
   ControlCenter for 7 sets, TLauncher for 4, the AV1 decoder for 2, WindowServer for 355 (vkwin / nvmtl.log high-water,
   2026-10-29) - and some 18 Metal processes on a plain desktop filled the window. Pools now grow with the need: 256 sets
   (4.8 MB) first, doubling up to 4096 per pool, the same 64-pool ceiling. */
#define NVMTL_FIRST_SETS 256u
static uint32_t nvmtl_desc_pool_sets(unsigned k)
{
    return k >= 4 ? NVMTL_MAX_SETS : NVMTL_FIRST_SETS << k;
}
static unsigned nvmtl_desc_pools_max(void)   /* NVMTL_DESC_POOLS_MAX=1..64: a lower ceiling (tests of the full-pools path) */
{
    static unsigned n;
    if (!n) { const char *e = getenv("NVMTL_DESC_POOLS_MAX"); const int v = e && *e ? atoi(e) : NVMTL_MAX_POOLS;
        n = v < 1 ? 1 : v > NVMTL_MAX_POOLS ? NVMTL_MAX_POOLS : (unsigned)v;
        if (e) nvlog("vk: descriptor pools: at most %u (NVMTL_DESC_POOLS_MAX=%s)", n, e); }
    return n;
}
static int nvmtl_desc_pool_open(void)
{
    if (g_ndescPools >= nvmtl_desc_pools_max()) {
        nvlog("vk: descriptor pool CEILING: %u pools all live in one frame — draws are being skipped", g_ndescPools);
        return -1;
    }
    const uint32_t ns = nvmtl_desc_pool_sets(g_ndescPools);
    VkDescriptorPoolSize sizes[5] = {
        { VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, ns },
        { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, (NVMTL_NBUF + 1) * ns },
        { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE,  NVMTL_NTEX  * ns },
        { VK_DESCRIPTOR_TYPE_SAMPLER,        NVMTL_NSAMP * ns },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE,  NVMTL_NSTEX * ns } };
    VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
        VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, ns, 5, sizes };
    static const VkDescriptorType mut4[4] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER,
                                              VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
    VkMutableDescriptorTypeListEXT plists[5] = { { 0, NULL }, { 0, NULL }, { 4, mut4 }, { 0, NULL }, { 4, mut4 } };
    VkMutableDescriptorTypeCreateInfoEXT pmi = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, NULL, 5, plists };
    if (g_mutable_layout) { sizes[2].type = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; sizes[4].type = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; pci.pNext = &pmi; }
    VKCK(pvkCreateDescriptorPool(g_dev, &pci, NULL, &g_descPools[g_ndescPools]), "vkCreateDescriptorPool");
    g_ndescPools++;
    if (g_ndescPools > 1) {
        uint32_t before = 0;
        for (unsigned k = 0; k + 1 < g_ndescPools; k++) before += nvmtl_desc_pool_sets(k);
        nvlog("vk: descriptor pool %u opened (%u sets) — the open pools' %u sets (%u draws) were all live",
              g_ndescPools, ns, before, before / 2);
    }
    return 0;
}

static int g_descriptor_state;
static void nvmtl_vk_zero_dummies(void);
static int ensure_descriptors_locked(void)
{
    if (g_descriptor_state) return g_descriptor_state == 1 ? 0 : -1;
    g_descriptor_state = -1;
    /* One VkDescriptorSetLayoutBinding per binding NUMBER, 0..640, so that pBindings[n].binding == n: the bindings the
       shaders use (NVMTL_BINDING_NUMBER: buffers and textures 0-192, storage textures 480-607, the address table 640)
       have one descriptor, the numbers between have none (descriptorCount 0: reserved, takes no space).
       Why: VkMutableDescriptorTypeCreateInfoEXT::pMutableDescriptorTypeLists[i] belongs to pBindings[i] (radv and nvk
       index it so), but ANV looks it up by binding number (anv_descriptor_set.c: anv_descriptor_size_for_mutable_type(...,
       mutable_info, b, ...) with b the binding number). With the sparse layout the storage textures at 480-607 were past
       the 321 lists, so ANV sized each of them for any descriptor type - a combined image sampler, with a sampler state
       in the pool's sampler heap - and a set took 160 sampler states where the pool had room for 32 per set: every pool
       held a fifth of its sets (a 256-set pool ran out at 51), while its surface BO stayed 80% unused in the BAR window.
       With index == number both readings agree. */
    #define NVMTL_NBINDNUM (NVMTL_ADDRESS_BINDING_NUMBER + 1)
    _Static_assert(NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX <= NVMTL_ADDRESS_BINDING_NUMBER &&
                   NVMTL_ADDRESS_BINDING < NVMTL_STEX_BINDING_NUMBER, "binding numbers fit the dense layout");
    static VkDescriptorSetLayoutBinding b[NVMTL_NBINDNUM];
    for (uint32_t n = 0; n < NVMTL_NBINDNUM; n++)
        b[n] = (VkDescriptorSetLayoutBinding){ n, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 0, VK_SHADER_STAGE_ALL, NULL };
    for (uint32_t i = 0; i < NVMTL_NBIND; i++) {
        const uint32_t n = NVMTL_BINDING_NUMBER(i);
        b[n].descriptorType = binding_type(i); b[n].descriptorCount = 1;
    }
    VkDescriptorSetLayoutCreateInfo lci = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, NVMTL_NBINDNUM, b };
    static const VkDescriptorType texTypes[2] = { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER };
    static const VkDescriptorType stexTypes[2] = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER };
    static VkMutableDescriptorTypeListEXT mlists[NVMTL_NBINDNUM];
    VkMutableDescriptorTypeCreateInfoEXT mci = { VK_STRUCTURE_TYPE_MUTABLE_DESCRIPTOR_TYPE_CREATE_INFO_EXT, NULL, NVMTL_NBINDNUM, mlists };
    g_mutable_layout = g_mutable;
    if (g_mutable_layout) {
        for (uint32_t i = 0; i < NVMTL_NBINDNUM; i++) {
            mlists[i] = (VkMutableDescriptorTypeListEXT){ 0, NULL };
            if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) { b[i].descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; mlists[i] = (VkMutableDescriptorTypeListEXT){ 2, texTypes }; }
            else if (b[i].descriptorType == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE) { b[i].descriptorType = VK_DESCRIPTOR_TYPE_MUTABLE_EXT; mlists[i] = (VkMutableDescriptorTypeListEXT){ 2, stexTypes }; }
        }
        lci.pNext = &mci;
    }
    if (!g_dbuf) {   // the buffer model has no global set layout and no pools: per-pipeline layouts (nvmtl_dbuf.c)
        VKCK(pvkCreateDescriptorSetLayout(g_dev, &lci, NULL, &g_setLayout), "vkCreateDescriptorSetLayout");
        if (nvmtl_desc_pool_open()) return -1;
    }
    if (nvmtl_vk_buffer_create(256, 1, &g_dummyBuf)) return -1;
    if (nvmtl_vk_image_create_fmt(1, 1, 0, &g_dummyImg)) return -1;
    if (nvmtl_vk_image_create_ex2(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 1, 1, &g_dummyStex) || !g_dummyStex.storage) {
        nvlog("vk: NO storage dummy image (RGBA8 storage refused) — a declared-but-unbound storage texture stays unwritten");
        g_dummyStex.view = NULL;
    }
    // 939b6b5: the 2D-array storage dummy for the heap's storage binding (not needed with null descriptors)
    if (!g_null_desc && (nvmtl_vk_image_create_typed(1, 1, VK_FORMAT_R8G8B8A8_UNORM, 4, 0, 1, 1, 3, 1, &g_dummyStexArray) || !g_dummyStexArray.storage)) {
        nvlog("vk: NO 2D-array storage dummy (RGBA8 storage refused) - heap slots keep the 2D storage dummy");
        if (g_dummyStexArray.img) nvmtl_vk_image_destroy(&g_dummyStexArray);
        memset(&g_dummyStexArray, 0, sizeof g_dummyStexArray);
    }
    if (g_mutable && nvmtl_vk_texel_view_create(&g_dummyBuf, 0, 256, VK_FORMAT_R8G8B8A8_UNORM, 1, &g_dummyTexAlias, &g_dummyTexView, NULL)) {
        nvlog("vk: par3 NO texel dummy — texture buffers are turned OFF for this process (the layout stays mutable)"); g_mutable = 0; }
    /* The sampler a shader gets at a slot nothing was bound to. A Metal constexpr sampler defaults to mip_filter::none,
     * so this one reads level 0 only: with maxLod unclamped it picked a mip level, and CoreAnimation's Dock badge digit (a
     * 108x108 texture with seven never-filled levels, drawn into 46 px) sampled an empty level and vanished (I-057, N-127). */
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = VK_FILTER_LINEAR; sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST; sci.maxLod = 0.25f;
    VKCK(nvmtl_sampler_acquire(&sci, &g_dummySampler), "sampler acquire (dummy)");
    nvmtl_vk_zero_dummies();
    if (g_bindless && g_dbuf) (void)nvmtl_db_heap_create_locked();   // logs; no heap = bindless off, as below
    else if (g_bindless) {
        VkDescriptorBindingFlags hf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
                                      VK_DESCRIPTOR_BINDING_UPDATE_UNUSED_WHILE_PENDING_BIT;
        // binding 0: sampled images; binding 1: samplers, the translator's BINDLESS_SAMPLER_BINDING (a sampler an argument
        // buffer holds is read as the slot in the low word of its gpuResourceID, nvmtl_vk_bindless_sampler_slot)
        // NVMTL_NO_ARGBUF_SAMPLERS=1 leaves the sampler binding out. A device that refuses the two-binding heap gets the
        // one-binding heap (below), never none: a heap that fails to create turns every bindless texture off.
        // bindings 2-4: storage images, uniform texel buffers, storage texel buffers - the translator's
        // BINDLESS_STORAGE/UTEXEL/STEXEL_BINDING. A shader reaches a texture it writes, or a texture buffer, through
        // these; a heap without them leaves those descriptors unbound (Cyberpunk: 621 modules write storage images
        // through the heap, and every one of them drew with undefined descriptors - wrong colors).
        // NVMTL_NO_HEAP_STORAGE=1 keeps the two-binding heap.
        const char *nas = getenv("NVMTL_NO_ARGBUF_SAMPLERS"), *nhs = getenv("NVMTL_NO_HEAP_STORAGE");
        uint32_t hnb = (nas && nas[0] == '1') ? 1u : (nhs && nhs[0] == '1') ? 2u : 5u;
        const VkDescriptorBindingFlags hfs[5] = { hf, hf, hf, hf, hf };
        VkDescriptorSetLayoutBindingFlagsCreateInfo hfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, hnb, hfs };
        VkDescriptorSetLayoutBinding hb[5] = { { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
                                               { 1, VK_DESCRIPTOR_TYPE_SAMPLER, NVMTL_BINDLESS_SAMPLER_SLOTS, VK_SHADER_STAGE_ALL, NULL },
                                               { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
                                               { 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL },
                                               { 4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS, VK_SHADER_STAGE_ALL, NULL } };
        VkDescriptorSetLayoutCreateInfo hli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &hfi,
                                                VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, hnb, hb };
        VkDescriptorPoolSize hs[5] = { { VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NVMTL_BINDLESS_SLOTS }, { VK_DESCRIPTOR_TYPE_SAMPLER, NVMTL_BINDLESS_SAMPLER_SLOTS },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, NVMTL_BINDLESS_SLOTS }, { VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS },
                                       { VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NVMTL_BINDLESS_SLOTS } };
        VkDescriptorPoolCreateInfo hpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL,
                                           VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, hnb, hs };
        VkDescriptorSetLayout hl = VK_NULL_HANDLE; VkDescriptorPool hp = VK_NULL_HANDLE; VkDescriptorSet hset = VK_NULL_HANDLE;
        VkResult hr = VK_ERROR_UNKNOWN;
        for (;;) {
            hfi.bindingCount = hli.bindingCount = hpi.poolSizeCount = hnb;
            hr = pvkCreateDescriptorSetLayout(g_dev, &hli, NULL, &hl);
            if (hr == VK_SUCCESS) hr = pvkCreateDescriptorPool(g_dev, &hpi, NULL, &hp);
            if (hr == VK_SUCCESS) { VkDescriptorSetAllocateInfo hai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, hp, 1, &hl };
                                    hr = pvkAllocateDescriptorSets(g_dev, &hai, &hset); }
            if (hr == VK_SUCCESS || hnb == 1) break;
            nvlog("vk: BINDLESS heap with %u bindings refused (vk result %d) - %s", hnb, (int)hr,
                  hnb == 5 ? "two bindings instead; kernels that write a texture they read from memory are refused"
                           : "the texture-only heap instead; argument-buffer samplers read the default");
            if (hp) { pvkDestroyDescriptorPool(g_dev, hp, NULL); hp = VK_NULL_HANDLE; }
            if (hl) { pvkDestroyDescriptorSetLayout(g_dev, hl, NULL); hl = VK_NULL_HANDLE; }
            hset = VK_NULL_HANDLE; hnb = hnb == 5 ? 2u : 1u;
        }
        VkDescriptorImageInfo *fill = hr == VK_SUCCESS ? (VkDescriptorImageInfo *)malloc(sizeof *fill * NVMTL_BINDLESS_SLOTS) : NULL;
        if (fill && g_dummyImg.view) {
            for (uint32_t k = 0; k < NVMTL_BINDLESS_SLOTS; k++)
                fill[k] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, g_null_desc ? VK_NULL_HANDLE : (VkImageView)g_dummyImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
            VkWriteDescriptorSet hw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, hset, 0, 0, NVMTL_BINDLESS_SLOTS,
                                        VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, fill, NULL, NULL };
            pvkUpdateDescriptorSets(g_dev, 1, &hw, 0, NULL);
            static VkDescriptorImageInfo sfill[NVMTL_BINDLESS_SAMPLER_SLOTS];
            for (uint32_t k = 0; k < NVMTL_BINDLESS_SAMPLER_SLOTS; k++)
                sfill[k] = (VkDescriptorImageInfo){ (VkSampler)g_dummySampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            VkWriteDescriptorSet sw = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, hset, 1, 0, NVMTL_BINDLESS_SAMPLER_SLOTS,
                                        VK_DESCRIPTOR_TYPE_SAMPLER, sfill, NULL, NULL };
            if (hnb >= 2) { pvkUpdateDescriptorSets(g_dev, 1, &sw, 0, NULL); g_heap_sampler_binding = 1; }
            if (hnb == 5) g_heap_storage_binding = 1;
            g_heap_nbind = hnb;
            // a48e29b (prefill part): bindings 2-4 used to stay unwritten for every slot no texture filled; a stale or
            // foreign slot number then read an undefined storage descriptor. Every slot holds the dummies (or null
            // descriptors) at 2-4 from the start, as binding 0 always has. NVMTL_HEAP_PREFILL=0 restores the old state.
            { const char *e = getenv("NVMTL_HEAP_PREFILL"); const int prefill = !(e && e[0] == '0');
              if (hnb == 5 && prefill && nvmtl_heap_storage_dummy_ok()) {
                  for (uint32_t k = 0; k < NVMTL_BINDLESS_SLOTS; k++) fill[k] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (VkImageView)nvmtl_heap_storage_dummy(), VK_IMAGE_LAYOUT_GENERAL };
                  VkWriteDescriptorSet w2 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, hset, 2, 0, NVMTL_BINDLESS_SLOTS, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, fill, NULL, NULL };
                  pvkUpdateDescriptorSets(g_dev, 1, &w2, 0, NULL);
                  const int texel = g_null_desc || nvmtl_heap_texel_dummy();
                  VkBufferView *tf = texel ? (VkBufferView *)malloc(sizeof *tf * NVMTL_BINDLESS_SLOTS) : NULL;
                  if (tf) { for (uint32_t k = 0; k < NVMTL_BINDLESS_SLOTS; k++) tf[k] = (VkBufferView)nvmtl_heap_texel_dummy();
                            VkWriteDescriptorSet w3 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, hset, 3, 0, NVMTL_BINDLESS_SLOTS, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, tf };
                            VkWriteDescriptorSet w4 = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, hset, 4, 0, NVMTL_BINDLESS_SLOTS, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, tf };
                            pvkUpdateDescriptorSets(g_dev, 1, &w3, 0, NULL); pvkUpdateDescriptorSets(g_dev, 1, &w4, 0, NULL); free(tf); }
                  nvlog("vk: BINDLESS heap prefilled - every slot holds %s at binding 2%s (NVMTL_HEAP_PREFILL=1)",
                        g_null_desc ? "a null descriptor" : "the storage dummy", tf ? " and at 3-4" : "; no texel dummy, 3-4 stay unwritten");
              } else if (hnb == 5) nvlog("vk: BINDLESS heap bindings 2-4 left unwritten for unfilled slots (%s)", prefill ? "no storage dummy" : "NVMTL_HEAP_PREFILL=0"); }
            g_heapLayout = hl; g_heapPool = hp; g_heapSet = hset; g_heap_ready = 1;
            nvlog("vk: BINDLESS heap ready - set 2, %d sampled images, every slot the dummy until a texture asks for its gpuResourceID", NVMTL_BINDLESS_SLOTS);
        } else {
            nvlog("vk: BINDLESS heap NOT created (vk result %d, fill %s) - gpuResourceID stays a lookup token, a runtime-indexed texture reads element 0",
                  (int)hr, fill ? "ok" : "none");
        }
        free(fill);
    }

    nvlog("vk: descriptor model ready — set 0, %d bindings (buffers 0-%d, textures %d-%d, samplers %d-%d)",
          NVMTL_NBIND, NVMTL_NBUF - 1, NVMTL_NBUF, NVMTL_NBUF + NVMTL_NTEX - 1, NVMTL_NBUF + NVMTL_NTEX, NVMTL_ADDRESS_BINDING - 1);
    nvlog("vk: + address table at %d, storage textures %d-%d (dummy %s)", NVMTL_ADDRESS_BINDING_NUMBER,
          NVMTL_STEX_BINDING_NUMBER, NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX - 1, g_dummyStex.view ? "ok" : "MISSING");
    g_descriptor_state = 1;
    return 0;
}

static int ensure_descriptors(void) {
    pthread_mutex_lock(&g_desc_lock);
    int result = ensure_descriptors_locked();
    pthread_mutex_unlock(&g_desc_lock);
    return result;
}

static void nvmtl_heap_write_locked(uint32_t slot, void *view)
{
    if (g_dbuf) { nvmtl_db_heap_write_image(slot, view); return; }
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, (VkImageView)view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 0, slot, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &ii, NULL, NULL };
    pvkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
}
// A heap slot for a sampler an argument buffer carries (Metal argument buffers hold samplers by gpuResourceID). Samplers
// come from nvmtl_sampler_acquire's cache, so one VkSampler per distinct state: slots are kept for the process and shared
// by every sampler object with that state. 0 = no slot (no heap, or 2047 distinct states already placed).
static void *g_heap_samplers[NVMTL_BINDLESS_SAMPLER_SLOTS]; static uint32_t g_heap_sampler_n = 1;
/* A VkSampler is destroyed when its last sampler state goes (nvmtl_sampler_release), and a new one may get the same
   handle: its heap slot is forgotten then, else the new sampler would be found at the old slot, whose descriptor still
   holds the destroyed sampler's state. The slot is reused for the next new sampler. */
static void nvmtl_vk_bindless_sampler_forget(VkSampler vksampler)
{
    if (!vksampler) return;
    pthread_mutex_lock(&g_heap_lock);
    for (uint32_t k = 1; k < g_heap_sampler_n; k++) if (g_heap_samplers[k] == (void *)vksampler) g_heap_samplers[k] = NULL;
    pthread_mutex_unlock(&g_heap_lock);
}
uint32_t nvmtl_vk_bindless_sampler_slot(void *vksampler)
{
    if (!vksampler || ensure_descriptors() || !g_heap_ready || !g_heap_sampler_binding) return 0;
    uint32_t slot = 0, hole = 0;
    pthread_mutex_lock(&g_heap_lock);
    for (uint32_t k = 1; k < g_heap_sampler_n; k++) { if (g_heap_samplers[k] == vksampler) { slot = k; break; } if (!hole && !g_heap_samplers[k]) hole = k; }
    if (!slot && (hole || g_heap_sampler_n < NVMTL_BINDLESS_SAMPLER_SLOTS)) {
        slot = hole ? hole : g_heap_sampler_n++; g_heap_samplers[slot] = vksampler;
        if (g_dbuf) nvmtl_db_heap_write_sampler(slot, vksampler);
        else {
            VkDescriptorImageInfo ii = { (VkSampler)vksampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
            VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 1, slot, 1, VK_DESCRIPTOR_TYPE_SAMPLER, &ii, NULL, NULL };
            pvkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
        }
    }
    pthread_mutex_unlock(&g_heap_lock);
    if (!slot) { static int said; if (!said++) nvlog("vk: BINDLESS sampler heap FULL (%d states) - an argument-buffer sampler reads the default", NVMTL_BINDLESS_SAMPLER_SLOTS - 1); }
    return slot;
}
/* The distinct samplers argument buffers can name at once: the heap's sampler slots less slot 0, the dummy (the
   device's maxArgumentBufferSamplerCount). */
uint32_t nvmtl_vk_bindless_sampler_capacity(void) { return NVMTL_BINDLESS_SAMPLER_SLOTS - 1; }
uint32_t nvmtl_vk_bindless_put(void *view)
{
    if (!view || ensure_descriptors() || !g_heap_ready) return 0;
    uint32_t slot = 0, live = 0;
    pthread_mutex_lock(&g_heap_lock);
    if (g_heap_next < NVMTL_BINDLESS_SLOTS) slot = g_heap_next++;
    else if (g_heap_fifo_n > NVMTL_BINDLESS_QUARANTINE) {
        slot = g_heap_fifo[g_heap_fifo_head]; g_heap_fifo_head = (g_heap_fifo_head + 1) % NVMTL_BINDLESS_SLOTS; g_heap_fifo_n--;
    }
    if (slot) { nvmtl_heap_write_locked(slot, view); live = ++g_heap_live; }
    pthread_mutex_unlock(&g_heap_lock);
    if (!slot) { static int said; if (!said++) nvlog("vk: BINDLESS heap FULL (%d live) - this texture gets a lookup token, not a slot", NVMTL_BINDLESS_SLOTS); }
    else if ((live & 0xFFF) == 0) nvlog("vk: BINDLESS heap census - %u textures hold slots", live);
    return slot;
}
// The storage descriptor of a heap slot: the texture's identity view when it can be a storage image, else the storage
// dummy, so a slot recycled from a storage texture never keeps a dead view. No-op without bindings 2-4.
void nvmtl_vk_bindless_put_storage(uint32_t slot, void *view)
{
    if (!slot || !g_heapSet || !g_heap_storage_binding) return;
    void *v = view ? view : nvmtl_heap_storage_dummy();
    if (!v && !g_null_desc) return;
    VkDescriptorImageInfo ii = { VK_NULL_HANDLE, (VkImageView)v, VK_IMAGE_LAYOUT_GENERAL };
    VkWriteDescriptorSet w = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 2, slot, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii, NULL, NULL };
    pthread_mutex_lock(&g_heap_lock);
    pvkUpdateDescriptorSets(g_dev, 1, &w, 0, NULL);
    pthread_mutex_unlock(&g_heap_lock);
    { static int said; if (view && !said++) nvlog("vk: BINDLESS storage heap - first storage texture at slot %u", slot); }
}
// A texture buffer's heap slot: its VkBufferView at binding 3 (uniform texel, reads) and, when the view can be written,
// binding 4 (storage texel, writes). The translator reads a texture-buffer handle from memory as that slot. 0 = no slot.
uint32_t nvmtl_vk_bindless_put_texel(void *bufview, int storage)
{
    if (!bufview || ensure_descriptors() || !g_heapSet || !g_heap_storage_binding) return 0;
    uint32_t slot = 0;
    pthread_mutex_lock(&g_heap_lock);
    if (g_heap_next < NVMTL_BINDLESS_SLOTS) slot = g_heap_next++;
    else if (g_heap_fifo_n > NVMTL_BINDLESS_QUARANTINE) {
        slot = g_heap_fifo[g_heap_fifo_head]; g_heap_fifo_head = (g_heap_fifo_head + 1) % NVMTL_BINDLESS_SLOTS; g_heap_fifo_n--;
    }
    if (slot) {
        VkBufferView bv = (VkBufferView)bufview;
        VkDescriptorImageInfo di = { VK_NULL_HANDLE, (VkImageView)g_dummyImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        VkWriteDescriptorSet w[3]; uint32_t n = 0;
        if (g_dummyImg.view) w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 0, slot, 1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, &di, NULL, NULL };
        w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 3, slot, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &bv };
        if (storage) w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 4, slot, 1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &bv };
        pvkUpdateDescriptorSets(g_dev, n, w, 0, NULL);
        g_heap_live++;
    }
    pthread_mutex_unlock(&g_heap_lock);
    { static int said; if (slot && !said++) nvlog("vk: BINDLESS texel heap - first texture buffer at slot %u%s", slot, storage ? " (writable)" : ""); }
    return slot;
}
void nvmtl_vk_bindless_drop(uint32_t slot);
#define NVMTL_BL_CAP (1u << 18)
static void **g_bl_key; static uint32_t *g_bl_val; static uint32_t g_bl_n; static pthread_mutex_t g_bl_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t nvmtl_bl_hash(const void *p) { uint64_t x = (uint64_t)(uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdull; x ^= x >> 33; return (uint32_t)x & (NVMTL_BL_CAP - 1); }
static int nvmtl_bl_find_locked(const void *v) { uint32_t h = nvmtl_bl_hash(v); while (g_bl_key[h]) { if (g_bl_key[h] == v) return (int)h; h = (h + 1) & (NVMTL_BL_CAP - 1); } return -1; }
uint32_t nvmtl_bl_slot(void *view)
{
    if (!view || !g_heap_ready) return 0;
    pthread_mutex_lock(&g_bl_lock);
    if (!g_bl_key) { g_bl_key = (void **)calloc(NVMTL_BL_CAP, sizeof *g_bl_key); g_bl_val = (uint32_t *)calloc(NVMTL_BL_CAP, sizeof *g_bl_val);
        if (!g_bl_key || !g_bl_val) { free(g_bl_key); free(g_bl_val); g_bl_key = NULL; g_bl_val = NULL; pthread_mutex_unlock(&g_bl_lock); return 0; } }
    int at = nvmtl_bl_find_locked(view);
    if (at >= 0) { uint32_t s = g_bl_val[at]; pthread_mutex_unlock(&g_bl_lock); return s; }
    const int full = g_bl_n >= NVMTL_BL_CAP / 2;
    pthread_mutex_unlock(&g_bl_lock);
    if (full) { static int said; if (!said++) nvlog("vk: bindless-all map FULL - this texture reads the dummy"); return 0; }
    uint32_t s = nvmtl_vk_bindless_put(view);
    if (!s) return 0;
    pthread_mutex_lock(&g_bl_lock);
    at = nvmtl_bl_find_locked(view);
    if (at >= 0) { uint32_t won = g_bl_val[at]; pthread_mutex_unlock(&g_bl_lock); nvmtl_vk_bindless_drop(s); return won; }
    uint32_t h = nvmtl_bl_hash(view); while (g_bl_key[h]) h = (h + 1) & (NVMTL_BL_CAP - 1);
    g_bl_key[h] = view; g_bl_val[h] = s; g_bl_n++;
    pthread_mutex_unlock(&g_bl_lock);
    { static unsigned said; if (said++ < 2) nvlog("vk: bindless-all - textures reach shaders as heap slots in the per-draw table (first slot %u)", s); }
    return s;
}
static void nvmtl_bl_forget(const void *view)
{
    if (!view) return;
    pthread_mutex_lock(&g_bl_lock);
    int at = g_bl_key ? nvmtl_bl_find_locked(view) : -1;
    if (at < 0) { pthread_mutex_unlock(&g_bl_lock); return; }
    uint32_t slot = g_bl_val[at], i = (uint32_t)at, j = i;
    g_bl_key[i] = NULL; g_bl_n--;
    for (;;) {
        j = (j + 1) & (NVMTL_BL_CAP - 1);
        if (!g_bl_key[j]) break;
        uint32_t k = nvmtl_bl_hash(g_bl_key[j]);
        if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) { g_bl_key[i] = g_bl_key[j]; g_bl_val[i] = g_bl_val[j]; g_bl_key[j] = NULL; i = j; }
    }
    pthread_mutex_unlock(&g_bl_lock);
    nvmtl_vk_bindless_drop(slot);
}
static PFN_vkDestroyImageView g_bl_real_destroy_view;
static VKAPI_ATTR void VKAPI_CALL nvmtl_bl_destroy_view(VkDevice d, VkImageView v, const VkAllocationCallbacks *a)
{ nvmtl_bl_forget((const void *)v); g_bl_real_destroy_view(d, v, a); }
void nvmtl_bl_hook_destroy(void) { if (!g_bl_real_destroy_view && pvkDestroyImageView) { g_bl_real_destroy_view = pvkDestroyImageView; pvkDestroyImageView = nvmtl_bl_destroy_view; } }
/* svfix2: a dropped heap slot (its view destroyed, or a gpuResourceID given up) is rewritten - to the null descriptor,
   then handed out again - only once every submission made before the drop has finished. The heap set is created with
   UPDATE_AFTER_BIND | UPDATE_UNUSED_WHILE_PENDING | PARTIALLY_BOUND: the spec lets a pending command buffer's descriptors
   be updated only "as long as they are not dynamically used by any shader invocations", and ANV's vkUpdateDescriptorSets
   memcpys the new descriptor into the set's GPU-visible memory at once (anv_descriptor_set_write_image_view), so a
   submitted draw that still reads the slot read the NEW descriptor - the null one - and sampled zeros. WAS: rewritten at
   the drop: a VRAM-shared surface's WindowServer wrapper owns its image and view (no shared private backing), so one
   destroyed while a composite that read it was still queued blanked that composite (Minecraft 1.21.1's Apple-GL window,
   whole layer black in ~half the frames, surfvram=2, 2026-10-31). Same rule as parked buffers (nvmtl_bpark_ready): wait
   for every submission made before. */
typedef struct { uint32_t slot; uint64_t seq, t; } nvmtl_heap_defer;
static nvmtl_heap_defer *g_hdef; static uint32_t g_hdef_n, g_hdef_cap; static pthread_mutex_t g_hdef_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_hdef_total, g_hdef_drained, g_hdef_timeouts;
static void nvmtl_vk_bindless_drop_now(uint32_t slot);
/* svfix2: a VRAM-shared IOSurface wrapper's image and its imported backing are freed the same way - after every
   submission made before the wrapper went - so a draw still queued never samples memory that was just unbound (an
   unbound GPU address reads zeros on Xe as well). Its views go at once: an ANV descriptor holds a copy of the surface
   state, which stays valid while the image's memory is bound. */
typedef struct { nvk_image img; nvk_buffer buf; uint64_t seq, t; } nvmtl_img_defer;
static nvmtl_img_defer *g_idef; static uint32_t g_idef_n, g_idef_cap; static uint64_t g_idef_total;
int nvmtl_vk_defer_image_free(nvk_image *img, nvk_buffer *buf)
{
    if (g_state != 1 || !img || !img->img) return 0;
    const uint64_t seq = __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);
    if (seq <= atomic_load(&g_sub_wm)) return 0;
    nvmtl_vk_image_upload_barrier(img);   // keyed by the image's address: settled before the struct moves
    if (img->view) { pvkDestroyImageView(g_dev, (VkImageView)img->view, NULL); img->view = NULL; }   // its heap slot is deferred (above)
    int queued = 0;
    pthread_mutex_lock(&g_hdef_lock);
    if (g_idef_n == g_idef_cap) { const uint32_t cap = g_idef_cap ? g_idef_cap * 2 : 64;
        nvmtl_img_defer *n = (nvmtl_img_defer *)realloc(g_idef, sizeof *n * cap); if (n) { g_idef = n; g_idef_cap = cap; } }
    if (g_idef_n < g_idef_cap) {
        nvmtl_img_defer *e = &g_idef[g_idef_n++]; memset(e, 0, sizeof *e);
        e->img = *img; if (buf) e->buf = *buf; e->seq = seq; e->t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_idef_total++; queued = 1;
    }
    pthread_mutex_unlock(&g_hdef_lock);
    if (!queued) return 0;
    memset(img, 0, sizeof *img); if (buf) memset(buf, 0, sizeof *buf);
    return 1;
}
/* nm271 grace (upstream 8b8fa09 buffers, 235dc31 + 09b5e56 images), on the fork's submission numbers (g_sub_seq /
   g_sub_wm) instead of upstream's in-order "finished" counter. vkDestroyBuffer / vkFreeMemory release a resource's GPU
   address at once and the driver does not wait for the GPU, so a resource the app freed while a submission (or a command
   buffer already begun, which can still record its handle or address and be committed later) reads it is a page fault on
   Xe as on NVIDIA. A freed resource is held until every submission that may still read it is done:
     - buffers: on by default, NVMTL_BUFFER_GRACE=0 destroys them at once (as before); imported ones are not held;
     - images: off by default as upstream, NVMTL_IMAGE_GRACE=1 holds them. A held image's view goes at once - an ANV
       descriptor is a copy of the surface state, valid while the image's memory is bound (svfix2's rule) - so upstream's
       view grace (1a1c7ac, 60e3d40: NVK descriptor-table indices) is not needed here.
   The bound is the last submission number plus every command buffer begun and not yet retired (g_cb_live: recording,
   or submitted and not yet completed - the latter only make the bound a little high). Upstream counts begun less
   submitted less abandoned; here a prologue command buffer is begun and submitted inside another's submit, so that
   difference drifts upward for the life of the process, and every later death would be held longer and longer. An entry
   whose bound was never reached because the GPU went idle goes after 2 s of idleness. Reaped where the watermark moves (nvmtl_sub_done -> nvmtl_bindless_drain), where svfix2's deferred
   images are already freed. */
/* seq0: the last submission number when it died. Whatever was submitted by then is done once the watermark passes it;
   only a command buffer begun before the death and committed later can still read it. After 2 s past seq0 that is not
   waited for: g_cb_live can drift up (a command buffer begun and never retired), and then the bound below was never
   reached while the GPU stayed busy - Minecraft on Apple GL held every freed buffer, 130 MB/s of system memory (5 Nov). */
typedef struct { nvk_buffer b; nvk_image i; uint64_t seq, seq0, t; uint8_t image; } nvmtl_grace_ent;
static nvmtl_grace_ent *g_gr; static uint32_t g_gr_n, g_gr_cap; static pthread_mutex_t g_gr_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_gr_total, g_gr_reaped;
static __thread int t_grace_reaping;   /* the reap's own destroys go straight through */
static _Atomic int64_t g_cb_live;      /* command buffers between nvmtl_vk_cmd_begin and nvmtl_cmd_retire */
static void nvmtl_vk_buffer_destroy_body(nvk_buffer *b);
static void nvmtl_vk_image_destroy_now(nvk_image *i);
static int nvmtl_buffer_grace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_BUFFER_GRACE"); on = !(e && e[0] == '0');
        nvlog("vk: freed buffers keep their GPU address until the submissions that may still read them are done (NVMTL_BUFFER_GRACE=%d)", on); }
    return on;
}
static int nvmtl_image_grace_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_IMAGE_GRACE"); on = e && e[0] == '1';
        if (on) nvlog("vk: dead images are released only after the submissions that may still read them are done (NVMTL_IMAGE_GRACE=1)"); }
    return on;
}
static uint64_t nvmtl_grace_bound(void)
{
    const uint64_t seq = __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);
    const int64_t live = atomic_load_explicit(&g_cb_live, memory_order_relaxed);
    return seq + (live > 0 ? (uint64_t)live : 0);
}
static void nvmtl_grace_reap(void)
{
    if (!__atomic_load_n(&g_gr_n, __ATOMIC_RELAXED)) return;
    const uint64_t wm = atomic_load(&g_sub_wm), now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const int idle = wm >= __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);
    nvmtl_grace_ent done[32]; uint32_t nd = 0;
    pthread_mutex_lock(&g_gr_lock);
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_gr_n; i++) {
        const int old = now - g_gr[i].t > 2000000000ull;
        if (nd < 32 && (g_gr[i].seq <= wm || (old && (idle || g_gr[i].seq0 <= wm)))) done[nd++] = g_gr[i];
        else g_gr[k++] = g_gr[i];
    }
    g_gr_n = k; g_gr_reaped += nd;
    const uint64_t reaped = g_gr_reaped, held = g_gr_n;
    pthread_mutex_unlock(&g_gr_lock);
    { static uint64_t next; if (now >= next) { next = now + 10000000000ull;
        nvlog("vk: grace: %llu held, %llu reaped of %llu; bound %llu = submission %llu + %lld command buffers live; watermark %llu",
              (unsigned long long)held, (unsigned long long)reaped, (unsigned long long)g_gr_total, (unsigned long long)nvmtl_grace_bound(),
              (unsigned long long)__atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE), (long long)atomic_load(&g_cb_live), (unsigned long long)wm); } }
    t_grace_reaping = 1;
    for (uint32_t i = 0; i < nd; i++) { if (done[i].image) nvmtl_vk_image_destroy_now(&done[i].i); else nvmtl_vk_buffer_destroy_body(&done[i].b); }
    t_grace_reaping = 0;
    if (nd && (reaped & 0xFFF) < nd) nvlog("vk: grace: %llu freed resources destroyed after their readers finished so far, %llu held now",
                                          (unsigned long long)reaped, (unsigned long long)held);
    if (nd == 32) nvmtl_grace_reap();   // more may be due
}
/* 1 when the resource was taken (and *b / *i cleared); 0: destroy it now */
static int nvmtl_grace_defer(nvk_buffer *b, nvk_image *i)
{
    if (g_state != 1 || t_grace_reaping) return 0;
    const uint64_t bound = nvmtl_grace_bound();
    if (bound <= atomic_load(&g_sub_wm)) return 0;
    int queued = 0;
    pthread_mutex_lock(&g_gr_lock);
    if (g_gr_n == g_gr_cap) { const uint32_t cap = g_gr_cap ? g_gr_cap * 2 : 256;
        nvmtl_grace_ent *n = (nvmtl_grace_ent *)realloc(g_gr, sizeof *n * cap); if (n) { g_gr = n; g_gr_cap = cap; } }
    if (g_gr_n < g_gr_cap) {
        nvmtl_grace_ent *e = &g_gr[g_gr_n++]; memset(e, 0, sizeof *e);
        if (i) { e->i = *i; e->image = 1; } else e->b = *b;
        e->seq = bound; e->seq0 = __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE); e->t = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); g_gr_total++; queued = 1;
    }
    pthread_mutex_unlock(&g_gr_lock);
    if (!queued) return 0;
    if (i) memset(i, 0, sizeof *i); else memset(b, 0, sizeof *b);
    return 1;   // (no reap here: a caller may hold a pool lock the destroys take)
}
static void nvmtl_bindless_drain(void)
{
    nvmtl_grace_reap();
    if (g_idef_n) {
        nvmtl_img_defer done[16]; uint32_t nd = 0;
        const uint64_t wm = atomic_load(&g_sub_wm), now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        pthread_mutex_lock(&g_hdef_lock);
        uint32_t k = 0;
        for (uint32_t i = 0; i < g_idef_n; i++) {
            const int late = now - g_idef[i].t > 2000000000ull;
            if (nd < 16 && (g_idef[i].seq <= wm || late)) done[nd++] = g_idef[i]; else g_idef[k++] = g_idef[i];
        }
        g_idef_n = k;
        pthread_mutex_unlock(&g_hdef_lock);
        t_grace_reaping = 1;   // already waited for their readers: not held a second time by the grace above
        for (uint32_t i = 0; i < nd; i++) { nvmtl_vk_image_destroy(&done[i].img); if (done[i].buf.buf) nvmtl_vk_buffer_destroy(&done[i].buf); }
        t_grace_reaping = 0;
    }
    if (!g_hdef_n) return;
    uint32_t ready[256]; uint32_t nr = 0;
    const uint64_t wm = atomic_load(&g_sub_wm), now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_hdef_lock);
    uint32_t k = 0;
    for (uint32_t i = 0; i < g_hdef_n; i++) {
        const nvmtl_heap_defer e = g_hdef[i];
        // a submission that never reports done (a lost fence) must not hold a slot for good: after 2 s it is given back
        const int late = now - e.t > 2000000000ull;
        if (nr < 256 && (e.seq <= wm || late)) { ready[nr++] = e.slot; if (late && e.seq > wm) g_hdef_timeouts++; }
        else g_hdef[k++] = e;
    }
    g_hdef_n = k; g_hdef_drained += nr;
    pthread_mutex_unlock(&g_hdef_lock);
    for (uint32_t i = 0; i < nr; i++) nvmtl_vk_bindless_drop_now(ready[i]);
}
void nvmtl_vk_bindless_drop(uint32_t slot)
{
    if (!g_heap_ready || !slot || slot >= NVMTL_BINDLESS_SLOTS) return;
    nvmtl_vk_objgen_bump();   // the slot (a texture's gpuResourceID) will name another texture: skipped tables are re-read
    const uint64_t seq = __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);   // every submission so far may still read it
    if (seq > atomic_load(&g_sub_wm)) {
        int queued = 0;
        pthread_mutex_lock(&g_hdef_lock);
        if (g_hdef_n == g_hdef_cap) {
            const uint32_t cap = g_hdef_cap ? g_hdef_cap * 2 : 1024;
            nvmtl_heap_defer *n = (nvmtl_heap_defer *)realloc(g_hdef, sizeof *n * cap);
            if (n) { g_hdef = n; g_hdef_cap = cap; }
        }
        if (g_hdef_n < g_hdef_cap) { g_hdef[g_hdef_n++] = (nvmtl_heap_defer){ slot, seq, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) }; g_hdef_total++; queued = 1; }
        pthread_mutex_unlock(&g_hdef_lock);
        if (queued) {
            static uint64_t said; if (g_hdef_total >= said * 2 + 1) { said = g_hdef_total; nvlog("vk: bindless slot rewrites deferred until their readers finished: %llu so far (%llu drained, %llu after 2 s)",
                (unsigned long long)g_hdef_total, (unsigned long long)g_hdef_drained, (unsigned long long)g_hdef_timeouts); }
            return;
        }
    }
    nvmtl_vk_bindless_drop_now(slot);
}
static void nvmtl_vk_bindless_drop_now(uint32_t slot)
{
    if (!g_heap_ready || !slot || slot >= NVMTL_BINDLESS_SLOTS) return;
    pthread_mutex_lock(&g_heap_lock);
    nvmtl_heap_write_locked(slot, g_null_desc ? NULL : g_dummyImg.view);
    // 5228d78: the slot's storage-image and texel-buffer descriptors (bindings 2-4) go back to the dummies too. They stayed
    // as the dead resource left them: a slot reused after the quarantine still carried the old storage descriptor at
    // binding 2 (a new sampled texture rewrites binding 0 only), and a kernel writing through it stored into the old
    // image's memory.
    if (g_heap_storage_binding && g_heapSet) {
        VkDescriptorImageInfo si = { VK_NULL_HANDLE, (VkImageView)nvmtl_heap_storage_dummy(), VK_IMAGE_LAYOUT_GENERAL };
        VkBufferView tv = (VkBufferView)nvmtl_heap_texel_dummy();
        VkWriteDescriptorSet w[3]; uint32_t n = 0;
        if (nvmtl_heap_storage_dummy_ok()) w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 2, slot, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &si, NULL, NULL };
        if (tv || g_null_desc) { w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 3, slot, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &tv };
                                 w[n++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 4, slot, 1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &tv }; }
        if (n) pvkUpdateDescriptorSets(g_dev, n, w, 0, NULL);
    }
    g_heap_fifo[(g_heap_fifo_head + g_heap_fifo_n) % NVMTL_BINDLESS_SLOTS] = slot; g_heap_fifo_n++;
    if (g_heap_live) g_heap_live--;
    pthread_mutex_unlock(&g_heap_lock);
}

int nvmtl_vk_bind_buffer_offset(nvk_cmdbuf *c, uint32_t set, uint32_t index, size_t offset);

static uint64_t nvmtl_dummy_addr(void)
{
    // the dummy buffer lives for the process (ensure_descriptors_locked): its address is read once
    static uint64_t a;
    uint64_t v = __atomic_load_n(&a, __ATOMIC_RELAXED);
    if (!v) { v = nvmtl_vk_buffer_address(&g_dummyBuf); __atomic_store_n(&a, v, __ATOMIC_RELAXED); }
    return v;
}
/* entry i of the address table (a buffer or texture binding index) from its slot, in t->tab once the table is built */
static inline uint64_t nvmtl_tab_entry(const nvmtl_slot *s, uint32_t i)
{
    if (i < NVMTL_NBUF) return s->kind == NVMTL_SLOT_BUF ? s->address + s->off : nvmtl_dummy_addr();
    return (s->kind == NVMTL_SLOT_TEX && s->h) ? (uint64_t)s->size : 0;
}
static void nvmtl_push_table_entries(nvk_cmdbuf *c, const nvk_pipeline *p)
{
    /* Only our two disjoint stage ranges are cached. A different graphics layout
       starts fresh; compute push constants have their own stage state. Other
       graphics pushes in this plugin are sample positions/stencil [96,164),
       outside our vertex [0,96) and fragment [176,256) ranges. */
    if (c->pc_table_layout != p->layout) { c->pc_table_layout = p->layout; c->pc_table_valid = 0; }
    const nvmtl_bindtable *t = c->table;
    for (uint32_t stage = 0; stage < 2; stage++) {
        const nvmtl_pc_table *pc = &p->pc_table[stage];
        if (!pc->count) continue;
        uint64_t values[NVMTL_PC_TABLE_MAX];
        for (uint32_t k = 0; k < pc->count; k++) {
            const uint32_t idx = pc->index[k], slot = idx < NVMTL_NBUF ? idx : idx - NVMTL_NBUF;
            values[k] = t ? nvmtl_tab_entry(&t->s[stage][slot], slot) : idx < NVMTL_NBUF ? nvmtl_dummy_addr() : 0;
        }
        if ((c->pc_table_valid & (1u << stage)) && !memcmp(c->pc_table_values[stage], values, pc->count * 8)) continue;
        pvkCmdPushConstants(c->cb, p->layout, stage ? VK_SHADER_STAGE_FRAGMENT_BIT : VK_SHADER_STAGE_VERTEX_BIT,
            stage ? NVMTL_PC_TABLE_FS_OFFSET : NVMTL_PC_TABLE_VS_OFFSET, pc->count * 8, values);
        memcpy(c->pc_table_values[stage], values, pc->count * 8); c->pc_table_valid |= 1u << stage;
    }
}
static inline void nvmtl_tab_note(nvmtl_bindtable *t, uint32_t set, uint32_t i)
{
    if (!t->tinit[set] || i >= NVMTL_NBUF + NVMTL_NTEX) return;
    t->tab[set][i < NVMTL_NBUF ? i : NVMTL_NBUF + i] = nvmtl_tab_entry(&t->s[set][i], i);
}
static int nvmtl_bind_slot(nvk_cmdbuf *c, uint32_t set, uint32_t binding, uint8_t kind, void *h, size_t off)
{
    if (set > 1 || binding >= NVMTL_NBIND) return -1;
    if (kind != NVMTL_SLOT_NONE && h && !nvmtl_handle_plausible(h)) {
        static int said;
        if (said < 8) {
            said++;
            nvlog("vk: binding %u of set %u (kind %u) was given handle 0x%llx, which cannot be a live object - the slot is "
                  "left empty (said up to 8 times)", binding, set, kind, (unsigned long long)(uintptr_t)h);
        }
        kind = NVMTL_SLOT_NONE; h = NULL; off = 0;
    }
    if (!c->table) {
        c->table = nvmtl_bt_get();
        if (!c->table) { nvlog("vk: out of memory for the binding table"); return -1; }
    }
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    nvmtl_slot *sl = &t->s[set][binding];
    if (sl->kind != kind || sl->h != h || sl->off != off) {
        t->dirty[set] = 1;
        t->chg[set][binding >> 6] |= 1ull << (binding & 63);
        if (kind != NVMTL_SLOT_BUF || sl->kind != NVMTL_SLOT_BUF) t->texgen[set]++;
    }
    sl->kind = kind; sl->h = h; sl->off = off;
    nvmtl_tab_note(t, set, binding);
    return 0;
}

int nvmtl_vk_emb_same(nvk_cmdbuf *c, uint32_t set, const void *fn, const uint64_t *key, uint32_t n)
{
    nvmtl_bindtable *t = c ? (nvmtl_bindtable *)c->table : NULL;
    if (!t || set > 1 || !fn || n > NVMTL_EMB_KEYMAX || !nvmtl_dirty_only_on()) return 0;
    // the key holds the argument buffer's handles, not the objects they resolved to: a handle (a heap slot) that was given
    // up and handed to another texture, or a view replaced at the same address, reads the same key (nvmtl_objgen)
    return t->emb_fn[set] == fn && t->emb_gen[set] == t->texgen[set] && t->emb_og[set] == nvmtl_objgen()
           && t->emb_n[set] == n && !memcmp(t->emb_key[set], key, (size_t)n * 8);
}
void nvmtl_vk_emb_note(nvk_cmdbuf *c, uint32_t set, const void *fn, const uint64_t *key, uint32_t n)
{
    nvmtl_bindtable *t = c ? (nvmtl_bindtable *)c->table : NULL;
    if (!t || set > 1) return;
    if (!fn || n > NVMTL_EMB_KEYMAX) { t->emb_fn[set] = NULL; return; }
    t->emb_fn[set] = fn; t->emb_gen[set] = t->texgen[set]; t->emb_og[set] = nvmtl_objgen();
    t->emb_n[set] = n; memcpy(t->emb_key[set], key, (size_t)n * 8);
}
int nvmtl_vk_bind_buffer_offset(nvk_cmdbuf *c, uint32_t set, uint32_t index, size_t offset)
{
    if (set > 1 || index >= NVMTL_NBUF || !c->table) return -1;
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    if (t->s[set][index].kind != NVMTL_SLOT_BUF) return -1;
    if (offset >= t->s[set][index].size) { nvlog("buffer offset %zu exceeds buffer length %zu", offset, t->s[set][index].size); return -1; }
    if (t->s[set][index].off != offset) { t->dirty[set] = 1; t->chg[set][index >> 6] |= 1ull << (index & 63); }
    t->s[set][index].off = offset;
    nvmtl_tab_note(t, set, index);
    return 0;
}

#define NVMTL_TABLES_PER_PAGE 256
#define NVMTL_TABLE_BYTES ((2 * NVMTL_NBUF + NVMTL_NTEX) * sizeof(uint64_t))
/* addr: buffer's device address, read once when the page is made (a kept page keeps its buffer, so it stays valid) */
/* udesc/uvalid (descriptor-buffer model): the descriptor of each of the page's tables, made once per page (a kept page
   keeps them: the table addresses do not move) */
typedef struct nvmtl_address_page { nvk_buffer buffer; struct nvmtl_address_page *next; uint64_t addr;
                                    uint8_t *udesc; uint64_t uvalid[(256 + 63) / 64]; } nvmtl_address_page;
#define NVMTL_KEEP 256
static pthread_mutex_t g_keep_lock = PTHREAD_MUTEX_INITIALIZER;
static VkCommandPool g_keep_pool[NVMTL_KEEP]; static unsigned g_keep_npool;
static nvmtl_address_page *g_keep_page[NVMTL_KEEP]; static unsigned g_keep_npage;
// Set once a process has lost NVMTL_WIT_LOST_MAX witnesses: reuse stops and every pool is destroyed after its fence
// signals, so a witness that keeps going missing costs speed, never memory. Each lost witness has already cost its
// submit thread the verdict's wait (1 s), so the cap is small.
#define NVMTL_WIT_LOST_MAX 8
static int g_keep_retired;
static int nvmtl_keep_on(void)
{
    static int on = -1;
    if (__atomic_load_n(&g_keep_retired, __ATOMIC_ACQUIRE)) return 0;
    if (on < 0) {
        const char *e = getenv("NVMTL_POOL_REUSE");
        on = (!e || e[0] == '1') && getenv("NVMTL_NO_POOL_REUSE") == NULL;
        if (on) nvlog("vk: pool reuse ON (witness-guarded default) - empty command pools and address pages kept (<= %d), reused only on THE WITNESS", NVMTL_KEEP);
        else nvlog("vk: pool reuse OFF by environment - command pools and address pages are created and destroyed per commit");
    }
    return on;
}
static VkCommandPool nvmtl_keep_pool_pop(void)
{
    VkCommandPool p = VK_NULL_HANDLE;
    if (!nvmtl_keep_on()) return p;
    pthread_mutex_lock(&g_keep_lock); if (g_keep_npool) p = g_keep_pool[--g_keep_npool]; pthread_mutex_unlock(&g_keep_lock);
    return p;
}
static int nvmtl_keep_pool_push(VkCommandPool p)
{
    int kept = 0;
    if (!nvmtl_keep_on()) return 0;
    pthread_mutex_lock(&g_keep_lock); if (g_keep_npool < NVMTL_KEEP) { g_keep_pool[g_keep_npool++] = p; kept = 1; } pthread_mutex_unlock(&g_keep_lock);
    return kept;
}
static nvmtl_address_page *nvmtl_keep_page_pop(void)
{
    nvmtl_address_page *p = NULL;
    if (!nvmtl_keep_on()) return p;
    pthread_mutex_lock(&g_keep_lock); if (g_keep_npage) p = g_keep_page[--g_keep_npage]; pthread_mutex_unlock(&g_keep_lock);
    return p;
}
static int nvmtl_keep_page_push(nvmtl_address_page *p)
{
    int kept = 0;
    if (!nvmtl_keep_on()) return 0;
    pthread_mutex_lock(&g_keep_lock); if (g_keep_npage < NVMTL_KEEP) { p->next = NULL; g_keep_page[g_keep_npage++] = p; kept = 1; } pthread_mutex_unlock(&g_keep_lock);
    return kept;
}
#define NVMTL_WIT_SLOTS 4096u
#define NVMTL_WSEQ_BLIND 0xFFFFFFFFu
/* The fence signalled but the witness never landed: the GPU is done with the command buffer, but its pool and address
 * pages are not certified for reuse, so retire leaks them instead of keeping or destroying them. */
#define NVMTL_WSEQ_LOST 0xFFFFFFFEu
static nvk_buffer g_wit; static int g_wit_state;
static uint32_t g_wit_seq;
static uint64_t g_wit_ok, g_wit_waited, g_wit_leak, g_wit_blind, g_wit_ctl_run, g_wit_ctl_fired, g_wit_lost, g_wit_lapped;
static int nvmtl_wit_ready(void)
{
    int st = __atomic_load_n(&g_wit_state, __ATOMIC_ACQUIRE);
    if (st) return st > 0;
    pthread_mutex_lock(&g_keep_lock);
    st = g_wit_state;
    if (!st) {
        if (nvmtl_vk_buffer_create(NVMTL_WIT_SLOTS * 4, 1, &g_wit) == 0 && g_wit.map) { memset(g_wit.map, 0, NVMTL_WIT_SLOTS * 4); st = 1; }
        else { nvmtl_vk_buffer_destroy(&g_wit); st = -1;
               nvlog("vk: keep witness buffer REFUSED - command pools and address pages go back to create/destroy per commit"); }
        __atomic_store_n(&g_wit_state, st, __ATOMIC_RELEASE);
    }
    pthread_mutex_unlock(&g_keep_lock);
    return st > 0;
}
static uint32_t nvmtl_wit_read(uint32_t s) { return ((volatile uint32_t *)g_wit.map)[s % NVMTL_WIT_SLOTS]; }
static uint32_t nvmtl_wit_record(nvk_cmdbuf *c)
{
    /* A command buffer that ran compiled machine code is never certified for reuse: the program, its
     * constant bank and its launch descriptor come from this pool's upload memory outside what the
     * witness proves finished, so a kept pool could be overwritten while the GPU still reads them. */
    if (c->in_rp || c->direct_sass || !nvmtl_wit_ready()) { __atomic_add_fetch(&g_wit_blind, 1, __ATOMIC_RELAXED); return NVMTL_WSEQ_BLIND; }
    uint32_t s;
    do s = __atomic_add_fetch(&g_wit_seq, 1, __ATOMIC_RELAXED); while (s == 0 || s >= NVMTL_WSEQ_LOST);
    // NVMTL_WIT_SKIP_EVERY=N leaves every Nth witness unwritten: the test that a lost witness costs one pool, not the process
    static long skip_every = -1; if (skip_every < 0) { const char *e = getenv("NVMTL_WIT_SKIP_EVERY"); skip_every = e ? atol(e) : 0; }
    /* VK_AMD_buffer_marker: the marker is written once every earlier command has passed ALL_COMMANDS, in the order the
       spec gives markers against prior work, so it needs neither barrier of the fill below (two full pipeline drains
       per command buffer; the fence wait that precedes the host read makes the coherent write visible).
       NVMTL_WIT_FILL=1 keeps the fill. */
    static int fill = -1; if (fill < 0) { const char *e = getenv("NVMTL_WIT_FILL"); fill = (e && e[0] == '1') || !pvkCmdWriteBufferMarker2AMD; }
    if (!fill) {
        if (!(skip_every > 0 && s % (uint32_t)skip_every == 0))
            pvkCmdWriteBufferMarker2AMD(c->cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, (VkBuffer)g_wit.buf, (VkDeviceSize)(s % NVMTL_WIT_SLOTS) * 4, s);
        return s;
    }
    VkMemoryBarrier toT = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &toT, 0, NULL, 0, NULL);
    if (!(skip_every > 0 && s % (uint32_t)skip_every == 0))
        pvkCmdFillBuffer(c->cb, (VkBuffer)g_wit.buf, (VkDeviceSize)(s % NVMTL_WIT_SLOTS) * 4, 4, s);
    VkMemoryBarrier toH = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toH, 0, NULL, 0, NULL);
    return s;
}
static void nvmtl_wit_control(uint32_t s)
{
    static int on = -1; if (on < 0) { const char *e = getenv("NVMTL_KEEP_WITNESS_CONTROL"); on = e && e[0] == '1'; }
    if (!on || s == 0 || s >= NVMTL_WSEQ_LOST) return;
    __atomic_add_fetch(&g_wit_ctl_run, 1, __ATOMIC_RELAXED);
    if (nvmtl_wit_read(s) != s) __atomic_add_fetch(&g_wit_ctl_fired, 1, __ATOMIC_RELAXED);
    else nvlog("vk: keep witness CONTROL DID NOT FIRE - slot already held %u before its submit; the witness proves nothing", s);
}
static int nvmtl_keep_verdict(uint32_t s)
{
    if (!nvmtl_keep_on() || s == NVMTL_WSEQ_BLIND) return 0;
    if (s == 0) return 1;
    if (s == NVMTL_WSEQ_LOST) return -1;
    if (g_state != 1 || g_wit_state <= 0) return -1;
    if (nvmtl_wit_read(s) == s) { __atomic_add_fetch(&g_wit_ok, 1, __ATOMIC_RELAXED); return 1; }
    // A later lap of the same slot: a command buffer submitted NVMTL_WIT_SLOTS (or a multiple) after this one wrote it, on
    // the same in-order queue, so this one finished long ago. Without this, a verdict asked that late waited the full
    // NVMTL_WIT_WAIT_MS below and counted the pool as lost, and after NVMTL_WIT_LOST_MAX of them pool reuse was off for
    // the whole process (WindowServer then made RM mappings for every command buffer and ran at ~12 fps).
    { const uint32_t v = nvmtl_wit_read(s), d = v - s;
      if (v && v < NVMTL_WSEQ_LOST && d && d < 0x80000000u && d % NVMTL_WIT_SLOTS == 0) {
          const unsigned long long n = __atomic_add_fetch(&g_wit_lapped, 1, __ATOMIC_RELAXED);
          if (n == 1 || (n & 0xFF) == 0) nvlog("vk: keep witness %u checked after its slot was reused by %u (%llu so far): finished, not lost", s, v, n);
          return 1; } }
    static long wait_ms = -1; if (wait_ms < 0) { const char *e = getenv("NVMTL_WIT_WAIT_MS"); wait_ms = e ? atol(e) : 1000; }
    const uint64_t w0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    for (;;) {
        const uint32_t v = nvmtl_wit_read(s);
        const uint64_t us = (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - w0) / 1000;
        if (v == s) {
            if (!__atomic_fetch_add(&g_wit_waited, 1, __ATOMIC_RELAXED))
                nvlog("vk: keep witness %u landed only %llu us AFTER the flush returned - the flush is not synchronous any more", s, (unsigned long long)us);
            return 1;
        }
        if (us >= (uint64_t)wait_ms * 1000) {
            if (!__atomic_fetch_add(&g_wit_leak, 1, __ATOMIC_RELAXED))
                nvlog("vk: keep witness %u NOT seen after %ld ms (slot holds %u) - pool and pages LEAKED, not reused", s, wait_ms, v);
            return -1;
        }
        if (us < 1000) sched_yield(); else usleep(50);
    }
}

#include <os/lock.h>
/* NVMTL_HITCH_MS=<ms>: a frame (present to present: CGLFlushDrawable's -[NSCGLSurface synchronize], or a Metal drawable's
   present) that took longer than that is logged with what the process did in it: Vulkan memory allocations and frees,
   image creates and destroys, pipeline and shader-module creations, buffer parks, reuses and evictions, AIR->SPIR-V
   translations and spvcache misses, surface-in/out, submits, the longest fence wait, waitUntilCompleted and
   waitUntilScheduled, and the GPU time of the command buffers that finished in it (sum and longest). Unset: every hook is
   one test of a cached int and the Vulkan entry points are not wrapped. Lines go to the plugin log as "HITCH ...". */
static const char *const g_hitch_name[NVH_N] = { "memalloc", "memfree", "imgnew", "imgdel", "pipeline", "shadermod", "translate",
    "cachemiss", "park", "reuse", "evict", "surfout", "surfin", "submit", "fencewait", "waitcompleted", "waitscheduled", "gpu", "donelag",
    "eventwait", "reswait", "surffold", "staleout", "outoforder", "xlatesamp", "memalloc.vram<1M", "memalloc.vram>=1M", "memalloc.host", "memalloc.import",
    "cmd.renderpass", "cmd.dispatch", "cmd.draw", "cmd.barrier", "cmd.imagebarrier", "cmd.transfer", "cmd.compute<->render" };
static _Atomic uint64_t g_hitch_n[NVH_N], g_hitch_ns[NVH_N], g_hitch_bytes[NVH_N], g_hitch_max[NVH_N];
int nvmtl_hitch_ms(void)
{
    if (g_hitch_ms < 0) { const char *e = getenv("NVMTL_HITCH_MS"); const int v = e && *e ? atoi(e) : 0;
        g_hitch_ms = v > 0 ? v : 0;
        if (g_hitch_ms) nvlog("hitch log: frames longer than %d ms are logged with what ran in them (NVMTL_HITCH_MS)", g_hitch_ms); }
    return g_hitch_ms;
}
uint64_t nvmtl_hitch_t0(void) { return nvmtl_hitch_ms() ? clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) : 0; }
void nvmtl_hitch_add(int what, uint64_t t0, uint64_t bytes)
{
    if (!t0 || what < 0 || what >= NVH_N) return;
    const uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0;
    atomic_fetch_add_explicit(&g_hitch_n[what], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_hitch_ns[what], dt, memory_order_relaxed);
    if (bytes) atomic_fetch_add_explicit(&g_hitch_bytes[what], bytes, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&g_hitch_max[what], memory_order_relaxed);
    while (dt > m && !atomic_compare_exchange_weak_explicit(&g_hitch_max[what], &m, dt, memory_order_relaxed, memory_order_relaxed)) {}
}
// a duration measured elsewhere (GPU time from the timestamps)
void nvmtl_hitch_add_ns(int what, uint64_t dt)
{
    if (!nvmtl_hitch_ms() || what < 0 || what >= NVH_N) return;
    atomic_fetch_add_explicit(&g_hitch_n[what], 1, memory_order_relaxed);
    atomic_fetch_add_explicit(&g_hitch_ns[what], dt, memory_order_relaxed);
    uint64_t m = atomic_load_explicit(&g_hitch_max[what], memory_order_relaxed);
    while (dt > m && !atomic_compare_exchange_weak_explicit(&g_hitch_max[what], &m, dt, memory_order_relaxed, memory_order_relaxed)) {}
}
static _Atomic uint64_t g_hitch_last_frame, g_hitch_last_tick;
static int nvmtl_u32_cmp(const void *a, const void *b) { const uint32_t x = *(const uint32_t *)a, y = *(const uint32_t *)b; return x < y ? -1 : x > y; }
// frame: a present ended a frame; 0: the 5 s timer of a process that presents nothing (OpenCL, compute), which only
// drains the counters into the HITCH-SUM totals
static void nvmtl_hitch_point(const char *source, int frame)
{
    const int ms = nvmtl_hitch_ms();
    if (!ms) return;
    static _Atomic uint64_t frames, hitches;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), was = frame ? atomic_exchange(&g_hitch_last_frame, now) : 0;
    uint64_t n[NVH_N], ns[NVH_N], by[NVH_N], mx[NVH_N];
    for (int i = 0; i < NVH_N; i++) {
        n[i] = atomic_exchange_explicit(&g_hitch_n[i], 0, memory_order_relaxed); ns[i] = atomic_exchange_explicit(&g_hitch_ns[i], 0, memory_order_relaxed);
        by[i] = atomic_exchange_explicit(&g_hitch_bytes[i], 0, memory_order_relaxed); mx[i] = atomic_exchange_explicit(&g_hitch_max[i], 0, memory_order_relaxed);
    }
    const uint64_t f = frame ? atomic_fetch_add(&frames, 1) + 1 : atomic_load(&frames);
    // every 5 s, the totals over all frames (hitches or not): where the time went on average - waits against GPU time
    // against the completion lag says whether a wait (glClientWaitSync's) is the GPU being busy or the signal being late
    static uint64_t sum_n[NVH_N], sum_ns[NVH_N], sum_by[NVH_N], sum_t0, sum_f0, sum_hitch_ns; static os_unfair_lock sum_lock = OS_UNFAIR_LOCK_INIT;
    static uint32_t ft[4096]; static unsigned nft;   // the window's frame times (us): percentiles, frames over twice the median
    os_unfair_lock_lock(&sum_lock);
    for (int i = 0; i < NVH_N; i++) { sum_n[i] += n[i]; sum_ns[i] += ns[i]; sum_by[i] += by[i]; }
    if (frame && was && nft < 4096) { const uint64_t us = (now - was) / 1000; ft[nft++] = us > 0xFFFFFFFFull ? 0xFFFFFFFFu : (uint32_t)us; }
    if (was && now - was >= (uint64_t)ms * 1000000ull) sum_hitch_ns += now - was;
    if (!sum_t0) { sum_t0 = now; sum_f0 = f; }
    else if (now - sum_t0 >= 5000000000ull) {
        const double wall = (double)(now - sum_t0); char b[1800]; int k = 0;
        for (int i = 0; i < NVH_N && k < (int)sizeof b - 80; i++) if (sum_n[i])
            k += snprintf(b + k, sizeof b - (size_t)k, sum_by[i] ? " %s %llu/%.1f%%(%.3fms each, %.1f MB)" : " %s %llu/%.1f%%(%.3fms each)", g_hitch_name[i],
                          (unsigned long long)sum_n[i], 100.0 * (double)sum_ns[i] / wall, (double)sum_ns[i] / (double)sum_n[i] / 1e6, sum_by[i] / 1048576.0);
        char pc[200] = "";
        if (nft) {
            qsort(ft, nft, sizeof ft[0], nvmtl_u32_cmp);
            const uint32_t med = ft[nft / 2]; unsigned over = 0; for (unsigned i = 0; i < nft; i++) over += ft[i] > 2 * med;
            snprintf(pc, sizeof pc, "; frame ms p50 %.1f p90 %.1f p99 %.1f max %.1f, %u over 2x median", med / 1e3, ft[nft * 9 / 10] / 1e3,
                     ft[nft * 99 / 100] / 1e3, ft[nft - 1] / 1e3, over);
        }
        nvlog_keep("HITCH-SUM%s %.1f s, %llu frames (%.1f fps)%s, %.1f%% of wall in hitch frames; share of wall:%s", frame ? "" : " (timer: no presents)", wall / 1e9,
              (unsigned long long)(f - sum_f0), (double)(f - sum_f0) * 1e9 / wall, pc, 100.0 * (double)sum_hitch_ns / wall, k ? b : " nothing");
        memset(sum_n, 0, sizeof sum_n); memset(sum_ns, 0, sizeof sum_ns); memset(sum_by, 0, sizeof sum_by); sum_t0 = now; sum_f0 = f; sum_hitch_ns = 0; nft = 0;
    }
    os_unfair_lock_unlock(&sum_lock);
    if (!frame || !was || now - was < (uint64_t)ms * 1000000ull) return;
    char b[1400]; int k = 0;
    for (int i = 0; i < NVH_N && k < (int)sizeof b - 96; i++) if (n[i])
        k += snprintf(b + k, sizeof b - (size_t)k, " %s %llu/%.2fms(max %.2f)", g_hitch_name[i], (unsigned long long)n[i], ns[i] / 1e6, mx[i] / 1e6);
    for (int i = 0; i < NVH_N && k < (int)sizeof b - 32; i++) if (by[i]) k += snprintf(b + k, sizeof b - (size_t)k, " %s_MB %.1f", g_hitch_name[i], by[i] / 1048576.0);
    const uint64_t h = atomic_fetch_add(&hitches, 1) + 1;
    nvlog_keep("HITCH %.1f ms frame (%s, frame %llu, hitch %llu):%s", (now - was) / 1e6, source, (unsigned long long)f, (unsigned long long)h, k ? b : " nothing counted");
}
void nvmtl_hitch_frame(const char *source) { nvmtl_gpt_frame(); nvmtl_hitch_point(source, 1); }
// from each completed submission: with no present for 5 s, the timer drains the totals (HITCH-SUM without frames)
static void nvmtl_hitch_tick(void)
{
    const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), lf = atomic_load(&g_hitch_last_frame), lt = atomic_load(&g_hitch_last_tick);
    if ((lf && now - lf < 5000000000ull) || now - lt < 1000000000ull) return;
    uint64_t want = lt; if (!atomic_compare_exchange_strong(&g_hitch_last_tick, &want, now)) return;
    nvmtl_hitch_point("timer", 0);
}
#define NVH_WRAP_DECL(f) static PFN_##f h_##f;
NVH_WRAP_DECL(vkAllocateMemory) NVH_WRAP_DECL(vkFreeMemory) NVH_WRAP_DECL(vkCreateImage) NVH_WRAP_DECL(vkDestroyImage)
NVH_WRAP_DECL(vkCreateGraphicsPipelines) NVH_WRAP_DECL(vkCreateComputePipelines) NVH_WRAP_DECL(vkCreateShaderModule)
NVH_WRAP_DECL(vkQueueSubmit) NVH_WRAP_DECL(vkWaitForFences)
static VkResult VKAPI_CALL nvh_AllocateMemory(VkDevice d, const VkMemoryAllocateInfo *i, const VkAllocationCallbacks *a, VkDeviceMemory *m)
{
    const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkAllocateMemory(d, i, a, m); nvmtl_hitch_add(NVH_MEMNEW, t, i ? i->allocationSize : 0);
    if (t && i) {   // which kind: an import of host pages, VRAM (small or not), or host memory
        int import = 0; for (const VkBaseInStructure *n = (const VkBaseInStructure *)i->pNext; n; n = n->pNext)
            import |= n->sType == VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT;
        const int dev = i->memoryTypeIndex < g_memp.memoryTypeCount && (g_memp.memoryTypes[i->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)
                        && !(g_memp.memoryTypes[i->memoryTypeIndex].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
        nvmtl_hitch_add(import ? NVH_MA_IMPORT : dev ? (i->allocationSize < (1u << 20) ? NVH_MA_DEVS : NVH_MA_DEVL) : NVH_MA_HOST, t, i->allocationSize);
    }
    return r;
}
static void VKAPI_CALL nvh_FreeMemory(VkDevice d, VkDeviceMemory m, const VkAllocationCallbacks *a)
{ const uint64_t t = nvmtl_hitch_t0(); h_vkFreeMemory(d, m, a); nvmtl_hitch_add(NVH_MEMFREE, t, 0); }
static VkResult VKAPI_CALL nvh_CreateImage(VkDevice d, const VkImageCreateInfo *i, const VkAllocationCallbacks *a, VkImage *o)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkCreateImage(d, i, a, o); nvmtl_hitch_add(NVH_IMGNEW, t, 0); return r; }
static void VKAPI_CALL nvh_DestroyImage(VkDevice d, VkImage i, const VkAllocationCallbacks *a)
{ const uint64_t t = nvmtl_hitch_t0(); h_vkDestroyImage(d, i, a); nvmtl_hitch_add(NVH_IMGDEL, t, 0); }
static VkResult VKAPI_CALL nvh_CreateGraphicsPipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkGraphicsPipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkCreateGraphicsPipelines(d, c, n, i, a, o); nvmtl_hitch_add(NVH_PIPE, t, 0); return r; }
static VkResult VKAPI_CALL nvh_CreateComputePipelines(VkDevice d, VkPipelineCache c, uint32_t n, const VkComputePipelineCreateInfo *i, const VkAllocationCallbacks *a, VkPipeline *o)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkCreateComputePipelines(d, c, n, i, a, o); nvmtl_hitch_add(NVH_PIPE, t, 0); return r; }
static VkResult VKAPI_CALL nvh_CreateShaderModule(VkDevice d, const VkShaderModuleCreateInfo *i, const VkAllocationCallbacks *a, VkShaderModule *o)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkCreateShaderModule(d, i, a, o); nvmtl_hitch_add(NVH_SHADERMOD, t, 0); return r; }
static VkResult VKAPI_CALL nvh_QueueSubmit(VkQueue q, uint32_t n, const VkSubmitInfo *s, VkFence f)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkQueueSubmit(q, n, s, f); nvmtl_hitch_add(NVH_SUBMIT, t, 0); return r; }
static VkResult VKAPI_CALL nvh_WaitForFences(VkDevice d, uint32_t n, const VkFence *f, VkBool32 all, uint64_t timeout)
{ const uint64_t t = nvmtl_hitch_t0(); VkResult r = h_vkWaitForFences(d, n, f, all, timeout); nvmtl_hitch_add(NVH_FENCEWAIT, t, 0); return r; }
/* The command census (NVMTL_HITCH_MS, or NVMTL_CMDCENSUS=1): what the recorded command buffers hold - render passes,
   dispatches, draws, barriers (image barriers are the layout transitions), transfers (each a blorp pass on ANV), and
   switches between compute and render work (a pipeline select and its flushes on Intel). Per frame in the hitch log;
   NVMTL_CMDCENSUS=1 also logs each submission's counts (the first 64, then one in 1024). */
static _Atomic uint64_t g_cc[7]; static _Atomic int g_cc_last;   // last kind recorded: 1 render, 2 compute, 3 transfer
static int g_cmdcensus = -1;
static inline void nvmtl_cc(int what, int kind)
{
    atomic_fetch_add_explicit(&g_cc[what - NVH_C_RPASS], 1, memory_order_relaxed);
    if (g_hitch_ms > 0) nvmtl_hitch_add_ns(what, 0);
    if (kind) { const int was = atomic_exchange_explicit(&g_cc_last, kind, memory_order_relaxed);
        if (was && was != kind && (was <= 2 && kind <= 2)) { atomic_fetch_add_explicit(&g_cc[NVH_C_SWITCH - NVH_C_RPASS], 1, memory_order_relaxed);
            if (g_hitch_ms > 0) nvmtl_hitch_add_ns(NVH_C_SWITCH, 0); } }
}
#define NVH_CC_DECL(f) static PFN_##f c_##f;
NVH_CC_DECL(vkCmdBeginRenderPass) NVH_CC_DECL(vkCmdDispatch) NVH_CC_DECL(vkCmdDispatchIndirect) NVH_CC_DECL(vkCmdDraw) NVH_CC_DECL(vkCmdDrawIndexed)
NVH_CC_DECL(vkCmdDrawIndirect) NVH_CC_DECL(vkCmdDrawIndexedIndirect) NVH_CC_DECL(vkCmdPipelineBarrier) NVH_CC_DECL(vkCmdCopyBuffer)
NVH_CC_DECL(vkCmdCopyBufferToImage) NVH_CC_DECL(vkCmdCopyImageToBuffer) NVH_CC_DECL(vkCmdFillBuffer) NVH_CC_DECL(vkCmdClearColorImage)
NVH_CC_DECL(vkCmdClearDepthStencilImage) NVH_CC_DECL(vkCmdBlitImage) NVH_CC_DECL(vkCmdCopyImage)
static void VKAPI_CALL cc_BeginRenderPass(VkCommandBuffer b, const VkRenderPassBeginInfo *i, VkSubpassContents s) { nvmtl_cc(NVH_C_RPASS, 1); c_vkCmdBeginRenderPass(b, i, s); }
static void VKAPI_CALL cc_Dispatch(VkCommandBuffer b, uint32_t x, uint32_t y, uint32_t z) { nvmtl_cc(NVH_C_DISPATCH, 2); c_vkCmdDispatch(b, x, y, z); }
static void VKAPI_CALL cc_DispatchIndirect(VkCommandBuffer b, VkBuffer f, VkDeviceSize o) { nvmtl_cc(NVH_C_DISPATCH, 2); c_vkCmdDispatchIndirect(b, f, o); }
static void VKAPI_CALL cc_Draw(VkCommandBuffer b, uint32_t a, uint32_t c, uint32_t d, uint32_t e) { nvmtl_cc(NVH_C_DRAW, 1); c_vkCmdDraw(b, a, c, d, e); }
static void VKAPI_CALL cc_DrawIndexed(VkCommandBuffer b, uint32_t a, uint32_t c, uint32_t d, int32_t e, uint32_t f) { nvmtl_cc(NVH_C_DRAW, 1); c_vkCmdDrawIndexed(b, a, c, d, e, f); }
static void VKAPI_CALL cc_DrawIndirect(VkCommandBuffer b, VkBuffer f, VkDeviceSize o, uint32_t n, uint32_t s) { nvmtl_cc(NVH_C_DRAW, 1); c_vkCmdDrawIndirect(b, f, o, n, s); }
static void VKAPI_CALL cc_DrawIndexedIndirect(VkCommandBuffer b, VkBuffer f, VkDeviceSize o, uint32_t n, uint32_t s) { nvmtl_cc(NVH_C_DRAW, 1); c_vkCmdDrawIndexedIndirect(b, f, o, n, s); }
static void VKAPI_CALL cc_PipelineBarrier(VkCommandBuffer b, VkPipelineStageFlags ss, VkPipelineStageFlags ds, VkDependencyFlags f, uint32_t nm, const VkMemoryBarrier *m,
                                          uint32_t nb, const VkBufferMemoryBarrier *bb, uint32_t ni, const VkImageMemoryBarrier *im)
{ nvmtl_cc(NVH_C_BARRIER, 0); for (uint32_t k = 0; k < ni; k++) if (im[k].oldLayout != im[k].newLayout) nvmtl_cc(NVH_C_IMGBAR, 0); c_vkCmdPipelineBarrier(b, ss, ds, f, nm, m, nb, bb, ni, im); }
static void VKAPI_CALL cc_CopyBuffer(VkCommandBuffer b, VkBuffer s, VkBuffer d, uint32_t n, const VkBufferCopy *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdCopyBuffer(b, s, d, n, r); }
static void VKAPI_CALL cc_CopyBufferToImage(VkCommandBuffer b, VkBuffer s, VkImage d, VkImageLayout l, uint32_t n, const VkBufferImageCopy *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdCopyBufferToImage(b, s, d, l, n, r); }
static void VKAPI_CALL cc_CopyImageToBuffer(VkCommandBuffer b, VkImage s, VkImageLayout l, VkBuffer d, uint32_t n, const VkBufferImageCopy *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdCopyImageToBuffer(b, s, l, d, n, r); }
static void VKAPI_CALL cc_FillBuffer(VkCommandBuffer b, VkBuffer d, VkDeviceSize o, VkDeviceSize s, uint32_t v) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdFillBuffer(b, d, o, s, v); }
static void VKAPI_CALL cc_ClearColorImage(VkCommandBuffer b, VkImage i, VkImageLayout l, const VkClearColorValue *c, uint32_t n, const VkImageSubresourceRange *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdClearColorImage(b, i, l, c, n, r); }
static void VKAPI_CALL cc_ClearDepthStencilImage(VkCommandBuffer b, VkImage i, VkImageLayout l, const VkClearDepthStencilValue *c, uint32_t n, const VkImageSubresourceRange *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdClearDepthStencilImage(b, i, l, c, n, r); }
static void VKAPI_CALL cc_BlitImage(VkCommandBuffer b, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageBlit *r, VkFilter f) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdBlitImage(b, s, sl, d, dl, n, r, f); }
static void VKAPI_CALL cc_CopyImage(VkCommandBuffer b, VkImage s, VkImageLayout sl, VkImage d, VkImageLayout dl, uint32_t n, const VkImageCopy *r) { nvmtl_cc(NVH_C_XFER, 3); c_vkCmdCopyImage(b, s, sl, d, dl, n, r); }
static void nvmtl_cc_submitted(void)
{
    if (g_cmdcensus <= 0) return;
    static uint64_t last[7], n; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    uint64_t now[7], d[7]; for (int i = 0; i < 7; i++) { now[i] = atomic_load(&g_cc[i]); d[i] = now[i] - last[i]; last[i] = now[i]; }
    const uint64_t k = ++n;
    pthread_mutex_unlock(&lk);
    if (k <= 64 || (k & 1023) == 0)
        nvlog_keep("CMDCENSUS submit %llu: %llu render passes, %llu dispatches, %llu draws, %llu barriers (%llu layout transitions), %llu transfers, %llu compute<->render switches",
                   (unsigned long long)k, (unsigned long long)d[0], (unsigned long long)d[1], (unsigned long long)d[2], (unsigned long long)d[3],
                   (unsigned long long)d[4], (unsigned long long)d[5], (unsigned long long)d[6]);
}
// NVMTL_HITCH_MS set: the Vulkan entry points above go through their timing wrappers (once, after the device is made)
static void nvmtl_hitch_wrap(void)
{
    { const char *e = getenv("NVMTL_CMDCENSUS"); g_cmdcensus = e && e[0] == '1'; }
    if (g_cmdcensus || nvmtl_hitch_ms()) {
#define NVH_CC(f, w) do { c_##f = p##f; p##f = w; } while (0)
        NVH_CC(vkCmdBeginRenderPass, cc_BeginRenderPass); NVH_CC(vkCmdDispatch, cc_Dispatch); NVH_CC(vkCmdDispatchIndirect, cc_DispatchIndirect);
        NVH_CC(vkCmdDraw, cc_Draw); NVH_CC(vkCmdDrawIndexed, cc_DrawIndexed); NVH_CC(vkCmdDrawIndirect, cc_DrawIndirect);
        NVH_CC(vkCmdDrawIndexedIndirect, cc_DrawIndexedIndirect); NVH_CC(vkCmdPipelineBarrier, cc_PipelineBarrier); NVH_CC(vkCmdCopyBuffer, cc_CopyBuffer);
        NVH_CC(vkCmdCopyBufferToImage, cc_CopyBufferToImage); NVH_CC(vkCmdCopyImageToBuffer, cc_CopyImageToBuffer); NVH_CC(vkCmdFillBuffer, cc_FillBuffer);
        NVH_CC(vkCmdClearColorImage, cc_ClearColorImage); NVH_CC(vkCmdClearDepthStencilImage, cc_ClearDepthStencilImage);
        NVH_CC(vkCmdBlitImage, cc_BlitImage); NVH_CC(vkCmdCopyImage, cc_CopyImage);
#undef NVH_CC
        nvlog("command census: ON (%s)", g_cmdcensus ? "NVMTL_CMDCENSUS=1: each submission's counts" : "per frame in the hitch log");
    }
    if (!nvmtl_hitch_ms()) return;
#define NVH_WRAP(f, w) do { h_##f = p##f; p##f = w; } while (0)
    NVH_WRAP(vkAllocateMemory, nvh_AllocateMemory); NVH_WRAP(vkFreeMemory, nvh_FreeMemory);
    NVH_WRAP(vkCreateImage, nvh_CreateImage); NVH_WRAP(vkDestroyImage, nvh_DestroyImage);
    NVH_WRAP(vkCreateGraphicsPipelines, nvh_CreateGraphicsPipelines); NVH_WRAP(vkCreateComputePipelines, nvh_CreateComputePipelines);
    NVH_WRAP(vkCreateShaderModule, nvh_CreateShaderModule); NVH_WRAP(vkQueueSubmit, nvh_QueueSubmit); NVH_WRAP(vkWaitForFences, nvh_WaitForFences);
#undef NVH_WRAP
}
static int g_perf_on = -1;
static pthread_mutex_t g_perf_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { uint64_t n, ns, bytes; } nvmtl_perf_acc;
static nvmtl_perf_acc g_perf[NVP_N]; static uint64_t g_perf_last;
static __thread int g_perf_kind;
uint64_t nvmtl_perf_now(void)
{
    if (g_perf_on < 0) { const char *e = getenv("NVMTL_PERF_LOG"); g_perf_on = e && e[0] && e[0] != '0'; }
    return g_perf_on ? clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) : 0;
}
void nvmtl_perf_note(int what, uint64_t t0, uint64_t bytes)
{
    if (!t0 || what < 0 || what >= NVP_N) return;
    const uint64_t dt = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - t0;
    pthread_mutex_lock(&g_perf_lock); g_perf[what].n++; g_perf[what].ns += dt; g_perf[what].bytes += bytes; pthread_mutex_unlock(&g_perf_lock);
}
static void nvmtl_perf_submit(unsigned sets, const uint64_t *pt)
{
    if (!pt[0]) return;
    static const char *const nm[NVP_N] = { "begin", "bufnew", "bufpool", "bufdel", "heap", "syncin", "syncout", "copy" };
    char s[640]; int k = 0; s[0] = 0;
    pthread_mutex_lock(&g_perf_lock);
    const double gap = !g_perf_kind && g_perf_last ? (double)(pt[0] - g_perf_last) / 1e3 : 0;
    for (int i = 0; i < NVP_N; i++) if (g_perf[i].n && k < (int)sizeof s - 64)
        k += snprintf(s + k, sizeof s - (size_t)k, " %s %llu:%.1f:%lluK", nm[i], (unsigned long long)g_perf[i].n,
                      (double)g_perf[i].ns / 1e3, (unsigned long long)(g_perf[i].bytes >> 10));
    memset(g_perf, 0, sizeof g_perf);
    if (!g_perf_kind) g_perf_last = pt[7];
    pthread_mutex_unlock(&g_perf_lock);
    fprintf(stderr, "PERFSUB k%d sets %u gap %.1f hook %.1f end %.1f fence %.1f submit %.1f wait %.1f retire %.1f wit %llu:%llu:%llu:%llu ctl %llu/%llu |%s\n",
            g_perf_kind, sets, gap, (double)(pt[1] - pt[0]) / 1e3, (double)(pt[2] - pt[1]) / 1e3, (double)(pt[3] - pt[2]) / 1e3,
            (double)(pt[4] - pt[3]) / 1e3, (double)(pt[5] - pt[4]) / 1e3, (double)(pt[7] - pt[6]) / 1e3,
            (unsigned long long)g_wit_ok, (unsigned long long)g_wit_waited, (unsigned long long)g_wit_leak,
            (unsigned long long)g_wit_blind, (unsigned long long)g_wit_ctl_fired, (unsigned long long)g_wit_ctl_run, s);
}
static int nvmtl_address_snapshot(nvk_cmdbuf *c, uint32_t which, VkDescriptorBufferInfo *out) {
    unsigned slot = c->address_count % NVMTL_TABLES_PER_PAGE;
    nvmtl_address_page *page = c->address_pages;
    if (slot == 0) {
        page = nvmtl_keep_page_pop();
        if (!page) {
            page = calloc(1, sizeof(*page));
            if (!page) return -1;
            if (nvmtl_vk_buffer_create(NVMTL_TABLE_BYTES * NVMTL_TABLES_PER_PAGE, 1, &page->buffer)) { nvmtl_vk_buffer_destroy(&page->buffer); free(page); return -1; }
            page->addr = nvmtl_vk_buffer_address(&page->buffer);
        }
        page->next = c->address_pages; c->address_pages = page;
    }
    uint64_t *addresses = (uint64_t *)((uint8_t *)page->buffer.map + slot * NVMTL_TABLE_BYTES);
    nvmtl_bindtable *t = c->table;
    uint64_t self = page->addr;
    if (!self) nvlog("address table: page has no device address - array_ref<void> arguments will read 0");
    else self += slot * NVMTL_TABLE_BYTES;
    if (t && which < 2) {
        if (!t->tinit[which]) {   // built once from the slots; every slot write keeps it current from then on (nvmtl_tab_note)
            for (uint32_t i = 0; i < NVMTL_NBUF + NVMTL_NTEX; ++i) t->tab[which][i < NVMTL_NBUF ? i : NVMTL_NBUF + i] = nvmtl_tab_entry(&t->s[which][i], i);
            t->tinit[which] = 1;
        }
        const uint32_t need = c->tab_need[which];
        if (need) {   // stage 1d: only the entries this stage's module reads (it reads no self address)
            const uint32_t nb = NVMTL_TABNEED_BUFS(need), nt = NVMTL_TABNEED_TEXS(need);
            if (nb) memcpy(addresses, t->tab[which], nb * sizeof(uint64_t));
            if (nt) memcpy(addresses + 2 * NVMTL_NBUF, t->tab[which] + 2 * NVMTL_NBUF, nt * sizeof(uint64_t));
            *out = (VkDescriptorBufferInfo){ page->buffer.buf, slot * NVMTL_TABLE_BYTES, NVMTL_TABLE_BYTES };
            c->address_count++;
            return 0;
        }
        memcpy(addresses, t->tab[which], NVMTL_NBUF * sizeof(uint64_t));
        memcpy(addresses + 2 * NVMTL_NBUF, t->tab[which] + 2 * NVMTL_NBUF, NVMTL_NTEX * sizeof(uint64_t));
    } else {
        const uint64_t dummyAddr = nvmtl_dummy_addr();
        for (unsigned i = 0; i < NVMTL_NBUF; ++i) addresses[i] = dummyAddr;
        memset(addresses + 2 * NVMTL_NBUF, 0, NVMTL_NTEX * sizeof(uint64_t));
    }
    for (unsigned i = 0; i < NVMTL_NBUF; ++i) addresses[NVMTL_NBUF + i] = self ? self + i * sizeof(uint64_t) : 0;
    *out = (VkDescriptorBufferInfo){ page->buffer.buf, slot * NVMTL_TABLE_BYTES, NVMTL_TABLE_BYTES };
    c->address_count++;
    return 0;
}

static void nvmtl_spirv_retarget_set(uint32_t *w, size_t words, uint32_t newSet)
{
    if (words < 5 || w[0] != 0x07230203u) return;
    size_t i = 5;
    while (i < words) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
        if (count == 0 || i + count > words) return;
        if (op == 71  && count >= 4 && w[i + 2] == 34  && w[i + 3] == 0) w[i + 3] = newSet;
        i += count;
    }
}

static uint32_t nvmtl_spirv_has_specid(const void *code, size_t words, uint32_t id)
{
    const uint32_t *w = code;
    for (size_t i = 5; i < words; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > words) return 0;
        if (op == 71 && wc >= 4 && w[i + 2] == 1 && w[i + 3] == id) return 1;
        if (op == 54) return 0;
        i += wc;
    }
    return 0;
}
static int nvmtl_spirv_uses_set(const void *code, size_t words, uint32_t set)
{
    const uint32_t *w = (const uint32_t *)code;
    if (!w || words < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < words; ) {
        uint32_t count = w[i] >> 16, op = w[i] & 0xffff;
        if (count == 0 || i + count > words) return 0;
        if (op == 71 && count >= 4 && w[i + 2] == 34 && w[i + 3] == set) return 1;
        i += count;
    }
    return 0;
}

#define NVMTL_DESC_BATCH_MAX 32
/* Descriptor sets taken from the pool per allocation (the rest wait in the command buffer's cache). Default 4: 32 cost ~15 us
   per command buffer whose first draw took them (mtl_lat "lat", set+dispatch 20.0 -> 5.2 us at 4, 2026-10-28), most of
   them never used by a small command buffer. NVMTL_DESC_BATCH=1..32 overrides. */
#define NVMTL_DESC_BATCH_DEFAULT 4
static unsigned nvmtl_desc_batch(void)
{
    static int n = -1;
    if (n < 0) {
        const char *e = getenv("NVMTL_DESC_BATCH");
        n = e && *e ? atoi(e) : NVMTL_DESC_BATCH_DEFAULT;
        if (n < 1) n = 1;
        if (n > NVMTL_DESC_BATCH_MAX) n = NVMTL_DESC_BATCH_MAX;
    }
    return (unsigned)n;
}
/* Every pool is full and no more may be opened (or opening one failed): the sets that GPU work still in flight holds come
   back when that work finishes (nvmtl_cmd_retire recycles them). Wait for that (g_desc_lock held on entry and on return)
   instead of dropping the draw: up to 250 ms in all for one allocation, and only while some submission is not finished -
   20 ms after the last one finished nothing more will come. Returns 1 if sets came back (allocate again), 0 if not
   (the command buffer then ends with MTLCommandBufferErrorOutOfMemory rather than run with draws missing). The waits
   happen on the encoding thread; completions are finished by the execution worker (NVMTLEventSync.m), never under
   g_desc_lock, which the wait releases. */
static int nvmtl_desc_reclaim(uint64_t seen, uint64_t *t0)
{
    const uint64_t start = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!*t0) *t0 = start;
    uint64_t idle_since = 0;
    for (;;) {
        if (g_desc_gen != seen) { atomic_fetch_add(&g_desc_reclaims, 1); return 1; }
        const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (now - *t0 >= 250000000ull) break;
        const int idle = atomic_load(&g_sub_wm) >= __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);
        if (!idle) idle_since = 0;
        else if (!idle_since) idle_since = now;
        else if (now - idle_since >= 20000000ull) break;
        struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5000000; if (ts.tv_nsec >= 1000000000) { ts.tv_sec++; ts.tv_nsec -= 1000000000; }
        pthread_cond_timedwait(&g_desc_cond, &g_desc_lock, &ts);
    }
    const unsigned long long k = atomic_fetch_add(&g_desc_reclaim_fail, 1) + 1;
    if (k <= 8 || !(k & (k - 1)))
        nvlog("vk: descriptor sets: every pool full (%u open) and nothing came back within %llu ms - the command buffer ends with "
              "MTLCommandBufferErrorOutOfMemory (%llu so far; %llu reclaims succeeded)", g_ndescPools,
              (unsigned long long)((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - *t0) / 1000000), k, (unsigned long long)atomic_load(&g_desc_reclaims));
    return 0;
}
static VkDescriptorSet nvmtl_snapshot_set_m(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi, const uint64_t *mask, const uint64_t *tb);
static VkDescriptorSet nvmtl_snapshot_set(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi)
{
    return nvmtl_snapshot_set_m(c, which, stex_hi, NULL, NULL);
}
static VkDescriptorSet nvmtl_snapshot_set_t(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi, const uint64_t *tb)
{
    return nvmtl_snapshot_set_m(c, which, stex_hi, NULL, tb);
}
#define NVMTL_E78_HAS(m, i) (!(m) || (((m)[(i) >> 6] >> ((i) & 63)) & 1))
static VkDescriptorSet nvmtl_snapshot_set_md(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi, const uint64_t *mask, const uint64_t *tb,
                                             VkDescriptorSet from, const uint64_t *copym);
static VkDescriptorSet nvmtl_snapshot_set_m(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi, const uint64_t *mask, const uint64_t *tb)
{ if (which < 2) c->tab_need[which] = 0; return nvmtl_snapshot_set_md(c, which, stex_hi, mask, tb, VK_NULL_HANDLE, NULL); }
#define NVMTL_COPY_HAS(m, i) ((m) && (((m)[(i) >> 6] >> ((i) & 63)) & 1))
static VkDescriptorSet nvmtl_snapshot_set_md(nvk_cmdbuf *c, uint32_t which, uint32_t stex_hi, const uint64_t *mask, const uint64_t *tb,
                                             VkDescriptorSet from, const uint64_t *copym)
{
    VkDescriptorSet set = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    VkResult r = VK_ERROR_OUT_OF_POOL_MEMORY;
    if (c->ndcache) {
        set = (VkDescriptorSet)c->dcache[--c->ndcache];
        pool = (VkDescriptorPool)c->dcachepool;
        r = VK_SUCCESS;
        if (c->nsets == c->capsets) {
            unsigned cap = c->capsets ? c->capsets * 2 : 64;
            nvk_setref *grown = (nvk_setref *)realloc(c->sets, cap * sizeof *grown);
            if (grown) { c->sets = grown; c->capsets = cap; }
            else { c->dcache[c->ndcache++] = (void *)set; c->skipped++; c->skip_oom++; return VK_NULL_HANDLE; }
        }
    } else {
        uint64_t reclaim_t0 = 0;
        pthread_mutex_lock(&g_desc_lock);
        if (ensure_descriptors_locked()) { pthread_mutex_unlock(&g_desc_lock); c->skipped++; c->skip_oom++; return VK_NULL_HANDLE; }
    retry_alloc:;
        const uint64_t gen_seen = g_desc_gen;
        unsigned n = nvmtl_desc_batch();
        VkDescriptorSetLayout ls[NVMTL_DESC_BATCH_MAX];
        VkDescriptorSet got[NVMTL_DESC_BATCH_MAX];
        for (unsigned k = 0; k < NVMTL_DESC_BATCH_MAX; k++) ls[k] = g_setLayout;
        if (g_nrecyc) {   // retired sets first, a run from one pool (the command buffer's cache holds one pool's sets)
            unsigned k = 0; pool = g_recyc[g_nrecyc - 1].pool;
            while (k < n && g_nrecyc && g_recyc[g_nrecyc - 1].pool == pool) {
                const nvmtl_recycled_set *e = &g_recyc[--g_nrecyc];
                if (nvmtl_reuse_check() && !nvmtl_sub_is_done(e->seq)) nvmtl_reuse_flag("descriptor set", e->seq, 0);
                got[k++] = e->set;
            }
            n = k; r = VK_SUCCESS; g_recyc_hits += k;
        } else for (;;) {
            r = VK_ERROR_OUT_OF_POOL_MEMORY;
            for (unsigned k = g_ndescPools; k-- > 0 && r != VK_SUCCESS;) {
                VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_descPools[k], n, ls };
                pool = g_descPools[k]; r = pvkAllocateDescriptorSets(g_dev, &ai, got);
            }
            if (r != VK_SUCCESS && nvmtl_desc_pool_open() == 0) {
                VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_descPools[g_ndescPools - 1], n, ls };
                pool = g_descPools[g_ndescPools - 1]; r = pvkAllocateDescriptorSets(g_dev, &ai, got);
            }
            if (r == VK_SUCCESS || n == 1) break;
            n = 1;
        }
        // every pool full, none more could be opened: wait for in-flight work to give sets back, then try again
        // (once a draw of this command buffer could not be recorded it ends with an error anyway: no more waits for it)
        if (r != VK_SUCCESS && !c->skip_oom && nvmtl_desc_reclaim(gen_seen, &reclaim_t0)) goto retry_alloc;
        if (r == VK_SUCCESS) {
            set = got[0];
            c->dcachepool = (void *)pool;
            for (unsigned k = 1; k < n; k++) c->dcache[c->ndcache++] = (void *)got[k];
        }
        if (r == VK_SUCCESS && c->nsets == c->capsets) {
            unsigned cap = c->capsets ? c->capsets * 2 : 64;
            nvk_setref *grown = (nvk_setref *)realloc(c->sets, cap * sizeof *grown);
            if (grown) { c->sets = grown; c->capsets = cap; }
            else {
                pvkFreeDescriptorSets(g_dev, pool, 1, &set);
                if (c->ndcache) {
                    VkDescriptorSet back[NVMTL_DESC_BATCH_MAX];
                    for (unsigned k = 0; k < c->ndcache; k++) back[k] = (VkDescriptorSet)c->dcache[k];
                    pvkFreeDescriptorSets(g_dev, pool, c->ndcache, back);
                    c->ndcache = 0; c->dcachepool = NULL;
                }
                r = VK_ERROR_OUT_OF_HOST_MEMORY;
            }
        }
        pthread_mutex_unlock(&g_desc_lock);
    }
    if (r != VK_SUCCESS) {
        nvlog("vk: descriptor set allocation FAILED (%d) with %u pools, %u sets in this command buffer — this draw is not recorded "
              "and its command buffer will end with MTLCommandBufferErrorOutOfMemory instead of running without it", r, g_ndescPools, c->nsets);
        c->skipped++; c->skip_oom++; return VK_NULL_HANDLE;
    }
    c->sets[c->nsets++] = (nvk_setref){ pool, set };
    VkWriteDescriptorSet w[NVMTL_NBIND];
    VkCopyDescriptorSet cp[NVMTL_NBIND]; uint32_t ncp = 0;
    if (!from) copym = NULL;
    VkDescriptorBufferInfo bi[NVMTL_NBIND];
    VkDescriptorImageInfo  ii[NVMTL_NBIND];
    VkBufferView tv[NVMTL_NBIND];
    VkDescriptorBufferInfo addressInfo;
    if (!g_mutable) tb = NULL;
    uint32_t dyn = 0; void *tab = NULL;
    if (NVMTL_E78_HAS(mask, NVMTL_ADDRESS_BINDING)) {
        if (nvmtl_address_snapshot(c, which, &addressInfo)) { c->skipped++; c->skip_oom++; return VK_NULL_HANDLE; }
        // A dynamic-offset table is described from its page start and located per bind, so a set whose other
        // descriptors still hold can be bound again with a newer table on the same page (nvmtl_snapshot_set_c).
        if (nvmtl_table_ubo()) { dyn = (uint32_t)addressInfo.offset; tab = (void *)addressInfo.buffer; addressInfo.offset = 0; }
    }
    if (which < 2) { c->dyn_out[which] = dyn; c->tab_out[which] = tab; }
    VkDescriptorBufferInfo dummyB = { g_null_desc ? VK_NULL_HANDLE : (VkBuffer)g_dummyBuf.buf, 0, VK_WHOLE_SIZE };
    VkDescriptorImageInfo  dummyI = { VK_NULL_HANDLE, g_null_desc ? VK_NULL_HANDLE : (VkImageView)g_dummyImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
    VkBufferView const dummyTV = g_null_desc ? VK_NULL_HANDLE : (VkBufferView)g_dummyTexView;
    VkDescriptorImageInfo  dummyS = { g_dummySampler, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED };
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    uint32_t nw = 0;
    for (uint32_t i = 0; i < NVMTL_STEX_BASE; i++) {
        if (!NVMTL_E78_HAS(mask, i)) continue;
        VkDescriptorType ty = binding_type(i);
        const nvmtl_slot *sl = nvmtl_slot_checked(t ? &t->s[which][i] : NULL, which, i);
        const VkDescriptorBufferInfo *pb = NULL; const VkDescriptorImageInfo *pi = NULL;
        if (ty == VK_DESCRIPTOR_TYPE_STORAGE_BUFFER || ty == VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC) {
            if (i == NVMTL_ADDRESS_BINDING) pb = &addressInfo;
            else if (sl && sl->kind == NVMTL_SLOT_BUF) {
                // an offset the device cannot take (a byte offset of Apple's OpenCL copies) binds rounded down: the
                // translator adds the remainder to the index of every buffer it keeps as a descriptor (lower_with), and
                // the address table still holds the exact address for everything it lowered
                const size_t at = sl->off & ~(size_t)(NVMTL_SSBO_ALIGN - 1);
                if (at != sl->off) { static _Atomic unsigned long long k; const unsigned long long n = atomic_fetch_add(&k, 1) + 1;
                    if (!(n & (n - 1))) nvlog("vk: buffer slot %u bound at offset %zu binds its descriptor at %zu (the shader adds the remainder; %llu so far)", i, sl->off, at, n); }
                bi[i] = (VkDescriptorBufferInfo){ (VkBuffer)sl->h, at, VK_WHOLE_SIZE }; pb = &bi[i];
            } else pb = &dummyB;
        } else if (ty == VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE) {
            const uint32_t k = i - NVMTL_NBUF; const int wantT = tb && ((tb[k >> 6] >> (k & 63)) & 1);
            if (wantT) {
                tv[i] = sl && sl->kind == NVMTL_SLOT_TEXBUF && sl->h ? (VkBufferView)sl->h : dummyTV;
                w[nw++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_BINDING_NUMBER(i), 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &tv[i] };
                continue;
            }
            if (NVMTL_COPY_HAS(copym, i)) { cp[ncp++] = (VkCopyDescriptorSet){ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, from, NVMTL_BINDING_NUMBER(i), 0, set, NVMTL_BINDING_NUMBER(i), 0, 1 }; continue; }
            // A texture slot whose sampled view could not be built is recorded with a null handle. The descriptor
            // written for it is the dummy image, never the null handle: a null view reads as zero in NVK, and a
            // debug NVK asserts on any handle that is not a live image view.
            if (sl && sl->kind == NVMTL_SLOT_TEX && sl->h) { ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (VkImageView)sl->h, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL }; pi = &ii[i]; }
            else {
                if (sl && sl->kind == NVMTL_SLOT_TEX) { static int said; if (!said++) nvlog("vk: texture slot %u is bound to a texture with no sampled view - the dummy image is bound instead (said once)", i - NVMTL_NBUF); }
                pi = &dummyI;
            }
        } else {
            if (NVMTL_COPY_HAS(copym, i)) { cp[ncp++] = (VkCopyDescriptorSet){ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, from, NVMTL_BINDING_NUMBER(i), 0, set, NVMTL_BINDING_NUMBER(i), 0, 1 }; continue; }
            // Same for a sampler slot: a sampler state whose VkSampler was never created binds the dummy sampler.
            if (sl && sl->kind == NVMTL_SLOT_SAMP && sl->h) { ii[i] = (VkDescriptorImageInfo){ (VkSampler)sl->h, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED }; pi = &ii[i]; }
            else {
                if (sl && sl->kind == NVMTL_SLOT_SAMP) { static int said; if (!said++) nvlog("vk: sampler slot %u is bound to a sampler state with no VkSampler - the dummy sampler is bound instead (said once)", i - NVMTL_NBUF - NVMTL_NTEX); }
                pi = &dummyS;
            }
        }
        w[nw++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_BINDING_NUMBER(i), 0, 1, ty, pi, pb, NULL };
    }
    if (stex_hi > NVMTL_NSTEX) stex_hi = NVMTL_NSTEX;
    for (uint32_t j = 0; j < stex_hi; j++) {
        uint32_t i = NVMTL_STEX_BASE + j;
        const nvmtl_slot *sl = nvmtl_slot_checked(t ? &t->s[which][i] : NULL, which, i);
        if (tb && ((tb[2 + (j >> 6)] >> (j & 63)) & 1)) {
            tv[i] = sl && sl->kind == NVMTL_SLOT_TEXBUF && sl->h ? (VkBufferView)sl->h : dummyTV;
            w[nw++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_BINDING_NUMBER(i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &tv[i] };
            continue;
        }
        if (NVMTL_COPY_HAS(copym, i)) { cp[ncp++] = (VkCopyDescriptorSet){ VK_STRUCTURE_TYPE_COPY_DESCRIPTOR_SET, NULL, from, NVMTL_BINDING_NUMBER(i), 0, set, NVMTL_BINDING_NUMBER(i), 0, 1 }; continue; }
        int bound = sl && sl->kind == NVMTL_SLOT_TEX && sl->h;
        void *view = bound ? sl->h : g_null_desc ? NULL : g_dummyStex.view;
        if (!bound) { static int said; if (!said++) nvlog("vk: storage texture %u is declared by the shader (through %u) but nothing writable is bound there — %s", j, stex_hi - 1, g_null_desc ? "null descriptor" : view ? "dummy" : "left UNWRITTEN"); }
        if (!view && !g_null_desc) continue;
        ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (VkImageView)view, VK_IMAGE_LAYOUT_GENERAL };
        w[nw++] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_BINDING_NUMBER(i), 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &ii[i], NULL, NULL };
    }
    nvmtl_enc78_count(E78C_WRITES, nw);
    pvkUpdateDescriptorSets(g_dev, nw, w, ncp, cp);
    return set;
}
static VkDescriptorSet nvmtl_snapshot_set_c(nvk_cmdbuf *c, uint32_t which, const nvk_pipeline *p)
{
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    if (!t || which > 1 || !nvmtl_enc78_on(NVMTL_E78_H3)) return nvmtl_snapshot_set_t(c, which, p->stex_hi[which], p->tbany ? p->tbm[which] : NULL);
    const uint32_t need = p->tabneed[which];
    c->tab_need[which] = need;
    const int tabok = nvmtl_tabneed_covers(t->lastneed[which], need);   /* the last set's table holds what this stage reads */
    const uint64_t all[4] = { ~0ull, ~0ull, ~0ull, ~0ull };
    // The masks without the framebuffer-fetch set when they exist; otherwise the masks that count it, which are a
    // superset and so always safe. Falling straight to "all" would write every descriptor on every draw of a pipeline
    // that has only the older mask, the per-draw cost these masks exist to remove.
    const int m2 = nvmtl_desc_mask_on();
    const uint64_t *pm = (m2 && p->bmask2_ok) ? p->bmask2 : (m2 && p->gmask2_ok && which < 2) ? p->gmask2[which]
                       : p->bmask_ok ? p->bmask : (p->gmask_ok && which < 2) ? p->gmask[which] : all;
    uint32_t hi = p->stex_hi[which];
    // A texture binding holds an image or, for a shader that reads a texel buffer there, a texel buffer (the layout's
    // descriptors are mutable). The last set can be bound again, or copied from, only by a pipeline that reads the same
    // bindings as texel buffers. WAS: any texel-buffer pipeline rebuilt its whole set for every draw (Sodium's terrain:
    // ~60k sets a second).
    static const uint64_t notb[4] = { 0, 0, 0, 0 };
    const uint64_t *tb = p->tbany ? p->tbm[which] : notb;
    const int sametb = !memcmp(t->lasttb[which], tb, sizeof notb);
    // read before anything is decided or built: a view or sampler destroyed from here on bumps it past what lastgen gets
    const uint64_t gen = nvmtl_objgen();
    const nvmtl_ds_in in = { pm, t->lastw[which], t->chg[which], hi, t->last_hi[which],
                             sametb && t->last[which] && t->last_idx[which] < c->nsets
                             && (VkDescriptorSet)c->sets[t->last_idx[which]].set == t->last[which],
                             t->dirty[which], tabok, t->lastgen[which] == gen, nvmtl_dirty_only_on() };
    if (nvmtl_ds_reuse(&in)) {
        c->dyn_out[which] = t->lastdyn[which]; c->tab_out[which] = t->lasttab[which];
        nvmtl_enc78_count(E78C_SETREUSE, 1); return t->last[which];
    }
    // Only buffer slots changed (Apple's GL rebinds its uniform bytes for every draw). A pipeline that reaches every
    // buffer through the address table (no buffer binding in its mask) still has all its descriptors in the last set;
    // what changed is the table. On the same table page that is a new dynamic offset, not a new set.
    // Also taken with nothing dirty when the last set's table lacks entries this pipeline reads (stage 1d): the new
    // table on the same page is again only a new dynamic offset. Nothing changed, so buffer descriptors may be read.
    // The changed buffers' descriptors in the set are stale from here on: nvmtl_ds_was_taken drops them from lastw.
    if (nvmtl_ds_was(&in, nvmtl_table_ubo())) {
        const int table = NVMTL_E78_HAS(pm, NVMTL_ADDRESS_BINDING);
        const nvmtl_address_page *page = (const nvmtl_address_page *)c->address_pages;
        const int samePage = !table || (t->lasttab[which] && page && (c->address_count % NVMTL_TABLES_PER_PAGE) != 0
                                        && (void *)page->buffer.buf == t->lasttab[which]);
        VkDescriptorBufferInfo ai;
        if (samePage && (!table || !nvmtl_address_snapshot(c, which, &ai))) {
            if (table) { t->lastdyn[which] = (uint32_t)ai.offset; t->lastneed[which] = need; }
            t->dirty[which] = 0; nvmtl_ds_was_taken(t->lastw[which], t->chg[which]);
            c->dyn_out[which] = t->lastdyn[which]; c->tab_out[which] = t->lasttab[which];
            nvmtl_enc78_count(E78C_SETREUSE, 1);
            return t->last[which];
        }
    }
    uint64_t m[4], cm[NVMTL_CHG_WORDS];
    const VkDescriptorSet from = nvmtl_ds_new(&in, m, &hi, cm) ? t->last[which] : VK_NULL_HANDLE;
    VkDescriptorSet s = nvmtl_snapshot_set_md(c, which, hi, m, p->tbany ? tb : NULL, from, from ? cm : NULL);
    if (!s) return s;
    t->last[which] = s; t->last_idx[which] = c->nsets - 1; t->last_hi[which] = hi; t->dirty[which] = 0; t->lastgen[which] = gen;
    memcpy(t->lasttb[which], tb, sizeof notb);
    t->lastdyn[which] = c->dyn_out[which]; t->lasttab[which] = c->tab_out[which]; t->lastneed[which] = need;
    memset(t->chg[which], 0, sizeof t->chg[which]);
    memcpy(t->lastw[which], m, sizeof m);
    nvmtl_enc78_count(E78C_SETNEW, 1);
    return s;
}

static int nvmtl_ktime_on(void);
int nvmtl_vk_bind_buffer(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_buffer *b, size_t offset)
{
    if (index >= NVMTL_NBUF) { nvlog("bind buffer: index %u is outside [0,%d)", index, NVMTL_NBUF); return -1; }
    if (!b || !b->buf || offset >= b->size) { nvlog("bind buffer: offset %zu outside length %zu", offset, b ? b->size : 0); return -1; }
    int r = nvmtl_bind_slot(c, set, index, NVMTL_SLOT_BUF, b->buf, offset);
    if (!r && set < 2 && nvmtl_ktime_on()) {   // NVMTL_KTIME: does the dispatch read or write system memory over PCIe
        // 64 KiB and up, outside the pools: setBytes constants and Apple OpenCL's argument ring live in the system-RAM
        // pool and would mark every dispatch
        const int sys = b->size >= 65536 && !b->pool && (b->imported || b->sysmem || (b->mtype1 && b->mtype1 <= g_memp.memoryTypeCount &&
                        !(g_memp.memoryTypes[b->mtype1 - 1].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)));
        if (sys) c->kt_sys[set] |= 1u << index; else c->kt_sys[set] &= ~(1u << index);
    }
    if (!r) { nvmtl_bindtable *t = (nvmtl_bindtable *)c->table; nvmtl_slot *sl = &t->s[set][index]; uint64_t a = nvmtl_vk_buffer_address(b);
              // a new buffer at the same handle and offset still changes the descriptor: mark it changed, not only dirty
              if (sl->size != b->size || sl->address != a) { t->dirty[set] = 1; t->chg[set][index >> 6] |= 1ull << (index & 63); }
              sl->size = b->size; sl->address = a; nvmtl_tab_note(t, set, index);
              sl->vextra = b->vksize > b->size && b->vksize - b->size <= UINT32_MAX ? (uint32_t)(b->vksize - b->size) : 0; }
    return r;
}
int nvmtl_vk_trace_buffers(nvk_cmdbuf *c, char *out, size_t cap)
{
    if (!out || cap < 16) return 0;
    out[0] = 0;
    if (!c || !c->table) { snprintf(out, cap, " (no bind table)"); return 0; }
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    size_t n = 0, lim = cap - 8;
    int k = 0;
    for (int set = 0; set < 2; set++)
        for (int i = 0; i < NVMTL_NBUF; i++) {
            nvmtl_slot *s = &t->s[set][i];
            if (s->kind != NVMTL_SLOT_BUF) continue;
            int w = snprintf(out + n, lim - n, " %c%d 0x%llx+%zu/%zu", set ? 'f' : 'v', i,
                             (unsigned long long)s->address, s->off, s->size);
            if (w < 0 || (size_t)w >= lim - n) { out[n] = 0; snprintf(out + n, cap - n, " +more"); return k; }
            n += (size_t)w; k++;
        }
    if (!k) snprintf(out, cap, " (none)");
    return k;
}
int nvmtl_vk_bind_texture(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_image *img)
{
    if (index >= NVMTL_NTEX) { nvlog("bind texture: index %u is outside [0,%d)", index, NVMTL_NTEX); return -1; }
    return nvmtl_bind_slot(c, set, NVMTL_NBUF + index, NVMTL_SLOT_TEX, img->view, 0);
}
int nvmtl_vk_bind_sampler(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_sampler *s)
{
    if (index >= NVMTL_NSAMP) { nvlog("bind sampler: index %u is outside [0,%d)", index, NVMTL_NSAMP); return -1; }
    return nvmtl_bind_slot(c, set, NVMTL_NBUF + NVMTL_NTEX + index, s ? NVMTL_SLOT_SAMP : NVMTL_SLOT_NONE, s ? s->s : NULL, 0);
}

int nvmtl_vk_sample_positions_backend(void) { return nvmtl_vk_init() == 0 && g_sample_locations; }
int nvmtl_vk_sample_positions_default(uint32_t count, nvmtl_sample_pattern *out)
{
    /* Without the programmable-position backend the hardware still draws the standard pattern, which is Metal's default.
       Apps read these positions and pass them back to setSamplePositions; returning nothing left their array
       uninitialised, the pass then failed validation and drew nothing (Chess). */
    if (nvmtl_vk_sample_positions_backend() && count != 1 && !(g_sample_props.sampleLocationSampleCounts & count)) return -1;
    return nvmtl_sample_default(count, out);
}
static VkSampleLocationsInfoEXT nvmtl_sample_info(const nvmtl_sample_pattern *p, VkSampleLocationEXT vk[8])
{
    for (uint32_t i = 0; i < p->count; ++i) vk[i] = (VkSampleLocationEXT){ nvmtl_sample_raster_coordinate(p->position[i].x), nvmtl_sample_raster_coordinate(p->position[i].y) };
    return (VkSampleLocationsInfoEXT){ VK_STRUCTURE_TYPE_SAMPLE_LOCATIONS_INFO_EXT, NULL,
        (VkSampleCountFlagBits)p->count, {1,1}, p->count, vk };
}

/*
 * Framebuffer fetch ([[color(n)]] reads, Apple's programmable blending): the translator gives each read colour attachment an
 * input attachment at set 3, binding 192+n. Every render pass in an app declares its colour attachments as input
 * attachments too (in GENERAL layout, with a self-dependency), so a pipeline that reads them stays compatible with every
 * other pipeline of the pass; a draw with such a pipeline binds set 3 to the open pass's colour views behind a by-region
 * barrier, so it reads what the earlier draws wrote. Blender's Eevee resolves its lighting that way on Apple GPUs (the
 * identity this driver answers with), and read nothing before: every lit surface came out black (#152). WindowServer keeps
 * its passes as they were (NVMTL_FBFETCH_WS=1 opts it in) until this is proven on the desktop; NVMTL_NO_FBFETCH=1 turns it off.
 */
#define NVMTL_FETCH_SET 3
#define NVMTL_FETCH_BINDING 192
static VkDescriptorSetLayout g_fetchLayout, g_emptyLayout;
static VkDescriptorPool g_fetchPools[64]; static unsigned g_nfetchPools;
static int nvmtl_fbfetch_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *off = getenv("NVMTL_NO_FBFETCH"), *ws = getenv("NVMTL_FBFETCH_WS");
        const int is_ws = getprogname() && !strcmp(getprogname(), "WindowServer");
        /* AMD/Intel (NMXAccel): on in WindowServer too - Core Animation's vibrancy materials read [[color(n)]], and
         * without fetch the lock-screen clock box composited solid black on the B580 (2026-10-28). NVMTL_FBFETCH_WS=0
         * turns it back off there. */
        const int ws_on = ws ? ws[0] == '1' : nvmtl_vk_vendor() != 0x10de;
        on = !(off && off[0] == '1') && (!is_ws || ws_on);
        nvlog("vk: framebuffer fetch %s", on ? "ON - render passes declare their colour attachments as input attachments"
                                           : is_ws ? "off in WindowServer (NVMTL_FBFETCH_WS=1 opts in)" : "OFF (NVMTL_NO_FBFETCH=1)");
    }
    return on;
}
#define NVMTL_STENCIL_EMU_OFFSET 160u   /* the translator's native::stencil_export::STENCIL_EXPORT_PUSH_OFFSET */
/* the translator's stencil-export lowering: a push-constant block member at offset 160 (stencil emulation, see NVMTL_SEMU_DRAW) */
uint32_t nvmtl_spirv_stencil_emu(const void *code, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)code; const size_t nw = bytes / 4;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 72 && wc == 5 && w[i + 3] == 35 && w[i + 4] == 160u) return 1;   // OpMemberDecorate .. Offset 160
        i += wc;
    }
    return 0;
}
uint32_t nvmtl_spirv_fetch_mask(const void *code, size_t bytes)
{
    const uint32_t *w = (const uint32_t *)code; const size_t nw = bytes / 4; uint32_t m = 0;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    for (size_t i = 5; i < nw; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) break;
        if (op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 3] >= NVMTL_FETCH_BINDING && w[i + 3] < NVMTL_FETCH_BINDING + NVMTL_NCOL)
            m |= 1u << (w[i + 3] - NVMTL_FETCH_BINDING);
        i += wc;
    }
    return m;
}
static int nvmtl_fetch_layouts_locked(void)
{
    if (g_fetchLayout) return 0;
    VkDescriptorSetLayoutBinding fb[NVMTL_NCOL];
    for (uint32_t i = 0; i < NVMTL_NCOL; i++)
        fb[i] = (VkDescriptorSetLayoutBinding){ NVMTL_FETCH_BINDING + i, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1, VK_SHADER_STAGE_FRAGMENT_BIT, NULL };
    VkDescriptorSetLayoutCreateInfo fli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, nvmtl_db_layout_flags(), NVMTL_NCOL, fb };
    VkDescriptorSetLayoutCreateInfo eli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, nvmtl_db_layout_flags(), 0, NULL };
    if (pvkCreateDescriptorSetLayout(g_dev, &fli, NULL, &g_fetchLayout) != VK_SUCCESS) { g_fetchLayout = VK_NULL_HANDLE; return -1; }
    if (pvkCreateDescriptorSetLayout(g_dev, &eli, NULL, &g_emptyLayout) != VK_SUCCESS) { g_emptyLayout = VK_NULL_HANDLE; return -1; }
    return 0;
}
static void nvmtl_fetch_barrier(nvk_cmdbuf *c)
{
    // what the earlier draws (or triangles) of this pass wrote is what the next one reads (the pass's self-dependency)
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                          VK_DEPENDENCY_BY_REGION_BIT, 1, &mb, 0, NULL, 0, NULL);
}
static VkDescriptorSet nvmtl_fetch_set_make(nvk_cmdbuf *c, uint32_t mask);
static VkDescriptorSet nvmtl_fetch_set(nvk_cmdbuf *c, uint32_t mask)
{
    // One set per pass and mask, not one per draw: the set only names the pass's colour views, which do not change
    // between the draws of a pass, and allocating and writing it for each draw cost WindowServer about a millisecond
    // per frame of window drag.
    if (c->fetch_last_set && c->fetch_last_mask == mask && !memcmp(c->fetch_last_view, c->fetchview, sizeof c->fetchview))
        return (VkDescriptorSet)c->fetch_last_set;
    VkDescriptorSet set = nvmtl_fetch_set_make(c, mask);
    c->fetch_last_set = (void *)set; c->fetch_last_mask = mask;
    memcpy(c->fetch_last_view, c->fetchview, sizeof c->fetchview);
    return set;
}
static VkDescriptorSet nvmtl_fetch_set_make(nvk_cmdbuf *c, uint32_t mask)
{
    VkDescriptorSet set = VK_NULL_HANDLE; VkDescriptorPool pool = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_desc_lock);
    VkResult r = VK_ERROR_OUT_OF_POOL_MEMORY;
    for (unsigned k = g_nfetchPools; k-- > 0 && r != VK_SUCCESS;) {
        VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_fetchPools[k], 1, &g_fetchLayout };
        pool = g_fetchPools[k]; r = pvkAllocateDescriptorSets(g_dev, &ai, &set);
    }
    if (r != VK_SUCCESS && g_nfetchPools < 64) {
        VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, NVMTL_NCOL * 1024 };
        VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 1024, 1, &ps };
        if (pvkCreateDescriptorPool(g_dev, &pci, NULL, &g_fetchPools[g_nfetchPools]) == VK_SUCCESS) {
            pool = g_fetchPools[g_nfetchPools++];
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, pool, 1, &g_fetchLayout };
            r = pvkAllocateDescriptorSets(g_dev, &ai, &set);
        }
    }
    pthread_mutex_unlock(&g_desc_lock);
    if (r != VK_SUCCESS) return VK_NULL_HANDLE;
    if (c->nsets == c->capsets) {
        unsigned cap = c->capsets ? c->capsets * 2 : 64;
        nvk_setref *grown = (nvk_setref *)realloc(c->sets, cap * sizeof *grown);
        if (!grown) { pthread_mutex_lock(&g_desc_lock); pvkFreeDescriptorSets(g_dev, pool, 1, &set); pthread_mutex_unlock(&g_desc_lock); return VK_NULL_HANDLE; }
        c->sets = grown; c->capsets = cap;
    }
    c->sets[c->nsets++] = (nvk_setref){ pool, set };   // freed with the command buffer's other sets
    VkDescriptorImageInfo ii[NVMTL_NCOL]; VkWriteDescriptorSet w[NVMTL_NCOL]; uint32_t nw = 0;
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
        if (!(mask & (1u << i))) continue;
        // A [[color(n)]] the pass has no attachment for reads the zeroed dummy image (Metal leaves that read undefined).
        // Core Animation's fragment shaders declare color(0), color(1) and color(2) and draw in passes with one colour
        // attachment: refusing those draws dropped every background fill, so in Terminal cleared screens, erased lines
        // and backspaced letters stayed on screen (2.1.0, which added framebuffer fetch).
        void *view = c->fetchview[i] ? c->fetchview[i] : g_dummyImg.view;
        if (!view) return VK_NULL_HANDLE;
        ii[nw] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (VkImageView)view,
                                          c->fetchview[i] ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
        w[nw] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_FETCH_BINDING + i, 0, 1,
                                        VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &ii[nw], NULL, NULL };
        nw++;
    }
    if (nw) pvkUpdateDescriptorSets(g_dev, nw, w, 0, NULL);
    return set;
}

/* With framebuffer fetch off (the WindowServer default) a fragment shader that reads [[color(n)]] still has the input
 * attachment descriptors at set 3, and nothing was bound there: the read was undefined. Core Animation's uber fragment
 * shader reads color(0..2) at its entry, so every compositor pass that uses it depends on that read. One set, made once,
 * points every binding at the zeroed dummy image, the same image a fetch pass uses for an attachment it does not have. */
static VkDescriptorSet nvmtl_fetch_zero_set(void)
{
    static VkDescriptorSet zero;
    if (zero) return zero;
    if (!g_dummyImg.view) return VK_NULL_HANDLE;
    pthread_mutex_lock(&g_desc_lock);
    if (!zero && !nvmtl_fetch_layouts_locked()) {
        VkDescriptorSet set = VK_NULL_HANDLE; VkResult r = VK_ERROR_OUT_OF_POOL_MEMORY;
        for (unsigned k = g_nfetchPools; k-- > 0 && r != VK_SUCCESS;) {
            VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_fetchPools[k], 1, &g_fetchLayout };
            r = pvkAllocateDescriptorSets(g_dev, &ai, &set);
        }
        if (r != VK_SUCCESS && g_nfetchPools < 64) {
            VkDescriptorPoolSize ps = { VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, NVMTL_NCOL * 1024 };
            VkDescriptorPoolCreateInfo pci = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT, 1024, 1, &ps };
            if (pvkCreateDescriptorPool(g_dev, &pci, NULL, &g_fetchPools[g_nfetchPools]) == VK_SUCCESS) {
                VkDescriptorSetAllocateInfo ai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, g_fetchPools[g_nfetchPools++], 1, &g_fetchLayout };
                r = pvkAllocateDescriptorSets(g_dev, &ai, &set);
            }
        }
        if (r == VK_SUCCESS) {
            VkDescriptorImageInfo ii[NVMTL_NCOL]; VkWriteDescriptorSet w[NVMTL_NCOL];
            for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
                ii[i] = (VkDescriptorImageInfo){ VK_NULL_HANDLE, (VkImageView)g_dummyImg.view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL };
                w[i] = (VkWriteDescriptorSet){ VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, set, NVMTL_FETCH_BINDING + i, 0, 1, VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, &ii[i], NULL, NULL };
            }
            pvkUpdateDescriptorSets(g_dev, NVMTL_NCOL, w, 0, NULL);
            zero = set;
            nvlog("vk: framebuffer fetch is off and a pipeline reads [[color(n)]] - those reads see the dummy image, not an unbound descriptor");
        }
    }
    pthread_mutex_unlock(&g_desc_lock);
    return zero;
}

/* glub1 NVMTL_GL_UBO: shared state (the pass and its switch are with table-pc, nvmtl_gl_ubo_on) */
#include "nvmtl_gl_ubo.h"
static __thread int t_gl_ubo_off;   /* set while a twin is built: the pass does not run */
static _Atomic unsigned long long g_ubo_twin_draws, g_ubo_twins;
/* what a pipeline the pass changed was made from: its twin is the same call with the pass off. The two modules are
   shared between pipelines (Apple GL makes many pipelines of one program): one counted copy per distinct module. */
typedef struct { void *vs, *fs; size_t vn, fn; uint32_t vkfmt, depthfmt, samples, has_blend, has_vin, has_rt;
                 nvk_blend_state blend; nvk_vertex_input vin; nvk_rt rt; } nvmtl_ubo_src;
static pthread_mutex_t g_ubo_mod_lock = PTHREAD_MUTEX_INITIALIZER;
static NSCountedSet *g_ubo_mods;
static void *nvmtl_ubo_mod_get(const void *bytes, size_t n)   /* a retained NSData with these bytes */
{
    NSData *d = [NSData dataWithBytes:bytes length:n];
    pthread_mutex_lock(&g_ubo_mod_lock);
    if (!g_ubo_mods) g_ubo_mods = [NSCountedSet new];
    [g_ubo_mods addObject:d];
    NSData *shared = [g_ubo_mods member:d];
    void *r = (void *)CFBridgingRetain(shared ?: d);
    pthread_mutex_unlock(&g_ubo_mod_lock);
    return r;
}
static void nvmtl_ubo_mod_put(void *r)
{
    if (!r) return;
    pthread_mutex_lock(&g_ubo_mod_lock);
    [g_ubo_mods removeObject:(__bridge NSData *)r];
    pthread_mutex_unlock(&g_ubo_mod_lock);
    CFRelease(r);
}
static void nvmtl_ubo_src_free(void *p) { nvmtl_ubo_src *s = p; if (s) { nvmtl_ubo_mod_put(s->vs); nvmtl_ubo_mod_put(s->fs); free(s); } }
/* glub1: the promises a converted pipeline's block reads rest on, for this draw's bindings. ANV pushes from the qword
   (it must be ANV_UBO_ALIGNMENT-aligned) whole 64-byte chunks up to the block's extent: they must lie in the pages the
   bound buffer occupies (a buffer's pages are mapped; reading past its length inside them is harmless). An unbound
   slot pushes the dummy buffer (256 bytes). */
static int nvmtl_gl_ubo_bad(const nvk_cmdbuf *c, const nvk_pipeline *p)
{
    const nvmtl_bindtable *t = (const nvmtl_bindtable *)c->table;
    for (uint32_t stage = 0; stage < 2; stage++) {
        const uint32_t mask = p->ubo_mask[stage];
        for (uint32_t k = 0; mask >> k; k++) {
            if (!(mask >> k & 1)) continue;
            const uint32_t idx = p->pc_table[stage].index[k];
            const uint64_t ext = ((uint64_t)p->ubo_ext[stage][k] + 63u) & ~63ull;
            const nvmtl_slot *s = t && idx < NVMTL_NBUF ? &t->s[stage][idx] : NULL;
            if (!s || s->kind != NVMTL_SLOT_BUF) { if (ext > 256) return 1; continue; }
            const uint64_t a = s->address + s->off, end = (s->address + s->size + s->vextra + 4095u) & ~4095ull;
            if ((a & (NVMTL_GL_UBO_ALIGN - 1)) || s->off > s->size || a + ext > end) return 1;
        }
    }
    return 0;
}
static pthread_mutex_t g_ubo_lock = PTHREAD_MUTEX_INITIALIZER;
static int nvmtl_vk_pipeline_create_impl(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                         const void *vin_, uint32_t samples, const nvk_tess_desc *td, const nvk_rt *rt, void *out_);
static nvk_pipeline *nvmtl_gl_ubo_twin(nvk_pipeline *p)
{
    pthread_mutex_lock(&g_ubo_lock);
    nvk_pipeline *tw = (nvk_pipeline *)p->ubo_twin;
    if (!tw && !p->ubo_twin_failed && p->ubo_src) {
        const nvmtl_ubo_src *s = (const nvmtl_ubo_src *)p->ubo_src;
        tw = calloc(1, sizeof *tw);
        t_gl_ubo_off = 1;
        const int rc = tw ? nvmtl_vk_pipeline_create_impl(((__bridge NSData *)s->vs).bytes, s->vn, ((__bridge NSData *)s->fs).bytes, s->fn,
                                                          s->vkfmt, s->depthfmt, s->has_blend ? &s->blend : NULL,
                                                          s->has_vin ? &s->vin : NULL, s->samples, NULL, s->has_rt ? &s->rt : NULL, tw) : -1;
        t_gl_ubo_off = 0;
        if (rc) { free(tw); tw = NULL; p->ubo_twin_failed = 1; }
        else { p->ubo_twin = tw; atomic_fetch_add(&g_ubo_twins, 1); }
        nvlog("gl-ubo: a draw's binding breaks a block-read promise (address not %u-aligned or the buffer shorter than the block) - %s",
              NVMTL_GL_UBO_ALIGN, tw ? "this pipeline's twin without the pass now runs such draws" : "twin FAILED - such draws are not recorded");
    }
    pthread_mutex_unlock(&g_ubo_lock);
    return tw;
}
/* binds p or its twin for this draw; nonzero = the draw cannot run (no twin) */
static int nvmtl_gl_ubo_pick(nvk_cmdbuf *c, nvk_pipeline *p)
{
    const nvk_pipeline *run = p;
    if (nvmtl_gl_ubo_bad(c, p)) {
        run = nvmtl_gl_ubo_twin(p);
        if (!run) return 1;
        const unsigned long long k = atomic_fetch_add(&g_ubo_twin_draws, 1) + 1;
        if (!(k & (k - 1))) nvlog("gl-ubo: %llu draw(s) ran a twin (%llu twin pipeline(s))", k, (unsigned long long)atomic_load(&g_ubo_twins));
    }
    if (run->pipe && c->graphics_pipeline != run->pipe) {
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)run->pipe);
        c->graphics_pipeline = run->pipe;
    }
    return 0;
}
static void nvmtl_bind_graphics_sets(nvk_cmdbuf *c, VkPipelineLayout layout, uint32_t count,
                                    const VkDescriptorSet *sets, uint32_t offset_count, const uint32_t *offsets)
{
    int same = c->graphics_layout == (void *)layout && c->graphics_set_count == count
        && c->graphics_offset_count == offset_count;
    for (uint32_t i = 0; same && i < count; i++) same = c->graphics_sets[i] == (void *)sets[i];
    for (uint32_t i = 0; same && i < offset_count; i++) same = c->graphics_offsets[i] == offsets[i];
    if (same) return;
    pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, layout, 0, count, sets, offset_count, offsets);
    c->graphics_layout = (void *)layout; c->graphics_set_count = count; c->graphics_offset_count = offset_count;
    for (uint32_t i = 0; i < count; i++) c->graphics_sets[i] = (void *)sets[i];
    for (uint32_t i = 0; i < offset_count; i++) c->graphics_offsets[i] = offsets[i];
}

#include "nvmtl_dbuf.c"

void nvmtl_vk_cmd_bind_set(nvk_cmdbuf *c, nvk_pipeline *p)
{
    if (g_dbuf) { nvmtl_db_cmd_bind_set(c, p); return; }
    VkDescriptorSet s[3] = { nvmtl_snapshot_set_c(c, 0, p), nvmtl_snapshot_set_c(c, 1, p), g_heapSet };
    if (!s[0] || !s[1]) { c->nobind = 1; return; }
    c->nobind = 0;
    nvmtl_push_table_entries(c, p);
    if ((p->ubo_mask[0] | p->ubo_mask[1]) && nvmtl_gl_ubo_pick(c, p)) { c->nobind = 1; return; }
    if (g_sample_locations && c->sample_pattern.count)
        pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_FRAGMENT_BIT, NVMTL_SAMPLE_POSITION_OFFSET,
                           NVMTL_SAMPLE_POSITION_BYTES, c->sample_pattern.position);
    nvmtl_bind_graphics_sets(c, (VkPipelineLayout)p->layout, g_heapSet ? 3 : 2, s, nvmtl_table_ubo() ? 2 : 0, c->dyn_out);
    c->semu_layout = p->stencil_emu ? p->layout : NULL;
    c->fetch_draw = 0;
    if (p->skip_draws) { c->nobind = 1; return; }
    if (p->fetch_zero) {
        VkDescriptorSet zs = nvmtl_fetch_zero_set();
        if (zs) pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)p->layout, NVMTL_FETCH_SET, 1, &zs, 0, NULL);
        else { static int said; if (said++ < 4) nvlog("vk: no descriptor set for the [[color(n)]] reads of a pipeline with fetch off - they stay undefined"); }
    }
    if (p->fetch_mask && c->in_rp) {
        VkDescriptorSet fs = nvmtl_fetch_set(c, p->fetch_mask);
        // counted: the command buffer then ends with an error instead of running without the draw (fix30)
        if (!fs) { static int said; if (said++ < 4) nvlog("vk: framebuffer fetch set could not be made (mask 0x%x) - this draw is not recorded", p->fetch_mask); c->skipped++; c->skip_oom++; c->nobind = 1; return; }
        pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipelineLayout)p->layout, NVMTL_FETCH_SET, 1, &fs, 0, NULL);
        nvmtl_fetch_barrier(c);
        c->fetch_draw = 1;
    }
}

#define NVMTL_DEPTH_USAGE (VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | \
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT)
int nvmtl_vk_depth_create(uint32_t w, uint32_t h, nvk_image *out)
{
    return nvmtl_vk_depth_create_ex(w, h, VK_FORMAT_D32_SFLOAT, VK_IMAGE_ASPECT_DEPTH_BIT, out);
}
int nvmtl_vk_depth_create_ex(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_image *out)
{ return nvmtl_vk_depth_create_ms(w, h, vkfmt, aspect, 1, out); }
int nvmtl_vk_depth_create_ms(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, nvk_image *out)
{ return nvmtl_vk_depth_create_full(w, h, vkfmt, aspect, samples, samples > 1 ? 4u : 2u, 1u, 1u, out); }
static int nvmtl_vk_depth_create_full_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, uint32_t mtl_type,
                               uint32_t layers, uint32_t mips, nvk_image *out);
int nvmtl_vk_depth_create_full(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, uint32_t mtl_type,
                               uint32_t layers, uint32_t mips, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_depth_create_full_lifetime_body(w, h, vkfmt, aspect, samples, mtl_type, layers, mips, &pending);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
static int nvmtl_vk_depth_create_full_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, uint32_t mtl_type,
                               uint32_t layers, uint32_t mips, nvk_image *out)
{
    if (nvmtl_vk_init()) return -1;
    if (!layers) layers = 1;
    if (!mips) mips = 1;
    if (mtl_type == 5 && layers != 6) layers = 6;
    const int cube = mtl_type == 5 || mtl_type == 6;
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->fmt = vkfmt; out->samples = samples > 1 ? samples : 1;
    /* bytes per texel of a buffer copy of this aspect: Depth16 2, Stencil8 1, the 32-bit depths (and D24, packed in 32 bits) 4.
     * WAS: 4 for all, so a buffer upload to a Depth16 texture had its 8-byte rows refused (Dome Keeper's 4x4 Depth16) */
    out->bpp = vkfmt == VK_FORMAT_D16_UNORM ? 2 : vkfmt == VK_FORMAT_S8_UINT ? 1 : 4;
    out->mtl_type = mtl_type == 3 || cube || mtl_type == 8 ? mtl_type : (samples > 1 ? 4 : 2); out->layers = layers; out->mips = mips;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, VK_IMAGE_TYPE_2D,
        (VkFormat)vkfmt, { w, h, 1 }, mips, layers, (VkSampleCountFlagBits)out->samples, VK_IMAGE_TILING_OPTIMAL,
        NVMTL_DEPTH_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage(depth)");
    out->usage = (uint32_t)ici.usage;
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, out->img, &mr);
    VKCK(nvmtl_alloc_opaque_image_mem(&mr, &out->mem, &out->sysmem, "depth"), "vkAllocateMemory(depth)");
    out->alloc = mr.size;
    VKCK(pvkBindImageMemory(g_dev, out->img, out->mem, 0), "vkBindImageMemory(depth)");
    VkImageViewType vt = out->mtl_type == 3 || out->mtl_type == 8 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : out->mtl_type == 5 ? VK_IMAGE_VIEW_TYPE_CUBE
                       : out->mtl_type == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, vt,
        (VkFormat)vkfmt, { 0 }, { (VkImageAspectFlags)aspect, 0, mips, 0, layers } };
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView(depth)");
    return 0;
}

void nvmtl_vk_cmd_set_depth(nvk_cmdbuf *c, int test, int write, int compare)
{
    c->depth_write = write;
    pvkCmdSetDepthTestEnable(c->cb, test ? VK_TRUE : VK_FALSE);
    pvkCmdSetDepthWriteEnable(c->cb, write ? VK_TRUE : VK_FALSE);
    pvkCmdSetDepthCompareOp(c->cb, (VkCompareOp)compare);
}

int nvmtl_vk_pipeline_create_depth(const void *vs, size_t vn, const void *fs, size_t fn, int bgra, int hasDepth, nvk_pipeline *out)
{
    return nvmtl_vk_pipeline_create_ex(vs, vn, fs, fn, bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, hasDepth, out);
}
int nvmtl_vk_pipeline_create_ex(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, int hasDepth, nvk_pipeline *out)
{
    return nvmtl_vk_pipeline_create_ex2(vs, vn, fs, fn, vkfmt, hasDepth ? VK_FORMAT_D32_SFLOAT : 0, out);
}
#include "nvmtl_physical_bounds.h"
#include "nvmtl_independent_helpers.h"
static NSData *nvmtl_safe_physical_shader(const void *data, size_t len) {
    if (len % sizeof(uint32_t)) { nvlog("physical bounds: malformed shader size"); return nil; }
    NSMutableData *result = [NSMutableData dataWithBytes:data length:len];
    int n = nvmtl_relax_physical_bounds(result.mutableBytes, len / sizeof(uint32_t));
    if (n < 0) { nvlog("physical bounds: malformed shader or allocation failure"); return nil; }
    if (n) nvlog("physical bounds: relaxed %d allocation-relative access chains", n);
    return nvmtl_independent_helpers(result);
}
#include "nvmtl_readonly_metadata.h"
static int nvmtl_align_guard_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_NO_ALIGN_GUARD"); on = !(e && e[0] == '1');
        nvlog("align guard: %s (NVMTL_NO_ALIGN_GUARD=%s, translator %s)", on ? "ON - a misaligned binding runs the SAFE module, as Apple runs it" : "OFF",
              e ? e : "unset", xlate_lower_ex ? "reports promises" : "has NO nvmtl_lower_buffer_addresses_ex - guard inert"); }
    return on;
}
static _Atomic unsigned long long g_ag_render_safe, g_ag_untraceable, g_ag_guarded, g_ag_switched;
static int nvmtl_spirv_ssbo_table(const void *spv, size_t n)
{
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4;
    if (nw < 5 || w[0] != 0x07230203u) return 0;
    uint32_t tid = 0;
    for (size_t i = 5; i < nw; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffffu; if (!len || i + len > nw) return 0;
        if (op == 71 && len >= 4 && w[i + 2] == 33 && w[i + 3] == 640) tid = w[i + 1];
        i += len; }
    if (!tid) return 0;
    for (size_t i = 5; i < nw; ) { uint32_t len = w[i] >> 16, op = w[i] & 0xffffu; if (!len || i + len > nw) return 0;
        if (op == 59 && len >= 4 && w[i + 2] == tid) return w[i + 3] == 12;
        i += len; }
    return 0;
}
static _Atomic unsigned long long g_g1_refused;
static NSData *nvmtl_address_shader_g0(const void *data, size_t len, uint32_t flags, uint32_t *info);
static NSData *nvmtl_address_shader_g_uncached(const void *data, size_t len, uint32_t flags, uint32_t *info);
static NSData *nvmtl_address_shader_g(const void *data, size_t len, uint32_t flags, uint32_t *info) {
    static NSMutableDictionary *cache; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    NSMutableData *key = [NSMutableData dataWithBytes:&flags length:sizeof flags];
    [key appendBytes:data length:len];
    pthread_mutex_lock(&lk);
    NSArray *hit = cache[key];
    pthread_mutex_unlock(&lk);
    if (hit) {
        if (info) memcpy(info, [hit[1] bytes], 4 * sizeof *info);
        return hit[0] == (id)[NSNull null] ? nil : hit[0];
    }
    uint32_t got[4] = {0};
    NSData *d = nvmtl_address_shader_g_uncached(data, len, flags, got);
    if (info) memcpy(info, got, sizeof got);
    pthread_mutex_lock(&lk);
    if (!cache || cache.count >= 4096) cache = [NSMutableDictionary new];
    cache[key] = @[d ?: (id)[NSNull null], [NSData dataWithBytes:got length:sizeof got]];
    pthread_mutex_unlock(&lk);
    return d;
}
static _Atomic unsigned long long g_g1_descriptors;
static NSData *nvmtl_address_shader_g_uncached(const void *data, size_t len, uint32_t flags, uint32_t *info) {
    NSData *d = nvmtl_address_shader_g0(data, len, flags, info);
    if (d && nvmtl_table_ubo() && nvmtl_spirv_ssbo_table(d.bytes, d.length)) {
        // Address lowering refused this module, so its buffers are still descriptors and its table at 640 is still a
        // storage buffer. The descriptor path serves such a module as it did before the table became a Uniform block;
        // only the table has to move with it.
        uint8_t *out = NULL; size_t n = 0; char err[512] = {0};
        if (xlate_to_uniform && !xlate_to_uniform(d.bytes, d.length, &out, &n, err, sizeof err)) {
            NSData *moved = [NSData dataWithBytes:out length:n]; xlate_free(out, n);
            if (!nvmtl_spirv_ssbo_table(moved.bytes, moved.length)) {
                const unsigned long long k = atomic_fetch_add(&g_g1_descriptors, 1) + 1;
                if (k <= 16 || !(k & (k - 1))) nvlog("G1: a module address lowering refused keeps its buffer descriptors; its table at 640 moved to a Uniform block (%llu so far)", k);
                return moved;
            }
        }
        const unsigned long long k = atomic_fetch_add(&g_g1_refused, 1) + 1;
        if (k <= 16 || !(k & (k - 1))) nvlog("G1: a module still declares a STORAGE-buffer table at 640 while the table is Uniform - REFUSED (%llu so far; %s; NVMTL_TABLE_SSBO=1 restores it)", k,
                                             xlate_to_uniform ? (err[0] ? err : "the table did not move") : "the translator has no nvmtl_table_to_uniform");
        return nil;
    }
    return d;
}
static NSData *nvmtl_address_shader_g0(const void *data, size_t len, uint32_t flags, uint32_t *info) {
    if (info) memset(info, 0, 4 * sizeof *info);
    __attribute__((objc_precise_lifetime)) NSData *prepared = nvmtl_preserve_readonly_metadata([NSData dataWithBytes:data length:len]);
    data = prepared.bytes; len = prepared.length;
    static unsigned said;
    if (!nvmtl_xlate_ready() || !xlate_lower || !xlate_free) {
        if (said++ < 40) nvlog("address lowering UNAVAILABLE (translator exports no nvmtl_lower_buffer_addresses): shader kept as-is");
        return nvmtl_safe_physical_shader(data, len);
    }
    uint8_t *out = NULL; size_t n = 0; char err[512] = {0};
    const int ubo = nvmtl_table_ubo();
    // the translator indexes descriptor-kept buffers from their offset rounded down to the device's alignment
    // (flags bits 8..15: its log2; buffer_addresses::lower_with), which the descriptor writes below bind them at
    const int ex = xlate_lower_ex != NULL;
    const uint32_t ra = nvmtl_ssbo_align(); uint32_t lg = 0; while ((1u << lg) < ra && lg < 8) lg++;
    if (ex ? xlate_lower_ex(data, len, flags | (ubo ? 2u : 0u) | (lg << 8), &out, &n, info, err, sizeof err) : xlate_lower(data, len, &out, &n, err, sizeof err)) {
        if (info) memset(info, 0, 4 * sizeof *info);
        if (said++ < 40) nvlog("address lowering REFUSED, shader kept as-is: %s", err);
        return nvmtl_safe_physical_shader(data, len);
    }
    if (info && !nvmtl_align_guard_on()) memset(info, 0, 4 * sizeof *info);
    NSData *result = nvmtl_safe_physical_shader(out, n); xlate_free(out, n); return result;
}
static NSData *nvmtl_address_shader(const void *data, size_t len) {
    uint32_t info[4];
    NSData *d = nvmtl_address_shader_g(data, len, 0, info);
    if (d && (info[0] || info[2])) {
        const unsigned long long k = atomic_fetch_add(&g_ag_render_safe, 1) + 1;
        if (!(k & (k - 1))) nvlog("align guard: %llu render module(s) built SAFE - %u access(es) rested on a promised %u-byte alignment (bindings %#x%s)",
                                  k, info[3], info[1], info[0], info[2] ? ", untraceable" : "");
        d = nvmtl_address_shader_g(data, len, 1, NULL);
    }
    return d;
}
static NSData *nvmtl_pc_table_shader(NSData *source, uint32_t model, nvmtl_pc_table *plan)
{
    size_t bytes = 0;
    uint32_t *moved = nvmtl_spirv_table_pc(source.bytes, source.length, model,
        model ? NVMTL_PC_TABLE_FS_OFFSET : NVMTL_PC_TABLE_VS_OFFSET, model ? 10 : NVMTL_PC_TABLE_MAX, plan, &bytes);
    NSData *result = moved ? [NSData dataWithBytesNoCopy:moved length:bytes freeWhenDone:YES] : source;
    static _Atomic unsigned applied[2], refused[2];
    const unsigned k = atomic_fetch_add(moved ? &applied[!!model] : &refused[!!model], 1) + 1;
    if (k <= 8 || !(k & (k - 1))) nvlog("table-pc %s: %s %u entries (%u %s)", model ? "fragment" : "vertex",
        moved ? "compact push constants," : "descriptor fallback,", plan->count, k, moved ? "converted" : "unchanged");
    const char *dump = getenv("NVMTL_PC_DUMP_DIR");
    if (dump && *dump && k <= 256) {
        uint32_t h = 2166136261u; const uint8_t *b = source.bytes;
        for (NSUInteger j = 0; j < source.length; j++) { h ^= b[j]; h *= 16777619u; }
        NSString *root = @(dump);
        [[NSFileManager defaultManager] createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *stem = [NSString stringWithFormat:@"%08x.%@", h, model ? @"frag" : @"vert"];
        [source writeToFile:[root stringByAppendingPathComponent:[stem stringByAppendingString:@".before.spv"]] atomically:YES];
        if (moved) [result writeToFile:[root stringByAppendingPathComponent:[stem stringByAppendingString:@".after.spv"]] atomically:YES];
    }
    return result;
}
static int nvmtl_pc_table_on(void)
{
    static int on; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("NVMTL_PC_TABLE");
        /* General Intel/ANV default, independent of the application. Other GPU
           backends keep their default until validated; explicit 1 opts in and 0
           opts out. Shader eligibility still conservatively keeps descriptors. */
        const uint16_t vendor = nvmtl_vk_vendor();
        const int want = e && *e ? !strcmp(e, "1") : vendor == 0x8086;
        on = want && !g_dbuf && g_lim.maxPushConstantsSize >= 256;
        nvlog("table-pc: %s (NVMTL_PC_TABLE=%s, vendor %#x, classic %d, maxPushConstants %u)",
            on ? "ON for eligible graphics modules" : "OFF", e && *e ? e : "default", vendor, !g_dbuf, g_lim.maxPushConstantsSize);
    });
    return on;
}
/* glub1 NVMTL_GL_UBO (nvmtl_gl_ubo.h): a table-pc module's constant-index reads of a read-only buffer whose address it
   pushes become one 64-aligned block read, which ANV pushes into registers (anv_nir_is_pushable_pointer) instead of a
   load.ugm SEND per read. Apple GL's default-block uniforms (setVertexBytes/setFragmentBytes, buffer 0) are the main
   case; any Metal constant buffer read at constant offsets qualifies. No descriptor, layout or push range changes, and
   no per-draw CPU work beyond a check: a draw whose pushed address is not 64-aligned, or whose buffer does not hold the
   block, binds the pipeline built without the pass (ubo_twin, made on first need). prod9: default 1 (B580 A/B, 4 Nov:
   Minecraft GPU frame time -13%, glub_twin 0/6, gl_bind_stream hashes equal in 0/1/2); unset = 1.
   1 = every eligible stage (most GPU work removed). 2 = only stages the pass leaves with no other read of their pushed
   qwords, so ANV drops the stage's client push data (never more CPU per draw; offline: -20..-40 ns/draw on those,
   +0..+20 ns on stages that keep client data and gain pointer ranges, e.g. a heap-texture qword). 0 = off. */
static int nvmtl_gl_ubo_on(void)
{
    static int on; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("NVMTL_GL_UBO");
        const int want = e && *e ? (!strcmp(e, "1") ? 1 : !strcmp(e, "2") ? 2 : 0) : 1;   /* prod9: unset = 1 */
        on = want && nvmtl_pc_table_on() && !g_dbuf ? want : 0;
        nvlog("gl-ubo: %s (NVMTL_GL_UBO=%s, vendor %#x, table-pc %d) - constant reads of a pushed buffer address become one "
              "%u-aligned block read that ANV pushes into registers", on == 1 ? "ON for every eligible stage" : on == 2 ?
              "ON for stages left with no client push data" : "OFF", e && *e ? e : "unset (prod9 default 1)",
              nvmtl_vk_vendor(), nvmtl_pc_table_on(), NVMTL_GL_UBO_ALIGN);
    });
    return on;
}
static NSData *nvmtl_gl_ubo_shader(NSData *source, uint32_t model, const nvmtl_pc_table *plan, uint32_t *mask, uint32_t *ext)
{
    *mask = 0;
    if (!plan->count) return source;
    nvmtl_gl_ubo_info info; size_t bytes = 0;
    uint32_t *out = nvmtl_spirv_gl_ubo(source.bytes, source.length, model ? NVMTL_PC_TABLE_FS_OFFSET : NVMTL_PC_TABLE_VS_OFFSET,
                                       plan, &info, &bytes);
    if (out && nvmtl_gl_ubo_on() == 2 && !info.complete) { free(out); out = NULL; }   /* mode 2: this stage keeps client data */
    static _Atomic unsigned applied[2], left[2];
    const unsigned k = atomic_fetch_add(out ? &applied[!!model] : &left[!!model], 1) + 1;
    if (k <= 8 || !(k & (k - 1))) {
        if (out) nvlog("gl-ubo %s: %u block(s), %u load(s) + %u re-cast read(s) (%u words) now from registers, entries %#x%s (%u converted)",
                       model ? "fragment" : "vertex", info.bases, info.loads, info.recasts, info.words, info.mask,
                       info.complete ? ", no client push data left" : "", k);
        else nvlog("gl-ubo %s: unchanged - no constant read of a read-only 32-bit buffer it pushes%s (%u unchanged)", model ? "fragment" : "vertex",
                   nvmtl_gl_ubo_on() == 2 ? ", or (mode 2) the stage would keep client push data" : "", k);
    }
    if (!out) return source;
    *mask = info.mask; memcpy(ext, info.extent, sizeof info.extent);
    NSData *result = [NSData dataWithBytesNoCopy:out length:bytes freeWhenDone:YES];
    const char *dump = getenv("NVMTL_PC_DUMP_DIR");
    if (dump && *dump && k <= 256) {
        uint32_t h = 2166136261u; const uint8_t *b = source.bytes;
        for (NSUInteger j = 0; j < source.length; j++) { h ^= b[j]; h *= 16777619u; }
        NSString *root = @(dump);
        [[NSFileManager defaultManager] createDirectoryAtPath:root withIntermediateDirectories:YES attributes:nil error:nil];
        NSString *stem = [NSString stringWithFormat:@"%08x.%@", h, model ? @"frag" : @"vert"];
        [source writeToFile:[root stringByAppendingPathComponent:[stem stringByAppendingString:@".pc.spv"]] atomically:YES];
        [result writeToFile:[root stringByAppendingPathComponent:[stem stringByAppendingString:@".ubo.spv"]] atomically:YES];
    }
    return result;
}
/* compute2: NVMTL_CS_PC_TABLE - the compute counterpart of table-pc. prod7 converted graphics modules only: a kernel
   already has one static push block (the 48-byte dispatch region, translator stage_input/kernel_grid.rs) and Vulkan
   allows one per entry point, so nvmtl_spirv_table_pc refused every kernel that reads its grid. Here the table qwords
   are appended to that block at [48, 48+8n) (NVMTL_PC_MERGE) and pushed with the dispatch values. Same eligibility as
   graphics (constant indices, no self address, no escaping pointer, at most 12 entries, table in set 0); anything else
   keeps the descriptor table. ANV never pushes UBO ranges for compute (anv_nir_compute_push_layout.c: push_ubo_ranges
   requires stage != COMPUTE), so on prod7 each kernel loads its buffer addresses from the system-RAM table page with a
   SEND; here they arrive with the thread payload. Default as graphics: on for Intel in the classic model, 0 = off. */
static int nvmtl_cs_pc_table_on(void)
{
    static int on; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *e = getenv("NVMTL_CS_PC_TABLE");
        const uint16_t vendor = nvmtl_vk_vendor();
        const int want = e && *e ? !strcmp(e, "1") : vendor == 0x8086;
        on = want && !g_dbuf && g_lim.maxPushConstantsSize >= NVMTL_PC_TABLE_CS_OFFSET + 8;
        nvlog("cs-table-pc: %s (NVMTL_CS_PC_TABLE=%s, vendor %#x, classic %d, maxPushConstants %u)",
            on ? "ON for eligible compute modules" : "OFF", e && *e ? e : "default", vendor, !g_dbuf, g_lim.maxPushConstantsSize);
    });
    return on;
}
static NSData *nvmtl_cs_pc_table_shader(NSData *source, nvmtl_pc_table *plan)
{
    uint32_t cap = (g_lim.maxPushConstantsSize - NVMTL_PC_TABLE_CS_OFFSET) / 8;
    if (cap > NVMTL_PC_TABLE_MAX) cap = NVMTL_PC_TABLE_MAX;
    /* NVMTL_CS_PC_MAXQ=<n>: convert only kernels reading at most n qwords. ANV puts at most 16 used push dwords in
       COMPUTE_WALKER inline data on Xe2 (anv_nir_compute_push_layout.c compute_final_push_range); past that the kernel
       loads its push data from the dynamic-state pool with a SEND, which replaces the table load instead of removing it
       (cs_offline: 1-2 qwords remove one SEND, 4 qwords add one). An A/B knob for the hardware, default 12. */
    { static int mq = -1; if (mq < 0) { const char *e = getenv("NVMTL_CS_PC_MAXQ"); mq = e && *e ? atoi(e) : NVMTL_PC_TABLE_MAX; }
      if (mq >= 0 && (uint32_t)mq < cap) cap = (uint32_t)mq; }
    if (!cap) { memset(plan, 0, sizeof *plan); return source; }
    size_t bytes = 0;
    uint32_t *moved = nvmtl_spirv_table_pc_ex(source.bytes, source.length, 5, NVMTL_PC_TABLE_CS_OFFSET, cap,
                                              NVMTL_PC_MERGE | NVMTL_PC_SET0, plan, &bytes);
    if (!moved) memset(plan, 0, sizeof *plan);
    static _Atomic unsigned applied, refused;
    const unsigned k = atomic_fetch_add(moved ? &applied : &refused, 1) + 1;
    if (k <= 8 || !(k & (k - 1))) nvlog("cs-table-pc: %s %u entries (%u %s)", moved ? "push constants at [48,...)," : "descriptor fallback,",
        plan->count, k, moved ? "converted" : "unchanged");
    return moved ? [NSData dataWithBytesNoCopy:moved length:bytes freeWhenDone:YES] : source;
}
/* the qwords a converted kernel reads, from set 0's slots as the table snapshot would hold them at this dispatch */
static uint32_t nvmtl_cs_pc_values(const nvk_cmdbuf *c, const nvk_pipeline *p, uint32_t *pc)
{
    const nvmtl_pc_table *pt = &p->pc_table[0];
    if (!pt->count) return NVMTL_DISPATCH_PC_BYTES;
    const nvmtl_bindtable *t = (const nvmtl_bindtable *)c->table;
    for (uint32_t k = 0; k < pt->count; k++) {
        const uint32_t idx = pt->index[k], slot = idx < NVMTL_NBUF ? idx : idx - NVMTL_NBUF;
        const uint64_t v = t ? nvmtl_tab_entry(&t->s[0][slot], slot) : idx < NVMTL_NBUF ? nvmtl_dummy_addr() : 0;
        memcpy(pc + 12 + 2 * k, &v, 8);
    }
    return NVMTL_DISPATCH_PC_BYTES + pt->count * 8;
}
int nvmtl_vk_pipeline_create_ex2(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, nvk_pipeline *out)
{
    return nvmtl_vk_pipeline_create_blend(vs, vn, fs, fn, vkfmt, depthfmt, NULL, out);
}
static void nvmtl_spv_input_kinds(const void *spv, size_t n, uint8_t kind[32])
{
    const uint32_t *w = (const uint32_t *)spv; size_t nw = n / 4; memset(kind, 0, 32);
    if (nw < 5 || w[0] != 0x07230203u) return;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return;
    uint8_t *tk = calloc(bound, 1), *loc = calloc(bound, 1);
    if (tk && loc) for (size_t i = 5; i < nw; ) {
        uint32_t len = w[i] >> 16, op = w[i] & 0xffffu; if (!len || i + len > nw) break;
        const uint32_t *a = w + i + 1;
        if (op == 71 && len >= 4 && a[0] < bound && a[1] == 30 && a[2] < 32) loc[a[0]] = (uint8_t)(a[2] + 1);
        else if (op == 22 && len >= 3 && a[0] < bound) tk[a[0]] = 1;
        else if (op == 21 && len >= 4 && a[0] < bound) tk[a[0]] = a[2] ? 2 : 3;
        else if (op == 23 && len >= 4 && a[0] < bound && a[1] < bound) tk[a[0]] = tk[a[1]];
        else if (op == 32 && len >= 4 && a[0] < bound && a[2] < bound) tk[a[0]] = tk[a[2]];
        else if (op == 59 && len >= 4 && a[0] < bound && a[1] < bound && a[2] == 1 && loc[a[1]]) kind[loc[a[1]] - 1] = tk[a[0]] ? tk[a[0]] : 1;
        i += len;
    }
    free(tk); free(loc);
}
static int nvmtl_vk_format_is_vertex(VkFormat f)
{
    static void (*fp)(VkPhysicalDevice, VkFormat, VkFormatProperties *); static int tried;
    if (!tried) { tried = 1; fp = (void (*)(VkPhysicalDevice, VkFormat, VkFormatProperties *))pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
                  if (!fp) nvlog("G16 vk: vkGetPhysicalDeviceFormatProperties is missing — vertex formats are taken on trust"); }
    if (!fp) return 1;
    VkFormatProperties p; memset(&p, 0, sizeof p); fp(g_pdev, f, &p);
    return (p.bufferFeatures & VK_FORMAT_FEATURE_VERTEX_BUFFER_BIT) != 0;
}
_Static_assert(VK_FORMAT_R8G8_UINT - 2 == VK_FORMAT_R8G8_USCALED && VK_FORMAT_R8G8_SINT - 2 == VK_FORMAT_R8G8_SSCALED, "INT-2 = SCALED (8-bit)");
_Static_assert(VK_FORMAT_R8_UINT - 2 == VK_FORMAT_R8_USCALED && VK_FORMAT_R8G8B8A8_SINT - 2 == VK_FORMAT_R8G8B8A8_SSCALED, "INT-2 = SCALED (8-bit)");
_Static_assert(VK_FORMAT_R16_UINT - 2 == VK_FORMAT_R16_USCALED && VK_FORMAT_R16G16B16A16_SINT - 2 == VK_FORMAT_R16G16B16A16_SSCALED, "INT-2 = SCALED (16-bit)");
_Static_assert(VK_FORMAT_R8G8B8_UNORM + 14 == VK_FORMAT_R8G8B8A8_UNORM && VK_FORMAT_R8G8B8_SINT + 14 == VK_FORMAT_R8G8B8A8_SINT, "RGB8+14 = RGBA8");
_Static_assert(VK_FORMAT_R16G16B16_UNORM + 7 == VK_FORMAT_R16G16B16A16_UNORM && VK_FORMAT_R16G16B16_SFLOAT + 7 == VK_FORMAT_R16G16B16A16_SFLOAT, "RGB16+7 = RGBA16");
static VkFormat nvmtl_vertex_vkformat(uint32_t mtl, int kind)
{
    static const struct { uint8_t mtl, isint; VkFormat vk; } T[] = {
        { 1, 1, VK_FORMAT_R8G8_UINT }, { 2, 1, VK_FORMAT_R8G8B8_UINT }, { 3, 1, VK_FORMAT_R8G8B8A8_UINT },
        { 4, 1, VK_FORMAT_R8G8_SINT }, { 5, 1, VK_FORMAT_R8G8B8_SINT }, { 6, 1, VK_FORMAT_R8G8B8A8_SINT },
        { 7, 0, VK_FORMAT_R8G8_UNORM }, { 8, 0, VK_FORMAT_R8G8B8_UNORM }, { 9, 0, VK_FORMAT_R8G8B8A8_UNORM },
        { 10, 0, VK_FORMAT_R8G8_SNORM }, { 11, 0, VK_FORMAT_R8G8B8_SNORM }, { 12, 0, VK_FORMAT_R8G8B8A8_SNORM },
        { 13, 1, VK_FORMAT_R16G16_UINT }, { 14, 1, VK_FORMAT_R16G16B16_UINT }, { 15, 1, VK_FORMAT_R16G16B16A16_UINT },
        { 16, 1, VK_FORMAT_R16G16_SINT }, { 17, 1, VK_FORMAT_R16G16B16_SINT }, { 18, 1, VK_FORMAT_R16G16B16A16_SINT },
        { 19, 0, VK_FORMAT_R16G16_UNORM }, { 20, 0, VK_FORMAT_R16G16B16_UNORM }, { 21, 0, VK_FORMAT_R16G16B16A16_UNORM },
        { 22, 0, VK_FORMAT_R16G16_SNORM }, { 23, 0, VK_FORMAT_R16G16B16_SNORM }, { 24, 0, VK_FORMAT_R16G16B16A16_SNORM },
        { 25, 0, VK_FORMAT_R16G16_SFLOAT }, { 26, 0, VK_FORMAT_R16G16B16_SFLOAT }, { 27, 0, VK_FORMAT_R16G16B16A16_SFLOAT },
        { 28, 0, VK_FORMAT_R32_SFLOAT }, { 29, 0, VK_FORMAT_R32G32_SFLOAT }, { 30, 0, VK_FORMAT_R32G32B32_SFLOAT }, { 31, 0, VK_FORMAT_R32G32B32A32_SFLOAT },
        { 32, 2, VK_FORMAT_R32_SINT }, { 33, 2, VK_FORMAT_R32G32_SINT }, { 34, 2, VK_FORMAT_R32G32B32_SINT }, { 35, 2, VK_FORMAT_R32G32B32A32_SINT },
        { 36, 2, VK_FORMAT_R32_UINT }, { 37, 2, VK_FORMAT_R32G32_UINT }, { 38, 2, VK_FORMAT_R32G32B32_UINT }, { 39, 2, VK_FORMAT_R32G32B32A32_UINT },
        { 40, 0, VK_FORMAT_A2B10G10R10_SNORM_PACK32 }, { 41, 0, VK_FORMAT_A2B10G10R10_UNORM_PACK32 }, { 42, 0, VK_FORMAT_B8G8R8A8_UNORM },
        { 45, 1, VK_FORMAT_R8_UINT }, { 46, 1, VK_FORMAT_R8_SINT }, { 47, 0, VK_FORMAT_R8_UNORM }, { 48, 0, VK_FORMAT_R8_SNORM },
        { 49, 1, VK_FORMAT_R16_UINT }, { 50, 1, VK_FORMAT_R16_SINT }, { 51, 0, VK_FORMAT_R16_UNORM }, { 52, 0, VK_FORMAT_R16_SNORM },
        { 53, 0, VK_FORMAT_R16_SFLOAT }, { 54, 0, VK_FORMAT_B10G11R11_UFLOAT_PACK32 }, { 55, 0, VK_FORMAT_E5B9G9R9_UFLOAT_PACK32 } };
    VkFormat v = VK_FORMAT_UNDEFINED; int isint = 0;
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) if (T[i].mtl == mtl) { v = T[i].vk; isint = T[i].isint; break; }
    if (v == VK_FORMAT_UNDEFINED) return v;
    if (isint == 1 && kind == 1) v = (VkFormat)(v - 2);
    else if (isint == 2 && kind == 1) { static int said; if (said++ < 4) nvlog("G16 vertex format %u: a 32-bit integer read as float has no Vulkan format — bound as integer, values will be WRONG", mtl); }
    if (!nvmtl_vk_format_is_vertex(v)) {
        VkFormat wide = (v >= VK_FORMAT_R8G8B8_UNORM && v <= VK_FORMAT_R8G8B8_SINT) ? (VkFormat)(v + 14)
                      : (v >= VK_FORMAT_R16G16B16_UNORM && v <= VK_FORMAT_R16G16B16_SFLOAT) ? (VkFormat)(v + 7) : VK_FORMAT_UNDEFINED;
        if (wide != VK_FORMAT_UNDEFINED && nvmtl_vk_format_is_vertex(wide)) return wide;
        nvlog("G16 vertex format %u -> VkFormat %d is not a vertex-buffer format on this device", mtl, (int)v); return VK_FORMAT_UNDEFINED;
    }
    return v;
}
static int nvmtl_vertex_input_build(const nvk_vertex_input *vin, const void *vs, size_t vn, VkVertexInputBindingDescription *vib, uint32_t *nvib,
                                    VkVertexInputAttributeDescription *via, uint32_t *nvia, VkVertexInputBindingDivisorDescriptionKHR *vdiv,
                                    uint32_t *ndiv, uint32_t *mask, uint32_t *dyn, uint32_t stv[32])
{
    *nvib = *nvia = *ndiv = *mask = *dyn = 0; memset(stv, 0, 32 * sizeof *stv);
    uint8_t kind[32]; nvmtl_spv_input_kinds(vs, vn, kind);
    uint32_t described = 0;
    for (uint32_t k = 0; vin && k < vin->nattr && k < NVMTL_NVIN; k++) {
        uint32_t loc = vin->attr[k].location, buf = vin->attr[k].buffer;
        if (loc >= NVMTL_NVIN || buf >= NVMTL_NVIN) { nvlog("G16 pipeline: vertex attribute %u / buffer %u is outside [0,%d)", loc, buf, NVMTL_NVIN); return -1; }
        described |= 1u << loc;
        if (!kind[loc]) continue;
        VkFormat f = nvmtl_vertex_vkformat(vin->attr[k].mtlfmt, kind[loc]);
        if (f == VK_FORMAT_UNDEFINED) { nvlog("G16 pipeline: MTLVertexFormat %u at attribute %u is not carried", vin->attr[k].mtlfmt, loc); return -1; }
        via[*nvia] = (VkVertexInputAttributeDescription){ loc, buf, f, vin->attr[k].offset }; (*nvia)++;
        if (*mask & (1u << buf)) continue;
        *mask |= 1u << buf;
        uint32_t step = vin->layout[buf].step, stride = step == 0 ? 0 : vin->layout[buf].stride;
        if (stride == NVMTL_STRIDE_DYNAMIC) { *dyn |= 1u << buf; stride = 0; }
        stv[buf] = stride;
        if (step > 2) { nvlog("G16 pipeline: vertex step function %u (per patch) is tessellation — not carried", step); return -1; }
        const uint32_t rate = vin->layout[buf].rate;
        if (step == 2 && rate != 1) {
            if (g_divisor && rate && rate <= g_max_divisor) vdiv[(*ndiv)++] = (VkVertexInputBindingDivisorDescriptionKHR){ buf, rate };
            else { static int said; if (said++ < 4) nvlog("G16 pipeline: per-instance step rate %u needs VK_KHR_vertex_attribute_divisor "
                "(%s) — stepping once per instance", rate, g_divisor ? "beyond its limit" : "absent"); }
        }
        vib[*nvib] = (VkVertexInputBindingDescription){ buf, stride, step == 2 ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX }; (*nvib)++;
    }
    for (uint32_t l = 0; l < NVMTL_NVIN; l++) if (kind[l] && !(described & (1u << l))) { static int said; if (said++ < 8)
        nvlog("G16 pipeline: the vertex shader reads attribute %u and the vertex descriptor does not describe it — it reads undefined values", l); }
    return 0;
}
static uint32_t nvmtl_rt_shape(const nvk_pipeline *p, uint32_t cf[NVMTL_NCOL], uint32_t *rmask)
{
    memset(cf, 0, NVMTL_NCOL * sizeof *cf);
    *rmask = p->rmask;
    if (!p->ncol && p->cfmt) { cf[0] = p->cfmt; *rmask = p->samples > 1 ? 1u : 0u; return 1; }
    uint32_t n = p->ncol > NVMTL_NCOL ? NVMTL_NCOL : p->ncol;
    for (uint32_t i = 0; i < n; i++) cf[i] = p->cfmts[i];
    return n;
}
static VkResult nvmtl_make_render_pass(const nvk_pipeline *p, uint32_t load, VkRenderPass *out)
{
    VkSampleCountFlagBits sc = (VkSampleCountFlagBits)(p->samples > 1 ? p->samples : 1);
    int ms = p->samples > 1, anyres = 0;
    uint32_t cf[NVMTL_NCOL], rmask, ncol = nvmtl_rt_shape(p, cf, &rmask);
    VkAttachmentDescription atts[2 * NVMTL_NCOL + 1]; uint32_t n = 0;
    VkAttachmentReference cref[NVMTL_NCOL], rref[NVMTL_NCOL], dref = { 0, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    for (uint32_t i = 0; i < ncol; i++) {
        cref[i] = rref[i] = (VkAttachmentReference){ VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
        if (!cf[i]) continue;
        int ld = (load & NVMTL_LOAD_COLOUR(i)) != 0;
        cref[i].attachment = n;
        atts[n++] = (VkAttachmentDescription){ 0, (VkFormat)cf[i], sc,
            ld ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            ld ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    }
    if (p->has_depth) {
        int dl = (load & NVMTL_LOAD_DEPTH) != 0, sl = p->has_stencil ? (load & NVMTL_LOAD_STENCIL) != 0 : dl;
        dref.attachment = n;
        atts[n++] = (VkAttachmentDescription){ 0, (VkFormat)(p->dfmt ? p->dfmt : VK_FORMAT_D32_SFLOAT), sc,
            dl ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            sl ? VK_ATTACHMENT_LOAD_OP_LOAD : p->has_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            (sl || p->has_stencil) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            (dl || sl) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }
    for (uint32_t i = 0; ms && i < ncol; i++) {
        if (!cf[i] || !(rmask & (1u << i))) continue;
        rref[i].attachment = n; anyres = 1;
        atts[n++] = (VkAttachmentDescription){ 0, (VkFormat)cf[i], VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    }
    // framebuffer fetch: the colour attachments are input attachments of the same subpass, in GENERAL layout (the feedback
    // loop Vulkan allows), and every pass declares them so every pipeline of the pass stays compatible
    VkAttachmentReference iref[NVMTL_NCOL];
    const int fetch = nvmtl_fbfetch_on() && ncol;
    for (uint32_t i = 0; fetch && i < ncol; i++) {
        if (cref[i].attachment != VK_ATTACHMENT_UNUSED) cref[i].layout = VK_IMAGE_LAYOUT_GENERAL;
        iref[i] = (VkAttachmentReference){ cref[i].attachment, VK_IMAGE_LAYOUT_GENERAL };
    }
    VkSubpassDescription sp = { 0, VK_PIPELINE_BIND_POINT_GRAPHICS, fetch ? ncol : 0, fetch ? iref : NULL, ncol, ncol ? cref : NULL, anyres ? rref : NULL,
        p->has_depth ? &dref : NULL, 0, NULL };
    const VkSubpassDependency deps[3] = {
        { VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
        { 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0 },
        { 0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT } };
    VkRenderPassCreateInfo rpci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO, NULL, 0, n, atts, 1, &sp, fetch ? 3u : 2u, deps };
    return pvkCreateRenderPass(g_dev, &rpci, NULL, out);
}
int nvmtl_vk_pipeline_create_blend(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const nvk_blend_state *blend, nvk_pipeline *out)
{ return nvmtl_vk_pipeline_create_vin(vs, vn, fs, fn, vkfmt, depthfmt, blend, NULL, out); }
int nvmtl_vk_pipeline_create_vin(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const nvk_blend_state *blend,
                                 const nvk_vertex_input *vin, nvk_pipeline *out)
{ return nvmtl_vk_pipeline_create_vin_ms(vs, vn, fs, fn, vkfmt, depthfmt, blend, vin, 1, out); }
#define NVMTL_TESS_NVB 8
#define NVMTL_TESS_PC_BYTES 96
_Static_assert(NVMTL_SAMPLE_POSITION_OFFSET >= NVMTL_TESS_PC_BYTES, "fragment and TESC push ranges must not overlap");

typedef struct { uint32_t *w; size_t n, cap; int bad; } nvtsb;
static void nvt_put(nvtsb *b, uint32_t v) {
    if (b->bad) return;
    if (b->n == b->cap) { size_t nc = b->cap ? b->cap * 2 : 256; uint32_t *nw = (uint32_t *)realloc(b->w, nc * 4);
        if (!nw) { b->bad = 1; return; } b->w = nw; b->cap = nc; }
    b->w[b->n++] = v;
}
static void nvt_ins(nvtsb *b, uint32_t op, const uint32_t *a, uint32_t na) { nvt_put(b, ((na + 1) << 16) | op); for (uint32_t i = 0; i < na; i++) nvt_put(b, a[i]); }
#define NVT(b, op, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; nvt_ins((b), (op), a_, (uint32_t)(sizeof a_ / sizeof a_[0])); } while (0)
static void nvt_str(nvtsb *b, uint32_t op, const uint32_t *pre, uint32_t npre, const char *s, const uint32_t *post, uint32_t npost) {
    size_t sl = strlen(s), sw = sl / 4 + 1;
    nvt_put(b, (uint32_t)((1 + npre + sw + npost) << 16) | op);
    for (uint32_t i = 0; i < npre; i++) nvt_put(b, pre[i]);
    for (size_t i = 0; i < sw; i++) { uint32_t v = 0; for (int k = 0; k < 4; k++) { size_t j = i * 4 + k; if (j < sl) v |= (uint32_t)(uint8_t)s[j] << (8 * k); } nvt_put(b, v); }
    for (uint32_t i = 0; i < npost; i++) nvt_put(b, post[i]);
}

typedef struct {
    nvtsb caps, ep, deco, glob, fn;
    uint32_t id, glsl, cur;
    uint32_t t_void, t_fn, t_bool, t_u64, t_vec[3][5];
    struct { uint32_t ty, lo, hi, id; } k[96]; int nk;
    struct { uint32_t sc, ty, id; } p[48]; int np;
    int err;
} nvtc;
static uint32_t nvt_id(nvtc *c) { return c->id++; }
static uint32_t nvt_ptr(nvtc *c, uint32_t sc, uint32_t ty) {
    for (int i = 0; i < c->np; i++) if (c->p[i].sc == sc && c->p[i].ty == ty) return c->p[i].id;
    if (c->np == 48) { c->err = 1; return 0; }
    uint32_t id = nvt_id(c); NVT(&c->glob, 32, id, sc, ty); c->p[c->np].sc = sc; c->p[c->np].ty = ty; c->p[c->np++].id = id; return id;
}
static uint32_t nvt_k2(nvtc *c, uint32_t ty, uint32_t lo, uint32_t hi, int wide) {
    for (int i = 0; i < c->nk; i++) if (c->k[i].ty == ty && c->k[i].lo == lo && c->k[i].hi == hi) return c->k[i].id;
    if (c->nk == 96) { c->err = 1; return 0; }
    uint32_t id = nvt_id(c);
    if (wide) NVT(&c->glob, 43, ty, id, lo, hi); else NVT(&c->glob, 43, ty, id, lo);
    c->k[c->nk].ty = ty; c->k[c->nk].lo = lo; c->k[c->nk].hi = hi; c->k[c->nk++].id = id; return id;
}
#define NVT_F 0
#define NVT_I 1
#define NVT_U 2
static uint32_t nvt_ku(nvtc *c, uint32_t v) { return nvt_k2(c, c->t_vec[NVT_U][1], v, 0, 0); }
static uint32_t nvt_ki(nvtc *c, int32_t v) { return nvt_k2(c, c->t_vec[NVT_I][1], (uint32_t)v, 0, 0); }
static uint32_t nvt_kf(nvtc *c, float f) { uint32_t v; memcpy(&v, &f, 4); return nvt_k2(c, c->t_vec[NVT_F][1], v, 0, 0); }
static uint32_t nvt_k64(nvtc *c, uint64_t v) { return nvt_k2(c, c->t_u64, (uint32_t)v, (uint32_t)(v >> 32), 1); }
static uint32_t nvt_op1(nvtc *c, uint32_t op, uint32_t ty, uint32_t a) { uint32_t r = nvt_id(c); NVT(&c->fn, op, ty, r, a); return r; }
static uint32_t nvt_op2(nvtc *c, uint32_t op, uint32_t ty, uint32_t a, uint32_t b) { uint32_t r = nvt_id(c); NVT(&c->fn, op, ty, r, a, b); return r; }
static uint32_t nvt_ext1(nvtc *c, uint32_t ty, uint32_t inst, uint32_t a) { uint32_t r = nvt_id(c); NVT(&c->fn, 12, ty, r, c->glsl, inst, a); return r; }
static uint32_t nvt_ext2(nvtc *c, uint32_t ty, uint32_t inst, uint32_t a, uint32_t b) { uint32_t r = nvt_id(c); NVT(&c->fn, 12, ty, r, c->glsl, inst, a, b); return r; }
static uint32_t nvt_sel(nvtc *c, uint32_t ty, uint32_t cond, uint32_t a, uint32_t b) { uint32_t r = nvt_id(c); NVT(&c->fn, 169, ty, r, cond, a, b); return r; }
static uint32_t nvt_ex(nvtc *c, uint32_t ty, uint32_t comp, uint32_t i) { uint32_t r = nvt_id(c); NVT(&c->fn, 81, ty, r, comp, i); return r; }
static uint32_t nvt_add64(nvtc *c, uint32_t a, uint32_t b) { return nvt_op2(c, 128, c->t_u64, a, b); }
static uint32_t nvt_widen(nvtc *c, uint32_t u32) { return nvt_op1(c, 113, c->t_u64, u32); }
static uint32_t nvt_load32(nvtc *c, uint32_t addr) {
    uint32_t pt = nvt_ptr(c, 5349, c->t_vec[NVT_U][1]), p = nvt_op1(c, 120, pt, addr), v = nvt_id(c);
    NVT(&c->fn, 61, c->t_vec[NVT_U][1], v, p, 2, 4);
    return v;
}
static void nvt_types(nvtc *c, int wide) {
    c->id = 1;
    c->t_void = nvt_id(c); NVT(&c->glob, 19, c->t_void);
    c->t_fn = nvt_id(c); NVT(&c->glob, 33, c->t_fn, c->t_void);
    c->t_bool = nvt_id(c); NVT(&c->glob, 20, c->t_bool);
    c->t_vec[NVT_F][1] = nvt_id(c); NVT(&c->glob, 22, c->t_vec[NVT_F][1], 32);
    c->t_vec[NVT_I][1] = nvt_id(c); NVT(&c->glob, 21, c->t_vec[NVT_I][1], 32, 1);
    c->t_vec[NVT_U][1] = nvt_id(c); NVT(&c->glob, 21, c->t_vec[NVT_U][1], 32, 0);
    if (wide) { c->t_u64 = nvt_id(c); NVT(&c->glob, 21, c->t_u64, 64, 0); }
    for (int k = 0; k < 3; k++) for (uint32_t n = 2; n <= 4; n++) { c->t_vec[k][n] = nvt_id(c); NVT(&c->glob, 23, c->t_vec[k][n], c->t_vec[k][1], n); }
}
static uint32_t *nvt_link(nvtc *c, size_t *outn) {
    nvtsb *s[5] = { &c->caps, &c->ep, &c->deco, &c->glob, &c->fn };
    size_t n = 5; for (int i = 0; i < 5; i++) { if (s[i]->bad) c->err = 1; n += s[i]->n; }
    uint32_t *w = c->err ? NULL : (uint32_t *)malloc(n * 4);
    if (w) {
        w[0] = 0x07230203; w[1] = 0x00010300; w[2] = 0; w[3] = c->id; w[4] = 0;
        size_t o = 5; for (int i = 0; i < 5; i++) { if (s[i]->n) memcpy(w + o, s[i]->w, s[i]->n * 4); o += s[i]->n; }
        *outn = n;
    }
    for (int i = 0; i < 5; i++) free(s[i]->w);
    return w;
}

typedef struct { uint32_t op, a, b; } nvt_def;
typedef struct { uint32_t var, loc, patch, kind, comps, arrlen; } nvt_in;
typedef struct { uint32_t model, domain, spacing_at, order_at, pid_deco_at, pid_var, pid_kind, maxloc; int has_loc; int nin; nvt_in in[40]; } nvt_tese;
static int nvt_scan_tese(const uint32_t *w, size_t n, nvt_tese *t) {
    memset(t, 0, sizeof *t); t->model = ~0u;
    if (n < 5 || w[0] != 0x07230203) return -1;
    uint32_t bound = w[3]; if (!bound || bound > (1u << 22)) return -1;
    nvt_def *d = (nvt_def *)calloc(bound, sizeof *d); uint32_t *loc = (uint32_t *)calloc(bound, 4), *fl = (uint32_t *)calloc(bound, 4);
    if (!d || !loc || !fl) { free(d); free(loc); free(fl); return -1; }
    int rc = 0;
    for (size_t i = 5; i < n; ) {
        uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > n) { rc = -1; break; }
        const uint32_t *o = w + i + 1;
        #define NVT_ID_OK(x) ((x) < bound)
        if (op == 15 && wc >= 3) t->model = o[0];
        else if (op == 16 && wc >= 3) {
            if (o[1] == 22 || o[1] == 24 || o[1] == 25) t->domain = o[1];
            else if (o[1] >= 1 && o[1] <= 3) t->spacing_at = (uint32_t)(i + 2);
            else if (o[1] == 4 || o[1] == 5) t->order_at = (uint32_t)(i + 2);
        } else if (op == 71 && wc >= 3 && NVT_ID_OK(o[0])) {
            if (o[1] == 30 && wc >= 4) loc[o[0]] = o[2] + 1;
            else if (o[1] == 15) fl[o[0]] |= 1;
            else if (o[1] == 11 && wc >= 4) { fl[o[0]] |= 2; if (o[2] == 7) { t->pid_deco_at = (uint32_t)i; t->pid_var = o[0]; } }
        } else if ((op == 21 || op == 22 || op == 23 || op == 28 || op == 32) && wc >= 3 && NVT_ID_OK(o[0])) {
            d[o[0]].op = op; d[o[0]].a = o[1]; d[o[0]].b = wc >= 4 ? o[2] : 0;
        } else if (op == 43 && wc >= 4 && NVT_ID_OK(o[1])) { d[o[1]].op = 43; d[o[1]].a = o[0]; d[o[1]].b = o[2]; }
        else if (op == 59 && wc >= 4 && NVT_ID_OK(o[1]) && o[2] == 1) { d[o[1]].op = 59; d[o[1]].a = o[0]; }
        i += wc;
    }
    for (uint32_t id = 0; rc == 0 && id < bound; id++) {
        if (d[id].op != 59 || !loc[id] || (fl[id] & 2)) continue;
        uint32_t L = loc[id] - 1; if (!t->has_loc || L > t->maxloc) t->maxloc = L; t->has_loc = 1;
        if (t->nin == 40) { rc = -2; break; }
        nvt_in *e = &t->in[t->nin++]; e->var = id; e->loc = L; e->patch = fl[id] & 1;
        uint32_t ty = d[id].a < bound && d[d[id].a].op == 32 ? d[d[id].a].b : 0;
        if (ty < bound && d[ty].op == 28) {
            uint32_t len = d[ty].b; e->arrlen = len < bound && d[len].op == 43 ? d[len].b : 0; ty = d[ty].a;
        }
        e->comps = 1;
        if (ty < bound && d[ty].op == 23) { e->comps = d[ty].b; ty = d[ty].a; }
        if (ty < bound && d[ty].op == 22 && d[ty].a == 32) e->kind = NVT_F;
        else if (ty < bound && d[ty].op == 21 && d[ty].a == 32) e->kind = d[ty].b ? NVT_I : NVT_U;
        else e->kind = 99;
    }
    if (rc == 0 && t->pid_var) {
        uint32_t pt = t->pid_var < bound ? d[t->pid_var].a : 0, ty = pt < bound && d[pt].op == 32 ? d[pt].b : 0;
        t->pid_kind = ty < bound && d[ty].op == 21 && d[ty].a == 32 ? (d[ty].b ? NVT_I : NVT_U) : 99;
    }
    #undef NVT_ID_OK
    free(d); free(loc); free(fl);
    return rc;
}
int nvmtl_spirv_tess_info(const void *spv, size_t bytes, uint32_t *patchType, uint32_t *cps) {
    nvt_tese t; if (!spv || nvt_scan_tese((const uint32_t *)spv, bytes / 4, &t) || t.model != 2) return 0;
    *patchType = t.domain == 22 ? 1 : t.domain == 24 ? 2 : 0; *cps = 0;
    for (int i = 0; i < t.nin; i++) if (!t.in[i].patch && t.in[i].arrlen) { *cps = t.in[i].arrlen; break; }
    return *patchType != 0;
}

static uint32_t *nvmtl_tess_patch_tese(const uint32_t *w, size_t n, uint32_t spacing, uint32_t order, size_t *outn, uint32_t *pidloc, const char **why) {
    nvt_tese t; *pidloc = ~0u;
    if (nvt_scan_tese(w, n, &t)) { *why = "the post-tessellation module does not parse"; return NULL; }
    if (t.model != 2) { *why = "the vertex function is not a post-tessellation (TessellationEvaluation) module"; return NULL; }
    if (!t.spacing_at || !t.order_at) { *why = "the post-tessellation module declares no spacing or vertex order to set"; return NULL; }
    uint32_t *o = (uint32_t *)malloc((n + 3) * 4); if (!o) { *why = "out of memory"; return NULL; }
    size_t at = t.pid_deco_at;
    if (!at) { memcpy(o, w, n * 4); *outn = n; }
    else {
        memcpy(o, w, (at + 4) * 4);
        *pidloc = t.has_loc ? t.maxloc + 1 : 0;
        o[at + 2] = 30; o[at + 3] = *pidloc;
        o[at + 4] = (3u << 16) | 71; o[at + 5] = t.pid_var; o[at + 6] = 15;
        memcpy(o + at + 7, w + at + 4, (n - at - 4) * 4); *outn = n + 3;
    }
    size_t sh = at && t.spacing_at > at ? 3 : 0, oh = at && t.order_at > at ? 3 : 0;
    o[t.spacing_at + sh] = spacing; o[t.order_at + oh] = order;
    return o;
}

typedef struct { uint8_t fmt, kind, bits, comps, bgra; } nvt_fmt;
static const nvt_fmt k_nvt_fmt[] = {
    { 1, 4, 8, 2, 0 }, { 2, 4, 8, 3, 0 }, { 3, 4, 8, 4, 0 }, { 4, 5, 8, 2, 0 }, { 5, 5, 8, 3, 0 }, { 6, 5, 8, 4, 0 },
    { 7, 2, 8, 2, 0 }, { 8, 2, 8, 3, 0 }, { 9, 2, 8, 4, 0 }, { 10, 3, 8, 2, 0 }, { 11, 3, 8, 3, 0 }, { 12, 3, 8, 4, 0 },
    { 13, 4, 16, 2, 0 }, { 14, 4, 16, 3, 0 }, { 15, 4, 16, 4, 0 }, { 16, 5, 16, 2, 0 }, { 17, 5, 16, 3, 0 }, { 18, 5, 16, 4, 0 },
    { 19, 2, 16, 2, 0 }, { 20, 2, 16, 3, 0 }, { 21, 2, 16, 4, 0 }, { 22, 3, 16, 2, 0 }, { 23, 3, 16, 3, 0 }, { 24, 3, 16, 4, 0 },
    { 25, 1, 16, 2, 0 }, { 26, 1, 16, 3, 0 }, { 27, 1, 16, 4, 0 }, { 28, 0, 32, 1, 0 }, { 29, 0, 32, 2, 0 }, { 30, 0, 32, 3, 0 },
    { 31, 0, 32, 4, 0 }, { 32, 5, 32, 1, 0 }, { 33, 5, 32, 2, 0 }, { 34, 5, 32, 3, 0 }, { 35, 5, 32, 4, 0 }, { 36, 4, 32, 1, 0 },
    { 37, 4, 32, 2, 0 }, { 38, 4, 32, 3, 0 }, { 39, 4, 32, 4, 0 }, { 42, 2, 8, 4, 1 }, { 45, 4, 8, 1, 0 }, { 46, 5, 8, 1, 0 },
    { 47, 2, 8, 1, 0 }, { 48, 3, 8, 1, 0 }, { 49, 4, 16, 1, 0 }, { 50, 5, 16, 1, 0 }, { 51, 2, 16, 1, 0 }, { 52, 3, 16, 1, 0 },
    { 53, 1, 16, 1, 0 },
};
static const nvt_fmt *nvt_fmt_of(uint32_t f) { for (size_t i = 0; i < sizeof k_nvt_fmt / sizeof k_nvt_fmt[0]; i++) if (k_nvt_fmt[i].fmt == f) return &k_nvt_fmt[i]; return NULL; }
static int nvt_fmt_class(const nvt_fmt *f) { return f->kind <= 3 ? NVT_F : f->kind == 4 ? NVT_U : NVT_I; }

static uint32_t nvt_pull(nvtc *c, const nvt_fmt *f, uint32_t addr, uint32_t kind, uint32_t m) {
    uint32_t wd[4] = { 0 }, comp[4] = { 0 }, nw = (f->comps * f->bits / 8 + 3) / 4, F = c->t_vec[NVT_F][1], U = c->t_vec[NVT_U][1], I = c->t_vec[NVT_I][1];
    for (uint32_t i = 0; i < nw; i++) wd[i] = nvt_load32(c, i ? nvt_add64(c, addr, nvt_k64(c, 4 * i)) : addr);
    for (uint32_t k = 0; k < f->comps; k++) {
        uint32_t word = wd[k * f->bits / 32], off = (k * f->bits) % 32;
        float scale = f->kind == 2 ? (float)((1u << f->bits) - 1) : f->kind == 3 ? (float)((1u << (f->bits - 1)) - 1) : 1.0f;
        switch (f->kind) {
        case 0: comp[k] = nvt_op1(c, 124, F, word); break;
        case 1: comp[k] = nvt_ex(c, F, nvt_ext1(c, c->t_vec[NVT_F][2], 62, word), off / 16); break;
        case 2: comp[k] = nvt_op2(c, 133, F, nvt_op1(c, 112, F, f->bits == 32 ? word :
                    ({ uint32_t r = nvt_id(c); NVT(&c->fn, 203, U, r, word, nvt_ku(c, off), nvt_ku(c, f->bits)); r; })), nvt_kf(c, 1.0f / scale)); break;
        case 3: { uint32_t s = nvt_id(c); NVT(&c->fn, 202, I, s, nvt_op1(c, 124, I, word), nvt_ku(c, off), nvt_ku(c, f->bits));
                  comp[k] = nvt_ext2(c, F, 40, nvt_op2(c, 133, F, nvt_op1(c, 111, F, s), nvt_kf(c, 1.0f / scale)), nvt_kf(c, -1.0f)); break; }
        case 4: if (f->bits == 32) comp[k] = word; else { comp[k] = nvt_id(c); NVT(&c->fn, 203, U, comp[k], word, nvt_ku(c, off), nvt_ku(c, f->bits)); } break;
        default: { uint32_t iw = nvt_op1(c, 124, I, word); if (f->bits == 32) comp[k] = iw;
                   else { comp[k] = nvt_id(c); NVT(&c->fn, 202, I, comp[k], iw, nvt_ku(c, off), nvt_ku(c, f->bits)); } break; }
        }
    }
    if (f->bgra) { uint32_t s = comp[0]; comp[0] = comp[2]; comp[2] = s; }
    uint32_t have = (uint32_t)nvt_fmt_class(f), v[4];
    for (uint32_t j = 0; j < m; j++) {
        if (j < f->comps) v[j] = have == kind ? comp[j] : nvt_op1(c, 124, c->t_vec[kind][1], comp[j]);
        else v[j] = kind == NVT_F ? nvt_kf(c, j == 3 ? 1.0f : 0.0f) : kind == NVT_I ? nvt_ki(c, j == 3) : nvt_ku(c, j == 3);
    }
    if (m == 1) return v[0];
    uint32_t r = nvt_id(c);
    if (m == 2) NVT(&c->fn, 80, c->t_vec[kind][2], r, v[0], v[1]);
    else if (m == 3) NVT(&c->fn, 80, c->t_vec[kind][3], r, v[0], v[1], v[2]);
    else NVT(&c->fn, 80, c->t_vec[kind][4], r, v[0], v[1], v[2], v[3]);
    return r;
}
static uint32_t nvt_load_if(nvtc *c, uint32_t cond, uint32_t addr, uint32_t other) {
    uint32_t lt = nvt_id(c), lm = nvt_id(c), from = c->cur;
    NVT(&c->fn, 247, lm, 0); NVT(&c->fn, 250, cond, lt, lm);
    NVT(&c->fn, 248, lt); c->cur = lt; uint32_t v = nvt_load32(c, addr); NVT(&c->fn, 249, lm);
    NVT(&c->fn, 248, lm); uint32_t r = nvt_id(c); NVT(&c->fn, 245, c->t_vec[NVT_U][1], r, v, lt, other, from); c->cur = lm;
    return r;
}

#ifndef NVMTL_TESS_DESC_DEFINED
#define NVMTL_TESS_DESC_DEFINED
typedef struct { uint32_t patch_type, n, partition, winding, stepfn, scale_en, cpidx; float maxf; int32_t inst_loc; } nvk_tess_desc;
#endif

static uint32_t *nvmtl_tess_make_vs(size_t *n) {
    nvtc c; memset(&c, 0, sizeof c); nvt_types(&c, 0);
    NVT(&c.caps, 17, 1); NVT(&c.caps, 14, 0, 1);
    uint32_t I = c.t_vec[NVT_I][1], U2 = c.t_vec[NVT_U][2];
    uint32_t pin = nvt_ptr(&c, 1, I), pout = nvt_ptr(&c, 3, U2);
    uint32_t vi = nvt_id(&c), ii = nvt_id(&c), out = nvt_id(&c), main_ = nvt_id(&c);
    NVT(&c.glob, 59, pin, vi, 1); NVT(&c.glob, 59, pin, ii, 1); NVT(&c.glob, 59, pout, out, 3);
    NVT(&c.deco, 71, vi, 11, 42); NVT(&c.deco, 71, ii, 11, 43); NVT(&c.deco, 71, out, 30, 0);
    { const uint32_t pre[2] = { 0, main_ }, post[3] = { vi, ii, out }; nvt_str(&c.ep, 15, pre, 2, "main", post, 3); }
    NVT(&c.fn, 54, c.t_void, main_, 0, c.t_fn); NVT(&c.fn, 248, nvt_id(&c));
    uint32_t a = nvt_id(&c), b = nvt_id(&c); NVT(&c.fn, 61, I, a, vi); NVT(&c.fn, 61, I, b, ii);
    uint32_t v = nvt_id(&c); NVT(&c.fn, 80, U2, v, nvt_op1(&c, 124, c.t_vec[NVT_U][1], a), nvt_op1(&c, 124, c.t_vec[NVT_U][1], b));
    NVT(&c.fn, 62, out, v); NVT(&c.fn, 253); NVT(&c.fn, 56);
    return nvt_link(&c, n);
}

static uint32_t *nvmtl_tess_make_tcs(const nvt_tese *t, const nvk_tess_desc *td, const nvk_vertex_input *vin, uint32_t pidloc,
                                     uint32_t *vbmap, uint32_t *nvb, size_t *n, char *why, size_t wl) {
    nvtc c; memset(&c, 0, sizeof c); nvt_types(&c, 1);
    uint32_t F = c.t_vec[NVT_F][1], U = c.t_vec[NVT_U][1], I = c.t_vec[NVT_I][1], U64 = c.t_u64, quad = td->patch_type == 2;
    *nvb = 0;
    struct { const nvt_in *in; const nvt_fmt *f; uint32_t off, slot, stride, step; int sys; } pl[40]; int npl = 0;
    int inst_found = 0;
    for (int i = 0; i < t->nin; i++) {
        const nvt_in *e = &t->in[i];
        if (e->loc == pidloc && e->patch) continue;
        if (e->kind == 99) { snprintf(why, wl, "TESE input Location %u is not a 32-bit scalar/vector", e->loc); goto refuse; }
        if (!e->patch && e->arrlen != td->n) { snprintf(why, wl, "TESE control-point input Location %u has %u elements, the patch has %u", e->loc, e->arrlen, td->n); goto refuse; }
        int a = -1; for (uint32_t k = 0; vin && k < vin->nattr; k++) if (vin->attr[k].location == e->loc) a = (int)k;
        int is_inst = e->patch && ((td->inst_loc >= 0 && (uint32_t)td->inst_loc == e->loc) ||
                                   (td->inst_loc < 0 && a < 0 && !inst_found && e->comps == 1 && e->kind != NVT_F));
        if (is_inst) { if (e->comps != 1 || e->kind == NVT_F) { snprintf(why, wl, "instance_id Location %u is not an integer scalar", e->loc); goto refuse; }
            pl[npl].in = e; pl[npl++].sys = 1; inst_found = 1; continue; }
        if (a < 0) { snprintf(why, wl, "TESE reads Location %u and the vertex descriptor has no attribute %u", e->loc, e->loc); goto refuse; }
        const nvt_fmt *f = nvt_fmt_of(vin->attr[a].mtlfmt);
        if (!f) { snprintf(why, wl, "attribute %u: MTLVertexFormat %u is not carried for tessellation", e->loc, vin->attr[a].mtlfmt); goto refuse; }
        if ((nvt_fmt_class(f) == NVT_F) != (e->kind == NVT_F)) { snprintf(why, wl, "attribute %u: format %u and the shader's type disagree on float vs integer", e->loc, vin->attr[a].mtlfmt); goto refuse; }
        uint32_t b = vin->attr[a].buffer, step = vin->layout[b].step, rate = vin->layout[b].rate;
        if (step == 1) { snprintf(why, wl, "attribute %u: buffer %u steps PER VERTEX, which a post-tessellation function cannot fetch", e->loc, b); goto refuse; }
        if (step == 2 && rate != 1) { snprintf(why, wl, "attribute %u: per-instance step rate %u is not carried for tessellation", e->loc, rate); goto refuse; }
        if (step && vin->layout[b].stride == NVMTL_STRIDE_DYNAMIC) { snprintf(why, wl, "attribute %u: buffer %u has a dynamic stride "
            "(MTLBufferLayoutStrideDynamic), which the tessellation pull does not carry", e->loc, b); goto refuse; }
        if (vin->attr[a].offset & 3 || (step && vin->layout[b].stride & 3)) { snprintf(why, wl, "attribute %u: offset/stride not 4-byte aligned", e->loc); goto refuse; }
        uint32_t s = 0; while (s < *nvb && vbmap[s] != b) s++;
        if (s == *nvb) { if (*nvb == NVMTL_TESS_NVB) { snprintf(why, wl, "more than %d vertex buffers feed one tessellation pipeline", NVMTL_TESS_NVB); goto refuse; } vbmap[(*nvb)++] = b; }
        pl[npl].in = e; pl[npl].f = f; pl[npl].off = vin->attr[a].offset; pl[npl].slot = s; pl[npl].stride = step ? vin->layout[b].stride : 0;
        pl[npl].step = step; pl[npl++].sys = 0;
    }

    NVT(&c.caps, 17, 1); NVT(&c.caps, 17, 3); NVT(&c.caps, 17, 11); NVT(&c.caps, 17, 5347);
    { const uint32_t none[1] = { 0 }; nvt_str(&c.caps, 10, none, 0, "SPV_KHR_physical_storage_buffer", none, 0);
      c.glsl = nvt_id(&c); const uint32_t pre[1] = { c.glsl }; nvt_str(&c.caps, 11, pre, 1, "GLSL.std.450", none, 0); }
    NVT(&c.caps, 14, 5348, 1);
    uint32_t arr8 = nvt_id(&c); NVT(&c.glob, 28, arr8, U64, nvt_ku(&c, NVMTL_TESS_NVB)); NVT(&c.deco, 71, arr8, 6, 8);
    uint32_t pcs = nvt_id(&c); NVT(&c.glob, 30, pcs, U64, U64, U64, U, F, arr8);
    NVT(&c.deco, 71, pcs, 2);
    { const uint32_t offs[6] = { 0, 8, 16, 24, 28, 32 }; for (uint32_t m = 0; m < 6; m++) NVT(&c.deco, 72, pcs, m, 35, offs[m]); }
    uint32_t pc = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 9, pcs), pc, 9);
    uint32_t inids_t = nvt_id(&c); NVT(&c.glob, 28, inids_t, c.t_vec[NVT_U][2], nvt_ku(&c, 32));
    uint32_t inids = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 1, inids_t), inids, 1); NVT(&c.deco, 71, inids, 30, 0);
    uint32_t invid = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 1, I), invid, 1); NVT(&c.deco, 71, invid, 11, 8);
    uint32_t f4 = nvt_id(&c); NVT(&c.glob, 28, f4, F, nvt_ku(&c, 4)); uint32_t f2 = nvt_id(&c); NVT(&c.glob, 28, f2, F, nvt_ku(&c, 2));
    uint32_t outer = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 3, f4), outer, 3); NVT(&c.deco, 71, outer, 11, 11); NVT(&c.deco, 71, outer, 15);
    uint32_t inner = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 3, f2), inner, 3); NVT(&c.deco, 71, inner, 11, 12); NVT(&c.deco, 71, inner, 15);
    uint32_t iface[48]; uint32_t nif = 0; iface[nif++] = inids; iface[nif++] = invid; iface[nif++] = outer; iface[nif++] = inner;
    uint32_t outv[40], pidout = 0;
    for (int i = 0; i < npl; i++) {
        const nvt_in *e = pl[i].in; uint32_t ty = c.t_vec[e->kind][e->comps];
        if (!e->patch) { uint32_t at = nvt_id(&c); NVT(&c.glob, 28, at, ty, nvt_ku(&c, td->n)); ty = at; }
        outv[i] = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 3, ty), outv[i], 3); NVT(&c.deco, 71, outv[i], 30, e->loc);
        if (e->patch) NVT(&c.deco, 71, outv[i], 15);
        iface[nif++] = outv[i];
    }
    if (pidloc != ~0u && t->pid_kind == 99) { snprintf(why, wl, "the post-tessellation patch_id is not a 32-bit integer"); goto refuse; }
    if (pidloc != ~0u) { pidout = nvt_id(&c); NVT(&c.glob, 59, nvt_ptr(&c, 3, c.t_vec[t->pid_kind][1]), pidout, 3); NVT(&c.deco, 71, pidout, 30, pidloc); NVT(&c.deco, 71, pidout, 15); iface[nif++] = pidout; }
    uint32_t main_ = nvt_id(&c);
    { const uint32_t pre[2] = { 1, main_ }; nvt_str(&c.ep, 15, pre, 2, "main", iface, nif); }
    NVT(&c.ep, 16, main_, 26, td->n);

    NVT(&c.fn, 54, c.t_void, main_, 0, c.t_fn); c.cur = nvt_id(&c); NVT(&c.fn, 248, c.cur);
    uint32_t pu64 = nvt_ptr(&c, 9, U64), pu32 = nvt_ptr(&c, 9, U), pf32 = nvt_ptr(&c, 9, F);
    #define NVT_PC(ptrty, ty, ...) ({ uint32_t p_ = nvt_id(&c), v_ = nvt_id(&c); NVT(&c.fn, 65, ptrty, p_, pc, __VA_ARGS__); NVT(&c.fn, 61, ty, v_, p_); v_; })
    uint32_t fac = NVT_PC(pu64, U64, nvt_ku(&c, 0)), cpx = NVT_PC(pu64, U64, nvt_ku(&c, 1)), pix = NVT_PC(pu64, U64, nvt_ku(&c, 2));
    uint32_t istr = NVT_PC(pu32, U, nvt_ku(&c, 3)), scl = NVT_PC(pf32, F, nvt_ku(&c, 4));
    uint32_t ids = ({ uint32_t p_ = nvt_id(&c), v_ = nvt_id(&c); NVT(&c.fn, 65, nvt_ptr(&c, 1, c.t_vec[NVT_U][2]), p_, inids, nvt_ku(&c, 0)); NVT(&c.fn, 61, c.t_vec[NVT_U][2], v_, p_); v_; });
    uint32_t pos = nvt_ex(&c, U, ids, 0), iid = nvt_ex(&c, U, ids, 1);
    uint32_t inv = ({ uint32_t v_ = nvt_id(&c); NVT(&c.fn, 61, I, v_, invid); nvt_op1(&c, 124, U, v_); });
    uint32_t z64 = nvt_k64(&c, 0);
    uint32_t pid = nvt_load_if(&c, nvt_op2(&c, 171, c.t_bool, pix, z64),
                               nvt_add64(&c, pix, nvt_op2(&c, 132, U64, nvt_widen(&c, pos), nvt_k64(&c, 4))), pos);
    uint32_t cpv = nvt_op2(&c, 128, U, nvt_op2(&c, 132, U, pid, nvt_ku(&c, td->n)), inv);
    if (td->cpidx) {
        uint32_t w4 = td->cpidx == 1 ? 2 : 4, byte = nvt_op2(&c, 132, U64, nvt_widen(&c, cpv), nvt_k64(&c, w4));
        uint32_t ea = nvt_add64(&c, cpx, byte), wa = nvt_op2(&c, 199, U64, ea, nvt_k64(&c, ~3ull));
        uint32_t word = nvt_load_if(&c, nvt_op2(&c, 171, c.t_bool, cpx, z64), wa, cpv);
        if (td->cpidx == 1) {
            uint32_t sh = nvt_op2(&c, 132, U, nvt_op2(&c, 199, U, nvt_op1(&c, 113, U, ea), nvt_ku(&c, 2)), nvt_ku(&c, 8));
            uint32_t hv = nvt_id(&c); NVT(&c.fn, 203, U, hv, word, sh, nvt_ku(&c, 16));
            cpv = nvt_sel(&c, U, nvt_op2(&c, 171, c.t_bool, cpx, z64), hv, cpv);
        } else cpv = word;
    }
    for (int i = 0; i < npl; i++) {
        const nvt_in *e = pl[i].in; uint32_t ty = c.t_vec[e->kind][e->comps], val;
        if (pl[i].sys) val = e->kind == NVT_U ? iid : nvt_op1(&c, 124, I, iid);
        else {
            uint32_t idx = pl[i].step == 4 ? cpv : pl[i].step == 3 ? pid : pl[i].step == 2 ? iid : nvt_ku(&c, 0);
            uint32_t vb = NVT_PC(pu64, U64, nvt_ku(&c, 5), nvt_ku(&c, pl[i].slot));
            uint32_t addr = nvt_add64(&c, nvt_add64(&c, vb, nvt_op2(&c, 132, U64, nvt_widen(&c, idx), nvt_k64(&c, pl[i].stride))), nvt_k64(&c, pl[i].off));
            val = nvt_pull(&c, pl[i].f, addr, e->kind, e->comps);
        }
        if (e->patch) NVT(&c.fn, 62, outv[i], val);
        else { uint32_t p_ = nvt_id(&c); NVT(&c.fn, 65, nvt_ptr(&c, 3, ty), p_, outv[i], inv); NVT(&c.fn, 62, p_, val); }
    }
    if (pidout) NVT(&c.fn, 62, pidout, t->pid_kind == NVT_U ? pid : nvt_op1(&c, 124, I, pid));
    uint32_t fsz = quad ? 12 : 8, fa = fac;
    if (td->stepfn & 2) fa = nvt_add64(&c, fa, nvt_op2(&c, 132, U64, nvt_widen(&c, iid), nvt_widen(&c, istr)));
    if (td->stepfn & 1) fa = nvt_add64(&c, fa, nvt_op2(&c, 132, U64, nvt_widen(&c, pos), nvt_k64(&c, fsz)));
    uint32_t h[3], e[6], ne = quad ? 4 : 3, ni = quad ? 2 : 1;
    for (uint32_t k = 0; k < (quad ? 3u : 2u); k++) h[k] = nvt_ext1(&c, c.t_vec[NVT_F][2], 62, nvt_load32(&c, k ? nvt_add64(&c, fa, nvt_k64(&c, 4 * k)) : fa));
    for (uint32_t k = 0; k < ne + ni; k++) e[k] = nvt_ex(&c, F, h[k / 2], k % 2);
    uint32_t ok = 0;
    for (uint32_t k = 0; k < ne; k++) { uint32_t g = nvt_op2(&c, 186, c.t_bool, e[k], nvt_kf(&c, 0.0f)); ok = k ? nvt_op2(&c, 167, c.t_bool, ok, g) : g; }
    uint32_t kmax = nvt_kf(&c, td->maxf), z = nvt_kf(&c, 0.0f);
    for (uint32_t k = 0; k < ne + ni; k++) {
        uint32_t x = e[k];
        if (td->scale_en) x = nvt_op2(&c, 133, F, x, scl);
        x = nvt_sel(&c, F, nvt_op2(&c, 186, c.t_bool, x, kmax), kmax, x);
        if (td->partition == 0) x = nvt_ext1(&c, F, 29, nvt_ext1(&c, F, 9, nvt_ext1(&c, F, 30, x)));
        if (k < ne) x = nvt_sel(&c, F, ok, x, z);
        uint32_t p_ = nvt_id(&c), dst = k < ne ? outer : inner, di = k < ne ? k : k - ne;
        NVT(&c.fn, 65, nvt_ptr(&c, 3, F), p_, dst, nvt_ki(&c, (int32_t)di)); NVT(&c.fn, 62, p_, x);
    }
    for (uint32_t k = ne; k < 4; k++) { uint32_t p_ = nvt_id(&c); NVT(&c.fn, 65, nvt_ptr(&c, 3, F), p_, outer, nvt_ki(&c, (int32_t)k)); NVT(&c.fn, 62, p_, z); }
    if (!quad) { uint32_t p_ = nvt_id(&c); NVT(&c.fn, 65, nvt_ptr(&c, 3, F), p_, inner, nvt_ki(&c, 1)); NVT(&c.fn, 62, p_, z); }
    NVT(&c.fn, 253); NVT(&c.fn, 56);
    #undef NVT_PC
    uint32_t *out = nvt_link(&c, n);
    if (!out) snprintf(why, wl, "out of memory building the tessellation control stage");
    return out;
refuse:
    free(c.caps.w); free(c.ep.w); free(c.deco.w); free(c.glob.w); free(c.fn.w);
    return NULL;
}
_Static_assert(sizeof(((nvk_pipeline *)0)->tess_vb) / sizeof(uint32_t) == NVMTL_TESS_NVB, "nvk_pipeline.tess_vb holds NVMTL_TESS_NVB slots");
_Static_assert(NVMTL_LOAD_ALL == (1u << (NVMTL_NCOL + 2)) - 1u, "NVMTL_LOAD_ALL must be every load bit: NVMTL_NCOL colour + depth + stencil");
_Static_assert(NVMTL_RPX_MAX == (1u << (NVMTL_NCOL + 2)) - 4u, "NVMTL_RPX_MAX must be every nonzero load key minus the 3 that rpv[] holds");

static void nvmtl_failed_pipeline(nvk_pipeline *pipeline)
{
    if (pipeline->pipe) pvkDestroyPipeline(g_dev, (VkPipeline)pipeline->pipe, NULL);
    if (pipeline->mod) pvkDestroyShaderModule(g_dev, (VkShaderModule)pipeline->mod, NULL);
    if (pipeline->layout) pvkDestroyPipelineLayout(g_dev, (VkPipelineLayout)pipeline->layout, NULL);
    if (pipeline->rp) pvkDestroyRenderPass(g_dev, (VkRenderPass)pipeline->rp, NULL);
    free(pipeline->ag_src);
    memset(pipeline, 0, sizeof *pipeline);
}
_Static_assert(sizeof(VkRenderPass) == sizeof(void *) && _Alignof(VkRenderPass) == _Alignof(void *), "VkRenderPass bridge ABI");
#define NVMTL_PIPELINE_STAGE(call, what) do { VkResult stage_result = (call); if (stage_result != VK_SUCCESS) { nvlog("vk: %s -> %d", what, stage_result); goto pipeline_failed; } } while (0)
static int nvmtl_vk_pipeline_create_impl(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                         const void *vin_, uint32_t samples, const nvk_tess_desc *td, const nvk_rt *rt, void *out_);
int nvmtl_vk_pipeline_create_vin_ms(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                    const void *vin_, uint32_t samples, void *out_)
{ return nvmtl_vk_pipeline_create_impl(vs, vn, fs, fn, vkfmt, depthfmt, blend_, vin_, samples, NULL, NULL, out_); }
int nvmtl_vk_pipeline_create_tess(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend,
                                  const void *vin, uint32_t samples, const nvk_tess_desc *td, void *out)
{
    if (!td) { nvlog("tess pipeline REFUSED: no tessellation description"); return -1; }
    return nvmtl_vk_pipeline_create_impl(vs, vn, fs, fn, vkfmt, depthfmt, blend, vin, samples, td, NULL, out);
}
int nvmtl_vk_pipeline_create_rt(const void *vs, size_t vn, const void *fs, size_t fn, const nvk_rt *rt, const void *vin, uint32_t samples,
                                const nvk_tess_desc *td, void *out)
{
    if (!rt || rt->ncol > NVMTL_NCOL) { nvlog("pipeline REFUSED: %u colour attachments (this driver carries %u)", rt ? rt->ncol : 0u, NVMTL_NCOL); return -1; }
    if (nvmtl_vk_init()) return -1;
    uint32_t f0 = 0; while (f0 < rt->ncol && !rt->cfmt[f0]) f0++;
    for (uint32_t i = f0 + 1; !g_indep && i < rt->ncol; i++)
        if (rt->cfmt[i] && memcmp(&rt->blend[i], &rt->blend[f0], sizeof rt->blend[i])) {
            nvlog("pipeline REFUSED: colour attachments %u and %u blend differently and this device has no independentBlend", f0, i); return -1; }
    return nvmtl_vk_pipeline_create_impl(vs, vn, fs, fn, rt->ncol ? rt->cfmt[0] : 0u, rt->dfmt, NULL, vin, samples, td, rt, out);
}
static int nvmtl_vk_pipeline_create_impl_lifetime_body(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                         const void *vin_, uint32_t samples, const nvk_tess_desc *td, const nvk_rt *rt, void *out_);
static int nvmtl_vk_pipeline_create_impl(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                         const void *vin_, uint32_t samples, const nvk_tess_desc *td, const nvk_rt *rt, void *out_)
{
    nvk_pipeline pending = {0};
    int result = nvmtl_vk_pipeline_create_impl_lifetime_body(vs, vn, fs, fn, vkfmt, depthfmt, blend_, vin_, samples, td, rt, &pending);
    if (!result && (pending.ubo_mask[0] | pending.ubo_mask[1])) {
        /* glub1: keep what the twin is made from; without it the pass cannot be undone per draw, so it is not kept */
        nvmtl_ubo_src *s = td ? NULL : calloc(1, sizeof *s);
        if (s) { s->vs = nvmtl_ubo_mod_get(vs, vn); s->fs = nvmtl_ubo_mod_get(fs, fn); }
        if (s && s->vs && s->fs) {
            s->vn = vn; s->fn = fn; s->vkfmt = vkfmt; s->depthfmt = depthfmt; s->samples = samples;
            if ((s->has_blend = blend_ != NULL)) s->blend = *(const nvk_blend_state *)blend_;
            if ((s->has_vin = vin_ != NULL)) s->vin = *(const nvk_vertex_input *)vin_;
            if ((s->has_rt = rt != NULL)) s->rt = *rt;
            pending.ubo_src = s;
        } else {
            nvmtl_ubo_src_free(s); nvmtl_failed_pipeline(&pending);
            nvlog("gl-ubo: no memory to keep a pipeline's source - built without the pass");
            t_gl_ubo_off = 1;
            result = nvmtl_vk_pipeline_create_impl_lifetime_body(vs, vn, fs, fn, vkfmt, depthfmt, blend_, vin_, samples, td, rt, &pending);
            t_gl_ubo_off = 0;
        }
    }
    if (result) { nvmtl_failed_pipeline(&pending); memset(out_, 0, sizeof pending); }
    else *(nvk_pipeline *)out_ = pending;
    return result;
}
static int nvmtl_vk_pipeline_create_impl_lifetime_body(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend_,
                                         const void *vin_, uint32_t samples, const nvk_tess_desc *td, const nvk_rt *rt, void *out_)
{
    if (nvmtl_heap_binding_refused(vs, vn, "vertex") || nvmtl_heap_binding_refused(fs, fn, "fragment")) return -1;
    const nvk_blend_state *blend = (const nvk_blend_state *)blend_; const nvk_vertex_input *vin = (const nvk_vertex_input *)vin_; nvk_pipeline *out = (nvk_pipeline *)out_;
    nvk_rt lrt;
    if (!rt) {
        memset(&lrt, 0, sizeof lrt); lrt.ncol = vkfmt ? 1u : 0u; lrt.cfmt[0] = vkfmt; lrt.dfmt = depthfmt;
        lrt.blend[0] = blend ? *blend : (nvk_blend_state){ VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                                           VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0xf };
        rt = &lrt;
    }
    if (samples != 1 && samples != 2 && samples != 4 && samples != 8) { nvlog("pipeline: rasterSampleCount %u is not carried (1, 2, 4, 8)", samples); return -1; }
    NSData *vdata = nvmtl_address_shader(vs, vn), *fdata = nvmtl_address_shader(fs, fn);
    if (!vdata || !fdata) return -1;
    vs = vdata.bytes; vn = vdata.length; fs = fdata.bytes; fn = fdata.length;
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out);
    if (!td && nvmtl_pc_table_on()) {
        vdata = nvmtl_pc_table_shader(vdata, 0, &out->pc_table[0]);
        fdata = nvmtl_pc_table_shader(fdata, 4, &out->pc_table[1]);
        if (!t_gl_ubo_off && nvmtl_gl_ubo_on()) {   /* glub1: after table-pc, on its output (nvmtl_gl_ubo.h) */
            vdata = nvmtl_gl_ubo_shader(vdata, 0, &out->pc_table[0], &out->ubo_mask[0], out->ubo_ext[0]);
            fdata = nvmtl_gl_ubo_shader(fdata, 4, &out->pc_table[1], &out->ubo_mask[1], out->ubo_ext[1]);
        }
        vs = vdata.bytes; vn = vdata.length; fs = fdata.bytes; fn = fdata.length;
    }
    NSData *tvsd = nil, *ttcd = nil;
    if (td) {
        if (!g_tess) { nvlog("tess pipeline REFUSED: this device reports no tessellationShader"); return -1; }
        nvt_tese t0;
        if (nvt_scan_tese((const uint32_t *)vs, vn / 4, &t0) || t0.model != 2) { nvlog("tess pipeline REFUSED: the vertex module is not a TessellationEvaluation module after address lowering"); return -1; }
        uint32_t spacing = td->partition == 2 ? 3 : td->partition == 3 ? 2 : 1;
        const char *flip = getenv("NVMTL_TESS_ORDER_FLIP"); int fl = flip && *flip == '1';
        uint32_t order = ((td->winding == 0) != fl) ? 5 : 4;
        size_t ten = 0, tcn = 0, gvn = 0; uint32_t pidloc = ~0u, nvb = 0; const char *why = NULL; char why2[256] = "";
        uint32_t *te = nvmtl_tess_patch_tese((const uint32_t *)vs, vn / 4, spacing, order, &ten, &pidloc, &why);
        if (!te) { nvlog("tess pipeline REFUSED: %s", why ? why : "?"); return -1; }
        uint32_t *tc = nvmtl_tess_make_tcs(&t0, td, vin, pidloc, out->tess_vb, &nvb, &tcn, why2, sizeof why2);
        uint32_t *gv = tc ? nvmtl_tess_make_vs(&gvn) : NULL;
        if (!tc || !gv) { nvlog("tess pipeline REFUSED: %s", tc ? "out of memory building the tessellation vertex stage" : why2); free(te); free(tc); return -1; }
        vdata = [NSData dataWithBytesNoCopy:te length:ten * 4 freeWhenDone:YES];
        ttcd = [NSData dataWithBytesNoCopy:tc length:tcn * 4 freeWhenDone:YES];
        tvsd = [NSData dataWithBytesNoCopy:gv length:gvn * 4 freeWhenDone:YES];
        vs = vdata.bytes; vn = vdata.length;
        out->tess = 1; out->tess_n = td->n; out->tess_cpidx = td->cpidx; out->tess_nvb = nvb;
        nvlog("tess pipeline: %s patch, %u control points, partition %u, winding %u%s, factor step %u, scale %u, cp index %u, max %.0f, "
              "%u vertex buffer(s) pulled, patch_id at Location %d, instance_id Location %d",
              td->patch_type == 2 ? "quad" : "triangle", td->n, td->partition, td->winding, fl ? " (vertex order STRAIGHT by NVMTL_TESS_ORDER_FLIP - the negative control)" : "",
              td->stepfn, td->scale_en, td->cpidx, (double)td->maxf, nvb, (int)pidloc, (int)td->inst_loc);
    }
    VkVertexInputBindingDescription vib[NVMTL_NVIN]; VkVertexInputAttributeDescription via[NVMTL_NVIN]; uint32_t nvib = 0, nvia = 0;
    VkVertexInputBindingDivisorDescriptionKHR vdiv[NVMTL_NVIN]; uint32_t ndiv = 0;
    out->vin_dyn = 0;
    if (!td && nvmtl_vertex_input_build(vin, vs, vn, vib, &nvib, via, &nvia, vdiv, &ndiv, &out->vin_mask, &out->vin_dyn, out->vin_stride)) return -1;
    if (out->vin_dyn && !pvkCmdBindVertexBuffers2) { nvlog("apps-dyn pipeline: layout(s) %#x are MTLBufferLayoutStrideDynamic and "
        "vkCmdBindVertexBuffers2 is not loaded — REFUSED", out->vin_dyn); return -1; }
    int hasDepth = rt->dfmt != 0;
    out->has_depth = hasDepth; out->dfmt = rt->dfmt; out->cfmt = rt->ncol ? rt->cfmt[0] : 0u; out->samples = samples;
    out->ncol = rt->ncol; memcpy(out->cfmts, rt->cfmt, sizeof out->cfmts);
    out->has_stencil = rt->dfmt >= VK_FORMAT_S8_UINT && rt->dfmt <= VK_FORMAT_D32_SFLOAT_S8_UINT;
    for (uint32_t i = 0; samples > 1 && i < rt->ncol; i++) if (rt->cfmt[i]) out->rmask |= 1u << i;
    out->stex_hi[0] = nvmtl_spirv_stex_hi(vs, vn); out->stex_hi[1] = nvmtl_spirv_stex_hi(fs, fn);
    nvmtl_spirv_texel_mask(vs, vn, out->tbm[0]); nvmtl_spirv_texel_mask(fs, fn, out->tbm[1]);
    out->tbany = 0; for (int k = 0; k < 4; k++) out->tbany |= (out->tbm[0][k] | out->tbm[1][k]) != 0;
    (void)vkfmt;
    VkRenderPass pass = VK_NULL_HANDLE;
    VKCK(nvmtl_make_render_pass(out, 0, &pass), "vkCreateRenderPass");
    out->rp = pass;

    if (!nvmtl_spirv_ok(vs, vn, "vertex module") || !nvmtl_spirv_ok(fs, fn, "fragment module")) return -1;
    if (!td) out->gmask_ok = (uint32_t)(nvmtl_spirv_bmask(vs, vn, out->gmask[0]) && nvmtl_spirv_bmask(fs, fn, out->gmask[1]));
    if (!td) { out->tabneed[0] = nvmtl_spirv_tabneed(vs, vn); out->tabneed[1] = nvmtl_spirv_tabneed(fs, fn);
        static _Atomic unsigned said; const unsigned k = atomic_fetch_add(&said, 1);
        if (k < 6) nvlog("stage 1d: render pipeline reads table entries vs %s%u+%u fs %s%u+%u (buffers+textures; all = whole table)",
                         out->tabneed[0] ? "" : "all ", NVMTL_TABNEED_BUFS(out->tabneed[0]), NVMTL_TABNEED_TEXS(out->tabneed[0]),
                         out->tabneed[1] ? "" : "all ", NVMTL_TABNEED_BUFS(out->tabneed[1]), NVMTL_TABNEED_TEXS(out->tabneed[1])); }
    if (!td) out->gmask2_ok = (uint32_t)(nvmtl_spirv_bmask_x(vs, vn, out->gmask2[0], 1) && nvmtl_spirv_bmask_x(fs, fn, out->gmask2[1], 1));
    VkShaderModuleCreateInfo vsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, vn, (const uint32_t *)vs };
    uint32_t *fcopy = (uint32_t *)malloc(fn);
    if (!fcopy) { nvlog("vk: out of memory copying the fragment module"); return -1; }
    memcpy(fcopy, fs, fn);
    if (ensure_descriptors()) { free(fcopy); return -1; }
    if (!g_heap_ready && (nvmtl_spirv_uses_set(vs, vn / 4, 2) || nvmtl_spirv_uses_set(fs, fn / 4, 2))) {
        free(fcopy); nvlog("vk: render pipeline REFUSED - a shader indexes the bindless heap (set 2) and this device has none"); return -1; }
    nvmtl_spirv_retarget_set(fcopy, fn / 4, 1);
    VkShaderModuleCreateInfo fsm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, fn, fcopy };
    VkShaderModule vmod = VK_NULL_HANDLE, fmod = VK_NULL_HANDLE, tvm = VK_NULL_HANDLE, tcm = VK_NULL_HANDLE;
    NVMTL_PIPELINE_STAGE(pvkCreateShaderModule(g_dev, &vsm, NULL, &vmod), "vkCreateShaderModule(vertex)");
    NVMTL_PIPELINE_STAGE(pvkCreateShaderModule(g_dev, &fsm, NULL, &fmod), "vkCreateShaderModule(fragment)");
    VkPipelineShaderStageCreateInfo stages[4] = {
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, td ? VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT : VK_SHADER_STAGE_VERTEX_BIT, vmod, "main", NULL },
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fmod, "main", NULL } };
    uint32_t nstages = 2;
    if (td) {
        VkShaderModuleCreateInfo tvi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, tvsd.length, (const uint32_t *)tvsd.bytes };
        VkShaderModuleCreateInfo tci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, ttcd.length, (const uint32_t *)ttcd.bytes };
        NVMTL_PIPELINE_STAGE(pvkCreateShaderModule(g_dev, &tvi, NULL, &tvm), "vkCreateShaderModule(tessellation vertex)");
        NVMTL_PIPELINE_STAGE(pvkCreateShaderModule(g_dev, &tci, NULL, &tcm), "vkCreateShaderModule(tessellation control)");
        stages[nstages++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, tvm, "main", NULL };
        stages[nstages++] = (VkPipelineShaderStageCreateInfo){ VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, tcm, "main", NULL };
    }

    if (ensure_descriptors()) goto pipeline_failed;
    VkDescriptorSetLayout sl2[4] = { g_setLayout, g_setLayout, g_heapLayout, VK_NULL_HANDLE };
    uint32_t nsl = g_heap_ready ? 3 : 2;
    out->dbplan[0] = out->dbplan[1] = NULL;
    if (g_dbuf) {   // a stage layout of the bindings each stage reads (all of them when the masks are unknown: tessellation)
        const nvmtl_dbplan *pv = nvmtl_db_plan(out->gmask_ok ? out->gmask[0] : NULL, out->stex_hi[0], out->tbany ? out->tbm[0] : NULL, 0);
        const nvmtl_dbplan *pf = nvmtl_db_plan(out->gmask_ok ? out->gmask[1] : NULL, out->stex_hi[1], out->tbany ? out->tbm[1] : NULL, 0);
        if (!pv || !pf) goto pipeline_failed;
        out->dbplan[0] = pv; out->dbplan[1] = pf; sl2[0] = pv->layout; sl2[1] = pf->layout;
    }
    out->fetch_mask = nvmtl_fbfetch_on() ? nvmtl_spirv_fetch_mask(fs, fn) : 0;
    out->fetch_zero = nvmtl_fbfetch_on() ? 0 : nvmtl_spirv_fetch_mask(fs, fn);
    if (out->fetch_mask || out->fetch_zero) {
        pthread_mutex_lock(&g_desc_lock); const int fl = nvmtl_fetch_layouts_locked(); pthread_mutex_unlock(&g_desc_lock);
        if (fl) { nvlog("vk: framebuffer fetch layouts could not be made - pipeline refused"); return -1; }
        if (!g_heap_ready) sl2[2] = g_emptyLayout;
        sl2[3] = g_fetchLayout; nsl = 4;
    }
    VkPushConstantRange pc[3]; uint32_t npc = 0;
    if (td) pc[npc++] = (VkPushConstantRange){ VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, 0, NVMTL_TESS_PC_BYTES };
    if (out->pc_table[0].count) pc[npc++] = (VkPushConstantRange){ VK_SHADER_STAGE_VERTEX_BIT,
        NVMTL_PC_TABLE_VS_OFFSET, out->pc_table[0].count * 8 };
    out->stencil_emu = nvmtl_spirv_stencil_emu(fs, fn);
    // NVMTL_SKIP_FS=<hex>[,<hex>...]: draws with a fragment shader of that FNV-1a hash (the trace's "fs %08x") are skipped,
    // to find which shader hangs the GPU. A diagnostic only; unset, nothing is skipped.
    { const char *sk = getenv("NVMTL_SKIP_FS"); out->skip_draws = 0;
      if (sk && *sk) { uint32_t hh = 2166136261u; const uint8_t *bb = (const uint8_t *)fs; for (size_t i = 0; i < fn; i++) { hh ^= bb[i]; hh *= 16777619u; }
          char want[16]; snprintf(want, sizeof want, "%08x", hh); if (strstr(sk, want)) { out->skip_draws = 1; nvlog("NVMTL_SKIP_FS: fragment shader %s - its draws are skipped", want); } } }
    uint32_t fragment_start = 256, fragment_end = 0;
    if (g_sample_locations) { fragment_start = NVMTL_SAMPLE_POSITION_OFFSET; fragment_end = NVMTL_SAMPLE_POSITION_OFFSET + NVMTL_SAMPLE_POSITION_BYTES; }
    if (out->stencil_emu) { if (NVMTL_STENCIL_EMU_OFFSET < fragment_start) fragment_start = NVMTL_STENCIL_EMU_OFFSET; fragment_end = NVMTL_STENCIL_EMU_OFFSET + 4u; }
    if (out->pc_table[1].count) { if (NVMTL_PC_TABLE_FS_OFFSET < fragment_start) fragment_start = NVMTL_PC_TABLE_FS_OFFSET;
        fragment_end = NVMTL_PC_TABLE_FS_OFFSET + out->pc_table[1].count * 8; }
    if (fragment_end) pc[npc++] = (VkPushConstantRange){ VK_SHADER_STAGE_FRAGMENT_BIT, fragment_start, fragment_end - fragment_start };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nsl, sl2, npc, npc ? pc : NULL };
    NVMTL_PIPELINE_STAGE(nvmtl_opaque_CreatePipelineLayout(g_dev, &plci, NULL, &out->layout), "vkCreatePipelineLayout");

    VkPipelineVertexInputDivisorStateCreateInfoKHR vdi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_DIVISOR_STATE_CREATE_INFO_KHR, NULL, ndiv, vdiv };
    VkPipelineVertexInputStateCreateInfo vi = { VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, ndiv ? &vdi : NULL, 0, nvib, vib, nvia, via };
    VkPipelineInputAssemblyStateCreateInfo ia = { VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0,
        td ? VK_PRIMITIVE_TOPOLOGY_PATCH_LIST : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE };
    VkPipelineTessellationStateCreateInfo tss = { VK_STRUCTURE_TYPE_PIPELINE_TESSELLATION_STATE_CREATE_INFO, NULL, 0, 1 };
    VkPipelineViewportStateCreateInfo vps = { VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL };
    // At most 25 states with stencil, depth bounds, dynamic vertex stride, line width, sample locations and dynamic-state3.
    VkDynamicState dyn[26] = { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR, VK_DYNAMIC_STATE_CULL_MODE,
        VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE, VK_DYNAMIC_STATE_DEPTH_BIAS,
        VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY, VK_DYNAMIC_STATE_PRIMITIVE_RESTART_ENABLE, VK_DYNAMIC_STATE_FRONT_FACE,
        VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE, VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE, VK_DYNAMIC_STATE_DEPTH_COMPARE_OP,
        VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE, VK_DYNAMIC_STATE_STENCIL_OP, VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK, VK_DYNAMIC_STATE_STENCIL_WRITE_MASK };
    uint32_t ndyn = hasDepth ? (out->has_stencil ? 16u : 12u) : 9u;
    dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_BIAS_ENABLE; // core Vulkan 1.3, like primitive-restart enable
    // Metal's depth bounds test (macOS 26) is set on the encoder, not the pipeline, and compares the depth already STORED
    // in the attachment, which is Vulkan's depth bounds test exactly (measured on Apple silicon: stored 0.25 with a
    // fragment at 0.9 and bounds [0.5, 1] is discarded). Both the switch and the range are dynamic.
    out->db_dyn = hasDepth && g_depth_bounds;
    if (out->db_dyn) { dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS_TEST_ENABLE; dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_BOUNDS; }
    if (g_dyn3) {
        dyn[ndyn++] = VK_DYNAMIC_STATE_POLYGON_MODE_EXT;
        if (g_provoking) dyn[ndyn++] = VK_DYNAMIC_STATE_PROVOKING_VERTEX_MODE_EXT;
        if (g_clip) dyn[ndyn++] = VK_DYNAMIC_STATE_DEPTH_CLIP_ENABLE_EXT;
    }
    if (out->vin_dyn) dyn[ndyn++] = VK_DYNAMIC_STATE_VERTEX_INPUT_BINDING_STRIDE;
    if (g_wide) dyn[ndyn++] = VK_DYNAMIC_STATE_LINE_WIDTH;
    if (g_sample_locations && samples > 1) dyn[ndyn++] = VK_DYNAMIC_STATE_SAMPLE_LOCATIONS_EXT;
    VkPipelineDynamicStateCreateInfo dsci = { VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, ndyn, dyn };
    // Metal has no depth-bias enable switch. Enable it dynamically for nonzero bias, keeping the ordinary zero-bias
    // path disabled rather than making every pipeline use depth bias, and reset it for every new Metal encoder.
    VkPipelineRasterizationStateCreateInfo rs = { VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, rt->discard ? VK_TRUE : VK_FALSE, VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f };
    if (rt->a2one && !g_a2one) { nvlog("pipeline REFUSED: alphaToOneEnabled and this device has no alphaToOne feature"); goto pipeline_failed; }
    VkPipelineMultisampleStateCreateInfo ms = { VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0,
        (VkSampleCountFlagBits)(samples > 1 ? samples : 1), VK_FALSE, 0, NULL,
        rt->a2c ? VK_TRUE : VK_FALSE, rt->a2one ? VK_TRUE : VK_FALSE };
    nvmtl_sample_pattern default_sample;
    VkSampleLocationEXT default_vk[8];
    VkPipelineSampleLocationsStateCreateInfoEXT sample_state = { VK_STRUCTURE_TYPE_PIPELINE_SAMPLE_LOCATIONS_STATE_CREATE_INFO_EXT };
    if (g_sample_locations && samples > 1) {
        if (nvmtl_sample_default(samples > 1 ? samples : 1, &default_sample)) goto pipeline_failed;
        sample_state.sampleLocationsEnable = VK_TRUE;
        sample_state.sampleLocationsInfo = nvmtl_sample_info(&default_sample, default_vk);
        ms.pNext = &sample_state;
    }
    VkPipelineColorBlendAttachmentState cba[NVMTL_NCOL];
    for (uint32_t i = 0; i < rt->ncol; i++) {
        const nvk_blend_state *b = &rt->blend[i];
        cba[i] = rt->cfmt[i]
            ? (VkPipelineColorBlendAttachmentState){ b->enabled, b->src_rgb, b->dst_rgb, b->op_rgb, b->src_alpha, b->dst_alpha, b->op_alpha, b->write_mask }
            : (VkPipelineColorBlendAttachmentState){ VK_FALSE, VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD,
                                                     VK_BLEND_FACTOR_ONE, VK_BLEND_FACTOR_ZERO, VK_BLEND_OP_ADD, 0 };
    }
    VkPipelineColorBlendStateCreateInfo cb = { VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0,
        VK_FALSE, VK_LOGIC_OP_COPY, rt->ncol, rt->ncol ? cba : NULL, { 0, 0, 0, 0 } };
    VkPipelineDepthStencilStateCreateInfo ds = { VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0,
        VK_TRUE, VK_TRUE, VK_COMPARE_OP_LESS, VK_FALSE, VK_FALSE, { 0 }, { 0 }, 0.0f, 1.0f };
    VkGraphicsPipelineCreateInfo gp = { VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(), nstages, stages,
        &vi, &ia, td ? &tss : NULL, &vps, &rs, &ms, hasDepth ? &ds : NULL, &cb, &dsci, out->layout, out->rp, 0, VK_NULL_HANDLE, -1 };
    NVMTL_PIPELINE_STAGE(nvmtl_opaque_CreateGraphicsPipelines(g_dev, VK_NULL_HANDLE, 1, &gp, NULL, &out->pipe), "vkCreateGraphicsPipelines");
    if (tcm) pvkDestroyShaderModule(g_dev, tcm, NULL);
    if (tvm) pvkDestroyShaderModule(g_dev, tvm, NULL);
    pvkDestroyShaderModule(g_dev, fmod, NULL);
    pvkDestroyShaderModule(g_dev, vmod, NULL);
    free(fcopy);
    return 0;
pipeline_failed:
    if (tcm) pvkDestroyShaderModule(g_dev, tcm, NULL);
    if (tvm) pvkDestroyShaderModule(g_dev, tvm, NULL);
    if (fmod) pvkDestroyShaderModule(g_dev, fmod, NULL);
    if (vmod) pvkDestroyShaderModule(g_dev, vmod, NULL);
    free(fcopy);
    return -1;
}

int nvmtl_vk_pipeline_create_fmt(const void *vs, size_t vn, const void *fs, size_t fn, int bgra, nvk_pipeline *out)
{ return nvmtl_vk_pipeline_create_depth(vs, vn, fs, fn, bgra, 0, out); }

int nvmtl_vk_pipeline_create(const void *vs, size_t vn, const void *fs, size_t fn, nvk_pipeline *out)
{ return nvmtl_vk_pipeline_create_fmt(vs, vn, fs, fn, 0, out); }

int nvmtl_vk_sampler_create(int linear, int repeat, nvk_sampler *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof *out);
    if (nvmtl_vk_init()) return -1;
    VkSamplerAddressMode am = repeat ? VK_SAMPLER_ADDRESS_MODE_REPEAT : VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    VkSamplerCreateInfo sci = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO };
    sci.magFilter = sci.minFilter = linear ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    sci.mipmapMode = linear ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = sci.addressModeV = sci.addressModeW = am;
    sci.maxLod = VK_LOD_CLAMP_NONE;
    VkSampler handle = VK_NULL_HANDLE;
    VKCK(nvmtl_sampler_acquire(&sci, &handle), "sampler acquire");
    out->s = (void *)handle;
    out->linear = linear; out->repeat = repeat; out->lod_max = VK_LOD_CLAMP_NONE;
    return 0;
}

int nvmtl_vk_sampler_create_desc(const nvmtl_sampler_desc *desc, nvk_sampler *out)
{
    if (!out) return -1;
    nvmtl_sampler_desc d = {0}; if (desc) d = *desc;
    memset(out, 0, sizeof *out);
    if (!desc || nvmtl_vk_init()) return -1;
    const nvmtl_sampler_caps caps = { g_sampler_anisotropy, g_sampler_mirror_clamp, g_lim.maxSamplerAnisotropy };
    VkSamplerCreateInfo sci;
    if (nvmtl_sampler_info(&d, &caps, &sci)) { nvlog("sampler descriptor unsupported or invalid - refused, not substituted"); return -1; }
    VkSampler h = VK_NULL_HANDLE;
    VkResult r = nvmtl_sampler_acquire(&sci, &h);
    if (r != VK_SUCCESS || h == VK_NULL_HANDLE) { nvlog("vkCreateSampler (full state) -> %d", r); return -1; }
    out->s = (void *)h; out->desc = d; out->full_state = 1; out->lod_min = d.lod_min; out->lod_max = d.lod_max;
    out->linear = d.min_filter || d.mag_filter; out->repeat = d.address_s == 2;
    return 0;
}

void nvmtl_vk_sampler_destroy(nvk_sampler *s)
{
    if (!s || !s->s) return;
    VkSampler handle = (VkSampler)s->s;
    s->s = NULL;
    nvmtl_sampler_release(handle);
}

static int nvmtl_vk_compute_pipeline_create_g(const void *cs, size_t n, uint32_t flags, nvk_pipeline *out);
int nvmtl_vk_compute_pipeline_create(const void *cs, size_t n, nvk_pipeline *out) { return nvmtl_vk_compute_pipeline_create_g(cs, n, 0, out); }
static int nvmtl_vk_compute_pipeline_create_g_lifetime_body(const void *cs, size_t n, uint32_t flags, nvk_pipeline *out);
static int nvmtl_vk_compute_pipeline_create_g(const void *cs, size_t n, uint32_t flags, nvk_pipeline *out)
{
    nvk_pipeline pending = {0};
    int result = nvmtl_vk_compute_pipeline_create_g_lifetime_body(cs, n, flags, &pending);
    if (result) { nvmtl_failed_pipeline(&pending); memset(out, 0, sizeof pending); }
    else *(nvk_pipeline *)out = pending;
    return result;
}
// 02ba248: a module that indexes a heap binding the heap was not created with (NVMTL_NO_HEAP_STORAGE=1 or a refused
// five-binding heap keeps two bindings, NVMTL_NO_ARGBUF_SAMPLERS=1 one; the descriptor-buffer model has at most two) reads
// its descriptor past the end of the set: a garbage image descriptor, and a store through it is a GPU fault at whatever
// address the garbage names. Such a pipeline is refused before it can run, and the log names it.
static int nvmtl_heap_binding_refused(const void *p, size_t n, const char *what)
{
    if (!p || ensure_descriptors() || !g_heap_ready || !g_heap_nbind) return 0;
    const int hi = nvmtl_spirv_heap_binding_max(p, n);
    if (hi < (int)g_heap_nbind) return 0;
    nvlog("pipeline REFUSED: the %s module indexes heap binding %d but the heap was created with %u binding(s) "
          "(NVMTL_NO_HEAP_STORAGE / NVMTL_NO_ARGBUF_SAMPLERS / descriptor-buffer model) - it would read a garbage descriptor", what, hi, g_heap_nbind);
    return 1;
}
static int nvmtl_vk_compute_pipeline_create_g_lifetime_body(const void *cs, size_t n, uint32_t flags, nvk_pipeline *out)
{
    if (nvmtl_heap_binding_refused(cs, n, "compute")) return -1;

    const void *const ag_src = cs; const size_t ag_srcn = n; uint32_t ag[4];
    NSData *data = nvmtl_address_shader_g(cs, n, flags, ag); if (!data) return -1;
    if (!(flags & 1) && ag[2]) {
        const unsigned long long k = atomic_fetch_add(&g_ag_untraceable, 1) + 1;
        if (!(k & (k - 1))) nvlog("align guard: %llu compute module(s) built SAFE - a promised access could not be traced to a binding", k);
        data = nvmtl_address_shader_g(cs, n, 1, NULL); if (!data) return -1;
        ag[0] = 0;
    }
    { const char *dd = getenv("NVMTL_SPV_DUMP");   // NVMTL_SPV_DUMP=<dir>: each compute module, as given and as lowered
      if (dd && *dd) { static _Atomic unsigned k; const unsigned i = atomic_fetch_add(&k, 1); char fp[600];
          snprintf(fp, sizeof fp, "%s/%d-%03u-in-%zu.spv", dd, (int)getpid(), i, ag_srcn); FILE *f = fopen(fp, "wb"); if (f) { fwrite(ag_src, 1, ag_srcn, f); fclose(f); }
          snprintf(fp, sizeof fp, "%s/%d-%03u-low-%u.spv", dd, (int)getpid(), i, flags); f = fopen(fp, "wb"); if (f) { fwrite(data.bytes, 1, data.length, f); fclose(f); } } }
    cs = data.bytes; n = data.length;
    if (nvmtl_vk_init()) return -1;
    nvmtl_pc_table cs_pc = {0};
    if (nvmtl_cs_pc_table_on()) {
        data = nvmtl_cs_pc_table_shader(data, &cs_pc); cs = data.bytes; n = data.length;
        const char *dd = getenv("NVMTL_SPV_DUMP");
        if (cs_pc.count && dd && *dd) { static _Atomic unsigned k; char fp[600];
            snprintf(fp, sizeof fp, "%s/%d-pc%03u-%u.spv", dd, (int)getpid(), atomic_fetch_add(&k, 1), flags);
            FILE *f = fopen(fp, "wb"); if (f) { fwrite(cs, 1, n, f); fclose(f); } }
    }
    if (ensure_descriptors()) return -1;
    if (!g_heap_ready && nvmtl_spirv_uses_set(cs, n / 4, 2)) {
        nvlog("vk: compute pipeline REFUSED - the kernel indexes the bindless heap (set 2) and this device has none"); return -1; }
    memset(out, 0, sizeof *out);
    out->pc_table[0] = cs_pc;
    out->cs_acc = nvmtl_cs_access(cs, n);
    { int wr = 1; out->cs_reach = cs_pc.count && nvmtl_cs_reach(cs, n, &wr); out->cs_wr = (uint8_t)wr; }
    if (!(flags & 1) && ag[0]) {
        if (!(out->ag_src = malloc(ag_srcn))) { nvlog("align guard: out of memory keeping a module's source - pipeline REFUSED"); return -1; }
        memcpy(out->ag_src, ag_src, ag_srcn); out->ag_srcn = ag_srcn; out->ag_mask = ag[0]; out->ag_req = ag[1] >= 8 ? ag[1] : 8;
        atomic_fetch_add(&g_ag_guarded, 1);
    }
    if (!nvmtl_spirv_ok(cs, n, "compute module")) return -1;
    out->stex_hi[0] = nvmtl_spirv_stex_hi(cs, n);
    nvmtl_spirv_texel_mask(cs, n, out->tbm[0]); memset(out->tbm[1], 0, sizeof out->tbm[1]);
    out->tbany = (out->tbm[0][0] | out->tbm[0][1] | out->tbm[0][2] | out->tbm[0][3]) != 0;
    out->lin = nvmtl_spirv_has_specid(cs, n / 4, 3);
    out->bmask_ok = (uint32_t)nvmtl_spirv_bmask(cs, n, out->bmask);
    if (!out->ag_mask) out->tabneed[0] = nvmtl_spirv_tabneed(cs, n);   /* an align-guarded kernel's SAFE twin is another module: whole table */
    out->bmask2_ok = (uint32_t)nvmtl_spirv_bmask_x(cs, n, out->bmask2, 1);
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, n, (const uint32_t *)cs };
    VkShaderModule mod = VK_NULL_HANDLE;
    VKCK(pvkCreateShaderModule(g_dev, &smi, NULL, &mod), "vkCreateShaderModule(compute)");
    out->mod = mod;
    VkDescriptorSetLayout sl2c[3] = { g_setLayout, g_setLayout, g_heapLayout };
    out->dbplan[0] = out->dbplan[1] = NULL;
    if (g_dbuf) {   // set 0: what the kernel reads; set 1 only if the module names it (the classic model bound it anyway)
        if (ensure_descriptors()) return -1;
        const nvmtl_dbplan *p0 = nvmtl_db_plan(out->bmask_ok ? out->bmask : NULL, out->stex_hi[0], out->tbany ? out->tbm[0] : NULL, 0);
        const nvmtl_dbplan *p1 = nvmtl_spirv_uses_set(cs, n / 4, 1) ? nvmtl_db_plan(out->bmask_ok ? out->bmask : NULL, out->stex_hi[1], NULL, 0)
                                                                     : nvmtl_db_plan(NULL, 0, NULL, 1);
        if (!p0 || !p1) return -1;
        out->dbplan[0] = p0; out->dbplan[1] = p1; sl2c[0] = p0->layout; sl2c[1] = p1->layout;
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, NVMTL_DISPATCH_PC_BYTES + out->pc_table[0].count * 8 };
    VkPipelineLayoutCreateInfo plci = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, g_heap_ready ? 3 : 2, sl2c, 1, &pcr };
    VKCK(nvmtl_opaque_CreatePipelineLayout(g_dev, &plci, NULL, &out->layout), "vkCreatePipelineLayout(compute)");
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL },
        out->layout, VK_NULL_HANDLE, -1 };
    VKCK(nvmtl_opaque_CreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &out->pipe), "vkCreateComputePipelines");
    out->mod = mod;
    return 0;
}

int nvmtl_vk_tg_reject(const uint32_t l[3], const uint32_t *t, const char *kernel, uint32_t lin)
{
    static _Atomic unsigned zero_n, over_n, lin_n;
    if (!l[0] || !l[1] || !l[2] || (t && (!t[0] || !t[1] || !t[2]))) {
        if (!zero_n++) nvlog("dispatch: zero dimension (%s grid %ux%ux%u tg %ux%ux%u) is a silent no-op, as on Apple - first of its kind in this process",
            kernel ? kernel : "-", t ? t[0] : 0, t ? t[1] : 0, t ? t[2] : 0, l[0], l[1], l[2]);
        return 1;
    }
    if (g_state != 1) return 0;
    const uint32_t *w = g_lim.maxComputeWorkGroupSize;
    if (lin && (l[0] > w[0] || l[1] > w[1] || l[2] > w[2]) && (uint64_t)l[0] * l[1] * l[2] <= g_lim.maxComputeWorkGroupInvocations) {
        if (!lin_n++) nvlog("dispatch: %s threadgroup %ux%ux%u runs LINEARIZED as %ux1x1 (SpecId 3) - over the per-dimension limit %ux%ux%u, inside %u invocations",
            kernel ? kernel : "-", l[0], l[1], l[2], l[0] * l[1] * l[2], w[0], w[1], w[2], g_lim.maxComputeWorkGroupInvocations);
        return 0;
    }
    if (l[0] > w[0] || l[1] > w[1] || l[2] > w[2] || (uint64_t)l[0] * l[1] * l[2] > g_lim.maxComputeWorkGroupInvocations) {
        unsigned n = ++over_n;
        if (n <= 16 || !(n & 1023))
            nvlog("dispatch REFUSED: %s threadgroup %ux%ux%u exceeds the hardware limit %ux%ux%u (%u invocations) - launched, QMD CTA_THREAD_DIMENSION2 (8 bits) wraps and SKEDCHECK16 kills the channel (Xid 13); skipped [%u so far]",
                  kernel ? kernel : "-", l[0], l[1], l[2], w[0], w[1], w[2], g_lim.maxComputeWorkGroupInvocations, n);
        return 1;
    }
    return 0;
}
static pthread_mutex_t g_var_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_ag_lock = PTHREAD_MUTEX_INITIALIZER;
static nvk_pipeline *nvmtl_align_guard_pick(nvk_cmdbuf *c, nvk_pipeline *p)
{
    if (!p->ag_mask) return p;
    const nvmtl_bindtable *t = (const nvmtl_bindtable *)c->table;
    const uint64_t m = (uint64_t)p->ag_req - 1;
    int bad = -1; uint64_t at = 0;
    for (uint32_t b = 0; b < 32 && b < NVMTL_NBUF; b++) {
        if (!(p->ag_mask & (1u << b))) continue;
        const nvmtl_slot *s = t ? &t->s[0][b] : NULL;
        if (s && s->kind == NVMTL_SLOT_BUF && ((s->address + s->off) & m)) { bad = (int)b; at = s->address + s->off; break; }
    }
    if (bad < 0) return p;
    pthread_mutex_lock(&g_ag_lock);
    nvk_pipeline *sp = (nvk_pipeline *)p->ag_safe;
    if (!sp && p->ag_src) {
        sp = calloc(1, sizeof *sp);
        if (sp && nvmtl_vk_compute_pipeline_create_g(p->ag_src, p->ag_srcn, 1, sp)) { free(sp); sp = NULL; }
        p->ag_safe = sp;
        nvlog("align guard: buffer %d bound at GPU address %#llx is not %u-aligned - %s", bad, (unsigned long long)at, p->ag_req,
              sp ? "this kernel now runs its SAFE module on such bindings (Apple runs a broken alignment promise exactly)" : "SAFE module FAILED - such dispatches are SKIPPED");
    }
    pthread_mutex_unlock(&g_ag_lock);
    const unsigned long long k = atomic_fetch_add(&g_ag_switched, 1) + 1;
    if (!(k & (k - 1))) nvlog("align guard: %llu dispatch(es) ran a SAFE module (%llu modules guarded)", k, (unsigned long long)atomic_load(&g_ag_guarded));
    return sp;
}
static VkPipeline nvmtl_compute_variant(nvk_pipeline *p, const uint32_t l[3])
{
    if (nvmtl_vk_tg_reject(l, NULL, "(variant)", p->lin)) return VK_NULL_HANDLE;
    if (!p->mod) return (VkPipeline)p->pipe;
    VkPipeline found = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_var_lock);
    for (uint32_t i = 0; i < p->nvar; i++) if (!memcmp(p->var[i].l, l, sizeof p->var[i].l)) { found = (VkPipeline)p->var[i].pipe; break; }
    if (!found) {
        const uint32_t *wl = g_lim.maxComputeWorkGroupSize;
        uint32_t sd[4] = { l[0], l[1], l[2], (p->lin && (l[0] > wl[0] || l[1] > wl[1] || l[2] > wl[2])) ? 1u : 0u };
        VkSpecializationMapEntry me[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
        VkSpecializationInfo si = { 4, me, 16, sd };
        // compute1: NVMTL_PSTAT statistics of the variant that runs (prod7 captured them only for the default-size pipeline,
        // so KTIME showed the SIMD width and spills of a 64x1x1 compile, not of the dispatched size)
        VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL,
            nvmtl_db_pipeline_flags() | (g_pstat ? VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR : 0),
            { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, (VkShaderModule)p->mod, "main", &si },
            (VkPipelineLayout)p->layout, VK_NULL_HANDLE, -1 };
        VkPipelineShaderStageRequiredSubgroupSizeCreateInfo rss = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_REQUIRED_SUBGROUP_SIZE_CREATE_INFO, NULL, g_cs_simd };
        VkResult r = VK_ERROR_UNKNOWN;
        if (g_cs_simd) {
            cpi.stage.pNext = &rss;
            r = pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &found);
            if (r != VK_SUCCESS) {
                if (found) pvkDestroyPipeline(g_dev, found, NULL);
                found = VK_NULL_HANDLE; cpi.stage.pNext = NULL;
                static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 8)
                    nvlog("vk: compute variant %ux%ux%u at forced SIMD%u -> %d - compiled at the compiler's own width instead", l[0], l[1], l[2], g_cs_simd, r);
            }
        }
        if (r != VK_SUCCESS) r = pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &cpi, NULL, &found);
        if (r != VK_SUCCESS) {
            if (found) pvkDestroyPipeline(g_dev, found, NULL);
            nvlog("vk: compute variant %ux%ux%u -> %d — this dispatch is SKIPPED", l[0], l[1], l[2], r); found = VK_NULL_HANDLE;
        }
        else {
            if (p->nvar == p->capvar && p->capvar < NVMTL_NVAR_MAX) {
                uint32_t cap = p->capvar ? p->capvar * 2 : NVMTL_NVAR;
                struct nvmtl_cvar *g = realloc(p->var, cap * sizeof *g);
                if (g) { p->var = g; p->capvar = cap;
                    if (cap > NVMTL_NVAR) nvlog("vk: cvar1 one kernel now has %u threadgroup-size variants (table grown to %u)", p->nvar, cap); }
            }
            if (p->nvar < p->capvar) { memcpy(p->var[p->nvar].l, l, sizeof p->var[p->nvar].l); p->var[p->nvar].pipe = found; p->nvar++; }
            else { static int said; if (!said++) nvlog("vk: one kernel was dispatched with more than %u threadgroup sizes — variants now leak", p->capvar); }
        }
    }
    pthread_mutex_unlock(&g_var_lock);
    return found;
}

int nvmtl_vk_cmd_dispatch_sass(nvk_cmdbuf *c,
                               const void *sass, uint32_t sass_len,
                               uint32_t regs, uint32_t smem, uint32_t barriers, uint32_t nparams,
                               const uint32_t threads[3], const uint32_t tg[3])
{
    static int (*hook)(void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
                       const uint64_t *, uint32_t, const uint32_t *, const uint32_t *);
    static int looked;
    uint64_t params[16];
    uint32_t grid[3];

    if (!c || !c->cb || !sass || !sass_len) return -1;
    if (nvmtl_vk_vendor() != 0x10de) return -3;     /* NVIDIA machine code: the caller dispatches the SPIR-V instead */
    if (nparams > 16) { nvlog("sass dispatch: %u parameters is more than 16 — REFUSED", nparams); return -2; }

    if (!looked) {

        const char *path = getenv("NVMTL_NVK_DYLIB");
        void *h = dlopen(path && *path && nvmtl_code_path_allowed(path) ? path : nvmtl_companion("libvulkan_nouveau.dylib"),
                         RTLD_NOW | RTLD_LOCAL);
        looked = 1;
        if (!h) nvlog("sass dispatch: cannot open NVK: %s", dlerror());
        else {
            hook = (int (*)(void *, const void *, uint32_t, uint32_t, uint32_t, uint32_t,
                            const uint64_t *, uint32_t, const uint32_t *, const uint32_t *))
                       dlsym(h, "nvmtl_nvk_dispatch_sass_v1");
            nvlog("sass dispatch: NVK hook %s", hook ? "found" : "MISSING — this NVK predates it");
        }
    }
    if (!hook) return -3;

    for (int d = 0; d < 3; d++) {
        if (!tg[d] || !threads[d]) return -4;
        if (threads[d] % tg[d]) {
            nvlog("sass dispatch: %u threads is not a whole number of %u-wide groups on axis %d — "
                  "REFUSED, the SPIR-V path owns the partial tail", threads[d], tg[d], d);
            return -5;
        }
        grid[d] = threads[d] / tg[d];
    }

    if (!c->table) { nvlog("sass dispatch: no bind table"); return -6; }
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    for (uint32_t i = 0; i < nparams; i++) {
        nvmtl_slot *s = &t->s[0][i];
        if (s->kind != NVMTL_SLOT_BUF || !s->address) {
            nvlog("sass dispatch: parameter %u wants a buffer at [[buffer(%u)]] and slot kind is %u "
                  "— REFUSED", i, i, s->kind);
            return -7;
        }
        params[i] = s->address + (uint64_t)s->off;
    }

    int rc = hook(c->cb, sass, sass_len, regs, smem, barriers, params, nparams, tg, grid);
    if (rc) { nvlog("sass dispatch: NVK refused it, rc=%d", rc); return -8; }
    if (nvmtl_trace_on())
        nvtrace("SASS DISPATCH %u B, regs %u, %u param(s) | tg %ux%ux%u grid %ux%ux%u | p0 0x%llx",
                sass_len, regs, nparams, tg[0], tg[1], tg[2], grid[0], grid[1], grid[2],
                nparams ? (unsigned long long)params[0] : 0ull);
    return 0;
}

/* NVMTL_KTIME=1: a timestamp pair around each compute dispatch (64 per command buffer), summed per kernel and logged every
   5 s as "KTIME": dispatches, GPU time per dispatch, threadgroup size, threads per dispatch, the share of the wall the
   kernel kept the GPU busy, and debug.intelxe.freq at that moment. The pair is written at BOTTOM_OF_PIPE: the first
   waits for the work before it, so a dispatch's time is its own when dispatches are ordered by barriers. */
static long double nvmtl_tick_delta(uint64_t value, uint64_t anchor, unsigned bits);
static int nvmtl_ktime_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_KTIME"); on = e && e[0] == '1';
        if (on) nvlog("kernel time: ON - each dispatch is timed on the GPU, KTIME lines every NVMTL_KTIME_MS (5000 ms) (NVMTL_KTIME)"); }
    return on;
}
static void nvmtl_kt_begin(nvk_cmdbuf *c, const nvk_pipeline *p, const uint32_t tg[3], uint64_t threads)
{
    if (!nvmtl_ktime_on() || !c || c->in_rp || c->kt_open || c->kt_n >= 64) return;
    if (!c->kt_pool) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 128, 0 };
        VkQueryPool qp; if (pvkCreateQueryPool(g_dev, &qi, NULL, &qp) != VK_SUCCESS) return;
        pvkCmdResetQueryPool(c->cb, qp, 0, 128); c->kt_pool = (void *)qp;
    }
    pvkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkQueryPool)c->kt_pool, 2 * c->kt_n);
    c->kt_p[c->kt_n] = p; c->kt_tg[c->kt_n] = tg ? tg[0] * tg[1] * tg[2] : 0; c->kt_thr[c->kt_n] = threads; c->kt_open = 1;
    c->kt_dsys[c->kt_n] = (c->kt_sys[0] | c->kt_sys[1]) != 0;
}
static void nvmtl_kt_end(nvk_cmdbuf *c)
{
    if (!c || !c->kt_open) return;
    pvkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkQueryPool)c->kt_pool, 2 * c->kt_n + 1);
    c->kt_n++; c->kt_open = 0;
}
typedef struct { const void *p; char name[48]; uint32_t tg; uint64_t n, ns, max, thr, nsys; char st[200]; } nvmtl_kt_row;
#define NVMTL_KT_ROWS 1024
static nvmtl_kt_row g_kt[NVMTL_KT_ROWS]; static unsigned g_nkt; static uint64_t g_kt_t0, g_kt_dropped; static pthread_mutex_t g_kt_lock = PTHREAD_MUTEX_INITIALIZER;
static int nvmtl_kt_cmp(const void *a, const void *b) { const uint64_t x = ((const nvmtl_kt_row *)a)->ns, y = ((const nvmtl_kt_row *)b)->ns; return x < y ? 1 : x > y ? -1 : 0; }
static void nvmtl_kt_collect(nvk_cmdbuf *c)
{
    if (!c->kt_pool) return;
    const unsigned n = c->kt_n; uint64_t r[128] = {0};
    if (n && pvkGetQueryPoolResults(g_dev, (VkQueryPool)c->kt_pool, 0, 2 * n, sizeof(uint64_t) * 2 * n, r, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
        const uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
        pthread_mutex_lock(&g_kt_lock);
        if (!g_kt_t0) g_kt_t0 = now;
        for (unsigned i = 0; i < n; i++) {
            const uint64_t ns = (uint64_t)((double)nvmtl_tick_delta(r[2 * i + 1], r[2 * i], g_timestamp_bits) * g_lim.timestampPeriod);
            const nvk_pipeline *p = (const nvk_pipeline *)c->kt_p[i];
            const char *nm = p && p->ktname[0] ? p->ktname : "?";   // the name too: a freed pipeline's address is reused
            // one row per kernel name and threadgroup size: Geekbench makes a pipeline per variant and filled 256 rows
            unsigned k = 0; while (k < g_nkt && !(g_kt[k].tg == c->kt_tg[i] && !strcmp(g_kt[k].name, nm))) k++;
            if (k == g_nkt) { if (g_nkt == NVMTL_KT_ROWS) { g_kt_dropped++; continue; }
                memset(&g_kt[k], 0, sizeof g_kt[k]); g_kt[k].p = p; g_kt[k].tg = c->kt_tg[i];
                strlcpy(g_kt[k].name, nm, sizeof g_kt[k].name); g_nkt++;
                void *vp = p ? ((p->nvar && p->var) ? p->var[0].pipe : p->pipe) : NULL;   // NVMTL_PSTAT: the compiler's view of the variant that ran (compute1)
                if (vp && nvmtl_vk_pipeline_stats(vp, g_kt[k].st, sizeof g_kt[k].st)) g_kt[k].st[0] = 0; }
            g_kt[k].n++; g_kt[k].ns += ns; g_kt[k].thr += c->kt_thr[i]; g_kt[k].nsys += c->kt_dsys[i]; if (ns > g_kt[k].max) g_kt[k].max = ns;
        }
        static uint64_t every = 0;   // NVMTL_KTIME_MS: the report interval (5000 ms)
        if (!every) { const char *e = getenv("NVMTL_KTIME_MS"); const long v = e ? atol(e) : 0; every = (uint64_t)(v > 0 ? v : 5000) * 1000000ull; }
        if (now - g_kt_t0 >= every) {
            static nvmtl_kt_row rows[NVMTL_KT_ROWS]; const unsigned nr = g_nkt; memcpy(rows, g_kt, nr * sizeof rows[0]);   // printed outside g_kt_lock: only the thread that reset g_kt_t0 for this interval gets here
            const double wall = (double)(now - g_kt_t0); g_nkt = 0; g_kt_t0 = now; const uint64_t dropped = g_kt_dropped; g_kt_dropped = 0;
            pthread_mutex_unlock(&g_kt_lock);
            qsort(rows, nr, sizeof rows[0], nvmtl_kt_cmp);
            int freq = 0; size_t fl = sizeof freq; if (sysctlbyname("debug.intelxe.freq", &freq, &fl, NULL, 0)) freq = -1;
            uint64_t tot = 0; for (unsigned i = 0; i < nr; i++) tot += rows[i].ns;
            uint64_t nd = 0; for (unsigned i = 0; i < nr; i++) nd += rows[i].n;
            nvlog_keep("KTIME %.1f s: %u kernels, %llu dispatches, GPU busy in timed dispatches %.1f%% of wall, debug.intelxe.freq (step-11 request knob, NOT the GT clock) %d%s", wall / 1e9, nr,
                  (unsigned long long)nd, 100.0 * (double)tot / wall, freq, dropped ? " (some kernels past the table not counted)" : "");
            for (unsigned i = 0; i < nr && i < 24; i++)
                nvlog_keep("KTIME   %-40s tg %4u: %6llu dispatches, %8.1f us each (max %8.1f), %6.1f%% of wall, %llu threads each, %llu with an unpooled system-memory buffer >= 64 KiB%s%s",
                      rows[i].name, rows[i].tg, (unsigned long long)rows[i].n, (double)rows[i].ns / (double)rows[i].n / 1e3, (double)rows[i].max / 1e3,
                      100.0 * (double)rows[i].ns / wall, (unsigned long long)(rows[i].thr / rows[i].n), (unsigned long long)rows[i].nsys,
                      rows[i].st[0] ? " |" : "", rows[i].st);
        } else pthread_mutex_unlock(&g_kt_lock);
    }
    pvkDestroyQueryPool(g_dev, (VkQueryPool)c->kt_pool, NULL); c->kt_pool = NULL; c->kt_n = 0; c->kt_open = 0;
}


/* b81204c: an ordering barrier after every compute dispatch and render pass - the barrier upstream's per-dispatch witness
   emitted (src compute + colour-attachment output, shader write -> transfer write), without the witness's record. On
   NVIDIA it was the only configuration in which a game's benchmark completed every time: a pass read what an earlier
   dispatch or render pass had not finished writing. Its Vulkan scope ends at TRANSFER, so what it bought there is NVK's
   whole-queue drain on every barrier; on ANV it is a CS stall after every dispatch and every render pass (WindowServer
   included). B580: OFF unless NVMTL_ORDER_BARRIER=1, for an A/B when a game shows the same symptoms. */
static int nvmtl_order_barrier_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_ORDER_BARRIER"); on = e && e[0] == '1';
        if (on) nvlog("vk: ordering barrier after every compute dispatch and render pass ON (NVMTL_ORDER_BARRIER=1)"); }
    return on;
}
static void nvmtl_order_barrier(nvk_cmdbuf *c)
{
    if (!c->cb || !nvmtl_order_barrier_on()) return;
    VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &after, 0, NULL, 0, NULL);
}
void nvmtl_vk_cmd_dispatch_threads(nvk_cmdbuf *c, nvk_pipeline *p, const uint32_t threads[3], const uint32_t tgIn[3])
{
    uint32_t tg[3], groups[3];
    for (int d = 0; d < 3; d++) { if (!tgIn[d] || !threads[d]) return; tg[d] = tgIn[d]; groups[d] = (threads[d] + tg[d] - 1) / tg[d]; }
    if (!(p = nvmtl_align_guard_pick(c, p))) { c->skipped++; return; }
    VkDescriptorSet s[3] = { VK_NULL_HANDLE, VK_NULL_HANDLE, g_heapSet };
    if (g_dbuf) { if (nvmtl_db_bind_compute(c, p)) return; }
    else {
        s[0] = nvmtl_snapshot_set_c(c, 0, p); s[1] = nvmtl_snapshot_set_c(c, 1, p);
        if (!s[0] || !s[1]) { c->nobind = 1; return; }
        c->nobind = 0;
    }
    int bound = g_dbuf;   // the buffer model's offsets are set; classic sets are bound after the first pipeline
    uint32_t pc[12 + 2 * NVMTL_PC_TABLE_MAX];
    const uint32_t pcb = nvmtl_cs_pc_values(c, p, pc);   // compute2: the table qwords after the dispatch region
    nvmtl_kt_begin(c, p, tg, (uint64_t)threads[0] * threads[1] * threads[2]);
    for (uint32_t mask = 0; mask < 8; mask++) {
        uint32_t local[3], gc[3], tbase[3] = { 0, 0, 0 }, gbase[3] = { 0, 0, 0 }; int nonempty = 1;
        for (int d = 0; d < 3; d++) {
            uint32_t full = threads[d] / tg[d], tail = threads[d] % tg[d];
            local[d] = tg[d]; gc[d] = 0;
            if (!(mask & (1u << d))) gc[d] = full;
            else if (!tail) nonempty = 0;
            else { local[d] = tail; gc[d] = 1; tbase[d] = full * tg[d]; gbase[d] = full; }
            if (!gc[d]) nonempty = 0;
        }
        if (!nonempty) continue;
        VkPipeline vp = nvmtl_compute_variant(p, local);
        if (!vp) { c->skipped++; continue; }
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vp);
        if (!bound) { pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)p->layout, 0, g_heapSet ? 3 : 2, s, nvmtl_table_ubo() ? 2 : 0, c->dyn_out); bound = 1; }
        // Metal allows any group count per axis; Vulkan caps it (NVK: 0x7fffffff x 65535 x 65535, the width of the QMD
        // grid fields). Geekbench's OpenCL Face Detection dispatched past 65535 on y and NAK's nak_fill_qmd aborted the
        // process. Split into chunks and advance thread_base/threadgroup_base, which the translated kernel adds to
        // GlobalInvocationId/WorkgroupId, so every chunk sees the positions of the whole grid.
        uint32_t cap[3];
        for (int d = 0; d < 3; d++) cap[d] = g_lim.maxComputeWorkGroupCount[d] ? g_lim.maxComputeWorkGroupCount[d] : 65535;
        if ((gc[0] > cap[0] || gc[1] > cap[1] || gc[2] > cap[2])) {
            static int said;
            if (!said++) nvlog("vk: dispatch of %ux%ux%u groups exceeds the device's %ux%ux%u — split into chunks",
                               gc[0], gc[1], gc[2], cap[0], cap[1], cap[2]);
        }
        for (uint32_t z = 0; z < gc[2]; z += cap[2])
        for (uint32_t y = 0; y < gc[1]; y += cap[1])
        for (uint32_t x = 0; x < gc[0]; x += cap[0]) {
            uint32_t off[3] = { x, y, z }, n[3];
            for (int d = 0; d < 3; d++) n[d] = gc[d] - off[d] < cap[d] ? gc[d] - off[d] : cap[d];
            const uint32_t dr[12] = { threads[0], threads[1], threads[2],
                                tbase[0] + off[0] * local[0], tbase[1] + off[1] * local[1], tbase[2] + off[2] * local[2],
                                gbase[0] + off[0], gbase[1] + off[1], gbase[2] + off[2], groups[0], groups[1], groups[2] };
            memcpy(pc, dr, sizeof dr);
            pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcb, pc);
            pvkCmdDispatch(c->cb, n[0], n[1], n[2]);
            nvmtl_order_barrier(c);
        }
    }
    nvmtl_kt_end(c);
}
void nvmtl_vk_cmd_dispatch_indirect_tg(nvk_cmdbuf *c, nvk_pipeline *p, nvk_buffer *b, size_t off, const uint32_t tgIn[3])
{
    uint32_t tg[3], g[3] = { 0, 0, 0 };
    for (int d = 0; d < 3; d++) { if (!tgIn[d]) return; tg[d] = tgIn[d]; }
    if (!(p = nvmtl_align_guard_pick(c, p))) { c->skipped++; return; }
    VkPipeline vp = nvmtl_compute_variant(p, tg);
    if (!vp) { c->skipped++; return; }
    (void)g;   // the grid is the GPU's to write: never read at record time (it was stale or 0, Eevee's light culling)
    if (g_dbuf) {
        if (nvmtl_db_bind_compute(c, p)) return;
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vp);
    } else {
        VkDescriptorSet s[3] = { nvmtl_snapshot_set_c(c, 0, p), nvmtl_snapshot_set_c(c, 1, p), g_heapSet };
        if (!s[0] || !s[1]) { c->nobind = 1; return; }
        c->nobind = 0;
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vp);
        pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)p->layout, 0, g_heapSet ? 3 : 2, s, nvmtl_table_ubo() ? 2 : 0, c->dyn_out);
    }
    // 0x80000000 | threadgroup size: the shader takes threads_per_grid and threadgroups_per_grid from NumWorkgroups
    // (translator stage_input/kernel_grid.rs), the grid the GPU actually dispatched
    const uint32_t F = 0x80000000u;
    uint32_t pc[12 + 2 * NVMTL_PC_TABLE_MAX] = { F | tg[0], F | tg[1], F | tg[2], 0, 0, 0, 0, 0, 0, F, F, F };
    const uint32_t pcb = nvmtl_cs_pc_values(c, p, pc);   // compute2: the table qwords after the dispatch region
    pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcb, pc);
    nvmtl_kt_begin(c, p, tg, 0);
    pvkCmdDispatchIndirect(c->cb, (VkBuffer)b->buf, off);
    nvmtl_order_barrier(c);
    nvmtl_kt_end(c);
}
void nvmtl_vk_cmd_dispatch(nvk_cmdbuf *c, nvk_pipeline *p, uint32_t gx, uint32_t gy, uint32_t gz)
{
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)p->pipe);
    if (g_dbuf) { if (nvmtl_db_bind_compute(c, p)) return; }
    else {
        VkDescriptorSet s[3] = { nvmtl_snapshot_set_t(c, 0, p->stex_hi[0], p->tbany ? p->tbm[0] : NULL),
                                 nvmtl_snapshot_set_t(c, 1, p->stex_hi[1], p->tbany ? p->tbm[1] : NULL), g_heapSet };
        if (!s[0] || !s[1]) { c->nobind = 1; return; }
        c->nobind = 0;
        pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)p->layout, 0, g_heapSet ? 3 : 2, s, nvmtl_table_ubo() ? 2 : 0, c->dyn_out);
    }
    if (p->pc_table[0].count) {   // compute2: a converted kernel reads its addresses from push constants on every path
        uint32_t pc[12 + 2 * NVMTL_PC_TABLE_MAX] = {0}; const uint32_t pcb = nvmtl_cs_pc_values(c, p, pc);
        pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcb, pc);
    }
    // This path reads WorkgroupId raw (no base push constants), so it cannot be chunked. Past the device's group
    // count NAK aborts the whole app inside nak_fill_qmd; refuse the one dispatch loudly instead.
    if ((g_lim.maxComputeWorkGroupCount[1] && gy > g_lim.maxComputeWorkGroupCount[1]) ||
        (g_lim.maxComputeWorkGroupCount[2] && gz > g_lim.maxComputeWorkGroupCount[2]) ||
        (g_lim.maxComputeWorkGroupCount[0] && gx > g_lim.maxComputeWorkGroupCount[0])) {
        nvlog("vk: raw dispatch of %ux%ux%u groups exceeds the device limit — SKIPPED", gx, gy, gz);
        c->skipped++; return;
    }
    pvkCmdDispatch(c->cb, gx, gy, gz);
    nvmtl_order_barrier(c);
}

int nvmtl_vk_queue_create(nvk_queue *out)
{
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out);
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, g_qfam };
    VKCK(nvmtl_opaque_CreateCommandPool(g_dev, &cpci, NULL, &out->pool), "vkCreateCommandPool");
    return 0;
}

static long double nvmtl_tick_delta(uint64_t value, uint64_t anchor, unsigned bits)
{
    uint64_t d = value - anchor;
    if (bits < 64) d &= (1ull << bits) - 1;
    if (d & (1ull << (bits - 1))) {
        uint64_t magnitude = bits == 64 ? (~d + 1) : ((1ull << bits) - d);
        return -(long double)magnitude;
    }
    return (long double)d;
}
void nvmtl_vk_clock_refresh(void)
{
    if (g_state != 1 || !pvkGetCalibratedTimestampsEXT || !g_timestamp_bits) return;
    pthread_mutex_lock(&g_gpu_clock_lock);
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (g_gpu_clock.valid && now >= g_gpu_clock.uptime_ns &&
        now - g_gpu_clock.uptime_ns < 1000000000ull) {
        pthread_mutex_unlock(&g_gpu_clock_lock); return;
    }
    nvmtl_gpu_clock best = {0}, previous = {0};
    unsigned advancing = 0;
    VkCalibratedTimestampInfoEXT info = { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT };
    for (unsigned i = 0; i < 3; ++i) {
        uint64_t tick = 0, deviation = UINT64_MAX;
        uint64_t before = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        VkResult rc = pvkGetCalibratedTimestampsEXT(g_dev, 1, &info, &tick, &deviation);
        uint64_t after = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        if (rc != VK_SUCCESS || after < before || after - before > 2000000ull || deviation > 1000000ull) break;
        uint64_t error = (after - before + 1) / 2 + deviation;
        if (error > 1000000ull) break;
        nvmtl_gpu_clock sample = { tick, before + (after-before)/2, error,
                                  g_timestamp_bits, g_lim.timestampPeriod, 1 };
        if (previous.valid) {
            long double elapsed = nvmtl_tick_delta(tick, previous.gpu_tick, g_timestamp_bits) * sample.period_ns;
            long double host = (long double)sample.uptime_ns - previous.uptime_ns;
            long double tolerance = (long double)error + previous.uncertainty_ns + 2000;
            if (elapsed <= 0 || fabsl(elapsed - host) > tolerance) break;
            ++advancing;
        }
        previous = sample;
        if (!best.valid || error < best.uncertainty_ns) best = sample;
    }
    g_gpu_clock = advancing == 2 ? best : (nvmtl_gpu_clock){0};
    pthread_mutex_unlock(&g_gpu_clock_lock);
}
static nvmtl_gpu_clock nvmtl_clock_snapshot(void)
{
    pthread_mutex_lock(&g_gpu_clock_lock);
    nvmtl_gpu_clock clock = g_gpu_clock;
    pthread_mutex_unlock(&g_gpu_clock_lock);
    return clock;
}
int nvmtl_vk_interval_seconds(const nvmtl_gpu_interval *t, double *start, double *end)
{
    if (start) *start = 0;
    if (end) *end = 0;
    if (!t || !t->valid || !t->clock.valid || !t->clock.valid_bits || t->clock.valid_bits > 64) return -1;
    long double a = (long double)t->clock.uptime_ns +
        nvmtl_tick_delta(t->start_tick, t->clock.gpu_tick, t->clock.valid_bits) * t->clock.period_ns;
    long double b = (long double)t->clock.uptime_ns +
        nvmtl_tick_delta(t->end_tick, t->clock.gpu_tick, t->clock.valid_bits) * t->clock.period_ns;
    long double error = t->clock.uncertainty_ns;
    if (!isfinite(a) || !isfinite(b) || a <= 0 || b < a) return -1;
    const long double lo = (long double)t->host_before_ns - error, hi = (long double)t->host_after_ns + error, dur = b - a;
    if (a < lo || b > hi) {
        if (dur > hi - lo) return -1;
        if (b > hi) { b = hi; a = b - dur; }
        if (a < lo) { a = lo; b = a + dur; }
    }
    if (start) *start = (double)(a / 1e9L);
    if (end) *end = (double)(b / 1e9L);
    return 0;
}
#define NVMTL_TSPOOL_MAX 256
static VkQueryPool g_tspool[NVMTL_TSPOOL_MAX]; static unsigned g_ntspool; static pthread_mutex_t g_tspool_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned nvmtl_tspool_cap(void) {
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("NVMTL_TSPOOL_KEEP"); int v = e ? atoi(e) : NVMTL_TSPOOL_MAX;
        cap = v < 0 ? 0 : v > NVMTL_TSPOOL_MAX ? NVMTL_TSPOOL_MAX : v;
        nvlog("vk: timing query pools recycled (keep <= %d, NVMTL_TSPOOL_KEEP=%s) - one mapped allocation per pool, not per command buffer", cap, e ? e : "unset"); }
    return (unsigned)cap;
}
static VkQueryPool nvmtl_tspool_pop(void) {
    VkQueryPool q = VK_NULL_HANDLE;
    if (!nvmtl_tspool_cap()) return q;
    pthread_mutex_lock(&g_tspool_lock); if (g_ntspool) q = g_tspool[--g_ntspool]; pthread_mutex_unlock(&g_tspool_lock);
    return q;
}
static void nvmtl_tspool_push(VkQueryPool q) {
    int kept = 0;
    pthread_mutex_lock(&g_tspool_lock);
    if (g_ntspool < nvmtl_tspool_cap()) { g_tspool[g_ntspool++] = q; kept = 1; }
    pthread_mutex_unlock(&g_tspool_lock);
    if (!kept) pvkDestroyQueryPool(g_dev, q, NULL);
}
// Every caller of nvmtl_vk_cmd_begin sees only -1, and the queue's nil for WindowServer used to carry no cause. The
// reason is logged here with the Vulkan result where there is one. One call site per cause: nvlog throttles by format.
static int nvmtl_cmd_refused(nvmtl_refusal why, int vkresult)
{
    switch (why) {
    case NVMTL_REFUSE_BACKEND_DOWN: nvlog("vk: command buffer refused in %s: " NVMTL_REFUSE_TEXT_BACKEND_DOWN, getprogname()); break;
    case NVMTL_REFUSE_POOL:  nvlog("vk: command buffer refused in %s: " NVMTL_REFUSE_TEXT_POOL " (VkResult %d)", getprogname(), vkresult); break;
    case NVMTL_REFUSE_ALLOC: nvlog("vk: command buffer refused in %s: " NVMTL_REFUSE_TEXT_ALLOC " (VkResult %d)", getprogname(), vkresult); break;
    case NVMTL_REFUSE_BEGIN: nvlog("vk: command buffer refused in %s: " NVMTL_REFUSE_TEXT_BEGIN " (VkResult %d)", getprogname(), vkresult); break;
    default: break;
    }
    return -1;
}
int nvmtl_vk_cmd_begin(nvk_queue *q, nvk_cmdbuf *out)
{
    (void)q;
    if (nvmtl_vk_init()) {
        if (!atomic_load(&g_lost_after_up)) return nvmtl_cmd_refused(NVMTL_REFUSE_BACKEND_DOWN, 0);
        memset(out, 0, sizeof *out); out->dead = 1; out->cb = &g_dead_cb_token;   // a token, never dereferenced
        static _Atomic unsigned long dead_made;
        const unsigned long n = atomic_fetch_add(&dead_made, 1) + 1;
        if (n < 4 || n % 1000 == 0) nvlog("vk: command buffer #%lu made after the device was lost in %s: it records nothing and completes with a device-removed error", n, getprogname());
        return 0;
    }
    const uint64_t b0 = nvmtl_perf_now();
    memset(out, 0, sizeof *out);
    VkCommandPool pool = nvmtl_keep_pool_pop();
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
        VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, g_qfam };
    VkResult result = pool != VK_NULL_HANDLE ? VK_SUCCESS : pvkCreateCommandPool(g_dev, &cpci, NULL, &pool);
    if (result != VK_SUCCESS) { nvlog("vk: recording pool creation -> %d", result); return nvmtl_cmd_refused(NVMTL_REFUSE_POOL, result); }
    out->pool = pool;
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    result = nvmtl_opaque_AllocateCommandBuffers(g_dev, &ai, &out->cb);
    if (result != VK_SUCCESS) { pvkDestroyCommandPool(g_dev, pool, NULL); memset(out, 0, sizeof *out); return nvmtl_cmd_refused(NVMTL_REFUSE_ALLOC, result); }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
        VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    result = pvkBeginCommandBuffer(out->cb, &bi);
    if (result != VK_SUCCESS) { pvkDestroyCommandPool(g_dev, pool, NULL); memset(out, 0, sizeof *out); return nvmtl_cmd_refused(NVMTL_REFUSE_BEGIN, result); }
    if (pvkGetCalibratedTimestampsEXT && g_timestamp_bits) {
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2, 0 };
        VkQueryPool timing = nvmtl_tspool_pop();
        if (timing != VK_NULL_HANDLE || pvkCreateQueryPool(g_dev, &qi, NULL, &timing) == VK_SUCCESS) {
            out->timing_pool = timing;
            pvkCmdResetQueryPool(out->cb, timing, 0, 2);
            pvkCmdWriteTimestamp(out->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, timing, 0);
        }
    }
    g_cmd_begun++;
    atomic_fetch_add(&g_cb_live, 1); out->life_live = 1;
    nvmtl_gpt_begin(out);
    nvmtl_perf_note(NVP_BEGIN, b0, 0);
    return 0;
}

static pthread_mutex_t g_rpv_lock = PTHREAD_MUTEX_INITIALIZER;
static void *nvmtl_rp_variant(nvk_pipeline *p, uint32_t load, uint32_t was)
{
    uint32_t cf[NVMTL_NCOL], rmask, ncol = nvmtl_rt_shape(p, cf, &rmask), keep = 0;
    for (uint32_t i = 0; i < ncol; i++) if (cf[i]) keep |= NVMTL_LOAD_COLOUR(i);
    if (p->has_depth) keep |= NVMTL_LOAD_DEPTH | (p->has_stencil ? NVMTL_LOAD_STENCIL : 0u);
    load &= keep;
    {
        static uint32_t seenmrt, seenany;
        if (ncol >= 2 && !(__atomic_fetch_or(&seenmrt, 1u << ncol, __ATOMIC_RELAXED) & (1u << ncol)))
            nvlog("vk: apps2 MRT PASS: %u colour slots (load 0x%x of 0x%x possible, depth %d, stencil %u, samples %u, resolve 0x%x)",
                  ncol, load, keep, p->has_depth, p->has_stencil, p->samples, rmask);
        if (!__atomic_fetch_or(&seenany, 1u, __ATOMIC_RELAXED))
            nvlog("vk: apps2 pass-shape instrument LIVE (first pass: %u colour slot(s), depth %d) — a run with no "
                  "'MRT PASS' line above therefore MEASURED that every render pass in this process had one colour slot",
                  ncol, p->has_depth);
    }
    if (!load) return p->rp;
    pthread_mutex_lock(&g_rpv_lock);
    int legacy = load < 4u; void *rp = legacy ? p->rpv[load] : NULL;
    for (uint32_t i = 0; !legacy && i < p->nrpx; i++) if (p->rpxk[i] == load) { rp = p->rpx[i]; break; }
    if (!rp && !legacy && p->nrpx >= p->crpx && p->crpx < NVMTL_RPX_MAX) {
        uint32_t cap = p->crpx ? p->crpx * 2u : NVMTL_RPX_GROW;
        if (cap > NVMTL_RPX_MAX) cap = NVMTL_RPX_MAX;
        void **gx = (void **)realloc(p->rpx, cap * sizeof *gx);
        if (gx) { p->rpx = gx; uint32_t *gk = (uint32_t *)realloc(p->rpxk, cap * sizeof *gk); if (gk) { p->rpxk = gk; p->crpx = cap; } }
    }
    if (!rp && !legacy && p->nrpx >= p->crpx)
        nvlog("vk: apps2 load variant 0x%x on pipeline %p: rpx holds %u of a DERIVED ceiling of %u and did not grow — "
              "THIS PASS CLEARS INSTEAD OF LOADING and the attachment's previous contents are LOST",
              load, (void *)p, p->crpx, NVMTL_RPX_MAX);
    else if (!rp) {
        VkRenderPass made = VK_NULL_HANDLE;
        VkResult r = nvmtl_make_render_pass(p, load, &made);
        if (r == VK_SUCCESS && legacy) { rp = p->rpv[load] = (void *)made; nvlog("vk: G11 load-action render pass variant %u made (%u colour slot(s), colour %s, depth %s)", load, ncol, (load & 1u) ? "LOAD" : "clear", (load & 2u) ? "LOAD" : "clear"); }
        else if (r == VK_SUCCESS) { rp = (void *)made; p->rpxk[p->nrpx] = load; p->rpx[p->nrpx++] = rp;
            nvlog("vk: apps2 load variant 0x%x made (%u colour slot(s), stencil %s)", load, ncol, (load & NVMTL_LOAD_STENCIL) ? "LOAD" : "clear"); }
        else if (legacy) nvlog("vk: G11 vkCreateRenderPass(variant %u) -> %d — this pass CLEARS instead of loading", load, r);
        else nvlog("vk: apps2 vkCreateRenderPass(variant 0x%x) -> %d — this pass CLEARS instead of loading", load, r);
    }
    pthread_mutex_unlock(&g_rpv_lock);
    if (rp && (load & 1u) && was != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL && was != VK_IMAGE_LAYOUT_UNDEFINED) {
        static int said; if (!said++) nvlog("vk: G11 a Load pass began on an image resting in layout %u (declared TRANSFER_SRC)", was);
    }
    return rp;
}

int nvmtl_vk_cmd_begin_render_ex(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4], uint32_t load)
{ return nvmtl_vk_cmd_begin_render_ms(c, img, p, clear, load, NULL); }
static void nvmtl_clear_colour(VkClearColorValue *cv, uint32_t fmt, const float c[4])
{
    int kind = 0; double lo = 0, hi[4] = { 0, 0, 0, 0 }, m = 0;
    switch (fmt) {
        case VK_FORMAT_R8_UINT: case VK_FORMAT_R8G8_UINT: case VK_FORMAT_R8G8B8A8_UINT: case VK_FORMAT_B8G8R8A8_UINT: kind = 1; m = 255; break;
        case VK_FORMAT_R16_UINT: case VK_FORMAT_R16G16_UINT: case VK_FORMAT_R16G16B16A16_UINT: kind = 1; m = 65535; break;
        case VK_FORMAT_R32_UINT: case VK_FORMAT_R32G32_UINT: case VK_FORMAT_R32G32B32A32_UINT: kind = 1; m = 4294967295.0; break;
        case VK_FORMAT_A2B10G10R10_UINT_PACK32: case VK_FORMAT_A2R10G10B10_UINT_PACK32: kind = 1; m = -1; break;
        case VK_FORMAT_R8_SINT: case VK_FORMAT_R8G8_SINT: case VK_FORMAT_R8G8B8A8_SINT: kind = 2; m = 127; break;
        case VK_FORMAT_R16_SINT: case VK_FORMAT_R16G16_SINT: case VK_FORMAT_R16G16B16A16_SINT: kind = 2; m = 32767; break;
        case VK_FORMAT_R32_SINT: case VK_FORMAT_R32G32_SINT: case VK_FORMAT_R32G32B32A32_SINT: kind = 2; m = 2147483647.0; break;
        default: memcpy(cv->float32, c, 4 * sizeof(float)); return;
    }
    for (int i = 0; i < 4; i++) hi[i] = m < 0 ? (i == 3 ? 3 : 1023) : m;
    for (int i = 0; i < 4; i++) {
        double v = c[i];
        if (kind == 1) { lo = 0; v = v < lo ? lo : v > hi[i] ? hi[i] : v; cv->uint32[i] = (uint32_t)v; }
        else { lo = -hi[i] - 1; v = v < lo ? lo : v > hi[i] ? hi[i] : v; cv->int32[i] = (int32_t)v; }
    }
}

static void nvmtl_private_depth_layout(nvk_cmdbuf *, nvk_image *, VkImageLayout);
int nvmtl_vk_cmd_begin_pass(nvk_cmdbuf *c, const nvk_pass *ps, nvk_pipeline *p)
{
    if (!p->rp || !ps->w || !ps->h) { nvlog("begin_render: apps2 no render pass or a %ux%u pass — pass REFUSED", ps->w, ps->h); return -1; }
    nvmtl_sample_pattern pattern = ps->sample_pattern;
    if (!pattern.count && nvmtl_sample_default(p->samples > 1 ? p->samples : 1, &pattern)) return -1;
    if (!nvmtl_sample_pattern_valid(&pattern) || pattern.count != (p->samples > 1 ? p->samples : 1)) {
        nvlog("sample positions: count/pattern incompatible with render pipeline"); return -1;
    }
    if (nvmtl_sample_pattern_quantize(&pattern, &pattern)) return -1;
    if (ps->sample_pattern.count && !g_sample_locations) {
        /* No programmable-position backend: the pass draws at the standard positions. A pass asking for exactly those
           (what getDefaultSamplePositions returns) is plain MSAA. Any other pattern is drawn at the standard positions
           too, said once, instead of refusing the pass: refusing it left Chess's 4x MSAA passes black. */
        nvmtl_sample_pattern std;
        int same = !nvmtl_sample_default(pattern.count, &std) && !nvmtl_sample_pattern_quantize(&std, &std) &&
                   !memcmp(std.position, pattern.position, pattern.count * sizeof pattern.position[0]);
        static int said;
        if (!same && said++ < 4) nvlog("sample positions: backend unavailable; custom %u-sample pattern drawn at the standard positions", pattern.count);
    }
    uint32_t cf[NVMTL_NCOL], rmask, ncol = nvmtl_rt_shape(p, cf, &rmask);
    for (uint32_t i = 0; i < ncol; i++) {
        if (!cf[i]) continue;
        nvk_image *img = ps->col[i], *resolve = ps->res[i];
        if (!img || !ps->colview[i]) { nvlog("begin_render: apps2 the pipeline draws colour attachment %u and the pass has no texture there — pass REFUSED", i); return -1; }
        if (p->samples > 1) {
            if (img->samples != p->samples) { nvlog("begin_render: pipeline has %u samples but the colour target has %u — pass REFUSED", p->samples, img->samples); return -1; }
            if ((rmask & (1u << i)) && resolve && (resolve->samples > 1 || ps->resw[i] != ps->colw[i] || ps->resh[i] != ps->colh[i])) { nvlog("begin_render: resolve texture must be 1-sample and the same size — pass REFUSED"); return -1; }
        } else if (img->samples > 1) { nvlog("begin_render: a 1-sample pipeline on a %u-sample target — pass REFUSED", img->samples); return -1; }
        // The render pass is built in the pipeline's format and the attachment view is in the texture's: Vulkan's render
        // pass begin asserts they are equal, and that assert aborted the app (1.13 crash report, iOS Simulator). WAS a
        // census line only. The pass is refused instead, like the sample-count mismatches above.
        if ((ps->colfmt[i] ? ps->colfmt[i] : img->fmt) != cf[i]) { static int said; if (said++ < 4) nvlog("begin_render: colour %u: the pipeline's format %u draws into a format-%u attachment - pass REFUSED", i, cf[i], ps->colfmt[i] ? ps->colfmt[i] : img->fmt); return -1; }
    }
    if (p->has_depth && !ps->dsview) { nvlog("begin_render: pipeline has a depth attachment but the encoder has no depth texture — pass REFUSED"); return -1; }
    if (p->has_depth && ps->ds) {
        uint32_t ds_s = ps->ds->samples > 1 ? ps->ds->samples : 1u, p_s = p->samples > 1 ? p->samples : 1u;
        if (ds_s != p_s) { nvlog("begin_render: apps2 pipeline has %u samples but the depth target has %u — pass REFUSED", p_s, ds_s); return -1; }
        if (ps->ds->fmt != p->dfmt) { static int said; if (said++ < 4) nvlog("begin_render: apps2 depth: the pipeline's format %u on a format-%u texture (census)", p->dfmt, ps->ds->fmt); }
    }
    nvk_pass full;
    {
        uint32_t add = 0;
        const uint32_t nl = ps->layers > 1 ? ps->layers : 1u;
        for (uint32_t i = 0; i < ncol; i++) {
            nvk_image *img = ps->col[i];
            if (!cf[i] || !img || (ps->load & NVMTL_LOAD_COLOUR(i)) || (ps->colw[i] <= ps->w && ps->colh[i] <= ps->h)) continue;
            if (c->in_rp || img->mtl_type == 7 || (ps->colfmt[i] && ps->colfmt[i] != img->fmt)) {
                static int said; if (said++ < 4) nvlog("begin_render: fullclear colour %u (%ux%u) is larger than the %ux%u render area but is a %s — only the area is cleared",
                    i, ps->colw[i], ps->colh[i], ps->w, ps->h, c->in_rp ? "pass in progress" : img->mtl_type == 7 ? "3D slice" : "format view");
                continue;
            }
            VkImageSubresourceRange r = { VK_IMAGE_ASPECT_COLOR_BIT, ps->col_level[i], 1, ps->col_layer[i], nl };
            VkImageMemoryBarrier b0 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, r };
            pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
            VkClearColorValue cv; nvmtl_clear_colour(&cv, img->fmt, ps->clear[i]);
            pvkCmdClearColorImage(c->cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &r);
            VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, r };
            pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
            img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            add |= NVMTL_LOAD_COLOUR(i);
        }
        if (p->has_depth && ps->ds && !c->in_rp) {
            nvk_image *d = ps->ds;
            const uint32_t dw = (d->w >> ps->ds_level) ? (d->w >> ps->ds_level) : 1u, dh = (d->h >> ps->ds_level) ? (d->h >> ps->ds_level) : 1u;
            const int dl = (ps->load & NVMTL_LOAD_DEPTH) != 0, sl = p->has_stencil ? (ps->load & NVMTL_LOAD_STENCIL) != 0 : dl;
            VkImageAspectFlags all = nvmtl_copy_aspect(d), clr = 0;
            if (d->fmt == VK_FORMAT_D16_UNORM_S8_UINT || d->fmt == VK_FORMAT_D24_UNORM_S8_UINT || d->fmt == VK_FORMAT_D32_SFLOAT_S8_UINT)
                all |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (!dl && (all & VK_IMAGE_ASPECT_DEPTH_BIT)) clr |= VK_IMAGE_ASPECT_DEPTH_BIT;
            if (p->has_stencil && !sl && (all & VK_IMAGE_ASPECT_STENCIL_BIT)) clr |= VK_IMAGE_ASPECT_STENCIL_BIT;
            if (clr && (dw > ps->w || dh > ps->h)) {
                VkImageSubresourceRange r = { all, ps->ds_level, 1, ps->ds_layer, nl };
                VkImageMemoryBarrier b0 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                    clr == all ? VK_IMAGE_LAYOUT_UNDEFINED : (VkImageLayout)d->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)d->img, r };
                pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b0);
                VkClearDepthStencilValue dv = { ps->clear_depth, ps->clear_stencil };
                VkImageSubresourceRange rc = r; rc.aspectMask = clr;
                pvkCmdClearDepthStencilImage(c->cb, (VkImage)d->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &dv, 1, &rc);
                VkImageMemoryBarrier b1 = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
                    VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)d->img, r };
                pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                    VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
                d->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
                add |= ((clr & VK_IMAGE_ASPECT_DEPTH_BIT) ? NVMTL_LOAD_DEPTH : 0u) | ((clr & VK_IMAGE_ASPECT_STENCIL_BIT) ? NVMTL_LOAD_STENCIL : 0u);
                if (!p->has_stencil) add |= NVMTL_LOAD_STENCIL;
            }
        }
        if (add) {
            static int said; if (said++ < 8) nvlog("begin_render: fullclear %ux%u render area — Clear attachments 0x%x are larger and were cleared WHOLE, as Apple and AMD do",
                                                   ps->w, ps->h, add);
            full = *ps; full.load |= add; ps = &full;
        }
    }
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
        nvk_image *img = ps->col[i];
        if (!img || (i < ncol && cf[i]) || (ps->load & NVMTL_LOAD_COLOUR(i))) continue;
        if (ps->colview[i] == img->view && img->samples <= 1) { if (nvmtl_vk_cmd_clear_image(c, img, ps->clear[i])) nvlog("begin_render: apps2 colour %u clear FAILED", i); }
        else { static int said; if (said++ < 4) nvlog("begin_render: apps2 colour %u is not drawn by this pipeline and is a level/slice/multisample view — its Clear is NOT applied", i); }
    }
    if (p->has_depth && ps->ds && ps->ds->samples > 1 && g_sample_locations &&
        (ps->load & (NVMTL_LOAD_DEPTH | NVMTL_LOAD_STENCIL)))
        nvmtl_private_depth_layout(c, ps->ds, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL);
    VkImageView views[2 * NVMTL_NCOL + 2]; uint32_t nviews = 0, was = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; int first = 1;
    uint32_t prm = 0;
    for (uint32_t i = 0; p->samples > 1 && i < ncol; i++) if (cf[i] && (rmask & (1u << i)) && ps->res[i] && ps->resview[i]) prm |= 1u << i;
    const int dres = p->samples > 1 && p->has_depth && ps->dres && ps->dresview && (ps->dresmode || ps->sresmode);
    void *rpShape = NULL;
    if (p->samples > 1 && (prm != (rmask & ((1u << ncol) - 1u)) || dres)) {
        rpShape = nvmtl_rp_shape_variant(p, ps->load, prm, dres ? ps->dresmode : 0, dres ? ps->sresmode : 0);
        if (!rpShape) { nvlog("begin_render: par1 no resolve-shape render pass — pass REFUSED"); return -1; }
        static int said; if (said++ < 8) nvlog("begin_render: par1 %u-sample pass resolves colour 0x%x of 0x%x%s", p->samples, prm, rmask, dres ? " + depth/stencil" : "");
    }
    for (uint32_t i = 0; i < ncol; i++) {
        if (!cf[i]) continue;
        if (first) { was = ps->col[i]->layout; first = 0; }
        ps->col[i]->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        views[nviews++] = (VkImageView)ps->colview[i];
    }
    if (p->has_depth) { views[nviews++] = (VkImageView)ps->dsview; if (ps->ds) ps->ds->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; }
    for (uint32_t i = 0; p->samples > 1 && i < ncol; i++) {
        if (!cf[i] || !(prm & (1u << i))) continue;
        views[nviews++] = (VkImageView)ps->resview[i]; ps->res[i]->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    }
    if (dres) { views[nviews++] = (VkImageView)ps->dresview; if (ps->dres) ps->dres->layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL; }
    VkFramebufferCreateInfo fci = { VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO, NULL, 0, rpShape ? (VkRenderPass)rpShape : p->rp,
        nviews, views, ps->w, ps->h, ps->layers > 1 ? ps->layers : 1 };
    VkFramebuffer fb = VK_NULL_HANDLE;
    VKCK(pvkCreateFramebuffer(g_dev, &fci, NULL, &fb), "vkCreateFramebuffer");
    c->fb = fb;
    if (c->nfb == c->capfb) {
        unsigned cap = c->capfb ? c->capfb * 2 : 16;
        void **grown = (void **)realloc(c->fbs, cap * sizeof *grown);
        if (grown) { c->fbs = grown; c->capfb = cap; }
    }
    if (c->nfb < c->capfb) c->fbs[c->nfb++] = fb;
    else {

        pvkDestroyFramebuffer(g_dev, fb, NULL); c->fb = NULL;
        nvlog("vk: framebuffer retention allocation failed; pass refused");
        return -1;
    }
    VkClearValue cv[2 * NVMTL_NCOL + 1]; uint32_t ncv = 0; memset(cv, 0, sizeof cv);
    for (uint32_t i = 0; i < ncol; i++) if (cf[i]) { nvmtl_clear_colour(&cv[ncv].color, cf[i], ps->clear[i]); ncv++; }
    if (p->has_depth) { cv[ncv].depthStencil.depth = ps->clear_depth; cv[ncv].depthStencil.stencil = ps->clear_stencil; ncv++; }
    void *rpUse = rpShape ? rpShape : nvmtl_rp_variant(p, ps->load, was);
    if (!rpUse) rpUse = p->rp;
    VkRenderPassBeginInfo rbi = { VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO, NULL, (VkRenderPass)rpUse, c->fb,
        { { 0, 0 }, { ps->w, ps->h } }, ncv, cv };
    c->sample_pattern = pattern;
    VkSampleLocationEXT vkpos[8];
    VkSampleLocationsInfoEXT locations = nvmtl_sample_info(&pattern, vkpos);
    VkAttachmentSampleLocationsEXT attachment = { p->has_depth ? ncv - 1 : 0, locations };
    VkSubpassSampleLocationsEXT subpass = { 0, locations };
    VkRenderPassSampleLocationsBeginInfoEXT location_begin = {
        VK_STRUCTURE_TYPE_RENDER_PASS_SAMPLE_LOCATIONS_BEGIN_INFO_EXT, NULL,
        p->has_depth ? 1u : 0u, p->has_depth ? &attachment : NULL,
        p->has_depth ? 1u : 0u, p->has_depth ? &subpass : NULL };
    if (g_sample_locations && pattern.count > 1) {

        rbi.pNext = &location_begin;
        pvkCmdSetSampleLocationsEXT(c->cb, &locations);
    }
    { extern int nvmtl_pt_armed(void);
      if (nvmtl_pt_armed() && c->pt_n < 128 && g_timestamp_bits) {
          if (!c->pt_pool) { VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 256, 0 };
              VkQueryPool qp; if (pvkCreateQueryPool(g_dev, &qi, NULL, &qp) == VK_SUCCESS) { pvkCmdResetQueryPool(c->cb, qp, 0, 256); c->pt_pool = (void *)qp; } }
          if (c->pt_pool) { pvkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkQueryPool)c->pt_pool, 2 * c->pt_n);
              c->pt_w[c->pt_n] = ps->w; c->pt_h[c->pt_n] = ps->h; c->pt_f[c->pt_n] = ncol ? cf[0] : (p->has_depth ? 1000u + p->dfmt : 0u); c->pt_open = 1;
              memset(c->pt_fs[c->pt_n], 0, sizeof c->pt_fs[c->pt_n]); }
      } }
    pvkCmdBeginRenderPass(c->cb, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    c->in_rp = 1;
    nvmtl_gpt_pass(c->cb, ps->w, ps->h, ncol ? cf[0] : 0u, ncol, p->has_depth ? p->dfmt : 0u, p->samples > 1 ? p->samples : 1u, ps->load);
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) c->fetchview[i] = (i < ncol && cf[i]) ? ps->colview[i] : NULL;
    if (p->pipe) nvmtl_vk_cmd_bind_pipeline(c, p);
    VkViewport vp = { 0, (float)ps->h, (float)ps->w, -(float)ps->h, 0, 1 }; VkRect2D sc = { { 0, 0 }, { ps->w, ps->h } };
    pvkCmdSetViewport(c->cb, 0, 1, &vp); pvkCmdSetScissor(c->cb, 0, 1, &sc);
    return 0;
}
int nvmtl_vk_cmd_begin_render_ms(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4], uint32_t load, nvk_image *resolve)
{
    nvk_pass ps; memset(&ps, 0, sizeof ps);
    ps.col[0] = img; ps.colview[0] = img->view; ps.colw[0] = img->w; ps.colh[0] = img->h;
    if (resolve) { ps.res[0] = resolve; ps.resview[0] = resolve->view; ps.resw[0] = resolve->w; ps.resh[0] = resolve->h; }
    memcpy(ps.clear[0], clear, 4 * sizeof(float));
    ps.dsview = c->depth_view; ps.clear_depth = 1.0f;
    ps.load = load | ((load & NVMTL_LOAD_DEPTH) ? NVMTL_LOAD_STENCIL : 0u);
    ps.w = img->w; ps.h = img->h;
    return nvmtl_vk_cmd_begin_pass(c, &ps, p);
}

int nvmtl_vk_cmd_retire_view(nvk_cmdbuf *c, void *view)
{
    if (c->nrv == c->caprv) {
        unsigned cap = c->caprv ? c->caprv * 2 : 8;
        void **grown = (void **)realloc(c->rvs, cap * sizeof *grown);
        if (grown) { c->rvs = grown; c->caprv = cap; }
    }
    if (c->nrv < c->caprv) { c->rvs[c->nrv++] = view; return 0; }
    nvlog("vk: apps2 attachment-view list realloc FAILED — view %p destroyed now and its pass REFUSED", view);
    pvkDestroyImageView(g_dev, (VkImageView)view, NULL);
    return -1;
}
int nvmtl_vk_cmd_attachment_view(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, void **out, uint32_t *w, uint32_t *h)
{
    const uint32_t mips = img->mips ? img->mips : 1, layers = img->layers ? img->layers : 1;
    *out = NULL;
    if (level >= mips || slice >= layers) { nvlog("encoder: apps2 attachment level %u slice %u is outside the image (%u level(s), %u slice(s)) — pass REFUSED", level, slice, mips, layers); return -1; }
    *w = img->w >> level ? img->w >> level : 1u; *h = img->h >> level ? img->h >> level : 1u;
    if ((mips <= 1 && layers <= 1) || ((depth || img->mtl_type == 7) && !level && !slice)) {
        if (!img->view) { nvlog("encoder: apps2 attachment image has no view — pass REFUSED"); return -1; }
        if (mips > 1 || layers > 1 || img->mtl_type == 7) { static int said; if (said++ < 4) nvlog("encoder: apps2 a %s attachment at level 0 slice 0 of a %u-level %u-slice image uses the image's own view (as before)", depth ? "depth/stencil" : "3D", mips, layers); }
        *out = img->view; return 0;
    }
    if (depth) { nvlog("encoder: apps2 a depth/stencil attachment at level %u slice %u is not carried — pass REFUSED", level, slice); return -1; }
    if (img->mtl_type == 7 && !(img->vkflags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT)) {
        nvlog("encoder: apps2 a 3D attachment at level %u slice %u needs 2D_ARRAY_COMPATIBLE, which this image did not get — pass REFUSED", level, slice);
        return -1; }
    if (img->mtl_type == 7 && mips > 1) {
        nvlog("encoder: apps2 a 3D attachment at level %u slice %u of a %u-level volume is not carried (a mip's depth is not `layers`) — pass REFUSED", level, slice, mips);
        return -1; }
    uint32_t vt = img->mtl_type <= 1 ? 0u : img->mtl_type == 4 ? 4u : 2u;
    void *v = NULL;
    if (nvmtl_vk_image_view_create_range(img, img->fmt, 0, level, 1, slice, 1, vt, &v)) return -1;
    if (nvmtl_vk_cmd_retire_view(c, v)) return -1;
    *out = v; return 0;
}
int nvmtl_vk_cmd_attachment_view_fmt(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, uint32_t vkfmt, int a8,
                                     void **out, uint32_t *w, uint32_t *h)
{
    if (depth || !vkfmt || vkfmt == img->fmt) return nvmtl_vk_cmd_attachment_view(c, img, depth, level, slice, out, w, h);
    const uint32_t mips = img->mips ? img->mips : 1, layers = img->layers ? img->layers : 1;
    *out = NULL;
    if (level >= mips || slice >= layers) { nvlog("encoder: gl3 format-view attachment level %u slice %u is outside the image (%u level(s), %u slice(s)) — pass REFUSED", level, slice, mips, layers); return -1; }
    if (img->mtl_type == 7) { nvlog("encoder: gl3 a format view of a 3D attachment is not carried — pass REFUSED"); return -1; }
    *w = img->w >> level ? img->w >> level : 1u; *h = img->h >> level ? img->h >> level : 1u;
    uint32_t vt = img->mtl_type <= 1 ? 0u : img->mtl_type == 4 ? 4u : 2u;
    void *v = NULL;
    if (nvmtl_vk_image_view_create_range(img, vkfmt, a8, level, 1, slice, 1, vt, &v)) return -1;
    if (nvmtl_vk_cmd_retire_view(c, v)) return -1;
    static int said; if (said++ < 4) nvlog("encoder: gl3 colour attachment in its VIEW's format %u over a format-%u image", vkfmt, img->fmt);
    *out = v; return 0;
}
void nvmtl_vk_pipeline_sig(const nvk_pipeline *p, nvk_rtsig *out)
{
    uint32_t rmask; memset(out, 0, sizeof *out);
    out->ncol = nvmtl_rt_shape(p, out->cfmts, &rmask);
    while (out->ncol && !out->cfmts[out->ncol - 1]) out->ncol--;
    out->has_depth = p->has_depth ? 1u : 0u; out->dfmt = p->has_depth ? p->dfmt : 0u; out->samples = p->samples > 1 ? p->samples : 1u;
}
int nvmtl_vk_pipeline_compatible(const nvk_rtsig *sig, const nvk_pipeline *p)
{ nvk_rtsig s; nvmtl_vk_pipeline_sig(p, &s); return !memcmp(&s, sig, sizeof s); }

static int nvmtl_fmt_stencil(uint32_t f) { return f >= VK_FORMAT_S8_UINT && f <= VK_FORMAT_D32_SFLOAT_S8_UINT; }
int nvmtl_vk_cmd_attachment_view_n(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, uint32_t nlayers,
                                   uint32_t vkfmt, int a8, void **out, uint32_t *w, uint32_t *h)
{
    if (nlayers <= 1 && !(depth && (level || slice || img->layers > 1 || img->mips > 1)))
        return nvmtl_vk_cmd_attachment_view_fmt(c, img, depth, level, slice, vkfmt, a8, out, w, h);
    if (!nlayers) nlayers = 1;
    const uint32_t mips = img->mips ? img->mips : 1, layers = img->layers ? img->layers : 1;
    *out = NULL;
    if (level >= mips || slice + nlayers > layers) {
        nvlog("encoder: par1 attachment level %u slices [%u,%u) is outside the image (%u level(s), %u slice(s)) — pass REFUSED", level, slice, slice + nlayers, mips, layers);
        return -1; }
    // A layered colour attachment over a 3D texture's depth planes (Metal renderTargetArrayLength > 1 on a 3D texture; Blender
    // Eevee's volume passes) is a 2D-array view of the 3D image's level 0, which Vulkan allows for an image created
    // 2D_ARRAY_COMPATIBLE (all our 3D colour images, nvmtl_vk_image_create). Other levels have fewer planes than `layers`.
    const int vol_layers = img->mtl_type == 7 && !depth && !level && (img->vkflags & VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT);
    if (img->mtl_type == 7 && !vol_layers) { nvlog("encoder: par1 a %s 3D attachment is not carried — pass REFUSED", depth ? "depth" : level ? "non-zero-level layered" : "layered (not 2D_ARRAY_COMPATIBLE)"); return -1; }
    *w = img->w >> level ? img->w >> level : 1u; *h = img->h >> level ? img->h >> level : 1u;
    void *v = NULL;
    if (depth) {
        VkImageAspectFlags asp = img->fmt == VK_FORMAT_S8_UINT ? VK_IMAGE_ASPECT_STENCIL_BIT
                               : (VkImageAspectFlags)(VK_IMAGE_ASPECT_DEPTH_BIT | (nvmtl_fmt_stencil(img->fmt) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0));
        VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img->img,
            nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D, (VkFormat)img->fmt, { 0 }, { asp, level, 1, slice, nlayers } };
        VkImageView vv = VK_NULL_HANDLE;
        VkResult r = pvkCreateImageView(g_dev, &vci, NULL, &vv);
        if (r != VK_SUCCESS) { nvlog("encoder: par1 depth attachment view level %u slices [%u,%u) -> %d — pass REFUSED", level, slice, slice + nlayers, r); return -1; }
        v = (void *)vv;
    } else {
        uint32_t vt = nlayers > 1 ? (img->mtl_type <= 1 ? 1u : 3u) : (img->mtl_type <= 1 ? 0u : img->mtl_type == 4 ? 4u : 2u);
        if (nvmtl_vk_image_view_create_range(img, vkfmt ? vkfmt : img->fmt, a8, level, 1, slice, nlayers, vt, &v)) return -1;
    }
    if (nvmtl_vk_cmd_retire_view(c, v)) return -1;
    *out = v; return 0;
}
static VkResolveModeFlagBits nvmtl_resolve_mode(uint32_t mtlp1, int stencil)
{
    if (!mtlp1) return VK_RESOLVE_MODE_NONE;
    uint32_t f = mtlp1 - 1;
    VkResolveModeFlagBits m = stencil ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : f == 1 ? VK_RESOLVE_MODE_MIN_BIT : f == 2 ? VK_RESOLVE_MODE_MAX_BIT : VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    if (stencil && f == 1) { static int said; if (!said++) nvlog("encoder: par1 MTLMultisampleStencilResolveFilterDepthResolvedSample has no Vulkan mode — stencil resolves sample 0"); }
    uint32_t have = stencil ? g_dsr_stencil_modes : g_dsr_depth_modes;
    if (!(have & m)) { static int said; if (said++ < 4) nvlog("encoder: par1 %s resolve mode 0x%x is not offered by this device (0x%x) — sample 0", stencil ? "stencil" : "depth", m, have); m = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; }
    return m;
}
static VkResult nvmtl_make_render_pass_shape(const nvk_pipeline *p, uint32_t load, uint32_t prm, uint32_t dmode, uint32_t smode, VkRenderPass *out)
{
    if (!pvkCreateRenderPass2) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkSampleCountFlagBits sc = (VkSampleCountFlagBits)(p->samples > 1 ? p->samples : 1);
    int ms = p->samples > 1, anyres = 0;
    uint32_t cf[NVMTL_NCOL], rmask, ncol = nvmtl_rt_shape(p, cf, &rmask);
    VkAttachmentDescription2 atts[2 * NVMTL_NCOL + 2]; uint32_t n = 0;
    VkAttachmentReference2 cref[NVMTL_NCOL], rref[NVMTL_NCOL];
    VkAttachmentReference2 dref = { VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 0 };
    VkAttachmentReference2 drref = dref;
    for (uint32_t i = 0; i < ncol; i++) {
        cref[i] = rref[i] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, VK_ATTACHMENT_UNUSED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0 };
        if (!cf[i]) continue;
        int ld = (load & NVMTL_LOAD_COLOUR(i)) != 0;
        cref[i].attachment = n;
        atts[n++] = (VkAttachmentDescription2){ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)cf[i], sc,
            ld ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            ld ? VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    }
    if (p->has_depth) {
        int dl = (load & NVMTL_LOAD_DEPTH) != 0, sl = p->has_stencil ? (load & NVMTL_LOAD_STENCIL) != 0 : dl;
        dref.attachment = n;
        atts[n++] = (VkAttachmentDescription2){ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)(p->dfmt ? p->dfmt : VK_FORMAT_D32_SFLOAT), sc,
            dl ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_STORE,
            sl ? VK_ATTACHMENT_LOAD_OP_LOAD : p->has_stencil ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            (sl || p->has_stencil) ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            (dl || sl) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }
    for (uint32_t i = 0; ms && i < ncol; i++) {
        if (!cf[i] || !(prm & (1u << i))) continue;
        rref[i].attachment = n; anyres = 1;
        atts[n++] = (VkAttachmentDescription2){ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)cf[i], VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_STORE, VK_ATTACHMENT_LOAD_OP_DONT_CARE, VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL };
    }
    VkResolveModeFlagBits dm = VK_RESOLVE_MODE_NONE, sm = VK_RESOLVE_MODE_NONE;
    const uint32_t df = p->dfmt ? p->dfmt : VK_FORMAT_D32_SFLOAT;
    if (ms && p->has_depth && (dmode || smode)) {
        dm = df == VK_FORMAT_S8_UINT ? VK_RESOLVE_MODE_NONE : nvmtl_resolve_mode(dmode, 0);
        sm = p->has_stencil ? nvmtl_resolve_mode(smode, 1) : VK_RESOLVE_MODE_NONE;
        if (dm && sm && dm != sm && !g_dsr_indep) dm = sm = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        if (p->has_stencil && df != VK_FORMAT_S8_UINT && (!dm || !sm) && !g_dsr_indep_none) { if (!dm) dm = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; if (!sm) sm = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT; }
        drref.attachment = n;
        atts[n++] = (VkAttachmentDescription2){ VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2, NULL, 0, (VkFormat)df, VK_SAMPLE_COUNT_1_BIT,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, dm ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_ATTACHMENT_LOAD_OP_DONT_CARE, sm ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
    }
    VkSubpassDescriptionDepthStencilResolve dsr = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE, NULL, dm, sm, &drref };
    VkAttachmentReference2 iref2[NVMTL_NCOL];
    const int fetch = nvmtl_fbfetch_on() && ncol;
    for (uint32_t i = 0; fetch && i < ncol; i++) {
        if (cref[i].attachment != VK_ATTACHMENT_UNUSED) cref[i].layout = VK_IMAGE_LAYOUT_GENERAL;
        iref2[i] = (VkAttachmentReference2){ VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2, NULL, cref[i].attachment, VK_IMAGE_LAYOUT_GENERAL,
                                             cref[i].attachment != VK_ATTACHMENT_UNUSED ? VK_IMAGE_ASPECT_COLOR_BIT : 0 };
    }
    VkSubpassDescription2 sp = { VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2, drref.attachment != VK_ATTACHMENT_UNUSED ? (const void *)&dsr : NULL, 0,
        VK_PIPELINE_BIND_POINT_GRAPHICS, 0, fetch ? ncol : 0, fetch ? iref2 : NULL, ncol, ncol ? cref : NULL, anyres ? rref : NULL, p->has_depth ? &dref : NULL, 0, NULL };
    const VkSubpassDependency2 deps2[3] = {
        { VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, VK_SUBPASS_EXTERNAL, 0, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0, 0 },
        { VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, VK_SUBPASS_EXTERNAL, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
          VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, 0, 0 },
        { VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2, NULL, 0, 0, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
          VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_INPUT_ATTACHMENT_READ_BIT, VK_DEPENDENCY_BY_REGION_BIT, 0 } };
    VkRenderPassCreateInfo2 rci = { VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2, NULL, 0, n, atts, 1, &sp, fetch ? 3u : 2u, deps2, 0, NULL };
    return pvkCreateRenderPass2(g_dev, &rci, NULL, out);
}
typedef struct { uint64_t identity; uint32_t load, prm, dmode, smode; void *rp; } nvmtl_shape_rp;
static uint64_t g_shape_identity;
static nvmtl_shape_rp *g_srp; static uint32_t g_nsrp, g_csrp;
#define NVMTL_SRP_MAX 65536
static void *nvmtl_rp_shape_variant(nvk_pipeline *p, uint32_t load, uint32_t prm, uint32_t dmode, uint32_t smode)
{
    uint32_t cf[NVMTL_NCOL], rmask, ncol = nvmtl_rt_shape(p, cf, &rmask), keep = 0;
    for (uint32_t i = 0; i < ncol; i++) if (cf[i]) keep |= NVMTL_LOAD_COLOUR(i);
    if (p->has_depth) keep |= NVMTL_LOAD_DEPTH | (p->has_stencil ? NVMTL_LOAD_STENCIL : 0u);
    load &= keep;
    void *rp = NULL;
    pthread_mutex_lock(&g_rpv_lock);
    if (!p->shape_identity) {
        if (g_shape_identity == UINT64_MAX) { pthread_mutex_unlock(&g_rpv_lock); return NULL; }
        p->shape_identity = ++g_shape_identity;
    }
    for (uint32_t i = 0; i < g_nsrp; i++)
        if (g_srp[i].identity == p->shape_identity && g_srp[i].load == load && g_srp[i].prm == prm && g_srp[i].dmode == dmode && g_srp[i].smode == smode) { rp = g_srp[i].rp; break; }
    if (!rp && g_nsrp == g_csrp && g_csrp < NVMTL_SRP_MAX) {
        uint32_t cap = g_csrp ? g_csrp * 2u : 16u;
        nvmtl_shape_rp *gx = (nvmtl_shape_rp *)realloc(g_srp, cap * sizeof *gx);
        if (gx) { g_srp = gx; g_csrp = cap; }
    }
    if (!rp && g_nsrp < g_csrp) {
        VkRenderPass made = VK_NULL_HANDLE;
        VkResult r = nvmtl_make_render_pass_shape(p, load, prm, dmode, smode, &made);
        if (r == VK_SUCCESS) {
            g_srp[g_nsrp++] = (nvmtl_shape_rp){ p->shape_identity, load, prm, dmode, smode, (void *)made }; rp = (void *)made;
            nvlog("vk: par1 resolve-shape render pass made (%u colour slot(s), %u samples, resolves colour 0x%x of 0x%x, depth filter %d, stencil filter %d, load 0x%x)",
                  ncol, p->samples, prm, rmask, (int)dmode - 1, (int)smode - 1, load);
        } else nvlog("vk: par1 resolve-shape vkCreateRenderPass2 -> %d (%s)", r, pvkCreateRenderPass2 ? "the device refused it" : "vkCreateRenderPass2 not loaded");
    } else if (!rp) nvlog("vk: par1 resolve-shape list full (%u) — this pass is REFUSED", g_nsrp);
    pthread_mutex_unlock(&g_rpv_lock);
    return rp;
}
int nvmtl_vk_image_view_create_range_swz(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount, uint32_t baseLayer,
                                         uint32_t layerCount, uint32_t mtl_view_type, const uint8_t mtl_swz[4], void **out_view)
{
    void *v = NULL;
    if (nvmtl_vk_image_view_create_range(img, vkfmt, a8, baseLevel, levelCount, baseLayer, layerCount, mtl_view_type, &v)) return -1;
    pvkDestroyImageView(g_dev, (VkImageView)v, NULL);
    static const VkComponentSwizzle a8map[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_ZERO,
                                                 VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_R };
    static const VkComponentSwizzle argbmap[6] = { VK_COMPONENT_SWIZZLE_ZERO, VK_COMPONENT_SWIZZLE_ONE, VK_COMPONENT_SWIZZLE_G,
                                                   VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A, VK_COMPONENT_SWIZZLE_R };   // a8 == 2: ARGB8
    VkComponentSwizzle m[4];
    for (int i = 0; i < 4; i++) {
        uint8_t s = mtl_swz[i] <= 5 ? mtl_swz[i] : (uint8_t)(2 + i);
        m[i] = a8 == 1 ? a8map[s] : a8 == 2 ? argbmap[s] : (VkComponentSwizzle)(s + 1);
    }
    const uint32_t mips = img->mips ? img->mips : 1, layers = img->layers ? img->layers : 1;
    if (!levelCount) levelCount = mips - baseLevel;
    if (!layerCount) layerCount = layers - baseLayer;
    VkImageViewType vt = mtl_view_type == 0 ? VK_IMAGE_VIEW_TYPE_1D : mtl_view_type == 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : (mtl_view_type == 3 || mtl_view_type == 8) ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                       : mtl_view_type == 5 ? VK_IMAGE_VIEW_TYPE_CUBE : mtl_view_type == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : mtl_view_type == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    if (mtl_view_type == 7) layerCount = 1;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img->img, vt, (VkFormat)vkfmt, { m[0], m[1], m[2], m[3] },
        { VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levelCount, baseLayer, layerCount } };
    VkImageViewUsageCreateInfo uci;
    int vg = nvmtl_vk_view_guard(img, vkfmt, a8, vci.viewType, vci.subresourceRange.levelCount, &uci, "image_view_create(swizzle)");
    if (vg < 0) return -1;
    if (vg > 0) {
        uci.usage &= ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT; vci.pNext = &uci; }
    else { uci = (VkImageViewUsageCreateInfo){ VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
               (VkImageUsageFlags)(img->usage ? img->usage : (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT)) & ~(VkImageUsageFlags)VK_IMAGE_USAGE_STORAGE_BIT };
           if (uci.usage) vci.pNext = &uci; }
    VkImageView vv = VK_NULL_HANDLE;
    VKCK(pvkCreateImageView(g_dev, &vci, NULL, &vv), "vkCreateImageView(swizzle)");
    *out_view = vv;
    return 0;
}
int nvmtl_vk_pass_kind(const nvk_pass *ps)
{
    int legacy = ps->col[0] && ps->colview[0] == ps->col[0]->view && !ps->res[0] && !ps->ds && !ps->dsview;
    int allLoad = 1, anyRes = 0, any = 0;
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
        if (!ps->col[i]) continue;
        if (i) legacy = 0;
        any = 1;
        if (!(ps->load & NVMTL_LOAD_COLOUR(i))) allLoad = 0;
        if (ps->res[i]) anyRes = 1;
    }
    if (ps->ds) {
        any = 1;
        if (!(ps->load & NVMTL_LOAD_DEPTH) || (nvmtl_fmt_stencil(ps->ds->fmt) && !(ps->load & NVMTL_LOAD_STENCIL))) allLoad = 0;
    }
    if (ps->dres || ps->layers > 1) { legacy = 0; anyRes |= ps->dres != NULL; }
    if (legacy) return 0;
    if (!any || (allLoad && !anyRes)) return 1;
    return 2;
}
#define NVMTL_NSHAPE 32
static struct { nvk_rtsig k; uint32_t rmask; nvk_pipeline p; } g_shapes[NVMTL_NSHAPE]; static uint32_t g_nshapes;
int nvmtl_vk_cmd_empty_pass(nvk_cmdbuf *c, const nvk_pass *ps)
{
    nvk_rtsig k; memset(&k, 0, sizeof k); uint32_t rmask = 0, samples = 0;
    for (uint32_t i = 0; i < NVMTL_NCOL; i++) {
        if (!ps->col[i]) continue;
        uint32_t s = ps->col[i]->samples > 1 ? ps->col[i]->samples : 1u;
        if (samples && s != samples) { nvlog("encoder: apps2 a pass with no draws mixes %u- and %u-sample attachments — REFUSED", samples, s); return -1; }
        uint32_t vf = ps->colfmt[i] ? ps->colfmt[i] : ps->col[i]->fmt;
        samples = s; k.cfmts[i] = vf; k.ncol = i + 1;
        if (ps->res[i]) { uint32_t rf = ps->resfmt[i] ? ps->resfmt[i] : ps->res[i]->fmt;
            if (s > 1 && rf != vf) { nvlog("encoder: emptyfmt a pass with no draws resolves format %u into format %u — REFUSED (Vulkan and Metal both require one format)", vf, rf); return -1; }
            rmask |= 1u << i; }
    }
    if (ps->ds) {
        uint32_t s = ps->ds->samples > 1 ? ps->ds->samples : 1u;
        if (samples && s != samples) { nvlog("encoder: apps2 a pass with no draws mixes %u- and %u-sample attachments — REFUSED", samples, s); return -1; }
        samples = s; k.has_depth = 1; k.dfmt = ps->ds->fmt;
    }
    k.samples = samples ? samples : 1u;
    if (k.samples == 1) rmask = 0;
    pthread_mutex_lock(&g_rpv_lock);
    nvk_pipeline *p = NULL;
    for (uint32_t i = 0; i < g_nshapes; i++) if (!memcmp(&g_shapes[i].k, &k, sizeof k) && g_shapes[i].rmask == rmask) { p = &g_shapes[i].p; break; }
    if (!p && g_nshapes < NVMTL_NSHAPE) {
        nvk_pipeline *q = &g_shapes[g_nshapes].p; memset(q, 0, sizeof *q);
        q->ncol = k.ncol; memcpy(q->cfmts, k.cfmts, sizeof q->cfmts); q->cfmt = k.cfmts[0];
        q->has_depth = (int)k.has_depth; q->dfmt = k.dfmt; q->has_stencil = (uint32_t)nvmtl_fmt_stencil(k.dfmt); q->samples = k.samples; q->rmask = rmask;
        VkRenderPass made = VK_NULL_HANDLE; VkResult r = nvmtl_make_render_pass(q, 0, &made);
        if (r == VK_SUCCESS) { q->rp = (void *)made; g_shapes[g_nshapes].k = k; g_shapes[g_nshapes].rmask = rmask; g_nshapes++; p = q; }
        else nvlog("encoder: apps2 empty-pass vkCreateRenderPass -> %d", r);
    }
    pthread_mutex_unlock(&g_rpv_lock);
    if (!p) { static int said; if (!said++) nvlog("encoder: apps2 no empty pass for this shape (%u kept) — its clears and resolves are NOT applied", g_nshapes); return -1; }
    if (nvmtl_vk_cmd_begin_pass(c, ps, p)) return -1;
    nvmtl_vk_cmd_end_render(c);
    return 0;
}
int nvmtl_vk_cmd_begin_render(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4])
{ return nvmtl_vk_cmd_begin_render_ex(c, img, p, clear, 0); }
void nvmtl_vk_cmd_set_viewport(nvk_cmdbuf *c, float x, float y, float w, float h, float zn, float zf)
{
    /* Vulkan requires minDepth/maxDepth in [0,1] and NVK asserts it (nvk_emit_viewport "0.0 <= zmin && zmin <= 1.0");
       Apple GPUs take any value. 1.10 crash reports (10-11, RTX 4060): mediaanalysisd's Live Text draw aborted there on
       every run. The !(>=) form also sends NaN to Metal's defaults, 0 and 1. */
    if (!(zn >= 0.0f)) zn = 0.0f; else if (zn > 1.0f) zn = 1.0f;
    if (!(zf >= 0.0f)) zf = isnan(zf) ? 1.0f : 0.0f; else if (zf > 1.0f) zf = 1.0f;
    VkViewport vp = { x, y + h, w, -h, zn, zf }; pvkCmdSetViewport(c->cb, 0, 1, &vp);
}
static void nvmtl_vk_zero_dummies(void)
{
    static int done;
    if (done) return;
    done = 1;
    nvk_image *imgs[3] = { &g_dummyImg, g_dummyStex.view ? &g_dummyStex : NULL, g_dummyStexArray.view ? &g_dummyStexArray : NULL };
    VkCommandPool pool = VK_NULL_HANDLE; VkCommandBuffer cb = VK_NULL_HANDLE; VkFence fence = VK_NULL_HANDLE;
    VkCommandPoolCreateInfo cpci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, g_qfam };
    if (pvkCreateCommandPool(g_dev, &cpci, NULL, &pool) != VK_SUCCESS) { nvlog("vk: dummy zero-fill SKIPPED — vkCreateCommandPool failed; an unbound texture still samples uninitialised memory"); return; }
    VkCommandBufferAllocateInfo cbai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    if (pvkAllocateCommandBuffers(g_dev, &cbai, &cb) != VK_SUCCESS) { nvlog("vk: dummy zero-fill SKIPPED — vkAllocateCommandBuffers failed; an unbound texture still samples uninitialised memory"); return; }
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    if (pvkBeginCommandBuffer(cb, &bi) != VK_SUCCESS) { nvlog("vk: dummy zero-fill SKIPPED — vkBeginCommandBuffer failed; an unbound texture still samples uninitialised memory"); return; }
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkClearColorValue zero; memset(&zero, 0, sizeof zero);
    for (int k = 0; k < 3; k++) {
        if (!imgs[k] || !imgs[k]->img) continue;
        VkImageLayout fin = (imgs[k] == &g_dummyStex || imgs[k] == &g_dummyStexArray) ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
            0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)imgs[k]->img, range };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
        pvkCmdClearColorImage(cb, (VkImage)imgs[k]->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
        VkImageMemoryBarrier toFin = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, fin,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)imgs[k]->img, range };
        pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &toFin);
        imgs[k]->layout = (uint32_t)fin;
    }
    if (pvkEndCommandBuffer(cb) != VK_SUCCESS) { nvlog("vk: dummy zero-fill SKIPPED — vkEndCommandBuffer failed; an unbound texture still samples uninitialised memory"); return; }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0 };
    if (pvkCreateFence(g_dev, &fci, NULL, &fence) != VK_SUCCESS) { nvlog("vk: dummy zero-fill SKIPPED — vkCreateFence failed; an unbound texture still samples uninitialised memory"); return; }
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cb, 0, NULL };
    VkResult sr = pvkQueueSubmit(g_queue, 1, &si, fence);
    if (sr != VK_SUCCESS) { nvlog("vk: dummy zero-fill FAILED — vkQueueSubmit -> %d; an unbound texture still samples uninitialised memory", sr); pvkDestroyFence(g_dev, fence, NULL); return; }
    VkResult wr = pvkWaitForFences(g_dev, 1, &fence, VK_TRUE, 2000000000ull);
    pvkDestroyFence(g_dev, fence, NULL);
    if (wr != VK_SUCCESS) { nvlog("vk: dummy zero-fill did not complete — vkWaitForFences -> %d; an unbound texture may still sample uninitialised memory", wr); return; }
    nvlog("vk: dummy images ZEROED and transitioned (sampled -> SHADER_READ_ONLY_OPTIMAL%s) — a declared-but-unbound texture now reads opaque black, not uninitialised VRAM",
          g_dummyStex.view ? ", storage -> GENERAL" : "; no storage dummy");
}

int nvmtl_vk_cmd_clear_image(nvk_cmdbuf *c, nvk_image *img, const float clear[4])
{
    VkImageSubresourceRange range = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &toDst);
    VkClearColorValue cv; nvmtl_clear_colour(&cv, img->fmt, clear);
    pvkCmdClearColorImage(c->cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &cv, 1, &range);
    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                          0, 0, NULL, 0, NULL, 1, &toSrc);
    img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    return 0;
}

/*
 * Stencil export ([[stencil]] fragment output) on hardware without it. NVIDIA GPUs cannot export stencil from a fragment
 * shader (NVIDIA's own drivers expose neither GL_ARB_ nor VK_EXT_shader_stencil_export), but Metal guarantees it and
 * apps rely on it: Blender's Metal backend hard-codes it, and Eevee classifies its G-buffer with it, so every lit
 * surface rendered black (#152). The translator turns the export into "discard unless (value & bit)", the bit pushed at
 * offset 160; a draw is then replayed the way Blender itself does where export is missing: once with reference 0 (which
 * clears the write-masked bits, and writes colour and depth as the draw would), then once per write-mask bit, with only
 * that bit writable, that bit as the reference, and depth writes off.
 */
#define NVMTL_SEMU_DRAW(c, DRAW) do { \
    if ((c)->skip_group) break; \
    if (!(c)->semu_layout) { DRAW; break; } \
    uint32_t z_ = 0; const uint32_t m_ = (c)->st_wmask[0] | (c)->st_wmask[1]; \
    pvkCmdPushConstants((c)->cb, (VkPipelineLayout)(c)->semu_layout, VK_SHADER_STAGE_FRAGMENT_BIT, NVMTL_STENCIL_EMU_OFFSET, 4, &z_); \
    pvkCmdSetStencilReference((c)->cb, VK_STENCIL_FACE_FRONT_AND_BACK, 0); \
    DRAW; \
    if (m_) { pvkCmdSetDepthWriteEnable((c)->cb, VK_FALSE); \
        for (uint32_t b_ = 1; b_ <= 0x80u; b_ <<= 1) { if (!(m_ & b_)) continue; \
            pvkCmdPushConstants((c)->cb, (VkPipelineLayout)(c)->semu_layout, VK_SHADER_STAGE_FRAGMENT_BIT, NVMTL_STENCIL_EMU_OFFSET, 4, &b_); \
            pvkCmdSetStencilWriteMask((c)->cb, VK_STENCIL_FACE_FRONT_BIT, (c)->st_wmask[0] & b_); \
            pvkCmdSetStencilWriteMask((c)->cb, VK_STENCIL_FACE_BACK_BIT, (c)->st_wmask[1] & b_); \
            pvkCmdSetStencilReference((c)->cb, VK_STENCIL_FACE_FRONT_AND_BACK, b_); \
            DRAW; } \
        pvkCmdSetStencilWriteMask((c)->cb, VK_STENCIL_FACE_FRONT_BIT, (c)->st_wmask[0]); \
        pvkCmdSetStencilWriteMask((c)->cb, VK_STENCIL_FACE_BACK_BIT, (c)->st_wmask[1]); \
        pvkCmdSetDepthWriteEnable((c)->cb, (c)->depth_write ? VK_TRUE : VK_FALSE); } \
    pvkCmdSetStencilReference((c)->cb, VK_STENCIL_FACE_FRONT_BIT, (c)->st_ref[0]); \
    pvkCmdSetStencilReference((c)->cb, VK_STENCIL_FACE_BACK_BIT, (c)->st_ref[1]); \
} while (0)
/*
 * Metal orders [[color(n)]] reads per pixel between the primitives of one draw: a fragment reads what every earlier
 * triangle of the same draw wrote there. An input attachment read here is not ordered within a draw, and NVK has neither
 * rasterization-order attachment access nor fragment shader interlock. Core Animation draws a text cell's background, its
 * glyph and the cursor in one draw, so a later triangle could read the pixel before the erase landed and put the old
 * cursor's edge back (a one-pixel line left in Terminal). Small triangle-list draws whose pipeline reads the attachments
 * are drawn one triangle (and instance) at a time behind the same by-region barrier used between draws. Draws over
 * NVMTL_FETCH_SPLIT_MAX triangles (default 256; a game's full-screen blend) stay whole: splitting them would cost far more
 * than the ordering is worth there. NVMTL_FETCH_UNORDERED=1 draws every one whole.
 */
static int nvmtl_fetch_split(nvk_cmdbuf *c, uint32_t count, uint32_t instances)
{
    static int unordered = -1, maxTris = -1;
    if (unordered < 0) { const char *e = getenv("NVMTL_FETCH_UNORDERED"); unordered = e && e[0] == '1'; }
    if (maxTris < 0) { const char *e = getenv("NVMTL_FETCH_SPLIT_MAX"); maxTris = e ? atoi(e) : 256; if (maxTris < 0) maxTris = 0; }
    if (!c->fetch_draw || unordered) return 0;
    if (c->topo != 3) {
        static int said; if (said++ < 2) nvlog("vk: framebuffer fetch draw of MTLPrimitiveType %u is drawn whole - "
                                               "overlapping primitives in it can read stale colour", c->topo);
        return 0;
    }
    const uint64_t tris = (uint64_t)(count / 3) * instances;
    if (tris > (uint64_t)maxTris) {
        static int said2; if (said2++ < 2) nvlog("vk: framebuffer fetch draw of %llu triangles drawn whole (split limit %d)",
                                                 (unsigned long long)tris, maxTris);
        return 0;
    }
    return count > 3 || instances > 1;
}
void nvmtl_vk_cmd_draw(nvk_cmdbuf *c, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance)
{
    if (c->nobind) return;
    const uint32_t n = instances ? instances : 1;
    if (nvmtl_fetch_split(c, count, n)) {
        for (uint32_t k = 0; k < n; k++) for (uint32_t i = 0; i + 3 <= count; i += 3) {
            if (k || i) nvmtl_fetch_barrier(c);
            NVMTL_SEMU_DRAW(c, pvkCmdDraw(c->cb, 3, 1, first + i, baseInstance + k));
        }
        return;
    }
    NVMTL_SEMU_DRAW(c, pvkCmdDraw(c->cb, count, n, first, baseInstance));
}
void nvmtl_vk_cmd_set_scissor(nvk_cmdbuf *c, int32_t x, int32_t y, uint32_t w, uint32_t h)
{ VkRect2D r = { { x, y }, { w, h } }; pvkCmdSetScissor(c->cb, 0, 1, &r); }
void nvmtl_vk_cmd_bind_pipeline(nvk_cmdbuf *c, nvk_pipeline *p) {
    if (p->pipe && c->graphics_pipeline == p->pipe) return;
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_GRAPHICS, (VkPipeline)p->pipe);
    c->graphics_pipeline = p->pipe;
    if (p->db_dyn) {
        pvkCmdSetDepthBoundsTestEnable(c->cb, c->db_on ? VK_TRUE : VK_FALSE);
        pvkCmdSetDepthBounds(c->cb, c->db_on ? c->db_lo : 0.0f, c->db_on ? c->db_hi : 1.0f);
    }
}
int nvmtl_vk_cmd_set_topology(nvk_cmdbuf *c, unsigned mtlType) {
    static const VkPrimitiveTopology map[5] = { VK_PRIMITIVE_TOPOLOGY_POINT_LIST, VK_PRIMITIVE_TOPOLOGY_LINE_LIST,
        VK_PRIMITIVE_TOPOLOGY_LINE_STRIP, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP };
    if (c->nobind) return 0;
    if (mtlType > 4) { static int said; if (said++ < 4) nvlog("G14 draw: unknown MTLPrimitiveType %u — NOT drawn", mtlType); return -1; }
    if (mtlType < 3 && !g_topo_any) { static int said2; if (said2++ < 4) nvlog("G14 draw: point/line primitives need "
        "dynamicPrimitiveTopologyUnrestricted, which this device does not report — NOT drawn (was: rasterised as triangles)"); return -1; }
    c->topo = mtlType;
    pvkCmdSetPrimitiveTopology(c->cb, map[mtlType]);
    pvkCmdSetPrimitiveRestartEnable(c->cb, (mtlType == 2 || mtlType == 4) && !c->restart_off ? VK_TRUE : VK_FALSE);
    if (mtlType != 3) { static int told[5]; if (!told[mtlType]++) nvlog("G14 first draw of MTLPrimitiveType %u in this process", mtlType); }
    return 0;
}
/* NVMTL_BINDCACHE_OFF=1: every draw binds its vertex and index buffers again (stage 1d bisection switch) */
static int nvmtl_bindcache_off(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NVMTL_BINDCACHE_OFF"); v = e && e[0] == '1'; if (v) nvlog("stage 1d: NVMTL_BINDCACHE_OFF=1 - vertex and index buffers are bound for every draw"); }
    return v;
}
/* glcpu4 NVMTL_VB_ADDR: the vertex buffer bound by the device address the bind table already holds (sl->address, set with
   the slot by nvmtl_vk_bind_buffer). vkCmdBindVertexBuffers(2) went through vk_common_CmdBindVertexBuffers ->
   vk_common_CmdBindVertexBuffers2EXT -> anv_CmdBindVertexBuffers3KHR, and the middle wrapper read the anv_buffer's
   device_address/size/address_flags: a cache miss per draw when the app binds a different buffer each draw (Minecraft's
   chunk buffers: 49 of 3595 render-thread samples at the two loads, prod7 profile). Same range as the handle path: the
   whole VkBuffer from off (VK_WHOLE_SIZE), stride only when the pipeline's stride is dynamic. Address flags: the two
   "unknown usage" bits, a claim that is always true (VkAddressCommandFlagBitsKHR); no buffer here is protected, the only
   bit ANV reads for a vertex buffer. Returns 0 to take the handle path (piece off, no address, offset past the end). */
static inline int nvmtl_vb_bind_addr(nvk_cmdbuf *c, uint32_t i, const nvmtl_slot *sl, VkDeviceSize off, int setStride, VkDeviceSize st)
{
    if (!pvkCmdBindVertexBuffers3KHR || !sl->address) return 0;
    const uint64_t vksize = (uint64_t)sl->size + sl->vextra;
    if (off >= vksize) return 0;
    const VkBindVertexBuffer3InfoKHR bi = { VK_STRUCTURE_TYPE_BIND_VERTEX_BUFFER_3_INFO_KHR, NULL, setStride ? VK_TRUE : VK_FALSE,
        { sl->address + off, vksize - off, setStride ? st : 0 },
        VK_ADDRESS_COMMAND_UNKNOWN_STORAGE_BUFFER_USAGE_BIT_KHR | VK_ADDRESS_COMMAND_UNKNOWN_TRANSFORM_FEEDBACK_BUFFER_USAGE_BIT_KHR };
    pvkCmdBindVertexBuffers3KHR(c->cb, i, 1, &bi);
    return 1;
}
int nvmtl_vk_cmd_vertex_inputs(nvk_cmdbuf *c, const nvk_pipeline *p, const uint32_t *vstride, uint32_t vset)
{
    if (c->nobind || !p || !p->vin_mask) return 0;
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    if (c->vbc_cb != c->cb || nvmtl_bindcache_off()) { c->vbc_cb = c->cb; c->vbc_valid = 0; }   // perf2: once, not per binding
    // perf2: the pipeline's vertex buffers by their bits (WAS: all 31 indices tested on every draw)
    for (uint32_t m = p->vin_mask & ((1u << NVMTL_NVIN) - 1u); m; m &= m - 1u) { const uint32_t i = (uint32_t)__builtin_ctz(m);
        nvmtl_slot *sl = t ? &t->s[0][i] : NULL;
        if (!sl || sl->kind != NVMTL_SLOT_BUF || !sl->h) { static int said; if (said++ < 8) nvlog("G16 draw: the pipeline fetches vertex "
            "attributes from buffer index %u and nothing is bound there — NOT drawn", i); return -1; }
        VkBuffer b = (VkBuffer)sl->h; VkDeviceSize off = sl->off;
        if (!p->vin_dyn) {
            if (!((c->vbc_valid >> i) & 1u) || c->vbc_buf[i] != (void *)b || c->vbc_off[i] != off || c->vbc_st[i] != NVMTL_VBC_NOSTRIDE) {
                if (!nvmtl_vb_bind_addr(c, i, sl, off, 0, 0)) pvkCmdBindVertexBuffers(c->cb, i, 1, &b, &off);
                c->vbc_valid |= 1u << i; c->vbc_buf[i] = (void *)b; c->vbc_off[i] = off; c->vbc_st[i] = NVMTL_VBC_NOSTRIDE;
            }
            continue;
        }
        VkDeviceSize st = p->vin_stride[i];
        if (p->vin_dyn & (1u << i)) {
            if (!vstride || !(vset & (1u << i))) { static int said; if (said++ < 8) nvlog("apps-dyn draw: buffer index %u has a dynamic stride "
                "(MTLBufferLayoutStrideDynamic) and no attributeStride was set there — NOT drawn", i); return -1; }
            st = vstride[i];
            if (st > g_lim.maxVertexInputBindingStride) { static int said; if (said++ < 8) nvlog("apps-dyn draw: attributeStride %llu at "
                "buffer index %u is past maxVertexInputBindingStride %u — NOT drawn", (unsigned long long)st, i, g_lim.maxVertexInputBindingStride); return -1; }
        }
        if (!((c->vbc_valid >> i) & 1u) || c->vbc_buf[i] != (void *)b || c->vbc_off[i] != off || c->vbc_st[i] != st) {
            if (!nvmtl_vb_bind_addr(c, i, sl, off, 1, st)) pvkCmdBindVertexBuffers2(c->cb, i, 1, &b, &off, NULL, &st);
            c->vbc_valid |= 1u << i; c->vbc_buf[i] = (void *)b; c->vbc_off[i] = off; c->vbc_st[i] = st;
        }
    }
    return 0;
}
/* vkCmdBindIndexBuffer unless the same buffer, offset and type are bound in this VkCommandBuffer already */
static inline void nvmtl_bind_index(nvk_cmdbuf *c, const nvk_buffer *idx, size_t offset, int is32)
{
    const uint32_t ty = is32 ? VK_INDEX_TYPE_UINT32 : VK_INDEX_TYPE_UINT16;
    if (c->ibc_cb == c->cb && c->ibc_buf == idx->buf && c->ibc_off == offset && c->ibc_type == ty && !nvmtl_bindcache_off()) return;
    pvkCmdBindIndexBuffer(c->cb, (VkBuffer)idx->buf, offset, (VkIndexType)ty);
    c->ibc_cb = c->cb; c->ibc_buf = idx->buf; c->ibc_off = offset; c->ibc_type = ty;
}
void nvmtl_vk_cmd_set_cull(nvk_cmdbuf *c, int mode)
{
    int m = mode & 3;
    pvkCmdSetCullMode(c->cb, m == 1 ? VK_CULL_MODE_FRONT_BIT : m == 2 ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE);
    pvkCmdSetFrontFace(c->cb, (mode & 4) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
}
// An index offset at or past the end of the index buffer: Vulkan's whole-size bind then underflows to a range over
// 4 GB and NVK's assert aborted the app (1.13 crash report, a video app drawing through OpenGL). Metal treats such a draw
// as invalid; it is skipped here and logged once.
static int nvmtl_index_range_ok(const nvk_buffer *idx, size_t offset)
{
    if (idx && idx->buf && offset < idx->size && idx->size - offset <= 0xffffffffull) return 1;
    static int told; if (!told++) nvlog("draw: index offset %zu outside its %llu-byte buffer - draw skipped", offset,
                                        (unsigned long long)(idx ? idx->size : 0));
    return 0;
}
void nvmtl_vk_cmd_draw_indexed(nvk_cmdbuf *c, nvk_buffer *idx, size_t offset, uint32_t count, int is32,
                               uint32_t instances, int32_t baseVertex, uint32_t baseInstance)
{
    if (!nvmtl_index_range_ok(idx, offset)) return;
    nvmtl_bind_index(c, idx, offset, is32);
    if (c->nobind) return;
    const uint32_t n = instances ? instances : 1;
    if (nvmtl_fetch_split(c, count, n)) {
        for (uint32_t k = 0; k < n; k++) for (uint32_t i = 0; i + 3 <= count; i += 3) {
            if (k || i) nvmtl_fetch_barrier(c);
            NVMTL_SEMU_DRAW(c, pvkCmdDrawIndexed(c->cb, 3, 1, i, baseVertex, baseInstance + k));
        }
        return;
    }
    NVMTL_SEMU_DRAW(c, pvkCmdDrawIndexed(c->cb, count, n, 0, baseVertex, baseInstance));
}

void nvmtl_vk_cmd_set_blend_color(nvk_cmdbuf *c, const float rgba[4]) { pvkCmdSetBlendConstants(c->cb, rgba); }
void nvmtl_vk_cmd_set_stencil_ref(nvk_cmdbuf *c, uint32_t front, uint32_t back)
{
    c->st_ref[0] = front; c->st_ref[1] = back;
    pvkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_FRONT_BIT, front);
    pvkCmdSetStencilReference(c->cb, VK_STENCIL_FACE_BACK_BIT, back);
}
void nvmtl_vk_cmd_set_stencil(nvk_cmdbuf *c, int enable, const uint32_t *f, const uint32_t *b)
{
    const uint32_t *o[2] = { f, b }; const VkStencilFaceFlags face[2] = { VK_STENCIL_FACE_FRONT_BIT, VK_STENCIL_FACE_BACK_BIT };
    pvkCmdSetStencilTestEnable(c->cb, enable ? VK_TRUE : VK_FALSE);
    for (int i = 0; i < 2; i++) {
        pvkCmdSetStencilOp(c->cb, face[i], (VkStencilOp)o[i][1], (VkStencilOp)o[i][3], (VkStencilOp)o[i][2], (VkCompareOp)o[i][0]);
        pvkCmdSetStencilCompareMask(c->cb, face[i], o[i][4]);
        pvkCmdSetStencilWriteMask(c->cb, face[i], o[i][5]);
        c->st_wmask[i] = enable ? o[i][5] : 0;
    }
}
// Returns -1 when the device has no depthBounds feature and the test was asked for; the caller says so once.
// Vulkan needs the range inside [0, 1] without VK_EXT_depth_range_unrestricted; a stored depth is in that range
// anyway, so clamping keeps the test's answer.
int nvmtl_vk_cmd_set_depth_bounds(nvk_cmdbuf *c, int on, float lo, float hi)
{
    if (!g_depth_bounds) return on ? -1 : 0;
    lo = !(lo > 0.0f) ? 0.0f : lo > 1.0f ? 1.0f : lo;
    hi = !(hi < 1.0f) ? 1.0f : hi < 0.0f ? 0.0f : hi;
    c->db_on = on ? 1 : 0; c->db_lo = lo; c->db_hi = hi;
    pvkCmdSetDepthBoundsTestEnable(c->cb, on ? VK_TRUE : VK_FALSE);
    pvkCmdSetDepthBounds(c->cb, on ? lo : 0.0f, on ? hi : 1.0f);
    return 0;
}
void nvmtl_vk_cmd_set_depth_bias(nvk_cmdbuf *c, float constant, float slope, float clamp)
{
    pvkCmdSetDepthBiasEnable(c->cb, constant != 0.0f || slope != 0.0f);
    pvkCmdSetDepthBias(c->cb, constant, clamp, slope);
}
void nvmtl_vk_cmd_set_line_width(nvk_cmdbuf *c, float w)
{
    if (!g_wide) return;
    float px = !(w >= 1.0f) ? 1.0f : w > 64.0f ? 64.0f : (float)(int)(w + 0.5f);
    pvkCmdSetLineWidth(c->cb, px);
}

int nvmtl_vk_fill_buffer(nvk_queue *q, nvk_buffer *b, size_t offset, size_t size, uint8_t value)
{
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_fill_buffer(&c, b, offset, size, value)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    return nvmtl_vk_submit_wait(&c);
}
static int nvmtl_counter_flush(nvk_cmdbuf *c);
static void nvmtl_counter_retire(nvk_cmdbuf *c);
static int nvmtl_counter_reset_finish(nvk_cmdbuf *c, VkCommandBuffer *reset);
static int g_pt_left; static uint64_t g_pt_seq;
void nvmtl_vk_pt_note(nvk_cmdbuf *c, uint32_t fs)
{
    if (!c || !c->pt_open || !c->pt_pool || c->pt_n >= 128 || !fs) return;
    for (unsigned k = 0; k < 4; k++) { if (c->pt_fs[c->pt_n][k] == fs) return; if (!c->pt_fs[c->pt_n][k]) { c->pt_fs[c->pt_n][k] = fs; return; } }
}
int nvmtl_pt_armed(void)
{
    static uint64_t next; uint64_t now = nvmtl_poll_ns();
    if (g_pt_left <= 0 && now >= next) { next = now + 250000000ull;
        if (nvmtl_diag_flag("passtime-request")) { unlink("/tmp/nvmtl-passtime-request"); g_pt_left = 400; nvlog("passtime: ARMED for 400 passes"); } }
    return g_pt_left > 0;
}
void nvmtl_vk_cmd_end_render(nvk_cmdbuf *c) {
    pvkCmdEndRenderPass(c->cb); c->in_rp = 0;
    nvmtl_order_barrier(c);
    if (c->pt_open && c->pt_pool) { pvkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkQueryPool)c->pt_pool, 2 * c->pt_n + 1); c->pt_n++; c->pt_open = 0; g_pt_left--; }
    (void)nvmtl_counter_flush(c); }

static void nvmtl_vk_cmd_retire_asq(nvk_cmdbuf *c);
static void nvmtl_cmd_retire(nvk_cmdbuf *c)
{
    if (c->life_live) { c->life_live = 0; atomic_fetch_sub(&g_cb_live, 1); }
    nvmtl_gpt_retire(c->cb);
    nvmtl_counter_retire(c);
    if (c->kt_pool) { pvkDestroyQueryPool(g_dev, (VkQueryPool)c->kt_pool, NULL); c->kt_pool = NULL; c->kt_n = 0; c->kt_open = 0; }
    nvmtl_vk_cmd_retire_occlusion(c);
    nvmtl_vk_cmd_retire_asq(c);
    if (c->timing_pool) {
        nvmtl_tspool_push((VkQueryPool)c->timing_pool);
        c->timing_pool = NULL;
    }
    for (unsigned i = 0; i < c->nfb; i++) pvkDestroyFramebuffer(g_dev, (VkFramebuffer)c->fbs[i], NULL);
    free(c->fbs); c->fbs = NULL; c->nfb = c->capfb = 0; c->fb = NULL;
    for (unsigned i = 0; i < c->nrv; i++) pvkDestroyImageView(g_dev, (VkImageView)c->rvs[i], NULL);
    free(c->rvs); c->rvs = NULL; c->nrv = c->caprv = 0;
    if (c->ndcache) {
        pthread_mutex_lock(&g_desc_lock);
        VkDescriptorSet back[NVMTL_DESC_BATCH_MAX];
        for (unsigned i = 0; i < c->ndcache && i < NVMTL_DESC_BATCH_MAX; i++) back[i] = (VkDescriptorSet)c->dcache[i];
        pvkFreeDescriptorSets(g_dev, (VkDescriptorPool)c->dcachepool, c->ndcache, back);
        g_desc_gen++; pthread_cond_broadcast(&g_desc_cond);   // nvmtl_desc_reclaim
        pthread_mutex_unlock(&g_desc_lock);
        c->ndcache = 0; c->dcachepool = NULL;
    }
    if (c->nsets) {
        static int recycle = -1;
        if (recycle < 0) { const char *e = getenv("NVMTL_DESC_RECYCLE"); recycle = !(e && e[0] == '0');
            nvlog("vk: retired descriptor sets %s (NVMTL_DESC_RECYCLE=%s)", recycle ? "recycled for the next allocations (up to 16384 kept)" : "freed", e ? e : "unset"); }
        pthread_mutex_lock(&g_desc_lock);
        for (unsigned i = 0; i < c->nsets;) {
            VkDescriptorPool p = (VkDescriptorPool)c->sets[i].pool; VkDescriptorSet run[256]; unsigned n = 0;
            int main_pool = 0;
            for (unsigned k = 0; recycle && k < g_ndescPools && !main_pool; k++) main_pool = g_descPools[k] == p;
            while (i < c->nsets && c->sets[i].pool == (void *)p && n < 256) {
                VkDescriptorSet set = (VkDescriptorSet)c->sets[i++].set;
                if (main_pool && g_nrecyc < NVMTL_DESC_RECYCLE_MAX) { g_recyc[g_nrecyc++] = (nvmtl_recycled_set){ p, set, c->subseq }; g_recyc_kept++; }
                else run[n++] = set;
            }
            if (n) pvkFreeDescriptorSets(g_dev, p, n, run);
        }
        const uint64_t hits = g_recyc_hits;
        g_desc_gen++; pthread_cond_broadcast(&g_desc_cond);   // nvmtl_desc_reclaim
        pthread_mutex_unlock(&g_desc_lock);
        static uint64_t said; if (recycle && hits >= said * 2 + 4096) { said = hits; nvlog("vk: descriptor sets recycled %llu times (%u waiting)", (unsigned long long)hits, g_nrecyc); }
    }
    free(c->sets); c->sets = NULL; c->nsets = c->capsets = 0; c->nobind = 0; c->skipped = 0; c->skip_oom = 0;
    c->fetch_last_set = NULL;   // freed with the sets above
    nvmtl_bt_put(c->table); c->table = NULL;
    nvmtl_address_page *page = c->address_pages;
    const int keep = nvmtl_keep_verdict(c->wseq);
    while (page) { nvmtl_address_page *next = page->next;
        if (keep >= 0 && !(keep > 0 && nvmtl_keep_page_push(page))) { nvmtl_vk_buffer_destroy(&page->buffer); free(page->udesc); free(page); }
        page = next; }
    c->address_pages = NULL; c->address_count = 0;
    nvmtl_db_retire(c, keep);   // the descriptor ring: kept, destroyed or leaked with the address pages
    if (c->cb && c->pool) {
        VkCommandBuffer cb = (VkCommandBuffer)c->cb;
        pvkFreeCommandBuffers(g_dev, (VkCommandPool)c->pool, 1, &cb);
        if (keep >= 0 && !(keep > 0 && nvmtl_keep_pool_push((VkCommandPool)c->pool))) pvkDestroyCommandPool(g_dev, (VkCommandPool)c->pool, NULL);
        c->cb = NULL; c->pool = NULL;
    }
}

int nvmtl_vk_cmd_abandon(nvk_cmdbuf *c)
{
    if (g_state != 1 || !c || !c->cb) return 0;
    g_cmd_abandoned++;
    nvmtl_cmd_retire(c);
    return 0;
}

static pthread_mutex_t nvmtl_submit_mutex = PTHREAD_RECURSIVE_MUTEX_INITIALIZER;
void (*nvmtl_pre_submit_hook)(void);
void (*nvmtl_pre_release_hook)(void *);
typedef struct { nvk_cmdbuf cb; unsigned begun, n, fills, copies, nhold; unsigned long long bytes; void *hold[4]; } nvmtl_prologue;
static __thread nvmtl_prologue *g_pre_cur;
static _Atomic unsigned long long g_pre_subs, g_pre_copies, g_pre_fills, g_pre_bytes, g_pre_fail;
static int nvmtl_prologue_on(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_PRESUBMIT_PROLOGUE"); on = !(e && e[0] == '0');
        nvlog("vk: pre-submit prologue %s (NVMTL_PRESUBMIT_PROLOGUE=%s)", on ? "ON - managed uploads and zero fills ride the submit they serve"
              : "OFF - every managed upload is its own submit, as before b78", e ? e : "unset");
    }
    return on;
}
int nvmtl_vk_pre_open(void)
{
    nvmtl_prologue *p = g_pre_cur;
    if (!p || p->nhold >= sizeof p->hold / sizeof p->hold[0]) return 0;
    if (!p->begun) {
        if (nvmtl_vk_cmd_begin(NULL, &p->cb)) { g_pre_fail++; nvlog("vk: pre-submit prologue begin FAILED - this flush submits on its own"); return 0; }
        p->begun = 1;
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
        pvkCmdPipelineBarrier(p->cb.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    return 1;
}
int nvmtl_vk_pre_copy(nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size)
{
    nvmtl_prologue *p = g_pre_cur;
    if (!p || !p->begun || !src || !dst) return -1;
    if (!size) return 0;
    if (srcOff > src->size || size > src->size - srcOff || dstOff > dst->size || size > dst->size - dstOff) return -1;
    if (p->fills && !p->copies) {
        VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT };
        pvkCmdPipelineBarrier(p->cb.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
    }
    VkBufferCopy bc = { srcOff, dstOff, size };
    pvkCmdCopyBuffer(p->cb.cb, (VkBuffer)src->buf, (VkBuffer)dst->buf, 1, &bc);
    p->n++; p->copies++; p->bytes += size;
    return 0;
}
int nvmtl_vk_pre_fill(nvk_buffer *b, size_t offset, size_t size, uint32_t word)
{
    nvmtl_prologue *p = g_pre_cur;
    if (!p || !p->begun || !b) return -1;
    if (!size) return 0;
    if ((offset | size) & 3 || offset > b->size || size > b->size - offset) return -1;
    pvkCmdFillBuffer(p->cb.cb, (VkBuffer)b->buf, offset, size, word);
    p->n++; p->fills++; p->bytes += size;
    return 0;
}
void nvmtl_vk_pre_hold(void *retained)
{
    nvmtl_prologue *p = g_pre_cur;
    if (p && retained && p->nhold < sizeof p->hold / sizeof p->hold[0]) p->hold[p->nhold++] = retained;
}
static void nvmtl_pre_drop(nvmtl_prologue *p)
{
    for (unsigned i = 0; i < p->nhold; i++) if (p->hold[i] && nvmtl_pre_release_hook) nvmtl_pre_release_hook(p->hold[i]);
    p->nhold = 0;
}
struct nvmtl_inflight { nvk_cmdbuf *c; VkFence fence; nvmtl_prologue pre; uint64_t pt[8]; unsigned gen; };
nvmtl_inflight *nvmtl_vk_inflight_new(void) { return calloc(1, sizeof(nvmtl_inflight)); }
void nvmtl_vk_inflight_free(nvmtl_inflight *f) { free(f); }
void nvmtl_vk_gsub_nvk_stats(uint64_t *out, unsigned n)
{
    static void (*fn)(uint64_t *, unsigned); static int looked;
    if (!looked) { fn = g_lib ? (void (*)(uint64_t *, unsigned))dlsym(g_lib, "nvkmd_nvrm_gsub_stats") : NULL; looked = 1; }
    for (unsigned i = 0; i < n; i++) out[i] = 0;
    if (fn) fn(out, n); else if (n) out[0] = ~0ull;
}
static struct { const char *who; uint64_t n; uint64_t ns; } g_subwho[24];
static unsigned g_subwho_n;
static void nvmtl_subwho_note(const char *who, uint64_t ns)
{
    if (!who) who = "?";
    unsigned i = 0;
    for (; i < g_subwho_n; i++) if (g_subwho[i].who == who) break;
    if (i == g_subwho_n) {
        if (g_subwho_n >= 24) return;
        g_subwho[g_subwho_n].who = who; g_subwho[g_subwho_n].n = 0; g_subwho[g_subwho_n].ns = 0;
        i = g_subwho_n++;
    }
    g_subwho[i].n++; g_subwho[i].ns += ns;
    uint64_t total = 0;
    for (unsigned k = 0; k < g_subwho_n; k++) total += g_subwho[k].n;
    if ((total & 0xfff) == 0) {
        char line[1200]; size_t off = 0;
        for (unsigned k = 0; k < g_subwho_n && off < sizeof line - 90; k++) {
            int w = snprintf(line + off, sizeof line - off, "%s%s=%llu", off ? ", " : "",
                             g_subwho[k].who, (unsigned long long)g_subwho[k].n);
            if (w > 0) off += (size_t)w;
        }
        nvlog("vk: submit census over %llu submissions - %s", (unsigned long long)total, line);
    }
}
/* Submit fences are recycled (reset, kept up to NVMTL_FENCE_KEEP): on ANV/Xe each one is a DRM syncobj, so a create and
   a destroy per submission were two ioctls into IntelXe.kext on every commit. A fence whose wait did not end in success is
   destroyed, never reused. NVMTL_FENCE_KEEP=0: create and destroy per submit, as before. */
#define NVMTL_FENCE_KEEP 64
static VkFence g_fence_keep[NVMTL_FENCE_KEEP]; static unsigned g_nfence_keep; static pthread_mutex_t g_fence_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned nvmtl_fence_cap(void)
{
    static int cap = -1;
    if (cap < 0) { const char *e = getenv("NVMTL_FENCE_KEEP"); const int v = e ? atoi(e) : NVMTL_FENCE_KEEP;
        cap = v < 0 ? 0 : v > NVMTL_FENCE_KEEP ? NVMTL_FENCE_KEEP : v;
        nvlog("vk: submit fences recycled (keep <= %d, NVMTL_FENCE_KEEP=%s)", cap, e ? e : "unset"); }
    return (unsigned)cap;
}
static VkFence nvmtl_fence_get(VkResult *r)
{
    VkFence f = VK_NULL_HANDLE;
    if (nvmtl_fence_cap()) { pthread_mutex_lock(&g_fence_lock); if (g_nfence_keep) f = g_fence_keep[--g_nfence_keep]; pthread_mutex_unlock(&g_fence_lock); }
    if (f) {
        if (nvmtl_reuse_check() && pvkWaitForFences(g_dev, 1, &f, VK_TRUE, 0) != VK_TIMEOUT) nvmtl_reuse_flag("fence (not reset)", 0, 0);
        *r = VK_SUCCESS; return f; }
    VkFenceCreateInfo fci = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    *r = pvkCreateFence(g_dev, &fci, NULL, &f);
    return *r == VK_SUCCESS ? f : VK_NULL_HANDLE;
}
// fence has signalled (its wait returned VK_SUCCESS): reset and keep it, or destroy it
static void nvmtl_fence_put(VkFence f)
{
    if (!f) return;
    if (nvmtl_fence_cap() && pvkResetFences(g_dev, 1, &f) == VK_SUCCESS) {
        pthread_mutex_lock(&g_fence_lock);
        if (g_nfence_keep < nvmtl_fence_cap()) { g_fence_keep[g_nfence_keep++] = f; f = VK_NULL_HANDLE; }
        pthread_mutex_unlock(&g_fence_lock);
    }
    if (f) pvkDestroyFence(g_dev, f, NULL);
}
/* ---- implicit IOSurface sync (IntelXe.kext surface fences, uapi/intelxe_drm.h; Mesa anv_nvmtl_surface.c) -------------------
   Apple's drivers order a compositor's GPU read of an IOSurface after the producer's GPU write in the kernel, and
   CoreAnimation and Apple's GL hand a surface over once its writer is scheduled. IntelXe.kext keeps the fences of each
   surface's last writers and readers (the dma-resv model): a submission that writes or reads a surface first imports
   the fences it must wait for into g_isync_sem (a timeline used for nothing else, so waiting on its newest point waits
   on every import) and waits for them in the kernel's job queue, then attaches its own fence. NVMTL_IMPLICIT_SYNC=0, an
   older kernel (no INTELXE_CAP_SURFACE_FENCES), or a Mesa without the entry points: the CPU-side ordering (fix19). */
static VkSemaphore g_isync_sem;
static _Atomic unsigned long long g_isync_attached, g_isync_waited, g_isync_waits_added, g_isync_notready, g_isync_fail;
int nvmtl_vk_isync(void)
{
    static int on = -1;
    if (on >= 0) return on;
    static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&m);
    if (on < 0) {
        const char *e = getenv("NVMTL_IMPLICIT_SYNC");
        int v = 0; uint32_t cap = 0; const char *why = NULL;
        if (e && e[0] == '0') why = "off (NVMTL_IMPLICIT_SYNC=0)";
        else if (nvmtl_vk_init() || !g_dev) why = "no device";
        else if (!panv_surf_cap || !panv_surf_attach || !panv_surf_import || !panv_surf_wait) why = "the Vulkan driver has no nvmtl_anv_surface_* entry points";
        else if (!g_timeline) why = "no timeline semaphores";
        else if (panv_surf_cap(g_dev, &cap) != VK_SUCCESS || cap != 1) why = "the kernel has no surface fences (INTELXE_CAP_SURFACE_FENCES)";
        else {
            PFN_vkCreateSemaphore cs = (PFN_vkCreateSemaphore)pvkGetDeviceProcAddr(g_dev, "vkCreateSemaphore");
            VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
            VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0 };
            if (!cs || cs(g_dev, &sci, NULL, &g_isync_sem) != VK_SUCCESS) why = "no timeline semaphore for the imports";
            else {
                uint64_t w = 0; uint32_t n = 0;   /* the driver must accept this semaphore as a syncobj timeline */
                if (panv_surf_import(g_dev, g_isync_sem, 0xffffffffu, 1u, &w, &n) == VK_ERROR_FEATURE_NOT_PRESENT) why = "the import timeline is not a kernel syncobj";
                else v = 1;
            }
        }
        nvlog("implicit IOSurface sync: %s", v ? "ON - surface writers attach their fences, readers wait for them on the GPU (IntelXe.kext surface fences)" : why);
        on = v;
    }
    pthread_mutex_unlock(&m);
    return on;
}
int nvmtl_vk_cmd_surface_use(nvk_cmdbuf *c, uint32_t sid, int write)
{
    if (!c || !sid || !nvmtl_vk_isync()) return -1;
    for (unsigned i = 0; i < c->n_isync; i++) if (c->isync_sid[i] == sid) { c->isync_write[i] |= write ? 1 : 0; return 0; }
    if (c->n_isync >= 8) return -1;
    c->isync_sid[c->n_isync] = sid; c->isync_write[c->n_isync++] = write ? 1 : 0;
    return 0;
}
int nvmtl_vk_surface_cpu_wait(uint32_t sid, int writer, unsigned timeout_ms)
{
    if (!sid || !nvmtl_vk_isync()) return -1;
    const VkResult r = panv_surf_wait(g_dev, sid, writer ? 2u : 1u, (int64_t)(clock_gettime_nsec_np(CLOCK_UPTIME_RAW) + (uint64_t)timeout_ms * 1000000ull));
    if (r == VK_TIMEOUT) { static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 8) nvlog("implicit sync: surface %u: its %s did not finish in %u ms - used as it is", sid, writer ? "users" : "writers", timeout_ms); return 1; }
    return 0;
}
/* Before the submit (nvmtl_submit_mutex held): the fences c's surfaces must wait for, as one point of g_isync_sem. */
static uint64_t nvmtl_isync_import(nvk_cmdbuf *c)
{
    uint64_t wait = 0;
    for (unsigned i = 0; i < c->n_isync; i++) {
        uint64_t w = 0; uint32_t n = 0;
        const VkResult r = panv_surf_import(g_dev, g_isync_sem, c->isync_sid[i], c->isync_write[i] ? 2u : 1u, &w, &n);
        if (r != VK_SUCCESS) { if (atomic_fetch_add(&g_isync_fail, 1) < 8) nvlog("implicit sync: import for surface %u -> %d - not waited for", c->isync_sid[i], r); continue; }
        if (n) { atomic_fetch_add(&g_isync_waits_added, n); if (w > wait) wait = w; }
    }
    if (wait) atomic_fetch_add(&g_isync_waited, 1);
    return wait;
}
/* After the submit: c's fence becomes the newest writer (or a reader) of its surfaces. */
static void nvmtl_isync_attach(nvk_cmdbuf *c, VkFence fence)
{
    for (unsigned i = 0; i < c->n_isync; i++) {
        VkResult r;
        for (int tries = 0; ; tries++) {   /* the driver submits at once; VK_NOT_READY (no fence yet) would be a deferred submit */
            r = panv_surf_attach(g_dev, fence, VK_NULL_HANDLE, 0, c->isync_sid[i], c->isync_write[i] ? 1u : 2u);
            if (r != VK_NOT_READY || tries >= 200) break;
            if (!tries) atomic_fetch_add(&g_isync_notready, 1);
            usleep(50);
        }
        if (r == VK_SUCCESS) atomic_fetch_add(&g_isync_attached, 1);
        else if (atomic_fetch_add(&g_isync_fail, 1) < 8) nvlog("implicit sync: attach to surface %u -> %d - its readers will not wait for it", c->isync_sid[i], r);
    }
    const unsigned long long n = atomic_load(&g_isync_attached);
    if (n && (n == 1 || n % 20000 == 0))
        nvlog("implicit sync: %llu fences attached, %llu submissions waited on the GPU for %llu imported fences, %llu not ready at once, %llu failures",
              n, atomic_load(&g_isync_waited), atomic_load(&g_isync_waits_added), atomic_load(&g_isync_notready), atomic_load(&g_isync_fail));
}
int nvmtl_vk_submit_wait_at(nvk_cmdbuf *c, const char *who)
{
    nvmtl_inflight f;
    const int r = nvmtl_vk_submit_begin_at(c, &f, who);
    return r ? r : nvmtl_vk_submit_finish(&f);
}
int nvmtl_vk_submit_partial_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SUBMIT_PARTIAL"); on = e && e[0] == '1';
        if (on) nvlog("vk: NVMTL_SUBMIT_PARTIAL=1 - recordings with draws that could not be recorded are submitted anyway (before fix30)"); }
    return on;
}
int nvmtl_vk_submit_begin_at(nvk_cmdbuf *c, nvmtl_inflight *f, const char *who)
{
    nvmtl_subwho_note(who, 0);
    g_last_submit_who = who;
    memset(f, 0, sizeof *f); f->c = c;
    uint64_t *const pt = f->pt;
    if (c && c->dead) return -3;   // made after the loss: nothing was recorded, nothing is in flight
    if (g_state != 1 || !c || !c->cb) return -2;
    if (c->counter_error || nvmtl_counter_flush(c)) { nvmtl_cmd_retire(c); return -1; }
    // Draws or dispatches that could not be recorded (no descriptor set, an unsupported dispatch): the recording is not
    // the work it was asked to be, and running it would lose writes for good (Minecraft's atlas uploads, 2026-10-29: black
    // blocks for the whole session). It is not submitted; the Metal side has ended its command buffer with an error
    // (nvmtlSealPartWithSignal). NVMTL_SUBMIT_PARTIAL=1: submit it anyway, as before fix30.
    if (c->skipped && !nvmtl_vk_submit_partial_on()) {
        static _Atomic unsigned long long refused; const unsigned long long k = atomic_fetch_add(&refused, 1) + 1;
        if (k <= 8 || !(k & (k - 1)))
            nvlog("vk: %s: NOT submitted - %u draws/dispatches could not be recorded (%u for want of memory); %llu such so far",
                  who ? who : "submit", c->skipped, c->skip_oom, k);
        nvmtl_cmd_retire(c); return -1;
    }
    VkCommandBuffer counter_reset = VK_NULL_HANDLE;
    if (nvmtl_counter_reset_finish(c, &counter_reset)) { nvmtl_cmd_retire(c); return -1; }
    c->timing.clock = nvmtl_clock_snapshot();
    c->timing.host_before_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (c->timing_pool && !c->in_rp)
        pvkCmdWriteTimestamp(c->cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, (VkQueryPool)c->timing_pool, 1);
    else c->timing.clock.valid = 0;
    pt[0] = nvmtl_perf_now();
    const int pro = nvmtl_prologue_on();
    if (!pro && nvmtl_pre_submit_hook) { const int k0 = g_perf_kind; g_perf_kind |= 1; nvmtl_pre_submit_hook(); g_perf_kind = k0; }
    const uint64_t sA = nvmtl_perf_now();
    const uint32_t wseq = nvmtl_keep_on() ? nvmtl_wit_record(c) : 0;
    if (!c->in_rp) nvmtl_gpt_end(c->cb, who);
    VkResult result = pvkEndCommandBuffer(c->cb);
    const uint64_t sB = nvmtl_perf_now();
    if (result != VK_SUCCESS) { nvmtl_cmd_retire(c); return -1; }
    VkFence fence = nvmtl_fence_get(&result);
    const uint64_t sC = nvmtl_perf_now();
    if (result != VK_SUCCESS) { nvmtl_cmd_retire(c); return -1; }
    nvmtl_prologue *const pre = &f->pre;
    pthread_mutex_lock(&nvmtl_submit_mutex);
    if (pro && nvmtl_pre_submit_hook) {
        nvmtl_prologue *const up = g_pre_cur; g_pre_cur = pre;
        const int k0 = g_perf_kind; g_perf_kind |= 1; nvmtl_pre_submit_hook(); g_perf_kind = k0;
        g_pre_cur = up;
    }
    const uint64_t sD = nvmtl_perf_now();
    VkCommandBuffer cbs[3]; uint32_t ncb = 0;
    if (counter_reset) cbs[ncb++] = counter_reset;
    if (pre->begun) {
        VkResult pr = VK_SUCCESS;
        if (pre->n) {
            VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
            pvkCmdPipelineBarrier(pre->cb.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
            nvmtl_gpt_end(pre->cb.cb, "prologue (managed uploads)");
            pr = pvkEndCommandBuffer(pre->cb.cb);
        }
        if (pre->n && pr == VK_SUCCESS) cbs[ncb++] = pre->cb.cb;
        else {
            if (pre->n) { g_pre_fail++; nvlog("vk: pre-submit prologue End -> %d - %u uploads/fills (%llu bytes) LOST, the GPU keeps the old bytes", pr, pre->n, pre->bytes); }
            nvmtl_cmd_retire(&pre->cb); nvmtl_pre_drop(pre); pre->begun = 0;
        }
    }
    cbs[ncb++] = c->cb;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, ncb, cbs, 0, NULL };
    const int isync = c->n_isync && nvmtl_vk_isync();
    uint64_t isync_wait = isync ? nvmtl_isync_import(c) : 0;
    const VkPipelineStageFlags isync_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &isync_wait, 0, NULL };
    if (isync_wait) { si.pNext = &tsi; si.waitSemaphoreCount = 1; si.pWaitSemaphores = &g_isync_sem; si.pWaitDstStageMask = &isync_stage; }
    c->wseq = wseq;
    nvmtl_wit_control(wseq);
    const uint64_t sE = nvmtl_perf_now();
    if (pt[0]) { pt[1] = pt[0] + (sA - pt[0]) + (sD - sC); pt[2] = pt[1] + (sB - sA) + (sE - sD); pt[3] = pt[2] + (sC - sB); }
    c->subseq = __atomic_add_fetch(&g_sub_seq, 1, __ATOMIC_ACQ_REL);
    if (g_cmdcensus > 0) nvmtl_cc_submitted();
    f->gen = atomic_load(&g_loss_gen);
    int fake_submit = 0, resubmitted = 0;
    if (g_state == 1 && nvmtl_fake_fire(&g_fake_submit)) { result = VK_ERROR_DEVICE_LOST; fake_submit = 1; }
    else result = g_state == 1 ? pvkQueueSubmit(g_queue, 1, &si, fence) : VK_ERROR_DEVICE_LOST;
    if (result == VK_ERROR_DEVICE_LOST && g_state == 1 && nvmtl_vk_recover(f->gen, fake_submit, who ? who : "vkQueueSubmit", result)) {
        // The submission itself met the loss (a banned queue refuses execs): none of it reached the GPU, and a failed
        // vkQueueSubmit consumes neither its wait nor its fence. Once the queue is replaced it goes to the new one.
        f->gen = atomic_load(&g_loss_gen);
        result = pvkQueueSubmit(g_queue, 1, &si, fence);
        resubmitted = 1;
        nvlog("vk: %s: submitted again after the GPU recovery -> %d", who ? who : "submit", result);
    }
    if (result == VK_SUCCESS && isync) nvmtl_isync_attach(c, fence);
    pthread_mutex_unlock(&nvmtl_submit_mutex);
    pt[4] = nvmtl_perf_now();
    g_cmd_submitted++;
    if (result != VK_SUCCESS) {
        if (resubmitted && g_state == 1) {   // lost again at once: this command buffer fails, the next submit recovers (bounded)
            nvmtl_sub_done(c->subseq);
            atomic_fetch_add(&g_lost_victim, 1);
            return -4;
        }
        g_state = -1;
        nvlog("vk: queue submit -> %d; disabling backend and retaining uncertain in-flight resources", result);
        nvmtl_vk_note_loss("submit", (int)result, 0);
        return -2;
    }
    f->fence = fence;
    if (c->on_submit) { void (*cb)(void *) = c->on_submit; c->on_submit = NULL; cb(c->on_submit_ctx); }
    return 0;
}
void nvmtl_vk_asyncup_reap(void);
int nvmtl_vk_submit_finish(nvmtl_inflight *f)
{
    nvk_cmdbuf *const c = f->c; const VkFence fence = f->fence; uint64_t *const pt = f->pt;
    nvmtl_prologue *const pre = &f->pre;
    const long budget_ms = nvmtl_gpu_wait_budget_ms();
    const uint64_t w0 = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    VkResult wr; long waited_ms = 0, said_ms = 0;
    // A fence that is late is a GPU that is busy, not a GPU that is gone. Giving up at the budget used to disable the
    // backend for the rest of the process; in WindowServer every later command buffer was then refused and CoreDisplay
    // aborted the session, which is what three Macs on macOS 26 reported as a crash. The wait now runs on past the
    // budget, in calls twice as long as the budget: the Vulkan driver's own no-progress detector is armed per call
    // with a five-second threshold, checked after the call's own deadline, so it turns a stalled channel into
    // VK_ERROR_DEVICE_LOST only when a single call outlasts that threshold. A fence that neither signals nor errors
    // for ten budgets (50 s in WindowServer, ten minutes elsewhere) is given up as lost, so a stall nothing reports still ends.
    const long give_up_ms = budget_ms * 10;
    /* NVMTL_FENCE_SPIN_US=<n> (A/B, default 0): poll the fence for up to n us before sleeping in the kernel's wait, so a
       short job's completion is seen without the interrupt -> syncobj -> thread wake-up path (Intel's own runtimes poll
       their completion tag before a kernel wait the same way). */
    { static long spin_us = -1;
      if (spin_us < 0) { const char *e = getenv("NVMTL_FENCE_SPIN_US"); spin_us = e && *e ? atol(e) : 0; if (spin_us < 0) spin_us = 0;
                         if (spin_us) nvlog("vk: fence wait polls for %ld us before sleeping (NVMTL_FENCE_SPIN_US)", spin_us); }
      if (spin_us) {
          const uint64_t end = w0 + (uint64_t)spin_us * 1000ull;
          do { if (pvkWaitForFences(g_dev, 1, &fence, VK_TRUE, 0) != VK_TIMEOUT) break; __builtin_ia32_pause(); }
          while (clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) < end);
      } }
    for (;;) {
        const long slice_ms = budget_ms < 250 ? 500 : budget_ms * 2;
        nvmtl_wait_enter();
        wr = pvkWaitForFences(g_dev, 1, &fence, VK_TRUE, (uint64_t)slice_ms * 1000 * 1000);
        nvmtl_wait_leave();
        waited_ms = (long)((clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - w0) / 1000000ull);
        if (wr != VK_TIMEOUT) break;
        if (g_state != 1) break;
        if (waited_ms >= give_up_ms) {
            nvlog("vk: fence still pending after %ld ms in %s with no error from the device; giving it up as lost", waited_ms, getprogname());
            break;
        }
        if (waited_ms >= budget_ms && waited_ms - said_ms >= budget_ms) {
            said_ms = waited_ms;
            nvlog("vk: fence still pending after %ld ms (budget %ld ms) in %s - waiting on, the device has not reported a loss", waited_ms, budget_ms, getprogname());
        }
    }
    pt[5] = nvmtl_perf_now();
    { extern __thread uint64_t nvmtl_done_split_fence_ns; nvmtl_done_split_fence_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
    atomic_fetch_add(&g_submit_wait_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - w0);
    atomic_fetch_add(&g_submit_waits, 1);
    if (waited_ms > g_slow_submit_ms) {
        g_slow_submit_ms = waited_ms;
        if (waited_ms >= 1000)
            nvlog("vk: slowest submit so far %ld ms (budget %ld ms) - this is the wait the old 5 s watchdog called a dead device", waited_ms, budget_ms);
    }
    int injected = 0;
    if (wr == VK_SUCCESS && g_state == 1 && nvmtl_fake_fire(&g_fake_wait)) { wr = VK_ERROR_DEVICE_LOST; injected = 1; }
    // a wait that succeeded after the queue it went to was replaced: its fence may have been signalled by the ban
    if (wr == VK_SUCCESS && g_state == 1 && f->gen != atomic_load(&g_loss_gen))
        return nvmtl_vk_lost(c->subseq, f->gen, 0, "a fence wait across a GPU recovery", VK_ERROR_DEVICE_LOST);
    if (wr != VK_SUCCESS || g_state != 1) {
        if (g_state == 1 && (wr == VK_ERROR_DEVICE_LOST || wr == VK_TIMEOUT)) {
            const int r = nvmtl_vk_lost(c->subseq, f->gen, injected, "fence wait", wr);
            if (r != -2) return r;
        }
        g_state = -1;
        nvmtl_vk_note_loss("fence", (int)wr, waited_ms);
        nvlog("vk: fence wait -> %d after %ld ms (budget %ld ms); the device is lost, disabling backend without retiring in-flight resources", wr, waited_ms, budget_ms);
        return -2;
    }
    nvmtl_sub_done(c->subseq);
    // Uploads left in flight (surface uploads through imported pages) were finished only by the next such upload or a
    // later use of their image; one that nothing touched again held the submission watermark back for good, and with it
    // every freed buffer waiting on it (Minecraft on Apple GL: 5 uploads at start, then 130 MB/s held, 5 Nov). Each
    // finished wait now also finishes the ones whose fence has signalled.
    { static __thread int t_aureap; if (!t_aureap) { t_aureap = 1; nvmtl_vk_asyncup_reap(); t_aureap = 0; } }
    if (g_hitch_ms > 0) nvmtl_hitch_tick();
    VkFence spent = fence;   // signalled: recycled below (nvmtl_fence_put)
    if (c->wseq && c->wseq != NVMTL_WSEQ_BLIND) {
        // The fence has already signalled, so the GPU is finished with this command buffer; the witness only certifies
        // its pool and address pages for reuse. A missing one used to switch the backend off for the whole process, and
        // every later command buffer came back nil: a terminal printing fast froze for good with its CoreAnimation commit
        // waiting forever (issue 322). Now that command buffer's pool and pages are leaked, never reused, and work goes on.
        if (nvmtl_keep_verdict(c->wseq) < 0) {
            const unsigned long long n = __atomic_add_fetch(&g_wit_lost, 1, __ATOMIC_RELAXED);
            if (n == 1 || (n & 0xFF) == 0)
                nvlog("vk: completion witness %u missing after its fence signalled (slot holds %u; %llu so far in %s) - "
                      "its pool and pages are leaked, the backend stays on", c->wseq, nvmtl_wit_read(c->wseq), n, getprogname());
            c->wseq = NVMTL_WSEQ_LOST;
            if (n == NVMTL_WIT_LOST_MAX && !__atomic_exchange_n(&g_keep_retired, 1, __ATOMIC_ACQ_REL))
                nvlog("vk: %d completion witnesses missing in %s - pool reuse is off for this process; pools are destroyed after their fences",
                      NVMTL_WIT_LOST_MAX, getprogname());
        } else c->wseq = 0;
    }
    c->timing.host_after_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (c->timing_pool && c->timing.clock.valid) {
        uint64_t pair[4] = {0};
        VkResult qr = pvkGetQueryPoolResults(g_dev, (VkQueryPool)c->timing_pool, 0, 2,
            sizeof pair, pair, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
        // the ticks only: the conversion to host time runs when the command buffer completes, once, not per part
        if (qr == VK_SUCCESS && pair[1] && pair[3]) { c->timing.start_tick = pair[0]; c->timing.end_tick = pair[2]; c->timing.valid = 1;
            if (g_hitch_ms > 0) {   // GPU time, and how long after the GPU's end the host saw the completion (signalling + worker lag)
                nvmtl_hitch_add_ns(NVH_GPU, (uint64_t)((double)nvmtl_tick_delta(pair[2], pair[0], g_timestamp_bits) * g_lim.timestampPeriod));
                const long double end = (long double)c->timing.clock.uptime_ns +
                    nvmtl_tick_delta(pair[2], c->timing.clock.gpu_tick, c->timing.clock.valid_bits) * c->timing.clock.period_ns;
                if (isfinite(end) && end > 0 && (long double)c->timing.host_after_ns > end)
                    nvmtl_hitch_add_ns(NVH_DONELAG, (uint64_t)((long double)c->timing.host_after_ns - end));
            } }
    }
    if (c->kt_pool) nvmtl_kt_collect(c);
    if (c->pt_pool && c->pt_n) {
        uint64_t r[256] = {0}; const unsigned n = c->pt_n;
        if (pvkGetQueryPoolResults(g_dev, (VkQueryPool)c->pt_pool, 0, 2 * n, sizeof(uint64_t) * 2 * n, r, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
            static FILE *pf; if (!pf) { char fp[512]; const char *td = getenv("TMPDIR"); snprintf(fp, sizeof fp, "%s/nvmtl-passtime-%d.log", td && *td ? td : "/tmp", (int)getpid()); pf = fopen(fp, "a"); }
            const double us = g_lim.timestampPeriod / 1000.0; const uint64_t mask = g_timestamp_bits >= 64 ? ~0ull : ((1ull << g_timestamp_bits) - 1);
            const uint64_t seq = ++g_pt_seq;
            for (unsigned i = 0; pf && i < n; i++) {
                const double gpu = (double)((r[2 * i + 1] - r[2 * i]) & mask) * us;
                const double gap = i ? (double)((r[2 * i] - r[2 * i - 1]) & mask) * us : 0.0;
                fprintf(pf, "cb %llu pass %u %ux%u fmt %u gpu_us %.1f gap_us %.1f fs %08x %08x %08x %08x\n", (unsigned long long)seq, i, c->pt_w[i], c->pt_h[i], c->pt_f[i], gpu, gap,
                        c->pt_fs[i][0], c->pt_fs[i][1], c->pt_fs[i][2], c->pt_fs[i][3]);
            }
            if (pf) fflush(pf);
        }
        pvkDestroyQueryPool(g_dev, (VkQueryPool)c->pt_pool, NULL); c->pt_pool = NULL; c->pt_n = 0;
    }
    nvmtl_fence_put(spent);
    if (c->nsets > g_peak_sets) { g_peak_sets = c->nsets; nvlog("vk: commit high-water: %u descriptor sets = %u draws in one command buffer (%u pools open)", c->nsets, c->nsets / 2, g_ndescPools); }
    if (c->nfb > g_peak_fbs) g_peak_fbs = c->nfb;
    if (c->skipped) nvlog("vk: commit with %u draws/dispatches SKIPPED (%u for want of memory; NVMTL_SUBMIT_PARTIAL=1 submitted it anyway)", c->skipped, c->skip_oom);
    const unsigned perf_sets = c->nsets;
    pt[6] = nvmtl_perf_now();
    if (pre->begun) {
        pre->cb.wseq = c->wseq; nvmtl_cmd_retire(&pre->cb); nvmtl_pre_drop(pre);
        const unsigned long long k = g_pre_subs++; g_pre_copies += pre->copies; g_pre_fills += pre->fills; g_pre_bytes += pre->bytes;
        if (!k || ((k + 1) & 0xFFF) == 0)
            nvlog("vk: pre-submit prologue census: %llu submits carried %llu managed uploads + %llu zero fills (%llu KB) - each WAS its own submit; %llu prologues failed",
                  k + 1, (unsigned long long)g_pre_copies, (unsigned long long)g_pre_fills, (unsigned long long)(g_pre_bytes >> 10), (unsigned long long)g_pre_fail);
    }
    nvmtl_cmd_retire(c);
    pt[7] = nvmtl_perf_now();
    nvmtl_perf_submit(perf_sets, pt);
    return 0;
}

static void nvmtl_vk_buffer_destroy_body(nvk_buffer *b);
void nvmtl_vk_buffer_destroy(nvk_buffer *b)
{
    const uint64_t t0 = nvmtl_perf_now();
    const size_t n = b ? b->size : 0;
    // imported buffers (host memory, IOSurfaces: the fork's import cache) keep their own release order: the memory
    // behind them belongs to someone who frees it right after this returns
    if (b && b->buf && !b->imported && nvmtl_buffer_grace_on() && nvmtl_grace_defer(b, NULL)) { nvmtl_perf_note(NVP_BUFDEL, t0, n); return; }
    nvmtl_vk_buffer_destroy_body(b);
    nvmtl_perf_note(NVP_BUFDEL, t0, n);
}
static int nvmtl_impc_put(nvk_buffer *b);
static void nvmtl_vk_buffer_destroy_body(nvk_buffer *b)
{
    if (g_state != 1 || !b || !b->buf) return;
    if (b->imported && nvmtl_impc_put(b)) return;
    g_buf_gone++;
    pvkDestroyBuffer(g_dev, (VkBuffer)b->buf, NULL);
    if (b->pool) pool_buffer_release(b);
    if (!b->placed && !b->imported && b->mem && b->map && pvkUnmapMemory) { pvkUnmapMemory(g_dev, (VkDeviceMemory)b->mem); b->map = NULL; }
    if (b->bar1) g_bar1_bytes -= b->alloc;
    if (!b->placed && b->mem) { pvkFreeMemory(g_dev, (VkDeviceMemory)b->mem, NULL); g_alloc_bytes -= b->alloc; }
    memset(b, 0, sizeof *b);
}

void nvmtl_vk_buffer_retire(nvk_buffer *b)
{
    if (g_state != 1 || !b || !b->buf) return;
    if (!bpark_on() || b->pool || b->placed || b->imported || b->moff || !b->mem || !b->alloc || b->alloc > NVMTL_BUFPARK_MAX1) {
        nvmtl_vk_buffer_destroy(b); return; }
    const uint64_t t0 = nvmtl_perf_now();
    const size_t n = b->size;
    nvk_buffer ev[NVMTL_BUFPARK_N + 1]; unsigned nev = 0;
    const uint64_t pnow = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), age = bpark_age_ns();
    pthread_mutex_lock(&g_bpark_lock);
    while (g_bpark_n && (g_bpark_n >= NVMTL_BUFPARK_N || g_bpark_bytes + b->alloc > NVMTL_BUFPARK_BYTES
                         || (age && pnow - g_bpark_ns[0] > age))) {
        ev[nev++] = g_bpark[0]; g_bpark_bytes -= g_bpark[0].alloc;
        memmove(&g_bpark[0], &g_bpark[1], (g_bpark_n - 1) * sizeof g_bpark[0]);
        memmove(&g_bpark_len[0], &g_bpark_len[1], (g_bpark_n - 1) * sizeof g_bpark_len[0]);
        memmove(&g_bpark_seq[0], &g_bpark_seq[1], (g_bpark_n - 1) * sizeof g_bpark_seq[0]);
        memmove(&g_bpark_ns[0], &g_bpark_ns[1], (g_bpark_n - 1) * sizeof g_bpark_ns[0]); g_bpark_n--; }
    g_bpark_ns[g_bpark_n] = pnow;
    g_bpark_seq[g_bpark_n] = __atomic_load_n(&g_sub_seq, __ATOMIC_ACQUIRE);
    g_bpark_len[g_bpark_n] = b->vksize ? b->vksize : b->size;
    g_bpark[g_bpark_n++] = *b; g_bpark_bytes += b->alloc;
    pthread_mutex_unlock(&g_bpark_lock);
    g_buf_gone++;
    memset(b, 0, sizeof *b);
    for (unsigned k = 0; k < nev; k++) bpark_free(&ev[k]);
    if (nev) atomic_fetch_add(&g_bpark_evicted, nev);
    atomic_fetch_add(&g_bpark_parked, 1); bpark_census("park");
    if (g_hitch_ms > 0) { nvmtl_hitch_add_ns(NVH_PARK, 0); for (unsigned k = 0; k < nev; k++) nvmtl_hitch_add_ns(NVH_EVICT, 0); }
    nvmtl_perf_note(NVP_BUFDEL, t0, n);
}

void nvmtl_vk_image_destroy(nvk_image *i)
{
    if (g_state != 1 || !i || !i->img) return;
    if (nvmtl_image_grace_on() && !t_grace_reaping) {
        nvmtl_vk_image_upload_barrier(i);   // keyed by the image's address: settled before the struct moves
        void *view = i->view; i->view = NULL;
        if (nvmtl_grace_defer(NULL, i)) { if (view) pvkDestroyImageView(g_dev, (VkImageView)view, NULL); return; }
        i->view = view;
    }
    nvmtl_vk_image_destroy_now(i);
}
static void nvmtl_vk_image_destroy_now(nvk_image *i)
{
    if (!i->img) return;
    nvmtl_vk_image_upload_barrier(i);   // an async upload must finish before we free this image's memory (#96)
    g_img_gone++;
    if (i->view) pvkDestroyImageView(g_dev, (VkImageView)i->view, NULL);
    pvkDestroyImage(g_dev, (VkImage)i->img, NULL);
    if (!i->placed && i->mem) { pvkFreeMemory(g_dev, (VkDeviceMemory)i->mem, NULL); g_alloc_bytes -= i->alloc; if (i->sysmem) { g_sys_bytes -= i->alloc; g_sys_align -= (i->alloc + 65535ull) & ~65535ull; } else { g_vram_bytes -= i->alloc; g_img_align -= (i->alloc + 65535ull) & ~65535ull; }    }
    memset(i, 0, sizeof *i);
}

static uint64_t g_res1_off, g_res1_on, g_res1_refused, g_res1_off_b, g_res1_on_b;
void nvmtl_vk_res1_stats(uint64_t out[6]) { out[0] = g_res1_off; out[1] = g_res1_on; out[2] = g_res1_refused;
    out[3] = g_res1_off_b >> 20; out[4] = g_res1_on_b >> 20; out[5] = g_res1_calls; }
int nvmtl_vk_image_reback(nvk_image *i, int to_sysmem, int a8)
{
    if (g_state != 1 || !i || !i->img || i->placed || !i->usage || i->vfl || i->samples > 1 || i->mtl_type == 4 || i->mtl_type > 7
        || (i->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return -1;
    nvmtl_vk_image_upload_barrier(i);   // migrating the image recreates its memory: drain a pending upload first
    if (!to_sysmem == !i->sysmem) return 1;
    const uint32_t t = i->mtl_type;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, (VkImageCreateFlags)i->vkflags,
        t == 7 ? VK_IMAGE_TYPE_3D : t < 2 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, (VkFormat)i->fmt,
        { i->w, i->h, t == 7 ? i->layers : 1 }, i->mips ? i->mips : 1, t == 7 ? 1 : i->layers, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, (VkImageUsageFlags)i->usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    if (nvmtl_typed_CreateImage(g_dev, &ici, NULL, &img) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, img, &mr);
    int mt = -1;
    if (to_sysmem) {
        for (uint32_t k = 0; k < g_memp.memoryTypeCount; k++)
            if ((mr.memoryTypeBits & (1u << k)) && !(g_memp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = (int)k; break; }
    } else {
        if (nvmtl_vk_vram_wall(mr.size, NULL, NULL, NULL, NULL)) { pvkDestroyImage(g_dev, img, NULL); g_res1_refused++; return 1; }
        mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (mt < 0) { pvkDestroyImage(g_dev, img, NULL); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt };
    if (pvkAllocateMemory(g_dev, &mai, NULL, &mem) != VK_SUCCESS) { pvkDestroyImage(g_dev, img, NULL); if (!to_sysmem) g_res1_refused++; return to_sysmem ? -1 : 1; }
    if (pvkBindImageMemory(g_dev, img, mem, 0) != VK_SUCCESS) { pvkFreeMemory(g_dev, mem, NULL); pvkDestroyImage(g_dev, img, NULL); return -1; }
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewType viewType = t == 0 ? VK_IMAGE_VIEW_TYPE_1D : t == 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : t == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                             : t == 5 ? VK_IMAGE_VIEW_TYPE_CUBE : t == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : t == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, viewType,
        (VkFormat)i->fmt, swz, { VK_IMAGE_ASPECT_COLOR_BIT, 0, i->mips ? i->mips : 1, 0, t == 7 ? 1 : i->layers } };
    VkImageViewUsageCreateInfo vu = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, (VkImageUsageFlags)(i->usage & ~(uint32_t)VK_IMAGE_USAGE_STORAGE_BIT) };
    if (i->storage == 2) vci.pNext = &vu;   // a view-storage sRGB image's own view (33ed311)
    if (pvkCreateImageView(g_dev, &vci, NULL, &view) != VK_SUCCESS) { pvkFreeMemory(g_dev, mem, NULL); pvkDestroyImage(g_dev, img, NULL); return -1; }
    if (i->view) pvkDestroyImageView(g_dev, (VkImageView)i->view, NULL);
    pvkDestroyImage(g_dev, (VkImage)i->img, NULL);
    if (i->mem) { pvkFreeMemory(g_dev, (VkDeviceMemory)i->mem, NULL); g_alloc_bytes -= i->alloc; if (i->sysmem) { g_sys_bytes -= i->alloc; g_sys_align -= (i->alloc + 65535ull) & ~65535ull; } else { g_vram_bytes -= i->alloc; g_img_align -= (i->alloc + 65535ull) & ~65535ull; }    }
    uint64_t aligned = (mr.size + 65535ull) & ~65535ull;
    g_alloc_bytes += mr.size;
    if (to_sysmem) { g_sys_bytes += mr.size; g_sys_align += aligned; g_res1_off++; g_res1_off_b += i->alloc; }
    else { g_vram_bytes += mr.size; g_img_align += aligned; nvmtl_vk_budget_charge(mr.size); g_res1_on++; g_res1_on_b += mr.size; }
    i->img = img; i->mem = mem; i->view = view; i->alloc = mr.size; i->sysmem = to_sysmem ? 1 : 0; i->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return 0;
}

static uint64_t g_res2_off, g_res2_on, g_res2_off_b, g_res2_on_b, g_res2_fail;
void nvmtl_vk_res2_stats(uint64_t out[5]) { out[0] = g_res2_off; out[1] = g_res2_on; out[2] = g_res2_off_b >> 20; out[3] = g_res2_on_b >> 20; out[4] = g_res2_fail; }
int nvmtl_vk_vram_room(uint64_t want) { g_budget_stale = 1; return !nvmtl_vk_vram_wall0(want, NULL, NULL, NULL, NULL); }
uint64_t nvmtl_vk_vram_free_now(void) { uint64_t f = 0; g_budget_stale = 1; (void)nvmtl_vk_vram_wall0(0, &f, NULL, NULL, NULL); return f; }
uint64_t nvmtl_vk_vram_headroom(void) { return NVMTL_VRAM_HEADROOM; }
void nvmtl_vk_bindless_rewrite(uint32_t slot, void *view)
{
    if (!g_heap_ready || !slot || slot >= NVMTL_BINDLESS_SLOTS || !view) return;
    pthread_mutex_lock(&g_heap_lock); nvmtl_heap_write_locked(slot, view); pthread_mutex_unlock(&g_heap_lock);
}
void nvmtl_vk_image_free_backing(nvk_image *o)
{
    if (g_state != 1 || !o || !o->img) return;
    if (o->view) pvkDestroyImageView(g_dev, (VkImageView)o->view, NULL);
    pvkDestroyImage(g_dev, (VkImage)o->img, NULL);
    if (o->mem) { pvkFreeMemory(g_dev, (VkDeviceMemory)o->mem, NULL); g_alloc_bytes -= o->alloc; if (o->sysmem) { g_sys_bytes -= o->alloc; g_sys_align -= (o->alloc + 65535ull) & ~65535ull; } else { g_vram_bytes -= o->alloc; g_img_align -= (o->alloc + 65535ull) & ~65535ull; }    }
    memset(o, 0, sizeof *o);
}
static __thread nvk_cmdbuf t_mig_cb; static __thread nvk_cmdbuf *t_mig_batch; static __thread unsigned t_mig_n;
int nvmtl_vk_mig_batch_begin(void)
{
    if (t_mig_batch) return 0;
    if (nvmtl_vk_cmd_begin(NULL, &t_mig_cb)) return -1;
    t_mig_batch = &t_mig_cb; t_mig_n = 0; return 0;
}
int nvmtl_vk_mig_batch_end(void)
{
    nvk_cmdbuf *c = t_mig_batch; if (!c) return 0;
    t_mig_batch = NULL;
    if (!t_mig_n) { nvmtl_vk_cmd_abandon(c); return 0; }
    const unsigned n = t_mig_n; t_mig_n = 0;
    const int r = nvmtl_vk_submit_wait(c);
    if (r) nvlog("residency (res2): evict-batch of %u copies FAILED to complete (%d) - those textures now read undefined contents", n, r);
    return r;
}
int nvmtl_vk_image_migrate(nvk_image *i, int to_sysmem, int a8, nvk_image *old_out)
{
    if (g_state != 1 || !i || !i->img || i->placed || !i->usage || i->vfl || i->samples > 1 || i->mtl_type == 4 || i->mtl_type > 7
        || (i->usage & VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) return -1;
    if (!to_sysmem == !i->sysmem) return 1;
    const uint32_t t = i->mtl_type, mips = i->mips ? i->mips : 1, nl = t == 7 ? 1 : (i->layers ? i->layers : 1);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, (VkImageCreateFlags)i->vkflags,
        t == 7 ? VK_IMAGE_TYPE_3D : t < 2 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D, (VkFormat)i->fmt,
        { i->w, i->h, t == 7 ? i->layers : 1 }, mips, nl, VK_SAMPLE_COUNT_1_BIT,
        VK_IMAGE_TILING_OPTIMAL, (VkImageUsageFlags)i->usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage img = VK_NULL_HANDLE; VkDeviceMemory mem = VK_NULL_HANDLE; VkImageView view = VK_NULL_HANDLE;
    if (nvmtl_typed_CreateImage(g_dev, &ici, NULL, &img) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, img, &mr);
    int mt = -1;
    if (to_sysmem) {
        for (uint32_t k = 0; k < g_memp.memoryTypeCount; k++)
            if ((mr.memoryTypeBits & (1u << k)) && !(g_memp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt = (int)k; break; }
    } else {
        if (nvmtl_vk_vram_wall0(mr.size, NULL, NULL, NULL, NULL)) { pvkDestroyImage(g_dev, img, NULL); return 1; }
        mt = memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }
    if (mt < 0) { pvkDestroyImage(g_dev, img, NULL); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)mt };
    if (pvkAllocateMemory(g_dev, &mai, NULL, &mem) != VK_SUCCESS) { pvkDestroyImage(g_dev, img, NULL); return to_sysmem ? -1 : 1; }
    if (pvkBindImageMemory(g_dev, img, mem, 0) != VK_SUCCESS) goto fail;
    { VkComponentMapping swz = nvmtl_a8_mapping(a8);
      VkImageViewType vt = t == 0 ? VK_IMAGE_VIEW_TYPE_1D : t == 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : t == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                         : t == 5 ? VK_IMAGE_VIEW_TYPE_CUBE : t == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : t == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
      VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, vt, (VkFormat)i->fmt, swz,
                                    { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, nl } };
      VkImageViewUsageCreateInfo vu = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL, (VkImageUsageFlags)(i->usage & ~(uint32_t)VK_IMAGE_USAGE_STORAGE_BIT) };
      if (i->storage == 2) vci.pNext = &vu;   // a view-storage sRGB image's own view (33ed311)
      if (pvkCreateImageView(g_dev, &vci, NULL, &view) != VK_SUCCESS) goto fail; }
    const VkImageLayout was = (VkImageLayout)i->layout;
    if (was != VK_IMAGE_LAYOUT_UNDEFINED) {
        nvk_cmdbuf c0, *cp = t_mig_batch; if (!cp) { if (nvmtl_vk_cmd_begin(NULL, &c0)) goto fail; cp = &c0; }
        const VkImageSubresourceRange rr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, nl };
        VkImageMemoryBarrier b[2] = {
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
              was, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)i->img, rr },
            { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
              VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rr } };
        pvkCmdPipelineBarrier(cp->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 2, b);
        VkImageCopy reg[16]; const uint32_t nm = mips > 16 ? 16 : mips;
        for (uint32_t m = 0; m < nm; m++) {
            const uint32_t w = i->w >> m ? i->w >> m : 1, h = i->h >> m ? i->h >> m : 1, d = t == 7 ? (i->layers >> m ? i->layers >> m : 1) : 1;
            reg[m] = (VkImageCopy){ { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, nl }, { 0, 0, 0 }, { VK_IMAGE_ASPECT_COLOR_BIT, m, 0, nl }, { 0, 0, 0 }, { w, h, d } };
        }
        pvkCmdCopyImage(cp->cb, (VkImage)i->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, nm, reg);
        VkImageMemoryBarrier a = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, was,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, rr };
        pvkCmdPipelineBarrier(cp->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &a);
        if (cp == &c0) { if (nvmtl_vk_submit_wait(&c0)) goto fail; }
        else t_mig_n++;
    }
    *old_out = *i;
    { uint64_t aligned = (mr.size + 65535ull) & ~65535ull;
      g_alloc_bytes += mr.size;
      if (to_sysmem) { g_sys_bytes += mr.size; g_sys_align += aligned; g_res2_off++; g_res2_off_b += i->alloc; }
      else { g_vram_bytes += mr.size; g_img_align += aligned; nvmtl_vk_budget_charge(mr.size); g_res2_on++; g_res2_on_b += mr.size; } }
    i->img = img; i->mem = mem; i->view = view; i->alloc = mr.size; i->sysmem = to_sysmem ? 1 : 0; i->layout = (uint32_t)was;
    return 0;
fail:
    if (view) pvkDestroyImageView(g_dev, view, NULL);
    pvkDestroyImage(g_dev, img, NULL);
    pvkFreeMemory(g_dev, mem, NULL);
    g_res2_fail++;
    return -1;
}

int nvmtl_vk_buffer_migrate(nvk_buffer *b, int to_sysmem)
{
    if (g_state != 1 || !b || !b->sparse || !b->buf || !b->mem) return -1;
    if (!to_sysmem == !b->sysmem) return 1;
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, (VkBuffer)b->buf, &mr);
    int mt;
    if (to_sysmem) mt = nvmtl_vk_sysmem_type(mr.memoryTypeBits);
    else { if (nvmtl_vk_vram_wall0(mr.size, NULL, NULL, NULL, NULL)) return 1; mt = nvmtl_vk_buffer_vram_type(mr.memoryTypeBits); }
    if (mt < 0) return -1;
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, mr.size, (uint32_t)mt };
    VkDeviceMemory nm = VK_NULL_HANDLE;
    if (pvkAllocateMemory(g_dev, &mai, NULL, &nm) != VK_SUCCESS) return to_sysmem ? -1 : 1;
    VkBufferCreateInfo tci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, mr.size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                               VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer tmp = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &tci, NULL, &tmp) != VK_SUCCESS) { pvkFreeMemory(g_dev, nm, NULL); return -1; }
    if (pvkBindBufferMemory(g_dev, tmp, nm, 0) != VK_SUCCESS) { pvkDestroyBuffer(g_dev, tmp, NULL); pvkFreeMemory(g_dev, nm, NULL); return -1; }
    { nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(NULL, &c)) { pvkDestroyBuffer(g_dev, tmp, NULL); pvkFreeMemory(g_dev, nm, NULL); return -1; }
      VkMemoryBarrier pre = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
      pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &pre, 0, NULL, 0, NULL);
      VkBufferCopy bc = { 0, 0, b->size < mr.size ? b->size : mr.size };
      pvkCmdCopyBuffer(c.cb, (VkBuffer)b->buf, tmp, 1, &bc);
      VkMemoryBarrier post = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
      pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &post, 0, NULL, 0, NULL);
      if (nvmtl_vk_submit_wait(&c)) { pvkDestroyBuffer(g_dev, tmp, NULL); pvkFreeMemory(g_dev, nm, NULL); return -1; } }
    pvkDestroyBuffer(g_dev, tmp, NULL);
    if (nvmtl_vk_sparse_bind((VkBuffer)b->buf, 1, nm, mr.size)) {
        if (nvmtl_vk_sparse_bind((VkBuffer)b->buf, 0, (VkDeviceMemory)b->mem, mr.size))
            nvlog("residency (res3): a %llu MB buffer is LEFT WITHOUT BACKING (rebind and restore both refused) - its GPU address will fault",
                  (unsigned long long)(mr.size >> 20));
        pvkFreeMemory(g_dev, nm, NULL);
        return -1;
    }
    pvkFreeMemory(g_dev, (VkDeviceMemory)b->mem, NULL);
    g_alloc_bytes -= b->alloc; g_alloc_bytes += mr.size;
    if (to_sysmem) { g_res3_off++; g_res3_off_b += b->alloc; }
    else { nvmtl_vk_budget_charge(mr.size); g_res3_on++; g_res3_on_b += mr.size; }
    b->mem = nm; b->alloc = mr.size; b->sysmem = to_sysmem ? 1 : 0; b->mtype1 = (uint32_t)mt + 1;
    return 0;
}

void nvmtl_vk_queue_destroy(nvk_queue *q)
{
    if (g_state != 1 || !q) return;
    nvmtl_vk_buffer_destroy(&q->stage);
    if (q->pool) pvkDestroyCommandPool(g_dev, (VkCommandPool)q->pool, NULL);
    memset(q, 0, sizeof *q);
}

size_t nvmtl_vk_allocated_bytes(void) { return g_alloc_bytes; }

/* One reusable staging buffer per thread for surface uploads, instead of a new one per upload (community patch #96).
 * The upload itself stays synchronous: #96 also deferred the fence wait to overlap uploads, but
 * nvmtl_vk_image_destroy() frees an image and its memory at once, so an app that uploads a surface and frees it
 * (IOSurface-backed textures do) would leave the GPU copying into freed memory. A thread keeps at most
 * NVMTL_UPSTAGE_KEEP bytes between uploads (a 4K BGRA frame); a larger one-off upload frees its buffer afterwards. */
#define NVMTL_UPSTAGE_KEEP (32u << 20)
typedef struct { nvk_buffer buf; nvk_cmdbuf cb; } nvmtl_upstage;
static pthread_key_t g_upstage_key; static pthread_once_t g_upstage_once = PTHREAD_ONCE_INIT;
static void nvmtl_upstage_free(void *p)
{
    nvmtl_upstage *st = p;
    if (!st) return;
    if (st->buf.buf) nvmtl_vk_buffer_destroy(&st->buf);
    free(st);
}
static void nvmtl_upstage_key_make(void)
{
    if (pthread_key_create(&g_upstage_key, nvmtl_upstage_free)) nvlog("stage: pthread_key_create FAILED");
}
static nvmtl_upstage *nvmtl_upstage_get(size_t size)
{
    pthread_once(&g_upstage_once, nvmtl_upstage_key_make);
    nvmtl_upstage *st = pthread_getspecific(g_upstage_key);
    if (!st) {
        st = calloc(1, sizeof *st);
        if (!st) return NULL;
        if (pthread_setspecific(g_upstage_key, st)) { free(st); return NULL; }
    }
    if (!st->buf.buf || st->buf.size < size) {
        if (st->buf.buf) nvmtl_vk_buffer_destroy(&st->buf);
        memset(&st->buf, 0, sizeof st->buf);
        if (nvmtl_vk_buffer_create(size, 1, &st->buf)) { memset(&st->buf, 0, sizeof st->buf); return NULL; }
    }
    return st;
}

// Streaming loads avoid serial PCIe reads from write-combining IOSurface pages.
// Ordinary memory, unaligned sources, short rows, and other CPUs retain memcpy.
#if defined(__x86_64__)
#include <immintrin.h>
__attribute__((target("sse4.1")))
static void nvmtl_copy_surface_stream(void *dst, const void *src, size_t n)
{
    const __m128i *s = (const __m128i *)src;
    char *d = (char *)dst;
    const size_t blocks = n / 64;
    _mm_mfence();
    for (size_t i = 0; i < blocks; i++, s += 4, d += 64) {
        const __m128i a = _mm_stream_load_si128((__m128i *)s);
        const __m128i b = _mm_stream_load_si128((__m128i *)s + 1);
        const __m128i c = _mm_stream_load_si128((__m128i *)s + 2);
        const __m128i e = _mm_stream_load_si128((__m128i *)s + 3);
        _mm_storeu_si128((__m128i *)d, a);
        _mm_storeu_si128((__m128i *)(d + 16), b);
        _mm_storeu_si128((__m128i *)(d + 32), c);
        _mm_storeu_si128((__m128i *)(d + 48), e);
    }
    _mm_mfence();
    memcpy(d, s, n - blocks * 64);
}
#endif
static void nvmtl_copy_from_surface(void *dst, const void *src, size_t n)
{
#if defined(__x86_64__)
    if (n >= 4096 && !((uintptr_t)src & 15) && __builtin_cpu_supports("sse4.1")) {
        nvmtl_copy_surface_stream(dst, src, n); return;
    }
#endif
    memcpy(dst, src, n);
}

// Surface-in from the IOSurface's own pages. nvmtl_vk_image_write reads every byte of the surface with the CPU, and
// WindowServer lets go of a client's surface after each frame, so for a large window most of that copy is page faults:
// a full-screen 4K Safari window took ~20 ms per upload against ~7 ms for a 3200x1800 one. Importing the locked pages as
// host memory (VK_EXT_external_memory_host) lets the copy engine read them, and the CPU never touches them (~8 ms at 4K).
// The caller holds the surface lock across the call, and the import is released before it returns.
// Non-zero when the pages cannot be imported (block-compressed format, base or length not a multiple of the import
// alignment, no host import on this device); the caller then uses nvmtl_vk_image_write.
// The image's rows live in hb from byte off on, row_bytes apart. Copies on the GPU need whole texels per row, a start on a
// texel (and, for depth/stencil, a 4-byte) boundary, and every row inside hb; block-compressed formats keep the CPU paths.
static int nvmtl_host_rows_fit(const nvk_image *img, const nvk_buffer *hb, size_t off, size_t row_bytes)
{
    const size_t bpp = img->bpp ? img->bpp : 4;
    if (!hb || !hb->buf || img->bw > 1 || img->bh > 1 || !img->w || !img->h) return 0;
    if (!row_bytes || row_bytes % bpp || row_bytes < (size_t)img->w * bpp || off % bpp || (off & 3)) return 0;
    return off <= hb->size && (size_t)(img->h - 1) * row_bytes + (size_t)img->w * bpp <= hb->size - off;
}
/* Staged host copies (gpuperf fix29c; NVMTL_HOST_STAGE=0 or the diag flag /tmp/nvmtl-nohoststage turns them off): a copy
   between an optimally tiled image and IOSurface pages in system memory goes through a linear VRAM buffer - image <-> VRAM
   buffer on the GPU's local memory, then one linear buffer <-> buffer copy over PCIe. Measured on the B580 (2560x1440
   BGRA8, 14.7 MB): image -> system memory 2.07 ms direct, 1.49 ms staged; system memory -> image 2.06 ms direct, 1.35 ms
   staged (blorp's tiled image copy writes system memory in small 2D blocks; the buffer copy streams it). Same bytes in
   the pages: only surfaces whose rows have no padding (row == width * bpp) take it. One staging buffer serves every copy
   of the process: a global barrier before each reuse orders it after the earlier copies on the queue. */
static int nvmtl_host_stage_on(void)
{
    static int env = -1;
    if (env < 0) { const char *e = getenv("NVMTL_HOST_STAGE"); env = !(e && e[0] == '0');
        nvlog("vk: host copies of IOSurface pages %s (NVMTL_HOST_STAGE=%s; /tmp/nvmtl-nohoststage turns it off at run time)",
              env ? "staged through a linear VRAM buffer" : "direct (image <-> system memory)", e ? e : "unset"); }
    return env && !nvmtl_diag_flag("nohoststage");
}
static pthread_mutex_t g_hst_lk = PTHREAD_MUTEX_INITIALIZER;
static nvk_buffer g_hst, g_hst_old[4]; static unsigned g_nhst_old;
static VkBuffer nvmtl_host_stage(size_t need)
{
    VkBuffer b = VK_NULL_HANDLE;
    pthread_mutex_lock(&g_hst_lk);
    if (!g_hst.buf || g_hst.size < need) {
        if (g_hst.buf && g_nhst_old >= 4) { pthread_mutex_unlock(&g_hst_lk); return VK_NULL_HANDLE; }
        /* a smaller one may still be in flight: kept, never freed (at most four, a few times per process) */
        if (g_hst.buf) { g_hst_old[g_nhst_old++] = g_hst; memset(&g_hst, 0, sizeof g_hst); }
        size_t sz = (need + (4u << 20) - 1) & ~(size_t)((4u << 20) - 1); if (sz < (16u << 20)) sz = 16u << 20;
        if (nvmtl_vk_buffer_create(sz, 0, &g_hst)) { memset(&g_hst, 0, sizeof g_hst); nvlog("vk: host copy staging buffer of %zu bytes REFUSED - direct copies", sz); }
        else nvlog("vk: host copy staging buffer %zu MB in VRAM (%u older kept)", sz >> 20, g_nhst_old);
    }
    if (g_hst.buf && g_hst.size >= need) b = (VkBuffer)g_hst.buf;
    pthread_mutex_unlock(&g_hst_lk);
    return b;
}
static int nvmtl_host_stage_fits(const nvk_image *img, size_t row_bytes)
{
    const size_t bpp = img->bpp ? img->bpp : 4;
    return img->bw <= 1 && img->bh <= 1 && row_bytes == (size_t)img->w * bpp && (size_t)img->h * row_bytes >= (1u << 20) && nvmtl_host_stage_on();
}
static void nvmtl_xfer_barrier(VkCommandBuffer cb, VkAccessFlags src, VkAccessFlags dst)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, src, dst };
    pvkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
/* img (in TRANSFER_SRC_OPTIMAL) -> hb + off with rows of row_bytes, through VRAM: the scan-out buffer when it was just
   filled with the same rows (via, pitch == row_bytes), else the staging buffer. 0 = done, -1 = not taken (copy directly). */
static int nvmtl_cmd_img_to_host_staged(VkCommandBuffer cb, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, VkBuffer via)
{
    if (!hb || !hb->buf || !nvmtl_host_stage_fits(img, row_bytes)) return -1;
    const size_t bytes = row_bytes * img->h;
    if (!via) {
        via = nvmtl_host_stage(bytes); if (!via) return -1;
        nvmtl_xfer_barrier(cb, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
        VkBufferImageCopy bic = { 0, img->w, 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
        pvkCmdCopyImageToBuffer(cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, via, 1, &bic);
    }
    nvmtl_xfer_barrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferCopy bc = { 0, off, bytes };
    pvkCmdCopyBuffer(cb, via, (VkBuffer)hb->buf, 1, &bc);
    return 0;
}
/* hb + off -> img (in TRANSFER_DST_OPTIMAL) through the staging buffer. 0 = done, -1 = not taken. */
static int nvmtl_cmd_host_to_img_staged(VkCommandBuffer cb, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes)
{
    if (!hb || !hb->buf || !nvmtl_host_stage_fits(img, row_bytes)) return -1;
    const size_t bytes = row_bytes * img->h;
    VkBuffer via = nvmtl_host_stage(bytes); if (!via) return -1;
    nvmtl_xfer_barrier(cb, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_WRITE_BIT);
    VkBufferCopy bc = { off, 0, bytes };
    pvkCmdCopyBuffer(cb, (VkBuffer)hb->buf, via, 1, &bc);
    nvmtl_xfer_barrier(cb, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
    VkBufferImageCopy bic = { 0, img->w, 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
    pvkCmdCopyBufferToImage(cb, via, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    return 0;
}
// NVMTL_SURFIN_ORDERED (on by default, =0 off): the surface-in copy is ordered after earlier GPU reads of the image. Its
// layout transition started from top-of-pipe with no source access, so a composite still sampling the previous frame
// was not waited for (write-after-read: a possible torn frame).
static int nvmtl_surfin_ordered_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SURFIN_ORDERED"); on = !(e && e[0] == '0'); nvlog("ordered surface-in %s (NVMTL_SURFIN_ORDERED=%s)", on ? "on" : "off", e ? e : "unset"); }
    return on;
}
// The upload half of nvmtl_vk_image_write_host, from pages the caller has imported already: off is the image's first row
// inside hb (an IOSurface plane's offset in its allocation). NVMTLObjects.m keeps one import per IOSurface across uploads
// (nvmtl_surfimp_get), so a window that changes every frame no longer pays a VM_BIND plus a page wiring per upload.
int nvmtl_vk_image_write_host_buf(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes)
{
    const size_t bpp = img->bpp ? img->bpp : 4;
    // bufferRowLength counts texels, so the row pitch has to be a whole number of them
    if (!nvmtl_host_rows_fit(img, hb, off, row_bytes)) return -1;
    nvk_cmdbuf c;
    if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, nvmtl_surfin_ordered_on() ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT) : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c.cb, nvmtl_surfin_ordered_on() ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
    VkBufferImageCopy bic = { off, (uint32_t)(row_bytes / bpp), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
    if (nvmtl_cmd_host_to_img_staged(c.cb, img, hb, off, row_bytes))
        pvkCmdCopyBufferToImage(c.cb, (VkBuffer)hb->buf, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier toRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toRead);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    return nvmtl_vk_submit_wait_at(&c, "surface upload (imported pages)");
}
int nvmtl_vk_image_write_host(nvk_queue *q, nvk_image *img, void *base, size_t row_bytes, size_t bytes)
{
    const size_t bpp = img->bpp ? img->bpp : 4;
    if (img->bw > 1 || img->bh > 1 || !row_bytes || row_bytes % bpp || row_bytes < (size_t)img->w * bpp) return -1;
    if (g_host_align) bytes = (bytes + g_host_align - 1) & ~(g_host_align - 1);
    if ((size_t)img->h * row_bytes > bytes) return -1;
    nvk_buffer hb;
    if (nvmtl_vk_buffer_import_host(base, bytes, &hb)) return -1;
    const int rc = nvmtl_vk_image_write_host_buf(q, img, &hb, 0, row_bytes);
    nvmtl_vk_buffer_destroy(&hb);
    return rc;
}
int nvmtl_vk_image_write(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes)
{
    const size_t bpp = img->bpp ? img->bpp : 4;
    const uint32_t cols = nvmtl_blocks(img->w, img->bw), rows = nvmtl_blocks(img->h, img->bh);
    if (row_bytes < (size_t)cols * bpp) { nvlog("image_write: bytesPerRow %zu < %u blocks * %zu B", row_bytes, cols, bpp); return -1; }
    const size_t tight = (size_t)cols * bpp;
    nvmtl_upstage *sl = nvmtl_upstage_get(tight * (size_t)rows);
    if (!sl) return -1;
    uint8_t *dst = (uint8_t *)sl->buf.map;
    if (row_bytes == tight) nvmtl_copy_from_surface(dst, src, tight * (size_t)rows);   /* a 4K surface is 2160 rows of 15 KB: one pass, not 2160 calls */
    else for (uint32_t y = 0; y < rows; y++)
        nvmtl_copy_from_surface(dst + (size_t)y * tight, (const uint8_t *)src + (size_t)y * row_bytes, tight);
    nvk_cmdbuf *const c = &sl->cb;
    if (nvmtl_vk_cmd_begin(q, c)) return -1;
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, nvmtl_surfin_ordered_on() ? (VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_SHADER_READ_BIT) : 0, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, nvmtl_surfin_ordered_on() ? VK_PIPELINE_STAGE_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
    VkBufferImageCopy bic = { 0, 0, 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
    pvkCmdCopyBufferToImage(c->cb, (VkBuffer)sl->buf.buf, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier toRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 1, &toRead);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    const int rc = nvmtl_vk_submit_wait_at(c, "surface upload");
    if (sl->buf.size > NVMTL_UPSTAGE_KEEP) { nvmtl_vk_buffer_destroy(&sl->buf); memset(&sl->buf, 0, sizeof sl->buf); }
    return rc;
}

int nvmtl_vk_copy_buffer(nvk_queue *q, nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size)
{
    const uint64_t t0 = nvmtl_perf_now();
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_copy_buffer(&c, src, srcOff, dst, dstOff, size)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    const int k0 = g_perf_kind; g_perf_kind |= 2;
    const int r = nvmtl_vk_submit_wait(&c);
    g_perf_kind = k0;
    nvmtl_perf_note(NVP_COPY, t0, size);
    return r;
}

int nvmtl_vk_image_read(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes)
{
    nvmtl_vk_image_upload_barrier(img);   // a readback must observe a pending async upload: finish it first
    const size_t bpp = img->bpp ? img->bpp : 4;
    const uint32_t cols = nvmtl_blocks(img->w, img->bw), rows = nvmtl_blocks(img->h, img->bh);
    if (row_bytes < (size_t)cols * bpp) { nvlog("image_read: bytesPerRow %zu < %u blocks * %zu B", row_bytes, cols, bpp); return -1; }
    nvk_buffer stage;
    if (nvmtl_vk_stage(q, (size_t)cols * rows * bpp, &stage)) return -1;
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    // SurfaceOut also reads images last used as sampled/storage images or blit
    // destinations. A completed fence does not change their image layout or
    // provide the transfer-read dependency. Transition the copied subresource.
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        (VkImageLayout)img->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &toSrc);
    img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkBufferImageCopy bic = { 0, 0, 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
    pvkCmdCopyImageToBuffer(c.cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, stage.buf, 1, &bic);
    if (nvmtl_vk_submit_wait(&c)) return -1;
    const uint8_t *src = (const uint8_t *)stage.map;
    for (uint32_t y = 0; y < rows; y++) memcpy((uint8_t *)dst + y * row_bytes, src + (size_t)y * cols * bpp, (size_t)cols * bpp);
    return 0;
}

// Surface-out straight into the IOSurface's pages: hb is the surface's imported allocation (nvmtl_surfimp_get, made
// writable), off the plane's first row in it. The staged path (nvmtl_vk_image_read) copies the image into a sysmem staging
// buffer and then memcpys it into the surface, 14.75 MB per 2560x1440 frame on the CPU and twice per WindowServer frame
// (the composite and CoreDisplay's DisplaySurface). Here the copy engine writes the surface itself: bufferRowLength is the
// surface's row pitch, so only the w*bpp bytes of each of the h rows are written and the row padding keeps its bytes, as
// with the memcpy. The import is cached-coherent system memory (ANV gives imported BOs pat.cached_coherent; Linux xe
// refuses a non-coherent PAT for a userptr), so after the fence the CPU and the kernel's own mapping of the surface (the
// IOP copy) see the pixels with no flush. Non-zero (nothing submitted) when the rows do not fit; the caller then stages.
int nvmtl_vk_image_read_host(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes)
{
    return nvmtl_vk_image_read_host_scan(q, img, hb, off, row_bytes, NULL, 0);
}
// The same, plus (frame path stage 3) a copy of the image into its scan-out buffer, linear with scan_row bytes per row,
// in the same submission: the display reads that buffer once NMXAccel flips the plane to it, after this has completed
// (the kernel's present waits for the surface's mark, released after this fence) and the L2 is flushed. hb NULL: only
// the scan-out copy (the surface took the staged readback).
int nvmtl_vk_image_read_host_scan(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, nvk_buffer *scan, size_t scan_row)
{
    nvmtl_vk_image_upload_barrier(img);   // a readback must observe a pending async upload: finish it first
    const size_t bpp = img->bpp ? img->bpp : 4;
    if (hb && !nvmtl_host_rows_fit(img, hb, off, row_bytes)) return -1;
    if (scan && (bpp != 4 || !scan->buf || (scan_row & 3) || scan_row < (size_t)img->w * 4 || scan->size < scan_row * img->h)) scan = NULL;
    if (!hb && !scan) return -1;
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        (VkImageLayout)img->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, NULL, 0, NULL, 1, &toSrc);
    img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (scan) {
        VkBufferImageCopy sic = { 0, (uint32_t)(scan_row / 4), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
        pvkCmdCopyImageToBuffer(c.cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)scan->buf, 1, &sic);
    }
    if (hb && nvmtl_cmd_img_to_host_staged(c.cb, img, hb, off, row_bytes, (scan && scan_row == row_bytes) ? (VkBuffer)scan->buf : VK_NULL_HANDLE)) {
        VkBufferImageCopy bic = { off, (uint32_t)(row_bytes / bpp), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
        pvkCmdCopyImageToBuffer(c.cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)hb->buf, 1, &bic);
    }
    // the host reads these pages next (the frame dump, WindowServer's CPU paths, the kernel's IOP copy): make the transfer
    // writes host-visible before the fence signals (Vulkan 1.3 7.9 "host write ordering"; a no-op on coherent memory).
    // The scan-out buffer is read by the display, which no barrier reaches: the kernel flushes the L2 before the flip.
    VkMemoryBarrier toHost = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &toHost, 0, NULL, 0, NULL);
    return nvmtl_vk_submit_wait_at(&c, hb ? (scan ? "surface-out + scan-out" : "surface-out (imported pages)") : "scan-out copy") ? -2 : 0;
}

// Folded surface-out (NVMTL_SURFOUT_FOLD): the same copy into the IOSurface's imported pages, recorded at the end of c,
// the command buffer that wrote img, instead of a submission of its own that waits behind whatever the queue has ahead
int nvmtl_vk_cmd_image_to_host(nvk_cmdbuf *c, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes)
{ return nvmtl_vk_cmd_image_to_host_scan(c, img, hb, off, row_bytes, NULL, 0); }
// the same, and/or into a scan-out buffer (stage 3: WindowServer's presented surface, VRAM to VRAM at the scan-out pitch)
int nvmtl_vk_cmd_image_to_host_scan(nvk_cmdbuf *c, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, nvk_buffer *scan, size_t scan_row)
{
    if (!c || !c->cb || c->in_rp || !img || !img->img || (hb && !hb->buf)) return -1;
    nvmtl_vk_image_upload_barrier(img);
    const size_t bpp = img->bpp ? img->bpp : 4;
    if (hb && !nvmtl_host_rows_fit(img, hb, off, row_bytes)) return -1;
    if (scan && (bpp != 4 || !scan->buf || (scan_row & 3) || scan_row < (size_t)img->w * 4 || scan->size < scan_row * img->h)) scan = NULL;
    if (!hb && !scan) return -1;
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
        (VkImageLayout)img->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
    img->layout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    if (scan) {
        VkBufferImageCopy sic = { 0, (uint32_t)(scan_row / 4), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
        pvkCmdCopyImageToBuffer(c->cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)scan->buf, 1, &sic);
    }
    if (hb && nvmtl_cmd_img_to_host_staged(c->cb, img, hb, off, row_bytes, (scan && scan_row == row_bytes) ? (VkBuffer)scan->buf : VK_NULL_HANDLE)) {
        VkBufferImageCopy bic = { off, (uint32_t)(row_bytes / bpp), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
        pvkCmdCopyImageToBuffer(c->cb, img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)hb->buf, 1, &bic);
    }
    // later commands of the same command buffer may write img again: they wait for the copy's read
    VkMemoryBarrier after = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_TRANSFER_READ_BIT,
                              VK_ACCESS_HOST_READ_BIT | VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &after, 0, NULL, 0, NULL);
    return 0;
}

static int nvmtl_copy_has_image(const char *what, const nvk_image *img)
{
    if (img && img->img) return 1;
    static _Atomic unsigned said;   // the first few name the texture; a per-frame copy must not flood the log
    if (atomic_fetch_add(&said, 1) >= 16) return 0;
    if (!img) { nvlog("%s: no texture - not copied", what); return 0; }
    nvlog("%s: texture %ux%u fmt %u type %u layers %u mips %u usage 0x%x placed %d sysmem %u has no VkImage - not copied",
          what, img->w, img->h, img->fmt, img->mtl_type, img->layers, img->mips, img->usage, img->placed, img->sysmem);
    return 0;
}

static void img_to(nvk_cmdbuf *c, nvk_image *img, VkImageLayout to,
                   VkAccessFlags srcA, VkAccessFlags dstA, VkPipelineStageFlags srcS, VkPipelineStageFlags dstS)
{
    if ((VkImageLayout)img->layout == to) return;
    VkImageSubresourceRange r = { nvmtl_barrier_aspect(img), 0, img->mips ? img->mips : 1, 0, img->layers ? img->layers : 1 };
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, srcA, dstA,
        (VkImageLayout)img->layout, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, r };
    nvmtl_sample_pattern pattern = c->sample_pattern;
    VkSampleLocationEXT positions[8]; VkSampleLocationsInfoEXT locations;
    if (g_sample_locations && img->samples > 1 && (r.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT)) {
        if (pattern.count != img->samples) nvmtl_sample_default(img->samples, &pattern);
        locations = nvmtl_sample_info(&pattern, positions);

        b.pNext = &locations;
    }
    pvkCmdPipelineBarrier(c->cb, srcS, dstS, 0, 0, NULL, 0, NULL, 1, &b);
    img->layout = to;
}

static void nvmtl_private_depth_layout(nvk_cmdbuf *c, nvk_image *img, VkImageLayout to)
{
    VkSampleLocationEXT positions[8]; nvmtl_sample_pattern pattern;
    if (nvmtl_sample_default(img->samples ? img->samples : 1, &pattern)) return;
    VkSampleLocationsInfoEXT locations = nvmtl_sample_info(&pattern, positions);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, &locations,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, to, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED,
        (VkImage)img->img, {nvmtl_barrier_aspect(img), 0, img->mips ? img->mips : 1, 0, img->layers ? img->layers : 1} };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    img->layout = to;
}
int nvmtl_vk_cmd_prepare_sampled_image(nvk_cmdbuf *c, nvk_image *img, int storage)
{
    if (!c || !img || c->in_rp) return -1;
    const VkImageLayout to = storage ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    if (g_sample_locations && img->samples > 1 && (nvmtl_barrier_aspect(img) & VK_IMAGE_ASPECT_DEPTH_BIT))
        nvmtl_private_depth_layout(c, img, to);
    else img_to(c, img, to, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                VK_ACCESS_SHADER_READ_BIT | (storage ? VK_ACCESS_SHADER_WRITE_BIT : 0),
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
    return 0;
}

// Asynchronous texture upload (NVMTL_ASYNC_TEX_UPLOAD, default OFF). replaceRegion otherwise submits a copy
// and drains the GPU before returning, so a GL app that uploads many textures per frame stalls once per
// upload. With the flag on, a region upload gets its OWN staging buffer and command buffer, is submitted
// without waiting, and its handle is kept on the image (nvk_image.up_token). The handle is finished (fence
// waited, staging freed) before anything overwrites, reads back or frees that image's memory -- see
// plugin/nvmtl_tex_upload.h for the invariants (unit-tested in tools/test_tex_upload.c). An earlier attempt
// (community PR #96) deferred the wait without this lifetime guard and corrupted IOSurface textures that are
// uploaded then destroyed; the guard below is what makes it safe, and it is a no-op on the synchronous path
// because up_token is only ever set here.
static int nvmtl_async_tex_upload(void)
{
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("NVMTL_ASYNC_TEX_UPLOAD");
        on = e && e[0] == '1';
        nvlog("vk: async texture upload %s (NVMTL_ASYNC_TEX_UPLOAD)", on ? "ON" : "OFF (default)");
    }
    return on;
}
typedef struct nvmtl_asyncup { nvmtl_inflight infl; nvk_buffer stage; nvk_cmdbuf cb; void (*rel)(void *); void *relctx;
                               nvk_image *img; struct nvmtl_asyncup *next; int listed; } nvmtl_asyncup;
/* Implicit-sync surface uploads hold their surface's page import busy until they land. Left to the next upload of the
   same texture they held it for as long as the texture lived: WindowServer's 16 import slots filled with busy entries
   within seconds, and every later surface-in fell back to a CPU copy that waited for the producer on WindowServer's
   thread (stutter) and attached no read fence, so the producer's next frame could overwrite the pages it was still
   copying (torn game frames). nvmtl_vk_asyncup_reap retires the uploads whose fence has signaled. */
static pthread_mutex_t g_aulist_mutex = PTHREAD_MUTEX_INITIALIZER;
static nvmtl_asyncup *g_aulist;
static pthread_mutex_t g_asyncup_mutex = PTHREAD_MUTEX_INITIALIZER;
// Atomically read and clear an image's pending async-upload handle (the consume/finish-first decision).
static uint64_t nvmtl_asyncup_take(nvk_image *img)
{
    if (!img) return 0;
    pthread_mutex_lock(&g_asyncup_mutex);
    nvmtl_up_state s = { img->up_token };
    const uint64_t token = nvmtl_up_before_consume(&s);
    img->up_token = 0;
    pthread_mutex_unlock(&g_asyncup_mutex);
    return token;
}
static void nvmtl_asyncup_store(nvk_image *img, uint64_t token)
{
    pthread_mutex_lock(&g_asyncup_mutex);
    nvmtl_up_state s = { img->up_token };
    nvmtl_up_set(&s, token);
    img->up_token = s.infl;
    pthread_mutex_unlock(&g_asyncup_mutex);
}
// Finish (fence-wait) and free a handle taken from an image. Called OUTSIDE the lock so one image's wait
// never blocks uploads to another; a no-op on 0 (which is every call on the synchronous path).
static void nvmtl_asyncup_finish(uint64_t token)
{
    if (!token) return;
    nvmtl_asyncup *au = (nvmtl_asyncup *)(uintptr_t)token;
    if (au->listed) {
        pthread_mutex_lock(&g_aulist_mutex);
        for (nvmtl_asyncup **pp = &g_aulist; *pp; pp = &(*pp)->next) if (*pp == au) { *pp = au->next; break; }
        au->listed = 0;
        pthread_mutex_unlock(&g_aulist_mutex);
    }
    if (nvmtl_vk_submit_finish(&au->infl) != 0) return;   /* not landed (device lost): the pages' import stays busy */
    nvmtl_vk_buffer_destroy(&au->stage);
    if (au->rel) au->rel(au->relctx);
    free(au);
}
// Public guard for the consumers (readback, surface-out, migration, destroy): finish a pending upload first.
void nvmtl_vk_image_upload_barrier(nvk_image *img) { nvmtl_asyncup_finish(nvmtl_asyncup_take(img)); }
static _Atomic unsigned long long g_au_reaped;
void nvmtl_vk_asyncup_reap(void)
{
    static PFN_vkGetFenceStatus gfs;
    if (!gfs && g_dev) gfs = (PFN_vkGetFenceStatus)pvkGetDeviceProcAddr(g_dev, "vkGetFenceStatus");
    if (!gfs || !__atomic_load_n(&g_aulist, __ATOMIC_ACQUIRE)) return;
    nvmtl_asyncup *done[32]; unsigned nd = 0;
    pthread_mutex_lock(&g_aulist_mutex);
    for (nvmtl_asyncup **pp = &g_aulist; *pp && nd < 32; ) {
        nvmtl_asyncup *au = *pp;
        if (!au->infl.fence || gfs(g_dev, au->infl.fence) != VK_SUCCESS) { pp = &au->next; continue; }
        bool mine = false;   /* still the image's pending upload: take it (else whoever took it finishes it) */
        pthread_mutex_lock(&g_asyncup_mutex);
        if (au->img && au->img->up_token == (uint64_t)(uintptr_t)au) { au->img->up_token = 0; mine = true; }
        pthread_mutex_unlock(&g_asyncup_mutex);
        if (!mine) { pp = &au->next; continue; }
        *pp = au->next; au->listed = 0; done[nd++] = au;
    }
    pthread_mutex_unlock(&g_aulist_mutex);
    for (unsigned i = 0; i < nd; i++) nvmtl_asyncup_finish((uint64_t)(uintptr_t)done[i]);
    if (nd) atomic_fetch_add(&g_au_reaped, nd);
}
int nvmtl_vk_image_write_host_buf_isync(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, uint32_t sid,
                                        void (*rel)(void *), void *ctx)
{
    if (!nvmtl_vk_isync() || !sid) {
        const int r = nvmtl_vk_image_write_host_buf(q, img, hb, off, row_bytes);
        if (rel) rel(ctx);
        return r;
    }
    const size_t bpp = img->bpp ? img->bpp : 4;
    if (!nvmtl_host_rows_fit(img, hb, off, row_bytes)) { if (rel) rel(ctx); return -1; }
    nvmtl_vk_asyncup_reap();
    nvmtl_vk_image_upload_barrier(img);   /* the previous upload into img has landed (its pages' import is released) */
    nvmtl_asyncup *au = (nvmtl_asyncup *)calloc(1, sizeof *au);
    if (!au) { const int r = nvmtl_vk_image_write_host_buf(q, img, hb, off, row_bytes); if (rel) rel(ctx); return r; }
    nvk_cmdbuf *c = &au->cb;
    if (nvmtl_vk_cmd_begin(q, c)) { free(au); if (rel) rel(ctx); return -1; }
    VkImageSubresourceRange range = { nvmtl_barrier_aspect(img), 0, 1, 0, 1 };
    VkImageMemoryBarrier toDst = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
        VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toDst);
    VkBufferImageCopy bic = { off, (uint32_t)(row_bytes / bpp), 0, { nvmtl_copy_aspect(img), 0, 0, 1 }, { 0, 0, 0 }, { img->w, img->h, 1 } };
    if (nvmtl_cmd_host_to_img_staged(c->cb, img, hb, off, row_bytes))
        pvkCmdCopyBufferToImage(c->cb, (VkBuffer)hb->buf, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    VkImageMemoryBarrier toRead = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_MEMORY_READ_BIT,
        VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, range };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &toRead);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    (void)nvmtl_vk_cmd_surface_use(c, sid, 0);   /* waits for the surface's writers in the job queue; attaches as a reader */
    au->rel = rel; au->relctx = ctx; au->img = img;
    /* left in flight: queue order and the barrier above put it before every later draw that samples img */
    if (nvmtl_vk_submit_begin_at(c, &au->infl, "surface upload (imported pages, implicit sync)")) { free(au); if (rel) rel(ctx); return -1; }
    nvmtl_asyncup_store(img, (uint64_t)(uintptr_t)au);
    pthread_mutex_lock(&g_aulist_mutex); au->next = g_aulist; g_aulist = au; au->listed = 1; pthread_mutex_unlock(&g_aulist_mutex);
    return 0;
}

int nvmtl_vk_image_write_region_level(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level)
{ return nvmtl_vk_image_write_region_level_layer(q, img, src, row_bytes, x, y, w, h, level, 0); }
int nvmtl_vk_image_write_region_level_layer(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer)
{
    if (layer >= (img->layers ? img->layers : 1)) { nvlog("image_write_region: layer %u — this image has %u", layer, img->layers ? img->layers : 1); return -1; }
    { const uint32_t lw = img->w >> level ? img->w >> level : 1, lh = img->h >> level ? img->h >> level : 1;
      if (!w || !h || x > lw || w > lw - x || y > lh || h > lh - y) { nvlog("image_write_region: %ux%u at (%u,%u) is outside %ux%u (level %u)", w, h, x, y, lw, lh, level); return -1; } }
    const size_t bpp = img->bpp ? img->bpp : 4;
    const uint32_t cols = nvmtl_blocks(w, img->bw), rows = nvmtl_blocks(h, img->bh);
    if (img->bw > 1 && ((x % img->bw) || (y % img->bh))) { nvlog("image_write_region: origin (%u,%u) is not block-aligned (%ux%u blocks)", x, y, img->bw, img->bh); return -1; }
    if (row_bytes < (size_t)cols * bpp) { nvlog("image_write_region: bytesPerRow %zu < %u blocks * %zu B", row_bytes, cols, bpp); return -1; }
    const size_t bytes = (size_t)cols * rows * bpp;
    // Decide sync vs async. Async gives the upload its own staging buffer + command buffer so the submission
    // can be left in flight; sync keeps the shared per-thread staging buffer and waits (the default path).
    nvmtl_up_state upst = { img->up_token };
    uint64_t finish_first = 0;
    const nvmtl_up_submit mode = nvmtl_up_before_upload(&upst, nvmtl_async_tex_upload(), &finish_first);
    nvmtl_asyncup *au = NULL;
    nvk_buffer *stageb, stage_sync;
    nvk_cmdbuf local_cb, *c;
    if (mode == NVMTL_UP_SUBMIT_ASYNC) {
        // a prior async upload to THIS image must land before we overwrite the texture or its staging
        if (finish_first) nvmtl_asyncup_finish(nvmtl_asyncup_take(img));
        au = (nvmtl_asyncup *)calloc(1, sizeof *au);
        if (!au || nvmtl_vk_buffer_create(bytes, 1, &au->stage)) { free(au); au = NULL; }   // fall back to sync
    }
    if (au) { stageb = &au->stage; c = &au->cb; }
    else {
        if (nvmtl_vk_stage(q, bytes, &stage_sync)) return -1;
        stageb = &stage_sync; c = &local_cb;
    }
    uint8_t *dst = (uint8_t *)stageb->map;
    for (uint32_t r = 0; r < rows; r++) memcpy(dst + (size_t)r * cols * bpp, (const uint8_t *)src + (size_t)r * row_bytes, (size_t)cols * bpp);
    if (nvmtl_vk_cmd_begin(q, c)) { if (au) { nvmtl_vk_buffer_destroy(&au->stage); free(au); } return -1; }
    img_to(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = { 0, 0, 0, { nvmtl_copy_aspect(img), level, img->mtl_type == 7 ? 0u : layer, 1 }, { (int32_t)x, (int32_t)y, img->mtl_type == 7 ? (int32_t)layer : 0 }, { w, h, 1 } };
    pvkCmdCopyBufferToImage(c->cb, (VkBuffer)stageb->buf, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    img_to(c, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    if (au) {
        // submit without waiting; queue order + the SHADER_READ barrier above order the copy before any later
        // draw that samples this image, and the handle is finished before a readback, destroy or re-upload.
        if (nvmtl_vk_submit_begin_at(c, &au->infl, "tex upload async")) { nvmtl_vk_buffer_destroy(&au->stage); free(au); return -1; }
        nvmtl_asyncup_store(img, (uint64_t)(uintptr_t)au);
        return 0;
    }
    return nvmtl_vk_submit_wait(c);
}

int nvmtl_vk_image_read_region_level(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes,
                                     uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level)
{ return nvmtl_vk_image_read_region_level_layer(q, img, dst, row_bytes, x, y, w, h, level, 0); }
int nvmtl_vk_image_read_region_level_layer(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes,
                                     uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer)
{
    nvmtl_vk_image_upload_barrier(img);   // a readback must observe a pending async upload: finish it first
    if (layer >= (img->layers ? img->layers : 1)) { nvlog("image_read_region: layer %u — this image has %u", layer, img->layers ? img->layers : 1); return -1; }
    { const uint32_t lw = img->w >> level ? img->w >> level : 1, lh = img->h >> level ? img->h >> level : 1;
      if (!w || !h || x > lw || w > lw - x || y > lh || h > lh - y) { nvlog("image_read_region: %ux%u at (%u,%u) is outside %ux%u (level %u)", w, h, x, y, lw, lh, level); return -1; } }
    const size_t bpp = img->bpp ? img->bpp : 4;
    const uint32_t cols = nvmtl_blocks(w, img->bw), rows = nvmtl_blocks(h, img->bh);
    if (img->bw > 1 && ((x % img->bw) || (y % img->bh))) { nvlog("image_read_region: origin (%u,%u) is not block-aligned", x, y); return -1; }
    if (row_bytes < (size_t)cols * bpp) { nvlog("image_read_region: bytesPerRow %zu < %u blocks * %zu B", row_bytes, cols, bpp); return -1; }
    nvk_buffer stage;
    if (nvmtl_vk_stage(q, (size_t)cols * rows * bpp, &stage)) return -1;
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    img_to(&c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = { 0, 0, 0, { nvmtl_copy_aspect(img), level, img->mtl_type == 7 ? 0u : layer, 1 }, { (int32_t)x, (int32_t)y, img->mtl_type == 7 ? (int32_t)layer : 0 }, { w, h, 1 } };
    pvkCmdCopyImageToBuffer(c.cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)stage.buf, 1, &bic);
    if (nvmtl_vk_submit_wait(&c)) return -1;
    const uint8_t *src = (const uint8_t *)stage.map;
    for (uint32_t r = 0; r < rows; r++) memcpy((uint8_t *)dst + (size_t)r * row_bytes, src + (size_t)r * cols * bpp, (size_t)cols * bpp);
    return 0;
}

int nvmtl_vk_copy_image(nvk_queue *q, nvk_image *src, uint32_t sx, uint32_t sy,
                        nvk_image *dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_copy_image(&c, src, sx, sy, dst, dx, dy, w, h)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    return nvmtl_vk_submit_wait(&c);
}

int nvmtl_vk_copy_buffer_to_image(nvk_queue *q, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
    return nvmtl_vk_copy_buffer_to_image_layer(q, b, off, row_bytes, img, x, y, w, h, 0);
}
int nvmtl_vk_copy_buffer_to_image_layer(nvk_queue *q, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer)
{
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_copy_buffer_to_image_layer(&c, b, off, row_bytes, img, x, y, w, h, layer)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    return nvmtl_vk_submit_wait(&c);
}

int nvmtl_vk_copy_image_to_buffer(nvk_queue *q, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes)
{
    return nvmtl_vk_copy_image_to_buffer_layer(q, img, x, y, w, h, b, off, row_bytes, 0);
}
int nvmtl_vk_copy_image_to_buffer_layer(nvk_queue *q, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t layer)
{
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_copy_image_to_buffer_layer(&c, img, x, y, w, h, b, off, row_bytes, layer)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    return nvmtl_vk_submit_wait(&c);
}

void nvmtl_vk_cmd_draw_indirect(nvk_cmdbuf *c, nvk_buffer *b, size_t off)
{ if (c->nobind) return; NVMTL_SEMU_DRAW(c, pvkCmdDrawIndirect(c->cb, (VkBuffer)b->buf, off, 1, 0)); }
/* n indirect draws whose arguments are stride bytes apart (GPU-encodable ICB runs); 1 at a time without multiDrawIndirect */
void nvmtl_vk_cmd_draw_indirect_n(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t n, uint32_t stride)
{
    if (c->nobind || !n) return;
    if (g_multidraw) { NVMTL_SEMU_DRAW(c, pvkCmdDrawIndirect(c->cb, (VkBuffer)b->buf, off, n, stride)); return; }
    for (uint32_t i = 0; i < n; i++) NVMTL_SEMU_DRAW(c, pvkCmdDrawIndirect(c->cb, (VkBuffer)b->buf, off + (size_t)i * stride, 1, 0));
}
int nvmtl_vk_cmd_draw_patches(nvk_cmdbuf *c, const nvk_pipeline *p, uint64_t fac, uint32_t istride, float scale, uint64_t cpx, uint64_t pix,
                              uint32_t start, uint32_t count, uint32_t instances, uint32_t baseInstance, nvk_buffer *ind, size_t indoff)
{
    if (c->nobind) return 0;
    if (!p || !p->tess || !fac) return -1;
    struct { uint64_t fac, cpx, pix; uint32_t istride; float scale; uint64_t vb[NVMTL_TESS_NVB]; } pc;
    _Static_assert(sizeof pc == NVMTL_TESS_PC_BYTES, "the TESC's push-constant block is 96 bytes, offsets 0/8/16/24/28/32");
    memset(&pc, 0, sizeof pc); pc.fac = fac; pc.cpx = cpx; pc.pix = pix; pc.istride = istride; pc.scale = scale;
    nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
    for (uint32_t k = 0; k < p->tess_nvb && k < NVMTL_TESS_NVB; k++) {
        uint32_t i = p->tess_vb[k]; nvmtl_slot *sl = t && i < NVMTL_NVIN ? &t->s[0][i] : NULL;
        if (!sl || sl->kind != NVMTL_SLOT_BUF || !sl->h || !sl->address) { static int said; if (said++ < 8) nvlog("tess draw: the pipeline pulls "
            "control points from vertex buffer index %u and nothing is bound there — NOT drawn", i); return -1; }
        pc.vb[k] = sl->address + sl->off;
        if (pc.vb[k] & 3) { static int said2; if (said2++ < 8) nvlog("tess draw: vertex buffer index %u is bound at an offset that is not "
            "4-byte aligned — NOT drawn (the TESC loads whole words)", i); return -1; }
    }
    pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, 0, sizeof pc, &pc);
    pvkCmdSetPrimitiveTopology(c->cb, VK_PRIMITIVE_TOPOLOGY_PATCH_LIST);
    pvkCmdSetPrimitiveRestartEnable(c->cb, VK_FALSE);
    if (ind) pvkCmdDrawIndirect(c->cb, (VkBuffer)ind->buf, indoff, 1, 0);
    else pvkCmdDraw(c->cb, count, instances ? instances : 1, start, baseInstance);
    { static int told; if (!told++) nvlog("tess: first patch draw in this process (%s)", ind ? "indirect" : "direct"); }
    return 0;
}
void nvmtl_vk_cmd_draw_indexed_indirect(nvk_cmdbuf *c, nvk_buffer *idx, size_t idxOff, int is32, nvk_buffer *b, size_t off)
{
    if (!nvmtl_index_range_ok(idx, idxOff)) return;
    nvmtl_bind_index(c, idx, idxOff, is32);
    if (c->nobind) return;
    NVMTL_SEMU_DRAW(c, pvkCmdDrawIndexedIndirect(c->cb, (VkBuffer)b->buf, off, 1, 0));
}
void nvmtl_vk_cmd_draw_indexed_indirect_n(nvk_cmdbuf *c, nvk_buffer *idx, size_t idxOff, int is32, nvk_buffer *b, size_t off,
                                          uint32_t n, uint32_t stride)
{
    if (!n || !nvmtl_index_range_ok(idx, idxOff)) return;
    nvmtl_bind_index(c, idx, idxOff, is32);
    if (c->nobind) return;
    if (g_multidraw) { NVMTL_SEMU_DRAW(c, pvkCmdDrawIndexedIndirect(c->cb, (VkBuffer)b->buf, off, n, stride)); return; }
    for (uint32_t i = 0; i < n; i++) NVMTL_SEMU_DRAW(c, pvkCmdDrawIndexedIndirect(c->cb, (VkBuffer)b->buf, off + (size_t)i * stride, 1, 0));
}
void nvmtl_vk_cmd_dispatch_indirect(nvk_cmdbuf *c, nvk_pipeline *p, nvk_buffer *b, size_t off)
{
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipeline)p->pipe);
    if (c->nobind) return;
    if (p->pc_table[0].count) {   // compute2 (see nvmtl_vk_cmd_dispatch)
        uint32_t pc[12 + 2 * NVMTL_PC_TABLE_MAX] = {0}; const uint32_t pcb = nvmtl_cs_pc_values(c, p, pc);
        pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcb, pc);
    }
    pvkCmdDispatchIndirect(c->cb, (VkBuffer)b->buf, off);
    nvmtl_order_barrier(c);
}

void nvmtl_vk_cmd_barrier(nvk_cmdbuf *c)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
}
/* compute2 NVMTL_CS_BAR (default 1 with synchronization2; 0 = prod7's synchronization-1 masks). Metal: a serial compute
   pass "runs its commands serially" (Resource synchronization); between two dispatches the second must see the first's
   writes. prod7 asked for SHADER_WRITE -> SHADER_READ|SHADER_WRITE|UNIFORM_READ, which ANV turns into HDC + untyped
   dataport flush (src), constant-cache and texture-cache invalidation (dst SHADER_READ, UNIFORM_READ) and the
   compute->compute stall (genX_cmd_buffer.c anv_pipe_{flush,invalidate}_bits_for_access_flags). Every kernel memory
   access of this translator goes through the untyped/typed dataport (brw_from_nir.cpp: load/store_global* use
   MEMORY_MODE_UNTYPED; the only Uniform block is the address table, written by the CPU before the submit and
   invalidated with STATE_BASE_ADDRESS at each command buffer's start), so the invalidations are needed only for what the
   next kernel reads through the sampler (SHADER_SAMPLED_READ) or a GPU-written Uniform block (UNIFORM_READ): p->cs_acc.
   The stall and the dataport flush stay: they are what makes the second dispatch see the first's writes. */
static int nvmtl_cs_bar_narrow(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NVMTL_CS_BAR"); v = g_sync2 && !(e && e[0] == '0');
        nvlog("cs-bar: compute->compute barriers %s (NVMTL_CS_BAR=%s, synchronization2 %d)", v ? "NARROW (storage write -> storage read/write, + sampled/uniform read only for kernels that read so)" : "as prod7", e ? e : "unset", g_sync2); }
    return v;
}
void nvmtl_vk_cmd_barrier_c2c_p(nvk_cmdbuf *c, const nvk_pipeline *p)
{
    if (!p || !nvmtl_cs_bar_narrow()) { nvmtl_vk_cmd_barrier_c2c(c); return; }
    VkMemoryBarrier2 mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
        VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT
            | ((p->cs_acc & NVMTL_CS_ACC_SAMPLED) ? VK_ACCESS_2_SHADER_SAMPLED_READ_BIT : 0)
            | ((p->cs_acc & NVMTL_CS_ACC_UNIFORM) ? VK_ACCESS_2_UNIFORM_READ_BIT : 0) };
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &mb, 0, NULL, 0, NULL };
    pvkCmdPipelineBarrier2_(c->cb, &di);
}
/* compute2 NVMTL_CS_HAZARD=1 (opt-in, default off): within a serial compute pass, no barrier between two dispatches that
   cannot conflict. Eligible kernels: nvmtl_cs_reach (every address from a pushed table qword). A dispatch reaches the
   whole buffer bound at each of its buffer entries ([address, address + size)); conflict = it reaches a range written
   since the last barrier, or writes a range read since then. Overlap is by GPU address, so two Metal buffers on the
   same memory (heap aliasing, sub-allocation) conflict as they should. Ineligible kernels and any doubt keep the
   barrier, and the state then counts as "anything may have been touched". */
int nvmtl_vk_cs_hazard_on(void)
{
    static int v = -1;
    if (v < 0) { const char *e = getenv("NVMTL_CS_HAZARD"); v = e && e[0] == '1';
        nvlog("cs-hazard: serial dispatch barriers %s (NVMTL_CS_HAZARD=%s)", v ? "only on a buffer conflict for eligible kernels" : "before every dispatch (Metal serial order, prod7)", e ? e : "unset"); }
    return v;
}
int nvmtl_vk_cs_hazard(nvk_cmdbuf *c, const nvk_pipeline *p, int forced)
{
    const nvmtl_bindtable *t = (const nvmtl_bindtable *)c->table;
    uint64_t lo[NVMTL_PC_TABLE_MAX], hi[NVMTL_PC_TABLE_MAX]; uint32_t n = 0;
    int known = p && p->cs_reach && t != NULL;
    for (uint32_t k = 0; known && k < p->pc_table[0].count; k++) {
        const uint32_t idx = p->pc_table[0].index[k];
        if (idx >= NVMTL_NBUF) continue;   /* a texture entry is a size, not memory (no image can be declared) */
        const nvmtl_slot *s = &t->s[0][idx];
        if (s->kind != NVMTL_SLOT_BUF || !s->address) { known = 0; break; }   /* the shared dummy: treat as unknown */
        lo[n] = s->address; hi[n] = s->address + s->size + s->vextra; n++;
    }
    int conflict = forced || !known || c->hz_unknown;
    for (uint32_t k = 0; !conflict && k < n; k++)
        for (uint32_t j = 0; j < c->hz_n; j++)
            if (lo[k] < c->hz_hi[j] && c->hz_lo[j] < hi[k] && (p->cs_wr || ((c->hz_wr >> j) & 1))) { conflict = 1; break; }
    if (!conflict && c->hz_n + n > 32) conflict = 1;
    if (conflict) { c->hz_n = 0; c->hz_wr = 0; c->hz_unknown = !known; }
    for (uint32_t k = 0; known && k < n && c->hz_n < 32; k++) {
        c->hz_lo[c->hz_n] = lo[k]; c->hz_hi[c->hz_n] = hi[k];
        if (p->cs_wr) c->hz_wr |= 1u << c->hz_n;
        c->hz_n++;
    }
    return conflict;
}
void nvmtl_vk_cmd_barrier_c2c(nvk_cmdbuf *c)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_UNIFORM_READ_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                          0, 1, &mb, 0, NULL, 0, NULL);
}

int nvmtl_vk_rt_available(void) { return nvmtl_vk_init() ? 0 : g_rt; }
int nvmtl_vk_barycentrics_available(void) { return nvmtl_vk_init() ? 0 : g_bary; }
uint64_t nvmtl_vk_rt_limit(int which)
{
    if (nvmtl_vk_init() || !g_rt) return 0;
    return which == 0 ? g_asprops.maxGeometryCount : which == 1 ? g_asprops.maxInstanceCount : g_asprops.maxPrimitiveCount;
}
int nvmtl_vk_cmd_transform_matrix(nvk_cmdbuf *c, const nvk_buffer *src, uint64_t src_off, int rowmajor, nvk_buffer *dst, uint64_t dst_off)
{
    if (!c || !c->cb || c->in_rp || !src || !src->buf || !dst || !dst->buf || src_off > src->size || src->size - src_off < 48 ||
        dst_off > dst->size || dst->size - dst_off < 48)
        return -1;
    /* row r, column k of the 3x4 result: row-major source word r*4+k; column-major (Metal's MTLPackedFloat4x3, columns
       of 3) word k*3+r. 12 four-byte regions (vkCmdCopyBuffer has no alignment rule for its regions). */
    VkBufferCopy r[12];
    for (uint32_t row = 0; row < 3; row++)
        for (uint32_t k = 0; k < 4; k++)
            r[row * 4 + k] = (VkBufferCopy){ src_off + 4 * (rowmajor ? row * 4 + k : k * 3 + row), dst_off + 4 * (row * 4 + k), 4 };
    pvkCmdCopyBuffer(c->cb, (VkBuffer)src->buf, (VkBuffer)dst->buf, rowmajor ? 1 : 12, rowmajor ? &(VkBufferCopy){ src_off, dst_off, 48 } : r);
    return 0;
}

static uint32_t nvmtl_fill_geometries(const nvmtl_geom *g, uint32_t n, uint64_t instances_addr, uint32_t instance_count, int instance,
                                      VkAccelerationStructureGeometryKHR *out, uint32_t *counts)
{
    if (instance) {
        out[0] = (VkAccelerationStructureGeometryKHR){ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, NULL, VK_GEOMETRY_TYPE_INSTANCES_KHR };
        out[0].geometry.instances = (VkAccelerationStructureGeometryInstancesDataKHR){
            VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR, NULL, VK_FALSE, { .deviceAddress = instances_addr } };
        counts[0] = instance_count;
        return 1;
    }
    for (uint32_t i = 0; i < n; i++) {
        out[i] = (VkAccelerationStructureGeometryKHR){ VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR, NULL,
                                                        g[i].is_boxes ? VK_GEOMETRY_TYPE_AABBS_KHR : VK_GEOMETRY_TYPE_TRIANGLES_KHR };
        if (g[i].is_boxes)
            out[i].geometry.aabbs = (VkAccelerationStructureGeometryAabbsDataKHR){
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR, NULL, { .deviceAddress = g[i].boxes }, g[i].box_stride };
        else
            out[i].geometry.triangles = (VkAccelerationStructureGeometryTrianglesDataKHR){
                VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR, NULL, (VkFormat)g[i].vertex_format,
                { .deviceAddress = g[i].vertices }, g[i].stride, g[i].max_vertex, (VkIndexType)g[i].index_type,
                { .deviceAddress = g[i].indices }, { .deviceAddress = g[i].transform } };
        out[i].flags = g[i].opaque ? VK_GEOMETRY_OPAQUE_BIT_KHR : 0;
        counts[i] = g[i].count;
    }
    return n;
}

uint32_t nvmtl_vk_accel_flags(uint64_t u)
{
    /* MTLAccelerationStructureUsage: Refit 1, PreferFastBuild 2, ExtendedLimits 4 (no Vulkan limit to lift),
       PreferFastIntersection 16, MinimizeMemory 32. ALLOW_COMPACTION always: writeCompactedAccelerationStructureSize and
       copyAndCompact may follow any build (ANV compacts only without ALLOW_UPDATE and reports the full size otherwise). */
    uint32_t f = VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR;
    if (u & 1u) f |= VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR;
    f |= ((u & 2u) && !(u & 16u)) ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    if (u & 32u) f |= VK_BUILD_ACCELERATION_STRUCTURE_LOW_MEMORY_BIT_KHR;
    return f;
}

static uint64_t nvmtl_accel_shape(const nvmtl_geom *g, uint32_t n, int instance, uint32_t instance_count)
{
    uint64_t h = 1469598103934665603ull;   /* FNV-1a over the geometry count, kinds and primitive counts */
    const uint32_t head[2] = { instance ? 0x80000000u : n, instance ? instance_count : 0u };
    for (int k = 0; k < 2; k++) { h ^= head[k]; h *= 1099511628211ull; }
    for (uint32_t i = 0; !instance && i < n; i++) {
        h ^= g[i].count; h *= 1099511628211ull;
        const uint32_t kind[3] = { g[i].is_boxes | (g[i].opaque << 1) | ((g[i].transform != 0) << 2), g[i].index_type, g[i].vertex_format };
        for (int k = 0; k < 3; k++) { h ^= kind[k]; h *= 1099511628211ull; }
    }
    return h;
}

int nvmtl_vk_accel_sizes(const nvmtl_geom *g, uint32_t n, int instance, uint32_t instance_count, uint32_t flags,
                         size_t *as_size, size_t *scratch_size, size_t *update_scratch_size)
{
    if (nvmtl_vk_init() || !g_rt) return -1;
    if (!instance && n > g_asprops.maxGeometryCount) { nvlog("vk: %u geometries in one acceleration structure — more than the device's %llu", n, (unsigned long long)g_asprops.maxGeometryCount); return -1; }
    /* per call, not static: Metal lets several threads ask for sizes and encode builds at once */
    const uint32_t cap = instance || !n ? 1 : n;
    VkAccelerationStructureGeometryKHR *geoms = calloc(cap, sizeof *geoms); uint32_t *counts = calloc(cap, sizeof *counts);
    if (!geoms || !counts) { free(geoms); free(counts); return -1; }
    uint32_t ng = nvmtl_fill_geometries(g, n, 0, instance_count, instance, geoms, counts);
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, NULL,
        instance ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        flags, VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        VK_NULL_HANDLE, VK_NULL_HANDLE, ng, geoms, NULL, { .deviceAddress = 0 } };
    VkAccelerationStructureBuildSizesInfoKHR sz = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR };
    pvkGetAccelerationStructureBuildSizesKHR(g_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &bi, counts, &sz);
    free(geoms); free(counts);
    *as_size = sz.accelerationStructureSize; *scratch_size = sz.buildScratchSize;
    if (update_scratch_size) *update_scratch_size = sz.updateScratchSize;
    return 0;
}

int nvmtl_vk_accel_create(nvk_buffer *backing, size_t offset, size_t size, int instance, nvk_accel *out)
{
    if (nvmtl_vk_init() || !g_rt) return -1;
    memset(out, 0, sizeof *out);
    if ((offset & 255) || offset > backing->size || size > backing->size - offset) return -1;   /* Vulkan: offset multiple of 256 */
    VkAccelerationStructureCreateInfoKHR ci = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR, NULL, 0, (VkBuffer)backing->buf, offset, size,
        instance ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, 0 };
    VkAccelerationStructureKHR as;
    VKCK(pvkCreateAccelerationStructureKHR(g_dev, &ci, NULL, &as), "vkCreateAccelerationStructureKHR");
    VkAccelerationStructureDeviceAddressInfoKHR ai = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR, NULL, as };
    out->as = as; out->addr = pvkGetAccelerationStructureDeviceAddressKHR(g_dev, &ai); out->size = size; out->instance = instance;
    return 0;
}

void nvmtl_vk_accel_destroy(nvk_accel *a)
{
    if (a->as && pvkDestroyAccelerationStructureKHR) pvkDestroyAccelerationStructureKHR(g_dev, (VkAccelerationStructureKHR)a->as, NULL);
    a->as = NULL;
}

int nvmtl_vk_accel_can_update(const nvk_accel *a, const nvmtl_geom *g, uint32_t n, uint32_t instance_count)
{
    return a && a->as && a->built && (a->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_UPDATE_BIT_KHR) &&
           a->shape == nvmtl_accel_shape(g, n, a->instance, instance_count);
}

int nvmtl_vk_cmd_build_accel(nvk_cmdbuf *c, nvk_accel *dst, const nvmtl_geom *g, uint32_t n, uint64_t instances_addr, uint32_t instance_count,
                             uint64_t scratch_addr, uint32_t flags, nvk_accel *update_src)
{
    if (!g_rt || !dst->as) return -1;
    if (!dst->instance && n > g_asprops.maxGeometryCount) return -1;
    if (update_src && (!nvmtl_vk_accel_can_update(update_src, g, n, instance_count) || update_src->instance != dst->instance)) return -1;
    const uint32_t cap = dst->instance || !n ? 1 : n;
    VkAccelerationStructureGeometryKHR *geoms = calloc(cap, sizeof *geoms); uint32_t *counts = calloc(cap, sizeof *counts);
    VkAccelerationStructureBuildRangeInfoKHR *ranges = calloc(cap, sizeof *ranges);
    if (!geoms || !counts || !ranges) { free(geoms); free(counts); free(ranges); return -1; }
    uint32_t ng = nvmtl_fill_geometries(g, n, instances_addr, instance_count, dst->instance, geoms, counts);
    for (uint32_t i = 0; i < ng; i++) ranges[i] = (VkAccelerationStructureBuildRangeInfoKHR){ counts[i], 0, 0, 0 };
    /* an update repeats the flags of the build it updates (Vulkan: srcAccelerationStructure's last build flags) */
    const uint32_t f = update_src ? update_src->flags : flags;
    VkAccelerationStructureBuildGeometryInfoKHR bi = { VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR, NULL,
        dst->instance ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR,
        f, update_src ? VK_BUILD_ACCELERATION_STRUCTURE_MODE_UPDATE_KHR : VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR,
        update_src ? (VkAccelerationStructureKHR)update_src->as : VK_NULL_HANDLE, (VkAccelerationStructureKHR)dst->as, ng, geoms, NULL,
        { .deviceAddress = scratch_addr } };
    const VkAccelerationStructureBuildRangeInfoKHR *rp = ranges;
    nvmtl_vk_cmd_barrier(c);   /* geometry / instance data and the scratch written by earlier commands of c */
    pvkCmdBuildAccelerationStructuresKHR((VkCommandBuffer)c->cb, 1, &bi, &rp);   /* the arrays are read at record time */
    free(geoms); free(counts); free(ranges);
    nvmtl_vk_cmd_barrier(c);
    dst->flags = f; dst->built = 1; dst->shape = nvmtl_accel_shape(g, n, dst->instance, instance_count);
    return 0;
}

int nvmtl_vk_cmd_copy_accel(nvk_cmdbuf *c, nvk_accel *src, nvk_accel *dst, int compact)
{
    if (!g_rt || !src->as || !dst->as || !pvkCmdCopyAccelerationStructureKHR) return -1;
    /* COMPACT needs a source built with ALLOW_COMPACTION (every build here); ANV copies the used size either way */
    compact = compact && src->built && (src->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR);
    VkCopyAccelerationStructureInfoKHR ci = { VK_STRUCTURE_TYPE_COPY_ACCELERATION_STRUCTURE_INFO_KHR, NULL,
        (VkAccelerationStructureKHR)src->as, (VkAccelerationStructureKHR)dst->as,
        compact ? VK_COPY_ACCELERATION_STRUCTURE_MODE_COMPACT_KHR : VK_COPY_ACCELERATION_STRUCTURE_MODE_CLONE_KHR };
    nvmtl_vk_cmd_barrier(c);
    pvkCmdCopyAccelerationStructureKHR((VkCommandBuffer)c->cb, &ci);
    nvmtl_vk_cmd_barrier(c);
    dst->flags = src->flags; dst->built = src->built; dst->shape = src->shape;
    return 0;
}

/* compacted-size queries: pages of 32 per command buffer, destroyed when it retires */
enum { NVMTL_ASQ_PAGE = 32 };
typedef struct nvmtl_asq_page { struct nvmtl_asq_page *next; VkQueryPool pool; } nvmtl_asq_page;
static void nvmtl_vk_cmd_retire_asq(nvk_cmdbuf *c)
{
    nvmtl_asq_page *p = c->asq_pages;
    while (p) { nvmtl_asq_page *next = p->next; pvkDestroyQueryPool(g_dev, p->pool, NULL); free(p); p = next; }
    c->asq_pages = NULL; c->asq_count = 0;
}

#include "nvmtl_rt_sizeadd_spv.h"
static pthread_mutex_t g_rt_sizeadd_lock = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline g_rt_sizeadd_pipeline;
static VkPipelineLayout g_rt_sizeadd_layout;
static int g_rt_sizeadd_failed;
typedef struct { uint64_t src, dst; uint32_t add, bytes; } nvmtl_sizeadd_args;

static int nvmtl_rt_sizeadd_init(void)
{
    int rc = -1;
    pthread_mutex_lock(&g_rt_sizeadd_lock);
    if (g_rt_sizeadd_pipeline) { rc = 0; goto done; }
    if (g_rt_sizeadd_failed) goto done;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof nvmtl_rt_sizeadd_spv, nvmtl_rt_sizeadd_spv };
    if (pvkCreateShaderModule(g_dev, &sm, NULL, &mod) != VK_SUCCESS) { g_rt_sizeadd_failed = 1; goto done; }
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(nvmtl_sizeadd_args) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pvkCreatePipelineLayout(g_dev, &li, NULL, &layout) == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
                                          stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS) {
            g_rt_sizeadd_layout = layout; g_rt_sizeadd_pipeline = pipeline; rc = 0;
        } else {
            if (pipeline) pvkDestroyPipeline(g_dev, pipeline, NULL);
            pvkDestroyPipelineLayout(g_dev, layout, NULL);
        }
    }
    pvkDestroyShaderModule(g_dev, mod, NULL);
    if (rc) { g_rt_sizeadd_failed = 1; nvlog("vk: compacted size pass pipeline FAILED"); }
done:
    pthread_mutex_unlock(&g_rt_sizeadd_lock);
    return rc;
}

/* rtopt5: whether primitive structures carry their one-instance top-level wrapper (nvmtl_rt_layout.h), decided once */
static pthread_once_t g_rt_wrap_once = PTHREAD_ONCE_INIT;
static uint32_t g_rt_wrap; static size_t g_rt_wrap_scratch;
static void nvmtl_rt_wrap_decide(void)
{
    const char *e = getenv("NVMTL_RT_WRAP");
    if (e && e[0] == '0') { nvlog("rt: NVMTL_RT_WRAP=0 - primitive structures are traced as bottom-level roots (rtopt4 layout)"); return; }
    size_t as = 0, sc = 0, up = 0;
    if (nvmtl_vk_accel_sizes(NULL, 0, 1, 1, nvmtl_vk_accel_flags(0), &as, &sc, &up)) { nvlog("rt: one-instance top-level size query FAILED - no wrappers (rtopt4 layout)"); return; }
    if (as > NVMTL_AS_WRAP) { nvlog("rt: a one-instance top-level structure is %zu bytes, more than NVMTL_AS_WRAP %u - no wrappers (rtopt4 layout)", as, NVMTL_AS_WRAP); return; }
    g_rt_wrap = NVMTL_AS_WRAP; g_rt_wrap_scratch = sc;
    nvlog("rt: primitive structures carry a one-instance top-level wrapper (%zu of %u bytes, scratch %zu)", as, NVMTL_AS_WRAP, sc);
}
uint32_t nvmtl_vk_rt_wrap(size_t *scratch)
{
    if (nvmtl_vk_init() || !g_rt) { if (scratch) *scratch = 0; return 0; }
    pthread_once(&g_rt_wrap_once, nvmtl_rt_wrap_decide);
    if (scratch) *scratch = g_rt_wrap_scratch;
    return g_rt_wrap;
}

int nvmtl_vk_cmd_write_compacted_size(nvk_cmdbuf *c, nvk_accel *a, nvk_buffer *dst, size_t off, uint32_t bytes, nvk_buffer *stage)
{ return nvmtl_vk_cmd_write_compacted_size_add(c, a, dst, off, bytes, stage, 0); }
int nvmtl_vk_cmd_write_compacted_size_add(nvk_cmdbuf *c, nvk_accel *a, nvk_buffer *dst, size_t off, uint32_t bytes, nvk_buffer *stage, uint32_t add)
{
    if (!g_rt || !c || !c->cb || c->in_rp || !a || !a->as || !a->built || !(a->flags & VK_BUILD_ACCELERATION_STRUCTURE_ALLOW_COMPACTION_BIT_KHR) ||
        !pvkCmdWriteAccelerationStructuresPropertiesKHR || !dst || !dst->buf || (bytes != 4 && bytes != 8) || off > dst->size || dst->size - off < bytes ||
        !stage || !stage->buf || stage->size < 8)
        return -1;
    const uint32_t slot = c->asq_count % NVMTL_ASQ_PAGE;
    if (!slot) {
        nvmtl_asq_page *p = calloc(1, sizeof *p);
        if (!p) return -1;
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
            VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, NVMTL_ASQ_PAGE, 0 };
        if (pvkCreateQueryPool(g_dev, &qi, NULL, &p->pool) != VK_SUCCESS) { free(p); return -1; }
        p->next = c->asq_pages; c->asq_pages = p;
    }
    VkQueryPool pool = ((nvmtl_asq_page *)c->asq_pages)->pool;
    /* the result lands in the caller's 8-byte staging buffer (held until c retires), then a buffer copy puts the 4 or 8
       bytes Metal asked for at any offset (vkCmdCopyQueryPoolResults wants 4- or 8-aligned destinations) */
    pvkCmdResetQueryPool(c->cb, pool, slot, 1);
    VkAccelerationStructureKHR h = (VkAccelerationStructureKHR)a->as;
    pvkCmdWriteAccelerationStructuresPropertiesKHR(c->cb, 1, &h, VK_QUERY_TYPE_ACCELERATION_STRUCTURE_COMPACTED_SIZE_KHR, pool, slot);
    pvkCmdCopyQueryPoolResults(c->cb, pool, slot, 1, (VkBuffer)stage->buf, 0, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    nvmtl_vk_cmd_barrier(c);
    const uint64_t sa = add ? nvmtl_vk_buffer_address(stage) : 0, da = add ? nvmtl_vk_buffer_address(dst) : 0;
    if (add && sa && da && !(off & 3) && !nvmtl_rt_sizeadd_init()) {
        /* rtopt5: + the wrapper (Metal's size of a primitive structure), 4 or 8 bytes at a 4-aligned offset */
        nvmtl_sizeadd_args pa = { sa, da + off, add, bytes };
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rt_sizeadd_pipeline);
        pvkCmdPushConstants(c->cb, g_rt_sizeadd_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pa, &pa);
        pvkCmdDispatch(c->cb, 1, 1, 1);
    } else {
        if (add) nvlog("writeCompactedAccelerationStructureSize: the + wrapper pass could not run - the size written lacks %u bytes", add);
        VkBufferCopy r = { 0, off, bytes };   /* little-endian: the low 4 bytes are the 32-bit size */
        pvkCmdCopyBuffer(c->cb, (VkBuffer)stage->buf, (VkBuffer)dst->buf, 1, &r);
    }
    nvmtl_vk_cmd_barrier(c);
    c->asq_count++;
    return 0;
}

#include "nvmtl_rt_instances_spv.h"
static pthread_mutex_t g_rt_instances_lock = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline g_rt_instances_pipeline;
static VkPipelineLayout g_rt_instances_layout;
static int g_rt_instances_failed;

static int nvmtl_rt_instances_init(void)
{
    int rc = -1;
    pthread_mutex_lock(&g_rt_instances_lock);
    if (g_rt_instances_pipeline) { rc = 0; goto done; }
    if (g_rt_instances_failed) goto done;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof nvmtl_rt_instances_spv, nvmtl_rt_instances_spv };
    if (pvkCreateShaderModule(g_dev, &sm, NULL, &mod) != VK_SUCCESS) { g_rt_instances_failed = 1; goto done; }
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(nvmtl_inst_args) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pvkCreatePipelineLayout(g_dev, &li, NULL, &layout) == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
        /* no sets, as the visibility sum: valid in either descriptor model */
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
                                          stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS) {
            g_rt_instances_layout = layout; g_rt_instances_pipeline = pipeline; rc = 0;
        } else {
            if (pipeline) pvkDestroyPipeline(g_dev, pipeline, NULL);
            pvkDestroyPipelineLayout(g_dev, layout, NULL);
        }
    }
    pvkDestroyShaderModule(g_dev, mod, NULL);
    if (rc) { g_rt_instances_failed = 1; nvlog("vk: instance conversion pipeline FAILED - instance acceleration structures are refused"); }
done:
    pthread_mutex_unlock(&g_rt_instances_lock);
    return rc;
}

int nvmtl_vk_cmd_convert_instances(nvk_cmdbuf *c, const nvmtl_inst_args *a)
{
    _Static_assert(sizeof(nvmtl_inst_args) == 72 && offsetof(nvmtl_inst_args, flags) == 64, "push constant block of nvmtl_rt_instances.comp");
    if (!c || !c->cb || c->in_rp || !a || !a->dst || (a->max_count && !a->src) || (a->stride & 3) || nvmtl_rt_instances_init()) return -1;
    if (!a->max_count) return 0;
    nvmtl_vk_cmd_barrier(c);   /* descriptors, count and table written by earlier commands of c (blit, compute) */
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rt_instances_pipeline);
    pvkCmdPushConstants(c->cb, g_rt_instances_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *a, a);   /* the shader's block: 6 addresses, 6 words */
    pvkCmdDispatch(c->cb, (a->max_count + 63u) / 64u, 1, 1);
    nvmtl_vk_cmd_barrier(c);   /* the build reads what it wrote */
    return 0;
}

#include "nvmtl_rt_curves_spv.h"
static pthread_mutex_t g_rt_curves_lock = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline g_rt_curves_pipeline;
static VkPipelineLayout g_rt_curves_layout;
static int g_rt_curves_failed;

static int nvmtl_rt_curves_init(void)
{
    int rc = -1;
    pthread_mutex_lock(&g_rt_curves_lock);
    if (g_rt_curves_pipeline) { rc = 0; goto done; }
    if (g_rt_curves_failed) goto done;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof nvmtl_rt_curves_spv, nvmtl_rt_curves_spv };
    if (pvkCreateShaderModule(g_dev, &sm, NULL, &mod) != VK_SUCCESS) { g_rt_curves_failed = 1; goto done; }
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(nvmtl_curve_args) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pvkCreatePipelineLayout(g_dev, &li, NULL, &layout) == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
                                          stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS) {
            g_rt_curves_layout = layout; g_rt_curves_pipeline = pipeline; rc = 0;
        } else {
            if (pipeline) pvkDestroyPipeline(g_dev, pipeline, NULL);
            pvkDestroyPipelineLayout(g_dev, layout, NULL);
        }
    }
    pvkDestroyShaderModule(g_dev, mod, NULL);
    if (rc) { g_rt_curves_failed = 1; nvlog("vk: curve bounding box pipeline FAILED - curve geometry is refused"); }
done:
    pthread_mutex_unlock(&g_rt_curves_lock);
    return rc;
}

int nvmtl_vk_cmd_curve_aabbs(nvk_cmdbuf *c, const nvmtl_curve_args *a)
{
    _Static_assert(sizeof(nvmtl_curve_args) == 80, "push constant block of nvmtl_rt_curves.comp");
    if (!c || !c->cb || c->in_rp || !a || !a->dst || (a->seg_count && (!a->cp || !a->radius || !a->index)) || nvmtl_rt_curves_init()) return -1;
    if (!a->seg_count) return 0;
    nvmtl_vk_cmd_barrier(c);   /* control points, radii and indices written by earlier commands of c */
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_rt_curves_pipeline);
    pvkCmdPushConstants(c->cb, g_rt_curves_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof *a, a);
    pvkCmdDispatch(c->cb, (a->seg_count + 63u) / 64u, 1, 1);
    nvmtl_vk_cmd_barrier(c);   /* the build reads the boxes */
    return 0;
}

int nvmtl_vk_cmd_upload(nvk_cmdbuf *c, nvk_buffer *b, size_t off, const void *data, size_t size, nvk_buffer *stage)
{
    if (!size) return 0;
    if (!c || !c->cb || c->in_rp || !b || !b->buf || !stage || !stage->buf || !stage->map || stage->size < size ||
        off > b->size || size > b->size - off) return -1;
    memcpy(stage->map, data, size);
    return nvmtl_vk_cmd_copy_buffer(c, stage, 0, b, off, size);
}

int nvmtl_vk_image_write_region(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ return nvmtl_vk_image_write_region_level(q, img, src, row_bytes, x, y, w, h, 0); }
int nvmtl_vk_image_read_region(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes,
                               uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{ return nvmtl_vk_image_read_region_level(q, img, dst, row_bytes, x, y, w, h, 0); }

int nvmtl_vk_image_gen_mipmaps(nvk_queue *q, nvk_image *img)
{
    nvk_cmdbuf c; if (nvmtl_vk_cmd_begin(q, &c)) return -1;
    if (nvmtl_vk_cmd_image_gen_mipmaps(&c, img)) { nvmtl_vk_cmd_abandon(&c); return -1; }
    return nvmtl_vk_submit_wait(&c);
}

int nvmtl_vk_image_view_create(nvk_image *img, int bgra, uint32_t baseLevel, uint32_t levelCount, void **out_view)
{
    return nvmtl_vk_image_view_create_ex(img, bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM, 0, baseLevel, levelCount, out_view);
}
static VkImageViewType nvmtl_vk_view_type_of(const nvk_image *img) {
    switch (img->mtl_type) { case 0: return VK_IMAGE_VIEW_TYPE_1D; case 1: return VK_IMAGE_VIEW_TYPE_1D_ARRAY; case 3: return VK_IMAGE_VIEW_TYPE_2D_ARRAY;
        case 5: return VK_IMAGE_VIEW_TYPE_CUBE; case 6: return VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; case 7: return VK_IMAGE_VIEW_TYPE_3D; default: return VK_IMAGE_VIEW_TYPE_2D; }
}
static uint32_t nvmtl_vk_view_layers_of(const nvk_image *img) { return img->mtl_type == 7 ? 1u : (img->layers ? img->layers : 1u); }
int nvmtl_vk_image_view_create_ex(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount, void **out_view)
{
    if (nvmtl_vk_init()) return -1;
    if (!levelCount) levelCount = (img->mips ? img->mips : 1) - baseLevel;
    if (baseLevel + levelCount > (img->mips ? img->mips : 1)) {
        nvlog("image_view_create: levels [%u,%u) is outside the %u level(s) this texture has", baseLevel, baseLevel + levelCount, img->mips);
        return -1;
    }
    VkFormat fmt = (VkFormat)vkfmt;
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img->img,
        nvmtl_vk_view_type_of(img), fmt, swz, { VK_IMAGE_ASPECT_COLOR_BIT, baseLevel, levelCount, 0, nvmtl_vk_view_layers_of(img) } };
    VkImageViewUsageCreateInfo uci;
    int vg = nvmtl_vk_view_guard(img, vkfmt, a8, vci.viewType, vci.subresourceRange.levelCount, &uci, "image_view_create");
    if (vg < 0) return -1;
    if (vg > 0) vci.pNext = &uci;
    VkImageView v = VK_NULL_HANDLE;
    VKCK(pvkCreateImageView(g_dev, &vci, NULL, &v), "vkCreateImageView(view)");
    *out_view = v;
    return 0;
}
int nvmtl_vk_image_view_create_aspect(nvk_image *img, uint32_t vkfmt, uint32_t aspect, uint32_t baseLevel, uint32_t levelCount, void **out_view)
{
    if (nvmtl_vk_init()) return -1;
    if (!levelCount) levelCount = (img->mips ? img->mips : 1) - baseLevel;
    if (baseLevel + levelCount > (img->mips ? img->mips : 1)) { nvlog("image_view_create(aspect): levels [%u,%u) is outside the %u level(s)", baseLevel, baseLevel + levelCount, img->mips); return -1; }
    if (img->fmt != vkfmt) { nvlog("image_view_create(aspect): view format %u is not the image's format %u", vkfmt, img->fmt); return -1; }
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img->img,
        nvmtl_vk_view_type_of(img), (VkFormat)vkfmt, { 0 }, { (VkImageAspectFlags)aspect, baseLevel, levelCount, 0, nvmtl_vk_view_layers_of(img) } };
    VkImageView v = VK_NULL_HANDLE;
    VKCK(pvkCreateImageView(g_dev, &vci, NULL, &v), "vkCreateImageView(aspect view)");
    *out_view = v;
    return 0;
}
void nvmtl_vk_image_view_destroy(void *view) { if (view) pvkDestroyImageView(g_dev, (VkImageView)view, NULL); }
int nvmtl_vk_texel_on(void) { return g_mutable && pvkCreateBufferView != NULL; }
int nvmtl_vk_texel_view_create(nvk_buffer *b, size_t offset, size_t range, uint32_t vkfmt, int storage, void **alias, void **view, int *storage_ok)
{
    *alias = *view = NULL; if (storage_ok) *storage_ok = 0;
    if (!nvmtl_vk_texel_on()) { nvlog("texel buffer: not carried on this device (mutable descriptors %d, vkCreateBufferView %s)", g_mutable, pvkCreateBufferView ? "yes" : "NO"); return -1; }
    if (!b || !b->buf || !b->mem) { nvlog("texel buffer: the buffer has no memory of its own (pooled/imported) — not carried"); return -1; }
    if (!range || offset + range > b->size) { nvlog("texel buffer: [%zu,+%zu) is outside the %zu-byte buffer", offset, range, b->size); return -1; }
    static PFN_vkGetPhysicalDeviceFormatProperties gfp; if (!gfp) gfp = (PFN_vkGetPhysicalDeviceFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties fp = { 0 }; if (gfp) gfp(g_pdev, (VkFormat)vkfmt, &fp);
    if (!(fp.bufferFeatures & VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) { nvlog("texel buffer: VkFormat %u is not a uniform texel format on this device", vkfmt); return -1; }
    const int st = storage && (fp.bufferFeatures & VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT);
    if (storage && !st) { static int said; if (said++ < 4) nvlog("texel buffer: VkFormat %u is not a STORAGE texel format — the texture reads, its writes are refused", vkfmt); }
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, range,
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | (st ? VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT : 0), VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer ab = VK_NULL_HANDLE;
    VKCK(pvkCreateBuffer(g_dev, &bci, NULL, &ab), "vkCreateBuffer(texel alias)");
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, ab, &mr);
    const VkDeviceSize at = (VkDeviceSize)b->moff + offset;
    if (mr.alignment && at % mr.alignment) {
        nvlog("texel buffer: offset %llu is not a multiple of the texel alignment %llu — refused", (unsigned long long)at, (unsigned long long)mr.alignment);
        pvkDestroyBuffer(g_dev, ab, NULL); return -1; }
    VkResult r = pvkBindBufferMemory(g_dev, ab, (VkDeviceMemory)b->mem, at);
    if (r != VK_SUCCESS) { nvlog("texel buffer: vkBindBufferMemory(alias) -> %d", r); pvkDestroyBuffer(g_dev, ab, NULL); return -1; }
    VkBufferViewCreateInfo vci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, ab, (VkFormat)vkfmt, 0, range };
    VkBufferView v = VK_NULL_HANDLE;
    r = pvkCreateBufferView(g_dev, &vci, NULL, &v);
    if (r != VK_SUCCESS) { nvlog("texel buffer: vkCreateBufferView -> %d", r); pvkDestroyBuffer(g_dev, ab, NULL); return -1; }
    *alias = ab; *view = v; if (storage_ok) *storage_ok = st;
    // the buffer model makes texel descriptors from the address, range and format (the alias is b's memory at `at`)
    if (g_dbuf) nvmtl_db_texel_note(v, nvmtl_vk_buffer_address(b) + offset, range, vkfmt);
    return 0;
}
void nvmtl_vk_texel_view_destroy(void *alias, void *view)
{
    if (g_dbuf) nvmtl_db_texel_forget(view);
    if (view) nvmtl_vk_objgen_bump();   // nvmtl_dirty.h: a buffer view made later may get this address
    if (view && pvkDestroyBufferView) pvkDestroyBufferView(g_dev, (VkBufferView)view, NULL);
    if (alias) pvkDestroyBuffer(g_dev, (VkBuffer)alias, NULL);
}
int nvmtl_vk_bind_texel_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view, int storage)
{
    if (index >= NVMTL_NTEX) { nvlog("bind texel view: index %u is outside [0,%d)", index, NVMTL_NTEX); return -1; }
    int rc = nvmtl_bind_slot(c, set, NVMTL_NBUF + index, NVMTL_SLOT_TEXBUF, view, 0);
    if (index < NVMTL_NSTEX) nvmtl_bind_slot(c, set, NVMTL_STEX_BASE + index, storage && view ? NVMTL_SLOT_TEXBUF : NVMTL_SLOT_NONE, storage ? view : NULL, 0);
    return rc;
}
static void nvmtl_spirv_texel_mask(const void *p, size_t n, uint64_t out[4])
{
    memset(out, 0, 4 * sizeof *out);
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4;
    if (!p || nw < 5 || w[0] != 0x07230203u || !w[3] || w[3] > (1u << 22)) return;
    const uint32_t bound = w[3];
    uint8_t *kind = (uint8_t *)calloc(bound, 1);
    if (!kind) return;
    for (int pass = 0; pass < 2; pass++)
        for (size_t i = 5; i < nw; ) {
            uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
            if (!wc || i + wc > nw) break;
            if (pass == 0 && op == 25 && wc >= 4 && w[i + 1] < bound && w[i + 3] == 5) kind[w[i + 1]] = 1;
            if (pass == 0 && op == 32 && wc == 4 && w[i + 1] < bound && w[i + 3] < bound && kind[w[i + 3]] == 1) kind[w[i + 1]] = 2;
            if (pass == 0 && op == 59 && wc >= 4 && w[i + 2] < bound && w[i + 1] < bound && kind[w[i + 1]] == 2) kind[w[i + 2]] = 3;
            if (pass == 1 && op == 71 && wc == 4 && w[i + 2] == 33 && w[i + 1] < bound && kind[w[i + 1]] == 3) {
                uint32_t b = w[i + 3];
                if (b >= NVMTL_NBUF && b < NVMTL_NBUF + NVMTL_NTEX) { uint32_t k = b - NVMTL_NBUF; out[k >> 6] |= 1ull << (k & 63); }
                else if (b >= NVMTL_STEX_BINDING_NUMBER && b < NVMTL_STEX_BINDING_NUMBER + NVMTL_NSTEX) { uint32_t k = b - NVMTL_STEX_BINDING_NUMBER; out[2 + (k >> 6)] |= 1ull << (k & 63); }
            }
            i += wc;
        }
    free(kind);
}

int nvmtl_vk_bind_texture_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view)
{
    if (index >= NVMTL_NTEX) { nvlog("bind texture view: index %u is outside [0,%d)", index, NVMTL_NTEX); return -1; }
    const nvmtl_slot *was = (c->table && set < 2) ? &((nvmtl_bindtable *)c->table)->s[set][NVMTL_NBUF + index] : NULL;
    // A texture slot keeps in vextra (a buffer slot's field) the object generation its heap slot was looked up at: the same
    // view pointer after a view was destroyed may be another view with another heap slot (nvmtl_objgen)
    const uint32_t gen = (uint32_t)nvmtl_objgen();
    const int ptrsame = was && was->kind == NVMTL_SLOT_TEX && was->h == view, same = ptrsame && was->vextra == gen;
    int r = nvmtl_bind_slot(c, set, NVMTL_NBUF + index, NVMTL_SLOT_TEX, view, 0);
    if (!r && !same && c->table && set < 2) { extern uint32_t nvmtl_bl_slot(void *); nvmtl_bindtable *t = (nvmtl_bindtable *)c->table;
        const uint32_t i = NVMTL_NBUF + index; nvmtl_slot *sl = &t->s[set][i]; const size_t was_size = sl->size;
        sl->size = view ? nvmtl_bl_slot(view) : 0; sl->vextra = gen;
        // same pointer, another heap slot: the address table entry changed although nvmtl_bind_slot saw no change
        if (ptrsame && sl->size != was_size) { t->dirty[set] = 1; t->chg[set][i >> 6] |= 1ull << (i & 63); t->texgen[set]++; }
        nvmtl_tab_note(t, set, i); }
    return r;
}
int nvmtl_vk_bind_storage_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view)
{
    if (index >= NVMTL_NSTEX) return -1;
    return nvmtl_bind_slot(c, set, NVMTL_STEX_BASE + index, view ? NVMTL_SLOT_TEX : NVMTL_SLOT_NONE, view, 0);
}

int nvmtl_vk_have_polygon_mode(void) { return g_dyn3; }
int nvmtl_vk_have_provoking_last(void) { return g_provoking; }
void nvmtl_vk_cmd_set_provoking_last(nvk_cmdbuf *c, int last)
{
    if (g_provoking)
        pvkCmdSetProvokingVertexModeEXT(c->cb, last ? VK_PROVOKING_VERTEX_MODE_LAST_VERTEX_EXT : VK_PROVOKING_VERTEX_MODE_FIRST_VERTEX_EXT);
}
int nvmtl_vk_cmd_set_polygon_mode(nvk_cmdbuf *c, int line)
{
    if (!g_dyn3 || !pvkCmdSetPolygonModeEXT) return -1;
    pvkCmdSetPolygonModeEXT(c->cb, line ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL);
    return 0;
}

#include "nvmtl_visibility_sum_spv.h"
static pthread_mutex_t g_visibility_sum_lock = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline g_visibility_sum_pipeline;
static VkPipelineLayout g_visibility_sum_layout;

static int nvmtl_visibility_sum_init(void)
{
    int rc = -1;
    pthread_mutex_lock(&g_visibility_sum_lock);
    if (g_visibility_sum_pipeline) { rc = 0; goto done; }
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0,
                                   sizeof nvmtl_visibility_sum_spv, nvmtl_visibility_sum_spv };
    if (pvkCreateShaderModule(g_dev, &sm, NULL, &mod) != VK_SUCCESS) goto done;
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 24 };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pvkCreatePipelineLayout(g_dev, &li, NULL, &layout) == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
        // no sets: valid in either model; flagged so a buffer-model command buffer holds no classic pipeline at all
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
                                          stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS) {
            g_visibility_sum_layout = layout; g_visibility_sum_pipeline = pipeline; rc = 0;
        } else {
            if (pipeline) pvkDestroyPipeline(g_dev, pipeline, NULL);
            pvkDestroyPipelineLayout(g_dev, layout, NULL);
        }
    }
    pvkDestroyShaderModule(g_dev, mod, NULL);
done:
    pthread_mutex_unlock(&g_visibility_sum_lock);
    return rc;
}

int nvmtl_vk_cmd_sum_occlusion(nvk_cmdbuf *c, nvk_buffer *src, size_t first, uint32_t count, nvk_buffer *dst, size_t offset)
{
    if (!c || c->in_rp || !src || !dst || !count || first > src->size / 8 ||
        count > src->size / 8 - first || (offset & 7) || offset > dst->size || dst->size - offset < 8 ||
        nvmtl_visibility_sum_init()) return -1;
    struct { uint64_t src, dst; uint32_t count, pad; } args = {
        nvmtl_vk_buffer_address(src) + first * 8,
        nvmtl_vk_buffer_address(dst) + offset, count, 0
    };
    if (!args.src || !args.dst) return -1;
    nvmtl_vk_cmd_barrier(c);
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_visibility_sum_pipeline);
    pvkCmdPushConstants(c->cb, g_visibility_sum_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof args, &args);
    pvkCmdDispatch(c->cb, 1, 1, 1);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

enum { NVMTL_OCCLUSION_PAGE = 128 };
typedef struct nvmtl_occlusion_page {
    struct nvmtl_occlusion_page *next;
    VkQueryPool pool;
    uint32_t base;
} nvmtl_occlusion_page;

static nvmtl_occlusion_page *nvmtl_occlusion_find(nvk_cmdbuf *c, uint32_t query)
{
    for (nvmtl_occlusion_page *p = c->occlusion_pages; p; p = p->next)
        if (query >= p->base && query - p->base < NVMTL_OCCLUSION_PAGE) return p;
    return NULL;
}

int nvmtl_vk_cmd_alloc_occlusion(nvk_cmdbuf *c, uint32_t *query)
{
    if (!c || !c->cb || !query || !g_host_query_reset || !g_occlusion_precise ||
        !pvkResetQueryPool || c->occlusion_count == UINT32_MAX) {
        nvlog("visibility: precise host-reset queries unavailable"); return -1;
    }
    const uint32_t idx = c->occlusion_count;
    nvmtl_occlusion_page *p = nvmtl_occlusion_find(c, idx);
    if (!p) {
        p = calloc(1, sizeof *p);
        if (!p) return -1;
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
                                    VK_QUERY_TYPE_OCCLUSION, NVMTL_OCCLUSION_PAGE, 0 };
        if (pvkCreateQueryPool(g_dev, &qi, NULL, &p->pool) != VK_SUCCESS) { free(p); return -1; }
        pvkResetQueryPool(g_dev, p->pool, 0, NVMTL_OCCLUSION_PAGE);
        p->base = idx; p->next = c->occlusion_pages; c->occlusion_pages = p;
    }
    *query = idx; c->occlusion_count++;
    return 0;
}

void nvmtl_vk_cmd_retire_occlusion(nvk_cmdbuf *c)
{
    nvmtl_occlusion_page *p = c->occlusion_pages;
    while (p) {
        nvmtl_occlusion_page *next = p->next;
        pvkDestroyQueryPool(g_dev, p->pool, NULL); free(p); p = next;
    }
    c->occlusion_pages = NULL; c->occlusion_count = 0;
}

int nvmtl_vk_cmd_begin_occlusion(nvk_cmdbuf *c, uint32_t idx)
{
    nvmtl_occlusion_page *p = nvmtl_occlusion_find(c, idx);
    if (!p || !c->in_rp) return -1;
    pvkCmdBeginQuery(c->cb, p->pool, idx - p->base, VK_QUERY_CONTROL_PRECISE_BIT);
    return 0;
}

void nvmtl_vk_cmd_end_occlusion(nvk_cmdbuf *c, uint32_t idx)
{
    nvmtl_occlusion_page *p = nvmtl_occlusion_find(c, idx);
    if (p && c->in_rp) pvkCmdEndQuery(c->cb, p->pool, idx - p->base);
}

int nvmtl_vk_cmd_copy_occlusion(nvk_cmdbuf *c, uint32_t first, uint32_t count, nvk_buffer *dst, size_t off)
{
    nvmtl_occlusion_page *p = nvmtl_occlusion_find(c, first);
    if (!p || !count || c->in_rp || first - p->base + count > NVMTL_OCCLUSION_PAGE ||
        !dst || !dst->buf || off > dst->size || (size_t)count * 8 > dst->size - off) return -1;
    pvkCmdCopyQueryPoolResults(c->cb, p->pool, first - p->base, count, (VkBuffer)dst->buf, off, 8,
                              VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    return 0;
}

int nvmtl_vk_cmd_set_depth_clip(nvk_cmdbuf *c, int clip)
{
    if (!g_clip || !pvkCmdSetDepthClipEnableEXT) return -1;
    pvkCmdSetDepthClipEnableEXT(c->cb, clip ? VK_TRUE : VK_FALSE);
    return 0;
}

/* Metal GPU timestamps (MTLCommonCounterSetTimestamp, the WebGPU 'timestamp-query' of WebKit). A sample is a
   VK_QUERY_TYPE_TIMESTAMP query of the recording's own pages (reset by its counter-reset command buffer, submitted
   ahead of it), written bottom-of-pipe where Metal asks (stage boundaries of a pass, sampleCountersInBuffer:), and copied
   as raw ticks into the sample buffer's host-visible values at the next flush point (a render pass end, a resolve, the
   submit). Ticks become nanoseconds on the host's CLOCK_UPTIME_RAW scale - the scale -sampleTimestamps:gpuTimestamp:
   answers in - when they are resolved: on the CPU (-resolveCounterRange:) or by nvmtl_ts_convert.comp in the resolve's
   destination (-resolveCounters:inRange:destinationBuffer:destinationOffset:), both from one reference pair (a GPU tick
   and the uptime it was read at, nvmtl_ts_ref) and the same fixed-point arithmetic, so the two agree to the nanosecond.
   A sample never written stays MTLCounterErrorValue (~0). */
#define NVMTL_COUNTER_PAGE 64u
typedef struct nvmtl_counter_state { uint32_t count; nvk_buffer values; } nvmtl_counter_state;
typedef struct nvmtl_counter_page { struct nvmtl_counter_page *next; VkQueryPool pool; uint32_t used; } nvmtl_counter_page;
typedef struct { nvmtl_counter_state *state; VkQueryPool pool; uint32_t query, index; } nvmtl_counter_write;
typedef struct {
    nvk_cmdbuf reset;
    nvmtl_counter_page *pages;
    nvmtl_counter_write *pending;
    size_t count, capacity;
    unsigned reset_ended;
} nvmtl_counter_recording;
typedef struct { uint64_t tick, ns, mask; uint32_t period_int, period_frac; } nvmtl_ts_ref;

static int nvmtl_counter_fail(nvk_cmdbuf *c) { if (c) c->counter_error = 1; return -1; }
static void nvmtl_counter_memory_barrier(nvk_cmdbuf *c)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL,
        VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_WRITE_BIT,
        VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_HOST_READ_BIT };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}
static int nvmtl_counter_flush(nvk_cmdbuf *c)
{
    if (!c || c->counter_error) return -1;
    nvmtl_counter_recording *r = c->counter_recording;
    if (!r || !r->count) return 0;
    if (c->in_rp) return nvmtl_counter_fail(c);
    nvmtl_counter_memory_barrier(c);
    for (size_t i = 0; i < r->count; ++i) {   /* in recording order: an index sampled twice keeps the later sample */
        nvmtl_counter_write *w = &r->pending[i];
        pvkCmdCopyQueryPoolResults(c->cb, w->pool, w->query, 1,
            (VkBuffer)w->state->values.buf, (VkDeviceSize)w->index * 8, 8,
            VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
    }
    r->count = 0;
    nvmtl_counter_memory_barrier(c);
    return 0;
}
static int nvmtl_counter_reset_finish(nvk_cmdbuf *c, VkCommandBuffer *reset)
{
    *reset = VK_NULL_HANDLE;
    nvmtl_counter_recording *r = c->counter_recording;
    if (!r) return 0;
    if (r->reset_ended || !r->reset.cb || r->reset.in_rp) return nvmtl_counter_fail(c);
    if (pvkEndCommandBuffer(r->reset.cb) != VK_SUCCESS) return nvmtl_counter_fail(c);
    r->reset_ended = 1; *reset = (VkCommandBuffer)r->reset.cb;
    return 0;
}
static void nvmtl_counter_retire(nvk_cmdbuf *c)
{
    nvmtl_counter_recording *r = c->counter_recording;
    if (!r) return;
    r->reset.wseq = c->wseq;
    nvmtl_cmd_retire(&r->reset);
    for (nvmtl_counter_page *p = r->pages; p;) {
        nvmtl_counter_page *next = p->next;
        pvkDestroyQueryPool(g_dev, p->pool, NULL); free(p); p = next;
    }
    free(r->pending); free(r); c->counter_recording = NULL;
}

/* ticks -> ns: |tick - ref| modulo timestampValidBits, signed (a sample may precede the reference), times the period as
   period_int + period_frac / 2^32; the same arithmetic as nvmtl_ts_convert.comp */
static uint64_t nvmtl_ts_ns(uint64_t v, const nvmtl_ts_ref *r)
{
    if (v == ~0ull) return v;
    const uint64_t d = (v - r->tick) & r->mask;
    const int before = (d & ((r->mask >> 1) + 1)) != 0;
    const uint64_t mag = before ? ((~d + 1) & r->mask) : d;
    const uint64_t hi = mag >> 32, lo = mag & 0xffffffffull;
    const uint64_t ns = mag * r->period_int + hi * r->period_frac + ((lo * r->period_frac) >> 32);
    return before ? r->ns - ns : r->ns + ns;
}
static pthread_mutex_t g_cal_lock = PTHREAD_MUTEX_INITIALIZER;
static VkQueryPool g_cal_pool;
static nvmtl_ts_ref g_cal_ref;
static uint64_t g_cal_at;
static int g_ts_said;
/* The reference: the calibrated device clock (VK_EXT_calibrated_timestamps, nvmtl_vk_clock_refresh, renewed each
   second) when it reads, else a timestamp written by a command buffer of its own that the CPU waits for, bracketed by
   the host clock (tightest of 3, renewed each second). */
static int nvmtl_ts_ref_get(nvmtl_ts_ref *out)
{
    if (!g_timestamp_bits || g_timestamp_bits > 64 || !isfinite(g_lim.timestampPeriod) || !(g_lim.timestampPeriod > 0)) return -1;
    const uint64_t mask = g_timestamp_bits >= 64 ? ~0ull : (1ull << g_timestamp_bits) - 1;
    const double period = g_lim.timestampPeriod;
    const uint32_t pi = period >= 4294967295.0 ? 0xffffffffu : (uint32_t)period;
    const double fr = (period - pi) * 4294967296.0;
    const uint32_t pf = fr >= 4294967295.0 ? 0xffffffffu : (uint32_t)(fr + 0.5);
    nvmtl_vk_clock_refresh();
    const nvmtl_gpu_clock k = nvmtl_clock_snapshot();
    if (k.valid) {
        *out = (nvmtl_ts_ref){ k.gpu_tick & mask, k.uptime_ns, mask, pi, pf };
        if (!__atomic_exchange_n(&g_ts_said, 1, __ATOMIC_RELAXED))
            nvlog("counters: timestamps referenced to the calibrated device clock (+/- %llu ns)", (unsigned long long)k.uncertainty_ns);
        return 0;
    }
    pthread_mutex_lock(&g_cal_lock);
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!g_cal_at || now - g_cal_at > 1000000000ull) {
        if (!g_cal_pool && pvkCreateQueryPool) {
            VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
                VK_QUERY_TYPE_TIMESTAMP, 1, 0 };
            VkQueryPool pool = VK_NULL_HANDLE;
            if (pvkCreateQueryPool(g_dev, &qi, NULL, &pool) == VK_SUCCESS) g_cal_pool = pool;
        }
        uint64_t best = UINT64_MAX, tick = 0, mid = 0;
        for (int k = 0; g_cal_pool && k < 3; k++) {
            nvk_cmdbuf cb; if (nvmtl_vk_cmd_begin(NULL, &cb)) break;
            pvkCmdResetQueryPool(cb.cb, g_cal_pool, 0, 1);
            pvkCmdWriteTimestamp(cb.cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, g_cal_pool, 0);
            const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            const int rc = nvmtl_vk_submit_wait(&cb);
            const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
            if (rc) break;
            uint64_t result[2] = {0};
            VkResult qr = pvkGetQueryPoolResults(g_dev, g_cal_pool, 0, 1,
                sizeof result, result, 16, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WITH_AVAILABILITY_BIT);
            if (qr != VK_SUCCESS || !result[1] || !result[0]) break;
            if (t1 - t0 < best) { best = t1 - t0; tick = result[0] & mask; mid = t0 + (t1 - t0) / 2; }
        }
        if (best != UINT64_MAX) {
            const nvmtl_ts_ref ref = { tick, mid, mask, pi, pf };
            if (!g_cal_at) nvlog("counters: timestamps referenced to a waited-for timestamp write (no calibrated device clock), bracket %llu ns (tightest of 3)",
                                 (unsigned long long)best);
            else {
                /* where the previous reference puts this tick, against where the host clock does: the drift */
                const int64_t moved = (int64_t)(nvmtl_ts_ns(tick, &g_cal_ref) - mid);
                if (llabs(moved) > 1000000) nvlog("counters: GPU clock RE-REFERENCED, it moved %lld ns against the host clock", (long long)moved);
            }
            g_cal_ref = ref; g_cal_at = now;
        }
    }
    const int ok = g_cal_at != 0;
    if (ok) *out = g_cal_ref;
    pthread_mutex_unlock(&g_cal_lock);
    return ok ? 0 : -1;
}

#include "nvmtl_ts_convert_spv.h"
static pthread_mutex_t g_ts_convert_lock = PTHREAD_MUTEX_INITIALIZER;
static VkPipeline g_ts_convert_pipeline;
static VkPipelineLayout g_ts_convert_layout;
static int g_ts_convert_failed;
typedef struct { uint64_t dst, ref_tick, ref_ns, mask; uint32_t count, period_int, period_frac, pad; } nvmtl_ts_convert_args;
static int nvmtl_ts_convert_init(void)
{
    int rc = -1;
    pthread_mutex_lock(&g_ts_convert_lock);
    if (g_ts_convert_pipeline) { rc = 0; goto done; }
    if (g_ts_convert_failed) goto done;
    VkShaderModule mod = VK_NULL_HANDLE;
    VkShaderModuleCreateInfo sm = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof nvmtl_ts_convert_spv, nvmtl_ts_convert_spv };
    if (pvkCreateShaderModule(g_dev, &sm, NULL, &mod) != VK_SUCCESS) { g_ts_convert_failed = 1; goto done; }
    VkPushConstantRange push = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(nvmtl_ts_convert_args) };
    VkPipelineLayoutCreateInfo li = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 0, NULL, 1, &push };
    VkPipelineLayout layout = VK_NULL_HANDLE;
    if (pvkCreatePipelineLayout(g_dev, &li, NULL, &layout) == VK_SUCCESS) {
        VkPipelineShaderStageCreateInfo stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0,
                                                 VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", NULL };
        VkComputePipelineCreateInfo pi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, nvmtl_db_pipeline_flags(),
                                          stage, layout, VK_NULL_HANDLE, -1 };
        VkPipeline pipeline = VK_NULL_HANDLE;
        if (pvkCreateComputePipelines(g_dev, VK_NULL_HANDLE, 1, &pi, NULL, &pipeline) == VK_SUCCESS) {
            g_ts_convert_layout = layout; g_ts_convert_pipeline = pipeline; rc = 0;
        } else {
            if (pipeline) pvkDestroyPipeline(g_dev, pipeline, NULL);
            pvkDestroyPipelineLayout(g_dev, layout, NULL);
        }
    }
    pvkDestroyShaderModule(g_dev, mod, NULL);
    if (rc) { g_ts_convert_failed = 1; nvlog("vk: timestamp conversion pipeline FAILED"); }
done:
    pthread_mutex_unlock(&g_ts_convert_lock);
    return rc;
}

/* Whether this device samples GPU timestamps: what -supportsCounterSampling: and -counterSets answer (WebKit offers
   'timestamp-query' only with MTLCounterSamplingPointAtStageBoundary and a timestamp counter set). Decided once. */
static pthread_once_t g_ts_once = PTHREAD_ONCE_INIT;
static int g_ts_ok;
static void nvmtl_ts_decide(void)
{
    const char *why = nvmtl_vk_init() ? "no Vulkan device"
        : !g_timestamp_bits ? "the queue has no timestampValidBits"
        : g_timestamp_bits > 64 ? "timestampValidBits above 64"
        : !g_lim.timestampComputeAndGraphics ? "no timestampComputeAndGraphics"
        : !isfinite(g_lim.timestampPeriod) || !(g_lim.timestampPeriod > 0) || g_lim.timestampPeriod >= 4294967295.0 ? "no usable timestampPeriod"
        : !pvkCreateQueryPool || !pvkCmdWriteTimestamp || !pvkCmdResetQueryPool || !pvkCmdCopyQueryPoolResults || !pvkGetQueryPoolResults ? "query entry points missing"
        : !pvkGetBufferDeviceAddress ? "no buffer device addresses"
        : nvmtl_ts_convert_init() ? "the conversion pipeline could not be made" : NULL;
    g_ts_ok = !why;
    nvlog("counters: GPU timestamps %s%s - timestampValidBits %u, timestampPeriod %.6f ns, timestampComputeAndGraphics %u, %s; "
          "sampling at stage boundaries (pass sampleBufferAttachments) and draw/dispatch/blit boundaries (sampleCountersInBuffer) %s",
          why ? "OFF: " : "ON", why ? why : "", g_timestamp_bits, g_state == 1 ? g_lim.timestampPeriod : 0.0,
          g_state == 1 ? g_lim.timestampComputeAndGraphics : 0u,
          pvkGetCalibratedTimestampsEXT ? "calibrated device clock present" : "no calibrated device clock (waited-for reference writes)",
          why ? "NOT offered" : "offered");
}
/* nm29 (nullmoth f8c5bfb): -queryTimestampFrequency, the unit of resolved timestamps (Metal 4 counter heaps resolve through
   the counter sample buffers above). This copy resolves every sample to nanoseconds on the host's clock (see the sample
   buffer's conversion), not to device ticks, so the frequency is 1 GHz whenever timestamps are offered; 0 when not. */
uint64_t nvmtl_vk_timestamp_frequency(void) { return nvmtl_vk_ts_supported() ? 1000000000ull : 0; }
int nvmtl_vk_ts_supported(void)
{
    pthread_once(&g_ts_once, nvmtl_ts_decide);
    return g_ts_ok;
}

void *nvmtl_vk_ts_pool_create(uint32_t count)
{
    if (!count || (size_t)count > SIZE_MAX / 8 || !nvmtl_vk_ts_supported()) return NULL;
    nvmtl_counter_state *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->count = count;
    if (nvmtl_vk_buffer_create((size_t)count * 8, 1, &s->values) || !s->values.map) {
        nvmtl_vk_buffer_destroy(&s->values); free(s); return NULL;
    }
    memset(s->values.map, 0xff, (size_t)count * 8);   /* MTLCounterErrorValue until sampled */
    return s;
}
void nvmtl_vk_ts_pool_destroy(void *pool)
{
    nvmtl_counter_state *s = pool;
    if (!s) return;
    if (g_dev && g_state == 1) nvmtl_vk_buffer_destroy(&s->values);
    free(s);
}
int nvmtl_vk_cmd_ts_write(nvk_cmdbuf *c, void *pool, uint32_t idx, int barrier)
{
    nvmtl_counter_state *s = pool;
    if (!c || !c->cb || !s || idx >= s->count || c->counter_error) return nvmtl_counter_fail(c);
    nvmtl_counter_recording *r = c->counter_recording;
    if (!r) {
        r = calloc(1, sizeof *r);
        if (!r) return nvmtl_counter_fail(c);
        c->counter_recording = r;
        if (nvmtl_vk_cmd_begin(NULL, &r->reset)) return nvmtl_counter_fail(c);
    }
    if (r->reset_ended) return nvmtl_counter_fail(c);
    if (r->count == r->capacity) {
        size_t cap = r->capacity ? r->capacity * 2 : 16;
        if (cap < r->capacity || cap > SIZE_MAX / sizeof *r->pending) return nvmtl_counter_fail(c);
        void *q = realloc(r->pending, cap * sizeof *r->pending);
        if (!q) return nvmtl_counter_fail(c);
        r->pending = q; r->capacity = cap;
    }
    nvmtl_counter_page *p = r->pages;
    if (!p || p->used == NVMTL_COUNTER_PAGE) {
        p = calloc(1, sizeof *p);
        if (!p) return nvmtl_counter_fail(c);
        VkQueryPoolCreateInfo qi = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0,
            VK_QUERY_TYPE_TIMESTAMP, NVMTL_COUNTER_PAGE, 0 };
        if (pvkCreateQueryPool(g_dev, &qi, NULL, &p->pool) != VK_SUCCESS) { free(p); return nvmtl_counter_fail(c); }
        p->next = r->pages; r->pages = p;
        pvkCmdResetQueryPool(r->reset.cb, p->pool, 0, NVMTL_COUNTER_PAGE);
    }
    uint32_t query = p->used++;
    /* bottom of pipe: after all the work recorded before it (ANV: a PIPE_CONTROL post-sync timestamp write); top of
       pipe (withBarrier:NO): when the command streamer reaches it (a register store). Allowed inside a render pass. */
    pvkCmdWriteTimestamp(c->cb, barrier ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
        p->pool, query);
    r->pending[r->count++] = (nvmtl_counter_write){s, p->pool, query, idx};
    /* copied into the sample buffer at the next flush point (a render pass end, a resolve, the submit), with the samples
       before it: a copy here would stall the pipe at every pass boundary once more */
    return 0;
}
int nvmtl_vk_cmd_ts_resolve(nvk_cmdbuf *c, void *pool, uint32_t first, uint32_t count, nvk_buffer *dst, size_t off)
{
    nvmtl_counter_state *s = pool;
    if (!c || !c->cb || !s || !count || first > s->count || count > s->count-first ||
        !dst || !dst->buf || (off & 7) || off > dst->size || (size_t)count*8 > dst->size-off || c->in_rp)
        return nvmtl_counter_fail(c);
    nvmtl_ts_ref ref;
    const uint64_t da = nvmtl_vk_buffer_address(dst);
    if (!da || nvmtl_ts_convert_init() || nvmtl_ts_ref_get(&ref)) {
        static int said; if (said++ < 4) nvlog("resolveCounters: %s - not resolved", !da ? "the destination has no device address"
                                               : g_ts_convert_failed ? "no conversion pipeline" : "the GPU clock could not be referenced");
        return nvmtl_counter_fail(c);
    }
    if (nvmtl_counter_flush(c)) return -1;
    /* the raw ticks into Metal's destination (full barriers on both sides), then converted there in place */
    if (nvmtl_vk_cmd_copy_buffer(c, &s->values, (size_t)first*8, dst, off, (size_t)count*8)) return nvmtl_counter_fail(c);
    const nvmtl_ts_convert_args a = { da + off, ref.tick, ref.ns, ref.mask, count, ref.period_int, ref.period_frac, 0 };
    pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_ts_convert_pipeline);
    pvkCmdPushConstants(c->cb, g_ts_convert_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof a, &a);
    pvkCmdDispatch(c->cb, (count + 63) / 64, 1, 1);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}
int nvmtl_vk_ts_read(void *pool, uint32_t first, uint32_t count, uint64_t *out)
{
    nvmtl_counter_state *s = pool;
    if (!s || !out || !count || first > s->count || count > s->count-first || !s->values.map || g_state != 1) return -1;
    nvmtl_ts_ref ref;
    if (nvmtl_ts_ref_get(&ref)) return -1;
    memcpy(out, (const char *)s->values.map + (size_t)first*8, (size_t)count*8);
    for (uint32_t i = 0; i < count; i++) out[i] = nvmtl_ts_ns(out[i], &ref);
    return 0;
}
/* -sampleTimestamps:gpuTimestamp: the host's CLOCK_UPTIME_RAW (mach_absolute_time on x86) and the GPU's clock on the
   scale resolved samples use: the device clock read now through the reference, or (no calibrated device clock) the
   reference extrapolated, which is the host time itself */
int nvmtl_vk_gpu_timestamp(uint64_t *cpu_ns, uint64_t *gpu)
{
    nvmtl_ts_ref ref;
    const int ok = nvmtl_vk_ts_supported() && !nvmtl_ts_ref_get(&ref);
    uint64_t now = 0, tick = 0; int fresh = 0;
    if (ok && pvkGetCalibratedTimestampsEXT) {
        VkCalibratedTimestampInfoEXT info = { VK_STRUCTURE_TYPE_CALIBRATED_TIMESTAMP_INFO_EXT, NULL, VK_TIME_DOMAIN_DEVICE_EXT };
        uint64_t deviation = UINT64_MAX;
        const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        fresh = pvkGetCalibratedTimestampsEXT(g_dev, 1, &info, &tick, &deviation) == VK_SUCCESS;
        const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        now = t0 + (t1 - t0) / 2;
        if (fresh && (t1 - t0 > 2000000ull || deviation > 1000000ull)) fresh = 0;
    } else now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (cpu_ns) *cpu_ns = now;
    if (gpu) *gpu = !ok ? 0 : fresh ? nvmtl_ts_ns(tick & ref.mask, &ref) : now;
    return ok ? 0 : -1;
}

uint64_t nvmtl_vk_buffer_address(nvk_buffer *b)
{
    if (!b || !b->buf || !pvkGetBufferDeviceAddress) return 0;
    // a VkBuffer's address does not change (sparse buffers keep a fixed range): asked once per VkBuffer, not on every
    // bind (vkGetBufferDeviceAddress was ~20% of setVertexBuffer/setFragmentBuffer under Apple's GL, perf1)
    if (b->addr_of == b->buf && b->addr) return b->addr;
    VkBufferDeviceAddressInfo i = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, (VkBuffer)b->buf };
    const uint64_t a = pvkGetBufferDeviceAddress(g_dev, &i);
    if (a) { b->addr = a; b->addr_of = b->buf; }
    return a;
}

int nvmtl_vk_cmd_fill_buffer(nvk_cmdbuf *c, nvk_buffer *b, size_t offset, size_t size, uint8_t value)
{
    if (!size) return 0;
    if ((offset | size) & 3 || offset > b->size || size > b->size - offset) return -1;
    nvmtl_vk_cmd_barrier(c);
    uint32_t word = value; word |= word << 8; word |= word << 16;
    pvkCmdFillBuffer(c->cb, (VkBuffer)b->buf, offset, size, word);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

int nvmtl_vk_cmd_write_word(nvk_cmdbuf *c, nvk_buffer *b, size_t offset, uint32_t word)
{
    if ((offset & 3) || offset > b->size || b->size - offset < 4) return -1;
    nvmtl_vk_cmd_barrier(c);
    pvkCmdFillBuffer(c->cb, (VkBuffer)b->buf, offset, 4, word);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

int nvmtl_vk_cmd_copy_buffer(nvk_cmdbuf *c, nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size)
{
    if (!size) return 0;
    if (srcOff > src->size || size > src->size-srcOff || dstOff > dst->size || size > dst->size-dstOff) return -1;
    nvmtl_vk_cmd_barrier(c);
    VkBufferCopy bc = { srcOff, dstOff, size };
    pvkCmdCopyBuffer(c->cb, (VkBuffer)src->buf, (VkBuffer)dst->buf, 1, &bc);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

int nvmtl_vk_cmd_copy_image(nvk_cmdbuf *c, nvk_image *src, uint32_t sx, uint32_t sy,
                        nvk_image *dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    return nvmtl_vk_cmd_copy_image_sub(c, src, 0, 0, sx, sy, dst, 0, 0, dx, dy, w, h);
}

static inline uint32_t nvmtl_lvl(uint32_t d, uint32_t l) { d >>= l; return d ? d : 1; }
int nvmtl_vk_cmd_copy_buffer_to_image_layer(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer)
{ return nvmtl_vk_cmd_copy_buffer_to_image_level_layer(c, b, off, row_bytes, img, x, y, w, h, 0, layer); }
int nvmtl_vk_cmd_copy_buffer_to_image_level_layer(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer)
{
    // A texture with no VkImage reached here from RealityKit's texture setup (Logic Pro, Dolby Atmos mixer): the layout
    // barrier below handed NVK a null image and the app crashed in nvk_CmdPipelineBarrier2 (read at 0x520). Refuse the
    // copy and name the texture, so the path that left it without an image can be found from the log.
    if (!nvmtl_copy_has_image("copy_buffer_to_image", img)) return -1;
    if (level >= (img->mips ? img->mips : 1)) { nvlog("copy_buffer_to_image: level %u — this image has %u", level, img->mips ? img->mips : 1); return -1; }
    const uint32_t lw = nvmtl_lvl(img->w, level), lh = nvmtl_lvl(img->h, level);
    const uint32_t bpp = g_copy_bpp_override ? g_copy_bpp_override : (img->bpp ? img->bpp : 4);
    const uint32_t bw = img->bw ? img->bw : 1, bh = img->bh ? img->bh : 1;
    const uint32_t cols = nvmtl_blocks(w, bw), rows = nvmtl_blocks(h, bh);
    const uint64_t tight = (uint64_t)cols * bpp;
    if (!row_bytes) { if (tight > UINT32_MAX) return -1; row_bytes = (uint32_t)tight; }
    if (bw > 1 && ((x % bw) || (y % bh))) { nvlog("copy: origin (%u,%u) is not block-aligned", x, y); return -1; }
    if (layer >= (img->layers ? img->layers : 1) || !w || !h || x > lw || w > lw-x || y > lh || h > lh-y
        || row_bytes < tight || row_bytes % bpp || off > b->size || (uint64_t)(rows-1)*row_bytes+tight > b->size-off) return -1;
    if (!w || !h || x + w > lw || y + h > lh) { nvlog("copy_buffer_to_image: out of bounds (level %u is %ux%u)", level, lw, lh); return -1; }
    nvmtl_vk_cmd_barrier(c);
    img_to(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = { off, row_bytes / bpp * bw, 0, { nvmtl_copy_aspect(img), level, img->mtl_type == 7 ? 0u : layer, 1 },
                              { (int32_t)x, (int32_t)y, img->mtl_type == 7 ? (int32_t)layer : 0 }, { w, h, 1 } };
    pvkCmdCopyBufferToImage(c->cb, (VkBuffer)b->buf, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    img_to(c, img, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

int nvmtl_vk_cmd_copy_image_to_buffer_layer(nvk_cmdbuf *c, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t layer)
{ return nvmtl_vk_cmd_copy_image_to_buffer_level_layer(c, img, x, y, w, h, b, off, row_bytes, 0, layer); }
int nvmtl_vk_cmd_copy_image_to_buffer_level_layer(nvk_cmdbuf *c, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t level, uint32_t layer)
{
    if (!nvmtl_copy_has_image("copy_image_to_buffer", img)) return -1;
    if (level >= (img->mips ? img->mips : 1)) { nvlog("copy_image_to_buffer: level %u — this image has %u", level, img->mips ? img->mips : 1); return -1; }
    const uint32_t lw = nvmtl_lvl(img->w, level), lh = nvmtl_lvl(img->h, level);
    const uint32_t bpp = g_copy_bpp_override ? g_copy_bpp_override : (img->bpp ? img->bpp : 4);
    const uint32_t bw = img->bw ? img->bw : 1, bh = img->bh ? img->bh : 1;
    const uint32_t cols = nvmtl_blocks(w, bw), rows = nvmtl_blocks(h, bh);
    const uint64_t tight = (uint64_t)cols * bpp;
    if (!row_bytes) { if (tight > UINT32_MAX) return -1; row_bytes = (uint32_t)tight; }
    if (bw > 1 && ((x % bw) || (y % bh))) { nvlog("copy: origin (%u,%u) is not block-aligned", x, y); return -1; }
    if (layer >= (img->layers ? img->layers : 1) || !w || !h || x > lw || w > lw-x || y > lh || h > lh-y
        || row_bytes < tight || row_bytes % bpp || off > b->size || (uint64_t)(rows-1)*row_bytes+tight > b->size-off) return -1;
    if (!w || !h || x + w > lw || y + h > lh) { nvlog("copy_image_to_buffer: out of bounds (level %u is %ux%u)", level, lw, lh); return -1; }
    nvmtl_vk_cmd_barrier(c);
    img_to(c, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy bic = { off, row_bytes / bpp * bw, 0, { nvmtl_copy_aspect(img), level, img->mtl_type == 7 ? 0u : layer, 1 },
                              { (int32_t)x, (int32_t)y, img->mtl_type == 7 ? (int32_t)layer : 0 }, { w, h, 1 } };
    pvkCmdCopyImageToBuffer(c->cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, (VkBuffer)b->buf, 1, &bic);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

int nvmtl_vk_cmd_image_gen_mipmaps(nvk_cmdbuf *c, nvk_image *img)
{
    if (!img->mips || img->mips < 2) { nvlog("generateMipmaps: texture has %u level(s); nothing to generate", img->mips); return 0; }
    const int how = nvmtl_vk_format_mip_blit(img->fmt);
    if (!how) { nvlog("generateMipmaps: VkFormat %u cannot be blitted on this device - levels left as they are", img->fmt); return -1; }
    nvmtl_vk_cmd_barrier(c);
    img_to(c, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    int32_t mw = (int32_t)img->w, mh = (int32_t)img->h;
    for (uint32_t i = 1; i < img->mips; i++) {
        VkImageSubresourceRange src = { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 1, 0, 1 };
        VkImageMemoryBarrier toSrc = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL,
            VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img, src };
        pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &toSrc);
        int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        VkImageBlit b = { { VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1 }, { { 0, 0, 0 }, { mw, mh, 1 } },
                          { VK_IMAGE_ASPECT_COLOR_BIT, i,     0, 1 }, { { 0, 0, 0 }, { nw, nh, 1 } } };
        pvkCmdBlitImage(c->cb, (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        (VkImage)img->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, how == 2 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
        mw = nw; mh = nh;
    }
    VkImageMemoryBarrier fin[2] = {
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img,
          { VK_IMAGE_ASPECT_COLOR_BIT, 0, img->mips - 1, 0, 1 } },
        { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
          VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, (VkImage)img->img,
          { VK_IMAGE_ASPECT_COLOR_BIT, img->mips - 1, 1, 0, 1 } },
    };
    pvkCmdPipelineBarrier(c->cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, NULL, 0, NULL, 2, fin);
    img->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    nvmtl_vk_cmd_barrier(c);
    return 0;
}

#define NVMTL_BUFFER_USAGE (VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | \
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | \
        VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT | NVMTL_RT_BUFFER_USAGE)
static uint32_t heap_memtype_bits(void)
{
    static uint32_t bits; static int done;
    if (done) return bits;
    bits = ~0u;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &bci, NULL, &b) == VK_SUCCESS) { VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, b, &mr); bits &= mr.memoryTypeBits; pvkDestroyBuffer(g_dev, b, NULL); }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 64, 64, 1 }, 1, 1,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage im = VK_NULL_HANDLE;
    if (nvmtl_typed_CreateImage(g_dev, &ici, NULL, &im) == VK_SUCCESS) { VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, im, &mr); bits &= mr.memoryTypeBits; pvkDestroyImage(g_dev, im, NULL); }
    ici.format = VK_FORMAT_D32_SFLOAT_S8_UINT; ici.usage = NVMTL_DEPTH_USAGE; im = VK_NULL_HANDLE;
    if (nvmtl_typed_CreateImage(g_dev, &ici, NULL, &im) == VK_SUCCESS) { VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, im, &mr); bits &= mr.memoryTypeBits; pvkDestroyImage(g_dev, im, NULL); }
    done = 1;
    nvlog("vk: placement-heap memory types: 0x%x", bits);
    return bits;
}
static int nvmtl_vk_heap_create_body(size_t size, int host_visible, nvk_heap *out);
// 5fcf1ad: a fresh MTLHeap reads as zero on Apple's stack (the kernel scrubs pages at allocation; a re-placement inside a
// live heap is NOT zeroed, and aliasing relies on that), so an argument-buffer item the app never wrote holds NULL pointers
// its kernels null-check. A fresh VkDeviceMemory holds whatever the memory held before - ANV recycles this process's freed
// BOs through its BO cache - and such an item then carries a stale pointer: a page-table fault in the kernel that
// dereferences it. Every app heap is therefore cleared once, on creation, before anything is placed in it: a host memset
// when it is mapped, else a transient buffer over the whole memory and one vkCmdFillBuffer, waited for, so the first
// placement's use is ordered after it. NVMTL_HEAP_ZERO=0 switches it off. (The fork's buffer pools keep their own
// per-buffer zeroing, nvmtl_zero_new_buffer.)
static int nvmtl_heap_zero_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_HEAP_ZERO"); on = !(e && e[0] == '0');
        nvlog("vk: heaps are cleared to zero on creation, as a fresh MTLHeap reads (NVMTL_HEAP_ZERO=%d)", on); }
    return on;
}
static uint64_t g_heap_zero_n, g_heap_zero_bytes, g_heap_zero_ns, g_heap_zero_failed;
int nvmtl_vk_heap_zero(nvk_heap *h, const char *what)
{
    if (!nvmtl_heap_zero_on() || g_state != 1 || !h || !h->mem || !h->size) return 0;
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); const char *how = "host memset";
    if (h->map) memset(h->map, 0, h->size);
    else {
        how = "GPU fill";
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, h->size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer buf = VK_NULL_HANDLE; VkMemoryRequirements mr = {0}; int ok = 0;
        if (pvkCreateBuffer(g_dev, &bci, NULL, &buf) == VK_SUCCESS) {
            pvkGetBufferMemoryRequirements(g_dev, buf, &mr);
            if ((mr.memoryTypeBits & (1u << h->memtype)) && mr.size <= h->size
                && pvkBindBufferMemory(g_dev, buf, (VkDeviceMemory)h->mem, 0) == VK_SUCCESS) {
                nvk_cmdbuf c;
                if (!nvmtl_vk_cmd_begin(NULL, &c)) {
                    pvkCmdFillBuffer(c.cb, buf, 0, h->size & ~(VkDeviceSize)3, 0);
                    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
                    pvkCmdPipelineBarrier(c.cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
                    ok = nvmtl_vk_submit_wait(&c) == 0;
                }
            }
            pvkDestroyBuffer(g_dev, buf, NULL);
        }
        if (!ok) { g_heap_zero_failed++; nvlog("heap zero-fill: %s heap %p of %llu bytes NOT cleared (buffer over its memory refused: type bits 0x%x vs %u, need %llu) - %llu failed so far",
                                               what, h->mem, (unsigned long long)h->size, mr.memoryTypeBits, h->memtype, (unsigned long long)mr.size, (unsigned long long)g_heap_zero_failed); return -1; }
    }
    const uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    g_heap_zero_n++; g_heap_zero_bytes += h->size; g_heap_zero_ns += dt;
    nvlog("heap zero-fill: %s heap %p %llu bytes cleared on creation in %.2f ms (%s); %llu heaps, %llu MB, %.1f ms so far",
          what, h->mem, (unsigned long long)h->size, dt / 1e6, how, (unsigned long long)g_heap_zero_n, (unsigned long long)(g_heap_zero_bytes >> 20), g_heap_zero_ns / 1e6);
    return 0;
}
int nvmtl_vk_heap_create(size_t size, int host_visible, nvk_heap *out)
{
    const uint64_t t0 = nvmtl_perf_now();
    nvk_heap pending = {0};
    const int r = nvmtl_vk_heap_create_body(size, host_visible, &pending);
    if (r) memset(out, 0, sizeof *out);
    else *out = pending;
    nvmtl_perf_note(NVP_HEAP, t0, size);
    return r;
}
static int nvmtl_vk_heap_create_body(size_t size, int host_visible, nvk_heap *out)
{
    if (nvmtl_vk_init()) return -1;
    memset(out, 0, sizeof *out); out->size = size; out->host_visible = host_visible;
    uint32_t bits = heap_memtype_bits();
    int mt = memtype(bits, host_visible ? (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { nvlog("vk: no memory type serves a %s placement heap (bits 0x%x)", host_visible ? "shared" : "private", bits); return -1; }
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, size, (uint32_t)mt };
    VkDeviceMemory mem = VK_NULL_HANDLE;
    { uint64_t hbud = 0, hused = 0, hfree = 0; int hmeas = 0;
      int hgate = host_visible ? 0 : nvmtl_vk_vram_wall(size, &hfree, &hbud, &hused, &hmeas);
      VkResult r_ = hgate ? VK_ERROR_OUT_OF_DEVICE_MEMORY : pvkAllocateMemory(g_dev, &mai, NULL, &mem);
      if (r_ == VK_SUCCESS && !host_visible) nvmtl_vk_budget_charge(size);
      if (r_ == VK_ERROR_OUT_OF_DEVICE_MEMORY && !host_visible) {
          int mt2 = -1; for (uint32_t i = 0; i < g_memp.memoryTypeCount; i++) if ((bits & (1u << i)) && !(g_memp.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) { mt2 = (int)i; break; }
          if (mt2 >= 0) { mai.memoryTypeIndex = (uint32_t)mt2; r_ = pvkAllocateMemory(g_dev, &mai, NULL, &mem); mt = mt2;
              if (r_ == VK_SUCCESS) { g_heap_vram_refused++;
                  nvlog("vk: private placement heap of %zu bytes REFUSED VRAM (%s) -> SYSTEM MEMORY: %llu MB free, "
                        "%llu MB headroom, heapUsage %llu of budget %llu MB (%s), %llu heaps refused so far",
                        size, hgate ? "our soft gate" : "RM refused it", (unsigned long long)(hfree >> 20),
                        (unsigned long long)(NVMTL_VRAM_HEADROOM >> 20), (unsigned long long)(hused >> 20),
                        (unsigned long long)(hbud >> 20), hmeas ? "measured" : "estimated",
                        (unsigned long long)g_heap_vram_refused); } }
      }
      if (r_ != VK_SUCCESS) { nvlog("vk: vkAllocateMemory(placement heap) -> %d", r_); return -1; } }
    out->mem = mem; out->memtype = (uint32_t)mt;
    if (host_visible) {
        VkResult mapped = pvkMapMemory(g_dev, mem, 0, VK_WHOLE_SIZE, 0, &out->map);
        if (mapped != VK_SUCCESS) { pvkFreeMemory(g_dev, mem, NULL); memset(out, 0, sizeof *out); return -1; }
    }
    g_alloc_bytes += size;
    return 0;
}
void nvmtl_vk_heap_destroy(nvk_heap *h)
{
    if (g_state != 1 || !h || !h->mem) return;
    if (h->map && pvkUnmapMemory) { pvkUnmapMemory(g_dev, (VkDeviceMemory)h->mem); h->map = NULL; }
    pvkFreeMemory(g_dev, (VkDeviceMemory)h->mem, NULL); g_alloc_bytes -= h->size;
    memset(h, 0, sizeof *h);
}
static int placed_ok(const VkMemoryRequirements *mr, const nvk_heap *heap, size_t offset, const char *what)
{
    if (!(mr->memoryTypeBits & (1u << heap->memtype))) { nvlog("vk: %s cannot live in memory type %u", what, heap->memtype); return -1; }
    if (mr->alignment && (offset % mr->alignment)) { nvlog("vk: %s offset %zu breaks alignment %llu", what, offset, (unsigned long long)mr->alignment); return -1; }
    if (offset + mr->size > heap->size) { nvlog("vk: %s at %zu + %llu overruns the heap (%zu)", what, offset, (unsigned long long)mr->size, heap->size); return -1; }
    return 0;
}
int nvmtl_vk_buffer_create_placed(size_t size, nvk_heap *heap, size_t offset, nvk_buffer *out)
{
    if (nvmtl_vk_init() || !heap || !heap->mem) return -1;
    memset(out, 0, sizeof *out); out->size = size;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VKCK(nvmtl_opaque_CreateBuffer(g_dev, &bci, NULL, &out->buf), "vkCreateBuffer(placed)");
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, out->buf, &mr);
    if (placed_ok(&mr, heap, offset, "placed buffer")) { pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); memset(out, 0, sizeof *out); return -1; }
    if (pvkBindBufferMemory(g_dev, out->buf, (VkDeviceMemory)heap->mem, offset) != VK_SUCCESS) {
        pvkDestroyBuffer(g_dev, (VkBuffer)out->buf, NULL); memset(out, 0, sizeof *out); return -1;
    }
    out->mem = heap->mem; out->placed = 1; out->alloc = 0; out->mtype1 = heap->memtype + 1;
    out->moff = offset;
    out->map = heap->map ? (char *)heap->map + offset : NULL;
    g_buf_made++;
    return 0;
}

static int nvmtl_vk_image_create_buffer_alias_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                       const nvk_buffer *b, size_t offset, size_t row_pitch, nvk_image *out, uint64_t modifier);
int nvmtl_vk_image_create_buffer_alias(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                       const nvk_buffer *b, size_t offset, size_t row_pitch, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_image_create_buffer_alias_lifetime_body(w, h, vkfmt, bpp, a8, storage, render, b, offset, row_pitch, &pending, 0);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
/* An image over a VRAM backing its creator renders in `modifier` (Tile4, from SURFACE_VRAM2) at `pitch`: the same bytes
   read through the same layout (a DRM format modifier image with that explicit pitch), as a Linux compositor imports a
   tiled dma-buf. Single format (no view-format list), never written by a reader. */
int nvmtl_vk_image_create_buffer_alias_tiled(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                             const nvk_buffer *b, uint64_t modifier, size_t pitch, nvk_image *out)
{
    nvk_image pending = {0};
    int result = modifier ? nvmtl_vk_image_create_buffer_alias_lifetime_body(w, h, vkfmt, bpp, a8, storage, render, b, 0, pitch, &pending, modifier) : -1;
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
static int nvmtl_vk_image_create_buffer_alias_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                       const nvk_buffer *b, size_t offset, size_t row_pitch, nvk_image *out, uint64_t modifier)
{
    const int tiled = modifier != 0;
    if (nvmtl_vk_init() || !b || !b->buf || !b->mem || !w || !h) { nvlog("buffer texture: no buffer memory to alias (%ux%u)", w, h); return -1; }
    { uint32_t bw = 0, bh = 0, bb = 0;
      if (nvmtl_vk_format_block(vkfmt, &bw, &bh, &bb)) { nvlog("buffer texture: VkFormat %u is block-compressed; a linear alias of it is not built", vkfmt); return -1; } }
    if (!bpp) bpp = 4;
    if (row_pitch < (size_t)w * bpp || row_pitch > UINT32_MAX || row_pitch % bpp) {
        nvlog("buffer texture: bytesPerRow %zu cannot hold %u texels of %u B", row_pitch, w, bpp); return -1; }
    if (tiled && (row_pitch % 128 || (size_t)row_pitch * ((h + 31) & ~31u) > b->size)) {
        nvlog("buffer texture: Tile4 pitch %zu for %ux%u does not fit the %zu-byte backing", row_pitch, w, h, b->size); return -1; }
    // NVK's texture header counts a linear pitch in 32-byte units; Intel's surface state takes any multiple of the texel
    // size for a linear render target or texture (isl_calc_row_pitch_alignment), which the check above already asks
    if (!tiled && g_linmod && row_pitch % 32 && nvmtl_vk_vendor() != 0x8086) {
        nvlog("buffer texture: bytesPerRow %zu is not a multiple of 32 B (the texture header's linear pitch unit)", row_pitch); return -1; }
    if (offset > b->size || (size_t)row_pitch * (h - 1) + (size_t)w * bpp > b->size - offset) {
        nvlog("buffer texture: %ux%u at offset %zu pitch %zu overruns the %zu-byte buffer", w, h, offset, row_pitch, b->size); return -1; }
    PFN_vkGetPhysicalDeviceFormatProperties gfp = (PFN_vkGetPhysicalDeviceFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties fp; memset(&fp, 0, sizeof fp);
    if (gfp) gfp(g_pdev, (VkFormat)vkfmt, &fp);
    VkFormatFeatureFlags lf = tiled ? fp.optimalTilingFeatures : fp.linearTilingFeatures;
    if (!(lf & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT)) { nvlog("buffer texture: VkFormat %u cannot be sampled %s (features 0x%x)", vkfmt, tiled ? "Tile4" : "LINEAR", (unsigned)lf); return -1; }
    int wantS = storage, wantR = render;
    storage = storage && !a8 && (lf & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);
    render = render && (lf & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT);
    if (wantS != storage || wantR != render)
        nvlog("buffer texture: VkFormat %u linear features 0x%x drop%s%s", vkfmt, (unsigned)lf, wantS != storage ? " ShaderWrite" : "", wantR != render ? " RenderTarget" : "");
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->mips = 1; out->fmt = vkfmt; out->bpp = bpp;
    out->storage = storage ? 1 : 0; out->mtl_type = 2; out->layers = 1;
    VkSubresourceLayout pl = { 0, 0, (VkDeviceSize)row_pitch, 0, 0 };
    VkImageDrmFormatModifierExplicitCreateInfoEXT eci = { VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT, NULL,
        modifier, 1, &pl };
    const int drmmod = g_linmod || tiled;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, drmmod ? (void *)&eci : NULL, 0, VK_IMAGE_TYPE_2D, (VkFormat)vkfmt,
        { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, drmmod ? VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT : VK_IMAGE_TILING_LINEAR,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
            | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0) | (render ? VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT : 0),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_PREINITIALIZED };
    /* Name every format of this texture's own size that the device will carry for this tiling and
       usage, so that a view of any of them is legal. A format that cannot be proved supported is
       left out, because naming it would fail the whole vkCreateImage, and the mask of the ones
       that made it in is kept on the image so a view of an unlisted format stays refused. */
    uint32_t vmask = 0;
    const VkFormat *vtbl = NULL;
    const uint32_t vtbln = (a8 || tiled) ? 0 : nvmtl_vk_view_group(bpp, &vtbl);
    VkFormat vlist[32];
    uint32_t nlist = 0;
    VkImageFormatListCreateInfo fl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, ici.pNext, 0, vlist };
    if (vtbln) {
        const VkFormatFeatureFlags need = nvmtl_vk_usage_features(ici.usage);
        int self_listed = 0;
        for (uint32_t i = 0; i < vtbln && nlist < (uint32_t)(sizeof vlist / sizeof *vlist); i++) {
            VkFormatProperties pp; memset(&pp, 0, sizeof pp);
            if (gfp) gfp(g_pdev, vtbl[i], &pp);
            if ((pp.linearTilingFeatures & need) != need) continue;
            if ((uint32_t)vtbl[i] == vkfmt) self_listed = 1;
            vlist[nlist++] = vtbl[i];
            vmask |= 1u << i;
        }
        /* An image has to name its own format. If it did not survive the filter then this size
           group cannot describe it, so fall through to a single-format image rather than build a
           list the driver will reject. */
        if (!self_listed || nlist < 2) {
            if (nlist) nvlog("buffer texture: VkFormat %u stays single-format - only %u of its %u same-size format(s) are supported LINEAR for usage 0x%x%s",
                             vkfmt, nlist, vtbln, (unsigned)ici.usage, self_listed ? "" : " and its own format is not among them");
            vmask = 0; nlist = 0;
        }
    }
    if (nlist) {
        PFN_vkGetPhysicalDeviceImageFormatProperties2 gif = (PFN_vkGetPhysicalDeviceImageFormatProperties2)
            pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceImageFormatProperties2");
        VkImageFormatListCreateInfo qfl = { VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, NULL, nlist, vlist };
        VkPhysicalDeviceImageDrmFormatModifierInfoEXT qmi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT, &qfl,
            0 , VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkPhysicalDeviceImageFormatInfo2 qi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, g_linmod ? (void *)&qmi : (void *)&qfl,
            ici.format, ici.imageType, ici.tiling, ici.usage, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT };
        VkImageFormatProperties2 qo; memset(&qo, 0, sizeof qo); qo.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2;
        VkResult qr = gif ? gif(g_pdev, &qi, &qo) : VK_ERROR_EXTENSION_NOT_PRESENT;
        if (qr == VK_SUCCESS) { ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; fl.viewFormatCount = nlist; ici.pNext = &fl; }
        else { nvlog("buffer texture: VkFormat %u stays single-format - a mutable image over its %u same-size format(s) is refused (VkResult %d%s)",
                     vkfmt, nlist, (int)qr, gif ? "" : ", no vkGetPhysicalDeviceImageFormatProperties2"); vmask = 0; nlist = 0; }
    }
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage(buffer texture)");
    out->layout = VK_IMAGE_LAYOUT_PREINITIALIZED;
    out->vkflags = (uint32_t)ici.flags; out->usage = (uint32_t)ici.usage; out->vfl = vmask;
    if (!drmmod) {
        PFN_vkGetImageSubresourceLayout gsl = (PFN_vkGetImageSubresourceLayout)pvkGetInstanceProcAddr(g_inst, "vkGetImageSubresourceLayout");
        VkImageSubresource sr = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }; VkSubresourceLayout got; memset(&got, 0, sizeof got);
        if (gsl) gsl(g_dev, (VkImage)out->img, &sr, &got);
        if (!gsl || got.rowPitch != row_pitch || got.offset) {
            nvlog("buffer texture: LINEAR tiling picked pitch %llu offset %llu, the app's bytesPerRow is %zu - refused",
                  (unsigned long long)got.rowPitch, (unsigned long long)got.offset, row_pitch);
            pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1; }
    }
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, (VkImage)out->img, &mr);
    uint32_t all = g_memp.memoryTypeCount >= 32 ? 0xffffffffu : ((1u << g_memp.memoryTypeCount) - 1u);
    size_t at = b->moff + offset;
    /* The image must accept the memory type the buffer actually lives in. Requiring every type (NVK's linear images
       accept all) refused every buffer texture on ANV/Xe2, whose images leave out the compressed and protected types;
       newTiledTextureWithDescriptor: then handed CoreAnimation a one-time COPY of the app's pages, and WindowServer kept
       drawing that copy after the app redrew its backing store in place (stale text fields, 2026-10-28). */
    const int typeok = b->mtype1 ? (int)((mr.memoryTypeBits >> (b->mtype1 - 1)) & 1u) : (mr.memoryTypeBits & all) == all;
    const char *why = !typeok ? "memory types" : (mr.alignment && at % mr.alignment) ? "alignment" : NULL;
    if (why) {
        nvlog("buffer texture: refused on %s - image types 0x%x of 0x%x, bind at %zu (buffer %zu + %zu) vs alignment %llu",
              why, mr.memoryTypeBits, all, at, b->moff, offset, (unsigned long long)mr.alignment);
        pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1; }
    if (mr.size > b->size - offset)
        nvlog("buffer texture: image size %llu runs %llu B past the buffer's %zu at offset %zu (the tail of the last row, never addressed)",
              (unsigned long long)mr.size, (unsigned long long)(mr.size - (b->size - offset)), b->size, offset);
    VKCK(pvkBindImageMemory(g_dev, (VkImage)out->img, (VkDeviceMemory)b->mem, at), "vkBindImageMemory(buffer texture)");
    out->mem = b->mem; out->placed = 1; out->alloc = 0;
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, VK_IMAGE_VIEW_TYPE_2D,
        (VkFormat)vkfmt, swz, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView(buffer texture)");

    nvlog("buffer texture: %ux%u VkFormat %u pitch %zu bound at %zu (%s, align %llu, size %llu, storage %d, render %d, view formats 0x%x)", w, h, vkfmt, row_pitch, at,
          tiled ? "DRM Tile4" : g_linmod ? "DRM linear" : "LINEAR", (unsigned long long)mr.alignment, (unsigned long long)mr.size, storage, render, out->vfl);
    return 0;
}

typedef struct { size_t off, len; } pool_range;
typedef struct pool_block {
    struct pool_block *next; nvk_heap heap; size_t used; uint32_t nlive;
    int vram;
    pool_range *fr; uint32_t nfr, cfr;
} pool_block;
static pool_block *g_pool;
#define NVMTL_VPOOL_BLOCK (8u << 20)
static pool_block *g_vpool;
/* Private buffers under 1 MiB (the sparse path takes larger ones) are placed in 16 MiB VRAM blocks instead of one
   vkAllocateMemory each: Minecraft's chunk meshes made 1209 allocations of 0.23 ms in 5 s while moving (2026-10-28).
   The blocks take the uncompressed VRAM type every buffer takes (nvmtl_vk_buffer_vram_type: a buffer texture aliases
   them). NVMTL_PRIVATE_POOL=0: one allocation per buffer, as before. */
#define NVMTL_PPOOL_BLOCK (16u << 20)
static pool_block *g_ppool;
static uint64_t g_ppool_blocks;
static int ppool_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_PRIVATE_POOL"); on = !(e && e[0] == '0');
        nvlog("buffer pool: Private buffers under %u KiB %s (NVMTL_PRIVATE_POOL=%s)", NVMTL_PPOOL_MAX >> 10,
              on ? "are placed in 16 MiB VRAM blocks" : "get one allocation each", e ? e : "unset"); }
    return on;
}
static int ppool_heap_create(size_t size, nvk_heap *out)
{
    static uint32_t bits;
    memset(out, 0, sizeof *out);
    if (!bits) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer pb = VK_NULL_HANDLE;
        if (pvkCreateBuffer(g_dev, &bci, NULL, &pb) != VK_SUCCESS) return -1;
        VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, pb, &mr); pvkDestroyBuffer(g_dev, pb, NULL);
        bits = mr.memoryTypeBits; if (!bits) return -1;
    }
    const int mt = nvmtl_vk_buffer_vram_type(bits); if (mt < 0) return -1;
    if (nvmtl_vk_vram_wall0(size, NULL, NULL, NULL, NULL)) return -1;   // near the wall: the old per-buffer path decides
    VkMemoryAllocateFlagsInfo mafi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &mafi, size, (uint32_t)mt };
    VkDeviceMemory m = VK_NULL_HANDLE;
    if (pvkAllocateMemory(g_dev, &mai, NULL, &m) != VK_SUCCESS) return -1;
    out->mem = m; out->size = size; out->memtype = (uint32_t)mt; out->host_visible = 0;
    g_alloc_bytes += size; nvmtl_vk_budget_charge(size);
    return 0;
}
static uint64_t g_vpool_blocks;
static pthread_mutex_t g_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static size_t g_pool_align;

static size_t pool_align(void)
{
    if (g_pool_align) return g_pool_align;
    size_t a = 256;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    if (pvkCreateBuffer(g_dev, &bci, NULL, &b) == VK_SUCCESS) {
        VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, b, &mr);
        if (mr.alignment > a) a = (size_t)mr.alignment;
        pvkDestroyBuffer(g_dev, b, NULL);
    }
    g_pool_align = a; return a;
}
static int pool_block_take(pool_block *blk, size_t len, size_t *off)
{
    for (uint32_t i = 0; i < blk->nfr; i++) {
        if (blk->fr[i].len >= len) {
            *off = blk->fr[i].off; blk->fr[i].off += len; blk->fr[i].len -= len;
            if (!blk->fr[i].len) blk->fr[i] = blk->fr[--blk->nfr];
            return 1;
        }
    }
    if (blk->used + len <= blk->heap.size) { *off = blk->used; blk->used += len; return 1; }
    return 0;
}
static void pool_block_give(pool_block *blk, size_t off, size_t len)
{
    if (off + len == blk->used) { blk->used = off; }
    else {
        for (uint32_t i = 0; i < blk->nfr; i++) {
            if (blk->fr[i].off + blk->fr[i].len == off) { blk->fr[i].len += len; return; }
            if (off + len == blk->fr[i].off) { blk->fr[i].off = off; blk->fr[i].len += len; return; }
        }
        if (blk->nfr == blk->cfr) { uint32_t c = blk->cfr ? blk->cfr * 2 : 64; pool_range *n = realloc(blk->fr, c * sizeof *n); if (!n) return; blk->fr = n; blk->cfr = c; }
        blk->fr[blk->nfr].off = off; blk->fr[blk->nfr].len = len; blk->nfr++;
    }
    if (!blk->nlive) { blk->used = 0; blk->nfr = 0; }
}
static int vpool_on(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SHARED_POOL_VRAM"); on = !(e && e[0] == '0');
        if (!on) nvlog("vk: small Shared buffers stay in the system-RAM pool (NVMTL_SHARED_POOL_VRAM=0)"); }
    return on && bar1_on() && !g_bar1_off;
}
static const char *pool_place_why(int vram)
{
    if (vram == 2) return "Private, VRAM";
    if (vram) return "VRAM through BAR1";
    if (!bar1_on()) return "system RAM BY DESIGN - Shared is system RAM on a discrete GPU (batch 56; NVMTL_SHARED_VRAM=1 opts in)";
    if (g_bar1_off) return "system RAM - BAR1 REFUSED a VRAM block for this process (see the REFUSED line above)";
    return "system RAM - this request never asked for VRAM (host_visible != 2)";
}
static int vpool_heap_create(size_t size, nvk_heap *out)
{
    static uint32_t bits;
    memset(out, 0, sizeof *out);
    if (!bits) {
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer pb = VK_NULL_HANDLE;
        if (pvkCreateBuffer(g_dev, &bci, NULL, &pb) != VK_SUCCESS) return -1;
        VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, pb, &mr); pvkDestroyBuffer(g_dev, pb, NULL);
        bits = mr.memoryTypeBits; if (!bits) return -1;
    }
    int bt = bar1_memtype(bits, size); if (bt < 0) return -1;
    VkMemoryAllocateFlagsInfo bf = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo bi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &bf, size, (uint32_t)bt };
    VkDeviceMemory bm = VK_NULL_HANDLE; void *bp = NULL;
    VkResult r1 = pvkAllocateMemory(g_dev, &bi, NULL, &bm);
    VkResult r2 = r1 == VK_SUCCESS ? pvkMapMemory(g_dev, bm, 0, VK_WHOLE_SIZE, 0, &bp) : r1;
    if (r2 != VK_SUCCESS || !bp) {
        if (r1 == VK_SUCCESS && r2 == VK_SUCCESS) pvkUnmapMemory(g_dev, bm);
        if (bm) pvkFreeMemory(g_dev, bm, NULL);
        g_bar1_off = 1;
        nvlog("buffer pool: VRAM block REFUSED (alloc %d, map %d) at %zu BAR bytes - small Shared buffers take system RAM for this process", r1, r2, g_bar1_bytes);
        return -1; }
    out->mem = bm; out->map = bp; out->size = size; out->memtype = (uint32_t)bt; out->host_visible = 1;
    g_alloc_bytes += size; g_bar1_bytes += size;
    return 0;
}
static int pool_buffer_create_in(int vram, size_t size, nvk_buffer *out)
{
    size_t al = pool_align(), len = (size + al - 1) / al * al; if (!len) len = al;
    pthread_mutex_lock(&g_pool_lock);
    pool_block **head = vram == 2 ? &g_ppool : vram ? &g_vpool : &g_pool;
    pool_block *blk = *head; size_t off = 0; int got = 0;
    for (; blk; blk = blk->next) if (pool_block_take(blk, len, &off)) { got = 1; break; }
    if (!got) {
        pool_block *nb = calloc(1, sizeof *nb);
        if (!nb || (vram == 2 ? ppool_heap_create(NVMTL_PPOOL_BLOCK, &nb->heap) : vram ? vpool_heap_create(NVMTL_VPOOL_BLOCK, &nb->heap)
                                 : nvmtl_vk_heap_create(NVMTL_POOL_BLOCK, 1, &nb->heap))) { free(nb); pthread_mutex_unlock(&g_pool_lock); return -1; }
        nb->vram = vram; nb->next = *head; *head = nb; g_pool_blocks++; if (vram == 1) g_vpool_blocks++; if (vram == 2) g_ppool_blocks++;
        nvlog("buffer pool: block #%llu mapped (%zu MiB, %s; %llu VRAM blocks, memory type %u); %llu pooled buffers live in %llu bytes of ranges",
              (unsigned long long)g_pool_blocks, nb->heap.size >> 20, pool_place_why(vram), (unsigned long long)g_vpool_blocks,
              nb->heap.memtype, (unsigned long long)g_pool_live, (unsigned long long)g_pool_bytes);
        blk = nb; if (!pool_block_take(blk, len, &off)) { pthread_mutex_unlock(&g_pool_lock); return -1; }
    }
    blk->nlive++; g_pool_live++; g_pool_bytes += len;
    pthread_mutex_unlock(&g_pool_lock);
    nvk_buffer pb; memset(&pb, 0, sizeof pb);
    if (nvmtl_vk_buffer_create_placed(size, &blk->heap, off, &pb)) {
        pthread_mutex_lock(&g_pool_lock); blk->nlive--; g_pool_live--; g_pool_bytes -= len; pool_block_give(blk, off, len); pthread_mutex_unlock(&g_pool_lock);
        return -1;
    }
    *out = pb; out->pool = blk; out->poff = off; out->alloc = len; out->size = size;
    if (!out->map && blk->heap.map) out->map = (uint8_t *)blk->heap.map + off;
    if ((g_buf_made & 0xFFF) == 0) nvlog("buffer census: %llu made, %llu gone, %llu live, %zu bytes allocated; pool %llu blocks, %llu pooled live, %llu bytes in ranges",
        (unsigned long long)g_buf_made, (unsigned long long)g_buf_gone, (unsigned long long)(g_buf_made - g_buf_gone), g_alloc_bytes,
        (unsigned long long)g_pool_blocks, (unsigned long long)g_pool_live, (unsigned long long)g_pool_bytes);
    return 0;
}
static int pool_buffer_create(size_t size, nvk_buffer *out) { return pool_buffer_create_in(0, size, out); }
static void pool_buffer_release(nvk_buffer *b)
{
    pool_block *blk = (pool_block *)b->pool; if (!blk) return;
    pthread_mutex_lock(&g_pool_lock);
    blk->nlive--; g_pool_live--; g_pool_bytes -= b->alloc;
    pool_block_give(blk, b->poff, b->alloc);
    if (!blk->nlive) {
        pool_block **head = blk->vram == 2 ? &g_ppool : blk->vram ? &g_vpool : &g_pool;
        uint32_t empties = 0; for (pool_block *p = *head; p; p = p->next) if (!p->nlive) empties++;
        if (empties > NVMTL_POOL_KEEP) {
            pool_block **pp = head; while (*pp && *pp != blk) pp = &(*pp)->next;
            if (*pp) { *pp = blk->next; const size_t hs = blk->heap.size; const int v = blk->vram;
                nvmtl_vk_heap_destroy(&blk->heap); free(blk->fr); free(blk); g_pool_blocks--;
                if (v == 1) { g_vpool_blocks--; g_bar1_bytes -= hs; }
                if (v == 2) { g_ppool_blocks--; g_budget_stale = 1; } }
        }
    }
    pthread_mutex_unlock(&g_pool_lock);
    b->pool = NULL;
}

int nvmtl_vk_image_placed_fits(const nvk_image *img, size_t len)
{
    if (g_state != 1 || !img || !img->img) return 0;
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, (VkImage)img->img, &mr);
    if ((size_t)mr.size <= len) return 1;
    nvlog("vk: placed image %ux%u fmt %u needs %llu B but its range is %zu B - REFUSED (the size query answered for a different image)",
          img->w, img->h, img->fmt, (unsigned long long)mr.size, len);
    return 0;
}
static int nvmtl_vk_image_create_typed_placed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_heap *heap, size_t offset, nvk_image *out);
int nvmtl_vk_image_create_typed_placed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_heap *heap, size_t offset, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_image_create_typed_placed_lifetime_body(w, h, vkfmt, bpp, a8, mips, storage, mtl_type, layers, heap, offset, &pending);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
static int nvmtl_vk_image_create_typed_placed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_heap *heap, size_t offset, nvk_image *out)
{
    if (!layers || (mtl_type < 2 && h != 1) || mtl_type == 4 || mtl_type > 7
        || ((mtl_type == 0 || mtl_type == 2) && layers != 1) || (mtl_type == 5 && layers != 6) || (mtl_type == 6 && layers % 6)) {
        nvlog("image_create: textureType %u with %u layer(s) is not carried", mtl_type, layers); return -1; }
    if (nvmtl_vk_init() || !heap || !heap->mem) return -1;
    if (!mips) mips = 1;
    VkFormat fmt = (VkFormat)vkfmt;
    storage = storage && !a8 && nvmtl_vk_format_is_storage(vkfmt);
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->mips = mips; out->fmt = vkfmt; out->bpp = bpp ? bpp : 4;
    out->storage = storage ? 1 : 0; out->mtl_type = mtl_type; out->layers = layers;
    { uint32_t bb = 0; if (nvmtl_vk_format_block(vkfmt, &out->bw, &out->bh, &bb)) { out->bpp = bb; out->storage = 0; storage = 0; } }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, (mtl_type == 5 || mtl_type == 6) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, mtl_type == 7 ? VK_IMAGE_TYPE_3D : mtl_type < 2 ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
        fmt, { w, h, mtl_type == 7 ? layers : 1 }, mips, mtl_type == 7 ? 1 : layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        (out->bw > 1 ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    nvmtl_vk_image_mutable(&ici, out);
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage(placed)");
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, out->img, &mr);
    if (placed_ok(&mr, heap, offset, "placed image")) { pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1; }
    VKCK(pvkBindImageMemory(g_dev, out->img, (VkDeviceMemory)heap->mem, offset), "vkBindImageMemory(placed)");
    out->mem = heap->mem; out->placed = 1; out->alloc = 0;
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewType viewType = mtl_type == 0 ? VK_IMAGE_VIEW_TYPE_1D : mtl_type == 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : mtl_type == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY
                             : mtl_type == 5 ? VK_IMAGE_VIEW_TYPE_CUBE : mtl_type == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : mtl_type == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, viewType,
        fmt, swz, { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, mtl_type == 7 ? 1 : layers } };
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView(placed)");

    return 0;
}
static int nvmtl_vk_depth_create_ex_placed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_heap *heap, size_t offset, nvk_image *out);
int nvmtl_vk_depth_create_ex_placed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_heap *heap, size_t offset, nvk_image *out)
{
    nvk_image pending = {0};
    int result = nvmtl_vk_depth_create_ex_placed_lifetime_body(w, h, vkfmt, aspect, heap, offset, &pending);
    if (result) { nvmtl_failed_image(&pending); memset(out, 0, sizeof *out); }
    else { nvmtl_commit_image(&pending); *out = pending; }
    return result;
}
static int nvmtl_vk_depth_create_ex_placed_lifetime_body(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_heap *heap, size_t offset, nvk_image *out)
{
    if (nvmtl_vk_init() || !heap || !heap->mem) return -1;
    memset(out, 0, sizeof *out); out->w = w; out->h = h; out->fmt = vkfmt; out->bpp = 4; out->mtl_type = 2; out->layers = 1;
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D,
        (VkFormat)vkfmt, { w, h, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        NVMTL_DEPTH_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VKCK(nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img), "vkCreateImage(placed depth)");
    out->usage = (uint32_t)ici.usage;
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, out->img, &mr);
    if (placed_ok(&mr, heap, offset, "placed depth image")) { pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1; }
    VKCK(pvkBindImageMemory(g_dev, out->img, (VkDeviceMemory)heap->mem, offset), "vkBindImageMemory(placed depth)");
    out->mem = heap->mem; out->placed = 1; out->alloc = 0;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, out->img, VK_IMAGE_VIEW_TYPE_2D,
        (VkFormat)vkfmt, { 0 }, { (VkImageAspectFlags)aspect, 0, 1, 0, 1 } };
    VKCK(nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view), "vkCreateImageView(placed depth)");
    return 0;
}
int nvmtl_vk_buffer_size_align(size_t size, size_t *out_size, size_t *out_align)
{
    if (nvmtl_vk_init()) return -1;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size ? size : 1, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    VKCK(pvkCreateBuffer(g_dev, &bci, NULL, &b), "vkCreateBuffer(size query)");
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, b, &mr); pvkDestroyBuffer(g_dev, b, NULL);
    *out_size = (size_t)mr.size; *out_align = (size_t)(mr.alignment ? mr.alignment : 256);
    return 0;
}
static int nvmtl_vk_image_size_align_uncached(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t mips, uint32_t mtl_type, uint32_t layers, int depth_attachment, int storage, size_t *out_size, size_t *out_align)
{
    if (nvmtl_vk_init()) return -1;
    if (!mips) mips = 1;
    if (!layers) layers = 1;
    uint32_t qbw = 1, qbh = 1, qbb = 0; int compressed = nvmtl_vk_format_block(vkfmt, &qbw, &qbh, &qbb);
    storage = storage && !depth_attachment && !compressed && nvmtl_vk_format_is_storage(vkfmt);
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, (!depth_attachment && (mtl_type == 5 || mtl_type == 6)) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0, (!depth_attachment && mtl_type == 7) ? VK_IMAGE_TYPE_3D : (!depth_attachment && mtl_type < 2) ? VK_IMAGE_TYPE_1D : VK_IMAGE_TYPE_2D,
        (VkFormat)vkfmt, { w, h, (!depth_attachment && mtl_type == 7) ? layers : 1 }, depth_attachment ? 1 : mips, (depth_attachment || mtl_type == 7) ? 1 : layers, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        depth_attachment ? NVMTL_DEPTH_USAGE
                         : ((compressed ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0)),
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage im = VK_NULL_HANDLE;
    if (!depth_attachment) ici.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    if (!depth_attachment && nvmtl_vk_cube_compat(mtl_type, w, h, layers))
        ici.flags |= VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (!depth_attachment && ici.imageType == VK_IMAGE_TYPE_3D && (ici.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)
        && nvmtl_vk_3d_slice_ok(ici.format, ici.usage))
        ici.flags |= VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    VKCK(nvmtl_typed_CreateImage(g_dev, &ici, NULL, &im), "vkCreateImage(size query)");
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, im, &mr); pvkDestroyImage(g_dev, im, NULL);
    *out_size = (size_t)mr.size; *out_align = (size_t)(mr.alignment ? mr.alignment : 256);
    return 0;
}

typedef struct {
    VkDevice dev;
    uint32_t key[8];
    size_t size, align;
    int valid;
} nvmtl_sizequery_entry;
static nvmtl_sizequery_entry nvmtl_sizequeries[256];
static unsigned nvmtl_sizequery_next;
static pthread_mutex_t nvmtl_sizequery_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic unsigned long g_sq_hit, g_sq_miss;
int nvmtl_vk_image_size_align(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t mips, uint32_t mtl_type, uint32_t layers, int depth_attachment, int storage, size_t *out_size, size_t *out_align)
{
    if (nvmtl_vk_init()) return -1;
    const uint32_t key[8] = {w, h, vkfmt, mips, mtl_type, layers,
                            (uint32_t)depth_attachment, (uint32_t)storage};
    const char *disable = getenv("NVMTL_NO_SIZEQUERY_CACHE");
    const int enabled = !(disable && disable[0] && disable[0] != '0');
    if (enabled) {
        pthread_mutex_lock(&nvmtl_sizequery_lock);
        for (unsigned i = 0; i < 256; ++i) {
            const nvmtl_sizequery_entry *e = &nvmtl_sizequeries[i];
            if (e->valid && e->dev == g_dev && !memcmp(e->key, key, sizeof key)) {
                *out_size = e->size; *out_align = e->align;
                pthread_mutex_unlock(&nvmtl_sizequery_lock);
                unsigned long hn = ++g_sq_hit;
                if (!(hn & (hn - 1))) nvlog("sizequery: HIT %ux%u fmt %u -> %zu/%zu [%lu hit, %lu miss]", w, h, vkfmt, *out_size, *out_align, hn, (unsigned long)g_sq_miss);
                return 0;
            }
        }
        pthread_mutex_unlock(&nvmtl_sizequery_lock);
    }
    const int rc = nvmtl_vk_image_size_align_uncached(w, h, vkfmt, mips, mtl_type,
                      layers, depth_attachment, storage, out_size, out_align);
    unsigned long mn = ++g_sq_miss;
    if (!(mn & (mn - 1))) nvlog("sizequery: %s %ux%u fmt %u rc %d [%lu hit, %lu miss]", enabled ? "miss" : "OFF", w, h, vkfmt, rc, (unsigned long)g_sq_hit, mn);
    if (!rc && enabled) {
        pthread_mutex_lock(&nvmtl_sizequery_lock);
        nvmtl_sizequery_entry *e = &nvmtl_sizequeries[nvmtl_sizequery_next++ % 256];
        e->dev = g_dev; memcpy(e->key, key, sizeof key);
        e->size = *out_size; e->align = *out_align; e->valid = 1;
        pthread_mutex_unlock(&nvmtl_sizequery_lock);
    }
    return rc;
}

/* Texture-to-texture copy with a depth range. A 3D image keeps its depth in ->layers; a Metal copy into or out of a 3D texture names
   the plane with origin.z and the plane count with size.depth (slice must be 0). WAS: z and depth were dropped, so every plane of a
   3D destination landed on plane 0 (Factorio builds its night colour-grading LUT, a 16x16x16 3D texture, from sixteen 16x16 copies). */
int nvmtl_vk_cmd_copy_image_sub3(nvk_cmdbuf *c, nvk_image *src, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy, uint32_t sz,
                                 nvk_image *dst, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t dz,
                                 uint32_t w, uint32_t h, uint32_t depth)
{
    if (!depth) depth = 1;
    const int s3 = src && src->mtl_type == 7, d3 = dst && dst->mtl_type == 7;
    /* Two formats join when vkCmdCopyImage accepts them (nvmtl_copy_compat.h): the same texel size, or the same BC class.
     * WAS: an identical VkFormat only, so a Metal copy between an sRGB texture and its linear twin, a channel-order twin, or
     * a view in another format of the same size was dropped and its destination kept the bytes it held before.
     * NVMTL_COPY_EXACT_FORMAT=1 restores the exact-match refusal. */
    static int exact = -1;
    if (exact < 0) { const char *e = getenv("NVMTL_COPY_EXACT_FORMAT"); exact = e && e[0] == '1';
                     nvlog("copy_image_sub: formats join when %s (NVMTL_COPY_EXACT_FORMAT=%s)", exact ? "identical" : "size-compatible, as vkCmdCopyImage allows", e ? e : "unset"); }
    const int fmt_ok = src && dst && (exact ? src->fmt == dst->fmt : nvmtl_copy_formats_ok(src->fmt, dst->fmt));
    if (!c || !src || !dst || !w || !h || !fmt_ok
        || (src->samples ? src->samples : 1) != (dst->samples ? dst->samples : 1)
        || ss >= (src->layers ? src->layers : 1) || ds >= (dst->layers ? dst->layers : 1)
        || sl >= (src->mips ? src->mips : 1) || dl >= (dst->mips ? dst->mips : 1)) {
        static _Atomic unsigned long long refused;
        const unsigned long long k = atomic_fetch_add(&refused, 1) + 1;
        if (!src || !dst) nvlog("copy_image_sub: REFUSED - %s image is missing (%llu refused so far)", src ? "destination" : "source", k);
        else nvlog("copy_image_sub: REFUSED (%llu so far) - format %u -> %u%s, samples %u -> %u, layer %u of %u -> %u of %u, level %u of %u -> %u of %u, "
                   "%ux%u (src %ux%u type %u, dst %ux%u type %u)", k, src->fmt, dst->fmt, fmt_ok ? "" : " (not size-compatible)",
                   src->samples, dst->samples, ss, src->layers, ds, dst->layers, sl, src->mips, dl, dst->mips, w, h,
                   src->w, src->h, src->mtl_type, dst->w, dst->h, dst->mtl_type);
        return -1;
    }
    if (src->fmt != dst->fmt) {
        static _Atomic unsigned long long joined;
        const unsigned long long k = atomic_fetch_add(&joined, 1) + 1;
        if (!(k & (k - 1))) nvlog("copy_image_sub: %llu copy(ies) between size-compatible formats so far (latest %u -> %u, %ux%u)", k, src->fmt, dst->fmt, w, h);
    }
    const uint32_t sw = nvmtl_lvl(src->w, sl), sh = nvmtl_lvl(src->h, sl);
    const uint32_t dw = nvmtl_lvl(dst->w, dl), dh = nvmtl_lvl(dst->h, dl);
    {   /* Metal sizes a block-compressed copy in whole blocks: the 1x1 and 2x2 mips of a BC texture are copied as 4x4
         * (Unity's mip chains, Cult of the Lamb's font atlases). Vulkan wants the extent to end at the mip's edge instead;
         * clamp a region that runs past the edge by less than one block. WAS: "region outside selected mip extent". */
        uint32_t bw = 1, bh = 1, bb = 0;
        if (nvmtl_vk_format_block(src->fmt, &bw, &bh, &bb) && (bw > 1 || bh > 1)) {
            const uint32_t rw = (sw + bw - 1) / bw * bw, rh = (sh + bh - 1) / bh * bh;
            const uint32_t rdw = (dw + bw - 1) / bw * bw, rdh = (dh + bh - 1) / bh * bh;
            if (sx < sw && dx < dw && w <= rw - sx && w <= rdw - dx) { if (w > sw - sx) w = sw - sx; if (w > dw - dx) w = dw - dx; }
            if (sy < sh && dy < dh && h <= rh - sy && h <= rdh - dy) { if (h > sh - sy) h = sh - sy; if (h > dh - dy) h = dh - dy; }
            // Cropping at the destination edge must not split a source block. Offsets stay block-aligned;
            // a partial source block is valid only at the source mip's edge (vkCmdCopyImage-01728/01729).
            if (sx % bw || dx % bw || sy % bh || dy % bh
                || (w % bw && (uint64_t)sx + w != sw)
                || (h % bh && (uint64_t)sy + h != sh)) {
                nvlog("copy_image_sub: compressed region is not block-aligned or at the source mip edge");
                return -1;
            }
        }
    }
    if (sx > sw || w > sw - sx || sy > sh || h > sh - sy
        || dx > dw || w > dw - dx || dy > dh || h > dh - dy) {
        nvlog("copy_image_sub: region outside selected mip extent");
        return -1;
    }
    {   /* planes: a 3D image has nvmtl_lvl(layers, level) of them; a 2D image only plane 0 */
        const uint32_t sd = s3 ? nvmtl_lvl(src->layers ? src->layers : 1, sl) : 1u, dd = d3 ? nvmtl_lvl(dst->layers ? dst->layers : 1, dl) : 1u;
        if ((s3 ? (sz > sd || depth > sd - sz) : (sz != 0 || (!d3 && depth != 1)))
            || (d3 ? (dz > dd || depth > dd - dz) : (dz != 0 || (!s3 && depth != 1)))) {
            nvlog("copy_image_sub: depth range z %u/%u +%u is outside the 3D extent (%u / %u planes)", sz, dz, depth, sd, dd);
            return -1;
        }
    }

    const VkImageAspectFlags aspects = nvmtl_barrier_aspect(src);
    VkImageCopy regions[2]; uint32_t count = 0;
    for (VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
         aspect <= VK_IMAGE_ASPECT_STENCIL_BIT; aspect <<= 1) {
        if (!(aspects & aspect)) continue;
        /* Vulkan: a 3D image uses base layer 0 / layer count 1 and offset.z; a 2D side of a 2D<->3D copy carries the plane count as its layerCount */
        regions[count++] = (VkImageCopy){ { aspect, sl, s3 ? 0u : ss, s3 ? 1u : (d3 ? depth : 1u) }, { (int32_t)sx, (int32_t)sy, s3 ? (int32_t)sz : 0 },
                                         { aspect, dl, d3 ? 0u : ds, d3 ? 1u : (s3 ? depth : 1u) }, { (int32_t)dx, (int32_t)dy, d3 ? (int32_t)dz : 0 },
                                         { w, h, depth } };
    }
    nvmtl_vk_cmd_barrier(c);
    img_to(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    img_to(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    pvkCmdCopyImage(c->cb, (VkImage)src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   (VkImage)dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, count, regions);
    img_to(c, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}
int nvmtl_vk_srgb_format(uint32_t fmt)
{
    switch (fmt) {
    case VK_FORMAT_R8_SRGB: case VK_FORMAT_R8G8_SRGB: case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_B8G8R8A8_SRGB:
    case VK_FORMAT_A8B8G8R8_SRGB_PACK32: return 1;
    default: return 0;
    }
}
int nvmtl_vk_cmd_blit_image_2d(nvk_cmdbuf *c, nvk_image *src, nvk_image *dst, uint32_t w, uint32_t h)
{
    if (!c || !src || !dst || !src->img || !dst->img || !w || !h || w > src->w || h > src->h || w > dst->w || h > dst->h
        || (src->samples ? src->samples : 1) > 1 || (dst->samples ? dst->samples : 1) > 1) {
        nvlog("blit_image_2d: refused (%ux%u, VkFormat %u -> %u)", w, h, src ? src->fmt : 0, dst ? dst->fmt : 0);
        return -1;
    }
    const VkImageBlit r = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } },
                            { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { { 0, 0, 0 }, { (int32_t)w, (int32_t)h, 1 } } };
    nvmtl_vk_cmd_barrier(c);
    img_to(c, src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    img_to(c, dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    pvkCmdBlitImage(c->cb, (VkImage)src->img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    (VkImage)dst->img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &r, VK_FILTER_NEAREST);
    img_to(c, dst, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
           VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    nvmtl_vk_cmd_barrier(c);
    return 0;
}
int nvmtl_vk_cmd_copy_image_sub(nvk_cmdbuf *c, nvk_image *src, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy,
                                nvk_image *dst, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h)
{
    return nvmtl_vk_cmd_copy_image_sub3(c, src, sl, ss, sx, sy, 0, dst, dl, ds, dx, dy, 0, w, h, 1);
}
int nvmtl_vk_image_view_create_range(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount,
                                     uint32_t baseLayer, uint32_t layerCount, uint32_t mtl_view_type, void **out_view)
{
    nvk_image format_image = {.fmt = vkfmt};
    return nvmtl_vk_image_view_create_range_aspect(img, vkfmt, a8, nvmtl_copy_aspect(&format_image), baseLevel, levelCount, baseLayer, layerCount, mtl_view_type, out_view);
}
int nvmtl_vk_image_view_create_range_aspect(nvk_image *img, uint32_t vkfmt, int a8, uint32_t aspect, uint32_t baseLevel, uint32_t levelCount,
                                     uint32_t baseLayer, uint32_t layerCount, uint32_t mtl_view_type, void **out_view)
{
    if (!img || !out_view || nvmtl_vk_init()) return -1;
    nvk_image format_image = {.fmt = vkfmt};
    if (!aspect || (aspect & ~nvmtl_barrier_aspect(&format_image))) return -1;
    const uint32_t mips = img->mips ? img->mips : 1, layers = img->layers ? img->layers : 1;
    if (baseLevel >= mips || baseLayer >= layers) return -1;
    if (!levelCount) levelCount = mips - baseLevel;
    if (!layerCount) layerCount = layers - baseLayer;
    if (levelCount > mips - baseLevel) { nvlog("image_view_create(range): levels [%u,%u) is outside the %u level(s)", baseLevel, baseLevel + levelCount, mips); return -1; }
    if (layerCount > layers - baseLayer) { nvlog("image_view_create(range): slices [%u,%u) is outside the %u slice(s)", baseLayer, baseLayer + layerCount, layers); return -1; }
    VkImageViewType vt;
    switch (mtl_view_type) {
        case 0: vt = VK_IMAGE_VIEW_TYPE_1D; break;
        case 1: vt = VK_IMAGE_VIEW_TYPE_1D_ARRAY; break;
        case 2: vt = VK_IMAGE_VIEW_TYPE_2D; if (layerCount != 1) { nvlog("image_view_create(range): a 2D view of %u slices", layerCount); return -1; } break;
        case 3: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case 4: vt = VK_IMAGE_VIEW_TYPE_2D; if ((img->mtl_type != 4 && img->mtl_type != 8) || layerCount != 1 || baseLevel != 0 || levelCount != 1) { nvlog("image_view_create(range): a multisample view of a %u-typed image / %u slices", img->mtl_type, layerCount); return -1; } break;
        case 8: vt = VK_IMAGE_VIEW_TYPE_2D_ARRAY; if (img->mtl_type != 8 || baseLevel != 0 || levelCount != 1) return -1; break;
        case 5: vt = VK_IMAGE_VIEW_TYPE_CUBE; if (layerCount != 6) { nvlog("image_view_create(range): a cube view of %u slices", layerCount); return -1; } break;
        case 6: vt = VK_IMAGE_VIEW_TYPE_CUBE_ARRAY; if (layerCount % 6) { nvlog("image_view_create(range): a cube-array view of %u slices", layerCount); return -1; } break;
        case 7: vt = VK_IMAGE_VIEW_TYPE_3D; if (img->mtl_type != 7) { nvlog("image_view_create(range): a 3D view of a non-3D image"); return -1; } layerCount = 1; break;
        default: nvlog("image_view_create(range): view textureType %u is not carried", mtl_view_type); return -1;
    }
    VkComponentMapping swz = nvmtl_a8_mapping(a8);
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)img->img, vt, (VkFormat)vkfmt, swz,
        { (VkImageAspectFlags)aspect, baseLevel, levelCount, baseLayer, layerCount } };
    VkImageViewUsageCreateInfo uci;
    int vg = nvmtl_vk_view_guard(img, vkfmt, a8, vci.viewType, vci.subresourceRange.levelCount, &uci, "image_view_create(range)");
    if (vg < 0) return -1;
    if (vg > 0) vci.pNext = &uci;
    VkImageView v = VK_NULL_HANDLE;
    VKCK(pvkCreateImageView(g_dev, &vci, NULL, &v), "vkCreateImageView(range)");
    *out_view = v;
    return 0;
}

static void nvmtl_destroy_pipe_once(VkPipeline *seen, uint32_t *nseen, uint32_t cap, VkPipeline h)
{
    if (!h) return;
    for (uint32_t i = 0; i < *nseen; i++) if (seen[i] == h) return;
    if (*nseen < cap) seen[(*nseen)++] = h;
    pvkDestroyPipeline(g_dev, h, NULL);
}
void nvmtl_vk_pipeline_destroy(nvk_pipeline *p)
{
    if (!p || !g_dev) return;
    VkPipeline seen[64]; uint32_t nseen = 0;
    pthread_mutex_lock(&g_var_lock);
    for (uint32_t i = 0; i < p->nvar; i++) nvmtl_destroy_pipe_once(seen, &nseen, 64, (VkPipeline)p->var[i].pipe);
    free(p->var); p->var = NULL; p->nvar = p->capvar = 0;
    pthread_mutex_unlock(&g_var_lock);
    nvmtl_destroy_pipe_once(seen, &nseen, 64, (VkPipeline)p->pipe);
    pthread_mutex_lock(&g_ag_lock);
    nvk_pipeline *safe = (nvk_pipeline *)p->ag_safe; p->ag_safe = NULL;
    pthread_mutex_unlock(&g_ag_lock);
    if (safe) { nvmtl_vk_pipeline_destroy(safe); free(safe); }
    pthread_mutex_lock(&g_ubo_lock);
    nvk_pipeline *twin = (nvk_pipeline *)p->ubo_twin; p->ubo_twin = NULL;
    void *usrc = p->ubo_src; p->ubo_src = NULL;
    pthread_mutex_unlock(&g_ubo_lock);
    if (twin) { nvmtl_vk_pipeline_destroy(twin); free(twin); }
    nvmtl_ubo_src_free(usrc);
    pthread_mutex_lock(&g_rpv_lock);
    for (uint32_t i = 1; i < 4; i++) if (p->rpv[i] && p->rpv[i] != p->rp) pvkDestroyRenderPass(g_dev, (VkRenderPass)p->rpv[i], NULL);
    for (uint32_t i = 0; i < p->nrpx; i++) if (p->rpx[i] && p->rpx[i] != p->rp) pvkDestroyRenderPass(g_dev, (VkRenderPass)p->rpx[i], NULL);
    free(p->rpx); free(p->rpxk); p->rpx = NULL; p->rpxk = NULL; p->nrpx = p->crpx = 0;
    if (p->shape_identity) {
        uint32_t w = 0;
        for (uint32_t i = 0; i < g_nsrp; i++) {
            if (g_srp[i].identity == p->shape_identity) { if (g_srp[i].rp) pvkDestroyRenderPass(g_dev, (VkRenderPass)g_srp[i].rp, NULL); }
            else g_srp[w++] = g_srp[i];
        }
        g_nsrp = w;
    }
    pthread_mutex_unlock(&g_rpv_lock);
    if (p->rp) pvkDestroyRenderPass(g_dev, (VkRenderPass)p->rp, NULL);
    if (p->mod) pvkDestroyShaderModule(g_dev, (VkShaderModule)p->mod, NULL);
    if (p->layout) pvkDestroyPipelineLayout(g_dev, (VkPipelineLayout)p->layout, NULL);
    free(p->ag_src);
    memset(p, 0, sizeof *p);
    static _Atomic unsigned long long freed;
    const unsigned long long k = atomic_fetch_add(&freed, 1) + 1;
    if (!(k & (k - 1))) nvlog("psofree: %llu pipeline state(s) freed after their last command buffer completed", k);
}
#include "nvmtl_icb_dgc.c"   /* NVMTL_ICB_DGC=1: GPU-encoded ICBs through VK_EXT_device_generated_commands */
