# Intel's compute runtime (Level Zero / oneAPI, OpenCL) on macOS

Intel's compute runtime ("NEO", [intel/compute-runtime](https://github.com/intel/compute-runtime)) is the Level Zero
driver under oneAPI/SYCL and Intel's OpenCL driver. Here it runs on macOS over IntelXe.kext: its Linux DRM/xe backend
is built unchanged, and its system calls go to `user/xemac` (the user-space side of the kext's xe uAPI, the same layer
Mesa's ANV uses) instead of a `/dev/dri` render node. Design, kernel interface, blockers and the staged plan:
[docs/design/DESIGN-level-zero.md](../docs/design/DESIGN-level-zero.md).

**Status (12 Nov):** stage 1 done, stage 2 waiting for its first hardware run. `libze_intel_gpu.1.dylib` (Level Zero
1.18 driver, Battlemage and Lunar Lake) builds for x86_64 with gmmlib and the Level Zero loader; under Rosetta against
xemac's canned Battlemage (`XEMAC_FAKE=1`, no kext) it initializes, enumerates the B580, creates a context and makes
device, host and shared USM allocations. Not yet run on the B580. No compiler: kernels come as native binaries
compiled by Intel's `ocloc` on Linux (`ocloc-docker.sh`).

| File | What |
|---|---|
| `build-neo.sh` | fetch the pinned sources, apply the series, build gmmlib, the loader, the IGC headers and NEO for x86_64, stage the result with `ze_hello` |
| `ocloc-docker.sh` | Intel's `ocloc` (compute-runtime 26.40, IGC 2.42) in a linux/amd64 container: a `.cl` to a Battlemage binary, or NEO's buffer built-in kernels as files the driver loads from its working directory |
| `patches/compute-runtime/` | NEO at `248eb54f8441` (2026-10-30, the commit `docs/parity` reads): the macOS port |
| `patches/gmmlib/` | gmmlib `intel-gmmlib-22.10.2`: macOS takes the Linux user-mode paths |
| `patches/level-zero/` | loader `v1.35.0`: Mach-O names, no `dlinfo`, `DYLD_LIBRARY_PATH` |
| `../user/tests/compute/ze_hello.c` | the Level Zero test: enumeration, USM, copy-engine fill/copy (immediate and queue + fence), compute-engine fill/copy (`compute`), a native kernel (`kernel <bin>`) |

Each `patches/*/series` is applied with `git am` on a branch `intelxe` at the pinned revision.

## Build (the build Mac)

```sh
pip install ninja                       # or brew install ninja; cmake >= 3.21 and the Xcode command line tools
L0_WORK=~/intelxe-l0 compute/build-neo.sh            # ~5 min on 8 cores; OCL=1 adds the OpenCL ICD (libigdrcl)
L0_WORK=~/intelxe-l0 compute/ocloc-docker.sh kernel user/tests/compute/kernels/fill_ids.cl ~/intelxe-l0/stage/kernels
L0_WORK=~/intelxe-l0 compute/ocloc-docker.sh builtins ~/intelxe-l0/src/neo ~/intelxe-l0/stage/builtins
```

`~/intelxe-l0/stage` is then copied to the test machine as one directory (`bin/`, `lib/`, `kernels/`, `builtins/`,
`SHA256SUMS`).

## Run

```sh
cd stage && DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello            # enumeration, USM, copy engine
XEMAC_FAKE=1 DYLD_LIBRARY_PATH=$PWD/lib ./bin/ze_hello init       # no kext (any Mac, x86_64 or Rosetta)
```

Debugging: `NEOReadDebugKeys=1 PrintDebugMessages=1 PrintXeLogs=1` (NEO), `XEMAC_TRACE=1` (every ioctl),
`ZE_ENABLE_LOADER_DEBUG_TRACE=1` (the loader's driver search).
