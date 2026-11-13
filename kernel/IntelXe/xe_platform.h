// SPDX-License-Identifier: MIT
// Platform descriptors: what Linux xe_pci.c keeps per device (struct xe_device_desc, with the xe_graphics_desc and
// xe_media_desc of the IP the platform ships) plus the per-platform values xe picks elsewhere (xe_uc_fw.c firmware,
// xe_wopcm.c, xe_pat.c n_entries, xe_mocs.c uc_index, i915 intel_dmc.c). One entry per device-id list of
// xe_probe_ids.inc (Linux include/drm/intel/pciids.h). Looked up once by PCI device id: IntelXe::start, which starts
// only on an entry marked supported, and XeProbe, which takes any. Plain C++ without IOKit types, so
// tests/platform_test.cpp builds it on the host. Design and the roadmap per family: docs/design/DESIGN-platforms.md.
#pragma once
#include <stdint.h>

// xe_platform_types.h enum xe_platform, same order (graphics version, then chronological)
enum XePlatform : uint8_t {
	XE_PLATFORM_UNINITIALIZED = 0,
	XE_TIGERLAKE,
	XE_ROCKETLAKE,
	XE_ALDERLAKE_S,
	XE_ALDERLAKE_P,
	XE_ALDERLAKE_N,
	XE_DG1,
	XE_DG2,
	XE_PVC,
	XE_METEORLAKE,
	XE_LUNARLAKE,
	XE_BATTLEMAGE,
	XE_PANTHERLAKE,
};

// Where the GPU's energy counters are (Linux xe_hwmon.c xe_hwmon_energy_get, :500-539). Only BMG is implemented here;
// the register path the other dGPUs take (REG_PKG_ENERGY_STATUS: PCU_CR_PACKAGE_ENERGY_STATUS, PVC/CRI registers, unit
// from PKG_POWER_SKU_UNIT) would be a second value.
enum XeEnergySource : uint8_t {
	XE_ENERGY_NONE = 0,   // not read (debug.intelxe.sensors prints no energy)
	XE_ENERGY_PMT,        // XE_BATTLEMAGE: the PUNIT PMT telemetry word (xe_pmt.h), card and package
};

// xe_hw_engine_types.h enum xe_hw_engine_id: bit positions of XePlatformDesc::engineMask
enum : uint32_t {
	XE_HW_ENGINE_RCS0 = 0,
	XE_HW_ENGINE_BCS0 = 1, XE_HW_ENGINE_BCS8 = 9,
	XE_HW_ENGINE_VCS0 = 10,    // VCS0-7: 10-17
	XE_HW_ENGINE_VECS0 = 18,   // VECS0-3: 18-21
	XE_HW_ENGINE_CCS0 = 22,    // CCS0-3: 22-25
	XE_HW_ENGINE_GSCCS0 = 26,
};

