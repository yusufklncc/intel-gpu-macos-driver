# rb8 cost: what the CAT-fault fixes add on the hot paths

Geekbench 6 OpenCL (import mode) fell about 3.5% when the kernel went from rb8b (parent of 128aa27; median 86.7k, 6
runs) to a945443 (rb8c-rb8f + LR mode; median 83.7k, 24 runs), same userland, no GPU resets either way. This note lists
every wait or extra piece of work rb8c-rb8f put on the exec, bind, free, fence and TLB paths, what it is for, how Linux
xe does it, and the `debug.intelxe.*` knob that turns it off for an A/B run. Counters: the `rb8 cost` line of
`debug.intelxe.stats`. Night phase: `kernel/IntelXe/tests/night-rb8cost.sh` (`phase_rb8cost`).

Linux references are the ones the rb8c-rb8f commits cite (`xe_pt.c`, `xe_exec.c`, `xe_ggtt.c`, `xe_guc_tlb_inval.c`,
v6.17/6.18). This note does not re-read them: the Linux tree was not in `~/hackintosh/src` when it was written.

## Findings (ordered by the suspicion before the measurement; the B580 A/B below cleared all of them)

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

## Measured on the B580 (night of 13 Nov, `phase_rb8cost` and `phase_tlbinring`)

Geekbench 6 OpenCL, import mode, three runs per side, off and on alternating; scores in thousands, `guc+`/`gt+` the GuC
context resets and GT resets during the run.

| Knob | Off (runs; median) | On (runs; median) | Resets |
|------|--------------------|-------------------|--------|
| `bind_tlb_fence` | 85.8, 79.7, 84.1; 84.1 | 82.6, 85.3, 84.8; 84.8 | **off, first run: 3 GuC context resets + 1 GT reset**; none on |
| `exec_tlb_dep` | 81.7, 86.7, 84.1; 84.1 | 84.9, 81.9, 83.3; 83.3 | none |
| `defer_jobs` | 81.8, 85.6, 86.0; 85.6 | 76.3, 82.3, 82.7; 82.3 | none (76.3 is a single outlier; being re-measured with more runs) |
| `ggtt_inval_wait` | 83.0, 80.4, 85.5; 83.0 | 84.4, 85.0, 81.8; 84.4 | none |
| all four | 84.1, 87.6, 85.9; 85.9 | 85.2, 85.1, 82.3; 85.1 | none |
| `tlb_inring` (0 / 1) | 84.1, 81.5, 86.2; 84.1 | 85.0, 83.5, 82.6; 83.5 | none |

Run-to-run spread is about +-4k (one side alone spans 79.7-85.8), so no knob moves the score measurably, all four off
together included: the waits rb8c-rb8f added are not where the 3.5% went (findings 1-4 are cleared; 5-10 have no knob
and act per copy or per queue). `cl_gbmicro` (chain, churn, import, mixed; 3 runs per side) is the same with
`tlb_inring` 0 and 1 (chain 12.2-12.5k/s, churn 645-675/s, import 2.72-2.82k/s, mixed 1.71-1.76k/s).

Decisions (kept in the code, `xe_core.cpp` next to the knobs and `IntelXe.cpp` at their sysctls):

- `bind_tlb_fence`, `exec_tlb_dep`, `ggtt_inval_wait`: stay 1. They cost nothing measurable and are what Linux does.
- `bind_tlb_fence` 0 is unsafe as well as useless: its first run reset the GPU. Since 13 Nov a write of 0 is refused
  (`EPERM`) unless the kext was booted with `-intelxe_unsafe_bindfence` (`xe_tlbinval.h xeBindTlbFenceWrite`,
  `tests/tlbinval_test.cpp` section 11); 1 is always taken, other values `EINVAL`. `night-rb8cost.sh` no longer
  toggles it by default (`RB8COST_KNOBS` overrides).
- `defer_jobs`: stays 1 until the re-measurement with more runs (the one low run is the only sign of a cost).
- `tlb_inring`: stays 0. Correct on hardware (`bindstress 10 r,rl,c,cl bindfence` with it on: pass, no CAT error, GT
  reset, page fault or ban), but no gain: in Geekbench only 66-182 EXECs a run took the armed slot against ~243k sent
  to the worker for in-fences; in bindstress 24.9k took it and 385k went the old way because their queue was not armed
  (knob 1 takes armed queues only) and 75k because another kind of in-fence was pending. Knob 2 (the normal ring
  path too) was not measured.

