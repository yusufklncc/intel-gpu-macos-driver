// SPDX-License-Identifier: MIT
// vkscene: a native Vulkan renderer path on macOS, the way a game engine or Minecraft's Vulkan renderer drives the
// driver, small enough to read. Checks what vksurf (clears only) does not: a pipeline with vertex/index buffers,
// MSAA with a resolve into the swapchain image, a depth buffer, a bindless texture array (descriptor indexing:
// runtime array, variable count, partially bound, update after bind, nonuniformEXT), per-frame instance data through
// a dynamic storage buffer, Vulkan 1.3 dynamic rendering and synchronization2, timeline semaphores for frame pacing
// (no fences) and a GPU wait-before-host-signal, GPU timestamps.
//
//   1. a check pass into a 256x256 offscreen target, verified pixel by pixel: 64 cells each showing texel (0,0) of
//      texture[cell] (descriptors 32-63 written after the command buffer was recorded: update after bind), a far
//      magenta layer that must lose the depth test, a near triangle that must win it, and partial coverage along
//      its edge that the MSAA resolve must blend;
//   2. N frames of 256 instanced, textured, lit cubes (FIFO by default), timed: present-to-present interval
//      (avg, p50, p95, p99, max), GPU time per frame from timestamps, acquire and timeline-wait times;
//   3. a last frame rendered with the same pipeline into an offscreen image, read back (an FNV-1a checksum, the same
//      on every path that reaches the same driver) and copied into the swapchain; the IOSurface ANV's WSI shows is
//      then compared with the read-back pixel for pixel.
//
//   vkscene [how to load Vulkan] [options]
//     --icd PATH        an ICD through vk_icdGetInstanceProcAddr (default: /Library/GPUBundles/nvmtl/libvulkan_intel.dylib)
//     --lib PATH|NAME   a library exporting vkGetInstanceProcAddr, opened with dlopen as a game or Wine opens it:
//                       the provider (libintelxe-vk.dylib, or a bare name through DYLD_LIBRARY_PATH) or the Khronos
//                       loader (/usr/local/lib/libvulkan.1.dylib with VK_DRIVER_FILES)
//     --frames N        default 300;  --mailbox / --immediate  present mode (default FIFO)
//     --size WxH        default 800x600 (the checksum depends on it);  --grid G  G x G cubes (default 16)
//     --samples N       MSAA samples (default 4, 1 = none);  --inflight N  frames in flight (default 2)
//     --resize          resize the window halfway (the swapchain must be recreated)
//     --mtl-device      give the CAMetalLayer a Metal device, as Wine does (loads the Metal driver into the process);
//                       default: none, as GLFW and SDL (Minecraft) create their layer
//     --png FILE        write the read-back frame as a PNG (to look at it)
//     --no-wbs          skip the GPU wait on a host signal at the start. That wait (as vkd3d-proton and DXVK do it)
//                       gives Mesa's queue a submit thread for the rest of the run, and with it the WAIT_PENDING and
//                       SYNCOBJ_TRANSFER paths a present without a blit takes: a kernel answering a point-0
//                       TIMELINE_WAIT too early makes such a present fail (VK_ERROR_UNKNOWN; IntelXe before 14 Nov)
// The driver's own messages (VK_EXT_debug_utils, warnings and errors: Mesa's vk_errorf text) are printed as "driver:".
// Prints "RESULT vkscene ..." and PASS/FAIL; exit status 0 on PASS.
// Build: vulkan/build.sh (bin/vkscene), or clang -arch x86_64 -fobjc-arc -I<vulkan headers> vkscene.m -framework
//        AppKit -framework QuartzCore -framework Metal -framework IOSurface -o vkscene. Shaders: vkscene/*.vert|frag,
//        compiled into vkscene/spv.h by vkscene/gen.sh.
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#include <vulkan/vulkan.h>
#include "vkscene/spv.h"

#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)
#define NTEX 64          // textures in the bindless array
#define MAXTEX 1024      // the variable-count binding's upper bound
#define TEXSZ 64
#define CHK 256          // check-pass target size
#define MAXF 4           // frames in flight, at most

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;

#define INSTFNS(X) X(vkEnumeratePhysicalDevices) X(vkGetPhysicalDeviceProperties) X(vkGetPhysicalDeviceFeatures2) \
    X(vkGetPhysicalDeviceQueueFamilyProperties) X(vkGetPhysicalDeviceSurfaceSupportKHR) X(vkGetPhysicalDeviceMemoryProperties) \
    X(vkGetPhysicalDeviceFormatProperties) X(vkGetPhysicalDeviceSurfaceCapabilitiesKHR) X(vkGetPhysicalDeviceSurfaceFormatsKHR) \
    X(vkGetPhysicalDeviceSurfacePresentModesKHR) X(vkEnumerateDeviceExtensionProperties) X(vkCreateDevice) \
    X(vkDestroySurfaceKHR) X(vkDestroyInstance) X(vkCreateMetalSurfaceEXT)
#define DEVFNS(X) X(vkGetDeviceQueue) X(vkCreateCommandPool) X(vkAllocateCommandBuffers) X(vkBeginCommandBuffer) \
    X(vkEndCommandBuffer) X(vkResetCommandBuffer) X(vkQueueSubmit2) X(vkQueuePresentKHR) X(vkAcquireNextImageKHR) \
    X(vkCreateSemaphore) X(vkDestroySemaphore) X(vkWaitSemaphores) X(vkSignalSemaphore) X(vkGetSemaphoreCounterValue) \
    X(vkDeviceWaitIdle) X(vkQueueWaitIdle) X(vkCreateSwapchainKHR) X(vkDestroySwapchainKHR) X(vkGetSwapchainImagesKHR) \
    X(vkCreateImage) X(vkDestroyImage) X(vkCreateImageView) X(vkDestroyImageView) X(vkGetImageMemoryRequirements) \
    X(vkBindImageMemory) X(vkCreateBuffer) X(vkDestroyBuffer) X(vkGetBufferMemoryRequirements) X(vkBindBufferMemory) \
    X(vkAllocateMemory) X(vkFreeMemory) X(vkMapMemory) X(vkCreateSampler) X(vkDestroySampler) \
    X(vkCreateDescriptorSetLayout) X(vkDestroyDescriptorSetLayout) X(vkCreateDescriptorPool) X(vkDestroyDescriptorPool) \
    X(vkAllocateDescriptorSets) X(vkUpdateDescriptorSets) X(vkCreatePipelineLayout) X(vkDestroyPipelineLayout) \
    X(vkCreateShaderModule) X(vkDestroyShaderModule) X(vkCreateGraphicsPipelines) X(vkDestroyPipeline) \
    X(vkCreateQueryPool) X(vkDestroyQueryPool) X(vkGetQueryPoolResults) X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp2) \
    X(vkCmdPipelineBarrier2) X(vkCmdBeginRendering) X(vkCmdEndRendering) X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) \
    X(vkCmdBindVertexBuffers) X(vkCmdBindIndexBuffer) X(vkCmdPushConstants) X(vkCmdSetViewport) X(vkCmdSetScissor) \
    X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdCopyBuffer) X(vkCmdCopyBufferToImage) X(vkCmdCopyImageToBuffer) \
    X(vkCmdCopyImage) X(vkCmdFillBuffer) X(vkDestroyCommandPool) X(vkDestroyDevice)
#define DECL(n) static PFN_##n n;
INSTFNS(DECL)
DEVFNS(DECL)

static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkPhysicalDeviceProperties props;
static VkPhysicalDeviceMemoryProperties memprops;
static VkSurfaceKHR surf;
static VkSwapchainKHR sc;
static VkFormat scfmt, depthfmt;
static VkExtent2D ext;
static uint32_t nimg;
static VkImage imgs[16];
static VkImageView views[16];
static VkSemaphore done[16];
static VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
static VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_4_BIT;
static int fails, memfallbacks, wbs_ran;

typedef struct { VkImage img; VkDeviceMemory mem; VkImageView view; } Img;
typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } Buf;
static Img mscolor, depth;   // per swapchain size, shared by the frames in flight (ordered by barriers)

static VKAPI_ATTR VkBool32 VKAPI_CALL on_message(VkDebugUtilsMessageSeverityFlagBitsEXT sev, VkDebugUtilsMessageTypeFlagsEXT type,
                                                 const VkDebugUtilsMessengerCallbackDataEXT *d, void *user) {
    (void)type; (void)user;
    printf("driver: %s %s\n", sev >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? "error" : "warning", d && d->pMessage ? d->pMessage : "?");
    fflush(stdout);
    return VK_FALSE;
}

