// SPDX-License-Identifier: MIT
// xemac: the user-space side of IntelXe.kext's xe interface. Mesa's ANV (and the tests) call these in place of
// open()/ioctl()/mmap() on a Linux DRM render node.
//
// Contract with the kext (kernel/IntelXe, class IntelXe, user client type 0):
//  - xemac_open() opens /dev/null read-only for a real file descriptor number and binds it to an IOServiceOpen()
//    connection; the other calls look the fd up in that table.
//  - xemac_ioctl(fd, request, arg) passes `request` exactly as Mesa's include/drm-uapi headers encode it on macOS
//    (<sys/ioccom.h>: IOC_IN/IOC_OUT, IOCPARM_LEN(request) bytes, group 'd', DRM_COMMAND_BASE + DRM_XE_* or a DRM core
//    number such as DRM_IOCTL_SYNCOBJ_CREATE). External method 0 takes scalars {request, arg address}; the kext copies
//    IOCPARM_LEN bytes in and out of the caller's memory itself and returns {errno} as one scalar output.
//    Pointers inside the structures (drm_xe_device_query.data, drm_xe_exec.syncs, ...) are user addresses of the
//    calling task. Returns 0, or -1 with errno set, like ioctl(2).
//  - DRM_IOCTL_XE_GEM_MMAP_OFFSET returns offset = (u64)handle << 12. xemac_mmap(addr, len, prot, flags, fd, offset)
//    maps memory type (offset >> 12) with IOConnectMapMemory64 (kIOMapAnywhere unless MAP_FIXED, write-combined for
//    VRAM and WC BOs as the kext reports through the map's cache mode). xemac_munmap() undoes it.
//  - Absolute timeouts (drm_syncobj_wait / drm_syncobj_timeline_wait timeout_nsec, xe user-fence waits with ABSTIME)
//    are compared with the kernel's mach_absolute_time() in nanoseconds, i.e. CLOCK_UPTIME_RAW. xemac_ioctl converts
//    values computed from CLOCK_MONOTONIC to that clock before passing them on (INT64_MAX stays "forever", 0 = poll).
//    Likewise DRM_XE_DEVICE_QUERY_ENGINE_CYCLES: the kext samples cpu_timestamp on CLOCK_UPTIME_RAW whatever clockid
//    asks for, and xemac_ioctl moves it to the requested clock.
//  - External method 1 returns the PCI identity as scalars {vendor, device, revision, subsystem vendor, subsystem
//    device, bus, device, function} for xemac_pci_info(), which stands in for libdrm's drmGetDevice2().
//  - Object fds (syncobj fds, sync files, PRIME): the kext cannot create a file descriptor, so for SYNCOBJ_HANDLE_TO_FD
//    and PRIME_HANDLE_TO_FD xemac_ioctl opens the clone device /dev/intelxeobj (INTELXE_OBJ_DEVICE) and passes that fd IN
//    args->fd; the kext binds the object to that open (one object per minor) and the fd is the caller's, as Linux's.
//    The imports and DRM_IOCTL_INTELXE_OBJ_INFO resolve an fd in the caller's fd table. Only when the kernel reports
//    INTELXE_CAP_FD_OBJECTS (xemac_fd_sharing); otherwise the ioctls go to the kext unchanged (ENOTTY), as before.
//  - XEMAC_FAKE=1 in the environment: no kext. A canned Battlemage G21 device (the values measured on the real card:
//    graphics 20.01, 20 Xe-cores, 8 EU each, 12 GB VRAM with a 256 MB CPU-visible window) answers device queries,
//    BOs are anonymous memory, VM binds succeed, execs complete at once. For bring-up of Mesa without the hardware.
//    BOs and syncobjs are shared by all fake devices of the process; object fds are AF_UNIX socketpair ends standing in
//    for opens of /dev/intelxeobj (released on their last close; process-local). XEMAC_FAKE_FDS=0: a kernel without them.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int xemac_open(void);
int xemac_close(int fd);
bool xemac_is_fd(int fd);
int xemac_ioctl(int fd, unsigned long request, void *arg);
void *xemac_mmap(void *addr, size_t len, int prot, int flags, int fd, uint64_t offset);
int xemac_munmap(void *addr, size_t len);
// xemac_munmap, but the range is left reserved (PROT_NONE anonymous memory) as VK_MEMORY_UNMAP_RESERVE_BIT_EXT asks,
// without a hole in between: on macOS 15 an mmap(MAP_FIXED) over a range that is partly unmapped kills the process
// with EXC_GUARD (GUARD_TYPE_VIRT_MEMORY, DEALLOC_GAP).
int xemac_munmap_reserve(void *addr, size_t len);

