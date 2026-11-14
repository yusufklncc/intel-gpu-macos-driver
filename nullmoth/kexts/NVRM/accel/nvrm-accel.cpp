/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "../nvrm_off.h"
#ifdef NMX_ACCEL
// NMXAccel: this accelerator built for the AMD and Intel GPUs that NMXGPU drives (see nmx_accel_names.h)
#include "nmx_accel_names.h"
#endif
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <IOKit/pci/IOPCIDevice.h>
#include <pexpert/pexpert.h>
#include <libkern/OSKextLib.h>
#include <mach/kmod.h>
#include <IOKit/IOBufferMemoryDescriptor.h>
#include <IOKit/IORangeAllocator.h>
#include <IOKit/IOWorkLoop.h>
#include <IOKit/IOTimerEventSource.h>
#include <IOKit/IOMemoryDescriptor.h>
#include "IOAccelerator.h"
#include "IOGraphicsAccelerator2.h"
#include "IOAccelEventMachineFast2.h"
#include "IOAccelLegacyDisplayMachine.h"
#include "IOAccelTask.h"
#include "IOAccelMemoryMap.h"
#include "../nvrm_gpuva_abi.h"
class NVMemoryMap;
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                           unsigned long long len, unsigned long long align);
static void               nvAccelGpuVaFree(IOAccelMemoryMap *map);
static bool               nvAccelFlipTo(IOAccelResource2 *res);
static bool               nvAccelCopyToScanout(IOAccelResource2 *res);
void                      nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2);
bool                      nvAccelResHasContent(IOAccelResource2 *res);
bool                      nvAccelCopyRects(IOAccelResource2 *dst, IOAccelResource2 *src, const void *rects, unsigned n);
static void               nvAccelWriteStamp(int index, unsigned int value);
#ifdef NMX_ACCEL
#define NV_LOG_TAG "NMXAccel: "
#else
#define NV_LOG_TAG "NVAccel: "
#endif
#define ALOG(fmt, ...) do { kprintf(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); IOLog(NV_LOG_TAG fmt "\n", ##__VA_ARGS__); } while (0)
// Per-allocation lines: WindowServer and every Metal app create resources continuously, and logging each one filled the
// system log (#242). They print only with the -nvaccelverbose boot-arg.
static bool nvaccel_verbose() { static int v = -1; if (v < 0) { int x = 0; v = PE_parse_boot_argn("-nvaccelverbose", &x, sizeof x) ? 1 : 0; } return v == 1; }
#define ALOGV(fmt, ...) do { if (nvaccel_verbose()) ALOG(fmt, ##__VA_ARGS__); } while (0)
#include <libkern/c++/OSSymbol.h>
#include "nvrm_vram_abi.h"
#include <IOKit/IODeviceMemory.h>
#include <IOKit/IODeviceTreeSupport.h>   // gIODTPlane: NVRAM settings for the boot-time arm
#include <IOKit/IOKitKeys.h>             // kIONVRAMSyncNowPropertyKey
#include "tahoe_fwd.h"   // macOS 26: inherited slots Apple no longer exports, forwarded through its vtables
#include "nvaccel_stubs.h"
#ifdef NMX_ACCEL
#include "intelxe_surfvram_kabi.h"
#include "intelxe_bootargs.h"   // -intelxe_noaccel and its name before the rename, for one release
#endif
#ifdef NMX_ACCEL
// IOServiceOpen type of IOAccelNMXPresent, the Metal plugin's client for GPU marks and scan-out buffers ('NMXP')
static const UInt32 kNmxPresentType = 0x4E4D5850u;
static IOReturn nmxNewPresentClient(IOService *accel, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler);
// IOServiceOpen type of IntelXe.kext's xe connection (IOAccelIntelXeClient) opened on this accelerator ('B580'); the property
// tells xemac (user/xemac/xemac.c) that this accelerator hands it out (nmxNewIntelXeClient at the end of this file)
static const UInt32 kNmxIntelXeType = 0x42353830u;
static const char kNmxIntelXeClientKey[] = "NMXIntelXeClientType";
static IOReturn nmxNewIntelXeClient(IOService *accel, IOService *pci, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler);
static void nmxInstallSysctl();
#endif
#ifndef NMX_ACCEL
#include "nvkms-kapi.h"   // NVIDIA open-gpu-kernel-modules; only the display CRC sysctl uses it
#endif
#include <sys/sysctl.h>
#include <sys/vnode.h>   // vnode_lookup (com.apple.kpi.bsd): the system-volume Metal plugin probe
#include <libkern/OSAtomic.h>

// ---- GPU core temperature, published into PerformanceStatistics -------------
// Same register the out-of-tree NvidiaSensor.kext reads. nouveau's gp100_temp_get() reads
// 0x020460 as well, but it insists on the "valid" bit (0x20000000) being set and returns
// -ENODEV otherwise; a GPU whose firmware is GSP-RM never sets it, which is why
// gp100_therm_new() refuses to attach at all under GSP. Here the range is the judge instead:
// a value outside 1..125 C is treated as "no reading" rather than published.
static constexpr UInt32   kNvTsensOffset     = 0x20460;
static constexpr UInt32   kNvTsensValueMask  = 0x0001fff8u;
static constexpr unsigned kNvTempMinC        = 1;
static constexpr unsigned kNvTempMaxC        = 125;
// The family rewrites PerformanceStatistics 0.25-2 s apart, so a slower writer would be
// overwritten before iStat reads it; 250 ms keeps our key present essentially always.
static constexpr unsigned kNvTempIntervalMs  = 250;
// Nothing touches BAR0 until the GPU has settled: bring-up still runs for a while after start().
static constexpr unsigned kNvTempFirstMs     = 5000;

extern "C" {   // kern/thread_call.h and kern/clock.h clash with the family headers here, so the KPIs are declared by hand
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter_delayed(struct thread_call *call, uint64_t deadline);
void clock_interval_to_deadline(uint32_t interval, uint32_t scale_factor, uint64_t *result);
uint64_t mach_absolute_time(void);
void absolutetime_to_nanoseconds(uint64_t abstime, uint64_t *result);
void proc_selfname(char *buf, int size);
}
static volatile SInt64 gCntVmAlloc, gCntVmAllocBytes, gCntVmDealloc, gCntVmDeallocBytes, gCntVmRefused,
                       gCntResNew, gCntResFree, gCntMapCommit, gCntMapRelease, gCntMapFree;
static void nvaccelInstallSysctl(void);
static int gNvAccelArmed = 0;
#ifdef NMX_ACCEL
// Boot-time arming (2026-10-29; replaces intelxe-accel.sh and its WindowServer restart): when IntelXeFB publishes with
// IntelXeFBAccelAllowed, NMXAccel arms the display pipe itself (levels 1/3/4, transactions, async, flips by default) and
// only then registers, so the Metal plugin's WindowServer gate (debug.nmxaccelfb) already reads armed when WindowServer
// first sees this accelerator. Off: -intelxe_noaccel (and safe mode): registered at start, nothing armed, as before.
// nmxearlyreg=1 registers at start even with arming on (WindowServer then gets no B580 device until it restarts).
class NVAccel;
static int gNmxAuto = 0;        // this boot arms on its own
static int gNmxAutoState = 0;   // 1 waiting for IntelXeFB, 2 arming, 3 armed, -1 held by IntelXeFB, -2 no device control, -3 arm failed, -4 no framebuffer
static void nmxAutoArm(NVAccel *a);
// IntelXeFB's boot guard, read here too: when it holds acceleration this boot, NMXAccel must not register at all. A
// registered accelerator that is never armed made WindowServer open its display pipe and mark the display offline: the
// Apple logo stayed on screen (5 Nov). Unregistered, WindowServer runs on IntelXeFB alone, as the macOS installer does on
// the firmware's framebuffer. The rule is IntelXeFB's (guardBegin): intelxe-accel-off set and not "ok", or the previous
// boot left intelxe-guard "accel<n>" with n >= 4. Read before or after IntelXeFB's start, one of the two says so.
static bool nmxNvramRead(const char *key, char *out, size_t len) {
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
static bool nmxGuardHolds(char *why, size_t len) {
    char v[96];
    if (nmxNvramRead("intelxe-accel-off", v, sizeof v) && v[0] && strcmp(v, "ok")) { snprintf(why, len, "intelxe-accel-off: %s", v); return true; }
    if (nmxNvramRead("intelxe-guard", v, sizeof v) && !strncmp(v, "accel", 5)) {
        unsigned n = 0;
        for (const char *c = v + 5; *c >= '0' && *c <= '9' && n < 1000; c++) n = n * 10 + (unsigned)(*c - '0');
        if (n >= 4) { snprintf(why, len, "intelxe-guard %s: %u accelerated boots in a row ended inside their first minute", v, n); return true; }   // IntelXeFB's kGuardAccelHold
    }
    return false;
}
#endif
// 1 once the headless check restored the plugin name without arming (no NVIDIA display head). The Metal plugin reads it:
// a headless GPU hands frames to another GPU's WindowServer through system memory, so IOSurfaces must be copied out.
static int gNvHeadless = 0;
#ifdef NMX_ACCEL
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = AMD/Intel Metal device for apps; frames are copied to the framebuffer WindowServer draws");
#else
SYSCTL_INT(_debug, OID_AUTO, nvaccel_headless, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadless, 0,
           "1 = no NVIDIA display: Metal device for apps only, WindowServer on the GPU that owns the display");
#endif
// 4 NVIDIA heads (nvfbheads=4) + 1 for an Intel built-in panel (NMIntelFB, IOFBDependentIndex 4): WindowServer composites
// that panel on the NVIDIA card and the display-pipe copy path below writes each frame into the Intel scan-out surface.
static const unsigned kNvMaxFB = 5;
static unsigned gNvHeadsPublished = 0, gNvHeadsRegistered = 0;
static unsigned gNvHeadsPiped = 0;
// The Intel built-in panel (head 4) joins only when this is 1. Off by default while the path is proven: a write at run
// time (then debug.nvaccelfb=3 and a WindowServer restart) can be undone by a reboot, so a hang never needs hands
// (the first virtual-panel test froze the machine at boot and needed a power cycle).
static int gNvIgpu = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_igpu, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvIgpu, 0,
           "1 = give an Intel built-in panel (NMIntelFB, head 4) a display pipe on this accelerator");
#ifdef NMX_ACCEL
// More than one IntelXeFB head (IntelXeFB's second pipe publishes IntelXeFramebufferPipe as head 1; design:
// docs/design/DESIGN-multihead-accel.md). Off by default: boot-arg nmxheads=1 or NVRAM nmx-heads=1 (2 = register without
// the mode change), or debug.nmxaccel_heads_auto at run time. On, a head published after the arm is registered on its
// own (through IOGraphics' gates, as debug.nvaccelfb=3 does it), a head that terminates frees its slot, and a head
// without flips/pipe gamma is never asked for them. Off, or with one head, nothing here runs.
#include "nmx_heads.h"
#include "nmx_vbldue.h"
#include <sys/kauth.h>   // debug.nmxaccel_heads is root-only: it calls into every framebuffer
static int gNmxHeads = 0;
struct NMXHead {
    volatile UInt32 caps;                   // kNmxCap*, read when the head publishes
    volatile UInt32 terminated;             // how often a framebuffer in this slot terminated
    volatile SInt32 lateResult;             // 0 never, 1 registered with its own pipe, -1 registered without one
    volatile UInt32 srcW, srcH, scW, scH;   // the last copied frame's source size and the scan-out's (scaler check)
    volatile SInt64 mismatch;               // frames refused because the source was not the scan-out's size
    uint64_t publishedAt, armedAt;          // mach_absolute_time
};
static NMXHead gNmxHead[kNvMaxFB];
static unsigned nmxHeadCapsOf(IOService *fb)
{
    unsigned c = 0;
    if (!fb) return 0;
    if (OSObject *o = fb->copyProperty("IntelXeFBScanoutVersion")) { c |= kNmxCapFlip; o->release(); }
    if (OSObject *o = fb->copyProperty("IntelXeFBPipeGammaSize")) { c |= kNmxCapGamma; o->release(); }
    if (OSObject *o = fb->copyProperty("NVRMTiming")) { c |= kNmxCapTiming; o->release(); }
    if (fb->metaCast("IntelXeFramebufferPipe")) c |= kNmxCapPipeFB;
    return c;
}
static inline bool nmxHeadSkips(unsigned head, unsigned cap)
{
    return head < kNvMaxFB && nmxHeadLacks(gNmxHeads != 0, head, gNmxHead[head].caps, cap);
}
static int nmxSetting(const char *arg, const char *nv, int def);
#endif

extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_ACCEL
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAccel, "0.1", _start, _stop) }
#else
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVAccel, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }

class NVEventMachine : public IOAccelEventMachineFast2 {
    OSDeclareDefaultStructors(NVEventMachine)
    NM_TAHOE_FWD(NVEventMachine)
public:
    void writeStamp(int s, vendevtCommandRec *r, unsigned int v) APPLE_KEXT_OVERRIDE {
        (void)r;
        nvAccelWriteStamp(s, v);
    }
    void prepareBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM prepareBarrier(%p)", b); }
    void completeBarrier(vendevtBarrierRec *b) APPLE_KEXT_OVERRIDE { ALOG("EM completeBarrier(%p)", b); }
    void writeBarrierElement(vendevtBarrierRec *b, int i, unsigned int v) APPLE_KEXT_OVERRIDE { ALOG("EM writeBarrierElement(%p, %d, %u)", b, i, v); }
    bool checkGPUProgress() APPLE_KEXT_OVERRIDE { return true; }
    bool checkChannelProgress(int ch) APPLE_KEXT_OVERRIDE { return true; }
    void *eventTimeout(int stamp) APPLE_KEXT_OVERRIDE { ALOG("EM eventTimeout(%d) — ignored, we do not use family channels", stamp); return nullptr; }
};
OSDefineMetaClassAndStructors(NVEventMachine, IOAccelEventMachineFast2)
static void (*gEnableAccel)(const char *why) = nullptr;
static int gNvScanout = 0;
static int gNvAperture = 0;
class NVDisplayMachine : public IOAccelLegacyDisplayMachine {
    OSDeclareDefaultStructors(NVDisplayMachine)
    NM_TAHOE_FWD(NVDisplayMachine)
public:
    bool displayModeWillChange() APPLE_KEXT_OVERRIDE { ALOG("DM displayModeWillChange"); return true; }
    bool displayModeDidChange() APPLE_KEXT_OVERRIDE {
        ALOG("DM displayModeDidChange -> true");
        if (gEnableAccel) gEnableAccel("displayModeDidChange");
        return true;
    }
};
OSDefineMetaClassAndStructors(NVDisplayMachine, IOAccelLegacyDisplayMachine)
#define NV_PRIVATE_VA_BASE  0x800000000000ull
#define NV_PRIVATE_VA_SIZE  0x000010000000ull
class NVTask : public IOAccelTask {
    OSDeclareDefaultStructors(NVTask)
    NM_TAHOE_FWD(NVTask)
public:
    IORangeAllocator *fVa = nullptr;
    unsigned fAllocLogged = 0, fPrivate = 0;
    unsigned long long allocate(const IOAccelMemoryMap *map) APPLE_KEXT_OVERRIDE {
        const unsigned long long len = map ? ((IOAccelMemoryMap *)map)->getLength() : 0;
        const unsigned long long align = map ? *(const volatile unsigned long long *)((const unsigned char *)map + 0x80) : 0;
        unsigned long long got = nvAccelGpuVaAlloc((IOAccelMemoryMap *)map, len, align);
        if (!got && fAllocLogged < 12)
            ALOG("NVTask::allocate: RM refused %llu bytes (align %llu) -- no GPU address", len, align);
        return got;
    }
    void deallocate(const IOAccelMemoryMap *map, unsigned long long addr) APPLE_KEXT_OVERRIDE {
        (void)addr;
        nvAccelGpuVaFree((IOAccelMemoryMap *)map);
    }
};
OSDefineMetaClassAndStructors(NVTask, IOAccelTask)
class NVMemoryMap : public IOAccelMemoryMap {
    OSDeclareDefaultStructors(NVMemoryMap)
    NM_TAHOE_FWD(NVMemoryMap)
public:
    IOAccelMemory     *fMem       = nullptr;
    unsigned int       fHVirt     = 0;
    unsigned int       fHRmMemory = 0;
    unsigned long long fGpuVa     = 0;
    unsigned long long fVaLen     = 0;
    bool               fMapped    = false;
    unsigned int fCommits = 0;
    bool init(IOGraphicsAccelerator2 *a, IOAccelTask *t, IOAccelMemory *m,
              unsigned int o) APPLE_KEXT_OVERRIDE {
        if (!IOAccelMemoryMap::init(a, t, m, o)) return false;
        fMem = m;
        if (fMem) fMem->retain();
        return true;
    }
    void free() APPLE_KEXT_OVERRIDE {
        if (fMem) { fMem->release(); fMem = nullptr; }
        IOAccelMemoryMap::free();
    }
    bool commitIntoGPUPageTable() APPLE_KEXT_OVERRIDE;
    void releaseFromGPUPageTable() APPLE_KEXT_OVERRIDE;
    bool updateGPUPageTable() APPLE_KEXT_OVERRIDE { return fMapped; }
};
OSDefineMetaClassAndStructors(NVMemoryMap, IOAccelMemoryMap)

#define NV_CARRIERS 32
class IOAccelNVRMCarrier : public IOService {
    OSDeclareDefaultStructors(IOAccelNVRMCarrier)
};
OSDefineMetaClassAndStructors(IOAccelNVRMCarrier, IOService)

class NVAccel : public IOGraphicsAccelerator2 {
    OSDeclareDefaultStructors(NVAccel)
    NM_TAHOE_FWD(NVAccel)
    IOAccelNVRMCarrier *fCarriers[NV_CARRIERS] = { nullptr };
    unsigned            fCarrierNext = 0;
    unsigned            fCarrierLogged = 0;
public:
    IOBufferMemoryDescriptor *fStamps = nullptr; IORangeAllocator *fRanges[32] = {};
    uint64_t  getStampMemory(unsigned int *p) APPLE_KEXT_OVERRIDE {
        if (!fStamps) { fStamps = IOBufferMemoryDescriptor::inTaskWithOptions(kernel_task, kIODirectionInOut | kIOMemoryPhysicallyContiguous, 16384, 4096); if (fStamps) { fStamps->prepare(); bzero(fStamps->getBytesNoCopy(), 16384); } }
        if (p) *p = fStamps ? 4096 : 0;
        ALOG("getStampMemory(%p) -> descriptor %p (%u stamps, 16 KB)", p, fStamps, p ? *p : 0); return (uint64_t)fStamps; }
    OSObject *newEventMachine() APPLE_KEXT_OVERRIDE { NVEventMachine *m = OSTypeAlloc(NVEventMachine); ALOG("newEventMachine -> %p (family inits it)", m); return m; }
    OSObject *newGPUTask(const char *who) {
        for (unsigned i = 0; i < 32; i++) if (!fRanges[i]) {
            fRanges[i] = IORangeAllocator::withRange(1ULL << 40, 4096);
            if (fRanges[i]) fRanges[i]->allocateRange(0, 0x100000);
        }
        NVTask *t = OSTypeAlloc(NVTask); bool ok = t && t->init(this, 1, fRanges);
        ALOG("%s -> %p init=%d", who, t, ok); if (t && !ok) { t->release(); t = nullptr; } return t; }
    OSObject *createUserGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createUserGPUTask"); }
    void populateAccelConfig(IOAccelConfig *c) APPLE_KEXT_OVERRIDE {
        uint8_t *p = reinterpret_cast<uint8_t *>(c);
        if (!p) { ALOG("populateAccelConfig(NULL) — nothing to do"); return; }
        if (((uintptr_t)p & 0xFFF) + 0x70 > 0x1000) {
            ALOG("populateAccelConfig(%p) — 0x70 bytes would cross out of this page; REFUSING to touch it", p);
            return;
        }
        ALOG("populateAccelConfig(%p) — what the FAMILY pre-filled, before we write a byte:", p);
        for (unsigned i = 0; i < 0x70; i += 8)
            ALOG("   cfg+0x%02x = 0x%016llx", i, *(const uint64_t *)(p + i));

        *(uint64_t *)(p + 0x00) = (uint64_t)(uintptr_t)"NVIDIAAccelerator";
        *(uint32_t *)(p + 0x08) = 0x30844300u;
        *(uint32_t *)(p + 0x0c) = 15u;
        *(uint32_t *)(p + 0x18) = 8u;
        *(uint32_t *)(p + 0x30) = 0x40004000u;
        *(uint32_t *)(p + 0x34) = 0x00008000u;
        *(uint32_t *)(p + 0x38) = 0x00008000u;
        *(uint8_t  *)(p + 0x44) = 0;
        *(uint8_t  *)(p + 0x46) = 1;
        *(uint8_t  *)(p + 0x47) = 1;
        *(uint64_t *)(p + 0x48) = 0x0000001F0000000FULL;
        *(uint64_t *)(p + 0x50) = 0x7FFFFFFF0000001FULL;
        *(uint64_t *)(p + 0x58) = 0x0000000C00000000ULL;
        *(uint32_t *)(p + 0x64) = 0x4Bu;
        *(uint32_t *)(p + 0x28) = kNvMaxFB;
        ALOG("populateAccelConfig(%p) — filled: AccelCaps=15, flags=0x30844300, maxtex=16384x16384, maxfb=%u written (cfg+0x2c left %u)",
             p, kNvMaxFB, *(const uint32_t *)(p + 0x2c));
    }
    bool      configureDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("configureDevice(%p) %04x:%04x — RM owns the card (NVRM.kext); nothing to do here yet", pci, pci ? pci->configRead16(0) : 0, pci ? pci->configRead16(2) : 0); return true; }
    void      teardownDevice(IOPCIDevice *pci) APPLE_KEXT_OVERRIDE { ALOG("teardownDevice(%p)", pci); }
    OSObject *createKernelGPUTask() APPLE_KEXT_OVERRIDE { return newGPUTask("createKernelGPUTask"); }
    OSObject *newDisplayPipe() APPLE_KEXT_OVERRIDE {
        NVDisplayPipe *p = OSTypeAlloc(NVDisplayPipe);
        if (!p) { ALOG("newDisplayPipe: OSTypeAlloc FAILED - the family will store this NULL unchecked"); return NULL; }
        ALOG("newDisplayPipe -> %p", p); return p; }
    OSObject *newDisplayMachine() APPLE_KEXT_OVERRIDE { NVDisplayMachine *d = OSTypeAlloc(NVDisplayMachine);
        fDM = d;
        ALOG("newDisplayMachine -> %p", d); return d; }
    OSObject *newGLContext() APPLE_KEXT_OVERRIDE { NVGLContext *c = OSTypeAlloc(NVGLContext); ALOG("newGLContext -> %p", c); return c; }
    OSObject *newCLContext() APPLE_KEXT_OVERRIDE { NVCLContext *c = OSTypeAlloc(NVCLContext); ALOG("newCLContext -> %p", c); return c; }
    OSObject *newSurface() APPLE_KEXT_OVERRIDE { NVSurface *o = OSTypeAlloc(NVSurface); ALOG("newSurface -> %p", o); return o; }
    OSObject *new2DContext() APPLE_KEXT_OVERRIDE { NV2DContext *c = OSTypeAlloc(NV2DContext); ALOG("new2DContext -> %p", c); return c; }
    OSObject *newVideoContext() APPLE_KEXT_OVERRIDE { NVVideoContext *c = OSTypeAlloc(NVVideoContext); ALOG("newVideoContext -> %p", c); return c; }
    OSObject *newSysMemory() APPLE_KEXT_OVERRIDE { NVSysMemory *m = OSTypeAlloc(NVSysMemory); ALOGV("newSysMemory -> %p", m); return m; }
    OSObject *newVidMemory() APPLE_KEXT_OVERRIDE { NVVidMemory *m = OSTypeAlloc(NVVidMemory); ALOGV("newVidMemory -> %p", m); return m; }
    OSObject *newResource() APPLE_KEXT_OVERRIDE { NVResource *r = OSTypeAlloc(NVResource); OSAddAtomic64(1, &gCntResNew); ALOGV("newResource -> %p", r); return r; }
    OSObject *newMemoryMap() APPLE_KEXT_OVERRIDE { NVMemoryMap *m = OSTypeAlloc(NVMemoryMap); ALOG("newMemoryMap -> %p", m); return m; }

    NVDisplayMachine *fDM = nullptr;
    IOPCIDevice *fPCI = nullptr;
    // GPU core temperature: BAR0 is mapped lazily on the first tick, never in start().
    IOMemoryMap        *fTempMap    = nullptr;
    volatile UInt32    *fTempRegs   = nullptr;
    IOTimerEventSource *fTempTimer  = nullptr;
    int                 fTempC      = 0;
    UInt32              fTempFails  = 0;
    bool                fTempLogged = false;
    IONotifier *fFBNote = nullptr;
#ifdef NMX_ACCEL
    volatile SInt32 fRegistered = 0;
    thread_call_t fAutoCall = nullptr, fRegDeadline = nullptr;
    void registerOnce(const char *why) {
        if (!OSCompareAndSwap(0, 1, (volatile UInt32 *)&fRegistered)) return;
        registerService();
        ALOG("registerService() (%s) -- Metal can now find MetalPluginName=NVMTLDriver", why);
    }
    void autoNote(const char *what) { setProperty("NMXAutoArm", what); ALOG("auto-arm: %s", what); }
#ifdef NMX_NOMETAL_FALLBACK
    // macOS 26 black screen with a cursor (upstream c4f7e79 "a GPU with no Metal accelerator does not own the desktop",
    // adapted). The plugin gives WindowServer the B580 only while the arm is pending or done (nvmtl_accel_armed). When the
    // arm ends unarmed and no OTHER GPU has a Metal accelerator either (UHD 770 not accelerated: WhateverXe missing, too
    // old for this macOS, or the iGPU disabled), WindowServer finds a plugin that refuses it and no Metal device
    // anywhere: "Failed to create MetalDevice", restart every ten seconds. Upstream's test is vendor-id != 0x10de, which
    // on this machine is true for nothing (B580 and UHD 770 are both 0x8086): "other GPU" here is any display-class PCI
    // device that is not our own provider (pointer identity), and it counts only if a Metal accelerator sits under it.
    // Then MetalPluginName is withheld, WindowServer starts unaccelerated on the framebuffer, and a later successful arm
    // (manual debug.nmxaccelfb=1 -> onCountGrewLocked) puts the name back. Boot-arg -nmxkeepmetal turns this off.
    bool fNmxWithheld = false;
    static bool nmxGpuHasMetal(IORegistryEntry *pci) {   // upstream gpuHasMetalAccelerator: child or grandchild
        if (!pci) return false;
        bool found = false;
        if (OSIterator *it = pci->getChildIterator(gIOServicePlane)) {
            while (!found) {
                IORegistryEntry *c = OSDynamicCast(IORegistryEntry, it->getNextObject());
                if (!c) break;
                if (c->getProperty("MetalPluginName")) { found = true; break; }
                if (OSIterator *it2 = c->getChildIterator(gIOServicePlane)) {
                    while (IORegistryEntry *g = OSDynamicCast(IORegistryEntry, it2->getNextObject()))
                        if (g->getProperty("MetalPluginName")) { found = true; break; }
                    it2->release();
                }
            }
            it->release();
        }
        return found;
    }
    // another display-class PCI device (class 0x03) with a Metal accelerator; *who names it (or the reason for "yes")
    bool nmxOtherGpuHasMetal(char *who, size_t len) {
        OSDictionary *m = serviceMatching("IOPCIDevice");
        OSIterator *it = m ? getMatchingServices(m) : nullptr;
        if (m) m->release();
        if (!it) { snprintf(who, len, "%s", "PCI devices not listable: assumed"); return true; }   // unknown: keep today's behaviour
        bool found = false; unsigned gpus = 0;
        while (OSObject *o = it->getNextObject()) {
            IOPCIDevice *d = OSDynamicCast(IOPCIDevice, o);
            if (!d || d == fPCI) continue;
            OSData *cc = OSDynamicCast(OSData, d->getProperty("class-code"));
            if (!cc || cc->getLength() < 3 || ((const uint8_t *)cc->getBytesNoCopy())[2] != 0x03) continue;
            gpus++;
            if (nmxGpuHasMetal(d)) { found = true; snprintf(who, len, "%s (%s)", d->getName(), d->getLocation() ? d->getLocation() : "?"); break; }
        }
        it->release();
        if (!found) snprintf(who, len, "%u other display device%s, none with a Metal accelerator", gpus, gpus == 1 ? "" : "s");
        return found;
    }
    void nmxNoMetalFallback(const char *why) {
        if (fNmxWithheld || !getProperty("MetalPluginName")) return;
        char who[96] = "";
        int keep = 0;
        if (PE_parse_boot_argn("-nmxkeepmetal", &keep, sizeof keep)) { ALOG("no-Metal fallback (%s): off (-nmxkeepmetal)", why); return; }
        if (nmxOtherGpuHasMetal(who, sizeof who)) {
            ALOG("no-Metal fallback (%s): WindowServer has another Metal GPU: %s -- MetalPluginName kept for apps", why, who);
            return;
        }
        if (OSObject *mp = getProperty("MetalPluginName")) {
            mp->retain();
            if (fMetalPlugin) fMetalPlugin->release();
            fMetalPlugin = mp;
        }
        fNmxWithheld = true;
        removeProperty("MetalPluginName");
        setProperty("NMXMetalWithheld", why);
        ALOG("no-Metal fallback (%s): %s -- MetalPluginName WITHHELD so WindowServer starts unaccelerated on the "
             "framebuffer instead of failing to create a Metal device; a later arm puts it back", why, who);
    }
    thread_call_t fNoMetalCall = nullptr;
    static void noMetalFire(thread_call_param_t p0, thread_call_param_t p1) { ((NVAccel *)p0)->nmxNoMetalFallback((const char *)p1); }
#endif
    static void autoFire(thread_call_param_t p0, thread_call_param_t) { nmxAutoArm((NVAccel *)p0); }
    static void regDeadlineFire(thread_call_param_t p0, thread_call_param_t) {
        NVAccel *me = (NVAccel *)p0;
        if (gNmxAutoState != 1) return;   // arming (it registers when done) or already decided
        gNmxAutoState = -4;
        me->autoNote("no IntelXeFB within 30 s of start: registered unarmed");
        me->registerOnce("no framebuffer to arm");
#ifdef NMX_NOMETAL_FALLBACK
        me->nmxNoMetalFallback("no IntelXeFB");
#endif
    }
#endif
    IONotifier *fIntelNote = nullptr;
    static NVAccel *gAccel;
    IOService *fFB = nullptr;
    IOService *volatile fFBs[kNvMaxFB] = {};
    unsigned fFBRegistered = 0;
    IOLock *fRegLock = nullptr;
    volatile bool fBootWalk = false;   // set while nmxAutoArm's levels 1/3/4 run (NMX_ACCEL only)
    OSObject *fMetalPlugin = nullptr;
#ifdef NMX_ACCEL
    // Which NVMTLDriver.bundle Metal loads. Safari/WebKit's GPU process may load Metal plugins only from the system
    // volume; ssv-install.sh (the OCLP root-patch flow) puts a copy into /System/Library/Extensions. When that copy is
    // there, MetalPluginName names it ("NVMTLDriver": Metal resolves plugin names in /System/Library/Extensions).
    // Metal looks in /Library/GPUBundles before /System/Library/Extensions and takes the first <name>.bundle it finds,
    // so a /Library copy with the same name would shadow the system one (and be sandbox-denied in WebKit's GPU
    // process, with no fallback): the /Library copy is installed as NVMTLDriverLib.bundle, and that is the fallback
    // name, "../../../Library/GPUBundles/NVMTLDriverLib" (relative to /System/Library/Extensions). An install from before
    // the rename (no NVMTLDriverLib.bundle) keeps "../../../Library/GPUBundles/NVMTLDriver". Boot-arg
    // nmxplugin=lib, or the file /Library/GPUBundles/NVMTLDriver.use-library (the data volume: `sudo touch` over ssh,
    // then reboot; OpenCore usually rewrites boot-args from its config), forces the /Library copy: recovery from a bad
    // system copy without touching the sealed volume.
    // Published as NMXMetalPluginFrom ("system volume" / "/Library"). IOGLBundleName is not touched.
    // The probe runs at start (an AuxKC kext starts after kernelmanagerd has the root and data volumes mounted: the
    // collection itself is read from the data volume) and once more at the first user client, in case it ran early.
    bool fPluginFromSys = false, fPluginRechecked = false;
    void selectMetalPlugin(const char *when) {
        char arg[8] = {};
        bool forceLib = PE_parse_boot_argn("nmxplugin", arg, sizeof arg) && !strncmp(arg, "lib", 3);
        bool sys = false;
        int lerr = -1;
        if (!forceLib) {
            vnode_t fp = NULLVP;
            if (!vnode_lookup("/Library/GPUBundles/NVMTLDriver.use-library", 0, &fp, vfs_context_current())) {
                vnode_put(fp);
                forceLib = true;
            }
        }
        if (!forceLib) {
            vnode_t vp = NULLVP;
            lerr = vnode_lookup("/System/Library/Extensions/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver", 0, &vp,
                                vfs_context_current());
            if (!lerr) {
                sys = true;
                vnode_put(vp);
            }
        }
        bool libRenamed = false;
        if (!sys) {
            vnode_t lp = NULLVP;
            if (!vnode_lookup("/Library/GPUBundles/NVMTLDriverLib.bundle", 0, &lp, vfs_context_current())) {
                vnode_put(lp);
                libRenamed = true;
            }
        }
        const char *name = sys ? "NVMTLDriver" : libRenamed ? "../../../Library/GPUBundles/NVMTLDriverLib"
                                                            : "../../../Library/GPUBundles/NVMTLDriver";
#ifdef NMX_NOMETAL_FALLBACK
        if (!fNmxWithheld)
#endif
        setProperty("MetalPluginName", name);
        setProperty("NMXMetalPluginFrom", sys ? "system volume" : "/Library");
        if (fMetalPlugin) {   // a withheld name that a later restore puts back: the same choice
            fMetalPlugin->release();
            fMetalPlugin = OSString::withCString(name);
        }
        fPluginFromSys = sys;
        ALOG("Metal plugin (%s): %s%s (system copy lookup %d)", when, sys ? "/System/Library/Extensions/NVMTLDriver.bundle"
             : libRenamed ? "/Library/GPUBundles/NVMTLDriverLib.bundle" : "/Library/GPUBundles/NVMTLDriver.bundle (no NVMTLDriverLib)",
             forceLib ? " -- forced (nmxplugin=lib or NVMTLDriver.use-library)" : "", lerr);
    }
#endif
    static bool fbPublished(void *target, void *ref, IOService *newService, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !newService) return true;
        unsigned idx = 0;
        OSNumber *num = OSDynamicCast(OSNumber, newService->getProperty("IOFBDependentIndex"));
        if (num) idx = num->unsigned32BitValue();
        const char *what = "kept";
        if (idx >= kNvMaxFB) what = "OUT OF RANGE -- this head will get no pipe";
        else {
            newService->retain();
            if (OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFBs[idx])) {
#ifdef NMX_ACCEL
                gNmxHead[idx].caps = nmxHeadCapsOf(newService);
                gNmxHead[idx].publishedAt = mach_absolute_time();
#endif
                OSBitOrAtomic(1u << idx, &gNvHeadsPublished);
            }
            else { newService->release(); what = "DUPLICATE -- the first one keeps the slot"; }
        }
        newService->retain();
        if (idx == 0 && me->fFBs[0] == newService) {
            IOService *old;
            do { old = me->fFB; } while (!OSCompareAndSwapPtr(old, newService, (void *volatile *)&me->fFB));
            if (old == newService) newService->release();
            else if (old) {
                bool slotted = false;
                for (unsigned j = 0; j < kNvMaxFB; j++) if (me->fFBs[j] == old) slotted = true;
                ALOG("fFB: head 0 (%s) displaced %s, which published first (%s)", newService->getName(), old->getName(),
                     slotted ? "its extra reference returned" : "NOT slotted: keeping its reference rather than risk a reader");
                if (slotted) old->release();
            }
        // only head 0 may become the primary framebuffer: an Intel panel (index 4) publishing first became fFB (10-10 log:
        // "head 0 displaced NMIntelFB, which published first"), and on a laptop with nothing on the NVIDIA ports it stays
        } else if (idx != 0 || !OSCompareAndSwapPtr(nullptr, newService, (void *volatile *)&me->fFB)) newService->release();
        unsigned before = me->fDM ? me->fDM->getFramebufferCount() : 0xFFFFFFFFu;
        ALOG("%s published -- display machine framebuffer count = %u (index %u %s)", newService->getName(), before, idx, what);
#ifdef NMX_ACCEL
        if (idx == 0 && gNmxAuto && OSCompareAndSwap(1, 2, (volatile UInt32 *)&gNmxAutoState)) {
            OSObject *allowed = newService->getProperty("IntelXeFBAccelAllowed");
            if (allowed == kOSBooleanTrue && me->fAutoCall) {
                me->autoNote("IntelXeFB published: arming");
                thread_call_enter(me->fAutoCall);
            } else {
                gNmxAutoState = -1;
                OSString *why = OSDynamicCast(OSString, newService->getProperty("IntelXeFBAccelHeld"));
                char m[160];
                snprintf(m, sizeof m, "held: %s", !allowed ? "IntelXeFB publishes no IntelXeFBAccelAllowed (older build)" :
                         why ? why->getCStringNoCopy() : "IntelXeFB says no");
                me->autoNote(m);
                // registered at start already (the guard did not hold yet when NMXAccel started): Metal is withheld below.
                // Not registered (held at start): it stays so.
                if (me->fRegistered) {
#ifdef NMX_NOMETAL_FALLBACK
                    me->nmxNoMetalFallback("acceleration held");
#endif
                }
            }
            return true;
        }
        if (gNvAccelArmed && gNmxHeads && idx > 0 && idx < kNvMaxFB && me->fHeadsCall && me->fFBs[idx] == newService) {
            // not from here: this runs inside the publisher's registerService (IntelXeFB's second pipe holds its lock)
            ALOG("  index %u published AFTER the arm: the multi-head knob registers it in 2 s "
                 "(`sysctl debug.nmxaccel_heads` shows the plan)", idx);
            me->headsKick("a head published after the arm", 2000);
            return true;
        }
#endif
        if (gNvAccelArmed)
            ALOG("  index %u published AFTER the arm: it gets a pipe only when debug.nvaccelfb=3 is written again, "
                 "and WindowServer composites it only after a WindowServer restart", idx);
