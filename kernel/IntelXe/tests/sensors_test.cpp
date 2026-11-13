// SPDX-License-Identifier: MIT
// sensors_test: offline check of debug.intelxe.sensors and sensors_raw's pure parts (xe_sensors.h), no GPU needed.
//   1. struct intelxe_sensors' layout (uapi/intelxe_sensors.h: xemac and NEO's sysman read it; offsets fixed; version
//      2 appends the engine counters after version 1's 120 bytes)
//   2. the sample from register values: the 50/3 MHz decoding, the temperature fields, an absent device
//   3. the energy meter and the power limits into the struct (microjoules, milliwatts, enable bits, I1)
//   4. the text: identical to what IntelXe::sensorsString printed before the binary form (a copy of that code below,
//      compared over fixed and pseudo-random samples and every buffer length up to the whole text)
// c++ -std=c++17 -Wall -Wextra -I.. -o sensors_test sensors_test.cpp && ./sensors_test
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "xe_sensors.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// IntelXe::sensorsString before sensors_raw (kernel/IntelXe/IntelXe.cpp at 6adbb5b), with the register reads, the
// telemetry read and the limits read as inputs. `energyKnown` = PMT platform with its lock, `limitsKnown` = mailbox
// limits with their lock.
struct Legacy {
	uint32_t wp1, req, cap, pkg, vram;
	bool energyKnown;
	int err;
	uint32_t guid;
	XeEnergyMeter energy;
	bool limitsKnown;
	XePowerLimits pl;
};
static int legacyText(char *buf, size_t len, const Legacy &in) {
	auto mhz = [](uint32_t raw) { return (raw * 50 + 1) / 3; };
	const uint32_t wp1 = in.wp1, req = in.req, cap = in.cap, pkg = in.pkg, vram = in.vram;
	if (wp1 == 0xffffffffu && cap == 0xffffffffu) return snprintf(buf, len, "unavailable (the device does not answer)\n");
	size_t n = (size_t)snprintf(buf, len, "act %u MHz, req %u MHz, RP0 %u MHz, RPn %u MHz, package %u C, VRAM %u C",
		mhz(wp1 & 0x1ff), mhz(req >> 23 & 0x1ff), mhz(cap & 0x1ff), mhz(cap >> 16 & 0x1ff), pkg & 0xff, vram & 0xff);
	if (in.energyKnown) {
		uint64_t uj[XE_ENERGY_CHANNELS] = {};
		uint32_t mw[XE_ENERGY_CHANNELS] = {}, rejected = 0;
		bool power = false, pkgPower = false;
		const int err = in.err;
		if (!err) {
			for (uint32_t c = 0; c < XE_ENERGY_CHANNELS; c++) uj[c] = in.energy.uj(c), mw[c] = in.energy.mw[c];
			power = in.energy.havePower;
			pkgPower = in.energy.pkgValid;
			rejected = in.energy.rejected;
		}
		if (err && n < len) {
			n += (size_t)snprintf(buf + n, len - n, ", energy unavailable (PMT error %d, GUID 0x%08x)", err, in.guid);
		} else if (n < len) {
			n += (size_t)snprintf(buf + n, len - n, ", card energy %llu.%03llu J, package energy %llu.%03llu J",
				(unsigned long long)(uj[XE_ENERGY_CARD] / 1000000), (unsigned long long)(uj[XE_ENERGY_CARD] % 1000000 / 1000),
				(unsigned long long)(uj[XE_ENERGY_PKG] / 1000000), (unsigned long long)(uj[XE_ENERGY_PKG] % 1000000 / 1000));
			if (n < len && power)
				n += (size_t)snprintf(buf + n, len - n, ", card power %u.%02u W", mw[XE_ENERGY_CARD] / 1000,
					mw[XE_ENERGY_CARD] % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", card power n/a");
			if (n < len && pkgPower)
				n += (size_t)snprintf(buf + n, len - n, ", package power %u.%02u W", mw[XE_ENERGY_PKG] / 1000,
					mw[XE_ENERGY_PKG] % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", package power n/a");
			if (n < len) n += (size_t)snprintf(buf + n, len - n, ", power windows rejected %u", rejected);
		}
	}
	if (in.limitsKnown && n < len) n += xePowerLimitsText(buf + n, len - n, in.pl);
	if (n < len) n += (size_t)snprintf(buf + n, len - n, "\n");
	return (int)n;
}

