// SPDX-License-Identifier: MIT
// The user client behind xemac (user/xemac/xemac.h): method 0 runs one xe/DRM ioctl for the calling task, method 1
// returns the PCI identity, memory type N maps GEM handle N. One XeClientState (handles, VMs, queues, syncobjs) per
// connection, torn down when the connection closes or its task dies.
#include "IntelXe.h"
#include "uapi/intelxe_surfvram_kabi.h"
#include <IOKit/IOUserClient.h>
#include <sys/errno.h>

struct XeClientState;
XeClientState *xeClientCreate(void *task);
void xeClientDestroy(XeClientState *c);
void xeClientEnter(XeClientState *c);
void xeClientLeave(XeClientState *c);
int xeIoctl(XeClientState *c, uint32_t request, uint64_t arg);
IOMemoryDescriptor *xeClientMemory(XeClientState *c, uint32_t handle, bool *wc, IOReturn *why);
int xeScanoutPin(void *task, uint64_t va, uint64_t size, uint64_t *dpa, void **pin, IntelXeDpaRun *runs = nullptr,
	uint32_t maxRuns = 0, uint32_t *nRuns = nullptr);
void xeScanoutUnpin(void *pin);
int xeDispVramAlloc(uint64_t size, uint32_t flags, uint64_t *dpa, void **cookie);
void xeDispVramFree(void *cookie);
int xeVramRead(uint64_t dpa, uint64_t len, void *dst);
int xeDispGgttAlloc(uint64_t size, uint64_t align, bool fixed, uint32_t *ggtt);
int xeDispGgttFree(uint32_t ggtt);

// The class name keeps the IOAccel prefix: WindowServer's sandbox opens only user clients whose class starts with one of
// a few prefixes (com.apple.WindowServer.sb: iokit-registry-entry-class-prefix "IOAccel"), and WindowServer runs ANV
// through this client once it composites on the B580 ("deny iokit-open-user-client IntelXeClient" before the rename).
class IOAccelIntelXeClient : public IOUserClient {
	OSDeclareDefaultStructors(IOAccelIntelXeClient)
public:
	bool initWithTask(task_t owningTask, void *securityID, UInt32 type) override {
		fTask = owningTask;
		return IOUserClient::initWithTask(owningTask, securityID, type);
	}
	bool start(IOService *provider) override;
	IOReturn clientClose() override;
	IOReturn clientDied() override;
	IOReturn externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *dispatch,
		OSObject *target, void *reference) override;
	IOReturn clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) override;
	void free() override {
		OSSafeReleaseNULL(dev);
		if (fGate) IOLockFree(fGate);
		fGate = nullptr;
		IOUserClient::free();
	}
	// The device is fixed before start: the provider is IntelXe (IOServiceOpen on IntelXe) or NMXAccel ("intelxeNewClient").
	void setDevice(IntelXe *d) {
		d->retain();
		dev = d;
	}
private:
	IntelXe *dev {nullptr};
	XeClientState *state {nullptr};
	task_t fTask {nullptr};
	// fGate guards `state` against teardown: a caller takes the state and enters it (xeClientEnter) under the gate;
	// xeClientDestroy waits until every thread that entered has left.
	IOLock *fGate {nullptr};
	XeClientState *enter() {
		IOLockLock(fGate);
		XeClientState *s = state;
		if (s) xeClientEnter(s);
		IOLockUnlock(fGate);
		return s;
	}
	void teardown();
};

OSDefineMetaClassAndStructors(IOAccelIntelXeClient, IOUserClient)

// One xe connection for `task`, attached to and started on `provider`: this service for IOServiceOpen(IntelXe, type 0),
// or NMXAccel ("intelxeNewClient" below) when the connection is opened on the accelerator.
static IOReturn newXeClient(IntelXe *dev, IOService *provider, task_t task, void *securityID, UInt32 type, IOUserClient **handler) {
	auto *c = OSTypeAlloc(IOAccelIntelXeClient);
	if (!c) return kIOReturnNoMemory;
	if (!c->initWithTask(task, securityID, type)) {
		c->release();
		return kIOReturnNoResources;
	}
	c->setDevice(dev);
	if (!c->attach(provider)) {
		c->release();
		return kIOReturnNoResources;
	}
	if (!c->start(provider)) {
		c->detach(provider);
		c->release();
		return kIOReturnNoResources;
	}
	*handler = c;
	return kIOReturnSuccess;
}