struct XePlatformDesc {
	const char *name {nullptr};          // family as XeProbe names it (debug.xeprobe.family): "BMG", "ADL-P", ...
	const char *subplatform {nullptr};   // xe_subplatform_desc name ("G21"), nullptr = none
	XePlatform platform {XE_PLATFORM_UNINITIALIZED};
	const uint16_t *ids {nullptr};       // PCI device ids (vendor 8086)
	uint32_t nIds {0};
	// IP versions (verx100). preGmdid*: xe_device_desc pre_gmdid_*_ip, 0 on platforms with GMD_ID (graphics >= 12.70).
	// graphics/mediaVerx100: the IP this entry describes: pre_gmdid_* where set, else the GMD_ID value Linux's
	// tests/xe_pci.c GMDID_CASE gives the platform (0 = not listed there; read GMD_ID).
	uint16_t preGmdidGraphics {0}, preGmdidMedia {0};
	uint16_t graphicsVerx100 {0}, mediaVerx100 {0};
	// xe_device_desc
	uint8_t dmaMaskBits {0};     // dma_mask_size
	uint8_t vaBits {0};          // va_bits
	uint8_t vmMaxLevel {0};      // vm_max_level (root page-table level)
	uint8_t maxGtPerTile {0};    // 2 = standalone media GT at MEDIA_GT_GSI_OFFSET (media >= 13.00)
	bool isDgfx {false}, hasFlatCcs {false}, hasLlc {false}, hasDisplay {false}, hasSriov {false}, hasPxp {false};
	bool hasCachedPt {false}, hasHeciGscfi {false}, hasHeciCscfi {false}, hasGscNvm {false}, hasFanControl {false};
	bool hasMbxThermalInfo {false}, hasMbxPowerLimits {false}, needsScratch {false}, need64kVram {false};
	bool hasSocRemapperTelem {false};   // has_soc_remapper_telem: the telemetry window's region set via SG_REMAP_INDEX1
	// xe_graphics_desc of the graphics IP
	bool hasUsm {false}, hasAsid {false}, hasAccessCounter {false}, hasAtomicPteBit {false}, hasRangeTlbInval {false};
	bool has64bitTimestamp {false};
	uint64_t engineMask {0};     // graphics hw_engine_mask | media hw_engine_mask (XE_HW_ENGINE_* bits)
	// elsewhere in xe
	uint32_t wopcmBytes {0};     // xe_wopcm.c xe_wopcm_size
	uint8_t patEntries {0};      // xe_pat.c xe_pat_init_early n_entries (valid pat_index values)
	uint8_t mocsUcIndex {0};     // xe_mocs.c get_mocs_settings uc_index (GuC shim MOCS, MEM_SET MOCS)
	XeEnergySource energySource {XE_ENERGY_NONE};   // xe_hwmon.c xe_hwmon_energy_get: platform == XE_BATTLEMAGE -> PMT
	uint8_t energyShift {0};     // xe_hwmon scl_shift_energy: ENERGY_UNIT (14) on mailbox-power-limit platforms
	// linux-firmware paths: xe_uc_fw.c XE_GUC/HUC/GSC_FIRMWARE_DEFS, i915 intel_dmc.c dmc_firmware_default. nullptr: Linux
	// loads none for the platform. gucStem: the <stem> of <stem>_guc_70.bin (XeProbe's firmware directory names).
	const char *gucFw {nullptr}, *gucStem {nullptr}, *hucFw {nullptr}, *gscFw {nullptr}, *dmcFw {nullptr};
	uint16_t gucMajor {0}, gucMinor {0};   // the GuC release XE_GUC_FIRMWARE_DEFS wants (major, minimum minor)
	// The GSC compatibility version XE_GSC_FIRMWARE_DEFS wants (xe_gsc.c query_compatibility_version: project major,
	// compatibility major; "cv104.1.0" on LNL). The file is named by the compatibility major (fw_filename_gsc:
	// xe/lnl_gsc_1.bin), not by the project.
	uint16_t gscMajor {0}, gscMinor {0};
	// Device workarounds (xe_device_wa_oob.rules), where this driver applies them per platform.
	// Wa_14022085890: SUBPLATFORM(BATTLEMAGE, G21). xe_guc_pc.c pc_needs_min_freq_change raises the SLPC minimum to
	// BMG_MIN_FREQ (1200 MHz) on the primary GT of every BMG (platform == XE_BATTLEMAGE), and with this workaround on the
	// media GT too (IntelXe::mediaPcStart). xePlatformSlpcMin* below.
	bool wa14022085890 {false};
	// Wa_15015404425: PLATFORM(LUNARLAKE), PLATFORM(PANTHERLAKE) (PTL: off again from media B0, 15015404425_disable).
	// xe_mmio.c mmio_flush_pending_writes: four dummy writes to 0x130030 before every MMIO read. XeProbe does them.
	bool wa15015404425 {false};
	// this driver
	bool supported {false};        // IntelXe starts on it; every other entry is refused at start with `missing`
	const char *missing {nullptr}; // what the family still needs (the start refusal's log line)
	// An entry that is not supported but that a tester may start anyway: the boot-arg (a flag, without its leading '-')
	// that lets IntelXe start on it, with a warning in the log. nullptr: refused, whatever the boot-args.
	const char *testerBootArg {nullptr};
};

extern const XePlatformDesc kXePlatforms[];
extern const uint32_t kXePlatformCount;

// The entry listing devId, or nullptr (xe_pci.c pciidlist / pci_match_id).
const XePlatformDesc *xePlatformFind(uint16_t devId);
// The first entry of that family name (debug.xeprobe.family), or nullptr.
const XePlatformDesc *xePlatformByName(const char *name);

