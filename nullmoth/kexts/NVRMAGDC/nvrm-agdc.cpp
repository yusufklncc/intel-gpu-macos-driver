/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#include "nvrm_off.h"
#include <IOKit/IOService.h>
#include <IOKit/IOLib.h>
#include <pexpert/pexpert.h>     // PE_parse_boot_argn (nmxgamma)
#include <sys/sysctl.h>
#include <kern/thread_call.h>
#include <kern/clock.h>          // clock_get_uptime/nanoseconds_to_absolutetime: the 0x980 rescan throttle
#include <libkern/OSAtomic.h>
#include <mach/kmod.h>
extern "C" kern_return_t _start(kmod_info_t *ki, void *d); extern "C" kern_return_t _stop(kmod_info_t *ki, void *d);
#ifdef NMX_AGDC
// NMXAGDC: the same device control for the Arc B580, whose framebuffer is IntelXeFB (kernel/IntelXeFB). WindowServer builds its
// display pipes from the 0x980 reply, so without this object CoreDisplay logged "Unable to find display pipe for IOFB
// service" and never sent the accelerated frames (2026-10-28). Classes renamed so it can sit beside NVRMAGDC.
#define FBLOG(fmt, ...) IOLog("NMX-agdc: " fmt "\n", ##__VA_ARGS__)
#define NVDADeviceControl NMXDeviceControl
#define NVDAGDCLoader NMXAGDCLoader
#define AGDC_FB_CLASS "IntelXeFramebuffer"
#define AGDC_VENDOR "Intel"
#define AGDC_VENDOR_ID 0x8086
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NMXAGDC, "0.1", _start, _stop) }
#else
#define FBLOG(fmt, ...) IOLog("NVRM-agdc: " fmt "\n", ##__VA_ARGS__)
#define AGDC_FB_CLASS "NVRMNVDAFramebuffer"
#define AGDC_VENDOR "NVIDIA"
#define AGDC_VENDOR_ID 0x10de
extern "C" { KMOD_EXPLICIT_DECL(com.nullmoth.NVRMAGDC, "0.1", _start, _stop) }
#endif
extern "C" kern_return_t _start(kmod_info_t *ki, void *d) { return KERN_SUCCESS; }
extern "C" kern_return_t _stop(kmod_info_t *ki, void *d)  { return KERN_SUCCESS; }
#include "AppleGraphicsDeviceControl_re.h"
#include "nvrm-agdc-k5.h"
#ifdef NMX_AGDC
#include "intelxe_bootargs.h"   // -intelxe_noaccel and its name before the rename, for one release
#endif
#pragma pack(push, 1)
struct NVAGDCVendorInfo {
    uint32_t version;
    char     vendor[0x20];
    uint32_t vendorID;
    uint32_t vendorClass;
};
struct NVAGDCGPUCapability {
    uint64_t portMask;
    uint64_t portFeatureA;
    uint64_t portFeatureB;
    uint64_t portFeatureC;
    uint32_t countA, countB, countC, countD;
    uint32_t maxFramebuffers;
    IOService *pciDevice;
    IOService *framebuffer[20];
};
#pragma pack(pop)
static_assert(sizeof(NVAGDCVendorInfo) == 0x2c, "AGDCVendorInfo_t is 0x2c");
static_assert(sizeof(NVAGDCGPUCapability) == 0xdc, "AGDCGPUCapability_t is 0xdc");

static int    gAgdcK5 = 0;
static SInt64 gAgdcK5Link = 0, gAgdcK5Caps = 0;
static int    gAgdcK5Why = 0;
static SInt64 gAgdcCmds = 0, gAgdcUnsupported = 0;
static int    gAgdcLastRefused = 0;
static int    gAgdcLogged = 0;
// Slot-table rescans done while answering 0x980 (see nvdaAgdcRescanForReply).
static SInt64 gAgdcRescans = 0;

enum { kAgdcMaxFB = 4 };
static IOService *gAgdcFB[kAgdcMaxFB];
static unsigned  gAgdcNFB = 0;
enum { kAgdcFBMapBytes = 3 + kAgdcMaxFB * 25 + 52 + 1 };
static_assert(kAgdcMaxFB >= 1 && kAgdcMaxFB <= 8, "IOPresentment's map holds 8 (kext 0x31b3), and kAgdcFBMapBytes assumes one digit for n and each slot index");
static char   gAgdcFBMap[kAgdcFBMapBytes] = "never scanned";
static int    gAgdcMaxFBOut = 0;
static int    gAgdcK5Asked = 0, gAgdcK5LinkMask = 0, gAgdcK5CapsMask = 0;
static int    gAgdcK5LastRefusedIdx = -1;
// Stage 4 (NMX only): answer IOPresentment's pipe-gamma capability query (0x711 type 2) for a framebuffer that says it
// can load a pipe gamma table (IntelXeFB publishes IntelXeFBPipeGammaSize). With it CoreDisplay applies its EOTF / system
// gamma through the display pipe instead of its own full-screen GPU pass (DisplayPipe::RunFullDisplayPipe), as long as
// nothing else (linearization, CSC, scaling) is needed. IOPresentment reads the capabilities when WindowServer builds
// its display pipes, so a change takes effect at the next WindowServer start. Default 0; boot-arg nmxgamma=1 sets 1.
static int    gAgdcGamma = 0;
static SInt64 gAgdcGammaAnswers = 0;
static uint32_t nvdaAgdcGammaSize(IOService *fb)
{
#ifdef NMX_AGDC
    if (!gAgdcGamma || !fb) return 0;
    OSObject *o = fb->copyProperty("IntelXeFBPipeGammaSize"); OSNumber *n = OSDynamicCast(OSNumber, o);
    uint32_t v = n ? n->unsigned32BitValue() : 0; OSSafeReleaseNULL(o);
    return (v >= 2 && v <= 4096) ? v : 0;
#else
    return 0;
#endif
}

