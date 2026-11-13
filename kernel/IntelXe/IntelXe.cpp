// SPDX-License-Identifier: MIT
//  IntelXe: bring-up driver for Intel Xe GPUs, first the Intel Arc B580 (Battlemage BMG-G21, 8086:E20B), phase 1 of docs/research/arc-b580.md.
//
//  It matches the card by ID only (SSDT-ARCOFF's class-code FFFFFFFF keeps IONDRVFramebuffer away and does not stop
//  IOPCIMatch) and does nothing at boot. Every hardware step is started by hand:
//
//    sysctl debug.intelxe.state          report of the last step (any user)
//    sudo sysctl debug.intelxe.step=1    read-only probe: PCI state, then BAR0 registers (IP versions, LMEM/pcode state,
//                                     VRAM range, GuC/WOPCM, temperatures). Nothing is written except the PCI command
//                                     register's memory-decode bit, and only if it was off.
//    sudo sysctl debug.intelxe.step=2    pcode mailbox DGFX_GET_INIT_STATUS, as xe_pcode_ready() does at every Linux probe
//    sudo sysctl debug.intelxe.step=3    GT forcewake, then fuses, PAT and MOCS tables, GuC registers (read only)
//    sudo sysctl debug.intelxe.step=4    GuC load as xe_guc_min_load_for_hwconfig: PAT + MOCS tables, WOPCM, GGTT entries for
//                                     the firmware and a minimal ADS in system memory, params, DMA, wait for READY.
//                                     The GuC is reset (GDRST) first, so the step can be repeated.
//    sudo sysctl debug.intelxe.step=5    GuC status registers only
//    sudo sysctl debug.intelxe.step=6    first batch: BCS0 default state + HWSP, GuC reload with an ADS that enables the
//                                     copy class, CT buffers (H2G/G2H) over self-config, an LRC whose ring holds
//                                     MI_STORE_DATA_IMM to a GGTT page, REGISTER_CONTEXT + SCHED_CONTEXT_MODE_SET, then
//                                     poll the page and dump the G2H messages and engine registers.
//    sudo sysctl debug.intelxe.step=7    PCI config only: BAR registers, the Resizable BAR capability, link status
//    sudo sysctl debug.intelxe.step=8    VRAM (after step 6, needs OpenCore ResizeAppleGpuBars 0 (256 MB BAR2) or 10 (1 GB)): host
//                                     L2/VRAM setup of Wa_16023588340, CPU pattern test through BAR2, then the GPU
//                                     stores into VRAM through a GGTT entry with the DM bit, L2 flush, CPU reads it back.
//    sudo sysctl debug.intelxe.step=9    PPGTT (after step 8): a 5-level page table in VRAM (BMG vm_max_level 4, va_bits 48)
//                                     mapping VA 0x100000 to a system page and 0x101000 to a VRAM page, context 1 with
//                                     that root and ASID 1, two PPGTT stores, CPU checks both pages.
//    sudo sysctl debug.intelxe.step=13   media GT probe, read only apart from its forcewake: VDBOX/VEBOX enable fuses,
//                                     media GuC and HuC status (the standalone media GT, xe_gt_mmio_init +0x380000)
//    sudo sysctl debug.intelxe.step=14   media GuC: VCS/VECS default state and media workarounds, GuC load on the media GT
//                                     (its own ADS with the video classes, PAT/MOCS, WOPCM, misc pages), media CT.
//                                     Independent of the primary GT; repeatable (resets the media GuC first).
//    sudo sysctl debug.intelxe.step=15   after 14: MI_STORE_DATA_IMM on VCS0 (and VCS2) through the media GuC
//    sudo sysctl debug.intelxe.step=16   media GT frequency: caps, RPa/RPe, requested and actual, SLPC state
//    sudo sysctl debug.intelxe.step=17   GT SLPC: send debug.intelxe.slpc_{min,max,ignore_eff,strategy,profile} (+ the raw
//                                     slpc_param_id/slpc_param_val), then QUERY_TASK_STATE
//    sudo sysctl debug.intelxe.step=18   GT power report: C-state, RC6/MC6 residency over 100 ms, RC_CONTROL, POWERGATE_ENABLE
//    sudo sysctl debug.intelxe.step=19   apply debug.intelxe.gucrc (GuC RC host/firmware; bring-up sends firmware control,
//                                     as Linux xe_guc_rc_enable does) and debug.intelxe.rc6 (RC_CONTROL)
//    sudo sysctl debug.intelxe.step=20   read BAR0 register debug.intelxe.mmio_addr
//    sudo sysctl debug.intelxe.step=21   rb7p, read only: every engine's RING_IDLEDLY / RING_PWRCTX_MAXCNT (and what the
//                                     first bring-up's adjust_idledly did), RING_PSMI_CTL, NONPRIV slots,
//                                     FF_SLICE_CS_CHICKEN1, TDL_CHICKEN (media engines only when the media GT is up)
//    sudo sysctl debug.intelxe.step=22   rb7p: adjust_idledly once more on every engine that is up (Linux runs it at probe
//                                     only; this is a test lever for what a resume or GT reset left behind)
//    sudo sysctl debug.intelxe.fw_hold=1 hold GT forcewake (keeps the GT out of RC6) until fw_hold=0
//
//  Boot argument -intelxe_off keeps the driver from starting. Register offsets and fields: Linux v7.2 drivers/gpu/drm/xe
//  regs/xe_regs.h, regs/xe_gt_regs.h, regs/xe_guc_regs.h, regs/xe_pcode_regs.h, xe_pcode_api.h, xe_pat.c, xe_mocs.c,
//  xe_wopcm.c, xe_ggtt.c, xe_guc.c, xe_uc_fw.c, xe_guc_ads.c (MIT). The firmware is xe/bmg_guc_70.bin, built in.
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <IOKit/pwr_mgt/RootDomain.h>
#include <IOKit/IOKitKeys.h>
#include <IOKit/IODeviceTreeSupport.h>
#include <mach/kmod.h>
#include <pexpert/pexpert.h>
#include <sys/proc.h>
#include "xe_fd.h"   // rb7t
#include <sys/sysctl.h>
#include "guc_abi.h"
#include "lrc.h"
#include "guc_fw.inc"   // generated by build.py: kGucFw[], kGucFwSize
// XeProbe (xe_probe.cpp) loads this image when it probes a Battlemage, rather than carrying a second copy.
const uint8_t *intelxeGucImage(uint32_t *size) {
	*size = kGucFwSize;
	return kGucFw;
}
#include "huc_fw.inc"   // generated by build.py: kHucFw[], kHucFwSize

#include "IntelXe.h"
#include "xe_aspm.h"   // PCIe capability register offsets
#include "uapi/intelxe_bootargs.h"   // the boot-arg names before the rename, for one release

OSDefineMetaClassAndStructors(IntelXe, IOService)

IntelXe *gDev;
const XePlatformDesc *gXePlat;
uint16_t gXePlatId;
IOLock *gDevLock, *gH2gLock[2], *gG2hLock[2], *gFwLock, *gSayLock, *gDispLock;
// xe->pmt.lock (xe_vsec.c:160): one PMT read at a time, the re-mapper index and the energy meter with it
static IOLock *gPmtLock;
// tile->pcode.lock (xe_pcode.c:111, 120-147): one pcode mailbox transaction at a time (the init-status polls at start
// and resume, the power-limit reads of debug.intelxe.sensors)
static IOLock *gPcodeLock;
XeEngine gEngines[8];
uint32_t gNumEngines;
XeEngine gMediaEngines[4];
uint32_t gNumMediaEngines;
int gUsm = 1;           // rb7j: GuC UM (page fault) queues on the primary GT (-intelxe_nousm: 0)
uint64_t gUmDpa;        // their VRAM (set at the first full primary GuC load, then fixed)
int gDirectWanted = 1;  // direct submission switched on once the device is up (-intelxe_nodirect: 0; debug.intelxe.direct changes it later)
int gVideoWanted = 1;   // the media GT's VCS engines listed and its GuC/HuC/SLPC loaded at bring-up (-intelxe_novideo: 0)
int gMediaSlpc = 1;
static char gState[4096] = "idle: no step run yet";
static int gStep;
static int gFreqMhz = 2850, gFreqKeepManual;
static size_t gLen;

void say(const char *fmt, ...) {
	IOLockLock(gSayLock);
	if (gLen < sizeof(gState) - 1) {
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(gState + gLen, sizeof(gState) - gLen, fmt, ap);
		va_end(ap);
		if (n > 0) gLen = gLen + n < sizeof(gState) ? gLen + n : sizeof(gState) - 1;
	}
	IOLockUnlock(gSayLock);
}

// Steps that reload or reset firmware, reprogram the GT or submit their own contexts would pull the GPU from under
// live clients: only in --debug builds. The others read state, or set what Linux exposes at runtime (SLPC, GuC RC).
static bool stepAllowed(int step) {
#if INTELXE_DEBUG
	return true;
#else
	switch (step) {
		case 1: case 2: case 3: case 5: case 7: case 10: case 12: case 13: case 16: case 17: case 18: case 19: case 20:
		case 21: case 22:
			return true;
		default:
			return false;
	}
#endif
}

static int stepHandler SYSCTL_HANDLER_ARGS {
	int err = sysctl_handle_int(oidp, &gStep, 0, req);
	if (err || !req->newptr) return err;
	if (!gDev) return ENODEV;
	if (!stepAllowed(gStep)) return EPERM;
	IOLockLock(gDevLock);
	err = gDev->runStep(gStep);
	IOLockUnlock(gDevLock);
	return err;
}

static int stateHandler SYSCTL_HANDLER_ARGS {
	IOLockLock(gSayLock);
	int err = SYSCTL_OUT(req, gState, strlen(gState) + 1);
	IOLockUnlock(gSayLock);
	return err;
}

int xeDispCheck(char *out, size_t len);
static int dispCheckHandler SYSCTL_HANDLER_ARGS {   // rb7d diagnostics (root): see xeDispCheck
	if (req->p && proc_suser(req->p)) return EPERM;
	char *buf = static_cast<char *>(IOMalloc(8192));
	if (!buf) return ENOMEM;
	const int n = gDev ? xeDispCheck(buf, 8192) : snprintf(buf, 8192, "no device\n");
	int err = SYSCTL_OUT(req, buf, (size_t)(n > 0 ? (n < 8191 ? n : 8191) : 0) + 1);
	IOFree(buf, 8192);
	return err;
}

static int statsHandler SYSCTL_HANDLER_ARGS {
	constexpr size_t kLen = 12288;   // rb8e: WAS 4096, which the rb8d and rb8e lines would overflow (the tail was cut);
	                                 // rb8f: 8192 before its two lines
	char *buf = static_cast<char *>(IOMalloc(kLen));
	if (!buf) return ENOMEM;
	const int n = gDev ? xeStatsString(buf, kLen) : snprintf(buf, kLen, "no device\n");
	int err = SYSCTL_OUT(req, buf, (size_t)(n > 0 ? (n < (int)kLen - 1 ? n : (int)kLen - 1) : 0) + 1);
	IOFree(buf, kLen);
	return err;
}

static int vramHandler SYSCTL_HANDLER_ARGS {
	constexpr size_t kLen = 16384;
	char *buf = static_cast<char *>(IOMalloc(kLen));
	if (!buf) return ENOMEM;
	const int n = gDev ? xeVramString(buf, kLen) : snprintf(buf, kLen, "no device\n");
	int err = SYSCTL_OUT(req, buf, (size_t)(n < (int)kLen - 1 ? n : (int)kLen - 1) + 1);
	IOFree(buf, kLen);
	return err;
}

// The GuC hwconfig table (GET_HWCONFIG during step 6); DEVICE_QUERY_HWCONFIG returns it to Mesa.
uint8_t gHwconfig[4096];
uint32_t gHwconfigLen;

static int hwconfigHandler SYSCTL_HANDLER_ARGS {
	return SYSCTL_OUT(req, gHwconfig, gHwconfigLen);
}

SYSCTL_NODE(_debug, OID_AUTO, intelxe, CTLFLAG_RW, 0, "IntelXe bring-up");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, step, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, stepHandler, "I", "run a bring-up step");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, state, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, stateHandler, "A", "last report");
int gIrqWanted = 1;
SYSCTL_INT(_debug_intelxe, OID_AUTO, irq, CTLFLAG_RW, &gIrqWanted, 0, "use MSI interrupts (set before the first client)");
int gDeferAsync = 1;
SYSCTL_INT(_debug_intelxe, OID_AUTO, unbind_async, CTLFLAG_RW, &gDeferAsync, 0, "VM_BIND: unbinds free their pages after an asynchronous TLB invalidation; 0 = wait in the ioctl");
SYSCTL_INT(_debug_intelxe, OID_AUTO, hugepages, CTLFLAG_RW, &gHugePages, 0, "new binds: 2 MB / 1 GB leaves and PS64 (0 = 4 KB only; boot-arg -intelxe_nohugepages)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, range_inval, CTLFLAG_RW, &gRangeInval, 0, "unbinds: ranged per-ASID TLB invalidation; 0 = full");
SYSCTL_INT(_debug_intelxe, OID_AUTO, bind_async, CTLFLAG_RW, &gBindAsync, 0, "rb8f: 1 = a VM_BIND with in-fences pending is queued on its bind queue and applied when they signal (xe_pt_update_ops_run via a job); 0 = waited for in the ioctl (default)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, bind_tlb_fence, CTLFLAG_RW, &gBindTlbFence, 0, "rb8cost: 1 = a VM_BIND out-fence includes the TLB invalidation of what it unbound (rb8d, default); 0 = the clears alone (rb8b; DESIGN-rb8-cost.md)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, exec_tlb_dep, CTLFLAG_RW, &gExecTlbDep, 0, "rb8cost: 1 = an EXEC waits for TLB-invalidation in-fences (default); 0 = dropped, the batch invalidates in its ring (DESIGN-rb8-cost.md)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, defer_jobs, CTLFLAG_RW, &gDeferJobs, 0, "rb8cost: 1 = deferred frees wait for the VM's in-flight jobs (rb8c, default); 0 = for the invalidation alone (rb8b; DESIGN-rb8-cost.md)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, ggtt_inval_wait, CTLFLAG_RW, &gGgttInvalWait, 0, "rb8cost: 1 = GGTT invalidations through the GuC are waited for (rb8d, default); 0 = issued only (DESIGN-rb8-cost.md)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, tlb_every_batch, CTLFLAG_RW, &gTlbEveryBatch, 0, "rb7k: 1 = every batch on a scratch VM invalidates the TLB first (Linux xe batch_invalidate_tlb); 0 = only after page-table changes (default)");
static int gtResetHandler SYSCTL_HANDLER_ARGS {
	int v = 0, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v != 1 || !gDev || !gDev->up) return EINVAL;
	xeRequestGtReset();
	return 0;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, gt_reset, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, gtResetHandler, "I", "write 1: reset the GT (as a hang would)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, ctx_policies, CTLFLAG_RW, &gCtxPolicies, 0, "new queues: Linux GuC scheduling policies (1 ms timeslice, 640 ms preemption timeout); 0 = GuC defaults");
extern int gLrcVram;
SYSCTL_INT(_debug_intelxe, OID_AUTO, usm, CTLFLAG_RD, &gUsm, 0, "rb7j: GuC page fault (UM) queues on the primary GT (boot-arg -intelxe_nousm: 0)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, lrc_vram, CTLFLAG_RW, &gLrcVram, 0, "new exec queues: LRC and ring in VRAM (xe_lrc_init XE_BO_FLAG_VRAM_IF_DGFX); 0 = system memory as before rb7");
SYSCTL_INT(_debug_intelxe, OID_AUTO, rc_yield, CTLFLAG_RW, &gRcYield, 0, "GuC render/compute yield policy (Linux: 80 ms, 20% to CCS) at the next GuC load or GT reset; 0 = not sent");
SYSCTL_INT(_debug_intelxe, OID_AUTO, job_timeout_ms, CTLFLAG_RW, &gJobTimeoutMs, 0, "job watchdog: ban a queue whose oldest job has not completed for this long; 0 = off");
SYSCTL_INT(_debug_intelxe, OID_AUTO, video, CTLFLAG_RW, &gVideoWanted, 0, "list the media GT's video decode engines (gt 1)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, media_slpc, CTLFLAG_RW, &gMediaSlpc, 0, "start SLPC on the media GuC (next media load)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, vcs_single, CTLFLAG_RW, &gVcsSingle, 0, "rb7o: 1 = new video queues run on their first placement only (before rb7o); 0 = every placement in the GuC's logical mask, as xe (boot-arg -intelxe_vcssingle: 1)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, nonpriv, CTLFLAG_RD, &gNonpriv, 0, "rb7p: 1 = engines get Linux's RING_FORCE_TO_NONPRIV whitelist (FF_MODE/VFLSKPD on RCS, OA slots denied); boot-arg -intelxe_nowhitelist: 0 (as rb7o)");
// rb7o: debug.intelxe.vcs (anybody, reads nothing from the GPU) and debug.intelxe.vcs_busy (root: wakes the media GT and samples
// the video engines for 250 ms)
static int vcsHandler SYSCTL_HANDLER_ARGS {
	const bool sample = arg2 != 0;
	if (sample && req->p && proc_suser(req->p)) return EPERM;
	char *buf = static_cast<char *>(IOMalloc(4096));
	if (!buf) return ENOMEM;
	const int n = !gDev ? snprintf(buf, 4096, "no device\n") : sample ? xeVcsBusyString(buf, 4096) : xeVcsString(buf, 4096);
	int err = SYSCTL_OUT(req, buf, (size_t)(n > 0 ? (n < 4095 ? n : 4095) : 0) + 1);
	IOFree(buf, 4096);
	return err;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, vcs, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, vcsHandler, "A",
	"rb7o: video queues (logical masks, the engine each last job started on) and user interrupts per video engine");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, vcs_busy, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 1, vcsHandler, "A",
	"rb7o: video engine busy sampling over 250 ms (root; wakes the media GT while it samples)");
extern char gG2hLog[4096];
#if INTELXE_DEBUG
// Batch dumps hold other processes' command dwords: debug builds only, and readable by root only (no CTLFLAG_ANYBODY).
extern char gExecLog[16384];
extern int gDumpExec;
static int execLogHandler SYSCTL_HANDLER_ARGS {
	if (req->p && proc_suser(req->p)) return EPERM;
	return SYSCTL_OUT(req, gExecLog, strnlen(gExecLog, sizeof(gExecLog)) + 1);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, execlog, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, execLogHandler, "A", "batch dumps (root)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, dumpexec, CTLFLAG_RW, &gDumpExec, 0, "record batches");
#endif
static int g2hHandler SYSCTL_HANDLER_ARGS {
	if (req->p && proc_suser(req->p)) return EPERM;
	IOLockLock(gSayLock);
	int err = SYSCTL_OUT(req, gG2hLog, strnlen(gG2hLog, sizeof(gG2hLog)) + 1);
	IOLockUnlock(gSayLock);
	return err;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, g2h, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, g2hHandler, "A", "unusual G2H messages (root)");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, statsHandler, "A", "interrupt, scheduler and TLB counters");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, dispcheck, CTLTYPE_STRING | CTLFLAG_RD, nullptr, 0, dispCheckHandler, "A",
	"display buffers read back by CPU, copy engine (raw, decompressed) and CPU after an L2 flush (root)");
extern int gSurfVram, gSurfVramMb, gSurfVramWsFull;
int xeSurfVramStats(char *out, size_t len);
int xeSurfVramList(char *out, size_t len);
static int surfVramHandler SYSCTL_HANDLER_ARGS {
	char b[2048];   // rb7q: four lines
	const int n = xeSurfVramStats(b, sizeof(b));
	return SYSCTL_OUT(req, b, (size_t)(n > 0 ? (n < (int)sizeof(b) ? n : (int)sizeof(b) - 1) : 0) + 1);
}
SYSCTL_INT(_debug_intelxe, OID_AUTO, surfvram, CTLFLAG_RW, &gSurfVram, 0, "VRAM-shared IOSurfaces: 0 off, 1 apps, 2 apps + WindowServer (gpuperf)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, surfvram_mb, CTLFLAG_RW, &gSurfVramMb, 0, "VRAM all shared IOSurface backings together may take, MB");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, surfvram_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, surfVramHandler, "A", "VRAM-shared IOSurfaces");
// rb7q (xe_core.cpp gSVQ): the backings one by one and the boot's surfvram events; WindowServer full-screen creates
static int surfVramListHandler SYSCTL_HANDLER_ARGS {
	constexpr size_t kLen = 65536;
	char *b = static_cast<char *>(IOMalloc(kLen));
	if (!b) return ENOMEM;
	const int n = xeSurfVramList(b, kLen);
	const int err = SYSCTL_OUT(req, b, (size_t)(n > 0 ? (n < (int)kLen ? n : (int)kLen - 1) : 0) + 1);
	IOFree(b, kLen);
	return err;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, surfvram_list, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, surfVramListHandler, "A",
	"rb7q: live VRAM-shared IOSurface backings (creator, age, full-screen) and the surfvram event ring");
SYSCTL_INT(_debug_intelxe, OID_AUTO, surfvram_wsfull, CTLFLAG_RW, &gSurfVramWsFull, 0,
	"rb7q: 1 = WindowServer only imports (never makes) a VRAM backing of at least one scan-out frame; 0 = as rb7p");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, vram, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, vramHandler, "A", "VRAM and BAR window use by process");
// debug.intelxe.sensors (anyone may read; IntelXe Manager's monitor): the render GT's actual and requested clock and its
// limits, and the package and VRAM temperatures, as Linux xe reads them: xe_guc_pc_get_act_freq reads CAGF from
// MTL_MIRROR_TARGET_WP1 without forcewake (0 while the GT sits in RC6), the caps from MTL_RP_STATE_CAP; xe_hwmon reads
// BMG_PACKAGE_TEMPERATURE and BMG_VRAM_TEMPERATURE, degrees Celsius in bits 7:0 (xe_pcode_regs.h TEMP_MASK). Then, on a
// platform whose energy comes from PMT telemetry (BMG), the card and package energy as xe_hwmon's energy1/2_input
// (joules, summed across the counters' 32-bit wraps) and the average power of each since an earlier reading at least
// 0.5 s old (n/a until there is one, and for a window xePowerWindowSane refuses: "power windows rejected" counts
// those). Then, on a platform with has_mbx_power_limits (BMG), the card and package PL1/PL2 and the card's I1 (crit) as
// xe_hwmon reads them through the pcode mailbox (xe_pcode.h; read only, cached for a second). The text before the energy
// fields is unchanged (IntelXe Manager parses it: the first "act", "req", "RP0", "RPn", "package" and "VRAM";
// tools/regress the first "card power" and "package power"); new fields go at the end. No forcewake: reading it does
// not wake the GPU. The writes are the telemetry region index in SG_REMAP_INDEX1, which xe_pmt_telem_read sets before
// every telemetry read too, and the pcode mailbox's request registers (PCODE_DATA0/1, PCODE_MAILBOX) of a read command.
static int sensorsHandler SYSCTL_HANDLER_ARGS {
	char buf[768];
	const int n = gDev ? gDev->sensorsString(buf, sizeof buf) : snprintf(buf, sizeof buf, "no device\n");
	return SYSCTL_OUT(req, buf, (size_t)(n < (int)sizeof buf - 1 ? n : (int)sizeof buf - 1) + 1);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, sensors, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, sensorsHandler, "A",
	"act/req MHz, RP0/RPn MHz, package and VRAM temperature (C), card and package energy (J), average power (W) and "
	"power limits (W)");
int IntelXe::sensorsString(char *buf, size_t len)
{
	if (!bar0 || suspended) return snprintf(buf, len, "unavailable (%s)\n", suspended ? "asleep" : "not mapped");
	auto mhz = [](uint32_t raw) { return (raw * 50 + 1) / 3; };   // units of 50/3 MHz (decode_freq)
	const uint32_t wp1 = rd(0xc60), req = rd(0xa008), cap = rd(0x138000);
	const uint32_t pkg = rd(BMG_PACKAGE_TEMPERATURE), vram = rd(BMG_VRAM_TEMPERATURE);
	if (wp1 == 0xffffffffu && cap == 0xffffffffu) return snprintf(buf, len, "unavailable (the device does not answer)\n");
	size_t n = (size_t)snprintf(buf, len, "act %u MHz, req %u MHz, RP0 %u MHz, RPn %u MHz, package %u C, VRAM %u C",
		mhz(wp1 & 0x1ff), mhz(req >> 23 & 0x1ff), mhz(cap & 0x1ff), mhz(cap >> 16 & 0x1ff), pkg & 0xff, vram & 0xff);
	if (gXePlat && gXePlat->energySource == XE_ENERGY_PMT && gPmtLock) {
		// xe_hwmon.c:513-538 on XE_BATTLEMAGE: the 64-bit word at BMG_ENERGY_STATUS_PMT_OFFSET of the region the
		// PUNIT_TELEMETRY_GUID register names, package in bits 31:0 and card in 63:32, each summed across wraps and
		// scaled by 2^-scl_shift_energy J.
		uint64_t v = 0, ns = 0, uj[XE_ENERGY_CHANNELS] = {};
		uint32_t mw[XE_ENERGY_CHANNELS] = {}, rejected = 0;
		bool power = false, pkgPower = false;
		IOLockLock(gPmtLock);
		const uint32_t guid = rd(PUNIT_TELEMETRY_GUID);
		const int err = pmtTelemRead(guid, BMG_ENERGY_STATUS_PMT_OFFSET, &v);
		if (!err) {
			uint64_t t;
			clock_get_uptime(&t);
			absolutetime_to_nanoseconds(t, &ns);
			energy.shift = gXePlat->energyShift;
			energy.update(v, ns, 500000000ull);
			for (uint32_t c = 0; c < XE_ENERGY_CHANNELS; c++) uj[c] = energy.uj(c), mw[c] = energy.mw[c];
			power = energy.havePower;
			pkgPower = energy.pkgValid;
			rejected = energy.rejected;
		}
		IOLockUnlock(gPmtLock);
		if (err && n < len) {
			n += (size_t)snprintf(buf + n, len - n, ", energy unavailable (PMT error %d, GUID 0x%08x)", err, guid);
		} else if (n < len) {
			n += (size_t)snprintf(buf + n, len - n, ", card energy %llu.%03llu J, package energy %llu.%03llu J",
				uj[XE_ENERGY_CARD] / 1000000, uj[XE_ENERGY_CARD] % 1000000 / 1000, uj[XE_ENERGY_PKG] / 1000000,
				uj[XE_ENERGY_PKG] % 1000000 / 1000);
			if (n < len && power)
				n += (size_t)snprintf(buf + n, len - n, ", card power %u.%02u W", mw[XE_ENERGY_CARD] / 1000,
					mw[XE_ENERGY_CARD] % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", card power n/a");
			if (n < len && pkgPower)
				n += (size_t)snprintf(buf + n, len - n, ", package power %u.%02u W", mw[XE_ENERGY_PKG] / 1000,
					mw[XE_ENERGY_PKG] % 1000 / 10);
			else if (n < len)
				n += (size_t)snprintf(buf + n, len - n, ", package power n/a");
			if (n < len) n += (size_t)snprintf(buf + n, len - n, ", power windows rejected %u", rejected);
		}
	}
	if (gXePlat && gXePlat->hasMbxPowerLimits && gPcodeLock && n < len) {
		XePowerLimits pl;
		powerLimitsRead(&pl);
		n += xePowerLimitsText(buf + n, len - n, pl);
	}
	if (n < len) n += (size_t)snprintf(buf + n, len - n, "\n");
	return (int)n;
}
// xe_vsec.c:143-174 xe_pmt_telem_read for 8 bytes: the GUID names the telemetry region and the offset in the window
// (xe_guid_decode, xe_pmt.h), the SoC re-mapper shows that region (xe_soc_remapper_set_telem_region: rmw of
// SG_REMAP_INDEX1 bits 31:24), then the bytes are read from BAR0 + BMG_TELEMETRY_OFFSET + offset + userOffset. Linux
// answers -ENODATA unless the device is runtime-active (xe_pm_runtime_get_if_active); here: not while suspended. Linux
// copies with memcpy_fromio; here two aligned 32-bit reads, low dword first: the energy word is two independent 32-bit
// counters (ENERGY_PKG 31:0, ENERGY_CARD 63:32), so a read between their updates still gives two valid values.
// Caller holds gPmtLock. 0 or an errno.
int IntelXe::pmtTelemRead(uint32_t guid, uint32_t userOffset, uint64_t *data)
{
	uint32_t index = 0, offset = 0;
	const int dec = xePmtGuidDecode(guid, &index, &offset);
	if (dec) return dec == -19 ? ENODEV : EINVAL;
	if (!gXePlat || !gXePlat->hasSocRemapperTelem) return ENODEV;   // soc_remapper.set_telem_region unset: -ENODEV
	if (!bar0 || suspended) return ENODATA;
	wr(SG_REMAP_INDEX1, xePmtRemapValue(rd(SG_REMAP_INDEX1), index));
	const uint32_t off = xePmtTelemOffset(offset, userOffset);
	const uint32_t lo = rd(off), hi = rd(off + 4);
	*data = (uint64_t)hi << 32 | lo;
	return 0;
}
// xe_pcode.c:138-147 xe_pcode_read: __pcode_mailbox_rw (xePcodeMailboxRw, xe_pcode.h) with PCODE_DEFAULT_TIMEOUT_MS and
// the data read back. Busy-waits (IODelay) for at most the timeout, as xe_mmio_wait32 polls. Caller holds gPcodeLock.
int IntelXe::pcodeReadLocked(uint32_t mbox, uint32_t *val0, uint32_t *val1)
{
	struct Io {
		IntelXe *x;
		uint32_t rd(uint32_t off) { return x->rd(off); }
		void wr(uint32_t off, uint32_t v) { x->wr(off, v); }
		void delayUs(uint32_t us) { IODelay(us); }
	} io {this};
	return xePcodeMailboxRw(io, mbox, val0, val1, PCODE_DEFAULT_TIMEOUT_MS * 1000, true);
}
// xe_pcode_read with its lock. Linux's callers hold a runtime-PM reference (xe_hwmon: guard(xe_pm_runtime)); here: not
// while the device sleeps (ENODATA), checked under the lock that suspend takes to set `suspended`.
int IntelXe::pcodeRead(uint32_t mbox, uint32_t *val0, uint32_t *val1)
{
	if (!gPcodeLock) return ENODEV;
	IOLockLock(gPcodeLock);
	const int err = !bar0 || suspended ? ENODATA : pcodeReadLocked(mbox, val0, val1);
	IOLockUnlock(gPcodeLock);
	return err;
}
// The power limits as xe_hwmon_get_preregistration_info reads them on a has_mbx_power_limits platform (xe_hwmon.c:
// 1499-1508 through xe_hwmon_pcode_read_power_limit, :190-221): PCODE_POWER_SETUP with READ_PSYSGPU_POWER_LIMIT (card)
// and READ_PACKAGE_POWER_LIMIT (package), param2 READ_PL_FROM_FW, PL1 in DATA0 and PL2 in DATA1; then I1
// (xe_hwmon_pcode_read_i1, :856-867). Linux goes on to write those limits back (:1513-1525) and reads READ_PL_FROM_PCODE
// from then on; nothing is written here, so the request stays READ_PL_FROM_FW. The first reading also asks
// READ_PL_FROM_PCODE once and logs both raw, for comparison on hardware. Cached for a second (a failure too): sysctl
// calls the handler twice per read (size, then text), and the limits change only when written.
void IntelXe::powerLimitsRead(XePowerLimits *out)
{
	uint64_t t, ns;
	clock_get_uptime(&t);
	absolutetime_to_nanoseconds(t, &ns);
	IOLockLock(gPcodeLock);
	if (!bar0 || suspended) {
		out->err = ENODATA;
		IOLockUnlock(gPcodeLock);
		return;
	}
	if (plNs && ns - plNs < 1000000000ull) {   // a failed reading too: a pcode that times out costs 10 ms a try
		*out = plCache;
		IOLockUnlock(gPcodeLock);
		return;
	}
	XePowerLimits pl;
	pl.err = 0;
	static const uint32_t kCmd[XE_PL_CHANNELS] = {READ_PSYSGPU_POWER_LIMIT, READ_PACKAGE_POWER_LIMIT};
	for (uint32_t c = 0; c < XE_PL_CHANNELS && !pl.err; c++) {
		uint32_t v0 = 0, v1 = 0;
		pl.err = pcodeReadLocked(xePcodeMbox(PCODE_POWER_SETUP, kCmd[c], READ_PL_FROM_FW), &v0, &v1);
		pl.word[c][0] = v0;
		pl.word[c][1] = v1;
	}
	if (!pl.err) {
		uint32_t i1 = 0;
		pl.i1Err = pcodeReadLocked(xePcodeMbox(PCODE_POWER_SETUP, POWER_SETUP_SUBCOMMAND_READ_I1, 0), &i1, nullptr);
		pl.i1 = i1;
	}
	if (!plLogged) {
		plLogged = true;
		uint32_t p[XE_PL_CHANNELS][2] = {};
		int perr = 0;
		for (uint32_t c = 0; c < XE_PL_CHANNELS && !perr; c++)
			perr = pcodeReadLocked(xePcodeMbox(PCODE_POWER_SETUP, kCmd[c], READ_PL_FROM_PCODE), &p[c][0], &p[c][1]);
		LOG("power limits (pcode mailbox, read only): FROM_FW %d card 0x%08x 0x%08x package 0x%08x 0x%08x; FROM_PCODE %d "
			"card 0x%08x 0x%08x package 0x%08x 0x%08x; I1 %d 0x%08x; PL1 tau card %llu ms package %llu ms", pl.err,
			pl.word[0][0], pl.word[0][1], pl.word[1][0], pl.word[1][1], perr, p[0][0], p[0][1], p[1][0], p[1][1], pl.i1Err,
			pl.i1, xePowerLimitTauMs(pl.word[0][0], XE_TIME_UNIT_MBX), xePowerLimitTauMs(pl.word[1][0], XE_TIME_UNIT_MBX));
	}
	plCache = pl;
	plNs = ns;
	*out = pl;
	IOLockUnlock(gPcodeLock);
}
// rb7r: purgeable BOs (DRM_IOCTL_XE_MADVISE). purge: write 1 = purge every DONTNEED BO that can be now (memory pressure
// on demand); purge_stats: madvise and purge counters.
static int purgeHandler SYSCTL_HANDLER_ARGS {
	int v = 0, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v != 1 || !gDev || !gDev->up) return EINVAL;
	return xePurgeNow();
}
static int purgeStatsHandler SYSCTL_HANDLER_ARGS {
	char buf[1024];
	const int n = xePurgeStats(buf, sizeof(buf));
	return SYSCTL_OUT(req, buf, (size_t)(n > 0 ? (n < (int)sizeof(buf) - 1 ? n : (int)sizeof(buf) - 1) : 0) + 1);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, purge, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, purgeHandler, "I",
	"rb7r: write 1: purge every DONTNEED BO that can be purged now (memory pressure on demand)");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, purge_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, purgeStatsHandler, "A",
	"rb7r: DRM_IOCTL_XE_MADVISE purgeable state and purge counters");
