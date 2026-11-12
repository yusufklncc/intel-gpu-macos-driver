// SPDX-License-Identifier: MIT
// plan_test: offline check of IntelXeDisplayPlan.h (the second display pipe's arithmetic), no GPU needed. The expected
// values are worked out by hand from the Linux v6.18 functions each helper names (skl_watermark.c, intel_display.c,
// intel_dp.c, intel_pmdemand.c, intel_bw.c); the comments show the working.
// c++ -std=c++17 -Wall -Wextra -Wconversion -Wsign-conversion -I.. -o plan_test plan_test.cpp ../IntelXeEdid.cpp && ./plan_test
#include <stdio.h>
#include <string.h>
#include "IntelXeDisplayPlan.h"
#include "IntelXeEdid.h"

using namespace intelxeplan;

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)
#define EQ(a, b) CHECK((uint64_t)(a) == (uint64_t)(b), "%s = %llu (0x%llx), want %llu (0x%llx)", #a, \
	(unsigned long long)(a), (unsigned long long)(a), (unsigned long long)(b), (unsigned long long)(b))

// CEA 1920x1080@60 (VIC 16): 148.5 MHz, 2200 x 1125
static const Timing k1080 = { 148500, 1920, 2008, 2052, 2200, 1080, 1084, 1089, 1125, true, true };
// 2560x1440 at about 180 Hz with reduced blanking (the panel's kind of mode): 706250 kHz, 2720 x 1443 (made up but sane)
static const Timing k1440 = { 706250, 2560, 2608, 2640, 2720, 1440, 1443, 1448, 1443 + 0, false, true };

static void testPorts()
{
	Port p = {};
	CHECK(portInfo(4, &p), "TC2");
	EQ(p.phy, 6); EQ(p.aux, 0x16F410); EQ(p.hpd, 1u << 25); EQ(p.ddiSel, 5); EQ(p.clkSel, 0x70000000u);   // display-plan.md facts
	CHECK(!strcmp(p.name, "TC2"), "name %s", p.name);
	CHECK(portInfo(0, &p), "A");
	EQ(p.phy, 0); EQ(p.aux, 0x64010); EQ(p.hpd, 1u << 16); EQ(p.ddiSel, 1); EQ(p.clkSel, 1u << 28);
	CHECK(portInfo(6, &p), "TC4");
	EQ(p.phy, 8); EQ(p.aux, 0x16F810); EQ(p.hpd, 1u << 27); EQ(p.clkSel, 9u << 28);
	CHECK(!portInfo(1, &p) && !portInfo(2, &p) && !portInfo(7, &p) && !portInfo(-1, &p), "B, C, 7, -1 are not BMG ports");
	EQ(portFromName("tc1"), 3); EQ(portFromName("TC4"), 6); EQ(portFromName("a"), 0);
	EQ(portFromName("tc5"), -1); EQ(portFromName("b"), -1); EQ(portFromName(""), -1);
	EQ(pipePowerWell(1), 6); EQ(pwReq(6), 1u << 13); EQ(pwState(6), 1u << 12); EQ(pwFuse(6), 1u << 20);
}

