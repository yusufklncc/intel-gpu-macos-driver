// xemac.c: IntelXe.kext's xe interface for user space (see xemac.h for the contract), plus the XEMAC_FAKE=1 device.
// SPDX-License-Identifier: MIT
//
// XEMAC_TRACE=1 logs every ioctl to stderr.

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <time.h>
#include <unistd.h>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <mach/mach.h>
#include <mach/mach_vm.h>

#include "drm-uapi/drm.h"
#include "drm-uapi/xe_drm.h"
#include "../uapi/intelxe_drm.h"   // IntelXe.kext extensions (surface fences)
#include "../uapi/intelxe_sensors.h" // debug.intelxe.sensors_raw (xemac_sensors)
#include "xemac.h"

static bool window_background(void);

#define MAX_FD 1024

struct fake;

// INTELXE_CAP_STATUS_PAGE: exec queues seen unbanned, and at which ban epoch (anv asks for every queue's ban after every
// wait: vk_device_check_status). While the epoch has not moved, a queue seen unbanned is still unbanned.
#define XEMAC_BAN_CACHE 16
struct file {
	io_connect_t conn; // 0 for the fake device
	struct fake *fake;
	int status_state;                      // 0 not looked at yet, 1 mapped, -1 not available
	const volatile uint64_t *ban_epoch;    // in the kext's status page
	mach_vm_address_t status_addr;
	uint32_t status_type;
	struct xemac_ban { uint32_t queue; uint64_t epoch; } unbanned[XEMAC_BAN_CACHE];
	unsigned unbanned_count;
	int fd_sharing;                        // XEMAC_FD_* + 1 once asked (0: not asked yet)
};

static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static struct file *files[MAX_FD];
static int trace = -1;

static struct file *file_of(int fd)
{
	if (fd < 0 || fd >= MAX_FD)
		return NULL;
	pthread_mutex_lock(&lock);
	struct file *f = files[fd];
	pthread_mutex_unlock(&lock);
	return f;
}

static bool tracing(void)
{
	if (trace < 0)
		trace = getenv("XEMAC_TRACE") && atoi(getenv("XEMAC_TRACE")) > 0;
	return trace;
}

static int64_t clock_ns(clockid_t id)
{
	struct timespec ts;
	clock_gettime(id, &ts);
	return (int64_t)ts.tv_sec * 1000000000 + ts.tv_nsec;
}

// Mesa's absolute timeouts are on CLOCK_MONOTONIC; the kext's are on CLOCK_UPTIME_RAW (mach_absolute_time).
static int64_t monotonic_to_uptime(int64_t t)
{
	if (t <= 0 || t == INT64_MAX)
		return t;
	int64_t r = clock_ns(CLOCK_UPTIME_RAW) + (t - clock_ns(CLOCK_MONOTONIC));
	return r > 0 ? r : 1;
}

static const char *ioctl_name(unsigned long request)
{
	switch (request) {
#define N(x) case DRM_IOCTL_##x: return #x;
	N(VERSION) N(GET_CAP) N(GEM_CLOSE) N(PRIME_HANDLE_TO_FD) N(PRIME_FD_TO_HANDLE)
	N(SYNCOBJ_CREATE) N(SYNCOBJ_DESTROY) N(SYNCOBJ_HANDLE_TO_FD) N(SYNCOBJ_FD_TO_HANDLE) N(SYNCOBJ_WAIT)
	N(SYNCOBJ_RESET) N(SYNCOBJ_SIGNAL) N(SYNCOBJ_TIMELINE_WAIT) N(SYNCOBJ_QUERY) N(SYNCOBJ_TRANSFER)
	N(SYNCOBJ_TIMELINE_SIGNAL)
	N(XE_DEVICE_QUERY) N(XE_GEM_CREATE) N(XE_GEM_MMAP_OFFSET) N(XE_VM_CREATE) N(XE_VM_DESTROY) N(XE_VM_BIND)
	N(XE_EXEC_QUEUE_CREATE) N(XE_EXEC_QUEUE_DESTROY) N(XE_EXEC_QUEUE_GET_PROPERTY) N(XE_EXEC)
	N(XE_WAIT_USER_FENCE) N(XE_OBSERVATION) N(XE_MADVISE) N(XE_VM_GET_PROPERTY)
	N(INTELXE_SURFACE_FENCE_ATTACH) N(INTELXE_SURFACE_FENCE_IMPORT) N(INTELXE_SURFACE_FENCE_WAIT) N(INTELXE_OBJ_INFO)
#undef N
	default: return "?";
	}
}

static void trace_ioctl(int fd, unsigned long request, const void *arg, int ret, int err)
{
	char extra[192] = "";
	if (request == DRM_IOCTL_XE_DEVICE_QUERY) {
		const struct drm_xe_device_query *q = arg;
		snprintf(extra, sizeof(extra), " query=%u size=%u%s", q->query, q->size, q->data ? "" : " (size probe)");
	} else if (request == DRM_IOCTL_XE_EXEC) {
		const struct drm_xe_exec *e = arg;
		const struct drm_xe_sync *s = (const void *)(uintptr_t)e->syncs;
		int n = snprintf(extra, sizeof(extra), " queue=%u bb=%u addr=0x%llx syncs(type/flags/value)",
				 e->exec_queue_id, e->num_batch_buffer, (unsigned long long)e->address);
		for (uint32_t i = 0; i < e->num_syncs && n < (int)sizeof(extra) - 24; i++)
			n += snprintf(extra + n, sizeof(extra) - n, " %u/%u/%llu", s[i].type, s[i].flags,
				      (unsigned long long)s[i].timeline_value);
	} else if (request == DRM_IOCTL_XE_VM_BIND) {
		const struct drm_xe_vm_bind *b = arg;
		const struct drm_xe_sync *s = (const void *)(uintptr_t)b->syncs;
		snprintf(extra, sizeof(extra), " vm=%u q=%u binds=%u op=%u obj=%u addr=0x%llx range=0x%llx pat=%u fl=0x%x "
			 "sync0=%u/%u/%llu", b->vm_id, b->exec_queue_id, b->num_binds, b->num_binds == 1 ? b->bind.op : 0,
			 b->num_binds == 1 ? b->bind.obj : 0, (unsigned long long)(b->num_binds == 1 ? b->bind.addr : 0),
			 (unsigned long long)(b->num_binds == 1 ? b->bind.range : 0),
			 b->num_binds == 1 ? b->bind.pat_index : 0, b->num_binds == 1 ? b->bind.flags : 0,
			 b->num_syncs ? s[0].type : 0, b->num_syncs ? s[0].flags : 0,
			 (unsigned long long)(b->num_syncs ? s[0].timeline_value : 0));
	} else if (request == DRM_IOCTL_SYNCOBJ_WAIT) {
		const struct drm_syncobj_wait *w = arg;
		snprintf(extra, sizeof(extra), " count=%u flags=0x%x timeout=%lld", w->count_handles, w->flags,
			 (long long)w->timeout_nsec);
	} else if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
		const struct drm_syncobj_timeline_wait *w = arg;
		snprintf(extra, sizeof(extra), " count=%u flags=0x%x timeout=%lld point0=%llu", w->count_handles,
			 w->flags, (long long)w->timeout_nsec,
			 (unsigned long long)*(const uint64_t *)(uintptr_t)w->points);
	} else if (request == DRM_IOCTL_SYNCOBJ_CREATE) {
		const struct drm_syncobj_create *c = arg;
		snprintf(extra, sizeof(extra), " flags=0x%x -> %u", c->flags, c->handle);
	} else if (request == DRM_IOCTL_GET_CAP) {
		const struct drm_get_cap *c = arg;
		snprintf(extra, sizeof(extra), " cap=0x%llx -> %llu", (unsigned long long)c->capability,
			 (unsigned long long)c->value);
	} else if (request == DRM_IOCTL_XE_VM_CREATE) {
		const struct drm_xe_vm_create *c = arg;
		snprintf(extra, sizeof(extra), " flags=0x%x -> %u", c->flags, c->vm_id);
	} else if (request == DRM_IOCTL_XE_EXEC_QUEUE_CREATE) {
		const struct drm_xe_exec_queue_create *c = arg;
		const struct drm_xe_engine_class_instance *in = (const void *)(uintptr_t)c->instances;
		int n = snprintf(extra, sizeof(extra), " vm=%u width=%u class=%u x%u flags=0x%x props", c->vm_id, c->width,
				 c->num_placements ? in[0].engine_class : 0, c->num_placements, c->flags);
		for (uint64_t e = c->extensions; e && n < (int)sizeof(extra) - 16;) {
			const struct drm_xe_ext_set_property *p = (const void *)(uintptr_t)e;
			n += snprintf(extra + n, sizeof(extra) - n, " %u=%llu", p->property, (unsigned long long)p->value);
			e = p->base.next_extension;
		}
		snprintf(extra + n, sizeof(extra) - n, " -> %u", c->exec_queue_id);
	} else if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD || request == DRM_IOCTL_PRIME_FD_TO_HANDLE) {
		const struct drm_prime_handle *p = arg;
		snprintf(extra, sizeof(extra), " handle=%u flags=0x%x fd=%d", p->handle, p->flags, p->fd);
	} else if (request == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD || request == DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE) {
		const struct drm_syncobj_handle *h = arg;
		snprintf(extra, sizeof(extra), " handle=%u flags=0x%x fd=%d", h->handle, h->flags, h->fd);
	} else if (request == DRM_IOCTL_INTELXE_OBJ_INFO) {
		const struct drm_intelxe_obj_info *o = arg;
		snprintf(extra, sizeof(extra), " fd=%d -> kind %u size 0x%llx status %u", o->fd, o->kind,
			 (unsigned long long)o->size, o->status);
	} else if (request == DRM_IOCTL_XE_GEM_CREATE) {
		const struct drm_xe_gem_create *g = arg;
		snprintf(extra, sizeof(extra), " size=0x%llx placement=0x%x flags=0x%x caching=%u vm=%u -> %u",
			 (unsigned long long)g->size, g->placement, g->flags, g->cpu_caching, g->vm_id, g->handle);
	}
	fprintf(stderr, "xemac[%d] %s%s -> %d%s%s\n", fd, ioctl_name(request), extra, ret, ret ? " " : "",
		ret ? strerror(err) : "");
}

// ---- fake device ----------------------------------------------------------------------------------------------