// rb7s: shared-BO tests. evict: write a pid = move that process's never-CPU-mapped, idle BAR-window BOs out of the window
// now (0: every process's; a BO move on demand, other clients' bindings rewritten too). flink: 1 = DRM_IOCTL_GEM_FLINK
// and DRM_IOCTL_GEM_OPEN answer as on Linux's primary node for an authenticated client; 0 (default) = EACCES, as on a
// render node. Both root-only to write.
extern int gFlink;
static int evictHandler SYSCTL_HANDLER_ARGS {
	int v = -1, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v < 0 || !gDev || !gDev->up) return EINVAL;
	xeEvictNow(v);
	return 0;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, evict, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, evictHandler, "I",
	"rb7s: write a pid (0 = all): move its never-mapped, idle BAR-window BOs out of the window now");
SYSCTL_INT(_debug_intelxe, OID_AUTO, flink, CTLFLAG_RW, &gFlink, 0,
	"rb7s: 1 = GEM_FLINK / GEM_OPEN allowed (primary-node behaviour, for tests); 0 = EACCES (render node)");
// rb7t: object fds (fd sharing stages 1-2: syncobj fds, sync files, PRIME BO fds through the clone device /dev/intelxeobj;
// xe_fd.cpp). fds: 0 (default) = off: no device node, INTELXE_CAP_FD_OBJECTS and DRM_CAP_PRIME read 0 and the four ioctls
// answer ENOTTY, as before rb7t; 1 = on (the node is made at the first 1 and stays; switching back to 0 refuses new
// opens and the ioctls, and leaves the objects already bound to their fds until those close). Boot-arg intelxe_fds=1 sets it
// at start. Writing is root-only. fd_stats: the records and objects behind them (leak checks).
static int fdsHandler SYSCTL_HANDLER_ARGS {
	int v = gFds, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (v != 0 && v != 1) return EINVAL;
	if (v && (err = xeFdEnable())) return err;
	gFds = v;
	return 0;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, fds, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, fdsHandler, "I",
	"rb7t: 1 = object fds (syncobj fds, sync files, PRIME) through /dev/intelxeobj; 0 = off, as before (boot-arg intelxe_fds=1)");
static int fdStatsHandler SYSCTL_HANDLER_ARGS {
	char b[1536];
	const int n = xeFdStats(b, sizeof(b));
	return SYSCTL_OUT(req, b, (size_t)(n > 0 ? (n < (int)sizeof(b) ? n : (int)sizeof(b) - 1) : 0) + 1);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, fd_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, fdStatsHandler, "A",
	"rb7t: object fds: minors open by kind, opens / closes, binds / imports, objects alive");
SYSCTL_UINT(_debug_intelxe, OID_AUTO, display_irqs, CTLFLAG_RD | CTLFLAG_ANYBODY, &gStats.displayIrqs, 0, "display interrupts passed to IntelXeFB");
SYSCTL_INT(_debug_intelxe, OID_AUTO, freq, CTLFLAG_RW, &gFreqMhz, 0, "MHz for step 11");
SYSCTL_INT(_debug_intelxe, OID_AUTO, freq_manual, CTLFLAG_RW, &gFreqKeepManual, 0, "step 11 keeps manual RP control");
SYSCTL_PROC(_debug_intelxe, OID_AUTO, hwconfig, CTLTYPE_OPAQUE | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, hwconfigHandler, "S", "GuC hwconfig table");
// GT power management (xe_core.cpp). Values are used at bring-up and by steps 17 (SLPC) and 19 (GuC RC / RC6);
// Defaults are Linux's: min 1200, max RP0, strategy compute, profile base, GuC RC firmware control; RC_CONTROL untouched.
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_min, CTLFLAG_RW, &gSlpcMin, 0, "SLPC min GT MHz (step 17)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_max, CTLFLAG_RW, &gSlpcMax, 0, "SLPC max GT MHz, 0 = RP0 (step 17)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_ignore_eff, CTLFLAG_RW, &gSlpcIgnoreEff, 0, "SLPC ignore efficient freq: -1 Linux rule, 0, 1");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_strategy, CTLFLAG_RW, &gSlpcStrategy, 0, "SLPC strategies bits (1 = compute)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_profile, CTLFLAG_RW, &gSlpcProfile, 0, "SLPC power profile: 0 base, 1 power saving, -1 not sent");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_param_id, CTLFLAG_RW, &gSlpcParamId, 0, "raw SLPC parameter id for step 17 (-1 none)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, slpc_param_val, CTLFLAG_RW, &gSlpcParamVal, 0, "its value (-1 = unset)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, gucrc, CTLFLAG_RW, &gGucRc, 0, "GuC RC: -1 not sent, 0 host, 1 firmware (default, Linux)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, rc6, CTLFLAG_RW, &gRc6Ctl, 0, "RC_CONTROL: -1 untouched, 0 RC6 off, 1 host RC6 (step 19)");
SYSCTL_INT(_debug_intelxe, OID_AUTO, mmio_addr, CTLFLAG_RW, &gMmioAddr, 0, "BAR0 offset step 20 reads");
static int fwHoldHandler SYSCTL_HANDLER_ARGS {
	int v = gFwHold, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	if (!gDev) return ENODEV;
	IOLockLock(gDevLock);
	if (gDev->fwHold(v != 0)) gFwHold = v != 0;
	else err = ETIMEDOUT;
	IOLockUnlock(gDevLock);
	return err;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, fw_hold, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, fwHoldHandler, "I", "1 holds GT and render forcewake (no RC6)");
// debug.intelxe.jobtime / jobtime_stats: where a job's round trip goes, in GPU timestamps (xe_core.cpp, job timing).
// Write 1 to start (or restart) the statistics, 0 to stop; run with fw_hold=1.
static int jobTimeHandler SYSCTL_HANDLER_ARGS {
	int v = gJobTime, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	return xeJobTimeSet(v);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, jobtime, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, jobTimeHandler, "I", "1 = time each render-GT job in GPU timestamps (restarts the statistics)");
static int jobTimeStatsHandler SYSCTL_HANDLER_ARGS {
	char buf[640];
	const int n = gDev ? xeJobTimeString(buf, sizeof buf, gFwHold) : snprintf(buf, sizeof buf, "no device");
	return SYSCTL_OUT(req, buf, (size_t)(n > 0 && n < (int)sizeof buf - 1 ? n : (int)sizeof buf - 1) + 1);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, jobtime_stats, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, jobTimeStatsHandler, "A", "job timing averages");
// debug.intelxe.direct / direct_idle_us: direct submission (xe_core.cpp): a low-latency compute queue keeps its context on
// the engine, polling a mailbox, while its jobs come less than direct_idle_us apart. On from the bring-up since 12 Nov
// (-intelxe_nodirect: off); the counters are in debug.intelxe.stats.
static int directHandler SYSCTL_HANDLER_ARGS {
	int v = gDirect, err = sysctl_handle_int(oidp, &v, 0, req);
	if (err || !req->newptr) return err;
	return xeDirectSet(v);
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, direct, CTLTYPE_INT | CTLFLAG_RW, nullptr, 0, directHandler, "I", "1 = direct submission for low-latency compute queues (armed ring slot, mailbox; default, -intelxe_nodirect: 0); 0 = off");
SYSCTL_INT(_debug_intelxe, OID_AUTO, direct_idle_us, CTLFLAG_RW, &gDirectIdleUs, 0, "direct submission: a queue leaves after this long without a job (us, 20-1000000)");
// debug.intelxe.dump: every value a report needs, read-only, in one call (tools/report/collect-macos.sh reads it first).
// Format: "intelxe-dump 1", then "[section]" blocks: the integer tunables as name = value, the bring-up report
// (debug.intelxe.state), interrupt/scheduler/TLB counters (stats), VRAM and BAR window use (vram) and the VRAM-shared
// IOSurface counters (surfvram_stats). Readable by anyone like those sysctls. It never contains debug.intelxe.g2h,
// execlog or hwconfig, and reading it changes nothing: it only formats what the other handlers already return.
static void dumpf(char *b, size_t len, size_t *n, const char *fmt, ...) __printflike(4, 5);
static void dumpf(char *b, size_t len, size_t *n, const char *fmt, ...) {
	if (*n >= len - 1) return;
	va_list ap;
	va_start(ap, fmt);
	const int k = vsnprintf(b + *n, len - *n, fmt, ap);
	va_end(ap);
	if (k > 0) *n = *n + (size_t)k < len - 1 ? *n + (size_t)k : len - 1;
}
static void dumpText(char *b, size_t len, size_t *n, const char *section, const char *text, size_t tlen) {
	dumpf(b, len, n, "[%s]\n", section);
	if (*n + tlen >= len - 1) tlen = len - 1 - *n;
	memcpy(b + *n, text, tlen);
	*n += tlen;
	if (*n && b[*n - 1] != '\n') dumpf(b, len, n, "\n");
}
static int dumpHandler SYSCTL_HANDLER_ARGS {
	// SYSCTL_INT/UINT oids: oid_arg1 points at the value (the same thing sysctl_handle_int reads)
	static struct sysctl_oid *const kInts[] = {
		&sysctl__debug_intelxe_irq, &sysctl__debug_intelxe_unbind_async, &sysctl__debug_intelxe_bind_async, &sysctl__debug_intelxe_hugepages,
		&sysctl__debug_intelxe_range_inval, &sysctl__debug_intelxe_ctx_policies, &sysctl__debug_intelxe_rc_yield, &sysctl__debug_intelxe_lrc_vram, &sysctl__debug_intelxe_usm,
		&sysctl__debug_intelxe_job_timeout_ms, &sysctl__debug_intelxe_video, &sysctl__debug_intelxe_media_slpc, &sysctl__debug_intelxe_vcs_single,
		&sysctl__debug_intelxe_nonpriv, &sysctl__debug_intelxe_direct_idle_us,
		&sysctl__debug_intelxe_surfvram, &sysctl__debug_intelxe_surfvram_mb, &sysctl__debug_intelxe_surfvram_wsfull, &sysctl__debug_intelxe_display_irqs,
		&sysctl__debug_intelxe_freq, &sysctl__debug_intelxe_freq_manual, &sysctl__debug_intelxe_slpc_min,
		&sysctl__debug_intelxe_slpc_max, &sysctl__debug_intelxe_slpc_ignore_eff, &sysctl__debug_intelxe_slpc_strategy,
		&sysctl__debug_intelxe_slpc_profile, &sysctl__debug_intelxe_gucrc, &sysctl__debug_intelxe_rc6,
	};
	constexpr size_t kLen = 32768;
	char *b = static_cast<char *>(IOMalloc(kLen));
	char *t = static_cast<char *>(IOMalloc(16384));
	if (!b || !t) {
		if (b) IOFree(b, kLen);
		if (t) IOFree(t, 16384);
		return ENOMEM;
	}
	size_t n = 0;
	dumpf(b, kLen, &n, "intelxe-dump 1\n[tunables]\n");
	for (struct sysctl_oid *o : kInts) {
		const bool u = (o->oid_kind & CTLTYPE) == CTLTYPE_INT && o->oid_fmt && o->oid_fmt[0] == 'I' && o->oid_fmt[1] == 'U';
		const int v = o->oid_arg1 ? *static_cast<const int *>(o->oid_arg1) : o->oid_arg2;
		if (u) dumpf(b, kLen, &n, "%s = %u\n", o->oid_name, (unsigned)v);
		else dumpf(b, kLen, &n, "%s = %d\n", o->oid_name, v);
	}
	dumpf(b, kLen, &n, "fw_hold = %d\ndirect = %d\nstep = %d\ndevice = %s\n", gFwHold, gDirect, gStep, gDev ? "up" : "none");
	IOLockLock(gSayLock);
	dumpText(b, kLen, &n, "state", gState, strnlen(gState, sizeof(gState)));
	IOLockUnlock(gSayLock);
	int k = gDev ? xeStatsString(t, 4096) : snprintf(t, 4096, "no device\n");
	dumpText(b, kLen, &n, "stats", t, k > 0 ? (size_t)(k < 4095 ? k : 4095) : 0);
	k = gDev ? xeVramString(t, 16384) : snprintf(t, 16384, "no device\n");
	dumpText(b, kLen, &n, "vram", t, k > 0 ? (size_t)(k < 16383 ? k : 16383) : 0);
	k = xeSurfVramStats(t, 2048);
	dumpText(b, kLen, &n, "surfvram", t, k > 0 ? (size_t)(k < 2047 ? k : 2047) : 0);
	k = xePurgeStats(t, 1024);   // rb7r
	dumpText(b, kLen, &n, "purge", t, k > 0 ? (size_t)(k < 1023 ? k : 1023) : 0);
	k = xeSurfVramList(t, 16384);   // rb7q (the dump is 32 KB: the tail of a long ring may be cut)
	dumpText(b, kLen, &n, "surfvram_list", t, k > 0 ? (size_t)(k < 16383 ? k : 16383) : 0);
	const int err = SYSCTL_OUT(req, b, n + 1);
	IOFree(t, 16384);
	IOFree(b, kLen);
	return err;
}
SYSCTL_PROC(_debug_intelxe, OID_AUTO, dump, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_ANYBODY, nullptr, 0, dumpHandler, "A",
	"all read-only state in one call (tools/report)");
#define INTELXE_PM_OIDS(X) X(slpc_min) X(slpc_max) X(slpc_ignore_eff) X(slpc_strategy) X(slpc_profile) X(slpc_param_id) \
	X(slpc_param_val) X(gucrc) X(rc6) X(mmio_addr) X(fw_hold)
#define INTELXE_REG_OID(n) sysctl_register_oid(&sysctl__debug_intelxe_##n);
#define INTELXE_UNREG_OID(n) sysctl_unregister_oid(&sysctl__debug_intelxe_##n);

// xe_pci_probe: the device's descriptor (xe_platform.cpp), looked up once. The id comes from config space; a card that
// does not answer yet (rb7u: 0xffff in the first seconds of a macOS 26 boot) is looked up by the PCI family's
// "device-id", read at enumeration as Linux's pdev->device is. nullptr: no entry for either.
static const XePlatformDesc *platformOf(IOPCIDevice *pci, uint16_t *id) {
	*id = pci->configRead16(kIOPCIConfigDeviceID);
	const XePlatformDesc *d = xePlatformFind(*id);
	if (!d) {
		OSData *p = OSDynamicCast(OSData, pci->getProperty("device-id"));
		if (p && p->getLength() >= 2) {
			*id = *static_cast<const uint16_t *>(p->getBytesNoCopy());
			d = xePlatformFind(*id);
		}
	}
	return d;
}

// xe_uc_fw.c picks the file per platform; here the GuC and HuC images are built in (build.py): the descriptor must name
// those files ("xe/bmg_guc_70.bin" ends in kGucFwName).
static bool fwIs(const char *path, const char *builtIn) {
	if (!path) return false;
	const size_t n = strlen(path), m = strlen(builtIn);
	return n > m && path[n - m - 1] == '/' && !strcmp(path + n - m, builtIn);
}

bool IntelXe::start(IOService *provider) {
	char dummy[8];
	if (intelxeBootArg("IntelXe", "-intelxe_off", dummy, sizeof(dummy))) return false;
	if (intelxeBootArg("IntelXe", "-intelxe_nohugepages", dummy, sizeof(dummy))) gHugePages = 0;
	if (intelxeBootArg("IntelXe", "-intelxe_novideo", dummy, sizeof(dummy))) gVideoWanted = 0;
	if (intelxeBootArg("IntelXe", "-intelxe_nodirect", dummy, sizeof(dummy))) gDirectWanted = 0;
	if (intelxeBootArg("IntelXe", "-intelxe_nousm", dummy, sizeof(dummy))) gUsm = 0;   // rb7j: no GuC page fault queues
	if (intelxeBootArg("IntelXe", "-intelxe_lrcsys", dummy, sizeof(dummy))) gLrcVram = 0;   // rb7: LRCs in system memory as before
	if (intelxeBootArg("IntelXe", "-intelxe_vcssingle", dummy, sizeof(dummy))) gVcsSingle = 1;   // rb7o: video queues on one engine
	if (intelxeBootArg("IntelXe", "-intelxe_nowhitelist", dummy, sizeof(dummy))) gNonpriv = 0;   // rb7p: no NONPRIV slots (as rb7o)
	if (!IOService::start(provider) || gDev) return false;
	// debug.intelxe.surfvram at boot: the personality's IntelXeSurfVramDefault (Info.plist: build.py --surfvram N), then the
	// boot-arg intelxe_surfvram=N, which wins. Neither: 0 (off), as before. 0-3 as the sysctl takes them.
	if (OSNumber *d = OSDynamicCast(OSNumber, getProperty("IntelXeSurfVramDefault"))) gSurfVram = (int)d->unsigned32BitValue();
	int sv = 0;
	if (intelxeBootArg("IntelXe", "intelxe_surfvram", &sv, sizeof(sv))) gSurfVram = sv;
	if (gSurfVram < 0 || gSurfVram > 3) gSurfVram = 0;
	// IntelXe Manager's power settings, kept in NVRAM, read before SLPC starts (xe_core.cpp sends them at its init):
	// intelxe-gpu-profile 0 base / 1 power saving (SLPC_PARAM_POWER_PROFILE), intelxe-gpu-min-mhz and intelxe-gpu-max-mhz
	// (SLPC global min / max; 0 = the default). "ok" or anything not a number counts as absent.
	{
		auto nvInt = [](const char *key, int lo, int hi, int *out) {
			IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane);
			if (!opt) return;
			if (OSObject *o = opt->copyProperty(key)) {
				char b[16] = {};
				if (OSData *d = OSDynamicCast(OSData, o)) bcopy(d->getBytesNoCopy(), b, d->getLength() < sizeof b - 1 ? d->getLength() : sizeof b - 1);
				else if (OSString *str = OSDynamicCast(OSString, o)) strlcpy(b, str->getCStringNoCopy(), sizeof b);
				o->release();
				if (b[0] >= '0' && b[0] <= '9') {
					long v = 0; for (const char *p = b; *p >= '0' && *p <= '9' && v < 100000; p++) v = v * 10 + (*p - '0');
					if (v >= lo && v <= hi) *out = (int)v;
				}
			}
			opt->release();
		};
		nvInt("intelxe-gpu-profile", 0, 1, &gSlpcProfile);
		nvInt("intelxe-gpu-min-mhz", 0, 3000, &gSlpcMin);
		nvInt("intelxe-gpu-max-mhz", 0, 3000, &gSlpcMax);
		if (gSlpcMin == 0) gSlpcMin = 1200;   // 0 in NVRAM: the default floor (Wa_14022085890)
	}
	int wf = 0;   // rb7q: debug.intelxe.surfvram_wsfull at boot (default 1)
	if (intelxeBootArg("IntelXe", "intelxe_svwsfull", &wf, sizeof(wf))) gSurfVramWsFull = wf;
	int fds = 0;   // rb7t: debug.intelxe.fds at boot (default 0: no object fds, no /dev/intelxeobj). Switched on with the first
	               // client (xeClientCreate): devfs may not take nodes yet this early in the boot (devfs_ready)
	if (intelxeBootArg("IntelXe", "intelxe_fds", &fds, sizeof(fds)) && fds == 1) gFdsBoot = 1;
	pci = OSDynamicCast(IOPCIDevice, provider);
	if (!pci) return false;
	// Only a supported platform starts (IOPCIMatch lists the ids too); the others are refused with what they lack.
	uint16_t id = 0;
	const XePlatformDesc *plat = platformOf(pci, &id);
	const char *sub = plat && plat->subplatform ? plat->subplatform : "";
	if (!plat) {
		LOG("8086:%04x: no platform descriptor, not starting", id);
		return false;
	}
	if (!plat->supported) {
		LOG("8086:%04x is %s%s%s: not supported yet (%s)", id, plat->name, *sub ? "-" : "", sub, plat->missing);
		return false;
	}
	if (!fwIs(plat->gucFw, kGucFwName) || !fwIs(plat->hucFw, kHucFwName)) {
		LOG("8086:%04x (%s): built-in firmware %s, %s; the platform needs %s, %s", id, plat->name, kGucFwName, kHucFwName,
			plat->gucFw ? plat->gucFw : "none", plat->hucFw ? plat->hucFw : "none");
		return false;
	}
	gXePlat = plat;
	gXePlatId = id;
	{
		// The same line as a registry property ("IntelXePlatform"): the log of a boot does not keep a message this early
		// (the kext starts from the boot kernel collection before logd runs, and the kernel message buffer wraps), so the
		// regression check, the report and the app read it from here.
		char line[128];
		snprintf(line, sizeof(line), "8086:%04x: %s%s%s, graphics %u.%02u, media %u.%02u, %s", id, plat->name, *sub ? "-" : "",
			sub, plat->graphicsVerx100 / 100, plat->graphicsVerx100 % 100, plat->mediaVerx100 / 100,
			plat->mediaVerx100 % 100, plat->isDgfx ? "discrete" : "integrated");
		LOG("%s", line);
		setProperty("IntelXePlatform", line);
	}
	// xe->info.has_usm (the graphics IP's) and a standalone media GT (max_gt_per_tile 2): off where the platform has none.
	// The UM queues sit in VRAM here, so has_usm needs a discrete part too.
	if (!plat->hasUsm || !plat->isDgfx) gUsm = 0;
	if (plat->maxGtPerTile < 2) gVideoWanted = 0;
	pci->retain();
	gDev = this;
	sysctl_register_oid(&sysctl__debug_intelxe);
	sysctl_register_oid(&sysctl__debug_intelxe_step);
	sysctl_register_oid(&sysctl__debug_intelxe_state);
	sysctl_register_oid(&sysctl__debug_intelxe_hwconfig);
	sysctl_register_oid(&sysctl__debug_intelxe_freq);
	sysctl_register_oid(&sysctl__debug_intelxe_freq_manual);
	sysctl_register_oid(&sysctl__debug_intelxe_g2h);
	sysctl_register_oid(&sysctl__debug_intelxe_irq);
	sysctl_register_oid(&sysctl__debug_intelxe_video);
	sysctl_register_oid(&sysctl__debug_intelxe_media_slpc);
	sysctl_register_oid(&sysctl__debug_intelxe_vcs_single);
	sysctl_register_oid(&sysctl__debug_intelxe_nonpriv);
	sysctl_register_oid(&sysctl__debug_intelxe_vcs);
	sysctl_register_oid(&sysctl__debug_intelxe_vcs_busy);
#if INTELXE_DEBUG
	sysctl_register_oid(&sysctl__debug_intelxe_execlog);
	sysctl_register_oid(&sysctl__debug_intelxe_dumpexec);
