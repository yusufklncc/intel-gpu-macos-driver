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
| 23 | GPU | sticky refusal: bit 0 a state command the execute cannot express was encoded into this slot, bit 1 a render `set_barrier` (`clear_barrier` clears bit 1, reset clears both) |
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
| `set_barrier` | **refused** (sticky bit 1) | — |
| `clear_barrier` | accepted (no barrier is what the execute does); lifts a `set_barrier`'s refusal (sticky bit 1), so a draw encoded after it runs | — |
| `set_object_buffer`, mesh draws, `copy_command` | not lowered (refused as before) | |

Which buffer indices a shader may set (words 21/22, `nvmtl_vk_icb_dgc_plan_gfx`):

- only in an ICB that does not inherit buffers (Metal allows `set_*_buffer` only there; the descriptor's `inheritBuffers`
  is now kept on the ICB). Since 2026-11-12 this holds in both descriptor models (the buffer model has table-pc too,
  `DESCRIPTOR-MODEL.md` section 13); before, only in the classic model;
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
one. So a shader's render `set_barrier` refuses its slot (drawn as nothing, logged) and `clear_barrier` is accepted and
lifts that refusal again (sticky bit 1; until 13 Nov it left the slot refused for good, mtl_gpu_icb2 B4). A draw encoded
*before* the `set_barrier` had its counts zeroed by the refusal and stays empty until the slot is encoded again.
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
- Buffers a stage reads from the descriptor table, GL-UBO block reads, more than 32 tokens, `inheritBuffers` ICBs: those
  indices are not offered, so a shader's write to them refuses the slot. (The descriptor-buffer model was on this list
  until 2026-11-12.)
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

## 8. First night run (13-14 Nov, staged plugin of 12 Nov)

`mtl_gpu_icb` 6/6 with the knob off and on. `mtl_icb_cost`: records cost x1.60 (+20.9 ns/slot) against replay with the
knob, x1.11 without; not addressed yet. `mtl_gpu_icb2`:

- **Knob off: abort (134).** Without the knob the translator refuses `encB`/`encC`/`encBar` (state commands have no
  classic lowering, section 6), and `newFunctionWithName:` returned **nil** for them, which the test handed to
  `MTLComputePipelineDescriptor` (Metal asserts on a nil function). Apple returns the function and fails the pipeline
  instead. Fixed: a function the translator refuses for `*_render_command` / `*_compute_command` calls is returned without
  SPIR-V, and its pipeline creation fails with an error (`function has no SPIR-V`, the reason logged at lookup). The test
  also checks the function and reports `REFUSED ... exit 2`, the expected refusal.
