/* nvmtl_sparse_vk.c - the Vulkan half of Metal sparse textures (nvmtl_sparse.h; NVMTL_SPARSE_TEXTURES=1).
   Included at the end of nvmtl_vk.c: it uses that file's device, queue, submit mutex and helpers.

   Ordering. vkQueueBindSparse is a queue operation, but Vulkan orders it against the queue's other submissions only
   through semaphores (spec "Sparse Memory Binding": binds are not implicitly ordered with command buffers). A Metal
   resource state encoder's mappings take effect between the encoders before it and the ones after it, in queue order.
   So a batch of binds runs, with the submit mutex held, as
     1. a vkQueueSubmit with no command buffers that signals point v1 of g_sp.sem: the first synchronization scope of a
        semaphore signal in vkQueueSubmit is every command submitted earlier in submission order, so v1 is "everything
        before the binds has finished";
     2. vkQueueBindSparse waiting for v1 and signalling v2;
     3. the submission the batch rides (the part after the encoder) waiting for v2; and every later submission waits
        for v2 too until the semaphore is seen at v2 (one vkGetSemaphoreCounterValue per submit, only while a bind may
        be outstanding).
   On ANV with IntelXe.kext the bind is ANV's VM_BIND path (anv_sparse_bind_vm_bind): the kernel waits for the bind's
   in-fences on the CPU and writes the page tables before the ioctl returns, so step 2 holds the submitting thread (and
   the submit mutex) until the GPU has drained the work before the binds. That is the price of tier 1 here; a kernel
   with asynchronous binds would make it a GPU-side wait without a change in this file. */
#include "nvmtl_sparse.h"

static struct {
    int on;                       /* nvmtl_sparse_tex_enable said yes */
    int ready;                    /* the semaphore and the entry points below are there */
    int img3d;                    /* sparseResidencyImage3D enabled */
    int aliased;                  /* sparseResidencyAliased enabled: images are made SPARSE_ALIASED */
    int buf;                      /* sparseResidencyBuffer enabled: placement sparse buffers */
    int buf_ready;                /* ... and the probe buffer gave a usable block size and memory types */
    uint32_t buf_align, buf_bits; /* a sparse buffer's block size and the memory types it binds */
    VkSemaphore sem;              /* the binds' timeline */
    uint64_t value;               /* last point handed out (submit mutex) */
    uint64_t last;                /* the newest point a bind signals; later submissions wait for it until it is seen */
    uint64_t seen;                /* the newest point seen reached */
    uint32_t tile_bytes, mem_bits;
    PFN_vkGetPhysicalDeviceSparseImageFormatProperties fmtprops;
    PFN_vkGetImageSparseMemoryRequirements reqs;
    PFN_vkGetSemaphoreCounterValue counter;
    PFN_vkWaitSemaphores wait;
    _Atomic unsigned long long batches, binds, unbinds, opaque, fails, waited_us, max_us;
} g_sp;

static int nvmtl_sparse_ready(void);
int nvmtl_sparse_knob(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SPARSE_TEXTURES"); on = e && e[0] == '1'; }
    return on;
}
int nvmtl_vk_sparse_tex(void) { return nvmtl_sparse_knob() && nvmtl_vk_init() == 0 && nvmtl_sparse_ready(); }

int nvmtl_sparse_tex_enable(const void *supported_, void *enabled_, int queue_sparse, int timeline)
{
    if (!nvmtl_sparse_knob()) return 0;
    const VkPhysicalDeviceFeatures *s = (const VkPhysicalDeviceFeatures *)supported_;
    VkPhysicalDeviceFeatures *e = (VkPhysicalDeviceFeatures *)enabled_;
    const int ok = s->sparseBinding && s->sparseResidencyImage2D && s->shaderResourceResidency && queue_sparse && timeline;
    nvlog("sparse textures (NVMTL_SPARSE_TEXTURES=1): %s - sparseBinding %u sparseResidencyImage2D %u sparseResidencyImage3D %u "
          "sparseResidencyAliased %u shaderResourceResidency %u, queue binds sparse %s, timeline semaphores %s", ok ? "ON" : "OFF (the device lacks a piece)",
          s->sparseBinding, s->sparseResidencyImage2D, s->sparseResidencyImage3D, s->sparseResidencyAliased, s->shaderResourceResidency,
          queue_sparse ? "yes" : "NO", timeline ? "yes" : "NO");
    if (!ok) return 0;
    e->sparseBinding = VK_TRUE; e->sparseResidencyImage2D = VK_TRUE; e->shaderResourceResidency = VK_TRUE;
    e->sparseResidencyImage3D = s->sparseResidencyImage3D;
    /* Metal 4 lets placement sparse textures share tiles (copyTextureMappingsFromTexture, a placement heap's other
       resources); Vulkan allows one memory range bound to several sparse resources only with SPARSE_ALIASED */
    e->sparseResidencyAliased = s->sparseResidencyAliased;
    /* Metal 4 placement sparse buffers (MTLBufferSparseTier1): VkBuffers with SPARSE_RESIDENCY, tiles bound from
       placement heaps; unbound ranges read zero and drop writes (residencyNonResidentStrict, NULL PTEs on Xe) */
    e->sparseResidencyBuffer = s->sparseResidencyBuffer;
    /* tier 2 (NVMTL_SPARSE_TIER2=1): the counted reads of fragment shaders are atomics */
    if (nvmtl_sparse_tier2_knob()) e->fragmentStoresAndAtomics = s->fragmentStoresAndAtomics;
    g_sp.on = 1; g_sp.img3d = s->sparseResidencyImage3D == VK_TRUE; g_sp.aliased = s->sparseResidencyAliased == VK_TRUE;
    g_sp.buf = s->sparseResidencyBuffer == VK_TRUE;
    nvlog("sparse buffers (NVMTL_SPARSE_TEXTURES=1): sparseResidencyBuffer %u - placement sparse buffers %s", s->sparseResidencyBuffer,
          g_sp.buf ? "possible" : "OFF (sparseBufferTier None)");
    return 1;
}

