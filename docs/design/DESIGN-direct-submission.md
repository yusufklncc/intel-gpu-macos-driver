# Direct submission in IntelXe.kext (design, 2026-11-09)

> Design note with its date: it records the plan and the state of that day, not the current status.
>
> Update (2026-11-10): merged and released in 0.1.4, still off by default (`debug.intelxe.direct=1` until the next
> restart); one armed queue at a time. Measured on the B580: a tiny job's exec -> signal 46-52 -> 15-17 us, Geekbench 7
> Metal 97.2k, OpenCL 92.9k. Current state: the README, `docs/ROADMAP.md` and `docs/devices/b580.md`.
>
> Update (2026-11-12): after an unattended night (idle, sleep and wake, repeated Geekbench runs: no fault) and a power
> check (no idle cost; +7 W card power while a stream of tiny jobs keeps a queue armed, open item 1 below), on by default
> in the tree (`-intelxe_nodirect`: off). Numbers: `docs/ROADMAP.md`, "Submission cost and latency".

## Why

One tiny Metal command buffer (one dispatch) takes 56 us from commit to the waiter waking at 2850 MHz, 94 us at the
1200 MHz SLPC floor; Windows takes 5.4 us (`user/tests/metal/mtl_lat.m split`). The job-timing kext
(`debug.intelxe.jobtime`, RING_TIMESTAMP stores in the ring) splits the 56 us:

| step | us (2850 MHz) |
|---|---|
| exec ioctl entry to H2G SCHED_CONTEXT | 2.7 |
| H2G to the ring starting the job (GuC scheduling, context load) | **29** |
| pre-batch invalidate | 1.5 |
| the batch | 8.6 |
| end-of-job flush and seqno store | 0.8 |
| to the interrupt | ~7 |
| interrupt to the waiter running | 3 |

The largest step is the GuC putting the context on the engine for every job. `user/tests/kernel/ulls.c` (4e7a52b)
showed that a context which stays on the engine, polling a mailbox with `MI_SEMAPHORE_WAIT`, starts a job 4.0-4.6 us
after the CPU's write, against 28.5-34.7 us through the exec path. That is Intel's "ULLS" (ultra-low-latency
submission) of the compute runtime, done there in user space.

## Where: the kernel, not Mesa or the plugin

