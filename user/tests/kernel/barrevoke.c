// SPDX-License-Identifier: MIT
// barrevoke: CPU-mapped BOs leaving the BAR window with their mappings redirected (debug.intelxe.bar_revoke;
// docs/design/DESIGN-rebar-window.md section 8), on IntelXe.kext.
//   ./barrevoke now [MB]      fills the window with CPU-mapped NEEDS_VISIBLE BOs of MB each (default 4), the CPU and the
//                             GPU write a pattern into each, then debug.intelxe.bar_revoke_now=<own pid> revokes them
//                             (needs root: run with sudo; any bar_revoke value)
//   ./barrevoke natural [MB]  the same fill, then more mapped BOs than the window has room for: GEM_CREATE's eviction
//                             must revoke the oldest (needs debug.intelxe.bar_revoke=1; SKIP otherwise)
// Then, through the pointers the maps returned before the move:
//   1. the CPU reads back what the CPU and the GPU wrote before the move (the copy, the redirect)
//   2. the CPU writes a new value, the GPU copies it to a neighbouring dword (MI_COPY_MEM_MEM through the same GPU VA:
//      the GPU reads what the CPU wrote, its PTEs rewritten), the CPU reads the GPU's copy
//   3. the GPU stores again (MI_STORE_DATA_IMM), the CPU reads it
//   4. every map is unmapped with IOConnectUnmapMemory64, which must succeed (it finds the mapping through the
//      descriptor the revocation gave the map) and the kernel's "CPU maps gone" count must follow
// and debug.intelxe.vram's bar_revoke line must show moves and no broken / latched result.
// TAKES THE WHOLE WINDOW for a few seconds: not while a game or benchmark runs.
// Test only.   clang -arch x86_64 -O2 -Ikernel/IntelXe/uapi -o barrevoke user/tests/kernel/barrevoke.c -framework IOKit
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>
#include <signal.h>
#include <unistd.h>
#include "xe_drm.h"

static io_connect_t conn;
static int xioctl(unsigned long req, void *arg) {
	uint64_t in[2] = {req, (uint64_t)(uintptr_t)arg}, out = 0;
	uint32_t n = 1;
	if (IOConnectCallScalarMethod(conn, 0, in, 2, &out, &n)) return -1;
	errno = (int)out;
	return out ? -1 : 0;
}
static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t gem(uint64_t size, uint32_t placement, uint32_t flags, uint16_t caching) {
	struct drm_xe_gem_create g = {.size = size, .placement = placement, .flags = flags, .cpu_caching = caching};
	if (xioctl(DRM_IOCTL_XE_GEM_CREATE, &g)) return 0;
	return g.handle;
}
static void *map(uint32_t handle, uint64_t size, uint32_t *type) {
	struct drm_xe_gem_mmap_offset m = {.handle = handle};
	if (xioctl(DRM_IOCTL_XE_GEM_MMAP_OFFSET, &m)) { printf("MMAP_OFFSET %d\n", errno); return NULL; }
	mach_vm_address_t addr = 0; mach_vm_size_t sz = size;
	*type = (uint32_t)(m.offset >> 12);
	// bit 63: write-combined (what xemac_mmap asks for)
	IOOptionBits o = kIOMapAnywhere | ((m.offset >> 63) ? kIOMapWriteCombineCache : 0);
	if (IOConnectMapMemory64(conn, *type, mach_task_self(), &addr, &sz, o)) return NULL;
	return (void *)(uintptr_t)addr;
}
static int bind(uint32_t vm, uint32_t bo, uint64_t addr, uint64_t range) {
	struct drm_syncobj_create sc = {0};
	if (xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc)) return -1;
	struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = sc.handle};
	struct drm_xe_vm_bind b = {.vm_id = vm, .num_binds = 1, .num_syncs = 1, .syncs = (uint64_t)(uintptr_t)&s};
	b.bind.obj = bo; b.bind.range = range; b.bind.addr = addr; b.bind.op = DRM_XE_VM_BIND_OP_MAP; b.bind.pat_index = 2;
	int r = xioctl(DRM_IOCTL_XE_VM_BIND, &b);
	struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&sc.handle, .count_handles = 1, .timeout_nsec = INT64_MAX};
	if (!r) r = xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w);
	struct drm_syncobj_destroy d = {.handle = sc.handle};
	xioctl(DRM_IOCTL_SYNCOBJ_DESTROY, &d);
	return r;
}
static int sysint(const char *name, int dflt) {
	int v = 0; size_t l = sizeof(v);
	return sysctlbyname(name, &v, &l, NULL, 0) ? dflt : v;
}
static char vram[16384];
static const char *vramText(void) {
	size_t l = sizeof(vram);
	if (sysctlbyname("debug.intelxe.vram", vram, &l, NULL, 0)) vram[0] = 0;
	return vram;
}
static uint64_t num(const char *text, const char *key) {   // the number after `key`, 0 if absent
	const char *p = strstr(text, key);
	return p ? strtoull(p + strlen(key), NULL, 10) : 0;
}
static void revokeLine(const char *what) {
	const char *p = strstr(vramText(), "bar_revoke ");
	if (!p) { printf("%s: no bar_revoke line (kext without bar_revoke?)\n", what); return; }
	const char *e = strchr(p, '\n');
	printf("%s: %.*s\n", what, (int)(e ? e - p : (long)strlen(p)), p);
}

