// SPDX-License-Identifier: MIT
// vkfence_lat: completion latency of a tiny Vulkan submission, ANV loaded directly as an ICD (like vkcaps.c), to compare
// ANV's user-fence completion (ANV_UFENCE=1, mesa/patches/0104) against the plain syncobj wait. Each iteration submits
// one tiny command buffer and waits for it; it prints p50/p90/p99/min/max of vkQueueSubmit -> wait returned, and of the
// vkQueueSubmit call alone, in microseconds. Measurement only.
//   clang -arch x86_64 -O2 -I<mesa>/include -o vkfence_lat vkfence_lat.c
//   ./vkfence_lat [libvulkan_intel.dylib] [mode] [iterations] [queue family]
// modes:
//   empty     an empty command buffer, vkWaitForFences (infinite timeout)
//   fill      vkCmdFillBuffer of 4 bytes (the iteration number) into host-visible VRAM + a barrier to HOST, fence wait;
//             after the wait the CPU checks the value (counts mismatches: memory ordering of the fast path)
//   fillsys   the same into host-visible system memory
//   poll      fill, then vkGetFenceStatus in a loop instead of a blocking wait
//   timeline  fill signalling a timeline semaphore (value = iteration), vkWaitSemaphores
//   behind    two submissions in flight, timeline: wait for the older while the newer runs (DXVK's fence thread)
// The default library is the installed one; queue family 0 is the render queue on ANV.
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define IP(n) ((PFN_##n)gipa(inst, #n))
#define DP(n) ((PFN_##n)gdpa(dev, #n))
#define CHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("%s:%d %s = %d\n", __FILE__, __LINE__, #x, r_); exit(1); } } while (0)

static mach_timebase_info_data_t tb;
static double us(uint64_t a, uint64_t b) { return (double)(b - a) * tb.numer / tb.denom / 1000.0; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
static void stats(const char *what, double *v, int n) {
    qsort(v, n, sizeof *v, cmpd);
    printf("  %-26s p50 %7.1f  p90 %7.1f  p99 %7.1f  min %7.1f  max %8.1f us\n", what, v[n / 2], v[n * 9 / 10], v[n * 99 / 100],
           v[0], v[n - 1]);
}

int main(int argc, char **argv) {
    const char *lib = argc > 1 ? argv[1] : "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib";
    const char *mode = argc > 2 ? argv[2] : "fill";
    const int iters = argc > 3 ? atoi(argv[3]) : 2000;
    const uint32_t family = argc > 4 ? (uint32_t)atoi(argv[4]) : 0;
    const int warm = 50;
    mach_timebase_info(&tb);
    if (iters < 10) { printf("iterations >= 10\n"); return 1; }
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("dlopen %s: %s\n", lib, dlerror()); return 1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { printf("no vk_icdGetInstanceProcAddr\n"); return 1; }

    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vkfence_lat", 1, NULL, 0, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL};
    CHECK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
    uint32_t pc = 1; VkPhysicalDevice pd;
    VkResult er = IP(vkEnumeratePhysicalDevices)(inst, &pc, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !pc) { printf("no physical device\n"); return 1; }
    VkPhysicalDeviceProperties pp; IP(vkGetPhysicalDeviceProperties)(pd, &pp);
    uint32_t nf = 0; IP(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nf, NULL);
    VkQueueFamilyProperties qf[16]; nf = nf > 16 ? 16 : nf; IP(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &nf, qf);
    printf("queue families:");
    for (uint32_t i = 0; i < nf; i++)
        printf(" %u:%s%s%s%s", i, qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT ? "G" : "", qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT ? "C" : "",
               qf[i].queueFlags & VK_QUEUE_TRANSFER_BIT ? "T" : "", qf[i].queueFlags & 0x60 ? "V" : "");
    printf("\n");
    if (family >= nf) { printf("queue family %u of %u\n", family, nf); return 1; }

    VkPhysicalDeviceTimelineSemaphoreFeatures tsf = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES, NULL, VK_TRUE};
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, family, 1, &prio};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &tsf, 0, 1, &qci, 0, NULL, 0, NULL, NULL};
    CHECK(IP(vkCreateDevice)(pd, &dci, NULL, &dev));
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkQueue q; DP(vkGetDeviceQueue)(dev, family, 0, &q);

    // the target of the fill: host-visible + coherent, device-local (BAR VRAM) unless fillsys
    const int sys = !strcmp(mode, "fillsys");
    VkPhysicalDeviceMemoryProperties mp; IP(vkGetPhysicalDeviceMemoryProperties)(pd, &mp);
    VkBufferCreateInfo bci = {VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, 4096, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                              VK_SHARING_MODE_EXCLUSIVE, 0, NULL};
    VkBuffer buf; CHECK(DP(vkCreateBuffer)(dev, &bci, NULL, &buf));
    VkMemoryRequirements mr; DP(vkGetBufferMemoryRequirements)(dev, buf, &mr);
    const VkMemoryPropertyFlags want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                       (sys ? 0 : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    uint32_t mt = UINT32_MAX;
    for (uint32_t i = 0; i < mp.memoryTypeCount && mt == UINT32_MAX; i++) {
        const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if ((mr.memoryTypeBits & (1u << i)) && (f & want) == want && (!sys || !(f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)))
            mt = i;
    }
    if (mt == UINT32_MAX) { printf("no memory type for %s\n", sys ? "system memory" : "host-visible VRAM"); return 1; }
    VkMemoryAllocateInfo mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, mt};
    VkDeviceMemory mem; CHECK(DP(vkAllocateMemory)(dev, &mai, NULL, &mem));
    CHECK(DP(vkBindBufferMemory)(dev, buf, mem, 0));
    volatile uint32_t *map; CHECK(DP(vkMapMemory)(dev, mem, 0, VK_WHOLE_SIZE, 0, (void **)&map));
    map[0] = 0xffffffffu; map[1] = 0xffffffffu;

    VkCommandPoolCreateInfo cpci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL,
                                    VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, family};
    VkCommandPool pool; CHECK(DP(vkCreateCommandPool)(dev, &cpci, NULL, &pool));
    VkCommandBufferAllocateInfo cbai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool,
                                        VK_COMMAND_BUFFER_LEVEL_PRIMARY, 2};
    VkCommandBuffer cb[2]; CHECK(DP(vkAllocateCommandBuffers)(dev, &cbai, cb));
    PFN_vkBeginCommandBuffer begin = DP(vkBeginCommandBuffer);
    PFN_vkEndCommandBuffer end = DP(vkEndCommandBuffer);
    PFN_vkCmdFillBuffer fill = DP(vkCmdFillBuffer);
    PFN_vkCmdPipelineBarrier barrier = DP(vkCmdPipelineBarrier);
    PFN_vkQueueSubmit submit = DP(vkQueueSubmit);
    PFN_vkWaitForFences waitf = DP(vkWaitForFences);
    PFN_vkResetFences resetf = DP(vkResetFences);
    PFN_vkGetFenceStatus statusf = DP(vkGetFenceStatus);
    PFN_vkWaitSemaphores waits = DP(vkWaitSemaphores);
    PFN_vkResetCommandBuffer resetcb = DP(vkResetCommandBuffer);

    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, 0};
    VkFence fence; CHECK(DP(vkCreateFence)(dev, &fci, NULL, &fence));
    VkSemaphoreTypeCreateInfo stci = {VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0};
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &stci, 0};
    VkSemaphore tl; CHECK(DP(vkCreateSemaphore)(dev, &sci, NULL, &tl));

    const int empty = !strcmp(mode, "empty"), poll = !strcmp(mode, "poll"), timeline = !strcmp(mode, "timeline"),
              behind = !strcmp(mode, "behind");
    if (!empty && !poll && !timeline && !behind && strcmp(mode, "fill") && !sys) { printf("unknown mode %s\n", mode); return 1; }
    const char *uf = getenv("ANV_UFENCE"), *spin = getenv("ANV_UFENCE_SPIN_US");
    printf("vkfence_lat: %s, %s, family %u, mode %s, %d iterations, ANV_UFENCE=%s ANV_UFENCE_SPIN_US=%s\n", pp.deviceName, lib,
           family, mode, iters, uf ? uf : "(unset)", spin ? spin : "(unset)");

    // record: fill the iteration number (each command buffer is re-recorded with its value) + barrier to HOST
    const VkMemoryBarrier mb = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT};
    const VkCommandBufferBeginInfo cbbi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL,
                                           VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL};
