# Metal sparse textures (tier 1) over Vulkan sparse residency (design, 2026-11-12)

> Design note with its date: it records what was built and the state of that day. Status: implemented behind
> `NVMTL_SPARSE_TEXTURES=1` (default off), **not yet run on the B580**. The gate is `user/tests/vulkan/vksparse.c`.

## Why it can work

ANV on IntelXe.kext advertises `sparseBinding`, `sparseResidencyBuffer`, `sparseResidencyImage2D/3D`,
`sparseResidencyAliased` and `shaderResourceResidency` (vkcaps). Our kernel identifies as xe, so ANV takes its
VM_BIND path (`anv_sparse.c` `anv_sparse_bind_vm_bind`): a sparse image is a VA range with no BO, bound tile by tile
with VM_BIND MAP of a BO range, every unbound tile a `DRM_XE_VM_BIND_FLAG_NULL` range. IntelXe.kext implements both
(`xe_core.cpp` `bindOpCheck`, NULL leaves written as `XE_PTE_NULL`): reads of a NULL page return zero and writes are
dropped, which is Metal's tier-1 contract for unmapped tiles and Vulkan's `residencyNonResidentStrict`. On Xe2 a sparse
image is Tile64: a tile is 64 KiB with the standard block shape (RGBA8: 128x128, the same shape an Apple GPU reports for
64 KiB pages), and the smallest levels share one 64 KiB mip tail.

## What maps to what

