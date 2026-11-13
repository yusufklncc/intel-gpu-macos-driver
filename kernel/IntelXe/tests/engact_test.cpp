// SPDX-License-Identifier: MIT
// engact_test: offline check of the GuC engine activity arithmetic (xe_engact.h) against Linux's
// xe_guc_engine_activity.c (v7.4-rc3), no GPU needed.
//   1. the ABI: record and metadata sizes, the record offset per [GuC class][logical instance], the 12 KB buffer, the
//      supported-version rule (1.14.1; BMG's 70.72.1 image has 1.38.1), the GPM shift from RPM_CONFIG0
//   2. active ticks, record by record, against a direct transcription of get_engine_active_ticks: nothing before the
//      GuC's first update, the idle delta summed, a running engine's part from the GPM timestamp (u32 wrap), unchanged
//      global or engine change numbers ignoring the record, change_num 0 ignored
//   3. total ticks against get_engine_total_ticks: the CPU time scaled by quanta_ratio / 0x8000 with its remainder,
//      in GuC ticks (mul_u64_u32_div without 128-bit division, checked against a 128-bit reference)
//   4. the kext's two departures: a re-enable after a GuC reload re-bases the records (counters restart in the GuC, the
//      totals here only grow); a GPM timestamp of 0 or all ones leaves the running part as it was
//   5. 2000 pseudo-random sequences: active never decreases across updates without a reload when the GuC's counters
//      only grow, total never decreases, both match the reference transcription
// c++ -std=c++17 -Wall -Wextra -I.. -o engact_test engact_test.cpp && ./engact_test
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "xe_engact.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// get_engine_active_ticks / get_engine_total_ticks transcribed with Linux's names and types (u16/u32/u64), reading
// from a record and metadata instead of the iosys_map.
struct LxActivity { uint16_t change_num, quanta_ratio; uint32_t last_update_tick; uint64_t active_ticks; };
struct LxMeta { uint32_t guc_tsc_frequency_hz, lag_latency_usec, global_change_num, reserved; };
struct LxEa { uint64_t active, last_cpu_ts, quanta, quanta_ns, quanta_remainder_ns, total; bool running; LxMeta metadata;
	LxActivity activity; };
static uint64_t lxActive(LxEa *ea, const LxMeta &meta, const LxActivity &rec, uint64_t misc_status_0, uint32_t shift) {
	LxActivity *cached_activity = &ea->activity;
	LxMeta *cached_metadata = &ea->metadata;
	uint32_t last_update_tick, global_change_num;
	uint64_t active_ticks, gpm_ts;
	uint16_t change_num;
	global_change_num = meta.global_change_num;
	if (!global_change_num) goto update;
	if (global_change_num == cached_metadata->global_change_num) goto update;
	cached_metadata->global_change_num = global_change_num;
	change_num = rec.change_num;
	if (!change_num || change_num == cached_activity->change_num) goto update;
	last_update_tick = rec.last_update_tick;
	active_ticks = rec.active_ticks;
	ea->running = !!last_update_tick;
	ea->total += active_ticks - cached_activity->active_ticks;
	ea->active = 0;
	cached_activity->change_num = change_num;
	cached_activity->last_update_tick = last_update_tick;
	cached_activity->active_ticks = active_ticks;
update:
	if (ea->running) {
		gpm_ts = misc_status_0 >> shift;
		ea->active = (uint32_t)gpm_ts - cached_activity->last_update_tick;
	}
	return ea->total + ea->active;
}
static uint64_t lxTotal(LxEa *ea, const LxMeta &meta, const LxActivity &rec, uint64_t now) {
	if (!ea->metadata.guc_tsc_frequency_hz) ea->metadata.guc_tsc_frequency_hz = meta.guc_tsc_frequency_hz;
	ea->activity.quanta_ratio = rec.quanta_ratio;
	const uint64_t cpu_delta = now - ea->last_cpu_ts;
	ea->last_cpu_ts = now;
	const uint64_t numerator = (ea->quanta_remainder_ns + cpu_delta) * ea->activity.quanta_ratio;
	ea->quanta_ns += numerator / 0x8000;
	ea->quanta_remainder_ns = numerator % 0x8000;
	ea->quanta = (uint64_t)((unsigned __int128)ea->quanta_ns * ea->metadata.guc_tsc_frequency_hz / 1000000000u);
	return ea->quanta;
}

