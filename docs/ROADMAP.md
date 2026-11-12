# Roadmap

The order is roughly the intended one. Every item should follow the Linux xe/i915 and Mesa way of doing it, not a macOS-only shortcut (see the README's philosophy section).

## Near term (Arc B580, and the B570 with it)

### VideoToolbox
- **Done (2026-10-29):** AV1 decoder and HEVC/H.264/AV1 encoders, registered in-process by the plugin in every Metal process (VTDecoderXPCService included); FFmpeg shared.
- **Done (2026-10-30):** H.264 and HEVC decoders; encoder quality work after media-driver (Mesa `0040`–`0045`); FFmpeg 9.0.2 with the patches in `videotoolbox/ffmpeg/`.
- **Done (2026-10-30):** AV1 encode with one B frame, the HuC BRC GOP model and two PAK passes (Mesa `0046`/`0047`, codecs-vqa5). 1080p60 VBR 3/6/12 Mbit/s, BD-rate against SVT-AV1 on Netflix Aerial: PSNR-Y +161.8% → +135.7%, VMAF +89.8% → +68.6%. Known limitation: the rate overshoots by 16–18% at the lowest rate (3 Mbit/s).
- **Done (in the sources since 0.1.0, AV1 registered by default since 0.1.4):** AV1 with the 7-B, 3-level dyadic pyramid,
  as HEVC (`encoder/intelxevenc.c`; FFmpeg `vulkan-encode-multiref-h264-pyramid-av1-pyramid` and `dyadic-reference-pattern`,
  Mesa `0075`). The 11 Nov entry here that listed it as still to do was wrong. Checked on 12 Nov (`vtencq --codec av1`,
  1080p VBR, `NVMTL_PROCESS_ENV=1 INTELXEVENC_BFRAMES=1` for the old single B frame): the overshoot is gone (`noise` at
  3 Mbit/s +22.5% with one B frame, -5.4% with the pyramid; 6 Mbit/s +4.3% -> -5.6%) at equal or better PSNR-Y for the
  bits (~+0.3 dB at 3.7 Mbit/s on `noise`); the release plugin forgets inherited `INTELXEVENC_*` knobs unless
  `NVMTL_PROCESS_ENV=1`.
- **Next:** BD-rate against SVT-AV1 on Netflix Aerial with the pyramid (last measured with one B frame: PSNR-Y +135.7%), and
  whether CDEF under rate control (`ffmpeg-vulkan-encode-av1-enable-cdef-under-rc.patch`) gains on top.
- GPU copies, decode side: done (intelxeav1: each picture is copied by the copy engine into the pixel buffer's IOSurface,
  imported once per surface). Encode side, measured on 11 Nov 2026 (HEVC 1080p, 300 frames, 8 Mbit/s, vtenctest): letting
  libavutil import each pixel buffer's IOSurface as host memory (`VK_EXT_external_memory_host`, `host_map_frame`) gave
  the same bitstream at 245 fps against 352 fps with its staging copy: an import per frame costs more than the ~0.3 ms
  CPU copy of a 1080p frame. Not kept. What is left there is an import cached per IOSurface (as the decoder does) and
  our own buffer-to-image copy synchronised with libavutil's frame semaphores: at most ~10% at 1080p.
- **Next:** QuickTime/Safari playback checks; drive Vulkan Video directly where libavcodec adds nothing.
- **Upstream:** H.264 and AV1 encode for Xe2 in ANV (`mesa/patches/0010`, `0012`, `0015`).

### CPU-visible VRAM (BAR window)
- **Done:** page-table reserve, spills to system memory, map-time moves to system memory, working CPU unmaps, `debug.intelxe.vram`; plugin descriptor pools that grow and a dense set layout (window per drawing process 100 -> 30 MB).
- **Watch (no measured shortage since 6 Nov):** a BO the CPU has mapped never leaves the window (an IOKit mapping of a physical range cannot be repointed, unlike Linux's faulting mmaps), so placement is first come, first served. Options if it fills again: fewer persistent mappings in Mesa's internal pools, or a revocable mapping scheme. macOS cannot use a BAR larger than 1 GB (OpenCore `ResizeAppleGpuBars` documentation). The pressure measured on 5 Nov (24 processes, Minecraft spilling 137 MB) went with the smaller bindless heap and background processes' CPU-visible BOs in system memory (6 Nov); on 12 Nov, after ~40 Geekbench runs and the desktop: 194 of 1024 MB used, 0 refusals, 0 spills. The Manager's Monitor page shows the window and its refusals.
- **Upstream:** the ANV mutable-descriptor-list indexing fix (`mesa/patches/0013`).

### VRAM-shared IOSurfaces
- **Done:** on at boot with `intelxe_surfvram=2` (applications and WindowServer), as the 0.1.x install guide sets it; frames stay in VRAM from render to scan-out instead of crossing PCIe twice per frame (90 % of a game frame's GPU time). The kernel's own default is still 0 (off) without the boot-arg.
- Open: make it the kernel's default once more machines have run it.

### Submission cost and latency
- **Done (0.1.4):** the Metal driver's commit and its execution worker no longer share one class-wide lock (lock V2, on
  by default for applications; WindowServer and the system's interface keep the class lock): 18.2 -> 11.1 us per
  command buffer back to back, Geekbench 7 Metal 89.0k -> 95.4-95.7k, OpenCL ~81.7k -> 84.1-85.5k on the B580.
- **Done (0.1.4, prototype, off by default):** direct submission in `IntelXe.kext` (`debug.intelxe.direct`): an
  application's render or compute queue keeps an armed slot on its ring (`MI_SEMAPHORE_WAIT` on a mailbox, then a jump
  to the job), so a job starts without a GuC round trip; WindowServer and the system's interface never arm. A tiny
  job's exec -> signal 46-52 -> 15-17 us; Geekbench 7 Metal 97.2k, OpenCL 92.9k. Design:
  [`design/DESIGN-direct-submission.md`](design/DESIGN-direct-submission.md).
- **Unattended tests (11-12 Nov, 0.1.4 kext, `direct=1`):** three rounds of the quick regression, Geekbench 7 Metal and
  OpenCL, 30 min idle and a 4 min system sleep: every check passed, no GT or GuC reset, no ban, no CAT error; Metal
  97.5k-98.9k, OpenCL 93.3k-94.0k. Card power (PMT energy over the interval, `direct` alternated 0/1/0/1): idle 36.5 W
  either way (nothing is armed without work); a stream of tiny compute jobs 43.6 W -> 50.5 W with ~10% more jobs done,
  the GT polling its mailbox while jobs come less than `direct_idle_us` (2 ms) apart. `direct_lat`, compute queue:
  exec -> store visible 27-49 -> 6.4-6.5 us, exec -> signal 37-60 -> 15-17 us (gaps 20 us-1 ms); 5 ms gaps unchanged
  (the queue has left by then).
- **In this tree (12 Nov, untested on hardware):** on by default: the kext switches it on once the device is up;
  `-intelxe_nodirect` keeps it off, `debug.intelxe.direct=0` turns it off until the next restart.
- **Open:** the GuC `MEMORY_CAT_ERROR` after a long idle (3 of 6 cold runs with the GuC low-latency hint on on 9-10 Nov,
  0 of 4 more on 12 Nov (50 min idle, then Geekbench OpenCL and Metal first; direct off); 0 of 4 without; the replacement queue never starts and a GT reset follows). The hint is opt-in until the root cause is
  known; the engine dump in the G2H log (0.1.4) is there to catch the next one. Two such faults on 9-10 Nov came
  without a page-fault descriptor (a VM without fault mode touched an unmapped address); the candidates are a BAR-window
  BO move or an unbind racing a job or its TLB invalidation, and the dump now records the moves, the TLB invalidations
  and the VM's last unbind, remap and bind (10 Nov).
  12 Nov analysis ([`design/DESIGN-cat-fault.md`](design/DESIGN-cat-fault.md)): the 12 Nov fault was inside a batch
  (ring head just past its `MI_BATCH_BUFFER_START`, normal ring path, not an armed slot), in a scratch VM with no
  translation fault, 21 ms after an unbind whose invalidation had completed, no BO move since boot. Against Linux xe
  the kernel freed unbound memory (page-table pages, BO VRAM, userptr pages) as soon as the TLB invalidation was done,
  while xe also waits for every job the VM had in flight (TTM delayed delete on the VM's dma-resv); a split 2 MB leaf
  was invalidated with only the 4 KB pages under it (xe: the whole leaf, >= 16 MB); a new table was linked without a
  fence after its write-combined fill. **In this tree (rb8c, untested on hardware):** deferred frees wait for the VM's
  jobs as well, splits invalidate the whole leaf, new tables are fenced, binds end with `xe_device_wmb`, the migrate
  windows get their own invalidation; `debug.intelxe.stats` counts frees held by jobs, splits, detached and leaked
  tables and read-only userptr binds, and the CAT dump adds the context's PDP0, page-table walks of the batch
  addresses and a VRAM-against-shadow check of the VM's tables. `user/tests/kernel/bindstress.c` churns binds and
  unbinds under running render and compute jobs for N minutes; host test `kernel/IntelXe/tests/tlbinval_test.cpp`.

### Compute (OpenCL and Metal compute)
- **Done (0.1.4):** the translator turns an `or` of disjoint bits back into an add, so Mesa vectorizes the loads it
  indexes: clbench `sgemm_reg` 3778 -> 7130 GFLOP/s (Windows 5456). Apps see an 8.2 GB working set, not 5.1.
- **Next, Geekbench gaps against Windows:** Background Blur (the kernels compile well: SIMD32, no spills; the gap is
  thought to be per-dispatch cost and the MPS path, to be measured on the B580) and Super Resolution; both depend on
  the GPU clock and score lower with the GuC low-latency hint off (the default since 0.1.4). clbench `blur_h` is still
  at 64% of Windows. Tried 12 Nov: the setBytes ring in VRAM behind the BAR instead of system RAM (the kernels read each
  setBytes scalar through two dependent loads, 19% of the GB7 kernels' SEND messages): no gain (same plugin, alternated:
  OpenCL 86.5k/87.4k in VRAM against 87.0k/87.5k in system RAM, Metal 92.1k against 92.3k), not kept. Those loads are
  not what Background Blur waits on; the inlining of the scalars into the table slot (a SPIR-V pass, prototyped offline
  on 4 Nov) would remove the same loads and is not expected to gain more.
  Measured the same day: Shared buffers in VRAM (`NVMTL_SHARED_VRAM=1`) gave nothing either (OpenCL 86.9k/87.3k against
  87.2k/87.3k). The plugin's KTIME during Background Blur: the GPU is busy 11-12% of the wall time, and a CPU sample of
  Geekbench shows its main thread mostly waiting (Geekbench's own `nanosleep`, then `clWaitForEvents` on the GL-based
  OpenCL's fences; Apple's OpenCL also finishes the whole queue on every `clEnqueueWriteBuffer`). Completion latency of
  a tiny command buffer (`mtl_lat split`, direct submission on): commit -> GPU start 15 us, GPU 8 us, GPU end -> waiter
  awake 25 us, of which the plugin takes ~7 us (fence return -> broadcast 4.3 us, broadcast -> waiter 2.6 us,
  `NVMTL_DONE_SPLIT=1`) and the kernel's interrupt -> syncobj -> wake-up the rest. Polling the fence for up to 100 us
  before the kernel wait (`NVMTL_FENCE_SPIN_US=100`, opt-in) takes commit -> waiter 34.2 -> 28.3 us (p50) but moves
  neither Geekbench score (OpenCL 87.0k/88.3k against 87.6k/87.4k, Metal 92.7k against 92.1k): Background Blur is not
  bound by the per-command-buffer round trip. Next there: where Geekbench's own waits come from (its OpenCL timing
  loop), and the 442 KB `newBufferWithBytesNoCopy` copy for an unaligned host pointer that is re-synced at every
  submission (import the page-aligned range around it instead). Scores here are from `gb_compute_profile.sh`, which
  reads ~6% below `tools/regress` (93.5k OpenCL) on the same build; compare within one harness only.
  The unaligned host pointer is imported through the pages around it since 12 Nov, opt-in (`NVMTL_IMPORT_UNALIGNED=1`):
  Apple's OpenCL hands `newBufferWithBytesNoCopy` malloc-aligned pointers, which were copied and re-synced at every
  submission. OpenCL 87.7k -> 89.0-89.4k (six runs), but one of those six met the GuC `MEMORY_CAT_ERROR` (render queue,
  scratch VM, no fault recorded, an unbind 21 ms before; then a queue that never started and a GT reset), against none
  in ~30 runs without it that day. Importing adds binds and unbinds, the fault's suspect, so it stays opt-in until an
  unattended A/B counts the faults with and without it.
  **In this tree (12 Nov, branch `anv`, untested on the B580): user-fence completion in ANV** (`mesa/patches/0104`,
  `ANV_UFENCE=1`, off by default): every exec with a batch also carries a `DRM_XE_SYNC_TYPE_USER_FENCE` (a per-queue
  sequence number into a cached system-memory page), as Intel's compute runtime does on xe; `vkWaitForFences`,
  `vkGetFenceStatus` and timeline `vkWaitSemaphores` check that memory first and poll it for up to
  `ANV_UFENCE_SPIN_US` (default 100) before the syncobj wait, which stays the path for everything else. It skips the
  kernel's interrupt -> syncobj -> wake-up (~18 us of the 25 us above) for waits that start before the job ends. In the
  kext a job with a user fence leaves direct submission, so with `debug.intelxe.direct=1` it trades the direct start
  for the faster wake-up until the armed slot can carry the fence's store (a kernel change). Night test:
  `user/tests/vulkan/vkfence_lat_night.sh` (staged ANV loaded directly, nothing installed). For Metal apps the plugin
  reads `ANV_UFENCE` only with `NVMTL_PROCESS_ENV=1` (or the per-process options file of `mesa/patches/0020`).
- **Done (0.1.4):** relaxed threadgroup atomics stay relaxed in the translator, so Mesa no longer fences shared local
  memory around them (clbench `hist256`'s loop now matches IGC's).
- **Done (10 Nov, kernel side for Intel's compute runtime):** user fences (`DRM_XE_SYNC_TYPE_USER_FENCE`,
  `DRM_IOCTL_XE_WAIT_USER_FENCE`) as xe has them, and the `GEM_CREATE` extension chain checked as `xe_bo.c` does; the
  compute-runtime items left in `docs/parity/` went from 35 to 24. Host tests in `kernel/IntelXe/tests/uapi_test.cpp`,
  the GPU test in `user/tests/kernel/ufence.c`. A Level Zero / oneAPI runtime on macOS is still to come.

### Metal API coverage
- **Done (10 Nov):** the 31 protocol properties the compiler found unimplemented in the plugin are answered from real
  data (12 of them by Metal's own device class, now declared); an application that asked `supportsPlacementSparse`
  crashed with an unrecognized selector and now gets NO (checked on the B580 with `mtl_props`; `mtl_f32filter` passes
  for `supports32BitFloatFiltering`).
- **Done (10 Nov):** texel reads of sampled cube and cube-array textures keep the cube view their heap slot holds and
  read the right level through the synthesized read sampler, which the plugin now binds (`mtl_cube_read`: every case
  passes on the B580; before, every texel was wrong and cube arrays did not translate).
- **Done (10 Nov, translator 619e00d):** `supportsQueryTextureLOD` and `supportsPullModelInterpolation` answer YES: the
  LOD queries lower to `OpImageQueryLod` on every texture shape, and pull-model interpolation (offsets from the pixel
  corner, as Metal counts them) is translated.
- **Implemented (12 Nov), untested on hardware:** sparse textures, tier 1, behind `NVMTL_SPARSE_TEXTURES=1` (default
  off; a release plugin keeps it with `NVMTL_PROCESS_ENV=1` or the conf file): sparse heaps as tile pools, sparse
  textures as Vulkan sparse-residency images, `resourceStateCommandEncoder` mappings as `vkQueueBindSparse` ordered on
  the queue's timeline, Metal 4 queue mappings of placement sparse textures, `sparse_sample` / `sparse_read` /
  `sparse_gather` residency in shaders (`docs/design/DESIGN-sparse-textures.md`). The gate is `vksparse` (image tiles
  through VM_BIND on IntelXe.kext), then `mtl_sparse` (106/106 on an M1 as the reference). Off, every answer is as
  before (tiers None).
- Open: sparse buffers (`sparseBufferTier` None), sparse tier 2, and a pipeline's reflection nil (no `MTL4Compiler`).

### GPU-encoded indirect command buffers
- **Done (10 Nov, merged; 6/6 on the B580):** shaders that encode draws into an `MTLIndirectCommandBuffer`
  (`render_command::draw_primitives`, `draw_indexed_primitives` with uint16 and uint32 indices, `reset`) translate and
  run. WebKit's WebGPU index-validation helpers `vsICB` / `vsUshortICB` (render bundles, `drawIndexed`) were refused as
  an unrecognised AIR family; they now translate and pass spirv-val. Design (`plugin/nvmtl_icb_layout.h`,
  `translator/.../native/render_command.rs`):
  - An ICB whose `gpuResourceID` is taken (an argument encoder's `setIndirectCommandBuffer:atIndex:`, or the app) gets
    a host-visible record buffer, 64 bytes per slot, and the ID is its device address: the value a shader's
    `command_buffer` holds. Words 0-10 are the draw (kind, primitive type, `VkDrawIndexedIndirectCommand`,
    `VkDrawIndirectCommand`), written by the CPU when the app encodes the slot and by a shader when it does; words
    11-15 are written by the CPU only: the topology the execute uses and the index buffer it binds (address, size,
    index size).
  - The translator lowers each call to plain stores into `address + slot * 64`; it zeroes the counts of the form the
    call does not use.
  - `executeCommandsInBuffer:withRange:` replays each slot's CPU state (pipeline, buffers, depth and raster state, as
    before) and draws it from its record with `vkCmdDrawIndexedIndirect` when the slot has an index buffer, else
    `vkCmdDrawIndirect` (one draw per slot since 11 Nov; a shader's plain draw into a CPU-indexed slot is refused). ICBs nobody took the ID of keep the CPU replay. Blit `resetCommandsInBuffer` and
    `copyIndirectCommandBuffer` also update the records on the GPU timeline. The app orders the shader's writes before
    the execute (render-encoder memory barrier or encoder boundary), as Metal requires; both are already
    `ALL_COMMANDS`/`MEMORY_WRITE -> MEMORY_READ` and cover the indirect-argument read.
  - Test: `user/tests/metal/mtl_gpu_icb.m` (a kernel encodes a draw, two indexed draws whose first index comes from
    the index pointer, and a reset; a rasterizer-less vertex shader empties a CPU draw as `vsICB` does; the index
    pointers are loaded from memory, as `vsICB` loads them). 6/6 on the B580 and on the Apple GPU (`--any`); in
    `tools/regress` as the `gpu_icb` stage. The first B580 run refused both GPU-encoded indexed draws: the emitter
    lowers a `ptrtoint` without an address payload to 0. The lowering's own `ptrtoint`s (`%nvicbN.addr`) now take a
    loaded pointer's device address plus its offset, an entry buffer parameter's address, and `p + k` as the address
    of `p` plus k elements; other shaders' `ptrtoint`s are unchanged. The entry-parameter form is untested on the GPU.
  - **Cost (11 Nov, `user/tests/metal/mtl_icb_cost.m`, 1024 tiny draws, GPU time per slot):** direct draws and the CPU
    replay ~0.04 us; a GPU-encodable ICB 11.2 us per indexed slot and 5.1 us per plain slot. Xe2 ANV executes a
    single-draw `vkCmdDraw*Indirect` with one synchronous `MI_LOAD_REGISTER_MEM` per parameter, and the records were in
    system RAM (a PCIe read each); an indexed slot also issued a second, zero-count draw. Now the records are in VRAM
    through the BAR (`nvmtl_vk_buffer_create` host_visible 3, whatever the Shared policy; the CPU keeps its own copy and
    never reads them back) and each slot issues one draw: 2.6 us per indexed slot, 2.2 us per plain slot (4.3x and
    2.3x). Then runs (11 Nov evening): consecutive slots that replay the same state (the same pipeline, depth and
    raster state and buffer bindings, the same draw form, index buffer and primitive type) are one
    `vkCmdDraw*Indirect` with `drawCount` > 1 over their records (stride 64; `multiDrawIndirect` is now enabled), which
    ANV executes without a register load per argument: 0.030 us per indexed slot and 0.020 us per plain slot, encode
    0.033 us per slot (88x and 110x; the same as direct draws). Slots whose state differs from the previous slot's
    still cost ~2 us each.
- **Limits (core Vulkan takes neither an index buffer address nor a topology from GPU memory).** A GPU-written indexed
  draw must point into the index buffer the CPU encoded in that slot (same index type, inside the buffer, aligned):
  the first index is computed from the pointer. A GPU-written draw must use the topology the execute chose (the
  CPU draw's, otherwise the pipeline's topology class: point, line or triangle list). Anything else is written with
  zero counts and kind 3: nothing is drawn, and the plugin logs the slot after the command buffer completes (up to 8
  times per ICB). WebKit's `vsICB` meets both conditions (it rewrites the CPU-encoded draw with index count 0 and the
  same buffer). The newer WebKit `vsICBIndirect*` helpers (WebKit main, not yet in Safari 26.7) encode indexed draws
  into slots the CPU never filled: those are refused until the next step.
- **Next:**
  - Index buffer by address: `VK_EXT_device_generated_commands` (ANV exposes it for verx10 >= 125, so on Xe2) has
    an index-buffer token that takes a device address from GPU memory. A slot record laid out as a DGC sequence
    (index-buffer token + draw-indexed token) executed with `vkCmdExecuteGeneratedCommandsEXT` would remove the
    "same index buffer" condition. Topology still cannot come from GPU memory: the execute could issue one draw per
    topology of the pipeline's class (two per slot) with the record writing the counts only for its own.
  - `set_pipeline_state`, `set_vertex_buffer` / `set_fragment_buffer` (also refused today): the record would hold the
    pipeline's resource ID and the buffer addresses; buffers through the binding table's addresses (or DGC
    vertex-buffer / push-constant tokens), pipelines through DGC indirect execution sets.
  - `set_barrier` / `clear_barrier` render commands, and compute ICBs (`concurrent_dispatch_threadgroups`,
    `set_kernel_buffer`, `set_compute_pipeline_state`: a `VkDispatchIndirectCommand` per slot, buffers through the
    push-constant address table) - refused today.
  - Run `mtl_gpu_icb` and Safari's WebGPU render-bundle samples on the B580.

### Descriptor model
- Stage 1 (front end without per-draw atomics or retain/release, address table, bind cache): done. Stage 2, the descriptor-buffer model, is in the plugin behind `NVMTL_DESC_MODEL=buffer` and off by default until it has been soaked (`docs/design/DESCRIPTOR-MODEL.md`).
- Pools: done (see the BAR window). A full pool set now waits for in-flight work and then fails the command buffer with a Metal error, never silently.
- WindowServer and app frames still cost a lot of CPU time in descriptor and submit handling. Minecraft profiling showed descriptor-set allocation as the top cost before the recycling fix.
- The goal is a descriptor model that maps Metal's argument buffers and bindless resources onto ANV's descriptor heaps (`VK_EXT_descriptor_buffer` style) instead of per-draw descriptor sets.

### Variable refresh
- **Done (2026-10-29):** Adaptive-Sync with an LFC pacer (monitor 48–180 Hz), and the "Variable" entry in System Settings.
- **Done (2026-11-06, 0.1.3):** refresh rates above the firmware's mode (EDID timings on the firmware's raster at a higher pixel clock), so 180 Hz and Variable are listed from the first boot; Adaptive-Sync modes on by default.
- Next: tests with games and video playback.

### HiDPI modes
- **Done (2026-11-06, 0.1.3, opt-in):** scaled "looks like" sizes in System Settings > Displays at the monitor's full refresh rate. The modes are accelerator backed: WindowServer renders at twice the size and scales on the GPU, the display engine scans out the native size (its own plane scaler would hold HiDPI to about 100 Hz at this display clock). `hidpi.sh` / the app's Display page turns them on (NVRAM flag, display override, DisplayResolutionEnabled).
- Open: sizes for monitors other than 2560×1440 are computed but untested.

### GPU resets
- **Open:** the root cause of the GuC `MEMORY_CAT_ERROR` after a long idle (see Submission cost and latency; analysis
  and the rb8c changes, untested on hardware: [`design/DESIGN-cat-fault.md`](design/DESIGN-cat-fault.md)). Also open:
  after the CAT error the render engine kept the dead context and the next queue never started until a GT reset.
- **Done (2026-10-30):** register save/restore lists for engine resets, queue replacement on the same VM in Mesa and the plugin; WindowServer survives a GT reset.
- The full Vulkan CTS run of 2026-11-04 (3.15 M cases) finished without a hang, a GT reset or a lost device; the three cases that used to hang (a timeline blit, an R8G8B8 sRGB multi-layer clear, a transform-feedback teardown) pass or fail cleanly.
- Open: deliberate hang tests on the released kernel (the last ones ran on rb5/rb6b).

### Browsers
- **Done (2026-10-30):** Chrome/Electron in their GPU sandbox; Safari WebGL through the optional system-volume plugin copy.
- **Done (10 Nov, translator):** Safari WebGPU's argument-buffer textures, which WebKit reads as 64-bit resource IDs:
  the webgpu-samples deferredRendering sample failed with "Fragment function failed creation" and its G-buffer reads
  were zero; all 88 WebGPU libraries Safari compiled on the B580 now translate (one failed). Textures merged by a phi or
  select read the chosen heap slot (no captured shader has the pattern; untested on the device).
- **Done (11 Nov):** the WebGPU samples in Safari on the B580: 8 of 8 draw (helloTriangle, computeBoids, particles,
  shadowMapping, deferredRendering, bitonicSort, gameOfLife, renderBundles). Argument-buffer samplers merged by a phi or
  a select (their field addresses) read the chosen field's sampler-heap slot: the phi already did through the raw
  pointer merge; the select was refused ("unknown SSA value": the call's argument check wanted a value the raw select
  does not make). Tests `tests/sampler_phi.rs`; no captured shader has the pattern.
- Open: the system-volume install breaks the sealed snapshot (as OpenCore Legacy Patcher's root patches do); a macOS update removes it (`ssv-install.sh --check`).

### Hybrid Intel CPUs (HybridPM)
- **Done (0.1.4):** `HybridPM.kext` published (`kernel/HybridPM/`, a Lilu plugin): it corrects XCPM's HWP requests on
  Alder Lake / Raptor Lake hybrid CPUs, which held the P-cores low and busy cores at 800 MHz (i7-14700K: one thread
  ~55 -> 128 M it/s; Geekbench GPU scores about a third higher). It leaves Core Ultra, CPUs without E-cores and other
  CPUs alone. Optional; `efi.sh`, the app's EFI page and the Windows EFI tool (`IntelXe-EFI.ps1`) add it below Lilu at
  stage 4 when the CPU needs it (Lilu is required and not shipped). The U and P laptop models (e.g. i7-1255U, i7-1360P)
  are recognised too.
- **Next:** run the Windows EFI tool's HybridPM step on Windows (checked on macOS against `efi.sh` only).

### Idle power (PCIe ASPM)
- **Done (10 Nov):** card and package energy and average power in `debug.intelxe.sensors`, from the BMG PMT telemetry
  as Linux `xe_hwmon` reads it (host test `pmt_test`).
- **In this tree, untested on hardware (branch `power-limits`):** a pcode mailbox read helper modelled on Linux
  `xe_pcode.c` (`xe_pcode.h`, host test `pcode_test`), and through it the card and package PL1/PL2 and the card's I1
  in `debug.intelxe.sensors`, read only (`READ_PL_FROM_FW`, as xe before it writes the limits back; nothing is
  written). A power window whose package power exceeds the card's by more than a quarter plus 10 W, or either above
  600 W, is reported n/a and counted ("power windows rejected"): right after a boot the B580 once read package 85 W
  and then 152 W against a card 37 W.
- Measured on 10 Nov 2026 through the same PMT registers (read-only reads): idle card power 36.2 W (package
  19.6 W), light load 40.4 W (package 23.3 W). Intel's documentation for Arc B-series ties low idle power to PCIe ASPM L1 on the card's
  path (root port, the card's own switch, the GPU); Windows with ASPM idles far lower.
- **Experimental, untested on hardware (since 10 Nov in this tree, in the 0.1.4 kext):** `debug.intelxe.aspm` reports every link between
  the GPU and its root port (Link Capabilities/Control/Status, Device Capabilities, L1 PM Substates) and whether L1
  passes Linux's checks; `debug.intelxe.aspm_l1=1` turns L1 on through `IOPCIDevice::setASPMState` where every check
  passes, `0` turns ASPM off, `-1` (default) leaves the firmware's settings. Nothing changes unless it is written.
- **Read on the B580 (12 Nov):** the card's own link (switch downstream port <-> GPU) already runs with ASPM L1 from the
  firmware; the root port 00:01.0 (8086:a70d) <-> the card's switch upstream port advertises no ASPM support in its Link
  Capabilities (bit 11 clear), so `aspm_l1=1` refuses and changes nothing. No device on the path has L1 PM Substates.
  The root port's capability most likely follows the BIOS's PCIe ASPM setting; not tested with that setting changed.
- Next: with a root port that advertises L1, opt in, measure idle power and look for link errors or stalls; then L1
  substates (L1.1/L1.2: LTR, T_power_on, CLKREQ#), which IOPCIFamily only enables with device-tree properties, and
  whether a measured gain justifies an NVRAM setting or an OpenCore `pci-aspm-default` property.

### Integrated GPU
- Out of scope for this stack: the test machine runs with the CPU's integrated GPU off. A headless iGPU next to the B580
  (Quick Sync through VideoToolbox) needs a separate iGPU driver and is not tested with this release.

### Remaining items from the workaround audit (2026-10-29)
- Boot guard: checked over power cuts and failing boots on 5–6 November 2026 (0.1.2: safe settings after two failures, held after four, the clear written by a LaunchDaemon). Open: a held boot on macOS 26 stays at the Apple logo because WindowServer has no display without the accelerator; the way out is OpenCore's Reset NVRAM.
- Video was behind a debug gate; it is now on by default with the media GT up. The ffmpeg vkshim is gone: `user/tests/inv/videotest.sh` uses the Khronos loader with an ICD manifest of its own.
- Remove the AMFI boot-args: sign the plugin properly and use entitlements. **Not possible by signing (12 Nov):** the
  Metal plugin runs inside WindowServer, Safari's GPU process and every Metal application; WindowServer is an Apple
  platform binary with library validation, which loads only Apple-signed code unless the process itself carries
  `com.apple.security.cs.disable-library-validation`, and no signature a third party can make (Developer ID included)
  changes that. The same holds for `DYLD_INSERT_LIBRARIES` into hardened applications (the Zink layer). The boot-arg
  stays a requirement of this design; what can shrink is what else needs relaxed security (SIP's custom value).
- System sleep: works on the released kernel (rb7u + IntelXeFB v10m, macOS 26.7.1). On 0.1.4 (12 Nov, unattended): three
  sleeps of 4 minutes each woke on the RTC timer as asked (no early wake), the display came back each time (full modeset
  from the saved state, link trained at 5.4 Gbit/s x4, 366 ms) and the Metal checks passed after each wake. The PHY
  register dump before that modeset read a PHY that is not powered yet (82 message-bus errors, ~150 ms): it now skips a
  lane that does not answer its first read (IntelXeFB, in this tree, untested on hardware).
- Display hot-plug: interrupt-driven since IntelXeFB v10f (SDE hotplug interrupt, long/short pulses, storm masking; polling
  only without the display interrupt or for a storm-masked pin). Checked on the B580 on 11 Nov 2026: the DisplayPort
  cable pulled and put back gave two long-pulse interrupts (`debug.intelxefb_hpdinfo`: "long: panel port live, link
  ok") and the picture came back at 2560x1440, 179 Hz.
- Second display pipe (pipe B, DBUF/MBUS split, watermarks). The test setup has only one monitor.

### OpenGL through Zink
- **Done, in the release packages from 0.1.2 (`-zink-gl.zip`):** Mesa's Zink for applications that open their OpenGL
  context through GLFW/EGL (LWJGL 3 games). Minecraft 1.21.1 with Sodium, Iris and BSL (HIGH), windowed: 197–205 fps
  against 214–218 on Windows; without a shader pack 1400–1500 against 700–1000.
- Next: the remaining ~5–7% with shaders is GPU work per frame (Windows' GPU is not fully busy, ours is): measure per
  draw (`INTEL_MEASURE=draw`), Zink's multi-draw path, the generated ISA.
- **Done (0.1.4, experimental):** Zink for native CGL/NSOpenGL applications, one application at a time
  (`zink-gl/cgl`, `cglzink-app.sh`: a launcher that starts the app with the layer). GZDoom MAP01 uncapped: ~1910 fps
  against ~1600 on Apple's OpenGL.
- **Done (10 Nov, experimental, off by default):** a switch for every native OpenGL application
  (`zink-gl/install.sh --global-gl on`): with AMFI off a LaunchAgent puts the small `libcglzink-boot.dylib` into the
  session's `DYLD_INSERT_LIBRARIES`, and it loads the layer only into application bundles that are not Apple's (Chess
  excepted), browsers, Electron apps or video players. Not yet tested on the B580.
- **In this tree (12 Nov, untested on the B580):** drawing into IOSurface textures (copied back into the surface at
  each flush once the texture is a framebuffer attachment) and CGL pbuffers (`CGLSetPBuffer`, `CGLTexImagePBuffer`, by
  copies); `test/run.sh` checks the IOSurface case against Apple's GL through the stand-in EGL.
- The LaunchAgent sets `JAVA_TOOL_OPTIONS` at login, after apps that macOS reopens at login may already have started.
  No general fix: launchd loads the agent and loginwindow reopens applications in an order nothing here controls, and
  `launchctl` cannot store an environment variable for later sessions; restarting the application is the way (README).

### Native Vulkan for games (after 0.1.4)
Applications that speak Vulkan reach the GPU today only through MoltenVK -> Metal -> our Metal driver -> ANV, two
translations deep. ANV is already on the machine (Zink uses it directly). The idea, as the NVIDIA fork
[HighDelay/nvidia-macos-driver-nvk](https://github.com/HighDelay/nvidia-macos-driver-nvk) does it on NVK
(same licences: PolyForm Noncommercial, LGPL translator, MIT Mesa):
- **Wine / CrossOver with DXVK (D3D11) and vkd3d-proton (D3D12) on ANV**, bypassing MoltenVK and the Metal translation:
  a Vulkan provider under `/Library/GPUBundles/nvmtl/wine`, a Khronos loader built from pinned source, presentation
  through `VK_EXT_metal_surface` (GPU copies into Metal drawables, macOS composition). Likely the largest gain for
  Windows games on the B580.
- **Minecraft's official Vulkan renderer (Minecraft 26.3, Java 25)** on ANV through a small bridge mod, as the fork's
  `minecraft-nvk`; Minecraft stays our benchmark (general path, no game-specific driver code).
- **Other Vulkan applications** (MPV and apps that let one choose the Vulkan library).
- **Research note (12 Nov):** [`design/DESIGN-native-vulkan.md`](design/DESIGN-native-vulkan.md). ANV on macOS already
  offers Vulkan 1.4 with 217 extensions on the B580, including everything DXVK and vkd3d-proton use except
  `VK_EXT_memory_priority` (optional), and presents through IOSurface-backed `VK_EXT_metal_surface` swapchains without a
  copy (the NVIDIA fork copies each frame). **In this tree (12 Nov, branch `anv`, untested on the B580):**
  `VK_EXT_memory_priority` is offered as a recorded hint (`mesa/patches/0103`): neither xe nor IntelXe.kext has a BO
  priority to give it to, and upstream ANV does not offer it at all (only RADV and lavapipe do);
  `VK_EXT_pageable_device_local_memory` stays off. Missing: a `libMoltenVK.dylib` provider for Wine (the Khronos loader or a
  thin front end, per process), DXVK/vkd3d-proton packaging and a prefix helper, and kernel robustness under D3D
  workloads (the open CAT fault). Next: a native Vulkan sample through the loader, then Wine 11 + DXVK on a D3D11
  benchmark against MoltenVK (needs a Wine build and games chosen by the maintainer).
- **Built, untested on the B580 (12 Nov, branch `wine`):** Wine's source settles the interface: `win32u` dlopen()s the
  name configure found (`libMoltenVK.dylib`, or `libvulkan.1.dylib` in builds against the Khronos loader such as
  Gcenx's) and takes only `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr` and `vkCreateMetalSurfaceEXT` from it (Wine 10
  also `vkCreateMacOSSurfaceMVK`); no MoltenVK private API; surfaces are `VK_EXT_metal_surface` on a `CAMetalLayer`.
  [`vulkan/`](../vulkan/README.md): `libintelxe-vk.dylib`, a thin provider under both names (per process through
  `DYLD_LIBRARY_PATH`, in `/Library/IntelXe/Vulkan`), the Khronos loader as an alternative (`run --loader`),
  `intelxe-vk` (`check`, `run`, `fetch`, `prefix-install`/`prefix-restore` with DXVK 3.1.1 and vkd3d-proton 3.1 pinned
  by sha256; `dxvk.conf` with descriptor buffers and heap off), `user/tests/vulkan/vksurf.m` (a Wine-style swapchain
  with read-back), an optional `-vulkan.zip` (`release/package.sh VULKAN_DIR`). Next: `vksurf` on the B580 (ANV
  directly, through the provider, through the loader), then a D3D11 game with the maintainer's Wine build. Details and
  the night test: the design note.

### IntelXe Manager (the app)
- **Released from 0.1.2:** `app/`, shows the system and the GPU, compares the installed driver with the latest
  release, runs `install.sh` (check, install, repair, undo, uninstall), Zink's script, and `efi.sh` for stages 2 and 4
  in the EFI OpenCore booted from or a folder the user picks. The Windows counterpart for the EFI is
  `windows/IntelXe-EFI.ps1`.
- **Done (0.1.4):** a redesign (Overview with a status header, charts on the Monitor page, a menu bar item, an About
  window); a setup assistant from a fresh macOS install to a verified driver (Safari's setting with stage 2, the
  extras on its last page, an EFI chooser when OpenCore recorded no boot path); the system-volume copy for Safari (and
  OpenCore's `SecureBootModel`); SSDT-ARCOFF adapted to the card's ACPI path and HybridPM at stage 4; diagnostics for a
  bug report (Report page); GPU tests; a check at login.
- The EFI page reads the EFI again when its files change outside the app (a fingerprint of config.plist, ACPI, Kexts and
  Drivers: when the page shows, when the app comes to the front, every 5 s while it is open).
- Release signatures independent of GitHub (11 Nov): `release/signing.swift` (Ed25519, CryptoKit) signs the `.sha256`
  list when `package.sh` runs with `SIGN_KEY`; `release.sh` refuses a release without a signature its app accepts; the
  app (`Signing.swift`) refuses a download whose list is not signed. Next: the key's successor procedure (a new key
  added to the app in a release signed with the old one).

## Other Xe2 (Battlemage) cards

The kext is organized around the graphics IP version (Xe2 / graphics 20.01, media 13.01, display 14.01), not the board.

| Card | Device | What is expected to differ |
|---|---|---|
| Arc B570 | BMG-G21, fewer Xe-cores | **Done (0.1.4):** device ID 8086:E20C in the kexts, `install.sh`, the app, `efi.sh` and the Windows EFI tool (PR #2 by `@1964356263`); topology read from fuses. Not run on hardware yet: needs a tester |
| Arc Pro B50 | BMG-G21 workstation | device ID; VRAM size and BAR layout |
| Arc Pro B60 | BMG-G21 workstation, 24 GB | device ID; larger VRAM (allocator and BAR window are already sized from the hardware) |
| Arc Pro B70 | larger Battlemage configuration | device ID, topology; possibly display/PHY tables |

Since the platform descriptors (`kernel/IntelXe/xe_platform.cpp`, 10 Nov), every BMG-G21 card shares the supported
entry: the B50 and B60 need only their entry in `data/platforms.json` `"supported"` (`tools/parity/devices.py` then
names every copy of the list to change: the `IOPCIMatch` IDs, `install.sh`, `efi.sh`, the app, the Windows EFI tool)
and a `model` name, then a test. BMG-G31 (e220-e223) has its own
entry, refused until it is tested; xe's G21-only SLPC minimum (Wa_14022085890) must become conditional first
([DESIGN-platforms.md](design/DESIGN-platforms.md)).

Each needs:
- the `IOPCIMatch` IDs;
- a check of the per-SKU workaround and tuning tables against Linux xe (`xe_wa.c`, `xe_tuning.c`);
- a check of the firmware it uses (BMG GuC/HuC are shared within the family);
- a hardware test.

## Later generations

These need per-generation modules, the way Linux xe separates them, rather than conditionals spread through one file.
The first step is in place (10 Nov): `kernel/IntelXe/xe_platform.cpp` describes every family of Linux `xe_pci.c` (IP
versions, flags, engines, firmware) and `IntelXe.kext` refuses all but BMG-G21 at start with one log line. What each
family still needs (memory path, LRC and PAT/MOCS tables, workarounds, display) and the order of work:
[DESIGN-platforms.md](design/DESIGN-platforms.md).

| Generation | Examples | Main new work |
|---|---|---|
| Lunar Lake (Xe2-LPG, integrated) | Core Ultra 200V | No VRAM: an integrated memory model with system memory and the stolen region. Different display (Xe2_LPD). GSC-loaded HuC. Shared LLC. |
| Alchemist (Xe-HPG, DG2) | Arc A380/A750/A770 | Graphics 12.55: different LRC layouts, workarounds, MOCS/PAT, GuC ABI subset. Display 13. GSC for HuC and firmware loading. |
| Panther Lake (Xe3) | Core Ultra 300 | Graphics 30.x: new LRC/workaround tables, new display IP. Integrated memory model as for Lunar Lake. |