#endif
	sysctl_register_oid(&sysctl__debug_intelxe_unbind_async);
	sysctl_register_oid(&sysctl__debug_intelxe_range_inval);
	sysctl_register_oid(&sysctl__debug_intelxe_tlb_every_batch);
	sysctl_register_oid(&sysctl__debug_intelxe_bind_async);
	sysctl_register_oid(&sysctl__debug_intelxe_hugepages);
	sysctl_register_oid(&sysctl__debug_intelxe_ctx_policies);
	sysctl_register_oid(&sysctl__debug_intelxe_rc_yield);
	sysctl_register_oid(&sysctl__debug_intelxe_lrc_vram);
	sysctl_register_oid(&sysctl__debug_intelxe_usm);
	sysctl_register_oid(&sysctl__debug_intelxe_dispcheck);
	sysctl_register_oid(&sysctl__debug_intelxe_gt_reset);
	sysctl_register_oid(&sysctl__debug_intelxe_job_timeout_ms);
	sysctl_register_oid(&sysctl__debug_intelxe_stats);
	sysctl_register_oid(&sysctl__debug_intelxe_vram);
	sysctl_register_oid(&sysctl__debug_intelxe_sensors);
	sysctl_register_oid(&sysctl__debug_intelxe_jobtime);
	sysctl_register_oid(&sysctl__debug_intelxe_jobtime_stats);
	sysctl_register_oid(&sysctl__debug_intelxe_direct);
	sysctl_register_oid(&sysctl__debug_intelxe_direct_idle_us);
	sysctl_register_oid(&sysctl__debug_intelxe_surfvram);
	sysctl_register_oid(&sysctl__debug_intelxe_surfvram_mb);
	sysctl_register_oid(&sysctl__debug_intelxe_surfvram_stats);
	sysctl_register_oid(&sysctl__debug_intelxe_surfvram_list);
	sysctl_register_oid(&sysctl__debug_intelxe_surfvram_wsfull);
	sysctl_register_oid(&sysctl__debug_intelxe_purge);
	sysctl_register_oid(&sysctl__debug_intelxe_purge_stats);
	sysctl_register_oid(&sysctl__debug_intelxe_evict);
	sysctl_register_oid(&sysctl__debug_intelxe_flink);
	sysctl_register_oid(&sysctl__debug_intelxe_fds);
	sysctl_register_oid(&sysctl__debug_intelxe_fd_stats);
	sysctl_register_oid(&sysctl__debug_intelxe_display_irqs);
	sysctl_register_oid(&sysctl__debug_intelxe_dump);
	xeAspmRegister(true);   // debug.intelxe.aspm, aspm_l1 (xe_aspm.cpp)
	INTELXE_PM_OIDS(INTELXE_REG_OID)
	publishVram();   // VRAM size for System Information (again after the bring-up)
	// The GPU's name in About This Mac / System Information comes from the PCI device's "model" (data, NUL included,
	// as OpenCore DeviceProperties set it); without one the display section was titled "#display".
	// The B570 (8086:e20c) is the same BMG-G21 with fewer Xe cores (#2).
	static const char kB580[] = "Intel Arc B580 Graphics", kB570[] = "Intel Arc B570 Graphics";
	const bool b570 = pci->configRead16(kIOPCIConfigDeviceID) == 0xe20c;
	if (OSData *d = OSData::withBytes(b570 ? kB570 : kB580, b570 ? sizeof(kB570) : sizeof(kB580))) { pci->setProperty("model", d); d->release(); }
	registerService();
	// System sleep: two power states under the PCI device (xe_pm_suspend / xe_pm_resume in setPowerState), and the ReBAR
	// state to restore on wake (pci_save_rebar_state).
	{
		IOByteCount off = 0;
		if (pci->extendedFindPCICapability((UInt32)-0x15, &off)) {
			rebarOff = (uint32_t)off;
			rebarN = (pci->extendedConfigRead32(off + 8) >> 5) & 7;
			for (uint32_t i = 0; i < rebarN && i < 6; i++) rebarCtl[i] = pci->extendedConfigRead32(off + 8 + 8 * i);
		}
		bar2Lo = pci->configRead32(0x18);
		bar2Hi = pci->configRead32(0x1c);
		static IOPMPowerState states[2] = {
			{kIOPMPowerStateVersion1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0},
			{kIOPMPowerStateVersion1, kIOPMPowerOn | kIOPMDeviceUsable, kIOPMPowerOn, kIOPMPowerOn, 0, 0, 0, 0, 0, 0, 0, 0}};
		PMinit();
		provider->joinPMtree(this);
		registerPowerDriver(this, states, 2);
		changePowerStateTo(1);   // on; the power domain takes it to 0 only for system sleep
	}
	// Shutdown and restart: quiesce the GPU first (xe_device_shutdown -> xe_gt_shutdown).
	powerNotifier = registerPrioritySleepWakeInterest(&IntelXe::powerMessage, this, nullptr);
	// xe_device_probe brings the GT up at probe; here on a thread call so start() does not wait for the GuC (the first
	// ioctl waits for it on gDevLock instead). -intelxe_lazy: on the first ioctl, as before.
	// DMA: the mapper the platform assigned to this device (AppleVTD domain), as AirportItlwm's ItlDma takes it.
	gDmaMapper = IOMapper::copyMapperForDevice(pci);
	pcieExtTags(true);   // before the bring-up's first DMA (GuC load)
	pwrLock = IORecursiveLockAlloc();
	if (!intelxeBootArg("IntelXe", "-intelxe_lazy", dummy, sizeof(dummy))) bringUpAsync();
	LOG("attached to %s", pci->getName());
	// NMXAccel (nullmoth's IOGraphicsAccelerator2 for AMD/Intel, src/nullmoth) starts only on a device its GPU driver
	// has claimed: mark the PCI device and re-run matching on it. The Metal plugin behind NMXAccel reaches the GPU
	// through Mesa ANV and our user client; the GPU is brought up on its first ioctl. -intelxe_nometal leaves it unclaimed.
	if (!intelxeBootArg("IntelXe", "-intelxe_nometal", dummy, sizeof(dummy))) {
		pci->setProperty("nmxgpu-claimed", kOSBooleanTrue);
		pci->registerService();
	}
	return true;
}

// rb7t: kextunload terminates the service first (IOCatalogue::terminateDriversForModule): while an object fd is open its
// object (a BO in VRAM, a syncobj) and its d_close are this kext's, so the unload is refused until every one has closed.
bool IntelXe::terminate(IOOptionBits options) {
	if (const uint32_t n = xeFdOpenCount()) {
		LOG("terminate refused: %u object fd(s) open on /dev/intelxeobj", n);
		return false;
	}
	return IOService::terminate(options);
}

void IntelXe::stop(IOService *provider) {
	PMstop();
	if (pwrLock) {
		IORecursiveLockLock(pwrLock);
		pwrHandler = nullptr;
		IORecursiveLockUnlock(pwrLock);
	}
	if (powerNotifier) {
		powerNotifier->remove();
		powerNotifier = nullptr;
	}
	if (upCall) {
		thread_call_cancel_wait(upCall);
		thread_call_free(upCall);
		upCall = nullptr;
	}
	INTELXE_PM_OIDS(INTELXE_UNREG_OID)
	if (gFwHold && bar0) { fwHold(false); gFwHold = 0; }
	irqTeardown();   // while BAR0 is still mapped: the tile master enable is cleared before the handler goes
	xeAspmRegister(false);
	sysctl_unregister_oid(&sysctl__debug_intelxe_dump);
	sysctl_unregister_oid(&sysctl__debug_intelxe_display_irqs);
	sysctl_unregister_oid(&sysctl__debug_intelxe_surfvram_wsfull);
	sysctl_unregister_oid(&sysctl__debug_intelxe_purge);
	sysctl_unregister_oid(&sysctl__debug_intelxe_purge_stats);
	sysctl_unregister_oid(&sysctl__debug_intelxe_evict);
	sysctl_unregister_oid(&sysctl__debug_intelxe_flink);
	sysctl_unregister_oid(&sysctl__debug_intelxe_fds);
	sysctl_unregister_oid(&sysctl__debug_intelxe_fd_stats);
	sysctl_unregister_oid(&sysctl__debug_intelxe_surfvram_list);
	sysctl_unregister_oid(&sysctl__debug_intelxe_surfvram_stats);
	sysctl_unregister_oid(&sysctl__debug_intelxe_surfvram_mb);
	sysctl_unregister_oid(&sysctl__debug_intelxe_surfvram);
	sysctl_unregister_oid(&sysctl__debug_intelxe_vram);
	sysctl_unregister_oid(&sysctl__debug_intelxe_sensors);
	sysctl_unregister_oid(&sysctl__debug_intelxe_jobtime_stats);
	sysctl_unregister_oid(&sysctl__debug_intelxe_direct_idle_us);
	sysctl_unregister_oid(&sysctl__debug_intelxe_direct);
	sysctl_unregister_oid(&sysctl__debug_intelxe_jobtime);
	sysctl_unregister_oid(&sysctl__debug_intelxe_stats);
	sysctl_unregister_oid(&sysctl__debug_intelxe_unbind_async);
	sysctl_unregister_oid(&sysctl__debug_intelxe_tlb_every_batch);
	sysctl_unregister_oid(&sysctl__debug_intelxe_bind_async);
	sysctl_unregister_oid(&sysctl__debug_intelxe_range_inval);
	sysctl_unregister_oid(&sysctl__debug_intelxe_hugepages);
	sysctl_unregister_oid(&sysctl__debug_intelxe_ctx_policies);
	sysctl_unregister_oid(&sysctl__debug_intelxe_rc_yield);
	sysctl_unregister_oid(&sysctl__debug_intelxe_lrc_vram);
	sysctl_unregister_oid(&sysctl__debug_intelxe_usm);
	sysctl_unregister_oid(&sysctl__debug_intelxe_dispcheck);
	sysctl_unregister_oid(&sysctl__debug_intelxe_gt_reset);
	sysctl_unregister_oid(&sysctl__debug_intelxe_job_timeout_ms);
#if INTELXE_DEBUG
	sysctl_unregister_oid(&sysctl__debug_intelxe_dumpexec);
	sysctl_unregister_oid(&sysctl__debug_intelxe_execlog);
#endif
	sysctl_unregister_oid(&sysctl__debug_intelxe_media_slpc);
	sysctl_unregister_oid(&sysctl__debug_intelxe_vcs_single);
	sysctl_unregister_oid(&sysctl__debug_intelxe_nonpriv);
	sysctl_unregister_oid(&sysctl__debug_intelxe_vcs);
	sysctl_unregister_oid(&sysctl__debug_intelxe_vcs_busy);
	sysctl_unregister_oid(&sysctl__debug_intelxe_video);
	sysctl_unregister_oid(&sysctl__debug_intelxe_irq);
	sysctl_unregister_oid(&sysctl__debug_intelxe_g2h);
	sysctl_unregister_oid(&sysctl__debug_intelxe_freq_manual);
	sysctl_unregister_oid(&sysctl__debug_intelxe_freq);
	sysctl_unregister_oid(&sysctl__debug_intelxe_hwconfig);
	sysctl_unregister_oid(&sysctl__debug_intelxe_state);
	sysctl_unregister_oid(&sysctl__debug_intelxe_step);
	sysctl_unregister_oid(&sysctl__debug_intelxe);
	gDev = nullptr;
	OSSafeReleaseNULL(sysPage);
	OSSafeReleaseNULL(lrc1Mem);
	OSSafeReleaseNULL(lrcMem);
	OSSafeReleaseNULL(miscMem);
	OSSafeReleaseNULL(ctMem);
	OSSafeReleaseNULL(adsMem);
	OSSafeReleaseNULL(fwMem);
	OSSafeReleaseNULL(bar2);
	OSSafeReleaseNULL(bar0);
	OSSafeReleaseNULL(pci);
	OSSafeReleaseNULL(gDmaMapper);
	if (pwrLock) IORecursiveLockFree(pwrLock);
	pwrLock = nullptr;
	IOService::stop(provider);
}

int IntelXe::runStep(int step) {
	IOLockLock(gSayLock);
	gLen = 0;
	gState[0] = 0;
	IOLockUnlock(gSayLock);
	say("step %d\n", step);
	int err = EINVAL;
	switch (step) {
		case 1: err = probe(); break;
		case 2: err = mapBar0() ? pcodeInitStatus() : ENXIO; break;
		case 3: err = readGt(); break;
		case 4: err = loadGuc(false); break;
		case 5: if ((err = mapBar0() ? 0 : ENXIO) == 0) gucStatus(); break;
		case 6: err = firstBatch(); break;
		case 7: err = pciDump(); break;
		case 8: err = vramTest(); break;
		case 12:   // display probe, read only (Linux i915/display: TRANSCONF, PIPESRC, TRANS_*TOTAL, TRANS_DDI_FUNC_CTL, PLANE_*_1)
			if ((err = mapBar0() ? 0 : ENXIO) == 0) {
				uint32_t g = rd(0x510a0);   // GMD_ID_DISPLAY
				say("display IP %u.%02u, PWR_WELL_CTL1 0x%08x CTL2 0x%08x\n", g >> 22, (g >> 14) & 0xff, rd(0x45400), rd(0x45404));
				auto *gsm = reinterpret_cast<volatile uint64_t *>(bar0->getVirtualAddress() + 0x800000);
				for (uint32_t p = 0; p < 4; p++) {
					const uint32_t o = p * 0x1000, ctl = rd(0x70180 + o), src = rd(0x6001c + o);
					say("pipe %c: TRANSCONF 0x%08x DDI_FUNC 0x%08x src %ux%u HTOTAL 0x%08x VTOTAL 0x%08x\n", 'A' + p,
						rd(0x70008 + o), rd(0x60400 + o), (src >> 16) + 1, (src & 0xffff) + 1, rd(0x60000 + o), rd(0x6000c + o));
					say("  plane1 CTL 0x%08x STRIDE 0x%08x SIZE 0x%08x SURF 0x%08x OFFSET 0x%08x\n", ctl, rd(0x70188 + o),
						rd(0x70190 + o), rd(0x7019c + o), rd(0x701a4 + o));
					if (ctl & 1u << 31) {
						uint32_t surf = rd(0x7019c + o) & ~0xfffu;
						say("  GGTT 0x%x -> PTE 0x%016llx\n", surf, gsm[surf >> 12]);
					}
				}
				// Present GGTT entries in the first 64 MB, as ranges (who else uses the GGTT: GOP, us from 8 MB)
				uint32_t start = ~0u, shown = 0;
				for (uint32_t i = 0; i <= 16384 && shown < 12; i++) {
					bool present = i < 16384 && (gsm[i] & 1);
					if (present && start == ~0u) start = i;
					if (!present && start != ~0u) {
						say("  GGTT 0x%07x-0x%07x present (first PTE 0x%016llx)\n", start << 12, i << 12, gsm[start]);
						start = ~0u;
						shown++;
					}
				}
			}
			break;
		case 11:   // pc_set_cur_freq: request gFreqMhz with manual RP control, as xe_guc_pc_raise_unslice does
			if ((err = mapBar0() ? 0 : ENXIO) == 0 && forcewake(true)) {
				wr(0xa024, 2u << 9);                                   // RP_CONTROL: RPSWCTL_ENABLE
				wr(0xa008, ((gFreqMhz * 3 + 25) / 50) << 23);          // RPNSWREQ ratio in 16.67 MHz units
				IODelay(300);
				uint32_t a = rd(0xc60) & 0x1ff;
				if (!gFreqKeepManual) wr(0xa024, 0);
				IODelay(1000);
				say("requested %u MHz: act %u MHz right after, %u MHz 1 ms later (manual control %s)\n", gFreqMhz,
					(a * 50 + 1) / 3, ((rd(0xc60) & 0x1ff) * 50 + 1) / 3, gFreqKeepManual ? "kept" : "released");
				forcewake(false);
			}
			break;
		case 10:   // GT frequency (xe_guc_pc.c): CAGF in MTL_MIRROR_TARGET_WP1, request in RPNSWREQ, caps in MTL_RP_STATE_CAP
			if ((err = mapBar0() ? 0 : ENXIO) == 0 && forcewake(true)) {
				auto mhz = [](uint32_t raw) { return (raw * 50 + 1) / 3; };
				uint32_t cap = rd(0x138000);
				say("act %u MHz, requested %u MHz, RP0 %u MHz, RPn %u MHz, RPe %u MHz, RPa %u MHz\n", mhz(rd(0xc60) & 0x1ff),
					mhz(rd(0xa008) >> 23 & 0x1ff), mhz(cap & 0x1ff), mhz(cap >> 16 & 0x1ff), mhz(rd(0x13800c) & 0x1ff),
					mhz(rd(0x138008) & 0x1ff));
				say("interrupts %s, %u handled\n", irqOn ? "on" : "off", irqCount);
				forcewake(false);
			}
			break;
		case 9: err = ppgttTest(); break;
		case 17: err = slpcStep(); break;   // SLPC parameters from debug.intelxe.slpc_*, then QUERY_TASK_STATE
		case 18: err = powerReport(); break;   // GT C-state, RC6/MC6 residency, RC_CONTROL, POWERGATE_ENABLE
		case 19:   // debug.intelxe.gucrc / debug.intelxe.rc6 now
			if (!up) { say("GPU not up\n"); err = ENXIO; break; }
			err = 0;
			if (gGucRc >= 0) gucRcSetup((uint32_t)gGucRc);
			if (gRc6Ctl >= 0) rc6Control(gRc6Ctl);
			if (gGucRc < 0 && gRc6Ctl < 0) say("gucrc and rc6 are both -1: nothing to do\n");
			break;
		case 20:   // read one BAR0 register (debug.intelxe.mmio_addr), without forcewake, then with it
			if (!mapBar0() || gMmioAddr < 0 || gMmioAddr >= (16 << 20) || (gMmioAddr & 3)) { err = EINVAL; break; }
			err = 0;
			say("0x%06x = 0x%08x", gMmioAddr, rd((uint32_t)gMmioAddr));
			if (forcewake(true)) { say(", 0x%08x with forcewake\n", rd((uint32_t)gMmioAddr)); forcewake(false); }
			else say("\n");
			break;
		case 13: {   // media 13.01 is its own GT: its registers below 0x40000 sit at +0x380000 (xe_gt_mmio_init)
			if (!mapBar0()) { err = ENXIO; break; }
			constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
			const bool fw = forcewake(true, M);
			const uint32_t ack = rd(M + FORCEWAKE_ACK_GT_MTL);
			uint32_t fuse = rd(M + GT_VEBOX_VDBOX_DISABLE);   // Xe_HP+: enable bits, VDBOX 7:0, VEBOX 19:16
			say("media GT: forcewake %s (ack 0x%08x), GMD_ID 0x%08x, VDBOX enable 0x%02x VEBOX enable 0x%x (0x%08x), "
				"GuC status 0x%08x, HuC load info 0x%08x\n", fw ? "ok" : "NO ACK", ack, rd(M + GMD_ID), fuse & 0xff,
				(fuse >> 16) & 0xf, fuse, rd(M + 0xc000), rd(M + 0xc1dc));
			if (fw) forcewake(false, M);
			err = fw ? 0 : ETIMEDOUT;
			break;
		}
		case 14: err = mediaGucLoad(); break;
		case 15: err = mediaFirstBatch(); break;
		case 21: err = engineStateReport(); break;   // rb7p: per-engine IDLEDLY/MAXCNT, NONPRIV, PSMI_CTL (read only)
		case 22: err = idledlyStep(); break;         // rb7p: adjust_idledly again on every engine that is up
		case 16: if ((err = mapBar0() ? 0 : ENXIO) == 0) mediaFreqReport(); break;
		default: say("unknown step\n");
	}
	say("result %d\n", err);
	IOLockLock(gSayLock);
	LOG("%s", gState);
	IOLockUnlock(gSayLock);
	return err;
}

// PCIe Extended Tag Field Enable (Device Control bit 8) on the GPU, as Linux pci_configure_extended_tags sets it for every
// device whose Device Capabilities advertise it (drivers/pci/probe.c). macOS's IOPCIFamily sets Max Payload Size and Max
// Read Request Size but never this bit, so the card has only 32 tags for its outstanding reads of system memory: with a
// platform's read latency that caps host-to-VRAM copies (blits from Shared buffers, texture uploads) well below the link.
// Measured on a Core Ultra 7 265KF + B580 (PCIe 4.0 x8) under Linux, Mesa ANV, 256 MB copy: 13.4 GB/s on the copy engine
// and 11.8 GB/s on the render engine with the bit set, 9.5 and 2.4 GB/s with it clear; VRAM-to-host (the GPU writes,
// no tags) unchanged. The PCIe spec requires every completer to handle 8-bit tags whatever its own setting. 10-bit tags
// (Device Control 2 bit 12) made no difference there and stay off. -intelxe_noexttag leaves the register alone.
// Again on wake, in case the card lost power and the register with it.
void IntelXe::pcieExtTags(bool log) {
	char dummy[8];
	if (intelxeBootArg("IntelXe", "-intelxe_noexttag", dummy, sizeof(dummy))) {
		if (log) setProperty("IntelXePCIeExtTag", "left alone (-intelxe_noexttag)");
		return;
	}
	uint8_t off = 0;
	if (!pci->findPCICapability(kIOPCIPCIExpressCapability, &off) || !off) return;
	const uint32_t devCap = pci->configRead32((UInt8)(off + XE_PCIE_DEVCAP));
	const uint16_t devCtl = pci->configRead16((UInt8)(off + XE_PCIE_DEVCTL));
	const char *what;
	if (!(devCap & XE_DEVCAP_EXT_TAG)) what = "not supported by the device";
	else if (devCtl & XE_DEVCTL_EXT_TAG) what = "on (already)";
	else {
		pci->configWrite16((UInt8)(off + XE_PCIE_DEVCTL), devCtl | XE_DEVCTL_EXT_TAG);
		what = (pci->configRead16((UInt8)(off + XE_PCIE_DEVCTL)) & XE_DEVCTL_EXT_TAG) ? "on (turned on)" : "write did not stick";
	}
	if (log) {
		LOG("PCIe Extended Tags: %s (DevCtl 0x%04x)", what, pci->configRead16((UInt8)(off + XE_PCIE_DEVCTL)));
		setProperty("IntelXePCIeExtTag", what);
	}
}

// The card must be in D0 with memory decode on before BAR0 is touched.
bool IntelXe::mapBar0() {
	if (bar0) return true;
	uint16_t pmcsr = 0;
	uint8_t pm = 0;
	if (pci->findPCICapability(kIOPCIPowerManagementCapability, &pm)) pmcsr = pci->configRead16(pm + 4);
	if (pm && (pmcsr & 3)) {
		say("PCI power state D%u, not D0: BAR0 left alone\n", pmcsr & 3);
		return false;
	}
	uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
	if (!(cmd & kIOPCICommandMemorySpace)) {
		say("memory decode was off, enabling it\n");
		pci->setMemoryEnable(true);
	}
	bar0 = pci->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, kIOMapInhibitCache);
	if (!bar0) {
		say("BAR0 map failed\n");
		return false;
	}
	say("BAR0 phys 0x%llx len 0x%llx\n", bar0->getPhysicalAddress(), bar0->getLength());
	if (bar0->getLength() < 0x1000000) {
		say("BAR0 shorter than 16 MB, refusing\n");
		OSSafeReleaseNULL(bar0);
		return false;
	}
	return true;
}

int IntelXe::probe() {
	uint8_t pm = 0;
	pci->findPCICapability(kIOPCIPowerManagementCapability, &pm);
	say("PCI id %04x:%04x rev %02x cmd 0x%04x pmcsr 0x%04x\n", pci->configRead16(kIOPCIConfigVendorID),
		pci->configRead16(kIOPCIConfigDeviceID), pci->configRead8(kIOPCIConfigRevisionID),
		pci->configRead16(kIOPCIConfigCommand), pm ? pci->configRead16(pm + 4) : 0xffff);
	if (!mapBar0()) return ENXIO;

	uint32_t g = rd(GMD_ID), m = rd(MEDIA_GT_GSI_OFFSET + GMD_ID);
	say("graphics IP %u.%02u rev %u (GMD_ID 0x%08x), media IP %u.%02u rev %u (0x%08x)\n", g >> 22, (g >> 14) & 0xff,
		g & 0x3f, g, m >> 22, (m >> 14) & 0xff, m & 0x3f, m);
	uint32_t gu = rd(GU_CNTL), s0 = rd(PCODE_SCRATCH0);
	say("GU_CNTL 0x%08x (LMEM_INIT %u), GU_DEBUG 0x%08x, tiles %u\n", gu, (gu >> 7) & 1, rd(GU_DEBUG),
		(rd(XEHP_MTCFG_ADDR) >> 8) & 0xff);
	say("PCODE_SCRATCH0 0x%08x (boot status %u%s), mailbox 0x%08x, PCIE_CAP 0x%08x\n", s0, (s0 >> 1) & 7,
		((s0 >> 1) & 7) == 4 ? " CRITICAL" : ((s0 >> 1) & 7) == 7 ? " NON-CRITICAL" : "", rd(PCODE_MAILBOX),
		rd(BMG_PCIE_CAP));
	uint32_t r = rd(SG_TILE_ADDR_RANGE0);
	say("SG_TILE_ADDR_RANGE0 0x%08x (%u GB at %u GB), GSMBASE 0x%08x%08x, DSMBASE 0x%08x%08x, STOLEN_RESERVED 0x%08x\n",
		r, (r >> 8) & 0x3ff, (r >> 1) & 0x7f, rd(GSMBASE + 4), rd(GSMBASE), rd(DSMBASE + 4), rd(DSMBASE), rd(STOLEN_RESERVED));
	say("forcewake ack GT 0x%08x render 0x%08x\n", rd(FORCEWAKE_ACK_GT), rd(FORCEWAKE_ACK_RENDER));
	say("GuC status 0x%08x, WOPCM size 0x%08x offset 0x%08x\n", rd(GUC_STATUS), rd(GUC_WOPCM_SIZE), rd(DMA_GUC_WOPCM_OFFSET));
	say("fuses: geometry DSS 0x%08x compute DSS 0x%08x vebox/vdbox disable 0x%08x\n", rd(XELP_GT_GEOMETRY_DSS_ENABLE),
		rd(XEHP_GT_COMPUTE_DSS_ENABLE), rd(GT_VEBOX_VDBOX_DISABLE));
	say("package temp 0x%08x, VRAM temp 0x%08x, fan1 0x%08x\n", rd(BMG_PACKAGE_TEMPERATURE), rd(BMG_VRAM_TEMPERATURE),
		rd(BMG_FAN_1_SPEED));
	return 0;
}

// xe_pcode.c __pcode_mailbox_rw with DGFX_PCODE_STATUS / DGFX_GET_INIT_STATUS, one try with a 10 ms wait, under
// gPcodeLock (pcode_try_request with locked, xe_pcode.c:158-160).
int IntelXe::pcodeInitStatus() {
	if (gPcodeLock) IOLockLock(gPcodeLock);
	const int err = pcodeInitStatusLocked();
	if (gPcodeLock) IOLockUnlock(gPcodeLock);
	return err;
}
int IntelXe::pcodeInitStatusLocked() {
	uint32_t mb = rd(PCODE_MAILBOX);
	if (mb & PCODE_READY) {
		say("mailbox busy 0x%08x\n", mb);
		return EBUSY;
	}
	wr(PCODE_DATA0, DGFX_GET_INIT_STATUS);   // pcode_try_request passes the request in DATA0, mbox = command only
	wr(PCODE_DATA1, 0);
	wr(PCODE_MAILBOX, PCODE_READY | DGFX_PCODE_STATUS);
	for (int i = 0; i < 100 && (rd(PCODE_MAILBOX) & PCODE_READY); i++) IODelay(100);
	mb = rd(PCODE_MAILBOX);
	uint32_t d0 = rd(PCODE_DATA0);
	say("mailbox 0x%08x (error 0x%02x), data0 0x%08x: init %s\n", mb, mb & 0xff, d0,
		(mb & PCODE_READY) ? "timeout" : (d0 & DGFX_INIT_STATUS_COMPLETE) ? "complete" : "NOT complete");
	return (mb & PCODE_READY) ? ETIMEDOUT : 0;
}

// System Information shows "VRAM (Total)" from these PCI properties; without them it took the framebuffer's 14 MB
// aperture. SG_TILE_ADDR_RANGE0 bits 17:8 hold the tile's local memory in GB (xe_vram.c). rb7u: a card that does not
// answer yet reads 0xffffffff (1023 GB in About This Mac on macOS 26): nothing is published from such a read.
void IntelXe::publishVram() {
	if (!mapBar0()) return;
	const uint32_t r = rd(SG_TILE_ADDR_RANGE0);
	const uint64_t gb = (r >> 8) & 0x3ff;
	if (r == 0xffffffffu || !gb || gb > 64) return;
	uint64_t bytes = gb << 30;
	pci->setProperty("VRAM,totalsize", &bytes, sizeof(bytes));
	pci->setProperty("VRAM,totalMB", (unsigned long long)(gb << 10), 32);
}

// rb7u: wait until the card answers, as Linux does before xe probes it: the PCI core waits for config space after a
// reset (pci_dev_wait: a device that is not ready reads 0xffff or returns CRS), then xe_device_probe_early ->
// xe_pcode_probe_early -> xe_pcode_ready polls DGFX_PCODE_STATUS / DGFX_GET_INIT_STATUS for up to 3 minutes
// (xe_pcode.c). The previous bring-up at start read config space 0xffff0000 and GT registers 0xffffffff on macOS 26.
int IntelXe::waitDeviceReady() {
	const uint64_t limitMs = 180000;
	uint64_t t0, now, cfgMs = 0;
	clock_get_uptime(&t0);
	auto elapsedMs = [&]() { clock_get_uptime(&now); uint64_t ns; absolutetime_to_nanoseconds(now - t0, &ns); return ns / 1000000; };
	uint32_t id = 0;
	for (;;) {
		id = pci->configRead32(kIOPCIConfigVendorID);
		if (id == ((uint32_t)gXePlatId << 16 | 0x8086u)) break;   // the device IntelXe started on
		if (elapsedMs() >= limitMs) { say("device not ready: config space reads 0x%08x after %llu ms\n", id, elapsedMs()); return ENXIO; }
		IOSleep(20);
	}
	cfgMs = elapsedMs();
	if (!mapBar0()) return ENXIO;
	uint32_t mb = 0, d0 = 0, tries = 0;
	for (;;) {
		IOLockLock(gPcodeLock);
		mb = rd(PCODE_MAILBOX);
		bool ready = false;
		if (mb != 0xffffffffu && !(mb & PCODE_READY)) {
			wr(PCODE_DATA0, DGFX_GET_INIT_STATUS);   // as pcodeInitStatus(), without its log line per try
			wr(PCODE_DATA1, 0);
			wr(PCODE_MAILBOX, PCODE_READY | DGFX_PCODE_STATUS);
			for (int i = 0; i < 100 && (rd(PCODE_MAILBOX) & PCODE_READY); i++) IODelay(100);
			mb = rd(PCODE_MAILBOX);
			d0 = rd(PCODE_DATA0);
			tries++;
			ready = !(mb & PCODE_READY) && (d0 & DGFX_INIT_STATUS_COMPLETE);
		}
		IOLockUnlock(gPcodeLock);
		if (ready) break;
		if (elapsedMs() >= limitMs) {
			say("pcode initialization timed out after %llu ms (mailbox 0x%08x data0 0x%08x, %u tries)\n", elapsedMs(), mb, d0, tries);
			return ETIMEDOUT;
		}
		IOSleep(20);
	}
	say("device ready: config space after %llu ms, pcode init complete after %llu ms (%u mailbox tries)\n", cfgMs,
		elapsedMs(), tries);
	return 0;
}

bool IntelXe::waitFor(uint32_t off, uint32_t mask, uint32_t val, uint32_t us, uint32_t *last) {
	uint32_t v = 0;
	for (uint32_t t = 0;; t += 10) {
		v = rd(off);
		if ((v & mask) == val || t >= us) break;
		IODelay(10);
	}
	if (last) *last = v;
	return (v & mask) == val;
}

