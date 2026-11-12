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
   `DYLD_LIBRARY_PATH`); Wine's own MoltenVK stays for everything else.
2. **DXVK and vkd3d-proton builds** to ship or point to (their licences allow redistribution), and a helper that
   installs them into a Wine prefix and restores the originals.
3. **Kernel robustness under D3D workloads.** DXVK and vkd3d-proton allocate, bind and free far more than Zink or the
   Metal path, use sparse binding for D3D12 tiled resources, and keep many queues busy. The open GuC
   `MEMORY_CAT_ERROR` (suspected: an unbind racing a job) matters more here.
4. **Minecraft's Vulkan renderer:** a bridge mod like `minecraft-nvk`, or a loader setting in the launcher; Minecraft 26.3
   and Java 25 are needed.
5. **`VK_EXT_memory_priority`** in ANV (a hint to the kernel's placement; optional).

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
