// SPDX-License-Identifier: MIT
// debug.intelxe.sensors (text) and debug.intelxe.sensors_raw (uapi/intelxe_sensors.h, binary) from one sample. The
// pure parts live here so that tests/sensors_test.cpp builds them on the host: the sample filled from the register
// values, the energy meter and the power limits, and the text made from the sample, which is the text
// IntelXe::sensorsString printed before the binary form existed (IntelXe Manager and tools/regress parse it: the first
// "act", "req", "RP0", "RPn", "package", "VRAM", "card power" and "package power"; new fields only at the end). The
// register reads, the locks and the sysctls are in IntelXe.cpp.
#pragma once
#include <stdint.h>
#include "uapi/intelxe_sensors.h"
#include "xe_pcode.h"
#include "xe_pmt.h"

struct XeSensorsSample {
	intelxe_sensors s {};
	bool energyKnown {false};   // the platform reads energy from PMT telemetry (BMG): the text has the energy fields
	int pmtErr {0};             // the telemetry read's errno (energy unavailable), with the GUID it used
	uint32_t guid {0};
	uint32_t rejected {0};      // power windows the meter refused (xePowerWindowSane)
	bool limitsKnown {false};   // has_mbx_power_limits (BMG): the text has the power limits
	XePowerLimits pl {};
};

// GT frequency fields are in units of 50/3 MHz (xe_guc_pc.c decode_freq)
static inline uint32_t xeSensorsMhz(uint32_t raw) { return (raw * 50 + 1) / 3; }

static inline void xeSensorsBegin(XeSensorsSample &x, uint32_t state, uint64_t nowNs) {
	x.s.version = INTELXE_SENSORS_VERSION;
	x.s.size = (uint32_t)sizeof(intelxe_sensors);
	x.s.state = state;
	x.s.timestamp_ns = nowNs;
}

// MTL_MIRROR_TARGET_WP1 (CAGF in 8:0), the request register (23+8:23), MTL_RP_STATE_CAP (RP0 8:0, RPn 24:16), the BMG
// package and VRAM temperature registers (degrees in 7:0). A device that reads all ones in WP1 and the cap does not
// answer: nothing is valid.
static inline void xeSensorsSetRegs(XeSensorsSample &x, uint32_t wp1, uint32_t req, uint32_t cap, uint32_t pkg, uint32_t vram) {
	if (wp1 == 0xffffffffu && cap == 0xffffffffu) {
		x.s.state = INTELXE_SENSORS_NO_ANSWER;
		return;
	}
	x.s.act_mhz = xeSensorsMhz(wp1 & 0x1ff);
	x.s.req_mhz = xeSensorsMhz(req >> 23 & 0x1ff);
	x.s.rp0_mhz = xeSensorsMhz(cap & 0x1ff);
	x.s.rpn_mhz = xeSensorsMhz(cap >> 16 & 0x1ff);
	x.s.pkg_temp_c = (int32_t)(pkg & 0xff);
	x.s.vram_temp_c = (int32_t)(vram & 0xff);
	x.s.valid |= INTELXE_SENSORS_FREQ | INTELXE_SENSORS_TEMP;
}

// SLPC's efficient frequency and the range it runs with; 0 for max while SLPC has not started (no range then).
static inline void xeSensorsSetRange(XeSensorsSample &x, uint32_t rpe, uint32_t min, uint32_t max) {
	if (!max) return;
	x.s.rpe_mhz = rpe;
	x.s.min_mhz = min;
	x.s.max_mhz = max;
	x.s.valid |= INTELXE_SENSORS_RANGE;
}

// After the meter took the telemetry word (err 0), or the read's error.
static inline void xeSensorsSetEnergy(XeSensorsSample &x, int err, uint32_t guid, const XeEnergyMeter &m) {
	x.energyKnown = true;
	x.pmtErr = err;
	x.guid = guid;
	if (err) return;
	x.rejected = m.rejected;
	for (uint32_t c = 0; c < XE_ENERGY_CHANNELS; c++) x.s.energy_uj[c] = m.uj(c);
	x.s.valid |= INTELXE_SENSORS_ENERGY;
	if (m.havePower) {
		x.s.power_mw[XE_ENERGY_CARD] = m.mw[XE_ENERGY_CARD];
		x.s.valid |= INTELXE_SENSORS_POWER_CARD;
	}
	if (m.pkgValid) {
		x.s.power_mw[XE_ENERGY_PKG] = m.mw[XE_ENERGY_PKG];
		x.s.valid |= INTELXE_SENSORS_POWER_PKG;
	}
}

