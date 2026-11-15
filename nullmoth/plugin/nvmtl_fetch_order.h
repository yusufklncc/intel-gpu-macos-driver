/* SPDX-License-Identifier: MIT */
/* nvmtl_fetch_order.h: which triangles of a framebuffer-fetch draw can be drawn together (NVMTL_FETCH_OVERLAP).
   Pure C, no Vulkan or Objective-C: nvmtl_vk.c includes it, and so does the host check (tools/fetchorder-check.c).

   Metal orders [[color(n)]] reads per pixel between the primitives of one draw. The B580 (Xe2) has no coherent framebuffer
   fetch, so nvmtl_vk.c draws a small fetch draw one triangle at a time with a barrier in between (nvmtl_fetch_split).
   That order only matters where two triangles of the draw cover the same pixel. This file answers, for a draw, the runs
   of consecutive triangles in which no triangle overlaps an earlier one of the same run; each run can then be drawn as
   one vkCmdDraw(Indexed) and the barriers go between runs only. All triangles disjoint = the whole draw in one run.

   1. nvmtl_fo_certify(): pipeline creation, on the vertex module given to Vulkan. It certifies that the value written to
      the Position built-in is an affine function of ONE per-vertex attribute with draw-uniform coefficients:
          position = M * a + b,   M and b the same for every vertex of the draw, a = the attribute (float/half 2-4).
      A value-state analysis over flattened scalar slots: every slot of every SSA value (and of every Private/Function
      variable) is BOT (not known yet), U (draw-uniform), A(k) (affine in the attribute at Location k with uniform
      coefficients) or X (anything else). U: constants, spec constants, OpUndef excluded; loads through a uniform pointer
      from Uniform, UniformConstant, PushConstant, StorageBuffer or PhysicalStorageBuffer memory (the module and the
      pipeline's other stage write no memory: nvmtl_fo_spirv_writes, so that memory is constant during the draw); loads
      of Private/Function variables whose every store is uniform. A(k): loads of the attribute variable. Linear ops keep
      A (FAdd/FSub/FNegate, FMul/VectorTimesScalar/MatrixTimesVector/VectorTimesMatrix/Dot with one side U, FDiv by U,
      GLSL Fma/FMix with uniform weights, composite construct/extract/insert, shuffle, copy, Select and Phi); any other op
      on an A operand is X (a conversion to half rounds: X). Iterated to a fixpoint (states only rise).
      Refused: any function call (more than one function), a branch or switch on a non-U value (then Private stores and
      Phis could differ per vertex), the position written in more than one place or partly, subgroup ops, derivatives,
      any opcode not on the list below, memory writes, ViewportIndex / ViewportMask outputs, MultiView.
   2. nvmtl_fo_partition(): draw time, on the attribute's x, y as the CPU sees them (nvmtl_vk.c reads them and requires
      every other component the same, bit for bit, across the draw's vertices; see the planarity note there).

   ROUNDING. The GPU does not draw the exact image of the attribute triangles: the vertex transform rounds and the
   rasterizer snaps vertices to its sub-pixel grid (1/256 px on Intel). Exact contacts are safe only where both triangles
   go through the SAME computation: a shared edge (two bitwise-identical vertices) is the same snapped edge in both, and
   the fill rule gives each sample to one side; a shared vertex is the same snapped point. Everything else needs a gap:
   - no shared vertex: separated along an edge normal (or the axes) by more than m;
   - shared edge: the two third vertices strictly on opposite sides, each farther than m from the edge's line;
   - one shared vertex V: a line through V along one of the four edges at V with each triangle on its own side, every
     vertex off the line farther than m from it (more where the other triangle has an edge along the line: see
     nvmtl_fo_disjoint), vertices exactly on the line only on opposite rays from V;
   - T-junctions and other touching contacts count as overlap.
   m = max(extent / 16384, max|coordinate| / 2^18) in attribute units, where extent is the draw's larger bounding-box side.
   The draw's screen size is not known here (the transform is in GPU memory), so m is relative: it covers sub-pixel
   snapping (1/256 px) when the draw spans at least 64 px on screen in its smaller direction, and float noise in the
   coordinates (~2^-23 of their size) always. On a smaller draw (or a layer squashed to a few pixels) the margin can be
   below the snapping step: two triangles that come closer than 1/16384 of the draw's extent without touching exactly
   (a deliberate exact contact is fine) could then share a sample and be ordered wrongly. Triangles whose
   vertices coincide in pairs (zero area for every transform) cover nothing and are skipped; collinear distinct vertices
   are NOT skipped (rounding could give them area). Any doubt: overlap, i.e. a new run, i.e. today's behaviour there. */
#ifndef NVMTL_FETCH_ORDER_H
#define NVMTL_FETCH_ORDER_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

/* ======================================================================================================================
   SPIR-V: does the module write memory (buffers, texel buffers, images) */
static int nvmtl_fo_ptr_sc_writable(uint32_t sc)
{
    /* Function 7, Private 6, Output 3, Workgroup 4, Input 1 (invalid to store): not memory another draw reads */
    return !(sc == 7 || sc == 6 || sc == 3 || sc == 4 || sc == 1);
}
/* 1 = the module stores to, or performs an atomic on, memory outside its own invocation / workgroup (or cannot be read).
   buffers_only: image stores and image atomics do not count (a texture over a buffer marks the buffer when it is made) */
static int nvmtl_fo_spirv_writes(const void *bytes, size_t nbytes, int buffers_only)
{
    const uint32_t *w = (const uint32_t *)bytes; const size_t nw = nbytes / 4;
    if (!w || nw < 5 || w[0] != 0x07230203u) return 1;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 22)) return 1;
    uint32_t *ptrsc = (uint32_t *)calloc(bound, 4), *ty = (uint32_t *)calloc(bound, 4);
    if (!ptrsc || !ty) { free(ptrsc); free(ty); return 1; }
    int r = 0;
    /* pass 1: pointer types (storage class + 1) and the result type of every id defined with [1] = type, [2] = id */
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (!wc || at + wc > nw) { r = 1; goto out; }
        if (op == 32 && wc >= 4 && i[1] < bound) ptrsc[i[1]] = i[2] + 1;
        else if (op == 39 && wc >= 3 && i[1] < bound) ptrsc[i[1]] = i[2] + 1;   /* OpTypeForwardPointer */
        else if ((op == 59 || op == 65 || op == 66 || op == 67 || op == 70 || op == 120 || op == 124 || op == 83 || op == 55 ||
                  op == 245 || op == 169 || op == 61 || op == 60 || op == 57 || op == 121 || op == 122 || op == 123) && wc >= 3 && i[2] < bound)
            ty[i[2]] = i[1];
        at += wc;
    }
    for (size_t at = 5; at < nw && !r;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        uint32_t ptr = 0;
        if ((op == 62 || op == 63 || op == 64) && wc >= 3) ptr = i[1];                  /* Store, CopyMemory(Sized): target */
        else if (op == 228 || op == 319) ptr = wc >= 2 ? i[1] : 0;                      /* AtomicStore, AtomicFlagClear */
        else if ((op >= 227 && op <= 242) || op == 318 || op == 5614 || op == 5615 || op == 6035) ptr = wc >= 4 ? i[3] : 0;
        else if (op == 99 && !buffers_only) { r = 1; break; }                            /* ImageWrite */
        else if (op == 4458 || op == 5360 || op == 4460) { r = 1; break; }               /* cooperative matrix stores */
        if (op == 227) { at += wc; continue; }                                           /* AtomicLoad: a read */
        if (ptr) {
            const uint32_t t = ptr < bound ? ty[ptr] : 0, sc1 = t && t < bound ? ptrsc[t] : 0;
            if (!sc1 || (nvmtl_fo_ptr_sc_writable(sc1 - 1) && !(buffers_only && sc1 - 1 == 11))) r = 1;   /* 11: Image (texel pointers) */
        }
        at += wc;
    }
