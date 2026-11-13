// SPDX-License-Identifier: MIT
// PCIe ASPM on the links between the GPU and the CPU's root port (debug.intelxe.aspm, debug.intelxe.aspm_l1): the pure
// parts, so that tests/aspm_test.cpp builds them on the host. The register layout is the PCIe Base Specification's as
// Linux include/uapi/linux/pci_regs.h names it; the rules are Linux drivers/pci/pcie/aspm.c (tree abf8c33, v7.4 merge
// window):
//   - L1 only where both ends of the link advertise it in Link Capabilities (pcie_aspm_cap_init, aspm.c:994);
//   - no ASPM on a link with a pre-1.1 function (no Role-Based Error Reporting, pcie_aspm_sanity_check, aspm.c:1129-1161)
//     or with a function that is not PCIe;
//   - the L1 exit latency of each link, plus 1 us for every link between it and the endpoint, must not exceed the L1
//     acceptable latency of every endpoint below it (pcie_aspm_check_latency, aspm.c:608-675); the exit latencies are
//     the Link Capabilities' as read now, which depend on the Common Clock configuration (aspm.c:961-965);
//   - Linux sets Common Clock Configuration on a link whose two ends report Slot Clock Configuration and retrains it
//     before it looks at the latencies (pcie_aspm_configure_common_clock, aspm.c:409-475). No retraining here: a link
//     that would need it (shared reference clock, CCC clear or inconsistent) is refused instead;
//   - order (PCIe r6.2 sec 7.5.3.7, aspm.c:1084-1096): L1 is enabled in the upstream component before the downstream
//     one and disabled in the downstream component first; every function of a multi-function device gets the same value.
// The kext reads the registers through IOPCIDevice and changes them only through IOPCIDevice::setASPMState (xe_aspm.cpp).
#pragma once
#include <stdint.h>
#include <stddef.h>
#if defined(KERNEL)
#include <libkern/libkern.h>   // snprintf
#else
#include <stdio.h>
#endif

// pci_regs.h: offsets in the PCI Express capability, and in the L1 PM Substates extended capability (ID 0x1e)
static constexpr uint32_t XE_PCIE_FLAGS = 0x02, XE_PCIE_DEVCAP = 0x04, XE_PCIE_LNKCAP = 0x0c, XE_PCIE_LNKCTL = 0x10,
	XE_PCIE_LNKSTA = 0x12, XE_PCIE_DEVCTL = 0x08;
static constexpr uint32_t XE_L1SS_CAP = 0x04, XE_L1SS_CTL1 = 0x08, XE_L1SS_CTL2 = 0x0c;
static constexpr uint32_t XE_PCIE_EXT_CAP_L1SS = 0x1e;
// PCI_EXP_FLAGS_TYPE values
enum : uint8_t { XE_PCIE_ENDPOINT = 0, XE_PCIE_LEG_END = 1, XE_PCIE_ROOT_PORT = 4, XE_PCIE_UPSTREAM = 5, XE_PCIE_DOWNSTREAM = 6 };
static constexpr uint32_t XE_DEVCAP_EXT_TAG = 0x20;   // PCI_EXP_DEVCAP_EXT_TAG
static constexpr uint16_t XE_DEVCTL_EXT_TAG = 0x100;   // PCI_EXP_DEVCTL_EXT_TAG
static constexpr uint32_t XE_DEVCAP_L0S_ACC = 0x1c0, XE_DEVCAP_L1_ACC = 0xe00, XE_DEVCAP_RBER = 0x8000;
static constexpr uint32_t XE_LNKCAP_ASPM_L0S = 0x400, XE_LNKCAP_ASPM_L1 = 0x800, XE_LNKCAP_L0SEL = 0x7000,
	XE_LNKCAP_L1EL = 0x38000, XE_LNKCAP_CLKPM = 0x40000;
static constexpr uint16_t XE_LNKCTL_ASPM_L0S = 0x1, XE_LNKCTL_ASPM_L1 = 0x2, XE_LNKCTL_ASPMC = 0x3, XE_LNKCTL_CCC = 0x40,
	XE_LNKCTL_CLKREQ_EN = 0x100;
