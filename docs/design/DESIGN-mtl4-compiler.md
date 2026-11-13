# Pipeline reflection and MTL4Compiler (design, 2026-11-13)

> Design note with its date: it records what was built and the state of that day. Status: implemented on branch
> `mtl4`, **not yet run on the B580**; the Metal 4 parts need `NVMTL_METAL4=1` (the layer is off by default). Checked
> off-hardware against Apple's answers (`user/tests/plugin/refl_host`) and on an M1 Pro as the reference.
> Later the same day (branch `mtl4more`, also not yet run on the B580): binary functions and dynamic linking built, tile
> pipelines, machine-learning pipelines, pipelines scripts and pipeline-independent binary functions refused with an
> error (sections "Binary functions and dynamic linking" and "What is refused, and why"). Branch `mtl4icb` (also not yet
> run on the B580): pipeline-independent binary functions built (section "Pipeline-independent binary functions").

## What was missing

- **Reflection.** The Metal 3 entry points (`newComputePipelineStateWithFunction:options:reflection:` and the render,
  mesh and descriptor forms) already returned reflection objects built from the translator's reflection (bindings,
  argument-buffer fields) and the AIR's argument metadata (names, type names, struct layouts):
  `nvmtl_arguments_for_function` in `NVMTLObjects.m`. Three things were wrong or absent:
  1. a kernel's `[[threadgroup(N)]]` pointers were not listed at all, and because the AIR calls them `air.buffer` with
     a `location_index` of their own, the name table filed them under `buffer:N`: the threadgroup argument's name
     replaced the name of `[[buffer(N)]]` (`out` reported as `tg`);
  2. access came from what the translator saw the shader do (a `device float4 *` only written: write-only), where Metal
     reports the declaration (`air.read_write`: read-write); every writable texture was read-write, also
     `access::write` ones;
  3. buffer alignment was 16 for everything (a `const device uint *` is 4).
- **`-reflection` of a pipeline** (macOS 26) answered nil through the forwarding net, and `MTL4Compiler` ignored
  `MTL4PipelineOptions.shaderReflection`, so a Metal 4 app had no way to get reflection.
- **`MTL4Compiler`** (`NVMTLMetal4.m`, nullmoth 2.9) converted Metal 4 descriptors to Metal 3 ones, but dropped
  `requiredThreadsPerThreadgroup` and the static linking descriptors, and had no archives.

## What reflection is built from

| Metal | source |
| --- | --- |
| buffer / texture / sampler / threadgroup bindings, `index`, `used` | the translator's `bindings` (`Buffer`, `Texture`, `TextureArray`, `StorageImage`, `Sampler`, `ThreadgroupBuffer`; access `Unused` = not used) |
| `name` | AIR `air.arg_name`, keyed `buffer:N` / `texture:N` / `sampler:N` / `threadgroup:N` (address space 3) |
| `access` | AIR `air.read` / `air.write` / `air.read_write` / `air.sample` (Metal reports the declaration) |
| buffer data type, size, alignment, struct members | AIR `air.arg_type_name`, `air.arg_type_size` / the translator's declared size, `air.arg_type_align_size`, `air.struct_type_info` |
| threadgroup memory size and alignment | AIR `air.arg_type_size`, `air.arg_type_align_size` of the pointee |
| texture type, component type, depth | the translator's texture shape, the AIR type name (`depth2d`, `<uint>`, `<half>`) |
| stage-in vertex buffers (`vertexBuffer.N`, not an argument) | the vertex descriptor and the vertex function's attributes |

The reflection classes are the plugin's own (`NVMTLComputePipelineReflection`, `NVMTLRenderPipelineReflection`,
`NVMTLArgument` and subclasses conforming to `MTLBufferBinding`, `MTLTextureBinding`, `MTLThreadgroupBinding`), as
before: Metal's `MTLComputePipelineReflection` and `MTLArgument` are public classes but have no public initializers or
setters, so objects that answer the same getters are what the plugin already returned (apps and Apple's GL read them
through the protocols and getters, and `isKindOfClass:` against them is not something any app was seen to do).

## `-reflection` and `shaderReflection`

On Apple GPUs a pipeline's `-reflection` is nil unless the pipeline came from an `MTL4Compiler` with
`MTL4PipelineOptions.shaderReflection` set (checked on an M1: Metal 3 pipelines answer nil, also when their
creation returned reflection through the out-parameter). The plugin now does the same: the compiler maps
`BindingInfo` / `BufferTypeInfo` to `MTLPipelineOptionBindingInfo` / `BufferTypeInfo`, asks the Metal 3 path for
reflection only then, and keeps the object on the pipeline (an associated object read by a `-reflection` category on
`NVMTLComputePipelineState` and `NVMTLRenderPipelineState`). The Metal 3 out-parameter is unchanged (the plugin fills
it whatever the options say, which apps have relied on).

