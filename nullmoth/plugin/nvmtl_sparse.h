/* nvmtl_sparse.h - Metal sparse textures (tier 1) over Vulkan sparse image residency, behind NVMTL_SPARSE_TEXTURES=1.
   The Vulkan side is nvmtl_sparse_vk.c (included at the end of nvmtl_vk.c, so it shares its device and queue), the Metal
   side NVMTLSparse.m; docs/design/DESIGN-sparse-textures.md says what maps to what and in which order.
   Off (the default) nothing here is reached: no feature is enabled, no answer changes, no submission waits. */
#ifndef NVMTL_SPARSE_H
#define NVMTL_SPARSE_H
#include <stdint.h>
#include <stddef.h>

/* NVMTL_SPARSE_TEXTURES=1 in this process (a release plugin keeps it with NVMTL_PROCESS_ENV=1 or the conf file) */
int nvmtl_sparse_knob(void);
/* the knob is on and the device was created with sparseBinding + sparseResidencyImage2D + shaderResourceResidency on a
   queue that binds sparse memory, with timeline semaphores: every sparse answer of the plugin follows this */
int nvmtl_vk_sparse_tex(void);
/* nvmtl_vk.c's device creation: enables the features (from what the device supports) when the knob asks for them.
   supported/enabled are VkPhysicalDeviceFeatures. Returns 1 when sparse textures are on. */
int nvmtl_sparse_tex_enable(const void *supported, void *enabled, int queue_sparse, int timeline);

/* The tile shape (texels) of a sparse image of this format and shape (Vulkan's sparse image format granularity),
   0 when the format cannot be sparse. is3d: a 3D image; samples: 1 for every texture here. */
int nvmtl_vk_sparse_tile_shape(uint32_t vkfmt, int is3d, uint32_t samples, uint32_t *tw, uint32_t *th, uint32_t *td);
/* The bytes of one tile (the sparse block size: VkMemoryRequirements.alignment of a sparse image; 64 KiB on ANV), 0 off */
uint32_t nvmtl_vk_sparse_tile_bytes(void);

/* A sparse image's layout: tile shape, and its mip tail (Vulkan: imageMipTailFirstLod and the tail's opaque range). */
typedef struct {
    uint32_t tw, th, td;          /* tile shape in texels */
    uint32_t tile_bytes;
    uint32_t tail_first;          /* first mip level of the tail (== mips: no tail) */
    uint32_t single_tail;         /* one tail for every layer (VK_SPARSE_IMAGE_FORMAT_SINGLE_MIPTAIL_BIT) */
    uint64_t tail_size, tail_offset, tail_stride;   /* bytes; offset/stride in the image's opaque range */
    uint32_t mem_bits;            /* memory types the image may be bound to */
} nvmtl_sparse_layout;

/* A colour image with SPARSE_BINDING | SPARSE_RESIDENCY and nothing bound (every tile reads zero), its base view, and
   its layout. Arguments as nvmtl_vk_image_create_typed. out->placed is 1 (no memory of its own). -1 when the format or
   shape cannot be sparse here. */
int nvmtl_vk_image_create_sparse(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage,
                                 uint32_t mtl_type, uint32_t layers, void *nvk_image_out, nvmtl_sparse_layout *lay);
/* The tile pool of a sparse MTLHeap: one VkDeviceMemory of `bytes` (whole tiles) of a type sparse images can bind. */
int nvmtl_vk_sparse_pool_create(size_t bytes, void **mem_out);
void nvmtl_vk_sparse_pool_free(void *mem, size_t bytes);
/* 1 when a placement heap's memory type (nvk_heap.memtype) can back tiles of an image of layout `lay` */
int nvmtl_vk_sparse_memtype_ok(const nvmtl_sparse_layout *lay, uint32_t memtype);

/* A batch of binds that runs when the submission it rides is submitted (nvk_cmdbuf.sparse), before that submission's
   commands and after everything submitted before it (nvmtl_vk_submit_begin_at). mem NULL unbinds. */
typedef struct nvmtl_sparse_batch nvmtl_sparse_batch;
nvmtl_sparse_batch *nvmtl_sparse_batch_of(void *nvk_cmdbuf_c);   /* the batch of a recording, made on first use */
int nvmtl_sparse_batch_tile(nvmtl_sparse_batch *b, void *img, uint32_t mip, uint32_t layer, uint32_t x, uint32_t y, uint32_t z,
                            uint32_t w, uint32_t h, uint32_t d, void *mem, uint64_t mem_off);
int nvmtl_sparse_batch_opaque(nvmtl_sparse_batch *b, void *img, uint64_t res_off, uint64_t size, void *mem, uint64_t mem_off);
/* run(b, ctx) once the work submitted before the batch has COMPLETED (a CPU wait), then drop(ctx): for binds whose
   arguments the GPU writes (updateTextureMapping:indirectBuffer:). drop alone when the batch is never run. */
int nvmtl_sparse_batch_defer(nvmtl_sparse_batch *b, void (*run)(nvmtl_sparse_batch *, void *), void *ctx, void (*drop)(void *));
void nvmtl_sparse_batch_free(nvmtl_sparse_batch *b);
/* nvmtl_vk_submit_begin_at, submit mutex held: queues c's batch (if any) and returns the timeline point c must wait for
   on *sem (0: none). */
uint64_t nvmtl_sparse_before_submit(void *nvk_cmdbuf_c, void **sem);
#ifdef __OBJC__
/* NVMTLSparse.m (the Metal half). heap/texture/command buffer arguments are NVMTLHeap / NVMTLTexture /
   NVMTLCommandBuffer, typed id here because this header comes before those classes. */
@class MTLHeapDescriptor, MTLTextureDescriptor;
int nvmtl_sparse_heap_init(id heap, MTLHeapDescriptor *d);       /* MTLHeapTypeSparse: 1 a tile pool, 0 not ours, -1 failed */
id nvmtl_sparse_new_texture(id heap, MTLTextureDescriptor *d);   /* on a sparse heap; heap nil: placement sparse (Metal 4) */
id nvmtl_sparse_encoder(id commandBuffer);                       /* resourceStateCommandEncoder: nil when off */
NSUInteger nvmtl_sparse_heap_used(id heap);
NSInteger nvmtl_sparse_texture_tier(id texture);                 /* MTLTextureSparseTier: 1 for a sparse texture, else 0 */
#endif
#endif
