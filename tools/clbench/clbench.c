// SPDX-License-Identifier: MIT
// clbench: the same OpenCL C kernels on any OpenCL GPU, timed with profiling events. Built for Windows (Intel's own
// OpenCL, compiler IGC) and for macOS (Apple's OpenCL front end on this project's Metal driver), so one card can be
// compared kernel by kernel: where the time differs, the code the two compilers made differs.
//
//   clbench [device-substring]     the first GPU whose name contains it; default (or none matches): Intel's GPU (below)
//   CLBENCH_PLATFORM=n             only platform n (Linux has two OpenCLs for the card: Mesa's rusticl and Intel's)
//
// Writes the results to stdout and clbench-<os>.txt, and on request (CLBENCH_DUMP=1) the program binary to
// clbench-<os>.bin. Windows: x86_64-w64-mingw32-gcc -O2 clbench.c -o clbench.exe (OpenCL.dll is loaded at run time).
// macOS: clang -O2 clbench.c -o clbench (OpenCL.framework is loaded at run time). Linux: cc -O2 clbench.c -o clbench-linux -ldl -lm
// (RUSTICL_ENABLE=iris turns Mesa's rusticl on for Intel).
// The default device, best first: a GPU with Intel's vendor id (CL_DEVICE_VENDOR_ID 0x8086) whose PCI device id
// (CL_DEVICE_ID_INTEL, cl_intel_device_attribute_query: Intel's runtime) is a card this driver supports; any GPU with
// vendor id 0x8086; a GPU whose CL_DEVICE_VENDOR names Intel (Apple's OpenCL reports vendor ids of its own, not PCI
// ones); the first GPU.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <math.h>
#ifdef _WIN32
#include <windows.h>
#define OS "windows"
#else
#include <dlfcn.h>
#include <time.h>
#ifdef __linux__
#define OS "linux"
#else
#define OS "macos"
#endif
#endif

typedef int32_t cl_int; typedef uint32_t cl_uint; typedef uint64_t cl_ulong; typedef cl_ulong cl_bitfield;
typedef struct _p *cl_platform_id; typedef struct _d *cl_device_id; typedef struct _c *cl_context; typedef struct _q *cl_command_queue;
typedef struct _m *cl_mem; typedef struct _g *cl_program; typedef struct _k *cl_kernel; typedef struct _e *cl_event;
#define CL_DEVICE_TYPE_GPU (1 << 2)
#define CL_PLATFORM_NAME 0x0902
#define CL_PLATFORM_VERSION 0x0901
#define CL_DEVICE_VENDOR_ID 0x1001
#define CL_DEVICE_MAX_COMPUTE_UNITS 0x1002
#define CL_DEVICE_MAX_CLOCK_FREQUENCY 0x100C
#define CL_DEVICE_NAME 0x102B
#define CL_DEVICE_VENDOR 0x102C
#define CL_DRIVER_VERSION 0x102D
#define CL_DEVICE_VERSION 0x102F
#define CL_QUEUE_PROFILING_ENABLE (1 << 1)
#define CL_MEM_READ_WRITE (1 << 0)
#define CL_PROGRAM_BUILD_LOG 0x1183
#define CL_PROGRAM_BINARY_SIZES 0x1165
#define CL_PROGRAM_BINARIES 0x1166
#define CL_KERNEL_WORK_GROUP_SIZE 0x11B0
#define CL_KERNEL_LOCAL_MEM_SIZE 0x11B2
#define CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE 0x11B3
#define CL_KERNEL_PRIVATE_MEM_SIZE 0x11B4
#define CL_PROFILING_COMMAND_START 0x1282
#define CL_PROFILING_COMMAND_END 0x1283
#define CL_DEVICE_ID_INTEL 0x4251   /* cl_intel_device_attribute_query */

// The cards this driver supports: PCI device ids, vendor 8086 (data/platforms.json "supported"; tools/parity/devices.py)
static const cl_uint kSupported[] = { 0xe20b, 0xe20c, 0xe211, 0xe212 };

