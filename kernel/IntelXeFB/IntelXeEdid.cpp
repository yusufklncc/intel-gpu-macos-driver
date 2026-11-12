// SPDX-License-Identifier: MIT
// Contains material from Linux drivers/gpu/drm (drm_edid.c, drm_modes.c, drm_displayid.c, i915 intel_dp.c), MIT/X11
// licences, Copyright Luc Verhaegen, Intel Corporation, Red Hat, The XFree86 Project, Dave Airlie and others: the full
// notices are in kernel/IntelXeFB/LICENSE.linux-drm.
// IntelXeEdid: Linux v6.17 EDID mode parsing (drivers/gpu/drm/drm_edid.c, drm_displayid.c, drm_modes.c) and the i915 DP
// mode checks, ported for IntelXeFB. Each function names the Linux function and file:line it follows. See IntelXeEdid.h.
#include "IntelXeEdid.h"

namespace intelxeedid {

#include "IntelXeEdidTables.h"

namespace {

constexpr int kEdidLength = 128;
constexpr uint8_t kCeaExt = 0x02, kDisplayIdExt = 0x70;
// drm_edid.h:180-188 display descriptor tags
constexpr uint8_t kDetailEstTimings = 0xF7, kDetailCvt3Byte = 0xF8, kDetailStdModes = 0xFA, kDetailMonitorRange = 0xFD;
// drm_edid.h:68-72 detailed pixel timing misc bits
constexpr uint8_t kPtHsyncPositive = 1 << 1, kPtVsyncPositive = 1 << 2, kPtStereo = 1 << 5, kPtInterlaced = 1 << 7;
// drm_edid.h:104-110 range descriptor flags
constexpr uint8_t kDefaultGtf = 0x00, kRangeLimitsOnly = 0x01, kSecondaryGtf = 0x02, kCvtSupport = 0x04;
constexpr uint8_t kCvtFlagsReducedBlanking = 1 << 4;
// drm_edid.h:223-225 features byte (base block offset 24)
constexpr uint8_t kFeatureContinuousFreq = 1 << 0, kFeaturePreferredTiming = 1 << 1;
// drm_displayid_internal.h:40-63
constexpr uint8_t kDidType1Detailed = 0x03, kDidCta = 0x81, kDid2Type7Detailed = 0x22, kDid2Type9Formula = 0x24,
                  kDid2Type10Formula = 0x2A;
// drm_edid.c:4151-4164 CTA tag codes
constexpr int kCtaDbVideo = 2, kCtaDbVendor = 3, kCtaDbExtendedTag = 7, kCtaExtDb420VideoData = 14;
constexpr int kHdmiIeeeOui = 0x000C03;
enum { kLevelDmt, kLevelGtf, kLevelGtf2, kLevelCvt };

int iabs(int v) { return v < 0 ? -v : v; }
int divRoundClosest(long long n, long long d) { return (int)((n + d / 2) / d); }
// KHZ2PICOS (include/linux/fb.h): 1000000000UL / khz
unsigned long khz2picos(int khz) { return khz ? 1000000000UL / (unsigned long)khz : 0; }
uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | p[1] << 8); }

Mode fromTable(const TableMode &t)
{
	Mode m = {};
	m.clock = t.clock; m.hdisplay = t.hd; m.hsync_start = t.hss; m.hsync_end = t.hse; m.htotal = t.ht; m.hskew = t.hsk;
	m.vdisplay = t.vd; m.vsync_start = t.vss; m.vsync_end = t.vse; m.vtotal = t.vt; m.vscan = t.vs;
	m.flags = t.flags; m.aspect = t.aspect; m.type = kTypeDriver;
	return m;
}

// The EDID as Linux holds it after edid_filter_invalid_blocks: the base block and the valid extension blocks.
struct Edid {
	const uint8_t *base;
	const uint8_t *ext[255];
	int extCount;
	int revision() const { return base[19]; }
	uint8_t features() const { return base[24]; }
	bool digital() const { return base[20] & 0x80; }   // drm_edid_is_digital (input bit 7)
};

// The probed list (connector->probed_modes)
struct Probed {
	Result *r;
	void add(const Mode &m, uint8_t src) {
		if (r->count >= kMaxModes) { r->overflow++; return; }
		r->modes[r->count] = m;
		r->modes[r->count].source = src;
		r->count++;
	}
};

// ---- descriptors (drm_edid.c:3070-3145) ---------------------------------------------------------------------------
bool isDisplayDescriptor(const uint8_t *d, uint8_t type) { return d[0] == 0 && d[1] == 0 && d[2] == 0 && d[3] == type; }
bool isDetailedTimingDescriptor(const uint8_t *d) { return (d[0] | d[1] << 8) != 0; }

template <typename F> void ceaForEachDetailedBlock(const uint8_t *ext, F cb)   // drm_edid.c:3090
{
	const int d = ext[2];
	if (d < 4 || d > 127) return;
	const int n = (127 - d) / 18;
	for (int i = 0; i < n; i++) cb(ext + d + 18 * i);
}
template <typename F> void forEachDetailedBlock(const Edid &e, F cb)   // drm_edid.c:3118 drm_for_each_detailed_block
{
	for (int i = 0; i < 4; i++) cb(e.base + 54 + 18 * i);
	for (int i = 0; i < e.extCount; i++)
		if (e.ext[i][0] == kCeaExt) ceaForEachDetailedBlock(e.ext[i], cb);
}

// drm_edid.c:3148 is_rb / 3163 drm_monitor_supports_rb
bool monitorSupportsRb(const Edid &e)
{
	if (e.revision() >= 4) {
		bool ret = false;
		forEachDetailedBlock(e, [&](const uint8_t *d) {
			if (isDisplayDescriptor(d, kDetailMonitorRange) && d[10] == kCvtSupport && (d[15] & kCvtFlagsReducedBlanking)) ret = true;
		});
		return ret;
	}
	return e.digital();
}

// drm_edid.c:3178 find_gtf2 and the drm_gtf2_* readers
const uint8_t *findGtf2(const Edid &e)
{
	const uint8_t *res = nullptr;
	forEachDetailedBlock(e, [&](const uint8_t *d) { if (isDisplayDescriptor(d, kDetailMonitorRange) && d[10] == kSecondaryGtf) res = d; });
	return res;
}
int gtf2Hbreak(const Edid &e) { const uint8_t *d = findGtf2(e); return d ? d[12] * 2 : 0; }
int gtf2C2(const Edid &e) { const uint8_t *d = findGtf2(e); return d ? d[13] : 0; }
int gtf2M(const Edid &e) { const uint8_t *d = findGtf2(e); return d ? le16(d + 14) : 0; }
int gtf2K(const Edid &e) { const uint8_t *d = findGtf2(e); return d ? d[16] : 0; }
int gtf2J2(const Edid &e) { const uint8_t *d = findGtf2(e); return d ? d[17] : 0; }

// drm_edid.c:3253 get_timing_level / 3278 standard_timing_level
int standardTimingLevel(const Edid &e)
{
	if (e.revision() >= 4) {
		int ret = kLevelCvt;
		forEachDetailedBlock(e, [&](const uint8_t *d) {
			if (!isDisplayDescriptor(d, kDetailMonitorRange)) return;
			switch (d[10]) {
			case kDefaultGtf: ret = kLevelGtf; break;
			case kSecondaryGtf: ret = kLevelGtf2; break;
			case kCvtSupport: ret = kLevelCvt; break;
			default: break;
			}
		});
		return ret;
	} else if (e.revision() >= 3 && gtf2Hbreak(e)) {
		return kLevelGtf2;
	} else if (e.revision() >= 2) {
		return kLevelGtf;
	}
	return kLevelDmt;
}

