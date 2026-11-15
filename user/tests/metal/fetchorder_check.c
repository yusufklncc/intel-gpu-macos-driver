// SPDX-License-Identifier: MIT
// fetchorder_check: the plugin's framebuffer-fetch ordering analysis (nullmoth/plugin/nvmtl_fetch_order.h), off the GPU.
//   - for every SPIR-V module given: does it write memory ("writes 2": buffer memory - a render pipeline with such a stage
//     turns the CPU-side check off for the process; "writes 1": only images), and for vertex modules (file name containing ".vert.") does the
//     certifier accept it (position = M * attribute + b with draw-uniform M, b), with the attribute's Location;
//   - the certifier on small modules made to fool it (fetchorder_spv/*.spvasm) and on truncated / corrupted copies of them;
//   - the triangle partition on fixed meshes (nine-part, overlapping quads, T-junction, diagonal neighbours, a shared
//     edge with both third vertices on one side), each against its expected runs.
//
//   clang -O1 -g -Wall -I nullmoth/plugin -o fetchorder_check user/tests/metal/fetchorder_check.c
//   ./fetchorder_check [module.spv ...]
// Exit: 0 every geometry case as expected, 1 a geometry case differs, 2 usage or I/O.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_fetch_order.h"

/* certifier cases: user/tests/metal/fetchorder_spv/<name>.spvasm through spirv-as --target-env vulkan1.2 (words below) */
static const uint32_t k_good[] = {
    0x07230203, 0x00010500, 0x00070000, 0x00000016, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00000003, 0x00000004, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00040047, 0x00000003,
    0x0000000b, 0x00000000, 0x00030047, 0x00000005, 0x00000002, 0x00050048, 0x00000005, 0x00000000,
    0x00000023, 0x00000000, 0x00040048, 0x00000005, 0x00000000, 0x00000005, 0x00050048, 0x00000005,
    0x00000000, 0x00000007, 0x00000010, 0x00020013, 0x00000006, 0x00030021, 0x00000007, 0x00000006,
    0x00030016, 0x00000008, 0x00000020, 0x00040017, 0x00000009, 0x00000008, 0x00000004, 0x00040018,
    0x0000000a, 0x00000009, 0x00000004, 0x0003001e, 0x00000005, 0x0000000a, 0x00040020, 0x0000000b,
    0x00000009, 0x00000005, 0x00040020, 0x0000000c, 0x00000009, 0x0000000a, 0x00040020, 0x0000000d,
    0x00000001, 0x00000009, 0x00040020, 0x0000000e, 0x00000003, 0x00000009, 0x00040015, 0x0000000f,
    0x00000020, 0x00000001, 0x0004002b, 0x0000000f, 0x00000010, 0x00000000, 0x0004003b, 0x0000000b,
    0x00000004, 0x00000009, 0x0004003b, 0x0000000d, 0x00000002, 0x00000001, 0x0004003b, 0x0000000e,
    0x00000003, 0x00000003, 0x00050036, 0x00000006, 0x00000001, 0x00000000, 0x00000007, 0x000200f8,
    0x00000011, 0x00050041, 0x0000000c, 0x00000012, 0x00000004, 0x00000010, 0x0004003d, 0x0000000a,
    0x00000013, 0x00000012, 0x0004003d, 0x00000009, 0x00000014, 0x00000002, 0x00050091, 0x00000009,
    0x00000015, 0x00000013, 0x00000014, 0x0003003e, 0x00000003, 0x00000015, 0x000100fd, 0x00010038,
};
static const uint32_t k_ptrload[] = {
    0x07230203, 0x00010500, 0x00070000, 0x0000002a, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x000014e3, 0x00020011, 0x0000000b, 0x0009000a, 0x5f565053, 0x5f52484b, 0x73796870, 0x6c616369,
    0x6f74735f, 0x65676172, 0x6675625f, 0x00726566, 0x0003000e, 0x000014e4, 0x00000001, 0x0009000f,
    0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002, 0x00000003, 0x00000004, 0x00000005,
    0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00040047, 0x00000003, 0x0000000b, 0x00000000,
    0x00040047, 0x00000004, 0x0000000b, 0x0000002a, 0x00030047, 0x00000006, 0x00000002, 0x00050048,
    0x00000006, 0x00000000, 0x00000023, 0x00000000, 0x00050048, 0x00000006, 0x00000001, 0x00000023,
    0x00000008, 0x00030047, 0x00000007, 0x00000002, 0x00050048, 0x00000007, 0x00000000, 0x00000023,
    0x00000000, 0x00040047, 0x00000008, 0x00000006, 0x00000008, 0x00030047, 0x00000009, 0x00000002,
    0x00050048, 0x00000009, 0x00000000, 0x00000023, 0x00000000, 0x00020013, 0x0000000a, 0x00030021,
    0x0000000b, 0x0000000a, 0x00030016, 0x0000000c, 0x00000020, 0x00040017, 0x0000000d, 0x0000000c,
    0x00000004, 0x00040015, 0x0000000e, 0x00000020, 0x00000000, 0x00040015, 0x0000000f, 0x00000040,
    0x00000000, 0x00020014, 0x00000010, 0x00030027, 0x00000011, 0x000014e5, 0x0003001e, 0x00000009,
    0x0000000d, 0x00040020, 0x00000012, 0x000014e5, 0x00000009, 0x0003001d, 0x00000008, 0x00000012,
    0x0003001e, 0x00000007, 0x00000008, 0x00040020, 0x00000011, 0x000014e5, 0x00000007, 0x00040020,
    0x00000013, 0x000014e5, 0x00000012, 0x00040020, 0x00000014, 0x000014e5, 0x0000000d, 0x0004001e,
    0x00000006, 0x00000011, 0x0000000e, 0x00040020, 0x00000015, 0x00000009, 0x00000006, 0x00040020,
    0x00000016, 0x00000009, 0x00000011, 0x00040020, 0x00000017, 0x00000009, 0x0000000e, 0x00040020,
    0x00000018, 0x00000001, 0x0000000d, 0x00040020, 0x00000019, 0x00000001, 0x0000000e, 0x00040020,
    0x0000001a, 0x00000003, 0x0000000d, 0x0004002b, 0x0000000e, 0x0000001b, 0x00000000, 0x0004002b,
    0x0000000e, 0x0000001c, 0x00000001, 0x0004003b, 0x00000015, 0x00000005, 0x00000009, 0x0004003b,
    0x00000018, 0x00000002, 0x00000001, 0x0004003b, 0x00000019, 0x00000004, 0x00000001, 0x0004003b,
    0x0000001a, 0x00000003, 0x00000003, 0x00050036, 0x0000000a, 0x00000001, 0x00000000, 0x0000000b,
    0x000200f8, 0x0000001d, 0x00050041, 0x00000016, 0x0000001e, 0x00000005, 0x0000001b, 0x0004003d,
    0x00000011, 0x0000001f, 0x0000001e, 0x00050041, 0x00000017, 0x00000020, 0x00000005, 0x0000001c,
    0x0004003d, 0x0000000e, 0x00000021, 0x00000020, 0x000500ab, 0x00000010, 0x00000022, 0x00000021,
    0x0000001b, 0x0004003d, 0x0000000e, 0x00000023, 0x00000004, 0x00060041, 0x00000013, 0x00000024,
    0x0000001f, 0x0000001b, 0x00000023, 0x0006003d, 0x00000012, 0x00000025, 0x00000024, 0x00000002,
    0x00000008, 0x00050041, 0x00000014, 0x00000026, 0x00000025, 0x0000001b, 0x0006003d, 0x0000000d,
    0x00000027, 0x00000026, 0x00000002, 0x00000010, 0x0004003d, 0x0000000d, 0x00000028, 0x00000002,
    0x000600a9, 0x0000000d, 0x00000029, 0x00000022, 0x00000028, 0x00000027, 0x0003003e, 0x00000003,
    0x00000029, 0x000100fd, 0x00010038,
};
static const uint32_t k_arrayoob[] = {
    0x07230203, 0x00010500, 0x00070000, 0x0000001e, 0x00000000, 0x00020011, 0x00000001, 0x0003000e,
    0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000001, 0x6e69616d, 0x00000000, 0x00000002,
    0x00000003, 0x00000004, 0x00040047, 0x00000002, 0x0000001e, 0x00000000, 0x00040047, 0x00000003,
    0x0000000b, 0x00000000, 0x00040047, 0x00000004, 0x0000000b, 0x0000002a, 0x00020013, 0x00000005,
    0x00030021, 0x00000006, 0x00000005, 0x00030016, 0x00000007, 0x00000020, 0x00040017, 0x00000008,
    0x00000007, 0x00000004, 0x00040015, 0x00000009, 0x00000020, 0x00000000, 0x0004002b, 0x00000009,
    0x0000000a, 0x00000004, 0x0004002b, 0x00000009, 0x0000000b, 0x00000000, 0x0004002b, 0x00000009,
    0x0000000c, 0x00000009, 0x0004001c, 0x0000000d, 0x00000008, 0x0000000a, 0x00040020, 0x0000000e,
    0x00000007, 0x0000000d, 0x00040020, 0x0000000f, 0x00000007, 0x00000008, 0x00040020, 0x00000010,
    0x00000001, 0x00000008, 0x00040020, 0x00000011, 0x00000001, 0x00000009, 0x00040020, 0x00000012,
    0x00000003, 0x00000008, 0x0003002e, 0x00000008, 0x00000013, 0x0004003b, 0x00000010, 0x00000002,
    0x00000001, 0x0004003b, 0x00000011, 0x00000004, 0x00000001, 0x0004003b, 0x00000012, 0x00000003,
    0x00000003, 0x00050036, 0x00000005, 0x00000001, 0x00000000, 0x00000006, 0x000200f8, 0x00000014,
    0x0004003b, 0x0000000e, 0x00000015, 0x00000007, 0x00050041, 0x0000000f, 0x00000016, 0x00000015,
    0x0000000b, 0x0003003e, 0x00000016, 0x00000013, 0x0004003d, 0x00000009, 0x00000017, 0x00000004,
    0x00040070, 0x00000007, 0x00000018, 0x00000017, 0x00070050, 0x00000008, 0x00000019, 0x00000018,
    0x00000018, 0x00000018, 0x00000018, 0x00050041, 0x0000000f, 0x0000001a, 0x00000015, 0x0000000c,
    0x0003003e, 0x0000001a, 0x00000019, 0x0004003d, 0x00000008, 0x0000001b, 0x00000016, 0x0004003d,
    0x00000008, 0x0000001c, 0x00000002, 0x00050081, 0x00000008, 0x0000001d, 0x0000001c, 0x0000001b,
    0x0003003e, 0x00000003, 0x0000001d, 0x000100fd, 0x00010038,
};
static const uint32_t k_outptr[] = {
    0x07230203, 0x00010500, 0x00070000, 0x00000015, 0x00000000, 0x00020011, 0x00000001, 0x00020011,
    0x0000115a, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000001, 0x6e69616d,
    0x00000000, 0x00000002, 0x00000003, 0x00000004, 0x00040047, 0x00000002, 0x0000001e, 0x00000000,
    0x00040047, 0x00000003, 0x0000000b, 0x00000000, 0x00040047, 0x00000004, 0x0000000b, 0x0000002a,
    0x00020013, 0x00000005, 0x00030021, 0x00000006, 0x00000005, 0x00030016, 0x00000007, 0x00000020,
    0x00040017, 0x00000008, 0x00000007, 0x00000004, 0x00040015, 0x00000009, 0x00000020, 0x00000000,
    0x00040020, 0x0000000a, 0x00000001, 0x00000008, 0x00040020, 0x0000000b, 0x00000001, 0x00000009,
    0x00040020, 0x0000000c, 0x00000003, 0x00000008, 0x00040020, 0x0000000d, 0x00000007, 0x0000000c,
    0x0004003b, 0x0000000a, 0x00000002, 0x00000001, 0x0004003b, 0x0000000b, 0x00000004, 0x00000001,
    0x0004003b, 0x0000000c, 0x00000003, 0x00000003, 0x00050036, 0x00000005, 0x00000001, 0x00000000,
    0x00000006, 0x000200f8, 0x0000000e, 0x0004003b, 0x0000000d, 0x0000000f, 0x00000007, 0x0004003d,
    0x00000008, 0x00000010, 0x00000002, 0x0003003e, 0x00000003, 0x00000010, 0x0003003e, 0x0000000f,
    0x00000003, 0x0004003d, 0x0000000c, 0x00000011, 0x0000000f, 0x0004003d, 0x00000009, 0x00000012,
    0x00000004, 0x00040070, 0x00000007, 0x00000013, 0x00000012, 0x00070050, 0x00000008, 0x00000014,
    0x00000013, 0x00000013, 0x00000013, 0x00000013, 0x0003003e, 0x00000011, 0x00000014, 0x000100fd,
    0x00010038,
};
static const struct { const char *name, *what; const uint32_t *w; size_t n; int certify; } k_cert[] = {
    { "good", "position = M * a, M a push constant: certified", k_good, sizeof k_good, 1 },
    { "ptrload", "pos = flag ? a : *table[vertex_index] (a pointer loaded with a per-vertex index)", k_ptrload, sizeof k_ptrload, 0 },
    { "arrayoob", "float4 t[4]; t[9] = per-vertex; pos = a + t[0] (constant index past the array)", k_arrayoob, sizeof k_arrayoob, 0 },
    { "outptr", "pos = a, then a per-vertex store through a pointer kept in a Function variable", k_outptr, sizeof k_outptr, 0 },
};

