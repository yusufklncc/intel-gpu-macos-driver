// SPDX-License-Identifier: MIT
// vksparse: does image residency work on this kernel? The gate for Metal sparse textures (NVMTL_SPARSE_TEXTURES).
//
// ANV advertises sparseBinding, sparseResidencyImage2D/3D, sparseResidencyAliased and shaderResourceResidency here
// (vkcaps), through its VM_BIND path (anv_sparse.c anv_sparse_bind_vm_bind: xe VM_BIND MAP of a BO range per tile, and a
// DRM_XE_VM_BIND_FLAG_NULL range for every tile that is not bound, which IntelXe.kext writes as NULL PTEs). Nothing had
// bound image tiles on this kernel before. This test loads the installed ANV directly as an ICD (like vkcaps.c) and:
//   1. prints the sparse properties, the sparse image format properties of RGBA8 2D and the image's sparse memory
//      requirements (tile shape, tile bytes, mip tail);
//   2. binds 4 of the 16 tiles of a 4x4-tile RGBA8 image with vkQueueBindSparse (semaphore-ordered after a submission
//      on the same queue), uploads a distinct pattern into each bound tile (vkCmdCopyBufferToImage over the whole
//      image: the unbound tiles' writes must vanish), reads the whole image back and checks every tile: bound tiles
//      their pattern, unbound tiles zero (Vulkan guarantees zero only with residencyNonResidentStrict, which is printed);
//   3. runs a compute shader that reads each tile's centre with OpImageSparseFetch and OpImageSparseSampleExplicitLod and
//      reports OpImageSparseTexelsResident: resident exactly where bound;
//   4. remaps: unbinds one tile and binds another tile position to the page the first one used (its data must follow:
//      Tile64 tiles are position-independent), unbinds a second tile, and checks again (copy and shader);
//   5. binds the mip tail of a mipmapped image (VkSparseImageOpaqueMemoryBindInfo) and round-trips its smallest level.
// Every check prints PASS or FAIL; the last line is "vksparse: N PASS, M FAIL". Exit 0 when nothing failed, 1 on a
// failed check, 2 when sparse residency is not offered or the setup failed.
//
//   clang -arch x86_64 -O1 -I<mesa>/include -o vksparse vksparse.c
//   ./vksparse [libvulkan_intel.dylib]          (default /Library/GPUBundles/nvmtl/libvulkan_intel.dylib)
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkDevice dev;
#define IP(n) ((PFN_##n)gipa(inst, #n))
#define DP(n) ((PFN_##n)gdpa(dev, #n))
static int npass, nfail;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
#include <stdarg.h>
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
#define VK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { printf("SETUP FAIL: %s -> %d (line %d)\n", #x, r_, __LINE__); return 2; } } while (0)

