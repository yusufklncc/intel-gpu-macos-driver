// SPDX-License-Identifier: MIT
/*
 * Contains material from Linux drivers/gpu/drm/i915/display: intel_cx0_phy.c and
 * intel_ddi_buf_trans.c (C20 PHY tables and sequences), and the link training steps of intel_ddi.c,
 * intel_dp_link_training.c and intel_display.c.
 *
 * Copyright © 2020-2023 Intel Corporation
 * Copyright (c) 2026 Yusuf İhsan Kılınç (this adaptation)
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */
// IntelXeLink: retrain the DP link of the output the GOP lit, or take that output fully down (including its C20 PLL) and
// bring it back from scratch at the same mode. Display 14.01 (Xe2_HPD): every port is a C20 PHY, none is Type-C
// (intel_phy_is_tc: dgfx -> false), so there is no FIA/TCSS step, no DP-alt lane ownership and no protocol-switch bank
// clearing (intel_c20_protocol_switch_valid: legacy TC mode only). The driver loads no DMC, so DC states stay off and
// the PHY message bus is always usable (intel_cx0_phy_transaction_begin's DC_OFF power reference).
//
// Every step names the Linux function it comes from (drivers/gpu/drm/i915/display: intel_ddi.c, intel_cx0_phy.c,
// intel_dp_link_training.c, intel_display.c; drm_dp_helper.c). Where this file departs from Linux, the comment says so.
// Waits are bounded by Linux's timeouts; when Linux only warns on a timeout, so does this file and it goes on.
//
// Parameterized by port, PHY, pipe and transcoder (IntelXeLink::Out). What a second pipe on another port would still need
// is listed in the report, not here: PW_B, PMDEMAND, MBUS/DBUF split, DDB and watermarks, its own surface and timings.

#include "IntelXeLink.h"
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <kern/clock.h>
#include <stdarg.h>

namespace {
char gLog[24576];
uint32_t gLogLen = 0;

__attribute__((format(printf, 1, 2))) void llog(const char *fmt, ...)
{
	char line[320];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(line, sizeof line, fmt, ap);
	va_end(ap);
	IOLog("IntelXeFB: link: %s\n", line);
	uint32_t n = (uint32_t)strlen(line);
	if (gLogLen + n + 2 < sizeof gLog) {
		bcopy(line, gLog + gLogLen, n);
		gLogLen += n;
		gLog[gLogLen++] = '\n';
		gLog[gLogLen] = 0;
	}
}

uint64_t nowUs()
{
	uint64_t ns;
	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000;
}

constexpr uint32_t BIT(int n) { return 1u << n; }
constexpr uint32_t EN = BIT(31);

// enum port (intel_display_limits.h): A 0, B 1, TC1 = PORT_D 3 .. TC4 6. Display 14 has no xe2lpd index remap.
constexpr int PORT_TC1 = 3;
// _PICK_EVEN_2RANGES(port, PORT_TC1, A, B, USBC1, USBC2) (intel_cx0_phy_regs.h)
constexpr uint32_t portReg(int port, uint32_t a, uint32_t b, uint32_t tc1, uint32_t tc2)
{
	return port < PORT_TC1 ? a + (uint32_t)port * (b - a) : tc1 + (uint32_t)(port - PORT_TC1) * (tc2 - tc1);
}
constexpr uint32_t PORT_BUF_CTL1(int port)          { return portReg(port, 0x64004, 0x64104, 0x16F200, 0x16F400); }
constexpr uint32_t PORT_BUF_CTL2(int port)          { return PORT_BUF_CTL1(port) + 4; }
constexpr uint32_t PORT_BUF_CTL3(int port)          { return PORT_BUF_CTL1(port) + 8; }
constexpr uint32_t M2P_MSGBUS_CTL(int port, int ln) { return portReg(port, 0x64040, 0x64140, 0x16F240, 0x16F440) + (uint32_t)ln * 4; }
constexpr uint32_t P2M_MSGBUS_STATUS(int port, int ln) { return M2P_MSGBUS_CTL(port, ln) + 8; }
constexpr uint32_t MSGBUS_TIMER(int port, int ln)   { return portReg(port, 0x640D8, 0x641D8, 0x16F258, 0x16F458) + (uint32_t)ln * 4; }
constexpr uint32_t PORT_CLOCK_CTL(int port)         { return portReg(port, 0x640E0, 0x641E0, 0x16F260, 0x16F460); }
constexpr uint32_t DDI_BUF_CTL(int port)            { return 0x64000 + (uint32_t)port * 0x100; }   // "DDI_CTL_DE" on MTL+
constexpr uint32_t DDI_CLK_VALFREQ(int port)        { return 0x64030 + (uint32_t)port * 0x100; }

// PORT_BUF_CTL1
constexpr uint32_t PB_D2D_EN = BIT(29), PB_D2D_STATE = BIT(28), PB_SOC_PHY_READY = BIT(24);
constexpr uint32_t PB_DATA_WIDTH_MASK = 3u << 18, PB_REVERSAL = BIT(16), PB_IO_SELECT_TBT = BIT(11), PB_PHY_IDLE = BIT(7);
constexpr uint32_t PB_WIDTH_MASK = 7u << 1;
constexpr uint32_t PB_WIDTH(int w) { return (uint32_t)(w == 3 ? 4 : w - 1) << 1; }   // XELPDP_PORT_WIDTH
// PORT_BUF_CTL2
constexpr uint32_t PIPE_RESET(int ln)        { return BIT(31 - ln); }
constexpr uint32_t PHY_CUR_STATUS(int ln)    { return BIT(29 - ln); }
constexpr uint32_t PD_UPDATE(int ln)         { return BIT(25 - ln); }
constexpr uint32_t PD_NEW_STATE(int ln, uint32_t s) { return s << (ln ? 16 : 20); }
constexpr uint32_t POWER_STATE_READY_MASK = 0xFu << 4;
// PORT_BUF_CTL3
constexpr uint32_t POWER_STATE_ACTIVE_MASK = 0xFu, LANE_STAGGER_MASK = 0xFFu << 8;
// powerdown states
constexpr uint8_t P0_STATE_ACTIVE = 0x0, P2_STATE_READY = 0x2, P2_STATE_RESET = 0x2, P2PG_STATE_DISABLE = 0x9, P4PG_STATE_DISABLE = 0xC;
// PORT_CLOCK_CTL
constexpr uint32_t PLL_REQ(int ln)    { return BIT(31 - ln * 4); }
constexpr uint32_t PLL_ACK(int ln)    { return BIT(30 - ln * 4); }
constexpr uint32_t REFCLK_REQ(int ln) { return BIT(29 - ln * 4); }
constexpr uint32_t REFCLK_ACK(int ln) { return BIT(28 - ln * 4); }
constexpr uint32_t DDI_CLOCK_SELECT_MASK = 0xFu << 12, DDI_CLOCK_SELECT_MAXPCLK = 0x8u << 12;
constexpr uint32_t FORWARD_CLOCK_UNGATE = BIT(10), LANE1_PHY_CLOCK_SELECT = BIT(8), SSC_ENABLE_PLLA = BIT(1), SSC_ENABLE_PLLB = BIT(0);
// message bus
constexpr uint32_t M2P_PENDING = BIT(31), M2P_WRITE_UNCOMMITTED = 1u << 27, M2P_WRITE_COMMITTED = 2u << 27, M2P_READ = 3u << 27;
constexpr uint32_t M2P_RESET = BIT(15);
constexpr uint32_t P2M_READY = BIT(31), P2M_ERROR = BIT(15), P2M_READ_ACK = 4, P2M_WRITE_ACK = 5;
constexpr uint32_t TIMER_TIMED_OUT = BIT(31), TIMER_VAL_MASK = 0xFFFFFF, TIMER_VAL = 0xA000;
// PHY registers over the bus
constexpr uint16_t C20_WR_ADDRESS_L = 0xC02, C20_WR_ADDRESS_H = 0xC03, C20_WR_DATA_L = 0xC04, C20_WR_DATA_H = 0xC05;
constexpr uint16_t C20_RD_ADDRESS_L = 0xC06, C20_RD_ADDRESS_H = 0xC07, C20_RD_DATA_L = 0xC08, C20_RD_DATA_H = 0xC09;
constexpr uint16_t C20_VDR_SERDES_RATE = 0xD00, C20_VDR_CUSTOM_WIDTH = 0xD02, CX0_VDR_OVRD = 0xD71, CX0_VDROVRD_CTL = 0xD80;
constexpr uint8_t C20_CONTEXT_TOGGLE = 0x01, C20_IS_DP = 0x40, C20_SERDES_RATE_MASK = 0xDE;   // IS_DP | DP_RATE 4:1 | IS_HDMI_FRL
constexpr uint16_t CX0_TX_CONTROL(int tx, int c) { return (uint16_t)(0x400 + (tx - 1) * 0x200 + c); }
constexpr uint8_t CONTROL2_DISABLE_SINGLE_TX = 0x40;
// Xe2_HPD (display 14.01) SRAM contexts; the index is subtracted (PHY_C20_A_TX_CNTX_CFG ...)
constexpr uint16_t C20_A_TX = 0xCF5E, C20_B_TX = 0xCF5A, C20_A_CMN = 0xCE8E, C20_B_CMN = 0xCE89;
constexpr uint16_t C20_A_MPLLA = 0xCE58, C20_B_MPLLA = 0xCE4D, C20_A_MPLLB = 0xCCC2, C20_B_MPLLB = 0xCCB6;
constexpr uint16_t C20_PHY_USE_MPLLB = 0x80;

// transcoder (A..D = +0x1000), pipe (+0x1000), scaler (+0x800 per pipe, +0x100 per scaler)
constexpr uint32_t TR(int t, uint32_t a) { return a + (uint32_t)t * 0x1000; }
constexpr uint32_t PS(int p, int s, uint32_t a) { return a + (uint32_t)p * 0x800 + (uint32_t)s * 0x100; }
// TRANS_DDI_FUNC_CTL
constexpr uint32_t FUNC_PORT_MASK = 0xFu << 27, FUNC_MODE_MASK = 7u << 24, FUNC_MODE_SST = 2u << 24;
constexpr uint32_t FUNC_BPC_MASK = 7u << 20, FUNC_PVSYNC = BIT(17), FUNC_PHSYNC = BIT(16), FUNC_WIDTH_MASK = 7u << 1;
constexpr uint32_t DDI_PORT_WIDTH(int w) { return (uint32_t)(w == 3 ? 4 : w - 1) << 1; }
// DP_TP_CTL / DP_TP_STATUS
constexpr uint32_t TP_FEC = BIT(30), TP_MODE_MST = BIT(27), TP_ENHANCED_FRAME = BIT(18), TP_TRAIN_MASK = 7u << 8;
constexpr uint32_t TP_PAT1 = 0u << 8, TP_PAT2 = 1u << 8, TP_IDLE = 2u << 8, TP_NORMAL = 3u << 8, TP_PAT3 = 4u << 8, TP_PAT4 = 5u << 8;
constexpr uint32_t TP_STATUS_IDLE_DONE = BIT(25);
// DDI_BUF_CTL
constexpr uint32_t BUF_REVERSAL = BIT(16), BUF_A_4_LANES = BIT(4);
// TRANSCONF, CHICKEN_TRANS (display 14: 0x604E0), interrupts
constexpr uint32_t TRANSCONF_STATE = BIT(30);
constexpr uint32_t CHICKEN_FECSTALL_DIS = BIT(23), CHICKEN_FEC_BS_JITTER_WA = BIT(15), CHICKEN_DSC_SF_EOL_WA = BIT(4);
constexpr uint32_t FRAME_START_DELAY_MASK = 3u << 27;
constexpr uint32_t PIPE_FIFO_UNDERRUN = BIT(31);

// DPCD
constexpr uint32_t DP_LINK_BW_SET = 0x100, DP_DOWNSPREAD_CTRL = 0x107, DP_TRAINING_PATTERN_SET = 0x102, DP_TRAINING_LANE0_SET = 0x103;
constexpr uint32_t DP_LANE_COUNT_SET = 0x101, DP_LANE0_1_STATUS = 0x202, DP_SET_POWER = 0x600;
constexpr uint8_t DP_LANE_COUNT_ENHANCED_FRAME_EN = 0x80, DP_POST_LT_ADJ_REQ_GRANTED = 0x20, DP_SET_ANSI_8B10B = 0x01;
constexpr uint8_t DP_LINK_SCRAMBLING_DISABLE = 0x20, DP_TRAINING_PATTERN_DISABLE = 0;
constexpr uint8_t DP_SET_POWER_D0 = 1, DP_SET_POWER_D3 = 2;
constexpr uint8_t VSWING_MASK = 0x03, MAX_SWING_REACHED = 0x04, PREEMPH_MASK = 0x18, MAX_PREEMPH_REACHED = 0x20;

// index_to_dp_signal_levels (intel_ddi.c) and the C20 DP1.4 table (_mtl_c20_trans_dp14, intel_ddi_buf_trans.c:
// .snps = { vswing, pre_cursor, post_cursor })
const uint8_t kSignalLevels[10] = { 0x00, 0x08, 0x10, 0x18, 0x01, 0x09, 0x11, 0x02, 0x0A, 0x03 };
struct SnpsTrans { uint8_t vswing, pre, post; };
const SnpsTrans kC20Dp14[10] = {
	{ 20, 0, 0 }, { 24, 0, 4 }, { 30, 0, 9 }, { 34, 0, 14 }, { 29, 0, 0 },
	{ 34, 0, 5 }, { 38, 0, 10 }, { 36, 0, 0 }, { 40, 0, 6 }, { 48, 0, 0 },
};

// DP 1.4 rates on Xe2_HPD: xe2hpd_c20_dp_tables uses the MTL RBR/HBR1/HBR2/HBR3 contexts (intel_cx0_phy.c);
// serdes = PHY_C20_IS_DP | PHY_C20_DP_RATE(intel_c20_get_dp_rate), custom width 0 for 8b/10b
struct Rate { int clock; uint8_t bw; IntelXeLink::C20Pll pll; };
const Rate kRates[4] = {
	{ 162000, 0x06, { { 0xbe88, 0x5800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },        // mtl_c20_dp_rbr
	                  { 0x50a8, 0x2120, 0xcd9a, 0xbfc1, 0x5ab8, 0x4c34, 0x2000, 0x0001, 0x6000, 0x0000, 0x0000 }, 0, C20_IS_DP | 0 << 1 } },
	{ 270000, 0x0A, { { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },        // mtl_c20_dp_hbr1
	                  { 0x308c, 0x2110, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81, 0x2000, 0x0001, 0x5000, 0x0000, 0x0000 }, 0, C20_IS_DP | 1 << 1 } },
	{ 540000, 0x14, { { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },        // mtl_c20_dp_hbr2
	                  { 0x108c, 0x2108, 0xcc9c, 0xbfc1, 0x4b9a, 0x3f81, 0x2000, 0x0001, 0x5000, 0x0000, 0x0000 }, 0, C20_IS_DP | 2 << 1 } },
	{ 810000, 0x1E, { { 0xbe88, 0x4800, 0x0000 }, { 0x0500, 0x0005, 0x0000, 0x0000 },        // mtl_c20_dp_hbr3
	                  { 0x10d2, 0x2108, 0x8d98, 0xbfc1, 0x7166, 0x5f42, 0x2000, 0x0001, 0x7800, 0x0000, 0x0000 }, 0, C20_IS_DP | 3 << 1 } },
};
const Rate *rateFor(int clock) { for (const Rate &r : kRates) if (r.clock == clock) return &r; return nullptr; }
const Rate *rateForBw(uint8_t bw) { for (const Rate &r : kRates) if (r.bw == bw) return &r; return nullptr; }

// snapshot slots: every register this file writes, plus the ones it reports
enum {
	S_TRANSCONF, S_FUNC_CTL, S_FUNC_CTL2, S_DP_TP_CTL, S_DP_TP_STATUS, S_DDI_BUF_CTL, S_BUF_CTL1, S_BUF_CTL2, S_BUF_CTL3,
	S_CLOCK_CTL, S_CLK_VALFREQ, S_MSGBUS_TIMER0, S_MSGBUS_TIMER1, S_M2P0, S_M2P1, S_TRANS_CLK_SEL, S_DP2_CTL,
	S_CHICKEN_TRANS, S_VRR_CTL, S_HTOTAL, S_HBLANK, S_HSYNC, S_VTOTAL, S_VBLANK, S_VSYNC, S_VSYNCSHIFT, S_MULT, S_SCL,
	S_DATA_M1, S_DATA_N1, S_LINK_M1, S_LINK_N1, S_MSA_MISC, S_PIPESRC, S_PIPE_MISC, S_PIPE_CHICKEN, S_LINETIME,
	S_PLANE_CTL, S_PLANE_SURF, S_PS0_CTRL, S_PS0_WIN_POS, S_PS0_WIN_SZ, S_PS0_VPHASE, S_PS0_HPHASE, S_PS1_CTRL,
	S_PS1_WIN_POS, S_PS1_WIN_SZ, S_PS1_VPHASE, S_PS1_HPHASE, S_PIPE_IIR, S_FRMCOUNT, S_PIPEDSL, S_DC_STATE,
	S_PMDEMAND0, S_PMDEMAND1, S_CDCLK_CTL, S_SDEISR, S_NUM
};
static_assert(S_NUM <= 64, "snapshot table");

// result codes (debug.intelxefb_link read-back)
enum { R_OK = 0, R_IDENTIFY = -1, R_DC_ON = -2, R_DPCD = -3, R_UNSUPPORTED = -4, R_MSGBUS = -5, R_LINK_DOWN = -6,
       R_TRAINING = -10, R_PLL = -12, R_VERIFY = -20 };

// the output of the last successful action, for a run after a failed one left TRANS_DDI_FUNC_CTL without a port
IntelXeLink::Out gLast;
bool gLastOk = false;
// kSave's snapshot (values only; the offsets are this output's and are taken again by kRestore)
uint32_t gSaved[64];
bool gSavedOk = false;
// the first C20 context read back since the kext loaded, i.e. the GOP's while no modeset has run yet: action 3 uses
// it, so that after a failed action 2 it does not read back (and reuse) the table that action 2 left in the PHY
IntelXeLink::C20Pll gFirstPll;
bool gFirstPllOk = false;
int gFirstPllClock = 0;
}