static void testDbuf()
{
	EQ(mbusJoined(1), 1); EQ(mbusJoined(2), 1); EQ(mbusJoined(3), 0); EQ(mbusJoined(5), 0);
	EQ(dbufSlices(0, 1, true), 0xF); EQ(dbufSlices(0, 1, false), 0x3);
	EQ(dbufSlices(0, 3, false), 0x3); EQ(dbufSlices(1, 3, false), 0xC); EQ(dbufSlices(1, 3, true), 0);   // no joined A+B entry
	EQ(enabledSlices(1, true), 0xF); EQ(enabledSlices(3, false), 0xF); EQ(enabledSlices(1, false), 0x3);
	EQ(mbusDdbOffset(0x3), 0); EQ(mbusDdbOffset(0xC), 2048); EQ(mbusDdbOffset(0xF), 0);
	uint32_t w[4] = { 2560, 1920, 0, 0 }, off = 99;
	Ddb d = pipeDdb(0, 1, true, w, &off);
	EQ(d.start, 0); EQ(d.end, 4096); EQ(off, 0);
	d = pipeDdb(0, 3, false, w, &off);
	EQ(d.start, 0); EQ(d.end, 2048); EQ(off, 0);
	d = pipeDdb(1, 3, false, w, &off);
	EQ(d.start, 0); EQ(d.end, 2048); EQ(off, 2048);   // relative to the S3/S4 bank
	// B + C share S3/S4: weights 1920 / 3840 -> B 0..682, C 682..2048 (2048 x 1920 / 5760 = 682.67)
	uint32_t w2[4] = { 0, 1920, 3840, 0 };
	d = pipeDdb(1, 6, false, w2, &off); EQ(d.start, 0); EQ(d.end, 682); EQ(off, 2048);
	d = pipeDdb(2, 6, false, w2, &off); EQ(d.start, 682); EQ(d.end, 2048);
	d = pipeDdb(3, 3, false, w, &off); EQ(d.size(), 0);   // not active
	// pipe_mbus_dbox_ctl, display 14.01: I 2 << 5, B2B max 16 << 20, delay 1 << 17, regulate 1 << 16, B 0xA << 8, A 12 / 8
	EQ(mbusDboxCtl(true), 0x01030A4Cu); EQ(mbusDboxCtl(false), 0x01030A48u);
	EQ(mbusCtlBits(true, 0), 0xC0000000u); EQ(mbusCtlBits(false, 0), 0u); EQ(mbusCtlBits(false, -1), 7u << 26);
	EQ(dbufMinTracker(true), 3); EQ(dbufMinTracker(false), 1);
	EQ(DBUF_CTL_S(0), 0x45008); EQ(DBUF_CTL_S(1), 0x44FE8); EQ(DBUF_CTL_S(2), 0x44300); EQ(DBUF_CTL_S(3), 0x44304);
	EQ(ddbReg(Ddb{ 0, 2048 }), 0x07FF0000u); EQ(ddbReg(Ddb{ 2040, 2048 }), 0x07FF07F8u); EQ(ddbReg(Ddb{ 0, 0 }), 0);
	d = ddbFromReg(0x03E70000u); EQ(d.start, 0); EQ(d.end, 0x3E8);
}

static void testLink()
{
	Link l = {};
	// 1080p60: 148500 x 24 / 8 = 445500 kB/s; RBR x1 162000, x2 324000, x4 648000 -> RBR x4
	CHECK(linkConfig(148500, 24, 810000, 4, &l), "1080p60");
	EQ(l.rate, 162000); EQ(l.lanes, 4);
	// 3840x2160@60 CVT-RB (533250 kHz): 1599750 kB/s -> HBR2 x4 (HBR x4 = 1080000, HBR2 x2 = 1080000 too small)
	CHECK(linkConfig(533250, 24, 810000, 4, &l), "4k60");
	EQ(l.rate, 540000); EQ(l.lanes, 4);
	CHECK(!linkConfig(533250, 24, 270000, 4, &l), "4k60 on an HBR sink");
	CHECK(linkConfig(533250, 24, 810000, 2, &l), "4k60 on 2 lanes"); EQ(l.rate, 810000); EQ(l.lanes, 2);
	EQ(maxDataRate(810000, 4), 3240000); EQ(rateFromBwCode(0x1E), 810000);
	// intel_link_compute_m_n: data 445500 / 648000 = 0.6875 -> M 0x580000 / N 0x800000 after four halvings of
	// 0x8000000; link 148500 x 0x80000 / 162000 = 480597.33 -> 480597 / 0x80000
	const MN m = linkMN(24, 4, 148500, 162000);
	EQ(m.tu, 64); EQ(m.dataM, 0x580000); EQ(m.dataN, 0x800000); EQ(m.linkM, 480597); EQ(m.linkN, 0x80000);
	EQ(dataMReg(m), 0x7E580000u);
	// 706250 kHz on HBR2 x4: data 2118750 / 2160000 x 2^23 = 8228352 (0x7D8E00 after 4 halvings: 0x8000000 -> 0x800000)
	const MN m2 = linkMN(24, 4, 706250, 540000);
	EQ(m2.dataN, 0x800000); EQ(m2.dataM, (uint32_t)((uint64_t)2118750 * 0x8000000 / 2160000 >> 4));
	EQ(m2.linkM, (uint32_t)((uint64_t)706250 * 0x80000 / 540000)); EQ(m2.linkN, 0x80000);
	// hsw_linetime_wm: 2200 x 8000 / 148500 = 118.52 -> 119
	EQ(linetimeWm(2200, 148500), 119); EQ(linetimeWm(65535, 20000), 0x1FF);
	EQ(minCdclk(1305600), 652800); EQ(minCdclk(1305601), 652801);
	const TimingRegs r = timingRegs(k1080);
	EQ(r.htotal, (2200u - 1) << 16 | (1920 - 1)); EQ(r.hblank, (2200u - 1) << 16 | (1920 - 1));
	EQ(r.hsync, (2052u - 1) << 16 | (2008 - 1)); EQ(r.vtotal, (1125u - 1) << 16 | (1080 - 1));
	EQ(r.vblank, (1125u - 1) << 16); EQ(r.vsync, (1089u - 1) << 16 | (1084 - 1)); EQ(r.scl, 0);
	EQ(r.pipesrc, (1920u - 1) << 16 | (1080 - 1)); EQ(r.funcSync, 3u << 16);
	const Timing back = timingFromRegs(r.htotal, r.hsync, r.vtotal, r.vsync, r.funcSync, 148500);
	CHECK(!memcmp(&back, &k1080, sizeof back), "timing round trip");
	EQ(refreshMilliHz(k1080), 60000);
}

