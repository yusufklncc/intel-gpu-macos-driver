// SPDX-License-Identifier: MIT
// IntelXeLink: DP link retraining and full modeset for one DDI of the Arc B580 (display 14.01, C20 PHYs, no Type-C/FIA).
// Run only from IntelXeFB.cpp (the debug.intelxefb_link sysctl, display sleep/wake, hotplug recovery and, behind
// -intelxefb_pipeb, the second pipe), never on its own. Sequences follow Linux
// drivers/gpu/drm/i915/display (intel_ddi.c, intel_cx0_phy.c, intel_dp_link_training.c, intel_display.c); see
// IntelXeLink.cpp for the step-by-step mapping.
#ifndef INTELXELINK_H
#define INTELXELINK_H

#include <IOKit/IOTypes.h>

class IntelXeLink {
public:
	typedef int (*DpcdFn)(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len, bool write);
	enum Action { kDump = 0, kRetrain = 1, kModeset = 2, kModesetReadback = 3, kPowerDown = 4, kSave = 5, kRestore = 6 };
	// kSave: snapshot this output's registers (read only) for kRestore, the full modeset after system sleep cut the
	// display's power (the registers it replays are then gone); kRestore replays the saved values instead of live ones
	// regs: BAR0 MMIO; aux: AUX_CH_CTL the framebuffer's dpcd() talks to; dpcd(ctx, ...): native DPCD access over it
	IntelXeLink(volatile uint8_t *regs, uint32_t aux, DpcdFn dpcd, void *ctx) : fRegs(regs), fAuxFb(aux), fDpcdFn(dpcd), fCtx(ctx) {}
	// action on the output that feeds pipe `pipe` (transcoder = pipe); planeOn: whether plane 1 should scan out at the end
	// (display sleep keeps it off). Returns 0 or a negative step code; the text log is in logText().
	int run(int action, int pipe, bool planeOn);
	static const char *logText();
	static uint32_t logLength();

	// The output, as found on the hardware and in the sink's DPCD (enum port/phy numbering as Linux)
	struct Out {
		int port = -1;          // enum port: A 0, B 1, TC1 3 .. TC4 6
		int phy = -1;           // enum phy: TC1..4 = PHY_F..I (5..8) on display 13+ (intel_port_to_phy)
		int pipe = 0, trans = 0;
		int lanes = 0;          // 1, 2, 4
		int clock = 0;          // port clock in kHz (8b/10b only: 162000, 270000, 540000, 810000)
		bool reversal = false;  // DDI_BUF_CTL / PORT_BUF_CTL1 bit 16 as the GOP set it (VBT lane reversal)
		bool enhanced = false;  // sink DPCD 0x002 bit 7
		bool ssc = false;       // PORT_CLOCK_CTL SSC_ENABLE_PLLB as the GOP set it
		bool tps3 = false, tps4 = false, postLtAdj = false;
		uint8_t dpcd[16] = {};  // receiver caps 0x000-0x00F (extended caps 0x2200 if present)
		uint8_t ts[4] = {};     // train_set per lane (DPCD 0x103-0x106 format)
	};
	struct C20Pll { uint16_t tx[3], cmn[4], mpll[11]; uint8_t width, serdes; };

	// An output nothing drives yet: the second pipe (docs/design/DESIGN-second-pipe.md). Every value the full modeset
	// replays from the GOP's snapshot for pipe A is given here instead, computed by the caller (IntelXeDisplayPlan.h).
	// kEnableNew: hsw_crtc_enable for it (PLL, link training, pipe/transcoder, plane 1 with its DDB and watermarks);
	// kDisableNew: hsw_crtc_disable + intel_ddi_post_disable_dp + intel_dpll_disable, plane DDB/WM cleared. The global
	// steps around them (PMDEMAND, DBUF slices, MBUS, the pipe's power well) are the caller's. Never touches
	// gLast/gSaved/gFirstPll, the panel's recovery state.
	enum { kEnableNew = 7, kDisableNew = 8 };
	struct NewOutput {
		int port, pipe, lanes, clock;     // enum port, pipe = transcoder, lanes 1/2/4, port clock kHz (8b/10b)
		bool ssc;                          // SSC_ENABLE_PLLB (intel_cx0pll_update_ssc: VBT SSC and sink downspread)
		uint32_t funcBpcSync;              // TRANS_DDI_FUNC_CTL BPC 22:20 | PVSYNC 17 | PHSYNC 16
		uint32_t htotal, hblank, hsync, vtotal, vblank, vsync, vsyncshift, scl, mult;
		uint32_t dataM, dataN, linkM, linkN, pipesrc, pipeMisc, pipeChickenSet, transconf, linetime, msaMisc;
		uint32_t planeCtl, planeStride, planeSize, planeSurf, planeColorCtl, planeBufCfg, planeWm[8], planeWmTrans, curBufCfg;
	};
	int runNew(int action, const NewOutput &n);

private:
	volatile uint8_t *fRegs;
	uint32_t fAuxFb;
	DpcdFn fDpcdFn;
	void *fCtx;
	Out o;
	int fPhyErrs = 0, fWarns = 0;
	bool fPlaneOn = true;
	C20Pll fReadback = {};
	bool fReadbackOk = false;
	enum { kMaxRegs = 64 };
	struct Reg { const char *name; uint32_t off, val; };
	Reg fSnap[kMaxRegs] = {};

	uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(fRegs + off); }
	void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fRegs + off) = v; }
	void rmw(uint32_t off, uint32_t clear, uint32_t set) { wr(off, (rd(off) & ~clear) | set); }
	bool waitBits(uint32_t off, uint32_t mask, uint32_t want, uint32_t us, uint32_t *last = nullptr);
	bool waitOrWarn(const char *what, uint32_t off, uint32_t mask, uint32_t want, uint32_t us);
	static void sleepUs(uint32_t us);

	// registers of this output
	uint32_t snap(int id) const { return fSnap[id].val; }
	uint32_t reg(int id) const { return fSnap[id].off; }
	void takeSnapshot();
	void logSnapshot();

	// DPCD
	bool dpcdRead(uint32_t addr, uint8_t *buf, uint32_t len);
	bool dpcdWrite(uint32_t addr, const uint8_t *buf, uint32_t len);
	bool dpcdWrite1(uint32_t addr, uint8_t v) { return dpcdWrite(addr, &v, 1); }
	void logDpcd(uint32_t addr, uint32_t len);
	void logLinkStatus(const char *when);
	bool readSinkCaps();
	int lttprCount();

	// CX0 message bus (intel_cx0_phy.c)
	void busReset(int lane);
	void clearResponse(int lane);
	int waitAck(int lane, uint32_t cmd, uint32_t *val);
	int msgReadOnce(int lane, uint16_t addr);
	int msgWriteOnce(int lane, uint16_t addr, uint8_t data, bool committed);
	int msgRead(int lane, uint16_t addr);
	bool msgWrite(int laneMask, uint16_t addr, uint8_t data, bool committed);
	bool msgRmw(int laneMask, uint16_t addr, uint8_t clear, uint8_t set, bool committed);
	bool sramWrite(uint16_t addr, uint16_t data);
	int sramRead(uint16_t addr);
	void msgbusTimer();
	bool c20Readout(C20Pll *p);
	void logC20(const char *what, const C20Pll &p);
	void logPhyLevels();

	// PHY / PLL (intel_cx0_phy.c)
	void powerdownChange(int laneMask, uint8_t state);
	void laneReset();
	bool c20Program(const C20Pll &p);
	void programPhyLanes();
	bool pllEnable(const C20Pll &p);
	void pllDisable();
	void setSignalLevels();

	// DDI / transcoder / pipe (intel_ddi.c, intel_display.c)
	uint32_t funcCtlValue() const;
	uint32_t ddiBufValue() const;
	void planesOff();
	void planesOn();
	void scalersOff();
	void transcoderOff();
	void transcoderOn();
	void funcOff();
	void funcConfig();
	void funcEnable();
	void bufDisable();
	void replayPipeConfig();
	bool waitVblank(uint32_t ms);

	// link training (intel_dp_link_training.c)
	void prepareLinkRetrain();
	void setTpCtlPattern(uint8_t dpPattern);
	bool setLinkTrain(uint8_t dpPattern);
	bool updateLinkTrain();
	bool getAdjustTrain(const uint8_t st[6]);
	bool clockRecovery();
	bool channelEq();
	bool postLtAdjReq();
	void setIdleLinkTrain();
	bool startLinkTrain();
	void setPower(uint8_t mode);

	bool identify(int pipe);
	int precheck(int action);
	int retrain();
	int modeset(bool readback);
	int powerDown();
	int verify();
	void logFailure(const char *step);

	// the second pipe (runNew)
	bool setupNew(const NewOutput &n);
	void newPlaneNoarm(const NewOutput &n, bool clear);
	int enableNew(const NewOutput &n);
	int disableNew(const NewOutput &n);
};

#endif
