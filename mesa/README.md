# Mesa changes

These are our changes to Mesa's Intel Vulkan driver (ANV), in patch form. They let ANV run on macOS over IntelXe.kext's xe uAPI (xemac). The Mesa tree itself is not part of this repository.

## Quick start: the released build

`intelxe.diff` is the whole difference between upstream Mesa `f01e9b07a91` and the source of the released
`libvulkan_intel.dylib`: the installed prod9 build (`a7021b7e`) with the rename of `patches/0101` on top.
`apply.sh <checkout>` applies it to a checkout of that commit (and creates one empty generated header the diff cannot
carry); `configure.sh` then sets the build up with the released options (ray tracing on, Vulkan Video codecs,
`-Dxemac=user/xemac`), and `ninja -C <build> src/intel/vulkan/libvulkan_intel.dylib` builds it (export
`MESA_GIT_SHA1_OVERRIDE=a3ad934832` for the ninja step as well, as `configure.sh` does for meson). The tables below
split the same changes into reviewable steps. Gaps in the numbering are experiments that are neither in the released
build nor in this repository.

The kernel's uAPI extensions (`intelxe_drm.h`) are taken from `<xemac>/../uapi`, here `user/uapi`, a link to
`kernel/IntelXe/uapi`.

Until 2026-11-05 the stack was called B580 (`B580.kext`, `DRM_IOCTL_B580_*`, `b580_drm.h`); the patches up to `0100` <!-- intelxe-rename: keep -->
keep those names, as they were written. `0101` renames them in the Mesa tree; ioctl numbers and structures are unchanged.

## Bases

| Tree | Base | Contents |
|---|---|---|
| `macos-anv` | upstream Mesa `f01e9b07a91f7e7ab730edfbb383b083f146067b` (main, 2026-10-25, "aco: fix packing negated 16-bit constants…") | `patches/0001`–`0006` give exactly commit `62a38137177` |
| `xe2-venc` | `62a38137177` + `xe2-venc/0001`–`0018` gives exactly commit `2663375102e` | `xe2-venc/0001` is our work-in-progress commit. `0002`–`0018` are Emil J. Tywoniak's Xe2 VDEnc (HEVC encode) commits, cherry-picked from https://github.com/widlarizer/mesa with authorship kept. |

Both chains were checked to reproduce those trees byte for byte.

## Files

