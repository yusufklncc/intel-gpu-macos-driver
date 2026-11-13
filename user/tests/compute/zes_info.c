// SPDX-License-Identifier: MIT
// zes_info: Level Zero sysman (zes*) through the loader, the way oneAPI tools (xpu-smi, the SYCL runtime's free-memory
// and clock queries) see the device: zesInit, then every module the driver offers, each printed with its values.
// Lines: "PASS name: ..." (the call answered), "NONE name" (the driver offers no such handle: count 0, or
// ZE_RESULT_ERROR_UNSUPPORTED_FEATURE), "FAIL name: 0x.." (any other error). Read only, except "set-freq MIN MAX".
//   zes_info                  every module once
//   zes_info watch [N]        then N (default 5) one-second samples of frequency, power, temperature, engine activity
//   zes_info set-freq MIN MAX zesFrequencySetRange on the GPU domain, read back, then the old range restored
//   zes_info expect-fake      the default run, then the values xemac's canned Battlemage gives checked (XEMAC_FAKE=1)
// Exit status 0 when no line is FAIL (and, with expect-fake, every canned value matched).
//
// Build (the build Mac; L0 = the prefix compute/build-neo.sh installs to):
//   clang -arch x86_64 -O1 -I$L0/include -o zes_info zes_info.c -L$L0/lib -lze_loader -Wl,-rpath,@executable_path/../lib
// Run: DYLD_LIBRARY_PATH=$PWD/lib ./bin/zes_info   (XEMAC_FAKE=1: no kext, the canned values)
#include <level_zero/zes_api.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures, mismatches;

static const char *res(ze_result_t r)
{
	static char buf[32];
	switch (r) {
	case ZE_RESULT_SUCCESS: return "SUCCESS";
	case ZE_RESULT_ERROR_UNSUPPORTED_FEATURE: return "UNSUPPORTED_FEATURE";
	case ZE_RESULT_ERROR_NOT_AVAILABLE: return "NOT_AVAILABLE";
	case ZE_RESULT_ERROR_INSUFFICIENT_PERMISSIONS: return "INSUFFICIENT_PERMISSIONS";
	case ZE_RESULT_ERROR_UNINITIALIZED: return "UNINITIALIZED";
	case ZE_RESULT_ERROR_UNKNOWN: return "UNKNOWN";
	case ZE_RESULT_ERROR_DEVICE_LOST: return "DEVICE_LOST";
	case ZE_RESULT_ERROR_INVALID_ARGUMENT: return "INVALID_ARGUMENT";
	default: snprintf(buf, sizeof buf, "0x%x", (unsigned)r); return buf;
	}
}

// A call that must answer: PASS or FAIL. Returns 1 when it answered.
static int ok(const char *name, ze_result_t r)
{
	if (r == ZE_RESULT_SUCCESS)
		return 1;
	printf("FAIL %s: %s\n", name, res(r));
	failures++;
	return 0;
}

// A call the driver may not offer: UNSUPPORTED_FEATURE is "NONE", not a failure.
static int opt(const char *name, ze_result_t r)
{
	if (r == ZE_RESULT_SUCCESS)
		return 1;
	if (r == ZE_RESULT_ERROR_UNSUPPORTED_FEATURE) {
		printf("NONE %s\n", name);
		return 0;
	}
	return ok(name, r);
}

static void expect(int cond, const char *what)
{
	if (!cond) {
		printf("MISMATCH %s\n", what);
		mismatches++;
	}
}

static const char *freq_domain(zes_freq_domain_t d)
{
	switch (d) {
	case ZES_FREQ_DOMAIN_GPU: return "gpu";
	case ZES_FREQ_DOMAIN_MEMORY: return "memory";
	case ZES_FREQ_DOMAIN_MEDIA: return "media";
	default: return "other";
	}
}

static const char *temp_sensor(zes_temp_sensors_t t)
{
	switch (t) {
	case ZES_TEMP_SENSORS_GLOBAL: return "global";
	case ZES_TEMP_SENSORS_GPU: return "gpu";
	case ZES_TEMP_SENSORS_MEMORY: return "memory";
	default: return "other";
	}
}

