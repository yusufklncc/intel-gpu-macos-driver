// SPDX-License-Identifier: MIT
// engact: per-engine busy time from the GuC's engine activity statistics, for Level Zero sysman's zesEngineGetActivity
// (debug.intelxe.sensors_raw version 2, uapi/intelxe_sensors.h). The pure parts, so that tests/engact_test.cpp builds
// them on the host; the buffers, the H2G and the register reads are in xe_core.cpp (engActEnable, xeEngActSample).
//
// Linux (v7.4-rc3 drivers/gpu/drm/xe/xe_guc_engine_activity.c, read 13 Nov 2026; the PMU events engine-active-ticks and
// engine-total-ticks of xe_pmu.c): when the GuC's submission interface is 1.14.1 or newer (BMG's 70.72.1 firmware has
// 1.38.1), xe_guc_engine_activity_enable_stats sends XE_GUC_ACTION_SET_DEVICE_ENGINE_ACTIVITY_BUFFER (0x550C) with the
// GGTT addresses of a metadata page (struct guc_engine_activity_metadata) and an activity buffer (struct
// guc_engine_activity_data: one 16-byte struct guc_engine_activity per [GuC class][logical instance], 16 x 32), after
// pc_start and rc_enable on every GuC load. The GuC then keeps, per engine, the ticks it was active and the tick it went
// active at (0 when idle), with a change number per record and a global one in the metadata. Reading:
//  - active ticks (get_engine_active_ticks): when the global change number moved and the engine's own did, the active
//    ticks since the last read are added to a running total; while the engine runs, the time since it went active
//    (the GPM timestamp, MISC_STATUS_0 >> (3 - RPM_CONFIG0 CTC shift), low 32 bits, minus last_update_tick) is added on top;
//  - total ticks (get_engine_total_ticks): the CPU time since the statistics were enabled, scaled by the engine's
//    quanta_ratio / 0x8000 (its share under SR-IOV; the whole in native mode), in GuC timestamp ticks
//    (guc_tsc_frequency_hz from the metadata).
// The same arithmetic here, with two differences, both because the kext reloads the GuC at every GT reset and wake and
// clears the buffers before it hands them over again: at each enable the records' snapshot is re-based to zero (Linux
// keeps it, and a GuC whose counters restarted would make active go backwards by the old count) while the running
// totals stay, so both counters only grow; and a GPM timestamp that reads 0 or all ones (a GT that went down between
// the record and the register) does not move the running engine's part.
#pragma once
#include <stdint.h>

static constexpr uint32_t kXeEaClasses = 16;        // GUC_MAX_ENGINE_CLASSES
static constexpr uint32_t kXeEaInstances = 32;      // GUC_MAX_INSTANCES_PER_CLASS
static constexpr uint32_t kXeEaTotalQuanta = 0x8000;   // TOTAL_QUANTA
static constexpr uint32_t kXeEaActionSetDeviceBuffer = 0x550C;   // XE_GUC_ACTION_SET_DEVICE_ENGINE_ACTIVITY_BUFFER
static constexpr uint32_t kXeEaRequiredSubmitVer = 1u << 16 | 14u << 8 | 1u;   // 1.14.1, as sw_version (major 23:16)
static constexpr uint32_t kXeEaMetaOff = 0, kXeEaDataOff = 0x1000;   // in the kext's one 12 KB buffer per GuC
static constexpr uint32_t kXeEaBytes = 0x3000;

// xe_guc_fwif.h struct guc_engine_activity (packed)
struct XeEaRecord {
	uint16_t changeNum;
	uint16_t quantaRatio;
	uint32_t lastUpdateTick;
	uint64_t activeTicks;
} __attribute__((packed));
static_assert(sizeof(XeEaRecord) == 16, "guc_engine_activity");
// struct guc_engine_activity_metadata
struct XeEaMeta {
	uint32_t gucTscHz;
	uint32_t lagLatencyUs;
	uint32_t globalChangeNum;
	uint32_t reserved;
} __attribute__((packed));
static_assert(sizeof(XeEaMeta) == 16, "guc_engine_activity_metadata");
static_assert(kXeEaDataOff + kXeEaClasses * kXeEaInstances * sizeof(XeEaRecord) <= kXeEaBytes, "activity data fits");

// The record's offset in the activity data: engine_activity[guc_class][guc_logical_instance].
static inline uint32_t xeEaRecordOffset(uint32_t gucClass, uint32_t logical) {
	return (gucClass * kXeEaInstances + logical) * (uint32_t)sizeof(XeEaRecord);
}

