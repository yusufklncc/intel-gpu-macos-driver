# GPU-encoded indirect command buffers through VK_EXT_device_generated_commands

> Design note with its date (2026-11-12): it records the plan and the state of that day, not the current status. The
> current status is in `docs/ROADMAP.md` ("GPU-encoded indirect command buffers").

What this covers: the Metal ICB commands a shader can encode beyond `draw_primitives` / `draw_indexed_primitives` /
`reset` (done since 10-11 Nov, `plugin/nvmtl_icb_layout.h`, translator `native/render_command.rs`), and the conditions the
classic execute still imposes on those draws. Everything here is behind **`NVMTL_ICB_DGC=1`** (default off). With the knob
off the translator's output, the record layout, the device's extensions and the execute are exactly as before.

Code: translator `native/render_command/dgc.rs`; plugin `nvmtl_icb_layout.h` (record), `nvmtl_icb_dgc.h/.c` (Vulkan side,
included at the end of `nvmtl_vk.c`), `NVMTLObjects.m` (`nvmtlWriteDGCRecord:`, `nvmtlExecuteDGC:`,
`nvmtlExecuteRecordsCompute:`); test `user/tests/metal/mtl_gpu_icb2.m`.

## 1. What ANV gives us on Xe2

From our Mesa tree (`~/hackintosh/work/mesa014`, ANV 27.0.0-devel):

| fact | source |
|---|---|
| `VK_EXT_device_generated_commands` for `verx10 >= 125` (so the B580); `vkcaps` confirmed it on the card 12 Nov | `anv_physical_device.c:580` |
| `maxIndirectCommandsTokenCount` 32, `maxIndirectCommandsTokenOffset` 64 KiB, `maxIndirectCommandsIndirectStride` UINT32_MAX, `maxIndirectSequenceCount` 2^20 | `anv_physical_device.c:2128-2133` |
| input modes: Vulkan and DXGI index buffers | `anv_physical_device.c:2134` |
| **pipeline / shader binding (indirect execution sets) only for compute and ray tracing**: `supportedIndirectCommandsShaderStagesPipelineBinding = stages & (COMPUTE \| RT)` | `anv_physical_device.c:2135-2138` |
| no `DRAW_COUNT` tokens (`deviceGeneratedCommandsMultiDrawIndirectCount = false`: Xe2 has no hardware gl_DrawID for it) | `anv_physical_device.c:2140-2143` |
| at most 32 push-constant entries and 31 vertex buffers per layout | `anv_types.h:354` (`entries[32]`), `:399` (`buffers[31]`) |
| per sequence: `3DSTATE_INDEX_BUFFER` from a `VkBindIndexBufferIndirectCommandEXT` (address, size, type), `VERTEX_BUFFER_STATE` from a `VkBindVertexBufferIndirectCommandEXT` (address, size, **stride from the data**), push constants copied over the CPU's push constants (the CPU values are the template), then `3DPRIMITIVE` | `shaders/dgc.cl:394-600` |
| without explicit preprocessing the execute runs ANV's generation shader (a "simple shader" fragment dispatch, one invocation per sequence) inside `vkCmdExecuteGeneratedCommandsEXT`, then a data-cache flush + CS stall, then jumps into the generated batch | `genX_cmd_dgc.c:53-176, 876-1000` |
| sequences run in the order of the indirect buffer; ANV does not look at `UNORDERED_SEQUENCES` | no reference in `src/intel` |
| after the execute ANV marks the index buffer, vertex buffers and push constants dirty and re-emits its own state | `genX_cmd_dgc.c:1045-1052` |
| preprocess memory: prolog + epilog + `maxSequenceCount * cmd_size` + `maxSequenceCount * data_size` (data includes a full `anv_push_constants` per sequence), alignment 64, default buffer memory types on Gfx12.5+ | `anv_dgc_layout.c:351-375` |

Consequences:

- **Index buffer by address works** (index buffer token). Topology still cannot come from GPU memory (no token): the CPU
  keeps choosing it, as in the classic execute.
- **Vertex buffers and buffer addresses work**: vertex fetch through vertex buffer tokens; a stage's buffer addresses
  through push-constant tokens where the pipeline reads them from push constants (table-pc, see 3).
- **A render pipeline cannot be switched from GPU memory on ANV.** Compute pipelines could be (indirect execution sets),
  but see 5.
- No barrier token exists in the extension.

## 2. Record layout

The classic 64-byte record stays as it is (words 0..15). With the knob every GPU-encodable ICB gets **1024-byte records**
whose first 64 bytes are the classic record; the translator addresses a slot as `address + slot * 1024` (shift 8 words
instead of 4), so both sides read the same knob. The rest (`nvmtl_icb_dgc_record`):

