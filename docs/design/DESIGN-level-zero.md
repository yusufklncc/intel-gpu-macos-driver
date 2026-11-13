# Level Zero / oneAPI (and Intel's OpenCL) on macOS: Intel's compute runtime over IntelXe.kext

Status 12 Nov 2026 (branches `levelzero`, `igc`). Stage 1 done: Intel's compute runtime ("NEO") builds for macOS x86_64
against our kernel interface, with gmmlib and the Level Zero loader (`compute/`). Stage 2 is written and checked as far
as a Mac without the B580 can check it; its first hardware run is the night test. Stage 3b done on the build Mac: Intel's
compiler (IGC, with LLVM 22 and Clang) builds for macOS (`compute/build-igc.sh`, section 4), and NEO compiles SPIR-V and
OpenCL C at run time, compiles its built-in kernels at build time and gets its SIP kernel. Nothing here has run on the
B580 yet. 13 Nov (branch `l0sysman`): **Level Zero sysman** (`zesInit` and the zes* modules) works over the kext: NEO's
Linux sysman runs unchanged against the device's sysfs, answered from the kext (patch 0013, `debug.intelxe.sensors_raw`;
section 10); checked against xemac's canned device, the B580 run is in the night snippet.

## 1. What NEO is, and the shape of the port

[intel/compute-runtime](https://github.com/intel/compute-runtime) (pinned at `248eb54f8441`, 2026-10-30, the tree
`docs/parity` already reads) is one code base with three front ends over one core:

| Directory | What | Port |
|---|---|---|
| `level_zero/` | the Level Zero driver `libze_intel_gpu.so.1`: `core/` (driver, device, context, command lists/queues, modules, kernels, events, USM), `sysman/` and `tools/` (sysman, metrics, debugger), `experimental/` (graphs) | builds unchanged |
| `opencl/` | the OpenCL ICD `libigdrcl.so` | builds unchanged (`OCL=1`) |
| `shared/source/` | device, memory manager, command stream receivers, GPU command encoding per core (`xe2_hpg_core/bmg`), built-ins, compiler interface, page fault manager | builds; 3 small fixes |
| `shared/source/os_interface/linux/` | the DRM backend: `drm_neo.cpp` (class `Drm`: discovery, queries, VM and context creation), `ioctl_helper*.cpp` (i915, i915-prelim, xe), `drm_memory_manager.cpp`, `drm_buffer_object.cpp`, `drm_command_stream.inl` (submission), `os_context_linux.cpp`, `engine_info.cpp`, `memory_info.cpp` | **used as is on macOS** |
| `shared/source/os_interface/linux/xe/ioctl_helper_xe.cpp` | the translation of NEO's abstract `DrmIoctl` to Linux's xe uAPI (`getIoctlRequestValue`, `getFlagsForVmCreate`, user fences, the exec/bind paths) | 2 changes |
| `shared/source/os_interface/windows/` | WDDM (Windows, and WSL's `/dev/dxg`) | not built |

The decisive property: **every call the DRM backend makes to the kernel goes through `NEO::SysCalls`**
(`shared/source/os_interface/linux/sys_calls.h`: `open`, `ioctl`, `mmap`, `munmap`, `readlink`, `opendir`, `poll`,
`pidfd*`, `prctl`, ... implemented in `sys_calls_linux.cpp`), because Intel's unit tests mock them. A macOS
`sys_calls_macos.cpp` that sends the render node to `xemac` makes the whole Linux backend run over IntelXe.kext without
a second backend. This is the same layer Mesa's ANV uses on macOS (`user/xemac`: `xemac_open`/`xemac_ioctl`/
`xemac_mmap` in place of a DRM node, `mesa/patches/0006`), so NEO and ANV share one user-space kernel interface, and
`XEMAC_FAKE=1` (xemac's canned Battlemage) works for NEO too.

Two places bypassed `SysCalls` and were changed: `DrmMemoryManager::mmapFunction`/`munmapFunction` were libc
`mmap`/`munmap` (a BO map must be xemac's IOKit mapping), and the page fault manager listens for `SIGSEGV` only.

## 2. What NEO needs from the kernel, against IntelXe.kext

Every ioctl NEO issues on xe (`IoctlHelperXe::getIoctlRequestValue`, `ioctl_helper_xe.cpp:2309` upstream, plus
`Drm::getDrmVersion`), mapped to `kernel/IntelXe/xe_core.cpp` (`IntelXe::ioctl` dispatch) and the uAPI headers in
`kernel/IntelXe/uapi`. NEO's `third_party/uapi/drm-next/xe/xe_drm.h` and ours are identical apart from comments, so
the BSD-encoded request numbers (`<sys/ioccom.h>`, `IOCPARM_LEN` = the struct size) match what the kext decodes.
The items left unimplemented are those `docs/parity/TODO.md` lists under "OpenCL / oneAPI (Intel compute-runtime)" (24).

| NEO use | ioctl / query | IntelXe.kext |
|---|---|---|
| driver check (`"xe"`) | `DRM_IOCTL_VERSION` | yes (`name = "xe"`) |
| device config, flags, VA bits | `DRM_IOCTL_XE_DEVICE_QUERY` `CONFIG` | yes; `HAS_CPU_ADDR_MIRROR` not reported (no SVM) |
| engines (CCS, BCS, RCS, video) | `QUERY_ENGINES` | yes |
| memory regions, small-BAR detection | `QUERY_MEM_REGIONS` | yes; `cpu_visible_size` = the 1 GB BAR window |
| GT list, IP version (selects the built-ins' `20_1_0`) | `QUERY_GT_LIST` | yes (`ip_ver_rev` 0) |
| slices/DSS/EUs, L3 banks | `QUERY_GT_TOPOLOGY` | yes |
| system info (threads, caches) | `QUERY_HWCONFIG` | yes (the GuC's table) |
| CPU/GPU timestamps (events, `zeDeviceGetGlobalTimestamps`) | `QUERY_ENGINE_CYCLES` | yes; xemac converts the clock |
| EU stall sampling, OA metrics | `QUERY_EU_STALL`, `DRM_IOCTL_XE_OBSERVATION` | **no** (refused): no zet metrics, no EU stall |
| BOs (placement, WB/WC caching, `DEFER_BACKING`, `NO_COMPRESSION`) | `DRM_IOCTL_XE_GEM_CREATE` | yes |
| CPU maps | `DRM_IOCTL_XE_GEM_MMAP_OFFSET` | yes; `FLAG_PCI_BARRIER` not implemented (NEO then flushes without the barrier page: "PCI barrier ptr 0x0") |
| | `DRM_IOCTL_GEM_CLOSE` | yes |
| VMs: `LR_MODE` always, `SCRATCH_PAGE` unless disabled, `FAULT_MODE`+`NO_VM_OVERCOMMIT` when page faults are on | `DRM_IOCTL_XE_VM_CREATE` | `SCRATCH_PAGE` yes; `LR_MODE` **yes since branch `lrmode`** (DESIGN-lr-mode.md; refused before: patch 0006 probes it and falls back to dma-fence-mode VMs on an older kext); `FAULT_MODE` refused (no GPU fault service) |
| binds: `MAP`, `UNMAP`, `MAP_USERPTR` (host USM and `zeMemAllocHost`), `PREFETCH`, `UNMAP_ALL`; flags `IMMEDIATE`, `READONLY`, `DUMPABLE`, `NULL`, `DECOMPRESS`; bind user fences | `DRM_IOCTL_XE_VM_BIND` | yes; `CPU_ADDR_MIRROR` refused (needs a fault-mode VM) |
| memory advice (`zeMemAdvise`, preferred location, atomics, PAT) | `DRM_IOCTL_XE_MADVISE` | preferred location and purgeable state yes; `ATOMIC`, `PAT` refused |
| | `DRM_IOCTL_XE_VM_QUERY_MEM_RANGE_ATTRS` | **no** (refused) |
| contexts: priority, low-latency hint; multi-queue groups (Xe3P) | `DRM_IOCTL_XE_EXEC_QUEUE_CREATE` + `SET_PROPERTY` extension | yes; `MULTI_GROUP`, `MULTI_QUEUE_PRIORITY` refused (not BMG) |
| | `DRM_IOCTL_XE_EXEC_QUEUE_DESTROY` | yes |
| context health (`ZE_RESULT_ERROR_DEVICE_LOST`) | `DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY` `BAN` | yes |
| | `DRM_IOCTL_XE_EXEC_QUEUE_SET_PROPERTY` (after creation) | **no** (refused) |
| submission with user fences | `DRM_IOCTL_XE_EXEC` | yes |
| completion waits | `DRM_IOCTL_XE_WAIT_USER_FENCE` | yes (10 Nov, `user/tests/kernel/ufence.c`) |
| IPC handles, external memory | `DRM_IOCTL_PRIME_HANDLE_TO_FD` / `FD_TO_HANDLE` | yes with `debug.intelxe.fds=1` (`INTELXE_CAP_FD_OBJECTS`) |
| external semaphores | `DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE`, `DESTROY`, `WAIT`, `TIMELINE_WAIT`, `SIGNAL`, `TIMELINE_SIGNAL` | yes |
| GPU debugger (eudebug) | `DRM_XE_EUDEBUG_*`, metadata | no; NEO is built without it (`NEO_ENABLE_XE_EU_DEBUG_SUPPORT` off) |

So the kernel already answers everything NEO needs for allocation, copies and kernels. What it lacks changes
behaviour, not function, except metrics and the debugger:

1. **Long-running mode** (`DRM_XE_VM_CREATE_FLAG_LR_MODE`). NEO creates every VM with it
   (`IoctlHelperXe::getFlagsForVmCreate`). Without it a compute job is under the kernel's job timeout like a graphics
   job, so a kernel that runs for seconds is reset. Linux's LR mode: exec queues without a job timeout, only user
   fences as out-syncs, and memory eviction by preempting the queue (preempt fences, `xe_preempt_fence.c`) instead of
   waiting for jobs. **Done in branch `lrmode` (12 Nov, untested on hardware; DESIGN-lr-mode.md):** the kext accepts
   the flag (FAULT_MODE still refused), refuses syncobj out-syncs on such VMs as xe does, exempts their queues from the
   TDR (GuC and GT resets still ban them), and preempts the VM's queues (GuC schedule disable, then enable) for a BO
   move and before freeing memory the VM mapped. Patches 0006 and 0009 already probe the flag at run time, so NEO now
   creates LR VMs and turns its direct submission (ULLS, on by default for Xe2) on by itself; against an older kext
   the fallback (dma-fence VMs, no ULLS) remains.
2. **Page faults / SVM** (`FAULT_MODE`, `CPU_ADDR_MIRROR`, `VM_QUERY_MEM_RANGE_ATTRS`, `MADVISE` atomics/PAT): system
   allocator USM ("shared system USM") and on-demand migration. NEO reports "Shared System USM NOT allowed" and uses
   its own CPU page fault manager for shared allocations, which works without the kernel.
3. **Small BAR.** The kext offers a 1 GB CPU-visible window of the 12 GB; NEO on xe refuses such a device
   ("Resizable BAR not detected", `isSmallBarConfigAllowed() == false`). The kext maps a BO into the window on CPU
   map and moves it to system memory when it cannot enter (as xe's fault handler does), so patch 0006 allows it on
   macOS. Large host-visible device allocations will be slow, not wrong.
4. **Metrics and EU stall** (`DRM_IOCTL_XE_OBSERVATION`): zet metric groups are empty. Same item as Mesa's
   `intel/perf` in `docs/parity/TODO.md`.
5. **`DRM_IOCTL_XE_EXEC_QUEUE_SET_PROPERTY`** (priority changes after creation) and `MMAP_OFFSET_FLAG_PCI_BARRIER`:
   small, optional.

No kernel change was made for the port itself; long-running VMs came after it (branch `lrmode`, item 1).

## 3. What NEO assumes from Linux, and the macOS replacement

| Linux facility | Where NEO uses it | macOS (this port) |
|---|---|---|
| `/dev/dri/renderD*`, `/dev/dri/by-path` | `Drm::discoverDevices` (`drm_neo.cpp:818`) | `SysCalls::open("/dev/dri/renderD128")` = `xemac_open()` (NMXAccel, else IntelXe directly; `XEMAC_FAKE=1`); `by-path` does not exist, so NEO tries the renderD nodes and finds one |
| DRM ioctls on that fd | everywhere in `os_interface/linux` | `xemac_ioctl()`: the request numbers as `<sys/ioccom.h>` encodes them, absolute timeouts moved to the kext's clock |
| `mmap` of the DRM fd at `GEM_MMAP_OFFSET` | `DrmMemoryManager` | `xemac_mmap()` (IOConnectMapMemory64, write-combined for VRAM; `MAP_FIXED` remaps the IOKit pages at NEO's address); `xemac_munmap()` |
| sysfs: `/sys/dev/char/M:m` → PCI path | `getPciPath()` (bus/device/function for `zeDevicePciGetPropertiesExt`, BDF filters) | synthesized from `xemac_pci_info()` (`../../devices/pci0000:00/0000:00:01.0/0000:bb:dd.f/drm/renderD128`) |
| sysfs: frequencies, tile info, `prelim_*`, eudebug switch, `intel_pmt` telemetry; hwmon; `/proc/<pid>/fd` | `Drm::readSysFsAsString`, sysman, `pmt_util.cpp` | since patch 0013 a read-only tree of the files xe has for the device, answered from the kext (`sysfs_macos.cpp`, section 10): the PCI ids, `tile0/gt0/freq0/*_freq`, hwmon energy, power limits and temperatures, `/proc/<pid>/fd/<fd>` of the render node; the core reads `max_freq` (core clock = RP0). Not there: `intel_pmt`, `prelim_*`, eudebug, the media GT, PCI link files |
| `/proc/self/exe` | AIL (per-application settings), compiler cache | `readlink` answered with `proc_pidpath()` |
| `/proc/cpuinfo`, `/sys/devices/system/cpu` | `CpuInfo` (cache sizes, CPU flags) | absent: CPUID is used, cache topology defaults |
| `/proc/self/maps` | `OSMemoryLinux::getMemoryMaps`, for CPU ranges above 47 bits (`gfx_partition.cpp`) | not reached: x86_64 macOS user space ends at 47 bits; a `mach_vm_region` walk if it ever is |
| `dlinfo(RTLD_DI_LINKMAP)`, `<link.h>`, `RTLD_DEEPBIND` | `OsLibrary::getFullPath`, `isLibraryLoaded` | `SysCalls::dlinfo` builds the list from dyld's images; no deep binding (two-level namespace) |
| `mremap(MREMAP_FIXED)`, `MAP_FIXED_NOREPLACE` | GPU VA / CPU VA alignment of USM | `mach_vm_remap`, `mach_vm_allocate(VM_FLAGS_FIXED)` |
| `SIGSEGV` + `mprotect` | CPU page fault manager (shared USM migration) | `SIGBUS` on macOS (a `PROT_NONE` page is `KERN_PROTECTION_FAILURE`); patch 0007 |
| `pidfd_open`/`pidfd_getfd`, `prctl(PR_SET_PTRACER)`, `SCM_RIGHTS` sockets | IPC handles between processes | pidfd and prctl fail with `ENOSYS`; NEO falls back to its socket server ("opaque handles still supported"); BO fds are the kext's object fds (`/dev/intelxeobj`, `debug.intelxe.fds=1`) |
| dma-buf fds | `zeMemGetIpcHandle`, external memory import | the kext's PRIME object fds; size by `xemac_obj_size` (not `lseek`). Untested with NEO |
| sync files / syncobj fds | external semaphores | the kext's syncobj fds |
| `perf_event_open` | sysman engine utilization, RAS counters | no event source is found; `perfEventOpen` fails with `ENOSYS` (syscall 298 is another call on macOS) |
| udev, libgenl (netlink), MTD | sysman (hotplug, fabric, firmware flash) | built without (NEO's own stubs); the MTD path finds no node |
| `memfd`, `eventfd`, futex, userfaultfd | not used by NEO's runtime path | — |
| `CLOCK_MONOTONIC_RAW` | `OSTimeLinux` | exists on macOS; the kext samples `CLOCK_UPTIME_RAW` and xemac converts |
| ELF shared objects, version scripts, `-Bsymbolic`, `librt` | the build | Mach-O dylibs with an exported-symbols list generated from the version script (`neo_apple_export_symbols`), no `librt` |

## 4. The compiler (IGC) on macOS

NEO compiles OpenCL C and SPIR-V with **IGC**
([intel/intel-graphics-compiler](https://github.com/intel/intel-graphics-compiler), `0f18b8dc` for this NEO: API 2.42),
loaded at run time through CIF: `libigdfcl` (the OpenCL C front end, over opencl-clang) and `libigc` (SPIR-V/LLVM to
Gen ISA, and the source of the **SIP kernel**, the system routine that saves and restores threads for mid-thread
preemption and debugging). Since 12 Nov (branch `igc`) IGC builds for macOS x86_64 with `compute/build-igc.sh`, and
NEO, ocloc and the built-in kernels use it.

### 4.1 What is built, and how

IGC's own build in its "Source" mode: one CMake tree that builds LLVM and Clang with IGC's LLVM patches, then the
dependencies and IGC, all linked statically into three dylibs. Pins (`compute/build-igc.sh`):

| Component | Revision | Why |
|---|---|---|
| IGC | `0f18b8dc2e5d` (2026-10-20, v2.42.0+240 on master) | the revision NEO `248eb54f8441`'s manifest names |
| LLVM + Clang (+ lld as a library) | `llvmorg-22.1.8` (`ca7933e47d3a`), plus IGC's 10 patches in `external/llvm/releases/22.0.0`, which IGC's build applies to its copy | IGC's `DEFAULT_IGC_LLVM_VERSION` |
| opencl-clang | `ocl-open-220` `b953ecee` | IGC v2.42.6's release notes |
| SPIRV-LLVM-Translator | `llvm_release_220` `67b3fc33` (2026-10-15) | v2.42.6 lists `0e672448`, but this IGC calls `TranslatorOpts::setErrorHandlingKind` (on the branch since 2026-09-29): the branch as of IGC's date |
| SPIRV-Tools, SPIRV-Headers | `f8035151`, `575b6512` | v2.42.6's release notes |
| vc-intrinsics | `v0.25.0` (`27f7c4f3`) | v2.42.6's release notes |

The build runs on the build Mac (Apple silicon) and produces x86_64: `CMAKE_OSX_ARCHITECTURES=x86_64`,
`LLVM_HOST_TRIPLE=x86_64-apple-darwin`, and no `CMAKE_SYSTEM_NAME`, so CMake does not set up a cross build and LLVM does
not build a second, native tablegen: the x86_64 build tools (tablegen, the in-tree clang that compiles the OpenCL
built-in library to bitcode, IGC's generators) run under Rosetta. Rosetta was chosen over a native-host/x86_64-target
split because IGC's build runs many of its own tools and has no host/target distinction of its own; over running the
whole build under Rosetta because the compilers then stay native (Homebrew's cmake and python are arm64-only, too).
LLVM's optional dependencies are off (`LLVM_ENABLE_ZSTD/LIBXML2/LIBEDIT=OFF`): LLVM would otherwise find Homebrew's
arm64 libraries and fail at the link; the static analyzer and ARC migrator are left out of the clang it builds.

Result (`$IGC_PREFIX`, installed by `build-igc.sh install`, staged next to NEO by `build-neo.sh`):

| File | Size | What |
|---|---|---|
| `libigc.2.dylib` | 104 MB | the back end, BiF bitcode, SIP kernels; exports 234 symbols (CIF entry points, the `_igc_bif_*` resources) |
| `libigdfcl.2.dylib` | 2.5 MB | the OpenCL C front end (CIF), the OpenCL C header resource; links `libopencl-clang.dylib` |
| `libopencl-clang.dylib` | 140 MB | Clang + the SPIR-V translator; `Compile`, `CheckCompileOptions`, the `PCM_*` precompiled modules |
| `libiga64.2.dylib`, `bin/iga64` | 3 MB | IGA, the Gen ISA (dis)assembler (ocloc `disasm`/`asm`) |

### 4.2 What IGC assumed from Linux (the patches, `compute/patches/igc`, `compute/patches/opencl-clang`)

IGC knows Windows, Linux and Android; macOS is built as Linux (the same POSIX user-mode code: `LINUX`/`ICBE_LINUX`,
`LLVM_ON_UNIX`, dlopen, pthreads) with the ELF and GNU-linker parts replaced. Nothing found needs Linux at run time.

| Patch | Linux-ism | macOS |
|---|---|---|
| igc 0001 | architecture detection knows only Linux/Android/Windows; `--whole-archive`/`--start-group`; `-Bsymbolic`, `-z relro/now/noexecstack`, `--no-undefined`, `--gc-sections`, `--exclude-libs`; a `--version-script` made from `objdump -t` | Darwin = a "Linux" OS, processor from `CMAKE_OSX_ARCHITECTURES`, host = target; `-force_load` for the archives whose members are only reached by `dlsym` (the BiF and OpenCL C header resources); the GNU/ELF flags removed (two-level namespace binds a dylib's own references; dyld's defaults; ld64's default), `-dead_strip`; an `-exported_symbols_list` made from `nm -m` with the same selection |
| igc 0002 | `<link.h>`, `<linux/limits.h>` (LoadBuffer.cpp), `<malloc.h>` (igax.hpp), `/proc/<pid>/cmdline`; `__linux__` branches that are POSIX or compiler (bit scans, `__sync` reference counts, GCC pragmas, visibility, dump paths, the `IGC_*` environment variables); iStdLib's `#error` for other systems | the portable headers; `getprogname()`; those branches taken on macOS too |
| igc 0003 | `igc.opencl.h` names only `.dll`/`.so` | `libigc.2.dylib`, `libigdfcl.2.dylib` |
| igc 0004 | header-only static libraries (GNU `ar` makes empty archives); `-lrt`; deprecations as errors under `-Werror` | one empty object for Apple's `ar`; no librt (libSystem); LLVM 22's deprecations in vc-intrinsics v0.25.0 stay warnings |
| igc 0005 | `size_t` and `uint64_t` are one type (`unsigned long`) | on macOS `uint64_t` is `unsigned long long`: metadata (`MDFrameWork`) and YAML (`OCLRuntimeInfoPrinter`) overloads for `size_t`, `toInt<uint64_t>`, `PRIu64` formats |
| opencl-clang 0001 | version script + `-Bsymbolic` + `--no-undefined`; the PCH resources found by `dlopen(<own soname>)` | an exported-symbols list (without the names the version script lists but this version lacks: ld64 fails on them, GNU ld ignores them); the library opens itself by its `dladdr()` path |

The resources (the BiF bitcode in libigc, the OpenCL C header in libigdfcl, the headers and PCMs in libopencl-clang)
stay C arrays found by `dlsym` as on Linux: on Mach-O they only need to be exported (they are, default visibility) and
linked in (`-force_load`). No Mach-O section or `getsectiondata` scheme was needed.

On the NEO side (`compute/patches/compute-runtime`): 0010 lets `SysCalls::dlopen` find a leaf name such as
`libigc.2.dylib` next to the NEO library (dyld searches only `DYLD_LIBRARY_PATH` and the fallback paths for a leaf
name), 0011 builds ocloc on macOS (the macOS SysCalls, an exported-symbols list, rpaths, `DYLD_LIBRARY_PATH` for the
build-time ocloc, `libiga64.2.dylib`), and 0012 fixes a libc++ difference found once OpenCL ran to the end of a program:
a pool buffer's `unique_ptr` was cleared before the buffer's destructor asked whether it was a pool buffer, and
`clReleaseContext` aborted on a negative reference count. With `IGC_PREFIX`, `build-neo.sh` builds NEO with
`COMPILE_BUILT_INS=TRUE`: NEO's build runs the macOS ocloc (x86_64, Rosetta) for every built-in kernel (buffer **and
image** copies and fills, in all addressing modes, for BMG and LNL) and embeds them (`libze_intel_gpu` 10 → 17.6 MB,
`libigdrcl` 6 → 13 MB), so no `builtins/` directory is needed any more.

### 4.3 Checked on the build Mac (Rosetta, `XEMAC_FAKE=1`)

- **ocloc**: `ocloc compile -file fill_ids.cl -device bmg` gives a 4264-byte zebin, the size of the Linux container's
  (ocloc 26.40 + IGC 2.42.6). Its kernel ISA, disassembled with `iga64 -p=xe2`, is identical to the container's except
  the two `mov null, <hash>` instructions that carry the hash of the input SPIR-V; the SPIR-V differs only in ID
  numbering (the translator revisions differ). Given the container's SPIR-V, the macOS ocloc produces a binary
  **byte-identical** to the container's. Repeated compiles are deterministic. 0.5 s per compile (the first after a
  build ~30 s: Rosetta translates the libraries once).
- **SPIR-V at run time** (Level Zero): `ze_hello spirv-build kernels/fill_ids_bmg.spv`: `zeModuleCreate`
  (`ZE_MODULE_FORMAT_IL_SPIRV`) builds through libigc, a 4264-byte native binary, the kernel created (SIMD 32, 1 arg).
- **OpenCL C at run time** (OpenCL): `cl_hello source-build kernels/fill_ids.cl`: `clCreateProgramWithSource` +
  `clBuildProgram` through libigdfcl → libopencl-clang → libigc, a 4264-byte program binary, the kernel created; the
  program, queue and context released cleanly (after patch 0012).
- **SIP kernel**: `NEOReadDebugKeys=1 DumpSipHeaderFile=sip ze_hello init` writes `sip_header.bin` (the "tssarea" state
  save area header IGC returns with the SIP binary): the device created its SIP kernel from IGC and keeps its default
  mid-thread preemption; NEO aborts there if IGC returns no SIP. With the compiler libraries removed from `lib/`, the
  same command passes without creating a SIP (patch 0008's thread-group fallback, kept for that case).
- Not checkable without the B580: that the kernels and the SIP run (fake mode executes nothing).

### 4.4 Cost

Measured on the build Mac (M1 Pro, 8 cores, 16 GB, other agents' builds running in parallel at times), a cold
`compute/build-igc.sh` without ccache in a fresh work directory (the sources already cloned):

| Step | Time |
|---|---|
| patch + configure (IGC copies LLVM's sources into the build tree and patches them) | 3 min 47 s |
| `llvm`: LLVM + Clang (2828 steps; tablegen under Rosetta) | 23 min 44 s |
| `deps`: SPIR-V translator, opencl-clang (with its PCMs), SPIRV-Tools, vc-intrinsics | 3 min 49 s |
| `igc`: IGC, vISA/IGA, VC, the BiF library (compiled by the x86_64 clang under Rosetta) | ~15 min |
| total | **~46 min** |
| a rebuild from a clean build tree with a warm ccache (`ninja -t clean`, then `llvm deps igc install`) | 4 min 44 s |

Of the ~6.5 CPU-hours, LLVM is 1.5, Clang 1.1, IGC proper 1.25, IGC's built-in library 1.5 (Rosetta), VC 0.55,
vISA/IGA 0.3. The first clone of the sources (llvm-project shallow at the tag, the others partial) is ~2.8 GB.
Disk: ~2.8 GB of sources, ~10-11 GB of build tree. The macOS-specific work was build-system and type-width fixes; no
change to IGC's code generation.

**The container path stays** (`compute/ocloc-docker.sh`): for a build Mac without the IGC build, and as a reference
compiler to compare against.

## 5. gmmlib and the loader

- **gmmlib** `intel-gmmlib-22.10.2` builds for x86_64 once its `__linux__` gates also take `__APPLE__` (macOS user
  mode = Linux user mode for gmmlib: pthread locks, GCC atomics, syslog). `compute/patches/gmmlib/0001`.
- **Level Zero loader** `v1.35.0`: `<link.h>`/`dlinfo` guarded, Mach-O library names (`libze_intel_gpu.1.dylib`),
  `size_t` overloads in `ze_to_string.h`, and `DYLD_LIBRARY_PATH` searched for drivers. Builds and loads NEO.
  `compute/patches/level-zero/0001`.

## 6. The patch series (compute/patches/compute-runtime)

| # | Patch |
|---|---|
| 0001 | portability: `size_t` vs `uint64_t` overloads and fields, libc++'s inline namespace (`std::mutex` forward declaration), the Annex K stand-ins on macOS, gmmlib's Linux init args, `RTLD_DEEPBIND`, no perf syscall |
| 0002 | `shared/source/os_interface/macos/include`: stand-ins for `linux/types.h`, `linux/limits.h`, `linux/const.h`, `linux/bits.h`, `linux/perf_event.h`, `sys/prctl.h`, `sys/sysmacros.h`, `sys/sysinfo.h`, `link.h`, `mtd/mtd-user.h`, `sys/socket.h` (`SOCK_CLOEXEC`) |
| 0003 | `sys_calls_macos.cpp`: `NEO::SysCalls` over xemac (section 3) |
| 0004 | CMake: `NEO_XEMAC_DIR`, AppleClang taken as clang, no forced response files (Apple `ar`), no `librt`, version scripts → exported-symbols lists |
| 0005 | `DrmMemoryManager`: BO maps through `SysCalls` on macOS |
| 0006 | xe: long-running VMs probed (the kext accepts them since `lrmode`), dma-fence-mode fallback for an older kext; small BAR allowed on macOS |
| 0007 | page fault manager: `SIGBUS` on macOS |
| 0008 | thread-group preemption when no compiler (no SIP kernel) |
| 0009 | no direct submission (ULLS: a batch that never ends) without long-running VMs; with them (the `lrmode` kext) ULLS is on as on Linux; `EnableDirectSubmission=1` still forces it |
| 0010 | `SysCalls::dlopen`: a leaf name (the compiler) not found by dyld is looked for next to the NEO library |
| 0011 | ocloc on macOS: the macOS SysCalls, an exported-symbols list, rpaths, `DYLD_LIBRARY_PATH` for the build-time ocloc, `libiga64.2.dylib` |
| 0012 | buffer pools: the pool buffer is still the pool's while it is destroyed (libc++ clears a `unique_ptr` before deleting; `clReleaseContext` aborted) |
| 0013 | sysman: the macOS SysCalls answer `/sys/...` and `/proc/...` from a read-only tree of the device's xe sysfs filled from the kext (`sysfs_macos.cpp`: xemac_pci_info, xemac_sensors); `Directory::getFiles` lists through SysCalls (section 10) |

Configuration (`compute/build-neo.sh`): Xe2 only (`SUPPORT_BMG`, `SUPPORT_LNL`), no ULTs, `COMPILE_BUILT_INS=FALSE`
(built-ins as files, above) or, with `IGC_PREFIX`, `TRUE` (compiled by the macOS ocloc and embedded), no LTO, `-DCMAKE_SYSTEM_PROCESSOR=x86_64` cross build from arm64. Result:
`libze_intel_gpu.1.18.0.dylib` (10 MB, Level Zero 1.18 driver, exports the `ze*GetProcAddrTable` set) and, with
`OCL=1`, `libigdrcl.dylib` (6 MB, `clIcdGetPlatformIDsKHR`). A clean build from the script: 4 min 49 s, ~1 GB of
sources and objects. With `IGC_PREFIX` (built-ins compiled and embedded, ocloc): 17.6 MB and 13 MB, 5 min 14 s.

## 7. Verified so far (on the build Mac, Rosetta, `XEMAC_FAKE=1`)

- The loader finds and loads `libze_intel_gpu.1.dylib`; `zeInitDrivers` returns one GPU driver; the B580 is
  enumerated (`Intel(R) Arc(TM) B580 Graphics`, `0xe20b`, 5 x 4 x 8 EUs, 12 GB); a context is created; device, host
  and shared USM allocations succeed (`ze_hello init`).
- OpenCL through the Khronos ICD loader (`OCL_ICD_FILENAMES`): "Intel(R) OpenCL Graphics (discrete), OpenCL 3.0",
  the B580 with 160 compute units, a context and a queue (`cl_hello`); buffers created and released and the context
  released cleanly with patch 0012 (`cl_hello lifetime`; section 9.2).
- The ioctls of a device creation: `VERSION`, `DEVICE_QUERY` (config, engines, memory, GT list, topology, hwconfig),
  `VM_CREATE` (the LR probe, then the VM), `EXEC_QUEUE_CREATE` per engine, `GEM_CREATE`/`MMAP_OFFSET`/`VM_BIND` with
  bind user fences and `WAIT_USER_FENCE`, then the matching destroys.
- A native ocloc binary loads (`zeModuleCreate` native, `zeKernelCreate`, `zeKernelSuggestGroupSize`, the launch is
  appended), and a built-in kernel is taken from its file instead of the compiler. Nothing executes in fake mode, so
  every wait after a submission times out there: completion, copies and kernels need the B580.
- With IGC (section 4.3): ocloc on macOS (code identical to the Linux ocloc's), SPIR-V and OpenCL C built at run time,
  the SIP kernel created, built-ins embedded; OpenCL releases its context cleanly (patch 0012).
- Sysman (section 10.4): `zesInit`, the device, PCI address, GPU frequency domain, card and package power with energy
  counters and limits, GPU and memory temperatures and the memory module answer with the canned values
  (`zes_info expect-fake`); the core clock is 2850 MHz instead of 0; the SYCL runtime's sysman queries answer.

## 8. Staged plan and estimates

| Stage | Content | State | Estimate (remaining) |
|---|---|---|---|
| 1 | NEO core + xemac backend compile; gmmlib, loader | **done** (this branch) | — |
| 2 | device discovery, USM, fill/copy on the copy engine, queue + fence | built; fake-mode checked; **first B580 run tonight** | 1-3 sessions of fixes after the first run (expected: xemac/kext mismatches in bind/exec details, the BAR window, the job timeout under dma-fence mode) |
| 3a | kernels from offline binaries: native module load, built-ins from files, compute-queue copy/fill, a user kernel | path works up to submission; hardware tonight | 1-2 sessions |
| 3b | IGC on macOS (JIT: SPIR-V and OpenCL C at run time, SIP kernel; built-ins incl. images compiled at build time) | **done on the build Mac** (branch `igc`, section 4); B580 run in the night test | fixes after the first run (expected: none in the compiler itself; the SIP and mid-thread preemption are new to the kext) |
| 4 | SYCL/oneAPI: Intel's DPC++ (`libsycl`, the Unified Runtime's Level Zero adapters, clang `-fsycl`) over this Level Zero; OpenCL applications through the Khronos ICD loader (built, `OCL=1`) | **built on the build Mac (13 Nov, branch `sycl`, `DESIGN-sycl.md`)**: DPC++ v7.2.0 with 5 patches + 2 on UMF; device query and kernel builds (AOT bmg image, and SPIR-V through IGC) pass against the canned device; first B580 run in the night snippet | fixes after the first run (expected in the adapters' use of NEO: immediate lists, counter-based events, UMF pools) |
| sysman | `zesInit` and the zes* modules over the kext (patch 0013, `debug.intelxe.sensors_raw`, section 10) | **done on the build Mac** (branch `l0sysman`); B580 run in the night snippet (`user/tests/compute/night-sysman.sh`) | fixes after the first run (expected: none in the data path; units and file existence checked against xe's sysfs) |
| kernel | long-running VMs (**done, branch `lrmode`, untested on hardware**), `EXEC_QUEUE_SET_PROPERTY`, OA/EU-stall observation, SVM/fault mode | items in `docs/parity/TODO.md` | LR mode: night run of `user/tests/kernel/lr.c` and NEO with ULLS; SVM is large |

## 9. Night test (stage 2/3a): exact commands

Build Mac: the five lines of `compute/README.md` (build, two `ocloc-docker.sh`, `build-neo.sh stage`); copy
`$L0_WORK/stage` to the test machine as `~/l0` (bin, lib, kernels, builtins, SHA256SUMS). On the test machine, each
command ≤ 60 s:

```sh
cd ~/l0 && shasum -a 256 -c SHA256SUMS | grep -v OK$            # nothing printed = intact
DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello init                  # 1-2: discovery, context, USM
DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello                       # 3-4: copy engine (no kernels)
cd ~/l0/builtins && DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/ze_hello compute                       # 5: built-ins
cd ~/l0/builtins && DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/ze_hello kernel ../kernels/fill_ids_bmg.bin  # 6
cd ~/l0/builtins && OCL_ICD_FILENAMES=$HOME/l0/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/cl_hello lifetime  # OpenCL object lifetimes only (9.2)
cd ~/l0/builtins && OCL_ICD_FILENAMES=$HOME/l0/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/cl_hello  # OpenCL
cd ~/l0/builtins && OCL_ICD_FILENAMES=$HOME/l0/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/cl_hello kernel ../kernels/fill_ids_bmg.bin
```

On a failure, the same command again with `NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1 XEMAC_TRACE=1` and
`2>&1 | tail -80`; `EnableDirectSubmission=0` (with `NEOReadDebugKeys=1`) rules out NEO's ring-buffer submission
(on by itself with the `lrmode` kext, which accepts LR VMs: `PrintXeLogs=1` shows `isLongRunningVmSupported 1`);
`sysctl debug.intelxe.stats` and the kext log show the kernel side.

### 9.1 Night test with IGC (stage 3b): exact commands

Build Mac: `compute/build-igc.sh`, then `IGC_PREFIX=$IGC_WORK/prefix OCL=1 compute/build-neo.sh` (compute/README.md);
the stage is packed as `l0igc-stage.tgz` (directory `l0igc/`: bin with ze_hello, cl_hello, ocloc; lib with the drivers,
loaders, gmmlib and the compiler; kernels with fill_ids as .cl, .spv and .bin; SHA256SUMS). It goes next to the stage-2
directory (`~/l0`), not over it. On the test machine (Intel, so no Rosetta: the compiler runs natively), each command
≤ 60 s; `t60` kills a command that hangs in a GPU wait:

```sh
t60() { "$@" & p=$!; { sleep 60; kill -9 $p; } >/dev/null 2>&1 & w=$!; wait $p; r=$?; kill $w 2>/dev/null; return $r; }
cd ~ && tar xzf l0igc-stage.tgz && cd ~/l0igc && shasum -a 256 -c SHA256SUMS | grep -v ': OK$'   # nothing = intact
export DYLD_LIBRARY_PATH=$PWD/lib OCL_ICD_FILENAMES=$PWD/lib/libigdrcl.dylib NEO_CACHE_PERSISTENT=0
export NEOReadDebugKeys=1 DumpSipHeaderFile=/tmp/l0igc-sip   # NEO writes /tmp/l0igc-sip_header.bin when it makes the SIP
t60 ./bin/ze_hello init; ls -l /tmp/l0igc-sip_header.bin    # discovery, USM, and the SIP kernel from IGC
t60 ./bin/ze_hello                                     # copy engine, as in stage 2
t60 ./bin/ze_hello compute                             # built-ins, now embedded (no builtins/ directory)
t60 ./bin/ze_hello kernel kernels/fill_ids_bmg.bin     # native binary (compiled by the macOS ocloc)
t60 ./bin/ze_hello spirv kernels/fill_ids_bmg.spv      # SPIR-V compiled by libigc in the driver, run, checked
t60 ./bin/cl_hello                                     # OpenCL buffer round trip
t60 ./bin/cl_hello source kernels/fill_ids.cl          # OpenCL C compiled by libigdfcl + libigc, run, checked
t60 ./bin/ocloc compile -file kernels/fill_ids.cl -device bmg -out_dir /tmp/l0igc-oc && ls -l /tmp/l0igc-oc
```

(Variables go through `export`, not `env VAR=... cmd`: `/usr/bin/env` is a protected system binary, so dyld drops
`DYLD_LIBRARY_PATH` when it runs and the loader finds no driver.) The same sequence with `XEMAC_FAKE=1` and the
`spirv-build`/`source-build` variants ran on the build Mac in 4 s.

Reading the results: the `init` line must pass and leave `/tmp/l0igc-sip_header.bin` (32 bytes, "tssarea"): the
device runs with mid-thread preemption and IGC's SIP kernel. A hang or GPU reset in the later commands that the stage-2
package (`~/l0`, thread-group preemption) does not show points at the SIP/mid-thread path; `export
ForcePreemptionMode=3` (thread group; `NEOReadDebugKeys=1` is already set) and the same command again tells it apart.
On a failure, the command again with `PrintDebugMessages=1 PrintXeLogs=1 XEMAC_TRACE=1` exported and
`2>&1 | tail -80`; IGC's own dumps: `IGC_ShaderDumpEnable=1` (in `/tmp/IntelIGC`).

### 9.2 The OpenCL abort of the first B580 night (13 Nov, stage-2 package `~/l0`)

On the B580 every Level Zero step passed, but both `cl_hello` runs of the stage-2 package aborted after their last
GPU step had passed (`buffer write + read back`, `kernel from native binary`): "Abort was called at 137 line in file
reference_tracked_object.h", exit 134. The IGC package (`~/l0igc`) ran `cl_hello` with exit 0 the same night.

- Line 137 is `UNRECOVERABLE_IF(current < 0)` in `ReferenceTrackedObject::decRefInternal`: an internal reference
  count below zero. It is the abort patch 0012 fixes (section 4.2): a context's buffer pool deletes its pool buffer,
  `MemObj::~MemObj` asks the context `isPoolBuffer(this)` to know whether the buffer holds a context reference, and
  with libc++ the pool's `unique_ptr` is already null then, so the pool buffer drops a context reference it never took.
  `cl_hello` creates a buffer before it releases the context in both modes (the 4 MB round trip, the 16 KB kernel
  output), and `clCreateBuffer` creates the context's small-buffers pool (`initAggregatedSmallBuffers`), so both
  modes reach it; the device-query steps alone do not.
- The stage-2 package was built (12 Nov, 16:05) from a NEO tree with patches 0001-0009; 0010-0012 came with the IGC
  work later that day (0012 at 18:13). The IGC package has all twelve. Same source, same patch, the one difference.
- Not the ICD path: macOS's OpenCL.framework loads no ICDs, `cl_hello` links only the Khronos loader, and the loader
  opens the single `OCL_ICD_FILENAMES` library once; a second NEO image would also give a second platform, and one is
  listed. Nor a shim lifetime difference: the abort is in NEO's own object accounting, before any xemac call of the
  teardown.
- Reproduced without the GPU: `cl_hello lifetime` (new: a 4 KB, a 16 KB and a 4 MB buffer created and released, then
  the queue and the context; no GPU work, so it runs under `XEMAC_FAKE=1`). On the build Mac (Rosetta, fake device):
  the 12 Nov 16:05 stage aborts at `reference_tracked_object.h:137` after "buffers created and released" (exit 134);
  the IGC stage, and the stage rebuilt without IGC from the current series (0001-0012), pass (exit 0).

Fix: no new patch; the stage-2 package is rebuilt from the current series (`OCL=1 compute/build-neo.sh`) and replaces
`~/l0`. The night snippet runs `cl_hello lifetime` first (it isolates the release path from GPU work) and, for any
OpenCL failure, the same command again with `NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1 XEMAC_TRACE=1`
exported in the subshell.

## 10. Sysman (zes*) over the kext

Level Zero's system management API (`zesInit`, then per device: properties, PCI, frequency, power and energy,
temperature, memory, engines, standby, schedulers, firmware, RAS, fans, ...) is what monitoring tools (xpu-smi, the
oneAPI tools) and the SYCL runtime use for clocks, power and free memory. The Unified Runtime's Level Zero adapter calls
`zesInit` for Battlemage (IP version 20.1 and newer) and answers `ext_intel_power_limits`,
`ext_intel_current_clock_throttle_reasons`, fan speed and, when the core cannot, the free memory from it.

### 10.1 The approach: the Linux sysman unchanged, the sysfs answered from the kext

NEO's sysman (`level_zero/sysman/source`) has a Linux and a Windows backend; the Linux one is built on macOS (it is
`UNIX`). Before this change `zesInit` returned `ZE_RESULT_ERROR_UNINITIALIZED`: `LinuxSysmanImp::init` names its device
by `readlink(/proc/<pid>/fd/<fd>)` and looks for it under `/sys/class/drm`, and macOS has neither. Every procfs,
sysfs and hwmon access of the Linux sysman goes through `NEO::SysCalls` (open, pread, read, stat, access, readlink,
realpath, opendir/readdir), as the DRM backend's do. So, as the render node is answered by xemac, patch 0013 answers
those paths: `shared/source/os_interface/macos/sysfs_macos.cpp` keeps a small read-only tree of the files Linux xe has
for the device and fills it from the kext. The sysman modules, the xe KMD interface (`sysman_kmd_interface_xe.cpp`: which
file means what) and the BMG product helper run as on Linux; nothing in sysman is changed. A native macOS sysman
backend (an `OsSysman` and 19 `Os<module>` implementations) was the alternative: several times the code, and each
module's semantics (units, which handles exist, error codes) re-done by hand instead of taken from the code Intel tests
against xe.

The tree (`D` = `/sys/devices/pci0000:00/0000:00:01.0/0000:bb:dd.f`, the address the synthetic `/sys/dev/char` link
already gave the core):

| Path | Content | From |
|---|---|---|
| `/proc/<pid>/fd/<xemac fd>` | link to `/dev/dri/renderD128` (sysman's device name) | xemac_is_fd |
| `/sys/dev/char/226:128`, `/sys/class/drm/{card0,renderD128}`, `/sys/bus/pci/devices/<bdf>`, `D/drm/*/device` | links to `D` and its `drm/` entries, as on Linux | xemac_pci_info |
| `D/vendor`, `device`, `subsystem_vendor`, `subsystem_device`, `revision` | `0x8086`, ... | xemac_pci_info |
| `D/tile0/gt0/freq0/act_freq`, `cur_freq`, `rp0_freq`, `rpn_freq` | MHz: CAGF, the request, RP0, RPn | sensors_raw (`FREQ`) |
| `D/tile0/gt0/freq0/rpe_freq`, `min_freq`, `max_freq` | MHz: RPe; the floor SLPC runs with (`slpc_min`, raised by an `slpc_boost` lease); the ceiling (`slpc_max` or RP0) | sensors_raw (`RANGE`) |
| `D/hwmon/hwmon0/name` | `xe` | |
| `energy1_input`, `energy2_input` | card, package energy, microjoules (PMT telemetry, summed across wraps) | sensors_raw (`ENERGY`) |
| `temp2_input`, `temp3_input` | package, VRAM temperature, m°C | sensors_raw (`TEMP`) |
| `power1_max`, `power1_cap`, `power2_max`, `power2_cap` | card and package PL1 and PL2, microwatts, 0 when the limit is off (as xe) | sensors_raw (`LIMITS`) |
| `power1_crit` or `curr1_crit` | the card's I1, microwatts or milliamperes | sensors_raw (`CRIT`) |

Every file is mode 0444. A file exists when the kext reported its quantity in the first sample of the process (a kext
without `sensors_raw`: only the device part, and sysman finds no power or temperature handles; the GPU frequency domain
then reads -1); every read takes a new sample (one sysctl, no forcewake). `NEO_MACOS_SYSFS_TRACE=1` logs every path
asked and the answer.

`Directory::getFiles` (the core's directory listing) also goes through SysCalls on macOS, so the core finds the device's
sysfs directory and reads `tile0/gt0/freq0/max_freq`: `zeDeviceGetProperties.coreClockRate` and SYCL's
`max_clock_frequency` are RP0 (or `slpc_max`) instead of 0.

### 10.2 The kext side: debug.intelxe.sensors_raw

`debug.intelxe.sensors` (text; IntelXe Manager and tools/regress parse it) already had everything except RPe and SLPC's
range, but as text with millijoule energy. `debug.intelxe.sensors_raw` (read only, anyone) gives the same sample as
`struct intelxe_sensors` (`kernel/IntelXe/uapi/intelxe_sensors.h`, 120 bytes, versioned, a `valid` bit per group, a
CLOCK_UPTIME_RAW timestamp, MHz, degrees, microjoules, milliwatts). Both sysctls come from one function,
`IntelXe::sensorsSample` (`xe_sensors.h` holds the pure parts: the sample from the register values, the energy meter
and the limits, and the text), so the two cannot disagree; `tests/sensors_test.cpp` checks the struct layout, the
conversions and that the text is byte for byte what `sensorsString` printed before (against a copy of the old code,
fixed and 300 pseudo-random samples, every buffer length). User space reads it through `xemac_sensors()` (`user/xemac`),
which with `XEMAC_FAKE=1` returns canned values (act 2100 MHz, request 2400, RP0 2850, RPn 400, RPe 1800, range
1200-2850, 47 C and 52 C, energy growing at 40 W and 25 W, card PL1 190 W on, PL2 250 W off, package PL1 150 W, I1
300 W; `XEMAC_FAKE_SENSORS=0`: a kext without the sysctl).

### 10.3 What sysman offers on macOS

| Module | macOS | Why |
|---|---|---|
| `zesInit`, driver, device list, `zesDeviceGetProperties` | yes (name, ids, `type`, `flags`; serial and board numbers "unknown": Linux reads them from PMT; driver version "unknown": `/sys/module/xe`) | |
| `zesDeviceGetState` | yes (no reset pending) | |
| `zesDeviceProcessesGetState` | `ZE_RESULT_ERROR_NOT_AVAILABLE` | Linux walks `/proc/<pid>/fd` and fdinfo of every process |
| PCI: properties, state | address yes; link speed/width -1 (no link files) | |
| PCI: stats, BARs | `UNSUPPORTED_FEATURE` / none | PMT counters; `resource` file |
| Frequency, GPU domain | hardware range (RPn-RP0), range (SLPC's floor and ceiling), request, actual, efficient; TDP -1, throttle reasons 0 | |
| Frequency, memory domain | listed (BMG product helper), values -1 | PMT telemetry |
| `zesFrequencySetRange` | `UNSUPPORTED_FEATURE` (the files are read only; `canControl` still reads 1: the product helper decides it) | no live range control in the kext (`slpc_min`/`slpc_max` apply at SLPC start, `slpc_boost` is a 10 s lease) |
| Power: card and package domains | energy counters, sustained/burst/peak limits (PL1, PL2, I1), `zesDeviceGetCardPowerDomain`; default/min/max limit -1 (xe hides `power1_rated_max` on BMG, as on Linux) | |
| `zesPowerSetLimits*` | `UNSUPPORTED_FEATURE` (write fails) | read-only pcode use |
| Temperature | GPU (package) and memory (VRAM); max -1 (no `temp2_emergency`); global: none | global/board sensors come from PMT |
| Memory | one module, 12 GB, state from the xe memory-region query (as before), health unknown; bandwidth `UNSUPPORTED_FEATURE` | bandwidth from PMT |
| Engines | none | Linux reads engine activity from the xe PMU (`perf_event_open`), which macOS does not have; the kext keeps no per-engine busy time |
| Standby, schedulers, performance factors, firmware, RAS, fabric, diagnostics, fans, PSUs, LEDs, ECC | none / `UNSUPPORTED_FEATURE` | sysfs files or interfaces the kext does not have |

### 10.4 Checked on the build Mac (Rosetta, `XEMAC_FAKE=1`)

- `zes_info expect-fake` (`user/tests/compute/zes_info.c`, staged by `build-neo.sh` in `bin/`): PASS with 0 failed and
  0 mismatched: the canned clocks, range, temperatures, limits; power from the energy counters 40.0 W and 25.0 W; the
  other modules answer with no handles or `UNSUPPORTED_FEATURE`. `zes_info set-freq 1500 2400`: "NONE
  zesFrequencySetRange". `zes_info watch 2`: the samples. With `XEMAC_FAKE_SENSORS=0` (an older kext): `zesInit` works,
  the frequency domain reads -1, no power or temperature handles, 0 failed.
- `ze_hello init` and `cl_hello lifetime` unchanged (pass).
- SYCL over this stage (`sycl-ls --verbose`, `sycl_hello build`, both adapters): `ext_intel_power_limits` is now listed
  (it was not: `zesInit` failed), `max_clock_frequency` 2850 MHz (was 0), free memory 12042 of 12288 MB (that one came
  from the core's usable-memory extension already), sysman queries answer (`PASS sysman`).
- The IGC stage builds with the patch (13 min cold, built-ins compiled by its ocloc, which links `sysfs_macos.cpp` through
  `NEO_SYS_CALLS_SOURCES`); over it `zes_info expect-fake` and `ze_hello spirv-build` pass, and `sycl_hello_jit build`
  passes all four steps (device, free memory, sysman, 10 kernels compiled from SPIR-V by IGC).
- Not checkable without the B580: the kext's numbers through the real sysctl (`xemac_sensors` outside fake mode).

### 10.5 Night test

The commands are `user/tests/compute/night-sysman.sh` (phase Z): Z0 the stage checksums and whether the loaded kext has
`sensors_raw`; Z1 `zes_info` once; Z2 `zes_info watch 5`; Z3 `zes_info set-freq 1500 2400` (expect "NONE
zesFrequencySetRange"); Z4 `sysctl debug.intelxe.sensors` for comparison; Z5 `ze_hello init`, `cl_hello lifetime`;
Z6-Z8 `sycl-ls`, `sycl_hello build`, `sycl_hello` over this stage; Z9 `zes_info` over the IGC stage. Each run `tmo 60` in
a subshell with the variables exported; a failed `zes_info` run is repeated with `NEO_MACOS_SYSFS_TRACE=1` and NEO's
messages. The stages go to `~/l0sm` and `~/l0smigc` (not over `~/l0`/`~/l0igc`), the SYCL stage with the new
`sycl_hello` to `~/b580/sycl`; the kext with `sensors_raw` is this branch's `kernel/IntelXe` build.
