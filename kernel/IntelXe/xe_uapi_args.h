// SPDX-License-Identifier: MIT
// The parts of the xe uAPI that are argument checks and arithmetic only: no device, no locks, no IOKit types, so
// tests/uapi_test.cpp builds them on the host (as xe_platform.cpp). Each function names the Linux function it follows
// (drivers/gpu/drm/xe, dl/linux-xe); xe_core.cpp calls them from the ioctls.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "uapi/xe_drm.h"

// Reads len bytes of the caller's memory at addr into dst: 0, or EFAULT (copyin in the kext, a fake memory in the test).
typedef int (*XeUserRead)(void *ctx, uint64_t addr, void *dst, size_t len);

// xe_bo.c gem_create_user_extensions -> gem_create_user_ext_set_property -> gem_create_set_pxp_type, on a device without
// PXP (Battlemage: bmg_desc has no has_pxp; xe_pxp_key_assign answers ENODEV). At most MAX_USER_EXTENSIONS (16) in the
// chain (E2BIG), each read once (EFAULT), base.pad 0 and name SET_PROPERTY (EINVAL), the property PXP_TYPE with pad 0
// (EINVAL); PXP_TYPE_NONE accepted, HWDRM ENODEV, any other type EINVAL.
int xeGemCreateExtensions(uint64_t ext, XeUserRead rd, void *ctx);

// xe_sync.c xe_sync_entry_parse, the checks that need no object: flags only SIGNAL and reserved 0 (EINVAL); syncobj
// types: no signal in LR mode (EOPNOTSUPP), a 32-bit handle (EINVAL), a nonzero timeline point (EINVAL); USER_FENCE:
// refused where the caller disallows it (EOPNOTSUPP: a VM_BIND without binds), never a wait (EOPNOTSUPP), 8-byte aligned
// (EINVAL). Other types EINVAL. The caller then looks the syncobj up (ENOENT) or, for a bind's user fence, reads the
// address once (user_fence_create's get_user: EFAULT).
enum : uint32_t {
	kXeSyncParseExec = 1u << 0,             // SYNC_PARSE_FLAG_EXEC
	kXeSyncParseLrMode = 1u << 1,           // SYNC_PARSE_FLAG_LR_MODE
	kXeSyncParseDisallowUfence = 1u << 2,   // SYNC_PARSE_FLAG_DISALLOW_USER_FENCE
};
int xeSyncEntryCheck(const drm_xe_sync &s, uint32_t flags);

// xe_vm.c xe_vm_create_ioctl, its argument checks: extensions and reserved zero, only ALL_DRM_XE_VM_CREATE_FLAGS,
// FAULT_MODE only with LR_MODE, NO_VM_OVERCOMMIT only with FAULT_MODE (all EINVAL). FAULT_MODE is refused as xe refuses
// it on a device without has_usm (EINVAL): there is no GPU page-fault service here. LR_MODE without it is accepted
// (lrmode: xe_lr.h). *lr: the VM is long-running.
int xeVmCreateCheck(const drm_xe_vm_create &v, bool *lr);

// xe_wait_user_fence.c xe_wait_user_fence_ioctl, its argument checks in its order: extensions, pad, pad2 and reserved
// 0; only the ABSTIME flag; op at most LTE; addr 8-byte aligned. All EINVAL.
int xeUfenceArgsCheck(const drm_xe_wait_user_fence &a);

// xe_wait_user_fence.c do_compare once the value is read: 0 when (rvalue & mask) OP (value & mask) holds, 1 when it does
// not, EINVAL for an unknown op.
int xeUfenceCompare(uint64_t rvalue, uint64_t value, uint64_t mask, uint16_t op);

// xe_wait_user_fence.c to_jiffies_timeout in nanoseconds of the caller's clock (now): the deadline, INT64_MAX = forever,
// 0 = compare once. A negative timeout waits forever and is set to INT64_MAX in a (MAX_SCHEDULE_TIMEOUT), so the time
// left written back stays huge; ABSTIME is a deadline on that clock (xemac converts CLOCK_MONOTONIC to it).
int64_t xeUfenceDeadline(drm_xe_wait_user_fence &a, uint64_t now);

// After the wait (any outcome): a relative timeout loses the time waited, never below 0; an absolute one stays.
void xeUfenceTimeoutLeft(drm_xe_wait_user_fence &a, uint64_t start, uint64_t now);