static XeSensorsSample sampleOf(const Legacy &in) {
	XeSensorsSample x;
	xeSensorsBegin(x, INTELXE_SENSORS_OK, 123);
	xeSensorsSetRegs(x, in.wp1, in.req, in.cap, in.pkg, in.vram);
	if (x.s.state != INTELXE_SENSORS_OK) return x;
	if (in.energyKnown) xeSensorsSetEnergy(x, in.err, in.guid, in.energy);
	if (in.limitsKnown) xeSensorsSetLimits(x, in.pl);
	return x;
}

// Every buffer length from 0 to the whole text + 2: same return value, same bytes (snprintf's cut included).
static void compareText(const Legacy &in, const char *what) {
	const XeSensorsSample x = sampleOf(in);
	char a[1024], b[1024];
	const int full = legacyText(a, sizeof a, in);
	CHECK(full > 0 && full < (int)sizeof a, "%s: legacy text length %d", what, full);
	for (size_t len = 0; len <= (size_t)full + 2; len++) {
		memset(a, 0x5a, sizeof a);
		memset(b, 0x5a, sizeof b);
		const int ra = legacyText(a, len, in), rb = xeSensorsText(b, len, x);
		if (ra != rb || memcmp(a, b, sizeof a)) {
			CHECK(false, "%s, len %zu: legacy %d \"%.*s\", new %d \"%.*s\"", what, len, ra, (int)(len ? len - 1 : 0), a,
				rb, (int)(len ? len - 1 : 0), b);
			return;
		}
	}
	checks++;
}

static uint64_t rng = 0x9e3779b97f4a7c15ull;
static uint32_t rnd() {
	rng ^= rng << 13, rng ^= rng >> 7, rng ^= rng << 17;
	return (uint32_t)(rng >> 16);
}