const char *IntelXeLink::logText() { return gLog; }
uint32_t IntelXeLink::logLength() { return gLogLen; }

// ---- helpers -----------------------------------------------------------------------------------------------------

// intel_de_wait_*: poll (mask, want) for up to `us`; 1 us steps first, then 10 us, then 1 ms sleeps
bool IntelXeLink::waitBits(uint32_t off, uint32_t mask, uint32_t want, uint32_t us, uint32_t *last)
{
	uint32_t v = rd(off), t = 0;
	while ((v & mask) != want && t < us) {
		if (t < 100) { IODelay(1); t += 1; }
		else if (t < 2000) { IODelay(10); t += 10; }
		else { IOSleep(1); t += 1000; }
		v = rd(off);
	}
	if (last) *last = v;
	return (v & mask) == want;
}

bool IntelXeLink::waitOrWarn(const char *what, uint32_t off, uint32_t mask, uint32_t want, uint32_t us)
{
	uint32_t v;
	if (waitBits(off, mask, want, us, &v)) return true;
	fWarns++;
	llog("timeout: %s (0x%x = 0x%08x, mask 0x%08x, want 0x%08x, %u us)", what, off, v, mask, want, us);
	return false;
}

void IntelXeLink::sleepUs(uint32_t us)   // fsleep
{
	if (us <= 1000) IODelay(us);
	else IOSleep((us + 999) / 1000);
}

bool IntelXeLink::waitVblank(uint32_t ms)
{
	uint32_t f0 = rd(reg(S_FRMCOUNT));
	for (uint32_t t = 0; t < ms; t++) {
		IOSleep(1);
		if (rd(reg(S_FRMCOUNT)) != f0) return true;
	}
	return false;
}

void IntelXeLink::takeSnapshot()
{
	const int t = o.trans, p = o.pipe, port = o.port;
	struct { int id; const char *name; uint32_t off; } const regs[] = {
		{ S_TRANSCONF, "TRANSCONF", TR(p, 0x70008) },          { S_FUNC_CTL, "TRANS_DDI_FUNC_CTL", TR(t, 0x60400) },
		{ S_FUNC_CTL2, "TRANS_DDI_FUNC_CTL2", TR(t, 0x60404) }, { S_DP_TP_CTL, "DP_TP_CTL", TR(t, 0x60540) },
		{ S_DP_TP_STATUS, "DP_TP_STATUS", TR(t, 0x60544) },    { S_DDI_BUF_CTL, "DDI_BUF_CTL", DDI_BUF_CTL(port) },
		{ S_BUF_CTL1, "PORT_BUF_CTL1", PORT_BUF_CTL1(port) },  { S_BUF_CTL2, "PORT_BUF_CTL2", PORT_BUF_CTL2(port) },
		{ S_BUF_CTL3, "PORT_BUF_CTL3", PORT_BUF_CTL3(port) },  { S_CLOCK_CTL, "PORT_CLOCK_CTL", PORT_CLOCK_CTL(port) },
		{ S_CLK_VALFREQ, "DDI_CLK_VALFREQ", DDI_CLK_VALFREQ(port) },
		{ S_MSGBUS_TIMER0, "MSGBUS_TIMER_LN0", MSGBUS_TIMER(port, 0) }, { S_MSGBUS_TIMER1, "MSGBUS_TIMER_LN1", MSGBUS_TIMER(port, 1) },
		{ S_M2P0, "M2P_MSGBUS_CTL_LN0", M2P_MSGBUS_CTL(port, 0) }, { S_M2P1, "M2P_MSGBUS_CTL_LN1", M2P_MSGBUS_CTL(port, 1) },
		{ S_TRANS_CLK_SEL, "TRANS_CLK_SEL", 0x46140 + (uint32_t)t * 4 }, { S_DP2_CTL, "TRANS_DP2_CTL", TR(t, 0x600A0) },
		{ S_CHICKEN_TRANS, "CHICKEN_TRANS", TR(t, 0x604E0) },  { S_VRR_CTL, "TRANS_VRR_CTL", TR(t, 0x60420) },
		{ S_HTOTAL, "TRANS_HTOTAL", TR(t, 0x60000) },          { S_HBLANK, "TRANS_HBLANK", TR(t, 0x60004) },
		{ S_HSYNC, "TRANS_HSYNC", TR(t, 0x60008) },            { S_VTOTAL, "TRANS_VTOTAL", TR(t, 0x6000C) },
		{ S_VBLANK, "TRANS_VBLANK", TR(t, 0x60010) },          { S_VSYNC, "TRANS_VSYNC", TR(t, 0x60014) },
		{ S_VSYNCSHIFT, "TRANS_VSYNCSHIFT", TR(t, 0x60028) },  { S_MULT, "TRANS_MULT", TR(t, 0x6002C) },
		{ S_SCL, "TRANS_SET_CONTEXT_LATENCY", TR(t, 0x6007C) },
		{ S_DATA_M1, "DATA_M1", TR(t, 0x60030) },              { S_DATA_N1, "DATA_N1", TR(t, 0x60034) },
		{ S_LINK_M1, "LINK_M1", TR(t, 0x60040) },              { S_LINK_N1, "LINK_N1", TR(t, 0x60044) },
		{ S_MSA_MISC, "TRANS_MSA_MISC", TR(t, 0x60410) },      { S_PIPESRC, "PIPESRC", TR(p, 0x6001C) },
		{ S_PIPE_MISC, "PIPE_MISC", TR(p, 0x70030) },          { S_PIPE_CHICKEN, "PIPE_CHICKEN", TR(p, 0x70038) },
		{ S_LINETIME, "WM_LINETIME", 0x45270 + (uint32_t)p * 4 },
		{ S_PLANE_CTL, "PLANE_CTL_1", TR(p, 0x70180) },        { S_PLANE_SURF, "PLANE_SURF_1", TR(p, 0x7019C) },
		{ S_PS0_CTRL, "PS_CTRL_1", PS(p, 0, 0x68180) },        { S_PS0_WIN_POS, "PS_WIN_POS_1", PS(p, 0, 0x68170) },
		{ S_PS0_WIN_SZ, "PS_WIN_SZ_1", PS(p, 0, 0x68174) },    { S_PS0_VPHASE, "PS_VPHASE_1", PS(p, 0, 0x68188) },
		{ S_PS0_HPHASE, "PS_HPHASE_1", PS(p, 0, 0x68194) },    { S_PS1_CTRL, "PS_CTRL_2", PS(p, 1, 0x68180) },
		{ S_PS1_WIN_POS, "PS_WIN_POS_2", PS(p, 1, 0x68170) },  { S_PS1_WIN_SZ, "PS_WIN_SZ_2", PS(p, 1, 0x68174) },
		{ S_PS1_VPHASE, "PS_VPHASE_2", PS(p, 1, 0x68188) },    { S_PS1_HPHASE, "PS_HPHASE_2", PS(p, 1, 0x68194) },
		{ S_PIPE_IIR, "GEN8_DE_PIPE_IIR", 0x44408 + (uint32_t)p * 0x10 },
		{ S_FRMCOUNT, "PIPE_FRMCOUNT", TR(p, 0x70040) },       { S_PIPEDSL, "PIPEDSL", TR(p, 0x70000) },
		{ S_DC_STATE, "DC_STATE_EN", 0x45504 },                { S_PMDEMAND0, "PMDEMAND_REQ0", 0x45230 },
		{ S_PMDEMAND1, "PMDEMAND_REQ1", 0x45234 },             { S_CDCLK_CTL, "CDCLK_CTL", 0x46000 },
		{ S_SDEISR, "SDEISR", 0xC4000 },
	};
	static_assert(sizeof regs / sizeof regs[0] == S_NUM, "every snapshot slot named");
	for (const auto &r : regs) fSnap[r.id] = { r.name, r.off, rd(r.off) };
}

void IntelXeLink::logSnapshot()
{
	char line[300];
	for (int i = 0; i < S_NUM; i += 3) {
		int n = 0;
		for (int j = i; j < i + 3 && j < S_NUM; j++)
			n += snprintf(line + n, sizeof line - (size_t)n, "%s%s 0x%x=0x%08x", j > i ? ", " : "", fSnap[j].name, fSnap[j].off, fSnap[j].val);
		llog("snap %s", line);
	}
}

// ---- DPCD --------------------------------------------------------------------------------------------------------

