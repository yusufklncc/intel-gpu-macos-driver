# SYCL on macOS: Intel's DPC++ runtime over our Level Zero (design, 2026-11-13)

> Design note with its date: it records what was chosen, built and checked that day. Status: **first B580 run 13 Nov**:
> `sycl-ls`, the device query and the vector add pass through both Level Zero adapters and the IGC JIT; the reduction
> then called a null pointer, a C++ calling-convention difference between this clang and Apple's (section 3.1, fixed by
> intel-llvm patch 0006, checked on the build Mac; its B580 run is the next night). **13 Nov evening (branch `sycl2`,
> section 7):** run-time compilation of SYCL source (sycl-jit), XPTI tracing (`sycl-trace`, `sycl-prof`), `-fsycl -c`
> with a separate link (any CMake project), oneDPL; checked against the canned device, B580 run in the night snippet
> (Y6-Y10). oneDNN v3.14.2 builds (10 min) and creates its primitives (section 7.5); oneMKL needs Intel's closed library.

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
| intel-llvm 0006 | (none: an ABI question of the platform) | this clang decides how a C++ record is passed by the C++11 rule; Apple's clang, which builds libsycl here and every C++ library of the platform, by the Clang 4 rule on Darwin. A record whose copy and move constructors are all deleted (`type_erased_cgfo_ty`) went by address from the program and was read from two registers by libsycl: the reduction crash of section 3.1. `getCallingConvKind` gives the Clang 4 rule on Darwin targets |
| intel-llvm 0007 | the runtime compilation of OpenCL C (`kernel_compiler`) loads `libocloc.so` | `libocloc.dylib` on Darwin (section 6) |
| intel-llvm 0008 | libsycl loads sycl-jit as `libsycl-jit.so` by leaf name (Linux finds it through libsycl's RUNPATH); sycl-jit hides LLVM with a linker version script, not used on Apple; sycl-prof, sycl-sanitize and sycl-trace's collector name `.so` libraries | `libsycl-jit.dylib` from libsycl's own directory (a leaf-name `dlopen` does not search the caller's rpaths on Darwin), as on Windows; an `-exported_symbols_list` with the same six entry points (without it libsycl-jit exported all of LLVM 23, weak definitions included, which dyld coalesces with LLVM 22's from IGC's `libigdfcl` in the same process); `.dylib` names for the XPTI dispatcher and collectors (section 7) |
| intel-llvm 0009 | `clang -fsycl -c` writes a fat object through `llvm-objcopy`: ELF section names `__CLANG_OFFLOAD_BUNDLE__<triple>` flagged `exclude` | Mach-O has neither (16-character `segment,section` names; llvm-objcopy refuses `--set-section-flags` there), so every separate compile failed. For a Mach-O host object the bundler writes its own binary bundle format, which the `-fsycl` link unbundles (section 7.3) |
| umf 0001 | hwloc (fetched from git and built with autotools) for the NUMA topology and memory binding | `UMF_HWLOC_STANDIN=ON`: `src/hwloc_macos/hwloc.h`, one NUMA node with every CPU and the machine's memory, binding fails with `ENOSYS` (what hwloc's Darwin backend reports; UMF's OS provider tolerates it) |
| umf 0002 | the Level Zero provider `dlopen`s `libze_loader.so.1` | `libze_loader.1.dylib` (without it every USM pool failed: `UR_RESULT_ERROR_UNKNOWN` at the first allocation) |
| onednn 0001 | oneDNN `v3.14.2` looks up Level Zero and OpenCL symbols with `dlopen` under `__linux__` only, and nGEN (its kernel generator) names `libze_loader.so.1` and `libOpenCL.so.1` | the same on Apple, `libze_loader.1.dylib` and `libOpenCL.1.dylib`; without nGEN's name the engine creation threw `level_zero_error` `ZE_RESULT_ERROR_UNINITIALIZED` (section 7.5) |
| onednn 0002 | `size_t` and `uint64_t` are one type (`std::min` of both; `size_t` kernel arguments for `ulong` parameters) | `uint64_t` / `std::min<size_t>` in the two places the compiler found |

What was already there and needed nothing: the runtime's Darwin branches (`os_util`: the library's own directory
through `dladdr`, memory size through `sysctl`), the UR loader's Mach-O adapter names (`libur_adapter_level_zero.0.dylib`)
and adapter search next to the loader, the Unified Runtime's own macOS checks (no sanitizer layer on Apple).