class NVDADeviceControl : public AppleGraphicsDeviceControl {
    OSDeclareDefaultStructors(NVDADeviceControl)
public:
    IOService *fPci; IOService *fFB;
    using AppleGraphicsDeviceControl::vendor_doDeviceAttribute;
    virtual IOReturn vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                              unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args) APPLE_KEXT_OVERRIDE;
};
OSDefineMetaClassAndStructors(NVDADeviceControl, AppleGraphicsDeviceControl)

static IOReturn nvdaK5Answer(IOService *fb, unsigned int cmd, unsigned long *in, unsigned long inSize, unsigned long *out, unsigned long *outSize)
{
    IOService *slot[kAgdcMaxFB]; unsigned nh = gAgdcNFB;
    if (nh == 0 || nh > kAgdcMaxFB) { slot[0] = fb; nh = 1; } else for (unsigned i = 0; i < nh; i++) slot[i] = gAgdcFB[i];
    uint32_t t[kAgdcMaxFB][NVK5_TIMING_WORDS]; struct nvk5_head h[kAgdcMaxFB];
    for (unsigned i = 0; i < nh; i++) {
        unsigned long tBytes = 0; bool have = false;
        OSObject *o = slot[i] ? slot[i]->copyProperty("NVRMTiming") : NULL; OSData *d = OSDynamicCast(OSData, o);
        if (d) { have = true; tBytes = d->getLength(); bzero(t[i], sizeof t[i]); if (tBytes == sizeof t[i]) memcpy(t[i], d->getBytesNoCopy(), sizeof t[i]); }
        OSSafeReleaseNULL(o);
        h[i].t = have ? t[i] : NULL; h[i].tBytes = tBytes; h[i].gammaSize = nvdaAgdcGammaSize(slot[i]);
    }
    int why = 0; int32_t idx = -1;
    int r = nvk5_answer_heads(cmd, in, inSize, out, outSize ? *outSize : 0, h, nh, &why, &idx);
    const UInt32 bit = (idx >= 0 && idx < 32) ? (1u << idx) : 0;
    if (bit) OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5Asked);
    if (r == NVK5_R_LINK) {
        // a "Variable" refresh mode on IntelXeFB (v4): the link's timing carries its vertical blanking extension, as Apple's
        // drivers report a variable timing (IODetailedTimingInformationV2.verticalBlankingExtension)
        if (idx >= 0 && (unsigned)idx < nh && slot[idx] && out) {
            if (OSObject *eo = slot[idx]->copyProperty("IntelXeFBVRRModeExtension")) {
                if (OSNumber *en = OSDynamicCast(OSNumber, eo)) ((struct NVAGDCLinkConfig *)out)->timing.vBlankingExtension = en->unsigned32BitValue();
                eo->release();
            }
        }
        OSAddAtomic64(1, &gAgdcK5Link); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5LinkMask); return kIOReturnSuccess;
    }
    if (r == NVK5_R_CAPS) { OSAddAtomic64(1, &gAgdcK5Caps); OSBitOrAtomic(bit, (volatile UInt32 *)&gAgdcK5CapsMask); return kIOReturnSuccess; }
    if (r == NVK5_R_GAMMA) {
        if (OSAddAtomic64(1, &gAgdcGammaAnswers) < 4) FBLOG("AGDC K5: pipe gamma capability answered for index %d: 3-channel 10-bit table of %u entries", idx, h[idx].gammaSize);
        return kIOReturnSuccess;
    }
    if (r == NVK5_R_UNSUPPORTED) return kIOReturnUnsupported;
    if (r == NVK5_R_NOTFOUND && idx >= 0 && (unsigned)idx < nh && !slot[idx]) why = 0x30;
    gAgdcK5Why = why; gAgdcK5LastRefusedIdx = idx;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC K5: command %#x index %d NOT answered (why %#x, %u slot(s)) [%d/40]", cmd, idx, why, nh, gAgdcLogged); }
    return r == NVK5_R_BADARG ? kIOReturnBadArgument : kIOReturnNotFound;
}

