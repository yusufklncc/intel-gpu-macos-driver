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
| `sudo ./install.sh --undo` | the state before the install |
| `intelxe-vk check [Wine.app]` | what is installed; which Vulkan library a Wine build opens and how to start it; the prefixes set up |
| `intelxe-vk fetch` | downloads DXVK 3.1.1 and vkd3d-proton 3.1 (pinned, sha256 checked) into `~/Library/Application Support/IntelXe/vulkan`; `fetch --from FILE` takes a tarball you have |
| `intelxe-vk prefix-install PREFIX` | their DLLs into a 64-bit Wine prefix, `PREFIX/dxvk.conf`, DLL overrides; the prefix's own DLLs and overrides kept |
| `intelxe-vk prefix-restore PREFIX` | Wine's own Direct3D again, as before `prefix-install` |
| `WINEPREFIX=PREFIX intelxe-vk run wine game.exe` | the game on ANV; `run --loader` uses the Khronos loader instead of the provider |
| `intelxe-vk minecraft` | notes for Minecraft's Vulkan renderer (one JVM argument; not tried yet) |

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

## Limits

- Not tried with Wine, DXVK or vkd3d-proton on the B580 yet; `vksurf` and the provider's offline test are the only
  checks so far.
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
| `icd.d/intel_icd.x86_64.json` | the Khronos loader's manifest for ANV | `user/vulkan-loader/` |
| `intelxe-vk`, `install.sh` | the helper and the installer | here |
| `test/` | the provider's offline test against a stand-in driver (`test/run.sh OUT`) | here |

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
