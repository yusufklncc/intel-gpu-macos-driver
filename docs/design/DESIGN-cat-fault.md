# The GuC MEMORY_CAT_ERROR: analysis and the rb8c changes (2026-11-12)

> Design note with its date: it records the analysis and the state of that day, not the current status. Current
> state: `docs/ROADMAP.md` ("Submission cost and latency", "GPU resets").
>
> Everything in "What rb8c changes" and "rb8d" is in the tree, **untested on hardware**.

## The faults

- 9-10 Nov: three `MEMORY_CAT_ERROR`s in six cold Geekbench OpenCL runs with the GuC low-latency hint on, none in four
  without it; two of them came without a page-fault descriptor. 12 Nov night: none in four runs with the hint (direct
  submission off).
- 12 Nov 12:2x: one in six Geekbench 7 OpenCL runs with the plugin importing unaligned host pointers through the pages
  around them (`NVMTL_IMPORT_UNALIGNED=1`: more userptr binds and unbinds), none in ~30 runs without it that day.
  Direct submission was on. The G2H log:

```
G2H MEMORY_CAT_ERROR for guc_id 39 (0x0): queue to be banned
  guc_id 39 reset by the GuC: geekbench_avx2 (pid 5698), class 0, seqno 16113 done 16112
  dump (GuC reset): fault 0x00000000 va 0x210000000
  rcs0 h d20015c t 198 acthd 3fa06364 lrca 20bc001 (geekbench_avx2 39) el 1380:3098 mi 1000 esr 0 eir 0 ipehr 5000001 idone ffdeffff
  q 39 geekbench_avx2 class 0: done 16112 started 16113 submitted 16113 guc 0x0 tdr 0
  moves: in 0 out 0 sys 0 purge 0 fail 0 busy 0 evict 0; last - never
  tlb: issued 128034 done 128034/128034, last 4 ms ago; deferred frees 0
  vm asid 201 scratch: ptGen 10776, queue flushed at 10776; binds 93
  vm unbind 21 ms ago (n 5351, tlb 128031 done), remap never, bind 12 ms ago
job timeout: queue guc_id 17 (class 0) seqno 3543 never started in 10020 ms: GT reset
```

### What the dump says