User-space ULLS (the compute runtime's way) bypasses the kernel's fences: every syncobj, IOSurface mark, the
WindowServer handoff and the job watchdog would have to learn about jobs the kernel never saw. In the kext, nothing
above it changes: `DRM_IOCTL_XE_EXEC` stays the submission call (2.7 us), seqnos, fences, syncobjs, bans and GT-reset
replay keep working, and only the path from the ioctl to the engine gets shorter. Mesa and the Metal plugin stay
as they are.

## How: the ring keeps one slot armed

Today `queueEmitEx` writes a job's dwords into the context's ring (GGTT), sets the LRC tail and sends
SCHED_CONTEXT (0x1000). In direct mode a queue's ring always ends with one **armed slot** for the next seqno `s`:

```
[MI_SEMAPHORE_WAIT  poll, GGTT mailbox >= s]          // the context stays on the engine here
[pre-job PIPE_CONTROL invalidate]                       // fixed (jobDwords' first part)
[MI_BATCH_BUFFER_START first level, GGTT -> stub[s % N]]
[end-of-job flush, seqno s store, MI_USER_INTERRUPT, MI_ARB_CHECK]   // fixed: s is known in advance
```

and the LRC tail ends right after it. The stub (a few dwords in kernel GGTT memory, one per slot) is written at
submit time: an optional TLB-invalidating PIPE_CONTROL when the VM's page tables changed, then a chained
`MI_BATCH_BUFFER_START` (first level, PPGTT) to the application's batch, whose `MI_BATCH_BUFFER_END` returns to the
ring. Prefetch is safe: the ring dwords are all written before the slot is armed, and the stub is only read after
the jump that follows the semaphore.

Submitting job `s` in direct mode: write the stub, `deviceWmb`, write `s` to the mailbox (the latency path ends
here), then append the armed slot for `s + 1` and move the tail with SCHED_CONTEXT. That H2G is off the latency
path: the context is already running, so the GuC only updates its tail.

**Leaving direct mode** (no job for `debug.intelxe.direct_idle_us`, default about 2 ms; another queue submitting to
the same engine class; suspend; a GT reset): the kext submits a no-op job into the armed slot (stub = a bare
`MI_BATCH_BUFFER_END`, seqno consumed like an empty kernel job) **without** arming another slot. The ring runs dry,
the context goes idle and the GuC switches it out, so the engine can reach RC6 and other contexts run without
timeslicing against a polling one. The next job takes the normal path (SCHED_CONTEXT) and re-arms.

**Entering direct mode**: a queue with the low-latency hint (`DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT`, which the plugin
already sets for applications and never for WindowServer and the system UI) on the compute class whose jobs arrive
less than about the idle limit apart. Render stays on the normal path: WindowServer shares the render engine and
would be timesliced against a polling context.

*Changed after the first measurements (9 Nov, evening):* the GuC low-latency hint became opt-in (a GPU fault after
long idle), and Geekbench's OpenCL and Metal work turned out to run on the render class (its faults named class 0).
Now any application queue of the render or compute class may arm, the hint no longer matters, and the kext keeps
WindowServer and the system's interface out by process name. On the render class WindowServer's next frame is
"another queue on the armed one's class": the armed queue leaves before that frame goes to the ring. Measured with
the compute class first: a 180 fps animation stayed at 179 fps with direct mode on or off under load.

## What stays the same

- Fences: the seqno store and user interrupt are in the ring as now; `done` and `submitted` mean what they mean now.
  A queue waiting at an armed slot has `done == submitted`, so the watchdog sees it as idle and times only real jobs.
- Faults and bans: a CAT error or context reset still arrives per guc_id, so the queue is banned as now; the armed
  slot goes with the ring.
- GT reset: replay rewinds the ring head to the first unfinished job as now; the armed slot is just ring contents.
  The mailbox is reset to the replayed seqno.
- Suspend: leave direct mode first, as `xeSuspend` already drains the queues.
- Long-running VMs (branch `lrmode`, DESIGN-lr-mode.md section 3): an LR queue arms and leaves under the same rules; the
  LR preemption (a BO move, a deferred free's cycle) makes an armed queue leave before it closes the ring ("other").

## Risks and checks before code

1. A context polling `MI_SEMAPHORE_WAIT` keeps SLPC busy and the GT out of RC6 while armed. Measure the power with
   the idle limit at 0.5, 2 and 10 ms (`step=20` energy counter) against the latency it buys.
2. GuC timeslicing against a polling context: with two direct-mode queues on the CCS, the second must not wait up to
   one timeslice. The "another queue submits, leave direct mode" rule covers it; test with two `ulls`.
3. The jump to a GGTT stub from the ring and the chained jump from the stub to a PPGTT batch: verify on the hardware
   (kext build behind `debug.intelxe.direct`, off by default).
4. SCHED_CONTEXT for a context that is running a job: confirm (job timing) that the GuC moves the tail without
   preempting or stalling the job.
5. The `ulls chain` mode (a second-level batch that chains, as an ANV batch would if called at the second level)
   tells whether the alternative layout (stub called at the second level, post-job commands in a persistent batch)
   is available.

## Expected gain

The 29 us of GuC scheduling becomes about 4 us. A tiny command buffer would go from about 56 to 31 us at 2850 MHz
and from 94 to about 55 us at 1200 MHz. Windows is still ahead in the batch (8.6 us), the interrupt path (10 us)
and the plugin's CPU cost per command buffer (back to back 18-21 us against 1.9: the plugin's class-wide lock),
which is separate work.

## Implementation (branch direct-submit, 2026-11-09, untested on the hardware)

`xe_core.cpp`, section "direct submission"; `debug.intelxe.direct` (default 0) and `debug.intelxe.direct_idle_us`
(default 2000); counters in the "direct submission" line of `debug.intelxe.stats`. Differences from the plan above:

- The whole pre-job part (preparser off, the invalidating PIPE_CONTROL, preparser on, CTX_TIMESTAMP and start seqno)
  is in the stub, not in the ring: the TLB-invalidate bit depends on the job, and one PIPE_CONTROL is cheaper than two.
  The ring slot is preparser off, `MI_SEMAPHORE_WAIT`, preparser on (as the compute runtime disables the prefetcher
  around its semaphore), the jump to the stub, and `jobPost`.
- Back-to-back jobs: each submit arms the slot for the next seqno before it returns, so a job submitted while the
  previous one runs finds its slot armed; there is no fallback to the normal path for it.
- Entering: a job on the normal path arms the next slot when the queue's previous job was less than the idle limit ago
  and no other queue used the compute class in that time; so the first two jobs of a burst take the normal path.
- An exec that has to wait in the FIFO (in-fences, a held queue) makes the queue leave first: an armed queue's FIFO is
  always empty, so the no-op never takes a seqno a fence already has.
- A GT reset makes the queue leave before the reset; the replay then passes every slot left in the ring, since the
  mailbox only grows within a stretch, and a new stretch starts only once the last one's slots have all run.
- Stubs: 256 per queue (one per `ringPos` entry; job x's stub is written only after job x - 256 completed), 32 entries
  of 32 KB plus a mailbox page in system memory, GGTT-mapped at the first `debug.intelxe.direct=1`.
- rb8e (12 Nov, untested on the B580): exec user fences. Until then a job carrying `DRM_XE_SYNC_TYPE_USER_FENCE` made
  the armed queue leave (the slot's `jobPost` is written at arming time and had no store for it), so with ANV's
  `ANV_UFENCE=1` (a user fence on every exec with a batch) no queue stayed armed. Now the slot's `jobPost` calls a
  per-seqno **post-stub** (first-level `MI_BATCH_BUFFER_START`, GGTT) between the end-of-job flush and the seqno store,
  where the ring path stores the user fence: it holds `MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1)` to the fence's PPGTT
  address (render/compute; copy/video would get `MI_FLUSH_DW` first, as `xe_ring_ops.c`) and `MI_BATCH_BUFFER_END`, or
  the return alone (`xe_direct.h xeDsPostStub`). It is written with the stub before the mailbox and follows the stubs'
  reuse rule (256 per queue, 64 bytes each, after the stubs in the same GGTT-mapped memory). Counted:
  `debug.intelxe.stats` "submits N (with a user fence M)".