#ifdef NMX_ACCEL
        else if (idx > 0 && idx < kNvMaxFB && (gNmxHead[idx].caps & kNmxCapPipeFB) && gNmxAuto && (gNmxAutoState == 1 || gNmxAutoState == 2))
            ALOG("  index %u is IntelXeFB's second framebuffer, published before the arm (%s): %s", idx,
                 newService->getProperty("IntelXeFBConnected") == kOSBooleanFalse ? "no monitor" : "a monitor",
                 gNmxHeads ? "the boot arm gives it a display pipe (multi-head knob on)"
                           : "the multi-head knob is off, so the boot arm leaves it for `sysctl debug.nvaccelfb=3`");
#endif
        else
            ALOG("  to register it:  sudo sysctl -w debug.nvaccelfb=1   (2 = re-run the family's own sweep)");
        return true;
    }
    // Headless GPU (laptops whose panel and outputs are all on the Intel or AMD GPU: issues #76, #106). NVKMS finds no
    // display, so no NVRMFB head ever publishes and the withheld MetalPluginName was never restored: Metal never offered
    // the card, although NVRM and NVK run it fine (#106: vkCreateInstance enumerates the GTX 1660 Ti). Apple's own
    // two-GPU MacBook Pros work this way: the integrated GPU drives every display and WindowServer, and the discrete GPU
    // is a second Metal device that apps choose and macOS copies their frames from. So when no head has published by the
    // deadline, the plugin name goes back on WITHOUT arming: apps get the NVIDIA device, and WindowServer (allow-listed
    // "!WindowServer" = only when armed) keeps the GPU that owns the display. Nothing here touches a display pipe or a
    // mode change. Boot-arg -nvaccelnoheadless keeps the old behaviour.
    // The check runs every few seconds rather than once at the deadline: on a laptop whose screen is on the other GPU,
    // NVKMS has listed this card's connectors, with none connected, about 7 s after start, and every app opened before
    // the plugin name comes back never sees the NVIDIA device (Ampere and Turing laptops reported "Metal device: no" at
    // first login while the name came back at 60 s). So the name goes back as soon as both are true: NVKMS reports no
    // display on any of this card's connectors, and another GPU's framebuffer already shows the desktop. A desktop whose
    // monitor is slow to wake has no other GPU's framebuffer and still waits for the deadline.
    static constexpr uint32_t kHeadlessDeadlineSec = 60;   // chosen: NVRMFB heads publish within ~20 s of start (#76 log)
    static constexpr uint32_t kHeadlessPollSec = 2;        // chosen: a small fraction of the ~7 s NVKMS takes to enumerate
    struct thread_call *fHeadlessCall = nullptr;
    uint32_t fHeadlessWaited = 0;
    // true when NVKMS enumerated this card's connectors (NVRMFB writes connected-displays on each display nub it
    // enumerates) and found a display on none of them
    static bool headlessNoDisplayHere() {
        bool seen = false, none = true;
        if (OSIterator *it = getMatchingServices(serviceMatching("NVRMDisplay"))) {
            while (OSObject *o = it->getNextObject()) {
                IOService *s = OSDynamicCast(IOService, o);
                OSNumber *cd = s ? OSDynamicCast(OSNumber, s->getProperty("connected-displays")) : nullptr;
                if (!cd) continue;
                seen = true;
                if (cd->unsigned32BitValue() > 0) none = false;
            }
            it->release();
        }
        return seen && none;
    }
    // the first framebuffer that is not one of this driver's heads; *other is set when it sits on another vendor's GPU
    static void headlessDisplayOwner(char *owner, size_t len, bool *other) {
        *other = false;
        OSIterator *it = getMatchingServices(serviceMatching("IOFramebuffer"));
        if (!it) return;
        while (OSObject *o = it->getNextObject()) {
            IOService *s = OSDynamicCast(IOService, o);
            if (!s || s->metaCast("NVRMNVDAFramebuffer")) continue;
            strlcpy(owner, s->getMetaClass()->getClassName(), len);
            // the Intel panel driver publishes as a head of this accelerator, so it is not another GPU's desktop here
            if (s->metaCast("NMIntelFB")) break;
            IORegistryEntry *e = s->getProvider();
            for (int up = 0; e && up < 8 && !e->metaCast("IOPCIDevice"); up++) e = e->getParentEntry(gIOServicePlane);
            OSData *v = e ? OSDynamicCast(OSData, e->getProperty("vendor-id")) : nullptr;
            if (v && v->getLength() >= 2) {
                const uint8_t *b = (const uint8_t *)v->getBytesNoCopy();
                *other = (uint16_t)(b[0] | (b[1] << 8)) != 0x10de;
            }
            break;
        }
        it->release();
    }
    void armHeadlessCheck() {
        int off = 0;
        if (PE_parse_boot_argn("-nvaccelnoheadless", &off, sizeof off)) { ALOG("headless check: off (-nvaccelnoheadless)"); return; }
        fHeadlessCall = thread_call_allocate(&NVAccel::headlessCheck, this);
        if (!fHeadlessCall) { ALOG("headless check: thread_call_allocate FAILED -- a GPU with no display stays hidden from Metal"); return; }
        fHeadlessWaited = 0;
        uint64_t when = 0;
        clock_interval_to_deadline(kHeadlessPollSec, kSecondScale, &when);
        thread_call_enter_delayed(fHeadlessCall, when);
        ALOG("headless check: armed, every %u s for up to %u s", kHeadlessPollSec, kHeadlessDeadlineSec);
    }
    static void headlessCheck(void *p0, void *) {
        NVAccel *me = (NVAccel *)p0;
        if (!me || me != gAccel) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const char *why = me->fFB ? "a display head published" : !me->fMetalPlugin ? "no plugin name to restore"
                        : me->getProperty("MetalPluginName") ? "the plugin name is already back" : nullptr;
        me->fHeadlessWaited += kHeadlessPollSec;
        if (why) {
            ALOG("headless check: not headless (%s) -- nothing to do", why);
        } else {
            // name the GPU that owns the display, so a log says which driver WindowServer is on
            char owner[64] = "none found";
            bool other = false;
            headlessDisplayOwner(owner, sizeof owner, &other);
            const bool early = other && headlessNoDisplayHere();
            if (!early && me->fHeadlessWaited < kHeadlessDeadlineSec) {
                uint64_t when = 0;
                clock_interval_to_deadline(kHeadlessPollSec, kSecondScale, &when);
                thread_call_enter_delayed(me->fHeadlessCall, when);
                if (me->fRegLock) IOLockUnlock(me->fRegLock);
                return;
            }
            // 2.5.3 field logs (desktops whose only GPU is this card): no head by the deadline and no other GPU showing the
            // desktop. With the name back, CoreDisplay asked this GPU's Metal plugin for WindowServer's device, the plugin
            // refuses WindowServer on a headless GPU, and WindowServer aborted at start ("Failed to create MetalDevice")
            // every ten seconds. With no other GPU there is no display to hand frames to, so the name stays withheld:
            // WindowServer runs unaccelerated on the firmware framebuffer and the desktop comes up, as before the check.
            if (!other && strcmp(owner, "NMIntelFB") != 0) {
                ALOG("headless check: no NVIDIA display head after %u s and no other GPU shows the desktop (%s) -- "
                     "MetalPluginName stays withheld so WindowServer starts on the firmware framebuffer",
                     me->fHeadlessWaited, owner);
                if (me->fRegLock) IOLockUnlock(me->fRegLock);
                return;
            }
            me->setProperty("MetalPluginName", me->fMetalPlugin);
            me->setProperty("NVAccelHeadless", kOSBooleanTrue);
            gNvHeadless = 1;
            ALOG("headless check: %s after %u s -- MetalPluginName restored WITHOUT arming: apps get "
                 "the NVIDIA Metal device, WindowServer stays on the display's GPU (%s)",
                 early ? "no display on any NVIDIA connector and another GPU shows the desktop" : "no NVIDIA display head, another GPU shows the desktop",
                 me->fHeadlessWaited, owner);
            // Not a good start for the boot guard: a card whose connectors carry no display clears the guard in the
            // framebuffer's own enumeration (nvrm-fb.cpp), with the connector list as evidence. Reaching this deadline
            // without that means the framebuffer never enumerated at all, which is a failure the guard must keep counting.
        }
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
    }
    void deliverModeChange() {
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        ALOG("  delivering display_mode_will_change/did_change (nest %u, pipes %u)",
             *(const volatile uint32_t *)(dm + 0x10c), *(const volatile uint32_t *)(dm + 0x108));
        fDM->display_mode_will_change(0);
        fDM->display_mode_did_change(0);
        reportPipeGates("after deliverModeChange");
    }
    void reportPipeGates(const char *when) {
        uint8_t accelFlags = *(const volatile uint8_t *)((const uint8_t *)this + 0xc78);
        ALOG("  GATES %s: accel+0xc78 = 0x%02x (enabled bit 0x2 %s)",
             when, accelFlags, (accelFlags & 0x2) ? "SET" : "CLEAR <- kIOReturnNotReady");
        if (!fDM) return;
        uint8_t *dm = (uint8_t *)fDM;
        uint32_t n = *(const volatile uint32_t *)(dm + 0x108);
        if (n > 8) { ALOG("  GATES %s: implausible pipe count %u -- not walking it", when, n); return; }
        for (uint32_t i = 0; i < n; i++) {
            uint8_t *p = *(uint8_t **)(dm + 0x88 + 8 * i);
            if (!p) { ALOG("  GATES %s: pipe %u is NULL", when, i); continue; }
            ALOG("  GATES %s: pipe %u active(+0x298) = %u, wsaaDefer(+0x282) = %u",
                 when, i, *(const volatile uint8_t *)(p + 0x298), *(const volatile uint8_t *)(p + 0x282));
        }
    }
    unsigned registerPendingLocked(const char *why) {
        unsigned added = 0;
        for (unsigned i = 0; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!fb) {
                // The Intel panel (index 4, NMIntelFB) does not wait for NVIDIA heads that never publish: a machine with one
                // monitor publishes only head 0, and the walk stopped there, so the Intel head got no pipe (measured 10-10,
                // published 0x11 registered 0x1). Pipes are found by their framebuffer pointer, not their number.
                if (gNvIgpu && i < 4 && ((gNvHeadsPublished >> 4) & 1)) { i = 3; continue; }
                if (gNvHeadsPublished >> i)
                    ALOG("  %s: index %u has not published (published 0x%x): stopping, so no head above it gets a pipe "
                         "this walk", why, i, gNvHeadsPublished);
                break;
            }
            if (fFBRegistered & (1u << i)) continue;
            if (i == 4 && !gNvIgpu) break;   // the Intel panel waits for debug.nvaccel_igpu=1
#ifdef NMX_ACCEL
            // IntelXeFB publishes its second framebuffer at its start (-intelxefb_pipeb), before this arm. Knob off, the
            // boot arm stops before it, as it never saw it in the late-publish days: debug.nvaccelfb=3 registers it
            if (fBootWalk && i < kNvMaxFB && nmxHeadBootSkips(i, (gNmxHead[i].caps & kNmxCapPipeFB) != 0, gNmxHeads != 0)) {
                ALOG("  %s: index %u is IntelXeFB's second framebuffer; multi-head knob off: left for `sysctl debug.nvaccelfb=3`", why, i);
                break;
            }
#endif
            unsigned b = fDM->getFramebufferCount();
            ALOG("  %s: calling found_framebuffer(%s index %u) -- count before %u", why, fb->getName(), i, b);
            fDM->found_framebuffer((IOFramebuffer *)fb);
            unsigned c = fDM->getFramebufferCount();
            fFBRegistered |= 1u << i;
            gNvHeadsRegistered = fFBRegistered;
            if (c != b + 1) {
                // IntelXeFB is on the GPU before the arm, so the family's own sweep already gave it a pipe (count stays 1):
                // that pipe is the one to use, not a refusal
                uint8_t *dm = (uint8_t *)fDM;
                unsigned np = *(const volatile uint32_t *)(dm + 0x108), k = 0;
                for (; np <= 8 && k < np; k++) {
                    void *pp = *(void *const volatile *)(dm + 0x88 + 8 * k);
                    if (pp && *(IOService *const volatile *)((const uint8_t *)pp + 0x98) == fb) break;
                }
                if (np <= 8 && k < np) {
                    ALOG("  %s: index %u already has pipe %u from the family's sweep -- using it", why, i, k);
                    gNvHeadsPiped |= 1u << i;
                    added++;
                    continue;
                }
                ALOG("  %s: index %u -- count went %u -> %u; the family did not add it (already known, or the cap "
                     "accel+0xcb0 = %u refused it). Stopping here so no later head takes this one's pipe number.",
                     why, i, b, c, *(const volatile uint32_t *)((const uint8_t *)this + 0xcb0));
                break;
            }
            uint8_t *dm = (uint8_t *)fDM;
            void *pipe = (b < 16) ? *(void *const volatile *)(dm + 0x88 + 8 * b) : nullptr;
            if (!pipe) {
                *(volatile uint32_t *)(dm + 0x108) = b;
                ALOG("  %s: index %u -- the family stored a NULL pipe in slot %u (0xe3b5, unchecked) and counted it; "
                     "count put back %u -> %u so no broadcast dereferences it (0xe320). This head gets no pipe this boot.",
                     why, i, b, c, fDM->getFramebufferCount());
                break;
            }
            IOService *pfb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
            if (pfb != fb) {
                ALOG("  %s: index %u -- pipe %u (%p) drives %p, not %s %p; keeping the pipe (it is real), stopping "
                     "the walk so no later head's pipe number is off by this one", why, i, b, pipe, pfb, fb->getName(), fb);
                break;
            }
            ALOG("  %s: index %u -> pipe %u (%p), count %u", why, i, b, pipe, c);
            gNvHeadsPiped |= 1u << i;
            added++;
        }
        return added;
    }
    void onCountGrewLocked() {
        deliverModeChange();
        gNvAccelArmed = 1;
        if (fMetalPlugin && !getProperty("MetalPluginName")) {
            setProperty("MetalPluginName", fMetalPlugin);
#ifdef NMX_NOMETAL_FALLBACK
            fNmxWithheld = false; removeProperty("NMXMetalWithheld");
#endif
            ALOG("  MetalPluginName restored -- a NEW process that asks Metal for this accelerator "
                 "can now load the plugin (already-running ones will not)");
        }
        if (!getProperty("IOGLBundleName")) {
            setProperty("IOGLBundleName", "AppleMetalOpenGLRenderer");
            ALOG("  IOGLBundleName=AppleMetalOpenGLRenderer -- NEW GL processes get a hardware renderer over Metal");
        }
        ALOG("  debug.nvaccelfb now reads 1 -- armed-only Metal clients may have a device THIS BOOT ONLY "
             "(heads published 0x%x, registered 0x%x, piped 0x%x)", gNvHeadsPublished, gNvHeadsRegistered, gNvHeadsPiped);
    }
    void registerFramebuffer(int how) {
        if (fRegLock) IOLockLock(fRegLock);
        registerFramebufferLocked(how);
        if (fRegLock) IOLockUnlock(fRegLock);
    }
    bool registerFramebufferLocked(int how) {
        if (!fDM) { ALOG("registerFramebuffer(%d): no display machine", how); return false; }
        if (!fFB) { ALOG("registerFramebuffer(%d): no framebuffer has published yet", how); return false; }
        unsigned before = fDM->getFramebufferCount(), added = 0;
        ALOG("registerFramebuffer(%d): framebuffer count before = %u", how, before);
        if (how == 2) {
            if (before != 0) ALOG("  already %u -- refusing to re-run the family's sweep", before);
            else { ALOG("  re-running IOAccelDisplayMachine::start(pci) -- the family's own sweep"); fDM->start(fPCI); }
        } else {
            added = registerPendingLocked("sysctl");
        }
        unsigned after = fDM->getFramebufferCount();
        ALOG("  framebuffer count after = %u", after);
        if (after > before || added) { onCountGrewLocked(); return true; }
        return false;
    }
    const OSSymbol *fSymAlloc = nullptr, *fSymFree = nullptr;
    IOReturn askFramebufferForVram(struct NVRMVramRequest *r, bool alloc) {
        if (!fFB) return kIOReturnNotAttached;
        if (!fSymAlloc) fSymAlloc = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_ALLOC);
        if (!fSymFree)  fSymFree  = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_FREE);
        const OSSymbol *fn = alloc ? fSymAlloc : fSymFree;
        if (!fn) return kIOReturnNoMemory;
        return fFB->callPlatformFunction(fn, false, r, nullptr, nullptr, nullptr);
    }
    IOReturn askFramebufferForScanout(struct NVRMVramRequest *r, IOService *fb = nullptr) {
        if (!fb) fb = fFB;
        if (!fb) return kIOReturnNotAttached;
        if (!fSymScanout) fSymScanout = OSSymbol::withCStringNoCopy(NVRM_VRAM_FN_SCANOUT);
        return fb->callPlatformFunction(fSymScanout, false, r, nullptr, nullptr, nullptr);
    }
    const OSSymbol *fSymScanout = nullptr;
    IOService *pipeFramebuffer(const void *pipe, unsigned *idx) {
        if (!pipe) return nullptr;
        IOService *fb = *(IOService *const volatile *)((const uint8_t *)pipe + 0x98);
        if (!fb) return nullptr;
        for (unsigned i = 0; i < kNvMaxFB; i++)
            if (fFBs[i] == fb) { if (idx) *idx = i; return fb; }
        return nullptr;
    }
    IOService *fNVRM = nullptr;
    IOReturn newUserClient(task_t owningTask, void *securityID, UInt32 type,
                           IOUserClient **handler) APPLE_KEXT_OVERRIDE {
#ifdef NMX_ACCEL
        if (!fPluginRechecked) {   // the start probe found no system copy: look once more now that user space runs
            fPluginRechecked = true;
            if (!fPluginFromSys) selectMetalPlugin("first user client");
        }
        // the Metal plugin's present client (frame path stages 2 and 3; nmxNewPresentClient at the end of this file)
        if (type == kNmxPresentType) return nmxNewPresentClient(this, owningTask, securityID, type, handler);
        // IntelXe.kext's xe connection, opened here so sandboxed Metal clients may open it (iokit-connection "IOAccelerator")
        if (type == kNmxIntelXeType) return nmxNewIntelXeClient(this, fPCI, owningTask, securityID, type, handler);
#endif
        if ((type & 0xFFFF0000u) != 0x4E560000u) {
            IOReturn fr = IOGraphicsAccelerator2::newUserClient(owningTask, securityID, type, handler);
            if (fUcLogged < 40) { fUcLogged++;
                ALOG("newUserClient(type %u / 0x%x) -> family returned 0x%x, client %s",
                     (unsigned)type, (unsigned)type, (unsigned)fr,
                     (handler && *handler) ? (*handler)->getMetaClass()->getClassName() : "(none)"); }
            return fr;
        }
#ifdef NMX_ACCEL
        // NVIDIA RM clients are NVK's; RADV and ANV talk to NMXGPU's DRM device nodes instead
        ALOG("newUserClient(0x%x): an NVIDIA RM client type on an AMD/Intel accelerator -- refused", type);
        return kIOReturnUnsupported;
#endif

        if (!fNVRM) {
            OSDictionary *m = serviceMatching("NVRM");
            if (m) fNVRM = waitForMatchingService(m, 100ULL * 1000 * 1000);
        }
        if (!fNVRM) { ALOG("newUserClient(0x%x): no NVRM service", type); return kIOReturnNotAttached; }

        const OSSymbol *fnSym = OSSymbol::withCStringNoCopy("nvNewAccelClient");
        if (!fnSym) return kIOReturnNoMemory;
        IOUserClient *uc = nullptr;
        IOReturn r = fNVRM->callPlatformFunction(fnSym, false, (void *)owningTask,
                                                 (void *)(uintptr_t)(type & 0xFFFFu), &uc, nullptr);
        fnSym->release();
        if (r != kIOReturnSuccess || !uc) { ALOG("newUserClient(0x%x): NVRM refused (0x%x)", type, r); return r ? r : kIOReturnNoResources; }

        IOService *carrier = nullptr;
        for (unsigned i = 0; i < NV_CARRIERS && !carrier; i++)
            carrier = fCarriers[(fCarrierNext + i) % NV_CARRIERS];
        fCarrierNext++;
        if (!carrier) {
            ALOG("newUserClient(0x%x): NO CARRIER available -- refusing rather than attaching to the "
                 "accelerator, which sleeps at ~1019 children", type);
            uc->release(); return kIOReturnNoResources;
        }
        if (!uc->attach(carrier))  { ALOG("newUserClient(0x%x): attach failed", type); uc->release(); return kIOReturnInternalError; }
        if (!uc->start(carrier))   { ALOG("newUserClient(0x%x): start failed", type); uc->detach(carrier); uc->release(); return kIOReturnInternalError; }
        ALOG("newUserClient(0x%x) -> %s on the ACCELERATOR (rm type %u)", type, uc->getMetaClass()->getClassName(), type & 0xFFFFu);
        *handler = uc;
        return kIOReturnSuccess;
    }
    // Registration and the mode-change delivery call IOAccelLegacyDisplayMachine, which takes the accelerator and then
    // calls back into IOGraphics for the controller gate; IOGraphics takes its gates first and then the accelerator
    // (IOFramebuffer::systemWork -> IOGraphicsAccelerator2::requestProbe). Run bare, the two orders deadlocked at boot
    // (12:48: this thread call against AppleGPUWrangler). IntelXeFB runs it inside IOGraphics' gates, in IOGraphics' order.
    static void triggerGated(void *how) { triggerUngated((int)(intptr_t)how); }
    static void triggerFromSysctl(int how) {
        IOService *fb = gAccel ? (gAccel->fFB ? gAccel->fFB : gAccel->fFBs[0]) : nullptr;
        const OSSymbol *s = fb ? OSSymbol::withCString("intelxefbRunGated") : nullptr;
        const IOReturn r = s ? fb->callPlatformFunction(s, false, reinterpret_cast<void *>(&NVAccel::triggerGated),
                                                        (void *)(intptr_t)how, nullptr, nullptr) : kIOReturnNotAttached;
        if (s) s->release();
        if (r == kIOReturnSuccess) return;
        ALOG("sysctl debug.nvaccelfb=%d: no IOGraphics gate from the framebuffer (0x%x) -- running ungated", how, r);
        triggerUngated(how);
    }
    static void triggerUngated(int how) {
        if (!gAccel) { ALOG("sysctl debug.nvaccelfb=%d but no NVAccel is running", how); return; }
        if (how >= 4) gNvAperture = 1;
        if (how >= 3) {
            gNvScanout = 1;
            ALOG("  level %d: registering any published head that has no pipe yet", how);
            if (gAccel->fRegLock) IOLockLock(gAccel->fRegLock);
            if (gAccel->registerFramebufferLocked(1))
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED; the registration just delivered the mode "
                     "change -- not delivering it twice", how);
            else {
                ALOG("sysctl debug.nvaccelfb=%d: scanout-backed display pipe ENABLED, re-delivering the mode change", how);
                gAccel->deliverModeChange();
            }
            if (gAccel->fRegLock) IOLockUnlock(gAccel->fRegLock);
            return;
        }
        gAccel->registerFramebuffer(how);
    }
#ifdef NMX_ACCEL
    // ---- more than one IntelXeFB head (docs/design/DESIGN-multihead-accel.md), behind gNmxHeads ----------------------
    // A head that publishes after the arm (IntelXeFB lights its second pipe seconds after the first framebuffer, or on a
    // hotplug) is registered by a thread call: what `sysctl debug.nvaccelfb=3` does by hand, for that head only, inside
    // IOGraphics' system and controller gates (head 0's intelxefbRunGated; the second framebuffer is on the same
    // controller). Never ungated: run bare, registration deadlocked against AppleGPUWrangler at boot (boot1d).
    thread_call_t fHeadsCall = nullptr;
    IONotifier *fFBTermNote = nullptr;
    volatile SInt32 fHeadsBusy = 0, fHeadsAgain = 0;
    unsigned fHeadsRetries = 0;
    char fHeadsLast[200] = "never ran";
    static void headsFire(thread_call_param_t p0, thread_call_param_t) { ((NVAccel *)p0)->headsLate(); }
    // knob on (start or the sysctl): the late-head thread call and the termination watch; idempotent
    bool headsEnable(const char *why) {
        if (!fHeadsCall) fHeadsCall = thread_call_allocate(&NVAccel::headsFire, this);
        if (!fHeadsCall) { ALOG("multi-head: thread_call_allocate FAILED -- a head published after the arm stays manual"); return false; }
        if (!fFBTermNote) {
            if (OSDictionary *m = serviceMatching("IntelXeFramebuffer")) {
                fFBTermNote = addMatchingNotification(gIOTerminatedNotification, m, &NVAccel::fbTerminated, this, nullptr, 0);
                m->release();
            }
        }
        ALOG("multi-head: on (%s, mode %d): a head published after the arm is registered on its own%s", why, gNmxHeads,
             fFBTermNote ? "" : " (no termination watch)");
        return true;
    }
    void headsKick(const char *why, uint32_t delayMs) {
        if (!gNmxHeads || !fHeadsCall) return;
        fHeadsRetries = 0;
        uint64_t when = 0;
        clock_interval_to_deadline(delayMs ? delayMs : 1, kMillisecondScale, &when);
        thread_call_enter_delayed(fHeadsCall, when);
        ALOG("multi-head: late-head check in %u ms (%s)", delayMs, why);
    }
    void headsAccelIn(NmxAccelIn *a) const {
        a->knob = gNmxHeads != 0;
        a->armed = gNvAccelArmed != 0;
        a->armPending = gNmxAuto && (gNmxAutoState == 1 || gNmxAutoState == 2);
        a->publishedMask = gNvHeadsPublished;
    }
    // what is known about head i; q (optional) gets its nvScanoutInfo answer
    void headIn(unsigned i, NmxHeadIn *h, struct NVRMVramRequest *q) {
        IOService *fb = i < kNvMaxFB ? fFBs[i] : nullptr;
        h->published = fb != nullptr && ((gNvHeadsPublished >> i) & 1u);
        h->registered = h->published && ((fFBRegistered >> i) & 1u);
        h->piped = h->registered && ((gNvHeadsPiped >> i) & 1u);
        h->terminated = i < kNvMaxFB && gNmxHead[i].terminated != 0;
        h->pipeFB = i < kNvMaxFB && (gNmxHead[i].caps & kNmxCapPipeFB) != 0;
        h->disconnected = fb && fb->getProperty("IntelXeFBConnected") == kOSBooleanFalse;   // absent (head 0): connected
        h->scanoutOk = false;
        if (q) {
            bzero(q, sizeof *q);
            q->version = NVRM_VRAM_ABI_VERSION;
            h->scanoutOk = fb && askFramebufferForScanout(q, fb) == kIOReturnSuccess && q->kva && q->width && q->height && q->pitch;
        }
    }
    unsigned fLateWant = 0;
    void headsLate() {
        if (!OSCompareAndSwap(0, 1, (volatile UInt32 *)&fHeadsBusy)) { fHeadsAgain = 1; return; }
        for (;;) {
            fHeadsAgain = 0;
            headsLateOnce();
            if (!fHeadsAgain) break;
        }
        fHeadsBusy = 0;
    }
    void headsLateOnce() {
        if (!gNmxHeads) return;
        NmxAccelIn a; headsAccelIn(&a);
        unsigned want = 0, notReady = 0;
        for (unsigned i = 1; i < kNvMaxFB; i++) {
            NmxHeadIn h; struct NVRMVramRequest q;
            headIn(i, &h, &q);
            const int v = nmxHeadVerdict(i, h, a);
            if (v == kNmxHeadLateArm) want |= 1u << i;
            else if (v == kNmxHeadNotReady) notReady |= 1u << i;
        }
        // not while its own registration and matching are still running (boot1c: arming 6 ms after a publish hung)
        for (unsigned i = 1; i < kNvMaxFB; i++) {
            IOService *fb = fFBs[i];
            if (!((want >> i) & 1u) || !fb) continue;
            const IOReturn qr = fb->waitQuiet(5ull * 1000 * 1000 * 1000);
            if (qr != kIOReturnSuccess) { want &= ~(1u << i); notReady |= 1u << i; ALOG("multi-head: head %u not quiet in 5 s (0x%x)", i, qr); }
        }
        if (want) {
            IOService *fb0 = fFB;
            const OSSymbol *s = fb0 ? OSSymbol::withCString("intelxefbRunGated") : nullptr;
            fLateWant = want;
            const IOReturn r = s ? fb0->callPlatformFunction(s, false, reinterpret_cast<void *>(&NVAccel::lateGated), this, nullptr, nullptr)
                                 : kIOReturnNotAttached;
            if (s) s->release();
            fLateWant = 0;
            if (r != kIOReturnSuccess) {
                snprintf(fHeadsLast, sizeof fHeadsLast, "heads 0x%x: no IOGraphics gate from head 0 (0x%x): not registered (never ungated)", want, r);
                ALOG("multi-head: %s", fHeadsLast);
                notReady |= want;
            }
        }
        if (notReady && fHeadsRetries < 15) {   // a scan-out that is not up yet, a head still matching: every 2 s for 30 s
            fHeadsRetries++;
            uint64_t when = 0; clock_interval_to_deadline(2000, kMillisecondScale, &when);
            if (fHeadsCall) thread_call_enter_delayed(fHeadsCall, when);
        } else if (notReady) {
            snprintf(fHeadsLast, sizeof fHeadsLast, "heads 0x%x never became ready in 30 s: left for `sysctl debug.nvaccelfb=3`", notReady);
            ALOG("multi-head: %s", fHeadsLast);
        } else if (!want && !strncmp(fHeadsLast, "never ran", 9)) {
            snprintf(fHeadsLast, sizeof fHeadsLast, "nothing to register (published 0x%x, registered 0x%x)", gNvHeadsPublished, fFBRegistered);
        }
    }
    // inside IOGraphics' system and controller gates (IntelXeFB runInFcGate)
    static void lateGated(void *p) {
        NVAccel *me = (NVAccel *)p;
        if (!me || !me->fDM) return;
        if (me->fRegLock) IOLockLock(me->fRegLock);
        const unsigned regBefore = me->fFBRegistered, pipedBefore = gNvHeadsPiped;
        const unsigned before = me->fDM->getFramebufferCount();
        const unsigned added = me->registerPendingLocked("late head");
        // mode 1: the family-wide mode change the boot arm and debug.nvaccelfb=3 deliver; mode 2: none (the new pipe gets
        // its own framebuffer's mode changes through its IOFramebuffer notification, group kIOFBNotifyGroupID_AppleIOAccelDisplayPipe)
        const bool modeChange = added && gNmxHeads != 2;
        if (modeChange) me->onCountGrewLocked();
        const unsigned after = me->fDM->getFramebufferCount();
        const unsigned newReg = me->fFBRegistered & ~regBefore, newPiped = gNvHeadsPiped & ~pipedBefore;
        for (unsigned i = 0; i < kNvMaxFB; i++) if ((newReg >> i) & 1u) {
            gNmxHead[i].armedAt = mach_absolute_time();
            gNmxHead[i].lateResult = ((newPiped >> i) & 1u) ? 1 : -1;
        }
        snprintf(me->fHeadsLast, sizeof me->fHeadsLast, "wanted 0x%x: registered 0x%x -> 0x%x, piped 0x%x -> 0x%x, framebuffers %u -> %u, mode change %s",
                 me->fLateWant, regBefore, me->fFBRegistered, pipedBefore, gNvHeadsPiped, before, after,
                 modeChange ? "delivered" : added ? "not delivered (mode 2)" : "none (nothing added)");
        if (me->fRegLock) IOLockUnlock(me->fRegLock);
        ALOG("multi-head: %s", me->fHeadsLast);
        if (newReg)
            ALOG("  WindowServer builds its display pipes from NMXAGDC's 0x980 reply when it starts: a head registered after "
                 "that is composited once WindowServer asks again (debug.nvrmfb_agdc_maxfb: the count it last got)");
    }
    // knob on: a head (not 0) whose framebuffer terminates frees its slot, so a new framebuffer at that index is registered
    // again. Its reference is kept: a transaction in flight may still hold the pointer (as fbPublished keeps a displaced one).
    static bool fbTerminated(void *target, void *ref, IOService *s, IONotifier *note) {
        (void)ref; (void)note;
        NVAccel *me = (NVAccel *)target;
        if (!me || !s || !gNmxHeads) return true;
        for (unsigned i = 1; i < kNvMaxFB; i++) {
            if (me->fFBs[i] != s || !OSCompareAndSwapPtr(s, nullptr, (void *volatile *)&me->fFBs[i])) continue;
            OSBitAndAtomic(~(1u << i), (volatile UInt32 *)&gNvHeadsPublished);
            OSBitAndAtomic(~(1u << i), (volatile UInt32 *)&me->fFBRegistered);
            OSBitAndAtomic(~(1u << i), (volatile UInt32 *)&gNvHeadsPiped);
            gNvHeadsRegistered = me->fFBRegistered;
            gNmxHead[i].caps = 0;
            OSIncrementAtomic((volatile SInt32 *)&gNmxHead[i].terminated);
            ALOG("multi-head: head %u (%s) terminated -- slot freed (its reference is kept)", i, s->getName());
        }
        return true;
    }
#endif
    static void enableFromHook(const char *why) {
        if (!gAccel) return;
        gAccel->enableAccelerator();
        gAccel->fEnableCount++;
        ALOG("enableAccelerator() from %s (call #%u)", why, gAccel->fEnableCount);
        gAccel->setProperty("NVAccelEnabledFrom", why);
        gAccel->setProperty("NVAccelEnableCount", gAccel->fEnableCount, 32);
    }
    unsigned fEnableCount = 0;
    unsigned fUcLogged = 0;
    virtual IOService *probe(IOService *provider, SInt32 *score) APPLE_KEXT_OVERRIDE {
        ALOG("probe(%s) score %d", provider ? provider->getName() : "?", score ? (int)*score : 0);
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            const unsigned cls = pci->configRead8(0x0B), sub = pci->configRead8(0x0A);
            const unsigned ven = pci->configRead16(0x00), dev = pci->configRead16(0x02);
#ifdef NMX_ACCEL
            static const char *const kind = "AMD/Intel";
            if (cls != 0x03 || (ven != 0x1002 && ven != 0x8086)) {
#else
            static const char *const kind = "NVIDIA";
            if (cls != 0x03 || (sub != 0x00 && sub != 0x02) || ven != 0x10de || dev < 0x0020) {
#endif
                ALOG("%04x:%04x class %02x.%02x is not an %s display device we support - not claiming",
                     ven, dev, cls, sub, kind);
                return NULL;
            }
            ALOG("%s display device %04x:%04x class %02x.%02x - claiming", kind, ven, dev, cls, sub);
        }
        return IOGraphicsAccelerator2::probe(provider, score);
    }
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE {
        { if (nvrm_driver_off()) return false; }
#ifdef NMX_ACCEL
        // NMXGPU decides which AMD/Intel GPUs are driven (nmxamd=1 / nmxintel=1 / nullmoth-gpu); this accelerator only
        // starts on a device it has claimed, so a GPU macOS drives itself never gets a second accelerator
        // No waiting here: NMXGPU attaches from a thread once the disks with its firmware are mounted, and mounting waits
        // for matching to settle - a start() that blocked matching for the claim would hold the boot at the logo. NMXGPU
        // re-runs matching on the device (registerService) after it sets nmxgpu-claimed, which offers it here again.
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            if (pci->getProperty("nmxgpu-claimed") != kOSBooleanTrue) {
                ALOG("NMXGPU has not claimed %04x:%04x yet - not starting now; offered again when it does",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NMXGPU drives %04x:%04x - starting", pci->configRead16(0x00), pci->configRead16(0x02));
        }
#else
        uint32_t gate = 0;
        if (!PE_parse_boot_argn("nvaccel", &gate, sizeof gate) || !gate) { ALOG("boot-arg nvaccel=1 absent: NOT starting (boot-loop guard)"); return false; }
        if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider)) {
            unsigned waited = 0;
            while (pci->getProperty("nvrm-claimed") != kOSBooleanTrue && waited < 300) { IOSleep(100); waited++; }
            if (pci->getProperty("nvrm-claimed") != kOSBooleanTrue) {
                ALOG("NVRM never claimed %04x:%04x in 30 s - NOT starting the accelerator on a GPU with no RM",
                     pci->configRead16(0x00), pci->configRead16(0x02));
                return false;
            }
            ALOG("NVRM owns %04x:%04x (waited %u ms) - starting", pci->configRead16(0x00), pci->configRead16(0x02), waited * 100);
        }
