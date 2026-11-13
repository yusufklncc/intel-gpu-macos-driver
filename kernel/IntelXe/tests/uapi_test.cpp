// SPDX-License-Identifier: MIT
// uapi_test: offline check of the xe uAPI argument checks (xe_uapi_args.cpp) against what Linux xe answers, no GPU needed.
//   1. DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY chains (xe_bo.c gem_create_user_extensions on a device without PXP):
//      the error codes, in xe's order, for every field xe looks at, the 16-extension limit and unreadable links
//   2. drm_xe_sync entries (xe_sync.c xe_sync_entry_parse): every type in exec and bind context, user fences included
//   3. DRM_IOCTL_XE_WAIT_USER_FENCE (xe_wait_user_fence.c): argument checks, every compare op under several masks
//      against a direct evaluation, the timeout arithmetic (relative, absolute, forever, poll, the time left)
//   4. DRM_IOCTL_XE_GEM_CREATE's argument checks (xe_bo.c xe_gem_create_ioctl): on BMG's memory regions the same
//      answers as the inline checks they replace, for every combination tried; on an integrated part (Lunar Lake: system
//      memory only) every VRAM placement refused
// c++ -std=c++17 -Wall -Wextra -I.. -o uapi_test uapi_test.cpp ../xe_uapi_args.cpp && ./uapi_test
#include <stdio.h>
#include <string.h>
#include <sys/errno.h>
#include "xe_uapi_args.h"

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); \
	printf("\n"); } } while (0)

// A fake user address space: 64 KB at kBase; anything else faults (EFAULT, as copy_from_user).
static constexpr uint64_t kBase = 0x10000000;
static uint8_t gMem[0x10000];
static uint32_t gReads;
static int fakeRead(void *, uint64_t addr, void *dst, size_t len) {
	gReads++;
	if (addr < kBase || addr + len > kBase + sizeof(gMem) || addr + len < addr) return EFAULT;
	memcpy(dst, gMem + (addr - kBase), len);
	return 0;
}
// extension i of a chain at kBase + i * 64
static drm_xe_ext_set_property *ext(uint32_t i) { return reinterpret_cast<drm_xe_ext_set_property *>(gMem + i * 64); }
static uint64_t extAddr(uint32_t i) { return kBase + i * 64; }
static void chain(uint32_t n, uint64_t pxp) {   // n good links, each PXP_TYPE = pxp
	memset(gMem, 0, sizeof(gMem));
	for (uint32_t i = 0; i < n; i++) {
		ext(i)->base.name = DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY;
		ext(i)->base.next_extension = i + 1 < n ? extAddr(i + 1) : 0;
		ext(i)->property = DRM_XE_GEM_CREATE_SET_PROPERTY_PXP_TYPE;
		ext(i)->value = pxp;
	}
}
static int run(uint64_t first) { return xeGemCreateExtensions(first, fakeRead, nullptr); }

