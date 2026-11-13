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
- **Done in the sources (12 Nov), default off until measured:** that cached import (`INTELXEVENC_UPLOAD=import`: each
  IOSurface imported once, the copy engine copies it into libavutil's frame under its timeline semaphores, fallback to
  the staging copy) and `vtencq --upload copy,import --ring K` to A/B it (stream checksums per mode, the encoder's
  upload/encode ms per frame); `INTELXEVENC_ASYNC` for libavcodec's encode depth (2, a host fence wait per frame). The
  night A/B decides the defaults (`videotoolbox/README.md`, "Encoder upload").
- **Next:** QuickTime/Safari playback checks; drive Vulkan Video directly where libavcodec adds nothing (assessed 12 Nov
  from code reading, README "Driving Vulkan Video directly": for the encoders not bounded as a rewrite; first the encode
  depth, a CPU profile, then a per-image view cache and a completion thread if libavcodec's own time shows up).
- **Done in the sources (13 Nov), default off until the B580 A/B:** the direct Vulkan Video decode path for H.264
  (`INTELXEDEC_DIRECT=h264`; `videotoolbox/vtdirect.c`, `vtdirect_h264.c`, `docs/design/vt-direct-plan.md`): our own
  parameter-set / slice-header / POC / reference-marking code (MIT, libavcodec's conventions) and one command buffer per
  picture, instead of libavcodec's decoder and Vulkan hwaccel; progressive 8-bit Baseline/Main/High, everything else
  (and a stream that changes to it) stays on libavcodec. Off the GPU its pictures, POCs and reference pictures are
  identical to libavcodec's on 6 recorded streams and 89 of the 185 JVT conformance streams (the rest are fields/MBAFF,
  other profiles, or allow frame_num gaps). B580 night A/B of 13 Nov: every picture's MD5 identical on both paths for
  the 92 conformance streams of the kit (89 direct, 3 handed to libavcodec); no timing that night (the timing loop found
  no clip and the summary failed on the missing `timing.txt`: fixed in `cdb745b`, the kit now carries the mediakit
  clips, and `timing.txt` is made before anything else).
- **Done in the sources (13 Nov), default off until the B580 A/B:** the same direct path for HEVC (`INTELXEDEC_DIRECT=hevc`,
  or `h264,hevc` / `all`; `videotoolbox/vtdirect_hevc.c`, design note section "HEVC"): VPS/SPS/PPS, slice segment headers
  to the RPS (short-term sets with inter prediction, long-term pictures), scaling lists, tiles, POCs and the RPS-driven
  DPB as libavcodec 9.0.2 runs them; Main / Main 10 / Main Still Picture 4:2:0, 8 or 10 bits (P010 pictures).
  Off the GPU (`tests/hevc/run.sh --conformance DIR`, against `tests/hevc/mkgolden.py`: FFmpeg's trace_headers, the
  same rules in Python, the POCs checked against libavcodec's decoder): 6 recorded streams (x265 open/closed GOP, slices,
  Main 10, POC wrap with temporal sub-layers; Apple's encoder 8/10-bit) and 144 of the 147 JCT-VC HEVC_v1 conformance
  streams identical; the other 3 change size mid-stream (RAP_B_Bossen_2, VPSSPSPPS_A_MainConcept_1: identical up to
  the change, then libavcodec) or have luma and chroma bit depths that differ (TSUNEQBD_A_MAIN10, which libavcodec does
  not decode either). The mediakit's HEVC 8/10-bit clips (1080p to 4320p) parse with libavcodec's POCs.
  B580 A/B of 13 Nov: 141 of 150 identical, the decode thread's CPU 20-35% lower; checked picture by picture against a
  software reference (design note, "First B580 A/B"): the direct path right wherever its input was intact, including
  RPS_B/C/E, where the libavcodec path aborts (FFmpeg passes DPB index 16 as a Vulkan slot, ANV has 16); 13 streams
  were broken by the kit itself (FFmpeg's `hvc1` muxing drops the mid-stream PPSs; now in-band `hvc1`) and MAXBINS_C
  had a zero-length track (now numbered). The A/B now checks both paths against that reference.
  **Next:** the night A/B again with the new kit (`vtdirect3`); if the direct path is right on every stream with a
  reference and the decode thread's CPU drops, make it the default (VP9 and AV1 after). Report the libavcodec slot bug
  to FFmpeg (and a slot bound check to ANV).
- **Done in the sources (13 Nov), default off until the B580 A/B:** the same direct path for AV1 (`INTELXEDEC_DIRECT=av1`,
  or `h264,hevc,av1` / `all`; `videotoolbox/vtdirect_av1.c`, design note section "AV1"): our own OBU parser (sequence
  header, the whole frame header with tiles, quantizer, segmentation, loop filter, CDEF, loop restoration, global
  motion and film grain, tile groups), the eight reference slots with order hints and saved parameters,
  show_existing_frame (a shown key frame refreshes every slot), DPB slots as libavcodec's Vulkan hwaccel assigns them;
  every frame of a temporal unit decoded (hidden alt-refs), the shown one emitted; film grain applied by the B580 (Xe2)
  into a separate output picture, as libavcodec does. Main profile 4:2:0, 8 or 10 bits; frames of another size than the
  sequence's maximum (reference scaling, switch frames) go to libavcodec mid-stream, with the samples since the last key
  frame replayed into it. Off the GPU (`tests/av1/run.sh --conformance DIR [--sanitize]`, against
  `tests/av1/mkgolden.py`: FFmpeg's trace_headers, the same rules in Python, the shown frames counted by dav1d): 10
  recorded SVT-AV1 streams (hidden alt-refs and show_existing, forward key frames, 10-bit film grain, 2x2 tiles with
  quantisation matrices and segmentation, superres, switch frames, screen content with intra block copy, lossless,
  reference scaling, a still picture) and 243 of the 247 libaom test vectors and FATE AV1 samples identical; the other 4
  are monochrome or have two spatial layers (refused / libavcodec). ASan/UBSan and damaged-sample fuzzing clean.
  **Next:** `phase_vtdirect_av1` on the B580 (identity on 252 conformance streams and the mediakit AV1 8/10-bit clips,
  then the timing); the Argon suite is not in AOM's test-vector bucket and was not used.
- **Upstream:** H.264 and AV1 encode for Xe2 in ANV (`mesa/patches/0010`, `0012`, `0015`).

### CPU-visible VRAM (BAR window)
- **Done:** page-table reserve, spills to system memory, map-time moves to system memory, working CPU unmaps, `debug.intelxe.vram`; plugin descriptor pools that grow and a dense set layout (window per drawing process 100 -> 30 MB).
- **Watch (no measured shortage since 6 Nov):** a BO the CPU has mapped never leaves the window (an IOKit mapping of a physical range cannot be repointed, unlike Linux's faulting mmaps), so placement is first come, first served. Options if it fills again: fewer persistent mappings in Mesa's internal pools, or a revocable mapping scheme. macOS cannot use a BAR larger than 1 GB (IOPCIFamily's `MAX_BAR_SIZE`; [`design/DESIGN-rebar-window.md`](design/DESIGN-rebar-window.md)). The pressure measured on 5 Nov (24 processes, Minecraft spilling 137 MB) went with the smaller bindless heap and background processes' CPU-visible BOs in system memory (6 Nov); on 12 Nov, after ~40 Geekbench runs and the desktop: 194 of 1024 MB used, 0 refusals, 0 spills. The Manager's Monitor page shows the window and its refusals.
- **Open (13 Nov, research only):** `ResizeAppleGpuBars = 10` stays despite ocvalidate's "unstable with macOS sleep-wake": macOS does not restore the Resizable BAR size after sleep, IntelXe.kext does (`gpuResume`, as Linux's `pci_restore_rebar_state`); 9 gets the same warning with half the window, 0 is clean but 256 MB. Not yet seen: whether the restore ever fires on the test board. Night test: sleep loops and window benchmarks at 10 and 9 ([`design/DESIGN-rebar-window.md`](design/DESIGN-rebar-window.md) section 6); kernel diagnostics to add: the restore in the system log and counted in the stats.
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
- **In this tree (rb8e, untested on hardware):** a job with an exec user fence stays on the direct path: the armed slot
  calls a per-job post-stub holding the fence's store (was: such a job made the queue leave, so with ANV_UFENCE=1 no
  queue would stay armed). `debug.intelxe.stats`: "submits N (with a user fence M)".
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
  **In this tree (rb8d, branch `kern2`, untested on hardware):** an unbind's VM_BIND out-fence (syncobj and user fence)
  signals only after its TLB invalidation, as `xe_pt_update_ops_run`, without waiting in the ioctl; the GGTT
  invalidation goes through the GuC on both GTs and is waited for (bounded) when an LRC window is mapped or cleared;
  the suspend/resume copy window is sealed with its own invalidation; an invalidation the GuC never answers counts as
  done after 1 s (logged). `user/tests/kernel/unbindfence.c`. Per-batch invalidation (`tlb_every_batch`) stays off:
  every page-table change already makes each queue's next batch invalidate (DESIGN-cat-fault.md, rb8d D8).
  **In this tree (rb8e, branch `kern3`, untested on hardware):** the surface-copy, VRAM-read and BO-move windows of the
  migrate VM free their pages only after their own invalidation has completed; bind user fences are written by the
  kernel when the bind fence signals (the page wired at bind time, as xe's `user_fence_worker`) instead of being waited
  for in the ioctl; out-fences are ordered per bind queue (`xe_exec_queue_last_fence`), not per VM; the TLB fence
  timeout is Linux's (2.25 s); direct submission keeps jobs that carry an exec user fence (ANV_UFENCE=1). Counters in
  `debug.intelxe.stats` ("rb8e" lines); `unbindfence bindq=N windows=N`.
  **In this tree (rb8f, branch `kern4`, untested on hardware):** an unbind of a VMA whose bind user fence has not
  signaled answers EBUSY (xe `check_ufence`); bind user fences pending at a client's close are still written while the
  process lives (xe `user_fence_worker`, bounded to 15 s); a user fence in a CPU mapping of a VRAM BO is written through
  the BO instead of waiting in the ioctl; D9: with `debug.intelxe.bind_async=1` (default 0) a VM_BIND with pending
  in-fences returns at once and is applied in bind-queue order when they signal (xe `xe_pt_vm_dependencies`: CPU only
  without waits, else a job). `debug.intelxe.stats` ("rb8f" lines); `unbindfence busy=N vramuf=N async=N close=N`.

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
- **Started (12 Nov, branch `levelzero`, `compute/`, `docs/design/DESIGN-level-zero.md`): Intel's compute runtime (NEO)
  on macOS.** Its Linux DRM/xe backend is built unchanged over `user/xemac`: NEO sends every kernel call through
  `NEO::SysCalls`, and a macOS `sys_calls_macos.cpp` turns the render node into xemac (9 patches on compute-runtime
  `248eb54f8441`, 1 on gmmlib 22.10.2, 1 on the Level Zero loader 1.35.0; `compute/build-neo.sh`, ~5 min).
  `libze_intel_gpu.1.dylib` (Level Zero 1.18, Xe2) and the OpenCL ICD `libigdrcl.dylib` build for x86_64. Under Rosetta
  against `XEMAC_FAKE=1` the driver loads, enumerates the B580, creates a context and makes device/host/shared USM
  allocations; a native kernel binary and the built-in kernels load. Not yet run on the B580
  (`user/tests/compute/ze_hello.c`). Kernel-side findings: NEO creates long-running VMs (`LR_MODE`), which the kext
  refused (patch 0006 falls back to dma-fence-mode VMs, so long kernels meet the job timeout, and 0009 turns NEO's
  ring-buffer direct submission off there), and NEO on xe refuses a small BAR (allowed on macOS, the kext moves BOs
  through the 1 GB window).
  **In this tree (branch `lrmode`, 12 Nov, untested on hardware; DESIGN-lr-mode.md):** the kext accepts `LR_MODE`
  (`FAULT_MODE` still refused): only user fences as out-syncs on such VMs (syncobj out-syncs EOPNOTSUPP, as xe), no job
  timeout for their queues (GuC and GT resets still ban), and their memory protected by preempting the VM's queues (a
  GuC schedule disable, then enable) for BO moves and before frees, as xe's preempt fences and rebind worker. NEO's
  run-time probe (patches 0006, 0009) then gives LR VMs and ULLS without a patch change. Host test
  `kernel/IntelXe/tests/lr_test.cpp`; GPU test `user/tests/kernel/lr.c` (`args`, `long`, `ds`); counters in the
  "long-running VMs" line of `debug.intelxe.stats`. No compiler on the Mac yet: kernels and NEO's built-ins are compiled by Intel's `ocloc` in a
  Linux container (`compute/ocloc-docker.sh`); IGC (LLVM 22, opencl-clang, the SPIR-V translator) for macOS, the SIP
  kernel (mid-thread preemption; thread-group until then) and SYCL's runtime are the next stages (estimates in the
  design document); OpenCL goes through the Khronos ICD loader, which builds unchanged (`cl_hello`).
- **Done on the build Mac (12 Nov, branch `igc`, `compute/build-igc.sh`): Intel's compiler (IGC) for macOS.** IGC
  `0f18b8dc` with LLVM 22.1.8 + Clang (IGC's 10 LLVM patches), opencl-clang, the SPIR-V translator, vc-intrinsics and
  SPIRV-Tools builds for x86_64 on the Apple-silicon build Mac (build tools under Rosetta): `libigc.2.dylib`,
  `libigdfcl.2.dylib`, `libopencl-clang.dylib` (5 patches on IGC: CMake for Mach-O/ld64, Linux headers, `size_t` vs
  `uint64_t`; 1 on opencl-clang; 3 more on compute-runtime: the compiler found next to NEO, ocloc on macOS, a libc++
  destruction-order abort in `clReleaseContext`). The macOS `ocloc` gives Battlemage code identical to Intel's Linux
  ocloc (byte-identical binary from the same SPIR-V); NEO builds kernels from SPIR-V and OpenCL C at run time, compiles
  and embeds its built-in kernels (buffer and image) at build time, and gets IGC's SIP kernel, so mid-thread preemption
  stays on (patch 0008's thread-group fallback only without IGC). Checked against xemac's canned device; the B580 run
  is in the night package (`DESIGN-level-zero.md` 9.1). Next: SYCL's runtime over this Level Zero.
- **Built on the build Mac (13 Nov, branch `sycl`, `compute/build-sycl.sh`, `docs/design/DESIGN-sycl.md`): SYCL through
  Intel's DPC++ over our Level Zero.** DPC++ was chosen over AdaptiveCpp (Intel's production Level Zero path, SPIR-V to
  our driver with no LLVM at run time, every dependency pinned; AdaptiveCpp's L0 backend is its less mature one and
  needs LLVM on the test machine). intel/llvm `v7.2.0` builds for macOS x86_64 in one LLVM tree (16 min cold, tools and
  compiler under Rosetta): clang `-fsycl` (SPIR-V, and Battlemage binaries ahead of time through the macOS ocloc),
  libdevice, libsycl, the Unified Runtime's Level Zero adapters v1 and v2, UMF. 5 patches on intel/llvm (no ELF section
  names in the Mach-O offload wrapper, libsycl linked by the Darwin driver, the SPIR-V builtins' 64-bit types =
  `int64_t`, which is `long long` on a Darwin host, two type fixes) and 2 on UMF (a one-NUMA-node hwloc stand-in, the
  loader's Mach-O name). Against xemac's canned B580: `sycl-ls` lists the B580 (`intel_gpu_bmg_g21`) through both
  adapters, `user/tests/sycl/sycl_hello build` builds its 10 kernels from the AOT image without IGC and from SPIR-V
  through IGC. First B580 run: `user/tests/sycl/night-sycl.sh` (vector add, reduction, USM shared/device, copies,
  fill/memset, through v2 and v1, AOT over `~/l0` and JIT over `~/l0igc`). Not built: sycl-jit, XPTI tracing, the
  OpenCL adapter, oneAPI libraries.
- **Built on the build Mac (13 Nov, branch `l0sysman`, `DESIGN-level-zero.md` section 10): Level Zero sysman.**
  `zesInit` failed on macOS (no procfs/sysfs); NEO patch 0013 answers the device's xe sysfs from the kext, so NEO's Linux
  sysman runs unchanged: GPU frequency (RPn-RP0, SLPC's range, request, actual, efficient), card and package power
  (energy counters, PL1/PL2/I1), GPU and VRAM temperature, memory, PCI address; the rest answers with no handles or
  `UNSUPPORTED_FEATURE` (no live frequency/limit control, no engine activity without a PMU). The kext's new
  `debug.intelxe.sensors_raw` gives `debug.intelxe.sensors`' sample as a struct (`uapi/intelxe_sensors.h`, host test
  `sensors_test`); `user/tests/compute/zes_info.c` checks it against xemac's canned device. The core clock and SYCL's
  `max_clock_frequency` are now RP0 instead of 0, and SYCL lists `ext_intel_power_limits`. B580 run:
  `user/tests/compute/night-sysman.sh`. `user/tests/sycl/sycl_hello.cpp`, missing from the SYCL merge, was written
  again.

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
- **Implemented (13 Nov, branch `mtl4`), untested on hardware: pipeline reflection and `MTL4Compiler`**
  (`docs/design/DESIGN-mtl4-compiler.md`). Reflection now lists a kernel's `[[threadgroup(N)]]` arguments
  (`MTLBindingTypeThreadgroupMemory`, size and alignment from the AIR), reports access as declared (`device T *`
  read-write, `access::write` textures write-only) and buffer alignment from the AIR; before, a threadgroup argument took
  the name of `[[buffer(N)]]` with the same index (Apple's answers checked off-hardware by `user/tests/plugin/refl_host`:
  main 12/19, now 20/20). `MTL4Compiler` pipelines carry `-reflection` when `MTL4PipelineOptions.shaderReflection` asks
  (nil otherwise and for Metal 3 pipelines, as on Apple GPUs); the compiler takes `requiredThreadsPerThreadgroup` and
  static linking descriptors (compute, vertex, fragment); a pipeline data set serializer records the pipelines a
  compiler builds and `newArchiveWithURL:` reads them back as an `MTL4Archive` whose lookups build only what it holds.
  Tests `mtl_reflection` (42/42 on an M1 Pro, the reference) and `mtl4_compiler` (14/14 on the M1). Binary functions
  and dynamic linking followed (`mtl4more`, next entry).
- **Implemented (13 Nov, branch `mtl4more`), untested on hardware: binary functions and dynamic linking; tile,
  machine-learning pipelines and pipelines scripts refused** (`DESIGN-mtl4-compiler.md` "Binary functions and dynamic
  linking", "What is refused, and why"). `MTLLinkedFunctions.binaryFunctions` were ignored, so a table of
  `CompileToBinary` functions (ray-tracing apps' intersection functions among them) linked nothing; they now link after
  the linked functions into the switch that emulates visible function tables, with handles, and
  `newCompute/RenderPipelineStateWithAdditionalBinaryFunctions:` build the pipeline again with the added functions last
  (earlier handles unchanged). Metal 4: `newBinaryFunctionWithDescriptor:`, dynamic linking descriptors (binary
  functions, preloaded libraries; before, logged and ignored), `functionHandleWithBinaryFunction:` / `WithName:`,
  `newCompute/RenderPipelineStateWithBinaryFunctions:`, archives keeping binary functions. Refused with
  `MTLDeviceErrorNotSupported` and the feature named: tile pipelines (no tile memory; emulation by splitting render
  passes and a per-pixel imageblock buffer not attempted; `supportsFamily:` Apple* stays NO), machine-learning
  pipelines, pipelines scripts, pipeline-independent binary functions, linking into object/mesh/tile functions (mesh
  pipelines' object/mesh static linking was dropped silently before). Tests `mtl4_linking` (18/18 on the M1, arm64 and
  x86_64) and `link_host` (no GPU, 17/17 against the build); night phase Rb in `night-mtl4.sh`.
- **Implemented (13 Nov, branch `mtl4`), untested on hardware: placement sparse buffers** (`MTLBufferSparseTier1`,
  `DESIGN-sparse-textures.md` "Sparse buffers") behind the same `NVMTL_SPARSE_TEXTURES=1` (with `NVMTL_METAL4=1` for the
  queue that maps them; off, every answer is as before): `newBufferWithLength:options:placementSparsePageSize:` makes a
  Vulkan sparse residency buffer, `MTL4CommandQueue updateBufferMappings` / `copyBufferMappingsFromBuffer` bind tiles of
  placement heaps through the texture mappings' batches and ordering. Test `mtl_sparse_buffer` (27/27 on the M1).
  The three tests, `refl_host` and the knob-off checks run on the B580 from `user/tests/metal/night-mtl4.sh`.
- **Implemented (13 Nov, branch `sparse2`), untested on hardware: sparse textures tier 2** (`MTLTextureSparseTier2`,
  `DESIGN-sparse-textures.md` "Tier 2") behind `NVMTL_SPARSE_TIER2=1` with `NVMTL_SPARSE_TEXTURES=1` (default off; off,
  translations are byte-identical and every answer is as before). Tier 2 adds only per-tile access counters, read with
  the blit encoder's `getTextureAccessCounters:` / `resetTextureAccessCounters:` (optional, deprecated in macOS 26.4,
  still answered by an M1). Vulkan and Xe2 have none: the translator makes every texel read of a bindless-heap texture
  add to its tile's counter (one helper per module; a directory per heap slot says whether the texture counts; lanes of a
  subgroup on one tile add once), the plugin keeps a 16 MiB counter buffer and the directory in a reserved heap slot and
  copies or clears counters on the GPU in queue order. Test `mtl_sparse_tier2` (21/21 on an M1 Pro); night phase S2
  (`user/tests/metal/night-sparse2.sh`) runs it with tier 2 on and off and `mtl_sparse` / `mtl_sparse_buffer` with both.
- **Implemented (13 Nov, branch `mtl4icb`), untested on hardware: `MTL4BinaryFunctionOptionPipelineIndependent`**
  (`DESIGN-mtl4-compiler.md` "Pipeline-independent binary functions"). Such a function gets a process-wide handle value
  (from 0x40000000; places in a pipeline's list stay 1..N) that is its switch case in every pipeline linking it: the
  translator takes per-candidate case values (`nvmtl_link_runtime_ids`, `specialize_runtime_table_module_with_ids`, also
  for intersection-function tables); without such a function every link is the call and the module text it was. Handle
  lookups match the function object before its name. Tests: translator `runtime_table_ids` (4), `link_host` 18/18 (the
  value in two pipelines at different places, the switch cases), `mtl4_linking` case 13 both directions (M1 Pro 18/18);
  night phase Q (`user/tests/metal/night-mtl4icb.sh`).
- Open: tile pipelines, if an app needs them (render-pass splitting, see the design note).

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
  into slots the CPU never filled: refused without `NVMTL_ICB_DGC=1`, drawn with it (below, 13 Nov).
- **Behind `NVMTL_ICB_DGC=1` (12 Nov, branch `icbdgc`; default off, off = unchanged; UNTESTED on the B580;
  `docs/design/DESIGN-icb-dgc.md`):** 1024-byte records (the 64-byte record plus DGC token data) and an execute through
  `VK_EXT_device_generated_commands` (one `vkCmdExecuteGeneratedCommandsEXT` per run of same-state slots):
  - index buffer by address (index buffer token; the "same index buffer as the CPU encoded" condition goes), plain draws
    through an identity index buffer so every slot of a run has one token layout;
  - `set_vertex_buffer` / `set_fragment_buffer` from shaders in ICBs that do not inherit buffers: vertex buffer tokens for
    fetched bindings, push-constant tokens for addresses a stage reads from its pushed table (table-pc); indices a stage
    reads from the descriptor table are refused;
  - `set_render_pipeline_state` / `set_compute_pipeline_state`: accepted only for the pipeline the execute binds (ANV has
    no graphics indirect execution sets; compute ones do not fit the plugin's per-dispatch pipeline variants);
  - compute ICBs: `concurrent_dispatch_threadgroups`, `concurrent_dispatch_threads` (exact grids), `set_kernel_buffer`,
    reset, barriers; each slot dispatched from its record behind a full barrier (DGC when the kernel takes a GPU-set
    buffer, else `vkCmdDispatchIndirect`); a threadgroup size the CPU does not know is learned from the first, refused
    execute;
  - render `set_barrier` from a shader refuses its slot (no barrier token, and the CPU cannot see the flag); `clear_barrier`
    is accepted.
  Test: `user/tests/metal/mtl_gpu_icb2.m` (12 cases; 12/12 on the Apple M1 Pro with `--any`, whose compiler refuses
  render `set_barrier`); `mtl_gpu_icb` must stay 6/6 with the knob.
- **Implemented (13 Nov, branch `mtl4icb`), untested on hardware: WebKit's `vsICBIndirect*` on the DGC path**
  (`DESIGN-icb-dgc.md` section 9). Their index pointer (`indexBuffer + slotData[2] + args.indexStart`) is two GEPs on an
  entry parameter: both lowerings now follow a GEP chain to its root (the first link's base alone gave address `0 + 4k`,
  which the knob's index buffer token would have bound), and in DGC mode an address whose root the emitter cannot address
  is 0 (refused). Slots without a CPU draw may now draw the strip of their pipeline's class: record word 234 offers it,
  words 235..239 hold its counts, and the execute learns the topology after completion (drawn from the second execute,
  as WebKit executes a bundle every frame; `NVMTL_ICB_DGC_STRIPS=1` draws both topologies from the first). Without the
  knob the indexed ones stay refused (an index buffer by address needs DGC). Test `mtl_gpu_icb3` (M1 Pro 16/16); night
  phase Q. C0..C2 (compute ICB read zeros): analysed in section 9, not fixed (the after-execute dispatch log is the next
  data; the suspect is the compute sequences' push-constant tokens).
- **Next:**
  - Run `mtl_gpu_icb3`, `mtl_gpu_icb2`, `mtl_gpu_icb` and `mtl_icb_cost` on the B580 with `NVMTL_ICB_DGC=1` (night phase Q),
    then a WebKit main render bundle with `drawIndexedIndirect`; dump the compute_command AIR
    (`NVMTL_XLATE_DUMP`) to confirm the assumed signatures.
  - After the hardware run: make the knob the default if the cost is right; topology from the GPU for slots with a CPU
    draw too (slots without one now learn the class's strip, above); render pipeline switches (would need
    ANV graphics indirect execution sets or shader objects); a per-slot barrier for render ICBs if an app needs it.
  - Run `mtl_gpu_icb` and Safari's WebGPU render-bundle samples on the B580.

### Descriptor model
- Stage 1 (front end without per-draw atomics or retain/release, address table, bind cache): done. Stage 2, the descriptor-buffer model, is in the plugin behind `NVMTL_DESC_MODEL=buffer` and off by default until it has been soaked (`docs/design/DESCRIPTOR-MODEL.md`).
- **In this tree (12 Nov, untested on hardware):** the buffer model made ready for the default (`DESCRIPTOR-MODEL.md`
  section 13). The heap has bindings 2-4 (writable textures and texture buffers in argument buffers were refused in this
  model). table-pc, cs-table-pc and GL-UBO now work in both models; they were classic-only, so switching would have lost
  them. GPU-encoded ICBs (DGC) now work in it too. A slot change that touches nothing a stage's region holds keeps the
  region (`NVMTL_DB_KEEP=0`: off). Classic-set calls are counted and logged as `LEGACY`. `NVMTL_DESC_STATS=1` gives the
  same counters in both models (`DESCSTAT`: draws, host ns per bind, sets/regions, tables, pushes). Tests:
  `dbuf_model` (host), `mtl_desc_paths` (26 binding paths; Apple M1 Pro reference 26/26), `mtl_desc_cost` (CPU per
  draw), night phase `user/tests/plugin/night_desc.sh`. Next: one night run in both models; the default switch criteria
  are in section 13.
- **13 Nov (B580, plugin `138a99111e3c`):** Geekbench 6 Metal classic 92,971, buffer 87,479; but `mtl_desc_cost geek`
  still wrote a region per dispatch in every buffer variant. Cause: the translator keeps a kernel's plainly indexed
  buffers as storage-buffer descriptors, and the plugin wrote them at the bound offset, so every setBuffer offset changed
  the region. **In this tree (branch `dbufgeek`, untested on hardware):** a compute kernel's storage buffers take their
  offsets from push constants and their descriptors hold the buffer from the offset's 256-byte window
  (`nvmtl_bufoff_pc.h`, `NVMTL_DB_BUFOFF=0`: off); 407 of 585 Geekbench kernels converted off the GPU, 0 failures.
  D5's 5/7 was table-pc off, not the table set. `DESCSTAT` counts sets moved. Staged: `~/hackintosh/work/dbufgeek`
  (`DESCRIPTOR-MODEL.md` section 15). Next: graphics stages (vertex/fragment buffer offsets), the night run.
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
  **In this tree (rb8d, untested on hardware):** a job that never started resets the GT at Linux's 5 s TDR (was 10 s),
  and within 30 s of a GuC context reset a job of the same reset domain (render + compute) not started 2 s after the
  reset resets it then (`debug.intelxe.stats` "recovery"); still open: why the engine keeps the context.
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
  ~55 -> 128 M it/s; Geekbench GPU scores about a third higher). It leaves Core Ultra (but Arrow Lake desktop, below),
  CPUs without E-cores and other CPUs alone. Optional; `efi.sh`, the app's EFI page and the Windows EFI tool (`IntelXe-EFI.ps1`) add it below Lilu at
  stage 4 when the CPU needs it (Lilu is required and not shipped). The U and P laptop models (e.g. i7-1255U, i7-1360P)
  are recognised too.
- **Done (12 Nov):** the Windows EFI tool's HybridPM step ran on Windows 11 in the shared EFI vectors (the Windows
  app's xUnit tests run `IntelXe-EFI.ps1`'s engine). **Next:** on an EFI a hybrid-CPU PC then boots.
- **Done (13 Nov, a contributor's fix, issue #3):** Arrow Lake desktop (Core Ultra 200S, model 0xC6) with per-core-type
  HWP scales measured from Linux CPPC on a Core Ultra 7 265KF (not on the test machine). **Open:** the EFI tools go by
  `efi-rules.json`'s `hybridpm` name rules, which exclude every Core Ultra, so on Arrow Lake HybridPM is added by hand
  (the install guide says so); whether to include Core Ultra 2xxK/KF/F there is a decision for the 0.1.5 release.

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
- Boot guard: checked over power cuts and failing boots on 5–6 November 2026 (0.1.2: safe settings after two failures, held after four, the clear written by a LaunchDaemon). A held boot on macOS 26 stayed at the Apple logo (WindowServer draws on IntelXeFB only through the accelerator's display pipe); the way out was OpenCore's Reset NVRAM.
  **In this tree (13 Nov, branch `bootguard`, untested on hardware):** a held boot (and `-intelxe_noaccel`, safe mode) gets an unaccelerated desktop on the firmware framebuffer: IntelXe.kext undoes SSDT-ARCOFF's identity on the GPU for that boot so Apple's IONDRVFramebuffer (what the macOS installer runs on with this card) takes the GOP's surface before IntelXeFB and NMXAccel are offered the GPU. The state is in `sysctl debug.intelxe.bootguard` and on the Manager's Overview page (*Boot guard*, with *Clear the boot guard*); `intelxe-held-display=fb` keeps 0.1.4's behaviour. Rules shared by both kexts in `uapi/intelxe_bootguard.h`, host test `tests/bootguard_test.cpp`. Design and the hardware test: [`design/DESIGN-boot-guard-held-display.md`](design/DESIGN-boot-guard-held-display.md). Known limit: sleep in a held boot (IONDRVFramebuffer's controller kept sleep waiting 90 s, SSDT-ARCOFF's note).
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
- Second display pipe (pipe B, DBUF/MBUS split, watermarks). **Implemented behind `-intelxefb_pipeb` (NVRAM
  `intelxefb-pipeb=1`), default off, untested: needs a second monitor (12 Nov).** Design and Linux sources:
  [`design/DESIGN-second-pipe.md`](design/DESIGN-second-pipe.md). With the knob on, every port with a live hotplug bit is
  probed over its AUX channel, a DP sink lights pipe B at its preferred EDID mode (8b/10b up to HBR3, 24 bpp; PMDEMAND,
  PW_B, DBUF slices, MBUS unjoin, the C20 PLL, link training, watermarks computed as skl_watermark.c does) and a second
  framebuffer (head 1) is connected; unplug, sleep and stop take the pipe down. Head 1 itself is published at IntelXeFB's
  start, disconnected (Apple's model, 12 Nov): a monitor plugged in or out is a connection change (the connect
  interrupt), the head stays published. Pipe A's DDB is not reallocated (the plan
  refuses when it does not fit). Out of scope: native HDMI, MST, DSC, UHBR, 10 bpc, VRR/HiDPI/hardware cursor on pipe B.
  A dry run that needs no second monitor: `sudo sysctl -n debug.intelxefb_pipeb_plan` compares pipe A recomputed from its
  own timing with its live registers and prints what pipe B would get (`debug.intelxefb_pipeb_mode` picks the mode).
  One-monitor check with the knob: head 1 visible as disconnected in ioreg (`IntelXeFBConnected` No) and in the head
  diagnostics, `system_profiler SPDisplaysDataType` still one display (commands in DESIGN-second-pipe.md §7).
  Open: the first run with two monitors.
- Accelerating the second head (NMXAccel, NMXAGDC). **Implemented behind NMXAccel's multi-head knob (`nmxheads=1` /
  NVRAM `nmx-heads=1` / `debug.nmxaccel_heads_auto`), default off, untested with two heads (12 Nov).** Design:
  [`design/DESIGN-multihead-accel.md`](design/DESIGN-multihead-accel.md). One accelerator, several displays (Apple's
  model): a head published after the arm is registered on its own (thread call, inside IOGraphics' gates), a terminated
  head frees its slot, and a head without flips, pipe gamma or a cursor plane (IntelXeFB's second framebuffer) is only
  copied to. NMXAGDC lists every head at each 0x980 (as before) and answers its link/plane-scaler queries from the
  second framebuffer's new `NVRMTiming`. One-monitor diagnostics: `sudo sysctl -n debug.nmxaccel_heads` (heads seen,
  armed, what would be armed) and `sudo sysctl -n debug.nmxagdc_heads` (what the next 0x980 would list).
  WindowServer reads the head list when it starts; IntelXeFB now publishes the second framebuffer at its start,
  disconnected (12 Nov), so it is in WindowServer's first list and, with the knob, has its display pipe from the boot
  arm: a monitor plugged in later is a connection change, no WindowServer restart (untested with two monitors). Knob
  off, the boot arm leaves head 1 for `debug.nvaccelfb=3`, as before.

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
  in the EFI OpenCore booted from or a folder the user picks. The Windows counterparts for the EFI are IntelXe Manager
  for Windows and `windows/IntelXe-EFI.ps1` (below).
- **Done (0.1.4):** a redesign (Overview with a status header, charts on the Monitor page, a menu bar item, an About
  window); a setup assistant from a fresh macOS install to a verified driver (Safari's setting with stage 2, the
  extras on its last page, an EFI chooser when OpenCore recorded no boot path); the system-volume copy for Safari (and
  OpenCore's `SecureBootModel`); SSDT-ARCOFF adapted to the card's ACPI path and HybridPM at stage 4; diagnostics for a
  bug report (Report page); GPU tests; a check at login.
- The EFI page reads the EFI again when its files change outside the app (a fingerprint of config.plist, ACPI, Kexts and
  Drivers: when the page shows, when the app comes to the front, every 5 s while it is open).
- Release signatures independent of GitHub (11 Nov): `release/signing.swift` (Ed25519, CryptoKit) signs the `.sha256`
  list when `package.sh` runs with `SIGN_KEY`; `release.sh` refuses a release without a signature its app accepts; the
  app (`Signing.swift`) refuses a download whose list is not signed.
- Key change (13 Nov, `docs/RELEASE-KEYS.md`): a successor key is added to both apps in a release signed with the old
  one (`.sha256.sig` by the old key for older apps, `.sha256.sigs` with a line per key), signs alone from the next
  release, and is retired later. `release/release-keys.sh` adds and retires keys in the macOS and Windows lists
  together; `package.sh` refuses signing keys the apps do not list and an app that does not carry them; `release.sh`
  checks `.sig` against the previous release's app and that a new key has its `.sigs` line (`KEY_RESET=1` for a lost or
  exposed key); `release/test-signing.sh` runs the whole change with throwaway keys. No successor key exists yet: making
  one and shipping it in an app is the maintainer's step 1-2.

### IntelXe Manager for Windows and the EFI rules
- **Done (12 Nov, for 0.1.5):** one set of EFI rules, `release/templates/efi-rules.json`, read by `efi.sh` (and the
  macOS app through it), `IntelXe-EFI.ps1` and the Windows app, with 32 shared test vectors run against all three
  (`docs/design/DESIGN-efi-rules.md`); comparing the two implementations found four fixes (efi.sh: `NVRAM > Delete`
  added when missing, the backup's list written before each step, every kext file checked against `SHA256SUMS`; the
  ps1: each `config.plist` value checked as it is set).
- **Done (12 Nov, for 0.1.5):** IntelXe Manager for Windows (`windows/app/`, WinUI 3, unpackaged and self-contained):
  Overview (read-only checks of the PC), EFI (stage 2, the Safari setting, stage 4, undo; IntelXe-EFI.ps1's engine
  compiled in; an elevated helper of its own for EFI partitions, none for EFI folders), Report, About; English and
  Turkish, the engine's plan lines included. Released as `IntelXeManager-VERSION-win-x64.zip` (about 66 MB; about 83 MB with the EFI builder of part 4), built
  on Windows or in CI and checked by `release/package.sh`.
- **Next:** change a real EFI partition with it (only the helper's logic is tested, with the mounting faked) and boot
  from it; a code-signing certificate (SmartScreen and UAC show an unknown publisher until then).

## Other Xe2 (Battlemage) cards

The kext is organized around the graphics IP version (Xe2 / graphics 20.01, media 13.01, display 14.01), not the board.

| Card | Device | What is expected to differ |
|---|---|---|
| Arc B570 | BMG-G21, fewer Xe-cores | **Done (0.1.4):** device ID 8086:E20C in the kexts, `install.sh`, the app, `efi.sh` and the Windows EFI tool (PR #2 by `@1964356263`); topology read from fuses. Not run on hardware yet: needs a tester |
| Arc Pro B50 | BMG-G21 workstation, 16 GB, 70 W | **Done (2026-11-13, branch `bmgpro`):** 8086:E212 in the supported list (kexts, NMXAccel, `install.sh`, `efi-rules.json`, both apps, clbench) with `model` "Intel Arc Pro B50 Graphics" and the GPU test's figures from Intel's specification; VRAM and BAR read from the card. Not run on hardware: needs a tester |
| Arc Pro B60 | BMG-G21 workstation, 24 GB | **Done (2026-11-13):** 8086:E211, as the B50; allocator and BAR window sized from the hardware (checked: no 12 GB / 16 GB constant left; the 11 GB fallback is capped by the tile). Needs a tester |
| Arc Pro B70 | BMG-G31, 32 Xe-cores, 32 GB | **Prepared (2026-11-13):** 8086:E223 (Intel's product page); Wa_14022085890 conditional as in Linux; same GuC/HuC/DMC files; a tester starts it with `-intelxe_allow_g31` (kernel only, no Metal). Its GMD_ID may be 20.02, which drops eight G21 workaround items ([DESIGN-platforms.md](design/DESIGN-platforms.md#pro-b50b60-enabled-pro-b70-g31-prepared)); possibly display/PHY tables |

Since the platform descriptors (`kernel/IntelXe/xe_platform.cpp`, 10 Nov), every BMG-G21 card shares the supported
entry: the B50 and B60 needed only their entry in `data/platforms.json` `"supported"` (`tools/parity/devices.py` then
names every copy of the list to change: the `IOPCIMatch` IDs, `install.sh`, `efi.sh`, the app, the Windows EFI tool)
and a `model` name; both are in since 13 Nov and wait for a test. BMG-G31 (e220-e223) has its own
entry, refused unless the tester boot-arg `-intelxe_allow_g31` is set; xe's G21-only SLPC minimum (Wa_14022085890) is
conditional now ([DESIGN-platforms.md](design/DESIGN-platforms.md)).

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
| Lunar Lake (Xe2-LPG, integrated) | Core Ultra 200V | No VRAM: an integrated memory model with system memory and the stolen region (behind the LMEMBAR), flat CCS in system memory. Different display (Xe2_LPD). GSC firmware (HuC authentication, media C6). No shared LLC (Linux `lnl_desc` has no `has_llc`): coherency through the PAT. Plan and first steps (13 Nov): [DESIGN-lunar-lake.md](design/DESIGN-lunar-lake.md). |
| Alchemist (Xe-HPG, DG2) | Arc A380/A750/A770 | Graphics 12.55: different LRC layouts, workarounds, MOCS/PAT, GuC ABI subset. Display 13. GSC for HuC and firmware loading. |
| Panther Lake (Xe3) | Core Ultra 300 | Graphics 30.x: new LRC/workaround tables, new display IP. Integrated memory model as for Lunar Lake. |
