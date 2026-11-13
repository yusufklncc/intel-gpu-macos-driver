// SPDX-License-Identifier: MIT
//  HybridPM: XCPM fixes and diagnostics for hybrid Alder Lake / Raptor Lake / Arrow Lake desktop CPUs (Lilu plugin).
//
//  XCPM predates hybrid CPUs. It writes IA32_HWP_REQUEST (0x774) max/desired in 100 MHz ratio units, but hybrid
//  P-cores count HWP performance in units of 78.741 MHz (Linux intel_pstate HYBRID_SCALING_FACTOR_ADL), so the
//  turbo ratio 56 of an i7-14700K becomes 56 * 78.741 = 4.41 GHz on the P-cores. The XCPM routine that builds and
//  writes the request (_xcpm_enable_pstate_ctrs+0x1140 on macOS 15.8.1, +0x1190 on 26.7.1) is routed; after Apple's
//  write the request is converted to HWP units on cores whose HWP_CAPABILITIES highest exceeds the requested max.
//  Same idea as NootedGreen XE-PM (JulioNectar/Hackintosh-XPS13Plus-IrisXe-Tahoe), which found the unit mismatch.
//  Arrow Lake (model 0xC6) counts in units below 100 MHz on both core types (P ~62.5 MHz, E ~70.8 MHz), so there the
//  conversion covers the E-cores too; on ADL/RPL the E-cores already count in 100 MHz units and are left alone.
//
//  XCPM also writes its own per-CPU pick from the frequency vector into the request's desired field, which pins the
//  core to that ratio. With CpuTopologyRebuild's SMT grouping it leaves fully busy cores at 800 MHz while others run
//  at 3.3 GHz under the same load. Autonomous mode writes desired 0, so the hardware picks each core's clock from its
//  own load within [min, highest] and EPP, as Linux intel_pstate does.
//
//  Live controls (root): sysctl debug.hybridpm.hwp=0|1, debug.hybridpm.autonomous=0|1, debug.hybridpm.epp=-1|0..255
//  (-1 keeps XCPM's EPP), debug.hybridpm.eet_off=0|1 (energy-efficient turbo, MSR_POWER_CTL bit 19). Boot-args:
//  -hpmoff (Lilu), hpmhwp=0, hpmauto=0, hpmepp=N, hpmeet=0. Defaults: conversion on, autonomous, EPP 0, EET off.
//  On an i7-14700K (perl loop, M iterations/s) these took one thread from a median of ~55 to 128 (5.39 GHz), eight
//  threads from ~400 to ~1010 and 28 threads from 920-1365 to ~1500 (PL2 253 W either way); idle package power stayed
//  ~10 W for every EPP. Everything takes effect 20 s after boot: HWP MSRs #GP before XCPM sets IA32_PM_ENABLE.
//
//  debug.hybridpm.state (read-only, any user) returns HpmState below: per-CPU APERF/MPERF, HWP capabilities and
//  request, core type and temperature, plus package RAPL energy, power limits (MSR 0x610 and the MCHBAR copy at
//  0x59A0, the lower one applies), perf-limit reasons and turbo ratio limits. hpmstat diffs two samples.
//
//  CPU guard: it acts only on Intel family 6 hybrid CPUs (CPUID.07H:EDX[15]) with HWP whose model has a known scale
//  (ADL/RPL, Arrow Lake desktop); everything else is left to XCPM untouched (no route, no MSR access) and
//  debug.hybridpm.why says why.
#include <Headers/plugin_start.hpp>
#include <Headers/kern_api.hpp>
#include <Headers/kern_patcher.hpp>
#include <IOKit/IOLib.h>
#include <IOKit/IOMemoryDescriptor.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <kern/thread_call.h>
#include <sys/sysctl.h>