// GuC hwconfig table of the real card (bmg_hwconfig.bin, 576 bytes): {key, length, value} triples.
static const uint32_t bmg_hwconfig[] = {
	1, 1, 5, 70, 1, 20, 71, 1, 8, 4, 1, 2, 63, 1, 1, 64, 1, 768, 10, 1, 2, 11, 1, 4, 12, 1, 1, 13, 1, 1075908608,
	73, 1, 160, 15, 1, 8, 16, 1, 546, 17, 1, 312, 18, 1, 336, 19, 1, 546, 21, 1, 96, 66, 1, 16, 23, 1, 1, 24, 1, 2,
	25, 1, 2, 26, 1, 2, 27, 1, 1, 81, 1, 1, 68, 1, 768, 29, 1, 64, 30, 1, 5888, 31, 1, 0, 32, 1, 0, 33, 1, 0,
	34, 1, 1548, 35, 1, 2, 36, 1, 1952, 37, 1, 50, 38, 1, 4480, 77, 1, 0, 78, 1, 1548, 79, 1, 4, 80, 1, 1952,
	39, 1, 32768, 41, 1, 8192, 42, 1, 64, 44, 1, 192, 45, 1, 324, 46, 1, 2048, 47, 1, 127, 59, 1, 0, 60, 1, 26,
};

#define FAKE_DEVICE 0xe20b
#define FAKE_REVISION 0
#define FAKE_VRAM_SIZE 0x300000000ull
#define FAKE_VRAM_VISIBLE 0x10000000ull
#define FAKE_REF_CLOCK 19200000 // not measured on the card
#define FAKE_VA_BITS 48

// RCS0, BCS0, CCS0 (BCS8 exists but xe keeps it for USM). XEMAC_FAKE_ENGINES=rcs,bcs,ccs picks a subset.
static const struct drm_xe_engine_class_instance all_engines[] = {
	{DRM_XE_ENGINE_CLASS_RENDER, 0, 0, 0},
	{DRM_XE_ENGINE_CLASS_COPY, 0, 0, 0},
	{DRM_XE_ENGINE_CLASS_COMPUTE, 0, 0, 0},
};
static const char *const engine_names[] = {"rcs", "bcs", "ccs"};
static struct drm_xe_engine_class_instance fake_engines[3];
static size_t num_fake_engines;

static void fake_init_engines(void)
{
	const char *list = getenv("XEMAC_FAKE_ENGINES");
	num_fake_engines = 0;
	for (size_t i = 0; i < 3; i++)
		if (!list || strstr(list, engine_names[i]))
			fake_engines[num_fake_engines++] = all_engines[i];
}

// BOs and syncobjs are reference counted objects shared by every fake device of the process: a handle holds one
// reference, and so does an object fd bound to it (fd sharing, below), as GEM objects and drm_syncobjs are on Linux.
struct fake_bo {
	int refs;
	uint64_t size;
	mach_vm_address_t mem;
	uint32_t placement, vm_id;
};

struct fake_syncobj {
	int refs;
	bool signaled;
	uint64_t point;
};

struct fake {
	struct fake_bo **bos; // index = handle; NULL = free
	uint32_t num_bos;
	struct fake_syncobj **syncobjs;
	uint32_t num_syncobjs;
	uint64_t vms, queues; // bitmasks of live ids 1..63
};

static pthread_mutex_t fake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t fake_cond = PTHREAD_COND_INITIALIZER;

static uint64_t host_memory(void)
{
	uint64_t mem = 0;
	size_t len = sizeof(mem);
	sysctlbyname("hw.memsize", &mem, &len, NULL, 0);
	return mem;
}

static void *query_engines(uint32_t *size)
{
	size_t n = num_fake_engines;
	struct drm_xe_query_engines *q = calloc(1, *size = sizeof(*q) + n * sizeof(q->engines[0]));
	q->num_engines = n;
	for (size_t i = 0; i < n; i++)
		q->engines[i].instance = fake_engines[i];
	return q;
}

static void *query_mem_regions(uint32_t *size)
{
	struct drm_xe_query_mem_regions *q = calloc(1, *size = sizeof(*q) + 2 * sizeof(q->mem_regions[0]));
	q->num_mem_regions = 2;
	q->mem_regions[0] = (struct drm_xe_mem_region){
		.mem_class = DRM_XE_MEM_REGION_CLASS_SYSMEM, .instance = 0, .min_page_size = 4096,
		.total_size = host_memory(), .cpu_visible_size = 0};
	q->mem_regions[1] = (struct drm_xe_mem_region){
		.mem_class = DRM_XE_MEM_REGION_CLASS_VRAM, .instance = 1, .min_page_size = 4096,
		.total_size = FAKE_VRAM_SIZE, .cpu_visible_size = FAKE_VRAM_VISIBLE};
	return q;
}

static void *query_config(uint32_t *size)
{
	struct drm_xe_query_config *q = calloc(1, *size = sizeof(*q) + 5 * sizeof(q->info[0]));
	q->num_params = 5;
	q->info[DRM_XE_QUERY_CONFIG_REV_AND_DEVICE_ID] = (FAKE_REVISION << 16) | FAKE_DEVICE;
	q->info[DRM_XE_QUERY_CONFIG_FLAGS] = DRM_XE_QUERY_CONFIG_FLAG_HAS_VRAM;
	q->info[DRM_XE_QUERY_CONFIG_MIN_ALIGNMENT] = 4096;
	q->info[DRM_XE_QUERY_CONFIG_VA_BITS] = FAKE_VA_BITS;
	q->info[DRM_XE_QUERY_CONFIG_MAX_EXEC_QUEUE_PRIORITY] = 1; // DRM_SCHED_PRIORITY_NORMAL
	return q;
}

static void *query_gt_list(uint32_t *size)
{
	struct drm_xe_query_gt_list *q = calloc(1, *size = sizeof(*q) + 2 * sizeof(q->gt_list[0]));
	q->num_gt = 2;
	q->gt_list[0] = (struct drm_xe_gt){
		.type = DRM_XE_QUERY_GT_TYPE_MAIN, .tile_id = 0, .gt_id = 0, .reference_clock = FAKE_REF_CLOCK,
		.near_mem_regions = 1 << 1, .far_mem_regions = 1 << 0, .ip_ver_major = 20, .ip_ver_minor = 1,
		.ip_ver_rev = 0};
	q->gt_list[1] = (struct drm_xe_gt){
		.type = DRM_XE_QUERY_GT_TYPE_MEDIA, .tile_id = 0, .gt_id = 1, .reference_clock = FAKE_REF_CLOCK,
		.near_mem_regions = 1 << 1, .far_mem_regions = 1 << 0, .ip_ver_major = 13, .ip_ver_minor = 1,
		.ip_ver_rev = 1};
	return q;
}

static uint8_t *topo_add(uint8_t *p, uint16_t type, uint32_t num_bytes, uint64_t mask)
{
	struct drm_xe_query_topology_mask *t = (void *)p;
	t->gt_id = 0;
	t->type = type;
	t->num_bytes = num_bytes;
	for (uint32_t i = 0; i < num_bytes && i < 8; i++)
		t->mask[i] = mask >> (8 * i);
	return p + sizeof(*t) + num_bytes;
}

static void *query_topology(uint32_t *size)
{
	// GT0 only, mask sizes as xe's: 3 fuse dwords for DSS masks, 64-bit L3 bank and EU masks.
	*size = 4 * sizeof(struct drm_xe_query_topology_mask) + 12 + 12 + 8 + 8;
	uint8_t *q = calloc(1, *size), *p = q;
	p = topo_add(p, DRM_XE_TOPO_DSS_GEOMETRY, 12, 0x000fffff);
	p = topo_add(p, DRM_XE_TOPO_DSS_COMPUTE, 12, 0x000fffff);
	p = topo_add(p, DRM_XE_TOPO_L3_BANK, 8, 0x00ffffff); // 24 x 768 KB = 18 MB; not read from the card
	topo_add(p, DRM_XE_TOPO_SIMD16_EU_PER_DSS, 8, 0xff);
	return q;
}

static void *query_hwconfig(uint32_t *size)
{
	void *q = malloc(*size = sizeof(bmg_hwconfig));
	memcpy(q, bmg_hwconfig, sizeof(bmg_hwconfig));
	return q;
}

static int fake_engine_cycles(struct drm_xe_query_engine_cycles *c)
{
	if (c->eci.gt_id != 0)
		return EINVAL;
	switch (c->clockid) {
	case CLOCK_MONOTONIC: case CLOCK_MONOTONIC_RAW: case CLOCK_REALTIME: case CLOCK_UPTIME_RAW: break;
	default: return EINVAL;
	}
	// The kext reports cpu_timestamp on CLOCK_UPTIME_RAW; xemac_ioctl moves it to c->clockid.
	int64_t now = clock_ns(CLOCK_UPTIME_RAW);
	c->cpu_timestamp = now;
	c->engine_cycles = (uint64_t)((__int128)now * FAKE_REF_CLOCK / 1000000000) & ((1ull << 36) - 1);
	c->cpu_delta = 0;
	c->width = 36;
	return 0;
}

static int fake_device_query(struct drm_xe_device_query *q)
{
	void *data = NULL;
	uint32_t size = 0;
	switch (q->query) {
	case DRM_XE_DEVICE_QUERY_ENGINES: data = query_engines(&size); break;
	case DRM_XE_DEVICE_QUERY_MEM_REGIONS: data = query_mem_regions(&size); break;
	case DRM_XE_DEVICE_QUERY_CONFIG: data = query_config(&size); break;
	case DRM_XE_DEVICE_QUERY_GT_LIST: data = query_gt_list(&size); break;
	case DRM_XE_DEVICE_QUERY_HWCONFIG: data = query_hwconfig(&size); break;
	case DRM_XE_DEVICE_QUERY_GT_TOPOLOGY: data = query_topology(&size); break;
	case DRM_XE_DEVICE_QUERY_ENGINE_CYCLES: {
		struct drm_xe_query_engine_cycles c;
		if (q->size == 0) { q->size = sizeof(c); return 0; }
		if (q->size != sizeof(c)) return EINVAL;
		memcpy(&c, (void *)(uintptr_t)q->data, sizeof(c));
		int err = fake_engine_cycles(&c);
		if (!err)
			memcpy((void *)(uintptr_t)q->data, &c, sizeof(c));
		return err;
	}
	case DRM_XE_DEVICE_QUERY_UC_FW_VERSION: {
		struct drm_xe_query_uc_fw_version v;
		if (q->size == 0) { q->size = sizeof(v); return 0; }
		if (q->size != sizeof(v)) return EINVAL;
		memcpy(&v, (void *)(uintptr_t)q->data, sizeof(v));
		if (v.uc_type != XE_QUERY_UC_TYPE_GUC_SUBMISSION)
			return ENODEV; // no HuC
		v.branch_ver = 0, v.major_ver = 1, v.minor_ver = 38, v.patch_ver = 1; // GuC 70.72.1
		memcpy((void *)(uintptr_t)q->data, &v, sizeof(v));
		return 0;
	}
	default: return ENODEV; // PXP_STATUS, OA_UNITS, EU_STALL
	}
	int err = 0;
	if (q->size == 0)
		q->size = size;
	else if (q->size < size)
		err = EINVAL;
	else
		memcpy((void *)(uintptr_t)q->data, data, size);
	free(data);
	return err;
}