#define RECORD(c, value, slot) do { \
        CHECK(resetcb((c), 0)); CHECK(begin((c), &cbbi)); \
        if (!empty) { fill((c), buf, (slot) * 4, 4, (value)); \
            barrier((c), VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, NULL, 0, NULL); } \
        CHECK(end((c))); } while (0)

    double *lat = calloc(iters, sizeof *lat), *sub = calloc(iters, sizeof *sub);
    int mismatches = 0;
    uint64_t value = 0;
    if (behind) {
        // keep one submission ahead: submit k+1, then wait for k (older), as a fence thread does
        RECORD(cb[1], 1, 1);
        VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &value};
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, &ts, 0, NULL, NULL, 1, &cb[1], 1, &tl};
        value = 1; CHECK(submit(q, 1, &si, VK_NULL_HANDLE));
        uint64_t t_prev = mach_absolute_time();
        for (int i = 0; i < iters + warm; i++) {
            const uint64_t v = value + 1; const VkCommandBuffer c = cb[v & 1];
            RECORD(c, (uint32_t)v, (uint32_t)(v & 1));
            value = v;
            si.pCommandBuffers = &cb[v & 1];
            const uint64_t t0 = mach_absolute_time();
            CHECK(submit(q, 1, &si, VK_NULL_HANDLE));
            const uint64_t t1 = mach_absolute_time();
            const uint64_t older = v - 1;
            VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &older};
            CHECK(waits(dev, &wi, UINT64_MAX));
            const uint64_t t2 = mach_absolute_time();
            if (map[older & 1] != (uint32_t)older) mismatches++;
            if (i >= warm) { lat[i - warm] = us(t_prev, t2); sub[i - warm] = us(t0, t1); }
            t_prev = t0;   // the older one was submitted one iteration ago
        }
        VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &value};
        CHECK(waits(dev, &wi, UINT64_MAX));
    } else {
        for (int i = 0; i < iters + warm; i++) {
            const uint32_t v = (uint32_t)i + 1;
            RECORD(cb[0], v, 0);
            value++;
            VkTimelineSemaphoreSubmitInfo ts = {VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &value};
            VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, timeline ? &ts : NULL, 0, NULL, NULL, 1, &cb[0],
                               timeline ? 1 : 0, timeline ? &tl : NULL};
            const uint64_t t0 = mach_absolute_time();
            CHECK(submit(q, 1, &si, timeline ? VK_NULL_HANDLE : fence));
            const uint64_t t1 = mach_absolute_time();
            if (timeline) {
                VkSemaphoreWaitInfo wi = {VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &value};
                CHECK(waits(dev, &wi, UINT64_MAX));
            } else if (poll) {
                VkResult r;
                while ((r = statusf(dev, fence)) == VK_NOT_READY) ;
                CHECK(r);
            } else {
                CHECK(waitf(dev, 1, &fence, VK_TRUE, UINT64_MAX));
            }
            const uint64_t t2 = mach_absolute_time();
            if (!empty && map[0] != v) mismatches++;
            if (!timeline) CHECK(resetf(dev, 1, &fence));
            if (i >= warm) { lat[i - warm] = us(t0, t2); sub[i - warm] = us(t0, t1); }
        }
    }
    stats(behind ? "older submit -> wait ret" : "submit -> wait returned", lat, iters);
    stats("vkQueueSubmit call", sub, iters);
    printf("  value mismatches after the wait: %d%s\n", mismatches, empty ? " (not checked: empty)" : "");
    DP(vkDeviceWaitIdle)(dev);
    DP(vkDestroySemaphore)(dev, tl, NULL);
    DP(vkDestroyFence)(dev, fence, NULL);
    DP(vkDestroyCommandPool)(dev, pool, NULL);
    DP(vkUnmapMemory)(dev, mem);
    DP(vkFreeMemory)(dev, mem, NULL);
    DP(vkDestroyBuffer)(dev, buf, NULL);
    DP(vkDestroyDevice)(dev, NULL);
    IP(vkDestroyInstance)(inst, NULL);
    return mismatches ? 2 : 0;
}