bool IntelXeLink::dpcdRead(uint32_t addr, uint8_t *buf, uint32_t len)
{
	for (uint32_t i = 0; i < len; i += 16) {
		uint32_t n = len - i < 16 ? len - i : 16;
		if (fDpcdFn(fCtx, addr + i, buf + i, n, false)) return false;
	}
	return true;
}

bool IntelXeLink::dpcdWrite(uint32_t addr, const uint8_t *buf, uint32_t len)
{
	uint8_t tmp[16];
	if (len > 16) return false;
	bcopy(buf, tmp, len);
	if (fDpcdFn(fCtx, addr, tmp, len, true) == 0) return true;
	llog("DPCD write 0x%x (%u bytes) failed", addr, len);
	return false;
}

void IntelXeLink::logDpcd(uint32_t addr, uint32_t len)
{
	uint8_t b[16];
	char line[80];
	for (uint32_t i = 0; i < len; i += 16) {
		uint32_t n = len - i < 16 ? len - i : 16;
		if (!dpcdRead(addr + i, b, n)) { llog("DPCD 0x%05x: AUX read failed", addr + i); continue; }
		int k = 0;
		for (uint32_t j = 0; j < n; j++) k += snprintf(line + k, sizeof line - (size_t)k, " %02x", b[j]);
		llog("DPCD 0x%05x:%s", addr + i, line);
	}
}

// 0x200-0x207: sink count, IRQ vector, lane 0/1 and 2/3 status (CR 0, EQ 1, symbol lock 2), align (interlane 0,
// post-LT 1), sink status (port 0 in sync, bit 0), adjust requests 0/1 and 2/3
void IntelXeLink::logLinkStatus(const char *when)
{
	uint8_t s[8];
	if (!dpcdRead(0x200, s, 8)) { llog("%s: DPCD 0x200-0x207 unreadable", when); return; }
	char lanes[64];
	int k = 0;
	for (int l = 0; l < 4; l++) {
		uint8_t ls = (s[2 + (l >> 1)] >> ((l & 1) * 4)) & 0xF;
		k += snprintf(lanes + k, sizeof lanes - (size_t)k, " L%d:%s%s%s", l, ls & 1 ? "CR" : "--", ls & 2 ? "EQ" : "--", ls & 4 ? "SL" : "--");
	}
	llog("%s: DPCD 200-207 %02x %02x %02x %02x %02x %02x %02x %02x:%s, interlane %s, sink %s, adjust %02x %02x",
	     when, s[0], s[1], s[2], s[3], s[4], s[5], s[6], s[7], lanes, s[4] & 1 ? "aligned" : "NOT aligned",
	     s[5] & 1 ? "in sync" : "NOT in sync", s[6], s[7]);
}

// drm_dp_read_dpcd_caps (with drm_dp_read_extended_dpcd_caps) and the link settings the GOP trained
bool IntelXeLink::readSinkCaps()
{
	uint8_t d[16], ext[16];
	if (!dpcdRead(0x000, d, 16) || d[0] == 0) { llog("DPCD caps unreadable over AUX 0x%x", fAuxFb); return false; }
	if ((d[0x0E] & 0x80) && dpcdRead(0x2200, ext, 16) && ext[0] >= d[0]) {
		if (bcmp(d, ext, 15)) llog("using extended receiver caps (0x2200): rev %x -> %x", d[0], ext[0]);
		bcopy(ext, d, 15);
	}
	bcopy(d, o.dpcd, 16);
	o.enhanced = d[0] >= 0x11 && (d[2] & 0x80);    // drm_dp_enhanced_frame_cap
	o.tps3 = d[0] >= 0x12 && (d[2] & 0x40);        // drm_dp_tps3_supported
	o.tps4 = d[0] >= 0x14 && (d[3] & 0x80);        // drm_dp_tps4_supported
	o.postLtAdj = d[0] >= 0x13 && (d[2] & 0x20);   // drm_dp_post_lt_adj_req_supported
	llog("sink: DPCD rev %x, max rate 0x%02x x%u, TPS3 %d TPS4 %d, enhanced framing %d, post-LT adjust %d, downspread %d, AUX RD interval 0x%02x",
	     d[0], d[1], d[2] & 0x1F, o.tps3, o.tps4, o.enhanced, o.postLtAdj, d[3] & 1, d[0x0E]);
	return true;
}

// drm_dp_lttpr_count over the LTTPR common caps (0xF0000-0xF0007)
int IntelXeLink::lttprCount()
{
	uint8_t c[8];
	if (!dpcdRead(0xF0000, c, 8)) return 0;   // no reply: no LTTPR-aware path (Linux falls back to transparent)
	uint8_t cnt = c[2];
	int bits = 0;
	for (int i = 0; i < 8; i++) bits += (cnt >> i) & 1;
	if (bits == 0) return 0;
	if (bits != 1) return -1;
	int lg = 0;
	while ((1u << (lg + 1)) <= cnt) lg++;
	return 8 - lg;
}

// ---- CX0 message bus (intel_cx0_phy.c) ---------------------------------------------------------------------------

void IntelXeLink::clearResponse(int lane)   // intel_cx0_clear_response_ready_flag
{
	uint32_t r = P2M_MSGBUS_STATUS(o.port, lane);
	wr(r, rd(r) | P2M_READY | P2M_ERROR);
}

void IntelXeLink::busReset(int lane)   // intel_cx0_bus_reset
{
	uint32_t r = M2P_MSGBUS_CTL(o.port, lane);
	wr(r, M2P_RESET);
	if (!waitBits(r, M2P_RESET, 0, 1000)) { fPhyErrs++; llog("PHY msgbus lane %d did not go idle after a bus reset", lane); return; }
	clearResponse(lane);
}

int IntelXeLink::waitAck(int lane, uint32_t cmd, uint32_t *val)   // intel_cx0_wait_for_ack
{
	if (!waitBits(P2M_MSGBUS_STATUS(o.port, lane), P2M_READY, P2M_READY, 1000, val)) {
		llog("PHY msgbus lane %d: no ACK (status 0x%08x, hardware timeout %s)", lane, *val,
		     rd(MSGBUS_TIMER(o.port, lane)) & TIMER_TIMED_OUT ? "seen" : "not seen");
		busReset(lane);
		return -1;
	}
	if (*val & P2M_ERROR) { llog("PHY msgbus lane %d: error, status 0x%08x", lane, *val); busReset(lane); return -2; }
	if (((*val >> 27) & 0xF) != cmd) { llog("PHY msgbus lane %d: not a %s ACK: 0x%08x", lane, cmd == P2M_READ_ACK ? "read" : "write", *val); busReset(lane); return -3; }
	return 0;
}

int IntelXeLink::msgReadOnce(int lane, uint16_t addr)   // __intel_cx0_read_once
{
	uint32_t m2p = M2P_MSGBUS_CTL(o.port, lane), val = 0;
	if (!waitBits(m2p, M2P_PENDING, 0, 1000)) { busReset(lane); return -1; }
	clearResponse(lane);
	wr(m2p, M2P_PENDING | M2P_READ | (addr & 0xFFFu));
	int a = waitAck(lane, P2M_READ_ACK, &val);
	if (a < 0) return a;
	clearResponse(lane);
	busReset(lane);   // display < 30: "let the message bus end up in a known state"
	return (int)((val >> 16) & 0xFF);
}

int IntelXeLink::msgWriteOnce(int lane, uint16_t addr, uint8_t data, bool committed)   // __intel_cx0_write_once
{
	uint32_t m2p = M2P_MSGBUS_CTL(o.port, lane), val = 0;
	if (!waitBits(m2p, M2P_PENDING, 0, 1000)) { busReset(lane); return -1; }
	clearResponse(lane);
	wr(m2p, M2P_PENDING | (committed ? M2P_WRITE_COMMITTED : M2P_WRITE_UNCOMMITTED) | (uint32_t)data << 16 | (addr & 0xFFFu));
	if (!waitBits(m2p, M2P_PENDING, 0, 1000)) { busReset(lane); return -1; }
	if (committed) {
		int a = waitAck(lane, P2M_WRITE_ACK, &val);
		if (a < 0) return a;
	} else if (rd(P2M_MSGBUS_STATUS(o.port, lane)) & P2M_ERROR) {
		busReset(lane);
		return -2;
	}
	clearResponse(lane);
	busReset(lane);
	return 0;
}

// __intel_cx0_read: 3 tries. Linux returns 0 after a failed read; this returns -1 so that an rmw never writes a value
// built from a failed read.
int IntelXeLink::msgRead(int lane, uint16_t addr)
{
	for (int i = 0; i < 3; i++) {
		int r = msgReadOnce(lane, addr);
		if (r >= 0) return r;
	}
	fPhyErrs++;
	llog("PHY read 0x%03x lane %d failed after 3 tries", addr, lane);
	return -1;
}

bool IntelXeLink::msgWrite(int laneMask, uint16_t addr, uint8_t data, bool committed)   // intel_cx0_write
{
	bool ok = true;
	for (int lane = 0; lane < 2; lane++) {
		if (!(laneMask & (1 << lane))) continue;
		int i = 0;
		while (i < 3 && msgWriteOnce(lane, addr, data, committed)) i++;
		if (i == 3) { ok = false; fPhyErrs++; llog("PHY write 0x%03x = 0x%02x lane %d failed after 3 tries", addr, data, lane); }
	}
	return ok;
}

bool IntelXeLink::msgRmw(int laneMask, uint16_t addr, uint8_t clear, uint8_t set, bool committed)   // intel_cx0_rmw
{
	bool ok = true;
	for (int lane = 0; lane < 2; lane++) {
		if (!(laneMask & (1 << lane))) continue;
		int old = msgRead(lane, addr);
		if (old < 0) { ok = false; continue; }
		uint8_t v = (uint8_t)(((uint8_t)old & ~clear) | set);
		if (v != (uint8_t)old && !msgWrite(1 << lane, addr, v, committed)) ok = false;
	}
	return ok;
}

bool IntelXeLink::sramWrite(uint16_t addr, uint16_t data)   // intel_c20_sram_write (lane 0)
{
	return msgWrite(1, C20_WR_ADDRESS_H, (uint8_t)(addr >> 8), false) && msgWrite(1, C20_WR_ADDRESS_L, (uint8_t)addr, false) &&
	       msgWrite(1, C20_WR_DATA_H, (uint8_t)(data >> 8), false) && msgWrite(1, C20_WR_DATA_L, (uint8_t)data, true);
}

int IntelXeLink::sramRead(uint16_t addr)   // intel_c20_sram_read (lane 0)
{
	if (!msgWrite(1, C20_RD_ADDRESS_H, (uint8_t)(addr >> 8), false) || !msgWrite(1, C20_RD_ADDRESS_L, (uint8_t)addr, true)) return -1;
	int h = msgRead(0, C20_RD_DATA_H), l = msgRead(0, C20_RD_DATA_L);
	return h < 0 || l < 0 ? -1 : (h << 8 | l);
}

void IntelXeLink::msgbusTimer()   // intel_cx0_program_msgbus_timer (intel_cx0_phy_transaction_begin)
{
	for (int lane = 0; lane < 2; lane++) rmw(MSGBUS_TIMER(o.port, lane), TIMER_VAL_MASK, TIMER_VAL);
}

// intel_c20pll_readout_hw_state: the context in use (VDR 0xD00 bit 0) and its SRAM
bool IntelXeLink::c20Readout(C20Pll *p)
{
	msgbusTimer();
	int serdes = msgRead(0, C20_VDR_SERDES_RATE), width = msgRead(0, C20_VDR_CUSTOM_WIDTH);
	if (serdes < 0 || width < 0) return false;
	bool cntx = serdes & C20_CONTEXT_TOGGLE;
	*p = {};
	p->serdes = (uint8_t)(serdes & C20_SERDES_RATE_MASK);
	p->width = (uint8_t)(width & 3);
	for (int i = 0; i < 3; i++) {
		int v = sramRead((uint16_t)((cntx ? C20_B_TX : C20_A_TX) - i));
		if (v < 0) return false;
		p->tx[i] = (uint16_t)v;
	}
	for (int i = 0; i < 4; i++) {
		int v = sramRead((uint16_t)((cntx ? C20_B_CMN : C20_A_CMN) - i));
		if (v < 0) return false;
		p->cmn[i] = (uint16_t)v;
	}
	bool b = p->tx[0] & C20_PHY_USE_MPLLB;
	for (int i = 0; i < (b ? 11 : 10); i++) {
		int v = sramRead((uint16_t)(b ? (cntx ? C20_B_MPLLB : C20_A_MPLLB) - i : (cntx ? C20_B_MPLLA : C20_A_MPLLA) - i));
		if (v < 0) return false;
		p->mpll[i] = (uint16_t)v;
	}
	llog("C20 context %c in use (VDR 0xD00 = 0x%02x)", cntx ? 'B' : 'A', serdes);
	return true;
}

void IntelXeLink::logC20(const char *what, const C20Pll &p)
{
	llog("%s: tx %04x %04x %04x cmn %04x %04x %04x %04x %s %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x %04x width %u serdes 0x%02x",
	     what, p.tx[0], p.tx[1], p.tx[2], p.cmn[0], p.cmn[1], p.cmn[2], p.cmn[3], p.tx[0] & C20_PHY_USE_MPLLB ? "mpllb" : "mplla",
	     p.mpll[0], p.mpll[1], p.mpll[2], p.mpll[3], p.mpll[4], p.mpll[5], p.mpll[6], p.mpll[7], p.mpll[8], p.mpll[9], p.mpll[10],
	     p.width, p.serdes);
}