static struct fake_syncobj *syncobj(struct fake *f, uint32_t handle)
{
	return handle && handle < f->num_syncobjs ? f->syncobjs[handle] : NULL;
}

static bool satisfied(const struct fake_syncobj *s, uint64_t point)
{
	return point ? s->point >= point : s->signaled;
}

static void signal_syncobj(struct fake_syncobj *s, uint64_t point)
{
	s->signaled = true;
	if (point > s->point)
		s->point = point;
}

static void fake_syncobj_put(struct fake_syncobj *s)
{
	if (s && --s->refs == 0)
		free(s);
}

// A new handle of f for s (one more reference).
static int fake_syncobj_add(struct fake *f, struct fake_syncobj *s, uint32_t *handle)
{
	uint32_t h = 1;
	while (h < f->num_syncobjs && f->syncobjs[h])
		h++;
	if (h >= f->num_syncobjs) {
		uint32_t n = f->num_syncobjs ? f->num_syncobjs * 2 : 64;
		struct fake_syncobj **t = realloc(f->syncobjs, n * sizeof(*t));
		if (!t)
			return ENOMEM;
		memset(t + f->num_syncobjs, 0, (n - f->num_syncobjs) * sizeof(*t));
		f->syncobjs = t, f->num_syncobjs = n;
	}
	s->refs++;
	f->syncobjs[h] = s;
	*handle = h;
	return 0;
}

static int fake_syncobj_create(struct fake *f, struct drm_syncobj_create *a)
{
	struct fake_syncobj *s = calloc(1, sizeof(*s));
	if (!s)
		return ENOMEM;
	s->signaled = a->flags & DRM_SYNCOBJ_CREATE_SIGNALED;
	int err = fake_syncobj_add(f, s, &a->handle);
	if (err)
		free(s);
	return err;
}

// DRM_IOCTL_SYNCOBJ_WAIT and _TIMELINE_WAIT. Execs complete at submission, so a point that is not signaled has not
// been submitted either: EINVAL without WAIT_FOR_SUBMIT, as drm_syncobj does; otherwise wait for another thread.
static int fake_syncobj_wait(struct fake *f, const uint32_t *handles, const uint64_t *points, uint32_t count,
			     int64_t timeout, uint32_t flags, uint32_t *first_signaled)
{
	if (count == 0)
		return EINVAL;
	for (;;) {
		uint32_t done = 0, first = UINT32_MAX;
		for (uint32_t i = 0; i < count; i++) {
			struct fake_syncobj *s = syncobj(f, handles[i]);
			if (!s)
				return ENOENT;
			if (satisfied(s, points ? points[i] : 0)) {
				done++;
				if (first == UINT32_MAX)
					first = i;
			} else if (!(flags & (DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT | DRM_SYNCOBJ_WAIT_FLAGS_WAIT_AVAILABLE))) {
				return EINVAL;
			}
		}
		if ((flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) ? done == count : done > 0) {
			if (first_signaled)
				*first_signaled = first;
			return 0;
		}
		int64_t left = timeout == INT64_MAX ? INT64_MAX : timeout - clock_ns(CLOCK_UPTIME_RAW);
		if (left <= 0)
			return ETIME;
		if (left > 1000000000)
			left = 1000000000;
		struct timespec ts = {left / 1000000000, left % 1000000000};
		pthread_cond_timedwait_relative_np(&fake_cond, &fake_lock, &ts);
	}
}

static int fake_process_syncs(struct fake *f, uint64_t syncs, uint32_t num_syncs, bool user_ptr_fences)
{
	const struct drm_xe_sync *s = (const void *)(uintptr_t)syncs;
	for (uint32_t i = 0; i < num_syncs; i++) {
		if (s[i].type == DRM_XE_SYNC_TYPE_USER_FENCE) {
			if (s[i].flags & DRM_XE_SYNC_FLAG_SIGNAL && user_ptr_fences)
				*(volatile uint64_t *)(uintptr_t)s[i].addr = s[i].timeline_value;
			continue;
		}
		if (s[i].type != DRM_XE_SYNC_TYPE_SYNCOBJ && s[i].type != DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ)
			return EINVAL;
		struct fake_syncobj *so = syncobj(f, s[i].handle);
		if (!so)
			return ENOENT;
		uint64_t point = s[i].type == DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ ? s[i].timeline_value : 0;
		if (!(s[i].flags & DRM_XE_SYNC_FLAG_SIGNAL) && !satisfied(so, point))
			return EINVAL; // waiting on something never submitted
	}
	for (uint32_t i = 0; i < num_syncs; i++) {
		if (s[i].type == DRM_XE_SYNC_TYPE_USER_FENCE || !(s[i].flags & DRM_XE_SYNC_FLAG_SIGNAL))
			continue;
		signal_syncobj(syncobj(f, s[i].handle),
			       s[i].type == DRM_XE_SYNC_TYPE_TIMELINE_SYNCOBJ ? s[i].timeline_value : 0);
	}
	pthread_cond_broadcast(&fake_cond);
	return 0;
}

static struct fake_bo *fake_bo(struct fake *f, uint32_t handle)
{
	return handle && handle < f->num_bos ? f->bos[handle] : NULL;
}

static void fake_bo_put(struct fake_bo *bo)
{
	if (bo && --bo->refs == 0) {
		mach_vm_deallocate(mach_task_self(), bo->mem, bo->size);
		free(bo);
	}
}

// A new handle of f for bo (one more reference).
static int fake_bo_add(struct fake *f, struct fake_bo *bo, uint32_t *handle)
{
	uint32_t h = 1;
	while (h < f->num_bos && f->bos[h])
		h++;
	if (h >= f->num_bos) {
		uint32_t n = f->num_bos ? f->num_bos * 2 : 256;
		struct fake_bo **t = realloc(f->bos, n * sizeof(*t));
		if (!t)
			return ENOMEM;
		memset(t + f->num_bos, 0, (n - f->num_bos) * sizeof(*t));
		f->bos = t, f->num_bos = n;
	}
	bo->refs++;
	f->bos[h] = bo;
	*handle = h;
	return 0;
}

static int fake_gem_create(struct fake *f, struct drm_xe_gem_create *a)
{
	if (!a->size || a->size & 4095 || !a->placement || a->placement & ~3u)
		return EINVAL;
	if (a->cpu_caching != DRM_XE_GEM_CPU_CACHING_WB && a->cpu_caching != DRM_XE_GEM_CPU_CACHING_WC)
		return EINVAL;
	if (a->placement & 2 && a->cpu_caching == DRM_XE_GEM_CPU_CACHING_WB)
		return EINVAL; // xe: VRAM BOs are WC on the CPU side
	if (a->vm_id && !(a->vm_id < 64 && f->vms & (1ull << a->vm_id)))
		return ENOENT;
	struct fake_bo *bo = calloc(1, sizeof(*bo));
	if (!bo)
		return ENOMEM;
	mach_vm_address_t mem = 0;
	if (mach_vm_allocate(mach_task_self(), &mem, a->size, VM_FLAGS_ANYWHERE) != KERN_SUCCESS) {
		free(bo);
		return ENOMEM;
	}
	*bo = (struct fake_bo){.size = a->size, .mem = mem, .placement = a->placement, .vm_id = a->vm_id};
	int err = fake_bo_add(f, bo, &a->handle);
	if (err) {
		bo->refs = 1;
		fake_bo_put(bo);
	}
	return err;
}

static int fake_vm_bind(struct fake *f, struct drm_xe_vm_bind *a)
{
	if (!(a->vm_id < 64 && f->vms & (1ull << a->vm_id)))
		return ENOENT;
	if (a->exec_queue_id && !(a->exec_queue_id < 64 && f->queues & (1ull << a->exec_queue_id)))
		return ENOENT;
	const struct drm_xe_vm_bind_op *ops =
		a->num_binds > 1 ? (const void *)(uintptr_t)a->vector_of_binds : &a->bind;
	for (uint32_t i = 0; i < a->num_binds; i++) {
		const struct drm_xe_vm_bind_op *op = &ops[i];
		struct fake_bo *bo = fake_bo(f, op->obj);
		switch (op->op) {
		case DRM_XE_VM_BIND_OP_MAP:
			if (op->flags & DRM_XE_VM_BIND_FLAG_NULL)
				break;
			if (!bo)
				return ENOENT;
			if (op->obj_offset + op->range > bo->size || (bo->vm_id && bo->vm_id != a->vm_id))
				return EINVAL;
			break;
		case DRM_XE_VM_BIND_OP_UNMAP_ALL:
			if (!bo)
				return ENOENT;
			continue;
		case DRM_XE_VM_BIND_OP_UNMAP: case DRM_XE_VM_BIND_OP_MAP_USERPTR: case DRM_XE_VM_BIND_OP_PREFETCH:
			break;
		default:
			return EINVAL;
		}
		if (!op->range || (op->addr | op->range) & 4095 || op->addr + op->range > (1ull << FAKE_VA_BITS))
			return EINVAL;
	}
	return fake_process_syncs(f, a->syncs, a->num_syncs, true);
}

static int fake_exec_queue_create(struct fake *f, struct drm_xe_exec_queue_create *a)
{
	if (!(a->vm_id < 64 && f->vms & (1ull << a->vm_id)))
		return ENOENT;
	if (a->width != 1 || !a->num_placements)
		return EINVAL;
	const struct drm_xe_engine_class_instance *in = (const void *)(uintptr_t)a->instances;
	for (uint32_t i = 0; i < a->num_placements; i++) {
		bool found = in[i].engine_class == DRM_XE_ENGINE_CLASS_VM_BIND;
		for (size_t e = 0; e < num_fake_engines; e++)
			found |= in[i].engine_class == fake_engines[e].engine_class &&
				 in[i].engine_instance == fake_engines[e].engine_instance && in[i].gt_id == fake_engines[e].gt_id;
		if (!found)
			return EINVAL;
	}
	for (uint64_t ext = a->extensions; ext;) {
		const struct drm_xe_ext_set_property *p = (const void *)(uintptr_t)ext;
		if (p->base.name != DRM_XE_EXEC_QUEUE_EXTENSION_SET_PROPERTY)
			return EINVAL;
		ext = p->base.next_extension;
	}
	uint32_t id = 1;
	while (id < 64 && f->queues & (1ull << id))
		id++;
	if (id == 64)
		return ENOSPC;
	f->queues |= 1ull << id;
	a->exec_queue_id = id;
	return 0;
}