out:
    free(ptrsc); free(ty);
    return r;
}

/* ======================================================================================================================
   SPIR-V: the certifier */
enum { FO_BOT = 0, FO_U = 1, FO_A0 = 2, FO_X = 255 };   /* FO_A0 + k: affine in the attribute at Location k (k < 32) */
#define FO_MAXSLOT 64
static inline uint8_t fo_join(uint8_t a, uint8_t b)
{
    if (a == FO_BOT) return b; if (b == FO_BOT) return a; if (a == b) return a;
    if (a == FO_U) return b; if (b == FO_U) return a; return FO_X;
}
static inline uint8_t fo_add(uint8_t a, uint8_t b) { return (a == FO_BOT || b == FO_BOT) ? FO_BOT : fo_join(a, b); }
static inline uint8_t fo_mul(uint8_t a, uint8_t b)
{
    if (a == FO_BOT || b == FO_BOT) return FO_BOT; if (a == FO_U) return b; if (b == FO_U) return a; return FO_X;
}
static inline uint8_t fo_gen(uint8_t a, uint8_t b)
{
    if (a == FO_BOT || b == FO_BOT) return FO_BOT; return (a == FO_U && b == FO_U) ? FO_U : FO_X;
}

typedef struct {
    const uint32_t *w; size_t nw; uint32_t bound;
    uint32_t *def;       /* word offset of the defining instruction (types, constants, variables, values) */
    uint32_t *ty;        /* result type of a value id */
    uint8_t *tsl;        /* slots of a type, 0 = not computed; 255 = more than FO_MAXSLOT (one summary slot) */
    uint32_t *voff; uint8_t *vn;   /* a value's (or a Private/Function variable's memory) slots in pool */
    uint8_t *pool; uint32_t npool, cpool;
    /* pointers: root variable (0 = memory outside the module's variables), path (constant indices) or dynamic */
    uint32_t *proot; uint8_t *pdepth, *pdyn, *paddr; uint16_t (*ppath)[4];
    uint8_t *isattr;     /* Input variable: 1 + Location (float32 scalar/vector, Location, not built-in) */
    uint8_t *isbuiltin;  /* variable or struct type (member) carries a BuiltIn */
    uint32_t *loc;       /* Location + 1 */
    uint32_t posvar, posmember;   /* the Position variable, and its member index when it is a gl_PerVertex block (~0 = direct) */
    uint32_t glsl;       /* the GLSL.std.450 import */
    int changed;
} fo_ctx;

static uint32_t fo_tslots_raw(fo_ctx *x, uint32_t t, int depth);
static uint32_t fo_tslots(fo_ctx *x, uint32_t t)   /* 1..64, or 255 = big (one summary slot) */
{
    if (!t || t >= x->bound) return 255;
    if (!x->tsl[t]) { uint32_t n = fo_tslots_raw(x, t, 0); x->tsl[t] = (uint8_t)(n == 0 || n > FO_MAXSLOT ? 255 : n); }
    return x->tsl[t];
}
static uint32_t fo_nslots(fo_ctx *x, uint32_t t) { const uint32_t n = fo_tslots(x, t); return n == 255 ? 1 : n; }
static uint32_t fo_const_u32(fo_ctx *x, uint32_t id, int *ok)
{
    *ok = 0;
    if (!id || id >= x->bound || !x->def[id]) return 0;
    const uint32_t *i = x->w + x->def[id];
    if ((i[0] & 0xffffu) != 43 || (i[0] >> 16) < 4) return 0;   /* OpConstant (not a spec constant) */
    *ok = 1; return i[3];
}
static uint32_t fo_tslots_raw(fo_ctx *x, uint32_t t, int depth)
{
    if (depth > 16 || !t || t >= x->bound || !x->def[t]) return 0;
    const uint32_t *i = x->w + x->def[t], op = i[0] & 0xffffu, wc = i[0] >> 16;
    switch (op) {
    case 20: case 21: case 22: return 1;                                  /* bool, int, float */
    case 23: return wc >= 4 ? i[3] : 0;                                   /* vector */
    case 24: { const uint32_t c = fo_tslots_raw(x, i[2], depth + 1); return c && c <= FO_MAXSLOT ? c * i[3] : 0; }
    case 28: { int ok; const uint32_t n = fo_const_u32(x, i[3], &ok); const uint32_t e = fo_tslots_raw(x, i[2], depth + 1);
               return ok && e && e <= FO_MAXSLOT && n <= FO_MAXSLOT ? e * n : 0; }
    case 30: { uint32_t s = 0; for (uint32_t k = 2; k < wc; k++) { const uint32_t m = fo_tslots_raw(x, i[k], depth + 1);
               if (!m || m > FO_MAXSLOT) return 0; s += m; if (s > FO_MAXSLOT) return 0; } return s; }
    default: return 1;                                                     /* pointer, image, sampler, ...: opaque */
    }
}
/* the type and slot offset of index k inside type t (composite access); 0 = not a composite this follows */
static int fo_member(fo_ctx *x, uint32_t t, uint32_t k, uint32_t *sub, uint32_t *off)
{
    if (!t || t >= x->bound || !x->def[t]) return 0;
    const uint32_t *i = x->w + x->def[t], op = i[0] & 0xffffu, wc = i[0] >> 16;
    switch (op) {
    case 23: if (k >= i[3]) return 0; *sub = i[2]; *off = k; return 1;
    case 24: if (k >= i[3]) return 0; *sub = i[2]; *off = k * fo_nslots(x, i[2]); return 1;
    case 28: *sub = i[2]; *off = k * fo_nslots(x, i[2]); return 1;
    case 29: *sub = i[2]; *off = 0; return 1;   /* runtime array: only through memory, never a value */
    case 30: { if (k + 2 >= wc) return 0; uint32_t o = 0; for (uint32_t m = 2; m < k + 2; m++) o += fo_nslots(x, i[m]);
               *sub = i[k + 2]; *off = o; return 1; }
    default: return 0;
    }
}
static int fo_alloc(fo_ctx *x, uint32_t id, uint32_t n)
{
    if (!id || id >= x->bound || x->vn[id]) return 1;
    if (x->npool + n > x->cpool) {
        uint32_t c = x->cpool ? x->cpool * 2 : 4096; while (c < x->npool + n) c *= 2;
        uint8_t *p = (uint8_t *)realloc(x->pool, c); if (!p) return 0;
        memset(p + x->cpool, 0, c - x->cpool); x->pool = p; x->cpool = c;
    }
    x->voff[id] = x->npool; x->vn[id] = (uint8_t)n; x->npool += n;
    return 1;
}
static inline uint8_t *fo_slots(fo_ctx *x, uint32_t id) { return id && id < x->bound && x->vn[id] ? x->pool + x->voff[id] : NULL; }
static inline uint8_t fo_summary(fo_ctx *x, uint32_t id)
{
    const uint8_t *s = fo_slots(x, id); if (!s) return FO_X;
    uint8_t r = FO_BOT; for (uint32_t k = 0; k < x->vn[id]; k++) r = fo_join(r, s[k]);
    /* BOT anywhere: not known yet */
    for (uint32_t k = 0; k < x->vn[id]; k++) if (s[k] == FO_BOT) return FO_BOT;
    return r;
}
static inline void fo_set(fo_ctx *x, uint8_t *dst, uint8_t v)
{
    const uint8_t n = fo_join(*dst, v);
    if (n != *dst) { *dst = n; x->changed = 1; }
}
/* the state of a value operand: constants and spec constants U, OpUndef X */
static inline int fo_isvalue(fo_ctx *x, uint32_t id) { return id && id < x->bound && x->vn[id]; }