// the vswing/pre/post overrides (PHY_CX0_VDROVRD_CTL), override enables (0xD71) and TX disables, per PHY lane.
// A lane that does not answer the first read is skipped: after a system sleep the PHY is unpowered until the modeset,
// and its other eight reads would only fail too (about 75 ms per lane).
void IntelXeLink::logPhyLevels()
{
	for (int lane = 0; lane < 2; lane++) {
		if (msgRead(lane, CX0_VDR_OVRD) < 0) { llog("PHY lane %d: no answer on the message bus, levels not read", lane); continue; }
		int v[9] = { msgRead(lane, CX0_VDROVRD_CTL + 0), msgRead(lane, CX0_VDROVRD_CTL + 1), msgRead(lane, CX0_VDROVRD_CTL + 2),
		             msgRead(lane, CX0_VDROVRD_CTL + 0x10), msgRead(lane, CX0_VDROVRD_CTL + 0x11), msgRead(lane, CX0_VDROVRD_CTL + 0x12),
		             msgRead(lane, CX0_VDR_OVRD), msgRead(lane, CX0_TX_CONTROL(1, 2)), msgRead(lane, CX0_TX_CONTROL(2, 2)) };
		llog("PHY lane %d: tx0 pre/main/post %d/%d/%d, tx1 %d/%d/%d, 0xD71 0x%02x, TX disable 0x402 0x%02x 0x602 0x%02x",
		     lane, v[0], v[1], v[2], v[3], v[4], v[5], v[6] & 0xFF, v[7] & 0xFF, v[8] & 0xFF);
	}
}

// ---- PHY and PLL (intel_cx0_phy.c) -------------------------------------------------------------------------------

void IntelXeLink::powerdownChange(int laneMask, uint8_t state)   // intel_cx0_powerdown_change_sequence
{
	uint32_t ctl2 = PORT_BUF_CTL2(o.port), set = 0, upd = 0;
	for (int ln = 0; ln < 2; ln++) if (laneMask & (1 << ln)) { set |= PD_NEW_STATE(ln, state); upd |= PD_UPDATE(ln); }
	rmw(ctl2, PD_NEW_STATE(0, 0xF) | PD_NEW_STATE(1, 0xF), set);
	for (int ln = 0; ln < 2; ln++)
		if ((laneMask & (1 << ln)) && !waitBits(M2P_MSGBUS_CTL(o.port, ln), M2P_PENDING, 0, 1000)) busReset(ln);
	rmw(ctl2, PD_UPDATE(0) | PD_UPDATE(1), upd);
	waitOrWarn("powerdown state change", ctl2, upd, 0, 2000);
}

void IntelXeLink::laneReset()   // intel_cx0_phy_lane_reset (both lanes owned: no DP-alt on dGPU)
{
	uint32_t ctl1 = PORT_BUF_CTL1(o.port), ctl2 = PORT_BUF_CTL2(o.port), clk = PORT_CLOCK_CTL(o.port);
	int ln = o.reversal ? 1 : 0;
	uint32_t reset = PIPE_RESET(0) | PIPE_RESET(1), status = PHY_CUR_STATUS(0) | PHY_CUR_STATUS(1);
	waitOrWarn("PHY out of SOC reset", ctl1, PB_SOC_PHY_READY, PB_SOC_PHY_READY, 100);
	rmw(ctl2, reset, reset);
	waitOrWarn("PHY lane reset start", ctl2, status, status, 10);
	rmw(clk, REFCLK_REQ(0) | REFCLK_REQ(1), REFCLK_REQ(ln));
	waitOrWarn("PHY refclk request", clk, REFCLK_ACK(0) | REFCLK_ACK(1), REFCLK_ACK(ln), 10);
	powerdownChange(3, P2_STATE_RESET);
	// intel_cx0_setup_powerdown
	rmw(ctl2, POWER_STATE_READY_MASK, (uint32_t)P2_STATE_READY << 4);
	rmw(PORT_BUF_CTL3(o.port), POWER_STATE_ACTIVE_MASK | LANE_STAGGER_MASK, P0_STATE_ACTIVE);
	rmw(ctl2, reset, 0);
	waitOrWarn("PHY lane reset end", ctl2, status, 0, 15000);
}

// intel_c20_pll_program: write the context not in use, then the VDR rate/width, then toggle the context bit
bool IntelXeLink::c20Program(const C20Pll &p)
{
	int serdes = msgRead(0, C20_VDR_SERDES_RATE);
	if (serdes < 0) return false;
	bool cntx = serdes & C20_CONTEXT_TOGGLE, ok = true;
	llog("C20 program: context %c in use, writing %c", cntx ? 'B' : 'A', cntx ? 'A' : 'B');
	for (int i = 0; i < 3; i++) ok &= sramWrite((uint16_t)((cntx ? C20_A_TX : C20_B_TX) - i), p.tx[i]);
	for (int i = 0; i < 4; i++) ok &= sramWrite((uint16_t)((cntx ? C20_A_CMN : C20_B_CMN) - i), p.cmn[i]);
	if (p.tx[0] & C20_PHY_USE_MPLLB)
		for (int i = 0; i < 11; i++) ok &= sramWrite((uint16_t)((cntx ? C20_A_MPLLB : C20_B_MPLLB) - i), p.mpll[i]);
	else
		for (int i = 0; i < 10; i++) ok &= sramWrite((uint16_t)((cntx ? C20_A_MPLLA : C20_B_MPLLA) - i), p.mpll[i]);
	// intel_c20_program_vdr_params (DP: no HDMI rate)
	ok &= msgRmw(3, C20_VDR_CUSTOM_WIDTH, 0x3, p.width, true);
	ok &= msgRmw(3, C20_VDR_SERDES_RATE, C20_SERDES_RATE_MASK, p.serdes, true);
	ok &= msgRmw(3, C20_VDR_SERDES_RATE, C20_CONTEXT_TOGGLE, cntx ? 0 : C20_CONTEXT_TOGGLE, true);
	return ok;
}

void IntelXeLink::programPhyLanes()   // intel_cx0_program_phy_lane
{
	uint8_t disables = o.reversal ? (uint8_t)(0xF >> o.lanes) : (uint8_t)(0xF << o.lanes);
	for (int i = 0; i < 4; i++)
		msgRmw(i < 2 ? 1 : 2, CX0_TX_CONTROL(i % 2 + 1, 2), CONTROL2_DISABLE_SINGLE_TX,
		       (disables & (1 << i)) ? CONTROL2_DISABLE_SINGLE_TX : 0, true);
}

// intel_cx0pll_enable. SSC: Linux enables it when intel_panel_use_ssc() and the sink allows downspread; this keeps the
// GOP's PORT_CLOCK_CTL SSC bit instead (o.ssc), since the VBT/modparam input is not available here.
bool IntelXeLink::pllEnable(const C20Pll &p)
{
	uint32_t clk = PORT_CLOCK_CTL(o.port);
	int ln = o.reversal ? 1 : 0;   // the lane for maxPCLK
	msgbusTimer();
	// 1. intel_program_port_clock_ctl
	rmw(PORT_BUF_CTL1(o.port), PB_REVERSAL, o.reversal ? PB_REVERSAL : 0);
	uint32_t v = (o.reversal ? LANE1_PHY_CLOCK_SELECT : 0) | FORWARD_CLOCK_UNGATE | DDI_CLOCK_SELECT_MAXPCLK | (o.ssc ? SSC_ENABLE_PLLB : 0);
	rmw(clk, LANE1_PHY_CLOCK_SELECT | FORWARD_CLOCK_UNGATE | DDI_CLOCK_SELECT_MASK | SSC_ENABLE_PLLA | SSC_ENABLE_PLLB, v);
	// 2. intel_cx0_phy_lane_reset
	laneReset();
	// 3. powerdown to P2 ready (both lanes)
	powerdownChange(3, P2_STATE_READY);
	// 5. intel_c20_pll_program
	bool ok = c20Program(p);
	if (!ok) llog("C20 PLL programming had message bus errors");
	// 6. intel_cx0_program_phy_lane
	programPhyLanes();
	// 8. DDI_CLK_VALFREQ = port clock (kHz)
	wr(DDI_CLK_VALFREQ(o.port), (uint32_t)o.clock);
	// 9. PCLK PLL request on the maxPCLK lane, 10. its ACK (3.2 ms)
	rmw(clk, PLL_REQ(0) | PLL_REQ(1), PLL_REQ(ln));
	bool locked = waitOrWarn("PLL lock (PCLK PLL ack)", clk, PLL_ACK(0) | PLL_ACK(1), PLL_ACK(ln), 3200);
	llog("PLL %s: PORT_CLOCK_CTL 0x%08x, PORT_BUF_CTL1 0x%08x CTL2 0x%08x CTL3 0x%08x", locked ? "locked" : "NOT locked",
	     rd(clk), rd(PORT_BUF_CTL1(o.port)), rd(PORT_BUF_CTL2(o.port)), rd(PORT_BUF_CTL3(o.port)));
	return ok && locked;
}

void IntelXeLink::pllDisable()   // intel_cx0pll_disable
{
	uint32_t clk = PORT_CLOCK_CTL(o.port);
	msgbusTimer();
	// 1. lanes to the disable state: P2PG on BMG port A, P4PG otherwise (cx0_power_control_disable_val)
	powerdownChange(3, o.port == 0 ? P2PG_STATE_DISABLE : P4PG_STATE_DISABLE);
	// 3. drop the PLL and refclk requests
	rmw(clk, PLL_REQ(0) | PLL_REQ(1) | REFCLK_REQ(0) | REFCLK_REQ(1), 0);
	// 4.
	wr(DDI_CLK_VALFREQ(o.port), 0);
	// 5. acks clear (20 us)
	waitOrWarn("PLL unlock", clk, PLL_ACK(0) | PLL_ACK(1) | REFCLK_ACK(0) | REFCLK_ACK(1), 0, 20);
	// 7. gate the clocks
	rmw(clk, DDI_CLOCK_SELECT_MASK, 0);
	rmw(clk, FORWARD_CLOCK_UNGATE, 0);
	llog("PLL off: PORT_CLOCK_CTL 0x%08x, PORT_BUF_CTL1 0x%08x CTL2 0x%08x", rd(clk), rd(PORT_BUF_CTL1(o.port)), rd(PORT_BUF_CTL2(o.port)));
}

// intel_cx0_phy_set_signal_levels (C20: no C10 vboost/termination, no msgbus access/commit) with intel_ddi_level per lane
void IntelXeLink::setSignalLevels()
{
	msgbusTimer();
	for (int ln = 0; ln < o.lanes; ln++) {
		uint8_t sig = o.ts[ln] & (VSWING_MASK | PREEMPH_MASK);
		int level = 0;
		while (level < 10 && kSignalLevels[level] != sig) level++;
		if (level == 10) { llog("unsupported vswing/pre-emphasis 0x%02x on lane %d", sig, ln); level = 0; }   // translate_signal_level
		const SnpsTrans &e = kC20Dp14[level];
		int lane = ln / 2, tx = ln % 2;
		uint16_t base = (uint16_t)(CX0_VDROVRD_CTL + ((lane ^ tx) << 4));
		msgRmw(1 << lane, base + 0, 0x3F, e.pre, true);
		msgRmw(1 << lane, base + 1, 0x3F, e.vswing, true);
		msgRmw(1 << lane, base + 2, 0x3F, e.post, true);
	}
	msgRmw(3, CX0_VDR_OVRD, 0, 0x05, true);   // PHY_C10_VDR_OVRD_TX1 | _TX2
}

// ---- DDI, transcoder, pipe ---------------------------------------------------------------------------------------

// intel_ddi_transcoder_func_reg_val_get for DP SST: enable, port select, BPC and sync polarity as the GOP set them
uint32_t IntelXeLink::funcCtlValue() const
{
	return EN | (uint32_t)(o.port + 1) << 27 | FUNC_MODE_SST | (snap(S_FUNC_CTL) & (FUNC_BPC_MASK | FUNC_PVSYNC | FUNC_PHSYNC)) |
	       DDI_PORT_WIDTH(o.lanes);
}

// intel_ddi_init_dp_buf_reg (display 14, 8b/10b: 10-bit data width = 0)
uint32_t IntelXeLink::ddiBufValue() const
{
	return DDI_PORT_WIDTH(o.lanes) | (o.reversal ? BUF_REVERSAL : 0) | (snap(S_DDI_BUF_CTL) & BUF_A_4_LANES);
}

// plane 1 off (icl_plane_disable_arm writes PLANE_CTL then PLANE_SURF, which arms it), then a vblank so it has latched
// before the transcoder goes down (intel_disable_transcoder asserts planes off)
void IntelXeLink::planesOff()
{
	uint32_t ctl = rd(reg(S_PLANE_CTL));
	if (ctl & EN) {
		wr(reg(S_PLANE_CTL), ctl & ~EN);
		wr(reg(S_PLANE_SURF), rd(reg(S_PLANE_SURF)));
		if (!waitVblank(50)) llog("planes off: no vblank within 50 ms");
	}
}