static uint64_t rng = 0x243f6a8885a308d3ull;
static uint32_t rnd() {
	rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

// The kernel's read (engActSample) for one engine: update, then the GPM part when it runs, then the total.
static uint64_t kActive(XeEaEngine &e, uint32_t gcn, const XeEaRecord &r, uint64_t misc, uint32_t shift) {
	if (xeEaActiveUpdate(e, gcn, r)) {
		uint32_t g = 0;
		const bool ok = xeEaGpmLow((uint32_t)misc, (uint32_t)(misc >> 32), shift, &g);
		xeEaActiveRunning(e, ok, g);
	}
	return xeEaActiveTicks(e);
}

int main() {
	// 1.
	CHECK(sizeof(XeEaRecord) == 16 && sizeof(XeEaMeta) == 16, "sizes");
	CHECK(offsetof(XeEaRecord, quantaRatio) == 2 && offsetof(XeEaRecord, lastUpdateTick) == 4 &&
		offsetof(XeEaRecord, activeTicks) == 8, "record offsets");
	CHECK(offsetof(XeEaMeta, globalChangeNum) == 8, "metadata offsets");
	CHECK(xeEaRecordOffset(0, 0) == 0 && xeEaRecordOffset(0, 1) == 16 && xeEaRecordOffset(1, 0) == 512 &&
		xeEaRecordOffset(4, 3) == (4 * 32 + 3) * 16, "record offset");
	CHECK(kXeEaDataOff + xeEaRecordOffset(15, 31) + 16 == kXeEaBytes, "the last record ends the buffer");
	CHECK(kXeEaActionSetDeviceBuffer == 0x550C, "action");
	CHECK(xeEaSupported(1u << 16 | 38u << 8 | 1u), "1.38.1 (BMG 70.72.1)");
	CHECK(xeEaSupported(1u << 16 | 14u << 8 | 1u) && !xeEaSupported(1u << 16 | 14u << 8 | 0u) &&
		!xeEaSupported(1u << 16 | 13u << 8 | 9u) && xeEaSupported(2u << 16), "1.14.1 boundary");
	CHECK(!xeEaSupported(0), "no version");
	CHECK(xeEaGpmShift(0) == 3 && xeEaGpmShift(1u << 1) == 2 && xeEaGpmShift(3u << 1) == 0 && xeEaGpmShift(0xfffffff9u) == 3,
		"shift");
	{
		uint32_t g = 7;
		CHECK(!xeEaGpmLow(0, 0, 0, &g) && !xeEaGpmLow(0xffffffffu, 0xffffffffu, 0, &g) && g == 7, "no timestamp");
		CHECK(xeEaGpmLow(0x80000000u, 0x5u, 1, &g) && g == 0xc0000000u, "shifted across dwords: 0x%x", g);
	}

	// 2. fixed sequence, against the transcription
	{
		XeEaEngine e = {};
		LxEa lx = {};
		xeEaEnable(e, 1000);
		lx.last_cpu_ts = 1000;
		XeEaMeta m = {};
		XeEaRecord r = {};
		auto step = [&](uint64_t misc, const char *what) {
			const LxMeta lm = {m.gucTscHz, m.lagLatencyUs, m.globalChangeNum, 0};
			const LxActivity la = {r.changeNum, r.quantaRatio, r.lastUpdateTick, r.activeTicks};
			const uint64_t want = lxActive(&lx, lm, la, misc, 3);
			const uint64_t got = kActive(e, m.globalChangeNum, r, misc, 3);
			CHECK(got == want, "%s: active %llu, Linux %llu", what, (unsigned long long)got, (unsigned long long)want);
			return got;
		};
		CHECK(step(0, "before the GuC's first update") == 0, "zero");
		m.globalChangeNum = 1;
		r = {1, 0x8000, 0, 500};
		CHECK(step(0, "idle, 500 ticks done") == 500, "500");
		m.globalChangeNum = 2;
		r = {2, 0x8000, 1000, 500};   // went active at tick 1000
		CHECK(step(1200ull << 3, "running since 1000, now 1200") == 700, "500 + 200");
		CHECK(step(1500ull << 3, "same record, now 1500") == 1000, "500 + 500");
		m.globalChangeNum = 3;   // another engine changed: this record did not
		CHECK(step(1600ull << 3, "global moved, record not") == 1100, "500 + 600");
		m.globalChangeNum = 4;
		r = {3, 0x8000, 0, 1300};   // idle again after 800 more
		CHECK(step(1700ull << 3, "idle at 1300") == 1300, "1300");
		m.globalChangeNum = 5;
		r = {0, 0x8000, 0, 9999};   // change_num 0: ignored
		CHECK(step(0, "change_num 0") == 1300, "ignored");
		m.globalChangeNum = 6;
		r = {4, 0x8000, 0xfffffff0u, 1300};   // running, its start tick about to wrap
		CHECK(step((uint64_t)0x10u << 3, "u32 wrap") == 1300 + 0x20, "wrap");
	}

	// 3. total ticks: 19.2 MHz GuC TSC, ratio 1.0 then 0.5, remainders
	{
		XeEaEngine e = {};
		LxEa lx = {};
		xeEaEnable(e, 0);
		const LxMeta lm = {19200000, 0, 1, 0};
		LxActivity la = {1, 0x8000, 0, 0};
		uint64_t t = 0;
		for (int i = 0; i < 5; i++) {
			t += 1000000000ull / 3;
			const uint64_t want = lxTotal(&lx, lm, la, t);
			const uint64_t got = xeEaTotalTicks(e, la.quanta_ratio, lm.guc_tsc_frequency_hz, t);
			CHECK(got == want, "total %llu, Linux %llu", (unsigned long long)got, (unsigned long long)want);
		}
		CHECK(e.quanta >= 31999990 && e.quanta <= 32000000, "5/3 s at 19.2 MHz: %llu", (unsigned long long)e.quanta);
		la.quanta_ratio = 0x4000;
		const uint64_t before = e.quanta;
		t += 1000000000ull;
		const uint64_t want = lxTotal(&lx, lm, la, t);
		const uint64_t got = xeEaTotalTicks(e, la.quanta_ratio, lm.guc_tsc_frequency_hz, t);
		CHECK(got == want && got - before == 9600000, "half share: +%llu", (unsigned long long)(got - before));
		XeEaEngine z = {};
		CHECK(xeEaTotalTicks(z, 0, 19200000, 5000000000ull) == 0, "ratio 0 (no record yet): 0");
		for (int i = 0; i < 1000; i++) {
			const uint64_t a = (uint64_t)rnd() << 30 | rnd();
			const uint32_t b = rnd() | 1;
			CHECK(xeEaMulDiv(a, b, 1000000000u) == (uint64_t)((unsigned __int128)a * b / 1000000000u), "muldiv %llu %u",
				(unsigned long long)a, b);
		}
	}

	// 4. re-enable after a reload: the GuC's counters restart at 0, the totals continue
	{
		XeEaEngine e = {};
		xeEaEnable(e, 0);
		kActive(e, 1, XeEaRecord{1, 0x8000, 0, 5000}, 0, 3);
		CHECK(xeEaActiveTicks(e) == 5000, "before the reload");
		xeEaTotalTicks(e, 0x8000, 1000000000u, 1000);
		const uint64_t tot = e.quanta;
		xeEaEnable(e, 2000);   // GT reset: buffer cleared, handed over again
		CHECK(xeEaActiveTicks(e) == 5000 && !e.running, "kept across the enable");
		kActive(e, 1, XeEaRecord{1, 0x8000, 0, 300}, 0, 3);   // the new GuC's first update: change numbers from 1 again
		CHECK(xeEaActiveTicks(e) == 5300, "continues: %llu", (unsigned long long)xeEaActiveTicks(e));
		xeEaTotalTicks(e, 0x8000, 1000000000u, 2500);
		CHECK(e.quanta == tot + 500, "total continues from the enable, the gap left out: %llu", (unsigned long long)e.quanta);
		// a running engine and a GPM read of 0 (GT down): the running part stays
		kActive(e, 2, XeEaRecord{2, 0x8000, 100, 300}, 150ull << 3, 3);
		CHECK(xeEaActiveTicks(e) == 5350, "running 50: %llu", (unsigned long long)xeEaActiveTicks(e));
		kActive(e, 2, XeEaRecord{2, 0x8000, 100, 300}, 0, 3);
		CHECK(xeEaActiveTicks(e) == 5350, "GPM 0: unchanged %llu", (unsigned long long)xeEaActiveTicks(e));
		kActive(e, 2, XeEaRecord{2, 0x8000, 100, 300}, ~0ull, 3);
		CHECK(xeEaActiveTicks(e) == 5350, "GPM all ones: unchanged");
	}

	// 5. random sequences: a GuC whose active_ticks grow and whose last_update_tick follows its timestamp
	for (int seq = 0; seq < 2000; seq++) {
		XeEaEngine e = {};
		LxEa lx = {};
		xeEaEnable(e, 0);
		uint32_t gcn = 0;
		uint16_t cn = 0;
		uint64_t act = 0, now = 0, ts = 1000, prevA = 0, prevT = 0;
		uint32_t start = 0;
		bool running = false;
		const uint32_t shift = rnd() % 4;
		for (int k = 0; k < 40; k++) {
			const uint32_t dt = 1 + rnd() % 100000;
			ts += dt;
			now += dt * 52;   // ~19.2 MHz
			if (running) act += dt;
			const uint32_t what = rnd() % 4;
			if (what == 0) {   // the engine changes state, the GuC updates its record
				running = !running;
				start = running ? (uint32_t)ts : 0;
				gcn++;
				if (!++cn) cn = 1;
			} else if (what == 1) {
				gcn++;   // another engine's update
			}
			const XeEaRecord r = {cn, 0x8000, start, act};
			const uint64_t misc = ts << shift;
			const LxMeta lm = {19200000, 0, gcn, 0};
			const LxActivity la = {r.changeNum, r.quantaRatio, r.lastUpdateTick, r.activeTicks};
			const uint64_t want = lxActive(&lx, lm, la, misc, shift);
			const uint64_t got = kActive(e, gcn, r, misc, shift);
			CHECK(got == want, "seq %d step %d: %llu vs %llu", seq, k, (unsigned long long)got, (unsigned long long)want);
			CHECK(got >= prevA, "seq %d step %d: active went back", seq, k);
			const uint64_t tw = lxTotal(&lx, lm, la, now), tg = xeEaTotalTicks(e, 0x8000, 19200000, now);
			CHECK(tg == tw && tg >= prevT, "seq %d step %d: total %llu vs %llu", seq, k, (unsigned long long)tg,
				(unsigned long long)tw);
			prevA = got;
			prevT = tg;
		}
	}

	printf("%s: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
	return fails ? 1 : 0;
}