static constexpr uint16_t XE_LNKSTA_CLS = 0xf, XE_LNKSTA_NLW = 0x3f0, XE_LNKSTA_SLC = 0x1000, XE_LNKSTA_DLLLA = 0x2000;
// L1SS capability / control 1: PCI-PM L1.2, PCI-PM L1.1, ASPM L1.2, ASPM L1.1 in bits 0-3 of both
static constexpr uint32_t XE_L1SS_BITS = 0xf, XE_L1SS_CAP_SS = 0x10;

static constexpr uint32_t XE_ASPM_MAX_FN = 8, XE_ASPM_MAX_LINKS = 4, XE_ASPM_MAX_EPS = 16;

// One PCIe function's registers, as read from config space. pcie false: no PCI Express capability, or the function did
// not answer (vendor 0xffff).
struct XeAspmFn {
	uint16_t vendor, device;
	uint8_t bus, dev, fn;
	bool pcie;
	uint8_t type;               // PCI_EXP_FLAGS_TYPE
	uint32_t devCap, lnkCap;
	uint16_t lnkCtl, lnkSta;
	bool l1ss;                  // the L1 PM Substates capability is present
	uint32_t l1ssCap, l1ssCtl1, l1ssCtl2;
};

// A link: the upstream component (a root port or a switch downstream port) and the functions of the downstream
// component (all on the port's secondary bus, function 0 first).
struct XeAspmLink {
	XeAspmFn up;
	XeAspmFn down[XE_ASPM_MAX_FN];
	uint32_t nDown;
};

// An endpoint below the path: its Device Capabilities, the lowest path link above it (index into the links, 0 = the
// GPU's own link) and the number of links between it and that one (0: it sits on it; an endpoint behind another
// downstream port of the card's switch is 1 link below the root port's link).
struct XeAspmEndpoint {
	uint16_t vendor, device;
	uint8_t bus, dev, fn;
	uint32_t devCap;
	uint32_t link, hops;
};

// aspm.c:478-512: exit latencies from Link Capabilities and acceptable latencies from Device Capabilities, in ns
static inline uint32_t xeAspmL0sExitNs(uint32_t lnkCap) {
	const uint32_t e = (lnkCap & XE_LNKCAP_L0SEL) >> 12;
	return e == 7 ? 5000 : 64u << e;
}
static inline uint32_t xeAspmL1ExitNs(uint32_t lnkCap) {
	const uint32_t e = (lnkCap & XE_LNKCAP_L1EL) >> 15;
	return e == 7 ? 65000 : 1000u << e;
}
static inline uint32_t xeAspmL0sAcceptableNs(uint32_t devCap) {
	const uint32_t e = (devCap & XE_DEVCAP_L0S_ACC) >> 6;
	return e == 7 ? UINT32_MAX : 64u << e;
}
static inline uint32_t xeAspmL1AcceptableNs(uint32_t devCap) {
	const uint32_t e = (devCap & XE_DEVCAP_L1_ACC) >> 9;
	return e == 7 ? UINT32_MAX : 1000u << e;
}
static inline bool xeAspmIsEndpoint(uint8_t type) { return type == XE_PCIE_ENDPOINT || type == XE_PCIE_LEG_END; }
// A pair (parent port, child function) forms a link when the parent faces downstream and the child is what sits at the
// other end of a link: an endpoint or a switch upstream port. A switch's downstream port under its upstream port is
// inside the switch, not a link.
static inline bool xeAspmIsLink(uint8_t parentType, uint8_t childType) {
	return (parentType == XE_PCIE_ROOT_PORT || parentType == XE_PCIE_DOWNSTREAM) &&
		(xeAspmIsEndpoint(childType) || childType == XE_PCIE_UPSTREAM);
}