// icl_plane_update_arm: plane scaler first (skl_program_plane_scaler), then PLANE_CTL, PLANE_SURF last (arms it)
void IntelXeLink::planesOn()
{
	for (int s = 0; s < 2; s++) {
		int b = s ? S_PS1_CTRL : S_PS0_CTRL;   // CTRL, WIN_POS, WIN_SZ, VPHASE, HPHASE
		if (!(snap(b) & EN)) continue;
		wr(reg(b + 3), snap(b + 3)); wr(reg(b + 4), snap(b + 4));
		wr(reg(b + 1), snap(b + 1)); wr(reg(b + 2), snap(b + 2));
		wr(reg(b), snap(b));
	}
	uint32_t ctl = fPlaneOn ? snap(S_PLANE_CTL) | EN : snap(S_PLANE_CTL) & ~EN;
	wr(reg(S_PLANE_CTL), ctl);
	wr(reg(S_PLANE_SURF), snap(S_PLANE_SURF));
}

void IntelXeLink::scalersOff()   // skl_scaler_disable -> skl_detach_scaler for every scaler of the pipe
{
	for (int s = 0; s < 2; s++) {
		int b = s ? S_PS1_CTRL : S_PS0_CTRL;
		wr(reg(b), 0); wr(reg(b + 1), 0); wr(reg(b + 2), 0);
	}
}

void IntelXeLink::transcoderOff()   // intel_disable_transcoder + intel_wait_for_pipe_off (100 ms)
{
	uint32_t v = rd(reg(S_TRANSCONF));
	if (!(v & EN)) return;
	wr(reg(S_TRANSCONF), v & ~EN);
	rmw(reg(S_CHICKEN_TRANS), CHICKEN_FECSTALL_DIS, 0);
	waitOrWarn("transcoder off (TRANSCONF state)", reg(S_TRANSCONF), TRANSCONF_STATE, 0, 100000);
}

// intel_enable_transcoder: display 14 sets DP_FEC_BS_JITTER_WA and clears DP_DSC_INSERT_SF_AT_EOL_WA first. Linux waits
// for the scanline only without a frame counter; display 14 has one, so it does not wait here either.
void IntelXeLink::transcoderOn()
{
	rmw(reg(S_CHICKEN_TRANS), CHICKEN_DSC_SF_EOL_WA, CHICKEN_FEC_BS_JITTER_WA);
	uint32_t v = rd(reg(S_TRANSCONF));
	if (v & EN) return;
	wr(reg(S_TRANSCONF), v | EN);
	(void)rd(reg(S_TRANSCONF));
}

void IntelXeLink::funcOff()   // intel_ddi_disable_transcoder_func (SST: also deselect the port and the mode)
{
	wr(reg(S_FUNC_CTL2), 0);
	wr(reg(S_FUNC_CTL), rd(reg(S_FUNC_CTL)) & ~(EN | FUNC_PORT_MASK | FUNC_MODE_MASK));
}

void IntelXeLink::funcConfig()   // intel_ddi_config_transcoder_func: TRANS_DP2_CTL (8b/10b: 0), FUNC_CTL without enable
{
	wr(reg(S_DP2_CTL), 0);
	wr(reg(S_FUNC_CTL), funcCtlValue() & ~EN);
}

void IntelXeLink::funcEnable()   // intel_ddi_enable_transcoder_func (no port sync: FUNC_CTL2 = 0)
{
	wr(reg(S_FUNC_CTL2), 0);
	wr(reg(S_FUNC_CTL), funcCtlValue());
}

void IntelXeLink::bufDisable()   // intel_ddi_buf_disable (display 14: PHY idle is PORT_BUF_CTL1 bit 7, 10 ms; no FEC)
{
	rmw(reg(S_DDI_BUF_CTL), EN, 0);
	waitOrWarn("DDI buffer idle", reg(S_BUF_CTL1), PB_PHY_IDLE, PB_PHY_IDLE, 10000);
	// mtl_ddi_disable_d2d
	rmw(reg(S_BUF_CTL1), PB_D2D_EN, 0);
	waitOrWarn("D2D link disable", reg(S_BUF_CTL1), PB_D2D_STATE, 0, 100);
	rmw(reg(S_DP_TP_CTL), EN, 0);
}

// hsw_crtc_enable between the encoder pre-enable and intel_ddi_enable: intel_set_pipe_src_size, bdw_set_pipe_misc,
// hsw_configure_cpu_transcoder (M1/N1, intel_set_transcoder_timings, TRANS_MULT, frame start delay, hsw_set_transconf
// without enable), hsw_set_linetime_wm, icl_set_pipe_chicken. Same mode: the values are the GOP's, from the snapshot;
// a pipe on another output computes them instead (see the report).
void IntelXeLink::replayPipeConfig()
{
	wr(reg(S_PIPESRC), snap(S_PIPESRC));
	wr(reg(S_PIPE_MISC), snap(S_PIPE_MISC));
	wr(reg(S_DATA_M1), snap(S_DATA_M1));
	wr(reg(S_DATA_N1), snap(S_DATA_N1));
	wr(reg(S_LINK_M1), snap(S_LINK_M1));
	wr(reg(S_LINK_N1), snap(S_LINK_N1));
	wr(reg(S_SCL), snap(S_SCL));
	wr(reg(S_VSYNCSHIFT), snap(S_VSYNCSHIFT));
	wr(reg(S_HTOTAL), snap(S_HTOTAL));
	wr(reg(S_HBLANK), snap(S_HBLANK));
	wr(reg(S_HSYNC), snap(S_HSYNC));
	wr(reg(S_VTOTAL), snap(S_VTOTAL));
	wr(reg(S_VBLANK), snap(S_VBLANK));
	wr(reg(S_VSYNC), snap(S_VSYNC));
	wr(reg(S_MULT), snap(S_MULT));
	rmw(reg(S_CHICKEN_TRANS), FRAME_START_DELAY_MASK, snap(S_CHICKEN_TRANS) & FRAME_START_DELAY_MASK);
	wr(reg(S_TRANSCONF), snap(S_TRANSCONF) & ~(EN | TRANSCONF_STATE));
	wr(reg(S_LINETIME), snap(S_LINETIME));
	wr(reg(S_PIPE_CHICKEN), snap(S_PIPE_CHICKEN));
}

// ---- link training (intel_dp_link_training.c) --------------------------------------------------------------------

// mtl_ddi_prepare_link_retrain: 6.d DP_TP_CTL with TPS1, 6.f D2D, 6.g signal levels, 6.h PORT_BUF_CTL1, 6.i DDI_BUF_CTL
void IntelXeLink::prepareLinkRetrain()
{
	uint32_t tp = rd(reg(S_DP_TP_CTL));
	if (tp & EN) { fWarns++; llog("DP_TP_CTL still enabled before training (0x%08x)", tp); }
	wr(reg(S_DP_TP_CTL), EN | TP_PAT1 | (o.enhanced ? TP_ENHANCED_FRAME : 0));   // DP_TP_CTL_MODE_SST = 0
	(void)rd(reg(S_DP_TP_CTL));
	// mtl_ddi_enable_d2d
	rmw(reg(S_BUF_CTL1), 0, PB_D2D_EN);
	waitOrWarn("D2D link enable", reg(S_BUF_CTL1), PB_D2D_STATE, PB_D2D_STATE, 100);
	setSignalLevels();
	// mtl_port_buf_ctl_program
	rmw(reg(S_BUF_CTL1), PB_WIDTH_MASK | PB_DATA_WIDTH_MASK, PB_WIDTH(o.lanes) | (o.reversal ? PB_REVERSAL : 0));
	// intel_ddi_buf_enable + intel_wait_ddi_buf_active (PHY idle clear, 10 ms)
	wr(reg(S_DDI_BUF_CTL), ddiBufValue() | EN);
	(void)rd(reg(S_DDI_BUF_CTL));
	waitOrWarn("DDI buffer active", reg(S_BUF_CTL1), PB_PHY_IDLE, 0, 10000);
}

// intel_ddi_set_link_train: the source's pattern (intel_dp_training_pattern_symbol)
void IntelXeLink::setTpCtlPattern(uint8_t dpPattern)
{
	uint32_t bits;
	switch (dpPattern & 0xF) {
	case 1: bits = TP_PAT1; break;
	case 2: bits = TP_PAT2; break;
	case 3: bits = TP_PAT3; break;
	case 7: bits = TP_PAT4; break;
	default: bits = TP_NORMAL; break;
	}
	rmw(reg(S_DP_TP_CTL), TP_TRAIN_MASK, bits);
}

// intel_dp_set_link_train: source pattern, then DPCD TRAINING_PATTERN_SET + TRAINING_LANEx_SET in one write
bool IntelXeLink::setLinkTrain(uint8_t dpPattern)
{
	setTpCtlPattern(dpPattern);
	uint8_t buf[5] = { dpPattern, o.ts[0], o.ts[1], o.ts[2], o.ts[3] };
	return dpcdWrite(DP_TRAINING_PATTERN_SET, buf, (uint32_t)o.lanes + 1);
}

bool IntelXeLink::updateLinkTrain()   // intel_dp_update_link_train
{
	setSignalLevels();
	return dpcdWrite(DP_TRAINING_LANE0_SET, o.ts, (uint32_t)o.lanes);
}

// intel_dp_get_adjust_train / intel_dp_get_lane_adjust_vswing_preemph: per-lane levels (display >= 10), source max
// vswing level 3 (10-entry table) and max pre-emphasis level 3 (intel_ddi_dp_voltage_max/_preemph_max)
bool IntelXeLink::getAdjustTrain(const uint8_t st[6])
{
	bool changed = false;
	for (int lane = 0; lane < 4; lane++) {
		int l = lane < o.lanes - 1 ? lane : o.lanes - 1;
		uint8_t adj = (uint8_t)(st[4 + (l >> 1)] >> ((l & 1) * 4));
		uint8_t v = adj & 3, p = (uint8_t)((adj & 0xC) << 1);
		if (p >= 0x18) p = 0x18 | MAX_PREEMPH_REACHED;
		uint8_t vmax = (p & PREEMPH_MASK) == 0x00 ? 3 : (p & PREEMPH_MASK) == 0x08 ? 2 : (p & PREEMPH_MASK) == 0x10 ? 1 : 0;   // dp_voltage_max
		if (v > vmax) v = vmax;
		if (v >= 3) v = 3 | MAX_SWING_REACHED;
		uint8_t n = v | p;
		if (o.ts[lane] != n) { o.ts[lane] = n; changed = true; }
	}
	return changed;
}

namespace {
uint8_t laneStatus(const uint8_t st[6], int lane) { return (st[lane >> 1] >> ((lane & 1) * 4)) & 0xF; }
bool crOk(const uint8_t st[6], int lanes)   // drm_dp_clock_recovery_ok
{
	for (int l = 0; l < lanes; l++) if (!(laneStatus(st, l) & 1)) return false;
	return true;
}
bool eqOk(const uint8_t st[6], int lanes)   // drm_dp_channel_eq_ok
{
	if (!(st[2] & 1)) return false;
	for (int l = 0; l < lanes; l++) if ((laneStatus(st, l) & 7) != 7) return false;
	return true;
}
bool maxVswingReached(const uint8_t ts[4], int lanes)   // intel_dp_link_max_vswing_reached (8b/10b)
{
	for (int l = 0; l < lanes; l++)
		if (!(ts[l] & MAX_SWING_REACHED) || (ts[l] & 3) + ((ts[l] >> 3) & 3) != 3) return false;
	return true;
}
bool adjustChanged(const uint8_t a[6], const uint8_t b[6], int lanes)   // intel_dp_adjust_request_changed
{
	for (int l = 0; l < lanes; l++)
		if (((a[4 + (l >> 1)] >> ((l & 1) * 4)) & 0xF) != ((b[4 + (l >> 1)] >> ((l & 1) * 4)) & 0xF)) return true;
	return false;
}
}

// intel_dp_link_training_clock_recovery (DPRX, 8b/10b)
bool IntelXeLink::clockRecovery()
{
	uint8_t rd_interval = o.dpcd[0x0E] & 0x7F;   // drm_dp_read_clock_recovery_delay
	uint32_t delay = o.dpcd[0] >= 0x14 || rd_interval == 0 ? 100 : rd_interval * 4000u;
	// intel_dp_reset_link_train: levels to 0, TPS1 with scrambling off
	bzero(o.ts, sizeof o.ts);
	setSignalLevels();
	if (!setLinkTrain(1 | DP_LINK_SCRAMBLING_DISABLE)) { llog("CR: failed to enable link training"); return false; }
	int maxTries = o.dpcd[0] >= 0x14 ? 10 : 80, voltageTries = 1;
	bool maxVs = false;
	uint8_t st[6] = {}, old[6] = {};
	for (int tries = 0; tries < maxTries; tries++) {
		sleepUs(delay);
		if (!dpcdRead(DP_LANE0_1_STATUS, st, 6)) { llog("CR: failed to read link status"); return false; }
		if (crOk(st, o.lanes)) { llog("CR: done after %d tries, levels %02x %02x %02x %02x", tries + 1, o.ts[0], o.ts[1], o.ts[2], o.ts[3]); return true; }
		if (voltageTries == 5) { llog("CR: same voltage tried 5 times (status %02x %02x %02x %02x adj %02x %02x)", st[0], st[1], st[2], st[3], st[4], st[5]); return false; }
		if (maxVs) { llog("CR: max voltage swing reached (status %02x %02x adj %02x %02x)", st[0], st[1], st[4], st[5]); return false; }
		getAdjustTrain(st);
		llog("CR try %d: status %02x %02x, sink asks %02x %02x -> levels %02x %02x %02x %02x", tries + 1, st[0], st[1], st[4], st[5],
		     o.ts[0], o.ts[1], o.ts[2], o.ts[3]);
		if (!updateLinkTrain()) { llog("CR: failed to update link training"); return false; }
		voltageTries = adjustChanged(old, st, o.lanes) ? 1 : voltageTries + 1;
		bcopy(st, old, 6);
		if (maxVswingReached(o.ts, o.lanes)) maxVs = true;
	}
	llog("CR: failed %d times, giving up (status %02x %02x)", maxTries, st[0], st[1]);
	return false;
}