| Path | What |
|---|---|
| `patches/0001`–`0006` | the macOS port: futexes on `os_sync_wait_on_address`, `vk_image::drm_format_mod` on macOS, build without expat/mesa-clc/glslang (`-Dintel-internal-shaders=stub`), dynamic lookup, and `-Dxemac=<dir>` (ANV's xe backend calls `xemac_ioctl`/`xemac_mmap` instead of a DRM node) |
| `patches/0007` | `anv_nvmtl_surface.c`: implicit IOSurface synchronization entry points for the Metal plugin, on IntelXe.kext's surface fences. It applies on `62a38137177` or `2663375102e`. |
| `patches/0008` | Vulkan Video decode and encode on by default with xemac (`ANV_NO_VIDEO=1` turns them off). It applies on `2663375102e`, with or without `0007`. |
| `patches/0009` | `util/anon_file`: anonymous files in the per-user temporary directory (`confstr(_CS_DARWIN_USER_TEMP_DIR)`), so sandboxed processes (QuickTime's VTDecoderXPCService) can create them |
| `patches/0010` | H.264 encode on Xe2 (VDEnc AVC state from Intel's media-driver tables) |
| `patches/0011` | xemac: video session memory in CPU-visible VRAM (the engines read row-store/statistics buffers per macroblock; system memory cost 3.4x on H.264 encode) |
| `patches/0012`, `0015` | AV1 encode on Xe2 (VDEnc + AVP, then HuC BRC bitrate control). Plain diffs from the AV1 work tree (`mesa-av1`), not `git am` patches. |
| `patches/0013` | upstream-style fix: index `VkMutableDescriptorTypeCreateInfoEXT` lists by `pBindings` index, not binding number (a sparse layout's mutable bindings were sized for every type; the plugin's pools held a fifth of their sets) |
| `patches/0014` | VRAM-shared IOSurfaces for the Metal plugin: import an IOSurface's B580 VRAM backing (`DRM_IOCTL_INTELXE_SURFACE_VRAM`) as an ANV BO, dirty marking. Plain diff. |
| `patches/0016` | AV1 BRC on Xe2: the HuC also picks the loop filter levels for its q index (media-driver's loop filter back-annotation); the frame header length then varies, so the tile group prefix and its `obu_size` patch are chosen on the GPU. `vk_video`: the header layout size ends after `trailing_one_bit`. Plain diff on `0015`. |
| `patches/0017` | H.264 HuC BRC as a single PAK pass (media-driver `EnableVdencSinglePass`): the kernels were told two passes and the extra post-encode update counted every frame twice (CBR 6M, 1080p60, 300 frames: +17% → +5%). Plain diff on `0016`. |
| `patches/0018` | AV1 encode: quarter-pel motion search (`VDENC_CMD2` SubPelMode was never programmed; media-driver's default is 3). 1080p60 VBR 2/4/8 Mbit/s: +2.1/+2.9/+4.0 dB. Plain diff on `0017`. |
| `patches/0019` | `wsi/metal`, ANV: `VK_EXT_metal_surface` swapchains on IOSurfaces for a driver that does not render through Metal: zero-copy present at submission, IMMEDIATE/MAILBOX/FIFO (`-Dplatforms=macos`) |
| `patches/0020` | `util/os_misc`: a per-process options file (`/tmp/mesa-options-<progname>`) as a fallback to `getenv()`, for processes started by launchers or daemons |
| `patches/0021` | `wsi/metal`, ANV: Tile4 swapchain images in the IOSurface's VRAM (IntelXe.kext `SURFACE_VRAM2`), on top of `0019`, `0020`, `0022`–`0025` |
| `patches/0022` | `anv/measure`: initialize the batch list link in `anv_measure_init()` (`INTEL_MEASURE` with `start=` crashed; platform independent) |
| `patches/0023` | ANV: keep push descriptor data in host memory instead of reading it back from write-combined VRAM |
| `patches/0024` | `wsi/metal`: at most two Core Animation commits per display refresh, kernel calls only for frames that are shown |
| `patches/0025` | Intel: drop `-Dintel-internal-shaders=stub` again (reverts `0004`); internal shaders built with a host mesa-clc (LLVM 23.1.2) |
| `patches/0026` | `anv/xemac`: `VK_MEMORY_UNMAP_RESERVE_BIT_EXT` leaves the range reserved without a hole (needs the placed-map xemac in `user/xemac`) |
| `patches/0040` | H.264 encode: B frames as media-driver programs them on Xe2 (random access, CMD3 cost row, inter rounding) |
| `patches/0041` | AV1 encode: two-reference P pictures are P, not GPB, with their own reference names |
| `patches/0042` | AV1 encode: the HuC BRC picks the CDEF parameters on Xe2 |
| `patches/0043` | H.264 HuC BRC with the Xe2_HPM update tables |
| `patches/0044` | HEVC encode: PAK RDOQ on Xe2, as media-driver |
| `patches/0045` | H.264 HuC BRC also patches the slice states (SLBB layout as media-driver) |
| `patches/0046` | AV1 HuC BRC models a GOP with one B frame between the anchors, as media-driver (needs an encoder that asks for one B frame: codecs-vqa5) |
| `patches/0047` | AV1 HuC BRC runs two PAK passes, as media-driver (the second only when the BRC asks for a re-encode) |
| `patches/0050` | ANV/nvmtl: GPU reset recovery, `nvmtl_anv_device_recover_v1()` replaces a banned exec queue on the same VM (used by the Metal plugin) |
| `patches/0051` | blorp: a compute clear's bounds rectangle after the RGB widening and tile offsets |
| `patches/0071` | util u_queue destroy the finish barrier after every thread left it |
| `patches/0072` | egl zink a macOS EGL platform zink through kopper with VK_EXT_metal_surface (the Zink OpenGL package, not part of `libvulkan_intel.dylib`) |
| `patches/0073` | wsi metal OPAQUE composite alpha marks the IOSurface contents opaque |
| `patches/0074` | anv video Xe2 decode AV1 and HEVC up to 16K |
| `patches/0075` | anv HEVC and AV1 HuC BRC model a dyadic B pyramid per level |
| `patches/0076` | vulkan video H.264 encode slice headers carry ref list modifications and MMCOs |
| `patches/0077` | anv HEVC encode turns P slices into generalized P B before the BRC batch writes the slice header |
| `patches/0078` | anv macOS resolve the optional entrypoints to NULL at link time instead of dynamic lookup |
| `patches/0079` | anv VP9 decode alternates two motion vector buffers instead of copying |
| `patches/0080` | anv Xe2 decodes VP9 up to 16K |
| `patches/0081` | anv Xe2 encodes AV1 up to 8K multi tile frames and multi tile HuC BRC |
| `patches/0082` | anv nvmtl one handle per IOSurface backing shares one anv_bo as PRIME imports do |
| `patches/0083` | anv nvmtl a creator may ask for a Tile4 IOSurface backing when the window server reads layouts |
| `patches/0084` | anv Xe2 H.264 encode quality levels are VDEnc target usage 7 4 1 as media driver |
| `patches/0085` | anv RT set up the BTD FIFO and 3DSTATE_BTD only for devices that enabled ray tracing |
| `patches/0086` | anv RT allocate the ray query BO and the BTD FIFO and emit 3DSTATE_BTD on first use |
| `patches/0087` | anv Xe2 HEVC encode VDENC_PIPE_MODE_SELECT reference prefetch and DW5 as media driver |
| `patches/0089` | anv report fd and host memory handle types only with their extensions |
| `patches/0090` | wsi native images without dma buf export on devices that have none |
| `patches/0091` | anv clear the whole sampler state before packing it |
| `patches/0095` | anv Xe2 HEVC encode TU1 takes TU4 VDENC_CMD2 DW54 |
| `patches/0096` | meson dead strip on darwin as the ld64 form of gc sections |
| `patches/0097` | anv intel mi a batch out of memory is not written through |
| `patches/0098` | anv xemac fd handle types from what the kernel offers at runtime |
| `patches/0099` | anv OPAQUE_FD is not DMA_BUF compatible on a device without dma buf |
| `patches/0100` | anv AV1 video sessions hand out their internal buffers in a few memory bindings |
| `patches/0101` | the rename to IntelXe on top of the released prod9 tree: `intelxe_drm.h`, `DRM_IOCTL_INTELXE_*`, `INTELXE_*`, `struct drm_intelxe_*`, comments; the uAPI header found through the xemac dependency (`<xemac>/../uapi`) instead of a path relative to the Mesa checkout |
| `patches/0102` | `util/disk_cache`: keep the shader cache on macOS when the real user is root (WindowServer, loginwindow after login): Mesa turned it off there, so the compositor compiled every pipeline again after each boot. In 0.1.3: applies on `intelxe.diff` |
| `patches/0103` | ANV: `VK_EXT_memory_priority` as a recorded hint (no kernel driver has a BO priority; upstream ANV does not offer it; `VK_EXT_pageable_device_local_memory` stays off). Platform independent. Applies on `intelxe.diff` + `0102` |
| `patches/0104` | ANV on xe: user-fence completion, `ANV_UFENCE=1` (off by default): each exec with a batch also writes a per-queue sequence number to a CPU-mapped page (`DRM_XE_SYNC_TYPE_USER_FENCE`, as Intel's compute runtime does); CPU fence and timeline waits check and poll it (`ANV_UFENCE_SPIN_US`, default 100) before the syncobj wait. New file `xe/anv_ufence.c`. Applies on `0103` |
| `patches/0105` | ANV, descriptor buffers: no per-set surface state on LSC platforms (Xe2 among them): `update_descriptor_set_surface_state` allocated and filled a 64 B surface state for every new set address, read only by a binding-table entry that LSC platforms never get. CPU only, not built yet; analysis in `docs/design/DESIGN-anv-descbuf-cost.md`. Applies on upstream `f01e9b07a91` and on `intelxe.diff` |
| `patches/0106` | ANV video decode never corrupts its stack or stops a client over DPB slot indices: H.264/H.265 slot indices are keys of 256-entry tables (`dpb_slots`, `dpb_idx`; they were 17 / 16 entries on the stack, written unchecked: FFmpeg 9.0.2's H.265 slot 16 aborted the decoding process in `__stack_chk_fail` on the B580, 13 Nov), so such streams decode as meant; references beyond a codec's table or count (H.264 16, H.265 15: the HuC parameters' size) are left out, H.265 keeps at most 8 active references, a VP9 inter frame with an unbound reference name is not recorded; the command buffer always ends `VK_SUCCESS`. A first version refused such decodes with an error on the command buffer: FFmpeg 9.0.2 then hung on its execution fence (B580, 13 Nov). Platform independent; applies on upstream `f01e9b07a91` and on `intelxe.diff` + `0102`-`0105`. Built, the second version not yet run on the B580 |
| `patches/0107` | zink (the Zink OpenGL package's tree, on `0071`-`0073`): `ZINK_BAR_BUFFERS=sysmem` maps buffers that live in host memory directly instead of through a staging buffer and a GPU copy on a small BAR, `=all` maps every buffer as with a resizable BAR (host image copy stays off). Opt-in, unset changes nothing; `docs/design/DESIGN-zink-gpu-gap.md`. Built (13 Nov), not yet run on the B580 |
| `xe2-venc/` | the 18 commits from `62a38137177` to `2663375102e` (see Bases) |
| `working-tree/macos-anv-62a38137177-worktree.diff` | the uncommitted state of the `macos-anv` tree on 2026-10-29, as a diff against `62a38137177`: `0007`, the descriptor free list in host memory, the query pool in system memory, host-visible video session memory, the memory budget floor and `anon_file` with `geteuid` |
| `working-tree/xe2-venc-2663375102e-worktree.diff` | the uncommitted state of the video tree against `2663375102e`. This was the tree the system-wide `libvulkan_intel.dylib` of 2026-10-29 was built from (decode + HEVC encode, `0007`/`0008` included). |
| `configure.sh`, `darwin-x86_64.ini` | meson setup for an x86_64 macOS ANV-only build |

The `working-tree` diffs overlap with `0007`/`0008` and with each other. They are snapshots, not a series: use one of them, not both, and do not combine them with `0007`/`0008`.

## History: earlier installed libraries

The sections below describe builds that were installed before the released prod9 build (`intelxe.diff`); they are
kept to reproduce older measurements.

### The library of 2026-10-30

`5b2fba33` (installed 04:35): the 2026-10-29 library below, then `0019`, `0020`, `0022`, `0023`, `0024`, `0025`,
`0026`, `0021`, `0040`–`0043`, `0050`, `0044` (that much was `d8860b17`), then `0045`, `0051`, `0046`, `0047`.

To rebuild it (checked: the result is, file for file, the work tree `5b2fba33` was built from):

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git && cd mesa
git checkout -b intelxe f01e9b07a91f7e7ab730edfbb383b083f146067b
git am <repo>/mesa/patches/000[1-6]-*.patch            # = 62a38137177
git am <repo>/mesa/xe2-venc/*.patch                     # = 2663375102e
git apply <repo>/mesa/working-tree/xe2-venc-2663375102e-worktree.diff   # contains 0007 and 0008
for n in 0009 0010 0011 0013 0014 0012 0015 0016 0017 0018 0019 0020 0022 0023 0024 0025 0026 0021 \
         0040 0041 0042 0043 0050 0044 0045 0051 0046 0047; do git apply <repo>/mesa/patches/$n-*.patch; done
```

Then configure with the options of the next section.

### The library of 2026-10-29

`venc2` + sv: the video tree (`xe2-venc` + `working-tree/xe2-venc-2663375102e-worktree.diff`) with `0009`, `0010`, `0011`
applied (that is `venc2`, the base of the installed `libvulkan_intel.dylib`), then `0013` and `0014`, then `0012`,
`0015`, `0016`, `0017` (installed as `mesa-rel-pkg-av1lf`, c5110e4283ba). Same meson options as below.

### Reproducing the earlier video library

```sh
git clone https://gitlab.freedesktop.org/mesa/mesa.git && cd mesa
git checkout -b intelxe f01e9b07a91f7e7ab730edfbb383b083f146067b
git am <repo>/mesa/patches/000[1-6]-*.patch
git am <repo>/mesa/xe2-venc/*.patch
git apply <repo>/mesa/working-tree/xe2-venc-2663375102e-worktree.diff
# configure.sh now sets the released options; for this tree override them (later -D options win). Not re-checked since
# configure.sh moved to the released options.
MESA_SRC=$PWD BUILD=build-rel-video <repo>/mesa/configure.sh -Dplatforms= -Dintel-rt=disabled -Dmesa-clc=auto -Dintel-internal-shaders=stub
ninja -C build-rel-video src/intel/vulkan/libvulkan_intel.dylib
```

## Meson options of the released build

`configure.sh` sets them (the released prod9 build, `a7021b7e`, and its rename):

```
buildtype=release  b_ndebug=true
c_args=cpp_args=-Xclang -fno-split-cold-code
platforms=macos  vulkan-drivers=intel  gallium-drivers=  vulkan-layers=  tools=
opengl=false  gles1=disabled  gles2=disabled  glx=disabled  egl=disabled  gbm=disabled
llvm=disabled  intel-rt=enabled  intel-elk=false
video-codecs=h264dec,h265dec,av1dec,vp9dec,h264enc,h265enc,av1enc
zstd=disabled  valgrind=disabled  libunwind=disabled  lmsensors=disabled  perfetto=false  build-tests=false
xmlconfig=disabled  expat=disabled  spirv-tools=disabled
mesa-clc=system
xemac=<repo>/user/xemac
--cross-file=darwin-x86_64.ini   (clang -arch x86_64 -mmacosx-version-min=14.4)
```

The libraries of 2026-10-29/30 had the same options with `intel-rt=disabled`. The earlier video library had no
`platforms=macos` (the Metal WSI came with `0019`) and used `intel-internal-shaders=stub` instead of
`mesa-clc=system` (since `0025`). With `mesa-clc=system`, ANV's internal shaders are built by a host `mesa_clc` and
`vtn_bindgen2` (LLVM/clang 23.1.2 with SPIRV-LLVM-Translator 23.1.2), which must be on `PATH`.

`-Xclang -fno-split-cold-code` keeps clang from splitting cold code into separate functions. With splitting, the release build's per-submit round trip was about twice as long (221 µs, against 112 µs without it).

On Xe2, upstream ANV exposes video encode for HEVC only; `0010` adds H.264 and `0012`/`0015` AV1.

## Licence

Mesa is MIT (see https://docs.mesa3d.org/license.html). These patches are under the same terms. Some files in the Mesa tree have other licences; see docs/LICENSING.md.