static void gemCreate() {
	CHECK(run(0) == 0, "no chain");
	chain(1, DRM_XE_PXP_TYPE_NONE);
	CHECK(run(extAddr(0)) == 0, "PXP_TYPE NONE accepted");
	chain(1, DRM_XE_PXP_TYPE_HWDRM);
	CHECK(run(extAddr(0)) == ENODEV, "PXP_TYPE HWDRM: xe_pxp_key_assign without PXP is ENODEV, got %d", run(extAddr(0)));
	chain(1, 2);
	CHECK(run(extAddr(0)) == EINVAL, "an unknown PXP type is EINVAL");
	chain(1, ~0ull);
	CHECK(run(extAddr(0)) == EINVAL, "PXP type ~0 is EINVAL");
	// gem_create_user_extensions: base.pad, then the name
	chain(1, 0);
	ext(0)->base.pad = 1;
	CHECK(run(extAddr(0)) == EINVAL, "base.pad");
	chain(1, 0);
	ext(0)->base.name = 1;
	CHECK(run(extAddr(0)) == EINVAL, "an unknown extension name");
	// gem_create_user_ext_set_property: property in range, pad 0, property == SET_PROPERTY (0)
	chain(1, 0);
	ext(0)->property = 1;
	CHECK(run(extAddr(0)) == EINVAL, "an unknown property");
	chain(1, 0);
	ext(0)->pad = 1;
	CHECK(run(extAddr(0)) == EINVAL, "pad");
	chain(1, 0);
	ext(0)->reserved[0] = ext(0)->reserved[1] = 7;   // xe does not look at reserved here
	CHECK(run(extAddr(0)) == 0, "reserved is not checked by xe");
	// the bad pad wins over a bad value (xe tests the property before calling the setter)
	chain(1, 5);
	ext(0)->pad = 1;
	CHECK(run(extAddr(0)) == EINVAL, "pad before value");
	// MAX_USER_EXTENSIONS = 16: 16 links fine, the 17th E2BIG before it is read
	chain(16, 0);
	CHECK(run(extAddr(0)) == 0, "16 links");
	chain(17, 0);
	gReads = 0;
	CHECK(run(extAddr(0)) == E2BIG, "17 links: E2BIG");
	CHECK(gReads == 32, "17 links: 16 read twice each (base, then the property), the 17th not at all: %u reads", gReads);
	// a loop is caught by the same limit
	chain(2, 0);
	ext(1)->base.next_extension = extAddr(0);
	CHECK(run(extAddr(0)) == E2BIG, "a looping chain ends in E2BIG");
	// faults: the first link, a later link, a link whose property part is outside (the base is readable)
	CHECK(run(0x1000) == EFAULT, "unreadable first link");
	chain(2, 0);
	ext(0)->base.next_extension = 0x2000;
	CHECK(run(extAddr(0)) == EFAULT, "unreadable second link");
	memset(gMem, 0, sizeof(gMem));
	auto *tail = reinterpret_cast<drm_xe_user_extension *>(gMem + sizeof(gMem) - sizeof(drm_xe_user_extension));
	tail->name = DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY;
	CHECK(run(kBase + sizeof(gMem) - sizeof(drm_xe_user_extension)) == EFAULT, "base readable, property not: EFAULT");
	// an error in a later link after a good first one; HWDRM in link 2 stops the chain there
	chain(3, 0);
	ext(1)->value = DRM_XE_PXP_TYPE_HWDRM;
	ext(2)->base.pad = 1;
	CHECK(run(extAddr(0)) == ENODEV, "the first failing link decides");
}

static void syncEntries() {
	auto mk = [](uint32_t type, uint32_t flags, uint64_t addr, uint64_t point) {
		drm_xe_sync e = {};
		e.type = type;
		e.flags = flags;
		e.addr = addr;
		e.timeline_value = point;
		return e;
	};
	const uint32_t SIG = DRM_XE_SYNC_FLAG_SIGNAL, EXEC = kXeSyncParseExec, LR = kXeSyncParseLrMode,
		NOUF = kXeSyncParseDisallowUfence;
	// common: unknown flags, reserved
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, 2, 1, 0), EXEC) == EINVAL, "unknown sync flag");
	drm_xe_sync r = mk(DRM_XE_SYNC_TYPE_SYNCOBJ, 0, 1, 0);
	r.reserved[1] = 1;
	CHECK(xeSyncEntryCheck(r, EXEC) == EINVAL, "reserved");
	CHECK(xeSyncEntryCheck(mk(3, SIG, 1, 0), EXEC) == EINVAL, "unknown type");
	// syncobjs: a 32-bit handle, a timeline point, no signal in LR mode
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, SIG, 5, 0), EXEC) == 0, "binary signal");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, 0, 1ull << 32, 0), EXEC) == EINVAL, "handle above 32 bits");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, 0, 5, 0), EXEC) == EINVAL, "timeline point 0");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, SIG, 5, 9), 0) == 0, "timeline signal");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, SIG, 5, 0), EXEC | LR) == EOPNOTSUPP, "LR: syncobj signal");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_SYNCOBJ, 0, 5, 0), EXEC | LR) == 0, "LR: syncobj wait");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ, SIG, 5, 0), EXEC | LR) == EOPNOTSUPP,
		"LR is tested before the point");
	// user fences: signal only, 8-byte aligned, not in a bind without binds; the order of those tests
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x1000, 7), EXEC) == 0, "exec user fence");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x7fff00001000ull, 7), 0) == 0, "bind user fence, 64-bit");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, 0, 0x1000, 7), EXEC) == EOPNOTSUPP, "user fence wait");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x1004, 7), EXEC) == EINVAL, "unaligned");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0x1004, 7), NOUF) == EOPNOTSUPP, "disallowed first");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, 0, 0x1004, 7), EXEC) == EOPNOTSUPP, "wait before alignment");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, 2, 0x1004, 7), EXEC) == EINVAL, "flags before type checks");
	CHECK(xeSyncEntryCheck(mk(DRM_XE_SYNC_TYPE_USER_FENCE, SIG, 0, 0), EXEC | LR) == 0, "LR allows user fences");
}