IOReturn IntelXe::newUserClient(task_t owningTask, void *securityID, UInt32 type, IOUserClient **handler) {
	if (type != 0) return kIOReturnBadArgument;
	return newXeClient(this, this, owningTask, securityID, type, handler);
}

bool IOAccelIntelXeClient::start(IOService *provider) {
	if (!IOUserClient::start(provider)) return false;
	if (!fGate && !(fGate = IOLockAlloc())) return false;
	// rb7h: no IOKit "default locking". Without these three properties XNU turns it on at the first open (a kext that
	// does not depend on IOSurface; ioreg showed IOUserClientDefaultLocking = Yes on every connection): each
	// externalMethod (ioctl) then holds the connection's IORWLock for reading, and each map (clientMemoryForType via
	// is_io_connect_map_memory_into_task), unmap, port-death close and notification-port call takes it for writing,
	// uninterruptibly. A CPU map thus waited for every ioctl the task had in flight, an endless syncobj wait included:
	// with a timeline wait-before-signal (one thread waits for a point, the thread that will submit it first has to map
	// a new BO) the task deadlocked and could not be killed (dEQP-VK.synchronization.timeline_semaphore.device_host.
	// write_blit_image_read_blit_image.*: main thread in IORWLockWrite, state U; HostCopyThread in the TIMELINE_WAIT).
	// Linux serialises neither DRM ioctls nor mmap against each other. Everything here is already safe concurrently:
	// ioctls ran concurrently before (read lock); maps and unmaps go through clientMemoryForType -> enter() (fGate +
	// xeClientEnter) and xeClientMemory under XeClientState::lock; teardown (clientClose/clientDied) takes `state` under
	// fGate, marks the client dying (its waits return EINTR) and waits for every thread that entered to leave; a
	// descriptor handed to IOKit for a map that is still being made keeps its retain, so teardown lingers its BO
	// (lingerAdd) until IOKit drops it; IOKit's own mapping set is under gIOObjectPortLock either way.
	setProperty("IOUserClientDefaultLocking", kOSBooleanFalse);
	setProperty("IOUserClientDefaultLockingSetProperties", kOSBooleanFalse);
	setProperty("IOUserClientDefaultLockingSingleThreadExternalMethod", kOSBooleanFalse);
	state = xeClientCreate(fTask);
	return dev && state;
}

void IOAccelIntelXeClient::teardown() {
	if (!fGate) return;
	IOLockLock(fGate);
	XeClientState *s = state;
	state = nullptr;
	IOLockUnlock(fGate);
	if (s) xeClientDestroy(s);
}

IOReturn IOAccelIntelXeClient::clientClose() {
	teardown();
	terminate();
	return kIOReturnSuccess;
}

IOReturn IOAccelIntelXeClient::clientDied() {
	teardown();
	return IOUserClient::clientDied();
}

IOReturn IOAccelIntelXeClient::externalMethod(uint32_t selector, IOExternalMethodArguments *args, IOExternalMethodDispatch *,
	OSObject *, void *) {
	if (selector == 0) {
		if (args->scalarInputCount != 2 || args->scalarOutputCount < 1) return kIOReturnBadArgument;
		XeClientState *s = enter();
		int err = s ? xeIoctl(s, (uint32_t)args->scalarInput[0], args->scalarInput[1]) : ENXIO;
		if (s) xeClientLeave(s);
		args->scalarOutput[0] = (uint64_t)err;
		args->scalarOutputCount = 1;
		return kIOReturnSuccess;
	}
	if (selector == 1) {
		if (args->scalarOutputCount < 8) return kIOReturnBadArgument;
		IOPCIDevice *p = dev->pciDevice();
		if (!p) return kIOReturnNotAttached;   // IntelXe stopped under a connection opened on NMXAccel
		args->scalarOutput[0] = p->configRead16(kIOPCIConfigVendorID);
		args->scalarOutput[1] = p->configRead16(kIOPCIConfigDeviceID);
		args->scalarOutput[2] = p->configRead8(kIOPCIConfigRevisionID);
		args->scalarOutput[3] = p->configRead16(kIOPCIConfigSubSystemVendorID);
		args->scalarOutput[4] = p->configRead16(kIOPCIConfigSubSystemID);
		args->scalarOutput[5] = p->getBusNumber();
		args->scalarOutput[6] = p->getDeviceNumber();
		args->scalarOutput[7] = p->getFunctionNumber();
		args->scalarOutputCount = 8;
		return kIOReturnSuccess;
	}
	return kIOReturnUnsupported;
}

