/* nvmtl_sparse.h - Metal sparse textures (tier 1) over Vulkan sparse image residency, behind NVMTL_SPARSE_TEXTURES=1.
   The Vulkan side is nvmtl_sparse_vk.c (included at the end of nvmtl_vk.c, so it shares its device and queue), the Metal
   side NVMTLSparse.m; docs/design/DESIGN-sparse-textures.md says what maps to what and in which order.
   Off (the default) nothing here is reached: no feature is enabled, no answer changes, no submission waits. */
#ifndef NVMTL_SPARSE_H
#define NVMTL_SPARSE_H
#include <stdint.h>
#include <stddef.h>

/* (Placement sparse buffers - Metal 4's MTLBufferSparseTier1 - ride the same knob, the same batches and the same
   ordering; they need sparseResidencyBuffer too.) */
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
/* Sparse textures are on and the device binds sparse residency buffers (placement sparse buffers) */
int nvmtl_vk_sparse_buf(void);
/* A VkBuffer with SPARSE_BINDING | SPARSE_RESIDENCY (+ SPARSE_ALIASED where offered) of `size` bytes rounded up to
   `page_bytes` (which the buffer block size must divide), nothing bound: every byte reads zero, writes vanish.
   out->placed is 1 (no memory of its own); *mem_bits the memory types its tiles may come from. -1 when refused. */
int nvmtl_vk_buffer_create_sparse_residency(size_t size, uint32_t page_bytes, void *nvk_buffer_out, uint32_t *mem_bits);
/* Destroys a sparse buffer at once (no grace deferral), which unbinds its range */
void nvmtl_vk_sparse_buffer_destroy(void *nvk_buffer);
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
/* A range of a sparse buffer bound to `mem` at mem_off (NULL: unbound); merged with the previous range when contiguous */
int nvmtl_sparse_batch_buffer(nvmtl_sparse_batch *b, void *buf, uint64_t res_off, uint64_t size, void *mem, uint64_t mem_off);
/* run(b, ctx) once the work submitted before the batch has COMPLETED (a CPU wait), then drop(ctx): for binds whose
   arguments the GPU writes (updateTextureMapping:indirectBuffer:). drop alone when the batch is never run. */
int nvmtl_sparse_batch_defer(nvmtl_sparse_batch *b, void (*run)(nvmtl_sparse_batch *, void *), void *ctx, void (*drop)(void *));
void nvmtl_sparse_batch_free(nvmtl_sparse_batch *b);
/* nvmtl_vk_submit_begin_at, submit mutex held: queues c's batch (if any) and returns the timeline point c must wait for
   on *sem (0: none). */
uint64_t nvmtl_sparse_before_submit(void *nvk_cmdbuf_c, void **sem);
/* ---- tier 2: per-tile access counters (NVMTL_SPARSE_TIER2=1 with NVMTL_SPARSE_TEXTURES=1; DESIGN-sparse-textures.md
   "Tier 2"). Vulkan and Xe2 have none: the translator (src/sparse_counters.rs, caps bit 4) makes every texel read of a
   heap texture add to its tile's counter. Heap slot NVMTL_SC_SLOT holds the directory (binding 3, RGBA32UI, one element
   per heap slot) and the counters (binding 4, R32UI); a texture's counters are a header of NVMTL_SC_HEADER words (where
   each level's counters start; word 15: how many) and then level by level, slice, z, y, x. Off, nothing here is reached
   and the heap hands out every slot as before. */
