# Pipeline reflection and MTL4Compiler (design, 2026-11-13)

> Design note with its date: it records what was built and the state of that day. Status: implemented on branch
> `mtl4`, **not yet run on the B580**; the Metal 4 parts need `NVMTL_METAL4=1` (the layer is off by default). Checked
> off-hardware against Apple's answers (`user/tests/plugin/refl_host`) and on an M1 Pro as the reference.

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
translation cache keeps the SPIR-V, so a lookup that hits costs what a cached compile costs. Not built: binary functions,
dynamic linking descriptors (logged and ignored, as before), tile and machine-learning pipelines, pipelines scripts
(`serializeAsPipelinesScriptWithError:` is an error), `lookupArchives` (the compiler compiles anyway).

With `NVMTL_METAL4` off nothing here is installed: `supportsFamily:MTLGPUFamilyMetal4` answers NO and the device keeps
Metal's own Metal 4 entry points (`mtl_m4_inherit`, which now also checks `newPipelineDataSetSerializerWithDescriptor:`).

## Tests

- `user/tests/plugin/refl_host` (no GPU, x86_64): loads the plugin, feeds `nvmtl_compute_reflection` /
  `nvmtl_render_reflection` the AIR and translator reflection of `user/tests/plugin/refl/` (the shaders of
  `mtl_reflection`, compiled by this Mac's Metal compiler) and checks the result with
  `user/tests/metal/mtl_reflection_expect.h`, Apple's answers. main: 12 of 19 checks; this branch 20/20.
- `user/tests/metal/mtl_reflection`: the same shaders on a device, Metal 3 and Metal 4 paths. 42/42 on an M1 Pro.
- `user/tests/metal/mtl4_compiler`: library, compute (plain, specialized, threadgroup limits, asynchronous, static
  linking), render (specialized fragment, specialization), serializer and archive; each pipeline runs. 14/14 on the M1.
- Night: `user/tests/metal/night-mtl4.sh`.
