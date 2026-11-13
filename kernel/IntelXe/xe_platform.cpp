// SPDX-License-Identifier: MIT
// The platform descriptor table (xe_platform.h). Values from Linux drivers/gpu/drm/xe (MIT, torvalds/master as in
// dl/linux-xe): xe_pci.c device, graphics and media descs and pciidlist; tests/xe_pci.c GMDID_CASE (IP versions of
// GMD_ID platforms); xe_uc_fw.c XE_GUC/HUC/GSC_FIRMWARE_DEFS; xe_wopcm.c xe_wopcm_size; xe_pat.c xe_pat_init_early;
// xe_mocs.c get_mocs_settings. DMC paths: i915/display/intel_dmc.c dmc_firmware_default (not in dl/linux-xe; read
// from torvalds/master 2026-11-10). Built like Linux's tables: a device desc per platform, the graphics/media IP
// features folded in by the functions below, then one entry per pciids.h id list. Constant-initialized (constexpr):
// no static constructor in the kext.
#include "xe_platform.h"
#include "xe_probe_ids.inc"   // kIds<FAMILY>[]: device ids from Linux include/drm/intel/pciids.h

namespace {

constexpr uint64_t bits(uint32_t first, uint32_t n) { return ((1ull << n) - 1) << first; }
constexpr uint64_t kVcs = bits(XE_HW_ENGINE_VCS0, 8), kVecs = bits(XE_HW_ENGINE_VECS0, 4), kCcs = bits(XE_HW_ENGINE_CCS0, 4);
constexpr uint64_t kRcs0 = 1ull << XE_HW_ENGINE_RCS0, kBcs0 = 1ull << XE_HW_ENGINE_BCS0, kBcs8 = 1ull << XE_HW_ENGINE_BCS8;
constexpr uint64_t kCcs0 = 1ull << XE_HW_ENGINE_CCS0, kGsccs0 = 1ull << XE_HW_ENGINE_GSCCS0;

// ─── graphics IPs (xe_pci.c xe_graphics_desc) ────────────────────────────────────────────────────────────────────
constexpr XePlatformDesc graphicsXelp(XePlatformDesc d) {   // graphics_xelp (12.00, 12.10)
	d.engineMask |= kRcs0 | kBcs0;
	return d;
}
constexpr XePlatformDesc graphicsXehpg(XePlatformDesc d) {   // graphics_xehpg (12.55)
	d.engineMask |= kRcs0 | kBcs0 | kCcs;
	d.hasRangeTlbInval = true;
	return d;
}
constexpr XePlatformDesc graphicsXelpg(XePlatformDesc d) {   // graphics_xelpg (12.70, 12.71, 12.74)
	d.engineMask |= kRcs0 | kBcs0 | kCcs0;
	d.hasRangeTlbInval = true;
	return d;
}
constexpr XePlatformDesc graphicsXe2(XePlatformDesc d) {   // graphics_xe2 (20.01, 20.02, 20.04, 30.00-30.05): XE2_GFX_FEATURES
	d.hasAccessCounter = d.hasAsid = d.hasAtomicPteBit = d.hasRangeTlbInval = d.hasUsm = d.has64bitTimestamp = true;
	d.engineMask |= kRcs0 | kBcs8 | kBcs0 | kCcs;
	return d;
}
// ─── media IPs (xe_media_desc) ───────────────────────────────────────────────────────────────────────────────────
constexpr XePlatformDesc mediaXem(XePlatformDesc d) { d.engineMask |= kVcs | kVecs; return d; }                // 12.00, 12.55
constexpr XePlatformDesc mediaXelpmp(XePlatformDesc d) { d.engineMask |= kVcs | kVecs | kGsccs0; return d; }   // 13.00+

// ─── device descs (xe_pci.c), with the per-platform values xe picks elsewhere ───────────────────────────────────
constexpr XePlatformDesc gen12Integrated(XePlatform p) {   // tgl_desc, rkl_desc, adl_s_desc, adl_p_desc, adl_n_desc
	XePlatformDesc d;
	d.platform = p;
	d.preGmdidGraphics = d.graphicsVerx100 = 1200;   // graphics_ip_xelp
	d.preGmdidMedia = d.mediaVerx100 = 1200;         // media_ip_xem
	d.dmaMaskBits = 39;
	d.hasCachedPt = d.hasDisplay = d.hasLlc = true;
	d.hasSriov = p != XE_ROCKETLAKE;
	d.maxGtPerTile = 1;
	d.vaBits = 48;
	d.vmMaxLevel = 3;
	d.wopcmBytes = 2u << 20;   // WOPCM_SIZE
	d.patEntries = 4;          // xelp_pat_table (GRAPHICS_VERx100 <= 1210)
	d.mocsUcIndex = 3;         // gen12_mocs_desc
	d.gucMajor = 70;
	d.gucMinor = 44;
	d.hucFw = p == XE_ALDERLAKE_N ? nullptr : "i915/tgl_huc.bin";   // no ALDERLAKE_N line in XE_HUC_FIRMWARE_DEFS
	d.gucFw = p == XE_ALDERLAKE_P ? "i915/adlp_guc_70.bin" : "i915/tgl_guc_70.bin";
	d.gucStem = p == XE_ALDERLAKE_P ? "adlp" : "tgl";
	// display: tigerlake, rocketlake, alderlake_s, alderlake_p (ADL-N is its subplatform alderlake_n)
	d.dmcFw = p == XE_TIGERLAKE ? "i915/tgl_dmc_ver2_12.bin" : p == XE_ROCKETLAKE ? "i915/rkl_dmc_ver2_03.bin" :
		p == XE_ALDERLAKE_S ? "i915/adls_dmc_ver2_01.bin" : "i915/adlp_dmc.bin";
	d.missing = "Xe_LP integrated (12.00): no VRAM, so no integrated memory path (system memory BOs, stolen DSM, GGTT "
		"in GSM), Gen12 LRC/PAT/MOCS/workaround tables, xelp_irq_handler, i915 GuC 70.44";
	return mediaXem(graphicsXelp(d));
}
constexpr XePlatformDesc dg1() {   // dg1_desc
	XePlatformDesc d;
	d.platform = XE_DG1;
	d.preGmdidGraphics = d.graphicsVerx100 = 1210;   // graphics_ip_xelpp
	d.preGmdidMedia = d.mediaVerx100 = 1200;
	d.isDgfx = true;
	d.dmaMaskBits = 39;
	d.hasDisplay = d.hasGscNvm = d.hasHeciGscfi = true;
	d.maxGtPerTile = 1;
	d.vaBits = 48;
	d.vmMaxLevel = 3;
	d.wopcmBytes = 4u << 20;   // DGFX_WOPCM_SIZE
	d.patEntries = 4;
	d.mocsUcIndex = 1;         // dg1_mocs_desc
	d.gucFw = "i915/dg1_guc_70.bin";
	d.gucStem = "dg1";
	d.gucMajor = 70;
	d.gucMinor = 44;
	d.hucFw = "i915/dg1_huc.bin";
	d.dmcFw = "i915/dg1_dmc_ver2_02.bin";
	d.missing = "Xe_LP+ discrete (12.10): Gen12 LRC/PAT/MOCS/workaround tables, no CCS, i915 GuC 70.44 and HuC";
	return mediaXem(graphicsXelp(d));
}
constexpr XePlatformDesc dg2(bool atsm) {   // dg2_desc, ats_m_desc (DG2_FEATURES)
	XePlatformDesc d;
	d.platform = XE_DG2;
	d.preGmdidGraphics = d.graphicsVerx100 = 1255;   // graphics_ip_xehpg
	d.preGmdidMedia = d.mediaVerx100 = 1255;         // media_ip_xehpm
	d.isDgfx = d.hasFlatCcs = d.hasGscNvm = d.hasHeciGscfi = true;
	d.dmaMaskBits = 46;
	d.maxGtPerTile = 1;
	d.vaBits = 48;
	d.vmMaxLevel = 3;
	d.need64kVram = true;      // XE_VRAM_FLAGS_NEED64K
	d.hasDisplay = !atsm;
	d.hasSriov = atsm;
	d.hasFanControl = !atsm;
	d.wopcmBytes = 4u << 20;
	d.patEntries = 4;          // xelp_pat_table, xehp_pat_ops
	d.mocsUcIndex = 1;         // dg2_mocs_desc
	d.gucFw = "i915/dg2_guc_70.bin";
	d.gucStem = "dg2";
	d.gucMajor = 70;
	d.gucMinor = 53;
	d.dmcFw = atsm ? nullptr : "i915/dg2_dmc_ver2_08.bin";   // no HuC line for DG2 in XE_HUC_FIRMWARE_DEFS
	d.missing = "Xe_HPG (12.55): XeHP LRC images and register lists, xelp PAT / dg2 MOCS, 64K VRAM pages, DG2 "
		"workarounds (per G10/G11/G12 stepping), no standalone media GT, i915 GuC 70.53, no HuC in xe";
	return mediaXem(graphicsXehpg(d));
}
constexpr XePlatformDesc mtl(uint16_t gfx) {   // mtl_desc (MTL and ARL)
	XePlatformDesc d;
	d.platform = XE_METEORLAKE;
	d.graphicsVerx100 = gfx;   // GMDID_CASE(METEORLAKE, 1270/1271/1274, ..., 1300)
	d.mediaVerx100 = gfx ? 1300 : 0;
	d.dmaMaskBits = 46;
	d.hasDisplay = d.hasPxp = true;
	d.maxGtPerTile = 2;
	d.vaBits = 48;
	d.vmMaxLevel = 3;
	d.wopcmBytes = 4u << 20;   // MTL_WOPCM_SIZE
	d.patEntries = 5;          // xelpg_pat_table
	d.mocsUcIndex = 9;         // mtl_mocs_desc
	d.gucFw = "i915/mtl_guc_70.bin";
	d.gucStem = "mtl";
	d.gucMajor = 70;
	d.gucMinor = 53;
	d.hucFw = "i915/mtl_huc_gsc.bin";
	d.gscFw = "i915/mtl_gsc_1.bin";   // fw_filename_gsc: <stem>_gsc_<compatibility major>.bin
	d.gscMajor = 102;
	d.gscMinor = 1;
	d.dmcFw = "i915/mtl_dmc.bin";   // display 14.00
	d.missing = "Xe_LPG integrated (12.70): integrated memory path (no VRAM/BAR2), xelpg PAT (5) / MTL MOCS (uc 9), "
		"MTL LRC images and workarounds, GSC proxy for HuC, i915 GuC 70.53";
	return mediaXelpmp(graphicsXelpg(d));
}
constexpr XePlatformDesc lnl() {   // lnl_desc
	XePlatformDesc d;
	d.platform = XE_LUNARLAKE;
	d.graphicsVerx100 = 2004;   // GMDID_CASE(LUNARLAKE, 2004, ..., 2000)
	d.mediaVerx100 = 2000;
	d.dmaMaskBits = 46;
	d.hasDisplay = d.hasFlatCcs = d.hasPxp = d.needsScratch = true;
	d.maxGtPerTile = 2;
	d.vaBits = 48;
	d.vmMaxLevel = 4;
	d.wopcmBytes = 8u << 20;   // LNL_WOPCM_SIZE (platform >= LUNARLAKE)
	d.patEntries = 32;         // xe2_pat_table (no Wa_16023588340: not 20.01)
	d.mocsUcIndex = 3;         // xe2_mocs_table
	d.gucFw = "xe/lnl_guc_70.bin";
	d.gucStem = "lnl";
	d.gucMajor = 70;
	d.gucMinor = 53;
	d.hucFw = "xe/lnl_huc.bin";
	d.gscFw = "xe/lnl_gsc_1.bin";
	d.gscMajor = 104;
	d.gscMinor = 1;
	d.dmcFw = "i915/xe2lpd_dmc.bin";   // display 20.00
	d.wa15015404425 = true;   // xe_device_wa_oob.rules: 15015404425 PLATFORM(LUNARLAKE)
	// lnl_desc sets neither has_llc nor has_cached_pt: no shared LLC, CPU-WC page tables (xe_platform.h memory model)
	d.missing = "Xe2_LPG integrated (20.04): integrated memory path (system-memory BOs and page tables, stolen DSM behind "
		"the LMEMBAR, flat CCS in system memory), 32 PAT entries without PTA, Xe2_LPG/Xe2_LPM workarounds, GSC firmware "
		"(HuC, media C6), xe/lnl firmware, xe2lpd display (docs/design/DESIGN-lunar-lake.md)";
	return mediaXelpmp(graphicsXe2(d));
}
constexpr XePlatformDesc bmg(bool g21) {   // bmg_desc
	XePlatformDesc d;
	d.platform = XE_BATTLEMAGE;
	d.subplatform = g21 ? "G21" : nullptr;   // XE_SUBPLATFORM_BATTLEMAGE_G21 (bmg_g21_ids); G31 ids: no subplatform
	// GMDID_CASE(BATTLEMAGE, 2001, A0, 1301, A1). G31 is assumed the same until its GMD_ID is read: the Linux series
	// that added the G31 ids (9b779ff0e1d1) also added graphics 20.02 (26ff87d2e776) and moved most Xe2_HPG
	// workarounds to GRAPHICS_VERSION_RANGE(2001, 2002) (a5d221924e13); DESIGN-platforms.md lists what 20.02 drops.
	d.graphicsVerx100 = 2001;
	d.mediaVerx100 = 1301;
	d.isDgfx = true;
	d.dmaMaskBits = 46;
	d.hasDisplay = d.hasFanControl = d.hasFlatCcs = d.hasMbxPowerLimits = d.hasMbxThermalInfo = true;
	d.hasGscNvm = d.hasHeciCscfi = d.hasSriov = d.needsScratch = true;
	d.hasSocRemapperTelem = true;
	// xe_hwmon.c:513-528: BMG energy is the PMT telemetry word, not an MMIO energy register; ENERGY_UNIT since BMG
	// takes its power limits through the pcode mailbox (has_mbx_power_limits, xe_hwmon.c:1499, 1527).
	d.energySource = XE_ENERGY_PMT;
	d.energyShift = 14;
	d.maxGtPerTile = 2;
	d.vaBits = 48;
	d.vmMaxLevel = 4;
	d.wopcmBytes = 8u << 20;   // LNL_WOPCM_SIZE
	d.patEntries = 28;         // Wa_16023588340 (graphics 20.01): CLOS3 disabled, entries 28-31 dropped
	d.mocsUcIndex = 3;
	d.gucFw = "xe/bmg_guc_70.bin";
	d.gucStem = "bmg";
	d.gucMajor = 70;
	d.gucMinor = 54;
	d.hucFw = "xe/bmg_huc.bin";
	d.dmcFw = "i915/bmg_dmc.bin";   // display 14.01
	d.wa14022085890 = g21;   // xe_device_wa_oob.rules: 14022085890 SUBPLATFORM(BATTLEMAGE, G21)
	d.supported = g21;
	d.missing = g21 ? nullptr : "BMG-G31 (Arc Pro B70): never run with this driver; GMD_ID unread (20.01 assumed, 20.02 "
		"would drop G21-only workarounds this driver applies); to try it boot with -intelxe_allow_g31 "
		"(docs/design/DESIGN-platforms.md)";
	d.testerBootArg = g21 ? nullptr : "intelxe_allow_g31";
	return mediaXelpmp(graphicsXe2(d));
}
constexpr XePlatformDesc ptl(uint16_t gfx, const char *dmc) {   // ptl_desc (PTL and WCL)
	XePlatformDesc d;
	d.platform = XE_PANTHERLAKE;
	d.graphicsVerx100 = gfx;   // GMDID_CASE(PANTHERLAKE, 3000, A0, 3000, A0)
	d.mediaVerx100 = gfx ? 3000 : 0;
	d.dmaMaskBits = 46;
	d.hasDisplay = d.hasFlatCcs = d.hasSriov = d.hasPxp = d.needsScratch = true;
	d.maxGtPerTile = 2;
	d.vaBits = 48;
	d.vmMaxLevel = 4;
	d.wopcmBytes = 8u << 20;
	d.patEntries = 32;         // xe3_lpg_pat_table
	d.mocsUcIndex = 3;
	d.gucFw = "xe/ptl_guc_70.bin";
	d.gucStem = "ptl";
	d.gucMajor = 70;
	d.gucMinor = 54;
	d.hucFw = "xe/ptl_huc.bin";
	d.gscFw = "xe/ptl_gsc_1.bin";
	d.gscMajor = 105;
	d.gscMinor = 1;
	d.dmcFw = dmc;
	d.wa15015404425 = true;   // PLATFORM(PANTHERLAKE); 15015404425_disable from media B0 (not known before GMD_ID)
	d.missing = "Xe3_LPG integrated (30.0x): integrated memory path (no VRAM/BAR2), Xe3 PAT (32, compressed WB 16) "
		"and LRC, Xe3 workarounds, GSC for HuC, xe/ptl firmware";
	return mediaXelpmp(graphicsXe2(d));
}

constexpr XePlatformDesc entry(const char *name, const char *sub, const uint16_t *ids, uint32_t n, XePlatformDesc d) {
	d.name = name;
	if (sub) d.subplatform = sub;
	d.ids = ids;
	d.nIds = n;
	return d;
}
#define XP_IDS(a) a, sizeof(a) / sizeof(a[0])

// BMG ids by subplatform (pciids.h INTEL_BMG_G21_IDS; the rest of INTEL_BMG_IDS has none). kIdsBMG holds both.
const uint16_t kIdsBMGG21[] = {0xe202, 0xe209, 0xe20b, 0xe20c, 0xe20d, 0xe210, 0xe211, 0xe212, 0xe216};
const uint16_t kIdsBMGG31[] = {0xe220, 0xe221, 0xe222, 0xe223};

}   // namespace