#endif
        ALOG("start(%s): calling IOGraphicsAccelerator2::start — the family's own bring-up", provider ? provider->getName() : "?");
        bool r = IOGraphicsAccelerator2::start(provider);
        ALOG("IOGraphicsAccelerator2::start -> %d", r);
        if (r) {
            // Tells the Metal plugin that NVResource::pageoff waits for a surface WindowServer's GPU is still writing
            // (nvSurfWaitReady). Only then does the plugin mark WindowServer's surfaces newest in VRAM at commit; with an
            // accelerator that does not wait, that mark would let a CPU lock or a copied present read a surface before
            // its pixels landed, so the plugin keeps marking them after the GPU finishes.
            setProperty("NVAccelSurfaceWait", 1ULL, 32);
#ifdef NMX_ACCEL
            // and on the B580 for every surface, not only those with a VRAM copy (NVResource::pageoff): the plugin
            // then folds WindowServer's surface-out into the writing job and drops its commit-time capture wait
            setProperty("NMXSurfaceWaitAll", 1ULL, 32);
            if (IOPCIDevice *pci = OSDynamicCast(IOPCIDevice, provider))
                if (pci->configRead16(0x00) == 0x8086) setProperty(kNmxIntelXeClientKey, (unsigned long long)kNmxIntelXeType, 32);
            selectMetalPlugin("start");   // before registerService: the first Metal client already sees the choice
#endif
#ifdef NMX_ACCEL
            {
                char d[8]; int early = 0;
                IOPCIDevice *pd = OSDynamicCast(IOPCIDevice, provider);
                gNmxAuto = pd && pd->configRead16(0x00) == 0x8086 && !intelxeBootArg("NMXAccel", "-intelxe_noaccel", d, sizeof d) &&
                           !PE_parse_boot_argn("-x", d, sizeof d);
                PE_parse_boot_argn("nmxearlyreg", &early, sizeof early);
                if (gNmxAuto) {
                    fAutoCall = thread_call_allocate(&NVAccel::autoFire, this);
                    fRegDeadline = thread_call_allocate(&NVAccel::regDeadlineFire, this);
                    if (!fAutoCall || !fRegDeadline) gNmxAuto = 0;
                }
                char heldWhy[160];
                if (gNmxAuto && nmxGuardHolds(heldWhy, sizeof heldWhy)) {
                    gNmxAutoState = -1;
                    char m[200]; snprintf(m, sizeof m, "held at start, NOT registered (WindowServer runs on IntelXeFB alone): %s", heldWhy);
                    autoNote(m);
                } else if (gNmxAuto) {
                    gNmxAutoState = 1;
                    uint64_t when = 0;
                    clock_interval_to_deadline(30, kSecondScale, &when);
                    thread_call_enter_delayed(fRegDeadline, when);
                    // boot1d: registered at start again, as every working arm so far (intelxe-accel.sh) had it. boot1c held it back
                    // until armed and hung inside the arm (03:48); debug.nmxaccelfb reads 1 while the arm is pending instead,
                    // so the Metal plugin still gives WindowServer its B580 device
                    (void)early;
                    autoNote("waiting for IntelXeFB (registered at start; debug.nmxaccelfb reads 1 while the arm is pending)");
                    registerOnce("start");
                } else {
                    setProperty("NMXAutoArm", "off (-intelxe_noaccel, safe mode or not a B580)");
                    registerOnce("start");
#ifdef NMX_NOMETAL_FALLBACK
                    // WindowServer is refused from the start here; the other GPU's accelerator may still be starting, so
                    // decide after 20 s (chosen: the iGPU accelerator and WindowServer's first start are both earlier)
                    if ((fNoMetalCall = thread_call_allocate(&NVAccel::noMetalFire, this))) {
                        uint64_t when = 0; clock_interval_to_deadline(20, kSecondScale, &when);
                        thread_call_enter1_delayed(fNoMetalCall, (thread_call_param_t)"auto-arm off", when);
                    }
#endif
                }
            }
#else
            registerService(); ALOG("registerService() called — Metal can now find MetalPluginName=%s", "NVMTLDriver");
#endif
            gAccel = this; gEnableAccel = &NVAccel::enableFromHook;
            for (unsigned i = 0; i < NV_CARRIERS; i++) {
                IOAccelNVRMCarrier *c = OSTypeAlloc(IOAccelNVRMCarrier);
                if (!c) break;
                if (!c->init()) { c->release(); break; }
                if (!c->attach(this)) { c->release(); break; }
                if (!c->start(this)) { c->detach(this); c->release(); break; }
                fCarriers[i] = c;
            }
            { unsigned n = 0; for (unsigned i = 0; i < NV_CARRIERS; i++) if (fCarriers[i]) n++;
              ALOG("user-client carriers: %u of %u ready (each holds ~1019 clients before "
                   "IOService::attach starts sleeping)", n, (unsigned)NV_CARRIERS); }
            enableFromHook("start");
            fPCI = OSDynamicCast(IOPCIDevice, provider);
#ifndef NMX_ACCEL   // NVIDIA TSENS register (BAR0 + 0x20460): another vendor's BAR0 holds something else there
            startTemperature();
#endif
            ALOG("display machine framebuffer count after the family sweep = %u", fDM ? fDM->getFramebufferCount() : 0xFFFFFFFFu);
            fRegLock = IOLockAlloc();
            ALOG("multi-head: up to %u heads, registration lock %p%s", kNvMaxFB, fRegLock,
                 fRegLock ? "" : " -- NULL: two sysctl writers could interleave a registration");
#ifdef NMX_ACCEL
            // AMD/Intel: the Metal device is for apps; WindowServer draws into NMXGPU's framebuffer, and the plugin copies
            // finished frames into the IOSurfaces it reads (the headless path NVIDIA laptops use)
            nvaccelInstallSysctl();
            gNvHeadless = 1;
            setProperty("NMXAccelHeadless", kOSBooleanTrue);
            ALOG("AMD/Intel accelerator up: Metal device for apps, frames copied to the framebuffer WindowServer draws");
            // B580: its framebuffer (IntelXeFB, index 0) becomes head 0 of this accelerator, so WindowServer can composite
            // on the B580 once debug.nmxaccelfb / debug.nmxaccel_iop are written; frames reach the panel through the
            // display pipe's copy path (performTransaction -> IntelXeFB nvScanoutInfo)
            // the multi-head knob before the publish watch: a head already published is delivered from inside it
            gNmxHeads = nmxSetting("nmxheads", "nmx-heads", 0);
            if (gNmxHeads < 0 || gNmxHeads > 2) gNmxHeads = 0;
            if (gNmxHeads && !headsEnable("nmxheads / nmx-heads")) gNmxHeads = 0;
            OSDictionary *fbm = serviceMatching("IntelXeFramebuffer");
#else
            OSDictionary *fbm = serviceMatching("NVRMNVDAFramebuffer");
#endif
            if (fbm) {
                fFBNote = addMatchingNotification(gIOFirstPublishNotification, fbm, &NVAccel::fbPublished, this, nullptr, 0);
                fbm->release();
                ALOG("watching for NVRMFramebuffer to publish -> notifier %p", fFBNote);
#ifndef NMX_ACCEL
                // a laptop whose built-in panel is on the Intel GPU: NMIntelFB publishes as head 4 of this accelerator
                if (OSDictionary *im = serviceMatching("NMIntelFB")) {
                    fIntelNote = addMatchingNotification(gIOFirstPublishNotification, im, &NVAccel::fbPublished, this, nullptr, 0);
                    im->release();
                    ALOG("watching for an Intel built-in panel (NMIntelFB) -> notifier %p", fIntelNote);
                }
                nvaccelInstallSysctl();
                if (getProperty("MetalPluginName")) {
                    OSObject *mp = getProperty("MetalPluginName"); if (mp) mp->retain();
                    fMetalPlugin = mp;
                    removeProperty("MetalPluginName");
                    ALOG("MetalPluginName withheld -- nothing will load the Metal plugin until "
                         "`sysctl -w debug.nvaccelfb=1` puts it back, and a reboot takes it away again");
                    armHeadlessCheck();
                }
#endif   // NMX: apps keep the Metal device from the start; WindowServer is gated by the plugin's allow-list instead
            } else ALOG("serviceMatching(NVRMFramebuffer) returned NULL -- cannot watch for the framebuffer");
        }
        return r;
    }
    // ---- GPU core temperature ------------------------------------------------
    // Published into our own PerformanceStatistics as "Temperature(C)", the key iStat Menus
    // shows on the GPU panel. NVAccel already owns the IOPCIDevice, so no second kext and no
    // helper process are involved: that is the whole point of doing it here.
    void startTemperature() {
        if (fTempTimer || !fPCI) return;
        IOWorkLoop *wl = getWorkLoop();
        if (!wl) { ALOG("temperature: no work loop -- no temperature will be published"); return; }
        fTempTimer = IOTimerEventSource::timerEventSource(this, &NVAccel::tempTimerFired);
        if (!fTempTimer) { ALOG("temperature: timerEventSource FAILED"); return; }
        if (wl->addEventSource(fTempTimer) != kIOReturnSuccess) {
            ALOG("temperature: addEventSource FAILED");
            fTempTimer->release(); fTempTimer = nullptr; return;
        }
        fTempTimer->setTimeoutMS(kNvTempFirstMs);
        ALOG("temperature: will read TSENS 0x%05x every %u ms", kNvTsensOffset, kNvTempIntervalMs);
    }

    void stopTemperature() {
        if (fTempTimer) {
            fTempTimer->cancelTimeout();
            if (IOWorkLoop *wl = getWorkLoop()) wl->removeEventSource(fTempTimer);
            fTempTimer->release(); fTempTimer = nullptr;
        }
        if (fTempMap) { fTempMap->release(); fTempMap = nullptr; }
        fTempRegs = nullptr;
    }

    static void tempTimerFired(OSObject *owner, IOTimerEventSource *sender) {
        (void)sender;
        // Registered with `this` as the owner, so the cast cannot fail; OSDynamicCast is not
        // used because it needs the complete class, and this body sits inside the class.
        NVAccel *me = (NVAccel *)owner;
        if (!me) return;
        me->sampleTemperature();
        if (me->fTempTimer) me->fTempTimer->setTimeoutMS(kNvTempIntervalMs);
    }

    void sampleTemperature() {
        if (!fTempRegs) {
            fTempMap = fPCI->mapDeviceMemoryWithRegister(kIOPCIConfigBaseAddress0, 0);
            if (!fTempMap) { ALOG("temperature: BAR0 map FAILED -- giving up"); return; }
            fTempRegs = (volatile UInt32 *)fTempMap->getVirtualAddress();
            if (!fTempRegs) { ALOG("temperature: BAR0 has no virtual address -- giving up"); return; }
        }
        UInt32 raw = fTempRegs[kNvTsensOffset / 4];
        unsigned val = (raw & kNvTsensValueMask) >> 8;
        // A GPU in a low power state can answer 0xffffffff, and 0 means the sensor has not been
        // clocked yet: both fall outside the range and are left unpublished rather than guessed.
        if (raw == 0xffffffffu || val < kNvTempMinC || val > kNvTempMaxC) {
            if (fTempFails++ == 0)
                ALOG("temperature: TSENS raw=0x%08x outside %u..%u C -- not publishing",
                     raw, kNvTempMinC, kNvTempMaxC);
            return;
        }
        fTempFails = 0;
        if (!fTempLogged) { fTempLogged = true; ALOG("temperature: first reading raw=0x%08x -> %u C", raw, val); }
        fTempC = (int)val;
        publishTemperature((unsigned)fTempC);
    }

    // Read-copy-modify-write: the family keeps 70-odd keys in this dictionary and replaces it
    // wholesale every 0.25-2 s, so the copy is taken from whatever is in it at this moment.
    void publishTemperature(unsigned c) {
        OSDictionary *cur = OSDynamicCast(OSDictionary, getProperty("PerformanceStatistics"));
        OSDictionary *d = cur ? OSDynamicCast(OSDictionary, cur->copyCollection()) : nullptr;
        if (!d) { d = OSDictionary::withCapacity(8); if (!d) return; }
        OSNumber *n = OSNumber::withNumber((unsigned long long)c, 32);
        if (n) { d->setObject("Temperature(C)", n); n->release(); setProperty("PerformanceStatistics", d); }
        d->release();
    }

    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE {
        ALOG("stop");
#ifdef NMX_ACCEL
        // shutdown breadcrumb for the kernel agent (read with `nvram nmxaccel-shutdown`; nothing clears it but the next stop)
        if (IORegistryEntry *o = IORegistryEntry::fromPath("/options", gIODTPlane)) {
            if (OSString *v = OSString::withCString("1 NMXAccel stop")) { o->setProperty("nmxaccel-shutdown", v); v->release(); }
            // no SYNCNOW key: x86's AppleEFINVRAM stored it as a variable (IONVRAM-SYNCNOW-PROPERTY) and synced nothing; a
            // clean shutdown writes NVRAM anyway
            o->release();
        }
        if (fRegDeadline) thread_call_cancel(fRegDeadline);
        if (fAutoCall) thread_call_cancel(fAutoCall);
        if (fHeadsCall) thread_call_cancel(fHeadsCall);
        if (fFBTermNote) { fFBTermNote->remove(); fFBTermNote = nullptr; }
#endif
        stopTemperature();
        IOGraphicsAccelerator2::stop(provider);
    }
    virtual IOReturn callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVAccel, IOGraphicsAccelerator2)
NVAccel *NVAccel::gAccel = nullptr;

IOAccelMemory *NVDisplayPipe::initFramebufferResource(unsigned int index, IOAccelResource2 *res)
{
    if (!gNvScanout) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): scanout gate CLOSED -- returning NULL, "
             "which is the family's own default (sysctl debug.nvaccelfb=3 opens it)", index);
        return nullptr;
    }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !res) { ALOG("NVDisplayPipe::initFramebufferResource: no accelerator or no resource"); return nullptr; }
    unsigned head = 0;
    IOService *fb = a->pipeFramebuffer(this, &head);
    if (!fb) {
        ALOG("NVDisplayPipe::initFramebufferResource(index %u): pipe %p drives framebuffer %p, which is none of ours -- "
             "refusing (never head 0's pages: that IS the bug)", index, this,
             *(void *const volatile *)((const uint8_t *)this + 0x98));
        return nullptr;
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&r, fb);
    if (ret == kIOReturnSuccess && r.kva && !r.phys) {
        // Another GPU's panel (NMIntelFB on a laptop without a MUX): its scanout is Intel memory with a CPU mapping and
        // no NVIDIA VRAM address. No vidmem may stand for it (addToAperture/getApertureMemoryDescriptor would hand
        // BAR1 offset 0 to the GPU), and none may be allocated in its place (fresh VRAM mapped here is the 10-10
        // os_map_kernel_space hang shape). NULL is the family's own default: frames reach this panel only through
        // performTransaction's copy into kva (debug.nvaccel_iop_ok_h4 counts them).
        ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u): CPU-only scanout %u x %u pitch %u (another GPU's "
             "panel) -- no framebuffer resource, frames are copied", head, index, r.width, r.height, r.pitch);
        return nullptr;
    }
    if (ret != kIOReturnSuccess || !r.phys || !r.actualSize) {
        ALOG("NVDisplayPipe::initFramebufferResource: framebuffer would not describe its scanout (0x%x)", (unsigned)ret);
        return nullptr;
    }

    OSObject *o = a->createVidMemory(nullptr, res, r.actualSize, nullptr);
    NVVidMemory *m = OSDynamicCast(NVVidMemory, o);
    if (!m) {
        ALOG("NVDisplayPipe::initFramebufferResource: createVidMemory gave %p, not an NVVidMemory", o);
        if (o) o->release();
        return nullptr;
    }
    m->fScanout = true;
    m->fPhys    = r.phys;
    m->fBytes   = r.actualSize;
    *(volatile unsigned long long *)((unsigned char *)m + 0xc8) = r.phys;
    ALOG("NVDisplayPipe::initFramebufferResource(head %u, index %u, res %p) -> vidmem %p over the LIVE SCANOUT "
         "phys 0x%llx %u x %u pitch %u (%llu bytes)",
         head, index, res, m, (unsigned long long)r.phys, r.width, r.height, r.pitch,
         (unsigned long long)r.actualSize);
    return (IOAccelMemory *)m;
}

static NVVidMemory *nvres_vidmem(IOAccelResource2 *r)
{
    if (!r) return nullptr;
    OSObject *o = *(OSObject **)((unsigned char *)r + 0x88);
    return OSDynamicCast(NVVidMemory, o);
}

bool NVResource::addToAperture()
{
    NVVidMemory *m = nvres_vidmem(this);
    if (fApLogged < 4) { fApLogged++;
        ALOG("NVResource::addToAperture -> true (vidmem %p phys 0x%llx %llu bytes, already in BAR1)",
             m, m ? m->fPhys : 0ull, m ? m->fBytes : 0ull); }
    return m && m->fPhys && m->fBytes;
}

void NVResource::removeFromAperture() { }

void *NVResource::getApertureMemoryDescriptor(unsigned long long *a0)
{
    if (a0) *a0 = 0;
    if (!gNvAperture) {
        if (fApLogged < 4) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: aperture gate CLOSED -- returning nullptr "
                 "(sysctl debug.nvaccelfb=4 opens it)"); }
        return nullptr;
    }
    if (fApDesc) return fApDesc;
    NVVidMemory *m = nvres_vidmem(this);
    if (!m || !m->fPhys || !m->fBytes) {
        if (fApLogged < 8) { fApLogged++;
            ALOG("NVResource::getApertureMemoryDescriptor: no BAR1-backed vidmem at +0x88 (mem %p)", m); }
        return nullptr;
    }
    IODeviceMemory *d = IODeviceMemory::withRange((IOPhysicalAddress)m->fPhys, (IOPhysicalLength)m->fBytes);
    if (!d) { ALOG("NVResource::getApertureMemoryDescriptor: IODeviceMemory::withRange(0x%llx, %llu) FAILED",
                   m->fPhys, m->fBytes); return nullptr; }
    fApDesc = d;
    if (fApLogged < 8) { fApLogged++;
        ALOG("NVResource::getApertureMemoryDescriptor -> %p over BAR1 phys 0x%llx (%llu bytes), offset 0",
             d, m->fPhys, m->fBytes); }
    return fApDesc;
}

static void nvSVForgetResource(NVResource *resource);

void NVResource::free()
{
    nvSVForgetResource(this);
    if (fApDesc) { fApDesc->release(); fApDesc = nullptr; }
    OSAddAtomic64(1, &gCntResFree);
    IOAccelResource2::free();
}

// 1 = a surface keeps its VRAM across the family's deallocPhysical/allocPhysical (see deallocPhysical).
static int gVmSticky = 1;
static SInt64 gCntVmParked, gCntVmUnparked, gCntVmParkReleased;

#ifdef NMX_ACCEL
static int nmxSVAdoptPending(NVVidMemory *vm);
static void nmxSVRelease(NVVidMemory *vm);
#endif
bool NVVidMemory::allocPhysical()
{
    if (fScanout) {
        if (!fPhys || !fBytes) { ALOG("NVVidMemory::allocPhysical(%p): scanout with no address", this); return false; }
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
        ALOG("NVVidMemory::allocPhysical(%p): SCANOUT phys 0x%llx (%llu bytes) -- already backed, nothing allocated",
             this, fPhys, fBytes);
        return true;
    }
#ifdef NMX_ACCEL
    if (fIntelXe) {   // re-wired after the family unwired it: the same registry VRAM (plugins keep drawing into it)
        fRmHandle = fIntelXe;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return true;
    }
    // nmxSurfVram is wiring this vidmem: IntelXe's registry backs it, or nothing does (a refusal must not fall through to
    // IntelXeFB's system memory: the surface would then have another backing and every later import answer EBUSY)
    if (const int ad = nmxSVAdoptPending(this)) return ad > 0;
#endif
    if (fRmHandle) {
        ALOG("NVVidMemory::allocPhysical(%p): already backed by 0x%llx (%llu bytes) -- false, as AMD's does",
             this, fPhys, fBytes);
        return false;
    }
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)this + 0x40);
    if (!want) { ALOG("NVVidMemory::allocPhysical(%p): length at +0x40 is 0 -- nothing to back", this); return false; }
    if (!NVAccel::gAccel) { ALOG("NVVidMemory::allocPhysical(%p): no accelerator", this); return false; }
    if (fParkHandle) {
        if (fParkBytes >= want) {
            fRmHandle = fParkHandle; fKva = fParkKva; fPhys = fParkPhys; fBytes = fParkBytes; fAllocationCookie = fParkCookie;
            fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
            *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
            OSAddAtomic64(1, &gCntVmUnparked);
            return true;
        }
        releaseParked();   // the resource grew: its old VRAM cannot hold it
    }

    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.size    = want;
    IOReturn ret = NVAccel::gAccel->askFramebufferForVram(&r, true);
    if (ret != kIOReturnSuccess || !r.handle || !r.phys) {
        // A full BAR1 window refuses every frame (~100/s on a 256 MB BAR1, a third of everything the machine logs:
        // #387). Same throttle as the framebuffer's nvAllocVram: the first eight, then every 256th with the count;
        // debug.nvaccel_vm_refused keeps the exact total.
        const SInt64 nRefused = OSAddAtomic64(1, &gCntVmRefused) + 1;
        if (nRefused <= 8 || (nRefused & 255) == 0)
            ALOG("NVVidMemory::allocPhysical(%p): %llu bytes REFUSED by the framebuffer (0x%x, #%lld)",
                 this, want, (unsigned)ret, (long long)nRefused);
        // once per boot: how much of the window this side holds as sticky parks (kept across the family's dealloc)
        static volatile SInt32 said = 0;
        if (OSCompareAndSwap(0, 1, &said))
            ALOG("VRAM refused: %lld allocs (%lld bytes), %lld deallocs (%lld bytes), %lld parked now",
                 (long long)gCntVmAlloc, (long long)gCntVmAllocBytes, (long long)gCntVmDealloc, (long long)gCntVmDeallocBytes,
                 (long long)(gCntVmParked - gCntVmUnparked - gCntVmParkReleased));
        return false;
    }
    fRmHandle = r.handle; fKva = r.kva; fPhys = r.phys; fBytes = r.actualSize; fAllocationCookie = r.allocationCookie;
    OSAddAtomic64(1, &gCntVmAlloc); OSAddAtomic64((SInt64)fBytes, &gCntVmAllocBytes);
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = fPhys;
    ALOGV("NVVidMemory::allocPhysical(%p): %llu bytes of REAL VRAM -- phys 0x%llx kva %p [BAR1 %llu]",
         this, fBytes, fPhys, fKva, r.mappedTotal);
    return true;
}

void NVVidMemory::deallocPhysical()
{
    if (fScanout) {
        ALOG("NVVidMemory::deallocPhysical(%p): SCANOUT -- not ours to free, only forgetting it", this);
        fPhys = 0; fBytes = 0; fScanout = false;
        *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
        return;
    }
#ifdef NMX_ACCEL
    // Kept, as debug.nvaccel_vm_sticky keeps NVIDIA VRAM: plugins imported this backing and keep drawing into it, so the
    // family's next allocPhysical must hand back the same one. The registry reference goes with the vidmem (free()).
    if (fIntelXe) { fRmHandle = nullptr; return; }
#endif
    if (!fRmHandle) return;
    OSAddAtomic64(1, &gCntVmDealloc); OSAddAtomic64((SInt64)fBytes, &gCntVmDeallocBytes);
    // The family frees a surface's VRAM after copying it back for the CPU, and backs the surface with new VRAM when the
    // GPU needs it again. The Metal plugin imported the old VRAM once and keeps drawing into it, so from then on the
    // display and every copy-back read new, never-written VRAM: black frames, and windows black until a drag made
    // WindowServer import the surface again. Parked instead, this VRAM is what the next allocPhysical hands back, so the
    // surface keeps the memory the plugin draws into. It is released when the resource goes away or outgrows it.
    if (gVmSticky) {
        releaseParked();
        fParkHandle = fRmHandle; fParkKva = fKva; fParkPhys = fPhys; fParkBytes = fBytes; fParkCookie = fAllocationCookie;
        OSAddAtomic64(1, &gCntVmParked);
    } else {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
    }
    fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    *(volatile unsigned long long *)((unsigned char *)this + 0xc8) = 0;
}

void NVVidMemory::releaseVram(void *handle, void *kva, unsigned long long bytes, unsigned long long cookie)
{
    struct NVRMVramRequest r;
    for (unsigned i = 0; i < sizeof r; i++) ((volatile unsigned char *)&r)[i] = 0;
    r.version = NVRM_VRAM_ABI_VERSION;
    r.handle = handle; r.kva = kva; r.actualSize = bytes; r.allocationCookie = cookie;
    if (NVAccel::gAccel) NVAccel::gAccel->askFramebufferForVram(&r, false);
}

void NVVidMemory::releaseParked()
{
    if (!fParkHandle) return;
    releaseVram(fParkHandle, fParkKva, fParkBytes, fParkCookie);
    fParkHandle = nullptr; fParkKva = nullptr; fParkPhys = fParkBytes = fParkCookie = 0;
    OSAddAtomic64(1, &gCntVmParkReleased);
}

void NVVidMemory::free()
{
#ifdef NMX_ACCEL
    if (fIntelXe) nmxSVRelease(this);
#endif
    if (fRmHandle && !fScanout) {
        releaseVram(fRmHandle, fKva, fBytes, fAllocationCookie);
        fRmHandle = nullptr; fKva = nullptr; fPhys = 0; fBytes = 0; fAllocationCookie = 0;
    }
    releaseParked();
    IOAccelVidMemory::free();
}

class IOSurface {
public:
    IOMemoryDescriptor *getMemoryDescriptor() const;
    unsigned long long  getWidth() const;
    unsigned long long  getHeight() const;
    unsigned long long  getBytesPerRow() const;
    unsigned int        getPixelFormat() const;
    unsigned int        getSurfaceID() const;
};
class IOAccelDisplayPipeGammaTable2;
class IOAccelDisplayPipeTransaction2 {
public:
    IOSurface *getPlaneIOSurface(unsigned int, unsigned int) const;
    OSObject  *getPlaneResource(unsigned int, unsigned int) const;
#ifdef NMX_ACCEL
    // IOAcceleratorFamily2 (sys.kc, macOS 15): the transaction's colour state. set_pipe_postgamma_table ORs 0x80 into the
    // dirty bits (+0x48), set_pipe_pregamma_table 0x20, set_pipe_precsclinearization_vid 0x200, set_pipe_postcscgamma_vid
    // 0x400; the tables live in the transaction's state block (+0x38: +0 pre, +8 post, +0x40 + 0x70 * plane).
    unsigned long long getTransactionDirtyBits() const;
    IOAccelDisplayPipeGammaTable2 *getPipePreGammaTable() const;
    IOAccelDisplayPipeGammaTable2 *getPipePostGammaTable() const;
    IOAccelDisplayPipeGammaTable2 *getPlaneGammaTable(unsigned int) const;
#endif
};
#ifdef NMX_ACCEL
// IOAccelDisplayPipeGammaTable2 wraps the IOAccelDisplayPipeGammaTableData IOPresentment sent: { u32 count; u32; float
// min; float max; { float r, g, b, pad } entry[count] } (init / getTableEntry disassembled from sys.kc). CoreDisplay
// fills it in IOPTransaction::SetPipeGamma: entry i is the curve at input i / (count - 1), with count the table length
// our gamma capability advertises (NMXAGDC, 1024). Only the non-virtual accessors are used.
class IOAccelDisplayPipeGammaTable2 {
public:
    unsigned int getTableCount() const;
    void getTableEntry(unsigned int, float *, float *, float *) const;
};
#endif

static int    gNvIop = 0;
static SInt64 gCntIopOk, gCntIopFail, gCntIopEmpty, gCntIopSrcRes, gCntIopSrcSurf, gCntIopBlank;
static SInt64 gCntIopCopyUs;   // microseconds spent in performTransaction's copy into the scanout (/ iop_ok = per frame)
static SInt64 gCntIopOkHead[kNvMaxFB];
#define NVIOP_LOG_MAX 40
static unsigned gIopLogs = 0;
#define NVIOPLOG(fmt, ...) do { if (gIopLogs < NVIOP_LOG_MAX) { gIopLogs++; ALOG("iop: " fmt, ##__VA_ARGS__); } } while (0)
#define NVIOP_FAIL(rc, fmt, ...) do { OSAddAtomic64(1, &gCntIopFail); NVIOPLOG(fmt, ##__VA_ARGS__); return (rc); } while (0)

void NVDisplayPipe::iopDropSources()
{
    for (unsigned i = 0; i < kIopSrcSlots; i++) {
        NVIopSrc *e = &fIopSrc[i];
        if (e->map) { e->map->release(); e->map = nullptr; }
        if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
        e->id = 0;
    }
}

void NVDisplayPipe::free()
{
    iopDropSources();
    IOAccelLegacyDisplayPipe::free();
}

const void *NVDisplayPipe::iopMapSurface(unsigned id, IOMemoryDescriptor *md, unsigned long long need)
{
    for (unsigned i = 0; i < kIopSrcSlots; i++)
        if (fIopSrc[i].md == md && fIopSrc[i].id == id && fIopSrc[i].map)
            return (fIopSrc[i].map->getLength() >= need) ? (const void *)fIopSrc[i].map->getVirtualAddress() : nullptr;
    if (md->getLength() < need) return nullptr;
    NVIopSrc *e = &fIopSrc[fIopNext % kIopSrcSlots]; fIopNext++;
    if (e->map) { e->map->release(); e->map = nullptr; }
    if (e->md)  { e->md->complete(); e->md->release(); e->md = nullptr; }
    e->id = 0;
    if (md->prepare() != kIOReturnSuccess) return nullptr;
    IOMemoryMap *m = md->map(kIOMapReadOnly);
    if (!m) { md->complete(); return nullptr; }
    md->retain(); e->md = md; e->map = m; e->id = id;
    NVIOPLOG("mapped source surface 0x%x (%llu bytes) into slot %u", id, (unsigned long long)m->getLength(), (fIopNext - 1) % kIopSrcSlots);
    return (const void *)m->getVirtualAddress();
}

IOReturn NVDisplayPipe::validateTransaction(IOAccelDisplayPipeTransaction2 *)
{
    return kIOReturnSuccess;
}

static bool nvAccelIopAsyncBusy(const void *pipe);
#ifdef NMX_ACCEL
static bool nmxIopAsyncBusy(const void *pipe);
static bool nmxIopAsyncQueued(const void *pipe);
static int gNmxDepth = 0;   // nmx-tahoe2: debug.nmxaccel_depth, 0/1 = one frame per head (as before), 2 = one more queued
#endif
bool NVDisplayPipe::isTransactionComplete(IOAccelDisplayPipeTransaction2 *)
{
#ifdef NMX_ACCEL
    // stage 2/3: the frame in flight completes from the worker or the flip. nmx-tahoe2: with debug.nmxaccel_depth=2 a
    // second frame may be presented while one is in flight (it waits as the worker's queued frame), so WindowServer
    // composites frame N+1 while frame N waits for its GPU work and its vblank instead of after it.
    if (nmxIopAsyncBusy(this) && !(gNmxDepth >= 2 && !nmxIopAsyncQueued(this))) return false;
#endif
    return gNvIop != 0 && !nvAccelIopAsyncBusy(this);
}

#ifdef NMX_ACCEL
/*
 * Stage 4: CoreDisplay's EOTF / system gamma in the display pipe. NMXAGDC advertises a 1024-entry 3-channel pipe gamma
 * (debug.nmxagdc_gamma); CoreDisplay then sends its final per-channel curve (DisplayPipe::AddEOTF ->
 * IOPTransaction::SetPipeGamma) with the frame instead of rendering it in its own full-screen GPU pass, and it arrives
 * here as the transaction's post-gamma table. It is converted to the 10:10:10 entries of the B580's 10-bit pipe LUT and
 * handed to IntelXeFB ("nmxPipeGamma"), which loads it into PREC_PAL_DATA and enables POST_CSC_GAMMA. CoreDisplay sends a
 * new table whenever its curve changes, including a linear 3-entry one when it applies the curve on the GPU again.
 * Nothing else is advertised, so linearization and CSC never arrive; if they ever do they are counted, not applied.
 */
static int    gNmxGamma = 1;                 // debug.nmxaccel_gamma: 0 = ignore tables and restore the firmware gamma
static int    gNmxGammaRestore = 0;          // set when the sysctl goes 0: the next transaction restores each head
static SInt64 gCntGammaLoads, gCntGammaFail, gCntColorUnhandled;
static unsigned gGammaLogs = 0;
static const OSSymbol *gSymPipeGamma = nullptr;
enum { kNmxGammaSize = 1024 };
struct NMXPipeGamma { unsigned int version, count, lut[kNmxGammaSize]; };   // IntelXeFB.cpp, same layout
// IEEE-754 single (as bits) -> round(clamp(v, 0, 1) * 65535) with integer arithmetic only (no FPU use in the kernel).
static inline unsigned nmxF32ToU16(uint32_t b)
{
    const uint32_t e = (b >> 23) & 0xFF, m = b & 0x7FFFFF;
    if (b >> 31) return 0;                          // negative (and -0)
    if (e == 0xFF) return m ? 0 : 65535;            // NaN -> 0, +inf -> 1
    if (e >= 127) return 65535;                     // >= 1.0
    if (e == 0) return 0;                           // denormal
    const unsigned shift = 150 - e;                 // v = (1.m) * 2^(e - 127) = (0x800000|m) >> (150 - e)
    if (shift >= 63) return 0;
    const unsigned long long num = (unsigned long long)(0x800000u | m) * 65535ull;
    const unsigned long long r = (num + (1ull << (shift - 1))) >> shift;
    return r > 65535 ? 65535 : (unsigned)r;
}
static void nmxGammaEntry(const IOAccelDisplayPipeGammaTable2 *t, unsigned i, unsigned rgb[3])
{
    uint32_t bits[3] = { 0, 0, 0 };
    t->getTableEntry(i, (float *)&bits[0], (float *)&bits[1], (float *)&bits[2]);   // the family copies the bits (movss)
    for (int c = 0; c < 3; c++) rgb[c] = nmxF32ToU16(bits[c]);
}
static void nmxGammaSend(IOService *fb, NMXPipeGamma *g)
{
    if (!gSymPipeGamma) gSymPipeGamma = OSSymbol::withCStringNoCopy("nmxPipeGamma");
    IOReturn r = gSymPipeGamma ? fb->callPlatformFunction(gSymPipeGamma, false, g, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (r == kIOReturnSuccess) { if (g->count) OSAddAtomic64(1, &gCntGammaLoads); return; }
    OSAddAtomic64(1, &gCntGammaFail);
    if (gGammaLogs < 20) { gGammaLogs++; ALOG("iop: gamma: framebuffer refused the pipe gamma table (0x%x, count %u)", (unsigned)r, g->count); }
}
static void nmxApplyPipeColor(IOAccelDisplayPipeTransaction2 *txn, IOService *fb)
{
    const unsigned long long dirty = txn->getTransactionDirtyBits();
    if ((dirty & (0x20 | 0x200 | 0x400)) || txn->getPlaneGammaTable(0)) {
        OSAddAtomic64(1, &gCntColorUnhandled);
        if (gGammaLogs < 20) { gGammaLogs++; ALOG("iop: gamma: transaction carries colour state we do not apply (dirty 0x%llx, plane gamma %p)", dirty, txn->getPlaneGammaTable(0)); }
    }
    if (!gNmxGamma) {
        if (gNmxGammaRestore) {
            gNmxGammaRestore = 0;
            // count 0 only reads the header: a whole NMXPipeGamma on the stack (4 KB) pulled in ___chkstk_darwin,
            // a weak reference kmutil refuses in a kext
            struct { unsigned int version, count; } off = { 1, 0 };
            nmxGammaSend(fb, (NMXPipeGamma *)&off);
        }
        return;
    }
    if (!(dirty & 0x80)) return;
    const IOAccelDisplayPipeGammaTable2 *t = txn->getPipePostGammaTable();
    if (!t) return;
    const unsigned n = t->getTableCount();
    if (n < 2 || n > 0x20000) {
        OSAddAtomic64(1, &gCntGammaFail);
        if (gGammaLogs < 20) { gGammaLogs++; ALOG("iop: gamma: post-gamma table with %u entries ignored", n); }
        return;
    }
    NMXPipeGamma *g = (NMXPipeGamma *)IOMalloc(sizeof *g);
    if (!g) { OSAddAtomic64(1, &gCntGammaFail); return; }
    g->version = 1; g->count = kNmxGammaSize;
    // Linear interpolation from the n entries CoreDisplay sampled at i / (n - 1) to the 1024 of the hardware LUT
    // (n is 1024 when CoreDisplay used our advertised size; 3 for its linear "bypass" ramp).
    unsigned a[3], b[3]; unsigned cur = ~0u;
    for (unsigned i = 0; i < kNmxGammaSize; i++) {
        const unsigned long long pos = (unsigned long long)i * (n - 1);
        const unsigned idx = (unsigned)(pos / (kNmxGammaSize - 1)), frac = (unsigned)(pos % (kNmxGammaSize - 1));
        if (idx != cur) { nmxGammaEntry(t, idx, a); if (idx + 1 < n) nmxGammaEntry(t, idx + 1, b); else { b[0] = a[0]; b[1] = a[1]; b[2] = a[2]; } cur = idx; }
        unsigned v10[3];
        for (int c = 0; c < 3; c++) {
            const long long v = (long long)a[c] + (((long long)b[c] - (long long)a[c]) * (long long)frac) / (long long)(kNmxGammaSize - 1);
            const unsigned u = v < 0 ? 0u : (v > 65535 ? 65535u : (unsigned)v);
            v10[c] = (u * 1023u + 32767u) / 65535u;
        }
        g->lut[i] = v10[0] << 20 | v10[1] << 10 | v10[2];
    }
    if (gGammaLogs < 20) {
        gGammaLogs++;
        ALOG("iop: gamma: post-gamma table of %u entries -> pipe LUT: [0] %03x/%03x/%03x [512] %03x/%03x/%03x [1023] %03x/%03x/%03x",
             n, g->lut[0] >> 20, (g->lut[0] >> 10) & 0x3ff, g->lut[0] & 0x3ff, g->lut[512] >> 20, (g->lut[512] >> 10) & 0x3ff,
             g->lut[512] & 0x3ff, g->lut[1023] >> 20, (g->lut[1023] >> 10) & 0x3ff, g->lut[1023] & 0x3ff);
    }
    nmxGammaSend(fb, g);
    IOFree(g, sizeof *g);
}
static int nmxaccel_gamma_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNmxGamma;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    if (gNmxGamma && !v) gNmxGammaRestore = 1;
    gNmxGamma = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_gamma, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nmxaccel_gamma_sysctl, "I",
            "stage 4: 1 = load the pipe gamma tables CoreDisplay sends into IntelXeFB's LUT (default), 0 = ignore them and restore the firmware gamma");
SYSCTL_QUAD(_debug, OID_AUTO, nmxaccel_gamma_loads, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntGammaLoads, "pipe gamma tables loaded into the LUT");
SYSCTL_QUAD(_debug, OID_AUTO, nmxaccel_gamma_fail, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntGammaFail, "pipe gamma tables not loaded (refused, bad size, no memory)");
SYSCTL_QUAD(_debug, OID_AUTO, nmxaccel_color_unhandled, CTLFLAG_RD | CTLFLAG_LOCKED, &gCntColorUnhandled, "transactions with pre-gamma, linearization, post-CSC gamma or plane gamma state (not applied)");
#endif
static void nvAccelSurfScanoutSync(IOSurface *s);
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head);
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe);
static bool nvSurfWaitReady(unsigned int id);
static void nvAccelIopHome(IOService *fb, unsigned head);
#ifdef NMX_ACCEL
// Frame path stages 2 and 3 (B580): the frame is presented on a thread call after WindowServer's composite has finished
// on the GPU, by a flip of the plane when the frame has a scan-out buffer, else by the copy below. Defined at the end.
static bool nmxIopAsyncOn();
static bool nmxIopAsyncStart(NVDisplayPipe *pipe, IOService *fb, unsigned head, IOSurface *s);
static bool nmxIopAsyncBusy(const void *pipe);
static void nmxNotePresented(unsigned sid);
static void nmxCopyLock(bool lock);
static void nmxHomeAfterCopy(IOService *fb, unsigned head);
#endif
static SInt32 gIopK6Logs;
// The CPU copy of one frame into the framebuffer's scan-out (IntelXeFB: its write-combined BAR2 mapping of the firmware's
// surface). src is cached memory (the IOSurface's pages or our resource's kva); the scan-out is never read.
static IOReturn nvIopCopyOut(NVAccel *a, IOService *fb, unsigned head, const volatile unsigned int *src, unsigned w, unsigned h,
                             unsigned pitch, bool fromRes)
{
    if (!w || !h || pitch < 4) NVIOP_FAIL(kIOReturnBadArgument, "source geometry %ux%u pitch %u", w, h, pitch);

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q, fb);
    if (ret != kIOReturnSuccess || !q.kva || q.pitch < 4 || !q.height) NVIOP_FAIL(kIOReturnNotReady, "framebuffer would not describe its scanout (0x%x kva %p pitch %u)", (unsigned)ret, q.kva, q.pitch);