#define NVMTL_SC_SLOT 32767u        /* the translator's sparse_counters::COUNTER_HEAP_SLOT: the heap's last slot */
#define NVMTL_SC_HEADER 16u
#define NVMTL_SC_LEVELS 15u
/* NVMTL_SPARSE_TIER2=1 and NVMTL_SPARSE_TEXTURES=1 in this process */
int nvmtl_sparse_tier2_knob(void);
/* the knob, sparse textures on, and the counters made (the heap has its texel bindings, the slot is kept) */
int nvmtl_vk_sparse_tier2(void);
/* A texture's counters: the record its heap slots' directory elements get, and where each level starts */
typedef struct {
    uint32_t rec[4];                      /* directory element: header word, w | h << 16, d | slices << 16, levels | log2 tile */
    uint32_t start[NVMTL_SC_LEVELS];      /* level m's first counter, from the end of the header */
    uint32_t gx[NVMTL_SC_LEVELS], gy[NVMTL_SC_LEVELS], gz[NVMTL_SC_LEVELS];   /* tiles per level */
    uint32_t levels, slices, count;       /* counted levels, slices (layers; 1 for 3D), counters after the header */
} nvmtl_sc_layout;
/* Counters for a new sparse texture of level-0 size w x h x d (d 1 unless 3D), `slices` layers, `mips` levels and tile
   shape tw x th x td; views[0..1] (NULL allowed) are the image views its heap slots will hold. Zeroed on the GPU before
   this returns. 0 on success; -1 (logged): the texture stays tier 1. */
int nvmtl_sparse_counters_make(void *const views[2], uint32_t w, uint32_t h, uint32_t d, uint32_t slices, uint32_t mips,
                               uint32_t tw, uint32_t th, uint32_t td, nvmtl_sc_layout *out);
/* the texture is gone: its slots stop counting at once, its counters go back */
void nvmtl_sparse_counters_free(const nvmtl_sc_layout *l);
/* nvmtl_vk.c's heap: a slot now holds `view` / the slot was given up. No-ops unless the counters are on. */
void nvmtl_sparse_counters_slot(uint32_t slot, const void *view);
void nvmtl_sparse_counters_slot_gone(uint32_t slot);
/* Records into c (an nvk_cmdbuf), ordered as a blit: the counters of tiles [x, x + w) x [y, y + h) x [z, z + d) of `level`
   and `slice` copied into dst (an nvk_buffer) at dst_off, one uint32 per tile, x fastest, then y, then z (reset: then
   cleared); or only cleared (dst NULL). Tiles outside the level are skipped. Returns -1 when nothing could be recorded. */
int nvmtl_sparse_counters_get(void *c, const nvmtl_sc_layout *l, uint32_t level, uint32_t slice, uint32_t x, uint32_t y, uint32_t z,
                              uint32_t w, uint32_t h, uint32_t d, void *dst, size_t dst_off, int reset);
#ifdef __OBJC__
/* NVMTLSparse.m (the Metal half). heap/texture/command buffer arguments are NVMTLHeap / NVMTLTexture /
   NVMTLCommandBuffer, typed id here because this header comes before those classes. */
@class MTLHeapDescriptor, MTLTextureDescriptor;
int nvmtl_sparse_heap_init(id heap, MTLHeapDescriptor *d);       /* MTLHeapTypeSparse: 1 a tile pool, 0 not ours, -1 failed */
id nvmtl_sparse_new_texture(id heap, MTLTextureDescriptor *d);   /* on a sparse heap; heap nil: placement sparse (Metal 4) */
id nvmtl_sparse_encoder(id commandBuffer);                       /* resourceStateCommandEncoder: nil when off */
NSUInteger nvmtl_sparse_heap_used(id heap);
NSInteger nvmtl_sparse_texture_tier(id texture);                 /* MTLTextureSparseTier: 1 for a sparse texture, else 0 */
id nvmtl_sparse_new_buffer(NSUInteger length, NSUInteger options, NSInteger pageSize);   /* placement sparse buffer, nil refused */
NSInteger nvmtl_sparse_buffer_tier(id buffer);                   /* MTLBufferSparseTier: 1 for a placement sparse buffer, else 0 */
void nvmtl_sparse_buffer_gone(id buffer);                        /* NVMTLBuffer dealloc: its VkBuffer, at once */
#endif
#endif