// intel_dp_link_training_channel_equalization: TPS4 (scrambled) if the sink has it, else TPS3, else TPS2
bool IntelXeLink::channelEq()
{
	uint8_t rd_interval = o.dpcd[0x0E] & 0x7F;   // drm_dp_read_channel_eq_delay
	uint32_t delay = rd_interval == 0 ? 400 : rd_interval * 4000u;
	uint8_t pattern = o.tps4 ? 7 : o.tps3 ? 3 : 2;
	if (pattern != 7) pattern |= DP_LINK_SCRAMBLING_DISABLE;
	if (!setLinkTrain(pattern)) { llog("EQ: failed to start channel equalization"); return false; }
	uint8_t st[6] = {};
	for (int tries = 0; tries < 5; tries++) {
		sleepUs(delay);
		if (!dpcdRead(DP_LANE0_1_STATUS, st, 6)) { llog("EQ: failed to read link status"); return false; }
		if (!crOk(st, o.lanes)) { llog("EQ: clock recovery lost (status %02x %02x %02x)", st[0], st[1], st[2]); return false; }
		if (eqOk(st, o.lanes)) { llog("EQ: done with TPS%c after %d tries, levels %02x %02x %02x %02x", (pattern & 0xF) == 7 ? '4' : '0' + (pattern & 0xF), tries + 1, o.ts[0], o.ts[1], o.ts[2], o.ts[3]); return true; }
		getAdjustTrain(st);
		llog("EQ try %d: status %02x %02x %02x, sink asks %02x %02x -> levels %02x %02x %02x %02x", tries + 1, st[0], st[1], st[2], st[4], st[5],
		     o.ts[0], o.ts[1], o.ts[2], o.ts[3]);
		if (!updateLinkTrain()) { llog("EQ: failed to update link training"); return false; }
	}
	llog("EQ: failed 5 times (status %02x %02x %02x)", st[0], st[1], st[2]);
	return false;
}

// intel_dp_post_lt_adj_req (only without TPS4, when the sink grants it). Linux waits up to 200 ms after each change and
// allows 6 changes; this also stops after 600 ms in all to keep the sysctl short.
bool IntelXeLink::postLtAdjReq()
{
	uint8_t st[6];
	if (!dpcdRead(DP_LANE0_1_STATUS, st, 6)) return false;
	uint64_t start = nowUs(), deadline = start + 200000;
	int changes = 0;
	bool timeout = false;
	for (;;) {
		if (!crOk(st, o.lanes) || !eqOk(st, o.lanes)) { llog("post-LT adjust: link lost (status %02x %02x %02x)", st[0], st[1], st[2]); return false; }
		if (!(st[2] & 0x02) || changes == 6 || timeout) { llog("post-LT adjust: done, %d changes%s", changes, timeout ? " (timeout)" : ""); return true; }
		IOSleep(5);
		if (!dpcdRead(DP_LANE0_1_STATUS, st, 6)) return false;
		uint64_t now = nowUs();
		if (getAdjustTrain(st)) {
			deadline = now + 200000;
			changes++;
			if (!updateLinkTrain()) return false;
		} else if (now > deadline) timeout = true;
		if (now > start + 600000) timeout = true;
	}
}

void IntelXeLink::setIdleLinkTrain()   // intel_ddi_set_idle_link_train (2 ms for DP_TP_STATUS_IDLE_DONE)
{
	rmw(reg(S_DP_TP_CTL), TP_TRAIN_MASK, TP_IDLE);
	waitOrWarn("idle patterns sent", reg(S_DP_TP_STATUS), TP_STATUS_IDLE_DONE, TP_STATUS_IDLE_DONE, 2000);
}

// intel_dp_start_link_train (no LTTPR: refused in precheck) + intel_dp_stop_link_train, as mtl_ddi_pre_enable_dp runs them
bool IntelXeLink::startLinkTrain()
{
	const Rate *r = rateFor(o.clock);
	bool postLt = o.postLtAdj && !o.tps4;   // intel_dp_use_post_lt_adj_req (DDI has set_idle_link_train)
	prepareLinkRetrain();
	// intel_dp_prepare_link_train: intel_dp_link_training_set_mode (no VRR, no PR: MSA-ignore 0; 8b/10b), then link BW
	uint8_t mode[2] = { 0, DP_SET_ANSI_8B10B };
	dpcdWrite(DP_DOWNSPREAD_CTRL, mode, 2);
	uint8_t bw[2] = { r ? r->bw : (uint8_t)0x14, (uint8_t)(o.lanes | (o.enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0) |
	                                                       (postLt ? DP_POST_LT_ADJ_REQ_GRANTED : 0)) };
	dpcdWrite(DP_LINK_BW_SET, bw, 2);
	// intel_dp_link_train_all_phys -> intel_dp_link_train_phy (DPRX)
	bool ok = clockRecovery() && channelEq();
	dpcdWrite1(DP_TRAINING_PATTERN_SET, DP_TRAINING_PATTERN_DISABLE);   // intel_dp_disable_dpcd_training_pattern
	setIdleLinkTrain();
	if (ok && postLt) ok = postLtAdjReq();
	if (postLt) dpcdWrite1(DP_LANE_COUNT_SET, (uint8_t)(o.lanes | (o.enhanced ? DP_LANE_COUNT_ENHANCED_FRAME_EN : 0)));   // intel_dp_stop_post_lt_adj_req
	llog("link training %s at %d kHz x%d", ok ? "passed" : "FAILED", o.clock, o.lanes);
	if (!ok) logFailure("training");
	// intel_dp_stop_link_train: DP_TP_CTL normal (mtl_ddi_pre_enable_dp calls it whether training passed or not)
	setTpCtlPattern(DP_TRAINING_PATTERN_DISABLE);
	return ok;
}

void IntelXeLink::setPower(uint8_t mode)   // intel_dp_set_power (D0: up to 3 tries 1 ms apart)
{
	if (o.dpcd[0] < 0x11) return;
	for (int i = 0; i < (mode == DP_SET_POWER_D0 ? 3 : 1); i++) {
		if (dpcdWrite1(DP_SET_POWER, mode)) return;
		IOSleep(1);
	}
	llog("set power D%d failed", mode == DP_SET_POWER_D0 ? 0 : 3);
}

void IntelXeLink::logFailure(const char *step)
{
	uint8_t s[6];
	if (dpcdRead(DP_LANE0_1_STATUS, s, 6))
		llog("%s failed: DPCD 202-207 %02x %02x %02x %02x %02x %02x", step, s[0], s[1], s[2], s[3], s[4], s[5]);
	else
		llog("%s failed: DPCD 202-207 unreadable", step);
	llog("%s failed: DP_TP_STATUS 0x%08x DP_TP_CTL 0x%08x PORT_BUF_CTL1 0x%08x PORT_BUF_CTL2 0x%08x PORT_CLOCK_CTL 0x%08x DDI_BUF_CTL 0x%08x, levels %02x %02x %02x %02x",
	     step, rd(reg(S_DP_TP_STATUS)), rd(reg(S_DP_TP_CTL)), rd(reg(S_BUF_CTL1)), rd(reg(S_BUF_CTL2)), rd(reg(S_CLOCK_CTL)),
	     rd(reg(S_DDI_BUF_CTL)), o.ts[0], o.ts[1], o.ts[2], o.ts[3]);
}

// ---- the actions -------------------------------------------------------------------------------------------------

// The output from the pipe's TRANS_DDI_FUNC_CTL (port select = port + 1), lane reversal from DDI_BUF_CTL, SSC from
// PORT_CLOCK_CTL. The AUX channel must be this port's (AUX A, or USBC n for TC n), the one the framebuffer talks to.
bool IntelXeLink::identify(int pipe)
{
	o = Out();
	o.pipe = o.trans = pipe;   // no eDP/DSI transcoders on BMG: transcoder = pipe
	uint32_t ctl = rd(TR(pipe, 0x60400));
	int sel = (int)((ctl >> 27) & 0xF);
	if (sel && (ctl & FUNC_MODE_MASK) == FUNC_MODE_SST) {
		uint32_t w = (ctl & FUNC_WIDTH_MASK) >> 1;
		o.port = sel - 1;
		o.lanes = w == 4 ? 3 : (int)w + 1;
	} else if (gLastOk && gLast.pipe == pipe) {
		llog("TRANS_DDI_FUNC_CTL 0x%08x selects no DP SST port: using the output of the last run (port %d x%d)", ctl, gLast.port, gLast.lanes);
		o.port = gLast.port;
		o.lanes = gLast.lanes;
	} else {
		llog("transcoder %c: TRANS_DDI_FUNC_CTL 0x%08x does not drive a DP SST port", 'A' + pipe, ctl);
		return false;
	}
	if (o.port != 0 && (o.port < PORT_TC1 || o.port > PORT_TC1 + 3)) { llog("port %d is not on BMG (A, TC1-TC4)", o.port); return false; }
	if (o.lanes != 1 && o.lanes != 2 && o.lanes != 4) { llog("lane count %d", o.lanes); return false; }
	o.phy = o.port >= PORT_TC1 ? 5 + o.port - PORT_TC1 : o.port;   // intel_port_to_phy, display 13+
	uint32_t aux = o.port == 0 ? 0x64010 : 0x16F210 + (uint32_t)(o.port - PORT_TC1) * 0x200;
	if (aux != fAuxFb) { llog("port %d uses AUX 0x%x but the framebuffer talks to 0x%x", o.port, aux, fAuxFb); return false; }
	uint32_t buf = rd(DDI_BUF_CTL(o.port)), pb1 = rd(PORT_BUF_CTL1(o.port));
	o.reversal = (buf & BUF_REVERSAL) || (pb1 & PB_REVERSAL);
	if (!(buf & BUF_REVERSAL) != !(pb1 & PB_REVERSAL)) llog("lane reversal differs: DDI_BUF_CTL 0x%08x, PORT_BUF_CTL1 0x%08x", buf, pb1);
	if (!(buf & BUF_REVERSAL) && !(pb1 & PB_REVERSAL) && gLastOk && gLast.port == o.port) o.reversal = gLast.reversal;
	o.ssc = rd(PORT_CLOCK_CTL(o.port)) & SSC_ENABLE_PLLB;
	if (!(rd(PORT_CLOCK_CTL(o.port)) & (PLL_REQ(0) | PLL_REQ(1))) && gLastOk && gLast.port == o.port) o.ssc = gLast.ssc;
	llog("output: pipe/transcoder %c, port %d (%s%d), PHY %c, x%d, lane reversal %d, SSC %d, AUX 0x%x", 'A' + pipe, o.port,
	     o.port ? "TC" : "A", o.port ? o.port - PORT_TC1 + 1 : 0, 'A' + o.phy, o.lanes, o.reversal, o.ssc, aux);
	return true;
}