| Metal | Vulkan / plugin |
| --- | --- |
| `MTLHeapTypeSparse` heap of N bytes | one `VkDeviceMemory` of N / 64 KiB tiles (device-local, a type the sparse probe image accepts) and a free stack of its pages (`NVMTLSparsePool`) |
| texture made on that heap | `VkImage` with `SPARSE_BINDING | SPARSE_RESIDENCY` (+ `SPARSE_ALIASED` where `sparseResidencyAliased` is offered, for Metal 4's shared tiles; 2D, 2D array, cube, cube array, 3D; colour formats Vulkan gives sparse format properties for), nothing bound: every tile reads 0 |
| placement sparse texture (`placementSparsePageSize` = 64 KiB, Metal 4) | the same image, tiles bound from an `MTLHeapTypePlacement` heap's memory at the offsets the app names |
| `sparseTileSizeWithTextureType:pixelFormat:sampleCount:(sparsePageSize:)` | `vkGetPhysicalDeviceSparseImageFormatProperties` granularity (one page size: 64 KiB, whatever is asked) |
| `sparseTileSizeInBytes`, `...ForSparsePageSize:` | the sparse block size (`VkMemoryRequirements.alignment` of a sparse image) |
| `firstMipmapInTail`, `tailSizeInBytes` | `imageMipTailFirstLod`, `imageMipTailSize` |
| `resourceStateCommandEncoder` `updateTextureMapping(s):mode:region:mipLevel:slice:` | per tile, a page taken from (map) or given back to (unmap) the heap's pool and a `VkSparseImageMemoryBind` (NULL memory to unmap); a tail page a `VkSparseMemoryBind` in the tail's opaque range (`imageMipTailOffset + layer * imageMipTailStride`) |
| `updateTextureMapping:mode:indirectBuffer:indirectBufferOffset:` | the same, its arguments read on the CPU once the work before the binds has finished (shared/managed buffers only; a private buffer is refused) |
| `moveTextureMappingsFromTexture:...` | the source tiles' pages handed to the destination's unmapped tiles (same heap): an unbind and a bind |
| `MTL4CommandQueue updateTextureMappings:heap:operations:count:` | the same binds with explicit pages (`heapOffset` in tiles, X then Y then Z), on a command buffer of the Metal 4 layer's private queue |
| `MTL4CommandQueue copyTextureMappingsFromTexture:...` | the destination binds the source tiles' pages (both placement sparse, one heap) |
| `sparse_sample`, `sparse_read`, `sparse_gather`, `sparse_sample_compare`, `sparse_gather_compare`, `resident()` | `OpImageSparse*` + `OpImageSparseTexelsResident` (translator `images/residency.rs`; see below) |
| `sparseTextureTier` | Tier1 for a sparse texture, None otherwise; `supportsPlacementSparse` YES |
| device creation | `sparseBinding`, `sparseResidencyImage2D`, `shaderResourceResidency` (+ `sparseResidencyImage3D`, `sparseResidencyAliased` when offered), only with the knob, on the plugin's one graphics+compute queue (which must have `VK_QUEUE_SPARSE_BINDING_BIT`) and with timeline semaphores |

Pages are taken and given back when a mapping is *encoded*, in encoding order, since the binds reach the GPU in that
order. A command buffer encoded and never committed keeps its pages taken. A texture gives its pages back when it is
released; `nvmtl_vk_image_destroy` destroys a sparse image at once (no grace deferral), which unbinds its whole range
in ANV (`anv_free_sparse_bindings`), so a heap freed right after cannot stay mapped. `heap.usedSize` is mapped tiles
x tile size (an M1 answers the same).

## Ordering

Vulkan does not order `vkQueueBindSparse` against the queue's command buffers except through semaphores. Metal applies a
resource state encoder's mappings after the encoders before it and before the ones after it, in queue order. So:

1. Creating the encoder seals what the command buffer recorded so far into a submission part of its own (the event
   machinery's `nvmtlSealPartWithSignal:restart:`), and counts itself as an encoder of the next part so that part is
   never retired as empty. The binds collect in that next recording (`nvk_cmdbuf.sparse`).
2. When that recording is submitted (`nvmtl_vk_submit_begin_at`, submit mutex held): a `vkQueueSubmit` with no command
   buffers signals point v1 of a timeline (its signal's first scope is every earlier submission), `vkQueueBindSparse`
   waits for v1 and signals v2, and the recording's own submit waits for v2. Later submissions wait for v2 too until
   the timeline is seen there (one `vkGetSemaphoreCounterValue` per submit, only while a bind may be outstanding).
3. Metal 4 queue mappings ride a command buffer of the private queue, like its waits and signals, so they order
   against commits the same way.

Price on our kernel: VM_BIND waits for its in-fences on the CPU (`waitSyncsIn`, up to 10 s) and writes the page
tables before it returns, so the bind holds the submitting thread and the submit mutex until the GPU has drained the
work before it. Mapping updates are a pipeline drain, as they effectively are on many drivers; an asynchronous bind in
the kernel would turn it into a GPU-side wait with no change in the plugin.

## Shaders

Apple's frontend has no sparse intrinsic: `t.sparse_sample(s, uv)` is `{ <4 x float>, i8 } @air.sample_texture_2d.v4f32`
like `t.sample(s, uv)`, and only extracts member 1 (nonzero = not resident; the header passes a `bool *nack`). Checked
by compiling `fixtures/sparse/probe.metal` with this Mac's Metal compiler (Metal 4.1, AIR 2.9). The translator lowered
member 1 to OpUndef; now, when it is read, the lowered image instruction becomes its sparse twin and the flag is
`OpImageSparseTexelsResident(code) ? 0 : 1`. That needs `shaderResourceResidency`, enabled with the knob; the
translator's caps bit 3 (`NVMTL_SPARSE_TEXTURES`) turns it on, and the shader cache tag gets `-sr`. Without the knob
the flag is the constant 0: every texture is resident, which Metal says of a texture that is not sparse. A colour the
lowering computes from several texels (a linear pixel-coordinate sampler on an array, bicubic filtering, guarded
fetches) is refused while the knob is on: there is no single residency to report.

## Limits (tier 1 here)

- One page size, 64 KiB. A heap asked for 16 or 256 KiB pages still gets 64 KiB tiles (logged; the tile-size queries
  answer for 64 KiB); a placement sparse texture with another `placementSparsePageSize` is refused.
- Colour textures only; depth/stencil sparse textures, multisampled sparse textures and sparse buffers
  (`sparseBufferTier` None) are not built. No sparse 1D textures (Vulkan has none).
- A placement sparse texture maps tiles from one heap at a time.
- No residency counters/feedback (`MTLTextureSparseTier2`, `sparse_sample` LOD clamp feedback).
- Mappings encoded in a command buffer that is never committed keep their pages.

## Testing

- `user/tests/vulkan/vksparse.c` (the gate, ANV directly): format and memory requirements, tile binds ordered on a
  timeline, upload over bound and unbound tiles, readback, `OpImageSparseFetch` / `OpImageSparseSampleExplicitLod`
  residency, a remap whose page keeps its data at the new position, a mip-tail opaque bind.
- `user/tests/metal/mtl_sparse.m`: the Metal path end to end (heap, texture, map/unmap, blit, compute `sparse_read` /
  `sparse_sample`, fragment `sparse_sample`, `usedSize`, mip tail). 106/106 on an M1 Pro (`--any`, the reference).
- Translator: `tests/sparse_residency.rs` (both modes, under spirv-val) and unit tests in `images/residency.rs`.