// Two VMs, each with its render queue: the filled BOs in 0, `natural`'s extra ones in 1, so that the extra BOs' EXECs do
// not make the filled ones recent (the policy counts a VM's last EXEC as a touch of every BO bound in it).
static uint32_t vmIds[2], qs[2], so, cur;
static volatile uint32_t *bb;
static void submit(uint32_t k) {
	bb[k++] = 0xa << 23; bb[k++] = 0;   // MI_BATCH_BUFFER_END
	__sync_synchronize();
	struct drm_xe_sync s = {.type = DRM_XE_SYNC_TYPE_SYNCOBJ, .flags = DRM_XE_SYNC_FLAG_SIGNAL, .handle = so};
	struct drm_xe_exec x = {.exec_queue_id = qs[cur], .num_syncs = 1, .syncs = (uint64_t)(uintptr_t)&s, .address = 0x100000, .num_batch_buffer = 1};
	CHECK(!xioctl(DRM_IOCTL_XE_EXEC, &x), "exec %d", errno);
	struct drm_syncobj_wait w = {.handles = (uint64_t)(uintptr_t)&so, .count_handles = 1, .timeout_nsec = INT64_MAX};
	CHECK(!xioctl(DRM_IOCTL_SYNCOBJ_WAIT, &w), "wait %d", errno);
}
// MI_STORE_DATA_IMM of value ^ offset at va + offset + 8, every 64 KB
static void gpuStore(uint64_t va, uint64_t size, uint32_t value) {
	uint32_t k = 0;
	for (uint64_t o = 0; o < size; o += 0x10000) {
		const uint64_t a = va + o + 8;
		bb[k++] = 0x20 << 23 | 2; bb[k++] = (uint32_t)a; bb[k++] = (uint32_t)(a >> 32); bb[k++] = value ^ (uint32_t)o;
	}
	submit(k);
}
// MI_COPY_MEM_MEM dword va + o + 16 -> va + o + 24, every 64 KB (destination first, as surffence.c)
static void gpuCopy(uint64_t va, uint64_t size) {
	uint32_t k = 0;
	for (uint64_t o = 0; o < size; o += 0x10000) {
		const uint64_t s = va + o + 16, d = va + o + 24;
		bb[k++] = 0x2e << 23 | 3; bb[k++] = (uint32_t)d; bb[k++] = (uint32_t)(d >> 32); bb[k++] = (uint32_t)s; bb[k++] = (uint32_t)(s >> 32);
	}
	submit(k);
}

