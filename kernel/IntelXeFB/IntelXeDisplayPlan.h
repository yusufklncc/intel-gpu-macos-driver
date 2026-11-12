// SPDX-License-Identifier: MIT
/*
 * Contains material from Linux drivers/gpu/drm/i915/display (skl_watermark.c, intel_pmdemand.c, intel_display.c,
 * intel_dp.c, intel_bw.c, intel_cdclk.c, intel_display_power_map.c) and drivers/gpu/drm/display/drm_dp_helper.c,
 * Copyright © 2006-2024 Intel Corporation, MIT licence (notices in kernel/IntelXeFB/LICENSE.linux-drm).
 * Copyright (c) 2026 Yusuf İhsan Kılınç (this adaptation)
 */
// IntelXeDisplayPlan: the arithmetic a second display pipe needs on display 14.01 (Xe2_HPD, the Arc B580), as Linux
// v6.18 computes it: port/PHY/AUX/HPD mapping, DP link choice and M/N, transcoder timing registers, DBUF slices and
// MBUS joining, the DDB split per pipe and per plane, plane watermarks (with the SAGV and transition watermarks and the
// vblank check), PMDEMAND, CDCLK and memory bandwidth. Header only, no allocation, no kernel or libc calls: the same
// code builds into IntelXeFB.kext and into the host test (tests/plan_test.cpp). Every function names the Linux function
// it follows; a "display 14" remark means a branch of that function taken for DISPLAY_VER 14 / BMG (dGPU), the others
// are left out. Register values are computed, never read or written, here: the caller passes live registers in
// (LiveState) and programs what comes out.
#ifndef INTELXEDISPLAYPLAN_H
#define INTELXEDISPLAYPLAN_H

#include <stdint.h>