// Everything that can refuse runs before the first write. Also the read-only dump (action 0).
int IntelXeLink::precheck(int action)
{
	uint32_t dc = snap(S_DC_STATE);
	if (dc & 0x3) { llog("DC_STATE_EN 0x%08x: DC5/DC6 allowed, the PHY message bus needs DC off", dc); return R_DC_ON; }
	if (!readSinkCaps()) return R_DPCD;
	logDpcd(0x000, 16);
	logDpcd(0x100, 9);
	logDpcd(0x600, 1);
	logLinkStatus("now");
	uint8_t ls[2];
	if (!dpcdRead(DP_LINK_BW_SET, ls, 2)) return R_DPCD;
	const Rate *r = rateForBw(ls[0]);
	int dpcdLanes = ls[1] & 0x1F;
	uint32_t valfreq = snap(S_CLK_VALFREQ);
	if (!r && gLastOk && gLast.port == o.port) { llog("sink LINK_BW_SET 0x%02x: using the last run's rate", ls[0]); r = rateFor(gLast.clock); }
	if (!r && rateFor((int)valfreq)) { llog("sink LINK_BW_SET 0x%02x: using DDI_CLK_VALFREQ %u", ls[0], valfreq); r = rateFor((int)valfreq); }
	if (!r) { llog("sink LINK_BW_SET 0x%02x is not an 8b/10b rate this file has a table for", ls[0]); return R_UNSUPPORTED; }
	o.clock = r->clock;
	llog("link: %d kHz x%d (sink LINK_BW_SET 0x%02x, LANE_COUNT_SET 0x%02x, DDI_CLK_VALFREQ %u)", o.clock, o.lanes, ls[0], ls[1], valfreq);
	if (dpcdLanes && dpcdLanes != o.lanes) llog("note: sink lane count %d, transcoder %d", dpcdLanes, o.lanes);
	if (valfreq && valfreq != (uint32_t)o.clock) llog("note: DDI_CLK_VALFREQ %u differs from the link rate", valfreq);
	int lttpr = lttprCount();
	if (lttpr) { llog("LTTPR count %d: non-transparent LTTPR training is not implemented", lttpr); if (action != kDump) return R_UNSUPPORTED; }
	logDpcd(0xF0000, 8);
	if (snap(S_VRR_CTL) & EN) { llog("VRR timing generator on (TRANS_VRR_CTL 0x%08x)", snap(S_VRR_CTL)); if (action != kDump) return R_UNSUPPORTED; }
	if (snap(S_DP_TP_CTL) & (TP_FEC | TP_MODE_MST)) { llog("DP_TP_CTL 0x%08x: FEC or MST", snap(S_DP_TP_CTL)); if (action != kDump) return R_UNSUPPORTED; }
	if (snap(S_DP2_CTL)) { llog("TRANS_DP2_CTL 0x%08x: 128b/132b", snap(S_DP2_CTL)); if (action != kDump) return R_UNSUPPORTED; }
	// planes 2-5 and the cursor of this pipe must be off: only plane 1 is turned off and back on here
	for (int n = 2; n <= 5; n++) {
		uint32_t c = rd(TR(o.pipe, 0x70180 + (uint32_t)(n - 1) * 0x100));
		if (c & EN) { llog("plane %d of pipe %c is on (0x%08x)", n, 'A' + o.pipe, c); if (action != kDump) return R_UNSUPPORTED; }
	}
	uint32_t cur = rd(TR(o.pipe, 0x70080));
	if (cur & 0x27) { llog("cursor of pipe %c is on (0x%08x)", 'A' + o.pipe, cur); if (action != kDump) return R_UNSUPPORTED; }
	llog("PMDEMAND 0x%08x 0x%08x (phys %u, plls %u, ddiclk %u MHz): unchanged, the end state equals the start state",
	     snap(S_PMDEMAND0), snap(S_PMDEMAND1), snap(S_PMDEMAND0) & 7, snap(S_PMDEMAND1) & 7, (snap(S_PMDEMAND1) >> 8) & 0x7FF);
	// the PHY: message bus, the C20 context in use, the levels the GOP trained to
	// The retrain and the readback modeset need it; the table modeset goes on without it, since its lane reset may be
	// what brings the bus back after an earlier failed run.
	fReadbackOk = c20Readout(&fReadback);
	if (!fReadbackOk) {
		llog("C20 readout over the message bus failed");
		if (action == kRetrain) return R_MSGBUS;
	} else {
		if (!gFirstPllOk) { gFirstPll = fReadback; gFirstPllOk = true; gFirstPllClock = o.clock; }
		logC20("C20 readback", fReadback);
		logC20("Linux table ", r->pll);
		bool same = !bcmp(fReadback.tx, r->pll.tx, sizeof r->pll.tx) && !bcmp(fReadback.cmn, r->pll.cmn, sizeof r->pll.cmn) &&
		            !bcmp(fReadback.mpll, r->pll.mpll, sizeof r->pll.mpll) && fReadback.serdes == r->pll.serdes && fReadback.width == r->pll.width;
		llog("C20 readback %s the Linux table for %d kHz", same ? "matches" : "DIFFERS from", o.clock);
	}
	logPhyLevels();
	uint32_t clk = snap(S_CLOCK_CTL);
	llog("PORT_CLOCK_CTL 0x%08x: PLL req/ack lane0 %u/%u lane1 %u/%u, refclk req/ack lane0 %u/%u lane1 %u/%u, clock select %u, "
	     "ungate %u, lane1 clock select %u, SSC A/B %u/%u (Linux would use lane %d for maxPCLK)",
	     clk, !!(clk & PLL_REQ(0)), !!(clk & PLL_ACK(0)), !!(clk & PLL_REQ(1)), !!(clk & PLL_ACK(1)), !!(clk & REFCLK_REQ(0)),
	     !!(clk & REFCLK_ACK(0)), !!(clk & REFCLK_REQ(1)), !!(clk & REFCLK_ACK(1)), (clk & DDI_CLOCK_SELECT_MASK) >> 12,
	     !!(clk & FORWARD_CLOCK_UNGATE), !!(clk & LANE1_PHY_CLOCK_SELECT), !!(clk & SSC_ENABLE_PLLA), !!(clk & SSC_ENABLE_PLLB),
	     o.reversal ? 1 : 0);
	if (action == kRetrain) {
		if (!(clk & (PLL_ACK(0) | PLL_ACK(1))) || !(snap(S_DDI_BUF_CTL) & EN)) {
			llog("retrain needs the PLL locked and the DDI on (PORT_CLOCK_CTL 0x%08x, DDI_BUF_CTL 0x%08x): use action 2",
			     snap(S_CLOCK_CTL), snap(S_DDI_BUF_CTL));
			return R_LINK_DOWN;
		}
	}
	if (action == kModesetReadback) {
		if (!gFirstPllOk || gFirstPllClock != o.clock) { llog("no first readback at %d kHz to reuse", o.clock); return R_UNSUPPORTED; }
		fReadback = gFirstPll;
		logC20("action 3 uses the first readback", fReadback);
		if ((fReadback.serdes & C20_IS_DP) == 0) { llog("first readback is not a DP context"); return R_UNSUPPORTED; }
	}
	return R_OK;
}

// After the enable: the sink's view, frames moving, no underrun
int IntelXeLink::verify()
{
	waitVblank(50);
	uint32_t iir = rd(reg(S_PIPE_IIR));
	wr(reg(S_PIPE_IIR), PIPE_FIFO_UNDERRUN);   // W1C: start clean after the plane has latched
	uint32_t f0 = rd(reg(S_FRMCOUNT));
	uint32_t d0 = rd(reg(S_PIPEDSL)) & 0xFFFFF;
	IOSleep(5);
	uint32_t d1 = rd(reg(S_PIPEDSL)) & 0xFFFFF;
	IOSleep(95);
	uint32_t f1 = rd(reg(S_FRMCOUNT));
	uint32_t iir2 = rd(reg(S_PIPE_IIR));
	uint32_t conf = rd(reg(S_TRANSCONF));
	uint8_t s[6];
	bool st = dpcdRead(0x200, s, 6);
	logLinkStatus("after");
	bool trained = st && eqOk(s + 2, o.lanes) && (s[5] & 1);
	llog("verify: TRANSCONF 0x%08x, frames %u in 100 ms (FRMCOUNT %u -> %u), PIPEDSL %u -> %u, IIR before 0x%08x after 0x%08x (underrun %s)",
	     conf, f1 - f0, f0, f1, d0, d1, iir, iir2, iir2 & PIPE_FIFO_UNDERRUN ? "YES" : "no");
	llog("verify: DP_TP_CTL 0x%08x DP_TP_STATUS 0x%08x DDI_BUF_CTL 0x%08x PORT_BUF_CTL1 0x%08x PORT_BUF_CTL2 0x%08x PORT_CLOCK_CTL 0x%08x FUNC_CTL 0x%08x PLANE_CTL 0x%08x",
	     rd(reg(S_DP_TP_CTL)), rd(reg(S_DP_TP_STATUS)), rd(reg(S_DDI_BUF_CTL)), rd(reg(S_BUF_CTL1)), rd(reg(S_BUF_CTL2)),
	     rd(reg(S_CLOCK_CTL)), rd(reg(S_FUNC_CTL)), rd(reg(S_PLANE_CTL)));
	logPhyLevels();
	bool ok = (conf & TRANSCONF_STATE) && f1 != f0 && trained && !(iir2 & PIPE_FIFO_UNDERRUN);
	llog("verify: %s (transcoder %s, frames %s, sink %s, underrun %s)", ok ? "OK" : "FAILED", conf & TRANSCONF_STATE ? "on" : "OFF",
	     f1 != f0 ? "advancing" : "STOPPED", trained ? "trained and in sync" : "NOT trained/in sync", iir2 & PIPE_FIFO_UNDERRUN ? "YES" : "none");
	return ok ? R_OK : R_VERIFY;
}

// Retrain at the same rate and lane count: transcoder and DDI down, PLL and PHY clocking untouched, DDI up and trained.
// Linux has no such path any more (intel_dp_retrain_link does a full modeset); the steps are the disable and enable
// halves of hsw_crtc_disable/hsw_crtc_enable without intel_dpll_disable/enable, the DPCD D3/D0 and TRANS_CLK_SEL.
int IntelXeLink::retrain()
{
	llog("retrain: %d kHz x%d, PLL kept", o.clock, o.lanes);
	// intel_crtc_disable_planes, intel_ddi_post_disable_hdmi_or_sst
	planesOff();
	transcoderOff();
	funcOff();
	scalersOff();
	// intel_ddi_post_disable_dp (D3 and TRANS_CLK_SEL left alone)
	bufDisable();
	// mtl_ddi_pre_enable_dp
	funcConfig();
	bool ok = startLinkTrain();
	if (!ok) {   // second attempt (Linux: seq_train_failures < MAX_SEQ_TRAIN_FAILURES), from a disabled DDI again
		llog("retrying the training once");
		bufDisable();
		ok = startLinkTrain();
	}
	// intel_ddi_enable, then the planes
	funcEnable();
	transcoderOn();
	planesOn();
	int v = verify();
	return ok ? v : R_TRAINING;
}

// Full disable of the output including its PLL (hsw_crtc_disable), then the full enable at the same mode
// (hsw_crtc_enable) with the C20 context from Linux's table (or, readback = true, the one read from the PHY before).
int IntelXeLink::modeset(bool readback)
{
	const Rate *r = rateFor(o.clock);
	const C20Pll &pll = readback ? fReadback : r->pll;
	llog("full modeset: %d kHz x%d, C20 context from %s", o.clock, o.lanes, readback ? "the PHY readback" : "the Linux table");
	// hsw_crtc_disable: intel_encoders_disable (nothing for this sink: no PSR/DSC/MSA-ignore was set), post_disable
	planesOff();
	transcoderOff();
	funcOff();
	scalersOff();
	// intel_ddi_post_disable_dp
	setPower(DP_SET_POWER_D3);
	bufDisable();
	wr(reg(S_DP2_CTL), 0);                      // intel_ddi_config_transcoder_dp2(false)
	wr(reg(S_TRANS_CLK_SEL), 0);                // intel_ddi_disable_transcoder_clock
	rmw(reg(S_BUF_CTL1), PB_IO_SELECT_TBT, 0);  // de-select Thunderbolt
	// intel_dpll_disable -> intel_mtl_pll_disable
	pllDisable();
	// hsw_crtc_enable: intel_dpll_enable -> intel_mtl_pll_enable
	bool pllOk = pllEnable(pll);
	// intel_encoders_pre_enable -> mtl_ddi_pre_enable_dp
	rmw(reg(S_BUF_CTL1), PB_IO_SELECT_TBT, 0);                     // mtl_port_buf_ctl_io_selection (not TBT)
	wr(reg(S_TRANS_CLK_SEL), (uint32_t)(o.phy + 1) << 28);         // intel_ddi_enable_transcoder_clock (TGL_TRANS_CLK_SEL_PORT(phy))
	funcConfig();
	setPower(DP_SET_POWER_D0);
	bool ok = startLinkTrain();
	if (!ok) {
		llog("retrying the training once");
		bufDisable();
		ok = startLinkTrain();
	}
	wr(reg(S_MSA_MISC), snap(S_MSA_MISC));   // intel_ddi_set_dp_msa (the GOP's value)
	replayPipeConfig();
	// intel_encoders_enable -> intel_ddi_enable, then the planes
	funcEnable();
	transcoderOn();
	planesOn();
	int v = verify();
	return !pllOk ? R_PLL : !ok ? R_TRAINING : v;
}

// Display sleep with the output off (DPMS): the disable half of modeset(), as hsw_crtc_disable + intel_ddi_post_disable_dp
// + intel_dpll_disable run it, sink in D3. The pipe's power well and the transcoder timing registers stay, so the wake
// is action 2 (full modeset at the same mode, which replays them). gLast keeps the output for that run, since
// TRANS_DDI_FUNC_CTL no longer selects a port.
int IntelXeLink::powerDown()
{
	llog("power down: %d kHz x%d, sink to D3, PLL off", o.clock, o.lanes);
	planesOff();
	transcoderOff();
	funcOff();
	scalersOff();
	setPower(DP_SET_POWER_D3);
	bufDisable();
	wr(reg(S_DP2_CTL), 0);
	wr(reg(S_TRANS_CLK_SEL), 0);
	rmw(reg(S_BUF_CTL1), PB_IO_SELECT_TBT, 0);
	pllDisable();
	uint32_t conf = rd(reg(S_TRANSCONF));
	llog("power down: TRANSCONF 0x%08x DDI_BUF_CTL 0x%08x PORT_CLOCK_CTL 0x%08x", conf, rd(reg(S_DDI_BUF_CTL)), rd(reg(S_CLOCK_CTL)));
	return (conf & TRANSCONF_STATE) ? R_VERIFY : R_OK;
}