// probe.comp, compiled with glslangValidator -V --target-env vulkan1.2:
//   #version 450
//   #extension GL_ARB_sparse_texture2 : require
//   // one invocation per tile: residency and texel of the tile's centre, by fetch (OpImageSparseFetch) and by an explicit-LOD
//   // sample (OpImageSparseSampleExplicitLod), both followed by OpImageSparseTexelsResident
//   layout(local_size_x = 1, local_size_y = 1) in;
//   layout(binding = 0) uniform sampler2D tex;
//   layout(std430, binding = 1) buffer Out { uvec4 o[]; };
//   layout(push_constant) uniform P { ivec2 tile; uint tiles_x; } pc;
//   void main() {
//     ivec2 t = ivec2(gl_GlobalInvocationID.xy);
//     ivec2 c = t * pc.tile + pc.tile / 2;
//     vec4 f, s;
//     int cf = sparseTexelFetchARB(tex, c, 0, f);
//     int cs = sparseTextureLodARB(tex, (vec2(c) + 0.5) / vec2(textureSize(tex, 0)), 0.0, s);
//     o[t.y * pc.tiles_x + t.x] = uvec4(sparseTexelsResidentARB(cf) ? 1u : 0u, packUnorm4x8(f), sparseTexelsResidentARB(cs) ? 1u : 0u, packUnorm4x8(s));
//   }
static const uint32_t kProbe[] = {
    0x07230203, 0x00010500, 0x0008000b, 0x00000067, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x00000029, 0x00020011, 0x00000032, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e,
    0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0009000f, 0x00000005, 0x00000004, 0x6e69616d,
    0x00000000, 0x0000000d, 0x00000016, 0x00000028, 0x00000049, 0x00060010, 0x00000004, 0x00000011,
    0x00000001, 0x00000001, 0x00000001, 0x00030003, 0x00000002, 0x000001c2, 0x00070004, 0x415f4c47,
    0x735f4252, 0x73726170, 0x65745f65, 0x72757478, 0x00003265, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00030005, 0x00000009, 0x00000074, 0x00080005, 0x0000000d, 0x475f6c67, 0x61626f6c,
    0x766e496c, 0x7461636f, 0x496e6f69, 0x00000044, 0x00030005, 0x00000012, 0x00000063, 0x00030005,
    0x00000014, 0x00000050, 0x00050006, 0x00000014, 0x00000000, 0x656c6974, 0x00000000, 0x00050006,
    0x00000014, 0x00000001, 0x656c6974, 0x00785f73, 0x00030005, 0x00000016, 0x00006370, 0x00030005,
    0x00000023, 0x00006663, 0x00030005, 0x00000028, 0x00786574, 0x00030005, 0x0000002d, 0x00000066,
    0x00040005, 0x0000002f, 0x54736552, 0x00657079, 0x00030005, 0x00000033, 0x00007363, 0x00030005,
    0x00000041, 0x00000073, 0x00030005, 0x00000047, 0x0074754f, 0x00040006, 0x00000047, 0x00000000,
    0x0000006f, 0x00030005, 0x00000049, 0x00000000, 0x00040047, 0x0000000d, 0x0000000b, 0x0000001c,
    0x00030047, 0x00000014, 0x00000002, 0x00050048, 0x00000014, 0x00000000, 0x00000023, 0x00000000,
    0x00050048, 0x00000014, 0x00000001, 0x00000023, 0x00000008, 0x00040047, 0x00000028, 0x00000021,
    0x00000000, 0x00040047, 0x00000028, 0x00000022, 0x00000000, 0x00040047, 0x00000046, 0x00000006,
    0x00000010, 0x00030047, 0x00000047, 0x00000002, 0x00050048, 0x00000047, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000049, 0x00000021, 0x00000001, 0x00040047, 0x00000049, 0x00000022,
    0x00000000, 0x00040047, 0x00000066, 0x0000000b, 0x00000019, 0x00020013, 0x00000002, 0x00030021,
    0x00000003, 0x00000002, 0x00040015, 0x00000006, 0x00000020, 0x00000001, 0x00040017, 0x00000007,
    0x00000006, 0x00000002, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040015, 0x0000000a,
    0x00000020, 0x00000000, 0x00040017, 0x0000000b, 0x0000000a, 0x00000003, 0x00040020, 0x0000000c,
    0x00000001, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000001, 0x00040017, 0x0000000e,
    0x0000000a, 0x00000002, 0x0004001e, 0x00000014, 0x00000007, 0x0000000a, 0x00040020, 0x00000015,
    0x00000009, 0x00000014, 0x0004003b, 0x00000015, 0x00000016, 0x00000009, 0x0004002b, 0x00000006,
    0x00000017, 0x00000000, 0x00040020, 0x00000018, 0x00000009, 0x00000007, 0x0004002b, 0x00000006,
    0x0000001e, 0x00000002, 0x00040020, 0x00000022, 0x00000007, 0x00000006, 0x00030016, 0x00000024,
    0x00000020, 0x00090019, 0x00000025, 0x00000024, 0x00000001, 0x00000000, 0x00000000, 0x00000000,
    0x00000001, 0x00000000, 0x0003001b, 0x00000026, 0x00000025, 0x00040020, 0x00000027, 0x00000000,
    0x00000026, 0x0004003b, 0x00000027, 0x00000028, 0x00000000, 0x00040017, 0x0000002b, 0x00000024,
    0x00000004, 0x00040020, 0x0000002c, 0x00000007, 0x0000002b, 0x0004001e, 0x0000002f, 0x00000006,
    0x0000002b, 0x00040017, 0x00000036, 0x00000024, 0x00000002, 0x0004002b, 0x00000024, 0x00000038,
    0x3f000000, 0x0004002b, 0x00000024, 0x00000040, 0x00000000, 0x00040017, 0x00000045, 0x0000000a,
    0x00000004, 0x0003001d, 0x00000046, 0x00000045, 0x0003001e, 0x00000047, 0x00000046, 0x00040020,
    0x00000048, 0x0000000c, 0x00000047, 0x0004003b, 0x00000048, 0x00000049, 0x0000000c, 0x0004002b,
    0x0000000a, 0x0000004a, 0x00000001, 0x0004002b, 0x00000006, 0x0000004e, 0x00000001, 0x00040020,
    0x0000004f, 0x00000009, 0x0000000a, 0x0004002b, 0x0000000a, 0x00000053, 0x00000000, 0x00020014,
    0x00000059, 0x00040020, 0x00000064, 0x0000000c, 0x00000045, 0x0006002c, 0x0000000b, 0x00000066,
    0x0000004a, 0x0000004a, 0x0000004a, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003b, 0x00000008,
    0x00000012, 0x00000007, 0x0004003b, 0x00000022, 0x00000023, 0x00000007, 0x0004003b, 0x0000002c,
    0x0000002d, 0x00000007, 0x0004003b, 0x00000022, 0x00000033, 0x00000007, 0x0004003b, 0x0000002c,
    0x00000041, 0x00000007, 0x0004003d, 0x0000000b, 0x0000000f, 0x0000000d, 0x0007004f, 0x0000000e,
    0x00000010, 0x0000000f, 0x0000000f, 0x00000000, 0x00000001, 0x0004007c, 0x00000007, 0x00000011,
    0x00000010, 0x0003003e, 0x00000009, 0x00000011, 0x0004003d, 0x00000007, 0x00000013, 0x00000009,
    0x00050041, 0x00000018, 0x00000019, 0x00000016, 0x00000017, 0x0004003d, 0x00000007, 0x0000001a,
    0x00000019, 0x00050084, 0x00000007, 0x0000001b, 0x00000013, 0x0000001a, 0x00050041, 0x00000018,
    0x0000001c, 0x00000016, 0x00000017, 0x0004003d, 0x00000007, 0x0000001d, 0x0000001c, 0x00050050,
    0x00000007, 0x0000001f, 0x0000001e, 0x0000001e, 0x00050087, 0x00000007, 0x00000020, 0x0000001d,
    0x0000001f, 0x00050080, 0x00000007, 0x00000021, 0x0000001b, 0x00000020, 0x0003003e, 0x00000012,
    0x00000021, 0x0004003d, 0x00000026, 0x00000029, 0x00000028, 0x0004003d, 0x00000007, 0x0000002a,
    0x00000012, 0x00040064, 0x00000025, 0x0000002e, 0x00000029, 0x00070139, 0x0000002f, 0x00000030,
    0x0000002e, 0x0000002a, 0x00000002, 0x00000017, 0x00050051, 0x0000002b, 0x00000031, 0x00000030,
    0x00000001, 0x0003003e, 0x0000002d, 0x00000031, 0x00050051, 0x00000006, 0x00000032, 0x00000030,
    0x00000000, 0x0003003e, 0x00000023, 0x00000032, 0x0004003d, 0x00000026, 0x00000034, 0x00000028,
    0x0004003d, 0x00000007, 0x00000035, 0x00000012, 0x0004006f, 0x00000036, 0x00000037, 0x00000035,
    0x00050050, 0x00000036, 0x00000039, 0x00000038, 0x00000038, 0x00050081, 0x00000036, 0x0000003a,
    0x00000037, 0x00000039, 0x0004003d, 0x00000026, 0x0000003b, 0x00000028, 0x00040064, 0x00000025,
    0x0000003c, 0x0000003b, 0x00050067, 0x00000007, 0x0000003d, 0x0000003c, 0x00000017, 0x0004006f,
    0x00000036, 0x0000003e, 0x0000003d, 0x00050088, 0x00000036, 0x0000003f, 0x0000003a, 0x0000003e,
    0x00070132, 0x0000002f, 0x00000042, 0x00000034, 0x0000003f, 0x00000002, 0x00000040, 0x00050051,
    0x0000002b, 0x00000043, 0x00000042, 0x00000001, 0x0003003e, 0x00000041, 0x00000043, 0x00050051,
    0x00000006, 0x00000044, 0x00000042, 0x00000000, 0x0003003e, 0x00000033, 0x00000044, 0x00050041,
    0x00000022, 0x0000004b, 0x00000009, 0x0000004a, 0x0004003d, 0x00000006, 0x0000004c, 0x0000004b,
    0x0004007c, 0x0000000a, 0x0000004d, 0x0000004c, 0x00050041, 0x0000004f, 0x00000050, 0x00000016,
    0x0000004e, 0x0004003d, 0x0000000a, 0x00000051, 0x00000050, 0x00050084, 0x0000000a, 0x00000052,
    0x0000004d, 0x00000051, 0x00050041, 0x00000022, 0x00000054, 0x00000009, 0x00000053, 0x0004003d,
    0x00000006, 0x00000055, 0x00000054, 0x0004007c, 0x0000000a, 0x00000056, 0x00000055, 0x00050080,
    0x0000000a, 0x00000057, 0x00000052, 0x00000056, 0x0004003d, 0x00000006, 0x00000058, 0x00000023,
    0x0004013c, 0x00000059, 0x0000005a, 0x00000058, 0x000600a9, 0x0000000a, 0x0000005b, 0x0000005a,
    0x0000004a, 0x00000053, 0x0004003d, 0x0000002b, 0x0000005c, 0x0000002d, 0x0006000c, 0x0000000a,
    0x0000005d, 0x00000001, 0x00000037, 0x0000005c, 0x0004003d, 0x00000006, 0x0000005e, 0x00000033,
    0x0004013c, 0x00000059, 0x0000005f, 0x0000005e, 0x000600a9, 0x0000000a, 0x00000060, 0x0000005f,
    0x0000004a, 0x00000053, 0x0004003d, 0x0000002b, 0x00000061, 0x00000041, 0x0006000c, 0x0000000a,
    0x00000062, 0x00000001, 0x00000037, 0x00000061, 0x00070050, 0x00000045, 0x00000063, 0x0000005b,
    0x0000005d, 0x00000060, 0x00000062, 0x00060041, 0x00000064, 0x00000065, 0x00000049, 0x00000017,
    0x00000057, 0x0003003e, 0x00000065, 0x00000063, 0x000100fd, 0x00010038,
};