| words | who | content |
|---|---|---|
| 16..19 | CPU+GPU | `VkBindIndexBufferIndirectCommandEXT` (address lo/hi, size, VkIndexType) |
| 20 | CPU | mode: bit 0 = this ICB's render executes run through DGC (per ICB: the device has DGC) |
| 21 / 22 | CPU | vertex / fragment-or-kernel buffer indices a shader may set (bit m), from the plan of the pipeline the execute binds |
| 23 | GPU | sticky refusal: a state command the execute cannot express was encoded into this slot (reset clears it) |
| 24,25 / 26,27 | CPU / GPU | resource ID of the pipeline the execute binds / the one a shader set |
| 28..30 | CPU | address and element count of the identity index buffer |
| 31 | GPU | barrier flag (diagnostic) |
| 32..34 | CPU+GPU | `VkDispatchIndirectCommand` |
| 35..37 / 38..40 | GPU / CPU | threads per threadgroup a shader asked for / the execute's |
| 41..47 | GPU | scratch: where a refused buffer store lands (no branches in the lowering) |
| 48 + 4m | CPU+GPU | vertex buffer m (0..30): `VkBindVertexBufferIndirectCommandEXT` |
| 172 + 2m | CPU+GPU | fragment (render) or kernel (compute) buffer m: its address |

A DGC sequence is one record: `indirectAddress = records + first slot * 1024`, `indirectStride` 1024. Token offsets are
fixed per word; which tokens a layout has depends on the pipeline (3). The CPU writes the whole record when the app encodes
the slot (as before: last writer wins); the words it owns that depend on the pipeline the execute binds (topology, masks,
pipeline ID, static strides, threadgroup size) are written back by each execute, before the command buffer runs, so a
shader encoding into the slot later in that command buffer checks against them (the mechanism word 11 already uses).

Why one record instead of a parallel DGC stream: the shader only has the `command_buffer` value (one address), so a
second stream would need its address in the record anyway; a fixed 1024-byte stride keeps the lowering a shift and the
classic execute working unchanged on the same records (it passes `stride` 1024 to `vkCmdDraw*Indirect`). Cost: 1 KiB per
slot (a 4096-slot ICB is 4 MiB, in VRAM through the BAR like the classic records).

## 3. Command by command

### Render

| Metal (AIR) | lowering (knob on) | execute |
|---|---|---|
| `draw_indexed_primitives` (`.p1i16/.p1i32`) | DGC mode: index buffer token = the pointer (must be non-zero and index-aligned), size `count << shift`, first index 0; counts gated by topology (word 11) and the sticky word. Classic mode: the classic checks. | indexed draw token |
| `draw_primitives` | DGC mode: an indexed draw through the **identity index buffer** (uint32 0..n-1, first index = first vertex, vertex offset 0: `vertex_id` is unchanged): one token layout serves every slot of a run, so runs stay in order. Refused if first vertex + count > n (default 2M, `NVMTL_ICB_DGC_IOTA`). | same token |
| `reset` | kind 0, counts 0, sticky and barrier cleared | — |
| `set_vertex_buffer(ptr, [stride,] index)` | stored in entry `index` when bit `index` of word 21 is set and the address is non-zero; a stride other than -1 replaces the entry's stride; else the slot is refused | vertex buffer token (binding m) if the pipeline fetches from m; push-constant token (VS, `8k`) if the vertex stage reads its address from push slot k |
| `set_fragment_buffer(ptr, index)` | same with word 22 | push-constant token (FS, `176 + 8k`) |
| `set_render_pipeline_state(pso)` | accepted when the ID equals word 24/25 (the pipeline the execute binds: the slot's CPU pipeline or the encoder's), else the slot is refused | — (ANV has no graphics indirect execution sets) |
| `set_barrier` | **refused** (sticky) | — |
| `clear_barrier` | accepted (no barrier is what the execute does) | — |
| `set_object_buffer`, mesh draws, `copy_command` | not lowered (refused as before) | |

Which buffer indices a shader may set (words 21/22, `nvmtl_vk_icb_dgc_plan_gfx`):

- only in an ICB that does not inherit buffers (Metal allows `set_*_buffer` only there; the descriptor's `inheritBuffers`
  is now kept on the ICB) and in the classic descriptor model (`NVMTL_DESC_MODEL=buffer` has no table-pc);
- for each stage and index m: the stage does not read address-table entry m at all (`tabneed`), or reads it through its
  pushed table (table-pc plan entry k), but not through a GL-UBO 64-aligned block load (its promise is checked on the CPU
  per draw). A stage that reads entry m from the descriptor table cannot see a per-sequence value: m is not offered;