/* opcodes whose words are [1] result type, [2] result id: the ones this file reads (anything else in the entry function is
   refused by fo_step, so its result is never needed) */
static int fo_has_result(uint32_t op)
{
    return op == 1 || (op >= 41 && op <= 46) || (op >= 48 && op <= 52) || op == 54 || op == 55 || op == 57 || op == 59 ||
           op == 60 || op == 61 || (op >= 65 && op <= 70) || (op >= 77 && op <= 84) || (op >= 86 && op <= 107) ||
           (op >= 109 && op <= 124) || (op >= 126 && op <= 152) || (op >= 154 && op <= 205) || op == 12 || op == 169 ||
           op == 245 || op == 400;
}
/* result slot r of an operation */
static void fo_def_all(fo_ctx *x, uint32_t id, uint8_t v)
{
    uint8_t *d = fo_slots(x, id); if (!d) return;
    for (uint32_t k = 0; k < x->vn[id]; k++) fo_set(x, d + k, v);
}

/* the module's description of a pointer after an access chain: walks the base pointer's pointee type */
static uint32_t fo_pointee(fo_ctx *x, uint32_t ptrtype)
{
    if (!ptrtype || ptrtype >= x->bound || !x->def[ptrtype]) return 0;
    const uint32_t *i = x->w + x->def[ptrtype];
    return (i[0] & 0xffffu) == 32 ? i[3] : 0;
}
static uint32_t fo_ptr_sc(fo_ctx *x, uint32_t ptrtype)
{
    if (!ptrtype || ptrtype >= x->bound || !x->def[ptrtype]) return ~0u;
    const uint32_t *i = x->w + x->def[ptrtype];
    return (i[0] & 0xffffu) == 32 ? i[2] : ~0u;
}
/* slots [*off, *off + *len) of root variable memory that a pointer with this path names; 0 = the whole (dynamic) */
static int fo_path_range(fo_ctx *x, uint32_t root, uint32_t p, uint32_t *off, uint32_t *len)
{
    uint32_t t = fo_pointee(x, x->ty[root]), o = 0;
    if (x->pdyn[p] || fo_tslots(x, t) == 255) return 0;
    for (uint32_t d = 0; d < x->pdepth[p]; d++) {
        uint32_t sub, so; if (!fo_member(x, t, x->ppath[p][d], &sub, &so)) return 0;
        o += so; t = sub;
    }
    *off = o; *len = fo_nslots(x, t);
    return 1;
}

