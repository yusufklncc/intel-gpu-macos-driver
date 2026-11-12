// SPDX-License-Identifier: MIT
// Shared declarations of IntelXe.kext: register offsets, the GGTT layout, the IntelXe service and the xe objects.
#pragma once
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IODMACommand.h>
#include <IOKit/IOMapper.h>
#include <IOKit/IOLib.h>
#include <IOKit/IOService.h>
#include <IOKit/IOInterruptEventSource.h>
#include <IOKit/IOWorkLoop.h>
#include <kern/thread_call.h>
#include <IOKit/pci/IOPCIDevice.h>
#include "xe_platform.h"
#include "xe_pmt.h"
#include "xe_pcode.h"   // PCODE_MAILBOX, PCODE_DATA0/1, PCODE_READY, DGFX_*, the power-limit words

#define LOG(fmt, ...) IOLog("IntelXe: " fmt "\n", ##__VA_ARGS__)

// BAR0 offsets (Linux v7.2 xe)
static constexpr uint32_t GMD_ID = 0xd8c;                 // ARCH 31:22, RELEASE 21:14, REVID 5:0
static constexpr uint32_t MEDIA_GT_GSI_OFFSET = 0x380000;
static constexpr uint32_t FORCEWAKE_ACK_RENDER = 0xd84;
static constexpr uint32_t FORCEWAKE_ACK_GT_MTL = 0xdfc;    // Xe2 GT domain ack (graphics_verx100 >= 1270)
static constexpr uint32_t GLOBAL_MOCS0 = 0x4000;           // XEHP_GLOBAL_MOCS(i), MCR, multicast write
static constexpr uint32_t PAT_ATS = 0x47fc, PAT_PTA = 0x4820;
static constexpr uint32_t GUC_STATUS = 0xc000;             // AUTH 31:30, MIA 18:16, UKERNEL 15:8, BOOTROM 7:1
static constexpr uint32_t BOOT_HASH_CHK = 0xc010, GUC_HEADER_INFO = 0xc014;
static constexpr uint32_t GUC_WOPCM_SIZE = 0xc050;         // SIZE 31:12, LOCKED bit 0
static constexpr uint32_t GUC_SHIM_CONTROL = 0xc064;
static constexpr uint32_t SOFT_SCRATCH0 = 0xc180;
static constexpr uint32_t UOS_RSA_SCRATCH0 = 0xc200;
static constexpr uint32_t DMA_ADDR_0_LOW = 0xc300, DMA_ADDR_0_HIGH = 0xc304, DMA_ADDR_1_LOW = 0xc308, DMA_ADDR_1_HIGH = 0xc30c;
static constexpr uint32_t DMA_COPY_SIZE = 0xc310, DMA_CTRL = 0xc314;
static constexpr uint32_t DMA_GUC_WOPCM_OFFSET = 0xc340;   // OFFSET 31:14, VALID bit 0
static constexpr uint32_t PVC_GUC_TLB_INV_DESC0 = 0xcf7c, PVC_GUC_TLB_INV_DESC1 = 0xcf80;
static constexpr uint32_t XELP_EU_ENABLE = 0x9134;
static constexpr uint32_t XELP_GT_GEOMETRY_DSS_ENABLE = 0x913c;
static constexpr uint32_t GT_VEBOX_VDBOX_DISABLE = 0x9140;
static constexpr uint32_t XEHP_GT_COMPUTE_DSS_ENABLE = 0x9144, XEHPC_GT_COMPUTE_DSS_ENABLE_EXT = 0x9148;
static constexpr uint32_t XE2_GT_COMPUTE_DSS_2 = 0x914c, XE2_GT_GEOMETRY_DSS_1 = 0x9150, XE2_GT_GEOMETRY_DSS_2 = 0x9154;
static constexpr uint32_t PMINTRMSK = 0xa168;              // ARAT_EXPIRED_INTRMSK bit 9
static constexpr uint32_t FORCEWAKE_GT = 0xa188;
static constexpr uint32_t FORCEWAKE_RENDER = 0xa278;   // xe_force_wake: XE_FW_RENDER (ack FORCEWAKE_ACK_RENDER 0xd84)
static constexpr uint32_t GT_PM_CONFIG = 0x13816c;         // GT_DOORBELL_ENABLE bit 0
static constexpr uint32_t GU_CNTL = 0x101010;              // LMEM_INIT bit 7
static constexpr uint32_t GU_DEBUG = 0x101018;
static constexpr uint32_t XEHP_MTCFG_ADDR = 0x101800;      // TILE_COUNT 15:8
static constexpr uint32_t DSMBASE = 0x1080c0;
static constexpr uint32_t GSMBASE = 0x108100;
static constexpr uint32_t STOLEN_RESERVED = 0x1082c0;
static constexpr uint32_t SG_TILE_ADDR_RANGE0 = 0x1083a0;  // size GB 17:8, offset GB 7:1
static constexpr uint32_t FORCEWAKE_ACK_GT = 0x130044;
static constexpr uint32_t BMG_FAN_1_SPEED = 0x138140;
static constexpr uint32_t BMG_VRAM_TEMPERATURE = 0x1382c0;
static constexpr uint32_t PCODE_SCRATCH0 = 0x138320;       // BOOT_STATUS 3:1 (4 critical, 7 non-critical failure)
static constexpr uint32_t BMG_PCIE_CAP = 0x138340;
static constexpr uint32_t BMG_PACKAGE_TEMPERATURE = 0x138434;