static void testWm()
{
	// latencies: LP0 0 (-> + 6 read latency on the valid levels), LP1 4, LP2 8, LP3 10, LP4 0 (levels 4-7 off)
	const Latency lat = latencyFromRegs(0x00040000, 0x000A0008, 0, 20);
	const uint16_t want[8] = { 6, 10, 14, 16, 0, 0, 0, 0 };
	for (int i = 0; i < 8; i++) EQ(lat.us[i], want[i]);
	EQ(lat.sagvUs, 20);
	const Latency lat2 = latencyFromRegs(0x00050002, 0x00000007, 0x00090008, 0);   // LP3 0: levels 4-5 zeroed, no + 6
	const uint16_t want2[8] = { 2, 5, 7, 0, 0, 0, 0, 0 };
	for (int i = 0; i < 8; i++) EQ(lat2.us[i], want2[i]);
	// 1920 wide, 148.5 MHz, htotal 2200: 7680 bytes/line -> 15 blocks + 1 = 16; line time 14.81 -> 15 us
	const WmParams p = wmParams(1920, 4, 148500, 2200);
	EQ(p.blocksPerLine.val, 16u << 16); EQ(p.linetimeUs, 15);
	// 6 us: method1 = 6 x 148500 x 4 / 512000 + 1 = 7.96 (6 < 15 us line time: method1); blocks 8 + 1 = 9 -> at least
	// one line (16); lines ceil(7.96 / 16) = 1; min_ddb = 16 + 2 = 18, + 1 = 19
	WmLevel w = planeWm(p, 6);
	EQ(w.enable, 1); EQ(w.blocks, 16); EQ(w.lines, 1); EQ(w.minDdb, 19);
	// 50 us: method2 = ceil(50 x 148500 / 2200000) = 4 lines x 16 = 64 (50 >= 15 us); blocks 65, lines 4, min 65 + 7 + 1
	w = planeWm(p, 50);
	EQ(w.blocks, 65); EQ(w.lines, 4); EQ(w.minDdb, 73);
	w = planeWm(p, 0);
	EQ(w.enable, 0); EQ(w.minDdb, kRejected);
	// transition: (16 - 1) + 4 + 10 + 1 = 30 blocks, min max(19, 31)
	const WmLevel t = transWm(planeWm(p, 6));
	EQ(t.blocks, 30); EQ(t.minDdb, 31); EQ(t.enable, 1);
	// a rejected SAGV WM0 makes a transition WM that never fits (u16 arithmetic as Linux)
	const WmLevel tr = transWm(planeWm(p, 0));
	EQ(tr.minDdb, kRejected);
	EQ(wmReg(WmLevel{ true, 16, 1, 19 }), 0x80004010u);
	// cursor: 256 x 4 = 1024 bytes -> 2 + 1 = 3 blocks/line; its last usable level (3, 16 us >= 15 us: method2 =
	// ceil(16 x 148500 / 2200000) = 2 lines x 3 = 6) needs 6 + 1 = 7 blocks, min 7 + 1 + 1 = 9; with one pipe at least 32
	EQ(cursorAlloc(148500, 2200, lat, 2), 9); EQ(cursorAlloc(148500, 2200, lat, 1), 32);
	PipeWm pw = {};
	CHECK(pipeWm(k1080, 1920, lat, Ddb{ 0, 2048 }, 2, &pw), "1080p pipe in 2048 blocks");
	EQ(pw.vblankMaxLevel, 3); EQ(pw.ddbLevel, 3);
	EQ(pw.cursor.start, 2039); EQ(pw.cursor.end, 2048); EQ(pw.plane.start, 0); EQ(pw.plane.end, 2039);
	EQ(pw.regBufCfg, 0x07F60000u); EQ(pw.regCurBufCfg, 0x07FF07F7u);
	EQ(pw.useSagv, 1);   // SAGV WM0 (latency 26) fits
	EQ(pw.regWm[0], wmReg(pw.sagv0)); EQ(pw.regTrans, wmReg(pw.sagvTrans));
	CHECK(pw.wm[1].enable && pw.wm[3].enable && !pw.wm[4].enable, "levels 1-3 on, 4 off");
	EQ(pw.regWm[4], wmReg(WmLevel{ false, pw.wm[3].blocks, pw.wm[3].lines, 0 }));   // the copy WA keeps 3's values, disabled
	// a 20-block DDB: 9 for the cursor, 11 left; level 0 needs 19 -> refused
	CHECK(!pipeWm(k1080, 1920, lat, Ddb{ 0, 20 }, 2, &pw) && pw.ddbLevel < 0, "20 blocks: no level fits");
	// a vblank of 2 lines: level 0 with latency 0 needs 1 + 0 + 1 line <= 2; level 1 (10 us: 1 line) does not fit
	Timing tight = k1080; tight.vtotal = 1082;
	CHECK(pipeWm(tight, 1920, lat, Ddb{ 0, 2048 }, 2, &pw), "tight vblank");
	EQ(pw.vblankMaxLevel, 0); CHECK(!pw.wm[1].enable, "level 1 off by the vblank");
	EQ(pw.useSagv, 0);   // SAGV latency 26 us does not fit the vblank either
	Timing none = k1080; none.vtotal = 1081;   // 1 line of vblank: nothing fits
	CHECK(!pipeWm(none, 1920, lat, Ddb{ 0, 2048 }, 2, &pw) && pw.vblankMaxLevel < 0, "1-line vblank refused");
}