IOReturn IOAccelIntelXeClient::clientMemoryForType(UInt32 type, IOOptionBits *options, IOMemoryDescriptor **memory) {
	bool wc = false;
	IOReturn why = kIOReturnBadArgument;   // rb7r: kIOReturnBusy for a DONTNEED BO (xe's mmap: EBUSY)
	XeClientState *s = enter();
	IOMemoryDescriptor *m = s ? xeClientMemory(s, type, &wc, &why) : nullptr;
	if (s) xeClientLeave(s);
	if (!m) return why;
	if (wc) *options = (*options & ~kIOMapCacheMask) | kIOMapWriteCombineCache;
	if (type == (1u << 30) - 1) *options |= kIOMapReadOnly;   // INTELXE_CAP_STATUS_PAGE: every client reads, nobody writes
	*memory = m;
	return kIOReturnSuccess;
}

bool IntelXe::l2Flush() {
	if (!forcewake(true)) return false;
	wr(0xb404, 1);   // XE2_GLOBAL_INVAL
	uint32_t last = 0;
	const bool ok = waitFor(0xb404, 1, 0, 1000, &last);
	forcewake(false);
	return ok;
}

// NMXAccel (nullmoth kexts/NVRM/accel/nvrm-accel.cpp, the B580 display pipe) calls these for the frame path's stage 3,
// where the plane scans WindowServer's frame out of a VRAM buffer the Metal plugin copies it into, instead of a CPU copy
// into the firmware's surface. The layout is shared with nvrm-accel.cpp (IntelXeScanoutPin there).
//   "intelxeScanoutPin"   p1 = IntelXeScanoutPin: pin the VRAM behind va..va+size of `task`'s VMs (xeScanoutPin), dpa out
//   "intelxeScanoutUnpin" p1 = the pin cookie
//   "intelxeL2Flush"      no arguments: write back the GPU's L2 before the display reads what the GPU wrote. The display
//                      engine is never coherent with the GT caches; Linux flushes before every flip
//                      (intel_atomic_commit_tail -> intel_td_flush -> xe_device_td_flush), and on BMG (graphics 20.01,
//                      Wa_16023588340) a transient flush is not enough: xe_device_l2_flush writes XE2_GLOBAL_INVAL and
//                      waits up to 1 ms, under GT forcewake.
//   Version 2 (xe's way, __xe_pin_fb_vma_ggtt: one GGTT PTE per page): the VRAM behind the range need not be one
//   physical run; runs[] receives it as DPA runs in address order (each 4 KB aligned, a multiple of 4 KB, adjacent
//   blocks merged; at most one per 64 KB block), the display writes a PTE per 4 KB page of each. status E2BIG: runs[]
//   too small, nRuns = the number needed, nothing pinned. Version 1 (the first three fields' layout is shared) still
//   requires one physical run and returns only dpa. Either way the BO is pinned (never moved, its VRAM kept) until
//   "intelxeScanoutUnpin".
struct IntelXeScanoutPin {
	uint32_t version;   // 1 or 2
	uint32_t status;    // errno: 0, ENOENT (no binding), ENOTSUP (not VRAM; v1: not one run), EINVAL, EBUSY (moving), E2BIG
	void *task;
	uint64_t va, size, dpa;   // dpa out: the first byte's DPA (v1: the whole range is contiguous from it)
	void *pin;                // out: the cookie for intelxeScanoutUnpin
	// version 2:
	uint32_t maxRuns;         // in: entries in runs
	uint32_t nRuns;           // out: entries used (with E2BIG: needed)
	IntelXeDpaRun *runs;         // in: the caller's array, out: the runs
};
// "intelxeDispBoost" no arguments (dispboost, docs/design/DESIGN-display-boost.md): NMXAccel's present worker saw a frame's
// GPU work still running at the start of vblank it was due at. Counted and handed to a thread call that may raise the
// GT's minimum clock for a short lease (debug.intelxe.disp_boost); returns at once, from any thread.
// "intelxeNewClient" p1 = IntelXeNewClient: an xe connection (IOAccelIntelXeClient, the same external methods and memory types
// as IOServiceOpen(IntelXe, 0)) for `task`, attached to and started on `provider`, returned retained in `client` for the
// caller's newUserClient to hand out. NMXAccel (an IOAccelerator) uses it for IOServiceOpen type 'B580', because the
// system-graphics sandbox (system.sb: iokit-open-user-client (iokit-connection "IOAccelerator")) lets Safari,
// iconservicesagent, VTDecoderXPCService and the other sandboxed Metal clients open only connections whose service is
// an IOAccelerator; IntelXe itself is a plain IOService. The client keeps a reference to this IntelXe, and its class (code)
// lives in this kext, which therefore cannot unload while one is open; NMXAccel's termination terminates it as any
// child, and the connection is torn down at clientClose/clientDied as before.
// "intelxeDisplayIrq" p1 = IntelXeDisplayIrq: IntelXeFB's display interrupt handler (handler nullptr unregisters). IntelXe owns the
// MSI and the dg1 sequence; with GFX_MSTR_IRQ DISPLAY_IRQ (bit 16) set it calls handler(ctx, GFX_MSTR_IRQ) on its work
// loop thread before the tile master is re-enabled (dg1_irq_handler -> xe_display_irq_handler), and never touches a
// display interrupt register itself. The handler must not block or call into IntelXe. Calls happen under gDispLock, so
// once unregistering returns, none is in flight. Registering sets the MSI up even before any GPU client.
struct IntelXeDisplayIrq {
	uint32_t version;   // 1
	void (*handler)(void *ctx, uint32_t gfxMstrIrq);
	void *ctx;
};