static uint64_t now_us(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1000; }

static VkPhysicalDeviceMemoryProperties memp;
static uint32_t mem_type(uint32_t bits, VkMemoryPropertyFlags want)
{
    for (uint32_t i = 0; i < memp.memoryTypeCount; i++)
        if ((bits & (1u << i)) && (memp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

// the pattern of tile t (row-major tile index), as packed RGBA8 bytes R,G,B,A = t*13+1, 0x55, 0xAA, 0xFF; page p of the
// pool is marked by the tile it was first filled for
static uint32_t pattern(uint32_t t) { return (uint32_t)((t * 13 + 1) & 0xff) | 0x55u << 8 | 0xAAu << 16 | 0xFFu << 24; }

typedef struct { VkBuffer buf; VkDeviceMemory mem; void *map; VkDeviceSize size; } hbuf;
static int hbuf_make(VkDeviceSize size, VkBufferUsageFlags usage, hbuf *out)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VK(DP(vkCreateBuffer)(dev, &bci, NULL, &out->buf));
    VkMemoryRequirements mr; DP(vkGetBufferMemoryRequirements)(dev, out->buf, &mr);
    uint32_t t = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (t == UINT32_MAX) { printf("SETUP FAIL: no host-visible coherent memory type\n"); return 2; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, t };
    VK(DP(vkAllocateMemory)(dev, &mai, NULL, &out->mem));
    VK(DP(vkBindBufferMemory)(dev, out->buf, out->mem, 0));
    VK(DP(vkMapMemory)(dev, out->mem, 0, size, 0, &out->map));
    out->size = size;
    return 0;
}

static VkQueue queue;
static VkCommandPool pool;
static VkSemaphore tl;      // one timeline orders every submission and bind of the test
static uint64_t tlv;        // its last value handed out

// one queue submission of a recorded command buffer, after timeline point tlv, signalling tlv + 1; waited for on the CPU
static int submit_wait(VkCommandBuffer cb, const char *what)
{
    const uint64_t wait = tlv, sig = ++tlv;
    VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &wait, 1, &sig };
    const VkPipelineStageFlags st = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &tsi, 1, &tl, &st, 1, &cb, 1, &tl };
    VK(DP(vkQueueSubmit)(queue, 1, &si, VK_NULL_HANDLE));
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &sig };
    const uint64_t t0 = now_us();
    VkResult r = DP(vkWaitSemaphores)(dev, &wi, 5000000000ull);
    if (r != VK_SUCCESS) { printf("SETUP FAIL: %s: submission did not complete (%d)\n", what, r); return 2; }
    printf("  %s: done in %llu us\n", what, (unsigned long long)(now_us() - t0));
    return 0;
}