/* After vkCreateDevice: the entry points, the timeline, and the tile size and memory types of a probe image. Once. */
static void nvmtl_sparse_init_once(void)
{
    g_sp.fmtprops = (PFN_vkGetPhysicalDeviceSparseImageFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceSparseImageFormatProperties");
    g_sp.reqs = (PFN_vkGetImageSparseMemoryRequirements)pvkGetDeviceProcAddr(g_dev, "vkGetImageSparseMemoryRequirements");
    g_sp.counter = (PFN_vkGetSemaphoreCounterValue)pvkGetDeviceProcAddr(g_dev, "vkGetSemaphoreCounterValue");
    g_sp.wait = (PFN_vkWaitSemaphores)pvkGetDeviceProcAddr(g_dev, "vkWaitSemaphores");
    PFN_vkCreateSemaphore cs = (PFN_vkCreateSemaphore)pvkGetDeviceProcAddr(g_dev, "vkCreateSemaphore");
    VkSemaphoreTypeCreateInfo ti = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
    VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &ti, 0 };
    if (!g_sp.fmtprops || !g_sp.reqs || !g_sp.counter || !g_sp.wait || !cs || cs(g_dev, &sci, NULL, &g_sp.sem) != VK_SUCCESS) {
        nvlog("sparse textures: OFF - entry points or the bind timeline missing"); return; }
    /* the probe: a 2D RGBA8 sparse image; its alignment is the sparse block size (Vulkan: a sparse resource's
       VkMemoryRequirements.alignment is its block size), its memory types the ones tiles may come from */
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL, VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT,
        VK_IMAGE_TYPE_2D, VK_FORMAT_R8G8B8A8_UNORM, { 512, 512, 1 }, 1, 1, VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL, VK_IMAGE_LAYOUT_UNDEFINED };
    VkImage probe = VK_NULL_HANDLE;
    if (pvkCreateImage(g_dev, &ici, NULL, &probe) != VK_SUCCESS) { nvlog("sparse textures: OFF - a sparse RGBA8 probe image was refused"); return; }
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, probe, &mr);
    pvkDestroyImage(g_dev, probe, NULL);
    if (!mr.alignment || mr.alignment > (1u << 24) || !mr.memoryTypeBits) {
        nvlog("sparse textures: OFF - probe alignment %llu, types 0x%x", (unsigned long long)mr.alignment, mr.memoryTypeBits); return; }
    g_sp.tile_bytes = (uint32_t)mr.alignment; g_sp.mem_bits = mr.memoryTypeBits;
    g_sp.ready = 1;
    if (g_sp.buf) {
        /* the buffer probe: one 64 KiB residency buffer; its alignment is the block size buffer binds must be made of */
        VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT,
            65536, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
        VkBuffer pb = VK_NULL_HANDLE;
        if (pvkCreateBuffer(g_dev, &bci, NULL, &pb) == VK_SUCCESS) {
            VkMemoryRequirements br; pvkGetBufferMemoryRequirements(g_dev, pb, &br);
            pvkDestroyBuffer(g_dev, pb, NULL);
            if (br.alignment && br.alignment <= 65536 && !(65536 % br.alignment) && br.memoryTypeBits) {
                g_sp.buf_align = (uint32_t)br.alignment; g_sp.buf_bits = br.memoryTypeBits; g_sp.buf_ready = 1; }
            nvlog("sparse buffers: %s - block %llu bytes, memory types 0x%x", g_sp.buf_ready ? "ready" : "OFF (unusable probe)",
                  (unsigned long long)br.alignment, br.memoryTypeBits);
        } else nvlog("sparse buffers: OFF - a sparse residency probe buffer was refused");
    }
    nvlog("sparse textures: ready - tiles of %u bytes, memory types 0x%x, 3D %s; mappings are vkQueueBindSparse between a signalling "
          "submission and the submission after the resource state encoder", g_sp.tile_bytes, g_sp.mem_bits, g_sp.img3d ? "yes" : "no");
}
static int nvmtl_sparse_ready(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (!g_sp.on || g_state != 1) return 0;
    pthread_once(&once, nvmtl_sparse_init_once);
    return g_sp.ready;
}

uint32_t nvmtl_vk_sparse_tile_bytes(void) { return nvmtl_vk_sparse_tex() ? g_sp.tile_bytes : 0; }
int nvmtl_vk_sparse_buf(void) { return nvmtl_vk_sparse_tex() && g_sp.buf_ready; }

/* ---- placement sparse buffers -------------------------------------------------------------------------------------- */
int nvmtl_vk_buffer_create_sparse_residency(size_t size, uint32_t page_bytes, void *out_, uint32_t *mem_bits)
{
    nvk_buffer *out = (nvk_buffer *)out_;
    memset(out, 0, sizeof *out); *mem_bits = 0;
    if (!nvmtl_vk_sparse_buf() || !size || !page_bytes || page_bytes % g_sp.buf_align) return -1;
    const size_t vsize = (size + page_bytes - 1) / page_bytes * page_bytes;
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL,
        VK_BUFFER_CREATE_SPARSE_BINDING_BIT | VK_BUFFER_CREATE_SPARSE_RESIDENCY_BIT | (g_sp.aliased ? VK_BUFFER_CREATE_SPARSE_ALIASED_BIT : 0),
        vsize, NVMTL_BUFFER_USAGE, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer b = VK_NULL_HANDLE;
    const VkResult r = pvkCreateBuffer(g_dev, &bci, NULL, &b);
    if (r != VK_SUCCESS) { nvlog("sparse buffer: vkCreateBuffer(%zu B, residency) -> %d", vsize, r); return -1; }
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, b, &mr);
    if (!mr.alignment || page_bytes % mr.alignment) {
        nvlog("sparse buffer: block %llu bytes does not divide the %u-byte page - refused", (unsigned long long)mr.alignment, page_bytes);
        pvkDestroyBuffer(g_dev, b, NULL); return -1; }
    out->buf = b; out->size = size; out->vksize = vsize != size ? vsize : 0;
    out->placed = 1;   /* no memory of its own: tiles come from placement heaps; the free path leaves memory alone */
    *mem_bits = mr.memoryTypeBits;
    g_buf_made++;
    return 0;
}
/* At once, not through the grace deferral: destroying a sparse buffer unbinds its whole range (anv_free_sparse_bindings),
   so a placement heap freed right after it cannot stay mapped. */
void nvmtl_vk_sparse_buffer_destroy(void *b_)
{
    nvk_buffer *b = (nvk_buffer *)b_;
    if (!b || !b->buf) return;
    if (g_state == 1) { pvkDestroyBuffer(g_dev, (VkBuffer)b->buf, NULL); g_buf_gone++; }
    memset(b, 0, sizeof *b);
}

