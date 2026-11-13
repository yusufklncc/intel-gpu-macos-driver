# gbcl: a Geekbench-OpenCL-shaped microbenchmark

`cl_gbmicro` drives Apple's `OpenCL.framework`, the layer Geekbench's OpenCL workloads use. On the B580 that is
Apple's OpenCL over our Metal plugin (`nullmoth/plugin`), over ANV's xe backend, over the kext's xe uAPI, so a kernel
change shows up in seconds instead of a 2-minute Geekbench run. `night-gbcl.sh` defines `phase_gbclmicro` for the
night scripts.

    clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wno-deprecated-declarations -o cl_gbmicro cl_gbmicro.c -framework OpenCL
    ./cl_gbmicro [-t secs] [-m chain,churn,import,mixed,live] [-q]     # 15 s per mode by default

`nullmoth/build/build-intelxe.sh tests` builds it too.

## What each mode does to the kernel

From the code (plugin `nvmtl_vk.c`, ANV `xe/anv_kmd_backend.c`), per op, on the B580:

| mode | op | kernel calls per op (expected) |
|---|---|---|
| `chain` | 4 tiny kernels + one 256 KB kernel, `clFinish` | 1 `XE_EXEC` per command buffer Apple's OpenCL makes (1-5), 1 syncobj wait |
| `churn` | `clCreateBuffer` (64 KB .. 24 MB, 12 sizes in turn), 4 KB blocking write, 1 kernel, 4-byte blocking read, release | buffers past the plugin's pools (4 MB shared, 1 MB private) and not reused by its buffer park: `XE_GEM_CREATE` + `XE_VM_BIND` map, later `XE_VM_BIND` unmap + `GEM_CLOSE`; 2-3 execs and waits (each blocking write/read finishes the queue) |
| `import` | `CL_MEM_USE_HOST_PTR` on 442 KB + 48 B at 16 B past a page (8 ranges in turn), 2 kernels, blocking map, unmap, release | with `NVMTL_IMPORT_UNALIGNED=1`: `XE_VM_BIND` `MAP_USERPTR` + unmap per op (no GEM object; `NVMTL_IMPORT_CACHE=1` reuses them); without: no bind, the plugin copies the range at every submission |
| `mixed` | blocking 16 KB upload, 1-4 kernels, a temporary buffer every 4th op, an import every 8th, `clWaitForEvents`, 4-byte read | a blend of the above, waits through events as Geekbench does |
| `live16`..`live1024` | with L buffers live (half 64 KB, half 4 MB + 64 KB), release one, make its replacement, 2 kernels, `clFinish` | a bind and an unbind per op against a VM holding ~L/2 BOs of their own; shows costs that grow with the VM (rebind lists, LR-mode preemption, TLB invalidation) |

Apple's OpenCL may also back an ordinary `cl_mem` with its own malloc'd memory (`newBufferWithBytesNoCopy`), which
the plugin then imports or copies like the `import` mode: compare `churn` copy against import to see which.

## Reading the output

    RESULT mode=churn ops=2098 secs=2.00 ops_s=1048.9 p50_us=831.3 p99_us=2355.3 mean_us=953.4 max_us=7861.2 check=ok
      kstat churn: user fences: execs                    +1234  (0.588/op)

- `ops_s` is the throughput; `p50_us` the typical op, `p99_us` the tail (where a slow bind, a TLB invalidation wait
  or an LR preemption cycle shows first). Compare a kernel against another with the same plugin setting, alternated,
  the median of three runs: on the B580 the spread between runs is a few percent; a difference smaller than that is
  noise.
- `kstat` lines are the counters of `debug.intelxe.stats` that changed during the timed part, with the delta and the
  delta per op. Nothing is parsed by name (the text changes with each kernel revision): every number is labelled with
  its line's head and clause. Gauges (VRAM used, live queues) appear as deltas too. Per-op rates are the useful part:
  e.g. jobs direct/via worker per op is submissions per op, TLB invalidations per op is binds and unbinds reaching the
  GPU, deferred frees per op is BO frees. If the kernel was reloaded in between, no deltas are printed.
- `CHECK` lines mean a wrong result (the GPU's answer is verified for every op); `FAIL` is an OpenCL error.
- On a machine without the IntelXe kernel (a smoke test against Apple's driver) the `kstat` line says so.

The night phase writes `$O/gbcl/table.txt` (per mode and plugin setting: the three runs' ops/s, their median, median
p50 and p99), `$O/gbcl/results.txt` and `$O/gbcl/kstat-run1.txt`, and reports GuC/GT resets per run.
