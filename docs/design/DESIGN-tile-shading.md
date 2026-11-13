# Tile pipelines by render-pass splitting (design, 2026-11-13)

> Design note with its date: it records what was built and the state of that day. Status: implemented on branch
> `mtl4tile`, **not yet run on the B580**; checked off-hardware (translator tests, `tile_host` against the build) and on an
> M1 Pro as the reference (`mtl_tile --any` 7/7). Night phase T (`user/tests/metal/night-tile.sh`). Before this branch
> tile pipelines were refused (`DESIGN-mtl4-compiler.md` "What is refused, and why").

## What Metal asks for

A tile function is an AIR kernel (`!air.kernel`) that runs between the draws of a render pass, one threadgroup per tile,
over the tile memory that holds the pass's colour attachments:

- **implicit imageblock** (`imageblock<T, imageblock_layout_implicit>`, members `[[color(n)]]`):
  `air.load.implicit_imageblock.<type>(i32 attachment, <2 x i16> coord, i32 rate, i16 index)`,
  `air.store.implicit_imageblock.<type>(value, attachment, coord, rate, index)` and
  `air.store.implicit_imageblock.mask.<type>(value, attachment, coord, i16 coverage)`; the coordinate is relative to the
  tile. Rate 0 is the plain `read(coord)` / `write(v, coord)`, 1 `imageblock_data_rate::color`, 2 `::sample` (Metal's
  header, `metal_imageblocks`). `get_num_colors`, `get_color_coverage_mask`, `get_imageblock_samples`,
  `get_imageblock_width/height` are the other queries;
- **explicit imageblock** (`imageblock<T, imageblock_layout_explicit>`): `air.imageblock_data(coord, rate, index)` gives a
  `threadgroup_imageblock` pointer to the cell of a pixel;
- tile-stage resources (`setTileBuffer/Bytes/Texture/SamplerState`, acceleration structures, function tables),
  `setThreadgroupMemoryLength:offset:atIndex:`, the pass's `tileWidth/tileHeight`, `dispatchThreadsPerTile:`;
- Metal 4: `MTL4TileRenderPipelineDescriptor` through an `MTL4Compiler`, the tile stage of an argument table
  (`MTLRenderStageTile`), `MTL4RenderCommandEncoder dispatchThreadsPerTile:`.

The Arc B580 renders straight to memory: there is no tile memory and no tile stage in Vulkan.

## How it runs here

1. **The pipeline** (`nvmtl_tile_pipeline`, `nullmoth/plugin/NVMTLTile.m`). The translator's `tile_lower`
   (`nullmoth/translator/translator/src/tile_lower.rs`, wrapper `nvmtl_lower_tile`) rewrites the tile function's AIR into
   an ordinary kernel, and the plugin makes a compute pipeline of it (`newComputePipelineStateWithFunction:`, the lowered
   AIR kept as the function's AIR so threadgroup-length, sampler and function-constant variants come from it):
   - every implicit imageblock access becomes a `read/write_texture_2d_array` of the attachment's **plane**, texture index
     `120 + n` (`tile_lower::PLANE_TEXTURE_BASE`), at `threadgroup_position_in_grid * tile size + coordinate`; the
     function's own `threadgroup_position_in_grid` is used when it has one (a second input with that builtin is invalid
     SPIR-V);
   - samples: a plane has one layer per sample, and each sample is its own colour (`get_num_colors` = sample count,
     colour i covers sample i). Rate 0 reads sample 0 and writes every sample; rates 1 and 2 read and write the layer the
     index names; a coverage-mask write is, per sample, read-select-write of the bit;
   - the tile size comes from a hidden constant buffer at index 30 (`uint4` tile width, tile height, render width,
     render height): one pipeline for every tile size;
   - a threadgroup barrier on threadgroup or imageblock memory also orders texture memory (the planes are images);
   - an explicit imageblock stays what the translator makes of one in any kernel: per-threadgroup memory, one cell per
     thread. A function's own texture at 120..127 or buffer 30 is refused at pipeline creation, as is an explicit
     imageblock aliased onto the colour attachments (`[[alias_implicit_imageblock]]`), a call to another imageblock
     intrinsic, and linked functions.
   The plane's component type follows the descriptor's `colorAttachments[n].pixelFormat`: float/normalized formats a
   float plane (RGBA16F for 8-bit normalized formats, exact round trip; RGBA32F otherwise), integer formats an RGBA32
   uint/int plane. The translator declares a float plane formatless (as every float storage image here) and an integer
   one RGBA32 exactly (wrapper `formatless_options`, only in a module that names an `nvt_plane` argument).