#define FNS(X) \
    X(cl_int, clGetPlatformIDs, (cl_uint, cl_platform_id *, cl_uint *)) \
    X(cl_int, clGetPlatformInfo, (cl_platform_id, cl_uint, size_t, void *, size_t *)) \
    X(cl_int, clGetDeviceIDs, (cl_platform_id, cl_bitfield, cl_uint, cl_device_id *, cl_uint *)) \
    X(cl_int, clGetDeviceInfo, (cl_device_id, cl_uint, size_t, void *, size_t *)) \
    X(cl_context, clCreateContext, (const intptr_t *, cl_uint, const cl_device_id *, void *, void *, cl_int *)) \
    X(cl_command_queue, clCreateCommandQueue, (cl_context, cl_device_id, cl_bitfield, cl_int *)) \
    X(cl_mem, clCreateBuffer, (cl_context, cl_bitfield, size_t, void *, cl_int *)) \
    X(cl_program, clCreateProgramWithSource, (cl_context, cl_uint, const char **, const size_t *, cl_int *)) \
    X(cl_int, clBuildProgram, (cl_program, cl_uint, const cl_device_id *, const char *, void *, void *)) \
    X(cl_int, clGetProgramBuildInfo, (cl_program, cl_device_id, cl_uint, size_t, void *, size_t *)) \
    X(cl_int, clGetProgramInfo, (cl_program, cl_uint, size_t, void *, size_t *)) \
    X(cl_kernel, clCreateKernel, (cl_program, const char *, cl_int *)) \
    X(cl_int, clSetKernelArg, (cl_kernel, cl_uint, size_t, const void *)) \
    X(cl_int, clGetKernelWorkGroupInfo, (cl_kernel, cl_device_id, cl_uint, size_t, void *, size_t *)) \
    X(cl_int, clEnqueueNDRangeKernel, (cl_command_queue, cl_kernel, cl_uint, const size_t *, const size_t *, const size_t *, cl_uint, const cl_event *, cl_event *)) \
    X(cl_int, clEnqueueWriteBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, const void *, cl_uint, const cl_event *, cl_event *)) \
    X(cl_int, clEnqueueReadBuffer, (cl_command_queue, cl_mem, cl_uint, size_t, size_t, void *, cl_uint, const cl_event *, cl_event *)) \
    X(cl_int, clWaitForEvents, (cl_uint, const cl_event *)) \
    X(cl_int, clGetEventProfilingInfo, (cl_event, cl_uint, size_t, void *, size_t *)) \
    X(cl_int, clReleaseEvent, (cl_event)) \
    X(cl_int, clFinish, (cl_command_queue))
#define DECL(r, n, a) static r (*p_##n) a;
FNS(DECL)

static FILE *g_out;
static void say(const char *fmt, ...) {
    va_list a; va_start(a, fmt); vprintf(fmt, a); va_end(a);
    if (g_out) { va_start(a, fmt); vfprintf(g_out, fmt, a); va_end(a); fflush(g_out); }
    fflush(stdout);
}
static double now_s(void) {
#ifdef _WIN32
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c); return (double)c.QuadPart / f.QuadPart;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9;
#endif
}
static int load(void) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA("OpenCL.dll"); if (!h) return 0;
#define LD(r, n, a) p_##n = (r (*) a)(void *)GetProcAddress(h, #n); if (!p_##n) return 0;
#else
#ifdef __linux__
    void *h = dlopen("libOpenCL.so.1", RTLD_NOW); if (!h) h = dlopen("libOpenCL.so", RTLD_NOW); if (!h) return 0;
#else
    void *h = dlopen("/System/Library/Frameworks/OpenCL.framework/OpenCL", RTLD_NOW); if (!h) return 0;
#endif
#define LD(r, n, a) p_##n = (r (*) a)dlsym(h, #n); if (!p_##n) return 0;
#endif
    FNS(LD)
    return 1;
}