static const char *engine_group(zes_engine_group_t g)
{
	switch (g) {
	case ZES_ENGINE_GROUP_ALL: return "all";
	case ZES_ENGINE_GROUP_COMPUTE_ALL: return "compute-all";
	case ZES_ENGINE_GROUP_MEDIA_ALL: return "media-all";
	case ZES_ENGINE_GROUP_COPY_ALL: return "copy-all";
	case ZES_ENGINE_GROUP_COMPUTE_SINGLE: return "compute";
	case ZES_ENGINE_GROUP_RENDER_SINGLE: return "render";
	case ZES_ENGINE_GROUP_MEDIA_DECODE_SINGLE: return "decode";
	case ZES_ENGINE_GROUP_MEDIA_ENCODE_SINGLE: return "encode";
	case ZES_ENGINE_GROUP_COPY_SINGLE: return "copy";
	case ZES_ENGINE_GROUP_MEDIA_ENHANCEMENT_SINGLE: return "enhance";
	case ZES_ENGINE_GROUP_3D_SINGLE: return "3d";
	case ZES_ENGINE_GROUP_RENDER_ALL: return "render-all";
	default: return "other";
	}
}

#define MAXH 32

static void device_info(zes_device_handle_t d, int fake)
{
	zes_device_ext_properties_t ext = {.stype = ZES_STRUCTURE_TYPE_DEVICE_EXT_PROPERTIES};
	zes_device_properties_t p = {.stype = ZES_STRUCTURE_TYPE_DEVICE_PROPERTIES, .pNext = &ext};
	if (ok("zesDeviceGetProperties", zesDeviceGetProperties(d, &p))) {
		printf("PASS device: %s, 0x%04x, %u sub-devices; brand \"%s\", model \"%s\", vendor \"%s\", driver \"%s\", "
		       "serial \"%s\", board \"%s\"; type %d, flags 0x%x\n",
		       p.core.name, p.core.deviceId, p.numSubdevices, p.brandName, p.modelName, p.vendorName, p.driverVersion,
		       p.serialNumber, p.boardNumber, ext.type, ext.flags);
		if (fake) {
			expect(p.core.deviceId == 0xe20b, "device id 0xe20b");
			expect(strstr(p.modelName, "B580") != NULL, "model name names the B580");
		}
	}
	zes_device_state_t st = {.stype = ZES_STRUCTURE_TYPE_DEVICE_STATE};
	if (opt("zesDeviceGetState", zesDeviceGetState(d, &st)))
		printf("PASS state: reset 0x%x, repaired %d\n", st.reset, st.repaired);
	uint32_t np = 0;
	const ze_result_t pr = zesDeviceProcessesGetState(d, &np, NULL);
	if (pr == ZE_RESULT_ERROR_NOT_AVAILABLE) // Linux reads /proc/<pid>/fd and fdinfo; macOS has no procfs
		printf("NONE zesDeviceProcessesGetState (not available)\n");
	else if (opt("zesDeviceProcessesGetState", pr))
		printf("PASS processes: %u\n", np);
}

static void pci_info(zes_device_handle_t d, int fake)
{
	zes_pci_properties_t p = {.stype = ZES_STRUCTURE_TYPE_PCI_PROPERTIES};
	if (ok("zesDevicePciGetProperties", zesDevicePciGetProperties(d, &p))) {
		printf("PASS pci: %04x:%02x:%02x.%x, max gen %d x%d (%lld B/s); counters bandwidth %d packets %d replay %d\n",
		       p.address.domain, p.address.bus, p.address.device, p.address.function, p.maxSpeed.gen, p.maxSpeed.width,
		       (long long)p.maxSpeed.maxBandwidth, p.haveBandwidthCounters, p.havePacketCounters, p.haveReplayCounters);
		if (fake)
			expect(p.address.bus == 3, "PCI bus 3 (the canned device)");
	}
	zes_pci_state_t s = {.stype = ZES_STRUCTURE_TYPE_PCI_STATE};
	if (opt("zesDevicePciGetState", zesDevicePciGetState(d, &s)))
		printf("PASS pci state: status %d, gen %d x%d\n", s.status, s.speed.gen, s.speed.width);
	zes_pci_stats_t ps = {0};
	if (opt("zesDevicePciGetStats", zesDevicePciGetStats(d, &ps)))
		printf("PASS pci stats: rx %llu, tx %llu, packets %llu, replays %llu\n", (unsigned long long)ps.rxCounter,
		       (unsigned long long)ps.txCounter, (unsigned long long)ps.packetCounter,
		       (unsigned long long)ps.replayCounter);
	uint32_t nb = 0;
	if (opt("zesDevicePciGetBars", zesDevicePciGetBars(d, &nb, NULL))) {
		zes_pci_bar_properties_t bars[8];
		if (nb > 8)
			nb = 8;
		for (uint32_t i = 0; i < nb; i++)
			bars[i] = (zes_pci_bar_properties_t){.stype = ZES_STRUCTURE_TYPE_PCI_BAR_PROPERTIES};
		if (ok("zesDevicePciGetBars", zesDevicePciGetBars(d, &nb, bars)))
			for (uint32_t i = 0; i < nb; i++)
				printf("PASS pci bar %u: type %d, base 0x%llx, size %llu MB\n", bars[i].index, bars[i].type,
				       (unsigned long long)bars[i].base, (unsigned long long)(bars[i].size >> 20));
	}
}