// xe_force_wake.c: the Xe2 GT domain, FORCEWAKE_MT(FORCEWAKE_KERNEL) as a masked write with its ack in
// FORCEWAKE_ACK_GT_MTL, and on the primary GT also the RENDER domain (FORCEWAKE_RENDER, ack FORCEWAKE_ACK_RENDER):
// xe_force_wake_init_engines gives every non-media GT a render domain, and xe takes XE_FORCEWAKE_ALL around init,
// reset, resume and engine register access. RCS/CCS engine registers (RING_CMD_CCTL, GFX_MODE, HWS_PGA, ...) and the
// render/compute workaround registers are in the render domain: with only the GT domain awake, a read of one while
// GuC RC6 has the render well down returned 0 (debug.intelxe.step 20 on 0x20c4) and a write was dropped. Taken in that
// order and released in the reverse. The media GT has the GT pair at +0x380000 (xe_gt_mmio_init adj_offset) and its
// VDBOX/VEBOX domains in mediaWake. Reference counted per GT under gFwLock (xe_force_wake_get/put): ioctls
// (ENGINE_CYCLES), NMXAccel's L2 flush and the bring-up may hold it at once, and the last put alone releases. A get
// that gets no ack takes no reference.
bool IntelXe::forcewake(bool on, uint32_t gt) {
	const uint32_t g = gt ? 1 : 0;
	uint32_t ack = 0;
	bool ok = true;
	const char *dom = "GT";
	IOLockLock(gFwLock);
	if (on) {
		if (!fwCount[g]) {
			wr(gt + FORCEWAKE_GT, 0x10001);
			ok = waitFor(gt + FORCEWAKE_ACK_GT_MTL, 1, 1, 50000, &ack) && ack != ~0u;
			if (ok && !gt) {
				dom = "render";
				wr(FORCEWAKE_RENDER, 0x10001);
				ok = waitFor(FORCEWAKE_ACK_RENDER, 1, 1, 50000, &ack) && ack != ~0u;
				if (!ok) wr(FORCEWAKE_RENDER, 0x10000);
			}
			if (!ok) wr(gt + FORCEWAKE_GT, 0x10000);
		}
		if (ok) fwCount[g]++;
	} else if (fwCount[g] && !--fwCount[g]) {
		if (!gt) {
			wr(FORCEWAKE_RENDER, 0x10000);
			if (!waitFor(FORCEWAKE_ACK_RENDER, 1, 0, 50000, &ack)) { ok = false; dom = "render"; }
		}
		wr(gt + FORCEWAKE_GT, 0x10000);
		if (!waitFor(gt + FORCEWAKE_ACK_GT_MTL, 1, 0, 50000, &ack)) ok = false;
	}
	IOLockUnlock(gFwLock);
	if (!ok) say("%sforcewake %s (%s): no ack (0x%08x)\n", gt ? "media " : "", on ? "on" : "off", dom, ack);
	return ok;
}

// The media GT and its VDBOX/VEBOX domains (ENGINE_CYCLES on a video engine), counted like forcewake.
bool IntelXe::mediaWake(bool on) {
	if (on && !forcewake(true, MEDIA_GT_GSI_OFFSET)) return false;
	bool ok = true;
	IOLockLock(gFwLock);
	if (on) {
		if (!mediaEngCount && !(ok = mediaEngineWake(true))) mediaEngineWake(false);
		if (ok) mediaEngCount++;
	} else if (mediaEngCount && !--mediaEngCount) {
		mediaEngineWake(false);
	}
	IOLockUnlock(gFwLock);
	if (!on || !ok) forcewake(false, MEDIA_GT_GSI_OFFSET);
	return ok;
}

static uint32_t patReg(int i) { return i < 8 ? 0x4800 + i * 4 : 0x4848 + (i - 8) * 4; }   // _PAT_INDEX

int IntelXe::readGt() {
	if (!mapBar0()) return ENXIO;
	if (!forcewake(true)) return ETIMEDOUT;
	say("fuses: geometry DSS 0x%08x 0x%08x 0x%08x, compute DSS 0x%08x 0x%08x 0x%08x, EU 0x%08x, vebox/vdbox off 0x%08x\n",
		rd(XELP_GT_GEOMETRY_DSS_ENABLE), rd(XE2_GT_GEOMETRY_DSS_1), rd(XE2_GT_GEOMETRY_DSS_2), rd(XEHP_GT_COMPUTE_DSS_ENABLE),
		rd(XEHPC_GT_COMPUTE_DSS_ENABLE_EXT), rd(XE2_GT_COMPUTE_DSS_2), rd(XELP_EU_ENABLE), rd(GT_VEBOX_VDBOX_DISABLE));
	say("PAT:");
	for (int i = 0; i < 32; i++) say(" %x", rd(patReg(i)));
	say("\nPAT_ATS 0x%x PAT_PTA 0x%x\nMOCS:", rd(PAT_ATS), rd(PAT_PTA));
	for (int i = 0; i < 16; i++) say(" %x", rd(GLOBAL_MOCS0 + i * 4));
	say("\n");
	gucStatus();
	forcewake(false);
	return 0;
}

void IntelXe::gucStatus(uint32_t gt) {
	uint32_t s = rd(gt + GUC_STATUS);
	say("GUC_STATUS 0x%08x: auth %u, MIA %u, ukernel 0x%02x, bootrom 0x%02x, in reset %u\n", s, s >> 30, (s >> 16) & 7,
		(s >> 8) & 0xff, (s >> 1) & 0x7f, s & 1);
	say("BOOT_HASH_CHK 0x%08x, HEADER_INFO 0x%08x, WOPCM size 0x%08x offset 0x%08x, SHIM 0x%08x, DMA_CTRL 0x%08x\n",
		rd(gt + BOOT_HASH_CHK), rd(gt + GUC_HEADER_INFO), rd(gt + GUC_WOPCM_SIZE), rd(gt + DMA_GUC_WOPCM_OFFSET),
		rd(gt + GUC_SHIM_CONTROL), rd(gt + DMA_CTRL));
	say("SOFT_SCRATCH 0-3: 0x%08x 0x%08x 0x%08x 0x%08x\n", rd(gt + SOFT_SCRATCH0), rd(gt + SOFT_SCRATCH0 + 4),
		rd(gt + SOFT_SCRATCH0 + 8), rd(gt + SOFT_SCRATCH0 + 12));
}

// xe_pat.c xe2_pat_table (28 entries on graphics 20.01, Wa_16023588340), xe2_pat_ats, xe2_pat_pta (dGFX);
// xe_mocs.c xe2_mocs_table, unused entries = entry 4. Multicast writes need no steering (xe_gt_mcr_multicast_write).
// The media GT takes the same tables as plain registers at +0x380000 (xe2_pat_ops program_media = program_pat with
// pat_media_pta = xe2_pat_pta on dGFX; regs_are_mcr false for media < 20, uc_index 3).
void IntelXe::programPatMocs(uint32_t gt) {
	#define XE2_PAT(np, comp, clos, l3, l4, coh) ((np) << 10 | (comp) << 9 | (clos) << 6 | (l3) << 4 | (l4) << 2 | (coh))
	static const uint32_t pat[28] = {
		XE2_PAT(0, 0, 0, 0, 3, 0), XE2_PAT(0, 0, 0, 0, 3, 2), XE2_PAT(0, 0, 0, 0, 3, 3), XE2_PAT(0, 0, 0, 3, 3, 0),
		XE2_PAT(0, 0, 0, 3, 0, 2), XE2_PAT(0, 0, 0, 3, 3, 2), XE2_PAT(1, 0, 0, 1, 3, 0), XE2_PAT(0, 0, 0, 3, 0, 3),
		XE2_PAT(0, 0, 0, 3, 0, 0), XE2_PAT(0, 1, 0, 0, 3, 0), XE2_PAT(0, 1, 0, 3, 0, 0), XE2_PAT(1, 1, 0, 1, 3, 0),
		XE2_PAT(0, 1, 0, 3, 3, 0), XE2_PAT(0, 0, 0, 0, 0, 0), XE2_PAT(0, 1, 0, 0, 0, 0), XE2_PAT(1, 1, 0, 1, 1, 0),
		0, 0, 0, 0,
		XE2_PAT(0, 0, 1, 0, 3, 0), XE2_PAT(0, 1, 1, 0, 3, 0), XE2_PAT(0, 0, 1, 0, 3, 2), XE2_PAT(0, 0, 1, 0, 3, 3),
		XE2_PAT(0, 0, 2, 0, 3, 0), XE2_PAT(0, 1, 2, 0, 3, 0), XE2_PAT(0, 0, 2, 0, 3, 2), XE2_PAT(0, 0, 2, 0, 3, 3),
	};
	for (int i = 0; i < 28; i++) wr(gt + patReg(i), pat[i]);
	wr(gt + PAT_ATS, XE2_PAT(0, 0, 0, 0, 3, 3));
	wr(gt + PAT_PTA, XE2_PAT(0, 0, 0, 0, 3, 0));
	#undef XE2_PAT
	static const uint32_t mocs[5] = {0x00c, 0x10c, 0x130, 0x13c, 0x100};   // IG_PAT 8, L3 5:4, L4 3:2
	for (int i = 0; i < 16; i++) wr(gt + GLOBAL_MOCS0 + i * 4, mocs[i < 5 ? i : 4]);
}

void flushRange(void *p, size_t len) {
	for (uintptr_t a = reinterpret_cast<uintptr_t>(p) & ~63ul, e = reinterpret_cast<uintptr_t>(p) + len; a < e; a += 64)
		asm volatile("clflush (%0)" : : "r"(a) : "memory");
	asm volatile("mfence" : : : "memory");
}

// Every GGTT mapping made here, with the device addresses of its pages (DmaMap: IOVAs under VT-d), so resume can write
// the PTEs again (GSM is stolen VRAM: lost in D3cold, as xe restores pinned GGTT BOs in xe_bo_restore_early). Mapping a
// range again reuses its DMA mapping if the memory is the same; ggttClear completes it.
// mem == nullptr: a VRAM range (dpa, len), as the LRCs in VRAM since rb7 (ggttMapVram).
struct GgttRec { IOBufferMemoryDescriptor *mem; uint32_t ggtt, gt; DmaMap dma; uint64_t dpa; uint32_t len; bool uc; };
static GgttRec *gGgttRec;
static uint32_t gGgttRecN, gGgttRecCap;
static IOLock *gGgttLock;
uint32_t ggttRecCount() {   // GGTT mappings of system memory IntelXe keeps (rewritten at resume): LRCs, CT, ADS, seqnos
	if (!gGgttLock) return 0;
	IOLockLock(gGgttLock);
	const uint32_t n = gGgttRecN;
	IOLockUnlock(gGgttLock);
	return n;
}

// GGTT PTEs live at BAR0 + 8 MB (xe_ggtt_init_early), 8 bytes per 4 KB page: device address | PRESENT, PAT index 2 (Xe2
// WB, 2-way coherent: XE_CACHE_WB) in bits 52/53 (xelpg_ggtt_pte_flags). Then the MMIO GGTT TLB invalidation that
// send_tlb_inval_ggtt uses on graphics 20 while GuC is not running (on the given GT's registers).
int IntelXe::ggttMap(IOBufferMemoryDescriptor *mem, uint32_t ggtt, uint32_t gt, uint32_t *inval) {
	if (!gGgttLock || !mem) return EINVAL;
	IOLockLock(gGgttLock);
	uint32_t r = 0;
	while (r < gGgttRecN && !(gGgttRec[r].ggtt == ggtt && gGgttRec[r].gt == gt)) r++;
	if (r < gGgttRecN && gGgttRec[r].mem != mem) gGgttRec[r].dma.unmap();
	if (r == gGgttRecN) {
		if (gGgttRecN == gGgttRecCap) {
			const uint32_t nc = gGgttRecCap ? gGgttRecCap * 2 : 256;
			auto *n = static_cast<GgttRec *>(IOMallocZero(nc * sizeof(GgttRec)));
			if (!n) {
				IOLockUnlock(gGgttLock);
				return ENOMEM;
			}
			if (gGgttRec) {
				memcpy(n, gGgttRec, gGgttRecN * sizeof(GgttRec));
				IOFree(gGgttRec, gGgttRecCap * sizeof(GgttRec));
			}
			gGgttRec = n;
			gGgttRecCap = nc;
		}
		gGgttRec[gGgttRecN++] = GgttRec{};
	}
	GgttRec &rec = gGgttRec[r];
	rec.mem = mem;
	rec.ggtt = ggtt;
	rec.gt = gt;
	rec.dpa = 0;
	rec.len = 0;
	int err = rec.dma.n || rec.dma.map(mem) ? 0 : EFAULT;
	volatile uint64_t *gsm = reinterpret_cast<volatile uint64_t *>(bar0->getVirtualAddress() + 0x800000);
	for (IOByteCount off = 0; off < mem->getLength() && !err; off += 4096) {
		uint64_t pa = 0, run = 0;
		if (!rec.dma.lookup(off, &pa, &run) || (pa & 0xfff) || pa >= xePlatformDmaLimit(gXePlat)) {
			say("bad device address 0x%llx at +0x%llx\n", pa, (uint64_t)off);
			err = EFAULT;
			break;
		}
		gsm[(ggtt + off) >> 12] = pa | 1 | (1ull << 53);
	}
	if (err) {
		rec.dma.unmap();
		gGgttRec[r] = gGgttRec[--gGgttRecN];
	}
	IOLockUnlock(gGgttLock);
	if (err) return err;
	(void)gsm[(ggtt >> 12)];
	// rb8d: xe_ggtt.c xe_ggtt_insert_bo of an XE_BO_FLAG_GGTT_INVALIDATE BO (an LRC, the CT) invalidates and waits
	const uint32_t seq = xeGgttInvalidate(gt);
	if (inval) *inval = seq;
	else xeGgttInvalWait(seq);
	return 0;
}

// A VRAM range in the GGTT (xe_ggtt_map_bo for a VRAM BO): DPA | PRESENT | XE_GGTT_PTE_DM (bit 1, local memory), PAT
// index 2 = XE_CACHE_WB as xe_ggtt_map_bo uses for kernel BOs without XE_BO_FLAG_NEEDS_UC. Recorded like the system
// mappings, so gpuResume writes it again after D3cold (xe_bo_restore_and_map_ggtt); ggttClear completes it.
int IntelXe::ggttMapVram(uint64_t dpa, uint32_t len, uint32_t ggtt, uint32_t gt, bool uc, uint32_t *inval) {
	if (!gGgttLock || !len || (dpa & 0xfff) || (len & 0xfff)) return EINVAL;
	IOLockLock(gGgttLock);
	uint32_t r = 0;
	while (r < gGgttRecN && !(gGgttRec[r].ggtt == ggtt && gGgttRec[r].gt == gt)) r++;
	if (r < gGgttRecN && gGgttRec[r].mem) gGgttRec[r].dma.unmap();
	if (r == gGgttRecN) {
		if (gGgttRecN == gGgttRecCap) {
			const uint32_t nc = gGgttRecCap ? gGgttRecCap * 2 : 256;
			auto *n = static_cast<GgttRec *>(IOMallocZero(nc * sizeof(GgttRec)));
			if (!n) {
				IOLockUnlock(gGgttLock);
				return ENOMEM;
			}
			if (gGgttRec) {
				memcpy(n, gGgttRec, gGgttRecN * sizeof(GgttRec));
				IOFree(gGgttRec, gGgttRecCap * sizeof(GgttRec));
			}
			gGgttRec = n;
			gGgttRecCap = nc;
		}
		gGgttRec[gGgttRecN++] = GgttRec{};
	}
	GgttRec &rec = gGgttRec[r];
	rec = GgttRec{};
	rec.ggtt = ggtt;
	rec.gt = gt;
	rec.dpa = dpa;
	rec.len = len;
	rec.uc = uc;
	// PAT index 2 (WB), or 3 (XE_CACHE_NONE on Xe2, for XE_BO_FLAG_NEEDS_UC: the GuC's UM queues): bits 52 / 53
	const uint64_t pat = uc ? 3ull << 52 : 1ull << 53;
	volatile uint64_t *gsm = reinterpret_cast<volatile uint64_t *>(bar0->getVirtualAddress() + 0x800000);
	for (uint32_t off = 0; off < len; off += 4096) gsm[(ggtt + off) >> 12] = (dpa + off) | 1 | 2 | pat;
	IOLockUnlock(gGgttLock);
	(void)gsm[(ggtt >> 12)];
	// rb8d: xe_ggtt.c xe_ggtt_insert_bo of an XE_BO_FLAG_GGTT_INVALIDATE BO (an LRC, the CT) invalidates and waits
	const uint32_t seq = xeGgttInvalidate(gt);
	if (inval) *inval = seq;
	else xeGgttInvalWait(seq);
	return 0;
}

// The GGTT TLB invalidation over MMIO (xe_guc_tlb_inval.c send_tlb_inval_ggtt, the path without a GuC action; rb8d: only
// while the CT is not up, xe_core.cpp xeGgttInvalidate sends the GuC action otherwise and waits for it):
// PVC_GUC_TLB_INV_DESC1/DESC0 are GT-domain registers, written with forcewake held (xe takes XE_FW_GT there). Without
// it a write while the GT slept was dropped and the GGTT TLB kept a translation of a range just remapped (an LRC
// window reused by the next queue on that slot, a scan-out range).
void IntelXe::ggttInvalidate(uint32_t gt) {
	const bool fw = forcewake(true, gt);
	wr(gt + PVC_GUC_TLB_INV_DESC1, 1u << 6);
	wr(gt + PVC_GUC_TLB_INV_DESC0, 1u << 0);
	if (fw) forcewake(false, gt);
}

// xe_ggtt_remove_node: a freed range's PTEs stop pointing at pages that go back to the system, and its DMA mapping goes.
// rb8d: the invalidation is waited for before the DMA mapping goes and before the caller hands the range or its memory
// out again, as ggtt_node_remove -> xe_ggtt_invalidate does for an XE_BO_FLAG_GGTT_INVALIDATE BO (an LRC); not with
// gResLock held (the callers drop it first).
void IntelXe::ggttClear(uint32_t ggtt, uint32_t len, uint32_t gt) {
	volatile uint64_t *gsm = reinterpret_cast<volatile uint64_t *>(bar0->getVirtualAddress() + 0x800000);
	for (uint32_t off = 0; off < len; off += 4096) gsm[(ggtt + off) >> 12] = 0;
	(void)gsm[ggtt >> 12];
	xeGgttInvalWait(xeGgttInvalidate(gt));
	if (!gGgttLock) return;
	IOLockLock(gGgttLock);
	for (uint32_t i = 0; i < gGgttRecN; i++)
		if (gGgttRec[i].ggtt == ggtt && gGgttRec[i].gt == gt) {
			gGgttRec[i].dma.unmap();
			gGgttRec[i] = gGgttRec[--gGgttRecN];
			break;
		}
	IOLockUnlock(gGgttLock);
}

IOBufferMemoryDescriptor *IntelXe::alloc(size_t len, bool contiguous) {
	auto *m = IOBufferMemoryDescriptor::inTaskWithPhysicalMask(kernel_task,
		kIODirectionInOut | (contiguous ? kIOMemoryPhysicallyContiguous : 0), round_page(len), xePlatformPhysMask(gXePlat));
	if (m) bzero(m->getBytesNoCopy(), m->getLength());
	return m;
}

// xe_guc_reset: GDRST GRDOM_GUC, then the MIA core must report reset. On the media GT first Wa_14025883347
// (guc_prevent_fw_dma_failure_on_reset): with a running GuC, disable its idle flow and let SRAM save/restore finish.
// ─── DMA mappings (DmaMap, IntelXe.h) ──────────────────────────────────────────────────────────────────────────────
IOMapper *gDmaMapper;

bool DmaMap::map(IOMemoryDescriptor *md) {
	unmap();
	if (!md) return false;
	cmd = IODMACommand::withSpecification(kIODMACommandOutputHost64, gXePlat->dmaMaskBits, 0, IODMACommand::kMapped, 0, 0x1000,
		gDmaMapper);   // dma_mask_size
	if (!cmd) return false;
	if (cmd->setMemoryDescriptor(md, true) != kIOReturnSuccess) {   // and prepare(): the IOVA mapping is made here
		OSSafeReleaseNULL(cmd);
		return false;
	}
	const uint64_t len = md->getLength();
	uint64_t off = 0;
	while (off < len) {
		if (n == cap) {
			const uint32_t nc = cap ? cap * 2 : 16;
			auto *ns = static_cast<IODMACommand::Segment64 *>(IOMalloc(nc * sizeof(IODMACommand::Segment64)));
			auto *no = static_cast<uint64_t *>(IOMalloc(nc * sizeof(uint64_t)));
			if (!ns || !no) {
				if (ns) IOFree(ns, nc * sizeof(IODMACommand::Segment64));
				if (no) IOFree(no, nc * sizeof(uint64_t));
				unmap();
				return false;
			}
			if (seg) {
				memcpy(ns, seg, n * sizeof(IODMACommand::Segment64));
				memcpy(no, start, n * sizeof(uint64_t));
				IOFree(seg, cap * sizeof(*seg));
				IOFree(start, cap * sizeof(uint64_t));
			}
			seg = ns;
			start = no;
			cap = nc;
		}
		const uint64_t at = off;
		UInt32 got = 1;
		if (cmd->genIOVMSegments(&off, &seg[n], &got) != kIOReturnSuccess || !got || off <= at) {
			unmap();
			return false;
		}
		// merge with the previous segment when the device addresses continue (one IOVA range under VT-d)
		if (n && seg[n - 1].fIOVMAddr + seg[n - 1].fLength == seg[n].fIOVMAddr) seg[n - 1].fLength += seg[n].fLength;
		else start[n++] = at;
	}
	return true;
}

void DmaMap::unmap() {
	if (cmd) {
		cmd->clearMemoryDescriptor(true);   // complete(): the IOVA mapping goes
		OSSafeReleaseNULL(cmd);
	}
	if (seg) IOFree(seg, cap * sizeof(*seg));
	if (start) IOFree(start, cap * sizeof(uint64_t));
	seg = nullptr;
	start = nullptr;
	n = cap = 0;
}

bool DmaMap::lookup(uint64_t off, uint64_t *addr, uint64_t *run) const {
	uint32_t lo = 0, hi = n;
	while (lo < hi) {   // the last segment starting at or before off
		const uint32_t mid = (lo + hi) / 2;
		if (start[mid] <= off) lo = mid + 1;
		else hi = mid;
	}
	if (!lo) return false;
	const uint32_t i = lo - 1;
	if (off - start[i] >= seg[i].fLength) return false;
	*addr = seg[i].fIOVMAddr + (off - start[i]);
	*run = seg[i].fLength - (off - start[i]);
	return true;
}

// ─── bring-up at start, GT reset, shutdown ────────────────────────────────────────────────────────────────────────
void IntelXe::bringUpAsync() {
	if (!upCall)
		upCall = thread_call_allocate_with_options([](thread_call_param_t p0, thread_call_param_t) {
			auto *d = static_cast<IntelXe *>(p0);
			// how the previous shutdown went (intelxeCrumb), then cleared for this run; NVRAM is published after IntelXe
			// starts (it loads early from OpenCore), so it is waited for here
			if (IOService *nv = IOService::waitForMatchingService(IOService::serviceMatching("IODTNVRAM"), 10000000000ull)) {
				OSObject *prev = nv->copyProperty("intelxe-shutdown");
				if (auto *str = OSDynamicCast(OSString, prev)) say("previous shutdown: IntelXe reached \"%s\"\n", str->getCStringNoCopy());
				else if (auto *dd = OSDynamicCast(OSData, prev))
					say("previous shutdown: IntelXe reached \"%.*s\"\n", (int)dd->getLength(), static_cast<const char *>(dd->getBytesNoCopy()));
				else say("previous shutdown: no IntelXe record\n");
				OSSafeReleaseNULL(prev);
				nv->removeProperty("intelxe-shutdown");
				nv->release();
			}
			// rb7u: macOS 26 starts the drivers while the card is still coming out of its own reset (config space and
			// BAR0 read 0xffff0000 / 0xffffffff for several seconds), so first wait as Linux does before probing
			// The card also drops out for a while after answering (forcewake ack 0xffffffff right after pcode said ready,
			// config space 0xffff0000 at 14 s): retry the bring-up once a second for up to 3 minutes.
			int err = ENXIO, tries = 0;
			for (; tries < 180; tries++) {
				IOLockLock(gDevLock);
				err = d->up ? 0 : d->waitDeviceReady();
				if (!err && !d->up) err = d->bringUp();
				IOLockUnlock(gDevLock);
				if (!err) break;
				IOSleep(1000);
			}
			LOG("bring-up at start: %d after %d retries", err, tries);
			// IntelXeFB and NMXAccel match only a device with IntelXeReady (their IOPropertyMatch): they start once the GuC runs,
			// never on a card that is still dropping out. Publishing the device again offers it to them now.
			if (!err) {
				// direct submission on before the first application can open a queue (outside gDevLock: it takes it)
				if (gDirectWanted) { const int de = xeDirectSet(1); LOG("direct submission at start: %s (%d)", de ? "off" : "on", de); }
				d->publishVram();   // rb7u: read again on a card that answers (at start() it may read 0xffffffff)
				d->pci->setProperty("IntelXeReady", true);
				d->pci->registerService();
			}
		}, this, THREAD_CALL_PRIORITY_KERNEL, THREAD_CALL_OPTIONS_ONCE);
	if (upCall) thread_call_enter(upCall);
}

// GT reset (gt_reset_worker -> do_gt_reset, do_gt_restart): a full graphics reset (GDRST GRDOM_FULL, waited for up to
// 5 s as GRDOM_RESET_TIMEOUT_MS), then the engine registers and workarounds, the GuC load and its CT, SLPC and GuC RC
// again; WOPCM stays locked, the GGTT and the golden contexts (system memory, ADS) survive. Every TLB is empty after it,
// so every invalidation issued counts as done. The caller (xe_core.cpp xeGtReset) stopped submission and re-registers
// the contexts afterwards. gDevLock.
int IntelXe::gtReset() {
	if (!up || !mapBar0()) return ENXIO;
	if (!forcewake(true)) return ETIMEDOUT;
	// rb7: the LRCs live in VRAM, written by the CPU through BAR2, which lands in the GPU's L2 (Wa_16023588340): write it
	// back before the full reset, so no ring or context field the next registration needs can stay behind in it.
	// Defensive (xe's do_gt_reset has no flush); a hung GPU makes it time out after 1 ms, which changes nothing.
	wr(0xb404, 1);   // XE2_GLOBAL_INVAL
	uint32_t gi = 0;
	waitFor(0xb404, 1, 0, 1000, &gi);
	wr(0x941c, 1);   // GDRST: GRDOM_FULL
	uint32_t g = 0;
	const bool done = waitFor(0x941c, 1, 0, 5000000, &g);
	forcewake(false);
	say("GT reset: GDRST %s (0x%08x)\n", done ? "done" : "TIMEOUT", g);
	if (!done) return ETIMEDOUT;
	IOLockLock(gH2gLock[0]);
	IOLockLock(gG2hLock[0]);
	ctBroken[0] = false;
	tlbDone = tlbSeqno;
	IOLockUnlock(gG2hLock[0]);
	IOLockUnlock(gH2gLock[0]);
	int err = firstBatchSetupOnly();   // engines, workarounds, GuC load, hwconfig, CT
	if (!err && forcewake(true)) {
		wr(0x4194, rd(0x4194) | 1u << 6);   // Wa_16023588340 as bringUp applies it: XE2_GAMREQSTRM_CTRL CG_DIS_CNTLBUS
		wr(0xb194 + 3 * 8, 0xf);             //                                     XEHPC_L3CLOS_MASK(3)
		pcStart();
		forcewake(false);
	}
	if (!err && irqOn) irqGtEnable();
	tlbDone = tlbSeqno;
	say("GT reset: restart %d\n", err);
	return err;
}

void IntelXe::gtStop() {
	if (!up || !mapBar0() || !forcewake(true)) return;
	wr(0x941c, 1);   // GDRST: GRDOM_FULL
	uint32_t g = 0;
	waitFor(0x941c, 1, 0, 500000, &g);
	forcewake(false);
	LOG("shutdown: GT stopped (GDRST 0x%08x)", g);
}

// System sleep (the power plane calls this with the PCI device's state: 0 before it goes to D3, 1 after it is back in
// D0 with its config space restored). Synchronous: the GPU is quiet or running again on return.
// IntelXeFB's handler ("intelxePowerEvent"): 1 will suspend (first, while the display engine is powered), 2 resumed (after the
// device level is back: ReBAR/BAR2, pcode, GGTT, interrupts), 3/4 GT reset begins/done. On IntelXe's thread, never under
// gDispLock (IntelXeFB unregisters its display interrupt from inside event 1).
void IntelXe::powerEvent(uint32_t event) {
	if (!pwrLock) return;
	IORecursiveLockLock(pwrLock);
	if (pwrHandler) pwrHandler(pwrCtx, event);
	IORecursiveLockUnlock(pwrLock);
}

void xePowerEvent(uint32_t event) {
	if (gDev) gDev->powerEvent(event);
}

IOReturn IntelXe::setPowerState(unsigned long ordinal, IOService *) {
	if (ordinal == 0 && !pwrSuspendSent) {
		powerEvent(1);
		pwrSuspendSent = true;
		if (up && !suspended) {
			const int err = xeSuspend();
			LOG("suspend: %d", err);
		}
	} else if (ordinal == 1 && pwrSuspendSent) {
		if (suspended) {
			const int err = xeResume();
			LOG("resume: %d", err);
		}
		pwrSuspendSent = false;
		if (!suspended) powerEvent(2);   // the device level came back (or was never suspended)
		else LOG("resume: device not back, display not told");
		if (!suspended) xeAspmWake();   // debug.intelxe.aspm_l1 0 or 1: again where the wake lost it (no-op at -1)
	}
	return kIOPMAckImplied;
}

// xe_uc_suspend -> xe_guc_suspend: CLIENT_SOFT_RESET over MMIO (the GuC stops scheduling and saves nothing more), then
// the GT interrupts and the tile master off (xe_irq_suspend). gDevLock.
void IntelXe::gpuSuspend() {
	if (!up || !mapBar0()) return;
	if (forcewake(true)) {
		uint32_t a[1] = {0x5507}, reply = 0;   // XE_GUC_ACTION_CLIENT_SOFT_RESET
		const int err = mmioSend(a, 1, &reply);
		forcewake(false);
		say("suspend: GuC soft reset %d\n", err);
	}
	if (irqOn) wr(0x190008, 0);   // DG1_MSTR_TILE_INTR
	IOLockLock(gFwLock);
	const uint32_t f0 = fwCount[0], f1 = fwCount[1];
	IOLockUnlock(gFwLock);
	if (f0 || f1) say("suspend: forcewake still referenced (GT %u, media %u): re-requested at resume\n", f0, f1);
	// The energy counters may restart while the card is off: the first reading after resume only re-bases them. Set
	// together with `suspended` under gPmtLock, so that no PMT read between the two takes the re-base before sleep.
	IOLockLock(gPmtLock);
	IOLockLock(gPcodeLock);   // and no pcode mailbox read (pcodeRead, powerLimitsRead) after this point
	energy.resync = true;
	suspended = true;
	IOLockUnlock(gPcodeLock);
	IOLockUnlock(gPmtLock);
}

