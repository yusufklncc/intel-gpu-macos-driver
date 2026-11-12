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
