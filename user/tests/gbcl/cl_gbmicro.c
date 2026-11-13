// SPDX-License-Identifier: MIT
// cl_gbmicro: a fast, deterministic stand-in for Geekbench's OpenCL workloads, for kernel A/B runs on the B580.
// It drives Apple's OpenCL.framework (the layer Geekbench uses; on the B580 it runs over our Metal plugin, then ANV,
// then the kext's xe uAPI) with the call shapes that reach the kernel: many small and medium kernel enqueues, cl_mem
// create/use/release churn (VM_BIND map/unmap, GEM create/close when a buffer is past the plugin's pools), unaligned
// CL_MEM_USE_HOST_PTR imports (userptr binds with NVMTL_IMPORT_UNALIGNED=1, a staged copy without), and completion
// waits (syncobj waits). Per mode: ops, ops/s, per-op latency p50/p99, and the changed counters of debug.intelxe.stats.
//   clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -Wall -Wno-deprecated-declarations -o cl_gbmicro cl_gbmicro.c -framework OpenCL
//   ./cl_gbmicro [-t secs] [-m chain,churn,import,mixed,live] [-q]
//     -t   seconds per mode (default 15; live splits it over its four levels)
//     -m   modes, in this order (default all five)
//     -q   only the RESULT lines and the counter deltas
//   GBCL_DEVICE=<n> picks GPU n of the list it prints (default: the one reporting Intel, else GPU 0).
// The op sequence is fixed (an LCG with a fixed seed): two runs of one mode do the same calls; only how many fit in
// the time differs. Counter deltas are also printed per op, so runs of different lengths compare.
#include <OpenCL/opencl.h>
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/sysctl.h>

static mach_timebase_info_data_t tb;
static double now_us(void) { return (double)mach_absolute_time() * tb.numer / tb.denom / 1e3; }
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
#define CK(x) do { cl_int e_ = (x); if (e_) { printf("FAIL %s:%d %s -> %d\n", __FILE__, __LINE__, #x, e_); exit(1); } } while (0)

static int quiet;
static cl_context ctx;
static cl_device_id dev;
static cl_command_queue q;
static cl_kernel k_tiny, k_touch;
static uint32_t lcg_s;
static uint32_t lcg(void) { lcg_s = lcg_s * 1664525u + 1013904223u; return lcg_s >> 8; }

static const char *src =
	"__kernel void tiny(__global uint *o, uint v) { if (get_global_id(0) == 0) o[0] += v; }\n"
	"__kernel void touch(__global uint *b, uint n, uint v) { size_t i = get_global_id(0); if (i < n) b[i] = b[i] * 3u + v; }\n";

