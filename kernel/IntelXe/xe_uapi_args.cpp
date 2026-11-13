// SPDX-License-Identifier: MIT
// xe uAPI argument checks (xe_uapi_args.h), function by function after Linux drivers/gpu/drm/xe (MIT, dl/linux-xe):
// xe_bo.c, xe_sync.c, xe_vm.c, xe_wait_user_fence.c.
#include "xe_uapi_args.h"
#include <sys/errno.h>

// xe_bo.c MAX_USER_EXTENSIONS
static constexpr uint32_t kMaxUserExtensions = 16;

// gem_create_set_pxp_type: NONE is the default; HWDRM needs xe_pxp_key_assign, which is ENODEV with PXP off (xe->pxp
// NULL on every platform without has_pxp)
static int gemCreateSetPxpType(uint64_t value) {
	if (value == DRM_XE_PXP_TYPE_NONE) return 0;
	if (value != DRM_XE_PXP_TYPE_HWDRM) return EINVAL;
	return ENODEV;
}

// gem_create_user_ext_set_property: gem_create_set_property_funcs has the one entry PXP_TYPE (index 0)
static int gemCreateUserExtSetProperty(uint64_t extension, XeUserRead rd, void *ctx) {
	drm_xe_ext_set_property ext;
	if (rd(ctx, extension, &ext, sizeof(ext))) return EFAULT;
	// xe's three tests: below ARRAY_SIZE(gem_create_set_property_funcs), pad 0, and equal to the extension's name
	if (ext.property >= 1 || ext.pad || ext.property != DRM_XE_GEM_CREATE_EXTENSION_SET_PROPERTY) return EINVAL;
	return gemCreateSetPxpType(ext.value);   // gem_create_set_property_funcs[DRM_XE_GEM_CREATE_SET_PROPERTY_PXP_TYPE]
}

// gem_create_user_extensions: recursive in xe (ext_number + 1 per link), a loop here
int xeGemCreateExtensions(uint64_t ext, XeUserRead rd, void *ctx) {
	for (uint32_t n = 0; ext; n++) {
		if (n >= kMaxUserExtensions) return E2BIG;
		drm_xe_user_extension u;
		if (rd(ctx, ext, &u, sizeof(u))) return EFAULT;
		if (u.pad || u.name >= 1) return EINVAL;   // gem_create_user_extension_funcs has one entry, SET_PROPERTY (0)
		if (int err = gemCreateUserExtSetProperty(ext, rd, ctx)) return err;
		ext = u.next_extension;
	}
	return 0;
}