enum XeAspmWhy : uint32_t {
	XE_ASPM_OK,
	XE_ASPM_NO_FN,             // the link has no downstream function
	XE_ASPM_NOT_PCIE,          // a function without a PCIe capability or not answering
	XE_ASPM_PRE_1_1,           // a downstream function without RBER (pre-1.1)
	XE_ASPM_NO_L1_UP,          // the upstream port does not advertise L1
	XE_ASPM_NO_L1_DOWN,        // a downstream function does not advertise L1
	XE_ASPM_CCC_INCONSISTENT,  // Common Clock Configuration differs between the ends or functions
	XE_ASPM_CCC_NOT_SET,       // both ends share the slot clock but CCC is clear (Linux would set it and retrain)
	XE_ASPM_LATENCY,           // an endpoint below does not accept the L1 exit latency
	XE_ASPM_TOO_MANY,          // more functions or endpoints than the tables hold
};
static inline const char *xeAspmWhyName(XeAspmWhy w) {
	switch (w) {
		case XE_ASPM_OK: return "ok";
		case XE_ASPM_NO_FN: return "no downstream function";
		case XE_ASPM_NOT_PCIE: return "a function has no PCIe capability or does not answer";
		case XE_ASPM_PRE_1_1: return "a downstream function is pre-1.1 (no RBER; Linux pcie_aspm_sanity_check)";
		case XE_ASPM_NO_L1_UP: return "the upstream port does not support ASPM L1 (Link Capabilities bit 11)";
		case XE_ASPM_NO_L1_DOWN: return "a downstream function does not support ASPM L1 (Link Capabilities bit 11)";
		case XE_ASPM_CCC_INCONSISTENT: return "Common Clock Configuration differs between the link's functions";
		case XE_ASPM_CCC_NOT_SET:
			return "both ends use the slot clock but Common Clock Configuration is clear (Linux sets it and retrains the "
				"link first; not done here)";
		case XE_ASPM_LATENCY: return "an endpoint does not accept the L1 exit latency (Device Capabilities 11:9)";
		case XE_ASPM_TOO_MANY: return "more functions or endpoints than the tables hold";
	}
	return "?";
}

struct XeAspmVerdict {
	XeAspmWhy why;
	uint32_t index;          // the function (down[] index, or endpoint index for XE_ASPM_LATENCY) the check failed on
	uint32_t latencyNs;      // XE_ASPM_LATENCY: the link's exit latency plus the switch latency for that endpoint,
	uint32_t acceptableNs;   //   and what the endpoint accepts
};