// fd sharing (uapi/intelxe_drm.h, INTELXE_CAP_FD_OBJECTS): which object fds the device offers, as XEMAC_FD_* bits; 0 for a
// kernel without them (and with XEMAC_FDS=0 in the environment). Asked once per device fd.
#define XEMAC_FD_SYNC 1u  // DRM syncobj fds and sync files: SYNCOBJ_HANDLE_TO_FD / SYNCOBJ_FD_TO_HANDLE
#define XEMAC_FD_PRIME 2u // BO fds: PRIME_HANDLE_TO_FD / PRIME_FD_TO_HANDLE
unsigned xemac_fd_sharing(int fd);
// The size of the BO an object fd holds (DRM_IOCTL_INTELXE_OBJ_INFO), where Linux reads lseek(dmabuf, 0, SEEK_END): an
// object fd is a character device here, whose SEEK_END is 0. -1 with errno set when obj_fd holds no BO.
int64_t xemac_obj_size(int fd, int obj_fd);

struct xemac_pci_info {
	uint16_t vendor, device, subvendor, subdevice;
	uint8_t revision, bus, dev, func;
};
int xemac_pci_info(int fd, struct xemac_pci_info *info);

// The GPU's sensors as one sample: the render GT's clocks and SLPC range, the package and VRAM temperatures, the card
// and package energy (microjoules) and average power, the power limits (struct intelxe_sensors, ../uapi/
// intelxe_sensors.h; the kext's debug.intelxe.sensors_raw sysctl, which anyone may read). Level Zero sysman over NEO
// reads its frequency, power and temperature data through this. With XEMAC_FAKE=1: canned values of the fake
// Battlemage (act 2100 MHz, request 2400, RP0 2850, RPn 400, RPe 1800, range 1200-2850; package 47 C, VRAM 52 C; the
// card's energy grows by 40 W of real time, the package's by 25 W; card PL1 190 W on, PL2 250 W off, package PL1
// 150 W on, crit 300 W), XEMAC_FAKE_SENSORS=0: a kext without the sysctl. fd -1: no device fd needed (the sysctl is
// global; XEMAC_FAKE decides fake or not). Returns 0, or -1 with errno: EBADF (not an xemac fd), ENOENT (a kext without
// debug.intelxe.sensors_raw), EPROTO (a struct version this xemac does not know). Fields the kext did not fill (an
// older, shorter struct) are 0.
struct intelxe_sensors;
int xemac_sensors(int fd, struct intelxe_sensors *out);

// Version 2 of the sample (a kext with engine activity) also carries per-engine busy and total ticks
// (INTELXE_SENSORS_ENGINES); the fake Battlemage's: RCS0 busy half the time, BCS0 a tenth, CCS0 a quarter, 19.2 MHz.

// The GPU frequency range SLPC runs with (Level Zero sysman's zesFrequencySetRange through min_freq / max_freq):
// which 0 = the floor (the kext's debug.intelxe.slpc_min), 1 = the ceiling (slpc_max); mhz 0 = the default (1200 MHz
// floor, RP0 ceiling), else clamped by the kext to RPn..RP0. The kext sends it to the GuC at once and keeps it for
// every later SLPC start. Root only. Returns 0, or -1 with errno: EPERM (not root), ENOENT (a kext without live range
// control, i.e. before sensors_raw version 2: its slpc_min/max only applied at SLPC start), EINVAL, EBADF.
// XEMAC_FAKE=1: the canned range changes instead (for any user, unless XEMAC_FAKE_ROOT=0).
int xemac_freq_set(int fd, int which, uint32_t mhz);
// Whether xemac_freq_set can work for this process: 1 when the kext has live range control and the process is root
// (fake: unless XEMAC_FAKE_ROOT=0), else 0.
int xemac_freq_can_set(int fd);

#define XEMAC_METHOD_IOCTL 0
#define XEMAC_METHOD_PCI_INFO 1

#ifdef __cplusplus
}
#endif