// xe_gem_create_ioctl up to xe_vm_lookup. The VRAM placement is bit 1 (tile 0's VRAM: XE_BO_FLAG_VRAM_MASK after the
// shift by ffs(XE_BO_FLAG_SYSTEM) - 1).
int xeGemCreateCheck(const drm_xe_gem_create &g, uint32_t memRegionMask, uint16_t graphicsVerx100) {
	constexpr uint32_t kFlags = DRM_XE_GEM_CREATE_FLAG_DEFER_BACKING | DRM_XE_GEM_CREATE_FLAG_SCANOUT |
		DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM | DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION;
	if (g.pad[0] || g.pad[1] || g.pad[2] || g.reserved[0] || g.reserved[1]) return EINVAL;
	if ((g.placement & ~memRegionMask) || !g.placement) return EINVAL;   // at least one valid placement
	if (g.flags & ~kFlags) return EINVAL;
	if (g.handle) return EINVAL;
	if (!g.size) return EINVAL;
	if (g.size & 0xfff) return EINVAL;   // ~PAGE_MASK (size > SIZE_MAX cannot happen with a 64-bit size_t)
	if ((g.flags & DRM_XE_GEM_CREATE_FLAG_NO_COMPRESSION) && graphicsVerx100 < 2000) return EOPNOTSUPP;
	const bool vram = g.placement & 2;   // bo_flags & XE_BO_FLAG_VRAM_MASK
	if ((g.flags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM) && !vram) return EINVAL;
	if (!g.cpu_caching || g.cpu_caching > DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
	if (vram && g.cpu_caching != DRM_XE_GEM_CPU_CACHING_WC) return EINVAL;
	if ((g.flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT) && g.cpu_caching == DRM_XE_GEM_CPU_CACHING_WB)
		return EINVAL;   // XE_BO_FLAG_FORCE_WC: display scan-out is never CPU-cache coherent
	return 0;
}

// xe_sync_entry_parse, up to the object lookups
int xeSyncEntryCheck(const drm_xe_sync &s, uint32_t flags) {
	if ((s.flags & ~(uint32_t)DRM_XE_SYNC_FLAG_SIGNAL) || s.reserved[0] || s.reserved[1]) return EINVAL;
	const bool signal = s.flags & DRM_XE_SYNC_FLAG_SIGNAL;
	switch (s.type) {
		case DRM_XE_SYNC_TYPE_SYNCOBJ:
			if ((flags & kXeSyncParseLrMode) && signal) return EOPNOTSUPP;
			if (s.addr >> 32) return EINVAL;   // upper_32_bits(sync_in.addr): the handle shares a union with addr
			return 0;
		case DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ:
			if ((flags & kXeSyncParseLrMode) && signal) return EOPNOTSUPP;
			if (s.addr >> 32) return EINVAL;
			if (!s.timeline_value) return EINVAL;
			return 0;
		case DRM_XE_SYNC_TYPE_USER_FENCE:
			if (flags & kXeSyncParseDisallowUfence) return EOPNOTSUPP;
			if (!signal) return EOPNOTSUPP;
			if (s.addr & 0x7) return EINVAL;
			return 0;
		default:
			return EINVAL;
	}
}

// xe_vm_create_ioctl up to xe_vm_create: every check answers EINVAL, so their order does not matter
int xeVmCreateCheck(const drm_xe_vm_create &v, bool *lr) {
	constexpr uint32_t kAll = DRM_XE_VM_CREATE_FLAG_SCRATCH_PAGE | DRM_XE_VM_CREATE_FLAG_LR_MODE |
		DRM_XE_VM_CREATE_FLAG_FAULT_MODE | DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT;   // ALL_DRM_XE_VM_CREATE_FLAGS
	*lr = false;
	if (v.extensions || v.reserved[0] || v.reserved[1] || (v.flags & ~kAll)) return EINVAL;
	if ((v.flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE) && !(v.flags & DRM_XE_VM_CREATE_FLAG_LR_MODE)) return EINVAL;
	if ((v.flags & DRM_XE_VM_CREATE_FLAG_NO_VM_OVERCOMMIT) && !(v.flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE)) return EINVAL;
	if (v.flags & DRM_XE_VM_CREATE_FLAG_FAULT_MODE) return EINVAL;   // FAULT_MODE && !xe->info.has_usm
	*lr = (v.flags & DRM_XE_VM_CREATE_FLAG_LR_MODE) != 0;
	return 0;
}

// xe_wait_user_fence_ioctl: VALID_FLAGS = ABSTIME, MAX_OP = LTE
int xeUfenceArgsCheck(const drm_xe_wait_user_fence &a) {
	if (a.extensions || a.pad || a.pad2 || a.reserved[0] || a.reserved[1]) return EINVAL;
	if (a.flags & ~(uint16_t)DRM_XE_UFENCE_WAIT_FLAG_ABSTIME) return EINVAL;
	if (a.op > DRM_XE_UFENCE_WAIT_OP_LTE) return EINVAL;
	if (a.addr & 0x7) return EINVAL;
	return 0;
}

// do_compare after copy_from_user
int xeUfenceCompare(uint64_t rvalue, uint64_t value, uint64_t mask, uint16_t op) {
	bool passed;
	switch (op) {
		case DRM_XE_UFENCE_WAIT_OP_EQ: passed = (rvalue & mask) == (value & mask); break;
		case DRM_XE_UFENCE_WAIT_OP_NEQ: passed = (rvalue & mask) != (value & mask); break;
		case DRM_XE_UFENCE_WAIT_OP_GT: passed = (rvalue & mask) > (value & mask); break;
		case DRM_XE_UFENCE_WAIT_OP_GTE: passed = (rvalue & mask) >= (value & mask); break;
		case DRM_XE_UFENCE_WAIT_OP_LT: passed = (rvalue & mask) < (value & mask); break;
		case DRM_XE_UFENCE_WAIT_OP_LTE: passed = (rvalue & mask) <= (value & mask); break;
		default: return EINVAL;   // XE_WARN_ON("Not possible"): the ioctl checked op
	}
	return passed ? 0 : 1;
}

// to_jiffies_timeout: negative = forever (and the field becomes MAX_SCHEDULE_TIMEOUT); 0 = no wait; else relative or
// absolute. xe never waits less than one jiffy for a nonzero timeout (`return timeout ?: 1`): a deadline already passed
// still gets one more look (now + 1).
int64_t xeUfenceDeadline(drm_xe_wait_user_fence &a, uint64_t now) {
	if (a.timeout < 0) {
		a.timeout = INT64_MAX;
		return INT64_MAX;
	}
	if (a.timeout == 0) return 0;
	const uint64_t n = now > (uint64_t)INT64_MAX - 2 ? (uint64_t)INT64_MAX - 2 : now;
	if (a.flags & DRM_XE_UFENCE_WAIT_FLAG_ABSTIME) return (uint64_t)a.timeout <= n ? (int64_t)(n + 1) : a.timeout;
	// beyond MAX_SCHEDULE_TIMEOUT is "very long", not forever (MAX_SCHEDULE_TIMEOUT - 1)
	if ((uint64_t)a.timeout >= (uint64_t)INT64_MAX - n) return INT64_MAX - 1;
	return (int64_t)(n + (uint64_t)a.timeout);
}

// the tail of xe_wait_user_fence_ioctl
void xeUfenceTimeoutLeft(drm_xe_wait_user_fence &a, uint64_t start, uint64_t now) {
	if (a.flags & DRM_XE_UFENCE_WAIT_FLAG_ABSTIME) return;
	const uint64_t spent = now > start ? now - start : 0;
	a.timeout = spent >= (uint64_t)a.timeout ? 0 : a.timeout - (int64_t)spent;
}