// ---- formula modes (drm_modes.c) -----------------------------------------------------------------------------------
// drm_modes.c:623 drm_cvt_mode
bool cvtMode(int hdisplay, int vdisplay, int vrefresh, bool reduced, bool interlaced, bool margins, Mode *out)
{
	const int HV_FACTOR = 1000, CVT_MARGIN_PERCENTAGE = 18, CVT_H_GRANULARITY = 8, CVT_MIN_V_PORCH = 3, CVT_MIN_V_BPORCH = 6,
	          CVT_CLOCK_STEP = 250;
	Mode m = {};
	unsigned int vfieldrate, hperiod;
	int hdisplay_rnd, hmargin, vdisplay_rnd, vmargin, vsync, interlace;
	unsigned long long tmp;
	if (!hdisplay || !vdisplay) return false;
	if (!vrefresh) vrefresh = 60;
	vfieldrate = interlaced ? vrefresh * 2 : vrefresh;
	hdisplay_rnd = hdisplay - (hdisplay % CVT_H_GRANULARITY);
	hmargin = 0;
	if (margins) { hmargin = hdisplay_rnd * CVT_MARGIN_PERCENTAGE / 1000; hmargin -= hmargin % CVT_H_GRANULARITY; }
	m.hdisplay = hdisplay_rnd + 2 * hmargin;
	vdisplay_rnd = interlaced ? vdisplay / 2 : vdisplay;
	vmargin = 0;
	if (margins) vmargin = vdisplay_rnd * CVT_MARGIN_PERCENTAGE / 1000;
	m.vdisplay = vdisplay + 2 * vmargin;
	interlace = interlaced ? 1 : 0;
	if (!(vdisplay % 3) && ((vdisplay * 4 / 3) == hdisplay)) vsync = 4;
	else if (!(vdisplay % 9) && ((vdisplay * 16 / 9) == hdisplay)) vsync = 5;
	else if (!(vdisplay % 10) && ((vdisplay * 16 / 10) == hdisplay)) vsync = 6;
	else if (!(vdisplay % 4) && ((vdisplay * 5 / 4) == hdisplay)) vsync = 7;
	else if (!(vdisplay % 9) && ((vdisplay * 15 / 9) == hdisplay)) vsync = 7;
	else vsync = 10;
	if (!reduced) {
		const int CVT_MIN_VSYNC_BP = 550, CVT_HSYNC_PERCENTAGE = 8, CVT_M_FACTOR = 600, CVT_C_FACTOR = 40, CVT_K_FACTOR = 128,
		          CVT_J_FACTOR = 20;
		const int CVT_M_PRIME = CVT_M_FACTOR * CVT_K_FACTOR / 256;
		const int CVT_C_PRIME = (CVT_C_FACTOR - CVT_J_FACTOR) * CVT_K_FACTOR / 256 + CVT_J_FACTOR;
		int tmp1, tmp2, vsyncandback_porch, hblank;
		unsigned int hblank_percentage;
		tmp1 = HV_FACTOR * 1000000 - CVT_MIN_VSYNC_BP * HV_FACTOR * (int)vfieldrate;
		tmp2 = (vdisplay_rnd + 2 * vmargin + CVT_MIN_V_PORCH) * 2 + interlace;
		hperiod = (unsigned int)(tmp1 * 2 / (tmp2 * (int)vfieldrate));
		tmp1 = CVT_MIN_VSYNC_BP * HV_FACTOR / (int)hperiod + 1;
		if (tmp1 < (vsync + CVT_MIN_V_PORCH)) vsyncandback_porch = vsync + CVT_MIN_V_PORCH;
		else vsyncandback_porch = tmp1;
		m.vtotal = vdisplay_rnd + 2 * vmargin + vsyncandback_porch + CVT_MIN_V_PORCH;
		hblank_percentage = (unsigned int)(CVT_C_PRIME * HV_FACTOR - CVT_M_PRIME * (int)hperiod / 1000);
		if (hblank_percentage < 20u * HV_FACTOR) hblank_percentage = 20 * HV_FACTOR;
		hblank = (int)((unsigned int)m.hdisplay * hblank_percentage / (100 * HV_FACTOR - hblank_percentage));
		hblank -= hblank % (2 * CVT_H_GRANULARITY);
		m.htotal = m.hdisplay + hblank;
		m.hsync_end = m.hdisplay + hblank / 2;
		m.hsync_start = m.hsync_end - (m.htotal * CVT_HSYNC_PERCENTAGE) / 100;
		m.hsync_start += CVT_H_GRANULARITY - m.hsync_start % CVT_H_GRANULARITY;
		m.vsync_start = m.vdisplay + CVT_MIN_V_PORCH;
		m.vsync_end = m.vsync_start + vsync;
	} else {
		const int CVT_RB_MIN_VBLANK = 460, CVT_RB_H_SYNC = 32, CVT_RB_H_BLANK = 160, CVT_RB_VFPORCH = 3;
		int vbilines, tmp1, tmp2;
		tmp1 = HV_FACTOR * 1000000 - CVT_RB_MIN_VBLANK * HV_FACTOR * (int)vfieldrate;
		tmp2 = vdisplay_rnd + 2 * vmargin;
		hperiod = (unsigned int)(tmp1 / (tmp2 * (int)vfieldrate));
		vbilines = CVT_RB_MIN_VBLANK * HV_FACTOR / (int)hperiod + 1;
		if (vbilines < (CVT_RB_VFPORCH + vsync + CVT_MIN_V_BPORCH)) vbilines = CVT_RB_VFPORCH + vsync + CVT_MIN_V_BPORCH;
		m.vtotal = vdisplay_rnd + 2 * vmargin + vbilines;
		m.htotal = m.hdisplay + CVT_RB_H_BLANK;
		m.hsync_end = m.hdisplay + CVT_RB_H_BLANK / 2;
		m.hsync_start = m.hsync_end - CVT_RB_H_SYNC;
		m.vsync_start = m.vdisplay + CVT_RB_VFPORCH;
		m.vsync_end = m.vsync_start + vsync;
	}
	tmp = (unsigned long long)m.htotal;
	tmp *= HV_FACTOR * 1000;
	tmp /= hperiod;
	tmp -= m.clock % CVT_CLOCK_STEP;   // as Linux: clock is still 0 here, so nothing is taken off
	m.clock = (int)tmp;
	if (interlaced) { m.vtotal *= 2; m.flags |= kInterlace; }
	m.flags |= reduced ? (kPHSync | kNVSync) : (kPVSync | kNHSync);
	m.type = kTypeDriver;
	*out = m;
	return true;
}

// drm_modes.c:849 drm_gtf_mode_complex
bool gtfModeComplex(int hdisplay, int vdisplay, int vrefresh, bool interlaced, int margins, int GTF_M, int GTF_2C, int GTF_K,
                    int GTF_2J, Mode *out)
{
	const int GTF_MARGIN_PERCENTAGE = 18, GTF_CELL_GRAN = 8, GTF_MIN_V_PORCH = 1, V_SYNC_RQD = 3, H_SYNC_PERCENT = 8,
	          MIN_VSYNC_PLUS_BP = 550;
	const int GTF_C_PRIME = (((GTF_2C - GTF_2J) * GTF_K / 256) + GTF_2J) / 2;
	const int GTF_M_PRIME = GTF_K * GTF_M / 256;
	Mode m = {};
	unsigned int hdisplay_rnd, vdisplay_rnd, vfieldrate_rqd;
	int top_margin, bottom_margin, interlace;
	unsigned int hfreq_est;
	int vsync_plus_bp;
	unsigned int vtotal_lines;
	int left_margin, right_margin;
	unsigned int total_active_pixels, ideal_duty_cycle, hblank, total_pixels, pixel_freq;
	int hsync, hfront_porch, vodd_front_porch_lines;
	unsigned int tmp1, tmp2;
	if (!hdisplay || !vdisplay) return false;
	hdisplay_rnd = (unsigned int)(hdisplay + GTF_CELL_GRAN / 2) / GTF_CELL_GRAN;
	hdisplay_rnd = hdisplay_rnd * GTF_CELL_GRAN;
	vdisplay_rnd = interlaced ? (unsigned int)vdisplay / 2 : (unsigned int)vdisplay;
	vfieldrate_rqd = interlaced ? (unsigned int)vrefresh * 2 : (unsigned int)vrefresh;
	top_margin = 0;
	if (margins) top_margin = (int)((vdisplay_rnd * GTF_MARGIN_PERCENTAGE + 500) / 1000);
	bottom_margin = top_margin;
	interlace = interlaced ? 1 : 0;
	tmp1 = (1000000 - MIN_VSYNC_PLUS_BP * vfieldrate_rqd) / 500;
	tmp2 = (vdisplay_rnd + 2 * top_margin + GTF_MIN_V_PORCH) * 2 + interlace;
	hfreq_est = (tmp2 * 1000 * vfieldrate_rqd) / tmp1;
	vsync_plus_bp = (int)(MIN_VSYNC_PLUS_BP * hfreq_est / 1000);
	vsync_plus_bp = (vsync_plus_bp + 500) / 1000;
	vtotal_lines = vdisplay_rnd + top_margin + bottom_margin + vsync_plus_bp + GTF_MIN_V_PORCH;
	if (margins) left_margin = (int)((hdisplay_rnd * GTF_MARGIN_PERCENTAGE + 500) / 1000);
	else left_margin = 0;
	right_margin = left_margin;
	total_active_pixels = hdisplay_rnd + left_margin + right_margin;
	ideal_duty_cycle = GTF_C_PRIME * 1000 - (GTF_M_PRIME * 1000000 / hfreq_est);
	hblank = total_active_pixels * ideal_duty_cycle / (100000 - ideal_duty_cycle);
	hblank = (hblank + GTF_CELL_GRAN) / (2 * GTF_CELL_GRAN);
	hblank = hblank * 2 * GTF_CELL_GRAN;
	total_pixels = total_active_pixels + hblank;
	pixel_freq = total_pixels * hfreq_est / 1000;
	hsync = (int)(H_SYNC_PERCENT * total_pixels / 100);
	hsync = (hsync + GTF_CELL_GRAN / 2) / GTF_CELL_GRAN;
	hsync = hsync * GTF_CELL_GRAN;
	hfront_porch = (int)(hblank / 2) - hsync;
	vodd_front_porch_lines = GTF_MIN_V_PORCH;
	m.hdisplay = (int)hdisplay_rnd;
	m.hsync_start = (int)hdisplay_rnd + hfront_porch;
	m.hsync_end = m.hsync_start + hsync;
	m.htotal = (int)total_pixels;
	m.vdisplay = (int)vdisplay_rnd;
	m.vsync_start = (int)vdisplay_rnd + vodd_front_porch_lines;
	m.vsync_end = m.vsync_start + V_SYNC_RQD;
	m.vtotal = (int)vtotal_lines;
	m.clock = (int)pixel_freq;
	if (interlaced) { m.vtotal *= 2; m.flags |= kInterlace; }
	if (GTF_M == 600 && GTF_2C == 80 && GTF_K == 128 && GTF_2J == 40) m.flags = kNHSync | kPVSync;
	else m.flags = kPHSync | kNVSync;
	m.type = kTypeDriver;
	*out = m;
	return true;
}
// drm_modes.c:1054 drm_gtf_mode
bool gtfMode(int h, int v, int r, bool interlaced, int margins, Mode *out) { return gtfModeComplex(h, v, r, interlaced, margins, 600, 80, 128, 40, out); }