static const char *fo_step(fo_ctx *x, size_t fbeg, size_t fend, int final, uint32_t *posval, uint32_t *posoff, int *nposstore)
{
    const uint32_t *w = x->w;
    for (size_t at = fbeg; at < fend;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        at += wc;
        switch (op) {
        case 248: case 249: case 247: case 246: case 253: case 255: case 8: case 317: case 256: case 257: case 0: case 59: case 54:
            continue;                                                  /* labels, branches, merges, return, lines, variables */
        case 250:                                                      /* BranchConditional: the condition must be uniform */
            if (final && fo_summary(x, i[1]) != FO_U) return "a branch on a per-vertex value";
            continue;
        case 251:
            if (final && fo_summary(x, i[1]) != FO_U) return "a switch on a per-vertex value";
            continue;
        case 252: case 4416: case 5380: return "kill / demote in a vertex shader";
        case 254: return "a value return from the entry point";
        case 57: return "a function call";
        default: break;
        }
        if (wc < 3) return "an instruction this check does not know";
        const uint32_t rt = i[1], id = i[2];
        uint8_t *d = fo_slots(x, id);
        switch (op) {
        case 61: {                                                     /* OpLoad */
            const uint32_t p = i[3]; if (!p || p >= x->bound || !x->proot) return "a load from an unknown pointer";
            const uint32_t sc = fo_ptr_sc(x, x->ty[p]), root = x->proot[p];
            if (!d) continue;   /* a pointer loaded from memory: its own analysis is in proot / paddr (set below) */
            if (sc == 1) {                                                /* Input */
                if (root && x->isattr[root] && !x->isbuiltin[root]) {
                    const uint8_t a = (uint8_t)(FO_A0 + x->isattr[root] - 1);
                    const uint8_t v = x->pdyn[p] ? (x->paddr[p] == FO_U ? a : x->paddr[p] == FO_BOT ? FO_BOT : FO_X) : a;
                    fo_def_all(x, id, v);
                } else fo_def_all(x, id, FO_X);
            } else if (sc == 6 || sc == 7) {                              /* Private, Function */
                uint8_t *m = root ? fo_slots(x, root) : NULL;
                if (!m) { fo_def_all(x, id, FO_X); continue; }
                uint32_t o, n;
                if (fo_path_range(x, root, p, &o, &n) && n == x->vn[id]) { for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, m[o + k]); }
                else { const uint8_t s = x->pdyn[p] && x->paddr[p] != FO_U ? (x->paddr[p] == FO_BOT ? FO_BOT : FO_X) : fo_summary(x, root);
                       fo_def_all(x, id, s); }
            } else if (sc == 2 || sc == 0 || sc == 9 || sc == 12 || sc == 5349) {
                fo_def_all(x, id, x->paddr[p]);                          /* uniform address: uniform value */
            } else fo_def_all(x, id, FO_X);
            continue; }
        case 62: {                                                     /* OpStore: [1] pointer [2] value (no result) */
            const uint32_t p = i[1], v = i[2];
            if (!p || p >= x->bound) return "a store through an unknown pointer";
            const uint32_t sc = fo_ptr_sc(x, x->ty[p]), root = x->proot[p];
            if (sc == 3) {                                                /* Output */
                if (root && root == x->posvar) {
                    if (x->posmember == ~0u) { if (x->pdepth[p] || x->pdyn[p]) return "a partial write of the position"; *posval = v; *posoff = 0; }
                    else if (!x->pdepth[p] && !x->pdyn[p]) { uint32_t sub, so;   /* the whole gl_PerVertex block */
                        if (!fo_member(x, fo_pointee(x, x->ty[root]), x->posmember, &sub, &so)) return "the position block is not understood";
                        *posval = v; *posoff = so; }
                    else if (x->pdepth[p] == 1 && !x->pdyn[p] && x->ppath[p][0] == x->posmember) { *posval = v; *posoff = 0; }
                    else if (x->pdepth[p] >= 1 && !x->pdyn[p] && x->ppath[p][0] != x->posmember) continue;   /* another built-in */
                    else return "a partial write of the position";
                    if (final) (*nposstore)++;
                }
                continue;
            }
            if (sc == 6 || sc == 7) {
                uint8_t *m = root ? fo_slots(x, root) : NULL; const uint8_t *s = fo_slots(x, v);
                if (!m) return "a store to a variable this check does not follow";
                uint32_t o, n;
                if (s && fo_path_range(x, root, p, &o, &n) && n == x->vn[v]) { for (uint32_t k = 0; k < n; k++) fo_set(x, m + o + k, s[k]); }
                else { uint8_t sv = s ? fo_summary(x, v) : FO_X;
                       if (x->pdyn[p] && x->paddr[p] != FO_U) sv = x->paddr[p] == FO_BOT ? FO_BOT : FO_X;
                       if (sv != FO_BOT) for (uint32_t k = 0; k < x->vn[root]; k++) fo_set(x, m + k, sv); }
                continue;
            }
            return "a store to memory";
        }
        case 65: case 66: case 67: case 70: {                          /* access chains */
            const uint32_t b = i[3]; if (!b || b >= x->bound) return "an access chain on an unknown base";
            x->proot[id] = x->proot[b]; x->pdepth[id] = x->pdepth[b]; x->pdyn[id] = x->pdyn[b];
            memcpy(x->ppath[id], x->ppath[b], sizeof x->ppath[id]);
            uint8_t a = x->paddr[b];
            const uint32_t first = (op == 67 || op == 70) ? 5 : 4;   /* PtrAccessChain: [4] is the element (no path step) */
            if (op == 67 || op == 70) { a = fo_gen(a, fo_summary(x, i[4])); if (x->proot[id]) x->pdyn[id] = 1; }
            for (uint32_t k = first; k < wc; k++) {
                int ok; const uint32_t c = fo_const_u32(x, i[k], &ok);
                a = fo_gen(a, fo_isvalue(x, i[k]) ? fo_summary(x, i[k]) : FO_U);
                if (ok && c < 0xffffu && x->pdepth[id] < 4 && !x->pdyn[id]) x->ppath[id][x->pdepth[id]++] = (uint16_t)c;
                else x->pdyn[id] = 1;
            }
            if (x->paddr[id] != a) { const uint8_t n2 = fo_join(x->paddr[id], a); if (n2 != x->paddr[id]) { x->paddr[id] = n2; x->changed = 1; } }
            continue; }
        case 120: case 124: case 83: case 400:                         /* ConvertUToPtr, Bitcast, CopyObject, CopyLogical */
            if (!d) {   /* a pointer result: from an integer (ConvertUToPtr, Bitcast of an integer) or a copy of a pointer */
                const uint32_t s = i[3];
                if (s >= x->bound) return "a pointer from an unknown operand";
                uint8_t a;
                if (op == 120 || fo_slots(x, s)) { x->proot[id] = 0; x->pdyn[id] = 1; x->pdepth[id] = 0; a = fo_gen(FO_U, fo_summary(x, s)); }
                else { x->proot[id] = x->proot[s]; x->pdepth[id] = x->pdepth[s]; x->pdyn[id] = x->pdyn[s];
                       memcpy(x->ppath[id], x->ppath[s], sizeof x->ppath[id]); a = x->paddr[s]; }
                const uint8_t n2 = fo_join(x->paddr[id], a); if (n2 != x->paddr[id]) { x->paddr[id] = n2; x->changed = 1; }
                continue;
            }
            if (op == 124) { fo_def_all(x, id, fo_gen(FO_U, fo_summary(x, i[3]))); continue; }   /* a bit cast rounds nothing, but is not linear */
            { const uint8_t *s = fo_slots(x, i[3]);
              if (!s || x->vn[i[3]] != x->vn[id]) { fo_def_all(x, id, fo_isvalue(x, i[3]) ? fo_summary(x, i[3]) : FO_X); continue; }
              for (uint32_t k = 0; k < x->vn[id]; k++) fo_set(x, d + k, s[k]); }
            continue;
        default: break;
        }
        if (!d) {
            /* a result that is not a value this check tracks (a pointer from an op not listed, or a big composite op) */
            if (x->proot && id < x->bound && fo_ptr_sc(x, rt) != ~0u) return "a pointer from an operation this check does not follow";
            if (op == 68) continue;
            return "a result this check does not follow";
        }
        const uint32_t n = x->vn[id];
        switch (op) {
        case 126: case 127: {                                          /* SNegate, FNegate: linear */
            const uint8_t *a = fo_slots(x, i[3]); if (!a || x->vn[i[3]] != n) { fo_def_all(x, id, FO_X); break; }
            for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, a[k]);
            break; }
        case 128: case 129: case 130: case 131: case 132: case 133: case 136: {   /* add, sub, mul, fdiv */
            const uint8_t *a = fo_slots(x, i[3]), *b = fo_slots(x, i[4]);
            if (!a || !b || x->vn[i[3]] != n || x->vn[i[4]] != n) { fo_def_all(x, id, FO_X); break; }
            for (uint32_t k = 0; k < n; k++) {
                uint8_t v;
                if (op <= 131) v = fo_add(a[k], b[k]);
                else if (op == 136) v = (b[k] == FO_BOT || a[k] == FO_BOT) ? FO_BOT : b[k] == FO_U ? a[k] : FO_X;
                else v = fo_mul(a[k], b[k]);
                fo_set(x, d + k, v);
            }
            break; }
        case 142: case 143: {                                          /* VectorTimesScalar, MatrixTimesScalar */
            const uint8_t *a = fo_slots(x, i[3]), *b = fo_slots(x, i[4]);
            if (!a || !b || x->vn[i[3]] != n || x->vn[i[4]] != 1) { fo_def_all(x, id, FO_X); break; }
            for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, fo_mul(a[k], b[0]));
            break; }
        case 145: case 144: {                                          /* MatrixTimesVector, VectorTimesMatrix */
            const uint32_t mi = op == 145 ? i[3] : i[4], vi = op == 145 ? i[4] : i[3];
            const uint8_t *m = fo_slots(x, mi), *v = fo_slots(x, vi);
            if (!m || !v || !x->def[x->ty[mi]]) { fo_def_all(x, id, FO_X); break; }
            const uint32_t *mt = w + x->def[x->ty[mi]];
            if ((mt[0] & 0xffffu) != 24) { fo_def_all(x, id, FO_X); break; }
            const uint32_t cols = mt[3], rows = fo_nslots(x, mt[2]);
            if (cols * rows != x->vn[mi] || (op == 145 ? (x->vn[vi] != cols || n != rows) : (x->vn[vi] != rows || n != cols))) { fo_def_all(x, id, FO_X); break; }
            for (uint32_t r = 0; r < n; r++) {
                uint8_t s = FO_BOT; int first = 1;
                const uint32_t terms = op == 145 ? cols : rows;
                for (uint32_t c = 0; c < terms; c++) {
                    const uint8_t t = op == 145 ? fo_mul(m[c * rows + r], v[c]) : fo_mul(v[c], m[r * rows + c]);
                    s = first ? t : fo_add(s, t); first = 0;
                }
                fo_set(x, d + r, s);
            }
            break; }
        case 148: {                                                    /* Dot */
            const uint8_t *a = fo_slots(x, i[3]), *b = fo_slots(x, i[4]);
            if (!a || !b || x->vn[i[3]] != x->vn[i[4]] || n != 1) { fo_def_all(x, id, FO_X); break; }
            uint8_t s = FO_BOT; for (uint32_t k = 0; k < x->vn[i[3]]; k++) { const uint8_t t = fo_mul(a[k], b[k]); s = k ? fo_add(s, t) : t; }
            fo_set(x, d, s);
            break; }
        case 169: {                                                    /* Select: a uniform condition picks one operand */
            const uint8_t *a = fo_slots(x, i[4]), *b = fo_slots(x, i[5]); const uint8_t c = fo_summary(x, i[3]);
            if (!a || !b || x->vn[i[4]] != n || x->vn[i[5]] != n) { fo_def_all(x, id, FO_X); break; }
            for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, c == FO_BOT ? FO_BOT : c == FO_U ? fo_join(a[k], b[k]) : FO_X);
            break; }
        case 245: {                                                    /* Phi (control flow is checked uniform) */
            for (uint32_t k = 3; k + 1 < wc; k += 2) {
                const uint8_t *a = fo_slots(x, i[k]);
                if (!a || x->vn[i[k]] != n) { fo_def_all(x, id, FO_X); break; }
                for (uint32_t s = 0; s < n; s++) fo_set(x, d + s, a[s]);
            }
            break; }
        case 80: {                                                     /* CompositeConstruct: operand slots in order */
            uint32_t o = 0; int bad = 0;
            for (uint32_t k = 3; k < wc && !bad; k++) {
                const uint8_t *a = fo_slots(x, i[k]); if (!a) { bad = 1; break; }
                if (o + x->vn[i[k]] > n) { bad = 1; break; }
                for (uint32_t s = 0; s < x->vn[i[k]]; s++) fo_set(x, d + o + s, a[s]);
                o += x->vn[i[k]];
            }
            if (bad || o != n) fo_def_all(x, id, FO_X);
            break; }
        case 81: case 82: {                                            /* CompositeExtract / CompositeInsert */
            const uint32_t comp = op == 81 ? i[3] : i[4], obj = op == 82 ? i[3] : 0, first = op == 81 ? 4 : 5;
            const uint8_t *s = fo_slots(x, comp); if (!s) { fo_def_all(x, id, FO_X); break; }
            uint32_t t = x->ty[comp], o = 0; int big = fo_tslots(x, t) == 255;
            for (uint32_t k = first; k < wc && !big; k++) { uint32_t sub, so; if (!fo_member(x, t, i[k], &sub, &so)) { big = 1; break; } o += so; t = sub; }
            if (op == 81) {
                if (big || o + n > x->vn[comp]) { fo_def_all(x, id, fo_summary(x, comp)); break; }
                for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, s[o + k]);
            } else {
                const uint8_t *b = fo_slots(x, obj);
                if (big || !b || x->vn[comp] != n || o + x->vn[obj] > n) { fo_def_all(x, id, fo_join(fo_summary(x, comp), b ? fo_summary(x, obj) : FO_X)); break; }
                for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, (k >= o && k < o + x->vn[obj]) ? b[k - o] : s[k]);
            }
            break; }
        case 79: {                                                     /* VectorShuffle */
            const uint8_t *a = fo_slots(x, i[3]), *b = fo_slots(x, i[4]);
            if (!a || !b || wc - 5 != n) { fo_def_all(x, id, FO_X); break; }
            const uint32_t na = x->vn[i[3]];
            for (uint32_t k = 0; k < n; k++) {
                const uint32_t c = i[5 + k];
                fo_set(x, d + k, c == 0xffffffffu ? FO_X : c < na ? a[c] : c - na < x->vn[i[4]] ? b[c - na] : FO_X);
            }
            break; }
        case 12: {                                                     /* ExtInst */
            if (i[3] != x->glsl) return "an extended instruction set other than GLSL.std.450";
            const uint32_t e = i[4];
            if (e == 50 && wc == 8) {                                  /* Fma(a, b, c) = a*b + c */
                const uint8_t *a = fo_slots(x, i[5]), *b = fo_slots(x, i[6]), *c = fo_slots(x, i[7]);
                if (!a || !b || !c || x->vn[i[5]] != n || x->vn[i[6]] != n || x->vn[i[7]] != n) { fo_def_all(x, id, FO_X); break; }
                for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, fo_add(fo_mul(a[k], b[k]), c[k]));
                break;
            }
            if (e == 46 && wc == 8) {                                  /* FMix(x, y, a) = x*(1-a) + y*a */
                const uint8_t *a = fo_slots(x, i[5]), *b = fo_slots(x, i[6]), *c = fo_slots(x, i[7]);
                if (!a || !b || !c || x->vn[i[5]] != n || x->vn[i[6]] != n || x->vn[i[7]] != n) { fo_def_all(x, id, FO_X); break; }
                for (uint32_t k = 0; k < n; k++) fo_set(x, d + k, fo_add(fo_mul(a[k], c[k]), fo_mul(b[k], c[k])));
                break;
            }
            uint8_t s = FO_U; for (uint32_t k = 5; k < wc; k++) s = fo_gen(s, fo_isvalue(x, i[k]) ? fo_summary(x, i[k]) : FO_X);
            fo_def_all(x, id, s);
            break; }
        default: {
            /* pure operations whose result is uniform when every operand is: conversions, arithmetic, logic, comparisons,
               bit ops, image sampling and queries. On any A operand: X. */
            const int pure = (op >= 109 && op <= 119) || (op >= 126 && op <= 152) || (op >= 154 && op <= 205 && op != 192 && op != 193) ||
                             (op >= 86 && op <= 107 && op != 99) || op == 77 || op == 78 || op == 84 || op == 68;
            if (!pure) return "an instruction this check does not know";
            uint8_t s = FO_U;
            for (uint32_t k = 3; k < wc; k++) {
                if (fo_isvalue(x, i[k])) s = fo_gen(s, fo_summary(x, i[k]));
                else if (i[k] < x->bound && x->proot && x->def[i[k]] && fo_ptr_sc(x, x->ty[i[k]]) != ~0u) s = fo_gen(s, x->paddr[i[k]]);
                /* literals (image operands masks, extract indices) and ids of types are not values */
            }
            fo_def_all(x, id, s);
            break; }
        }
    }
    return NULL;
}