static inline uint64_t rdmsr64(uint32_t m) { uint32_t lo, hi; asm volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return ((uint64_t)hi << 32) | lo; }
static inline void wrmsr64(uint32_t m, uint64_t v) { asm volatile("wrmsr" : : "c"(m), "a"((uint32_t)v), "d"((uint32_t)(v >> 32))); }
static inline void cpuid(uint32_t leaf, uint32_t sub, uint32_t r[4]) { asm volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(sub)); }
static inline uint32_t cpuidEax(uint32_t leaf) { uint32_t r[4]; cpuid(leaf, 0, r); return r[0]; }

static constexpr int MaxCpus = 64;
// kHz per HWP performance unit, per core type (set by cpuSupported()); 0 = that core type counts in 100 MHz units
// and is left as XCPM wrote it. ADL/RPL: P-cores 78741 (Linux HYBRID_SCALING_FACTOR_ADL), E-cores 100 MHz.
static uint32_t pScale = 78741, eScale = 0;

using RendezvousFn = void (*)(void (*)(void *), void *);
using CpuNumberFn = int (*)();
static RendezvousFn rendezvous;
static CpuNumberFn cpuNumber;

static int enabled, hwpOn = 1, autonomous = 1, eppOverride = 0, eetOff = 1, route, applied;
static uint8_t coreType[MaxCpus];   // CPUID 0x1A EAX[31:24]: 0x40 Core, 0x20 Atom; 0 = not seen yet
static mach_vm_address_t orgSetRatio;
static uint32_t cpuModel;
static char why[96] = "not checked";   // debug.hybridpm.why

// ─── CPU guard ───────────────────────────────────────────────────────────────────────────────────────────────────
// The ADL/RPL P-core scale (pScale 78741) is Linux intel_pstate's HYBRID_SCALING_FACTOR_ADL, which its
// hybrid_scaling_factor table applies to these family 6 models (names from arch/x86/include/asm/intel-family.h):
// ALDERLAKE 0x97, ALDERLAKE_L 0x9A, RAPTORLAKE 0xB7, RAPTORLAKE_P 0xBA, RAPTORLAKE_S 0xBF. ALDERLAKE_N 0xBE has E-cores
// only, so the hybrid bit already rules it out; it is listed as Alder Lake. ARROWLAKE 0xC6 uses the scales measured
// from Linux CPPC on a 265KF (see the case below). Other Core Ultra models use other scales (METEORLAKE_L 0xAA /
// METEORLAKE 0xAC: HYBRID_SCALING_FACTOR_MTL 80000; LUNARLAKE_M 0xBD: _LNL 86957) or scales not measured here
// (ARROWLAKE_H 0xC5, ARROWLAKE_U 0xB5, PANTHERLAKE_L 0xCC), and anything else is unknown: all of those are left alone.
// The raw CPUID instruction gives the real signature; OpenCore's Cpuid1Data only changes what the kernel records.
static bool cpuSupported() {
	uint32_t r[4];
	cpuid(0, 0, r);
	const uint32_t maxLeaf = r[0];
	if (r[1] != 0x756E6547 || r[3] != 0x49656E69 || r[2] != 0x6C65746E) {   // "GenuineIntel"
		strlcpy(why, "not an Intel CPU", sizeof(why));
		return false;
	}
	cpuid(1, 0, r);
	const uint32_t family = ((r[0] >> 8) & 0xF) + (((r[0] >> 8) & 0xF) == 0xF ? (r[0] >> 20) & 0xFF : 0);
	cpuModel = ((r[0] >> 4) & 0xF) | ((r[0] >> 12) & 0xF0);
	if (family != 6) {
		snprintf(why, sizeof(why), "not family 6 (family %u, model 0x%02X)", family, cpuModel);
		return false;
	}
	if (maxLeaf < 0x1A || (cpuid(7, 0, r), !(r[3] & (1U << 15)))) {
		snprintf(why, sizeof(why), "not hybrid (model 0x%02X)", cpuModel);
		return false;
	}
	cpuid(6, 0, r);
	if (!(r[0] & (1U << 7))) {
		snprintf(why, sizeof(why), "no HWP (model 0x%02X)", cpuModel);
		return false;
	}
	switch (cpuModel) {
	case 0x97: case 0x9A: case 0xBE: case 0xBF:
		snprintf(why, sizeof(why), "Alder Lake hybrid (model 0x%02X): waiting for XCPM", cpuModel);
		return true;
	case 0xB7: case 0xBA:
		snprintf(why, sizeof(why), "Raptor Lake hybrid (model 0x%02X): waiting for XCPM", cpuModel);
		return true;
	case 0xC6:
		// ARROWLAKE (Core Ultra 200S desktop). Both core types count in units below 100 MHz. Measured on a 265KF from
		// Linux CPPC (highest_perf against cpuinfo_max_freq): P-cores 88 = 5.5 GHz (62.5 MHz), E-cores 65 = 4.6 GHz
		// (~70.8 MHz); nominal_perf against nominal_freq gives 62.9 / 71.7. XCPM's ratio 55 would cap P-cores near
		// 3.4 GHz and E-cores near 3.9 GHz, so max -> highest is applied to both types.
		pScale = 62500;
		eScale = 70800;
		snprintf(why, sizeof(why), "Arrow Lake hybrid (model 0x%02X): waiting for XCPM", cpuModel);
		return true;
	case 0xAA: case 0xAC: case 0xB5: case 0xBD: case 0xC5: case 0xCC:
		snprintf(why, sizeof(why), "unsupported model 0x%02X (Core Ultra: another HWP scale)", cpuModel);
		return false;
	default:
		snprintf(why, sizeof(why), "unsupported model 0x%02X (HWP scale unknown)", cpuModel);
		return false;
	}
}