static uint32_t freq_handles(zes_device_handle_t d, zes_freq_handle_t *h)
{
	uint32_t n = 0;
	if (!ok("zesDeviceEnumFrequencyDomains", zesDeviceEnumFrequencyDomains(d, &n, NULL)))
		return 0;
	if (n > MAXH)
		n = MAXH;
	if (n && !ok("zesDeviceEnumFrequencyDomains", zesDeviceEnumFrequencyDomains(d, &n, h)))
		return 0;
	return n;
}

static void freq_info(zes_device_handle_t d, int fake)
{
	zes_freq_handle_t h[MAXH];
	const uint32_t n = freq_handles(d, h);
	if (!n)
		printf("NONE frequency domains\n");
	for (uint32_t i = 0; i < n; i++) {
		zes_freq_properties_t p = {.stype = ZES_STRUCTURE_TYPE_FREQ_PROPERTIES};
		if (!ok("zesFrequencyGetProperties", zesFrequencyGetProperties(h[i], &p)))
			continue;
		printf("PASS frequency %u (%s): hardware %.0f-%.0f MHz, can control %d, throttle events %d\n", i,
		       freq_domain(p.type), p.min, p.max, p.canControl, p.isThrottleEventSupported);
		zes_freq_range_t r = {0};
		if (opt("zesFrequencyGetRange", zesFrequencyGetRange(h[i], &r)))
			printf("PASS frequency %u range: %.0f-%.0f MHz\n", i, r.min, r.max);
		zes_freq_state_t s = {.stype = ZES_STRUCTURE_TYPE_FREQ_STATE};
		if (opt("zesFrequencyGetState", zesFrequencyGetState(h[i], &s)))
			printf("PASS frequency %u state: request %.0f MHz, actual %.0f MHz, efficient %.0f MHz, tdp %.0f MHz, "
			       "voltage %.3f V, throttle 0x%x\n",
			       i, s.request, s.actual, s.efficient, s.tdp, s.currentVoltage, s.throttleReasons);
		uint32_t nc = 0;
		if (opt("zesFrequencyGetAvailableClocks", zesFrequencyGetAvailableClocks(h[i], &nc, NULL)))
			printf("PASS frequency %u available clocks: %u\n", i, nc);
		zes_freq_throttle_time_t t = {0};
		if (opt("zesFrequencyGetThrottleTime", zesFrequencyGetThrottleTime(h[i], &t)))
			printf("PASS frequency %u throttle time %llu us\n", i, (unsigned long long)t.throttleTime);
		if (fake && p.type == ZES_FREQ_DOMAIN_GPU) {
			expect(p.min == 400 && p.max == 2850, "GPU hardware range 400-2850 MHz (canned RPn/RP0)");
			expect(r.min == 1200 && r.max == 2850, "GPU range 1200-2850 MHz (canned slpc_min, RP0)");
			expect(s.actual == 2100 && s.request == 2400, "GPU actual 2100 MHz, request 2400 MHz (canned)");
		}
	}
}