static const char *memflags(VkMemoryPropertyFlags f, char *buf, size_t n) {
    snprintf(buf, n, "%s%s%s%s%s", f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? "DEVICE_LOCAL " : "",
             f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT ? "HOST_VISIBLE " : "", f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? "COHERENT " : "",
             f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT ? "CACHED " : "", f ? "" : "none");
    return buf;
}

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static void pump(void) {
    NSEvent *e;
    while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))
        [NSApp sendEvent:e];
}

// ---- matrices, column-major as GLSL reads them
typedef struct { float m[16]; } M4;
static M4 m4_mul(M4 a, M4 b) {
    M4 r;
    for (int c = 0; c < 4; c++)
        for (int k = 0; k < 4; k++) {
            float s = 0;
            for (int i = 0; i < 4; i++) s += a.m[i * 4 + k] * b.m[c * 4 + i];
            r.m[c * 4 + k] = s;
        }
    return r;
}
// Vulkan clip space: depth 0..1, y down (the projection flips y)
static M4 m4_persp(float fovy, float aspect, float n, float f) {
    float t = 1.0f / tanf(fovy / 2);
    M4 r = {{0}};
    r.m[0] = t / aspect; r.m[5] = -t; r.m[10] = f / (n - f); r.m[11] = -1; r.m[14] = n * f / (n - f);
    return r;
}
static void v3_norm(float *v) { float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); v[0] /= l; v[1] /= l; v[2] /= l; }
static void v3_cross(const float *a, const float *b, float *o) {
    o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0];
}
static M4 m4_lookat(const float *eye, const float *at, const float *up) {
    float f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]}, s[3], u[3];
    v3_norm(f); v3_cross(f, up, s); v3_norm(s); v3_cross(s, f, u);
    M4 r = {{s[0], u[0], -f[0], 0, s[1], u[1], -f[1], 0, s[2], u[2], -f[2], 0,
             -(s[0] * eye[0] + s[1] * eye[1] + s[2] * eye[2]), -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2]),
             f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2], 1}};
    return r;
}
// translation * rotation about a unit axis * uniform scale
static M4 m4_model(float tx, float ty, float tz, const float *ax, float ang, float sc_) {
    float c = cosf(ang), s = sinf(ang), t = 1 - c, x = ax[0], y = ax[1], z = ax[2];
    M4 r = {{(t * x * x + c) * sc_, (t * x * y + s * z) * sc_, (t * x * z - s * y) * sc_, 0,
             (t * x * y - s * z) * sc_, (t * y * y + c) * sc_, (t * y * z + s * x) * sc_, 0,
             (t * x * z + s * y) * sc_, (t * y * z - s * x) * sc_, (t * z * z + c) * sc_, 0, tx, ty, tz, 1}};
    return r;
}

// ---- memory, images, buffers
static uint32_t memtype(uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags nice) {
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++) {
            VkMemoryPropertyFlags f = memprops.memoryTypes[i].propertyFlags, need = pass ? want : want | nice;
            if ((bits & (1u << i)) && (f & need) == need) return i;
        }
    printf("FAIL no memory type with flags 0x%x (bits 0x%x)\n", want, bits);
    exit(1);
}

static Img make_image(VkFormat f, VkExtent2D e, VkSampleCountFlagBits s, VkImageUsageFlags u, VkImageAspectFlags asp) {
    Img r = {0};
    VkImageCreateInfo ci = {VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, 0, VK_IMAGE_TYPE_2D, f, {e.width, e.height, 1}, 1, 1, s,
        VK_IMAGE_TILING_OPTIMAL, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED};
    CHECK(vkCreateImage(dev, &ci, NULL, &r.img));
    VkMemoryRequirements mr;
    vkGetImageMemoryRequirements(dev, r.img, &mr);
    VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0)};
    CHECK(vkAllocateMemory(dev, &ai, NULL, &r.mem));
    CHECK(vkBindImageMemory(dev, r.img, r.mem, 0));
    VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, r.img, VK_IMAGE_VIEW_TYPE_2D, f, {0}, {asp, 0, 1, 0, 1}};
    CHECK(vkCreateImageView(dev, &vi, NULL, &r.view));
    return r;
}
static void free_image(Img *i) {
    if (i->view) vkDestroyImageView(dev, i->view, NULL);
    if (i->img) vkDestroyImage(dev, i->img, NULL);
    if (i->mem) vkFreeMemory(dev, i->mem, NULL);
    memset(i, 0, sizeof *i);
}

// host: 0 device-local, 1 host-visible coherent, 2 the same, cached if there is such a type. If allocating or binding
// in the preferred type fails, the other types the buffer allows (and that meet a host buffer's needs) are tried, as
// VMA does; each fallback is printed and counted (memfallback= in the RESULT line).
static Buf make_buffer(const char *what, VkDeviceSize sz, VkBufferUsageFlags u, int host) {
    Buf r = {0};
    r.size = sz;
    VkBufferCreateInfo ci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, sz, u, VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    CHECK(vkCreateBuffer(dev, &ci, NULL, &r.buf));
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(dev, r.buf, &mr);
    const VkMemoryPropertyFlags need = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : 0;
    uint32_t first = host ? memtype(mr.memoryTypeBits, need, host == 2 ? VK_MEMORY_PROPERTY_HOST_CACHED_BIT : 0)
                          : memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0);
    VkResult ra = VK_SUCCESS, rb = VK_SUCCESS;
    char fb[96];
    for (int k = -1; k < (int)memprops.memoryTypeCount; k++) {
        uint32_t t = k < 0 ? first : (uint32_t)k;
        if (k >= 0 && (t == first || !(mr.memoryTypeBits & (1u << t)) || (memprops.memoryTypes[t].propertyFlags & need) != need)) continue;
        VkMemoryAllocateInfo ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t};
        if ((ra = vkAllocateMemory(dev, &ai, NULL, &r.mem)) != VK_SUCCESS) {
            printf("note: %s: vkAllocateMemory(%llu bytes, type %u: %s) = %d\n", what, (unsigned long long)mr.size, t,
                   memflags(memprops.memoryTypes[t].propertyFlags, fb, sizeof fb), ra);
            continue;
        }
        if ((rb = vkBindBufferMemory(dev, r.buf, r.mem, 0)) != VK_SUCCESS) {
            printf("note: %s: vkBindBufferMemory(%llu bytes, usage 0x%x, type %u: %s, heap %u) = %d\n", what,
                   (unsigned long long)sz, u, t, memflags(memprops.memoryTypes[t].propertyFlags, fb, sizeof fb),
                   memprops.memoryTypes[t].heapIndex, rb);
            vkFreeMemory(dev, r.mem, NULL);
            r.mem = VK_NULL_HANDLE;
            continue;
        }
        if (k >= 0) {
            printf("note: %s: in memory type %u (%s) instead of %u\n", what, t, memflags(memprops.memoryTypes[t].propertyFlags, fb, sizeof fb), first);
            memfallbacks++;
        }
        break;
    }
    if (!r.mem) { printf("FAIL %s: no memory type took the buffer (last vkAllocateMemory %d, vkBindBufferMemory %d)\n", what, ra, rb); exit(1); }
    if (host) CHECK(vkMapMemory(dev, r.mem, 0, VK_WHOLE_SIZE, 0, &r.map));
    return r;
}
static void free_buffer(Buf *b) { vkDestroyBuffer(dev, b->buf, NULL); vkFreeMemory(dev, b->mem, NULL); memset(b, 0, sizeof *b); }

static void barrier(VkCommandBuffer cb, VkImage img, VkImageAspectFlags asp, VkImageLayout from, VkImageLayout to,
                    VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkImageMemoryBarrier2 b = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da, from, to,
        VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, {asp, 0, 1, 0, 1}};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, 1, &b};
    vkCmdPipelineBarrier2(cb, &di);
}
static void mem_barrier(VkCommandBuffer cb, VkPipelineStageFlags2 ss, VkAccessFlags2 sa, VkPipelineStageFlags2 ds, VkAccessFlags2 da) {
    VkMemoryBarrier2 b = {VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, NULL, ss, sa, ds, da};
    VkDependencyInfo di = {VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 1, &b, 0, NULL, 0, NULL};
    vkCmdPipelineBarrier2(cb, &di);
}

// ---- the scene's data
typedef struct { float pos[3], nrm[3], uv[2]; } Vtx;
typedef struct { float model[16]; float tint[4]; uint32_t tex, pad[3]; } Inst;   // std430, 96 bytes
typedef struct { float viewproj[16]; float light[4]; int32_t mode; float depth; int32_t pad[2]; } Push;

