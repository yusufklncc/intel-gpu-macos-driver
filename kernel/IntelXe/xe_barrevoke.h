// SPDX-License-Identifier: MIT
// BAR-window revocation (debug.intelxe.bar_revoke, default 0): the pure parts, so that tests/barrevoke_test.cpp builds
// them on the host. Design: docs/design/DESIGN-rebar-window.md section 8.
//
// A BO the CPU has mapped used to stay in the 1 GB BAR window for good: an IOKit user mapping of a physical range has no
// fault handler a driver can hook (Linux's TTM zaps the mmap and lets the next CPU fault find the BO wherever it is now).
// With bar_revoke 1, a cold, idle, CPU-mapped window BO can move to system memory while its user mapping stays valid:
//  1. the mapping is blocked (IOMemoryMap::redirect(NULL): a UPL_BLOCK_ACCESS UPL on its device-pager VM object, the
//     user PTEs removed, so every CPU access waits in vm_fault),
//  2. the copy engine copies the VRAM into new, physically contiguous system pages,
//  3. the GPU PTEs of every binding are rewritten to the new pages (the BO's VMs are idle and held),
//  4. the mapping is redirected (IOMemoryMap::redirect(new backing)): xnu transposes the user's VM object with the VM
//     object of a kernel mapping of the new pages (upl_transpose / vm_object_transpose, the "Virtual VRAM" path
//     vm_fault knows), which unblocks it,
//  5. the result is checked (a kernel mapping of the object the user now maps must resolve to the new pages) before
//     the old VRAM is given back.
// What can go wrong at each step, and what is kept or undone, is the state machine below; which BOs are taken, and in
// which order (least recently touched first), is the policy.
#pragma once
#include <stdint.h>

// ─── candidates ──────────────────────────────────────────────────────────────────────────────────────────────────
// What the kernel knows about one window BO, under its client's lock.
struct XeRevokeFacts {
	uint64_t size;
	uint64_t touchNs;        // last CPU touch through the uAPI (GEM_MMAP_OFFSET, a map) or EXEC on a VM binding it
	bool inWindow;           // VRAM inside the BAR window
	bool mapped;             // the CPU was given a mapping (XeBo::mapped)
	bool contig;             // one physical run (a phys_contiguous device-pager object; the only shape redirected)
	bool pinned;             // a scan-out pin or DRM_XE_GEM_CREATE_FLAG_SCANOUT: the display reads its DPA
	bool shared;             // another client has a handle, a GEM name or a PRIME export of it
	bool compressed;         // bound with a compressed PAT once (system memory has no CCS)
	bool moving;             // a move is in progress, or its binding records do not describe its PTEs (noMove)
	bool clearPending;       // its fresh-VRAM clear has not landed
	bool vmBusy;             // a VM that binds it has work outstanding (in-flight jobs pin it)
	bool purgeable;          // not WILLNEED: the purge path frees it instead
	uint32_t liveMaps;       // CPU maps with a mapping (all its maps)
	uint32_t unusedMaps;     // GEM_MMAP_OFFSET cookies not mapped yet (a map in flight)
	uint32_t otherMaps;      // maps owned by another client, by handle (old user space) or detached from a closed handle
	bool descBusy;           // its map's descriptor is held by more than the map entry and the mapping (IOKit is mapping
	                         // or unmapping it right now)
	bool refsUnaccounted;    // a reference other than its handles, bindings and the one map (a deferred free, a pending
	                         // user-fence store, a lingering map)
};

enum XeRevokeWhy : uint8_t {
	kRvOk,
	kRvNotWindow,     // not in the window (nothing to revoke)
	kRvUnmapped,      // never CPU-mapped: the ordinary eviction (boMove) takes it
	kRvPinned,
	kRvShared,
	kRvCompressed,
	kRvNotContig,
	kRvTooBig,        // above debug.intelxe.bar_revoke_max_mb (contiguous system pages are hard to get)
	kRvMoving,
	kRvClear,
	kRvBusy,          // in-flight jobs
	kRvPurgeable,
	kRvMaps,          // not exactly one live map, or maps the redirect would not reach
	kRvInFlight,      // a map being made or removed
	kRvRefs,
	kRvHot,           // touched within the hot window (not for the forced path)
	kRvWhyCount
};
static inline const char *xeRevokeWhyName(XeRevokeWhy w) {
	static const char *const n[kRvWhyCount] = {"ok", "not in window", "unmapped", "pinned", "shared", "compressed",
		"not contiguous", "too big", "moving", "clear pending", "busy", "purgeable", "maps", "map in flight", "references",
		"hot"};
	return w < kRvWhyCount ? n[w] : "?";
}