// drm_edid.c:3313 drm_mode_hsync
int modeHsync(const Mode &m) { return m.htotal <= 0 ? 0 : divRoundClosest(m.clock, m.htotal); }

// drm_edid.c:3322 drm_gtf2_mode
bool gtf2Mode(const Edid &e, int h, int v, int r, Mode *out)
{
	if (!gtfMode(h, v, r, false, 0, out)) return false;
	if (modeHsync(*out) > gtf2Hbreak(e))
		return gtfModeComplex(h, v, r, false, 0, gtf2M(e), gtf2C2(e), gtf2K(e), gtf2J2(e), out);
	return true;
}

// drm_edid.c:3025 mode_is_rb / 3045 drm_mode_find_dmt
bool modeIsRb(const Mode &m)
{
	return (m.htotal - m.hdisplay == 160) && (m.hsync_end - m.hdisplay == 80) && (m.hsync_end - m.hsync_start == 32) &&
	       (m.vsync_start - m.vdisplay == 3);
}
bool findDmt(int hsize, int vsize, int fresh, bool rb, Mode *out)
{
	for (const TableMode &t : drm_dmt_modes) {
		Mode m = fromTable(t);
		if (hsize != m.hdisplay || vsize != m.vdisplay || fresh != vrefresh(m) || rb != modeIsRb(m)) continue;
		*out = m;
		return true;
	}
	return false;
}

// ---- base block modes ----------------------------------------------------------------------------------------------
// drm_edid.c:3354 drm_mode_std
bool modeStd(const Probed &p, const Edid &e, const uint8_t *t, Mode *out)
{
	const unsigned aspect_ratio = (t[1] & 0xC0) >> 6;   // EDID_TIMING_ASPECT_MASK/SHIFT
	const unsigned vfreq = t[1] & 0x3F;                 // EDID_TIMING_VFREQ_MASK/SHIFT
	const int timing_level = standardTimingLevel(e);
	int hsize, vsize, vrefresh_rate;
	// drm_edid.c:3306 bad_std_timing
	if ((t[0] == 0x00 && t[1] == 0x00) || (t[0] == 0x01 && t[1] == 0x01) || (t[0] == 0x20 && t[1] == 0x20)) return false;
	hsize = t[0] * 8 + 248;
	vrefresh_rate = (int)vfreq + 60;
	if (aspect_ratio == 0) vsize = e.revision() < 3 ? hsize : (hsize * 10) / 16;
	else if (aspect_ratio == 1) vsize = (hsize * 3) / 4;
	else if (aspect_ratio == 2) vsize = (hsize * 4) / 5;
	else vsize = (hsize * 9) / 16;
	if (vrefresh_rate == 60 && ((hsize == 1360 && vsize == 765) || (hsize == 1368 && vsize == 769))) { hsize = 1366; vsize = 768; }
	for (int i = 0; i < p.r->count; i++) {
		const Mode &m = p.r->modes[i];
		if (m.hdisplay == hsize && m.vdisplay == vsize && vrefresh(m) == vrefresh_rate) return false;
	}
	if (hsize == 1366 && vsize == 768 && vrefresh_rate == 60) {
		if (!cvtMode(1366, 768, vrefresh_rate, false, false, false, out)) return false;
		out->hdisplay = 1366; out->hsync_start -= 1; out->hsync_end -= 1;
		return true;
	}
	if (monitorSupportsRb(e) && findDmt(hsize, vsize, vrefresh_rate, true, out)) return true;
	if (findDmt(hsize, vsize, vrefresh_rate, false, out)) return true;
	switch (timing_level) {
	case kLevelDmt: return false;
	case kLevelGtf: return gtfMode(hsize, vsize, vrefresh_rate, false, 0, out);
	case kLevelGtf2: return gtf2Mode(e, hsize, vsize, vrefresh_rate, out);
	case kLevelCvt: return cvtMode(hsize, vsize, vrefresh_rate, false, false, false, out);
	}
	return false;
}

// drm_edid.c:3457 drm_mode_do_interlace_quirk
void doInterlaceQuirk(Mode *m, const uint8_t *d)   // d: the 18-byte descriptor, misc at byte 17
{
	static const struct { int w, h; } cea_interlaced[] = {
		{ 1920, 1080 }, { 720, 480 }, { 1440, 480 }, { 2880, 480 }, { 720, 576 }, { 1440, 576 }, { 2880, 576 },
	};
	if (!(d[17] & kPtInterlaced)) return;
	for (const auto &c : cea_interlaced)
		if (m->hdisplay == c.w && m->vdisplay == c.h / 2) {
			m->vdisplay *= 2; m->vsync_start *= 2; m->vsync_end *= 2; m->vtotal *= 2; m->vtotal |= 1;
		}
	m->flags |= kInterlace;
}

// drm_edid.c:3495 drm_mode_detailed (the quirk paths -- FORCE_REDUCED_BLANKING, 135_CLOCK_TOO_HIGH, DETAILED_SYNC_PP --
// are keyed to listed monitors and not taken)
bool modeDetailed(const uint8_t *d, Mode *out)
{
	const uint8_t *pt = d + 2;   // struct detailed_pixel_timing
	const unsigned hactive = (pt[2] & 0xF0) << 4 | pt[0];
	const unsigned vactive = (pt[5] & 0xF0) << 4 | pt[3];
	const unsigned hblank = (pt[2] & 0x0F) << 8 | pt[1];
	const unsigned vblank = (pt[5] & 0x0F) << 8 | pt[4];
	const unsigned hsync_offset = (pt[9] & 0xC0) << 2 | pt[6];
	const unsigned hsync_pulse_width = (pt[9] & 0x30) << 4 | pt[7];
	const unsigned vsync_offset = (pt[9] & 0x0C) << 2 | pt[8] >> 4;
	const unsigned vsync_pulse_width = (pt[9] & 0x03) << 4 | (pt[8] & 0x0F);
	if (hactive < 64 || vactive < 64) return false;
	if (pt[15] & kPtStereo) return false;
	if (!hsync_pulse_width || !vsync_pulse_width) return false;
	Mode m = {};
	m.clock = le16(d) * 10;
	m.hdisplay = (int)hactive;
	m.hsync_start = m.hdisplay + (int)hsync_offset;
	m.hsync_end = m.hsync_start + (int)hsync_pulse_width;
	m.htotal = m.hdisplay + (int)hblank;
	m.vdisplay = (int)vactive;
	m.vsync_start = m.vdisplay + (int)vsync_offset;
	m.vsync_end = m.vsync_start + (int)vsync_pulse_width;
	m.vtotal = m.vdisplay + (int)vblank;
	if (m.hsync_end > m.htotal) m.hsync_end = m.htotal;
	if (m.vsync_end > m.vtotal) m.vsync_end = m.vtotal;
	// struct detailed_pixel_timing: misc is byte 15 of the pixel data (byte 17 of the descriptor)
	doInterlaceQuirk(&m, d);
	m.flags |= (pt[15] & kPtHsyncPositive) ? kPHSync : kNHSync;
	m.flags |= (pt[15] & kPtVsyncPositive) ? kPVSync : kNVSync;
	m.type = kTypeDriver;
	*out = m;
	return true;
}

// ---- CTA-861 (drm_edid.c:4236-4600) -------------------------------------------------------------------------------
const TableMode *ceaModeForVic(int vic)   // drm_edid.c:4236
{
	if (vic >= 1 && vic < 1 + (int)(sizeof edid_cea_modes_1 / sizeof edid_cea_modes_1[0])) return &edid_cea_modes_1[vic - 1];
	if (vic >= 193 && vic < 193 + (int)(sizeof edid_cea_modes_193 / sizeof edid_cea_modes_193[0])) return &edid_cea_modes_193[vic - 193];
	return nullptr;
}
int ceaNumVics() { return 193 + (int)(sizeof edid_cea_modes_193 / sizeof edid_cea_modes_193[0]); }
int ceaNextVic(int vic) { if (++vic == 1 + (int)(sizeof edid_cea_modes_1 / sizeof edid_cea_modes_1[0])) vic = 193; return vic; }
bool validCeaVic(int vic) { return ceaModeForVic(vic) != nullptr; }
constexpr int kNum4k = (int)(sizeof edid_4k_modes / sizeof edid_4k_modes[0]);
bool validHdmiVic(int vic) { return vic > 0 && vic < kNum4k; }