static VkImageUsageFlags nvmtl_sparse_usage(uint32_t vkfmt, int storage, int compressed)
{
    return (compressed ? 0 : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT
         | VK_IMAGE_USAGE_SAMPLED_BIT | (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
}

int nvmtl_vk_sparse_tile_shape(uint32_t vkfmt, int is3d, uint32_t samples, uint32_t *tw, uint32_t *th, uint32_t *td)
{
    *tw = *th = *td = 0;
    if (!nvmtl_vk_sparse_tex()) return 0;
    uint32_t bw = 1, bh = 1, bb = 0;
    const int compressed = nvmtl_vk_format_block(vkfmt, &bw, &bh, &bb);
    VkSparseImageFormatProperties fp[4]; uint32_t n = 4;
    g_sp.fmtprops(g_pdev, (VkFormat)vkfmt, is3d ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, (VkSampleCountFlagBits)(samples ? samples : 1),
                  nvmtl_sparse_usage(vkfmt, 0, compressed), VK_IMAGE_TILING_OPTIMAL, &n, fp);
    for (uint32_t i = 0; i < n; i++)
        if (fp[i].aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            *tw = fp[i].imageGranularity.width; *th = fp[i].imageGranularity.height; *td = fp[i].imageGranularity.depth;
            return *tw && *th && *td;
        }
    return 0;
}

int nvmtl_vk_image_create_sparse(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage,
                                 uint32_t mtl_type, uint32_t layers, void *out_, nvmtl_sparse_layout *lay)
{
    nvk_image *out = (nvk_image *)out_;
    memset(out, 0, sizeof *out); memset(lay, 0, sizeof *lay);
    if (!nvmtl_vk_sparse_tex()) return -1;
    /* Vulkan has no sparse 1D image (VUID-VkImageCreateInfo-imageType-00970) and ANV refuses sparse linear and
       multisampled-depth images; Metal's tier 1 is 2D, 2D array, cube (array) and 3D colour textures */
    if (!layers || mtl_type < 2 || mtl_type == 4 || mtl_type > 7 || ((mtl_type == 2) && layers != 1) || (mtl_type == 5 && layers != 6)
        || (mtl_type == 6 && layers % 6)) { nvlog("sparse texture: textureType %u with %u layer(s) cannot be sparse here", mtl_type, layers); return -1; }
    if (mtl_type == 7 && !g_sp.img3d) { nvlog("sparse texture: 3D refused - the device has no sparseResidencyImage3D"); return -1; }
    if (!mips) mips = 1;
    storage = storage && !a8 && nvmtl_vk_format_is_storage(vkfmt);
    out->w = w; out->h = h; out->mips = mips; out->fmt = vkfmt; out->bpp = bpp ? bpp : 4;
    out->storage = storage ? 1 : 0; out->mtl_type = mtl_type; out->layers = layers;
    uint32_t bb = 0; const int compressed = nvmtl_vk_format_block(vkfmt, &out->bw, &out->bh, &bb);
    if (compressed) { out->bpp = bb; out->storage = 0; storage = 0; }
    VkImageCreateInfo ici = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, NULL,
        VK_IMAGE_CREATE_SPARSE_BINDING_BIT | VK_IMAGE_CREATE_SPARSE_RESIDENCY_BIT | (g_sp.aliased ? VK_IMAGE_CREATE_SPARSE_ALIASED_BIT : 0)
            | ((mtl_type == 5 || mtl_type == 6) ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0),
        mtl_type == 7 ? VK_IMAGE_TYPE_3D : VK_IMAGE_TYPE_2D, (VkFormat)vkfmt, { w, h, mtl_type == 7 ? layers : 1 }, mips, mtl_type == 7 ? 1 : layers,
        VK_SAMPLE_COUNT_1_BIT, VK_IMAGE_TILING_OPTIMAL, nvmtl_sparse_usage(vkfmt, storage, compressed), VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        VK_IMAGE_LAYOUT_UNDEFINED };
    nvmtl_vk_image_mutable(&ici, out);
    /* VUID-VkImageCreateInfo-flags-09403: no 2D-array view of a sparse 3D image (its slices cannot be render targets) */
    ici.flags &= ~(VkImageCreateFlags)VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT;
    out->vkflags = (uint32_t)ici.flags;
    { VkSparseImageFormatProperties fp; uint32_t n = 1;
      g_sp.fmtprops(g_pdev, ici.format, ici.imageType, ici.samples, ici.usage, ici.tiling, &n, &fp);
      if (!n) { nvlog("sparse texture: format %u (%s) has no sparse image format properties - refused", vkfmt, mtl_type == 7 ? "3D" : "2D"); return -1; } }
    VkResult r = nvmtl_opaque_CreateImage(g_dev, &ici, NULL, &out->img);
    if (r != VK_SUCCESS) { nvlog("sparse texture: vkCreateImage(%ux%u fmt %u type %u, %u mips, %u layers) -> %d", w, h, vkfmt, mtl_type, mips, layers, r);
                           out->img = NULL; return -1; }
    VkMemoryRequirements mr; pvkGetImageMemoryRequirements(g_dev, (VkImage)out->img, &mr);
    VkSparseImageMemoryRequirements sr[2]; uint32_t ns = 2;
    g_sp.reqs(g_dev, (VkImage)out->img, &ns, sr);
    int found = 0;
    for (uint32_t i = 0; i < ns; i++) {
        if (!(sr[i].formatProperties.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT)) continue;
        lay->tw = sr[i].formatProperties.imageGranularity.width; lay->th = sr[i].formatProperties.imageGranularity.height;
        lay->td = sr[i].formatProperties.imageGranularity.depth;
        lay->tail_first = sr[i].imageMipTailFirstLod < mips ? sr[i].imageMipTailFirstLod : mips;
        lay->single_tail = (sr[i].formatProperties.flags & VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) != 0;
        lay->tail_size = sr[i].imageMipTailSize; lay->tail_offset = sr[i].imageMipTailOffset; lay->tail_stride = sr[i].imageMipTailStride;
        found = 1;
    }
    lay->tile_bytes = (uint32_t)mr.alignment; lay->mem_bits = mr.memoryTypeBits;
    if (!found || !lay->tw || !lay->th || !lay->td || mr.alignment != g_sp.tile_bytes
        || (lay->tail_first < mips && (!lay->tail_size || lay->tail_size % mr.alignment))) {
        nvlog("sparse texture: unexpected sparse requirements (found %d, tile %ux%ux%u, alignment %llu, tail size %llu) - refused",
              found, lay->tw, lay->th, lay->td, (unsigned long long)mr.alignment, (unsigned long long)lay->tail_size);
        pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1;
    }
    const VkImageViewType vt = mtl_type == 3 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : mtl_type == 5 ? VK_IMAGE_VIEW_TYPE_CUBE
                             : mtl_type == 6 ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : mtl_type == 7 ? VK_IMAGE_VIEW_TYPE_3D : VK_IMAGE_VIEW_TYPE_2D;
    VkImageViewCreateInfo vci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, NULL, 0, (VkImage)out->img, vt, (VkFormat)vkfmt,
        nvmtl_a8_mapping(a8), { VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, mtl_type == 7 ? 1 : layers } };
    r = nvmtl_opaque_CreateImageView(g_dev, &vci, NULL, &out->view);
    if (r != VK_SUCCESS) { nvlog("sparse texture: vkCreateImageView -> %d", r); pvkDestroyImage(g_dev, (VkImage)out->img, NULL); memset(out, 0, sizeof *out); return -1; }
    out->placed = 1;   /* no memory of its own: the residency mover, the reback and the free path leave it alone */
    g_img_made++;
    { static _Atomic unsigned said; if (atomic_fetch_add(&said, 1) < 16)
        nvlog("sparse texture: %ux%u fmt %u type %u, %u mips (tail from %u, %llu B%s), %u layers: tiles %ux%ux%u of %u B",
              w, h, vkfmt, mtl_type, mips, lay->tail_first, (unsigned long long)lay->tail_size,
              lay->single_tail ? ", one for all layers" : "", layers, lay->tw, lay->th, lay->td, lay->tile_bytes); }
    return 0;
}

int nvmtl_vk_sparse_memtype_ok(const nvmtl_sparse_layout *lay, uint32_t memtype)
{
    return lay && memtype < 32 && (lay->mem_bits & (1u << memtype));
}

