# Native Vulkan for games on the B580 (research note, 2026-11-12)

> Design note with its date: it records what was found and planned that day, not the current status
> (`docs/ROADMAP.md`, "Native Vulkan for games").

## Question

Applications that speak Vulkan reach the GPU today only through MoltenVK -> Metal -> our Metal driver -> ANV: two
translations deep. ANV already runs on the machine (Zink uses it directly). What would it take to give Wine (DXVK for
D3D11, vkd3d-proton for D3D12), Minecraft's Vulkan renderer and other Vulkan applications ANV directly, as the NVIDIA
fork [HighDelay/nvidia-macos-driver-nvk](https://github.com/HighDelay/nvidia-macos-driver-nvk) does with NVK?

## How the NVIDIA fork does it (from its public documentation)

- **Wine:** a provider under `/Library/GPUBundles/nvmtl/wine`: `lib/libnvk_wine.dylib`, `lib/libnvk_renderer.dylib`, and
  `lib/libMoltenVK.dylib`, an alias to its NVK front end, so Wine's macOS Vulkan driver, which opens MoltenVK by name,
  gets NVK. `DYLD_LIBRARY_PATH` points at that `lib` folder for the game's process only; `VK_ICD_FILENAMES` alone did
  not select the provider in their Wine build. Verified with YAAGL's Wine 11.0-1, 64-bit prefixes only; a repaired
  DXVK 3.1.1 `d3d11.dll`/`dxgi.dll` pair (stock 3.1.1 crashed on unsupported shared textures),
  `dxvk.enableDescriptorBuffer = False` (text was lost with descriptor buffers), vkd3d-proton for D3D12. A helper,
  `bin/nvk-runtime`, installs and restores the DLLs in a prefix. No 32-bit games, no HDR, no cross-process GPU handle
  sharing; no measured gain over MoltenVK yet.
- **Presentation:** Mesa's own Metal WSI (`VK_EXT_metal_surface`, `nvk-metal-wsi.patch`) in its software mode: a
  linear host-visible image copied into the `CAMetalLayer` drawable. The hardware mode needs a `VkImage` bound to a Metal
  texture, which NVK lacks.
- **Minecraft:** Minecraft 26.3 with Java 25 and a small bridge mod (`minecraft-nvk`, Fabric/NeoForge) that points
  the game's Vulkan renderer at NVK's runtime.

## What we already have

- **ANV on macOS, Vulkan 1.4.363**, loaded as an ICD from `/Library/GPUBundles/nvmtl/libvulkan_intel.dylib` (Mesa with
  `mesa/patches`), 217 device extensions on the B580 (`vkcaps`, 12 Nov). Against what DXVK and vkd3d-proton use:
  swapchain, robustness2, transform feedback, extended dynamic state 3, graphics pipeline library, custom border colour,
  depth clip, vertex attribute divisor, maintenance5, descriptor buffer, mutable descriptors, buffer device address,
  timeline semaphores, dynamic rendering, synchronization2, fragment shading rate, mesh shaders, ray tracing pipelines
  and ray queries, device-generated commands, present id/wait (and their 2 variants), swapchain maintenance1, line
  rasterization, conservative rasterization: all present. Core features: geometry and tessellation shaders, BC
  compression, multi-draw indirect, depth bounds, 64-bit floats and integers, and the sparse binding and residency
  features (buffer, 2D and 3D images, aliased; advertised, not yet exercised on our kernel).
- **Missing:** `VK_EXT_memory_priority` (DXVK uses it when present), and `VK_KHR_external_memory_fd` /
  `VK_KHR_external_semaphore_fd` (Linux file descriptors; Wine on macOS does not use them, cross-process sharing would
  need another handle type).
- **Presentation without a copy:** ANV's `VK_EXT_metal_surface` swapchain hands out IOSurface-backed images in VRAM
  (`anv_wsi.c`, `anv_nvmtl_surface.c`, `wsi_device::iosurface`), the path Zink presents through. The NVIDIA fork copies
  every frame instead.
- **The Khronos loader** 1.4.363 in `/usr/local/lib` on the test machine.

## What is missing

1. **A provider for Wine.** A `libMoltenVK.dylib` that Wine's macOS Vulkan driver opens by name: either the Khronos
   loader under that name with an ICD manifest for ANV, or a thin front end that loads ANV and exports
   `vkGetInstanceProcAddr` from its `vk_icdGetInstanceProcAddr`. Per process only (a launcher sets
   `DYLD_LIBRARY_PATH`); Wine's own MoltenVK stays for everything else. **Built the same day (below): the thin front
   end, `vulkan/`; the loader route is `intelxe-vk run --loader`.**
2. **DXVK and vkd3d-proton builds** to ship or point to (their licences allow redistribution), and a helper that
   installs them into a Wine prefix and restores the originals. **Built the same day: `intelxe-vk fetch`,
   `prefix-install`, `prefix-restore` (pinned release builds, not shipped in the package).**
3. **Kernel robustness under D3D workloads.** DXVK and vkd3d-proton allocate, bind and free far more than Zink or the
   Metal path, use sparse binding for D3D12 tiled resources, and keep many queues busy. The open GuC
   `MEMORY_CAT_ERROR` (suspected: an unbind racing a job) matters more here.
4. **Minecraft's Vulkan renderer:** a bridge mod like `minecraft-nvk`, or a loader setting in the launcher; Minecraft 26.3
   and Java 25 are needed.
5. **`VK_EXT_memory_priority`** in ANV (a hint to the kernel's placement; optional).

## How Wine on macOS loads Vulkan (from its source, 12 Nov)

Read in Wine 10.0, 11.0 and master (`7384c8ba`, 12 Nov) and in the Wine 11.20 source; links are to the 11.0 tag.

- **Which library.** `configure.ac` looks for `-lvulkan` first and only without it for `-lMoltenVK`, defining
  `SONAME_LIBVULKAN` to whichever it found ([configure.ac L1929-1939](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/configure.ac#L1929-1939); the same in
  10.0 at L1874 and in master). On macOS `WINE_CHECK_SONAME` takes the leaf name of the library's install name from
  `otool -L` ([aclocal.m4 L72](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/aclocal.m4#L72)), so a build is tied to `libvulkan.1.dylib` (the Khronos
  loader) or to `libMoltenVK.dylib`. `win32u` opens it with `dlopen(SONAME_LIBVULKAN, RTLD_NOW)`, a bare name
  ([win32u/vulkan.c L3005](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/win32u/vulkan.c#L3005); 10.0 L671), which dyld looks up in
  `DYLD_LIBRARY_PATH` before the build's rpaths.
- **Which symbols.** `dlsym` of `vkGetDeviceProcAddr` and `vkGetInstanceProcAddr` only, Vulkan refused if either is
  missing ([L3021-3022](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/win32u/vulkan.c#L3021-3022)); then the global commands through
  `vkGetInstanceProcAddr(NULL, ...)` (L3029-3031; `winevulkan/vulkan.c` L265-267) and everything else through the two
  `ProcAddr` functions (`win32u/vulkan.c` L623 and L799). `winemac.drv` adds one `dlsym`: in 11.0 and master the
  presence of `vkCreateMetalSurfaceEXT` picks `VK_EXT_metal_surface`, else `VK_MVK_macos_surface`
  ([winemac.drv/vulkan.c L137](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/winemac.drv/vulkan.c#L137), mapping at L101-114); in 10.0 both
  `vkCreateMacOSSurfaceMVK` and `vkCreateMetalSurfaceEXT` must resolve or the driver returns
  `STATUS_PROCEDURE_NOT_FOUND` ([10.0 L223-226](https://gitlab.winehq.org/wine/wine/-/blob/wine-10.0/dlls/winemac.drv/vulkan.c#L223-226)), and it then calls the
  `dlsym`'d `vkCreateMetalSurfaceEXT` itself. **No MoltenVK private entry point** (`vkGetMTLDeviceMVK`,
  `vkSetMTLTextureMVK`, `vkGetMoltenVKConfigurationMVK`, ...) is used anywhere in `win32u`, `winevulkan` or
  `winemac.drv`; `make_vulkan` lists `VK_EXT_metal_surface`, `VK_MVK_macos_surface` and `VK_EXT_metal_objects` only as
  host-side or unexposed extensions (L110, L132, L139).
- **Surfaces.** `VK_EXT_metal_surface` with a `CAMetalLayer`
  ([L58-66](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/winemac.drv/vulkan.c#L58-66)); the `VK_MVK_macos_surface` fallback passes the same
  layer as `pView` (L70-76). The layer is the backing layer of a `WineMetalView`: `framebufferOnly = YES`, nearest
  magnification, opaque black background, its `device` from `MTLCreateSystemDefaultDevice()`
  ([cocoa_window.m L980-990, L3929](https://gitlab.winehq.org/wine/wine/-/blob/wine-11.0/dlls/winemac.drv/cocoa_window.m#L980-990)), so the Metal driver is
  loaded into every Vulkan game's process. master adds an offscreen `CAMetalLayer` in a `CAContext` for windows of
  other processes (`window.c` `macdrv_client_surface_acquire_metal_swapchain`); a game's own window keeps the view.
  ANV's WSI shows its IOSurfaces in a sublayer of whatever `CAMetalLayer` it is given and never uses the layer's
  device or drawables, so neither matters to it.
- **Swapchains.** `win32u` asks for `VkSwapchainPresentScalingCreateInfoEXT` (stretch) when the image size differs from
  the client rectangle and the host has `VK_EXT_surface_maintenance1` and `VK_KHR_swapchain_maintenance1`, because
  "MoltenVK's vkQueuePresentKHR returns VK_SUBOPTIMAL_KHR" otherwise (L1816-1828). ANV has both extensions.
- **32-bit games (WoW64).** Mapped memory must lie below 4 GB: `VK_EXT_map_memory_placed` with `VK_KHR_map_memory2`,
  else `VK_EXT_external_memory_host` (`win32u/vulkan.c` L508-539). ANV offers them when the kernel interface reports
  `has_mmap_offset` / `has_userptr_uapi` (`anv_physical_device.c` L592, L614); `vkcaps` on the B580 says which.
- **Public builds.** Gcenx's builds (the Homebrew `wine-stable`/`wine-devel` casks) come from his MacPorts overlay,
  whose `wine-devel` (11.21) depends on `vulkan-loader` and runs MoltenVK through it, with a patch that turns on
  `VK_KHR_portability_enumeration` ([macports-wine emulators/wine-devel](https://github.com/Gcenx/macports-wine/tree/master/emulators/wine-devel));
  the Wine Devel 11.20 build at hand (inspected with `otool`/`grep`, not run) has `libvulkan.1.dylib` as the name in
  `win32u.so`, rpath `@loader_path/../../`, the loader 1.4.350 in `Contents/Resources/wine/lib` and
  `share/vulkan/icd.d/MoltenVK_icd.json` (portability driver). The NVIDIA fork's YAAGL Wine 11.0-1 opens
  `libMoltenVK.dylib` (its notes). **CrossOver: not checked** (closed build; its sources are published per release,
  not read here); `intelxe-vk check /Applications/CrossOver.app` reads the name from its `win32u.so`.
- **The Khronos loader** exports `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `vkCreateMetalSurfaceEXT` and
  `vkCreateMacOSSurfaceMVK` (`nm` of 1.4.350 and our 1.4.363), so it satisfies Wine 10 and 11 under either name. ANV
  exports only `vk_icdGetInstanceProcAddr`, `vk_icdGetPhysicalDeviceProcAddr` and
  `vk_icdNegotiateLoaderICDInterfaceVersion`: Wine cannot open ANV itself.
- **LWJGL (Minecraft):** on macOS it opens `org.lwjgl.vulkan.libname` if set, else its bundled MoltenVK, else
  `libvulkan.1.dylib`, and needs only `vkGetInstanceProcAddr` from it
  ([VK.java](https://github.com/LWJGL/lwjgl3/blob/master/modules/lwjgl/vulkan/src/main/java/org/lwjgl/vulkan/VK.java),
  `create()` and `GlobalCommands`).

## What was built (12 Nov; untested on the B580)

- **`vulkan/src/intelxe_vk.m` -> `libintelxe-vk.dylib`**, a thin front end rather than the loader under another name:
  it serves both kinds of Wine build (installed as `libMoltenVK.dylib` and `libvulkan.1.dylib` in
  `/Library/IntelXe/Vulkan/provider`), cannot pick up another driver's manifest (MoltenVK's, which a loader in the same
  process would also enumerate), and does no work per call. It exports exactly what Wine resolves:
  `vkGetInstanceProcAddr`, `vkGetDeviceProcAddr`, `vkCreateMetalSurfaceEXT`, `vkCreateMacOSSurfaceMVK`, plus the global
  commands and `vkDestroySurfaceKHR`. It answers the loader-level parts a single driver needs: lists and accepts
  `VK_KHR_portability_enumeration` (drops it and its flag, as the loader does for a full driver, so Gcenx's patch is
  harmless), lists `VK_MVK_macos_surface` and turns it into `VK_EXT_metal_surface` (a `CAMetalLayer`, or an `NSView`
  that gets one, as MoltenVK does). It loads ANV from `/Library/GPUBundles/nvmtl`, or the ANV image already in the
  process (the Metal plugin's, which may be the system-volume copy) so one process does not run two copies of the
  driver; `INTELXE_VK_ICD` overrides, `INTELXE_VK_DEBUG=1` logs. Device functions are ANV's own.
- **The loader route** stays available as `intelxe-vk run --loader`: the Khronos loader from zink-gl linked as
  `libMoltenVK.dylib`, `VK_DRIVER_FILES` = ANV's manifest (Gcenx's builds can also keep their own bundled loader with
  just `VK_DRIVER_FILES`). Useful for layers and as a comparison.
- **Where:** `/Library/IntelXe/Vulkan` (next to zink-gl's `/Library/IntelXe/GL`), not `/Library/GPUBundles/nvmtl`:
  the driver's `install.sh` replaces that folder whole on `--apply` and `--undo`, which would drop or resurrect the
  provider behind the user's back. Its own `install.sh` (`--undo`, `--status`, `--test`) and an optional
  `-vulkan.zip` from `release/package.sh` (`VULKAN_DIR`).
- **`vulkan/intelxe-vk`:** `check`, `run [--loader]`, `fetch` (DXVK 3.1.1 and vkd3d-proton 3.1, sha256 pinned to the
  digests GitHub lists; vkd3d-proton is `.tar.zst`, which macOS's `tar` cannot read without `zstd`),
  `prefix-install` / `prefix-restore` (DLLs into `system32`/`syswow64` and `DllOverrides` = native, the prefix's own
  kept; 64-bit prefixes), `minecraft`.
- **`dxvk.conf` decision:** `dxvk.enableDescriptorBuffer = False`. DXVK 3.1.1's `Auto` already disables descriptor
  buffers on Intel's Mesa driver when `minSubgroupSize` is 16, i.e. on Xe2 (`dxvk_device_info.cpp`, the
  `VK_DRIVER_ID_INTEL_OPEN_SOURCE_MESA` case, "Does not affect Battlemage, and EDB generally costs perf on Intel");
  writing it out keeps that across DXVK versions, and the NVIDIA fork lost text with descriptor buffers on.
  `dxvk.enableDescriptorHeap = False` too for the first runs: 3.1.1 prefers `VK_EXT_descriptor_heap` on Intel's Mesa
  driver and ANV offers it, but that path has not run on our kernel; classic descriptor sets first, the heap as an A/B
  once a game runs.
- **`user/tests/vulkan/vksurf.m`:** a window set up as Wine's `WineMetalView`, a swapchain cleared to a new colour each
  frame, a resize halfway (must yield SUBOPTIMAL/OUT_OF_DATE and a new swapchain), present timings, and a read-back
  of the IOSurface ANV's WSI shows; `--icd` (ANV directly), `--lib NAME` (as Wine opens it), `--wine10`, `--mvk`.
- **Verified on the build Mac only:** builds (x86_64, ad-hoc signed), `nm` shows the exports above and nothing of
  MoltenVK's private API, `vulkan/test/run.sh` (the provider opened by both names through `DYLD_LIBRARY_PATH` against a
  stand-in ICD, under Rosetta: extension lists, the MVK and portability rewriting, surfaces, `vkGetDeviceProcAddr`)
  passes; the helper against a fake prefix and a stand-in `wine reg`; the package zip and its install/undo against
  redirected paths. Nothing has run on the B580 or with Wine.

## Night test on the test machine (12-13 Nov)

From the unpacked `-vulkan` package folder (or `vulkan/build.sh OUT` output copied over); each step is short.

1. `./install.sh --test`-equivalent without installing: `/usr/bin/perl -e 'alarm 30; exec @ARGV' ./bin/vksurf --frames 300`
   (ANV directly) -> `device: Intel(R) Arc(tm) B580 Graphics ...`, `swapchain 800x600 format 44 ...`, a
   `(recreated)` line after the resize, `shown IOSurface ... match`, fps near the display's refresh rate (FIFO),
   `PASS`.
2. `sudo ./install.sh` then `./install.sh --test` -> the same through `DYLD_LIBRARY_PATH` and the name
   `libMoltenVK.dylib`, with `provider: intelxe-vk-provider 1 ...`, `provider loads ANV from
   /Library/GPUBundles/nvmtl/libvulkan_intel.dylib` (or the system-volume copy if the Metal plugin loaded first), `PASS`.
3. `/usr/bin/perl -e '$ENV{DYLD_LIBRARY_PATH}="/Library/IntelXe/Vulkan/provider"; alarm 30; exec @ARGV'
   /Library/IntelXe/Vulkan/bin/vksurf --lib libvulkan.1.dylib --mvk --frames 120` -> `VK_MVK_macos_surface yes`,
   `surface created ... (vkCreateMacOSSurfaceMVK)`, `PASS`.
4. `... vksurf --frames 300 --mailbox` and `--immediate` -> PASS; fps above the refresh rate for those two (if ANV offers them; otherwise vksurf says so and uses FIFO).
5. Loader: `/usr/bin/perl -e 'alarm 30; exec @ARGV' env VK_DRIVER_FILES=/Library/IntelXe/Vulkan/icd.d/intel_icd.x86_64.json
   /Library/IntelXe/Vulkan/bin/vksurf --lib /usr/local/lib/libvulkan.1.dylib --wine10 --frames 120` -> both surface
   functions `exported`, `PASS`.
6. `/Library/IntelXe/Vulkan/bin/intelxe-vk check` -> provider, ANV, loader, `AMFI boot-arg: set`.
7. `sysctl debug.intelxe.stats` before and after: no new resets or faults.

Step 2 decides whether the Metal plugin, loaded by the layer's `MTLCreateSystemDefaultDevice()` as in Wine, and ANV
under the provider coexist in one process. With Wine (later, the maintainer's Wine build and games):
`intelxe-vk fetch` (or `fetch --from` the two tarballs), `intelxe-vk prefix-install PREFIX`,
`WINEPREFIX=PREFIX INTELXE_VK_DEBUG=1 DXVK_HUD=devinfo,fps intelxe-vk run wine game.exe`; DXVK's HUD must name the
B580 and "Intel open-source Mesa driver".

## Test plan

1. A native Vulkan sample through the loader (`VK_DRIVER_FILES` with ANV's manifest): vkcube or vkmark on a
   `CAMetalLayer`; checks the swapchain outside Zink.
2. Wine 11 (64-bit prefix, a public macOS build) with our `libMoltenVK.dylib` provider and DXVK on a D3D11 benchmark,
   then vkd3d-proton on a D3D12 sample. Compare against the same Wine with MoltenVK (frame rate, correctness), and watch
   `debug.intelxe.stats` for resets and faults.
3. Minecraft's Vulkan renderer once 1-2 hold.
4. Sparse residency images through Vulkan on our kernel (a small test), which also decides how much of Metal's sparse
   textures (`docs/ROADMAP.md`, Metal API coverage) the plugin can build on ANV.

Steps 2 and 3 need a Wine build and games chosen by the maintainer; they are not on the test machine.