// ─── debug.intelxe.stats: every number of the text, with a label from the text before it ───────────────────────────
// The format changes with each kernel revision (rb8c..f added lines), so nothing is parsed by name: before and after
// are tokenized the same way, and when both have the same text around their numbers the changed ones are printed with
// "<line head>: <clause>" as the label. When the shapes differ (a kernel reload in between) nothing is printed.
#define KMAX 1024
typedef struct { int n; long long v[KMAX]; char lab[KMAX][96]; } kstats;
static int kread(char **out)
{
	size_t len = 0;
	if (sysctlbyname("debug.intelxe.stats", NULL, &len, NULL, 0) || !len) return -1;
	len += 4096; char *b = calloc(1, len + 1);
	if (sysctlbyname("debug.intelxe.stats", b, &len, NULL, 0)) { free(b); return -1; }
	*out = b; return 0;
}
static int isal(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '.'; }
static void kparse(const char *t, kstats *k)
{
	k->n = 0;
	const char *line = t;
	for (const char *p = t; *p && k->n < KMAX; p++) {
		if (*p == '\n') { line = p + 1; continue; }
		if (!(*p >= '0' && *p <= '9') || (p > t && (isal(p[-1]) || p[-1] == '-'))) continue;
		if (p[0] == '0' && p[1] == 'x') { while (isal(*p)) p++; p--; continue; }   // addresses: not counters
		const char *e = p; long long v = 0; while (*e >= '0' && *e <= '9') v = v * 10 + (*e++ - '0');
		if (isal(*e)) { p = e - 1; continue; }   // part of a word ("rb8e", "4K")
		// label: the line's head up to its first ':' plus the clause the number ends (back to ; , ( or the head)
		const char *colon = memchr(line, ':', (size_t)(p - line));
		const char *cl = p; while (cl > line && cl[-1] != ';' && cl[-1] != ',' && cl[-1] != '(' && cl[-1] != ':' && cl[-1] != ')') cl--;
		while (*cl == ' ') cl++;
		const char *he = colon;   // a line without ':' ("TLB invalidations 5 (ranged 2, ...)"): its words before the first number
		if (!he) { he = line; while (he < p && !(*he >= '0' && *he <= '9')) he++; while (he > line && he[-1] == ' ') he--; }
		char head[40] = ""; if (he > line && he < cl) { size_t hl = (size_t)(he - line); if (hl > 38) hl = 38; memcpy(head, line, hl); head[hl] = 0; }
		char clause[64] = ""; size_t cn = (size_t)(p - cl); if (cn > 62) cn = 62; memcpy(clause, cl, cn); clause[cn] = 0;
		while (cn && clause[cn - 1] == ' ') clause[--cn] = 0;
		snprintf(k->lab[k->n], sizeof k->lab[0], "%s%s%s", head, *head ? ": " : "", clause);
		k->v[k->n++] = v;
		p = e - 1;
	}
}
static kstats ks0, ks1;
static int kbefore(void) { char *t; if (kread(&t)) return -1; kparse(t, &ks0); free(t); return 0; }
static void kafter(const char *mode, long ops)
{
	char *t; if (kread(&t)) { printf("  kstat %s: no debug.intelxe.stats (not the IntelXe kernel)\n", mode); return; }
	kparse(t, &ks1); free(t);
	if (ks1.n != ks0.n) { printf("  kstat %s: the stats text changed shape (%d -> %d numbers); no deltas\n", mode, ks0.n, ks1.n); return; }
	int shown = 0;
	for (int i = 0; i < ks1.n; i++) {
		if (strcmp(ks0.lab[i], ks1.lab[i])) { printf("  kstat %s: label %d moved; no deltas\n", mode, i); return; }
		const long long d = ks1.v[i] - ks0.v[i];
		if (!d) continue;
		printf("  kstat %s: %-70s %+8lld  (%.3f/op)\n", mode, ks1.lab[i], d, ops ? (double)d / ops : 0.0);
		shown++;
	}
	if (!shown) printf("  kstat %s: no counter changed\n", mode);
}

// ─── timing ─────────────────────────────────────────────────────────────────────────────────────────────────────────
static double *lat; static long latcap;
typedef void (*opfn)(long i, void *st);
static void run(const char *mode, opfn op, void *st, double secs, const char *check)
{
	for (long i = 0; i < 32; i++) op(-1 - i, st);   // warm-up: pipelines, pools, the first BOs (not counted)
	CK(clFinish(q));
	const int haveK = kbefore() == 0;
	long n = 0; const double t0 = now_us(), end = t0 + secs * 1e6; double t = t0;
	while (t < end && n < latcap) { op(n, st); const double t1 = now_us(); lat[n++] = t1 - t; t = t1; }
	CK(clFinish(q));
	const double wall = (now_us() - t0) / 1e6;
	qsort(lat, (size_t)n, 8, dcmp); double s = 0; for (long i = 0; i < n; i++) s += lat[i];
	printf("RESULT mode=%s ops=%ld secs=%.2f ops_s=%.1f p50_us=%.1f p99_us=%.1f mean_us=%.1f max_us=%.1f check=%s\n", mode, n, wall,
	       n / wall, lat[n / 2], lat[n * 99 / 100], s / n, lat[n - 1], check ? check : "ok");
	if (haveK) kafter(mode, n); else printf("  kstat %s: no debug.intelxe.stats (not the IntelXe kernel)\n", mode);
	fflush(stdout);
}
static void set_touch(cl_mem b, cl_uint n, cl_uint v)
{ CK(clSetKernelArg(k_touch, 0, sizeof b, &b)); CK(clSetKernelArg(k_touch, 1, 4, &n)); CK(clSetKernelArg(k_touch, 2, 4, &v)); }
static void enq_touch(cl_mem b, cl_uint n, cl_uint v, cl_event *ev)
{
	set_touch(b, n, v); size_t g = ((size_t)n + 63) & ~(size_t)63, l = 64;
	CK(clEnqueueNDRangeKernel(q, k_touch, 1, NULL, &g, &l, 0, NULL, ev));
}
static void enq_tiny(cl_mem o, cl_uint v)
{
	CK(clSetKernelArg(k_tiny, 0, sizeof o, &o)); CK(clSetKernelArg(k_tiny, 1, 4, &v));
	size_t g = 64, l = 64; CK(clEnqueueNDRangeKernel(q, k_tiny, 1, NULL, &g, &l, 0, NULL, NULL));
}