- vertex fetch: every binding the pipeline fetches from gets a token, or none does (then the CPU binds them as before and
  those indices are not offered);
- at most 32 tokens (index buffer + draw + vertex buffers + push slots); past that only indices no stage reads are offered.

The plan is cached by what it is made of (pipeline layout + token list), never by a pointer. Its
`VkIndirectCommandsLayoutEXT` has `indirectStride` 1024, shader stages VERTEX|FRAGMENT, the pipeline's layout (push
constant tokens need it), no `UNORDERED_SEQUENCES` (blending needs the order) and no explicit preprocessing.

### Compute

| Metal (AIR) | lowering | execute |
|---|---|---|
| `concurrent_dispatch_threadgroups(grid, tg)` | grid into words 32..34 when `tg` equals words 38..40 (and is known), else refused; `tg` is always stored in 35..37 | DGC dispatch token, or `vkCmdDispatchIndirect` from the record |
| `concurrent_dispatch_threads(threads, tg)` | as above with grid = threads / tg, only when tg divides threads exactly (a ragged edge would run threads Metal clips) | same |
| `set_kernel_buffer(ptr, [stride,] index)` | entry 172 + 2m when bit m of word 22 is set | push-constant token (`48 + 8k`) |
| `set_compute_pipeline_state(pso)` | accepted when it is the pipeline the execute binds (word 24/25) | — |
| `set_barrier` / `clear_barrier` | accepted (flag only) | every slot is dispatched behind a full barrier |
| `reset` | kind 0, grid 0, sticky cleared | — |
| `set_threadgroup_memory_length`, `set_stage_in_region` | not lowered (refused) | |

The threadgroup size is part of the Vulkan pipeline variant (`nvmtl_compute_variant`), chosen on the CPU. The execute
uses the CPU dispatch's size, else the pipeline's `requiredThreadsPerThreadgroup`, else a size **learned** from an earlier
execute: a shader's dispatch refused for its size leaves the size in words 35..37, the completion handler reads it, and
the next execute of that slot uses it. A GPU-only compute slot therefore runs from its second execute on (the first is
refused and logged).

The compute execute (`nvmtlExecuteRecordsCompute:`) replays each slot's CPU state, writes back words 22/24/25/38..40,
and dispatches each slot on its own, behind the encoder's indirect-dispatch barrier (`nvmtlE78Barrier:YES`, a full
barrier): through DGC (push-constant tokens + dispatch token, `nvmtl_vk_cmd_icb_dgc_dispatch`, the binding of
`nvmtl_vk_cmd_dispatch_indirect_tg`) when the kernel reads an offered buffer from its pushed table (cs-table-pc), else
with `dispatchThreadgroupsWithIndirectBuffer:` on the record (core Vulkan: this part works on any device, DGC or not).
An align-guarded kernel (`ag_mask`, it may run its SAFE twin) is not run through DGC.

## 4. Execute, ordering, barriers

Render (`nvmtlExecuteDGC:`): per slot the CPU state is replayed as before; consecutive slots that replay the same state
and draw the same topology form a run, which is **one** `vkCmdExecuteGeneratedCommandsEXT` (`maxSequenceCount` = run
length, `isPreprocessed` false, `VkGeneratedCommandsPipelineInfoEXT` = the bound pipeline). Runs and the sequences in them
keep slot order. After the execute the plugin forgets its index-buffer, vertex-buffer and pushed-table caches (the
extension leaves those bindings undefined).

Ordering with the encoding shader: unchanged from the classic path. The app orders the shader's writes before the execute
with a barrier or an encoder boundary; the plugin's render-encoder memory barrier and pass boundary are
`ALL_COMMANDS / MEMORY_WRITE -> MEMORY_READ`, which covers ANV's implicit preprocessing (a shader read of the records in
the execute) as it covers the indirect-argument read.

Barriers between commands: a Vulkan render pass gives rasterization order for attachments, not memory ordering between
draws, and the extension has no barrier token; the CPU cannot see a GPU-written flag when it records the execute. Keeping a
barrier would mean one execute per slot with a by-region barrier between all of them, for every ICB that might contain
one. So a shader's render `set_barrier` refuses its slot (drawn as nothing, logged) and `clear_barrier` is accepted.
Apple's own M1 compiler refuses `set_barrier_render_command` (`Encountered unlowered function call`), so this is not a
common pattern. Compute slots are serialised by construction, so compute barriers are honoured.