static const char *SRC =
"__kernel void fma_ind(__global float *o, float a, float b) {\n"
"    const uint g = get_global_id(0); float x0 = g, x1 = g + 1, x2 = g + 2, x3 = g + 3, x4 = g + 4, x5 = g + 5, x6 = g + 6, x7 = g + 7;\n"
"    float y0 = -x0, y1 = -x1, y2 = -x2, y3 = -x3, y4 = -x4, y5 = -x5, y6 = -x6, y7 = -x7;\n"
"    for (int i = 0; i < 256; i++) {\n"
"        x0 = fma(x0, a, b); x1 = fma(x1, a, b); x2 = fma(x2, a, b); x3 = fma(x3, a, b); x4 = fma(x4, a, b); x5 = fma(x5, a, b); x6 = fma(x6, a, b); x7 = fma(x7, a, b);\n"
"        y0 = fma(y0, a, b); y1 = fma(y1, a, b); y2 = fma(y2, a, b); y3 = fma(y3, a, b); y4 = fma(y4, a, b); y5 = fma(y5, a, b); y6 = fma(y6, a, b); y7 = fma(y7, a, b); }\n"
"    o[g] = x0 + x1 + x2 + x3 + x4 + x5 + x6 + x7 + y0 + y1 + y2 + y3 + y4 + y5 + y6 + y7; }\n"
"__kernel void copy4(__global const float4 *a, __global float4 *b) { const uint i = get_global_id(0); b[i] = a[i]; }\n"
"__kernel void scale_args(__global const float *x, __global float *y, float a, float b, float c, float d, int n, int s, float e, float f) {\n"
"    const int i = get_global_id(0); if (i >= n) return; const float v = x[(i * s) % n]; y[i] = fma(v, a, b) * c + d * e - f; }\n"
"__kernel void tiny(__global int *o) { if (get_global_id(0) == 0) o[0] += 1; }\n"
"#define TS 16\n"
"__kernel void sgemm_tile(int N, __global const float *A, __global const float *B, __global float *C) {\n"
"    __local float As[TS][TS], Bs[TS][TS]; const int r = get_local_id(1), c = get_local_id(0), row = get_global_id(1), col = get_global_id(0);\n"
"    float acc = 0; for (int t = 0; t < N; t += TS) {\n"
"        As[r][c] = A[row * N + t + c]; Bs[r][c] = B[(t + r) * N + col]; barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (int k = 0; k < TS; k++) acc = fma(As[r][k], Bs[k][c], acc);\n"
"        barrier(CLK_LOCAL_MEM_FENCE); }\n"
"    C[row * N + col] = acc; }\n"
"#define BM 64\n#define BN 64\n#define BK 16\n"
"__kernel __attribute__((reqd_work_group_size(16, 16, 1))) void sgemm_reg(int N, __global const float *A, __global const float *B, __global float *C) {\n"
"    __local float As[BK][BM + 4], Bs[BK][BN + 4]; const int tx = get_local_id(0), ty = get_local_id(1), lid = ty * 16 + tx;\n"
"    const int m0 = get_group_id(1) * BM, n0 = get_group_id(0) * BN; float acc[4][4] = {{0}};\n"
"    for (int k0 = 0; k0 < N; k0 += BK) {\n"
"        for (int i = 0; i < 4; i++) { const int e = lid + i * 256; const int am = e / BK, ak = e % BK; As[ak][am] = A[(m0 + am) * N + k0 + ak];\n"
"                                      const int bk = e / BN, bn = e % BN; Bs[bk][bn] = B[(k0 + bk) * N + n0 + bn]; }\n"
"        barrier(CLK_LOCAL_MEM_FENCE);\n"
"        for (int kk = 0; kk < BK; kk++) { float a[4], b[4];\n"
"            for (int i = 0; i < 4; i++) a[i] = As[kk][ty * 4 + i]; for (int j = 0; j < 4; j++) b[j] = Bs[kk][tx * 4 + j];\n"
"            for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) acc[i][j] = fma(a[i], b[j], acc[i][j]); }\n"
"        barrier(CLK_LOCAL_MEM_FENCE); }\n"
"    for (int i = 0; i < 4; i++) for (int j = 0; j < 4; j++) C[(m0 + ty * 4 + i) * N + n0 + tx * 4 + j] = acc[i][j]; }\n"
"__kernel void conv3x3_nhwc(int W, int H, int CI, int CO, __global const float *in, __global const float *wt, __global const float *bias, __global float *out) {\n"
"    const int co = get_global_id(0), x = get_global_id(1), y = get_global_id(2); if (co >= CO) return; float acc = bias[co];\n"
"    for (int ky = 0; ky < 3; ky++) { const int iy = y + ky - 1; if (iy < 0 || iy >= H) continue;\n"
"        for (int kx = 0; kx < 3; kx++) { const int ix = x + kx - 1; if (ix < 0 || ix >= W) continue;\n"
"            const __global float *ip = in + (iy * W + ix) * CI; const __global float *wp = wt + ((ky * 3 + kx) * CI) * CO + co;\n"
"            for (int ci = 0; ci < CI; ci++) acc = fma(ip[ci], wp[ci * CO], acc); } }\n"
"    out[(y * W + x) * CO + co] = acc; }\n"
"__kernel void dwconv3x3(int W, int H, int C, __global const float *in, __global const float *wt, __global float *out) {\n"
"    const int c = get_global_id(0), x = get_global_id(1), y = get_global_id(2); if (c >= C) return; float acc = 0;\n"
"    for (int ky = 0; ky < 3; ky++) for (int kx = 0; kx < 3; kx++) { const int ix = x + kx - 1, iy = y + ky - 1;\n"
"        if (ix >= 0 && iy >= 0 && ix < W && iy < H) acc = fma(in[(iy * W + ix) * C + c], wt[(ky * 3 + kx) * C + c], acc); }\n"
"    out[(y * W + x) * C + c] = fmax(acc, 0.0f); }\n"
"__kernel void reduce_sum(__global const float *x, __global float *part, int n) {\n"
"    __local float s[256]; const int l = get_local_id(0); float v = 0;\n"
"    for (int i = get_global_id(0); i < n; i += get_global_size(0)) v += x[i];\n"
"    s[l] = v; barrier(CLK_LOCAL_MEM_FENCE);\n"
"    for (int k = 128; k > 0; k >>= 1) { if (l < k) s[l] += s[l + k]; barrier(CLK_LOCAL_MEM_FENCE); }\n"
"    if (l == 0) part[get_group_id(0)] = s[0]; }\n"
"__kernel void blur_h(int W, int H, __global const float *in, __global float *out) {\n"
"    const int x = get_global_id(0), y = get_global_id(1); if (x >= W) return; const float w[9] = {0.0162f, 0.0540f, 0.1216f, 0.1945f, 0.2270f, 0.1945f, 0.1216f, 0.0540f, 0.0162f};\n"
"    float a = 0; for (int k = -4; k <= 4; k++) a += w[k + 4] * in[y * W + clamp(x + k, 0, W - 1)]; out[y * W + x] = a; }\n"
"__kernel void blur_v(int W, int H, __global const float *in, __global float *out) {\n"
"    const int x = get_global_id(0), y = get_global_id(1); if (x >= W) return; const float w[9] = {0.0162f, 0.0540f, 0.1216f, 0.1945f, 0.2270f, 0.1945f, 0.1216f, 0.0540f, 0.0162f};\n"
"    float a = 0; for (int k = -4; k <= 4; k++) a += w[k + 4] * in[clamp(y + k, 0, H - 1) * W + x]; out[y * W + x] = a; }\n"
"__kernel void hist256(__global const uchar *img, __global volatile int *h, int n) {\n"
"    __local int lh[256]; const int l = get_local_id(0); lh[l] = 0; barrier(CLK_LOCAL_MEM_FENCE);\n"
"    for (int i = get_global_id(0); i < n; i += get_global_size(0)) atomic_inc(&lh[img[i]]);\n"
"    barrier(CLK_LOCAL_MEM_FENCE); atomic_add(&h[l], lh[l]); }\n";