int nvmtl_vk_sparse_pool_create(size_t bytes, void **mem_out)
{
    *mem_out = NULL;
    if (!nvmtl_vk_sparse_tex() || !bytes || bytes % g_sp.tile_bytes) return -1;
    const int mt = memtype(g_sp.mem_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mt < 0) { nvlog("sparse heap: no device-local memory type among the sparse types 0x%x", g_sp.mem_bits); return -1; }
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, bytes, (uint32_t)mt };
    VkDeviceMemory m = VK_NULL_HANDLE;
    const VkResult r = pvkAllocateMemory(g_dev, &mai, NULL, &m);
    if (r != VK_SUCCESS) { nvlog("sparse heap: vkAllocateMemory(%zu B, type %d) -> %d", bytes, mt, r); return -1; }
    nvmtl_vk_budget_charge(bytes);
    *mem_out = m;
    return 0;
}
void nvmtl_vk_sparse_pool_free(void *mem, size_t bytes)
{
    (void)bytes;
    if (mem && g_state == 1) { pvkFreeMemory(g_dev, (VkDeviceMemory)mem, NULL); g_budget_stale = 1; }
}

/* ---- batches ---------------------------------------------------------------------------------------------------------- */
typedef struct { VkImage img; VkSparseImageMemoryBind b; } nvmtl_sparse_tile_op;
typedef struct { VkImage img; VkSparseMemoryBind b; } nvmtl_sparse_opaque_op;
typedef struct { VkBuffer buf; VkSparseMemoryBind b; } nvmtl_sparse_buffer_op;
typedef struct { void (*run)(nvmtl_sparse_batch *, void *); void *ctx; void (*drop)(void *); } nvmtl_sparse_deferred;
struct nvmtl_sparse_batch {
    nvmtl_sparse_tile_op *t; uint32_t nt, ct;
    nvmtl_sparse_opaque_op *o; uint32_t no, co;
    nvmtl_sparse_buffer_op *bo; uint32_t nbo, cbo;
    nvmtl_sparse_deferred *d; uint32_t nd, cd;
};
nvmtl_sparse_batch *nvmtl_sparse_batch_of(void *c_)
{
    nvk_cmdbuf *c = (nvk_cmdbuf *)c_;
    if (!c) return NULL;
    if (!c->sparse) c->sparse = calloc(1, sizeof(nvmtl_sparse_batch));
    return (nvmtl_sparse_batch *)c->sparse;
}
#define NVMTL_SP_GROW(arr, n, cap) ((n) < (cap) || ((arr) = reallocf((arr), sizeof *(arr) * ((cap) = (cap) ? (cap) * 2 : 16))) != NULL)
int nvmtl_sparse_batch_tile(nvmtl_sparse_batch *b, void *img, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, uint32_t z,
                            uint32_t w, uint32_t h, uint32_t d, void *mem, uint64_t mem_off)
{
    if (!b || !img) return -1;
    if (!NVMTL_SP_GROW(b->t, b->nt, b->ct)) { b->nt = b->ct = 0; return -1; }
    b->t[b->nt++] = (nvmtl_sparse_tile_op){ (VkImage)img, { { VK_IMAGE_ASPECT_COLOR_BIT, mip, layer }, { (int32_t)x, (int32_t)y, (int32_t)z },
                                                           { w, h, d }, (VkDeviceMemory)mem, mem ? mem_off : 0, 0 } };
    return 0;
}
int nvmtl_sparse_batch_opaque(nvmtl_sparse_batch *b, void *img, uint64_t res_off, uint64_t size, void *mem, uint64_t mem_off)
{
    if (!b || !img) return -1;
    if (!NVMTL_SP_GROW(b->o, b->no, b->co)) { b->no = b->co = 0; return -1; }
    b->o[b->no++] = (nvmtl_sparse_opaque_op){ (VkImage)img, { res_off, size, (VkDeviceMemory)mem, mem ? mem_off : 0, 0 } };
    return 0;
}
int nvmtl_sparse_batch_buffer(nvmtl_sparse_batch *b, void *buf, uint64_t res_off, uint64_t size, void *mem, uint64_t mem_off)
{
    if (!b || !buf || !size) return -1;
    if (b->nbo) {   /* the continuation of the last bind: one range */
        nvmtl_sparse_buffer_op *l = &b->bo[b->nbo - 1];
        if (l->buf == (VkBuffer)buf && l->b.memory == (VkDeviceMemory)mem && l->b.resourceOffset + l->b.size == res_off
            && (!mem || l->b.memoryOffset + l->b.size == mem_off)) { l->b.size += size; return 0; }
    }
    if (!NVMTL_SP_GROW(b->bo, b->nbo, b->cbo)) { b->nbo = b->cbo = 0; return -1; }
    b->bo[b->nbo++] = (nvmtl_sparse_buffer_op){ (VkBuffer)buf, { res_off, size, (VkDeviceMemory)mem, mem ? mem_off : 0, 0 } };
    return 0;
}
int nvmtl_sparse_batch_defer(nvmtl_sparse_batch *b, void (*run)(nvmtl_sparse_batch *, void *), void *ctx, void (*drop)(void *))
{
    if (!b || !run) { if (drop) drop(ctx); return -1; }
    if (!NVMTL_SP_GROW(b->d, b->nd, b->cd)) { b->nd = b->cd = 0; if (drop) drop(ctx); return -1; }
    b->d[b->nd++] = (nvmtl_sparse_deferred){ run, ctx, drop };
    return 0;
}
void nvmtl_sparse_batch_free(nvmtl_sparse_batch *b)
{
    if (!b) return;
    for (uint32_t i = 0; i < b->nd; i++) if (b->d[i].drop) b->d[i].drop(b->d[i].ctx);
    free(b->t); free(b->o); free(b->bo); free(b->d); free(b);
}