static void freq_set(zes_device_handle_t d, double mn, double mx)
{
	zes_freq_handle_t h[MAXH];
	const uint32_t n = freq_handles(d, h);
	for (uint32_t i = 0; i < n; i++) {
		zes_freq_properties_t p = {.stype = ZES_STRUCTURE_TYPE_FREQ_PROPERTIES};
		if (!ok("zesFrequencyGetProperties", zesFrequencyGetProperties(h[i], &p)) || p.type != ZES_FREQ_DOMAIN_GPU)
			continue;
		zes_freq_range_t old = {0}, want = {mn, mx}, now = {0};
		if (!opt("zesFrequencyGetRange", zesFrequencyGetRange(h[i], &old)))
			return;
		if (!opt("zesFrequencySetRange", zesFrequencySetRange(h[i], &want)))
			return;
		if (ok("zesFrequencyGetRange", zesFrequencyGetRange(h[i], &now)))
			printf("PASS frequency set: asked %.0f-%.0f MHz, reads %.0f-%.0f MHz (was %.0f-%.0f)\n", mn, mx, now.min,
			       now.max, old.min, old.max);
		sleep(1);
		zes_freq_state_t s = {.stype = ZES_STRUCTURE_TYPE_FREQ_STATE};
		if (ok("zesFrequencyGetState", zesFrequencyGetState(h[i], &s)))
			printf("PASS frequency after set: request %.0f MHz, actual %.0f MHz\n", s.request, s.actual);
		ok("zesFrequencySetRange (restore)", zesFrequencySetRange(h[i], &old));
		return;
	}
}

static uint32_t power_handles(zes_device_handle_t d, zes_pwr_handle_t *h)
{
	uint32_t n = 0;
	if (!ok("zesDeviceEnumPowerDomains", zesDeviceEnumPowerDomains(d, &n, NULL)))
		return 0;
	if (n > MAXH)
		n = MAXH;
	if (n && !ok("zesDeviceEnumPowerDomains", zesDeviceEnumPowerDomains(d, &n, h)))
		return 0;
	return n;
}

static void power_info(zes_device_handle_t d, int fake)
{
	zes_pwr_handle_t h[MAXH];
	const uint32_t n = power_handles(d, h);
	if (!n)
		printf("NONE power domains\n");
	zes_power_energy_counter_t e0[MAXH] = {0};
	for (uint32_t i = 0; i < n; i++) {
		zes_power_ext_properties_t ext = {.stype = ZES_STRUCTURE_TYPE_POWER_EXT_PROPERTIES};
		zes_power_properties_t p = {.stype = ZES_STRUCTURE_TYPE_POWER_PROPERTIES, .pNext = &ext};
		if (!ok("zesPowerGetProperties", zesPowerGetProperties(h[i], &p)))
			continue;
		printf("PASS power %u (domain %d): can control %d, default limit %d mW, min %d, max %d\n", i, ext.domain,
		       p.canControl, p.defaultLimit, p.minLimit, p.maxLimit);
		if (opt("zesPowerGetEnergyCounter", zesPowerGetEnergyCounter(h[i], &e0[i])))
			printf("PASS power %u energy: %llu uJ at %llu us\n", i, (unsigned long long)e0[i].energy,
			       (unsigned long long)e0[i].timestamp);
		uint32_t nl = 0;
		if (opt("zesPowerGetLimitsExt", zesPowerGetLimitsExt(h[i], &nl, NULL)) && nl) {
			zes_power_limit_ext_desc_t l[8];
			if (nl > 8)
				nl = 8;
			for (uint32_t k = 0; k < nl; k++)
				l[k] = (zes_power_limit_ext_desc_t){.stype = ZES_STRUCTURE_TYPE_POWER_LIMIT_EXT_DESC};
			if (ok("zesPowerGetLimitsExt", zesPowerGetLimitsExt(h[i], &nl, l)))
				for (uint32_t k = 0; k < nl; k++) {
					printf("PASS power %u limit %u: level %d, source %d, %d mW, enabled %d, interval %d ms\n", i,
					       k, l[k].level, l[k].source, l[k].limit, l[k].enabled, l[k].interval);
					if (fake && ext.domain == ZES_POWER_DOMAIN_CARD && l[k].level == ZES_POWER_LEVEL_SUSTAINED)
						expect(l[k].limit == 190000, "card sustained limit 190 W (canned PL1)");
					if (fake && ext.domain == ZES_POWER_DOMAIN_PACKAGE && l[k].level == ZES_POWER_LEVEL_SUSTAINED)
						expect(l[k].limit == 150000, "package sustained limit 150 W (canned PL1)");
				}
		}
	}
	zes_pwr_handle_t card = NULL;
	if (opt("zesDeviceGetCardPowerDomain", zesDeviceGetCardPowerDomain(d, &card)))
		printf("PASS card power domain: %s\n", card ? "yes" : "null");
	if (!n)
		return;
	usleep(600000); // the kext averages power over windows of at least 0.5 s; energy deltas need as long
	for (uint32_t i = 0; i < n; i++) {
		zes_power_energy_counter_t e1 = {0};
		if (e0[i].timestamp && zesPowerGetEnergyCounter(h[i], &e1) == ZE_RESULT_SUCCESS &&
		    e1.timestamp > e0[i].timestamp)
		{
			const double w = (double)(e1.energy - e0[i].energy) / (double)(e1.timestamp - e0[i].timestamp);
			printf("PASS power %u: %.2f W over %.2f s\n", i, w, (double)(e1.timestamp - e0[i].timestamp) / 1e6);
			if (fake)
				expect(w > 0 && w < 100, "energy grows by a canned power (card 40 W, package 25 W)");
		}
	}
}

