# Display boost: the GT clock while frames miss their vblank

> Design note with its date (2026-11-14, branch `dispboost`): it records the plan and the state of that day, not the
> current status. Default on while it is measured on the B580; the measurement decides the default.

## 1. The problem (B580, macOS 26, 180 Hz, HiDPI 4096x2304 backing, 14 Nov)

`user/tests/wsi/p4check.m smooth -v` (a 640x360 pt CAMetalLayer window, displaySync on, 3 drawables, one clear per
frame): in steady state 0 of ~1790 frames late in 10 s. Started after the GPU was idle, the first ~370 ms run at 90 Hz
(every frame two refreshes, ~20-23 late frames, the app's `nextDrawable` blocks ~8 ms), then 180 Hz. With
`sysctl debug.intelxe.slpc_min=2850` the same start has 0-1 late frames. So what is left is the SLPC ramp from idle (floor
1200 MHz, RP0 2850): WindowServer's composite at this size is ~1.37 ms per command buffer and ~3.6 command buffers per
frame, which at the idle clock does not fit in 5.56 ms. SLPC raises the clock from its own busyness samples, which
take a few hundred ms to see a desktop that has just started animating.

## 2. What Linux does (read in torvalds/linux v7.4-rc2, commit 7475ea8b5861, 8 Nov 2026)

### i915: a boost when a flip misses its vblank

- `drivers/gpu/drm/i915/display/intel_plane.c` `intel_prepare_plane_fb()`: for a plane state with a fence,
  `intel_display_rps_boost_after_vblank(crtc, fence)`, then `intel_display_rps_mark_interactive(..., true)`.
- `drivers/gpu/drm/i915/display/intel_display_rps.c` `intel_display_rps_boost_after_vblank()`: if the parent driver has an
  rps interface (`intel_parent_rps_available()`), takes a vblank reference and adds a wait entry to the crtc's vblank wait
  queue. At the next vblank `do_rps_boost()` calls `intel_parent_rps_boost_if_not_started(display, fence)` once and frees
  the entry. Its comment: "If we missed the vblank, but the request is already running it is reasonable to assume that
  it will complete before the next vblank without our intervention, so leave RPS alone if not started."
- `drivers/gpu/drm/i915/gt/intel_rps.c` `boost_if_not_started()` (the `i915_display_rps_interface`): an i915 fence whose
  request has not started (`!i915_request_started(rq)`) gets `intel_rps_boost(rq)`.
- `intel_rps_boost()` (same file): nothing for a signalled or already boosted request, nothing for a context with
  `CONTEXT_LOW_LATENCY`; sets `I915_FENCE_FLAG_BOOST`; with GuC SLPC nothing under the power-saving profile
  (`SLPC_POWER_PROFILES_POWER_SAVING`), else `atomic_fetch_inc(&slpc->num_waiters)` and, on the first waiter, unless
  `min_freq_softlimit >= boost_freq`, queues `slpc->boost_work`.
- `drivers/gpu/drm/i915/gt/uc/intel_guc_slpc.c` `slpc_boost_work()` -> `slpc_force_min_freq(slpc, slpc->boost_freq)`: a
  non-blocking `SLPC_PARAM_GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ` that leaves the user's soft limit alone. `boost_freq` is RP0
  unless set (`slpc_get_rp_values`; sysfs `gt_boost_freq_mhz`, `intel_guc_slpc_set_boost_freq`).
- It ends with the request: `drivers/gpu/drm/i915/i915_request.c` `i915_request_retire()` -> `intel_rps_dec_waiters()` ->
  `intel_guc_slpc_dec_waiters()`: when `num_waiters` drops to 0, `slpc_force_min_freq(slpc, min_freq_softlimit)`. No
  timer: the floor stays up exactly while some boosted request is unfinished.
- `intel_rps_mark_interactive()` only changes host RPS thresholds; with SLPC `intel_rps_enable()` returns before
  `intel_rps_set_active()`, so it does nothing.

### i915: waitboost

- `i915_request.c` `i915_request_wait_timeout()`: `if (flags & I915_WAIT_PRIORITY && !i915_request_started(rq))
  intel_rps_boost(rq)` before sleeping. `I915_WAIT_PRIORITY` comes from `i915_fence_wait()` (the dma_fence `.wait`, so any
  `dma_fence_wait()` on an i915 fence), `gem/i915_gem_wait.c` (GEM_WAIT; it first boosts every not-started fence of the
  reservation, `i915_gem_object_boost`) and `gem/i915_gem_domain.c` (set-domain).
- `drivers/gpu/drm/drm_syncobj.c` `drm_syncobj_array_wait_timeout()` does not call the fence's `.wait`: it adds callbacks
  and sleeps, and calls `dma_fence_set_deadline()` only when the caller passes `DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE`.
  Neither i915 nor xe implements `.set_deadline`. So a Vulkan driver's fence waits (syncobjs) never waitboost, on i915
  either. (msm does use deadlines: `drivers/gpu/drm/msm/msm_fence.c` `deadline_work()` -> `msm_devfreq_boost()`, with
  `drm_atomic_helper.c` `set_fence_deadline()` giving plane fences the next vblank start.)

### xe: nothing of the kind

- `drivers/gpu/drm/xe`: no occurrence of "boost" at all.
- The display code is the same (`xe/Makefile` builds `i915-display/intel_display_rps.o`), but `xe/display/xe_display.c`'s
  `intel_display_parent_interface` has no `.rps`, so `intel_parent_rps_available()` is false and
  `intel_display_rps_boost_after_vblank()` returns at once.
- xe leaves the clock to SLPC: `xe_guc_pc.c` sets `SLPC_OPTIMIZED_STRATEGY_COMPUTE` and the power profile (sysfs
  `power_profile`, `xe_gt_freq.c`), and a queue created with `DRM_XE_EXEC_QUEUE_LOW_LATENCY_HINT` gets
  `SLPC_CTX_FREQ_REQ_IS_COMPUTE` in its GuC policy (`xe_exec_queue.c`, `xe_guc_submit.c`).

## 3. What this stack has

- The present path (stage 2/3 of `framepath.md`, phase 4): WindowServer does not wait for its composite. The Metal
  plugin marks the composite surface pending at commit and ready when its GPU work is done; NMXAccel's present worker
  (`nullmoth/kexts/NVRM/accel/nvrm-accel.cpp` `nmxPresent`) waits for the mark (`nvSurfWaitReady`), then flips (or
  copies). That wait is the one place the kernel knows "this frame is presented, its GPU work is not done": the
  equivalent of i915's plane fence at `prepare_fb`.
- The vblank: IntelXeFB times it (`vblFire`, `usSinceVblank()`: `TIMESTAMP_CTR - PIPE_FRMTMSTMP`, microseconds) and
  publishes the frame length (`NVRMFlipWaitUs`, 5/4 of a frame).
- The clock: `xe_slpcboost.h`'s lease on SLPC's minimum (slpc_boost, 13 Nov), with its timer, the sleep handling and
  the rule that the minimum the GuC gets is the highest of the administrator's floor and the leases, clamped to
  `slpc_max`.
- CPU waits on GPU fences: `xe_core.cpp` `syncobjWait` (SYNCOBJ_WAIT / TIMELINE_WAIT) and `waitUserFence`.

## 4. The choice

(a) display boost: implemented. (b) waitboost: not implemented, because:

- Linux xe has none, and i915 has none on the path a Vulkan driver uses (syncobj waits, section 2);
- here every WindowServer composite is waited for by the plugin's worker (`vkWaitForFences`), so a waitboost without
  i915's "not started" test would fire on every desktop frame, video included; the kernel has no cheap "has this job
  started" per fence;
- the measurement points at the display: the late frames are composites; (a) sees them directly.

## 5. Design

**Trigger (NMXAccel, `nmx_vbldue.h`, `nmxPresent`).** When the present worker takes a frame whose surface is still
pending, it works out when the frame was due: the first start of vblank after `t0`, the time the worker took it (its
present, or, for a frame that waited behind another, the landing of that one). The phase comes from IntelXeFB's
`nmxScanout` op 4 (NMXScanout version 4: `vblAgoNs` since the last start of vblank, `frameNs`); without it (an older
IntelXeFB, the second pipe) one frame after `t0` (any interval of one frame holds a start of vblank). The wait
(`nvSurfWaitReadyDue`) sleeps until the due time; a frame still pending then missed its vblank: NMXAccel counts it
(`debug.nmxaccel_vbl_miss`) and calls IntelXe's `intelxeDispBoost` once, then waits on as before. Like i915, the boost is
asked at the vblank, while the late frame is still running. Unlike i915 there is no "not started" test (no cheap
per-frame started bit for a plugin mark): "not done at the vblank" is the condition.

**Lease (IntelXe, `xe_slpcboost.h` `XeDispLease`, `xe_core.cpp` `dispRun`).** `intelxeDispBoost` never blocks: it counts
the miss and enters a thread call that takes `gDevLock` and applies the rules:

- a miss while a display lease runs renews it: it ends `disp_boost_ms` (50 ms) after the last miss;
- without a lease, misses less than a lease length apart form a streak; the `disp_boost_misses`-th (2) starts a lease,
  so one late frame of an idle desktop does not lift the clock (`1` = i915's every miss);
- at most 5 new leases a second (renewals send nothing and are not counted);
- the floor is `disp_boost_mhz`, 0 = the ceiling (RP0, i915's `boost_freq`), always clamped to [RPn, `slpc_max`]; the
  GuC gets the highest of `slpc_min`, a `slpc_boost` lease and the display lease, sent as slpc_boost sends it
  (`IGNORE_EFFICIENT_FREQUENCY` + `GLOBAL_MIN_GT_UNSLICE_FREQ_MHZ`); the lease's end arms the slpc_boost timer, which
  sends the floor back;
- nothing while switched off, while `debug.intelxe.freq_manual` is set, with the power-saving SLPC profile
  (`slpc_profile 1`, as i915's `intel_rps_boost`) or while the GT cannot take a new minimum (reset, sleep, no SLPC);
  each is counted;
- system sleep ends the lease (`xeBoostSleep`), and so does writing 0 to `disp_boost` (the floor goes back at once).

Why a lease and not i915's "until the request retires": the late frame's work is done in a few ms, and SLPC's own
estimate needs longer than that to follow; a 50 ms lease renewed by each miss holds the clock while frames keep missing
and lets go 50 ms after the last one. The cost is bounded by the misses: a desktop whose frames are on time never boosts.

**Knobs and counters.**

| Name | What |
|---|---|
| `debug.intelxe.disp_boost` | 1 on (default), 0 off (ends a running lease); root writes |
| boot-arg `-intelxe_nodispboost` | 0 from the start |
| `debug.intelxe.disp_boost_ms` | lease length, 10-200 ms (200 since the B580 results below; was 50) |
| `debug.intelxe.disp_boost_misses` | streak that starts a lease, 1-8 (1; was 2) |
| `debug.intelxe.disp_boost_mhz` | floor, 0 = RP0 / `slpc_max` (0) |
| `debug.intelxe.disp_boost_stats` | anyone: `disp_boost lease_ms misses_needed floor_mhz active left_ms misses grants renewals streak ratelimited off manual powersave notready expiries sleepends switchoffs boosted_ms` (name value pairs; also in `debug.intelxe.dump` as `[disp_boost]`) |
| `debug.nmxaccel_vbl_miss` | frames still on the GPU at the vblank they were due at (counted with the boost off too) |
| `debug.nmxaccel_vbl_due` / `_nophase` / `_unknown` | presents of a pending frame, due from IntelXeFB's phase / one refresh after t0 / not checked |
| `debug.nmxaccel_vbl_boost_fail` | misses IntelXe did not take (an older IntelXe.kext) |

## 6. Measuring it (`user/tests/wsi/night-dispboost.sh`)

For boost off and on, five times each, interleaved (ABBA): wait until the GT is idle (`debug.intelxe.sensors` act at or
below the idle clock for 3 s), run `p4check smooth -t 3 -v` with the GT clock sampled every 25 ms, and record the late
frames in the first 500 ms and overall, the clock ramp, `debug.nmxaccel_vbl_miss` and the boost counters. Then 20 s of
idle desktop with each setting (and 20 s of video if a player is given): misses, leases, time boosted, mean clock and
the card's average power from the PMT energy counter. What decides the default: late frames in the first 500 ms with
the boost on near the `slpc_min=2850` result (0-1), and at idle / video few leases and a power difference within the
noise.

## 7. Files

- `kernel/IntelXe/xe_slpcboost.h`: `XeDispLease`, `xeDispMiss`, `xeDispEnd`, `xeDispSwitchOff`, and the display lease in
  `xeBoostEffectiveMin` / `xeBoostExpire` / `xeBoostSleep` / `xeBoostNextNs`.
- `kernel/IntelXe/xe_core.cpp`: `dispRun`, `xeDispBoostMiss`, `xeDispBoostSet`, `xeDispBoostStats`, `gDispCall`.
- `kernel/IntelXe/IntelXe.cpp`: the sysctls, the boot-arg, the dump section; `IntelXeClient.cpp`: `intelxeDispBoost`.
- `kernel/IntelXeFB/IntelXeFB.cpp`: NMXScanout version 4 (op 4 answers the vblank phase).
- `nullmoth/kexts/NVRM/accel/nmx_vbldue.h`, `nvrm-accel.cpp` (`nmxVblDueAbs`, `nmxVblMissed`, `nvSurfWaitReadyDue`).
- Tests: `kernel/IntelXe/tests/dispboost_test.cpp`, `nullmoth/kexts/NVRM/accel/tests/nmx_vbldue_test.cpp`.

## B580 results and the idle-exit trigger (14 Nov 2026)

First run with the defaults above (2 misses within 50 ms, 50 ms lease): no change (about 23 late frames per start from
idle, boost off or on). NMXAccel saw only ~3 misses per start: in the ~370 ms SLPC ramp WindowServer composites every
other refresh, but its frames are rarely still on the GPU at the vblank they were due at, so the miss trigger fires
seldom and late. Measured instead:

| Setting (8 starts from idle each, `p4check smooth -t 3 -v`) | late frames (sum) | per start | fps |
|---|---|---|---|
| boost off | 206 | 23-42 | 170.8 |
| boost on: 1 miss starts a 200 ms lease, plus the idle-exit trigger | 16 | 0-5 | 179.0 |

Reference: `slpc_min` pinned at 2400 MHz gives 1-25 late per start; 2000 MHz gives 4-5.

**Idle-exit trigger** (NMXAccel, `debug.nmxaccel_idle_boost_ms`, default 250, 0 = off; counter
`debug.nmxaccel_idle_boosts`): the first frame after the display was quiet that long asks IntelXe for the boost at once,
so the clock is raised when the ramp starts, not after frames have already been missed. Defaults changed to
`disp_boost_misses` 1 and `disp_boost_ms` 200.

Cost: idle desktop 20 s, 36.58 W off / 36.57 W on, no leases. Periodic small updates (a window changing colour every
600 ms, like a blinking caret): a lease on every update (31% of the time boosted), card power 36.70 / 37.96 W off,
36.66 / 36.65 W on, so no measurable cost. Video playback was not sampled (QuickTime automation was refused); it
presents without quiet gaps, so only the miss trigger can fire there.