static void nvdaAgdcScan(IOService *pci);
// WindowServer reads AGDC command 0x980 when it (re)starts and builds one display pipe per entry in the reply. The
// slot table used to be scanned only once, when AGDC came up, so a framebuffer that started later -- a monitor
// plugged in after boot -- was never in the answer: WindowServer then built a single display pipe, the second
// display's composited frames had no pipe to go to, and the panel showed only its cursor while `screencapture -D 2`
// showed a full desktop (iop_ok_h0 grew, iop_ok_h1 stayed 0). Re-scan before answering, at most once a second and
// logging only when the count changes, so the reply is never stale. This runs in the vendor-call context, which
// holds no registry lock, and nvdaAgdcScan only matches services and reads their properties.
static void nvdaAgdcRescanForReply(IOService *pci)
{
    if (!pci) return;
    static uint64_t last = 0;
    uint64_t now = 0; clock_get_uptime(&now);
    uint64_t per = 0; nanoseconds_to_absolutetime(1000000000ull, &per);
    if (last && now - last < per) return;
    last = now;
    const unsigned before = gAgdcNFB;
    nvdaAgdcScan(pci);
    OSAddAtomic64(1, &gAgdcRescans);
    if (gAgdcNFB != before)
        FBLOG("AGDC: 0x980 rescan -> %u framebuffer slot(s) (was %u)", gAgdcNFB, before);
}

IOReturn NVDADeviceControl::vendor_doDeviceAttribute(unsigned int cmd, unsigned long *in, unsigned long inSize,
                                                     unsigned long *out, unsigned long *outSize, IOExternalMethodArguments *args)
{
    OSAddAtomic64(1, &gAgdcCmds);
    if (cmd == 1) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCVendorInfo)) return kIOReturnBadArgument;
        NVAGDCVendorInfo *v = (NVAGDCVendorInfo *)out; bzero(v, sizeof *v);
        v->version = 0x30000; strlcpy(v->vendor, AGDC_VENDOR, sizeof v->vendor); v->vendorID = AGDC_VENDOR_ID; v->vendorClass = 2;
        return kIOReturnSuccess;
    }
    if (cmd == 0x980) {
        if (!out || !outSize || *outSize != sizeof(NVAGDCGPUCapability)) return kIOReturnBadArgument;
        nvdaAgdcRescanForReply(fPci);   // a framebuffer that started after boot has to be in this reply
        NVAGDCGPUCapability *c = (NVAGDCGPUCapability *)out; bzero(c, sizeof *c);
        unsigned n = gAgdcNFB;
        if (n == 0 || n > kAgdcMaxFB) {
            c->portMask = 1ull << 1;
            c->countA = c->countB = c->countC = c->countD = 1;
            c->maxFramebuffers = 1; c->pciDevice = fPci; c->framebuffer[0] = fFB;
        } else {
            for (unsigned i = 0; i < n; i++) { c->framebuffer[i] = gAgdcFB[i]; c->portMask |= 1ull << (1 + i); }
            c->countA = c->countB = c->countC = c->countD = n;
            c->maxFramebuffers = n; c->pciDevice = fPci;
        }
        gAgdcMaxFBOut = (int)c->maxFramebuffers;
        return kIOReturnSuccess;
    }
    if (gAgdcK5 && (cmd == NVK5_CMD_LINKCONFIG || cmd == NVK5_CMD_FBCAPEX)) {
        IOReturn k5 = nvdaK5Answer(fFB, cmd, in, inSize, out, outSize);
        if (k5 != kIOReturnUnsupported) return k5;
    }
    OSAddAtomic64(1, &gAgdcUnsupported); gAgdcLastRefused = (int)cmd;
    if (gAgdcLogged < 40) { gAgdcLogged++; FBLOG("AGDC: command %#x (in %lu bytes) -> Unsupported [%d/40]", cmd, inSize, gAgdcLogged); }
    return kIOReturnUnsupported;
}

static int gAgdcState = 0; static NVDADeviceControl *gAgdc = nullptr; static thread_call_t gAgdcCall = nullptr; static int gAgdcWant = 0;
static IOService *gAgdcFBRef = nullptr;
// The framebuffer the publish notification handed over (retained). copyMatchingService() does not return a service
// still inside its first-publish notification (boot1, 2026-10-29 03:39: "no PCI device or no framebuffer yet"), so the
// bring-up starts from this one.
static IOService *volatile gAgdcFbHint = nullptr;
static int gAgdcTries = 0;
static void nvdaAgdcDropTable(void)
{
    unsigned n = gAgdcNFB; gAgdcNFB = 0;
    __asm__ __volatile__("" ::: "memory");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) { IOService *f = gAgdcFB[i]; gAgdcFB[i] = nullptr; if (f) f->release(); }
    if (n) snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "released (the last table had %u slot(s))", n);
}
static void nvdaAgdcDropFB(void) { if (gAgdcFBRef) { gAgdcFBRef->release(); gAgdcFBRef = nullptr; } nvdaAgdcDropTable(); }
static void nvdaAgdcTargets(IOService **pci, IOService **fb)
{
    *pci = nullptr; *fb = nullptr;
    IOService *f = gAgdcFbHint;
    if (f) f->retain();
    else {
        OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS); if (!m) return;
        f = IOService::copyMatchingService(m); m->release(); if (!f) return;
    }
    IOService *p = f->getProvider(); int hops = 0;
    while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
    if (!p || !p->metaCast("IOPCIDevice")) { f->release(); return; }
    gAgdcFBRef = f; *fb = f; *pci = p;
}