int IntelXeLink::run(int action, int pipe, bool planeOn)
{
	gLogLen = 0;
	gLog[0] = 0;
	fPhyErrs = fWarns = 0;
	fPlaneOn = planeOn;
	uint64_t t0 = nowUs();
	static const char *const kNames[] = { "dump (read-only)", "retrain", "full modeset (Linux C20 table)", "full modeset (C20 readback)",
	                                      "power down (DPMS)", "save state", "full modeset from the saved state" };
	if (action < kDump || action > kRestore) return R_UNSUPPORTED;
	llog("action %d: %s, pipe %c, plane %s at the end", action, kNames[action], 'A' + pipe, planeOn ? "on" : "off (display asleep)");
	if (!identify(pipe)) return R_IDENTIFY;
	takeSnapshot();
	if (action == kSave) {
		for (int i = 0; i < S_NUM; i++) gSaved[i] = fSnap[i].val;
		gSavedOk = true; gLast = o; gLastOk = true;
		llog("saved %d registers (TRANSCONF 0x%08x DDI_BUF_CTL 0x%08x)", (int)S_NUM, snap(S_TRANSCONF), snap(S_DDI_BUF_CTL));
		return R_OK;
	}
	if (action == kRestore) {
		if (!gSavedOk) { llog("nothing saved"); return R_UNSUPPORTED; }
		// the configuration as it was before sleep; live status registers stay live
		for (int i = 0; i < S_NUM; i++)
			if (i != S_PIPE_IIR && i != S_FRMCOUNT && i != S_PIPEDSL && i != S_DC_STATE && i != S_SDEISR) fSnap[i].val = gSaved[i];
		llog("restoring from the saved state");
	}
	logSnapshot();
	int r = precheck(action == kRestore ? kModeset : action);
	if (r == R_OK) {
		if (action == kRetrain) r = retrain();
		else if (action == kModeset) r = modeset(false);
		else if (action == kModesetReadback) r = modeset(true);
		else if (action == kPowerDown) r = powerDown();
		else if (action == kRestore) r = modeset(false);
	}
	if (r == R_OK) { gLast = o; gLastOk = true; }
	llog("result %d: %d PHY message bus errors, %d timeouts, %llu ms", r, fPhyErrs, fWarns, (unsigned long long)((nowUs() - t0) / 1000));
	return r;
}

// ---- an output nothing drives yet: the second pipe ---------------------------------------------------------------
// The output from the caller's plan instead of TRANS_DDI_FUNC_CTL. Lane reversal is read as identify() reads it
// (Linux takes it from the VBT, which the GOP also programs into DDI_BUF_CTL/PORT_BUF_CTL1); SSC is the caller's. The
// snapshot is taken at the new pipe's/port's offsets, then the values the pipe-A modeset would replay from the GOP are
// replaced by the computed ones, so funcCtlValue, replayPipeConfig, planesOn and verify run unchanged.
bool IntelXeLink::setupNew(const NewOutput &n)
{
	o = Out();
	o.pipe = o.trans = n.pipe;
	o.port = n.port;
	o.lanes = n.lanes;
	o.clock = n.clock;
	if (n.pipe < 1 || n.pipe > 3) { llog("new output: pipe %d is not a second pipe", n.pipe); return false; }
	if (o.port != 0 && (o.port < PORT_TC1 || o.port > PORT_TC1 + 3)) { llog("new output: port %d is not on BMG (A, TC1-TC4)", o.port); return false; }
	if (o.lanes != 1 && o.lanes != 2 && o.lanes != 4) { llog("new output: lane count %d", o.lanes); return false; }
	if (!rateFor(o.clock)) { llog("new output: %d kHz is not an 8b/10b rate this file has a C20 table for", o.clock); return false; }
	o.phy = o.port >= PORT_TC1 ? 5 + o.port - PORT_TC1 : o.port;   // intel_port_to_phy, display 13+
	const uint32_t aux = o.port == 0 ? 0x64010 : 0x16F210 + (uint32_t)(o.port - PORT_TC1) * 0x200;
	if (aux != fAuxFb) { llog("new output: port %d uses AUX 0x%x, the caller talks to 0x%x", o.port, aux, fAuxFb); return false; }
	const uint32_t buf = rd(DDI_BUF_CTL(o.port)), pb1 = rd(PORT_BUF_CTL1(o.port));
	o.reversal = (buf & BUF_REVERSAL) || (pb1 & PB_REVERSAL);
	o.ssc = n.ssc;
	takeSnapshot();
	fSnap[S_FUNC_CTL].val = n.funcBpcSync;
	fSnap[S_HTOTAL].val = n.htotal; fSnap[S_HBLANK].val = n.hblank; fSnap[S_HSYNC].val = n.hsync;
	fSnap[S_VTOTAL].val = n.vtotal; fSnap[S_VBLANK].val = n.vblank; fSnap[S_VSYNC].val = n.vsync;
	fSnap[S_VSYNCSHIFT].val = n.vsyncshift; fSnap[S_SCL].val = n.scl; fSnap[S_MULT].val = n.mult;
	fSnap[S_DATA_M1].val = n.dataM; fSnap[S_DATA_N1].val = n.dataN; fSnap[S_LINK_M1].val = n.linkM; fSnap[S_LINK_N1].val = n.linkN;
	fSnap[S_PIPESRC].val = n.pipesrc; fSnap[S_PIPE_MISC].val = n.pipeMisc;
	fSnap[S_PIPE_CHICKEN].val |= n.pipeChickenSet;               // icl_set_pipe_chicken is a read-modify-write
	fSnap[S_CHICKEN_TRANS].val &= ~FRAME_START_DELAY_MASK;        // hsw_set_frame_start_delay: framestart_delay 1 -> 0
	fSnap[S_TRANSCONF].val = n.transconf;
	fSnap[S_LINETIME].val = n.linetime;
	fSnap[S_MSA_MISC].val = n.msaMisc;
	fSnap[S_PLANE_CTL].val = n.planeCtl; fSnap[S_PLANE_SURF].val = n.planeSurf;
	fSnap[S_PS0_CTRL].val = 0; fSnap[S_PS1_CTRL].val = 0;          // no pipe or plane scaler
	llog("new output: pipe/transcoder %c, port %d (%s%d), PHY %c, x%d at %d kHz, lane reversal %d, SSC %d, AUX 0x%x", 'A' + o.pipe,
	     o.port, o.port ? "TC" : "A", o.port ? o.port - PORT_TC1 + 1 : 0, 'A' + o.phy, o.lanes, o.clock, o.reversal, o.ssc, aux);
	return true;
}

// icl_plane_update_noarm's registers for plane 1 (linear XRGB8888 at 0,0, no colour key, no chroma upsampler) and
// skl_write_plane_wm / skl_write_cursor_wm: the levels, the transition watermark, PLANE_BUF_CFG and the cursor's DDB
// block (its watermarks 0: the cursor stays off). clear: what skl_plane_disable_arm leaves (watermarks and DDB 0).
void IntelXeLink::newPlaneNoarm(const NewOutput &n, bool clear)
{
	const int p = o.pipe;
	if (!clear) {
		wr(TR(p, 0x70188), n.planeStride);   // PLANE_STRIDE
		wr(TR(p, 0x7018C), 0);               // PLANE_POS
		wr(TR(p, 0x70190), n.planeSize);     // PLANE_SIZE
		wr(TR(p, 0x70194), 0); wr(TR(p, 0x70198), 0); wr(TR(p, 0x701A0), 0);   // PLANE_KEYVAL, KEYMSK, KEYMAX
		wr(TR(p, 0x701A4), 0);               // PLANE_OFFSET
		wr(TR(p, 0x701C8), 0);               // PLANE_CUS_CTL
		wr(TR(p, 0x701CC), n.planeColorCtl); // PLANE_COLOR_CTL
	}
	for (int l = 0; l < 8; l++) wr(TR(p, 0x70240) + (uint32_t)l * 4, clear ? 0 : n.planeWm[l]);   // PLANE_WM_1 level l
	wr(TR(p, 0x70268), clear ? 0 : n.planeWmTrans);                                            // PLANE_WM_TRANS_1
	wr(TR(p, 0x7027C), clear ? 0 : n.planeBufCfg);                                             // PLANE_BUF_CFG_1
	for (int l = 0; l < 8; l++) wr(TR(p, 0x70140) + (uint32_t)l * 4, 0);                       // CUR_WM level l
	wr(TR(p, 0x70168), 0);                                                                     // CUR_WM_TRANS
	wr(TR(p, 0x7017C), clear ? 0 : n.curBufCfg);                                               // CUR_BUF_CFG
}

// hsw_crtc_enable for a pipe on an unused output, after the caller powered the pipe and its DBUF slices: the checks of
// precheck() that apply (DC off, sink caps, no LTTPR, nothing on this pipe or port), then modeset()'s enable half with
// the computed values, then verify().
int IntelXeLink::enableNew(const NewOutput &n)
{
	const Rate *r = rateFor(o.clock);
	if (snap(S_DC_STATE) & 0x3) { llog("DC_STATE_EN 0x%08x: DC5/DC6 allowed, the PHY message bus needs DC off", snap(S_DC_STATE)); return R_DC_ON; }
	if ((rd(reg(S_TRANSCONF)) & (EN | TRANSCONF_STATE)) || (snap(S_DDI_BUF_CTL) & EN) || (snap(S_CLOCK_CTL) & (PLL_REQ(0) | PLL_REQ(1)))) {
		llog("new output: pipe %c or port %d is in use (TRANSCONF 0x%08x, DDI_BUF_CTL 0x%08x, PORT_CLOCK_CTL 0x%08x)", 'A' + o.pipe,
		     o.port, rd(reg(S_TRANSCONF)), snap(S_DDI_BUF_CTL), snap(S_CLOCK_CTL));
		return R_UNSUPPORTED;
	}
	if (!readSinkCaps()) return R_DPCD;
	logDpcd(0x000, 16);
	logDpcd(0x200, 8);
	const int lttpr = lttprCount();
	if (lttpr) { llog("LTTPR count %d: non-transparent LTTPR training is not implemented", lttpr); return R_UNSUPPORTED; }
	const uint8_t maxRate = o.dpcd[1], maxLanes = o.dpcd[2] & 0x1F;
	if ((int)maxRate * 27000 < o.clock || maxLanes < o.lanes) { llog("sink caps 0x%02x x%u below %d kHz x%d", maxRate, maxLanes, o.clock, o.lanes); return R_UNSUPPORTED; }
	llog("enable: %d kHz x%d, C20 context from the Linux table", o.clock, o.lanes);
	// intel_dpll_enable -> intel_mtl_pll_enable
	const bool pllOk = pllEnable(r->pll);
	// intel_encoders_pre_enable -> mtl_ddi_pre_enable_dp
	rmw(reg(S_BUF_CTL1), PB_IO_SELECT_TBT, 0);
	wr(reg(S_TRANS_CLK_SEL), (uint32_t)(o.phy + 1) << 28);
	funcConfig();
	setPower(DP_SET_POWER_D0);
	bool ok = startLinkTrain();
	if (!ok) {
		llog("retrying the training once");
		bufDisable();
		ok = startLinkTrain();
	}
	wr(reg(S_MSA_MISC), snap(S_MSA_MISC));
	replayPipeConfig();
	// the plane update after intel_enable_crtc: noarm registers and watermarks, then (planesOn) PLANE_CTL and PLANE_SURF
	newPlaneNoarm(n, false);
	funcEnable();
	transcoderOn();
	planesOn();
	const int v = verify();
	return !pllOk ? R_PLL : !ok ? R_TRAINING : v;
}

// hsw_crtc_disable for the second pipe: planes off, transcoder and DDI down, sink to D3, PLL off (powerDown's steps),
// then skl_plane_disable_arm's watermark/DDB clear so its DDB range is free for the next allocation
int IntelXeLink::disableNew(const NewOutput &n)
{
	llog("disable: pipe %c, port %d", 'A' + o.pipe, o.port);
	if (!readSinkCaps()) llog("disable: sink gone (no DPCD), no D3 request");   // setPower needs the DPCD revision
	planesOff();
	transcoderOff();
	funcOff();
	scalersOff();
	setPower(DP_SET_POWER_D3);
	bufDisable();
	wr(reg(S_DP2_CTL), 0);
	wr(reg(S_TRANS_CLK_SEL), 0);
	rmw(reg(S_BUF_CTL1), PB_IO_SELECT_TBT, 0);
	pllDisable();
	newPlaneNoarm(n, true);
	wr(reg(S_PLANE_SURF), 0);
	const uint32_t conf = rd(reg(S_TRANSCONF));
	llog("disable: TRANSCONF 0x%08x DDI_BUF_CTL 0x%08x PORT_CLOCK_CTL 0x%08x", conf, rd(reg(S_DDI_BUF_CTL)), rd(reg(S_CLOCK_CTL)));
	return (conf & TRANSCONF_STATE) ? R_VERIFY : R_OK;
}

int IntelXeLink::runNew(int action, const NewOutput &n)
{
	gLogLen = 0;
	gLog[0] = 0;
	fPhyErrs = fWarns = 0;
	fPlaneOn = true;
	const uint64_t t0 = nowUs();
	if (action != kEnableNew && action != kDisableNew) return R_UNSUPPORTED;
	llog("action %d: %s of an output nothing else drives (pipe %c)", action, action == kEnableNew ? "enable" : "disable", 'A' + n.pipe);
	if (!setupNew(n)) return R_IDENTIFY;
	logSnapshot();
	const int r = action == kEnableNew ? enableNew(n) : disableNew(n);
	llog("result %d: %d PHY message bus errors, %d timeouts, %llu ms", r, fPhyErrs, fWarns, (unsigned long long)((nowUs() - t0) / 1000));
	return r;
}
