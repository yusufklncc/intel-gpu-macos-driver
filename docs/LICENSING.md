# Licensing

**This repository is public and noncommercial.** It mixes our own MIT code with nullmoth's code under the PolyForm Noncommercial License 1.0.0. That licence lets anyone use, change and distribute the nullmoth parts (the Metal plugin, NMXAccel, NMXAGDC and IntelXeFB) for any noncommercial purpose, provided every copy carries the licence terms (or their URL) and the `Required Notice:` line; this repository and its release packages do both. Any product that contains the nullmoth parts may therefore be used and shared only for noncommercial purposes; commercial use would need the licensor's permission.

## Per directory

| Path | Licence | Notes |
|---|---|---|
| `kernel/IntelXe/` | MIT | our code. `guc_abi.h` and `lrc.h` contain Linux xe material (MIT, © Intel, full notice in each file). Register tables from xe_pat/xe_mocs/xe_wa/xe_tuning (see NOTICE). |
| `kernel/IntelXe/uapi/` | MIT (`intelxe_drm.h`, ours). `xe_drm.h`, `drm.h`, `drm_mode.h`: Linux/Mesa uAPI headers under their own MIT-style notices | |
| `kernel/IntelXeFB/IntelXeFB.cpp` | **PolyForm Noncommercial 1.0.0** | derived from nullmoth NMIntelFB/NVRMFB; Required Notice in the file and in `kernel/IntelXeFB/LICENSE` |
| `kernel/IntelXeFB/IntelXeLink.cpp`, `IntelXeLink.h` | MIT | `IntelXeLink.cpp` contains i915 display material (intel_cx0_phy.c, intel_ddi_buf_trans.c; MIT, © Intel, full notice in the file) |
| `kernel/IntelXeFB/scripts/` | MIT | |
| `kernel/HybridPM/` | MIT | our code (the Lilu plugin and `hpmstat`). `LICENSE.Lilu` is Lilu's BSD-3-Clause notice: Lilu's `plugin_start.cpp` is compiled into the kext |
| `user/` | MIT | xemac, tests, tools, the Vulkan loader notes (`user/vulkan-loader/`: our notes, script and check program; the loader itself is not here) |
| `nullmoth/` (except `translator/`, `build/`) | **PolyForm Noncommercial 1.0.0** | `nullmoth/LICENSE`, `nullmoth/NOTICE`; modifications listed in `nullmoth/CHANGES.md` |
| `nullmoth/translator/` | **LGPL-3.0-or-later** | metal2vulkan-based; `translator/LICENSE` (LGPL-3.0) and `translator/COPYING` (GPL-3.0) |
| `nullmoth/build/build-intelxe.sh` | MIT | our build script |
| `mesa/` | MIT | patches to Mesa; `xe2-venc/0002`–`0018` by Emil J. Tywoniak. `patches/0010`, `0012`, `0015` contain encoder settings tables from Intel's media-driver (MIT, © Intel; the notice is in the added files); `0040`–`0045` port further media-driver settings into existing Mesa files |
| `videotoolbox/` | MIT (our code); the FFmpeg patches under FFmpeg's licence | the codec plug-ins, scripts and tools. FFmpeg itself is not included. `ffmpeg/configure-shared.sh` records how the shared libraries are configured, and `ffmpeg/*.patch` are our changes to FFmpeg 9.0.2's Vulkan decode and encode code, applied to the shipped `libavcodec` and `libavutil` as `ffmpeg/apply-patches.sh` lists (LGPL-2.1-or-later, as the files they change) |
| `compute/` | MIT (the build scripts); each `patches/<project>/` under the licence of the project it changes | patches to Intel's compute-runtime, IGC, gmmlib and the Level Zero loader (MIT), and to opencl-clang, intel/llvm (DPC++: LLVM, clang, the SYCL runtime, the Unified Runtime, sycl-jit, XPTI) and UMF (Apache-2.0 WITH LLVM-exception), and to oneDNN (Apache-2.0). The projects themselves are fetched at pinned revisions and built on the build Mac, not stored here, as are the libraries the SYCL build adds: parallel-hashmap (Apache-2.0, in libxptifw) and oneDPL (Apache-2.0 WITH LLVM-exception, header-only, compiled into the test program). Nothing built from them is in a release package (the test stages carry them to the test machine only). The SYCL stage's `rtc-sysroot/` holds headers copied from the build Mac's macOS SDK (Apple's SDK licence; libc++'s under Apache-2.0 WITH LLVM-exception) for the run-time SYCL compile on the user's own test machine; it is never published or packaged |
| `tools/report/`, `tools/quality/`, `tools/clbench/` | MIT | diagnostics collectors; encoder rate-distortion and VMAF scripts (they call FFmpeg and libvmaf, which are not included); the OpenCL timing tool |
| `app/` | MIT | the IntelXe Manager app |
| `windows/` | MIT | `IntelXe-EFI.ps1`, the EFI tool for Windows, and its tests |
| `windows/efibuild/` | MIT | the EFI builder (`docs/design/DESIGN-efi-build.md`). OpCore Simplify and Hardware Sniffer (BSD-3-Clause) are fetched at a pinned commit and checked against the manifests in `upstream/`, not stored here; the test fixtures from other repositories are fetched the same way. A build downloads OpenCore (BSD-3-Clause), kexts and ACPICA's `iasl.exe`/`acpidump.exe` pinned in `downloads.lock.json`, each under its own licence, on the user's PC |
| `docs/design/` | MIT | design notes |
| `firmware/` | `fetch.sh` MIT; `LICENSE.xe` and `LICENSE.i915` are Intel's firmware licence texts (linux-firmware `LICENSES/`, tag 20261019) | the blobs are not in the repository |
| `zink-gl/` | MIT (scripts, `src/glshim.c`, `src/egltri.c`, `cgl/`); `src/mesa-zink.diff` under Mesa's licences; the GLFW patch under GLFW's zlib licence | the optional OpenGL-through-zink component; `src/` has the GLFW and Vulkan-Loader licence texts |
| `release/` | MIT | `install.sh`, the install guide and its templates, `package.sh` |
| `tools/parity/`, `data/` | MIT | the parity generators and their data (measurements, CTS results) |
| `efi/`, `docs/`, root files | MIT | |