/* The binds of b, ordered after every earlier submission (see the top of this file). Submit mutex held. */
static int nvmtl_sparse_queue(nvmtl_sparse_batch *b)
{
    if (!b->nt && !b->no && !b->nbo && !b->nd) return 0;
    const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    const uint64_t v1 = g_sp.value + 1;
    VkTimelineSemaphoreSubmitInfo t1 = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 0, NULL, 1, &v1 };
    VkSubmitInfo s1 = { VK_STRUCTURE_TYPE_SUBMIT_INFO, &t1, 0, NULL, NULL, 0, NULL, 1, &g_sp.sem };
    VkResult r = pvkQueueSubmit(g_queue, 1, &s1, VK_NULL_HANDLE);
    if (r != VK_SUCCESS) { atomic_fetch_add(&g_sp.fails, 1); nvlog("sparse: the ordering submission -> %d - %u tile and %u tail binds NOT made", r, b->nt, b->no); return -1; }
    g_sp.value = v1; g_sp.last = v1;
    if (b->nd) {
        /* binds whose arguments the GPU writes: read once everything before them has finished */
        VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &g_sp.sem, &v1 };
        r = g_sp.wait(g_dev, &wi, 10ull * 1000 * 1000 * 1000);
        if (r != VK_SUCCESS) nvlog("sparse: waiting for the work before indirect mappings -> %d - they read their arguments anyway", r);
        for (uint32_t i = 0; i < b->nd; i++) { b->d[i].run(b, b->d[i].ctx); if (b->d[i].drop) b->d[i].drop(b->d[i].ctx); }
        b->nd = 0;
    }
    if (!b->nt && !b->no && !b->nbo) return 0;
    VkSparseImageMemoryBindInfo *ib = calloc(b->nt ? b->nt : 1, sizeof *ib);
    VkSparseImageOpaqueMemoryBindInfo *ob = calloc(b->no ? b->no : 1, sizeof *ob);
    VkSparseBufferMemoryBindInfo *bb = calloc(b->nbo ? b->nbo : 1, sizeof *bb);
    if (!ib || !ob || !bb) { free(ib); free(ob); free(bb); atomic_fetch_add(&g_sp.fails, 1); nvlog("sparse: out of memory for %u binds", b->nt + b->no + b->nbo); return -1; }
    unsigned long long nb = 0, nu = 0;
    for (uint32_t i = 0; i < b->nt; i++) { ib[i] = (VkSparseImageMemoryBindInfo){ b->t[i].img, 1, &b->t[i].b }; if (b->t[i].b.memory) nb++; else nu++; }
    for (uint32_t i = 0; i < b->no; i++) { ob[i] = (VkSparseImageOpaqueMemoryBindInfo){ b->o[i].img, 1, &b->o[i].b }; if (b->o[i].b.memory) nb++; else nu++; }
    for (uint32_t i = 0; i < b->nbo; i++) { bb[i] = (VkSparseBufferMemoryBindInfo){ b->bo[i].buf, 1, &b->bo[i].b }; if (b->bo[i].b.memory) nb++; else nu++; }
    const uint64_t v2 = v1 + 1;
    VkTimelineSemaphoreSubmitInfo t2 = { VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO, NULL, 1, &v1, 1, &v2 };
    VkBindSparseInfo bi = { VK_STRUCTURE_TYPE_BIND_SPARSE_INFO, &t2, 1, &g_sp.sem, b->nbo, bb, b->no, ob, b->nt, ib, 1, &g_sp.sem };
    r = g_state == 1 ? pvkQueueBindSparse(g_queue, 1, &bi, VK_NULL_HANDLE) : VK_ERROR_DEVICE_LOST;
    free(ib); free(ob); free(bb);
    const uint64_t us = (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1000;
    if (r != VK_SUCCESS) {
        atomic_fetch_add(&g_sp.fails, 1);
        nvlog("sparse: vkQueueBindSparse(%llu maps, %llu unmaps) -> %d - the mappings did NOT change; the texture reads as before", nb, nu, r);
        return -1;
    }
    g_sp.value = v2; g_sp.last = v2;
    const unsigned long long k = atomic_fetch_add(&g_sp.batches, 1) + 1;
    atomic_fetch_add(&g_sp.binds, nb); atomic_fetch_add(&g_sp.unbinds, nu); atomic_fetch_add(&g_sp.opaque, b->no);
    atomic_fetch_add(&g_sp.waited_us, us);
    if (us > atomic_load(&g_sp.max_us)) atomic_store(&g_sp.max_us, us);
    if (k <= 4 || !(k & (k - 1)))
        nvlog("sparse: %llu batches so far (this one %llu maps, %llu unmaps, %u of them tails, %u buffer ranges, %llu us with the queue held); "
              "totals %llu maps, %llu unmaps, %llu us, longest %llu us, %llu failures", k, nb, nu, b->no, b->nbo, us,
              atomic_load(&g_sp.binds), atomic_load(&g_sp.unbinds), atomic_load(&g_sp.waited_us), atomic_load(&g_sp.max_us), atomic_load(&g_sp.fails));
    return 0;
}

uint64_t nvmtl_sparse_before_submit(void *c_, void **sem)
{
    *sem = NULL;
    if (!g_sp.ready) return 0;
    nvk_cmdbuf *c = (nvk_cmdbuf *)c_;
    if (c && c->sparse) {
        nvmtl_sparse_batch *b = (nvmtl_sparse_batch *)c->sparse; c->sparse = NULL;
        (void)nvmtl_sparse_queue(b);
        nvmtl_sparse_batch_free(b);
    }
    if (g_sp.last > g_sp.seen) {
        uint64_t v = 0;
        if (g_sp.counter(g_dev, g_sp.sem, &v) == VK_SUCCESS && v > g_sp.seen) g_sp.seen = v;
    }
    if (g_sp.last <= g_sp.seen) return 0;
    *sem = (void *)g_sp.sem;
    return g_sp.last;
}

/* ---- tier 2: per-tile access counters (NVMTL_SPARSE_TIER2=1; nvmtl_sparse.h, DESIGN-sparse-textures.md "Tier 2") ------
   Made when the bindless heap is made (nvmtl_sparse_counters_heap_ready, heap lock held by its maker): the directory, a
   host-visible buffer of one RGBA32UI element per heap slot that the CPU writes as slots take and give up a texture,
   and the counters, a device-local R32UI buffer the shaders add to. Both are texel buffers of heap slot NVMTL_SC_SLOT,
   which the heap then never hands out (nvmtl_heap_slot_end). A texture's counters are a range of the counter buffer,
   first fit, zeroed on the GPU when it is taken. */
#define NVMTL_SC_WORDS (4u << 20)   /* 16 MiB of counters: a 16384x16384 RGBA8 texture with its mips takes ~22000 */
typedef struct { const void *view[2]; uint32_t rec[4]; uint32_t base, len; uint32_t *slots; uint32_t nslots, cslots; } nvmtl_sc_tex;
static struct {
    int tried, ready;
    VkBuffer dirbuf, cntbuf; VkDeviceMemory dirmem, cntmem; VkBufferView dirview, cntview;
    uint32_t *dir;                       /* the directory, mapped (4 words per heap slot) */
    pthread_mutex_t lk;                  /* the textures, the free list, directory writes */
    nvmtl_sc_tex *tex; uint32_t ntex, ctex;
    uint32_t *fo, *fl, nf, cf;           /* free ranges of the counter buffer (word offset, length), by offset */
    PFN_vkCmdUpdateBuffer update;
    unsigned long long made, refused, gets, resets;
} g_spc = { .lk = PTHREAD_MUTEX_INITIALIZER };

int nvmtl_sparse_tier2_knob(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_SPARSE_TIER2"); on = e && e[0] == '1' && nvmtl_sparse_knob(); }
    return on;
}
static void nvmtl_sparse_counters_heap_ready(void);
int nvmtl_vk_sparse_tier2(void)
{
    if (!nvmtl_sparse_tier2_knob() || !nvmtl_vk_sparse_tex()) return 0;
    if (!g_spc.tried) {   /* the counters are made with the heap; a process with no heap yet makes it now */
        (void)ensure_descriptors();
        pthread_mutex_lock(&g_desc_lock); nvmtl_sparse_counters_heap_ready(); pthread_mutex_unlock(&g_desc_lock);
    }
    return g_spc.ready;
}
/* the heap slots handed out are [1, end): the last one is the counters' while they are on */
static uint32_t nvmtl_heap_slot_end(void) { return g_spc.ready ? NVMTL_SC_SLOT : NVMTL_BINDLESS_SLOTS; }