// Derived values, as xe computes them from the descriptor.
static inline uint64_t xePlatformVmSize(const XePlatformDesc *d) { return 1ull << d->vaBits; }   // xe_vm_create vm->size
static inline uint64_t xePlatformDmaLimit(const XePlatformDesc *d) { return 1ull << d->dmaMaskBits; }
// IOBufferMemoryDescriptor physical mask for memory the GPU reads by device address: dma_mask_size bits, pages
static inline uint64_t xePlatformPhysMask(const XePlatformDesc *d) { return (xePlatformDmaLimit(d) - 1) & ~0xfffull; }
// xe_guc_pc.c pc_needs_min_freq_change: whether pc_adjust_freq_bounds raises the GT's SLPC minimum to BMG_MIN_FREQ.
// Primary GT: Wa_14022085890, or platform BATTLEMAGE (CRESCENTISLAND has no entry here). Media GT: Wa_14022085890 only.
static inline bool xePlatformSlpcMinPrimary(const XePlatformDesc *d) {
	return d->wa14022085890 || d->platform == XE_BATTLEMAGE;
}
static inline bool xePlatformSlpcMinMedia(const XePlatformDesc *d) { return d->wa14022085890; }
// The enabled-engine bits of a class from engineMask (first: XE_HW_ENGINE_* of instance 0, n instances)
static inline uint32_t xePlatformEngines(const XePlatformDesc *d, uint32_t first, uint32_t n) {
	return (uint32_t)(d->engineMask >> first) & ((1u << n) - 1);
}

// ─── memory model: what xe derives from is_dgfx (and has_llc, has_cached_pt, the graphics version) ───────────────
// For the code that differs on an integrated part (Lunar Lake and the other !isDgfx entries). None of those is
// supported yet, so on BMG every answer is the discrete one the code has always assumed. docs/design/DESIGN-lunar-lake.md.