static void testPmdemand()
{
	const uint32_t r0 = 0xFFFF0000u | 2u << 12 | 0u << 8 | 1u << 6 | 3u << 4 | 1u, r1 = 653u << 20 | 810u << 8 | 7u << 4 | 2u;
	const PmDemand p = pmdemandFromRegs(r0, r1);
	EQ(p.qclkBw, 0xFFFF); EQ(p.voltage, 2); EQ(p.pipes, 1); EQ(p.dbufs, 3); EQ(p.phys, 1);
	EQ(p.cdclkMhz, 653); EQ(p.ddiclkMhz, 810); EQ(p.scalers, 7); EQ(p.plls, 2);
	uint32_t a = r0 | 1u << 3, b = r1 | PMDEMAND_REQ;   // bits outside the fields stay, REQ is cleared
	pmdemandToRegs(p, &a, &b);
	EQ(a, r0 | 1u << 3); EQ(b, r1);
	const PmDemand n = pmdemandFor(p, 2, 0xF, 2, 540000);
	EQ(n.pipes, 2); EQ(n.dbufs, 3); EQ(n.phys, 2); EQ(n.plls, 3); EQ(n.ddiclkMhz, 540); EQ(n.scalers, 7);
	EQ(n.cdclkMhz, 653); EQ(n.voltage, 2); EQ(n.qclkBw, 0xFFFF);
	const PmDemand m = pmdemandMax(p, n);
	EQ(m.ddiclkMhz, 810); EQ(m.pipes, 2); EQ(m.phys, 2); EQ(m.plls, 3);
	// bandwidth: 6 channels, 2 QGV points, GDDR (type 8); dclk field 900 -> 15000 MHz -> 360000 MB/s, capped at 53000
	const uint32_t qgv[8] = { 900, 450 };
	EQ(deratedBwMBps(2u << 8 | 6u << 4 | 8u, qgv, 8), 53000);
	const uint32_t slow[8] = { 30, 15 };   // 500 MHz: 12000 MB/s x 70 % = 8400
	EQ(deratedBwMBps(2u << 8 | 6u << 4 | 8u, slow, 8), 8400);
	EQ(deratedBwMBps(2u << 8 | 6u << 4 | 9u, slow, 8), 6600);   // ECC: 45 %
	EQ(deratedBwMBps(0, slow, 8), 0);
}