// D3cold clears the forcewake requests in the hardware, but not our reference counts: a domain still referenced at
// suspend (it should not be: xe expects every get to be put by then) would read as held while the GT and render wells
// sleep, and the resume's engine programming would be dropped. Request again what is still referenced (gFwLock), and
// the debug hold (debug.intelxe.fw_hold). Media: its GT is powered with the device; its VDBOX/VEBOX domains come back
// through mediaGucLoad.
void IntelXe::forcewakeRestore() {
	IOLockLock(gFwLock);
	uint32_t ack = 0;
	bool ok = true;
	if (fwCount[0]) {
		wr(FORCEWAKE_GT, 0x10001);
		ok = waitFor(FORCEWAKE_ACK_GT_MTL, 1, 1, 50000, &ack) && ok;
		wr(FORCEWAKE_RENDER, 0x10001);
		ok = waitFor(FORCEWAKE_ACK_RENDER, 1, 1, 50000, &ack) && ok;
	}
	if (fwCount[1]) {
		wr(MEDIA_GT_GSI_OFFSET + FORCEWAKE_GT, 0x10001);
		ok = waitFor(MEDIA_GT_GSI_OFFSET + FORCEWAKE_ACK_GT_MTL, 1, 1, 50000, &ack) && ok;
	}
	const uint32_t f0 = fwCount[0], f1 = fwCount[1];
	IOLockUnlock(gFwLock);
	if (f0 || f1) say("resume: forcewake re-requested (GT %u, media %u)%s\n", f0, f1, ok ? "" : " -- NO ACK");
	if (gFwHold) fwHold(true);
}

// xe_pm_resume for the device level: the Resizable BAR size the platform may have reset (pci_restore_rebar_state: size
// first, then the BAR address again), pcode ready (xe_pcode_ready), every GGTT mapping written again, the GT set up and
// the GuC loaded as at bring-up (WOPCM is programmed again if it lost its lock), SLPC and GuC RC, interrupts. The
// caller (xeResume) rewrites the page tables, registers the queues again and restores the VRAM. gDevLock.
int IntelXe::gpuResume() {
	if (rebarOff && rebarN) {
		bool changed = false;
		for (uint32_t i = 0; i < rebarN && i < 6; i++)
			changed |= (pci->extendedConfigRead32(rebarOff + 8 + 8 * i) & 0x3f00) != (rebarCtl[i] & 0x3f00);
		if (changed) {
			const uint16_t cmd = pci->configRead16(kIOPCIConfigCommand);
			pci->configWrite16(kIOPCIConfigCommand, cmd & ~(uint16_t)2);   // memory decode off while the BAR changes
			for (uint32_t i = 0; i < rebarN && i < 6; i++) {
				const uint32_t c = pci->extendedConfigRead32(rebarOff + 8 + 8 * i);
				pci->extendedConfigWrite32(rebarOff + 8 + 8 * i, (c & ~0x3f00u) | (rebarCtl[i] & 0x3f00));
			}
			pci->configWrite32(0x18, bar2Lo);
			pci->configWrite32(0x1c, bar2Hi);
			pci->configWrite16(kIOPCIConfigCommand, cmd);
			say("resume: Resizable BAR size restored\n");
		}
	}
	pcieExtTags(false);
	if (!mapBar0()) return ENXIO;
	int err = ETIMEDOUT;
	for (int i = 0; i < 100 && err; i++) {   // up to ~10 s for pcode (xe_pcode_ready waits as long after a cold boot)
		err = pcodeInitStatus();
		if (err) IOSleep(100);
	}
	if (err) return err;
	forcewakeRestore();   // after pcode reports the device ready: the GT acks only then
	if (gGgttLock) {
		IOLockLock(gGgttLock);
		GgttRec *r = gGgttRecN ? static_cast<GgttRec *>(IOMalloc(gGgttRecN * sizeof(GgttRec))) : nullptr;
		const uint32_t n = r ? gGgttRecN : 0;
		if (r) memcpy(r, gGgttRec, n * sizeof(GgttRec));
		IOLockUnlock(gGgttLock);
		for (uint32_t i = 0; i < n; i++)   // one GGTT for both GTs
			if (r[i].mem) ggttMap(r[i].mem, r[i].ggtt, r[i].gt);
			else ggttMapVram(r[i].dpa, r[i].len, r[i].ggtt, r[i].gt, r[i].uc);
		if (r) IOFree(r, n * sizeof(GgttRec));
	}
	IOLockLock(gH2gLock[0]);
	IOLockLock(gG2hLock[0]);
	ctBroken[0] = false;
	IOLockUnlock(gG2hLock[0]);
	IOLockUnlock(gH2gLock[0]);
	if ((err = firstBatchSetupOnly())) return err;
	if (forcewake(true)) {
		wr(0x4194, rd(0x4194) | 1u << 6);   // Wa_16023588340 (bringUp)
		wr(0xb194 + 3 * 8, 0xf);
		pcStart();
		forcewake(false);
	}
	if (irqOn) {
		wr(0x190008, 1u << 31);   // DG1_MSTR_TILE_INTR
		irqGtEnable();
	}
	tlbDone = tlbSeqno;
	suspended = false;
	if (gVideoWanted && !mediaGucLoad()) say("resume: media GT up\n");   // xe_gt_resume for the media GT
	return 0;
}

// System power-off and restart (registerPrioritySleepWakeInterest): quiesce before the platform goes down. Sleep
// messages are acknowledged without action (suspend/resume is setPowerState's job).
// The shutdown stages IntelXe reached, in NVRAM ("nvram intelxe-shutdown"; the next start reports it in debug.intelxe.state):
// whether a power-off that did not finish cleanly stopped in IntelXe or after it.
void intelxeCrumb(const char *stage) {
	IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane);
	if (!o) return;
	if (OSString *v = OSString::withCString(stage)) {
		o->setProperty("intelxe-shutdown", v);
		v->release();
	}
	o->setProperty(kIONVRAMSyncNowPropertyKey, kOSBooleanTrue);
	o->release();
}

void IntelXe::systemWillShutdown(IOOptionBits specifier) {
	intelxeCrumb("4 power plane shutdown reached IntelXe");
	IOService::systemWillShutdown(specifier);
}

IOReturn IntelXe::powerMessage(void *target, void *, UInt32 type, IOService *, void *, vm_size_t) {
	if (type == kIOMessageSystemWillPowerOff || type == kIOMessageSystemWillRestart) {
		intelxeCrumb("1 power-off message: quiescing");
		xeShutdown();
		intelxeCrumb("3 quiesce done");
	}
	(void)target;
	return kIOReturnSuccess;
}

bool IntelXe::gucReset(uint32_t gt) {
	uint32_t g;
	if (gt && !(rd(gt + GUC_STATUS) & 1) && (rd(gt + BOOT_HASH_CHK) & 1u << 31)) {
		wr(gt + 0xc3e4, rd(gt + 0xc3e4) | 1u << 31);                     // GUC_MAX_IDLE_COUNT: GUC_IDLE_FLOW_DISABLE
		waitFor(gt + GUC_STATUS, 0xff00, 0xf000, 100000, &g);              // ukernel READY
		waitFor(gt + 0xc398, 3u << 7, 0, 5000, &g);                       // GUC_SRAM_STATUS handling done
	}
	wr(gt + 0x941c, 1u << 3);
	bool ok = waitFor(gt + 0x941c, 1u << 3, 0, 5000, &g) && (rd(gt + GUC_STATUS) & 1);
	say("%sGuC reset %s (GDRST 0x%08x, GUC_STATUS 0x%08x)\n", gt ? "media " : "", ok ? "done" : "FAILED", g,
		rd(gt + GUC_STATUS));
	const uint32_t gi = gt ? 1 : 0;
	IOLockLock(gH2gLock[gi]);
	IOLockLock(gG2hLock[gi]);
	if (gt) mH2gTail = mG2hHead = 0;
	else h2gTail = g2hHead = 0;
	ctBroken[gi] = false;
	// a reset GuC answers no invalidation sent before it, and its TLBs start empty: count every one issued as done, so
	// what waits for one (xe_core.cpp deferReap) is freed and nobody waits for a reply that will not come
	if (gt) mTlbDone = tlbSeqno; else tlbDone = tlbSeqno;
	IOLockUnlock(gG2hLock[gi]);
	IOLockUnlock(gH2gLock[gi]);
	return ok;
}


// gt = MEDIA_GT_GSI_OFFSET loads the media GT's GuC the same way (xe_uc_load_hw on gt1): same firmware image and GGTT
// copy, its own registers, ADS (video classes only), misc pages and params (no hwconfig, no SLPC, no WA flags).

// guc_mmio_reg_state_init / guc_mmio_regset_write: for every engine, the registers the GuC saves before an engine reset
// and writes back after it. An engine reset (the GuC's reset of a hung context; on Xe2 RCS and the CCS engines share
// one reset domain) puts the engine's MMIO back to its power-on values: what is not in this list is lost, and stays
// lost until the next full GT reset or reboot. Before rb5 the list was empty: after the first CONTEXT_RESET of a CCS
// context every render/compute workaround below (LSC, ROW/TDL chicken bits, HALF_SLICE_CHICKEN7 clear optimization,
// RCU_MODE ...) and the engines' default state were gone for everyone, WindowServer included (2026-10-29 21:30 and
// 2026-10-30 02:08: dots in every surface rendered afterwards).
// Linux writes each engine's reg_sr (hw_engine_setup_default_state, the engine workarounds and tunings, render/compute
// ones on the first render-or-compute engine) and RING_HWS_PGA, RING_IMR and (Wa_16023105232) RING_IDLEDLY; masked
// registers are flagged MASKED, MCR ones steered to a non-terminated instance (xe_gt_mcr_get_nonterminated_steering:
// on Xe2 these offsets are DSS-steered to the first enabled DSS, 4 DSS per group). The values are the GuC's to fill.
// Only engine registers belong here: hwe->reg_sr (hw_engine_setup_default_state, engine_was, engine_tunings) plus the
// extra registers; gt_was / gt_tunings (GT-level, not reset with an engine) never are, and the GuC rejects a list that
// carries them (load status 0x74, INIT_MMIO_SAVE_RESTORE_INVALID). Checked entry by entry against xe_wa.c and
// xe_tuning.c: every entry below comes from engine_was, engine_tunings or the default state.
struct GucMmioReg { uint32_t offset, value, flags, mask; };
void IntelXe::adsRegsets(void *adsBlob, uint8_t *area, uint32_t areaG, uint32_t areaLen, uint32_t gt) {
	auto *ads = static_cast<guc_ads_blob *>(adsBlob);
	auto *out = reinterpret_cast<GucMmioReg *>(area);
	const uint32_t cap = areaLen / sizeof(GucMmioReg);
	uint32_t n = 0;
	// DSS steering target: the first DSS enabled for geometry or compute
	uint32_t dssMask = rd(XELP_GT_GEOMETRY_DSS_ENABLE) | rd(XEHP_GT_COMPUTE_DSS_ENABLE), dss = 0;
	while (dss < 32 && !(dssMask >> dss & 1)) dss++;
	if (dss == 32) dss = 0;
	const uint32_t steer = 1u << 1 | (dss / 4) << 12 | (dss % 4) << 20;   // STEERING_NEEDED, GROUP 16:12, INSTANCE 23:20
	enum : uint32_t { kMasked = 1u << 0 };
	struct R { uint32_t off, flags; };
	auto engine = [&](uint32_t gucClass, uint32_t phys, const R *regs, uint32_t nRegs) {
		if (gucClass >= GUC_MAX_ENGINE_CLASSES || phys >= GUC_MAX_INSTANCES_PER_CLASS || n + nRegs > cap) return;
		ads->ads.reg_state_list[gucClass][phys].address = areaG + n * (uint32_t)sizeof(GucMmioReg);
		ads->ads.reg_state_list[gucClass][phys].count = (uint16_t)nRegs;
		for (uint32_t i = 0; i < nRegs; i++) out[n++] = {regs[i].off, 0, regs[i].flags, 0};
	};
	if (!gt) {
		bool firstRC = true;
		for (uint32_t k = 0; k < gNumEngines; k++) {
			const XeEngine &e = gEngines[k];
			const uint32_t b = e.base;
			R r[32];
			uint32_t m = 0;
			// hw_engine_setup_default_state + engine workarounds on every engine (firstBatchSetupOnly's loop)
			r[m++] = {b + 0xc4, kMasked};    // RING_CMD_CCTL
			r[m++] = {b + 0x98, 0};          // RING_HWSTAM
			r[m++] = {b + 0x29c, kMasked};   // GFX_MODE
			r[m++] = {b + 0xd4, kMasked};    // CSFE_CHICKEN1 (CS_PRIORITY_MEM_READ, Wa_16021639441)
			r[m++] = {b + 0x50, kMasked};    // RING_PSMI_CTL (Wa_16023105232)
			r[m++] = {b + 0x80, 0};          // RING_HWS_PGA (extra_regs)
			r[m++] = {b + 0xa8, 0};          // RING_IMR (extra_regs)
			r[m++] = {b + 0x23c, 0};         // RING_IDLEDLY (Wa_16023105232)
			if (firstRC && (e.drmClass == 0 || e.drmClass == 4)) {   // xe_rtp_match_first_render_or_compute
				firstRC = false;
				r[m++] = {0x14800, kMasked};            // RCU_MODE: CCS enable
				if (gNumEngines > 3) r[m++] = {0x14804, kMasked};   // CCS_MODE (extra_regs: more than one CCS)
				r[m++] = {0xe7c8, steer};               // LSC_CHICKEN_BIT_0 (18032247524, 14019811474)
				r[m++] = {0xe7cc, steer};               // LSC_CHICKEN_BIT_0_UDW (16025250150, 16018712365, 14018471104)
				r[m++] = {0xe49c, steer | kMasked};     // ROW_CHICKEN3 (14020338487)
				r[m++] = {0xe4c4, steer | kMasked};     // TDL_TSL_CHICKEN (13012615864, 14021821874)
				r[m++] = {0xe48c, steer | kMasked};     // ROW_CHICKEN4 (18034896535)
				r[m++] = {0xe4f0, steer | kMasked};     // ROW_CHICKEN (16018737384)
				r[m++] = {0xe530, steer};               // RT_CTRL DIS_NULL_QUERY (tuning)
				if (dssGroupsDiscontiguous()) r[m++] = {0xe5f4, steer | kMasked};   // rb7p: TDL_CHICKEN (18041344222)
			}
			if (e.drmClass == 0) r[m++] = {0xe194, steer | kMasked};   // HALF_SLICE_CHICKEN7 (14021402888, render)
			if (e.drmClass == 0) r[m++] = {b + 0xe0, kMasked};        // rb7p: FF_SLICE_CS_CHICKEN1 (FtrPerCtxtPreemption...)
			if (gNonpriv) {   // rb7p: RING_FORCE_TO_NONPRIV slots (xe_reg_whitelist_process_engine puts them in reg_sr)
				uint32_t s[12];
				const uint32_t ns = engineNonpriv(e.drmClass, s);
				for (uint32_t i = 0; i < ns; i++) r[m++] = {b + 0x4d0 + 4 * i, 0};
			}
			engine(e.gucClass, e.phys, r, m);
		}
	} else {   // media: mediaGucLoad's engine programming
		static const uint32_t vcsBase[8] = {0x1c0000, 0x1c4000, 0x1d0000, 0x1d4000, 0x1e0000, 0x1e4000, 0x1f0000, 0x1f4000};
		static const uint32_t vecsBase[4] = {0x1c8000, 0x1d8000, 0x1e8000, 0x1f8000};
		for (uint32_t i = 0; i < 12; i++) {
			const bool vcs = i < 8;
			if (!((vcs ? mVdbox >> i : mVebox >> (i - 8)) & 1)) continue;
			const uint32_t b = vcs ? vcsBase[i] : vecsBase[i - 8];
			R r[24];
			uint32_t m = 0;
			r[m++] = {b + 0xc4, kMasked};
			r[m++] = {b + 0x98, 0};
			r[m++] = {b + 0x29c, kMasked};
			r[m++] = {b + 0xd4, kMasked};
			r[m++] = {b + 0x50, kMasked};
			r[m++] = {b + 0x80, 0};
			r[m++] = {b + 0xa8, 0};
			r[m++] = {b + 0x23c, 0};
			if (gNonpriv) {   // rb7p: RING_FORCE_TO_NONPRIV slots (OAM, DENY)
				uint32_t s[12];
				const uint32_t ns = engineNonpriv(vcs ? 2 : 3, s);
				for (uint32_t k = 0; k < ns; k++) r[m++] = {b + 0x4d0 + 4 * k, 0};
			}
			// Not VDBOX_CGCTL3F1C / 3F08 (Wa_16021867713, Wa_14019449301): xe_wa.c has them in gt_was (FOREACH_ENGINE),
			// so they are in the GT's reg_sr, never in an engine's save/restore list; listed here (rb5/rb6) the media
			// GuC refused the whole ADS at load (GUC_STATUS ukernel 0x74, INIT_MMIO_SAVE_RESTORE_INVALID): no video.
			engine(vcs ? 1 : 2, vcs ? i : i - 8, r, m);
		}
	}
	say("%sADS register save/restore lists: %u entries (DSS steering %u/%u)\n", gt ? "media " : "", n, dss / 4, dss % 4);
}

int IntelXe::loadGuc(bool full, uint32_t gt) {
	if (!mapBar0()) return ENXIO;
	const uc_css_header *css = reinterpret_cast<const uc_css_header *>(kGucFw);
	uint32_t ucode = (css->size_dw - css->header_size_dw) * 4, rsa = css->key_size_dw * 4;
	uint32_t upload = sizeof(uc_css_header) + ucode;
	say("firmware %u.%u.%u, %u bytes: ucode %u, rsa %u, private data 0x%x\n", (css->sw_version >> 16) & 0xff,
		(css->sw_version >> 8) & 0xff, css->sw_version & 0xff, kGucFwSize, ucode, rsa, css->private_data_size);
	if ((css->header_size_dw - css->key_size_dw - css->modulus_size_dw - css->exponent_size_dw) * 4 != sizeof(uc_css_header) ||
		kGucFwSize < upload + rsa) {
		say("bad CSS header\n");
		return EINVAL;
	}
	if (!forcewake(true, gt)) return ETIMEDOUT;
	uint32_t s = rd(gt + GUC_STATUS);
	if (!(s & 1) && !gucReset(gt)) {
		forcewake(false, gt);
		return EIO;
	}

	programPatMocs(gt);

	// xe_wopcm.c xe_wopcm_size (8 MB from Lunar Lake on); no HuC, so GuC starts at ALIGN(16K, 16K).
	const uint32_t wopcmSize = gXePlat->wopcmBytes, ctxRsvd = 36u << 10;
	uint32_t base, size, rs = rd(gt + GUC_WOPCM_SIZE), ro = rd(gt + DMA_GUC_WOPCM_OFFSET);
	if ((rs & 1) && (ro & 1)) {
		base = ro & 0xffffc000;
		size = rs & 0xfffff000;
		say("WOPCM already locked: base 0x%x size 0x%x\n", base, size);
	} else {
		// Media: the HuC sits at the WOPCM bottom, GuC from ALIGN(HuC + WOPCM_RESERVED_SIZE 16K, 16K), and
		// HUC_LOADING_AGENT_GUC (bit 1) in DMA_GUC_WOPCM_OFFSET (__wopcm_init_regs).
		const uint32_t hucAgent = gt ? 1u << 1 : 0;
		base = gt ? (kHucFwSize + (16u << 10) + 0x3fff) & ~0x3fffu : 16u << 10;
		size = (wopcmSize - ctxRsvd - base) & 0xfffff000;
		wr(gt + GUC_WOPCM_SIZE, size);
		wr(gt + DMA_GUC_WOPCM_OFFSET, base | hucAgent);
		rs = rd(gt + GUC_WOPCM_SIZE);
		ro = rd(gt + DMA_GUC_WOPCM_OFFSET);
		say("WOPCM base 0x%x size 0x%x -> regs 0x%08x 0x%08x\n", base, size, rs, ro);
		if ((rs & 0xfffff001) != (size | 1) || (ro & (0xffffc001 | hucAgent)) != (base | 1 | hucAgent)) {
			say("WOPCM registers did not take\n");
			forcewake(false, gt);
			return EIO;
		}
	}
	if (size < upload + (24u << 10)) {
		say("WOPCM too small for GuC\n");
		forcewake(false, gt);
		return E2BIG;
	}

	// ADS: the blob, then the engines' register save/restore lists (one page), then the GuC's private data
	const size_t regsetOff = round_page(sizeof(guc_ads_blob)), privOff = regsetOff + 0x1000;
	IOBufferMemoryDescriptor *&adsM = gt ? mAdsMem : adsMem, *&miscM = gt ? mMiscMem : miscMem;
	const uint32_t aG = gt ? mAdsG : adsG;
	if (!fwMem) fwMem = alloc(kGucFwSize, true);
	if (!adsM) adsM = alloc(privOff + round_page(css->private_data_size), false);
	if (!miscM) miscM = alloc(gt ? mMiscSize : miscSize, false);
	if (!fwMem || !adsM || !miscM) {
		say("allocation failed\n");
		forcewake(false, gt);
		return ENOMEM;
	}
	memcpy(fwMem->getBytesNoCopy(), kGucFw, kGucFwSize);
	flushRange(fwMem->getBytesNoCopy(), kGucFwSize);

	// xe_guc_ads_populate_minimal: zeroed blob, policies, an all-invalid engine mapping table, three pointers.
	// full: as xe_guc_ads_populate for the one engine we use, BCS0: engine mask, mapping table entry and a zeroed
	// golden context of the class's LRC size (Linux fills it only after recording the default LRCs).
	auto *ads = reinterpret_cast<guc_ads_blob *>(adsM->getBytesNoCopy());
	bzero(adsM->getBytesNoCopy(), adsM->getLength());
	ads->policies.dpc_promote_time = GLOBAL_POLICY_DEFAULT_DPC_PROMOTE_TIME_US;
	ads->policies.max_num_work_items = GLOBAL_POLICY_MAX_NUM_WI;
	ads->policies.is_valid = 1;
	memset(ads->system_info.mapping_table, GUC_MAX_INSTANCES_PER_CLASS, sizeof(ads->system_info.mapping_table));
	if (full && gt) {
		// Media: GUC_VIDEO_CLASS (1) and GUC_VIDEOENHANCE_CLASS (2) with the fused-in physical instances, logical ids
		// in instance order (xe_hw_engine setup_logical_ids), golden contexts of PPHWSP + 1 page (xe_gt_lrc_size), so
		// eng_state_size = 4 KB - xe_lrc_reg_size (96 dwords).
		const uint32_t masks[3] = {0, mVdbox, mVebox};
		for (uint32_t c = 1; c <= 2; c++) {
			ads->system_info.engine_enabled_masks[c] = masks[c];
			for (uint32_t i = 0, l = 0; i < 8; i++)
				if (masks[c] & 1u << i) ads->system_info.mapping_table[c][l++] = (uint8_t)i;
			ads->ads.golden_context_lrca[c] = mMiscG + (c == 1 ? mGoldenVcsOff : mGoldenVecsOff);
			ads->ads.eng_state_size[c] = 0x2000 - 0x1000 - 96 * 4;
		}
	} else if (full) {
		// engine_enable_mask (physical instances), guc_mapping_table_init (logical -> physical), guc_golden_lrc_init
		// (eng_state_size = hang replay size - xe_lrc_reg_size).
		for (uint32_t i = 0; i < gNumEngines; i++) {
			const XeEngine &e = gEngines[i];
			ads->system_info.engine_enabled_masks[e.gucClass] |= 1u << e.phys;
			ads->system_info.mapping_table[e.gucClass][e.logical] = (uint8_t)e.phys;
			ads->ads.golden_context_lrca[e.gucClass] =
				miscG + (e.gucClass == 0 ? goldenRcsOff : e.gucClass == 4 ? goldenCcsOff : goldenOff);
			ads->ads.eng_state_size[e.gucClass] = e.lrcSize - 0x1000 - 96 * 4;
		}
	}
	ads->ads.scheduler_policies = aG + offsetof(guc_ads_blob, policies);
	ads->ads.gt_system_info = aG + offsetof(guc_ads_blob, system_info);
	ads->ads.private_data = aG + (uint32_t)privOff;
	if (full) {   // guc_waklv_init for graphics 20.01: 18024947630, 16022287689, 14022293748/22019794406, 16026508708
		static const uint32_t klvs[] = {0x9005u << 16, 0x9007u << 16, 0x9009u << 16, 0x900bu << 16};
		// media 13.01: 16026508708 (RESET_BB_STACK_PTR_ON_VF_SWITCH), 14025515070 (CLR_CS_INDIRECT_RING_STATE, fw >= 70.53)
		static const uint32_t mklvs[] = {0x900bu << 16, 0x900eu << 16};
		const uint32_t off = (sizeof(guc_ads_blob) + 63) & ~63u;
		memcpy(static_cast<uint8_t *>(adsM->getBytesNoCopy()) + off, gt ? mklvs : klvs, gt ? sizeof(mklvs) : sizeof(klvs));
		ads->ads.wa_klv_addr_lo = aG + off;
		ads->ads.wa_klv_size = gt ? sizeof(mklvs) : sizeof(klvs);
	}
	if (full) adsRegsets(ads, static_cast<uint8_t *>(adsM->getBytesNoCopy()) + regsetOff, aG + (uint32_t)regsetOff, 0x1000, gt);
	// rb7j: guc_um_init_params (has_usm, primary GT): the three UM queues, by DPA (pre-Xe3p: the GAM writes them through
	// base_dpa, xe_guc_using_main_gamctrl_queues false) and GGTT, 64 KB each, page_response_timeout_in_us = -1
	// (GUC_PAGE_RES_TIMEOUT_US). The media GT gets none here (Linux sets them on both GTs; a media fault stays a CAT
	// error), so the media GuC's ADS is as before.
	const bool um = full && !gt && gUsm;
	if (um) {
		if (!gUmDpa) gUmDpa = umDpaFor(vramUsableEnd());
		if (gUmDpa) {
			for (uint32_t i = 0; i < GUC_UM_HW_QUEUE_MAX; i++) {
				ads->um_init_params.queue_params[i].base_dpa = gUmDpa + i * kUmQueueBytes;
				ads->um_init_params.queue_params[i].base_ggtt_address = umG + i * kUmQueueBytes;
				ads->um_init_params.queue_params[i].size_in_bytes = kUmQueueBytes;
			}
			ads->um_init_params.page_response_timeout_in_us = ~0ull;
			ads->ads.um_init_data = aG + offsetof(guc_ads_blob, um_init_params);
		}
	}
	flushRange(adsM->getBytesNoCopy(), adsM->getLength());

	int err = ggttMap(fwMem, fwG, gt);
	if (!err) err = ggttMap(adsM, aG, gt);
	if (!err) err = ggttMap(miscM, gt ? mMiscG : miscG, gt);
	if (!err && um && gUmDpa) err = ggttMapVram(gUmDpa, kUmBytes, umG, 0, true);   // XE_BO_FLAG_NEEDS_UC
	if (err) {
		forcewake(false, gt);
		return err;
	}
	say("GGTT: firmware 0x%x (%llu KB), ADS 0x%x (%llu KB, private data at +0x%zx)\n", fwG,
		(uint64_t)fwMem->getLength() >> 10, aG, (uint64_t)adsM->getLength() >> 10, privOff);

	// guc_init_params with logging off: LOG_PARAMS 0, WA 0, FEATURE 0, DEBUG = LOG_DISABLED, ADS, DEVID.
	uint32_t params[GUC_CTL_MAX_DWORDS] = {};
	params[GUC_CTL_DEBUG] = GUC_LOG_DISABLED;
	if (full && gt) {   // guc_ctl_feature_flags (SLPC unless debug.intelxe.media_slpc=0); guc_ctl_wa_flags: none on 13.01
		params[GUC_CTL_FEATURE] = 1u << 4 | (gMediaSlpc ? 1u << 2 : 0);   // LITE_RESTORE, ENABLE_SLPC
	} else if (full) {   // guc_init_params_post_hwconfig: feature flags; DUAL_QUEUE WA with CCS on >= 12.70
		params[GUC_CTL_FEATURE] = 1u << 4 | 1u << 2;   // LITE_RESTORE, ENABLE_SLPC
		params[GUC_CTL_WA] = 1u << 11;
	}
	// xe_uc_load_hw: xe_huc_upload before xe_guc_upload (media GT only; a failed HuC DMA does not stop the GuC)
	if (full && gt) hucUpload();
	params[GUC_CTL_ADS] = (aG >> 12) << 1;
	params[GUC_CTL_DEVID] = (uint32_t)pci->configRead16(kIOPCIConfigDeviceID) << 16 | pci->configRead8(kIOPCIConfigRevisionID);
	wr(gt + SOFT_SCRATCH0, 0);
	for (int i = 0; i < GUC_CTL_MAX_DWORDS; i++) wr(gt + SOFT_SCRATCH0 + 4 * (1 + i), params[i]);

	// guc_prepare_xfer (graphics 20: no SRAM-init/MIA-caching bits, GUC_MOCS_INDEX = the MOCS uc_index); GT_PM_CONFIG is
	// above 0x40000, so the media GuC writes the same register.
	wr(gt + GUC_SHIM_CONTROL, 1u << 1 | 1u << 9 | 1u << 10 | 1u << 15 | (uint32_t)gXePlat->mocsUcIndex << 24);
	wr(GT_PM_CONFIG, 1);
	wr(gt + PMINTRMSK, rd(gt + PMINTRMSK) & ~(1u << 9));

	// guc_xfer_rsa: keys over 256 bytes go by GGTT address
	if (rsa > 256) {
		wr(gt + UOS_RSA_SCRATCH0, fwG + upload);
	} else {
		const uint32_t *r = reinterpret_cast<const uint32_t *>(kGucFw + upload);
		for (uint32_t i = 0; i < 64; i++) wr(gt + UOS_RSA_SCRATCH0 + 4 * i, i < rsa / 4 ? r[i] : 0);
	}

	// uc_fw_xfer: CSS header + ucode, GGTT -> WOPCM 0x2000, UOS_MOVE
	wr(gt + DMA_ADDR_0_LOW, fwG);
	wr(gt + DMA_ADDR_0_HIGH, 8u << 16);
	wr(gt + DMA_ADDR_1_LOW, 0x2000);
	wr(gt + DMA_ADDR_1_HIGH, 7u << 16);
	wr(gt + DMA_COPY_SIZE, upload);
	wr(gt + DMA_CTRL, (1u << 4 | 1u) << 16 | 1u << 4 | 1u);
	uint32_t dma;
	bool dmaOk = waitFor(gt + DMA_CTRL, 1, 0, 100000, &dma);
	wr(gt + DMA_CTRL, (1u << 4) << 16);
	say("DMA %s (DMA_CTRL 0x%08x)\n", dmaOk ? "done" : "TIMEOUT", dma);
	if (!dmaOk) {
		gucStatus(gt);
		forcewake(false, gt);
		return ETIMEDOUT;
	}

	// guc_wait_ucode: ukernel READY 0xF0, or one of the error codes, within 3 s
	static const uint8_t ukErr[] = {0x02, 0x03, 0x04, 0x07, 0x08, 0x60, 0x70, 0x71, 0x73, 0x74, 0x75, 0x76};
	static const uint8_t brErr[] = {0x13, 0x50, 0x73, 0x74, 0x75, 0x77, 0x79, 0x7a, 0x7e, 0x2b};
	uint32_t prev = ~0u, ms = 0, result = 0;
	for (; ms <= 3000 && !result; ms += 10) {
		s = rd(gt + GUC_STATUS);
		uint8_t uk = (s >> 8) & 0xff, br = (s >> 1) & 0x7f;
		if (s != prev) say("  %4u ms GUC_STATUS 0x%08x (ukernel 0x%02x bootrom 0x%02x)\n", ms, s, uk, br);
		prev = s;
		if (uk == 0xf0) result = 1;
		for (uint8_t e : ukErr) if (uk == e) result = 2;
		for (uint8_t e : brErr) if (br == e) result = 2;
		if (!result) IOSleep(10);
	}
	say("%sGuC %s after %u ms\n", gt ? "media " : "", result == 1 ? "READY" : result == 2 ? "load FAILED" : "TIMEOUT", ms);
	gucStatus(gt);
	forcewake(false, gt);
	return result == 1 ? 0 : EPROTO;
}

