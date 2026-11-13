# What ANV emits for descriptor buffers on Xe2 (research, 2026-11-13)

> Design note with its date: it records what the source says on that day. Nothing here was measured on the B580.

## The question

Geekbench 6 Metal on the B580 scored 31,969 with the plugin's descriptor-buffer model (`NVMTL_DESC_MODEL=buffer`) and
93,475 with classic descriptor sets: 3x slower, although the plugin spent less CPU time per bind. In buffer mode, almost
every compute dispatch gets a new region (0.97 new regions per bind) and so new `vkCmdSetDescriptorBufferOffsetsEXT`
offsets; `vkCmdBindDescriptorBuffersEXT` runs when the 256 KiB page changes (`nvmtl_db_bind_buffers`,
`nullmoth/plugin/nvmtl_dbuf.c`). This note answers what ANV puts in the batch for those calls on Xe2 (Gfx20,
`GFX_VERx10 == 200`), and which plugin patterns are expensive.

Source: `~/hackintosh/work/mesa014` (upstream `f01e9b07a91` + `mesa/patches`; none of our patches touch the code
below). Paths are under `src/intel/vulkan/`. Facts that select the Xe2 paths:

- `uses_efficient_64bit` is false: it needs `verx10 >= 350` (`anv_physical_device.c:3254`). Every
  `GFX_VERx10 < 350 || !uses_efficient_64bit` branch is taken.
- `intel_has_extended_bindless()` is true (`verx10 >= 125`, `intel/dev/intel_device_info.h:240`), and `has_lsc` is true.
- Wa_1607854226 (pipeline select around SBA) is TGL/DG1 only; Wa_16013000631 (instruction cache invalidate) is
  DG2/ATS-M only; Wa_18039438632 (render target flush before SBA) does apply to BMG (`intel/dev/mesa_defs.json`).

## Short answer

In a command buffer that stays in descriptor-buffer mode, ANV on Xe2 emits **no PIPE_CONTROL, no STATE_BASE_ADDRESS
and no 3DSTATE_BINDING_TABLE_POOL_ALLOC** for `vkCmdBindDescriptorBuffersEXT` or `vkCmdSetDescriptorBufferOffsetsEXT`.
Their cost at the next dispatch is the same as a classic set change: new push constants (a new indirect data block for
the COMPUTE_WALKER) and CPU work. The only stall these paths can create is a **binding-mode switch** (descriptor
buffer <-> classic sets), which costs a CS stall, cache flushes and a state, constant and texture cache invalidation.

So ANV's command stream does not explain the 3x by itself unless something still switches modes. The GPU-side
differences that remain are in how the shaders reach their descriptors (section 5): classic compute shaders use the
binding table, and ANV **prefetches up to 31 of its surface states at each dispatch**; descriptor-buffer shaders are
fully bindless, with no prefetch, and the plugin's per-dispatch fresh regions mean every descriptor is at a new
address, so the GPU must fetch it from memory instead of finding it in its state cache.

## 1. `vkCmdBindDescriptorBuffersEXT`

`anv_CmdBindDescriptorBuffersEXT`, `anv_cmd_buffer.c:769-796`: CPU state only. For each index whose address changed
it stores the address, notes which index holds surfaces/samplers, and sets `descriptor_buffers.dirty = true` and
`offsets_dirty = ~0` (all stages). If the address did not change, it does nothing at all.

At the next draw or dispatch, `genX(flush_binding_mode)` (`genX_cmd_buffer.c:3515`) handles BUFFER mode at
`genX_cmd_buffer.c:3533-3546`: on `GFX_VERx10 >= 125` it emits SBA **only if the current mode is not BUFFER or
HEAP**, then clears `descriptor_buffers.dirty`. A new descriptor buffer address does not emit SBA: the bindless surface
base is the start of the whole `dynamic_visible` pool, with a size covering that pool plus the push descriptor pool
(`fill_state_base_addr`, `genX_cmd_buffer.c:213-222`), and ANV asserts that every descriptor buffer lies inside it
(`anv_cmd_buffer.c:782`). The per-address SBA (`#else` branch, `genX_cmd_buffer.c:3541-3547`) is for Gfx9-12.0 only.