## MTL4Compiler

| Metal 4 | Metal 3 path |
| --- | --- |
| `MTL4LibraryFunctionDescriptor`, `MTL4SpecializedFunctionDescriptor` | `newFunctionWithName:`, `newFunctionWithDescriptor:error:` (as before) |
| compute: `maxTotalThreadsPerThreadgroup`, `requiredThreadsPerThreadgroup`, ICB support | `MTLComputePipelineDescriptor` fields |
| `staticLinkingDescriptor` (compute), `vertex/fragmentStaticLinkingDescriptor` | `MTLLinkedFunctions` (functions, private functions, groups by function name) on `linkedFunctions` / `vertexLinkedFunctions` / `fragmentLinkedFunctions` |
| render, mesh pipelines | `MTLRenderPipelineDescriptor`, `MTLMeshRenderPipelineDescriptor` (as before) |
| specialization (`newRenderPipelineStateBySpecialization...`) | a full build (as before) |
| pipeline data set serializer (`CaptureBinaries`) | records the archive key of each pipeline a compiler with it builds |
| `serializeAsArchiveAndFlushToURL:` | a binary plist `{format NVMTL4Archive, version, keys}` |
| `newArchiveWithURL:` / `MTL4Archive` lookups | the key list; a lookup of a pipeline it holds builds it through the compiler, any other fails (`MTLArchiveDomain`; a missing file is code 1 "Invalid URL", as Metal says) |

The keys are those of the Metal 3 binary archive (`nvmtl_archive_key_compute` / `_render`: function name, AIR hash,
function constants). There is no GPU binary to store: ANV compiles SPIR-V and caches it itself, and the plugin's
translation cache keeps the SPIR-V, so a lookup that hits costs what a cached compile costs. A binary function the
compiler makes is recorded too (`binary:` and its function's key) and `MTL4Archive newBinaryFunctionWithDescriptor:`
finds it; the archive's dynamic-linking lookups pass the descriptor on (they ignored it before). Not built:
`lookupArchives` (the compiler compiles anyway). Binary functions and dynamic linking: next section; tile and
machine-learning pipelines and pipelines scripts: refused (below).

With `NVMTL_METAL4` off nothing here is installed: `supportsFamily:MTLGPUFamilyMetal4` answers NO and the device keeps
Metal's own Metal 4 entry points (`mtl_m4_inherit`, which now also checks `newPipelineDataSetSerializerWithDescriptor:`).

## Binary functions and dynamic linking

What the plugin already had (Metal 3): `[[visible]]` functions, `MTLVisibleFunctionTable` and
`MTLIntersectionFunctionTable` are emulated by linking. A kernel, vertex or fragment function that reads a visible
function table is linked at pipeline creation with the pipeline's linked functions (`nvmtl_link_stage`,
`NVMTLLibraryLoad.m`); the translator (`nvmtl_link_runtime`, `specialize_runtime_table_module`) replaces each table
call by an `OpSwitch` over the linked set, case `k + 1` calling the k-th function, and a function handle's ID is that
`k + 1`. A table is a buffer of handle IDs the app fills with `setFunction:atIndex:`; the shader reads the slot and
switches. Direct calls to `[[visible]]` externs resolve to the linked function's body, and `MTLDynamicLibrary` calls
through `preloadedLibraries` / compile-time libraries link the same way (`nvmtl_link_dylibs`). So linking is inlining
plus switch dispatch; there is no function pointer at the SPIR-V level.

What was missing, and what it is now:

| API | before | now |
| --- | --- | --- |
| `MTLLinkedFunctions.binaryFunctions` (Metal 3; functions made with `MTLFunctionOptionCompileToBinary`, intersection functions of ray-tracing apps among them) | **ignored**: a kernel whose table holds only binary functions linked an empty set and failed ("the pipeline links no functions"); handles were nil | linked like `functions`, after them: the table list is `functions` then `binaryFunctions` (`nvmtl_link_table_list`), so each gets a switch case and a handle |
| `MTLFunction.options` | always None | the descriptor's options (`CompileToBinary` answered; the function object is the same either way: AIR, linked per pipeline) |
| `newComputePipelineStateWithAdditionalBinaryFunctions:` / `newRenderPipelineStateWithAdditionalBinaryFunctions:` | not implemented (the forwarding net's answer) | the pipeline built again from its kept descriptor with the added functions appended to `binaryFunctions`: the switch gets a case for each, and every earlier function keeps its handle (same place in the list). Tile additions are refused |
| `MTL4Compiler newBinaryFunctionWithDescriptor:` (and the task form) | error | `NVMTL4BinaryFunction`: the descriptor's name and its function (`nvmtl_m4_function`); Apple's M1 makes one from any function, so does this |
| `MTL4PipelineStageDynamicLinkingDescriptor` on compute and render pipelines | logged and ignored | `binaryLinkedFunctions` -> the Metal 3 stage's `binaryFunctions` (after the static linking descriptor's functions), `preloadedLibraries` -> `preloadedLibraries` / `vertex/fragmentPreloadedLibraries`, `maxCallStackDepth` -> the Metal 3 depth; `supportBinaryLinking` -> `supportAdding...BinaryFunctions` |
| `functionHandleWithBinaryFunction:` (`:stage:`), `functionHandleWithName:` (`:stage:`), `newComputePipelineStateWithBinaryFunctions:`, `newRenderPipelineStateWithBinaryFunctions:` (macOS 26) | not implemented | the handle of the binary function's `MTLFunction`; by name only functions linked at the IR level (static linking), as on the M1 (a binary function's name answers nil there); the additions as above |

A pipeline now keeps its own handle list per stage (`nvmtl_pipe_keep_handles`). Before, the list lived only on the
linked function object, and a kernel that needs no link *is* the caller's function object, shared by every pipeline
made from it: the first pipeline's linked functions answered `functionHandleWithFunction:` for all of them.

Not linked: object and mesh functions (the translator's link entry points are vertex, fragment and kernel), so static or
dynamic linking into them is refused with an error (the mesh pipeline's object and mesh static linking descriptors were
dropped silently before); tile functions (no tile pipelines). `maxCallStackDepth` is passed on but means nothing here:
calls are inlined, so recursion through a table is not possible either way.

### Pipeline-independent binary functions

`MTL4BinaryFunctionOptionPipelineIndependent` promises one handle value for a function in every pipeline that links it,
so an app may fill one table and use it with several pipelines. Before branch `mtl4icb` it was refused: a handle was the
function's place in each pipeline's list, and two pipelines agree on it only when both list the function at the same
place. Now:

- `newBinaryFunctionWithDescriptor:` gives the function object (a new one per binary function: the library makes a fresh
  `NVMTLFunction` per lookup) a process-wide value, `nvmtl_mark_independent`: 0x40000000, 0x40000001, ... (the translator's
  `linked_functions::INDEPENDENT_ID_BASE`). Places stay 1..N below it, so the two never meet.
- A handle is `nvmtl_handle_value(function, place)`: the independent value, or the place + 1 as before. The handle lookup
  matches the function object first and its name only after (a list may hold a pipeline-independent binary function and a
  plain one of the same library function, whose handles differ).
- The link (`nvmtl_link_stage`) passes each table candidate's value to the translator through `nvmtl_link_runtime_ids`
  (wrapper), which calls `specialize_runtime_table_module_with_ids`: the switch that replaces a visible-function-table
  call gets case `value` for that candidate, and so does the intersection-function-table switch (`ift_runtime`, the IDs
  the plugin writes into an intersection table). Without an independent function the plugin calls `nvmtl_link_runtime`
  as before, and `_with_ids` given the places produces the same module text (translator test `runtime_table_ids`), so no
  existing link changes.
- The linkage's `size` still counts places (it is what `get_size_visible_function_table` answers: `entries + 1`, as
  before); validation allows an entry at or above the base past it, and refuses 0, a value between N and the base, and two
  equal values.
- The switch selector is the low 32 bits of the table's 64-bit slot, so 2^30 values are available; past that the option
  is refused with `MTLDeviceErrorNotSupported`.

Checked off-hardware (`user/tests/plugin/link_host`, the plugin loaded with no GPU, against this build: 17/17; branch
`mtl4icb`: 18/18, the pipeline-independent checks in place of its refusal): a
kernel and a fragment function linked with `binaryFunctions` give SPIR-V with a switch case per function (spirv-val
clean), handles 1/2, then 1/2/3 with a function appended; `functions` then `binaryFunctions` order; names; a binary
function without AIR is an error naming it; the Metal 4 descriptors become the Metal 3 ones listed above (a stand-in
device records them); the refusals below.

## What is refused, and why

Each refusal is `MTLDeviceErrorDomain` code 1 (`MTLDeviceErrorNotSupported`, macOS 26.4: "the requested feature is not
supported by the current device"), with the feature named in the message, logged with `REFUSED` (kept in the release
syslog). No Apple GPU refuses these (Metal 4 needs Apple silicon, which has tile memory and runs ML pipelines), so there
is no Apple refusal to copy; this is the code Metal defines for it.