/* 1 = certified: *loc the attribute's Location, *comps its component count (2-4, 32-bit float) */
static int nvmtl_fo_certify(const void *bytes, size_t nbytes, uint32_t *loc_out, uint32_t *comps_out, const char **why)
{
    const uint32_t *w = (const uint32_t *)bytes; const size_t nw = nbytes / 4;
    *why = "not a SPIR-V module";
    if (!w || nw < 5 || w[0] != 0x07230203u) return 0;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 20)) { *why = "id bound too large"; return 0; }
    if (nvmtl_fo_spirv_writes(bytes, nbytes, 0)) { *why = "the vertex module writes memory"; return 0; }
    fo_ctx X; fo_ctx *x = &X; memset(x, 0, sizeof X);
    x->w = w; x->nw = nw; x->bound = bound; x->posmember = ~0u;
    x->def = (uint32_t *)calloc(bound, 4); x->ty = (uint32_t *)calloc(bound, 4); x->tsl = (uint8_t *)calloc(bound, 1);
    x->voff = (uint32_t *)calloc(bound, 4); x->vn = (uint8_t *)calloc(bound, 1);
    x->proot = (uint32_t *)calloc(bound, 4); x->pdepth = (uint8_t *)calloc(bound, 1); x->pdyn = (uint8_t *)calloc(bound, 1);
    x->paddr = (uint8_t *)calloc(bound, 1); x->ppath = (uint16_t (*)[4])calloc(bound, sizeof *x->ppath);
    x->isattr = (uint8_t *)calloc(bound, 1); x->isbuiltin = (uint8_t *)calloc(bound, 1); x->loc = (uint32_t *)calloc(bound, 4);
    int ok = 0; size_t fbeg = 0, fend = 0; uint32_t entry = 0, nentry = 0, nfunc = 0;
    if (!x->def || !x->ty || !x->tsl || !x->voff || !x->vn || !x->proot || !x->pdepth || !x->pdyn || !x->paddr || !x->ppath ||
        !x->isattr || !x->isbuiltin || !x->loc) { *why = "out of memory"; goto out; }
    /* pass 1: definitions, decorations, entry point, the function body */
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (!wc || at + wc > nw) { *why = "a malformed instruction"; goto out; }
        switch (op) {
        case 17: if (wc >= 2 && (i[1] == 4439 || i[1] == 5254 || i[1] == 5255 || i[1] == 5260 || i[1] == 69 || i[1] == 70)) { *why = "multiview / per-view or viewport-array capability"; goto out; } break;
        case 11: if (wc >= 3 && !strncmp((const char *)(i + 2), "GLSL.std.450", (wc - 2) * 4)) x->glsl = i[1]; break;
        case 15: nentry++; if (wc >= 3 && i[1] == 0) entry = i[2]; break;
        case 73: case 74: case 75: *why = "decoration groups"; goto out;
        case 71:
            if (wc >= 4 && i[1] < bound) {
                if (i[2] == 11) { x->isbuiltin[i[1]] = 1; if (i[3] == 0) x->posvar = i[1];
                                  if (i[3] == 10 || i[3] == 5262 || i[3] == 9 || i[3] == 4440) { *why = "a viewport / layer / view built-in"; goto out; } }
                if (i[2] == 30) x->loc[i[1]] = i[3] + 1;
                if (i[2] == 31 && i[3]) x->loc[i[1]] |= 0x80000000u;   /* Component: not the plain attribute */
            }
            break;
        case 72:
            if (wc >= 5 && i[1] < bound && i[3] == 11) {
                x->isbuiltin[i[1]] = 1;
                if (i[4] == 10 || i[4] == 5262 || i[4] == 9 || i[4] == 4440) { *why = "a viewport / layer / view built-in"; goto out; }
                if (i[4] == 0) { x->posmember = i[2]; x->posvar = ~0u - i[1]; /* resolved below: the variable of this block type */ }
            }
            break;
        case 54: nfunc++; if (wc >= 3 && i[2] == entry) fbeg = at; break;
        case 56: if (fbeg && !fend && at > fbeg) fend = at; break;
        default: break;
        }
        /* definitions: types and imports by [1]; values (and variables, functions) by [1] = type, [2] = id */
        if ((op >= 19 && op <= 39) && wc >= 2 && i[1] < bound) x->def[i[1]] = (uint32_t)at;
        else if (op == 11 && wc >= 2 && i[1] < bound) x->def[i[1]] = (uint32_t)at;
        else if (fo_has_result(op) && wc >= 3 && i[2] < bound) { x->def[i[2]] = (uint32_t)at; x->ty[i[2]] = i[1]; }
        at += wc;
    }
    if (nentry != 1 || !entry) { *why = nentry > 1 ? "more than one entry point" : "no vertex entry point"; goto out; }
    if (nfunc != 1) { *why = "a function besides the entry point (calls)"; goto out; }
    if (!fbeg || !fend) { *why = "no entry function body"; goto out; }
    if (!x->glsl) x->glsl = ~0u;
    /* the Position variable: decorated directly, or the Output variable of a block type with a Position member */
    if (x->posvar >= bound) {
        const uint32_t bt = ~0u - x->posvar; x->posvar = 0;
        for (uint32_t id = 1; id < bound && bt < bound; id++) {
            if (!x->def[id]) continue;
            const uint32_t *i = w + x->def[id];
            if ((i[0] & 0xffffu) == 59 && fo_ptr_sc(x, i[1]) == 3 && fo_pointee(x, i[1]) == bt) { if (x->posvar) { *why = "two position blocks"; goto out; } x->posvar = id; }
        }
    } else x->posmember = ~0u;
    if (!x->posvar) { *why = "no position output"; goto out; }
    /* pass 2: value slots for constants, undefs, variables' memory and the function's results; attribute variables */
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        at += wc;
        if (op == 59 && wc >= 4) {                                    /* OpVariable */
            const uint32_t id = i[2], sc = i[3], pt = fo_pointee(x, i[1]);
            if (id >= bound) continue;
            x->proot[id] = id; x->paddr[id] = FO_U;
            if (sc == 1 && x->loc[id] && !(x->loc[id] & 0x80000000u) && !x->isbuiltin[id] && (x->loc[id] - 1) < 32) {
                /* a float32 scalar or 2-4 vector */
                const uint32_t *t = pt && x->def[pt] ? w + x->def[pt] : NULL; uint32_t ft = 0, comps = 0;
                if (t && (t[0] & 0xffffu) == 22) { ft = pt; comps = 1; }
                else if (t && (t[0] & 0xffffu) == 23) { ft = t[2]; comps = t[3]; }
                const uint32_t *f = ft && x->def[ft] ? w + x->def[ft] : NULL;
                if (f && (f[0] & 0xffffu) == 22 && f[2] == 32 && comps >= 1 && comps <= 4) x->isattr[id] = (uint8_t)x->loc[id];
            }
            if (sc == 6 || sc == 7) {
                if (!fo_alloc(x, id, fo_nslots(x, pt))) { *why = "out of memory"; goto out; }
                if (wc >= 5) { /* initializer: a constant */ uint8_t *m = fo_slots(x, id); for (uint32_t k = 0; k < x->vn[id]; k++) m[k] = FO_U; }
            }
            continue;
        }
        if ((op >= 41 && op <= 46) || (op >= 48 && op <= 52)) {        /* constants and spec constants */
            if (wc >= 3 && i[2] < bound && fo_alloc(x, i[2], fo_nslots(x, i[1]))) { uint8_t *m = fo_slots(x, i[2]); for (uint32_t k = 0; k < x->vn[i[2]]; k++) m[k] = FO_U; }
            continue;
        }
        if (op == 1 && wc >= 3 && i[2] < bound) {                      /* OpUndef: any value, per invocation */
            if (fo_alloc(x, i[2], fo_nslots(x, i[1]))) { uint8_t *m = fo_slots(x, i[2]); for (uint32_t k = 0; k < x->vn[i[2]]; k++) m[k] = FO_X; }
            continue;
        }
    }
    for (size_t at = fbeg; at < fend;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        at += wc;
        if (op == 54 || op == 248 || op == 59 || wc < 3) continue;
        const uint32_t rt = i[1], id = i[2];
        if (id >= bound || x->def[id] != (uint32_t)(i - w)) continue;
        if (fo_ptr_sc(x, rt) != ~0u) continue;                        /* a pointer: proot / paddr */
        if (!x->def[rt]) continue;
        const uint32_t top = w[x->def[rt]] & 0xffffu;
        if (top < 19 || top > 39 || top == 19) continue;              /* not a type, or void */
        if (!fo_alloc(x, id, fo_nslots(x, rt))) { *why = "out of memory"; goto out; }
    }
    /* fixpoint */
    uint32_t posval = 0, posoff = 0; int nps = 0, pass;
    const char *e = NULL;
    for (pass = 0; pass < 64; pass++) {
        x->changed = 0;
        if ((e = fo_step(x, fbeg, fend, 0, &posval, &posoff, &nps))) { *why = e; goto out; }
        if (!x->changed) break;
    }
    if (pass == 64) { *why = "no fixpoint"; goto out; }
    nps = 0; posval = 0;
    if ((e = fo_step(x, fbeg, fend, 1, &posval, &posoff, &nps))) { *why = e; goto out; }
    if (nps != 1) { *why = nps ? "the position is written in more than one place" : "the position is not written"; goto out; }
    const uint8_t *ps = fo_slots(x, posval);
    if (!ps || posoff + 4 > x->vn[posval]) { *why = "the position value is not followed"; goto out; }
    uint8_t a = FO_BOT;
    for (int k = 0; k < 4; k++) {
        const uint8_t s = ps[posoff + k];
        if (s == FO_BOT || s == FO_X) { *why = s == FO_X ? "the position is not an affine function of one attribute" : "the position is not known"; goto out; }
        if (s != FO_U) { if (a != FO_BOT && a != s) { *why = "the position reads two attributes"; goto out; } a = s; }
    }
    if (a == FO_BOT) { *why = "the position reads no attribute"; goto out; }
    {
        const uint32_t L = (uint32_t)(a - FO_A0);
        uint32_t comps = 0;
        for (uint32_t id = 1; id < bound; id++) if (x->isattr[id] && x->isattr[id] - 1u == L) {
            const uint32_t pt = fo_pointee(x, x->ty[id]); const uint32_t *t = w + x->def[pt];
            comps = (t[0] & 0xffffu) == 23 ? t[3] : 1;
        }
        if (comps < 2) { *why = "the attribute has fewer than 2 components"; goto out; }
        *loc_out = L; *comps_out = comps; *why = NULL; ok = 1;
    }