// Whether a BO may be revoked now. maxBytes: the size limit; hotNs: a BO touched less than this long ago stays (0: no
// such rule, the forced path).
static inline XeRevokeWhy xeRevokeCheck(const XeRevokeFacts &f, uint64_t maxBytes, uint64_t now, uint64_t hotNs) {
	if (!f.inWindow) return kRvNotWindow;
	if (!f.mapped && !f.liveMaps && !f.unusedMaps) return kRvUnmapped;
	if (f.pinned) return kRvPinned;
	if (f.shared) return kRvShared;
	if (f.purgeable) return kRvPurgeable;
	if (f.compressed) return kRvCompressed;
	if (!f.contig) return kRvNotContig;
	if (!f.size || f.size > maxBytes || (f.size & 0xfff)) return kRvTooBig;
	if (f.moving) return kRvMoving;
	if (f.unusedMaps || f.descBusy) return kRvInFlight;
	if (f.liveMaps != 1 || f.otherMaps) return kRvMaps;
	if (f.refsUnaccounted) return kRvRefs;
	if (f.clearPending) return kRvClear;
	if (f.vmBusy) return kRvBusy;
	if (hotNs && now >= f.touchNs && now - f.touchNs < hotNs) return kRvHot;
	if (hotNs && now < f.touchNs) return kRvHot;   // touched after `now` was read
	return kRvOk;
}

// ─── least recently touched first ──────────────────────────────────────────────────────────────────────────────
struct XeRevokeCand {
	uint64_t size, touchNs;
	uint32_t id;
};
// Sorts c[0..n) by touch time, oldest first (ties: larger first, then id: deterministic), and returns how many from the
// front are needed to cover `need` bytes (n if all of them do not).
static inline uint32_t xeRevokePick(XeRevokeCand *c, uint32_t n, uint64_t need) {
	for (uint32_t i = 1; i < n; i++) {   // insertion sort: n is small (one scan's candidates, capped by the caller)
		const XeRevokeCand x = c[i];
		uint32_t j = i;
		for (; j > 0; j--) {
			const XeRevokeCand &p = c[j - 1];
			const bool before = x.touchNs < p.touchNs || (x.touchNs == p.touchNs &&
				(x.size > p.size || (x.size == p.size && x.id < p.id)));
			if (!before) break;
			c[j] = c[j - 1];
		}
		c[j] = x;
	}
	uint64_t got = 0;
	uint32_t k = 0;
	while (k < n && got < need) got += c[k++].size;
	return k;
}
// The cut-off of a revocation pass over several clients: the touch time of the last candidate xeRevokePick takes for
// `need`. Each client then revokes its candidates touched at or before it, oldest first, without holding two client
// locks at once. 0: nothing to take.
static inline uint64_t xeRevokeCutoff(XeRevokeCand *c, uint32_t n, uint64_t need) {
	const uint32_t k = xeRevokePick(c, n, need);
	return k ? c[k - 1].touchNs : 0;
}