static cl_device_id g_dev; static cl_context g_ctx; static cl_command_queue g_q; static cl_program g_prog;
static cl_mem buf(size_t n) { cl_int e; cl_mem m = p_clCreateBuffer(g_ctx, CL_MEM_READ_WRITE, n, NULL, &e); if (e) { say("clCreateBuffer(%zu) failed %d\n", n, e); exit(1); } return m; }
static void fill(cl_mem m, size_t n, float scale) {
    float *h = malloc(n); for (size_t i = 0; i < n / 4; i++) h[i] = (float)((i * 2654435761u) % 1000) * scale / 1000.0f;
    p_clEnqueueWriteBuffer(g_q, m, 1, 0, n, h, 0, NULL, NULL); free(h);
}
static cl_kernel kern(const char *n) {
    cl_int e; cl_kernel k = p_clCreateKernel(g_prog, n, &e); if (e) { say("clCreateKernel(%s) failed %d\n", n, e); exit(1); }
    size_t wg = 0, mul = 0; cl_ulong lm = 0, pm = 0;
    p_clGetKernelWorkGroupInfo(k, g_dev, CL_KERNEL_WORK_GROUP_SIZE, sizeof wg, &wg, NULL);
    p_clGetKernelWorkGroupInfo(k, g_dev, CL_KERNEL_PREFERRED_WORK_GROUP_SIZE_MULTIPLE, sizeof mul, &mul, NULL);
    p_clGetKernelWorkGroupInfo(k, g_dev, CL_KERNEL_LOCAL_MEM_SIZE, sizeof lm, &lm, NULL);
    p_clGetKernelWorkGroupInfo(k, g_dev, CL_KERNEL_PRIVATE_MEM_SIZE, sizeof pm, &pm, NULL);
    say("  kernel %-14s max wg %zu, preferred multiple %zu, local %llu B, private %llu B\n", n, wg, mul, (unsigned long long)lm, (unsigned long long)pm);
    return k;
}
#define ARG(k, i, v) do { __typeof__(v) v_ = (v); cl_int e_ = p_clSetKernelArg(k, i, sizeof v_, &v_); if (e_) { say("arg %d failed %d\n", i, e_); exit(1); } } while (0)
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return x < y ? -1 : x > y; }
// median GPU time (us) of `reps` runs after 3 warm-ups
static double timed(cl_kernel k, cl_uint dim, const size_t *gs, const size_t *ls, int reps) {
    double t[64]; if (reps > 64) reps = 64;
    for (int i = 0; i < 3 + reps; i++) {
        cl_event ev; cl_int e = p_clEnqueueNDRangeKernel(g_q, k, dim, NULL, gs, ls, 0, NULL, &ev); if (e) { say("enqueue failed %d\n", e); exit(1); }
        p_clWaitForEvents(1, &ev); cl_ulong s = 0, en = 0;
        p_clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_START, sizeof s, &s, NULL); p_clGetEventProfilingInfo(ev, CL_PROFILING_COMMAND_END, sizeof en, &en, NULL);
        p_clReleaseEvent(ev); if (i >= 3) t[i - 3] = (en - s) / 1000.0;
    }
    qsort(t, reps, sizeof t[0], cmpd); return t[reps / 2];
}