// ---- fake object fds -------------------------------------------------------------------------------------------
//
// The fake device's stand-in for the clone device INTELXE_OBJ_DEVICE (uapi/intelxe_drm.h): one "minor" per object fd, the
// fd being one end of an AF_UNIX socketpair whose other end (the keeper) stays here. As an open of the clone device it
// is a real file: dup, fork and SCM_RIGHTS carry it, and it is identified by its open file, not by its number: a
// socket's st_ino (so_gencnt) is unique among live sockets and the same through dup and SCM_RIGHTS. Its last close is
// seen on the keeper (EOF): fake_minors_sweep() then drops the minor and the object reference it held, as the kext's
// d_close will. The sweep runs before every object fd operation and on device close, so a release is late at most until
// then. Process-local: another process cannot import these (the fake objects live in this process anyway).
struct fake_minor {
	uint64_t ino;  // st_ino of the caller's end
	int keeper;    // our end
	uint32_t kind; // INTELXE_OBJ_KIND_*
	struct fake_bo *bo;           // KIND_BO
	struct fake_syncobj *syncobj; // KIND_SYNCOBJ
	uint32_t status;              // KIND_SYNC_FILE: INTELXE_OBJ_SIGNALED (fake fences have all signaled)
};
static struct fake_minor *fake_minors;
static uint32_t num_fake_minors, cap_fake_minors;

static bool fake_fds_enabled(void)
{
	static int on = -1;
	if (on < 0) {
		const char *e = getenv("XEMAC_FAKE_FDS");   // XEMAC_FAKE_FDS=0: a kernel without fd sharing
		on = !e || atoi(e) > 0;
	}
	return on;
}

static bool keeper_orphaned(int keeper)
{
	struct pollfd p = {.fd = keeper, .events = POLLIN};
	char c;
	return poll(&p, 1, 0) > 0 && recv(keeper, &c, 1, MSG_PEEK | MSG_DONTWAIT) == 0;
}

static void fake_minor_release(struct fake_minor *m)
{
	if (tracing())
		fprintf(stderr, "xemac: object fd (minor %llu, kind %u) released\n", (unsigned long long)m->ino, m->kind);
	close(m->keeper);
	fake_bo_put(m->bo);
	fake_syncobj_put(m->syncobj);
}

// Under fake_lock.
static void fake_minors_sweep(void)
{
	for (uint32_t i = 0; i < num_fake_minors;) {
		if (keeper_orphaned(fake_minors[i].keeper)) {
			fake_minor_release(&fake_minors[i]);
			fake_minors[i] = fake_minors[--num_fake_minors];
		} else {
			i++;
		}
	}
}

// open(INTELXE_OBJ_DEVICE): a new unbound minor; returns the caller's fd.
static int fake_obj_open(bool cloexec)
{
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return -1;
	struct stat st;
	fcntl(sv[0], F_SETFD, FD_CLOEXEC);
	if (cloexec)
		fcntl(sv[1], F_SETFD, FD_CLOEXEC);
	int nosig = 1;
	setsockopt(sv[0], SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
	setsockopt(sv[1], SOL_SOCKET, SO_NOSIGPIPE, &nosig, sizeof(nosig));
	if (fstat(sv[1], &st))
		goto fail;
	pthread_mutex_lock(&fake_lock);
	fake_minors_sweep();
	if (num_fake_minors == cap_fake_minors) {
		uint32_t n = cap_fake_minors ? cap_fake_minors * 2 : 64;
		struct fake_minor *t = realloc(fake_minors, n * sizeof(*t));
		if (!t) {
			pthread_mutex_unlock(&fake_lock);
			errno = ENOMEM;
			goto fail;
		}
		fake_minors = t, cap_fake_minors = n;
	}
	fake_minors[num_fake_minors++] = (struct fake_minor){.ino = st.st_ino, .keeper = sv[0],
							      .kind = INTELXE_OBJ_KIND_NONE};
	pthread_mutex_unlock(&fake_lock);
	return sv[1];
fail:
	close(sv[0]);
	close(sv[1]);
	return -1;
}

// The kext's fd resolution (file_vnode + vnode_specrdev on the caller's fd table): the minor behind fd, or NULL with
// *err = EBADF (not an open fd) / EINVAL (not one of ours). Under fake_lock.
static struct fake_minor *fake_minor_of(int fd, int *err)
{
	struct stat st;
	if (fd < 0 || fstat(fd, &st)) {
		*err = EBADF;
		return NULL;
	}
	fake_minors_sweep();
	if (S_ISSOCK(st.st_mode))
		for (uint32_t i = 0; i < num_fake_minors; i++)
			if (fake_minors[i].ino == st.st_ino)
				return &fake_minors[i];
	*err = EINVAL;
	return NULL;
}

// args->fd: the unbound minor xemac_ioctl opened for the export.
static struct fake_minor *fake_minor_unbound(int fd, int *err)
{
	struct fake_minor *m = fake_minor_of(fd, err);
	if (m && m->kind != INTELXE_OBJ_KIND_NONE) {
		*err = EBUSY;
		return NULL;
	}
	return m;
}

static int fake_prime_handle_to_fd(struct fake *f, struct drm_prime_handle *a)
{
	if (a->flags & ~(uint32_t)(DRM_CLOEXEC | DRM_RDWR))
		return EINVAL;
	struct fake_bo *bo = fake_bo(f, a->handle);
	if (!bo)
		return ENOENT;
	if (bo->vm_id)
		return EPERM; // xe_gem_prime_export: a VM-private BO cannot be shared
	int err;
	struct fake_minor *m = fake_minor_unbound(a->fd, &err);
	if (!m)
		return err;
	m->kind = INTELXE_OBJ_KIND_BO;
	m->bo = bo;
	bo->refs++;
	return 0;
}

static int fake_prime_fd_to_handle(struct fake *f, struct drm_prime_handle *a)
{
	int err;
	struct fake_minor *m = fake_minor_of(a->fd, &err);
	if (!m)
		return err;
	if (m->kind != INTELXE_OBJ_KIND_BO)
		return EINVAL;
	for (uint32_t h = 1; h < f->num_bos; h++)
		if (f->bos[h] == m->bo) { // one handle per object per client
			a->handle = h;
			return 0;
		}
	return fake_bo_add(f, m->bo, &a->handle);
}

static bool has_fence(const struct fake_syncobj *s)
{
	return s->signaled || s->point; // fake fences signal at submission: "has a fence" = "has signaled"
}

static int fake_syncobj_handle_to_fd(struct fake *f, struct drm_syncobj_handle *a)
{
	if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_HANDLE_TO_FD_FLAGS_EXPORT_SYNC_FILE))
		return EINVAL;
	struct fake_syncobj *s = syncobj(f, a->handle);
	if (!s)
		return a->flags ? ENOENT : EINVAL; // drm_syncobj_find_fence / drm_syncobj_handle_to_fd
	if (a->flags && !has_fence(s))
		return EINVAL;
	int err;
	struct fake_minor *m = fake_minor_unbound(a->fd, &err);
	if (!m)
		return err;
	if (a->flags) {
		m->kind = INTELXE_OBJ_KIND_SYNC_FILE;
		m->status = INTELXE_OBJ_SIGNALED;
	} else {
		m->kind = INTELXE_OBJ_KIND_SYNCOBJ;
		m->syncobj = s;
		s->refs++;
	}
	return 0;
}

static int fake_syncobj_fd_to_handle(struct fake *f, struct drm_syncobj_handle *a)
{
	if (a->pad || (a->flags & ~(uint32_t)DRM_SYNCOBJ_FD_TO_HANDLE_FLAGS_IMPORT_SYNC_FILE))
		return EINVAL;
	int err;
	struct fake_minor *m = fake_minor_of(a->fd, &err);
	if (!m)
		return EINVAL; // drm_syncobj_fd_to_handle / sync_file_get_fence: EINVAL for an fd that is not open, too (WAS EBADF)
	if (a->flags) {
		if (m->kind != INTELXE_OBJ_KIND_SYNC_FILE)
			return EINVAL;
		struct fake_syncobj *s = syncobj(f, a->handle);
		if (!s)
			return ENOENT;
		signal_syncobj(s, 0); // the file's fence, signaled, replaces the syncobj's
		pthread_cond_broadcast(&fake_cond);
		return 0;
	}
	if (m->kind != INTELXE_OBJ_KIND_SYNCOBJ)
		return EINVAL;
	return fake_syncobj_add(f, m->syncobj, &a->handle); // a new handle each time, as drm_syncobj_fd_to_handle
}

static int fake_obj_info(struct drm_intelxe_obj_info *a)
{
	if (a->pad)
		return EINVAL;
	int err;
	struct fake_minor *m = fake_minor_of(a->fd, &err);
	if (!m)
		return err;
	a->kind = m->kind;
	a->size = m->kind == INTELXE_OBJ_KIND_BO ? m->bo->size : 0;
	a->status = m->status;
	return 0;
}