static bool direct(uint64_t r, uint64_t v, uint64_t m, uint16_t op) {
	const uint64_t a = r & m, b = v & m;
	switch (op) {
		case 0: return a == b;
		case 1: return a != b;
		case 2: return a > b;
		case 3: return a >= b;
		case 4: return a < b;
		default: return a <= b;
	}
}

static void waitUfence() {
	drm_xe_wait_user_fence a = {};
	a.addr = 0x1000;
	CHECK(xeUfenceArgsCheck(a) == 0, "good args");
	a.extensions = 1;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "extensions");
	a = {}; a.pad = 1;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "pad");
	a = {}; a.pad2 = 1;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "pad2");
	a = {}; a.reserved[0] = 1;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "reserved");
	a = {}; a.flags = 2;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "unknown flag");
	a = {}; a.flags = DRM_XE_UFENCE_WAIT_FLAG_ABSTIME;
	CHECK(xeUfenceArgsCheck(a) == 0, "ABSTIME");
	a = {}; a.op = DRM_XE_UFENCE_WAIT_OP_LTE + 1;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "op above LTE");
	a = {}; a.op = DRM_XE_UFENCE_WAIT_OP_LTE;
	CHECK(xeUfenceArgsCheck(a) == 0, "op LTE");
	a = {}; a.addr = 0x1004;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "unaligned addr");
	a = {}; a.addr = 0x1004; a.op = 9;
	CHECK(xeUfenceArgsCheck(a) == EINVAL, "op and addr");

	// every op, values around the mask edges, every mask the uAPI names and two odd ones
	const uint64_t masks[] = {0xffu, 0xffffu, 0xffffffffu, ~0ull, 0xff00ff00ff00ff00ull, 0};
	const uint64_t vals[] = {0, 1, 0xfe, 0xff, 0x100, 0xffff, 0x10000, 0xffffffffull, 0x100000000ull, 0x8000000000000000ull,
		~0ull, 0x1234567890abcdefull};
	uint32_t n = 0;
	for (uint16_t op = 0; op <= DRM_XE_UFENCE_WAIT_OP_LTE; op++)
		for (uint64_t m : masks)
			for (uint64_t r : vals)
				for (uint64_t v : vals) {
					const int got = xeUfenceCompare(r, v, m, op), want = direct(r, v, m, op) ? 0 : 1;
					if (got != want) CHECK(false, "op %u mask %llx r %llx v %llx: %d, want %d", op, (unsigned long long)m,
						(unsigned long long)r, (unsigned long long)v, got, want);
					n++;
				}
	CHECK(n == 6 * 6 * 12 * 12, "compare cases %u", n);
	CHECK(xeUfenceCompare(1, 1, ~0ull, 6) == EINVAL, "unknown op");
	// the mask makes high bits irrelevant: a u32 fence that wrapped its upper half
	CHECK(xeUfenceCompare(0xdead00000005ull, 5, 0xffffffffu, DRM_XE_UFENCE_WAIT_OP_EQ) == 0, "u32 mask EQ");
	CHECK(xeUfenceCompare(0xdead00000005ull, 5, ~0ull, DRM_XE_UFENCE_WAIT_OP_EQ) == 1, "u64 mask EQ");
	CHECK(xeUfenceCompare(0, 5, 0, DRM_XE_UFENCE_WAIT_OP_EQ) == 0, "mask 0: everything equal");
	CHECK(xeUfenceCompare(0, 5, 0, DRM_XE_UFENCE_WAIT_OP_GT) == 1, "mask 0: nothing greater");

	// timeouts on a clock at 1e12 ns
	const uint64_t now = 1000000000000ull;
	a = {}; a.timeout = 0;
	CHECK(xeUfenceDeadline(a, now) == 0, "timeout 0: compare once");
	xeUfenceTimeoutLeft(a, now, now + 5);
	CHECK(a.timeout == 0, "poll: 0 left");
	a = {}; a.timeout = -1;
	CHECK(xeUfenceDeadline(a, now) == INT64_MAX && a.timeout == INT64_MAX, "negative: forever, field MAX_SCHEDULE_TIMEOUT");
	xeUfenceTimeoutLeft(a, now, now + 1000);
	CHECK(a.timeout == INT64_MAX - 1000, "forever: the time waited comes off MAX_SCHEDULE_TIMEOUT");
	a = {}; a.timeout = 5000000;
	CHECK(xeUfenceDeadline(a, now) == (int64_t)(now + 5000000), "relative 5 ms");
	xeUfenceTimeoutLeft(a, now, now + 2000000);
	CHECK(a.timeout == 3000000, "3 ms left after 2");
	a = {}; a.timeout = 5000000;
	xeUfenceDeadline(a, now);
	xeUfenceTimeoutLeft(a, now, now + 9000000);
	CHECK(a.timeout == 0, "overrun: 0 left, never negative");
	a = {}; a.timeout = INT64_MAX;
	CHECK(xeUfenceDeadline(a, now) == INT64_MAX - 1, "huge relative: very long, not forever");
	a = {}; a.flags = DRM_XE_UFENCE_WAIT_FLAG_ABSTIME; a.timeout = (int64_t)(now + 7);
	CHECK(xeUfenceDeadline(a, now) == (int64_t)(now + 7), "absolute in the future");
	xeUfenceTimeoutLeft(a, now, now + 100);
	CHECK(a.timeout == (int64_t)(now + 7), "absolute stays intact");
	a = {}; a.flags = DRM_XE_UFENCE_WAIT_FLAG_ABSTIME; a.timeout = 5;
	CHECK(xeUfenceDeadline(a, now) == (int64_t)(now + 1), "absolute in the past: one more look");
	a = {}; a.flags = DRM_XE_UFENCE_WAIT_FLAG_ABSTIME; a.timeout = -3;
	CHECK(xeUfenceDeadline(a, now) == INT64_MAX && a.timeout == INT64_MAX, "absolute negative: forever");
}

