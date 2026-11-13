// SPDX-License-Identifier: MIT
// platform_test: offline check of the platform descriptor table (xe_platform.cpp), no GPU needed.
//   1. every id of xe_probe_ids.inc finds exactly one entry, the one that lists it; unknown ids find none
//   2. only the BMG-G21 entry is supported (and IntelXe's IOPCIMatch ids, e20b/e20c/e211/e212, are in it); every other
//      entry carries the reason it is refused; only BMG-G31 has a tester boot-arg (build.py G31_TESTER_MATCH)
//   3. the BMG-G21 values equal the constants the code used before the descriptor (2026-11-10, b17eac9)
//   4. XeProbe's former family table (copied below as it was) gives the same family and values per id and per name
//   5. the BMG ids equal Linux pciids.h INTEL_BMG_G21_IDS / INTEL_BMG_IDS, by subplatform
//   6. Wa_14022085890 (SLPC minimum on the media GT) on BMG-G21 only, BMG_MIN_FREQ on every BMG primary GT
//   7. the LNL entry equals Linux lnl_desc (with graphics_xe2, media_xelpmp, firmware, WOPCM, PAT, MOCS) field by field;
//      the GSC firmware names follow xe_uc_fw.c fw_filename_gsc
//   8. the memory model every entry derives from is_dgfx (integrated, LLC, regions, stolen, BAR2, GSM, coherency, CCS)
//   9. memory regions (xe_memregion.h): uAPI regions, user BO and kernel object placement, BMG unchanged
//  10. the integrated stolen-memory decode (detect_lmembar_integrated) on register values
// c++ -std=c++17 -Wall -Wextra -I.. -o platform_test platform_test.cpp ../xe_platform.cpp && ./platform_test
#include <stdio.h>
#include <string.h>
#include "xe_platform.h"
#include "xe_memregion.h"
#include "xe_probe_ids.inc"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// xe_probe.cpp kFamilies before the descriptor (b17eac9): name, ids, gfx/media verx100 for families without GMD_ID
// (0 = read GMD_ID), dgfx, xe_wopcm_size, GuC file stem, xe_mocs uc_index
struct OldFamily { const char *name; const uint16_t *ids; uint32_t nIds; uint16_t gfx, media; bool dgfx; uint32_t wopcm;
	const char *guc; uint8_t mocsUc; };
#define XP_IDS(a) a, sizeof(a) / sizeof(a[0])
static const OldFamily kOld[] = {
	{"TGL", XP_IDS(kIdsTGL), 1200, 1200, false, 2u << 20, "tgl", 3},
	{"RKL", XP_IDS(kIdsRKL), 1200, 1200, false, 2u << 20, "tgl", 3},
	{"ADL-S", XP_IDS(kIdsADLS), 1200, 1200, false, 2u << 20, "tgl", 3},
	{"RPL-S", XP_IDS(kIdsRPLS), 1200, 1200, false, 2u << 20, "tgl", 3},
	{"ADL-P", XP_IDS(kIdsADLP), 1200, 1200, false, 2u << 20, "adlp", 3},
	{"RPL-U", XP_IDS(kIdsRPLU), 1200, 1200, false, 2u << 20, "adlp", 3},
	{"RPL-P", XP_IDS(kIdsRPLP), 1200, 1200, false, 2u << 20, "adlp", 3},
	{"ADL-N", XP_IDS(kIdsADLN), 1200, 1200, false, 2u << 20, "tgl", 3},
	{"DG1", XP_IDS(kIdsDG1), 1210, 1200, true, 4u << 20, "dg1", 1},
	{"DG2", XP_IDS(kIdsDG2), 1255, 1255, true, 4u << 20, "dg2", 1},
	{"ATS-M", XP_IDS(kIdsATSM), 1255, 1255, true, 4u << 20, "dg2", 1},
	{"MTL", XP_IDS(kIdsMTL), 0, 0, false, 4u << 20, "mtl", 9},
	{"ARL", XP_IDS(kIdsARL), 0, 0, false, 4u << 20, "mtl", 9},
	{"LNL", XP_IDS(kIdsLNL), 0, 0, false, 8u << 20, "lnl", 3},
	{"BMG", XP_IDS(kIdsBMG), 0, 0, true, 8u << 20, "bmg", 3},
	{"PTL", XP_IDS(kIdsPTL), 0, 0, false, 8u << 20, "ptl", 3},
	{"WCL", XP_IDS(kIdsWCL), 0, 0, false, 8u << 20, "ptl", 3},
};

