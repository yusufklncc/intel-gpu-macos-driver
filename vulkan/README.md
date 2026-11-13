# vulkan: native Vulkan for games on the Intel Arc card (experimental)

An optional component, **not yet tested with Wine on the hardware**. Wine's DXVK (Direct3D 8-11) and vkd3d-proton
(Direct3D 12), and other programs that open a Vulkan library by name, reach the driver's ANV directly instead of
MoltenVK -> Metal -> the Metal driver -> ANV. Nothing loads it unless a program is started through `intelxe-vk run`;
Wine's own MoltenVK, the Khronos loader, ANV and the Metal driver are not touched. It needs the driver installed.
Background and the Wine source it follows: [`docs/design/DESIGN-native-vulkan.md`](../docs/design/DESIGN-native-vulkan.md).

| Command | What it does |
|---|---|
| `sudo ./install.sh` | `/Library/IntelXe/Vulkan`: the provider, the helper, `vksurf`; backs up what it replaces to `./previous/` |
| `./install.sh --test` | `vksurf` through the provider: a window, 120 cleared frames, PASS/FAIL (about 3 s) |
| `vkscene [--lib NAME]` | a renderer-path sample: MSAA, depth, bindless textures, timeline semaphores; timings, a read-back checksum, PASS/FAIL |
| `sudo ./install.sh --undo` | the state before the install |
| `intelxe-vk check [Wine.app]` | what is installed; which Vulkan library a Wine build opens and how to start it; the prefixes set up |
| `intelxe-vk fetch` | downloads DXVK 3.1.1 and vkd3d-proton 3.1 (pinned, sha256 checked) into `~/Library/Application Support/IntelXe/vulkan`; `fetch --from FILE` takes a tarball you have |
| `intelxe-vk prefix-install PREFIX` | their DLLs into a 64-bit Wine prefix, `PREFIX/dxvk.conf`, DLL overrides; the prefix's own DLLs and overrides kept |
| `intelxe-vk prefix-restore PREFIX` | Wine's own Direct3D again, as before `prefix-install` |
| `WINEPREFIX=PREFIX intelxe-vk run wine game.exe` | the game on ANV; `run --loader` uses the Khronos loader instead of the provider |
| `intelxe-vk minecraft-args` | the JVM argument for Minecraft's Vulkan renderer (official launcher, Prism); checks the Java runtimes |
| `intelxe-vk bench --wine W --exe E` | a D3D benchmark under Wine, DXVK on ANV vs on MoltenVK (same front end), runs interleaved, medians |
| `INTELXE_VK_FRAMELOG=F`, `intelxe-vk frametimes F` | the provider logs present intervals (no layer); the summary: fps, p50/p95/p99, 1% low |

`intelxe-vk` is `/Library/IntelXe/Vulkan/bin/intelxe-vk` after the install. Nothing is downloaded unless you run
`fetch`; nothing in a prefix changes unless you run `prefix-install`.

## How it works

Wine's macOS Vulkan driver opens one library by the name its build was configured with (`libMoltenVK.dylib`, or
`libvulkan.1.dylib` in builds against the Khronos loader such as Gcenx's) and takes `vkGetInstanceProcAddr`,
`vkGetDeviceProcAddr` and `vkCreateMetalSurfaceEXT` from it (Wine 10 also `vkCreateMacOSSurfaceMVK`); it uses no
MoltenVK-only function. `intelxe-vk check` reads which name a build uses. ANV exports only the driver interface
(`vk_icdGetInstanceProcAddr`), so `libintelxe-vk.dylib` supplies those names and forwards to ANV; device functions go to
ANV directly, at no cost per call. `intelxe-vk run` puts `/Library/IntelXe/Vulkan/provider` (the provider under both
names) first in `DYLD_LIBRARY_PATH` for that one process and the Wine processes it starts, and sets `VK_DRIVER_FILES` to
ANV's manifest. Frames are presented through ANV's `VK_EXT_metal_surface` swapchain into the window Wine creates, with
IOSurfaces in video memory (no copy per frame).

## vkscene: a renderer path, not only a swapchain