- **Where the engine was.** A render job without user fence or job timing is 34 dwords in the ring (`jobPre`: preparser
  off, the invalidating `PIPE_CONTROL`, preparser on, the `CTX_TIMESTAMP` store, the start seqno, `MI_BATCH_BUFFER_START`
  = 19 dwords; `jobPost` 14, one `MI_NOOP`). With the tail at 0x198 the job starts at 0x110, and 0x110 + 19 x 4 = 0x15c
  is the head: the dword after the `MI_BATCH_BUFFER_START`. The command streamer was inside the batch (ACTHD 0x3fa06364
  is a PPGTT address, not the LRC's ring at GGTT 0x20bc000), the start seqno 16113 had been written ("started 16113"),
  the end-of-job flush, user fence and seqno had not. An armed direct-submission slot (24 dwords: semaphore wait, then a
  first-level jump to a stub) does not fit these numbers: the job went through the normal ring path.
- **IPEHR 0x05000001**: bits 31:29 = 0 (MI), opcode bits 28:23 = 0x0A, `MI_BATCH_BUFFER_END` (xe_mi_commands.h
  `__MI_INSTR(0xA)`), which has no length field. The last instruction parsed was a batch end while ACTHD stayed in a
  PPGTT batch: the end of a second-level (or chained-from) batch, back in the batch that called it.
- **No translation fault.** The fault register (0xcec4) reads 0 and no page-fault descriptor came, and the VM has a
  scratch page, so an access to an unmapped address reads the NULL page instead of faulting. What is left is a walk
  or a memory access that went wrong below the translation: a page-table entry that is not one (a page reused while a
  walk or a cached path still reached it, a table read before its contents landed), a leaf pointing outside VRAM, or a
  corrupted context image (PDP0). The type dword (0x0) is HW-defined (Linux does not decode it, bspec 72187); rb8c logs
  whether the GuC sent it at all (the old line printed 0 for a missing one).
- **Timing.** The VM's last unbind was 21 ms before, its last bind 12 ms before, 5351 unbinds in that process; every
  invalidation issued had completed; no BO move of any kind had happened since boot. So the fault is not a move, and
  not an invalidation still in flight: it is something that survives a *completed* invalidation.
- The queue that "never started" afterwards (guc_id 17) found the render engine still holding the dead context
  (RING_CURRENT_LRCA unchanged, ACTHD 0): a consequence (the GuC did not recover the engine), not the cause.

## Where IntelXe's ordering differed from Linux xe

Linux sources read: v6.18 `drivers/gpu/drm/xe/` `xe_pt.c`, `xe_vm.c`, `xe_bo.c`, `xe_exec.c`, `xe_migrate.c`,
`xe_guc_tlb_inval.c`, `xe_tlb_inval*.c`, `xe_guc_submit.c`, `xe_ring_ops.c`, `xe_lrc.c`, `xe_ggtt.c`, `xe_device.c`;
`drivers/gpu/drm/ttm/ttm_bo.c`.

| # | IntelXe before rb8c | Linux xe | rb8c |
|---|---|---|---|
| D1 | A deferred free (page-table page, an unbound BO's VRAM or pages, userptr pages) waited for the unbind's TLB invalidation only | Page-table BOs are created on the VM's dma-resv (`xe_pt_create` -> `xe_bo_create_pin_map(..., vm, ...)`) and dropped with `xe_bo_put_deferred`; `ttm_bo_release` keeps any BO whose resv is not idle for `DMA_RESV_USAGE_BOOKKEEP` (delayed delete). Every exec adds its job fence to the VM resv and each external BO's (`xe_exec.c`: `drm_gpuvm_resv_add_fence(..., BOOKKEEP)`), the bind adds its update and TLB-invalidation fences (`xe_pt_update_ops_run`). Freed memory therefore waits for every job the VM had in flight **and** the invalidation | fixed: the VM's queues are listed (`XeVm::queues`); `deferSeal` snapshots the last seqno each had handed out; `deferReap` frees only once those jobs and the invalidation are done |
| D2 | Splitting a live 2 MB / 1 GB leaf (partial unbind, or a bind inside a huge mapping) wrote the parent entry without widening the invalidation: only the 4 KB pages changed underneath were invalidated | `xe_pt_update_ops_rfence_interval` aligns the update's range to the span of the highest-level entry it wrote; `send_tlb_inval_ppgtt`: "Minimum invalidation size for a 2MB page that the hardware expects is 16MB" | fixed: `ptSplit` touches the whole leaf span (a split 2 MB leaf gives a >= 16 MB invalidation) |
| D3 | `ptNodeNew` filled a new table through write-combined BAR2 and the caller wrote the parent entry right after, no fence between (WC stores to different lines may become visible in any order) | the new table is written when staged (`xe_pt_stage_bind`) and linked in the later commit, with locked operations in between | fixed: `sfence` at the end of `ptNodeNew` |
| D4 | binds, purges and remaps ended their PTE writes with `sfence` | `xe_migrate_update_pgtables_cpu` ends with `xe_device_wmb` (`wmb()` and, on dGFX, a posted register write) | fixed: `deviceWmb` |
| D5 | the migrate VM's copy windows (`vramCopyToSys`, `svCopyBo`, `xeVramRead`) were sealed with `gTlbSeq`, the last invalidation already issued, often complete before the unmap | every unbind's frees wait for an invalidation issued after it | fixed: `migWindowSeal` issues a ranged invalidation of the window (`pmCopyAll`, suspend/resume, left as it was) |
| D6 | a table detached with no memory for the deferred list was freed at once, before its invalidation | never | fixed: leaked and counted |
| D7 | an unbind's out-fence is the bind's clear fence: it signals before the invalidation completes | "tlb invalidation must be done before signaling unbind/rebind" (`xe_pt_update_ops_run`: the out-fence is the invalidation fence) | not changed: here the memory itself waits (D1) and every queue invalidates in its ring before its next batch, so this is user-visible ordering only |
| D8 | the ring invalidates the TLB before the first batch of each queue after a page-table change | for a scratch VM, before every batch (`vm->batch_invalidate_tlb`) | not changed (equivalent for stale scratch entries of one engine); `debug.intelxe.tlb_every_batch=1` gives Linux's rule for an A/B |
| D9 | binds are always CPU page-table writes, in the ioctl, after waiting for the in-fences | CPU only when the bind queue is idle and has no in-fences, else a migrate job ordered by fences (`xe_pt_vm_dependencies`, `xe_migrate_update_pgtables`) | not changed: same order of effects once the waits are done |
| D10 | the GGTT TLB invalidation (an LRC slot reused by a new queue) is a fire-and-forget MMIO write | with the CT up, a GuC `TLB_INVAL_GUC` action whose fence is waited for (`send_tlb_inval_ggtt`) | not changed: it would hit a new queue's first job, the faulting queue was at seqno 16113 |
| D11 | userptr pages stay wired while bound (no MMU notifier) and are unwired at the deferred free | pages released in `xe_vma_destroy_late` after the unbind fence | now after the VM's jobs as well (D1) |
| D12 | `debug.intelxe.unbind_async=0` (debug) waits for the invalidation in the ioctl, not for jobs | - | not changed (debug switch) |

## Hypotheses, ranked

**H1. Memory released by an unbind reused while a job of the VM still runs (D1), made fatal by anything a completed
invalidation does not cover.** A table page or a BO's VRAM goes back to the allocator the moment the GuC reports the
invalidation done. Page-table pages are 4 KB pieces of 64 KB blocks of the BAR window; an empty block returns to the
window's buddy allocator and comes back as another table, an LRC or a CPU-visible BO. If a batch that was running
across the unbind still has anything that names the old page (a page walk in progress, a page-walk cache entry the
ranged invalidation missed (H2), dirty lines it writes back to the old physical address), the reused page is read as a
table or overwritten with data: a garbage entry, a walk to nowhere, a CAT error with no translation fault. Linux never
gets there, because the memory waits for the VM's jobs as well. Evidence: the fault is in the VM that churns (5351
unbinds, one 21 ms before), with every invalidation complete; it appeared when the plugin added binds and unbinds; a
Geekbench batch runs for milliseconds. Predicts: faults scale with bind/unbind churn while jobs run, in the churning
VM, at random points of a batch; with rb8c they stop, and "held by jobs" in `debug.intelxe.stats` grows (each count is
a free that would have happened under a running job before). Dump: `walk bb/acthd` or `tables` showing entries whose
VRAM copy differs from the shadow, a PDE not naming its table, or a leaf "OUT OF RANGE"; `pt: detached ... last dpa`
near the bad table's address.

**H2. A split 2 MB / 1 GB leaf invalidated with a sub-range (D2).** A TLB that cached the 2 MB entry keeps it through
a 4 KB-64 KB invalidation (the hardware wants >= 16 MB for 2 MB pages), so the pages unbound from inside it stay
reachable; once they are freed (H1) and reused the stale entry translates into someone else's memory. VRAM BOs of 2 MB
and more get 2 MB leaves (`debug.intelxe.hugepages=1`); a partial unbind or a bind into a huge mapping splits one.
Predicts: faults only with huge pages on; `pt: splits` moving before the fault (the dump gives the last split's VA,
level and age). Test: `debug.intelxe.hugepages=0` runs, `bindstress ... nohuge` against `huge`.

**H3. A new table read before its contents landed (D3, D4).** A bind writes the new table through write-combining
BAR2 and the entry that links it right after; a walk by a running batch through the new parent entry (a neighbouring
VA in the same 2 MB / 1 GB span, or a prefetch) could read the reused page's old contents. The window is short (the
last bind was 12 ms before the fault), so this ranks below H1/H2. Predicts: a fault shortly after a bind next to an
address the batch uses; by the time of the dump the writes have landed, so `tables` shows no difference. Fixed by
the fence either way.