## Knobs (root, `sysctl debug.intelxe.<name>=0|1`, take effect at the next call; `bind_tlb_fence=0` also needs the boot-arg `-intelxe_unsafe_bindfence`)

- `bind_tlb_fence` 0 - the VM_BIND out-fence no longer waits for the invalidation of what the call unbound (rb8b).
  Risk: userland that reuses an unbound VA range or the BO's memory right after waiting on the out-fence can be reached
  through a stale TLB entry on an engine that does not invalidate in its ring first (another VM's queue, the copy
  engine). Kernel memory stays safe: frees still wait for the invalidation (and the jobs, `defer_jobs`). On the B580
  its first Geekbench run with 0 had 3 GuC context resets and a GT reset, so 0 is refused without
  `-intelxe_unsafe_bindfence` (above).
- `exec_tlb_dep` 0 - an EXEC drops TLB-invalidation fences from its in-fences. Risk: lower than the above; the
  batch's own engine invalidates in the ring because the unbind moved the VM's ptGen. A TLB fence from another VM's
  unbind (shared syncobj) is no longer ordered before the batch, which only matters for a BO the two processes share.
- `defer_jobs` 0 - deferred frees wait for the invalidation only, not for the VM's in-flight jobs (rb8b). Risk: the
  CAT suspect #1 comes back (memory reused as another table or LRC while a batch still translates through it). rb8b ran
  with it; keep the run short and watch `guc+`/`gt+` in gb-results.
- `ggtt_inval_wait` 0 - GGTT invalidations still go to the GuC but are not waited for. Risk: a new queue's first job
  may race the invalidation of its slot's old LRC translation (rb8b never invalidated through the GuC at all).

Set all four back to 1 after a run; they are not boot-args and reset at the next kext load (`-intelxe_unsafe_bindfence`
only unlocks the write of 0, it does not set it).

## tlbinring: the EXEC waits for its invalidations in the ring (branch `tlbinring`, 13 Nov; on the B580 the same night: correct, no gain, default 0 kept)

Linux read for this section (kernel.org, v6.18 tag, and v6.17 where the file differs): `xe_pt.c`, `xe_exec.c`,
`xe_sync.c`, `xe_sched_job.c`, `xe_tlb_inval_job.c`, `xe_tlb_inval.c`, `xe_guc_tlb_inval.c`, `xe_ring_ops.c`,
`xe_vm.c` (v6.17: `xe_gt_tlb_invalidation.c`).