// 4. DRM_IOCTL_XE_GEM_CREATE's argument checks (xe_bo.c xe_gem_create_ioctl up to the VM lookup) per memory model.
// oldGemCreate is the check xe_core.cpp made inline before xeGemCreateCheck (2026-11-13, branch lnl), copied as it was:
// with BMG's mask (3) and graphics 20.01 the two give the same answer for every combination below.
static int oldGemCreate(const drm_xe_gem_create *g) {
	constexpr uint32_t kMemRegionMask = 3;   // xe->info.mem_region_mask: system (bit 0) and tile 0's VRAM (bit 1)
	constexpr uint32_t kFlags = DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING | DRM_XE_GEM_CREATE_FLAG_SCANOUT |
		DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM | DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION;
	if (g->pad[0] || g->pad[1] || g->pad[2] || g->reserved[0] || g->reserved[1]) return EINVAL;
	if ((g->placement & ~kMemRegionMask) || !g->placement) return EINVAL;   // at least one valid placement
	if (g->flags & ~kFlags) return EINVAL;
	if (g->handle) return EINVAL;
	if (!g->size) return EINVAL;
	if (g->size & 0xfff) return EINVAL;   // ~PAGE_MASK (size > SIZE_MAX cannot happen with a 64-bit size_t)
	const bool vramPl = g->placement & 2;   // bo_flags & XE_BO_FLAG_VRAM_MASK
	if ((g->flags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM) && !vramPl) return EINVAL;
	if (!g->cpu_caching || g->cpu_caching > DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
	if (vramPl && g->cpu_caching != DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
	if ((g->flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT) && g->cpu_caching == DRM_XE_GEM_CPU_CACHING_WB)
		return EINVAL;   // XE_BO_FLAG_FORCE_WC: display scan-out is never CPU-cache coherent
	return 0;
}

static void gemCreateArgs() {
	static const uint32_t kPlacements[] = {0, 1, 2, 3, 4, 5, 0x80000000u};
	static const uint32_t kFlagSets[] = {0, DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING, DRM_XE_GEM_CREATE_FLAG_SCANOUT,
		DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM, DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION,
		DRM_XE_GEM_CREATE_FLAG_SCANOUT | DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM, 1u << 4, 0xffffffffu};
	static const uint16_t kCaching[] = {0, DRM_XE_GEM_CPU_CACHING_WB, DRM_XE_GEM_CPU_CACHING_WC, 3};
	static const uint64_t kSizes[] = {0, 0x1000, 0x1001, 0x10000, 1ull << 40};
	uint32_t n = 0, same = 0;
	for (uint32_t pl : kPlacements)
		for (uint32_t fl : kFlagSets)
			for (uint16_t cc : kCaching)
				for (uint64_t sz : kSizes)
					for (int extra = 0; extra < 4; extra++) {
						drm_xe_gem_create g = {};
						g.placement = pl; g.flags = fl; g.cpu_caching = cc; g.size = sz;
						if (extra == 1) g.handle = 7;
						if (extra == 2) g.pad[1] = 1;
						if (extra == 3) g.reserved[0] = 1;
						const int want = oldGemCreate(&g), got = xeGemCreateCheck(g, 3, 2001);
						if (got != want)
							CHECK(false, "BMG placement 0x%x flags 0x%x caching %u size 0x%llx extra %d: %d, was %d", pl, fl, cc,
								(unsigned long long)sz, extra, got, want);
						else
							same++;
						n++;
						// integrated (xe->info.mem_region_mask 1): never a VRAM placement; otherwise the discrete answer
						const int igpu = xeGemCreateCheck(g, 1, 2004);
						const bool vramBit = pl & 2;
						if (vramBit && !g.pad[1] && !g.reserved[0]) CHECK(igpu == EINVAL, "LNL placement 0x%x: %d", pl, igpu);
						if (!vramBit) CHECK(igpu == want, "LNL placement 0x%x flags 0x%x: %d, BMG %d", pl, fl, igpu, want);
					}
	CHECK(same == n && n == 7 * 8 * 4 * 5 * 4, "BMG: %u of %u the same", same, n);

	// the integrated cases that matter to ANV on Lunar Lake: system-memory BOs, WB or WC
	drm_xe_gem_create g = {};
	g.size = 0x10000; g.placement = 1; g.cpu_caching = DRM_XE_GEM_CPU_CACHING_WB;
	CHECK(xeGemCreateCheck(g, 1, 2004) == 0, "LNL: system WB");
	g.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	CHECK(xeGemCreateCheck(g, 1, 2004) == 0, "LNL: system WC");
	g.flags = DRM_XE_GEM_CREATE_FLAG_SCANOUT;
	CHECK(xeGemCreateCheck(g, 1, 2004) == 0, "LNL: scan-out WC in system memory");
	g.cpu_caching = DRM_XE_GEM_CPU_CACHING_WB;
	CHECK(xeGemCreateCheck(g, 1, 2004) == EINVAL, "LNL: scan-out WB (FORCE_WC)");
	g.flags = DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM; g.cpu_caching = DRM_XE_GEM_CPU_CACHING_WC;
	CHECK(xeGemCreateCheck(g, 1, 2004) == EINVAL, "LNL: NEEDS_VISIBLE_VRAM without VRAM");
	g.flags = 0; g.placement = 3;
	CHECK(xeGemCreateCheck(g, 1, 2004) == EINVAL, "LNL: system + VRAM (Mesa's discrete placement)");
	g.placement = 1; g.flags = DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION;
	CHECK(xeGemCreateCheck(g, 1, 2004) == 0, "LNL: NO_COMPRESSION (graphics 20)");
	CHECK(xeGemCreateCheck(g, 1, 1270) == EOPNOTSUPP, "MTL: NO_COMPRESSION below graphics 20");
	g.pad[0] = 1;
	CHECK(xeGemCreateCheck(g, 1, 1270) == EINVAL, "pad before NO_COMPRESSION");
}

int main() {
	gemCreate();
	gemCreateArgs();
	syncEntries();
	waitUfence();
	printf("%d checks, %d failed\n", checks, fails);
	return fails ? 1 : 0;
}