**H4. Direct submission.** An armed context stays on the engine, polling a mailbox, across binds, unbinds and
invalidations. The ring arithmetic puts the faulting job on the normal path, and the 12 Nov night (direct off, hint on)
had no fault, so this ranks low; the 9-10 Nov correlation with the low-latency hint may be the hint's frequency effect
(SLPC treats the context as compute: higher clocks, more overlap between jobs and binds) rather than submission.
Predicts: faults only with `debug.intelxe.direct=1`; the job's ring layout in the dump (head inside an armed slot) and
the armed guc_id in `debug.intelxe.stats` equal to the faulting one.
Test: bindstress with `direct` 0 and 1, queues `rl`/`cl` alone (they arm) against all four.

**H5. Migrate-window tables freed under the copy engine's page-walk caches (D5).** No move happened before the
12 Nov fault; surface copies (`svCopyBo`) could have. Low. Fixed.

**H6. Context image or GGTT.** A corrupted LRC (PDP0, ASID) or a stale GGTT translation of a reused LRC slot (D10).
The faulting context had run 16113 jobs, which a slot-reuse race would not explain. Dump: `ctx pdp0 ... DIFFERENT`.

**H7. A GPU write into a read-only userptr range.** A range the task cannot write is bound read-only; importing an
unaligned pointer through its surrounding pages can take in a read-only neighbour and make the whole import
read-only, and a kernel writing its buffer then faults. Deterministic per allocation layout, which fits 1 in 6 runs.
Predicts: `userptr read-only` (stats, `pt:` dump line, rb8c) non-zero before the fault.