// The lowest graphics version the entry can have: graphicsVerx100, or the family's first for an entry whose IP is read
// from GMD_ID per SKU (ARL: 12.70/12.74, WCL: 30.xx; graphicsVerx100 0). For xe's GRAPHICS_VERx100(xe) >= N checks.
static inline uint16_t xePlatformGraphicsMin(const XePlatformDesc *d) {
	if (d->graphicsVerx100) return d->graphicsVerx100;
	return d->platform == XE_METEORLAKE ? 1270 : d->platform == XE_PANTHERLAKE ? 3000 : 0;
}
// No device memory: every BO is in system memory (xe_tile.c xe_tile_alloc_vram and xe_device.c xe_device_vram_alloc
// make a VRAM region only when IS_DGFX).
static inline bool xePlatformIntegrated(const XePlatformDesc *d) { return !d->isDgfx; }
static inline bool xePlatformHasVram(const XePlatformDesc *d) { return d->isDgfx; }
// has_llc: the GPU shares the CPU's last-level cache (Gen12 integrated). Lunar Lake does not (lnl_desc sets no has_llc):
// CPU-cached memory is coherent only through the PAT index's coherency mode (xe_pat.c coh_mode, XE_COH_1WAY/2WAY).
static inline bool xePlatformHasLlc(const XePlatformDesc *d) { return d->hasLlc; }
// xe->info.mem_region_mask: bit 0 system memory (always: xe_device.c xe_device_probe), bit 1 tile 0's VRAM (xe_tile.c
// xe_tile_init_noalloc, IS_DGFX only). The uAPI's placement bits and the instances of QUERY_MEM_REGIONS.
static inline uint32_t xePlatformMemRegionMask(const XePlatformDesc *d) { return 1u | (d->isDgfx ? 2u : 0u); }
// xe_query.c query_gt_list: near_mem_regions 0x1 (system memory) on an integrated part, tile 0's VRAM bit otherwise;
// far_mem_regions = mem_region_mask ^ near.
static inline uint32_t xePlatformNearMemRegions(const XePlatformDesc *d) { return d->isDgfx ? 2u : 1u; }
static inline uint32_t xePlatformFarMemRegions(const XePlatformDesc *d) {
	return xePlatformMemRegionMask(d) ^ xePlatformNearMemRegions(d);
}
// Stolen memory (DSM): the detect function xe_ttm_stolen_mgr.c xe_ttm_stolen_mgr_init picks.
enum XeStolenModel : uint8_t {
	XE_STOLEN_LMEMBAR_DGFX = 1,   // detect_lmembar_dgfx: DSMBASE in VRAM, through BAR2, up to the tile's end minus WOPCM
	XE_STOLEN_LMEMBAR_IGFX,       // detect_lmembar_integrated (graphics >= 12.70): BAR2 (LMEMBAR) holds GSM (8 MB), then
	                              // DSM; stolen_base 8 MB, size from GGC.GMS minus WOPCM (xe_stolen.h)
	XE_STOLEN_BIOS,               // detect_stolen: intel_graphics_stolen_res (BDSM, reserved by the x86 early quirks)
};
static inline XeStolenModel xePlatformStolen(const XePlatformDesc *d) {
	if (d->isDgfx) return XE_STOLEN_LMEMBAR_DGFX;
	return xePlatformGraphicsMin(d) >= 1270 ? XE_STOLEN_LMEMBAR_IGFX : XE_STOLEN_BIOS;
}
// xe_ttm_stolen_cpu_access_needs_ggtt: before 12.70, the CPU reaches an integrated part's stolen memory only through
// the GGTT aperture (BAR2 is GMADR there), not directly.
static inline bool xePlatformStolenCpuViaGgtt(const XePlatformDesc *d) {
	return xePlatformGraphicsMin(d) < 1270 && !d->isDgfx;
}
// What BAR2 is. On a discrete part it is VRAM (LMEM_BAR, regs/xe_bars.h). On an integrated part from 12.70 it is the
// LMEMBAR, which holds GSM and DSM (detect_lmembar_integrated: DSM at BAR2 + 8 MB); before 12.70 it is the GGTT
// aperture. Code that maps BAR2 as VRAM must check this first.
static inline bool xePlatformBar2IsVram(const XePlatformDesc *d) { return d->isDgfx; }
// xe_ggtt.c xe_ggtt_init_early: the GSM (GGTT page table) size. From graphics 12.50 it is 8 MB, so a 4 GiB GGTT. Before
// that it comes from the GGMS field of the GMCH control word (probe_gsm_size); 0 here means "read GGC".
static inline uint32_t xePlatformGsmBytes(const XePlatformDesc *d) { return xePlatformGraphicsMin(d) >= 1250 ? 8u << 20 : 0; }
// System memory and the CPU caches.
// - Discrete: the GPU's system-memory accesses always snoop (xe_bo.c xe_ttm_tt_create: "DGFX GPU system memory accesses
//   are always coherent with the CPU", every TT page WB).
// - Integrated: only through a coherent PAT index (xe_vm.c refuses a userptr bind with a non-coherent one).
static inline bool xePlatformSysAlwaysCoherent(const XePlatformDesc *d) { return d->isDgfx; }
// xe_ttm_tt_create: on an integrated part without has_cached_pt the GPU's page-table walks do not snoop, so page-table
// BOs are CPU write-combined (Xe_LPG and later: MTL, LNL, PTL).
static inline bool xePlatformPtWriteCombined(const XePlatformDesc *d) { return !d->isDgfx && !d->hasCachedPt; }
// Flat CCS of system-memory BOs (xe_bo.c xe_bo_needs_ccs_pages, xe_migrate.c): kept in extra pages after the BO's own
// (CCS bytes = size / 512), cleared, copied and saved by the migrate engine. This applies to integrated Xe2 and later
// with flat CCS, if the BIOS left it on (xe_device.c probe_has_flat_ccs reads XE2_FLAT_CCS_ENABLE). A discrete Xe2
// part keeps flat CCS in VRAM only.
static inline bool xePlatformSysCcs(const XePlatformDesc *d) {
	return !d->isDgfx && d->hasFlatCcs && xePlatformGraphicsMin(d) >= 2000;
}
// xe_pat.c xe_pat_init_early (graphics 20 and 30): the page-table-access PAT entry (PAT_PTA) is programmed on discrete
// parts only; xe2_pat_ats on both.
static inline bool xePlatformPatPta(const XePlatformDesc *d) { return d->isDgfx; }
// Discrete-only steps of xe: the VF_CAP_REG write after wmb() in xe_device_wmb; the DGFX_PCODE_STATUS and LMEM_INIT
// waits at probe (xe_pcode.c xe_pcode_ready, xe_device.c assert_lmem_ready).
static inline bool xePlatformWmbMmio(const XePlatformDesc *d) { return d->isDgfx; }
static inline bool xePlatformLmemInitWait(const XePlatformDesc *d) { return d->isDgfx; }