static int nvmtl_sc_buffer(VkDeviceSize size, int host, VkFormat fmt, VkBuffer *buf, VkDeviceMemory *mem, VkBufferView *view, void **map)
{
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, NULL, 0, size,
        VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT
            | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (pvkCreateBuffer(g_dev, &bci, NULL, buf) != VK_SUCCESS) return -1;
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, *buf, &mr);
    const int mt = host ? memtype(mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
                        : nvmtl_vk_buffer_vram_type(mr.memoryTypeBits);
    VkMemoryAllocateFlagsInfo af = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &af, mr.size, (uint32_t)mt };
    if (mt < 0 || pvkAllocateMemory(g_dev, &mai, NULL, mem) != VK_SUCCESS) { *mem = VK_NULL_HANDLE; return -1; }
    if (pvkBindBufferMemory(g_dev, *buf, *mem, 0) != VK_SUCCESS) return -1;
    if (host && pvkMapMemory(g_dev, *mem, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) return -1;
    VkBufferViewCreateInfo vci = { VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, NULL, 0, *buf, fmt, 0, size };
    if (pvkCreateBufferView(g_dev, &vci, NULL, view) != VK_SUCCESS) return -1;
    if (g_dbuf) {   /* the buffer model makes texel descriptors from the address, range and format */
        VkBufferDeviceAddressInfo ai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, *buf };
        nvmtl_db_texel_note(*view, pvkGetBufferDeviceAddress(g_dev, &ai), size, (uint32_t)fmt);
    }
    if (!host) g_alloc_bytes += mr.size;
    return 0;
}
static int nvmtl_sc_format_ok(VkFormat f, VkFormatFeatureFlags want)
{
    static PFN_vkGetPhysicalDeviceFormatProperties gfp;
    if (!gfp) gfp = (PFN_vkGetPhysicalDeviceFormatProperties)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFormatProperties");
    VkFormatProperties fp = { 0 }; if (gfp) gfp(g_pdev, f, &fp);
    return (fp.bufferFeatures & want) == want;
}
extern void nvmtl_translate_counters_off(const char *why);
/* the heap was just made (classic or descriptor-buffer model; nothing else uses it yet): the counters and their slot */
static void nvmtl_sparse_counters_heap_ready(void)
{
    if (g_spc.tried || !nvmtl_sparse_tier2_knob()) return;
    g_spc.tried = 1;
    const char *why = NULL;
    void *map = NULL;
    if (!nvmtl_vk_sparse_tex()) why = "sparse textures are off on this device";
    else if (!g_heap_ready || g_heap_nbind < 5 || !g_heap_storage_binding) why = "the heap has no texel-buffer bindings (NVMTL_NO_HEAP_STORAGE / NVMTL_NO_ARGBUF_SAMPLERS, or a refused heap)";
    else if (!pvkCreateBufferView) why = "no vkCreateBufferView";
    else if (!nvmtl_sc_format_ok(VK_FORMAT_R32_UINT, VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_BIT | VK_FORMAT_FEATURE_STORAGE_TEXEL_BUFFER_ATOMIC_BIT)
             || !nvmtl_sc_format_ok(VK_FORMAT_R32G32B32A32_UINT, VK_FORMAT_FEATURE_UNIFORM_TEXEL_BUFFER_BIT)) why = "no R32UI atomic storage / RGBA32UI uniform texel buffers";
    else if (!(g_spc.update = (PFN_vkCmdUpdateBuffer)pvkGetDeviceProcAddr(g_dev, "vkCmdUpdateBuffer"))) why = "no vkCmdUpdateBuffer";
    else if (nvmtl_sc_buffer((VkDeviceSize)NVMTL_BINDLESS_SLOTS * 16, 1, VK_FORMAT_R32G32B32A32_UINT, &g_spc.dirbuf, &g_spc.dirmem, &g_spc.dirview, &map))
        why = "the directory buffer could not be made";
    else if (nvmtl_sc_buffer((VkDeviceSize)NVMTL_SC_WORDS * 4, 0, VK_FORMAT_R32_UINT, &g_spc.cntbuf, &g_spc.cntmem, &g_spc.cntview, NULL))
        why = "the counter buffer could not be made";
    if (!why) {
        g_spc.dir = (uint32_t *)map;
        memset(g_spc.dir, 0, (size_t)NVMTL_BINDLESS_SLOTS * 16);
        g_spc.fo = malloc(sizeof *g_spc.fo * 16); g_spc.fl = malloc(sizeof *g_spc.fl * 16);
        if (!g_spc.fo || !g_spc.fl) why = "out of memory";
        else { g_spc.cf = 16; g_spc.nf = 1; g_spc.fo[0] = NVMTL_SC_HEADER; g_spc.fl[0] = NVMTL_SC_WORDS - NVMTL_SC_HEADER; }   /* word 0 means "none" */
    }
    if (!why) {
        if (g_dbuf) { nvmtl_db_heap_write_texel(3, NVMTL_SC_SLOT, g_spc.dirview); nvmtl_db_heap_write_texel(4, NVMTL_SC_SLOT, g_spc.cntview); }
        else {
            VkBufferView dv = g_spc.dirview, cv = g_spc.cntview;
            VkWriteDescriptorSet w[2] = {
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 3, NVMTL_SC_SLOT, 1, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, NULL, NULL, &dv },
                { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, NULL, g_heapSet, 4, NVMTL_SC_SLOT, 1, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, NULL, NULL, &cv } };
            pvkUpdateDescriptorSets(g_dev, 2, w, 0, NULL);
        }
        g_spc.ready = 1;
        nvlog("sparse tier 2 (NVMTL_SPARSE_TIER2=1): ON - heap slot %u keeps the directory (%u x 16 B, host memory) and %u counters "
              "(%u MiB VRAM); sparse textures answer MTLTextureSparseTier2", NVMTL_SC_SLOT, NVMTL_BINDLESS_SLOTS, NVMTL_SC_WORDS, NVMTL_SC_WORDS >> 18);
        return;
    }
    nvlog("sparse tier 2 (NVMTL_SPARSE_TIER2=1): OFF - %s; sparse textures stay tier 1 and shaders are translated without counters", why);
    if (g_spc.dirview) pvkDestroyBufferView(g_dev, g_spc.dirview, NULL);
    if (g_spc.cntview) pvkDestroyBufferView(g_dev, g_spc.cntview, NULL);
    if (g_spc.dirbuf) pvkDestroyBuffer(g_dev, g_spc.dirbuf, NULL);
    if (g_spc.cntbuf) pvkDestroyBuffer(g_dev, g_spc.cntbuf, NULL);
    if (g_spc.dirmem) pvkFreeMemory(g_dev, g_spc.dirmem, NULL);
    if (g_spc.cntmem) pvkFreeMemory(g_dev, g_spc.cntmem, NULL);
    free(g_spc.fo); free(g_spc.fl);
    g_spc.dirview = g_spc.cntview = VK_NULL_HANDLE; g_spc.dirbuf = g_spc.cntbuf = VK_NULL_HANDLE; g_spc.dirmem = g_spc.cntmem = VK_NULL_HANDLE;
    g_spc.fo = g_spc.fl = NULL; g_spc.dir = NULL;
    nvmtl_translate_counters_off(why);
}