#ifdef NMX_ACCEL
    if (head < kNvMaxFB) {   // debug.nmxaccel_heads: what each head was last given (a scaled mode shows as a mismatch)
        gNmxHead[head].srcW = w; gNmxHead[head].srcH = h; gNmxHead[head].scW = q.width; gNmxHead[head].scH = q.height;
        if (w != q.width || h != q.height) OSAddAtomic64(1, &gNmxHead[head].mismatch);
    }
#endif
    if (w != q.width || h != q.height) NVIOP_FAIL(kIOReturnBadArgument, "source %ux%u is not the scanout's %ux%u", w, h, q.width, q.height);

    nvAccelIopHome(fb, head);
    // Row by row with memcpy: the common bytes of each row (the narrower pitch, whole words) of the h rows both have. The
    // scanout is IntelXeFB's write-combined BAR2 mapping, where 32-bit volatile stores, one load and one store per word,
    // were the slow part of the frame; XNU's memcpy (rep movsq/movsb, osfmk/x86_64/bcopy.s) streams whole lines into the
    // WC buffers. It only stores to the destination: a CPU READ of BAR2 runs at a few MB/s, so nothing here may read it.
    // Plain stores, not movnti/SSE: WC memory already combines them, and SSE in the kernel would need the FPU state saved.
    // The source is the IOSurface's system memory (iopMapSurface) or our resource's kva, both cached.
    const uint64_t c0 = mach_absolute_time();
    const unsigned rowBytes = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned char *sb = (const unsigned char *)src;
    unsigned char *db = (unsigned char *)q.kva;
    if (pitch == q.pitch && rowBytes == pitch) memcpy(db, sb, (size_t)h * pitch);   // one stream: same layout both sides
    else for (unsigned y = 0; y < h; y++) memcpy(db + (size_t)y * q.pitch, sb + (size_t)y * pitch, rowBytes);
    { uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - c0, &ns); OSAddAtomic64((SInt64)(ns / 1000), &gCntIopCopyUs); }
    const unsigned spr = pitch / 4;   // the samples below read the source, never the scanout
    OSAddAtomic64(1, fromRes ? &gCntIopSrcRes : &gCntIopSrcSurf);
    const SInt64 n = OSAddAtomic64(1, &gCntIopOk);
    const SInt64 nh = OSAddAtomic64(1, &gCntIopOkHead[head]);
    unsigned nz = 0, varied = 0; const unsigned first = src[0] & 0xffffffu;
    for (unsigned i = 0; i < 8; i++) for (unsigned j = 0; j < 8; j++) {
        unsigned sx = (w * (2 * j + 1)) / 16; if (sx >= spr) sx = spr - 1;
        const unsigned v = src[(unsigned long)((h * (2 * i + 1)) / 16) * spr + sx] & 0xffffffu;
        if (v) nz++;
        if (v != first) varied++;
    }
    if (!varied) OSAddAtomic64(1, &gCntIopBlank);
    if ((n <= 3 || n == 600 || nh == 0) && gIopK6Logs < 12) {
        OSIncrementAtomic(&gIopK6Logs);
        ALOG("iop: K6 transaction #%lld (head %u #%lld) on the panel: %ux%u pitch %u from %s — 64 samples: nonzero %u varied %u", (long long)n, head, (long long)nh, w, h, pitch,
             fromRes ? "our VRAM resource" : "the IOSurface's memory", nz, varied);
    }
    return kIOReturnSuccess;
}
// A frame from an IOSurface: its system memory is mapped (cached per pipe) and copied out. Called on WindowServer's
// thread, or on the stage 2 worker once the composite has finished.
static IOReturn nvIopCopySurface(NVDisplayPipe *dp, NVAccel *a, IOService *fb, unsigned head, IOSurface *s)
{
    // A copied frame must come from the surface's newest pixels. Copying while they were still only in VRAM put a
    // stale, one-colour buffer on the panel every other frame: full-screen flicker, 5 of 40 flicker recordings
    // (1.13-1.14.1), nvaccel_iop_blank about half of every copied frame, never on machines whose frames all flip.
    // Such a frame now keeps the previous picture on the panel instead.
    nvSurfWaitReady(s->getSurfaceID());   // a copied frame is read here, so its GPU work must be done first
    if (!nvAccelSurfCopyReady(s, head)) return kIOReturnSuccess;
    const unsigned fmt = s->getPixelFormat();
    if (fmt != 0x42475241u) NVIOP_FAIL(kIOReturnUnsupported, "surface 0x%x format 0x%08x is not BGRA", s->getSurfaceID(), fmt);
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 || W > 16384 || H > 16384 || P > 16384ull * 8) NVIOP_FAIL(kIOReturnBadArgument, "surface geometry %llux%llu pitch %llu", W, H, P);
    if (P < 4 * W || (P & 3)) NVIOP_FAIL(kIOReturnBadArgument, "surface %llux%llu: row pitch %llu is under 4*W or not word-aligned", W, H, P);
    IOMemoryDescriptor *md = s->getMemoryDescriptor();
    if (!md) NVIOP_FAIL(kIOReturnNoMemory, "surface 0x%x has no memory descriptor", s->getSurfaceID());
    const volatile unsigned int *src = (const volatile unsigned int *)dp->iopMapSurface(s->getSurfaceID(), md, H * P);
    if (!src) NVIOP_FAIL(kIOReturnVMError, "surface 0x%x would not prepare/map %llu bytes", s->getSurfaceID(), H * P);
    return nvIopCopyOut(a, fb, head, src, (unsigned)W, (unsigned)H, (unsigned)P, false);
}
#ifdef NMX_ACCEL
// macOS Adaptive-Sync. With a "Variable" refresh mode set (IntelXeFB v4: IOFBTimingRange sync flag 0x10, timings with a
// verticalBlankingExtension), CoreDisplay commits per-frame blanking extensions (IOPTransaction::
// CommitBlankingExtensions). Apple's Intel framebuffer (AppleIntelFramebuffer::Validate_Populate_OS_Req_Min_Max,
// macOS 15) reads them when transaction dirty bit 0x10000 is set: the u32 at +0x128 is the shortest frame and the u32
// at +0x130 the longest, each in lines added to the panel's minimum frame; without the bit the last values stay.
// They are kept per head here and travel with every flip (NMXScanout version 2), as that driver keeps them.
struct NMXVrrReq { volatile UInt32 flags, minExt, maxExt; };
static NMXVrrReq gNmxVrr[kNvMaxFB];
static SInt64 gCntNmxVrrTx, gCntNmxVrrBad;
static void nmxNoteVrr(IOAccelDisplayPipeTransaction2 *txn, unsigned head)
{
    if (head >= kNvMaxFB || !(txn->getTransactionDirtyBits() & 0x10000ull)) return;
    const unsigned char *t = (const unsigned char *)txn;
    const UInt32 mn = *(const volatile UInt32 *)(t + 0x128), mx = *(const volatile UInt32 *)(t + 0x130);
    const SInt64 n = OSAddAtomic64(1, &gCntNmxVrrTx);
    if (n < 8 || n == 1000) ALOG("iop: adaptive sync: transaction #%lld head %u asks frame length vmin+%u .. vmin+%u lines", (long long)n + 1, head, mn, mx);
    if (mn > 0xFFFFF || mx > 0xFFFFF || mn > mx) { OSAddAtomic64(1, &gCntNmxVrrBad); return; }
    gNmxVrr[head].minExt = mn; gNmxVrr[head].maxExt = mx; gNmxVrr[head].flags = 1;
}
#endif
IOReturn NVDisplayPipe::performTransaction(IOAccelDisplayPipeTransaction2 *txn)
{
    if (!gNvIop) return (IOReturn)0xe00002c7;
    if (!txn) NVIOP_FAIL(kIOReturnBadArgument, "performTransaction(NULL)");
    NVAccel *a = NVAccel::gAccel;
    unsigned head = 0;
    IOService *fb = a ? a->pipeFramebuffer(this, &head) : nullptr;
    if (!fb) NVIOP_FAIL(kIOReturnNotAttached, "no accelerator, or pipe %p drives a framebuffer that is none of ours", this);

#ifdef NMX_ACCEL
    // before the frame: a gamma-only transaction has no plane. A head without a pipe LUT (knob on) is never sent one.
    if (!nmxHeadSkips(head, kNmxCapGamma)) nmxApplyPipeColor(txn, fb);
    nmxNoteVrr(txn, head);
#endif
    IOSurface *s  = txn->getPlaneIOSurface(0, 0);
    OSObject  *ro = txn->getPlaneResource(0, 0);
    if (!s && !ro) { OSAddAtomic64(1, &gCntIopEmpty); return kIOReturnSuccess; }

    NVResource *res = OSDynamicCast(NVResource, ro);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (gIopK6Logs < 6) {
        OSIncrementAtomic(&gIopK6Logs);
        const unsigned char *kr = (const unsigned char *)res;
        ALOG("iop: K6 plane 0: surface %s id 0x%x %ux%u row %u fmt 0x%08x | resource %s w %u h %u +0xb8 %u +0xc0 %u vidmem %llu bytes kva %d scanout %d",
             s ? "yes" : "NO", s ? (unsigned)s->getSurfaceID() : 0u, s ? (unsigned)s->getWidth() : 0u, s ? (unsigned)s->getHeight() : 0u,
             s ? (unsigned)s->getBytesPerRow() : 0u, s ? (unsigned)s->getPixelFormat() : 0u,
             ro ? ro->getMetaClass()->getClassName() : "(none)",
             res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb0) : 0u, res ? (unsigned)*(const volatile unsigned short *)(kr + 0xb2) : 0u,
             res ? *(const volatile unsigned int *)(kr + 0xb8) : 0u, res ? *(const volatile unsigned int *)(kr + 0xc0) : 0u,
             vm ? (unsigned long long)vm->fBytes : 0ull, (vm && vm->fKva) ? 1 : 0, vm ? (int)vm->fScanout : -1);
    }
    if (!s && vm && vm->fKva) {
        const unsigned char *rr = (const unsigned char *)res;
        const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
        const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
        const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
        if ((unsigned long long)pitch < 4ull * w) NVIOP_FAIL(kIOReturnBadArgument, "resource %ux%u: +0xb8 reads %u, which is not a row pitch (need >= %u) — not a surface buffer", w, h, pitch, 4u * w);
        if ((unsigned long long)h * pitch > vm->fBytes) NVIOP_FAIL(kIOReturnOverrun, "resource %ux%u pitch %u exceeds its %llu bytes", w, h, pitch, (unsigned long long)vm->fBytes);
#ifdef NMX_ACCEL
        nmxCopyLock(true);
        IOReturn r = nvIopCopyOut(a, fb, head, (const volatile unsigned int *)vm->fKva, w, h, pitch, true);
        nmxCopyLock(false);
        nmxHomeAfterCopy(fb, head);
        return r;
#else
        return nvIopCopyOut(a, fb, head, (const volatile unsigned int *)vm->fKva, w, h, pitch, true);
#endif
    }
    if (s && nvAccelIopFlip(fb, head, s, this)) {
        OSAddAtomic64(1, &gCntIopSrcSurf); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[head]);
        return kIOReturnSuccess;
    }
    if (!s) NVIOP_FAIL(kIOReturnUnsupported, "plane resource %p is not an NVResource with a CPU mapping, and there is no IOSurface", ro);
#ifdef NMX_ACCEL
    nmxNotePresented(s->getSurfaceID());
    // Stage 2: WindowServer committed this composite without waiting for the GPU (the plugin marked it pending); the
    // worker waits for it, presents it and completes the transaction (isTransactionComplete stays false until then).
    if (s->getPixelFormat() == 0x42475241u && nmxIopAsyncOn() && nmxIopAsyncStart(this, fb, head, s)) return kIOReturnSuccess;
    nmxCopyLock(true);
    IOReturn r = nvIopCopySurface(this, a, fb, head, s);
    nmxCopyLock(false);
    nmxHomeAfterCopy(fb, head);
    return r;
#else
    return nvIopCopySurface(this, a, fb, head, s);
#endif
}

static int nvaccelSetIop(int v);
static int nvacceliop_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvIop;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    return nvaccelSetIop(v);
}
static int nvaccelSetIop(int v)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return ENXIO;
    if (v) {
        OSDictionary *caps = OSDictionary::withCapacity(2);
        if (!caps) return ENOMEM;
        caps->setObject("DisplayPipeSupported", kOSBooleanTrue);
        caps->setObject("TransactionsSupported", kOSBooleanTrue);
#ifdef NMX_ACCEL
        // The pipeline description CoreDisplay reads back through IOAccelDisplayPipe::copyCapabilities (which returns this
        // very property) and IOPresentment parses (__devicesFromDictionary / __planesFromDictionary). With only the two
        // flags above WindowServer logged "Attempting to get capabilities from capabilities with no devices" and never
        // sent a transaction (B580, 2026-10-28). One device, one plane: every pixel format bit (performTransaction only
        // copies BGRA and counts the rest as failures), planes up to 8192x8192, two queued transactions.
        auto num = [](unsigned long long v) { return OSNumber::withNumber(v, 64); };
        OSDictionary *plane = OSDictionary::withCapacity(5), *queue = OSDictionary::withCapacity(2),
                     *dev = OSDictionary::withCapacity(2);
        OSArray *planes = OSArray::withCapacity(1), *devs = OSArray::withCapacity(1);
        if (plane && queue && dev && planes && devs) {
            const struct { const char *k; unsigned long long v; } pk[] = {
                { "Flags", 0 }, { "BlendType", 0 }, { "Width", 8192 }, { "Height", 8192 }, { "PixelFormats", 0xFFFFFFFFull } };
            for (auto &e : pk) if (OSNumber *n = num(e.v)) { plane->setObject(e.k, n); n->release(); }
            if (OSNumber *n = num(2)) { queue->setObject("QueueDepth", n); n->release(); }
            if (OSNumber *n = num(0)) { queue->setObject("Flags", n); n->release(); }
            planes->setObject(plane);
            dev->setObject("PlaneArray", planes);
            dev->setObject("QueueDictionary", queue);
            devs->setObject(dev);
            caps->setObject("DeviceArray", devs);
            if (OSNumber *n = num(1)) { caps->setObject("PipelineDictionaryVersion", n); n->release(); }
        }
        OSSafeReleaseNULL(plane); OSSafeReleaseNULL(queue); OSSafeReleaseNULL(dev); OSSafeReleaseNULL(planes); OSSafeReleaseNULL(devs);
#endif
        a->setProperty("IOAccelDisplayPipeCapabilities", caps);
        caps->release();
        gNvIop = 1;
        ALOG("iop: debug.nvaccel_iop=1 -- capabilities published, transaction hooks OPEN");
    } else {
        gNvIop = 0;
        a->removeProperty("IOAccelDisplayPipeCapabilities");
        ALOG("iop: debug.nvaccel_iop=0 -- capabilities removed, transaction hooks answer as the base class");
    }
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I",
            "K3: 1 = publish IOAccelDisplayPipeCapabilities and perform display-pipe transactions, 0 = behave as the base class (default)");

static int nvaccelfb_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNvAccelArmed;
#ifdef NMX_ACCEL
    if (!v && gNmxAuto && (gNmxAutoState == 1 || gNmxAutoState == 2)) v = 1;   // armed at boot, about to be (the plugin's gate)
#endif
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || req->newptr == 0) return err;
    NVAccel::triggerFromSysctl(v);
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I",
            "register the NVIDIA framebuffer with the accelerator display machine (1 = found_framebuffer, 2 = family sweep)");
#define NVACCEL_CNT(nm, var, desc) SYSCTL_QUAD(_debug, OID_AUTO, nm, CTLFLAG_RD | CTLFLAG_LOCKED, (SInt64 *)&var, desc)
NVACCEL_CNT(nvaccel_vm_alloc,         gCntVmAlloc,        "NVVidMemory::allocPhysical successes");
NVACCEL_CNT(nvaccel_vm_alloc_bytes,   gCntVmAllocBytes,   "bytes of VRAM granted to NVVidMemory");
NVACCEL_CNT(nvaccel_vm_dealloc,       gCntVmDealloc,      "NVVidMemory::deallocPhysical calls with a handle");
NVACCEL_CNT(nvaccel_vm_dealloc_bytes, gCntVmDeallocBytes, "bytes of VRAM given back by NVVidMemory");
NVACCEL_CNT(nvaccel_vm_refused,       gCntVmRefused,      "allocPhysical refused by the framebuffer");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_vm_sticky, CTLFLAG_RW | CTLFLAG_LOCKED, &gVmSticky, 0, "1 = a surface keeps its VRAM across the family's dealloc/alloc (the memory the plugin draws into)");
NVACCEL_CNT(nvaccel_vm_parked,        gCntVmParked,       "VRAM kept when the family freed a surface's VRAM");
NVACCEL_CNT(nvaccel_vm_unparked,      gCntVmUnparked,     "allocations answered with the surface's kept VRAM");
NVACCEL_CNT(nvaccel_vm_park_released, gCntVmParkReleased, "kept VRAM released (resource freed or grown)");
NVACCEL_CNT(nvaccel_res_new,          gCntResNew,         "NVResource objects created");
NVACCEL_CNT(nvaccel_res_free,         gCntResFree,        "NVResource objects freed");
NVACCEL_CNT(nvaccel_map_commit,       gCntMapCommit,      "GPU page-table commits (MAP_MEMORY_DMA ok)");
NVACCEL_CNT(nvaccel_map_release,      gCntMapRelease,     "GPU page-table releases (UNMAP_ONLY)");
NVACCEL_CNT(nvaccel_map_free,         gCntMapFree,        "GPU VA ranges freed (nvAccelGpuVaFree)");
NVACCEL_CNT(nvaccel_iop_ok,           gCntIopOk,          "K3: display-pipe transactions copied to the panel");
NVACCEL_CNT(nvaccel_iop_ok_h0,        gCntIopOkHead[0],   "frames copied to head 0's panel");
NVACCEL_CNT(nvaccel_iop_ok_h1,        gCntIopOkHead[1],   "frames copied to head 1's panel");
NVACCEL_CNT(nvaccel_iop_ok_h2,        gCntIopOkHead[2],   "frames copied to head 2's panel");
NVACCEL_CNT(nvaccel_iop_ok_h3,        gCntIopOkHead[3],   "frames copied to head 3's panel");
NVACCEL_CNT(nvaccel_iop_ok_h4,        gCntIopOkHead[4],   "frames copied to head 4 (an Intel built-in panel, NMIntelFB)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_published, CTLFLAG_RD | CTLFLAG_LOCKED | CTLFLAG_KERN, &gNvHeadsPublished, 0,
            "bit i = head i's framebuffer has published to the accelerator");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_registered, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsRegistered, 0,
            "bit i = head i's framebuffer was handed to the display machine (found_framebuffer)");
SYSCTL_UINT(_debug, OID_AUTO, nvaccel_heads_piped, CTLFLAG_RD | CTLFLAG_LOCKED, &gNvHeadsPiped, 0,
            "bit i = head i has a display pipe that drives ITS OWN framebuffer (pipe+0x98 read back)");
static_assert(kNvMaxFB == 5, "kNvMaxFB changed: add debug.nvaccel_iop_ok_h<i> for every new head");
NVACCEL_CNT(nvaccel_iop_fail,         gCntIopFail,        "K3: transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nvaccel_iop_empty,        gCntIopEmpty,       "K3: transactions with no plane (gamma/options only)");
NVACCEL_CNT(nvaccel_iop_src_res,      gCntIopSrcRes,      "K3: frames sourced from our VRAM resource");
NVACCEL_CNT(nvaccel_iop_src_surf,     gCntIopSrcSurf,     "K3: frames sourced from the IOSurface memory");
NVACCEL_CNT(nvaccel_iop_blank,        gCntIopBlank,       "K6: frames copied whose 64 samples were all ONE colour (a blank frame)");
static SInt64 gCntIopCopySkip;
NVACCEL_CNT(nvaccel_iop_copy_skip,    gCntIopCopySkip,    "frames kept on the panel because the surface's newest pixels were not yet in system memory");
static int    gIopFlip = 0;
static SInt64 gCntIopFlip, gCntIopFlipHome, gCntIopFlipNo[8];
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_flip, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gIopFlip, 0, "1 = scan the composited IOSurface out of its own VRAM (no copy)");
NVACCEL_CNT(nvaccel_iop_flips,        gCntIopFlip,        "frames scanned out of the surface's own VRAM (zero copy)");
NVACCEL_CNT(nvaccel_iop_flip_home,    gCntIopFlipHome,    "flips back to the boot scanout before a copied frame");
NVACCEL_CNT(nvaccel_iop_flip_lock,    gCntIopFlipNo[3],   "copied because the accelerator lock was busy");
NVACCEL_CNT(nvaccel_iop_flip_novram,  gCntIopFlipNo[4],   "copied because the surface has no RM VRAM");
NVACCEL_CNT(nvaccel_iop_flip_stale,   gCntIopFlipNo[5],   "copied because system memory holds the newest copy");
NVACCEL_CNT(nvaccel_iop_flip_refused, gCntIopFlipNo[7],   "copied because the framebuffer refused the flip");
static SInt64 gCntIopUnwritten, gCntIopCopyUnwritten;
NVACCEL_CNT(nvaccel_iop_flip_unwritten, gCntIopUnwritten,     "copied because the family marked VRAM newest on a copy the plugin never wrote");
NVACCEL_CNT(nvaccel_iop_copy_unwritten, gCntIopCopyUnwritten, "copied frames that skipped the page-off because the plugin never wrote the VRAM copy");
// The two decisions the display pipe makes from the family's validity bits, kept as pure functions so they can be
// exercised outside the kernel (tools/test_iop_flip_gate.py). vramNewest is the family's bit (valid[6] & plane);
// sysmemCurrent is valid[0] & plane; pluginWrote is nvAccelPluginWrote(): the resource's recorded write cookie equals the
// allocation cookie of the VRAM it holds now.
// A zero-copy flip scans VRAM out directly, so it needs a VRAM copy that holds a frame: the family's bit alone is not
// enough, the plugin must have written it (its dirty escape sets both).
static inline bool nvAccelIopVramFlippable(bool vramNewest, bool pluginWrote) { return vramNewest && pluginWrote; }
// A copied frame is read from system memory. When the family says that copy is stale, the resource is paged off VRAM
// first; a VRAM copy the plugin never wrote holds nothing worth paging, and paging it would overwrite the pixels the
// plugin copied out. Such a surface is read as it is.
static inline bool nvAccelIopCopyNeedsPageoff(bool sysmemCurrent, bool pluginWrote) { return !sysmemCurrent && pluginWrote; }
static inline bool nvAccelPluginWrote(unsigned long long wroteCookie, unsigned long long allocCookie) { return wroteCookie != 0 && wroteCookie == allocCookie; }
static bool nvAccelResPluginWrote(NVResource *res) { NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr; return vm && nvAccelPluginWrote(res->fPluginWroteCookie, vm->fAllocationCookie); }
static SInt64 gCntIopFlipHit, gCntIopFlipMiss;
NVACCEL_CNT(nvaccel_iop_flip_hit,     gCntIopFlipHit,     "swaps resolved from the cache (no accelerator lock)");
NVACCEL_CNT(nvaccel_iop_flip_miss,    gCntIopFlipMiss,    "swaps that resolved under the accelerator lock");
static int    gIopAsync = 1;

/*
 * Frame completion off WindowServer's thread. NVIDIA's display path never makes the compositor wait for the GPU: a flip
 * completes on the display engine's flip event, and a flip ordered behind a fence waits for it in a worker
 * (nvidia-drm-modeset.c:636/692). Here WindowServer waited for every composite to finish on the GPU before swapping
 * (issue #196: ~1.3 ms of each 6.9 ms frame at 144 Hz, ~95-105 presents/s). Now the plugin marks the surfaces a
 * WindowServer command buffer renders as pending when it commits and ready when the GPU has finished them, and the swap
 * waits for that here: on the async flip thread for a zero-copy flip, before reading for a copied frame. A mark answers
 * 0 while this is on; the plugin keeps WindowServer's own wait when it answers anything else.
 */
#ifdef NMX_ACCEL
static int gSurfGpuOn = 0;   // B580: debug.nmxaccel_async (frame path stage 2), off until switched on
#else
static int gSurfGpuOn = 1;
#endif
static SInt64 gCntSurfMarks, gCntSurfWaits, gCntSurfTimeouts;
static int gSurfWaitMaxUs;
static const unsigned kSurfGpuWaitMs = 100;   // a GPU that has not finished a composite in 100 ms is hung; flip anyway
struct NVSurfPending { unsigned int id; unsigned int seq; };
static NVSurfPending gSurfPend[32];
static unsigned int gSurfPendSeq;
static IOLock *gSurfPendLock;
static IOLock *nvSurfLock()
{
    if (!gSurfPendLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gSurfPendLock)) IOLockFree(l); }
    return gSurfPendLock;
}
static void nvSurfMark(unsigned int id, bool pending)
{
    IOLock *l = nvSurfLock();
    if (!l || !id) return;
    IOLockLock(l);
    unsigned slot = 32, oldest = 0;
    for (unsigned i = 0; i < 32; i++) {
        if (gSurfPend[i].id == id) { slot = i; break; }
        if (slot == 32 && !gSurfPend[i].id) slot = i;
        if (gSurfPend[i].seq < gSurfPend[oldest].seq) oldest = i;
    }
    if (pending) {
        if (slot == 32) slot = oldest;   // 32 surfaces in flight at once is not a compositor; the oldest mark goes
        gSurfPend[slot].id = id; gSurfPend[slot].seq = ++gSurfPendSeq;
    } else if (slot < 32 && gSurfPend[slot].id == id) {
        gSurfPend[slot].id = 0;
        IOLockWakeup(l, gSurfPend, false);
    }
    IOLockUnlock(l);
    OSAddAtomic64(1, &gCntSurfMarks);
}
// Whether the surface's GPU work is still marked pending now.
static bool nvSurfPendingNow(unsigned int id)
{
    IOLock *l = gSurfPendLock;
    if (!l || !id || !gSurfGpuOn) return false;
    IOLockLock(l);
    unsigned i = 0;
    while (i < 32 && gSurfPend[i].id != id) i++;
    IOLockUnlock(l);
    return i < 32;
}
// As nvSurfWaitReady; when the surface is still pending at due (mach absolute time; 0 = never), onDue(ctx) runs once,
// outside the lock, and the wait goes on (dispboost: a frame still on the GPU at the vblank it was due at).
static bool nvSurfWaitReadyDue(unsigned int id, uint64_t due, void (*onDue)(void *), void *ctx)
{
    IOLock *l = gSurfPendLock;
    if (!l || !id || !gSurfGpuOn) return true;   // switched off: marks left from before never hold a swap
    uint64_t start = mach_absolute_time(), deadline = 0;
    clock_interval_to_deadline(kSurfGpuWaitMs, kMillisecondScale, &deadline);
    bool waited = false, ready = true;
    IOLockLock(l);
    for (;;) {
        unsigned i = 0;
        while (i < 32 && gSurfPend[i].id != id) i++;
        if (i == 32) break;
        const uint64_t now = mach_absolute_time();
        if (now >= deadline) { gSurfPend[i].id = 0; ready = false; break; }
        if (due && now >= due) {
            due = 0;
            if (onDue) { IOLockUnlock(l); onDue(ctx); IOLockLock(l); }
            continue;
        }
        waited = true;
        IOLockSleepDeadline(l, gSurfPend, due && due < deadline ? due : deadline, THREAD_UNINT);
    }
    IOLockUnlock(l);
    if (waited) {
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - start, &ns);
        if ((int)(ns / 1000) > gSurfWaitMaxUs) gSurfWaitMaxUs = (int)(ns / 1000);
        OSAddAtomic64(1, ready ? &gCntSurfWaits : &gCntSurfTimeouts);
    }
    return ready;
}
static bool nvSurfWaitReady(unsigned int id) { return nvSurfWaitReadyDue(id, 0, nullptr, nullptr); }
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "1 = WindowServer swaps wait for GPU completion here (same value as nvaccel_surface_gpu_on)");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_on, CTLFLAG_RW | CTLFLAG_LOCKED, &gSurfGpuOn, 0, "0 = WindowServer waits for its own composites again (the plugin re-asks every second)");
NVACCEL_CNT(nvaccel_surface_gpu_marks,    gCntSurfMarks,    "pending/ready marks from WindowServer");
NVACCEL_CNT(nvaccel_surface_gpu_waits,    gCntSurfWaits,    "swaps that waited here for their composite to finish");
NVACCEL_CNT(nvaccel_surface_gpu_timeouts, gCntSurfTimeouts, "swaps whose composite did not finish within 100 ms");
SYSCTL_INT(_debug, OID_AUTO, nvaccel_surface_gpu_wait_max_us, CTLFLAG_RD | CTLFLAG_LOCKED, &gSurfWaitMaxUs, 0, "longest swap wait for a composite, us");
static SInt64 gCntIopAsync, gCntIopAsyncRefused, gCntIopAsyncDrop;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_iop_async, CTLFLAG_RW | CTLFLAG_LOCKED, &gIopAsync, 0, "1 = the swap runs on a thread call and completes through signalTransactionInterrupt (Apple's shape); 0 = the earlier synchronous flip");
NVACCEL_CNT(nvaccel_iop_async_flips,   gCntIopAsync,        "swaps completed asynchronously");
NVACCEL_CNT(nvaccel_iop_async_refused, gCntIopAsyncRefused, "async swaps NVKMS refused (that frame stayed off the panel)");
NVACCEL_CNT(nvaccel_iop_async_drop,    gCntIopAsyncDrop,    "frames dropped because a flip was still in flight past a refresh period");
static SInt64 gCntIopLate;
NVACCEL_CNT(nvaccel_iop_late,          gCntIopLate,         "frames kept on the panel because the previous flip had not reached it yet (framebuffer answered busy)");
static int gCrcHead = 0;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_crc_head, CTLFLAG_RW | CTLFLAG_LOCKED, &gCrcHead, 0, "b85k-crc: which head debug.nvaccel_crc reads");
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS;
SYSCTL_PROC(_debug, OID_AUTO, nvaccel_crc, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, 0, 0, nvaccel_crc_sysctl, "I",
            "b85k-crc: display CRC32 of what head nvaccel_crc_head is showing (0 = unavailable)");
#ifdef NMX_ACCEL
extern struct sysctl_oid sysctl__debug_nmxaccel_scanout_copies, sysctl__debug_nmxaccel_copy_forwards;   // defined with the copy path
// the same handlers under names the NVIDIA accelerator does not own (both may be loaded)
SYSCTL_PROC(_debug, OID_AUTO, nmxaccelfb, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvaccelfb_sysctl, "I", "register IntelXeFB with the accelerator display machine (1, then 3 = scanout pipe)");
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_iop, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN,
            NULL, 0, nvacceliop_sysctl, "I", "1 = display-pipe transactions: WindowServer frames copied to IntelXeFB");
NVACCEL_CNT(nmxaccel_iop_ok,        gCntIopOk,       "display-pipe frames copied to the panel");
NVACCEL_CNT(nmxaccel_iop_fail,      gCntIopFail,     "transactions refused (see the capped iop: log lines)");
NVACCEL_CNT(nmxaccel_iop_empty,     gCntIopEmpty,    "transactions with no plane");
NVACCEL_CNT(nmxaccel_iop_blank,     gCntIopBlank,    "frames copied whose 64 samples were all one colour");
NVACCEL_CNT(nmxaccel_iop_copy_skip, gCntIopCopySkip, "frames kept because the surface's newest pixels were not in system memory");
NVACCEL_CNT(nmxaccel_iop_copy_us,   gCntIopCopyUs,   "microseconds spent copying frames into the scanout (divide by iop_ok)");
#endif
static bool gSysctlInstalled = false;
static void nvaccelInstallSysctl(void)
{
    if (gSysctlInstalled) return;
#ifdef NMX_ACCEL
    // only this one: the NVIDIA accelerator may be loaded too, and owns the debug.nvaccel* names
    sysctl_register_oid(&sysctl__debug_nmxaccel_headless);
    sysctl_register_oid(&sysctl__debug_nmxaccelfb);           sysctl_register_oid(&sysctl__debug_nmxaccel_iop);
    sysctl_register_oid(&sysctl__debug_nmxaccel_iop_ok);      sysctl_register_oid(&sysctl__debug_nmxaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nmxaccel_iop_empty);   sysctl_register_oid(&sysctl__debug_nmxaccel_iop_blank);
    sysctl_register_oid(&sysctl__debug_nmxaccel_iop_copy_skip); sysctl_register_oid(&sysctl__debug_nmxaccel_iop_copy_us);
    sysctl_register_oid(&sysctl__debug_nmxaccel_scanout_copies); sysctl_register_oid(&sysctl__debug_nmxaccel_copy_forwards);
    sysctl_register_oid(&sysctl__debug_nmxaccel_gamma);       sysctl_register_oid(&sysctl__debug_nmxaccel_gamma_loads);
    sysctl_register_oid(&sysctl__debug_nmxaccel_gamma_fail);  sysctl_register_oid(&sysctl__debug_nmxaccel_color_unhandled);
    nmxInstallSysctl();
    gSysctlInstalled = true;
#else
    sysctl_register_oid(&sysctl__debug_nvaccelfb);
    sysctl_register_oid(&sysctl__debug_nvaccel_headless);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc);       sysctl_register_oid(&sysctl__debug_nvaccel_vm_alloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc);     sysctl_register_oid(&sysctl__debug_nvaccel_vm_dealloc_bytes);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_sticky);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_parked);      sysctl_register_oid(&sysctl__debug_nvaccel_vm_unparked);
    sysctl_register_oid(&sysctl__debug_nvaccel_vm_park_released);
    sysctl_register_oid(&sysctl__debug_nvaccel_res_new);        sysctl_register_oid(&sysctl__debug_nvaccel_res_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_commit);     sysctl_register_oid(&sysctl__debug_nvaccel_map_release);
    sysctl_register_oid(&sysctl__debug_nvaccel_map_free);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok);         sysctl_register_oid(&sysctl__debug_nvaccel_iop_fail);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_empty);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_res);    sysctl_register_oid(&sysctl__debug_nvaccel_iop_src_surf);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_blank);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_skip);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip);       sysctl_register_oid(&sysctl__debug_nvaccel_iop_flips);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_home);  sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_lock);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_novram); sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_stale);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_unwritten); sysctl_register_oid(&sysctl__debug_nvaccel_iop_copy_unwritten);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_refused);
    sysctl_register_oid(&sysctl__debug_nvaccel_crc_head);       sysctl_register_oid(&sysctl__debug_nvaccel_crc);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_hit);   sysctl_register_oid(&sysctl__debug_nvaccel_iop_flip_miss);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_flips);
    { extern struct sysctl_oid sysctl__debug_nvaccel_svdiag; sysctl_register_oid(&sysctl__debug_nvaccel_svdiag); }
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu);    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_on);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_marks); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_waits);
    sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_timeouts); sysctl_register_oid(&sysctl__debug_nvaccel_surface_gpu_wait_max_us);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_refused); sysctl_register_oid(&sysctl__debug_nvaccel_iop_async_drop);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_late);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h0);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h1);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h2);      sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h3);
    sysctl_register_oid(&sysctl__debug_nvaccel_iop_ok_h4);      sysctl_register_oid(&sysctl__debug_nvaccel_igpu);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_published); sysctl_register_oid(&sysctl__debug_nvaccel_heads_registered);
    sysctl_register_oid(&sysctl__debug_nvaccel_heads_piped);
    gSysctlInstalled = true;
    ALOG("sysctl debug.nvaccelfb installed -- `sudo sysctl -w debug.nvaccelfb=1` registers the framebuffer");
#endif
}

static inline void nvAccelZero(void *p, unsigned long n)
{ volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static struct NVRMRmHandles gRmHandles;
static bool                 gRmHandlesValid;

static unsigned nvRmHandlesLogged;

static bool nvAccelRmHandles(struct NVRMRmHandles *out)
{
    if (gRmHandlesValid) { *out = gRmHandles; return true; }
    NVAccel *a = NVAccel::gAccel;
    if (!a) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no NVAccel instance yet -- too early"); }
        return false;
    }
    if (!a->fFB) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: no framebuffer has published yet -- will retry"); }
        return false;
    }
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_GPUVA_FN_HANDLES);
    if (!sym) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: OSSymbol allocation failed"); }
        return false;
    }
    struct NVRMRmHandles h;
    nvAccelZero(&h, sizeof h);
    h.version = NVRM_GPUVA_ABI_VERSION;
    IOReturn r = a->fFB->callPlatformFunction(sym, false, &h, nullptr, nullptr, nullptr);
    sym->release();
    if (r != kIOReturnSuccess || !h.kapiDevice) {
        if (nvRmHandlesLogged < 8) { nvRmHandlesLogged++;
            ALOG("nvAccelRmHandles: framebuffer gave no kapi device (ret 0x%x, kapiDevice %p)",
                 (unsigned)r, h.kapiDevice); }
        return false;
    }
    gRmHandles = h; gRmHandlesValid = true;
    ALOG("RM handles: kapiDevice %p (hClient/hDevice resolved inside NVRM by name) "
         "*** the accelerator can now build GPU page tables ***", h.kapiDevice);
    *out = h;
    return true;
}

static IOService *nvAccelNVRM()
{
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nullptr;
    if (!a->fNVRM) {
        OSDictionary *m = IOService::serviceMatching("NVRM");
        if (m) a->fNVRM = IOService::waitForMatchingService(m, 100ULL * 1000 * 1000);
    }
    return a->fNVRM;
}

static IOReturn nvAccelGpuVaCall(const char *fn, struct NVRMGpuVaRequest *g)
{
    IOService *nvrm = nvAccelNVRM();
    if (!nvrm) return kIOReturnNotAttached;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy(fn);
    if (!sym) return kIOReturnNoMemory;
    IOReturn r = nvrm->callPlatformFunction(sym, false, g, nullptr, nullptr, nullptr);
    sym->release();
    return r;
}