struct XeClientState;
struct IntelXeDpaRun { uint64_t dpa, len; };   // intelxeScanoutPin v2: one physically contiguous piece of a scan-out range

// Device addresses of system memory the GPU reads or writes (dma_map_sgtable in xe: the GGTT objects, system BOs, userptr
// imports, sleep backups): an IODMACommand with the system mapper (VT-d), prepared for the descriptor's whole length.
// Without a mapper (OpenCore DisableIoMapper) the segments are the physical addresses, as before. Up to 46-bit device
// addresses (Xe2 dGPU dma_mask_size).
extern IOMapper *gDmaMapper;   // the B580's own mapper (IOMapper::copyMapperForDevice), nullptr = the system default
struct DmaMap {
	IODMACommand *cmd {nullptr};
	IODMACommand::Segment64 *seg {nullptr};
	uint64_t *start {nullptr};   // BO offset of each segment
	uint32_t n {0}, cap {0};
	bool map(IOMemoryDescriptor *md);
	void unmap();
	// The device address of offset `off` and the bytes contiguous from it; false outside the mapping.
	bool lookup(uint64_t off, uint64_t *addr, uint64_t *run) const;
};

class IntelXe : public IOService {
	OSDeclareDefaultStructors(IntelXe)
public:
	bool start(IOService *provider) override;
	void stop(IOService *provider) override;
	bool terminate(IOOptionBits options = 0) override;   // rb7t: refused while object fds (/dev/intelxeobj minors) are open
	IOReturn setPowerState(unsigned long ordinal, IOService *device) override;   // xe_pm_suspend / xe_pm_resume
	void systemWillShutdown(IOOptionBits specifier) override;
	int runStep(int step);
private:
	IOPCIDevice *pci {nullptr};
	IOMemoryMap *bar0 {nullptr}, *bar2 {nullptr};
	IOBufferMemoryDescriptor *fwMem {nullptr}, *adsMem {nullptr}, *ctMem {nullptr}, *miscMem {nullptr}, *lrcMem {nullptr};
	IOBufferMemoryDescriptor *lrc1Mem {nullptr}, *sysPage {nullptr};
	uint32_t h2gTail {0}, g2hHead {0}, fence {0};
	uint32_t fwCount[2] {}, mediaEngCount {0};   // forcewake references (gFwLock): primary/media GT, media engines
	uint32_t ringTail {0};        // bytes; nonzero once step 6 has registered and enabled context 0
	// The standalone media GT (gt1, steps 14/15): its own GuC with its own ADS, CT, HWSPs and test LRC in the shared GGTT.
	IOBufferMemoryDescriptor *mAdsMem {nullptr}, *mCtMem {nullptr}, *mMiscMem {nullptr}, *mLrcMem {nullptr};
	uint32_t mH2gTail {0}, mG2hHead {0}, mFence {0};
	uint32_t mVdbox {0}, mVebox {0};   // enabled physical instances (GT_VEBOX_VDBOX_DISABLE on the media GT)
	bool mCtOn {false};
public:
	void mediaDown() { mCtOn = mHucOn = mSlpcOn = false; }   // after suspend: reloaded by mediaUp on next use
	int sensorsString(char *buf, size_t len);   // debug.intelxe.sensors: GT clock, temperatures, energy and power
	XeEnergyMeter energy;   // card and package energy sums and average power (xe_pmt.h), under gPmtLock
	// xe_pcode.c xe_pcode_read: one mailbox read under gPcodeLock, PCODE_DEFAULT_TIMEOUT_MS; ENODATA while asleep
	int pcodeRead(uint32_t mbox, uint32_t *val0, uint32_t *val1);
	void powerLimitsRead(XePowerLimits *out);   // the power limits for debug.intelxe.sensors (cached 1 s), read only
private:
	IOBufferMemoryDescriptor *mHucMem {nullptr}, *mSlpcMem {nullptr};
	bool mHucOn {false}, mSlpcOn {false};
	uint32_t rd(uint32_t off) { return *reinterpret_cast<volatile uint32_t *>(bar0->getVirtualAddress() + off); }
	void wr(uint32_t off, uint32_t v) { *reinterpret_cast<volatile uint32_t *>(bar0->getVirtualAddress() + off) = v; }
	bool waitFor(uint32_t off, uint32_t mask, uint32_t val, uint32_t us, uint32_t *last = nullptr);
	bool mapBar0();
	int probe();
	int pcodeInitStatus();         // takes gPcodeLock
	int pcodeInitStatusLocked();   // caller holds gPcodeLock
	int pcodeReadLocked(uint32_t mbox, uint32_t *val0, uint32_t *val1);   // caller holds gPcodeLock
	XePowerLimits plCache;   // powerLimitsRead's last reading and its time, under gPcodeLock
	uint64_t plNs {0};
	bool plLogged {false};
	int pmtTelemRead(uint32_t guid, uint32_t userOffset, uint64_t *data);   // xe_vsec.c xe_pmt_telem_read, 8 bytes
	int waitDeviceReady();
	void publishVram();
	// gt: register base of the GT (0 primary, MEDIA_GT_GSI_OFFSET media); applies to registers below 0x40000 only.
	bool forcewake(bool on, uint32_t gt = 0);
	void forcewakeRestore();   // resume: request again the forcewake domains still referenced
	void ggttInvalidate(uint32_t gt);   // GGTT TLB over MMIO, forcewake held
	int readGt();
	void gucStatus(uint32_t gt = 0);
	void programPatMocs(uint32_t gt = 0);
	int ggttMap(IOBufferMemoryDescriptor *mem, uint32_t ggtt, uint32_t gt = 0);
	int ggttMapVram(uint64_t dpa, uint32_t len, uint32_t ggtt, uint32_t gt = 0, bool uc = false);   // rb7: LRCs in VRAM
	IOBufferMemoryDescriptor *alloc(size_t len, bool contiguous);
	bool gucReset(uint32_t gt = 0);
	int loadGuc(bool full, uint32_t gt = 0);
	int mmioSend(const uint32_t *req, uint32_t len, uint32_t *reply, uint32_t gt = 0);
	int ctSetup(uint32_t gt = 0);
	int ctSend(const uint32_t *action, uint32_t len, uint32_t gt = 0, bool request = false);
	void adsRegsets(void *adsBlob, uint8_t *area, uint32_t areaG, uint32_t areaLen, uint32_t gt);   // GuC engine-reset save/restore lists
	// rb7p: what Linux xe's hw_engine_init sets per engine and rb7o lacked (IntelXe.cpp, before firstBatchSetupOnly)
	uint32_t engineNonpriv(uint16_t drmClass, uint32_t *slots);   // xe_reg_whitelist.c: FORCE_TO_NONPRIV values in slot order
	void engineExtras(uint32_t base, uint16_t drmClass);          // NONPRIV slots, FF_SLICE_CS_CHICKEN1 (render)
	bool dssGroupsDiscontiguous(uint32_t *perGroup = nullptr);    // xe_gt_has_discontiguous_dss_groups (18041344222)
	void adjustIdledly(uint32_t gt, uint32_t base, const char *name, uint32_t inst, bool record);   // xe_hw_engine.c adjust_idledly
	void adjustIdledlyAll(uint32_t gt, bool record);              // every engine of one GT (forcewake / media wake held)
	int engineStateReport();   // step 21: IDLEDLY/MAXCNT, NONPRIV, PSMI_CTL ... of every engine, read only
	int idledlyStep();         // step 22: adjust_idledly once more on every engine that is up (test lever)
	void ctDrain(uint32_t gt = 0);
	bool mediaEngineWake(bool on);
	int mediaGucLoad();
	int mediaFirstBatch();
	void gucPollGt(uint32_t gt);
	void mediaIrqEnable();
	int hucUpload();
	void hucAuth();
	void mediaPcStart();
	void mediaFreqReport();
	int firstBatch();
	int firstBatchSetupOnly();
	int pciDump();
	int submit(const uint32_t *cmd, uint32_t n, volatile uint32_t *done, uint32_t value, uint32_t ms);
	int vramTest();
	int ppgttTest();
public:
	// xe_core.cpp: the device behind the user client (bringUp with gDevLock held)
	int bringUp();
	void pcStart();        // SLPC start and parameters, GuC RC (xe_guc_pc_start, xe_guc_rc_enable): bring-up and GT reset
	int gtReset();         // xe_gt.c gt_reset_worker: GDRST full, then the GT, GuC and CT set up again (gDevLock)
	void gtStop();         // xe_gt_shutdown: GDRST full only (gDevLock)
	void gpuSuspend();     // xe_gt_suspend: GuC CLIENT_SOFT_RESET, interrupts off (gDevLock)
	int gpuResume();       // xe_pm_resume: ReBAR, pcode, GGTT, GT/GuC/CT/SLPC, interrupts (gDevLock)
	// "intelxePowerEvent" (IntelXeClient.cpp): IntelXeFB's handler for 1 will suspend, 2 resumed, 3 GT reset begins, 4 GT reset
	// done; called on IntelXe's own threads, under pwrLock only (recursive: the handler may unregister itself).
	void (*pwrHandler)(void *ctx, uint32_t event) {nullptr};
	void *pwrCtx {nullptr};
	IORecursiveLock *pwrLock {nullptr};
	bool pwrSuspendSent {false};
	void powerEvent(uint32_t event);
	bool suspended {false};
	uint32_t rebarOff {0}, rebarCtl[6] {}, bar2Lo {0}, bar2Hi {0}, rebarN {0};   // pci_save_rebar_state
	void bringUpAsync();   // bring-up on a thread call after start() (xe_device_probe), -intelxe_lazy: on the first ioctl
	uint64_t vramUsableEnd();
	bool ctBroken[2] {};   // G2H channel found corrupted (status or offsets): no more reads until the next CT setup
	// H2G made no forward progress for 1 s (guc_ct_send_locked "No forward process on H2G": CT_DEAD(DEADLOCK)): every
	// send fails at once until the next CT setup, and the primary GT is reset (kick_reset)
	volatile bool h2gDead[2] {};
	void schedPolicyInit();   // guc_init_global_schedule_policy (xe_guc_submit_enable): after each primary GuC load
	bool up {false};
	uint16_t devId {0}, revId {0};
	IOPCIDevice *pciDevice() { return pci; }
	IOMemoryMap *vramMap() { return bar2; }
	uint32_t mmio(uint32_t off) { return rd(off); }
	void mmioWrite(uint32_t off, uint32_t v) { wr(off, v); }
	uint32_t mmioRead(uint32_t off) { return rd(off); }
	int guc(const uint32_t *action, uint32_t len, uint32_t gt = 0) { return ctSend(action, len, gt); }
	void gucPoll();
	// GT power management (xe_core.cpp): SLPC parameters, GuC RC mode, host RC6 control, a GT forcewake hold
	void slpcSet(uint32_t id, uint32_t value);
	void slpcParams();
	void gucRcSetup(uint32_t mode);
	void rc6Control(int on);
	bool fwHold(bool on);
	int slpcStep();
	int powerReport();
	volatile uint32_t tlbDone {0}, tlbSeqno {0}, mTlbDone {0};   // TLB invalidation seqnos: done per GuC, last issued
	int ggttMapMem(IOBufferMemoryDescriptor *mem, uint32_t ggtt, uint32_t gt = 0) { return ggttMap(mem, ggtt, gt); }
	int ggttMapLrc(uint64_t dpa, uint32_t len, uint32_t ggtt, uint32_t gt = 0) { return ggttMapVram(dpa, len, ggtt, gt); }
	// xe_device_wmb on dGFX: the CPU's write-combined stores to VRAM through BAR2 are pushed out (sfence) and ordered
	// before what follows by a posted MMIO write (VF_CAP_REG 0x1901f8, which needs no forcewake), as xe does after
	// writing a ring or a CT buffer in VRAM (xe_lrc_write_ring, h2g_write).
	void deviceWmb() { asm volatile("sfence" : : : "memory"); wr(0x1901f8, 0); }
	// xe_device_l2_flush (Wa_16023588340): XE2_GLOBAL_INVAL under GT forcewake, waited up to 1 ms; false on timeout
	bool l2Flush();
	void ggttClear(uint32_t ggtt, uint32_t len, uint32_t gt = 0);
	IOBufferMemoryDescriptor *allocSys(size_t len, bool contiguous) { return alloc(len, contiguous); }
	bool fwOn(bool on) { return forcewake(on); }
	// Media GT for video queues: loaded on first use (mediaUp), GT + VDBOX/VEBOX forcewake (mediaWake, counted).
	bool mediaOn() { return mCtOn; }
	bool hucRunning() { return mHucOn; }
	uint32_t hucVer {0};   // CSS sw_version of the uploaded HuC (major 23:16, minor 15:8, patch 7:0)
	bool mediaUp();
	bool mediaWake(bool on);
	// Interrupts (MSI, xe_irq.c dg1_irq_handler): engine user interrupts wake the waiters, GuC-to-host interrupts of
	// either GuC have their G2H read here, display interrupts go to IntelXeFB's handler ("intelxeDisplayIrq").
	uint8_t *miscBytes() { return static_cast<uint8_t *>(miscMem->getBytesNoCopy()); }
	bool irqInit();        // MSI source and the tile master enable: at bring-up, or when IntelXeFB registers
	void irqGtEnable();    // engine and GuC interrupt enables (after the GuC load)
	void irqTeardown();
	void irqHandle();
	IOWorkLoop *workLoop {nullptr};
	IOInterruptEventSource *irqSource {nullptr};
	volatile bool irqOn {false};
	volatile uint32_t irqActive {0};   // irqHandle running (set before the first acknowledge, cleared after the wakeups)
	bool irqPending();                 // DG1_MSTR_TILE_INTR shows a tile interrupt not yet taken by irqHandle
	uint32_t irqCount {0};
	thread_call_t upCall {nullptr};
	IONotifier *powerNotifier {nullptr};   // system power-off / restart: quiesce (xe_device_shutdown)
	static IOReturn powerMessage(void *target, void *refCon, UInt32 type, IOService *provider, void *arg, vm_size_t argSize);
	void (*dispIrq)(void *ctx, uint32_t gfxMstrIrq) {nullptr};   // gDispLock
	void *dispIrqCtx {nullptr};
	IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) override;
	// IntelXeClient.cpp: scan-out pins and the L2 flush for NMXAccel's flips (frame path stage 3)
	IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) override;
};