static void cube(Vtx *v, uint16_t *idx) {
    static const float n[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (int f = 0; f < 6; f++) {
        const float *N = n[f];
        float u[3] = {N[1] != 0 ? 1.0f : 0, 0, 0}, w[3];   // two axes spanning the face
        if (N[0] != 0) { u[0] = 0; u[2] = 1; } else if (N[2] != 0) { u[0] = 1; }
        v3_cross(N, u, w);
        for (int k = 0; k < 4; k++) {
            float a = (k == 1 || k == 2) ? 0.5f : -0.5f, b = (k >= 2) ? 0.5f : -0.5f;
            Vtx *o = &v[f * 4 + k];
            for (int i = 0; i < 3; i++) { o->pos[i] = N[i] * 0.5f + u[i] * a + w[i] * b; o->nrm[i] = N[i]; }
            o->uv[0] = a + 0.5f; o->uv[1] = b + 0.5f;
        }
        static const uint16_t q[6] = {0, 1, 2, 0, 2, 3};
        for (int k = 0; k < 6; k++) idx[f * 6 + k] = (uint16_t)(f * 4 + q[k]);
    }
}

// texture i: a checkerboard of its colour at full and half strength; texel (0,0) is the half one
static void tex_colour(int i, uint8_t *rgb) { rgb[0] = (uint8_t)(i * 37 + 11); rgb[1] = (uint8_t)(i * 91 + 7); rgb[2] = (uint8_t)(i * 53 + 101); }
static void texel(int i, int x, int y, uint8_t *o) {
    uint8_t c[3];
    tex_colour(i, c);
    int full = ((x >> 3) ^ (y >> 3)) & 1;
    for (int k = 0; k < 3; k++) o[k] = full ? c[k] : c[k] / 2;
    o[3] = 255;
}

static int G = 16;   // grid side
static void scene_frame(int frame, Inst *out, Push *pc) {
    static const float axis0[3] = {0.3f, 1.0f, 0.2f};
    float ax[3] = {axis0[0], axis0[1], axis0[2]};
    v3_norm(ax);
    for (int i = 0; i < G; i++)
        for (int j = 0; j < G; j++) {
            int k = i * G + j;
            float x = (i - G / 2.0f + 0.5f) * 1.6f, z = (j - G / 2.0f + 0.5f) * 1.6f;
            float y = 0.4f * sinf(frame * 0.05f + i * 0.3f + j * 0.2f);
            M4 m = m4_model(x, y, z, ax, frame * 0.03f + k * 0.1f, 1.0f);
            memcpy(out[k].model, m.m, sizeof m.m);
            out[k].tint[0] = 0.6f + 0.4f * (float)((k * 7) % 5) / 4;
            out[k].tint[1] = 0.6f + 0.4f * (float)((k * 3) % 7) / 6;
            out[k].tint[2] = 0.6f + 0.4f * (float)((k * 5) % 3) / 2;
            out[k].tint[3] = 1;
            out[k].tex = (uint32_t)(k % NTEX);
        }
    float r = G * 1.2f, a = frame * 0.004f;
    float eye[3] = {r * cosf(a), G * 0.6f, r * sinf(a)}, at[3] = {0, 0, 0}, up[3] = {0, 1, 0};
    M4 vp = m4_mul(m4_persp(1.047f, (float)ext.width / ext.height, 0.1f, 200.0f), m4_lookat(eye, at, up));
    memset(pc, 0, sizeof *pc);
    memcpy(pc->viewproj, vp.m, sizeof vp.m);
    pc->light[0] = 0.4f; pc->light[1] = 1.0f; pc->light[2] = 0.3f;
}

static uint64_t fnv1a(const uint8_t *p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001b3ull; }
    return h;
}
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static double pct(const double *sorted, int n, double p) { return n ? sorted[(int)(p * (n - 1) + 0.5)] : 0; }

// ---- swapchain and the attachments that follow its size
static void make_swapchain(void) {
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    uint32_t nf = 64;
    VkSurfaceFormatKHR fmts[64];
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts));
    VkSurfaceFormatKHR f = fmts[0];
    for (uint32_t i = 0; i < nf; i++)
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) f = fmts[i];
    uint32_t np = 16;
    VkPresentModeKHR pms[16];
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &np, pms));
    int ok = 0;
    for (uint32_t i = 0; i < np; i++) ok |= pms[i] == mode;
    if (!ok) { printf("present mode %d not offered, using FIFO\n", mode); mode = VK_PRESENT_MODE_FIFO_KHR; }
    VkImageUsageFlags use = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if ((caps.supportedUsageFlags & use) != use) { printf("FAIL swapchain images cannot be colour attachments and transfer destinations\n"); exit(1); }
    ext = caps.currentExtent;
    if (ext.width == 0xffffffff) ext = (VkExtent2D){800, 600};
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainKHR old = sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, want, f.format, f.colorSpace, ext, 1,
        use, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, old};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
    if (old) {
        for (uint32_t i = 0; i < nimg; i++) vkDestroyImageView(dev, views[i], NULL);
        vkDestroySwapchainKHR(dev, old, NULL);
        free_image(&mscolor);
        free_image(&depth);
    }
    scfmt = f.format;
    nimg = 16;
    CHECK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
    for (uint32_t i = 0; i < nimg; i++) {
        VkImageViewCreateInfo vi = {VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, imgs[i], VK_IMAGE_VIEW_TYPE_2D, scfmt, {0},
            {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
        CHECK(vkCreateImageView(dev, &vi, NULL, &views[i]));
    }
    if (samples > 1) mscolor = make_image(scfmt, ext, samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    depth = make_image(depthfmt, ext, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
    printf("swapchain %ux%u format %d, %u images (min %u), present mode %d, MSAA %dx, depth format %d%s\n",
           ext.width, ext.height, scfmt, nimg, caps.minImageCount, mode, samples, depthfmt, old ? " (recreated)" : "");
}

// colour target (MSAA image resolved into `target`, or `target` itself), depth, cleared; the caller draws
static void begin_render(VkCommandBuffer cb, VkImageView target, VkImageView ms, VkImageView dview, VkExtent2D e, float r, float g, float b) {
    VkClearValue cc = {.color = {{r, g, b, 1.0f}}}, dc = {.depthStencil = {1.0f, 0}};
    VkRenderingAttachmentInfo ca = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, ms ? ms : target, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        ms ? VK_RESOLVE_MODE_AVERAGE_BIT : VK_RESOLVE_MODE_NONE, ms ? target : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        VK_ATTACHMENT_LOAD_OP_CLEAR, ms ? VK_ATTACHMENT_STORE_OP_DONT_CARE : VK_ATTACHMENT_STORE_OP_STORE, cc};
    VkRenderingAttachmentInfo da = {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, NULL, dview, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_RESOLVE_MODE_NONE, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED, VK_ATTACHMENT_LOAD_OP_CLEAR, VK_ATTACHMENT_STORE_OP_DONT_CARE, dc};
    VkRenderingInfo ri = {VK_STRUCTURE_TYPE_RENDERING_INFO, NULL, 0, {{0, 0}, e}, 1, 0, 1, &ca, &da, NULL};
    vkCmdBeginRendering(cb, &ri);
    VkViewport vp = {0, 0, (float)e.width, (float)e.height, 0, 1};
    VkRect2D sr = {{0, 0}, e};
    vkCmdSetViewport(cb, 0, 1, &vp);
    vkCmdSetScissor(cb, 0, 1, &sr);
}
// layouts of the colour/MSAA/depth images before a render pass that clears them all
static void pre_render(VkCommandBuffer cb, VkImage target, VkImage ms, VkImage dimg) {
    const VkPipelineStageFlags2 CAO = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, FT = VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
    barrier(cb, target, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            CAO | VK_PIPELINE_STAGE_2_TRANSFER_BIT, 0, CAO, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    if (ms) barrier(cb, ms, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    CAO, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, CAO, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_2_COLOR_ATTACHMENT_READ_BIT);
    barrier(cb, dimg, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
            FT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, FT, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
}

static VkShaderModule shader(const uint32_t *code, size_t size) {
    VkShaderModuleCreateInfo ci = {VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, size, code};
    VkShaderModule m;
    CHECK(vkCreateShaderModule(dev, &ci, NULL, &m));
    return m;
}
static VkPipeline pipeline(VkPipelineLayout pl, VkShaderModule vs, VkShaderModule fs, VkFormat colour, int vertices) {
    VkPipelineShaderStageCreateInfo st[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_VERTEX_BIT, vs, "main", NULL},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_FRAGMENT_BIT, fs, "main", NULL}};
    VkVertexInputBindingDescription bd = {0, sizeof(Vtx), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription ad[3] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, pos)},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vtx, nrm)}, {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vtx, uv)}};
    VkPipelineVertexInputStateCreateInfo vi = {VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO, NULL, 0,
        vertices ? 1 : 0, &bd, vertices ? 3 : 0, ad};
    VkPipelineInputAssemblyStateCreateInfo ia = {VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO, NULL, 0, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST, VK_FALSE};
    VkPipelineViewportStateCreateInfo vps = {VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO, NULL, 0, 1, NULL, 1, NULL};
    VkPipelineRasterizationStateCreateInfo rs = {VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO, NULL, 0, VK_FALSE, VK_FALSE,
        VK_POLYGON_MODE_FILL, VK_CULL_MODE_NONE, VK_FRONT_FACE_COUNTER_CLOCKWISE, VK_FALSE, 0, 0, 0, 1.0f};
    VkPipelineMultisampleStateCreateInfo ms = {VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO, NULL, 0, samples, VK_FALSE, 0, NULL, VK_FALSE, VK_FALSE};
    VkPipelineDepthStencilStateCreateInfo ds = {VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO, NULL, 0, VK_TRUE, VK_TRUE,
        VK_COMPARE_OP_LESS, VK_FALSE, VK_FALSE, {0}, {0}, 0, 1};
    VkPipelineColorBlendAttachmentState ba = {VK_FALSE, 0, 0, 0, 0, 0, 0, 0xf};
    VkPipelineColorBlendStateCreateInfo cb = {VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO, NULL, 0, VK_FALSE, 0, 1, &ba, {0}};
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy = {VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO, NULL, 0, 2, dyn};
    VkPipelineRenderingCreateInfo rci = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO, NULL, 0, 1, &colour, depthfmt, VK_FORMAT_UNDEFINED};
    VkGraphicsPipelineCreateInfo ci = {VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO, &rci, 0, 2, st, &vi, &ia, NULL, &vps, &rs, &ms, &ds, &cb, &dy,
        pl, VK_NULL_HANDLE, 0, VK_NULL_HANDLE, -1};
    VkPipeline p;
    CHECK(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ci, NULL, &p));
    return p;
}