#define NVRM_SS_KERNEL_ABI 1
#include "../nvrm_surfshare_abi.h"
extern "C" {
void *_ZN13IOSurfaceRoot13lookupSurfaceEjP4task(void *root, unsigned int id, task_t task);
void *_ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej(void *accel, void *surface, unsigned int plane);
void  _ZN16IOAccelVidMemory4wireEv(void *vidmem);
void  _ZN20IOSurfaceDeviceCache10setCurrentEv(void *cache);
void *_ZNK9IOSurface19getMemoryDescriptorEv(void *surface);
unsigned int _ZNK9IOSurface12getSurfaceIDEv(void *surface);
void  _ZN22IOGraphicsAccelerator29lock_busyEv(void *accel);
void  _ZN22IOGraphicsAccelerator211unlock_busyEv(void *accel);
}
static bool nvFamLock(NVAccel *a, bool tryOnly)
{
    unsigned char *b = (unsigned char *)a;
    IOLock *lk = *(IOLock *volatile *)(b + 0x88);
    if (!lk) return false;
    if (tryOnly) { if (!IOLockTryLock(lk)) return false; }
    else {
        OSIncrementAtomic((volatile SInt32 *)(b + 0x90));
        IOLockLock(lk);
        OSDecrementAtomic((volatile SInt32 *)(b + 0x90));
    }
    _ZN22IOGraphicsAccelerator29lock_busyEv(a);
    a->acceleratorDidLock("", 0);
    return true;
}
static void nvFamUnlock(NVAccel *a)
{
    a->acceleratorWillUnlock("", 0);
    _ZN22IOGraphicsAccelerator211unlock_busyEv(a);
    IOLockUnlock(*(IOLock *volatile *)((unsigned char *)a + 0x88));
}
static unsigned gNVSVLockMiss;
enum { kNVSVPend = 512 };
static unsigned gNVSVPendId[kNVSVPend];
static NVResource *gNVSVPendResource[kNVSVPend];
static IOSimpleLock *gNVSVPendLock;
static bool nvSVPend(unsigned id, int op, NVResource *resource = nullptr)
{
    if (!id) return op == 1 ? false : false;
    if (!gNVSVPendLock) return op == 1 ? false : false;
    bool r = false; int freeSlot = -1;
    IOSimpleLockLock(gNVSVPendLock);
    for (int i = 0; i < kNVSVPend; i++) {
        if (gNVSVPendId[i] == id) {
            r = true;
            if (op == 0) { gNVSVPendId[i] = 0; gNVSVPendResource[i] = nullptr; }
            else if (op == 1) gNVSVPendResource[i] = resource;
            break;
        }
        if (!gNVSVPendId[i] && freeSlot < 0) freeSlot = i;
    }
    if (op == 1 && !r) { if (freeSlot >= 0) { gNVSVPendId[freeSlot] = id; gNVSVPendResource[freeSlot] = resource; r = true; } }
    else if (op == 1) r = true;
    IOSimpleLockUnlock(gNVSVPendLock);
    return r;
}
// A pending mark belongs to the resource that last dirtied the surface. Never
// dereference a device cache during destruction, or clear a newer owner's mark.
static void nvSVForgetResource(NVResource *resource)
{
    if (!resource || !gNVSVPendLock) return;
    IOSimpleLockLock(gNVSVPendLock);
    for (unsigned i = 0; i < kNVSVPend; i++) if (gNVSVPendResource[i] == resource) {
        gNVSVPendId[i] = 0;
        gNVSVPendResource[i] = nullptr;
    }
    IOSimpleLockUnlock(gNVSVPendLock);
}
static void    *gNVSVRoot;
static unsigned gNVSVOk, gNVSVRefused;
static inline void nvSVZero(void *p, unsigned long n) { volatile unsigned char *b = (volatile unsigned char *)p; while (n--) *b++ = 0; }

static unsigned gNVSVDirty, gNVSVDirtyRefused, gNVSVPageoff, gNVSVPageoffFail;
// sysctl debug.nvaccel_svdiag=N logs the next N surface dirty marks and page-offs (surface, result, path); 0 = off
static int gSVDiag;
SYSCTL_INT(_debug, OID_AUTO, nvaccel_svdiag, CTLFLAG_RW | CTLFLAG_LOCKED, &gSVDiag, 0, "log the next N surface dirty marks and page-offs");
static bool nvSVDiag() { return gSVDiag > 0 && OSDecrementAtomic(&gSVDiag) > 0; }
static unsigned nvAccelSurfDirty(void *cache, NVResource *res, unsigned plane)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    unsigned char type = r[0x14];
    unsigned int fl = *(volatile unsigned int *)(r + 0xc);
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    if (type != 0xc0 || (fl & 0x8000) || !valid || plane > 15) return NVRM_SS_NO_CACHE;
    if (!gNVSVPendLock) gNVSVPendLock = IOSimpleLockAlloc();
    if (!nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(*(void *volatile *)((unsigned char *)cache + 0x10)), 1, res)) return NVRM_SS_NO_CACHE;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= (unsigned short)(1u << plane);
    valid[0] &= (unsigned short)~(1u << plane);
    { NVVidMemory *vm = nvres_vidmem(res); res->fPluginWroteCookie = vm ? vm->fAllocationCookie : 0; }
    return 0;
}
// The caller holds the accelerator family lock. The plugin can still write a
// retained allocation after the family unwires it; make that same backing active
// before marking the GPU copy current. Never allocate fresh memory for this case.
static bool nvAccelRestoreParkedForDirty(NVResource *res, NVVidMemory *vm, unsigned plane)
{
    if (!res || !vm || vm->fRmHandle || !vm->fParkHandle) return true;
    const volatile unsigned char *r = (const volatile unsigned char *)res;
    const unsigned flags = *(const volatile unsigned *)(r + 0xc);
    const volatile unsigned short *valid = *(unsigned short *volatile const *)(r + 0x30);
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)vm + 0x40);
    if (vm->fScanout || r[0x14] != 0xc0 || (flags & 0x8000) || !valid || plane > 15 ||
        !want || vm->fParkBytes < want || !vm->fParkPhys || !vm->fParkKva)
        return true;  // Preserve the original refusal and CPU-copy fallback.
    void *const handle = vm->fParkHandle;
    void *const mapping = vm->fParkKva;
    const unsigned long long bytes = vm->fParkBytes, phys = vm->fParkPhys, cookie = vm->fParkCookie;
    // IOAccelVidMemory::wire calls allocPhysical and updates family accounting;
    // it does not page CPU pixels in. The size check above selects allocPhysical's
    // retained-allocation branch, which moves the existing backing without copying.
    _ZN16IOAccelVidMemory4wireEv(vm);
    return vm->fRmHandle == handle && vm->fKva == mapping && vm->fPhys == phys &&
        vm->fBytes == bytes && vm->fAllocationCookie == cookie && !vm->fParkHandle;
}
// The marks arrive through NVK's surface escape (NVRM_ESC_SURF_DIRTY with plane NVRM_SS_PLANE_GPU_MARK | pending): the
// WindowServer sandbox denies sysctl writes. Only WindowServer's own marks are taken, so no other process can hold a swap.
static unsigned nvSurfGpuEscape(const struct NVRMSurfShareEsc *p)
{
    if (!gSurfGpuOn) return NVRM_SS_OFF;
    char nm[32] = { 0 };
    proc_selfname(nm, sizeof nm);
    if (strcmp(nm, "WindowServer")) return NVRM_SS_OFF;
    if (p->surfaceID) nvSurfMark(p->surfaceID, (p->plane & 1u) != 0);
    return 0;
}
static IOReturn nvAccelSurfVram(struct NVRMSurfShareEsc *p, task_t task, unsigned cmd)
{
    if (p->version == NVRM_SURFSHARE_VERSION && cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_GPU_MARK)) {
        p->hSrcClient = p->hSrcMemory = 0;
        p->status = nvSurfGpuEscape(p);
        return kIOReturnSuccess;
    }
    p->hSrcClient = p->hSrcMemory = 0;
    if (p->version != NVRM_SURFSHARE_VERSION) { p->status = NVRM_SS_BAD_VERSION; return kIOReturnSuccess; }
    // The plugin re-marks a surface after its last GPU write on the thread that releases the pending marks. A page-off
    // waiting for one of those marks holds the accelerator lock, so that re-mark must not wait for the lock: it is
    // dropped instead, and the mark taken at commit stands.
    const bool tryOnly = cmd == NVRM_ESC_SURF_DIRTY && (p->plane & NVRM_SS_PLANE_TRYLOCK);
    p->plane &= ~(NVRM_SS_PLANE_TRYLOCK | NVRM_SS_PLANE_GPU_MARK);
    // plane is a full uint32 from the caller and reaches IOGraphicsAccelerator2::deviceCacheForIOSurface
    // below. The plane > 15 refusals further down run after that call, and only on the dirty path.
    if (p->plane > 15) { p->status = NVRM_SS_NO_CACHE; return kIOReturnSuccess; }
    NVAccel *a = NVAccel::gAccel;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (!task || !a || !nvAccelRmHandles(&h)) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
        if (!gNVSVRoot) { p->status = NVRM_SS_OFF; return kIOReturnSuccess; }
    }
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(gNVSVRoot, p->surfaceID, task);
    if (!surf) { p->status = NVRM_SS_NO_SURFACE; gNVSVRefused++; return kIOReturnSuccess; }
    if (!nvFamLock(a, tryOnly)) { ((OSObject *)surf)->release(); p->status = tryOnly ? NVRM_SS_BUSY : NVRM_SS_OFF; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (cmd == NVRM_ESC_SURF_DIRTY) {
        const bool sameBacking = nvAccelRestoreParkedForDirty(res, vm, p->plane);
        p->status = (sameBacking && cache && res && vm && vm->fRmHandle)
            ? nvAccelSurfDirty(cache, res, p->plane) : NVRM_SS_NO_CACHE;
        if (p->status == 0) gNVSVDirty++; else gNVSVDirtyRefused++;
        if (nvSVDiag()) ALOG("SVDIAG dirty surface %u plane %u -> %#x (cache %d res %d vm %d)", p->surfaceID, p->plane, p->status,
                             cache ? 1 : 0, res ? 1 : 0, (vm && vm->fRmHandle) ? 1 : 0);
        if (gNVSVDirty + gNVSVDirtyRefused <= 6 || ((gNVSVDirty + gNVSVDirtyRefused) & 4095) == 0)
            ALOG("surfvram dirty: surface %u plane %u type 0x%x -> %#x (dirty %u, refused %u)", p->surfaceID, p->plane,
                 res ? (unsigned)((volatile unsigned char *)res)[0x14] : 0u, p->status, gNVSVDirty, gNVSVDirtyRefused);
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        ((OSObject *)surf)->release();
        return kIOReturnSuccess;
    }
    if (vm && !vm->fRmHandle && !vm->fScanout) _ZN16IOAccelVidMemory4wireEv(vm);
    if (!cache || !res) p->status = NVRM_SS_NO_CACHE;
    else if (!vm || !vm->fRmHandle || !vm->fBytes) p->status = NVRM_SS_NO_VRAM;
    else if (p->size > vm->fBytes) p->status = NVRM_SS_TOO_SMALL;
    else {
        struct NVRMSurfShareRm g; nvSVZero(&g, sizeof g);
        g.version = NVRM_SS_RM_VERSION; g.op = NVRM_SS_RM_SHARE_KAPI;
        g.kapiDevice = h.kapiDevice; g.kapiMemory = vm->fRmHandle; g.target = p->hClient; g.status = NVRM_SS_OFF;
        IOService *nvrm = nvAccelNVRM();
        const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_RM);
        if (nvrm && sym && nvrm->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) == kIOReturnSuccess) {}
        if (sym) sym->release();
        p->status = g.status; p->hSrcClient = g.hClient; p->hSrcMemory = g.hObject; p->size = vm->fBytes;
    }
    if (p->status == 0) gNVSVOk++; else gNVSVRefused++;
    if (gNVSVOk + gNVSVRefused <= 8 || ((gNVSVOk + gNVSVRefused) & 1023) == 0)
        ALOG("surfvram: surface %u plane %u -> cache %p res %p vidmem %p (%llu B) -> client %#x mem %#x status %#x (ok %u, refused %u)",
             p->surfaceID, p->plane, cache, res, vm, vm ? vm->fBytes : 0ull, p->hSrcClient, p->hSrcMemory, p->status, gNVSVOk, gNVSVRefused);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}

#include <mach/mach_time.h>
static IOLock *volatile gNVSVStageLock;
static IOBufferMemoryDescriptor *gNVSVStageBuf;
static unsigned gNVSVStaged, gNVSVStageNoBuf, gNVSVStageBackoff;
static IOLock *nvSVStageLock(void)
{
    if (!gNVSVStageLock) {
        IOLock *l = IOLockAlloc();
        if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNVSVStageLock)) IOLockFree(l);
    }
    return gNVSVStageLock;
}
static IOBufferMemoryDescriptor *nvSVStage(unsigned long long len)
{
    if (gNVSVStageBuf && gNVSVStageBuf->getLength() >= len) return gNVSVStageBuf;
    if (len > (64ull << 20) || gNVSVStageBackoff) { if (gNVSVStageBackoff) gNVSVStageBackoff--; return nullptr; }
    const unsigned long long want = (len + 0xFFFFFull) & ~0xFFFFFull;
    IOBufferMemoryDescriptor *b = IOBufferMemoryDescriptor::withOptions(kIODirectionInOut | kIOMemoryPhysicallyContiguous,
                                                                      (vm_size_t)want, PAGE_SIZE);
    if (b && b->prepare() != kIOReturnSuccess) { b->release(); b = nullptr; }
    if (!b) {
        gNVSVStageNoBuf++; gNVSVStageBackoff = 1024;
        ALOG("surfvram pageoff: no %llu B physically contiguous staging buffer (miss %u) -- per-page copies for the next 1024", want, gNVSVStageNoBuf);
        return nullptr;
    }
    if (gNVSVStageBuf) { gNVSVStageBuf->complete(); gNVSVStageBuf->release(); }
    gNVSVStageBuf = b;
    ALOG("surfvram pageoff: staging buffer %llu B, physically contiguous", want);
    return b;
}

#ifdef NMX_ACCEL
// ─── B580 VRAM-shared IOSurfaces (gpuperf phase 1; DESIGN-iosurface-vram.md, intelxe_surfvram_kabi.h) ──────────────────
// A surface's family vidmem adopts IntelXe.kext's registry entry for (surface ID, plane): nmxSurfVram wires the vidmem with
// that entry pending (family lock held), allocPhysical takes it instead of asking IntelXeFB for memory, and from then on
// the family's CPU-lock path (deviceCacheControl -> requirePageoff -> pageoffIfNeeded, which insists on an in-VRAM
// vidmem) reaches NVResource::pageoff, which has B580's copy engine fill the surface's pages. nmxSurfDirty sets the
// per-plane valid bits (device newer) after a plugin's GPU write, exactly as the NVIDIA escape does.
static struct { unsigned sid, plane; unsigned long long size; bool failed; } gNmxSVPend;   // family lock
static unsigned gNmxSVAdopt, gNmxSVAdoptFail, gNmxSVImport, gNmxSVDirty, gNmxSVDirtyMiss, gNmxSVPageoff, gNmxSVPageon, gNmxSVCopyFail;
// ─── Demotion of CPU-read VRAM-shared surfaces (rb7f; xe/TTM: a BO the CPU keeps faulting on moves to system memory) ─
// A VRAM-shared surface whose pages the CPU keeps reading is paged off on every read: a synchronous copy of the whole
// surface on B580's copy engine, inside the family's CPU-lock path, so every other user of the family lock waits too
// (WindowServer's dirty marks find it taken). Once a backing has been paged off kSvHotN times within one second it is
// "demoted": a writer that can copy out itself (SURFACE_DIRTY with INTELXE_SURFACE_DIRTY_DEMOTE_OK -> NMX_SV_DEMOTE_OK) is
// answered NMX_SV_DEMOTED instead of being marked newest, and writes the surface's own pages from then on - the pages
// are current when a CPU lock comes, so no page-off. GPU importers keep reading the backing, which the writer still
// renders into. A mark without the flag (a writer that cannot copy out) undoes the demotion, so the pages never go
// stale. Sticky for the backing's life otherwise (released with the vidmem). debug.nmxaccel_sv_demote 0 turns it off.
struct NmxSvHot { void *entry; unsigned sid; uint64_t win0; unsigned n; bool demoted; };
static NmxSvHot gNmxSvHot[64];
static IOSimpleLock *gNmxSvHotLock;
static int gNmxSvDemoteOn = 1, gNmxSvHotN = 6;
static SInt64 gCntSvDemoted, gCntSvDemoteRefused, gCntSvUndemoted;
static IOSimpleLock *nmxSvHotLock()
{
    if (!gNmxSvHotLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxSvHotLock)) IOSimpleLockFree(l); }
    return gNmxSvHotLock;
}
// a page-off of `entry` (surface sid) just completed: count it; true when this one demoted it
static bool nmxSvHotNote(void *entry, unsigned sid)
{
    IOSimpleLock *l = nmxSvHotLock();
    if (!l || !entry || !gNmxSvDemoteOn) return false;
    uint64_t now = mach_absolute_time(), sec = 0;
    nanoseconds_to_absolutetime(1000000000ull, &sec);
    bool demoted = false;
    IOSimpleLockLock(l);
    unsigned slot = 64, victim = 64;
    for (unsigned i = 0; i < 64; i++) {
        if (gNmxSvHot[i].entry == entry) { slot = i; break; }
        if (!gNmxSvHot[i].entry) { if (victim == 64 || gNmxSvHot[victim].entry) victim = i; }
        else if (!gNmxSvHot[i].demoted && (victim == 64 || (gNmxSvHot[victim].entry && gNmxSvHot[i].win0 < gNmxSvHot[victim].win0))) victim = i;
    }
    if (slot == 64 && victim < 64) { slot = victim; gNmxSvHot[slot] = { entry, sid, now, 0, false }; }
    if (slot < 64 && !gNmxSvHot[slot].demoted) {
        NmxSvHot &h = gNmxSvHot[slot];
        if (now - h.win0 > sec) { h.win0 = now; h.n = 0; }
        if (++h.n >= (unsigned)(gNmxSvHotN > 1 ? gNmxSvHotN : 2)) { h.demoted = true; demoted = true; }
    }
    IOSimpleLockUnlock(l);
    if (demoted) {
        const SInt64 n = OSAddAtomic64(1, &gCntSvDemoted) + 1;
        if (n <= 32 || (n & 255) == 0) ALOG("nmx: surfvram: surface %u demoted: %d page-offs within 1 s - its writers copy out from now on (demotions %lld)",
                                            sid, gNmxSvHotN, (long long)n);
    }
    return demoted;
}
// a dirty mark for `entry`: true = refuse it as demoted (the caller copies out); a mark that cannot be refused undoes it
static bool nmxSvHotRefuse(void *entry, unsigned sid, bool demoteOk)
{
    IOSimpleLock *l = gNmxSvHotLock;
    if (!l || !entry) return false;
    bool refuse = false, undone = false;
    IOSimpleLockLock(l);
    for (unsigned i = 0; i < 64; i++)
        if (gNmxSvHot[i].entry == entry) {
            if (gNmxSvHot[i].demoted) {
                if (demoteOk && gNmxSvDemoteOn) refuse = true;
                else { gNmxSvHot[i].demoted = false; gNmxSvHot[i].n = 0; gNmxSvHot[i].win0 = mach_absolute_time(); undone = true; }
            }
            break;
        }
    IOSimpleLockUnlock(l);
    if (refuse) OSAddAtomic64(1, &gCntSvDemoteRefused);
    if (undone) {
        const SInt64 n = OSAddAtomic64(1, &gCntSvUndemoted) + 1;
        if (n <= 16 || (n & 255) == 0) ALOG("nmx: surfvram: surface %u back on the shared path: a writer that cannot copy out marked it (%lld)", sid, (long long)n);
    }
    return refuse;
}
static bool nmxSvHotIsDemoted(void *entry)
{
    IOSimpleLock *l = gNmxSvHotLock;
    if (!l || !entry) return false;
    bool d = false;
    IOSimpleLockLock(l);
    for (unsigned i = 0; i < 64; i++) if (gNmxSvHot[i].entry == entry) { d = gNmxSvHot[i].demoted; break; }
    IOSimpleLockUnlock(l);
    return d;
}
static IOReturn nmxIntelXeCall(const char *fn, void *p1);
// rb7g: a CPU lock of a demoted surface waits for its GPU writers (the copy-out jobs that fill its pages; their fences are in
// IntelXe.kext's surface-fence registry), as IOSurfaceLock waits for the GPU on Apple's drivers. false: IntelXe.kext cannot wait
// (an older kernel) - the caller pages off instead, which waits for the writers too.
static SInt64 gCntSvCpuWaits, gCntSvCpuWaitTimeouts;
static bool nmxSvWaitWriters(unsigned sid)
{
    IntelXeSurfWait w; bzero(&w, sizeof w);
    w.version = INTELXE_SV_KABI_VERSION; w.sid = sid; w.timeoutMs = 1000; w.err = -1;
    if (nmxIntelXeCall("intelxeSurfWaitWriters", &w) != kIOReturnSuccess || w.err == -1 || w.err == 22 /* EINVAL */) return false;
    if (w.fences) OSAddAtomic64(1, &gCntSvCpuWaits);
    if (w.err) {
        const SInt64 n = OSAddAtomic64(1, &gCntSvCpuWaitTimeouts) + 1;
        if (n <= 16) ALOG("nmx: surfvram: CPU lock of demoted surface %u: its GPU writers did not finish in 1 s (err %d) - read as it is", sid, (int)w.err);
    }
    return true;
}
static void nmxSvHotForget(void *entry)
{
    IOSimpleLock *l = gNmxSvHotLock;
    if (!l || !entry) return;
    IOSimpleLockLock(l);
    for (unsigned i = 0; i < 64; i++) if (gNmxSvHot[i].entry == entry) gNmxSvHot[i] = { nullptr, 0, 0, 0, false };
    IOSimpleLockUnlock(l);
}
static IOReturn nmxIntelXeCall(const char *fn, void *p1);
static int nmxSVAdoptPending(NVVidMemory *vm)
{
    if (!gNmxSVPend.sid) return 0;
    const unsigned long long want = *(const volatile unsigned long long *)((const unsigned char *)vm + 0x40);
    IntelXeSurfAdopt a; bzero(&a, sizeof a);
    a.version = INTELXE_SV_KABI_VERSION; a.sid = gNmxSVPend.sid; a.plane = gNmxSVPend.plane;
    a.size = want > gNmxSVPend.size ? want : gNmxSVPend.size;
    if (nmxIntelXeCall("intelxeSurfAdopt", &a) != kIOReturnSuccess || !a.entry) { gNmxSVPend.failed = true; gNmxSVAdoptFail++; return -1; }
    vm->fIntelXe = a.entry; vm->fRmHandle = a.entry; vm->fBytes = a.bytes; vm->fPhys = 0; vm->fKva = nullptr;
    vm->fAllocationCookie = a.cookie;
    *(volatile unsigned long long *)((unsigned char *)vm + 0xc8) = 0;
    gNmxSVAdopt++;
    if (gNmxSVAdopt <= 16 || (gNmxSVAdopt & 255) == 0) ALOG("nmx: surfvram: surface %u plane %u vidmem %p adopted B580 VRAM (%llu KB)", a.sid, a.plane, vm, a.bytes >> 10);
    return 1;
}
static void nmxSVRelease(NVVidMemory *vm)
{
    if (!vm->fIntelXe) return;
    nmxSvHotForget(vm->fIntelXe);
    IntelXeSurfAdopt a; bzero(&a, sizeof a);
    a.version = INTELXE_SV_KABI_VERSION; a.entry = vm->fIntelXe;
    nmxIntelXeCall("intelxeSurfRelease", &a);
    vm->fIntelXe = nullptr; vm->fRmHandle = nullptr; vm->fBytes = 0; vm->fPhys = 0; vm->fKva = nullptr; vm->fAllocationCookie = 0;
    *(volatile unsigned long long *)((unsigned char *)vm + 0xc8) = 0;
}
// the surface's pages <-> its B580 VRAM, on B580's copy engine after the surface's GPU users (bounded: 2 s each)
static bool nmxSVCopy(NVResource *res, NVVidMemory *vm, bool toSys)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    void *cache = *(void *volatile *)(r + 0xe0);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    IOMemoryDescriptor *md = surf ? (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf) : nullptr;
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) { gNmxSVCopyFail++; return false; }
    IntelXeSurfCopy q; bzero(&q, sizeof q);
    q.version = INTELXE_SV_KABI_VERSION; q.toSys = toSys ? 1 : 0; q.entry = vm->fIntelXe; q.md = md;
    q.len = md->getLength() < vm->fBytes ? md->getLength() : vm->fBytes; q.timeoutMs = 2000; q.err = -1;
    const IOReturn ret = nmxIntelXeCall("intelxeSurfCopy", &q);
    const bool ok = ret == kIOReturnSuccess && q.err == 0;
    if (q.err != 60 /* ETIMEDOUT: the copy may still run, so the pages stay wired */) md->complete(kIODirectionNone);
    if (ok) { if (toSys) gNmxSVPageoff++; else gNmxSVPageon++; } else gNmxSVCopyFail++;
    const unsigned n = gNmxSVPageoff + gNmxSVPageon + gNmxSVCopyFail;
    if (n <= 8 || (n & 1023) == 0 || !ok)
        ALOG("nmx: surfvram: %s surface %u %llu KB -> err %d in %llu us (page-offs %u, page-ons %u, failed %u)", toSys ? "page-off" : "page-on",
               surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u, q.len >> 10, (int)q.err, q.usec, gNmxSVPageoff, gNmxSVPageon, gNmxSVCopyFail);
    return ok;
}
void NVResource::pageon(IOAccelEvent *ev, bool a1)
{
    NVVidMemory *vm = nvres_vidmem(this);
    if (vm && vm->fIntelXe) { (void)nmxSVCopy(this, vm, false); return; }
#ifdef NM_TAHOE   // macOS 26 no longer exports IOAccelResource2::pageon by name: call it through the base vtable slot
    nmx_super_IOAccelResource2_pageon_2(void, this, ev, a1);
#else
    IOAccelResource2::pageon(ev, a1);
#endif
}
void NVResource::pageon(IOAccelEvent *ev, bool a1, unsigned long long a2)
{
    NVVidMemory *vm = nvres_vidmem(this);
    if (vm && vm->fIntelXe) { (void)nmxSVCopy(this, vm, false); return; }
#ifdef NM_TAHOE
    nmx_super_IOAccelResource2_pageon_3(void, this, ev, a1, a2);
#else
    IOAccelResource2::pageon(ev, a1, a2);
#endif
}
static void *nmxSVRoot()
{
    if (!gNVSVRoot) {
        OSDictionary *m = IOService::serviceMatching("IOSurfaceRoot");
        if (m) { gNVSVRoot = IOService::waitForMatchingService(m, 5ULL * 1000 * 1000 * 1000); m->release(); }
    }
    return gNVSVRoot;
}
// "nmxSurfVram" / "nmxSurfDirty" (IntelXe.kext's SURFACE_VRAM / SURFACE_DIRTY ioctls, called without any IntelXe lock)
static IOReturn nmxSurfVram(NMXSurfVramReq *p, bool dirty)
{
    if (!p || p->version != INTELXE_SV_KABI_VERSION) return kIOReturnBadArgument;
    p->entry = nullptr; p->status = NMX_SV_OFF;
    NVAccel *a = NVAccel::gAccel;
    if (!a || !p->task || !p->sid || p->plane > 15) { p->status = NMX_SV_BAD; return kIOReturnSuccess; }
    void *root = nmxSVRoot();
    if (!root) return kIOReturnSuccess;
    void *surf = _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(root, p->sid, (task_t)p->task);
    if (!surf) { p->status = NMX_SV_NO_SURFACE; return kIOReturnSuccess; }
    IOMemoryDescriptor *smd = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    const unsigned long long sbytes = smd ? smd->getLength() : 0;
    if (!nvFamLock(a, dirty && (p->flags & NMX_SV_TRYLOCK))) { ((OSObject *)surf)->release(); p->status = NMX_SV_BUSY; return kIOReturnSuccess; }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, p->plane);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    NVVidMemory *vm = res ? nvres_vidmem(res) : nullptr;
    if (!cache || !res) p->status = NMX_SV_NO_CACHE;
    else if (!vm) p->status = NMX_SV_NO_VRAM;
    else if (dirty) {
        if (!vm->fIntelXe) { p->status = NMX_SV_NOT_FOUND; gNmxSVDirtyMiss++; }
        else if (nmxSvHotRefuse(vm->fIntelXe, p->sid, (p->flags & NMX_SV_DEMOTE_OK) != 0)) {   // rb7f: the writer copies out
            // rb7g: marked all the same, so a CPU lock reaches NVResource::pageoff, which waits for the copy-out (no copy)
            if (!vm->fRmHandle) _ZN16IOAccelVidMemory4wireEv(vm);
            (void)nvAccelSurfDirty(cache, res, p->plane);
            p->status = NMX_SV_DEMOTED;
        }
        else { if (!vm->fRmHandle) _ZN16IOAccelVidMemory4wireEv(vm);   // a page-off needs it wired ("in vram")
               p->status = nvAccelSurfDirty(cache, res, p->plane) == 0 ? NMX_SV_OK : NMX_SV_NO_CACHE;
               if (p->status) gNmxSVDirtyMiss++; else gNmxSVDirty++; }
    } else {
        if (vm->fIntelXe && !vm->fRmHandle) _ZN16IOAccelVidMemory4wireEv(vm);   // unwired by the family: wire it again (same VRAM)
        if (!vm->fIntelXe && !vm->fRmHandle && !vm->fScanout && !vm->fParkHandle && (p->flags & NMX_SV_CREATE)) {
            gNmxSVPend.sid = p->sid; gNmxSVPend.plane = p->plane; gNmxSVPend.size = p->size > sbytes ? p->size : sbytes; gNmxSVPend.failed = false;
            _ZN16IOAccelVidMemory4wireEv(vm);
            gNmxSVPend.sid = 0; gNmxSVPend.size = 0;
        }
        if (vm->fIntelXe) {
            IntelXeSurfAdopt g; bzero(&g, sizeof g);
            g.version = INTELXE_SV_KABI_VERSION; g.entry = vm->fIntelXe;
            if (nmxIntelXeCall("intelxeSurfRef", &g) == kIOReturnSuccess) { p->entry = vm->fIntelXe; p->size = vm->fBytes; p->status = NMX_SV_OK; gNmxSVImport++; }
            else p->status = NMX_SV_NO_VRAM;
        } else if (vm->fRmHandle || vm->fScanout || vm->fParkHandle) p->status = NMX_SV_OTHER_BACKING;
        else p->status = (p->flags & NMX_SV_CREATE) ? NMX_SV_NO_VRAM : NMX_SV_NOT_FOUND;
    }
    const unsigned n = gNmxSVImport + gNmxSVDirty + gNmxSVDirtyMiss;
    if ((p->status && p->status != NMX_SV_DEMOTED) || n <= 8 || (n & 4095) == 0)
        ALOG("nmx: surfvram: %s surface %u plane %u -> status %u (imports %u, adopted %u, adopt failures %u, dirty %u, dirty misses %u)",
               dirty ? "dirty" : (p->flags & NMX_SV_CREATE) ? "create" : "import", p->sid, p->plane, p->status, gNmxSVImport, gNmxSVAdopt,
               gNmxSVAdoptFail, gNmxSVDirty, gNmxSVDirtyMiss);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
    ((OSObject *)surf)->release();
    return kIOReturnSuccess;
}
static bool nmxSVCopy(NVResource *res, NVVidMemory *vm, bool toSys);
#endif
#ifdef NMX_ACCEL
static int nmxScanPageoff(unsigned sid, IOMemoryDescriptor *md, bool tryLock);
#endif
void NVResource::pageoff(IOAccelEvent *ev, bool a1, bool *out, unsigned long long a3)
{
    volatile unsigned char *r = (volatile unsigned char *)this;
    void *cache = *(void *volatile *)(r + 0xe0);
    NVVidMemory *vm = nvres_vidmem(this);
#ifdef NMX_ACCEL
    if (vm && vm->fIntelXe) {   // B580 VRAM-shared surface: the copy engine fills the pages; never the family's CPU path
        // rb7g: a demoted surface's pages are filled by its writers' own copy-out jobs: wait for them, copy nothing
        if (nmxSvHotIsDemoted(vm->fIntelXe)) {
            void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
            const unsigned sid = surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u;
            if (sid && nmxSvWaitWriters(sid)) {
                volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
                if (valid) valid[0] |= 1;
                nvSVPend(sid, 0);
                if (out) *out = true;
                return;
            }
        }
        const bool ok = nmxSVCopy(this, vm, true);
        if (ok) {
            volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
            if (valid) valid[0] |= 1;
            void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
            if (surf) nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
            (void)nmxSvHotNote(vm->fIntelXe, surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u);   // rb7f demotion
        }
        if (out) *out = ok;
        return;
    }
#endif
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    void *surf = (r[0x14] == 0xc0 && cache) ? *(void *volatile *)((unsigned char *)cache + 0x10) : nullptr;
    if (nvSVDiag()) {
        volatile unsigned short *vd = *(unsigned short *volatile *)(r + 0x30);
        ALOG("SVDIAG pageoff surface %u type 0x%x vm %d rm %d scanout %d valid0 %#x valid6 %#x", surf ? _ZNK9IOSurface12getSurfaceIDEv(surf) : 0u,
             (unsigned)r[0x14], vm ? 1 : 0, (vm && vm->fRmHandle) ? 1 : 0, vm ? (int)vm->fScanout : -1, vd ? vd[0] : 0xffff, vd ? vd[6] : 0xffff);
    }
    if (!surf || !vm || !vm->fRmHandle || vm->fScanout || !vm->fBytes || !nvAccelRmHandles(&h)) {
#ifdef NMX_ACCEL
        // gpuperf phase 4: a WindowServer composite whose readback the plugin skipped comes from its scan-out buffer
        if (surf) {
            IOMemoryDescriptor *smd = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
            const int pr = (smd && smd->prepare(kIODirectionNone) == kIOReturnSuccess)
                ? nmxScanPageoff(_ZNK9IOSurface12getSurfaceIDEv(surf), smd, true) : 0;
            if (smd && pr != 0) smd->complete(kIODirectionNone);
            else if (smd) smd->complete(kIODirectionNone);
            if (pr > 0) {
                volatile unsigned short *vd = *(unsigned short *volatile *)(r + 0x30);
                if (vd) vd[0] |= 1;
                if (out) *out = true;
                return;
            }
            if (pr < 0) { if (out) *out = false; return; }   // never the family's CPU path: its vidmem is not this frame
        }
        // B580: the surface has no VRAM copy here, its pixels are in its own pages once the plugin's mark is released
        // (the copy into them rides the GPU job that wrote the surface). A CPU lock - a screenshot, a window or Dock
        // capture - waits for that mark as a normal IOSurfaceLock waits for the GPU on Apple's drivers, instead of the
        // plugin guessing at commit which surfaces are captures (NMXSurfaceWaitAll). Bounded: nvSurfWaitReady gives up
        // after kSurfGpuWaitMs and drops the mark, so a hung GPU never holds WindowServer's lock path.
        if (surf && !nvSurfWaitReady(_ZNK9IOSurface12getSurfaceIDEv(surf))) {
            static unsigned said;
            if (said++ < 16) ALOG("pageoff: surface %u: the GPU write it waited for did not finish in %u ms - read as it is",
                                  _ZNK9IOSurface12getSurfaceIDEv(surf), kSurfGpuWaitMs);
        }
#endif
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    // a CPU lock that arrives while WindowServer's GPU is still writing this surface waits for that write first, as a
    // normal IOSurfaceLock does on Apple's drivers (the commit marked it pending; see nvmtl_ws_mark_pending)
    nvSurfWaitReady(_ZNK9IOSurface12getSurfaceIDEv(surf));
    IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
    if (!md || md->prepare(kIODirectionNone) != kIOReturnSuccess) {
        gNVSVPageoffFail++;
        ALOG("surfvram pageoff: res %p surface pages could not be wired (md %p) -- family pageoff instead", this, md);
        IOAccelResource2::pageoff(ev, a1, out, a3);
        return;
    }
    bool mayUnwire = true;
    unsigned long long len = md->getLength();
    if (len > vm->fBytes) len = vm->fBytes;
    unsigned long long pages = (len + 4095) >> 12;
    bool ok = false; unsigned st = NVRM_SS_OFF; unsigned long long usec = 0, cpuUs = 0;
    bool staged = false;
    if (IOLock *lk = nvSVStageLock()) {
        IOLockLock(lk);
        IOBufferMemoryDescriptor *sb = nvSVStage(len);
        IOByteCount sl = 0;
        addr64_t sp = sb ? sb->getPhysicalSegment(0, &sl, kIOMemoryMapperNone) : 0;
        unsigned long long *spa = (sp && sl >= len && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *spa) : nullptr;
        if (spa) {
            for (unsigned long long k = 0; k < pages; k++) spa[k] = sp + k * 4096;
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = spa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec;
            if (st == 0) {
                const uint64_t t0 = mach_absolute_time();
                const IOByteCount w = md->writeBytes(0, sb->getBytesNoCopy(), (IOByteCount)len);
                cpuUs = (mach_absolute_time() - t0) / 1000;
                ok = (w == (IOByteCount)len);
                if (!ok) st = NVRM_SS_TOO_SMALL;
                else gNVSVStaged++;
            } else if (st == 0x65u) {
                ALOG("surfvram pageoff: CE copy TIMED OUT into the staging buffer -- abandoning it (%llu B)", (unsigned long long)sb->getLength());
                gNVSVStageBuf = nullptr;
            }
            staged = true;
            IOFree(spa, pages * sizeof *spa);
        }
        IOLockUnlock(lk);
    }
    unsigned long long *pa = (!staged && pages) ? (unsigned long long *)IOMalloc(pages * sizeof *pa) : nullptr;
    if (pa) {
        unsigned long long i = 0;
        for (IOByteCount off = 0; i < pages; ) {
            IOByteCount seglen = 0;
            addr64_t phys = md->getPhysicalSegment(off, &seglen, kIOMemoryMapperNone);
            if (!phys || !seglen) break;
            for (IOByteCount k = 0; k < seglen && i < pages; k += 4096, off += 4096) pa[i++] = phys + k;
        }
        if (i == pages) {
            struct NVRMSurfPageoff q; nvSVZero(&q, sizeof q);
            q.version = NVRM_SS_RM_VERSION; q.status = NVRM_SS_OFF; q.kapiDevice = h.kapiDevice; q.kapiMemory = vm->fRmHandle;
            q.pages = pa; q.pageCount = pages; q.length = len;
            IOService *nvrm = nvAccelNVRM();
            const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_SS_FN_PAGEOFF);
            if (nvrm && sym) nvrm->callPlatformFunction(sym, false, &q, nullptr, nullptr, nullptr);
            if (sym) sym->release();
            st = q.status; usec = q.usec; ok = st == 0;
            if (st == 0x65u) mayUnwire = false;
        }
        IOFree(pa, pages * sizeof *pa);
    }
    if (mayUnwire) md->complete(kIODirectionNone);
    else ALOG("surfvram pageoff: res %p CE copy TIMED OUT -- surface pages stay wired (%llu B)", this, len);
    if (ok) {
        nvSVPend(_ZNK9IOSurface12getSurfaceIDEv(surf), 0);
        volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
        if (valid) valid[0] |= 1;
        gNVSVPageoff++;
    } else gNVSVPageoffFail++;
    if (out) *out = ok;
    if (gNVSVPageoff + gNVSVPageoffFail <= 8 || ((gNVSVPageoff + gNVSVPageoffFail) & 1023) == 0)
        ALOG("surfvram pageoff: res %p %llu B on the copy engine -> %#x in %llu us%s + cpu %llu us (ok %u, failed %u, staged %u)",
             this, len, st, usec, staged ? " (staged)" : " (per page)", cpuUs, gNVSVPageoff, gNVSVPageoffFail, gNVSVStaged);
}