// drm_edid.c:4265 cea_mode_alternate_clock
int ceaModeAlternateClock(const Mode &cea)
{
	int clock = cea.clock;
	if (vrefresh(cea) % 6 != 0) return clock;
	if (cea.vdisplay == 240 || cea.vdisplay == 480) clock = divRoundClosest((long long)clock * 1001, 1000);
	else clock = divRoundClosest((long long)clock * 1000, 1001);
	return clock;
}

// drm_edid.c:4286 cea_mode_alternate_timings
bool ceaModeAlternateTimings(int vic, Mode *m)
{
	if (((vic == 8 || vic == 9 || vic == 12 || vic == 13) && m->vtotal < 263) ||
	    ((vic == 23 || vic == 24 || vic == 27 || vic == 28) && m->vtotal < 314)) {
		m->vsync_start++; m->vsync_end++; m->vtotal++;
		return true;
	}
	return false;
}

bool matchTimingsFlags(const Mode &a, const Mode &b, bool aspect)   // drm_modes.c:1472-1563 drm_mode_match_timings/drm_mode_match (1531)
{
	return a.hdisplay == b.hdisplay && a.hsync_start == b.hsync_start && a.hsync_end == b.hsync_end && a.htotal == b.htotal &&
	       a.hskew == b.hskew && a.vdisplay == b.vdisplay && a.vsync_start == b.vsync_start && a.vsync_end == b.vsync_end &&
	       a.vtotal == b.vtotal && a.vscan == b.vscan && (a.flags & ~k3DMask) == (b.flags & ~k3DMask) &&
	       (!aspect || a.aspect == b.aspect);
}

// drm_edid.c:4320 drm_match_cea_mode_clock_tolerance (tolerance >= 0) and 4363 drm_match_cea_mode (tolerance < 0: the
// KHZ2PICOS comparison)
int matchCeaMode(const Mode &to, int tolerance)
{
	if (!to.clock) return 0;
	for (int vic = 1; vic < ceaNumVics(); vic = ceaNextVic(vic)) {
		Mode cea = fromTable(*ceaModeForVic(vic));
		const int clock1 = cea.clock, clock2 = ceaModeAlternateClock(cea);
		if (tolerance >= 0) {
			if (iabs(to.clock - clock1) > tolerance && iabs(to.clock - clock2) > tolerance) continue;
		} else if (khz2picos(to.clock) != khz2picos(clock1) && khz2picos(to.clock) != khz2picos(clock2)) continue;
		do {
			if (matchTimingsFlags(to, cea, to.aspect != 0)) return vic;
		} while (ceaModeAlternateTimings(vic, &cea));
	}
	return 0;
}
// drm_edid.c:4427 / 4466 the HDMI 4k twins
int matchHdmiMode(const Mode &to, int tolerance)
{
	if (!to.clock) return 0;
	for (int vic = 1; vic < kNum4k; vic++) {
		const Mode h = fromTable(edid_4k_modes[vic]);
		const int clock1 = h.clock, clock2 = ceaModeAlternateClock(h);
		if (tolerance >= 0) {
			if (iabs(to.clock - clock1) > tolerance && iabs(to.clock - clock2) > tolerance) continue;
		} else if (khz2picos(to.clock) != khz2picos(clock1) && khz2picos(to.clock) != khz2picos(clock2)) continue;
		if (matchTimingsFlags(to, h, to.aspect != 0)) return vic;
	}
	return 0;
}

// drm_edid.c:5349 fixup_detailed_cea_mode_clock
void fixupDetailedCeaModeClock(Mode *m)
{
	int clock1, clock2;
	int vic = matchCeaMode(*m, 5);
	if (validCeaVic(vic)) {
		const Mode cea = fromTable(*ceaModeForVic(vic));
		clock1 = cea.clock; clock2 = ceaModeAlternateClock(cea);
	} else {
		vic = matchHdmiMode(*m, 5);
		if (!validHdmiVic(vic)) return;
		const Mode h = fromTable(edid_4k_modes[vic]);
		clock1 = h.clock; clock2 = ceaModeAlternateClock(h);
	}
	m->clock = iabs(m->clock - clock1) < iabs(m->clock - clock2) ? clock1 : clock2;
}

// drm_edid.c:4570 svd_to_vic
int svdToVic(uint8_t svd) { return ((svd >= 1 && svd <= 64) || (svd >= 129 && svd <= 192)) ? (svd & 127) : svd; }

// CTA data block collections (drm_edid.c:5030-5180 cea_db_iter_*): every CTA extension (revision 3+), then the CTA blocks
// embedded in DisplayID
template <typename F> void forEachCeaDb(const Edid &e, F cb);

// DisplayID iteration (drm_displayid.c:14-150): each DisplayID extension block's section, its data blocks in order
template <typename F> void forEachDisplayIdBlock(const Edid &e, F cb)
{
	bool first = true;
	for (int i = 0; i < e.extCount; i++) {
		const uint8_t *ext = e.ext[i];
		if (ext[0] != kDisplayIdExt) continue;
		// drm_find_displayid_extension / validate_displayid: the section is at idx 1, its checksum over header + bytes + 1
		int length = kEdidLength - 1, idx = 1;
		if (4 > length - idx) continue;
		const uint8_t *hdr = ext + idx;
		const int dispid_length = 4 + hdr[1] + 1;
		if (dispid_length > length - idx) continue;
		uint8_t csum = 0;
		for (int k = 0; k < dispid_length; k++) csum = (uint8_t)(csum + ext[idx + k]);
		if (csum) continue;
		length = idx + 4 + hdr[1];
		(void)first; first = false;
		for (int off = idx + 4; off + 3 <= length && off + 3 + ext[off + 2] <= length; off += 3 + ext[off + 2])
			cb(ext + off, (int)ext[off + 2]);
	}
}

template <typename F> void forEachCeaDb(const Edid &e, F cb)
{
	for (int i = 0; i < e.extCount; i++) {
		const uint8_t *ext = e.ext[i];
		if (ext[0] != kCeaExt || ext[1] < 3) continue;   // cea_revision(ext) >= 3
		const int d = ext[2];
		if (d < 4 || d > 127) continue;                  // cea_db_collection_size
		const int end = d;
		for (int idx = 4; idx + 1 <= end && idx + 1 + (ext[idx] & 0x1F) <= end; idx += 1 + (ext[idx] & 0x1F))
			cb(ext + idx);
	}
	forEachDisplayIdBlock(e, [&](const uint8_t *blk, int num) {
		if (blk[0] != kDidCta) return;
		const uint8_t *c = blk;   // the collection starts after the 3-byte DisplayID block header
		const int end = 3 + num;
		for (int idx = 3; idx + 1 <= end && idx + 1 + (c[idx] & 0x1F) <= end; idx += 1 + (c[idx] & 0x1F)) cb(c + idx);
	});
}
int ceaDbTag(const uint8_t *db) { return db[0] >> 5; }
int ceaDbLen(const uint8_t *db) { return db[0] & 0x1F; }
bool ceaDbIsExtendedTag(const uint8_t *db, int tag) { return ceaDbTag(db) == kCtaDbExtendedTag && ceaDbLen(db) >= 1 && db[1] == tag; }
bool ceaDbIsVendor(const uint8_t *db, int oui)
{
	return ceaDbTag(db) == kCtaDbVendor && ceaDbLen(db) >= 3 && ((db[3] << 16) | (db[2] << 8) | db[1]) == oui;
}

// drm_edid.c:4202 drm_edid_has_cta_extension
bool hasCtaExtension(const Edid &e)
{
	for (int i = 0; i < e.extCount; i++) if (e.ext[i][0] == kCeaExt) return true;
	bool found = false;
	forEachDisplayIdBlock(e, [&](const uint8_t *blk, int) { if (blk[0] == kDidCta) found = true; });
	return found;
}