// pipe A as the GOP left it on the B580 (2560x1440 on TC2, HBR2 x4, MBUS joined) and a 1080p sink on TC1
static LiveState liveA()
{
	LiveState s = {};
	s.pipeA = 0;
	const TimingRegs r = timingRegs(k1440);
	s.htotal = r.htotal; s.hblank = r.hblank; s.hsync = r.hsync; s.vtotal = r.vtotal; s.vblank = r.vblank; s.vsync = r.vsync;
	s.funcCtl = 1u << 31 | 5u << 27 | 2u << 24 | 3u << 1 | r.funcSync;   // TC2, SST, x4
	s.valfreq = 540000;
	const MN mn = linkMN(24, 4, k1440.clock, 540000);
	s.dataM = dataMReg(mn); s.dataN = mn.dataN; s.linkM = mn.linkM; s.linkN = mn.linkN;
	s.planeBufCfg = ddbReg(Ddb{ 0, 900 }); s.curBufCfg = ddbReg(Ddb{ 900, 932 });   // the GOP's plane + IntelXeFB's cursor
	s.mbusCtl = MBUS_JOIN | MBUS_HASHING_1x4;
	s.dbufCtl[0] = DBUF_POWER_REQUEST | DBUF_POWER_STATE | 3u << 16;
	s.pmd0 = 0xFFFFu << 16 | 1u << 6 | 1u << 4 | 1u; s.pmd1 = 653u << 20 | 540u << 8 | 2u;
	s.lat01 = 0x00040000; s.lat23 = 0x000A0008; s.latSagv = 20;
	s.memGlobal = 2u << 8 | 6u << 4 | 8u; s.qgvLow[0] = 900; s.qgvLow[1] = 450;
	return s;
}

