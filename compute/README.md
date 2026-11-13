# Intel's compute runtime (Level Zero / oneAPI, OpenCL) on macOS

Intel's compute runtime ("NEO", [intel/compute-runtime](https://github.com/intel/compute-runtime)) is the Level Zero
driver under oneAPI/SYCL and Intel's OpenCL driver. Here it runs on macOS over IntelXe.kext: its Linux DRM/xe backend
is built unchanged, and its system calls go to `user/xemac` (the user-space side of the kext's xe uAPI, the same layer
Mesa's ANV uses) instead of a `/dev/dri` render node. Design, kernel interface, blockers and the staged plan:
[docs/design/DESIGN-level-zero.md](../docs/design/DESIGN-level-zero.md).

**Status (12 Nov):** stages 1 and 3b done on the build Mac, stage 2 waiting for its first hardware run.
`libze_intel_gpu.1.dylib` (Level Zero 1.18 driver, Battlemage and Lunar Lake) and the OpenCL ICD `libigdrcl.dylib` build
for x86_64 with gmmlib, the Level Zero loader and the Khronos OpenCL ICD loader, and **Intel's compiler, IGC, builds for
macOS too** (`build-igc.sh`: LLVM 22.1.8 + Clang, opencl-clang, the SPIR-V translator, vc-intrinsics, SPIRV-Tools and
IGC as `libigc.2.dylib`, `libigdfcl.2.dylib`, `libopencl-clang.dylib`). With it, under Rosetta against xemac's canned
Battlemage (`XEMAC_FAKE=1`, no kext): `ocloc` compiles OpenCL C to a Battlemage binary whose code is identical to the
Linux ocloc's, the driver builds kernels from SPIR-V (Level Zero) and from OpenCL C source (OpenCL) at run time, NEO's
built-in kernels (buffer and image) are compiled at build time and embedded, and the device gets IGC's SIP kernel, so it
keeps mid-thread preemption (patch 0008's thread-group fallback is only taken without IGC). Not yet run on the B580.
Without IGC (`IGC_PREFIX` unset) the build is as before: kernels and built-ins from Intel's Linux `ocloc` in a
container (`ocloc-docker.sh`).

| File | What |
|---|---|
| `build-igc.sh` | IGC for macOS x86_64: fetch the pinned sources (IGC, LLVM, opencl-clang, translator, SPIRV-Tools/Headers, vc-intrinsics), apply the series, build in IGC's "Source" mode, install libigc, libigdfcl, libopencl-clang, libiga64, iga64 and the headers into `$IGC_PREFIX` |
| `build-neo.sh` | fetch the pinned sources, apply the series, build gmmlib, the loader, the IGC headers (or take `$IGC_PREFIX`) and NEO for x86_64, stage the result with `ze_hello`, `cl_hello` (and with IGC: the compiler, `ocloc`, `kernels/`) |
| `ocloc-docker.sh` | Intel's `ocloc` (compute-runtime 26.40, IGC 2.42) in a linux/amd64 container: a `.cl` to a Battlemage binary, or NEO's buffer built-in kernels as files the driver loads from its working directory |
| `build-sycl.sh` | SYCL (Intel's DPC++ `v7.2.0`): fetch and check the pinned sources, apply the series, build clang `-fsycl`, libdevice, libsycl, the Unified Runtime's Level Zero adapters and UMF for x86_64 over build-neo.sh's loader, stage them with `sycl_hello` and `sycl-ls` (`docs/design/DESIGN-sycl.md`) |
| `patches/intel-llvm/`, `patches/umf/` | intel/llvm `v7.2.0`: Mach-O sections in the offload wrapper, libsycl linked by the Darwin driver, the SPIR-V builtins' 64-bit types, two `size_t`/`uint64_t` and type fixes (5 patches); UMF `v1.2.0`: a one-NUMA-node hwloc stand-in, the loader's Mach-O name (2 patches) |
| `../user/tests/sycl/` | `sycl_hello.cpp` (device query, vector add, reduction, USM shared/device, explicit copies, fill/memset; `build`: kernels only) and `night-sycl.sh` (phase Y) |
| `patches/compute-runtime/` | NEO at `248eb54f8441` (2026-10-30, the commit `docs/parity` reads): the macOS port (12 patches) |
| `patches/igc/` | IGC at `0f18b8dc` (the revision NEO's manifest names): CMake for Mach-O/ld64, the Linux headers and `__linux__` paths, Mach-O library names, Apple `ar`/libSystem, `size_t` != `uint64_t` (5 patches) |
| `patches/opencl-clang/` | opencl-clang `ocl-open-220` at `b953ecee`: an exported-symbols list for ld64, the PCH resources found through the library's own path (1 patch) |
| `patches/gmmlib/` | gmmlib `intel-gmmlib-22.10.2`: macOS takes the Linux user-mode paths |
| `patches/level-zero/` | loader `v1.35.0`: Mach-O names, no `dlinfo`, `DYLD_LIBRARY_PATH` |
| `../user/tests/compute/ze_hello.c` | the Level Zero test: enumeration, USM, copy-engine fill/copy (immediate and queue + fence), compute-engine fill/copy (`compute`), a native kernel (`kernel <bin>`), a kernel from SPIR-V compiled by IGC at run time (`spirv <spv>`; `spirv-build`: build only) |
| `../user/tests/compute/cl_hello.c` | the OpenCL test (`OCL=1`), through the Khronos ICD loader: platform and device, a buffer round trip, a native kernel (`kernel <bin>`), a kernel from OpenCL C source (`source <cl>`; `source-build`: build only) |
| `../user/tests/compute/kernels/fill_ids.cl` | the test kernel (`out[i] = i * 3 + 1`) |

Each `patches/*/series` is applied with `git am` on a branch `intelxe` at the pinned revision.

## Build (the build Mac)

With the compiler (IGC built on the Mac; no container):

```sh
brew install bison ccache               # bison >= 3 for IGC (macOS has 2.3); ccache optional, makes rebuilds cheap
export IGC_WORK=~/intelxe-igc L0_WORK=~/intelxe-l0
compute/build-igc.sh                    # IGC + LLVM/Clang etc.: ~14 GB, ~46 min on 8 cores (cold), ~5 min again with ccache
IGC_PREFIX=$IGC_WORK/prefix OCL=1 compute/build-neo.sh   # NEO against it: built-ins compiled and embedded, ocloc; ~5 min
```

`$L0_WORK/stage` then holds everything (`bin/` ze_hello, cl_hello, ocloc; `lib/` the drivers, loaders, gmmlib and the
compiler; `kernels/` fill_ids as source, SPIR-V and Battlemage binary; `SHA256SUMS`) and is copied to the test machine
as one directory. The x86_64 compiler runs under Rosetta on Apple silicon: the first `ocloc` run after a build takes
~30 s (Rosetta translating 250 MB of libraries), later ones ~0.5 s.

Without the compiler (as before IGC was ported):

```sh
pip install ninja                       # or brew install ninja; cmake >= 3.21 and the Xcode command line tools
export L0_WORK=~/intelxe-l0
OCL=1 compute/build-neo.sh              # ~5 min on 8 cores; OCL=1: also the OpenCL ICD, Khronos loader, cl_hello
compute/ocloc-docker.sh kernel user/tests/compute/kernels/fill_ids.cl $L0_WORK/kernels    # docker, linux/amd64
compute/ocloc-docker.sh builtins $L0_WORK/src/neo $L0_WORK/builtins
OCL=1 compute/build-neo.sh stage        # again, to take kernels/ and builtins/ in
```

`$L0_WORK/stage` is then copied to the test machine as one directory (`bin/`, `lib/`, `kernels/`, `builtins/`,
`SHA256SUMS`).

## Run

```sh
cd stage && DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello            # enumeration, USM, copy engine
XEMAC_FAKE=1 DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello init       # no kext (any Mac, x86_64 or Rosetta)
OCL_ICD_FILENAMES=$PWD/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$PWD/lib ./bin/cl_hello              # OpenCL
# with IGC staged:
DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello spirv kernels/fill_ids_bmg.spv                          # SPIR-V JIT
OCL_ICD_FILENAMES=$PWD/lib/libigdrcl.dylib DYLD_LIBRARY_PATH=$PWD/lib ./bin/cl_hello source kernels/fill_ids.cl
./bin/ocloc compile -file kernels/fill_ids.cl -device bmg -out_dir /tmp/oc                           # offline
XEMAC_FAKE=1 DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello spirv-build kernels/fill_ids_bmg.spv          # no kext
```

The driver finds the compiler next to itself in `lib/` (patch 0010); `DYLD_LIBRARY_PATH` is for the Level Zero loader's
driver search and the ICD loader. NEO caches compiled kernels in `~/.cache/neo_compiler_cache`
(`NEO_CACHE_PERSISTENT=0` turns that off). IGC's own debug variables (`IGC_ShaderDumpEnable=1`, dumps in
`/tmp/IntelIGC`) work as on Linux.

macOS's own OpenCL.framework loads no ICDs: an application that should see Intel's OpenCL links the Khronos loader
(`lib/libOpenCL.1.dylib`) instead.

Debugging: `NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1` (NEO), `XEMAC_TRACE=1` (every ioctl),
`ZE_ENABLE_LOADER_DEBUG_TRACE=1` (the loader's driver search).

## SYCL (DPC++)

```sh
export SYCL_WORK=~/intelxe-sycl L0_WORK=~/intelxe-l0          # L0_WORK: an OCL=1 build-neo.sh tree (loader, NEO source)
OCLOC=$IGC_L0_STAGE/bin/ocloc compute/build-sycl.sh            # ~2.5 GB of sources, ~16 min cold; OCLOC: AOT for bmg
cd $SYCL_WORK/stage
export ONEAPI_DEVICE_SELECTOR=level_zero:gpu DYLD_LIBRARY_PATH=$PWD/lib:$HOME/l0/lib   # export, not env (DYLD_*)
./bin/sycl-ls --verbose
(cd ~/l0/builtins && $OLDPWD/bin/sycl_hello)                   # AOT image: runs on an L0 stage without IGC
XEMAC_FAKE=1 ./bin/sycl_hello build                            # no kext: device query and kernel builds only
```

The compiler is `$SYCL_WORK/build/bin/clang++` (x86_64; Rosetta on Apple silicon, where the linker must be named:
`--ld-path=$(xcrun -f ld)`). `UR_LOADER_USE_LEVEL_ZERO_V2=0` selects the v1 adapter (v2 is the Battlemage default);
`SYCL_UR_TRACE=2` and `UR_LOG_LEVEL_ZERO="level:debug;output:stdout"` trace the runtime and the adapter.