// ---- inferred modes (drm_edid.c:3653-3925) -------------------------------------------------------------------------
bool modeInRange(const Mode &m, const Edid &e, const uint8_t *t)
{
	int hmin = t[7], hmax = t[8], vmin = t[5], vmax = t[6];
	if (e.revision() >= 4) {
		hmin += (t[4] & 0x04) ? 255 : 0; hmax += (t[4] & 0x08) ? 255 : 0;
		vmin += (t[4] & 0x01) ? 255 : 0; vmax += (t[4] & 0x02) ? 255 : 0;
	}
	const int hsync = modeHsync(m);
	if (!(hsync <= hmax && hsync >= hmin)) return false;
	const int vsync = vrefresh(m);
	if (!(vsync <= vmax && vsync >= vmin)) return false;
	// range_pixel_clock
	unsigned max_clock = 0;
	if (t[9] != 0 && t[9] != 255) {
		if (e.revision() >= 4 && t[10] == kCvtSupport) max_clock = (unsigned)(t[9] * 10000 - ((t[12] >> 2) * 250));
		else max_clock = (unsigned)(t[9] * 10000 + 5001);
	}
	if (max_clock && (unsigned)m.clock > max_clock) return false;
	if (e.revision() >= 4 && t[10] == kCvtSupport)
		if (t[13] && m.hdisplay > 8 * (t[13] + (256 * (t[12] & 0x3)))) return false;
	if (modeIsRb(m) && !monitorSupportsRb(e)) return false;
	return true;
}
bool validInferredMode(const Probed &p, const Mode &mode)
{
	bool ok = false;
	for (int i = 0; i < p.r->count; i++) {
		const Mode &m = p.r->modes[i];
		if (mode.hdisplay == m.hdisplay && mode.vdisplay == m.vdisplay && vrefresh(mode) == vrefresh(m)) return false;
		if (mode.hdisplay <= m.hdisplay && mode.vdisplay <= m.vdisplay) ok = true;
	}
	return ok;
}
void fixup1366x768(Mode *m) { if (m->hdisplay == 1368 && m->vdisplay == 768) { m->hdisplay = 1366; m->hsync_start--; m->hsync_end--; } }

void addInferredModes(Probed &p, const Edid &e)   // drm_edid.c:3825 do_inferred_modes / 3866 add_inferred_modes
{
	if (e.revision() < 1) return;
	forEachDetailedBlock(e, [&](const uint8_t *t) {
		if (!isDisplayDescriptor(t, kDetailMonitorRange)) return;
		for (const TableMode &tm : drm_dmt_modes) {   // drm_dmt_modes_for_range
			const Mode m = fromTable(tm);
			if (modeInRange(m, e, t) && validInferredMode(p, m)) p.add(m, kSrcInferred);
		}
		if (e.revision() < 2) return;
		const int flag = t[10];
		if (flag != kSecondaryGtf && flag != kDefaultGtf && !(flag == kCvtSupport && e.revision() >= 4)) return;
		const bool rb = monitorSupportsRb(e);
		for (const MiniMode &mm : extra_modes) {   // drm_gtf_modes_for_range / gtf2 / cvt
			Mode m;
			bool ok = flag == kSecondaryGtf ? gtf2Mode(e, mm.w, mm.h, mm.r, &m)
			        : flag == kDefaultGtf   ? gtfMode(mm.w, mm.h, mm.r, false, 0, &m)
			                                : cvtMode(mm.w, mm.h, mm.r, rb, false, false, &m);
			if (!ok) return;
			fixup1366x768(&m);
			if (!modeInRange(m, e, t) || !validInferredMode(p, m)) continue;
			p.add(m, kSrcInferred);
		}
	});
}

// ---- adders in _drm_edid_connector_add_modes order (drm_edid.c:6940) ----------------------------------------------
void addDetailedModes(Probed &p, const Edid &e)   // drm_edid.c:4131 / 4106 do_detailed_mode
{
	bool preferred = e.revision() >= 4 ? true : (e.features() & kFeaturePreferredTiming);
	forEachDetailedBlock(e, [&](const uint8_t *d) {
		if (!isDetailedTimingDescriptor(d)) return;
		Mode m;
		if (!modeDetailed(d, &m)) return;
		if (preferred) m.type |= kTypePreferred;
		fixupDetailedCeaModeClock(&m);
		p.add(m, kSrcDetailed);
		preferred = false;
	});
}

void addCvtModes(Probed &p, const Edid &e)   // drm_edid.c:4015 drm_cvt_modes / 4078 add_cvt_modes
{
	if (e.revision() < 3) return;
	static const int rates[] = { 60, 85, 75, 60, 50 };
	forEachDetailedBlock(e, [&](const uint8_t *t) {
		if (!isDisplayDescriptor(t, kDetailCvt3Byte)) return;
		for (int i = 0; i < 4; i++) {
			// data.other_data.data.cvt[i]: Linux's struct puts cvt[0] at byte 5 (where VESA has the version byte, 01h)
			// and reads the 4th code from bytes 14-16; ported as Linux reads it
			const uint8_t *code = t + 5 + 3 * i;
			if (!code[0] && !code[1] && !code[2]) continue;
			const int height = (code[0] + ((code[1] & 0xF0) << 4) + 1) * 2;
			int width;
			switch (code[1] & 0x0C) {
			default:
			case 0x00: width = height * 4 / 3; break;
			case 0x04: width = height * 16 / 9; break;
			case 0x08: width = height * 16 / 10; break;
			case 0x0C: width = height * 15 / 9; break;
			}
			for (int j = 1; j < 5; j++)
				if (code[2] & (1 << j)) {
					Mode m;
					if (cvtMode(width, height, rates[j], j == 0, false, false, &m)) p.add(m, kSrcCvt3);
				}
		}
	});
}

void addStandardModes(Probed &p, const Edid &e)   // drm_edid.c:3986 add_standard_modes / 3961 do_standard_modes
{
	for (int i = 0; i < 8; i++) {
		Mode m;
		if (modeStd(p, e, e.base + 38 + 2 * i, &m)) p.add(m, kSrcStandard);
	}
	if (e.revision() < 1) return;
	forEachDetailedBlock(e, [&](const uint8_t *t) {
		if (!isDisplayDescriptor(t, kDetailStdModes)) return;
		for (int i = 0; i < 6; i++) {
			Mode m;
			if (modeStd(p, e, t + 5 + 2 * i, &m)) p.add(m, kSrcStandard);
		}
	});
}

void addEstablishedModes(Probed &p, const Edid &e)   // drm_edid.c:3925 add_established_modes / 3890 drm_est3_modes
{
	const unsigned long est_bits = e.base[35] | (e.base[36] << 8) | ((e.base[37] & 0x80) << 9);
	for (int i = 0; i <= 16; i++)   // EDID_EST_TIMINGS 16
		if (est_bits & (1ul << i)) p.add(fromTable(edid_est_modes[i]), kSrcEstablished);
	if (e.revision() < 1) return;
	forEachDetailedBlock(e, [&](const uint8_t *t) {
		if (!isDisplayDescriptor(t, kDetailEstTimings)) return;
		const uint8_t *est = t + 6;
		const int n3 = (int)(sizeof est3_modes / sizeof est3_modes[0]);
		for (int i = 0; i < 6; i++)
			for (int j = 7; j >= 0; j--) {
				const int m = (i * 8) + (7 - j);
				if (m >= n3) break;
				Mode mode;
				if ((est[i] & (1 << j)) && findDmt(est3_modes[m].w, est3_modes[m].h, est3_modes[m].r, est3_modes[m].rb, &mode))
					p.add(mode, kSrcEstablished);
			}
	});
}

// drm_edid.c:5321 add_cea_modes: the VDB VICs (parse_cta_vdb 6011 / add_cta_vdb_modes 4657), HDMI VSDB HDMI_VICs
// (do_hdmi_vsdb_modes 4820, without its 3D modes), YCbCr 4:2:0 VDB modes (do_y420vdb_modes 4605)
void addCeaModes(Probed &p, const Edid &e, uint8_t *y420vdb)
{
	forEachCeaDb(e, [&](const uint8_t *db) {   // parse_cta_vdb: info->vics in VDB order
		if (ceaDbTag(db) != kCtaDbVideo) return;
		for (int i = 0; i < ceaDbLen(db); i++) {
			const int vic = svdToVic(db[1 + i]);
			if (validCeaVic(vic)) p.add(fromTable(*ceaModeForVic(vic)), kSrcCtaVdb);
		}
	});
	forEachCeaDb(e, [&](const uint8_t *db) {
		const int len = ceaDbLen(db);
		if (ceaDbIsVendor(db, kHdmiIeeeOui)) {
			// db[] as Linux indexes it: db[0] tag/length, payload from db[1]
			if (len < 8 || !(db[8] & (1 << 5))) return;   // HDMI_Video_Present
			int offset = 0;
			if (db[8] & (1 << 7)) offset += (db[8] & (1 << 6)) ? 4 : 2;   // hdmi_vsdb_latency_length
			if (len < 8 + offset + 2) return;
			offset++;            // 3D_Present byte (3D modes not added, see IntelXeEdid.h)
			offset++;
			const int vic_len = db[8 + offset] >> 5;
			for (int i = 0; i < vic_len && len >= 9 + offset + i; i++) {
				const int vic = db[9 + offset + i];
				if (validHdmiVic(vic)) p.add(fromTable(edid_4k_modes[vic]), kSrcHdmiVsdb);
			}
		} else if (ceaDbIsExtendedTag(db, kCtaExtDb420VideoData)) {
			for (int i = 0; i < len - 1; i++) {
				const int vic = svdToVic(db[2 + i]);
				if (!validCeaVic(vic)) continue;
				y420vdb[vic >> 3] |= (uint8_t)(1 << (vic & 7));   // parse_cta_y420vdb: hdmi.y420_vdb_modes
				p.add(fromTable(*ceaModeForVic(vic)), kSrcCtaY420);
			}
		}
	});
}

