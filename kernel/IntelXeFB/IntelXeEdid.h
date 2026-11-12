// SPDX-License-Identifier: MIT
// Contains material from Linux drivers/gpu/drm (drm_edid.c, drm_modes.c, drm_displayid.c, i915 intel_dp.c), MIT/X11
// licences, Copyright Luc Verhaegen, Intel Corporation, Red Hat, The XFree86 Project, Dave Airlie and others: the full
// notices are in kernel/IntelXeFB/LICENSE.linux-drm.
// IntelXeEdid: the monitor's mode list from its EDID, as Linux builds it (v6.17 drivers/gpu/drm/drm_edid.c
// _drm_edid_connector_add_modes), and the DP mode checks of i915 (intel_display.c intel_mode_valid,
// intel_cpu_transcoder_mode_valid, intel_mode_valid_max_plane_size; intel_dp.c intel_dp_mode_valid). Plain C++ with
// no allocation and no kernel or libc calls, so the same file builds into IntelXeFB.kext and into the host unit test
// (tests/edid). Line numbers in the comments are of the Linux v6.17 files.
//
// Not ported: the EDID quirk table (drm_edid.c edid_quirk_list, keyed by monitor vendor/product), HDMI 3D/stereo modes
// (never exposed to a client without the stereo cap, drm_connector.c drm_mode_expose_to_userspace), the HDMI Forum
// EEODB block count override, VTB extension blocks' detailed timings (drm_for_each_detailed_block does visit them;
// VTB-EXT is extinct and is skipped here), DSC and joiner (this driver runs neither).
#ifndef INTELXEEDID_H
#define INTELXEEDID_H

#include <stdint.h>   // the MacKernelSDK has one for the kext build

