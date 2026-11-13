# Descriptor model of the IntelXe Metal plugin

> Design note with its date: it records the plan and the state of that day, not the current status. The current
> state is in the README and `docs/devices/b580.md`.

How the plugin turns Metal resource bindings into Vulkan descriptors on our ANV (Xe2, B580), and how that changes. The goal
is less CPU time per draw in games such as Minecraft (GL 4.1 over Metal). Status 2026-10-29: measured, stage 1 built and
staged as `~/b580/desc1`, stages 2–4 designed. 2026-11-12: stage 2 completed and made ready for the default switch
(section 13); the default stays classic until the night measurement.

Tree: `src/nullmoth-desc`, copied from `src/nullmoth` (fix24b sources plus uncommitted work) on 2026-10-29.
Build: `tools/build_desc.sh <name>` gives `build-<name>/`. It uses the same flags, translator dylib, air-runtime and
ad-hoc signing scheme as `build_plugin_fix24b.sh`. `build-desc0` is bit-identical to the installed fix24b
(`710e621f2d2a`).

## 1. Measurement

### Benchmark

`tests/mtl_bind.m` is the synthetic Metal benchmark.

- Each frame is one render encoder of 2000 draws, one quad per draw in a 64×64 grid, on a 1024² RGBA8 target with blending.
- The run is 20–30 frames after 3 warm-up frames.
- Encode time is measured from `renderCommandEncoderWithDescriptor` to `endEncoding`. The output image is hashed and
  written to `-o <dir>`.
- `NVMTL_BUNDLE=<path>` loads a staged plugin build in-process, so nothing is installed.
- CPU profiles come from `/usr/bin/sample` (a real binary, not a CLT shim) running against `mtl_bind -loop`.

Modes:

| mode | per draw |
|---|---|
| none | only the draw (pipeline and bindings set once) |
| bytes | setVertexBytes + setFragmentBytes |
| vbo | setVertexBufferOffset (stage_in) |
| tex | setFragmentTexture (8 textures) |
| samp | setFragmentSamplerState (4 samplers) |
| gl | GL-like mix: setVertexBuffer + setVertexBytes + setFragmentBytes + 2 textures + 2 samplers |
| argbuf | setFragmentBuffer of an argument buffer (texture + sampler + constant) |
| pso | two pipelines alternating + setVertexBytes |
| cdisp | compute: setBuffer + setBytes + dispatch |

### Baseline: installed fix24b (= desc0), encode ns per draw (p50)

| none | bytes | vbo | tex | samp | gl | argbuf | pso | cdisp |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 535 | 1031 | 913 | 1342 | 1729 | 3220 | 1113 | 1465 | 1380 |

GPU time per frame is 0.5–0.8 ms in every render mode. The plugin's CPU time per draw is the limit.

### Where the time went (sample, main thread, share of encode samples)

**gl** (3220 ns/draw):

| share | where |
|---:|---|
| 35% | `setFragmentSamplerState`: `[@(i) stringValue]` builds an NSString key for the pixel-sampler dictionary on every call, even for normalized samplers (26%) |
| 16% | `nvmtl_snapshot_set_c`, the Vulkan descriptor set build: `vkUpdateDescriptorSets` writes, address-table fill, set recycling |
| 9% | `nvmtl_bind_embedded`: two associated-object lookups (static samplers, embedded plan) per stage per draw, with retain/release |
| 7% | ANV `vkCmdDraw` flush: `emit_binding_table` per stage, a new dynamic-UBO surface state for the address table every flush, push-constant re-upload |
| 15% | `setFragmentTexture`: surface tracking, ResSet, retains |
| ~5% | `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)` in the diagnostic-flag pollers (`nvmtl_trace_on` and others), on every bind and draw |
| 6% | `setBytes` ring, mostly objc_retain |

**none** (535 ns/draw): `nvmtl_bind_embedded` is 56% of the draw, the trace clock 10%, ANV draw 6%.

**bytes** (1031 ns/draw):

- The address table is 16%. Part of that is a `vkGetBufferDeviceAddress` call per unbound slot per snapshot.
- ANV flush is 14%.
- The set itself is reused through the dynamic offset, so no new set is built.

**Summary:** the Vulkan descriptor part (our set build plus ANV's flush) was about 21% of a GL-like draw, roughly
0.7 µs. The Metal-side binding front end around it was 50–60%. Both are fixed below. The front end came first because it
was larger, and because the new descriptor model needs a cheap per-stage dirty state anyway.

## 2. What ANV on Xe2 offers (src/mesa-video, gfx20)

Line references are to `src/mesa-video/src/intel/vulkan`.

### Descriptor storage

- Descriptors are **direct**: `indirect_descriptors = !intel_has_extended_bindless`, and Xe2 has extended bindless.
- An image, buffer or texel descriptor is a 64 B RENDER_SURFACE_STATE. A sampler is 32 B.
- They are memcpy'd from host copies kept in the view or sampler (`anv_descriptor_set.c` 2101–2115), or packed by
  `isl_buffer_fill_state`.

### Classic sets

- Classic sets live in a pool BO. On a dGPU that is **write-combined BAR VRAM**.
- `VkCopyDescriptorSet` is a memcpy *from* that WC mapping, which means uncached PCIe reads (D:2561–2583).
- Each draw with dirty descriptors re-emits, per stage:
  - the binding table, allocated from a 4 KB block;
  - the samplers;
  - the push constants;
  - one new surface state for each dynamic UBO or SSBO (G:2541–2587, genX_cmd_draw.c 996–1073).
- The plugin's address table is a dynamic UBO, so it pays that last item on every flush.

### VK_EXT_descriptor_buffer

Advertised unconditionally, with:

- descriptorBufferPushDescriptors and bufferlessPushDescriptors;
- offset alignment 64;
- 32 buffer bindings;
- a 2 GiB address space (the `dynamic_visible` pool).

Behaviour on gfx ≥ 12.5:

- **Record time:** `vkCmdBindDescriptorBuffersEXT` and `vkCmdSetDescriptorBufferOffsets2EXT` only store state
  (anv_cmd_buffer.c 730–839).
- **No STATE_BASE_ADDRESS when the descriptor buffer changes:** the bindless base is the whole pool (G:3528–3535).
- **Draw time, per changed set:** one 64 B surface state and the set offsets in push constants. There is no per-binding
  binding-table work.
- **Samplers** in a descriptor-buffer layout sit in the set's surface area (`descriptor_buffer_sampler_size == 0`,
  D:964). One buffer with both RESOURCE and SAMPLER usage holds both.
- **`vkGetDescriptorEXT`** is a pure host memcpy or `isl` fill (D:2753–2927).

### Hard constraint: never mix models

- Switching between classic (LEGACY) and BUFFER mode emits `PIPE_CONTROL` (CS stall) + `STATE_BASE_ADDRESS` + cache
  invalidates (G:3516–3565, 281–441).
- Once the extension is enabled, every `vkBeginCommandBuffer` starts in BUFFER mode with an SBA (G:4110–4118).

So descriptor buffers are enabled only when **every** layout the plugin uses is a descriptor-buffer layout:

- graphics and compute;
- the bindless heap;
- framebuffer fetch;
- the internal pipelines: visibility sum, tessellation, nvconv.

### Other limits

- KHR_push_descriptor: 32 descriptors per push set.
- maxPushConstantsSize 256.
- Dynamic UBOs/SSBOs: 8 each per set.
- Update templates, maintenance6, mutable descriptors and update-after-bind are all present.
- `EXT_descriptor_heap` is advertised too (P:558), but it is new; see stage 4.
- The CPU-visible VRAM BAR is 256 MB, shared system-wide. Descriptor memory competes for it, so descriptor rings stay
  in the low MB.

## 3. The plugin's model today (fix24b)

### Sets

- **Set 0 (vertex/compute) and set 1 (fragment):** one global 321-binding layout:
  - buffers 0–31 (SSBO);
  - textures 32–159 (mutable: sampled image / uniform texel);
  - samplers 160–191;
  - the address table at binding 640 (UBO_DYNAMIC);
  - storage textures 480–607 (mutable).
- **Set 2:** the bindless heap. It holds 131072 sampled images and 2048 samplers in an update-after-bind pool, and is used
  by argument buffers and gpuResourceID.
- **Set 3:** framebuffer-fetch input attachments.

### Per draw

1. `nvmtl_bind_*` updates a per-stage slot table with dirty masks.
2. `nvmtl_snapshot_set_c` either reuses the last set (only a new address-table dynamic offset) or builds a new one:
   - takes a recycled `VkDescriptorSet`;
   - writes the bindings the pipeline reads and copies the unchanged ones from the last set;
   - fills a 1.5 KB address table (32 buffer addresses, 32 self addresses, 128 texture sizes).