// Display memory for IntelXeFB, so its scan-out slots and cursor come from IntelXe's allocators instead of fixed addresses:
//   "intelxeDispVramAlloc" p1 = IntelXeDispVram: contiguous VRAM in the BAR window (flags bit 0: zeroed), dpa and cookie out
//   "intelxeDispVramFree"  p1 = the cookie
//   "intelxeGgttAlloc"     p1 = IntelXeGgttRange: a GGTT range in [0x10000000, 0xff000000) (flags bit 0: at the given ggtt,
//                       e.g. the 0xE0000000 slots and the 0xFBF00000 cursor IntelXeFB uses today); IntelXeFB writes the PTEs
//                       of its ranges itself, as now
//   "intelxeGgttFree"      p1 = the range's GGTT address (uintptr_t)
// Return kIOReturnNotReady before the GPU's first bring-up (no VRAM manager yet), kIOReturnNoSpace when full.
struct IntelXeDispVram {
	uint32_t version;   // 1
	uint32_t flags;
	uint64_t size;
	uint64_t dpa;       // out
	void *cookie;       // out
};
struct IntelXeGgttRange {
	uint32_t version;   // 1
	uint32_t flags;
	uint64_t size, align;
	uint32_t ggtt;      // in with flags bit 0, else out
};

// "intelxePowerEvent" p1 = IntelXePowerEvent: IntelXeFB's power-transition handler (handler nullptr unregisters; once that returns,
// no call is in flight). Events: 1 will suspend (first thing in setPowerState(0), display engine still powered, MMIO
// usable; synchronous), 2 resumed (after the device level is back: Resizable BAR and BAR2, pcode, GGTT mappings and
// tracked BOs, interrupts; IntelXeFB queues its restore and returns), 3 GT reset begins, 4 GT reset done (informational).
// Called on IntelXe's own threads, never under gDispLock. IntelXe never writes the GGTT at 0xE0000000-0xF7FFFFFF, 0xFBF00000
// (+256 KB) and 0xFC000000, nor VRAM at DPA 0x1F00000: IntelXeFB rewrites those itself. Exception (rb6d): at resume,
// before event 2, the copy engine zeroes DPA 0-32 MB (firmware framebuffer, fixed cursor range; see xeResume).
struct IntelXePowerEvent {
	uint32_t version;   // 1
	void (*handler)(void *ctx, uint32_t event);
	void *ctx;
};