// one-shot command buffers on the queue, waited with vkQueueWaitIdle
static VkCommandPool pool;
static VkCommandBuffer begin_once(void) {
    VkCommandBuffer cb;
    VkCommandBufferAllocateInfo ai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    CHECK(vkAllocateCommandBuffers(dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
    CHECK(vkBeginCommandBuffer(cb, &bi));
    return cb;
}
static void submit_once(VkCommandBuffer cb) {
    CHECK(vkEndCommandBuffer(cb));
    VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
    VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &ci, 0, NULL};
    CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
    CHECK(vkQueueWaitIdle(queue));
}

static int has(const VkExtensionProperties *p, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; i++) if (!strcmp(p[i].extensionName, name)) return 1;
    return 0;
}

int main(int argc, char **argv) {
    @autoreleasepool {
    const char *icd = "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib", *lib = NULL, *png = NULL;
    int frames = 300, W = 800, H = 600, F = 2, resize = 0, mtldev = 0, want_samples = 4, wbs = 1;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--mailbox")) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (!strcmp(argv[i], "--immediate")) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else if (!strcmp(argv[i], "--grid") && i + 1 < argc) G = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--samples") && i + 1 < argc) want_samples = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--inflight") && i + 1 < argc) F = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--resize")) resize = 1;
        else if (!strcmp(argv[i], "--mtl-device")) mtldev = 1;
        else if (!strcmp(argv[i], "--png") && i + 1 < argc) png = argv[++i];
        else if (!strcmp(argv[i], "--no-wbs")) wbs = 0;
        else { printf("usage: see the comment at the top of vkscene.m\n"); return 2; }
    }
    if (frames < 8) frames = 8;
    if (F < 1) F = 1;
    if (F > MAXF) F = MAXF;
    if (G < 1) G = 1;
    if (G > 64) G = 64;
    const int NI = G * G;

    void *h;
    if (lib) {
        h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("FAIL dlopen %s: %s\n", lib, dlerror()); return 1; }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        if (!gipa) { printf("FAIL %s has no vkGetInstanceProcAddr\n", lib); return 1; }
        const char *ver = (const char *)dlsym(h, "intelxe_vk_version");
        const char *(*anvp)(void) = (const char *(*)(void))dlsym(h, "intelxe_vk_anv_path");
        Dl_info di;
        printf("library %s (%s)\n", lib, dladdr((void *)gipa, &di) && di.dli_fname ? di.dli_fname : "?");
        if (ver) printf("provider: %s\n", ver);
        if (anvp) printf("provider loads the driver from %s\n", anvp() ?: "(failed)");
    } else {
        h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("FAIL dlopen %s: %s\n", icd, dlerror()); return 1; }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!gipa) { printf("FAIL %s has no vk_icdGetInstanceProcAddr\n", icd); return 1; }
        printf("ICD %s\n", icd);
    }

    // a window as GLFW makes one for Vulkan: a layer-hosting view with a CAMetalLayer (Wine: --mtl-device)
    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(80, 80, W, H)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    win.title = @"vkscene";
    NSView *view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    CAMetalLayer *layer = [CAMetalLayer layer];
    layer.contentsScale = 1.0;
    if (mtldev) layer.device = MTLCreateSystemDefaultDevice();
    view.layer = layer;
    view.wantsLayer = YES;
    win.contentView = view;
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp finishLaunching];
    pump();
    printf("layer: CAMetalLayer, device %s\n", layer.device ? layer.device.name.UTF8String : "(none, as GLFW)");

    // ---- instance
    PFN_vkEnumerateInstanceExtensionProperties eie = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    uint32_t n = 0;
    CHECK(eie(NULL, &n, NULL));
    VkExtensionProperties *ie = calloc(n, sizeof *ie);
    CHECK(eie(NULL, &n, ie));
    if (!has(ie, n, VK_EXT_METAL_SURFACE_EXTENSION_NAME)) { printf("FAIL VK_EXT_metal_surface is not offered\n"); return 1; }
    const char *iext[4] = {VK_KHR_SURFACE_EXTENSION_NAME, VK_EXT_METAL_SURFACE_EXTENSION_NAME};
    uint32_t nie = 2;
    VkInstanceCreateFlags iflags = 0;
    // as a MoltenVK-era program does it: through a loader, this also lists portability drivers (MoltenVK for an A/B)
    if (has(ie, n, VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)) {
        iext[nie++] = VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME;
        iflags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
    }
    int dbgu = has(ie, n, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (dbgu) iext[nie++] = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkscene", 1, "vkscene", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, iflags, &ai, 0, NULL, nie, iext};
    CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
#define ILOAD(f) f = (PFN_##f)gipa(inst, #f); if (!f) { printf("FAIL no %s\n", #f); return 1; }
    INSTFNS(ILOAD)
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
    if (dbgu) {
        PFN_vkCreateDebugUtilsMessengerEXT cm = (PFN_vkCreateDebugUtilsMessengerEXT)gipa(inst, "vkCreateDebugUtilsMessengerEXT");
        VkDebugUtilsMessengerCreateInfoEXT mci = {VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT, NULL, 0,
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
            VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
            on_message, NULL};
        if (!cm || cm(inst, &mci, NULL, &messenger) != VK_SUCCESS) messenger = VK_NULL_HANDLE;
    }
    printf("driver messages: %s\n", messenger ? "VK_EXT_debug_utils messenger (warnings, errors)" : "none (no VK_EXT_debug_utils)");
    VkPhysicalDevice pds[8];
    n = 8;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, pds));
    if (!n) { printf("FAIL no physical device\n"); return 1; }
    pd = pds[0];
    for (uint32_t i = 0; i < n; i++) {   // a discrete GPU first, as games choose
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(pds[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { pd = pds[i]; break; }
    }
    vkGetPhysicalDeviceProperties(pd, &props);
    vkGetPhysicalDeviceMemoryProperties(pd, &memprops);
    {
        char fb[96];
        printf("memory types:");
        for (uint32_t i = 0; i < memprops.memoryTypeCount; i++)
            printf(" %u:%sheap%u(%llu MB)", i, memflags(memprops.memoryTypes[i].propertyFlags, fb, sizeof fb), memprops.memoryTypes[i].heapIndex,
                   (unsigned long long)(memprops.memoryHeaps[memprops.memoryTypes[i].heapIndex].size >> 20));
        printf("\n");
    }
    printf("device: %s, Vulkan %u.%u.%u, driver 0x%x (%u devices)\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion), props.driverVersion, n);
    if (props.apiVersion < VK_API_VERSION_1_3) { printf("FAIL the device offers Vulkan %u.%u, vkscene needs 1.3\n", VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion)); return 1; }

    VkMetalSurfaceCreateInfoEXT msci = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, layer};
    CHECK(vkCreateMetalSurfaceEXT(inst, &msci, NULL, &surf));

    // ---- features: what a bindless, timeline-paced, dynamic-rendering renderer needs
    VkPhysicalDeviceVulkan13Features f13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceVulkan12Features f12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13};
    VkPhysicalDeviceFeatures2 f2 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f12};
    vkGetPhysicalDeviceFeatures2(pd, &f2);
    struct { const char *name; VkBool32 have; } need[] = {
        {"timelineSemaphore", f12.timelineSemaphore}, {"runtimeDescriptorArray", f12.runtimeDescriptorArray},
        {"shaderSampledImageArrayNonUniformIndexing", f12.shaderSampledImageArrayNonUniformIndexing},
        {"descriptorBindingPartiallyBound", f12.descriptorBindingPartiallyBound},
        {"descriptorBindingVariableDescriptorCount", f12.descriptorBindingVariableDescriptorCount},
        {"descriptorBindingSampledImageUpdateAfterBind", f12.descriptorBindingSampledImageUpdateAfterBind},
        {"dynamicRendering", f13.dynamicRendering}, {"synchronization2", f13.synchronization2}};
    int missing = 0;
    for (size_t i = 0; i < sizeof need / sizeof need[0]; i++) if (!need[i].have) { printf("FAIL feature %s not supported\n", need[i].name); missing++; }
    if (missing) return 1;
    VkPhysicalDeviceVulkan13Features e13 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, NULL};
    e13.dynamicRendering = e13.synchronization2 = VK_TRUE;
    VkPhysicalDeviceVulkan12Features e12 = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &e13};
    e12.timelineSemaphore = e12.runtimeDescriptorArray = e12.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
    e12.descriptorBindingPartiallyBound = e12.descriptorBindingVariableDescriptorCount = VK_TRUE;
    e12.descriptorBindingSampledImageUpdateAfterBind = e12.descriptorIndexing = VK_TRUE;
    if (!f12.descriptorIndexing) e12.descriptorIndexing = VK_FALSE;

    // MSAA and depth format
    VkSampleCountFlags sc_ok = props.limits.framebufferColorSampleCounts & props.limits.framebufferDepthSampleCounts;
    samples = VK_SAMPLE_COUNT_1_BIT;
    for (int s = want_samples; s > 1; s >>= 1) if (sc_ok & s) { samples = (VkSampleCountFlagBits)s; break; }
    if (samples != want_samples) printf("MSAA %dx not offered for colour and depth: %dx\n", want_samples, samples);
    VkFormat dcand[3] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT, VK_FORMAT_D16_UNORM};
    depthfmt = VK_FORMAT_UNDEFINED;
    for (int i = 0; i < 3 && !depthfmt; i++) {
        VkFormatProperties fp;
        vkGetPhysicalDeviceFormatProperties(pd, dcand[i], &fp);
        if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) depthfmt = dcand[i];
    }
    if (!depthfmt) { printf("FAIL no depth format\n"); return 1; }

    // queue and device
    uint32_t nq = 16;
    VkQueueFamilyProperties qp[16];
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++) {
        VkBool32 sup = 0;
        CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup));
        if (sup && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam = i;
    }
    if (qfam == UINT32_MAX) { printf("FAIL no graphics queue family can present to the surface\n"); return 1; }
    int timestamps = qp[qfam].timestampValidBits > 0;
    n = 0;
    CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &n, NULL));
    VkExtensionProperties *de = calloc(n, sizeof *de);
    CHECK(vkEnumerateDeviceExtensionProperties(pd, NULL, &n, de));
    const char *dext[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t nde = 1;
    if (has(de, n, "VK_KHR_portability_subset")) dext[nde++] = "VK_KHR_portability_subset";   // required when offered (MoltenVK)
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &e12, 0, 1, &qci, 0, NULL, nde, dext, NULL};
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
#define DLOAD(f) f = (PFN_##f)gdpa(dev, #f); if (!f) { printf("FAIL no %s\n", #f); return 1; }
    DEVFNS(DLOAD)
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    make_swapchain();

    // ---- timeline semaphores: one paces the frames; two more for a GPU wait on a host signal
    VkSemaphoreTypeCreateInfo tci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo tsci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &tci};
    VkSemaphore tl, hostsig, gpudone;
    CHECK(vkCreateSemaphore(dev, &tsci, NULL, &tl));
    CHECK(vkCreateSemaphore(dev, &tsci, NULL, &hostsig));
    CHECK(vkCreateSemaphore(dev, &tsci, NULL, &gpudone));
    if (!wbs) printf("timeline: wait-before-signal skipped (--no-wbs): the queue keeps submitting directly\n");
    else {
        wbs_ran = 1;
        Buf b = make_buffer("timeline test buffer", 256, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 1);
        memset(b.map, 0, 256);
        VkCommandBuffer cb = begin_once();
        vkCmdFillBuffer(cb, b.buf, 0, 256, 0x7e57c0de);
        CHECK(vkEndCommandBuffer(cb));
        VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, hostsig, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        VkSemaphoreSubmitInfo s = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, gpudone, 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0};
        VkCommandBufferSubmitInfo ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &w, 1, &ci, 1, &s};
        CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));   // waits for a value nobody has signalled yet
        usleep(20000);
        uint64_t v = 99;
        CHECK(vkGetSemaphoreCounterValue(dev, gpudone, &v));
        uint32_t early = ((uint32_t *)b.map)[0];
        double t = now_ms();
        VkSemaphoreSignalInfo sig = {VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO, NULL, hostsig, 1};
        CHECK(vkSignalSemaphore(dev, &sig));
        uint64_t one = 1;
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &gpudone, &one};
        VkResult r = vkWaitSemaphores(dev, &wi, 2000000000ull);
        double dt = now_ms() - t;
        uint32_t late = ((uint32_t *)b.map)[0];
        int ok = v == 0 && early == 0 && r == VK_SUCCESS && late == 0x7e57c0de;
        printf("timeline: wait-before-signal %s (before the host signal: counter %llu, buffer 0x%x; after: %s in %.2f ms, buffer 0x%x)\n",
               ok ? "OK" : "FAILED", (unsigned long long)v, early, r == VK_SUCCESS ? "released" : r == VK_TIMEOUT ? "TIMEOUT" : "error", dt, late);
        printf("timeline: from here on the queue may submit from a thread (Mesa's does: a wait on an unsignalled point started it)\n");
        if (!ok) fails++;
        CHECK(vkQueueWaitIdle(queue));
        free_buffer(&b);
    }

    // ---- resources: cube mesh, textures, sampler, descriptor sets, pipelines
    Vtx cv[24];
    uint16_t ci16[36];
    cube(cv, ci16);
    const VkDeviceSize texbytes = TEXSZ * TEXSZ * 4;
    Buf stage = make_buffer("staging buffer", sizeof cv + sizeof ci16 + NTEX * texbytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, 1);
    memcpy(stage.map, cv, sizeof cv);
    memcpy((char *)stage.map + sizeof cv, ci16, sizeof ci16);
    uint8_t *tp = (uint8_t *)stage.map + sizeof cv + sizeof ci16;
    for (int i = 0; i < NTEX; i++)
        for (int y = 0; y < TEXSZ; y++)
            for (int x = 0; x < TEXSZ; x++) texel(i, x, y, tp + i * texbytes + (y * TEXSZ + x) * 4);
    Buf vbuf = make_buffer("vertex buffer", sizeof cv, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    Buf ibuf = make_buffer("index buffer", sizeof ci16, VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, 0);
    Img tex[NTEX];
    for (int i = 0; i < NTEX; i++)
        tex[i] = make_image(VK_FORMAT_R8G8B8A8_UNORM, (VkExtent2D){TEXSZ, TEXSZ}, VK_SAMPLE_COUNT_1_BIT,
                            VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
    {
        VkCommandBuffer cb = begin_once();
        VkBufferCopy c1 = {0, 0, sizeof cv}, c2 = {sizeof cv, 0, sizeof ci16};
        vkCmdCopyBuffer(cb, stage.buf, vbuf.buf, 1, &c1);
        vkCmdCopyBuffer(cb, stage.buf, ibuf.buf, 1, &c2);
        for (int i = 0; i < NTEX; i++) {
            barrier(cb, tex[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_NONE, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy bic = {sizeof cv + sizeof ci16 + i * texbytes, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {TEXSZ, TEXSZ, 1}};
            vkCmdCopyBufferToImage(cb, stage.buf, tex[i].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
            barrier(cb, tex[i].img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        }
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_VERTEX_INPUT_BIT,
                    VK_ACCESS_2_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_2_INDEX_READ_BIT);
        submit_once(cb);
    }
    free_buffer(&stage);

    VkSampler smp;
    VkSamplerCreateInfo sci = {VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_LINEAR, VK_FILTER_LINEAR, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, VK_SAMPLER_ADDRESS_MODE_REPEAT, 0, VK_FALSE, 1, VK_FALSE,
        VK_COMPARE_OP_ALWAYS, 0, 0, VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK, VK_FALSE};
    CHECK(vkCreateSampler(dev, &sci, NULL, &smp));
    uint32_t maxtex = MAXTEX;
    if (props.limits.maxPerStageDescriptorSampledImages < maxtex) maxtex = props.limits.maxPerStageDescriptorSampledImages;
    // set 0: the immutable sampler + the bindless array (update after bind); set 1: instance data, dynamic offset
    VkDescriptorSetLayoutBinding b0[2] = {
        {0, VK_DESCRIPTOR_TYPE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, &smp},
        {1, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, maxtex, VK_SHADER_STAGE_FRAGMENT_BIT, NULL}};
    VkDescriptorBindingFlags bf[2] = {0, VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT |
        VK_DESCRIPTOR_BINDING_VARIABLE_DESCRIPTOR_COUNT_BIT};
    VkDescriptorSetLayoutBindingFlagsCreateInfo bfi = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 2, bf};
    VkDescriptorSetLayoutCreateInfo l0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, &bfi,
        VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 2, b0};
    VkDescriptorSetLayoutBinding b1 = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_VERTEX_BIT, NULL};
    VkDescriptorSetLayoutCreateInfo l1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 1, &b1};
    VkDescriptorSetLayout dsl[2];
    CHECK(vkCreateDescriptorSetLayout(dev, &l0, NULL, &dsl[0]));
    CHECK(vkCreateDescriptorSetLayout(dev, &l1, NULL, &dsl[1]));
    VkDescriptorPoolSize ps0[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, 1}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, NTEX}};
    VkDescriptorPoolCreateInfo dp0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, 1, 2, ps0};
    VkDescriptorPoolSize ps1 = {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, 1};
    VkDescriptorPoolCreateInfo dp1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 1, &ps1};
    VkDescriptorPool dpool[2];
    CHECK(vkCreateDescriptorPool(dev, &dp0, NULL, &dpool[0]));
    CHECK(vkCreateDescriptorPool(dev, &dp1, NULL, &dpool[1]));
    uint32_t varcount = NTEX;
    VkDescriptorSetVariableDescriptorCountAllocateInfo vca = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_VARIABLE_DESCRIPTOR_COUNT_ALLOCATE_INFO, NULL, 1, &varcount};
    VkDescriptorSetAllocateInfo da0 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, &vca, dpool[0], 1, &dsl[0]};
    VkDescriptorSetAllocateInfo da1 = {VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dpool[1], 1, &dsl[1]};
    VkDescriptorSet sets[2];
    CHECK(vkAllocateDescriptorSets(dev, &da0, &sets[0]));
    CHECK(vkAllocateDescriptorSets(dev, &da1, &sets[1]));
    VkDescriptorImageInfo dii[NTEX];
    for (int i = 0; i < NTEX; i++) dii[i] = (VkDescriptorImageInfo){VK_NULL_HANDLE, tex[i].view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wlow = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 1, 0, NTEX / 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, dii, NULL, NULL};
    VkWriteDescriptorSet whigh = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[0], 1, NTEX / 2, NTEX / 2, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, dii + NTEX / 2, NULL, NULL};
    vkUpdateDescriptorSets(dev, 1, &wlow, 0, NULL);   // the upper half after the check pass is recorded

    VkDeviceSize align = props.limits.minStorageBufferOffsetAlignment, slice = (NI * sizeof(Inst) + align - 1) / align * align;
    Buf ibufs = make_buffer("instance buffer", slice * F, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, 1);
    VkDescriptorBufferInfo dbi = {ibufs.buf, 0, NI * sizeof(Inst)};
    VkWriteDescriptorSet wb = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, sets[1], 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER_DYNAMIC, NULL, &dbi, NULL};
    vkUpdateDescriptorSets(dev, 1, &wb, 0, NULL);

    VkPushConstantRange pcr = {VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci = {VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 2, dsl, 1, &pcr};
    VkPipelineLayout pl;
    CHECK(vkCreatePipelineLayout(dev, &plci, NULL, &pl));
    VkShaderModule svs = shader(scene_vert_spv, sizeof scene_vert_spv), sfs = shader(scene_frag_spv, sizeof scene_frag_spv);
    VkShaderModule cvs = shader(check_vert_spv, sizeof check_vert_spv), cfs = shader(check_frag_spv, sizeof check_frag_spv);
    double tp0 = now_ms();
    VkPipeline scene_p = pipeline(pl, svs, sfs, scfmt, 1);
    VkPipeline check_p = pipeline(pl, cvs, cfs, VK_FORMAT_R8G8B8A8_UNORM, 0);
    printf("pipelines: 2 compiled in %.1f ms; %d textures bindless (variable count %u of max %u), %d cubes, %d frames in flight%s\n",
           now_ms() - tp0, NTEX, varcount, maxtex, NI, F, timestamps ? ", GPU timestamps" : ", no timestamps on this queue");

    // ---- 1. the check pass
    {
        VkExtent2D ce = {CHK, CHK};
        Img cres = make_image(VK_FORMAT_R8G8B8A8_UNORM, ce, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Img cms = {0};
        if (samples > 1) cms = make_image(VK_FORMAT_R8G8B8A8_UNORM, ce, samples, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
        Img cdep = make_image(depthfmt, ce, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, VK_IMAGE_ASPECT_DEPTH_BIT);
        Buf rb = make_buffer("check read-back buffer", CHK * CHK * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2);
        VkCommandBuffer cb = begin_once();
        pre_render(cb, cres.img, cms.img, cdep.img);
        begin_render(cb, cres.view, cms.view, cdep.view, ce, 0, 0, 0);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, check_p);
        uint32_t off = 0;
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 1, &off);
        Push p = {{0}};
        p.mode = 0; p.depth = 0.25f;
        vkCmdPushConstants(cb, pl, pcr.stageFlags, 0, sizeof p, &p);
        vkCmdDraw(cb, 6, NTEX, 0, 0);
        p.mode = 1; p.depth = 0.75f;   // behind: must not show
        vkCmdPushConstants(cb, pl, pcr.stageFlags, 0, sizeof p, &p);
        vkCmdDraw(cb, 6, NTEX, 0, 0);
        p.mode = 2; p.depth = 0.1f;    // in front: must show, with a blended edge
        vkCmdPushConstants(cb, pl, pcr.stageFlags, 0, sizeof p, &p);
        vkCmdDraw(cb, 3, 1, 0, 0);
        vkCmdEndRendering(cb);
        barrier(cb, cres.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
        VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {CHK, CHK, 1}};
        vkCmdCopyImageToBuffer(cb, cres.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bic);
        mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
        vkUpdateDescriptorSets(dev, 1, &whigh, 0, NULL);   // after recording, before submitting: update after bind
        submit_once(cb);
        const uint8_t *px = rb.map;
        int bad_cells = 0, first_bad = -1;
        for (int c = 0; c < NTEX; c++) {
            int x = (c % 8) * 32 + 16, y = (c / 8) * 32 + 16;
            const uint8_t *q = px + (y * CHK + x) * 4;
            uint8_t e[4];
            texel(c, 0, 0, e);
            if (abs(q[0] - e[0]) > 1 || abs(q[1] - e[1]) > 1 || abs(q[2] - e[2]) > 1) { if (first_bad < 0) first_bad = c; bad_cells++; }
        }
        const uint8_t *in = px + (4 * CHK + 4) * 4;
        int depth_ok = in[0] == 255 && in[1] == 255 && in[2] == 255;
        uint8_t c0[4];
        texel(0, 0, 0, c0);
        int blended = 0, edge = 0;
        for (int x = 0; x <= 30; x++) {   // pixels whose square the edge x + y = 31.3 crosses
            int y = 30 - x;
            const uint8_t *q = px + (y * CHK + x) * 4;
            edge++;
            if (q[0] > c0[0] + 8 && q[0] < 247) blended++;
        }
        printf("check pass %dx%d, MSAA %dx: cells %d/%d right (texture[cell] through nonuniformEXT; 32-63 written after recording)%s",
               CHK, CHK, samples, NTEX - bad_cells, NTEX, bad_cells ? "" : "\n");
        if (bad_cells) {
            const uint8_t *q = px + (((first_bad / 8) * 32 + 16) * CHK + (first_bad % 8) * 32 + 16) * 4;
            uint8_t e[4];
            texel(first_bad, 0, 0, e);
            printf(": cell %d has %d %d %d, expected %d %d %d\n", first_bad, q[0], q[1], q[2], e[0], e[1], e[2]);
        }
        printf("check pass: depth test %s (near triangle over the cells, far magenta hidden: %s), MSAA edge %d/%d pixels blended%s\n",
               depth_ok ? "right" : "WRONG", bad_cells ? "see cells" : "yes", blended, edge, samples > 1 ? "" : " (no MSAA, not checked)");
        if (bad_cells || !depth_ok) fails++;
        if (samples > 1 && blended < edge / 2) { printf("FAIL the MSAA resolve did not blend the edge\n"); fails++; }
        free_buffer(&rb);
        free_image(&cres);
        free_image(&cms);
        free_image(&cdep);
    }

    // ---- 2. the frames
    VkCommandBuffer cbs[MAXF];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, (uint32_t)F};
    CHECK(vkAllocateCommandBuffers(dev, &cai, cbs));
    VkSemaphore acq[MAXF];
    VkSemaphoreCreateInfo bsci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    for (int i = 0; i < F; i++) CHECK(vkCreateSemaphore(dev, &bsci, NULL, &acq[i]));
    for (int i = 0; i < 16; i++) CHECK(vkCreateSemaphore(dev, &bsci, NULL, &done[i]));
    VkQueryPool qpool = VK_NULL_HANDLE;
    if (timestamps) {
        VkQueryPoolCreateInfo qpi = {VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * MAXF, 0};
        CHECK(vkCreateQueryPool(dev, &qpi, NULL, &qpool));
    }
    Img fin = {0};
    Buf rb = {0};
    double *iv = calloc(frames + 1, sizeof(double)), *gpu = calloc(frames + 1, sizeof(double)), *tlw = calloc(frames + 1, sizeof(double));
    double acq_max = 0, prev = 0, start = now_ms();
    int presented = 0, ngpu = 0, recreated = 0, resized = 0, niv = 0;
    uint64_t submitted = 0;
    for (int fr = 0; fr <= frames; fr++) {
        const int last = fr == frames;   // the read-back frame
        pump();
        if (resize && !resized && fr == frames / 2) { [win setContentSize:NSMakeSize(W * 3 / 4, H * 3 / 4)]; pump(); resized = 1; }
        int s = fr % F;
        // the slot's previous frame is done when the timeline reaches its value: no fences
        if (submitted >= (uint64_t)F) {
            uint64_t v = submitted - F + 1;
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &v};
            double t = now_ms();
            CHECK(vkWaitSemaphores(dev, &wi, 5000000000ull));
            tlw[fr] = now_ms() - t;
            if (timestamps) {
                uint64_t ts[2];
                if (vkGetQueryPoolResults(dev, qpool, s * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
                    gpu[ngpu++] = (double)(ts[1] - ts[0]) * props.limits.timestampPeriod / 1e6;
            }
        }
        uint32_t idx;
        double a = now_ms();
        VkResult r = vkAcquireNextImageKHR(dev, sc, 5000000000ull, acq[s], VK_NULL_HANDLE, &idx);
        if (now_ms() - a > acq_max) acq_max = now_ms() - a;
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { CHECK(vkDeviceWaitIdle(dev)); make_swapchain(); recreated++; fr--; continue; }
        if (r != VK_SUBOPTIMAL_KHR) CHECK(r);
        Inst *ins = (Inst *)((char *)ibufs.map + slice * s);
        Push pc;
        scene_frame(fr, ins, &pc);
        VkCommandBuffer cb = cbs[s];
        CHECK(vkResetCommandBuffer(cb, 0));
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkBeginCommandBuffer(cb, &bi));
        if (timestamps) {
            vkCmdResetQueryPool(cb, qpool, s * 2, 2);
            vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, qpool, s * 2);
        }
        VkImage target = imgs[idx];
        VkImageView tview = views[idx];
        if (last) {
            fin = make_image(scfmt, ext, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, VK_IMAGE_ASPECT_COLOR_BIT);
            rb = make_buffer("frame read-back buffer", (VkDeviceSize)ext.width * ext.height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, 2);
            target = fin.img;
            tview = fin.view;
        }
        pre_render(cb, target, mscolor.img, depth.img);
        begin_render(cb, tview, samples > 1 ? mscolor.view : VK_NULL_HANDLE, depth.view, ext, 0.10f, 0.12f, 0.16f);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, scene_p);
        uint32_t off = (uint32_t)(slice * s);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, pl, 0, 2, sets, 1, &off);
        VkDeviceSize vo = 0;
        vkCmdBindVertexBuffers(cb, 0, 1, &vbuf.buf, &vo);
        vkCmdBindIndexBuffer(cb, ibuf.buf, 0, VK_INDEX_TYPE_UINT16);
        vkCmdPushConstants(cb, pl, pcr.stageFlags, 0, sizeof pc, &pc);
        vkCmdDrawIndexed(cb, 36, (uint32_t)NI, 0, 0, 0);
        vkCmdEndRendering(cb);
        if (last) {
            // read back what was rendered and show the same pixels
            barrier(cb, fin.img, VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT);
            barrier(cb, imgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
            VkBufferImageCopy bic = {0, 0, 0, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {ext.width, ext.height, 1}};
            vkCmdCopyImageToBuffer(cb, fin.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, rb.buf, 1, &bic);
            VkImageCopy icp = {{VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, {0, 0, 0}, {ext.width, ext.height, 1}};
            vkCmdCopyImage(cb, fin.img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &icp);
            mem_barrier(cb, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_HOST_BIT, VK_ACCESS_2_HOST_READ_BIT);
            barrier(cb, imgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        } else {
            barrier(cb, imgs[idx], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_NONE, 0);
        }
        if (timestamps) vkCmdWriteTimestamp2(cb, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, qpool, s * 2 + 1);
        CHECK(vkEndCommandBuffer(cb));
        VkSemaphoreSubmitInfo w = {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, acq[s], 0,
            last ? VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT : VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, 0};
        VkSemaphoreSubmitInfo sg[2] = {{VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, done[idx], 0, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0},
                                       {VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, tl, submitted + 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, 0}};
        VkCommandBufferSubmitInfo cbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0};
        VkSubmitInfo2 si = {VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 1, &w, 1, &cbi, 2, sg};
        CHECK(vkQueueSubmit2(queue, 1, &si, VK_NULL_HANDLE));
        submitted++;
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done[idx], 1, &sc, &idx, NULL};
        r = vkQueuePresentKHR(queue, &pi);
        double q = now_ms();
        if (prev && !last) iv[niv++] = q - prev;
        prev = q;
        presented++;
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
            if (last) { printf("the last frame's present said %s: the shown-image check may be skipped\n", r == VK_SUBOPTIMAL_KHR ? "SUBOPTIMAL" : "OUT_OF_DATE"); }
            else { CHECK(vkDeviceWaitIdle(dev)); make_swapchain(); recreated++; }
        } else if (r < 0) {
            printf("FAIL vkQueuePresentKHR = %d at frame %d of %d (image %u, %s)%s\n", r, fr, frames, idx,
                   wbs_ran ? "the queue has a submit thread" : "no submit thread",
                   r == VK_ERROR_UNKNOWN && wbs_ran ? ": a present without a blit copies the semaphore payload "
                   "(WAIT_PENDING, then SYNCOBJ_TRANSFER); a kernel that answers WAIT_PENDING before the signal was "
                   "submitted fails it. Compare --no-wbs" : "");
            exit(1);
        }
    }
    CHECK(vkDeviceWaitIdle(dev));
    double total = now_ms() - start;
    uint64_t tlv = 0;
    CHECK(vkGetSemaphoreCounterValue(dev, tl, &tlv));
    if (tlv != submitted) { printf("FAIL the timeline semaphore is at %llu after %llu submits\n", (unsigned long long)tlv, (unsigned long long)submitted); fails++; }

    // ---- 3. the read-back frame and what the window shows
    const uint8_t *px = rb.map;
    size_t npx = (size_t)ext.width * ext.height;
    uint64_t sum = fnv1a(px, npx * 4);
    uint8_t *seen = calloc(1 << 21, 1);
    int distinct = 0;
    size_t clear_px = 0;
    uint8_t cl[4] = {0};
    {   // the clear colour as the driver wrote it into this format
        float c[3] = {0.10f, 0.12f, 0.16f};
        int bgra = scfmt == VK_FORMAT_B8G8R8A8_UNORM;
        for (int k = 0; k < 3; k++) cl[bgra ? 2 - k : k] = (uint8_t)(c[k] * 255 + 0.5f);
    }
    for (size_t i = 0; i < npx; i++) {
        const uint8_t *q = px + i * 4;
        uint32_t c = q[0] | q[1] << 8 | q[2] << 16;
        if (!(seen[c >> 3] & (1 << (c & 7)))) { seen[c >> 3] |= 1 << (c & 7); distinct++; }
        if (abs(q[0] - cl[0]) <= 1 && abs(q[1] - cl[1]) <= 1 && abs(q[2] - cl[2]) <= 1) clear_px++;
    }
    free(seen);
    double cover = 100.0 * (npx - clear_px) / npx;
    printf("scene checksum 0x%016llx (%ux%u format %d, frame %d): %.1f%% covered by cubes, %d colours\n",
           (unsigned long long)sum, ext.width, ext.height, scfmt, frames, cover, distinct);
    if (cover < 5 || cover > 99.5 || distinct < 64) { printf("FAIL the scene does not look drawn (coverage %.1f%%, %d colours)\n", cover, distinct); fails++; }
    if (png) {
        CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
        CGBitmapInfo bi = scfmt == VK_FORMAT_B8G8R8A8_UNORM ? kCGBitmapByteOrder32Little | kCGImageAlphaNoneSkipFirst
                                                            : kCGBitmapByteOrderDefault | kCGImageAlphaNoneSkipLast;
        CGContextRef ctx = CGBitmapContextCreate((void *)px, ext.width, ext.height, 8, ext.width * 4, cs, bi);
        CGImageRef im = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)png, strlen(png), false);
        CGImageDestinationRef d = im ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
        if (d) { CGImageDestinationAddImage(d, im, NULL); printf("png: %s %s\n", png, CGImageDestinationFinalize(d) ? "written" : "NOT written"); CFRelease(d); }
        else printf("png: %s NOT written\n", png);
        if (im) CGImageRelease(im);
        if (ctx) CGContextRelease(ctx);
        CFRelease(url);
        CGColorSpaceRelease(cs);
    }
    usleep(150000);
    pump();
    int shown = -1;
    for (CALayer *sub in layer.sublayers) {
        if (![sub.name isEqualToString:@"org.freedesktop.mesa.wsi.iosurface"]) continue;
        id c = sub.contents;
        if (!c || CFGetTypeID((__bridge CFTypeRef)c) != IOSurfaceGetTypeID()) { printf("IOSurface layer has no IOSurface contents\n"); break; }
        IOSurfaceRef s = (__bridge IOSurfaceRef)c;
        size_t w = IOSurfaceGetWidth(s), hh = IOSurfaceGetHeight(s);
        if (w != ext.width || hh != ext.height || IOSurfaceGetPixelFormat(s) != 'BGRA' || scfmt != VK_FORMAT_B8G8R8A8_UNORM) {
            printf("shown IOSurface %zux%zu pixel format 0x%08x: not compared\n", w, hh, IOSurfaceGetPixelFormat(s));
            break;
        }
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
        size_t bpr = IOSurfaceGetBytesPerRow(s), bad = 0;
        const uint8_t *base = IOSurfaceGetBaseAddress(s);
        for (size_t y = 0; y < hh; y++) {
            const uint8_t *a = base + y * bpr, *b = px + y * w * 4;
            for (size_t x = 0; x < w; x++) bad += a[x * 4] != b[x * 4] || a[x * 4 + 1] != b[x * 4 + 1] || a[x * 4 + 2] != b[x * 4 + 2];
        }
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        shown = bad == 0;
        printf("shown IOSurface %zux%zu: %zu of %zu pixels differ from the read-back: %s\n", w, hh, bad, w * hh, shown ? "match" : "MISMATCH");
        break;
    }
    if (shown < 0) printf("no IOSurface sublayer read (another WSI or driver): shown-image check skipped\n");
    if (shown == 0) fails++;

    // ---- timings
    qsort(iv, niv, sizeof *iv, cmpd);
    qsort(gpu, ngpu, sizeof *gpu, cmpd);
    double isum = 0, gsum = 0, wsum = 0;
    for (int i = 0; i < niv; i++) isum += iv[i];
    for (int i = 0; i < ngpu; i++) gsum += gpu[i];
    for (int i = 0; i <= frames; i++) wsum += tlw[i];
    double iavg = niv ? isum / niv : 0;
    printf("frames %d presented %d in %.0f ms: %.1f fps; interval avg %.2f p50 %.2f p95 %.2f p99 %.2f max %.2f ms\n",
           frames, presented, total, iavg > 0 ? 1000.0 / iavg : 0, iavg, pct(iv, niv, .5), pct(iv, niv, .95), pct(iv, niv, .99), niv ? iv[niv - 1] : 0);
    if (ngpu) printf("GPU per frame: avg %.3f p50 %.3f p99 %.3f max %.3f ms (%d frames, timestamps)\n", gsum / ngpu, pct(gpu, ngpu, .5), pct(gpu, ngpu, .99), gpu[ngpu - 1], ngpu);
    printf("acquire max %.3f ms; timeline wait avg %.3f ms; swapchains recreated %d%s\n", acq_max, wsum / (frames + 1), recreated, resized ? " (window resized halfway)" : "");
    if (resized && !recreated) { printf("FAIL the resize did not make the swapchain SUBOPTIMAL or OUT_OF_DATE\n"); fails++; }
    if (mode == VK_PRESENT_MODE_FIFO_KHR && iavg < 2.0) { printf("FAIL FIFO frames came faster than any display refresh (%.2f ms)\n", iavg); fails++; }
    printf("RESULT vkscene frames=%d fps=%.1f p50_ms=%.2f p99_ms=%.2f gpu_ms=%.3f mode=%d msaa=%d size=%ux%u wbs=%d memfallback=%d checksum=0x%016llx check=%s\n",
           frames, iavg > 0 ? 1000.0 / iavg : 0, pct(iv, niv, .5), pct(iv, niv, .99), ngpu ? gsum / ngpu : 0, mode, samples,
           ext.width, ext.height, wbs_ran, memfallbacks, (unsigned long long)sum, fails ? "FAIL" : "PASS");

    // ---- teardown
    if (qpool) vkDestroyQueryPool(dev, qpool, NULL);
    free_image(&fin);
    free_buffer(&rb);
    free_buffer(&ibufs);
    free_buffer(&vbuf);
    free_buffer(&ibuf);
    for (int i = 0; i < NTEX; i++) free_image(&tex[i]);
    vkDestroyPipeline(dev, scene_p, NULL);
    vkDestroyPipeline(dev, check_p, NULL);
    vkDestroyShaderModule(dev, svs, NULL); vkDestroyShaderModule(dev, sfs, NULL);
    vkDestroyShaderModule(dev, cvs, NULL); vkDestroyShaderModule(dev, cfs, NULL);
    vkDestroyPipelineLayout(dev, pl, NULL);
    vkDestroyDescriptorPool(dev, dpool[0], NULL); vkDestroyDescriptorPool(dev, dpool[1], NULL);
    vkDestroyDescriptorSetLayout(dev, dsl[0], NULL); vkDestroyDescriptorSetLayout(dev, dsl[1], NULL);
    vkDestroySampler(dev, smp, NULL);
    for (uint32_t i = 0; i < nimg; i++) vkDestroyImageView(dev, views[i], NULL);
    free_image(&mscolor);
    free_image(&depth);
    vkDestroySwapchainKHR(dev, sc, NULL);
    for (int i = 0; i < 16; i++) vkDestroySemaphore(dev, done[i], NULL);
    for (int i = 0; i < F; i++) vkDestroySemaphore(dev, acq[i], NULL);
    vkDestroySemaphore(dev, tl, NULL); vkDestroySemaphore(dev, hostsig, NULL); vkDestroySemaphore(dev, gpudone, NULL);
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    if (messenger) ((PFN_vkDestroyDebugUtilsMessengerEXT)gipa(inst, "vkDestroyDebugUtilsMessengerEXT"))(inst, messenger, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
    }
}