Build-side settings rather than patches: the combined Level Zero include directory; `UR_ENABLE_SYMBOLIZER=OFF` (UR's CMake
forces the symbolizer on, then refuses it without the sanitizer layer, which it turns off on Apple); the linker named by
path when the compiler runs under Rosetta (`--ld-path=$(xcrun -f ld)`: `/usr/bin/ld` is a shim that loads `libxcrun`,
which has no x86_64 slice); in the stage, the adapters' and UMF's build-tree rpaths replaced by `@loader_path`.

### 3.1 The reduction crash (B580, 13 Nov)

The first B580 run (night 13 Nov, Y3-Y5) passed the device query and the vector add through the v2 adapter, the v1
adapter and the IGC JIT alike, then all three stopped with `EXC_BAD_ACCESS` at address 0 in the reduction: frame 0 a
null address, frame 1 `HandlerAccess::preProcess(handler&, type_erased_cgfo_ty)` in libsycl, frame 2 the program's
`reduction_impl_algo<...>::getGroupsCounterAccDiscrete`. On a discrete GPU the reduction zeroes its group counter with
an extra command group: `getGroupsCounterAccDiscrete` (`sycl/include/sycl/reduction.hpp`) hands a lambda to
`HandlerAccess::preProcess` (`sycl/source/handler.cpp`), which builds a second handler and calls the lambda through the
type-erased wrapper `type_erased_cgfo_ty` (`handler.hpp`: an object pointer and an invoker function pointer, its copy and
move constructors all deleted), passed **by value**.

The two sides disagreed on how that value is passed. The disassembly: the program (built by this clang) puts the
wrapper in its frame and passes its address in `%rsi`; libsycl (built by the build Mac's `/usr/bin/c++`, Apple clang
21: the runtime's CMake uses the system compiler) takes the object pointer from `%rsi` and the invoker from `%rdx` and
calls `*%rdx`, which held 0. Clang has two rules for passing a C++ record in registers (`canPassInRegisters`,
`SemaDeclCXX.cpp`): the C++11 one (each copy/move constructor and the destructor trivial or deleted, **and at least one
copy or move constructor not deleted**) and the Clang 4 one (trivial copy constructor and destructor; moves not
considered), which upstream keeps for `-fclang-abi-compat=4` and PS4. Apple's clang uses the Clang 4 rule on Darwin.
Eleven records (combinations of deleted, defaulted and user-provided copy and move constructors, a non-trivial
destructor), compiled by both: on x86_64 Apple clang's signatures equal upstream clang's with `-fclang-abi-compat=4.0`
on all eleven and differ from upstream's default on six (all deleted, twice; copy deleted with a user-provided move;
copy deleted, move undeclared; move deleted; trivial copy with a user-provided move: Apple in registers, upstream by
address); on arm64 the all-deleted record differs the same way. Across the boundary
(`user/tests/sycl/darwin_abi.cpp`: the callee built by Apple's clang, the caller by the DPC++ clang, each record two
`long`s) the 13 Nov clang's program read garbage for all five kinds. Nothing is wrong in libsycl, the program or the
linking (no weak or interposed symbol: the null was a register the callee read and the caller never wrote); on Linux
both sides are built by compilers with the C++11 rule (gcc or clang), so the wrapper always goes by address there.

Fix: intel-llvm patch 0006, `TargetInfo::getCallingConvKind` gives the Clang 4 rule for Darwin triples, so programs
built by this clang pass records exactly as Apple's clang does, for libsycl and for libc++ and every other C++ library
of the platform. The SPIR-V device side (target `spir64`) is unchanged. After it, the eleven records give identical
signatures from both compilers on x86_64 and arm64, `darwin_abi` prints six "ok", and under lldb `preProcess` gets the
object in `%rsi` and the reduction lambda's invoker in `%rdx`.
The other way (libsycl built by this clang) was not taken: it would leave programs, libsycl and libc++ on different
rules, and needs a two-stage build.