struct IntelXeNewClient {
	uint32_t version;   // 1
	uint32_t type;      // the IOServiceOpen type on the provider
	void *task, *securityID;
	IOService *provider;
	IOUserClient *client;   // out
};

// VRAM-shared IOSurfaces (xe_core.cpp, uapi/intelxe_surfvram_kabi.h): NMXAccel's side of the registry.
int xeSurfAdopt(IntelXeSurfAdopt *a);
int xeSurfRef(IntelXeSurfAdopt *a, bool get);
int xeSurfCopy(IntelXeSurfCopy *q);
int xeSurfWaitWriters(IntelXeSurfWait *w);
int xeCopyPinToPages(IntelXePinCopy *q);
void xePinGet(void *pin);
void xePinPut(void *pin);
IOReturn IntelXe::callPlatformFunction(const OSSymbol *fn, bool wait, void *p1, void *p2, void *p3, void *p4) {
	if (fn && p1 && fn->isEqualTo("intelxeSurfAdopt")) return xeSurfAdopt(static_cast<IntelXeSurfAdopt *>(p1)) ? kIOReturnError : kIOReturnSuccess;
	if (fn && p1 && fn->isEqualTo("intelxeSurfRef")) return xeSurfRef(static_cast<IntelXeSurfAdopt *>(p1), true) ? kIOReturnError : kIOReturnSuccess;
	if (fn && p1 && fn->isEqualTo("intelxeSurfRelease")) return xeSurfRef(static_cast<IntelXeSurfAdopt *>(p1), false) ? kIOReturnError : kIOReturnSuccess;
	if (fn && p1 && fn->isEqualTo("intelxeSurfCopy")) { xeSurfCopy(static_cast<IntelXeSurfCopy *>(p1)); return kIOReturnSuccess; }   // result in ->err
	if (fn && p1 && fn->isEqualTo("intelxeSurfWaitWriters")) { xeSurfWaitWriters(static_cast<IntelXeSurfWait *>(p1)); return kIOReturnSuccess; }   // rb7g
	if (fn && p1 && fn->isEqualTo("intelxeCopyPinToPages")) { xeCopyPinToPages(static_cast<IntelXePinCopy *>(p1)); return kIOReturnSuccess; }   // ->err
	if (fn && p1 && fn->isEqualTo("intelxePinGet")) { xePinGet(p1); return kIOReturnSuccess; }
	if (fn && p1 && fn->isEqualTo("intelxePinPut")) { xePinPut(p1); return kIOReturnSuccess; }
	if (fn && fn->isEqualTo("intelxeScanoutPin")) {
		auto *r = static_cast<IntelXeScanoutPin *>(p1);
		if (!r || (r->version != 1 && r->version != 2) || (r->version == 2 && (!r->runs || !r->maxRuns)))
			return kIOReturnBadArgument;
		r->status = r->version == 2 ? (uint32_t)xeScanoutPin(r->task, r->va, r->size, &r->dpa, &r->pin, r->runs, r->maxRuns, &r->nRuns)
		                            : (uint32_t)xeScanoutPin(r->task, r->va, r->size, &r->dpa, &r->pin);
		return r->status ? kIOReturnNotFound : kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeDisplayIrq")) {
		auto *r = static_cast<IntelXeDisplayIrq *>(p1);
		if (!r || r->version != 1) return kIOReturnBadArgument;
		if (r->handler) {
			IOLockLock(gDevLock);
			const bool ok = irqInit();
			IOLockUnlock(gDevLock);
			if (!ok) return kIOReturnNoInterrupt;
		}
		IOLockLock(gDispLock);
		dispIrq = r->handler;
		dispIrqCtx = r->handler ? r->ctx : nullptr;
		IOLockUnlock(gDispLock);
		LOG("display interrupt handler %s", r->handler ? "registered" : "removed");
		return kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeNewClient")) {
		auto *r = static_cast<IntelXeNewClient *>(p1);
		if (!r || r->version != 1 || !r->task || !r->provider) return kIOReturnBadArgument;
		r->client = nullptr;
		if (isInactive()) return kIOReturnNotAttached;
		return newXeClient(this, r->provider, static_cast<task_t>(r->task), r->securityID, r->type, &r->client);
	}
	if (fn && fn->isEqualTo("intelxePowerEvent")) {
		auto *r = static_cast<IntelXePowerEvent *>(p1);
		if (!r || r->version != 1 || !pwrLock) return kIOReturnBadArgument;
		IORecursiveLockLock(pwrLock);   // waits for a call in flight (unless this is that call's own thread)
		pwrHandler = r->handler;
		pwrCtx = r->handler ? r->ctx : nullptr;
		IORecursiveLockUnlock(pwrLock);
		LOG("power event handler %s", r->handler ? "registered" : "removed");
		return kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeScanoutUnpin")) {
		if (!p1) return kIOReturnBadArgument;
		xeScanoutUnpin(p1);
		return kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeDispVramAlloc")) {
		auto *r = static_cast<IntelXeDispVram *>(p1);
		if (!r || r->version != 1) return kIOReturnBadArgument;
		const int err = xeDispVramAlloc(r->size, r->flags, &r->dpa, &r->cookie);
		return !err ? kIOReturnSuccess : err == ENXIO ? kIOReturnNotReady : err == ENOSPC ? kIOReturnNoSpace : kIOReturnBadArgument;
	}
	if (fn && fn->isEqualTo("intelxeVramRead")) {   // rb7e: p1 = IntelXeVramRead (diagnostics: IntelXeFB's thumbnail/crop)
		struct IntelXeVramRead { uint32_t version, flags; uint64_t dpa, size; void *dst; };
		auto *r = static_cast<IntelXeVramRead *>(p1);
		if (!r || r->version != 1) return kIOReturnBadArgument;
		const int err = xeVramRead(r->dpa, r->size, r->dst);
		return !err ? kIOReturnSuccess : err == ENXIO ? kIOReturnNotReady : err == ETIMEDOUT ? kIOReturnTimeout
			: err == ENOMEM ? kIOReturnNoMemory : err == EBUSY ? kIOReturnBusy : kIOReturnBadArgument;
	}
	if (fn && fn->isEqualTo("intelxeDispVramFree")) {
		if (!p1) return kIOReturnBadArgument;
		xeDispVramFree(p1);
		return kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeGgttAlloc")) {
		auto *r = static_cast<IntelXeGgttRange *>(p1);
		if (!r || r->version != 1) return kIOReturnBadArgument;
		const int err = xeDispGgttAlloc(r->size, r->align, r->flags & 1, &r->ggtt);
		return !err ? kIOReturnSuccess : err == ENOSPC ? kIOReturnNoSpace : err == EBUSY ? kIOReturnBusy : kIOReturnBadArgument;
	}
	if (fn && fn->isEqualTo("intelxeGgttFree"))
		return xeDispGgttFree((uint32_t)(uintptr_t)p1) ? kIOReturnNotFound : kIOReturnSuccess;
	if (fn && fn->isEqualTo("intelxeL2Flush")) {   // forcewake is counted, so this needs no device lock
		if (!up) return kIOReturnNotReady;
		return l2Flush() ? kIOReturnSuccess : kIOReturnTimeout;
	}
	if (fn && fn->isEqualTo("intelxeDispBoost")) {   // dispboost: counted and handed to a thread call, never blocks
		xeDispBoostMiss();
		return kIOReturnSuccess;
	}
	if (fn && fn->isEqualTo("intelxeDispBoostIdle")) {   // dispboost idle exit: a lease at once, never blocks
		xeDispBoostIdle();
		return kIOReturnSuccess;
	}
	return IOService::callPlatformFunction(fn, wait, p1, p2, p3, p4);
}