The `SPDX-License-Identifier` header at the top of each file names its licence. Files without one are third-party files, carrying their own notices.

## Obligations of the built products

| Product | Contains | What it brings |
|---|---|---|
| `IntelXe.kext` | our code, Intel xe material (MIT), **GuC and HuC firmware** (built into the binary) | Keep the MIT notices. The firmware may be redistributed only in binary form, unmodified, with `LICENSE.xe` (Intel's redistribution terms: no reverse engineering, notice kept). From 0.1.5 it also carries XeProbe's GuC images for other families (stage 3): `lnl`/`ptl` from linux-firmware `xe/` (`LICENSE.xe`) and `tgl`/`adlp`/`dg1`/`dg2`/`mtl` from `i915/` (`LICENSE.i915`), pinned in `firmware/probe-guc.sha256`. |
| `IntelXeFB.kext` | IntelXeFB.cpp (PolyForm NC) + IntelXeLink (MIT, Intel material) | Noncommercial use only. Pass on the PolyForm terms or their URL and the Required Notice line. |
| `HybridPM.kext` | our code (MIT) + Lilu's `plugin_start.cpp` (BSD-3-Clause, acidanthera) | Keep the MIT notice and Lilu's BSD-3-Clause notice (in the bundle's `Contents/Resources/LICENSE-Lilu.txt` and the package's `LICENSES/`). It needs Lilu.kext at run time, which is not shipped. |
| `hpmstat` | our code (MIT) | Keep the MIT notice. |
| `IntelXe Manager.app`, `libcglzink.dylib` and `libcglzink-boot.dylib` (optional, Zink package) | our code (MIT) | Keep the MIT notice. |
| `engine\` in the Windows app's zip (the EFI builder beside `IntelXeManager.exe`, `windows/app/build.ps1`) | `windows/efibuild` (MIT); python.org's embeddable **Python 3.12.10** (PSF-2.0, `python\LICENSE.txt`); **pywin32** 311 (PSF-2.0), **WMI** 1.5.1 (MIT), **certifi** (MPL-2.0, unmodified, its licence in its `dist-info`); **OpCore Simplify** and **Hardware Sniffer** at their pinned commits (BSD-3-Clause, `LICENSE` in each tree), unmodified | Keep each notice; the files are shipped as downloaded (pywin32's IDE and help left out). OpenCore, the kexts and ACPICA's tools are not shipped: the builder downloads them on the user's PC. |
| `NMXAccel.kext`, `NMXAGDC.kext`, `NVMTLDriver` (plugin) | nullmoth code (PolyForm NC) | as above |
| `nvmtl-air-dis` (in the plugin bundle, since the nullmoth 2.9 merge) | nullmoth code (`nullmoth/plugin/air_dis.c`, PolyForm NC), built here; it loads the bundle's `air-runtime/libLLVM.dylib` at run time | as above. It replaces the Metal toolchain's `air-opt`, which the plugin used to look for in `/Library/GPUBundles/nvmtl` and which this project never shipped. |
| `libnvmtl_translate.dylib` | translator (LGPL-3.0-or-later) | Provide the source of the translator (including our changes) to anyone who gets the binary, and allow relinking. It is a separate dylib loaded by the plugin, which satisfies that. |
| `air-runtime` (libLLVM, libzstd) | fetched from nullmoth's release package, not built here | LLVM is Apache-2.0 with LLVM exceptions; Zstandard is BSD-3-Clause. Their licence texts ship in the bundle's `Resources/air-runtime`. |
| `libvulkan_intel.dylib` (Mesa ANV) | Mesa (MIT) | Keep Mesa's notices. Some code compiled into it has other licences: Berkeley **SoftFloat** (BSD-3-Clause, `src/util/softfloat.*`), **BSL-1.0** (`src/c11/impl/threads_posix.c`), **BLAKE3** (CC0-1.0 or Apache-2.0, `src/util/blake3/`), and the Khronos Vulkan headers (Apache-2.0 or MIT). Ship their notices with the binary. |
| `libintelxeav1dec.dylib`, `libintelxevenc.dylib` (VideoToolbox codecs) | our code (MIT), linked dynamically against FFmpeg | Keep the MIT notice. |
| `libavcodec.63.dylib`, `libavutil.61.dylib` | **FFmpeg 9.0.2, LGPL-2.1-or-later** (configured without GPL/version3/nonfree parts). `libavcodec` and `libavutil` are **modified**: the patches `videotoolbox/ffmpeg/apply-patches.sh` lists (Vulkan decode and encode fixes, AV1 CDEF under rate control) are applied | Shipped as separate shared libraries that can be replaced. Whoever passes them on must offer the corresponding source: FFmpeg 9.0.2, the patches in `videotoolbox/ffmpeg/` and the configure line (`videotoolbox/ffmpeg/configure-shared.sh`), mark the library as modified, and include the LGPL text. |
| `libvulkan.1.4.363.dylib`, `vulkaninfo` (optional, `user/vulkan-loader/`) | Khronos Vulkan-Loader / Vulkan-Tools `vulkan-sdk-1.4.363.0`, unmodified, **Apache-2.0** | Include the Apache-2.0 licence and the projects' NOTICE files. Not built or shipped from this repository. |

## Build dependencies (not in this repository)

- **MacKernelSDK** (acidanthera, **APSL-2.0**): kernel headers and `libkmod.a`, used to build the kexts. Not vendored; `build.py --sdk`, `MACKERNELSDK`, or `external/MacKernelSDK`. The kexts link `libkmod.a` from it.
- **Lilu** (acidanthera, **BSD-3-Clause**): headers and `plugin_start.cpp` for HybridPM. Not vendored; `kernel/HybridPM/build.py --lilu`, `LILU`, or `external/Lilu.kext`.
- **Mesa source tree:** patched with `mesa/` (MIT and the licences above). It also provides the Vulkan headers for the plugin and the tests.
- **FFmpeg 9.0.2 source:** for the VideoToolbox codecs' shared libraries (LGPL-2.1-or-later), patched by `videotoolbox/ffmpeg/apply-patches.sh` and configured by `videotoolbox/ffmpeg/configure-shared.sh`.
- **Vulkan-Loader / Vulkan-Tools** (`vulkan-sdk-1.4.363.0`, Apache-2.0): optional, for ordinary Vulkan apps (`user/vulkan-loader/`).
- **Rust toolchain and crates:** for the translator. The crates' licences are listed in its `Cargo.lock` dependency graph (MIT/Apache-2.0 in the main).
- **linux-firmware:** `bmg_guc_70.bin`, `bmg_huc.bin` (`LICENSE.xe`); XeProbe's `lnl_guc_70.bin`, `ptl_guc_70.bin` (`LICENSE.xe`) and `tgl`/`adlp`/`dg1`/`dg2`/`mtl_guc_70.bin` (`LICENSE.i915`).
- **nullmoth 2.1.0 release package:** only the air-runtime (LLVM/zstd) is taken from it. The decoder program the plugin runs
  beside it (`nvmtl-air-dis`) is built from this repository's source.

## Not part of this repository

- Remote-access tooling of the test setup is not part of the repository and is not referenced by the build.
- No machine secrets: no ACPI tables with serials (e.g. `MSDM.aml`), no keys, no NVRAM dumps. `efi/` holds notes and the source of `SSDT-ARCOFF` only.
- The GuC/HuC firmware, MacKernelSDK, Mesa, FFmpeg and the Vulkan loader are fetched or built separately (see above).