out:
    free(x->def); free(x->ty); free(x->tsl); free(x->voff); free(x->vn); free(x->pool);
    free(x->proot); free(x->pdepth); free(x->pdyn); free(x->paddr); free(x->ppath); free(x->isattr); free(x->isbuiltin); free(x->loc);
    return ok;
}

/* ======================================================================================================================
   Geometry: runs of a draw's triangles */
static inline int fo_same(const float *a, const float *b) { return !memcmp(a, b, 2 * sizeof(float)); }
static inline float fo_cross(float ux, float uy, float vx, float vy) { return ux * vy - uy * vx; }

/* T, U: 3 vertices (x, y) each. 1 = robustly disjoint interiors (see the header), 0 = may overlap. */
static int nvmtl_fo_disjoint(const float *T, const float *U, float m)
{
    int tsh[3] = { -1, -1, -1 }, nsh = 0;
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) if (fo_same(T + 2 * i, U + 2 * j)) { if (tsh[i] < 0) { tsh[i] = j; nsh++; } }
    if (nsh >= 3) return 0;
    if (nsh == 2) {                                               /* a shared edge P Q; R and S the third vertices */
        int ti = 0, uj = 0; while (tsh[ti] >= 0) ti++;
        int used[3] = { 0, 0, 0 }; for (int i = 0; i < 3; i++) if (tsh[i] >= 0) used[tsh[i]] = 1;
        while (used[uj]) uj++;
        const float *P = NULL, *Q = NULL;
        for (int i = 0; i < 3; i++) if (tsh[i] >= 0) { if (!P) P = T + 2 * i; else Q = T + 2 * i; }
        const float ux = Q[0] - P[0], uy = Q[1] - P[1], len = sqrtf(ux * ux + uy * uy);
        if (!(len > 0)) return 0;
        const float *R = T + 2 * ti, *S = U + 2 * uj;
        const float sr = fo_cross(ux, uy, R[0] - P[0], R[1] - P[1]), ss = fo_cross(ux, uy, S[0] - P[0], S[1] - P[1]);
        return (sr > m * len && ss < -m * len) || (sr < -m * len && ss > m * len);
    }
    if (nsh == 1) {                                               /* one shared vertex V: a separating line through V */
        /* Candidate lines: through V along each of the four edges at V. Vertices exactly on the line (s == 0) are allowed
           on one ray per triangle, opposite rays for the two. Every other vertex X must be strictly on its triangle's side
           with distance d_X > m from the line, and, when the other triangle has an edge on the line along X's ray (to E),
           d_X > m (1 + |X - V| / |E - V|): snapping turns the edge V-X by up to m / |X - V| and the edge V-E by up to
           m / |E - V|, and the angle between them is d_X / |X - V|. */
        int vi = 0; while (tsh[vi] < 0) vi++;
        const float *V = T + 2 * vi;
        const float *pts[4]; int np = 0;
        for (int i = 0; i < 3; i++) if (i != vi) pts[np++] = T + 2 * i;
        for (int j = 0; j < 3; j++) if (j != tsh[vi]) pts[np++] = U + 2 * j;
        for (int dd = 0; dd < 4; dd++) {
            const float ux = pts[dd][0] - V[0], uy = pts[dd][1] - V[1], len = sqrtf(ux * ux + uy * uy);
            if (!(len > 0)) continue;
            int side[4], ray[4]; float dist[4], dl[4]; int bad = 0;
            for (int k = 0; k < 4; k++) {
                const float vx = pts[k][0] - V[0], vy = pts[k][1] - V[1];
                const float sv = fo_cross(ux, uy, vx, vy), t = ux * vx + uy * vy;
                dl[k] = sqrtf(vx * vx + vy * vy); dist[k] = fabsf(sv) / len;
                side[k] = sv > 0 ? 1 : sv < 0 ? -1 : 0; ray[k] = t > 0 ? 1 : t < 0 ? -1 : 0;
                if (!(dl[k] > 0) || (!side[k] && !ray[k])) bad = 1;
            }
            if (bad) continue;
            /* T's two (k 0, 1) on the closed sg side or on the line, U's (k 2, 3) on the other side or on the line */
            for (int sg = -1; sg <= 1; sg += 2) {
                int okk = 1, rT = 0, rU = 0;
                for (int k = 0; k < 4 && okk; k++) {
                    const int want = k < 2 ? sg : -sg;
                    if (side[k] == -want) okk = 0;
                    if (!side[k]) { int *r = k < 2 ? &rT : &rU; if (*r && *r != ray[k]) okk = 0; *r = ray[k]; }
                }
                if (!okk || (rT && rU && rT == rU)) continue;
                if ((!side[0] && !side[1]) || (!side[2] && !side[3])) continue;   /* a triangle along the line: no area */
                for (int k = 0; k < 4 && okk; k++) {
                    if (!side[k]) continue;
                    if (!(dist[k] > m)) { okk = 0; break; }
                    for (int e = (k < 2 ? 2 : 0); e < (k < 2 ? 4 : 2); e++)   /* the other triangle's on-line vertices */
                        if (!side[e] && ray[e] == ray[k] && !(dist[k] > m * (1.0f + dl[k] / dl[e]))) okk = 0;
                }
                if (okk) return 1;
            }
        }
        return 0;
    }
    /* no shared vertex: an edge normal (or an axis) that separates them by more than m */
    const float *tri[2] = { T, U };
    for (int a = 0; a < 2; a++) for (int e = 0; e < 3; e++) {
        const float *P = tri[a] + 2 * e, *Q = tri[a] + 2 * ((e + 1) % 3);
        const float nx = -(Q[1] - P[1]), ny = Q[0] - P[0], len = sqrtf(nx * nx + ny * ny);
        if (!(len > 0)) continue;
        float t0 = INFINITY, t1 = -INFINITY, u0 = INFINITY, u1 = -INFINITY;
        for (int k = 0; k < 3; k++) {
            const float pt = T[2 * k] * nx + T[2 * k + 1] * ny, pu = U[2 * k] * nx + U[2 * k + 1] * ny;
            t0 = fminf(t0, pt); t1 = fmaxf(t1, pt); u0 = fminf(u0, pu); u1 = fmaxf(u1, pu);
        }
        if (t1 + m * len < u0 || u1 + m * len < t0) return 1;
    }
    return 0;
}
/* xy: 6 floats per triangle; nt <= 256. run_start[r] = first triangle of run r. Returns the number of runs (>= 1), 0 = not
   finite / out of range (the caller draws as before). *pairs: pair tests made (cost) */