3. `vkCmdBindDescriptorSets` binds 3 sets with 2 dynamic offsets.

This design has already been tuned: set recycling, dirty-only writes, dynamic-offset reuse. Its remaining costs are
structural to classic sets on ANV:

- set objects;
- `anv_descriptor` host bookkeeping;
- copies read from WC memory;
- binding-table emission per stage;
- a dynamic-UBO surface state every flush;
- a Vulkan call per descriptor write.

## 4. Chosen model: VK_EXT_descriptor_buffer, exclusively, with per-pipeline compact layouts

### Why this one

1. **It is the descriptor model ANV implements natively on Xe2.** A direct descriptor is just 64 bytes in GPU memory.
   With descriptor buffers the plugin writes those bytes itself:
   - It caches them in host memory once per view, sampler or texel view (`vkGetDescriptorEXT` at creation).
   - It never reads WC memory.
   - There are no `VkDescriptorSet`/pool objects, no recycle lists, and no ANV per-set bookkeeping.
2. **It has the cheapest draw-time path in ANV:** no binding tables for resources and no SBA on buffer switches. Each
   changed set costs one surface state plus push-constant offsets.
3. **It matches Metal.** A Metal argument buffer is GPU memory holding resource descriptors, and gpuResourceID is a heap
   index. The bindless heap simply becomes a region of a descriptor buffer, written with `vkGetDescriptorEXT` bytes, with
   no update-after-bind pool.
