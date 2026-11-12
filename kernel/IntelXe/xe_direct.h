// SPDX-License-Identifier: MIT
// rb8e: the pure part of direct submission's user fences (xe_core.cpp "direct submission"), so that tests/direct_test.cpp
// builds it on the host.
//
// The armed ring slot (dsSlot) is written when the queue arms, before the job that will run in it is known; what it
// runs comes from memory written at submit time (dsSubmit) before the mailbox releases the slot: the job's stub (jobPre
// and the jump to its batch) and, after the batch and the cache flush, its post-stub, which the slot calls with a
// first-level MI_BATCH_BUFFER_START (GGTT) where the normal ring path stores the user fence (jobPost). The post-stub is
// the job's user fence or nothing, as xe_ring_ops.c emits it: render and compute store it right after the flush
// (__emit_job_gen12_render_compute), copy and video after a plain MI_FLUSH_DW (__emit_job_gen12_simple / _video:
// emit_flush_dw), both with emit_store_imm_ppgtt_posted (MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1), the qword at a PPGTT
// address); then MI_BATCH_BUFFER_END back to the slot, which stores the seqno and raises the interrupt, so a waiter
// woken by it reads the value. WAS: a job with a user fence made the armed queue leave direct submission (queueExec),
// since the slot had no store for it.
#pragma once
#include <stdint.h>

static constexpr uint32_t kXeDsPostBytes = 64;   // one post-stub (at most 11 dwords used)

// The post-stub of a job into dw (kXeDsPostBytes / 4 dwords at most); returns the dword count. rc: render or compute.
static inline uint32_t xeDsPostStub(uint32_t *dw, bool rc, bool hasUf, uint64_t addr, uint64_t value) {
	uint32_t n = 0;
	if (hasUf) {
		if (!rc) {   // emit_flush_dw: MI_FLUSH_DW | MI_FLUSH_IMM_DW, no post-sync
			dw[n++] = 0x26u << 23 | 2;
			dw[n++] = 0;
			dw[n++] = 0;
			dw[n++] = 0;
		}
		dw[n++] = 0x20u << 23 | 1u << 21 | (2 * 1 + 3 - 2);   // MI_STORE_DATA_IMM | MI_SDI_NUM_QW(1), PPGTT
		dw[n++] = (uint32_t)addr;
		dw[n++] = (uint32_t)(addr >> 32);
		dw[n++] = (uint32_t)value;
		dw[n++] = (uint32_t)(value >> 32);
	}
	dw[n++] = 0xau << 23;   // MI_BATCH_BUFFER_END
	return n;
}
