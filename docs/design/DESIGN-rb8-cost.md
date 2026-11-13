# rb8 cost: what the CAT-fault fixes add on the hot paths

Geekbench 6 OpenCL (import mode) fell about 3.5% when the kernel went from rb8b (parent of 128aa27; median 86.7k, 6
runs) to a945443 (rb8c-rb8f + LR mode; median 83.7k, 24 runs), same userland, no GPU resets either way. This note lists
every wait or extra piece of work rb8c-rb8f put on the exec, bind, free, fence and TLB paths, what it is for, how Linux
xe does it, and the `debug.intelxe.*` knob that turns it off for an A/B run. Counters: the `rb8 cost` line of
`debug.intelxe.stats`. Night phase: `kernel/IntelXe/tests/night-rb8cost.sh` (`phase_rb8cost`).

Linux references are the ones the rb8c-rb8f commits cite (`xe_pt.c`, `xe_exec.c`, `xe_ggtt.c`, `xe_guc_tlb_inval.c`,
v6.17/6.18). This note does not re-read them: the Linux tree was not in `~/hackintosh/src` when it was written.

## Findings (most likely cause of the 3.5% first)

| # | Path | Added cost | Needed for | Linux | Knob (default 1) |
|---|------|------------|------------|-------|------------------|
| 1 | VM_BIND out-fence includes the unbind's TLB invalidation (rb8d, `bindApply` -> `xeBindFenceNext`), carried by every later bind of that queue until it completes | Mesa makes every EXEC wait on the bind timeline. A pending invalidation in that point makes the EXEC's in-fence unsignaled, so `queueExec` takes the armed queue **out of direct submission** (`dsExit`) and the job goes through the FIFO and the submit worker, behind a G2H round trip. The next job must re-arm. A VM_BIND waiting on it (bind_async 0) blocks **in the ioctl** (`bindSubmit` kXeBindWait). Its user fence, if any, is written asynchronously (page wired and mapped into the kernel per bind). | An unbind's out-fence must not signal while the GPU can still translate through the removed entries (a range reused at once after waiting on it) | Same rule (`xe_pt_update_ops_run`: "tlb invalidation must be done before signaling unbind/rebind"), but every Linux job goes through drm_sched anyway, so the wait costs no fast path there and no ioctl blocks on it | `bind_tlb_fence` 0: out-fence = the clears only (rb8b) |
| 2 | EXEC waits for the TLB fences among its in-fences | as #1, from the EXEC side only | The EXEC's own engine does not need it: the unbind moved the VM's `ptGen`, so the queue's next batch invalidates its TLB in the ring (`tlbFlushDue`, direct submission included); the freed memory waits for the invalidation itself (`deferSeal`) | drm_sched dependency (asynchronous) | `exec_tlb_dep` 0: dropped from an EXEC's deps; binds and user fences keep it |
| 3 | Deferred frees wait for the VM's in-flight jobs (rb8c, `deferSeal` -> `deferJobsSnap`, `deferReap`) | Per unbind: an IOMalloc, a scan of the VM's queues under gSchedLock, an IOFree when no job runs. Memory (page-table pages, VRAM, userptr pages) held until the jobs end: higher VRAM use while a long batch runs. A CPU wait (5 s bound) only when the snapshot allocation fails. | CAT suspect #1: a page-table page or BO page reused while a batch that translated through it still runs | `ttm_bo_release` waits for the VM's dma-resv (BOOKKEEP: every exec) | `defer_jobs` 0: the invalidation alone (rb8b) |
| 4 | GGTT invalidation through the GuC, waited for (rb8d, `xeGgttInvalidate` / `xeGgttInvalWait`) in `queueCreate`, `queueTeardown`, `zombieFree` | One H2G per GT and a synchronous G2H round trip per exec-queue create and destroy (not per exec). Shares the in-order seqno space with PPGTT invalidations, so it also waits for any earlier ones. | A new queue's first job must not run through the GGTT translation of the LRC that had its slot | `xe_ggtt_invalidate` -> `xe_tlb_inval_ggtt` waits for the fence | `ggtt_inval_wait` 0: issued, not waited for |
| 5 | Migrate windows sealed with their own ranged invalidation and awaited (rb8c/rb8e, `migWindowSeal`, `migWindowAwait`) in `svCopyBo`, `xeVramRead`, failed BO moves | One more invalidation and a synchronous round trip per surface copy / VRAM read. Already counted: "migrate windows (rb8e)" (awaited, mean, max). | Pages and DMA mapping of the window let go only after the copy engine's TLB dropped them | `xe_migrate_access_memory` waits for the migrate fence before `xe_migrate_dma_unmap` | none (unmapping caller pages early is not safe enough even for a run) |
| 6 | Split of a live 2 MB / 1 GB leaf invalidates the whole span (rb8c, `ptSplit`), at least 16 MB | A larger ranged invalidation for binds that split a leaf (`splits` counter); no extra message | the TLB drops a 2 MB entry only for >= 16 MB ranges | `xe_pt_update_ops_rfence_interval`, `send_tlb_inval_ppgtt` | none (correctness) |
| 7 | `deviceWmb` after PTE writes instead of `sfence` (rb8c), `sfence` in `ptNodeNew` | one posted MMIO write per bind call; one sfence per new table | PTE writes ordered for the GPU on dGFX | `xe_device_wmb` in `xe_migrate_update_pgtables_cpu` | none (cost negligible) |
| 8 | `tlbStallCheck` in `workRun` and the waits' 2 ms fallback (rb8d) | gTlbLock and a 512-byte copy when an invalidation is outstanding; returns at once otherwise | a lost TLB_INVALIDATION_DONE must not hold every later bind fence | `xe_tlb_inval_fence_timeout` | none |
| 9 | `bindUfBusy` (rb8f) | one pass over the VM's binding records per VM_BIND with ops; gBjLock only while async binds are queued | EBUSY for an unbind under a pending user fence | `check_ufence` | none |
| 10 | LR mode | nothing unless the VM is created LR (the OpenCL stack does not) | | | |