What a re-bind costs: `offsets_dirty = ~0` makes the next flush of **every** bind point (graphics and compute)
recompute its set offsets, re-upload push constants and re-emit binding tables. That is CPU work and a new push
constant block per bind point; no stall.

## 2. `vkCmdSetDescriptorBufferOffsetsEXT`

`anv_CmdSetDescriptorBufferOffsets2EXT` -> `anv_cmd_buffer_set_descriptor_buffer_offsets`
(`anv_cmd_buffer.c:798-878`). Per set whose `(buffer index, offset)` changed: store it, mark descriptors dirty for the
set layout's stages (`anv_cmd_buffer_dirty_descriptors(..., "EXT_DB offset")`) and `offsets_dirty |= stages`. Then
`anv_cmd_buffer_update_binding_mode(BUFFER)` (`anv_cmd_buffer.c:877`), which only records the pending mode.

At the next dispatch, `cmd_buffer_flush_compute_state` (`genX_cmd_compute.c:89`):

1. `genX(flush_binding_mode)` (`genX_cmd_compute.c:108`). BUFFER case, `genX_cmd_buffer.c:3611-3641`: for each set
   below `max_bound_descriptors`:
   - `update_descriptor_set_surface_state` (`genX_cmd_buffer.c:3392`): if the set's address changed, allocate a 64 B
     surface state from the surface state stream and fill it as a constant buffer covering the set. **On Xe2 nothing
     ever reads it** (section 8), so this is pure CPU waste, once per set per new address.
   - `push->buffer.sets[i].surfaces_offset` / `samplers_offset` = the set's offset from the bindless base
     (`compute_descriptor_set_surface_offset`, `genX_cmd_buffer.c:3438`, the `intel_has_extended_bindless` branch).
   - If any value changed: `push_constants_dirty` and `push_constants_state = ANV_STATE_NULL`, so a new push
     constant block is allocated.
2. `genX(cmd_buffer_apply_pipe_flushes)`: applies only what barriers left pending. Offsets add no pipe bits.
3. Descriptors dirty -> `genX(cmd_buffer_flush_descriptor_sets)` (`genX_cmd_buffer.c:2922`): `emit_samplers` and
   `emit_binding_table`. In BUFFER mode the resource bindings get no binding-table entries (`anv_descriptor_set.c:
   208-215`: "Non push descriptor buffers cannot be accesses through the binding table"), and on LSC there is no
   descriptor-set entry either, so a compute shader's table is normally empty: `surface_count == 0` returns at once
   (`genX_cmd_buffer.c:2738-2741`).
4. `anv_cmd_buffer_cs_push_constants` (`genX_cmd_compute.c:246`): a copy of the push constant block in the dynamic state
   stream.
5. `emit_compute_walker` (`genX_cmd_compute.c:656`): one COMPUTE_WALKER whose inline data carries the push constant
   address and the inline dwords; the interface descriptor carries the (empty) binding table and sampler pointers.

No PIPE_CONTROL, no CS stall. The same push-constant re-upload happens in classic mode whenever a set or a dynamic
offset changes, so per dispatch this is not more GPU work than the classic model.

## 3. Switching between descriptor buffers and classic sets

The mode is chosen by the API call: `vkCmdBindDescriptorSets` / `vkCmdPushDescriptorSetKHR` with a layout that lacks
`VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT` sets LEGACY (`anv_cmd_buffer.c:642-646`); a set-offsets
call sets BUFFER (`anv_cmd_buffer.c:877`). At the next flush, LEGACY <-> BUFFER calls
`genX(cmd_buffer_emit_state_base_address)` (`genX_cmd_buffer.c:3519-3572`), which emits (`genX_cmd_buffer.c:281-441`):

1. an MI predicate: compare a scratch register holding the last bindless base with the new one and jump over the rest
   if equal (`genX_cmd_buffer.c:308`; upstream MR 29595, 2024). LEGACY and BUFFER use different bases
   (`internal_surface_state_pool` vs `dynamic_visible_pool`), so a real switch is never skipped;