extern IntelXe *gDev;
// The platform descriptor of the device IntelXe started on (xe_platform.h; xe->info as xe_pci_probe fills it), and that
// device's id. Set once in IntelXe::start before anything else runs, never cleared: only supported entries get here.
extern const XePlatformDesc *gXePlat;
extern uint16_t gXePlatId;
static inline IOMemoryMap *gDevVram() { return gDev->vramMap(); }


// GGTT layout above the 8 MB WOPCM (bring-up objects), then a 16 MB window per exec queue LRC from lrcPoolG.
static constexpr uint32_t fwG = 0x800000, adsG = 0x900000, ctG = 0x1400000, miscG = 0x1500000, lrcG = 0x1600000;
static constexpr uint32_t hwspOff = 0, targetOff = 0x1000;                             // inside misc
static constexpr uint32_t ringSize = 0x4000, lrcSize = 0x2000;                         // copy class: PPHWSP + 1 page
// Misc pages: BCS0 HWSP at 0, test target at 0x1000, per-engine HWSPs from 0x2000 (RCS0, CCS0-3), zeroed golden
// contexts per GuC class (xe_gt_lrc_size: copy 8 KB, render 16 KB, compute 12 KB on Xe2).
static constexpr uint32_t goldenOff = 0x8000, goldenRcsOff = 0xa000, goldenCcsOff = 0xe000, miscSize = 0x11000;