namespace intelxeedid {

// include/uapi/drm/drm_mode.h
enum : uint32_t {
	kPHSync = 1u << 0, kNHSync = 1u << 1, kPVSync = 1u << 2, kNVSync = 1u << 3, kInterlace = 1u << 4, kDblScan = 1u << 5,
	kCSync = 1u << 6, kPCSync = 1u << 7, kNCSync = 1u << 8, kHSkew = 1u << 9, kBcast = 1u << 10, kPixMux = 1u << 11,
	kDblClk = 1u << 12, kClkDiv2 = 1u << 13, k3DMask = 0x1Fu << 14,
};
enum : uint8_t { kTypePreferred = 1u << 3, kTypeDriver = 1u << 6 };

// where a mode came from (for logs and the unit test; Linux keeps no such thing)
enum Source : uint8_t {
	kSrcDetailed = 1,   // 18-byte detailed timing descriptor (base block, CTA extension)
	kSrcCvt3,           // CVT 3-byte code descriptor (0xF8)
	kSrcStandard,       // standard timing (base block, 0xFA descriptor)
	kSrcEstablished,    // established timings I/II, established timings III (0xF7)
	kSrcCtaVdb,         // CTA-861 video data block VIC
	kSrcHdmiVsdb,       // HDMI VSDB HDMI_VIC
	kSrcCtaY420,        // CTA-861 YCbCr 4:2:0 video data block (4:2:0-only modes)
	kSrcCtaAlternate,   // 59.94/60 Hz twin of a CTA mode (add_alternate_cea_modes)
	kSrcDisplayId,      // DisplayID type I (1.x) / type VII (2.x) detailed timing
	kSrcDisplayIdFormula, // DisplayID 2.x type IX/X formula timing
	kSrcInferred,       // range-limits inferred (DMT/GTF/GTF2/CVT within the monitor range)
};
const char *sourceName(uint8_t src);

struct Mode {
	int clock;   // kHz
	int hdisplay, hsync_start, hsync_end, htotal, hskew;
	int vdisplay, vsync_start, vsync_end, vtotal, vscan;
	uint32_t flags;
	uint8_t type, aspect, source;
	bool y420only;   // its VIC is in a YCbCr 4:2:0 VDB (drm_mode_is_420_only)
};

// the generated tables' entry types (IntelXeEdidTables.h)
struct TableMode { int clock, hd, hss, hse, ht, hsk, vd, vss, vse, vt, vs; uint32_t flags; uint8_t aspect; };
struct MiniMode { short w, h, r, rb; };

enum { kMaxModes = 160 };

struct Result {
	Mode modes[kMaxModes];
	int count;
	int overflow;            // modes that did not fit
	int revision;            // EDID 1.x revision
	bool digital, continuous;
	int minVfreq, maxVfreq;  // drm_get_monitor_range (0 when the EDID gives none)
	int blocks;              // valid blocks used (base + extensions)
	int badBlocks;           // extension blocks dropped as invalid (edid_block_valid)
	bool hasCta, hasDisplayId;
	// the "range limits only" descriptor of an EDID 1.4 continuous-frequency sink, which drm_get_monitor_range trusts as
	// "any and all timings inside are accepted" (drm_edid.c:6483), for modeInRangeLimits
	bool hasRangeOnly, supportsRb;
	uint8_t rangeDesc[18];
};

// The whole EDID (base block first). False when the base block is unusable (header/checksum/version, as
// edid_block_check). Extension blocks that fail edid_block_valid are skipped, as edid_filter_invalid_blocks.
bool parse(const uint8_t *edid, uint32_t len, Result *r);
// drm_edid.c:3653 mode_in_range against the range-limits-only descriptor (false without one)
bool modeInRangeLimits(const Result &r, const Mode &m);

// drm_modes.c:623 drm_cvt_mode, progressive, no margins (the second pipe's dry run: "WxH@Hz" is a CVT timing)
bool cvt(int hdisplay, int vdisplay, int vrefresh, bool reduced, Mode *out);
int vrefresh(const Mode &m);                 // drm_modes.c:1287 drm_mode_vrefresh (rounded Hz)
uint32_t refreshMilliHz(const Mode &m);      // the same in milli-Hz (for display)
bool sameTimings(const Mode &a, const Mode &b);   // drm_mode_match TIMINGS | CLOCK | FLAGS | 3D_FLAGS
void sort(Mode *m, int n);                   // drm_modes.c:1841 drm_mode_compare, stable (list_sort)

// drm_mode_status values used here (include/drm/drm_modes.h)
enum Status {
	kOk = 0, kHSyncBad, kVSyncBad, kHIllegal, kVIllegal, kBad, kNoInterlace, kNoDblScan, kNoVScan, kClockHigh, kClockLow,
	kNoStereo, kNo420, kDuplicate,
};
const char *statusName(int s);

struct Sink {
	int maxDotclk;     // kHz: intel_cdclk.c:3401 intel_compute_max_dotclk, one pipe (no joiner)
	int linkRate;      // kHz link symbol clock as i915 counts it (162000/270000/540000/810000)
	int lanes;         // 1, 2, 4
	int bpp;           // output bpp the link is checked at
	bool interlace;    // connector->interlace_allowed (intel_dp.c:6714: false on display 12+)
	bool ycbcr420;     // the pipe can send 4:2:0
	int displayVer;    // DISPLAY_VER (14 for BMG)
};
// drm_probe_helper.c:444 __drm_helper_update_and_validate order: intel_mode_valid (driver), the connector flags
// (interlace, doublescan, stereo), 4:2:0, then intel_dp_mode_valid.
int modeValid(const Mode &m, const Sink &s);

// ---- IntelXeFB's own mode table (not Linux) --------------------------------------------------------------------------
// The pipe keeps the firmware's raster and link: a size is the plane scaled by the pipe scaler, a refresh is the
// firmware's timing with a longer vertical total (LRR). That is i915's fixed-panel model (intel_panel.c:200
// intel_panel_compute_config: any mode is run on the panel's fixed timing; with VRR the vtotal is stretched to the
// requested refresh, else the refresh must be within 1 Hz of a fixed mode), with the EDID's modes as the requests.
struct Panel {
	uint32_t width, height, htotal, vtotal;   // the firmware's timing on the pipe
	uint32_t hsyncStart, hsyncEnd, vsyncStart, vsyncEnd;
	uint64_t pclkHz;
	bool vrr;                                 // intel_vrr_is_capable
	int vrrMin, vrrMax;                       // monitor range (drm_get_monitor_range)
	const Result *edid;                       // for the range limits and the EDID's own timings (may be null)
	bool scaling, rates, commonRates;         // -intelxefb_noscale, -intelxefb_norates, -intelxefb_edidonly
	uint32_t maxVtotal;                       // transcoder limit (8192)
};
enum Why : uint8_t { kWhyFirmware = 1, kWhyEdid, kWhyPanel, kWhyVrrMin, kWhyCommon };
const char *whyName(uint8_t w);
// milliHz: as run (vt); pclkKhz: 0 = the firmware's pixel clock, else an EDID timing's higher one on the same raster
struct DriverMode { uint16_t w, h; uint32_t vt, milliHz, nominalMilliHz; uint8_t why; bool stretched; uint32_t pclkKhz; };
enum { kMaxDriverModes = 256, kMaxSizes = 24, kMaxRatesPerSize = 16 };
// valid: the validated, deduplicated, sorted EDID modes (any may be passed; sizes above the panel's are skipped)
int buildDriverModes(const Mode *valid, int n, const Panel &p, DriverMode *out, int max);
// the fallback without a usable EDID: the panel's size and 2048x1152, 1920x1080, 1600x900, 1280x720 at the firmware's
// refresh and 144, 120, 100, 60 Hz (IntelXeFB v10l's table)
int fallbackDriverModes(const Panel &p, DriverMode *out, int max);

}  // namespace intelxeedid

#endif