// ─── chain: 4 tiny kernels + one 256 KB kernel, then clFinish (one submission and one wait per op) ──────────────────
typedef struct { cl_mem o, m; unsigned long long sum; } chain_st;
static void op_chain(long i, void *p)
{
	chain_st *s = p; (void)i;
	for (cl_uint k = 1; k <= 4; k++) { enq_tiny(s->o, k); s->sum += k; }
	enq_touch(s->m, 65536, 1, NULL);
	CK(clFinish(q));
}
// ─── churn: create a buffer (sizes cycle across the plugin's pool and park limits), write 4 KB, one kernel, read
// 4 bytes back, release. Sizes past 4 MB (shared) / 1 MB (private) are their own BO: GEM_CREATE + VM_BIND + unbind +
// close each, unless the plugin's buffer park reuses one (same size class within NVMTL_BUFPARK_AGE_MS). ───────────────
static const size_t churn_sz[] = { 64 << 10, 256 << 10, (1 << 20) + 4096, (4 << 20) + (64 << 10), 6 << 20, 12 << 20,
                                   3 << 20, (24 << 20) + 4096, 512 << 10, (9 << 20) + 8192, 2 << 20, (16 << 20) + 65536 };
#define NCHURN (sizeof churn_sz / sizeof churn_sz[0])
typedef struct { uint32_t host[1024]; int bad; } churn_st;
static void op_churn(long i, void *p)
{
	churn_st *s = p; cl_int err;
	const size_t S = churn_sz[(size_t)(i < 0 ? -i : i) % NCHURN];
	cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE, S, NULL, &err); CK(err);
	for (int k = 0; k < 1024; k++) s->host[k] = (uint32_t)i + k;
	CK(clEnqueueWriteBuffer(q, b, CL_TRUE, 0, 4096, s->host, 0, NULL, NULL));
	enq_touch(b, 16384, 7, NULL);
	uint32_t r = 0; CK(clEnqueueReadBuffer(q, b, CL_TRUE, 0, 4, &r, 0, NULL, NULL));
	if (r != (uint32_t)i * 3u + 7u) s->bad++;
	CK(clReleaseMemObject(b));
}
// ─── import: an unaligned malloc'd host range as CL_MEM_USE_HOST_PTR (442 KB + 48 B, like Geekbench's), 2 kernels,
// map/unmap to read it, release. Eight host ranges are reused in turn (so NVMTL_IMPORT_CACHE=1 can hit). ─────────────
#define IMP_N 8
#define IMP_SZ ((442u << 10) + 48u)
typedef struct { uint8_t *mem[IMP_N]; uint32_t *ptr[IMP_N]; int bad; } imp_st;
static void op_import_one(imp_st *s, long i)
{
	cl_int err; const int slot = (int)((i < 0 ? -i : i) % IMP_N); uint32_t *h = s->ptr[slot];
	h[0] = (uint32_t)i;
	cl_mem b = clCreateBuffer(ctx, CL_MEM_READ_WRITE | CL_MEM_USE_HOST_PTR, IMP_SZ, h, &err); CK(err);
	enq_touch(b, IMP_SZ / 4, 1, NULL);
	enq_touch(b, IMP_SZ / 4, 2, NULL);
	uint32_t *m = clEnqueueMapBuffer(q, b, CL_TRUE, CL_MAP_READ, 0, IMP_SZ, 0, NULL, NULL, &err); CK(err);
	if (m[0] != ((uint32_t)i * 3u + 1u) * 3u + 2u) s->bad++;
	CK(clEnqueueUnmapMemObject(q, b, m, 0, NULL, NULL));
	CK(clReleaseMemObject(b));
}
static void op_import(long i, void *p) { op_import_one(p, i); CK(clFinish(q)); }
// ─── mixed: Geekbench-like step: blocking 16 KB upload into a long-lived input, 1-4 kernels, every 4th op a temporary
// buffer, every 8th an import, completion by clWaitForEvents on the last kernel's event, then a 4-byte read. ──────────
typedef struct { cl_mem in, acc; uint32_t up[4096]; imp_st *imp; churn_st *ch; int bad; } mixed_st;
static void op_mixed(long i, void *p)
{
	mixed_st *s = p; cl_int err;
	for (int k = 0; k < 4096; k++) s->up[k] = (uint32_t)(i + k);
	CK(clEnqueueWriteBuffer(q, s->in, CL_TRUE, 0, sizeof s->up, s->up, 0, NULL, NULL));
	const uint32_t r = lcg(); const int nk = 1 + (int)(r & 3);
	cl_event ev = NULL;
	for (int k = 0; k < nk; k++) {
		if (ev) { CK(clReleaseEvent(ev)); ev = NULL; }
		if ((r >> (4 + k)) & 1) enq_touch(s->in, 4096, (cl_uint)k, &ev);
		else enq_touch(s->acc, 1u << (10 + ((r >> 12) & 7)), (cl_uint)k, &ev);   // 1K..128K elements
	}
	if ((i & 3) == 0) {
		cl_mem t = clCreateBuffer(ctx, CL_MEM_READ_WRITE, churn_sz[(r >> 16) % NCHURN], NULL, &err); CK(err);
		if (ev) { CK(clReleaseEvent(ev)); ev = NULL; }
		enq_touch(t, 16384, 3, &ev);
		CK(clReleaseMemObject(t));   // released while queued: freed when the kernel is done
	}
	CK(clWaitForEvents(1, &ev)); CK(clReleaseEvent(ev));
	if ((i & 7) == 0) op_import_one(s->imp, i);
	uint32_t v = 0; CK(clEnqueueReadBuffer(q, s->in, CL_TRUE, 0, 4, &v, 0, NULL, NULL));
	(void)v;
}
// ─── live: L buffers live (half 64 KB, half 4 MB + 64 KB, so half are BOs of their own); each op releases one,
// makes its replacement, runs a kernel on it and on another live one, clFinish. Levels 16, 64, 256, 1024. ───────────
typedef struct { cl_mem *b; int L; } live_st;
static size_t live_sz(int k) { return (k & 1) ? (4u << 20) + (64u << 10) : 64u << 10; }
static void op_live(long i, void *p)
{
	live_st *s = p; cl_int err; (void)i;
	const int v = (int)(lcg() % (uint32_t)s->L), w = (int)(lcg() % (uint32_t)s->L);
	CK(clReleaseMemObject(s->b[v]));
	s->b[v] = clCreateBuffer(ctx, CL_MEM_READ_WRITE, live_sz(v), NULL, &err); CK(err);
	enq_touch(s->b[v], 16384, 1, NULL);
	enq_touch(s->b[w], 16384, 2, NULL);
	CK(clFinish(q));
}