static int gAgdcScanLogged = 0;
#define SCANLOG(fmt, ...) do { if (gAgdcScanLogged < 8) { gAgdcScanLogged++; FBLOG("AGDC scan: " fmt " [%d/8]", ##__VA_ARGS__, gAgdcScanLogged); } } while (0)
static void nvdaAgdcScan(IOService *pci)
{
    nvdaAgdcDropTable(); gAgdcScanLogged = 0;
    IOService *tab[kAgdcMaxFB] = {}; unsigned seen = 0, noidx = 0, range = 0, dup = 0, other = 0, hole = 0;
    OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    if (!it && !gAgdcFbHint) { snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=0 (no iterator: the earlier one-head reply)"); FBLOG("AGDC: slot table %s", gAgdcFBMap); return; }
    OSObject *o;
    while (it && seen < 16 && (o = it->getNextObject())) {
        seen++;
        IOService *f = OSDynamicCast(IOService, o); if (!f) continue;
        unsigned long long rid = f->getRegistryEntryID();
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (p != pci) { other++; SCANLOG("framebuffer id %#llx is under another PCI device: not ours to report", rid); continue; }
        OSObject *io = f->copyProperty("IOFBDependentIndex"); OSNumber *num = OSDynamicCast(OSNumber, io);
        long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
        if (ix < 0) { noidx++; SCANLOG("framebuffer id %#llx has no IOFBDependentIndex number: left out", rid); continue; }
        if (ix >= kAgdcMaxFB) { range++; SCANLOG("framebuffer id %#llx IOFBDependentIndex %lld >= %d: left out", rid, ix, (int)kAgdcMaxFB); continue; }
        if (tab[ix]) { dup++; SCANLOG("IOFBDependentIndex %lld claimed twice: id %#llx kept, id %#llx left out", ix, (unsigned long long)tab[ix]->getRegistryEntryID(), rid); continue; }
        f->retain(); tab[ix] = f;
    }
    if (it) it->release();
    // the framebuffer still inside its publish notification is not in the iterator yet: the hint stands in for it
    if (IOService *h = gAgdcFbHint) {
        bool have = false;
        for (unsigned i = 0; i < kAgdcMaxFB; i++) if (tab[i] == h) have = true;
        OSObject *io = h->copyProperty("IOFBDependentIndex"); OSNumber *num = OSDynamicCast(OSNumber, io);
        const long long ix = num ? (long long)num->unsigned32BitValue() : -1; OSSafeReleaseNULL(io);
        IOService *p = h->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        if (!have && p == pci && ix >= 0 && ix < (long long)kAgdcMaxFB && !tab[ix]) { h->retain(); tab[ix] = h; SCANLOG("framebuffer id %#llx added from the publish notification", (unsigned long long)h->getRegistryEntryID()); }
    }
    unsigned n = 0; while (n < kAgdcMaxFB && tab[n]) n++;
    for (unsigned i = n; i < kAgdcMaxFB; i++) if (tab[i]) {
        hole++; SCANLOG("slot %u (id %#llx) sits beyond a hole at slot %u: left out", i, (unsigned long long)tab[i]->getRegistryEntryID(), n);
        tab[i]->release(); tab[i] = nullptr;
    }
    for (unsigned i = 0; i < n; i++) gAgdcFB[i] = tab[i];
    gAgdcNFB = n;
    int w = snprintf(gAgdcFBMap, sizeof gAgdcFBMap, "n=%u", n);
    for (unsigned i = 0; i < n && w > 0 && (unsigned)w < sizeof gAgdcFBMap; i++)
        w += snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " [%u]id %#llx", i, (unsigned long long)gAgdcFB[i]->getRegistryEntryID());
    if (w > 0 && (unsigned)w < sizeof gAgdcFBMap && (noidx | range | dup | other | hole))
        snprintf(gAgdcFBMap + w, sizeof gAgdcFBMap - (size_t)w, " (left out: noidx %u range %u dup %u gpu %u hole %u)", noidx, range, dup, other, hole);
    FBLOG("AGDC: slot table %s (%u framebuffer node(s) seen)", gAgdcFBMap, seen);
}