// xe_guc_mmio_send_recv: request in VF_SW_FLAG(0..3), notify through GUC_HOST_INTERRUPT, reply in VF_SW_FLAG(0)
// (ORIGIN = GuC; type 7 success with DATA0 27:0, 6 failure, 3 busy, 5 retry). The media GuC has its own
// MED_VF_SW_FLAG(n) and MED_GUC_HOST_INTERRUPT (xe_guc_mmio_send_recv, xe_guc_comm_init_early).
static constexpr uint32_t VF_SW_FLAG0 = 0x190240, GUC_HOST_INTERRUPT = 0x1901f0;

int IntelXe::mmioSend(const uint32_t *req, uint32_t len, uint32_t *reply, uint32_t gt) {
	const uint32_t flag0 = gt ? MED_VF_SW_FLAG0 : VF_SW_FLAG0;
	for (uint32_t i = 0; i < len; i++) wr(flag0 + 4 * i, req[i]);
	(void)rd(flag0 + 12);
	wr(gt ? MED_GUC_HOST_INTERRUPT : GUC_HOST_INTERRUPT, 0);
	uint32_t h;
	if (!waitFor(flag0, 1u << 31, 1u << 31, 50000, &h)) {
		say("MMIO 0x%04x: no reply (0x%08x)\n", req[0] & 0xffff, h);
		return ETIMEDOUT;
	}
	if (((h >> 28) & 7) == 3) waitFor(flag0, 6u << 28, 6u << 28, 2000000, &h);
	uint32_t type = (h >> 28) & 7;
	if (type == 7) {
		if (reply) *reply = h & 0x0fffffff;
		return 0;
	}
	say("MMIO 0x%04x: %s 0x%08x\n", req[0] & 0xffff, type == 6 ? "failure" : type == 5 ? "retry" : "unexpected reply", h);
	return EIO;
}

// CT buffers (xe_guc_ct.c): one allocation, H2G descriptor at 0, G2H descriptor at 2 KB, H2G ring 4 KB at 4 KB,
// G2H ring 128 KB at 8 KB. Registered with HOST2GUC_SELF_CFG KLVs over MMIO, then HOST2GUC_CONTROL_CTB enable.
static constexpr uint32_t h2gDwords = kH2gBytes / 4, g2hDwords = 0x8000;

int IntelXe::ctSetup(uint32_t gt) {
	IOBufferMemoryDescriptor *&mem = gt ? mCtMem : ctMem;
	const uint32_t cG = gt ? mCtG : ctG;
	if (!mem) mem = alloc(0x2000 + g2hDwords * 4, false);
	if (!mem) return ENOMEM;
	const uint32_t gi = gt ? 1 : 0;
	IOLockLock(gH2gLock[gi]);
	IOLockLock(gG2hLock[gi]);
	bzero(mem->getBytesNoCopy(), mem->getLength());
	flushRange(mem->getBytesNoCopy(), mem->getLength());
	(gt ? mH2gTail : h2gTail) = 0;
	(gt ? mG2hHead : g2hHead) = 0;
	ctBroken[gi] = false;
	h2gDead[gi] = false;
	IOLockUnlock(gG2hLock[gi]);
	IOLockUnlock(gH2gLock[gi]);
	int err = ggttMap(mem, cG, gt);
	if (err) return err;
	const struct { uint16_t key, len; uint32_t val; } kl[] = {
		{0x0903, 2, cG}, {0x0902, 2, cG + 0x1000}, {0x0904, 1, h2gDwords * 4},
		{0x0906, 2, cG + 0x800}, {0x0905, 2, cG + 0x2000}, {0x0907, 1, g2hDwords * 4},
	};
	for (auto &k : kl) {
		uint32_t req[4] = {0x0508, (uint32_t)k.key << 16 | k.len, k.val, 0}, n = 0;
		if ((err = mmioSend(req, 4, &n, gt))) return err;
		if (n != 1) {
			say("self cfg 0x%04x: GuC took %u KLVs\n", k.key, n);
			return EPROTO;
		}
	}
	uint32_t ctl[2] = {0x4509, 1};
	if ((err = mmioSend(ctl, 2, nullptr, gt))) return err;
	say("%sCT enabled: H2G 0x%x (+0x1000), G2H 0x%x (+0x2000)\n", gt ? "media " : "", cG, cG + 0x800);
	return 0;
}

// h2g_write as a FAST_REQUEST: CT header (fence 31:16, format HXG, length), HXG header, data; then the descriptor
// tail and a GuC notify. A message that would cross the end of the ring is preceded by zero dwords up to the end and
// the tail wraps to 0, as h2g_write does. Before writing, the GuC must have freed enough space (its head): polled for
// 1 ms, then in 1 ms sleeps (guc_ct_send_locked backs off with msleep) for up to 1 s, with this GuC's H2G lock held.
// Still full: the message is dropped and ETIMEDOUT returned, rather than overwriting what the GuC has not read.
int IntelXe::ctSend(const uint32_t *action, uint32_t len, uint32_t gt, bool request) {
	const uint32_t gi = gt ? 1 : 0;
	IOBufferMemoryDescriptor *mem = gt ? mCtMem : ctMem;
	if (!mem) return ENXIO;
	IOLockLock(gH2gLock[gi]);
	if (h2gDead[gi]) {   // until the next CT setup (ctSetup)
		IOLockUnlock(gH2gLock[gi]);
		return EPIPE;
	}
	uint32_t &tail = gt ? mH2gTail : h2gTail, &fen = gt ? mFence : fence;
	auto *base = static_cast<uint8_t *>(mem->getBytesNoCopy());
	auto *desc = reinterpret_cast<volatile uint32_t *>(base);
	auto *cmds = reinterpret_cast<uint32_t *>(base + 0x1000);
	const uint32_t full = len + 1;
	auto space = [&]() {
		flushRange(base, 64);
		return (desc[0] + h2gDwords - tail - 1) % h2gDwords;   // CIRC_SPACE(tail, head, size)
	};
	auto waitSpace = [&](uint32_t need) {
		for (uint32_t i = 0; i < 1100 && space() < need; i++) {
			if (i < 100) IODelay(10);
			else IOSleep(1);
		}
		if (space() >= need) return true;
		gStats.h2gFull++;
		LOG("H2G%s full (head %u tail %u status 0x%x): action 0x%04x dropped", gt ? " media" : "", desc[0], tail, desc[2],
			action[0] & 0xffff);
		// guc_ct_send_locked: no forward progress for 1 s means a dead channel (CT_DEAD(DEADLOCK)); every later send
		// fails at once instead of holding the H2G lock another second, and the primary GT is reset (kick_reset)
		if (!h2gDead[gi]) {
			h2gDead[gi] = true;
			gStats.h2gDead++;
			if (!gi) xeRequestGtResetFor("H2G made no forward progress");
		}
		return false;
	};
	if (tail + full > h2gDwords) {
		if (!waitSpace(h2gDwords - tail)) {
			IOLockUnlock(gH2gLock[gi]);
			return ETIMEDOUT;
		}
		bzero(cmds + tail, (h2gDwords - tail) * 4);
		flushRange(cmds + tail, (h2gDwords - tail) * 4);
		tail = 0;
		desc[1] = 0;
		flushRange(base, 64);
	}
	if (!waitSpace(full)) {
		IOLockUnlock(gH2gLock[gi]);
		return ETIMEDOUT;
	}
	uint32_t t = tail;
	const uint32_t f = ++fen & 0xffff;
	cmds[t] = f << 16 | len;
	cmds[t + 1] = (request ? 0u : 2u) << 28 | action[0];   // HXG REQUEST (reply in G2H) or FAST_REQUEST
	for (uint32_t i = 1; i < len; i++) cmds[t + 1 + i] = action[i];
	flushRange(cmds + t, full * 4);
	tail = (t + full) % h2gDwords;
	desc[1] = tail;
	flushRange(base, 64);
	wr(gt ? MED_GUC_HOST_INTERRUPT : GUC_HOST_INTERRUPT, 0);
	H2gHistory &h = gH2gHist[gi];
	h.fence[h.next % 64] = (uint16_t)f;
	h.action[h.next % 64] = (uint16_t)action[0];
	h.next++;
	IOLockUnlock(gH2gLock[gi]);
	return 0;
}

void IntelXe::ctDrain(uint32_t gt) {
	IOLockLock(gG2hLock[gt ? 1 : 0]);
	uint32_t &head = gt ? mG2hHead : g2hHead;
	auto *base = static_cast<uint8_t *>((gt ? mCtMem : ctMem)->getBytesNoCopy());
	auto *h2g = reinterpret_cast<volatile uint32_t *>(base);
	auto *g2h = reinterpret_cast<volatile uint32_t *>(base + 0x800);
	auto *cmds = reinterpret_cast<volatile uint32_t *>(base + 0x2000);
	flushRange(base, 0x1000);
	say("H2G head %u tail %u status 0x%x; G2H head %u tail %u status 0x%x\n", h2g[0], h2g[1], h2g[2], g2h[0], g2h[1], g2h[2]);
	uint32_t tail = g2h[1], shown = 0;
	while (head != tail && tail < g2hDwords) {
		flushRing(cmds, head, g2hDwords);
		uint32_t hdr = cmds[head], n = hdr & 0xff;
		if (shown++ < 12)
			say("  G2H fence %u len %u: %08x %08x %08x %08x\n", hdr >> 16, n, cmds[(head + 1) % g2hDwords],
				cmds[(head + 2) % g2hDwords], cmds[(head + 3) % g2hDwords], cmds[(head + 4) % g2hDwords]);
		head = (head + n + 1) % g2hDwords;
	}
	g2h[0] = head;
	flushRange(base + 0x800, 64);
	IOLockUnlock(gG2hLock[gt ? 1 : 0]);
}

// ─── rb7p: per-engine state of Linux xe's hw_engine_init that rb7o lacked ──────────────────────────────────────────
// Diffed against torvalds/linux 6628e7d4 (2026-11-01; drm-xe-next has the same xe_hw_engine.c adjust_idledly,
// xe_wa.c, xe_reg_whitelist.c and xe_guc_ads.c). Linux keeps the register part in hwe->reg_sr and writes it at
// hw_engine_init (xe_hw_engine.c:698) and in do_gt_restart (xe_gt.c:906: resume and GT reset), and lists every entry
// in the GuC's engine-reset save/restore list (xe_guc_ads.c guc_mmio_regset_write): here engineExtras() runs where
// the rest of the engine state is written (firstBatchSetupOnly / mediaGucLoad: bring-up, resume, GT reset) and
// adsRegsets() lists the same registers. adjust_idledly is not reg_sr: Linux calls it from hw_engine_init only
// (xe_hw_engine.c:724-725; probe), never from do_gt_restart; a GuC engine reset restores RING_IDLEDLY from the save
// list (xe_guc_ads.c:874-877). Here: once per GT, at its first engine bring-up (gIdledlyDone), never on resume or GT
// reset; debug.intelxe.step=22 runs it again by hand.
int gNonpriv = 1;
struct IdledlyRec {
	char name[6];
	uint32_t base, before, after, maxcnt, tsBase;
	bool written, warned;
};
static IdledlyRec gIdledly[2][12];   // [0] primary GT, [1] media GT: what the probe-time adjust_idledly found and did
static uint32_t gIdledlyN[2];
static bool gIdledlyDone[2];

// xe_reg_whitelist.c (register_whitelist, oa_whitelist; whitelist_apply_to_hwe): the RING_FORCE_TO_NONPRIV values of
// one engine in slot order. Each table is an xarray keyed by register address, so its slots go in address order;
// register_whitelist first (slot 0 up), then oa_whitelist from the next slot. Graphics 20.01 / media 13.01 matches:
//   14024997852 (xe_reg_whitelist.c:93, GRAPHICS_VERSION_RANGE(2001, 3005), RENDER): FF_MODE 0x6210 and VFLSKPD
//     0x62a8, RW (Mesa's Wa_14024997852 autostrip toggling writes them from user batches: anv genX_gfx_state.c,
//     iris_state.c, mi_builder.h mi_set_autostrip_state)
//   oag_mmio_trg_rcs / _ccs (:137/:142, graphics >= 12.00, RENDER / COMPUTE): OAG_OASTATUS 0xdafc RD, OAG_OAHEADPTR
//     0xdb00 RD|RANGE_4, OAG_MMIOTRIGGER 0xdb1c RW, all with DENY (xe_oa opens them only while an OA stream runs)
//   oam_mmio_trg_vcs / _vecs (:147/:152, media >= 13.00): OAM STATUS +0x19c RD, HEAD_POINTER +0x1a0 RD|RANGE_4,
//     MMIO_TRG +0x1d0 RW, DENY, for the SAG (0x13000) and SCMI 0/1 (0x14000, 0x14800) units at MEDIA_GT_GSI_OFFSET
//   (BCS: none; has_mert is not set for BMG, xe_pci.c bmg_desc)
uint32_t IntelXe::engineNonpriv(uint16_t drmClass, uint32_t *s) {
	constexpr uint32_t RD = 1u << 28, DENY = 1u << 30, RANGE_4 = 1u;   // ACCESS_RW is 0
	uint32_t n = 0;
	if (drmClass == 0) {
		s[n++] = 0x6210;   // FF_MODE (Wa_14024997852)
		s[n++] = 0x62a8;   // VFLSKPD (Wa_14024997852)
	}
	if (drmClass == 0 || drmClass == 4) {
		s[n++] = 0xdafc | RD | DENY;              // OAG_OASTATUS
		s[n++] = 0xdb00 | RD | RANGE_4 | DENY;    // OAG_OAHEADPTR
		s[n++] = 0xdb1c | DENY;                   // OAG_MMIOTRIGGER
	}
	if (drmClass == 2 || drmClass == 3) {
		static const uint32_t oam[3] = {0x393000, 0x394000, 0x394800};   // XE_OAM_{SAG,SCMI_0,SCMI_1}_BASE_ADJ
		for (uint32_t u = 0; u < 3; u++) {
			s[n++] = (oam[u] + 0x19c) | RD | DENY;             // OAM_STATUS
			s[n++] = (oam[u] + 0x1a0) | RD | RANGE_4 | DENY;   // OAM_HEAD_POINTER
			s[n++] = (oam[u] + 0x1d0) | DENY;                  // OAM_MMIO_TRG
		}
	}
	return n;
}

// The reg_sr entries rb7o did not write: RING_FORCE_TO_NONPRIV(base, slot) = base + 0x4d0 + 4 * slot (plain writes,
// clr_bits ~0), and for the render engine FtrPerCtxtPreemptionGranularityControl (xe_wa.c:345, engine_was,
// GRAPHICS_VERSION_RANGE(2000, ...), RENDER): FF_SLICE_CS_CHICKEN1(RENDER_RING_BASE) 0x20e0 FFSC_PERCTX_PREEMPT_CTRL
// bit 14, masked. Forcewake for the engine's GT (and its media power well) held by the caller.
void IntelXe::engineExtras(uint32_t base, uint16_t drmClass) {
	if (gNonpriv) {
		uint32_t s[12];
		const uint32_t n = engineNonpriv(drmClass, s);
		for (uint32_t i = 0; i < n; i++) wr(base + 0x4d0 + 4 * i, s[i]);
	}
	if (drmClass == 0) wr(base + 0xe0, (1u << 14) << 16 | 1u << 14);   // FF_SLICE_CS_CHICKEN1: FFSC_PERCTX_PREEMPT_CTRL
}

// xe_gt_has_discontiguous_dss_groups (xe_gt_topology.c:337): walking the DSS enabled for geometry or compute
// (xe_gt_topology_init: XELP_GT_GEOMETRY_DSS_ENABLE, XE2_GT_GEOMETRY_DSS_1/2; XEHP_GT_COMPUTE_DSS_ENABLE,
// XEHPC_GT_COMPUTE_DSS_ENABLE_EXT, XE2_GT_COMPUTE_DSS_2; 32 DSS per register), true when a steering group is skipped
// (the first group counts from -1, so a fused-off group 0 is a gap too). Group size: xe_gt_mcr.c dss_per_group, from
// the hwconfig table (MAX_SUBSLICES 70 / MAX_SLICES 1, rounded up) once the GuC gave it, else 4 (graphics >= 12.50).
// Primary GT forcewake held.
bool IntelXe::dssGroupsDiscontiguous(uint32_t *perGroup) {
	uint32_t per = 4, slices = 0, subslices = 0;
	const uint32_t *h = reinterpret_cast<const uint32_t *>(gHwconfig);
	for (uint32_t i = 0; i + 2 <= gHwconfigLen / 4;) {   // KLVs: key, length in dwords, value
		const uint32_t len = h[i + 1];
		if (len >= 1 && i + 2 + len <= gHwconfigLen / 4) {
			if (h[i] == 1) slices = h[i + 2];
			if (h[i] == 70) subslices = h[i + 2];
		}
		i += 2 + len;
	}
	if (slices && subslices) per = (subslices + slices - 1) / slices;
	if (perGroup) *perGroup = per;
	const uint32_t g[3] = {rd(XELP_GT_GEOMETRY_DSS_ENABLE), rd(XE2_GT_GEOMETRY_DSS_1), rd(XE2_GT_GEOMETRY_DSS_2)};
	const uint32_t c[3] = {rd(XEHP_GT_COMPUTE_DSS_ENABLE), rd(XEHPC_GT_COMPUTE_DSS_ENABLE_EXT), rd(XE2_GT_COMPUTE_DSS_2)};
	int last = -1;
	for (uint32_t dss = 0; dss < 96; dss++) {
		if (!((g[dss / 32] | c[dss / 32]) >> (dss % 32) & 1)) continue;
		const int group = (int)(dss / per);
		if (group != last) {
			if (group - last > 1) return true;
			last = group;
		}
	}
	return false;
}

// xe_hw_engine.c adjust_idledly (lines 588-688), for one engine. Both workarounds match BMG on both GTs
// (xe_wa_oob.rules:47-48 Wa_16023105232: GRAPHICS_VERSION_RANGE(2001, 3005) / MEDIA_VERSION_RANGE(1301, 3002);
// :76-77 Wa_14025941587: GRAPHICS_VERSION_RANGE(2001, 3511) / MEDIA_VERSION_RANGE(1301, 3503), not an SR-IOV VF):
//   RING_IDLEDLY (base + 0x23c): INHIBIT_SWITCH_UNTIL_PREEMPTED bit 31, IDLE_DELAY 20:0 in ticks of
//     8 * timestamp_base ps; RING_PWRCTX_MAXCNT (base + 0x54): IDLE_WAIT_TIME 19:0 in 640 ns units
//   timestamp_base (xe_gt_clock.c read_crystal_clock, RPM_CONFIG0 of the GT, CRYSTAL_CLOCK_FREQ 5:3): 24 MHz 83333,
//     19.2 / 38.4 MHz 52083, 25 MHz 80000 (ps); anything else 0, and adjust_idledly returns at once
//   Wa_14025941587: a delay below 5000 ns becomes 5000 ns (rounded up to whole ticks)
//   Wa_16023105232: INHIBIT_SWITCH_UNTIL_PREEMPTED cleared; a delay at or above the wait time becomes wait time - 1 ns
//     (floored to ticks), and so does a rounded-up 5 us that would reach it; a value read at or above the wait time
//     is warned about (xe_gt_WARN_ON)
// The register is written only when one of them changed something. Forcewake for the engine held by the caller.
void IntelXe::adjustIdledly(uint32_t gt, uint32_t base, const char *name, uint32_t inst, bool record) {
	constexpr bool wa16023105232 = true, wa14025941587 = true;
	static const uint32_t tsBases[8] = {83333, 52083, 52083, 80000, 0, 0, 0, 0};
	const uint32_t tsBase = tsBases[(rd(gt + 0xd00) >> 3) & 7];   // RPM_CONFIG0 (xe_gt_clock_init, per GT)
	const uint32_t unitsPs = 8 * tsBase, maxcntUnitsNs = 640, mincntNs = 5000;
	IdledlyRec r = {};
	snprintf(r.name, sizeof(r.name), "%s%u", name, inst);
	r.base = base;
	r.tsBase = tsBase;
	uint32_t reg = rd(base + 0x23c);
	const uint32_t maxRaw = rd(base + 0x54);
	r.before = r.after = reg;
	r.maxcnt = maxRaw;
	if (unitsPs) {
		const bool inhibit = reg >> 31 & 1;
		uint32_t idl = (uint32_t)(((uint64_t)(reg & 0x1fffff) * unitsPs + 500) / 1000);   // DIV_ROUND_CLOSEST_ULL
		const uint32_t idlHw = idl, maxcnt = (maxRaw & 0xfffff) * maxcntUnitsNs;
		bool applied = false, clamped = false;
		if (wa14025941587 && idl < mincntNs) {
			idl = mincntNs;
			applied = true;
		}
		if (wa16023105232) {
			if (inhibit) {
				reg &= ~(1u << 31);
				applied = true;
			}
			r.warned = idlHw >= maxcnt;
			if (idl >= maxcnt) {
				idl = maxcnt ? maxcnt - 1 : 0;
				clamped = applied = true;
			}
		}
		if (applied) {
			uint32_t ticks = clamped ? (uint32_t)((uint64_t)idl * 1000 / unitsPs)
									 : (uint32_t)(((uint64_t)idl * 1000 + unitsPs - 1) / unitsPs);   // DIV_ROUND_UP_ULL
			if (!clamped && wa16023105232 && (uint64_t)ticks * unitsPs >= (uint64_t)maxcnt * 1000) {
				idl = maxcnt ? maxcnt - 1 : 0;
				ticks = (uint32_t)((uint64_t)idl * 1000 / unitsPs);
			}
			reg = (reg & ~0x1fffffu) | (ticks & 0x1fffff);
			wr(base + 0x23c, reg);
			r.after = rd(base + 0x23c);
			r.written = true;
		}
		say("idledly %s: IDLEDLY 0x%08x -> 0x%08x (%u ns -> %u ns) MAXCNT 0x%08x (%u ns) inh=%u ts=%u%s%s\n", r.name, r.before,
			r.after, idlHw, idl, maxRaw, maxcnt, inhibit ? 1 : 0, tsBase, r.written ? "" : " (unchanged)",
			r.warned ? " WARN: hardware delay >= wait time" : "");
	} else {
		say("idledly %s: unknown crystal clock (RPM_CONFIG0 0x%08x), left alone\n", r.name, rd(gt + 0xd00));
	}
	const uint32_t g = gt ? 1 : 0;
	if (record && gIdledlyN[g] < 12) gIdledly[g][gIdledlyN[g]++] = r;
}

void IntelXe::adjustIdledlyAll(uint32_t gt, bool record) {
	if (record) gIdledlyN[gt ? 1 : 0] = 0;
	if (!gt) {
		for (uint32_t i = 0; i < gNumEngines; i++) {
			const XeEngine &e = gEngines[i];
			adjustIdledly(0, e.base, e.drmClass == 0 ? "RCS" : e.drmClass == 1 ? "BCS" : "CCS", e.phys, record);
		}
		return;
	}
	static const uint32_t vcsBase[8] = {0x1c0000, 0x1c4000, 0x1d0000, 0x1d4000, 0x1e0000, 0x1e4000, 0x1f0000, 0x1f4000};
	static const uint32_t vecsBase[4] = {0x1c8000, 0x1d8000, 0x1e8000, 0x1f8000};
	for (uint32_t i = 0; i < 12; i++) {
		const bool vcs = i < 8;
		if (!((vcs ? mVdbox >> i : mVebox >> (i - 8)) & 1)) continue;
		adjustIdledly(gt, vcs ? vcsBase[i] : vecsBase[i - 8], vcs ? "VCS" : "VECS", vcs ? i : i - 8, record);
	}
}

// debug.intelxe.step=21: read only. Per engine RING_IDLEDLY / RING_PWRCTX_MAXCNT now (and what the first bring-up's
// adjust_idledly found and wrote), RING_PSMI_CTL, the NONPRIV slots this driver programs (expected values in []),
// and for the render engine FF_SLICE_CS_CHICKEN1; then TDL_CHICKEN and whether 18041344222 applies. The media
// engines only when the media GT is up (woken for the read, as debug.intelxe.vcs_busy does).
int IntelXe::engineStateReport() {
	if (!up || suspended || !mapBar0()) { say("GPU not up\n"); return ENXIO; }
	auto one = [&](const char *name, uint32_t inst, uint32_t base, uint16_t drmClass, uint32_t gt) {
		const uint32_t idl = rd(base + 0x23c), mx = rd(base + 0x54);
		static const uint32_t tsBases[8] = {83333, 52083, 52083, 80000, 0, 0, 0, 0};
		const uint32_t ts = tsBases[(rd(gt + 0xd00) >> 3) & 7];
		say("%s%u: IDLEDLY 0x%08x (%llu ns, inh %u) MAXCNT 0x%08x (%u ns) PSMI_CTL 0x%08x", name, inst, idl,
			((uint64_t)(idl & 0x1fffff) * 8 * ts + 500) / 1000, idl >> 31, mx, (mx & 0xfffff) * 640, rd(base + 0x50));
		if (drmClass == 0) say(" FF_SLICE_CS_CHICKEN1 0x%08x", rd(base + 0xe0));
		uint32_t s[12];
		const uint32_t n = engineNonpriv(drmClass, s);
		say("\n   NONPRIV");
		for (uint32_t i = 0; i < n; i++) say(" %08x[%08x]", rd(base + 0x4d0 + 4 * i), gNonpriv ? s[i] : 0);
		say(n ? "\n" : " -\n");
		const uint32_t g = gt ? 1 : 0;
		for (uint32_t k = 0; k < gIdledlyN[g]; k++)
			if (gIdledly[g][k].base == base)
				say("   at first bring-up: IDLEDLY 0x%08x -> 0x%08x%s, MAXCNT 0x%08x, ts %u%s\n", gIdledly[g][k].before,
					gIdledly[g][k].after, gIdledly[g][k].written ? " (written)" : " (unchanged)", gIdledly[g][k].maxcnt,
					gIdledly[g][k].tsBase, gIdledly[g][k].warned ? ", WARN" : "");
	};
	if (!forcewake(true)) return ETIMEDOUT;
	for (uint32_t i = 0; i < gNumEngines; i++) {
		const XeEngine &e = gEngines[i];
		one(e.drmClass == 0 ? "RCS" : e.drmClass == 1 ? "BCS" : "CCS", e.phys, e.base, e.drmClass, 0);
	}
	uint32_t per = 0;
	const bool disc = dssGroupsDiscontiguous(&per);
	say("TDL_CHICKEN 0x%08x; DSS 0x%08x/0x%08x, %u per group: groups %s (18041344222 %s)\n", rd(0xe5f4),
		rd(XELP_GT_GEOMETRY_DSS_ENABLE), rd(XEHP_GT_COMPUTE_DSS_ENABLE), per, disc ? "discontiguous" : "contiguous",
		disc ? "applies" : "does not apply");
	// not programmed by this driver (report only): CCS mode (xe_gt_ccs_mode.c; Linux on 2 CCS: RCU_MODE fixed slice,
	// CCS_MODE all cslices on CCS0, CCS1 hidden) and the WMTP fuse of lrc_setup DISABLE_WMTP_ON_UNSUPPORTED_HW
	say("RCU_MODE 0x%08x CCS_MODE 0x%08x; XEHP_FUSE4 0x%08x (CCS_EN 0x%x, CFEG_WMTP_DISABLE %u)\n", rd(0x14800),
		rd(0x14804), rd(0x9114), (rd(0x9114) >> 16) & 0xf, (rd(0x9114) >> 20) & 1);
	forcewake(false);
	if (!mCtOn) {
		say("media GT not up: video engines not read\n");
		return 0;
	}
	if (!mediaWake(true)) { say("media wake failed\n"); return ETIMEDOUT; }
	for (uint32_t i = 0; i < gNumMediaEngines; i++) {
		const XeEngine &e = gMediaEngines[i];
		one(e.drmClass == 2 ? "VCS" : "VECS", e.phys, e.base, e.drmClass, MEDIA_GT_GSI_OFFSET);
	}
	mediaWake(false);
	return 0;
}

// debug.intelxe.step=22: adjust_idledly on every engine now (primary GT; media GT if up). Not something Linux does after
// probe: a test lever for what a resume or a GT reset left in RING_IDLEDLY. The first bring-up's record stays.
int IntelXe::idledlyStep() {
	if (!up || suspended || !mapBar0()) { say("GPU not up\n"); return ENXIO; }
	if (!forcewake(true)) return ETIMEDOUT;
	adjustIdledlyAll(0, false);
	forcewake(false);
	if (!mCtOn) {
		say("media GT not up: video engines left alone\n");
		return 0;
	}
	if (!mediaWake(true)) { say("media wake failed\n"); return ETIMEDOUT; }
	adjustIdledlyAll(MEDIA_GT_GSI_OFFSET, false);
	mediaWake(false);
	return 0;
}