namespace intelxeplan {

// ---- small helpers (include/linux/math.h) ---------------------------------------------------------------------------
constexpr uint32_t divRoundUp(uint64_t n, uint64_t d) { return (uint32_t)((n + d - 1) / d); }
constexpr uint32_t divRoundClosest(uint64_t n, uint64_t d) { return (uint32_t)((n + d / 2) / d); }
constexpr uint32_t umin(uint32_t a, uint32_t b) { return a < b ? a : b; }
constexpr uint32_t umax(uint32_t a, uint32_t b) { return a > b ? a : b; }
constexpr int popcount8(uint32_t v) { return (int)((v & 1) + ((v >> 1) & 1) + ((v >> 2) & 1) + ((v >> 3) & 1) + ((v >> 4) & 1) +
                                                   ((v >> 5) & 1) + ((v >> 6) & 1) + ((v >> 7) & 1)); }
constexpr int ffs8(uint32_t v) { for (int i = 0; i < 8; i++) if (v & (1u << i)) return i + 1; return 0; }
constexpr int fls8(uint32_t v) { for (int i = 7; i >= 0; i--) if (v & (1u << i)) return i + 1; return 0; }

// ---- ports (intel_display_device.c xe2_hpd_display, intel_display.c intel_port_to_phy, intel_dp_aux_regs.h,
//      intel_hotplug_irq.c hpd_mtp, intel_ddi.c intel_ddi_enable_transcoder_clock) -----------------------------------
// enum port: A 0, TC1 3 .. TC4 6; BMG's port_mask is A + TC1-TC4. All five are C20 PHYs (intel_encoder_is_c10phy: false
// on BMG), none Type-C (intel_phy_is_tc: false on a dGPU).
constexpr int kPortA = 0, kPortTc1 = 3, kNumPorts = 5;
constexpr int kPorts[kNumPorts] = { 0, 3, 4, 5, 6 };
struct Port {
	int port;           // enum port
	int phy;            // intel_port_to_phy, display 13+: A -> PHY_A (0), TCn -> PHY_F + n - 1 (5..8)
	uint32_t aux;       // AUX_CH_CTL: A 0x64010, USBC n 0x16F210 + (n - 1) * 0x200 (no xe2lpd remap below display 20)
	uint32_t hpd;       // SDE hotplug pin: DDI A bit 16, TC n bit 23 + n (SDE_TC_HOTPLUG_ICP)
	uint32_t ddiSel;    // TRANS_DDI_FUNC_CTL 30:27 = port + 1 (TRANS_DDI_SELECT_PORT)
	uint32_t clkSel;    // TRANS_CLK_SEL = TGL_TRANS_CLK_SEL_PORT(phy) = (phy + 1) << 28 (display 13+)
	const char *name;
};
inline bool portInfo(int port, Port *p)
{
	static const char *const kNames[] = { "A", "B", "C", "TC1", "TC2", "TC3", "TC4" };
	if (port != kPortA && (port < kPortTc1 || port > kPortTc1 + 3)) return false;
	p->port = port;
	p->phy = port == kPortA ? 0 : 5 + port - kPortTc1;
	p->aux = port == kPortA ? 0x64010u : 0x16F210u + (uint32_t)(port - kPortTc1) * 0x200u;
	p->hpd = port == kPortA ? 1u << 16 : 1u << (24 + port - kPortTc1);
	p->ddiSel = (uint32_t)port + 1;
	p->clkSel = (uint32_t)(p->phy + 1) << 28;
	p->name = kNames[port];
	return true;
}
// "a", "tc1".."tc4" (case-insensitive) -> enum port, -1 otherwise
inline int portFromName(const char *s)
{
	auto lc = [](char c) { return (char)(c >= 'A' && c <= 'Z' ? c + 32 : c); };
	if (!s) return -1;
	if (lc(s[0]) == 'a' && !s[1]) return kPortA;
	if (lc(s[0]) == 't' && lc(s[1]) == 'c' && s[2] >= '1' && s[2] <= '4' && !s[3]) return kPortTc1 + s[2] - '1';
	return -1;
}
// the power well of a pipe (intel_display_power_map.c xelpdp_power_wells_main): PW_A..PW_D = HSW_PWR_WELL_CTL2 index
// 5..8 (XELPD_PW_CTL_IDX_PW_A..D), request 2 << 2i, state 1 << 2i; the fuse of power gate idx + 1 is SKL_FUSE_STATUS
// bit 27 - (idx + 1) (pw_idx_to_pg, gen9_wait_for_power_well_fuses)
constexpr int pipePowerWell(int pipe) { return 5 + pipe; }
constexpr uint32_t pwReq(int idx) { return 2u << (idx * 2); }
constexpr uint32_t pwState(int idx) { return 1u << (idx * 2); }
constexpr uint32_t pwFuse(int idx) { return 1u << (27 - (idx + 1)); }

// ---- the mode -------------------------------------------------------------------------------------------------------
// drm_display_mode's crtc_* fields of a progressive mode (crtc_hblank_start = hdisplay, crtc_hblank_end = htotal)
struct Timing {
	uint32_t clock;   // kHz
	uint32_t hdisplay, hsyncStart, hsyncEnd, htotal;
	uint32_t vdisplay, vsyncStart, vsyncEnd, vtotal;
	bool phsync, pvsync;
};
inline bool timingSane(const Timing &t)
{
	return t.clock >= 10000 && t.hdisplay >= 64 && t.hdisplay <= t.hsyncStart && t.hsyncStart < t.hsyncEnd &&
	       t.hsyncEnd <= t.htotal && t.htotal <= 16384 && t.vdisplay >= 64 && t.vdisplay <= t.vsyncStart &&
	       t.vsyncStart < t.vsyncEnd && t.vsyncEnd <= t.vtotal && t.vtotal <= 16384;
}
inline uint32_t refreshMilliHz(const Timing &t) { return (t.htotal && t.vtotal) ? (uint32_t)((uint64_t)t.clock * 1000000ull / ((uint64_t)t.htotal * t.vtotal)) : 0; }

// ---- the second framebuffer while no sink drives it (docs/design/DESIGN-second-pipe.md §3, §4) ------------------------
// IntelXeFramebufferPipe is published at IntelXeFB's start, disconnected, as Apple's drivers publish every framebuffer of
// a GPU at start (DESIGN-multihead-accel.md §2). Not a Linux function: IOGraphics' model (IOFramebuffer.cpp updateOnline,
// processConnectChange; IONDRVFramebuffer.cpp kConnectionCheckEnable / searchOfflineMode).
// The placeholder it describes before a first sink: CEA-861 1920x1080@60 (VIC 16), never scanned out. Its only users are
// IOGraphics' mode queries and NMXAGDC's K5 answers for the head (NVRMTiming), which refuse a head without a sane timing.
inline Timing offlineTiming() { return { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true }; }
// Its one display mode: the sink's (1) while connected, else the offline mode (2), whose timing is
// kIOTimingIDApple_0x0_0hz_Offline. IOFramebuffer::updateOnline picks that mode as offlineMode when kConnectionCheckEnable
// reads 0, and processConnectChange sets it when the display goes (as an NDRV that lists timingApple_0x0_0hz_Offline).
constexpr int32_t kSecondModeSink = 1, kSecondModeOffline = 2;
constexpr int32_t secondFbMode(bool connected) { return connected ? kSecondModeSink : kSecondModeOffline; }
constexpr bool secondFbModeValid(int32_t mode) { return mode == kSecondModeSink || mode == kSecondModeOffline; }
// whether a newly lit sink has the last one's timing: field by field (a byte compare would read Timing's padding)
constexpr bool sameTiming(const Timing &a, const Timing &b)
{
	return a.clock == b.clock && a.hdisplay == b.hdisplay && a.hsyncStart == b.hsyncStart && a.hsyncEnd == b.hsyncEnd &&
	       a.htotal == b.htotal && a.vdisplay == b.vdisplay && a.vsyncStart == b.vsyncStart && a.vsyncEnd == b.vsyncEnd &&
	       a.vtotal == b.vtotal && a.phsync == b.phsync && a.pvsync == b.pvsync;
}
// Whether IOGraphics is told (the connect interrupt, kIOFBConnectInterruptType): the connection changed, or a connected
// head now drives another sink (other EDID or timing). Nothing for a repeat of the same state (the stop with nothing
// connected, a wake with the same monitor).
constexpr bool secondFbReport(bool wasConnected, bool nowConnected, bool sinkChanged)
{
	return wasConnected != nowConnected || (nowConnected && sinkChanged);
}

// intel_set_transcoder_timings (display 13+: TRANS_SET_CONTEXT_LATENCY = vblank_start - vdisplay, VBLANK start field
// written as 1 - 1), intel_set_pipe_src_size (PIPESRC: width - 1 << 16 | height - 1), TRANS_MULT = pixel multiplier - 1,
// and the sync polarity bits intel_ddi_transcoder_func_reg_val_get adds (TRANS_DDI_PHSYNC 16, TRANS_DDI_PVSYNC 17).
struct TimingRegs { uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, vsyncshift, scl, mult, pipesrc, funcSync; };
inline TimingRegs timingRegs(const Timing &t)
{
	TimingRegs r = {};
	const uint32_t vblankStart = t.vdisplay;   // crtc_vblank_start + intel_crtc_vblank_delay (no PSR: 0)
	r.scl = vblankStart - t.vdisplay;
	r.vsyncshift = 0;                           // progressive
	r.htotal = (t.hdisplay - 1) | (t.htotal - 1) << 16;
	r.hblank = (t.hdisplay - 1) | (t.htotal - 1) << 16;
	r.hsync = (t.hsyncStart - 1) | (t.hsyncEnd - 1) << 16;
	r.vtotal = (t.vdisplay - 1) | (t.vtotal - 1) << 16;
	r.vblank = (1u - 1) | (t.vtotal - 1) << 16;
	r.vsync = (t.vsyncStart - 1) | (t.vsyncEnd - 1) << 16;
	r.mult = 0;
	r.pipesrc = (t.hdisplay - 1) << 16 | (t.vdisplay - 1);
	r.funcSync = (t.phsync ? 1u << 16 : 0) | (t.pvsync ? 1u << 17 : 0);
	return r;
}
// intel_get_transcoder_timings for display 13+ (the reverse of the above; clock from the caller)
inline Timing timingFromRegs(uint32_t htotal, uint32_t hsync, uint32_t vtotal, uint32_t vsync, uint32_t funcCtl, uint32_t clock)
{
	Timing t = {};
	t.clock = clock;
	t.hdisplay = (htotal & 0xFFFF) + 1; t.htotal = (htotal >> 16) + 1;
	t.hsyncStart = (hsync & 0xFFFF) + 1; t.hsyncEnd = (hsync >> 16) + 1;
	t.vdisplay = (vtotal & 0xFFFF) + 1; t.vtotal = (vtotal >> 16) + 1;
	t.vsyncStart = (vsync & 0xFFFF) + 1; t.vsyncEnd = (vsync >> 16) + 1;
	t.phsync = funcCtl & (1u << 16); t.pvsync = funcCtl & (1u << 17);
	return t;
}

// ---- DP link (intel_dp.c, drm_dp_helper.c, intel_display.c) -------------------------------------------------------
// 8b/10b only: the C20 contexts IntelXeLink has (xe2hpd_c20_dp_tables RBR..HBR3); UHBR (128b/132b) is out of scope
constexpr uint32_t kRates[4] = { 162000, 270000, 540000, 810000 };
// DPCD MAX_LINK_RATE (0x001, x 0.27 Gbps) -> kHz link rate (drm_dp_bw_code_to_link_rate)
constexpr uint32_t rateFromBwCode(uint8_t bw) { return (uint32_t)bw * 27000u; }
// drm_dp_max_dprx_data_rate for 8b/10b: rate x 10 x lanes x 0.8 / 8 (kB/s)
constexpr uint32_t maxDataRate(uint32_t rate, uint32_t lanes) { return (uint32_t)((uint64_t)rate * 10u * lanes * 800000u / (1000000ull * 8u)); }
// intel_dp_link_required: kHz x bpp / 8 (kB/s)
constexpr uint32_t linkRequired(uint32_t clock, uint32_t bpp) { return divRoundUp((uint64_t)clock * bpp, 8); }
struct Link { uint32_t rate, lanes, bpp; };
// intel_dp_compute_link_config_wide at one bpp: the lowest common rate, then the lowest lane count, that carries the mode.
// The common rates are the source's (8b/10b ones here) up to the sink's DPCD maximum; lanes up to min(4, the sink's).
inline bool linkConfig(uint32_t clock, uint32_t bpp, uint32_t sinkMaxRate, uint32_t sinkMaxLanes, Link *out)
{
	const uint32_t need = linkRequired(clock, bpp);
	for (uint32_t rate : kRates) {
		if (rate > sinkMaxRate) continue;
		for (uint32_t lanes = 1; lanes <= 4 && lanes <= sinkMaxLanes; lanes <<= 1)
			if (need <= maxDataRate(rate, lanes)) { *out = { rate, lanes, bpp }; return true; }
	}
	return false;
}
// intel_link_compute_m_n (SST, no FEC: bw_overhead 1000000): data M/N = effective data rate / max DPRX data rate with
// N fixed at 0x8000000, link M/N = pixel clock / link symbol clock (= rate for 8b/10b) with N 0x80000, TU 64;
// compute_m_n halves both until they fit 24 bits (intel_reduce_m_n_ratio, DATA_LINK_M_N_MASK)
struct MN { uint32_t tu, dataM, dataN, linkM, linkN; };
inline void computeMN(uint32_t m, uint32_t n, uint32_t constantN, uint32_t *rm, uint32_t *rn)
{
	*rn = constantN;
	uint64_t mm = (uint64_t)m * constantN / n;
	uint64_t nn = constantN;
	while (mm > 0xFFFFFF || nn > 0xFFFFFF) { mm >>= 1; nn >>= 1; }
	*rm = (uint32_t)mm; *rn = (uint32_t)nn;
}
inline MN linkMN(uint32_t bpp, uint32_t lanes, uint32_t clock, uint32_t rate)
{
	MN r = {};
	r.tu = 64;
	const uint32_t dataM = divRoundUp((uint64_t)clock * (bpp * 16) * 1000000ull, 1000000ull * 16 * 8);   // intel_dp_effective_data_rate
	const uint32_t dataN = maxDataRate(rate, lanes);
	computeMN(dataM, dataN, 0x8000000, &r.dataM, &r.dataN);
	computeMN(clock, rate, 0x80000, &r.linkM, &r.linkN);   // intel_dp_link_symbol_clock(rate) = rate for 8b/10b
	return r;
}
// TRANS_DATA_M1 (TU_SIZE 30:25 = tu - 1 | data M), TRANS_DATA_N1, TRANS_LINK_M1, TRANS_LINK_N1 (intel_set_m_n)
constexpr uint32_t dataMReg(const MN &m) { return (m.tu - 1) << 25 | m.dataM; }
// hsw_linetime_wm: line time in 1/8 us, max 0x1FF (WM_LINETIME 8:0)
constexpr uint32_t linetimeWm(uint32_t htotal, uint32_t clock) { return umin(divRoundClosest((uint64_t)htotal * 1000 * 8, clock), 0x1FF); }
// intel_pixel_rate_to_cdclk: 2 pixels per clock (HAS_2PPC, display 14), guardband 100 %
constexpr uint32_t minCdclk(uint32_t pixelRate) { return divRoundUp((uint64_t)pixelRate * 100, 100 * 2); }
// intel_bw_crtc_min_cdclk: "Maximum Pipe Read Bandwidth", data rate (kB/s) x 10 / 512
constexpr uint32_t bwMinCdclk(uint64_t dataRate) { return divRoundUp(dataRate * 10, 512); }
constexpr uint32_t kBmgCdclk = 652800;   // xe2hpd_cdclk_table: the only entry
// bdw_set_pipe_misc at 24 bpp, RGB, no dither (display 12+: PIPE_MISC_PIXEL_ROUNDING_TRUNC, bit 8; BPC_8 = 0)
constexpr uint32_t kPipeMisc = 1u << 8;
// icl_set_pipe_chicken (read-modify-write): PER_PIXEL_ALPHA_BYPASS_EN 7, PIXEL_ROUNDING_TRUNC_FB_PASSTHRU 15,
// UNDERRUN_RECOVERY_DISABLE_ADLP 30 (display 13-29)
constexpr uint32_t kPipeChickenSet = 1u << 7 | 1u << 15 | 1u << 30;
// intel_ddi_set_dp_msa at 24 bpp, full-range RGB: DP_MSA_MISC_SYNC_CLOCK | DP_MSA_MISC_8_BPC
constexpr uint32_t kMsaMisc = 1u << 0 | 1u << 5;

// ---- DBUF slices and MBUS (skl_watermark.c) -------------------------------------------------------------------------
constexpr int kNumSlices = 4;
constexpr uint32_t kDbufSize = 4096, kSliceSize = kDbufSize / kNumSlices;   // XE_LPDP_FEATURES .dbuf
constexpr uint32_t DBUF_CTL_S(int slice) { return slice == 0 ? 0x45008u : slice == 1 ? 0x44FE8u : slice == 2 ? 0x44300u : 0x44304u; }
constexpr uint32_t DBUF_POWER_REQUEST = 1u << 31, DBUF_POWER_STATE = 1u << 30;
constexpr uint32_t DBUF_MIN_TRACKER_MASK = 7u << 16;
constexpr uint32_t MBUS_CTL = 0x4438C, MBUS_JOIN = 1u << 31, MBUS_HASHING_1x4 = 1u << 30, MBUS_PIPE_SELECT_MASK = 7u << 26;
constexpr uint32_t PIPE_MBUS_DBOX_CTL(int pipe) { return 0x7003Cu + (uint32_t)pipe * 0x1000u; }
// adlp_allowed_dbufs (display 13+ but DG2): active pipe mask -> per-pipe slice masks, join_mbus. The joined entries
// come first so check_mbus_joined prefers them.
struct DbufConf { uint8_t pipes; uint8_t mask[4]; bool join; };
constexpr uint8_t S1 = 1, S2 = 2, S3 = 4, S4 = 8;
constexpr DbufConf kAdlpDbufs[] = {
	{ 1, { S1 | S2 | S3 | S4, 0, 0, 0 }, true },
	{ 2, { 0, S1 | S2 | S3 | S4, 0, 0 }, true },
	{ 1, { S1 | S2, 0, 0, 0 }, false },
	{ 2, { 0, S3 | S4, 0, 0 }, false },
	{ 3, { S1 | S2, S3 | S4, 0, 0 }, false },
	{ 4, { 0, 0, S3 | S4, 0 }, false },
	{ 5, { S1 | S2, 0, S3 | S4, 0 }, false },
	{ 6, { 0, S3 | S4, S3 | S4, 0 }, false },
	{ 7, { S1 | S2, S3 | S4, S3 | S4, 0 }, false },
	{ 8, { 0, 0, 0, S1 | S2 }, false },
	{ 9, { S1 | S2, 0, 0, S1 | S2 }, false },
	{ 10, { 0, S3 | S4, 0, S1 | S2 }, false },
	{ 11, { S1 | S2, S3 | S4, 0, S1 | S2 }, false },
	{ 12, { 0, 0, S3 | S4, S1 | S2 }, false },
	{ 13, { S1 | S2, 0, S3 | S4, S1 | S2 }, false },
	{ 14, { 0, S3 | S4, S3 | S4, S1 | S2 }, false },
	{ 15, { S1 | S2, S3 | S4, S3 | S4, S1 | S2 }, false },
};
// adlp_check_mbus_joined -> check_mbus_joined: the first entry for these pipes
inline bool mbusJoined(uint8_t activePipes)
{
	for (const DbufConf &c : kAdlpDbufs) if (c.pipes == activePipes) return c.join;
	return false;
}
// adlp_compute_dbuf_slices -> compute_dbuf_slices: the entry for these pipes and this join state
inline uint8_t dbufSlices(int pipe, uint8_t activePipes, bool joined)
{
	if (pipe < 0 || pipe > 3) return 0;
	for (const DbufConf &c : kAdlpDbufs) if (c.pipes == activePipes && c.join == joined) return c.mask[pipe];
	return 0;
}
// intel_dbuf_enabled_slices: the union of the active pipes' slices
inline uint8_t enabledSlices(uint8_t activePipes, bool joined)
{
	uint8_t m = 0;
	for (int p = 0; p < 4; p++) if (activePipes & (1u << p)) m = (uint8_t)(m | dbufSlices(p, activePipes, joined));
	return m;
}
struct Ddb { uint32_t start, end; uint32_t size() const { return end > start ? end - start : 0; } };
// skl_ddb_entry_for_slices: from the first slice to the end of the last one
inline Ddb ddbForSlices(uint8_t mask)
{
	if (!mask) return { 0, 0 };
	return { (uint32_t)(ffs8(mask) - 1) * kSliceSize, (uint32_t)fls8(mask) * kSliceSize };
}
// mbus_ddb_offset: PLANE_BUF_CFG is relative to the MBUS bank (S1/S2 -> 0, S3/S4 -> 2048)
inline uint32_t mbusDdbOffset(uint8_t mask)
{
	if (mask & (S1 | S2)) mask = S1;
	else if (mask & (S3 | S4)) mask = S3;
	return ddbForSlices(mask).start;
}
// skl_crtc_allocate_ddb: the pipe's share of its slices, weighted by hdisplay (intel_crtc_ddb_weight) among the pipes
// with the same slice set (intel_crtc_dbuf_weights); relative to the MBUS bank. weight[p] = 0 for inactive pipes.
inline Ddb pipeDdb(int pipe, uint8_t activePipes, bool joined, const uint32_t weight[4], uint32_t *mbusOffset)
{
	*mbusOffset = 0;
	if (!(activePipes & (1u << pipe)) || !weight[pipe]) return { 0, 0 };
	const uint8_t mask = dbufSlices(pipe, activePipes, joined);
	const Ddb slices = ddbForSlices(mask);
	*mbusOffset = mbusDdbOffset(mask);
	uint32_t ws = 0, we = 0, wt = 0;
	for (int p = 0; p < 4; p++) {
		if (!(activePipes & (1u << p)) || dbufSlices(p, activePipes, joined) != mask) continue;
		wt += weight[p];
		if (p < pipe) { ws += weight[p]; we += weight[p]; }
		else if (p == pipe) we += weight[p];
	}
	const uint32_t size = slices.size();
	return { slices.start - *mbusOffset + size * ws / wt, slices.start - *mbusOffset + size * we / wt };
}
// pipe_mbus_dbox_ctl, display 14 (14.01: no MTL BW credits, those are DISPLAY_VERx100 == 1400 only)
inline uint32_t mbusDboxCtl(bool joined)
{
	uint32_t v = 2u << 5;                              // MBUS_DBOX_I_CREDIT(2)
	v |= 16u << 20 | 1u << 17 | 1u << 16;              // B2B_TRANSACTIONS_MAX(16), _DELAY(1), REGULATE_B2B_TRANSACTIONS_EN
	v |= joined ? 12u : 8u;                            // MBUS_DBOX_A_CREDIT
	v |= 0xAu << 8;                                    // MBUS_DBOX_B_CREDIT(0xA)
	return v;
}
// mbus_ctl_join_update: the MBUS_CTL bits under MBUS_HASHING_MODE_MASK | MBUS_JOIN | MBUS_JOIN_PIPE_SELECT_MASK
inline uint32_t mbusCtlBits(bool joined, int pipe)
{
	return (joined ? MBUS_HASHING_1x4 | MBUS_JOIN : 0u) | (uint32_t)(pipe >= 0 ? pipe : 7) << 26;
}
// intel_dbuf_mdclk_cdclk_ratio_update: DBUF_CTL_S* MIN_TRACKER_STATE_SERVICE = ratio - 1; display 14's MDCLK is CD2XCLK
// (intel_mdclk_cdclk_ratio: 2), doubled while MBUS is joined
constexpr uint32_t dbufMinTracker(bool joined) { return (joined ? 4u : 2u) - 1u; }

// ---- watermarks (skl_watermark.c; i915_fixed.h for the 16.16 arithmetic) -------------------------------------------
struct Fx { uint32_t val; };
constexpr Fx fxFromU32(uint32_t v) { return { v << 16 }; }
constexpr uint32_t fxRoundUp(Fx f) { return (uint32_t)(((uint64_t)f.val + 0xFFFF) >> 16); }
constexpr Fx fxDiv(uint32_t v, uint32_t d) { return { (uint32_t)((((uint64_t)v << 16) + d - 1) / d) }; }
constexpr Fx fxAddU32(Fx a, uint32_t b) { return { (uint32_t)((uint64_t)a.val + ((uint64_t)b << 16)) }; }
constexpr Fx fxMulU32(uint32_t v, Fx m) { return { (uint32_t)((uint64_t)v * m.val) }; }
constexpr uint32_t fxDivRoundUp(Fx a, Fx d) { return (uint32_t)(((uint64_t)a.val + d.val - 1) / d.val); }
constexpr uint32_t mulRoundUpU32Fx(uint32_t v, Fx m) { return (uint32_t)(((uint64_t)v * m.val + 0xFFFF) >> 16); }
constexpr int kWmLevels = 8;   // skl_setup_wm_latency: !HAS_HW_SAGV_WM (BMG is a dGPU) -> 8 levels
// The latencies in us (display->wm.skl_latency) and the SAGV block time
struct Latency { uint16_t us[kWmLevels]; uint32_t sagvUs; };
// mtl_read_wm_latency (MTL_LATENCY_LP0_LP1/LP2_LP3/LP4_LP5: even level 12:0, odd 28:16; levels 6-7 stay 0),
// adjust_wm_latency (a 0 at level n >= 1 zeroes the levels above; level 0 reading 0 adds the read latency 6 to the
// valid levels; no 16 Gb DIMM step: display 14 reads no DIMM information), intel_sagv_block_time (MTL_LATENCY_SAGV
// 12:0; intel_sagv_init: 0 without SAGV control -- BMG has it once xe2_hpd_get_bw_info read the QGV points)
inline Latency latencyFromRegs(uint32_t lp01, uint32_t lp23, uint32_t lp45, uint32_t sagv)
{
	Latency l = {};
	const uint32_t r[3] = { lp01, lp23, lp45 };
	for (int i = 0; i < 3; i++) { l.us[2 * i] = (uint16_t)(r[i] & 0x1FFF); l.us[2 * i + 1] = (uint16_t)((r[i] >> 16) & 0x1FFF); }
	int levels = kWmLevels;
	for (int level = 1; level < kWmLevels; level++)
		if (l.us[level] == 0) { for (int i = level + 1; i < kWmLevels; i++) l.us[i] = 0; levels = level; break; }
	if (l.us[0] == 0) for (int level = 0; level < levels; level++) l.us[level] = (uint16_t)(l.us[level] + 6);
	l.sagvUs = sagv & 0x1FFF;
	return l;
}
// skl_wm_params for a linear plane (not X/Y tiled, not planar, no rotation, display >= 10): 512-byte DBUF blocks
struct WmParams { uint32_t width, cpp, pixelRate, htotal, bytesPerLine, blockSize, linetimeUs; Fx blocksPerLine; };
inline WmParams wmParams(uint32_t width, uint32_t cpp, uint32_t pixelRate, uint32_t htotal)
{
	WmParams p = {};
	p.width = width; p.cpp = cpp; p.pixelRate = pixelRate; p.htotal = htotal;
	p.blockSize = 512;
	p.bytesPerLine = width * cpp;
	p.blocksPerLine = fxFromU32(divRoundUp(p.bytesPerLine, p.blockSize) + 1);   // linear: interm_pbpl++ (display >= 10)
	p.linetimeUs = fxRoundUp(fxDiv(htotal * 1000, pixelRate));                // intel_get_linetime_us
	return p;
}
struct WmLevel { bool enable; uint16_t blocks, lines, minDdb; };
constexpr uint16_t kRejected = 0xFFFF;   // min_ddb_alloc = U16_MAX: the level cannot be used
// skl_compute_plane_wm for a linear plane on display 14 (method1 + 1 on display >= 10, blocks + 1 below display 30,
// lines on every level, min_ddb_alloc = blocks + blocks / 10 rounded up on display >= 11, at most 255 lines on 13+)
inline WmLevel planeWm(const WmParams &p, uint32_t latency)
{
	WmLevel r = {};
	if (latency == 0) { r.minDdb = kRejected; return r; }
	const Fx m1 = fxAddU32(fxDiv(latency * p.pixelRate * p.cpp, 1000 * p.blockSize), 1);             // skl_wm_method1
	const Fx m2 = fxMulU32(divRoundUp((uint64_t)latency * p.pixelRate, (uint64_t)p.htotal * 1000), p.blocksPerLine);   // skl_wm_method2
	Fx sel;
	if (p.cpp * p.htotal / p.blockSize < 1 && p.bytesPerLine / p.blockSize < 1) sel = m2;
	else if (latency >= p.linetimeUs) sel = m2;
	else sel = m1;
	uint32_t blocks = fxRoundUp(sel) + 1;
	blocks = umax(blocks, fxRoundUp(p.blocksPerLine));
	const uint32_t lines = fxDivRoundUp(sel, p.blocksPerLine);
	const uint32_t minDdb = blocks + divRoundUp(blocks, 10);
	if (lines > 255) { r.minDdb = kRejected; return r; }
	r.blocks = (uint16_t)blocks; r.lines = (uint16_t)lines;
	r.minDdb = (uint16_t)(umax(minDdb, blocks) + 1);
	r.enable = true;
	return r;
}
// skl_compute_transition_wm with IPC on (has_ipc, skl_watermark_ipc_can_enable): trans_min 4 + amount 10 (display >= 11)
inline WmLevel transWm(const WmLevel &wm0)
{
	WmLevel t = {};
	const uint16_t wm0Blocks = (uint16_t)(wm0.blocks - 1);
	uint16_t blocks = (uint16_t)(wm0Blocks + 14);
	blocks++;
	t.blocks = blocks;
	t.minDdb = (uint16_t)umax(wm0.minDdb, (uint32_t)blocks + 1);
	t.enable = true;
	return t;
}
// skl_plane_wm_reg_val: PLANE_WM_EN 31, lines 26:14, blocks 11:0 (also CUR_WM)
constexpr uint32_t wmReg(const WmLevel &l) { return (l.enable ? 1u << 31 : 0u) | ((uint32_t)l.lines & 0x1FFF) << 14 | ((uint32_t)l.blocks & 0xFFF); }
// skl_plane_ddb_reg_val: PLANE_BUF_CFG / CUR_BUF_CFG = end - 1 in 27:16, start in 11:0, 0 for an empty entry
constexpr uint32_t ddbReg(const Ddb &d) { return d.end ? ((d.end - 1) & 0xFFF) << 16 | (d.start & 0xFFF) : 0u; }
// skl_ddb_entry_init_from_hw
inline Ddb ddbFromReg(uint32_t v) { Ddb d = { v & 0xFFF, (v >> 16) & 0xFFF }; if (d.end) d.end++; return d; }
// skl_cursor_allocation: the 256-pixel ARGB cursor's minimum over the usable levels, at least 32 blocks with one active
// pipe and 8 with more
inline uint32_t cursorAlloc(uint32_t pixelRate, uint32_t htotal, const Latency &lat, int numActive)
{
	const WmParams p = wmParams(256, 4, pixelRate, htotal);
	uint32_t minDdb = 0;
	for (int level = 0; level < kWmLevels; level++) {
		const WmLevel w = planeWm(p, lat.us[level]);
		if (w.minDdb == kRejected) break;
		minDdb = w.minDdb;
	}
	return umax(numActive == 1 ? 32u : 8u, minDdb);
}
// One pipe with plane 1 (linear XRGB8888, unscaled) and the cursor (off: no watermarks, its DDB block kept)
struct PipeWm {
	WmLevel wm[kWmLevels], trans, sagv0, sagvTrans;
	int vblankMaxLevel;   // skl_wm_check_vblank: the highest level the vblank allows (-1: none)
	int ddbLevel;         // skl_crtc_allocate_plane_ddb: the highest level whose blocks fit (-1: none)
	bool useSagv;         // use_sagv_wm: level 0 / transition registers carry the SAGV watermarks
	Ddb plane, cursor;
	uint32_t cursorBlocks;
	uint32_t regWm[kWmLevels], regTrans, regBufCfg, regCurBufCfg;
};
// intel_usecs_to_scanlines
constexpr uint32_t usToLines(const Timing &t, uint32_t us) { return t.htotal ? divRoundUp((uint64_t)us * t.clock, 1000ull * t.htotal) : 0; }
// skl_is_vblank_too_short: framestart_delay 1 + latency in lines (no scaler, no DSC prefill) + wm0 lines > vblank
inline bool vblankTooShort(const Timing &t, uint32_t wm0Lines, uint32_t latency)
{
	return 1 + usToLines(t, latency) + wm0Lines > t.vtotal - t.vdisplay;
}
// skl_build_pipe_wm (icl_build_plane_wm -> skl_build_plane_wm_single: levels, transition, tgl_compute_sagv_wm and its
// transition), skl_wm_check_vblank, skl_crtc_allocate_plane_ddb (cursor block at the end, then the highest level that
// fits, the rest to plane 1 by data rate), the checks against the plane's DDB, the copy WA (skl_need_wm_copy_wa), and
// use_sagv_wm (tgl_crtc_can_enable_sagv for a new pipe: no plane with WM0 enabled and its SAGV WM0 disabled).
// alloc: the pipe's DDB (skl_crtc_allocate_ddb, MBUS-relative). False: no level fits ("exceeds system DDB
// limitations") or the vblank allows no level.
inline bool pipeWm(const Timing &t, uint32_t planeW, const Latency &lat, const Ddb &alloc, int numActive, PipeWm *o)
{
	*o = {};
	const WmParams p = wmParams(planeW, 4, t.clock, t.htotal);   // pixel_rate = crtc_clock (no pfit), plane rate = pixel rate
	for (int level = 0; level < kWmLevels; level++) o->wm[level] = planeWm(p, lat.us[level]);
	o->trans = transWm(o->wm[0]);
	o->sagv0 = planeWm(p, lat.sagvUs ? lat.sagvUs + lat.us[0] : 0);
	o->sagvTrans = transWm(o->sagv0);
	// skl_wm_check_vblank (level 0 counts with latency 0)
	const uint32_t wm0Lines = o->wm[0].lines;
	o->vblankMaxLevel = -1;
	for (int level = kWmLevels - 1; level >= 0; level--) {
		uint32_t l = lat.us[level];
		if (l == 0) continue;
		if (level == 0) l = 0;
		if (!vblankTooShort(t, wm0Lines, l)) { o->vblankMaxLevel = level; break; }
	}
	if (o->vblankMaxLevel < 0) return false;
	for (int level = o->vblankMaxLevel + 1; level < kWmLevels; level++) o->wm[level].enable = false;
	if (lat.sagvUs && vblankTooShort(t, wm0Lines, lat.sagvUs)) { o->sagv0.enable = false; o->sagvTrans.enable = false; }
	// skl_crtc_allocate_plane_ddb
	uint32_t start = alloc.start, size = alloc.size();
	if (!size) return false;
	o->cursorBlocks = cursorAlloc(t.clock, t.htotal, lat, numActive);
	if (o->cursorBlocks >= size) return false;
	size -= o->cursorBlocks;
	o->cursor = { alloc.end - o->cursorBlocks, alloc.end };
	int level = kWmLevels - 1;
	for (; level >= 0; level--) {   // the cursor is off: its watermarks are 0 and always fit
		const uint32_t blocks = o->wm[level].minDdb;
		if (blocks <= size) { size -= blocks; break; }
	}
	o->ddbLevel = level;
	if (level < 0) return false;
	o->plane = { start, start + o->wm[level].minDdb + size };   // the extra blocks all go to plane 1 (the only data rate)
	for (int l = level + 1; l < kWmLevels; l++) {
		if (o->wm[l].minDdb > o->plane.size()) o->wm[l] = {};   // skl_check_wm_level
		if (!o->wm[l].enable) { o->wm[l].blocks = o->wm[l - 1].blocks; o->wm[l].lines = o->wm[l - 1].lines; }   // copy WA
	}
	if (o->trans.minDdb > o->plane.size()) o->trans = {};
	if (o->sagv0.minDdb > o->plane.size()) o->sagv0 = {};
	if (o->sagvTrans.minDdb > o->plane.size()) o->sagvTrans = {};
	o->useSagv = !(o->wm[0].enable && !o->sagv0.enable);
	// skl_write_plane_wm: skl_plane_wm_level / skl_plane_trans_wm pick the SAGV ones for level 0 / transition
	for (int l = 0; l < kWmLevels; l++) o->regWm[l] = wmReg(l == 0 && o->useSagv ? o->sagv0 : o->wm[l]);
	o->regTrans = wmReg(o->useSagv ? o->sagvTrans : o->trans);
	o->regBufCfg = ddbReg(o->plane);
	o->regCurBufCfg = ddbReg(o->cursor);
	return true;
}

// ---- PMDEMAND (intel_pmdemand.c, intel_display_regs.h XELPDP_INITIATE_PMDEMAND_REQUEST) ----------------------------
constexpr uint32_t PMDEMAND0 = 0x45230, PMDEMAND1 = 0x45234, PMDEMAND_REQ = 1u << 31, DCPR_STATUS_1 = 0x46440, PMDEMAND_INFLIGHT = 1u << 26;
struct PmDemand { uint32_t qclkBw, voltage, qclkIdx, pipes, dbufs, phys, cdclkMhz, ddiclkMhz, scalers, plls; };
inline PmDemand pmdemandFromRegs(uint32_t r0, uint32_t r1)   // intel_pmdemand_init_pmdemand_params
{
	PmDemand p = {};
	p.qclkBw = r0 >> 16; p.voltage = (r0 >> 12) & 7; p.qclkIdx = (r0 >> 8) & 0xF;
	p.pipes = (r0 >> 6) & 3; p.dbufs = (r0 >> 4) & 3; p.phys = r0 & 7;
	p.cdclkMhz = (r1 >> 20) & 0x7FF; p.ddiclkMhz = (r1 >> 8) & 0x7FF; p.scalers = (r1 >> 4) & 7; p.plls = r1 & 7;
	return p;
}
// intel_pmdemand_update_params into the two request dwords (other bits as they were; REQ_ENABLE not set here)
inline void pmdemandToRegs(const PmDemand &p, uint32_t *r0, uint32_t *r1)
{
	*r0 = (*r0 & ~(0xFFFFu << 16 | 7u << 12 | 0xFu << 8 | 3u << 6 | 3u << 4 | 7u)) |
	      (p.qclkBw & 0xFFFF) << 16 | (p.voltage & 7) << 12 | (p.qclkIdx & 0xF) << 8 | (p.pipes & 3) << 6 | (p.dbufs & 3) << 4 | (p.phys & 7);
	*r1 = (*r1 & ~(0x7FFu << 20 | 0x7FFu << 8 | 7u << 4 | 7u | PMDEMAND_REQ)) |
	      (p.cdclkMhz & 0x7FF) << 20 | (p.ddiclkMhz & 0x7FF) << 8 | (p.scalers & 7) << 4 | (p.plls & 7);
}
// the pre-plane request: field by field the larger of the old and the new parameters (update_reg's max3)
inline PmDemand pmdemandMax(const PmDemand &a, const PmDemand &b)
{
	PmDemand r;
	r.qclkBw = umax(a.qclkBw, b.qclkBw); r.voltage = umax(a.voltage, b.voltage); r.qclkIdx = umax(a.qclkIdx, b.qclkIdx);
	r.pipes = umax(a.pipes, b.pipes); r.dbufs = umax(a.dbufs, b.dbufs); r.phys = umax(a.phys, b.phys);
	r.cdclkMhz = umax(a.cdclkMhz, b.cdclkMhz); r.ddiclkMhz = umax(a.ddiclkMhz, b.ddiclkMhz);
	r.scalers = umax(a.scalers, b.scalers); r.plls = umax(a.plls, b.plls);
	return r;
}
// intel_pmdemand_atomic_check for a pipe set (cdclk, voltage and the QGV request kept as they are): pipes and enabled
// DBUF slices capped at 3, every PHY counted (none is TC on BMG), PLLs = PHYs + 1 (the CDCLK PLL), DDICLK = the highest
// port clock in MHz rounded up, scalers always 7
inline PmDemand pmdemandFor(const PmDemand &cur, int pipes, uint8_t slices, int phys, uint32_t maxPortClock)
{
	PmDemand p = cur;
	p.pipes = umin((uint32_t)pipes, 3);
	p.dbufs = umin((uint32_t)popcount8(slices), 3);
	p.phys = umin((uint32_t)phys, 7);
	p.plls = umin(p.phys + 1, 7);
	p.ddiclkMhz = divRoundUp(maxPortClock, 1000);
	p.scalers = 7;
	return p;
}

// ---- memory bandwidth (intel_bw.c) ---------------------------------------------------------------------------------
constexpr uint32_t MTL_MEM_SS_INFO_GLOBAL = 0x45700;
constexpr uint32_t MTL_MEM_SS_INFO_QGV_POINT_LOW(int point) { return 0x45710u + (uint32_t)point * 8u; }
// xe2_hpd_get_bw_info: per QGV point bw = channels x (32 / 8) x dclk (MB/s; GDDR channel_width 32), derated by 30 %
// (xe2_hpd_sa_info; 45 % for GDDR with ECC, DDR type 9: xe2_hpd_ecc_sa_info), at most 53 GB/s (deprogbwlimit) and
// peak x DEPROGBWPCLIMIT / 10;
// mtl_read_qgv_point_info: dclk = round(16667 x field / 1000). Returns the best derated point (0: no information).
inline uint32_t deratedBwMBps(uint32_t global, const uint32_t *qgvLow, int maxPoints)
{
	const uint32_t channels = (global >> 4) & 0xF, points = (global >> 8) & 0xF;
	if (!channels || !points) return 0;
	uint32_t maxDclk = 0, best = 0;
	for (uint32_t i = 0; i < points && (int)i < maxPoints; i++) maxDclk = umax(maxDclk, divRoundClosest((uint64_t)16667 * (qgvLow[i] & 0xFFFF), 1000));
	const uint32_t peak = channels * 4 * maxDclk;
	const uint32_t maxDe = umin(53 * 1000, peak * 60 / 10);
	const uint32_t derating = (global & 0xF) == 9 ? 45 : 30;
	for (uint32_t i = 0; i < points && (int)i < maxPoints; i++) {
		const uint32_t bw = channels * 4 * divRoundClosest((uint64_t)16667 * (qgvLow[i] & 0xFFFF), 1000);
		best = umax(best, umin(maxDe, (100 - derating) * bw / 100));
	}
	return best;
}
// intel_plane_data_rate for an unscaled XRGB8888 plane: pixel rate x cpp (kB/s)
constexpr uint64_t dataRate(uint32_t pixelRate) { return (uint64_t)pixelRate * 4; }

// ---- the plan: pipe A as it is, pipe B as it would be --------------------------------------------------------------
// The registers the plan starts from (read by IntelXeFB; faked by the host test)
struct LiveState {
	int pipeA;                                      // the adopted pipe (0 on the B580)
	uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, scl, pipesrc, funcCtl, transconf;
	uint32_t dataM, dataN, linkM, linkN, linetime;
	uint32_t valfreq;                               // DDI_CLK_VALFREQ of pipe A's port (kHz)
	uint32_t planeCtl, planeSize, planeBufCfg, curBufCfg, curCtl;
	uint32_t planeWm[kWmLevels], planeWmTrans;
	uint32_t mbusCtl, dbox[4], dbufCtl[kNumSlices];
	uint32_t pmd0, pmd1, cdclkCtl;
	uint32_t lat01, lat23, lat45, latSagv;
	uint32_t memGlobal, qgvLow[8];
	uint32_t pwCtl2, transconfB;                     // power well requests, the second pipe's TRANSCONF
};
struct Request {
	int pipe;                                       // the second pipe (B)
	int port;                                       // enum port of its output
	Timing mode;
	uint32_t sinkMaxRate, sinkMaxLanes;             // DPCD 0x001 / 0x002 of the sink (or what the dry run assumes)
};
struct PipePlan {
	Timing t;
	Link link;
	MN mn;
	TimingRegs regs;
	uint32_t linetime;
	uint8_t slices;
	uint32_t mbusOffset;
	Ddb ddb;
	PipeWm wm;
	bool wmOk;
	uint32_t dbox;
	uint32_t minCdclk;
};
enum Err {
	kOk = 0, kBadPort, kPortInUse, kPipeInUse, kBadMode, kNoLink, kCdclk, kDdbA, kDdbB, kVblank, kBandwidth, kPipeA,
};
inline const char *errName(int e)
{
	static const char *const k[] = { "ok", "port is not a BMG DDI (A, TC1-TC4)", "port drives pipe A already",
	                                 "the second pipe is already on", "mode not usable (timings)",
	                                 "no 8b/10b link carries the mode at 24 bpp", "pixel rate above 2 x CDCLK",
	                                 "pipe A's DDB is in slices 3-4 (shrinking it is not implemented)",
	                                 "no watermark level of the second pipe fits its DDB", "vblank too short for any level",
	                                 "memory bandwidth", "pipe A's timing is not readable" };
	return e >= 0 && e <= kPipeA ? k[e] : "?";
}
struct Plan {
	int err;
	Port portA, portB;
	PipePlan a;            // pipe A from its live timing, single pipe (as Linux computes it; compare with the live registers)
	PipePlan a2;           // pipe A in the two-pipe layout as Linux would allocate it (not programmed: see aKeepsDdb)
	PipePlan b;
	bool joinedNow;        // MBUS_CTL JOIN now
	bool joinedAfter;
	Ddb aPlaneNow, aCursorNow;
	bool aKeepsDdb;        // pipe A's live DDB lies in its two-pipe slices: kept as it is
	uint8_t slicesNow, slicesAfter;
	uint32_t mbusCtlAfter;        // MBUS_CTL value after the update (rmw on the live value)
	uint32_t trackerNow, trackerAfter;
	PmDemand pmdNow, pmdPre, pmdPost;
	uint32_t pmd0Pre, pmd1Pre, pmd0Post, pmd1Post;
	int pwIdx;
	uint32_t bwMBps, needMBps, maxCdclkNeeded;
};
inline void planPipe(const Timing &t, const Link &link, int pipe, uint8_t activePipes, bool joined, const uint32_t weight[4],
                     const Latency &lat, PipePlan *pp)
{
	*pp = {};
	pp->t = t;
	pp->link = link;
	pp->mn = linkMN(link.bpp, link.lanes, t.clock, link.rate);
	pp->regs = timingRegs(t);
	pp->linetime = linetimeWm(t.htotal, t.clock);
	pp->slices = dbufSlices(pipe, activePipes, joined);
	pp->ddb = pipeDdb(pipe, activePipes, joined, weight, &pp->mbusOffset);
	pp->wmOk = pipeWm(t, t.hdisplay, lat, pp->ddb, popcount8(activePipes), &pp->wm);
	pp->dbox = mbusDboxCtl(joined);
	pp->minCdclk = umax(minCdclk(t.clock), bwMinCdclk(dataRate(t.clock)));
}
// Pipe A recomputed from its live registers, and everything the second pipe needs. Nothing is written anywhere.
inline int computePlan(const LiveState &s, const Request &q, Plan *o)
{
	*o = {};
	const int pa = s.pipeA, pb = q.pipe;
	const Latency lat = latencyFromRegs(s.lat01, s.lat23, s.lat45, s.latSagv);
	// pipe A as it runs: port and lanes from TRANS_DDI_FUNC_CTL, port clock from DDI_CLK_VALFREQ, pixel clock from the link
	// M/N (intel_dotclock_calculate: link M x link symbol clock / link N, rounded up)
	const uint32_t selA = (s.funcCtl >> 27) & 0xF, wA = (s.funcCtl >> 1) & 7;
	if (!selA || !portInfo((int)selA - 1, &o->portA) || !s.valfreq || !s.linkN) { o->err = kPipeA; return o->err; }
	const uint32_t clockA = divRoundUp((uint64_t)(s.linkM & 0xFFFFFF) * s.valfreq, s.linkN & 0xFFFFFF);
	const Timing ta = timingFromRegs(s.htotal, s.hsync, s.vtotal, s.vsync, s.funcCtl, clockA);
	const Link linkA = { s.valfreq, wA == 4 ? 3u : wA + 1, 24 };   // DDI_PORT_WIDTH(lanes) = lanes - 1, but 3 lanes = 4
	uint32_t weight[4] = {};
	weight[pa] = ta.hdisplay;
	planPipe(ta, linkA, pa, (uint8_t)(1u << pa), mbusJoined((uint8_t)(1u << pa)), weight, lat, &o->a);
	o->joinedNow = s.mbusCtl & MBUS_JOIN;
	o->aPlaneNow = ddbFromReg(s.planeBufCfg);
	o->aCursorNow = ddbFromReg(s.curBufCfg);
	o->trackerNow = (s.dbufCtl[0] >> 16) & 7;
	for (int i = 0; i < kNumSlices; i++) if (s.dbufCtl[i] & DBUF_POWER_STATE) o->slicesNow = (uint8_t)(o->slicesNow | (1u << i));
	o->pmdNow = pmdemandFromRegs(s.pmd0, s.pmd1);
	// the second output
	if (!portInfo(q.port, &o->portB)) { o->err = kBadPort; return o->err; }
	if (q.port == o->portA.port) { o->err = kPortInUse; return o->err; }
	if (pb == pa || pb < 0 || pb > 3 || (s.transconfB & (1u << 31))) { o->err = kPipeInUse; return o->err; }
	if (!timingSane(q.mode)) { o->err = kBadMode; return o->err; }
	Link linkB;
	if (!linkConfig(q.mode.clock, 24, umin(q.sinkMaxRate, 810000), umin(q.sinkMaxLanes, 4), &linkB)) { o->err = kNoLink; return o->err; }
	const uint8_t both = (uint8_t)(1u << pa | 1u << pb);
	o->joinedAfter = mbusJoined(both);
	weight[pb] = q.mode.hdisplay;
	planPipe(ta, linkA, pa, both, o->joinedAfter, weight, lat, &o->a2);
	planPipe(q.mode, linkB, pb, both, o->joinedAfter, weight, lat, &o->b);
	o->slicesAfter = (uint8_t)(enabledSlices(both, o->joinedAfter) | o->slicesNow);
	o->mbusCtlAfter = (s.mbusCtl & ~(MBUS_JOIN | MBUS_HASHING_1x4 | MBUS_PIPE_SELECT_MASK)) | mbusCtlBits(o->joinedAfter, pa);
	o->trackerAfter = dbufMinTracker(o->joinedAfter);
	o->pwIdx = pipePowerWell(pb);
	// pipe A keeps its DDB when its plane and cursor blocks lie inside its two-pipe range and its MBUS bank does not move
	// (PLANE_BUF_CFG is bank-relative: pipe A stays in the S1/S2 bank, a panel on pipe B would move to S3/S4)
	o->aKeepsDdb = o->aPlaneNow.end <= o->a2.ddb.end && o->aCursorNow.end <= o->a2.ddb.end && o->a.mbusOffset == o->a2.mbusOffset;
	// PMDEMAND: the current request -> two pipes (pre: the larger of both, post: the new one)
	const PmDemand pmNew = pmdemandFor(o->pmdNow, 2, o->slicesAfter, o->portA.phy == o->portB.phy ? 1 : 2, umax(linkA.rate, linkB.rate));
	o->pmdPre = pmdemandMax(o->pmdNow, pmNew);
	o->pmdPost = pmNew;
	o->pmd0Pre = s.pmd0; o->pmd1Pre = s.pmd1; pmdemandToRegs(o->pmdPre, &o->pmd0Pre, &o->pmd1Pre);
	o->pmd0Post = s.pmd0; o->pmd1Post = s.pmd1; pmdemandToRegs(o->pmdPost, &o->pmd0Post, &o->pmd1Post);
	o->bwMBps = deratedBwMBps(s.memGlobal, s.qgvLow, 8);
	o->needMBps = divRoundUp(dataRate(ta.clock) + dataRate(q.mode.clock), 1000);
	o->maxCdclkNeeded = umax(o->a.minCdclk, o->b.minCdclk);
	if (o->b.minCdclk > kBmgCdclk) o->err = kCdclk;
	else if (!o->aKeepsDdb) o->err = kDdbA;
	else if (!o->b.wmOk) o->err = o->b.wm.vblankMaxLevel < 0 ? kVblank : kDdbB;
	else if (o->bwMBps && o->needMBps > o->bwMBps) o->err = kBandwidth;
	return o->err;
}

}  // namespace intelxeplan

#endif