Findings #1 and #2 together, fixed without giving up what rb8d added: `debug.intelxe.tlb_inring` (default 0, root,
takes effect at the next EXEC; needs `exec_tlb_dep` 1). Pure parts in `kernel/IntelXe/xe_tlbinring.h`, host test
`kernel/IntelXe/tests/tlbinring_test.cpp`, night phase `kernel/IntelXe/tests/night-tlbinring.sh` (`phase_tlbinring`;
the night kit's copy with a `bindstress` built from this branch is `~/hackintosh/work/night1214/night-tlbinring.sh`).

### What it does

An EXEC whose pending in-fences (after its own queue's are dropped) are **all** TLB-invalidation fences
(`kXeTlbSlot`: a VM_BIND out-fence since rb8d) goes to the ring at once, as an EXEC without in-fences would, and waits
for those invalidations on the GPU:

- **The TLB word.** One dword in the direct-submission page (`gDsMem` page 0 at 0x800, after the 32 mailboxes,
  GGTT-mapped since `debug.intelxe.direct` first went to 1, which it does at start unless `-intelxe_nodirect`), the GPU's
  copy of the invalidation done seqno: `xeTlbDoneBoth(tlbDone, mTlbDone, mediaOn)` (done on the primary and, while it is
  up, the media GuC: exactly what `tlbSeqDone` tests). `xeTlbWordPublish` moves it forward with a compare-and-swap (never
  back: racing publishers) and `clflush`es it, from every place that moves the done seqnos: the G2H
  `TLB_INVALIDATION_DONE` handler (no worker in between), `tlbStallCheck` (the 2.25 s fence timeout), the GT reset
  (`IntelXe::gtReset`, `xeGtReset`), a GuC reset or reload (`gucReset`, `mediaGucLoad`), resume (`gpuResume`, `xeResume`);
  the submit worker and the job watchdog call it as a backstop.
- **The wait.** `jobPre` puts `MI_SEMAPHORE_WAIT` (GGTT, poll, `SAD_GTE_SDD`, `xeSemWaitGte`) on the word >= the newest
  invalidation among the in-fences at the head of the job, inside the preparser-disabled part before the invalidating
  `PIPE_CONTROL` (render/compute) or `MI_FLUSH_DW` (copy/video): the in-ring TLB flush, the start seqno, the jump to the
  batch (no prefetch of it), the user fence and the seqno all come after it. 5 dwords; a direct-submission stub with job
  timing and a batch is then 32 dwords, its full size (`kDsStubBytes` 128).
- **Where.** `xeTlbInringPath`: knob 1, the queue is armed: into the armed slot (`dsSubmit` -> `dsStubWrite` with the
  wait in the stub), and **the queue stays armed** (no `dsExit`, no re-arm); knob 2: also an unarmed queue's job goes to
  the normal ring path with the wait (it may then arm, `dsEnter`). Otherwise, and whenever anything else holds the job
  (FIFO not empty, TDR, LR hold, GT reset, sleep), another kind of in-fence is pending too (a job, a clear, a queued
  bind), or the target fails the wrap guard: the old way (FIFO and worker; an armed queue leaves first).
- **Wrap guard** (`xeTlbInringPlan`). The semaphore compares unsigned; seqnos wrap at 2^32. A target is taken only if it
  is nonzero, below 0xfff00000 and on the same side of the wrap as the word, so the word passes it before it can wrap
  (that would take 2^20 invalidations unanswered at once, while each one is an ioctl and the fence timeout ends any
  within 2.25 s). Beyond that bound a job could only stay waiting (never-started rule: GT reset), never run early.

### Safety argument (the CAT guarantees)

1. **Freed or reused memory is untouched by this change.** Deferred frees (page-table pages, BO VRAM, userptr pages) are
   still released by `deferReap` only when `xeDeferReady(sealed, tlbSeqDone(seal seq), the VM's jobs of that moment done)`
   (rb8c); BO moves, migrate windows and the GGTT keep their own waits. Those read the CPU-side done seqnos, which the TLB
   word only mirrors. So "the freed pages are not reused before the invalidation completes" holds as before, whatever
   the EXEC does; `tlbinring_test` section 5 checks it on a model.
2. **The EXEC's order is Linux's.** In Linux xe the unbind's out-fence is the invalidation job's fence
   (`xe_pt.c xe_pt_update_ops_run`: "tlb invalidation must be done before signaling unbind/rebind";
   `xe_tlb_inval_job.c xe_tlb_inval_job_push` / `xe_tlb_inval_job_run`), the EXEC gets it as a drm_sched dependency
   (`xe_exec.c xe_exec_ioctl` -> `xe_sync.c xe_sync_entry_add_deps` -> `drm_sched_job_add_dependency`), and the job's
   ring commands run only once `xe_tlb_inval.c xe_tlb_inval_done_handler` (or `xe_tlb_inval_fence_timeout`) signaled it.
   Here nothing of the job after the semaphore runs before the word reaches the target, the word is never ahead of the
   done seqno (it is set from it, forward only), and done means the same events (every GuC's `TLB_INVALIDATION_DONE`, the
   fence timeout, a GT reset). The batch starts after the GuC's invalidation (every engine of each GT, heavy mode, cache
   flush: `xeTlbInvalOp`) completed, not only after the engine's own `PIPE_CONTROL` TLB invalidation (which still runs, `tlbFlushDue`,
   as `xe_sched_job.c xe_sched_job_arm` / `xe_ring_ops.c emit_pipe_invalidate` do). This is what `exec_tlb_dep` 0 (and
   relying on `tlbFlushDue` alone) gives up.