**H8. Stale scratch/NULL entries (D8).** Wrong data (zeros), not a CAT error; `tlb_every_batch=1` rules it out.

## What rb8c changes (commits on branch `catfault`)

1. `deferSeal(owner, seq, vm)` snapshots the last job of every queue on the VM (`DeferJobs`, shared by the entries of
   one seal); `deferReap` frees an entry once `xeDeferReady(sealed, invalidation done, jobs done)`. The VM_BIND path,
   purges and remaps pass their VM. Without memory for the snapshot the bind waits for the VM's jobs (5 s at most).
2. `ptSplit`: a live huge leaf that is split invalidates its whole span.
3. `ptNodeNew` fences the new table before it is linked; binds, purges and remaps end with `deviceWmb`.
4. `migWindowSeal`: the migrate VM's windows are invalidated after their unmap and their tables wait for that.
5. A detached table with no room on the deferred list is leaked, not freed early.
6. The range normalization (`send_tlb_inval_ppgtt`), the bind interval and the free rule live in `xe_tlbinval.h`;
   `tests/tlbinval_test.cpp` checks them against Linux's code (200000 random ranges) and the split case.
7. Diagnostics: `debug.intelxe.stats` gains "held by jobs", splits, detached, leaked, userptr read-only; the CAT dump
   gains the `pt:` line, the VM's job-held unbinds, the context's PDP0/ASID from its LRC, the page-table walks of
   `RING_BBADDR` and the 64-bit ACTHD with VRAM-against-shadow checks, and a check of every table of the VM (at most
   256 pages, once per 10 s).

The cost: unbound memory is reused only after the VM's running jobs end (as on Linux). A GEM_CREATE that finds the
window full while such frees wait falls back to purging, eviction and system memory as before.

## Telling the hypotheses apart from the next dump

| dump line | H1 | H2 | H3 | H4 | H6 | H7 |
|---|---|---|---|---|---|---|
| `walk`/`tables` VRAM differs from shadow, PDE not its table, leaf OUT OF RANGE | yes | maybe | rarely | - | - | - |
| `pt: splits` recent | - | yes | - | - | - | - |
| `ctx pdp0 ... DIFFERENT` | - | - | - | - | yes | - |
| `userptr read-only` > 0 | - | - | - | - | - | yes |
| head inside an armed slot, armed guc_id (stats) = faulting one | - | - | - | yes | - | - |
| "held by jobs" growing while the workload runs | rb8c at work | | | | | |

A CAT fault that still comes with rb8c, clean `walk`/`tables`, no split, PDP0 the same and no read-only userptr points
away from page tables: next suspects are the batch's own addresses (a user bug, or the translator) and the GuC's
handling of the engine after the reset (the queue that never started).

## rb8d: the items left open by rb8c (branch `kern2`, 12 Nov, untested on hardware)

Linux read for these: v6.18 `xe_pt.c`, `xe_vm.c`, `xe_sync.c`, `xe_ggtt.c`, `xe_tlb_inval.c`, `xe_guc_tlb_inval.c`,
`xe_lrc.c`, `xe_guc_submit.c`; v7.1 `xe_guc_submit.c`, `xe_sched_job.c`, `xe_vm.c`, `xe_pt.c`, `xe_gt.c`,
`xe_hw_engine.c`. Counters: `debug.intelxe.stats` lines "bind out-fences (rb8d)" and "recovery (rb8d)", the system
sleep line. Test: `user/tests/kernel/unbindfence.c` (items 1, 2, optionally 4); host tests `tests/tlbinval_test.cpp`
(sections 5-7) and `tests/recovery_test.cpp`.