- **Tile pipelines** (`newRenderPipelineStateWithTileDescriptor:` Metal 3 and both completion forms,
  `MTL4TileRenderPipelineDescriptor`, tile linking descriptors, tile additional binary functions). Not emulated:
  1. the GPU has no tile memory; a tile function reads and writes the tile's colour attachments through implicit
     imageblocks, so an emulation would end the render pass at each `dispatchThreadsPerTile`, run a compute pass over
     the attachments (storage-image access at tile origin + thread position, threadgroup memory as the explicit
     imageblock) and start a new pass that loads them. Metal render targets are made without shader-write usage and
     may be multisampled, so the attachments would need storage usage or copies;
  2. the translator has no tile stage (its entry points are vertex, fragment, kernel), and fragment functions that use
     explicit imageblocks (`[[imageblock_data]]`) keep per-pixel data across draws of the pass, which without tile
     memory is a per-pixel buffer the size of the imageblock for the whole target, read and written by fragment
     shaders in order (fragment shader interlock) - the part where correctness would cost most;
  3. the answers stay consistent: `supportsFamily:` is NO for every Apple family (tile shading is Apple4 and later),
     so an app that asks does not use tile shading, and one that does not ask gets a nil pipeline with the error
     instead of reaching Metal's own implementation of these selectors (which compiles for the GPU it is). Nothing
     binds a tile stage: the Metal 4 render encoder drops `dispatchThreadsPerTile:` (logged) and does not push
     argument tables set for `MTLRenderStageTile`. Reflection has no tile bindings, as there are no tile pipelines.
- **Machine-learning pipelines** (`newMachineLearningPipelineStateWithDescriptor:`, the completion form; the
  command buffer's `machineLearningCommandEncoder` answers nil). They run Core ML packages on Apple's own hardware path;
  there is none here. The M1, given an empty descriptor, answers `MTLLibraryErrorDomain` 3 "Function (null) not found";
  this driver answers the refusal for every descriptor.
- **Pipelines scripts** (`serializeAsPipelinesScriptWithError:`). The M1 writes a JSON script (version, generator
  `MetalFramework`, libraries by UUID, function descriptors, pipeline descriptors) whatever the serializer's
  configuration; it is the input of Apple's offline binary generator, which makes binaries for Apple GPUs only.
  Archives (`serializeAsArchiveAndFlushToURL:`) are this driver's offline path and stay.

## Tests

- `user/tests/plugin/refl_host` (no GPU, x86_64): loads the plugin, feeds `nvmtl_compute_reflection` /
  `nvmtl_render_reflection` the AIR and translator reflection of `user/tests/plugin/refl/` (the shaders of
  `mtl_reflection`, compiled by this Mac's Metal compiler) and checks the result with
  `user/tests/metal/mtl_reflection_expect.h`, Apple's answers. main: 12 of 19 checks; this branch 20/20.
- `user/tests/metal/mtl_reflection`: the same shaders on a device, Metal 3 and Metal 4 paths. 42/42 on an M1 Pro.
- `user/tests/metal/mtl4_compiler`: library, compute (plain, specialized, threadgroup limits, asynchronous, static
  linking), render (specialized fragment, specialization), serializer and archive; each pipeline runs. 14/14 on the M1.
- `user/tests/metal/mtl4_linking`: Metal 3 binary functions through a kernel's and a fragment function's table, the
  additional-binary-function pipelines, the tile pipeline (made and run where the device has tile shading, refused
  elsewhere); Metal 4 binary functions, dynamic linking (compute, render, preloaded libraries), handles by binary
  function and by name, the pipelines with binary functions added, a pipeline-independent binary function whose handle
  from one pipeline dispatches it from the table of another that lists it at another place (and back), and the refusals
  (each case also passes with Apple's answer: made and correct, or nil with an error). 18/18 on the M1 Pro (arm64 and
  x86_64; again 18/18 on 13 Nov with case 13 checking both directions). On the B580: 6 PASS 1 SKIP with Metal 4 off, 18
  PASS with `NVMTL_METAL4=1` expected.
- `user/tests/plugin/link_host` (no GPU, x86_64): above; its AIR fixtures (`user/tests/plugin/link/`) were made by
  `link_host --make` on the M1 (Metal's compiler, then the plugin's own metallib reader).
- Night: `user/tests/metal/night-mtl4.sh` (phase R; phase Rb for the `mtl4more` stage `~/b580/mtl4b`).