static uint32_t *slurp(const char *path, size_t *nbytes)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *nbytes = (size_t)n; return b;
}

/* a quad (x0,y0)-(x1,y1) as two triangles (0,1,2) (0,2,3), corners in the order a nine-part mesh lists them */
static void quad(float *o, float x0, float y0, float x1, float y1)
{
    const float v[4][2] = { { x0, y0 }, { x1, y0 }, { x1, y1 }, { x0, y1 } };
    const int ix[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) { o[2 * k] = v[ix[k]][0]; o[2 * k + 1] = v[ix[k]][1]; }
}
static int expect(const char *name, const float *xy, uint32_t nt, unsigned want)
{
    uint16_t st[256]; uint32_t pairs = 0;
    const unsigned r = nvmtl_fo_partition(xy, nt, st, &pairs);
    printf("%-44s %3u triangles -> %3u run(s) (expected %u), %u pair tests: starts", name, nt, r, want, pairs);
    for (unsigned k = 0; k < r && k < 12; k++) printf(" %u", st[k]);
    printf("  %s\n", r == want ? "ok" : "MISMATCH");
    return r == want;
}


/* the area of the intersection of two triangles (convex clipping in double); and the least distance between them */
static double tri_area2(const double *P, int n)
{
    double a = 0; for (int k = 0; k < n; k++) { const double *p = P + 2 * k, *q = P + 2 * ((k + 1) % n); a += p[0] * q[1] - p[1] * q[0]; }
    return a;
}
static double overlap_area(const float *T, const float *U)
{
    double poly[32], out[32]; int n = 3;
    for (int k = 0; k < 6; k++) poly[k] = T[k];
    double u[6]; for (int k = 0; k < 6; k++) u[k] = U[k];
    const double o = tri_area2(u, 3) >= 0 ? 1 : -1;   /* clip by each edge of U, inside = left for a CCW U */
    for (int e = 0; e < 3 && n; e++) {
        const double *a = u + 2 * e, *b = u + 2 * ((e + 1) % 3); int m = 0;
        for (int k = 0; k < n; k++) {
            const double *p = poly + 2 * k, *q = poly + 2 * ((k + 1) % n);
            const double sp = o * ((b[0] - a[0]) * (p[1] - a[1]) - (b[1] - a[1]) * (p[0] - a[0]));
            const double sq = o * ((b[0] - a[0]) * (q[1] - a[1]) - (b[1] - a[1]) * (q[0] - a[0]));
            if (sp >= 0) { out[2 * m] = p[0]; out[2 * m + 1] = p[1]; m++; }
            if ((sp >= 0) != (sq >= 0)) { const double t = sp / (sp - sq); out[2 * m] = p[0] + t * (q[0] - p[0]); out[2 * m + 1] = p[1] + t * (q[1] - p[1]); m++; }
            if (m > 14) return 1e9;
        }
        n = m; memcpy(poly, out, sizeof(double) * 2 * m);
    }
    return n >= 3 ? fabs(tri_area2(poly, n)) / 2 : 0;
}
static double seg_pt(const double *a, const double *b, const double *p)
{
    const double dx = b[0] - a[0], dy = b[1] - a[1], l2 = dx * dx + dy * dy;
    double t = l2 > 0 ? ((p[0] - a[0]) * dx + (p[1] - a[1]) * dy) / l2 : 0; t = t < 0 ? 0 : t > 1 ? 1 : t;
    const double x = a[0] + t * dx - p[0], y = a[1] + t * dy - p[1]; return sqrt(x * x + y * y);
}
static double tri_dist(const float *T, const float *U)   /* boundary distance (0 when they meet); interiors overlap -> 0 */
{
    double t[6], u[6]; for (int k = 0; k < 6; k++) { t[k] = T[k]; u[k] = U[k]; }
    double d = 1e30;
    for (int e = 0; e < 3; e++) for (int k = 0; k < 3; k++) {
        d = fmin(d, seg_pt(t + 2 * e, t + 2 * ((e + 1) % 3), u + 2 * k));
        d = fmin(d, seg_pt(u + 2 * e, u + 2 * ((e + 1) % 3), t + 2 * k));
    }
    return d;
}
static unsigned long long rng = 88172645463325252ull;
static unsigned rnd(unsigned n) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (unsigned)(rng % n); }

