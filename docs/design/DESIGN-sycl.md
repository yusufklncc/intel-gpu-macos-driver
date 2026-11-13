# SYCL on macOS: Intel's DPC++ runtime over our Level Zero (design, 2026-11-13)

> Design note with its date: it records what was chosen, built and checked that day. Status: **built on the build Mac,
> not yet run on the B580** (the night snippet `user/tests/sycl/night-sycl.sh` is its first hardware run).

SYCL is the C++ programming model over Level Zero that oneAPI applications use. Level Zero itself already runs here:
Intel's compute runtime (NEO) on xemac, with IGC compiling SPIR-V in the driver (`DESIGN-level-zero.md`). This note is
the next layer: a SYCL runtime and a SYCL compiler for macOS x86_64, so a SYCL program runs on the B580 through that
Level Zero.

## 1. Which implementation

Two open SYCL implementations have a Level Zero backend. Both were fetched and read at a pinned revision:

| | Intel DPC++ (`intel/llvm`) | AdaptiveCpp |
|---|---|---|
| Revision | tag `v7.2.0`, `71767974d44a` (2026-10-14) | tag `v25.10.0`, `9f842c701a59` (2025-11-05) |
| Licence | Apache-2.0 WITH LLVM-exception (LLVM, the SYCL runtime, the Unified Runtime, UMF); its fetched dependencies MIT (emhash, vc-intrinsics) and Khronos MIT-style (SPIRV-Headers) | BSD-2-Clause; needs LLVM (Apache-2.0 WITH LLVM-exception) and Boost (BSL-1.0) |
| Level Zero path | the Unified Runtime's Level Zero adapters (`v1`, and `v2`, the default for Battlemage and newer): Intel's own production path, the one oneAPI ships on Linux and Windows | the `ze` backend; its own documentation: "targeting SPIR-V devices through OpenCL is currently more mature" |
| Device code | clang `-fsycl` -> spir64 LLVM IR -> SPIR-V (the in-tree SPIR-V translator) wrapped into the host binary; at run time `zeModuleCreate` (`ZE_MODULE_FORMAT_IL_SPIRV`), i.e. IGC in NEO, exactly what `ze_hello spirv` already exercises. Ahead of time: the driver runs `ocloc` (`-fsycl-targets=spir64_gen`), and the macOS ocloc exists (`build-igc.sh`) | its single-pass compiler (a clang plugin) embeds LLVM IR; at run time its `llvm-to-spirv` lowers the IR and translates it with a SPIR-V translator, then `zeModuleCreate`. So the test machine needs **LLVM and the translator at run time** (a x86_64 libLLVM, ~100 MB, beside the program) |
| macOS | the runtime already has Darwin branches (`os_util`, the UR loader's `.dylib` names and adapter search); not supported or tested by Intel. What failed here is section 3 | a CPU (OpenMP) target is supported on macOS; the generic JIT compiler needs a 2-stage (bootstrapped) LLVM build with compiler-rt and lld, per its own install guide, plus Boost; the `ze` backend has never been described on macOS |
| What it fetches | at configure time, through FetchContent, at revisions its CMake names (UMF, emhash, vc-intrinsics, SPIRV-Headers, the OpenCL headers and ICD loader): all pinned, all prefetched here and the build run disconnected | the SPIR-V translator from AdaptiveCpp's fork at a **branch head** (`GIT_TAG origin/llvm_release_<N>0`, not a commit), during the build |
| Build | one LLVM tree: LLVM + clang (X86 and SPIRV targets) + the SYCL runtime + UR + libdevice: 3640 ninja steps, 16 min cold on the build Mac (section 2); 2.4 GB of sources (sparse partial clone), 1.3 GB of build tree | AdaptiveCpp itself is small (895 files) but needs an LLVM + clang development build first (the same LLVM cost, bootstrapped twice by its macOS recipe), Boost, and for the L0 path a x86_64 LLVM shipped to the test machine |

**Decision: DPC++.** Its Level Zero adapter is the production path Intel tests on Battlemage (and the one oneAPI
applications are built for: an application compiled by Intel's compiler on Linux carries the same SPIR-V and the same
runtime interfaces), its device code reaches our driver as plain SPIR-V through `zeModuleCreate`, with no LLVM at run
time, and every source and dependency is pinned by commit. AdaptiveCpp's L0 backend is the less mature path by its own
account, needs LLVM and a translator at run time on the test machine, and its build fetches an unpinned branch.

## 2. What was built

`compute/build-sycl.sh` (`fetch patch configure compiler runtime stage`), with `SYCL_WORK` (the work tree, here
`~/hackintosh/work/sycl`) and `L0_WORK` (build-neo.sh's tree: the Level Zero loader it installed, NEO's source):

| Pin | Revision | Tree SHA-256 (`git ls-tree -r <rev> \| shasum -a 256`) |
|---|---|---|
| intel/llvm (DPC++) | `v7.2.0`, `71767974d44a` | `08d6ade04888c35b...` |
| UMF (unified-memory-framework) | `v1.2.0`, `d8763834d68f` | `8c058107871176ac...` |
| emhash | `5e131ba09a52` | `ca8e7447efc4c0f9...` |
| SPIRV-Headers | `5c50cbd25a40` | `96ec83a58480759f...` |
| vc-intrinsics | `4fc83e12979d` | `7add8f3a5f54bed1...` |
| OpenCL headers, ICD loader | `v2025.07.22` (build-neo.sh's pins, `OCL=1`) | (build-neo.sh) |
| AdaptiveCpp (read, not built) | `v25.10.0`, `9f842c701a59` | `cc4564162cef5d22...` |

The full hashes are in the script; `fetch` checks the commit and the tree hash of every checkout and stops on a
mismatch. The dependencies are the revisions intel/llvm's own CMake names at that tag; FetchContent runs disconnected
(`FETCHCONTENT_FULLY_DISCONNECTED`), so nothing is downloaded during the build. The intel/llvm checkout is a sparse
partial clone of the directories the build reads (LLVM's CMake also wants `libc/` for its shared headers).

**One LLVM tree for x86_64**, as `build-igc.sh` builds IGC: `CMAKE_OSX_ARCHITECTURES=x86_64`,
`LLVM_HOST_TRIPLE=x86_64-apple-darwin`, minimum macOS 15, no `CMAKE_SYSTEM_NAME`, so the build's own tools (tablegen,
and the just-built clang that compiles libdevice) are x86_64 and run under Rosetta. The compiler is therefore an x86_64
program too: it runs under Rosetta on the build Mac and natively on the test machine. `DEFAULT_SYSROOT` is the SDK, so
the built clang finds libc++ and libSystem without `-isysroot`. Projects: clang, sycl, llvm-spirv, opencl (only for
`opencl-aot` and the OpenCL headers), xpti (headers), libdevice; `SYCL_ENABLE_BACKENDS=level_zero;level_zero_v2`; no
XPTI tracing, no sycl-jit (kernel fusion), no tests, no OpenCL adapter. The UR Level Zero adapters link build-neo.sh's
patched loader (v1.35.0, found through its pkg-config file) and compile against one include directory with the loader's
headers and NEO's `ze_intel_gpu.h` (the pinned NEO source, so the adapters see our driver's extension headers).
UMF is built with `UMF_HWLOC_STANDIN=ON` (patch umf 0001) instead of fetching and autotools-building hwloc.

Measured on the build Mac (M1 Pro, 8 cores, other agents' builds running at the same time, load average up to 70):

| Step | Time |
|---|---|
| configure (sources present) | under 1 min |
| `sycl-toolchain` cold (3640 steps: LLVM, clang, the tools, libdevice, libsycl, UR, UMF) | 16 min |
| the whole recipe again after a patch (`patch configure compiler runtime stage`, incremental) | 1 min 47 s |
| `sycl_hello` compiled ahead of time for `bmg` + SPIR-V (Rosetta clang, Rosetta ocloc) | 10 s |

The time-box (build only the runtime if LLVM took more than an hour) was not needed: the compiler is built, no prebuilt
clang is used. Results (`$SYCL_WORK/build`):

| What | Size | Note |
|---|---|---|
| `bin/clang++` (`clang-23`, "DPC++ compiler 7.2.0", clang 23.1.1) | 109 MB | `-fsycl`, `-fsycl-targets=spir64` (SPIR-V) and `spir64_gen` (ahead of time through `ocloc`) |
| `bin/` `llvm-spirv`, `sycl-post-link`, `clang-offload-wrapper`, `clang-offload-bundler`, `clang-linker-wrapper`, `file-table-tform`, `llvm-foreach`, `llvm-link`, `spirv-to-ir-wrapper`, `opencl-aot`, `sycl-ls` | | the tools the driver runs |
| `lib/dpcpp-7/sycl/*.bc` (20), `lib/libsycl-devicelib-host.a` | | libdevice: the device libraries (libc, math, bfloat16, ITT, sanitizers) and their host part |
| `lib/libsycl.9.dylib` | 4.6 MB | the SYCL runtime; the UR loader is linked in statically (`libur_loader.a`) and finds the adapters next to libsycl |
| `lib/libur_adapter_level_zero.0.dylib`, `libur_adapter_level_zero_v2.0.dylib` | 2.2, 2.1 MB | UR's Level Zero adapters; v2 is the default for Battlemage (`isBMGorNewer`), `UR_LOADER_USE_LEVEL_ZERO_V2=0` takes v1 |
| `lib/libumf.1.dylib` | 0.2 MB | UMF: the USM pools the adapters allocate through |

## 3. What macOS needed (the patches)

`compute/patches/intel-llvm/` (on `v7.2.0`) and `compute/patches/umf/` (on `v1.2.0`), applied with `git am` on a branch
`intelxe` by `build-sycl.sh patch`, like NEO's series:

| Patch | Linux/ELF assumption | macOS |
|---|---|---|
| intel-llvm 0001 | `clang-offload-wrapper` puts the device images in `__CLANG_OFFLOAD_BUNDLE__<kind>-<triple>`, the registration constructors in `.text.startup`, the image info in `.tgtimg`: not valid Mach-O section specifiers (`segment,section`), so the wrapper module fails in the back end | no section on Mach-O; the runtime gets the images from the descriptor the constructor registers (`__sycl_register_lib`), not from sections |
| intel-llvm 0002 | the Linux and Windows drivers add libsycl to an `-fsycl` link (`addOffloadRTLibs`); the Darwin linker job never asked | `AppleMachO::addOffloadRTLibs`: `-L<lib> -lsycl` and `libsycl-devicelib-host.a` |
| intel-llvm 0003 | clang's SPIR-V builtin declarations take "the first 64-bit integer type" (`long`), while the SYCL headers pass `[u]int64_t`, the host's type: the same on Linux (`long`) and Windows (`long long`), different on a Darwin host (`int64_t` is `long long`), so every 64-bit builtin (`__spirv_ocl_nan`, `__spirv_GroupIAdd`, ... used by `sycl.hpp` itself) was ambiguous | the builtins' 64-bit types are the target's `Int64Type` (which the SYCL device target copies from the host): unchanged on Linux and Windows. The SPIR-V names are then mangled with `x`/`y`, as on Windows, which the translator and IGC already read |
| intel-llvm 0004 | `size_t` and `uint64_t` are one type (`std::max` of both in the UR Level Zero adapter) | `std::max<uint64_t>`; the same width difference as NEO's 0001 and IGC's 0005 |
| intel-llvm 0005 | libdevice's ASan runtime declares `__AsanLaunchInfo` as `uint64_t *` in a header and defines it as `uptr *` (one type on Linux) | one type, `uptr` |
| umf 0001 | hwloc (fetched from git and built with autotools) for the NUMA topology and memory binding | `UMF_HWLOC_STANDIN=ON`: `src/hwloc_macos/hwloc.h`, one NUMA node with every CPU and the machine's memory, binding fails with `ENOSYS` (what hwloc's Darwin backend reports; UMF's OS provider tolerates it) |
| umf 0002 | the Level Zero provider `dlopen`s `libze_loader.so.1` | `libze_loader.1.dylib` (without it every USM pool failed: `UR_RESULT_ERROR_UNKNOWN` at the first allocation) |

What was already there and needed nothing: the runtime's Darwin branches (`os_util`: the library's own directory
through `dladdr`, memory size through `sysctl`), the UR loader's Mach-O adapter names (`libur_adapter_level_zero.0.dylib`)
and adapter search next to the loader, the Unified Runtime's own macOS checks (no sanitizer layer on Apple).

Build-side settings rather than patches: the combined Level Zero include directory; `UR_ENABLE_SYMBOLIZER=OFF` (UR's CMake
forces the symbolizer on, then refuses it without the sanitizer layer, which it turns off on Apple); the linker named by
path when the compiler runs under Rosetta (`--ld-path=$(xcrun -f ld)`: `/usr/bin/ld` is a shim that loads `libxcrun`,
which has no x86_64 slice); in the stage, the adapters' and UMF's build-tree rpaths replaced by `@loader_path`.

## 4. The test and the night run

`user/tests/sycl/sycl_hello.cpp`, SYCL 2020 (and, since 13 Nov, Intel's device-info extension), each step printed
PASS/FAIL: (1) platforms, devices and the queue's device (backend, compute units, sub-group sizes, memory, USM
aspects), then the max clock, memory clock and bus width and a "free memory" step (`ext_intel_free_memory`, within the
device's memory), then a "sysman" step: the card power limits and the throttle reasons, which the runtime answers only
through Level Zero sysman (SKIP when the device lacks `ext_intel_power_limits`, i.e. `zesInit` failed); (2) vector add, 1M floats,
buffers and accessors; (3) `sycl::reduction` (plus, 64-bit) of 1..262144; (4) USM shared, written by a kernel and read by
the host; (5) USM device with host->device, device->device and device->host `memcpy` around a kernel; (6) `fill` and
`memset` on device memory. `sycl_hello build` runs step 1 and then builds every kernel of the program for the device
(`get_kernel_bundle<bundle_state::executable>`) without GPU work.

The stage (`build-sycl.sh stage`, here `~/hackintosh/work/night1214/sycl`, 9.3 MB): `lib/` (libsycl, both adapters,
UMF; ad-hoc signed), `bin/sycl-ls`, `bin/sycl_hello` (SPIR-V plus an ahead-of-time Battlemage image compiled by the macOS
`ocloc` from build-igc.sh's stage), `bin/sycl_hello_jit` (SPIR-V only), `night-sycl.sh`, `SHA256SUMS`. The Level Zero
loader and driver are not in it: they come from the L0 stages already on the test machine.

Device code without the compiler's JIT: the ahead-of-time image (`-fsycl-targets=spir64_gen,spir64 -Xsycl-target-backend=spir64_gen "-device bmg"`)
lets the stage-2 package `~/l0`, which has no IGC, run SYCL kernels (the runtime picks the native image on a matching
device, `zeModuleCreate` with `ZE_MODULE_FORMAT_NATIVE`); `sycl_hello_jit` needs IGC in the driver (`~/l0igc`).

Checked on the build Mac against xemac's canned Battlemage (`XEMAC_FAKE=1`, Rosetta; nothing executes there):

| Run | L0 package | Result |
|---|---|---|
| `sycl-ls --verbose` | either | `[level_zero:gpu] Intel(R) oneAPI Unified Runtime over Level-Zero V2, Intel(R) Arc(TM) B580 Graphics 20.1.0 [1.18.0]`, architecture `intel_gpu_bmg_g21`, sub-groups 16 and 32, USM, fp64, atomic64 |
| `sycl_hello build` | without IGC (`l0ocl` stage = `~/l0`) | PASS: 10 kernels from the native image |
| `sycl_hello build`, `UR_LOADER_USE_LEVEL_ZERO_V2=0` | without IGC | PASS (the v1 adapter, platform "Unified Runtime over Level-Zero") |
| `sycl_hello_jit build` | with IGC (build-igc.sh's stage = `~/l0igc`) | PASS: 10 kernels, IGC's dumps show the SPIR-V compiled for Xe2 |
| `sycl_hello_jit build` | without IGC | FAIL, as expected: no compiler for the SPIR-V |
| `sycl_hello` | either | step 1 PASS, then the first copy waits for a completion fake mode never gives |

The night snippet `user/tests/sycl/night-sycl.sh` (phase Y, `phase_sycl`; the stage copied to `~/b580/sycl`): Y0 the
checksums; Y1 `sycl-ls`; Y2 `sycl_hello build` over `~/l0`; Y3 `sycl_hello` through the v2 adapter over `~/l0` (from
`~/l0/builtins`, for NEO's built-in kernels); Y4 the same through the v1 adapter; Y5 `sycl_hello_jit` over `~/l0igc`.
Each run is `tmo 60` in a subshell with `ONEAPI_DEVICE_SELECTOR=level_zero:gpu` and `DYLD_LIBRARY_PATH=<sycl>/lib:<l0>/lib`
exported (no env/perl wrapper); a failed run is repeated with `SYCL_UR_TRACE=2`, the UR loader's and adapter's debug
logs and NEO's `PrintDebugMessages`/`PrintXeLogs`, keeping the last 120 lines and the gzipped full log. The whole phase
ran on the build Mac against the canned device with the expected results above (Y1, Y2 pass; Y3-Y5 time out after the
device query).

## 5. Licences

Nothing here is in a release package; the stage goes to the test machine only. What it contains: libsycl, the UR adapters
and the compiler (intel/llvm: Apache-2.0 WITH LLVM-exception), UMF (Apache-2.0 WITH LLVM-exception), emhash (MIT,
header-only, in libsycl), vc-intrinsics (MIT, in the compiler), SPIRV-Headers (Khronos MIT-style). The patches in
`compute/patches/intel-llvm` and `compute/patches/umf` are under those projects' licence (`docs/LICENSING.md`, `compute/`).
Shipping the runtime with an application would need the Apache-2.0 text and the projects' notices beside it.

## 6. What is left

- **The first B580 run** (the night snippet): kernels, USM and copies through the UR adapters on the hardware. The
  adapters use more of NEO than `ze_hello` does: immediate command lists, counter-based events (v2), USM pools
  through UMF, `zeCommandListAppendMemoryFill` for `fill`/`memset`, 64-bit atomics for the reduction. Both adapters are
  staged so a v2-only failure can be told apart.
- **Sysman: done on the build Mac (13 Nov, branch `l0sysman`, `DESIGN-level-zero.md` section 10).** For Battlemage the
  adapter uses `zesInit` (not `ZES_ENABLE_SYSMAN`), which failed on macOS until NEO patch 0013 (the device's sysfs
  answered from the kext, `debug.intelxe.sensors_raw`). Against the canned device: `sycl-ls` now lists
  `ext_intel_power_limits`, `max_clock_frequency` is 2850 MHz (was 0), and sycl_hello's sysman step (power limits,
  throttle reasons) passes through both adapters; the free memory (12042 of 12288 MB) already worked, from NEO's
  usable-memory extension rather than sysman. B580 run: `user/tests/compute/night-sysman.sh` (Z6-Z8).
- **`sycl_hello.cpp` was not in the repository** (the `.gitignore` rule for `user/tests` let only `.c`/`.sh`/... in; the
  13 Nov merge has `night-sycl.sh` and `build-sycl.sh stage` compiling a file that was not committed, and no copy was
  left). It was written again on 13 Nov from section 4 (same steps and output, plus the free-memory and sysman steps)
  and `.gitignore` now admits `user/tests/sycl/*.cpp`. The night stage `night1214/sycl` holds binaries of the first one.
- **Runtime compilation of OpenCL C** (`kernel_compiler`, `source_language::opencl`): libsycl loads ocloc as
  `libocloc.so` (`sycl/source/detail/kernel_compiler/kernel_compiler_opencl.cpp`, `loadOclocLibrary`); build-neo.sh's
  IGC stage has `lib/libocloc.dylib`. An `__APPLE__` branch naming `libocloc.dylib` there is the whole change (a sixth
  intel-llvm patch and a libsycl rebuild, ~1 min incremental); not done on 13 Nov because the SYCL build tree is
  shared with other work. SPIR-V source (`source_language::spirv`) needs no ocloc and should already work.
- **The compiler on the build Mac runs under Rosetta** (x86_64, as IGC); a native arm64 clang for the build Mac would
  need a second, host LLVM tree (the runtime stays x86_64). Not needed for the test machine, which runs the x86_64 one.
- Not built: sycl-jit (kernel fusion, `SYCL_ENABLE_EXTENSION_JIT`), XPTI tracing and its framework (needs
  parallel-hashmap, another fetch), the OpenCL adapter (NEO's OpenCL works through the Khronos loader; enabling it is a
  `SYCL_ENABLE_BACKENDS` entry), oneAPI libraries (oneMKL, oneDPL, oneDNN), `-fsycl` on Mach-O through the new offload
  driver (`--offload-new-driver`, clang-linker-wrapper: ELF-only sections), and the runtime compilation extension
  (`kernel_compiler`, which looks for `libocloc.so`).
- The sanitizer device libraries build but `-fsanitize=address` for SYCL needs UR's sanitizer layer, which UR disables on
  macOS.