static SInt32 gAgdcBusy = 0;
static void nvdaAgdcWorkBody(void);
static void nvdaAgdcWork(thread_call_param_t, thread_call_param_t)
{
    OSIncrementAtomic(&gAgdcBusy); nvdaAgdcWorkBody(); OSDecrementAtomic(&gAgdcBusy);
}
static void nvdaAgdcWorkBody(void)
{
    if (gAgdcWant == 1 && !gAgdc) {
        // boot1c hung arming while IntelXeFB was still being published: let its registration and matching finish first
        if (IOService *h = gAgdcFbHint) {
            const IOReturn q = h->waitQuiet(10ull * 1000 * 1000 * 1000);
            if (q != kIOReturnSuccess) FBLOG("AGDC: %s still busy after 10 s (0x%x): going on", AGDC_FB_CLASS, q);
        }
        IOService *pci = nullptr, *fb = nullptr; nvdaAgdcTargets(&pci, &fb);
        if (!pci || !fb) {
            gAgdcState = -1;
#ifdef NMX_AGDC
            // retried with backoff (50 ms doubling to 1.6 s, ~6 s in all); the last failure is final and NMXAccel stops waiting
            if (gAgdcTries < 8 && gAgdcCall) {
                const uint32_t ms = 50u << (gAgdcTries < 5 ? gAgdcTries : 5);
                gAgdcTries++;
                FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p): retry %d in %u ms", pci, fb, gAgdcTries, ms);
                uint64_t when = 0; clock_interval_to_deadline(ms, kMillisecondScale, &when);
                thread_call_enter_delayed(gAgdcCall, when);
                return;
            }
            if (IOService *h = gAgdcFbHint) {
                IOService *p = h->getProvider(); int hops = 0;
                while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
                if (p) p->setProperty("nmx-agdc-state", "failed: no framebuffer or PCI device");
            }
#endif
            FBLOG("AGDC: no PCI device or no framebuffer yet (pci %p fb %p)", pci, fb); return;
        }
        nvdaAgdcScan(pci);
        NVDADeviceControl *d = OSTypeAlloc(NVDADeviceControl);
        if (!d) { gAgdcState = -2; FBLOG("AGDC: alloc failed"); nvdaAgdcDropFB(); return; }
        if (!d->init(nullptr)) { gAgdcState = -3; FBLOG("AGDC: init failed"); d->release(); nvdaAgdcDropFB(); return; }
        d->fPci = pci; d->fFB = fb;
        if (!d->attach(pci)) { gAgdcState = -4; FBLOG("AGDC: attach(%s) failed", pci->getName()); d->release(); nvdaAgdcDropFB(); return; }
        if (!d->start(pci)) {
            gAgdcState = -5; FBLOG("AGDC: AppleGraphicsDeviceControl::start failed (vendor calls so far %lld)", gAgdcCmds); d->detach(pci); d->release(); nvdaAgdcDropFB();
#ifdef NMX_AGDC
            pci->setProperty("nmx-agdc-state", "failed: AppleGraphicsDeviceControl::start");
#endif
            return;
        }
        gAgdc = d; gAgdcState = 1;
#ifdef NMX_AGDC
        pci->setProperty("nmx-agdc-up", kOSBooleanTrue);   // NMXAccel's auto-arm waits for this
        pci->setProperty("nmx-agdc-state", "up");
#endif
        FBLOG("AGDC: UP on %s — %u framebuffer slot(s), maxFramebuffers %d; vendor calls %lld, refused %lld (last %#x)", pci->getName(), gAgdcNFB, gAgdcMaxFBOut, gAgdcCmds, gAgdcUnsupported, gAgdcLastRefused);
    } else if (gAgdcWant == 0 && gAgdc) {
        NVDADeviceControl *d = gAgdc; gAgdc = nullptr;
#ifdef NMX_AGDC
        if (d->fPci) d->fPci->removeProperty("nmx-agdc-up");
#endif
        bool ok = d->terminate(kIOServiceRequired | kIOServiceSynchronous); d->release(); nvdaAgdcDropFB(); gAgdcState = 2;
        FBLOG("AGDC: terminate -> %d (a reboot is the certain way back; this is the polite one)", ok);
    }
}
static int nvrmfb_agdc_sysctl SYSCTL_HANDLER_ARGS
{
    int v = gAgdcState; int err = sysctl_handle_int(oidp, &v, 0, req);
    if (err || !req->newptr) return err;
    if (v != 0 && v != 1) return EINVAL;
    gAgdcWant = v;
    if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
    if (!gAgdcCall) return ENOMEM;
    thread_call_enter(gAgdcCall); return 0;
}
SYSCTL_PROC(_debug, OID_AUTO, nvrmfb_agdc, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, NULL, 0, nvrmfb_agdc_sysctl, "I",
            "write 1 = create our AppleGraphicsDeviceControl object, 0 = terminate it; reads the state (1 UP, <0 failed step)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_cmds,    CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcCmds,        "AGDC vendor calls received");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcUnsupported, "AGDC vendor calls answered Unsupported");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_last_refused, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcLastRefused, 0, "last AGDC command refused");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5,      CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcK5,     0, "K5: 1 = answer kAGDCLinkConfig + plane-scaler capabilities (0 after every boot)");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_link, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Link,    "K5: kAGDCLinkConfig queries answered");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_k5_caps, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Caps,    "K5: plane-scaler capability queries answered");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_why,  CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Why,  0, "K5: why the last K5 query was not answered");
SYSCTL_STRING(_debug, OID_AUTO, nvrmfb_agdc_fbmap, CTLFLAG_RD | CTLFLAG_LOCKED, gAgdcFBMap, sizeof gAgdcFBMap, "the slot table our 0x980 reports: n, then [i]id per IOFBDependentIndex");
SYSCTL_QUAD(_debug, OID_AUTO, nvrmfb_agdc_rescans, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcRescans, "slot-table rescans done while answering 0x980 (a framebuffer that started after boot)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_maxfb,   CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcMaxFBOut, 0, "maxFramebuffers in our last 0x980 reply (0 = never asked)");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_asked, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5Asked, 0, "bit i = a 0x921/0x711 named index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_link_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LinkMask, 0, "bit i = kAGDCLinkConfig answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_caps_mask, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5CapsMask, 0, "bit i = the plane-scaler capability answered for index i");
SYSCTL_INT (_debug, OID_AUTO, nvrmfb_agdc_k5_last_refused_idx, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcK5LastRefusedIdx, 0, "the index the last BADARG/NOTFOUND refusal named (-1 none); an Unsupported 0x711 type shows in k5_asked only");
#ifdef NMX_AGDC
SYSCTL_INT (_debug, OID_AUTO, nmxagdc_gamma, CTLFLAG_RW | CTLFLAG_LOCKED | CTLFLAG_KERN, &gAgdcGamma, 0,
            "stage 4: 1 = offer IntelXeFB's pipe gamma LUT to IOPresentment (0x711 type 2), so CoreDisplay can skip its GPU pass; "
            "read at WindowServer start; boot-arg nmxgamma=1 sets it at load");
SYSCTL_QUAD(_debug, OID_AUTO, nmxagdc_gamma_answers, CTLFLAG_RD | CTLFLAG_LOCKED, &gAgdcGammaAnswers, "pipe gamma capability queries answered");
// debug.nmxagdc_heads (read only): what the next 0x980 reply would list, scanned now into a table of its own (the live
// slot table is not touched), with each head's K5 answers (link timing, plane scaler at its own size, pipe gamma) and
// what IOPresentment last asked. For one monitor as for two: nothing is written, no framebuffer is called.
static int nmxagdc_heads_sysctl SYSCTL_HANDLER_ARGS
{
    const int cap = 3072;
    char *b = (char *)IOMalloc(cap);
    if (!b) return ENOMEM;
    int n = 0;
#define HP(...) do { if (n < cap) { int w_ = snprintf(b + n, (size_t)(cap - n), __VA_ARGS__); if (w_ > 0) n += w_; } } while (0)
    IOService *pci = gAgdc ? gAgdc->fPci : nullptr;
    HP("NMXAGDC: device control %s (state %d); live slot table: %s\n", gAgdc ? "up" : "not up", gAgdcState, gAgdcFBMap);
    HP("last 0x980 reply: maxFramebuffers %d (0 = never asked); 0x980 rescans %lld; K5 %d, asked 0x%x, link answered 0x%x, "
       "plane scaler answered 0x%x; pipe gamma %d\n", gAgdcMaxFBOut, gAgdcRescans, gAgdcK5, gAgdcK5Asked, gAgdcK5LinkMask,
       gAgdcK5CapsMask, gAgdcGamma);
    IOService *tab[kAgdcMaxFB] = {};
    unsigned seen = 0;
    OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS);
    OSIterator *it = m ? IOService::getMatchingServices(m) : nullptr; OSSafeReleaseNULL(m);
    while (it && seen < 16) {
        IOService *f = OSDynamicCast(IOService, it->getNextObject());
        if (!f) break;
        seen++;
        IOService *p = f->getProvider(); int hops = 0;
        while (p && !p->metaCast("IOPCIDevice") && hops++ < 8) p = p->getProvider();
        OSNumber *num = OSDynamicCast(OSNumber, f->getProperty("IOFBDependentIndex"));
        const long long ix = num ? (long long)num->unsigned32BitValue() : -1;
        const char *out = pci && p != pci ? "under another PCI device" : ix < 0 ? "no IOFBDependentIndex" : ix >= kAgdcMaxFB ? "index out of range"
                        : tab[ix] ? "index claimed twice" : nullptr;
        if (!out) { f->retain(); tab[ix] = f; }
        else HP("framebuffer id 0x%llx (%s): left out, %s\n", (unsigned long long)f->getRegistryEntryID(), f->getMetaClass()->getClassName(), out);
    }
    if (it) it->release();
    unsigned nh = 0; while (nh < kAgdcMaxFB && tab[nh]) nh++;
    HP("next 0x980 would list %u head(s)%s\n", nh, pci ? "" : " (no device control yet: the PCI device is not checked)");
    for (unsigned i = 0; i < kAgdcMaxFB; i++) {
        if (!tab[i]) continue;
        bool live = false;
        for (unsigned j = 0; j < gAgdcNFB && j < kAgdcMaxFB; j++) if (gAgdcFB[j] == tab[i]) live = true;
        HP("head %u: %s id 0x%llx%s%s, port %u\n", i, tab[i]->getMetaClass()->getClassName(), (unsigned long long)tab[i]->getRegistryEntryID(),
           i >= nh ? " BEYOND A HOLE (left out)" : "", live ? ", in the live table" : ", NOT in the live table yet (WindowServer has not asked since)", 1 + i);
        // IntelXeFB's second framebuffer is published at its start, disconnected (IntelXeFBConnected false): listed all the
        // same (Apple's model: every framebuffer of the GPU is in the reply; a monitor is a connection change), with
        // IntelXeFB's placeholder timing until a monitor lights its pipe
        if (OSObject *co = tab[i]->copyProperty("IntelXeFBConnected")) {
            HP("  monitor: %s\n", co == kOSBooleanTrue ? "connected" : "none (disconnected; the K5 timing below is IntelXeFB's placeholder or the last monitor's)");
            co->release();
        }
        OSObject *o = tab[i]->copyProperty("NVRMTiming"); OSData *d = OSDynamicCast(OSData, o);
        uint32_t t[NVK5_TIMING_WORDS] = {};
        const unsigned long tb = d ? d->getLength() : 0;
        if (d && tb == sizeof t) memcpy(t, d->getBytesNoCopy(), sizeof t);
        OSSafeReleaseNULL(o);
        const int bad = d ? nvk5_timing_bad(t, tb) : -1;
        if (bad == 0)
            HP("  K5: link timing %ux%u, %u Hz pixel clock, %u.%03u Hz; plane scaler %ux%u at 1:1 (0x10001)\n", t[NVK5_T_HVIS], t[NVK5_T_VVIS],
               t[NVK5_T_PCLK], t[NVK5_T_REFRESH_MHZ] / 1000, t[NVK5_T_REFRESH_MHZ] % 1000, t[NVK5_T_HVIS], t[NVK5_T_VVIS]);
        else HP("  K5: %s -- 0x921/0x711 for this index are refused (IOPresentment gets no capabilities for it)\n",
                bad < 0 ? "no NVRMTiming" : "NVRMTiming invalid");
        const uint32_t g = nvdaAgdcGammaSize(tab[i]);
        HP("  pipe gamma: %s", g ? "" : "not offered\n");
        if (g) HP("%u entries, 10-bit\n", g);
        HP("  IOPresentment: asked %s, link %s, plane scaler %s\n", (gAgdcK5Asked >> i) & 1 ? "yes" : "no",
           (gAgdcK5LinkMask >> i) & 1 ? "answered" : "no", (gAgdcK5CapsMask >> i) & 1 ? "answered" : "no");
    }
    for (unsigned i = 0; i < kAgdcMaxFB; i++) if (tab[i]) tab[i]->release();
#undef HP
    if (n >= cap) n = cap - 1;
    b[n] = 0;
    const int err = SYSCTL_OUT(req, b, (size_t)n + 1);
    IOFree(b, cap);
    return err;
}
SYSCTL_PROC(_debug, OID_AUTO, nmxagdc_heads, CTLTYPE_STRING | CTLFLAG_RD | CTLFLAG_LOCKED, nullptr, 0, nmxagdc_heads_sysctl, "A",
            "dry run: the heads the next 0x980 reply would list and each head's K5 answers (the live table is not touched)");
#endif
static void nvdaAgdcRemoveSysctl(void)
{
#ifdef NMX_AGDC
    sysctl_unregister_oid(&sysctl__debug_nmxagdc_gamma); sysctl_unregister_oid(&sysctl__debug_nmxagdc_gamma_answers);
    sysctl_unregister_oid(&sysctl__debug_nmxagdc_heads);
#endif
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_unregister_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}
static void nvdaAgdcInstallSysctl(void)
{
#ifdef NMX_AGDC
    { int v = 0; if (PE_parse_boot_argn("nmxgamma", &v, sizeof v)) gAgdcGamma = v ? 1 : 0; }
    sysctl_register_oid(&sysctl__debug_nmxagdc_gamma); sysctl_register_oid(&sysctl__debug_nmxagdc_gamma_answers);
    sysctl_register_oid(&sysctl__debug_nmxagdc_heads);
#endif
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_cmds);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_refused); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_last_refused);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_why);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_fbmap); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_maxfb);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_rescans);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_asked); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_link_mask);
    sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_caps_mask); sysctl_register_oid(&sysctl__debug_nvrmfb_agdc_k5_last_refused_idx);
}