Preprocess memory: `vkGetGeneratedCommandsMemoryRequirementsEXT` per execute (sequence count = run length), sub-allocated
from 1 MiB+ device-local buffers created with `VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT`
(`VkBufferUsageFlags2CreateInfoKHR`, hence `VK_KHR_maintenance5` is enabled with the extension), held by the command buffer
and returned to a small free list when it completes.

Cost (expected, unmeasured): one generation dispatch + CS stall per run on top of the generated commands; per sequence
ANV writes 3DSTATE_INDEX_BUFFER, optional VERTEX_BUFFERS, 3DSTATE_CONSTANT_* when there are push tokens, and
3DPRIMITIVE. Runs of identical-state slots stay one execute. Measure with `mtl_icb_cost` under the knob.

## 5. What stays refused, and why

- **Render pipeline switches** (`set_render_pipeline_state` to another pipeline than the one the execute binds): ANV
  supports indirect execution sets for compute and ray tracing only (`anv_physical_device.c:2138`). Enumerating candidate
  pipelines per slot would need a predicate per sequence (none exists) or a compaction pass, which cannot run inside the
  render pass where the execute is.
- **Compute pipeline switches** (`set_compute_pipeline_state` to another pipeline): ANV would allow an indirect execution
  set of pipelines, but each Metal compute pipeline here is several Vulkan pipelines chosen on the CPU at dispatch time
  (threadgroup-size variants, sampler variants, the align-guard SAFE twin), each with its own push-constant range size
  (`NVMTL_DISPATCH_PC_BYTES + 8 * table entries`); an execution set needs pipelines created `INDIRECT_BINDABLE` with
  compatible layouts. Not done; refused unless it is the pipeline the execute binds.
- **Render barriers set by a shader** (4).
- **Topology**: still the CPU's (word 11); a draw with another primitive type is refused as before.
- A plain draw past the identity buffer (`NVMTL_ICB_DGC_IOTA` elements).
- A pipeline DGC cannot run (stencil-export emulation, used on NVIDIA only; tessellation): its slots are refused in DGC
  mode, because the classic execute would misread draws a shader wrote by address.
- Buffers a stage reads from the descriptor table, GL-UBO block reads, more than 32 tokens, `inheritBuffers` ICBs, the
  descriptor-buffer model: those indices are not offered, so a shader's write to them refuses the slot.
- Pipeline changes between a shader's encode and the execute: the words a shader checks against come from the last
  execute (or the CPU's encode); if the next execute binds another pipeline the checks were made against the old one.
  The same holds for topology today.
- `set_threadgroup_memory_length`, `set_stage_in_region`, `set_object_buffer`, mesh draws, `copy_command` from a shader.

## 6. Knob, caches, fallbacks

- `NVMTL_ICB_DGC=1` (exactly 1). The release plugin drops `NVMTL_*` variables unless `NVMTL_PROCESS_ENV=1`
  (`nvmtl_env_knobs.h`); the translator runs in-process and reads the same environment.
- The SPIR-V cache tag gets `-icbdgc` with the knob (the lowering differs).
- Knob on, device without the extension (or maintenance5): records are still 1024 bytes and the new commands translate,
  but word 20 is 0 and the masks are 0, so the classic checks apply and every state command refuses its slot. The compute
  execute still runs dispatches from the records (`vkCmdDispatchIndirect`).
- `NVMTL_ICB_DGC_IOTA=<n>`: identity index buffer size (default 2M elements, 8 MB, VRAM through the BAR).

## 7. Unverified assumptions

- The AIR signatures were read from Metal's own output (the M1 Pro's compiler through `newLibraryWithSource:`, the
  library's metallib split and printed with `nvmtl-air-dis`): render (`~/hackintosh/work/gicb/probe`, 10 Nov) and
  compute (12 Nov: uint3 as `<3 x i32>`, `set_kernel_buffer_compute_command.p1i8/.p2i8` with an `i64` stride, -1 when
  none). The translator fixtures `icb_dgc_*.ll` are those modules, including the night test's two encoding kernels, and
  pass spirv-val. A kernel buffer's stride (for stage-in) is ignored: stage-in regions are not supported in ICBs.
- Vulkan validity not checked by a validation layer: vertex buffer tokens with pipelines whose stride is static (ANV
  writes the stride from the token, which the plugin sets to the pipeline's); `shaderStages` VERTEX|FRAGMENT for
  pipelines without a fragment shader.
- That ANV's implicit preprocessing inside a dynamic-rendering pass interacts correctly with the plugin's own state
  caches beyond index/vertex/push (it re-emits its state; nothing measured).
- Everything above is untested on hardware as of this note.
