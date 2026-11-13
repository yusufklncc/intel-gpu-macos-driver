# Zink's extra GPU work per frame on the B580 (research and kit, 2026-11-13)

> Design note with its date: it records what the source says and what was built that day (branch `zinkgap`), and the
> first B580 night of the kit (section 6, 13 Nov evening) with what was made default from it (branch `zinkdef`).

## 1. The question

Minecraft 1.21.1 with Sodium, Iris and BSL (HIGH), windowed: zink 197-205 fps, Windows 214-218 fps (ROADMAP, "OpenGL
through Zink"). Windows' GPU is not fully busy, ours is: the remaining 5-7% is GPU work per frame (~0.3 ms of ~5 ms).
Already ruled out (report of 6 Nov): WindowServer's GPU share (2.6%), the scan-out copy (off: no change), the GPU clock
(2850 MHz), compression (CCS is on). This note lists where zink on ANV does GPU work a native GL driver does not, from
the source of the trees we ship: zink `src/mesa-zink` (`93389ec0c23`, patches `0071`-`0073`), ANV `mesa014`
(`f01e9b07a91` + `mesa/patches`). Line numbers are of those trees. Only general mechanisms: no knob looks at the
application.

## 2. Candidates, ranked

| # | What | Per frame | Certain from the source | Knob (all opt-in) |
|---|---|---|---|---|
| 1 | ANV flushes the render target cache with a stall at **every** `vkCmdBeginRendering` and around every blorp operation (`ANV_PIPE_RT_BTI_CHANGE`), which also disables Xe2's cheap `RESOURCE_BARRIER`: every render-to-texture boundary becomes a full drain | one drain per pass boundary, per mip level, per copy | path yes, cost no | `anv_state_cache_perf_fix=true` (existing driconf, set by env or the options file) |
| 2 | `glGenerateMipmap`: one blorp blit per level, whole-image barriers between levels, each with an end-of-pipe sync | one drain per level of each mipmapped target | path yes, cost no | #1 helps; a real fix is a zink `generate_mipmap` (section 4) |
| 3 | Small BAR: every buffer of 8 KiB or more is `DONT_MAP_DIRECTLY`, so `glBufferSubData`, orphaning `glBufferData` and INVALIDATE maps become a staging buffer plus a GPU copy (a blorp 3D draw), often ending the render pass | one copy + barrier per such write | yes | `ZINK_BAR_BUFFERS=sysmem` / `=all` (patch `0107`) |
| 4 | Render pass splits: an upload or copy into something the batch already used goes to the main command buffer and ends the render pass; query result copies too | End/Begin + #1's drain + state re-emit per split | yes | `ZINK_DEBUG=rp` (zink's tracking, off on Intel) |
| 5 | Descriptor model: zink uses descriptor buffers on ANV (fully bindless, no surface-state prefetch, fresh descriptor addresses) | latency per draw | path yes, cost no | `ZINK_DESCRIPTORS=lazy` |
| 6 | Depth: HiZ write-through for every depth image (zink always adds SAMPLED); write bandwidth only, no resolve on Xe2 | small | yes | reference `INTEL_DEBUG=nohiz` |
| 7 | Present: Tile4 swapchain in the IOSurface's VRAM, no copy, no resolve; only the last pass writes an uncompressed target | small | yes | none |
| - | Image flags (MUTABLE, STORAGE, feedback loop, input attachment) do **not** cost compression on Xe2; color has no CLEAR aux state there, so no layout change resolves; fast clears work in any layout | none | yes | references `INTEL_DEBUG=noccs`, `nofc` |
| - | Native CGL applications only: an IOSurface texture that is drawn into is read back into the surface at **every** `glFlush`, `glFinish` and present (`zink-gl/cgl`) | one read-back (a GPU sync) per flush | yes | `CGLZINK_SURF_WRITEBACK=dirty` / `present` |

### 2.1 The render-target-change flush (#1)

- `rt_change_needs_flush` is true unless the driconf option `anv_state_cache_perf_fix` is set and the kernel reports
  `DRM_XE_QUERY_CONFIG_FLAG_HAS_DISABLE_STATE_CACHE_PERF_FIX` (`anv_physical_device.c:3263-3271`). Upstream sets the option
  only for vkd3d (`00-anv-defaults.conf:262`). IntelXe.kext reports the flag and implements the exec queue property
  (`kernel/IntelXe/xe_core.cpp`: query flags ~5889, `COMMON_SLICE_CHICKEN3` bit 13 in the ring ~2956), which ANV asks
  for when the option is on (`xe/anv_queue.c:145-150`).
- With the flush needed, `vkCmdBeginRendering` adds `ANV_PIPE_RT_BTI_CHANGE` (`genX_cmd_buffer.c:7063-7080`), blorp before
  and after each operation (`genX_blorp_exec.c:312`, `397`). That bit disables `RESOURCE_BARRIER` (`genX_cmd_buffer.c:1980`)
  and becomes a stall at scoreboard plus a render target flush (`2205-2209`). Zink's barrier for a target that is then
  sampled (COLOR_ATTACHMENT_WRITE -> SHADER_READ) is in the same pending bits: flush plus invalidate needs an end-of-pipe
  sync, a CS stall with a post-sync write (`2316-2322`, `2373-2378`).
- With the option, binding tables are no longer used for anything but render targets
  (`anv_nir_apply_pipeline_layout.c:187`, `BINDING_PROPERTY_NO_BINDING_TABLE`), and the shader cache key changes
  (`anv_shader_compile.c:285`). For zink's descriptor-buffer mode this changes nothing (it is bindless already); with
  `ZINK_DESCRIPTORS=lazy` it removes the binding-table prefetch #5 is about.
- A deferred shader pack has 10-20 full-screen passes and several mip chains: 30-60 drains a frame. At 10-30 us of
  pipeline drain each, that is 0.3-1.8 ms of a 5 ms frame: the size of the gap. Inferred, not measured.
- Risk: if the hardware's state cache is not keyed as the option assumes, render targets read stale surface states
  (corruption, not a hang). The kit's image hash checks the bench frame; Minecraft needs a look.
- Why upstream limits it to vkd3d (commit `af2d51eafa9`, "anv: enable BTP+BTI RCC keying for some workloads", MR 39982;
  Xe detection `3054192a08a`): performance, not correctness. "On DX12 this is a performance win on all workloads we've
  tested. On DX11 there are a bunch of performance regressions": with the option only render targets stay in the
  binding table, every other resource goes through the bindless heap, and DXVK's shaders paid registers for the pushed
  descriptor-set base offsets. Platforms without LSC never get it. Nothing in the option depends on the API: the
  hardware tags its state cache by binding table pointer + index, so a new binding table cannot hit an old surface
  state, in GL as in D3D12. Zink on ANV uses descriptor buffers (bindless already), so the D3D11 cost does not arise
  (with `ZINK_DESCRIPTORS=lazy` it would). Our kernel sets the bit as Linux xe does (`xe_lrc_init`
  `XE_LRC_DISABLE_STATE_CACHE_PERF_FIX`: an LRI in the ring at queue init, `xe_core.cpp` ~3156), and the GuC-reset
  recovery of `0050` recreates queues through the same `anv_xe_create_engine`, so a replaced queue keeps it.
- **Default since 13 Nov (branch `zinkdef`): `mesa/patches/0108`** puts the option into the `mesa zink` engine entry of
  `00-anv-defaults.conf` (it only reaches zink's Vulkan instances, not the Metal plugin's or other Vulkan
  applications'). Opt-out: `anv_state_cache_perf_fix=false` in the environment or `/tmp/mesa-options-<process>`
  (`0020`), which wins over driconf.

### 2.2 Mipmap generation (#2)

Zink has no `generate_mipmap`; gallium's `util_gen_mipmap` blits level by level (`u_gen_mipmap.c:105-127`), each through
`vkCmdBlitImage` (`zink_blit.c:333`) after ending the render pass. With source = destination the whole image goes
TRANSFER_READ/TRANSFER_WRITE in GENERAL (`zink_resource.c:3800-3824`) for every level, and each blit is a blorp draw
with #1's flushes and all 3D state dirty after it (`genX_blorp_exec.c:407-460`). Iris-style shader packs ask for
mipmaps of several colortex targets every frame.

### 2.3 Small-BAR buffer writes (#3)

Zink takes a resizable BAR only when CPU-visible VRAM is above 90% of VRAM (`zink_screen.c:3805-3815`); ours is the
1 GB window. Then every buffer of 8 KiB or more gets `PIPE_RESOURCE_FLAG_DONT_MAP_DIRECTLY` (`zink_resource.c:2068`),
whatever its placement: also STREAM/COHERENT buffers that live in host memory, where the "do not evict VRAM" reason
does not apply. The threaded context then turns every non-persistent DISCARD map into a staging upload plus
`tc_resource_copy_region` (`u_threaded_context.c:2747-2754`, `3096-3101`); `glBufferSubData` above 320 B does the same
(`3594`, `zink_resource.c:2912-2919`). The copy is reordered only if the destination was not used yet in the batch
(`zink_context.c:5270-5277`), otherwise it ends the render pass (`zink_synchronization.cpp:161`). On Xe2 the render
queue runs `vkCmdCopyBuffer` as a blorp 3D draw (`anv_private.h:5495`). Windows runs this card with a full resizable
BAR and writes those buffers directly.

`0107` adds `ZINK_BAR_BUFFERS`: `sysmem` drops the flag for buffers placed in host memory (no window cost at all);
`all` drops it for every buffer, as on a resizable-BAR device. EXT_host_image_copy stays off either way: with it zink
adds HOST_TRANSFER usage to images, and ANV disables compression for those (`anv_image.c:282-288`).
Risk of `all`: buffers written by the CPU get mapped, and a mapped BO stays in the 1 GB window
(DESIGN-rebar-window.md); a full window spills new BOs to system memory, the 5 Nov half-speed case. Watch
`debug.intelxe.vram` while it runs. Correction to a first reading: `user/xemac/xemac.c`'s 256 MB
(`FAKE_VRAM_VISIBLE`) is the fake device without the kernel; the kernel reports the BAR window (`xe_core.cpp:5875`).

### 2.4 The rest

- Render pass splits (#4): uploads into used resources and query copies call `zink_batch_no_rp`
  (`zink_synchronization.cpp:~375-385`, `zink_query.c:787`). `ZINK_DEBUG=rp` turns on zink's tracking, which Intel does not
  get by default (`zink_screen.c:3305-3325`).
- Descriptors (#5): auto picks descriptor buffers on ANV (`zink_screen.c:4032-4041`); what that means on Xe2 is in
  DESIGN-anv-descbuf-cost.md (no stalls; no surface-state prefetch, cold descriptors).
- Depth (#6): GL depth formats map straight to `X8_D24`, `D24S8`, `D32` (`zink_screen.c:2056-2073`); every depth image is
  sampled-capable, so HiZ is write-through (`anv_image.c:700-721`); sampling a shadow map needs no resolve on Xe2
  (`anv_image.c:3773-3800`).
- Present (#7): the swapchain image is `I915_FORMAT_MOD_4_TILED` without aux (`anv_nvmtl_surface.c:397-406`); kopper
  only adds the PRESENT_SRC barrier (`zink_kopper.c:1123-1126`).
- Clears: `glClear` before drawing becomes a load-op clear (fast clear, any color at level 0, full area); a clear inside
  a render pass is `vkCmdClearAttachments` (a draw). `INTEL_DEBUG=perf` names every slow clear.

### 2.5 cglzink's IOSurface copy-back

Native applications only (Minecraft is not affected). `CGLZINK_SURF_WRITEBACK` (in `libcglzink.dylib`, read at load;
since 13 Nov, branch `zinkdef`, unset means `dirty`, and `flush` asks for the copy at every flush as before):

- `dirty` (default): a copy is made only when the context drew into a framebuffer that holds an IOSurface texture, or uploaded a
  texture, since the last copy. The entries that write pixels (draws, clears, blits, `glEnd`, display lists, raster
  operations, texture uploads) go through generated marking wrappers (`gen/gen.py`, `CGLT_MARK`), and
  `glBindFramebuffer` keeps "is a surface framebuffer bound". A framebuffer name stays listed after a delete; more than
  eight force a copy at every flush. Same pixels as before in `test/run.sh` (and a new "iosurface redraw" case).
- `flush`: the copy at every `glFlush`, `glFinish` and present, whatever was drawn (the behaviour before 13 Nov).
- `present` (opt-in; with `dirty` unless `flush` is also given): no copy at `glFlush`, only at `glFinish` and present. A reader that syncs on `glFlush` (another process
  handed the surface) sees the previous frame: for applications that only show their IOSurfaces themselves.
- Not done: skipping surfaces that `IOSurfaceIsInUse` reports unused. The use count follows WindowServer's display,
  not a reader in another process (or in-process Apple GL contexts, which stay on Apple's GL), so it would show stale
  pixels exactly where sharing matters.

## 3. Built (13 Nov, untested on the B580)

| Where | What |
|---|---|
| `mesa/patches/0107` | zink: `ZINK_BAR_BUFFERS=sysmem|all` (on the zink tree, after `0073`); `~/hackintosh/work/zinkgap-mesa`, branch `zinkgap` |
| `zink-gl/cgl` | `CGLZINK_SURF_WRITEBACK=dirty|present`; `gen/gen.py` emits marking wrappers; `test/layertest.m` "iosurface redraw" |
| `zink-gl/bench/zgbench.c` | the workload (section 5) |
| `zink-gl/bench/zinkgap-kit.sh` | the A/B kit, runs on the B580 |
| `zink-gl/bench/stage.sh` | builds the kit directory: the zink build under test, the package's GLFW, the layer, zgbench |
| `mesa/patches/0108` (13 Nov, `zinkdef`) | ANV: `anv_state_cache_perf_fix` on for the engine `mesa zink`; built from the released source + `0102` in `~/hackintosh/work/zinkdef-anv`, staged as `lib/anv` (`stage.sh ZG_ANV=`) |

The unmodified build of the zink tree reproduces the released `libgallium` byte for byte before signing, and the staged
`libEGL.1.dylib` equals the released one; the staged `libgallium` differs only by `0107`, which does nothing unless
`ZINK_BAR_BUFFERS` is set. `anv_state_cache_perf_fix`, `ZINK_DEBUG=rp` and `ZINK_DESCRIPTORS=lazy` need no build: the
installed package reads them from the environment or from `/tmp/mesa-options-java` (`0020`).

## 4. Not done yet (after the measurements)

- A zink `generate_mipmap`: one compute dispatch for the whole chain (single-pass downsample) or per-level barriers on
  the level's subresource only, so a chain is one drain instead of one per level. Worth it only if the kit's `def`
  against `nomip` gap is large and `rtfix` does not close it.
- ~~If `rtfix` wins and stays correct: make it the default for zink~~ done: `mesa/patches/0108` (section 2.1).
- If `barsys` wins: upstream-style change, the flag only for device-local buffers (it is a fix, not a tuning knob).

## 5. The kit

`zgbench` draws a deferred-renderer-shaped frame (not a game): uploads into a 16 MiB instance arena (`-u subdata`,
`mapinv`, `orphan`, `persist`, `none`) and two uniform buffers (4 and 16 KiB), a depth-only shadow pass (`-S`, `-d 24|32f|24s8`),
a G-buffer of three targets, `-p` full-screen composite passes (ping-pong RGBA16F, each sampling the G-buffer, the shadow
map with compare and the previous pass), with `-m 1` a mip chain of one of them read at level 3, then the default
framebuffer and present (GLFW window, swap interval 0; `-e` EGL pbuffer; `-a` Apple's GL through CGL, also through
`libcglzink.dylib`; `-a -i` the final pass into an IOSurface with `-f` mid-frame `glFlush`). It prints fps, CPU and GPU
ms per frame (`GL_TIME_ELAPSED`, read 3 frames late; `-q 2` per pass) and an image hash of a fixed frame rendered from
cleared history, which a knob must not change.

`zinkgap-kit.sh` (on the B580, from the staged directory; every program under the bash `tmo` timer, knobs exported in a
subshell, never `perl`/`env`/`sh`):

| Command | Time | What |
|---|---|---|
| `check` | < 60 s | stage hashes, Vulkan loader, a pbuffer and a window run, which ANV zink gets, whether `INTEL_DEBUG` reaches ANV in a window |
| `ab1 CONFIG [WORKLOAD]` | < 60 s | one run, appended to `run-<tag>/results.tsv` |
| `repeat CONFIG WORKLOAD [N]` | N x < 60 s | N such runs: an image hash that must not vary |
| `diag [CONFIG]` | 3 runs, < 60 s each | `INTEL_DEBUG=perf` warnings, `INTEL_DEBUG=pc` PIPE_CONTROLs per frame (CS stalls, RT-BTI changes, RESOURCE_BARRIERs, end-of-pipe syncs, by reason), `INTEL_MEASURE=draw` GPU busy and idle time per event type |
| `ab [ROUNDS]` | ~5 min a round | the matrix below, rounds interleaved, then `table` |
| `cgl` | ~1.5 min | Apple's GL vs zink with `CGLZINK_SURF_WRITEBACK` unset (= dirty)/flush/present/flush,present; `layertest` pixels through real zink |
| `mc CONFIG|off` | - | adds a config's knobs to `/tmp/mesa-options-java` for the next Minecraft start (with `tools/b580/zink_ab.sh arm`/`sample`) |
| `table` | - | medians per workload and config, % against base, image hash check, failed runs |
| `night` | ~20 min | check, diag base/rtfix/rtoff, ab 2, repeat persist/orphan, table, cgl, expected |
| `expected` | - | the 13 Nov results and what a run should show against them |

Configs: `base`, `lazy` (`ZINK_DESCRIPTORS=lazy`), `rp` (`ZINK_DEBUG=rp`), `barsys`/`barall` (`ZINK_BAR_BUFFERS`),
`rtfix` (`anv_state_cache_perf_fix=true`), `rtoff` (`=false`, the opt-out of `0108`), `combo` (barsys + rtfix), and references that are not fixes: `ref-nofc`,
`ref-noccs`, `ref-nohiz`, `ref-norb` (`INTEL_DEBUG=no-resource-barrier`). Workloads: `def` (8 passes, mips, 2 MiB of
`glBufferSubData` a frame) with every config; `nomip` (passes and barriers only) with base, lazy, rp, rtfix, rtoff, ref-norb;
`mapinv`, `orphan`, `persist` (4 passes, streaming) with base, barsys, barall.

### Night snippet

On the build Mac, then one detached run on the B580 (the night runner polls `night.log`):

```bash
set -o pipefail
zink-gl/cgl/build.sh x86_64 ~/hackintosh/work/zinkgap-x86
zink-gl/bench/stage.sh ~/hackintosh/work/zinkgap-stage
# with an ANV build for zink (lib/anv, VK_DRIVER_FILES; e.g. the released source + 0108):
#   ZG_ANV=<build>/src/intel/vulkan/libvulkan_intel.dylib zink-gl/bench/stage.sh OUT
rsync -a --delete ~/hackintosh/work/zinkgap-stage/ hk:b580/zinkgap/
ssh hk 'cd ~/b580/zinkgap && shasum -a 256 -c SHA256SUMS | grep -vc ": OK$"'          # 0
ssh hk 'ZG_TAG=night1 nohup bash ~/b580/zinkgap/bin/zinkgap-kit.sh night >/dev/null 2>&1 &'
ssh hk 'tail -3 ~/b580/zinkgap/run-night1/night.log'                                   # poll, each call < 60 s
ssh hk 'bash ~/b580/zinkgap/bin/zinkgap-kit.sh table' ; rsync -a hk:b580/zinkgap/run-night1/ logs/zinkgap-night1/
```

The window runs need the logged-in session (the machine stops at the login window after a reboot); without it,
`ZG_MODE=pbuffer` measures everything but the present. Minecraft (day, with the user): `zink_ab.sh arm <label>`, then
`zinkgap-kit.sh mc rtfix` (or `lazy`, `rp`), `zink_ab.sh launch`, `zink_ab.sh sample <label>`, `zinkgap-kit.sh mc off`.

### Expected outcome

- `rtfix`: the largest gain where passes are many: `def` and `nomip` GPU ms down, `ref-norb` showing that today almost no
  RESOURCE_BARRIER is used (no change), and `diag rtfix` with far fewer `rt-bti-change` and CS-stall PIPE_CONTROLs per
  frame. If the bench image hash or Minecraft's picture changes, it is not usable as is.
- `def` minus `nomip`: the mip chain's cost (#2), and how much of it `rtfix` removes.
- `barsys`/`barall`: GPU ms down in `mapinv` and `orphan` (copies gone), unchanged in `persist` (the application copies
  itself); `barall` must not raise `debug.intelxe.vram` spill.
- `lazy`, `rp`: small either way; they decide whether to follow up (#4, #5).
- `ref-*` bound what compression, fast clears and HiZ are worth today; large losses there mean those paths work.
- `cgl`: `dirty` near Apple's GL for `-f 3` (one copy a frame instead of four), `present` without copies; layertest
  equal to Apple's GL for `flush` and `dirty`.
- Success for the ROADMAP item: Minecraft with BSL within ~2% of Windows' 214-218 fps with the winning knobs, and the
  knobs made defaults only after that.

## 6. Results: B580, 13 Nov 20:55-21:05 (kit `night`, installed ANV without `0108`)

Window 1920x1080 (drawable 2x), 8 s per run, 2 rounds interleaved, medians; 0 GPU resets; images identical by hash
except where noted. The bench runs at ~300 fps and is bound by the CPU and present, so GPU time is the measure; fps
moves by about 1%.

| Workload | Config | fps | GPU ms/frame | fps % | GPU % | Image |
|---|---|---|---|---|---|---|
| def | base | 294.6 | 2.091 | | | |
| def | rtfix | 298.8 | 1.866 | +1.4 | **-10.7** | same |
| def | combo (barsys + rtfix) | 299.2 | 1.848 | +1.6 | **-11.6** | same |
| def | lazy / rp / barsys / barall | 292.8-295.2 | 2.068-2.162 | -0.6..+0.2 | -1.1..+3.4 | same |
| def | ref-nofc / noccs / nohiz / norb | 294.6-296.3 | 2.079-2.104 | 0..+0.6 | -0.6..+0.6 | same (not applied, below) |
| nomip | base | 425.8 | 2.229 | | | |
| nomip | rtfix | 430.3 | 1.420 | +1.1 | **-36.3** | same |
| nomip | lazy / rp / ref-norb | 421.7-426.5 | 2.223-2.248 | -1.0..+0.2 | -0.3..+0.9 | same |
| mapinv | barsys / barall | 289.0 / 288.0 | 1.313 / 1.361 | -0.1 / -0.5 | -0.6 / +3.0 | same |
| orphan | barsys / barall | 277.6 / 278.7 | 2.812 / 2.846 | +0.6 / +1.0 | +2.0 / +3.2 | varied in every config, base too (zgbench, below) |
| persist | barsys / barall | 295.4 / 293.4 | 1.241 / 1.204 | -0.8 / -1.4 | -1.5 / -4.4 | barsys: one of two differed (zgbench race, below) |

libcglzink's IOSurface copy-back (`zgbench -a -i -f 3`: final pass into an IOSurface, 3 mid-frame `glFlush`), all with
the same image hash `f3d461ba`: Apple's GL 1188.6 fps; zink with a copy at every flush 120.0, `dirty` 391.2 (3.3x),
`present` 791.0, `dirty,present` 813.8. `layertest` through real zink: for `flush` and `dirty` only the lines that
always differ from Apple's GL (Apple reads its own IOSurface texels as different from the surface, zink as equal; the
`strings` lines); for `present` also the other IOSurface lines, as designed.

What was made of it (branch `zinkdef`):

- **rtfix is zink's default** (`mesa/patches/0108`, section 2.1): the largest and only clear GPU win, pixels unchanged.
  Opt-out `anv_state_cache_perf_fix=false`; the kit has it as config `rtoff`.
- **`CGLZINK_SURF_WRITEBACK=dirty` is libcglzink's default** (same pixels, 3.3x on the IOSurface path); `flush` brings the
  old behaviour back, `present` stays opt-in because a reader that syncs on `glFlush` then sees the previous frame.
- **barsys/barall stay opt-in and off**: no gain beyond noise (+-3%), and nothing to justify the window cost of `all`.
  The barsys `persist` hash that varied is explained by the bench, not by stale data (next item); the repeat runs of
  the next night confirm it.
- **zgbench was not deterministic** where its uploads stream: (1) `-u persist` reused a third of its persistent
  staging ring without a fence: with frames queued, the CPU overwrote data a queued `glCopyBufferSubData` had not read
  (undefined in GL), and the two hash frames always reused the same third back to back, so whether the hashed region got
  the wrong instances depended on where the arena offset stood when the timed loop ended (about one run in four, in
  any config: barsys only happened to be the one). (2) The upload offset carried over from the timed loop into the hash
  frames, and `-u orphan` draws exactly what it uploaded, so its hash depended on the number of frames run (all
  configs varied, base too). The kit's table compared "varies" with "varies" as `same`. Fixed: a fence per ring third
  (as Sodium does), the hash frames upload from offset 0, and the table says `VARIES` / `base VARIES`.
- **The diag counters and the `ref-*` configs measured nothing in window runs.** The release Metal plugin, which a
  window process loads (and Apple's GL), unsets every inherited `INTEL_*`, `MESA_*`, `ANV_*` variable before ANV reads
  it unless the process sets `NVMTL_PROCESS_ENV=1` (`nullmoth/plugin/nvmtl_env_knobs.h`): `INTEL_DEBUG=pc`/`perf`,
  `INTEL_MEASURE` and `INTEL_DEBUG=nofc/noccs/nohiz/no-resource-barrier` never reached ANV (the `anv_*` driconf names and
  `ZINK_*` were not affected, so the rtfix, bar and zink rows hold). The kit now exports `NVMTL_PROCESS_ENV=1`, forces
  `MESA_LOG=file` and `MESA_LOG_LEVEL=info` for diag, counts RT flushes as well, says so when an instrument printed
  nothing, and `check` probes that `INTEL_DEBUG` reaches ANV in a window. Minecraft is not affected by this:
  `/tmp/mesa-options-java` (`0020`) is not the environment.

Confirmation run (staged: `~/hackintosh/work/zinkdef-stage` = the zinkgap zink build, libcglzink with the `dirty`
default, zgbench with the fixes, and `lib/anv` = the released ANV source + `0102` + `0108`, which the kit hands to zink
through `VK_DRIVER_FILES`): `zinkgap-kit.sh night`, then `zinkgap-kit.sh expected` lists what each row should show
(base now near the old rtfix, `rtoff` near the old base, no `VARIES`, non-zero PIPE_CONTROL counts, `ref-*` now
applied). The staged ANV is built with clang 21, which no longer takes `-fno-split-cold-code`: its CPU submit cost is not
the release's, so compare GPU ms and rows within the staged run, not its fps against the installed ANV.

Minecraft (the user's benchmark: 1.21.1, Sodium, Iris, BSL HIGH, windowed, same scene as the 197-205 fps baseline):
until `0108` is installed, the default is not in the installed ANV, so A/B it through the options file:
`zink_ab.sh arm <label>`, `zinkgap-kit.sh mc rtfix`, `zink_ab.sh launch`, `zink_ab.sh sample rtfix`,
`zinkgap-kit.sh mc off`, then the same without `mc rtfix` as the baseline (the options file reaches the Metal plugin's
ANV in `java` too; `0108` scopes it to zink). Look at the picture: water, shadows and bloom (mip chains), the sky.
After `0108` is installed, `zinkgap-kit.sh mc rtoff` is the A/B in the other direction.