int main(int argc, char **argv)
{
	mach_timebase_info(&tb); setvbuf(stdout, NULL, _IOLBF, 0);
	double secs = 15; const char *modes = "chain,churn,import,mixed,live";
	for (int a = 1; a < argc; a++) {
		if (!strcmp(argv[a], "-t") && a + 1 < argc) secs = atof(argv[++a]);
		else if (!strcmp(argv[a], "-m") && a + 1 < argc) modes = argv[++a];
		else if (!strcmp(argv[a], "-q")) quiet = 1;
		else { printf("usage: %s [-t secs] [-m chain,churn,import,mixed,live] [-q]\n", argv[0]); return 2; }
	}
	cl_platform_id plat; CK(clGetPlatformIDs(1, &plat, NULL));
	cl_device_id devs[8]; cl_uint nd = 0; CK(clGetDeviceIDs(plat, CL_DEVICE_TYPE_GPU, 8, devs, &nd));
	int best = 0;
	for (cl_uint i = 0; i < nd; i++) {
		char name[256] = "", vendor[256] = ""; cl_uint vid = 0;
		clGetDeviceInfo(devs[i], CL_DEVICE_NAME, sizeof name, name, NULL);
		clGetDeviceInfo(devs[i], CL_DEVICE_VENDOR, sizeof vendor, vendor, NULL);
		clGetDeviceInfo(devs[i], CL_DEVICE_VENDOR_ID, sizeof vid, &vid, NULL);
		if (!quiet) printf("GPU %u: %s, vendor \"%s\" 0x%x\n", i, name, vendor, vid);
		const int rank = vid == 0x8086 ? 3 : (strstr(vendor, "Intel") || strstr(vendor, "intel")) ? 2 : 1;
		if (rank > best) { dev = devs[i]; best = rank; }
	}
	const char *pick = getenv("GBCL_DEVICE");
	if (pick) { cl_uint n = (cl_uint)atoi(pick); dev = n < nd ? devs[n] : NULL; }
	if (!dev) { printf("no OpenCL GPU\n"); return 2; }
	{ char name[256] = ""; clGetDeviceInfo(dev, CL_DEVICE_NAME, sizeof name, name, NULL);
	  printf("device: %s; %.0f s per mode; modes %s; NVMTL_IMPORT_UNALIGNED=%s\n", name, secs, modes,
	         getenv("NVMTL_IMPORT_UNALIGNED") ? getenv("NVMTL_IMPORT_UNALIGNED") : "unset"); }
	cl_int err;
	ctx = clCreateContext(NULL, 1, &dev, NULL, NULL, &err); CK(err);
	q = clCreateCommandQueue(ctx, dev, 0, &err); CK(err);
	cl_program pr = clCreateProgramWithSource(ctx, 1, &src, NULL, &err); CK(err);
	if (clBuildProgram(pr, 1, &dev, "", NULL, NULL)) {
		char log[8192] = ""; clGetProgramBuildInfo(pr, dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL); printf("build: %s\n", log); return 1; }
	k_tiny = clCreateKernel(pr, "tiny", &err); CK(err);
	k_touch = clCreateKernel(pr, "touch", &err); CK(err);
	latcap = 4 << 20; lat = malloc((size_t)latcap * 8);

	imp_st imp = { 0 };
	for (int k = 0; k < IMP_N; k++) {   // malloc-like alignment: 16 B past a page, plus 64 B per slot
		imp.mem[k] = NULL; if (posix_memalign((void **)&imp.mem[k], 4096, IMP_SZ + 8192)) return 1;
		imp.ptr[k] = (uint32_t *)(imp.mem[k] + 16 + 64 * k); memset(imp.ptr[k], 0, IMP_SZ);
	}
	char m[256]; snprintf(m, sizeof m, "%s", modes);
	for (char *tok = strtok(m, ","); tok; tok = strtok(NULL, ",")) {
		lcg_s = 12345;
		if (!strcmp(tok, "chain")) {
			chain_st s = { 0 };
			s.o = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 4096, NULL, &err); CK(err);
			s.m = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 65536 * 4, NULL, &err); CK(err);
			uint32_t z = 0; CK(clEnqueueWriteBuffer(q, s.o, CL_TRUE, 0, 4, &z, 0, NULL, NULL));
			// the check needs the sum after the run, so it is printed on its own line
			run("chain", op_chain, &s, secs, NULL);
			uint32_t r = 0; CK(clEnqueueReadBuffer(q, s.o, CL_TRUE, 0, 4, &r, 0, NULL, NULL));
			if (r != (uint32_t)s.sum) printf("CHECK chain: o[0] %u, expected %u\n", r, (uint32_t)s.sum);
			clReleaseMemObject(s.o); clReleaseMemObject(s.m);
		} else if (!strcmp(tok, "churn")) {
			churn_st s = { 0 }; run("churn", op_churn, &s, secs, NULL);
			if (s.bad) printf("CHECK churn: %d wrong read-backs\n", s.bad);
		} else if (!strcmp(tok, "import")) {
			imp.bad = 0; run("import", op_import, &imp, secs, NULL);
			if (imp.bad) printf("CHECK import: %d wrong results\n", imp.bad);
		} else if (!strcmp(tok, "mixed")) {
			churn_st ch = { 0 }; mixed_st s = { 0 }; s.imp = &imp; s.ch = &ch; imp.bad = 0;
			s.in = clCreateBuffer(ctx, CL_MEM_READ_WRITE, sizeof s.up, NULL, &err); CK(err);
			s.acc = clCreateBuffer(ctx, CL_MEM_READ_WRITE, 128 << 12, NULL, &err); CK(err);
			run("mixed", op_mixed, &s, secs, NULL);
			if (imp.bad) printf("CHECK mixed: %d wrong import results\n", imp.bad);
			clReleaseMemObject(s.in); clReleaseMemObject(s.acc);
		} else if (!strcmp(tok, "live")) {
			static const int levels[] = { 16, 64, 256, 1024 };
			for (int li = 0; li < 4; li++) {
				live_st s = { calloc((size_t)levels[li], sizeof(cl_mem)), levels[li] };
				for (int k = 0; k < s.L; k++) {
					s.b[k] = clCreateBuffer(ctx, CL_MEM_READ_WRITE, live_sz(k), NULL, &err); CK(err);
					enq_touch(s.b[k], 16384, 0, NULL);   // every one used once, so each is resident before timing
					if ((k & 63) == 63) CK(clFinish(q));
				}
				CK(clFinish(q));
				char nm[32]; snprintf(nm, sizeof nm, "live%d", s.L);
				run(nm, op_live, &s, secs / 4, NULL);
				for (int k = 0; k < s.L; k++) clReleaseMemObject(s.b[k]);
				CK(clFinish(q)); free(s.b);
			}
		} else { printf("unknown mode %s\n", tok); return 2; }
	}
	return 0;
}