static IOSurface *gIopFlipFront[kNvMaxFB], *gIopFlipPrev[kNvMaxFB];
static bool gIopFlipped[kNvMaxFB];
static unsigned gIopFlipLogs;
static bool nvAccelIopFlipNo(int why) { OSAddAtomic64(1, &gCntIopFlipNo[why & 7]); return false; }
struct NVIopFlipCache { IOSurface *s; NVResource *res; NVVidMemory *vm; void *kmem; unsigned long long bytes; unsigned long long use; };
static NVIopFlipCache gIopFlipCache[8];
static IOSimpleLock *gIopFlipCacheLock;
static unsigned long long gIopFlipCacheTick;
static bool nvAccelIopCacheGet(IOSurface *s, NVResource **res, NVVidMemory **vm, void **kmem, unsigned long long *bytes)
{
    if (!gIopFlipCacheLock) return false;
    bool hit = false;
    IOSimpleLockLock(gIopFlipCacheLock);
    for (unsigned i = 0; i < 8; i++) if (gIopFlipCache[i].s == s) {
        NVVidMemory *v = gIopFlipCache[i].vm;
        if (v && v->fRmHandle == gIopFlipCache[i].kmem && v->fBytes == gIopFlipCache[i].bytes) {
            *res = gIopFlipCache[i].res; *vm = v; *kmem = gIopFlipCache[i].kmem; *bytes = gIopFlipCache[i].bytes;
            gIopFlipCache[i].use = ++gIopFlipCacheTick; hit = true;
        }
        break;
    }
    IOSimpleLockUnlock(gIopFlipCacheLock);
    return hit;
}
static void nvAccelIopCachePut(IOSurface *s, NVResource *res, NVVidMemory *vm, void *kmem, unsigned long long bytes)
{
    if (!gIopFlipCacheLock) { IOSimpleLock *l = IOSimpleLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopFlipCacheLock)) IOSimpleLockFree(l); }
    if (!gIopFlipCacheLock) return;
    IOSurface *drop = nullptr;
    ((OSObject *)s)->retain();
    IOSimpleLockLock(gIopFlipCacheLock);
    unsigned k = 0;
    for (unsigned i = 0; i < 8; i++) {
        if (gIopFlipCache[i].s == s) { k = i; break; }
        if (!gIopFlipCache[i].s) { k = i; break; }
        if (gIopFlipCache[i].use < gIopFlipCache[k].use) k = i;
    }
    drop = gIopFlipCache[k].s;
    gIopFlipCache[k].s = s; gIopFlipCache[k].res = res; gIopFlipCache[k].vm = vm; gIopFlipCache[k].kmem = kmem;
    gIopFlipCache[k].bytes = bytes; gIopFlipCache[k].use = ++gIopFlipCacheTick;
    IOSimpleLockUnlock(gIopFlipCacheLock);
    if (drop) ((OSObject *)drop)->release();
}
extern "C" {
void _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(void *pipe, void *arg);
struct thread_call *thread_call_allocate(void (*func)(void *, void *), void *param0);
int thread_call_enter(struct thread_call *call);
}
// next/nextF: one frame WindowServer presented while the previous flip was still in flight past the bound, taken by
// the worker as soon as that flip lands (guarded by gIopAsLock, like busy's clearing).
struct NVIopAsync { void *pipe; IOService *fb; IOSurface *s; struct NVRMFlipRequest f; volatile SInt32 busy; struct thread_call *call;
                    IOSurface *next; struct NVRMFlipRequest nextF; };
static NVIopAsync gIopAs[kNvMaxFB];
static SInt32 gIopAsyncRefuseRun;
// WindowServer's next present waits here while the previous asynchronous flip is still on its way to the panel. It
// waited in IOSleep(1) steps, each overshooting by a millisecond or more - a large slice of a 6.9 ms frame at 144 Hz,
// where the desktop then ran near half the panel rate. The flip's worker wakes the waiter when it finishes; the busy
// flag is still re-read at least every 250 us. The bound is one refresh period and a quarter, published by the
// framebuffer from its mode (NVRMFlipWaitUs): a flip in flight lands at the next vblank, so a wait past that only
// holds WindowServer's thread; the frame is dropped instead and the next one follows the flip.
static IOLock *gIopAsLock;
static uint64_t nvAccelFlipWaitUs(IOService *fb)
{
    // A retained copy: a mode switch republishes the property, which releases the number getProperty would return.
    OSObject *o = fb ? fb->copyProperty("NVRMFlipWaitUs") : nullptr;
    OSNumber *n = OSDynamicCast(OSNumber, o);
    const uint64_t us = n ? n->unsigned64BitValue() : 0;
    if (o) o->release();
    return us ? us : 50000;   // before the framebuffer has a mode, the old 50 ms
}
// What the framebuffer's answer to a flip means for the frame: on the panel, kept (busy: the flip before it is still
// landing, the next present follows it), or refused (copied, and three in a row turn the asynchronous swap off).
enum { kNvFlipShown = 0, kNvFlipKept = 1, kNvFlipRefused = 2 };
static int nvAccelFlipOutcome(IOReturn rr)
{
    return rr == kIOReturnSuccess ? kNvFlipShown : rr == kIOReturnBusy ? kNvFlipKept : kNvFlipRefused;
}
static bool nvAccelIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gIopAs[h].pipe == pipe && __atomic_load_n(&gIopAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static void nvAccelIopAsyncRun(void *p0, void *)
{
    NVIopAsync *e = (NVIopAsync *)p0;
    const unsigned head = (unsigned)(e - gIopAs);
    IOSurface *s = e->s; e->s = nullptr;
  again:
    nvSurfWaitReady(s->getSurfaceID());   // WindowServer did not wait for this composite; the flip does, off its thread
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? e->fb->callPlatformFunction(sym, false, &e->f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    const int outcome = nvAccelFlipOutcome(rr);
    if (outcome == kNvFlipShown) {
        gIopAsyncRefuseRun = 0;
        if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
        gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
        const SInt64 n = OSAddAtomic64(1, &gCntIopFlip); OSAddAtomic64(1, &gCntIopAsync);
        if (n < 4 || n == 600 || (n & 0xffff) == 0)
            ALOG("iop: ASYNC swap #%lld head %u: surface 0x%x %ux%u pitch %u scanned out of its own VRAM (flipResult %d)",
                 (long long)n + 1, head, s->getSurfaceID(), e->f.width, e->f.height, e->f.pitch, e->f.flipResult);
    } else if (outcome == kNvFlipKept) {
        OSAddAtomic64(1, &gCntIopLate);
        ((OSObject *)s)->release();
    } else {
        OSAddAtomic64(1, &gCntIopAsyncRefused); OSAddAtomic64(1, &gCntIopFlipNo[7]);
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: async flip REFUSED (0x%x, flipResult %d) head %u -- that frame is not on the panel",
                                                     (unsigned)rr, e->f.flipResult, head); }
        if (++gIopAsyncRefuseRun >= 3 && gIopAsync) {
            gIopAsync = 0;
            ALOG("iop: b89k 3 async refusals in a row -- debug.nvaccel_iop_async=0 (synchronous flip, which copies on refusal)");
        }
        ((OSObject *)s)->release();
    }
    // A frame queued behind this flip follows it at once; otherwise the worker is free and the waiter is woken.
    IOSurface *queued = nullptr;
    if (gIopAsLock) {
        IOLockLock(gIopAsLock);
        queued = e->next; e->next = nullptr;
        if (queued) { for (unsigned i = 0; i < sizeof e->f; i++) ((volatile unsigned char *)&e->f)[i] = ((const volatile unsigned char *)&e->nextF)[i]; }
        else { __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE); IOLockWakeup(gIopAsLock, (event_t)&e->busy, false); }
        IOLockUnlock(gIopAsLock);
    } else {
        __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
    }
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
    if (queued) { s = queued; goto again; }
}
static bool nvAccelIopAsyncStart(void *pipe, IOService *fb, unsigned head, IOSurface *s, const struct NVRMFlipRequest *f)
{
    NVIopAsync *e = &gIopAs[head];
    if (!gIopAsLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gIopAsLock)) IOLockFree(l); }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        uint64_t end = 0; clock_interval_to_deadline((uint32_t)nvAccelFlipWaitUs(fb), kMicrosecondScale, &end);
        while (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && mach_absolute_time() < end) {
            if (!gIopAsLock) { IOSleep(1); continue; }
            uint64_t next = 0; clock_interval_to_deadline(250, kMicrosecondScale, &next);
            if (next > end) next = end;
            IOLockLock(gIopAsLock);
            if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE))
                IOLockSleepDeadline(gIopAsLock, (event_t)&e->busy, next, THREAD_UNINT);
            IOLockUnlock(gIopAsLock);
        }
    }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && gIopAsLock) {
        // Still in flight past the bound: this frame is queued for the worker instead of holding WindowServer any
        // longer, replacing an older queued frame (that one is the dropped frame the counter names).
        IOLockLock(gIopAsLock);
        if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE) && e->pipe == pipe && e->fb == fb) {
            IOSurface *old = e->next;
            ((OSObject *)s)->retain();
            e->next = s;
            for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->nextF)[i] = ((const unsigned char *)f)[i];
            IOLockUnlock(gIopAsLock);
            if (old) { OSAddAtomic64(1, &gCntIopAsyncDrop); ((OSObject *)old)->release(); }
            return true;
        }
        IOLockUnlock(gIopAsLock);
    }
    if (__atomic_load_n(&e->busy, __ATOMIC_ACQUIRE)) {
        OSAddAtomic64(1, &gCntIopAsyncDrop);
        return true;
    }
    if (!e->call) e->call = thread_call_allocate(nvAccelIopAsyncRun, e);
    if (!e->call) return false;
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->s = s;
    for (unsigned i = 0; i < sizeof *f; i++) ((volatile unsigned char *)&e->f)[i] = ((const unsigned char *)f)[i];
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    thread_call_enter(e->call);
    return true;
}
static bool nvAccelIopFlip(IOService *fb, unsigned head, IOSurface *s, void *pipe)
{
    if (!gIopFlip || !fb || !s || head >= kNvMaxFB) return false;
    // zero-copy scan-out is an NVIDIA display-engine flip; another vendor's panel (NMIntelFB) always takes the copy
    if (!fb->metaCast("NVRMNVDAFramebuffer")) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a) return nvAccelIopFlipNo(1);
    if (s->getPixelFormat() != 0x42475241u) return nvAccelIopFlipNo(2);
    NVResource *res = nullptr; NVVidMemory *vm = nullptr; void *kmem = nullptr; unsigned long long bytes = 0;
    if (nvAccelIopCacheGet(s, &res, &vm, &kmem, &bytes)) {
        OSAddAtomic64(1, &gCntIopFlipHit);
    } else {
        if (!nvFamLock(a, true)) return nvAccelIopFlipNo(3);
        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
        res = OSDynamicCast(NVResource, ctx);
        vm = res ? nvres_vidmem(res) : nullptr;
        kmem = (vm && vm->fRmHandle && vm->fBytes && !vm->fScanout) ? vm->fRmHandle : nullptr;
        bytes = vm ? vm->fBytes : 0;
        if (cache) ((OSObject *)cache)->release();
        nvFamUnlock(a);
        OSAddAtomic64(1, &gCntIopFlipMiss);
        if (kmem) nvAccelIopCachePut(s, res, vm, kmem, bytes);
    }
    volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
    const bool vramNewest = valid && (valid[6] & 1u);
    if (!kmem) return nvAccelIopFlipNo(4);
    if (!vramNewest) return nvAccelIopFlipNo(5);
    // Checked against the allocation this flip submits (vm, resolved above and cached), not re-resolved from the
    // resource: this runs outside the family lock, and the record must describe the VRAM that is about to be scanned.
    if (!nvAccelIopVramFlippable(vramNewest, nvAccelPluginWrote(res->fPluginWroteCookie, vm->fAllocationCookie))) {
        const SInt64 n = OSAddAtomic64(1, &gCntIopUnwritten);
        if (n < 4 || (n & 0xfff) == 0)
            ALOG("iop: surface 0x%x: VRAM marked newest but the plugin never wrote it -- copying its system memory instead of flipping (%lld so far)",
                 s->getSurfaceID(), (long long)n + 1);
        return false;
    }
    const unsigned long long W = s->getWidth(), H = s->getHeight(), P = s->getBytesPerRow();
    if (!W || !H || P < 4 * W || H * P > bytes) return nvAccelIopFlipNo(6);
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.kapiMemory = kmem; f.allocationCookie = vm->fAllocationCookie; f.width = (unsigned)W; f.height = (unsigned)H; f.pitch = (unsigned)P;
    if (gIopAsync && pipe) return nvAccelIopAsyncStart(pipe, fb, head, s, &f);
    nvSurfWaitReady(s->getSurfaceID());
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    if (nvAccelFlipOutcome(rr) == kNvFlipKept) { OSAddAtomic64(1, &gCntIopLate); return true; }   // the frame on the panel stays
    if (rr != kIOReturnSuccess) {
        if (gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: flip REFUSED (0x%x, flipResult %d) head %u %llux%llu pitch %llu -- copying",
                                                     (unsigned)rr, f.flipResult, head, W, H, P); }
        return nvAccelIopFlipNo(7);
    }
    ((OSObject *)s)->retain();
    if (gIopFlipPrev[head]) ((OSObject *)gIopFlipPrev[head])->release();
    gIopFlipPrev[head] = gIopFlipFront[head]; gIopFlipFront[head] = s; gIopFlipped[head] = true;
    const SInt64 n = OSAddAtomic64(1, &gCntIopFlip);
    if (n < 4 || n == 600 || (n & 0xffff) == 0)
        ALOG("iop: zero-copy swap #%lld head %u: surface 0x%x %llux%llu pitch %llu scanned out of its own VRAM (flipResult %d)",
             (long long)n + 1, head, s->getSurfaceID(), W, H, P, f.flipResult);
    return true;
}
static int nvaccel_crc_sysctl SYSCTL_HANDLER_ARGS
{
    unsigned v = 0;
    NVAccel *a = NVAccel::gAccel;
    IOService *fb = a ? a->fFB : nullptr;
    IOService *nub = fb ? fb->getProvider() : nullptr;
    OSNumber *n = nub ? OSDynamicCast(OSNumber, nub->getProperty("nvkms-kapi")) : nullptr;
#ifndef NMX_ACCEL   // NVKMS display CRC: there is no NVKMS behind an AMD/Intel accelerator
    struct NvKmsKapiFunctionsTable *k = n ? (struct NvKmsKapiFunctionsTable *)(uintptr_t)n->unsigned64BitValue() : nullptr;
    struct NVRMRmHandles h; nvSVZero(&h, sizeof h);
    if (k && k->getCRC32 && gCrcHead >= 0 && gCrcHead < 8 && nvAccelRmHandles(&h) && h.kapiDevice) {
        struct NvKmsKapiCrcs c; nvSVZero(&c, sizeof c);
        if (k->getCRC32((struct NvKmsKapiDevice *)h.kapiDevice, (NvU32)gCrcHead, &c))
            v = c.compositorCrc32.supported ? c.compositorCrc32.value : (c.outputCrc32.supported ? c.outputCrc32.value : 0);
    }
#else
    (void)n;
#endif
    int out = (int)v;
    return sysctl_handle_int(oidp, &out, 0, req);
}
static void nvAccelIopHome(IOService *fb, unsigned head)
{
    if (!fb || head >= kNvMaxFB || !gIopFlipped[head] || !fb->metaCast("NVRMNVDAFramebuffer")) return;
    struct NVRMFlipRequest f; nvSVZero(&f, sizeof f);
    f.version = NVRM_FLIP_ABI_VERSION; f.flags = 1u;
    const OSSymbol *sym = OSSymbol::withCStringNoCopy("nvFlipToSurfacePure");
    IOReturn rr = sym ? fb->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    OSAddAtomic64(1, &gCntIopFlipHome);
    // Busy: the flip before HOME is still landing and HOME was not submitted, so the panel is not home yet and the
    // next copied frame tries again. Clearing the flag here left every later copy offscreen behind that flip.
    if (rr == kIOReturnBusy) return;
    if (rr != kIOReturnSuccess && gIopFlipLogs < 8) { gIopFlipLogs++; ALOG("iop: HOME flip refused (0x%x) head %u", (unsigned)rr, head); }
    gIopFlipped[head] = false;
}
static void nvAccelSurfScanoutSync(IOSurface *s)
{
    if (!s || !NVAccel::gAccel || !nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 2)) return;
    NVAccel *a = NVAccel::gAccel;
    if (!nvFamLock(a, true)) {
        if (++gNVSVLockMiss <= 8 || (gNVSVLockMiss & 1023) == 0)
            ALOG("surfvram scanout: accelerator lock busy, surface %u stays pending (misses %u)",
                 _ZNK9IOSurface12getSurfaceIDEv((void *)s), gNVSVLockMiss);
        return;
    }
    void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
    OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
    NVResource *res = OSDynamicCast(NVResource, ctx);
    bool out = false;
    if (res) res->pageoff(nullptr, true, &out, 0);
    if (!out) nvSVPend(_ZNK9IOSurface12getSurfaceIDEv((void *)s), 0);
    if (cache) ((OSObject *)cache)->release();
    nvFamUnlock(a);
}
// True when the surface's system memory holds its newest pixels, after moving them there if the GPU drew them into VRAM.
// Unlike nvAccelSurfScanoutSync it waits a few ms for the accelerator lock and does not clear the pending mark when the
// page-off fails. At most 4 frames in a row are held back per head: a surface that never syncs is copied as before
// rather than freezing the panel.
static unsigned gIopCopyHeld[kNvMaxFB];
static bool nvAccelSurfCopyReady(IOSurface *s, unsigned head)
{
    NVAccel *a = NVAccel::gAccel;
    if (!s || !a) return true;
    const unsigned id = _ZNK9IOSurface12getSurfaceIDEv((void *)s);
    bool ready = !nvSVPend(id, 2);
    if (!ready) {
        bool locked = false;
        for (unsigned i = 0; i < 8 && !(locked = nvFamLock(a, true)); i++) IOSleep(1);
        if (locked) {
            void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, (void *)s, 0);
            OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
            NVResource *res = OSDynamicCast(NVResource, ctx);
            volatile unsigned short *valid = res ? *(unsigned short *volatile *)((volatile unsigned char *)res + 0x30) : nullptr;
            if (!res || !valid || (valid[0] & 1u)) ready = true;
            else if (!nvAccelIopCopyNeedsPageoff(false, nvAccelResPluginWrote(res))) { ready = true; OSAddAtomic64(1, &gCntIopCopyUnwritten); }
            else { bool out = false; res->pageoff(nullptr, true, &out, 0); ready = out || (valid[0] & 1u); }
            if (ready) nvSVPend(id, 0);
            if (cache) ((OSObject *)cache)->release();
            nvFamUnlock(a);
        }
    }
    if (head >= kNvMaxFB) return true;
    if (ready) { gIopCopyHeld[head] = 0; return true; }
    if (++gIopCopyHeld[head] > 4) { gIopCopyHeld[head] = 0; return true; }
    OSAddAtomic64(1, &gCntIopCopySkip);
    return false;
}

static unsigned nvVaAllocLogged, nvVaMapLogged;

#ifdef NMX_ACCEL
// AMD/Intel: nothing behind this accelerator maps memory for the GPU (the plugin's Vulkan driver keeps its own address
// space, and the display pipe copies frames with the CPU), but the family still asks a GPU address for every memory map
// and fails IOAccelResource2::prepare without one: WindowServer's framebuffer resource then never prepared, the display
// pipe never went active (init_framebuffer_resource: "failed to prepare framebuffer resource") and nothing was
// composited (2026-10-28, B580). Hand out unique placeholder addresses from a range no GPU address of ours is in.
static volatile SInt64 gNmxVaNext = 0;
static const unsigned long long kNmxVaBase = 0x200000000000ull, kNmxVaSize = 0x100000000000ull;   // 16 TB
#endif
static unsigned long long nvAccelGpuVaAlloc(IOAccelMemoryMap *map,
                                            unsigned long long len, unsigned long long align)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !len) return 0;
    if (mm->fGpuVa) return mm->fGpuVa;
#ifdef NMX_ACCEL
    {
        // Every map gets one (the family asks before it backs the memory). WindowServer's CPU blits hit NULL
        // (SIGSEGV in layer_blit_byte) while the framebuffer could not back VRAM maps; IntelXeFB's nvAllocVram does now.
        const unsigned long long a = align > 0x10000 ? align : 0x10000;
        const unsigned long long sz = (len + a - 1) & ~(a - 1);
        unsigned long long off = (unsigned long long)OSAddAtomic64((SInt64)(sz + a), &gNmxVaNext);
        off = (off + a - 1) & ~(a - 1);
        if (off + sz > kNmxVaSize) return 0;   // a 16 TB range is not expected to run out before a reboot
        mm->fHVirt = 1; mm->fGpuVa = kNmxVaBase + off; mm->fVaLen = len;
        return mm->fGpuVa;
    }
#endif

    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return 0;

    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.size = len; g.align = align;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_ALLOC, &g) != kIOReturnSuccess || !g.gpuva) {
        if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
            ALOG("gpuVaAlloc: RM refused %llu bytes (align %llu) -> status 0x%x", len, align, g.status); }
        return 0;
    }
    mm->fHVirt = g.hVirt; mm->fGpuVa = g.gpuva; mm->fVaLen = len;
    if (nvVaAllocLogged < 8) { nvVaAllocLogged++;
        ALOG("gpuVaAlloc: %llu bytes -> gpuVA 0x%llx (hVirt 0x%x)", len, g.gpuva, g.hVirt); }
    return g.gpuva;
}

static void nvAccelGpuVaFree(IOAccelMemoryMap *map)
{
    NVMemoryMap *mm = OSDynamicCast(NVMemoryMap, map);
    if (!mm || !mm->fHVirt) return;
#ifdef NMX_ACCEL
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;   // placeholder addresses are not reused
    return;
#endif
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = mm->fHVirt; g.hMemory = mm->fMapped ? mm->fHRmMemory : 0; g.size = mm->fVaLen;
    g.gpuva = mm->fGpuVa;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    OSAddAtomic64(1, &gCntMapFree);
    mm->fHVirt = 0; mm->fGpuVa = 0; mm->fVaLen = 0; mm->fMapped = false;
}

bool NVMemoryMap::commitIntoGPUPageTable()
{
    if (fMapped) return true;
#ifdef NMX_ACCEL
    if (fGpuVa) { fMapped = true; OSAddAtomic64(1, &gCntMapCommit); return true; }   // nothing to map (see nvAccelGpuVaAlloc)
#endif
    NVVidMemory *vm = OSDynamicCast(NVVidMemory, fMem);
    void *kapiMem = vm ? vm->fRmHandle : nullptr;
    unsigned long long len = fVaLen ? fVaLen : getLength();
    if (!fHVirt || !kapiMem || !len) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: NOT MAPPED -- hVirt 0x%x kapiMem %p len %llu (mem %p%s)",
                 fHVirt, kapiMem, len, fMem, vm ? "" : ", not an NVVidMemory"); }
        return false;
    }
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return false;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice; g.kapiMemory = kapiMem;
    g.hVirt = fHVirt; g.size = len;
    g.isVidmem = 1;
    if (nvAccelGpuVaCall(NVRM_GPUVA_FN_MAP, &g) != kIOReturnSuccess) {
        if (fCommits < 8) { fCommits++;
            ALOG("MM commit: MAP_MEMORY_DMA refused gpuVA 0x%llx len %llu -> status 0x%x",
                 fGpuVa, len, g.status); }
        return false;
    }
    fHRmMemory = g.hMemory; fMapped = true;
    OSAddAtomic64(1, &gCntMapCommit);
    if (g.gpuva) fGpuVa = g.gpuva;
    if (fCommits < 8) { fCommits++;
        ALOG("MM commit: MAPPED gpuVA 0x%llx len %llu <- hMem 0x%x  *** the GPU can reach these bytes ***",
             fGpuVa, len, g.hMemory); }
    return true;
}

void NVMemoryMap::releaseFromGPUPageTable()
{
    if (!fMapped || !fHVirt) return;
#ifdef NMX_ACCEL
    fMapped = false; OSAddAtomic64(1, &gCntMapRelease); return;
#endif
    struct NVRMRmHandles h;
    if (!nvAccelRmHandles(&h)) return;
    struct NVRMGpuVaRequest g;
    nvAccelZero(&g, sizeof g);
    g.version = NVRM_GPUVA_ABI_VERSION;
    g.kapiDevice = h.kapiDevice;
    g.hVirt = fHVirt; g.hMemory = fHRmMemory; g.size = fVaLen;
    g.gpuva = fGpuVa;
    g.flags = NVRM_GPUVA_FLAG_UNMAP_ONLY;
    nvAccelGpuVaCall(NVRM_GPUVA_FN_FREE, &g);
    fMapped = false;
    OSAddAtomic64(1, &gCntMapRelease);
}

static void nvAccelWriteStamp(int index, unsigned int value)
{
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fStamps || index < 0 || index >= 4096) return;
    volatile unsigned int *base = (volatile unsigned int *)a->fStamps->getBytesNoCopy();
    if (!base) return;
    base[index] = value;
}

static unsigned nvFrontSamples, nvFrontFlips;
static unsigned nvFrontLastSum;
static void nvAccelSampleFront(NVVidMemory *vm, unsigned w, unsigned h, unsigned pitch)
{
    if (!vm || !vm->fKva || !pitch || !w || !h) return;
#define NVFRONT_EVERY 10u
    if (++nvFrontFlips % NVFRONT_EVERY) return;
    if (nvFrontSamples >= 40) return;
    nvFrontSamples++;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    const unsigned p1 = (w > 100  && h > 100)  ? px[100  * ppr + 100]  : 0u;
    const unsigned p2 = (w > 960  && h > 540)  ? px[540  * ppr + 960]  : 0u;
    const unsigned p3 = (w > 1800 && h > 1000) ? px[1000 * ppr + 1800] : 0u;
    const bool moved = (nvFrontSamples > 1) && (sum != nvFrontLastSum);
    ALOG("FRONT SURFACE sample %u: sum 0x%08x %s nonzero %u/%u "
         "px(100,100)=0x%08x px(960,540)=0x%08x px(1800,1000)=0x%08x %ux%u pitch %u",
         nvFrontSamples, sum,
         moved ? "*** MOVED -- A LIVE WRITER IS PAINTING THE SCANNED-OUT SURFACE ***"
               : (nvFrontSamples > 1 ? "(UNCHANGED since last sample -- frozen, not proof of a picture)"
                                     : "(first sample -- motion is the proof, wait for the next)"),
         nonzero, n, p1, p2, p3, w, h, pitch);
    nvFrontLastSum = sum;
}

static unsigned nvSwapProbes;
static void nvAccelResStats(IOAccelResource2 *res, const char *tag)
{
    if (!res) { ALOG("  swap %s: (null)", tag); return; }
    NVVidMemory *vm = nvres_vidmem(res);
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!vm || !vm->fKva || !pitch || !w || !h) {
        ALOG("  swap %s: %ux%u pitch %u vm %p kva %p scanout %d -- NOT SAMPLEABLE",
             tag, w, h, pitch, vm, vm ? vm->fKva : nullptr, vm ? (int)vm->fScanout : -1);
        return;
    }
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    unsigned n = 0, nonzero = 0, sum = 0;
    for (unsigned y = 0; y < h; y += 8)
        for (unsigned x = 0; x < w; x += 8) {
            unsigned v = px[y * ppr + x];
            sum = sum * 33u + v; if (v) nonzero++; n++;
        }
    ALOG("  swap %s: %ux%u pitch %u scanout %d  sum 0x%08x  nonzero %u/%u  %s",
         tag, w, h, pitch, (int)vm->fScanout, sum, nonzero, n,
         nonzero ? "*** HAS CONTENT -- THIS IS THE SOURCE ***" : "(empty)");
}
bool nvAccelResHasContent(IOAccelResource2 *res)
{
    if (!res) return false;
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) return false;
    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || pitch < 4) return false;
    const volatile unsigned int *px = (const volatile unsigned int *)vm->fKva;
    const unsigned ppr = pitch / 4;
    for (unsigned iy = 1; iy <= 4; iy++)
        for (unsigned ix = 1; ix <= 4; ix++) {
            const unsigned x = (w * ix) / 5, y = (h * iy) / 5;
            if (x < w && y < h && px[y * ppr + x]) return true;
        }
    return false;
}

void nvAccelSwapProbe(IOAccelResource2 *a1, IOAccelResource2 *a2)
{
    if (nvSwapProbes >= 6) return;
    nvSwapProbes++;
    ALOG("SWAP PROBE %u -- which argument holds the composited frame?", nvSwapProbes);
    nvAccelResStats(a1, "a1");
    nvAccelResStats(a2, "a2");
}

static unsigned nvFlipWhy = 0;
#define NVFLIPWHY(...) do { if (nvFlipWhy < 20) { nvFlipWhy++; ALOG(__VA_ARGS__); } } while (0)

#ifdef NMX_ACCEL
#define NVSCANOUT_COPY_MAX 0x7fffffffu   // the copy path IS the display on AMD/Intel: no cap
#else
#define NVSCANOUT_COPY_MAX 120
#endif
static unsigned nvScanoutCopies = 0;
#ifdef NMX_ACCEL
static unsigned nvCopyForwards = 0;
SYSCTL_UINT(_debug, OID_AUTO, nmxaccel_scanout_copies, CTLFLAG_RD | CTLFLAG_LOCKED, &nvScanoutCopies, 0,
            "WindowServer swaps copied onto the panel (the page-flip path)");
SYSCTL_UINT(_debug, OID_AUTO, nmxaccel_copy_forwards, CTLFLAG_RD | CTLFLAG_LOCKED, &nvCopyForwards, 0,
            "copy-forward requests WindowServer made before drawing a frame");
