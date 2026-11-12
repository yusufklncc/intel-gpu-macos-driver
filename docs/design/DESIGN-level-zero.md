# Level Zero / oneAPI (and Intel's OpenCL) on macOS: Intel's compute runtime over IntelXe.kext

Status 12 Nov 2026 (branch `levelzero`). Stage 1 done: Intel's compute runtime ("NEO") builds for macOS x86_64 against
our kernel interface, with gmmlib and the Level Zero loader (`compute/`). Stage 2 is written and checked as far as a
Mac without the B580 can check it; its first hardware run is tonight's test. Stages 3-4 have a path that needs no
compiler on the Mac. Nothing here has run on the B580 yet.

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
| VMs: `LR_MODE` always, `SCRATCH_PAGE` unless disabled, `FAULT_MODE`+`NO_VM_OVERCOMMIT` when page faults are on | `DRM_IOCTL_XE_VM_CREATE` | `SCRATCH_PAGE` yes; **`LR_MODE` and `FAULT_MODE` refused** (no preempt fences, no GPU fault service): patch 0006 probes `LR_MODE` and falls back to dma-fence-mode VMs |
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
   waiting for jobs. The kext would need: accept the flag, refuse syncobj out-syncs on such VMs as xe does, no TDR for
   LR queues (the GuC still preempts them), and the eviction of a BO bound in an LR VM through a preemption of its
   queues. Until then patch 0006's fallback runs NEO in dma-fence mode, and patch 0009 turns off NEO's direct
   submission (ULLS, on by default for Xe2), whose ring-buffer batch never ends and would be reset as a hang.
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

No kernel change was made for this work.

## 3. What NEO assumes from Linux, and the macOS replacement