// The primary GT's engines (xe_hw_engine.c engine_infos) this driver drives.
struct XeEngine {
	uint16_t drmClass, logical, phys, gucClass;
	uint32_t base, lrcSize, hwsp;
	uint32_t gt;   // register base of its GT: 0, or MEDIA_GT_GSI_OFFSET for the media GT's engines (own GuC)
};
extern XeEngine gEngines[8];
extern uint32_t gNumEngines;
// The media GT's video decode engines (VCS, DRM class 2) as the uAPI exposes them with gt_id 1; filled by
// mediaGucLoad. Listed only with debug.intelxe.video=1 (ANV never uses VECS).
extern XeEngine gMediaEngines[4];
extern uint32_t gNumMediaEngines;
extern int gVideoWanted;
// rb7j: the GuC's unified-memory (UM) queues on the primary GT (xe_guc_ads.c guc_um_init_params, has_usm on Xe2): page
// fault, page fault response and access counter, 64 KB each (GUC_UM_QUEUE_SIZE), in VRAM at gUmDpa (the top of the usable
// VRAM, kept out of the BO allocator) and in the GGTT at umG, uncached (XE_BO_FLAG_NEEDS_UC). With them the GuC reports a
// GPU page fault as G2H REPORT_PAGE_FAULT_REQ_DESC (address, ASID, access) instead of a bare MEMORY_CAT_ERROR.
// gUsm: boot-arg -intelxe_nousm = 0 (no UM queues; a fault is then a CAT error as before rb7j); read-only after boot.
extern int gUsm;
extern uint64_t gUmDpa;
static constexpr uint32_t umG = 0xa800000, kUmQueueBytes = 0x10000, kUmBytes = 3 * kUmQueueBytes;
// The UM queues' DPA for a usable VRAM end: its last 192 KB, 64 KB aligned (0: VRAM too small, no UM queues).
static inline uint64_t umDpaFor(uint64_t vramUsableEnd) {
	return vramUsableEnd > kUmBytes + (64ull << 20) ? (vramUsableEnd - kUmBytes) & ~0xffffull : 0;
}
extern int gDeferAsync;
static constexpr uint32_t lrcPoolG = 0x2000000, seqnoG = 0x1f00000;
// Media GuC objects, past the LRC pool (lrcPoolG + 4096 slots x 32 KB = 0xa000000): ADS (+9 MB private data), CT,
// misc (VCS0/VCS2/VECS0/VECS1 HWSPs, test target, zeroed golden contexts for the video and video-enhance classes,
// xe_gt_lrc_size 8 KB each on Xe2), test LRCs (VCS0, VCS2).
static constexpr uint32_t mAdsG = 0xb000000, mCtG = 0xc000000, mMiscG = 0xc100000, mLrcG = 0xc200000;
static constexpr uint32_t mTargetOff = 0x4000, mGoldenVcsOff = 0x6000, mGoldenVecsOff = 0x8000, mMiscSize = 0xa000;
static constexpr uint32_t MED_GUC_HOST_INTERRUPT = 0x190304, MED_VF_SW_FLAG0 = 0x190310;   // absolute (>= 0x40000)
// Media SLPC shared data (8 KB) and the HuC image (whole xe/bmg_huc.bin, GSC CPD layout, ~572 KB).
static constexpr uint32_t mSlpcG = 0xc300000, mHucG = 0xc400000;
static constexpr uint32_t HUC_KERNEL_LOAD_INFO = 0xc1dc;   // HUC_LOAD_SUCCESSFUL bit 0 (media GT: +0x380000)
extern int gMediaSlpc;
extern int gSlpcMin, gSlpcMax, gSlpcIgnoreEff, gSlpcStrategy, gSlpcProfile, gSlpcParamId, gSlpcParamVal;
extern int gGucRc, gRc6Ctl, gFwHold, gMmioAddr, gRangeInval, gHugePages, gCtxPolicies, gJobTimeoutMs, gRcYield;
// rb7o: debug.intelxe.vcs_single (boot-arg -intelxe_vcssingle): 1 = new video queues on their first placement only (before rb7o)
extern int gVcsSingle;
extern int gTlbEveryBatch;   // rb7k
extern int gJobTime;         // debug.intelxe.jobtime (xe_core.cpp)
int xeJobTimeSet(int on);
int xeJobTimeString(char *b, size_t len, int fwHold);
extern int gDirect, gDirectIdleUs;   // debug.intelxe.direct, direct_idle_us (xe_core.cpp, direct submission)
int xeDirectSet(int on);
// rb7p: boot-arg -intelxe_nowhitelist: 0 = no RING_FORCE_TO_NONPRIV programming (as rb7o); 1 = xe_reg_whitelist.c (default)
extern int gNonpriv;