// vkQueueBindSparse after timeline point tlv, signalling tlv + 1 (ordered after every earlier submission of the test,
// whose points precede it); waited for on the CPU, so a failure is pinned to the bind
static int bind_wait(const VkBindSparseInfo *in, const char *what)
{
    const uint64_t wait = tlv, sig = ++tlv;
    VkTimelineSemaphoreSubmitInfo tsi = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &wait, 1, &sig };
    VkBindSparseInfo bi = *in;
    bi.pNext = &tsi; bi.waitSemaphoreCount = 1; bi.pWaitSemaphores = &tl; bi.signalSemaphoreCount = 1; bi.pSignalSemaphores = &tl;
    const uint64_t t0 = now_us();
    VkResult r = DP(vkQueueBindSparse)(queue, 1, &bi, VK_NULL_HANDLE);
    check(r == VK_SUCCESS, "%s: vkQueueBindSparse -> %d (%llu us in the call)", what, r, (unsigned long long)(now_us() - t0));
    if (r != VK_SUCCESS) return 2;
    VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &tl, &sig };
    r = DP(vkWaitSemaphores)(dev, &wi, 5000000000ull);
    check(r == VK_SUCCESS, "%s: its signal semaphore reached %llu (%d)", what, (unsigned long long)sig, r);
    return r == VK_SUCCESS ? 0 : 2;
}

static VkCommandBuffer cb_begin(void)
{
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cb = VK_NULL_HANDLE;
    if (DP(vkAllocateCommandBuffers)(dev, &ai, &cb) != VK_SUCCESS) return VK_NULL_HANDLE;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    DP(vkBeginCommandBuffer)(cb, &bi);
    return cb;
}
static void barrier_all(VkCommandBuffer cb)
{
    VkMemoryBarrier mb = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT };
    DP(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, NULL, 0, NULL);
}

// the test image: TX x TY tiles of GW x GH texels
enum { TX = 4, TY = 4 };
static uint32_t GW, GH, W, H;
static VkImage img;
static VkImageView view;
static hbuf up, down, sout;
static VkPipeline pipe;
static VkPipelineLayout playout;
static VkDescriptorSet dset;
// which pool page each tile is bound to (-1: unbound), and which pattern each page holds
static int page_of[TX * TY];
static uint32_t page_pattern[TX * TY];

