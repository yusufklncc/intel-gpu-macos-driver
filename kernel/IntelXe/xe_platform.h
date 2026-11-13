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
	// Device workarounds (xe_device_wa_oob.rules), where this driver applies them per platform.
	// Wa_14022085890: SUBPLATFORM(BATTLEMAGE, G21). xe_guc_pc.c pc_needs_min_freq_change raises the SLPC minimum to
	// BMG_MIN_FREQ (1200 MHz) on the primary GT of every BMG (platform == XE_BATTLEMAGE), and with this workaround on the
	// media GT too (IntelXe::mediaPcStart). xePlatformSlpcMin* below.
	bool wa14022085890 {false};
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
