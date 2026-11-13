// SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
// IntelXeFB.cpp is derived from nullmoth's NMIntelFB (kexts/NMIntelFB/intelfb.cpp) and NVRMFB and is therefore licensed
// under the PolyForm Noncommercial License 1.0.0 (kernel/IntelXeFB/LICENSE). IntelXeLink.cpp/.h in this directory are MIT.
// Required Notice: Copyright (c) 2026 NullMoth Systems (https://nullmothsystems.com)
// Modifications Copyright (c) 2026 Yusuf İhsan Kılınç
// IntelXeFB: IOFramebuffer for the Arc B580's outputs (phase 4a).
// The class name carries "Intel": CoreDisplay tells GPU vendors apart by framebuffer class name only (GetGPUVendorFor-
// FramebufferService looks for "Intel", "AMD", "AppleParavirt", "NVDA"), and uses display-pipe transactions
// (IOPresentment) only when every GPU's vendor is a known one. As "IntelXeFB" the vendor was unknown and WindowServer
// fell back to the legacy page-flip path. It takes over the mode the GOP already lit: the first
// pipe whose transcoder and plane 1 are on, linear XRGB8888, published as one fixed mode. The surface is found through
// the GGTT (PTEs at BAR0 + 8 MB) and handed to macOS as BAR2 + its VRAM address, so it must be contiguous VRAM inside
// the visible BAR2 window. Nothing is written to the display engine: if anything does not fit, start() fails, the
// reason is logged and the screen stays as the firmware left it.
//
// Based on nullmoth's NMIntelFB (kexts/NMIntelFB/intelfb.cpp): same plane/transcoder registers (display 14.01 keeps the
// display 12+ layout, Linux i915/display), same 60 Hz-style VBL timer and power answers. Lives in the AuxKC because it
// links IOGraphicsFamily. SSDT-ARCOFF stays: it keeps IONDRVFramebuffer off the card; this matches by PCI ID.

#include <IOKit/IOLib.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOCommandGate.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <IOKit/graphics/IOFramebuffer.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include <sys/sysctl.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include "IntelXeLink.h"
#include "IntelXeEdid.h"
#include "IntelXeDisplayPlan.h"
#include "../IntelXe/uapi/intelxe_bootargs.h"   // the boot-arg names before the rename, for one release
#include "../IntelXe/uapi/intelxe_bootguard.h"  // the boot guard's rules (shared with IntelXe.kext and the host test)

#define FBLOG(fmt, ...) IOLog("IntelXeFB: " fmt "\n", ##__VA_ARGS__)

// Matches at boot from the AuxKC like any GPU driver (2026-10-29; before, a LaunchDaemon loaded it once userspace was
// up, behind IOPropertyMatch intelxe-fb-armed). Off: boot-arg -intelxe_nofb (or -intelxefb_off), safe mode (-x). Loading it by
// hand (kmutil load) still matches it: kmutil sends the personality and the kernel re-runs matching on the GPU.
extern "C" kern_return_t IntelXeFB_start(kmod_info_t *, void *) { return KERN_SUCCESS; }
// NVRAM (/options) for the boot guard: values written here read back as OSData after a reboot, nvram(8) writes OSData
static bool nvramRead(const char *key, char *out, size_t len)
{
	if (!len) return false;
	out[0] = 0;
	IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!opt) return false;
	bool ok = false;
	if (OSObject *o = opt->copyProperty(key)) {
		if (OSData *d = OSDynamicCast(OSData, o)) {
			size_t n = d->getLength() < len - 1 ? d->getLength() : len - 1;
			bcopy(d->getBytesNoCopy(), out, n); out[n] = 0; ok = true;
		} else if (OSString *str = OSDynamicCast(OSString, o)) { strlcpy(out, str->getCStringNoCopy(), len); ok = true; }
		o->release();
	}
	opt->release();
	return ok;
}
static bool nvramRead(const char *key, char *out, size_t len);
// val nullptr: clear (written as "ok", see below). The IONVRAM-DELETE-PROPERTY request did not delete (boot1, 03:40: "intelxe-guard cleared" logged,
// `nvram intelxe-guard` still fb1), so a delete is removeProperty() checked by a read-back, and "ok" written if the key
// survives it; every reader treats "ok" as absent.
static void nvramWrite(const char *key, const char *val)
{
	IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!opt) return;
	if (val) {
		if (OSString *v = OSString::withCString(val)) { opt->setProperty(key, v); v->release(); }
	} else if (OSString *v = OSString::withCString("ok")) {
		// "ok", never a delete: removeProperty() reads back as gone but does not reach flash (5 Nov: a guard cleared at
		// 20:21:47 was read as accel1 after a power cut at 20:23). Every reader treats "ok" as absent.
		opt->setProperty(key, v);
		v->release();
	}
	// No sync request: x86's AppleEFINVRAM ignores the SYNCNOW keys (it stored them as variables), and calling
	// IODTNVRAM::sync() here hung the boot. A kernel write reaches flash at a clean shutdown; the boot guard's "cleared"
	// is also written from userspace by install.sh's LaunchDaemon, which survives a power cut.
	opt->release();
}
static bool bootArg(const char *name) { char d[8]; return intelxeBootArg("IntelXeFB", name, d, sizeof d); }
extern "C" kern_return_t IntelXeFB_stop(kmod_info_t *, void *) { return KERN_SUCCESS; }

namespace {
constexpr uint32_t PLANE_CTL(int p)    { return 0x70180u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_STRIDE(int p) { return 0x70188u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SIZE(int p)   { return 0x70190u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_SURF(int p)   { return 0x7019Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_POS(int p)    { return 0x7018Cu + (uint32_t)p * 0x1000u; }
// the surface address the plane is scanning out now (_PLANE_SURFLIVE_1_A); PLANE_SURF is the one armed for the next
// start of vblank (skl_universal_plane.c / intel_display_regs.h)
constexpr uint32_t PLANE_SURFLIVE(int p) { return 0x701ACu + (uint32_t)p * 0x1000u; }
// The rest of plane 1's non-arming registers that icl_plane_update_noarm writes for every update (skl_universal_plane_
// regs.h): colour key, offset, chroma upsampler (HDR planes) and PLANE_COLOR_CTL. PLANE_AUX_DIST is not written on BMG
// (HAS_AUX_DIST: display 9-12, ADL-P, MTL only; flat CCS needs none). Double buffered: the PLANE_SURF write arms them.
constexpr uint32_t PLANE_KEYVAL(int p)    { return 0x70194u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_KEYMSK(int p)    { return 0x70198u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_KEYMAX(int p)    { return 0x701A0u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_OFFSET(int p)    { return 0x701A4u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_CUS_CTL(int p)   { return 0x701C8u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_COLOR_CTL(int p) { return 0x701CCu + (uint32_t)p * 0x1000u; }
// glk_plane_color_ctl for an opaque RGB framebuffer without a plane colour pipeline: PLANE_COLOR_PLANE_GAMMA_DISABLE
// (bit 13), PLANE_COLOR_ALPHA_DISABLE (0), no input/plane CSC, no pre-CSC gamma. The GOP leaves 0x2000 (read on the
// B580, 2026-10-30); the register's reset value 0 turns the plane's post-CSC gamma on with whatever its LUT holds.
constexpr uint32_t PLANE_COLOR_PLANE_GAMMA_DISABLE = 1u << 13;
constexpr uint32_t kPlaneColorCtl = PLANE_COLOR_PLANE_GAMMA_DISABLE;
// Pipe colour registers icl_color_commit_arm / icl_load_csc_matrix write (intel_color_regs.h): SKL_BOTTOM_COLOR,
// PIPE_CSC_MODE and the pipe CSC (coefficients, pre- and post-offsets), pipes 0x100 apart (_PIPE_B_CSC_MODE 0x49128).
constexpr uint32_t SKL_BOTTOM_COLOR(int p) { return 0x70034u + (uint32_t)p * 0x1000u; }
constexpr uint32_t PIPE_CSC_MODE(int p)    { return 0x49028u + (uint32_t)p * 0x100u; }
constexpr uint32_t kCscRegs[] = { 0x49010, 0x49014, 0x49018, 0x4901C, 0x49020, 0x49024,   // COEFF RY_GY .. BV
                                  0x49030, 0x49034, 0x49038, 0x49040, 0x49044, 0x49048 }; // PREOFF HI/ME/LO, POSTOFF HI/ME/LO
constexpr int kNumCscRegs = (int)(sizeof kCscRegs / sizeof kCscRegs[0]);
// Pipe scaler 0 (intel_display_regs.h _PS_1A_*; pipe B +0x800) and its PS_CTRL bits
constexpr uint32_t PS_WIN_POS(int p)   { return 0x68170u + (uint32_t)p * 0x800u; }
constexpr uint32_t PS_WIN_SZ(int p)    { return 0x68174u + (uint32_t)p * 0x800u; }
constexpr uint32_t PS_CTRL(int p)      { return 0x68180u + (uint32_t)p * 0x800u; }
constexpr uint32_t PS_VPHASE(int p)    { return 0x68188u + (uint32_t)p * 0x800u; }
constexpr uint32_t PS_HPHASE(int p)    { return 0x68194u + (uint32_t)p * 0x800u; }
constexpr uint32_t PS_SCALER_EN = 1u << 31, PS_BINDING_PLANE1 = 1u << 25;
constexpr uint32_t SDEISR = 0xC4000;   // south display live status (hotplug)
// Display interrupts (Linux intel_display_regs.h; gen11_display_irq_handler, gen8_de_irq_handler, icp/mtp hotplug).
// IntelXe.kext owns the MSI and calls IntelXeFB's handler when GFX_MSTR_IRQ has DISPLAY_IRQ (bit 16); everything below is
// IntelXeFB's. BMG's south display is PCH_MTL-style: TC hotplug in SDE bits 27:24, status in SHOTPLUG_CTL_TC.
constexpr uint32_t GEN11_DISPLAY_INT_CTL = 0x44200, DISPLAY_IRQ_ENABLE = 1u << 31;
constexpr uint32_t DE_PCH_IRQ = 1u << 23, DE_MISC_IRQ = 1u << 22, DE_HPD_IRQ = 1u << 21, DE_PORT_IRQ = 1u << 20;
constexpr uint32_t DE_PIPE_IMR(int p) { return 0x44404u + (uint32_t)p * 0x10u; }
constexpr uint32_t DE_PIPE_IIR(int p) { return 0x44408u + (uint32_t)p * 0x10u; }
constexpr uint32_t DE_PIPE_IER(int p) { return 0x4440Cu + (uint32_t)p * 0x10u; }
constexpr uint32_t PIPE_VBLANK = 1u << 0, PIPE_FIFO_UNDERRUN = 1u << 31;
constexpr uint32_t DE_MISC_IIR = 0x44468, DE_PORT_IIR = 0x44448, DE_HPD_IIR = 0x44478;
constexpr uint32_t SDEIMR = 0xC4004, SDEIIR = 0xC4008, SDEIER = 0xC400C, SHOTPLUG_CTL_DDI = 0xC4030, SHOTPLUG_CTL_TC = 0xC4034;
constexpr uint32_t SDE_TC_HOTPLUG_MASK = 0xFu << 24;
// Hotplug (v10f; Linux 7.2 intel_hotplug_irq.c icp_irq_handler/mtp_hpd_irq_setup, intel_hotplug.c, intel_dp.c): MTP pins
// (hpd_mtp: what BMG uses, PCH_MTL) are DDI A/B at SDE bits 17:16 and TC1-4 at 27:24. SHOTPLUG_CTL_DDI/_TC hold, per pin
// nibble, enable 8, long-pulse 2 and short-pulse 1 status (sticky; written back to clear). SHPD_FILTER_CNT is the
// hardware HPD debounce: 250 us for DP 1.4a Table 3-4 (a short pulse is 0.5-1 ms).
constexpr uint32_t SDE_DDI_HOTPLUG_MASK = 3u << 16, kHpdPins = SDE_TC_HOTPLUG_MASK | SDE_DDI_HOTPLUG_MASK;
constexpr uint32_t SHPD_FILTER_CNT = 0xC4038, SHPD_FILTER_CNT_250 = 0xF8;
constexpr uint32_t SOUTH_CHICKEN1 = 0xC2000;   // HPD inversion bits (mtp_hpd_invert); kept as the firmware set them
constexpr uint32_t HPD_LONG = 2;   // the long-pulse status of a pin nibble (short = 1, cleared with it)
// intel_hotplug.c: long pulses count 10, short ones 1 (short storms only without DP MST: not counted here, BMG has MST);
// over 50 within 1 s the pin is masked and polled, and unmasked again after 2 minutes.
constexpr int kHpdStormThreshold = 50, kHpdStormPeriodMs = 1000, kHpdStormReenableMs = 2 * 60 * 1000;
constexpr uint32_t TRANSCONF(int p)    { return 0x70008u + (uint32_t)p * 0x1000u; }
constexpr uint32_t HTOTAL(int t)       { return 0x60000u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VTOTAL(int t)       { return 0x6000Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t HSYNC(int t)        { return 0x60008u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VSYNC(int t)        { return 0x60014u + (uint32_t)t * 0x1000u; }
constexpr uint32_t VBLANK(int t)       { return 0x60010u + (uint32_t)t * 0x1000u; }
constexpr uint32_t FRMCOUNT(int p)     { return 0x70040u + (uint32_t)p * 0x1000u; }
// start-of-vblank timestamp and the free-running counter it is taken from, both in microseconds (i915
// __intel_get_crtc_scanline_from_timestamp)
constexpr uint32_t FRMTMSTMP(int p)    { return 0x70048u + (uint32_t)p * 0x1000u; }
constexpr uint32_t TIMESTAMP_CTR = 0x44070;
constexpr uint32_t TRANS_DDI_FUNC_CTL(int t) { return 0x60400u + (uint32_t)t * 0x1000u; }   // port select 30:27 = port + 1
constexpr uint32_t PIPEDSL(int p)      { return 0x70000u + (uint32_t)p * 0x1000u; }   // scanline 19:0
// Seamless pixel-clock change (Linux "update_m_n", intel_cpu_transcoder_set_m1_n1): the DP M/N values are double
// buffered on the transcoder's vblank on display 9+ (HAS_DOUBLE_BUFFERED_M_N), the link stays trained.
constexpr uint32_t TRANS_DATA_M1(int t) { return 0x60030u + (uint32_t)t * 0x1000u; }   // TU 30:25, data M 23:0
constexpr uint32_t TRANS_DATA_N1(int t) { return 0x60034u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_LINK_M1(int t) { return 0x60040u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_LINK_N1(int t) { return 0x60044u + (uint32_t)t * 0x1000u; }
constexpr uint32_t DDI_CLK_VALFREQ(int port) { return 0x64030u + (uint32_t)port * 0x100u; }   // port clock, kHz
constexpr uint32_t WM_LINETIME(int p)   { return 0x45270u + (uint32_t)p * 4u; }   // line time 8:0, 1/8 us (hsw_linetime_wm)
// DP Adaptive-Sync (VRR) on display 14 (Linux 7.2 intel_vrr_regs.h, intel_vrr.c; transcoders A-D 0x1000 apart). The VRR
// timing generator ends a frame at the first "decision line" in [vmin, vmax] - guardband after a push (TRANS_PUSH SEND),
// at vmax - guardband without one; the guardband lines (the vblank) follow and double-buffered registers latch at the
// decision. Display 13+: no flipline offset, VRR_CTL carries the guardband (bits 15:0), display 14 drops IGN_MAX_SHIFT.
constexpr uint32_t TRANS_VRR_CTL(int t)       { return 0x60420u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_VMAX(int t)      { return 0x60424u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_STATUS(int t)    { return 0x6042Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_VMIN(int t)      { return 0x60434u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_FLIPLINE(int t)  { return 0x60438u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_VTOTAL_PREV(int t) { return 0x60480u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_PUSH(int t)          { return 0x60A70u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_VRR_VSYNC(int t)     { return 0x60078u + (uint32_t)t * 0x1000u; }
constexpr uint32_t EMP_AS_SDP_TL(int t)       { return 0x60204u + (uint32_t)t * 0x1000u; }
constexpr uint32_t TRANS_SET_CONTEXT_LATENCY(int t) { return 0x6007Cu + (uint32_t)t * 0x1000u; }
constexpr uint32_t VRR_CTL_VRR_ENABLE = 1u << 31, VRR_CTL_FLIP_LINE_EN = 1u << 29, VRR_CTL_GUARDBAND_MASK = 0xFFFFu;
constexpr uint32_t TRANS_PUSH_EN = 1u << 31, TRANS_PUSH_SEND = 1u << 30;
constexpr uint32_t VRR_STATUS_VRR_EN_LIVE = 1u << 27;
constexpr uint32_t DP_DOWNSPREAD_CTRL = 0x107, DP_MSA_TIMING_PAR_IGNORE_EN = 0x80;
// Hardware cursor (Linux intel_cursor_regs.h / intel_cursor.c i9xx_cursor_update_arm, skl_write_cursor_wm; pipes
// 0x1000 apart). CURBASE arms CURCNTR/CURPOS and the cursor's DDB and watermarks at the next start of vblank.
constexpr uint32_t CURCNTR(int p)      { return 0x70080u + (uint32_t)p * 0x1000u; }
constexpr uint32_t CURBASE(int p)      { return 0x70084u + (uint32_t)p * 0x1000u; }
constexpr uint32_t CURPOS(int p)       { return 0x70088u + (uint32_t)p * 0x1000u; }
constexpr uint32_t CUR_FBC_CTL(int p)  { return 0x700A0u + (uint32_t)p * 0x1000u; }
constexpr uint32_t CUR_WM(int p, int l) { return 0x70140u + (uint32_t)p * 0x1000u + (uint32_t)l * 4u; }
constexpr uint32_t CUR_BUF_CFG(int p)  { return 0x7017Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t PLANE_BUF_CFG1(int p) { return 0x7027Cu + (uint32_t)p * 0x1000u; }
constexpr uint32_t MCURSOR_MODE_256_ARGB_AX = 0x20 | 0x03;
constexpr uint32_t CURSOR_POS_Y_SIGN = 1u << 31, CURSOR_POS_X_SIGN = 1u << 15;
constexpr uint32_t CUR_WM_EN = 1u << 31;
// The cursor image: 256x256 ARGB (premultiplied) in VRAM, mapped at a fixed GGTT window between the scan-out slots
// (end 0xF8000000) and the firmware surface (0xFC000000). DPA 31 MB: below 64 MB IntelXe.kext's VRAM allocator does not
// hand out memory (xe_core.cpp vramUserStart), its page-table pool starts at 32 MB, the GOP surface ends near 15 MB.
// Until IntelXe.kext offers an allocator for display buffers, IntelXe.kext must keep DPA 0x1F00000-0x1F3FFFF free.
constexpr uint32_t kCurDim = 256, kCurBytes = kCurDim * kCurDim * 4, kCurGgtt = 0xFBF00000u;
constexpr uint64_t kCurDpa = 0x1F00000ull;
// v10h: the image lives in a display buffer of IntelXe.kext ("intelxeDispVramAlloc", zeroed by its copy engine) whenever
// IntelXe.kext can give one, as a Linux cursor framebuffer is a GEM BO: cleared when made (xe_migrate_clear), saved and
// restored by the GPU over system sleep (xe_bo_evict_all / restore; IntelXe.kext's gDispBos), which also leaves the lines'
// flat-CCS state valid. The fixed DPA above is the fallback (IntelXe.kext without the allocator, or not up yet at start:
// tried again with the next cursor images). After a wake the fixed range held whatever D3cold left -- including its
// flat-CCS metadata, which neither the GOP (it runs at a cold boot only) nor a CPU write through BAR2 puts right -- and
// the hardware cursor showed a pixelated, distorted image however often it was rewritten (sleep test 30 Oct 14:49).
constexpr uint32_t kCurDdbBlocks = 32;   // skl_cursor_allocation: max(32 with one pipe, the computed minimum)
// debug.intelxefb_vrrlog: what the display did, frame by frame, with the flips and VBL callbacks around it (us = the
// display's own microsecond counter, TIMESTAMP_CTR / FRMTMSTMP)
struct VrrEv { uint32_t us; uint16_t type, x; uint32_t a, b; };
enum { kEvFrame = 1, kEvFlip = 2, kEvFlipDone = 3, kEvVbl = 4, kEvLfc = 5, kEvPush = 6, kEvState = 7 };
constexpr uint32_t kVrrLogN = 8192;   // power of two
VrrEv gVrrLog[kVrrLogN];
volatile UInt32 gVrrLogIdx = 0;
int gVrrLogOn = 0;
// VRR at framebuffer start unless intelxefb_vrr=0 says otherwise (-intelxefb_novrr forbids it; debug.intelxefb_vrr=-1 at runtime)
// VRR at start: -1 follow the display mode (v10l). Until v10k this was 1, i.e. debug.intelxefb_vrr forced on at start, so a
// fixed-refresh mode ("180 Hz" in Displays) still ran Adaptive-Sync with LFC and the pacer, and the panel's brightness
// shifted with the varying refresh on an idle desktop. Linux enables VRR only when the CRTC state asks for it
// (intel_vrr_compute_config: the connector's vrr_capable and the CRTC's VRR_ENABLED property, i.e. the compositor's
// request); here the request is the mode: CoreDisplay's "Variable" timings carry a vertical blanking extension.
// Boot-arg intelxefb_vrr=1 / =0 still forces it on / off.
constexpr int gVrrDefault = -1;
uint32_t gVrrVbl = 1;      // debug.intelxefb_vrr_vbl : 1 = a steady tick at the panel's top rate (WindowServer paces by it); 0 and 1 measured alike, 2 worse
uint32_t gVrrTickUs = 0;   // debug.intelxefb_vrr_tick_us: tick of VBL modes 1/2, 0 = the panel's minimum frame
int gVrrLfc = 1;           // debug.intelxefb_vrr_lfc
uint32_t gVrrPaceLo = 5;   // debug.intelxefb_vrr_pace_lo_pct: frames no shorter than the content cadence - this % (0 = pacing off)
uint32_t gVrrPaceHi = 30;  // debug.intelxefb_vrr_pace_hi_pct: and no longer than the cadence + this %
uint32_t gVrrFloorHz = 0;  // debug.intelxefb_vrr_floor_hz: lowest refresh VRR may drop to (0 = the panel's); applied at enable
int gHidpi = 0;            // HiDPI modes (opt-in): boot-arg intelxefb_hidpi=1 or NVRAM intelxefb-hidpi=1
int gVrrModes = 1;         // macOS Adaptive-Sync modes: IOFBTimingRange + programmable timings (see vrrModesInit); on by
                           // default since 0.1.3, off with intelxefb_vrrmodes=0 or NVRAM intelxefb-vrr-modes=0
int gHwCursor = 1;         // debug.intelxefb_hwcursor: 0 = decline the hardware cursor (WindowServer draws it)
int gDpms = 0;             // debug.intelxefb_dpms: display sleep powers the output down (see blank)
int gVblPrecise = 1;       // debug.intelxefb_vbl_precise: wake in two legs (see early())
int gPmGuard = 1;          // debug.intelxefb_pm_guard: after a resume, restore pagingState if IOGraphics' wake left it off (pmGuard)
// Pipe post-CSC gamma (stage 4: CoreDisplay's EOTF / system gamma in the display pipe instead of its GPU pass). Linux
// intel_color_regs.h; pipes A/B/C 0x800 apart. Display 13+ (xe2_hpd_display: gamma_lut_size 1024) uses the 10-bit
// mode, 1024 entries of 10:10:10 (icl_gamma_mode, bdw_load_lut_10, then ivb_load_lut_ext_max + glk_load_lut_ext2_max
// clamp inputs above 1.0). PAL_PREC data is not double buffered; GAMMA_MODE latches at the next start of vblank.
constexpr uint32_t GAMMA_MODE(int p)      { return 0x4A480u + (uint32_t)p * 0x800u; }
constexpr uint32_t PREC_PAL_INDEX(int p)  { return 0x4A400u + (uint32_t)p * 0x800u; }
constexpr uint32_t PREC_PAL_DATA(int p)   { return 0x4A404u + (uint32_t)p * 0x800u; }
constexpr uint32_t PREC_PAL_EXT_GC_MAX(int p, int i)  { return 0x4A420u + (uint32_t)p * 0x800u + (uint32_t)i * 4u; }
constexpr uint32_t PREC_PAL_EXT2_GC_MAX(int p, int i) { return 0x4A430u + (uint32_t)p * 0x800u + (uint32_t)i * 4u; }
constexpr uint32_t POST_CSC_GAMMA_ENABLE = 1u << 30, GAMMA_MODE_MODE_MASK = 3u, GAMMA_MODE_MODE_10BIT = 1u;
constexpr uint32_t PAL_PREC_AUTO_INCREMENT = 1u << 15;
constexpr uint32_t kGammaSize = 1024;
// NMXAccel -> IntelXeFB "nmxPipeGamma" (callPlatformFunction p1): count kGammaSize loads lut[] (10:10:10, red 29:20) into
// the pipe's post-CSC gamma and enables it; count 0 puts back the GAMMA_MODE the firmware left.
struct NMXPipeGamma {
	unsigned int version;   // 1
	unsigned int count;     // kGammaSize or 0
	unsigned int lut[1024];
};
// DP AUX (Linux intel_dp_aux_regs.h, display 14 = no xe2lpd remap): A 0x64010, B 0x64110, USBC1-4 0x16f210 + n*0x200;
// five data registers follow each control register
constexpr uint32_t kAuxCtl[6] = { 0x64010, 0x64110, 0x16f210, 0x16f410, 0x16f610, 0x16f810 };
constexpr const char *kAuxName[6] = { "A", "B", "USBC1", "USBC2", "USBC3", "USBC4" };
constexpr uint32_t AUX_SEND_BUSY = 1u << 31, AUX_DONE = 1u << 30, AUX_TIME_OUT_ERROR = 1u << 28, AUX_TIME_OUT_MAX = 3u << 26;
constexpr uint32_t AUX_RECEIVE_ERROR = 1u << 25, AUX_POWER_REQUEST = 1u << 19;
// SKL+ sync pulses as intel_dp_aux: fast-wake 18 (10 precharge + 8 preamble), normal 32 (16 + 16), each field = count - 1
constexpr uint32_t AUX_SYNC = (17u << 5) | 31u;
constexpr uint8_t DP_AUX_I2C_WRITE = 0x0, DP_AUX_I2C_READ = 0x1, DP_AUX_I2C_MOT = 0x4;
constexpr uint32_t kEdidMax = 512;
// nullmoth's scan-out query (kexts/NMIntelFB/nvrm_vram_abi.h, version 2): its accelerator's display pipe asks a head for
// a CPU mapping of the scan-out surface and copies each composited frame into it (NVDisplayPipe::performTransaction)
struct NVRMVramRequest {
	unsigned int version, flags;
	unsigned long long size, actualSize, phys;
	void *kva, *handle;
	unsigned long long mappedTotal;
	unsigned int width, height, pitch;
	unsigned long long allocationCookie;
};
// NMXAccel -> IntelXeFB "nmxScanout" (callPlatformFunction p1), frame path stage 3: the plane flips to WindowServer's frame
// in a VRAM buffer (pinned by IntelXe.kext, "intelxeScanoutPin") instead of NMXAccel copying it into the firmware's surface.
//   op 0 map    dpa/size/width/height/pitch in, ggtt/gen out: map the buffer at a free scan-out slot of the GGTT
//   op 1 unmap  ggtt/gen in: unmap it; busy while the plane shows it or is about to, unless force (it goes home first)
//   op 2 flip   ggtt/gen in: arm PLANE_SURF with it. done(ctx, cookie, 0) runs from the vblank-locked VBL callback once
//               PLANE_SURFLIVE shows it; -1 when it has not in 100 ms, -2 when a mode change or unmap sent the plane home
//               first, -3 when the framebuffer stops. immediate = 1: nothing to wait for (display asleep), done not called
//   op 3 home   the same with the firmware's surface (NMXAccel copies a frame there first); immediate = 1 when it is live
//   op 4 query  armed/live: PLANE_SURF and PLANE_SURFLIVE (addresses)
// Never called back under a lock: done may present the next frame, which takes the display lock (nmxPipeGamma).
struct NMXScanout {
	unsigned int version;   // 1
	unsigned int op;
	unsigned int ggtt, gen;
	unsigned int width, height, pitch, force;
	unsigned long long dpa, size;
	void (*done)(void *ctx, unsigned long long cookie, int status);
	void *ctx;
	unsigned long long cookie;
	unsigned int immediate, armed, live, pad;
	// version 2: op 2 carries CoreDisplay's adaptive-sync frame-length window (NMXAccel nmxNoteVrr), in lines past
	// the minimum frame; flags bit 0 = valid. Version 1 callers stop before these fields.
	unsigned int vrrFlags, vrrMinExt, vrrMaxExt, pad2;
	// version 3: op 0 (map) may name the buffer's VRAM as DPA runs (IntelXe.kext "intelxeScanoutPin" v2: 4 KB aligned, in
	// address order; one GGTT PTE per 4 KB page of each run) instead of one contiguous dpa
	const struct NMXDpaRun *runs;
	unsigned int nRuns, pad3;
};
struct NMXDpaRun { unsigned long long dpa, len; };
// Scan-out slots: 64 MB GGTT windows (a 4K frame is 33 MB) below the firmware's surface, which sits at 0xfc000000 on
// this card. xe's GGTT allocator owns all of it on Linux; here IntelXe.kext uses only the first 0xd000000 (IntelXe.h), and
// these stay below GUC_GGTT_TOP (0xfee00000) like everything else.
constexpr uint32_t kSlotGgtt = 0xE0000000u, kSlotSpan = 0x4000000u;
// HiDPI (0.1.3): a mode larger than the panel (CoreDisplay's scaled timings, horizontalScaled above the active width,
// which the plane scaler shrinks to the panel) has its home surface in a display buffer from IntelXe.kext, mapped at
// kHomeGgtt between the scan-out slots and the cursor; the firmware's surface holds the panel's size only. The largest
// is 4096 wide: the scaler's source width limit on display 14 (skl_scaler_max_src_size), 4096x2304 at 16:9.
constexpr uint32_t kHomeGgtt = 0xF8000000u, kScalerMaxW = 4096, kScalerMaxH = 2304, kHomeMax = kScalerMaxW * kScalerMaxH * 4;
constexpr int kSlots = 6;
constexpr uint32_t kGsm = 0x800000;    // GGTT PTEs: BAR0 + 8 MB, 8 bytes per 4 KB page (xe_ggtt_init_early)
constexpr uint64_t kPteAddr = 0x000FFFFFFFFFF000ull;
constexpr uint32_t kEnable = 1u << 31;
constexpr uint32_t fmtOf(uint32_t ctl)  { return (ctl >> 23) & 0x1F; }   // 8 = XRGB8888
constexpr uint32_t tileOf(uint32_t ctl) { return (ctl >> 10) & 0x7; }   // 0 = linear
// IONDRVFramebuffer::initForPM's three states: 0 sleep (only while the GPU's power domain is off: system sleep), 1 doze
// (display asleep; the framebuffer's own floor, changePowerStateTo(1)), 2 on. Leaving 0 is what sends DidWake.
IOPMPowerState kPowerStates[3] = {
	{ kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ kIOPMPowerStateVersion1, 0, 0, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
	{ kIOPMPowerStateVersion1, kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0 },
};
}

class IntelXeFramebufferPipe;   // the second display pipe's framebuffer (below)

class IntelXeFramebuffer : public IOFramebuffer {
	OSDeclareDefaultStructors(IntelXeFramebuffer)
	IOPCIDevice *fPCI = nullptr;
	IOMemoryMap *fMMIO = nullptr;
	IOMemoryMap *fSurfMap = nullptr;
	// the HiDPI home surface (kHomeGgtt): in use while a mode larger than the panel is set
	bool fHomeBig = false;
	int fHomeOwned = -1;              // kHomeGgtt reserved in IntelXe.kext's GGTT allocator (ggttReserve)
	void *fHomeBo = nullptr;          // its display buffer's cookie ("intelxeDispVramFree")
	uint64_t fHomeDpa = 0;
	IOMemoryMap *fHomeMap = nullptr;
	uint32_t fCdclkKhz = 0;           // the display clock as the firmware set it (CDCLK_CTL): scaled timings' pixel rate
	bool fScalerPublished = false;
	volatile SInt64 fVramBytes = 0;   // system memory handed out as nvAllocVram   // write-combined CPU view of the scan-out surface (NMXAccel copies frames here)
	// WindowServer paces a display on its VBL interrupt; until the pipe's vblank interrupt is wired, a timer at the
	// measured refresh stands in (NMIntelFB, NVRMFB's fallback)
	struct Irq { IOFBInterruptProc proc = nullptr; OSObject *target = nullptr; void *ref = nullptr; };
	// VBL has several clients: IOFramebuffer itself and the accelerator's display pipe (IOAccelDisplayPipe, whose
	// events CoreDisplay waits on). One slot let the later registration silently replace the earlier one.
	static constexpr int kVblSlots = 4;
	Irq fVbl[kVblSlots], fConnect;
	volatile bool fVblOn[kVblSlots] = {};
	thread_call_t fVblTimer = nullptr;
	thread_call_t fReplug = nullptr;  // the panel's port came back: retrain (IntelXeLink), off the VBL thread
	// Display interrupts through IntelXe.kext ("intelxeDisplayIrq"): the vblank interrupt wakes the VBL thread call at the
	// start of vblank (flip-done, VBL clients), hotplug comes from the SDE interrupt. The timer then only runs the VRR
	// tick, LFC and a fallback wake 1.5 ms after an expected vblank; if vblank interrupts stop, polling takes over.
	volatile bool fIrqOn = false;
	// Hotplug, interrupt driven (v10f): irqHandle decodes the SDE pins and their long/short status, counts storms and
	// queues fHpdCall; hpdWork does what intel_dp_hpd_pulse does for the panel's port (long: detect + link check,
	// short: the sink's IRQ_HPD vectors + link check), off every interrupt and VBL thread. Polling (pollHpd from the VBL
	// thread) only without the display interrupt or for a pin masked after a storm.
	volatile UInt32 fHpdLong = 0, fHpdShort = 0;   // SDE pin bits with a pulse not yet handled
	thread_call_t fHpdCall = nullptr, fHpdReenableCall = nullptr;
	IOSimpleLock *fHpdImrLock = nullptr;           // SDEIMR read-modify-write (interrupt thread vs. re-enable)
	uint32_t fHpdPin = 0;                          // the SDE bit of the panel's port (TRANS_DDI_FUNC_CTL at start)
	volatile uint32_t fHpdStormMask = 0;           // pins masked after a storm: polled until fHpdReenableCall
	uint64_t fHpdStormT0[32] = {}; int fHpdStormCount[32] = {};
	uint32_t fHpdLongs = 0, fHpdShorts = 0, fHpdStorms = 0, fHpdRetrains = 0, fHpdRetrainFails = 0, fHpdEsi = 0;
	uint32_t fHpdFailStreak = 0;                   // retrains failed in a row: 3 stop short-pulse retrains (link.retrain_disabled)
	bool hpdOutputDown() const { return fSysSuspended || fDpmsDown || (gDpms && fBlanked); }
	uint32_t fShpdFilterBoot = ~0u;               // the firmware's SHPD_FILTER_CNT, put back at stop
	// v10i: the hotplug detection enables (SHOTPLUG_CTL_TC/_DDI enable bit 3 of each pin nibble) and SOUTH_CHICKEN1 as
	// the firmware left them at the first registration. D3cold clears them (after the 14:49 wake SHOTPLUG_CTL_TC read 0
	// and the hotplug interrupt came back with no pin); Linux programs them again on every resume (intel_hpd_init ->
	// xelpdp_hpd_irq_setup -> mtp_hpd_irq_setup: mtp_ddi/tc_hpd_detection_setup).
	uint32_t fHpdEnTc = 0, fHpdEnDdi = 0, fSouthChicken1Boot = 0; bool fHpdEnSaved = false;
	uint8_t fSinkCount = 0xFF;                     // DP_SINK_COUNT of a branch device (0xFF: not read / no branch)
	char fHpdLast[160] = "none";
	static void hpdFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->hpdWork(); }
	static void hpdReenableFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->hpdReenable(); }
	void hpdIrq(uint32_t pins, uint32_t longs);
	void hpdWork();
	void hpdReenable();
	bool hpdLinkOk(bool *checked);
	int hpdRetrain(const char *why);
	void hpdShortPulse();
	IOService *fIntelXe = nullptr;
	volatile uint32_t fIrqVbl = 0, fIrqUnderruns = 0, fIrqHpd = 0, fIrqCalls = 0;   // written on IntelXe's interrupt thread
	uint32_t fIrqVblLast = 0, fIrqMiss = 0;
	static void irqThunk(void *ctx, uint32_t master) { ((IntelXeFramebuffer *)ctx)->irqHandle(master); }
	void irqHandle(uint32_t master);
	bool irqRegister(bool on);
	IOService *intelxe() {
		if (!fIntelXe)
			if (OSDictionary *m = IOService::serviceMatching("IntelXe")) {
				if (OSIterator *it = IOService::getMatchingServices(m)) {
					if ((fIntelXe = OSDynamicCast(IOService, it->getNextObject()))) fIntelXe->retain();
					it->release();
				}
				m->release();
			}
		return fIntelXe;
	}
	// GGTT ranges IntelXeFB writes PTEs in, registered with IntelXe.kext's range allocator ("intelxeGgttAlloc", fixed address)
	// so nothing else is ever given them: the scan-out slot window, the cursor window and the firmware's surface.
	// 1 = reserved, 0 = IntelXe.kext has no allocator (older kext: as before), -1 = taken by someone else.
	uint32_t fGgttHeld[4] = {}; int fNumGgttHeld = 0;
	int ggttReserve(uint32_t ggtt, uint64_t size, const char *what) {
		IOService *b = intelxe();
		struct { uint32_t version, flags; uint64_t size, align; uint32_t ggtt; } r = { 1, 1, (size + 0xFFF) & ~0xFFFull, 0x1000, ggtt };
		const OSSymbol *sym = OSSymbol::withCString("intelxeGgttAlloc");
		const IOReturn rr = (b && sym) ? b->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
		if (sym) sym->release();
		if (rr == kIOReturnSuccess) { if (fNumGgttHeld < 4) fGgttHeld[fNumGgttHeld++] = ggtt; FBLOG("ggtt: 0x%x-0x%llx reserved for %s", ggtt, ggtt + r.size - 1, what); return 1; }
		if (rr == kIOReturnBusy || rr == kIOReturnNoSpace) { FBLOG("ggtt: 0x%x (%s) is not free in IntelXe.kext's allocator (0x%x)", ggtt, what, (unsigned)rr); return -1; }
		FBLOG("ggtt: no range allocator in IntelXe.kext (0x%x), %s used as before", (unsigned)rr, what);
		return 0;
	}
	void ggttRelease() {
		const OSSymbol *sym = OSSymbol::withCString("intelxeGgttFree");
		for (int i = 0; i < fNumGgttHeld; i++) if (fIntelXe && sym) (void)fIntelXe->callPlatformFunction(sym, false, (void *)(uintptr_t)fGgttHeld[i], nullptr, nullptr, nullptr);
		if (sym) sym->release();
		fNumGgttHeld = 0;
	}
	bool fSlotsOk = true;
	// System sleep (IntelXe.kext "intelxePowerEvent", phase F): the display loses its power with the GPU (D3cold), GGTT PTEs
	// of IntelXeFB's own ranges and VRAM outside the kernel's tracked buffers included.
	volatile bool fSysSuspended = false;   // between will-suspend and the end of the restore: no flips
	thread_call_t fResumeCall = nullptr;
	uint32_t fCoreSaved[48]; bool fCoreSavedOk = false;
	uint32_t fSuspends = 0, fResumes = 0, fResumeFails = 0;
	// The pipe's colour state as it was before the sleep (pipeColorSave), put back by pipeColorCommit; and what the
	// plane/pipe colour registers read right after the last wake, before anything wrote them (debug.intelxefb_pminfo).
	uint32_t fCscModeSaved = 0, fCscSaved[kNumCscRegs] = {}, fBottomSaved = 0; bool fColorSavedOk = false;
	uint32_t fWakeColorCtl = ~0u, fWakeCscMode = ~0u, fWakeGammaMode = ~0u, fWakePlaneCtl = ~0u;
	void pipeColorSave();
	void pipeColorCommit();
	void planeNoarm(int p);
	static void powerThunk(void *ctx, uint32_t event) { ((IntelXeFramebuffer *)ctx)->powerEvent(event); }
	void powerEvent(uint32_t event);
	static void resumeFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->resumeDisplay(); }
	void resumeDisplay();
	void coreSaveRestore(bool restore);
	void rewritePtes();
	bool powerRegister(bool on);
	int fSlotsOwned = 0, fCurOwned = 0;   // ggttReserve's answers for the slot window and the cursor window
	// Boot guard (replaces the LaunchDaemon's marker file /var/db/intelxefb.attempt): NVRAM intelxe-guard holds the stage this
	// boot reached and has not proven yet ("fb<n>" framebuffer only, "accel" once NMXAccel armed the display pipe). It
	// is deleted once the desktop has run for a minute, or at a clean shutdown after a steady desktop; a boot that ends
	// earlier (panic, hang, power cut) leaves it behind for the next start (guardBegin).
	IOLock *fGuardLock = nullptr;
	int fGuardStage = 0;                         // 1 framebuffer, 2 accelerated, 0 nothing open
	bool fAccelSafe = false;                     // IntelXeFBAccelSafe: arm with NMXAccel's conservative settings
	unsigned fGuardAccelFails = 0;               // accelerated boots in a row before this one that ended inside their first minute
	uint64_t fGuardT0 = 0, fAccelT0 = 0;         // when each stage began (mach absolute time)
	uint64_t fVblEvtAt = 0, fVblStableMax = 0;   // last VBL client change; longest stretch with clients and no change
	volatile uint32_t fScanouts = 0;             // NMXAccel frame calls answered (copies and flips)
	uint32_t fScanoutsAtAccel = 0;               // fScanouts when the accelerated stage began
	// WindowServer itself (v10c): its IOFramebuffer server connection (IOServiceOpen type kIOFBServerConnectType from the
	// process named WindowServer) - how many, the last one - and its WindowServer-active attribute. VBL clients alone are
	// not WindowServer: at the 12:48 boot (2026-10-29) NMXAccel's display pipe registered them and made one frame call,
	// WindowServer never started (deadlocked), and the guard still cleared ("accelerated desktop healthy").
	volatile uint32_t fWsOpens = 0;
	uint64_t fWsOpenAt = 0;
	bool fWsActive = false;
	thread_call_t fGuardCall = nullptr;
	bool fAccelAllowed = false;
	void guardBegin();
	IOReturn guardAccel();
	void guardCheck(bool shutdown);
	static void guardFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->guardCheck(false); }
	int vblClients() const { int n = 0; for (int i = 0; i < kVblSlots; i++) if (fVbl[i].proc) n++; return n; }
	void vblEvent() {   // before a VBL client comes or goes: the stretch that ends now counts if clients were there
		const uint64_t now = mach_absolute_time();
		if (vblClients() > 0 && fVblEvtAt && now - fVblEvtAt > fVblStableMax) fVblStableMax = now - fVblEvtAt;
		fVblEvtAt = now;
	}
	void irqWatch(bool hw) {   // in the VBL thread: frames that came without a vblank interrupt
		if (!fIrqOn) return;
		if (fIrqVbl != fIrqVblLast) { fIrqVblLast = fIrqVbl; fIrqMiss = 0; }
		else if (hw && ++fIrqMiss > 16) { fIrqOn = false; FBLOG("display irq: 16 frames without a vblank interrupt (%u so far) -- polling again", fIrqVbl); }
	}
	uint32_t fHpdPanel = 0;           // the hotplug bits of the ports lit at the first poll
	uint64_t fVblNext = 0;
	volatile bool fStopping = false;
	volatile uint8_t *fRegs = nullptr;
	IOPhysicalAddress fSurfPhys = 0;   // BAR2 physical base + the surface's VRAM address
	uint32_t fWidth = 0, fHeight = 0, fRowBytes = 0, fSurf = 0, fRefresh16 = 60 << 16;
	uint64_t fFrameNs = 16666667ull, fDpa = 0;
	uint32_t fHT = 0, fVT = 0, fHSS = 0, fHSE = 0, fVSS = 0, fVSE = 0, fRefreshMilliHz = 60000;
	uint64_t fPclk = 0;               // the firmware's pixel clock (driver modes run at it unless they name a higher one)
	uint64_t fPclkMax = 0;            // the highest pixel clock a mode may use: fPclk, or a higher EDID timing's (buildModes)
	uint64_t fPclkCur = 0;            // the pixel clock now: a programmable mode may run another (M/N change)
	uint32_t fDataMBoot = 0, fDataNBoot = 0, fLinkMBoot = 0, fLinkNBoot = 0, fLinetimeBoot = 0;
	int fPipe = -1;
	uint32_t fLastFrame = 0;
	bool fVblLocked = false;          // the timer follows the pipe's vblank timestamps
	volatile bool fBlanked = false;   // display asleep: the plane is off (read by the VBL and cursor paths)
	// Modes (v10m): built at start from the monitor's EDID (buildModes, fb/IntelXeEdid.cpp): Linux's EDID mode list,
	// checked as i915 checks a DP mode, gives the sizes and refresh rates; ids 1..fNumModes in table order, 1 = the
	// firmware's own timing. A size other than the panel's is the plane scaled by the pipe's scaler 0 (the pipe keeps the
	// firmware's timing and link, skl_program_plane_scaler); a refresh below the firmware's is the same pixel clock and
	// line length with a longer vertical total (Linux's live LRR path, intel_set_transcoder_timings_lrr), so the link and
	// the PLL stay as the GOP trained them. Without a usable EDID: v10l's 5 sizes x 5 rates (fallbackDriverModes).
	uint32_t fModeW = 0, fModeH = 0, fModeRB = 0, fModeVT = 0;
	// WindowServer's coordinate space for the cursor: the mode's size as reported (a HiDPI mode's scaled size, while the
	// plane runs the panel's size; fModeW/H otherwise)
	uint32_t fCurSpaceW = 0, fCurSpaceH = 0;
	IODisplayModeID fMode = 1;
	bool fScaling = true;             // -intelxefb_noscale offers the native size only
	bool fRates = true;               // -intelxefb_norates offers the firmware's refresh only
	bool fCommonRates = true;         // -intelxefb_edidonly: no VRR common rates (60/75/100/120) beyond the EDID's
	static constexpr IODisplayModeID kDriverModeIdLimit = 0x1000;   // driver ids are below; CoreDisplay's 0x80000000 up
	intelxeedid::DriverMode fModes[intelxeedid::kMaxDriverModes];
	int fNumModes = 0;
	bool fModesFromEdid = false;
	intelxeedid::Result *fEdidParse = nullptr;   // the parsed EDID (IOMalloc, ~10 KB), null without one
	uint32_t fEdidValid = 0, fEdidRejected = 0;
	void parseEdid();
	void buildModes();
	uint32_t fHpd = ~0u, fHpdFrames = 0;   // last SDEISR hotplug bits, frames since the last poll
	uint8_t fEdid[kEdidMax];
	uint32_t fEdidLen = 0;
	uint32_t fAux = 0;                // AUX_CH_CTL of the monitor's port (found by probeEdid)
	// Serializes display-register changes: blank, mode changes and the debug.intelxefb_link actions (fb/IntelXeLink.cpp),
	// which take the pipe down for up to ~2 s and restore the plane as blank() last asked.
	IOLock *fDispLock = nullptr;
	void dispLock() { if (fDispLock) IOLockLock(fDispLock); }
	void dispUnlock() { if (fDispLock) IOLockUnlock(fDispLock); }
	// Stage 4: the pipe gamma table NMXAccel last loaded (kept so it can be put back after the display wakes or the
	// link is retrained), and the GAMMA_MODE the firmware left (restored when NMXAccel turns the table off).
	uint32_t fGammaLut[kGammaSize];
	bool fGammaOn = false;
	bool fGammaAllowed = true;        // -intelxefb_nogamma: do not offer or load a pipe gamma table
	uint32_t fGammaModeBoot = 0;
	uint32_t fGammaLoads = 0;
	// Stage 3 scan-out slots (NMXScanout) and the one flip in flight, completed by vblFire. fFlipLock guards fPend only:
	// vblFire must not wait for fDispLock, which a link retrain holds for seconds.
	// runs/nRuns: the buffer's VRAM as DPA runs (a copy, for the PTE rewrite after sleep); nRuns 0: contiguous at dpa
	struct Slot { bool used; uint32_t gen, w, h, pitch; uint64_t dpa, size; IOMemoryMap *cpu; NMXDpaRun *runs; uint32_t nRuns; };
	// one PTE per 4 KB page of a slot: contiguous from dpa, or through each run in order
	void slotPtes(const Slot &sl, uint32_t g) {
		if (!sl.nRuns) { for (uint64_t off = 0; off < sl.size; off += 0x1000) pteW(g + (uint32_t)off, (sl.dpa + off) | fPteFlags); return; }
		uint64_t at = 0;
		for (uint32_t k = 0; k < sl.nRuns && at < sl.size; k++)
			for (uint64_t off = 0; off < sl.runs[k].len && at < sl.size; off += 0x1000, at += 0x1000) pteW(g + (uint32_t)at, (sl.runs[k].dpa + off) | fPteFlags);
	}
	Slot fSlot[kSlots] = {};
	uint32_t fSlotGen = 0, fSlotNext = 0;
	uint64_t fPteFlags = 0;            // the firmware surface's PTE bits (present, local memory, PAT), copied to the slots
	IOPhysicalAddress fBarBase = 0;
	IOPhysicalLength fBarLen = 0;
	IOSimpleLock *fFlipLock = nullptr;
	struct Pend { bool on, aborted; uint32_t ggtt; void (*done)(void *, unsigned long long, int); void *ctx; unsigned long long cookie; uint64_t t0; };
	Pend fPend = {};
	uint64_t fPend100ms = 0;
	uint32_t fFlips = 0, fFlipLate = 0;
	// DP Adaptive-Sync (VRR). Capable: the EDID's range limits span more than 10 Hz (intel_vrr_is_capable), the sink
	// ignores MSA timing (DPCD 0x007 bit 6) and is not a branch device. fVrrOn, the TRANS_PUSH writes and the timer's
	// view of them change under fFlipLock (the VBL thread never takes fDispLock); enable/disable also hold fDispLock.
	bool fVrrAllowed = true;          // -intelxefb_novrr: never enabled, the sysctl refuses
	bool fVrrCapable = false;
	volatile bool fVrrOn = false;
	int fVrrForce = -1;               // debug.intelxefb_vrr: 1/0 force on/off, -1 follow the mode (a timing with a vertical blanking extension)
	bool fVrrModeWants = false;       // the current mode is one of CoreDisplay's variable-refresh timings
	uint32_t fVrrModeExt = 0;         // its verticalBlankingExtension (lines past the vertical total)
	bool vrrWanted() const { return fVrrForce >= 0 ? fVrrForce != 0 : fVrrModeWants; }
	// Programmable modes (macOS Adaptive-Sync): CoreDisplay installs detailed timings when IOFBTimingRange is published,
	// and a "variable" twin of each with verticalBlankingExtension when the range's supportedSyncFlags has bit 0x10
	// (CoreDisplay IODisplayInstallTimings / IOFBInstallMode, macOS 15). Only timings this pipe can run without a modeset
	// are taken: the firmware's active size, horizontal timing and syncs, any vertical total from the firmware's up (LRR),
	// any pixel clock up to the firmware's (an M/N change on the trained link).
	struct Prog { IODisplayModeID id; IODetailedTimingInformationV2 t; };
	static constexpr int kMaxProg = 512;   // HiDPI adds CoreDisplay's scaled timings (and their variable twins) to these
	Prog fProg[kMaxProg];
	int fNumProg = 0;
	uint32_t fProgRejects = 0;
	bool fRangePublished = false;
	uint8_t fEdidOut[kEdidMax];       // the EDID handed to IODisplay (with the firmware's timing added, see vrrModesInit)
	bool fEdidAug = false;
	uint32_t fEdidOutLen = 0;         // fEdidOut's length (a DisplayID block may have been appended)
	const char *progCheck(const IODetailedTimingInformationV2 &t) const;
	const Prog *progFind(IODisplayModeID id) const { for (int i = 0; i < fNumProg; i++) if (fProg[i].id == id) return &fProg[i]; return nullptr; }
	bool modeLookup(IODisplayModeID mode, uint32_t *w, uint32_t *h, uint32_t *vt, uint64_t *pclk, uint32_t *ext) const {
		if (const Prog *pm = progFind(mode)) {
			const bool sc = pm->t.horizontalScaled && pm->t.verticalScaled;   // a HiDPI timing: the plane scans out more
			*w = sc ? pm->t.horizontalScaled : fWidth; *h = sc ? pm->t.verticalScaled : fHeight;
			*vt = pm->t.verticalActive + pm->t.verticalBlanking;
			*pclk = pm->t.pixelClock; *ext = pm->t.verticalBlankingExtension;
			return true;
		}
		*pclk = fPclk; *ext = 0;
		if (!modeSize(mode, w, h, vt)) return false;
		if (const uint32_t k = fModes[mode - 1].pclkKhz) *pclk = (uint64_t)k * 1000;   // a rate above the firmware's
		return true;
	}
	void programPixelClock(uint64_t pclk);
	void vrrModesInit();
	void publishTimingRange(bool on);
	void publishTiming();
	void applyVrrLocked(const char *why);
	uint32_t fVrrMinHz = 0, fVrrMaxHz = 0;
	uint32_t fVrrVmin = 0, fVrrVmax = 0, fVrrGuard = 0, fVrrCtlBoot = 0;
	uint32_t fVrrVminUs = 0, fVrrVmaxUs = 0;
	uint32_t fVrrFl = 0, fVrrVm = 0, fVrrFlUs = 0, fVrrVmUs = 0;   // the window programmed now (flipline, vmax)
	uint32_t fVrrReqs = 0;            // flips that carried CoreDisplay's window
	bool fVrrWin = false;             // the last flip carried CoreDisplay's window: it paces, the pacer and LFC stay out
	bool fLfcActive = false;          // LFC engaged (with hysteresis, see lfcOnFlip)
	uint32_t fLfcEnter = 0;           // consecutive slow intervals seen
	uint32_t fPaceFrames = 0;         // frames the cadence pacer windowed
	uint32_t fPaceUs = 0;             // the content's cadence: EMA (1/8) of the intervals between new frames on the panel
	uint32_t fPaceLastUs = 0, fPaceLastSurf = 0, fPaceN = 0;   // fPaceN: intervals learned since the last reset
	uint32_t fPaceC = 0;              // the window's centre now (us), ramped toward the cadence (or its LFC fraction)
	bool fPaceLfc = false;            // the cadence is below the panel's range: frames are a fraction of it
	uint32_t fPaceLfcFrames = 0;      // frames shown as a fraction of a below-range cadence
	// FLIPLINE (earliest decision line, the shortest frame) and VMAX (the longest) for the frame now scanning out
	void vrrPace(uint32_t stampUs);
	void setWindow(uint32_t fl, uint32_t vm) {
		if (fl != fVrrFl) { wr(TRANS_VRR_FLIPLINE(fPipe), fl - 1); fVrrFl = fl; fVrrFlUs = (uint32_t)((uint64_t)fHT * fl * 1000000ull / fPclkCur); }
		if (vm != fVrrVm) { wr(TRANS_VRR_VMAX(fPipe), vm - 1); fVrrVm = vm; fVrrVmUs = (uint32_t)((uint64_t)fHT * vm * 1000000ull / fPclkCur); }
	}
	// VRR time stamps for CoreDisplay (NMXAccel "nmxVrrStamp" registration): per scanned-out frame, the frame's start
	// and the earliest/latest start of the next one, as AppleIntelFramebuffer::reportVRRTimeStampInfoInInterrupt hands
	// IOAccelDisplayPipe::reportVRRStampInfo { u64 frameStart, nextEarliest, nextLatest, 0, 0 } (mach absolute time)
	void (*volatile fStampFn)(void *ctx, const uint64_t *info, unsigned n) = nullptr;
	void *fStampCtx = nullptr;
	uint32_t fStamps = 0;
	void vrrStamp(uint32_t stampUs);             // the last flip did: CoreDisplay paces the frames, the driver's LFC stays out
	uint8_t fMsaBoot = 0;             // DPCD 0x107 before the first enable
	bool fVrrSaved = false;           // the firmware's VRR registers (put back by every disable)
	uint32_t fVrrVsyncBoot = 0, fSdpTlBoot = 0;
	uint32_t fLastVblUs = 0;          // FRMTMSTMP of the last frame the timer saw
	uint32_t fLastCbUs = 0;           // when the VBL clients were last called
	uint64_t fWakeAt = 0;             // the timer's deadline (under fFlipLock)
	// LFC: below the panel's range the timer pushes repeats so the panel refreshes at a multiple of the content rate
	uint32_t fLastFlipUs = 0, fFlipIvUs = 0, fLfcNextUs = 0, fLfcLeft = 0, fLfcStepUs = 0;
	uint32_t fVrrPushes = 0, fLfcPushes = 0, fVrrFrames = 0;
	void evLog(uint16_t type, uint16_t x, uint32_t a, uint32_t b, uint32_t us = 0) {
		if (!gVrrLogOn || !fRegs) return;
		uint32_t i = (uint32_t)OSIncrementAtomic((volatile SInt32 *)&gVrrLogIdx) & (kVrrLogN - 1);
		gVrrLog[i] = { us ? us : rd(TIMESTAMP_CTR), type, x, a, b };
	}
	// Push: the next decision line ends the frame, so whatever was armed (a flip, the plane after display sleep, a
	// gamma table, a mode's plane) reaches the panel within one minimum-length frame instead of waiting for vmax.
	void push(uint16_t why) {
		if (!fFlipLock || fPipe < 0) return;
		IOSimpleLockLock(fFlipLock);
		if (fVrrOn) { wr(TRANS_PUSH(fPipe), TRANS_PUSH_EN | TRANS_PUSH_SEND); fVrrPushes++; }
		IOSimpleLockUnlock(fFlipLock);
		if (fVrrOn) evLog(kEvPush, why, 0, 0);
	}
	uint32_t usNow() const { return rd(TIMESTAMP_CTR); }
	// The timer's deadline: the VBL thread sets it (fromTimer), anyone else only brings it forward (a flip just pushed).
	void rearm(uint64_t deadline, bool fromTimer) {
		if (!fVblTimer || !fFlipLock || fStopping) return;
		IOSimpleLockLock(fFlipLock);
		if (fromTimer || deadline < fWakeAt) { fWakeAt = deadline; fVblNext = deadline; thread_call_enter_delayed(fVblTimer, deadline); }
		IOSimpleLockUnlock(fFlipLock);
	}
	// Timer coalescing lets a thread call fire up to 1/8 of its delay late (timer_call_slop for normal-urgency kernel
	// timers, about 0.7 ms on a 5.6 ms frame). Long waits are cut short so the last leg, and with it the slop, is small.
	static uint32_t early(uint32_t us) { return (gVblPrecise && us > 300) ? us - us / 7 : us; }
	static uint64_t absIn(uint64_t us) { uint64_t d; nanoseconds_to_absolutetime(us * 1000, &d); return mach_absolute_time() + d; }
	// Hardware cursor: IOFramebuffer's setCursorImage/setCursorState (what WindowServer uses when getAttribute
	// kIOHardwareCursorAttribute says so; NVRMFB does the same). fCurLock (a spinlock: setCursorState also comes from
	// IOFramebuffer's VBL handler on the timer thread) guards the cursor registers and state; link actions park the
	// cursor (IntelXeLink refuses to run with it on) through fCurParked.
	IOSimpleLock *fCurLock = nullptr;
	IOLock *fCurImgLock = nullptr;    // the image: setCursorImage and mode changes re-render it
	IOMemoryMap *fCurMap = nullptr;   // write-combined CPU view of the cursor image in VRAM
	uint32_t *fCurSrc = nullptr;      // the last converted image (straight ARGB, as convertCursorImage returns it)
	uint32_t fCurSrcW = 0, fCurSrcH = 0, fCurCtl = 0, fCurDdb = 0, fCurImages = 0, fCurMoves = 0;
	int32_t fCurX = 0, fCurY = 0;
	bool fCurReady = false, fCurHave = false, fCurVisible = false, fCurParked = false, fCurWm = false, fCurAllowed = true;
	uint64_t fCurDpa = kCurDpa;       // the image's VRAM: IntelXe.kext's display buffer (fCurBo) or the fixed fallback
	void *fCurBo = nullptr;           // the display buffer's cookie ("intelxeDispVramFree")
	uint32_t fCurBoTries = 0;         // allocation attempts after the first one at start (cursorBufferUpgrade)
	bool cursorBufferAlloc(uint64_t *dpa, void **cookie, IOMemoryMap **map);
	void cursorBufferUpgrade();
	// Wa_16023588340 (BMG): CPU writes through BAR2 go into the GPU's L2, which the display never reads; Linux flushes
	// the L2 after the CPU wrote a display buffer (xe_dsb_buffer_flush_map, xe_fb_pin's DPT) and before each flip
	// (intel_td_flush). IntelXe.kext "intelxeL2Flush" = xe_device_l2_flush.
	uint32_t fL2Flushes = 0, fL2Fails = 0;
	void l2Flush();
	// v10k diagnostics: the last cursor image written, as debug.intelxe.dispcheck (rb7d) prints it: word-wise FNV-1a over
	// the 256 KB and its first 4 words, and how many images were rendered
	uint32_t fCurFnv = 0, fCurW[4] = {}, fCurRenders = 0;
	bool cursorInit();
	void cursorRender();
	void cursorWriteLocked();
	void cursorPark(bool park) {
		if (!fCurLock) return;
		IOSimpleLockLock(fCurLock); fCurParked = park; cursorWriteLocked(); IOSimpleLockUnlock(fCurLock);
	}
	void cursorRefresh() { cursorPark(fCurParked); }
	bool vrrCompute(uint32_t vt);
	bool vrrEnableLocked();
	void vrrDisableLocked(const char *why);
	void vrrProbeCaps();
	void lfcOnFlip(uint32_t now);
	void vblFireVrr();
public:
	int vrrSet(int on);
	int vrrState() const { return fVrrOn ? 1 : 0; }
	int irqState() const { return fIrqOn ? 1 : 0; }
	int irqSet(bool on) { return irqRegister(on) == on ? 0 : EIO; }
	int vrrModesState() const { return fRangePublished ? 1 : 0; }
	void vrrModesSet(bool on) { gVrrModes = on; dispLock(); publishTimingRange(on); dispUnlock(); }
	int vrrInfo(char *buf, int len);
private:
	void pteW(uint32_t ggtt, uint64_t v) { *(volatile uint64_t *)(fRegs + kGsm + (ggtt >> 12) * 8) = v; }
	uint32_t surfArmed() const { return rd(PLANE_SURF(fPipe)) & ~0xFFFu; }
	uint32_t surfLive() const { return rd(PLANE_SURFLIVE(fPipe)) & ~0xFFFu; }
	// Abandon the flip in flight (its frame will not be the one on the panel); vblFire reports it.
	void abortPend() { if (!fFlipLock) return; IOSimpleLockLock(fFlipLock); if (fPend.on) fPend.aborted = true; IOSimpleLockUnlock(fFlipLock); }
	bool pendOn(uint32_t ggtt) {
		bool b = false;
		if (fFlipLock) { IOSimpleLockLock(fFlipLock); b = fPend.on && (!ggtt || fPend.ggtt == ggtt); IOSimpleLockUnlock(fFlipLock); }
		return b;
	}
	// The home surface: the firmware's, or the HiDPI buffer while a mode larger than the panel is set
	uint32_t homeGgtt() const { return fHomeBig ? kHomeGgtt : fSurf; }
	IOPhysicalAddress homePhys() const { return fHomeBig ? fBarBase + (IOPhysicalAddress)fHomeDpa : fSurfPhys; }
	IOMemoryMap *homeMap() const { return fHomeBig ? fHomeMap : fSurfMap; }
	bool homeBigAlloc();
	void homeBigFree();
	void publishScalerInfo();
	// Plane back to the home surface (dispLock held). A no-op when it is already there.
	void homeLocked() {
		if (fPipe < 0 || surfArmed() == homeGgtt()) return;
		wr(PLANE_SURF(fPipe), homeGgtt() | (rd(PLANE_SURF(fPipe)) & 0xFFFu));
		push(3);
		abortPend();
	}
	int slotOf(uint32_t ggtt, uint32_t gen) const {
		if (ggtt < kSlotGgtt) return -1;
		uint32_t i = (ggtt - kSlotGgtt) / kSlotSpan;
		return (i < (uint32_t)kSlots && ggtt == kSlotGgtt + i * kSlotSpan && fSlot[i].used && fSlot[i].gen == gen) ? (int)i : -1;
	}
	void slotClearLocked(int i) {
		Slot &sl = fSlot[i];
		const uint32_t g = kSlotGgtt + (uint32_t)i * kSlotSpan;
		for (uint64_t off = 0; off < sl.size; off += 0x1000) pteW(g + (uint32_t)off, 0);
		OSSafeReleaseNULL(sl.cpu);
		if (sl.runs) IOFree(sl.runs, sl.nRuns * sizeof(NMXDpaRun));
		sl = {};
	}
	IOReturn scanout(NMXScanout *r);
	// Caller holds dispLock. PREC_PAL_INDEX is set to 0 first and then with auto-increment, as Linux does (the hardware
	// ignores the index bits in the same write that sets auto-increment), and reset to 0 after.
	void loadGammaLocked() {
		if (fPipe < 0 || !fRegs) return;
		const int p = fPipe;
		if (!fGammaOn) { wr(GAMMA_MODE(p), fGammaModeBoot); push(4); return; }
		wr(PREC_PAL_INDEX(p), 0);
		wr(PREC_PAL_INDEX(p), PAL_PREC_AUTO_INCREMENT);
		for (uint32_t i = 0; i < kGammaSize; i++) wr(PREC_PAL_DATA(p), fGammaLut[i]);
		wr(PREC_PAL_INDEX(p), 0);
		for (int c = 0; c < 3; c++) { wr(PREC_PAL_EXT_GC_MAX(p, c), 1u << 16); wr(PREC_PAL_EXT2_GC_MAX(p, c), 1u << 16); }
		wr(GAMMA_MODE(p), (fGammaModeBoot & ~(POST_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_MASK)) | POST_CSC_GAMMA_ENABLE | GAMMA_MODE_MODE_10BIT);
		push(4);   // GAMMA_MODE latches at the next decision line
		fGammaLoads++;
	}
	void reloadGamma() { if (!fGammaOn) return; dispLock(); loadGammaLocked(); dispUnlock(); }

	uint32_t rd(uint32_t off) const { return *(volatile uint32_t *)(fRegs + off); }
	void wr(uint32_t off, uint32_t v) { *(volatile uint32_t *)(fRegs + off) = v; }
	int auxXfer(uint32_t ctl, const uint8_t *tx, uint32_t txn, uint8_t *rx, uint32_t rxmax);
	int i2cAux(uint32_t ctl, uint8_t req, const uint8_t *data, uint32_t len, uint8_t *out, uint8_t addr = 0x50);
public:
	int dpcd(uint32_t addr, uint8_t *buf, uint32_t len, bool write);
private:
	bool readEdid(uint32_t ctl);
	void probeEdid();
	// Display sleep turns the plane off, so the panel shows the pipe's black background as on any Mac whose display
	// sleeps; left on, the monitor kept the last frame and the machine looked frozen. Pipe, transcoder and DP link stay
	// up, so waking needs no link training. Plane registers latch on the PLANE_SURF write.
	void blank(bool off) {
		if (fPipe < 0 || !fRegs || off == fBlanked) return;
		if (!off && fDpmsDown) { dpmsWake(); return; }
		dispLock();
		uint32_t ctl = rd(PLANE_CTL(fPipe));
		wr(PLANE_CTL(fPipe), off ? ctl & ~kEnable : ctl | kEnable);
		wr(PLANE_SURF(fPipe), rd(PLANE_SURF(fPipe)));
		push(5);
		fBlanked = off;
		cursorRefresh();   // hidden while asleep
		if (!off && fGammaOn) loadGammaLocked();   // the table must still be there after the display wakes
		dispUnlock();
		FBLOG("display %s", off ? "asleep: plane off" : "awake: plane on");
		// DPMS (debug.intelxefb_dpms=1): asleep also takes the output down -- transcoder, DDI, PLL off, the sink in D3 -- as
		// Linux's crtc disable does; the wake is a full modeset at the same mode (dpmsWake). Off by default until tested.
		if (off && gDpms && fAux) {
			int r = linkAction(IntelXeLink::kPowerDown);
			fDpmsDown = r == 0;
			FBLOG("dpms: output %s (%d)", r ? "NOT powered down" : "powered down", r);
		}
	}
	volatile bool fDpmsDown = false;
	void dpmsWake() {
		dispLock(); fBlanked = false; dispUnlock();
		int r = linkAction(IntelXeLink::kModeset);
		if (r) { FBLOG("dpms: wake modeset failed (%d), once more", r); IOSleep(200); r = linkAction(IntelXeLink::kModeset); }
		fDpmsDown = false;
		dispLock();
		// the power down detached the pipe scaler; a scaled mode programs it (and its plane) again
		if (fModeW != fWidth || fModeH != fHeight) programMode(fModeW, fModeH);
		if (fGammaOn) loadGammaLocked();
		dispUnlock();
		FBLOG("dpms: output %s (%d)", r ? "NOT restored -- debug.intelxefb_link=2 to retry" : "back on", r);
	}
	bool modeSize(IODisplayModeID mode, uint32_t *w, uint32_t *h, uint32_t *vt = nullptr) const {
		if (mode < 1 || mode > fNumModes) return false;
		const intelxeedid::DriverMode &d = fModes[mode - 1];
		*w = d.w; *h = d.h;
		if (vt) *vt = d.vt;
		return true;
	}
	uint32_t refresh16(uint32_t vt) const {
		return (fPclk && fHT && vt) ? (uint32_t)((fPclk << 16) / ((uint64_t)fHT * vt)) : fRefresh16;
	}
	// LRR: TRANS_VBLANK, TRANS_VSYNC, then TRANS_VTOTAL, which latches at the transcoder's vblank (the sync pulse stays,
	// the back porch grows); then the VBL timer's period.
	void programVTotal(uint32_t vt) {
		const bool vrr = fVrrOn;   // Linux changes timings only with VRR off (intel_vblank_evade_init's warning)
		if (vrr) vrrDisableLocked("refresh mode change");
		programVTotalFixed(vt);
		if (vrr) vrrEnableLocked();
	}
	void programVTotalFixed(uint32_t vt) {
		int t = fPipe;
		uint32_t vb = rd(VBLANK(t));
		wr(VBLANK(t), (vt - 1) << 16 | (vb & 0xFFFF));
		wr(VSYNC(t), rd(VSYNC(t)));
		wr(VTOTAL(t), (vt - 1) << 16 | (fHeight - 1));
		fModeVT = vt;
		if (fPclkCur) fFrameNs = (uint64_t)fHT * vt * 1000000000ull / fPclkCur;
		publishFlipWait();
	}
	// How long NMXAccel lets WindowServer's next present wait for the frame in flight before queueing it: a refresh
	// period and a quarter (NVRMFB publishes the same property for nullmoth's NVIDIA display pipe).
	void publishFlipWait() { setProperty("NVRMFlipWaitUs", (unsigned long long)(fFrameNs * 5 / 4 / 1000), 32); }
	// Initial phase for RGB (skl_scaler_calc_phase, sub 1): -0.5 + scale/2 in u2.13, trip bit when not negative.
	static uint32_t scalerPhase(uint32_t scale) {
		int phase = -0x8000 + (int)(scale / 2);
		uint32_t trip = 0;
		if (phase < 0) phase += 0x10000; else trip = 1;
		return (((uint32_t)phase >> 2) & 0xfffe) | trip;
	}
	// Plane registers first, then the scaler, then PLANE_CTL and PLANE_SURF, whose write arms all of it for the next
	// vblank (skl_program_plane_noarm/_arm). The surface is cleared so the old layout does not flash stretched.
	void programMode(uint32_t w, uint32_t h) {
		int p = fPipe;
		uint32_t rb = (w * 4 + 63) & ~63u;
		// a mode the firmware's surface cannot hold uses the HiDPI buffer (setDisplayMode allocated it, at most kHomeMax)
		const bool big = (uint64_t)rb * h > (uint64_t)fRowBytes * fHeight;
		if (big && (!fHomeBo || (uint64_t)rb * h > kHomeMax)) { FBLOG("mode %ux%u: no HiDPI home surface, not programmed", w, h); return; }
		fHomeBig = big;
		if (IOMemoryMap *hm = homeMap()) { bzero((void *)hm->getVirtualAddress(), (size_t)rb * h); __asm__ __volatile__("sfence" ::: "memory"); l2Flush(); }
		wr(PLANE_STRIDE(p), rb / 64);
		wr(PLANE_POS(p), 0);
		wr(PLANE_SIZE(p), (h - 1) << 16 | (w - 1));
		planeNoarm(p);   // colour key, offset, CUS, PLANE_COLOR_CTL: what icl_plane_update_noarm writes besides the above
		if (w != fWidth || h != fHeight) {
			wr(PS_CTRL(p), PS_SCALER_EN | PS_BINDING_PLANE1);   // normal mode, medium filter
			wr(PS_VPHASE(p), scalerPhase((h << 16) / fHeight));
			wr(PS_HPHASE(p), scalerPhase((w << 16) / fWidth));
			wr(PS_WIN_POS(p), 0);
			wr(PS_WIN_SZ(p), fWidth << 16 | fHeight);
		} else {
			wr(PS_CTRL(p), 0); wr(PS_WIN_POS(p), 0); wr(PS_WIN_SZ(p), 0);   // skl_detach_scaler
		}
		wr(PLANE_CTL(p), rd(PLANE_CTL(p)));
		// home: a stage 3 slot holds a frame of the old size (NMXAccel's next flip to one is refused on the size)
		wr(PLANE_SURF(p), homeGgtt() | (rd(PLANE_SURF(p)) & 0xFFFu));
		push(6);
		abortPend();
		fModeW = w; fModeH = h; fModeRB = rb;
		// the cursor follows the new scale factor
		if (fCurReady) { IOLockLock(fCurImgLock); cursorRender(); IOLockUnlock(fCurImgLock); cursorRefresh(); }
	}
	// Hotplug by polling the south display's live HPD state (MTL+ SDE: DDI A/B bits 17:16, TC1-TC4 bits 27:24, enabled
	// by the firmware: SHOTPLUG_CTL_TC 0x8888): only without the display interrupt, or for a pin masked after a storm
	// (intel_hpd_irq_storm_switch_to_polling). A level poll sees neither short pulses nor a quick unplug/replug; the
	// interrupt path (hpdWork) does. When the panel's own port goes away and comes back, its link is gone: retrain it on
	// a thread call (about 150 ms, the sink's long-pulse case in intel_dp_detect / intel_dp_retrain_link).
	void pollHpd() {
		uint32_t live = rd(SDEISR) & kHpdPins;
		if (live == fHpd) return;
		FBLOG("hotplug: live ports 0x%08x -> 0x%08x (polled)", fHpd, live);
		const uint32_t panel = fHpdPin ? fHpdPin : fHpdPanel;
		if (fHpd == ~0u) fHpdPanel = live;
		else if (panel && !(fHpd & panel) && (live & panel) && fReplug) thread_call_enter(fReplug);
		if (fPipeBAllowed && fPipeBCall && fHpd != ~0u && ((fHpd ^ live) & ~panel)) thread_call_enter(fPipeBCall);   // another connector
		fHpd = live;
		setProperty("IntelXeFBHotplugLive", live, 32);
	}
	bool hpdPolling() const { return !fIrqOn || fHpdStormMask; }
	static void replugFire(thread_call_param_t p0, thread_call_param_t) {
		IntelXeFramebuffer *me = (IntelXeFramebuffer *)p0;
		if (me->fStopping || me->fDpmsDown) return;   // asleep with the output off: the wake modesets anyway
		IOSleep(500);   // let the sink's AUX come up after its hotplug pulse
		int r = me->linkAction(1);
		if (r && !me->fStopping) r = me->linkAction(2);
		if (!r && !me->fStopping) me->reloadGamma();
		FBLOG("hotplug: panel back, link %s (%d)", r ? "NOT restored" : "retrained", r);
	}
	uint64_t pte(uint32_t ggtt) const { return *(volatile uint64_t *)(fRegs + kGsm + (ggtt >> 12) * 8); }
	bool adoptFirmwareMode(IOPhysicalAddress barBase, IOPhysicalLength barLen);
	// Microseconds since the pipe's last start of vblank, or ~0u when the timestamps do not look live.
	uint32_t usSinceVblank() const {
		uint32_t since = rd(TIMESTAMP_CTR) - rd(FRMTMSTMP(fPipe));
		return since <= 2 * (fFrameNs / 1000) ? since : ~0u;
	}
	// The callback runs once per scanned-out frame (frame counter changed), just after the start of vblank: the next
	// deadline is the pipe's own next vblank (+50 us so its timestamp has latched), not a free-running period.
	static void vblFire(thread_call_param_t p0, thread_call_param_t) {
		IntelXeFramebuffer *me = (IntelXeFramebuffer *)p0;
		if (me->fStopping) return;
		if (me->fVrrOn) { me->vblFireVrr(); return; }
		uint32_t frame = me->rd(FRMCOUNT(me->fPipe));
		me->irqWatch(frame != me->fLastFrame);
		if (!me->fVblLocked || frame != me->fLastFrame) {
			if (gVrrLogOn && me->fVblLocked) me->evLog(kEvFrame, (uint16_t)(frame - me->fLastFrame), frame, 0, me->rd(FRMTMSTMP(me->fPipe)));
			me->fLastFrame = frame;
			me->fLastVblUs = me->rd(FRMTMSTMP(me->fPipe));
			for (int i = 0; i < kVblSlots; i++)
				if (me->fVbl[i].proc && me->fVblOn[i]) me->fVbl[i].proc(me->fVbl[i].target, me->fVbl[i].ref);
		}
		if (me->hpdPolling() && ++me->fHpdFrames >= 256) { me->fHpdFrames = 0; me->pollHpd(); }
		me->flipCheck();
		uint64_t frameUs = me->fFrameNs / 1000, delayNs = me->fFrameNs;
		uint32_t since = me->fVblLocked ? me->usSinceVblank() : ~0u;
		if (me->fIrqOn) delayNs = (uint64_t)((since < frameUs ? frameUs - since : 0) + 1500) * 1000;   // fallback only
		else if (since < frameUs) delayNs = (uint64_t)early((uint32_t)(frameUs - since + 50)) * 1000;
		else if (since != ~0u) delayNs = 50000;   // a vblank is due now
		uint64_t d; nanoseconds_to_absolutetime(delayNs, &d);
		me->rearm(mach_absolute_time() + d, true);
	}
	// The flip in flight is done when PLANE_SURFLIVE shows it. This runs just after the start of vblank, where armed plane
	// registers latch (the flip-done event Linux sends from its vblank work, drm_crtc_send_vblank_event).
	void flipCheck() {
		if (!fFlipLock || !fPend.on) return;
		Pend p = {};
		int st = 1;
		IOSimpleLockLock(fFlipLock);
		if (fPend.on) {
			if (fPend.aborted) st = -2;
			else if (surfLive() == fPend.ggtt) st = 0;
			else if (mach_absolute_time() - fPend.t0 > fPend100ms) st = -1;
			if (st <= 0) { p = fPend; fPend = {}; }
		}
		IOSimpleLockUnlock(fFlipLock);
		if (st > 0 || !p.done) return;
		evLog(kEvFlipDone, (uint16_t)(st + 8), p.ggtt, 0);
		if (st == 0) fFlips++;
		else if (st == -1 && fFlipLate++ < 8) FBLOG("flip to 0x%x not on the panel after 100 ms (armed 0x%x live 0x%x)", p.ggtt, surfArmed(), surfLive());
		p.done(p.ctx, p.cookie, st);
	}
public:
	// MMIO read for debug.intelxefb_peek: registers below the GGTT only, 4-byte aligned. Never writes.
	bool peek(uint32_t off, uint32_t *v) const {
		if (!fRegs || (off & 3) || off >= kGsm) return false;
		*v = rd(off);
		return true;
	}
	// debug.intelxefb_mode: program a mode's plane and scaler without WindowServer (hardware test from a remote shell;
	// WindowServer keeps drawing its own mode until it is told). setDisplayMode is the real path.
	bool testMode(IODisplayModeID mode) {
		uint32_t w, h, vt;
		if (!fRegs || fPipe < 0 || !modeSize(mode, &w, &h, &vt)) return false;
		dispLock();
		if (vt != fModeVT) programVTotal(vt);
		if (w != fModeW || h != fModeH) programMode(w, h);
		fMode = mode;
		dispUnlock();
		FBLOG("test mode %d: %ux%u, vertical total %u", (int)mode, w, h, vt);
		return true;
	}
	int currentMode() const { return (int)fMode; }
	bool poke(uint32_t off, uint32_t v) {
		if (!fRegs || (off & 3) || off < 0x40000 || off >= 0x200000) return false;
		wr(off, v);
		FBLOG("poke 0x%x = 0x%08x", off, v);
		return true;
	}
	// debug.intelxefb_link: one IntelXeLink action on the adopted pipe's output (0 dump, 1 retrain, 2/3 full modeset), under
	// the display lock so that a display sleep or mode change waits for it. Never called from anywhere else.
	static int dpcdThunk(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len, bool write) {
		return ((IntelXeFramebuffer *)ctx)->dpcd(addr, buf, len, write);
	}
	int linkAction(int action) {
		if (!fRegs || fPipe < 0) return -1;
		if (!fAux) { FBLOG("link: no AUX channel answered at start (no EDID), nothing to train over"); return -1; }
		dispLock();
		// the link actions take the transcoder down: VRR goes off first (as Linux disables it before a modeset) and
		// comes back only when the link did; a failed action leaves fixed refresh, the recovery state
		const bool readOnly = action == IntelXeLink::kDump || action == IntelXeLink::kSave;
		const bool vrr = fVrrOn && !readOnly;
		if (vrr) vrrDisableLocked("link action");
		const bool park = !readOnly && fCurReady;   // IntelXeLink runs only with the cursor off
		if (park) { cursorPark(true); IOSleep(25); }
		IntelXeLink link(fRegs, fAux, dpcdThunk, this);
		int r = link.run(action, fPipe, !fBlanked);
		if (park) cursorPark(false);
		if (r == 0 && action != IntelXeLink::kPowerDown && (vrr || vrrWanted())) vrrEnableLocked();
		dispUnlock();
		return r;
	}
	// debug.intelxefb_thumb / _crop (v10l): what the plane really scans out. The surface is PLANE_SURFLIVE's GGTT address
	// (the firmware's surface, or a scan-out slot NMXAccel bound, whose buffer the Metal plugin placed in VRAM above
	// the BAR2 window), its size and pitch from the mode or the slot; its bytes are found through the GGTT the way the
	// plane finds them: each 4 KB page's PTE gives its DPA. Runs inside the 1 GB BAR2 window are read by the CPU, the rest
	// by IntelXe.kext's copy engine ("intelxeVramRead", kernel rb7e; without it those pages read as 0). Slow, test only.
	bool liveGeom(uint32_t *ggtt, uint32_t *w, uint32_t *h, uint32_t *rb) const {
		if (!fModeW || !fModeH || fPipe < 0) return false;
		*ggtt = homeGgtt(); *w = fModeW; *h = fModeH; *rb = fModeRB;
		const uint32_t live = surfLive();
		for (int i = 0; i < kSlots; i++)
			if (fSlot[i].used && live == kSlotGgtt + (uint32_t)i * kSlotSpan) {
				*ggtt = live; *w = fSlot[i].w; *h = fSlot[i].h; *rb = fSlot[i].pitch;
			}
		return true;
	}
	// bytes [off, off + len) of the surface at GGTT `ggtt` into dst
	bool readGgtt(uint32_t ggtt, uint64_t off, uint64_t len, uint8_t *dst) {
		if (!fRegs || !len) return false;
		const uint64_t p0 = off & ~0xFFFull, p1 = (off + len + 0xFFF) & ~0xFFFull, n = p1 - p0;
		uint8_t *tmp = (uint8_t *)IOMallocZero((size_t)n);
		if (!tmp) return false;
		bool ok = true;
		for (uint64_t a = p0; a < p1 && ok;) {   // runs of pages contiguous in VRAM, all inside or all outside BAR2
			const uint64_t e = pte(ggtt + (uint32_t)a);
			if (!(e & 1) || e == ~0ull) { a += 0x1000; continue; }   // not mapped: zeros
			const uint64_t dpa = e & kPteAddr;
			const bool inBar = dpa + 0x1000 <= fBarLen;
			uint64_t run = 0x1000;
			while (a + run < p1 && run < (8ull << 20)) {
				const uint64_t f = pte(ggtt + (uint32_t)(a + run));
				if (!(f & 1) || f == ~0ull || (f & kPteAddr) != dpa + run || (dpa + run + 0x1000 <= fBarLen) != inBar) break;
				run += 0x1000;
			}
			uint8_t *to = tmp + (a - p0);
			if (inBar) {
				if (IODeviceMemory *m = IODeviceMemory::withRange(fBarBase + dpa, (IOPhysicalLength)run)) {
					if (IOMemoryMap *mm = m->map(kIOMapWriteCombineCache)) {
						const volatile uint32_t *src = (const volatile uint32_t *)mm->getVirtualAddress();
						for (uint64_t k = 0; k < run / 4; k++) ((uint32_t *)to)[k] = src[k];
						mm->release();
					} else ok = false;
					m->release();
				} else ok = false;
			} else {
				IOService *b = intelxe();
				const OSSymbol *sym = OSSymbol::withCString("intelxeVramRead");
				struct { uint32_t version, flags; uint64_t dpa, size; void *dst; } r = { 1, 0, dpa, run, to };
				const IOReturn rr = (b && sym) ? b->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
				if (sym) sym->release();
				if (rr != kIOReturnSuccess && fVramReadFails++ < 4)
					FBLOG("live view: VRAM 0x%llx +%llu KB outside BAR2 not read by IntelXe.kext (0x%x): zeros", dpa, run >> 10, (unsigned)rr);
			}
			a += run;
		}
		if (ok) bcopy(tmp + (off - p0), dst, (size_t)len);
		IOFree(tmp, (size_t)n);
		return ok;
	}
	// debug.intelxefb_crop: a full-resolution cw x ch window at (x0, y0) of the live surface, to compare the scanned-out
	// pixels with a screenshot
	bool crop(uint32_t *out, uint32_t x0, uint32_t y0, unsigned cw, unsigned ch) {
		uint32_t g, w, h, rb;
		if (!liveGeom(&g, &w, &h, &rb)) return false;
		uint32_t *row = (uint32_t *)IOMalloc(rb);
		if (!row) return false;
		bool ok = true;
		for (unsigned y = 0; y < ch && ok; y++) {
			const uint32_t sy = y0 + y < h ? y0 + y : 0;
			ok = readGgtt(g, (uint64_t)sy * rb, rb, (uint8_t *)row);
			for (unsigned x = 0; x < cw; x++) out[y * cw + x] = (x0 + x < w && y0 + y < h) ? row[x0 + x] : 0;
		}
		IOFree(row, rb);
		return ok;
	}
	bool thumb(uint32_t *out, unsigned tw, unsigned th) {
		uint32_t g, w, h, rb;
		if (!liveGeom(&g, &w, &h, &rb)) return false;
		uint32_t *row = (uint32_t *)IOMalloc(rb);
		if (!row) return false;
		bool ok = true;
		for (unsigned y = 0; y < th && ok; y++) {
			ok = readGgtt(g, (uint64_t)(y * h / th) * rb, rb, (uint8_t *)row);
			for (unsigned x = 0; x < tw; x++) out[y * tw + x] = row[x * w / tw];
		}
		IOFree(row, rb);
		return ok;
	}
	uint32_t fVramReadFails = 0;
private:
	void armVbl() {
		fVblTimer = thread_call_allocate(vblFire, this);
		if (fVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)(fFrameNs / 1000), kMicrosecondScale, &d); fVblNext = d; thread_call_enter_delayed(fVblTimer, d); }
		else FBLOG("vbl timer: thread_call_allocate failed");
	}

public:
	IOService *probe(IOService *provider, SInt32 *score) override;
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;

	IOReturn enableController() override { return kIOReturnSuccess; }
	bool isConsoleDevice() override { return true; }
	void systemWillShutdown(IOOptionBits spec) override {
		guardCheck(true);
		nvramWrite("intelxefb-shutdown", "1 IntelXeFB systemWillShutdown");   // the kernel agent's shutdown breadcrumbs (intelxe-shutdown)
		IOFramebuffer::systemWillShutdown(spec);
	}
	bool fCrumbRead = false;
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
		if (aperture != kIOFBSystemAperture) return nullptr;
		return IODeviceMemory::withRange(homePhys(), (IOPhysicalLength)fModeRB * fModeH);   // never past the home surface
	}
	IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
	const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
	// driver modes (ids 1..fNumModes), then the programmable ones CoreDisplay installed (setDetailedTimings)
	IOItemCount getDisplayModeCount() override { return (IOItemCount)fNumModes + (IOItemCount)fNumProg; }
	IOReturn getDisplayModes(IODisplayModeID *modes) override {
		IOItemCount n = 0;
		for (IODisplayModeID m = 1; m <= fNumModes; m++) modes[n++] = m;
		for (int i = 0; i < fNumProg; i++) modes[n++] = fProg[i].id;
		return kIOReturnSuccess;
	}
	IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
		uint32_t w, h, vt, ext; uint64_t pclk;
		if (!info || !modeLookup(mode, &w, &h, &vt, &pclk, &ext)) return kIOReturnBadArgument;
		bzero(info, sizeof(*info));
		info->nominalWidth = w; info->nominalHeight = h;
		info->refreshRate = (pclk == fPclk && vt == fVT) ? fRefresh16 : (uint32_t)((pclk << 16) / ((uint64_t)fHT * vt));
		// the default is the firmware's own timing (mode 1 unless an EDID rate above it sorted first)
		const bool fw = mode >= 1 && mode <= fNumModes && fModes[mode - 1].why == intelxeedid::kWhyFirmware &&
		                fModes[mode - 1].w == fWidth && fModes[mode - 1].h == fHeight;
		info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (fw ? kDisplayModeDefaultFlag : 0);
		// an EDID size of another aspect (1280x1024 on 16:9) fills the panel: say so, as Apple's drivers do
		if (mode >= 1 && mode <= fNumModes && fModes[mode - 1].stretched) info->flags |= kDisplayModeStretchedFlag;
		if (const Prog *pm = progFind(mode))
			if (pm->t.horizontalScaled && pm->t.horizontalScaled != pm->t.horizontalActive) info->flags |= kDisplayModeAcceleratorBackedFlag;
		return kIOReturnSuccess;
	}
	IOReturn validateDetailedTiming(void *description, IOByteCount size) override;
	IOReturn setDetailedTimings(OSArray *array) override;
	UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
	IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
		uint32_t w, h, vt0, ext0; uint64_t pc0;
		if (!modeLookup(mode, &w, &h, &vt0, &pc0, &ext0) || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
		bzero(pi, sizeof(*pi));
		pi->bytesPerRow = (w * 4 + 63) & ~63u; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
		pi->componentCount = 3; pi->bitsPerComponent = 8;
		pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
		strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
		pi->activeWidth = w; pi->activeHeight = h;
		return kIOReturnSuccess;
	}
	IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = fMode; *depth = 0; return kIOReturnSuccess; }
	IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
	// WindowServer showed 0.00 Hz without detailed timing (NMIntelFB)
	IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
		uint32_t w, h, vt;
		if (!info) return kIOReturnUnsupportedMode;
		if (const Prog *pm = progFind(mode)) {   // CoreDisplay's own timing, extension included
			bzero(info, sizeof(*info));
			info->appleTimingID = kIOTimingIDInvalid;
			info->flags = kIODetailedTimingValid;
			info->detailedInfo.v2 = pm->t;   // as installed (the mode id stays in __reservedA[0])
			return kIOReturnSuccess;
		}
		uint32_t ext; uint64_t pclk;
		if (!modeLookup(mode, &w, &h, &vt, &pclk, &ext)) return kIOReturnUnsupportedMode;
		bzero(info, sizeof(*info));
		info->appleTimingID = kIOTimingIDInvalid;
		if (!pclk || fHT <= fWidth || fVT <= fHeight) return kIOReturnSuccess;
		IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
		d.pixelClock = d.minPixelClock = d.maxPixelClock = pclk;
		d.horizontalActive = fWidth; d.horizontalBlanking = fHT - fWidth;
		d.horizontalSyncOffset = fHSS > fWidth ? fHSS - fWidth : 0; d.horizontalSyncPulseWidth = fHSE > fHSS ? fHSE - fHSS : 0;
		d.verticalActive = fHeight; d.verticalBlanking = vt - fHeight;
		d.verticalSyncOffset = fVSS > fHeight ? fVSS - fHeight : 0; d.verticalSyncPulseWidth = fVSE > fVSS ? fVSE - fVSS : 0;
		info->flags = kIODetailedTimingValid;
		if (w != fWidth || h != fHeight) {   // the panel timing stays native; the scaled size is what the plane scans out
			d.horizontalScaled = w; d.verticalScaled = h;
			info->flags |= kIOScalingInfoValid;
		}
		return kIOReturnSuccess;
	}
	IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
		uint32_t w, h, vt, ext; uint64_t pclk;
		if (depth != 0 || !modeLookup(mode, &w, &h, &vt, &pclk, &ext)) return kIOReturnUnsupported;
		if (mode == fMode) return kIOReturnSuccess;
		// the cursor's coordinates come in the reported size (a HiDPI mode's scaled size); the plane runs the panel's
		const uint32_t spaceW = w, spaceH = h;
		if (progFind(mode)) { w = fWidth; h = fHeight; }   // accelerator backed: WindowServer scales on the GPU
		const uint64_t need = (uint64_t)((w * 4 + 63) & ~63u) * h;
		if (need > (uint64_t)fRowBytes * fHeight && (need > kHomeMax || !homeBigAlloc())) {
			FBLOG("mode 0x%x: %ux%u needs the HiDPI home surface, which is not available", (unsigned)mode, w, h);
			return kIOReturnNoMemory;
		}
		dispLock();
		// timings change with VRR off (Linux: intel_vrr_disable before an M/N or LRR update); applyVrrLocked decides after
		if (fVrrOn && (pclk != fPclkCur || vt != fModeVT || ext != fVrrModeExt)) vrrDisableLocked("mode change");
		if (pclk != fPclkCur) programPixelClock(pclk);
		if (vt != fModeVT) programVTotalFixed(vt);
		if (w != fModeW || h != fModeH) programMode(w, h);
		if (spaceW != fCurSpaceW || spaceH != fCurSpaceH) {
			fCurSpaceW = spaceW; fCurSpaceH = spaceH;
			if (fCurReady) { IOLockLock(fCurImgLock); cursorRender(); IOLockUnlock(fCurImgLock); cursorRefresh(); }
		}
		fMode = mode;
		fVrrModeExt = ext; fVrrModeWants = ext != 0;
		applyVrrLocked("display mode");
		publishTiming();
		dispUnlock();
		const uint32_t r16 = (uint32_t)((pclk << 16) / ((uint64_t)fHT * vt));
		FBLOG("mode 0x%x: %ux%u%s, vertical total %u, pixel clock %llu kHz (%u.%02u Hz)%s", (unsigned)mode, w, h,
		      w == fWidth ? "" : " scaled to the panel", vt, pclk / 1000, r16 >> 16, ((r16 & 0xFFFF) * 100) >> 16,
		      ext ? ", variable refresh" : "");
		return kIOReturnSuccess;
	}
	IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
	IOItemCount getConnectionCount() override { return 1; }
	IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
		if (type == kIOFBVBLInterruptType) {
			for (int i = 0; i < kVblSlots; i++) if (!fVbl[i].proc) {
				vblEvent();
				fVbl[i] = { proc, target, ref }; fVblOn[i] = true;
				if (iref) *iref = &fVbl[i];
				FBLOG("vbl: client %d registered (%s)", i, target ? target->getMetaClass()->getClassName() : "no target");
				return kIOReturnSuccess;
			}
			FBLOG("vbl: no free slot"); return kIOReturnNoResources;
		}
		if (type == kIOFBConnectInterruptType) { fConnect = { proc, target, ref }; if (iref) *iref = &fConnect; return kIOReturnSuccess; }
		return kIOReturnUnsupported;
	}
	int vblSlot(void *iref) const { for (int i = 0; i < kVblSlots; i++) if (iref == &fVbl[i]) return i; return -1; }
	IOReturn unregisterInterrupt(void *iref) override {
		int i = vblSlot(iref);
		if (i >= 0) { vblEvent(); fVblOn[i] = false; fVbl[i] = {}; FBLOG("vbl: client %d unregistered", i); }
		return kIOReturnSuccess;
	}
	IOReturn setInterruptState(void *iref, UInt32 state) override {
		int i = vblSlot(iref);
		if (i >= 0) fVblOn[i] = state != 0;
		return kIOReturnSuccess;
	}
	IOReturn setCursorImage(void *cursorImage) override;
	IOReturn setCursorState(SInt32 x, SInt32 y, bool visible) override;
	IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
	IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
	IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override {
		const IOReturn r = IOFramebuffer::newUserClient(owningTask, securityID, type, handler);
		if (r == kIOReturnSuccess && type == kIOFBServerConnectType) {
			char who[32] = "";
			proc_selfname(who, sizeof who);   // IOServiceOpen runs in the opening process
			if (!strcmp(who, "WindowServer")) {
				fWsOpenAt = mach_absolute_time();
				const uint32_t n = (uint32_t)OSIncrementAtomic((volatile SInt32 *)&fWsOpens) + 1;
				FBLOG("guard: WindowServer opened the framebuffer (server connection %u)", n);
			}
		}
		return r;
	}
	// ---- Power (v10e): IOGraphics' own power protocol, the one IONDRVFramebuffer and AppleIntelFramebuffer use ----
	// IOFramebuffer.h (kIOPowerAttribute): "The IOFramebuffer class implements most power management (IOService)
	// methods. It calls the subclass to carry out the power management state change with this attribute." The path
	// (IOGraphics-599 IOFramebuffer.cpp): IOFramebuffer::setPowerState (:8004) queues the change and acknowledges later;
	// IOFramebuffer::systemWork -> IOFBController::checkPowerWork -> IOFramebuffer::checkPowerWork (:7337) marks its
	// thread as the controller's power thread (setPowerThread, :7351), calls setAttribute(kIOPowerStateAttribute), which
	// IOFramebuffer::setAttribute (:11766) turns into setAttribute(kIOPowerAttribute) -- powerApply below -- and then
	// acknowledgeSetPowerState. IOFramebuffer::handleEvent acts ONLY on that thread (:6540: off it, sendEvent = false and
	// the whole switch is skipped). v2..v10d overrode setPowerState, acknowledged at once and called handleEvent from the
	// PM work thread (v10d also from IntelXe's resume thread call): every one of those calls did nothing. A system sleep
	// clears pagingState (IOFramebuffer::systemWork :6913, after WillSleep(false)/(true) to every client); only
	// handleEvent(DidWake) or (DidPowerOn) on the power thread sets it again (:6580, :6589). So after every system sleep it
	// stayed false, and WindowServer's first framebuffer call after the wake slept for ever in IOFramebuffer::_extEntry
	// (:4176-4191, FBWL(this)->sleepGate(&fServerConnect, THREAD_UNINT)): white/grey screen, watchdogd panic 120 s later
	// (2026-10-30 03:44 v10c, 12:31 v10d; spindump IOGraphicsFamily +56852 = _extEntry+0x108). The 12:31 log has
	// "wake delivered (power state)" from the PM thread: that DidWake was one of the no-ops.
	volatile uint32_t fPowerOrd = 2;      // the power state last carried out (kPowerStates index)
	uint32_t fPmApplied[3] = {}, fPmDirect = 0, fPmOffThread = 0, fPmFallbacks = 0, fPmRecovers = 0, fPmGuardHits = 0;
	// IOGraphics' notifications to this framebuffer as its clients see them ([0] info false, [1] info true; pmTap)
	volatile SInt32 fEvWillSleep[2] = {}, fEvDidWake[2] = {}, fEvDidPowerOn = 0, fEvWillPowerOff = 0;
	IONotifier *fPmTap = nullptr;
	IOCommandGate *fSysGate = nullptr, *fFcGate = nullptr;   // on IOGraphics' system and this controller's work loops
	thread_call_t fPmGuardCall = nullptr, fPmTestCall = nullptr;
	volatile SInt32 fPmGuardSleeps = 0, fPmTestBusy = 0;
	char fPmTest[200] = "not run";
	IOReturn setPowerState(unsigned long ordinal, IOService *device) override {
		if (opened) return IOFramebuffer::setPowerState(ordinal, device);   // acknowledged by IOGraphics' power work
		// Not opened yet (WindowServer's server connection opens it): IOFramebuffer::open (:9042) is what lists the
		// framebuffer in its controller (fFbs), and the controller's power work walks only that list, so
		// IOFramebuffer::setPowerState would wait 45 s for an acknowledge -- the "acknowledge that never comes" v2 saw.
		// Nothing can wait on pagingState before the open (open sets it, :9177).
		fPmDirect++;
		powerApply((uint32_t)ordinal, false);
		return kIOPMAckImplied;
	}
	// IONDRVFramebuffer's: on while the domain (the GPU's PCI device) has power, sleep state only without it
	unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override { return (domainState & kIOPMPowerOn) ? 2 : 0; }
	unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) override { return (domainState & kIOPMPowerOn) ? 2 : 0; }
	// IONDRVFramebuffer::ndrvSetPowerState's order. family: called by IOGraphics' power work, where handleEvent acts.
	//   leaving 2: WillPowerOff, display off, ..., DidPowerOff      entering 2: WillPowerOn, ..., DidPowerOn, display on
	//   entering 0: WillSleep(true) (handleEvent keeps it: IOGraphics sent it at system sleep already)
	//   leaving 0: DidWake(true) = restoreFramebuffer, pagingState, wakeupGate(&fServerConnect), VBL, DidWake(true)+(false)
	void powerApply(uint32_t to, bool family) {
		if (to > 2) to = 2;
		const uint32_t from = fPowerOrd;
		if (to == from) return;
		IOIndex post = 0;
		if (from == 2) {
			if (family) handleEvent(kIOFBNotifyWillPowerOff);
			post = kIOFBNotifyDidPowerOff;
			blank(true);
		} else if (to == 2) {
			if (family) handleEvent(kIOFBNotifyWillPowerOn);
			post = kIOFBNotifyDidPowerOn;
		}
		if (to == 0 && family) handleEvent(kIOFBNotifyWillSleep, (void *) true);
		fPowerOrd = to;
		fPmApplied[to]++;
		bool fellBack = false;
		if (from == 0 && family) {
			handleEvent(kIOFBNotifyDidWake, (void *) true);
			// Belt and braces: if handleEvent did not act (not IOGraphics' power thread after all), do its wake work here,
			// or WindowServer hangs exactly as before. Inside the gates already (checkPowerWork), so no wait.
			if (!pagingState) { fPmFallbacks++; fellBack = true; (void)familyWake("power state left sleep, handleEvent did not act", -1); }
		}
		if (post) {
			if (family) handleEvent(post);
			if (post == kIOFBNotifyDidPowerOn) blank(false);
		}
		FBLOG("power: %u -> %u (%s)%s", from, to, family ? "IOGraphics' power work" : "before WindowServer's open, acknowledged here",
		      fellBack ? " -- handleEvent did not set pagingState: DidWake fallback used" : "");
	}
	// IOGraphics' notifications to this framebuffer, counted (diagnostics, and how many DidWake(false) a recovery owes:
	// IOGraphicsAccelerator2 counts WillSleep(false) as a mode change in progress until a DidWake(false), NVRMFB 2026)
	static IOReturn pmTap(OSObject *self, void *, IOFramebuffer *, IOIndex event, void *info) {
		IntelXeFramebuffer *me = OSDynamicCast(IntelXeFramebuffer, self);
		if (!me) return kIOReturnSuccess;
		const int i = info ? 1 : 0;
		switch (event) {
		case kIOFBNotifyWillSleep:    OSIncrementAtomic(&me->fEvWillSleep[i]); break;
		case kIOFBNotifyDidWake:      OSIncrementAtomic(&me->fEvDidWake[i]); break;
		case kIOFBNotifyDidPowerOn:   OSIncrementAtomic(&me->fEvDidPowerOn); break;
		case kIOFBNotifyWillPowerOff: OSIncrementAtomic(&me->fEvWillPowerOff); break;
		default: break;
		}
		return kIOReturnSuccess;
	}
	// fn inside IOGraphics' system gate, then this controller's gate (IOGraphics' lock order, the context systemWork
	// delivers in). attemptAction never blocks on a gate someone holds: up to 2 s of retries, then kIOReturnBusy.
	// Re-entrant: inside IOGraphics' power work both gates are this thread's already.
	typedef void (*GatedFn)(IntelXeFramebuffer *, intptr_t);
	static IOReturn gatedFc(OSObject *owner, void *fn, void *arg, void *, void *) {
		((GatedFn)fn)((IntelXeFramebuffer *)owner, (intptr_t)arg);
		return kIOReturnSuccess;
	}
	static IOReturn gatedSys(OSObject *owner, void *fn, void *arg, void *, void *) {
		IntelXeFramebuffer *me = (IntelXeFramebuffer *)owner;
		return me->fFcGate->attemptAction(&IntelXeFramebuffer::gatedFc, fn, arg);
	}
	IOReturn withGates(GatedFn fn, intptr_t arg, const char *why) {
		if (!fSysGate || !fFcGate) return kIOReturnNotReady;
		IOReturn r = kIOReturnCannotLock;
		for (int t = 0; t < 200; t++) {
			r = fSysGate->attemptAction(&IntelXeFramebuffer::gatedSys, (void *)fn, (void *)arg);
			if (r != kIOReturnCannotLock) break;
			IOSleep(10);
		}
		if (r == kIOReturnCannotLock) { FBLOG("pm: %s: IOGraphics' gates stayed taken for 2 s, nothing done", why); r = kIOReturnBusy; }
		return r;
	}
	// What handleEvent(kIOFBNotifyDidWake, true) does on the power thread, for when it was not done: the clients'
	// DidWake(true), DidWake(false) as many times as IOGraphics' WillSleep(false) is unanswered, DidPowerOn if the
	// framebuffer is on, pagingState, and the wakeup of WindowServer's _extEntry waiters (FBWL(this)->wakeupGate, the
	// IOLockWakeup on this controller's gate mutex WindowServer sleeps on). Not restoreFramebuffer/updateVBL
	// (IOFramebuffer-private; no VRAM save here, the VBL is this driver's). sleepsSeen >= 0: only if no new system sleep
	// began since (IOGraphics' WillSleep(true) count unchanged; it delivers and clears pagingState inside the system gate).
	struct WakeReq { const char *why; SInt32 sleepsSeen; IOReturn result; };
	static void familyWakeGated(IntelXeFramebuffer *me, intptr_t arg) {
		WakeReq *q = (WakeReq *)arg;
		if (me->pagingState) { q->result = kIOReturnNoResources; return; }   // nothing waits
		if (me->fPowerOrd == 0) { q->result = kIOReturnNotReady; return; }    // asleep: IOGraphics' own wake will come
		if (q->sleepsSeen >= 0 && me->fEvWillSleep[1] != q->sleepsSeen) { q->result = kIOReturnAborted; return; }
		SInt32 owed = me->fEvWillSleep[0] - me->fEvDidWake[0];
		if (owed < 0) owed = 0; if (owed > 16) owed = 16;
		(void)me->deliverFramebufferNotification(kIOFBNotifyDidWake, (void *) true);
		for (SInt32 i = 0; i < owed; i++) (void)me->deliverFramebufferNotification(kIOFBNotifyDidWake, (void *) false);
		if (me->fPowerOrd == 2) (void)me->deliverFramebufferNotification(kIOFBNotifyDidPowerOn, (void *) false);
		me->pagingState = true;
		me->fFcGate->commandWakeup(&me->fServerConnect, false);
		FBLOG("pm: %s: clients told DidWake(true) and %d x DidWake(false)%s, paging on, WindowServer's waiters woken",
		      q->why, (int)owed, me->fPowerOrd == 2 ? ", DidPowerOn" : "");
		q->result = kIOReturnSuccess;
	}
	IOReturn familyWake(const char *why, SInt32 sleepsSeen) {
		WakeReq q = { why, sleepsSeen, kIOReturnError };
		IOReturn r = withGates(&IntelXeFramebuffer::familyWakeGated, (intptr_t)&q, why);
		return r ? r : q.result;
	}
	// After IntelXe's resume: IOGraphics' wake has had 5 s; if pagingState is still off while the framebuffer is powered,
	// WindowServer is (or will be) stuck in _extEntry -- restore it instead of a watchdog panic. Never during the next
	// sleep: armed with IOGraphics' WillSleep(true) count, cancelled by IntelXe's will-suspend.
	static void pmGuardFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->pmGuard(); }
	void pmGuardArm() {
		if (!fPmGuardCall) return;
		fPmGuardSleeps = fEvWillSleep[1];
		uint64_t d; clock_interval_to_deadline(5000, kMillisecondScale, &d);
		thread_call_enter_delayed(fPmGuardCall, d);
	}
	void pmGuard() {
		if (fStopping || !gPmGuard || !opened || pagingState || fPowerOrd == 0) return;
		const IOReturn r = familyWake("guard: pagingState still off 5 s after the resume", fPmGuardSleeps);
		if (r == kIOReturnSuccess) fPmGuardHits++;
		FBLOG("pm guard: 0x%x%s", r, r == kIOReturnSuccess ? " -- IOGraphics' wake did not reach this framebuffer (report this)" : "");
	}
	// debug.intelxefb_pm=2: IOGraphics' system-sleep step for this framebuffer, then a power cycle through 0 driven by
	// IOGraphics' own power work, without a system sleep (pmTest).
	static void pmTestFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->pmTestRun(); }
	static void sleepStepGated(IntelXeFramebuffer *me, intptr_t arg) {
		int *ok = (int *)arg;
		if (!me->pagingState || me->fPowerOrd != 1) { *ok = 0; return; }
		for (uintptr_t a = 0; a < 2; a++) (void)me->deliverFramebufferNotification(kIOFBNotifyWillSleep, (void *) a);
		me->pagingState = false;   // as IOFramebuffer::systemWork at system sleep (:6913)
		*ok = 1;
	}
	void pmTestRun();
public:
	int pmCommand(int cmd);
	int pmInfo(char *b, int n);
	int pagingOn() const { return pagingState ? 1 : 0; }
	// debug.intelxefb_hpd: 1 = a short pulse on the panel's pin, 2 = a long one, handled as if the sink had sent it
	// (intel_hpd_trigger_irq); the interrupt itself is not involved. debug.intelxefb_hpdinfo: counters and the last result.
	int hpdTrigger(int kind) {
		if (!fHpdPin || !fHpdCall || (kind != 1 && kind != 2)) return EINVAL;
		OSBitOrAtomic(fHpdPin, kind == 2 ? &fHpdLong : &fHpdShort);
		thread_call_enter(fHpdCall);
		return 0;
	}
	int hpdInfo(char *b, int n) {
		return snprintf(b, (size_t)n, "panel pin 0x%08x | interrupt %s (irq %d), storm-masked 0x%08x | interrupts %u: long %u short %u, "
		                "storms %u | retrains %u failed %u (streak %u), sink IRQ vectors acked %u | SDEISR 0x%08x SDEIMR 0x%08x SDEIER 0x%08x "
		                "SHOTPLUG_CTL_TC 0x%08x DDI 0x%08x SHPD_FILTER_CNT 0x%x (firmware 0x%x) | last: %s",
		                fHpdPin, hpdPolling() ? "polled" : "driven", (int)fIrqOn, fHpdStormMask, fIrqHpd, fHpdLongs, fHpdShorts,
		                fHpdStorms, fHpdRetrains, fHpdRetrainFails, fHpdFailStreak, fHpdEsi, fRegs ? rd(SDEISR) : 0,
		                fRegs ? rd(SDEIMR) : 0, fRegs ? rd(SDEIER) : 0, fRegs ? rd(SHOTPLUG_CTL_TC) : 0,
		                fRegs ? rd(SHOTPLUG_CTL_DDI) : 0, fRegs ? rd(SHPD_FILTER_CNT) : 0, fShpdFilterBoot, fHpdLast);
	}
	IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
		if (attribute == kIOWindowServerActiveAttribute) { if (value) fWsActive = true; return kIOReturnSuccess; }
		if (attribute == kIOPowerAttribute || attribute == kIODriverPowerAttribute) {
			// From IOFramebuffer::checkPowerWork (kIOPowerStateAttribute) on IOGraphics' system work loop thread, or the
			// mux path's asyncWork (kIODriverPowerAttribute, controller work loop). Never IOFramebuffer::setAttribute
			// here: for kIOPowerAttribute that is setAttributeExt's MUX power change (:12943: pendingPowerState,
			// fPendingMuxPowerChange, a second power pass), which v2..v10d invoked whenever this was called.
			IOWorkLoop *sys = getGraphicsSystemWorkLoop(), *fc = getControllerWorkLoop();
			if (!(sys && sys->onThread()) && !(fc && fc->onThread())) {
				fPmOffThread++;
				FBLOG("power: attribute '%c%c%c%c' = %lu not from IOGraphics' power work (applied anyway)",
				      (char)(attribute >> 24), (char)(attribute >> 16), (char)(attribute >> 8), (char)attribute, (unsigned long)value);
			}
			powerApply((uint32_t)value, true);
			return kIOReturnSuccess;
		}
		return IOFramebuffer::setAttribute(attribute, value);
	}
	IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
		if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
		if (attribute == kIOHardwareCursorAttribute) {
			if (value) *value = (fCurReady && gHwCursor) ? 1 : 0;
			return kIOReturnSuccess;
		}
		return IOFramebuffer::getAttribute(attribute, value);
	}
	IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
		switch (attribute) {
		case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
		case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
		case kConnectionDisplayFlags: case kConnectionFlags:
			return kIOReturnSuccess;
		default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
		}
	}
	IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
		if (idx != 0) return kIOReturnBadArgument;
		// IODisplay::readFramebufferEDID asks this one with a NULL value: success alone means "use getDDCBlock"
		if (attr == kConnectionSupportsHLDDCSense) return fEdidLen ? kIOReturnSuccess : kIOReturnUnsupported;
		if (!value) return kIOReturnBadArgument;
		switch (attr) {
		case kConnectionEnable: case kConnectionCheckEnable: *value = 1; return kIOReturnSuccess;
		case kConnectionFlags: *value = 0; return kIOReturnSuccess;   // an external monitor, not a built-in panel
		case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
		case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
			*value = 0x00000002; return kIOReturnSuccess;
		default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
		}
	}
	bool hasDDCConnect(IOIndex idx) override { return idx == 0 && fEdidLen; }
	IOReturn getDDCBlock(IOIndex idx, UInt32 block, IOSelect type, IOOptionBits, UInt8 *data, IOByteCount *length) override {
		if (idx != 0 || type != kIODDCBlockTypeEDID || !data || !length || block < 1 || block * 128 > (fEdidAug ? fEdidOutLen : fEdidLen)) return kIOReturnUnsupported;
		IOByteCount n = *length < 128 ? *length : 128;
		bcopy((fEdidAug ? fEdidOut : fEdid) + (block - 1) * 128, data, n); *length = n;
		return kIOReturnSuccess;
	}
	// NMXAccel's display pipe: the CPU mapping (frames are copied into it) and the surface's physical address. Unlike
	// NMIntelFB's answer (phys 0: another GPU's panel), the surface is this GPU's own VRAM behind BAR2, and the family
	// marks a display pipe active only in init_framebuffer_resource, which nullmoth runs only with a physical address:
	// with phys 0 WindowServer composited nothing at all (IOAccelDisplayPipe +0x298 stayed 0, 2026-10-28).
	// intelxefbRunGated: the system gate is held here; the controller gate next, then the caller's function
	static IOReturn runInSysGate(OSObject *owner, void *fn, void *arg, void *fc, void *) {
		return ((IOWorkLoop *)fc)->runAction(&IntelXeFramebuffer::runInFcGate, owner, fn, arg);
	}
	static IOReturn runInFcGate(OSObject *, void *fn, void *arg, void *, void *) {
		((void (*)(void *))fn)(arg);
		return kIOReturnSuccess;
	}
	IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
		if (fn && fn->isEqualTo("nvScanoutInfo")) {
			NVRMVramRequest *r = (NVRMVramRequest *)p1;
			if (!r || r->version != 2) return kIOReturnBadArgument;
			IOMemoryMap *hm = homeMap();
			if (!hm) return kIOReturnNotReady;
			fScanouts++;   // asked once per copied frame
			r->kva = (void *)hm->getVirtualAddress();
			r->phys = homePhys(); r->actualSize = (unsigned long long)fModeRB * fModeH;
			r->width = fModeW; r->height = fModeH; r->pitch = fModeRB;
			return kIOReturnSuccess;
		}
		// nullmoth's "VRAM" for WindowServer's surfaces (NVVidMemory::allocPhysical). Nothing on the B580 reads these:
		// the plugin renders through its own Vulkan memory and the display pipe copies frames with the CPU, so wired,
		// physically contiguous system memory stands in (the accelerator describes it with one physical range). Without
		// it the surfaces had no CPU mapping and WindowServer's full-screen surface never prepared.
		if (fn && fn->isEqualTo("nvAllocVram")) {
			NVRMVramRequest *r = (NVRMVramRequest *)p1;
			if (!r || r->version != 2 || !r->size) return kIOReturnBadArgument;
			mach_vm_size_t len = (r->size + 0xFFFF) & ~0xFFFFull;
			IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
				kIODirectionInOut | kIOMemoryPhysicallyContiguous, len, 0x0000FFFFFFFFF000ull);
			if (!b) { FBLOG("nvAllocVram: %llu bytes: no contiguous memory", (unsigned long long)len); return kIOReturnNoMemory; }
			b->prepare();
			bzero(b->getBytesNoCopy(), (size_t)len);
			r->handle = b; r->kva = b->getBytesNoCopy(); r->phys = b->getPhysicalSegment(0, nullptr, kIOMemoryMapperNone);
			r->actualSize = len; r->allocationCookie = 0;
			r->mappedTotal = (unsigned long long)OSAddAtomic64((SInt64)len, &fVramBytes) + len;
			return kIOReturnSuccess;
		}
		// Stage 4: NMXAccel's display pipe hands over the pipe gamma table a transaction carried (CoreDisplay's EOTF /
		// system gamma, applied here instead of in its GPU pass). Only offered when IntelXeFBPipeGammaSize is published.
		if (fn && fn->isEqualTo("nmxPipeGamma")) {
			NMXPipeGamma *g = (NMXPipeGamma *)p1;
			if (!g || g->version != 1 || (g->count != kGammaSize && g->count != 0)) return kIOReturnBadArgument;
			if (!fGammaAllowed || fPipe < 0 || !fRegs) return kIOReturnUnsupported;
			dispLock();
			if (g->count) { bcopy(g->lut, fGammaLut, sizeof fGammaLut); fGammaOn = true; } else fGammaOn = false;
			loadGammaLocked();
			dispUnlock();
			return kIOReturnSuccess;
		}
		// NMXAccel registers where VRR frame time stamps go (fn == nullptr unregisters); called from the VBL timer thread
		if (fn && fn->isEqualTo("nmxVrrStamp")) {
			struct Reg { unsigned version, pad; void (*fn)(void *, const uint64_t *, unsigned); void *ctx; };
			Reg *g = (Reg *)p1;
			if (!g || g->version != 1) return kIOReturnBadArgument;
			if (fFlipLock) IOSimpleLockLock(fFlipLock);
			fStampCtx = g->ctx; fStampFn = g->fn;
			if (fFlipLock) IOSimpleLockUnlock(fFlipLock);
			FBLOG("vrr: time stamps %s", g->fn ? "registered" : "unregistered");
			return kIOReturnSuccess;
		}
		if (fn && fn->isEqualTo("nmxScanout")) {
			NMXScanout *r = (NMXScanout *)p1;
			if (!r || r->version < 1 || r->version > 3) return kIOReturnBadArgument;
			fScanouts++;
			return scanout(r);
		}
		// NMXAccel armed the display pipe at boot (its auto-arm): the guard's accelerated stage begins
		if (fn && fn->isEqualTo("intelxefbGuardAccel")) return guardAccel();
		// NMXAccel runs its pipe registration and mode-change delivery here (p1 = void (*)(void *), p2 = its argument):
		// inside IOGraphics' system gate, then this controller's gate, IOGraphics' own lock order (SYS before the
		// controllers, IOGraphicsWorkLoop::closeGate), the context IOGraphics delivers display-mode notifications to the
		// accelerator in. Called bare, IOAccelLegacyDisplayMachine::display_mode_did_change held the accelerator and
		// waited for the controller gate while AppleGPUWrangler held that gate (IOFramebuffer::systemWork) and waited in
		// IOGraphicsAccelerator2::requestProbe for the accelerator: boot 12:48, WindowServer stuck in IOKitWaitQuiet.
		if (fn && fn->isEqualTo("intelxefbRunGated")) {
			if (!p1) return kIOReturnBadArgument;
			IOWorkLoop *sys = getGraphicsSystemWorkLoop(), *fc = getControllerWorkLoop();
			if (!sys || !fc) return kIOReturnNotReady;
			return sys->runAction(&IntelXeFramebuffer::runInSysGate, this, p1, p2, fc);
		}
		if (fn && fn->isEqualTo("nvFreeVram")) {
			NVRMVramRequest *r = (NVRMVramRequest *)p1;
			if (!r || r->version != 2 || !r->handle) return kIOReturnBadArgument;
			IOBufferMemoryDescriptor *b = (IOBufferMemoryDescriptor *)r->handle;
			OSAddAtomic64(-(SInt64)b->getLength(), &fVramBytes);
			b->complete(); b->release();
			return kIOReturnSuccess;
		}
		return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
	}
private:
	// ---- second display pipe (docs/design/DESIGN-second-pipe.md), behind -intelxefb_pipeb / NVRAM intelxefb-pipeb=1,
	// default off. Off, none of this runs: the dry run (debug.intelxefb_pipeb_plan, pipeBPlanText) only reads registers.
	bool fPipeBAllowed = false;
	// A connector found by connScan: every BMG port with its SDE hotplug bit live is asked for its DPCD caps and EDID over
	// its own AUX channel (the panel's port excepted)
	struct Conn {
		int port;                      // enum port
		bool panel, live, dp;          // the panel's port; SDE hotplug live; a DP sink answered (sink count > 0)
		uint8_t dpcd[16];              // receiver caps 0x000-0x00F (the extended ones at 0x2200 if offered)
		uint8_t sinkCount;             // SINK_COUNT of a branch device (0xFF: not a branch)
		uint32_t edidLen;
		uint8_t edid[kEdidMax];
		bool modeOk;                   // a valid EDID mode for the second pipe
		intelxeplan::Timing mode;
		char name[14];                 // the EDID monitor name
	};
	Conn fConn[intelxeplan::kNumPorts] = {};
	bool fConnScanned = false;
	int fPipeB = -1;                   // the second pipe while it is lit
	int fPipeBConn = -1;               // the fConn entry it drives
	int fPipeBResult = 0;              // the last enable result (debug.intelxefb_pipeb)
	bool fPipeBWanted = false;         // light it again after a system sleep
	uint32_t fPipeBPw = 0;             // the power well request this driver added (HSW_PWR_WELL_CTL2 bit), 0 = none
	IntelXeLink::NewOutput fPipeBOut = {};
	// the global registers before the second pipe came up, put back when it goes
	struct PipeBSaved { uint32_t pmd0, pmd1, mbusCtl, dboxA, dbufCtl[intelxeplan::kNumSlices], aux; bool valid; } fPipeBSaved = {};
	// its surface: a zeroed display buffer from IntelXe.kext, at a GGTT range IntelXe.kext hands out
	void *fPipeBBo = nullptr; uint64_t fPipeBDpa = 0, fPipeBSize = 0; uint32_t fPipeBGgtt = 0; IOMemoryMap *fPipeBMap = nullptr;
	thread_call_t fPipeBCall = nullptr;
	IntelXeFramebufferPipe *fSecond = nullptr;
	static void pipeBFire(thread_call_param_t p0, thread_call_param_t) { ((IntelXeFramebuffer *)p0)->pipeBWork(); }
	void pipeBWork();
	void connScan();
	bool connMode(Conn &c, intelxeedid::Result *res);
	void planLive(intelxeplan::LiveState *s, int pipeB) const;
	bool readEdidInto(uint32_t ctl, uint8_t *edid, uint32_t *edidLen);
	bool pipeBSurface(uint64_t bytes);
	void pipeBSurfaceFree();
	bool pmdemandProgram(uint32_t r0, uint32_t r1, const char *why);
	void dbufSlicesSet(uint8_t mask);
	bool powerWellOn(int idx);
	void pipeBGlobalsBack(int pipeB);
	int pipeBOn(const char *why);
	int pipeBOff(const char *why, bool tellFb = true);
	void pipeBPublish(const Conn &c);
	void pipeBPublishOffline();
	void pipeBDisconnected(const char *why);
	void pipeBStopSecond();
	IOLock *fPipeBLock = nullptr;      // connScan / pipeBOn / pipeBOff: the hotplug work and the sysctl, one at a time
	struct AuxCtx { IntelXeFramebuffer *fb; uint32_t ctl; };
	static int dpcdAtThunk(void *ctx, uint32_t addr, uint8_t *buf, uint32_t len, bool write) {
		const AuxCtx *a = (const AuxCtx *)ctx;
		return a->fb->dpcdAt(a->ctl, addr, buf, len, write);
	}
public:
	int dpcdAt(uint32_t ctl, uint32_t addr, uint8_t *buf, uint32_t len, bool write);
	int pipeBPlanText(char *b, int n);
	int pipeBState() const { return fPipeB >= 0 ? 1 : -fPipeBResult; }
	int pipeBCommand(int v);
	void pipeBBlank(bool off);         // the second framebuffer's display sleep: plane 1 of the second pipe off/on
	uint32_t pipeBFrame() const { return fPipeB >= 0 && fRegs ? rd(FRMCOUNT(fPipeB)) : 0; }
	IOReturn connectFlags(IOIndex, IODisplayModeID mode, IOOptionBits *flags) override {
		if (flags) *flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (mode == 1 ? kDisplayModeDefaultFlag : 0);
		return kIOReturnSuccess;
	}
};

OSDefineMetaClassAndStructors(IntelXeFramebuffer, IOFramebuffer)

// debug.intelxefb_thumb: a 320x180 XRGB downsample of what the pipe is scanning out, read through the framebuffer's own
// CPU mapping, so the panel can be checked from a remote shell (screencapture shows WindowServer's view, not the panel).
static IntelXeFramebuffer *gThumbFB;
// Every debug.intelxefb_* handler is root-only, reads included: thumb/crop return what is on the screen (a TCC Screen
// Recording bypass otherwise), peek/dpcd/vrrlog/linklog expose hardware state. Writes need root anyway (sysctl(3)
// refuses writes without CTLFLAG_ANYBODY); the plain tunables (SYSCTL_INT/UINT) stay world-readable.
static bool rootOnly() { return kauth_cred_issuser(kauth_cred_get()); }
static int intelxefb_thumb SYSCTL_HANDLER_ARGS;
static uint32_t gCropXY;   // x << 16 | y of debug.intelxefb_crop's window
static int intelxefb_crop SYSCTL_HANDLER_ARGS;
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_cropxy, CTLFLAG_RW | CTLFLAG_LOCKED, &gCropXY, 0, "x << 16 | y of the debug.intelxefb_crop window");
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_crop, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_crop, "S",
            "256x128 XRGB8888 window of the live scan-out surface at debug.intelxefb_cropxy, full resolution");
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_thumb, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_thumb, "S",
            "320x180 XRGB8888 downsample of the scan-out surface");

// debug.intelxefb_peek: write an MMIO offset, read back the register there (display state for remote debugging).
static uint32_t gPeekOff = 0x70180;   // PLANE_CTL(A)
static int intelxefb_peek SYSCTL_HANDLER_ARGS;
// debug.intelxefb_poke: write a value to the display register debug.intelxefb_peek last selected (display engine range only)
static int intelxefb_poke SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_poke, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_poke, "IU",
            "write: value for the display register at debug.intelxefb_peek");
// debug.intelxefb_dpcd: write an address to read the sink's DPCD byte there; write 0x40000000 | addr << 8 | byte to write it
static uint32_t gDpcdAddr = 0;
static int intelxefb_dpcd SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_dpcd, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_dpcd, "I",
            "write: DPCD address to read (or 0x40000000|addr<<8|byte to write); read: the byte, -1 on AUX failure");
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_peek, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_peek, "IU",
            "write: MMIO offset to read; read: the register at that offset");

static int intelxefb_mode SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_mode, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_mode, "I",
            "current display mode; write a mode id to program its plane and scaler (test)");

// debug.intelxefb_link: display link actions (fb/IntelXeLink.cpp), only ever started by a write here. 0 = read-only dump of
// the output (registers, DPCD, C20 PLL context and levels over the PHY message bus), 1 = retrain the link at its rate
// with the PLL kept, 2 = full modeset with Linux's C20 table, 3 = full modeset with the C20 context read back from the
// PHY. Read: the last result (0 success, negative = the failed step, 1 = never run). debug.intelxefb_linklog: its log.
static int gLinkResult = 1;
static volatile UInt32 gLinkBusy = 0;
static int intelxefb_link SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_link, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_link, "I",
            "write 0 dump, 1 retrain, 2 full modeset (table), 3 full modeset (PHY readback), 4 power down (wake: 2), 5 save state, 6 modeset from the saved state; read: last result");
static int intelxefb_linklog SYSCTL_HANDLER_ARGS;
// DP Adaptive-Sync (VRR). debug.intelxefb_vrr: 1 = on, 0 = off (fixed refresh exactly as before); EPERM with -intelxefb_novrr,
// ENOTSUP when the monitor/port is not capable, EIO when enabling failed (state stays off). Recovery from a dark panel:
// sysctl debug.intelxefb_vrr=0 then debug.intelxefb_link=1 (retrain).
static int intelxefb_vrr SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_vrr, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_vrr, "I",
            "DP Adaptive-Sync: 1 force on, 0 force off, -1 follow the display mode ('Variable' refresh in Displays); read: on/off now");
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_vrr_vbl, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrVbl, 0,
            "VRR: VBL callbacks 0 = per panel frame, 1 = fixed tick, 2 = per frame + tick when idle");
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_vrr_tick_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrTickUs, 0,
            "VRR: tick of VBL modes 1/2 in us, 0 = the panel's minimum frame");
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_vrr_pace_lo_pct, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrPaceLo, 0,
            "VRR cadence pacer: frames no shorter than the content's cadence minus this % (0 = pacer off)");
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_vrr_pace_hi_pct, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrPaceHi, 0,
            "VRR cadence pacer: frames no longer than the cadence plus this %");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_vrr_lfc, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrLfc, 0,
            "VRR: 1 = repeat frames below the panel's minimum rate (LFC)");
SYSCTL_UINT(_debug, OID_AUTO, intelxefb_vrr_floor_hz, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrFloorHz, 0,
            "VRR: lowest refresh in Hz (0 = the panel's minimum; a higher floor avoids VA-panel flicker at idle); applied when VRR is switched on");
static int intelxefb_irq SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_irq, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_irq, "I",
            "1 = vblank/flip-done/hotplug by display interrupt through IntelXe.kext, 0 = polling timer (-intelxefb_noirq at boot)");
static int intelxefb_vrr_modes SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_vrr_modes, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_vrr_modes, "I",
            "1 = publish IOFBTimingRange (adaptive sync) for CoreDisplay's 'Variable' refresh modes; takes effect when WindowServer starts");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_hwcursor, CTLFLAG_RW | CTLFLAG_LOCKED, &gHwCursor, 0,
            "1 = hardware cursor plane (0: declined at the next cursor image, WindowServer draws it; -intelxefb_nohwcursor: never)");
static int intelxefb_pm SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_pm, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_pm, "I",
            "read: 1 = pagingState on; write 1 = restore a WindowServer stuck after a wake (paging off, framebuffer powered), "
            "2 = power-path test while the display sleeps (result in debug.intelxefb_pminfo)");
static int intelxefb_pminfo SYSCTL_HANDLER_ARGS;
static int intelxefb_hpd SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_hpd, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_hpd, "I",
            "write 1 = handle a short HPD pulse on the panel's port now, 2 = a long one (as if the sink sent it; result in debug.intelxefb_hpdinfo)");
static int intelxefb_hpdinfo SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_hpdinfo, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_hpdinfo, "A",
            "hotplug: panel pin, interrupt/polling, pulses, storms, retrains, registers, last pulse handled");
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_pminfo, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_pminfo, "A",
            "framebuffer power state, pagingState, IOGraphics' power notifications, recoveries, last test");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_pm_guard, CTLFLAG_RW | CTLFLAG_LOCKED, &gPmGuard, 0,
            "1 = 5 s after a resume, restore pagingState if IOGraphics' wake left it off while the framebuffer is powered");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_dpms, CTLFLAG_RW | CTLFLAG_LOCKED, &gDpms, 0,
            "1 = display sleep turns the output off (sink D3, transcoder/DDI/PLL off) and wakes with a full modeset");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_vbl_precise, CTLFLAG_RW | CTLFLAG_LOCKED, &gVblPrecise, 0,
            "1 = the VBL timer wakes in two legs so timer coalescing cannot delay vblank/flip-done handling by ~0.7 ms");
SYSCTL_INT(_debug, OID_AUTO, intelxefb_vrrlog_on, CTLFLAG_RW | CTLFLAG_LOCKED, &gVrrLogOn, 0,
            "1 = record frames, flips, pushes and VBL callbacks in debug.intelxefb_vrrlog");
static int intelxefb_vrrlog SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_vrrlog, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_vrrlog, "S",
            "VRR event ring: uint32 next index, then 8192 x {u32 us, u16 type, u16 x, u32 a, u32 b}");
static int intelxefb_vrrinfo SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_vrrinfo, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_vrrinfo, "A",
            "VRR state and registers");
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_linklog, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_linklog, "A",
            "log of the last debug.intelxefb_link action");
// The second display pipe (docs/design/DESIGN-second-pipe.md). _mode and _plan are registered always: the plan only
// reads registers (a dry run on the one-monitor machine). debug.intelxefb_pipeb only with -intelxefb_pipeb.
static char gPipeBMode[40] = "";
SYSCTL_STRING(_debug, OID_AUTO, intelxefb_pipeb_mode, CTLFLAG_RW | CTLFLAG_LOCKED, gPipeBMode, sizeof gPipeBMode,
              "the second pipe's mode for debug.intelxefb_pipeb_plan: empty = pipe A's timing, WxH@Hz (CVT-RB), WxH@Hzn "
              "(CVT), edid (the scanned sink's); optional :a / :tc1..:tc4 for the port");
static int intelxefb_pipeb_plan SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_pipeb_plan, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, intelxefb_pipeb_plan, "A",
            "dry run: pipe A recomputed next to its live registers, and what a second pipe would be programmed with");
static int intelxefb_pipeb SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, intelxefb_pipeb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, nullptr, 0, intelxefb_pipeb, "I",
            "second pipe (-intelxefb_pipeb): read 1 lit, 0 off, < 0 the last error; write 1 scan + light, 0 off, 2 scan");

// Stage 3 of the frame path: scan-out slots and flips for NMXAccel (struct NMXScanout above). Every register change
// is under the display lock, so a flip, a mode change, display sleep and the link actions never interleave; a flip
// arms PLANE_SURF only (PLANE_CTL keeps linear XRGB8888, PLANE_STRIDE stays, since a slot must have the mode's pitch),
// which is Linux's skl_plane_update_arm minus the registers that do not change.
IOReturn IntelXeFramebuffer::scanout(NMXScanout *r)
{
	if (!fRegs || fPipe < 0 || !fFlipLock || fStopping) return kIOReturnNotReady;
	if (fSysSuspended && (r->op == 2 || r->op == 3)) { r->immediate = 1; return kIOReturnSuccess; }   // asleep: nothing to wait for
	r->immediate = 0;
	switch (r->op) {
	case 0: {   // map
		const uint64_t need = (uint64_t)r->pitch * r->height;
		// linear surfaces: 64-byte stride units, 4 KB-aligned base (tgl_plane_min_alignment for linear on Xe2)
		const bool runs = r->version >= 3 && r->runs && r->nRuns;
		if (!r->width || !r->height || (r->pitch & 63) || r->pitch < r->width * 4 || r->size < need || r->size > kSlotSpan ||
		    (!runs && (r->dpa & 0xFFF)) || (runs && r->nRuns > 4096))
			return kIOReturnBadArgument;
		NMXDpaRun *copy = nullptr;
		if (runs) {   // validate and keep a copy: 4 KB aligned runs that cover the buffer
			uint64_t total = 0;
			for (uint32_t k = 0; k < r->nRuns; k++) {
				if ((r->runs[k].dpa & 0xFFF) || (r->runs[k].len & 0xFFF) || !r->runs[k].len) return kIOReturnBadArgument;
				total += r->runs[k].len;
			}
			if (total < ((r->size + 0xFFF) & ~0xFFFull)) return kIOReturnBadArgument;
			copy = (NMXDpaRun *)IOMalloc(r->nRuns * sizeof(NMXDpaRun));
			if (!copy) return kIOReturnNoMemory;
			bcopy(r->runs, copy, r->nRuns * sizeof(NMXDpaRun));
		}
		if (!fSlotsOk) { if (copy) IOFree(copy, r->nRuns * sizeof(NMXDpaRun)); return kIOReturnNoResources; }   // the slot window belongs to someone else: frames are copied
		dispLock();
		// next fit: a slot just unmapped is the last to be reused, so no translation the display may still hold for it
		// meets a new buffer (xe invalidates only the GT's GGTT TLBs on a GGTT update, not the display's)
		int i = -1;
		const uint64_t surfEnd = (uint64_t)fSurf + (uint64_t)fRowBytes * fHeight;
		for (uint32_t k = 0; k < (uint32_t)kSlots && i < 0; k++) {
			const uint32_t c = (fSlotNext + k) % (uint32_t)kSlots;
			const uint64_t g0 = kSlotGgtt + (uint64_t)c * kSlotSpan;
			// never over the firmware's surface, nor over PTEs something else wrote (a free slot's first PTE is 0)
			if (fSlot[c].used || (g0 < surfEnd && g0 + kSlotSpan > fSurf)) continue;
			// the firmware leaves unused GGTT entries all ones (read on the B580, 2026-10-28): no real mapping looks so
			// (with the window reserved in IntelXe.kext's allocator it is ours whatever a stale PTE says: after a warm reboot
			// they hold the last boot's mappings, or firmware fill such as 0xfffffff3ffffffff)
			const uint64_t p0 = pte((uint32_t)g0);
			if (fSlotsOwned < 1 && (p0 & 1) && p0 != ~0ull) {
				static uint32_t said;
				if (said++ < 4) FBLOG("scan-out slot %u: GGTT 0x%llx already mapped (pte 0x%llx) -- not used", c, g0, p0);
				continue;
			}
			i = (int)c;
		}
		if (i < 0) { dispUnlock(); if (copy) IOFree(copy, r->nRuns * sizeof(NMXDpaRun)); return kIOReturnNoResources; }
		fSlotNext = (uint32_t)(i + 1) % (uint32_t)kSlots;
		const uint32_t g = kSlotGgtt + (uint32_t)i * kSlotSpan;
		const uint64_t size = (r->size + 0xFFF) & ~0xFFFull;
		Slot &sl = fSlot[i];
		sl.used = true; sl.gen = ++fSlotGen; sl.dpa = runs ? copy[0].dpa : r->dpa; sl.size = size; sl.w = r->width; sl.h = r->height; sl.pitch = r->pitch;
		sl.runs = copy; sl.nRuns = runs ? r->nRuns : 0;
		if (sl.nRuns == 1) { IOFree(sl.runs, sizeof(NMXDpaRun)); sl.runs = nullptr; sl.nRuns = 0; }   // one run: contiguous
		// the firmware surface's PTE bits: present, XE_GGTT_PTE_DM (local memory) and its PAT index
		slotPtes(sl, g);
		(void)pte(g + (uint32_t)(size - 0x1000));   // posting read: the PTEs are in before the plane can be armed with them
		// the thumbnail's view (contiguous buffers inside the BAR2 window only; otherwise it shows the firmware surface)
		if (!sl.nRuns && sl.dpa + size <= fBarLen)
			if (IODeviceMemory *m = IODeviceMemory::withRange(fBarBase + sl.dpa, (IOPhysicalLength)size)) { sl.cpu = m->map(kIOMapWriteCombineCache); m->release(); }
		r->ggtt = g; r->gen = sl.gen;
		const uint32_t nr = sl.nRuns ? sl.nRuns : 1;
		dispUnlock();
		static uint32_t logs;
		if (logs++ < 12) FBLOG("scan-out slot %d: GGTT 0x%x -> VRAM 0x%llx (%llu KB in %u run%s) %ux%u pitch %u, PTE bits 0x%llx", i, g, sl.dpa,
		                       size >> 10, nr, nr == 1 ? "" : "s", r->width, r->height, r->pitch, fPteFlags);
		return kIOReturnSuccess;
	}
	case 1: {   // unmap
		dispLock();
		int i = slotOf(r->ggtt, r->gen);
		if (i < 0) { dispUnlock(); return kIOReturnNotFound; }
		bool busy = surfArmed() == r->ggtt || surfLive() == r->ggtt || pendOn(r->ggtt);
		if (busy && !r->force) { dispUnlock(); return kIOReturnBusy; }
		if (busy) {
			// WindowServer went away (or the buffer is freed) while the plane shows it: home first, and the PTEs go only
			// once the plane has left the slot. A stale frame until the next one beats scanning out freed VRAM.
			homeLocked();
			dispUnlock();
			for (int t = 0; t < 100 && surfLive() == r->ggtt; t++) IOSleep(1);
			dispLock();
			i = slotOf(r->ggtt, r->gen);
			if (i < 0) { dispUnlock(); return kIOReturnSuccess; }
			if (surfLive() == r->ggtt) FBLOG("scan-out slot %d still live after 100 ms -- unmapped anyway", i);
		}
		slotClearLocked(i);
		dispUnlock();
		return kIOReturnSuccess;
	}
	case 2:     // flip
	case 3: {   // home
		dispLock();
		uint32_t target = homeGgtt();
		if (r->op == 2) {
			int i = slotOf(r->ggtt, r->gen);
			if (i < 0 || fSlot[i].w != fModeW || fSlot[i].h != fModeH || fSlot[i].pitch != fModeRB) { dispUnlock(); return kIOReturnNotFound; }
			target = r->ggtt;
		}
		if (pendOn(0)) { dispUnlock(); return kIOReturnBusy; }   // NMXAccel keeps one flip in flight
		if (r->op == 3 && surfArmed() == homeGgtt() && surfLive() == homeGgtt()) { dispUnlock(); r->immediate = 1; return kIOReturnSuccess; }
		// Adaptive-Sync window for this frame (macOS "Variable" mode): no shorter than vmin + minExt (TRANS_VRR_FLIPLINE,
		// the earliest decision line), no longer than vmin + maxExt (TRANS_VRR_VMAX), as Apple's Intel driver programs
		// per transaction. Without one the mode's own window applies.
		// Otherwise the cadence pacer (vrrPace) keeps the window it set at this frame's start.
		if (fVrrOn && r->op == 2) {
			fVrrWin = r->version >= 2 && (r->vrrFlags & 1);
			if (fVrrWin) {
				uint32_t fl = fVrrVmin + r->vrrMinExt, vm = fVrrVmin + r->vrrMaxExt;
				if (fl > fVrrVmax) fl = fVrrVmax;
				if (vm > fVrrVmax) vm = fVrrVmax;
				if (vm < fl) vm = fl;
				fVrrReqs++;
				setWindow(fl, vm);
			} else if (!gVrrPaceLo) setWindow(fVrrVmin, fVrrVmax);
		}
		// where the frame stands before the arm: the push below may end it at once, after which FRMTMSTMP is the new one
		const uint32_t now0 = usNow(), since0 = now0 - rd(FRMTMSTMP(fPipe));
		wr(PLANE_SURF(fPipe), target | (rd(PLANE_SURF(fPipe)) & 0xFFFu));
		// VRR: push right behind the arm, so the frame ends at the next decision line (at once when the panel is already
		// waiting past its minimum frame length) and the flip latches there, then wake the timer just after it
		push((uint16_t)r->op);
		evLog(kEvFlip, (uint16_t)r->op, target, rd(PIPEDSL(fPipe)) & 0xFFFFF);
		if (fBlanked || !r->done) {
			r->immediate = 1;   // asleep: the plane is off and no picture is waited for
		} else {
			IOSimpleLockLock(fFlipLock);
			fPend.on = true; fPend.aborted = false; fPend.ggtt = target; fPend.done = r->done; fPend.ctx = r->ctx; fPend.cookie = r->cookie;
			fPend.t0 = mach_absolute_time();
			IOSimpleLockUnlock(fFlipLock);
		}
		if (fVrrOn) {
			if (r->op == 2) lfcOnFlip(now0);
			// the flip latches at the decision line: the minimum frame length after the last start of vblank, or now
			const uint32_t in = (since0 < fVrrFlUs ? fVrrFlUs - since0 : 0) + 60;
			if (!fIrqOn) rearm(absIn(early(in)), false);   // with interrupts the vblank wakes the VBL thread
		}
		dispUnlock();
		return kIOReturnSuccess;
	}
	case 4:
		r->armed = surfArmed(); r->live = surfLive();
		return kIOReturnSuccess;
	}
	return kIOReturnBadArgument;
}

// ---- DP Adaptive-Sync (VRR) ----------------------------------------------------------------------------------------
// Linux 7.2 drivers/gpu/drm/i915/display/intel_vrr.c for display 14 (BMG is 14.01), as it switches VRR on a running pipe
// (fastset): intel_vrr_set_transcoder_timings (fixed vmin = vmax = flipline, VRR_CTL guardband without enable,
// TRANS_VRR_VSYNC, EMP_AS_SDP_TL on 14.01), then intel_vrr_enable (vmin/vmax/flipline, TRANS_PUSH_EN, VRR_CTL enable);
// intel_vrr_disable undoes it (VRR_CTL without enable, wait for VRR_EN_LIVE to clear, PUSH_EN off, fixed timings).
// Each flip is followed by intel_vrr_send_push. The sink's MSA_TIMING_PAR_IGNORE_EN (DPCD 0x107 bit 7) is what Linux
// sets during link training when the mode is in the VRR range; Apple's Intel driver flips it at runtime
// (AppleIntelPort::enableVRR / disableVRR read-modify-write 0x107), which is what happens here.

void IntelXeFramebuffer::vrrProbeCaps()
{
	fVrrCapable = false;
	if (!fEdidParse) { FBLOG("vrr: no EDID, not capable"); return; }
	// drm_edid.c:6504 drm_get_monitor_range (v10m, fb/IntelXeEdid.cpp): EDID 1.4, continuous frequency, a range limits
	// descriptor of the "range limits only" kind; min/max vertical rate with the 1.4 offset flags. v10l read the first
	// range descriptor of any kind and any EDID version.
	const uint32_t lo = (uint32_t)fEdidParse->minVfreq, hi = (uint32_t)fEdidParse->maxVfreq;
	uint8_t dsp = 0xFF, msa = 0;
	const bool ok5 = dpcd(0x005, &dsp, 1, false) == 0, ok7 = dpcd(0x007, &msa, 1, false) == 0;
	fVrrMinHz = lo; fVrrMaxHz = hi;
	// intel_vrr_is_capable: DP, not a branch device, drm_dp_sink_can_do_video_without_timing_msa, range wider than 10 Hz
	fVrrCapable = lo && hi && lo < hi && hi - lo > 10 && ok5 && !(dsp & 1) && ok7 && (msa & 0x40);
	FBLOG("vrr: EDID range %u-%u Hz, DPCD 005 0x%02x 007 0x%02x (MSA ignore %s) -> %s", lo, hi, dsp, msa,
	      (msa & 0x40) ? "supported" : "not supported", fVrrCapable ? "capable" : "not capable");
	if (fVrrCapable) {
		char r[24]; snprintf(r, sizeof r, "%u-%u", lo, hi);
		setProperty("IntelXeFBVRRRange", r);
	}
}

// vmin = the mode's vertical total (intel_vrr_compute_vmin: fixed and variable timings share the guardband), vmax = the
// longest total at the panel's lowest rate (intel_vrr_compute_vmax), guardband = vmin - vdisplay - set context latency
// (the full vblank, intel_vrr_max_vblank_guardband; it keeps the vsync pulse inside the guardband for every mode).
bool IntelXeFramebuffer::vrrCompute(uint32_t vt)
{
	if (!fPclkCur || !fHT || !fVrrMinHz || !vt) return false;
	const uint32_t hz = (uint32_t)(fPclkCur / ((uint64_t)fHT * vt));
	if (hz + 1 < fVrrMinHz || hz > fVrrMaxHz + 1) { FBLOG("vrr: mode at %u Hz is outside the panel's %u-%u Hz", hz, fVrrMinHz, fVrrMaxHz); return false; }
	const uint32_t floorHz = gVrrFloorHz > fVrrMinHz && gVrrFloorHz < hz ? gVrrFloorHz : fVrrMinHz;
	uint64_t vmax = fPclkCur / ((uint64_t)fHT * floorHz);
	// a variable timing from CoreDisplay names its own longest frame; never past the panel's lowest rate
	if (fVrrModeExt && vt + fVrrModeExt < vmax) vmax = vt + fVrrModeExt;
	if (vmax < vt) vmax = vt;
	if (vmax > 0xFFFFF) vmax = 0xFFFFF;
	const uint32_t scl = rd(TRANS_SET_CONTEXT_LATENCY(fPipe)) & 0xFFFF;
	if (vt > 0xFFFF || vt <= fHeight + scl + 1) return false;
	fVrrVmin = vt; fVrrVmax = (uint32_t)vmax;
	fVrrGuard = vt - fHeight - scl;
	if (fVrrGuard > VRR_CTL_GUARDBAND_MASK) fVrrGuard = VRR_CTL_GUARDBAND_MASK;
	fVrrVminUs = (uint32_t)((uint64_t)fHT * fVrrVmin * 1000000ull / fPclkCur);
	fVrrVmaxUs = (uint32_t)((uint64_t)fHT * fVrrVmax * 1000000ull / fPclkCur);
	return true;
}

bool IntelXeFramebuffer::vrrEnableLocked()
{
	if (fVrrOn) return true;
	if (!fVrrCapable || !fVrrAllowed || fPipe < 0 || !fRegs || !fFlipLock) return false;
	const int t = fPipe;
	if (!fVblLocked) { FBLOG("vrr: the pipe's frame timestamps are not live, not enabled"); return false; }
	if (!(rd(TRANS_DDI_FUNC_CTL(t)) & kEnable) || !(rd(TRANSCONF(t)) & kEnable)) { FBLOG("vrr: transcoder not running"); return false; }
	if (rd(TRANS_VRR_CTL(t)) & VRR_CTL_VRR_ENABLE) FBLOG("vrr: VRR_CTL 0x%08x already enabled (firmware?)", rd(TRANS_VRR_CTL(t)));
	if (!vrrCompute(fModeVT)) { FBLOG("vrr: no timings for vertical total %u", fModeVT); return false; }
	uint8_t ds = 0;
	if (dpcd(DP_DOWNSPREAD_CTRL, &ds, 1, false)) { FBLOG("vrr: DPCD 0x107 read failed, not enabled"); return false; }
	if (!fVrrSaved) {
		fVrrSaved = true; fMsaBoot = ds;
		fVrrCtlBoot = rd(TRANS_VRR_CTL(t)); fVrrVsyncBoot = rd(TRANS_VRR_VSYNC(t)); fSdpTlBoot = rd(EMP_AS_SDP_TL(t));
	}
	if (!(ds & DP_MSA_TIMING_PAR_IGNORE_EN)) {
		ds |= DP_MSA_TIMING_PAR_IGNORE_EN;
		if (dpcd(DP_DOWNSPREAD_CTRL, &ds, 1, true)) { FBLOG("vrr: DPCD 0x107 write failed, not enabled"); return false; }
	}
	// intel_vrr_set_transcoder_timings
	const uint32_t vt = fModeVT;
	const uint32_t vsS = vt - fVSS, vsE = vt - fVSE;   // lines from the vsync edges to the end of the frame
	wr(TRANS_VRR_VMIN(t), vt - 1); wr(TRANS_VRR_VMAX(t), vt - 1); wr(TRANS_VRR_FLIPLINE(t), vt - 1);
	wr(TRANS_VRR_CTL(t), VRR_CTL_FLIP_LINE_EN | fVrrGuard);
	wr(TRANS_VRR_VSYNC(t), (vsE & 0x1FFF) << 16 | (vsS & 0x1FFF));
	wr(EMP_AS_SDP_TL(t), vsS & 0x1FFF);
	// intel_vrr_enable -> intel_vrr_set_vrr_timings + intel_vrr_tg_enable
	wr(TRANS_VRR_VMIN(t), fVrrVmin - 1); wr(TRANS_VRR_VMAX(t), fVrrVmax - 1); wr(TRANS_VRR_FLIPLINE(t), fVrrVmin - 1);
	IOSimpleLockLock(fFlipLock);
	wr(TRANS_PUSH(t), TRANS_PUSH_EN);
	wr(TRANS_VRR_CTL(t), VRR_CTL_VRR_ENABLE | VRR_CTL_FLIP_LINE_EN | fVrrGuard);
	fLastVblUs = rd(FRMTMSTMP(t)); fLastCbUs = usNow(); fLfcLeft = 0; fLastFlipUs = 0; fFlipIvUs = 0;
	fVrrFl = fVrrVmin; fVrrVm = fVrrVmax; fVrrFlUs = fVrrVminUs; fVrrVmUs = fVrrVmaxUs;
	fVrrOn = true;
	IOSimpleLockUnlock(fFlipLock);
	int ms = 0;
	for (; ms < 100 && !(rd(TRANS_VRR_STATUS(t)) & VRR_STATUS_VRR_EN_LIVE); ms++) IOSleep(1);
	const uint32_t st = rd(TRANS_VRR_STATUS(t));
	FBLOG("vrr: ON, vmin %u vmax %u flipline %u guardband %u (%u-%u us per frame), DPCD 0x107 0x%02x, status 0x%08x after %d ms",
	      fVrrVmin, fVrrVmax, fVrrVmin, fVrrGuard, fVrrVminUs, fVrrVmaxUs, ds, st, ms);
	evLog(kEvState, 1, fVrrVmin, fVrrVmax);
	setProperty("intelxefb_vrr", kOSBooleanTrue);
	push(0);   // whatever is armed goes out now
	rearm(absIn(200), false);
	return true;
}

void IntelXeFramebuffer::vrrDisableLocked(const char *why)
{
	if (fPipe < 0 || !fRegs || !fFlipLock) return;
	const int t = fPipe;
	if (!fVrrOn && !(rd(TRANS_VRR_CTL(t)) & VRR_CTL_VRR_ENABLE)) return;
	// intel_vrr_tg_disable
	IOSimpleLockLock(fFlipLock);
	fVrrOn = false;
	fLfcLeft = 0;
	wr(TRANS_VRR_CTL(t), VRR_CTL_FLIP_LINE_EN | fVrrGuard);
	IOSimpleLockUnlock(fFlipLock);
	int ms = 0;
	for (; ms < 250 && (rd(TRANS_VRR_STATUS(t)) & VRR_STATUS_VRR_EN_LIVE); ms++) IOSleep(1);
	if (rd(TRANS_VRR_STATUS(t)) & VRR_STATUS_VRR_EN_LIVE) FBLOG("vrr: VRR live status still set after 250 ms (0x%08x)", rd(TRANS_VRR_STATUS(t)));
	IOSimpleLockLock(fFlipLock);
	wr(TRANS_PUSH(t), rd(TRANS_PUSH(t)) & ~(TRANS_PUSH_EN | TRANS_PUSH_SEND));
	IOSimpleLockUnlock(fFlipLock);
	// intel_vrr_set_fixed_rr_timings, then the transcoder's VRR registers as the firmware left them: the legacy timing
	// generator scans out exactly as before VRR was first switched on
	wr(TRANS_VRR_VMIN(t), fModeVT - 1); wr(TRANS_VRR_VMAX(t), fModeVT - 1); wr(TRANS_VRR_FLIPLINE(t), fModeVT - 1);
	wr(TRANS_VRR_CTL(t), fVrrCtlBoot);
	wr(TRANS_VRR_VSYNC(t), fVrrVsyncBoot);
	wr(EMP_AS_SDP_TL(t), fSdpTlBoot);
	uint8_t ds = 0;
	if (!dpcd(DP_DOWNSPREAD_CTRL, &ds, 1, false) && (ds & DP_MSA_TIMING_PAR_IGNORE_EN) && !(fMsaBoot & DP_MSA_TIMING_PAR_IGNORE_EN)) {
		ds &= (uint8_t)~DP_MSA_TIMING_PAR_IGNORE_EN;
		(void)dpcd(DP_DOWNSPREAD_CTRL, &ds, 1, true);
	}
	FBLOG("vrr: OFF (%s), live cleared after %d ms, %u pushes, %u LFC repeats", why, ms, fVrrPushes, fLfcPushes);
	evLog(kEvState, 0, 0, 0);
	setProperty("intelxefb_vrr", kOSBooleanFalse);
	rearm(absIn(200), false);
}

int IntelXeFramebuffer::vrrSet(int on)
{
	if (!fRegs || fPipe < 0) return ENODEV;
	if (on == 1 && !fVrrAllowed) return EPERM;
	if (on == 1 && !fVrrCapable) return ENOTSUP;
	dispLock();
	fVrrForce = on;
	applyVrrLocked("sysctl");
	const bool ok = fVrrOn == vrrWanted() || !fVrrCapable || !fVrrAllowed;
	dispUnlock();
	return ok ? 0 : EIO;
}

// LFC (low framerate compensation, AMD's below-the-range): content slower than the panel's lowest rate meets a forced
// vmax refresh, and a frame flipped within a minimum frame length after it waits for that (up to 5.6 ms of judder).
// From a smoothed flip interval (WindowServer's presents of such content still jitter by its own refresh grid, so the
// tolerance is 25 %), the timer pushes repeats so that the last one has passed its minimum length before the earliest
// expected next flip, with no gap longer than vmax: the panel runs at a multiple of the content rate inside its range.
void IntelXeFramebuffer::lfcOnFlip(uint32_t now)
{
	if (!fVrrOn) return;   // v10l: LFC only with Adaptive-Sync on (a fixed mode has neither pushes nor repeats)
	const uint32_t iv = fLastFlipUs ? now - fLastFlipUs : 0;
	fLastFlipUs = now;
	if (!iv || iv > 200000) { fFlipIvUs = 0; fLfcActive = false; fLfcEnter = 0; }   // a pause: start over
	else fFlipIvUs = fFlipIvUs ? (fFlipIvUs * 3 + iv) / 4 : iv;    // EMA of the content's frame interval
	uint32_t left = 0, step = 0;
	const uint32_t est = fFlipIvUs;
	// Hysteresis (amdgpu's below-the-range entry/exit margins): engage after three intervals in a row longer than vmax
	// with the average 5 % past it; leave when the average drops 5 % below vmax or one interval is clearly in range.
	if (!fLfcActive) {
		fLfcEnter = (iv > fVrrVmaxUs && est > fVrrVmaxUs * 105 / 100) ? fLfcEnter + 1 : 0;
		if (fLfcEnter >= 3) fLfcActive = true;
	} else if (est < fVrrVmaxUs * 95 / 100 || iv < fVrrVmaxUs * 80 / 100) { fLfcActive = false; fLfcEnter = 0; }
	if (gVrrLfc && !gVrrPaceLo && fLfcActive && !fVrrWin && est > fVrrVmaxUs && est <= 4 * fVrrVmaxUs && iv * 10 > fVrrVmaxUs * 9) {
		const uint32_t lastRep = est * 3 / 4 - fVrrVminUs - 300;   // ends its minimum frame before est - 25 %
		const uint32_t gap = fVrrVmaxUs * 95 / 100;
		const uint32_t n = (lastRep + gap - 1) / gap;              // repeats, no gap above vmax
		if (n && lastRep / n >= fVrrVminUs + 200) { left = n; step = lastRep / n; }
	}
	IOSimpleLockLock(fFlipLock);   // the timer counts the repeats down under the same lock
	fLfcStepUs = step; fLfcLeft = left; fLfcNextUs = now + step;
	IOSimpleLockUnlock(fFlipLock);
}

// The VBL timer under VRR. Frames end when a push meets a decision line or at vmax, so the timer sleeps until the
// earliest point the next start of vblank can come (FRMTMSTMP + vmin with a push pending, + vmax without), polls briefly
// past it, and is brought forward by every flip (scanout). The VBL clients (IOFramebuffer, the accelerator's display
// pipe that paces WindowServer) are called per debug.intelxefb_vrr_vbl: 0 on every frame the panel shows, 1 at a fixed
// tick, 2 on every frame plus a tick whenever no frame came for a tick.
void IntelXeFramebuffer::vblFireVrr()
{
	const int p = fPipe;
	uint32_t now = usNow();
	const uint32_t frame = rd(FRMCOUNT(p));
	const bool hw = frame != fLastFrame;
	irqWatch(hw);
	if (hw) {
		const uint32_t stamp = rd(FRMTMSTMP(p));
		evLog(kEvFrame, (uint16_t)(frame - fLastFrame), frame, rd(TRANS_VRR_VTOTAL_PREV(p)), stamp);
		fLastFrame = frame; fLastVblUs = stamp; fVrrFrames++;
		vrrPace(stamp);
		if (fVrrModeWants && fStampFn) vrrStamp(stamp);
	}
	if (gVrrLogOn >= 2) evLog(8, hw, rd(PLANE_SURFLIVE(p)), rd(TRANS_PUSH(p)), now);
	const uint32_t tick = gVrrTickUs ? gVrrTickUs : fVrrVminUs;
	const bool due = (int32_t)(now - fLastCbUs) >= (int32_t)tick - 100;
	bool cb = gVrrVbl == 0 ? hw : gVrrVbl == 1 ? due : (hw || due);
	if (cb) {
		if (gVrrVbl == 1) { fLastCbUs += tick; if ((int32_t)(now - fLastCbUs) > (int32_t)tick) fLastCbUs = now; }
		else fLastCbUs = now;
		evLog(kEvVbl, hw ? 1 : 2, 0, 0, now);
		for (int i = 0; i < kVblSlots; i++)
			if (fVbl[i].proc && fVblOn[i]) fVbl[i].proc(fVbl[i].target, fVbl[i].ref);
	}
	if (hpdPolling() && ++fHpdFrames >= 256) { fHpdFrames = 0; pollHpd(); }
	flipCheck();
	now = usNow();
	bool repeat = false;
	uint32_t left = 0;
	IOSimpleLockLock(fFlipLock);
	if (fLfcLeft && (int32_t)(now - fLfcNextUs) >= 0) {
		repeat = !fPend.on && !(rd(TRANS_PUSH(p)) & TRANS_PUSH_SEND);
		left = fLfcLeft--; fLfcNextUs += fLfcStepUs;
	}
	IOSimpleLockUnlock(fFlipLock);
	if (repeat) { push(7); fLfcPushes++; evLog(kEvLfc, (uint16_t)left, fFlipIvUs, fLfcStepUs, now); }
	IOSimpleLockLock(fFlipLock);
	if (fVrrOn && !fStopping && fVblTimer) {
		now = usNow();
		const bool pushed = rd(TRANS_PUSH(p)) & TRANS_PUSH_SEND;
		const uint32_t since = now - rd(FRMTMSTMP(p)), len = pushed ? fVrrFlUs : fVrrVmUs;
		uint32_t in = since < len ? len - since + 60 : (pushed ? 100 : 250);   // past it: the frame is due, poll
		if (fIrqOn) in = (since < len ? len - since : 0) + 1500;   // the vblank interrupt wakes us; this is the fallback
		if (gVrrVbl) { int32_t tin = (int32_t)(fLastCbUs + tick - now); uint32_t u = tin > 50 ? (uint32_t)tin : 50; if (u < in) in = u; }
		if (fLfcLeft) { int32_t tin = (int32_t)(fLfcNextUs - now); uint32_t u = tin > 20 ? (uint32_t)tin : 20; if (u < in) in = u; }
		// a flip in flight whose push has been taken: it latched at that start of vblank, look again shortly
		if (fPend.on && !fIrqOn) { const uint32_t cap = (!pushed && since < 2000) ? 100 : 1000; if (in > cap) in = cap; }
		fWakeAt = absIn(early(in)); fVblNext = fWakeAt;
		thread_call_enter_delayed(fVblTimer, fWakeAt);
		IOSimpleLockUnlock(fFlipLock);
		return;
	}
	IOSimpleLockUnlock(fFlipLock);
	rearm(absIn(1000), true);   // VRR went off meanwhile: the fixed-refresh timer takes over
}

// On IntelXe.kext's work loop thread under its display lock: acknowledge as gen11_display_irq_handler does (master off,
// pipe/SDE/misc/port/HPD IIRs, master on), then wake the VBL thread call. Nothing here blocks.
void IntelXeFramebuffer::irqHandle(uint32_t master)
{
	if (!fRegs || fPipe < 0) return;
	fIrqCalls++;
	const uint32_t ctl = rd(GEN11_DISPLAY_INT_CTL);
	if (!(ctl & ~DISPLAY_IRQ_ENABLE)) return;
	wr(GEN11_DISPLAY_INT_CTL, 0);
	bool kick = false;
	for (int p = 0; p < 4; p++) {
		if (!(ctl & (1u << (16 + p)))) continue;
		const uint32_t iir = rd(DE_PIPE_IIR(p));
		if (!iir) continue;
		wr(DE_PIPE_IIR(p), iir);
		if (p != fPipe) continue;
		if (iir & PIPE_VBLANK) { fIrqVbl++; kick = true; }
		if (iir & PIPE_FIFO_UNDERRUN) fIrqUnderruns++;
	}
	if (ctl & DE_PCH_IRQ) {
		// gen8_read_and_ack_pch_irqs, then icp_irq_handler: SDEIIR acked first; the pins' long/short status latched in
		// SHOTPLUG_CTL_TC/_DDI is read and written back (intel_de_rmw(.., 0, 0)), which clears it
		const uint32_t sde = rd(SDEIIR);
		if (sde) {
			wr(SDEIIR, sde);
			const uint32_t tcTrig = sde & SDE_TC_HOTPLUG_MASK, ddiTrig = sde & SDE_DDI_HOTPLUG_MASK;
			uint32_t longs = 0;
			if (tcTrig) {
				const uint32_t v = rd(SHOTPLUG_CTL_TC);
				wr(SHOTPLUG_CTL_TC, v);
				for (int i = 0; i < 4; i++) if ((tcTrig & (1u << (24 + i))) && (v >> (4 * i)) & HPD_LONG) longs |= 1u << (24 + i);
			}
			if (ddiTrig) {
				const uint32_t v = rd(SHOTPLUG_CTL_DDI);
				wr(SHOTPLUG_CTL_DDI, v);
				for (int i = 0; i < 2; i++) if ((ddiTrig & (1u << (16 + i))) && (v >> (4 * i)) & HPD_LONG) longs |= 1u << (16 + i);
			}
			if (tcTrig | ddiTrig) { fIrqHpd++; hpdIrq(tcTrig | ddiTrig, longs); }
		}
	}
	if (ctl & DE_MISC_IRQ) { const uint32_t v = rd(DE_MISC_IIR); if (v) wr(DE_MISC_IIR, v); }
	if (ctl & DE_PORT_IRQ) { const uint32_t v = rd(DE_PORT_IIR); if (v) wr(DE_PORT_IIR, v); }
	if (ctl & DE_HPD_IRQ)  { const uint32_t v = rd(DE_HPD_IIR);  if (v) wr(DE_HPD_IIR, v); }
	wr(GEN11_DISPLAY_INT_CTL, DISPLAY_IRQ_ENABLE);
	if (kick && fIrqOn && fVblTimer && !fStopping) thread_call_enter(fVblTimer);
}

// ---- hotplug, interrupt driven (v10f) ---------------------------------------------------------------------------------
// intel_hpd_irq_handler on IntelXe's interrupt thread (under its gDispLock, nothing may block): per pin, storm detection
// (intel_hpd_irq_storm_detect: long +10, threshold 50 per second; a stormy pin is masked in SDEIMR and polled until
// hpdReenable, intel_hpd_irq_storm_switch_to_polling / _reenable_work), then the pulses go to hpdWork (dig_port_work).
void IntelXeFramebuffer::hpdIrq(uint32_t pins, uint32_t longs)
{
	const uint64_t now = mach_absolute_time();
	uint64_t period; nanoseconds_to_absolutetime((uint64_t)kHpdStormPeriodMs * 1000000ull, &period);
	uint32_t storm = 0;
	for (int b = 16; b < 28; b++) {
		const uint32_t bit = 1u << b;
		if (!(pins & bit) || !(longs & bit)) continue;   // short pulses are not counted (DP MST capable, as Linux)
		if (now - fHpdStormT0[b] > period) { fHpdStormT0[b] = now; fHpdStormCount[b] = 0; }
		fHpdStormCount[b] += 10;
		if (fHpdStormCount[b] > kHpdStormThreshold) storm |= bit;
	}
	if (storm) {
		if (fHpdImrLock) IOSimpleLockLock(fHpdImrLock);
		fHpdStormMask |= storm;
		wr(SDEIMR, rd(SDEIMR) | storm);
		if (fHpdImrLock) IOSimpleLockUnlock(fHpdImrLock);
		fHpdStorms++;
		if (fHpdReenableCall) {
			uint64_t d; clock_interval_to_deadline(kHpdStormReenableMs, kMillisecondScale, &d);
			thread_call_enter_delayed(fHpdReenableCall, d);
		}
	}
	pins &= ~storm;
	if (!pins) return;
	OSBitOrAtomic(pins & longs, &fHpdLong);
	OSBitOrAtomic(pins & ~longs, &fHpdShort);
	if (fHpdCall && !fStopping) thread_call_enter(fHpdCall);
}

void IntelXeFramebuffer::hpdReenable()
{
	if (fStopping) return;
	uint32_t was = 0;
	if (fHpdImrLock) IOSimpleLockLock(fHpdImrLock);
	was = fHpdStormMask;
	fHpdStormMask = 0;
	if (fIrqOn) wr(SDEIMR, rd(SDEIMR) & ~(was & rd(SDEIER)));
	if (fHpdImrLock) IOSimpleLockUnlock(fHpdImrLock);
	for (int b = 0; b < 32; b++) fHpdStormCount[b] = 0;
	FBLOG("hotplug: storm pins 0x%08x back on their interrupt", was);
}

// drm_dp_channel_eq_ok on the sink's link status (DPCD 0x202-0x204) for the lanes in use (LANE_COUNT_SET 0x101).
// Caller holds fDispLock (the AUX channel is shared with IntelXeLink and VRR). *checked = false if the AUX did not answer.
bool IntelXeFramebuffer::hpdLinkOk(bool *checked)
{
	uint8_t lanes = 0, st[3] = {};
	*checked = dpcd(0x101, &lanes, 1, false) == 0 && dpcd(0x202, st, 3, false) == 0;
	if (!*checked) return false;
	const int n = lanes & 0x0F;
	if (n < 1 || n > 4) { *checked = false; return false; }
	if (!(st[2] & 0x01)) return false;   // DP_INTERLANE_ALIGN_DONE
	for (int l = 0; l < n; l++) if (((st[l >> 1] >> ((l & 1) * 4)) & 0x7) != 0x7) return false;   // CR, EQ, symbol lock
	return true;
}

// intel_dp_retrain_link at the current mode (IntelXeLink, as debug.intelxefb_link=1), a full modeset if that fails (=2)
int IntelXeFramebuffer::hpdRetrain(const char *why)
{
	if (fStopping || hpdOutputDown()) { FBLOG("hotplug: %s: output down meanwhile, no retrain", why); return 0; }
	int r = linkAction(IntelXeLink::kRetrain);
	if (r && !fStopping) r = linkAction(IntelXeLink::kModeset);
	if (!r && !fStopping) reloadGamma();
	if (r) { fHpdRetrainFails++; fHpdFailStreak++; } else { fHpdRetrains++; fHpdFailStreak = 0; }
	FBLOG("hotplug: %s: link %s (%d)%s", why, r ? "NOT restored" : "retrained", r,
	      fHpdFailStreak >= 3 ? " -- 3 failures in a row: short pulses no longer retrain until a long pulse" : "");
	return r;
}

// intel_dp_short_pulse (SST): the sink's IRQ_HPD. SINK_COUNT and DEVICE_SERVICE_IRQ_VECTOR (0x200-0x201), with DPCD 1.2+
// LINK_SERVICE_IRQ_VECTOR_ESI0 (0x2005); the masked vectors are written back to ack them; a changed sink count or
// RX_CAP_CHANGED wants a full detect (handled as a long pulse); the link status is always checked (Linux forces
// LINK_STATUS_CHANGED) and the link retrained if channel EQ is lost. Automated test / CP_IRQ / sink-specific: logged.
void IntelXeFramebuffer::hpdShortPulse()
{
	uint8_t esi[4] = {}, rev = 0, dsp = 0;
	bool full = false, checked = false, ok = true, readOk;
	dispLock();
	readOk = dpcd(0x000, &rev, 1, false) == 0 && dpcd(0x005, &dsp, 1, false) == 0 && dpcd(0x200, esi, 2, false) == 0;
	if (readOk && rev >= 0x12) readOk = dpcd(0x2005, &esi[3], 1, false) == 0;
	if (readOk) {
		esi[1] &= (1u << 1) | (1u << 2) | (1u << 6);                         // AUTOMATED_TEST, CP_IRQ, SINK_SPECIFIC
		esi[3] &= (1u << 0) | (1u << 1) | (1u << 3) | (1u << 4) | (1u << 5);   // RX_CAP, LINK_STATUS, HDMI_LINK, OFF_ENTRY, TUNNELING
		if (esi[1]) (void)dpcd(0x201, &esi[1], 1, true);
		if (esi[3] && rev >= 0x12) (void)dpcd(0x2005, &esi[3], 1, true);
		if (esi[1] || esi[3]) fHpdEsi++;
		if (dsp & 1) {   // a branch device: its sink count is part of the detection (intel_dp_has_sink_count)
			const uint8_t sc = (uint8_t)(((esi[0] & 0x80) >> 1) | (esi[0] & 0x3F));
			if (fSinkCount != 0xFF && sc != fSinkCount) full = true;
			fSinkCount = sc;
		}
		if (esi[3] & 1) full = true;
		ok = hpdLinkOk(&checked);
	}
	dispUnlock();
	snprintf(fHpdLast, sizeof fHpdLast, "short: esi %02x %02x .. %02x, link %s%s", esi[0], esi[1], esi[3],
	         !readOk ? "AUX did not answer" : !checked ? "status unreadable" : ok ? "ok" : "LOST", full ? ", full detect" : "");
	FBLOG("hotplug: panel short pulse: %s", fHpdLast);
	// a sink whose link cannot be trained keeps sending IRQ_HPD: after 3 failed retrains only a long pulse retries
	if (readOk && (full || (checked && !ok)) && fHpdFailStreak < 3)
		(void)hpdRetrain(full ? "short pulse wants a full detect" : "short pulse, link lost");
}

// i915_digport_work_func + intel_dp_hpd_pulse for the panel's port; other ports are reported only (lighting another
// connector needs a modeset this driver does not do). Long pulse with the port live: the sink re-plugged or power
// cycled; after 500 ms for its AUX (replugFire) the link is checked and retrained if needed (intel_dp_detect ->
// intel_dp_check_link_state). Long pulse with the port down: unplugged, logged; its return is the next long pulse.
void IntelXeFramebuffer::hpdWork()
{
	if (fStopping) return;
	const uint32_t longs = __atomic_exchange_n(&fHpdLong, 0u, __ATOMIC_ACQ_REL);
	const uint32_t shorts = __atomic_exchange_n(&fHpdShort, 0u, __ATOMIC_ACQ_REL) & ~longs;   // a long pulse covers it
	if (!(longs | shorts)) return;
	fHpdLongs += (uint32_t)__builtin_popcount(longs); fHpdShorts += (uint32_t)__builtin_popcount(shorts);
	const uint32_t live = rd(SDEISR) & kHpdPins;
	FBLOG("hotplug: long 0x%08x short 0x%08x, live 0x%08x (panel pin 0x%08x)", longs, shorts, live, fHpdPin);
	if (live != fHpd) { fHpd = live; setProperty("IntelXeFBHotplugLive", live, 32); }
	if (fPipeBAllowed && fPipeBCall && ((longs | shorts) & kHpdPins & ~fHpdPin)) thread_call_enter(fPipeBCall);   // another connector
	if (!fHpdPin || !((longs | shorts) & fHpdPin)) return;
	if (hpdOutputDown() || !fAux) {   // resume and the DPMS wake do a full modeset anyway
		snprintf(fHpdLast, sizeof fHpdLast, "%s: not handled (%s)", (longs & fHpdPin) ? "long" : "short",
		         fSysSuspended ? "system sleep" : !fAux ? "no AUX" : "output powered down");
		FBLOG("hotplug: panel %s", fHpdLast);
		return;
	}
	if (shorts & fHpdPin) { hpdShortPulse(); return; }
	if (!(live & fHpdPin)) {
		snprintf(fHpdLast, sizeof fHpdLast, "long: panel port down (unplugged or powered off)");
		FBLOG("hotplug: %s", fHpdLast);
		return;
	}
	fHpdFailStreak = 0;   // a new detection may retrain again
	IOSleep(500);   // the sink's AUX after its hotplug pulse
	if (fStopping) return;
	bool checked = false, ok;
	dispLock();
	fSinkCount = 0xFF;   // a new detection
	ok = hpdLinkOk(&checked);
	dispUnlock();
	snprintf(fHpdLast, sizeof fHpdLast, "long: panel port live, link %s", !checked ? "status unreadable" : ok ? "ok" : "LOST");
	FBLOG("hotplug: %s", fHpdLast);
	if (!checked || !ok) (void)hpdRetrain("long pulse");
}

// Register with IntelXe.kext and unmask this pipe's vblank (and FIFO underrun, counted) and the TC hotplug pins whose
// detection the firmware enabled (SHOTPLUG_CTL_TC 0x8888), in the gen8_de_pipe / mtp_hpd order: IIR clear, IER, IMR.
bool IntelXeFramebuffer::irqRegister(bool on)
{
	if (fPipe < 0 || !fRegs) return false;
	const int p = fPipe;
	struct { uint32_t version; void (*handler)(void *, uint32_t); void *ctx; } r = { 1, on ? irqThunk : nullptr, this };
	const OSSymbol *sym = OSSymbol::withCString("intelxeDisplayIrq");
	if (on) {
		if (fIrqOn) return true;
		(void)intelxe();
		const IOReturn rr = (fIntelXe && sym) ? fIntelXe->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnNotFound;
		if (sym) sym->release();
		if (rr != kIOReturnSuccess) { FBLOG("display irq: IntelXe.kext did not take the handler (0x%x): polling", (unsigned)rr); return false; }
		wr(DE_PIPE_IIR(p), PIPE_VBLANK | PIPE_FIFO_UNDERRUN);
		wr(DE_PIPE_IER(p), rd(DE_PIPE_IER(p)) | PIPE_VBLANK | PIPE_FIFO_UNDERRUN);
		wr(DE_PIPE_IMR(p), rd(DE_PIPE_IMR(p)) & ~(PIPE_VBLANK | PIPE_FIFO_UNDERRUN));
		uint32_t tc = 0;
		uint32_t hctl = rd(SHOTPLUG_CTL_TC), dctl = rd(SHOTPLUG_CTL_DDI);
		if (!fHpdEnSaved) {   // the firmware's choice of pins, kept for every later registration (the wakes)
			fHpdEnTc = hctl & 0x8888u; fHpdEnDdi = dctl & 0x88u; fSouthChicken1Boot = rd(SOUTH_CHICKEN1); fHpdEnSaved = true;
		} else {
			if ((hctl & 0x8888u) != fHpdEnTc || (dctl & 0x88u) != fHpdEnDdi)
				FBLOG("hotplug: detection enables TC 0x%04x DDI 0x%02x (firmware's TC 0x%04x DDI 0x%02x, cleared by system sleep): set "
				      "again (mtp_ddi/tc_hpd_detection_setup)", hctl & 0x8888u, dctl & 0x88u, fHpdEnTc, fHpdEnDdi);
			const uint32_t sc = rd(SOUTH_CHICKEN1);
			if (sc != fSouthChicken1Boot) {   // HPD inversion lives here (mtp_hpd_invert): back to what the firmware had
				FBLOG("hotplug: SOUTH_CHICKEN1 0x%08x, firmware's 0x%08x: put back", sc, fSouthChicken1Boot);
				wr(SOUTH_CHICKEN1, fSouthChicken1Boot);
			}
		}
		hctl |= fHpdEnTc; dctl |= fHpdEnDdi;
		for (int pin = 0; pin < 4; pin++) if (hctl & (0x8u << (4 * pin))) tc |= 1u << (24 + pin);   // TC1-4 detection enabled
		// mtp_hpd_irq_setup: the 250 us HPD filter (the firmware leaves a longer one, which can swallow a short pulse),
		// then the detection enables (writing the sticky pulse status back clears it) and SDEIIR cleared before the
		// pins are unmasked
		if (fShpdFilterBoot == ~0u) fShpdFilterBoot = rd(SHPD_FILTER_CNT);
		wr(SHPD_FILTER_CNT, SHPD_FILTER_CNT_250);
		wr(SHOTPLUG_CTL_TC, hctl);
		wr(SHOTPLUG_CTL_DDI, dctl);
		wr(SDEIIR, rd(SDEIIR) & (tc | SDE_DDI_HOTPLUG_MASK));
		wr(SDEIER, rd(SDEIER) | tc);
		if (fHpdImrLock) IOSimpleLockLock(fHpdImrLock);
		wr(SDEIMR, rd(SDEIMR) & ~(tc & ~fHpdStormMask));
		if (fHpdImrLock) IOSimpleLockUnlock(fHpdImrLock);
		fIrqVblLast = fIrqVbl; fIrqMiss = 0;
		fIrqOn = true;
		wr(GEN11_DISPLAY_INT_CTL, DISPLAY_IRQ_ENABLE);
		FBLOG("display irq: vblank (pipe %c) and hotplug (SDE 0x%08x, panel pin 0x%08x, filter 0x%x -> 0x%x) interrupts on",
		      'A' + p, tc, fHpdPin, fShpdFilterBoot, SHPD_FILTER_CNT_250);
		return true;
	}
	fIrqOn = false;
	wr(GEN11_DISPLAY_INT_CTL, 0);
	wr(DE_PIPE_IMR(p), rd(DE_PIPE_IMR(p)) | PIPE_FIFO_UNDERRUN);   // as the firmware left it: vblank unmasked, nothing enabled
	wr(DE_PIPE_IER(p), rd(DE_PIPE_IER(p)) & ~(PIPE_VBLANK | PIPE_FIFO_UNDERRUN));
	wr(SDEIER, rd(SDEIER) & ~SDE_TC_HOTPLUG_MASK);
	wr(SDEIMR, rd(SDEIMR) | SDE_TC_HOTPLUG_MASK);
	if (fIntelXe && sym) (void)fIntelXe->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr);   // none in flight after this
	if (sym) sym->release();
	FBLOG("display irq: off (%u vblank, %u hotplug, %u underrun interrupts)", fIrqVbl, fIrqHpd, fIrqUnderruns);
	return false;
}

// Cadence pacer, at the start of each frame. Panels shift brightness with the refresh rate. Under forced VRR the flips
// come on WindowServer's 180 Hz grid, so 100 fps content alternates 5.6 and 11.1 ms frames: a refresh that jumps
// between 180 and 90 Hz frame to frame, seen as pulsing (G50D, 2026-10-29). The pacer learns the content's cadence
// (the interval between new frames on the panel) and holds each frame inside [cadence - lo %, cadence + hi %]: an early
// flip waits for FLIPLINE, so frames come at the content's rate instead of the grid's. Replayed against the logged
// flips this took frame-to-frame changes above 25 % from 44 % to 2 % and the shown-frame jitter from 1.9 to 0.7 ms;
// content that already flips at its own cadence (CoreDisplay's "Variable" mode) passes untouched. Below the panel's
// range LFC does the work; when the screen goes idle the longest frame grows 10 % per frame toward vmax (amdgpu's
// static-screen ramp) instead of dropping to 48 Hz at once.
void IntelXeFramebuffer::vrrPace(uint32_t stampUs)
{
	if (!fVrrOn) return;   // v10l: the cadence pacer only with Adaptive-Sync on
	if (fVrrWin) return;                                   // CoreDisplay sends the window
	const uint32_t live = surfLive();
	if (live != fPaceLastSurf) {                           // a new frame went out at this start of vblank
		const uint32_t iv = fPaceLastUs ? stampUs - fPaceLastUs : 0;
		fPaceLastUs = stampUs; fPaceLastSurf = live;
		if (!iv || iv > 100000) { fPaceUs = 0; fPaceN = 0; }   // after a pause: learn again
		else {
			// the first eight intervals as a plain running mean (one grid-quantised sample must not set the cadence:
			// that showed as a short pulse right after VRR came on), then the 1/8 moving average
			const int32_t div = fPaceN < 8 ? (int32_t)fPaceN + 1 : 8;
			fPaceUs = fPaceN ? (uint32_t)((int32_t)fPaceUs + ((int32_t)iv - (int32_t)fPaceUs) / div) : iv;
			fPaceN++;
		}
	} else if (fPaceLastUs && stampUs - fPaceLastUs > 100000) { fPaceUs = 0; fPaceN = 0; }
	if (!gVrrPaceLo || !fPclkCur) { setWindow(fVrrVmin, fVrrVmax); return; }
	const uint64_t lineNs = (uint64_t)fHT * 1000000000ull / fPclkCur;
	uint32_t fl = fVrrVmin, vm = fVrrVmax;
	if (fPaceUs && fPaceN < 3) return;                     // still learning: keep the window as it is
	if (fPaceUs && fPaceUs <= 4 * fVrrVmaxUs) {
		// Below the panel's range (with hysteresis) the frame length is the cadence divided by the smallest whole
		// number that brings it under 90 % of vmax (amdgpu's below-the-range frame multiplier): 30 fps content is shown
		// as 2 x 16.7 ms frames. The repeats come from VMAX (the window is tight), not from timed pushes, so they stay
		// on the content's cadence instead of WindowServer's 180 Hz grid.
		if (!fPaceLfc && fPaceUs * 100 > fVrrVmaxUs * 95) fPaceLfc = true;
		else if (fPaceLfc && fPaceUs * 100 < fVrrVmaxUs * 85) fPaceLfc = false;
		uint32_t tgt = fPaceUs, hiPct = gVrrPaceHi;
		if (fPaceLfc) {
			uint32_t m = (fPaceUs + fVrrVmaxUs * 9 / 10 - 1) / (fVrrVmaxUs * 9 / 10);
			while (m > 1 && fPaceUs / m < fVrrVminUs * 105 / 100) m--;
			tgt = fPaceUs / m; hiPct = gVrrPaceLo;
			fPaceLfcFrames++;
		}
		// the window's centre moves toward the target by at most 12 % a frame: no jump when the content's rate
		// changes, when LFC starts or ends, or when content resumes after idle (it starts from the idle frame length)
		if (!fPaceC) fPaceC = fVrrVmUs ? fVrrVmUs : tgt;
		const uint32_t up = fPaceC + fPaceC * 12 / 100, down = fPaceC - fPaceC * 12 / 100;
		fPaceC = tgt > up ? up : tgt < down ? down : tgt;
		const uint64_t lo = (uint64_t)fPaceC * (100 - (gVrrPaceLo > 50 ? 50 : gVrrPaceLo)) / 100, hi = (uint64_t)fPaceC * (100 + hiPct) / 100;
		fl = (uint32_t)(lo * 1000 / lineNs); vm = (uint32_t)(hi * 1000 / lineNs);
		if (fl < fVrrVmin) fl = fVrrVmin;
		if (vm > fVrrVmax) vm = fVrrVmax;
		if (vm < fl) vm = fl;
		fPaceFrames++;
	} else {                                               // idle: ramp the refresh down instead of a jump to vmax
		fPaceC = 0; fPaceLfc = false;
		if (fVrrVm < fVrrVmax) { vm = fVrrVm + fVrrVm / 10 + 1; if (vm > fVrrVmax) vm = fVrrVmax; }
	}
	setWindow(fl, vm);
}

void IntelXeFramebuffer::vrrStamp(uint32_t stampUs)
{
	if (!fPclkCur) return;
	const uint64_t lineNs = (uint64_t)fHT * 1000000000ull / fPclkCur;
	uint64_t ago = 0, abs = 0;
	nanoseconds_to_absolutetime((uint64_t)(usNow() - stampUs) * 1000, &ago);
	// FRMTMSTMP is the start of vblank (the decision line); the frame's active start follows the guardband
	nanoseconds_to_absolutetime(fVrrGuard * lineNs, &abs);
	const uint64_t start = mach_absolute_time() - ago + abs;
	uint64_t lo = 0, hi = 0;
	nanoseconds_to_absolutetime(fVrrFl * lineNs, &lo);
	nanoseconds_to_absolutetime(fVrrVm * lineNs, &hi);
	const uint64_t info[5] = { start, start + lo, start + hi, 0, 0 };
	void (*fn)(void *, const uint64_t *, unsigned) = fStampFn;
	if (fn) { fn(fStampCtx, info, 5); fStamps++; }
}

int IntelXeFramebuffer::vrrInfo(char *b, int n)
{
	const int t = fPipe;
	if (t < 0 || !fRegs) return snprintf(b, (size_t)n, "no pipe");
	uint8_t ds = 0xFF; (void)dpcd(DP_DOWNSPREAD_CTRL, &ds, 1, false);
	return snprintf(b, (size_t)n, "allowed %d capable %d on %d range %u-%u Hz vmin %u vmax %u guardband %u (%u-%u us) vbl %u tick %u lfc %d | "
	                "VRR_CTL 0x%08x VMIN %u VMAX %u FLIPLINE %u STATUS 0x%08x STATUS2 %u VTOTAL_PREV 0x%08x PUSH 0x%08x VSYNC 0x%08x SDP_TL 0x%08x "
	                "DPCD107 0x%02x | sleep %u resume %u failed %u | irq %d calls %u vblank %u hotplug %u underrun %u | frames %u pushes %u lfc %u flips %u late %u window %u-%u (%u requests, %u stamps) pace %u-%u%% cadence %u us centre %u us %u frames (%u lfc) lfc-active %d | cursor ready %d image %d visible %d %ux%u images %u moves %u CURCNTR 0x%08x CURPOS 0x%08x",
	                fVrrAllowed, fVrrCapable, (int)fVrrOn, fVrrMinHz, fVrrMaxHz, fVrrVmin, fVrrVmax, fVrrGuard, fVrrVminUs, fVrrVmaxUs,
	                gVrrVbl, gVrrTickUs, gVrrLfc, rd(TRANS_VRR_CTL(t)), rd(TRANS_VRR_VMIN(t)) + 1, rd(TRANS_VRR_VMAX(t)) + 1,
	                rd(TRANS_VRR_FLIPLINE(t)) + 1, rd(TRANS_VRR_STATUS(t)), rd(TRANS_VRR_STATUS(t) + 0x10) & 0xFFFFF,
	                rd(TRANS_VRR_VTOTAL_PREV(t)), rd(TRANS_PUSH(t)), rd(TRANS_VRR_VSYNC(t)), rd(EMP_AS_SDP_TL(t)), ds,
	                fSuspends, fResumes, fResumeFails, (int)fIrqOn, fIrqCalls, fIrqVbl, fIrqHpd, fIrqUnderruns,
	                fVrrFrames, fVrrPushes, fLfcPushes, fFlips, fFlipLate, fVrrFl, fVrrVm, fVrrReqs, fStamps, gVrrPaceLo, gVrrPaceHi, fPaceUs, fPaceC, fPaceFrames, fPaceLfcFrames, (int)(fLfcActive || fPaceLfc), fCurReady, fCurHave, fCurVisible, fCurSrcW, fCurSrcH,
	                fCurImages, fCurMoves, rd(CURCNTR(t)), rd(CURPOS(t)));
}

// ---- hardware cursor ------------------------------------------------------------------------------------------
// The image goes to a fixed 256x256 ARGB buffer (MCURSOR_MODE_256_ARGB_AX, the size Linux uses on skl+, stride 1 KB),
// premultiplied as the cursor plane blends. The pipe scaler stretches plane 1 only; the cursor is at the panel's
// resolution, so in a scaled mode the image is resampled by the same factor and positions are scaled.
bool IntelXeFramebuffer::cursorInit()
{
	if (!fCurAllowed || fPipe < 0 || !fRegs || !fBarLen) return false;
	const int p = fPipe;
	if (kCurDpa + kCurBytes > fBarLen) { FBLOG("cursor: VRAM 0x%llx is past the BAR2 window", kCurDpa); return false; }
	if (rd(CURCNTR(p)) & 0x27) { FBLOG("cursor: CURCNTR 0x%08x already on (firmware?), not taken", rd(CURCNTR(p))); return false; }
	for (uint32_t off = 0; off < kCurBytes; off += 0x1000) {
		const uint64_t e = pte(kCurGgtt + off);
		// our own mapping from before a warm reboot (the GGTT lives in VRAM and survives it) is not someone else's
		if (fCurOwned < 1 && (e & 1) && e != ~0ull && (e & kPteAddr) != kCurDpa + off) { FBLOG("cursor: GGTT 0x%x already mapped (pte 0x%llx)", kCurGgtt + off, e); return false; }
	}
	// DDB: right after plane 1's blocks (the GOP's allocation), inside the first DBUF slice (1024 blocks on BMG)
	const uint32_t buf = rd(PLANE_BUF_CFG1(p)), end = (buf >> 16) & 0xFFF;
	const uint32_t cur = rd(CUR_BUF_CFG(p));
	if (cur || end + 1 + kCurDdbBlocks > 1024) { FBLOG("cursor: no DDB room (plane 1 0x%08x, cursor 0x%08x)", buf, cur); return false; }
	fCurDdb = end + 1;
	fCurLock = IOSimpleLockAlloc();
	fCurImgLock = IOLockAlloc();
	fCurSrc = (uint32_t *)IOMalloc(kCurBytes);
	const bool own = cursorBufferAlloc(&fCurDpa, &fCurBo, &fCurMap);
	if (!own) {
		fCurDpa = kCurDpa;
		if (IODeviceMemory *m = IODeviceMemory::withRange(fBarBase + kCurDpa, kCurBytes)) { fCurMap = m->map(kIOMapWriteCombineCache); m->release(); }
	}
	if (!fCurLock || !fCurImgLock || !fCurSrc || !fCurMap) { FBLOG("cursor: no memory"); return false; }   // stop() frees what exists
	bzero((void *)fCurMap->getVirtualAddress(), kCurBytes);
	__asm__ __volatile__("sfence" ::: "memory");
	l2Flush();
	for (uint32_t off = 0; off < kCurBytes; off += 0x1000) pteW(kCurGgtt + off, (fCurDpa + off) | fPteFlags);
	(void)pte(kCurGgtt + kCurBytes - 0x1000);
	fCurReady = true;
	FBLOG("cursor: hardware cursor ready, 256x256 ARGB at GGTT 0x%x -> VRAM 0x%llx (%s), DDB %u-%u", kCurGgtt, fCurDpa,
	      own ? "IntelXe.kext display buffer" : "fixed fallback, IntelXe.kext display buffer asked again later", fCurDdb, fCurDdb + kCurDdbBlocks - 1);
	return true;
}

// A zeroed display buffer from IntelXe.kext inside the BAR2 window, mapped write-combined for the CPU. false (nothing
// held) if IntelXe.kext has no allocator, is not up yet, or the buffer is outside the window.
bool IntelXeFramebuffer::cursorBufferAlloc(uint64_t *dpa, void **cookie, IOMemoryMap **map)
{
	IOService *b = intelxe();
	const OSSymbol *sym = OSSymbol::withCString("intelxeDispVramAlloc");
	struct { uint32_t version, flags; uint64_t size, dpa; void *cookie; } r = { 1, 1, kCurBytes, 0, nullptr };   // IntelXeDispVram, zeroed
	const IOReturn rr = (b && sym) ? b->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
	if (sym) sym->release();
	if (rr != kIOReturnSuccess || !r.cookie) {
		FBLOG("cursor: no display buffer from IntelXe.kext (0x%x)", (unsigned)rr);
		return false;
	}
	IOMemoryMap *m = nullptr;
	if ((r.dpa & 0xFFF) == 0 && r.dpa + kCurBytes <= fBarLen)
		if (IODeviceMemory *d = IODeviceMemory::withRange(fBarBase + r.dpa, kCurBytes)) { m = d->map(kIOMapWriteCombineCache); d->release(); }
	if (!m) {
		FBLOG("cursor: display buffer at VRAM 0x%llx not mappable through BAR2 -- returned", r.dpa);
		if (const OSSymbol *f = OSSymbol::withCString("intelxeDispVramFree")) { b->callPlatformFunction(f, false, r.cookie, nullptr, nullptr, nullptr); f->release(); }
		return false;
	}
	*dpa = r.dpa; *cookie = r.cookie; *map = m;
	return true;
}

// The cursor still on the fixed fallback (IntelXe.kext was not up when the framebuffer started): move it into a display
// buffer, at most 3 attempts. Caller holds fCurImgLock and renders the image right after, then the PTEs follow
// (cursorRender points the GGTT window at fCurDpa once the new buffer holds the image).
void IntelXeFramebuffer::cursorBufferUpgrade()
{
	if (fCurBo || fCurBoTries >= 3 || !fCurReady) return;
	fCurBoTries++;
	uint64_t dpa = 0; void *cookie = nullptr; IOMemoryMap *map = nullptr;
	if (!cursorBufferAlloc(&dpa, &cookie, &map)) return;
	IOMemoryMap *old = fCurMap;
	fCurMap = map; fCurDpa = dpa; fCurBo = cookie;
	OSSafeReleaseNULL(old);
	FBLOG("cursor: image moved to IntelXe.kext's display buffer at VRAM 0x%llx", dpa);
}

void IntelXeFramebuffer::l2Flush()
{
	IOService *b = intelxe();
	const OSSymbol *sym = OSSymbol::withCString("intelxeL2Flush");
	const IOReturn r = (b && sym) ? b->callPlatformFunction(sym, false, nullptr, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
	if (sym) sym->release();
	if (r == kIOReturnSuccess) fL2Flushes++;
	else if (fL2Fails++ < 4) FBLOG("display buffer written by the CPU: IntelXe.kext L2 flush failed (0x%x)", (unsigned)r);
}

static inline uint32_t premul(uint32_t p)
{
	const uint32_t a = p >> 24;
	return a << 24 | ((((p >> 16) & 255) * a + 127) / 255) << 16 | ((((p >> 8) & 255) * a + 127) / 255) << 8 | ((p & 255) * a + 127) / 255;
}

// fCurSrc (fCurSrcW x fCurSrcH) into the VRAM buffer, scaled by panel/mode size (bilinear on premultiplied pixels)
void IntelXeFramebuffer::cursorRender()
{
	if (!fCurMap || !fCurSrc) return;
	volatile uint32_t *dst = (volatile uint32_t *)fCurMap->getVirtualAddress();
	const uint32_t mw = fCurSpaceW ? fCurSpaceW : fWidth, mh = fCurSpaceH ? fCurSpaceH : fHeight;
	const bool same = mw == fWidth && mh == fHeight;
	uint32_t w = (uint32_t)(((uint64_t)fCurSrcW * fWidth + mw - 1) / mw), h = (uint32_t)(((uint64_t)fCurSrcH * fHeight + mh - 1) / mh);
	if (w > kCurDim) w = kCurDim;
	if (h > kCurDim) h = kCurDim;
	uint32_t row[kCurDim];
	uint32_t fnv = 2166136261u;
	for (uint32_t y = 0; y < kCurDim; y++) {
		for (uint32_t x = 0; x < kCurDim; x++) {
			uint32_t v = 0;
			if (x < w && y < h && fCurSrcW && fCurSrcH) {
				if (same) v = premul(fCurSrc[y * fCurSrcW + x]);
				else {
					// source position of the pixel centre, 16.16
					int32_t u = (int32_t)(((2 * (int64_t)x + 1) * mw << 16) / (2 * (int64_t)fWidth)) - 0x8000;
					int32_t t = (int32_t)(((2 * (int64_t)y + 1) * mh << 16) / (2 * (int64_t)fHeight)) - 0x8000;
					if (u < 0) u = 0;
					if (t < 0) t = 0;
					uint32_t x0 = (uint32_t)u >> 16, y0 = (uint32_t)t >> 16, fx = ((uint32_t)u >> 8) & 255, fy = ((uint32_t)t >> 8) & 255;
					if (x0 >= fCurSrcW) x0 = fCurSrcW - 1;
					if (y0 >= fCurSrcH) y0 = fCurSrcH - 1;
					const uint32_t x1 = x0 + 1 < fCurSrcW ? x0 + 1 : x0, y1 = y0 + 1 < fCurSrcH ? y0 + 1 : y0;
					const uint32_t p00 = premul(fCurSrc[y0 * fCurSrcW + x0]), p01 = premul(fCurSrc[y0 * fCurSrcW + x1]);
					const uint32_t p10 = premul(fCurSrc[y1 * fCurSrcW + x0]), p11 = premul(fCurSrc[y1 * fCurSrcW + x1]);
					for (int c = 0; c < 32; c += 8) {
						const uint32_t a = (p00 >> c) & 255, b = (p01 >> c) & 255, d = (p10 >> c) & 255, e = (p11 >> c) & 255;
						const uint32_t top = a * (256 - fx) + b * fx, bot = d * (256 - fx) + e * fx;
						v |= (((top * (256 - fy) + bot * fy) + 32768) >> 16 & 255) << c;
					}
				}
			}
			row[x] = v;
		}
		for (uint32_t x = 0; x < kCurDim; x++) { dst[y * kCurDim + x] = row[x]; fnv = (fnv ^ row[x]) * 16777619u; }
		if (!y) for (int k = 0; k < 4; k++) fCurW[k] = row[k];
	}
	__asm__ __volatile__("sfence" ::: "memory");
	fCurFnv = fnv; fCurRenders++;
	l2Flush();   // the image is in VRAM, not only in the GPU's L2, before the cursor plane reads it
	// the GGTT window follows the buffer (after a move to IntelXe.kext's display buffer; a no-op compare otherwise)
	if ((pte(kCurGgtt) & kPteAddr) != fCurDpa) {
		for (uint32_t off = 0; off < kCurBytes; off += 0x1000) pteW(kCurGgtt + off, (fCurDpa + off) | fPteFlags);
		(void)pte(kCurGgtt + kCurBytes - 0x1000);
	}
}

// Caller holds fCurLock. i9xx_cursor_update_arm: watermarks and DDB, CUR_FBC_CTL (0 on display 14) and CURCNTR when
// they change, then CURPOS and CURBASE, whose write arms all of it. Under VRR the push brings it out at the next
// decision line instead of at vmax (Linux leaves legacy cursor moves to the next refresh; at 48 Hz idle that is a
// 21 ms cursor).
void IntelXeFramebuffer::cursorWriteLocked()
{
	if (!fCurReady || fPipe < 0 || !fRegs) return;
	const int p = fPipe;
	int32_t x = fCurX, y = fCurY;
	if (fCurSpaceW && fCurSpaceW != fWidth) x = (int32_t)((int64_t)x * (int64_t)fWidth / (int64_t)fCurSpaceW);
	if (fCurSpaceH && fCurSpaceH != fHeight) y = (int32_t)((int64_t)y * (int64_t)fHeight / (int64_t)fCurSpaceH);
	const bool show = gHwCursor && fCurHave && fCurVisible && !fCurParked && !fBlanked &&
	                  x > -(int32_t)kCurDim && y > -(int32_t)kCurDim && x < (int32_t)fWidth && y < (int32_t)fHeight;
	const uint32_t ctl = show ? MCURSOR_MODE_256_ARGB_AX : 0;
	if (show && !fCurWm) {
		// level 0 like the GOP programmed plane 1 (lines 3, deeper levels, transition and SAGV off); 16 blocks of the
		// cursor's 32 hold 8 lines of a 1 KB-per-line cursor
		wr(CUR_WM(p, 0), CUR_WM_EN | 4u << 14 | 16u);
		wr(CUR_BUF_CFG(p), (fCurDdb + kCurDdbBlocks - 1) << 16 | fCurDdb);
		fCurWm = true;
	}
	if (!show && !fCurCtl) return;   // off and staying off
	uint32_t pos = 0;
	if (x < 0) { pos |= CURSOR_POS_X_SIGN; x = -x; }
	if (y < 0) { pos |= CURSOR_POS_Y_SIGN; y = -y; }
	pos |= ((uint32_t)y & 0x7FFF) << 16 | ((uint32_t)x & 0x7FFF);
	if (ctl != fCurCtl) { wr(CUR_FBC_CTL(p), 0); wr(CURCNTR(p), ctl); fCurCtl = ctl; }
	wr(CURPOS(p), pos);
	wr(CURBASE(p), kCurGgtt);
	if (fVrrOn) push(9);
}

IOReturn IntelXeFramebuffer::setCursorImage(void *cursorImage)
{
	if (!fCurReady || !gHwCursor) {
		if (fCurLock) { IOSimpleLockLock(fCurLock); fCurHave = false; cursorWriteLocked(); IOSimpleLockUnlock(fCurLock); }
		return kIOReturnUnsupported;
	}
	// the largest image that still fits 256 panel pixels after a scaled mode's factor
	const uint32_t mw = fCurSpaceW ? fCurSpaceW : fWidth, mh = fCurSpaceH ? fCurSpaceH : fHeight;
	IOHardwareCursorDescriptor d; bzero(&d, sizeof d);
	d.majorVersion = kHardwareCursorDescriptorMajorVersion; d.minorVersion = kHardwareCursorDescriptorMinorVersion;
	d.width = kCurDim * mw / fWidth; d.height = kCurDim * mh / fHeight;
	d.bitDepth = 32;
	d.supportedSpecialEncodings = kTransparentEncodedPixel;
	IOHardwareCursorInfo ci; bzero(&ci, sizeof ci);
	ci.majorVersion = kHardwareCursorInfoMajorVersion; ci.minorVersion = kHardwareCursorInfoMinorVersion;
	IOLockLock(fCurImgLock);
	cursorBufferUpgrade();
	ci.hardwareCursorData = (UInt8 *)fCurSrc;
	const bool ok = convertCursorImage(cursorImage, &d, &ci) && ci.cursorWidth && ci.cursorHeight &&
	                ci.cursorWidth <= d.width && ci.cursorHeight <= d.height;
	if (ok) { fCurSrcW = ci.cursorWidth; fCurSrcH = ci.cursorHeight; cursorRender(); }
	IOLockUnlock(fCurImgLock);
	IOSimpleLockLock(fCurLock);
	fCurHave = ok;
	cursorWriteLocked();
	IOSimpleLockUnlock(fCurLock);
	if (fCurImages++ < 4) FBLOG("cursor image %u: %s %ux%u (hot %u,%u)", fCurImages, ok ? "hardware" : "declined", (unsigned)ci.cursorWidth,
	                            (unsigned)ci.cursorHeight, (unsigned)ci.cursorHotSpotX, (unsigned)ci.cursorHotSpotY);
	return ok ? kIOReturnSuccess : kIOReturnUnsupported;
}

IOReturn IntelXeFramebuffer::setCursorState(SInt32 x, SInt32 y, bool visible)
{
	if (!fCurReady || !fCurLock) return kIOReturnUnsupported;
	IOSimpleLockLock(fCurLock);
	fCurX = x; fCurY = y; fCurVisible = visible; fCurMoves++;
	cursorWriteLocked();
	IOSimpleLockUnlock(fCurLock);
	return gHwCursor ? kIOReturnSuccess : kIOReturnUnsupported;
}

// ---- macOS Adaptive-Sync: programmable timings (v4) ----------------------------------------------------------------
// CoreDisplay (macOS 15 x86) marks a display adaptive-sync capable when its preference enableAdaptiveSync is on (the
// default), the EDID says DisplayPort + continuous frequency, and the framebuffer's IOFBTimingRange has
// supportedSyncFlags bit 0x10 (IODisplayInstallTimings). It then installs each EDID timing twice through
// IOFBInstallMode/InstallTimingImpl: fixed, and "variable" with verticalBlankingExtension = the lines up to the lowest
// rate; the mode id is in detailedInfo.v2.__reservedA[0]. Apple's Intel framebuffer validates the extension
// (validateDetailedTiming) and enables VRR when such a timing is set (hwSetMode -> hwEnableVRR); this does the same.
const char *IntelXeFramebuffer::progCheck(const IODetailedTimingInformationV2 &t) const
{
	if (t.horizontalActive != fWidth || t.verticalActive != fHeight) return "active size";
	if (t.horizontalActive + t.horizontalBlanking != fHT) return "horizontal total";
	// syncs: the firmware's stay on the pipe (nothing reprograms them); a timing that names others (DisplayID's
	// minus-one fields, polarity bits) still runs the same raster, so they are not a reason to refuse it
	const uint32_t vt = t.verticalActive + t.verticalBlanking;
	if (vt < fVT || vt > 8192) return "vertical total";
	if (t.horizontalBorderLeft || t.horizontalBorderRight || t.verticalBorderTop || t.verticalBorderBottom) return "borders";
	// HiDPI: a scaled timing (horizontalScaled above the active width) is reported accelerator backed
	// (validateDetailedTiming): WindowServer renders the larger image and scales it to the panel on the GPU, and the pipe
	// runs the panel's size at this timing (setDisplayMode). The display engine's scaler is not used, so its display
	// clock limit (icl_plane_min_cdclk) does not apply; the sizes stay within what was tested: at most 4096x2304, the
	// plane scaler's source limit on display 14 (skl_scaler.c), and a factor below 3. WAS (first 0.1.3 builds): the plane
	// scaler downscaled them, which the display clock held to about 100 Hz.
	if (t.horizontalScaled || t.verticalScaled) {
		const uint32_t sw = t.horizontalScaled, sh = t.verticalScaled;
		if (sw != t.horizontalActive || sh != t.verticalActive) {
			if (!fScalerPublished || fHomeOwned < 0) return "no HiDPI home surface";
			if (!sw || !sh || sw < t.horizontalActive || sh < t.verticalActive) return "upscaled timing";
			if (t.horizontalScaledInset || t.verticalScaledInset || t.scalerFlags) return "scaler inset or flags";
			if (sw > kScalerMaxW || (uint64_t)((sw * 4 + 63) & ~63u) * sh > kHomeMax) return "scaled size above the scaler's";
			const uint32_t hs = (uint32_t)(((uint64_t)sw << 16) / t.horizontalActive), vs = (uint32_t)(((uint64_t)sh << 16) / t.verticalActive);
			if (hs >= 0x30000 || vs >= 0x30000) return "downscale of 3 or more";
			const uint64_t hsc = (hs & ~0xFFFFu) + 2ull * (hs & 0xFFFFu);
			const uint64_t rateK = ((t.pixelClock / 1000) * hsc * vs + (1ull << 32) - 1) >> 32;
			(void)rateK;   // not the display engine's scaling (see above): its clock limit does not apply
		}
	}
	if (t.signalConfig & kIOInterlacedCEATiming) return "interlaced";
	if (t.dynamicRange & ~(uint32_t)kIORangeDynamicRangeSDR) return "HDR";
	// the link was trained for the firmware's pixel clock: anything up to it is an M/N change on the same link
	if (!fPclk || t.pixelClock > fPclkMax + fPclkMax / 200 || t.pixelClock < fPclk / 8) return "pixel clock";
	const uint64_t hz = t.pixelClock / ((uint64_t)fHT * vt);
	if (fVrrMaxHz && hz > fVrrMaxHz + 1) return "refresh above the panel's";
	if (t.verticalBlankingExtension) {
		if (!fVrrCapable || !fVrrAllowed) return "variable refresh not available";
		const uint64_t longest = t.pixelClock / ((uint64_t)fHT * (fVrrMinHz ? fVrrMinHz : 1));
		if (vt + t.verticalBlankingExtension > longest + 2 || vt + t.verticalBlankingExtension > 0xFFFFF) return "blanking extension";
	}
	return nullptr;
}

// IOFBScalerInfo: CoreDisplay installs scaled timings (the HiDPI modes of a display override's scale-resolutions) only
// with it (IOGraphicsLib IOFBInstallScaledModes, IOFBInstallScaledResolution). Downscaling only, up to the scaler's
// source limit and the home surface; the upscaled sizes are the driver's own modes.
void IntelXeFramebuffer::publishScalerInfo()
{
	fScalerPublished = false;
	if (!gHidpi) { removeProperty(kIOFBScalerInfoKey); return; }   // opt-in: intelxefb-hidpi=1
	if (!fScaling || fHomeOwned < 0 || !fCdclkKhz) { removeProperty(kIOFBScalerInfoKey); FBLOG("scaler: no HiDPI modes (scaling %d, home %d, cdclk %u)", (int)fScaling, fHomeOwned, fCdclkKhz); return; }
	IODisplayScalerInformation si; bzero(&si, sizeof si);
	si.scalerFeatures = kIOScaleCanDownSamplePixels;
	si.maxHorizontalPixels = kScalerMaxW; si.maxVerticalPixels = kScalerMaxH;
	if (OSData *d = OSData::withBytes(&si, sizeof si)) { setProperty(kIOFBScalerInfoKey, d); d->release(); fScalerPublished = true; }
	FBLOG("scaler: IOFBScalerInfo published (downscale, up to %ux%u)", kScalerMaxW, kScalerMaxH);
}

// The HiDPI home surface: a zeroed display buffer from IntelXe.kext inside the BAR2 window, mapped write-combined and at
// kHomeGgtt. Kept once allocated (the plane may still scan it until the next vblank after a mode change); freed at stop.
bool IntelXeFramebuffer::homeBigAlloc()
{
	if (fHomeBo) return true;
	if (fHomeOwned < 0) return false;
	IOService *b = intelxe();
	const OSSymbol *sym = OSSymbol::withCString("intelxeDispVramAlloc");
	struct { uint32_t version, flags; uint64_t size, dpa; void *cookie; } r = { 1, 1, kHomeMax, 0, nullptr };   // IntelXeDispVram, zeroed
	const IOReturn rr = (b && sym) ? b->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
	if (sym) sym->release();
	if (rr != kIOReturnSuccess || !r.cookie) { FBLOG("hidpi: no display buffer from IntelXe.kext (0x%x)", (unsigned)rr); return false; }
	IOMemoryMap *m = nullptr;
	if ((r.dpa & 0xFFF) == 0 && r.dpa + kHomeMax <= fBarLen)
		if (IODeviceMemory *d = IODeviceMemory::withRange(fBarBase + (IOPhysicalAddress)r.dpa, kHomeMax)) { m = d->map(kIOMapWriteCombineCache); d->release(); }
	if (!m) {
		FBLOG("hidpi: display buffer at VRAM 0x%llx not mappable through BAR2 -- returned", r.dpa);
		if (const OSSymbol *f = OSSymbol::withCString("intelxeDispVramFree")) { b->callPlatformFunction(f, false, r.cookie, nullptr, nullptr, nullptr); f->release(); }
		return false;
	}
	for (uint32_t off = 0; off < kHomeMax; off += 0x1000) pteW(kHomeGgtt + off, (r.dpa + off) | fPteFlags);
	(void)pte(kHomeGgtt + kHomeMax - 0x1000);
	fHomeBo = r.cookie; fHomeDpa = r.dpa; fHomeMap = m;
	FBLOG("hidpi: home surface %u KB at GGTT 0x%x -> VRAM 0x%llx", kHomeMax >> 10, kHomeGgtt, r.dpa);
	return true;
}

// At stop, after the plane went back to the firmware's surface: GGTT window cleared, the buffer returned.
void IntelXeFramebuffer::homeBigFree()
{
	if (!fHomeBo) return;
	fHomeBig = false;
	for (uint32_t off = 0; off < kHomeMax; off += 0x1000) pteW(kHomeGgtt + off, 0);
	OSSafeReleaseNULL(fHomeMap);
	if (IOService *b = intelxe())
		if (const OSSymbol *f = OSSymbol::withCString("intelxeDispVramFree")) { b->callPlatformFunction(f, false, fHomeBo, nullptr, nullptr, nullptr); f->release(); }
	fHomeBo = nullptr; fHomeDpa = 0;
}

IOReturn IntelXeFramebuffer::validateDetailedTiming(void *description, IOByteCount size)
{
	IODetailedTimingInformationV2 *t = nullptr;
	if (description && size == sizeof(IOFBDisplayModeDescription)) t = &((IOFBDisplayModeDescription *)description)->timingInfo.detailedInfo.v2;
	else if (description && size == sizeof(IODetailedTimingInformationV2)) t = (IODetailedTimingInformationV2 *)description;
	static uint32_t sizesLogged;
	if (!t) { if (sizesLogged++ < 4) FBLOG("validateDetailedTiming: unknown description size %llu", (unsigned long long)size); return kIOReturnBadArgument; }
	if (!fRangePublished) return kIOReturnUnsupported;
	const char *why = progCheck(*t);
	const uint32_t vt = t->verticalActive + t->verticalBlanking;
	if (why) {
		if (fProgRejects++ < 32) FBLOG("timing %ux%u total %ux%u %llu kHz ext %u: refused (%s)", (unsigned)t->horizontalActive,
		                               (unsigned)t->verticalActive, (unsigned)(t->horizontalActive + t->horizontalBlanking), vt,
		                               (unsigned long long)(t->pixelClock / 1000), (unsigned)t->verticalBlankingExtension, why);
		return kIOReturnUnsupportedMode;
	}
	t->minPixelClock = t->maxPixelClock = t->pixelClock;   // exactly what the M/N will give
	// The mode information goes back with the timing, as IONDRVFramebuffer::validateDetailedTiming does it
	// (getResInfoForDetailed): CoreDisplay's IOFBDriverPreflight copies nominalWidth/Height and maxDepthIndex from the
	// answer into the mode it installs. Left as sent (0 x 0), the installed modes reached IOFBConfig without a size
	// and WindowServer's mode list dropped them, the variable one with them (v10b, 2026-10-29).
	if (size == sizeof(IOFBDisplayModeDescription)) {
		IODisplayModeInformation &info = ((IOFBDisplayModeDescription *)description)->info;
		const bool scaled = t->horizontalScaled && t->verticalScaled;
		info.nominalWidth = scaled ? t->horizontalScaled : t->horizontalActive;
		info.nominalHeight = scaled ? t->verticalScaled : t->verticalActive;
		info.maxDepthIndex = 0;   // one depth: 32-bit direct (getPixelInformation)
		if (scaled && (t->horizontalScaled != t->horizontalActive || t->verticalScaled != t->verticalActive))
			info.flags |= kDisplayModeAcceleratorBackedFlag;
		info.refreshRate = (uint32_t)((t->pixelClock << 16) / ((uint64_t)(t->horizontalActive + t->horizontalBlanking) * vt));
	}
	static uint32_t okLogged;
	if (okLogged++ < 16) FBLOG("timing %ux%u vtotal %u %llu kHz ext %u: accepted", (unsigned)t->horizontalActive, (unsigned)t->verticalActive,
	                           vt, (unsigned long long)(t->pixelClock / 1000), (unsigned)t->verticalBlankingExtension);
	return kIOReturnSuccess;
}

IOReturn IntelXeFramebuffer::setDetailedTimings(OSArray *array)
{
	int n = 0;
	for (unsigned i = 0; array && i < array->getCount() && n < kMaxProg; i++) {
		OSData *d = OSDynamicCast(OSData, array->getObject(i));
		if (!d || d->getLength() < sizeof(IODetailedTimingInformationV2)) continue;
		IODetailedTimingInformationV2 t;
		bcopy(d->getBytesNoCopy(), &t, sizeof t);
		const IODisplayModeID id = (IODisplayModeID)t.__reservedA[0];
		if ((uint32_t)id < (uint32_t)kDriverModeIdLimit || progCheck(t)) continue;   // CoreDisplay's ids are 0x80000000 and up
		fProg[n].id = id; fProg[n].t = t; n++;
	}
	fNumProg = n;
	FBLOG("detailed timings: %d of %u installed", n, array ? array->getCount() : 0);
	return kIOReturnSuccess;
}

// Caller holds dispLock, VRR off. Data and link M scale with the pixel clock (same link, lanes and bpp, so the
// firmware's N, TU and overhead stay); the line time watermark follows (hsw_linetime_wm, rounded up as the GOP does).
void IntelXeFramebuffer::programPixelClock(uint64_t pclk)
{
	if (!pclk || !fPclk || pclk == fPclkCur || fPipe < 0) return;
	const int t = fPipe;
	if (pclk == fPclk) {
		wr(TRANS_DATA_M1(t), fDataMBoot); wr(TRANS_DATA_N1(t), fDataNBoot);
		wr(TRANS_LINK_M1(t), fLinkMBoot); wr(TRANS_LINK_N1(t), fLinkNBoot);
		wr(WM_LINETIME(t), fLinetimeBoot);
	} else {
		const uint64_t dm = (uint64_t)(fDataMBoot & 0xFFFFFFu) * pclk / fPclk, lm = (uint64_t)fLinkMBoot * pclk / fPclk;
		if (dm >= fDataNBoot || lm > 0xFFFFFF) { FBLOG("pixel clock %llu kHz does not fit the link", pclk / 1000); return; }
		wr(TRANS_DATA_M1(t), (fDataMBoot & ~0xFFFFFFu) | (uint32_t)dm); wr(TRANS_DATA_N1(t), fDataNBoot);
		wr(TRANS_LINK_M1(t), (uint32_t)lm); wr(TRANS_LINK_N1(t), fLinkNBoot);
		const uint64_t khz = pclk / 1000;
		uint32_t lt = (uint32_t)(((uint64_t)fHT * 8000 + khz - 1) / khz);
		if (lt > 0x1FF) lt = 0x1FF;
		wr(WM_LINETIME(t), (rd(WM_LINETIME(t)) & ~0x1FFu) | lt);
	}
	fPclkCur = pclk;
	if (fModeVT) fFrameNs = (uint64_t)fHT * fModeVT * 1000000000ull / fPclkCur;
	publishFlipWait();
	FBLOG("pixel clock %llu kHz (M/N %s)", pclk / 1000, pclk == fPclk ? "the firmware's" : "scaled");
}

// nullmoth's NVRMTiming for NMXAGDC's link answer, and the flip wait, for the mode now on the pipe
void IntelXeFramebuffer::publishTiming()
{
	if (fPipe < 0 || !fPclkCur || !fModeVT) return;
	const uint32_t ddi = rd(TRANS_DDI_FUNC_CTL(fPipe));
	const uint32_t mhz = (uint32_t)(fPclkCur * 1000 / ((uint64_t)fHT * fModeVT));
	uint32_t t[13] = { 0x4b350001u, (uint32_t)fPclkCur, fWidth, fHSS, fHSE, fHT, fHeight, fVSS, fVSE, fModeVT, mhz,
	                   (ddi >> 16) & 1, (ddi >> 17) & 1 };
	if (OSData *d = OSData::withBytes(t, sizeof t)) { setProperty("NVRMTiming", d); d->release(); }
	setProperty("IntelXeFBVRRModeExtension", (unsigned long long)fVrrModeExt, 32);
	publishFlipWait();
}

void IntelXeFramebuffer::applyVrrLocked(const char *why)
{
	const bool want = vrrWanted() && fVrrCapable && fVrrAllowed;
	if (want && !fVrrOn) vrrEnableLocked();
	else if (!want && fVrrOn) vrrDisableLocked(why);
}

void IntelXeFramebuffer::publishTimingRange(bool on)
{
	if (!on || fPipe < 0 || !fPclk) { removeProperty(kIOFBTimingRangeKey); fRangePublished = false; return; }
	IODisplayTimingRangeV2 r; bzero(&r, sizeof r);
	r.version = kIOTimingRangeV2;
	// EDID timings carry 10 kHz steps: 0.5 % headroom (as progCheck allows) so the firmware's own timing, rounded up to
	// 706.25 MHz from the link's 706.249 MHz, is not above the range (CoreDisplay's CheckTimingWithRange)
	const uint64_t top = fPclkMax + fPclkMax / 200;   // the highest mode's clock (buildModes), not only the firmware's
	r.minPixelClock = fPclk / 8; r.maxPixelClock = top;
	// bit 0x10: adaptive sync (CoreDisplay tests it; Apple's Intel framebuffer sets it when the port supports VRR)
	r.supportedSyncFlags = kIORangeSupportsSeparateSyncs | ((fVrrCapable && fVrrAllowed) ? 0x10u : 0u);
	r.minFrameRate = fVrrMinHz ? fVrrMinHz : 24;
	r.maxFrameRate = (uint32_t)((fPclkMax * 1000 / ((uint64_t)fHT * fVT) + 999) / 1000);
	r.minLineRate = 1000; r.maxLineRate = (uint32_t)(top / fHT) + 1;
	r.maxHorizontalTotal = fHT; r.maxVerticalTotal = 8192;
	r.charSizeHorizontalActive = r.charSizeHorizontalBlanking = r.charSizeHorizontalSyncOffset = r.charSizeHorizontalSyncPulse = 1;
	r.charSizeVerticalActive = r.charSizeVerticalBlanking = r.charSizeVerticalSyncOffset = r.charSizeVerticalSyncPulse = 1;
	r.charSizeHorizontalBorderLeft = r.charSizeHorizontalBorderRight = r.charSizeVerticalBorderTop = r.charSizeVerticalBorderBottom = 1;
	r.charSizeHorizontalTotal = r.charSizeVerticalTotal = 1;
	// only what this pipe can run without a modeset: the firmware's active size and horizontal total. Syncs are not
	// pinned here (v9c): the pipe keeps the firmware's syncs whatever a timing says (progCheck), and an exact range let
	// CoreDisplay's CheckTimingWithRange drop the 180 Hz DisplayID timing over a one-off sync field ("fbRange_a", 03:39)
	r.minHorizontalActiveClocks = r.maxHorizontalActiveClocks = fWidth;
	r.minHorizontalBlankingClocks = r.maxHorizontalBlankingClocks = fHT - fWidth;
	r.minHorizontalSyncOffsetClocks = 0; r.maxHorizontalSyncOffsetClocks = fHT - fWidth;
	r.minHorizontalPulseWidthClocks = 0; r.maxHorizontalPulseWidthClocks = fHT - fWidth;
	r.minVerticalActiveClocks = r.maxVerticalActiveClocks = fHeight;
	r.minVerticalBlankingClocks = fVT - fHeight; r.maxVerticalBlankingClocks = 8192 - fHeight;
	r.minVerticalSyncOffsetClocks = 0; r.maxVerticalSyncOffsetClocks = 8192 - fHeight;
	r.minVerticalPulseWidthClocks = 0; r.maxVerticalPulseWidthClocks = 8192 - fHeight;
	// CoreDisplay counts "links" the DVI way: InstallTimingImpl sets numLinks = 2 on any timing above the 165 MHz
	// single-link crossover, and CheckTimingWithRange refuses numLinks > maxNumLinks (its check 0x23). With 1 here every
	// timing of this panel above 165 MHz (the 180 Hz one at 706.25 MHz) was refused as "fbRange_a" (v10, 2026-10-29).
	// DisplayPort has no second link: the pipe runs the same single DP link whatever numLinks says (progCheck ignores it).
	r.maxNumLinks = 2;
	r.minLink0PixelClock = (uint32_t)(fPclk / 8 / 1000); r.maxLink0PixelClock = (uint32_t)(top / 1000);
	r.minLink1PixelClock = r.minLink0PixelClock; r.maxLink1PixelClock = r.maxLink0PixelClock;
	r.supportedPixelEncoding = kIORangePixelEncodingRGB444; r.supportedBitsPerColorComponent = kIORangeBitsPerColorComponent8;
	r.supportedColorimetryModes = kIORangeColorimetryNativeRGB; r.supportedDynamicRangeModes = kIORangeDynamicRangeSDR;
	if (OSData *d = OSData::withBytes(&r, sizeof r)) { setProperty(kIOFBTimingRangeKey, d); d->release(); }
	fRangePublished = true;
	FBLOG("IOFBTimingRange published: %ux%u, %llu-%llu kHz, %u-%u Hz, sync flags 0x%x", fWidth, fHeight, fPclk / 8000,
	      top / 1000, r.minFrameRate, r.maxFrameRate, r.supportedSyncFlags);
}

// The firmware may run a timing the EDID does not list (on the G5: the 165 Hz detailed timing's raster at the pixel
// clock of 180 Hz). CoreDisplay builds its modes, variable ones included, from EDID timings only, and installs one only
// if it passes the EDID's own range limits (InstallTiming -> CheckTimingWithRange). With VRR modes on, the EDID handed
// to IODisplay gets the firmware's timing: in place of the last CTA detailed timing when its clock fits a detailed
// timing descriptor (655.35 MHz), else as a DisplayID extension block with a Type I timing (CoreDisplay's
// InstallDisplayIDEXT; the G5's 706.25 MHz). The range-limits descriptor is widened to the firmware's line rate (the G5
// lists 267 kHz only; 706.25 MHz / 2650 is 266.5 kHz). Checksums fixed; the monitor is untouched; -intelxefb_noedidfix
// serves the EDID as read (2026-10-29 v9: before, the 180 Hz timing was never added and no EDID timing passed the
// range, so CoreDisplay installed nothing and Displays showed no "Variable").
static void edidSum(uint8_t *blk) { uint8_t s = 0; for (int i = 0; i < 127; i++) s = (uint8_t)(s + blk[i]); blk[127] = (uint8_t)(0x100 - s); }
void IntelXeFramebuffer::vrrModesInit()
{
	if (!gVrrModes) return;
	char dummy[8];
	if (fEdidLen >= 256 && fEdidLen + 128 <= kEdidMax && !intelxeBootArg("IntelXeFB", "-intelxefb_noedidfix", dummy, sizeof dummy)) {
		const uint8_t *b = fEdid + 54;
		auto B = [b](int k) -> uint32_t { return b[k]; };
		const uint32_t ha = B(2) | (B(4) >> 4) << 8, hb = B(3) | (B(4) & 15) << 8, va = B(5) | (B(7) >> 4) << 8, vb = B(6) | (B(7) & 15) << 8;
		const uint32_t ho = B(8) | ((B(11) >> 6) & 3) << 8, hw = B(9) | ((B(11) >> 4) & 3) << 8;
		const uint32_t vo = (B(10) >> 4) | ((B(11) >> 2) & 3) << 4, vw = (B(10) & 15) | (B(11) & 3) << 4;
		const uint32_t pc = (B(0) | B(1) << 8), want = (uint32_t)((fPclk + 5000) / 10000);
		const bool same = pc && ha == fWidth && va == fHeight && ha + hb == fHT && va + vb == fVT && ho == fHSS - fWidth &&
		                  hw == fHSE - fHSS && vo == fVSS - fHeight && vw == fVSE - fVSS;
		bcopy(fEdid, fEdidOut, fEdidLen);
		fEdidOutLen = fEdidLen;
		bool changed = false;
		// v10m: the firmware's timing among the EDID's own (the G5 lists it in its DisplayID block, which v10l never read)
		bool listed = false;
		for (int i = 0; fEdidParse && i < fEdidParse->count; i++) {
			const intelxeedid::Mode &m = fEdidParse->modes[i];
			if ((uint32_t)m.hdisplay == fWidth && (uint32_t)m.vdisplay == fHeight && (uint32_t)m.htotal == fHT &&
			    (uint32_t)m.vtotal == fVT && (uint32_t)(m.hsync_start - m.hdisplay) == fHSS - fWidth && m.clock / 10 == (int)want)
				listed = true;
		}
		if (pc == want && same) {
			// the preferred timing already is the firmware's
		} else if (listed) {
			FBLOG("edid: the firmware's timing %ux%u at %u0 kHz is in the EDID already (%s): nothing added", fWidth, fHeight, want,
			      fEdidParse->hasDisplayId ? "DisplayID" : "a detailed timing");
		} else if (want <= 0xFFFF && fEdid[128] == 0x02 && fEdid[130] >= 4) {
			uint32_t last = 0;
			for (uint32_t o = 128u + fEdid[130]; o + 18 <= 255 && (fEdid[o] | fEdid[o + 1]); o += 18) last = o;
			if (last) {
				bcopy(b, fEdidOut + last, 18);
				fEdidOut[last] = (uint8_t)want; fEdidOut[last + 1] = (uint8_t)(want >> 8);
				// the raster is the firmware's even when the preferred timing's is not
				const uint32_t hbk = fHT - fWidth, vbk = fVT - fHeight, hso = fHSS - fWidth, hsw = fHSE - fHSS;
				const uint32_t vso = fVSS - fHeight, vsw = fVSE - fVSS;
				uint8_t *d = fEdidOut + last;
				d[2] = (uint8_t)fWidth; d[3] = (uint8_t)hbk; d[4] = (uint8_t)((fWidth >> 8) << 4 | (hbk >> 8));
				d[5] = (uint8_t)fHeight; d[6] = (uint8_t)vbk; d[7] = (uint8_t)((fHeight >> 8) << 4 | (vbk >> 8));
				d[8] = (uint8_t)hso; d[9] = (uint8_t)hsw; d[10] = (uint8_t)((vso & 15) << 4 | (vsw & 15));
				d[11] = (uint8_t)(((hso >> 8) & 3) << 6 | ((hsw >> 8) & 3) << 4 | ((vso >> 4) & 3) << 2 | ((vsw >> 4) & 3));
				edidSum(fEdidOut + 128);
				changed = true;
				FBLOG("edid: firmware timing %ux%u at %u0 kHz in the CTA block (slot %u) for CoreDisplay", fWidth, fHeight, want, last - 128);
			}
		} else if (want > 0xFFFF && want <= 0x1000000) {
			// DisplayID 1.3 extension: section header (version, length, product type, extension count), a Type I
			// detailed timing block (tag 0x03, 20-byte descriptor, every field stored minus 1), section checksum
			const uint32_t ddi = rd(TRANS_DDI_FUNC_CTL(fPipe));
			uint8_t *x = fEdidOut + fEdidLen;
			bzero(x, 128);
			x[0] = 0x70; x[1] = 0x13; x[2] = 23; x[3] = 0x00; x[4] = 0;
			uint8_t *t = x + 5;
			t[0] = 0x03; t[1] = 0x00; t[2] = 20;
			uint8_t *dsc = t + 3;
			auto w16 = [](uint8_t *q, uint32_t v) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); };
			const uint32_t c = want - 1;
			dsc[0] = (uint8_t)c; dsc[1] = (uint8_t)(c >> 8); dsc[2] = (uint8_t)(c >> 16);
			dsc[3] = 0x80 | 0x04;   // preferred, 16:9
			w16(dsc + 4, fWidth - 1); w16(dsc + 6, fHT - fWidth - 1);
			w16(dsc + 8, (fHSS - fWidth - 1) | (((ddi >> 16) & 1) << 15)); w16(dsc + 10, fHSE - fHSS - 1);
			w16(dsc + 12, fHeight - 1); w16(dsc + 14, fVT - fHeight - 1);
			w16(dsc + 16, (fVSS - fHeight - 1) | (((ddi >> 17) & 1) << 15)); w16(dsc + 18, fVSE - fVSS - 1);
			uint8_t sum = 0;
			for (int i = 1; i < 5 + 23; i++) sum = (uint8_t)(sum + x[i]);
			x[5 + 23] = (uint8_t)(0x100 - sum);
			edidSum(x);
			fEdidOutLen = fEdidLen + 128;
			fEdidOut[126] = (uint8_t)(fEdidOutLen / 128 - 1);
			changed = true;
			FBLOG("edid: firmware timing %ux%u at %u0 kHz as a DisplayID extension (block %u) for CoreDisplay", fWidth, fHeight, want, fEdidLen / 128);
		}
		// HiDPI (opt-in): the downscale multiplies the pipe's pixel rate, and the display clock caps it (progCheck), so the
		// firmware's 165/180 Hz timings take no HiDPI mode. CoreDisplay derives scaled timings only from the EDID's, so
		// the firmware's raster at lower pixel clocks (100, 75, 60 Hz: the same totals, M/N only) goes in as one more
		// DisplayID extension of Type I timings. Without intelxefb-hidpi nothing here changes.
		uint32_t lowestHz = 0;
		if (gHidpi && fEdidOutLen + 128 <= kEdidMax && fEdidOutLen >= 128) {
			static const uint32_t kLowHz[] = { 100, 75, 60 };
			const uint32_t ddi = rd(TRANS_DDI_FUNC_CTL(fPipe));
			uint8_t *x = fEdidOut + fEdidOutLen;
			bzero(x, 128);
			uint32_t nd = 0;
			auto w16 = [](uint8_t *q, uint32_t v) { q[0] = (uint8_t)v; q[1] = (uint8_t)(v >> 8); };
			for (uint32_t hz : kLowHz) {
				const uint64_t lowPc = (uint64_t)fHT * fVT * hz;
				if (lowPc >= fPclk || lowPc < fPclk / 8) continue;
				const uint32_t c = (uint32_t)((lowPc + 5000) / 10000) - 1;
				uint8_t *dsc = x + 8 + nd * 20;
				dsc[0] = (uint8_t)c; dsc[1] = (uint8_t)(c >> 8); dsc[2] = (uint8_t)(c >> 16);
				dsc[3] = 0x04;   // 16:9, not preferred
				w16(dsc + 4, fWidth - 1); w16(dsc + 6, fHT - fWidth - 1);
				w16(dsc + 8, (fHSS - fWidth - 1) | (((ddi >> 16) & 1) << 15)); w16(dsc + 10, fHSE - fHSS - 1);
				w16(dsc + 12, fHeight - 1); w16(dsc + 14, fVT - fHeight - 1);
				w16(dsc + 16, (fVSS - fHeight - 1) | (((ddi >> 17) & 1) << 15)); w16(dsc + 18, fVSE - fVSS - 1);
				nd++; lowestHz = hz;
			}
			if (nd) {
				const uint32_t payload = 3 + 20 * nd;
				x[0] = 0x70; x[1] = 0x13; x[2] = (uint8_t)payload; x[3] = 0x00; x[4] = 0;
				x[5] = 0x03; x[6] = 0x00; x[7] = (uint8_t)(20 * nd);
				uint8_t sum = 0;
				for (uint32_t i = 1; i < 5 + payload; i++) sum = (uint8_t)(sum + x[i]);
				x[5 + payload] = (uint8_t)(0x100 - sum);
				edidSum(x);
				fEdidOutLen += 128;
				fEdidOut[126] = (uint8_t)(fEdidOutLen / 128 - 1);
				changed = true;
				FBLOG("edid: HiDPI: %u lower-clock timings on the firmware's raster (down to %u Hz) as a DisplayID extension", nd, lowestHz);
			}
		}
		// the range limits descriptor (tag 0xFD) in the base block must take the firmware's line and frame rates
		for (int o = 54; o <= 108; o += 18) {
			uint8_t *d = fEdidOut + o;
			if (d[0] || d[1] || d[2] || d[3] != 0xFD) continue;
			const uint32_t lineK = (uint32_t)(fPclk / fHT / 1000), lineTopK = (uint32_t)(fPclkMax / fHT / 1000);   // kHz, rounded down
			uint32_t minH = d[7] + ((d[4] & 0x04) ? 255 : 0), maxH = d[8] + ((d[4] & 0x08) ? 255 : 0);
			const uint32_t lineLowK = lowestHz ? fVT * lowestHz / 1000 : lineK;   // the HiDPI timings' lowest line rate
			const uint32_t want0 = lineLowK > 1 ? lineLowK - 1 : lineLowK, want1 = lineTopK + 2;
			if (minH > want0 || maxH < want1) {
				if (minH > want0) minH = want0;
				if (maxH < want1) maxH = want1;
				d[4] &= (uint8_t)~0x0C;
				if (minH > 255) { d[4] |= 0x04; d[7] = (uint8_t)(minH - 255); } else d[7] = (uint8_t)minH;
				if (maxH > 255) { d[4] |= 0x08; d[8] = (uint8_t)(maxH - 255); } else d[8] = (uint8_t)maxH;
				if (d[9] * 10u < (uint32_t)(fPclkMax / 1000000) + 1) d[9] = (uint8_t)((fPclkMax / 1000000 + 10) / 10);
				changed = true;
				FBLOG("edid: range limits widened to %u-%u kHz for the firmware's %u kHz line rate", minH, maxH, lineK);
			}
			break;
		}
		if (changed) { edidSum(fEdidOut); fEdidAug = true; }
		else FBLOG("edid: served as read (the firmware's timing is the preferred one and inside the range limits)");
	}
	publishTimingRange(true);
}

// ---- system sleep (phase F) -----------------------------------------------------------------------------------
// The display core registers Linux brings up before any modeset (icl_display_core_init for display 14 / xe2_hpd:
// DC off, PCH reset handshake, power well 1 and the wells in use, CDCLK, DBUF slices, MBUS, BW_BUDDY, CHICKEN_MISC_2),
// then plane 1's DDB and watermarks. Saved before sleep, written back in this order after a wake that lost them
// (the GOP's values: no DMC here, DC states stay off). Pipe-relative entries are marked with bit 0 of the flags.
namespace {
struct CoreReg { uint32_t off; uint8_t pipe, wait; };   // wait: 1 power well pairs (state = request >> 1), 2 PLL lock, 3 DBUF state, 4 PMDEMAND
const CoreReg kCoreRegs[] = {
	{ 0x45504, 0, 0 },   // DC_STATE_EN
	{ 0x46408, 0, 0 },   // HSW_NDE_RSTWRN_OPT (PCH reset handshake)
	{ 0x45400, 0, 1 }, { 0x45404, 0, 1 },   // HSW_PWR_WELL_CTL1 (BIOS) / CTL2 (driver)
	{ 0x46070, 0, 2 },   // BXT_DE_PLL_ENABLE: CDCLK PLL ratio + enable
	{ 0x46008, 0, 0 }, { 0x46000, 0, 0 },   // CDCLK_SQUASH_CTL, CDCLK_CTL
	{ 0x45230, 0, 0 }, { 0x45234, 0, 4 },   // PMDEMAND request dwords (REQ_ENABLE on the second)
	{ 0x45008, 0, 3 }, { 0x44FE8, 0, 3 }, { 0x44300, 0, 3 }, { 0x44304, 0, 3 },   // DBUF_CTL_S0-3
	{ 0x45038, 0, 0 }, { 0x45048, 0, 0 }, { 0x4504C, 0, 0 }, { 0x4438C, 0, 0 },   // MBUS_ABOX0-2_CTL, MBUS_CTL
	{ 0x7003C, 1, 0 },   // PIPE_MBUS_DBOX_CTL
	{ 0x45130, 0, 0 }, { 0x45134, 0, 0 }, { 0x45140, 0, 0 }, { 0x45144, 0, 0 },   // BW_BUDDY0/1 CTL, PAGE_MASK
	{ 0x42084, 0, 0 },   // CHICKEN_MISC_2 (BMG_DARB_HALF_BLK_END_BURST)
	{ 0x7027C, 1, 0 },   // PLANE_BUF_CFG_1 (DDB)
	{ 0x70240, 1, 0 }, { 0x70244, 1, 0 }, { 0x70248, 1, 0 }, { 0x7024C, 1, 0 }, { 0x70250, 1, 0 }, { 0x70254, 1, 0 },
	{ 0x70258, 1, 0 }, { 0x7025C, 1, 0 }, { 0x70268, 1, 0 },   // PLANE_WM_1 levels 0-5, SAGV, SAGV_TRANS, TRANS
};
constexpr int kNumCoreRegs = (int)(sizeof kCoreRegs / sizeof kCoreRegs[0]);
static_assert(kNumCoreRegs <= 48, "core register table");
}

void IntelXeFramebuffer::coreSaveRestore(bool restore)
{
	if (fPipe < 0 || !fRegs) return;
	for (int i = 0; i < kNumCoreRegs; i++) {
		const uint32_t off = kCoreRegs[i].off + (kCoreRegs[i].pipe ? (uint32_t)fPipe * 0x1000u : 0u);
		if (!restore) { fCoreSaved[i] = rd(off); continue; }
		const uint32_t v = fCoreSaved[i];
		switch (kCoreRegs[i].wait) {
		case 4: wr(off, v | (1u << 31)); for (int t = 0; t < 100 && (rd(off) & (1u << 31)); t++) IODelay(100); break;
		default: wr(off, v); break;
		}
		const uint32_t want = kCoreRegs[i].wait == 1 ? (v & 0xAAAAAAAAu) >> 1 : kCoreRegs[i].wait == 2 ? (v & (1u << 31) ? 1u << 30 : 0u)
		                    : kCoreRegs[i].wait == 3 ? (v & (1u << 31) ? 1u << 30 : 0u) : 0u;
		if (want) {
			int t = 0;
			for (; t < 200 && (rd(off) & want) != want; t++) IODelay(50);
			if ((rd(off) & want) != want) FBLOG("resume: 0x%x = 0x%08x did not reach 0x%08x", off, rd(off), want);
		}
	}
	if (!restore) fCoreSavedOk = true;
}

// icl_plane_update_noarm's remaining writes for plane 1 scanning a linear XRGB8888 buffer: skl_plane_keyval/keymsk/keymax
// (no colour key: 0), PLANE_OFFSET (x = y = 0), PLANE_CUS_CTL (plane 1 is an HDR plane; no planar YUV: 0) and
// PLANE_COLOR_CTL (glk_plane_color_ctl). The GOP leaves exactly these, so before v10g nothing wrote them; system sleep
// resets them. Armed by the next PLANE_SURF write (programMode, a flip, IntelXeLink planesOn).
void IntelXeFramebuffer::planeNoarm(int p)
{
	if (p < 0 || !fRegs) return;
	wr(PLANE_KEYVAL(p), 0);
	wr(PLANE_KEYMSK(p), 0);
	wr(PLANE_KEYMAX(p), 0);
	wr(PLANE_OFFSET(p), 0);
	wr(PLANE_CUS_CTL(p), 0);
	wr(PLANE_COLOR_CTL(p), kPlaneColorCtl);
}

// The pipe colour state before the sleep: PIPE_CSC_MODE, the pipe CSC and the bottom colour (the GOP's values; Linux
// computes them from the CRTC state, csc_mode 0 for an RGB output without a CTM, but this board's firmware sets bit 29
// of PIPE_CSC_MODE, which Linux does not define, so the value is kept as found rather than rewritten).
void IntelXeFramebuffer::pipeColorSave()
{
	if (fPipe < 0 || !fRegs) return;
	fCscModeSaved = rd(PIPE_CSC_MODE(fPipe));
	for (int i = 0; i < kNumCscRegs; i++) fCscSaved[i] = rd(kCscRegs[i] + (uint32_t)fPipe * 0x100u);
	fBottomSaved = rd(SKL_BOTTOM_COLOR(fPipe));
	fColorSavedOk = true;
}

// icl_color_commit_noarm/_arm after a wake: the CSC coefficients and offsets (icl_load_csc_matrix), then
// SKL_BOTTOM_COLOR, GAMMA_MODE (loadGammaLocked: the 10-bit LUT NMXAccel loaded, or the firmware's GAMMA_MODE when it
// is off) and PIPE_CSC_MODE, all latched at the next start of vblank. Caller holds dispLock.
void IntelXeFramebuffer::pipeColorCommit()
{
	if (fPipe < 0 || !fRegs || !fColorSavedOk) return;
	for (int i = 0; i < kNumCscRegs; i++) wr(kCscRegs[i] + (uint32_t)fPipe * 0x100u, fCscSaved[i]);
	wr(SKL_BOTTOM_COLOR(fPipe), fBottomSaved);
	loadGammaLocked();
	wr(PIPE_CSC_MODE(fPipe), fCscModeSaved);
}

// The GGTT PTEs of IntelXeFB's own ranges: the firmware surface (contiguous at its DPA), the bound scan-out slots, the
// cursor. IntelXe.kext writes only its own mappings back after resume.
void IntelXeFramebuffer::rewritePtes()
{
	const uint64_t len = (uint64_t)fRowBytes * fHeight;
	for (uint64_t off = 0; off < len; off += 0x1000) pteW(fSurf + (uint32_t)off, (fDpa + off) | fPteFlags);
	for (int i = 0; i < kSlots; i++) if (fSlot[i].used) slotPtes(fSlot[i], kSlotGgtt + (uint32_t)i * kSlotSpan);
	if (fCurReady) for (uint32_t off = 0; off < kCurBytes; off += 0x1000) pteW(kCurGgtt + off, (fCurDpa + off) | fPteFlags);
	if (fHomeBo) for (uint32_t off = 0; off < kHomeMax; off += 0x1000) pteW(kHomeGgtt + off, (fHomeDpa + off) | fPteFlags);
	(void)pte(fSurf);
}

// IntelXe.kext's power events: 1 will suspend (called before its own suspend, display still powered), 2 resumed (its
// GGTT mappings, interrupts and VRAM manager are back), 3/4 GT reset begin/end (the display is not part of it).
void IntelXeFramebuffer::powerEvent(uint32_t event)
{
	if (fStopping || fPipe < 0) return;
	if (event == 1) {
		fSuspends++;
		if (fPmGuardCall) thread_call_cancel(fPmGuardCall);   // a guard from the last resume must not act during this sleep
		dispLock();
		fSysSuspended = true;
		abortPend();
		if (fVrrOn) vrrDisableLocked("system sleep");   // vrrWanted() keeps the wish for the wake
		dispUnlock();
		flipCheck();                                     // the frame in flight completes (aborted) now, not after the wake
		if (fIrqOn) irqRegister(false);                  // registered again after the restore
		cursorPark(true);
		if (fPipeBAllowed && fPipeB >= 0) {   // so that the single-pipe state is what gets saved
			if (fPipeBLock) IOLockLock(fPipeBLock);
			(void)pipeBOff("system sleep", false);   // the framebuffer stays connected over the sleep (pipeBWork decides after it)
			fPipeBWanted = true;
			if (fPipeBLock) IOLockUnlock(fPipeBLock);
		}
		coreSaveRestore(false);
		pipeColorSave();
		const int s = linkAction(IntelXeLink::kSave);
		const int d = linkAction(IntelXeLink::kPowerDown);   // sink to D3 and the output off cleanly, as Linux's suspend
		FBLOG("system sleep: display state saved (%d), output %s (%d)", s, d ? "NOT powered down" : "powered down", d);
		return;
	}
	if (event == 2) { if (fResumeCall) thread_call_enter(fResumeCall); return; }
	if (event == 3 || event == 4) FBLOG("GT reset %s (display untouched)", event == 3 ? "begins" : "done");
}

// After the wake, off IntelXe.kext's thread: PTEs, the display core if it lost power, a full modeset from the saved
// state, then the plane, cursor, gamma, interrupts and VRR as they were.
void IntelXeFramebuffer::resumeDisplay()
{
	if (fStopping || fPipe < 0) return;
	fResumes++;
	const bool coreLost = !(rd(0x46070) & (1u << 30));   // the CDCLK PLL is no longer locked: the display lost power
	rewritePtes();
	if (coreLost && fCoreSavedOk) coreSaveRestore(true);
	// What D3cold left in the plane and pipe colour registers (the power wells are back, nothing has written them yet).
	// Linux never meets this state: intel_display_driver_resume replays the whole atomic state, and every plane update
	// writes PLANE_COLOR_CTL (icl_plane_update_noarm) and every colour commit GAMMA_MODE/PIPE_CSC_MODE (icl_color_commit_
	// arm). v10e restored neither: after the 14:12 wake the plane ran with its reset PLANE_COLOR_CTL, i.e. with the
	// plane's post-CSC gamma on and an unloaded LUT (a whitish, noisy picture in which the desktop still moved).
	fWakeColorCtl = rd(PLANE_COLOR_CTL(fPipe)); fWakeCscMode = rd(PIPE_CSC_MODE(fPipe));
	fWakeGammaMode = rd(GAMMA_MODE(fPipe)); fWakePlaneCtl = rd(PLANE_CTL(fPipe));
	FBLOG("resume: colour registers after the wake: PLANE_COLOR_CTL 0x%08x (0x%08x wanted), PIPE_CSC_MODE 0x%08x "
	      "(0x%08x before the sleep), GAMMA_MODE 0x%08x, PLANE_CTL 0x%08x", fWakeColorCtl, kPlaneColorCtl, fWakeCscMode,
	      fCscModeSaved, fWakeGammaMode, fWakePlaneCtl);
	// before the restore modeset turns plane 1 on (IntelXeLink planesOn arms it with PLANE_SURF), so its first frame
	// already goes through the right colour path. The pipe's colour state follows the modeset (pipeColorCommit below):
	// Linux loads LUTs only with the pipe's clocks running (intel_color_modeset in hsw_crtc_enable).
	dispLock();
	planeNoarm(fPipe);
	dispUnlock();
	int r = linkAction(IntelXeLink::kRestore);
	if (r) { FBLOG("resume: restore modeset failed (%d), once more", r); IOSleep(200); r = linkAction(IntelXeLink::kRestore); }
	if (r) fResumeFails++;
	else fDpmsDown = false;                          // the restore modeset also brought a DPMS-off output back
	dispLock();
	// plane registers, scaler, surface home; the plane's enable stays as display sleep left it (blank(false) on the
	// display's own wake turns it on)
	programMode(fModeW, fModeH);   // writes the plane's colour registers again (planeNoarm)
	pipeColorCommit();             // icl_color_commit_arm: bottom colour, CSC, GAMMA_MODE (and the LUT when it is on)
	if (fCurReady) {
		IOLockLock(fCurImgLock); cursorRender(); IOLockUnlock(fCurImgLock);   // VRAM outside the kernel's buffers is gone
		fCurWm = false; fCurCtl = ~0u;
	}
	applyVrrLocked("resume");
	fSysSuspended = false;
	dispUnlock();
	cursorPark(false);
	(void)irqRegister(true);
	FBLOG("resume: display %s (core %s, modeset %d)", r ? "NOT restored -- debug.intelxefb_link=6 to retry" : "restored",
	      coreLost ? (fCoreSavedOk ? "restored" : "LOST, nothing saved") : "kept power", r);
	// IOGraphics' power work brings pagingState back when this framebuffer leaves power state 0 (powerApply); if a wake
	// ever does not take that path (a sleep in which the framebuffer never reached 0), the guard notices in 5 s.
	pmGuardArm();
	if (fPipeBAllowed && fPipeBWanted && fPipeBCall) thread_call_enter(fPipeBCall);   // the second pipe after the first
}

bool IntelXeFramebuffer::powerRegister(bool on)
{
	struct { uint32_t version; void (*handler)(void *, uint32_t); void *ctx; } r = { 1, on ? powerThunk : nullptr, this };
	const OSSymbol *sym = OSSymbol::withCString("intelxePowerEvent");
	IOService *b = intelxe();
	const IOReturn rr = (b && sym) ? b->callPlatformFunction(sym, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
	if (sym) sym->release();
	if (on) FBLOG("system sleep: %s", rr == kIOReturnSuccess ? "power events registered with IntelXe.kext" : "IntelXe.kext sends no power events (no restore after sleep)");
	return rr == kIOReturnSuccess;
}

IOService *IntelXeFramebuffer::probe(IOService *provider, SInt32 *score)
{
	IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return nullptr;
	if (bootArg("-intelxe_nofb") || bootArg("-intelxefb_off")) { FBLOG("off by boot-arg"); return nullptr; }
	if (bootArg("-x")) { FBLOG("safe mode: not starting"); return nullptr; }
	return IOFramebuffer::probe(provider, score);
}

// ---- boot guard -----------------------------------------------------------------------------------------------------
// The previous boot's intelxe-guard decides this one:
//   "accel<n>" (plain "accel" = 1) n accelerated boots in a row ended inside their first minute. One may be a power cut,
//            so the next boot tries again; from kGuardAccelSafe it arms with conservative settings; from kGuardAccelHold
//            intelxe-accel-off is written (persistent): this boot and later ones stay unaccelerated (IntelXeFB only) until
//            `sudo nvram intelxe-accel-off=ok` ("ok" counts as absent; `nvram -d` does not remove it on macOS 26).
//   "fb<n>"  n boots in a row ended before the unaccelerated desktop ran for a minute: logged and published only.
//            The guard never switches IntelXeFB off by itself; its kill switches are -intelxe_nofb and safe mode.
// Acceleration is also held by -intelxe_noaccel and safe mode. NMXAGDC and NMXAccel arm only when IntelXeFBAccelAllowed is
// true, which is published before the framebuffer is. The rules are in IntelXe/uapi/intelxe_bootguard.h.
// A held boot normally does not get here: IntelXe.kext reads the same values first and gives the display to Apple's
// IONDRVFramebuffer on the firmware's surface (IntelXe::bootGuardHandOff), which takes the IOFramebuffer match category
// before IntelXeReady offers the GPU to this driver. IntelXeFB starts on a held boot only when that hand-off is turned
// off (intelxe-held-display=fb) or failed, or with an IntelXe.kext older than 0.1.5.
static const char kGuardKey[] = "intelxe-guard", kAccelOffKey[] = "intelxe-accel-off";
// accelerated boots in a row that ended inside their first minute: from kGuardAccelSafe the next boot arms with NMXAccel's
// most conservative settings (no async swaps, no flips, one frame at a time: IntelXeFBAccelSafe), from kGuardAccelHold
// acceleration is held. On macOS 26 WindowServer does not draw on this framebuffer without the accelerator's display pipe
// (a held boot on IntelXeFB stays at the Apple logo), hence the firmware framebuffer above, and holding is the last step.
using intelxeguard::kGuardAccelSafe;
using intelxeguard::kGuardAccelHold;
static uint64_t absNs(uint64_t ns) { uint64_t a; nanoseconds_to_absolutetime(ns, &a); return a; }

void IntelXeFramebuffer::guardBegin()
{
	char prev[32], off[96];
	// IODTNVRAM is long up when the AuxKC matches; never wait more than 2 s for it in start()
	if (OSDictionary *m = IOService::serviceMatching("IODTNVRAM")) {
		IOService *nv = IOService::waitForMatchingService(m, 2000000000ull);
		m->release();
		if (!nv) { FBLOG("guard: no IODTNVRAM after 2 s: no guard this boot, acceleration allowed"); fAccelAllowed = !bootArg("-intelxe_noaccel") && !bootArg("-x"); setProperty("IntelXeFBAccelAllowed", fAccelAllowed); return; }
		nv->release();
	}
	// the rules are intelxe_bootguard.h's (IntelXe.kext reads the same values at its start, for the firmware framebuffer)
	const bool had = nvramRead(kGuardKey, prev, sizeof prev) && intelxeguard::isSet(prev);
	if (!nvramRead(kAccelOffKey, off, sizeof off)) off[0] = 0;
	if (had) setProperty("IntelXeFBGuardPrev", prev);
	const intelxeguard::Decision dec = intelxeguard::decide(had ? prev : "", off, bootArg("-intelxe_noaccel"), bootArg("-x"));
	fGuardAccelFails = dec.accelFails;
	fAccelSafe = dec.safe;
	if (dec.writeAccelOff) {
		snprintf(off, sizeof off, "%u accelerated boots in a row ended inside their first minute", fGuardAccelFails);
		nvramWrite(kAccelOffKey, off);
		FBLOG("guard: %s: acceleration OFF until `sudo nvram %s=ok`", off, kAccelOffKey);
	} else if (dec.accelFails >= kGuardAccelSafe) {
		FBLOG("guard: %u accelerated boots in a row ended inside their first minute: this boot arms with the safe settings "
		      "(held from %u)", fGuardAccelFails, kGuardAccelHold);
	} else if (dec.accelFails) FBLOG("guard: the last accelerated boot ended inside its first minute (1 of %u before the safe settings): trying again", kGuardAccelSafe);
	const unsigned fails = dec.fbFails;
	if (fails) FBLOG("guard: %u boot(s) in a row ended before the framebuffer desktop ran for a minute (intelxe-guard %s)", fails, prev);
	const char *held = dec.heldBy == intelxeguard::kHeldByBootArg ? "boot-arg -intelxe_noaccel"
	                 : dec.heldBy == intelxeguard::kHeldBySafeMode ? "safe mode" : dec.accelAllowed ? nullptr : off;
	fAccelAllowed = dec.accelAllowed;
	setProperty("IntelXeFBAccelAllowed", fAccelAllowed);
	setProperty("IntelXeFBAccelSafe", fAccelAllowed && fAccelSafe);
	setProperty("IntelXeFBGuardState", intelxeguard::stateName(dec.state));
	if (held) { setProperty("IntelXeFBAccelHeld", held); FBLOG("guard: acceleration held: %s", held); }
	char v[16];
	snprintf(v, sizeof v, "fb%u", fails + 1);
	nvramWrite(kGuardKey, v);
	fGuardLock = IOLockAlloc();
	fGuardStage = 1;
	fGuardT0 = mach_absolute_time();
	setProperty("IntelXeFBGuard", v);
	fGuardCall = thread_call_allocate(guardFire, this);
	if (fGuardCall) thread_call_enter_delayed(fGuardCall, fGuardT0 + absNs(60000000000ull));
}

// the start failed after guardBegin for a reason that is not a crash (no monitor, a BAR that will not map): nothing to hold
static void guardAbandon() { nvramWrite(kGuardKey, nullptr); }

IOReturn IntelXeFramebuffer::guardAccel()
{
	if (!fAccelAllowed) return kIOReturnNotPermitted;
	if (fGuardLock) IOLockLock(fGuardLock);
	fGuardStage = 2;
	fAccelT0 = mach_absolute_time();
	fScanoutsAtAccel = fScanouts;
	char v[16];
	snprintf(v, sizeof v, "accel%u", fGuardAccelFails + 1);
	nvramWrite(kGuardKey, v);
	setProperty("IntelXeFBGuard", v);
	if (fGuardLock) IOLockUnlock(fGuardLock);
	FBLOG("guard: acceleration armed: intelxe-guard=accel%u until the accelerated desktop has run for a minute", fGuardAccelFails + 1);
	if (fGuardCall) thread_call_enter_delayed(fGuardCall, fAccelT0 + absNs(60000000000ull));
	return kIOReturnSuccess;
}

// Healthy needs WindowServer itself, not only VBL clients (NMXAccel's display pipe registers those on its own, v10c):
//  - WindowServer has opened the framebuffer's server connection, the last time at least 45 s ago (one that crashes in
//    a loop opens it again and again);
//  - VBL clients without one coming or going for 45 s, and the stage a minute old;
//  - an accelerated stage: at least kGuardFrames NMXAccel frame calls (flips and copies) since it was armed - WindowServer
//    presenting through the accelerator, not the single call a pipe makes on its own.
// At a clean shutdown (WindowServer has already closed it) a steady stretch of 20 s at any time is enough with the same
// WindowServer and frame conditions: a WindowServer that crashed in a loop, or never started, never had one.
static constexpr uint32_t kGuardFrames = 120;
void IntelXeFramebuffer::guardCheck(bool shutdown)
{
	if (!shutdown && !fCrumbRead) {   // the last shutdown's breadcrumb: read and cleared here, never in start()
		fCrumbRead = true;
		char c[64];
		if (nvramRead("intelxefb-shutdown", c, sizeof c) && strcmp(c, "ok")) {
			setProperty("IntelXeFBLastShutdown", c);
			FBLOG("last shutdown reached: %s", c);
			nvramWrite("intelxefb-shutdown", nullptr);
		} else setProperty("IntelXeFBLastShutdown", "no breadcrumb (no clean shutdown reached IntelXeFB, or the first boot with it)");
	}
	if (!fGuardLock) return;
	IOLockLock(fGuardLock);
	if (!fGuardStage) { IOLockUnlock(fGuardLock); return; }
	const uint64_t now = mach_absolute_time();
	const uint64_t stable = vblClients() > 0 && fVblEvtAt ? now - fVblEvtAt : 0;
	const uint32_t accelFrames = fScanouts - fScanoutsAtAccel;
	const bool frames = fGuardStage != 2 || accelFrames >= kGuardFrames;
	const bool ws = fWsOpens > 0;
	bool ok;
	if (shutdown) ok = ws && frames && (stable > fVblStableMax ? stable : fVblStableMax) >= absNs(20000000000ull);
	else ok = ws && frames && now - fWsOpenAt >= absNs(45000000000ull) && stable >= absNs(45000000000ull) &&
	          now - (fGuardStage == 2 ? fAccelT0 : fGuardT0) >= absNs(60000000000ull);
	if (!ok && !shutdown) {   // every 15 s while not healthy; said with the reasons from the second minute on, every minute
		static uint32_t checks;
		if (++checks >= 4 && !(checks % 4))
			FBLOG("guard: not healthy yet (stage %d): WindowServer connections %u%s, VBL clients %d steady %llu s, accelerated "
			      "frame calls %u of %u", fGuardStage, fWsOpens, fWsActive ? " (active)" : "", vblClients(),
			      (unsigned long long)(stable / absNs(1000000000ull)), accelFrames, kGuardFrames);
	}
	if (ok) {
		const int st = fGuardStage;
		fGuardStage = 0;
		nvramWrite(kGuardKey, nullptr);
		setProperty("IntelXeFBGuard", shutdown ? "cleared at a clean shutdown" : "cleared: healthy");
		IOLockUnlock(fGuardLock);
		FBLOG("guard: %s desktop healthy%s: intelxe-guard cleared (WindowServer connections %u%s, %u accelerated frame calls)",
		      st == 2 ? "accelerated" : "framebuffer", shutdown ? " (clean shutdown)" : "", fWsOpens, fWsActive ? ", active" : "",
		      st == 2 ? accelFrames : 0);
		return;
	}
	IOLockUnlock(fGuardLock);
	if (shutdown) FBLOG("guard: shutdown before the desktop was steady: intelxe-guard stays (stage %d)", fGuardStage);
	else if (fGuardCall && !fStopping) thread_call_enter_delayed(fGuardCall, now + absNs(15000000000ull));
}

bool IntelXeFramebuffer::adoptFirmwareMode(IOPhysicalAddress barBase, IOPhysicalLength barLen)
{
	for (int p = 0; p < 4; p++) {
		uint32_t conf = rd(TRANSCONF(p)), ctl = rd(PLANE_CTL(p));
		if (!(conf & kEnable) || !(ctl & kEnable)) continue;
		if (fmtOf(ctl) != 8) { FBLOG("pipe %c: plane format %u is not XRGB8888", 'A' + p, fmtOf(ctl)); return false; }
		if (tileOf(ctl) != 0) { FBLOG("pipe %c: plane is tiled (%u)", 'A' + p, tileOf(ctl)); return false; }
		uint32_t size = rd(PLANE_SIZE(p)), stride = rd(PLANE_STRIDE(p)) & 0xFFF;   // PLANE_STRIDE__MASK 11:0
		fWidth = (size & 0xFFFF) + 1; fHeight = ((size >> 16) & 0xFFFF) + 1;
		fRowBytes = stride * 64;                     // linear planes count the stride in 64-byte units
		fModeW = fWidth; fModeH = fHeight; fModeRB = fRowBytes; fCurSpaceW = fWidth; fCurSpaceH = fHeight;
		fSurf = rd(PLANE_SURF(p)) & ~0xFFFu;         // GGTT address of the surface
		if (fWidth < 320 || fHeight < 200 || fRowBytes < fWidth * 4) {
			FBLOG("pipe %c: plane %ux%u stride %u is not a usable surface", 'A' + p, fWidth, fHeight, fRowBytes); return false;
		}
		// the GOP surface must be one VRAM run inside the CPU-visible BAR2 window (PTE bit 1 = local memory on Xe2 dGPU,
		// the GOP writes plain 0x3)
		uint64_t len = (uint64_t)fRowBytes * fHeight, first = pte(fSurf);
		if (!(first & 1)) { FBLOG("pipe %c: GGTT 0x%x is not mapped (pte 0x%llx)", 'A' + p, fSurf, first); return false; }
		fDpa = first & kPteAddr;
		for (uint64_t off = 0x1000; off < len; off += 0x1000) {
			uint64_t e = pte(fSurf + (uint32_t)off);
			if ((e & kPteAddr) != fDpa + off || !(e & 1)) {
				FBLOG("pipe %c: surface not contiguous at +0x%llx (pte 0x%llx, expected 0x%llx)", 'A' + p, off, e, fDpa + off);
				return false;
			}
		}
		if (fDpa + len > barLen) {
			FBLOG("pipe %c: surface at VRAM 0x%llx is past the %llu MiB BAR2 window", 'A' + p, fDpa, (unsigned long long)(barLen >> 20));
			return false;
		}
		fSurfPhys = barBase + fDpa;
		fBarBase = barBase; fBarLen = barLen;
		fPteFlags = first & ~kPteAddr;
		fHT = (rd(HTOTAL(p)) >> 16) + 1; fVT = (rd(VTOTAL(p)) >> 16) + 1; fModeVT = fVT;
		fHSS = (rd(HSYNC(p)) & 0xFFFF) + 1; fHSE = (rd(HSYNC(p)) >> 16) + 1;
		fVSS = (rd(VSYNC(p)) & 0xFFFF) + 1; fVSE = (rd(VSYNC(p)) >> 16) + 1;
		// The exact pixel clock from the link: port clock x LINK_M / LINK_N (intel_link_compute_m_n; M/N is the pixel clock
		// over the link symbol clock, so above 1 for fast modes: 706.25 / 540 MHz on the G5). When it gives a
		// plausible refresh, no vblanks are counted: the 250 ms count cost every boot a quarter second before this
		// framebuffer published (it now races the iGPU's framebuffers for WindowServer's first GPU registration).
		uint32_t frames = 0; uint64_t ns = 0;
		const uint32_t sel = (rd(TRANS_DDI_FUNC_CTL(p)) >> 27) & 0xF;
		const uint32_t vf = sel ? rd(DDI_CLK_VALFREQ((int)sel - 1)) : 0;
		const uint32_t lm = rd(TRANS_LINK_M1(p)) & 0xFFFFFF, ln = rd(TRANS_LINK_N1(p)) & 0xFFFFFF;
		const uint64_t exact = (vf >= 100000 && vf <= 2000000 && lm && ln && lm < 8 * ln) ? (uint64_t)vf * 1000 * lm / ln : 0;
		const uint64_t exactMhz = exact ? exact * 1000 / ((uint64_t)fHT * fVT) : 0;   // refresh in milli-Hz
		if (exactMhz >= 23000 && exactMhz <= 500000) {
			fPclk = exact;
		} else {
			// refresh: count vblanks for 250 ms; fall back to 60 Hz if the counter does not move
			uint32_t f0 = rd(FRMCOUNT(p)); uint64_t t0 = mach_absolute_time();
			IOSleep(250);
			uint32_t f1 = rd(FRMCOUNT(p));
			absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns);
			frames = f1 - f0;
			if (frames >= 5 && frames <= 100 && ns) {
				uint64_t mhz = (uint64_t)frames * 1000000000000ull / ns;   // refresh in milli-Hz
				fPclk = (uint64_t)fHT * fVT * mhz / 1000;
			} else fPclk = (uint64_t)fHT * fVT * 60;
			if (exact && exact > fPclk * 97 / 100 && exact < fPclk * 103 / 100) fPclk = exact;
		}
		fRefreshMilliHz = (uint32_t)(fPclk * 1000 / ((uint64_t)fHT * fVT));
		fRefresh16 = (uint32_t)((fPclk << 16) / ((uint64_t)fHT * fVT));
		fFrameNs = (uint64_t)fHT * fVT * 1000000000ull / fPclk;
		fDataMBoot = rd(TRANS_DATA_M1(p)); fDataNBoot = rd(TRANS_DATA_N1(p)) & 0xFFFFFF;
		fLinkMBoot = lm; fLinkNBoot = ln; fLinetimeBoot = rd(WM_LINETIME(p));
		fPclkCur = fPclk;
		fPipe = p;
		char how[48];
		if (ns) snprintf(how, sizeof how, "%u vblanks in %llu ms", frames, (unsigned long long)(ns / 1000000));
		else strlcpy(how, "the link's pixel clock", sizeof how);
		FBLOG("pipe %c: %ux%u stride %u GGTT 0x%x -> VRAM 0x%llx (phys 0x%llx), totals %ux%u, %s -> %u.%02u Hz",
		      'A' + p, fWidth, fHeight, fRowBytes, fSurf, fDpa, (unsigned long long)fSurfPhys, fHT, fVT, how,
		      fRefresh16 >> 16, ((fRefresh16 & 0xFFFF) * 100) >> 16);
		return true;
	}
	FBLOG("no pipe is scanning out (no monitor on the B580 at boot?)");
	return false;
}

bool IntelXeFramebuffer::start(IOService *provider)
{
	fPCI = OSDynamicCast(IOPCIDevice, provider);
	if (!fPCI) return false;
	char dummy[8];
	if (intelxeBootArg("IntelXeFB", "-intelxefb_noscale", dummy, sizeof(dummy))) { fScaling = false; FBLOG("scaled modes off by boot-arg"); }
	if (intelxeBootArg("IntelXeFB", "-intelxefb_norates", dummy, sizeof(dummy))) { fRates = false; FBLOG("other refresh rates off by boot-arg"); }
	if (intelxeBootArg("IntelXeFB", "-intelxefb_edidonly", dummy, sizeof(dummy))) { fCommonRates = false; FBLOG("VRR common rates off by boot-arg"); }
	if (intelxeBootArg("IntelXeFB", "-intelxefb_nogamma", dummy, sizeof(dummy))) { fGammaAllowed = false; FBLOG("pipe gamma off by boot-arg"); }
	if (intelxeBootArg("IntelXeFB", "-intelxefb_novrr", dummy, sizeof(dummy))) { fVrrAllowed = false; FBLOG("VRR off by boot-arg"); }
	if (intelxeBootArg("IntelXeFB", "-intelxefb_nohwcursor", dummy, sizeof(dummy))) { fCurAllowed = false; FBLOG("hardware cursor off by boot-arg"); }
	{
		char nv[8];
		if (intelxeBootArg("IntelXeFB", "-intelxefb_pipeb", dummy, sizeof(dummy)) || (nvramRead("intelxefb-pipeb", nv, sizeof nv) && nv[0] == '1')) {
			fPipeBAllowed = true;
			FBLOG("second pipe: on (-intelxefb_pipeb / intelxefb-pipeb=1) -- untested, needs a second monitor");
		}
	}
	int vrrBoot = gVrrDefault;
	if (intelxeBootArg("IntelXeFB", "intelxefb_vrr", &vrrBoot, sizeof(vrrBoot))) FBLOG("intelxefb_vrr=%d by boot-arg", vrrBoot);
	fPCI->setMemoryEnable(true);
	fMMIO = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0);
	IODeviceMemory *ap = fPCI->getDeviceMemoryWithRegister(kIOPCIConfigBaseAddress2);
	if (!fMMIO || !ap || fMMIO->getLength() < kGsm + 0x800000) { FBLOG("BAR0 or BAR2 is not available"); OSSafeReleaseNULL(fMMIO); return false; }
	fRegs = (volatile uint8_t *)fMMIO->getVirtualAddress();
	if (!adoptFirmwareMode(ap->getPhysicalAddress(), ap->getLength())) { OSSafeReleaseNULL(fMMIO); fRegs = nullptr; return false; }
	guardBegin();   // before anything that could take the machine down; published before the framebuffer is
	probeEdid();
	parseEdid();
	vrrProbeCaps();
	buildModes();
	(void)ggttReserve(fSurf, (uint64_t)fRowBytes * fHeight, "the firmware's surface");
	fSlotsOwned = ggttReserve(kSlotGgtt, (uint64_t)kSlotSpan * kSlots, "the scan-out slots");
	fSlotsOk = fSlotsOwned >= 0;
	fCurOwned = ggttReserve(kCurGgtt, kCurBytes, "the cursor");
	if (fCurOwned < 0) fCurAllowed = false;
	fHomeOwned = ggttReserve(kHomeGgtt, kHomeMax, "the HiDPI home surface");
	{
		// CDCLK_CTL bits 10:0, the CD frequency decimal: (MHz - 1) x 2 in u10.1 (intel_cdclk.c bxt_set_cdclk)
		const uint32_t cd = rd(0x46000) & 0x7FF;
		fCdclkKhz = cd ? (cd + 2) * 500 : 0;
		FBLOG("cdclk: %u kHz (CDCLK_CTL 0x%x)", fCdclkKhz, cd);
	}
	{
		int v = 0;
		if (intelxeBootArg("IntelXeFB", "intelxefb_hidpi", &v, sizeof v)) gHidpi = v;
		if (IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane)) {
			if (OSObject *o = opt->copyProperty("intelxefb-hidpi")) {
				OSData *d = OSDynamicCast(OSData, o); OSString *str = OSDynamicCast(OSString, o);
				const char c = d && d->getLength() ? *(const char *)d->getBytesNoCopy() : str && str->getLength() ? str->getCStringNoCopy()[0] : '0';
				gHidpi = c == '1';
				o->release();
			}
			opt->release();
		}
	}
	publishScalerInfo();
	{
		// IntelXe Manager's display settings, kept in NVRAM (the sysctls of the same names change them until the next boot):
		// intelxefb-hwcursor 0/1, intelxefb-dpms 0/1 (display sleep powers the output down), intelxefb-vrr-lfc 0/1 and
		// intelxefb-vrr-floor (Hz, 0 = the panel's). "ok" or anything else not a number counts as absent.
		auto nvNum = [](const char *key, long lo, long hi, long *out) {
			char b[16];
			if (!nvramRead(key, b, sizeof b) || b[0] < '0' || b[0] > '9') return false;
			long v = 0; for (const char *p = b; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
			if (v < lo || v > hi) return false;
			*out = v; return true;
		};
		long v = 0;
		if (nvNum("intelxefb-hwcursor", 0, 1, &v)) gHwCursor = (int)v;
		if (nvNum("intelxefb-dpms", 0, 1, &v)) gDpms = (int)v;
		if (nvNum("intelxefb-vrr-lfc", 0, 1, &v)) gVrrLfc = (int)v;
		if (nvNum("intelxefb-vrr-floor", 0, 500, &v)) gVrrFloorHz = (uint32_t)v;
	}
	cursorInit();
	{
		// macOS Adaptive-Sync modes, on unless boot-arg intelxefb_vrrmodes=0 or NVRAM intelxefb-vrr-modes=0 (the EDID change needs it at start)
		int v = 0;
		if (intelxeBootArg("IntelXeFB", "intelxefb_vrrmodes", &v, sizeof v)) gVrrModes = v;
		if (IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane)) {
			if (OSObject *o = opt->copyProperty("intelxefb-vrr-modes")) {
				OSData *d = OSDynamicCast(OSData, o); OSString *str = OSDynamicCast(OSString, o);
				const char c = d && d->getLength() ? *(const char *)d->getBytesNoCopy() : str && str->getLength() ? str->getCStringNoCopy()[0] : '0';
				gVrrModes = c == '1';
				o->release();
			}
			opt->release();
		}
		vrrModesInit();
	}
	uint32_t since = usSinceVblank();
	fVblLocked = since != ~0u;
	if (fVblLocked) FBLOG("vbl: locked to pipe %c vblank timestamps (%u us since the last one)", 'A' + fPipe, since);
	else FBLOG("vbl: frame timestamps not live (ctr 0x%x stamp 0x%x): free-running timer", rd(TIMESTAMP_CTR), rd(FRMTMSTMP(fPipe)));
	// IOFramebuffer::start creates the framebuffer's controller only when IOFBDependentID is set (copyController,
	// kDepIDCreate); without one it stays NULL until open() and the first power callback (powerStateWillChangeTo,
	// FBLOCK) dereferenced it: the 07:15 boot panic. Any ID unique to this GPU will do; index 0 = the only head.
	setProperty("IOFBDependentID", 0x4235383046420001ull, 64);   // "IntelXeFB" 0001
	setProperty("IOFBDependentIndex", 0ull, 32);
	fDispLock = IOLockAlloc();
	fHpdImrLock = IOSimpleLockAlloc();
	fFlipLock = IOSimpleLockAlloc();
	nanoseconds_to_absolutetime(100000000ull, &fPend100ms);
	if (!IOFramebuffer::start(provider)) {
		guardAbandon(); fGuardStage = 0;
		if (fGuardCall) { thread_call_cancel(fGuardCall); while (!thread_call_free(fGuardCall)) { thread_call_cancel(fGuardCall); IOSleep(5); } fGuardCall = nullptr; }
		OSSafeReleaseNULL(fMMIO); fRegs = nullptr;
		if (fDispLock) { IOLockFree(fDispLock); fDispLock = nullptr; }
		if (fFlipLock) { IOSimpleLockFree(fFlipLock); fFlipLock = nullptr; }
		if (fHpdImrLock) { IOSimpleLockFree(fHpdImrLock); fHpdImrLock = nullptr; }
		return false;
	}
	// SSDT-ARCOFF gives the GPU class-code ff ff ff ff so that no framebuffer driver takes it at boot. IOPresentment
	// (ioPresentmentGetNodeMap) only counts a GPU whose class-code is a display class: with ffffffff WindowServer's
	// display-pipe map stayed empty and an accelerated desktop never got a frame. Now that this framebuffer holds the
	// IOFramebuffer match category, the real class (VGA, 03 00 00) goes back on the nub.
	// The mode for AppleGraphicsDeviceControl's link-configuration answer (NMXAGDC K5, command 0x921, which IOPresentment
	// needs before it hands WindowServer any display-pipe capabilities: "Unable to acquire capabilities ... 0x80e").
	// nullmoth's NVRMTiming layout: tag, pixel clock, h visible/sync start/sync end/total, the same vertically, refresh
	// in mHz, h and v sync positive. Sync polarity: TRANS_DDI_FUNC_CTL bits 16 (h) and 17 (v), Linux TRANS_DDI_PHSYNC/PVSYNC.
	publishTiming();
	static const uint8_t kVga[4] = { 0x00, 0x00, 0x03, 0x00 };
	if (OSData *cc = OSData::withBytes(kVga, sizeof kVga)) { fPCI->setProperty("class-code", cc); cc->release(); }
	setProperty("IntelXeFBPipe", (unsigned long long)fPipe, 32);
	fGammaModeBoot = rd(GAMMA_MODE(fPipe));
	FBLOG("pipe %c GAMMA_MODE 0x%08x at start", 'A' + fPipe, fGammaModeBoot);
	pipeColorSave();
	FBLOG("pipe %c colour at start: PLANE_COLOR_CTL 0x%08x (Linux 0x%08x), PIPE_CSC_MODE 0x%08x, SKL_BOTTOM_COLOR 0x%08x",
	      'A' + fPipe, rd(PLANE_COLOR_CTL(fPipe)), kPlaneColorCtl, fCscModeSaved, fBottomSaved);
	// NMXAGDC offers the pipe gamma to IOPresentment only when this is published (debug.nmxagdc_gamma=1 as well)
	if (fGammaAllowed) setProperty("IntelXeFBPipeGammaSize", (unsigned long long)kGammaSize, 32);
	setProperty("IntelXeFBVram", fDpa, 64);
	setProperty("IntelXeFBScanoutVersion", 3ull, 32);   // NMXScanout 2 (adaptive-sync window) and 3 (DPA runs) understood
	publishFlipWait();
	if (IODeviceMemory *sm = IODeviceMemory::withRange(fSurfPhys, (IOPhysicalLength)fRowBytes * fHeight)) {
		fSurfMap = sm->map(kIOMapWriteCombineCache); sm->release();
	}
	if (!fSurfMap) FBLOG("the surface would not map write-combined: no accelerated frames for this display");
	armVbl();
	fHpdCall = thread_call_allocate(hpdFire, this);                    // before the hotplug interrupt is unmasked
	fHpdReenableCall = thread_call_allocate(hpdReenableFire, this);
	if (!intelxeBootArg("IntelXeFB", "-intelxefb_noirq", dummy, sizeof(dummy))) irqRegister(true);
	else FBLOG("display irq: off by boot-arg");
	fResumeCall = thread_call_allocate(resumeFire, this);
	coreSaveRestore(false);   // a baseline in case the first sleep comes without a will-suspend event
	(void)powerRegister(true);
	fReplug = thread_call_allocate(replugFire, this);
	// IOGraphics' power protocol (powerApply): command gates for the gated wake work, the notification counter, the
	// resume guard and the debug.intelxefb_pm test; then IONDRV's power registration and its doze floor.
	if (IOWorkLoop *sys = getGraphicsSystemWorkLoop()) {
		fSysGate = IOCommandGate::commandGate(this);
		if (fSysGate && sys->addEventSource(fSysGate) != kIOReturnSuccess) OSSafeReleaseNULL(fSysGate);
	}
	if (IOWorkLoop *fc = getControllerWorkLoop()) {
		fFcGate = IOCommandGate::commandGate(this);
		if (fFcGate && fc->addEventSource(fFcGate) != kIOReturnSuccess) OSSafeReleaseNULL(fFcGate);
	}
	fPmTap = addFramebufferNotification(&IntelXeFramebuffer::pmTap, this, nullptr);
	fPmGuardCall = thread_call_allocate(pmGuardFire, this);
	fPmTestCall = thread_call_allocate(pmTestFire, this);
	FBLOG("power: IOGraphics' protocol (3 states, doze floor), gates %s, notification counter %s",
	      fSysGate && fFcGate ? "ok" : "MISSING (no recovery)", fPmTap ? "on" : "MISSING");
	registerPowerDriver(this, kPowerStates, 3);
	changePowerStateTo(1);   // IONDRV: not below doze until the power domain goes (system sleep)
	gThumbFB = this; sysctl_register_oid(&sysctl__debug_intelxefb_thumb); sysctl_register_oid(&sysctl__debug_intelxefb_peek);
	sysctl_register_oid(&sysctl__debug_intelxefb_mode);
	sysctl_register_oid(&sysctl__debug_intelxefb_cropxy); sysctl_register_oid(&sysctl__debug_intelxefb_crop);
	sysctl_register_oid(&sysctl__debug_intelxefb_link); sysctl_register_oid(&sysctl__debug_intelxefb_linklog);
	sysctl_register_oid(&sysctl__debug_intelxefb_poke); sysctl_register_oid(&sysctl__debug_intelxefb_dpcd);
	sysctl_register_oid(&sysctl__debug_intelxefb_vrr); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_vbl);
	sysctl_register_oid(&sysctl__debug_intelxefb_vrr_tick_us); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_lfc); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_pace_lo_pct); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_pace_hi_pct);
	sysctl_register_oid(&sysctl__debug_intelxefb_vrrlog_on); sysctl_register_oid(&sysctl__debug_intelxefb_vrrlog);
	sysctl_register_oid(&sysctl__debug_intelxefb_vrrinfo); sysctl_register_oid(&sysctl__debug_intelxefb_vbl_precise); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_floor_hz); sysctl_register_oid(&sysctl__debug_intelxefb_dpms); sysctl_register_oid(&sysctl__debug_intelxefb_hwcursor); sysctl_register_oid(&sysctl__debug_intelxefb_vrr_modes); sysctl_register_oid(&sysctl__debug_intelxefb_irq);
	sysctl_register_oid(&sysctl__debug_intelxefb_pm); sysctl_register_oid(&sysctl__debug_intelxefb_pminfo); sysctl_register_oid(&sysctl__debug_intelxefb_pm_guard);
	sysctl_register_oid(&sysctl__debug_intelxefb_hpd); sysctl_register_oid(&sysctl__debug_intelxefb_hpdinfo);
	sysctl_register_oid(&sysctl__debug_intelxefb_pipeb_mode); sysctl_register_oid(&sysctl__debug_intelxefb_pipeb_plan);   // read only
	if (fPipeBAllowed) {   // the first connector scan (and the second pipe, if a sink is there) once this one is up
		sysctl_register_oid(&sysctl__debug_intelxefb_pipeb);
		fPipeBLock = IOLockAlloc();
		// the second framebuffer now, disconnected: it exists before NMXAccel's arm and WindowServer's first 0x980, so a
		// monitor found later is a connection change (DESIGN-multihead-accel.md §5)
		pipeBPublishOffline();
		fPipeBCall = thread_call_allocate(pipeBFire, this);
		if (fPipeBCall) { uint64_t d; clock_interval_to_deadline(3000, kMillisecondScale, &d); thread_call_enter_delayed(fPipeBCall, d); }
	}
	FBLOG("framebuffer published (%ux%u)", fWidth, fHeight);
	// VRR at start: forced only by boot-arg intelxefb_vrr=1/0 (gVrrDefault -1: follow the mode), never with -intelxefb_novrr
	if (vrrBoot == 1 && fVrrAllowed && fVrrCapable && vrrSet(1)) FBLOG("vrr: could not be enabled at start");
	else if (vrrBoot == 0) (void)vrrSet(0);
	else FBLOG("vrr: follows the display mode (on for CoreDisplay's 'Variable' timings, off for fixed ones)");
	return true;
}

void IntelXeFramebuffer::stop(IOService *provider)
{
	if (fGuardCall) { thread_call_cancel(fGuardCall); while (!thread_call_free(fGuardCall)) { thread_call_cancel(fGuardCall); IOSleep(5); } fGuardCall = nullptr; }
	if (fGuardStage) guardAbandon();   // unloaded on purpose: not a failed boot
	fGuardStage = 0;
	nvramWrite("intelxefb-shutdown", "2 IntelXeFB stop");
	if (fIntelXe) (void)powerRegister(false);
	if (fResumeCall) { thread_call_cancel(fResumeCall); while (!thread_call_free(fResumeCall)) { thread_call_cancel(fResumeCall); IOSleep(5); } fResumeCall = nullptr; }
	if (fPmGuardCall) { thread_call_cancel(fPmGuardCall); while (!thread_call_free(fPmGuardCall)) { thread_call_cancel(fPmGuardCall); IOSleep(5); } fPmGuardCall = nullptr; }
	if (fPmTestCall) { thread_call_cancel(fPmTestCall); while (!thread_call_free(fPmTestCall)) { thread_call_cancel(fPmTestCall); IOSleep(5); } fPmTestCall = nullptr; }
	if (fPmTap) { fPmTap->remove(); fPmTap = nullptr; }
	if (fSysGate) { if (IOWorkLoop *w = fSysGate->getWorkLoop()) w->removeEventSource(fSysGate); OSSafeReleaseNULL(fSysGate); }
	if (fFcGate) { if (IOWorkLoop *w = fFcGate->getWorkLoop()) w->removeEventSource(fFcGate); OSSafeReleaseNULL(fFcGate); }
	if (fPipeBCall) { thread_call_cancel(fPipeBCall); while (!thread_call_free(fPipeBCall)) { thread_call_cancel(fPipeBCall); IOSleep(5); } fPipeBCall = nullptr; }
	if (fPipeB >= 0) (void)pipeBOff("stop");
	pipeBStopSecond();
	pipeBSurfaceFree();
	if (fPipeBLock) { IOLockFree(fPipeBLock); fPipeBLock = nullptr; }
	if (fIrqOn || fIntelXe) irqRegister(false);   // fIntelXe stays for ggttRelease below
	// no hotplug interrupt reaches hpdIrq after irqRegister(false) (IntelXe.kext: none in flight once unregistered)
	if (fHpdCall) { thread_call_cancel(fHpdCall); while (!thread_call_free(fHpdCall)) { thread_call_cancel(fHpdCall); IOSleep(5); } fHpdCall = nullptr; }
	if (fHpdReenableCall) { thread_call_cancel(fHpdReenableCall); while (!thread_call_free(fHpdReenableCall)) { thread_call_cancel(fHpdReenableCall); IOSleep(5); } fHpdReenableCall = nullptr; }
	if (fRegs && fShpdFilterBoot != ~0u) wr(SHPD_FILTER_CNT, fShpdFilterBoot);   // the firmware's debounce back
	if (fRegs && fPipe >= 0) { dispLock(); vrrDisableLocked("stop"); programPixelClock(fPclk); dispUnlock(); }   // fixed refresh at the firmware's clock
	if (fCurReady) {   // cursor plane off (and its DDB/watermark back to the firmware's zero), then its GGTT window goes
		cursorPark(true);
		wr(CUR_WM(fPipe, 0), 0); wr(CUR_BUF_CFG(fPipe), 0); wr(CURBASE(fPipe), kCurGgtt);
		IOSleep(30);
		for (uint32_t off = 0; off < kCurBytes; off += 0x1000) pteW(kCurGgtt + off, 0);
		fCurReady = false;
	}
	OSSafeReleaseNULL(fCurMap);
	if (fCurBo) {   // the plane is off and the GGTT window cleared above: IntelXe.kext may reuse the VRAM
		if (IOService *b = intelxe())
			if (const OSSymbol *f = OSSymbol::withCString("intelxeDispVramFree")) { b->callPlatformFunction(f, false, fCurBo, nullptr, nullptr, nullptr); f->release(); }
		fCurBo = nullptr;
	}
	if (fCurSrc) { IOFree(fCurSrc, kCurBytes); fCurSrc = nullptr; }
	if (fEdidParse) { IOFree(fEdidParse, sizeof(*fEdidParse)); fEdidParse = nullptr; }
	if (fCurImgLock) { IOLockFree(fCurImgLock); fCurImgLock = nullptr; }
	fStopping = true;
	blank(false);   // unloading must not leave the panel dark
	if (fRegs && fPipe >= 0) {   // stage 3: back to the firmware's surface, then the slots go
		dispLock();
		if (fHomeBig) programMode(fWidth, fHeight);   // the panel's layout on the firmware's surface again
		homeLocked();
		dispUnlock();
		for (int t = 0; t < 100 && surfLive() != fSurf; t++) IOSleep(1);
		homeBigFree();
		dispLock();
		for (int i = 0; i < kSlots; i++) if (fSlot[i].used) slotClearLocked(i);
		dispUnlock();
	}
	if (fFlipLock) {
		IOSimpleLockLock(fFlipLock);
		Pend p = fPend; fPend = {};
		IOSimpleLockUnlock(fFlipLock);
		if (p.on && p.done) p.done(p.ctx, p.cookie, -3);
	}
	if (gThumbFB == this) {
		sysctl_unregister_oid(&sysctl__debug_intelxefb_thumb); sysctl_unregister_oid(&sysctl__debug_intelxefb_peek);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_mode);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_cropxy); sysctl_unregister_oid(&sysctl__debug_intelxefb_crop);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_link); sysctl_unregister_oid(&sysctl__debug_intelxefb_linklog);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_poke); sysctl_unregister_oid(&sysctl__debug_intelxefb_dpcd);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_vbl);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_tick_us); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_lfc); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_pace_lo_pct); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_pace_hi_pct);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_vrrlog_on); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrrlog);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_vrrinfo); sysctl_unregister_oid(&sysctl__debug_intelxefb_vbl_precise); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_floor_hz); sysctl_unregister_oid(&sysctl__debug_intelxefb_dpms); sysctl_unregister_oid(&sysctl__debug_intelxefb_hwcursor); sysctl_unregister_oid(&sysctl__debug_intelxefb_vrr_modes); sysctl_unregister_oid(&sysctl__debug_intelxefb_irq);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_pm); sysctl_unregister_oid(&sysctl__debug_intelxefb_pminfo); sysctl_unregister_oid(&sysctl__debug_intelxefb_pm_guard);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_hpd); sysctl_unregister_oid(&sysctl__debug_intelxefb_hpdinfo);
		sysctl_unregister_oid(&sysctl__debug_intelxefb_pipeb_mode); sysctl_unregister_oid(&sysctl__debug_intelxefb_pipeb_plan);
		if (fPipeBAllowed) sysctl_unregister_oid(&sysctl__debug_intelxefb_pipeb);
		gThumbFB = nullptr;
	}
	// thread_call_cancel_wait is not exported to kexts: cancel and retry the free until the callback is out (NMIntelFB)
	if (fVblTimer) { thread_call_cancel(fVblTimer); while (!thread_call_free(fVblTimer)) { thread_call_cancel(fVblTimer); IOSleep(5); } fVblTimer = nullptr; }
	if (fReplug) { thread_call_cancel(fReplug); while (!thread_call_free(fReplug)) { thread_call_cancel(fReplug); IOSleep(5); } fReplug = nullptr; }
	OSSafeReleaseNULL(fSurfMap); OSSafeReleaseNULL(fMMIO); fRegs = nullptr;
	if (fDispLock) { IOLockFree(fDispLock); fDispLock = nullptr; }
	if (fFlipLock) { IOSimpleLockFree(fFlipLock); fFlipLock = nullptr; }
	if (fHpdImrLock) { IOSimpleLockFree(fHpdImrLock); fHpdImrLock = nullptr; }
	if (fCurLock) { IOSimpleLockFree(fCurLock); fCurLock = nullptr; }
	if (fGuardLock) { IOLockFree(fGuardLock); fGuardLock = nullptr; }
	ggttRelease();
	OSSafeReleaseNULL(fIntelXe);
	IOFramebuffer::stop(provider);
}

// One AUX transaction (intel_dp_aux_xfer): load the data registers big-endian, send, wait for SEND_BUSY to clear, write
// the status back to clear DONE and the errors, retry timeouts and receive errors up to 5 times. Returns the reply size
// (reply byte included) or -1.
int IntelXeFramebuffer::auxXfer(uint32_t ctl, const uint8_t *tx, uint32_t txn, uint8_t *rx, uint32_t rxmax)
{
	if (txn > 20 || rxmax > 20) return -1;
	uint32_t send = AUX_SEND_BUSY | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_TIME_OUT_MAX | AUX_RECEIVE_ERROR |
	                (txn << 20) | AUX_SYNC | AUX_POWER_REQUEST;
	uint32_t status = 0;
	for (int t = 0; t < 5; t++) {
		for (uint32_t i = 0; i < txn; i += 4) {
			uint32_t v = 0;
			for (uint32_t j = 0; j < 4 && i + j < txn; j++) v |= (uint32_t)tx[i + j] << ((3 - j) * 8);
			wr(ctl + 4 + i, v);
		}
		wr(ctl, send);
		for (int w = 0; w < 1000 && ((status = rd(ctl)) & AUX_SEND_BUSY); w++) IODelay(10);   // up to 10 ms
		wr(ctl, status | AUX_DONE | AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR);
		if (status & AUX_SEND_BUSY) return -1;
		if (status & AUX_TIME_OUT_ERROR) continue;
		if (status & AUX_RECEIVE_ERROR) { IODelay(400); continue; }
		if (status & AUX_DONE) break;
	}
	if (!(status & AUX_DONE) || (status & (AUX_TIME_OUT_ERROR | AUX_RECEIVE_ERROR))) return -1;
	uint32_t n = (status >> 20) & 0x1F;
	if (n == 0 || n > 20) return -1;
	if (n > rxmax) n = rxmax;
	for (uint32_t i = 0; i < n; i += 4) {
		uint32_t v = rd(ctl + 4 + i);
		for (uint32_t j = 0; j < 4 && i + j < n; j++) rx[i + j] = (uint8_t)(v >> ((3 - j) * 8));
	}
	return (int)n;
}

// I2C-over-AUX to an I2C address, the EDID's 0x50 by default (drm_dp_i2c_xfer framing: request 7:4, address, size - 1;
// an empty transfer is the 3-byte address-only form). Retries DEFER replies. Returns 0 on ACK.
int IntelXeFramebuffer::i2cAux(uint32_t ctl, uint8_t req, const uint8_t *data, uint32_t len, uint8_t *out, uint8_t addr)
{
	uint8_t tx[20] = { (uint8_t)(req << 4), 0x00, addr, (uint8_t)(len ? len - 1 : 0) }, rx[20];
	bool write = !(req & DP_AUX_I2C_READ);
	uint32_t txn = len ? 4 : 3;
	if (write && len) { if (len > 16) return -1; bcopy(data, tx + 4, len); txn += len; }
	for (int t = 0; t < 32; t++) {
		int n = auxXfer(ctl, tx, txn, rx, write ? 1 : len + 1);
		if (n < 1) return -1;
		uint8_t reply = rx[0] >> 4;
		if ((reply & 0x3) == 0x2 || ((reply >> 2) & 0x3) == 0x2) { IODelay(500); continue; }   // native or I2C DEFER
		if (reply != 0) return -1;                                                            // NACK
		if (!write && (uint32_t)(n - 1) < len) return -1;                                     // short read
		if (!write) bcopy(rx + 1, out, len);
		return 0;
	}
	return -1;
}

// Native AUX to the sink's DPCD (drm_dp_dpcd_access framing: request 7:4 = 8 write / 9 read with address 19:16, then
// address 15:0 and size - 1), DEFER retried, up to 16 bytes. Returns 0 on ACK. dpcd: the panel's AUX channel; dpcdAt:
// any (the second pipe's).
int IntelXeFramebuffer::dpcd(uint32_t addr, uint8_t *buf, uint32_t len, bool write)
{
	return dpcdAt(fAux, addr, buf, len, write);
}

int IntelXeFramebuffer::dpcdAt(uint32_t ctl, uint32_t addr, uint8_t *buf, uint32_t len, bool write)
{
	if (!ctl || !len || len > 16 || addr > 0xFFFFF) return -1;
	uint8_t tx[20] = { (uint8_t)((write ? 0x80 : 0x90) | (addr >> 16)), (uint8_t)(addr >> 8), (uint8_t)addr, (uint8_t)(len - 1) }, rx[20];
	uint32_t txn = 4;
	if (write) { bcopy(buf, tx + 4, len); txn += len; }
	for (int t = 0; t < 32; t++) {
		int n = auxXfer(ctl, tx, txn, rx, write ? 1 : len + 1);
		if (n < 1) return -1;
		uint8_t reply = rx[0] >> 4;
		if ((reply & 0x3) == 0x2) { IODelay(500); continue; }   // DEFER
		if (reply & 0x3) return -1;                            // NACK
		if (!write) { if ((uint32_t)(n - 1) < len) return -1; bcopy(rx + 1, buf, len); }
		return 0;
	}
	return -1;
}

bool IntelXeFramebuffer::readEdid(uint32_t ctl)
{
	return readEdidInto(ctl, fEdid, &fEdidLen);
}

// the panel's EDID (readEdid) or another connector's (connScan): kEdidMax bytes at most
bool IntelXeFramebuffer::readEdidInto(uint32_t ctl, uint8_t *edid, uint32_t *edidLen)
{
	static const uint8_t kHeader[8] = { 0x00, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x00 };
	uint32_t total = 128;
	*edidLen = 0;
	// one I2C transaction per 128-byte block, as drm_edid.c:2140 drm_do_probe_ddc_edid: for blocks 2 and up the E-DDC
	// segment pointer first (address 0x30, segment = block / 2; never sent for blocks 0-1, "to not upset non-compliant
	// DDC monitors"), then the offset write (block * 128, mod 256), 16-byte reads with MOT, stop. v10l read two blocks
	// only, so the Odyssey G5's DisplayID block (block 2: 2560x1440 at 180 Hz, 1920x1080 at 180 Hz) was never seen.
	for (uint32_t blk = 0; blk * 128 < total && blk < kEdidMax / 128; blk++) {
		const uint8_t segment = (uint8_t)(blk >> 1), off = (uint8_t)(blk * 128);
		bool ok = !segment || i2cAux(ctl, DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT, &segment, 1, nullptr, 0x30) == 0;
		if (!ok) FBLOG("edid: segment pointer %u not acknowledged: %u blocks", segment, blk);
		ok = ok && i2cAux(ctl, DP_AUX_I2C_WRITE | DP_AUX_I2C_MOT, &off, 1, nullptr) == 0;
		for (uint32_t i = 0; ok && i < 128; i += 16)
			ok = i2cAux(ctl, DP_AUX_I2C_READ | DP_AUX_I2C_MOT, nullptr, 16, edid + blk * 128 + i) == 0;
		i2cAux(ctl, DP_AUX_I2C_READ, nullptr, 0, nullptr);
		if (!ok) break;
		uint8_t sum = 0;
		for (uint32_t i = 0; i < 128; i++) sum = (uint8_t)(sum + edid[blk * 128 + i]);
		if (sum) { FBLOG("edid: block %u checksum 0x%02x", blk, sum); break; }
		if (blk == 0) {
			if (bcmp(edid, kHeader, 8)) { FBLOG("edid: no EDID header"); break; }
			total = 128u * (1 + edid[126]);
		}
		*edidLen = (blk + 1) * 128;
	}
	// fewer blocks than the base block announces (an unreadable or third block): announce what was read, fix the checksum
	if (*edidLen && edid[126] != *edidLen / 128 - 1) {
		edid[126] = (uint8_t)(*edidLen / 128 - 1);
		uint8_t sum = 0;
		for (int i = 0; i < 127; i++) sum = (uint8_t)(sum + edid[i]);
		edid[127] = (uint8_t)(0x100 - sum);
	}
	return *edidLen != 0;
}

// The monitor's EDID over the AUX channel of the DDI that feeds the adopted pipe. Read only: the AUX power request is
// put back as it was. Without an EDID the display is published unnamed with the one adopted mode.
void IntelXeFramebuffer::probeEdid()
{
	uint32_t port = (rd(TRANS_DDI_FUNC_CTL(fPipe)) >> 27) & 0xF;   // 1 + enum port (A 0, B 1, TC1 3 ...)
	int first = port == 1 ? 0 : port == 2 ? 1 : (port >= 4 && port <= 7) ? (int)port - 2 : -1;
	// the panel's HPD pin (encoder->hpd_pin, hpd_mtp): DDI A/B -> SDE 16/17, TC1-4 -> SDE 24-27
	fHpdPin = port == 1 ? 1u << 16 : port == 2 ? 1u << 17 : (port >= 4 && port <= 7) ? 1u << (24 + port - 4) : 0;
	for (int k = -1; k < 6; k++) {
		int a = k < 0 ? first : k;
		if (a < 0 || (k >= 0 && a == first)) continue;
		uint32_t ctl = kAuxCtl[a], before = rd(ctl);
		if (!(before & AUX_POWER_REQUEST)) { wr(ctl, before | AUX_POWER_REQUEST); IODelay(1200); }   // 600 us + margin
		bool ok = readEdid(ctl);
		if (!(before & AUX_POWER_REQUEST)) wr(ctl, rd(ctl) & ~AUX_POWER_REQUEST);
		if (ok) {
			char name[14] = "";
			for (int d = 54; d <= 108; d += 18)   // the monitor-name descriptor (tag 0xFC)
				if (!fEdid[d] && !fEdid[d + 1] && fEdid[d + 3] == 0xFC) {
					for (int i = 0; i < 13 && fEdid[d + 5 + i] != 0x0A; i++) name[i] = (char)fEdid[d + 5 + i];
					break;
				}
			FBLOG("edid: %u bytes on AUX %s (DDI port select %u), monitor \"%s\"", fEdidLen, kAuxName[a], port, name);
			setProperty("IntelXeFBEdidAux", kAuxName[a]);
			fAux = ctl;
			return;
		}
	}
	FBLOG("edid: no reply on any AUX channel (DDI port select %u)", port);
}

// v10m: the EDID's mode list as Linux builds it (fb/IntelXeEdid.cpp; drm_edid.c _drm_edid_connector_add_modes) and the
// monitor range Linux reads from it (drm_get_monitor_range), parsed once at start from what readEdid got.
void IntelXeFramebuffer::parseEdid()
{
	if (fEdidLen < 128) return;
	fEdidParse = (intelxeedid::Result *)IOMallocZero(sizeof(intelxeedid::Result));
	if (!fEdidParse) { FBLOG("edid: no memory for the parse"); return; }
	if (!intelxeedid::parse(fEdid, fEdidLen, fEdidParse)) {
		FBLOG("edid: base block not usable (header, checksum or version): no EDID modes");
		IOFree(fEdidParse, sizeof(*fEdidParse)); fEdidParse = nullptr;
		return;
	}
	const intelxeedid::Result &r = *fEdidParse;
	FBLOG("edid: 1.%d, %u valid blocks (%u dropped), CTA %d, DisplayID %d, continuous %d, monitor range %d-%d Hz, %d modes%s",
	      r.revision, (unsigned)r.blocks, (unsigned)r.badBlocks, r.hasCta, r.hasDisplayId, r.continuous, r.minVfreq, r.maxVfreq,
	      r.count, r.overflow ? " (more did not fit)" : "");
}

// The driver's mode table (see the Modes comment at fModeW). The EDID's modes are validated as Linux validates a DP
// connector's (drm_probe_helper.c:444: intel_mode_valid, the connector flags, 4:2:0, intel_dp_mode_valid with the
// link as trained: DDI_CLK_VALFREQ x the transcoder's lane count, 24 bpp, BMG's 1305.6 MHz dotclock), duplicates
// dropped (drm_connector.c:3277 drm_mode_expose_to_userspace), sorted (drm_mode_sort), then turned into sizes x rates
// (intelxeedid::buildDriverModes). Published as IntelXeFBModes / IntelXeFBEdidModes.
void IntelXeFramebuffer::buildModes()
{
	fNumModes = 0; fModesFromEdid = false;
	intelxeedid::Panel pn = {};
	pn.width = fWidth; pn.height = fHeight; pn.htotal = fHT; pn.vtotal = fVT; pn.pclkHz = fPclk;
	pn.hsyncStart = fHSS; pn.hsyncEnd = fHSE; pn.vsyncStart = fVSS; pn.vsyncEnd = fVSE;
	pn.vrr = fVrrCapable && fVrrAllowed; pn.vrrMin = (int)fVrrMinHz; pn.vrrMax = (int)fVrrMaxHz;
	pn.scaling = fScaling; pn.rates = fRates; pn.commonRates = fCommonRates; pn.maxVtotal = 8192;
	pn.edid = fEdidParse;
	if (fEdidParse && fEdidParse->count) {
		const uint32_t ddi = fPipe >= 0 ? rd(TRANS_DDI_FUNC_CTL(fPipe)) : 0, sel = (ddi >> 27) & 0xF, w = (ddi >> 1) & 7;
		intelxeedid::Sink sk = {};
		sk.maxDotclk = 1305600;   // intel_cdclk.c:3401: 2 pixels/clock x 652800 kHz (display 11+, 38.4 MHz ref)
		sk.linkRate = sel ? (int)rd(DDI_CLK_VALFREQ((int)sel - 1)) : 0;
		sk.lanes = w == 3 ? 4 : (int)w + 1;   // DDI_PORT_WIDTH (IntelXeLink identify)
		if (sk.linkRate < 162000 || sk.linkRate > 810000) {   // no clock readback: the sink's LINK_BW_SET (x 27 MHz)
			uint8_t bw = 0;
			sk.linkRate = dpcd(0x100, &bw, 1, false) == 0 && bw ? bw * 27000 : 810000;
		}
		sk.bpp = 24; sk.interlace = false; sk.ycbcr420 = false; sk.displayVer = 14;
		intelxeedid::Mode *valid = (intelxeedid::Mode *)IOMallocZero(sizeof(intelxeedid::Mode) * intelxeedid::kMaxModes);
		if (valid) {
			int nv = 0;
			fEdidValid = fEdidRejected = 0;
			for (int i = 0; i < fEdidParse->count; i++) {
				const intelxeedid::Mode &m = fEdidParse->modes[i];
				int st = intelxeedid::modeValid(m, sk);
				if (st == intelxeedid::kOk)
					for (int j = 0; j < nv; j++)
						if (intelxeedid::sameTimings(valid[j], m)) { st = intelxeedid::kDuplicate; valid[j].type |= m.type & intelxeedid::kTypePreferred; break; }
				if (st == intelxeedid::kOk) { valid[nv++] = m; continue; }
				if (st != intelxeedid::kDuplicate) {
					fEdidRejected++;
					FBLOG("edid: %dx%d%s %u.%03u Hz %d kHz (%s) refused: %s", m.hdisplay, m.vdisplay, (m.flags & intelxeedid::kInterlace) ? "i" : "",
					      intelxeedid::refreshMilliHz(m) / 1000, intelxeedid::refreshMilliHz(m) % 1000, m.clock, intelxeedid::sourceName(m.source),
					      intelxeedid::statusName(st));
				}
			}
			intelxeedid::sort(valid, nv);
			fEdidValid = (uint32_t)nv;
			fNumModes = intelxeedid::buildDriverModes(valid, nv, pn, fModes, intelxeedid::kMaxDriverModes);
			fModesFromEdid = fNumModes > 0;
			FBLOG("modes: link %d kHz x%d, %d EDID modes valid, %u refused -> %d driver modes", sk.linkRate, sk.lanes, nv,
			      fEdidRejected, fNumModes);
			IOFree(valid, sizeof(intelxeedid::Mode) * intelxeedid::kMaxModes);
		}
	}
	if (!fNumModes) {
		fNumModes = intelxeedid::fallbackDriverModes(pn, fModes, intelxeedid::kMaxDriverModes);
		FBLOG("modes: no usable EDID modes, the fixed table: %d modes", fNumModes);
	}
	fPclkMax = fPclk;
	for (int i = 0; i < fNumModes; i++)
		if ((uint64_t)fModes[i].pclkKhz * 1000 > fPclkMax) fPclkMax = (uint64_t)fModes[i].pclkKhz * 1000;
	if (fPclkMax > fPclk) FBLOG("modes: rates above the firmware's up to %llu kHz (M/N on the trained link)", fPclkMax / 1000);
	// IntelXeFBModes: one line per size, "WxH: id=Hz ..." (the ids debug.intelxefb_mode takes)
	const size_t cap = 6144;
	char *b = (char *)IOMallocZero(cap);
	if (!b) return;
	size_t n = 0;
	for (int i = 0; i < fNumModes && n + 64 < cap; i++) {
		const intelxeedid::DriverMode &d = fModes[i];
		if (!i || d.w != fModes[i - 1].w || d.h != fModes[i - 1].h)
			n += (size_t)snprintf(b + n, cap - n, "%s%ux%u%s:", i ? "; " : "", d.w, d.h, d.stretched ? " (stretched)" : "");
		n += (size_t)snprintf(b + n, cap - n, " %d=%u.%02u", i + 1, d.milliHz / 1000, (d.milliHz % 1000) / 10);
	}
	setProperty("IntelXeFBModes", b);
	snprintf(b, cap, "%s: %d probed, %u valid, %u refused, %d driver modes", fModesFromEdid ? "EDID" : "fixed table",
	         fEdidParse ? fEdidParse->count : 0, fEdidValid, fEdidRejected, fNumModes);
	setProperty("IntelXeFBEdidModes", b);
	// the log: one line per size
	for (int i = 0; i < fNumModes;) {
		int j = i;
		n = (size_t)snprintf(b, cap, "%ux%u%s:", fModes[i].w, fModes[i].h, fModes[i].stretched ? " stretched" : "");
		for (; j < fNumModes && fModes[j].w == fModes[i].w && fModes[j].h == fModes[i].h && n + 48 < cap; j++)
			n += (size_t)snprintf(b + n, cap - n, " %d=%u.%02u(%s)", j + 1, fModes[j].milliHz / 1000, (fModes[j].milliHz % 1000) / 10,
			              intelxeedid::whyName(fModes[j].why));
		FBLOG("modes: %s", b);
		i = j;
	}
	IOFree(b, cap);
}

// ---- second display pipe (docs/design/DESIGN-second-pipe.md) -----------------------------------------------------
// Behind -intelxefb_pipeb / NVRAM intelxefb-pipeb=1 (fPipeBAllowed), except the dry run (pipeBPlanText), which only
// reads registers. Untested on hardware: the test machine has one monitor.

// The second pipe's framebuffer: a second IOFramebuffer on the same GPU and IOGraphics controller (the first one's
// IOFBDependentID, index 1). A subclass of IntelXeFramebuffer only so that NMXAccel, which watches
// serviceMatching("IntelXeFramebuffer") and slots heads by IOFBDependentIndex, takes it as head 1; it overrides every
// IOFramebuffer method the first one overrides and uses none of its state. One mode (the sink's), the sink's EDID, a VBL
// timer on the pipe's frame counter (no vblank interrupt for this pipe), its surface for nvScanoutInfo (NMXAccel's copy
// path), no hardware cursor, no flips, no VRR, no gamma, no detailed timings.
// Published at the first framebuffer's start, disconnected (Apple's model: every framebuffer of a GPU exists from the
// start, a monitor arriving or leaving is a connection change; DESIGN-multihead-accel.md §2, §5). IOGraphics' protocol
// for that (SDK IOKit/graphics/IOGraphicsTypes.h, IOFramebuffer.h; IOGraphics IOFramebuffer.cpp):
//  * getAttributeForConnection(0, kConnectionCheckEnable) is how IOFramebuffer::open and ::updateOnline decide whether the
//    framebuffer is online (IOFramebuffer.cpp updateOnline: "nowOnline = !dead && (err || connectEnabled)"); offline, no
//    IODisplayConnect/IODisplay is created (displaysOnline), so WindowServer sees no display (IOGraphicsLib IOFBGetState:
//    online = IODisplayForFramebuffer). kConnectionEnable reads the same.
//  * Offline, updateOnline takes the mode whose timing is kIOTimingIDApple_0x0_0hz_Offline as offlineMode, and
//    processConnectChange sets it when the display goes ("this timing will take the display off-line", IOGraphicsTypes.h;
//    IONDRVFramebuffer's kConnectionCheckEnable is "online = !searchOfflineMode()"). So the mode list is that mode alone
//    while disconnected and the sink's mode alone while connected (intelxeplan::secondFbMode).
//  * A change is reported with the connect interrupt (registerForInterruptType(kIOFBConnectInterruptType): IOFramebuffer's
//    connectChangeInterrupt bumps the controller's fConnectChange and starts its work; IOGraphics then calls
//    processConnectChange on every framebuffer of the controller, which asks kConnectionChanged (here: nothing to probe,
//    connScan already ran) and kConnectionCheckEnable again). hasDDCConnect / kConnectionSupportsHLDDCSense only while
//    connected, so IODisplay reads the sink's EDID through getDDCBlock.
// The connection state is also published as IntelXeFBConnected (ioreg, NMXAccel's and NMXAGDC's head diagnostics).
class IntelXeFramebufferPipe : public IntelXeFramebuffer {
	OSDeclareDefaultStructors(IntelXeFramebufferPipe)
	IntelXeFramebuffer *bOwner = nullptr;
	int bPipe = -1;
	uint32_t bRB = 0;
	intelxeplan::Timing bT = {};
	IOPhysicalAddress bPhys = 0;
	IOMemoryMap *bMap = nullptr;
	uint8_t bEdid[kEdidMax] = {};
	uint32_t bEdidLen = 0;
	volatile bool bConnected = false, bStopping = false;
	struct BIrq { IOFBInterruptProc proc; OSObject *target; void *ref; };
	static constexpr int kBVbl = 4;
	BIrq bVbl[kBVbl] = {}, bConnect = {};
	volatile bool bVblOn[kBVbl] = {};
	thread_call_t bVblTimer = nullptr;
	uint32_t bLastFrame = 0;
	volatile uint32_t bPowerOrd = 2;
	uint64_t bFrameUs() const { return (bT.clock && bT.htotal && bT.vtotal) ? (uint64_t)bT.htotal * bT.vtotal * 1000ull / bT.clock : 16667; }
	uint32_t bRefresh16() const { return (bT.htotal && bT.vtotal) ? (uint32_t)(((uint64_t)bT.clock * 1000 << 16) / ((uint64_t)bT.htotal * bT.vtotal)) : 60u << 16; }
	// polled twice a frame: the frame counter changing is the vblank (a free tick while nothing is lit)
	static void bVblFire(thread_call_param_t p0, thread_call_param_t) {
		IntelXeFramebufferPipe *me = (IntelXeFramebufferPipe *)p0;
		if (me->bStopping) return;
		const uint32_t f = me->bOwner ? me->bOwner->pipeBFrame() : 0;
		if (f != me->bLastFrame || !me->bConnected || !f) {
			me->bLastFrame = f;
			for (int i = 0; i < kBVbl; i++) if (me->bVbl[i].proc && me->bVblOn[i]) me->bVbl[i].proc(me->bVbl[i].target, me->bVbl[i].ref);
		}
		uint64_t d;
		clock_interval_to_deadline((uint32_t)(me->bConnected ? me->bFrameUs() / 2 : me->bFrameUs()), kMicrosecondScale, &d);
		if (!me->bStopping) thread_call_enter_delayed(me->bVblTimer, d);
	}
	// IONDRVFramebuffer::ndrvSetPowerState's order, as the first framebuffer's powerApply (without its recovery paths)
	void bPowerApply(uint32_t to, bool family) {
		if (to > 2) to = 2;
		const uint32_t from = bPowerOrd;
		if (to == from) return;
		IOIndex post = 0;
		if (from == 2) { if (family) handleEvent(kIOFBNotifyWillPowerOff); post = kIOFBNotifyDidPowerOff; if (bOwner) bOwner->pipeBBlank(true); }
		else if (to == 2) { if (family) handleEvent(kIOFBNotifyWillPowerOn); post = kIOFBNotifyDidPowerOn; }
		if (to == 0 && family) handleEvent(kIOFBNotifyWillSleep, (void *) true);
		bPowerOrd = to;
		if (from == 0 && family) handleEvent(kIOFBNotifyDidWake, (void *) true);
		if (post) {
			if (family) handleEvent(post);
			if (post == kIOFBNotifyDidPowerOn && bOwner) bOwner->pipeBBlank(false);
		}
		FBLOG("second framebuffer: power %u -> %u%s", from, to, family ? "" : " (before WindowServer's open)");
	}
	// NMXAGDC answers IOPresentment's link-configuration and plane-scaler queries for this head from it (as for the
	// first framebuffer: publishTiming), and NMXAccel bounds a present's wait by the flip wait
	// (docs/design/DESIGN-multihead-accel.md). Layout: tag, pixel clock in Hz, h visible/sync start/sync end/total, the
	// same vertically, refresh in mHz, h and v sync positive.
	void bPublishTiming() {
		const intelxeplan::Timing &t = bT;
		const uint32_t tm[13] = { 0x4b350001u, t.clock * 1000u, t.hdisplay, t.hsyncStart, t.hsyncEnd, t.htotal, t.vdisplay,
		                          t.vsyncStart, t.vsyncEnd, t.vtotal, intelxeplan::refreshMilliHz(t), t.phsync ? 1u : 0u, t.pvsync ? 1u : 0u };
		if (OSData *d = OSData::withBytes(tm, sizeof tm)) { setProperty("NVRMTiming", d); d->release(); }
		setProperty("NVRMFlipWaitUs", (unsigned long long)(bFrameUs() * 5 / 4), 32);
	}
public:
	// Before any sink (IntelXeFB's start): the placeholder timing (intelxeplan::offlineTiming), no surface, no EDID,
	// disconnected. pipe is the pipe a sink will be lit on.
	void configureOffline(IntelXeFramebuffer *owner, int pipe) {
		bOwner = owner; bPipe = pipe; bT = intelxeplan::offlineTiming(); bRB = (bT.hdisplay * 4 + 63) & ~63u; bPhys = 0;
		OSSafeReleaseNULL(bMap);
		bEdidLen = 0;
		bConnected = false;
		bPublishTiming();
		setProperty("IntelXeFBConnected", false);
	}
	// A sink lit on the pipe (before setConnected(true)). Returns whether it is another sink than the last one configured
	// (EDID, timing or surface), which a connected head must report even without a connection change.
	bool configure(IntelXeFramebuffer *owner, int pipe, const intelxeplan::Timing &t, uint32_t rb, IOPhysicalAddress phys,
	               IOMemoryMap *map, const uint8_t *edid, uint32_t edidLen) {
		const uint32_t len = edidLen <= kEdidMax ? edidLen : 0;
		const bool changed = !intelxeplan::sameTiming(t, bT) || rb != bRB || phys != bPhys || len != bEdidLen || (len && bcmp(edid, bEdid, len));
		bOwner = owner; bPipe = pipe; bT = t; bRB = rb; bPhys = phys;
		if (map != bMap) { if (map) map->retain(); OSSafeReleaseNULL(bMap); bMap = map; }
		bEdidLen = len;
		if (bEdidLen) bcopy(edid, bEdid, bEdidLen);
		bPublishTiming();
		return changed;
	}
	// kConnectionCheckEnable / kConnectionEnable and the mode list follow it; the connect interrupt makes IOGraphics probe
	// the connection again (only for a real change: intelxeplan::secondFbReport). The last sink's timing, EDID and surface
	// stay while disconnected (nvScanoutInfo answers not-ready, hasDDCConnect false).
	void setConnected(bool on, bool sinkChanged = false) {
		const bool was = bConnected;
		bConnected = on;
		setProperty("IntelXeFBConnected", on);
		if (!intelxeplan::secondFbReport(was, on, sinkChanged)) {
			FBLOG("second framebuffer: display still %s (nothing reported)", on ? "connected" : "disconnected");
			return;
		}
		FBLOG("second framebuffer: display %s%s%s", on ? "connected" : "disconnected", was && on ? " (another sink)" : "",
		      bConnect.proc ? ", connect interrupt raised" : " (IOGraphics has not opened it: read at its open)");
		if (bConnect.proc) bConnect.proc(bConnect.target, bConnect.ref);
	}
	bool connected() const { return bConnected; }
	IOService *probe(IOService *, SInt32 *) override { return nullptr; }   // never matched: created by the first framebuffer
	bool start(IOService *provider) override {
		if (!bOwner || bPipe < 0) return false;
		setProperty("IOFBDependentID", 0x4235383046420001ull, 64);   // the first framebuffer's: one IOGraphics controller
		setProperty("IOFBDependentIndex", 1ull, 32);
		setProperty("IntelXeFBPipe", (unsigned long long)bPipe, 32);
		setProperty("IntelXeFBConnected", (bool)bConnected);
		if (!IOFramebuffer::start(provider)) return false;   // registers the service
		bVblTimer = thread_call_allocate(bVblFire, this);
		if (bVblTimer) { uint64_t d; clock_interval_to_deadline((uint32_t)bFrameUs(), kMicrosecondScale, &d); thread_call_enter_delayed(bVblTimer, d); }
		registerPowerDriver(this, kPowerStates, 3);
		changePowerStateTo(1);
		FBLOG("second framebuffer published: pipe %c, %s, %ux%u, %u kHz", 'A' + bPipe, bConnected ? "connected" : "disconnected (placeholder mode)",
		      bT.hdisplay, bT.vdisplay, bT.clock);
		return true;
	}
	void stop(IOService *provider) override {
		bStopping = true;
		if (bVblTimer) { thread_call_cancel(bVblTimer); while (!thread_call_free(bVblTimer)) { thread_call_cancel(bVblTimer); IOSleep(5); } bVblTimer = nullptr; }
		OSSafeReleaseNULL(bMap);
		IOFramebuffer::stop(provider);
	}
	IOReturn enableController() override { return kIOReturnSuccess; }
	bool isConsoleDevice() override { return false; }
	void systemWillShutdown(IOOptionBits spec) override { IOFramebuffer::systemWillShutdown(spec); }
	IODeviceMemory *getApertureRange(IOPixelAperture aperture) override {
		if (aperture != kIOFBSystemAperture || !bPhys) return nullptr;
		return IODeviceMemory::withRange(bPhys, (IOPhysicalLength)bRB * bT.vdisplay);
	}
	IODeviceMemory *getVRAMRange() override { return getApertureRange(kIOFBSystemAperture); }
	const char *getPixelFormats() override { return IO32BitDirectPixels "\0"; }
	// one mode: the sink's while connected, the offline mode while not (intelxeplan::secondFbMode). The offline mode keeps
	// the last (or placeholder) size, so a pixel query or a setDisplayMode of it is always answerable.
	IOItemCount getDisplayModeCount() override { return 1; }
	IOReturn getDisplayModes(IODisplayModeID *modes) override { modes[0] = intelxeplan::secondFbMode(bConnected); return kIOReturnSuccess; }
	IOReturn getInformationForDisplayMode(IODisplayModeID mode, IODisplayModeInformation *info) override {
		if (!intelxeplan::secondFbModeValid(mode) || !info) return kIOReturnBadArgument;
		bzero(info, sizeof(*info));
		info->nominalWidth = bT.hdisplay; info->nominalHeight = bT.vdisplay; info->refreshRate = bRefresh16();
		info->flags = kDisplayModeValidFlag | kDisplayModeSafeFlag | (mode == intelxeplan::kSecondModeSink ? kDisplayModeDefaultFlag : 0);
		return kIOReturnSuccess;
	}
	IOReturn validateDetailedTiming(void *, IOByteCount) override { return kIOReturnUnsupported; }
	IOReturn setDetailedTimings(OSArray *) override { return kIOReturnUnsupported; }
	UInt64 getPixelFormatsForDisplayMode(IODisplayModeID, IOIndex) override { return 0; }
	IOReturn getPixelInformation(IODisplayModeID mode, IOIndex depth, IOPixelAperture aperture, IOPixelInformation *pi) override {
		if (!intelxeplan::secondFbModeValid(mode) || depth != 0 || aperture != kIOFBSystemAperture || !pi) return kIOReturnUnsupportedMode;
		bzero(pi, sizeof(*pi));
		pi->bytesPerRow = bRB; pi->bitsPerPixel = 32; pi->pixelType = kIORGBDirectPixels;
		pi->componentCount = 3; pi->bitsPerComponent = 8;
		pi->componentMasks[0] = 0x00FF0000; pi->componentMasks[1] = 0x0000FF00; pi->componentMasks[2] = 0x000000FF;
		strlcpy(pi->pixelFormat, IO32BitDirectPixels, sizeof(pi->pixelFormat));
		pi->activeWidth = bT.hdisplay; pi->activeHeight = bT.vdisplay;
		return kIOReturnSuccess;
	}
	IOReturn getCurrentDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { *mode = intelxeplan::secondFbMode(bConnected); *depth = 0; return kIOReturnSuccess; }
	IOReturn getStartupDisplayMode(IODisplayModeID *mode, IOIndex *depth) override { return getCurrentDisplayMode(mode, depth); }
	IOReturn getTimingInfoForDisplayMode(IODisplayModeID mode, IOTimingInformation *info) override {
		if (!intelxeplan::secondFbModeValid(mode) || !info) return kIOReturnUnsupportedMode;
		bzero(info, sizeof(*info));
		if (mode == intelxeplan::kSecondModeOffline) {   // what IOFramebuffer::updateOnline looks for: no detailed timing
			info->appleTimingID = kIOTimingIDApple_0x0_0hz_Offline;
			return kIOReturnSuccess;
		}
		info->appleTimingID = kIOTimingIDInvalid;
		info->flags = kIODetailedTimingValid;
		IODetailedTimingInformationV2 &d = info->detailedInfo.v2;
		d.pixelClock = d.minPixelClock = d.maxPixelClock = (uint64_t)bT.clock * 1000;
		d.horizontalActive = bT.hdisplay; d.horizontalBlanking = bT.htotal - bT.hdisplay;
		d.horizontalSyncOffset = bT.hsyncStart - bT.hdisplay; d.horizontalSyncPulseWidth = bT.hsyncEnd - bT.hsyncStart;
		d.verticalActive = bT.vdisplay; d.verticalBlanking = bT.vtotal - bT.vdisplay;
		d.verticalSyncOffset = bT.vsyncStart - bT.vdisplay; d.verticalSyncPulseWidth = bT.vsyncEnd - bT.vsyncStart;
		return kIOReturnSuccess;
	}
	// the offline mode is what processConnectChange sets when the display goes: nothing to do, the pipe is already down
	IOReturn setDisplayMode(IODisplayModeID mode, IOIndex depth) override {
		return intelxeplan::secondFbModeValid(mode) && depth == 0 ? kIOReturnSuccess : kIOReturnUnsupported;
	}
	IOReturn setStartupDisplayMode(IODisplayModeID, IOIndex) override { return kIOReturnSuccess; }
	IOItemCount getConnectionCount() override { return 1; }
	IOReturn registerForInterruptType(IOSelect type, IOFBInterruptProc proc, OSObject *target, void *ref, void **iref) override {
		if (type == kIOFBVBLInterruptType) {
			for (int i = 0; i < kBVbl; i++) if (!bVbl[i].proc) { bVbl[i] = { proc, target, ref }; bVblOn[i] = true; if (iref) *iref = &bVbl[i]; return kIOReturnSuccess; }
			return kIOReturnNoResources;
		}
		if (type == kIOFBConnectInterruptType) { bConnect = { proc, target, ref }; if (iref) *iref = &bConnect; return kIOReturnSuccess; }
		return kIOReturnUnsupported;
	}
	IOReturn unregisterInterrupt(void *iref) override {
		for (int i = 0; i < kBVbl; i++) if (iref == &bVbl[i]) { bVblOn[i] = false; bVbl[i] = {}; }
		if (iref == &bConnect) bConnect = {};
		return kIOReturnSuccess;
	}
	IOReturn setInterruptState(void *iref, UInt32 state) override {
		for (int i = 0; i < kBVbl; i++) if (iref == &bVbl[i]) bVblOn[i] = state != 0;
		return kIOReturnSuccess;
	}
	IOReturn setCursorImage(void *) override { return kIOReturnUnsupported; }
	IOReturn setCursorState(SInt32, SInt32, bool) override { return kIOReturnUnsupported; }
	IOReturn setGammaTable(UInt32, UInt32, UInt32, void *) override { return kIOReturnSuccess; }
	IOReturn setCLUTWithEntries(IOColorEntry *, UInt32, UInt32, IOOptionBits) override { return kIOReturnSuccess; }
	IOReturn setApertureEnable(IOPixelAperture, IOOptionBits) override { return kIOReturnSuccess; }
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override {
		return IOFramebuffer::newUserClient(owningTask, securityID, type, handler);
	}
	IOReturn setPowerState(unsigned long ordinal, IOService *device) override {
		if (opened) return IOFramebuffer::setPowerState(ordinal, device);   // acknowledged by IOGraphics' power work
		bPowerApply((uint32_t)ordinal, false);                            // not in the controller's list before open
		return kIOPMAckImplied;
	}
	unsigned long maxCapabilityForDomainState(IOPMPowerFlags domainState) override { return (domainState & kIOPMPowerOn) ? 2 : 0; }
	unsigned long initialPowerStateForDomainState(IOPMPowerFlags domainState) override { return (domainState & kIOPMPowerOn) ? 2 : 0; }
	IOReturn setAttribute(IOSelect attribute, uintptr_t value) override {
		if (attribute == kIOWindowServerActiveAttribute) return kIOReturnSuccess;
		if (attribute == kIOPowerAttribute || attribute == kIODriverPowerAttribute) { bPowerApply((uint32_t)value, true); return kIOReturnSuccess; }
		return IOFramebuffer::setAttribute(attribute, value);
	}
	IOReturn getAttribute(IOSelect attribute, uintptr_t *value) override {
		if (attribute == kIOWindowServerActiveAttribute) { if (value) *value = 1; return kIOReturnSuccess; }
		if (attribute == kIOHardwareCursorAttribute) { if (value) *value = 0; return kIOReturnSuccess; }
		return IOFramebuffer::getAttribute(attribute, value);
	}
	IOReturn setAttributeForConnection(IOIndex idx, IOSelect attribute, uintptr_t value) override {
		switch (attribute) {
		case kConnectionPower: case kConnectionColorModesSupported: case kConnectionColorDepthsSupported:
		case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported: case kConnectionColorMode:
		case kConnectionDisplayFlags: case kConnectionFlags:
			return kIOReturnSuccess;
		default: return IOFramebuffer::setAttributeForConnection(idx, attribute, value);
		}
	}
	IOReturn getAttributeForConnection(IOIndex idx, IOSelect attr, uintptr_t *value) override {
		if (idx != 0) return kIOReturnBadArgument;
		if (attr == kConnectionSupportsHLDDCSense) return bEdidLen && bConnected ? kIOReturnSuccess : kIOReturnUnsupported;
		// processConnectChange asks it first (value may be NULL): the hardware was probed already (connScan before
		// setConnected), so there is nothing to sense again. The post-wake probe likewise.
		if (attr == kConnectionChanged || attr == kConnectionPostWake) return kIOReturnSuccess;
		if (!value) return kIOReturnBadArgument;
		switch (attr) {
		case kConnectionEnable: case kConnectionCheckEnable: *value = bConnected ? 1 : 0; return kIOReturnSuccess;
		case kConnectionFlags: *value = 0; return kIOReturnSuccess;
		case kConnectionColorModesSupported: case kConnectionColorMode: *value = 0x00000001; return kIOReturnSuccess;
		case kConnectionColorDepthsSupported: case kConnectionControllerColorDepth: case kConnectionControllerDepthsSupported:
			*value = 0x00000002; return kIOReturnSuccess;
		default: return IOFramebuffer::getAttributeForConnection(idx, attr, value);
		}
	}
	bool hasDDCConnect(IOIndex idx) override { return idx == 0 && bEdidLen && bConnected; }
	IOReturn getDDCBlock(IOIndex idx, UInt32 block, IOSelect type, IOOptionBits, UInt8 *data, IOByteCount *length) override {
		if (idx != 0 || type != kIODDCBlockTypeEDID || !data || !length || block < 1 || block * 128 > bEdidLen) return kIOReturnUnsupported;
		IOByteCount n = *length < 128 ? *length : 128;
		bcopy(bEdid + (block - 1) * 128, data, n); *length = n;
		return kIOReturnSuccess;
	}
	// NMXAccel's copy path (nvScanoutInfo, NVRMVramRequest v2); its VRAM requests go to the first framebuffer. Nothing
	// that would act on the first pipe (nmxScanout, nmxPipeGamma, nmxVrrStamp, the guard) is answered here.
	IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override {
		if (fn && fn->isEqualTo("nvScanoutInfo")) {
			NVRMVramRequest *r = (NVRMVramRequest *)p1;
			if (!r || r->version != 2) return kIOReturnBadArgument;
			if (!bMap || !bConnected) return kIOReturnNotReady;
			r->kva = (void *)bMap->getVirtualAddress(); r->phys = bPhys; r->actualSize = (unsigned long long)bRB * bT.vdisplay;
			r->width = bT.hdisplay; r->height = bT.vdisplay; r->pitch = bRB;
			return kIOReturnSuccess;
		}
		if (fn && bOwner && (fn->isEqualTo("nvAllocVram") || fn->isEqualTo("nvFreeVram") || fn->isEqualTo("intelxefbRunGated")))
			return bOwner->callPlatformFunction(fn, wait, p1, p2, p3, p4);
		if (fn && (fn->isEqualTo("nmxScanout") || fn->isEqualTo("nmxPipeGamma") || fn->isEqualTo("nmxVrrStamp") || fn->isEqualTo("intelxefbGuardAccel")))
			return kIOReturnUnsupported;
		return IOFramebuffer::callPlatformFunction(fn, wait, p1, p2, p3, p4);
	}
	IOReturn connectFlags(IOIndex, IODisplayModeID mode, IOOptionBits *flags) override {
		if (flags) *flags = !intelxeplan::secondFbModeValid(mode) ? 0 : kDisplayModeValidFlag | kDisplayModeSafeFlag |
		                    (mode == intelxeplan::kSecondModeSink ? kDisplayModeDefaultFlag : 0);
		return kIOReturnSuccess;
	}
};
OSDefineMetaClassAndStructors(IntelXeFramebufferPipe, IntelXeFramebuffer)

namespace {
// PORT_CLOCK_CTL (intel_cx0_phy_regs.h XELPDP_PORT_CLOCK_CTL): A 0x640E0, TC n 0x16F260 + (n - 1) x 0x200
constexpr uint32_t portClockCtl(int port) { return port < 3 ? 0x640E0u + (uint32_t)port * 0x100u : 0x16F260u + (uint32_t)(port - 3) * 0x200u; }
constexpr uint32_t SSC_ENABLE_PLLB = 1u << 0;
constexpr uint32_t HSW_PWR_WELL_CTL2 = 0x45404, SKL_FUSE_STATUS = 0x42000;
constexpr uint32_t TR_(int t, uint32_t a) { return a + (uint32_t)t * 0x1000u; }
// a text buffer for the dry run
struct Txt {
	char *b; int n, len;
	__attribute__((format(printf, 2, 3))) void p(const char *fmt, ...) {
		if (len >= n - 1) return;
		va_list ap;
		va_start(ap, fmt);
		const int k = vsnprintf(b + len, (size_t)(n - len), fmt, ap);
		va_end(ap);
		if (k > 0) len = len + k < n - 1 ? len + k : n - 1;
	}
};
const char *same(uint32_t a, uint32_t b) { return a == b ? "match" : "DIFF"; }
}

// The registers the plan starts from (read only; the second pipe's own registers only while its power well is on)
void IntelXeFramebuffer::planLive(intelxeplan::LiveState *s, int pipeB) const
{
	using namespace intelxeplan;
	const int p = fPipe;
	*s = {};
	s->pipeA = p;
	s->htotal = rd(TR_(p, 0x60000)); s->hblank = rd(TR_(p, 0x60004)); s->hsync = rd(TR_(p, 0x60008));
	s->vtotal = rd(TR_(p, 0x6000C)); s->vblank = rd(TR_(p, 0x60010)); s->vsync = rd(TR_(p, 0x60014));
	s->scl = rd(TRANS_SET_CONTEXT_LATENCY(p)); s->pipesrc = rd(TR_(p, 0x6001C)); s->funcCtl = rd(TRANS_DDI_FUNC_CTL(p));
	s->transconf = rd(TRANSCONF(p));
	s->dataM = rd(TRANS_DATA_M1(p)); s->dataN = rd(TRANS_DATA_N1(p)); s->linkM = rd(TRANS_LINK_M1(p)); s->linkN = rd(TRANS_LINK_N1(p));
	s->linetime = rd(WM_LINETIME(p));
	const uint32_t sel = (s->funcCtl >> 27) & 0xF;
	s->valfreq = sel ? rd(DDI_CLK_VALFREQ((int)sel - 1)) : 0;
	s->planeCtl = rd(PLANE_CTL(p)); s->planeSize = rd(PLANE_SIZE(p)); s->planeBufCfg = rd(PLANE_BUF_CFG1(p));
	s->curBufCfg = rd(CUR_BUF_CFG(p)); s->curCtl = rd(CURCNTR(p));
	for (int l = 0; l < kWmLevels; l++) s->planeWm[l] = rd(TR_(p, 0x70240) + (uint32_t)l * 4);
	s->planeWmTrans = rd(TR_(p, 0x70268));
	s->mbusCtl = rd(MBUS_CTL);
	s->dbox[p] = rd(PIPE_MBUS_DBOX_CTL(p));
	s->pwCtl2 = rd(HSW_PWR_WELL_CTL2);
	if (pipeB >= 0 && pipeB < 4 && (s->pwCtl2 & pwState(pipePowerWell(pipeB)))) {
		s->transconfB = rd(TRANSCONF(pipeB));
		s->dbox[pipeB] = rd(PIPE_MBUS_DBOX_CTL(pipeB));
	}
	for (int i = 0; i < kNumSlices; i++) s->dbufCtl[i] = rd(DBUF_CTL_S(i));
	s->pmd0 = rd(PMDEMAND0); s->pmd1 = rd(PMDEMAND1); s->cdclkCtl = rd(0x46000);
	s->lat01 = rd(0x45780); s->lat23 = rd(0x45784); s->lat45 = rd(0x45788); s->latSagv = rd(0x4578C);
	s->memGlobal = rd(MTL_MEM_SS_INFO_GLOBAL);
	const uint32_t points = (s->memGlobal >> 8) & 0xF;
	for (uint32_t i = 0; i < points && i < 8; i++) s->qgvLow[i] = rd(MTL_MEM_SS_INFO_QGV_POINT_LOW((int)i));
}

// The sink's mode for the second pipe: the EDID's modes checked as i915 checks a DP mode (intelxeedid::modeValid with the
// sink's DPCD rate and lanes, 24 bpp, BMG's 1305.6 MHz dotclock), sorted as drm_mode_sort (the preferred one first),
// the first one whose surface fits a 4096x2304 buffer
bool IntelXeFramebuffer::connMode(Conn &c, intelxeedid::Result *res)
{
	c.modeOk = false;
	c.name[0] = 0;
	if (!res || c.edidLen < 128 || !intelxeedid::parse(c.edid, c.edidLen, res)) return false;
	for (int d = 54; d <= 108; d += 18)   // the monitor-name descriptor (tag 0xFC)
		if (!c.edid[d] && !c.edid[d + 1] && c.edid[d + 3] == 0xFC) {
			for (int i = 0; i < 13 && c.edid[d + 5 + i] != 0x0A; i++) c.name[i] = (char)c.edid[d + 5 + i];
			break;
		}
	intelxeedid::Sink sk = {};
	sk.maxDotclk = 1305600;
	sk.linkRate = (int)intelxeplan::umin(intelxeplan::rateFromBwCode(c.dpcd[1]), 810000);
	sk.lanes = (int)intelxeplan::umin(c.dpcd[2] & 0x1Fu, 4);
	sk.bpp = 24; sk.interlace = false; sk.ycbcr420 = false; sk.displayVer = 14;
	if (sk.linkRate < 162000 || sk.lanes < 1) return false;
	const size_t vsize = sizeof(intelxeedid::Mode) * intelxeedid::kMaxModes;
	intelxeedid::Mode *valid = (intelxeedid::Mode *)IOMallocZero(vsize);
	if (!valid) return false;
	int nv = 0;
	for (int i = 0; i < res->count && nv < intelxeedid::kMaxModes; i++) {
		const intelxeedid::Mode &m = res->modes[i];
		if (intelxeedid::modeValid(m, sk) != intelxeedid::kOk || (m.flags & intelxeedid::kInterlace)) continue;
		if (m.hdisplay > (int)kScalerMaxW || m.vdisplay > (int)kScalerMaxH) continue;
		valid[nv++] = m;
	}
	intelxeedid::sort(valid, nv);
	if (nv) {
		const intelxeedid::Mode &m = valid[0];
		c.mode = { (uint32_t)m.clock, (uint32_t)m.hdisplay, (uint32_t)m.hsync_start, (uint32_t)m.hsync_end, (uint32_t)m.htotal,
		           (uint32_t)m.vdisplay, (uint32_t)m.vsync_start, (uint32_t)m.vsync_end, (uint32_t)m.vtotal,
		           (m.flags & intelxeedid::kPHSync) != 0, (m.flags & intelxeedid::kPVSync) != 0 };
		c.modeOk = intelxeplan::timingSane(c.mode);
	}
	IOFree(valid, vsize);
	return c.modeOk;
}

// Every BMG port but the panel's: SDE hotplug live -> its AUX channel (power requested as probeEdid does,
// xelpdp_aux_power_well_enable), DPCD receiver caps (drm_dp_read_dpcd_caps with the extended caps), SINK_COUNT of a
// branch device (a DP-to-HDMI converter counts only with a sink behind it), the EDID, the mode. A live port that does
// not answer on AUX is not a DP sink (native HDMI is out of scope) and is left alone. The lit second pipe's port is only
// checked for its hotplug bit. Caller holds fPipeBLock.
void IntelXeFramebuffer::connScan()
{
	using namespace intelxeplan;
	if (!fRegs || fPipe < 0) return;
	const uint32_t live = rd(SDEISR);
	const uint32_t sel = (rd(TRANS_DDI_FUNC_CTL(fPipe)) >> 27) & 0xF;
	intelxeedid::Result *res = (intelxeedid::Result *)IOMallocZero(sizeof(intelxeedid::Result));
	for (int i = 0; i < kNumPorts; i++) {
		Conn &c = fConn[i];
		Port p;
		(void)portInfo(kPorts[i], &p);
		const bool was = c.dp;
		c.port = p.port;
		c.panel = sel == p.ddiSel || (fAux && fAux == p.aux);
		c.live = (live & p.hpd) != 0;
		if (c.panel) { c.dp = false; continue; }
		if (fPipeB >= 0 && fPipeBConn == i && c.live) continue;   // lit: its AUX belongs to the second pipe now
		bool dp = false;
		if (c.live) {
			const uint32_t before = rd(p.aux);
			if (!(before & AUX_POWER_REQUEST)) { wr(p.aux, before | AUX_POWER_REQUEST); IODelay(1200); }
			uint8_t d[16], ext[16];
			dp = dpcdAt(p.aux, 0x000, d, 16, false) == 0 && d[0];
			if (dp) {
				if ((d[0x0E] & 0x80) && dpcdAt(p.aux, 0x2200, ext, 16, false) == 0 && ext[0] >= d[0]) bcopy(ext, d, 15);
				bcopy(d, c.dpcd, 16);
				c.sinkCount = 0xFF;
				uint8_t sc = 0;
				if ((d[5] & 1) && dpcdAt(p.aux, 0x200, &sc, 1, false) == 0) {   // DOWNSTREAMPORT_PRESENT: a branch device
					c.sinkCount = (uint8_t)(((sc & 0x80) >> 1) | (sc & 0x3F));
					if (!c.sinkCount) dp = false;
				}
			}
			if (dp) {
				c.edidLen = 0;
				if (!readEdidInto(p.aux, c.edid, &c.edidLen)) c.edidLen = 0;
				(void)connMode(c, res);
			}
			if (!(before & AUX_POWER_REQUEST)) wr(p.aux, rd(p.aux) & ~AUX_POWER_REQUEST);
		}
		if (!dp) { c.edidLen = 0; c.modeOk = false; c.sinkCount = 0xFF; c.name[0] = 0; }
		c.dp = dp;
		if (!c.live) { if (was) FBLOG("second pipe: %s: sink gone", p.name); continue; }
		if (!dp) { FBLOG("second pipe: %s: hotplug live, no DP sink on AUX 0x%x (HDMI or nothing behind a converter): not driven", p.name, p.aux); continue; }
		FBLOG("second pipe: %s: DP rev %x, max rate 0x%02x x%u%s, EDID %u bytes \"%s\", mode %s%ux%u %u kHz (%u mHz)", p.name, c.dpcd[0],
		      c.dpcd[1], c.dpcd[2] & 0x1F, c.sinkCount != 0xFF ? " (branch)" : "", c.edidLen, c.name, c.modeOk ? "" : "NONE ",
		      c.mode.hdisplay, c.mode.vdisplay, c.mode.clock, refreshMilliHz(c.mode));
	}
	if (res) IOFree(res, sizeof(intelxeedid::Result));
	fConnScanned = true;
}

// A zeroed display buffer from IntelXe.kext ("intelxeDispVramAlloc", contiguous, inside BAR2) at a GGTT range IntelXe.kext
// picks ("intelxeGgttAlloc" flags 0; 256 KB aligned, intel_linear_alignment on display 9+), mapped write-combined.
// Kept while big enough; freed at stop or when a larger one is needed (the plane is off then).
bool IntelXeFramebuffer::pipeBSurface(uint64_t bytes)
{
	bytes = (bytes + 0xFFFF) & ~0xFFFFull;
	if (fPipeBBo && fPipeBSize >= bytes) return true;
	pipeBSurfaceFree();
	IOService *b = intelxe();
	const OSSymbol *va = OSSymbol::withCString("intelxeDispVramAlloc"), *ga = OSSymbol::withCString("intelxeGgttAlloc");
	struct { uint32_t version, flags; uint64_t size, dpa; void *cookie; } r = { 1, 1, bytes, 0, nullptr };
	struct { uint32_t version, flags; uint64_t size, align; uint32_t ggtt; } g = { 1, 0, bytes, 0x40000, 0 };
	IOReturn rr = (b && va) ? b->callPlatformFunction(va, false, &r, nullptr, nullptr, nullptr) : kIOReturnUnsupported;
	IOReturn gr = kIOReturnUnsupported;
	if (rr == kIOReturnSuccess && r.cookie && !(r.dpa & 0xFFF) && r.dpa + bytes <= fBarLen && ga)
		gr = b->callPlatformFunction(ga, false, &g, nullptr, nullptr, nullptr);
	if (va) va->release();
	if (ga) ga->release();
	IOMemoryMap *m = nullptr;
	if (gr == kIOReturnSuccess && g.ggtt)
		if (IODeviceMemory *d = IODeviceMemory::withRange(fBarBase + (IOPhysicalAddress)r.dpa, (IOPhysicalLength)bytes)) { m = d->map(kIOMapWriteCombineCache); d->release(); }
	fPipeBBo = r.cookie; fPipeBDpa = r.dpa; fPipeBSize = bytes; fPipeBGgtt = gr == kIOReturnSuccess ? g.ggtt : 0; fPipeBMap = m;
	if (!m) {
		FBLOG("second pipe: no surface (display buffer 0x%x at VRAM 0x%llx, GGTT range 0x%x): IntelXe.kext has no allocator, or "
		      "the buffer is outside BAR2", (unsigned)rr, r.dpa, (unsigned)gr);
		pipeBSurfaceFree();
		return false;
	}
	for (uint64_t off = 0; off < bytes; off += 0x1000) pteW(fPipeBGgtt + (uint32_t)off, (fPipeBDpa + off) | fPteFlags);
	(void)pte(fPipeBGgtt + (uint32_t)bytes - 0x1000);
	FBLOG("second pipe: surface %llu KB at GGTT 0x%x -> VRAM 0x%llx", bytes >> 10, fPipeBGgtt, fPipeBDpa);
	return true;
}

void IntelXeFramebuffer::pipeBSurfaceFree()
{
	if (!fPipeBBo && !fPipeBGgtt && !fPipeBMap) return;   // never allocated (the knob off): nothing at all happens
	IOService *b = intelxe();
	if (fPipeBGgtt) {
		for (uint64_t off = 0; off < fPipeBSize; off += 0x1000) pteW(fPipeBGgtt + (uint32_t)off, 0);
		if (b) if (const OSSymbol *f = OSSymbol::withCString("intelxeGgttFree")) { b->callPlatformFunction(f, false, (void *)(uintptr_t)fPipeBGgtt, nullptr, nullptr, nullptr); f->release(); }
	}
	OSSafeReleaseNULL(fPipeBMap);
	if (fPipeBBo && b)
		if (const OSSymbol *f = OSSymbol::withCString("intelxeDispVramFree")) { b->callPlatformFunction(f, false, fPipeBBo, nullptr, nullptr, nullptr); f->release(); }
	fPipeBBo = nullptr; fPipeBDpa = 0; fPipeBSize = 0; fPipeBGgtt = 0;
}

// intel_pmdemand_program_params: wait for the previous request (REQ_ENABLE and DCPR_STATUS_1's in-flight bit clear,
// 10 ms each), write the dwords that change, set REQ_ENABLE, wait for it to clear (10 ms; Linux waits on the PM demand
// response interrupt, which ends the same way)
bool IntelXeFramebuffer::pmdemandProgram(uint32_t r0, uint32_t r1, const char *why)
{
	using namespace intelxeplan;
	auto clear = [this](uint32_t off, uint32_t bit) { for (int t = 0; t < 200; t++) { if (!(rd(off) & bit)) return true; IODelay(50); } return false; };
	if (!clear(PMDEMAND1, PMDEMAND_REQ) || !clear(DCPR_STATUS_1, PMDEMAND_INFLIGHT)) { FBLOG("pmdemand (%s): a request is still in flight", why); return false; }
	const uint32_t c0 = rd(PMDEMAND0), c1 = rd(PMDEMAND1);
	r1 &= ~PMDEMAND_REQ;
	if (c0 == r0 && c1 == r1) return true;
	if (c0 != r0) wr(PMDEMAND0, r0);
	if (c1 != r1) wr(PMDEMAND1, r1);
	wr(PMDEMAND1, rd(PMDEMAND1) | PMDEMAND_REQ);
	const bool ok = clear(PMDEMAND1, PMDEMAND_REQ);
	FBLOG("pmdemand (%s): 0x%08x 0x%08x -> 0x%08x 0x%08x%s", why, c0, c1, r0, r1, ok ? "" : " -- no response within 10 ms");
	return ok;
}

// gen9_dbuf_slices_update -> gen9_dbuf_slice_set: request each slice's power as the mask says, 10 us, check the state
void IntelXeFramebuffer::dbufSlicesSet(uint8_t mask)
{
	using namespace intelxeplan;
	for (int s = 0; s < kNumSlices; s++) {
		const bool on = mask & (1u << s);
		const uint32_t v = rd(DBUF_CTL_S(s));
		if (((v & DBUF_POWER_REQUEST) != 0) == on) continue;
		wr(DBUF_CTL_S(s), on ? v | DBUF_POWER_REQUEST : v & ~DBUF_POWER_REQUEST);
		(void)rd(DBUF_CTL_S(s));
		IODelay(10);
		if (((rd(DBUF_CTL_S(s)) & DBUF_POWER_STATE) != 0) != on) FBLOG("dbuf: slice %d power %s timeout (0x%08x)", s + 1, on ? "on" : "off", rd(DBUF_CTL_S(s)));
	}
}

// hsw_power_well_enable for PW_A..D: the driver's request in HSW_PWR_WELL_CTL2, its state (1 ms), the fuse of its
// power gate (gen9_wait_for_power_well_fuses). fPipeBPw remembers a request this driver added (the BIOS may hold one).
bool IntelXeFramebuffer::powerWellOn(int idx)
{
	using namespace intelxeplan;
	const uint32_t req = pwReq(idx), st = pwState(idx);
	if (!(rd(HSW_PWR_WELL_CTL2) & req)) { wr(HSW_PWR_WELL_CTL2, rd(HSW_PWR_WELL_CTL2) | req); fPipeBPw = req; }
	int t = 0;
	for (; t < 100 && !(rd(HSW_PWR_WELL_CTL2) & st); t++) IODelay(10);
	const bool on = rd(HSW_PWR_WELL_CTL2) & st;
	for (t = 0; t < 100 && on && !(rd(SKL_FUSE_STATUS) & pwFuse(idx)); t++) IODelay(10);
	const bool fuse = rd(SKL_FUSE_STATUS) & pwFuse(idx);
	FBLOG("second pipe: power well %d %s (HSW_PWR_WELL_CTL2 0x%08x, fuse %s)", idx, on ? "on" : "did NOT come on", rd(HSW_PWR_WELL_CTL2), fuse ? "ok" : "not set");
	return on;
}

// The global registers back as they were before the second pipe (its own registers are the caller's): Linux's commit
// without it runs intel_dbuf_mbus_pre_ddb_update for unjoined -> joined (MBUS_CTL, then PIPE_MBUS_DBOX_CTL, then the min
// tracker), drops the pipe's power domains, the DBUF slices only it needed, and the PMDEMAND request last
// (intel_pmdemand_post_plane_update). Caller holds dispLock.
void IntelXeFramebuffer::pipeBGlobalsBack(int pipeB)
{
	using namespace intelxeplan;
	if (!fPipeBSaved.valid) return;
	const PipeBSaved &v = fPipeBSaved;
	const uint32_t mbusMask = MBUS_JOIN | MBUS_HASHING_1x4 | MBUS_PIPE_SELECT_MASK;
	wr(MBUS_CTL, (rd(MBUS_CTL) & ~mbusMask) | (v.mbusCtl & mbusMask));
	wr(PIPE_MBUS_DBOX_CTL(fPipe), v.dboxA);
	for (int s = 0; s < kNumSlices; s++) wr(DBUF_CTL_S(s), (rd(DBUF_CTL_S(s)) & ~DBUF_MIN_TRACKER_MASK) | (v.dbufCtl[s] & DBUF_MIN_TRACKER_MASK));
	if (fPipeBPw) { wr(HSW_PWR_WELL_CTL2, rd(HSW_PWR_WELL_CTL2) & ~fPipeBPw); fPipeBPw = 0; }
	uint8_t slices = 0;
	for (int s = 0; s < kNumSlices; s++) if (v.dbufCtl[s] & DBUF_POWER_REQUEST) slices = (uint8_t)(slices | (1u << s));
	dbufSlicesSet(slices);
	Port p;
	if (portInfo(fPipeBOut.port, &p) && !(v.aux & AUX_POWER_REQUEST)) wr(p.aux, rd(p.aux) & ~AUX_POWER_REQUEST);
	(void)pmdemandProgram(v.pmd0, v.pmd1, "second pipe off");
	fPipeBSaved.valid = false;
	FBLOG("second pipe: pipe %c's global state put back (MBUS_CTL 0x%08x, DBOX 0x%08x, slices 0x%x)", 'A' + pipeB, rd(MBUS_CTL),
	      rd(PIPE_MBUS_DBOX_CTL(fPipe)), slices);
}

// Light the second pipe on the scanned DP sink. The order is intel_atomic_commit_tail's for a commit that adds a pipe
// while pipe A keeps running: PMDEMAND pre (max of old and new), the pipe's power well (its power domains), the union of
// DBUF slices (intel_dbuf_pre_plane_update), MBUS unjoin (intel_dbuf_mbus_post_ddb_update: min tracker, DBOX of every
// active pipe, MBUS_CTL, a vblank on pipe A; pipe A's own DDB and watermarks stay, computePlan checked they fit), then
// hsw_crtc_enable on the new pipe (IntelXeLink kEnableNew), then PMDEMAND post. A failure takes it all back. Caller
// holds fPipeBLock. Returns 0 or an errno.
int IntelXeFramebuffer::pipeBOn(const char *why)
{
	using namespace intelxeplan;
	if (!fPipeBAllowed) return EPERM;
	if (!fRegs || fPipe < 0 || fStopping) return ENODEV;
	if (fPipeB >= 0) return 0;
	if (fSysSuspended || fDpmsDown) { FBLOG("second pipe: %s: not now (%s)", why, fSysSuspended ? "system sleep" : "display powered down"); return EBUSY; }
	int ci = -1;
	for (int i = 0; i < kNumPorts && ci < 0; i++) if (!fConn[i].panel && fConn[i].dp && fConn[i].modeOk) ci = i;
	if (ci < 0) { FBLOG("second pipe: %s: no DP sink with a usable mode", why); return ENODEV; }
	const Conn &c = fConn[ci];
	if (fPipe != 0) { FBLOG("second pipe: the panel is on pipe %c; only a panel on pipe A is handled", 'A' + fPipe); return ENOTSUP; }
	const int pb = 1;
	const uint32_t w = c.mode.hdisplay, h = c.mode.vdisplay, rb = (w * 4 + 63) & ~63u;
	if (!pipeBSurface((uint64_t)rb * h)) return ENOMEM;
	LiveState *s = (LiveState *)IOMallocZero(sizeof(LiveState));
	Plan *pl = (Plan *)IOMallocZero(sizeof(Plan));
	if (!s || !pl) { if (s) IOFree(s, sizeof *s); if (pl) IOFree(pl, sizeof *pl); return ENOMEM; }
	int err = 0;
	dispLock();
	planLive(s, pb);
	const Request q = { pb, c.port, c.mode, umin(rateFromBwCode(c.dpcd[1]), 810000), umin(c.dpcd[2] & 0x1Fu, 4) };
	const int e = computePlan(*s, q, pl);
	Port port;
	(void)portInfo(c.port, &port);
	if (e) {
		FBLOG("second pipe: %s: %s %ux%u refused: %s", why, port.name, w, h, errName(e));
		err = EINVAL;
	} else {
		IntelXeLink::NewOutput n = {};
		n.port = c.port; n.pipe = pb; n.lanes = (int)pl->b.link.lanes; n.clock = (int)pl->b.link.rate;
		n.ssc = (rd(portClockCtl(pl->portA.port)) & SSC_ENABLE_PLLB) && (c.dpcd[3] & 1);   // the VBT's SSC (as on pipe A) and sink downspread
		const TimingRegs &tr = pl->b.regs;
		n.funcBpcSync = tr.funcSync;   // BPC_8 = 0
		n.htotal = tr.htotal; n.hblank = tr.hblank; n.hsync = tr.hsync; n.vtotal = tr.vtotal; n.vblank = tr.vblank; n.vsync = tr.vsync;
		n.vsyncshift = tr.vsyncshift; n.scl = tr.scl; n.mult = tr.mult;
		n.dataM = dataMReg(pl->b.mn); n.dataN = pl->b.mn.dataN; n.linkM = pl->b.mn.linkM; n.linkN = pl->b.mn.linkN;
		n.pipesrc = tr.pipesrc; n.pipeMisc = kPipeMisc; n.pipeChickenSet = kPipeChickenSet; n.transconf = 0;
		n.linetime = pl->b.linetime; n.msaMisc = kMsaMisc;
		n.planeCtl = rd(PLANE_CTL(fPipe)) | kEnable;   // the panel's plane format: linear XRGB8888
		n.planeStride = rb / 64; n.planeSize = (h - 1) << 16 | (w - 1); n.planeSurf = fPipeBGgtt; n.planeColorCtl = kPlaneColorCtl;
		n.planeBufCfg = pl->b.wm.regBufCfg; n.planeWmTrans = pl->b.wm.regTrans; n.curBufCfg = pl->b.wm.regCurBufCfg;
		for (int l = 0; l < 8; l++) n.planeWm[l] = pl->b.wm.regWm[l];
		// what goes back if anything fails or when the pipe goes
		fPipeBSaved.pmd0 = s->pmd0; fPipeBSaved.pmd1 = s->pmd1; fPipeBSaved.mbusCtl = s->mbusCtl; fPipeBSaved.dboxA = s->dbox[fPipe];
		for (int i = 0; i < kNumSlices; i++) fPipeBSaved.dbufCtl[i] = s->dbufCtl[i];
		fPipeBSaved.aux = rd(port.aux);
		fPipeBSaved.valid = true;
		fPipeBOut = n;
		const uint32_t underruns = fIrqUnderruns, iirA = rd(DE_PIPE_IIR(fPipe));
		FBLOG("second pipe: %s: %s %ux%u %u kHz on pipe %c, link %u kHz x%u, DDB %u-%u (bank +%u), slices 0x%x -> 0x%x, MBUS %s -> %s",
		      why, port.name, w, h, c.mode.clock, 'A' + pb, pl->b.link.rate, pl->b.link.lanes, pl->b.ddb.start, pl->b.ddb.end,
		      pl->b.mbusOffset, pl->slicesNow, pl->slicesAfter, pl->joinedNow ? "joined" : "unjoined", pl->joinedAfter ? "joined" : "unjoined");
		bool ok = pmdemandProgram(pl->pmd0Pre, pl->pmd1Pre, "second pipe, before");
		ok = ok && powerWellOn(pl->pwIdx);
		if (ok) {
			dbufSlicesSet((uint8_t)(pl->slicesNow | pl->slicesAfter));
			for (int i = 0; i < kNumSlices; i++) wr(DBUF_CTL_S(i), (rd(DBUF_CTL_S(i)) & ~DBUF_MIN_TRACKER_MASK) | pl->trackerAfter << 16);
			wr(PIPE_MBUS_DBOX_CTL(fPipe), mbusDboxCtl(pl->joinedAfter));
			wr(PIPE_MBUS_DBOX_CTL(pb), mbusDboxCtl(pl->joinedAfter));
			if (rd(MBUS_CTL) != pl->mbusCtlAfter) {
				wr(MBUS_CTL, pl->mbusCtlAfter);
				const uint32_t f0 = rd(FRMCOUNT(fPipe));   // intel_crtc_wait_for_next_vblank on pipe A
				for (int t = 0; t < 50 && rd(FRMCOUNT(fPipe)) == f0; t++) IOSleep(1);
			}
			wr(port.aux, rd(port.aux) | AUX_POWER_REQUEST);   // kept while the pipe is lit
			IODelay(1200);
			AuxCtx ctx = { this, port.aux };
			IntelXeLink link(fRegs, port.aux, dpcdAtThunk, &ctx);
			const int r = link.runNew(IntelXeLink::kEnableNew, n);
			if (r) {
				FBLOG("second pipe: enable failed (%d, debug.intelxefb_linklog has the steps): taking it down", r);
				AuxCtx ctx2 = { this, port.aux };
				IntelXeLink undo(fRegs, port.aux, dpcdAtThunk, &ctx2);
				(void)undo.runNew(IntelXeLink::kDisableNew, n);
				ok = false;
			}
		}
		if (ok) {
			(void)pmdemandProgram(pl->pmd0Post, pl->pmd1Post, "second pipe, after");
			fPipeB = pb; fPipeBConn = ci;
		} else {
			pipeBGlobalsBack(pb);
			err = EIO;
		}
		const uint32_t iirA2 = rd(DE_PIPE_IIR(fPipe));
		FBLOG("second pipe: %s; pipe A underruns %u -> %u, PIPE_IIR 0x%08x -> 0x%08x", ok ? "lit" : "NOT lit", underruns, fIrqUnderruns, iirA, iirA2);
	}
	dispUnlock();
	IOFree(s, sizeof *s);
	IOFree(pl, sizeof *pl);
	if (!err) pipeBPublish(c);
	return err;
}

// hsw_crtc_disable for the second pipe, then the global state back. The second framebuffer is told first (disconnected,
// the connect interrupt), except for a system sleep (tellFb false): there the monitor is still there, and pipeBWork after
// the wake either lights the pipe again (no report: the same sink) or reports it gone. Caller holds fPipeBLock.
int IntelXeFramebuffer::pipeBOff(const char *why, bool tellFb)
{
	using namespace intelxeplan;
	if (fPipeB < 0) return 0;
	const int pb = fPipeB;
	if (fSecond && tellFb) fSecond->setConnected(false);
	dispLock();
	Port port;
	(void)portInfo(fPipeBOut.port, &port);
	AuxCtx ctx = { this, port.aux };
	IntelXeLink link(fRegs, port.aux, dpcdAtThunk, &ctx);
	const int r = link.runNew(IntelXeLink::kDisableNew, fPipeBOut);
	pipeBGlobalsBack(pb);
	fPipeB = -1; fPipeBConn = -1;
	dispUnlock();
	FBLOG("second pipe: %s: pipe %c off (%d)", why, 'A' + pb, r);
	return r ? EIO : 0;
}

// The second framebuffer at IntelXeFB's start: published disconnected (IntelXeFramebufferPipe's comment has IOGraphics'
// side), so that IOGraphics links it into this controller before its first open, NMXAccel's boot arm can give it a
// display pipe and NMXAGDC's first 0x980 reply (WindowServer's start) lists it. Only with a panel on pipe A: pipeBOn
// lights pipe B only then. Runs before fPipeBCall exists, so nothing else touches fSecond yet.
void IntelXeFramebuffer::pipeBPublishOffline()
{
	if (fSecond || !fPipeBAllowed) return;
	if (fPipe != 0) { FBLOG("second framebuffer: not published (the panel is on pipe %c; only a panel on pipe A gets a second pipe)", 'A' + fPipe); return; }
	IntelXeFramebufferPipe *f = OSTypeAlloc(IntelXeFramebufferPipe);
	if (!f || !f->init()) { OSSafeReleaseNULL(f); FBLOG("second framebuffer: no memory (published when a sink lights the pipe)"); return; }
	f->configureOffline(this, 1);
	if (!f->attach(fPCI)) { f->release(); FBLOG("second framebuffer: attach failed (published when a sink lights the pipe)"); return; }
	if (!f->start(fPCI)) { f->detach(fPCI); f->release(); FBLOG("second framebuffer: start failed (published when a sink lights the pipe)"); return; }
	fSecond = f;
	FBLOG("second framebuffer: published as head 1, disconnected -- a monitor on another DP port is reported to IOGraphics "
	      "as a connection change (no WindowServer restart)");
}

// the pipe lit on a sink: the second framebuffer gets the sink (mode, EDID, surface) and is connected. If it could not be
// published at start, it is published now (the late path: NMXAccel's multi-head knob or debug.nvaccelfb=3 registers it,
// WindowServer composites it after its restart).
void IntelXeFramebuffer::pipeBPublish(const Conn &c)
{
	const uint32_t rb = (c.mode.hdisplay * 4 + 63) & ~63u;
	if (fSecond) {
		const bool changed = fSecond->configure(this, fPipeB, c.mode, rb, fBarBase + (IOPhysicalAddress)fPipeBDpa, fPipeBMap, c.edid, c.edidLen);
		fSecond->setConnected(true, changed);
		return;
	}
	IntelXeFramebufferPipe *f = OSTypeAlloc(IntelXeFramebufferPipe);
	if (!f || !f->init()) { OSSafeReleaseNULL(f); FBLOG("second framebuffer: no memory"); return; }
	(void)f->configure(this, fPipeB, c.mode, rb, fBarBase + (IOPhysicalAddress)fPipeBDpa, fPipeBMap, c.edid, c.edidLen);
	f->setConnected(true);   // not started yet: no interrupt, IOGraphics reads it at its open
	if (!f->attach(fPCI)) { f->release(); FBLOG("second framebuffer: attach failed"); return; }
	if (!f->start(fPCI)) { f->detach(fPCI); f->release(); FBLOG("second framebuffer: start failed (the pipe stays lit, unused)"); return; }
	fSecond = f;
	FBLOG("second framebuffer: published late as head 1 -- NMXAccel registers a head published after its arm on its own with "
	      "nmxheads=1 (else debug.nvaccelfb=3); WindowServer composites it once it re-reads NMXAGDC's head list (its restart)");
}

// no pipe for the second framebuffer's sink any more (gone after a wake, or the pipe would not light): report it
// disconnected. A no-op when it already is. Caller holds fPipeBLock.
void IntelXeFramebuffer::pipeBDisconnected(const char *why)
{
	if (!fSecond || !fSecond->connected() || fPipeB >= 0) return;
	FBLOG("second framebuffer: %s: reported disconnected", why);
	fSecond->setConnected(false);
}

void IntelXeFramebuffer::pipeBStopSecond()
{
	if (!fSecond) return;
	fSecond->setConnected(false);
	fSecond->IOService::terminate(0);
	OSSafeReleaseNULL(fSecond);
}

// the second framebuffer's display sleep: plane 1 of the second pipe off/on (pipe, link and PLL stay up)
void IntelXeFramebuffer::pipeBBlank(bool off)
{
	if (!fRegs || fPipeB < 0) return;
	dispLock();
	if (fPipeB >= 0) {
		const uint32_t ctl = rd(PLANE_CTL(fPipeB));
		wr(PLANE_CTL(fPipeB), off ? ctl & ~kEnable : ctl | kEnable);
		wr(PLANE_SURF(fPipeB), rd(PLANE_SURF(fPipeB)));
	}
	dispUnlock();
}

// Hotplug on another port, the first scan after start, the wake: rescan, take the pipe down if its sink went, light it
// for a new one
void IntelXeFramebuffer::pipeBWork()
{
	if (fStopping || !fPipeBAllowed || fPipe < 0 || fSysSuspended || !fPipeBLock) return;
	IOSleep(500);   // a sink's AUX after its hotplug pulse (replugFire waits as long)
	if (fStopping) return;
	IOLockLock(fPipeBLock);
	connScan();
	if (fPipeB >= 0) {
		const Conn &c = fConn[fPipeBConn];
		if (c.live && c.dp) { IOLockUnlock(fPipeBLock); return; }
		(void)pipeBOff("sink gone");
		fPipeBWanted = false;
	}
	bool any = false;
	for (int i = 0; i < intelxeplan::kNumPorts; i++) if (!fConn[i].panel && fConn[i].dp && fConn[i].modeOk) any = true;
	if (any) { fPipeBResult = pipeBOn(fPipeBWanted ? "after the wake" : "sink found"); fPipeBWanted = fPipeBResult == 0; }
	else if (fPipeBWanted) { FBLOG("second pipe: the sink is gone after the wake"); fPipeBWanted = false; }
	pipeBDisconnected(any ? "the pipe did not light" : "no sink");   // still connected from before a sleep, but nothing lit
	IOLockUnlock(fPipeBLock);
}

// debug.intelxefb_pipeb: 1 scan + light, 0 off, 2 scan only
int IntelXeFramebuffer::pipeBCommand(int v)
{
	if (!fPipeBAllowed || !fPipeBLock) return EPERM;
	IOLockLock(fPipeBLock);
	int r = 0;
	if (v == 0) { fPipeBWanted = false; r = pipeBOff("debug.intelxefb_pipeb=0"); pipeBDisconnected("debug.intelxefb_pipeb=0"); }
	else {
		connScan();
		if (v == 1) { r = pipeBOn("debug.intelxefb_pipeb=1"); fPipeBResult = r; fPipeBWanted = r == 0 || fPipeBWanted; }
	}
	IOLockUnlock(fPipeBLock);
	return r;
}

// debug.intelxefb_pipeb_plan: pipe A recomputed from its own timing next to its live registers, then everything the
// second pipe would be programmed with for the mode in debug.intelxefb_pipeb_mode. Registers are read, nothing is
// written, no AUX transaction: the connectors are the last scan's (only with -intelxefb_pipeb).
int IntelXeFramebuffer::pipeBPlanText(char *b, int n)
{
	using namespace intelxeplan;
	Txt t = { b, n, 0 };
	b[0] = 0;
	if (!fRegs || fPipe < 0) { t.p("no pipe adopted\n"); return t.len; }
	LiveState *s = (LiveState *)IOMallocZero(sizeof(LiveState));
	Plan *pl = (Plan *)IOMallocZero(sizeof(Plan));
	if (!s || !pl) { if (s) IOFree(s, sizeof *s); if (pl) IOFree(pl, sizeof *pl); t.p("no memory\n"); return t.len; }
	const int pb = fPipe == 0 ? 1 : 0;
	planLive(s, pb);
	t.p("second-pipe plan -- dry run: registers read, nothing written, no AUX (Linux v6.18 arithmetic, docs/design/DESIGN-second-pipe.md)\n");
	t.p("knob -intelxefb_pipeb: %s; second pipe: %s; second framebuffer: %s\n", fPipeBAllowed ? "on" : "off", fPipeB >= 0 ? "lit" : "off",
	    !fSecond ? "not published" : fSecond->connected() ? "published, connected" : "published, disconnected");
	if (fConnScanned) {
		for (int i = 0; i < kNumPorts; i++) {
			const Conn &c = fConn[i];
			Port p; (void)portInfo(kPorts[i], &p);
			t.p("  %-3s %s%s", p.name, c.panel ? "the panel" : c.live ? "live" : "-", c.dp ? ", DP" : "");
			if (c.dp) t.p(" rev %x max 0x%02x x%u, \"%s\", %s %ux%u %u kHz", c.dpcd[0], c.dpcd[1], c.dpcd[2] & 0x1F, c.name,
			              c.modeOk ? "mode" : "NO mode", c.mode.hdisplay, c.mode.vdisplay, c.mode.clock);
			t.p("\n");
		}
	} else t.p("connectors: not scanned (knob off); SDEISR hotplug live 0x%08x\n", rd(SDEISR) & kHpdPins);
	// the request
	char spec[sizeof gPipeBMode];
	strlcpy(spec, gPipeBMode, sizeof spec);
	bool ok = true;
	int port = -1;
	if (char *colon = strchr(spec, ':')) {
		*colon = 0;
		port = portFromName(colon + 1);
		if (port < 0) { t.p("debug.intelxefb_pipeb_mode: port '%s' is not a, tc1-tc4\n", colon + 1); ok = false; }
	}
	const uint32_t selA = (s->funcCtl >> 27) & 0xF;
	const Conn *c = nullptr;
	if (port < 0)
		for (int i = 0; i < kNumPorts && port < 0; i++) if (!fConn[i].panel && fConn[i].dp) { port = fConn[i].port; c = &fConn[i]; }
	if (port < 0)
		for (int i = 0; i < kNumPorts && port < 0; i++) if ((uint32_t)kPorts[i] + 1 != selA) port = kPorts[i];
	if (!c) for (int i = 0; i < kNumPorts; i++) if (fConn[i].port == port && fConn[i].dp) c = &fConn[i];
	Timing mode = {};
	char src[64] = "";
	if (ok && !spec[0]) {
		mode = timingFromRegs(s->htotal, s->hsync, s->vtotal, s->vsync, s->funcCtl,
		                      s->linkN ? divRoundUp((uint64_t)(s->linkM & 0xFFFFFF) * s->valfreq, s->linkN & 0xFFFFFF) : 0);
		strlcpy(src, "pipe A's own timing (a second monitor like the first)", sizeof src);
	} else if (ok && !strcmp(spec, "edid")) {
		if (c && c->modeOk) { mode = c->mode; snprintf(src, sizeof src, "the EDID of the sink on the port"); }
		else { t.p("debug.intelxefb_pipeb_mode=edid: no scanned sink with a usable EDID mode on that port\n"); ok = false; }
	} else if (ok) {
		unsigned w = 0, h = 0, r = 0;
		const char *q = spec;
		while (*q >= '0' && *q <= '9') w = w * 10 + (unsigned)(*q++ - '0');
		if (*q == 'x') q++;
		while (*q >= '0' && *q <= '9') h = h * 10 + (unsigned)(*q++ - '0');
		if (*q == '@') q++;
		while (*q >= '0' && *q <= '9') r = r * 10 + (unsigned)(*q++ - '0');
		const bool normal = *q == 'n';
		intelxeedid::Mode m = {};
		if (w < 320 || h < 200 || r < 23 || w > 8192 || h > 8192 || r > 500 || !intelxeedid::cvt((int)w, (int)h, (int)r, !normal, &m)) {
			t.p("debug.intelxefb_pipeb_mode '%s': not WxH@Hz[n]\n", spec); ok = false;
		} else {
			mode = { (uint32_t)m.clock, (uint32_t)m.hdisplay, (uint32_t)m.hsync_start, (uint32_t)m.hsync_end, (uint32_t)m.htotal,
			         (uint32_t)m.vdisplay, (uint32_t)m.vsync_start, (uint32_t)m.vsync_end, (uint32_t)m.vtotal,
			         (m.flags & intelxeedid::kPHSync) != 0, (m.flags & intelxeedid::kPVSync) != 0 };
			snprintf(src, sizeof src, "drm_cvt_mode %ux%u@%u%s", w, h, r, normal ? "" : " reduced blanking");
		}
	}
	uint32_t rate = 810000, lanes = 4;
	if (c) { rate = umin(rateFromBwCode(c->dpcd[1]), 810000); lanes = umin(c->dpcd[2] & 0x1Fu, 4); }
	const Request q = { pb, port, mode, rate, lanes };
	const int e = computePlan(*s, q, pl);
	// pipe A
	const PipePlan &a = pl->a;
	if (e != kPipeA) {
		t.p("pipe %c (live, %s): output %s (PHY %c), x%u at %u kHz, %ux%u, htotal %u vtotal %u, sync %c%c, pixel clock %u kHz (%u.%03u Hz)\n",
		    'A' + fPipe, s->transconf & kEnable ? "on" : "off", pl->portA.name, 'A' + pl->portA.phy, a.link.lanes, a.link.rate,
		    a.t.hdisplay, a.t.vdisplay, a.t.htotal, a.t.vtotal, a.t.phsync ? '+' : '-', a.t.pvsync ? '+' : '-', a.t.clock,
		    refreshMilliHz(a.t) / 1000, refreshMilliHz(a.t) % 1000);
		t.p("  M/N      live  DATA_M 0x%08x DATA_N 0x%08x LINK_M 0x%08x LINK_N 0x%08x\n", s->dataM, s->dataN, s->linkM, s->linkN);
		t.p("           Linux DATA_M 0x%08x DATA_N 0x%08x LINK_M 0x%08x LINK_N 0x%08x  [%s]\n", dataMReg(a.mn), a.mn.dataN, a.mn.linkM,
		    a.mn.linkN, dataMReg(a.mn) == s->dataM && a.mn.dataN == (s->dataN & 0xFFFFFF) && a.mn.linkM == (s->linkM & 0xFFFFFF) &&
		    a.mn.linkN == (s->linkN & 0xFFFFFF) ? "match" : "DIFF (the GOP's choice of N; both describe the same ratio if close)");
		t.p("  timings  live  HTOTAL 0x%08x HBLANK 0x%08x HSYNC 0x%08x VTOTAL 0x%08x VBLANK 0x%08x VSYNC 0x%08x SCL %u PIPESRC 0x%08x\n",
		    s->htotal, s->hblank, s->hsync, s->vtotal, s->vblank, s->vsync, s->scl, s->pipesrc);
		t.p("           Linux HTOTAL 0x%08x HBLANK 0x%08x HSYNC 0x%08x VTOTAL 0x%08x VBLANK 0x%08x VSYNC 0x%08x SCL %u PIPESRC 0x%08x  [%s/%s]\n",
		    a.regs.htotal, a.regs.hblank, a.regs.hsync, a.regs.vtotal, a.regs.vblank, a.regs.vsync, a.regs.scl, a.regs.pipesrc,
		    same(a.regs.hblank, s->hblank), same(a.regs.vblank, s->vblank));
		t.p("  WM_LINETIME live 0x%x Linux 0x%x [%s]; TRANS_MSA_MISC 0x%x (Linux 0x%x); PIPE_MISC 0x%x (Linux 0x%x)\n", s->linetime & 0x1FF,
		    a.linetime, same(s->linetime & 0x1FF, a.linetime), rd(TR_(fPipe, 0x60410)), kMsaMisc, rd(TR_(fPipe, 0x70030)), kPipeMisc);
		const Latency lat = latencyFromRegs(s->lat01, s->lat23, s->lat45, s->latSagv);
		t.p("  latency us %u %u %u %u %u %u %u %u, SAGV block %u us (MTL_LATENCY 0x%08x 0x%08x 0x%08x, SAGV 0x%08x)\n", lat.us[0], lat.us[1],
		    lat.us[2], lat.us[3], lat.us[4], lat.us[5], lat.us[6], lat.us[7], lat.sagvUs, s->lat01, s->lat23, s->lat45, s->latSagv);
		t.p("  DDB live: plane 1 %u-%u, cursor %u-%u (CURCNTR 0x%x)\n", pl->aPlaneNow.start, pl->aPlaneNow.end, pl->aCursorNow.start,
		    pl->aCursorNow.end, s->curCtl);
		t.p("  DDB Linux, one pipe: pipe %u-%u, plane 1 %u-%u, cursor %u-%u (%u blocks), level %d fits, vblank allows %d, SAGV WM %s\n",
		    a.ddb.start, a.ddb.end, a.wm.plane.start, a.wm.plane.end, a.wm.cursor.start, a.wm.cursor.end, a.wm.cursorBlocks, a.wm.ddbLevel,
		    a.wm.vblankMaxLevel, a.wm.useSagv ? "used" : "not used");
		for (int l = 0; l < kWmLevels; l++) {
			const uint32_t lin = wmReg(a.wm.wm[l]), sg = l == 0 ? wmReg(a.wm.sagv0) : lin;
			t.p("  PLANE_WM %d  live 0x%08x  Linux 0x%08x%s", l, s->planeWm[l], lin, l == 0 ? "" : "\n");
			if (l == 0) t.p(" / SAGV 0x%08x  [%s]\n", sg, s->planeWm[0] == lin ? "match" : s->planeWm[0] == sg ? "match (SAGV)" : "DIFF");
		}
		t.p("  PLANE_WM T  live 0x%08x  Linux 0x%08x / SAGV 0x%08x  [%s]  (levels: [lines/blocks] differ when the GOP's DDB differs)\n",
		    s->planeWmTrans, wmReg(a.wm.trans), wmReg(a.wm.sagvTrans), s->planeWmTrans == wmReg(a.wm.trans) ? "match" :
		    s->planeWmTrans == wmReg(a.wm.sagvTrans) ? "match (SAGV)" : "DIFF");
		t.p("  MBUS_CTL 0x%08x (joined %s), PIPE_MBUS_DBOX_CTL live 0x%08x Linux 0x%08x [%s], DBUF_CTL S1-S4 0x%08x 0x%08x 0x%08x 0x%08x "
		    "(powered 0x%x, min tracker %u, Linux %u)\n", s->mbusCtl, pl->joinedNow ? "yes" : "no", s->dbox[fPipe], a.dbox,
		    same(s->dbox[fPipe], a.dbox), s->dbufCtl[0], s->dbufCtl[1], s->dbufCtl[2], s->dbufCtl[3], pl->slicesNow, pl->trackerNow,
		    dbufMinTracker(pl->joinedNow));
		const PmDemand &pm = pl->pmdNow;
		t.p("  PMDEMAND 0x%08x 0x%08x: pipes %u dbufs %u phys %u plls %u ddiclk %u MHz cdclk %u MHz voltage %u qclk bw 0x%x scalers %u\n",
		    s->pmd0, s->pmd1, pm.pipes, pm.dbufs, pm.phys, pm.plls, pm.ddiclkMhz, pm.cdclkMhz, pm.voltage, pm.qclkBw, pm.scalers);
		t.p("  CDCLK_CTL 0x%08x, HSW_PWR_WELL_CTL2 0x%08x, memory: MTL_MEM_SS_INFO_GLOBAL 0x%08x, derated %u MB/s\n", s->cdclkCtl,
		    s->pwCtl2, s->memGlobal, pl->bwMBps);
	}
	// the second output
	if (ok) {
		const PipePlan &bp = pl->b;
		t.p("second output: %s -- %ux%u htotal %u vtotal %u, %u kHz (%u.%03u Hz), port %s, sink %s %u kHz x%u\n", src, mode.hdisplay,
		    mode.vdisplay, mode.htotal, mode.vtotal, mode.clock, refreshMilliHz(mode) / 1000, refreshMilliHz(mode) % 1000,
		    pl->portB.name ? pl->portB.name : "?", c ? "caps (scanned)" : "caps ASSUMED", rate, lanes);
		if (e == kOk || (e >= kCdclk && e != kPipeA)) {
			t.p("  pipe/transcoder %c, power well %d (request 0x%08x), PHY %c, AUX 0x%x, HPD SDE bit %d, TRANS_CLK_SEL 0x%08x\n", 'A' + pb,
			    pl->pwIdx, pwReq(pl->pwIdx), 'A' + pl->portB.phy, pl->portB.aux, __builtin_ctz(pl->portB.hpd), pl->portB.clkSel);
			t.p("  link %u kHz x%u at 24 bpp (C20 DP table %s), TRANS_DDI_FUNC_CTL 0x%08x (+ enable), SSC if pipe A has it and the sink allows\n",
			    bp.link.rate, bp.link.lanes, bp.link.rate == 162000 ? "RBR" : bp.link.rate == 270000 ? "HBR1" : bp.link.rate == 540000 ? "HBR2" : "HBR3",
			    pl->portB.ddiSel << 27 | 2u << 24 | (bp.link.lanes == 4 ? 3u : bp.link.lanes - 1) << 1 | bp.regs.funcSync);
			t.p("  HTOTAL 0x%08x HBLANK 0x%08x HSYNC 0x%08x VTOTAL 0x%08x VBLANK 0x%08x VSYNC 0x%08x SCL %u PIPESRC 0x%08x\n", bp.regs.htotal,
			    bp.regs.hblank, bp.regs.hsync, bp.regs.vtotal, bp.regs.vblank, bp.regs.vsync, bp.regs.scl, bp.regs.pipesrc);
			t.p("  DATA_M 0x%08x DATA_N 0x%08x LINK_M 0x%08x LINK_N 0x%08x, WM_LINETIME 0x%x, PIPE_MISC 0x%x, TRANS_MSA_MISC 0x%x, "
			    "PIPE_CHICKEN |= 0x%08x\n", dataMReg(bp.mn), bp.mn.dataN, bp.mn.linkM, bp.mn.linkN, bp.linetime, kPipeMisc, kMsaMisc, kPipeChickenSet);
			t.p("  DBUF: pipe A slices 0x%x (MBUS-relative %u-%u; its live plane %u-%u and cursor %u-%u %s), second pipe slices 0x%x "
			    "(relative %u-%u, bank +%u)\n", pl->a2.slices, pl->a2.ddb.start, pl->a2.ddb.end, pl->aPlaneNow.start, pl->aPlaneNow.end,
			    pl->aCursorNow.start, pl->aCursorNow.end, pl->aKeepsDdb ? "stay as they are" : "do NOT fit", bp.slices, bp.ddb.start,
			    bp.ddb.end, bp.mbusOffset);
			t.p("  DDB: plane 1 %u-%u, cursor %u-%u (%u blocks, cursor off), level %d fits, vblank allows %d, SAGV watermarks %s\n",
			    bp.wm.plane.start, bp.wm.plane.end, bp.wm.cursor.start, bp.wm.cursor.end, bp.wm.cursorBlocks, bp.wm.ddbLevel,
			    bp.wm.vblankMaxLevel, bp.wm.useSagv ? "in level 0/TRANS" : "not used");
			t.p("  PLANE_WM 0-7 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x 0x%08x TRANS 0x%08x, PLANE_BUF_CFG 0x%08x, CUR_BUF_CFG 0x%08x\n",
			    bp.wm.regWm[0], bp.wm.regWm[1], bp.wm.regWm[2], bp.wm.regWm[3], bp.wm.regWm[4], bp.wm.regWm[5], bp.wm.regWm[6], bp.wm.regWm[7],
			    bp.wm.regTrans, bp.wm.regBufCfg, bp.wm.regCurBufCfg);
			t.p("  DBUF slices 0x%x -> 0x%x, min tracker %u -> %u, MBUS_CTL 0x%08x -> 0x%08x, PIPE_MBUS_DBOX_CTL A 0x%08x -> 0x%08x, second 0x%08x\n",
			    pl->slicesNow, pl->slicesNow | pl->slicesAfter, pl->trackerNow, pl->trackerAfter, s->mbusCtl, pl->mbusCtlAfter, s->dbox[fPipe],
			    bp.dbox, bp.dbox);
			t.p("  PMDEMAND 0x%08x 0x%08x -> before 0x%08x 0x%08x -> after 0x%08x 0x%08x (pipes %u dbufs %u phys %u plls %u ddiclk %u MHz scalers %u)\n",
			    s->pmd0, s->pmd1, pl->pmd0Pre, pl->pmd1Pre & ~PMDEMAND_REQ, pl->pmd0Post, pl->pmd1Post & ~PMDEMAND_REQ, pl->pmdPost.pipes,
			    pl->pmdPost.dbufs, pl->pmdPost.phys, pl->pmdPost.plls, pl->pmdPost.ddiclkMhz, pl->pmdPost.scalers);
			t.p("  CDCLK %u kHz: needed pipe A %u, second %u; memory bandwidth %u of %u MB/s%s\n", kBmgCdclk, a.minCdclk, bp.minCdclk,
			    pl->needMBps, pl->bwMBps, pl->bwMBps ? "" : " (no QGV information)");
		}
		t.p("verdict: %s%s\n", e ? "REFUSED: " : "OK -- this is what -intelxefb_pipeb would program", e ? errName(e) : "");
	}
	IOFree(s, sizeof *s);
	IOFree(pl, sizeof *pl);
	return t.len;
}

static int intelxefb_pipeb_plan SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	const int cap = 16384;
	char *b = (char *)IOMalloc(cap);
	if (!b) return ENOMEM;
	int n = gThumbFB->pipeBPlanText(b, cap);
	if (n < 0) n = 0;
	if (n > cap - 1) n = cap - 1;
	const int err = SYSCTL_OUT(req, b, (size_t)n + 1);
	IOFree(b, cap);
	return err;
}

static int intelxefb_pipeb SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gThumbFB->pipeBState();
	const int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v < 0 || v > 2) return EINVAL;
	return gThumbFB->pipeBCommand(v);
}

static int intelxefb_thumb SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	enum { W = 320, H = 180 };
	if (!gThumbFB) return ENODEV;
	if (!req->oldptr) return SYSCTL_OUT(req, nullptr, W * H * 4);
	uint32_t *buf = (uint32_t *)IOMalloc(W * H * 4);
	if (!buf) return ENOMEM;
	int err = gThumbFB->thumb(buf, W, H) ? SYSCTL_OUT(req, buf, W * H * 4) : ENODEV;
	IOFree(buf, W * H * 4);
	return err;
}

static int intelxefb_peek SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	uint32_t off = gPeekOff, v = 0;
	if (req->newptr) {
		int err = SYSCTL_IN(req, &off, sizeof(off));
		if (err) return err;
		if (!gThumbFB->peek(off, &v)) return EINVAL;
		gPeekOff = off;
	}
	if (!gThumbFB->peek(gPeekOff, &v)) return EINVAL;
	return SYSCTL_OUT(req, &v, sizeof(v));
}

static int intelxefb_mode SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int mode = gThumbFB->currentMode();
	int err = sysctl_handle_int(oidp, &mode, 0, req);
	if (err || !req->newptr) return err;
	return gThumbFB->testMode(mode) ? 0 : EINVAL;
}

static int intelxefb_poke SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	uint32_t v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	return gThumbFB->poke(gPeekOff, v) ? 0 : EINVAL;
}

static int intelxefb_dpcd SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = -1;
	if (req->newptr) {
		uint32_t in = 0;
		int err = SYSCTL_IN(req, &in, sizeof(in));
		if (err) return err;
		if (in & 0x40000000u) {
			uint8_t b = (uint8_t)in;
			gDpcdAddr = (in >> 8) & 0xFFFFF;
			if (gThumbFB->dpcd(gDpcdAddr, &b, 1, true)) return EIO;
		} else gDpcdAddr = in & 0xFFFFF;
	}
	uint8_t b = 0;
	if (!gThumbFB->dpcd(gDpcdAddr, &b, 1, false)) v = b;
	return SYSCTL_OUT(req, &v, sizeof(v));
}

static int intelxefb_link SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gLinkResult;
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v < 0 || v > 6) return EINVAL;
	if (!OSCompareAndSwap(0, 1, &gLinkBusy)) return EBUSY;
	gLinkResult = gThumbFB->linkAction(v);
	gLinkBusy = 0;
	return 0;
}

static int intelxefb_linklog SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	return SYSCTL_OUT(req, IntelXeLink::logText(), IntelXeLink::logLength() + 1);
}

static int intelxefb_crop SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	enum { W = 256, H = 128 };
	if (!gThumbFB) return ENODEV;
	if (!req->oldptr) return SYSCTL_OUT(req, nullptr, W * H * 4);
	uint32_t *buf = (uint32_t *)IOMalloc(W * H * 4);
	if (!buf) return ENOMEM;
	int err = gThumbFB->crop(buf, gCropXY >> 16, gCropXY & 0xFFFF, W, H) ? SYSCTL_OUT(req, buf, W * H * 4) : ENODEV;
	IOFree(buf, W * H * 4);
	return err;
}

static int intelxefb_vrr SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gThumbFB->vrrState();
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v < -1 || v > 1) return EINVAL;
	if (!OSCompareAndSwap(0, 1, &gLinkBusy)) return EBUSY;
	err = gThumbFB->vrrSet(v);
	gLinkBusy = 0;
	return err;
}

static int intelxefb_vrrlog SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!req->oldptr) return SYSCTL_OUT(req, nullptr, sizeof(uint32_t) + sizeof gVrrLog);
	uint32_t idx = gVrrLogIdx;
	int err = SYSCTL_OUT(req, &idx, sizeof idx);
	return err ? err : SYSCTL_OUT(req, gVrrLog, sizeof gVrrLog);
}

static int intelxefb_vrrinfo SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	char *b = (char *)IOMalloc(2048);
	if (!b) return ENOMEM;
	int n = gThumbFB->vrrInfo(b, 2048);
	if (n < 0) n = 0; if (n > 2047) n = 2047;
	int err = SYSCTL_OUT(req, b, (size_t)n + 1);
	IOFree(b, 2048);
	return err;
}

static int intelxefb_vrr_modes SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gThumbFB->vrrModesState();
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v != 0 && v != 1) return EINVAL;
	gThumbFB->vrrModesSet(v);
	return 0;
}

// ---- power protocol: test, recovery, state (v10e) -------------------------------------------------------------------
// debug.intelxefb_pm=2, only while the display sleeps (power state 1, doze; WindowServer has acknowledged the dim and draws
// nothing): (1) IOGraphics' system-sleep step for this framebuffer -- WillSleep(false), WillSleep(true), pagingState
// off, inside the system and controller gates as IOFramebuffer::systemWork does it; (2) changePowerStateTo(0): the
// framebuffer goes to 0 through IOFramebuffer::setPowerState and IOGraphics' power work, as at system sleep;
// (3) changePowerStateTo(1): leaving 0 the same way, which must bring pagingState back through handleEvent(DidWake)
// on IOGraphics' power thread. PASS = pagingState back with no fallback; otherwise the gated recovery restores it
// (WindowServer is never left waiting) and the result says which step failed. Nothing touches the GPU or the link.
void IntelXeFramebuffer::pmTestRun()
{
	const uint32_t fb0 = fPmFallbacks;
	const uint64_t t0 = mach_absolute_time();
	auto ms = [t0]() { uint64_t ns; absolutetime_to_nanoseconds(mach_absolute_time() - t0, &ns); return (unsigned)(ns / 1000000); };
	int ok = 0;
	IOReturn r = withGates(&IntelXeFramebuffer::sleepStepGated, (intptr_t)&ok, "pm test");
	if (r || !ok) {
		snprintf(fPmTest, sizeof fPmTest, "NOT RUN: sleep step 0x%x (needs paging on and power state 1, now %u)", r, fPowerOrd);
		FBLOG("pm test: %s", fPmTest); fPmTestBusy = 0; return;
	}
	changePowerStateTo(0);
	for (int t = 0; t < 80 && fPowerOrd != 0; t++) IOSleep(100);
	const bool reached0 = fPowerOrd == 0;
	const unsigned t1 = ms();
	changePowerStateTo(1);
	for (int t = 0; t < 80 && !pagingState; t++) IOSleep(100);
	const unsigned t2 = ms();
	const bool paging = pagingState, viaHandleEvent = paging && fPmFallbacks == fb0;
	if (!paging) (void)familyWake("pm test: recovery", -1);
	snprintf(fPmTest, sizeof fPmTest, "%s: power 1->0 %s (%u ms), 0->%u, pagingState %s (%u ms)%s",
	         viaHandleEvent && reached0 ? "PASS" : "FAIL", reached0 ? "done" : "NOT reached (children hold it)", t1,
	         fPowerOrd, paging ? (viaHandleEvent ? "back by handleEvent(DidWake) on IOGraphics' power thread" : "back by the fallback") : "NOT back",
	         t2, paging ? "" : ", recovered by the gated family wake");
	FBLOG("pm test: %s", fPmTest);
	fPmTestBusy = 0;
}

int IntelXeFramebuffer::pmCommand(int cmd)
{
	if (cmd == 1) {   // recovery: only what a stuck wake looks like (paging off, framebuffer powered)
		const IOReturn r = familyWake("debug.intelxefb_pm=1", -1);
		if (r == kIOReturnSuccess) { fPmRecovers++; return 0; }
		FBLOG("pm: recovery: 0x%x (%s)", r, r == kIOReturnNoResources ? "paging is on, nothing waits" :
		      r == kIOReturnNotReady ? "framebuffer in power state 0" : "gates busy or no gates");
		return r == kIOReturnNoResources ? 0 : EBUSY;
	}
	if (cmd == 2) {
		if (!opened || !pagingState || fPowerOrd != 1 || !fPmTestCall) return EBUSY;   // display must be asleep (doze)
		if (!OSCompareAndSwap(0, 1, (volatile UInt32 *)&fPmTestBusy)) return EBUSY;
		snprintf(fPmTest, sizeof fPmTest, "running");
		thread_call_enter(fPmTestCall);
		return 0;
	}
	return EINVAL;
}

int IntelXeFramebuffer::pmInfo(char *b, int n)
{
	// cursor registers of this pipe (pipe-relative offsets of pipe A, 0x1000 apart)
	auto cr = [&](uint32_t a) -> uint32_t { return (fRegs && fPipe >= 0) ? rd(a + (uint32_t)fPipe * 0x1000u) : 0u; };
	constexpr uint32_t CURCNTR = 0x70080u, CURBASE = 0x70084u, CURPOS = 0x70088u, CUR_BUF_CFG = 0x7017Cu, CUR_WM_0 = 0x70140u,
	                   CUR_FBC_CTL = 0x700A0u;
	return snprintf(b, (size_t)n, "power %u (applied ->0 %u ->1 %u ->2 %u, before open %u, off IOGraphics' threads %u), opened %d, "
	                "pagingState %d | IOGraphics notifications WillSleep %d/%d DidWake %d/%d (info false/true), DidPowerOn %d, "
	                "WillPowerOff %d | DidWake fallbacks %u, recoveries %u, guard %u (guard %s) | test: %s | system sleep: "
	                "suspends %u, resumes %u (failed %u); colour registers found after the last wake: PLANE_COLOR_CTL 0x%08x "
	                "PIPE_CSC_MODE 0x%08x GAMMA_MODE 0x%08x PLANE_CTL 0x%08x (0xffffffff: no wake yet); now PLANE_COLOR_CTL 0x%08x | "
	                "cursor VRAM 0x%llx (%s), L2 flushes %u, failed %u | cursor image %u: fnv 0x%08x  %08x %08x %08x %08x | "
	                "CURCNTR 0x%08x CURBASE 0x%08x CURSURFLIVE 0x%08x CURPOS 0x%08x CUR_BUF_CFG 0x%08x CUR_WM0 0x%08x CUR_CHICKEN 0x%08x "
	                "CUR_FBC_CTL 0x%08x, PTE[0] 0x%016llx PTE[63] 0x%016llx",
	                fPowerOrd, fPmApplied[0], fPmApplied[1], fPmApplied[2], fPmDirect, fPmOffThread, (int)opened, (int)pagingState,
	                (int)fEvWillSleep[0], (int)fEvWillSleep[1], (int)fEvDidWake[0], (int)fEvDidWake[1], (int)fEvDidPowerOn,
	                (int)fEvWillPowerOff, fPmFallbacks, fPmRecovers, fPmGuardHits, gPmGuard ? "on" : "off", fPmTest,
	                fSuspends, fResumes, fResumeFails, fWakeColorCtl, fWakeCscMode, fWakeGammaMode, fWakePlaneCtl,
	                (fPipe >= 0 && fRegs) ? rd(PLANE_COLOR_CTL(fPipe)) : 0u,
	                fCurDpa, fCurBo ? "IntelXe.kext display buffer" : "fixed fallback", fL2Flushes, fL2Fails,
	                fCurRenders, fCurFnv, fCurW[0], fCurW[1], fCurW[2], fCurW[3],
	                cr(CURCNTR), cr(CURBASE), cr(0x700ACu), cr(CURPOS), cr(CUR_BUF_CFG), cr(CUR_WM_0), cr(0x700A4u), cr(CUR_FBC_CTL),
	                fRegs ? (unsigned long long)pte(kCurGgtt) : 0ull, fRegs ? (unsigned long long)pte(kCurGgtt + 63 * 0x1000) : 0ull);
}

static int intelxefb_pm SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gThumbFB->pagingOn();
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	return gThumbFB->pmCommand(v);
}

static int intelxefb_hpd SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = 0;
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	return gThumbFB->hpdTrigger(v);
}

static int intelxefb_hpdinfo SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	char *b = (char *)IOMalloc(1024);
	if (!b) return ENOMEM;
	int n = gThumbFB->hpdInfo(b, 1024);
	if (n < 0) n = 0; if (n > 1023) n = 1023;
	int err = SYSCTL_OUT(req, b, (size_t)n + 1);
	IOFree(b, 1024);
	return err;
}

static int intelxefb_pminfo SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	char *b = (char *)IOMalloc(2048);
	if (!b) return ENOMEM;
	int n = gThumbFB->pmInfo(b, 2048);
	if (n < 0) n = 0; if (n > 2047) n = 2047;
	int err = SYSCTL_OUT(req, b, (size_t)n + 1);
	IOFree(b, 2048);
	return err;
}

static int intelxefb_irq SYSCTL_HANDLER_ARGS
{
	if (!rootOnly()) return EPERM;
	if (!gThumbFB) return ENODEV;
	int v = gThumbFB->irqState();
	int err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v != 0 && v != 1) return EINVAL;
	return gThumbFB->irqSet(v);
}