// Locks (IntelXe_start). gDevLock: bring-up, media bring-up, debug steps. gH2gLock/gG2hLock: the two directions of each
// GuC's CT (primary 0, media 1). gFwLock: forcewake reference counts. gSayLock: the step report and G2H log. gDispLock:
// IntelXeFB's display interrupt handler. Order and the rest: xe_core.cpp.
extern IOLock *gDevLock, *gH2gLock[2], *gG2hLock[2], *gFwLock, *gSayLock, *gDispLock;
extern int gIrqWanted;
// debug.intelxe.stats
struct IntelXeStats {
	uint32_t irqs, gtIrqs, displayIrqs, g2h[2], h2gFailed, h2gFull, fallbackWakes, rescued, jobsDirect, jobsDeferred,
		workerRuns, bans, queuesFreed, queuesLeaked, tlbInvals, tlbTimeouts, deferredFrees, identTimeouts, tlbRingFlushes, clears, tlbRanged;
	// A wait whose 2 ms safety slice ran out and that then found its condition true: raced = an interrupt was pending
	// in the hardware or being handled (its wakeup was on the way), g2hFound = G2H the fallback read itself completed
	// it, rescued = neither (a missed interrupt), by wait kind (WaitKind in xe_core.cpp) in missedBy.
	uint32_t raced, g2hFound, missedBy[10];
	uint32_t gtResets, tdrChecks, tdrRearms, tdrNotStarted;
	uint32_t resetBans, resetReplays, gucCtxResets, h2gDead;   // GT reset outcome per queue; G2H CONTEXT_RESET/CAT_ERROR
	uint32_t pageFaults, pageFaultsLost;   // rb7j: G2H REPORT_PAGE_FAULT_REQ_DESC answered; ring full (not answered)
	uint32_t sameQueueDeps, watchdogBans, surfAttach, surfImport, surfFull;
	uint32_t movesIn, movesOut, moveFails, moveBusy, evictScans;   // BO moves into / out of the BAR window (xe_core.cpp boMove)
	uint64_t movedBytes;
	// BAR window pressure (xe_core.cpp visAlloc, ptPageAlloc, GEM_CREATE): user allocations the window reserve turned
	// away, page-table pages that could not be allocated, BOs placed in system memory because the window (winSpills)
	// or all of VRAM (vramSpills) was full, CPU maps handed out / torn down by IOConnectUnmapMemory64 (XeCpuMap)
	uint32_t reserveHits, ptFails, winSpills, vramSpills, cpuMapsMade, cpuMapsGone;
	uint32_t mapSpills, mapSpillFails;   // CPU maps of BOs outside a full window: moved to system memory instead (boMoveSys)
	uint64_t winSpillBytes;
	// rb7o: video exec queues created with more than one engine in their logical mask (DRM_XE_EXEC_QUEUE_CREATE
	// placements, the GuC picks the engine) and with one (debug.intelxe.vcs_single, or one placement asked); user
	// interrupts (MI_USER_INTERRUPT, INTR_IDENTITY vector bit 0) per physical engine of the media GT: [0] VCSn, [1] VECSn
	uint32_t vqMulti, vqSingle;
	uint32_t mediaUserIrqs[2][8];
	// direct submission (debug.intelxe.direct): jobs put in an armed slot, stretches started, and the stretches that ended
	// idle, because another queue used the engine class, or otherwise (FIFO, suspend, reset, close, ban, switch off, ring)
	uint32_t dsSubmits, dsArms, dsIdleExits, dsContendExits, dsOtherExits;
	// rb8c: deferred frees whose TLB invalidation had completed while a job of their VM (sealed with them) still ran
	uint32_t deferJobHeld;
};
extern IntelXeStats gStats;
// The last 64 H2G actions per GuC by CT fence (ctSend), to name the request a G2H failure reply refers to.
struct H2gHistory { uint16_t fence[64]; uint16_t action[64]; uint32_t next; };
extern H2gHistory gH2gHist[2];
bool xeCoreInit();
void xeCoreFini();
int xeStatsString(char *out, size_t len);
int xeVcsString(char *out, size_t len);       // rb7o debug.intelxe.vcs: video queues, their masks and engines
int xeVcsBusyString(char *out, size_t len);   // rb7o debug.intelxe.vcs_busy: per-engine busy sampling (root)
int xeVramString(char *out, size_t len);   // debug.intelxe.vram: who holds VRAM and the BAR window
int xePurgeNow();                             // rb7r debug.intelxe.purge: purge every DONTNEED BO that can be now
uint64_t xeEvictNow(int pid);                  // rb7s debug.intelxe.evict: move a process's never-mapped window BOs out
int xePurgeStats(char *out, size_t len);      // rb7r debug.intelxe.purge_stats
int xeFdStats(char *out, size_t len);         // rb7t debug.intelxe.fd_stats (object fds, xe_fd.cpp)
void xeIrqEvent(bool gtEvent);
void xeShutdown();
void intelxeCrumb(const char *stage);   // NVRAM "intelxe-shutdown" = stage, synced at once (IntelXe.cpp)
void xeRequestGtReset();
uint32_t ggttRecCount();
void xeRequestGtResetFor(const char *why);   // xe_gt_reset_async from IntelXe.cpp (CT dead)
void xePowerEvent(uint32_t event);   // gDev->powerEvent, for xe_core.cpp
// xe_aspm.cpp: PCIe ASPM on the GPU's path (debug.intelxe.aspm, debug.intelxe.aspm_l1; experimental, off by default)
extern IOLock *gAspmLock;
int xeAspmSet(int v);                        // -1 leave, 0 off, 1 L1 where every check passes
void xeAspmWake();                           // after a system wake: the last 0 or 1 again where the links lost it
int xeAspmReport(char *out, size_t len);
void xeAspmRegister(bool on);                // the two sysctls
int xeSuspend();
int xeResume();   // xe_core.cpp: stop submissions, let the GPU go idle, then GDRST (power-off / restart)
extern uint8_t gHwconfig[4096];
extern uint32_t gHwconfigLen;
void say(const char *fmt, ...) __printflike(1, 2);
void flushRange(void *p, size_t len);
// Flush the 16 dwords of a G2H message at ring index head, split at the ring's end: a plain 64-byte flush from the last
// dwords ran past the buffer into an unmapped page (panic in gucPollGt on the media CT, 2026-10-28).
static inline void flushRing(volatile uint32_t *ring, uint32_t head, uint32_t size) {
	uint32_t room = size - head, n = room < 16 ? room : 16;
	flushRange(const_cast<uint32_t *>(ring + head), n * 4);
	if (n < 16) flushRange(const_cast<uint32_t *>(ring), (16 - n) * 4);
}
