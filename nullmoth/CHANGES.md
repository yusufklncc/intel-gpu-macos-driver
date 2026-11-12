# Changes to nullmoth for the Arc B580 (and B570)

This directory is a copy of the parts of [nullmoth/nvidia-macos-driver](https://github.com/nullmoth/nvidia-macos-driver)
that the IntelXe stack uses, with our modifications applied.

- **Upstream base:** `6e375469bd02f5421a2a3d7c408507d5099e2c38` ("Changelog 2.6.2", 2026-10-26), branch `main`.
- **Merged from upstream 2.6.6 (2026-11-03, in the released plugin):** of the 91 upstream commits between 2.6.2
  (`29432bb` on today's `main`; upstream rewrote its history, so `6e37546` is not an ancestor of `main`) and 2.6.6 that
  touch the plugin and the translator, the 41 general fixes and 4 that can be switched off were taken as 45 patches;
  NVIDIA-specific changes were left out. The released plugin is that merge plus our ray-tracing and uniform-buffer work.
- **Merged from upstream 2.7.1 (2026-11-05):** of the 107 upstream commits between `v2.6.6` and `v2.7.1` that touch the
  plugin and the translator, 51 were taken (38 whole, 13 in part) as 30 patches (8 translator, 22 plugin), 4 were
  already in this copy in another form, and 52 were left out: nvconv/SASS, changes specific to NVIDIA, NVK or the RM
  kernel module, application profiles, and the device-loss diagnostics chain. The plugin and translator sections below
  list what the merge changed. 2.7.2 was merged into the plugin on 2026-11-05 (`57b8599`; its translator had no changes).
- **Merged from upstream 2.8, 2.8.1 and 2.9 (2026-11-10, branch `nm29`):** of the 153 non-merge upstream commits between
  `v2.7.2` and `v2.9` that touch the plugin and the translator, 42 were taken (28 whole, 14 in part) as translator, plugin
  and documentation commits; 17 were already in this copy in another form or touch code this copy does not have; 94 were
  left out: DLSS and MetalFX-over-DLSS, application profiles and working-set/OS-version answers, Game Mode, hybrid laptops,
  multi-GPU card binding, NVENC and upstream's video registration, and the features upstream ships experimental or opt-in
  (its GPU-written indirect command buffer encoding, tile shading, vertex amplification, sparse textures, 4:2:2 textures,
  framebuffer fetch by copies, shared/owned VRAM working copies, IOSurface lease/seed/import-cache options, the frame-rate
  limiter). 2.8.1 changed nothing in the plugin or the translator. The fork github.com/HighDelay/nvidia-macos-driver-nvk
  (same licences) audited upstream 2.9 before integrating it; its findings were checked against upstream's code for every
  part taken here (no code of that fork is used). The sections below list what this merge changed.
- **Our work:** one commit on a local branch `b580` (`428ca50`, "NMXAccel for the Arc B580: class-name header, no NVIDIA headers, B580-only personality"), plus working-tree changes made 2026-10-28/30. Both are included here; the repository history records each installed state.
- **Recorded diff:** `b580-changes-vs-6e37546.diff` is the diff of these directories against the base commit as it
  stood on 2026-10-30, with the names of that time (the stack was renamed from B580 to IntelXe on 2026-11-05). It is a
  record and is not updated: later changes, the rename included, are in the files here and in the repository history.

## Why a copy and not a subtree

The upstream repository is the whole NVIDIA product: the NVRM kernel module built against NVIDIA's open GPU kernel modules, NVRMFB, NMDRM, NMHDA, the NVK patches, GSP firmware handling, the installer and tools. Only the Metal plugin, the shader translator and two kexts are used for the B580.

A `git subtree` would bring the full upstream history and all of those components into this repository. Our own history on top of it would still be short: most of our changes were made in a working tree, not as upstream-style commits.

A copy of the four components gives us:
- a self-contained build;
- the pinned base commit;
- this file and the diff, which record our changes exactly.

## What is included

| Path | Upstream path | Licence |
|---|---|---|
| `plugin/` | `plugin/` (NVMTLDriver.bundle, the Metal driver plugin) | PolyForm Noncommercial 1.0.0 |
| `translator/` | `translator/` (AIR → SPIR-V, based on metal2vulkan) | LGPL-3.0-or-later (`translator/LICENSE`, `translator/COPYING`) |
| `kexts/NVRM/accel/` | same (built with `-DNMX_ACCEL` as NMXAccel.kext) | PolyForm Noncommercial 1.0.0 |
| `kexts/NVRM/nvrm_off.h`, `nvrm_gpuva_abi.h`, `nvrm_vram_abi.h`, `nvrm_surfshare_abi.h` | same (headers NMXAccel includes, unmodified) | PolyForm Noncommercial 1.0.0 |
| `kexts/NVRMAGDC/` | same (built with `-DNMX_AGDC` as NMXAGDC.kext) | PolyForm Noncommercial 1.0.0 |
| `LICENSE`, `NOTICE` | upstream root, unmodified | — |
| `build/build-intelxe.sh` | ours (adapted from upstream `build/build_plugin_release.sh` / `build_xlate_release.sh` / `accel_build.sh`) | MIT |

Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)

Note: `kexts/NVRM/accel/iofam/`, `kexts/NVRM/accel/re/` and `kexts/NVRMAGDC/AppleGraphicsDeviceControl_re.h` are upstream's own interface declarations for Apple's IOAcceleratorFamily2 and AppleGraphicsDeviceControl classes. They are included unmodified, except for two one-line changes listed below, because the kexts do not build without them.

## Modifications by component

### NMXAccel (`kexts/NVRM/accel/nvrm-accel.cpp`, `nmx_accel_names.h`, `nvaccel_stubs.h`, `Info-NMX-IntelXe.plist`, `iofam/IOAccelSurface.h`, `iofam/IOAccelLegacySurface.h`)

- **`nmx_accel_names.h` (new):** the OSMetaClass names of the released NMXAccel.kext 2.1.0. Upstream's `-DNMX_ACCEL` build includes this header but does not ship it.
  - The NMX build no longer needs NVIDIA's `nvkms-kapi.h`.
  - It does not read NVIDIA's temperature register from another vendor's BAR0.
- **`Info-NMX-IntelXe.plist` (new):** matches `8086:e20b` (B580) and, since 0.1.4, `8086:e20c` (B570), in the `IOAccelerator` match category.
- **Display head:** the framebuffer of IntelXeFB (`IntelXeFramebuffer`) is watched as head 0.
  - `nvScanoutInfo` and VRAM allocation calls are answered through IntelXeFB.
  - Placeholder GPU VAs; commit/release are no-ops for NMX.
- **Display pipe:** a full `IOAccelDisplayPipeCapabilities` is published, so IOPresentment builds a display pipe. Multi-client VBL.
- **`NVSurface::submitCopyForward`:** implemented with CPU rect copies. The family default returned Unsupported, and WindowServer never swapped.
- **Present path:**
  - **Async swap:** user client `IOAccelNMXPresent` ('NMXP') carries the plugin's surface-ready marks. `performTransaction` runs on a thread call: wait for the mark, then copy or flip, then signal.
  - **Real flips:** the plane scans the composite's VRAM buffer, pinned through IntelXe.kext and mapped into GGTT slots by IntelXeFB. Completion is detected when `PLANE_SURFLIVE` matches.
  - **Hardware gamma:** the transaction's post-gamma table goes to the pipe's LUT through IntelXeFB `nmxPipeGamma`.
- **xe user client through NMXAccel:** `newUserClient` type 'B580' hands out IntelXe.kext's `IOAccelIntelXeClient`. Sandboxed system-graphics processes may open only IOAccelerator clients, so this gives them Metal.
- **Adaptive-Sync:** the frame-length range of an IOPresentment transaction (a "Variable" refresh mode published by IntelXeFB) is passed on to IntelXeFB.
- **Boot-time arming:** `boot1`. The display pipe and transactions are armed in the kernel when IntelXeFB matches at boot, with an NVRAM guard.
- **Sysctls:**
  - `debug.nmxaccelfb`, `debug.nmxaccel_iop*`, `debug.nmxaccel_async`, `debug.nmxaccel_flip`;
  - counters `debug.nmxaccel_scanout_copies`, `debug.nmxaccel_copy_forwards`, `debug.nmxaccel_flips`, `debug.nmxaccel_flip_late`, `debug.nmxaccel_flip_refused`.
- **Boot ordering (boot1d):** registered at start again, `debug.nmxaccelfb` reads 1 while the boot-time arm is pending; the arm waits for IntelXeFB and the accelerator to go quiet; registration and mode-change delivery run inside IOGraphics' system and controller gates through IntelXeFB's `intelxefbRunGated` (run bare they deadlocked against AppleGPUWrangler at boot).
- **VRAM-shared IOSurfaces (sv7):** the registry of IOSurfaces adopted into B580 VRAM (kernel ABI `kernel/IntelXe/uapi/intelxe_surfvram_kabi.h`, shared with IntelXe.kext; the build adds that directory to the include path), page-off/page-in through B580's copy engine, phase 4 (WindowServer composites whose readback the plugin skipped). Off until `debug.intelxe.surfvram` is set.
- **Metal plugin location (nmx-ssv2):** `MetalPluginName` names `/System/Library/Extensions/NVMTLDriver.bundle` when the optional system-volume copy is installed (WebKit's GPU process may load plugins only from there), otherwise the `/Library` copy, installed as `NVMTLDriverLib.bundle` so that it cannot shadow the system one. Boot-arg `nmxplugin=lib` or `/Library/GPUBundles/NVMTLDriver.use-library` force the `/Library` copy. Published as `NMXMetalPluginFrom`.
- **More than one IntelXeFB head (12 Nov, `docs/design/DESIGN-multihead-accel.md`), behind boot-arg `nmxheads=1` / NVRAM
  `nmx-heads=1` / `debug.nmxaccel_heads_auto` (default off):** a head published after the arm is registered by a thread
  call inside IOGraphics' gates (mode 1 with the mode change, 2 without), a terminated head frees its slot, and a head
  without flips or pipe gamma is never asked for them. The rule is `nmx_heads.h` (new; host test
  `tests/nmx_heads_test.cpp`, run by `build-intelxe.sh tests`). `debug.nmxaccel_heads` (root, read only) lists every head,
  its pipe, scan-out, capabilities, frames and what would be armed.
- **`iofam/IOAccelSurface.h`, `iofam/IOAccelLegacySurface.h`:** one line each. `submitCopyForward` and `set_shape_backing_length_ext` return `IOReturn` (not `void`), which matches the family and lets the override report a result.

### NMXAGDC (`kexts/NVRMAGDC/nvrm-agdc.cpp`, `nvrm-agdc-k5.h`, `Info-NMX-IntelXe.plist`)

- **Boot ordering:** waits for IntelXeFB's registration to finish before listing it (boot1c hung arming while IntelXeFB was still being published).

- **`Info-NMX-IntelXe.plist` (new):** loads the AGDC service (on `IOResources`; it serves IntelXeFB's display, no device-ID match).
- **Command 0x980:** lists IntelXeFB, so WindowServer builds a display pipe for it.
- **Commands 0x921 (link config) and 0x711:** answered with nullmoth's `NVRMTiming` (K5) for IntelXeFB.
- **0x711 gamma capability:** a 3-channel, 10-bit, 1024-entry pipe gamma, so CoreDisplay skips its own per-frame gamma pass. Switch: `debug.nmxagdc_gamma`.
- **Second head (12 Nov):** a head other than 0 publishing brings nothing up (it is listed at the next 0x980); `debug.nmxagdc_heads` is a dry run of the next 0x980 reply and each head's K5 answers.

### Metal plugin (`plugin/`)

- **Vendor `0x8086` path:**
  - loads our ANV (`/Library/GPUBundles/nvmtl/`);
  - short device name: the PCI device's `model` as About This Mac shows it ("Intel Arc B580 Graphics", "Intel Arc B570 Graphics"); the VideoToolbox codecs carry the same name;
  - the allow-list gains `!Name` (only while the display pipe is armed) and `!*`.
- **WindowServer:**
  - framebuffer fetch on by default for non-NVIDIA devices (`NVMTL_FBFETCH_WS=0` disables);
  - background translation off in WindowServer.
- **Buffer-backed textures (`nvmtl_vk.c`):** `image_create_buffer_alias` now checks only the import's memory type (`nvk_buffer.mtype1`), not every type. With ANV on Xe2, every buffer texture had been refused and turned into a one-time copy, which left stale text fields.
- **Mipmaps:** `nvmtl_mip_autofill` generates mips on the GPU for Core Animation's level-0-only uploads.
- **Samplers:** a stale constexpr sampler was left in the app's slot 0 and caused Dock/text pixelation. The plugin now rebinds the app sampler.
- **Screen capture:** synchronized exactly through a page-offset wait. This replaced the "first 16 flushes" heuristic.
- **Implicit IOSurface synchronization:** uses IntelXe.kext's surface fences (`uapi/intelxe_drm.h`) instead of CPU waits.
- **Import slots:** WindowServer now reaps implicit-sync import slots. Before this, the slots filled up and it fell back to unsynchronized CPU copies (the Minecraft glitches).
- **Surface-out:** copies go straight into imported IOSurface pages, cached imports, and folded surface-out. This is the first part of the async present path.
- **Memory:**
  - every Managed no-copy buffer gets a VRAM mirror;
  - descriptor recycling;
  - big-buffer parking;
  - an autorelease leak fixed;
  - scan-out buffers use an uncompressed memory type. ANV picks compressed PAT entries for device-local memory, and the linear display plane cannot read CCS.
- **OpenGL (Apple's GL over Metal):** Intel GL profile provoking-vertex bit.
- **Diagnostics:** `NVMTL_HITCH_MS`, `NVMTL_KTIME`, `NVMTL_PSTAT`, `NVMTL_XLATE_FAIL_DUMP`.
- **VideoToolbox codecs (fix28):** registers the B580 AV1 decoder and HEVC/H.264 encoder plug-ins (`libintelxeav1dec.dylib`, `libintelxevenc.dylib` in `/Library/GPUBundles/nvmtl`, see `videotoolbox/`) in every process that loads the plugin; `NVMTL_NO_INTELXEDEC=1` / `NVMTL_NO_INTELXEVENC=1` keep a process out.
- **GPUTIME (fix29b):** per-pass GPU time profiler, on while `/tmp/nvmtl-gputime` exists.
- **Staged IOSurface host copies, SURFCENSUS (fix29c).**
- **Descriptor pools (fix30-32):** pools grow from 256 sets instead of starting at 4096 (77 MB of the CPU-visible BAR window per process); a set layout with one binding per binding number, so ANV's reading of the mutable-type lists by binding number (see `mesa/patches/0013`) and the spec's by index agree (a pool held a fifth of its sets before); when no set can be had even after waiting for in-flight work to return some, the command buffer ends with `MTLCommandBufferErrorOutOfMemory` instead of being committed without the draws (`NVMTL_SUBMIT_PARTIAL=1`: the old behaviour; `NVMTL_DESC_POOLS_MAX`: a lower ceiling for tests).
- **Descriptor model (stages 1b-1d, 2):** no per-draw retain/release and no atomics in the draw path, an address-table shadow with trimming, a bind cache; the descriptor-buffer model (`nvmtl_dbuf.c`) behind `NVMTL_DESC_MODEL=buffer`, off by default. Design: `docs/design/DESCRIPTOR-MODEL.md`.
- **CAMetalLayer drawables (sv10)** count as writers of their IOSurface; **Tile4 IOSurface import** (size `pitch * align32(height)`).
- **VideoToolbox decoders (intelxedec v2):** H.264 and HEVC next to AV1, registered only when no other accelerator offers them; the AV1 encoder registration (sv9).
- **Sandboxed processes:** when `debug.nmxaccelfb` is sandbox-denied, the arm state comes from NMXAccel's `NMXAutoArm` property (Chrome's GPU process), and where WebKit hides that too, from the `IOAccelDisplayPipeCapabilities` (sc6arm). Where a sandbox refuses to open `/dev/null` (WebKit's GPU process), the descriptor xemac and the surface imports need is made by `dup` of a standard descriptor (sc6dn): Safari's WebGL runs on the B580.
- **Bundle companions (dec4):** Mesa and the translator are also found inside the plugin's own bundle (`Contents/Frameworks/nvmtl`), the layout of the system-volume copy.
- **GPU reset recovery (recover1):** a banned exec queue is replaced on the same VM through Mesa's `nvmtl_anv_device_recover_v1` (`mesa/patches/0050`); at most 3 per minute and 16 per process.
- **Function constants (speccst6):** variants are specializations of one translated module (`nvmtl_spec_fc.h`); only the constants that shape the module are baked in.
- **VRAM-shared IOSurfaces (sv2-sv8):** IOSurfaces in shared B580 VRAM through Mesa 0014 and NMXAccel's registry, with implicit sync of every access, instead of per-process VRAM copies synchronised with the system pages. Off until `debug.intelxe.surfvram` is 1 (apps) or 2-3 (also WindowServer).
- **Commit locking (lock V2, 0.1.4):** commit and the execution worker no longer share the command-buffer class lock
  (`nvmtl_plock.h`): 18.2 -> 11.1 us per command buffer back to back. On by default for applications; WindowServer and
  the system's interface keep the class lock; `NVMTL_LOCK_V2=0/1` decides for one process.
- **GuC low-latency hint (0.1.4):** off by default; `/Library/GPUBundles/NVMTLDriver.low-latency` turns it on for every
  process but WindowServer and the system's interface, `NVMTL_GUC_LOW_LATENCY=0/1` for one process (`nvmtl_vk.c`).
- **Working set on Intel (0.1.4):** `recommendedMaxWorkingSetSize` is ANV's heap budget less 1 GB for the desktop, at
  most the card less 2 GB (8.2 GB on the B580, was 5.1); `NVMTL_WS_INTEL=0` restores the formula below.
- **Device properties (0.1.4):** the 31 protocol properties the compiler found unimplemented are answered: 12 by
  Metal's `MTLIOAccelDevice`/`_MTLDevice` (now declared), the rest from real data: `location` from the PCIe topology,
  `maxTransferRate` from the root port's link, `supports32BitFloatFiltering`/`supports32BitMSAA` asked of ANV,
  `maxArgumentBufferSamplerCount` 2047 (the sampler heap), `supportsPlacementSparse` NO (asking it crashed an
  application with an unrecognized selector), `supportsQueryTextureLOD` and `supportsPullModelInterpolation` NO (both decided from the device since the nullmoth 2.9
  merge, below).
- **Synthesized read sampler (0.1.4):** bound like a constexpr sampler at its binding's offset into the sampler band;
  its slot used to read the dummy sampler (linear, maxLod 0.25), so cube texel reads at levels 1-2 read level 0.
- **`staticThreadgroupMemoryLength` (0.1.4):** reports the threadgroup memory the function declares; it answered 0,
  so apps and Apple's OpenCL showed "local 0 B".
- **Function-constant module key without a cache folder:** in a process with no shader cache folder (a sandbox such as
  WebKit's GPU process) the in-memory key of a specialized module printed the structural values' JSON with `%@` of an
  `NSData`, which shows only its length and a few bytes. Variants whose JSON had the same length shared one module, so
  rounded boxes and gradients in Safari drew through the module of a plain fill. The key now holds the JSON text.
- **nullmoth 2.7.1 merge, plugin part (22 patches):**
  - visible functions: a function that calls visible functions is deferred to pipeline link before it is translated
    (`NVMTL_EAGER_LINK_PROBE=1`: the old order); function tables are found from the intrinsics that read them and in
    linked modules, with `linkedFunctions` entries for their handles and handle 0 for the pipeline's own function;
  - bindless heaps: storage-image and texel-buffer bindings next to the sampled ones, prefilled with dummies, slots
    zeroed when released (`NVMTL_NO_HEAP_STORAGE=1`, `NVMTL_HEAP_PREFILL=0`); application heaps are zero-filled when
    created (`NVMTL_HEAP_ZERO=0`); a pipeline that indexes a binding the heap does not have is refused;
  - `useHeap:` members are pinned and residency sets kept alive; the identity-compared resource set is off
    (`NVMTL_RESSET_IDENTITY=1`; on upstream);
  - argument buffers owned by a Managed buffer are read from its CPU copy (`NVMTL_EMBEDDED_MANAGED=0`: the old reads);
  - a pipeline that does not fit the open render pass ends it and starts a compatible one (`NVMTL_NO_PASS_TRANSITION=1`);
  - freed buffers stay alive until the submissions that may read them complete, rewritten on this copy's submission
    tracking (`NVMTL_BUFFER_GRACE=0`; textures with `NVMTL_IMAGE_GRACE=1`); parked buffers are released after
    `NVMTL_BUFPARK_AGE_MS` (default 5000, 0 = never; a fixed 5 s upstream);
  - compute: binding tables in fixed arrays; indirect dispatch prepares the textures declared with
    `useResource:usage:`; a compute-to-compute barrier in concurrent encoders too (`NVMTL_CONC_NO_BARRIER=1`); the
    grid-only threadgroup reshape is taken but off (`NVMTL_TG_RESHAPE=1`; on upstream, tuned for NVIDIA SMs), and so is
    the ordering barrier after every dispatch and pass (`NVMTL_ORDER_BARRIER=1`; on upstream, for NVK);
  - `recommendedMaxWorkingSetSize` is capped at 71% of the card (`NVMTL_WS_RESERVE_MB`, 0 = no cap); setting the
    same pipeline again in an open pass is skipped (`NVMTL_PSO_REBIND=1`: rebind);
  - sRGB textures that ask for ShaderWrite keep their storage use through a UNORM view
    (`NVMTL_NO_SRGB_VIEW_STORAGE=1`); `newAccelerationStructureWithSize:` falls back to system memory when VRAM refuses;
  - the shader-cache tag names every translation mode and the stale sweep keeps other modes' entries
    (`nvmtl_spvcache_name.h`; the default tag is unchanged); `NVMTL_AIR_DUMP=<dir>`; `NVMTL_PROCESS_ENV=1` and the
    release-build environment filter (`nvmtl_env_knobs.h`, only in `RELEASE=1` builds).
  - Not taken: nvconv and the SASS path, the device-loss report/witness/frame-dump chain, `NVMTL_HAZARD`,
    `NVMTL_CANONICAL_POINTERS`, application profiles and `NVMTL_APPLE_FAMILY`, the RM-specific pool, sparse and
    zeroing changes, and the BGR10A2 storage remap (Xe writes B10G10R10A2 directly).

- **nullmoth 2.8/2.9 merge, plugin part:**
  - shader-cache entries prove they are whole (`nvmtl_cache_file.h`, 52a62bf): translated-shader, specialization,
    reflection, link, variant and sampler-variant entries are payload plus a footer (magic, length, 64-bit hash), read
    back only when the footer agrees; a torn, short or older-format file is removed and translated again; nothing is
    stored after the device is lost. Unlike upstream the temporary file is not flushed (no `fsync`/`F_FULLFSYNC`) before
    its rename: Safari's WebKit GPU-process sandbox refuses those calls, and the footer is what proves an entry whole.
    The AIR text cache, the library index and the library-key records keep `nvmtl_store_file` unframed;
  - the shared AIR cache folder is taken only after a real write (400111c's rule, inside this copy's chooser that
    re-chooses after a refused store): `access(W_OK)` says yes where WindowServer's sandbox then refuses the open;
  - after the GPU is lost for good (this copy's queue recovery exhausted, `g_state` -1), command buffers are made
    "dead": they record nothing and complete with MTLCommandBufferError 11 (device removed) instead of being nil, and
    the log names the cause (`nvmtl_loss.h`, f9fe87b). Upstream's `nvmtl_vk_lost(void)` is `nvmtl_vk_backend_lost` here
    (the name is taken by the recovery path); the recording stubs also cover the command entry points this copy loads
    outside DEVICE_FNS;
  - automatic heaps of 256 MB and up take device memory in 64 MB slots as resources land (a968812; `NVMTL_HEAP_LAZY=0`);
    heap members skip residency references, one pin build per heap, every pin installed, 16-link chains collapse flat
    (214612c, 2186041; `NVMTL_HEAP_PIN_LEGACY=1`); parallel render encoders record strided vertex buffers and `useHeap:`
    natively (ffe811f); a texture that owns a view of its image is never re-homed (803448c);
  - drawing into an attachment whose channel order differs from the pipeline's (BGRA into RGBA) through a twin
    pipeline, as Metal does (`nvmtl_format_twin.h`, aac237e, e3ae657; `NVMTL_FORMAT_TWIN=0`); texture copies between
    size-compatible formats are performed (`nvmtl_copy_compat.h`, eafda7b);
  - compute pipelines honour the stage-input descriptor (ba9b57e); opt-in copy-bind for compute buffers at offsets that
    are not storage-aligned, with the kernel's writes carried back (c3d9361, 951651e); nested argument encoders
    (`newArgumentEncoderForBufferAtIndex:`, 3118c6b); `setFragmentTexture:` in the GL path formats no string per bind
    (9182d64); `NVMTL_MANAGED_SYSMEM` is read once and only `1` enables it (bae78ca);
  - capability answers from the Vulkan device (`nvmtl_caps_query.h`, e012ec7/873b3df): `supportsQueryTextureLOD` YES
    while the device is up (the translator now lowers every LOD query shape, see below; upstream answers YES with the 2D
    lowering only), `supportsPullModelInterpolation` from `sampleRateShading`; this copy's own 32-bit float
    filtering/MSAA answers stay;
  - Metal 4 (`NVMTLMetal4.m`, fc4362e, b7b38a8, 29c8b8a, f8c5bfb): command queue, allocator, command buffers, encoders,
    argument tables, compiler and timestamp counter heaps over the Metal 3 objects, `supportsFamily:` Metal 4.
    **Off unless `NVMTL_METAL4=1`** (on upstream; not yet run on the B580); while off, `-gpuAddress` does not touch the
    Metal 4 address table. Counter heaps resolve through this copy's counter sample buffers (stage-boundary sampling,
    ef58dc9), so `queryTimestampFrequency` is 1 GHz (samples are host-clock nanoseconds) when timestamps are offered.
    MetalFX/DLSS hooks are not carried. The device's Metal 4 entry points (newMTL4CommandQueue, newCompilerWithDescriptor:,
    newCounterHeapWithDescriptor:, queryTimestampFrequency and the others Metal's `_MTLDevice` already answers) live on a
    holder class and are copied onto NVMTLDevice only with `NVMTL_METAL4=1` (`nvmtl_m4_install`). Upstream defines them
    as a device category, which replaced Metal's own answers in every process even with the layer off; on the B580 the
    first merged build drew WindowServer's desktop white with icons as empty boxes. `user/tests/metal/mtl_m4_inherit.m`
    (no GPU needed) fails on that build and passes on this one and on the plugin before the merge;
  - the out-of-process AIR decoder is `nvmtl-air-dis` (`air_dis.c`, ac5579d), built beside the plugin by
    `build-intelxe.sh` on the same air-runtime libLLVM, run with an empty environment. The plugin looked for the Metal
    toolchain's `air-opt` in `/Library/GPUBundles/nvmtl`, which this project never shipped, so a library too large for
    the in-process reader failed;
  - release builds create the files they write without following a link (ee45aa5).
  - Not taken, already here in another form: background translation and single flight (b2357a9; this copy's
    `nvmtl_bg_translate_library` jobs), managed no-copy VRAM copies of every size (d49f095; fix13), typed dummy heap
    slots (d69e911; robustness2 null descriptors fill this copy's heap), static samplers on the shader's device (0c69763),
    the sampler slot key (319fb50; `_pixMask`), the GL uniform slot check (0d689c6; `nvmtl_gl_ubo.h`), the image-history
    hash index (6ced1c9; no image history here), function-constant warm-up (2635d4f: this copy translates
    function-constant variants as specializations of one module, and upstream's join can hold a foreground request up
    to 60 s).

### Translator (`translator/`)

- **Structurizer:**
  - a loop's merge block belongs to the construct around the loop. A valid plan for clang-rotated bottom-tested loops is no longer rejected (Geekbench OpenCL `trace_bvh_shadow`: 130 ms → 1 ms per dispatch);
  - lifetime-only blocks count as bare returns;
  - plan-cache comparisons use `Arc` identity or value instead of Debug strings.
- **Rewrites:**
  - OpenCL `native_*` math as AIR `fast_*`;
  - integer atomics on a whole `__local` aggregate get an access chain to the scalar (`AtomicSMax`);
  - one-element vector views (`uchar4` over u32 words) are bitcast.
- **`wrapper/src/buffer_addresses.rs`:** descriptor-kept buffers at offsets that are not a multiple of the storage-buffer alignment are indexed from the rounded-down offset, plus the remainder.
- **Function constants as specialization constants (`wrapper/src/spec_fc.rs`, `meta/`):** `nvmtl_fc_structural` lists the constants the module's shape depends on; `nvmtl_translate_fc_spec2` bakes those, translates with the others unknown and turns each remaining one into a SPIR-V specialization constant. Every other entry point gives byte-identical output.
- **nullmoth 2.7.1 merge, translator part (8 patches):** barrier execution scope from the callee, memory scope from the
  operand; pixel-coordinate linear clamp-to-edge 2D samples without mips through the hardware filter
  (`NVMTL_NO_PIXEL_HW_FILTER=1` keeps the four-fetch emulation); `air.simdgroup_matrix_8x8_init_filled`; a pointer
  phi's load may follow sibling loads and arithmetic (`NVMTL_NO_PTR_PHI_LOAD=1` skips it); a device address plus a
  constant offset as a BDA phi arm; byte-addressed threadgroup parameters stay at their byte offset; the ray-tracing
  group (punned payload access chains, `punned_access_chain.rs`; named types kept apart across linked modules;
  function tables read from the body; the acceleration-structure BDA sidecar; reachability walk in
  `prune_unreferenced_functions`). The function-table runtime of `ift_runtime.rs` stays this copy's.
- **Translation speed (idx1, idx2):** the same SPIR-V, byte for byte and with the same refusals, on 362 recorded shaders
  (arm64 and x86_64), in less time: about a third less for the whole set, up to 72% less for large compute kernels.
  - reflection's normalized text and typed module are reused by the translation that follows
    (`native/module_reuse.rs`, scoped in the wrapper): reflection had parsed every compute kernel a second time;
  - the inliner parses and renders a module once per caller and collects call-result substitutions per caller (was
    quadratic); substitution tokens once per function, not per block;
  - per-instruction parsing skips what the opcode rules out; whole-module scans skip lines by their first byte;
    byte-level `split_top_level`; the vector-scalar merge analysis works on borrowed text, each line parsed once;
  - structurizer: shape regions and construct membership are bit sets, `check_structure` walks dominator intervals,
    switch-bypass reachability shares bit vectors;
  - `wrapper/src/arena.rs` (ours, MIT): inside a translation small blocks come from thread-owned chunks with one free
    list per size, given back when the translation ends; outside it, and for large blocks, the system allocator.
    Peak memory during a translation is higher (259 → 318 MB in a 4-thread stress test), memory kept afterwards lower
    (226 → 157 MB).
- **Function constants that have no value yet are not folded (material fix):** constant folding treated a private
  variable that is never written as a constant. In spec-constant mode (`nvmtl_translate_fc_spec2`, a function constant
  without a baked value) the initial value of `_shader_state.MTL_FC_INIT` is a placeholder zero, so the branches lowered
  from a `switch` were folded as if the constant were 0. SwiftUI's RenderBox took the default shape in every
  specialized variant, its coverage was zero and widgets, the Dock's Calendar icon and other materials drew nothing.
  Unbaked function constants are no longer folded; baked ones still are (unit test in `native/constfold/mod.rs`).
- **Disjoint `or` as add (0.1.4, `disjoint_or.rs`):** LLVM writes some index additions as an `or` of bits that do not
  overlap; the translator turns them back into adds, so Mesa vectorizes the loads (clbench `sgemm_reg` 3778 -> 7130
  GFLOP/s on the B580).
- **Relaxed threadgroup atomics (0.1.4):** an integer read-modify-write with memory order 0 on threadgroup memory is
  emitted Relaxed, not AcquireRelease, so Mesa no longer fences SLM around it; exchange keeps AcquireRelease.
- **Sampled cube texel reads (0.1.4):** a `texturecube` / `depthcube` / `texturecube_array` that is only read keeps
  Dim Cube (it was retyped to a 2D-array image over a cube view, and cube arrays were refused); the read is a nearest
  sample at the texel centre of that face, at the read's level, through the synthesized read sampler, which
  `reflect_sanitized` now reports too. `read_write` cubes keep `OpImageRead`.
- **WebGPU texture resource IDs (0.1.4, `native/texture_handle_loads.rs`):** WebKit's WGSL compiler reads an
  argument-buffer texture as `load i64` + `inttoptr`; such a load becomes the handle load when every use is a texture
  or sampler operand (Safari's deferredRendering sample: "unknown SSA value", zero G-buffer reads). Handles merged by a
  phi or select read the heap slot merged the same way (`emitter/pointers/texture_handle_merge.rs`).
- **nullmoth 2.8/2.9 merge, translator part:** the static initializer's config struct is folded for the function
  constants a variant supplies (b2b597b: Core Animation's `fixed_frag` variants, 4,094 blocks / ~700 KB of SPIR-V to
  20-35 KB upstream); program-scope sampler tables (839327f, run before the inline-sampler step without copying a module
  that has none); texture and sampler reads behind a switch are kept (4bdacbc); a vector loaded from a struct-wrapped
  scalar array (14c19f3); kernel `stage_in` read through the pipeline's stage-input layout (858945d, ba9b57e); the extra
  i32 operand AIR 2.9 adds to every atomic (074dd9b, c4a207b); a selected buffer pointer compared with its arguments
  (a7d5fa6); a sampler chosen by a condition or an index sampled per candidate (4a06e58, a7fdbcf, 4d343e3, b6e49f9); a
  scalar index into a threadgroup vector array (0a7c3e8, 1698287); the civ6 fixture replaced by one compiled from source
  (08c34da); pull-model interpolation (7da9f08), repaired and extended here: Metal's `interpolate_at_offset` counts from
  the pixel's upper-left corner and `InterpolateAtOffset` from its centre, so 0.5 is taken off the offset (upstream
  passed it through: every offset read half a pixel up and to the left); `interpolate_at_center` (an offset of 0, 0);
  half interpolants, interpolated at 32 bits as half varyings travel and converted (upstream refused any fragment
  function with a half interpolant). The struct-wrapped scalar-array load (14c19f3) came to upstream from WatchAndyTW's
  DaVinci Resolve report. In `nvmtl_translate_fc_spec2` (this copy's fc-spec path) the config-struct fold runs only
  after every defined constant is marked defined and with the unbaked ones unknown
  (`specialize_air_function_constants_for_spec`): its evaluator reads a remaining `air.is_function_constant_defined` as
  false, and the first merged build folded each field the initializer guards with `is_function_constant_defined(c) ?
  c : default` to its default for every constant left to a specialization constant. Core Animation's `fixed_frag` reads
  its image functions that way: WindowServer drew a white desktop, every textured and glyph draw empty, on the B580,
  while every SPIR-V corpus stayed byte-identical (the whole-variant path, which bakes every value, was right). The
  fc-spec module keeps those fields as the module before the merge did (530-590 KB for fixed_frag; whole variants fold to
  20-37 KB). Tests: three `fc_air_specialize` unit tests, and `tools/xlate-compare` (the frozen fc-spec module keeps at
  least the whole translation's texture operations on fixed_frag: the first merged build lost them in 3 of 8 sets).
  - **LOD queries on every shape (ours):** `calculate_clamped_lod` / `calculate_unclamped_lod` on 2D array, 3D, cube and
    cube array textures and on the depth forms lower to `OpImageQueryLod` (an array coordinate carries no layer); only
    the 2D form translated before, here and upstream. Tests: `tests/lod_query_and_pull_shapes.rs` on AIR compiled on this
    Mac from `tests/fixtures/lod_pull/probe.metal`; on a device, `user/tests/metal/mtl_lod_pull.m` (LOD 2 for every shape,
    the clamp, and the pull-model offsets, as an Apple GPU draws them).
  - Not taken: parse once per translation (597c591; this copy's idx1/idx2 reuse does the same), the atomic on an
    unindexed `__local` array (3d3d126; `descend_aggregate_atomic_pointers_module`), the test-suite fixes 61d87d1 and
    528bacd (this copy's suite already passes; 61d87d1 would also drop a duplicate ShaderNonUniform and change every
    bindless module's bytes), the curve intersection parsing (44a5b24, 606e526, 33e9ae1; rtopt4), one module per
    function-constant set (334bded, 5a2bfbb; speccst6), upstream's GPU-side ICB encoding (2559379, 7da362c, 378bbd8 and
    the plugin's 185d022, a2d4367, 91221fa, 64ca48f, 7d05ef2: this copy's `render_command.rs` stays the one lowering of
    those intrinsics), tile functions (26ca90e, 5e5e5ae, f339e05, 587c7e5), vertex amplification (66ef15b, 4e94184).
- **`translator/Cargo.lock`:** regenerated by `cargo build` of the wrapper on this machine. It is smaller than upstream's because it resolves only the wrapper's dependency graph.