static void temp_info(zes_device_handle_t d, int fake)
{
	zes_temp_handle_t h[MAXH];
	uint32_t n = 0;
	if (!ok("zesDeviceEnumTemperatureSensors", zesDeviceEnumTemperatureSensors(d, &n, NULL)))
		return;
	if (n > MAXH)
		n = MAXH;
	if (n && !ok("zesDeviceEnumTemperatureSensors", zesDeviceEnumTemperatureSensors(d, &n, h)))
		return;
	if (!n)
		printf("NONE temperature sensors\n");
	for (uint32_t i = 0; i < n; i++) {
		zes_temp_properties_t p = {.stype = ZES_STRUCTURE_TYPE_TEMP_PROPERTIES};
		if (!ok("zesTemperatureGetProperties", zesTemperatureGetProperties(h[i], &p)))
			continue;
		double t = 0;
		if (opt("zesTemperatureGetState", zesTemperatureGetState(h[i], &t)))
			printf("PASS temperature %u (%s): %.1f C (max %.1f C)\n", i, temp_sensor(p.type), t, p.maxTemperature);
		if (fake && p.type == ZES_TEMP_SENSORS_GPU)
			expect(t == 47, "GPU temperature 47 C (canned package)");
		if (fake && p.type == ZES_TEMP_SENSORS_MEMORY)
			expect(t == 52, "memory temperature 52 C (canned VRAM)");
	}
}

static void mem_info(zes_device_handle_t d, int fake)
{
	zes_mem_handle_t h[MAXH];
	uint32_t n = 0;
	if (!ok("zesDeviceEnumMemoryModules", zesDeviceEnumMemoryModules(d, &n, NULL)))
		return;
	if (n > MAXH)
		n = MAXH;
	if (n && !ok("zesDeviceEnumMemoryModules", zesDeviceEnumMemoryModules(d, &n, h)))
		return;
	if (!n) {
		printf("FAIL memory modules: none\n");
		failures++;
	}
	for (uint32_t i = 0; i < n; i++) {
		zes_mem_properties_t p = {.stype = ZES_STRUCTURE_TYPE_MEM_PROPERTIES};
		if (ok("zesMemoryGetProperties", zesMemoryGetProperties(h[i], &p)))
			printf("PASS memory %u: type %d, location %d, physical %llu MB, bus width %d, channels %d\n", i, p.type,
			       p.location, (unsigned long long)(p.physicalSize >> 20), p.busWidth, p.numChannels);
		zes_mem_state_t s = {.stype = ZES_STRUCTURE_TYPE_MEM_STATE};
		if (ok("zesMemoryGetState", zesMemoryGetState(h[i], &s))) {
			printf("PASS memory %u state: health %d, free %llu MB of %llu MB\n", i, s.health,
			       (unsigned long long)(s.free >> 20), (unsigned long long)(s.size >> 20));
			if (fake)
				expect(s.size == 12ull << 30 && s.free > 0 && s.free <= s.size, "memory 12 GB, free within it");
		}
		zes_mem_bandwidth_t b = {0};
		if (opt("zesMemoryGetBandwidth", zesMemoryGetBandwidth(h[i], &b)))
			printf("PASS memory %u bandwidth: read %llu, write %llu, max %llu B/s\n", i,
			       (unsigned long long)b.readCounter, (unsigned long long)b.writeCounter,
			       (unsigned long long)b.maxBandwidth);
	}
}