// ─── HWP request ─────────────────────────────────────────────────────────────────────────────────────────────────
static uint64_t setRatio(uint64_t ratio, uint64_t a1, uint64_t a2, uint64_t a3) {   // Apple uses only edi
	const uint64_t r = FunctionCast(setRatio, orgSetRatio)(ratio, a1, a2, a3);
	if (!enabled || !(rdmsr64(0x770) & 1)) return r;
	const int cpu = cpuNumber();
	if (cpu < 0 || cpu >= MaxCpus) return r;
	if (!coreType[cpu]) coreType[cpu] = (uint8_t)(cpuidEax(0x1A) >> 24);
	const uint64_t v = rdmsr64(0x774);
	if (v & (1ULL << 42)) return r;   // package-level control: not used by XCPM on these CPUs
	uint64_t nv = v;
	const uint32_t scale = coreType[cpu] == 0x40 ? pScale : coreType[cpu] == 0x20 ? eScale : 0;
	if (hwpOn && scale) {
		// max -> the core's highest (the turbo ratio table still applies), desired -> HWP units
		const uint32_t hi = (uint32_t)rdmsr64(0x771) & 0xff, max = (uint32_t)(v >> 8) & 0xff, des = (uint32_t)(v >> 16) & 0xff;
		auto toHwp = [&](uint32_t ratio) { uint32_t p = (ratio * 100000 + scale / 2) / scale; return p > hi ? hi : p; };
		if (max && hi > max)
			nv = (nv & ~0xFFFF00ULL) | ((uint64_t)hi << 8) | ((uint64_t)(des ? toHwp(des) : 0) << 16);
	}
	if (autonomous) nv &= ~0xFF0000ULL;
	if (eppOverride >= 0) nv = (nv & ~0xFF000000ULL) | ((uint64_t)(eppOverride > 255 ? 255 : eppOverride) << 24);
	if (nv != v) wrmsr64(0x774, nv);
	return r;
}

// ─── energy-efficient turbo ──────────────────────────────────────────────────────────────────────────────────────
static void eetOne(void *on) {
	const uint64_t v = rdmsr64(0x1FC), want = on ? (v | (1ULL << 19)) : (v & ~(1ULL << 19));
	if (want != v) wrmsr64(0x1FC, want);
}