// Everything up to a usable GuC submission path: BCS0 state, GuC with the copy class, hwconfig, CT. Leaves forcewake held.
int IntelXe::firstBatchSetupOnly() {
	if (!mapBar0()) return ENXIO;
	if (!miscMem) miscMem = alloc(miscSize, false);
	if (!miscMem) return ENOMEM;
	if (!forcewake(true)) return ETIMEDOUT;
	int err = ggttMap(miscMem, miscG);
	if (err) {
		forcewake(false);
		return err;
	}

	// Engines: RCS0, BCS0 (BCS8 is Linux's USM reserve) and the CCS instances XEHP_FUSE4 CCS_EN_MASK leaves on of those
	// the platform's engine mask has (read_compute_fuses_from_reg), then hw_engine_setup_default_state +
	// xe_hw_engine_enable_ring for each, before the GuC load as in xe_gt_init.
	uint32_t ccs = (rd(0x9114) >> 16) & 0xf & xePlatformEngines(gXePlat, XE_HW_ENGINE_CCS0, 4);
	gNumEngines = 0;
	gEngines[gNumEngines++] = {0, 0, 0, 0, 0x2000, 0x4000, 0x2000};       // RENDER: PPHWSP + 3 pages
	gEngines[gNumEngines++] = {1, 0, 0, 3, 0x22000, 0x2000, hwspOff};      // COPY: PPHWSP + 1 page
	static const uint32_t ccsBase[4] = {0x1a000, 0x1c000, 0x1e000, 0x26000};
	for (uint16_t i = 0, l = 0; i < 4; i++)
		if (ccs & 1u << i) gEngines[gNumEngines++] = {4, l++, i, 4, ccsBase[i], 0x3000, 0x3000u + i * 0x1000};   // COMPUTE: + 2 pages
	for (uint32_t i = 0; i < gNumEngines; i++) {
		const XeEngine &e = gEngines[i];
		uint32_t readMocs = e.drmClass == 4 ? 4 : 3;     // dGFX Xe2 compute reads through the WB MOCS entry
		wr(e.base + 0xc4, 0x3f7eu << 16 | 3u << 8 | readMocs << 1);   // RING_CMD_CCTL
		wr(e.base + 0x98, ~0u);                                       // RING_HWSTAM
		wr(e.base + 0x29c, 8u << 16 | 8u);                            // GFX_MODE: GFX_DISABLE_LEGACY_MODE
		// CSFE_CHICKEN1: CS_PRIORITY_MEM_READ, and Wa_16021639441 GHWSP_CSB_REPORT_DIS | PPHWSP_CSB_AND_TIMESTAMP_REPORT_DIS
		wr(e.base + 0xd4, 0xc080u << 16 | 0xc080u);
		wr(e.base + 0x50, 1u << 28 | 1u << 12);                        // RING_PSMI_CTL: Wa_16023105232
		engineExtras(e.base, e.drmClass);   // rb7p: NONPRIV whitelist slots, FF_SLICE_CS_CHICKEN1 (render)
		wr(e.base + 0x80, miscG + e.hwsp);                            // RING_HWS_PGA
	}
	if (ccs) wr(0x14800, 1u << 16 | 1u);                              // RCU_MODE: CCS enable

	// xe_wa.c gt_was / engine_was and xe_tuning.c gt_tunings / engine_tunings that match graphics 20.01, applied
	// over MMIO before the GuC load as xe_gt_init does (render/compute ones once: first render or compute engine).
	// MCR registers take a plain write as a multicast; masked registers get (bits << 16 | bits).
	auto setBits = [&](uint32_t r, uint32_t b) { wr(r, rd(r) | b); };
	auto masked = [&](uint32_t r, uint32_t b) { wr(r, b << 16 | b); };
	auto field = [&](uint32_t r, uint32_t m, uint32_t v) { wr(r, (rd(r) & ~m) | v); };
	field(0xb0c8, 0xfffff000, 0x11111000);            // LSN_VC_REG2 weights 1 (16025250150)
	setBits(0xe7cc, 1u << 8 | 1u << 15 | 1u << 12);   // LSC_CHICKEN_BIT_0_UDW (16025250150, 16018712365, 14018471104)
	setBits(0xe7c8, 1u << 13 | 1u << 26);             // LSC_CHICKEN_BIT_0 (18032247524, 14019811474)
	masked(0xe49c, 1u << 14);                         // ROW_CHICKEN3 XE2_EUPEND_CHK_FLUSH_DIS (14020338487)
	masked(0xe194, 1u << 6);                          // HALF_SLICE_CHICKEN7 CLEAR_OPTIMIZATION_DISABLE (14021402888)
	masked(0xe4c4, 1u << 6 | 1u << 12);               // TDL_TSL_CHICKEN RES_CHK_SPR_DIS, STK_ID_RESTRICT
	masked(0xe48c, 1u << 9);                          // ROW_CHICKEN4 DISABLE_TDL_PUSH (18034896535)
	masked(0xe4f0, 1u << 1);                          // ROW_CHICKEN EARLY_EOT_DIS (16018737384)
	setBits(0xe530, 1u << 10);                        // RT_CTRL DIS_NULL_QUERY (tuning)
	field(0xb158, 0x3ff, 0x7f);                       // XEHP_L3SQCREG5 L3 PWM timer (tuning)
	wr(0x8828, (rd(0x8828) & ~(1u << 18)) | 1u << 23);   // CCCHKNREG1 compression overfetch (tuning, flat CCS)
	setBits(0xb108, 1u << 28);                        // L3SQCREG3 COMPPWOVERFETCHEN (tuning)
	setBits(0xb104, 1u << 20);                        // L3SQCREG2 COMPMEMRD256BOVRFETCHEN (tuning)
	field(0x4148, 0xf, 0);                            // STATELESS_COMPRESSION_CTRL format 0 (tuning)
	// rb7p: 18041344222 (xe_wa.c:394, engine_was: GRAPHICS_VERSION_RANGE(2001, 3000), first render or compute engine,
	// xe_rtp_match_gt_has_discontiguous_dss_groups): TDL_CHICKEN EUSTALL_PERF_SAMPLING_DISABLE (MCR 0xe5f4 bit 5, masked)
	if (dssGroupsDiscontiguous()) masked(0xe5f4, 1u << 5);
	// rb7p: hw_engine_init's adjust_idledly ("Ensure IDLEDLY is lower than MAXCNT", xe_hw_engine.c:724-725), after
	// the engine's reg_sr and RING_HWS_PGA as there; probe only (see adjustIdledly)
	if (!gIdledlyDone[0]) {
		adjustIdledlyAll(0, true);
		gIdledlyDone[0] = true;
	}
	say("engines: RCS0 BCS0 CCS mask 0x%x\n", ccs);
	forcewake(false);

	if ((err = loadGuc(true))) return err;
	if (!forcewake(true)) return ETIMEDOUT;
	uint32_t hwcfg = 0, q[4] = {0x4100, 0, 0, 0};
	if (!mmioSend(q, 4, &hwcfg)) say("hwconfig table: %u bytes\n", hwcfg);
	// guc_hwconfig_copy: the same action with a GGTT buffer and its size; GuC fills it. Misc page 3 (golden context
	// area, unused by GuC until a context of the class is reset) is borrowed only while copying.
	if (hwcfg && hwcfg <= sizeof(gHwconfig) && hwcfg <= 0x1000) {
		uint8_t *buf = static_cast<uint8_t *>(miscMem->getBytesNoCopy()) + goldenOff + 0x1000;
		bzero(buf, 0x1000);
		flushRange(buf, 0x1000);
		uint32_t c[4] = {0x4100, miscG + goldenOff + 0x1000, 0, hwcfg}, got = 0;
		if (!mmioSend(c, 4, &got)) {
			flushRange(buf, hwcfg);
			memcpy(gHwconfig, buf, hwcfg);
			gHwconfigLen = hwcfg;
			say("hwconfig copied (%u), first KLVs %08x %08x %08x %08x\n", got, ((uint32_t *)buf)[0], ((uint32_t *)buf)[1],
				((uint32_t *)buf)[2], ((uint32_t *)buf)[3]);
		}
		bzero(buf, 0x1000);
		flushRange(buf, 0x1000);
	}
	if ((err = ctSetup())) {
		forcewake(false);
		return err;
	}
	forcewake(false);
	return 0;
}

int IntelXe::firstBatch() {
	if (!lrcMem) lrcMem = alloc(ringSize + lrcSize, false);
	if (!lrcMem) return ENOMEM;
	int err = firstBatchSetupOnly();
	if (err) return err;
	if (!forcewake(true)) return ETIMEDOUT;
	const uint32_t bcs = 0x22000;

	// The LRC: ring (16 KB), PPHWSP (4 KB), context image (4 KB) as empty_lrc_data + xe_lrc_init build it.
	auto *l = static_cast<uint8_t *>(lrcMem->getBytesNoCopy());
	bzero(l, lrcMem->getLength());
	auto *ring = reinterpret_cast<uint32_t *>(l);
	auto *regs = reinterpret_cast<uint32_t *>(l + ringSize + 0x1000);
	lrcSetOffsets(regs, xe2_bcs_offsets, bcs);
	regs[CTX_CONTEXT_CONTROL] = (1u << 3 | 1u) << 16 | (1u << 3 | 1u);   // INHIBIT_SYN_CTX_SWITCH, ENGINE_CTX_RESTORE_INHIBIT
	regs[CTX_RING_START] = lrcG;
	regs[CTX_RING_CTL] = (ringSize - 0x1000) | 1;
	uint32_t n = 0;
	ring[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	ring[n++] = MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1);
	ring[n++] = miscG + targetOff;
	ring[n++] = 0;
	ring[n++] = 0xb580c0de;
	while (n & 1) ring[n++] = MI_NOOP;
	regs[CTX_RING_TAIL] = n * 4;
	flushRange(l, lrcMem->getLength());
	auto *target = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(miscMem->getBytesNoCopy()) + targetOff);
	*target = 0x11111111;
	flushRange(const_cast<uint32_t *>(target), 4);
	if ((err = ggttMap(lrcMem, lrcG))) {
		forcewake(false);
		return err;
	}
	const uint32_t lrca = lrcG + ringSize, desc = lrca | 1u | 3u << 3;   // LRC_VALID, LEGACY_64B_CONTEXT
	say("LRC 0x%x (ring 0x%x, tail %u), descriptor 0x%08x, target 0x%x\n", lrca, lrcG, n * 4, desc, miscG + targetOff);

	// register_exec_queue + submit_exec_queue: KMD normal context 0 on GuC blitter class, logical mask 1.
	const uint32_t id = 0;
	uint32_t reg[12] = {0x4502, 1, id, 3, 1, 0, 0, 0, 0, 0, desc, 0};
	ctSend(reg, 12);
	uint32_t en[3] = {0x1001, id, 1};
	ctSend(en, 3);

	uint32_t v = 0, ms = 0;
	for (; ms <= 2000; ms += 10) {
		flushRange(const_cast<uint32_t *>(target), 4);
		if ((v = *target) == 0xb580c0de) break;
		IOSleep(10);
	}
	say("target after %u ms: 0x%08x -> %s\n", ms, v, v == 0xb580c0de ? "THE B580 EXECUTED OUR COMMAND" : "not written");
	ctDrain();
	say("BCS0: head 0x%x tail 0x%x start 0x%x ctl 0x%x acthd 0x%x hws 0x%x mi_mode 0x%x ccid 0x%x execlist 0x%08x%08x\n",
		rd(bcs + 0x34), rd(bcs + 0x30), rd(bcs + 0x38), rd(bcs + 0x3c), rd(bcs + 0x74), rd(bcs + 0x80), rd(bcs + 0x9c),
		rd(bcs + 0x180), rd(bcs + 0x238), rd(bcs + 0x234));
	flushRange(regs, 64);
	say("context image: tail %u head %u ctl 0x%x\n", regs[CTX_RING_TAIL], regs[CTX_RING_HEAD], regs[CTX_RING_CTL]);
	gucStatus();
	forcewake(false);
	if (v != 0xb580c0de) return EIO;
	ringTail = n * 4;
	return 0;
}

// Later jobs on context 0: append to the ring, move the LRC's tail, XE_GUC_ACTION_SCHED_CONTEXT (submit_exec_queue
// for an enabled queue), then poll a dword the job writes last.
int IntelXe::submit(const uint32_t *cmd, uint32_t n, volatile uint32_t *done, uint32_t value, uint32_t ms) {
	if (!ringTail) {
		say("no context: run step 6 first\n");
		return ENXIO;
	}
	auto *l = static_cast<uint8_t *>(lrcMem->getBytesNoCopy());
	auto *ring = reinterpret_cast<uint32_t *>(l);
	auto *regs = reinterpret_cast<uint32_t *>(l + ringSize + 0x1000);
	uint32_t t = ringTail / 4;
	if ((t + n + 1) * 4 > ringSize) {
		say("ring full\n");
		return ENOSPC;
	}
	for (uint32_t i = 0; i < n; i++) ring[t++] = cmd[i];
	while (t & 1) ring[t++] = MI_NOOP;
	flushRange(ring + ringTail / 4, (t - ringTail / 4) * 4);
	ringTail = t * 4;
	regs[CTX_RING_TAIL] = ringTail;
	flushRange(regs + CTX_RING_TAIL, 4);
	uint32_t act[2] = {0x1000, 0};
	ctSend(act, 2);
	uint32_t waited = 0;
	for (; waited <= ms; waited += 1) {
		flushRange(const_cast<uint32_t *>(done), 4);
		if (*done == value) break;
		IOSleep(1);
	}
	say("job done after %u ms: %s (0x%08x)\n", waited, *done == value ? "yes" : "NO", *done);
	return *done == value ? 0 : ETIMEDOUT;
}

int IntelXe::vramTest() {
	if (!mapBar0()) return ENXIO;
	if (!bar2) {
		bar2 = pci->mapDeviceMemoryWithRegister(0x18, kIOMapWriteCombineCache);
		if (!bar2) {
			say("BAR2 map failed (is ResizeAppleGpuBars 0..10 set?)\n");
			return ENXIO;
		}
	}
	say("BAR2 phys 0x%llx len 0x%llx\n", bar2->getPhysicalAddress(), bar2->getLength());
	if (!forcewake(true)) return ETIMEDOUT;
	// xe_gt_enable_host_l2_vram (Wa_16023588340, graphics 20.01)
	wr(0x4194, rd(0x4194) | 1u << 6);   // XE2_GAMREQSTRM_CTRL: CG_DIS_CNTLBUS
	wr(0xb194 + 3 * 8, 0xf);             // XEHPC_L3CLOS_MASK(3)

	// CPU only: 4 KB pattern at three places in the CPU-visible window.
	auto *v = reinterpret_cast<volatile uint32_t *>(bar2->getVirtualAddress());
	const uint64_t offs[] = {16ull << 20, 128ull << 20, bar2->getLength() - 0x1000};
	int bad = 0;
	for (uint64_t o : offs) {
		volatile uint32_t *p = v + o / 4;
		for (uint32_t i = 0; i < 1024; i++) p[i] = 0xa5000000u ^ (uint32_t)o ^ i * 0x9e3779b1u;
		asm volatile("sfence" : : : "memory");
		uint32_t miss = 0;
		for (uint32_t i = 0; i < 1024; i++) miss += p[i] != (0xa5000000u ^ (uint32_t)o ^ i * 0x9e3779b1u);
		say("CPU VRAM +0x%llx: %u/1024 dwords wrong\n", o, miss);
		bad += miss != 0;
	}

	// GPU into VRAM: GGTT page 0x1700000 -> VRAM DPA 17 MB (XE_GGTT_PTE_DM, PAT 2), MI_STORE_DATA_IMM there, then the
	// completion marker in system memory, xe_device_l2_flush, CPU read through BAR2.
	const uint32_t vramG = 0x1700000;
	const uint64_t dpa = 17ull << 20;
	volatile uint32_t *cpu = v + dpa / 4;
	cpu[0] = 0;
	asm volatile("sfence" : : : "memory");
	reinterpret_cast<volatile uint64_t *>(bar0->getVirtualAddress() + 0x800000)[vramG >> 12] = dpa | 1 | 1ull << 1 | 1ull << 53;
	wr(PVC_GUC_TLB_INV_DESC1, 1u << 6);
	wr(PVC_GUC_TLB_INV_DESC0, 1u << 0);
	auto *target = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(miscMem->getBytesNoCopy()) + targetOff);
	const uint32_t job[] = {
		MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1), vramG, 0, 0x0b580e3a,
		MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1), miscG + targetOff, 0, 0x2,
	};
	int err = submit(job, 8, target, 0x2, 2000);
	if (!err) {
		uint32_t before = cpu[0];
		wr(0xb404, 1);   // XE2_GLOBAL_INVAL
		uint32_t gi;
		bool ok = waitFor(0xb404, 1, 0, 1000, &gi);
		say("GPU->VRAM: CPU reads 0x%08x before L2 flush, 0x%08x after (flush %s) -> %s\n", before, cpu[0],
			ok ? "done" : "TIMEOUT", cpu[0] == 0x0b580e3a ? "VRAM WRITE SEEN" : "not seen");
		if (cpu[0] != 0x0b580e3a) err = EIO;
	}
	ctDrain();
	forcewake(false);
	return err ? err : bad ? EIO : 0;
}

// PPGTT as xe_vm/xe_pt build it on BMG: root at level 4 (vm_max_level 4), XE_PTE_SHIFT 12 + 9 bits per level. Page
// tables live in VRAM (xe_pt_create uses VRAM_IF_DGFX; a PDE has no DM bit), PDEs = DPA | PRESENT | RW | PAT 3
// (pde_pat_index: VRAM -> XE_CACHE_NONE). Leaves: system page PAT 2 (WB), VRAM page DM + PAT 3. The context gets the
// root in CTX_PDP0 (xe_lrc_set_ppgtt), the ASID (has_asid) and LRC_PRIVILEGE in its descriptor.
int IntelXe::ppgttTest() {
	if (!ringTail || !bar2) {
		say("run steps 6 and 8 first\n");
		return ENXIO;
	}
	if (!sysPage) sysPage = alloc(0x1000, true);
	if (!lrc1Mem) lrc1Mem = alloc(ringSize + lrcSize, false);
	if (!sysPage || !lrc1Mem) return ENOMEM;
	if (!forcewake(true)) return ETIMEDOUT;

	auto *vram = reinterpret_cast<volatile uint64_t *>(bar2->getVirtualAddress());
	const uint64_t ptDpa = 32ull << 20, vramPage = 40ull << 20;
	auto pde = [](uint64_t dpa) { return dpa | 1 | 2 | 1ull << 3 | 1ull << 4; };
	for (uint32_t i = 0; i < 5 * 512; i++) vram[ptDpa / 8 + i] = 0;
	for (uint32_t lvl = 0; lvl < 4; lvl++) vram[(ptDpa + lvl * 0x1000) / 8] = pde(ptDpa + (lvl + 1) * 0x1000);
	volatile uint64_t *l0 = vram + (ptDpa + 4 * 0x1000) / 8;
	static DmaMap sysDma;   // the page's device address (IOVA under VT-d), kept for the life of the page
	uint64_t sysPa = 0, sysRun = 0;
	if ((!sysDma.n && !sysDma.map(sysPage)) || !sysDma.lookup(0, &sysPa, &sysRun)) return EFAULT;
	l0[0x100] = sysPa | 1 | 2 | 1ull << 4;                                  // VA 0x100000 -> system page, PAT 2
	l0[0x101] = vramPage | 1 | 2 | 1ull << 11 | 1ull << 3 | 1ull << 4;      // VA 0x101000 -> VRAM 40 MB, DM, PAT 3
	auto *sysV = static_cast<volatile uint32_t *>(sysPage->getBytesNoCopy());
	auto *vramV = reinterpret_cast<volatile uint32_t *>(bar2->getVirtualAddress() + vramPage);
	sysV[0] = 0;
	flushRange(const_cast<uint32_t *>(sysV), 4);
	vramV[0] = 0;
	asm volatile("sfence" : : : "memory");
	say("page tables at VRAM 0x%llx, root PDE 0x%llx, L0[0x100] 0x%llx, L0[0x101] 0x%llx\n", ptDpa, pde(ptDpa),
		l0[0x100], l0[0x101]);

	// Context 1: LRC as in step 6 plus CTX_PDP0 / CTX_ASID.
	const uint32_t lrc1G = 0x1800000, bcs = 0x22000;
	auto *l = static_cast<uint8_t *>(lrc1Mem->getBytesNoCopy());
	bzero(l, lrc1Mem->getLength());
	auto *ring = reinterpret_cast<uint32_t *>(l);
	auto *regs = reinterpret_cast<uint32_t *>(l + ringSize + 0x1000);
	lrcSetOffsets(regs, xe2_bcs_offsets, bcs);
	regs[CTX_CONTEXT_CONTROL] = (1u << 3 | 1u) << 16 | (1u << 3 | 1u);
	regs[CTX_RING_START] = lrc1G;
	regs[CTX_RING_CTL] = (ringSize - 0x1000) | 1;
	regs[CTX_PDP0_UDW] = (uint32_t)(pde(ptDpa) >> 32);
	regs[CTX_PDP0_LDW] = (uint32_t)pde(ptDpa);
	regs[CTX_ASID] = 1;
	uint32_t n = 0;
	ring[n++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
	ring[n++] = MI_STORE_DATA_IMM | MI_SDI_NUM_DW(1);
	ring[n++] = 0x100000;
	ring[n++] = 0;
	ring[n++] = 0x5a5a0001;
	ring[n++] = MI_STORE_DATA_IMM | MI_SDI_NUM_DW(1);
	ring[n++] = 0x101000;
	ring[n++] = 0;
	ring[n++] = 0x5a5a0002;
	ring[n++] = MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1);
	ring[n++] = miscG + targetOff;
	ring[n++] = 0;
	ring[n++] = 0x3;
	while (n & 1) ring[n++] = MI_NOOP;
	regs[CTX_RING_TAIL] = n * 4;
	flushRange(l, lrc1Mem->getLength());
	int err = ggttMap(lrc1Mem, lrc1G);
	if (err) {
		forcewake(false);
		return err;
	}
	auto *target = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(miscMem->getBytesNoCopy()) + targetOff);
	*target = 0;
	flushRange(const_cast<uint32_t *>(target), 4);
	const uint32_t desc = (lrc1G + ringSize) | 1u | 3u << 3 | 1u << 8;   // VALID, LEGACY_64B, PRIVILEGE (PPGTT)
	uint32_t reg[12] = {0x4502, 1, 1, 3, 1, 0, 0, 0, 0, 0, desc, 0};
	ctSend(reg, 12);
	uint32_t en[3] = {0x1001, 1, 1};
	ctSend(en, 3);
	uint32_t ms = 0;
	for (; ms <= 2000; ms += 1) {
		flushRange(const_cast<uint32_t *>(target), 4);
		if (*target == 0x3) break;
		IOSleep(1);
	}
	wr(0xb404, 1);   // XE2_GLOBAL_INVAL before the CPU reads VRAM
	uint32_t gi;
	waitFor(0xb404, 1, 0, 1000, &gi);
	flushRange(const_cast<uint32_t *>(sysV), 4);
	bool ok = *target == 0x3 && sysV[0] == 0x5a5a0001 && vramV[0] == 0x5a5a0002;
	say("context 1 after %u ms: marker 0x%x, system page 0x%08x, VRAM page 0x%08x -> %s\n", ms, *target, sysV[0], vramV[0],
		ok ? "PPGTT WORKS" : "FAILED");
	ctDrain();
	say("BCS0: head 0x%x tail 0x%x acthd 0x%x ccid 0x%x\n", rd(bcs + 0x34), rd(bcs + 0x30), rd(bcs + 0x74), rd(bcs + 0x180));
	forcewake(false);
	return ok ? 0 : EIO;
}

// ─── media GT (gt1): GuC and video engines ─────────────────────────────────────────────────────────────────────
// Per-engine forcewake of the media GT (xe_force_wake_init_engines): FORCEWAKE_MEDIA_VDBOX(n) 0xa540 + 4n / ack 0xd50 + 4n,
// FORCEWAKE_MEDIA_VEBOX(n) 0xa560 + 4n / ack 0xd70 + 4n, at +0x380000, for the fused-in instances.
bool IntelXe::mediaEngineWake(bool on) {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	bool ok = true;
	for (uint32_t i = 0; i < 8; i++) {
		const uint32_t mask[2] = {mVdbox, mVebox}, req[2] = {0xa540, 0xa560}, ack[2] = {0xd50, 0xd70};
		for (uint32_t k = 0; k < 2; k++) {
			if (!(mask[k] & 1u << i)) continue;
			uint32_t a;
			wr(M + req[k] + 4 * i, 0x10000 | (on ? 1 : 0));
			if (!waitFor(M + ack[k] + 4 * i, 1, on ? 1 : 0, 50000, &a)) {
				say("%s%u forcewake %s: no ack (0x%08x)\n", k ? "VECS" : "VCS", i, on ? "on" : "off", a);
				ok = false;
			}
		}
	}
	return ok;
}

// The media GT as xe_gt_init brings up gt1 on BMG: engine default state (hw_engine_setup_default_state: RING_CMD_CCTL
// MOCS uc_index 3 read and write, HWSTAM, GFX_MODE legacy off; CS_PRIORITY_MEM_READ is a graphics-version rule and
// does not match media engines), the media 13.01 workarounds and tunings of xe_wa.c / xe_tuning.c, RING_HWS_PGA
// (xe_hw_engine_enable_ring), then the GuC load and CT. Power gating stays as found (Linux enables it in
// xe_gt_idle_enable_pg, minus VDN0/2 for Wa_14020316580).
int IntelXe::mediaGucLoad() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	if (!mapBar0()) return ENXIO;
	if (!mMiscMem) mMiscMem = alloc(mMiscSize, false);
	if (!mMiscMem) return ENOMEM;
	mCtOn = false;
	if (!forcewake(true, M)) return ETIMEDOUT;
	uint32_t g = rd(M + GMD_ID), fuse = rd(M + GT_VEBOX_VDBOX_DISABLE);   // Xe_HP+: enable bits, VDBOX 7:0, VEBOX 19:16
	mVdbox = fuse & 0xff & xePlatformEngines(gXePlat, XE_HW_ENGINE_VCS0, 8);     // read_media_fuses: of the engine mask
	mVebox = (fuse >> 16) & 0xf & xePlatformEngines(gXePlat, XE_HW_ENGINE_VECS0, 4);
	say("media GT %u.%02u rev %u: VDBOX 0x%02x VEBOX 0x%x, GuC status 0x%08x, POWERGATE_ENABLE 0x%08x\n", g >> 22,
		(g >> 14) & 0xff, g & 0x3f, mVdbox, mVebox, rd(M + GUC_STATUS), rd(M + 0xa210));
	if (!mVdbox || __builtin_popcount(mVdbox) + __builtin_popcount(mVebox) > 4) {
		say("unexpected media engine set\n");
		forcewake(false, M);
		return ENODEV;
	}
	int err = ggttMap(mMiscMem, mMiscG, M);
	if (err || !mediaEngineWake(true)) {
		mediaEngineWake(false);
		forcewake(false, M);
		return err ? err : ETIMEDOUT;
	}
	static const uint32_t vcsBase[8] = {0x1c0000, 0x1c4000, 0x1d0000, 0x1d4000, 0x1e0000, 0x1e4000, 0x1f0000, 0x1f4000};
	static const uint32_t vecsBase[4] = {0x1c8000, 0x1d8000, 0x1e8000, 0x1f8000};
	uint32_t hwsp = 0, nVecs = 0;
	gNumMediaEngines = 0;
	for (uint32_t i = 0; i < 12; i++) {
		const bool vcs = i < 8;
		if (!((vcs ? mVdbox >> i : mVebox >> (i - 8)) & 1)) continue;
		const uint32_t b = vcs ? vcsBase[i] : vecsBase[i - 8];
		wr(b + 0xc4, 0x3f7eu << 16 | 3u << 8 | 3u << 1);   // RING_CMD_CCTL: write/read MOCS uc_index 3
		wr(b + 0x98, ~0u);                                   // RING_HWSTAM
		wr(b + 0x29c, 8u << 16 | 8u);                        // GFX_MODE: GFX_DISABLE_LEGACY_MODE
		wr(b + 0xd4, 0xc000u << 16 | 0xc000u);               // CSFE_CHICKEN1: Wa_16021639441 CSB report disables
		wr(b + 0x50, 1u << 28 | 1u << 12);                   // RING_PSMI_CTL: Wa_16023105232 RC_SEMA_IDLE_MSG_DISABLE
		if (vcs) {
			wr(b + 0x3f1c, rd(b + 0x3f1c) | 1u << 3);        // VDBOX_CGCTL3F1C MFXPIPE_CLKGATE_DIS (Wa_16021867713)
			wr(b + 0x3f08, rd(b + 0x3f08) | 1u << 5);        // VDBOX_CGCTL3F08 CG3DDISHRS_CLKGATE_DIS (Wa_14019449301)
		}
		engineExtras(b, vcs ? 2 : 3);                        // rb7p: NONPRIV slots (OAM, DENY)
		wr(b + 0x80, mMiscG + hwsp);                         // RING_HWS_PGA
		say("  %s%u base 0x%x HWSP 0x%x CCTL 0x%08x GFX_MODE 0x%08x\n", vcs ? "VCS" : "VECS", vcs ? i : i - 8, b,
			mMiscG + hwsp, rd(b + 0xc4), rd(b + 0x29c));
		if (vcs) {   // DRM_XE_ENGINE_CLASS_VIDEO_DECODE, logical ids in instance order, GUC_VIDEO_CLASS, PPHWSP + 1 page
			gMediaEngines[gNumMediaEngines] = {2, (uint16_t)gNumMediaEngines, (uint16_t)i, 1, b, 0x2000, hwsp, M};
			gNumMediaEngines++;
		} else {   // rb7j: DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE (3), its own logical ids, GUC_VIDEOENHANCE_CLASS (2), as xe
			// lists every VECS (the ADS has listed them for the media GuC since the media bring-up)
			gMediaEngines[gNumMediaEngines] = {3, (uint16_t)nVecs++, (uint16_t)(i - 8), 2, b, 0x2000, hwsp, M};
			gNumMediaEngines++;
		}
		hwsp += 0x1000;
	}
	// rb7p: adjust_idledly for the media engines (hw_engine_init on gt1), at the media GT's first bring-up only
	if (!gIdledlyDone[1]) {
		adjustIdledlyAll(M, true);
		gIdledlyDone[1] = true;
	}
	wr(M + 0xc50c, rd(M + 0xc50c) | 1u << 1);    // GUC_INTR_CHICKEN DISABLE_SIGNALING_ENGINES (Wa_16028005424)
	wr(M + 0x4148, rd(M + 0x4148) & ~0xfu);      // STATELESS_COMPRESSION_CTRL format 0 (tuning, media >= 13.01)
	mediaEngineWake(false);
	forcewake(false, M);

	if ((err = loadGuc(true, M))) return err;
	if (!forcewake(true, M)) return ETIMEDOUT;
	err = ctSetup(M);
	if (!err) {
		// A freshly loaded GuC holds no translations: invalidations issued while it was down count as done for it, or
		// whatever waits for one would wait for a reply it never gets.
		mTlbDone = tlbSeqno;
		mCtOn = true;
		mediaPcStart();   // xe_uc_load_hw: xe_guc_pc_start, xe_guc_rc_enable, then xe_huc_auth(XE_HUC_AUTH_VIA_GUC)
		// xe_gt_idle_enable_pg for the media GT: MEDIA_POWERGATE_ENABLE, and VDN_HCP/VDN_MFXVDENC_POWERGATE_ENABLE for each
		// VCS except VCS0 and VCS2 (Wa_14020316580, media 13.01)
		{
			uint32_t pg = 1u << 1;
			for (uint32_t i = 0; i < 8; i++)
				if ((mVdbox >> i & 1) && i != 0 && i != 2) pg |= 1u << (3 + 2 * i) | 1u << (4 + 2 * i);
			wr(M + 0xa210, pg);
			say("media POWERGATE_ENABLE 0x%08x\n", rd(M + 0xa210));
		}
		if (mSlpcOn && gGucRc >= 0) {   // xe_guc_rc_enable on the media GuC too: it power-gates the media GT when idle
			uint32_t rc[2] = {0x3004, (uint32_t)gGucRc};   // GUC_ACTION_HOST2GUC_SETUP_PC_GUCRC
			ctSend(rc, 2, M);
		}
		hucAuth();
		IOSleep(10);
		ctDrain(M);
	}
	forcewake(false, M);
	if (!err && irqOn) mediaIrqEnable();
	return err;
}