static bool same(const char *a, const char *b) { return a && b && !strcmp(a, b); }

int main() {
	// 1. lookup of every listed id
	uint32_t ids = 0;
	for (const OldFamily &f : kOld)
		for (uint32_t i = 0; i < f.nIds; i++, ids++) {
			const uint16_t id = f.ids[i];
			const XePlatformDesc *d = xePlatformFind(id);
			CHECK(d, "8086:%04x (%s): no descriptor", id, f.name);
			if (!d) continue;
			bool listed = false;
			for (uint32_t k = 0; k < d->nIds; k++) listed |= d->ids[k] == id;
			CHECK(listed, "8086:%04x: entry %s does not list it", id, d->name);
			uint32_t holders = 0;
			for (uint32_t p = 0; p < kXePlatformCount; p++)
				for (uint32_t k = 0; k < kXePlatforms[p].nIds; k++) holders += kXePlatforms[p].ids[k] == id;
			CHECK(holders == 1, "8086:%04x in %u entries", id, holders);
			// 4. XeProbe's view of the id: same family and values as its old table
			const uint16_t gfx = d->preGmdidGraphics, media = d->preGmdidMedia;
			CHECK(same(d->name, f.name), "8086:%04x: family %s, was %s", id, d->name, f.name);
			CHECK(gfx == f.gfx && media == f.media, "8086:%04x: pre-GMD_ID IP %u/%u, was %u/%u", id, gfx, media, f.gfx, f.media);
			CHECK(d->isDgfx == f.dgfx, "8086:%04x: dgfx %d, was %d", id, d->isDgfx, f.dgfx);
			CHECK(d->wopcmBytes == f.wopcm, "8086:%04x: WOPCM 0x%x, was 0x%x", id, d->wopcmBytes, f.wopcm);
			CHECK(same(d->gucStem, f.guc), "8086:%04x: GuC stem %s, was %s", id, d->gucStem, f.guc);
			CHECK(d->mocsUcIndex == f.mocsUc, "8086:%04x: MOCS uc %u, was %u", id, d->mocsUcIndex, f.mocsUc);
		}
	uint32_t tableIds = 0;
	for (uint32_t p = 0; p < kXePlatformCount; p++) tableIds += kXePlatforms[p].nIds;
	CHECK(tableIds == ids, "table has %u ids, xe_probe_ids.inc %u", tableIds, ids);
	static const uint16_t kUnknown[] = {0x0000, 0xffff, 0x1234, 0x9a48, 0xe20a, 0xe224, 0x56c3};
	for (uint16_t id : kUnknown) CHECK(!xePlatformFind(id), "8086:%04x found", id);
	for (const OldFamily &f : kOld) {
		const XePlatformDesc *d = xePlatformByName(f.name);
		CHECK(d && same(d->name, f.name) && d->wopcmBytes == f.wopcm && same(d->gucStem, f.guc) && d->isDgfx == f.dgfx &&
			d->mocsUcIndex == f.mocsUc && d->preGmdidGraphics == f.gfx && d->preGmdidMedia == f.media,
			"name %s: different entry", f.name);
	}
	CHECK(!xePlatformByName("BM") && !xePlatformByName("BMGX") && !xePlatformByName(""), "partial names match");

	// 2. only BMG-G21 is supported; every entry is complete
	for (uint32_t p = 0; p < kXePlatformCount; p++) {
		const XePlatformDesc &d = kXePlatforms[p];
		const bool g21 = d.platform == XE_BATTLEMAGE && same(d.subplatform, "G21");
		CHECK(d.supported == g21, "%s %s: supported %d", d.name, d.subplatform ? d.subplatform : "", d.supported);
		CHECK(d.supported || (d.missing && *d.missing), "%s: refused without a reason", d.name);
		CHECK(d.platform != XE_PLATFORM_UNINITIALIZED && d.vaBits == 48 && d.vmMaxLevel >= 3 && d.dmaMaskBits >= 39 &&
			d.wopcmBytes && d.patEntries && d.mocsUcIndex && d.gucFw && d.gucStem && d.engineMask, "%s: incomplete", d.name);
		// gucFw is <dir>/<stem>_guc_70.bin (MAKE_FW_PATH with major_ver)
		char want[64];
		snprintf(want, sizeof(want), "%s/%s_guc_70.bin", d.platform >= XE_LUNARLAKE ? "xe" : "i915", d.gucStem);
		CHECK(same(d.gucFw, want), "%s: GuC %s, expected %s", d.name, d.gucFw, want);
		CHECK(!d.preGmdidGraphics || (d.preGmdidGraphics == d.graphicsVerx100 && d.preGmdidMedia == d.mediaVerx100),
			"%s: pre-GMD_ID IP differs from the IP", d.name);
		CHECK((d.maxGtPerTile == 2) == (d.platform >= XE_METEORLAKE), "%s: standalone media GT", d.name);
		CHECK(d.hasUsm == (d.graphicsVerx100 >= 2000 || d.platform >= XE_LUNARLAKE), "%s: has_usm", d.name);
	}
	static const uint16_t kMatch[] = {0xe20b, 0xe20c, 0xe211, 0xe212};   // build.py IOPCIMatch
	for (uint16_t id : kMatch) {
		const XePlatformDesc *d = xePlatformFind(id);
		CHECK(d && d->supported, "8086:%04x (IOPCIMatch) not supported", id);
	}
	for (uint32_t i = 0; i < kXePlatformCount; i++) {
		const XePlatformDesc &d = kXePlatforms[i];
		const bool g31 = d.platform == XE_BATTLEMAGE && !d.subplatform;
		CHECK(!d.supported || !d.testerBootArg, "%s: supported and a tester boot-arg", d.name);
		CHECK(!d.testerBootArg == !g31, "%s: only BMG-G31 has a tester boot-arg", d.name);
		CHECK(!d.testerBootArg || (d.testerBootArg[0] != '-' && strstr(d.missing, d.testerBootArg)),
			"%s: tester boot-arg %s (no leading '-'), named in the refusal line", d.name, d.testerBootArg);
	}

	// 3. BMG-G21 = the constants the code used before (file:line at b17eac9)
	const XePlatformDesc *b = xePlatformFind(0xe20b);
	if (b) {
		CHECK(b == xePlatformFind(0xe20c), "B580 and B570 in different entries");
		CHECK(b->platform == XE_BATTLEMAGE && same(b->name, "BMG") && same(b->subplatform, "G21"), "BMG identity");
		CHECK(b->graphicsVerx100 == 2001, "graphics %u: QUERY_GT_LIST ip_ver 20.1 (xe_core.cpp:4698)", b->graphicsVerx100);
		CHECK(b->mediaVerx100 == 1301, "media %u: QUERY_GT_LIST ip_ver 13.1 (xe_core.cpp:4704)", b->mediaVerx100);
		CHECK(b->vaBits == 48, "va_bits %u: QUERY_CONFIG_VA_BITS 48, kVmSize 1 << 48 (xe_core.cpp:4687, 5085, 5570, 7423)", b->vaBits);
		CHECK(xePlatformVmSize(b) == 1ull << 48, "VM size");
		CHECK(b->isDgfx, "dGFX: QUERY_CONFIG_FLAG_HAS_VRAM (xe_core.cpp:4683)");
		CHECK(b->dmaMaskBits == 46, "dma_mask %u: DmaMap 46 bits, GGTT pa < 1 << 46 (IntelXe.cpp:1136, 1240)", b->dmaMaskBits);
		CHECK(xePlatformDmaLimit(b) == 1ull << 46, "DMA limit");
		CHECK(xePlatformPhysMask(b) == 0x00003ffffffff000ull, "alloc physical mask (IntelXe.cpp:1227)");
		CHECK(b->wopcmBytes == 8u << 20, "WOPCM 0x%x: loadGuc wopcmSize 8 MB (IntelXe.cpp:1725)", b->wopcmBytes);
		CHECK(b->patEntries == 28, "PAT %u: kPatEntries 28, madvise 28 (xe_core.cpp:5043, 7393)", b->patEntries);
		CHECK(b->mocsUcIndex == 3, "MOCS uc %u: shim 3 << 24, MEM_SET 3 << 3 (IntelXe.cpp:1863, xe_core.cpp:3732, 5748)",
			b->mocsUcIndex);
		CHECK(b->hasUsm, "has_usm: gUsm 1 (IntelXe.cpp:84)");
		CHECK(b->maxGtPerTile == 2, "media GT: gVideoWanted 1 (IntelXe.cpp:86)");
		CHECK(xePlatformEngines(b, XE_HW_ENGINE_CCS0, 4) == 0xf, "CCS0-3: fuse mask kept whole (IntelXe.cpp:2344)");
		CHECK(xePlatformEngines(b, XE_HW_ENGINE_VCS0, 8) == 0xff, "VCS0-7: VDBOX fuse kept whole (IntelXe.cpp:2711)");
		CHECK(xePlatformEngines(b, XE_HW_ENGINE_VECS0, 4) == 0xf, "VECS0-3: VEBOX fuse kept whole (IntelXe.cpp:2712)");
		CHECK(xePlatformEngines(b, XE_HW_ENGINE_RCS0, 1) == 1 && xePlatformEngines(b, XE_HW_ENGINE_BCS0, 1) == 1,
			"RCS0, BCS0");
		CHECK(same(b->gucFw, "xe/bmg_guc_70.bin"), "GuC %s: build.py GUC bmg_guc_70.bin", b->gucFw);
		CHECK(same(b->hucFw, "xe/bmg_huc.bin"), "HuC %s: build.py HUC bmg_huc.bin", b->hucFw);
		CHECK(b->gucMajor == 70 && b->gucMinor <= 72, "GuC %u.%u: the built-in image is 70.72.1", b->gucMajor, b->gucMinor);
	}
	const XePlatformDesc *g31 = xePlatformFind(0xe221);
	CHECK(g31 && g31 != b && !g31->supported && g31->platform == XE_BATTLEMAGE, "BMG-G31 entry");
	// 5. the BMG ids against Linux include/drm/intel/pciids.h:852-868 (tree abf8c33): INTEL_BMG_G21_IDS in the supported
	// G21 entry (the Pro B60 e211 and Pro B50 e212 among them, matched since 2026-11-13), the rest of INTEL_BMG_IDS (G31) in the refused entry. Mesa's iris_pci_ids.h also names 0xe215, which
	// Linux does not list: no entry, as xe would not bind it either.
	static const uint16_t kLinuxG21[] = {0xe202, 0xe209, 0xe20b, 0xe20c, 0xe20d, 0xe210, 0xe211, 0xe212, 0xe216};
	static const uint16_t kLinuxG31[] = {0xe220, 0xe221, 0xe222, 0xe223};
	for (uint16_t id : kLinuxG21)
		CHECK(xePlatformFind(id) == b, "8086:%04x: Linux INTEL_BMG_G21_IDS, not in the G21 entry", id);
	for (uint16_t id : kLinuxG31)
		CHECK(xePlatformFind(id) == g31, "8086:%04x: Linux INTEL_BMG_IDS (G31), not in the G31 entry", id);
	CHECK(b && b->nIds == sizeof kLinuxG21 / sizeof kLinuxG21[0] && g31 && g31->nIds == sizeof kLinuxG31 / sizeof kLinuxG31[0],
		"BMG entries hold ids Linux does not list");
	CHECK(!xePlatformFind(0xe215), "8086:e215 (Mesa only) has no entry");

	// 6. Wa_14022085890: xe_device_wa_oob.rules SUBPLATFORM(BATTLEMAGE, G21); xe_guc_pc.c pc_needs_min_freq_change:
	// the workaround (both GTs), or platform BATTLEMAGE on the primary GT
	CHECK(b && b->wa14022085890 && xePlatformSlpcMinPrimary(b) && xePlatformSlpcMinMedia(b), "G21: SLPC min on both GTs");
	CHECK(g31 && !g31->wa14022085890 && xePlatformSlpcMinPrimary(g31) && !xePlatformSlpcMinMedia(g31),
		"G31: SLPC min on the primary GT only");
	CHECK(g31 && g31->testerBootArg && same(g31->testerBootArg, "intelxe_allow_g31"), "G31 tester boot-arg");
	for (uint32_t i = 0; i < kXePlatformCount; i++) {
		const XePlatformDesc &d = kXePlatforms[i];
		CHECK(!d.wa14022085890 || (d.platform == XE_BATTLEMAGE && same(d.subplatform, "G21")), "%s: Wa_14022085890",
			d.name);
		CHECK(xePlatformSlpcMinPrimary(&d) == (d.platform == XE_BATTLEMAGE), "%s: BMG_MIN_FREQ on the primary GT", d.name);
	}

	// 7. LNL = Linux lnl_desc, field by field. dl/linux-xe (abf8c33) xe_pci.c:399-410 lnl_desc, :105-121 graphics_xe2
	// (XE2_GFX_FEATURES), :152-157 media_xelpmp; tests/xe_pci.c:58-59 GMDID_CASE(LUNARLAKE, 2004, A0/B0, 2000, A0);
	// xe_uc_fw.c:121/134/146; xe_wopcm.c:182 (platform >= LUNARLAKE: LNL_WOPCM_SIZE); xe_pat.c:619-641 (graphics 20, not
	// 20.01: all 32 entries; no PTA on an integrated part); xe_mocs.c:606-614 (xe2_mocs_table, uc_index 3); pciids.h
	// INTEL_LNL_IDS (v6.18 :846-849). v6.18 has the same values (va_bits, vm_max_level and has_flat_ccs then in
	// XE2_GFX_FEATURES; no has_access_counter yet) and wants GuC 70.45.2; master wants 70.53.0. DMC: i915 intel_dmc.c
	// dmc_firmware_default, DISPLAY_VERx100 2000 -> XE2LPD_DMC_PATH (v6.18 :195-197).
	const XePlatformDesc *l = xePlatformFind(0x64a0);
	CHECK(l && same(l->name, "LNL") && l->platform == XE_LUNARLAKE && !l->subplatform, "LNL identity");
	if (l) {
		static const uint16_t kLinuxLnl[] = {0x6420, 0x64a0, 0x64b0};
		CHECK(l->nIds == 3, "LNL: %u ids, INTEL_LNL_IDS has 3", l->nIds);
		for (uint16_t id : kLinuxLnl) CHECK(xePlatformFind(id) == l, "8086:%04x: INTEL_LNL_IDS, not in the LNL entry", id);
		CHECK(!l->preGmdidGraphics && !l->preGmdidMedia && l->graphicsVerx100 == 2004 && l->mediaVerx100 == 2000,
			"LNL IP %u/%u (pre-GMD_ID %u/%u): GMD_ID 20.04 / 20.00", l->graphicsVerx100, l->mediaVerx100,
			l->preGmdidGraphics, l->preGmdidMedia);
		CHECK(l->dmaMaskBits == 46 && l->vaBits == 48 && l->vmMaxLevel == 4 && l->maxGtPerTile == 2,
			"LNL dma_mask_size 46, va_bits 48, vm_max_level 4, max_gt_per_tile 2");
		CHECK(l->hasDisplay && l->hasFlatCcs && l->hasPxp && l->needsScratch, "LNL has_display, has_flat_ccs, has_pxp, needs_scratch");
		// everything lnl_desc does not set (DGFX_FEATURES, has_llc, has_cached_pt, has_sriov, has_gsc_nvm, has_heci_*, fan,
		// mailbox power/thermal, soc remapper telemetry, XE_VRAM_FLAGS_NEED64K)
		CHECK(!l->isDgfx && !l->hasLlc && !l->hasCachedPt && !l->hasSriov && !l->hasGscNvm && !l->hasHeciGscfi &&
			!l->hasHeciCscfi && !l->hasFanControl && !l->hasMbxPowerLimits && !l->hasMbxThermalInfo && !l->need64kVram &&
			!l->hasSocRemapperTelem, "LNL: a flag lnl_desc does not set");
		CHECK(l->hasAccessCounter && l->hasAsid && l->hasAtomicPteBit && l->hasRangeTlbInval && l->hasUsm && l->has64bitTimestamp,
			"LNL: XE2_GFX_FEATURES");
		const uint64_t wantEngines = (1ull << XE_HW_ENGINE_RCS0) | (1ull << XE_HW_ENGINE_BCS0) | (1ull << XE_HW_ENGINE_BCS8) |
			(0xfull << XE_HW_ENGINE_CCS0) | (0xffull << XE_HW_ENGINE_VCS0) | (0xfull << XE_HW_ENGINE_VECS0) |
			(1ull << XE_HW_ENGINE_GSCCS0);
		CHECK(l->engineMask == wantEngines, "LNL engines 0x%llx, graphics_xe2 | media_xelpmp 0x%llx",
			(unsigned long long)l->engineMask, (unsigned long long)wantEngines);
		CHECK(l->wopcmBytes == 8u << 20 && l->patEntries == 32 && l->mocsUcIndex == 3, "LNL WOPCM 8 MB, 32 PAT, MOCS uc 3");
		CHECK(same(l->gucFw, "xe/lnl_guc_70.bin") && same(l->gucStem, "lnl") && l->gucMajor == 70 && l->gucMinor == 53,
			"LNL GuC %s %u.%u", l->gucFw, l->gucMajor, l->gucMinor);
		CHECK(same(l->hucFw, "xe/lnl_huc.bin"), "LNL HuC %s", l->hucFw);
		CHECK(same(l->gscFw, "xe/lnl_gsc_1.bin") && l->gscMajor == 104 && l->gscMinor == 1, "LNL GSC %s cv%u.%u", l->gscFw,
			l->gscMajor, l->gscMinor);
		CHECK(same(l->dmcFw, "i915/xe2lpd_dmc.bin"), "LNL DMC %s", l->dmcFw);
		CHECK(l->wa15015404425, "LNL: Wa_15015404425 (xe_device_wa_oob.rules PLATFORM(LUNARLAKE))");
		CHECK(!l->supported && !l->testerBootArg && l->missing && !l->wa14022085890 && l->energySource == XE_ENERGY_NONE,
			"LNL refused at start, no tester boot-arg, no BMG workaround or energy source");
	}
	// The GSC file is named by the compatibility major (xe_uc_fw.c fw_filename_gsc: "_" __stringify(b)), not the project
	// version: i915/mtl_gsc_1.bin, xe/lnl_gsc_1.bin, xe/ptl_gsc_1.bin (linux-firmware has those names)
	for (uint32_t i = 0; i < kXePlatformCount; i++) {
		const XePlatformDesc &d = kXePlatforms[i];
		const bool gsc = d.platform == XE_METEORLAKE || d.platform == XE_LUNARLAKE || d.platform == XE_PANTHERLAKE;
		CHECK(!d.gscFw == !gsc && !d.gscMajor == !gsc, "%s: a GSC firmware exactly on MTL, LNL, PTL", d.name);
		if (!d.gscFw) continue;
		char want[64];
		snprintf(want, sizeof(want), "%s/%s_gsc_%u.bin", d.platform >= XE_LUNARLAKE ? "xe" : "i915", d.gucStem, d.gscMinor);
		CHECK(same(d.gscFw, want), "%s: GSC %s, expected %s", d.name, d.gscFw, want);
		CHECK(d.gscMinor == 1 && d.gscMajor == (d.platform == XE_METEORLAKE ? 102 : d.platform == XE_LUNARLAKE ? 104 : 105),
			"%s: GSC compatibility version %u.%u", d.name, d.gscMajor, d.gscMinor);
	}

	for (uint32_t i = 0; i < kXePlatformCount; i++)
		CHECK(kXePlatforms[i].wa15015404425 == (kXePlatforms[i].platform == XE_LUNARLAKE ||
			kXePlatforms[i].platform == XE_PANTHERLAKE), "%s: Wa_15015404425 on LNL and PTL only", kXePlatforms[i].name);

	// 8. The memory model (xe_platform.h) for every entry, against what xe derives: integrated = !is_dgfx; the region
	// mask and the GT list's near/far regions (xe_device.c, xe_tile.c, xe_query.c); stolen (xe_ttm_stolen_mgr.c); BAR2;
	// GSM size (xe_ggtt.c); CPU-WC page tables (xe_bo.c xe_ttm_tt_create); system-memory flat CCS (xe_bo_needs_ccs_pages);
	// the PTA PAT entry (xe_pat.c); discrete-only barrier and LMEM_INIT/pcode waits.
	for (uint32_t i = 0; i < kXePlatformCount; i++) {
		const XePlatformDesc &d = kXePlatforms[i];
		const bool gen12 = d.platform <= XE_ALDERLAKE_N, dg = d.platform == XE_DG1 || d.platform == XE_DG2 ||
			d.platform == XE_BATTLEMAGE;
		const bool lpg = d.platform == XE_METEORLAKE || d.platform == XE_LUNARLAKE || d.platform == XE_PANTHERLAKE;
		CHECK(d.isDgfx == dg && xePlatformIntegrated(&d) == !dg && xePlatformHasVram(&d) == dg, "%s: integrated", d.name);
		CHECK(xePlatformHasLlc(&d) == gen12, "%s: has_llc only on Gen12 integrated", d.name);
		CHECK(xePlatformMemRegionMask(&d) == (dg ? 3u : 1u), "%s: mem_region_mask", d.name);
		CHECK(xePlatformNearMemRegions(&d) == (dg ? 2u : 1u) && xePlatformFarMemRegions(&d) == (dg ? 1u : 0u),
			"%s: near/far regions", d.name);
		const XeStolenModel sm = dg ? XE_STOLEN_LMEMBAR_DGFX : lpg ? XE_STOLEN_LMEMBAR_IGFX : XE_STOLEN_BIOS;
		CHECK(xePlatformStolen(&d) == sm, "%s: stolen model %u, want %u", d.name, xePlatformStolen(&d), sm);
		CHECK(xePlatformStolenCpuViaGgtt(&d) == gen12, "%s: stolen through the GGTT aperture", d.name);
		CHECK(xePlatformBar2IsVram(&d) == dg, "%s: BAR2 is VRAM", d.name);
		CHECK(xePlatformGsmBytes(&d) == ((gen12 || d.platform == XE_DG1) ? 0u : 8u << 20), "%s: GSM size", d.name);
		CHECK(xePlatformSysAlwaysCoherent(&d) == dg, "%s: system memory always coherent", d.name);
		CHECK(xePlatformPtWriteCombined(&d) == lpg, "%s: CPU-WC page tables (no has_cached_pt)", d.name);
		CHECK(xePlatformSysCcs(&d) == (d.platform == XE_LUNARLAKE || d.platform == XE_PANTHERLAKE),
			"%s: flat CCS in system memory", d.name);
		CHECK(xePlatformPatPta(&d) == dg && xePlatformWmbMmio(&d) == dg && xePlatformLmemInitWait(&d) == dg,
			"%s: discrete-only PTA, VF_CAP barrier, LMEM_INIT wait", d.name);
		CHECK(xePlatformGraphicsMin(&d) >= 1200, "%s: graphics minimum %u", d.name, xePlatformGraphicsMin(&d));
	}
	if (b) {   // BMG: the answers the code assumed before (VRAM bit 1, near VRAM, far system: xe_core.cpp QUERY_GT_LIST)
		CHECK(xePlatformMemRegionMask(b) == 3 && xePlatformNearMemRegions(b) == 1u << 1 && xePlatformFarMemRegions(b) == 1u << 0,
			"BMG regions");
		CHECK(xePlatformBar2IsVram(b) && xePlatformHasVram(b) && !xePlatformSysCcs(b) && xePlatformGsmBytes(b) == 8u << 20,
			"BMG memory model");
	}

	// 9. Memory regions (xe_memregion.h): the uAPI regions and the region of each BO and kernel object
	if (b && l) {
		CHECK(xeMemQueryRegions(b) == 2 && xeMemQueryRegions(l) == 1, "QUERY_MEM_REGIONS: BMG 2 regions, LNL 1");
		CHECK(xeMemUserRegion(b, 1) == XE_MEM_SYSTEM && xeMemUserRegion(b, 2) == XE_MEM_VRAM0 &&
			xeMemUserRegion(b, 3) == XE_MEM_VRAM0, "BMG: placement & 2 is VRAM, as GEM_CREATE did");
		CHECK(xeMemUserRegion(l, 1) == XE_MEM_SYSTEM && xeMemUserRegion(l, 2) == XE_MEM_SYSTEM &&
			xeMemUserRegion(l, 3) == XE_MEM_SYSTEM, "LNL: system memory only");
		static const XeKernelObject kObjs[] = {XE_KOBJ_PAGETABLE, XE_KOBJ_LRC, XE_KOBJ_GUC_UM, XE_KOBJ_DISPLAY, XE_KOBJ_INITIAL_FB};
		for (XeKernelObject o : kObjs) {
			CHECK(xeMemKernelRegion(b, o) == XE_MEM_VRAM0, "BMG: kernel object %u in VRAM", o);
			CHECK(xeMemKernelRegion(l, o) == (o == XE_KOBJ_INITIAL_FB ? XE_MEM_STOLEN : XE_MEM_SYSTEM),
				"LNL: kernel object %u", o);
		}
	}

	// 10. Stolen memory of an integrated part from 12.70 (xe_ttm_stolen_mgr.c detect_lmembar_integrated, get_wopcm_size)
	{
		CHECK(xeStolenWopcmBytes(0u << 7) == 1u << 20 && xeStolenWopcmBytes(3u << 7) == 8u << 20 &&
			xeStolenWopcmBytes(5u << 7) == 16u << 20 && xeStolenWopcmBytes(6u << 7) == 32u << 20 &&
			!xeStolenWopcmBytes(4u << 7) && !xeStolenWopcmBytes(7u << 7), "get_wopcm_size");
		CHECK(xeStolenWopcmBytes(~0ull & ~(7ull << 7) | (2ull << 7)) == 4u << 20, "WOPCM_SIZE is bits 9:7 only");
		CHECK(xeStolenGmsBytes(0x0000) == 0 && xeStolenGmsBytes(0x0200) == 64ull << 20 && xeStolenGmsBytes(0x0400) == 128ull << 20 &&
			xeStolenGmsBytes(0xf000) == 4ull << 20 && xeStolenGmsBytes(0xfe00) == 60ull << 20 && xeStolenGmsBytes(0x0500) == ~0ull &&
			xeStolenGmsBytes(0xff00) == ~0ull, "GMS sizes");
		const uint64_t bar = 0x4000000000ull;
		// 64 MB DSM, 8 MB WOPCM, a 256 MB BAR2: 56 MB at BAR2 + 8 MB
		XeStolenIgfx st = xeStolenDecodeIgfx(0x02c0, 3u << 7, bar, 256ull << 20, 0);
		CHECK(st.size == 56ull << 20 && st.gpuBase == 8ull << 20 && st.ioBase == bar + (8ull << 20) && !st.why &&
			st.gmsBytes == 64ull << 20 && st.wopcmBytes == 8u << 20, "64 MB DSM: %llu MB", (unsigned long long)st.size >> 20);
		st = xeStolenDecodeIgfx(0x0280, 3u << 7, bar, 256ull << 20, 0);
		CHECK(!st.size && st.why, "GGMS 2: no stolen (xe wants 3)");
		st = xeStolenDecodeIgfx(0x05c0, 3u << 7, bar, 256ull << 20, 0);
		CHECK(!st.size && st.why, "GMS 5: invalid");
		st = xeStolenDecodeIgfx(0x02c0, 4u << 7, bar, 256ull << 20, 0);
		CHECK(!st.size && st.why, "WOPCM_SIZE 4: unknown");
		st = xeStolenDecodeIgfx(0x00c0, 3u << 7, bar, 256ull << 20, 0);
		CHECK(!st.size && st.why, "GMS 0: no DSM (xe: the u32 underflow fails the BAR2 check)");
		st = xeStolenDecodeIgfx(0xf1c0, 3u << 7, bar, 256ull << 20, 0);
		CHECK(!st.size && st.why, "8 MB DSM, 8 MB WOPCM: nothing");
		st = xeStolenDecodeIgfx(0x04c0, 3u << 7, bar, 64ull << 20, 0);
		CHECK(!st.size && st.why, "128 MB DSM in a 64 MB BAR2: none");
		st = xeStolenDecodeIgfx(0x02c0, 3u << 7, bar, 64ull << 20, 0);
		CHECK(st.size == 56ull << 20, "56 MB + 8 MB GSM fit a 64 MB BAR2 exactly");
		// Wa_14019821291: the BIOS put GSCPSMI 4 MB below the WOPCM; the low bits are flags
		st = xeStolenDecodeIgfx(0x02c0, 3u << 7, bar, 256ull << 20, bar + (8ull << 20) + (52ull << 20) + 0x3f);
		CHECK(st.size == 52ull << 20 && st.psmiBytes == 4ull << 20, "GSCPSMI in the DSM: %llu MB",
			(unsigned long long)st.size >> 20);
		st = xeStolenDecodeIgfx(0x02c0, 3u << 7, bar, 256ull << 20, bar + (8ull << 20) + (56ull << 20));
		CHECK(st.size == 56ull << 20 && !st.psmiBytes, "GSCPSMI at the WOPCM (outside the usable DSM): no change");
		st = xeStolenDecodeIgfx(0x02c0, 3u << 7, bar, 256ull << 20, bar + (8ull << 20));
		CHECK(!st.size && st.why, "GSCPSMI at the DSM's start: nothing left");
	}

	printf("%s: %d checks, %d failed (%u ids, %u entries)\n", fails ? "FAIL" : "PASS", checks, fails, ids, kXePlatformCount);
	return fails ? 1 : 0;
}