Paths 1 and 2 are the prime suspects: they are the only ones that act per EXEC, and they turn a direct-submission job
into a worker job whenever the application unbinds between kernels (OpenCL import maps and unmaps buffers per
operation). Path 3 acts per unbind but only costs allocations; path 4 per queue lifetime. The counters say which one
fires: `EXEC in-fences on an invalidation` (paths 1-2), `EXECs to the worker for in-fences ... (left direct mode ...)`,
`VM_BIND waits in the ioctl ... (on an invalidation ...)`, `TLB round trips` (the G2H latency each such wait pays).

Nothing was found that is clearly redundant against Linux (no invalidation issued twice for one bind; the extra
in-ring flush of #2 is the existing ptGen mechanism, not new). No default was changed.

## Knobs (root, `sysctl debug.intelxe.<name>=0|1`, take effect at the next call)

- `bind_tlb_fence` 0 - the VM_BIND out-fence no longer waits for the invalidation of what the call unbound (rb8b).
  Risk: userland that reuses an unbound VA range or the BO's memory right after waiting on the out-fence can be reached
  through a stale TLB entry on an engine that does not invalidate in its ring first (another VM's queue, the copy
  engine). Kernel memory stays safe: frees still wait for the invalidation (and the jobs, `defer_jobs`).
- `exec_tlb_dep` 0 - an EXEC drops TLB-invalidation fences from its in-fences. Risk: lower than the above; the
  batch's own engine invalidates in the ring because the unbind moved the VM's ptGen. A TLB fence from another VM's
  unbind (shared syncobj) is no longer ordered before the batch, which only matters for a BO the two processes share.
- `defer_jobs` 0 - deferred frees wait for the invalidation only, not for the VM's in-flight jobs (rb8b). Risk: the
  CAT suspect #1 comes back (memory reused as another table or LRC while a batch still translates through it). rb8b ran
  with it; keep the run short and watch `guc+`/`gt+` in gb-results.
- `ggtt_inval_wait` 0 - GGTT invalidations still go to the GuC but are not waited for. Risk: a new queue's first job
  may race the invalidation of its slot's old LRC translation (rb8b never invalidated through the GuC at all).

Set all four back to 1 after a run; they are not boot-args and reset at the next kext load.
