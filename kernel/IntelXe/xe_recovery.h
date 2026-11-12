// SPDX-License-Identifier: MIT
// rb8d: the pure parts of the job watchdog's recovery after the GuC resets a context (xe_core.cpp watchRun), so that
// tests/recovery_test.cpp builds them on the host.
//
// Linux (v7.1 xe_guc_submit.c): a CONTEXT_RESET_NOTIFICATION or NOTIFY_MEMORY_CAT_ERROR
// (xe_guc_exec_queue_reset_handler, xe_guc_exec_queue_memory_cat_error_handler: "Treat the same as engine reset") runs
// the queue's TDR at once (xe_guc_exec_queue_reset_trigger_cleanup): banned, scheduling disabled with the minimum
// preemption timeout, a GT reset if the GuC does not confirm within 5 s; ENGINE_FAILURE_NOTIFICATION (the GuC could not
// reset the engine) is a GT reset (xe_guc_exec_queue_reset_failure_handler). Every other queue goes on as before, and
// one whose job the GuC never starts is caught by its own TDR after job_timeout_ms, 5 s by default (xe_hw_engine.c
// hw_engine_init_early), which resets the GT for a job that never started (timeout_needs_gt_reset), twice per job
// (xe_sched_invalidate_job(job, 2)), and bans the queue after that.
//
// IntelXe's watchdog used one limit, debug.intelxe.job_timeout_ms (10 s, Linux's DRM_XE_JOB_TIMEOUT_MAX, since a
// started job's run time is checked on top), for a never-started job too, so the queue after a CAT fault waited 10 s
// (12 Nov: "seqno 3543 never started in 10020 ms: GT reset"). Now:
//  - a job that never started gets Linux's 5 s (xeNotStartedLimitMs: job_timeout_ms if lower), as Linux's TDR fires at
//    job_timeout_ms and does not wait for a run time a job that never started cannot have;
//  - for a while after the GuC reset a context of a reset domain (xeResetDomain: render and compute are one), a job of
//    that domain that has not started 2 s after both the reset and its own submission
//    (xeStuckAfterReset) means the engine kept the reset context (the 12 Nov dump: RING_CURRENT_LRCA unchanged, ACTHD 0)
//    and the GT is reset then rather than after the timeout. This is IntelXe's own shortcut (bounded, logged and counted:
//    debug.intelxe.stats "recovery"), taken under the same twice-per-job rule.
#pragma once
#include <stdint.h>

static constexpr uint64_t kXeNotStartedMs = 5000;            // xe_hw_engine.c: job_timeout_ms = 5 * 1000
static constexpr uint64_t kXeStuckAfterResetNs = 2000000000ull;
static constexpr uint64_t kXeResetWindowNs = 30000000000ull;   // how long after a GuC context reset the shortcut applies

// The watchdog's limit for a job that never started, in ms (0: the watchdog is off).
static inline uint64_t xeNotStartedLimitMs(uint64_t jobTimeoutMs) {
	return jobTimeoutMs && jobTimeoutMs < kXeNotStartedMs ? jobTimeoutMs : jobTimeoutMs ? kXeNotStartedMs : 0;
}

// The reset domain of a primary-GT engine class (DRM_XE_ENGINE_CLASS_*): render (0) and compute (4) share one
// (xe_gt.c xe_gt_any_hw_engine_by_reset_domain; the GuC's engine-reset save list carries RCU_MODE / CCS_MODE once for
// that domain, xe_guc_ads.c guc_mmio_regset_write), every other class is its own (domain = class + 8). -1: unknown.
static inline int xeResetDomain(uint16_t drmClass) {
	if (drmClass == 0 || drmClass == 4) return 0;
	return drmClass < 8 ? 8 + drmClass : -1;
}

// A job of a queue in the domain of a GuC context reset that happened at resetNs (0: none): outstanding since sinceNs
// (the watchdog's first sight of it), started or not. True: stuck behind the reset context.
static inline bool xeStuckAfterReset(uint64_t now, uint64_t resetNs, uint64_t sinceNs, bool started) {
	if (started || !resetNs || !sinceNs || now < resetNs || now - resetNs > kXeResetWindowNs) return false;
	const uint64_t from = sinceNs > resetNs ? sinceNs : resetNs;
	return now >= from && now - from >= kXeStuckAfterResetNs;
}