/* a directory element, the header word last: a shader never sees a header with a stale size (lock held) */
static void nvmtl_sc_dir_write(uint32_t slot, const uint32_t rec[4])
{
    if (!g_spc.dir || slot >= NVMTL_SC_SLOT) return;
    uint32_t *e = g_spc.dir + (size_t)slot * 4;
    __atomic_store_n(&e[0], 0, __ATOMIC_RELEASE);
    e[1] = rec[1]; e[2] = rec[2]; e[3] = rec[3];
    __atomic_store_n(&e[0], rec[0], __ATOMIC_RELEASE);
}
static const uint32_t g_spc_zero[4];

/* first fit; lock held. 0: no room */
static uint32_t nvmtl_sc_take(uint32_t n)
{
    for (uint32_t i = 0; i < g_spc.nf; i++) {
        if (g_spc.fl[i] < n) continue;
        const uint32_t at = g_spc.fo[i];
        g_spc.fo[i] += n; g_spc.fl[i] -= n;
        if (!g_spc.fl[i]) { memmove(g_spc.fo + i, g_spc.fo + i + 1, (g_spc.nf - i - 1) * sizeof *g_spc.fo);
                           memmove(g_spc.fl + i, g_spc.fl + i + 1, (g_spc.nf - i - 1) * sizeof *g_spc.fl); g_spc.nf--; }
        return at;
    }
    return 0;
}
static void nvmtl_sc_give(uint32_t at, uint32_t n)
{
    uint32_t i = 0;
    while (i < g_spc.nf && g_spc.fo[i] < at) i++;
    if (i && g_spc.fo[i - 1] + g_spc.fl[i - 1] == at) {   /* joins the range before */
        g_spc.fl[i - 1] += n;
        if (i < g_spc.nf && at + n == g_spc.fo[i]) { g_spc.fl[i - 1] += g_spc.fl[i];
            memmove(g_spc.fo + i, g_spc.fo + i + 1, (g_spc.nf - i - 1) * sizeof *g_spc.fo);
            memmove(g_spc.fl + i, g_spc.fl + i + 1, (g_spc.nf - i - 1) * sizeof *g_spc.fl); g_spc.nf--; }
        return;
    }
    if (i < g_spc.nf && at + n == g_spc.fo[i]) { g_spc.fo[i] = at; g_spc.fl[i] += n; return; }
    if (g_spc.nf == g_spc.cf) {
        uint32_t *fo = realloc(g_spc.fo, sizeof *fo * g_spc.cf * 2), *fl = fo ? realloc(g_spc.fl, sizeof *fl * g_spc.cf * 2) : NULL;
        if (fo) g_spc.fo = fo;
        if (!fo || !fl) { nvlog("sparse tier 2: out of memory - %u counters are lost to later textures", n); return; }
        g_spc.fl = fl; g_spc.cf *= 2;
    }
    memmove(g_spc.fo + i + 1, g_spc.fo + i, (g_spc.nf - i) * sizeof *g_spc.fo);
    memmove(g_spc.fl + i + 1, g_spc.fl + i, (g_spc.nf - i) * sizeof *g_spc.fl);
    g_spc.fo[i] = at; g_spc.fl[i] = n; g_spc.nf++;
}

static int nvmtl_sc_log2(uint32_t v) { return v && !(v & (v - 1)) ? __builtin_ctz(v) : -1; }

int nvmtl_sparse_counters_make(void *const views[2], uint32_t w, uint32_t h, uint32_t d, uint32_t slices, uint32_t mips,
                               uint32_t tw, uint32_t th, uint32_t td, nvmtl_sc_layout *out)
{
    memset(out, 0, sizeof *out);
    if (!nvmtl_vk_sparse_tier2()) return -1;
    const int lw = nvmtl_sc_log2(tw), lh = nvmtl_sc_log2(th), ld = nvmtl_sc_log2(td);
    if (lw < 0 || lh < 0 || ld < 0 || lw > 15 || lh > 15 || ld > 15 || !w || !h || !d || !slices || w > 0xffff || h > 0xffff || d > 0xffff || slices > 0xffff) {
        g_spc.refused++;
        nvlog("sparse tier 2: a %ux%ux%u texture of %u slices, tiles %ux%ux%u, cannot have counters - it stays tier 1", w, h, d, slices, tw, th, td);
        return -1;
    }
    const uint32_t levels = mips < 1 ? 1 : mips > NVMTL_SC_LEVELS ? NVMTL_SC_LEVELS : mips;
    uint64_t n = 0;
    for (uint32_t m = 0; m < NVMTL_SC_LEVELS; m++) {
        const uint32_t mm = m < levels ? m : levels - 1;
        const uint32_t lw_ = MAX(1u, w >> mm), lh_ = MAX(1u, h >> mm), ld_ = MAX(1u, d >> mm);
        out->gx[m] = (lw_ + tw - 1) >> lw; out->gy[m] = (lh_ + th - 1) >> lh; out->gz[m] = (ld_ + td - 1) >> ld;
        if (m < levels) { out->start[m] = (uint32_t)n; n += (uint64_t)slices * out->gx[m] * out->gy[m] * out->gz[m]; }
        else out->start[m] = out->start[levels - 1];   /* the shader clamps the level; never read */
    }
    if (n + NVMTL_SC_HEADER > NVMTL_SC_WORDS / 2) {
        g_spc.refused++; nvlog("sparse tier 2: %llu counters is too many for one texture - it stays tier 1", (unsigned long long)n); return -1; }
    out->levels = levels; out->slices = slices; out->count = (uint32_t)n;
    const uint32_t len = (uint32_t)n + NVMTL_SC_HEADER;
    pthread_mutex_lock(&g_spc.lk);
    const uint32_t base = nvmtl_sc_take(len);
    pthread_mutex_unlock(&g_spc.lk);
    if (!base) { g_spc.refused++; nvlog("sparse tier 2: the counter buffer is full (%u words) - this texture stays tier 1", NVMTL_SC_WORDS); return -1; }
    /* the header and zeroed counters, on the GPU, after everything submitted before (work that still counts into this
       range for a texture freed earlier) */
    uint32_t hdr[NVMTL_SC_HEADER];
    for (uint32_t m = 0; m < NVMTL_SC_LEVELS; m++) hdr[m] = out->start[m];
    hdr[NVMTL_SC_LEVELS] = out->count;
    nvk_cmdbuf c; int ok = 0;
    if (!nvmtl_vk_cmd_begin(NULL, &c)) {
        nvmtl_vk_cmd_barrier(&c);
        g_spc.update(c.cb, g_spc.cntbuf, (VkDeviceSize)base * 4, sizeof hdr, hdr);
        if (out->count) pvkCmdFillBuffer(c.cb, g_spc.cntbuf, (VkDeviceSize)(base + NVMTL_SC_HEADER) * 4, (VkDeviceSize)out->count * 4, 0);
        nvmtl_vk_cmd_barrier(&c);
        ok = nvmtl_vk_submit_wait(&c) == 0;
    }
    pthread_mutex_lock(&g_spc.lk);
    if (!ok) { nvmtl_sc_give(base, len); pthread_mutex_unlock(&g_spc.lk); g_spc.refused++;
               nvlog("sparse tier 2: clearing the counters FAILED - this texture stays tier 1"); return -1; }
    out->rec[0] = base; out->rec[1] = w | h << 16; out->rec[2] = d | slices << 16;
    out->rec[3] = levels | (uint32_t)lw << 5 | (uint32_t)lh << 9 | (uint32_t)ld << 13;
    if (g_spc.ntex == g_spc.ctex) {
        nvmtl_sc_tex *t = realloc(g_spc.tex, sizeof *t * (g_spc.ctex ? g_spc.ctex * 2 : 16));
        if (!t) { nvmtl_sc_give(base, len); pthread_mutex_unlock(&g_spc.lk); memset(out, 0, sizeof *out); return -1; }
        g_spc.tex = t; g_spc.ctex = g_spc.ctex ? g_spc.ctex * 2 : 16;
    }
    nvmtl_sc_tex *t = &g_spc.tex[g_spc.ntex++];
    memset(t, 0, sizeof *t);
    t->view[0] = views[0]; t->view[1] = views[1]; memcpy(t->rec, out->rec, sizeof t->rec); t->base = base; t->len = len;
    const unsigned long long k = ++g_spc.made;
    pthread_mutex_unlock(&g_spc.lk);
    if (k <= 8 || !(k & (k - 1)))
        nvlog("sparse tier 2: texture %ux%ux%u, %u slices, %u levels, tiles %ux%ux%u -> %u counters at word %u (%llu textures so far)",
              w, h, d, slices, levels, tw, th, td, out->count, base, k);
    return 0;
}