// xe_irq_enable_hwe for the media GT (GuC submission: user + flush-complete): VCS class enable in ENGINE1 of
// VCS_VECS_INTR_ENABLE, per-instance unmask; irqHandle acks them generically and wakes the waiters. guc_enable_irq for
// the media GuC: GUC2HOST in ENGINE0 of GUC_SG_INTR_MASK (the primary's is ENGINE1 of the same register; the enable
// bit is shared), so its G2H is read on its interrupt (identity OTHER class, instance 16).
void IntelXe::mediaIrqEnable() {
	const uint32_t common = 1u << 0 | 1u << 4;
	wr(0x1900e8, rd(0x1900e8) & ~(1u << 15));                    // GUC_SG_INTR_MASK: media GUC2HOST
	if (mVdbox & 0x3) wr(0x1900a8, ~(common << 16 | common));     // VCS0_VCS1_INTR_MASK
	if (mVdbox & 0xc) wr(0x1900ac, ~(common << 16 | common));     // VCS2_VCS3_INTR_MASK
	if (mVdbox & 0x30) wr(0x1900b0, ~(common << 16 | common));    // VCS4_VCS5_INTR_MASK
	if (mVdbox & 0xc0) wr(0x1900b4, ~(common << 16 | common));    // VCS6_VCS7_INTR_MASK
	// rb7j: VECS too (xe_irq.c gt_irq_postinstall: VCS_VECS_INTR_ENABLE ENGINE0 = vecs_mask, VECSn_VECSm_INTR_MASK)
	wr(0x190034, common << 16 | common);                         // VCS_VECS_INTR_ENABLE: VCS (ENGINE1) and VECS (ENGINE0)
	if (mVebox & 0x3) wr(0x1900d0, ~(common << 16 | common));     // VECS0_VECS1_INTR_MASK
	if (mVebox & 0xc) wr(0x1900d4, ~(common << 16 | common));     // VECS2_VECS3_INTR_MASK
}

// xe/bmg_huc.bin is a GSC CPD image (parse_cpd_header): "$CPD" header, entries {name[12], offset (24:0), length,
// reserved}; the "huc_fw" entry is a CSS image (header, ucode, RSA) like the GuC's. Returns the CSS offset.
static bool hucLayout(uint32_t *css, uint32_t *ucode, uint32_t *rsa, uint32_t *ver) {
	const uint8_t *d = kHucFw;
	if (kHucFwSize < 16 || memcmp(d, "$CPD", 4)) return false;
	uint32_t n, hl = d[10];
	memcpy(&n, d + 4, 4);
	if (hl < 16 || hl + n * 24 > kHucFwSize) return false;
	for (uint32_t i = 0; i < n; i++) {
		const uint8_t *e = d + hl + i * 24;
		if (strncmp(reinterpret_cast<const char *>(e), "huc_fw", 12)) continue;
		uint32_t off;
		memcpy(&off, e + 12, 4);
		off &= 0x1ffffff;
		if (off + sizeof(uc_css_header) > kHucFwSize) return false;
		uc_css_header h;
		memcpy(&h, d + off, sizeof(h));
		if ((h.header_size_dw - h.key_size_dw - h.modulus_size_dw - h.exponent_size_dw) * 4 != sizeof(uc_css_header)) return false;
		*css = off;
		*ucode = (h.size_dw - h.header_size_dw) * 4;
		*rsa = h.key_size_dw * 4;
		*ver = h.sw_version;
		return off + sizeof(uc_css_header) + *ucode + *rsa <= kHucFwSize;
	}
	return false;
}

// xe_huc_upload -> uc_fw_xfer(offset 0, HUC_UKERNEL): CSS header + ucode from the GGTT copy to the WOPCM bottom.
int IntelXe::hucUpload() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	uint32_t css, ucode, rsa, ver;
	if (!hucLayout(&css, &ucode, &rsa, &ver)) {
		say("HuC: unexpected firmware layout\n");
		return EINVAL;
	}
	hucVer = ver;
	if (!mHucMem) mHucMem = alloc(kHucFwSize, false);
	if (!mHucMem) return ENOMEM;
	memcpy(mHucMem->getBytesNoCopy(), kHucFw, kHucFwSize);
	flushRange(mHucMem->getBytesNoCopy(), kHucFwSize);
	int err = ggttMap(mHucMem, mHucG, M);
	if (err) return err;
	wr(M + DMA_ADDR_0_LOW, mHucG + css);
	wr(M + DMA_ADDR_0_HIGH, 8u << 16);              // DMA_ADDRESS_SPACE_GGTT
	wr(M + DMA_ADDR_1_LOW, 0);
	wr(M + DMA_ADDR_1_HIGH, 7u << 16);              // DMA_ADDRESS_SPACE_WOPCM
	wr(M + DMA_COPY_SIZE, (uint32_t)sizeof(uc_css_header) + ucode);
	wr(M + DMA_CTRL, (1u << 9 | 1u) << 16 | 1u << 9 | 1u);   // HUC_UKERNEL | START_DMA
	uint32_t dma;
	bool ok = waitFor(M + DMA_CTRL, 1, 0, 100000, &dma);
	wr(M + DMA_CTRL, (1u << 9) << 16);
	say("HuC %u.%u.%u: CSS at +0x%x, ucode %u, rsa %u; DMA %s (DMA_CTRL 0x%08x), load info 0x%08x\n", (ver >> 16) & 0xff,
		(ver >> 8) & 0xff, ver & 0xff, css, ucode, rsa, ok ? "done" : "TIMEOUT", dma, rd(M + HUC_KERNEL_LOAD_INFO));
	return ok ? 0 : ETIMEDOUT;
}

// xe_huc_auth(XE_HUC_AUTH_VIA_GUC): XE_GUC_ACTION_AUTHENTICATE_HUC with the GGTT address of the RSA signature
// (xe_uc_fw_rsa_offset: CSS offset + header + ucode), then HUC_KERNEL_LOAD_INFO bit 0 within 100 ms. Not fatal.
void IntelXe::hucAuth() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	uint32_t css, ucode, rsa, ver, v = rd(M + HUC_KERNEL_LOAD_INFO);
	mHucOn = false;
	if (v & 1) {   // "On newer platforms the HuC survives reset"
		mHucOn = true;
		say("HuC already authenticated (load info 0x%08x)\n", v);
		return;
	}
	if (!mHucMem || !hucLayout(&css, &ucode, &rsa, &ver)) return;
	uint32_t a[2] = {0x4000, mHucG + css + (uint32_t)sizeof(uc_css_header) + ucode};
	ctSend(a, 2, M, true);
	mHucOn = waitFor(M + HUC_KERNEL_LOAD_INFO, 1, 1, 100000, &v);
	say("HuC authentication via the media GuC (RSA at 0x%x): %s (load info 0x%08x)\n", a[1],
		mHucOn ? "RUNNING" : "FAILED", v);
}

// xe_guc_pc_start on the media GuC: SLPC shared data, RESET, wait for RUNNING, then the bounds pc_adjust_freq_bounds
// sets on BMG G21: max = media RP0 (MTL_MEDIAP_STATE_CAP), min = 1200 MHz (Wa_14022085890 applies to both GTs),
// compute strategy. Without SLPC (debug.intelxe.media_slpc=0, or it does not start): RP0 requested once as
// pc_set_cur_freq does.
void IntelXe::mediaPcStart() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	auto mhz = [](uint32_t raw) { return (raw * 50 + 1) / 3; };
	const uint32_t cap = rd(0x138020), rp0 = mhz(cap & 0x1ff), rpn = mhz(cap >> 16 & 0x1ff);
	mSlpcOn = false;
	uint32_t state = 0;
	if (gMediaSlpc) {
		if (!mSlpcMem) mSlpcMem = alloc(0x2000, true);
		if (mSlpcMem && !ggttMap(mSlpcMem, mSlpcG, M)) {
			auto *sd = static_cast<volatile uint32_t *>(mSlpcMem->getBytesNoCopy());
			bzero(mSlpcMem->getBytesNoCopy(), 0x2000);
			sd[0] = 0x2000;   // header.size
			flushRange(mSlpcMem->getBytesNoCopy(), 0x2000);
			uint32_t reset[4] = {0x3003, 0u << 8 | 2, mSlpcG, 0};   // SLPC_EVENT(RESET, 2)
			ctSend(reset, 4, M);
			for (int i = 0; i < 1000 && !mSlpcOn; i++) {
				flushRange(mSlpcMem->getBytesNoCopy(), 64);
				mSlpcOn = (state = sd[1]) == 3;   // SLPC_GLOBAL_STATE_RUNNING
				if (!mSlpcOn) IODelay(1000);
			}
			if (mSlpcOn) {
				const uint32_t params[][2] = {{7, rp0}, {6, rp0 < 1200 ? rp0 : 1200}, {26, 1}};   // MAX, MIN, STRATEGIES
				for (auto &p : params) {
					uint32_t set[4] = {0x3003, 6u << 8 | 2, p[0], p[1]};   // SLPC_EVENT(PARAMETER_SET, 2)
					ctSend(set, 4, M);
				}
			}
		}
	}
	if (!mSlpcOn) {
		wr(M + 0xa024, 2u << 9);                 // RP_CONTROL: RPSWCTL_ENABLE
		wr(M + 0xa008, (cap & 0x1ff) << 23);     // RPNSWREQ
		IODelay(300);
		wr(M + 0xa024, 0);
	}
	say("media SLPC %s (state %u); RP0 %u MHz, RPn %u MHz%s\n", mSlpcOn ? "running" : gMediaSlpc ? "did NOT start" : "off",
		state, rp0, rpn, mSlpcOn ? "" : ", RP0 requested directly");
}

// Media GT frequency (xe_guc_pc.c): caps in MTL_MEDIAP_STATE_CAP, MTL_MPA/MPE_FREQUENCY; actual (CAGF) in
// MTL_MIRROR_TARGET_WP1 and the request in RPNSWREQ on the media GT. Sampled with the media GT forcewake held.
void IntelXe::mediaFreqReport() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET;
	auto mhz = [](uint32_t raw) { return (raw * 50 + 1) / 3; };
	if (!forcewake(true, M)) return;
	const uint32_t cap = rd(0x138020);
	say("media: RP0 %u MHz, RPn %u MHz, RPa %u MHz, RPe %u MHz, requested %u MHz; act:", mhz(cap & 0x1ff),
		mhz(cap >> 16 & 0x1ff), mhz(rd(0x138028) & 0x1ff), mhz(rd(0x13802c) & 0x1ff), mhz(rd(M + 0xa008) >> 23 & 0x1ff));
	for (int i = 0; i < 5; i++) {
		say(" %u", mhz(rd(M + 0xc60) & 0x1ff));
		IOSleep(10);
	}
	uint32_t state = 0;
	if (mSlpcMem) {
		flushRange(mSlpcMem->getBytesNoCopy(), 64);
		state = static_cast<volatile uint32_t *>(mSlpcMem->getBytesNoCopy())[1];
	}
	say(" MHz; media GuC %s, SLPC %s (state %u), HuC load info 0x%08x\n", mCtOn ? "up" : "down", mSlpcOn ? "on" : "off",
		state, rd(M + HUC_KERNEL_LOAD_INFO));
	forcewake(false, M);
}

// First video queue: the media GuC comes up here (or by step 14), never at bring-up. Callers hold at most a client
// lock (gDevLock comes after it).
// The media GT on demand when bring-up or resume did not bring it up; after a failure, again only 10 s later (the engine
// and GT queries ask on every call).
static uint64_t gMediaFailedAt;
bool IntelXe::mediaUp() {
	if (mCtOn) return true;
	uint64_t now;
	absolutetime_to_nanoseconds(mach_absolute_time(), &now);
	if (gMediaFailedAt && now - gMediaFailedAt < 10000000000ull) return false;
	IOLockLock(gDevLock);
	int err = 0;
	if (!mCtOn) {
		say("media bring-up\n");
		err = mediaGucLoad();
		if (err) {
			say("media bring-up failed: %d\n", err);
			gMediaFailedAt = now;
		}
	}
	IOLockUnlock(gDevLock);
	return !err && mCtOn;
}

// First media batch, as step 6 on BCS0: an LRC per video decode engine (16 KB ring, PPHWSP, xe2_xcs_offsets image at
// the engine's base), REGISTER_CONTEXT on GUC_VIDEO_CLASS with the engine's logical mask, SCHED_CONTEXT_MODE_SET, poll
// a GGTT dword the ring writes with MI_STORE_DATA_IMM. Context ids 0/1 in the media GuC's own id space.
int IntelXe::mediaFirstBatch() {
	constexpr uint32_t M = MEDIA_GT_GSI_OFFSET, stride = ringSize + lrcSize;
	if (!mCtOn) {
		say("media GuC not up: run step 14 first\n");
		return ENXIO;
	}
	if (!mLrcMem) mLrcMem = alloc(2 * stride, false);
	if (!mLrcMem) return ENOMEM;
	if (!forcewake(true, M)) return ETIMEDOUT;
	auto *l = static_cast<uint8_t *>(mLrcMem->getBytesNoCopy());
	bzero(l, mLrcMem->getLength());
	auto *target = reinterpret_cast<volatile uint32_t *>(static_cast<uint8_t *>(mMiscMem->getBytesNoCopy()) + mTargetOff);
	static const uint32_t vcsBase[8] = {0x1c0000, 0x1c4000, 0x1d0000, 0x1d4000, 0x1e0000, 0x1e4000, 0x1f0000, 0x1f4000};
	uint32_t phys[2], n = 0;
	for (uint32_t i = 0; i < 8 && n < 2; i++)
		if (mVdbox & 1u << i) phys[n++] = i;
	for (uint32_t k = 0; k < n; k++) {
		auto *ring = reinterpret_cast<uint32_t *>(l + k * stride);
		auto *regs = reinterpret_cast<uint32_t *>(l + k * stride + ringSize + 0x1000);
		lrcSetOffsets(regs, xe2_xcs_offsets, vcsBase[phys[k]]);
		regs[CTX_CONTEXT_CONTROL] = (1u << 3 | 1u) << 16 | (1u << 3 | 1u);   // INHIBIT_SYN_CTX_SWITCH, ENGINE_CTX_RESTORE_INHIBIT
		regs[CTX_RING_START] = mLrcG + k * stride;
		regs[CTX_RING_CTL] = (ringSize - 0x1000) | 1;
		uint32_t t = 0;
		ring[t++] = MI_ARB_ON_OFF | MI_ARB_ENABLE;
		ring[t++] = MI_STORE_DATA_IMM | MI_SDI_GGTT | MI_SDI_NUM_DW(1);
		ring[t++] = mMiscG + mTargetOff + 4 * k;
		ring[t++] = 0;
		ring[t++] = 0xb580dec0 | phys[k];
		while (t & 1) ring[t++] = MI_NOOP;
		regs[CTX_RING_TAIL] = t * 4;
		target[k] = 0x11111111;
	}
	flushRange(l, mLrcMem->getLength());
	flushRange(const_cast<uint32_t *>(target), 8);
	int err = ggttMap(mLrcMem, mLrcG, M);
	if (err) {
		forcewake(false, M);
		return err;
	}
	for (uint32_t k = 0; k < n; k++) {
		const uint32_t lrca = mLrcG + k * stride + ringSize, desc = lrca | 1u | 3u << 3;   // LRC_VALID, LEGACY_64B_CONTEXT
		say("VCS%u: LRC 0x%x descriptor 0x%08x, target 0x%x\n", phys[k], lrca, desc, mMiscG + mTargetOff + 4 * k);
		// register_exec_queue + submit_exec_queue: KMD normal context k, GUC_VIDEO_CLASS, logical instance k.
		uint32_t reg[12] = {0x4502, 1, k, 1, 1u << k, 0, 0, 0, 0, 0, desc, 0};
		ctSend(reg, 12, M);
		uint32_t en[3] = {0x1001, k, 1};
		ctSend(en, 3, M);
		uint32_t v = 0, ms = 0;
		for (; ms <= 2000; ms += 1) {
			flushRange(const_cast<uint32_t *>(target + k), 4);
			if ((v = target[k]) == (0xb580dec0 | phys[k])) break;
			IOSleep(1);
		}
		const bool ok = v == (0xb580dec0 | phys[k]);
		say("VCS%u target after %u ms: 0x%08x -> %s\n", phys[k], ms, v, ok ? "THE VIDEO ENGINE EXECUTED OUR COMMAND" : "not written");
		if (!ok && !err) err = EIO;
	}
	ctDrain(M);
	mediaEngineWake(true);
	for (uint32_t k = 0; k < n; k++) {
		const uint32_t b = vcsBase[phys[k]];
		say("VCS%u: head 0x%x tail 0x%x start 0x%x ctl 0x%x acthd 0x%x hws 0x%x mi_mode 0x%x lrca 0x%x eir 0x%x esr 0x%x\n",
			phys[k], rd(b + 0x34), rd(b + 0x30), rd(b + 0x38), rd(b + 0x3c), rd(b + 0x74), rd(b + 0x80), rd(b + 0x9c),
			rd(b + 0x240), rd(b + 0xb0), rd(b + 0xb8));
	}
	mediaEngineWake(false);
	gucStatus(M);
	forcewake(false, M);
	return err;
}

// ─── interrupts (xe_irq.c, dGFX path) ───────────────────────────────────────────────────────────────────────────
static constexpr uint32_t DG1_MSTR_TILE_INTR = 0x190008, GFX_MSTR_IRQ = 0x190010, GT_INTR_DW0 = 0x190018,
	RENDER_COPY_INTR_ENABLE = 0x190030, GUC_SG_INTR_ENABLE = 0x190038, CCS_RSVD_INTR_ENABLE = 0x190048,
	INTR_IDENTITY_REG0 = 0x190060, IIR_REG_SELECTOR0 = 0x190070, RCS0_RSVD_INTR_MASK = 0x190090,
	BCS_RSVD_INTR_MASK = 0x1900a0, GUC_SG_INTR_MASK = 0x1900e8, CCS0_CCS1_INTR_MASK = 0x190100;

// The first message-signalled interrupt of the device on our own work loop, and the top-level enable (dg1_intr_enable).
// Done at bring-up, or earlier when IntelXeFB registers its display handler: display interrupts need no GT.
bool IntelXe::irqInit() {
	if (irqOn) return true;
	if (!mapBar0()) return false;
	int index = -1, type = 0;
	for (int i = 0; pci->getInterruptType(i, &type) == kIOReturnSuccess; i++)
		if (type & kIOInterruptTypePCIMessaged) {
			index = i;
			break;
		}
	if (index < 0) {
		say("no MSI interrupt\n");
		return false;
	}
	workLoop = IOWorkLoop::workLoop();
	irqSource = workLoop ? IOInterruptEventSource::interruptEventSource(this,
		[](OSObject *o, IOInterruptEventSource *, int) { static_cast<IntelXe *>(o)->irqHandle(); }, pci, index) : nullptr;
	if (!irqSource || workLoop->addEventSource(irqSource) != kIOReturnSuccess) {
		OSSafeReleaseNULL(irqSource);
		OSSafeReleaseNULL(workLoop);
		say("interrupt source failed\n");
		return false;
	}
	irqOn = true;
	irqSource->enable();
	wr(DG1_MSTR_TILE_INTR, 1u << 31);
	(void)rd(DG1_MSTR_TILE_INTR);
	say("MSI interrupt index %d enabled\n", index);
	return true;
}

// xe_irq_enable_hwe (GT_MI_USER_INTERRUPT | GT_FLUSH_COMPLETE_INTERRUPT per engine class, unmasked per instance) and
// guc_enable_irq (GUC2HOST: the shared enable, the primary GuC's mask bit in ENGINE1).
void IntelXe::irqGtEnable() {
	const uint32_t common = 1u << 0 | 1u << 4;
	wr(RENDER_COPY_INTR_ENABLE, common << 16 | common);
	wr(CCS_RSVD_INTR_ENABLE, common << 16);
	wr(RCS0_RSVD_INTR_MASK, ~(common << 16));
	wr(BCS_RSVD_INTR_MASK, ~(common << 16));
	wr(CCS0_CCS1_INTR_MASK, ~(common << 16 | common));
	wr(GUC_SG_INTR_ENABLE, (1u << 15) << 16);
	wr(GUC_SG_INTR_MASK, rd(GUC_SG_INTR_MASK) & ~((1u << 15) << 16));
	if (mCtOn) mediaIrqEnable();
}

void IntelXe::irqTeardown() {
	if (bar0 && irqOn) wr(DG1_MSTR_TILE_INTR, 0);
	irqOn = false;
	if (irqSource) {
		irqSource->disable();
		if (workLoop) workLoop->removeEventSource(irqSource);
	}
	OSSafeReleaseNULL(irqSource);
	OSSafeReleaseNULL(workLoop);
}

// dg1_irq_handler + gt_irq_handler: disable and ack the tile master, ack GFX_MSTR_IRQ, read each pending GT_INTR_DW
// bank, fetch and clear every identity through IIR_REG_SELECTOR / INTR_IDENTITY_REG, ack the bank; display interrupts
// (GFX_MSTR_IRQ DISPLAY_IRQ, bit 16) go to IntelXeFB's handler, which owns every display interrupt register; re-enable.
// Then G2H of each GuC that raised GUC2HOST (class OTHER, instance 0 primary / 16 media: gt_other_irq_handler) is read,
// and engine interrupts wake the waiters and the submit worker. Runs on our work loop thread.
bool IntelXe::irqPending() { return bar0 && irqOn && (rd(DG1_MSTR_TILE_INTR) & 0xf); }

void IntelXe::irqHandle() {
	if (!bar0 || !irqOn) return;
	irqActive = 1;
	wr(DG1_MSTR_TILE_INTR, 0);
	const uint32_t tile = rd(DG1_MSTR_TILE_INTR);
	if (!tile) {
		wr(DG1_MSTR_TILE_INTR, 1u << 31);
		irqActive = 0;
		return;
	}
	wr(DG1_MSTR_TILE_INTR, tile);
	bool gtEvent = false, g2h[2] = {};
	if (tile & 1) {
		const uint32_t master = rd(GFX_MSTR_IRQ);
		if (master == ~0u) {   // device inaccessible (PCIe DPC): leave it off, as Linux does
			irqActive = 0;
			return;
		}
		wr(GFX_MSTR_IRQ, master);
		for (uint32_t bank = 0; bank < 2; bank++) {
			if (!(master & 1u << bank)) continue;
			const uint32_t dw = rd(GT_INTR_DW0 + 4 * bank);
			for (uint32_t bit = 0; bit < 32; bit++) {
				if (!(dw & 1u << bit)) continue;
				wr(IIR_REG_SELECTOR0 + 4 * bank, 1u << bit);
				uint32_t ident = 0;
				for (int t = 0; t < 100 && !((ident = rd(INTR_IDENTITY_REG0 + 4 * bank)) & 1u << 31); t++) IODelay(1);
				if (!(ident & 1u << 31)) {   // gt_engine_identity: "INTR_IDENTITY_REG not valid"; wake anyway
					gStats.identTimeouts++;
					gtEvent = true;
					continue;
				}
				wr(INTR_IDENTITY_REG0 + 4 * bank, ident);
				const uint32_t cls = ident >> 16 & 7, inst = ident >> 20 & 0x3f, vec = ident & 0xffff;
				if (cls == 4 && (inst == 0 || inst == 16)) {
					if (vec & 1u << 15) g2h[inst ? 1 : 0] = true;   // GUC_INTR_GUC2HOST
				} else {
					// rb7o: which video engine ran the job that ended (gt_engine_identity: VIDEO_DECODE_CLASS 1,
					// VIDEO_ENHANCE_CLASS 2, physical instance; GT_RENDER_USER_INTERRUPT is vector bit 0)
					if ((cls == 1 || cls == 2) && inst < 8 && (vec & 1)) gStats.mediaUserIrqs[cls - 1][inst]++;
					gtEvent = true;
				}
			}
			wr(GT_INTR_DW0 + 4 * bank, dw);
		}
		if (master & 1u << 16) {   // DISPLAY_IRQ
			IOLockLock(gDispLock);
			if (dispIrq) {
				dispIrq(dispIrqCtx, master);
				gStats.displayIrqs++;
			}
			IOLockUnlock(gDispLock);
		}
	}
	wr(DG1_MSTR_TILE_INTR, 1u << 31);
	irqCount++;
	gStats.irqs++;
	if (gtEvent || g2h[0] || g2h[1]) gStats.gtIrqs++;
	if (g2h[0]) gucPollGt(0);
	if (g2h[1] && mCtOn) gucPollGt(MEDIA_GT_GSI_OFFSET);
	xeIrqEvent(gtEvent);
	irqActive = 0;
}

// PCIe Resizable BAR capability (ID 0x15): per BAR a capability dword (bit n set: 2^n MB supported, bits 31:4) and a
// control dword (BAR index 2:0, number of resizable BARs 7:5 in the first entry, current size 13:8 as log2 MB,
// sizes above 128 TB in 31:16).
int IntelXe::pciDump() {
	say("cmd 0x%04x status 0x%04x\n", pci->configRead16(kIOPCIConfigCommand), pci->configRead16(kIOPCIConfigStatus));
	for (int i = 0; i < 6; i++) say("BAR%d (0x%02x) 0x%08x\n", i, 0x10 + 4 * i, pci->configRead32(0x10 + 4 * i));
	IOByteCount off = 0;
	if (!pci->extendedFindPCICapability((UInt32)-0x15, &off)) {
		say("no Resizable BAR capability\n");
		return 0;
	}
	uint32_t n = (pci->extendedConfigRead32(off + 8) >> 5) & 7;
	say("Resizable BAR capability at 0x%llx, %u BAR(s)\n", (uint64_t)off, n);
	for (uint32_t i = 0; i < n && i < 6; i++) {
		uint32_t cap = pci->extendedConfigRead32(off + 4 + 8 * i), ctl = pci->extendedConfigRead32(off + 8 + 8 * i);
		say("  BAR%u: current %llu MB, supported (MB):", ctl & 7, 1ull << ((ctl >> 8) & 0x3f));
		for (int b = 4; b < 32; b++)
			if (cap & (1u << b)) say(" %llu", 1ull << (b - 4));
		say(" (cap 0x%08x ctl 0x%08x)\n", cap, ctl);
	}
	uint8_t pcie = 0;
	if (pci->findPCICapability(kIOPCIPCIExpressCapability, &pcie))
		say("PCIe link status 0x%04x (gen %u x%u)\n", pci->configRead16(pcie + 0x12), pci->configRead16(pcie + 0x12) & 0xf,
			(pci->configRead16(pcie + 0x12) >> 4) & 0x3f);
	return 0;
}

extern "C" {
static IOLock **const kLocks[] = {&gDevLock, &gH2gLock[0], &gH2gLock[1], &gG2hLock[0], &gG2hLock[1], &gFwLock, &gSayLock,
	&gDispLock, &gGgttLock, &gPmtLock, &gAspmLock, &gPcodeLock};
kern_return_t IntelXe_stop(kmod_info_t *, void *);
kern_return_t IntelXe_start(kmod_info_t *, void *) {
	bool ok = true;
	for (IOLock **l : kLocks) ok = (*l = IOLockAlloc()) && ok;
	if (ok && xeCoreInit()) return KERN_SUCCESS;
	IntelXe_stop(nullptr, nullptr);
	return KERN_FAILURE;
}
kern_return_t IntelXe_stop(kmod_info_t *, void *) {
	if (gDev) return KERN_FAILURE;
	if (!xeFdFini()) return KERN_FAILURE;   // rb7t: an object fd is still open (its d_close is ours to answer)
	xeCoreFini();
	for (IOLock **l : kLocks)
		if (*l) {
			IOLockFree(*l);
			*l = nullptr;
		}
	return KERN_SUCCESS;
}
}