int main() {
	// 1. layout
	// version 1 ends at 120 (reserved1 at 116); version 2 appends the engine counters there and changes nothing before
	CHECK(offsetof(intelxe_sensors, reserved1) == 116, "reserved1 at %zu", offsetof(intelxe_sensors, reserved1));
	CHECK(offsetof(intelxe_sensors, engine_count) == 120, "engine_count at %zu", offsetof(intelxe_sensors, engine_count));
	CHECK(offsetof(intelxe_sensors, engine_tick_hz) == 128, "engine_tick_hz at %zu", offsetof(intelxe_sensors, engine_tick_hz));
	CHECK(offsetof(intelxe_sensors, engines) == 136, "engines at %zu", offsetof(intelxe_sensors, engines));
	CHECK(sizeof(intelxe_sensors_engine) == 32, "engine entry %zu", sizeof(intelxe_sensors_engine));
	CHECK(offsetof(intelxe_sensors_engine, active_ticks) == 16 && offsetof(intelxe_sensors_engine, total_ticks) == 24,
		"engine entry offsets");
	CHECK(sizeof(intelxe_sensors) == 136 + 32 * INTELXE_SENSORS_MAX_ENGINES, "sizeof %zu", sizeof(intelxe_sensors));
	CHECK(INTELXE_SENSORS_VERSION == 2, "version");
	CHECK(offsetof(intelxe_sensors, timestamp_ns) == 16, "timestamp_ns at %zu", offsetof(intelxe_sensors, timestamp_ns));
	CHECK(offsetof(intelxe_sensors, act_mhz) == 24, "act_mhz at %zu", offsetof(intelxe_sensors, act_mhz));
	CHECK(offsetof(intelxe_sensors, rpe_mhz) == 40, "rpe_mhz at %zu", offsetof(intelxe_sensors, rpe_mhz));
	CHECK(offsetof(intelxe_sensors, pkg_temp_c) == 52, "pkg_temp_c at %zu", offsetof(intelxe_sensors, pkg_temp_c));
	CHECK(offsetof(intelxe_sensors, energy_uj) == 64, "energy_uj at %zu", offsetof(intelxe_sensors, energy_uj));
	CHECK(offsetof(intelxe_sensors, power_mw) == 80, "power_mw at %zu", offsetof(intelxe_sensors, power_mw));
	CHECK(offsetof(intelxe_sensors, limit_mw) == 88, "limit_mw at %zu", offsetof(intelxe_sensors, limit_mw));
	CHECK(offsetof(intelxe_sensors, limit_on) == 104, "limit_on at %zu", offsetof(intelxe_sensors, limit_on));
	CHECK(offsetof(intelxe_sensors, crit_milli) == 108, "crit_milli at %zu", offsetof(intelxe_sensors, crit_milli));
	CHECK(INTELXE_SENSORS_CARD == XE_ENERGY_CARD && INTELXE_SENSORS_PKG == XE_ENERGY_PKG &&
		INTELXE_SENSORS_CARD == XE_PL_CARD && INTELXE_SENSORS_PKG == XE_PL_PKG, "channel indices");

	// 2. registers: CAGF 126 = 2100 MHz, request 144 = 2400 MHz, RP0 171 = 2850 MHz, RPn 24 = 400 MHz, 47 C, 52 C (the
	// bits above the temperature field ignored)
	Legacy b580 = {};
	b580.wp1 = 0x0000007e;
	b580.req = 144u << 23 | 0x1234;
	b580.cap = 24u << 16 | 171u;
	b580.pkg = 0xab00 | 47;
	b580.vram = 0xcd00 | 52;
	XeSensorsSample x = sampleOf(b580);
	CHECK(x.s.version == INTELXE_SENSORS_VERSION && x.s.size == sizeof(intelxe_sensors) && x.s.timestamp_ns == 123,
		"header %u %u %llu", x.s.version, x.s.size, (unsigned long long)x.s.timestamp_ns);
	CHECK(x.s.state == INTELXE_SENSORS_OK && x.s.valid == (INTELXE_SENSORS_FREQ | INTELXE_SENSORS_TEMP), "valid 0x%x",
		x.s.valid);
	CHECK(x.s.act_mhz == 2100 && x.s.req_mhz == 2400 && x.s.rp0_mhz == 2850 && x.s.rpn_mhz == 400, "MHz %u %u %u %u",
		x.s.act_mhz, x.s.req_mhz, x.s.rp0_mhz, x.s.rpn_mhz);
	CHECK(x.s.pkg_temp_c == 47 && x.s.vram_temp_c == 52, "temperatures %d %d", x.s.pkg_temp_c, x.s.vram_temp_c);
	char t[1024];
	xeSensorsText(t, sizeof t, x);
	CHECK(!strcmp(t, "act 2100 MHz, req 2400 MHz, RP0 2850 MHz, RPn 400 MHz, package 47 C, VRAM 52 C\n"), "text %s", t);
	compareText(b580, "registers only");
	xeSensorsSetRange(x, 0, 0, 0);
	CHECK(!(x.s.valid & INTELXE_SENSORS_RANGE), "no range before SLPC");
	xeSensorsSetRange(x, 1800, 1200, 2850);
	CHECK((x.s.valid & INTELXE_SENSORS_RANGE) && x.s.rpe_mhz == 1800 && x.s.min_mhz == 1200 && x.s.max_mhz == 2850,
		"range %u %u %u", x.s.rpe_mhz, x.s.min_mhz, x.s.max_mhz);
	xeSensorsText(t, sizeof t, x);
	CHECK(!strcmp(t, "act 2100 MHz, req 2400 MHz, RP0 2850 MHz, RPn 400 MHz, package 47 C, VRAM 52 C\n"),
		"the range is not in the text: %s", t);

	Legacy gone = b580;
	gone.wp1 = gone.cap = 0xffffffffu;
	x = sampleOf(gone);
	CHECK(x.s.state == INTELXE_SENSORS_NO_ANSWER && x.s.valid == 0 && x.s.act_mhz == 0, "absent device: state %u valid 0x%x",
		x.s.state, x.s.valid);
	compareText(gone, "absent device");
	XeSensorsSample asleep;
	xeSensorsBegin(asleep, INTELXE_SENSORS_ASLEEP, 5);
	xeSensorsText(t, sizeof t, asleep);
	CHECK(!strcmp(t, "unavailable (asleep)\n") && asleep.s.valid == 0, "asleep: %s", t);
	xeSensorsBegin(asleep, INTELXE_SENSORS_NOT_MAPPED, 5);
	xeSensorsText(t, sizeof t, asleep);
	CHECK(!strcmp(t, "unavailable (not mapped)\n"), "not mapped: %s", t);

	// 3. energy: two telemetry words 1 s apart (card in 63:32, package in 31:0, units of 2^-14 J): card +36 J, package
	// +20 J
	Legacy e = b580;
	e.energyKnown = true;
	e.energy.update((uint64_t)1000000u << 32 | 500000u, 1000000000ull, 500000000ull);
	e.energy.update((uint64_t)(1000000u + 36u * 16384u) << 32 | (500000u + 20u * 16384u), 2000000000ull, 500000000ull);
	x = sampleOf(e);
	const uint64_t card = xeEnergyMicrojoules(1000000ull + 36ull * 16384, 14), pkg = xeEnergyMicrojoules(500000ull + 20ull * 16384, 14);
	CHECK(x.s.energy_uj[INTELXE_SENSORS_CARD] == card && x.s.energy_uj[INTELXE_SENSORS_PKG] == pkg, "energy %llu %llu",
		(unsigned long long)x.s.energy_uj[0], (unsigned long long)x.s.energy_uj[1]);
	CHECK((x.s.valid & (INTELXE_SENSORS_ENERGY | INTELXE_SENSORS_POWER_CARD | INTELXE_SENSORS_POWER_PKG)) ==
		(INTELXE_SENSORS_ENERGY | INTELXE_SENSORS_POWER_CARD | INTELXE_SENSORS_POWER_PKG), "valid 0x%x", x.s.valid);
	CHECK(x.s.power_mw[INTELXE_SENSORS_CARD] == 36000 && x.s.power_mw[INTELXE_SENSORS_PKG] == 20000, "power %u %u",
		x.s.power_mw[0], x.s.power_mw[1]);
	compareText(e, "energy and power");
	Legacy first = b580;   // one reading: energy, no power window yet
	first.energyKnown = true;
	first.energy.update((uint64_t)7u << 32 | 9u, 1, 500000000ull);
	x = sampleOf(first);
	CHECK((x.s.valid & INTELXE_SENSORS_ENERGY) && !(x.s.valid & (INTELXE_SENSORS_POWER_CARD | INTELXE_SENSORS_POWER_PKG)),
		"first reading valid 0x%x", x.s.valid);
	compareText(first, "energy, no window");
	Legacy refused = b580;   // package above the card: the package window is refused, the card's kept
	refused.energyKnown = true;
	refused.energy.update(0, 1000000000ull, 500000000ull);
	refused.energy.update((uint64_t)(37u * 16384u) << 32 | (152u * 16384u), 2000000000ull, 500000000ull);
	x = sampleOf(refused);
	CHECK((x.s.valid & INTELXE_SENSORS_POWER_CARD) && !(x.s.valid & INTELXE_SENSORS_POWER_PKG) &&
		x.s.power_mw[INTELXE_SENSORS_PKG] == 0 && x.rejected == 1, "refused window: valid 0x%x pkg %u rejected %u",
		x.s.valid, x.s.power_mw[1], x.rejected);
	compareText(refused, "refused package window");
	Legacy pmtErr = b580;
	pmtErr.energyKnown = true;
	pmtErr.err = 61;   // ENODATA
	pmtErr.guid = 0x1e2f8200;
	x = sampleOf(pmtErr);
	CHECK(!(x.s.valid & INTELXE_SENSORS_ENERGY) && x.s.energy_uj[0] == 0, "PMT error: valid 0x%x", x.s.valid);
	compareText(pmtErr, "PMT error");

	// power limits: card PL1 190 W on, PL2 250 W off, package PL1 120 W on, PL2 0; I1 = 300 W
	Legacy l = e;
	l.limitsKnown = true;
	l.pl.err = 0;
	l.pl.word[XE_PL_CARD][0] = PWR_LIM_EN | 190 * 8;
	l.pl.word[XE_PL_CARD][1] = 250 * 8;
	l.pl.word[XE_PL_PKG][0] = PWR_LIM_EN | 120 * 8 + 1;
	l.pl.word[XE_PL_PKG][1] = 0;
	l.pl.i1Err = 0;
	l.pl.i1 = POWER_SETUP_I1_WATTS | 300u << POWER_SETUP_I1_SHIFT;
	x = sampleOf(l);
	CHECK(x.s.limit_mw[0][0] == 190000 && x.s.limit_mw[0][1] == 250000 && x.s.limit_mw[1][0] == 120125 &&
		x.s.limit_mw[1][1] == 0, "limits %u %u %u %u", x.s.limit_mw[0][0], x.s.limit_mw[0][1], x.s.limit_mw[1][0],
		x.s.limit_mw[1][1]);
	CHECK(x.s.limit_on == (1u << 0 | 1u << 2), "limit_on 0x%x", x.s.limit_on);
	CHECK((x.s.valid & INTELXE_SENSORS_CRIT) && x.s.crit_milli == 300000 && x.s.crit_is_watts == 1, "crit %u %u",
		x.s.crit_milli, x.s.crit_is_watts);
	compareText(l, "limits");
	Legacy amps = l;
	amps.pl.i1 = 50u << POWER_SETUP_I1_SHIFT;
	x = sampleOf(amps);
	CHECK(x.s.crit_milli == 50000 && x.s.crit_is_watts == 0, "crit in amperes %u %u", x.s.crit_milli, x.s.crit_is_watts);
	compareText(amps, "crit in amperes");
	Legacy noI1 = l;
	noI1.pl.i1Err = 5;
	x = sampleOf(noI1);
	CHECK((x.s.valid & INTELXE_SENSORS_LIMITS) && !(x.s.valid & INTELXE_SENSORS_CRIT) && x.s.crit_milli == 0, "no I1 0x%x",
		x.s.valid);
	compareText(noI1, "no I1");
	Legacy plErr = l;
	plErr.pl.err = 61;
	x = sampleOf(plErr);
	CHECK(!(x.s.valid & (INTELXE_SENSORS_LIMITS | INTELXE_SENSORS_CRIT)) && x.s.limit_mw[0][0] == 0, "limits error 0x%x",
		x.s.valid);
	compareText(plErr, "limits error");

	// 4. pseudo-random samples (any register values, meters after random readings, both lock/platform choices)
	for (int i = 0; i < 300; i++) {
		Legacy r = {};
		r.wp1 = rnd();
		r.req = rnd();
		r.cap = (i % 37 == 0) ? 0xffffffffu : rnd();
		if (i % 37 == 0) r.wp1 = 0xffffffffu;
		r.pkg = rnd();
		r.vram = rnd();
		r.energyKnown = rnd() & 1;
		r.err = (rnd() % 4 == 0) ? (int)(rnd() % 100) : 0;
		r.guid = rnd();
		uint64_t ns = 0;
		for (uint32_t k = rnd() % 4; k; k--) {
			ns += 1 + rnd() % 2000000000u;
			r.energy.update((uint64_t)rnd() << 32 | rnd(), ns, 500000000ull);
		}
		r.limitsKnown = rnd() & 1;
		r.pl.err = (rnd() % 4 == 0) ? 61 : 0;
		for (uint32_t c = 0; c < XE_PL_CHANNELS; c++)
			for (uint32_t k = 0; k < 2; k++) r.pl.word[c][k] = rnd();
		r.pl.i1Err = (rnd() % 3 == 0) ? 5 : 0;
		r.pl.i1 = rnd();
		char what[32];
		snprintf(what, sizeof what, "random %d", i);
		compareText(r, what);
	}

	printf("%s: %d of %d checks failed\n", fails ? "FAIL" : "PASS", fails, checks);
	return fails ? 1 : 0;
}