#endif
static bool nvAccelCopyToScanout(IOAccelResource2 *res)
{
    if (nvScanoutCopies >= NVSCANOUT_COPY_MAX) return false;
    if (!res) return false;
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("copyToScanout: no accelerator/framebuffer"); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("copyToScanout: refused -- heads piped 0x%x, no way to tell whose frame this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fKva) { NVFLIPWHY("copyToScanout: source has no kva"); return false; }

    const unsigned char *rr = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(rr + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(rr + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(rr + 0xb8);
    if (!w || !h || pitch < 4) { NVFLIPWHY("copyToScanout: source geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    IOReturn ret = a->askFramebufferForScanout(&q);
    if (ret != kIOReturnSuccess || !q.kva || !q.pitch) {
        NVFLIPWHY("copyToScanout: framebuffer would not describe its scanout (0x%x kva %p pitch %u)",
                  (unsigned)ret, q.kva, q.pitch);
        return false;
    }

    const unsigned rows = (h < q.height) ? h : q.height;
    const unsigned rowb = ((pitch < q.pitch) ? pitch : q.pitch) & ~3u;
    const unsigned words = rowb / 4;
    const unsigned spr = pitch / 4, dpr = q.pitch / 4;
    const volatile unsigned int *s = (const volatile unsigned int *)vm->fKva;
    volatile unsigned int *d = (volatile unsigned int *)q.kva;
    for (unsigned y = 0; y < rows; y++) {
        const volatile unsigned int *sp = s + (unsigned long)y * spr;
        volatile unsigned int *dp = d + (unsigned long)y * dpr;
        for (unsigned x = 0; x < words; x++) dp[x] = sp[x];
    }
    nvScanoutCopies++;
    if (nvScanoutCopies <= 3 || nvScanoutCopies == NVSCANOUT_COPY_MAX)
        ALOG("*** COPIED THE COMPOSITED FRAME ONTO THE PANEL *** #%u: %ux%u, %u rows x %u bytes, "
             "src kva %p pitch %u -> scanout kva %p phys 0x%llx pitch %u%s",
             nvScanoutCopies, w, h, rows, rowb, vm->fKva, pitch, q.kva,
             (unsigned long long)q.phys, q.pitch,
             nvScanoutCopies == NVSCANOUT_COPY_MAX ? "  [CAP REACHED -- stopping, panel keeps this frame]" : "");
    return true;
}

// CPU view of a resource: its own VRAM mapping, or for the framebuffer resource (scanout vidmem, no kva of its own) the
// framebuffer's scanout mapping. Geometry from the resource (+0xb0 w, +0xb2 h, +0xb8 pitch).
static volatile unsigned char *nvAccelResCpu(IOAccelResource2 *res, unsigned *w, unsigned *h, unsigned *pitch)
{
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm) return nullptr;
    const unsigned char *r = (const unsigned char *)res;
    *w = *(const volatile unsigned short *)(r + 0xb0); *h = *(const volatile unsigned short *)(r + 0xb2);
    *pitch = *(const volatile unsigned int *)(r + 0xb8);
    if (vm->fKva) return (volatile unsigned char *)vm->fKva;
    if (!vm->fScanout || !NVAccel::gAccel) return nullptr;
    struct NVRMVramRequest q;
    for (unsigned i = 0; i < sizeof q; i++) ((volatile unsigned char *)&q)[i] = 0;
    q.version = NVRM_VRAM_ABI_VERSION;
    if (NVAccel::gAccel->askFramebufferForScanout(&q) != kIOReturnSuccess || !q.kva) return nullptr;
    *pitch = q.pitch;
    return (volatile unsigned char *)q.kva;
}

// Copy rectangles (IOAccelBounds: int16 x, y, w, h) from src to dst with the CPU, clipped to both surfaces.
static unsigned nvCopyRectsLogged = 0;
bool nvAccelCopyRects(IOAccelResource2 *dst, IOAccelResource2 *src, const void *rects, unsigned n)
{
    unsigned dw, dh, dp, sw, sh, sp;
    volatile unsigned char *d = dst ? nvAccelResCpu(dst, &dw, &dh, &dp) : nullptr;
    volatile unsigned char *s = src ? nvAccelResCpu(src, &sw, &sh, &sp) : nullptr;
    if (!d || !s || !rects || dp < 4 || sp < 4) {
        if (nvCopyRectsLogged < 10) { nvCopyRectsLogged++;
            ALOG("copyForward: no CPU view (dst %p src %p rects %p n %u)", d, s, rects, n); }
        return false;
    }
#ifdef NMX_ACCEL
    nvCopyForwards++;
#endif
    if (d == s) return true;
    const short *b = (const short *)rects;
    const unsigned W = dw < sw ? dw : sw, H = dh < sh ? dh : sh;
    for (unsigned i = 0; i < n; i++, b += 4) {
        int x = b[0], y = b[1], w = b[2], h = b[3];
        if (x < 0) { w += x; x = 0; } if (y < 0) { h += y; y = 0; }
        if (w <= 0 || h <= 0 || (unsigned)x >= W || (unsigned)y >= H) continue;
        if ((unsigned)(x + w) > W) w = (int)W - x;
        if ((unsigned)(y + h) > H) h = (int)H - y;
        for (int row = 0; row < h; row++) {
            volatile unsigned int *dr = (volatile unsigned int *)(d + (unsigned long)(y + row) * dp) + x;
            const volatile unsigned int *srw = (const volatile unsigned int *)(s + (unsigned long)(y + row) * sp) + x;
            for (int k = 0; k < w; k++) dr[k] = srw[k];
        }
    }
    if (nvCopyRectsLogged < 10) { nvCopyRectsLogged++;
        ALOG("copyForward: %u rect(s) %ux%u pitch %u -> %ux%u pitch %u", n, sw, sh, sp, dw, dh, dp); }
    return true;
}

static bool nvAccelFlipTo(IOAccelResource2 *res)
{
    if (!res) { NVFLIPWHY("nvAccelFlipTo: no resource"); return false; }
    NVAccel *a = NVAccel::gAccel;
    if (!a || !a->fFB) { NVFLIPWHY("nvAccelFlipTo: accel %p fFB %p", a, a ? a->fFB : nullptr); return false; }
    if (gNvHeadsPiped & (gNvHeadsPiped - 1)) { NVFLIPWHY("nvAccelFlipTo: refused -- heads piped 0x%x, no way to tell whose surface this is", gNvHeadsPiped); return false; }
    NVVidMemory *vm = nvres_vidmem(res);
    if (!vm || !vm->fRmHandle) {
        NVFLIPWHY("nvAccelFlipTo: vm %p fRmHandle %p -- the composited surface has no RM memory to flip to",
                  vm, vm ? vm->fRmHandle : nullptr);
        return false;
    }
    if (vm->fScanout) { NVFLIPWHY("nvAccelFlipTo: already the scanout"); return false; }

    const unsigned char *r = (const unsigned char *)res;
    const unsigned w     = *(const volatile unsigned short *)(r + 0xb0);
    const unsigned h     = *(const volatile unsigned short *)(r + 0xb2);
    const unsigned pitch = *(const volatile unsigned int   *)(r + 0xb8);
    if (!w || !h || !pitch) { NVFLIPWHY("nvAccelFlipTo: geometry %ux%u pitch %u", w, h, pitch); return false; }

    struct NVRMFlipRequest f;
    volatile unsigned char *z = (volatile unsigned char *)&f;
    for (unsigned i = 0; i < sizeof f; i++) z[i] = 0;
    f.version = NVRM_FLIP_ABI_VERSION;
    f.kapiMemory = vm->fRmHandle; f.allocationCookie = vm->fAllocationCookie;
    f.width = w; f.height = h; f.pitch = pitch;

    const OSSymbol *sym = OSSymbol::withCStringNoCopy(NVRM_FLIP_FN);
    if (!sym) { NVFLIPWHY("nvAccelFlipTo: OSSymbol allocation failed"); return false; }
    IOReturn rr = a->fFB->callPlatformFunction(sym, false, &f, nullptr, nullptr, nullptr);
    sym->release();
    if (rr != kIOReturnSuccess) {
        NVFLIPWHY("nvAccelFlipTo: framebuffer refused the flip (0x%x, flipResult %d) for %ux%u pitch %u kapiMemory %p",
                  (unsigned)rr, f.flipResult, w, h, pitch, vm->fRmHandle);
        return false;
    }
    NVFLIPWHY("nvAccelFlipTo: *** FLIPPED *** to %ux%u pitch %u kapiMemory %p (flipResult %d)",
              w, h, pitch, vm->fRmHandle, f.flipResult);
    nvAccelSampleFront(vm, w, h, pitch);
    return true;
}

IOReturn NVAccel::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4)
{
    if (fn && p1 && fn->isEqualTo(NVACCEL_SS_FN))
        return nvAccelSurfVram((struct NVRMSurfShareEsc *)p1, (task_t)p2, (unsigned)(uintptr_t)p3);
#ifdef NMX_ACCEL
    if (fn && p1 && fn->isEqualTo("nmxSurfVram")) return nmxSurfVram((NMXSurfVramReq *)p1, false);
    if (fn && p1 && fn->isEqualTo("nmxSurfDirty")) return nmxSurfVram((NMXSurfVramReq *)p1, true);
#endif
    return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}

#ifdef NMX_ACCEL
/*
 * Frame path stages 2 and 3 on the B580 (kernel/IntelXe, kernel/IntelXeFB; plan in docs/design/framepath.md).
 *
 * Stage 2, WindowServer stops waiting for the GPU. The Metal plugin marks each surface a WindowServer command buffer
 * renders as pending when it commits and ready when the GPU work AND its copy-out to the IOSurface's pages are done
 * (nvmtl_ws_mark_pending / nvmtl_ws_mark_ready, the "late" ordering NMXAccel gets because it does not publish
 * NVAccelSurfaceWait as NVAccel). With the marks taken, commitAndWaitUntilSubmitted only waits until scheduled, and both
 * per-frame waits on WindowServer's main thread go (the composite, and CoreDisplay's DisplaySurface::AccessComplete).
 * The present then runs here on a thread call: wait for the frame's mark (nvSurfWaitReady, at most 100 ms, after which
 * the frame is presented anyway and counted), copy or flip it, and only then complete the transaction
 * (isTransactionComplete stays false while the worker is busy; signalTransactionInterrupt makes the family ask again),
 * which is the shape nullmoth's NVIDIA pipe already uses for its flips (nvAccelIopAsyncRun).
 *   Why the plugin's mark and not the GPU's own fence (an xe syncobj point or the exec's seqno): the copy path reads
 * the IOSurface's system memory, which the plugin's surface-out fills with a second GPU job that is only submitted
 * after the composite's fence has signalled; no fence known at commit covers it. The mark is released after it.
 *   Transport: the plugin cannot reach NVK's escape on Intel (ANV exports no nvmtl_nvk_surface_dirty), so it opens
 * this accelerator's own user client (IOAccelNMXPresent; the IOAccel prefix is what WindowServer's sandbox lets it
 * open) and sends the same mark nvSurfGpuEscape takes.
 *
 * Stage 3, flips instead of the CPU copy. For a surface WindowServer presents at the mode's size, the plugin also copies
 * each frame (GPU, VRAM to VRAM) into a linear, uncompressed VRAM buffer of the mode's pitch and binds it here:
 * IntelXe.kext pins the VRAM behind the buffer's GPU address ("intelxeScanoutPin"; one contiguous DPA run) and IntelXeFB maps it
 * at a GGTT scan-out slot ("nmxScanout" map). A present of a bound surface then writes back the GPU's L2
 * ("intelxeL2Flush": Linux flushes before every flip, xe_device_td_flush, a full L2 flush on BMG for Wa_16023588340) and
 * arms PLANE_SURF with the slot; IntelXeFB calls nmxFlipDone from its vblank-locked VBL callback when PLANE_SURFLIVE shows
 * it, and only then does the transaction complete, so CoreDisplay reuses the previous surface once it is off the panel.
 * Anything else (no binding, another size, refused) takes the copy into the firmware's surface, and the plane goes back
 * to that surface at the next vblank (op home), so the copy itself never tears a frame on a slot.
 *
 * Switches (both default 0, read per frame): debug.nmxaccel_async (stage 2: marks taken, async presents; the plugin
 * notices within a second), debug.nmxaccel_flip (stage 3, needs async). Plugin side: NVMTL_WS_WAIT_COMPLETION=1 keeps
 * WindowServer's own wait, NVMTL_SCANOUT=0 never binds a scan-out buffer.
 */
#include <IOKit/IOUserClient.h>
static int gNmxFlip = 0;
static SInt64 gCntNmxFrames, gCntNmxQueued, gCntNmxDrop, gCntNmxWaitUs, gCntNmxLost, gCntNmxMarkLate;
static SInt64 gCntNmxFlips, gCntNmxFlipUnbound, gCntNmxFlipRefused, gCntNmxFlipLate, gCntNmxFlipAbort, gCntNmxL2Fail, gCntNmxHome;
static SInt64 gCntNmxBinds, gCntNmxUnbinds, gCntNmxBindRefused;
static volatile SInt32 gNmxEverFlipped;
static unsigned gNmxLogs;
#define NMXLOG(fmt, ...) do { if (gNmxLogs < 60) { gNmxLogs++; ALOG("nmx: " fmt, ##__VA_ARGS__); } } while (0)

// The layouts shared with IntelXeFB (fb/IntelXeFB.cpp) and IntelXe.kext (IntelXeClient.cpp)
struct NMXScanout {
    unsigned int version;   // 1
    unsigned int op;        // 0 map, 1 unmap, 2 flip, 3 home, 4 query
    unsigned int ggtt, gen;
    unsigned int width, height, pitch, force;
    unsigned long long dpa, size;
    void (*done)(void *ctx, unsigned long long cookie, int status);
    void *ctx;
    unsigned long long cookie;
    unsigned int immediate, armed, live, pad;
    // version 2 (IntelXeFB publishes IntelXeFBScanoutVersion >= 2): op 2 carries the frame-length window CoreDisplay asked
    // for under Adaptive-Sync, in lines past the panel's minimum frame (flags bit 0 = valid)
    unsigned int vrrFlags, vrrMinExt, vrrMaxExt, pad2;
    // version 3 (IntelXeFBScanoutVersion >= 3): op 0 maps the buffer from DPA runs instead of one contiguous dpa
    const struct IntelXeDpaRun *runs;
    unsigned int nRuns, pad3;
    // version 4 (IntelXeFBScanoutVersion >= 4): op 4 also answers the time since the pipe's last start of vblank (ns, ~0
    // when not live) and the frame length (ns): when a frame was due (dispboost, nmx_vbldue.h)
    unsigned long long vblAgoNs, frameNs;
};
// IntelXe.kext "intelxeScanoutPin": version 1 pins one contiguous DPA run (dpa); version 2 (as xe's __xe_pin_fb_vma_ggtt)
// pins any VRAM BO and returns its DPA runs (4 KB aligned, merged, in address order); E2BIG: nRuns is the count
// needed and nothing is pinned. Older kernels refuse version 2 (kIOReturnBadArgument): version 1 then.
struct IntelXeDpaRun { uint64_t dpa, len; };
struct IntelXeScanoutPin {
    uint32_t version;   // 1 or 2
    uint32_t status;
    void *task;
    uint64_t va, size, dpa;
    void *pin;
    uint32_t maxRuns, nRuns;   // version 2
    IntelXeDpaRun *runs;
};
// The scan-out request version the framebuffer understands: 2 from IntelXeFB v4 on (property IntelXeFBScanoutVersion), else 1.
static unsigned nmxFbScanoutVersion(IOService *fb)
{
    static IOService *cachedFb; static unsigned cachedV;
    if (fb == cachedFb && cachedV) return cachedV;
    unsigned v = 1;
    if (OSObject *o = fb ? fb->copyProperty("IntelXeFBScanoutVersion") : nullptr) {
        if (OSNumber *n = OSDynamicCast(OSNumber, o)) { v = n->unsigned32BitValue(); if (v < 1) v = 1; if (v > 4) v = 4; }
        o->release();
    }
    cachedFb = fb; cachedV = v;
    return v;
}
static IOReturn nmxFbScanout(IOService *fb, NMXScanout *r)
{
    static const OSSymbol *sym;
    if (!sym) sym = OSSymbol::withCStringNoCopy("nmxScanout");
    r->version = nmxFbScanoutVersion(fb);
    return (fb && sym) ? fb->callPlatformFunction(sym, false, r, nullptr, nullptr, nullptr) : kIOReturnNotAttached;
}
static IOService *nmxIntelXe()
{
    static IOService *b;
    if (b && !b->isInactive()) return b;
    if (OSDictionary *m = IOService::serviceMatching("IntelXe")) {
        IOService *n = IOService::copyMatchingService(m);
        m->release();
        if (n && !OSCompareAndSwapPtr(nullptr, n, (void *volatile *)&b)) n->release();
    }
    return b;
}
static IOReturn nmxIntelXeCall(const char *fn, void *p1)
{
    IOService *b = nmxIntelXe();
    const OSSymbol *sym = OSSymbol::withCString(fn);
    IOReturn r = (b && sym) ? b->callPlatformFunction(sym, false, p1, nullptr, nullptr, nullptr) : kIOReturnNotAttached;
    if (sym) sym->release();
    return r;
}

// ─── scan-out bindings: surface ID -> GGTT slot (stage 3) ──────────────────────────────────────────────────────────
enum { kNmxScOk = 0, kNmxScOff = 1, kNmxScNotPresented = 2, kNmxScGeometry = 3, kNmxScBusy = 4, kNmxScPin = 5,
       kNmxScNoSlot = 6, kNmxScNotFound = 7, kNmxScBad = 8, kNmxScNotWS = 9 };
struct NMXScanBind { unsigned sid; uint32_t ggtt, gen, w, h, pitch; void *pin; void *owner; bool newer, readback; };   // newer: phase 4 (no readback); readback: op 3, never on the display
static NMXScanBind gNmxScan[8];
static volatile SInt32 gNmxPinV1;   // IntelXe.kext answered that it has no pin version 2
static SInt64 gCntNmxPinRuns;      // binds whose VRAM came in more than one DPA run
static IOLock *gNmxScanLock;
static unsigned gNmxPresented[8];
static volatile UInt32 gNmxPresentedN;
static void nmxNotePresented(unsigned sid)
{
    for (unsigned i = 0; i < 8; i++) if (gNmxPresented[i] == sid) return;
    gNmxPresented[OSIncrementAtomic((volatile SInt32 *)&gNmxPresentedN) & 7] = sid;
}
static bool nmxWasPresented(unsigned sid)
{
    for (unsigned i = 0; i < 8; i++) if (gNmxPresented[i] == sid) return true;
    return false;
}
static IOLock *nmxScanLock()
{
    if (!gNmxScanLock) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxScanLock)) IOLockFree(l); }
    return gNmxScanLock;
}
static bool nmxScanLookup(unsigned sid, NMXScanBind *out)
{
    IOLock *l = nmxScanLock();
    if (!l) return false;
    bool hit = false;
    IOLockLock(l);
    for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid) { *out = gNmxScan[i]; hit = true; break; }
    IOLockUnlock(l);
    return hit;
}
// ─── gpuperf phase 4: WindowServer composites without the system-memory readback ────────────────────────────────────
// The plugin skips the copy of a bound composite into its IOSurface's pages and marks the binding "newer" (selector 2):
// the pages are then stale until something reads them with the CPU. A CPU lock reaches NVResource::pageoff (the family
// valid bits say the device copy is newer, set with the mark), and a frame copied instead of flipped reaches nmxPresent's
// copy path; both page the composite off from its scan-out buffer here, on B580's copy engine, after the composite's mark.
// 1 paged off, 0 nothing to do (not bound or not newer), -1 failed (pages stay stale; counted). tryLock: from the family
// lock (pageoff) the scan lock is only tried (a scan op may hold it while it waits for the framebuffer's gate).
static SInt64 gCntNmxP4Mark, gCntNmxP4MarkNoRes, gCntNmxP4Pageoff, gCntNmxP4Fail, gCntNmxP4Busy;
static int nmxScanPageoff(unsigned sid, IOMemoryDescriptor *md, bool tryLock)
{
    IOLock *l = nmxScanLock();
    if (!l || !sid || !md) return 0;
    if (tryLock) {
        unsigned tries = 0;
        while (!IOLockTryLock(l)) { if (++tries > 20) { OSAddAtomic64(1, &gCntNmxP4Busy); return -1; } IOSleep(1); }
    } else IOLockLock(l);
    void *pin = nullptr; uint64_t len = 0;
    for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid && gNmxScan[i].newer && gNmxScan[i].pin) {
        pin = gNmxScan[i].pin; len = (uint64_t)gNmxScan[i].pitch * gNmxScan[i].h; break; }
    if (pin) nmxIntelXeCall("intelxePinGet", pin);
    IOLockUnlock(l);
    if (!pin) return 0;
    nvSurfWaitReady(sid);
    len = (len + 0xfff) & ~0xfffull;
    if (len > md->getLength()) len = md->getLength() & ~0xfffull;
    IntelXePinCopy q; bzero(&q, sizeof q);
    q.version = INTELXE_SV_KABI_VERSION; q.timeoutMs = 2000; q.pin = pin; q.md = md; q.len = len; q.err = -1;
    const IOReturn r = nmxIntelXeCall("intelxeCopyPinToPages", &q);
    nmxIntelXeCall("intelxePinPut", pin);
    const bool ok = r == kIOReturnSuccess && q.err == 0;
    if (ok) {
        IOLockLock(l);
        for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid && gNmxScan[i].pin == pin) gNmxScan[i].newer = false;
        IOLockUnlock(l);
    }
    const SInt64 n = OSAddAtomic64(1, ok ? &gCntNmxP4Pageoff : &gCntNmxP4Fail);
    if (n < 8 || !ok) ALOG("nmx: phase 4: composite 0x%x paged off from its scan-out buffer: %s (%llu KB, %llu us, err %d)", sid, ok ? "ok" : "FAILED",
                           len >> 10, q.usec, (int)q.err);
    return ok ? 1 : -1;
}
// the family's per-plane valid bits for plane 0: the device copy is newer, so a CPU lock calls NVResource::pageoff
static bool nmxSetDeviceNewer(void *cache, NVResource *res)
{
    volatile unsigned char *r = (volatile unsigned char *)res;
    volatile unsigned short *valid = *(unsigned short *volatile *)(r + 0x30);
    const unsigned fl = *(volatile unsigned int *)(r + 0xc);
    if (r[0x14] != 0xc0 || (fl & 0x8000) || !valid) return false;
    _ZN20IOSurfaceDeviceCache10setCurrentEv(cache);
    valid[6] |= 1;
    valid[0] &= (unsigned short)~1u;
    return true;
}
// Caller holds the scan lock. force: the owner is gone, so the plane leaves the slot first (IntelXeFB homes it).
static unsigned nmxScanDropLocked(unsigned i, bool force)
{
    NMXScanBind &b = gNmxScan[i];
    NVAccel *a = NVAccel::gAccel;
    NMXScanout r; bzero(&r, sizeof r);
    r.op = 1; r.ggtt = b.ggtt; r.gen = b.gen; r.force = force;
    // a readback binding (op 3) was never given to the framebuffer: nothing to unmap
    IOReturn rr = b.readback ? kIOReturnSuccess : a ? nmxFbScanout(a->fFB, &r) : kIOReturnNotAttached;
    if (rr == kIOReturnBusy) return kNmxScBusy;
    // unmapped (or the framebuffer is gone, or never knew the slot): the VRAM can go
    if (rr != kIOReturnSuccess && rr != kIOReturnNotFound) NMXLOG("scan-out slot 0x%x of surface 0x%x: unmap answered 0x%x -- released anyway", b.ggtt, b.sid, (unsigned)rr);
    if (b.pin) nmxIntelXeCall("intelxeScanoutUnpin", b.pin);
    OSAddAtomic64(1, &gCntNmxUnbinds);
    bzero(&b, sizeof b);
    return kNmxScOk;
}
static void nmxScanUnbindOwner(void *owner)
{
    IOLock *l = nmxScanLock();
    if (!l) return;
    IOLockLock(l);
    for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid && gNmxScan[i].owner == owner) nmxScanDropLocked(i, true);
    IOLockUnlock(l);
}
// op 0 query (va 0: may this surface be bound?), 1 bind, 2 unbind, 3 bind for readback only.
// op 3 (scaled HiDPI modes): WindowServer's composite of a scaled mode is not on the display - a second pass scales it to
// the panel - so it cannot have a scan-out binding, and every frame of it went back to its system-memory pages over PCIe
// (two 23-37 MB copies per frame: 90-145 frames/s where drawing took 0.2 ms). A readback binding pins the plugin's VRAM
// copy of the frame like a scan-out buffer, at the surface's own pitch, but never maps it for the display: phase 4 then
// skips the copy into the pages, and a CPU lock (a screenshot) pages the frame off from the pin (nmxScanPageoff).
static unsigned nmxScanOp(void *owner, task_t task, unsigned op, unsigned sid, uint64_t va, uint64_t size, unsigned w, unsigned h,
                          unsigned pitch, uint32_t *ggttOut)
{
    NVAccel *a = NVAccel::gAccel;
    IOLock *l = nmxScanLock();
    if (!a || !l || !sid) return kNmxScBad;
    if (op == 2) {
        unsigned st = kNmxScNotFound;
        IOLockLock(l);
        for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid && gNmxScan[i].owner == owner) { st = nmxScanDropLocked(i, false); break; }
        IOLockUnlock(l);
        return st;
    }
    const bool readback = op == 3;
    if (readback) {
        if (!gSurfGpuOn) return kNmxScOff;
        if (!w || !h || w > 16384 || h > 16384 || pitch < w * 4 || (pitch & 3) || size < (uint64_t)pitch * h) return kNmxScGeometry;
    } else {
    if (!gNmxFlip || !gSurfGpuOn) return kNmxScOff;
    if (!nmxWasPresented(sid)) return kNmxScNotPresented;
    struct NVRMVramRequest q;
    bzero(&q, sizeof q);
    q.version = NVRM_VRAM_ABI_VERSION;
    if (a->askFramebufferForScanout(&q, a->fFB) != kIOReturnSuccess || w != q.width || h != q.height || pitch != q.pitch ||
        size < (uint64_t)pitch * h)
        return kNmxScGeometry;
    }
    if (op == 0) {   // the answer names the slot when the surface is bound already
        IOLockLock(l);
        for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid && ggttOut) *ggttOut = gNmxScan[i].ggtt;
        IOLockUnlock(l);
        return kNmxScOk;
    }
    if ((op != 1 && op != 3) || !va) return kNmxScBad;
    IOLockLock(l);
    unsigned slot = 8;
    for (unsigned i = 0; i < 8; i++) {
        if (gNmxScan[i].sid == sid) {   // a new buffer for a bound surface: the old binding goes first
            if (nmxScanDropLocked(i, false) != kNmxScOk) { IOLockUnlock(l); return kNmxScBusy; }
        }
        if (!gNmxScan[i].sid && slot == 8) slot = i;
    }
    if (slot == 8) { IOLockUnlock(l); OSAddAtomic64(1, &gCntNmxBindRefused); return kNmxScNoSlot; }
    // Pin: version 2 with DPA runs when both IntelXe.kext and the framebuffer (scan-out version 3) take them, so the BO
    // need not be one contiguous run; otherwise version 1 (one run).
    IntelXeScanoutPin pin; bzero(&pin, sizeof pin);
    IntelXeDpaRun *runs = nullptr; uint32_t runsCap = 0;
    IOReturn pr = kIOReturnUnsupported;
    if (nmxFbScanoutVersion(a->fFB) >= 3 && !gNmxPinV1) {
        runsCap = 64;
        for (int tries = 0; tries < 2; tries++) {
            runs = (IntelXeDpaRun *)IOMalloc(runsCap * sizeof(IntelXeDpaRun));
            if (!runs) { pr = kIOReturnNoMemory; break; }
            bzero(&pin, sizeof pin);
            pin.version = 2; pin.task = task; pin.va = va; pin.size = size; pin.maxRuns = runsCap; pin.runs = runs;
            pr = nmxIntelXeCall("intelxeScanoutPin", &pin);
            if (pr == kIOReturnSuccess || pin.status != 7 /* E2BIG */ || pin.nRuns <= runsCap || pin.nRuns > 4096) break;
            IOFree(runs, runsCap * sizeof(IntelXeDpaRun)); runs = nullptr;
            runsCap = pin.nRuns;   // the count IntelXe.kext needs; nothing was pinned
        }
        if (pr == kIOReturnBadArgument && pin.status == 0) {   // an older kernel: no version 2
            gNmxPinV1 = 1;
            ALOG("nmx: IntelXe.kext has no scan-out pin v2 -- contiguous pins (v1) from now on");
        }
        if (pr != kIOReturnSuccess && runs) { IOFree(runs, runsCap * sizeof(IntelXeDpaRun)); runs = nullptr; }
    }
    if (pr != kIOReturnSuccess && !runs) {
        bzero(&pin, sizeof pin);
        pin.version = 1; pin.task = task; pin.va = va; pin.size = size;
        pr = nmxIntelXeCall("intelxeScanoutPin", &pin);
    }
    if (pr != kIOReturnSuccess || !pin.pin) {
        IOLockUnlock(l);
        if (runs) IOFree(runs, runsCap * sizeof(IntelXeDpaRun));
        OSAddAtomic64(1, &gCntNmxBindRefused);
        NMXLOG("scan-out bind of surface 0x%x: IntelXe would not pin va 0x%llx +%llu (0x%x, errno %u) -- it stays on the copy path", sid, va, size, (unsigned)pr, pin.status);
        return kNmxScPin;
    }
    NMXScanout r; bzero(&r, sizeof r);
    r.op = 0; r.dpa = runs && pin.nRuns ? runs[0].dpa : pin.dpa; r.size = size; r.width = w; r.height = h; r.pitch = pitch;
    if (runs && pin.nRuns) { r.runs = runs; r.nRuns = pin.nRuns; }
    IOReturn mr = readback ? kIOReturnSuccess : nmxFbScanout(a->fFB, &r);   // IntelXeFB copies the runs it keeps
    if (runs) { if (pin.nRuns > 1) OSAddAtomic64(1, &gCntNmxPinRuns); IOFree(runs, runsCap * sizeof(IntelXeDpaRun)); runs = nullptr; }
    if (mr != kIOReturnSuccess) {
        nmxIntelXeCall("intelxeScanoutUnpin", pin.pin);
        IOLockUnlock(l);
        OSAddAtomic64(1, &gCntNmxBindRefused);
        NMXLOG("scan-out bind of surface 0x%x: the framebuffer would not map VRAM 0x%llx +%llu (0x%x)", sid, pin.dpa, size, (unsigned)mr);
        return mr == kIOReturnNoResources ? kNmxScNoSlot : kNmxScGeometry;
    }
    NMXScanBind &b = gNmxScan[slot];
    b.sid = sid; b.ggtt = readback ? 0 : r.ggtt; b.gen = readback ? 0 : r.gen; b.w = w; b.h = h; b.pitch = pitch; b.pin = pin.pin; b.owner = owner;
    b.readback = readback;
    IOLockUnlock(l);
    const SInt64 n = OSAddAtomic64(1, &gCntNmxBinds);
    if (n < 16) ALOG("nmx: surface 0x%x %ux%u pitch %u: %s at VRAM 0x%llx, GGTT 0x%x", sid, w, h, pitch, readback ? "readback buffer (not displayed)" : "scan-out buffer", pin.dpa, b.ggtt);
    if (ggttOut) *ggttOut = r.ggtt;
    return kNmxScOk;
}

// ─── the plugin's user client: GPU marks (stage 2) and scan-out bindings (stage 3) ─────────────────────────────────
class IOAccelNMXPresent : public IOUserClient {
    OSDeclareDefaultStructors(IOAccelNMXPresent)
public:
    task_t fTask = nullptr;
    bool initWithTask(task_t owningTask, void *securityID, UInt32 type) APPLE_KEXT_OVERRIDE {
        fTask = owningTask;
        return IOUserClient::initWithTask(owningTask, securityID, type);
    }
    IOReturn clientClose() APPLE_KEXT_OVERRIDE {   // IOUserClient::clientDied comes here too
        nmxScanUnbindOwner(this);
        terminate();
        return kIOReturnSuccess;
    }
    IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(IOAccelNMXPresent, IOUserClient)
// Only WindowServer's marks are taken (as nvSurfGpuEscape): no other process can hold a present.
static bool nmxCallerIsWS()
{
    char nm[32] = { 0 };
    proc_selfname(nm, sizeof nm);
    return !strcmp(nm, "WindowServer");
}
// selector 0: in {surfaceID, pending} -> out {status}: 0 taken (surface 0 is the plugin's probe), NVRM_SS_OFF otherwise
// selector 1: in {op, surfaceID, va, size, width | height << 32, pitch} -> out {status (kNmxSc*), ggtt}; op 3: readback only
IOReturn IOAccelNMXPresent::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *, OSObject *, void *)
{
    if (selector == 0) {
        if (args->scalarInputCount < 2 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
        unsigned st = NVRM_SS_OFF;
        const unsigned sid = (unsigned)args->scalarInput[0];
        const bool pending = args->scalarInput[1] != 0;
        if (nmxCallerIsWS()) {
            // a ready mark is always taken: one dropped while stage 2 was off would hold that surface's next present
            // for the full 100 ms once it is back on
            if (gSurfGpuOn || (sid && !pending)) { if (sid) nvSurfMark(sid, pending); }
            if (gSurfGpuOn) st = 0;
        }
        args->scalarOutput[0] = st;
        args->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }
    if (selector == 1) {
        if (args->scalarInputCount < 6 || args->scalarOutputCount < 2) return kIOReturnBadArgument;
        uint32_t ggtt = 0;
        unsigned st = nmxCallerIsWS()
            ? nmxScanOp(this, fTask, (unsigned)args->scalarInput[0], (unsigned)args->scalarInput[1], args->scalarInput[2], args->scalarInput[3],
                        (unsigned)args->scalarInput[4], (unsigned)(args->scalarInput[4] >> 32), (unsigned)args->scalarInput[5], &ggtt)
            : kNmxScNotWS;
        args->scalarOutput[0] = st;
        args->scalarOutput[1] = ggtt;
        args->scalarOutputCount = 2;
        return kIOReturnSuccess;
    }
    // selector 2 (gpuperf phase 4): in {surfaceID, newer} -> out {status}. newer 1: the plugin skipped this frame's readback
    // of a bound composite; the binding is marked and the family's valid bits say the device copy is newer (status 0), or
    // kNmxScNotFound (not bound by this client) / kNmxScBad (no family resource: a CPU lock would not page it off) - the
    // plugin then reads back after all. newer 0: page it off now if it is marked (before an unbind).
    if (selector == 2) {
        if (args->scalarInputCount < 2 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
        const unsigned sid = (unsigned)args->scalarInput[0];
        const bool newer = args->scalarInput[1] != 0;
        unsigned st = kNmxScNotWS;
        NVAccel *a = NVAccel::gAccel;
        if (nmxCallerIsWS() && a && sid) {
            void *root = nmxSVRoot();
            void *surf = root ? _ZN13IOSurfaceRoot13lookupSurfaceEjP4task(root, sid, fTask) : nullptr;
            if (!surf) st = kNmxScNotFound;
            else if (newer) {
                bool bound = false;
                IOLock *l = nmxScanLock();
                if (l) { IOLockLock(l);
                    for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid && gNmxScan[i].owner == this) { gNmxScan[i].newer = true; bound = true; }
                    IOLockUnlock(l); }
                if (!bound) st = kNmxScNotFound;
                else {
                    bool marked = false;
                    if (nvFamLock(a, false)) {
                        void *cache = _ZN22IOGraphicsAccelerator223deviceCacheForIOSurfaceEP9IOSurfacej((void *)a, surf, 0);
                        OSObject *ctx = cache ? *(OSObject *volatile *)((unsigned char *)cache + 0x20) : nullptr;
                        NVResource *res = OSDynamicCast(NVResource, ctx);
                        if (cache && res) marked = nmxSetDeviceNewer(cache, res);
                        if (cache) ((OSObject *)cache)->release();
                        nvFamUnlock(a);
                    }
                    if (!marked) {   // the CPU would not reach the page-off: no newer pages without a readback
                        if (l) { IOLockLock(l); for (unsigned i = 0; i < 8; i++) if (gNmxScan[i].sid == sid) gNmxScan[i].newer = false; IOLockUnlock(l); }
                        OSAddAtomic64(1, &gCntNmxP4MarkNoRes);
                        st = kNmxScBad;
                    } else { OSAddAtomic64(1, &gCntNmxP4Mark); st = kNmxScOk; }
                }
            } else {
                IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv(surf);
                st = kNmxScOk;
                if (md && md->prepare(kIODirectionNone) == kIOReturnSuccess) {
                    if (nmxScanPageoff(sid, md, false) < 0) st = kNmxScBusy;
                    md->complete(kIODirectionNone);
                } else st = kNmxScBad;
            }
            if (surf) ((OSObject *)surf)->release();
        }
        args->scalarOutput[0] = st;
        args->scalarOutputCount = 1;
        return kIOReturnSuccess;
    }
    return kIOReturnUnsupported;
}
static IOReturn nmxNewPresentClient(IOService *accel, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
    IOAccelNMXPresent *c = OSTypeAlloc(IOAccelNMXPresent);
    if (!c || !c->initWithTask(owningTask, securityID, type) || !c->attach(accel)) { if (c) c->release(); return kIOReturnNoResources; }
    if (!c->start(accel)) { c->detach(accel); c->release(); return kIOReturnNoResources; }
    *handler = c;
    return kIOReturnSuccess;
}

// ─── IntelXe.kext's xe connection, opened on this accelerator ─────────────────────────────────────────────────────────
// Mesa ANV reaches the B580 through IOAccelIntelXeClient (kernel/IntelXe/IntelXeClient.cpp; xemac opens it). On the IntelXe service
// itself the system-graphics sandbox refuses it in every sandboxed Metal client (Safari, iconservicesagent,
// VTDecoderXPCService, ...: system.sb allows iokit-open-user-client only with (iokit-connection "IOAccelerator")), and
// the plugin then finds no Vulkan device. So the same client is handed out here: IntelXe.kext creates it for the caller and
// attaches and starts it on this accelerator ("intelxeNewClient"), so the service opened and the client's provider are
// both this IOAccelerator, like the family's own clients. Its class and code stay in IntelXe.kext (same external methods
// and memory types); it holds a reference to the IntelXe service, IntelXe.kext cannot unload while one is open, and this
// accelerator's termination terminates it as any child. An older IntelXe.kext answers kIOReturnUnsupported, and xemac
// falls back to IOServiceOpen on IntelXe.
struct IntelXeNewClient {   // IntelXeClient.cpp, same layout
    uint32_t version;    // 1
    uint32_t type;
    void *task, *securityID;
    IOService *provider;
    IOUserClient *client;
};
static IOReturn nmxNewIntelXeClient(IOService *accel, IOService *pci, task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler)
{
    IOService *b = nmxIntelXe();
    if (!b || (pci && b->getProvider() != pci)) return kIOReturnNotAttached;   // IntelXe drives another GPU (or none)
    IntelXeNewClient r = { 1, type, owningTask, securityID, accel, nullptr };
    IOReturn ret = nmxIntelXeCall("intelxeNewClient", &r);
    if (ret != kIOReturnSuccess || !r.client) {
        NMXLOG("xe client for the B580: IntelXe.kext refused (0x%x)", ret);
        return ret != kIOReturnSuccess ? ret : kIOReturnNoResources;
    }
    *handler = r.client;
    return kIOReturnSuccess;
}

// ─── the present worker (stage 2) and flips (stage 3) ──────────────────────────────────────────────────────────────
// One frame in flight per head; one more may wait behind it (next), replaced (and counted as dropped) by a newer one.
struct NMXIopAsync {
    NVDisplayPipe *pipe; IOService *fb; unsigned head;
    volatile SInt32 busy;
    struct thread_call *call;
    IOSurface *s, *next;            // the frame the worker takes, and the one queued behind the frame in flight
    unsigned long long cookie;      // the flip/home in flight; a completion with another cookie is stale
    bool awaitFlip; int flipKind;   // waiting for IntelXeFB's completion; 2 = flip to a slot, 3 = home after a copy
    bool finishPending;             // that completion came: the worker finishes the frame (never on IntelXeFB's VBL thread)
    uint64_t t0;
};
static NMXIopAsync gNmxAs[kNvMaxFB];
static IOLock *gNmxAsLock, *gNmxCopyMtx;
static uint64_t gNmx500ms;
static bool nmxIopAsyncOn() { return gSurfGpuOn != 0; }
static bool nmxIopAsyncBusy(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gNmxAs[h].pipe == pipe && __atomic_load_n(&gNmxAs[h].busy, __ATOMIC_ACQUIRE)) return true;
    return false;
}
static bool nmxIopAsyncQueued(const void *pipe)
{
    for (unsigned h = 0; h < kNvMaxFB; h++)
        if (gNmxAs[h].pipe == pipe && __atomic_load_n(&gNmxAs[h].busy, __ATOMIC_ACQUIRE) && gNmxAs[h].next) return true;
    return false;
}
// The worker and a synchronous present (stage 2 switched off while a frame is in flight) never copy at once: both use
// the pipe's surface mappings and the firmware's surface.
static void nmxCopyLock(bool lock)
{
    if (!gNmxCopyMtx) { IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxCopyMtx)) IOLockFree(l); }
    if (!gNmxCopyMtx) return;
    if (lock) IOLockLock(gNmxCopyMtx); else IOLockUnlock(gNmxCopyMtx);
}
// A synchronous copy went into the firmware's surface: if an earlier flip left the plane on a slot, it goes back (armed
// for the next vblank; nobody waits).
static void nmxHomeAfterCopy(IOService *fb, unsigned head)
{
    if (!gNmxEverFlipped || nmxHeadSkips(head, kNmxCapFlip)) return;   // a head that never flips is always home
    NMXScanout r; bzero(&r, sizeof r);
    r.op = 3;
    (void)nmxFbScanout(fb, &r);
}
// The frame in flight is done: the queued one (if any) becomes the worker's, else the worker is free; then the family
// asks isTransactionComplete again. Returns true when a queued frame was handed over and inWorker (run it now).
static bool nmxFinish(NMXIopAsync *e, bool inWorker)
{
    IOLockLock(gNmxAsLock);
    IOSurface *q = e->next;
    e->next = nullptr;
    if (q) { e->s = q; e->t0 = mach_absolute_time(); }
    else { __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE); IOLockWakeup(gNmxAsLock, (event_t)&e->busy, false); }
    IOLockUnlock(gNmxAsLock);
    OSAddAtomic64(1, &gCntNmxFrames);
    _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(e->pipe, nullptr);
    if (q && !inWorker) thread_call_enter(e->call);
    return q != nullptr && inWorker;
}
// IntelXeFB, from its VBL callback (or its stop): the flip/home armed with this cookie is on the panel (0), late (-1:
// 100 ms without PLANE_SURFLIVE showing it), abandoned for a mode change or unmap (-2), or the framebuffer stopped (-3).
// The frame is finished on the worker: signalTransactionInterrupt may present the next frame from inside the family,
// and that present may wait up to a refresh for this one, which must not happen on the thread that times vblanks.
static void nmxFlipDone(void *ctx, unsigned long long cookie, int status)
{
    NMXIopAsync *e = (NMXIopAsync *)ctx;
    IOLockLock(gNmxAsLock);
    const bool mine = e->awaitFlip && cookie == e->cookie;
    const int kind = e->flipKind;
    if (mine) { e->awaitFlip = false; e->finishPending = true; }
    IOLockUnlock(gNmxAsLock);
    if (!mine) return;   // completed by the lost-completion guard already
    if (status == 0 && kind == 2) {
        const SInt64 n = OSAddAtomic64(1, &gCntNmxFlips);
        OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[e->head]);
        if (n < 3 || n == 600) ALOG("nmx: flip #%lld on the panel (head %u)", (long long)n + 1, e->head);
    } else if (status == 0) OSAddAtomic64(1, &gCntNmxHome);
    else if (status == -1) OSAddAtomic64(1, &gCntNmxFlipLate);
    else OSAddAtomic64(1, &gCntNmxFlipAbort);
    thread_call_enter(e->call);
}
// Arms a flip (op 2) or home (op 3) with nmxFlipDone as its completion: 1 = armed, the completion finishes the frame;
// 0 = armed with nothing to wait for (display asleep, or already home); -1 = refused.
static int nmxArm(NMXIopAsync *e, NMXScanout *r)
{
    r->done = nmxFlipDone; r->ctx = e;
    IOLockLock(gNmxAsLock);
    r->cookie = ++e->cookie; e->awaitFlip = true; e->flipKind = (int)r->op;
    IOLockUnlock(gNmxAsLock);
    IOReturn rr = nmxFbScanout(e->fb, r);
    if (rr == kIOReturnSuccess && !r->immediate) return 1;
    IOLockLock(gNmxAsLock);
    if (e->cookie == r->cookie) e->awaitFlip = false;
    IOLockUnlock(gNmxAsLock);
    if (rr == kIOReturnSuccess) return 0;
    if (r->op == 2) {
        OSAddAtomic64(1, &gCntNmxFlipRefused);
        NMXLOG("flip to GGTT 0x%x refused (0x%x) -- copying this frame", r->ggtt, (unsigned)rr);
    }
    return -1;
}
// dispboost (docs/design/DESIGN-display-boost.md, nmx_vbldue.h): when the frame the worker presents was due on the panel,
// and what happens when its GPU work is still running then. i915 does the same from the crtc's vblank queue
// (intel_display_rps_boost_after_vblank -> do_rps_boost -> intel_rps_boost); IntelXe decides what to do with it
// (debug.intelxe.disp_boost). The misses are counted here whatever IntelXe does, so the count compares boost off and on.
static SInt64 gCntNmxVblDue, gCntNmxVblNoPhase, gCntNmxVblUnknown, gCntNmxVblMiss, gCntNmxBoostFail, gCntNmxIdleBoost;
// dispboost, idle exit: the first frame after the display was quiet this long (ms; 0 = off) asks IntelXe for the boost
// at once. The GuC's SLPC takes ~370 ms to raise the clock from idle, and in that time WindowServer composites every
// other refresh without a frame ever being late at its vblank (B580: ~23 of the first ~60 frames 2 refreshes apart).
static int gNmxIdleBoostMs = 250;
static uint64_t gNmxLastPresentAbs;
// The first start of vblank after e->t0 (the present, or the landing of the frame it waited behind), mach absolute time;
// 0 = unknown. The phase from IntelXeFB's op 4 (NMXScanout 4); without it one frame after t0.
static uint64_t nmxVblDueAbs(NMXIopAsync *e)
{
    uint64_t frameNs = 0, agoNs = ~0ull, qNs = 0, t0Ns = 0;
    if (e->fb && nmxFbScanoutVersion(e->fb) >= 4) {
        NMXScanout r; bzero(&r, sizeof r);
        r.op = 4;
        if (nmxFbScanout(e->fb, &r) == kIOReturnSuccess) { frameNs = r.frameNs; agoNs = r.vblAgoNs; }
    }
    absolutetime_to_nanoseconds(mach_absolute_time(), &qNs);
    absolutetime_to_nanoseconds(e->t0, &t0Ns);
    if (!frameNs) frameNs = nmxFrameNsFromFlipWait(nvAccelFlipWaitUs(e->fb));
    const uint64_t last = nmxLastVblNs(qNs, agoNs, frameNs);
    const uint64_t dueNs = nmxVblDueNs(t0Ns, last, frameNs);
    if (!dueNs) { OSAddAtomic64(1, &gCntNmxVblUnknown); return 0; }
    OSAddAtomic64(1, last ? &gCntNmxVblDue : &gCntNmxVblNoPhase);
    uint64_t due = 0;
    nanoseconds_to_absolutetime(dueNs, &due);
    return due;
}
static void nmxVblMissed(void *ctx)
{
    NMXIopAsync *e = (NMXIopAsync *)ctx;
    const SInt64 n = OSAddAtomic64(1, &gCntNmxVblMiss);
    if (nmxIntelXeCall("intelxeDispBoost", nullptr) != kIOReturnSuccess) {
        const SInt64 f = OSAddAtomic64(1, &gCntNmxBoostFail);
        if (f < 3) ALOG("nmx: head %u: IntelXe takes no display boost (an older IntelXe.kext?)", e ? e->head : 0);
    }
    if (n < 3) ALOG("nmx: head %u: frame still on the GPU at the vblank it was due at (miss #%lld)", e ? e->head : 0, (long long)n + 1);
}
// Presents one frame on the worker. True: IntelXeFB's completion finishes it (nmxFlipDone); false: finished now.
static bool nmxPresent(NMXIopAsync *e, IOSurface *s)
{
    const unsigned sid = s->getSurfaceID();
    // dispboost: a frame still on the GPU gets a due time (the first start of vblank after the worker took it); if it is
    // still running then, the wait reports the miss to IntelXe, which may raise the GT's minimum clock for a while
    uint64_t due = 0;
    {
        const uint64_t now = mach_absolute_time(), last = gNmxLastPresentAbs;
        gNmxLastPresentAbs = now;
        uint64_t gapNs = 0;
        if (last && now > last) absolutetime_to_nanoseconds(now - last, &gapNs);
        if (gNmxIdleBoostMs > 0 && last && gapNs >= (uint64_t)gNmxIdleBoostMs * 1000000ull) {
            OSAddAtomic64(1, &gCntNmxIdleBoost);
            if (nmxIntelXeCall("intelxeDispBoost", nullptr) != kIOReturnSuccess) OSAddAtomic64(1, &gCntNmxBoostFail);
        }
    }
    if (nvSurfPendingNow(sid)) due = nmxVblDueAbs(e);
    if (!nvSurfWaitReadyDue(sid, due, nmxVblMissed, e)) {   // the composite did not finish in 100 ms: shown as it is (the plugin's GPU is hung)
        const SInt64 n = OSAddAtomic64(1, &gCntNmxMarkLate);
        if (n < 8) ALOG("nmx: surface 0x%x not finished on the GPU after 100 ms -- presented anyway (%lld so far)", sid, (long long)n + 1);
    }
    NVAccel *a = NVAccel::gAccel;
    if (!a) return false;
    // scan-out bindings are mapped by head 0's framebuffer at head 0's size: a head without flips (knob on) copies
    const bool noFlip = nmxHeadSkips(e->head, kNmxCapFlip);
    if (gNmxFlip && !noFlip) {
        NMXScanBind b;
        if (nmxScanLookup(sid, &b) && !b.readback && b.w == s->getWidth() && b.h == s->getHeight()) {
            if (nmxIntelXeCall("intelxeL2Flush", nullptr) != kIOReturnSuccess) {
                // the slot may still be partly in the GPU's L2: never scan it out unflushed
                OSAddAtomic64(1, &gCntNmxL2Fail);
                NMXLOG("L2 flush failed -- copying surface 0x%x instead of flipping", sid);
            } else {
                NMXScanout r; bzero(&r, sizeof r);
                r.op = 2; r.ggtt = b.ggtt; r.gen = b.gen;
                if (e->head < kNvMaxFB && gNmxVrr[e->head].flags) {
                    r.vrrFlags = 1; r.vrrMinExt = gNmxVrr[e->head].minExt; r.vrrMaxExt = gNmxVrr[e->head].maxExt;
                }
                const int armed = nmxArm(e, &r);
                if (armed >= 0) OSCompareAndSwap(0, 1, (volatile UInt32 *)&gNmxEverFlipped);
                if (armed > 0) return true;
                if (armed == 0) {   // display asleep: armed, counted as presented
                    OSAddAtomic64(1, &gCntNmxFlips); OSAddAtomic64(1, &gCntIopOk); OSAddAtomic64(1, &gCntIopOkHead[e->head]);
                    return false;
                }
            }
        } else OSAddAtomic64(1, &gCntNmxFlipUnbound);
    }
    {   // gpuperf phase 4: a composite whose readback was skipped is paged off from its scan-out buffer before the copy
        IOMemoryDescriptor *md = (IOMemoryDescriptor *)_ZNK9IOSurface19getMemoryDescriptorEv((void *)s);
        if (md && md->prepare(kIODirectionNone) == kIOReturnSuccess) { (void)nmxScanPageoff(sid, md, false); md->complete(kIODirectionNone); }
    }
    nmxCopyLock(true);
    (void)nvIopCopySurface(e->pipe, a, e->fb, e->head, s);
    nmxCopyLock(false);
    if (!gNmxEverFlipped || noFlip) return false;
    // the copy went into the firmware's surface, which is not on the panel while a slot is: home at the next vblank,
    // and the frame completes once it is live (it was written before the plane reads it, so it does not tear)
    NMXScanout r; bzero(&r, sizeof r);
    r.op = 3;
    return nmxArm(e, &r) > 0;
}
static void nmxIopAsyncRun(void *p0, void *)
{
    NMXIopAsync *e = (NMXIopAsync *)p0;
    for (;;) {
        IOLockLock(gNmxAsLock);
        const bool fin = e->finishPending;
        e->finishPending = false;
        IOSurface *s = fin ? nullptr : e->s;
        if (!fin) e->s = nullptr;
        IOLockUnlock(gNmxAsLock);
        if (fin) { if (!nmxFinish(e, true)) return; continue; }   // a flip landed: finish its frame, run the queued one
        if (!s) return;
        const bool later = nmxPresent(e, s);
        ((OSObject *)s)->release();
        if (later || !nmxFinish(e, true)) return;
    }
}
// Adaptive-Sync time stamps (step 5): IntelXeFB calls this from its VBL timer for every frame a "Variable" mode scans out,
// with { frame start, earliest next frame, latest next frame, 0, 0 } in mach absolute time -- what Apple's Intel driver
// hands IOAccelDisplayPipe::reportVRRStampInfo (a copy into the pipe's shared page plus an event, any context).
extern "C" void _ZN18IOAccelDisplayPipe18reportVRRStampInfoEP34IOAccelDisplayPipeVRRTimeStampInfo(void *pipe, void *info);
static SInt64 gCntNmxVrrStamps;
static bool gNmxStampReg[kNvMaxFB];
static void nmxVrrStampFn(void *ctx, const uint64_t *info, unsigned n)
{
    NMXIopAsync *e = (NMXIopAsync *)ctx;
    NVDisplayPipe *pipe = e ? e->pipe : nullptr;
    if (!pipe || !info || n < 3) return;
    uint64_t buf[8] = {};   // the family reads five words
    for (unsigned i = 0; i < n && i < 8; i++) buf[i] = info[i];
    _ZN18IOAccelDisplayPipe18reportVRRStampInfoEP34IOAccelDisplayPipeVRRTimeStampInfo(pipe, buf);
    const SInt64 c = OSAddAtomic64(1, &gCntNmxVrrStamps);
    if (c < 3) ALOG("iop: adaptive sync: time stamp #%lld head %u (next frame in %llu..%llu ticks)", (long long)c + 1, e->head,
                    (unsigned long long)(buf[1] - buf[0]), (unsigned long long)(buf[2] - buf[0]));
}
static void nmxVrrStampRegister(IOService *fb, NMXIopAsync *e, unsigned head)
{
    if (head >= kNvMaxFB || gNmxStampReg[head] || !fb) return;
    struct { unsigned version, pad; void (*fn)(void *, const uint64_t *, unsigned); void *ctx; } g = { 1, 0, nmxVrrStampFn, e };
    const OSSymbol *sym = OSSymbol::withCString("nmxVrrStamp");
    const IOReturn r = sym ? fb->callPlatformFunction(sym, false, &g, nullptr, nullptr, nullptr) : kIOReturnNoMemory;
    if (sym) sym->release();
    gNmxStampReg[head] = true;   // an older framebuffer without the call: asked once
    if (r != kIOReturnSuccess) ALOG("nmx: head %u: framebuffer takes no VRR time stamps (0x%x)", head, (unsigned)r);
}
static bool nmxIopAsyncStart(NVDisplayPipe *pipe, IOService *fb, unsigned head, IOSurface *s)
{
    if (head >= kNvMaxFB) return false;
    NMXIopAsync *e = &gNmxAs[head];
    if (!gNmxAsLock) {
        IOLock *l = IOLockAlloc(); if (l && !OSCompareAndSwapPtr(nullptr, l, (void *volatile *)&gNmxAsLock)) IOLockFree(l);
        nanoseconds_to_absolutetime(500000000ull, &gNmx500ms);
    }
    if (!gNmxAsLock) return false;
    if (!e->call) e->call = thread_call_allocate(nmxIopAsyncRun, e);
    if (!e->call) return false;
    uint64_t now = mach_absolute_time();
    bool lost = false;
    IOSurface *drop = nullptr;
    IOLockLock(gNmxAsLock);
    if (e->busy && e->awaitFlip && now - e->t0 > gNmx500ms) {
        // IntelXeFB never answered (unloaded, or its timer stopped): WindowServer must not freeze behind it
        e->awaitFlip = false; e->finishPending = false; e->cookie++;
        drop = e->next; e->next = nullptr;
        __atomic_store_n(&e->busy, 0, __ATOMIC_RELEASE);
        lost = true;
    }
    if (e->busy && gNmxDepth < 2) {
        // the previous frame is still waiting for its GPU work or its flip: give it up to a refresh and a quarter, then
        // queue this one behind it instead of holding WindowServer longer (nvAccelIopAsyncStart's bound)
        // (IntelXeFB publishes NVRMFlipWaitUs from its mode; an older one does not, and 50 ms is no bound at 144-180 Hz)
        uint64_t us = nvAccelFlipWaitUs(fb);
        if (us >= 50000) us = 8000;
        uint64_t end = 0; clock_interval_to_deadline((uint32_t)us, kMicrosecondScale, &end);
        while (e->busy && mach_absolute_time() < end) {
            uint64_t next = 0; clock_interval_to_deadline(250, kMicrosecondScale, &next);
            IOLockSleepDeadline(gNmxAsLock, (event_t)&e->busy, next < end ? next : end, THREAD_UNINT);
        }
        uint64_t ns = 0; absolutetime_to_nanoseconds(mach_absolute_time() - now, &ns);
        OSAddAtomic64((SInt64)(ns / 1000), &gCntNmxWaitUs);
    }
    if (e->busy) {
        IOSurface *old = e->next;
        ((OSObject *)s)->retain();
        e->next = s;
        IOLockUnlock(gNmxAsLock);
        OSAddAtomic64(1, &gCntNmxQueued);
        if (old) { OSAddAtomic64(1, &gCntNmxDrop); ((OSObject *)old)->release(); }
        if (drop) ((OSObject *)drop)->release();
        return true;
    }
    ((OSObject *)s)->retain();
    e->pipe = pipe; e->fb = fb; e->head = head; e->s = s; e->t0 = mach_absolute_time();
    __atomic_store_n(&e->busy, 1, __ATOMIC_RELEASE);
    IOLockUnlock(gNmxAsLock);
    nmxVrrStampRegister(fb, e, head);
    if (drop) ((OSObject *)drop)->release();
    if (lost) {
        OSAddAtomic64(1, &gCntNmxLost);
        ALOG("nmx: head %u: the framebuffer never completed a flip in 500 ms -- frame released", head);
        _ZN18IOAccelDisplayPipe26signalTransactionInterruptEPv(pipe, nullptr);
    }
    thread_call_enter(e->call);
    return true;
}