Reproduced and checked on the build Mac against the canned device (`XEMAC_FAKE=1`, Rosetta; `sycl_hello reduction`
runs the query and then only the reduction): with the 13 Nov compiler, segmentation fault in the reduction, under
lldb the same three frames as on the B580 and `%rdx` = 0; with patch 0006, the reduction submits its command groups
and waits for the GPU, which fake mode never answers (`sample`: the main thread in `urEventWait` under the
reduction's `handler::finalize`; killed by the time-out, no crash), through the v2 and the v1 adapter;
`sycl_hello build` and `sycl_hello_jit build` (over the IGC stage) still pass with 10 kernels.

## 4. The test and the night run

`user/tests/sycl/sycl_hello.cpp`, SYCL 2020 (and, since 13 Nov, Intel's device-info extension), each step printed
PASS/FAIL: (1) platforms, devices and the queue's device (backend, compute units, sub-group sizes, memory, USM
aspects), then the max clock, memory clock and bus width and a "free memory" step (`ext_intel_free_memory`, within the
device's memory), then a "sysman" step: the card power limits and the throttle reasons, which the runtime answers only
through Level Zero sysman (SKIP when the device lacks `ext_intel_power_limits`, i.e. `zesInit` failed); (2) vector add, 1M floats,
buffers and accessors; (3) `sycl::reduction` (plus, 64-bit) of 1..262144; (4) USM shared, written by a kernel and read by
the host; (5) USM device with host->device, device->device and device->host `memcpy` around a kernel; (6) `fill` and
`memset` on device memory. `sycl_hello build` runs step 1 and then builds every kernel of the program for the device
(`get_kernel_bundle<bundle_state::executable>`) without GPU work. `sycl_hello vector|reduction|usm_shared|usm_device|fill`
(one or more) runs step 1 and only those steps; each step's name is printed before it runs, so a crash names its step.

The stage (`build-sycl.sh stage`, here `~/hackintosh/work/night1214/sycl`, 9.3 MB; with patches 0006 and 0007,
13 Nov: `~/hackintosh/work/syclred/stage`, built from a fresh work tree `~/hackintosh/work/syclred` in 16 min): `lib/` (libsycl, both adapters,
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
| `sycl_hello reduction` (13 Nov, patch 0006), v2 and v1 | without IGC | step 1 PASS, the reduction's command groups submitted (its group-counter `memset` through `preProcess` included), then the wait for the GPU; before 0006: segmentation fault in `preProcess` |

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
- **Runtime compilation of OpenCL C** (`kernel_compiler`, `source_language::opencl`): libsycl loaded ocloc as
  `libocloc.so` (`sycl/source/detail/kernel_compiler/kernel_compiler_opencl.cpp`, `loadOclocLibrary`); build-neo.sh's
  IGC stage has `lib/libocloc.dylib`. Intel-llvm patch 0007 (13 Nov) names `libocloc.dylib` on Darwin, found through
  `DYLD_LIBRARY_PATH` (the IGC stage's `lib/`). Checked on the build Mac against the canned device over the IGC stage:
  `device::ext_oneapi_can_build(source_language::opencl)` is true (false before the patch) and
  `create_kernel_bundle_from_source` + `build` of a one-line OpenCL C kernel gives a bundle with that kernel. Not in
  the night snippet yet. SPIR-V source (`source_language::spirv`) needs no ocloc. **Since 13 Nov evening** with SYCL
  source too, and in the night snippet (Y7, section 7.1).
- **The compiler on the build Mac runs under Rosetta** (x86_64, as IGC); a native arm64 clang for the build Mac would
  need a second, host LLVM tree (the runtime stays x86_64). Not needed for the test machine, which runs the x86_64 one.
- Built since 13 Nov evening (section 7): sycl-jit, XPTI tracing and its framework, oneDPL, separate compilation
  (`-fsycl -c`). Still not built: the OpenCL adapter (NEO's OpenCL works through the Khronos loader; enabling it is a
  `SYCL_ENABLE_BACKENDS` entry), oneMKL/oneMath (section 7.5), `-fsycl` on Mach-O through the new offload driver
  (`--offload-new-driver`, clang-linker-wrapper: ELF-only sections).
- The sanitizer device libraries build but `-fsanitize=address` for SYCL needs UR's sanitizer layer, which UR disables on
  macOS.

## 7. On top of the runtime (13 Nov evening, branch `sycl2`)

Built in a fresh work tree (`~/hackintosh/work/sycl2`, the 13 Nov sources cloned) with `build-sycl.sh`'s new settings:
`xptifw` and `sycl-jit` in the LLVM projects, `SYCL_ENABLE_XPTI_TRACING=ON`, `SYCL_ENABLE_EXTENSION_JIT=ON`,
`UR_ENABLE_TRACING=ON` (the UR loader's tracing layer), and two new pins: parallel-hashmap `8a889d3699b3` (the commit
xptifw's CMake names, v1.3.12, Apache-2.0) and oneDPL `oneDPL-release-2022.14.0` `66efa97efe01` (Apache-2.0 WITH
LLVM-exception), each checked by commit and tree SHA-256 like the others. Upstream's own configure script leaves
sycl-jit and libdevice out on Darwin; both build here.

| Step (build Mac, M1 Pro, other builds running, load 15-80) | Time |
|---|---|
| `fetch patch configure` (sources present) | 31 s |
| `compiler runtime` cold, now 5100 steps (sycl-jit adds the clang libraries it links, xptifw, the collectors) | about 35 min of building, in three runs (two were interrupted) |
| `stage` (sycl_hello twice, sycl_libs with an AOT Battlemage image of oneDPL's 61 kernels, the SPIR-V kernel, the header subset) | 2 min 28 s |
| `sycl_libs` alone, SPIR-V only / with the AOT image | 18 s / about 1.5 min |

### 7.1 Run-time compilation (`sycl_ext_oneapi_kernel_compiler`)

`device::ext_oneapi_can_build` now answers true for all three languages on the B580 (canned): SPIR-V (no compiler
needed by the runtime: the module goes to `zeModuleCreate`, i.e. IGC in the driver), OpenCL C (ocloc, patch 0007) and
SYCL (sycl-jit). sycl-jit is `libsycl-jit.dylib` (105 MB: clang, LLVM, the SPIR-V translator, the SYCL headers and
libdevice embedded as a virtual file system); libsycl loads it from its own directory (patch 0008) and finds the six
entry points, which are now the library's only exports. A SYCL source string is compiled in the process as the
driver's `-fsycl-device-only` compile does, with the program's host target (x86_64 Darwin) as the auxiliary triple.
That compile includes the platform's C++ headers (libc++, the C library), so it needs a macOS SDK. The build Mac has
one (the compiler's `DEFAULT_SYSROOT`); the test machine has none (no command line tools). The test passes
`-isysroot $SYCL_RTC_SYSROOT` as a build option, and the stage carries `rtc-sysroot/`: the 851 SDK headers
(7.2 MB) that a device-only compile of the test's kernel source includes, plus `SDKSettings.json`
(`build-sycl.sh rtc_sysroot`, from the compile's dependency list). With an empty directory instead the SYCL build
fails ("Device compilation failed"), so the subset is what is used. A program with other host includes in its kernel
source would need those headers too, or an SDK on the machine.

`user/tests/sycl/sycl_libs.cpp`, `rtc` (and `rtc-build` without GPU work): the three `can_build` answers, then one
kernel from each language, built with `syclex::build` and run on USM: `rtc_spv_scale` from `share/rtc_kernels.spv`
(`kernels/rtc_kernels.cl`, compiled at stage time by the DPC++ clang for spir64 and `llvm-spirv`), `rtc_cl_scale` from
OpenCL C source, `rtc_sycl_iota` from SYCL source (a free-function `nd_range_kernel`). Against the canned device over
the IGC stage, `rtc-build`: PASS for all four steps. Under Rosetta the first run after a build takes about a minute
(Rosetta translating libsycl-jit and IGC; 2.7 s of user time), later runs 2.8 s in all.

### 7.2 XPTI tracing

libsycl and the UR loader now emit XPTI trace points; `libxptifw.dylib` is the dispatcher and the collectors are
`libsycl_ur_trace_collector.dylib` (sycl-trace), `libsycl_profiler_collector.dylib` (sycl-prof) and
`libze_trace_collector.dylib`. The tools set `XPTI_FRAMEWORK_DISPATCHER`, `XPTI_SUBSCRIBERS`, `XPTI_TRACE_ENABLE` and
`UR_ENABLE_LAYERS=UR_LAYER_TRACING` and `execve` the program (its `DYLD_LIBRARY_PATH` passes through: neither the tools
nor the test programs are protected binaries); the leaf names are found through `DYLD_LIBRARY_PATH` (the stage's
`lib/`). Against the canned device, `sycl-trace --sycl --ur.call sycl_hello build`: 83 `[UR]` calls with their
arguments and results, the `[SYCL]` queue create/destroy events, and the program's own PASS lines; `sycl-prof -o
prof.json sycl_hello build`: a 23 KB Chrome trace with 177 events. `sycl-trace --level_zero` prints nothing on any
platform at this tag: its collector listens for the `sycl.experimental.level_zero.debug` stream, which nothing in
v7.2.0 emits any more (the old PI Level Zero plugin did; the UR adapter does not). Level Zero calls are traced by the
Level Zero loader's own layers instead (`ZE_ENABLE_TRACING_LAYER`, build-neo.sh's loader has them).

### 7.3 Separate compilation (`-fsycl -c`)

Until now a SYCL program had to be compiled and linked in one command: `clang++ -fsycl -c` failed in the offload
bundler (llvm-objcopy: "option is not supported for MachO"), which ruled out every CMake or make project. With patch
0009 the object of a `-fsycl -c` compile is the bundler's binary container (magic `__CLANG_OFFLOAD_BUNDLE__`, a host
bundle `host-x86_64-apple-macosx15.0.0` with the Mach-O object and a `sycl-spir64-unknown-unknown` bundle with the
device bitcode; `clang-offload-bundler -list` reads it). The `-fsycl` link unbundles it like the Linux fat objects: two
objects compiled separately and linked by `clang++ -fsycl a.o m.o` give a working program. The difference from Linux:
the object is not a Mach-O file, so **every link that takes such objects must be a `clang++ -fsycl` link** (ld64 alone,
or a link without `-fsycl`, reports an unknown file type); CMake projects therefore get `-fsycl` in
`CMAKE_EXE_LINKER_FLAGS`/`CMAKE_SHARED_LINKER_FLAGS` (CMake's own compiler checks link without the compile flags).
Static archives of such objects were not tried (Apple's `ranlib` does not index non-Mach-O members).

### 7.4 oneDPL

Header-only; `-I<oneDPL>/include`. No TBB on macOS here: oneDPL's host policies fall back to its serial backend,
the device policy needs nothing else. `sycl_libs dpl` (1M elements, USM device memory, `make_device_policy(queue)`):
`sort` of int32 (radix sort), `sort` of floats with `std::greater` (merge sort), `reduce` of int64, `transform_reduce`
(a dot product into a double), `inclusive_scan`, `count_if` and `find_if`, `copy_if`, each compared with the host's
`std::` result. The program instantiates 61 kernels; `sycl_libs dpl-build` builds all of them for the canned B580 from
the AOT image without IGC (`~/l0`) and from SPIR-V through IGC (1.5 min under Rosetta). ocloc compiles them ahead of
time without errors (a few IGC "RetryManager" notes).

### 7.5 oneDNN and oneMKL

**oneDNN `v3.14.2` (`4db284662a37`, Apache-2.0; `build-sycl.sh onednn`)** builds and creates its primitives on the
canned B580. Configuration: SYCL GPU runtime, Intel GPUs, no CPU engine (`DNNL_CPU_RUNTIME=NONE`), Xe2 kernels only
(`ONEDNN_ENABLE_PRIMITIVE_GPU_ISA=XE2`), no graph API, tests or examples; compiled by the DPC++ clang, which needed
`-fsycl -c` (section 7.3; every oneDNN source is compiled with `-fsycl`). Cost: 579 ninja steps, about 10 minutes on 8
jobs with other builds running (57 min of CPU under Rosetta), 25 MB `libdnnl.3.14.dylib`, an 89 MB build tree,
119 MB of sources. What macOS needed (patches `compute/patches/onednn/`): the Level Zero and OpenCL loaders opened by
their macOS names, in oneDNN's `xpu` layer and in nGEN (without the latter the first primitive threw
`ZE_RESULT_ERROR_UNINITIALIZED`: nGEN found no library), and three `size_t`/`uint64_t` mix-ups; CMake needs `-fsycl` in the
linker flags (section 7.3) and `MACOSX_DEPLOYMENT_TARGET=15.0` in the environment, because a link-only `-fsycl` command
ignores `-mmacosx-version-min` (the dylib came out as "minimum macOS 27", the build Mac's, which the test machine's
macOS 26 would refuse; the driver skips `AddDeploymentTarget` when no architecture is bound, which the SYCL link-only
path does not bind; the environment variable is honoured).

At run time oneDNN generates most GPU kernels itself (nGEN: Xe2 binaries, loaded with `zeModuleCreate` from native
code, no compiler involved) and compiles its OpenCL C kernels through **an OpenCL device**, the one whose UUID equals the
Level Zero device's (`sycl_dev2ocl_dev`), i.e. NEO's OpenCL through the Khronos loader (`OCL_ICD_FILENAMES=<IGC
stage>/lib/libigdrcl.dylib`). Without it: "OpenCL device not found" and no primitive. (oneDNN's other route, OpenCL C
through kernel_compiler and ocloc, is its `ONEDNN_EXPERIMENTAL_SYCL_KERNEL_COMPILER` build option, not used.)
`user/tests/sycl/onednn_hello.cpp`: a GPU engine on the SYCL queue's device, then eltwise ReLU (1M floats), matmul f32
192x160x224 and a 3x3 convolution (1x16x32x32 to 32 channels), each checked against a host reference; `build` creates
the three primitives without running them. Against the canned device over the IGC stage: four PASS, the engine reports
"backend:Level Zero ... binary_kernels:enabled", the matmul is `jit:gemm:any` and the convolution `jit:ir` (both nGEN),
3.5 s. (oneDNN reports itself as 3.14.1: the tag's version file was not bumped.)

**oneMKL / oneMath: not built.** The open oneMath interfaces (`uxlfoundation/oneMath`) reach Intel GPUs only through its
`mklgpu` backend, which wraps Intel's closed oneMKL SYCL library (Linux and Windows binaries, no macOS); its generic
SYCL BLAS backend (the former portBLAS, a further fetch) would run on any SYCL device but is a tuning project of its own.
oneDNN's matmul (above) already gives GEMM on the B580.

### 7.6 Native x86_64 on the B580 machine, Rosetta on the build Mac

Everything here is x86_64: the compiler, its tools, libsycl, libsycl-jit, IGC and ocloc. On the Apple-silicon build Mac
they run under Rosetta: the first start of each large library after a build waits for Rosetta's translation (about a
minute for libsycl-jit plus IGC, ~30 s for ocloc's 250 MB; the translation is cached afterwards), the user time of the
work itself is small (sycl-jit's compile of the test kernel and IGC's backend: 2.7 s), and the compiler builds the stage
in 2.5 minutes. The linker must be named (`--ld-path=$(xcrun -f ld)`) because `/usr/bin/ld` is a shim with no x86_64
slice. On the B580 machine the same binaries run natively: no translation, and the run-time compilers (sycl-jit,
ocloc, IGC) start at once; that machine has no SDK and no command line tools (anything that runs `xcrun` or a
developer tool there opens the install dialog), so the stage carries what the run-time SYCL compile needs
(`rtc-sysroot/`, section 7.1) and nothing is compiled ahead of time there. Building on the B580 machine itself would
need the command line tools installed there; it is not needed.

### 7.7 Stage and night lines

`build-sycl.sh stage` (`~/hackintosh/work/sycl2/stage`, 149 MB, 1-2.5 min): as before, plus `lib/` libsycl-jit, libxptifw,
the three collectors and libdnnl, `bin/` `sycl_libs` (SPIR-V + AOT bmg), `onednn_hello`, `sycl-trace`, `sycl-prof`,
`share/rtc_kernels.spv`, `rtc-sysroot/`. The night snippet (phase Y) adds:

| Line | What | Expected on the B580 |
|---|---|---|
| Y6 | `sycl_libs dpl` over `~/l0` (AOT image) | seven PASS (the oneDPL steps), `PASS: 0 failure(s)` |
| Y7 | `sycl_libs rtc` over `~/l0igc`, `SYCL_RTC_SYSROOT=<stage>/rtc-sysroot` | `can_build: spirv 1, opencl 1, sycl 1`, four PASS (can_build, SPIR-V, OpenCL C, SYCL source, each kernel run and checked) |
| Y8 | `sycl-trace --sycl --ur.call sycl_hello vector` over `~/l0` | about 100 `[UR]` calls, the `[SYCL]` queue events, `PASS vector add` |
| Y9 | `sycl-prof -o 9-prof.json sycl_hello vector` | a Chrome trace of a few hundred events (gzipped in the results) |

The whole phase ran on the build Mac against the canned device (the staged `night-sycl.sh`, `~/l0` = build-neo.sh's
stage, `~/l0igc` = the IGC stage): Y0 0 not OK, Y1 and Y2 pass as before; Y3-Y5 pass the device query and wait;
Y6 waits at the first oneDPL kernel; Y7 prints `can_build: spirv 1, opencl 1, sycl 1` and its PASS, then waits at the
first launch; Y8 92 `[UR]` calls and 16 `[SYCL]` lines before the wait; Y9 a 26 KB trace with 210 events; Y10 PASS for
the GPU engine, then the wait at the ReLU. Every wait ends with tmo's kill and the debug repeat, as expected where
nothing executes (`sycl_libs rtc-build`, `sycl_libs dpl-build` and `onednn_hello build` are the canned-device checks
of the same code paths, all PASS).