// The engine activity statistics exist from GuC submission interface 1.14.1 (is_engine_activity_supported).
static inline bool xeEaSupported(uint32_t submissionVersion) {
	return (submissionVersion & 0xffffff) >= kXeEaRequiredSubmitVer;
}

// 3 - RPM_CONFIG0's CTC shift parameter (bits 2:1): how far MISC_STATUS_0 is shifted to get GPM ticks.
static inline uint32_t xeEaGpmShift(uint32_t rpmConfig0) { return 3 - ((rpmConfig0 >> 1) & 3); }

// MISC_STATUS_0 as xe_mmio_read64_2x32 reads it, shifted: the low 32 bits are what is compared with last_update_tick.
// 0 or all ones (no answer) is not a timestamp.
static inline bool xeEaGpmLow(uint32_t lo, uint32_t hi, uint32_t shift, uint32_t *out) {
	if ((lo == 0 && hi == 0) || (lo == 0xffffffffu && hi == 0xffffffffu)) return false;
	*out = (uint32_t)((((uint64_t)hi << 32) | lo) >> shift);
	return true;
}

// Linux struct engine_activity, one per engine
struct XeEaEngine {
	uint64_t active;          // the running part (ticks since the engine went active), 0 when idle
	uint64_t lastCpuNs;       // CPU time of the previous total-ticks read
	uint64_t quanta, quantaNs, quantaRemNs;
	uint64_t total;           // active ticks summed from the records
	bool running;
	uint32_t globalChange;    // cached metadata.global_change_num
	uint32_t tscHz;           // cached metadata.guc_tsc_frequency_hz (re-read while 0)
	XeEaRecord cached;        // the record as last taken
};

// engine_activity_set_cpu_ts at an enable, and the re-base described above (the totals stay).
static inline void xeEaEnable(XeEaEngine &e, uint64_t nowNs) {
	e.lastCpuNs = nowNs;
	e.running = false;
	e.active = 0;
	e.globalChange = 0;
	e.cached = XeEaRecord{};
}

// get_engine_active_ticks, first half: take the record if it changed. Returns whether the engine runs (the caller then
// reads the GPM timestamp and calls xeEaActiveRunning).
static inline bool xeEaActiveUpdate(XeEaEngine &e, uint32_t globalChange, const XeEaRecord &rec) {
	if (globalChange && globalChange != e.globalChange) {
		e.globalChange = globalChange;
		if (rec.changeNum && rec.changeNum != e.cached.changeNum) {
			e.running = rec.lastUpdateTick != 0;
			e.total += rec.activeTicks - e.cached.activeTicks;
			e.active = 0;
			e.cached.changeNum = rec.changeNum;
			e.cached.lastUpdateTick = rec.lastUpdateTick;
			e.cached.activeTicks = rec.activeTicks;
		}
	}
	return e.running;
}

// Second half: the running part from the GPM timestamp (gpmOk false: no timestamp, the part stays as it was).
static inline void xeEaActiveRunning(XeEaEngine &e, bool gpmOk, uint32_t gpmLow) {
	if (e.running && gpmOk) e.active = (uint32_t)(gpmLow - e.cached.lastUpdateTick);
}

static inline uint64_t xeEaActiveTicks(const XeEaEngine &e) { return e.total + e.active; }

// mul_u64_u32_div(a, b, c) without a 128-bit division: a = q c + r, so a b / c = q b + r b / c (r b < 2^64 for c <= 2^32).
static inline uint64_t xeEaMulDiv(uint64_t a, uint32_t b, uint32_t c) {
	return (a / c) * b + (a % c) * b / c;
}

// get_engine_total_ticks: the CPU time since the last read, scaled by the quanta ratio, in GuC ticks. quantaRatio is the
// record's (read every time; the cached one when the GuC is down), tscHz the metadata's.
static inline uint64_t xeEaTotalTicks(XeEaEngine &e, uint16_t quantaRatio, uint32_t tscHz, uint64_t nowNs) {
	if (!e.tscHz) e.tscHz = tscHz;
	e.cached.quantaRatio = quantaRatio;
	const uint64_t delta = nowNs > e.lastCpuNs ? nowNs - e.lastCpuNs : 0;
	e.lastCpuNs = nowNs;
	const uint64_t numerator = (e.quantaRemNs + delta) * e.cached.quantaRatio;
	e.quantaNs += numerator / kXeEaTotalQuanta;
	e.quantaRemNs = numerator % kXeEaTotalQuanta;
	e.quanta = xeEaMulDiv(e.quantaNs, e.tscHz, 1000000000u);
	return e.quanta;
}