static unsigned nvmtl_fo_partition(const float *xy, uint32_t nt, uint16_t *run_start, uint32_t *pairs)
{
    if (!nt || nt > 256) return 0;
    float lo[2] = { INFINITY, INFINITY }, hi[2] = { -INFINITY, -INFINITY }, amax = 0;
    for (uint32_t k = 0; k < nt * 6; k++) {
        const float v = xy[k];
        if (!isfinite(v) || fabsf(v) > 1e30f) return 0;
        lo[k & 1] = fminf(lo[k & 1], v); hi[k & 1] = fmaxf(hi[k & 1], v); amax = fmaxf(amax, fabsf(v));
    }
    const float ext = fmaxf(hi[0] - lo[0], hi[1] - lo[1]);
    const float m = fmaxf(ext * (1.0f / 16384.0f), amax * (1.0f / 262144.0f));
    float bb[256][4]; uint8_t cov[256];
    for (uint32_t t = 0; t < nt; t++) {
        const float *T = xy + 6 * t;
        bb[t][0] = fminf(fminf(T[0], T[2]), T[4]); bb[t][1] = fminf(fminf(T[1], T[3]), T[5]);
        bb[t][2] = fmaxf(fmaxf(T[0], T[2]), T[4]); bb[t][3] = fmaxf(fmaxf(T[1], T[3]), T[5]);
        cov[t] = !(fo_same(T, T + 2) || fo_same(T, T + 4) || fo_same(T + 2, T + 4));   /* two equal vertices: no area, ever */
    }
    unsigned runs = 1; uint32_t s = 0, np = 0; run_start[0] = 0;
    for (uint32_t j = 1; j < nt; j++) {
        if (!cov[j]) continue;
        for (uint32_t i = s; i < j; i++) {
            if (!cov[i]) continue;
            if (bb[i][2] + m < bb[j][0] || bb[j][2] + m < bb[i][0] || bb[i][3] + m < bb[j][1] || bb[j][3] + m < bb[i][1]) continue;
            np++;
            if (!nvmtl_fo_disjoint(xy + 6 * i, xy + 6 * j, m)) { run_start[runs++] = (uint16_t)j; s = j; break; }
        }
    }
    if (pairs) *pairs = np;
    return runs;
}
#endif