void nvmtl_sparse_counters_free(const nvmtl_sc_layout *l)
{
    if (!l || !l->rec[0] || !g_spc.ready) return;
    pthread_mutex_lock(&g_spc.lk);
    for (uint32_t i = 0; i < g_spc.ntex; i++) {
        nvmtl_sc_tex *t = &g_spc.tex[i];
        if (t->base != l->rec[0]) continue;
        for (uint32_t k = 0; k < t->nslots; k++) nvmtl_sc_dir_write(t->slots[k], g_spc_zero);   /* its slots stop counting */
        free(t->slots);
        nvmtl_sc_give(t->base, t->len);
        g_spc.tex[i] = g_spc.tex[--g_spc.ntex];
        break;
    }
    pthread_mutex_unlock(&g_spc.lk);
}

void nvmtl_sparse_counters_slot(uint32_t slot, const void *view)
{
    if (!g_spc.ready || !view || !slot || slot >= NVMTL_SC_SLOT) return;
    pthread_mutex_lock(&g_spc.lk);
    for (uint32_t i = 0; i < g_spc.ntex; i++) {
        nvmtl_sc_tex *t = &g_spc.tex[i];
        if (t->view[0] != view && t->view[1] != view) continue;
        if (t->nslots == t->cslots) {
            uint32_t *s = realloc(t->slots, sizeof *s * (t->cslots ? t->cslots * 2 : 4));
            if (!s) break;
            t->slots = s; t->cslots = t->cslots ? t->cslots * 2 : 4;
        }
        t->slots[t->nslots++] = slot;
        nvmtl_sc_dir_write(slot, t->rec);
        break;
    }
    pthread_mutex_unlock(&g_spc.lk);
}

void nvmtl_sparse_counters_slot_gone(uint32_t slot)
{
    if (!g_spc.ready || !slot || slot >= NVMTL_SC_SLOT) return;
    pthread_mutex_lock(&g_spc.lk);
    nvmtl_sc_dir_write(slot, g_spc_zero);
    for (uint32_t i = 0; i < g_spc.ntex; i++) {
        nvmtl_sc_tex *t = &g_spc.tex[i];
        for (uint32_t k = 0; k < t->nslots; k++) if (t->slots[k] == slot) { t->slots[k] = t->slots[--t->nslots]; break; }
    }
    pthread_mutex_unlock(&g_spc.lk);
}

int nvmtl_sparse_counters_get(void *c_, const nvmtl_sc_layout *l, uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t z,
                              uint32_t w, uint32_t h, uint32_t d, void *dst_, size_t dst_off, int reset)
{
    nvk_cmdbuf *c = (nvk_cmdbuf *)c_; nvk_buffer *dst = (nvk_buffer *)dst_;
    if (!c || !l || !l->rec[0] || !g_spc.ready || level >= l->levels || slice >= l->slices) return -1;
    if (!d) d = 1;
    const uint32_t gx = l->gx[level], gy = l->gy[level], gz = l->gz[level];
    const uint32_t x1 = MIN(x + w, gx), y1 = MIN(y + h, gy), z1 = MIN(z + d, gz);
    if (x >= x1 || y >= y1 || z >= z1) return 0;
    const uint32_t nrows = (z1 - z) * (y1 - y), row = (x1 - x) * 4;
    const uint64_t first = (uint64_t)l->rec[0] + NVMTL_SC_HEADER + l->start[level];
    VkBufferCopy *bc = dst ? calloc(nrows, sizeof *bc) : NULL;
    if (dst && !bc) return -1;
    uint32_t k = 0;
    for (uint32_t zz = z; zz < z1; zz++)
        for (uint32_t yy = y; yy < y1; yy++) {
            const uint64_t src = (first + (((uint64_t)slice * gz + zz) * gy + yy) * gx + x) * 4;
            const uint64_t out = dst_off + (((uint64_t)(zz - z) * h + (yy - y)) * w) * 4;
            if (dst) {
                if (out + row > dst->size) continue;   /* past the app's buffer: Metal leaves that to the app; skipped */
                bc[k++] = (VkBufferCopy){ src, out, row };
            }
        }
    nvmtl_vk_cmd_barrier(c);
    if (dst && k) { pvkCmdCopyBuffer(c->cb, g_spc.cntbuf, (VkBuffer)dst->buf, k, bc); g_spc.gets++; }
    free(bc);
    if (reset || !dst) {
        if (dst) nvmtl_vk_cmd_barrier(c);   /* the copy has read them */
        for (uint32_t zz = z; zz < z1; zz++)
            for (uint32_t yy = y; yy < y1; yy++)
                pvkCmdFillBuffer(c->cb, g_spc.cntbuf, (first + (((uint64_t)slice * gz + zz) * gy + yy) * gx + x) * 4, row, 0);
        g_spc.resets++;
    }
    nvmtl_vk_cmd_barrier(c);
    return 0;
}