static inline void xeSensorsSetLimits(XeSensorsSample &x, const XePowerLimits &pl) {
	x.limitsKnown = true;
	x.pl = pl;
	if (pl.err) return;
	for (uint32_t c = 0; c < XE_PL_CHANNELS; c++)
		for (uint32_t l = 0; l < 2; l++) {
			x.s.limit_mw[c][l] = xePowerLimitMw(pl.word[c][l], XE_PWR_UNIT_MBX);
			if (pl.word[c][l] & PWR_LIM_EN) x.s.limit_on |= 1u << (c * 2 + l);
		}
	x.s.valid |= INTELXE_SENSORS_LIMITS;
	if (!pl.i1Err) {
		x.s.crit_milli = xePowerI1Milli(pl.i1);
		x.s.crit_is_watts = (pl.i1 & POWER_SETUP_I1_WATTS) ? 1 : 0;
		x.s.valid |= INTELXE_SENSORS_CRIT;
	}
}

// The debug.intelxe.sensors text. Returns what snprintf would (the length of the whole text, which may exceed len; the
// handler cuts it).
static inline int xeSensorsText(char *buf, size_t len, const XeSensorsSample &x) {
	const intelxe_sensors &s = x.s;
	switch (s.state) {
	case INTELXE_SENSORS_ASLEEP: return snprintf(buf, len, "unavailable (asleep)\n");
	case INTELXE_SENSORS_NOT_MAPPED: return snprintf(buf, len, "unavailable (not mapped)\n");
	case INTELXE_SENSORS_NO_ANSWER: return snprintf(buf, len, "unavailable (the device does not answer)\n");
	case INTELXE_SENSORS_NO_DEVICE: return snprintf(buf, len, "no device\n");
	default: break;
	}
	size_t n = (size_t)snprintf(buf, len, "act %u MHz, req %u MHz, RP0 %u MHz, RPn %u MHz, package %u C, VRAM %u C",
		s.act_mhz, s.req_mhz, s.rp0_mhz, s.rpn_mhz, (uint32_t)s.pkg_temp_c, (uint32_t)s.vram_temp_c);
	if (x.energyKnown) {
		if (x.pmtErr && n < len) {
			n += (size_t)snprintf(buf + n, len - n, ", energy unavailable (PMT error %d, GUID 0x%08x)", x.pmtErr, x.guid);
		} else if (n < len) {
			const uint64_t card = s.energy_uj[XE_ENERGY_CARD], pkg = s.energy_uj[XE_ENERGY_PKG];
			n += (size_t)snprintf(buf + n, len - n, ", card energy %llu.%03llu J, package energy %llu.%03llu J",
				(unsigned long long)(card / 1000000), (unsigned long long)(card % 1000000 / 1000),
				(unsigned long long)(pkg / 1000000), (unsigned long long)(pkg % 1000000 / 1000));
			const uint32_t cmw = s.power_mw[XE_ENERGY_CARD], pmw = s.power_mw[XE_ENERGY_PKG];
			if (n < len && (s.valid & INTELXE_SENSORS_POWER_CARD))
				n += (size_t)snprintf(buf + n, len - n, ", card power %u.%02u W", cmw / 1000, cmw % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", card power n/a");
			if (n < len && (s.valid & INTELXE_SENSORS_POWER_PKG))
				n += (size_t)snprintf(buf + n, len - n, ", package power %u.%02u W", pmw / 1000, pmw % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", package power n/a");
			if (n < len) n += (size_t)snprintf(buf + n, len - n, ", power windows rejected %u", x.rejected);
		}
	}
	if (x.limitsKnown && n < len) n += xePowerLimitsText(buf + n, len - n, x.pl);
	if (n < len) n += (size_t)snprintf(buf + n, len - n, "\n");
	return (int)n;
}