// ─── one revocation ─────────────────────────────────────────────────────────────────────────────────────────────
// States: where the BO's data and its two views (the CPU's mapping, the GPU's PTEs) are.
enum XeRevokeState : uint8_t {
	kRsStart,        // VRAM; nothing done (the new pages and descriptors may exist)
	kRsBlocked,      // the CPU mapping blocked (redirect(NULL)), VRAM still the BO's
	kRsCopied,       // the new pages hold a copy, the CPU still blocked
	kRsGpuMoved,     // the GPU PTEs point at the new pages, the CPU still blocked
	kRsRedirected,   // redirect(new backing) returned: the CPU is unblocked, on the new pages if it worked
	kRsDone,         // verified: the BO lives in system memory, the old VRAM may go
	kRsRolledBack,   // the BO is in VRAM as before, both views on it
	kRsBroken,       // a step reported success and the result was not there: everything kept, the feature off
	kRsUndecided,    // the check itself could not run (no memory for its mapping): the BO stays in system memory as the
	                 // redirect reported, the VRAM is kept too (never freed), the feature off
};
enum XeRevokeEvent : uint8_t {
	kReBlockOk,
	kReBlockFail,       // no live mapping found, or the redirect(NULL) failed
	kReCopyOk,
	kReCopyFail,        // the copy failed and has completed (or never ran)
	kReCopyLate,        // the copy did not complete in time: it may still write the new pages
	kReRemapOk,
	kReRemapFail,       // a page-table page could not be allocated
	kReRedirectOk,      // redirect(new backing) returned success and the map now names the new descriptor
	kReRedirectRefused, // it failed before the transpose: the map still names the old descriptor (unblocked, on VRAM)
	kReVerifyOk,
	kReVerifyFail,      // the map names the new descriptor but its object does not resolve to the new pages
	kReVerifyUnknown,   // no kernel mapping could be made to check it
};
// What the mechanism must do on a transition (bits).
enum : uint16_t {
	kRaUnblock = 1u << 0,    // redirect the blocked map to its own descriptor again (commits the blocking UPL)
	kRaGpuBack = 1u << 1,    // rewrite the GPU PTEs back to the VRAM
	kRaFreeVram = 1u << 2,   // the old VRAM goes (after the TLB invalidation)
	kRaFreeSys = 1u << 3,    // the new pages go (nothing maps them)
	kRaKeepSys = 1u << 4,    // the new pages are never freed (something may still reach them)
	kRaKeepVram = 1u << 5,   // the VRAM is held (a late copy job still reads it)
	kRaAdopt = 1u << 6,      // the BO now lives in the new pages; its map takes the new descriptor
	kRaLatchOff = 1u << 7,   // the mechanism misbehaved: no more revocations until the knob is written again
	kRaAdoptDesc = 1u << 8,  // the map takes the descriptor IOKit now has for it (the redirect re-parented the mapping)
	kRaKeepPagers = 1u << 9, // the old descriptor and the new pages' physical descriptor live as long as the BO (below)
};