void addAlternateCeaModes(Probed &p, const Edid &e)   // drm_edid.c:4498 add_alternate_cea_modes
{
	if (!hasCtaExtension(e)) return;
	const int n = p.r->count;   // new modes go on a side list, then to the tail
	static Mode add[kMaxModes];   // 9 KB: not on the kernel stack (one parse at a time, see parse)
	int na = 0;
	for (int i = 0; i < n; i++) {
		const Mode &mode = p.r->modes[i];
		Mode cea;
		int clock2;
		int vic = matchCeaMode(mode, -1);
		if (validCeaVic(vic)) { cea = fromTable(*ceaModeForVic(vic)); clock2 = ceaModeAlternateClock(cea); }
		else {
			vic = matchHdmiMode(mode, -1);
			if (!validHdmiVic(vic)) continue;
			cea = fromTable(edid_4k_modes[vic]); clock2 = ceaModeAlternateClock(cea);
		}
		const int clock1 = cea.clock;
		if (clock1 == clock2) continue;
		if (mode.clock != clock1 && mode.clock != clock2) continue;
		Mode nm = cea;
		nm.flags |= mode.flags & k3DMask;
		nm.clock = mode.clock != clock1 ? clock1 : clock2;
		if (na < kMaxModes) add[na++] = nm;
	}
	for (int i = 0; i < na; i++) p.add(add[i], kSrcCtaAlternate);
}

// drm_edid.c:6785 drm_mode_displayid_detailed / 6832 add_displayid_detailed_1_modes / 6859 drm_mode_displayid_formula /
// 6888 add_displayid_formula_modes / 6919 add_displayid_detailed_modes
void addDisplayIdModes(Probed &p, const Edid &e)
{
	forEachDisplayIdBlock(e, [&](const uint8_t *blk, int num) {
		const uint8_t tag = blk[0];
		if (tag == kDidType1Detailed || tag == kDid2Type7Detailed) {
			const bool type_7 = tag == kDid2Type7Detailed;
			if (num % 20) return;
			for (int i = 0; i < num / 20; i++) {
				const uint8_t *t = blk + 3 + 20 * i;
				const unsigned pixel_clock = (unsigned)(t[0] | t[1] << 8 | t[2] << 16) + 1;
				const unsigned hactive = le16(t + 4) + 1u, hblank = le16(t + 6) + 1u;
				const unsigned hsync = (le16(t + 8) & 0x7FFFu) + 1, hsync_width = le16(t + 10) + 1u;
				const unsigned vactive = le16(t + 12) + 1u, vblank = le16(t + 14) + 1u;
				const unsigned vsync = (le16(t + 16) & 0x7FFFu) + 1, vsync_width = le16(t + 18) + 1u;
				const bool hpos = le16(t + 8) & (1 << 15), vpos = le16(t + 16) & (1 << 15);
				Mode m = {};
				m.clock = (int)(type_7 ? pixel_clock : pixel_clock * 10);
				m.hdisplay = (int)hactive; m.hsync_start = m.hdisplay + (int)hsync; m.hsync_end = m.hsync_start + (int)hsync_width;
				m.htotal = m.hdisplay + (int)hblank;
				m.vdisplay = (int)vactive; m.vsync_start = m.vdisplay + (int)vsync; m.vsync_end = m.vsync_start + (int)vsync_width;
				m.vtotal = m.vdisplay + (int)vblank;
				m.flags = (hpos ? kPHSync : kNHSync) | (vpos ? kPVSync : kNVSync);
				m.type = kTypeDriver;
				if (t[3] & 0x80) m.type |= kTypePreferred;
				p.add(m, kSrcDisplayId);
			}
		} else if (tag == kDid2Type9Formula || tag == kDid2Type10Formula) {
			const int timing_size = 6 + ((blk[1] & 0x70) >> 4);
			if (timing_size != 6 || num % timing_size) return;
			for (int i = 0; i < num / timing_size; i++) {
				const uint8_t *t = blk + 3 + 6 * i;
				const int formula = t[0] & 0x7;
				if (formula > 1) continue;
				Mode m;
				if (!cvtMode(le16(t + 1) + 1, le16(t + 3) + 1, t[5] + 1, formula == 1, false, false, &m)) continue;
				m.type = kTypeDriver;
				p.add(m, kSrcDisplayIdFormula);
			}
		}
	});
}

// drm_edid.c:6469 get_monitor_range / 6504 drm_get_monitor_range
void monitorRange(const Edid &e, Result *r)
{
	r->minVfreq = r->maxVfreq = 0;
	if (e.revision() < 4 || !(e.features() & kFeatureContinuousFreq)) return;
	forEachDetailedBlock(e, [&](const uint8_t *t) {
		if (!isDisplayDescriptor(t, kDetailMonitorRange) || t[10] != kRangeLimitsOnly) return;
		r->minVfreq = t[5]; r->maxVfreq = t[6];
		if (t[4] & 0x01) r->minVfreq += 255;   // DRM_EDID_RANGE_OFFSET_MIN_VFREQ
		if (t[4] & 0x02) r->maxVfreq += 255;   // DRM_EDID_RANGE_OFFSET_MAX_VFREQ
		for (int i = 0; i < 18; i++) r->rangeDesc[i] = t[i];
		r->hasRangeOnly = true;
	});
}

// drm_edid.c:1866 edid_block_check / 1937 edid_block_status_valid (edid_fixup 6: a header with 6 of 8 bytes right is
// repaired; drm_edid_header_is_valid scores the bytes)
bool blockValid(const uint8_t *b, bool base)
{
	if (base) {
		static const uint8_t hdr[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
		int score = 0;
		for (int i = 0; i < 8; i++) if (b[i] == hdr[i]) score++;
		if (score < 6) return false;
	}
	uint8_t csum = 0;
	for (int i = 0; i < kEdidLength - 1; i++) csum = (uint8_t)(csum + b[i]);
	const bool sumOk = (uint8_t)(0x100 - csum) == b[127];
	if (!sumOk) return !base && b[0] == kCeaExt;   // CTA extensions with a bad checksum are kept
	if (base && b[18] != 1) return false;          // EDID_BLOCK_VERSION
	return true;
}

}  // namespace

bool cvt(int hdisplay, int vdisplay, int vrefresh, bool reduced, Mode *out)
{
	return cvtMode(hdisplay, vdisplay, vrefresh, reduced, false, false, out);
}

const char *sourceName(uint8_t s)
{
	switch (s) {
	case kSrcDetailed: return "DTD";
	case kSrcCvt3: return "CVT3";
	case kSrcStandard: return "STD";
	case kSrcEstablished: return "EST";
	case kSrcCtaVdb: return "VIC";
	case kSrcHdmiVsdb: return "HDMI-VIC";
	case kSrcCtaY420: return "Y420";
	case kSrcCtaAlternate: return "VIC-alt";
	case kSrcDisplayId: return "DisplayID";
	case kSrcDisplayIdFormula: return "DisplayID-formula";
	case kSrcInferred: return "inferred";
	}
	return "?";
}

const char *statusName(int s)
{
	switch (s) {
	case kOk: return "OK";
	case kHSyncBad: return "HSYNC";
	case kVSyncBad: return "VSYNC";
	case kHIllegal: return "H_ILLEGAL";
	case kVIllegal: return "V_ILLEGAL";
	case kBad: return "BAD";
	case kNoInterlace: return "NO_INTERLACE";
	case kNoDblScan: return "NO_DBLESCAN";
	case kNoVScan: return "NO_VSCAN";
	case kClockHigh: return "CLOCK_HIGH";
	case kClockLow: return "CLOCK_LOW";
	case kNoStereo: return "NO_STEREO";
	case kNo420: return "NO_420";
	case kDuplicate: return "DUPLICATE";
	}
	return "?";
}

int vrefresh(const Mode &m)
{
	unsigned long long num = 1, den = 1;
	if (m.htotal == 0 || m.vtotal == 0) return 0;
	if (m.flags & kInterlace) num *= 2;
	if (m.flags & kDblScan) den *= 2;
	if (m.vscan > 1) den *= (unsigned)m.vscan;
	num = (unsigned long long)m.clock * num * 1000;
	den = (unsigned long long)m.htotal * m.vtotal * den;
	return (int)((num + den / 2) / den);
}

uint32_t refreshMilliHz(const Mode &m)
{
	if (m.htotal == 0 || m.vtotal == 0) return 0;
	unsigned long long num = (unsigned long long)m.clock * 1000000ull * ((m.flags & kInterlace) ? 2 : 1);
	unsigned long long den = (unsigned long long)m.htotal * m.vtotal * ((m.flags & kDblScan) ? 2 : 1) * (m.vscan > 1 ? m.vscan : 1);
	return (uint32_t)((num + den / 2) / den);
}

bool sameTimings(const Mode &a, const Mode &b)
{
	const bool clockOk = (a.clock && b.clock) ? khz2picos(a.clock) == khz2picos(b.clock) : a.clock == b.clock;
	return matchTimingsFlags(a, b, false) && clockOk && (a.flags & k3DMask) == (b.flags & k3DMask);
}

