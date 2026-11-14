# VRAM-shared IOSurfaces on the B580 (design, 2026-10-29)

> Design note with its date: it records the plan and the state of that day, not the current status. The current
> state is in the README and `docs/devices/b580.md`.

## 1. Why

Measured live (GPUTIME, fix29b, Minecraft 26.3 at 2560x1440, 121-122 fps, Windows 1580-1600 fps at the same spot):

| per game frame (game process only) | GPU ms |
|---|---|
| all render passes (world, sky, GUI, blits) | 0.61 |
| 2 x image -> IOSurface pages copy (14.06 MB each) | 6.8 |
| 3 x managed-buffer uploads (0.17 MB each) | 0.5 |
| everything else (compute, barriers) | 0.1 |
| GPU busy in the game's command buffers | 7.95 of 8.2 ms wall |

WindowServer (not yet profiled, same GPU) adds per game frame:
- an upload of the game's frame from its IOSurface pages back into VRAM (surface-in from imported pages);
- the composite;
- a copy into the scan-out buffer;
- and, with `NVMTL_SCANOUT_READBACK=1` (`full`; since 2026-11-14 the default is phase 4, section 12: the readback cost a B580 at 3440x1440 12 ms of GPU per frame), a readback of the composite into its IOSurface pages.

Transfer speeds on the B580 (gpt_bench, 14.7 MB):

| copy | GPU ms | GB/s |
|---|---|---|
| tiled image -> system memory | 2.0-2.6 | 5.4-7.4 |
| system memory -> tiled image | 1.9-2.1 | 7.1-7.7 |
| linear buffer system -> VRAM | 1.26-1.28 | 11.5-11.7 |
| image -> VRAM buffer | 0.14-0.15 | ~100 |
| VRAM buffer -> image | 0.06-0.07 | 210-240 |

So 90+% of the GPU time of a frame is spent moving finished frames across PCIe (PCIe 4.0 x8), because:
- on Intel/AMD, `nvmtl_gpu_headless()` is 1, so `item 7` (IOSurfaces living in shared VRAM) is off;
- every process keeps a private VRAM image per IOSurface and synchronises it with the IOSurface's system pages:
  - copy-out after every GPU write;
  - copy-in after every foreign write.

On Windows, the frame stays in VRAM from render to scan-out.

## 2. Why two copies per game frame (step 1)

Code path, both copies:
1. `NVMTLCommandBuffer nvmtlSealPartWithSignal`
2. -> `nvmtl_fold_surface_out` for every texture with an IOSurface the part wrote (`_surfDirty`)
3. -> `nvmtl_vk_cmd_image_to_host_scan`: image -> the IOSurface's imported pages, folded into the writing command buffer.

The two IOSurfaces are Apple GL's drawable surfaces. CGLFlushDrawable -> `glSwap_Exec`:
- (a) Minecraft's last pass writes the GL default framebuffer, an IOSurface (the BGRA8 2560x1440 pass `fs 0fab9477`, 84 us);
- (b) at swap, GLEngine blits it into the IOSurface handed to the window server (`glSwap_Exec` -> `glBlitFramebufferEXT_Exec` -> `gldBlitFramebufferData`; the second BGRA8 pass `fs 6fe43146`, 50 us);
- then `gldPresentFramebufferData` presents (b).

Both are IOSurface-backed render targets, so both are copied out.

Only (b) is read by another process (WindowServer). (a)'s pages are read by nobody in steady state: this process's GPU reads (a) from its private VRAM image (the swap blit).

But (a)'s copy-out cannot be dropped *correctly* on its own. Any IOSurfaceLock of (a) — by any process, or by GLEngine itself — must see current pixels, and today nothing tells the plugin about a CPU lock. Without a lock hook, dropping it is a guess. The CPU-lock hook comes with the design below (page-off on lock), and then both copies go away.

SURFCENSUS (fix29c, `/tmp/nvmtl-surfcensus`) logs per IOSurface ID:
- copy-outs per second;
- reads by this GPU;
- whether any process has it in use;
- its IOSurface values.

It confirms (a)/(b) in one 10 s live run.

### Quick, separately testable step: fix29c (staged host copies)

Every image <-> IOSurface-pages copy goes through a linear VRAM buffer. Only surfaces whose rows have no padding take it; the bytes in the pages are the same.
- **Copy-out:** image -> VRAM buffer -> one streaming buffer copy into the pages.
- **Copy-in:** the reverse.
- **Scan-out:** a surface WindowServer scans out reads back from its scan-out buffer instead of the image.

Results:
- **Synthetic:** -28% (copy-out) and -34% (copy-in) GPU time per 14.7 MB copy.
- **IOSurface round-trip test:** byte-identical to the direct path (same FNV of the pages, 0 mismatches); mtl_tests all pass.
- **Run-time A/B:** `/tmp/nvmtl-nohoststage` switches it off.