int main(int argc, char **argv) {
    char path[64]; snprintf(path, sizeof path, "clbench-%s.txt", OS); g_out = fopen(path, "w");
    if (!load()) { say("OpenCL library not found\n"); return 1; }
    cl_platform_id plats[8]; cl_uint np = 0; p_clGetPlatformIDs(8, plats, &np);
    const char *want = argc > 1 ? argv[1] : NULL; cl_device_id pick = NULL; int pickrank = 0;
    const char *onlyp = getenv("CLBENCH_PLATFORM");   // one platform's devices only (Linux: rusticl or Intel's runtime)
    for (cl_uint i = 0; i < np; i++) {
        if (onlyp && (cl_uint)atoi(onlyp) != i) continue;
        char pn[256] = ""; p_clGetPlatformInfo(plats[i], CL_PLATFORM_NAME, sizeof pn, pn, NULL);
        cl_device_id ds[8]; cl_uint nd = 0; if (p_clGetDeviceIDs(plats[i], CL_DEVICE_TYPE_GPU, 8, ds, &nd)) continue;
        for (cl_uint j = 0; j < nd; j++) { char dn[256] = "", dv[256] = ""; cl_uint vid = 0, pci = 0;
            p_clGetDeviceInfo(ds[j], CL_DEVICE_NAME, sizeof dn, dn, NULL); p_clGetDeviceInfo(ds[j], CL_DEVICE_VENDOR, sizeof dv, dv, NULL);
            p_clGetDeviceInfo(ds[j], CL_DEVICE_VENDOR_ID, sizeof vid, &vid, NULL);
            if (p_clGetDeviceInfo(ds[j], CL_DEVICE_ID_INTEL, sizeof pci, &pci, NULL)) pci = 0;   // not Intel's runtime
            say("platform %u \"%s\" device %u \"%s\", vendor \"%s\" 0x%x", i, pn, j, dn, dv, vid);
            if (pci) say(", PCI device 0x%04x", pci);
            say("\n");
            int supported = 0; for (size_t k = 0; k < sizeof kSupported / sizeof *kSupported; k++) supported |= pci == kSupported[k];
            const int rank = want && strstr(dn, want) ? 5 : vid == 0x8086 && supported ? 4 : vid == 0x8086 ? 3
                : (strstr(dv, "Intel") || strstr(dv, "intel")) ? 2 : 1;
            if (rank > pickrank) { pick = ds[j]; pickrank = rank; } }
    }
    g_dev = pick; if (!g_dev) { say("no GPU\n"); return 1; }
    { char dn[256] = "", dv[256] = "", vv[256] = ""; cl_uint cu = 0, mhz = 0;
      p_clGetDeviceInfo(g_dev, CL_DEVICE_NAME, sizeof dn, dn, NULL); p_clGetDeviceInfo(g_dev, CL_DRIVER_VERSION, sizeof dv, dv, NULL);
      p_clGetDeviceInfo(g_dev, CL_DEVICE_VERSION, sizeof vv, vv, NULL); p_clGetDeviceInfo(g_dev, CL_DEVICE_MAX_COMPUTE_UNITS, 4, &cu, NULL);
      p_clGetDeviceInfo(g_dev, CL_DEVICE_MAX_CLOCK_FREQUENCY, 4, &mhz, NULL);
      say("== clbench (%s) on \"%s\": %s, driver %s, %u compute units, %u MHz\n", OS, dn, vv, dv, cu, mhz); }
    cl_int e; g_ctx = p_clCreateContext(NULL, 1, &g_dev, NULL, NULL, &e); if (e) { say("context %d\n", e); return 1; }
    g_q = p_clCreateCommandQueue(g_ctx, g_dev, CL_QUEUE_PROFILING_ENABLE, &e); if (e) { say("queue %d\n", e); return 1; }
    g_prog = p_clCreateProgramWithSource(g_ctx, 1, &SRC, NULL, &e);
    double tb = now_s(); e = p_clBuildProgram(g_prog, 1, &g_dev, "-cl-std=CL1.2", NULL, NULL); double tb2 = now_s();
    { char log[16384] = ""; p_clGetProgramBuildInfo(g_prog, g_dev, CL_PROGRAM_BUILD_LOG, sizeof log, log, NULL);
      say("build %s in %.0f ms%s%s\n", e ? "FAILED" : "ok", (tb2 - tb) * 1000, log[0] ? ", log:\n" : "", log); if (e) return 1; }
    if (getenv("CLBENCH_DUMP")) { size_t sz = 0; p_clGetProgramInfo(g_prog, CL_PROGRAM_BINARY_SIZES, sizeof sz, &sz, NULL);
        unsigned char *b = malloc(sz ? sz : 1); unsigned char *bp[1] = { b }; p_clGetProgramInfo(g_prog, CL_PROGRAM_BINARIES, sizeof bp, bp, NULL);
        snprintf(path, sizeof path, "clbench-%s.bin", OS); FILE *f = fopen(path, "wb"); if (f) { fwrite(b, 1, sz, f); fclose(f); } say("program binary: %zu bytes -> %s\n", sz, path); free(b); }

    say("-- kernels\n");
    // compute peak
    { cl_kernel k = kern("fma_ind"); const size_t n = 20 * 160 * 1024; cl_mem o = buf(n * 4); ARG(k, 0, o); ARG(k, 1, 0.999f); ARG(k, 2, 0.001f);
      size_t gs = n, ls = 256; double us = timed(k, 1, &gs, &ls, 9); say("fma_ind      %9.1f us  %7.0f GFLOP/s\n", us, n * 16.0 * 256 * 2 / us / 1e3); }
    // bandwidth
    { cl_kernel k = kern("copy4"); const size_t bytes = 256u << 20; cl_mem a = buf(bytes), b = buf(bytes); fill(a, bytes, 1); ARG(k, 0, a); ARG(k, 1, b);
      size_t gs = bytes / 16, ls = 256; double us = timed(k, 1, &gs, &ls, 9); say("copy4 256MB  %9.1f us  %7.0f GB/s (read+write)\n", us, 2.0 * bytes / us / 1e3); }
    // scalar arguments
    { cl_kernel k = kern("scale_args"); const int n = 1 << 20; cl_mem x = buf(n * 4), y = buf(n * 4); fill(x, n * 4, 1);
      ARG(k, 0, x); ARG(k, 1, y); ARG(k, 2, 1.1f); ARG(k, 3, 0.1f); ARG(k, 4, 0.9f); ARG(k, 5, 0.2f); ARG(k, 6, n); ARG(k, 7, 7); ARG(k, 8, 0.3f); ARG(k, 9, 0.05f);
      size_t gs = n, ls = 256; double us = timed(k, 1, &gs, &ls, 15); say("scale_args1M %9.1f us\n", us); }
    // GEMM, two styles
    { const int N = 2048; cl_mem A = buf((size_t)N * N * 4), B = buf((size_t)N * N * 4), C = buf((size_t)N * N * 4); fill(A, (size_t)N * N * 4, 1); fill(B, (size_t)N * N * 4, 1);
      cl_kernel k = kern("sgemm_tile"); ARG(k, 0, N); ARG(k, 1, A); ARG(k, 2, B); ARG(k, 3, C);
      size_t gs[2] = { N, N }, ls[2] = { 16, 16 }; double us = timed(k, 2, gs, ls, 5); say("sgemm_tile   %9.1f us  %7.0f GFLOP/s (2048^3, 16x16 tiles)\n", us, 2.0 * N * N * N / us / 1e3);
      cl_kernel k2 = kern("sgemm_reg"); ARG(k2, 0, N); ARG(k2, 1, A); ARG(k2, 2, B); ARG(k2, 3, C);
      size_t gs2[2] = { N / 4, N / 4 }; us = timed(k2, 2, gs2, ls, 5); say("sgemm_reg    %9.1f us  %7.0f GFLOP/s (2048^3, 64x64 blocks, 4x4 per thread)\n", us, 2.0 * N * N * N / us / 1e3); }
    // convolutions (Super Resolution's main layer shape, and a depthwise one)
    { const int W = 48, H = 48, CI = 64, CO = 64; cl_mem in = buf(W * H * CI * 4), wt = buf(9 * CI * CO * 4), bi = buf(CO * 4), out = buf(W * H * CO * 4);
      fill(in, W * H * CI * 4, 1); fill(wt, 9 * CI * CO * 4, 0.1f); fill(bi, CO * 4, 0.01f);
      cl_kernel k = kern("conv3x3_nhwc"); ARG(k, 0, W); ARG(k, 1, H); ARG(k, 2, CI); ARG(k, 3, CO); ARG(k, 4, in); ARG(k, 5, wt); ARG(k, 6, bi); ARG(k, 7, out);
      size_t gs[3] = { CO, W, H }, ls[3] = { 64, 4, 1 }; double us = timed(k, 3, gs, ls, 15);
      say("conv3x3 48x48x64->64 %9.1f us  %7.0f GFLOP/s\n", us, 2.0 * W * H * CI * CO * 9 / us / 1e3); }
    { const int W = 128, H = 72, C = 144; cl_mem in = buf(W * H * C * 4), wt = buf(9 * C * 4), out = buf(W * H * C * 4); fill(in, W * H * C * 4, 1); fill(wt, 9 * C * 4, 0.1f);
      cl_kernel k = kern("dwconv3x3"); ARG(k, 0, W); ARG(k, 1, H); ARG(k, 2, C); ARG(k, 3, in); ARG(k, 4, wt); ARG(k, 5, out);
      size_t gs[3] = { 144, W, H }, ls[3] = { 16, 8, 2 }; double us = timed(k, 3, gs, ls, 15); say("dwconv3x3 128x72x144 %9.1f us\n", us); }
    // reduction, blur, histogram (local memory, barriers, local atomics)
    { const int n = 64 << 20; cl_mem x = buf((size_t)n * 4), part = buf(4096 * 4); fill(x, (size_t)n * 4, 1);
      cl_kernel k = kern("reduce_sum"); ARG(k, 0, x); ARG(k, 1, part); ARG(k, 2, n); size_t gs = 4096 * 256, ls = 256;
      double us = timed(k, 1, &gs, &ls, 9); say("reduce 256MB %9.1f us  %7.0f GB/s\n", us, (double)n * 4 / us / 1e3); }
    { const int W = 3840, H = 2160; cl_mem a = buf((size_t)W * H * 4), b = buf((size_t)W * H * 4); fill(a, (size_t)W * H * 4, 1);
      cl_kernel kh = kern("blur_h"), kv = kern("blur_v"); ARG(kh, 0, W); ARG(kh, 1, H); ARG(kh, 2, a); ARG(kh, 3, b); ARG(kv, 0, W); ARG(kv, 1, H); ARG(kv, 2, b); ARG(kv, 3, a);
      size_t gs[2] = { W, H }, ls[2] = { 64, 4 }; double uh = timed(kh, 2, gs, ls, 9), uv = timed(kv, 2, gs, ls, 9);
      say("blur 4K h/v  %9.1f / %.1f us\n", uh, uv); }
    { const int n = 64 << 20; cl_mem img = buf(n), h = buf(256 * 4); fill(img, n, 255); cl_kernel k = kern("hist256"); ARG(k, 0, img); ARG(k, 1, h); ARG(k, 2, n);
      size_t gs = 1024 * 256, ls = 256; double us = timed(k, 1, &gs, &ls, 9); say("hist256 64MB %9.1f us\n", us); }
    // dispatch cost: GPU time of a tiny kernel, wall time per kernel when many are queued, and one enqueue+finish round trip
    { cl_kernel k = kern("tiny"); cl_mem o = buf(64); ARG(k, 0, o); size_t gs = 64, ls = 64;
      double us = timed(k, 1, &gs, &ls, 31);
      p_clFinish(g_q); double t0 = now_s(); for (int i = 0; i < 2000; i++) p_clEnqueueNDRangeKernel(g_q, k, 1, NULL, &gs, &ls, 0, NULL, NULL); p_clFinish(g_q); double t1 = now_s();
      double rt[64]; for (int i = 0; i < 64; i++) { double a = now_s(); p_clEnqueueNDRangeKernel(g_q, k, 1, NULL, &gs, &ls, 0, NULL, NULL); p_clFinish(g_q); rt[i] = (now_s() - a) * 1e6; }
      qsort(rt, 64, sizeof rt[0], cmpd);
      say("tiny kernel  %9.1f us GPU; 2000 queued: %.1f us each (wall); enqueue+finish round trip median %.1f us\n", us, (t1 - t0) * 1e6 / 2000, rt[32]); }
    say("== done\n");
    return 0;
}