void sort(Mode *m, int n)
{
	auto cmp = [](const Mode &a, const Mode &b) {   // < 0: a first
		int diff = ((b.type & kTypePreferred) != 0) - ((a.type & kTypePreferred) != 0);
		if (diff) return diff;
		diff = b.hdisplay * b.vdisplay - a.hdisplay * a.vdisplay;
		if (diff) return diff;
		diff = vrefresh(b) - vrefresh(a);
		if (diff) return diff;
		return b.clock - a.clock;
	};
	for (int i = 1; i < n; i++) {   // insertion sort: stable, as list_sort
		Mode x = m[i];
		int j = i - 1;
		while (j >= 0 && cmp(m[j], x) > 0) { m[j + 1] = m[j]; j--; }
		m[j + 1] = x;
	}
}

bool parse(const uint8_t *edid, uint32_t len, Result *r)
{
	r->count = r->overflow = 0;
	r->revision = 0; r->digital = r->continuous = false;
	r->minVfreq = r->maxVfreq = 0;
	r->blocks = r->badBlocks = 0;
	r->hasCta = r->hasDisplayId = false;
	r->hasRangeOnly = r->supportsRb = false;
	if (!edid || len < (uint32_t)kEdidLength || !blockValid(edid, true)) return false;
	static Edid e;   // 2 KB of pointers: not on the kernel stack (callers serialize, see IntelXeFB::buildModes)
	e.base = edid;
	e.extCount = 0;
	const int want = edid[126];
	for (int i = 1; i <= want && (uint32_t)(i + 1) * kEdidLength <= len; i++) {
		const uint8_t *b = edid + i * kEdidLength;
		if (blockValid(b, false)) e.ext[e.extCount++] = b; else r->badBlocks++;
	}
	r->blocks = 1 + e.extCount;
	r->revision = e.revision();
	r->digital = e.digital();
	r->continuous = e.features() & kFeatureContinuousFreq;
	for (int i = 0; i < e.extCount; i++) {
		if (e.ext[i][0] == kCeaExt) r->hasCta = true;
		if (e.ext[i][0] == kDisplayIdExt) r->hasDisplayId = true;
	}
	monitorRange(e, r);
	r->supportsRb = monitorSupportsRb(e);
	Probed p = { r };
	uint8_t y420vdb[32] = {};
	addDetailedModes(p, e);
	addCvtModes(p, e);
	addStandardModes(p, e);
	addEstablishedModes(p, e);
	addCeaModes(p, e, y420vdb);
	addAlternateCeaModes(p, e);
	addDisplayIdModes(p, e);
	if (e.features() & kFeatureContinuousFreq) addInferredModes(p, e);
	for (int i = 0; i < r->count; i++) {   // drm_mode_is_420_only
		const int vic = matchCeaMode(r->modes[i], -1);
		r->modes[i].y420only = vic && (y420vdb[vic >> 3] & (1 << (vic & 7)));
	}
	return true;
}

bool modeInRangeLimits(const Result &r, const Mode &m)
{
	if (!r.hasRangeOnly) return false;
	const uint8_t *t = r.rangeDesc;   // revision >= 4 (drm_get_monitor_range), so the offset flags apply
	const int hmin = t[7] + ((t[4] & 0x04) ? 255 : 0), hmax = t[8] + ((t[4] & 0x08) ? 255 : 0);
	const int vmin = t[5] + ((t[4] & 0x01) ? 255 : 0), vmax = t[6] + ((t[4] & 0x02) ? 255 : 0);
	const int hsync = modeHsync(m), vsync = vrefresh(m);
	if (hsync > hmax || hsync < hmin || vsync > vmax || vsync < vmin) return false;
	if (t[9] != 0 && t[9] != 255 && (unsigned)m.clock > (unsigned)(t[9] * 10000 + 5001)) return false;   // range_pixel_clock, flags != CVT
	if (modeIsRb(m) && !r.supportsRb) return false;
	return true;
}

int modeValid(const Mode &m, const Sink &s)
{
	// intel_display.c:7918 intel_mode_valid (display 11+ limits)
	if (m.vscan > 1) return kNoVScan;
	if (m.flags & kHSkew) return kHIllegal;
	if (m.flags & (kCSync | kNCSync | kPCSync)) return kHSyncBad;
	if (m.flags & (kBcast | kPixMux | kClkDiv2)) return kBad;
	if (m.clock > s.maxDotclk * 4) return kClockHigh;   // max_dotclock(): x4 with HAS_ULTRAJOINER (dGPU display 14)
	if (m.hdisplay > 16384 || m.hsync_start > 16384 || m.hsync_end > 16384 || m.htotal > 16384) return kHIllegal;
	if (m.vdisplay > 8192 || m.vsync_start > 8192 || m.vsync_end > 8192 || m.vtotal > 8192) return kVIllegal;
	// drm_probe_helper.c:71 drm_mode_validate_flag with the DP connector's flags (no doublescan, no stereo)
	if ((m.flags & kInterlace) && !s.interlace) return kNoInterlace;
	if (m.flags & kDblScan) return kNoDblScan;
	if (m.flags & k3DMask) return kNoStereo;
	// drm_modes.c:1727 drm_mode_validate_ycbcr420
	if (m.y420only && !s.ycbcr420) return kNo420;
	// intel_dp.c:1415 intel_dp_mode_valid (external DP: no fixed mode, no DSC, one pipe)
	// intel_display.c:8000 intel_cpu_transcoder_mode_valid
	if (m.hdisplay < 64 || m.htotal - m.hdisplay < 32) return kHIllegal;
	if (m.vtotal - m.vdisplay < 5) return kVIllegal;
	if (m.hsync_start == m.hdisplay) return kHIllegal;
	if (m.flags & kDblClk) return kHIllegal;
	if (m.clock < 10000) return kClockLow;
	if (m.clock > s.maxDotclk) return kClockHigh;
	// intel_dp.c:441 intel_dp_link_required vs intel_dp_max_link_data_rate -> drm_dp_helper.c:4635
	// drm_dp_max_dprx_data_rate (8b/10b: 80 % channel coding efficiency)
	const long long mode_rate = ((long long)m.clock * s.bpp + 7) / 8;
	const long long max_rate = (long long)s.linkRate * 10 * s.lanes * 800000 / (1000000LL * 8);
	if (mode_rate > max_rate) return kClockHigh;
	// intel_display.c:8034 intel_mode_valid_max_plane_size (display 11-29)
	if (s.displayVer >= 30) { if (m.hdisplay > 6144) return kHIllegal; if (m.vdisplay > 4800) return kVIllegal; }
	else if (s.displayVer >= 11) { if (m.hdisplay > 5120) return kHIllegal; if (m.vdisplay > 4320) return kVIllegal; }
	return kOk;
}

}  // namespace intelxeedid