- **Knob on: 8 PASS, 4 FAIL.**
  - B4 (set_barrier + clear_barrier, then draw) drew nothing: **bug**, the barrier's refusal was sticky and
    `clear_barrier` did not lift it (log: slot 4 `state refused 1 ... barrier 0`). Fixed (sticky bit 1, section 3).
  - C0, C1, C2 (compute ICB, kernel-set buffers and grid) read zeros in both executes, even C0, whose slot the CPU encoded
    and which the execute ran through DGC (`4 of 4 slot(s) ... 4 through device-generated commands`, and C1/C2 were not
    refused on the second execute). **Bug, not yet found**: either the words `encC` wrote (grid 32..34, buffers 172..)
    do not reach the record the DGC dispatch reads, or ANV's DGC compute dispatch does not take them. The completion
    handler now logs each dispatch slot's record as the execute left it (`ICB ... slot N after the execute: dispatch ...,
    kernel buffers ...`, 12 per process) to tell the two apart in the next run. Not a documented limitation: the test
    keeps expecting `tag * 1000 + i`.
  - C3 (another compute pipeline) PASSED as the documented refusal (section 5); B2/B3 likewise.

## 9. WebKit's `vsICBIndirect*`: draws into slots the CPU never drew (13 Nov, branch `mtl4icb`, untested on the B580)

WebKit main (not yet Safari 26.7) encodes a render bundle's `drawIndirect` / `drawIndexedIndirect` into its ICB on the GPU
at `executeBundles` time (`Device::icbIndirectEncodeFunction`): the bundle encoder gives the slot its pipeline (topology
class only), vertex and fragment buffers on the CPU and no draw; a vertex function with rasterization off, drawn as one
point per slot, reads the (clamped) indirect arguments and encodes `draw_primitives` or `draw_indexed_primitives` with the
primitive type from a constant buffer and the index pointer `indexBuffer + slotData[2] + args.indexStart`, an entry buffer
parameter plus two dynamic offsets. A slot is encoded once and executed every frame while its buffers are unchanged.
What stood in the way, and what changed:

- **The index pointer is a chain of two GEPs** in the AIR (`%add.ptr3 = gep %add.ptr, %b`, `%add.ptr = gep %indexBuffer, %a`).
  The lowering took the address of the first link's base only, and a GEP result has no address payload in the emitter
  (its `ptrtoint` is 0), so the index buffer was addressed as `0 + 4 * b`: with the knob a non-zero, aligned address that
  the index buffer token would have bound (a GPU fault), without it a refusal. Now both lowerings follow the chain to its
  root (an entry buffer parameter or a loaded pointer) and add every link's terms. In DGC mode an address whose root the
  emitter cannot address is 0 (refused) instead of "0 + offset" (`%nvicbN.pknown`); this also covers buffer pointers of
  `set_vertex_buffer` / `set_fragment_buffer` / `set_kernel_buffer`.
- **An index buffer no CPU command named**: the classic execute binds the CPU's index buffer, so these stay refused
  without the knob (core Vulkan). With `NVMTL_ICB_DGC=1` the index buffer token takes the address (as A0 already did).
- **The topology**: a slot without a CPU draw executes with its pipeline's class as a list (word 11), and WebGPU pipelines
  may be strips. A record now has word 234, the strip of the class (CPU, only for a render slot without a CPU draw), and
  words 235..239, a second `VkDrawIndexedIndirectCommand`. In DGC mode the translator accepts a draw of word 234's type too
  and writes its counts there (the main area's counts are 0; a draw of word 11's type does the opposite; reset and the
  refusals clear both). The execute cannot take a topology from GPU memory, so it learns it: after a command buffer
  completes, the plugin reads word 1 (the type last drawn) of each watched slot (`nvmtl_icb_dgc_learn_topology`, 4 words
  per slot through the BAR, an ICB's first 64 executes and every 32nd after), and the next execute draws a slot that held
  a strip with the strip topology and a plan whose draw token reads words 235..239. Runs split where the learned topology
  changes, so slot order is kept. The record keeps the draw, so the strip shows from the second execute on (WebKit's
  bundles execute every frame). `NVMTL_ICB_DGC_STRIPS=1` instead executes every run with slots without a CPU draw twice,
  list then strip, from the first execute (a run that mixes both draws its lists first).
- The vertex-shader encoder, `rasterizationEnabled = NO`, the argument-buffer container (`setIndirectCommandBuffer:` at
  id 1) and the primitive type from data needed nothing new.

Test `user/tests/metal/mtl_gpu_icb3.m` (own shaders of WebKit's shape): six slots with CPU state, five GPU-encoded (u32 and
u16 indexed lists through the two-GEP pointer, a plain list, an indexed strip with a base vertex, a plain strip) and a CPU
draw, encoded in the first of two command buffers and executed in both. Apple M1 Pro (`--any`): 16/16. On the B580: knob
off, the indexed draws and the plain strip are the expected refusals; knob on, all from the second execute; with
`NVMTL_ICB_DGC_STRIPS=1` all from the first. Night phase Q (`user/tests/metal/night-mtl4icb.sh`).

### C0..C2 (compute ICB, open)

Not fixed: no hardware run since the after-execute dispatch log went in. What the code says (13 Nov):

- the record layout, the word offsets of the translator and the plugin (172 + 2m, 32..34) and the push-constant offsets
  (`NVMTL_PC_TABLE_CS_OFFSET` 48 = `nvmtl_cs_pc_values`' `pc + 12 + 2k`) agree; the kernel's two buffers are push slots 0
  and 1 and the plan has them (log: 3 tokens, kernel mask 0x7fffffff);
- the encoder kernel's writes are ordered before the execute: a new compute encoder, then `nvmtlE78Barrier:YES` (indirect:
  `ALL_COMMANDS`, `MEMORY_WRITE -> MEMORY_READ|WRITE`), which covers the generation shader's read of the records;
- C0's slot carries the CPU's buffers (scratch, tag) under the shader's (out, tag 1): out[0..63] = 0 is what a dispatch
  that ran with the CPU's buffer 0 leaves. So the likeliest fault is that the push-constant tokens of a compute sequence
  do not reach the kernel (render push tokens do: B1's per-slot fragment buffer passed). In ANV (`genX_cmd_dgc.c`
  `preprocess_cs_sequences`, `dgc.cl` `write_app_push_constant_data` / `emit_dispatch_commands`) the per-sequence push data
  is the CPU template with the tokens copied over, and the walker's inline data points at it; nothing in that path looks
  wrong for Xe2 (`uses_efficient_64bit` is Gfx35+ only).
- The next run's `ICB ... slot N after the execute: dispatch ..., kernel buffers ...` lines tell "the record holds out +
  0 and the grid" (then the push path) from "it does not" (then the encode). A fallback that needs no push token would
  be a non-table-pc variant of ICB kernels with the record's addresses copied into the table snapshot before each
  dispatch (core Vulkan); not built.

## 10. C0..C2: the kernel wrote the CPU's buffer (13 Nov, branch `icbc`, untested on the B580)

**What the 13 Nov 17:41 run said.** The after-execute lines (`night1315` Q7) show slot 0 holding the shader's grid
(`dispatch 2x1x1`), threadgroup 32 and kernel buffers `out + 0` / `tag`, slots 1 and 2 `out + 256 B` / `tag + 4` and
`out + 512 B` / `tag + 8`, sticky 0, all dispatched through DGC. So the encode and the barrier were right and the record
the sequence reads was right; ANV's compute DGC path (`preprocess_cs_sequences`, `dgc.cl` `write_app_push_constant_data`,
the walker's inline data, gfx125 layout on Xe2: `uses_efficient_64bit` is Gfx35+) also reads right.

**Root cause: the kernel never read buffer 0 from its table.** `w` is `o[t] = tag * 1000 + t` with
`t = thread_position_in_grid`, which the translator lowers to `GlobalInvocationId + dispatch_region[3]` (the pushed thread
base, `stage_input` `LoadBuiltinPlusKernelDispatchField`). The plugin's address lowering (wrapper `buffer_addresses.rs`
`kept_roots`) keeps the *descriptor* of a runtime-array buffer whose index depends on loaded data (robustBufferAccess
bounds-checks a data-driven index; a physical address would fault), and `data_dependent` counted any load that is not an
`Input` as data, the push-constant thread base included. So `o` stayed a StorageBuffer at set 0 binding 0, indexed from
the descriptor's offset rounded down plus the remainder read from table entry 0 (the residue rule); cs-table-pc then
pushed entries 0 and 1 (`2 entries` in the log). The DGC plan offered index 0 because the kernel "reads entry 0 from its
pushed table", the token put `out`'s address there, and the kernel used only its low bits: every write went to the
CPU's binding 0, `scratch` (C0's CPU buffer, left bound by the replay for C1 and C2). `tag` (a fixed-size block) was by
address and did arrive. Render slots passed because `pos[vertex_id]` and `col[instance_id]` index with `Input`s only.
The host check `icb_lower_check` shows it on the translated `w`: default lowering `descriptors {0}`, pushed `{0,1}`.

**Fix.**
- Translator: `buffer_addresses::lower_with_opts(.., push_not_data)`, FFI flag bit 4 (`NVMTL_LOWER_PUSH_NOT_DATA`) on
  `nvmtl_lower_buffer_addresses_ex/_g2`: a load from a push constant is a driver value (the dispatch region; Metal has no
  push constants of its own), so `o[thread_position_in_grid]` is lowered to an address. Off by default (`lower()` and
  every other caller unchanged).
- Plugin: a compute pipeline created with `supportIndirectCommandBuffers` (the descriptor path, and its sampler variants
  through `_icb`) is built with that flag when `NVMTL_ICB_DGC=1` (`nvmtl_vk_compute_pipeline_create_icb`; the align
  guard's SAFE twin keeps it; `NVMTL_ICB_DGC_LOWER=0` builds it as before). Such a kernel keeps a descriptor only for a
  truly data-driven index (`o[idx[t]]`).
- Guard: each pipeline records per stage the buffer indices its final module still reads through a descriptor
  (`desc_bufs`, `nvmtl_desc_bufs.h`: a StorageBuffer/Uniform variable at binding < 32 outside sets 2 and 3), and
  `nvmtl_dgc_takes` never offers them (render and compute). A shader's `set_*_buffer` to such an index now refuses the
  slot (logged) instead of the kernel silently using the CPU's buffer.

**Tests.** Wrapper unit tests (`an_index_from_push_constants_keeps_the_descriptor_unless_push_constants_are_not_data`,
`an_index_read_from_a_buffer_keeps_the_descriptor_either_way`). Host `icb_lower_check` (M1 Pro under Rosetta, the Mac's
front end, the built plugin and translator): `w`/`kt` descriptor before, address with the flag; `ki` (`o[idx[t]]`) a
descriptor either way, index 0 never offered; `kc` by address. New `mtl_icb_kept` (K0 `o[tid]` set by a shader; K1
`o[idx[t]]` set by a shader: refused on the B580, never written into the CPU's buffer; K2 only the tag and grid from the
shader, the CPU's `o`: the DGC compute path itself) and `mtl_gpu_icb2` now print `scratch` as a diagnostic; M1 Pro
`--any`: 3/3 and 12/12. Night phase `phase_icbc` (`~/hackintosh/work/icbc-stage/night_icbc.sh`): expected on the B580
with the knob `mtl_gpu_icb2` 12 PASS and `scratch[0]=0`, `mtl_icb_kept` K0..K2 PASS (K1 as the refusal); with
`NVMTL_ICB_DGC_LOWER=0` C0..C2 and K0 FAIL as refusals (the guard: not run, `scratch` 0, a "NOT run" line per slot;
before the guard they wrote `scratch`) and K1/K2 PASS.