3. **Out-fences stay transitive.** The EXEC's seqno store and user fence come after the wait, so whoever waits for the
   EXEC's out-fence (another queue, the CPU, a later bind) also knows the invalidations completed, as with Linux's
   finished fence. Jobs behind it on its queue are behind it in the ring.
4. **The VM_BIND out-fence is unchanged** (rb8d: it includes the invalidation): binds, user fences and syncobj waits see
   what they saw.

`tlbinring_test` checks the decision (sections 1-2), and on a model of the GPU's unsigned compare with racing
publishers around the 2^32 wrap that every accepted wait passes exactly when its invalidations are done (never early,
never stranded within the bound; 3.8 M checks; a mutation that drops the wrap guard makes it report both early and
stranded waits), and that the word's value is done exactly when `tlbSeqDone` is (section 4).

### Options looked at

- (a) **In-ring wait** (this). Same order as Linux, the wait moves from the CPU (drm_sched callback, here the FIFO and
  the worker) to the command streamer; the armed queue stays armed and the job's GuC scheduling (29 us of a 56 us round
  trip, DESIGN-direct-submission.md) disappears from the path, as does the worker wake-up after the G2H.
  "Relying on `tlbFlushDue` instead" is `exec_tlb_dep` 0: the batch's engine flushes its own TLB, but the batch may run
  before the GuC's invalidation completed, and the EXEC's out-fence no longer implies it. Rejected as the fix.
- (b) **Signal separation** (out-fence = PTEs written, a separate "memory reusable" fence for frees). Linux does not do
  this: the unbind's out-fence *is* the invalidation fence. Here the frees already wait on their own (point 1), so (b)
  amounts to `bind_tlb_fence` 0 for EXECs: userland that waits on the bind (or on an EXEC after it) and then reuses the
  range from an engine that does not flush first (another VM's queue, the copy engine) is exposed again. Rejected.
- (c) **Spin briefly** in the ioctl for the G2H. The "TLB round trips" counter gives the G2H latency (tens of us
  expected): a spin that long per EXEC burns CPU for what (a) gets on the GPU, and a miss still falls back. Not done.

### Differences that remain (documented, not fixed)

- While it waits, the job holds its engine (polling, timesliced against other contexts by the GuC), as the armed slot's
  own mailbox wait does. With knob 2 that also applies to an unarmed queue.
- The job counts as not started while it waits (the semaphore precedes the start seqno): the 5 s never-started rule
  is far above the 2.25 s fence timeout, but rb8d's 2 s shortcut after a GuC context reset in the render/compute domain
  could reset the GT for a job whose `TLB_INVALIDATION_DONE` was lost in those 30 s.
- System sleep: `tlbStallCheck` is off while `gSuspended`, so a job waiting in the ring for a lost
  `TLB_INVALIDATION_DONE` at the moment of a sleep is banned by `xeSuspend`'s 2 s drain, where the FIFO would have held
  it.
- Job timing (`debug.intelxe.jobtime`): the wait is counted in "invalidate".
- A CAT dump of a job that waits shows ACTHD in its stub (direct) or at the semaphore in the ring.

### Counters

`debug.intelxe.stats`, "rb8 cost" line: `tlb_inring N`, and "EXECs waiting in the ring for invalidations (tlbinring):
armed slot A, normal ring R (word already there P); old path: other in-fences, wrap guard, not armed (knob 1), held,
no TLB word, no ring room". With the knob on, "EXECs to the worker for in-fences ... (left direct mode ...)" should drop
by about A + R against a run with it off; P counts jobs whose wait passed at once (the G2H had been handled already).

### Night (phase_tlbinring)

Geekbench 6 OpenCL import x3 with the knob 0 and 1 alternating (which side goes first alternates too), the counters
before and after each run; `cl_gbmicro -m chain,churn,import,mixed` (unaligned import on) x3 per knob; then `bindstress
10 r,rl,c,cl bindfence` with the knob on (the `bindfence` option, added on this branch, makes every VM_BIND signal a
timeline point and every EXEC wait on the last one, as Mesa does; without it bindstress's EXECs have no in-fences and
never take the new path). Expected: no CAT error, GT reset, page fault, ban or check mismatch; "armed slot" in the
thousands. `TLBINRING_ON=2` runs the "on" side with the normal ring path too.