2. **The dispatch** (`dispatchThreadsPerTile:` on the Metal 3 render encoder; the Metal 4 encoder pushes its tile table
   and forwards):
   - the Vulkan render pass ends (store ops are always STORE here); a pass that no draw began has its clears applied
     first (the empty-pass path of `endEncoding`). Every attachment is marked loaded, as the pipeline-transition split
     already does;
   - each attachment the function uses is copied into its plane by a compute copy that reads it as a sampled texture
     (sRGB decodes, an MSAA target is read per sample), unless the previous tile dispatch left the plane current (no draw
     between them);
   - the kernel runs with the tile bindings, the planes and the tile parameters, one threadgroup per tile over the render
     area (partial tiles at the edges: their texels outside the plane are not written);
   - full barriers separate the copies, the kernel and the pass;
   - the encoder's bindings: the internal encoders bind through the command buffer's binding table, so both stages' slots
     are saved before and written back after (`nvmtl_vk_bind_save/restore`, every slot that differs marked changed).
3. **The copy back** of every written plane is recorded when the pass resumes (the next render pipeline set) or ends: a
   full-screen draw per attachment (per-sample shading with `[[sample_id]]` for MSAA, sRGB encodes on store, integer
   formats through uint/int outputs), then the pass begins again with every attachment loaded. Attachments only read
   return to the layout a pass begins them in. Store and resolve actions happen at the real end of the pass (a pass whose
   last command was a tile dispatch resolves through the empty-pass path).

## What is not carried (said in the log or refused)

- Explicit imageblock data and threadgroup memory do not persist across tile dispatches, nor are they shared with
  fragment functions (`[[imageblock_data]]`, `threadgroup` arguments of fragment functions): on Apple they live in tile
  memory for the whole pass. An explicit imageblock needs threads per tile to cover the tile (the cell of a pixel is the
  cell of the thread at it). A threadgroup memory offset is not carried (each index is its own variable).
- Fragment functions with `[[imageblock_data]]` (a per-pixel buffer read and written in raster order) are unchanged.
- Layered render passes (`renderTargetArrayLength` > 1) and 3D attachments: the dispatch is dropped with a log line.
- Every tile dispatch costs a pass end, a copy of each used attachment and a draw per written one; consecutive dispatches
  share the copies. It is correct, not fast; nothing here touches a pass without tile dispatches (one class compare per
  `setRenderPipelineState:`).

## How the device answers

`supportsFamily:` stays NO for every Apple family. Tile shading is one of many things Apple4 promises (raster order
groups, memoryless render targets, fragment imageblocks, threadgroup memory shared with fragment functions, read-write
texture tier 2 semantics), and this device has only the tile pipelines; an app that asks for Apple4 before using tile
shading takes its other path, as before. An app that makes tile pipelines without asking gets working ones instead of
`MTLDeviceErrorNotSupported`. Reflection: `tileArguments` / `tileBindings` of the pipeline's reflection (when asked with
`MTLPipelineOptionBindingInfo`) are the tile function's arguments. `imageblockSampleLength` is the colour attachments'
per-sample size as an implicit imageblock lays them out (a hint; nothing is allocated from it).

## Byte identity

`tile_lower` runs only for tile pipelines; the wrapper's integer-plane formats apply only to modules with an `nvt_plane`
argument, which only the lowered kernels and the plugin's plane copies have. Every other translation is the module it
was: the `.ll` modules under `~/hackintosh/work` (deduplicated by content, AIR with one stage root; the Geekbench corpus
among them) translated through the plugin's path (`nvmtl_translate`, caps 7) with the base commit's translator and this
branch's gave 826 identical modules, 0 different (60 fail in both).

## Tests

- Translator `tile_lower` tests (7): deferred lighting over three planes, per-sample colours and coverage masks
  (unrolled over 4 samples), integer and float classes, barriers, explicit imageblocks, narrow types and sample-rate
  reads, reserved indices; each lowered module translates, passes `spirv-val` (where installed) and binds its planes at
  480 + 120 + n. Fixtures in `src/testdata/tile_*.ll` are Metal's compiler output read by the plugin's metallib reader.
- `user/tests/plugin/tile_host` (no GPU, x86_64): the plugin's `nvmtl_tile_pipeline` with the real translator and a
  stand-in device, six tile functions (planes, masks, SPIR-V bindings) and three refusals. 9/9.
- `user/tests/plugin/link_host`: the Metal 4 tile descriptor becomes the Metal 3 one; static linking into a tile function
  is refused. 19/19.
- `user/tests/metal/mtl_tile` (on a device): deferred lighting with a blended draw after the dispatch, an explicit
  imageblock struct, threadgroup memory with tile buffers, MSAA 4x (per-colour reads, coverage-mask writes, resolve),
  RGBA32Float + R32Uint with no draw before the dispatch, a dispatch / draw sequence, Metal 4. 7/7 on the M1 Pro.
- `user/tests/metal/mtl4_linking` cases 5 and 12 expect the tile pipeline made and run on the B580 driver too.
- Night phase T (`user/tests/metal/night-tile.sh`).
