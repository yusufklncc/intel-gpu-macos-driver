# Long-running VMs in IntelXe.kext (lrmode, 2026-11-12)

> Design note with its date: it records the plan and the state of that day, not the current status. Branch `lrmode`,
> **untested on hardware** (build and host tests only). Current status: `docs/ROADMAP.md`.

`DRM_XE_VM_CREATE_FLAG_LR_MODE` is what Intel's compute runtime (NEO) asks for on every VM
(`IoctlHelperXe::getFlagsForVmCreate`). Until this branch the kext refused it, so NEO ran in dma-fence VMs (compute
patch 0006's fallback) without its ring-buffer direct submission (ULLS, patch 0009), and a kernel running longer than
the job timeout (10 s, `debug.intelxe.job_timeout_ms`) was reset and its queue banned (DESIGN-level-zero.md, section 2,
"Blockers found" item 1). This note is what the kext does now, against Linux xe (v6.18+ tree in `dl/linux-xe`).

## 1. What Linux does

| Topic | Linux (file: function) |
|---|---|
| accepting the flag | `xe_vm.c: xe_vm_create_ioctl`: LR_MODE alone, or with FAULT_MODE (`has_usm`), NO_VM_OVERCOMMIT only with FAULT_MODE. Without FAULT_MODE the VM is in *preempt-fence mode* (`xe_vm.h: xe_vm_in_preempt_fence_mode`) |
| out-syncs | `xe_exec.c: xe_exec_ioctl` and `xe_vm.c: xe_vm_bind_ioctl` parse every sync with `SYNC_PARSE_FLAG_LR_MODE`; `xe_sync.c: xe_sync_entry_parse` answers `-EOPNOTSUPP` for a `SYNCOBJ` or `TIMELINE_SYNCOBJ` with `DRM_XE_SYNC_FLAG_SIGNAL`. In-syncs and user fences are allowed (uAPI doc of `drm_xe_vm_create`) |
| exec without batch | `xe_exec_ioctl`: `if (!xe_vm_in_lr_mode(vm))` around `xe_sync_in_fence_get` / `xe_sync_entry_signal`: on an LR VM nothing happens |
| exec and kernel fences | `xe_exec_ioctl` skips the drm_exec locking, the `DMA_RESV_USAGE_KERNEL` deps and `drm_gpuvm_resv_add_fence` for LR VMs: job fences are not in the BOs' dma-resv |
| job timeout | `xe_guc_submit.c: guc_exec_queue_init`: `timeout = MAX_SCHEDULE_TIMEOUT` when `q->vm` is in LR mode; `guc_exec_queue_timedout_job` asserts an LR job only gets there killed or after an error; `xe_exec_queue.c: xe_exec_queue_is_lr` (not for VM bind queues) |
| reset and ban | unchanged for LR: a GuC context reset or CAT error bans (`xe_guc_exec_queue_reset_trigger_cleanup`), a GT reset bans a queue whose first pending job started (`guc_exec_queue_stop`) |
| memory protection | `xe_vm.c: xe_vm_add_compute_exec_queue`: every exec queue of the VM has a preempt fence in the BOOKKEEP slot of the VM's resv (and of its external BOs, `drm_gpuvm_resv_add_fence`). Anything that waits for a BO to be idle (TTM eviction, `xe_bo.c: xe_bo_trigger_rebind`, `ttm_bo_release` of a freed BO or page table) enables those fences |
| preempting | `xe_preempt_fence.c: preempt_fence_enable_signaling` -> `q->ops->suspend` (`xe_guc_submit.c: __guc_exec_queue_process_msg_suspend`: a schedule disable of an enabled context, after it ran `min_run_period_lr_ms` (5 ms, `xe_device.c`) since its last resume); `preempt_fence_work_func` -> `suspend_wait` (`guc_exec_queue_wait_suspend_done`, 5 s; on timeout `guc_exec_queue_suspend_timeout_ban`), then the fence signals and the rebind worker is queued |
| resuming | `xe_vm.c: preempt_rebind_work_func`: revalidate and rebind what moved, `dma_resv_wait_timeout(KERNEL)` for the rebinds, then `resume_and_reinstall_preempt_fences` -> `q->ops->resume` (`__guc_exec_queue_process_msg_resume`: a schedule enable). Work submitted meanwhile only moved the ring tail (`submit_exec_queue` returns early for a suspended queue) |
| engine groups | `xe_hw_engine_group.c: xe_hw_engine_group_add_exec_queue` returns at once for preempt-fence-mode VMs; `xe_hw_engine_group_find_exec_mode` gives them `EXEC_MODE_DMA_FENCE`: only fault-mode VMs switch a group to LR mode, so without fault mode there is nothing to arbitrate |
| TLB | `xe_vm_create`: `batch_invalidate_tlb = false` for LR VMs |
| power | `xe_vm_create` takes a runtime-PM reference for an LR VM (no runtime suspend while it exists) |
| system sleep | `xe_pm.c: xe_pm_suspend`: `xe_bo_evict_all` (preempts LR queues through their fences), then `xe_gt_suspend` -> `xe_uc_stop` -> `xe_guc_submit_stop` -> `guc_exec_queue_stop`, which bans a queue whose job started and did not complete, LR or not |

## 2. What the kext does (`kernel/IntelXe/xe_core.cpp` "long-running VMs", pure logic in `xe_lr.h`)

- **VM_CREATE** (`xeVmCreateCheck`, `xe_uapi_args.cpp`): LR_MODE accepted, `XeVm::lr`. FAULT_MODE stays EINVAL (no GPU
  page-fault service; xe's answer on a device without `has_usm`), so NO_VM_OVERCOMMIT is EINVAL too.
- **EXEC / VM_BIND syncs**: `kXeSyncParseLrMode` on an LR VM (`xeSyncEntryCheck` had the rule since rb7). VM_BIND now
  looks the VM up before its syncs (EINVAL for an unknown VM, as `xe_vm_lookup`'s caller). An EXEC without batch on an
  LR VM does nothing (`xeLrExecNoop`); one on a banned queue is ECANCELED first, as `reset_status`.
- **Job timeout**: `watchRun` skips an LR VM's queues (`xeLrJobTimeoutApplies`), including the "never started" and
  "stuck after a GuC reset" rules (rb8d): Linux has no timer for them at all. A job that outlives the limit is counted
  once ("past the job timeout"). GuC context resets and GT resets still ban them (`queueKickOff`, `xeGtReset`).
- **Preemption** replaces the job waits that protect memory. Two users:
  - *Moves* (`boMove` with `mayWait`, which is the CPU-map path `boMakeVisible`, and `boMoveSys`): `lrHoldBegin` holds
    every LR VM that binds the BO; `lrMoveReady` takes each queue off (`lrQueueOff`, one `xeLrQueueStep` per call):
    `lrOff` keeps the ring closed (the FIFO keeps jobs; `queueExec`, the submit worker and direct submission all test
    it, as they test `tdr`), an armed direct-submission slot leaves first; a queue with work in its ring gets
    `SCHED_CONTEXT_MODE_SET(disable)` once 5 ms have passed since its last resume, and counts as off when
    `SCHED_CONTEXT_MODE_DONE` arrives; a queue with an idle ring needs no round trip. Then the copy and remap as before;
    `lrMoveDone` waits for the remap's TLB invalidation (the rebind worker's `dma_resv_wait` before the resume) and
    `lrHoldEnd` sends `SCHED_CONTEXT_MODE_SET(enable)` to each queue it disabled (`lrVmOn`) and kicks the FIFOs.
  - *Deferred frees* (`deferSealLr`): an unbind, a purge or a remap of an LR VM releases page tables and BO memory only
    after one **preemption cycle** of its queues instead of their jobs (which may never end: an endless ULLS ring would
    leak every unbind). The cycle is asked for only when a queue has work outstanding (the rule `deferJobsSnap` applies
    to dma-fence VMs: a job submitted after the seal starts with the PTEs gone and invalidates its TLB in the ring).
    `lrRequest` puts the VM on `gLrHead`; the submit worker (`lrRun`, every 2 ms while one is pending, kicked by
    `SCHED_CONTEXT_MODE_DONE`) takes the queues off the same way, counts the cycle done (`lrDone`, which the deferred
    entries compare with `xeLrCycleCovers`) and resumes them.
  - A disable not confirmed within 5 s bans the queue and, on the primary GT, resets the GT
    (`guc_exec_queue_suspend_timeout_ban`; the watchdog's rule for an unconfirmed disable). A move that gives up
    waiting leaves the VM to a cycle, which resumes it once every disable is confirmed (`lrVmSettled`).
  - Holds and cycles stack (`xeLrResumeNow`: no resume while a hold or a requested cycle remains), as xe's
    `suspend_count`.
  - A GT reset or a wake from sleep registers a held queue **disabled** and marks it confirmed off; the resume's enable
    then runs what its ring holds (Linux: `submit_exec_queue` returns early for a suspended queue). The replay's
    `SCHED_CONTEXT` is not sent for it.
- **Evictions and purges** (`evictClient`, `boMove` without `mayWait`, `purgeBlocked`) still take only BOs whose VMs are
  idle: an eviction does not wait, so a busy LR VM's window BOs stay (as a busy dma-fence VM's do), and the map that
  needed the room moves its own BO to system memory if the window is full (`boMoveSys`, which does preempt).
- **System sleep**: unchanged. `xeSuspend` waits 2 s for the queues and bans those still running: Linux bans a started
  LR job in `guc_exec_queue_stop` after `xe_bo_evict_all` suspended it, so both end with the queue banned (NEO reports
  a lost device after the wake, as on Linux). VRAM is saved and restored in place (same DPAs), so no rebind is needed.
  No preemption runs during sleep (`xeSuspend` holds every client lock, so no move is in progress; a pending cycle
  waits for the GuC, `kXeLrWaitGuc`, and completes after the wake).
- **TLB**: `tlbFlushDue` never applies the on-demand per-batch rule (`tlb_every_batch`) to an LR VM.
- **Teardown** of a held queue does not wait 2 s for jobs that cannot run.
- **Counters**: `debug.intelxe.stats`, line "long-running VMs (lrmode)": VMs made; live and held queues; execs (and
  those without batch); out-syncs refused; jobs past the job timeout; disables, enables, unconfirmed (banned); cycles
  asked, done, pending; move holds, how many waited and how long (mean, max); invalidations still pending at a resume.

### Where the kext differs from xe, and why

| Item | xe | kext | Why |
|---|---|---|---|
| kernel fences on exec | not waited for on an LR VM | still waited (`vm->kern`, the clears of fresh VRAM) | xe's LR bind job waits for the BO's clear before writing the PTEs; here VM_BIND writes the PTEs in the ioctl and the exec's wait is what keeps a batch from reading VRAM before its clear |
| what is preempted for a free | every free of a BO or page table on the VM's resv (TTM) | only when a queue of the VM has work outstanding | an idle queue cannot be translating through the removed PTEs, and its next job invalidates first; saves a GuC round trip per unbind of an idle VM |
| queue with an idle ring | disabled anyway when enabled | held (ring closed) without a disable | nothing of it can run until the hold ends |
| rebind | a worker revalidates evicted BOs and rebinds them on the GPU | the move rewrites the PTEs on the CPU itself (`boRemap`) | binds are CPU page-table writes here; the queues are off for exactly the copy, remap and invalidation |
| evictions | TTM evicts LR BOs by preempting | only idle VMs' BOs | an eviction here never waits; the mapping path preempts instead |
| runtime PM | an LR VM holds a runtime-PM reference | nothing | the kext has no runtime D3 |

## 3. Direct submission (debug.intelxe.direct) and LR queues

The kext's own direct submission and NEO's ULLS are independent: ULLS is one exec of NEO's ring buffer, a batch that
spins on a semaphore in user memory and never ends while NEO keeps it running. With LR VMs that job is exempt from the
watchdog, so ULLS can run (patch 0009 lets NEO turn it on once the kext accepts LR_MODE). The kext's slot arms only
after two execs within `direct_idle_us`, so a ULLS queue (one exec) normally never arms; a slot armed behind a job that
does not end is harmless (it runs after the job, or its no-op does when the idle exit fires). An LR queue that is not
running ULLS (NEO with `EnableDirectSubmission=0`, any other user) arms and leaves exactly as a dma-fence queue does:
the idle exit, contention, a FIFO wait, suspend, GT reset, close. The one new exit is the LR preemption: `lrQueueOff`
makes an armed queue leave before it closes the ring (counted as "other"), since a held queue's FIFO must take the
jobs and an armed queue's FIFO must stay empty; `dsEnter` and `dsSubmit` refuse a held queue. An LR queue is not kept
armed past the idle exit: Linux has no kernel-side direct submission for user queues (its `xe_ring_ops.c` ULLS is the
migrate queue's), and a context that polls when nothing comes costs power (+7 W measured, DESIGN-direct-submission.md)
and keeps other contexts waiting for a timeslice.

## 4. Tests

- Host: `kernel/IntelXe/tests/lr_test.cpp` (49 checks: VM_CREATE flags, the sync rules, the preemption step and a
  whole multi-queue preemption, cycle counters, resume rules), with the other host tests unchanged.
  `c++ -std=c++17 -Wall -Wextra -I.. -o lr_test lr_test.cpp ../xe_uapi_args.cpp && ./lr_test`
- GPU: `user/tests/kernel/lr.c`, phases `args`, `long`, `ds` (see its header). Build on the build Mac:
  `clang -arch x86_64 -O2 -I../../../kernel/IntelXe/uapi -o lr lr.c -framework IOKit`.

Night commands (test machine, the kext from this branch installed; each ≤ 60 s):

```sh
./lr args                                            # < 1 s: the uAPI rules
./lr long                                            # job_timeout_ms + 2 s (13 s by default): no TDR, the move and cycle
./lr ds                                              # < 1 s, only with debug.intelxe.direct=1 (the default in the tree)
sysctl debug.intelxe.stats | grep -E "long-running|^TDR|^jobs|^BO moves|direct submission"
./ufence && ./unbindfence && ./bomove && ./bomove evict   # dma-fence VMs unchanged
# NEO in LR mode with ULLS (DESIGN-level-zero.md section 9 for the setup):
cd ~/l0/builtins && DYLD_LIBRARY_PATH=$HOME/l0/lib NEOReadDebugKeys=1 PrintXeLogs=1 ../bin/ze_hello kernel ../kernels/fill_ids_bmg.bin 2>&1 | grep -iE "LongRunning|ze_hello|error" | tail -20
```

On a failure: `log show --last 2m --predicate 'sender == "IntelXe"' | tail -60` (the G2H log has the "LR preemption"
lines) and the stats line above.

## 5. Open items

- Untested on the B580: whether a GuC schedule disable takes a context polling `MI_SEMAPHORE_WAIT` off promptly (it
  must: NEO's ULLS on Linux depends on it, and i915's timeslicing relied on semaphore waits being preemption points);
  `./lr long` shows it (move hold wait in the stats line).
- Evictions of busy LR VMs' window BOs (xe preempts for them); purges of DONTNEED BOs in busy LR VMs.
- A preemption cycle per unbind of a busy VM is xe's behaviour too, but NEO frees often: measure the disable/enable
  rate under a real Level Zero workload (stats line) before tuning (batching frees per cycle already happens: one cycle
  covers every free asked for while it runs).
- Fault mode (`FAULT_MODE`, SVM, `CPU_ADDR_MIRROR`) remains refused.