`bin/vkscene` (`user/tests/vulkan/vkscene.m`, shaders in `vkscene/`) drives the driver the way an engine does: a check
pass into a 256x256 target verified pixel by pixel (64 cells, each texel (0,0) of `texture[cell]` through a bindless
array with `nonuniformEXT`, half of the descriptors written after the command buffer was recorded (update after bind);
a far layer that must lose the depth test, a near triangle that must win it, and its edge, which the 4x MSAA resolve
must blend), a GPU wait on a host-signalled timeline semaphore, then N frames of 256 instanced, textured, lit cubes
(vertex/index buffers, a dynamic storage buffer per frame in flight, dynamic rendering, synchronization2, frame pacing
by a timeline semaphore instead of fences, GPU timestamps), and a last frame read back: an FNV-1a checksum that must
be the same on every path to the same driver (`--icd` ANV directly, `--lib libvulkan.1.dylib` through the provider,
`--lib /usr/local/lib/libvulkan.1.dylib` through the loader), compared pixel for pixel with the IOSurface ANV's WSI
shows. `--immediate`/`--mailbox`, `--resize`, `--samples N`, `--grid G`, `--mtl-device` (a Wine-like layer),
`--png FILE`. Ends with `RESULT vkscene fps=... p99_ms=... gpu_ms=... checksum=... check=PASS`. Verified on the build
Mac against MoltenVK (arm64 build, Apple M1 Pro): all checks pass, directly and through the provider.

## Minecraft (Java 26.x, the Vulkan renderer)

