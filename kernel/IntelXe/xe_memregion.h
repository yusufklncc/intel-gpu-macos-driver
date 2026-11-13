// SPDX-License-Identifier: MIT
// Memory regions: where a GPU object's memory comes from, as Linux xe decides it (its TTM placements XE_PL_TT,
// XE_PL_VRAM0 and XE_PL_STOLEN), keyed by the platform descriptor. The first step towards an integrated memory path
// (Lunar Lake: no VRAM, BOs in system memory, the stolen DSM behind BAR2). Today only BMG is started, and for it every
// answer here is the VRAM-or-system choice the code always made. Plain C++ without IOKit types, header only:
// tests/platform_test.cpp checks it on the host. Design: docs/design/DESIGN-lunar-lake.md.
//
// Linux references are to drivers/gpu/drm/xe in dl/linux-xe (abf8c33), checked against v6.18 where the note says so.
#pragma once
#include <stdint.h>
#include "xe_platform.h"

// xe_bo.h XE_PL_*: the region a BO's backing lives in.
enum XeMemRegion : uint8_t {
	XE_MEM_SYSTEM = 0,   // XE_PL_TT: system pages; uAPI DRM_XE_MEM_REGION_CLASS_SYSMEM, instance 0, placement bit 0
	XE_MEM_VRAM0 = 1,    // XE_PL_VRAM0: tile 0's VRAM; uAPI DRM_XE_MEM_REGION_CLASS_VRAM, instance 1, placement bit 1
	XE_MEM_STOLEN = 2,   // XE_PL_STOLEN: the DSM; kernel objects only, never in the uAPI
};

// ─── user BOs (DRM_IOCTL_XE_GEM_CREATE) ──────────────────────────────────────────────────────────────────────────
// The region a user BO is made in first, from its placement bits (already checked against xePlatformMemRegionMask):
// tile 0's VRAM if the placement names it, else system memory. xe_bo.c __xe_bo_placement_for_flags lists the VRAM
// placement before the system one, and TTM validates into the first with room; a full VRAM falls back to system memory
// when the placement allows it (here: GEM_CREATE's spill). On an integrated part the mask has no VRAM bit, so this is
// always system memory.
static inline XeMemRegion xeMemUserRegion(const XePlatformDesc *d, uint32_t placement) {
	return (placement & xePlatformMemRegionMask(d) & (1u << XE_MEM_VRAM0)) ? XE_MEM_VRAM0 : XE_MEM_SYSTEM;
}
// xe_query.c query_mem_regions: system memory (instance 0), then one VRAM region per tile that has one (instance 1).
static inline uint32_t xeMemQueryRegions(const XePlatformDesc *d) { return xePlatformHasVram(d) ? 2u : 1u; }

// ─── the driver's own objects ────────────────────────────────────────────────────────────────────────────────────
// The kernel objects this driver keeps in VRAM on BMG, and the region Linux gives each. On a discrete part all of them
// are VRAM; on an integrated one they are system memory, except the firmware's initial framebuffer, which is stolen.
enum XeKernelObject : uint8_t {
	XE_KOBJ_PAGETABLE,    // xe_pt.c xe_pt_create: XE_BO_FLAG_VRAM_IF_DGFX (here: ptPageAlloc)
	XE_KOBJ_LRC,          // xe_lrc.c xe_lrc_init: XE_BO_FLAG_VRAM_IF_DGFX (here: lrcAlloc, debug.intelxe.lrc_vram)
	XE_KOBJ_GUC_UM,       // xe_guc_ads.c guc_um_init_params' queues: XE_BO_FLAG_VRAM_IF_DGFX (here: gUmDpa)
	XE_KOBJ_DISPLAY,      // display/xe_fb_pin.c, xe_display_bo.c, xe_dsb_buffer.c: XE_BO_FLAG_VRAM_IF_DGFX
	                      // (here: xeDispVramAlloc for IntelXeFB)
	XE_KOBJ_INITIAL_FB,   // display/xe_initial_plane.c initial_plane_bo: the firmware's framebuffer, XE_BO_FLAG_VRAM0
	                      // on a discrete part, XE_BO_FLAG_STOLEN on an integrated one
};
static inline XeMemRegion xeMemKernelRegion(const XePlatformDesc *d, XeKernelObject o) {
	if (xePlatformHasVram(d)) return XE_MEM_VRAM0;
	return o == XE_KOBJ_INITIAL_FB ? XE_MEM_STOLEN : XE_MEM_SYSTEM;
}