int main(int argc, char **argv)
{
    int bad = 0;
    for (int a = 1; a < argc; a++) {
        size_t n = 0; uint32_t *w = slurp(argv[a], &n);
        if (!w) { printf("%s: cannot read\n", argv[a]); return 2; }
        const char *base = strrchr(argv[a], '/'); base = base ? base + 1 : argv[a];
        const int wr = nvmtl_fo_spirv_writes(w, n, 1) ? 2 : nvmtl_fo_spirv_writes(w, n, 0);   /* 2 buffers, 1 images only */
        if (strstr(base, ".vert.")) {
            uint32_t loc = 0, comps = 0; const char *why = NULL;
            const int ok = nvmtl_fo_certify(w, n, &loc, &comps, &why);
            if (ok) printf("%-28s writes %d  CERTIFIED: position = M * attribute(location %u, %u components) + b\n", base, wr, loc, comps);
            else printf("%-28s writes %d  not certified: %s\n", base, wr, why ? why : "?");
        } else printf("%-28s writes %d\n", base, wr);
        free(w);
    }
    /* certifier cases (small modules made to fool it: each must get the expected answer, none may read or write out of
       bounds - build with -fsanitize=address to check that too) */
    for (size_t k = 0; k < sizeof k_cert / sizeof k_cert[0]; k++) {
        uint32_t loc = 0, comps = 0; const char *why = NULL;
        const int ok = nvmtl_fo_certify(k_cert[k].w, k_cert[k].n, &loc, &comps, &why);
        printf("certify %-9s %s: %s%s%s\n", k_cert[k].name, k_cert[k].what, ok ? "certified" : "not certified (", ok ? "" : why ? why : "?", ok ? "" : ")");
        if (ok != k_cert[k].certify) { printf("  MISMATCH: expected %s\n", k_cert[k].certify ? "certified" : "not certified"); bad++; }
    }
    /* malformed: every truncation of the cases, and single-word corruptions, must not crash */
    for (size_t k = 0; k < sizeof k_cert / sizeof k_cert[0]; k++) {
        uint32_t buf[256]; const size_t nw = k_cert[k].n / 4; if (nw > 256) continue;
        for (size_t cut = 5; cut < nw; cut++) { uint32_t l, c; const char *y; memcpy(buf, k_cert[k].w, cut * 4); (void)nvmtl_fo_certify(buf, cut * 4, &l, &c, &y); }
        for (size_t at = 5; at < nw; at++) for (int v = 0; v < 4; v++) {
            uint32_t l, c; const char *y; memcpy(buf, k_cert[k].w, nw * 4);
            buf[at] = v == 0 ? 0xffffffffu : v == 1 ? buf[at] + 1 : v == 2 ? (buf[at] & 0xffffu) | 0x00010000u : 9;
            (void)nvmtl_fo_certify(buf, nw * 4, &l, &c, &y); (void)nvmtl_fo_spirv_writes(buf, nw * 4, 0);
        }
    }
    printf("certify: malformed variants ran\n");
    /* geometry */
    static float xy[256 * 6];
    /* nine-part mesh: 3x3 quads, borders 2 units, centre 96 (18 triangles): one run */
    { const float xs[4] = { 0, 2, 98, 100 }, ys[4] = { 0, 2, 98, 100 }; int t = 0;
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { quad(xy + 12 * t, xs[c], ys[r], xs[c + 1], ys[r + 1]); t++; }
      bad += !expect("nine-part 3x3 (18 tris)", xy, 18, 1); }
    /* the same in pixel units with a 1-unit border on a wide layer */
    { const float xs[4] = { 100, 101, 1999, 2000 }, ys[4] = { 50, 51, 649, 650 }; int t = 0;
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) { quad(xy + 12 * t, xs[c], ys[r], xs[c + 1], ys[r + 1]); t++; }
      bad += !expect("nine-part thin border (18 tris)", xy, 18, 1); }
    /* a 4x7 grid of quads (56 triangles) */
    { int t = 0; for (int r = 0; r < 4; r++) for (int c = 0; c < 7; c++) { quad(xy + 12 * t, c * 10.f, r * 10.f, c * 10.f + 10, r * 10.f + 10); t++; }
      bad += !expect("grid 7x4 (56 tris)", xy, 56, 1); }
    /* two overlapping quads: runs at 0 and 2 */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 5, 5, 15, 15);
    bad += !expect("two overlapping quads", xy, 4, 2);
    /* two separate quads with a gap */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 12, 0, 22, 10);
    bad += !expect("two quads with a 2-unit gap", xy, 4, 1);
    /* two quads touching along a full edge with distinct vertex objects but identical positions: shared edge */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 10, 0, 20, 10);
    bad += !expect("two quads sharing an edge", xy, 4, 1);
    /* T-junction: a quad, then two half-height quads to its right (their middle vertex on the first quad's edge) */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 10, 0, 20, 5); quad(xy + 24, 10, 5, 20, 10);
    bad += !expect("T-junction (touching, no shared edge)", xy, 6, 2);
    /* diagonal neighbours only (sharing one vertex) */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 10, 10, 20, 20);
    bad += !expect("diagonal quads sharing a corner", xy, 4, 1);
    /* a shared edge with both third vertices on one side (folded) */
    { const float f[12] = { 0, 0, 10, 0, 5, 8,   0, 0, 10, 0, 5, 4 }; memcpy(xy, f, sizeof f);
      bad += !expect("shared edge, third vertices on one side", xy, 2, 2); }
    /* a sliver gap far below the margin */
    quad(xy, 0, 0, 10, 10); quad(xy + 12, 10.0001f, 0, 20, 10);
    bad += !expect("quads 1e-4 apart (below the margin)", xy, 4, 2);
    /* degenerate triangles (two equal vertices) cover nothing */
    { const float f[18] = { 0, 0, 10, 0, 10, 10,   5, 5, 5, 5, 50, 50,   0, 0, 10, 10, 0, 10 }; memcpy(xy, f, sizeof f);
      bad += !expect("a degenerate triangle in a quad", xy, 3, 1); }
    /* text-like: overlapping glyph quads in a row (each overlaps the next) */
    { for (int k = 0; k < 8; k++) quad(xy + 12 * k, k * 8.f, 0, k * 8.f + 10, 12);
      bad += !expect("overlapping glyph quads", xy, 16, 8); }
    /* cross-shaped opposite-quadrant contact through one vertex (both with edges on the same lines, opposite rays) */
    { const float f[12] = { 1, 1, 1, 0, 999, 1,   1, 1, 0, 1, 1, 999 }; memcpy(xy, f, sizeof f);
      bad += !expect("opposite quadrants at one vertex", xy, 2, 1); }
    /* a fan of 256 triangles round one vertex: pairwise disjoint, but every pair shares the centre and all boxes meet;
       the pair-test budget bounds the cost (the rest of the fan gets a run per triangle) */
    { for (int k = 0; k < 256; k++) { const float a0 = 6.2831853f * k / 256, a1 = 6.2831853f * (k + 1) / 256;
          const float t[6] = { 0, 0, 100 * cosf(a0), 100 * sinf(a0), 100 * cosf(a1), 100 * sinf(a1) }; memcpy(xy + 6 * k, t, sizeof t); }
      uint16_t st[256]; uint32_t pairs = 0; const unsigned r = nvmtl_fo_partition(xy, 256, st, &pairs);
      printf("%-44s 256 triangles -> %3u run(s), %u pair tests (budget %d)  %s\n", "fan round one vertex", r, pairs, FO_PAIR_BUDGET,
             r >= 1 && pairs <= FO_PAIR_BUDGET + 1 ? "ok" : "MISMATCH");
      bad += !(r >= 1 && pairs <= FO_PAIR_BUDGET + 1); }
    /* not finite */
    quad(xy, 0, 0, 10, 10); xy[3] = NAN;
    bad += !expect("NaN position (0 = draw as before)", xy, 2, 0);
    /* random pairs on a small grid (shared vertices and edges are common there): a "disjoint" answer must be right -
       no common area, and no contact other than through a shared vertex or edge */
    { unsigned long long n = 0, dis = 0, wrong = 0;
      for (int it = 0; it < 400000; it++) {
          float T[6], U[6]; const unsigned g = 2 + rnd(6);
          for (int k = 0; k < 6; k++) { T[k] = (float)rnd(g); U[k] = (float)rnd(g); }
          if (rnd(2)) { const int a = rnd(3), b = rnd(3); U[2 * b] = T[2 * a]; U[2 * b + 1] = T[2 * a + 1]; }
          if (rnd(3) == 0) { const int a = rnd(3), b = rnd(3), c = (a + 1) % 3, d = (b + 1 + rnd(2)) % 3;
              U[2 * b] = T[2 * a]; U[2 * b + 1] = T[2 * a + 1]; U[2 * d] = T[2 * c]; U[2 * d + 1] = T[2 * c + 1]; }
          const double at = fabs(tri_area2((double[]){ T[0], T[1], T[2], T[3], T[4], T[5] }, 3)), au = fabs(tri_area2((double[]){ U[0], U[1], U[2], U[3], U[4], U[5] }, 3));
          if (at == 0 || au == 0) continue;
          n++;
          if (!nvmtl_fo_disjoint(T, U, 0.01f)) continue;
          dis++;
          int shared = 0; for (int a = 0; a < 3; a++) for (int b = 0; b < 3; b++) shared += T[2 * a] == U[2 * b] && T[2 * a + 1] == U[2 * b + 1];
          const double ar = overlap_area(T, U), dd = tri_dist(T, U);
          if (ar > 1e-9 || (!shared && dd < 0.01)) {
              if (wrong++ < 5) printf("random: WRONG disjoint (area %g, distance %g, shared %d): (%g,%g)(%g,%g)(%g,%g) | (%g,%g)(%g,%g)(%g,%g)\n",
                                      ar, dd, shared, T[0], T[1], T[2], T[3], T[4], T[5], U[0], U[1], U[2], U[3], U[4], U[5]);
          }
      }
      printf("random: %llu pairs, %llu answered disjoint, %llu of those wrong\n", n, dis, wrong);
      bad += wrong != 0; }
    printf("%s\n", bad ? "geometry: MISMATCH" : "geometry: all as expected");
    return bad ? 1 : 0;
}
