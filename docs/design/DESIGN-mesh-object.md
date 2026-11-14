# Metal mesh pipelines with an object stage on the B580

Branch fix016, 14 November 2026. Plugin `nullmoth/plugin/NVMTLLibraryLoad.m` ("batch 42a"), translator
`nullmoth/translator/translator/src/mesh_lower.rs`.

## What 0.1.5 did

Mesh shading is emulated with compute, as nullmoth did on NVK: the mesh function is lowered (AIR to AIR) into a kernel
that writes each threadgroup's vertices, primitive attributes, indices and primitive count into a block of a scratch
buffer, and a generated vertex function (`m2v_mesh_vs`) draws the blocks. With an object stage the object function is
lowered into a kernel too: it writes its grid and payload into a per-object record and raises, with atomic max, the
header the GPU then reads: the indirect dispatch of the mesh kernel (`threadgroups x objects x 1`, bytes 0-11) and the
indirect draw (`vertex count, objects as instances`, bytes 16-31). Layout of the scratch:

| bytes | content |
| --- | --- |
| 0-63 | header: dispatch arguments (0), objects (4), draw arguments (16) |
| 64 - 64 + block | the block dead threadgroups write into |
| `tr` + o x `rs` | object o's record: its grid (16 bytes), then its payload |
| `tr` + objects x `rs` + (o x cap + j) x block | mesh threadgroup j of object o |

On 13 November an object + mesh pipeline (`mtl_desc_paths` "mesh") ran 20 s on the B580 and the device was lost
("vk: slowest submit so far 20348 ms ... GPU RECOVERY", one GuC context reset). 0.1.5 refused such pipelines with an
`NSError` (code 42) instead; `NVMTL_MESH_OBJECT=1` built them.

## The cause

Not the object stage. The mesh lowering rebases the mesh handle of every `air.set_*_mesh` call in the entry onto
`%m2v.mesh` (the scratch plus this threadgroup's block offset) by matching the argument text: `<type> %h` and
`<type> nocapture %h`, the forms of the typed-pointer compilers its fixtures came from. Metal's compiler on macOS 26
writes `ptr addrspace(7) captures(none) %m.coerce`, so the handle was never rebased and every store went relative to
the scratch's start: every mesh threadgroup wrote its block at offset 0. Vertex 0's position landed on bytes 16-31, the
indirect draw's arguments: the draw read float bits as its vertex and instance counts (about 3.2 x 10^9 and 1 x 10^9
for the test's quad) and ran until the GuC reset the context. Reproduced off the device: the test's shaders compiled on
an Apple GPU Mac (`libraryDataContents`), decoded with `nvmtl-air-dis`, lowered with the host-built translator (7336-byte
kernel and 2756-byte vertex SPIR-V, the sizes the B580 log shows): three of the four mesh stores went through
`%m.coerce`.

The same bug hit the other two modes: a mesh pipeline without an object stage drew every threadgroup into block 0 (one
quad visible; right only for a one-threadgroup draw), and `drawMeshThreadgroupsWithIndirectBuffer:` overwrote its
header like the object path.

## The fix

- `rebase_handle` rebases `<type> [parameter attributes] %h` followed by `,` or `)` whatever the attributes
  (`nocapture`, `captures(none)`, `noundef`, `align 16`, ...). Output for the typed-pointer fixtures is byte-identical.
- A `set_*` call in the entry whose handle is not `%m2v.mesh` after that is refused with an error naming it, instead of
  being lowered relative to an arbitrary pointer: a form the lowering does not know can no longer write over the header.
  (In a helper function the handle is the helper's own argument, which the entry passes as `%m2v.mesh`: unchanged.)
- The plugin builds object + mesh pipelines again; `NVMTL_MESH_OBJECT=0` refuses them as 0.1.5 did.

Other translations do not go through `mesh_lower.rs` (only `nvmtl_lower_mesh`/`nvmtl_lower_mesh2` call it): a sample of
247 corpus modules under `~/hackintosh/work` translated byte-identically with the translator before and after.

## Native task and mesh shaders (VK_EXT_mesh_shader)

ANV exposes `VK_EXT_mesh_shader` on the B580 (task and mesh shaders; the CTS `dEQP-VK.mesh_shader` group ran with no
failures in the 4 November run). It was not used here because a task shader can only feed a native mesh shader: the
object stage alone cannot be native while the mesh stage stays the compute emulation. Going native means a second
backend for both stages:

- the translator emitting `TaskEXT` / `MeshEXT` entry points (`OutputVertices`, `OutputPrimitivesEXT`,
  `OutputTrianglesEXT`/lines/points, `LocalSize`), per-vertex and per-primitive output arrays with the fragment
  function's locations, `PrimitiveTriangleIndicesEXT` and `CullPrimitiveEXT`;
- Metal's model rewritten to SPIR-V's: Metal lets any thread call `set_primitive_count` at any point and write outputs
  before it, while `OpSetMeshOutputsEXT` must come first, once, in uniform control flow (outputs buffered in workgroup
  memory and copied out after a barrier); `set_threadgroups_per_grid` from one object thread becomes
  `OpEmitMeshTasksEXT`, which every invocation must reach and which ends the task shader (the grid kept in workgroup
  memory until the end);
- the payload as a `TaskPayloadWorkgroupEXT` variable (`maxTaskPayloadSize`), and the mesh grid limits checked against
  `maxMeshWorkGroupCount`;
- the plugin creating graphics pipelines with task and mesh stages (descriptor and push-constant layouts with the
  `TASK`/`MESH` stage bits in both descriptor models) and drawing with `vkCmdDrawMeshTasksEXT` / `...IndirectEXT`
  (which would also give the object stage indirect draws, still "batch 42c" in the emulation).

That is the route to take for speed (no scratch round trip, no render-pass split per draw); the emulation is kept
because it is correct now and every piece of it runs on paths the rest of the driver already uses.

## Tests

- Translator: `a_handle_with_captures_none_writes_the_threadgroups_block_in_every_mode` (direct, indirect and object
  modes on the current compiler's output: `testdata/mesh_captures_*.ll`), `rebase_handle_skips_any_parameter_attributes_
  and_leaves_other_values`, `a_mesh_intrinsic_on_another_handle_is_refused_not_written_over_the_header`; the 25 earlier
  `mesh_lower` tests unchanged. 28/28.
- `user/tests/metal/mtl_mesh_object` (on a device; `--any` on an Apple GPU as the reference, 4/4 on the M1 Pro):
  object-rows (four object threadgroups launching 0, 1, 2 and 1 x 3 mesh threadgroups, payload colour and row, mesh
  bytes and a mesh buffer), object-redraw (two draws in one encoder, the second with other bindings and a mesh buffer
  offset), mesh-grid (4 x 2 threadgroups without an object stage), mesh-indirect (3 x 1 x 1 from a buffer at offset 16).
- Night phase M (`user/tests/metal/night-meshobj.sh`): those in both descriptor models, `mtl_desc_paths mesh`, the
  `NVMTL_MESH_OBJECT=0` switch, `mtl_smoke`, and the GuC reset counters before and after.