One JVM argument, no mod: `intelxe-vk minecraft-args` prints it
(`-Dorg.lwjgl.vulkan.libname=/Library/IntelXe/Vulkan/lib/libintelxe-vk.dylib`) and where it goes in the official
launcher and Prism. Minecraft initialises Vulkan with LWJGL's `VK.create()`, which opens the file the property names
(no MoltenVK fallback when it is set), and hands that file to its window layer: GLFW (26.2, LWJGL's
`GLFWVulkan.setPath`) or SDL3 (26.3, `SDL_Vulkan_LoadLibrary` with LWJGL's path). So instance, device and window
surface all reach ANV. Details and how this was established: the design note. `--java PATH` (or the runtimes it
finds) checks that a Java runtime is x86_64 and may load an ad-hoc signed library (Minecraft's own Java 25 carries
`disable-library-validation`). `--framelog FILE` explains how to get `INTELXE_VK_FRAMELOG` into the game's
environment.

## A/B measurements

- **Frame log:** `INTELXE_VK_FRAMELOG=FILE` (`%p` = process id) makes the provider wrap `vkQueuePresentKHR` and write
  one line per present: `frame t_ms interval_ms present_ms result swapchain`. Without the variable the driver's own
  function is handed out, as before. `intelxe-vk frametimes FILE [--skip S]` summarises it. `INTELXE_VK_ICD=` a
  MoltenVK (under another leaf name than `libMoltenVK.dylib`: dyld looks leaf names up in `DYLD_LIBRARY_PATH` first;
  the provider refuses itself with a message) puts MoltenVK behind the same front end and the same log, so A and B
  differ only in the driver.
- **`intelxe-vk bench`:** `--wine` (wine, its `bin/` or a Wine `.app`), `--exe` and `--prefix` (or `--prefix-mvk` for
  a DXVK build that runs on MoltenVK), `--modes anv,mvk,loader,wine`, `--runs N` (interleaved), `--seconds`,
  `--warmup`, `--score REGEX` (default `avg_fps=`, the RESULT line of `user/tests/wine`'s d3d11bench/d3d12bench),
  `--env NAME=VALUE`. Per run: the program's output, the frame log, Mesa's `MESA_VK_WSI_FPS_LOG` (shown frames per
  second, ANV modes), DXVK's log (the adapter it chose), the environment; `results.tsv`; `bench-report DIR` prints
  medians per mode and the anv/mvk ratios. `--help` lists everything. Tested offline against a stand-in Wine
  (`test/bench-test.sh`); the Wine build and the games are the maintainer's choice.

## Limits

- Not tried with Wine, DXVK or vkd3d-proton on the B580 yet; `vksurf` (V1-V6 passed on the B580, 13 Nov) and the
  offline tests are the checks so far; `vkscene` and Minecraft have not run on the B580.
- 64-bit prefixes. 32-bit games in such a prefix (WoW64) get the 32-bit DLLs too, untested.
- `dxvk.conf` turns DXVK's descriptor buffers and descriptor heap off (the file explains why); edit it to try them.
- `DYLD_LIBRARY_PATH` reaches games started through shell-script launchers only with the AMFI boot-arg, which the
  driver's setup already has.
- Not covered: MoltenVK's own extra functions (`vkGetMTLDeviceMVK` and the like: Wine does not use them), Vulkan
  layers in the provider mode (use `run --loader`), HDR, sharing GPU memory between processes.

## Contents and sources

| File in the package | What | Source |
|---|---|---|
| `lib/libintelxe-vk.dylib` | the provider | `src/intelxe_vk.m` (`./build.sh OUT`) |
| `bin/vksurf` | the swapchain test | `user/tests/vulkan/vksurf.m` (same build) |
| `bin/vkscene` | the renderer-path sample | `user/tests/vulkan/vkscene.m`, shaders `user/tests/vulkan/vkscene/` (SPIR-V checked in as `spv.h`; `gen.sh` rebuilds it with glslang) |
| `icd.d/intel_icd.x86_64.json` | the Khronos loader's manifest for ANV | `user/vulkan-loader/` |
| `intelxe-vk`, `install.sh` | the helper and the installer | here |
| `test/` | the provider's offline test against a stand-in driver (`test/run.sh OUT`), the bench harness test (`test/bench-test.sh`), the night phase (`test/night-vkscene.sh`) | here |

Everything is x86_64 and ad-hoc signed; the build needs Vulkan-Headers 1.4 (`VK_HEADERS`). DXVK (zlib licence) and
vkd3d-proton (LGPL-2.1) are not in the package: `intelxe-vk fetch` downloads their release builds from GitHub. Our files
are MIT.

---

**Türkçe özet:** İsteğe bağlı, **deneysel ve donanımda Wine ile henüz denenmemiş** bir bileşen. Wine'daki DXVK
(Direct3D 8-11) ve vkd3d-proton (Direct3D 12) ile Vulkan kitaplığını adıyla açan diğer programlar, MoltenVK -> Metal ->
Metal sürücüsü -> ANV yerine doğrudan sürücünün ANV'sine gider. Yalnızca `intelxe-vk run` ile başlatılan program
kullanır; Wine'ın kendi MoltenVK'sı, Khronos yükleyicisi, ANV ve Metal sürücüsü değişmez. `sudo ./install.sh` kurar,
`./install.sh --test` bir pencerede 120 kare çizip PASS/FAIL yazar, `--undo` kaldırır. `intelxe-vk check` Wine
sürümünüzün hangi Vulkan kitaplığını açtığını gösterir; `intelxe-vk fetch` DXVK ve vkd3d-proton'u (sabit sürüm,
sha256 denetimli) indirir, `intelxe-vk prefix-install PREFIX` bunları 64 bit bir Wine önekine kurar (önekin kendi
DLL'leri saklanır), `prefix-restore` geri alır. Oyun: `WINEPREFIX=PREFIX intelxe-vk run wine oyun.exe`.
`vkscene` gerçek bir oyun motoru yolunu (MSAA, derinlik, bindless dokular, timeline semaforları) ölçer ve okunan son
karenin sağlama toplamını verir. Minecraft'ın Vulkan çizicisi için tek bir JVM argümanı yeterlidir (mod gerekmez):
`intelxe-vk minecraft-args`. `INTELXE_VK_FRAMELOG=dosya` kare sürelerini katmansız kaydeder (`intelxe-vk frametimes`);
`intelxe-vk bench` bir D3D ölçümünü Wine altında ANV ve MoltenVK üzerinde dönüşümlü çalıştırıp karşılaştırır.