static void apply(thread_call_param_t, thread_call_param_t) {
	rendezvous(eetOne, eetOff ? (void *)1 : nullptr);
	enabled = orgSetRatio != 0;   // the request is converted at XCPM's next write on each CPU
	applied = 1;
	snprintf(why, sizeof(why), "on (model 0x%02X)", cpuModel);
	SYSLOG("hpm", "on: route %d, hwp %d, autonomous %d, epp %d, eet_off %d", route, hwpOn, autonomous, eppOverride, (int)((rdmsr64(0x1FC) >> 19) & 1));
}

// ─── state for hpmstat ───────────────────────────────────────────────────────────────────────────────────────────
struct HpmCpu { uint64_t aperf, mperf, hwpReq, hwpCap; uint8_t type, temp, pad[6]; };
struct HpmState {
	uint32_t version, ncpu, tjmax, pkgTemp;
	uint64_t tsc, pkgEnergy, iaEnergy, gtEnergy, raplUnit, pkgPowerLimit, mchPowerLimit, perfLimitReasons, powerCtl,
	         turboRatio, turboRatioCores, platformInfo;
	HpmCpu cpu[MaxCpus];
};
static HpmState state;
static IOMemoryMap *mchMap;

static void sampleOne(void *) {
	const int c = cpuNumber();
	if (c < 0 || c >= MaxCpus) return;
	auto &s = state.cpu[c];
	s.aperf = rdmsr64(0xE8);
	s.mperf = rdmsr64(0xE7);
	if (rdmsr64(0x770) & 1) { s.hwpReq = rdmsr64(0x774); s.hwpCap = rdmsr64(0x771); }
	s.type = (uint8_t)(cpuidEax(0x1A) >> 24);
	const uint32_t tj = (uint32_t)(rdmsr64(0x1A2) >> 16) & 0xff, dro = (uint32_t)(rdmsr64(0x19C) >> 16) & 0x7f;
	s.temp = (uint8_t)(tj > dro ? tj - dro : 0);
	__sync_fetch_and_add(&state.ncpu, 1);
}

static void mapMchbar() {
	OSIterator *it = IOService::getMatchingServices(IOService::serviceMatching("IOPCIDevice"));
	if (!it) return;
	uint64_t mch = 0;
	while (auto *o = it->getNextObject()) {
		auto *d = OSDynamicCast(IOPCIDevice, o);
		if (d && !d->getBusNumber() && !d->getDeviceNumber() && !d->getFunctionNumber()) {
			const uint64_t v = ((uint64_t)d->configRead32(0x4C) << 32) | d->configRead32(0x48);
			if (v & 1) mch = v & 0x7FFFFE0000ULL;
			break;
		}
	}
	it->release();
	if (!mch) return;
	if (auto *md = IOMemoryDescriptor::withPhysicalAddress(mch + 0x5000, 0x1000, kIODirectionIn)) {
		mchMap = md->createMappingInTask(kernel_task, 0, kIOMapAnywhere | kIOMapInhibitCache | kIOMapReadOnly);
		md->release();
	}
}

static int stateHandler SYSCTL_HANDLER_ARGS {
	if (!rendezvous || !cpuNumber) return ENODEV;
	bzero(&state, sizeof(state));
	state.version = 1;
	rendezvous(sampleOne, nullptr);
	state.tsc = __builtin_ia32_rdtsc();
	state.tjmax = (uint32_t)(rdmsr64(0x1A2) >> 16) & 0xff;
	const uint32_t pd = (uint32_t)(rdmsr64(0x1B1) >> 16) & 0x7f;
	state.pkgTemp = state.tjmax > pd ? state.tjmax - pd : 0;
	state.raplUnit = rdmsr64(0x606);
	state.pkgEnergy = rdmsr64(0x611) & 0xffffffff;
	state.iaEnergy = rdmsr64(0x639) & 0xffffffff;
	state.gtEnergy = rdmsr64(0x641) & 0xffffffff;
	state.pkgPowerLimit = rdmsr64(0x610);
	state.perfLimitReasons = rdmsr64(0x64F);
	state.powerCtl = rdmsr64(0x1FC);
	state.turboRatio = rdmsr64(0x1AD);
	state.turboRatioCores = rdmsr64(0x1AE);
	state.platformInfo = rdmsr64(0xCE);
	if (mchMap) {
		auto *p = reinterpret_cast<volatile uint32_t *>(mchMap->getVirtualAddress() + 0x9A0);
		state.mchPowerLimit = ((uint64_t)p[1] << 32) | p[0];
	}
	return SYSCTL_OUT(req, &state, sizeof(state));
}