| # | rb8c state | Linux | rb8d |
|---|---|---|---|
| D7 / 1 | an unbind's out-fence = the bind's clear: signaled before the invalidation | `xe_pt_update_ops_run`: the out-fence is the invalidation job's fence ("tlb invalidation must be done before signaling unbind/rebind"); a bind queue's fences signal in order; no-op calls signal the queue's last fence (`xe_sync_in_fence_get`) | fixed: `XeFence` gets a TLB timeline (`kXeTlbSlot`, answered from the G2H done counters, nothing waits in the ioctl); out-fence = `XeBindFence` {clears, invalidation}, the VM's previous out-fence folded in; no-op calls signal it; the bind user fence waits for both (bounded, then the late list); `SYNCOBJ_TRANSFER` of a timeline point carries every fence at or below it (was the first only); an invalidation unanswered 1 s after issue counts as done, logged (`xe_tlb_inval_fence_timeout`). EXEC on a bind queue (an IntelXe extension; Linux `EINVAL`) still signals once its in-fences have |
| D10 / 2 | GGTT invalidation: MMIO write on one GT, never waited for | `xe_ggtt_invalidate` -> both GTs -> `xe_tlb_inval_ggtt`: GuC `TLB_INVALIDATION` {`XE_GUC_TLB_INVAL_GUC`}, fence waited for; MMIO (not waited for) only without the CT. LRC and CT BOs are `XE_BO_FLAG_GGTT_INVALIDATE`: insert and removal wait | fixed: `xeGgttInvalidate` (GuC action to every GuC that is up, MMIO while the CT is not), `xeGgttInvalWait` (1 s, counted); queue creation waits before the GuC hears of the queue, `ggttClear` before the DMA unmap and before the slot (its window) is freed, outside `gResLock` |
| D5' / 3 | `pmCopyAll` (suspend/resume) sealed its window with `gTlbSeq` and freed the backups right after on resume | an unbind's tables and pages wait for an invalidation issued after it and for the VM's jobs | fixed: `migWindowSeal(ctx, gMigVm)` (ranged invalidation after the unmap, tables wait for it and the migrate queue's jobs), waited for (bounded) before `pmCopyAll` returns; map side ends with `deviceWmb` |
| 4 | after a GuC context reset the next queue's job never started and waited the 10 s job limit for its GT reset | v7.1: CAT / CONTEXT_RESET run the queue's TDR at once (ban, immediate schedule disable, GT reset if unconfirmed in 5 s: as `queueKickOff`); other queues' TDR fires at `job_timeout_ms` = 5 s and resets the GT for a job that never started (`timeout_needs_gt_reset`, twice per job) | a never-started job is judged at 5 s (`xeNotStartedLimitMs`); IntelXe's own shortcut: within 30 s of a GuC context reset in a reset domain (render + compute are one, `xe_gt_any_hw_engine_by_reset_domain`), a job of that domain not started 2 s after both the reset and its submission resets the GT (same twice-per-job rule, then taken off); cleared by every GT reset (`xe_recovery.h`) |
| D8 / 5 | the ring invalidates before a queue's first batch after any page-table change (`ptGen`), every batch for multi-engine queues; `tlb_every_batch=1` every batch on scratch VMs | non-LR scratch VM: every batch (`batch_invalidate_tlb`, `xe_sched_job_arm`); otherwise only after a rebind (`tlb_flush_seqno`) | default kept (0). Linux needs every batch because a bind over a scratch entry does not bump `tlb_flush_seqno`; here every page-table change bumps `ptGen`, so each queue's next batch after a bind invalidates, on the engine it runs on (multi-engine queues: every batch). The one difference: a job emitted to the ring before a bind and started after it does not see that bind without its own invalidation, which only matters to a job not ordered after the bind's out-fence. A/B so far: 31 Oct (rb7k), `tlb_every_batch=1` did not change the Apple-GL flicker (stale TLB ruled out there); no CAT-fault run has used 1 yet; the night plan runs bindstress with 0 and 1 |