// ─── who must stay alive after the redirect (barwin2, B580 14 Nov) ───────────────────────────────────────────────
// Every CPU fault on a device-pager VM object, phys_contiguous or not, asks the pager first (vm_fault_page:
// MUST_ASK_PAGER is true for a pager that is not the compressor; memory_object_data_request -> device_pager_data_request
// -> device_data_action), and device_data_action answers KERN_ABORTED - a SIGBUS in the faulting thread - once the
// descriptor that made the pager is freed (IOGeneralMemoryDescriptor::free clears reserved->dp.memory). The transpose
// moves the pagers with the objects: after it the user's object faults through the pager of the physical descriptor of
// the new pages (`phys`), and if the transpose failed, still through the old descriptor's (`old`). barwin released
// both right after the redirect: the check passed (its fault came first), then every later user fault on a revoked BO
// was a SIGBUS (barrevoke now: exit 138; the natural run passed because the BOs it revoked were other processes', which
// had not touched them yet). The descriptor whose pager the user's object may have must live as long as the mapping,
// that is as long as the BO (the map holds the BO until its mapping is gone).
enum : uint8_t {
	kRdBridge = 1u << 0,   // the bridge (when the map did not take it)
	kRdKmap = 1u << 1,     // the kernel mapping of the new pages
	kRdPhys = 1u << 2,     // the new pages' physical descriptor
	kRdOld = 1u << 3,      // the map's old descriptor (when the map took the bridge)
};
// Descriptors the mechanism may release when it is done, from the actions taken. The rest are kept with the BO (or
// still the map's).
static inline uint8_t xeRevokeReleasable(uint16_t acts) {
	uint8_t r = kRdKmap;   // the kernel mapping: nothing faults through it after the commit
	if (!(acts & kRaAdoptDesc)) r |= kRdBridge;   // the map did not take it
	if (!(acts & kRaKeepPagers)) r |= kRdPhys;
	if ((acts & kRaAdoptDesc) && !(acts & kRaKeepPagers)) r |= kRdOld;
	return r;
}
// The descriptors whose pager the user's object may fault through in final state s (after the actions acts): old only
// while no transpose can have happened, phys after a verified one, either when it is not known.
static inline uint8_t xeRevokePagerOwners(XeRevokeState s, uint16_t acts) {
	if (!(acts & kRaAdoptDesc)) return kRdOld;   // makeMapping never ran or failed before the transpose
	if (s == kRsDone) return kRdPhys;
	return kRdPhys | kRdOld;                     // Broken (transpose failed silently) or Undecided
}
struct XeRevokeStep {
	XeRevokeState next;
	uint16_t actions;
	bool valid;   // false: the event cannot happen in that state (a bug in the caller)
};
static inline XeRevokeStep xeRevokeNext(XeRevokeState s, XeRevokeEvent e) {
	switch (s) {
	case kRsStart:
		if (e == kReBlockOk) return {kRsBlocked, 0, true};
		if (e == kReBlockFail) return {kRsRolledBack, kRaFreeSys, true};
		break;
	case kRsBlocked:
		if (e == kReCopyOk) return {kRsCopied, 0, true};
		if (e == kReCopyFail) return {kRsRolledBack, kRaUnblock | kRaFreeSys, true};
		if (e == kReCopyLate) return {kRsRolledBack, kRaUnblock | kRaKeepSys | kRaKeepVram, true};
		break;
	case kRsCopied:
		if (e == kReRemapOk) return {kRsGpuMoved, 0, true};
		if (e == kReRemapFail) return {kRsRolledBack, kRaGpuBack | kRaUnblock | kRaFreeSys, true};
		break;
	case kRsGpuMoved:
		if (e == kReRedirectOk) return {kRsRedirected, 0, true};
		// unblocked by redirect() itself and still on the VRAM: the GPU goes back. The pages are kept: whatever made
		// makeMapping fail may have left a descriptor that names them, and this is the first sign the mechanism does
		// not work on this kernel
		if (e == kReRedirectRefused) return {kRsRolledBack, kRaGpuBack | kRaKeepSys | kRaLatchOff, true};
		break;
	case kRsRedirected:
		if (e == kReVerifyOk) return {kRsDone, kRaFreeVram | kRaAdopt | kRaAdoptDesc | kRaKeepPagers, true};
		// the transpose failed silently (xnu logs upl_transpose and carries on): the CPU is on the VRAM, the memory
		// references were swapped all the same, so the new pages may be named by the old descriptor: keep them
		if (e == kReVerifyFail) return {kRsBroken, kRaGpuBack | kRaKeepSys | kRaLatchOff | kRaAdoptDesc | kRaKeepPagers, true};
		// most likely it worked (the map names the new descriptor): the GPU stays with the CPU's probable view, and
		// neither memory is freed, so whichever the CPU reaches is still allocated
		if (e == kReVerifyUnknown)
			return {kRsUndecided, kRaKeepVram | kRaAdopt | kRaLatchOff | kRaAdoptDesc | kRaKeepPagers, true};
		break;
	default:
		break;
	}
	return {s, 0, false};
}
// Whether the CPU mapping is blocked in state s (every path must leave it unblocked).
static inline bool xeRevokeCpuBlocked(XeRevokeState s) { return s == kRsBlocked || s == kRsCopied || s == kRsGpuMoved; }
// Whether the GPU PTEs point at the new pages in state s.
static inline bool xeRevokeGpuOnSys(XeRevokeState s) {
	return s == kRsGpuMoved || s == kRsRedirected || s == kRsDone || s == kRsUndecided;
}
// Terminal states.
static inline bool xeRevokeFinal(XeRevokeState s) {
	return s == kRsDone || s == kRsRolledBack || s == kRsBroken || s == kRsUndecided;
}