static int eetHandler SYSCTL_HANDLER_ARGS {
	int v = eetOff;
	int err = SYSCTL_OUT(req, &v, sizeof(v));
	if (err || !req->newptr) return err;
	if ((err = SYSCTL_IN(req, &v, sizeof(v)))) return err;
	eetOff = v != 0;
	if (applied) rendezvous(eetOne, eetOff ? (void *)1 : nullptr);
	return 0;
}

SYSCTL_NODE(_debug, OID_AUTO, hybridpm, CTLFLAG_RW, 0, "HybridPM");
SYSCTL_PROC(_debug_hybridpm, OID_AUTO, state, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, stateHandler, "S", "MSR sample");
SYSCTL_INT(_debug_hybridpm, OID_AUTO, hwp, CTLFLAG_RW, &hwpOn, 0, "convert XCPM ratios to HWP units (P-cores; on Arrow Lake E-cores too)");
SYSCTL_INT(_debug_hybridpm, OID_AUTO, autonomous, CTLFLAG_RW, &autonomous, 0, "1 = desired 0, the hardware picks the clock");
SYSCTL_INT(_debug_hybridpm, OID_AUTO, epp, CTLFLAG_RW, &eppOverride, 0, "EPP override, -1 keeps XCPM's");
SYSCTL_PROC(_debug_hybridpm, OID_AUTO, eet_off, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, eetHandler, "I", "1 = energy-efficient turbo off");
SYSCTL_INT(_debug_hybridpm, OID_AUTO, route, CTLFLAG_RD, &route, 0, "0 not tried, 1 routed, 2 no symbol, 3 no match, 4 route failed");
SYSCTL_INT(_debug_hybridpm, OID_AUTO, enabled, CTLFLAG_RD, &enabled, 0, "1 = the request is converted");
SYSCTL_STRING(_debug_hybridpm, OID_AUTO, why, CTLFLAG_RD, why, sizeof(why), "state, or why it does nothing");

// The routine builds the request from its ratio argument (edi) and writes it: prologue, then at +0x168
// "mov eax,edi; shr edx,0x18; mov ecx,0x774; wrmsr". Search forward from the exported neighbour for exactly one match.
static void off(const char *reason) {
	snprintf(why, sizeof(why), "route %d: %s", route, reason);
	SYSLOG("hpm", "off: %s", why);
}