4. **It is standard Vulkan, not an ANV trick.** NVK (this plugin's other backend, vendor 0x10de) implements it too, and
   DXVK and vkd3d-proton use the same model.

### Rejected

- **Push descriptors (KHR_push_descriptor + templates).**
  - A push set holds at most 32 descriptors, and a layout may have only one push set. Vertex and fragment bindings share
    numbers, so this needs a translator renumbering.
  - ANV allocates a new push buffer per push, memcpys the old contents back out of WC memory, and still re-emits binding
    tables (LEGACY mode).
  - It is no cheaper than descriptor buffers and caps the binding count.
- **Classic sets + update templates + compact per-pipeline layouts.**
  - Keeps every structural cost above, but would cut our own `vkUpdateDescriptorSets` call cost.
  - Kept only as the **fallback** model for a device without descriptor_buffer. Its compact layouts and binding plan are
    shared with the chosen model (stage 2a).
- **EXT_descriptor_heap.** It is closer still to Metal and D3D12: one resource heap, one sampler heap, and SPIR-V
  set/binding mapped to heap offsets at pipeline creation. But it is weeks old in our Mesa and absent elsewhere. It
  becomes cheap to adopt once the plugin already works in "descriptor bytes + offsets" terms (stage 4).

### Design

**Layouts per pipeline, not global.**

- At pipeline creation the plugin already knows which bindings each stage reads (`bmask`/`gmask`/`stex_hi`/`tbm`,
  computed from SPIR-V).
- Each pipeline gets descriptor-buffer set layouts (`DESCRIPTOR_BUFFER_BIT_EXT`) that list **only** the used bindings,
  keeping their binding numbers. ANV indexes bindings by number, so the SPIR-V does not change.
- A typical stage set is 3–8 descriptors (192–512 B), not 321 bindings (about 20 KB).
- `vkGetDescriptorSetLayoutBindingOffsetEXT` gives each used binding's offset once. That produces the stage's **binding
  plan**: an array of `(slot index, byte offset, kind)`.
- Layouts are deduplicated by binding mask, because many pipelines share them.

**Slot table holds descriptor bytes.**

- Each stage's slot keeps a pointer to the bound object's cached descriptor bytes (64 B image, 32 B sampler, 64 B texel
  view), plus a generation counter.
- Buffers bound as descriptors (rare: most buffer access goes through the address table) get their 64 B from
  `vkGetDescriptorEXT(STORAGE_BUFFER, {addr+off, range})` at bind time.
- A bind is a pointer store and a dirty bit. There is no Vulkan call.

**Per command buffer descriptor ring.**

- Host-visible buffer chunks (e.g. 256 KB) with usage `RESOURCE|SAMPLER_DESCRIPTOR_BUFFER|SHADER_DEVICE_ADDRESS`, taken
  from a pool and kept or retired with the command buffer's fence exactly like today's address pages (witness-guarded
  reuse).
- At a draw whose stage is dirty (a used slot changed, or the pipeline changed):
  1. allocate `plan.size` bytes from the ring;
  2. for each plan entry, memcpy the cached bytes to `ring + offset`. These are sequential WC writes; nothing is read
     back.
  3. `vkCmdSetDescriptorBufferOffsets2EXT` for that set.
- A stage that is not dirty keeps its offset, so no work is done.
- Ring use at 2000 draws × 2 stages × ~320 B is about 1.3 MB per frame.

**Address table.**

- Stays a UBO, but as a plain `UNIFORM_BUFFER` descriptor in the stage set: descriptor-buffer layouts cannot hold dynamic
  descriptors. Its 64 B come from `vkGetDescriptorEXT(UNIFORM_BUFFER, {table addr, 1536})`, about 50 ns.
- The SPIR-V does not distinguish dynamic from non-dynamic, so it does not change.
- The table is written only when a buffer slot or texture size the stage reads changed.
- Stage 4 moves its pointer to a push constant (BDA), which removes the descriptor.

**Bindless heap.**

- One process-wide descriptor buffer: 131072 × 64 B (8 MiB) plus 2048 × 32 B, written by `nvmtl_vk_bindless_put`/
  `_sampler_slot` with cached bytes.
- Bound as set 2 at buffer index 0 for the whole command buffer.
- Its quarantine/FIFO logic stays as is.

**Framebuffer fetch (set 3)** uses `INPUT_ATTACHMENT` descriptors (64 B) written into the ring per pass.

**Internal pipelines** (visibility sum, tessellation, nvconv, SASS launch) get descriptor-buffer layouts as well, so no
command buffer ever switches mode. This is the gating item for turning the model on.

**Selection.**

- `nvmtl_vk_init` enables `VK_EXT_descriptor_buffer` and the buffer model only when the device has it and
  `NVMTL_DESC_MODEL` is not `classic`. Until stage 3, the default is `classic`.
- The two models share the front end (slot table, binding plan) and differ only in how the plan is flushed.

**Expected effect** (from the profiles): the descriptor part of a GL-like draw drops from about 600–700 ns to about
150–250 ns. That is the ring memcpy of 3–8 × 64 B, one offsets call, and ANV's per-set surface state plus push
constants. Draws whose bindings did not change cost nothing in the descriptor path.

## 5. Stages

### Stage 1 — binding front end (DONE, `~/b580/desc1`)

All changes are semantics-preserving; every output image is byte-identical to the baseline.

1. **Sampler specialization key.** `_pixMask[2]` records which sampler indices hold a pixel-coordinate (unnormalized)
   state. Binding any other sampler touches neither dictionary, so no NSString key is built
   (`setVertex/FragmentSamplerState`, the lodClamp variants).
2. **Per-function binding info.** Constexpr samplers, their slot mask and the embedded argument-buffer plan are built
   once per NVMTLFunction (`nvmtl_fn_bindinfo`, published with release/acquire). They are read lock-free on every draw;
   per-draw associated-object lookups and NSArray walks are gone. The "give back static sampler slots" loop runs only
   when a slot is or was taken.
3. **Per-pipeline function cache.** `_bindVF`/`_bindFF` on NVMTLRenderPipelineState replace the descriptor property
   getters, which retain/autorelease on every draw.
4. **Diagnostic-flag pollers.** The trace, dump, noseed, static-linear and passtime pollers read `mach_approximate_time`
   (`nvmtl_poll_ns`) instead of `clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW)`. They still look at the flag files every
   250 ms.
5. **Address table.** The dummy buffer's and each table page's device address are read once, not by a
   `vkGetBufferDeviceAddress` per unbound slot per snapshot.

### Stage 1b — rest of the front end (measured, not done)

| share of desc1 gl encode | where |
|---:|---|
| 20% | `setFragmentTexture`: `nvmtl_surface_in_for_cb` (`[_surfOverwrites containsObject:]` + surface-in) and ResSet adds per bind. Fix: per-texture "already tracked in this command buffer" epoch, the same MRU idea as ResSet |
| 6% | `setBytes` ring: an objc_retain per call. Fix: return the ring chunk unretained (the command buffer's `_scratch` holds it) |
| ~3% | ResSet `addObject` of the pipeline on every draw |

### Stage 2 — descriptor-buffer model behind `NVMTL_DESC_MODEL=buffer`

- **2a.** Per-pipeline compact layouts and the binding plan, plus slot-table descriptor bytes (cached at
  view/sampler/texel-view creation), shared by both models.
- **2b.** Descriptor ring per command buffer, offsets per dirty stage, address table as a plain UBO descriptor.
- **2c.** Bindless heap as a descriptor buffer; framebuffer fetch; every internal pipeline. Verify no LEGACY layout
  remains (assert in layout creation).

Check:

- `mtl_bind` byte-identical images in every mode;
- `mtl_tests`;
- Geekbench Metal;
- the CMDCENSUS/HITCH counters;
- a run with ANV's SBA counted (or `INTEL_DEBUG=bat` on a short capture) to prove no mode switches.

### Stage 3 — default on

Make the buffer model the default on ANV after Minecraft/Geekbench soak runs. Keep classic for NVK and as a fallback.

### Stage 4 — translator-side

- Address-table pointer in a push constant (BDA) instead of a UBO descriptor.
- Consider gpuResourceID writes for argument buffers going straight into the heap (no per-draw embedded binding).
- Evaluate EXT_descriptor_heap once the buffer model is the default.

## 6. Stage 1 results

Encode ns per draw, p50 over 30 frames. Both sides are loaded with `NVMTL_BUNDLE` in the same session.

| mode | desc0 (fix24b) | desc1 | change |
|---|---:|---:|---:|
| none | 537 | 276 | −49% |
| bytes | 1025 | 676 | −34% |
| vbo | 917 | 589 | −36% |
| tex | 1327 | 1013 | −24% |
| samp | 1703 | 858 | −50% |
| gl | 3167 | 1685 | −47% |
| argbuf | 1111 | 828 | −25% |
| pso | 1434 | 1090 | −24% |
| cdisp | 1378 | 1223 | −11% |

Correctness:

- All 9 output files (8 render images, 1024² RGBA8, and the compute output) are byte-identical between desc0 and desc1.
- `mtl_tests` (vadd, reduce, math, texture, blit, render, perf) passes on both, with the same FMA GFLOPS and copy GB/s.
- No iGPU Metal device is available on the host for a reference image, so the reference is the shipping driver's output.

After stage 1, the gl draw's remaining CPU splits as:

| share | where |
|---:|---|
| 27% | descriptor set build (stage 2) |
| ~10% | ANV flush (stage 2) |
| 24% | `setFragmentTexture` (stage 1b) |
| 8% | `setBytes` (stage 1b) |
| 8% | `setSamplerState` ResSet/retains |

## 7. Files and merge

- `stage1-front-end.patch`: stage 1 as a unified diff against `src/nullmoth/plugin` (NVMTLObjects.h/.m,
  NVMTLEmbedded.m, nvmtl_vk.c; about 100 lines). NVMTLDevice.m is untouched, so it merges cleanly with the fix25 work
  there.
- `tests/mtl_bind.m`: the benchmark.
- `tools/build_desc.sh`: the build script.

Staged on the host:

| path | sha256 prefix |
|---|---|
| `~/b580/desc1/NVMTLDriver.bundle` (NVMTLDriver) | `971a485ff235` |
| `~/b580/desc1/mtl_bind` | `86eabf7b2029` |
| `~/b580/desc0` | baseline (= installed fix24b, `710e621f2d2a`) |

Nothing is installed.

Note: after the host reboot at 12:47 on 2026-10-29, the first `mtl_bind` run hung in `-[NVMTLDevice
initWithAcceleratorPort:]`. That run used desc0, which is identical to the installed driver. The process sits in state U
and survives `kill -9`. All the numbers above were taken before the reboot.

## 8. fix26 = fix25 + stage 1 (2026-10-29 13:10)

**Source.** `src/nullmoth-desc/plugin` was refreshed from `src/nullmoth/plugin` (fix25) and then
`stage1-front-end.patch` was applied. The old desc1 copy is kept in `plugin-stage1-old/`.

- Patch applied cleanly.
- The 4 patched files are identical to the desc1 versions; NVMTLDevice.m is fix25's.
- The fix25 source reproduces fix25's unsigned binary (`4ab548ccd8b7`).

**Build.**

- Script: `scratchpad/fix2/build_plugin_fix26.sh`. It is fix25's script with the plugin source path changed, so the
  signing scheme is the same.
- Output: `src/nullmoth/build-b580-fix26`.
- Bundle NVMTLDriver: `2b2b052e31e3`. The translator dylib and air-runtime are identical to fix25's.
- Staged at `~/b580/fix26`.

**Host results (after the 13:02 boot).**

`mtl_bind`, encode ns per draw, fix25 → fix26:

| mode | fix25 | fix26 |
|---|---:|---:|
| none | 532 | 278 |
| bytes | 1051 | 660 |
| vbo | 893 | 565 |
| tex | 1485 | 1114 |
| samp | 1846 | 960 |
| gl | 3340 | 1776 |
| argbuf | 1199 | 898 |
| pso | 1423 | 1104 |
| cdisp | 1812 | 1588 |

Checks:

- All 9 outputs are byte-identical between fix25 and fix26.
- `mtl_tests` passes on both.
- regtest (before and late): the fix26 image registers the AV1 decoder (`intelxedec: AV1 decoder REGISTERED`, image UUID
  14FD9A27) and `VTIsHardwareDecodeSupported('av01') = 1`.

## 9. Stage 2: the descriptor-buffer model (2026-10-29, on sv8)

**Tree.** `src/nullmoth-desc2`:

- `plugin/` = sv8 source (`nullmoth-gpuperf/plugin-p8`, which reproduces sv8's unsigned binary `870bfe3d5b34`) + `stage2-dbuf.patch`.
- `plugin.sv8/` is the untouched copy.

**Patch.** `stage2-dbuf.patch`: `nvmtl_dbuf.c` (new) + hooks in `nvmtl_vk.c`/`.h`, 769+/47−. It applies cleanly to plugin-p8.

**Switch.** `NVMTL_DESC_MODEL=buffer`. Default off: without it the device does not even enable `VK_EXT_descriptor_buffer`, and classic behaviour and speed equal sv8.

### What it does

These are §4 of this file, implemented.

- **Device:** probes `VK_EXT_descriptor_buffer` and enables the extension and `descriptorBuffer` only in buffer mode. It takes descriptor sizes from the properties, using the robust sizes when robustBufferAccess is on.
- **Layouts:**
  - Each pipeline stage gets a compact descriptor-buffer set layout of the bindings it reads (`nvmtl_db_plan`), cached by binding set. Typically 1–4 bindings, 64–192 bytes.
  - When the masks are unknown (tessellation), the layout holds all bindings.
  - A compute pipeline's set 1 is an empty layout, unless its module names set 1.
  - The heap, fetch and empty layouts carry the descriptor-buffer flag.
  - Every pipeline is created with `VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT`, including the variants, the align-guard SAFE modules and the visibility sum.
- **No mixing:** in buffer mode no `VkDescriptorPool`, `VkDescriptorSet`, `vkUpdateDescriptorSets` or `vkCmdBindDescriptorSets` is used anywhere. ANV therefore stays in BUFFER binding mode and never emits a mode-switch SBA/stall.
- **Per command buffer:**
  - A ring of 256 KiB descriptor-buffer pages (device-local, host-visible, dynamic-visible memory type). Pages are kept and reused under the same witness verdict as the address-table pages.
  - A host shadow of every slot's descriptor, regenerated with `vkGetDescriptorEXT` only for slots that changed.
  - A stage gets a new region only when its bindings or plan changed.
  - A 4-entry MRU returns a stage to an earlier region when pipelines alternate.
  - Each bind reserves its worst case first, so all regions of one draw land in the bound page.
- **Address table:** binding 640 is a plain UBO (or SSBO) descriptor. Its 64 bytes are made once per table slot of each page and cached with the page.
- **Bindless heap:** an 8.06 MB descriptor buffer bound at index 1, written by `bindless_put` and `sampler_slot`. Slot quarantine is unchanged.
- **Framebuffer fetch:** one region per fetch draw. The "fetch off" region is cached per page.
- **Texel buffers:** descriptors are made from address, range and format. `texel_view_create` and `destroy` record views in a small table.

### Fixes that also apply to the classic model

- `chg[2][4]` → `chg[2][6]`. The old array was 4 words for 321 binding indices, so a storage texture at index ≥ 256 (storage texture 63 and up) set a bit outside the array.
- `bind_buffer` marks the slot changed, not only dirty, when the buffer's address or size changes.
- The buffer-model entry points are `noinline`. Inlined, they slowed the classic `cmd_bind_set` by ~5% (bytes/tex).

### Results

Host after the 16:55 boot; Mesa mesa-rel-pkg-av1brc (`89331507d9f7`). Encode ns per draw, p50 over 30 frames; each run is a separate process.

| mode | sv8 (installed) | desc2 classic | desc2 buffer | buffer vs sv8 |
|---|---:|---:|---:|---:|
| none | 284 | 283 | 269 | −5% |
| bytes | 686 | 691 | 696 | ≈0 |
| vbo | 596 | 593 | 550 | −8% |
| tex | 1049 | 1005 | 759 | −28% |
| samp | 876 | 852 | 606 | −31% |
| gl | 1723 | 1702 | 1471 | −15% |
| argbuf | 829 | 821 | 780 | −6% |
| pso | 1102 | 1105 | 1058 | −4% |
| cdisp | 1219 | 1205 | 987 | −19% |
| texbuf | 971 | 951 | 749 | −23% |
| stex | 2396 | 2351 | 1834 | −23% |
| fetch | 4778 | 4790 | 2791 | −42% |

`bytes` is at parity:

- Only the address table changes there, which the classic model already handled with a dynamic offset.
- What is left per draw is ANV's own: one surface state per set whose address moved, and the push-constant re-upload.
- Stage 4 (the table pointer in a push constant) removes the table descriptor entirely.

Correctness:

- All 12 `mtl_bind` outputs are byte-identical across sv8, desc2 classic and desc2 buffer (11 images plus the compute output).
  - texbuf, stex and fetch are new modes that cover texel buffers, storage textures and input attachments.
  - argbuf and the bindless-all texture path cover the heap.
- `mtl_tests` (vadd, reduce, math, texture, blit, render, perf, mip with 22 sampling variants) passes in both models.

**Not covered by tests:** tessellation (it uses the all-bindings layouts) and WindowServer / Core Animation. Run Minecraft and Geekbench with `NVMTL_DESC_MODEL=buffer` before it can become the default (stage 3).

## 10. On sv9, and stage 1b (2026-10-29 evening)

**desc2-sv9** = sv9 (`plugin-p9`, unsigned `45dcba1ad635`; installed signed `4670a099fb35`) + `stage2-dbuf.patch`. Its source is
`plugin9/`.

- Build: `build-b580-desc2-sv9`, `8cdb80e50259`.
- Staged: `~/b580/desc2b`, with a guarded `install.sh` (SHA256SUMS + sigcheck, `./previous` backup, `--undo`).
- Tessellation check: `mtl_bind tess` runs a compute pass for the per-patch factors, then `drawPatches` per draw with `setVertexBytes` and `setFragmentTexture`.
  - The output is identical across sv9, classic and buffer.
  - In buffer mode it costs 4823 → 3037 ns/draw.
  - It exercises the all-bindings layouts.
- `svimg` (VRAM-shared IOSurfaces, `surfvram=3`) passes on sv9 and on both models.

**Stage 1b** (`stage1b-frontend.patch`, on top of desc2-sv9; source `plugin9b/`, 82+/37−, NVMTLObjects.h/.m). It is always on: it is not a descriptor-model switch.

1. **Tagged resource-set membership.**
   - Each `NVMTLResSet` has an epoch, unique in the process. It is renewed under the set's lock by every removal, by `removeAllObjects` and by `nvmtlTakeAll`.
   - Textures, buffers, sampler states and render pipeline states carry the epoch of the set that last took them (`_rsTag`), stored under that set's lock.
   - `nvmtl_retain_resource` skips the set when the tag equals the current epoch. A resource can carry the current epoch only while it is a member.
   - Other classes, and any mismatch, take `-addObject:` as before.
   - The hot setters now go through `nvmtl_retain_resource`: vertex and fragment buffers, samplers, and the pipeline per draw.
2. **`nvmtl_surface_in_for_cb` returns at once for a texture that is not on an IOSurface.** Every step after it (VRAM-shared use, census, overwrites, pending writes, surface-in) only acted on IOSurface textures.
3. **No retain/release per bind.**
   - Unretained locals in the texture, buffer and sampler setters: the caller holds the object, then the command buffer's set does.
   - The `setBytes` ring chunk is returned unretained (`nvmtl_bytes_ring_at`; `cb->_scratch` holds it).
   - `_vbufAt`/`_fbufAt` are stored only when the buffer changes.

**Results.** Encode ns per draw:

| | none | bytes | vbo | tex | samp | gl | argbuf | pso | cdisp | texbuf | stex | fetch | tess |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| sv9 | 287 | 692 | 591 | 1018 | 861 | 1707 | 826 | 1096 | 1204 | 945 | 2355 | 4781 | 4823 |
| 1b classic | 275 | 663 | 572 | 829 | 826 | 1235 | 761 | 1073 | 1197 | 777 | 2072 | 4719 | 4608 |
| 1b + buffer | 264 | 658 | 541 | 562 | 570 | 993 | 707 | 1016 | 964 | 580 | 1547 | 2771 | 2806 |

- The GL-like draw is 1707 → 1235 ns with 1b alone, and 993 ns (−42%) with the buffer model as well.
- Against the original fix24b (3220 ns), that is −69%.

**Checks.** All 13 outputs are byte-identical to sv9 in both models; `mtl_tests` and `svimg` pass in both.

**Staged:** `~/b580/desc2b1` = sv9 + stage 2 + stage 1b, plugin `c9244b32e54f`, with a guarded `install.sh`.

## 11. From the Minecraft sample (GL render thread, desc2b1 installed, 2026-10-29 18:00)

Leaf time by library (5 s sample):

| library | share |
|---|---:|
| libobjc | 20.6% |
| AppleMetalOpenGLRenderer | 17.7% |
| NVMTLDriver | 14.1% |
| libvulkan_intel | 13.5% |

### Retain/release

666 samples of `objc_retain`/`objc_release` on the render thread.

Ours, about 620:

| samples | where | why |
|---:|---|---|
| ~200 | `drawIndexedPrimitives` | the index buffer retained twice (parameter and local) and released at the end; the release costs most because another thread touches that cache line (residency counters) |
| 110 | `nvmtl_bind_embedded` | inlined helpers retained the function |
| 77 | `nvmtl_retain_resource` | its command-buffer parameter was retained |
| ~125 | `setVertexBuffer` / `setFragmentBuffer` | strong stores into `_vbufAt`/`_fbufAt` |
| ~50 | `nvmtlBindSamplerPipeline` | the pipeline retained per draw |

Apple's GL (`setRenderUniformBuffers` retaining our buffers): 45.

### malloc

210 samples, nearly all Apple's: GL's `operator new` 106, and the `_Block_copy` of the completion handler GL adds per draw 63. Metal semantics make that copy unavoidable in `addCompletedHandler:`.

### Stage 1c (`stage1c-frontend.patch`, on desc2b1)

1. **No retain/release in the per-draw paths.** Index buffer, setter parameters, `nvmtl_retain_resource`, `bind_embedded`, `ps_bindfns`, `surface_in_for_cb` and the sampler-variant pipeline are all unretained. `_vbufAt`/`_fbufAt` are `__unsafe_unretained`: every buffer stored there is held by the command buffer (its resource set or `_scratch`) for longer than the encoder lives.
2. **The address table is kept as a per-stage shadow** (`tab`) in the binding table. Every slot write updates it: `bind_slot`, `bind_buffer`, `bind_buffer_offset`, and `bind_texture_view` for the texture's heap slot. A snapshot is then two memcpys and the 32 self-address entries, not a walk over 160 slots. `nvmtl_address_snapshot` was 194 samples (~5%) of Minecraft's render thread.

### `stage2b-model-switch.patch`

Selects the buffer model without environment variables:

- `/tmp/nvmtl-desc-buffer-<process name>`, e.g. `/tmp/nvmtl-desc-buffer-java`: that process only.
- `/tmp/nvmtl-desc-buffer`: every process started while it exists, except WindowServer, which needs its named file.
- `NVMTL_DESC_MODEL=buffer|classic` still decides when set.
- The files are checked like the other diagnostic flags (a regular file owned by root or the user), read once per process.

### Stage 4: designed, not done

The table's contents (buffer addresses per draw) are inherent to Metal's per-draw bindings, so moving the table's address into a push constant alone does not remove the snapshot.

**4a: the table address as a push constant** instead of binding 640.

- Removes the dynamic offset (classic) or the table's UBO descriptor (buffer model), plus ANV's surface state for it at every flush.
- Wrapper pass (`translator/wrapper`, next to `table_ubo.rs`):
  - Append a `ulong` member to the module's push-constant block, or create the block. Offsets: vertex 192, fragment 200, compute 48. The existing push-constant users are dispatch 0–48, tessellation control 0–96, sample positions 96–160 and stencil 160–164.
  - Load the table through `PhysicalStorageBuffer` from that member.
  - Name the member `nvmtl_table` so the plugin can tell.
- Plugin side:
  - Push ranges are per stage, merged with the existing ones.
  - Each draw pushes 8 bytes per stage.
  - Modules the pass declines keep binding 640.

**4b: the used table entries themselves as push constants**, e.g. 4 vertex + 3 fragment addresses = 56 bytes.

- Removes the table memory and the snapshot.
- Needs the pass to compact the constant indices it reads and record the index list in the module (member names `t<index>`). It must fall back to the table for dynamic indices and for the self-address entries.

### desc2c2 (2026-10-29 18:30)

desc2c2 = desc2b1 + `sv10-drawable-writer-vs-desc2b1.patch` + `stage1c-frontend.patch` + `stage2b-model-switch.patch`.

- desc2b1 + the sv10 patch alone rebuilds the installed sv10 exactly (`36b80552ca91`).
- desc2c2 plugin: `ddd1cffadc3a`. Staged at `~/b580/desc2c2` with a guarded `install.sh`; `--undo` restores sv10.

Encode ns per draw, p50 of 30 frames; host at surfvram 3; java/geekbench closed:

| mode | sv10 (installed) | desc2c2 classic | desc2c2 buffer |
|---|---:|---:|---:|
| none | 277 | 193 | 179 |
| bytes | 656 | 506 | 500 |
| vbo | 576 | 464 | 422 |
| tex | 850 | 675 | 419 |
| samp | 835 | 666 | 417 |
| gl | 1236 | 989 | 745 |
| argbuf | 759 | 612 | 551 |
| pso | 1091 | 939 | 883 |
| cdisp | 1257 | 1086 | 847 |
| texbuf | 808 | 621 | 436 |
| stex | 2108 | 1751 | 1258 |
| fetch | 4903 | 4533 | 2709 |
| tess | 4620 | 4487 | 2694 |
| glidx | 983 | 732 | 697 |

`glidx` is the Minecraft-like mode: indexed draws, vertex and uniform buffers per draw, the same atlas texture.

Checks:

- All 14 outputs are byte-identical to sv10 in both models.
- `mtl_tests` (vadd, reduce, math, texture, blit, render, mip) passes in both models.
- `svimg` (`~/b580/sv9`, level 3) passes in both models.
- `drawtest 120 1` and `120 0` pass in both models (VRAM backings adopted for the drawables, 119 fps).
- The switch: `/tmp/nvmtl-desc-buffer-mtl_bind` turned the buffer model on for `mtl_bind` (log: "BUFFER asked by /tmp/nvmtl-desc-buffer-<process>"); without the file the process was classic.

## 12. Stage 1d: per-draw overhead (2026-10-29 evening; NOT staged, NOT installed)

Tree `plugin12/` = desc2c2 (`plugin11/`, rebuilds `ddd1cffadc3a` exactly) + `stage1d-perf.patch` (209+/43−). Build:
`SRC=plugin12 OUT=… tools/build_src.sh <name>`; latest `build-b580-d3d` (`62a76edde18f`).

**Root cause found: the write-combining drain.** ANV puts batch, surface-state, binding-table and push-constant memory in
VRAM through the BAR (WC) on a dGPU (`anv_device_get_pat_entry`, no `NO_LOCAL_MEM`). The first locked instruction after a
draw waits for those WC buffers: `mtl_bind_at` measured 350 TSC ticks (~105 ns) for the first atomic after a draw and 31 for
the next one. Whatever atomic came first paid it: the sampler-state retain in `setFragmentSamplerState` (~17% of a
glidx draw), the fragment-function retain/release in `nvmtlBindSamplerPipeline` (buffer model: after the descriptor-ring
writes, a second drain per draw), the `lock inc` of the enc78 statistics counters in `nvmtl_snapshot_set_c`, and in
Apple GL the mutex of `addCompletedHandler:` (GL adds one handler per draw; that one cannot be removed). The plugin's
own host memory (pool type 2) is cached (memprobe: 37 GB/s reads).

**Changes** (all in `stage1d-perf.patch`):
1. No atomics in the per-draw path: `_appSamp` unretained; fragment/vertex function passed as plain pointers;
   enc78 counters only with `NVMTL_ENC78_CENSUS`; setBytes ring chunk unretained.
2. Address-table trimming: `nvmtl_spirv_tabneed` scans each stage module (after address lowering) for constant-index
   reads of binding 640; a snapshot copies only those entries. Anything else (dynamic index, self address, pointer
   escaping) copies the whole table. Region/set reuse requires the old table to cover the new need. `NVMTL_TABLE_FULL=1`
   turns it off. Checked on the host against 115 real lowered modules (x86_64 harness + spirv-dis): 0 mismatches.
3. Vertex/index buffer bind cache per VkCommandBuffer (`vbc_*`, `ibc_*` in nvk_cmdbuf); `NVMTL_BINDCACHE_OFF=1` turns it off.
4. `nvmtl_poll_ns` without the 64-bit divide; compute `setSamplerState:` builds no NSString key for plain samplers.

**Results** (`mtl_bind`, ns/draw p50, desc2c2 → d3c; all 14 outputs byte-identical in both models):

| | none | bytes | gl | glidx | samp | pso | cdisp |
|---|---:|---:|---:|---:|---:|---:|---:|
| classic | 197→135 | 521→396 | 987→875 | 750→564 | 675→534 | 934→920 | 1097→1067 |
| buffer | 191→126 | 529→359 | 771→654 | 737→526 | 433→283 | 916→841 | 874→854 |

`gl_bind` (new: Apple GL 4.1, 2000 draws/frame, through `nvmtl_interpose.dylib` so a staged plugin runs without install):
mix 1707→1598 ns/draw (260→276 fps) classic, 1657→1547 (270→288 fps) buffer.

**GL difference: not reproducible.** On kernel rb2 (cross-process isolation fixes), desc2c2 gives the same GL hashes in
two runs, and d3c and d3d (= desc2d) give the same hashes as desc2c2 in both models. The pre-reboot images are gone.
The 21:31 difference came from the system state at the time, not from these changes.

Final checks on rb2 (debug.intelxe.stats bans 0 / GuC context resets 4 / GT resets 0 before and after every run):
- mtl_bind: all 14 modes byte-identical to desc2c2 in both models;
- gl_bind: all 4 modes byte-identical in both models;
- mtl_tests: ALL PASSED in both models (the staged bundle was loaded; its log shows the stage 1d lines).

**Staged:** `~/b580/desc2d` (host copy `stage/desc2d`). Plugin `62a76edde18f`, ad-hoc signed. install.sh with `--undo`
(restores the bundle it backs up, i.e. desc2c2), SHA256SUMS (24 files), sigcheck. Not installed.

## 13. Stage 2 made ready for the default (2026-11-12, branch `descmodel`; built, not run on the B580)

Every Metal binding path was read against the classic model to find what the buffer model (`NVMTL_DESC_MODEL=buffer`)
still lacked. Five gaps were found; all are closed. The default stays classic until a night run on the B580 meets the
criteria at the end of this section.

### What was missing, and what was done

1. **Heap bindings 2–4.**
   - The classic heap has five bindings: sampled images, samplers, storage images, uniform texel buffers and storage
     texel buffers. The buffer model's heap had only the first two.
   - Effect: every pipeline that indexes heap bindings 2–4 was refused (`nvmtl_heap_binding_refused`). These are kernels
     that write a texture they reach through an argument buffer, and shaders that read a texture buffer from one.
     Cyberpunk had 621 such modules. Separately, `nvmtl_vk_bindless_put_storage` and `_put_texel` tested `g_heapSet`,
     which the buffer model never has, so writable textures and texture buffers got no heap slot.
   - Now: the descriptor-buffer heap has the same five bindings, about 32 MiB (4 × 131072 × 64 B, plus 2048 × 32 B). The
     classic model's pool holds the same descriptors.
   - It has the same fallback (5, then 2, then 1 binding) and the same prefill (`NVMTL_HEAP_PREFILL`). Dropping a slot
     writes the dummies into bindings 2–4, as 5228d78 does in the classic model.
2. **table-pc, cs-table-pc and GL-UBO.**
   - Their switches required the classic model (`!g_dbuf`). The classic model has had them on by default on Intel since
     prod7/prod9; GL-UBO alone cut Minecraft's GPU frame time by 13%.
   - So the A/B of section 10 compared the buffer model without them against a classic model that, today, has them.
     Switching the default would have lost them.
   - Now they are on in both models. `nvmtl_db_cmd_bind_set` pushes the table entries (`nvmtl_push_table_entries`) and
     makes the GL-UBO pick.
   - A draw whose binding breaks a GL-UBO promise runs the pipeline's twin. The twin has its own stage layouts and
     pipeline layout, so the regions, offsets and pushes follow the pipeline that is bound.
3. **GPU-encoded ICBs (`NVMTL_ICB_DGC=1`).**
   - In the buffer model they offered no buffer index. Without table-pc there were no push-constant tokens, and the
     compute DGC dispatch returned -1.
   - Now the plans are made as in the classic model. The compute dispatch binds through `nvmtl_db_bind_compute`, and the
     offsets are set again after each execute.
   - ANV's DGC handles the buffer binding mode (`anv_dgc_layout.c` 406–411: the descriptor-buffer address and offset per
     set).
4. **Region reuse.**
   - Any slot change made the stage's next draw write a new region: an epoch bump, a copy of every descriptor of the
     plan, an offsets call and one new ANV surface state per set.
   - With table-pc, a GL draw's `setVertexBytes` changes only a pushed entry, yet each draw still paid all of that.
   - Now: a change that touches no slot of the current region's plan, and none of its address-table entries, keeps the
     region (`nvmtl_dbuf_reuse.h`, "keep").
   - The decisions are pure functions. The host model `user/tests/metal/dbuf_model.c` runs them against random binds,
     pipelines, page changes and destroy-and-remake at the same address, and checks after every draw that the region
     holds what the slots name.
     - Default (keep on) and with keep off (`NVMTL_DB_KEEP=0`): 1.36 M draws, 0 failures.
     - Two deliberately broken variants (a kept region's table ignored, its slots ignored): 6677 and 533 failures.
   - `NVMTL_DB_KEEP=0` restores the old behaviour.
5. **Proof of no mode switch.**
   - Stage 2c asked for proof that the buffer model never mixes in a classic call (ANV: a stall and a
     STATE_BASE_ADDRESS per switch).
   - In the buffer model, `vkCmdBindDescriptorSets`, `vkUpdateDescriptorSets` and `vkAllocateDescriptorSets` are now
     wrapped. They call through, count, and log the first calls as `LEGACY`. A night log with none is the proof (in place
     of `INTEL_DEBUG=bat`).

Smaller fix: a texel descriptor for a view the plugin does not know is now zero bytes when there are no null
descriptors. Before, `vkGetDescriptorEXT` was called with a NULL `pUniformTexelBuffer`, which is invalid without the
`nullDescriptor` feature.

### The binding paths

Each path, how the buffer model carries it, and which test case covers it:

| Metal | in the buffer model | test |
|---|---|---|
| vertex / fragment buffers, offsets, `setBytes` | slot → address table, or a pushed entry (table-pc), or an SSBO descriptor in the stage region; the bytes ring is a buffer slot | `vtx-buf-bytes`, `frag-bytes`, `bytes-4k`, `high-slots`, `churn`; `mtl_desc_cost` bufoff/small |
| textures, samplers, texel buffers, storage textures | slot → descriptor bytes made once per change (`nvmtl_db_gen`), copied into the region | `frag-tex-samp`, `vtx-tex`, `high-slots` (texture 30, sampler 15), `cs-tex`, `cs-stex-high` (storage texture 70: bindings past 255), `texel-buf` |
| texture views (mip, slice, format, swizzle) | each view is its own VkImageView; a destroyed one bumps the object generation, so every shadow is made again | `tex-views` |
| pipelines with different bindings | stage layouts per binding set; regions per plan, MRU of 4; keep | `pso-switch`, `churn`, `dbuf_model` |
| compute | the same stages on the compute bind point; cs-table-pc pushes | `cs-*`, `indirect`, `mtl_desc_cost` compute |
| mesh / object | compute emulation (lowered kernels) + a draw: the paths above | `mesh` (where the device has Metal 3) |
| tile | not implemented by the plugin (the encoder calls are no-ops) | `tile`: SKIP on the B580, the Apple reference runs it |
| argument buffers tier 1 (encoder) | an argument buffer is a buffer slot; its textures and samplers are heap slots (gpuResourceID) | `argbuf-t1` (fragment and kernel) |
| argument buffers tier 2 (CPU-written) | the same, written by the application | `argbuf-t2`, `argbuf-array` (array<texture2d, 8> by index), `argbuf-samplers` (two samplers, clamp vs repeat) |
| writable textures / texture buffers in argument buffers | heap bindings 2 / 3–4 (new in this model) | `argbuf-storage`, `argbuf-texbuf` |
| `useResource` / `useHeap` | residency only (resource sets); no descriptor work in either model | `use-heap` (textures in an MTLHeap, reached only through an argument buffer), `use-resource` (a buffer written through a pointer) |
| indirect draws and dispatches | the same binds as direct ones | `indirect` |
| ICBs, CPU-encoded | replayed through the encoder's paths | `icb-render`, `icb-compute` |
| ICBs, GPU-encoded (DGC) | push-constant tokens over the table-pc slots; offsets re-set after the execute | `mtl_gpu_icb2` with `NVMTL_ICB_DGC=1` |
| framebuffer fetch, tessellation | the fetch region per draw; all-bindings layouts | `mtl_bind` fetch / tess |

### Counters: `NVMTL_DESC_STATS=1`

The counters are the same in both models, so a night run compares them directly (`nvmtl_desc_stats.h`):

- **Draws and dispatches bound.**
- **Host time spent binding them:** `nvmtl_vk_cmd_bind_set` and the compute binds, measured with `mach_absolute_time`.
  In both models this covers building the sets or regions, the address table, the pushed entries and the Vulkan calls
  made for them.
- **Classic:** sets made and reused, `vkUpdateDescriptorSets` calls with their writes and copies, allocations,
  `vkCmdBindDescriptorSets` calls.
- **Buffer:** regions written, reused and kept, descriptors made, bytes written, offsets calls, descriptor-buffer binds.
- **Both:** address tables, `vkCmdPushConstants` calls, legacy calls, and binds whose draw was not recorded.

How they are kept:

- Each thread adds to its own block with plain adds. There is no locked instruction, so counting does not pay the
  write-combining drain of section 12.
- The blocks are summed for a `DESCSTAT` line every 5 s of command-buffer completions, and a `DESCSTAT-TOTAL` line at
  exit.
- Each line ends with ratios per bind: sets, regions, tables and descriptor bytes.
- Off, each site is one test of a cached int.

The existing `NVMTL_DBUF_CENSUS=1` is unchanged.

### Tests

| test | what |
|---|---|
| `user/tests/metal/dbuf_model.c` | the host model of the region reuse above; `./dbuf_model fmt` checks the DESCSTAT line |
| `user/tests/metal/mtl_desc_paths.m` | 26 cases (the table above), exact expected values computed on the CPU, PASS/FAIL/SKIP per case |
| `user/tests/metal/mtl_desc_cost.m` | CPU cost per draw: 9 modes of small binds (same, small, bufoff, tex4, heavy, argbuf, pso, unread, compute); encode ns/draw p50, draws/s, process CPU per draw (every thread), image hash |
| `user/tests/plugin/night_desc.sh` | night phase D: all of the above plus `mtl_bind`, `mtl_tests`, `mtl_gpu_icb2` (DGC) and `gl_bind` in both models, the buffer model also with keep off and with table-pc off, and Geekbench Metal per model with `DESC_GB=1` |

**Apple reference (M1 Pro, macOS 27, this build Mac):**

- `mtl_desc_paths --any`: 26 PASS, 0 FAIL, 0 SKIP, both arm64 and x86_64 (Rosetta). The B580 must give the same
  answers, except `tile` (SKIP).
- `mtl_desc_cost --any`, encode ns per draw:

| same | small | bufoff | tex4 | heavy | argbuf | pso | unread | compute |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 29 | 172 | 86 | 211 | 381 | 159 | 212 | 115 | 242 |

  Process CPU per draw over all modes is 231 ns. These numbers are Apple's driver on its own GPU: a target, not a
  reference to match.

**Builds:**

- The plugin builds with 69 warnings, as before, in both the diagnostic and the release (`NVMTL_RELEASE`) configuration.
- The x86_64 tests build with the build script's flags.

### Staged for the night run

`~/hackintosh/work/descm/stage` on the build Mac, to be copied to `~/b580/descm` on the B580 machine:

- `NVMTLDriver.bundle`, built by `build-intelxe.sh plugin` from this branch. Plugin `07df9c5b777c` (signed);
  translator `3f8d642be9c9`.
- `nvmtl_interpose.dylib`.
- x86_64 tests: `mtl_desc_paths`, `mtl_desc_cost`, `mtl_bind`, `mtl_tests`, `mtl_gpu_icb2`, `gl_bind`.
- `night_desc.sh`.

Nothing is installed. The night script sources `night_desc.sh` and calls `phase_desc`, or it runs alone with
`bash ~/b580/descm/night_desc.sh`. Geekbench needs `DESC_GB=1`.

### Criteria for making it the default (stage 3)

From one night run of `night_desc.sh`:

1. **Correctness:**
   - `mtl_desc_paths` all PASS in both models; only `tile` may SKIP, and `mesh` too when the device lacks it.
   - The same with `NVMTL_DB_KEEP=0`.
2. **No mode switch:** 0 `LEGACY` lines, and "0 legacy calls" in every buffer-model `DESCSTAT-TOTAL`.
3. **Byte-identical output:** every `mtl_bind` image identical between the models, and every `gl_bind` image too when it
   runs.
4. **Existing suites:**
   - `mtl_tests` ALL PASSED in both models.
   - `mtl_gpu_icb2` with `NVMTL_ICB_DGC=1`: the same PASS count in both models.
5. **CPU cost:** `mtl_desc_cost`, buffer against classic, best of two rounds:
   - no mode slower by more than 3%;
   - small, tex4, heavy and pso at least 15% faster (section 10 measured 25–40% on their counterparts);
   - the DESCSTAT ns per bind lower overall;
   - no draw not recorded.
6. **Geekbench Metal** (`DESC_GB=1`): the buffer model within 1% of classic or better.
7. **Health:** no GuC or GT reset during the phase, and no "no descriptor memory" line.

Then the switch:

- `nvmtl_db_wanted` returns 1 by default on Intel (vendor 0x8086) for every process except WindowServer. Classic stays
  for NVK and as the fallback, and `NVMTL_DESC_MODEL=classic` keeps it per process.
- WindowServer follows after a day of desktop use with `/tmp/nvmtl-desc-buffer-WindowServer`. That needs the plugin
  installed, which a night run does not do.

## 14. Geekbench's compute: a region per dispatch (2026-11-13, branch `dbufdispatch`; built, not run on the B580)

The night of 12 Nov ran Geekbench 6 Metal in both models. The buffer model scored **31,969** against classic's
**93,475**, although its CPU cost per bind was lower (365 ns against 901 ns). Its `DESCSTAT-TOTAL`:

| model | dispatches bound | ns per bind | per bind |
|---|---:|---:|---|
| classic | 184,933 | 901 | 0.85 sets, 0.67 tables |
| buffer | 185,029 | 365 | **0.97 regions**, 0.59 tables, 166 B |

### What the plugin did

- **The descriptor buffers are not bound again per dispatch.** `nvmtl_db_bind_buffers` calls
  `vkCmdBindDescriptorBuffersEXT` only when the ring moves to a new 256 KiB page. At 166 B a dispatch, that is once per
  ~1500 dispatches. ANV marks the buffers dirty only when an address changes (`anv_cmd_buffer.c` 746), and on Xe2 that
  emits no STATE_BASE_ADDRESS (`genX_cmd_buffer.c` 3528, GFX_VERx10 >= 125).
- **The offsets were set again for almost every dispatch**, because almost every dispatch had a new region. In ANV each
  `vkCmdSetDescriptorBufferOffsetsEXT` makes the next dispatch rebuild the push constants of every set. A set whose
  address moved also gets a new surface state (`update_descriptor_set_surface_state`, 3413). The GPU then reads that
  set's descriptors at a new address.
- **Root cause: the keep rule looked only at the current region.**
  - Geekbench's work is chains of kernels A, B, C, A ..., with `setBuffer` (new offsets) and `setBytes` before each
    dispatch.
  - Most of those kernels read their buffers from pushed entries (cs-table-pc), so their regions hold only textures.
  - A change kept the region the stage pointed at when the change touched nothing in it. The other recent regions (the
    MRU of 4) were dropped by the epoch bump.
  - So at B's dispatch, B's region from the round before was already stale: every dispatch wrote a new region, copied
    its descriptors, and set the offsets. `mtl_desc_cost compute` uses one kernel, so it never showed this; it was 12%
    faster in the buffer model.
- **The 166 B per bind** are those regions: one per dispatch, each a full copy of the kernel's descriptors (and its
  address table descriptor when it reads the table).
- **Kernels that read the address table** need a new table whenever a buffer they read changes. In the buffer model
  the table's descriptor lies inside the stage region, so that is a new region. In the classic model it is a new
  dynamic offset on the same set. That part is not changed here (see "Left").

### Fix

- `nvmtl_dbr_changed` (`nvmtl_dbuf_reuse.h`) now keeps **every** recent region of the page that was valid until the
  change and that the change does not touch (its slots, and its table entries when it has a table). Before, it kept
  only the current one.
- `NVMTL_DB_KEEP=cur` restores the 12 Nov rule, and `NVMTL_DB_KEEP=0` still turns keeping off.

### Host model (`dbuf_model.c`)

`./dbuf_model geek [steps] [seed] [variant] [tablekernels]` runs a Geekbench-like chain instead of random draws:

- four kernels take turns;
- before each dispatch, two `setBuffer`s at new offsets and a `setBytes` (a new ring offset);
- 1 time in 16, a new texture;
- now and then a page change or a new command buffer;
- `tablekernels` of the four read the table.

Results over 1 M dispatches, 0 failures in each run:

| rule | no table kernels | 1 of 4 | 4 of 4 |
|---|---:|---:|---:|
| keep (13 Nov, default) | **0.164** regions per bind | 0.656 | 1.000 |
| `NVMTL_DB_KEEP=cur` (12 Nov) | 1.000 | 1.000 | 1.000 |
| `NVMTL_DB_KEEP=0` | 1.000 | 1.000 | 1.000 |

- What remains with no table kernels is the texture changes and the page changes.
- The random-draw model still holds: 1.36 M draws, 0 failures with keep, keepcur and nokeep.
- New regions with keep: 1,075,253, against 1,132,191 with keepcur.
- The broken variants still fail (notable 33,056, noslots 2,881).

### `mtl_desc_cost geek`

The new mode `geek` is a three-kernel chain:

- `k_ga`: buffer to buffer;
- `k_gb`: also reads a texture;
- `k_gc`: threadgroup memory.

Each dispatch has `setBuffer` at new offsets for its input and output, plus `setBytes`. The mode prints an output
checksum, which must be the same in both models.

`night_desc.sh`:

- `geek` is in the D2 table, and its checksum is compared like an image hash.
- Phase D2b runs `geek` alone per model (classic, buffer, buffer with `NVMTL_DB_KEEP=cur`), so its `DESCSTAT` per-bind
  ratios are its own.
- Geekbench Metal (D7, `DESC_GB=1`) still runs in both models.

**Apple reference (M1 Pro, `--any`, x86_64 under Rosetta, 30 frames of 1000 dispatches):**

| mode | encode p50 (ns/dispatch) | process CPU (ns/dispatch) | checksum |
|---|---:|---:|---:|
| compute | 756 | 986 | – |
| geek | 1009 | 1166 | 174116 |

The checksum was the same in every run.

**Build:** the plugin builds with 69 warnings, as before, and the default model stays classic.

**Staged** in `~/hackintosh/work/night1214/descm2` (nothing installed):

- `NVMTLDriver.bundle`, built from this branch (build log: plugin `78b87bd86a31`, with the two diagnostics below);
- `nvmtl_interpose.dylib`, from `descm/stage`;
- `mtl_desc_cost`, `mtl_desc_paths`, `mtl_gpu_icb2` and `dbuf_model`, new builds;
- `mtl_bind`, `mtl_tests` and `gl_bind`, unchanged, from `descm/stage`;
- `night_desc.sh`.

### Diagnostics (from the ANV study, `DESIGN-anv-descbuf-cost.md`)

- **Memory type.** Each kind of descriptor buffer logs once where it lives: `vk: descriptor buffer of N bytes: memory
  type T (heap H), flags F = DEVICE-LOCAL | SYSTEM MEMORY`. The kinds are the heap (32 MiB), a 256 KiB page and a large
  page. Buffer mode is bindless and has no binding-table prefetch, so descriptors in system memory are read over PCIe.
- **Buffer binds.** `DESCSTAT`'s descriptor-buffer binds now count only `vkCmdBindDescriptorBuffersEXT` calls whose page
  address changed. Each such call makes ANV reload every bind point's push constants. A call at an unchanged address
  (ANV: a no-op) is counted apart, in the census (`NVMTL_DBUF_CENSUS=1`); by construction there should be none.
- **Region reuse also matters on the GPU.** Per the ANV study, a new region is a new descriptor address, which the
  state cache misses. That makes the keep fix a GPU fix as well as a CPU one.

### Table kernels: the address table in a set of its own (2026-11-13, branch `dbuftable`; built, not run on the B580)

The keep fix left one source of a region per dispatch: a stage that reads the address table. Every `setBuffer` offset
it sees needs a new table, and the table's descriptor was in the stage region, so the whole region was written again:
every texture, sampler and buffer descriptor of the plan copied to a new address that the GPU's state cache had never
seen. That is the "4 of 4" column above (1.000 regions per bind).

**Choice: a table set** (option a). The two options, against what ANV does (`DESIGN-anv-descbuf-cost.md`):

- **(a) the table's descriptor in a set of its own.** A new table is then a one-descriptor region and a new offset of
  that set. In ANV a changed offset dirties only the stages of that set's layout and makes their push constants again:
  no stall, no new descriptor-buffer bind, no mode switch. The stage region stays where it is, so its descriptors stay
  warm. The module change is one decoration (the table variable's `DescriptorSet`).
- **(b) the table's address in push constants.** cs-table-pc already pushes the entries it can; the stages that still
  read the table do so with dynamic indices, self addresses, table pointers passed on, or more entries than the push
  space holds. Pushing the table's address would turn the table from a uniform buffer (`nvmtl_table_ubo`, read through
  the constant path) into a `PhysicalStorageBuffer` pointer: every access chain and load of it rewritten, its reads
  becoming global memory loads. What it saves over (a) is the offsets call, and that call's ANV cost is the
  push-constant block, which a dispatch with `setBytes` or pushed entries makes again anyway.

So (a): the same kind of work as classic's dynamic offset, with a module change that cannot alter what the shader
computes.

**What it does:**

- **Sets.** Compute: set 1 (empty in every kernel that does not name it). Graphics: set 4 for the vertex stage and set 5
  for the fragment stage; sets 2 and 3 take the empty layout when there is no heap or no framebuffer fetch. Each table set
  has one binding (640, the table's type) visible to its stage only (`nvmtl_db_table_plan`), so a new vertex table does
  not make the fragment push constants again.
- **Pipelines.** A stage whose bindings are known and that reads binding 640 has its table variable moved to the table
  set (`nvmtl_spirv_retarget_table`: only the `DescriptorSet` decoration of the Binding 640 variables, and only when each
  has one, in the expected set; else nothing changes). Its stage layout leaves the table out. Tessellation (bindings
  unknown) and kernels that name set 1 keep the table in the region, as before.
- **Binds.** After the stage's region, `nvmtl_db_table_set`: the table the stage points at is kept while no change
  touches an entry it holds (buffer entries 0..nb and texture entries 0..nt of its need), the page is the same and no view
  or sampler was destroyed (`nvmtl_dbr_tab_*` in `nvmtl_dbuf_reuse.h`). Otherwise a new table is snapshotted into the
  address page as before; its descriptor (made once per page and table) is copied into a region of the table set, and
  that set's offset is set.
- A region without a table now serves any table need, so two pipelines with the same plan and different needs share it.
- `NVMTL_DB_TABSET=0` keeps every table in its stage region (the rule of the morning of 13 Nov).

**Host model** (`dbuf_model geek`, 1 M dispatches, seed 1, 0 failures in each run):

| rule | no table kernels | 1 of 4 | 4 of 4 |
|---|---:|---:|---:|
| keep + table set (default) | **0.164** regions per bind | **0.164** | **0.165** |
| keep, `NVMTL_DB_TABSET=0` (13 Nov morning) | 0.164 | 0.656 | 1.000 |
| `NVMTL_DB_KEEP=cur` | 1.000 | 1.000 | 1.000 |
| `NVMTL_DB_KEEP=0` | 1.000 | 1.000 | 1.000 |

- With the table set, the tables per bind are 0, 0.250 and 1.000: one per dispatch of a table-reading kernel, as
  classic's dynamic offset.
- What remains of the regions is what remains with no table kernels: texture changes (1 in 16 dispatches) and page and
  command-buffer changes.
- 4 M dispatches (seed 7) with 4 of 4 and 2 of 4: 0.163 regions per bind, 0 failures; seeds 2 and 3, 2 M each: 0.164 and
  0.163.

The random-draw model now gives three plans in four a table, most of them in the table set and the rest in the region
(the stages the plugin leaves there):

- 2 M steps, 1.36 M draws: 0 failures with keep, keepcur, nokeep and intable (`NVMTL_DB_TABSET=0`), seeds 1, 2 and 3;
  under ASan and UBSan too.
- New regions with keep: 857,327, against 961,954 with intable; 386,351 tables in the table set, 293,612 reused.
- The broken variants still fail: notable 7,328, noslots 10,704. The new one, tabstale (the table set's table never
  renewed), fails 299,150 times, and 999,323 times in `geek` with 4 of 4.

**The module change, off the GPU.** 621 Geekbench Metal modules that declare the table (the translator's output in
`~/hackintosh/work/gbspv`) were run through `nvmtl_spirv_retarget_table` (set 0 to 1). All 621 were moved, each by
exactly one changed decoration, and `spirv-val --target-env vulkan1.3 --scalar-block-layout` passes before and after.
A module whose table is not in the expected set, or that has none, is left byte-for-byte unchanged.

**Apple reference** (M1 Pro, `--any`; these tests did not change, so the B580 must still give these answers):

- `mtl_desc_paths`: 26 PASS, 0 FAIL, 0 SKIP, x86_64 (Rosetta) and arm64.
- `mtl_desc_cost -f 30`, x86_64, encode p50 in ns per draw or dispatch:

| same | small | bufoff | tex4 | heavy | argbuf | pso | unread | compute | geek |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 80 | 390 | 199 | 421 | 994 | 264 | 351 | 238 | 304 | 367 |

  - Process CPU over all modes: 415 ns. The geek checksum is 174116, as in every earlier run.
  - arm64: compute 219 and geek 223 ns, checksum 174116.
  - `-n 4000 geek` (night phase D2b): 515 ns, checksum 321699.
- `mtl_bind igpu -f 20` (the binary of `descm/stage`): 14 modes; every image byte-identical between two runs.

**Night checks** (`night_desc.sh`, new runs):

- D1: `mtl_desc_paths` in the buffer model with table-pc off (`NVMTL_PC_TABLE=0 NVMTL_CS_PC_TABLE=0`: every stage that
  uses a buffer reads the table, so every case goes through a table set), and the same with `NVMTL_DB_TABSET=0`.
  - Pass: every case PASS (only `tile` may SKIP).
- D2b: `geek` with table-pc off in classic, buffer, and buffer with `NVMTL_DB_TABSET=0`. All three kernels then read the
  table, which is the "4 of 4" column.
  - Pass: the buffer model's `DESCSTAT-TOTAL` has regions per bind near 0 and tables per bind near 1, against about 1
    region per bind with `NVMTL_DB_TABSET=0`; the checksum is the same in all three.
- D3: `mtl_bind` images of the buffer model with table-pc off against classic.
  - Pass: byte-identical.
- Every run: 0 `LEGACY` lines. The summary counts the table-set layouts made ("table sets"; at most 3 per process).
- D7: Geekbench Metal's buffer-model `DESCSTAT` regions per bind, against 0.97 on 12 Nov.

**Build.** The plugin builds with 69 warnings, as before, in the diagnostic and the release (`NVMTL_RELEASE`)
configuration. The default model stays classic. The translator is unchanged on this branch. Its build here
(`b30b039c873a`) already has the descpaths fix (argbuf-array), so it differs from descm2's.

**Staged** in `~/hackintosh/work/night1214/descm3` (nothing installed; `SHA256SUMS.short` lists the files):

- `NVMTLDriver.bundle` from this branch: plugin `a2d12351860a`, translator `b30b039c873a`;
- `nvmtl_interpose.dylib` from `descm/stage`;
- `mtl_desc_cost`, `mtl_desc_paths`, `mtl_gpu_icb2` and `dbuf_model` (arm64, for the build Mac), new builds;
- `mtl_bind`, `mtl_tests` and `gl_bind`, unchanged, from `descm/stage`;
- `night_desc.sh`.

### Left

- **ANV's cost of an offsets call.** A new surface state per moved set, and the push constants made again. The other
  branch (`anvdbuf`) studies it; `mesa/patches/0105` drops the surface state on LSC.
- **Stages that keep the table in the region:** tessellation, and kernels that name set 1. For those, a region per
  changed table remains.
- **How many of Geekbench's binds read a table.** Shown by the next night's D7 `DESCSTAT`: tables per bind against
  regions per bind.
