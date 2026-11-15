// SPDX-License-Identifier: MIT
// fetchorder_check: the plugin's framebuffer-fetch ordering analysis (nullmoth/plugin/nvmtl_fetch_order.h), off the GPU.
//   - for every SPIR-V module given: does it write memory ("writes 2": buffer memory - a render pipeline with such a stage
//     turns the CPU-side check off for the process; "writes 1": only images), and for vertex modules (file name containing ".vert.") does the
//     certifier accept it (position = M * attribute + b with draw-uniform M, b), with the attribute's Location;
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