| Linux facility | Where NEO uses it | macOS (this port) |
|---|---|---|
| `/dev/dri/renderD*`, `/dev/dri/by-path` | `Drm::discoverDevices` (`drm_neo.cpp:818`) | `SysCalls::open("/dev/dri/renderD128")` = `xemac_open()` (NMXAccel, else IntelXe directly; `XEMAC_FAKE=1`); `by-path` does not exist, so NEO tries the renderD nodes and finds one |
| DRM ioctls on that fd | everywhere in `os_interface/linux` | `xemac_ioctl()`: the request numbers as `<sys/ioccom.h>` encodes them, absolute timeouts moved to the kext's clock |
| `mmap` of the DRM fd at `GEM_MMAP_OFFSET` | `DrmMemoryManager` | `xemac_mmap()` (IOConnectMapMemory64, write-combined for VRAM; `MAP_FIXED` remaps the IOKit pages at NEO's address); `xemac_munmap()` |
| sysfs: `/sys/dev/char/M:m` → PCI path | `getPciPath()` (bus/device/function for `zeDevicePciGetPropertiesExt`, BDF filters) | synthesized from `xemac_pci_info()` (`../../devices/pci0000:00/0000:00:01.0/0000:bb:dd.f/drm/renderD128`) |
| sysfs: frequencies, tile info, `prelim_*`, eudebug switch, `intel_pmt` telemetry | `Drm::readSysFsAsString`, sysman, `pmt_util.cpp` | absent: reads fail and NEO takes defaults (core clock reports 0 MHz; sysman has no frequency/power/temperature). A later step answers the useful ones (max frequency) from the kext |
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

## 4. The compiler

NEO compiles OpenCL C and SPIR-V with **IGC**
([intel/intel-graphics-compiler](https://github.com/intel/intel-graphics-compiler), `0f18b8dc` for this NEO: API 2.42),
loaded at run time through CIF (`libigdfcl` = the OpenCL C front end built on
opencl-clang, `libigc` = SPIR-V/LLVM to Gen ISA, also the source of the **SIP kernel** for mid-thread preemption and
debugging). Building IGC means:

- **LLVM 22.1.8 + Clang** with IGC's 10 patches for 22.x (`external/llvm/releases/22.0.0`; IGC also carries patch sets
  for 14-17 and 23), **opencl-clang** `ocl-open-220`, **SPIRV-LLVM-Translator** `llvm_release_220`, **vc-intrinsics**,
  **SPIRV-Tools**/**SPIRV-Headers**, plus flex, bison, python3 with mako.
- On macOS: LLVM and Clang build there natively. IGC itself assumes ELF/GNU in a few places, found by reading the
  tree at `0f18b8dc`: `IGC/CMakeLists.txt` (`-Wl,--version-script`, `-Wl,-Bsymbolic`, `-fuse-ld`, a glibc check),
  `IGC/OCLFE/igd_fcl_mcl/source/LoadBuffer.cpp` (`<link.h>`, `<linux/limits.h>`; it finds the resources embedded in
  the shared object by `dlsym` of their symbols), `visa/iga/IGALibrary/api/igax.hpp` (`<malloc.h>`), and the
  embedding of the built-in bitcode and OpenCL C headers into the library. No Apple branch exists in its CMake. These
  are the same kinds of change NEO needed; nothing found so far needs Linux at run time.
- **Cost** (estimate, not measured): LLVM+Clang Release for x86_64 on this 8-core Mac 1-1.5 h and ~10 GB; IGC ~1 h
  and a few GB; the port of IGC's build and resource embedding 1-2 days, the SIP kernel and CIF loading included. The
  build can run entirely as x86_64: tablegen and IGC's generators then run under Rosetta on Apple silicon, so no
  host/target split is needed.
- Not attempted in this session: the session's goal was the runtime, and stage 3 has a path without it (below).

**Alternative now: compile on Linux, load binaries on the Mac.** Intel's `ocloc` (the offline compiler, with IGC)
produces native Battlemage binaries (zebin) that NEO loads without a compiler: `zeModuleCreate` with
`ZE_MODULE_FORMAT_NATIVE`, `clCreateProgramWithBinary`. `compute/ocloc-docker.sh` runs the released ocloc 26.40 and
IGC 2.42 (sha256-checked `.deb`s) in a linux/amd64 container on the build Mac (OrbStack/Rosetta); it works today:

- `ocloc-docker.sh kernel X.cl OUT` → `OUT/X_bmg.bin`;
- `ocloc-docker.sh builtins <neo> OUT` → NEO's buffer built-in kernels (fill, copy, copy rect, kernel timestamps) for
  BMG in its four addressing modes, named as NEO's `BuiltIn::FileStorage` looks them up in the working directory
  (`20_1_0_bindful_image_stateless_buffer_fill_buffer.builtin_kernel.bin`, ...). Intel's packages embed them from a
  `kernels_bin` repository its manifest names without a public URL; without them every fill and every copy on a
  compute queue needs the compiler.
- What it cannot give: SPIR-V/OpenCL C at run time (SYCL's JIT path, `clBuildProgram`), the SIP kernel (patch 0008
  makes the device use thread-group preemption when no compiler exists), image built-ins (not generated yet).

Using Mesa's compiler for NEO is not an option (different binary format, ABI and runtime contract).

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
| 0006 | xe: long-running VMs probed, dma-fence-mode fallback; small BAR allowed on macOS |
| 0007 | page fault manager: `SIGBUS` on macOS |
| 0008 | thread-group preemption when no compiler (no SIP kernel) |
| 0009 | no direct submission (ULLS: a batch that never ends) without long-running VMs; `EnableDirectSubmission=1` still forces it |

Configuration (`compute/build-neo.sh`): Xe2 only (`SUPPORT_BMG`, `SUPPORT_LNL`), no ULTs, `COMPILE_BUILT_INS=FALSE`
(built-ins as files, above), no LTO, `-DCMAKE_SYSTEM_PROCESSOR=x86_64` cross build from arm64. Result:
`libze_intel_gpu.1.18.0.dylib` (10 MB, Level Zero 1.18 driver, exports the `ze*GetProcAddrTable` set) and, with
`OCL=1`, `libigdrcl.dylib` (6 MB, `clIcdGetPlatformIDsKHR`). A clean build from the script: 4 min 49 s, ~1 GB of
sources and objects.

## 7. Verified so far (on the build Mac, Rosetta, `XEMAC_FAKE=1`)

- The loader finds and loads `libze_intel_gpu.1.dylib`; `zeInitDrivers` returns one GPU driver; the B580 is
  enumerated (`Intel(R) Arc(TM) B580 Graphics`, `0xe20b`, 5 x 4 x 8 EUs, 12 GB); a context is created; device, host
  and shared USM allocations succeed (`ze_hello init`).
- OpenCL through the Khronos ICD loader (`OCL_ICD_FILENAMES`): "Intel(R) OpenCL Graphics (discrete), OpenCL 3.0",
  the B580 with 160 compute units, a context and a queue (`cl_hello`).
- The ioctls of a device creation: `VERSION`, `DEVICE_QUERY` (config, engines, memory, GT list, topology, hwconfig),
  `VM_CREATE` (the LR probe, then the VM), `EXEC_QUEUE_CREATE` per engine, `GEM_CREATE`/`MMAP_OFFSET`/`VM_BIND` with
  bind user fences and `WAIT_USER_FENCE`, then the matching destroys.
- A native ocloc binary loads (`zeModuleCreate` native, `zeKernelCreate`, `zeKernelSuggestGroupSize`, the launch is
  appended), and a built-in kernel is taken from its file instead of the compiler. Nothing executes in fake mode, so
  every wait after a submission times out there: completion, copies and kernels need the B580.

## 8. Staged plan and estimates

| Stage | Content | State | Estimate (remaining) |
|---|---|---|---|
| 1 | NEO core + xemac backend compile; gmmlib, loader | **done** (this branch) | — |
| 2 | device discovery, USM, fill/copy on the copy engine, queue + fence | built; fake-mode checked; **first B580 run tonight** | 1-3 sessions of fixes after the first run (expected: xemac/kext mismatches in bind/exec details, the BAR window, the job timeout under dma-fence mode) |
| 3a | kernels from offline binaries: native module load, built-ins from files, compute-queue copy/fill, a user kernel | path works up to submission; hardware tonight | 1-2 sessions; image built-ins and the SIP kernel open |
| 3b | IGC on macOS (JIT: SPIR-V and OpenCL C at run time, SIP kernel) | not started; blockers listed in section 4 | 2-4 days (LLVM 22 + Clang, opencl-clang, translator, IGC's build port) |
| 4 | SYCL/oneAPI: Intel's DPC++ runtime (`libsycl`) over this Level Zero; OpenCL applications through the Khronos ICD loader (built, `OCL=1`) | not started (the ICD loader builds unchanged) | DPC++ runtime build for macOS 2-4 days (its own Linux assumptions); AOT-compiled SYCL apps (`-fsycl-targets=intel_gpu_bmg_g21` on Linux) could run before IGC; JIT needs 3b |
| kernel | long-running VMs, `EXEC_QUEUE_SET_PROPERTY`, OA/EU-stall observation, SVM/fault mode | items in `docs/parity/TODO.md` | LR mode ~1-2 sessions (with the preempt-fence eviction path); SVM is large |

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
cd ~/l0/builtins && OCL_ICD_FILENAMES=$HOME/l0/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/cl_hello  # OpenCL
cd ~/l0/builtins && OCL_ICD_FILENAMES=$HOME/l0/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$HOME/l0/lib ../bin/cl_hello kernel ../kernels/fill_ids_bmg.bin
```

On a failure, the same command again with `NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1 XEMAC_TRACE=1` and
`2>&1 | tail -80`; `EnableDirectSubmission=0` (with `NEOReadDebugKeys=1`) rules out NEO's ring-buffer submission;
`sysctl debug.intelxe.stats` and the kext log show the kernel side.