static int fake_ioctl(struct fake *f, unsigned long request, void *arg)
{
	switch (request) {
	case DRM_IOCTL_VERSION: {
		struct drm_version *v = arg;
		static const char *const s[3] = {"xe", "0", "Intel Xe2 Graphics"};
		char *bufs[3] = {v->name, v->date, v->desc};
		__kernel_size_t *lens[3] = {&v->name_len, &v->date_len, &v->desc_len};
		v->version_major = 1, v->version_minor = 1, v->version_patchlevel = 0;
		for (int i = 0; i < 3; i++) {
			size_t n = strlen(s[i]);
			if (bufs[i] && *lens[i])
				memcpy(bufs[i], s[i], n < *lens[i] ? n : *lens[i]);
			*lens[i] = n;
		}
		return 0;
	}
	case DRM_IOCTL_GET_CAP: {
		struct drm_get_cap *c = arg;
		if (c->capability == DRM_CAP_SYNCOBJ || c->capability == DRM_CAP_SYNCOBJ_TIMELINE ||
		    c->capability == INTELXE_CAP_SURFACE_FENCES)
			c->value = 1;
		else if (c->capability == DRM_CAP_PRIME)
			c->value = fake_fds_enabled() ? DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT : 0;
		else if (c->capability == INTELXE_CAP_FD_OBJECTS && fake_fds_enabled())
			c->value = 1;
		else
			return EINVAL;
		return 0;
	}
	case DRM_IOCTL_PRIME_HANDLE_TO_FD:
		return fake_fds_enabled() ? fake_prime_handle_to_fd(f, arg) : ENOTTY;
	case DRM_IOCTL_PRIME_FD_TO_HANDLE:
		return fake_fds_enabled() ? fake_prime_fd_to_handle(f, arg) : ENOTTY;
	case DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD:
		return fake_fds_enabled() ? fake_syncobj_handle_to_fd(f, arg) : ENOTTY;
	case DRM_IOCTL_SYNCOBJ_FD_TO_HANDLE:
		return fake_fds_enabled() ? fake_syncobj_fd_to_handle(f, arg) : ENOTTY;
	case DRM_IOCTL_INTELXE_OBJ_INFO:
		return fake_fds_enabled() ? fake_obj_info(arg) : ENOTTY;
	case DRM_IOCTL_GEM_CLOSE: {
		uint32_t h = ((struct drm_gem_close *)arg)->handle;
		struct fake_bo *bo = fake_bo(f, h);
		if (!bo)
			return EINVAL;
		f->bos[h] = NULL;
		fake_bo_put(bo);
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_CREATE:
		return fake_syncobj_create(f, arg);
	case DRM_IOCTL_SYNCOBJ_DESTROY: {
		uint32_t h = ((struct drm_syncobj_destroy *)arg)->handle;
		struct fake_syncobj *s = syncobj(f, h);
		if (!s)
			return EINVAL;
		f->syncobjs[h] = NULL;
		fake_syncobj_put(s);
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_WAIT: {
		struct drm_syncobj_wait *w = arg;
		return fake_syncobj_wait(f, (const uint32_t *)(uintptr_t)w->handles, NULL, w->count_handles,
					 w->timeout_nsec, w->flags, &w->first_signaled);
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT: {
		struct drm_syncobj_timeline_wait *w = arg;
		return fake_syncobj_wait(f, (const uint32_t *)(uintptr_t)w->handles, (const uint64_t *)(uintptr_t)w->points,
					 w->count_handles, w->timeout_nsec, w->flags, &w->first_signaled);
	}
	case DRM_IOCTL_SYNCOBJ_RESET:
	case DRM_IOCTL_SYNCOBJ_SIGNAL: {
		struct drm_syncobj_array *a = arg;
		const uint32_t *h = (const void *)(uintptr_t)a->handles;
		for (uint32_t i = 0; i < a->count_handles; i++)
			if (!syncobj(f, h[i]))
				return ENOENT;
		for (uint32_t i = 0; i < a->count_handles; i++) {
			if (request == DRM_IOCTL_SYNCOBJ_RESET)
				syncobj(f, h[i])->signaled = false;
			else
				signal_syncobj(syncobj(f, h[i]), 0);
		}
		pthread_cond_broadcast(&fake_cond);
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_TIMELINE_SIGNAL:
	case DRM_IOCTL_SYNCOBJ_QUERY: {
		struct drm_syncobj_timeline_array *a = arg;
		const uint32_t *h = (const void *)(uintptr_t)a->handles;
		uint64_t *p = (void *)(uintptr_t)a->points;
		for (uint32_t i = 0; i < a->count_handles; i++)
			if (!syncobj(f, h[i]))
				return ENOENT;
		for (uint32_t i = 0; i < a->count_handles; i++) {
			if (request == DRM_IOCTL_SYNCOBJ_QUERY)
				p[i] = syncobj(f, h[i])->point;
			else
				signal_syncobj(syncobj(f, h[i]), p[i]);
		}
		pthread_cond_broadcast(&fake_cond);
		return 0;
	}
	case DRM_IOCTL_SYNCOBJ_TRANSFER: {
		struct drm_syncobj_transfer *t = arg;
		struct fake_syncobj *src = syncobj(f, t->src_handle), *dst = syncobj(f, t->dst_handle);
		if (!src || !dst)
			return ENOENT;
		if (!satisfied(src, t->src_point))
			return EINVAL;
		signal_syncobj(dst, t->dst_point);
		pthread_cond_broadcast(&fake_cond);
		return 0;
	}
	case DRM_IOCTL_XE_DEVICE_QUERY:
		return fake_device_query(arg);
	case DRM_IOCTL_XE_GEM_CREATE:
		return fake_gem_create(f, arg);
	case DRM_IOCTL_XE_GEM_MMAP_OFFSET: {
		struct drm_xe_gem_mmap_offset *m = arg;
		if (!fake_bo(f, m->handle))
			return ENOENT;
		m->offset = (uint64_t)m->handle << 12;
		return 0;
	}
	case DRM_IOCTL_XE_VM_CREATE: {
		struct drm_xe_vm_create *c = arg;
		uint32_t id = 1;
		while (id < 64 && f->vms & (1ull << id))
			id++;
		if (id == 64)
			return ENOSPC;
		f->vms |= 1ull << id;
		c->vm_id = id;
		return 0;
	}
	case DRM_IOCTL_XE_VM_DESTROY: {
		uint32_t id = ((struct drm_xe_vm_destroy *)arg)->vm_id;
		if (!(id < 64 && f->vms & (1ull << id)))
			return ENOENT;
		f->vms &= ~(1ull << id);
		return 0;
	}
	case DRM_IOCTL_XE_VM_BIND:
		return fake_vm_bind(f, arg);
	case DRM_IOCTL_XE_VM_GET_PROPERTY: {
		struct drm_xe_vm_get_property *p = arg;
		if (!(p->vm_id < 64 && f->vms & (1ull << p->vm_id)))
			return ENOENT;
		if (p->property != DRM_XE_VM_GET_PROPERTY_FAULTS)
			return EINVAL;
		p->size = 0;
		return 0;
	}
	case DRM_IOCTL_XE_EXEC_QUEUE_CREATE:
		return fake_exec_queue_create(f, arg);
	case DRM_IOCTL_XE_EXEC_QUEUE_DESTROY: {
		uint32_t id = ((struct drm_xe_exec_queue_destroy *)arg)->exec_queue_id;
		if (!(id < 64 && f->queues & (1ull << id)))
			return ENOENT;
		f->queues &= ~(1ull << id);
		return 0;
	}
	case DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY: {
		struct drm_xe_exec_queue_get_property *p = arg;
		if (!(p->exec_queue_id < 64 && f->queues & (1ull << p->exec_queue_id)))
			return ENOENT;
		if (p->property != DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN)
			return EINVAL;
		p->value = 0;
		return 0;
	}
	case DRM_IOCTL_XE_EXEC: {
		struct drm_xe_exec *e = arg;
		if (!(e->exec_queue_id < 64 && f->queues & (1ull << e->exec_queue_id)))
			return ENOENT;
		if (e->num_batch_buffer > 1 || (e->num_batch_buffer && !e->address))
			return EINVAL;
		return fake_process_syncs(f, e->syncs, e->num_syncs, false);
	}
	case DRM_IOCTL_XE_WAIT_USER_FENCE: {
		// Bind fences (CPU addresses) are written at the bind here, so a wait is a compare. Exec fences sit at GPU
		// addresses nothing writes (no batch runs): such a wait reports ETIME at once. Intel's compute
		// runtime waits on bind fences this way while it creates a device.
		struct drm_xe_wait_user_fence *w = arg;
		if (w->addr & 7 || w->op > DRM_XE_UFENCE_WAIT_OP_LTE || w->flags & ~DRM_XE_UFENCE_WAIT_FLAG_ABSTIME || w->pad)
			return EINVAL;
		const uint64_t v = *(const volatile uint64_t *)(uintptr_t)w->addr & w->mask, want = w->value & w->mask;
		const bool met = w->op == DRM_XE_UFENCE_WAIT_OP_EQ ? v == want : w->op == DRM_XE_UFENCE_WAIT_OP_NEQ ? v != want :
				 w->op == DRM_XE_UFENCE_WAIT_OP_GT ? v > want : w->op == DRM_XE_UFENCE_WAIT_OP_GTE ? v >= want :
				 w->op == DRM_XE_UFENCE_WAIT_OP_LT ? v < want : v <= want;
		if (met)
			return 0;
		if (!(w->flags & DRM_XE_UFENCE_WAIT_FLAG_ABSTIME))
			w->timeout = 0;
		return ETIME;
	}
	// Surface fences: execs complete at submission here, so there is never anything to wait for.
	case DRM_IOCTL_INTELXE_SURFACE_FENCE_ATTACH: {
		const struct drm_intelxe_surface_fence_attach *a = arg;
		return !a->surface_id || a->pad || (a->flags != INTELXE_SURFACE_FENCE_WRITE && a->flags != INTELXE_SURFACE_FENCE_READ) ?
			EINVAL : 0;
	}
	case DRM_IOCTL_INTELXE_SURFACE_FENCE_IMPORT: {
		struct drm_intelxe_surface_fence_import *m = arg;
		if (m->flags != INTELXE_SURFACE_FENCE_READER && m->flags != INTELXE_SURFACE_FENCE_WRITER)
			return EINVAL;
		m->point_out = 0;
		m->count_out = 0;
		return 0;
	}
	case DRM_IOCTL_INTELXE_SURFACE_FENCE_WAIT:
		return 0;
	default:
		return ENOTTY;
	}
}

static void fake_free(struct fake *f)
{
	for (uint32_t h = 1; h < f->num_bos; h++)
		fake_bo_put(f->bos[h]);
	for (uint32_t h = 1; h < f->num_syncobjs; h++)
		fake_syncobj_put(f->syncobjs[h]);
	free(f->bos);
	free(f->syncobjs);
	free(f);
}

// ---- public interface -----------------------------------------------------------------------------------------

static bool fake_enabled(void)
{
	const char *e = getenv("XEMAC_FAKE");
	return e && atoi(e) > 0;
}

// The connection is opened on NMXAccel (an IOAccelerator) when it offers IntelXe's xe client, type 'B580' as its
// NMXIntelXeClientType property says: the system-graphics sandbox of Safari, iconservicesagent, VTDecoderXPCService, ...
// lets a process open only connections to an IOAccelerator, and IntelXe is not one. Otherwise (an older NMXAccel or
// IntelXe.kext, no accelerator, XEMAC_DIRECT=1) it is opened on IntelXe itself, type 0. Both give the same client.
static kern_return_t xemac_connect(io_connect_t *conn, const char **via)
{
	static const uint32_t intelxe_type = 0x42353830; // 'B580', kNmxIntelXeType in nullmoth's nvrm-accel.cpp
	kern_return_t kr = kIOReturnNotFound;
	const char *direct = getenv("XEMAC_DIRECT");
	CFMutableDictionaryRef m = direct && atoi(direct) > 0 ? NULL : IOServiceMatching("NMXAccel");
	if (m) {
		CFMutableDictionaryRef props = CFDictionaryCreateMutable(kCFAllocatorDefault, 1, &kCFTypeDictionaryKeyCallBacks,
			&kCFTypeDictionaryValueCallBacks);
		CFNumberRef type = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt32Type, &intelxe_type);
		if (props && type) {
			CFDictionarySetValue(props, CFSTR("NMXIntelXeClientType"), type);
			CFDictionarySetValue(m, CFSTR(kIOPropertyMatchKey), props);
		}
		if (type)
			CFRelease(type);
		if (props)
			CFRelease(props);
		io_service_t accel = IOServiceGetMatchingService(kIOMainPortDefault, m); // consumes m
		if (accel) {
			kr = IOServiceOpen(accel, mach_task_self(), intelxe_type, conn);
			IOObjectRelease(accel);
			if (kr == KERN_SUCCESS) {
				*via = "NMXAccel";
				return kr;
			}
			if (tracing())
				fprintf(stderr, "xemac: IOServiceOpen(NMXAccel, 'B580') 0x%x, opening IntelXe\n", kr);
		}
	}
	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc)
		return kr == kIOReturnNotPrivileged ? kr : kIOReturnNotFound;
	kr = IOServiceOpen(svc, mach_task_self(), 0, conn);
	IOObjectRelease(svc);
	*via = "IntelXe.kext";
	return kr;
}

int xemac_open(void)
{
	struct file *file = calloc(1, sizeof(*file));
	if (!file) {
		errno = ENOMEM;
		return -1;
	}
	const char *via = "fake BMG G21";
	if (fake_enabled()) {
		fake_init_engines();
		file->fake = calloc(1, sizeof(*file->fake));
	} else {
		kern_return_t kr = xemac_connect(&file->conn, &via);
		if (kr != KERN_SUCCESS) {
			free(file);
			errno = kr == kIOReturnNotPrivileged ? EACCES : ENODEV;
			return -1;
		}
	}
	int fd = open("/dev/null", O_RDONLY | O_CLOEXEC);
	if (fd < 0 || fd >= MAX_FD) {
		if (fd >= 0)
			close(fd);
		if (file->conn)
			IOServiceClose(file->conn);
		if (file->fake)
			fake_free(file->fake);
		free(file);
		errno = EMFILE;
		return -1;
	}
	pthread_mutex_lock(&lock);
	files[fd] = file;
	pthread_mutex_unlock(&lock);
	if (tracing())
		fprintf(stderr, "xemac[%d] open (%s)\n", fd, via);
	return fd;
}

int xemac_close(int fd)
{
	struct file *file = file_of(fd);
	if (file) {
		pthread_mutex_lock(&lock);
		files[fd] = NULL;
		pthread_mutex_unlock(&lock);
		if (file->status_state == 1)
			IOConnectUnmapMemory64(file->conn, file->status_type, mach_task_self(), file->status_addr);
		if (file->conn)
			IOServiceClose(file->conn);
		if (file->fake) {
			pthread_mutex_lock(&fake_lock);
			fake_free(file->fake);
			fake_minors_sweep();
			pthread_mutex_unlock(&fake_lock);
		}
		free(file);
		if (tracing())
			fprintf(stderr, "xemac[%d] close\n", fd);
	}
	return close(fd);
}

bool xemac_is_fd(int fd)
{
	return file_of(fd) != NULL;
}

// The status page (INTELXE_CAP_STATUS_PAGE), mapped read-only once per file. Under `lock`.
static void status_page_map(struct file *file)
{
	if (file->status_state || file->fake)
		return;
	file->status_state = -1;
	struct drm_get_cap cap = {.capability = INTELXE_CAP_STATUS_PAGE};
	uint64_t in[2] = {DRM_IOCTL_GET_CAP, (uint64_t)(uintptr_t)&cap}, out[1] = {0};
	uint32_t n = 1;
	if (IOConnectCallScalarMethod(file->conn, XEMAC_METHOD_IOCTL, in, 2, out, &n) != KERN_SUCCESS || out[0] || !cap.value)
		return;
	mach_vm_address_t at = 0;
	mach_vm_size_t size = 0;
	if (IOConnectMapMemory64(file->conn, (uint32_t)cap.value, mach_task_self(), &at, &size,
				 kIOMapAnywhere | kIOMapReadOnly) != KERN_SUCCESS || size < sizeof(uint64_t))
		return;
	file->status_addr = at;
	file->status_type = (uint32_t)cap.value;
	file->ban_epoch = (const volatile uint64_t *)(uintptr_t)at;
	file->status_state = 1;
}

// DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY(BAN) answered without the kext for a queue seen unbanned at the current ban
// epoch. Returns true when answered.
static bool ban_cached(struct file *file, struct drm_xe_exec_queue_get_property *p, uint64_t *epoch_out)
{
	bool hit = false;
	pthread_mutex_lock(&lock);
	status_page_map(file);
	if (file->status_state == 1) {
		const uint64_t epoch = *file->ban_epoch;
		__sync_synchronize();
		*epoch_out = epoch;
		for (unsigned i = 0; i < file->unbanned_count; i++)
			if (file->unbanned[i].queue == p->exec_queue_id && file->unbanned[i].epoch == epoch) {
				p->value = 0;
				hit = true;
				break;
			}
	}
	pthread_mutex_unlock(&lock);
	return hit;
}

static void ban_remember(struct file *file, uint32_t queue, uint64_t epoch, bool unbanned)
{
	pthread_mutex_lock(&lock);
	unsigned i = 0;
	while (i < file->unbanned_count && file->unbanned[i].queue != queue)
		i++;
	if (!unbanned) {   // banned, or the queue went away: forget it
		if (i < file->unbanned_count)
			file->unbanned[i] = file->unbanned[--file->unbanned_count];
	} else if (i < file->unbanned_count) {
		file->unbanned[i].epoch = epoch;
	} else if (file->unbanned_count < XEMAC_BAN_CACHE) {
		file->unbanned[file->unbanned_count++] = (struct xemac_ban){queue, epoch};
	}
	pthread_mutex_unlock(&lock);
}

// One GET_CAP straight to the device (no tracing, no timeouts to convert).
static uint64_t device_cap(struct file *file, uint64_t capability)
{
	struct drm_get_cap cap = {.capability = capability};
	int err;
	if (file->fake) {
		pthread_mutex_lock(&fake_lock);
		err = fake_ioctl(file->fake, DRM_IOCTL_GET_CAP, &cap);
		pthread_mutex_unlock(&fake_lock);
	} else {
		uint64_t in[2] = {DRM_IOCTL_GET_CAP, (uint64_t)(uintptr_t)&cap}, out[1] = {0};
		uint32_t n = 1;
		kern_return_t kr = IOConnectCallScalarMethod(file->conn, XEMAC_METHOD_IOCTL, in, 2, out, &n);
		err = kr != KERN_SUCCESS ? EIO : (int)out[0];
	}
	return err ? 0 : cap.value;
}

// What the kernel offers of fd sharing (uapi/intelxe_drm.h, INTELXE_CAP_FD_OBJECTS), asked once per device file.
// XEMAC_FDS=0 turns it off here whatever the kernel says (the object fd ioctls then go to the kernel as before).
static unsigned fd_sharing(struct file *file)
{
	int v = __atomic_load_n(&file->fd_sharing, __ATOMIC_ACQUIRE);
	if (v)
		return v - 1;
	unsigned bits = 0;
	const char *e = getenv("XEMAC_FDS");
	if ((!e || atoi(e) > 0) && device_cap(file, INTELXE_CAP_FD_OBJECTS) == 1) {
		bits |= XEMAC_FD_SYNC;
		if ((device_cap(file, DRM_CAP_PRIME) & (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT)) ==
		    (DRM_PRIME_CAP_IMPORT | DRM_PRIME_CAP_EXPORT))
			bits |= XEMAC_FD_PRIME;
	}
	__atomic_store_n(&file->fd_sharing, (int)bits + 1, __ATOMIC_RELEASE);
	return bits;
}

unsigned xemac_fd_sharing(int fd)
{
	struct file *file = file_of(fd);
	return file ? fd_sharing(file) : 0;
}

int64_t xemac_obj_size(int fd, int obj_fd)
{
	struct drm_intelxe_obj_info info = {.fd = obj_fd};
	if (xemac_ioctl(fd, DRM_IOCTL_INTELXE_OBJ_INFO, &info))
		return -1;
	if (info.kind != INTELXE_OBJ_KIND_BO) {
		errno = EINVAL;
		return -1;
	}
	return (int64_t)info.size;
}

// ---- the CPU-visible VRAM window ---------------------------------------------------------------------------------
// The BAR window the CPU reaches VRAM through is 1 GB on macOS (OpenCore's ResizeAppleGpuBars; macOS addresses no larger
// BAR) and every Vulkan process shares it. A BO there that the CPU has mapped cannot be moved out later (an IOKit user
// mapping of device memory cannot be redirected), so whoever came first kept it: at login twenty-odd agents and
// extensions took most of it, and a game started later had its mapped buffers spilled to system memory and ran at half
// speed (Minecraft + Sodium + BSL, 5 Nov). So the processes that do little GPU work put their CPU-visible BOs in system
// memory from the start, as the kernel does anyway once the window is full (the placement allows it): agents, menu-bar
// and background-only apps (LSUIElement, LSBackgroundOnly), app extensions (.appex), and launchd jobs that are no bundle.
// Applications, their XPC services (WebKit's GPU process), WindowServer and anything another process started (a game's
// java) keep the window. INTELXE_WINDOW=normal|background decides instead.
static bool window_background(void)
{
	static int cls = -1;   // 0 normal, 1 background
	if (cls >= 0) return cls == 1;
	int bg = 0;
	const char *e = getenv("INTELXE_WINDOW");
	if (e && !strcmp(e, "background")) bg = 1;
	else if (e && !strcmp(e, "normal")) bg = 0;
	else if (!strcmp(getprogname(), "WindowServer")) bg = 0;
	else {
		char path[1024] = "";
		CFBundleRef b = CFBundleGetMainBundle();
		CFURLRef u = b ? CFBundleCopyBundleURL(b) : NULL;
		if (u) { CFURLGetFileSystemRepresentation(u, true, (UInt8 *)path, sizeof path); CFRelease(u); }
		const size_t n = strlen(path);
		const bool app = n > 4 && !strcmp(path + n - 4, ".app"), appex = n > 6 && !strcmp(path + n - 6, ".appex"),
		           xpc = n > 4 && !strcmp(path + n - 4, ".xpc");
		if (app) {
			for (int k = 0; k < 2 && !bg; k++) {
				CFTypeRef v = CFBundleGetValueForInfoDictionaryKey(b, k ? CFSTR("LSBackgroundOnly") : CFSTR("LSUIElement"));
				if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) bg = CFBooleanGetValue((CFBooleanRef)v);
				else if (v && CFGetTypeID(v) == CFStringGetTypeID()) bg = CFStringGetIntValue((CFStringRef)v) != 0;
			}
		} else if (appex) bg = 1;
		else if (!xpc && getppid() == 1) bg = 1;   // a launchd job, not a bundle
	}
	cls = bg;
	if (tracing())
		fprintf(stderr, "xemac: %s: CPU-visible BOs in %s\n", getprogname(), bg ? "system memory (background process)" : "the VRAM window");
	return bg;
}

int xemac_ioctl(int fd, unsigned long request, void *arg)
{
	struct file *file = file_of(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}

	// An export's fd: the kernel cannot create file descriptors, so the object is bound to an open of the clone device
	// made here and passed in args->fd (uapi/intelxe_drm.h). Only when the kernel has object fds: an older one gets the
	// ioctl as before (ENOTTY).
	int obj_fd = -1;
	int32_t *obj_fd_arg = NULL;
	if ((request == DRM_IOCTL_PRIME_HANDLE_TO_FD || request == DRM_IOCTL_SYNCOBJ_HANDLE_TO_FD) &&
	    (fd_sharing(file) & (request == DRM_IOCTL_PRIME_HANDLE_TO_FD ? XEMAC_FD_PRIME : XEMAC_FD_SYNC))) {
		bool cloexec = true; // syncobj fds and sync files always are (drm_syncobj.c: O_CLOEXEC)
		if (request == DRM_IOCTL_PRIME_HANDLE_TO_FD) {
			struct drm_prime_handle *a = arg;
			cloexec = a->flags & DRM_CLOEXEC;
			obj_fd_arg = &a->fd;
		} else {
			obj_fd_arg = &((struct drm_syncobj_handle *)arg)->fd;
		}
		obj_fd = file->fake ? fake_obj_open(cloexec) : open(INTELXE_OBJ_DEVICE, O_RDWR | (cloexec ? O_CLOEXEC : 0));
		if (obj_fd < 0) {
			int e = errno;
			if (tracing())
				fprintf(stderr, "xemac[%d] %s: open(%s) -> %s\n", fd, ioctl_name(request), INTELXE_OBJ_DEVICE, strerror(e));
			errno = e;
			return -1;
		}
		*obj_fd_arg = obj_fd;
	}

	// anv asks every queue's ban after every wait: answered from the status page while no queue has been banned since
	uint64_t ban_epoch = 0;
	bool ban_query = false;
	if (request == DRM_IOCTL_XE_EXEC_QUEUE_GET_PROPERTY && !file->fake) {
		struct drm_xe_exec_queue_get_property *p = arg;
		if (p->property == DRM_XE_EXEC_QUEUE_GET_PROPERTY_BAN) {
			if (ban_cached(file, p, &ban_epoch))
				return 0;
			ban_query = file->status_state == 1;
		}
	}
	if (request == DRM_IOCTL_XE_EXEC_QUEUE_DESTROY && !file->fake)   // its id may come back for another queue
		ban_remember(file, ((struct drm_xe_exec_queue_destroy *)arg)->exec_queue_id, 0, false);

	// Absolute timeouts go to the kext on its clock; the caller's struct is restored afterwards.
	int64_t *timeout = NULL, *deadline = NULL, saved_timeout = 0, saved_deadline = 0;
	if (request == DRM_IOCTL_SYNCOBJ_WAIT) {
		struct drm_syncobj_wait *w = arg;
		timeout = &w->timeout_nsec;
		if (w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
			deadline = (int64_t *)&w->deadline_nsec;
	} else if (request == DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT) {
		struct drm_syncobj_timeline_wait *w = arg;
		timeout = &w->timeout_nsec;
		if (w->flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_DEADLINE)
			deadline = (int64_t *)&w->deadline_nsec;
	} else if (request == DRM_IOCTL_XE_WAIT_USER_FENCE) {
		struct drm_xe_wait_user_fence *w = arg;
		if (w->flags & DRM_XE_UFENCE_WAIT_FLAG_ABSTIME)
			timeout = &w->timeout;
	}
	if (timeout)
		saved_timeout = *timeout, *timeout = monotonic_to_uptime(*timeout);
	if (deadline)
		saved_deadline = *deadline, *deadline = monotonic_to_uptime(*deadline);

	// a background process's CPU-visible BO that may live in system memory goes there (window_background)
	struct drm_xe_gem_create *gc = NULL;
	uint32_t gc_placement = 0, gc_flags = 0;
	if (request == DRM_IOCTL_XE_GEM_CREATE && !file->fake) {
		struct drm_xe_gem_create *g = arg;
		if ((g->flags & DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM) && (g->placement & 1u) && (g->placement & ~1u) &&
		    !(g->flags & DRM_XE_GEM_CREATE_FLAG_SCANOUT) && window_background()) {
			gc = g; gc_placement = g->placement; gc_flags = g->flags;
			g->placement = 1u;   // instance 0: system memory
			g->flags &= ~(uint32_t)DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM;
		}
	}

	int err;
	if (file->fake) {
		pthread_mutex_lock(&fake_lock);
		err = fake_ioctl(file->fake, request, arg);
		pthread_mutex_unlock(&fake_lock);
	} else {
		uint64_t in[2] = {request, (uint64_t)(uintptr_t)arg}, out[1] = {0};
		uint32_t n = 1;
		kern_return_t kr = IOConnectCallScalarMethod(file->conn, XEMAC_METHOD_IOCTL, in, 2, out, &n);
		err = kr != KERN_SUCCESS ? EIO : (int)out[0];
	}

	if (gc) gc->placement = gc_placement, gc->flags = gc_flags;
	if (timeout)
		*timeout = saved_timeout;
	if (deadline)
		*deadline = saved_deadline;
	if (obj_fd >= 0 && err) {   // nothing was bound to it
		close(obj_fd);
		*obj_fd_arg = -1;
	}

	// the answer holds for the epoch read before asking (a ban after that moves the epoch)
	if (ban_query && !err) {
		struct drm_xe_exec_queue_get_property *p = arg;
		ban_remember(file, p->exec_queue_id, ban_epoch, p->value == 0);
	}

	// ENGINE_CYCLES: the kext samples cpu_timestamp on CLOCK_UPTIME_RAW; move it to the clock the caller asked for.
	if (!err && request == DRM_IOCTL_XE_DEVICE_QUERY) {
		struct drm_xe_device_query *q = arg;
		if (q->query == DRM_XE_DEVICE_QUERY_ENGINE_CYCLES && q->data) {
			struct drm_xe_query_engine_cycles *c = (void *)(uintptr_t)q->data;
			if (c->clockid != CLOCK_UPTIME_RAW)
				c->cpu_timestamp += clock_ns(c->clockid) - clock_ns(CLOCK_UPTIME_RAW);
		}
	}

	if (tracing())
		trace_ioctl(fd, request, arg, err ? -1 : 0, err);
	if (err) {
		errno = err;
		return -1;
	}
	return 0;
}

// Mappings of kext BOs are IOKit mappings: the kext holds them until IOConnectUnmapMemory64, so they are recorded here
// and only taken down when every byte of the range has been unmapped (Mesa unmaps the head of a mapping separately).
//
// MAP_FIXED (VK_EXT_map_memory_placed: DXVK under WoW64 places maps below 4 GB) cannot be an IOKit mapping at the
// address: IOConnectMapMemory64 keeps only kIOMapUserOptionsMask (0xfff) of its options from user space, so
// kIOMapOverwrite is dropped and a placed address that is already reserved (Mesa reserves it PROT_NONE first) fails.
// The BO is mapped anywhere by IOKit as usual (the kext's map, with its cache mode, accounting and teardown), and that
// memory is remapped, shared, onto the requested address with mach_vm_remap(VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE), as
// mmap(MAP_FIXED) replaces what was there. The IOKit map stays registered for as long as the placed view lives: both
// views are of the same pages, and the kext only lets go of them on IOConnectUnmapMemory64. xemac_munmap of the placed
// range deallocates the view, as munmap would; xemac_munmap_reserve (VK_MEMORY_UNMAP_RESERVE_BIT_EXT) overwrites it
// with a PROT_NONE reservation instead, never leaving a hole that a later mmap(MAP_FIXED) would trip over (EXC_GUARD
// DEALLOC_GAP on macOS 15). The IOKit map goes once the whole view is gone.
struct map {
	mach_vm_address_t addr;      // what the caller got (the placed address for MAP_FIXED)
	mach_vm_address_t io_addr;   // the IOKit mapping (== addr unless placed)
	mach_vm_size_t len, released;
	io_connect_t conn;
	uint32_t type;
	struct map *next;
};
static struct map *maps;

/* The kext answers a refused mapping (clientMemoryForType) with the IOReturn of the errno xe's mmap gives: a BO marked
 * DONTNEED (DRM_IOCTL_XE_MADVISE) kIOReturnBusy for xe_gem_object_mmap's EBUSY, a purged BO or an unknown offset
 * kIOReturnBadArgument for EINVAL (drm_gem_mmap). Each goes back to its errno, so Mesa sees what it sees on Linux. Before
 * (src/xemac-perf) everything but kIOReturnNoMemory became EINVAL, EBUSY included. */
static int map_errno(kern_return_t kr)
{
	switch (kr) {
	case kIOReturnNoMemory:
		return ENOMEM;  // IOKit could not make the mapping (as before)
	case kIOReturnBusy:
		return EBUSY;   // xe_gem_object_mmap: a DONTNEED BO (WAS EINVAL)
	default:
		return EINVAL;  // kIOReturnBadArgument (a purged BO, an unknown offset) and anything else, as before
	}
}

void *xemac_mmap(void *addr, size_t len, int prot, int flags, int fd, uint64_t offset)
{
	struct file *file = file_of(fd);
	if (!file || offset & 4095 || !len) {
		errno = file ? EINVAL : EBADF;
		return MAP_FAILED;
	}
	uint32_t type = offset >> 12;
	len = (len + 4095) & ~(size_t)4095;

	if (file->fake) {
		pthread_mutex_lock(&fake_lock);
		struct fake_bo *bo = fake_bo(file->fake, type);
		if (!bo || len > bo->size) {
			pthread_mutex_unlock(&fake_lock);
			errno = EINVAL;
			return MAP_FAILED;
		}
		mach_vm_address_t at = (mach_vm_address_t)(uintptr_t)addr;
		vm_prot_t cur, max;
		kern_return_t kr = mach_vm_remap(mach_task_self(), &at, len, 0,
						 flags & MAP_FIXED ? VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE : VM_FLAGS_ANYWHERE,
						 mach_task_self(), bo->mem, false, &cur, &max, VM_INHERIT_SHARE);
		pthread_mutex_unlock(&fake_lock);
		if (kr != KERN_SUCCESS) {
			errno = ENOMEM;
			return MAP_FAILED;
		}
		if (!(prot & PROT_WRITE))
			mprotect((void *)(uintptr_t)at, len, prot);
		return (void *)(uintptr_t)at;
	}

	struct map *m = calloc(1, sizeof(*m));
	if (!m) {
		errno = ENOMEM;
		return MAP_FAILED;
	}
	const bool placed = (flags & MAP_FIXED) != 0;
	if (placed && ((uintptr_t)addr & 4095)) {
		free(m);
		errno = EINVAL;
		return MAP_FAILED;
	}
	mach_vm_address_t at = 0;
	mach_vm_size_t size = 0;
	IOOptionBits options = kIOMapAnywhere;
	if (!(prot & PROT_WRITE))
		options |= kIOMapReadOnly;
	/* VRAM through BAR2 (the kernel sets offset bit 63): write-combined like xe's VRAM mmaps. The default cache mode
	 * for device memory is uncached, which held CPU writes into VRAM BOs at 56 MB/s. */
	if (offset >> 63)
		options |= kIOMapWriteCombineCache;
	kern_return_t kr = IOConnectMapMemory64(file->conn, type, mach_task_self(), &at, &size, options);
	if (kr != KERN_SUCCESS) {
		free(m);
		errno = map_errno(kr);
		return MAP_FAILED;
	}
	if (size < len) {
		IOConnectUnmapMemory64(file->conn, type, mach_task_self(), at);
		free(m);
		errno = EINVAL;
		return MAP_FAILED;
	}
	mach_vm_address_t user = at;
	if (placed) {
		// the same pages, shared (not copied), at the caller's address, replacing whatever was mapped there
		user = (mach_vm_address_t)(uintptr_t)addr;
		vm_prot_t cur = 0, max = 0;
		kr = mach_vm_remap(mach_task_self(), &user, len, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), at,
				   false, &cur, &max, VM_INHERIT_NONE);
		if (kr != KERN_SUCCESS || user != (mach_vm_address_t)(uintptr_t)addr) {
			if (kr == KERN_SUCCESS)
				mach_vm_deallocate(mach_task_self(), user, len);
			IOConnectUnmapMemory64(file->conn, type, mach_task_self(), at);
			free(m);
			errno = kr == KERN_NO_SPACE || kr == KERN_RESOURCE_SHORTAGE ? ENOMEM : EINVAL;
			return MAP_FAILED;
		}
		if (!(prot & PROT_WRITE))
			mach_vm_protect(mach_task_self(), user, len, false, VM_PROT_READ);
	}
	*m = (struct map){.addr = user, .io_addr = at, .len = len, .conn = file->conn, .type = type};
	if (tracing())
		fprintf(stderr, "xemac[%d] mmap type %u len 0x%zx flags 0x%x%s -> 0x%llx (IOKit 0x%llx)\n", fd, type, len, flags,
			placed ? " placed" : "", (unsigned long long)user, (unsigned long long)at);
	pthread_mutex_lock(&lock);
	m->next = maps;
	maps = m;
	pthread_mutex_unlock(&lock);
	return (void *)(uintptr_t)user;   // the placed view for MAP_FIXED
}

// Put a PROT_NONE reservation where [a, a + sz) was. A range still mapped (a placed view, a fake BO) is overwritten in
// place, so it never becomes a hole; a hole (an IOKit map just taken down) is filled without VM_FLAGS_OVERWRITE.
static int reserve_range(mach_vm_address_t a, mach_vm_size_t sz, bool mapped)
{
	if (mapped)
		return mmap((void *)(uintptr_t)a, sz, PROT_NONE, MAP_PRIVATE | MAP_ANON | MAP_FIXED, -1, 0) == MAP_FAILED ? -1 : 0;
	kern_return_t kr = mach_vm_allocate(mach_task_self(), &a, sz, VM_FLAGS_FIXED);
	if (kr == KERN_SUCCESS)
		kr = mach_vm_protect(mach_task_self(), a, sz, false, VM_PROT_NONE);
	if (kr != KERN_SUCCESS) {
		errno = ENOMEM;
		return -1;
	}
	return 0;
}

static int unmap(void *addr, size_t len, bool reserve)
{
	mach_vm_address_t a = (mach_vm_address_t)(uintptr_t)addr;
	pthread_mutex_lock(&lock);
	struct map **pm = &maps, *m = NULL;
	for (; *pm; pm = &(*pm)->next)
		if (a >= (*pm)->addr && a < (*pm)->addr + (*pm)->len) {
			m = *pm;
			break;
		}
	if (!m) {
		pthread_mutex_unlock(&lock);
		if (reserve)
			return reserve_range((mach_vm_address_t)(uintptr_t)addr & ~(mach_vm_address_t)4095, (len + 4095) & ~(size_t)4095, true);
		return munmap(addr, len); // fake BOs and anything else are ordinary VM
	}
	const mach_vm_size_t sz = (len + 4095) & ~(size_t)4095;
	m->released += sz;
	bool done = m->released >= m->len;
	if (done)
		*pm = m->next;
	// A placed view is the caller's VM like any munmap: each part goes when it is unmapped (a hole, as munmap leaves).
	// The IOKit map behind it can only go whole, once every part is gone.
	const bool placed = m->addr != m->io_addr;
	pthread_mutex_unlock(&lock);
	const mach_vm_address_t page = a & ~(mach_vm_address_t)4095;
	int ret = 0;
	if (placed) {   // only the part unmapped now: earlier holes may hold the caller's new reservations
		if (reserve)
			ret = reserve_range(page, sz, true);
		else
			mach_vm_deallocate(mach_task_self(), page, sz);
	}
	if (done) {
		IOConnectUnmapMemory64(m->conn, m->type, mach_task_self(), m->io_addr);
		free(m);
	}
	if (reserve && !placed && done)   // the IOKit map itself was at addr: reserve the hole it left
		ret = reserve_range(page, sz, false);
	return ret;
}

int xemac_munmap(void *addr, size_t len)
{
	return unmap(addr, len, false);
}

int xemac_munmap_reserve(void *addr, size_t len)
{
	return unmap(addr, len, true);
}

int xemac_pci_info(int fd, struct xemac_pci_info *info)
{
	struct file *file = file_of(fd);
	if (!file) {
		errno = EBADF;
		return -1;
	}
	if (file->fake) {
		// Bus position and subsystem IDs are placeholders.
		*info = (struct xemac_pci_info){.vendor = 0x8086, .device = FAKE_DEVICE, .subvendor = 0x8086,
						.subdevice = 0x1100, .revision = FAKE_REVISION, .bus = 3};
		return 0;
	}
	uint64_t out[8] = {0};
	uint32_t n = 8;
	kern_return_t kr = IOConnectCallScalarMethod(file->conn, XEMAC_METHOD_PCI_INFO, NULL, 0, out, &n);
	if (kr != KERN_SUCCESS || n < 8) {
		errno = EIO;
		return -1;
	}
	*info = (struct xemac_pci_info){.vendor = out[0], .device = out[1], .revision = out[2], .subvendor = out[3],
					.subdevice = out[4], .bus = out[5], .dev = out[6], .func = out[7]};
	return 0;
}

// The fake device's sensors: fixed clocks, temperatures and limits; energy that grows with CLOCK_UPTIME_RAW as the
// kext's counters grow with time (card 40 W, package 25 W), so sysman's power (energy over time) reads those powers.
static void fake_sensors(struct intelxe_sensors *s)
{
	const uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	static uint64_t first_ns;   // the counters start with the process's first sample (1000 J and 500 J then)
	if (!first_ns)
		first_ns = ns;
	*s = (struct intelxe_sensors){.version = INTELXE_SENSORS_VERSION, .size = sizeof(*s), .state = INTELXE_SENSORS_OK,
				      .timestamp_ns = ns, .act_mhz = 2100, .req_mhz = 2400, .rp0_mhz = 2850, .rpn_mhz = 400,
				      .rpe_mhz = 1800, .min_mhz = 1200, .max_mhz = 2850, .pkg_temp_c = 47, .vram_temp_c = 52};
	s->valid = INTELXE_SENSORS_FREQ | INTELXE_SENSORS_TEMP | INTELXE_SENSORS_RANGE | INTELXE_SENSORS_ENERGY |
		   INTELXE_SENSORS_POWER_CARD | INTELXE_SENSORS_POWER_PKG | INTELXE_SENSORS_LIMITS | INTELXE_SENSORS_CRIT;
	s->energy_uj[INTELXE_SENSORS_CARD] = 1000000000ull + (ns - first_ns) / 1000 * 40;   // 40 W = 40 uJ per us
	s->energy_uj[INTELXE_SENSORS_PKG] = 500000000ull + (ns - first_ns) / 1000 * 25;
	s->power_mw[INTELXE_SENSORS_CARD] = 40000;
	s->power_mw[INTELXE_SENSORS_PKG] = 25000;
	s->limit_mw[INTELXE_SENSORS_CARD][0] = 190000;
	s->limit_mw[INTELXE_SENSORS_CARD][1] = 250000;
	s->limit_mw[INTELXE_SENSORS_PKG][0] = 150000;
	s->limit_on = 1u << (INTELXE_SENSORS_CARD * 2) | 1u << (INTELXE_SENSORS_PKG * 2);
	s->crit_milli = 300000;
	s->crit_is_watts = 1;
}

int xemac_sensors(int fd, struct intelxe_sensors *out)
{
	struct file *file = fd >= 0 ? file_of(fd) : NULL;
	if (fd >= 0 && !file) {
		errno = EBADF;
		return -1;
	}
	memset(out, 0, sizeof(*out));
	if (file ? file->fake != NULL : fake_enabled()) {
		const char *e = getenv("XEMAC_FAKE_SENSORS");
		if (e && atoi(e) == 0) {
			errno = ENOENT;
			return -1;
		}
		fake_sensors(out);
		return 0;
	}
	struct intelxe_sensors s;
	size_t n = sizeof(s);
	memset(&s, 0, sizeof(s));
	if (sysctlbyname(INTELXE_SENSORS_SYSCTL, &s, &n, NULL, 0) != 0)
		return -1; // ENOENT: a kext before sensors_raw
	if (n < offsetof(struct intelxe_sensors, act_mhz) || s.version < 1) {
		errno = EPROTO;
		return -1;
	}
	memcpy(out, &s, n < sizeof(s) ? n : sizeof(s));
	return 0;
}