2. `PIPE_CONTROL` HDC pipeline flush + render target cache flush + **CS stall** ("pre STATE_BASE_ADDRESS flush",
   `genX_cmd_buffer.c:332`);
3. `STATE_BASE_ADDRESS` (`genX_cmd_buffer.c:344`);
4. `PIPE_CONTROL` **texture, constant and state cache invalidate** ("Post STATE_BASE_ADDRESS invalidate",
   `genX_cmd_buffer.c:416`);
5. `genX(cmd_buffer_emit_bt_pool_base_address)`: skipped when the binding table pool address did not change
   (`genX_cmd_buffer.c:453`), which is the case on a mode switch;
6. all stages' descriptors dirty (`genX_cmd_buffer.c:438`): every binding table and push block is emitted again.

Cost: the CS stall drains the GPU (no overlap of the next dispatch with the previous one), and the invalidations
make the next dispatches refetch surface states, samplers and constants. Done per dispatch, this is easily a
multiple of a short dispatch's time. Two variants of the same cost:

- **`vkBeginCommandBuffer`**: with `EXT_descriptor_buffer` enabled on the device, every command buffer starts in BUFFER
  mode and emits this SBA sequence (`genX_cmd_buffer.c:4114-4118`); the predicate skips it on the GPU when the
  previous command buffer also ended in BUFFER mode. A classic bind afterwards is a real switch.
- **`vkCmdExecuteCommands`**: re-emits SBA if the last secondary ended in another mode, otherwise only the binding
  table pool address (`genX_cmd_buffer.c:4733-4747`).

ANV's own operations do not switch modes: blorp and the internal compute kernels change only binding tables
(`anv_blorp.c:1801-1824`, `genX_simple_shader.c`), and `anv_cmd_buffer_ensure_valid_binding_mode`
(`anv_private.h:5299`) only fills in an unknown mode.

## 4. Compute dispatch on Xe2

- The walker is `COMPUTE_WALKER` with inline data (`emit_compute_walker`, `genX_cmd_compute.c:656-750`); GPGPU_WALKER
  and MEDIA_CURBE_LOAD/MEDIA_INTERFACE_DESCRIPTOR_LOAD are the pre-Gfx12.5 path. Push constants are a block in the
  dynamic state pool whose address goes into the inline data; a changed value means a new block, not a stall.
- The only CS stall tied to compute state is the pre-Gfx12.5 MEDIA_VFE_STATE one (`genX_cmd_compute.c:120-133`).
  On Xe2, `CFE_STATE` is emitted only when the scratch size grows (`genX(cmd_buffer_ensure_cfe_state)`,
  `genX_cmd_compute.c:43-86`).
- The back-to-back dispatch dataport flush (`cmd_buffer_pre_dispatch_wa`, `genX_cmd_compute.c:382`) is off by default
  (`anv_back_to_back_dispatch_dataport_flush`, `anv_drirc_gen.py:146`). The post-dispatch flushes
  (`genX_cmd_compute.c:402-456`) are per-application workarounds, off by default.
- Binding tables come from 4 KiB blocks (`anv_binding_table_block_size`, `anv_drirc_gen.py:196`). A full block means a
  new block (`anv_cmd_buffer_new_binding_table_block`, `anv_batch_chain.c:851`) and a call to
  `genX(cmd_buffer_emit_bt_pool_base_address)`. That emits `PIPE_CONTROL` (CS stall) +
  `3DSTATE_BINDING_TABLE_POOL_ALLOC` + texture/state invalidate (`genX_cmd_buffer.c:488-505`) only when the 1 MiB
  pool window moves (`BINDING_TABLE_VIEW_SIZE`, `anv_private.h:194`; `anv_batch_chain.c:469`): once every 256 blocks.
  Classic mode pays this a little (4 bytes per binding-table entry per dispatch); compute shaders in BUFFER mode have
  empty tables and never do.

So in steady BUFFER mode there is **no stall between dispatches** caused by descriptor state on Xe2.

## 5. Where the two models really differ on the GPU