static void processKernel(KernelPatcher &patcher) {
	rendezvous = reinterpret_cast<RendezvousFn>(patcher.solveSymbol(KernelPatcher::KernelID, "_mp_rendezvous_no_intrs"));
	cpuNumber = reinterpret_cast<CpuNumberFn>(patcher.solveSymbol(KernelPatcher::KernelID, "_cpu_number"));
	const mach_vm_address_t base = patcher.solveSymbol(KernelPatcher::KernelID, "_xcpm_enable_pstate_ctrs");
	patcher.clearError();
	if (!rendezvous || !cpuNumber || !base) { route = 2; return off("a kernel symbol is missing"); }
	static const uint8_t pro[] = {0x55, 0x48, 0x89, 0xE5, 0x53, 0x50, 0x89, 0xFB};
	static const uint8_t wr[] = {0x89, 0xF8, 0xC1, 0xEA, 0x18, 0xB9, 0x74, 0x07, 0x00, 0x00, 0x0F, 0x30};
	mach_vm_address_t fn = 0;
	int matches = 0;
	for (mach_vm_address_t a = base; a < base + 0x2000; a++) {
		if (memcmp(reinterpret_cast<const void *>(a), pro, sizeof(pro)) || memcmp(reinterpret_cast<const void *>(a + 0x168), wr, sizeof(wr))) continue;
		fn = a;
		matches++;
	}
	if (matches != 1) { route = 3; return off(matches ? "XCPM request writer matched more than once" : "XCPM request writer not found"); }
	orgSetRatio = patcher.routeFunction(fn, reinterpret_cast<mach_vm_address_t>(setRatio), true);
	if (patcher.getError() != KernelPatcher::Error::NoError) { orgSetRatio = 0; patcher.clearError(); route = 4; return off("routing failed"); }
	route = 1;
	snprintf(why, sizeof(why), "model 0x%02X, routed: on 20 s after boot", cpuModel);
	DBGLOG("hpm", "routed _xcpm_enable_pstate_ctrs+0x%llx", fn - base);
	mapMchbar();
	sysctl_register_oid(&sysctl__debug_hybridpm_hwp);
	sysctl_register_oid(&sysctl__debug_hybridpm_autonomous);
	sysctl_register_oid(&sysctl__debug_hybridpm_epp);
	sysctl_register_oid(&sysctl__debug_hybridpm_eet_off);
	uint64_t deadline;
	if (thread_call_t t = thread_call_allocate(apply, nullptr)) {
		clock_interval_to_deadline(20, kSecondScale, &deadline);
		thread_call_enter_delayed(t, deadline);
	}
}

static const char *bootargOff[] {"-hpmoff"};
static const char *bootargDebug[] {"-hpmdbg"};
static const char *bootargBeta[] {"-hpmbeta"};

PluginConfiguration ADDPR(config) {
	xStringify(PRODUCT_NAME),
	parseModuleVersion(xStringify(MODULE_VERSION)),
	LiluAPI::AllowNormal,
	bootargOff, arrsize(bootargOff),
	bootargDebug, arrsize(bootargDebug),
	bootargBeta, arrsize(bootargBeta),
	KernelVersion::BigSur,
	KernelVersion::Tahoe,
	[]() {
		int v;
		if (PE_parse_boot_argn("hpmhwp", &v, sizeof(v))) hwpOn = v != 0;
		if (PE_parse_boot_argn("hpmauto", &v, sizeof(v))) autonomous = v != 0;
		if (PE_parse_boot_argn("hpmepp", &v, sizeof(v)) && v >= -1 && v <= 255) eppOverride = v;
		if (PE_parse_boot_argn("hpmeet", &v, sizeof(v))) eetOff = v != 0;
		sysctl_register_oid(&sysctl__debug_hybridpm);
		sysctl_register_oid(&sysctl__debug_hybridpm_route);
		sysctl_register_oid(&sysctl__debug_hybridpm_enabled);
		sysctl_register_oid(&sysctl__debug_hybridpm_why);
		if (!cpuSupported()) {   // logged once; no route, no MSR access, no state sysctl
			SYSLOG("hpm", "off: %s", why);
			return;
		}
		sysctl_register_oid(&sysctl__debug_hybridpm_state);
		lilu.onPatcherLoadForce([](void *, KernelPatcher &patcher) { processKernel(patcher); });
	}
};

// Match Xcode's kext lifecycle: libkmod forwards start/stop through _realmain and _antimain.
extern "C" {
extern kern_return_t _start(kmod_info_t *, void *);
extern kern_return_t _stop(kmod_info_t *, void *);
extern kern_return_t HybridPM_kern_start(kmod_info_t *, void *);
extern kern_return_t HybridPM_kern_stop(kmod_info_t *, void *);
__attribute__((visibility("default"))) KMOD_EXPLICIT_DECL(io.github.yusufklncc.HybridPM, xStringify(MODULE_VERSION), _start, _stop)
__attribute__((visibility("hidden"))) kmod_start_func_t *_realmain = HybridPM_kern_start;
__attribute__((visibility("hidden"))) kmod_stop_func_t *_antimain = HybridPM_kern_stop;
__attribute__((visibility("hidden"))) int _kext_apple_cc = __APPLE_CC__;
}