static int nmxaccel_async_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gSurfGpuOn;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gSurfGpuOn = v;
    return 0;
}
static int nmxaccel_flip_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNmxFlip;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gNmxFlip = v;
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_async, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nmxaccel_async_sysctl, "I",
            "stage 2: 1 = WindowServer's GPU marks are taken and frames are presented on a thread call once their GPU work is done (default 0)");
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_flip, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nmxaccel_flip_sysctl, "I",
            "stage 3: 1 = frames with a scan-out buffer are flipped to (PLANE_SURF) instead of copied; needs nmxaccel_async=1 (default 0)");
NVACCEL_CNT(nmxaccel_async_frames,   gCntNmxFrames,      "frames the present worker completed");
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_depth, CTLFLAG_RW | CTLFLAG_LOCKED, &gNmxDepth, 0, "nmx-tahoe2: 2 = WindowServer may present one frame while another is in flight (queued behind it), 0 = one at a time");
NVACCEL_CNT(nmxaccel_async_queued,   gCntNmxQueued,      "presents that queued behind a frame still in flight past a refresh and a quarter");
NVACCEL_CNT(nmxaccel_async_drop,     gCntNmxDrop,        "queued frames replaced by a newer one (never shown)");
NVACCEL_CNT(nmxaccel_async_wait_us,  gCntNmxWaitUs,      "microseconds WindowServer's presents waited for the frame in flight");
NVACCEL_CNT(nmxaccel_async_lost,     gCntNmxLost,        "frames released because the framebuffer never completed their flip in 500 ms");
NVACCEL_CNT(nmxaccel_marks,          gCntSurfMarks,      "WindowServer GPU marks taken (pending + ready)");
NVACCEL_CNT(nmxaccel_mark_waits,     gCntSurfWaits,      "presents (and copies) that waited for their surface's GPU work");
NVACCEL_CNT(nmxaccel_mark_timeouts,  gCntSurfTimeouts,   "presents whose surface was not finished on the GPU within 100 ms (presented anyway)");
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_mark_wait_max_us, CTLFLAG_RW | CTLFLAG_LOCKED, &gSurfWaitMaxUs, 0, "longest wait for a surface's GPU work, us (write 0 to reset)");
NVACCEL_CNT(nmxaccel_flips,          gCntNmxFlips,       "frames flipped to (PLANE_SURFLIVE showed their scan-out buffer)");
NVACCEL_CNT(nmxaccel_flip_unbound,   gCntNmxFlipUnbound, "frames copied because their surface has no scan-out buffer (or another size)");
NVACCEL_CNT(nmxaccel_flip_refused,   gCntNmxFlipRefused, "frames copied because the framebuffer refused the flip");
NVACCEL_CNT(nmxaccel_flip_late,      gCntNmxFlipLate,    "flips/homes not on the panel within 100 ms (completed anyway)");
NVACCEL_CNT(nmxaccel_flip_abort,     gCntNmxFlipAbort,   "flips/homes abandoned for a mode change, an unmap or a framebuffer stop");
NVACCEL_CNT(nmxaccel_flip_l2fail,    gCntNmxL2Fail,      "frames copied because IntelXe could not flush the GPU's L2");
NVACCEL_CNT(nmxaccel_flip_home,      gCntNmxHome,        "returns of the plane to the firmware's surface after a copied frame");
NVACCEL_CNT(nmxaccel_vbl_due,        gCntNmxVblDue,      "frames still on the GPU when presented, due at a vblank from IntelXeFB's phase (dispboost)");
NVACCEL_CNT(nmxaccel_vbl_nophase,    gCntNmxVblNoPhase,  "frames still on the GPU when presented, due one refresh after it (no vblank phase)");
NVACCEL_CNT(nmxaccel_vbl_unknown,    gCntNmxVblUnknown,  "frames still on the GPU when presented, no refresh rate known (not checked)");
NVACCEL_CNT(nmxaccel_vbl_miss,       gCntNmxVblMiss,     "frames still on the GPU at the vblank they were due at (reported to IntelXe: debug.intelxe.disp_boost)");
NVACCEL_CNT(nmxaccel_vbl_boost_fail, gCntNmxBoostFail,   "misses IntelXe did not take (no intelxeDispBoost: an older IntelXe.kext)");
NVACCEL_CNT(nmxaccel_idle_boosts,    gCntNmxIdleBoost,   "first frames after a quiet display (nmxaccel_idle_boost_ms) that asked IntelXe for the boost");
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_idle_boost_ms, CTLFLAG_RW | CTLFLAG_LOCKED, &gNmxIdleBoostMs, 0,
           "dispboost: the first frame after the display was quiet this long (ms) asks IntelXe for the boost at once (0 = off)");
NVACCEL_CNT(nmxaccel_vrr_tx,         gCntNmxVrrTx,       "transactions carrying an adaptive-sync frame-length window (dirty bit 0x10000)");
NVACCEL_CNT(nmxaccel_vrr_bad,        gCntNmxVrrBad,      "adaptive-sync windows ignored as implausible");
NVACCEL_CNT(nmxaccel_pin_runs,       gCntNmxPinRuns,     "scan-out binds pinned as several DPA runs (intelxeScanoutPin v2)");
NVACCEL_CNT(nmxaccel_vrr_stamps,     gCntNmxVrrStamps,   "adaptive-sync frame time stamps reported to the display pipe");
NVACCEL_CNT(nmxaccel_scanout_binds,  gCntNmxBinds,       "scan-out buffers bound (pinned + mapped at a GGTT slot)");
NVACCEL_CNT(nmxaccel_scanout_unbinds, gCntNmxUnbinds,    "scan-out buffers released");
NVACCEL_CNT(nmxaccel_scanout_refused, gCntNmxBindRefused, "scan-out binds refused (no slot, not pinnable, not mappable)");
NVACCEL_CNT(nmxaccel_p4_marks,     gCntNmxP4Mark,      "phase 4: composites whose readback was skipped (marked newer)");
NVACCEL_CNT(nmxaccel_p4_noresource, gCntNmxP4MarkNoRes, "phase 4: marks refused (no family resource: readback kept)");
NVACCEL_CNT(nmxaccel_p4_pageoffs,  gCntNmxP4Pageoff,   "phase 4: composites paged off from their scan-out buffer (CPU lock, copied frame, unbind)");
NVACCEL_CNT(nmxaccel_p4_failed,    gCntNmxP4Fail,      "phase 4: page-offs that failed");
NVACCEL_CNT(nmxaccel_p4_busy,      gCntNmxP4Busy,      "phase 4: page-offs given up because the scan-out lock stayed taken");
NVACCEL_CNT(nmxaccel_sv_demoted,        gCntSvDemoted,       "rb7f: VRAM-shared surfaces demoted after repeated CPU page-offs (writers copy out)");
NVACCEL_CNT(nmxaccel_sv_demote_refused, gCntSvDemoteRefused, "rb7f: dirty marks of demoted surfaces answered NMX_SV_DEMOTED");
NVACCEL_CNT(nmxaccel_sv_undemoted,      gCntSvUndemoted,     "rb7f: demotions undone by a mark from a writer that cannot copy out");
NVACCEL_CNT(nmxaccel_sv_cpu_waits,      gCntSvCpuWaits,      "rb7g: CPU locks of demoted surfaces that waited for their GPU writers");
NVACCEL_CNT(nmxaccel_sv_cpu_wait_timeouts, gCntSvCpuWaitTimeouts, "rb7g: such waits that gave up after 1 s");
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_sv_demote, CTLFLAG_RW | CTLFLAG_LOCKED, &gNmxSvDemoteOn, 0, "rb7f: 1 = demote CPU-read VRAM-shared surfaces (0: never)");
SYSCTL_INT(_debug, OID_AUTO, nmxaccel_sv_hot_n, CTLFLAG_RW | CTLFLAG_LOCKED, &gNmxSvHotN, 0, "rb7f: page-offs within 1 s that demote a surface (min 2)");
// debug.nmxaccel_heads (read only, root): every IntelXeFB head this accelerator sees, what it is armed with and what the
// multi-head knob would do about it (nmx_heads.h). Usable with one monitor: no register writes, no registration, only
// each framebuffer's nvScanoutInfo answer and the display machine's pipe table read back. No kernel pointers printed.
static const char *nmxAutoStateName(int s)
{
    switch (s) {
    case 0: return "off"; case 1: return "waiting for IntelXeFB"; case 2: return "arming"; case 3: return "armed";
    case -1: return "held by IntelXeFB"; case -2: return "no device control"; case -3: return "arm failed"; case -4: return "no framebuffer";
    default: return "?";
    }
}
static int nmxHeadsText(char *b, int cap)
{
    NVAccel *a = NVAccel::gAccel;
    int n = 0;
#define HP(...) do { if (n < cap) { int w_ = snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); if (w_ > 0) n += w_; } } while (0)
    HP("NMXAccel heads (docs/design/DESIGN-multihead-accel.md)\n");
    HP("multi-head knob: %d (boot-arg nmxheads / NVRAM nmx-heads / debug.nmxaccel_heads_auto: 0 off, 1 register + mode change, "
       "2 register only)\n", gNmxHeads);
    if (!a) { HP("no accelerator running\n"); return n < cap ? n : cap - 1; }
    HP("accelerator: armed %d, boot arm %d (%s), scan-out gate %d, aperture %d, transactions %d, async %d, flip %d\n",
       gNvAccelArmed, gNmxAutoState, nmxAutoStateName(gNmxAutoState), gNvScanout, gNvAperture, gNvIop, gSurfGpuOn, gNmxFlip);
    HP("heads: published 0x%x, registered 0x%x, piped 0x%x\n", gNvHeadsPublished, a->fFBRegistered, gNvHeadsPiped);
    if (a->fDM) {
        const uint8_t *dm = (const uint8_t *)a->fDM;
        const unsigned np = *(const volatile uint32_t *)(dm + 0x108);
        HP("display machine: %u framebuffer(s), %u pipe(s)\n", a->fDM->getFramebufferCount(), np);
        for (unsigned k = 0; np <= 8 && k < np; k++) {
            const void *pp = *(void *const volatile *)(dm + 0x88 + 8 * k);
            IOService *pfb = pp ? *(IOService *const volatile *)((const uint8_t *)pp + 0x98) : nullptr;
            int head = -1;
            for (unsigned i = 0; i < kNvMaxFB; i++) if (pfb && a->fFBs[i] == pfb) head = (int)i;
            if (head >= 0) HP("  pipe %u -> head %d\n", k, head);
            else HP("  pipe %u -> %s\n", k, !pp ? "NULL pipe" : pfb ? "a framebuffer that is none of ours" : "no framebuffer");
        }
    } else HP("display machine: none\n");
    NmxAccelIn ai; a->headsAccelIn(&ai);
    for (unsigned i = 0; i < kNvMaxFB; i++) {
        NmxHeadIn h; struct NVRMVramRequest q;
        a->headIn(i, &h, &q);
        const int v = nmxHeadVerdict(i, h, ai);
        IOService *fb = a->fFBs[i];
        if (!fb) {
            if (v == kNmxHeadTerminated) HP("head %u: terminated %u time(s), slot free\n", i, gNmxHead[i].terminated);
            else if (i < 2) HP("head %u: absent\n", i);
            continue;
        }
        unsigned long long idx = 0;
        if (OSNumber *num = OSDynamicCast(OSNumber, fb->getProperty("IOFBDependentIndex"))) idx = num->unsigned64BitValue();
        const unsigned caps = nmxHeadCapsOf(fb);   // live (the present path uses the copy taken at publish)
        HP("head %u: %s id 0x%llx IOFBDependentIndex %llu%s\n", i, fb->getMetaClass()->getClassName(),
           (unsigned long long)fb->getRegistryEntryID(), idx, fb->isInactive() ? " INACTIVE" : "");
        if (OSObject *co = fb->getProperty("IntelXeFBConnected"))
            HP("  monitor: %s (IntelXeFBConnected)\n", co == kOSBooleanTrue ? "connected" : "none (disconnected: the head stays published, its pipe idles)");
        if (h.scanoutOk) HP("  scan-out %ux%u pitch %u (%s)\n", q.width, q.height, q.pitch, q.phys ? "in VRAM: a framebuffer resource" : "CPU only: frames copied");
        else HP("  scan-out: does not describe itself (not lit, or disconnected)\n");
        HP("  caps:%s%s%s%s%s; cursor: %s\n", caps & kNmxCapFlip ? " flips" : "", caps & kNmxCapGamma ? " pipe-gamma" : "",
           caps & kNmxCapTiming ? " agdc-timing" : " NO-agdc-timing", caps & kNmxCapPipeFB ? " second-pipe-fb" : "",
           caps & (kNmxCapFlip | kNmxCapGamma) ? "" : " (copy only)", caps & kNmxCapPipeFB ? "software (WindowServer draws it)" : "IntelXeFB's cursor plane or software (debug.intelxefb_hwcursor)");
        HP("  frames %lld; last source %ux%u onto %ux%u (%lld size mismatch(es))\n", (long long)gCntIopOkHead[i],
           gNmxHead[i].srcW, gNmxHead[i].srcH, gNmxHead[i].scW, gNmxHead[i].scH, (long long)gNmxHead[i].mismatch);
        if (gNmxHead[i].lateResult) {
            uint64_t ns = 0; absolutetime_to_nanoseconds(gNmxHead[i].armedAt - gNmxHead[i].publishedAt, &ns);
            HP("  registered late (%s), %llu ms after it published\n", gNmxHead[i].lateResult > 0 ? "own pipe" : "NO pipe", (unsigned long long)(ns / 1000000));
        }
        HP("  -> %s\n", nmxHeadVerdictName(v));
    }
    HP("late-head arm: %s (retries %u)\n", a->fHeadsLast, a->fHeadsRetries);
#undef HP
    return n < cap ? n : cap - 1;
}
static int nmxaccel_heads_sysctl SYSCTL_HANDLER_ARGS
{
    if (!kauth_cred_issuser(kauth_cred_get())) return EPERM;
    const int cap = 6144;
    char *b = (char *)IOMalloc(cap);
    if (!b) return ENOMEM;
    int n = nmxHeadsText(b, cap);
    if (n < 0) n = 0;
    b[n] = 0;
    const int err = SYSCTL_OUT(req, b, (size_t)n + 1);
    IOFree(b, cap);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_heads, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, nmxaccel_heads_sysctl, "A",
            "every IntelXeFB head: published/registered/piped, scan-out, capabilities, frames, and what the multi-head knob would do (root)");
static int nmxaccel_heads_auto_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gNmxHeads;
    int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v < 0 || v > 2) return EINVAL;
    NVAccel *a = NVAccel::gAccel;
    const int was = gNmxHeads;
    gNmxHeads = v;
    if (v && a && !a->headsEnable("debug.nmxaccel_heads_auto")) { gNmxHeads = was; return ENOMEM; }
    if (v && a) a->headsKick("debug.nmxaccel_heads_auto", 0);
    else ALOG("multi-head: off (debug.nmxaccel_heads_auto=0): heads published from now on wait for debug.nvaccelfb=3");
    return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nmxaccel_heads_auto, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nmxaccel_heads_auto_sysctl, "I",
            "multi-head: 1 = register a head published after the arm on its own (+ the mode change), 2 = without the mode change, 0 = off (default)");
static void nmxInstallSysctl()
{
    sysctl_register_oid(&sysctl__debug_nmxaccel_heads);         sysctl_register_oid(&sysctl__debug_nmxaccel_heads_auto);
    sysctl_register_oid(&sysctl__debug_nmxaccel_p4_marks);       sysctl_register_oid(&sysctl__debug_nmxaccel_p4_noresource);
    sysctl_register_oid(&sysctl__debug_nmxaccel_p4_pageoffs);    sysctl_register_oid(&sysctl__debug_nmxaccel_p4_failed);
    sysctl_register_oid(&sysctl__debug_nmxaccel_p4_busy);
    sysctl_register_oid(&sysctl__debug_nmxaccel_sv_demoted);    sysctl_register_oid(&sysctl__debug_nmxaccel_sv_demote_refused);
    sysctl_register_oid(&sysctl__debug_nmxaccel_sv_undemoted);  sysctl_register_oid(&sysctl__debug_nmxaccel_sv_demote);
    sysctl_register_oid(&sysctl__debug_nmxaccel_sv_hot_n);
    sysctl_register_oid(&sysctl__debug_nmxaccel_sv_cpu_waits); sysctl_register_oid(&sysctl__debug_nmxaccel_sv_cpu_wait_timeouts);
    sysctl_register_oid(&sysctl__debug_nmxaccel_async);          sysctl_register_oid(&sysctl__debug_nmxaccel_flip);
    sysctl_register_oid(&sysctl__debug_nmxaccel_async_frames);   sysctl_register_oid(&sysctl__debug_nmxaccel_async_queued); sysctl_register_oid(&sysctl__debug_nmxaccel_depth);
    sysctl_register_oid(&sysctl__debug_nmxaccel_async_drop);     sysctl_register_oid(&sysctl__debug_nmxaccel_async_wait_us);
    sysctl_register_oid(&sysctl__debug_nmxaccel_async_lost);     sysctl_register_oid(&sysctl__debug_nmxaccel_marks);
    sysctl_register_oid(&sysctl__debug_nmxaccel_mark_waits);     sysctl_register_oid(&sysctl__debug_nmxaccel_mark_timeouts);
    sysctl_register_oid(&sysctl__debug_nmxaccel_mark_wait_max_us);
    sysctl_register_oid(&sysctl__debug_nmxaccel_flips);          sysctl_register_oid(&sysctl__debug_nmxaccel_flip_unbound);
    sysctl_register_oid(&sysctl__debug_nmxaccel_flip_refused);   sysctl_register_oid(&sysctl__debug_nmxaccel_flip_late);
    sysctl_register_oid(&sysctl__debug_nmxaccel_flip_abort);     sysctl_register_oid(&sysctl__debug_nmxaccel_flip_l2fail);
    sysctl_register_oid(&sysctl__debug_nmxaccel_flip_home);
    sysctl_register_oid(&sysctl__debug_nmxaccel_vbl_due);        sysctl_register_oid(&sysctl__debug_nmxaccel_vbl_nophase);
    sysctl_register_oid(&sysctl__debug_nmxaccel_vbl_unknown);    sysctl_register_oid(&sysctl__debug_nmxaccel_vbl_miss);
    sysctl_register_oid(&sysctl__debug_nmxaccel_vbl_boost_fail);
    sysctl_register_oid(&sysctl__debug_nmxaccel_idle_boosts);    sysctl_register_oid(&sysctl__debug_nmxaccel_idle_boost_ms);
    sysctl_register_oid(&sysctl__debug_nmxaccel_vrr_tx);         sysctl_register_oid(&sysctl__debug_nmxaccel_vrr_bad);
    sysctl_register_oid(&sysctl__debug_nmxaccel_vrr_stamps);      sysctl_register_oid(&sysctl__debug_nmxaccel_pin_runs);      sysctl_register_oid(&sysctl__debug_nmxaccel_scanout_binds);
    sysctl_register_oid(&sysctl__debug_nmxaccel_scanout_unbinds); sysctl_register_oid(&sysctl__debug_nmxaccel_scanout_refused);
}
// A setting for the boot-time arm: boot-arg <arg>=N, else NVRAM <nv> ("0"/"1"), else the default
static int nmxSetting(const char *arg, const char *nv, int def)
{
    int v = def;
    if (PE_parse_boot_argn(arg, &v, sizeof v)) return v;
    if (IORegistryEntry *opt = IORegistryEntry::fromPath("/options", gIODTPlane)) {
        if (OSObject *o = opt->copyProperty(nv)) {
            OSData *d = OSDynamicCast(OSData, o); OSString *str = OSDynamicCast(OSString, o);
            const char c = d && d->getLength() ? *(const char *)d->getBytesNoCopy() : str && str->getLength() ? str->getCStringNoCopy()[0] : 0;
            if (c >= '0' && c <= '9') v = c - '0';
            o->release();
        }
        opt->release();
    }
    return v;
}
// What intelxe-accel.sh did, in the kernel and before WindowServer sees the accelerator: NMXAGDC's device control first
// (it comes up on its own when IntelXeFB publishes; nmx-agdc-up on the GPU), then the pipe (levels 1, 3, 4), transactions,
// async presents and flips. Flips on by default (the fix22 scan-out corruption is fixed by plugin fix22b, uncompressed
// scan-out memory); off: nmxflip=0 or NVRAM nmx-flip=0. Async off: nmxasync=0 or NVRAM nmx-async=0 (flips need it).
static void nmxAutoArm(NVAccel *a)
{
    if (!a) return;
    IOService *pci = a->fPCI;
    // NMXAGDC reports its outcome on the GPU: nmx-agdc-state "up" (with nmx-agdc-up) or "failed: ..." after its own retries
    // (~6 s). Waited for here up to 30 s, on this thread call, with no IOService lock held.
    unsigned waited = 0;
    const char *failed = nullptr;
    while (pci && waited < 30000) {
        if (pci->getProperty("nmx-agdc-up") == kOSBooleanTrue) break;
        OSString *st = OSDynamicCast(OSString, pci->getProperty("nmx-agdc-state"));
        if (st && !strncmp(st->getCStringNoCopy(), "failed", 6)) { failed = st->getCStringNoCopy(); break; }
        IOSleep(20); waited += 20;
    }
    if (!pci || pci->getProperty("nmx-agdc-up") != kOSBooleanTrue) {
        gNmxAutoState = -2;
        char m[200];
        snprintf(m, sizeof m, "NMXAGDC's device control %s after %u ms: NOT armed (WindowServer runs unaccelerated on IntelXeFB)",
                 failed ? failed : "did not come up", waited);
        a->autoNote(m);
        a->registerOnce("not armed");
#ifdef NMX_NOMETAL_FALLBACK
        a->nmxNoMetalFallback("device control down");
#endif
        return;
    }
    // Not while IntelXeFB or this accelerator is still being registered and matched: boot1c started arming 6 ms after
    // IntelXeFB published and never came back (03:48:15, ioreg hung). intelxe-accel.sh always armed long after both were quiet.
    {
        IOService *fb = a->fFBs[0];
        const IOReturn q1 = fb ? fb->waitQuiet(10ull * 1000 * 1000 * 1000) : kIOReturnNotAttached;
        const IOReturn q2 = a->waitQuiet(10ull * 1000 * 1000 * 1000);
        if (q1 != kIOReturnSuccess || q2 != kIOReturnSuccess) {
            gNmxAutoState = -3;
            char m[160];
            snprintf(m, sizeof m, "IntelXeFB (0x%x) or this accelerator (0x%x) never went quiet in 10 s: NOT armed", q1, q2);
            a->autoNote(m);
#ifdef NMX_NOMETAL_FALLBACK
            a->nmxNoMetalFallback("never quiet");
#endif
            return;
        }
    }
    ALOG("auto-arm: device control up after %u ms, IntelXeFB and the accelerator quiet; arming the display pipe", waited);
    // the guard's accelerated stage begins BEFORE the arm: a boot that hangs or panics inside it (boot1c) leaves
    // intelxe-guard=accel, and the next boot stays unaccelerated instead of hanging again
    IOService *gfb = a->fFBs[0];
    const OSSymbol *g = OSSymbol::withCString("intelxefbGuardAccel");
    const IOReturn gr = (gfb && g) ? gfb->callPlatformFunction(g, false, nullptr, nullptr, nullptr, nullptr) : kIOReturnNotFound;
    if (g) g->release();
    a->fBootWalk = true;    // registerPendingLocked: the boot walk (nmxHeadBootSkips)
    NVAccel::triggerFromSysctl(1);
    NVAccel::triggerFromSysctl(3);
    NVAccel::triggerFromSysctl(4);
    a->fBootWalk = false;
    (void)nvaccelSetIop(1);
    // IntelXeFB's boot guard after repeated failed boots (IntelXeFBAccelSafe): the most conservative settings, no async
    // swaps, no flips, one frame at a time - the stage-1 arm of intelxe-accel.sh
    const bool safe = gfb && gfb->getProperty("IntelXeFBAccelSafe") == kOSBooleanTrue;
    gSurfGpuOn = !safe && nmxSetting("nmxasync", "nmx-async", 1) ? 1 : 0;
    gNmxFlip = gSurfGpuOn && nmxSetting("nmxflip", "nmx-flip", 1) ? 1 : 0;
    // One frame per head by default again (boot-arg nmxdepth=2 or NVRAM nmx-depth=2: one more queued). With two, a frame
    // queued behind one in flight counts as complete early, WindowServer gets the previous surface back while its scan-out
    // buffer is still on the panel, and the next frame is copied into the buffer being scanned: tearing on moving edges,
    // most at the cheapest frames (6 Nov, seen in every mode, gone at depth 0). Measured 6 Nov at 180 Hz: a moving window
    // and a moving layer both 180 frames/s at depth 0 and 2 (the 4 Nov gain, 122 -> 165 for a window drag, was with
    // slower frames).
    gNmxDepth = gSurfGpuOn && gNmxFlip ? nmxSetting("nmxdepth", "nmx-depth", 0) : 0;
    if (!gNvAccelArmed || !gNvIop) {
        gNmxAutoState = -3;
        char m[128];
        snprintf(m, sizeof m, "arming failed (armed %d, transactions %d): registered unarmed", gNvAccelArmed, gNvIop);
        a->autoNote(m);
        a->registerOnce("arm failed");
#ifdef NMX_NOMETAL_FALLBACK
        a->nmxNoMetalFallback("arm failed");
#endif
        return;
    }
    gNmxAutoState = 3;
    char m[160];
    snprintf(m, sizeof m, "armed: pipe, transactions, async %d, flip %d, guard %s%s", gSurfGpuOn, gNmxFlip,
             gr == kIOReturnSuccess ? "accel" : "not told", safe ? ", SAFE settings (boot guard)" : "");
    a->autoNote(m);
    a->registerOnce("armed");
    // a head that published while the arm ran (after its registration walk) is registered now (knob on; else nothing)
    if (gNmxHeads) a->headsKick("after the boot arm", 2000);
}
#endif

NM_TAHOE_FWD_DEFS