static void testPlan()
{
	LiveState s = liveA();
	Request q = { 1, 3, k1080, 810000, 4 };
	Plan pl = {};
	EQ(computePlan(s, q, &pl), kOk);
	CHECK(!strcmp(pl.portA.name, "TC2") && !strcmp(pl.portB.name, "TC1"), "ports %s %s", pl.portA.name, pl.portB.name);
	// pipe A recomputed from its own registers gives its registers back
	EQ(pl.a.t.clock, k1440.clock); EQ(dataMReg(pl.a.mn), s.dataM); EQ(pl.a.mn.linkM, s.linkM); EQ(pl.a.link.lanes, 4);
	EQ(pl.a.ddb.end, 4096); EQ(pl.a.dbox, mbusDboxCtl(true));
	EQ(pl.joinedNow, 1); EQ(pl.joinedAfter, 0);
	EQ(pl.a2.ddb.end, 2048); EQ(pl.aKeepsDdb, 1);
	EQ(pl.b.link.rate, 162000); EQ(pl.b.link.lanes, 4); EQ(pl.b.ddb.start, 0); EQ(pl.b.ddb.end, 2048); EQ(pl.b.mbusOffset, 2048);
	EQ(pl.b.slices, 0xC); EQ(pl.slicesNow, 0x1); EQ(pl.slicesAfter, 0xF);
	EQ(pl.mbusCtlAfter, 0u); EQ(pl.trackerNow, 3); EQ(pl.trackerAfter, 1); EQ(pl.b.dbox, 0x01030A48u);
	EQ(pl.pwIdx, 6);
	EQ(pl.pmdPost.pipes, 2); EQ(pl.pmdPost.phys, 2); EQ(pl.pmdPost.plls, 3); EQ(pl.pmdPost.dbufs, 3); EQ(pl.pmdPost.ddiclkMhz, 540);
	EQ(pl.pmdPre.ddiclkMhz, 540); EQ(pl.pmd0Post >> 16, 0xFFFF); EQ((pl.pmd0Post >> 6) & 3, 2); EQ(pl.pmd1Post & 7, 3);
	EQ(pl.bwMBps, 53000); EQ(pl.needMBps, (706250u * 4 + 148500u * 4 + 999) / 1000);
	CHECK(pl.b.wmOk && pl.b.wm.useSagv, "pipe B watermarks");
	// refusals
	q.port = 4; EQ(computePlan(s, q, &pl), kPortInUse);
	q.port = 1; EQ(computePlan(s, q, &pl), kBadPort);
	q.port = 3; s.transconfB = 1u << 31; EQ(computePlan(s, q, &pl), kPipeInUse);
	s = liveA(); s.planeBufCfg = ddbReg(Ddb{ 0, 3000 }); EQ(computePlan(s, q, &pl), kDdbA);
	s = liveA(); q.sinkMaxRate = 162000; q.sinkMaxLanes = 2; EQ(computePlan(s, q, &pl), kNoLink);
	q = { 1, 3, k1080, 810000, 4 }; q.mode.clock = 1400000; q.mode.htotal = 9000; q.mode.vtotal = 2000;
	q.sinkMaxRate = 810000; EQ(computePlan(s, q, &pl), kNoLink);   // 4.2 GB/s: more than HBR3 x4
	s = liveA(); s.funcCtl = 0; EQ(computePlan(s, Request{ 1, 3, k1080, 810000, 4 }, &pl), kPipeA);
	s = liveA(); s.qgvLow[0] = 30; s.qgvLow[1] = 15; s.memGlobal = 2u << 8 | 1u << 4 | 8u;   // 2000 x 0.7 = 1400 MB/s
	EQ(computePlan(s, Request{ 1, 3, k1080, 810000, 4 }, &pl), kBandwidth);
}

// the dry run's "WxH@Hz" modes are drm_cvt_mode timings (intelxeedid::cvt): the VESA CVT 1.2 rasters; the clock is
// Linux's, which never takes off the 0.25 MHz step (it subtracts clock % 250 before setting the clock), so 138.611 MHz
// instead of the reference 138.5 for 1080p60 RB
static void testCvt()
{
	intelxeedid::Mode m = {};
	CHECK(intelxeedid::cvt(1920, 1080, 60, true, &m), "1080p60 RB");
	EQ(m.clock, 138611); EQ(m.htotal, 2080); EQ(m.hsync_start, 1968); EQ(m.hsync_end, 2000); EQ(m.vtotal, 1111);
	CHECK(intelxeedid::cvt(1920, 1080, 60, false, &m), "1080p60");
	EQ(m.clock, 173106); EQ(m.htotal, 2576); EQ(m.vtotal, 1120);
	CHECK(intelxeedid::cvt(3840, 2160, 60, true, &m), "4k60 RB");
	EQ(m.clock, 533120); EQ(m.htotal, 4000); EQ(m.vtotal, 2222);
}

int main()
{
	testCvt();
	testPorts();
	testDbuf();
	testLink();
	testWm();
	testPmdemand();
	testPlan();
	printf("plan_test: %d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