// In XeProbe's former order (xe_pci.c pciidlist order, by family): a name lookup finds the same entry as before.
constexpr XePlatformDesc kXePlatforms[] = {
	entry("TGL", nullptr, XP_IDS(kIdsTGL), gen12Integrated(XE_TIGERLAKE)),
	entry("RKL", nullptr, XP_IDS(kIdsRKL), gen12Integrated(XE_ROCKETLAKE)),
	entry("ADL-S", nullptr, XP_IDS(kIdsADLS), gen12Integrated(XE_ALDERLAKE_S)),
	entry("RPL-S", "RPLS", XP_IDS(kIdsRPLS), gen12Integrated(XE_ALDERLAKE_S)),
	entry("ADL-P", nullptr, XP_IDS(kIdsADLP), gen12Integrated(XE_ALDERLAKE_P)),
	entry("RPL-U", "RPLU", XP_IDS(kIdsRPLU), gen12Integrated(XE_ALDERLAKE_P)),
	entry("RPL-P", nullptr, XP_IDS(kIdsRPLP), gen12Integrated(XE_ALDERLAKE_P)),
	entry("ADL-N", nullptr, XP_IDS(kIdsADLN), gen12Integrated(XE_ALDERLAKE_N)),
	entry("DG1", nullptr, XP_IDS(kIdsDG1), dg1()),
	entry("DG2", nullptr, XP_IDS(kIdsDG2), dg2(false)),
	entry("ATS-M", nullptr, XP_IDS(kIdsATSM), dg2(true)),
	entry("MTL", nullptr, XP_IDS(kIdsMTL), mtl(1270)),
	entry("ARL", nullptr, XP_IDS(kIdsARL), mtl(0)),        // mtl_desc; graphics 12.70 or 12.74 by SKU (GMD_ID)
	entry("LNL", nullptr, XP_IDS(kIdsLNL), lnl()),
	entry("BMG", nullptr, XP_IDS(kIdsBMGG21), bmg(true)),
	entry("BMG", nullptr, XP_IDS(kIdsBMGG31), bmg(false)),
	entry("PTL", nullptr, XP_IDS(kIdsPTL), ptl(3000, "i915/xe3lpd_dmc.bin")),        // display 30.00
	entry("WCL", nullptr, XP_IDS(kIdsWCL), ptl(0, "i915/xe3lpd_3002_dmc.bin")),      // ptl_desc; display 30.02
};
const uint32_t kXePlatformCount = sizeof(kXePlatforms) / sizeof(kXePlatforms[0]);
static_assert(sizeof(kIdsBMG) == sizeof(kIdsBMGG21) + sizeof(kIdsBMGG31), "kIdsBMG = G21 + G31 (tests/platform_test.cpp)");

const XePlatformDesc *xePlatformFind(uint16_t devId) {
	for (uint32_t p = 0; p < kXePlatformCount; p++)
		for (uint32_t i = 0; i < kXePlatforms[p].nIds; i++)
			if (kXePlatforms[p].ids[i] == devId) return &kXePlatforms[p];
	return nullptr;
}

const XePlatformDesc *xePlatformByName(const char *name) {
	for (uint32_t p = 0; p < kXePlatformCount; p++) {
		const char *a = kXePlatforms[p].name, *b = name;
		while (*a && *a == *b) a++, b++;
		if (*a == *b) return &kXePlatforms[p];
	}
	return nullptr;
}