// ---- IntelXeFB's own mode table (see IntelXeEdid.h) ----------------------------------------------------------------------
namespace intelxeedid {
namespace {

// Rates Intel's Windows driver (32.0.101.9033) lists for every size of an Adaptive-Sync sink besides the EDID's own
// (observed on the Odyssey G5, 3 Nov 2026: 48 = the range minimum, 60, 75, 100, 120). Not from Linux: i915 accepts any
// in-range refresh for a scaled mode (intel_panel_compute_config) but its mode list holds EDID modes only. Used only
// for a VRR-capable sink and inside its range; -intelxefb_edidonly drops them.
const int kVrrCommonRates[] = { 60, 75, 100, 120 };

// Two rates are one entry when they round to the same Hz and are both, or both not, the 1000/1001 rate of a CTA rate
// (cea_mode_alternate_clock: rates that are a multiple of 6 have a 59.94-style twin) -- so 59.94/59.95 merge, 60.00
// stays apart from them, and 164.92/164.98 merge
uint32_t rateKey(uint32_t mhz)
{
	const uint32_t n = (mhz + 500) / 1000;
	const uint32_t alt = (uint32_t)(((uint64_t)n * 1000000ull + 500) / 1001);
	const bool ntsc = n % 6 == 0 && (mhz > alt ? mhz - alt : alt - mhz) <= 20 && (mhz > n * 1000 ? mhz - n * 1000 : n * 1000 - mhz) > 20;
	return n * 2 + (ntsc ? 1 : 0);
}

// the vertical total that gives `mhz` on the firmware's raster (DIV_ROUND_CLOSEST as intel_panel_compute_config)
uint32_t vtFor(const Panel &p, uint32_t mhz)
{
	const uint64_t den = (uint64_t)p.htotal * mhz;
	return den ? (uint32_t)((p.pclkHz * 1000ull + den / 2) / den) : 0;
}
uint32_t mhzFor(const Panel &p, uint32_t vt)
{
	const uint64_t den = (uint64_t)p.htotal * vt;
	return den ? (uint32_t)((p.pclkHz * 1000ull + den / 2) / den) : 0;
}

struct SizeRates {
	uint16_t w, h;
	int n;
	DriverMode r[kMaxRatesPerSize];
};

}  // namespace

const char *whyName(uint8_t w)
{
	switch (w) {
	case kWhyFirmware: return "firmware";
	case kWhyEdid: return "EDID";
	case kWhyPanel: return "panel";
	case kWhyVrrMin: return "VRR min";
	case kWhyCommon: return "VRR common";
	}
	return "?";
}

int buildDriverModes(const Mode *valid, int n, const Panel &p, DriverMode *out, int max)
{
	if (!p.width || !p.height || !p.htotal || !p.vtotal || !p.pclkHz) return 0;
	static SizeRates sz[kMaxSizes];   // 3 KB: not on the kernel stack (built once at start)
	int ns = 0;
	auto sizeIndex = [&](int w, int h) {
		for (int i = 0; i < ns; i++) if (sz[i].w == w && sz[i].h == h) return i;
		if (ns >= kMaxSizes) return -1;
		sz[ns].w = (uint16_t)w; sz[ns].h = (uint16_t)h; sz[ns].n = 0;
		return ns++;
	};
	// sizes: the panel's first, then the EDID's in the sorted list's order (largest first); the scaler only upscales here
	sizeIndex((int)p.width, (int)p.height);
	if (p.scaling)
		for (int i = 0; i < n; i++)
			if (valid[i].hdisplay <= (int)p.width && valid[i].vdisplay <= (int)p.height && !(valid[i].flags & kInterlace))
				sizeIndex(valid[i].hdisplay, valid[i].vdisplay);
	const uint32_t fwMhz = mhzFor(p, p.vtotal);
	// the panel's own rates: EDID modes of the panel's size (intel_panel.c:250 intel_panel_add_edid_alt_fixed_modes)
	uint32_t panelRates[kMaxRatesPerSize]; int np = 0;
	for (int i = 0; i < n && np < kMaxRatesPerSize; i++)
		if (valid[i].hdisplay == (int)p.width && valid[i].vdisplay == (int)p.height && !(valid[i].flags & kInterlace))
			panelRates[np++] = refreshMilliHz(valid[i]);
	// May the sink be sent the firmware's raster with vertical total vt?
	//  - VRR: the refresh is inside the range (intel_panel.c:200 intel_panel_compute_config is_vrr: vtotal stretched)
	//  - the timing is inside the EDID 1.4 range-limits-only descriptor (any timing there is accepted, drm_edid.c:6483)
	//  - it is one of the EDID's own timings (h timing, vtotal and clock of a listed mode)
	// The firmware's own timing always is. Without VRR this keeps a fixed-refresh monitor on timings it listed or bounded.
	auto runnable = [&](uint32_t vt) {
		if (vt == p.vtotal) return true;
		const uint32_t mhz = mhzFor(p, vt);
		const int hz = (int)((mhz + 500) / 1000);
		if (p.vrr && hz >= p.vrrMin && hz <= p.vrrMax) return true;
		Mode t = {};
		t.clock = (int)(p.pclkHz / 1000); t.hdisplay = (int)p.width; t.htotal = (int)p.htotal; t.vdisplay = (int)p.height;
		t.vtotal = (int)vt; t.hsync_start = (int)p.hsyncStart; t.hsync_end = (int)p.hsyncEnd; t.vsync_start = (int)p.vsyncStart;
		t.vsync_end = (int)p.vsyncEnd;
		if (p.edid && modeInRangeLimits(*p.edid, t)) return true;
		for (int i = 0; i < n; i++) {
			const Mode &m = valid[i];
			if (m.hdisplay == (int)p.width && m.vdisplay == (int)p.height && m.htotal == (int)p.htotal && m.vtotal == (int)vt &&
			    iabs(m.clock - t.clock) * 200 <= t.clock) return true;
		}
		return false;
	};
	for (int s = 0; s < ns; s++) {
		SizeRates &S = sz[s];
		auto add = [&](uint32_t mhz, uint8_t why) {
			if (!mhz || S.n >= kMaxRatesPerSize) return;
			if (why != kWhyFirmware && !p.rates) return;
			const uint32_t vt = why == kWhyFirmware ? p.vtotal : vtFor(p, mhz);
			if (vt < p.vtotal || vt > p.maxVtotal || !runnable(vt)) return;   // above the firmware's refresh, past VTOTAL
			const uint32_t key = rateKey(mhz);
			for (int i = 0; i < S.n; i++) if (S.r[i].vt == vt || rateKey(S.r[i].nominalMilliHz) == key) return;
			DriverMode &d = S.r[S.n++];
			d.w = S.w; d.h = S.h; d.vt = vt; d.milliHz = mhzFor(p, vt); d.nominalMilliHz = mhz; d.why = why; d.pclkKhz = 0;
			// a size whose aspect differs from the panel's by more than 1 % is stretched to it (kDisplayModeStretchedFlag)
			const uint64_t a = (uint64_t)S.w * p.height, b = (uint64_t)S.h * p.width;
			d.stretched = (a > b ? a - b : b - a) * 100 > b;
		};
		add(fwMhz, kWhyFirmware);
		for (int i = 0; i < n; i++)
			if (valid[i].hdisplay == S.w && valid[i].vdisplay == S.h && !(valid[i].flags & kInterlace)) add(refreshMilliHz(valid[i]), kWhyEdid);
		for (int i = 0; i < np; i++) add(panelRates[i], kWhyPanel);
		// Rates above the firmware's: the EDID's own panel-size timings on the firmware's horizontal raster with a
		// higher clock (the link validated them above, intel_dp_mode_valid). Only the clock changes (M/N on the trained
		// link) and the vertical total, never below the firmware's. The firmware may start the panel below its top rate
		// (the B580's GOP picks the G5's 165 Hz timing when OpenCore does not reconnect it; 180 Hz is the same raster).
		for (int i = 0; i < n; i++) {
			const Mode &m = valid[i];
			if (m.hdisplay != (int)p.width || m.vdisplay != (int)p.height || (m.flags & kInterlace) || m.htotal != (int)p.htotal ||
			    m.vtotal < (int)p.vtotal || (uint32_t)m.vtotal > p.maxVtotal || (uint64_t)m.clock * 1000 * 200 <= p.pclkHz * 201)
				continue;
			if (S.n >= kMaxRatesPerSize) break;
			const uint32_t mhz = refreshMilliHz(m), key = rateKey(mhz);
			bool dup = false;
			for (int j = 0; j < S.n; j++) if (rateKey(S.r[j].nominalMilliHz) == key) dup = true;
			if (dup) continue;
			DriverMode &d = S.r[S.n++];
			d.w = S.w; d.h = S.h; d.vt = (uint32_t)m.vtotal; d.milliHz = d.nominalMilliHz = mhz; d.why = kWhyEdid;
			d.pclkKhz = (uint32_t)m.clock;
			const uint64_t a = (uint64_t)S.w * p.height, b = (uint64_t)S.h * p.width;
			d.stretched = (a > b ? a - b : b - a) * 100 > b;
		}
		if (p.vrr && p.vrrMin > 0) add((uint32_t)p.vrrMin * 1000, kWhyVrrMin);
		if (p.vrr && p.commonRates)
			for (int r : kVrrCommonRates) if (r >= p.vrrMin && r <= p.vrrMax) add((uint32_t)r * 1000, kWhyCommon);
		// highest refresh first within a size (insertion sort, the firmware's stays first when it is the highest)
		for (int i = 1; i < S.n; i++) {
			DriverMode x = S.r[i]; int j = i - 1;
			while (j >= 0 && S.r[j].milliHz < x.milliHz) { S.r[j + 1] = S.r[j]; j--; }
			S.r[j + 1] = x;
		}
	}
	int k = 0;
	for (int s = 0; s < ns; s++)
		for (int i = 0; i < sz[s].n && k < max; i++) out[k++] = sz[s].r[i];
	return k;
}

int fallbackDriverModes(const Panel &p, DriverMode *out, int max)
{
	static const uint16_t kScaled[][2] = { { 2048, 1152 }, { 1920, 1080 }, { 1600, 900 }, { 1280, 720 } };
	static const uint16_t kRates[] = { 144, 120, 100, 60 };
	if (!p.width || !p.height || !p.htotal || !p.vtotal || !p.pclkHz) return 0;
	const uint32_t fwMhz = mhzFor(p, p.vtotal);
	int k = 0;
	for (int s = 0; s <= 4; s++) {
		const uint16_t w = s ? kScaled[s - 1][0] : (uint16_t)p.width, h = s ? kScaled[s - 1][1] : (uint16_t)p.height;
		if (s && (!p.scaling || w >= p.width || h >= p.height)) continue;
		for (int r = 0; r <= 4 && k < max; r++) {
			uint32_t vt = p.vtotal, mhz = fwMhz;
			if (r) {
				if (!p.rates || kRates[r - 1] * 1000u + 2000 > fwMhz) continue;
				vt = vtFor(p, kRates[r - 1] * 1000u);
				if (vt <= p.vtotal || vt > p.maxVtotal) continue;
				mhz = mhzFor(p, vt);
			}
			DriverMode &d = out[k++];
			d.w = w; d.h = h; d.vt = vt; d.milliHz = d.nominalMilliHz = mhz; d.why = kWhyFirmware; d.stretched = false; d.pclkKhz = 0;
		}
	}
	return k;
}

}  // namespace intelxeedid