// ─── stolen memory of an integrated part from graphics 12.70 (xe_ttm_stolen_mgr.c detect_lmembar_integrated) ─────
// BAR2 (the LMEMBAR) holds the GSM (the GGTT's 8 MB of entries) and then the DSM. stolen_base is 8 MB: from 12.70 a
// GGTT PTE with the DM bit takes the offset from GSMBASE as its address. Inputs are register values, so the decode is
// testable without the GPU:
//   ggc            GGC (0x108040; MMIO, not config space): GGMS 7:6 must be 3 (8 MB of GSM); GMS 15:8 the DSM size
//   stolenReserved STOLEN_RESERVED (0x1082c0, 64 bits as two dwords): WOPCM_SIZE 9:7 (get_wopcm_size)
//   bar2Start/Len  BAR2's physical address and length (pci_resource_start/len(pdev, 2))
//   gscpsmiBase    GSCPSMI_BASE (0x880c on the media GT: 0x38880c) when Wa_14019821291 applies (MEDIA_VERSION_RANGE
//                  (1300, 2000): MTL/ARL and LNL; the BIOS applies it, the driver keeps out of what it reserved), else 0
struct XeStolenIgfx {
	uint64_t size;        // usable DSM bytes; 0 = no stolen memory (why says which check failed)
	uint64_t gpuBase;     // mgr->stolen_base: 8 MB (GSM + 8 MB)
	uint64_t ioBase;      // mgr->io_base: BAR2 + 8 MB, where the CPU reaches it
	uint64_t gmsBytes;    // GGC.GMS decoded
	uint32_t wopcmBytes;  // the WOPCM at the top of the DSM, carved out
	uint64_t psmiBytes;   // Wa_14019821291: bytes below the WOPCM the BIOS reserved for GSC PSMI, carved out
	const char *why;      // nullptr when size > 0
};

// xe_ttm_stolen_mgr.c get_wopcm_size: STOLEN_RESERVED WOPCM_SIZE (9:7) 0-3: 1 << val MB, 5-6: 1 << (val - 1) MB, the
// rest (4, 7) unknown: 0 ("Missing case wopcm_size").
static inline uint32_t xeStolenWopcmBytes(uint64_t stolenReserved) {
	const uint32_t v = (uint32_t)(stolenReserved >> 7) & 7;
	if (v <= 3) return (1u << v) << 20;
	if (v == 5 || v == 6) return (1u << (v - 1)) << 20;
	return 0;
}
// detect_lmembar_integrated's GMS switch: 0x00-0x04 x 32 MB, 0xf0-0xfe (n - 0xf0 + 1) x 4 MB, else invalid (~0).
static inline uint64_t xeStolenGmsBytes(uint32_t ggc) {
	const uint32_t gms = (ggc >> 8) & 0xff;
	if (gms <= 0x04) return (uint64_t)gms * (32ull << 20);
	if (gms >= 0xf0 && gms <= 0xfe) return (uint64_t)(gms - 0xf0 + 1) * (4ull << 20);
	return ~0ull;
}

static inline XeStolenIgfx xeStolenDecodeIgfx(uint32_t ggc, uint64_t stolenReserved, uint64_t bar2Start, uint64_t bar2Len,
		uint64_t gscpsmiBase) {
	XeStolenIgfx s = {};
	s.gpuBase = 8ull << 20;
	s.ioBase = bar2Start + s.gpuBase;
	if (((ggc >> 6) & 3) != 3) {
		s.why = "GGC.GGMS is not 3 (8 MB of GSM)";
		return s;
	}
	s.gmsBytes = xeStolenGmsBytes(ggc);
	if (s.gmsBytes == ~0ull) {
		s.gmsBytes = 0;
		s.why = "GGC.GMS is not a valid size";
		return s;
	}
	s.wopcmBytes = xeStolenWopcmBytes(stolenReserved);
	if (!s.wopcmBytes) {
		s.why = "STOLEN_RESERVED.WOPCM_SIZE is not a known size";
		return s;
	}
	// Linux subtracts in u32 and lets the BAR2 check below catch an underflow (GMS 0, or a DSM smaller than the WOPCM)
	if (s.gmsBytes <= s.wopcmBytes) {
		s.why = "the DSM is no larger than the WOPCM";
		return s;
	}
	uint64_t size = s.gmsBytes - s.wopcmBytes;
	// Wa_14019821291: a GSCPSMI_BASE inside the DSM (bits 5:0 masked) marks the part from there up as reserved
	const uint64_t psmi = gscpsmiBase & ~0x3full;
	if (psmi >= s.ioBase && psmi < s.ioBase + size) {
		s.psmiBytes = s.ioBase + size - psmi;
		size = psmi - s.ioBase;
	}
	if (size + s.gpuBase > bar2Len) {
		s.why = "GSM + DSM do not fit in BAR2";
		return s;
	}
	if (!size) {
		s.why = "nothing left after the reservations";
		return s;
	}
	s.size = size;
	return s;
}