class NVDAGDCLoader : public IOService {
    OSDeclareDefaultStructors(NVDAGDCLoader)
public:
    virtual bool start(IOService *provider) APPLE_KEXT_OVERRIDE;
    virtual void stop(IOService *provider) APPLE_KEXT_OVERRIDE;
#ifdef NMX_AGDC
    IONotifier *fFbNote = nullptr;
    // Boot-time arming (2026-10-29, replaces intelxe-accel.sh's first half): when IntelXeFB publishes with IntelXeFBAccelAllowed
    // (its boot guard, -intelxe_noaccel and safe mode all clear) the device control comes up on its own, with the K5 link
    // answers and the pipe gamma (nmxgamma=0 keeps the gamma off). NMXAccel arms the display pipe once nmx-agdc-up shows.
    // A IntelXeFB without the property (older builds) leaves this idle until `sysctl debug.nvrmfb_agdc=1`, as before.
    static bool fbPublished(void *target, void *ref, IOService *fb, IONotifier *note) {
        (void)target; (void)ref; (void)note;
        // a second head (IntelXeFB's second pipe, IntelXeFramebufferPipe, IOFBDependentIndex 1; with -intelxefb_pipeb it is
        // published at IntelXeFB's start, disconnected) brings nothing up: the device control comes up from head 0, and
        // the slot table lists every head at the next 0x980 (rescanned there)
        if (OSNumber *ix = fb ? OSDynamicCast(OSNumber, fb->getProperty("IOFBDependentIndex")) : nullptr) {
            if (ix->unsigned32BitValue() != 0) {
                FBLOG("auto: %s head %u published (%s): listed in the next 0x980 reply (`sysctl debug.nmxagdc_heads`)", AGDC_FB_CLASS,
                      ix->unsigned32BitValue(), fb->getProperty("IntelXeFBConnected") == kOSBooleanFalse ? "no monitor" : "a monitor");
                return true;
            }
        }
        OSObject *allowed = fb ? fb->getProperty("IntelXeFBAccelAllowed") : nullptr;
        if (!allowed) { FBLOG("auto: %s publishes no IntelXeFBAccelAllowed: idle until `sysctl debug.nvrmfb_agdc=1`", AGDC_FB_CLASS); return true; }
        if (allowed != kOSBooleanTrue) {
            OSString *why = OSDynamicCast(OSString, fb->getProperty("IntelXeFBAccelHeld"));
            FBLOG("auto: acceleration held by IntelXeFB (%s): idle", why ? why->getCStringNoCopy() : "no reason given");
            return true;
        }
        if (gAgdcWant || gAgdc) return true;
        fb->retain();
        if (!OSCompareAndSwapPtr(nullptr, fb, (void *volatile *)&gAgdcFbHint)) fb->release();
        gAgdcTries = 0;
        gAgdcK5 = 1;
        { int v = 1; if (!PE_parse_boot_argn("nmxgamma", &v, sizeof v)) v = 1; gAgdcGamma = v ? 1 : 0; }
        gAgdcWant = 1;
        if (!gAgdcCall) gAgdcCall = thread_call_allocate(nvdaAgdcWork, nullptr);
        if (gAgdcCall) thread_call_enter(gAgdcCall);
        FBLOG("auto: %s published: device control coming up (K5 on, pipe gamma %s)", AGDC_FB_CLASS, gAgdcGamma ? "on" : "off (nmxgamma=0)");
        return true;
    }
#endif
};
OSDefineMetaClassAndStructors(NVDAGDCLoader, IOService)
bool NVDAGDCLoader::start(IOService *provider)
{
    { if (nvrm_driver_off()) return false; }
    if (!IOService::start(provider)) return false;
    nvdaAgdcInstallSysctl();
#ifdef NMX_AGDC
    { char d[8];
      if (intelxeBootArg("NMXAGDC", "-intelxe_noaccel", d, sizeof d)) FBLOG("loaded; -intelxe_noaccel: idle until `sysctl debug.nvrmfb_agdc=1`");
      else if (OSDictionary *m = IOService::serviceMatching(AGDC_FB_CLASS)) {
          fFbNote = addMatchingNotification(gIOFirstPublishNotification, m, &NVDAGDCLoader::fbPublished, this, nullptr, 0);
          m->release();
          FBLOG("loaded; comes up on its own when %s publishes (%s)", AGDC_FB_CLASS, fFbNote ? "watching" : "NO notifier: idle");
      } }
    return true;
#else
    FBLOG("loaded; idle until `sysctl debug.nvrmfb_agdc=1`"); return true;
#endif
}
void NVDAGDCLoader::stop(IOService *provider)
{
#ifdef NMX_AGDC
    if (fFbNote) { fFbNote->remove(); fFbNote = nullptr; }
#endif
    // the hint is released last, after the worker below is done with it
    nvdaAgdcRemoveSysctl();
    if (gAgdcCall) {
        thread_call_cancel(gAgdcCall); int spins = 0;
        while (gAgdcBusy && spins++ < 200) IOSleep(10);
        if (gAgdcBusy) FBLOG("stop: the worker is STILL running after 2 s — leaving its thread call allocated rather than freeing it under it");
        else { thread_call_free(gAgdcCall); gAgdcCall = nullptr; }
    }
    if (gAgdc && !gAgdcBusy) { gAgdcWant = 0; nvdaAgdcWorkBody(); }
    if (!gAgdcBusy) { IOService *h = gAgdcFbHint; gAgdcFbHint = nullptr; if (h) h->release(); }
    IOService::stop(provider);
}