// May link `li` of `links` take ASPM L1? Every endpoint whose path goes through it (e.link <= li) must accept the
// larger of the two ends' L1 exit latencies plus 1 us per link between the endpoint and this one (aspm.c:651-668).
static inline XeAspmVerdict xeAspmL1Check(const XeAspmLink *links, uint32_t nLinks, uint32_t li,
	const XeAspmEndpoint *eps, uint32_t nEps) {
	XeAspmVerdict v = {XE_ASPM_OK, 0, 0, 0};
	if (li >= nLinks) { v.why = XE_ASPM_NO_FN; return v; }
	const XeAspmLink &l = links[li];
	if (!l.nDown) { v.why = XE_ASPM_NO_FN; return v; }
	if (l.nDown > XE_ASPM_MAX_FN || nEps > XE_ASPM_MAX_EPS) { v.why = XE_ASPM_TOO_MANY; return v; }
	if (!l.up.pcie) { v.why = XE_ASPM_NOT_PCIE; return v; }
	for (uint32_t i = 0; i < l.nDown; i++)
		if (!l.down[i].pcie) { v.why = XE_ASPM_NOT_PCIE; v.index = i; return v; }
	for (uint32_t i = 0; i < l.nDown; i++)
		if (!(l.down[i].devCap & XE_DEVCAP_RBER)) { v.why = XE_ASPM_PRE_1_1; v.index = i; return v; }
	if (!(l.up.lnkCap & XE_LNKCAP_ASPM_L1)) { v.why = XE_ASPM_NO_L1_UP; return v; }
	for (uint32_t i = 0; i < l.nDown; i++)
		if (!(l.down[i].lnkCap & XE_LNKCAP_ASPM_L1)) { v.why = XE_ASPM_NO_L1_DOWN; v.index = i; return v; }
	const bool ccUp = l.up.lnkCtl & XE_LNKCTL_CCC;
	for (uint32_t i = 0; i < l.nDown; i++)
		if (((l.down[i].lnkCtl & XE_LNKCTL_CCC) != 0) != ccUp) { v.why = XE_ASPM_CCC_INCONSISTENT; v.index = i; return v; }
	// aspm.c:422-430: the slot clock is shared when both ends report Slot Clock Configuration (function 0 below)
	if ((l.up.lnkSta & XE_LNKSTA_SLC) && (l.down[0].lnkSta & XE_LNKSTA_SLC) && !ccUp) { v.why = XE_ASPM_CCC_NOT_SET; return v; }
	// aspm.c:625-629: the downstream exit latency is function 0's
	const uint32_t up = xeAspmL1ExitNs(l.up.lnkCap), dw = xeAspmL1ExitNs(l.down[0].lnkCap);
	const uint32_t exit = up > dw ? up : dw;
	for (uint32_t e = 0; e < nEps; e++) {
		if (eps[e].link > li) continue;   // its path does not go through this link
		const uint64_t lat = (uint64_t)exit + 1000ull * (eps[e].hops + (li - eps[e].link));
		const uint32_t acc = xeAspmL1AcceptableNs(eps[e].devCap);
		if (lat > acc) {
			v.why = XE_ASPM_LATENCY;
			v.index = e;
			v.latencyNs = lat > UINT32_MAX ? UINT32_MAX : (uint32_t)lat;
			v.acceptableNs = acc;
			return v;
		}
	}
	return v;
}

// The ASPM Control value every end of the link has now, or -1 when they differ.
static inline int xeAspmLinkState(const XeAspmLink &l) {
	const int s = l.up.lnkCtl & XE_LNKCTL_ASPMC;
	for (uint32_t i = 0; i < l.nDown; i++)
		if ((l.down[i].lnkCtl & XE_LNKCTL_ASPMC) != s) return -1;
	return s;
}

// One IOPCIDevice::setASPMState call: on function `fn` of link `link`'s downstream component, with ASPM Control `state`
// (0, or the L0s/L1 enable bits). IOPCI2PCIBridge::setDeviceASPMState (IOPCIFamily IOPCIBridge.cpp) then writes both
// ends of that link: to enable, the bridge (upstream) first and the device second, to disable (state 0) the device
// first and the bridge second; it keeps only the bits both ends support (aspmCaps from Link Capabilities 11:10).
struct XeAspmStep {
	uint8_t link, fn;
	uint8_t state;
};