#define MAXB 256
static volatile uint64_t *p[MAXB];
static uint64_t each;
// barwin2: a fault on an old pointer says which BO and where (barwin's kernel answered every later fault on a revoked BO
// with SIGBUS, and the test died without a line: stdout to a file is fully buffered)
static void onFault(int sig, siginfo_t *si, void *uc) {
	(void)uc;
	char b[160];
	const uintptr_t a = (uintptr_t)si->si_addr;
	int bo = -1;
	for (int i = 0; i < MAXB; i++)
		if (p[i] && a >= (uintptr_t)p[i] && a < (uintptr_t)p[i] + each) bo = i;
	const int n = snprintf(b, sizeof(b), "FAIL: signal %d at %#lx: BO %d offset %#lx\nFAILED: fault on an old pointer\n", sig,
		(unsigned long)a, bo, bo >= 0 ? (unsigned long)(a - (uintptr_t)p[bo]) : 0ul);
	write(1, b, (size_t)n);
	_exit(3);
}
int main(int argc, char **argv) {
	setvbuf(stdout, NULL, _IOLBF, 0);
	struct sigaction sa = {0};
	sa.sa_sigaction = onFault;
	sa.sa_flags = SA_SIGINFO;
	sigaction(SIGBUS, &sa, NULL);
	sigaction(SIGSEGV, &sa, NULL);
	const int forced = argc > 1 && !strcmp(argv[1], "now");
	const int natural = argc > 1 && !strcmp(argv[1], "natural");
	if (!forced && !natural) { fprintf(stderr, "usage: %s now|natural [MB per BO]\n", argv[0]); return 2; }
	each = (uint64_t)(argc > 2 ? atoi(argv[2]) : 4) << 20;
	if (each < (64u << 10) || each > (64ull << 20)) { fprintf(stderr, "MB: 1-64\n"); return 2; }
	const int knob = sysint("debug.intelxe.bar_revoke", -1);
	if (knob < 0) { printf("SKIP: kext without debug.intelxe.bar_revoke\n"); return 0; }
	if (natural && knob != 1) { printf("SKIP: debug.intelxe.bar_revoke is %d (natural needs 1)\n", knob); return 0; }
	if (forced && geteuid()) { printf("SKIP: 'now' writes debug.intelxe.bar_revoke_now (root)\n"); return 0; }

	io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IntelXe"));
	if (!svc || IOServiceOpen(svc, mach_task_self(), 0, &conn)) { fprintf(stderr, "no IntelXe service\n"); return 1; }
	uint32_t bbType;
	const uint32_t bbh = gem(0x10000, 1, 0, DRM_XE_GEM_CPU_CACHING_WB);   // system memory: never a candidate
	bb = bbh ? map(bbh, 0x10000, &bbType) : NULL;
	if (!bb) { fprintf(stderr, "batch\n"); return 1; }
	for (int v = 0; v < 2; v++) {
		struct drm_xe_vm_create vc = {0};
		if (xioctl(DRM_IOCTL_XE_VM_CREATE, &vc)) { fprintf(stderr, "vm create\n"); return 1; }
		vmIds[v] = vc.vm_id;
		if (bind(vmIds[v], bbh, 0x100000, 0x10000)) { fprintf(stderr, "batch bind\n"); return 1; }
		struct drm_xe_engine_class_instance ei = {.engine_class = DRM_XE_ENGINE_CLASS_RENDER};
		struct drm_xe_exec_queue_create qc = {.width = 1, .num_placements = 1, .vm_id = vmIds[v], .instances = (uint64_t)(uintptr_t)&ei};
		if (xioctl(DRM_IOCTL_XE_EXEC_QUEUE_CREATE, &qc)) { fprintf(stderr, "queue\n"); return 1; }
		qs[v] = qc.exec_queue_id;
	}
	struct drm_syncobj_create sc = {0};
	xioctl(DRM_IOCTL_SYNCOBJ_CREATE, &sc);
	so = sc.handle;

	const char *t = vramText();
	const uint64_t freeMB = num(t, "free "), movedBefore = num(strstr(t, "bar_revoke ") ?: "", "moved "),
		goneBefore = num(t, "gone ");
	revokeLine("before");
	// leave ~40 MB (the page-table reserve and a margin) so the extra BOs of `natural` cannot fit
	uint32_t n = freeMB > 40 ? (uint32_t)((freeMB - 40) / (each >> 20)) : 0;
	if (n > MAXB - 16) n = MAXB - 16;
	if (n < 2) { printf("SKIP: window free %llu MB\n", (unsigned long long)freeMB); return 0; }
	printf("window free %llu MB: %u CPU-mapped BOs of %llu MB (bar_revoke %d, mode %s)\n", (unsigned long long)freeMB, n,
		(unsigned long long)(each >> 20), knob, forced ? "now" : "natural");
	static uint32_t h[MAXB], type[MAXB];
	uint32_t made = 0;
	for (uint32_t i = 0; i < n; i++, made++) {
		if (!(h[i] = gem(each, 2, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM, DRM_XE_GEM_CPU_CACHING_WC))) break;
		if (!(p[i] = map(h[i], each, &type[i]))) { CHECK(0, "map %u", i); break; }
		for (uint64_t o = 0; o < each; o += 0x1000) p[i][o / 8] = (uint64_t)i << 32 | o;   // the CPU's pattern
		__sync_synchronize();
		const uint64_t va = (1ull << 33) + (uint64_t)i * each;
		CHECK(!bind(vmIds[0], h[i], va, each), "bind %u", i);
		gpuStore(va, each, 0x5a000000u | i);   // the GPU's, beside it
	}
	const int hot = sysint("debug.intelxe.bar_revoke_hot_ms", 1000);
	usleep((useconds_t)(hot > 0 ? hot + 300 : 300) * 1000);   // everything filled is cold now

	uint32_t extra = 0;
	if (forced) {
		int pid = getpid();
		CHECK(!sysctlbyname("debug.intelxe.bar_revoke_now", NULL, NULL, &pid, sizeof(pid)), "bar_revoke_now: %d", errno);
	} else {   // more mapped BOs than the window has room for: GEM_CREATE evicts, and only revocation can free anything
		cur = 1;
		for (uint32_t i = made; i < made + 12 && i < MAXB; i++, extra++) {
			if (!(h[i] = gem(each, 3, DRM_XE_GEM_CREATE_FLAG_NEEDS_VISIBLE_VRAM, DRM_XE_GEM_CPU_CACHING_WC))) break;
			if (!(p[i] = map(h[i], each, &type[i]))) { CHECK(0, "map extra %u", i); break; }
			for (uint64_t o = 0; o < each; o += 0x1000) p[i][o / 8] = (uint64_t)i << 32 | o;
			__sync_synchronize();
			const uint64_t va = (1ull << 33) + (uint64_t)i * each;
			CHECK(!bind(vmIds[1], h[i], va, each), "bind extra %u", i);
			gpuStore(va, each, 0x5a000000u | i);
		}
	}
	const uint32_t total = made + extra;
	t = vramText();
	const uint64_t moved = num(strstr(t, "bar_revoke ") ?: "", "moved ") - movedBefore;
	revokeLine("after the move");
	printf("revoked: %llu BOs\n", (unsigned long long)moved);
	CHECK(moved > 0, "nothing was revoked");
	// this process's own revoked BOs (debug.intelxe.vram per process: "(N, revoked K)"): the checks below exercise old
	// pointers only for those. The natural pass takes the least recently touched BOs of every process, which may all be
	// other processes'.
	char key[32];
	snprintf(key, sizeof(key), "pid %d ", getpid());
	const char *mine = strstr(t, key);
	const uint64_t own = mine ? num(mine, "revoked ") : 0;
	printf("revoked of this process: %llu\n", (unsigned long long)own);
	if (forced) CHECK(own > 0, "bar_revoke_now took none of this process's BOs");
	else if (!own) printf("NOTE: natural revoked only other processes' BOs: the old-pointer checks are vacuous\n");
	CHECK(!strstr(t, "LATCHED OFF"), "revocation latched off");
	CHECK(num(t, "broken ") == 0 && num(t, "undecided ") == 0, "broken / undecided revocations");
	cur = 0;

	// 1. what was written before the move, through the old pointers
	uint32_t bad = 0;
	for (uint32_t i = 0; i < total; i++)
		for (uint64_t o = 0; o < each; o += 0x1000) {
			if (p[i][o / 8] != ((uint64_t)i << 32 | o)) bad++;
			if (!(o & 0xffff) && ((volatile uint32_t *)p[i])[(o + 8) / 4] != ((0x5a000000u | i) ^ (uint32_t)o)) bad++;
		}
	CHECK(!bad, "%u words wrong after the move (CPU and GPU patterns)", bad);
	// 2. CPU writes, the GPU copies, the CPU reads the copy; 3. the GPU stores again
	bad = 0;
	for (uint32_t i = 0; i < total; i++) {
		for (uint64_t o = 0; o < each; o += 0x10000) ((volatile uint32_t *)p[i])[(o + 16) / 4] = 0xc0000000u | i << 16 | (uint32_t)(o >> 16);
		__sync_synchronize();
		const uint64_t va = (1ull << 33) + (uint64_t)i * each;
		cur = i >= made;
		gpuCopy(va, each);
		gpuStore(va, each, 0x3c000000u | i);
		for (uint64_t o = 0; o < each; o += 0x10000) {
			if (((volatile uint32_t *)p[i])[(o + 24) / 4] != (0xc0000000u | i << 16 | (uint32_t)(o >> 16))) bad++;
			if (((volatile uint32_t *)p[i])[(o + 8) / 4] != ((0x3c000000u | i) ^ (uint32_t)o)) bad++;
		}
	}
	CHECK(!bad, "%u words wrong after CPU writes / GPU copies / GPU stores through the old pointers and VAs", bad);
	// 4. unmaps
	uint32_t unmapFails = 0;
	for (uint32_t i = 0; i < total; i++)
		if (p[i] && IOConnectUnmapMemory64(conn, type[i], mach_task_self(), (mach_vm_address_t)(uintptr_t)p[i])) unmapFails++;
	CHECK(!unmapFails, "%u of %u unmaps failed", unmapFails, total);
	for (uint32_t i = 0; i < total; i++) {
		struct drm_gem_close c = {.handle = h[i]};
		xioctl(DRM_IOCTL_GEM_CLOSE, &c);
	}
	usleep(200000);
	t = vramText();
	const uint64_t gone = num(t, "gone ") - goneBefore;
	CHECK(gone >= total, "CPU maps gone %llu of %u", (unsigned long long)gone, total);
	revokeLine("end");
	printf("%s: %d failure(s)\n", fails ? "FAILED" : "PASSED", fails);
	return fails != 0;
}