// upload: every tile gets pattern(t) through one copy over the whole image (unbound tiles' writes must be dropped);
// readback: the whole image to `down`
static int upload_all(void)
{
    uint32_t *p = (uint32_t *)up.map;
    for (uint32_t y = 0; y < H; y++) for (uint32_t x = 0; x < W; x++) p[y * W + x] = pattern((y / GH) * TX + x / GW);
    VkCommandBuffer cb = cb_begin();
    VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, img, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    DP(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
    VkBufferImageCopy c = { 0, W, H, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    DP(vkCmdCopyBufferToImage)(cb, up.buf, img, VK_IMAGE_LAYOUT_GENERAL, 1, &c);
    barrier_all(cb);
    DP(vkEndCommandBuffer)(cb);
    return submit_wait(cb, "upload (one copy over all 16 tiles)");
}
static int readback_and_probe(void)
{
    VkCommandBuffer cb = cb_begin();
    barrier_all(cb);
    VkBufferImageCopy c = { 0, W, H, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 0, 0, 0 }, { W, H, 1 } };
    DP(vkCmdCopyImageToBuffer)(cb, img, VK_IMAGE_LAYOUT_GENERAL, down.buf, 1, &c);
    memset(sout.map, 0xEE, sout.size);
    DP(vkCmdBindPipeline)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipe);
    DP(vkCmdBindDescriptorSets)(cb, VK_PIPELINE_BIND_POINT_COMPUTE, playout, 0, 1, &dset, 0, NULL);
    struct { int32_t tw, th; uint32_t tiles_x; } pc = { (int32_t)GW, (int32_t)GH, TX };
    DP(vkCmdPushConstants)(cb, playout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    DP(vkCmdDispatch)(cb, TX, TY, 1);
    barrier_all(cb);
    DP(vkEndCommandBuffer)(cb);
    return submit_wait(cb, "readback copy + residency shader");
}
// every tile against page_of / page_pattern: the copy's texels (all of the tile), the shader's fetch and sample
static void check_tiles(const char *when, int strict)
{
    const uint32_t *p = (const uint32_t *)down.map;
    const uint32_t *o = (const uint32_t *)sout.map;
    for (uint32_t t = 0; t < TX * TY; t++) {
        const uint32_t tx = t % TX, ty = t / TX;
        const int bound = page_of[t] >= 0;
        const uint32_t want = bound ? page_pattern[page_of[t]] : 0;
        uint32_t bad = 0, first = 0, firstv = 0;
        for (uint32_t y = ty * GH; y < (ty + 1) * GH; y++) for (uint32_t x = tx * GW; x < (tx + 1) * GW; x++)
            if (p[y * W + x] != want) { if (!bad++) { first = y * W + x; firstv = p[first]; } }
        if (bound || strict)
            check(!bad, "%s: tile (%u,%u) %s: copy reads 0x%08x in all %u texels%s (%u differ; first at %u,%u = 0x%08x)", when, tx, ty,
                  bound ? "bound" : "unbound", want, GW * GH, bound ? "" : " (residencyNonResidentStrict)", bad, first % W, first / W, firstv);
        else printf("  %s: tile (%u,%u) unbound: copy reads %s (not checked: residencyNonResidentStrict is 0)\n", when, tx, ty, bad ? "nonzero" : "zero");
        const uint32_t *r = o + t * 4;
        check(r[0] == (uint32_t)bound, "%s: tile (%u,%u) OpImageSparseFetch resident=%u (want %u), texel 0x%08x", when, tx, ty, r[0], bound, r[1]);
        check(r[2] == (uint32_t)bound, "%s: tile (%u,%u) OpImageSparseSampleExplicitLod resident=%u (want %u), texel 0x%08x", when, tx, ty, r[2], bound, r[3]);
        if (bound) check(r[1] == want && r[3] == want, "%s: tile (%u,%u) shader texels 0x%08x / 0x%08x (want 0x%08x)", when, tx, ty, r[1], r[3], want);
    }
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char *lib = argc > 1 ? argv[1] : "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { printf("SETUP FAIL: dlopen %s: %s\n", lib, dlerror()); return 2; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) { printf("SETUP FAIL: no vk_icdGetInstanceProcAddr in %s\n", lib); return 2; }
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vksparse", 1, NULL, 0, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 0, NULL };
    VK(((PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance"))(&ici, NULL, &inst));
    uint32_t pc = 1; VkPhysicalDevice pd;
    VkResult er = IP(vkEnumeratePhysicalDevices)(inst, &pc, &pd);
    if ((er != VK_SUCCESS && er != VK_INCOMPLETE) || !pc) { printf("SETUP FAIL: no physical device (%d)\n", er); return 2; }
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    VkPhysicalDeviceProperties pp; IP(vkGetPhysicalDeviceProperties)(pd, &pp);
    IP(vkGetPhysicalDeviceMemoryProperties)(pd, &memp);
    VkPhysicalDeviceFeatures f; IP(vkGetPhysicalDeviceFeatures)(pd, &f);
    printf("device: %s, API %u.%u.%u\n", pp.deviceName, VK_API_VERSION_MAJOR(pp.apiVersion), VK_API_VERSION_MINOR(pp.apiVersion), VK_API_VERSION_PATCH(pp.apiVersion));
    printf("features: sparseBinding %u sparseResidencyImage2D %u sparseResidencyImage3D %u sparseResidencyAliased %u shaderResourceResidency %u shaderResourceMinLod %u\n",
           f.sparseBinding, f.sparseResidencyImage2D, f.sparseResidencyImage3D, f.sparseResidencyAliased, f.shaderResourceResidency, f.shaderResourceMinLod);
    const VkPhysicalDeviceSparseProperties *sp = &pp.sparseProperties;
    printf("sparse properties: residencyStandard2DBlockShape %u residencyStandard3DBlockShape %u residencyAlignedMipSize %u residencyNonResidentStrict %u; sparseAddressSpaceSize %llu MiB\n",
           sp->residencyStandard2DBlockShape, sp->residencyStandard3DBlockShape, sp->residencyAlignedMipSize, sp->residencyNonResidentStrict,
           (unsigned long long)(pp.limits.sparseAddressSpaceSize >> 20));
    if (!f.sparseBinding || !f.sparseResidencyImage2D || !f.shaderResourceResidency) { printf("SETUP FAIL: sparse image residency is not offered\n"); return 2; }

    uint32_t qn = 0; IP(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &qn, NULL);
    VkQueueFamilyProperties qf[16]; if (qn > 16) qn = 16; IP(vkGetPhysicalDeviceQueueFamilyProperties)(pd, &qn, qf);
    uint32_t fam = UINT32_MAX;
    for (uint32_t i = 0; i < qn; i++) {
        const VkQueueFlags need = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_SPARSE_BINDING_BIT;
        if ((qf[i].queueFlags & need) == need) { fam = i; break; }
    }
    if (fam == UINT32_MAX) { printf("SETUP FAIL: no graphics+compute queue family with VK_QUEUE_SPARSE_BINDING_BIT\n"); return 2; }
    printf("queue family %u: flags 0x%x (the plugin's queue: graphics+compute, sparse binding on the same queue)\n", fam, qf[fam].queueFlags);

    // the sparse image format properties of the test format
    const VkFormat fmt = VK_FORMAT_R8G8B8A8_UNORM;
    const VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    uint32_t nfp = 0;
    IP(vkGetPhysicalDeviceSparseImageFormatProperties)(pd, fmt, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &nfp, NULL);
    VkSparseImageFormatProperties fp[4]; if (nfp > 4) nfp = 4;
    IP(vkGetPhysicalDeviceSparseImageFormatProperties)(pd, fmt, VK_IMAGE_TYPE_2D, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &nfp, fp);
    check(nfp >= 1, "vkGetPhysicalDeviceSparseImageFormatProperties(RGBA8 2D): %u entr%s", nfp, nfp == 1 ? "y" : "ies");
    if (!nfp) return 2;
    for (uint32_t i = 0; i < nfp; i++)
        printf("  aspect 0x%x granularity %ux%ux%u flags 0x%x (single miptail %u, aligned mip size %u, nonstandard block %u)\n", fp[i].aspectMask,
               fp[i].imageGranularity.width, fp[i].imageGranularity.height, fp[i].imageGranularity.depth, fp[i].flags,
               !!(fp[i].flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT), !!(fp[i].flags & VK_SPARSE_IMAGE_FORMAT_ALIGNED_MIP_SIZE_BIT),
               !!(fp[i].flags & VK_SPARSE_IMAGE_FORMAT_NONSTANDARD_BLOCK_SIZE_BIT));
    GW = fp[0].imageGranularity.width; GH = fp[0].imageGranularity.height; W = GW * TX; H = GH * TY;
    // a 3D format too, for the record (Metal's 3D sparse textures)
    { uint32_t n3 = 1; VkSparseImageFormatProperties f3;
      IP(vkGetPhysicalDeviceSparseImageFormatProperties)(pd, fmt, VK_IMAGE_TYPE_3D, VK_SAMPLE_COUNT_1_BIT, usage, VK_IMAGE_TILING_OPTIMAL, &n3, &f3);
      if (n3) printf("  3D RGBA8 granularity %ux%ux%u\n", f3.imageGranularity.width, f3.imageGranularity.height, f3.imageGranularity.depth);
      else printf("  3D RGBA8: no sparse format properties\n"); }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, fam, 1, &prio };
    VkPhysicalDeviceFeatures en = { 0 };
    en.sparseBinding = VK_TRUE; en.sparseResidencyImage2D = VK_TRUE; en.shaderResourceResidency = VK_TRUE;
    en.sparseResidencyAliased = f.sparseResidencyAliased;
    VkPhysicalDeviceVulkan12Features v12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES };
    v12.timelineSemaphore = VK_TRUE;
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &v12, 0, 1, &qci, 0, NULL, 0, NULL, &en };
    VK(IP(vkCreateDevice)(pd, &dci, NULL, &dev));
    DP(vkGetDeviceQueue)(dev, fam, 0, &queue);
    VkCommandPoolCreateInfo cpi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, fam };
    VK(DP(vkCreateCommandPool)(dev, &cpi, NULL, &pool));
    VkSemaphoreTypeCreateInfo sti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &sti, 0 };
    VK(DP(vkCreateSemaphore)(dev, &sci, NULL, &tl));

    // the sparse image and its requirements
    VkImageCreateInfo ii = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
        VK_IMAGE_TYPE_2D, fmt, { W, H, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, usage, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED };
    VkResult cr = DP(vkCreateImage)(dev, &ii, NULL, &img);
    check(cr == VK_SUCCESS, "vkCreateImage %ux%u RGBA8 SPARSE_BINDING|SPARSE_RESIDENCY -> %d", W, H, cr);
    if (cr) return 2;
    VkMemoryRequirements mr; DP(vkGetImageMemoryRequirements)(dev, img, &mr);
    printf("  memory requirements: size %llu, alignment (tile bytes) %llu, type bits 0x%x\n", (unsigned long long)mr.size,
           (unsigned long long)mr.alignment, mr.memoryTypeBits);
    uint32_t nsr = 0; DP(vkGetImageSparseMemoryRequirements)(dev, img, &nsr, NULL);
    VkSparseImageMemoryRequirements sr[2]; if (nsr > 2) nsr = 2; DP(vkGetImageSparseMemoryRequirements)(dev, img, &nsr, sr);
    check(nsr >= 1, "vkGetImageSparseMemoryRequirements: %u entr%s", nsr, nsr == 1 ? "y" : "ies");
    for (uint32_t i = 0; i < nsr; i++)
        printf("  granularity %ux%ux%u, mip tail first lod %u size %llu offset %llu stride %llu\n", sr[i].formatProperties.imageGranularity.width,
               sr[i].formatProperties.imageGranularity.height, sr[i].formatProperties.imageGranularity.depth, sr[i].imageMipTailFirstLod,
               (unsigned long long)sr[i].imageMipTailSize, (unsigned long long)sr[i].imageMipTailOffset, (unsigned long long)sr[i].imageMipTailStride);
    const VkDeviceSize tile = mr.alignment;
    check(tile && (uint64_t)GW * GH * 4 == tile, "one tile is %ux%u RGBA8 texels = %llu bytes (the alignment)", GW, GH, (unsigned long long)tile);

    // the pool: one VkDeviceMemory of TX*TY pages, as a Metal sparse heap would be
    const uint32_t mt = mem_type(mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt == UINT32_MAX) { printf("SETUP FAIL: no device-local memory type in 0x%x\n", mr.memoryTypeBits); return 2; }
    VkMemoryAllocateInfo pai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, tile * TX * TY, mt };
    VkDeviceMemory poolmem; VK(DP(vkAllocateMemory)(dev, &pai, NULL, &poolmem));
    printf("  tile pool: %llu bytes, memory type %u\n", (unsigned long long)pai.allocationSize, mt);

    if (hbuf_make((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, &up) || hbuf_make((VkDeviceSize)W * H * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, &down)
        || hbuf_make(TX * TY * 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &sout)) return 2;

    // the residency shader
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, img, VK_IMAGE_VIEW_TYPE_2D, fmt, { 0 },
        { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
    VK(DP(vkCreateImageView)(dev, &vci, NULL, &view));
    VkSamplerCreateInfo smi = { VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO, NULL, 0, VK_FILTER_NEAREST, VK_FILTER_NEAREST, VK_SAMPLER_MIPMAP_MODE_NEAREST,
        VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE };
    VkSampler smp; VK(DP(vkCreateSampler)(dev, &smi, NULL, &smp));
    VkDescriptorSetLayoutBinding lb[2] = { { 0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL },
                                           { 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL } };
    VkDescriptorSetLayoutCreateInfo dli = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL, 0, 2, lb };
    VkDescriptorSetLayout dsl; VK(DP(vkCreateDescriptorSetLayout)(dev, &dli, NULL, &dsl));
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, 12 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, 1, &dsl, 1, &pcr };
    VK(DP(vkCreatePipelineLayout)(dev, &pli, NULL, &playout));
    VkShaderModuleCreateInfo smci = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, sizeof kProbe, kProbe };
    VkShaderModule sm; VK(DP(vkCreateShaderModule)(dev, &smci, NULL, &sm));
    VkComputePipelineCreateInfo cpci = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL, 0,
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, sm, "main", NULL }, playout, VK_NULL_HANDLE, 0 };
    VkResult pr = DP(vkCreateComputePipelines)(dev, VK_NULL_HANDLE, 1, &cpci, NULL, &pipe);
    check(pr == VK_SUCCESS, "compute pipeline with OpImageSparseFetch / OpImageSparseSampleExplicitLod / OpImageSparseTexelsResident -> %d", pr);
    if (pr) return 2;
    VkDescriptorPoolSize ps[2] = { { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1 }, { VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1 } };
    VkDescriptorPoolCreateInfo dpi = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, NULL, 0, 1, 2, ps };
    VkDescriptorPool dp; VK(DP(vkCreateDescriptorPool)(dev, &dpi, NULL, &dp));
    VkDescriptorSetAllocateInfo dai = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, NULL, dp, 1, &dsl };
    VK(DP(vkAllocateDescriptorSets)(dev, &dai, &dset));
    VkDescriptorImageInfo dii = { smp, view, VK_IMAGE_LAYOUT_GENERAL };
    VkDescriptorBufferInfo dbi = { sout.buf, 0, VK_WHOLE_SIZE };
    VkWriteDescriptorSet wd[2] = { { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, dset, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &dii, NULL, NULL },
                                   { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, dset, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, NULL, &dbi, NULL } };
    DP(vkUpdateDescriptorSets)(dev, 2, wd, 0, NULL);

    // an empty submission first, so the first bind waits for a point a submission signals (the plugin's order: a
    // submission's signal covers every earlier submission, the bind waits for it, the next submission waits for the bind)
    { VkCommandBuffer cb = cb_begin(); DP(vkEndCommandBuffer)(cb); if (submit_wait(cb, "empty submission before the first bind")) return 2; }

    // 1. bind four tiles to pages 0..3
    for (int t = 0; t < TX * TY; t++) page_of[t] = -1;
    static const uint32_t first[4] = { 0, 1, 6, 15 };   // (0,0) (1,0) (2,1) (3,3)
    VkSparseImageMemoryBind b[8]; uint32_t nb = 0;
    for (uint32_t k = 0; k < 4; k++) {
        const uint32_t t = first[k];
        b[nb++] = (VkSparseImageMemoryBind){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { (int32_t)((t % TX) * GW), (int32_t)((t / TX) * GH), 0 },
                                            { GW, GH, 1 }, poolmem, tile * k, 0 };
        page_of[t] = (int)k; page_pattern[k] = pattern(t);
    }
    VkSparseImageMemoryBindInfo ibi = { img, nb, b };
    VkBindSparseInfo bsi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 0, NULL, 1, &ibi, 0, NULL };
    if (bind_wait(&bsi, "bind tiles (0,0) (1,0) (2,1) (3,3) to pages 0-3")) goto out;
    if (upload_all() || readback_and_probe()) goto out;
    check_tiles("after the first binds", sp->residencyNonResidentStrict);

    // 2. remap: (2,1) unbound, (0,1) takes its page 2 (its data must follow), (1,0) unbound; nothing re-uploaded
    nb = 0;
    b[nb++] = (VkSparseImageMemoryBind){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { (int32_t)(2 * GW), (int32_t)(1 * GH), 0 }, { GW, GH, 1 }, VK_NULL_HANDLE, 0, 0 };
    b[nb++] = (VkSparseImageMemoryBind){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, (int32_t)(1 * GH), 0 }, { GW, GH, 1 }, poolmem, tile * 2, 0 };
    b[nb++] = (VkSparseImageMemoryBind){ { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { (int32_t)(1 * GW), 0, 0 }, { GW, GH, 1 }, VK_NULL_HANDLE, 0, 0 };
    page_of[6] = -1; page_of[4] = 2; page_of[1] = -1;
    ibi.bindCount = nb;
    if (bind_wait(&bsi, "remap: unbind (2,1) and (1,0), bind (0,1) to page 2")) goto out;
    if (readback_and_probe()) goto out;
    check_tiles("after the remap", sp->residencyNonResidentStrict);

    // 3. a mipmapped image: the mip tail through an opaque bind, one level-0 tile, and the smallest level round-tripped
    {
        VkImageCreateInfo mi = ii; mi.mipLevels = 1; for (uint32_t s = W > H ? W : H; s > 1; s >>= 1) mi.mipLevels++;
        VkImage mimg; VkResult r = DP(vkCreateImage)(dev, &mi, NULL, &mimg);
        check(r == VK_SUCCESS, "mipmapped sparse image %ux%u, %u levels -> %d", W, H, mi.mipLevels, r);
        if (r) goto out;
        VkSparseImageMemoryRequirements ms; uint32_t one = 1; DP(vkGetImageSparseMemoryRequirements)(dev, mimg, &one, &ms);
        printf("  mip tail: first lod %u, size %llu, offset %llu, stride %llu, flags 0x%x\n", ms.imageMipTailFirstLod, (unsigned long long)ms.imageMipTailSize,
               (unsigned long long)ms.imageMipTailOffset, (unsigned long long)ms.imageMipTailStride, ms.formatProperties.flags);
        VkMemoryRequirements mmr; DP(vkGetImageMemoryRequirements)(dev, mimg, &mmr);
        VkMemoryAllocateInfo tai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, ms.imageMipTailSize + tile, mt };
        VkDeviceMemory tmem; VK(DP(vkAllocateMemory)(dev, &tai, NULL, &tmem));
        VkSparseMemoryBind ob = { ms.imageMipTailOffset, ms.imageMipTailSize, tmem, 0, 0 };
        VkSparseImageOpaqueMemoryBindInfo obi = { mimg, 1, &ob };
        VkSparseImageMemoryBind l0 = { { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0 }, { 0, 0, 0 }, { GW, GH, 1 }, tmem, ms.imageMipTailSize, 0 };
        VkSparseImageMemoryBindInfo mibi = { mimg, 1, &l0 };
        // level 0 is outside the tail unless the whole image is smaller than a tile
        const int l0ok = ms.imageMipTailFirstLod > 0;
        VkBindSparseInfo mb = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, NULL, 0, NULL, 0, NULL, 1, &obi, l0ok ? 1u : 0u, &mibi, 0, NULL };
        if (bind_wait(&mb, "bind the mip tail (opaque) and level-0 tile (0,0)")) goto out;
        const uint32_t last = mi.mipLevels - 1;
        uint32_t *u = (uint32_t *)up.map; u[0] = 0x11223344u; u[1] = 0x55667788u;
        VkCommandBuffer cb = cb_begin();
        VkImageMemoryBarrier ib = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_GENERAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, mimg, { VK_IMAGE_ASPECT_COLOR_BIT, 0, mi.mipLevels, 0, 1 } };
        DP(vkCmdPipelineBarrier)(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &ib);
        VkBufferImageCopy c1 = { 0, 1, 1, { VK_IMAGE_ASPECT_COLOR_BIT, last, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
        VkBufferImageCopy c0 = { 4, 1, 1, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 3, 5, 0 }, { 1, 1, 1 } };
        DP(vkCmdCopyBufferToImage)(cb, up.buf, mimg, VK_IMAGE_LAYOUT_GENERAL, 1, &c1);
        DP(vkCmdCopyBufferToImage)(cb, up.buf, mimg, VK_IMAGE_LAYOUT_GENERAL, 1, &c0);
        barrier_all(cb);
        VkBufferImageCopy d1 = { 0, 1, 1, { VK_IMAGE_ASPECT_COLOR_BIT, last, 0, 1 }, { 0, 0, 0 }, { 1, 1, 1 } };
        VkBufferImageCopy d0 = { 4, 1, 1, { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 }, { 3, 5, 0 }, { 1, 1, 1 } };
        memset(down.map, 0, 8);
        DP(vkCmdCopyImageToBuffer)(cb, mimg, VK_IMAGE_LAYOUT_GENERAL, down.buf, 1, &d1);
        DP(vkCmdCopyImageToBuffer)(cb, mimg, VK_IMAGE_LAYOUT_GENERAL, down.buf, 1, &d0);
        barrier_all(cb);
        DP(vkEndCommandBuffer)(cb);
        if (submit_wait(cb, "mip tail round trip")) goto out;
        const uint32_t *d = (const uint32_t *)down.map;
        check(d[0] == 0x11223344u, "mip tail: level %u (1x1) reads back 0x%08x (want 0x11223344)", last, d[0]);
        if (l0ok) check(d[1] == 0x55667788u, "mip tail image: level 0 texel (3,5) in the bound tile reads back 0x%08x (want 0x55667788)", d[1]);
        DP(vkDestroyImage)(dev, mimg, NULL);
        DP(vkFreeMemory)(dev, tmem, NULL);
    }
out:
    DP(vkDeviceWaitIdle)(dev);
    printf("vksparse: %d PASS, %d FAIL\n", npass, nfail);
    return nfail ? 1 : 0;
}