static void engine_info(zes_device_handle_t d)
{
	zes_engine_handle_t h[MAXH];
	uint32_t n = 0;
	if (!ok("zesDeviceEnumEngineGroups", zesDeviceEnumEngineGroups(d, &n, NULL)))
		return;
	if (n > MAXH)
		n = MAXH;
	if (n && !ok("zesDeviceEnumEngineGroups", zesDeviceEnumEngineGroups(d, &n, h)))
		return;
	if (!n)
		printf("NONE engine groups\n");
	for (uint32_t i = 0; i < n; i++) {
		zes_engine_properties_t p = {.stype = ZES_STRUCTURE_TYPE_ENGINE_PROPERTIES};
		if (!ok("zesEngineGetProperties", zesEngineGetProperties(h[i], &p)))
			continue;
		zes_engine_stats_t s = {0};
		if (opt("zesEngineGetActivity", zesEngineGetActivity(h[i], &s)))
			printf("PASS engine %u (%s): active %llu us at %llu us\n", i, engine_group(p.type),
			       (unsigned long long)s.activeTime, (unsigned long long)s.timestamp);
		else
			printf("PASS engine %u (%s)\n", i, engine_group(p.type));
	}
}

// The modules this backend does not offer: each enumeration must answer (count 0, or UNSUPPORTED_FEATURE).
static void other_modules(zes_device_handle_t d)
{
#define COUNT(fn)                                                                                                      \
	do {                                                                                                           \
		uint32_t n_ = 0;                                                                                       \
		if (opt(#fn, fn(d, &n_, NULL)))                                                                        \
			printf("%s %s: %u\n", n_ ? "PASS" : "NONE", #fn, n_);                                      \
	} while (0)
	COUNT(zesDeviceEnumFans);
	COUNT(zesDeviceEnumStandbyDomains);
	COUNT(zesDeviceEnumRasErrorSets);
	COUNT(zesDeviceEnumFirmwares);
	COUNT(zesDeviceEnumFabricPorts);
	COUNT(zesDeviceEnumDiagnosticTestSuites);
	COUNT(zesDeviceEnumPerformanceFactorDomains);
	COUNT(zesDeviceEnumSchedulers);
	COUNT(zesDeviceEnumPsus);
	COUNT(zesDeviceEnumLeds);
#undef COUNT
	zes_device_ecc_properties_t ecc = {.stype = ZES_STRUCTURE_TYPE_DEVICE_ECC_PROPERTIES};
	ze_bool_t avail = 0;
	if (opt("zesDeviceEccAvailable", zesDeviceEccAvailable(d, &avail)))
		printf("PASS ecc available: %d\n", avail);
	if (avail && opt("zesDeviceGetEccState", zesDeviceGetEccState(d, &ecc)))
		printf("PASS ecc state: %d\n", ecc.currentState);
}

static void watch(zes_device_handle_t d, int samples)
{
	zes_freq_handle_t fh[MAXH];
	zes_pwr_handle_t ph[MAXH];
	zes_temp_handle_t th[MAXH];
	zes_engine_handle_t eh[MAXH];
	const uint32_t nf = freq_handles(d, fh), np = power_handles(d, ph);
	uint32_t nt = MAXH, ne = MAXH;
	if (zesDeviceEnumTemperatureSensors(d, &nt, th) != ZE_RESULT_SUCCESS)
		nt = 0;
	if (zesDeviceEnumEngineGroups(d, &ne, eh) != ZE_RESULT_SUCCESS)
		ne = 0;
	zes_power_energy_counter_t e0[MAXH] = {0};
	zes_engine_stats_t a0[MAXH] = {0};
	for (uint32_t i = 0; i < np; i++)
		zesPowerGetEnergyCounter(ph[i], &e0[i]);
	for (uint32_t i = 0; i < ne; i++)
		zesEngineGetActivity(eh[i], &a0[i]);
	for (int k = 0; k < samples; k++) {
		sleep(1);
		char line[512];
		int o = snprintf(line, sizeof line, "sample %d:", k + 1);
		for (uint32_t i = 0; i < nf; i++) {
			zes_freq_state_t s = {.stype = ZES_STRUCTURE_TYPE_FREQ_STATE};
			if (zesFrequencyGetState(fh[i], &s) == ZE_RESULT_SUCCESS && o < (int)sizeof line)
				o += snprintf(line + o, sizeof line - o, " f%u %.0f/%.0f MHz", i, s.actual, s.request);
		}
		for (uint32_t i = 0; i < np; i++) {
			zes_power_energy_counter_t e1 = {0};
			if (zesPowerGetEnergyCounter(ph[i], &e1) == ZE_RESULT_SUCCESS && e1.timestamp > e0[i].timestamp &&
			    o < (int)sizeof line)
				o += snprintf(line + o, sizeof line - o, " p%u %.2f W", i,
					      (double)(e1.energy - e0[i].energy) / (double)(e1.timestamp - e0[i].timestamp));
			e0[i] = e1;
		}
		for (uint32_t i = 0; i < nt; i++) {
			double t = 0;
			if (zesTemperatureGetState(th[i], &t) == ZE_RESULT_SUCCESS && o < (int)sizeof line)
				o += snprintf(line + o, sizeof line - o, " t%u %.0f C", i, t);
		}
		for (uint32_t i = 0; i < ne; i++) {
			zes_engine_stats_t a1 = {0};
			if (zesEngineGetActivity(eh[i], &a1) == ZE_RESULT_SUCCESS && a1.timestamp > a0[i].timestamp &&
			    o < (int)sizeof line)
				o += snprintf(line + o, sizeof line - o, " e%u %.0f%%", i,
					      100.0 * (double)(a1.activeTime - a0[i].activeTime) /
						      (double)(a1.timestamp - a0[i].timestamp));
			a0[i] = a1;
		}
		printf("%s\n", line);
	}
}

int main(int argc, char **argv)
{
	const char *mode = argc > 1 ? argv[1] : "";
	const int fake = strcmp(mode, "expect-fake") == 0;
	if (!ok("zesInit", zesInit(0)))
		return 1;
	uint32_t nd = 0;
	if (!ok("zesDriverGet", zesDriverGet(&nd, NULL)) || !nd) {
		printf("FAIL no sysman driver\n");
		return 1;
	}
	zes_driver_handle_t drivers[4];
	if (nd > 4)
		nd = 4;
	if (!ok("zesDriverGet", zesDriverGet(&nd, drivers)))
		return 1;
	for (uint32_t di = 0; di < nd; di++) {
		uint32_t n = 0;
		if (!ok("zesDeviceGet", zesDeviceGet(drivers[di], &n, NULL)))
			continue;
		zes_device_handle_t devs[8];
		if (n > 8)
			n = 8;
		if (!ok("zesDeviceGet", zesDeviceGet(drivers[di], &n, devs)))
			continue;
		printf("PASS driver %u: %u device(s)\n", di, n);
		for (uint32_t i = 0; i < n; i++) {
			if (!strcmp(mode, "set-freq")) {
				if (argc < 4) {
					fprintf(stderr, "usage: zes_info set-freq MIN MAX\n");
					return 2;
				}
				freq_set(devs[i], atof(argv[2]), atof(argv[3]));
				continue;
			}
			device_info(devs[i], fake);
			pci_info(devs[i], fake);
			freq_info(devs[i], fake);
			power_info(devs[i], fake);
			temp_info(devs[i], fake);
			mem_info(devs[i], fake);
			engine_info(devs[i]);
			other_modules(devs[i]);
			if (!strcmp(mode, "watch"))
				watch(devs[i], argc > 2 ? atoi(argv[2]) : 5);
		}
	}
	printf("%s: %d failed, %d mismatched\n", failures || mismatches ? "FAIL" : "PASS", failures, mismatches);
	return failures || mismatches ? 1 : 0;
}