// The calls that take the path to `target` (1: L1 on every link, 0: ASPM off on every link), in the Linux order.
// Enable: links from the root port down (the upstream link first), and on each link L1 only once the link holds no L1
// enable that a direct enable call would clear in the wrong order: IOPCIFamily's enable path first clears L1 in the
// bridge and then in the device, so a link with any ASPM bit set is first disabled (device before bridge), then
// enabled. L0s stays on where every end of the link has it on now and drops elsewhere. Disable: links from the GPU up
// (the downstream link first). Within a link the functions go from the highest to function 0 for a disable and from
// function 0 up for an enable: for a multi-function device only the states enabled in every function are enabled
// (PCIe r6.2 sec 7.5.3.7), so the first disabled function already disables the component before the bridge, and the
// last enabled one enables it after the bridge. Links already in the target state get no call. Returns the number of
// steps (at most `max`; 0 also when there is nothing to do).
static inline uint32_t xeAspmPlan(const XeAspmLink *links, uint32_t nLinks, int target, XeAspmStep *steps, uint32_t max) {
	uint32_t n = 0;
	auto add = [&](uint32_t li, uint32_t fn, uint8_t st) {
		if (n < max) steps[n++] = XeAspmStep{(uint8_t)li, (uint8_t)fn, st};
	};
	if (nLinks > XE_ASPM_MAX_LINKS) return 0;
	if (target == 1) {
		for (uint32_t k = nLinks; k-- > 0;) {
			const XeAspmLink &l = links[k];
			const int now = xeAspmLinkState(l);
			const uint8_t want = (uint8_t)(XE_LNKCTL_ASPM_L1 | (now >= 0 ? (now & XE_LNKCTL_ASPM_L0S) : 0));
			if (now == want) continue;
			bool any = l.up.lnkCtl & XE_LNKCTL_ASPMC;
			for (uint32_t i = 0; i < l.nDown; i++) any = any || (l.down[i].lnkCtl & XE_LNKCTL_ASPMC);
			if (any)
				for (uint32_t i = l.nDown; i-- > 0;) add(k, i, 0);
			for (uint32_t i = 0; i < l.nDown; i++) add(k, i, want);
		}
	} else if (target == 0) {
		for (uint32_t k = 0; k < nLinks; k++) {
			const XeAspmLink &l = links[k];
			if (xeAspmLinkState(l) == 0) continue;
			for (uint32_t i = l.nDown; i-- > 0;) add(k, i, 0);
		}
	}
	return n;
}