1. **Binding table vs fully bindless.** In classic (direct descriptor) mode on Xe2, every used binding that fits in
   240 entries gets a binding-table index (`binding_should_use_surface_binding_table`,
   `anv_nir_apply_pipeline_layout.c:2337`; `anv_descriptor_set.c:178-180`), and the shader uses BTI messages. In BUFFER
   mode all resource access is bindless: the shader builds a surface handle from `push->buffer.sets[i]` plus the
   binding's offset.
2. **Surface state prefetch.** For compute, `INTERFACE_DESCRIPTOR_DATA.BindingTableEntryCount` is
   `MIN2(surface_count, 31)` (`genX_shader.c:1240`, `get_surface_count` at `genX_shader.c:31-42`), and
   `intel_force_compute_surface_prefetch` is on by default (`anv_drirc_gen.py:221`). The hardware then loads up to 31
   surface states into the state cache when the walker starts. With descriptor buffers `surface_count` is 0: no
   prefetch, every surface state is fetched on first use by the threads.
3. **Cold descriptors.** The plugin writes a fresh region for almost every dispatch, so the 64 B surface states are at
   new addresses each time and cannot hit the state cache. In the classic model, a reused set is at the same address
   from one dispatch to the next (only the address table's dynamic offset changes).
4. **Descriptor memory.** Classic sets live in ANV's pools in VRAM. The plugin's pages take
   `DEVICE_LOCAL | HOST_VISIBLE | HOST_COHERENT` if offered and fall back to plain host-visible memory
   (`nvmtl_dbuf.c:147-153`). ANV offers a copy of every memory type for descriptor buffers, system memory types included
   (`anv_physical_device.c:2725-2763`). Pages that end up in system memory (no device-local host-visible type, or a
   kernel that places them outside the CPU-visible VRAM window) are read by the GPU over PCIe at every cold fetch.

Ranked by what the source suggests for a 3x on a compute benchmark:

- **A mode switch per dispatch** (section 3) would explain it alone. The plugin says it never mixes models
  (`nvmtl_dbuf.c` header), but one forgotten path (a layout created without the descriptor-buffer flag, a push
  descriptor, a classic bind from an internal pipeline, secondaries) would put a CS stall + cache invalidation before
  each dispatch. This is the first thing to rule out: it is one counter (section 9).
- **Descriptor memory in system memory** (point 4): a cold fetch per surface per dispatch over PCIe.
- **No prefetch + cold descriptors** (points 2-3): real, but per dispatch it is a few hundred ns to a few us of latency,
  not 3x, unless the dispatches are very short.

## 6. Upstream

Our base is upstream main of 2026-10-25. Relevant upstream work, all already in it:

- MR 22151 (2024-02): `VK_EXT_descriptor_buffer` in ANV.
- MR 29595 (2024-06): STATE_BASE_ADDRESS predicated on the bindless base (the MI compare in section 3).
- MR 36768 (2025-08): "check for pending_db_mode when dirtying descriptor mode".
- MR 43158 (2026-08): smaller push constant data for EXT_descriptor_buffer/heap (`push->buffer` / `push->legacy`).

Since the base (commits touching `genX_cmd_buffer.c`, `anv_cmd_buffer.c`, `genX_cmd_compute.c` after 2026-10-20, via
the GitLab API): barrier fixes (5c9832fc "fix PIPE_CONTROL with fragment->compute barrier on Xe2+", 606a0d69 "deferred
constant cache invalidation", 89bda40e), Gfx35 preemption changes and a null-pointer fix. None changes the
descriptor-buffer paths. Open issue 16028 ("switch to full bindless on Gfx12.5+ ... will allow to never reprogram
STATE_BASE_ADDRESS") confirms that LEGACY and BUFFER still use different bindless bases, so a mode switch still costs a
full SBA upstream. I found no upstream issue about descriptor buffer performance on Intel. `update_descriptor_set_surface_state`
is unchanged on main (checked 2026-11-13).

## 7. Recommendations for the plugin

Cheap (no GPU commands beyond a new push constant block):

- `vkCmdSetDescriptorBufferOffsetsEXT` with only changed sets. ANV compares each set's (index, offset) and does nothing
  for an unchanged one; the plugin's `nvmtl_db_set_offsets` skip of identical calls is still worth keeping (CPU).
- Re-binding the same addresses: free. Binding new addresses: no SBA on Xe2, but `offsets_dirty = ~0` makes every bind
  point re-upload push constants. Keeping one page bound for a whole command buffer (a larger page, or a per-command
  buffer arena) saves that CPU work; it is not a GPU stall.
- Push descriptors **in a descriptor-buffer layout** (`DESCRIPTOR_BUFFER_BIT_EXT | PUSH_DESCRIPTOR_BIT`) stay in BUFFER
  mode (buffer index -1, `anv_cmd_buffer.c:664-674`). Each push allocates a new block in ANV's push descriptor pool:
  CPU and memory, no stall. Not cheaper than writing the plugin's own region.

Expensive:

- **Anything that switches to LEGACY inside a command buffer**: `vkCmdBindDescriptorSets` or `vkCmdPushDescriptorSet`
  with a classic layout, at any bind point, including internal pipelines. Each switch: CS stall + flushes + SBA +
  state/constant/texture cache invalidation. Must be zero per command buffer, not just rare.
- Secondary command buffers recorded in a different mode than the primary.
- A fresh region for every dispatch whose descriptors did not change: no stall, but the GPU loses the state cache hits
  that the classic model had. Reusing an identical region (same bytes, same address) inside a command buffer is the GPU
  side of the plugin's region reuse, not only a CPU saving. Better still for compute: keep resource descriptors at a
  stable address per object (the bindless heap already does this for textures) and let per-dispatch data change
  only in the address table.
- Descriptor pages in system memory. Log the memory type and heap the page allocation chose.

## 8. ANV patch: `mesa/patches/0105`

`update_descriptor_set_surface_state` builds a 64 B surface state for every new set address. Its only reader is
`emit_binding_table` for an `ANV_DESCRIPTOR_SET_DESCRIPTORS` entry (`genX_cmd_buffer.c:2783-2788`, which asserts
`!has_lsc`), and that entry is only created when `!has_lsc` (`build_packed_binding_table`,
`anv_nir_apply_pipeline_layout.c:2509-2516`, the only caller of `add_desc_bti_entry`). LSC shaders read descriptor
buffers through `device->descriptor_buffer_view_state` (`anv_device.c:348-362`, `anv_shader.c:656-661`). The patch returns
early on `has_lsc`. It saves one surface-state allocation and pack per set per new address (about two per dispatch
for the plugin) and 64 B of surface state stream each time. CPU only; it does not change the batch, so it does not
address the 3x. It applies on upstream `f01e9b07a91` (dry run on `mesa014`). Not built.

No ANV patch removes a stall here: in steady BUFFER mode there is none to remove, and the mode-switch SBA is required
while LEGACY and BUFFER use different bindless bases (upstream issue 16028 is the real fix: one base for both).

## 9. How to measure (next hk run)

All through the per-process options file (`/tmp/mesa-options-<progname>`, patch 0020), since these are read with
`os_get_option`:

- `ANV_DEBUG=desc-dirty` logs every reason descriptors were dirtied (`anv_util.c:83`). Count, per command buffer:
  `state base address` (one per SBA ANV emitted, `genX_cmd_buffer.c:438`), `bt pool address` (binding table pool
  re-emits) and `EXT_DB offset` (offset changes). Expected in a clean buffer-mode run: one `state base address` per
  command buffer (the begin, `genX_cmd_buffer.c:4116`) and nothing more. More than that means a mode switch.
- `INTEL_DEBUG=pc` prints each PIPE_CONTROL ANV emits with its bits (`anv_debug_dump_pc`, `genX_cmd_buffer.c:95`).
- `intel_force_compute_surface_prefetch=false` (a driconf option, overridable through `os_get_option`,
  `util/xmlconfig.c:1049`) on the **classic** model: if classic falls towards the buffer model's score, the missing
  surface prefetch and cold descriptors (section 5, points 2-3) are the cause.
- Geekbench in buffer mode with `NVMTL_DB_KEEP=1` vs `0`: how much region reuse buys on the GPU side.
