// SPDX-License-Identifier: MIT
// platform_test: offline check of the platform descriptor table (xe_platform.cpp), no GPU needed.
//   1. every id of xe_probe_ids.inc finds exactly one entry, the one that lists it; unknown ids find none
//   2. only the BMG-G21 entry is supported (and IntelXe's IOPCIMatch ids, e20b/e20c/e211/e212, are in it); every other
//      entry carries the reason it is refused; only BMG-G31 has a tester boot-arg (build.py G31_TESTER_MATCH)
//   3. the BMG-G21 values equal the constants the code used before the descriptor (2026-11-10, b17eac9)
//   4. XeProbe's former family table (copied below as it was) gives the same family and values per id and per name
//   5. the BMG ids equal Linux pciids.h INTEL_BMG_G21_IDS / INTEL_BMG_IDS, by subplatform
//   6. Wa_14022085890 (SLPC minimum on the media GT) on BMG-G21 only, BMG_MIN_FREQ on every BMG primary GT
// c++ -std=c++17 -Wall -Wextra -I.. -o platform_test platform_test.cpp ../xe_platform.cpp && ./platform_test
#include <stdio.h>
#include <string.h>
#include "xe_platform.h"
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

	printf("%s: %d checks, %d failed (%u ids, %u entries)\n", fails ? "FAIL" : "PASS", checks, fails, ids, kXePlatformCount);
	return fails ? 1 : 0;
}