// Report text
static inline const char *xeAspmTypeName(uint8_t t) {
	switch (t) {
		case XE_PCIE_ENDPOINT: return "endpoint";
		case XE_PCIE_LEG_END: return "legacy endpoint";
		case XE_PCIE_ROOT_PORT: return "root port";
		case XE_PCIE_UPSTREAM: return "switch upstream port";
		case XE_PCIE_DOWNSTREAM: return "switch downstream port";
		default: return "other";
	}
}
static inline const char *xeAspmCtlName(uint32_t c) {
	static const char *const k[] = {"off", "L0s", "L1", "L0s L1"};
	return k[c & 3];
}
static inline void xeAspmPut(char *b, size_t len, size_t *n, const char *s) {
	if (*n >= len) return;
	const int k = snprintf(b + *n, len - *n, "%s", s);
	if (k > 0) *n = *n + (size_t)k < len ? *n + (size_t)k : len - 1;
}
// "1 us", "64 ns", ">64 us", "any"
static inline void xeAspmNs(char *o, size_t len, uint32_t ns, bool acceptable) {
	if (ns == UINT32_MAX) snprintf(o, len, "any");
	else if (!acceptable && ns == 65000) snprintf(o, len, ">64 us");
	else if (!acceptable && ns == 5000) snprintf(o, len, ">4 us");
	else if (ns >= 1000) snprintf(o, len, "%u us", ns / 1000);
	else snprintf(o, len, "%u ns", ns);
}
// One function, two lines: "<role> bb:dd.f vvvv:dddd <type>" and its link registers decoded.
static inline void xeAspmFormatFn(char *b, size_t len, size_t *n, const char *role, const XeAspmFn &f) {
	char t[256];
	snprintf(t, sizeof t, "  %s %02x:%02x.%x %04x:%04x", role, f.bus, f.dev, f.fn, f.vendor, f.device);
	xeAspmPut(b, len, n, t);
	if (!f.pcie) {
		xeAspmPut(b, len, n, ": no PCIe capability or no answer\n");
		return;
	}
	char l0s[16], l1[16], a0[16], a1[16];
	xeAspmNs(l0s, sizeof l0s, xeAspmL0sExitNs(f.lnkCap), false);
	xeAspmNs(l1, sizeof l1, xeAspmL1ExitNs(f.lnkCap), false);
	xeAspmNs(a0, sizeof a0, xeAspmL0sAcceptableNs(f.devCap), true);
	xeAspmNs(a1, sizeof a1, xeAspmL1AcceptableNs(f.devCap), true);
	snprintf(t, sizeof t, " %s\n    LnkCap 0x%08x: ASPM support %s, exit L0s %s L1 %s, ClkPM %u, port %u\n",
		xeAspmTypeName(f.type), f.lnkCap, xeAspmCtlName(f.lnkCap >> 10), l0s, l1, (f.lnkCap & XE_LNKCAP_CLKPM) ? 1 : 0,
		f.lnkCap >> 24);
	xeAspmPut(b, len, n, t);
	snprintf(t, sizeof t, "    LnkCtl 0x%04x: ASPM %s, CommonClk %u, ClkReq %u; LnkSta 0x%04x: gen %u x%u, SlotClk %u, DLActive %u\n",
		f.lnkCtl, xeAspmCtlName(f.lnkCtl), (f.lnkCtl & XE_LNKCTL_CCC) ? 1 : 0, (f.lnkCtl & XE_LNKCTL_CLKREQ_EN) ? 1 : 0,
		f.lnkSta, f.lnkSta & XE_LNKSTA_CLS, (f.lnkSta & XE_LNKSTA_NLW) >> 4, (f.lnkSta & XE_LNKSTA_SLC) ? 1 : 0,
		(f.lnkSta & XE_LNKSTA_DLLLA) ? 1 : 0);
	xeAspmPut(b, len, n, t);
	snprintf(t, sizeof t, "    DevCap 0x%08x: acceptable L0s %s L1 %s, RBER %u\n", f.devCap, a0, a1,
		(f.devCap & XE_DEVCAP_RBER) ? 1 : 0);
	xeAspmPut(b, len, n, t);
	if (!f.l1ss) {
		xeAspmPut(b, len, n, "    L1 PM Substates: no capability\n");
		return;
	}
	auto ss = [](char *o, size_t ol, uint32_t v) {
		snprintf(o, ol, "%s%s%s%s", (v & 8) ? " ASPM-L1.1" : "", (v & 4) ? " ASPM-L1.2" : "", (v & 2) ? " PCIPM-L1.1" : "",
			(v & 1) ? " PCIPM-L1.2" : "");
		if (!(v & XE_L1SS_BITS)) snprintf(o, ol, " none");
	};
	char sc[64], sl[64];
	ss(sc, sizeof sc, f.l1ssCap);
	ss(sl, sizeof sl, f.l1ssCtl1);
	snprintf(t, sizeof t, "    L1 PM Substates: cap 0x%08x (%s), ctl1 0x%08x (enabled%s), ctl2 0x%08x\n", f.l1ssCap,
		sc + 1, f.l1ssCtl1, sl, f.l1ssCtl2);
	xeAspmPut(b, len, n, t);
}
// A link's verdict for L1: "L1 allowed" or "L1 refused: <reason>".
static inline void xeAspmFormatVerdict(char *b, size_t len, size_t *n, const XeAspmVerdict &v, const XeAspmLink &l,
	const XeAspmEndpoint *eps) {
	char t[384];
	if (v.why == XE_ASPM_OK) snprintf(t, sizeof t, "  L1 check: allowed\n");
	else if (v.why == XE_ASPM_LATENCY)
		snprintf(t, sizeof t, "  L1 check: refused: %s: %04x:%04x at %02x:%02x.%x needs %u ns or less, the link gives %u ns\n",
			xeAspmWhyName(v.why), eps[v.index].vendor, eps[v.index].device, eps[v.index].bus, eps[v.index].dev,
			eps[v.index].fn, v.acceptableNs, v.latencyNs);
	else if (v.index < l.nDown && (v.why == XE_ASPM_NOT_PCIE || v.why == XE_ASPM_PRE_1_1 || v.why == XE_ASPM_NO_L1_DOWN ||
		v.why == XE_ASPM_CCC_INCONSISTENT))
		snprintf(t, sizeof t, "  L1 check: refused: %s (%04x:%04x)\n", xeAspmWhyName(v.why), l.down[v.index].vendor,
			l.down[v.index].device);
	else snprintf(t, sizeof t, "  L1 check: refused: %s\n", xeAspmWhyName(v.why));
	xeAspmPut(b, len, n, t);
}