It helps the game's copy-outs now, and WindowServer's surface-in and readback once WindowServer loads it. It is a mitigation, not the fix.

### Live confirmation (fix31, 2026-10-29, SURFCENSUS over 20 s at the Windows spot, 142 fps)

Six 2560x1440 BGRA IOSurfaces are copied out:
- **Two unnamed surfaces (86, 103), only `CreationProperties`:**
  - ~71 copy-outs/s each, together one per frame: GL's double-buffered default framebuffer, (a).
  - Read twice per write by this process's own GPU: Minecraft's final pass loads it, and the swap blit reads it.
- **Three named surfaces (82, 87, 104), `IOSurfaceName`:**
  - 35-65 copy-outs/s each, together one per frame: the rotating presentation surfaces handed to WindowServer, (b).
  - Read by this GPU once per write, the swap blit's load.
- **IOSurfaceIsInUse** was 0% at copy-out time for all of them, so use counts cannot tell (a) from (b) at that moment.

Phase 2 therefore puts both in VRAM:
- **(a):** its copies disappear entirely; no other process ever imports it.
- **(b):** WindowServer imports it. Its copies disappear as soon as WindowServer runs phase 3.

fix29c (staged copies) live: 123.5 -> 142.1 fps, and 7.92 -> 6.90 GPU ms/frame (img2buf 6.73 -> 5.68 ms).

## 3. Target

An IOSurface that a GPU writes or reads gets one VRAM backing, shared by every process that uses it with the GPU. The game, WindowServer, the scan-out and the CPU all use that one copy:

- **GPU writes and reads (any process):** go straight to the shared VRAM BO. Linear layout, pitch = the IOSurface's bytes-per-row, so the display can scan it out and the CPU layout matches.
- **GPU <-> GPU across processes:** ordered by the existing surface fences (INTELXE_CAP_SURFACE_FENCES): the writer attaches, readers wait on the GPU. Unchanged.
- **CPU reads (IOSurfaceLock):** page-off. The kernel copies VRAM -> the surface's pages with the copy engine, only when the device copy is newer, then the lock proceeds. This is Apple's semantics for accelerator-resident surfaces.
- **CPU writes (lock for write, unlock):** the seed moves. The next GPU user uploads pages -> VRAM once (page-on), with the item-7 VRAM tag (seed + generation in the IOSurface's values) shared by all processes, so the upload happens once, not once per process.
- **Scan-out:** WindowServer's presented surface is flipped from its own VRAM BO (IntelXeFB maps its DPA runs into GGTT). No copy into a scan-out buffer, no readback.

Per game frame, the cost goes from about 7-10 ms of PCIe copies to about 0. The swap blit (50 us) and the composite remain.

## 4. Components and changes

There are four parts. The kernel and Mesa changes are separate patches against the agreed bases:
- IntelXe.kext: `src/b580-vis` (pv);
- Mesa: `mesa-venc-rel` + 0009-0011, new patch 0014;
- NMXAccel: `src/nullmoth/kexts/NVRM/accel` (NMX_ACCEL build);
- plugin: separable patch on fix29b / fix30.

### 4.1 IntelXe.kext (xe_core): surface VRAM registry

**`XeSurfVram { sid, plane, bytes, pitch; XeBo *bo; refs; state }`**
- Global table, under its own lock.
- The BO is VRAM, preferably outside the CPU window (gHi), pinned (`noMove`, like a scan-out pin) and uncompressed (`noComp`: linear, display- and CPU-layout).
- The BO is contiguous when it can be (scan-out), else in runs (IntelXeFB maps runs).
- It has no CPU mapping: nobody maps it. The CPU sees only the IOSurface's own pages.

**New xe ioctl `DRM_INTELXE_SURFACE_VRAM`** (intelxe_drm.h, next to the surface-fence ioctls):
- `{ sid, plane, size, flags(CREATE|IMPORT_ONLY) } -> { handle, size, gen }`.
- Permission: `IOSurfaceRoot::lookupSurface(sid, task)` must succeed for the calling task, so a process can only import surfaces it can already see. NMXAccel already uses this symbol.
- Creates the registry entry on first use (CREATE: the writer asks), or fails with ENOENT (IMPORT_ONLY: a reader asks for a surface nobody put in VRAM).
- Adds a handle for the BO to the caller's client table. The BO is shared across clients: refs per client handle and per binding. The registry entry lives while any handle or the IOSurface lives.
- Budget: `debug.intelxe.surfvram_mb` (default 1024). Over budget, it is refused and the caller keeps today's copy path.
- Master switch: `debug.intelxe.surfvram` = 0 off (default at first), 1 apps only, 2 apps + WindowServer.

**`DRM_INTELXE_SURFACE_DIRTY { sid, plane, flags }`**
- The device copy is newer: called after a GPU write is submitted. The writer's fence is already attached through the surface-fence ioctl.
- The kernel records `state = DEVICE_NEWER, writerFence`.
- It tells NMXAccel to mark the family's device cache current, so a CPU lock calls the accelerator's page-off.

**`intelxeSurfPageoff(sid, plane, IOMemoryDescriptor *pages)`** (callPlatformFunction, for NMXAccel):
1. If DEVICE_NEWER: wait for the surface's writer fences.
2. Bind the wired pages into the kernel migrate VM.
3. Copy BO -> pages on the copy engine (the same kernel queue VRAM clears use; `MEM_COPY` linear).
4. Wait.
5. Set state = SYNCED.

**`intelxeSurfPageon`** (optional, for 4.4's tag logic). The pages -> BO direction: an alternative to the plugin's upload when the CPU wrote.

**Lifetime:**
- When the IOSurface is destroyed, its entry goes when the last handle closes. Kernel notifier on IOSurfaceRoot or lazy: checked at import and at budget pressure.
- When a client dies, its handles go; the entry stays while other holders remain.

**Coordination with the pv window agent:**
- The BOs come from the same VRAM allocator, as non-visible (gHi) pinned BOs.
- They never enter the CPU window.
- debug.intelxe.vram accounting gets a "surface" class.

### 4.2 NMXAccel: CPU-lock hook (page-off)

- `NVResource::pageoff`, NMX_ACCEL branch: when B580 says the surface (by ID and plane) is DEVICE_NEWER, call `intelxeSurfPageoff` with the surface's memory descriptor (as the NVIDIA branch already does for its CE copy). Otherwise keep today's path: wait for the plugin's mark, family pageoff.
- A dirty-mark entry for B580 (platform function `nmxSurfDirty`): `deviceCacheForIOSurface` + the existing `nvAccelSurfDirty` valid-bit update (`valid[6] |= plane`, `valid[0] &= ~plane`, `IOSurfaceDeviceCache::setCurrent`). Unlike the NVIDIA path it needs no vidmem handle: the B580 registry holds the VRAM.
  - This is how a CPU IOSurfaceLock in any process reaches `pageoff`.
  - **Must be verified first (phase 0):** that the family calls `pageoff` for a surface marked this way without an NVVidMemory backing. If it insists on vidmem, NVVidMemory gets a B580 "external" backing that points at the registry BO, with no kva.
- WindowServer's scan-out path (`nmxScanout`, IntelXeFB): accept the surface's registry BO (DPA runs) as the flip source.

### 4.3 Mesa ANV (xemac), patch 0014

- `nvmtl_anv_surface_vram_import_v1(VkDevice, uint32_t sid, uint32_t plane, uint64_t size, uint32_t flags, VkDeviceMemory *out, uint64_t *gen)`:
  1. ioctl `DRM_INTELXE_SURFACE_VRAM`;
  2. wrap the GEM handle as a device memory: `anv_device_import_bo` with the handle, like the PRIME fd import path minus the fd, uncompressed PAT, never mapped;
  3. return a VkDeviceMemory of the memory type a linear image accepts.
- `nvmtl_anv_surface_dirty_v1(VkDevice, sid, plane)`: ioctl `DRM_INTELXE_SURFACE_DIRTY`.
- Both are exported like `nvmtl_anv_surface_*_v1` (anv_nvmtl_surface.c). The plugin dlsyms them; absent means off.

### 4.4 Plugin (nullmoth): item 7 on the B580

Reuse the NVIDIA item-7 code, which is already vendor-neutral above the import:
- `nvmtl_vk_surface_share_on()`: on Intel when `panv_surf_vram` exists and the kernel accepts it (no "headless" veto).
- `nvmtl_vk_surface_vram()`: the ANV import instead of `pnvk_stage_import`. Policy:
  - CREATE when this process will write the surface (render target, blit or storage destination);
  - IMPORT_ONLY for read-only users;
  - CPU-drawn window backing stores therefore stay in system memory and are uploaded as today.
- `nvmtl_vk_surface_mark_newest()`: `nvmtl_anv_surface_dirty_v1` after the writing part's submit. Folded with the existing `isyncPublish`: the fence is attached first, then dirty, then the seed bump.
- The seed bump after a GPU write (`nvmtlSurfaceOutFolded`) uses `kIOSurfaceLockAvoidSync`, so the plugin's own bookkeeping lock does not trigger a page-off.
- Surface-out/in for `_vramOn` surfaces: already skipped (`_vramOn` branches); VRAM tags (seed + gen in the IOSurface values) decide when a CPU write must be uploaded.
- WindowServer scan-out: a presented `_vramOn` surface flips from its own BO (`nmxScanout` with the registry handle); `nvmtl_scanout_readback` is moot for it.

### Phase 0 result (2026-10-29, static analysis of the host's own family binary)

Source: IOAcceleratorFamily2 from the hackintosh's SystemKernelExtensions.kc (macOS 15.8.1). The kext was cut out at vmaddr 0x14e13000, size 0x6a000, and disassembled with symbols from the KC.

- A CPU lock reaches the accelerator through `IOGraphicsAccelerator2::deviceCacheControl(cache, op, ...)`.
  - The page-off op calls `IOAccelResource2::requirePageoff()`: the per-plane `valid` words at res+0x30, device-valid at +0xc and CPU-valid at +0x0 per plane, the same bits `nvAccelSurfDirty` sets.
  - Then it calls `pageoffIfNeeded(0x3f, ~0)` through the vtable (+0x260).
- `IOAccelResource2::pageoffIfNeeded` refuses a resource without `fVidMemory` (res+0x88). It logs "Requested pageoff of texture without fVidMemory. Perhaps there was a previous allocation failure?" and returns.
- It also refuses a vidmem whose state is (flags & 6) == 4, logging "Requested pageoff for texture not in vram!".

**So the marks alone are not enough.** A B580 surface needs an IOAccelVidMemory (our NVVidMemory) that the family considers wired in VRAM. The design therefore takes the "external backing" variant:

1. The B580 escape (4.2) calls `deviceCacheForIOSurface`. That creates the NVResource and an unbacked NVVidMemory through the family factory.
2. It publishes the registry entry as the pending backing (under the family lock), then calls `IOAccelVidMemory::wire()`. `wire()` runs the family's accounting and state changes, and calls `NVVidMemory::allocPhysical()`.
3. `allocPhysical()` adopts the registry BO instead of asking IntelXeFB's `nvAllocVram`: `fRmHandle` = the registry entry, `fPhys` = the BO's first DPA, `fKva` = NULL, `fBytes` = the BO size, and a new `fIntelXe` flag.
4. `deallocPhysical()` of an fIntelXe vidmem drops only its registry reference. Parking does not apply.
5. `NVResource::pageoff`/`pageon` for fIntelXe vidmem call B580's copy-engine copies and never the family's CPU copy (there is no kva).

## 5. Phases, each staged and measured

| phase | what | gate |
|---|---|---|
| 0 | NMXAccel experiment: does a CPU IOSurfaceLock reach `NVResource::pageoff` for a surface marked current via `deviceCacheForIOSurface` + valid bits with no vidmem? (`debug.nvaccel_svdiag`) | yes, or choose the NVVidMemory-external variant |
| 1 | kernel registry + 2 ioctls + page-off copy (switch default 0) | kxtest: import from two processes, CPU lock sees GPU writes, budget refusal, client death |
| 2 | Mesa 0014 + plugin import/dirty, `debug.intelxe.surfvram=1` (apps only) | gpt_bench two-process IOSurface test (writer/reader by ID, CPU lock coherency, seed semantics), mtl_tests, render-test images byte-identical |
| 3 | WindowServer import (`=2`), still with scan-out copy | desktop QA: login, Safari, video, Dock/Mission Control, screenshots (screencapture), window thumbnails; Minecraft GPUTIME (game + WS) before/after |
| 4 | WindowServer scan-out from the surface BO (no scan-out copy, no readback) | flips counted, no tearing/stale frames, screenshot correctness |

Every phase is off by a switch until its gate passes. There is no WindowServer opt-in until phase 3, and no default-on until the user has used it.

## 6. Risks

- **IOAccel family internals** (`valid` bits, device cache offsets) are reverse-engineered. NMXAccel already relies on them for the NVIDIA path, but the B580 branch (no vidmem) is new. Phase 0 checks this.
- **WindowServer stability:**
  - phase 3+ only, behind `debug.intelxe.surfvram=2`;
  - any import failure falls back to today's copies;
  - a page-off wait is bounded (like `nvSurfWaitReady`): a hung GPU never holds a CPU lock forever.
- **VRAM use:** budgeted. GPU-written surfaces only; window backing stores stay in system memory.
- **Linear render targets:** IOSurface surfaces become linear, uncompressed render targets. Only the final blits write them (about 0.1 ms per frame now); the world pass renders into tiled, compressed RTs as before.
- **Concurrency with the pv agent's VRAM window work:** surface BOs are pinned non-visible VRAM, outside the window, and accounted as their own class.

## 7. Status (2026-10-29, phase 1 checkpoint)

Phase 1 is staged as sv1 (~/b580/sv1, src/nullmoth-gpuperf/stage/sv1). Built and signed; **not yet loaded or run on the host.**

**IntelXe.kext (src/b580-surfvram, patch b580-surfvram-vs-pv.patch on pv)**
- Registry: `SurfVram` entries, pinned (`pins`, `noMove`), uncompressed, outside the CPU window. Cleared on the copy engine before first use; saved across sleep (pmAdd).
- uAPI:
  - `DRM_IOCTL_INTELXE_SURFACE_VRAM` / `_DIRTY` (0x53/0x54);
  - `INTELXE_CAP_SURFACE_VRAM`.
- Platform functions: `intelxeSurfAdopt` / `intelxeSurfRef` / `intelxeSurfRelease` / `intelxeSurfCopy`. The copy is MEM_COPY page mode on the migrate queue, through a `kSurfVa` window onto the IOSurface's pages (DmaMap). It waits first for the surface fences (writers; towards VRAM, readers too); both waits are bounded at 2 s.
- sysctls:
  - `debug.intelxe.surfvram` (0 off, the default; 1 apps; 2 apps + WindowServer);
  - `debug.intelxe.surfvram_mb` (1024);
  - `debug.intelxe.surfvram_stats`.
- Plane 0 only.

**NMXAccel (src/nullmoth-gpuperf/nmx, patch nmxaccel-surfvram-vs-gated.patch on the installed gated build)**
- `NVVidMemory::fIntelXe` adoption in `allocPhysical`, while `nmxSurfVram` wires it under the family lock.
- Parking across the family's dealloc/alloc: the same VRAM comes back; the registry reference is dropped in `free()`.
- `getPhysicalSegment` returns 0 for fIntelXe: there is no CPU address.
- `NVResource::pageoff` / `pageon` route to `intelxeSurfCopy`, and never to the family's CPU copy.
- `nmxSurfVram` / `nmxSurfDirty` (the latter re-wires an unwired fIntelXe vidmem before setting the valid bits).

**Test:** tests/surfvram.c covers:
- create + import (same backing);
- GPU write, fence, dirty, then a CPU lock showing the pixels;
- a second process importing, reading and writing, then exiting;
- the CPU lock showing that write;
- ENOENT / EINVAL / ENOSPC;
- the registry empties after release.

**Untested (needs the install + reboot):**
- whether a CPU IOSurfaceLock really reaches `NVResource::pageoff` for these surfaces (phase-0 static analysis says the vidmem requirement is met; the family's lock path itself is unexercised);
- the lock order under load (family lock -> B580 registry/VRAM locks, copy waits up to 2+2 s under the family lock);
- suspend/resume with backings;
- IOSurface destruction -> vidmem `free()` timing.

**Remaining after phase 1:**
- Phase 2: Mesa 0014 import/dirty entry points, plugin item 7 on Intel, the `kIOSurfaceLockAvoidSync` seed bump.
- Phase 3: WindowServer import.
- Phase 4: scan-out from the backing.

## 8. Status (2026-10-29, phase 2 staged as sv2; switch off by default)

**sv1 results on the host** (installed 14:57, merged kernel b9b9a9595bec + NMXAccel 1de11083f628): surfvram all PASS.
- page-off on CPU lock: 3029 us; second process's write seen: 1687 us; nothing newer: 1 us.
- One FAIL, the 3 GB backing case answered EBUSY instead of ENOSPC. Cause: a refused adoption fell through in `allocPhysical` to IntelXeFB's system-memory `nvAllocVram`, so the surface then had "another backing". Fixed in NMXAccel 1d5e9039f7eb: adoption returns -1/0/1, and -1 makes `allocPhysical` fail.

**Mesa 0014** (patches/mesa/0014-...; applies on mesa-venc-rel; staged with 0013):
- `nvmtl_anv_surface_vram_cap_v1`: the switch level.
- `nvmtl_anv_surface_vram_stage_v1`: a thread-local request. The next `vkAllocateMemory` with a `VkImportMemoryFdInfoKHR` (placeholder fd) on that thread becomes `DRM_IOCTL_INTELXE_SURFACE_VRAM`, then `anv_device_import_bo_handle` (new; like the PRIME import, but by GEM handle).
- `nvmtl_anv_surface_dirty_v1`.
- No fd-sharing extension is needed; macOS ANV has none.

**Plugin** (plugin-p2 = fix32 + sv2-plugin-vs-fix32.patch):
- `nvmtl_vk_anv_vram()` gates item 7 on ANV: the switch level must be >= 1 (>= 2 in WindowServer), the Mesa entry points must exist, and implicit sync must be on.
- Writers (RenderTarget or ShaderWrite wrappers) make the backing; readers only import one.
- The memory type is one a linear image accepts.
- Every read of a `_vramOn` surface records an implicit-sync READ (`nvmtl_surface_in_for_cb`). Every write records a WRITE in the sealing part and is published at submission: SURFACE_DIRTY plus the VRAM tag bump through `nvmtlSurfaceOutDirtied`, so readers see the new generation immediately. A CPU lock waits for the writer fences in the kernel page-off. There is no copy-out.
- The seed is not bumped with a CPU lock for `_vramOn` surfaces (the item-7 code already only bumps the VRAM tag), so `kIOSurfaceLockAvoidSync` was not needed.

**Tests:** svimg (two processes, byte-exact hashes), gpt_bench surf, mtl_tests. All PASS with the switch at 0 (copy path), using the staged plugin and Mesa through NVMTL_BUNDLE / NVMTL_ICD_LIB. **With the switch at 1: not yet run** (it needs sudo).

**Expected with the switch at 1:**
- Game: no copy-outs of (a) or (b).
- WindowServer (still copies at level 1): each new (b) frame costs a CPU-lock page-off (copy engine, ~1.5-3 ms on WindowServer's thread, off the 3D engine) plus its upload.
- Risk: that synchronous page-off on WindowServer's thread may cap its frame rate. Phase 3 (WindowServer imports, level 2) removes it.

## 9. Phase 2 live result and phase 3 (2026-10-29)

**Live at level 1** (capC; WindowServer at level 0, game 100 fps, previously 142):
- The game's img2buf is gone (transfers 0.27 ms/frame).
- The game is only 33.9% GPU-busy, with 6.6 ms/frame idle between its command buffers, so it is no longer GPU-bound. Its frame is held up elsewhere.
- WindowServer pages (b) off on every frame: 111 page-offs/s, ~1.4 ms each, synchronous inside its IOSurfaceLock, under the accelerator family lock. Then it uploads (b) again.

**Render-pass time 0.61 -> 3.06 ms/frame**, two separate effects:
1. **Linear, uncompressed targets (intrinsic):** only the two GL blits write the shared backings:
   - Minecraft's blit into (a): 84 -> 332 us.
   - The swap blit (a)->(b): 50 -> 478 us.
   Together ~0.7 ms/frame.
2. **Not linear at all:** the main world pass renders into a private Tile4 RGBA8 target, yet it went 392 -> 1634 us, and every pass now has a ~2 ms maximum. That is time-slicing: WindowServer's upload/composite jobs and the copy-engine page-off run inside the game's pass intervals (the timestamps measure wall time on the engine, not this context's own work).
   - The game's submit thread also blocked on the family lock: its SURFACE_DIRTY took the lock without a try while WindowServer's page-off held it.

**Phase 3 (sv3, plugin only; Mesa and kernel unchanged):**
- **WindowServer at level 2 imports app backings (IMPORT_ONLY) and never makes its own.** Its composite targets stay private until phase 4. It reads the game's frame directly from VRAM, and its reads/writes are ordered through the surface fences: no CPU lock, no page-off, no upload.
- **The switch is re-read every second** (one GET_CAP), so level 2 can be set without restarting WindowServer. Surface textures made afterwards follow it.
- **SURFACE_DIRTY at submission is a try-lock;** a busy lock is retried off-thread (`nvmtl_surface_remark_later`), so the submit thread never waits on a page-off.

**On tiled shared backings:**
- The linear cost (~0.7 ms/frame) is small next to what sharing removes (5.7-6.8 ms of copies per frame).
- A Tile4 backing would need a tiling-aware kernel page-off (BCS XY_FAST_COPY_BLT, Tile4 -> linear), a Tile4 scan-out (IntelXeFB PLANE_CTL tiling), and an agreed modifier in every importer.
- Decision deferred until phase 3 is measured.

## 10. Level 2 live (capD) and phase 4 (2026-10-29)

**Level 2 works:** WindowServer imports the game's backings. Page-offs stay flat (3 -> 3), and its SURFCENSUS shows game surfaces 77/81/96 [NVMTLVramAt] with 0 copy-outs.

**The game runs at 135 fps but is 70% idle.** The pipeline is latency-bound, not GPU-bound:
- The GL swap cannot run more than the surface queue's depth ahead of WindowServer.
- For each frame WindowServer does:
  - a 205 us composite pass;
  - then, in the same submission, 28 MB of copies (~1.96 ms per frame): its composite target goes into the scan-out buffer and, with `NVMTL_SCANOUT_READBACK=1`, back into the target's IOSurface pages over PCIe;
  - then NMXAccel waits for the mark and flips.
- That ~2 ms per frame sits on the critical path between "game frame ready" and "game may reuse a surface". The GPU as a whole is only ~60% busy (game 29.5% + WindowServer ~30%).

**Game render passes 1.89 ms (0.61 at level 0):**
- ~0.7 ms is the two linear blits into the shared backings.
- The rest is WindowServer's copy jobs time-sliced into the game's pass intervals (pass maxima of ~2 ms appear only with WindowServer active).

**"dirty failed" +1:** SURFACE_DIRTY at submission is now a try-lock. A busy family lock counts as a failed dirty in debug.intelxe.surfvram_stats and is retried off-thread (`nvmtl_surface_remark_later`), so nothing is lost.

**Phase 4: the readback is what remains.** WindowServer's composite targets (CoreDisplay framebuffer surfaces 1/3, IOSurfaceEDRFactor) are presented through the display pipe. In the family they already carry the display pipe's vidmem (`initFramebufferResource` SCANOUT vidmem or IntelXeFB's `nvAllocVram`), so a registry CREATE on them answers OTHER_BACKING (EBUSY). They cannot simply become registry backings without changing the display pipe.

Proper design:
1. **Readback dropped.** WindowServer skips the sysmem readback for a bound surface. Only the VRAM -> VRAM scan-out copy stays (0.14 ms; it could later become a flip of the composite target itself).
2. **CPU readers stay correct.** For a bound surface marked "scan-out buffer newer", NMXAccel's `NVResource::pageoff` (B580 branch, non-fIntelXe) copies the pinned scan-out BO (gNmxScan pin, DPA runs) into the surface's pages on B580's copy engine, after the surface's mark. New B580 platform function: `intelxeCopyPinToPages`.
3. **Kernel IOP copy fallback.** The IOP copy path (a frame copied instead of flipped) reads the scan-out buffer instead of the IOSurface pages for such surfaces.
4. **Marking.** The plugin marks "scan-out newer" through the IOAccelNMXPresent client it already uses for its pending/ready marks.

**Before building it: measure the gain.** sv4 adds the root-owned diag flag /tmp/nvmtl-no-scanout-readback in WindowServer, which drops the readback at run time. While it is set, CPU readers of the composite (screenshots of the whole display, a non-flipped frame) may see an older frame. It is a measurement switch, not the fix.

## 11. sv8 result and making level 3 the default (2026-10-29)

**sv8 (level 3)** = Minecraft at the Windows spot, measured:

| | value |
|---|---|
| game fps | 230-232 |
| game GPU | 1.88 ms/frame (43.5% busy) |
| CPU page-offs per frame | 0 (only on real CPU reads; a screenshot = 1) |
| WindowServer GPU busy | 9.8% (only the 14 MB VRAM scan-out copy per composite) |

**What limits it now:** Minecraft's own frame limiter, not the driver.
- jstack of the render thread: `FramerateLimiter.limitDisplayFPS` -> `LockSupport.parkNanos`.
- 79% of the render thread's samples are in that park. It is busy ~16% of the time, about 0.7 ms/frame.
- Raising Max Framerate to Unlimited shows the next limit.
- Projected from the measured times only, without the limiter the game would be bounded by GPU time at about 1.9 ms/frame. That is not CPU per-draw cost, so descriptor stage 2 is not the bottleneck now. Unmeasured: needs a run with the limiter off.

**Before level 3 can be the default:**
1. **Boot default.**
   - IntelXe.kext takes `debug.intelxe.surfvram` from a boot-arg (e.g. `intelxe_surfvram=3`).
   - Or intelxefb-load.sh sets it before WindowServer restarts at the login window.
   - The plugin re-reads the switch every second, so either way works without a WindowServer restart.
2. **Sleep/resume.**
   - Registry backings are in the suspend save list (pmAdd). Verify that a game left running across sleep shows its frames after wake.
   - Scan-out pins and bindings survive or are rebound. On wake, the "newer" composites must page off correctly: nothing reads them before the first new frame, but test it.
3. **Multi-display / mode changes.**
   - Bindings are per surface and per mode geometry. A mode change unbinds, and `nvmtl_scanout_drop` pages off a newer composite first.
   - Test: mirroring, a second display, resolution changes, HDR/EDR toggles.
4. **CPU readers of the composite.**
   - Cmd+Shift+3/4 already verified.
   - Still to test: QuickTime/ScreenCaptureKit screen recording (continuous captures: watch p4_pageoffs, which should be about 1 per captured frame), Screen Sharing/remote control (the Telegram bridge), the Dock/Mission Control thumbnails, and a minimized window's genie.
5. **Copy-instead-of-flip.** Force a refused or late flip (debug.nmxaccel_flip=0 at runtime) and check that the copied frames are current (they page off first).
6. **Stress.**
   - Games and video at the same time; WindowServer restart (relogin) with live backings; a client killed mid-frame.
   - Long runs: watch `debug.intelxe.surfvram_stats` (backings return to the start count when apps quit), `p4_failed`/`p4_busy` = 0, and the VRAM accounting.
7. **Lock order.**
   - Watch for any hang involving the family lock: pageoff tries the scan lock and gives up after ~20 ms, which is counted as `p4_busy`.
   - A spindump of WindowServer on any stall.

**Correction (16:55):** the FramerateLimiter evidence in §11 was taken while the user was idle, so the game was in the AFK throttle.
- 26.3's `Minecraft.renderFrame` calls `FramerateLimiter.limitDisplayFPS(limit)` only when `limit < 260`.
- `FramerateLimitTracker.getFramerateLimit` returns:
  - the option itself (maxFps:260 = unlimited) normally;
  - 10 when iconified, or unfocused in exclusive fullscreen;
  - 30 after 60 s and 10 after 10 min without input (inactivityFpsLimit "afk");
  - 60 outside a level.
- So at 230 fps with active input, the limiter is not engaged. The limiter at 230 fps is still unknown; ~/b580/playcap.sh (jstack x20 + sample during active play) will show it.

## 12. Why phase 4 did not engage, and the fix (2026-11-14)

**Report (issue #7, B580 at 3440x1440 165 Hz):** WindowServer spent 12.8 ms of GPU per frame reading every composite back
into its system-memory pages (19.8 MB over PCIe); `debug.nmxaccel_p4_*` stayed 0 and the plugin log had no "phase 4:" line.

**Root cause (plugin, `nvmtl_fold_surface_out`):** phase 4 was gated on two switches it does not use:
- `nvmtl_vk_anv_level() >= 2`, i.e. `debug.intelxe.surfvram >= 2` (VRAM-shared IOSurfaces). The kext's default is 0 and
  only the boot-arg `intelxe_surfvram=2` (install stage 2) raises it, so on a machine without that boot-arg every frame
  was read back. Pitch (13760), plane offset and page import were fine, and the kernel was never asked (all p4 counters 0).
- `nvmtl_scanout_readback()`: phase 4 only ran while the readback switch was on. td016 (31ea921) turned the readback off
  by default, which disarmed phase 4 everywhere: the pages then kept an old frame with no mark, so CPU readers of the
  composite and frames the kernel copies instead of flipping could see stale contents.

**Fix (`nullmoth/plugin/nvmtl_scanout_p4.h`):** one per-frame decision for a bound composite, independent of surfvram:
- `NVMTL_SCANOUT_READBACK` unset = phase 4 (default): the folded copy writes the scan-out buffer only; at publication the
  binding is marked newer (NMXAccel selector 2), so a CPU lock, a copied frame or an unbind pages the frame off from the
  scan-out buffer on the copy engine. `1`/`full` = read back every frame; `0`/`off` = no readback and no marks
  (measurement only). Root-owned diag flags switch at run time: `/tmp/nvmtl-scanout-readback-full`, `/tmp/nvmtl-no-scanout-readback`.
- Phase 4 still needs the scan-out layout in the pages (same pitch, plane at offset 0, an allocation that holds the
  page-rounded frame: the kernel's page-off copies whole pages). Anything else reads back, and says why once per binding:
  `phase 4: surface N WxH pitch P (...): ENGAGED / not engaged - <reason>`; a summary every 5000 frames
  (`phase 4 summary`).
- A refused mark reads that frame back and holds phase 4 off for that binding (1 s, doubling to 64 s), instead of a
  skipped folded readback plus a separate copy-out every frame.
- Whether a folded frame skipped its pages is recorded per part (`foldP4`): `_p4` on the texture was overwritten by a
  later part folded before the earlier one was published, which could publish skipped pages without a mark.
- An unbind pages a marked composite off whatever the surfvram level (`nvmtl_scanout_drop`).

**CPU readers:** an IOSurfaceLock of the composite (screenshots, captures that read the pages, another process's surface-in)
reaches NVResource::pageoff through the valid bits the mark set and gets the newest frame from the scan-out buffer;
kIOSurfaceLockAvoidSync readers get the pages as they are. Between the composite's ready mark and its newer mark (the
same completion thread, microseconds) a CPU lock sees the previous published frame, as before phase 4.

**Tests:** host `user/tests/plugin/scanout_p4_unit.sh` (modes, layout rules, the stale-pages safety rule over every input,
real IOSurfaces at 2560x1440 and 3440x1440); B580 `user/tests/wsi/night-phase4.sh` with `p4check` (WindowServer GPUTIME
per mode, smoothness, screenshots of a held colour, a recording of changing colours, copy-instead-of-flip).
