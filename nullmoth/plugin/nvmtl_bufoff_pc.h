/* SPDX-License-Identifier: MIT */
/* nvmtl_bufoff_pc.h: a kernel's storage-buffer descriptors that do not move with setBuffer:offset: (the descriptor-buffer
   model, DESCRIPTOR-MODEL.md "Geekbench's buffers"). Pure C, no Vulkan or ObjC: the host test runs it on the translator's
   output (user/tests/metal/bufoff_check.c).

   The translator keeps a buffer that a kernel indexes plainly as a StorageBuffer descriptor (binding = its Metal index,
   set 0) instead of lowering it to an address from the table, and the plugin writes that descriptor at the buffer's
   address + the bound offset (rounded down to minStorageBufferOffsetAlignment; the translator adds the remainder from the
   table). So a new offset is a new descriptor, and in the descriptor-buffer model a new region per dispatch: Geekbench's
   chains set new offsets for their input and output before every dispatch (2026-11-13 night, D2b: 1.00 regions per bind
   in every buffer variant, 128 B = the two descriptors).

   This pass gives such a buffer a 32-bit push constant q (one per binding, appended to the kernel's push block after the
   table-pc qwords, at base + 4k): every access chain into the buffer's variable gets q * (G / S) added to its element
   index (S = the runtime array's stride, G = NVMTL_BUFOFF_GRAN). The plugin then writes the descriptor at the buffer's
   address + (offset' mod G) and pushes q = offset' / G (offset' = the offset rounded down as before), so the address every
   access reaches is the one it reached before: address + offset' + (index + remainder) * S. The descriptor changes only
   when the buffer, its size or offset' mod G changes, not with every new offset.

   A binding is converted only when every variable with it (StorageBuffer, DescriptorSet 0, Binding < nbuf) is a Block
   struct of one member, a runtime array at offset 0 with an ArrayStride that divides G, and is used only as the base of
   OpAccessChain / OpInBoundsAccessChain with at least two indices, the first the constant 0 and the second a 32- or
   64-bit integer. Anything else (OpArrayLength, a pointer to the array passed on, a stride that does not divide G, an
   index whose type is not known) leaves that binding as it was; a module with none left, with no push block or more than
   one entry point, is returned unchanged (NULL). The block must have room: base + 4n <= limit.

   2026-11-13 night (branch dbufgb): words the grammar fixes as literals (nvmtl_spv_literal.h) no longer count as uses,
   so `OpVariable .. StorageBuffer` (12) or a GLSL.std.450 instruction number no longer refuse a module whose push block
   or buffer variable has that id; meta->why and meta->have say what was left and why (NVMTL_BO_R_*). */
#ifndef NVMTL_BUFOFF_PC_H
#define NVMTL_BUFOFF_PC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_spv_literal.h"

#define NVMTL_BUFOFF_GRAN 256u
/* why a binding (or the module) was left as it was: meta->why, set whether or not the pass converted anything */
enum {
    NVMTL_BO_R_MODULE = 1u << 0,   /* not one GLCompute entry point, no push block, or a push block the pass cannot move */
    NVMTL_BO_R_NONE   = 1u << 1,   /* no StorageBuffer variable in set 0 at a buffer binding */
    NVMTL_BO_R_SHAPE  = 1u << 2,   /* not Block { T[] } at offset 0, or a stride that does not divide G */
    NVMTL_BO_R_LENGTH = 1u << 3,   /* OpArrayLength of the buffer */
    NVMTL_BO_R_CALL   = 1u << 4,   /* the variable passed to a function */
    NVMTL_BO_R_CHAIN  = 1u << 5,   /* an access chain whose first index is not the constant 0 or whose element index is not known */
    NVMTL_BO_R_OTHER  = 1u << 6,   /* any other use of the variable (meta->op: the first such opcode) */
    NVMTL_BO_R_ROOM   = 1u << 7,   /* no room in the push block */
};
typedef struct {
    uint32_t mask, count;          /* the converted bindings; member k = the k-th set bit of mask */
    uint32_t have, why, op;        /* candidate bindings seen, NVMTL_BO_R_* of what was refused, the first other opcode */
} nvmtl_bufoff;

typedef struct { uint32_t *w; size_t n, cap; int failed; } nvmtl_bo_words;
static void nvmtl_bo_put(nvmtl_bo_words *o, const uint32_t *w, size_t n)
{
    if (o->failed) return;
    if (n > o->cap - o->n) { o->failed = 1; return; }
    memcpy(o->w + o->n, w, n * sizeof *w); o->n += n;
}
#define NVMTL_BO_EMIT(o, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; nvmtl_bo_put((o), a_, sizeof a_ / sizeof *a_); } while (0)
/* opcodes whose word 1 is a result type and word 2 the result: the instructions an access chain's index may come from */
static int nvmtl_bo_typed(uint32_t op)
{
    switch (op) {
    case 1: case 12: case 43: case 50: case 51: case 52: case 55: case 61: case 81: case 82: case 83: case 113: case 114:
    case 124: case 169: case 245: return 1;
    default: return (op >= 126 && op <= 151) || (op >= 194 && op <= 200);   /* integer arithmetic, shifts, bitwise */
    }
}
/* the words of instruction i that may be ids a move of the push block could break: none for debug, annotations, the
   entry point, int/float types and control flow; the type of a constant (not its value); every word elsewhere (a literal
   that equals one of those ids can only refuse the module) */
static void nvmtl_bo_id_words(const uint32_t *i, uint32_t *from, uint32_t *to)
{
    const uint32_t op = i[0] & 0xffffu;
    *from = 1; *to = i[0] >> 16;
    switch (op) {
    case 5: case 6: case 71: case 72: case 15: case 16: case 17: case 1: case 2: case 3: case 4: case 10: case 11: case 14:
    case 21: case 22: case 246: case 247: case 248: case 249: case 250: case 253: case 254: *from = *to = 0; break;
    case 32: *from = 3; break;                           /* OpTypePointer: the pointee */
    case 43: case 50: *from = 1; *to = 2; break;         /* OpConstant / OpSpecConstant: the type */
    case 61: if (*to > 4) *to = 4; break;                /* OpLoad: type, result, pointer (not its memory operands) */
    case 62: if (*to > 3) *to = 3; break;                /* OpStore: pointer, object */
    case 81: if (*to > 4) *to = 4; break;                /* OpCompositeExtract: type, result, composite */
    default: break;
    }
}
/* the integer width (32, 64) of id's type, 0 when it is not a 32/64-bit integer or not known */
static uint32_t nvmtl_bo_int_width(const uint32_t *w, const uint32_t *def, uint32_t bound, uint32_t id, uint32_t *type)
{
    if (!id || id >= bound || !def[id]) return 0;
    const uint32_t *i = w + def[id];
    if (!nvmtl_bo_typed(i[0] & 0xffffu) || (i[0] >> 16) < 3 || i[2] != id || i[1] >= bound || !def[i[1]]) return 0;
    const uint32_t *t = w + def[i[1]];
    if ((t[0] & 0xffffu) != 21 || (t[0] >> 16) != 4 || (t[2] != 32 && t[2] != 64)) return 0;
    *type = i[1];
    return t[2];
}
static int nvmtl_bo_const0(const uint32_t *w, const uint32_t *def, uint32_t bound, uint32_t id)
{
    if (!id || id >= bound || !def[id]) return 0;
    const uint32_t *i = w + def[id], wc = i[0] >> 16;
    if ((i[0] & 0xffffu) != 43 || (wc != 4 && wc != 5) || i[3] || (wc == 5 && i[4])) return 0;
    if (i[1] >= bound || !def[i[1]]) return 0;
    return (w[def[i[1]]] & 0xffffu) == 21;
}

/* module (bytes, nbytes): a GLCompute module after the plugin's address lowering and cs-table-pc. base: where the new
   members start in the push block (48 + 8 * table-pc qwords); limit: maxPushConstantsSize; nbuf: buffer bindings
   (NVMTL_NBUF). Returns an owned module and *meta, or NULL (nothing converted; the caller keeps its module). */
static uint32_t *nvmtl_spirv_bufoff_pc(const void *bytes, size_t nbytes, uint32_t base, uint32_t limit, uint32_t nbuf,
                                       nvmtl_bufoff *meta, size_t *out_bytes)
{
    const uint32_t *w = bytes; const size_t nw = nbytes / 4;
    if (!w || nbytes % 4 || nw < 5 || w[0] != 0x07230203u || (base & 3u) || base >= limit || !nbuf || nbuf > 32) return NULL;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 20) || nw > (1u << 24)) return NULL;
    uint32_t *def = calloc(bound, sizeof *def), *set = calloc(bound, sizeof *set), *bind = calloc(bound, sizeof *bind);
    uint32_t *stride = calloc(bound, sizeof *stride), *mof = calloc(bound, sizeof *mof);
    uint8_t *block = calloc(bound, 1), *cand = calloc(bound, 1);
    uint32_t *result = NULL; nvmtl_bo_words out = {0};
    uint32_t entries = 0, function_pos = 0, type_pos = 0, push_var = 0, push_ptr = 0, push_block = 0, push_members = 0;
    uint32_t u32 = 0, u64 = 0, bad = 0, have = 0, chains = 0, why = NVMTL_BO_R_MODULE, other = 0;
    uint32_t vstride[64] = {0}, vbind[64] = {0}, vid[64] = {0}, nv = 0;   /* the candidate variables */
    if (!def || !set || !bind || !stride || !mof || !block || !cand) goto done;
    for (uint32_t k = 0; k < bound; k++) { set[k] = ~0u; bind[k] = ~0u; mof[k] = ~0u; }
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (!wc || at + wc > nw) goto done;
        if (op == 15) { if (wc < 4 || i[1] != 5 || ++entries != 1) goto done; }
        if (op == 54 && !function_pos) function_pos = (uint32_t)at;
        if (op >= 19 && op <= 39 && wc >= 2) {
            if (i[1] >= bound) goto done;
            if (!type_pos) type_pos = (uint32_t)at;
            def[i[1]] = (uint32_t)at;
            if (op == 21 && wc == 4 && !i[3]) { if (i[2] == 32 && !u32) u32 = i[1]; if (i[2] == 64 && !u64) u64 = i[1]; }
        } else if (wc >= 3 && nvmtl_bo_typed(op)) {
            if (i[2] >= bound) goto done;
            def[i[2]] = (uint32_t)at;
        } else if (op == 59 && wc >= 4) {
            if (i[2] >= bound) goto done;
            def[i[2]] = (uint32_t)at;
            if (i[3] == 9) { if (push_var) goto done; push_var = i[2]; push_ptr = i[1]; }
        }
        if (op == 71 && wc == 4 && i[1] < bound) {
            if (i[2] == 34) set[i[1]] = i[3];
            if (i[2] == 33) bind[i[1]] = i[3];
            if (i[2] == 6) stride[i[1]] = i[3];
        }
        if (op == 71 && wc == 3 && i[1] < bound && i[2] == 2) block[i[1]] = 1;
        if (op == 72 && wc == 5 && i[1] < bound && i[2] == 0 && i[3] == 35) mof[i[1]] = i[4];
        at += wc;
    }
    if (entries != 1 || !function_pos || !type_pos || !push_var || push_ptr >= bound || !def[push_ptr]) goto done;
    {   /* the push block: pointer -> Block struct whose members all end at or below base */
        const uint32_t *pp = w + def[push_ptr];
        if ((pp[0] & 0xffffu) != 32 || (pp[0] >> 16) != 4 || pp[2] != 9 || pp[3] >= bound || !def[pp[3]]) goto done;
        push_block = pp[3];
        const uint32_t *pb = w + def[push_block];
        if ((pb[0] & 0xffffu) != 30 || (pb[0] >> 16) < 3 || (pb[0] >> 16) - 2 > 64 || !block[push_block]) goto done;
        push_members = (pb[0] >> 16) - 2;
        uint64_t placed = 0;
        for (size_t at = 5; at < function_pos;) {
            const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
            if (op == 72 && wc == 5 && i[1] == push_block && i[3] == 35) {
                if (i[2] >= push_members) goto done;
                const uint32_t mt = pb[2 + i[2]];
                if (mt >= bound || !def[mt]) goto done;
                const uint32_t *t = w + def[mt]; uint32_t size = 0;
                if (((t[0] & 0xffffu) == 21 || (t[0] & 0xffffu) == 22) && (t[2] == 32 || t[2] == 64)) size = t[2] / 8;
                else if ((t[0] & 0xffffu) == 23 && (t[0] >> 16) == 4 && t[2] < bound && def[t[2]]) {
                    const uint32_t *c = w + def[t[2]];
                    if (((c[0] & 0xffffu) == 21 || (c[0] & 0xffffu) == 22) && (c[2] == 32 || c[2] == 64)) size = c[2] / 8 * t[3];
                }
                if (!size || i[4] + size > base) goto done;
                placed |= 1ull << i[2];
            }
            at += wc;
        }
        if (placed != (push_members == 64 ? ~0ull : (1ull << push_members) - 1)) goto done;
        if ((w + def[push_var])[0] >> 16 != 4) goto done;   /* no initializer */
        /* the block type is used only by its pointer type, the pointer type only by the variable, the variable by nothing
           in the global section: all three move to the end of it. Names and decorations may name them. */
        for (size_t at = 5; at < nw;) {
            const uint32_t *i = w + at, wc = i[0] >> 16;
            uint32_t from, to; nvmtl_bo_id_words(i, &from, &to);
            if (at != def[push_block] && at != def[push_ptr] && at != def[push_var])
                for (uint32_t k = from; k < to; k++) {
                    if (nvmtl_spv_word_is_literal(i, k)) continue;
                    if (i[k] == push_block || i[k] == push_ptr) goto done;
                    if (at < function_pos && i[k] == push_var) goto done;
                }
            at += wc;
        }
    }
    why = 0;
    /* the candidates: StorageBuffer variables in set 0 at a buffer binding, Block { T[] } with a stride dividing G */
    for (size_t at = 5; at < function_pos;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (op == 59 && wc == 4 && i[3] == 12 && i[2] < bound) {
            const uint32_t v = i[2], b = bind[v];
            if (set[v] != 0 || b >= nbuf) { at += wc; continue; }
            uint32_t s = 0;
            if (i[1] < bound && def[i[1]]) {
                const uint32_t *pt = w + def[i[1]];
                if ((pt[0] & 0xffffu) == 32 && (pt[0] >> 16) == 4 && pt[2] == 12 && pt[3] < bound && def[pt[3]]) {
                    const uint32_t st = pt[3], *sty = w + def[st];
                    if ((sty[0] & 0xffffu) == 30 && (sty[0] >> 16) == 3 && block[st] && mof[st] == 0 && sty[2] < bound && def[sty[2]]) {
                        const uint32_t *arr = w + def[sty[2]];
                        if ((arr[0] & 0xffffu) == 29 && (arr[0] >> 16) == 3) s = stride[sty[2]];
                    }
                }
            }
            if (!s || s > NVMTL_BUFOFF_GRAN || (s & (s - 1)) || nv == 64) { bad |= 1u << b; why |= NVMTL_BO_R_SHAPE; }
            else { vid[nv] = v; vbind[nv] = b; vstride[nv] = s; nv++; cand[v] = 1; have |= 1u << b; }
        }
        at += wc;
    }
    /* every use of a candidate: the base of an access chain with >= 2 indices, the first constant 0, the second a known
       32/64-bit integer. A global-section use other than names, decorations and the entry point refuses it too. */
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (at < function_pos && op == 59) { at += wc; continue; }   /* the variables themselves */
        uint32_t from, to; nvmtl_bo_id_words(i, &from, &to);
        for (uint32_t k = from; k < to; k++) {
            if (i[k] >= bound || !cand[i[k]] || nvmtl_spv_word_is_literal(i, k)) continue;
            uint32_t ty = 0;
            const int ok = at >= function_pos && (op == 65 || op == 66) && k == 3 && wc >= 6 && nvmtl_bo_const0(w, def, bound, i[4])
                           && nvmtl_bo_int_width(w, def, bound, i[5], &ty);
            if (ok) { chains++; continue; }
            if (op == 68) why |= NVMTL_BO_R_LENGTH;
            else if (op == 57) why |= NVMTL_BO_R_CALL;
            else if ((op == 65 || op == 66) && k == 3) why |= NVMTL_BO_R_CHAIN;
            else { why |= NVMTL_BO_R_OTHER; if (!other) other = op; }
            for (uint32_t m = 0; m < nv; m++) if (vid[m] == i[k]) bad |= 1u << vbind[m];
        }
        at += wc;
    }
    if (!have && !bad) why |= NVMTL_BO_R_NONE;
    uint32_t mask = have & ~bad;
    uint32_t n = (uint32_t)__builtin_popcount(mask);
    while (n && base + 4 * n > limit) { mask &= ~(1u << (31 - __builtin_clz(mask))); n--; why |= NVMTL_BO_R_ROOM; }   /* no room: the highest go */
    if (!n) goto done;
    /* new ids */
    uint32_t next = bound;
    const uint32_t nu32 = u32 ? 0 : 1, t32 = u32 ? u32 : next++;
    int need64 = 0;
    for (size_t at = function_pos; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu; uint32_t ty = 0;
        if ((op == 65 || op == 66) && wc >= 6 && i[3] < bound && cand[i[3]]) {
            for (uint32_t m = 0; m < nv; m++) if (vid[m] == i[3] && (mask >> vbind[m] & 1u) && nvmtl_bo_int_width(w, def, bound, i[5], &ty) == 64) need64 = 1;
        }
        at += wc;
    }
    const uint32_t nu64 = (need64 && !u64) ? 1 : 0, t64 = need64 ? (u64 ? u64 : next++) : 0;
    const uint32_t pcu32 = next++;                         /* OpTypePointer PushConstant u32 */
    uint32_t member_c[32] = {0};                           /* member index constants, by binding */
    for (uint32_t b = 0; b < 32; b++) if (mask >> b & 1u) member_c[b] = next++;
    uint32_t mul32[9] = {0}, mul64[9] = {0};               /* G / S constants, by log2(G / S) */
    for (uint32_t m = 0; m < nv; m++) if (mask >> vbind[m] & 1u) {
        const uint32_t lg = (uint32_t)__builtin_ctz(NVMTL_BUFOFF_GRAN / vstride[m]);
        if (lg && !mul32[lg]) mul32[lg] = next++;
        if (lg && need64 && !mul64[lg]) mul64[lg] = next++;
    }
    out.cap = nw + 96 + (size_t)chains * 24;
    out.w = malloc(out.cap * 4); if (!out.w) goto done;
    nvmtl_bo_put(&out, w, 5);
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (at == type_pos) {   /* the new members' offsets, after the other annotations */
            uint32_t k = 0;
            for (uint32_t b = 0; b < 32; b++) if (mask >> b & 1u) { NVMTL_BO_EMIT(&out, (5u << 16) | 72u, push_block, push_members + k, 35u, base + 4 * k); k++; }
        }
        if (at == function_pos) {   /* new types, the block with its new members, its pointer and variable, constants */
            if (nu32) NVMTL_BO_EMIT(&out, (4u << 16) | 21u, t32, 32u, 0u);
            if (nu64) NVMTL_BO_EMIT(&out, (4u << 16) | 21u, t64, 64u, 0u);
            const uint32_t *pb = w + def[push_block];
            NVMTL_BO_EMIT(&out, ((2 + push_members + n) << 16) | 30u, push_block);
            nvmtl_bo_put(&out, pb + 2, push_members);
            for (uint32_t k = 0; k < n; k++) nvmtl_bo_put(&out, &t32, 1);
            NVMTL_BO_EMIT(&out, (4u << 16) | 32u, push_ptr, 9u, push_block);
            NVMTL_BO_EMIT(&out, (4u << 16) | 32u, pcu32, 9u, t32);
            { uint32_t k = 0; for (uint32_t b = 0; b < 32; b++) if (mask >> b & 1u) { NVMTL_BO_EMIT(&out, (4u << 16) | 43u, t32, member_c[b], push_members + k); k++; } }
            for (uint32_t lg = 1; lg < 9; lg++) {
                if (mul32[lg]) NVMTL_BO_EMIT(&out, (4u << 16) | 43u, t32, mul32[lg], 1u << lg);
                if (mul64[lg]) NVMTL_BO_EMIT(&out, (5u << 16) | 43u, t64, mul64[lg], 1u << lg, 0u);
            }
            NVMTL_BO_EMIT(&out, (4u << 16) | 59u, push_ptr, push_var, 9u);
        }
        if (at == def[push_block] || at == def[push_ptr] || at == def[push_var]) { at += wc; continue; }
        if (at >= function_pos && (op == 65 || op == 66) && wc >= 6 && i[3] < bound && cand[i[3]]) {
            uint32_t s = 0, b = 0;
            for (uint32_t m = 0; m < nv; m++) if (vid[m] == i[3]) { s = vstride[m]; b = vbind[m]; }
            if (s && (mask >> b & 1u)) {
                uint32_t ty = 0; const uint32_t width = nvmtl_bo_int_width(w, def, bound, i[5], &ty);
                const uint32_t lg = (uint32_t)__builtin_ctz(NVMTL_BUFOFF_GRAN / s);
                const uint32_t p = next++, q = next++;
                NVMTL_BO_EMIT(&out, (5u << 16) | 65u, pcu32, p, push_var, member_c[b]);
                NVMTL_BO_EMIT(&out, (4u << 16) | 61u, t32, q, p);
                uint32_t v = q;
                if (width == 64) { const uint32_t x = next++; NVMTL_BO_EMIT(&out, (4u << 16) | 113u, t64, x, v); v = x; }
                if (lg) { const uint32_t x = next++; NVMTL_BO_EMIT(&out, (5u << 16) | 132u, width == 64 ? t64 : t32, x, v, width == 64 ? mul64[lg] : mul32[lg]); v = x; }
                const uint32_t ni = next++;
                NVMTL_BO_EMIT(&out, (5u << 16) | 128u, ty, ni, i[5], v);
                NVMTL_BO_EMIT(&out, i[0], i[1], i[2], i[3], i[4], ni);
                if (wc > 6) nvmtl_bo_put(&out, i + 6, wc - 6);
                at += wc; continue;
            }
        }
        nvmtl_bo_put(&out, i, wc);
        at += wc;
    }
    if (out.failed || next > (1u << 22)) goto done;
    out.w[3] = next;
    result = out.w; out.w = NULL;
    meta->mask = mask; meta->count = n; *out_bytes = out.n * 4;
done:
    if (meta) { meta->have = have | bad; meta->why = why; meta->op = other; }
    free(out.w); free(def); free(set); free(bind); free(stride); free(mof); free(block); free(cand);
    return result;
}
#undef NVMTL_BO_EMIT

/* 2026-11-13 night (branch dbufdef): what the pass costs on Xe2 (DESCRIPTOR-MODEL.md section 17). ANV hands a compute
   shader its push data in COMPUTE_WALKER's inline data only when it reads fewer than 8 push dwords (mesa014
   anv_nir_compute_push_layout.c, compute_final_push_range: 16 needs uses_efficient_64bit, which Xe2 lacks); otherwise
   every thread first loads its push data from memory (LSC block loads, then the loads that depend on them). The pass
   adds a dword per converted buffer, so a kernel near the limit moves out of the inline registers: two serial memory
   round trips more per thread (activation_relu: 4 -> 6 SENDs, brw's cycles 407 -> 623). A kernel already past the limit
   pays one more uniform load and the index arithmetic only.

   nvmtl_bo_push_dwords: an estimate of the push dwords ANV reads for a GLCompute module in the descriptor-buffer model:
   the push block's dwords its access chains (or loads) name, 3 for the base workgroup when it reads WorkgroupId or
   GlobalInvocationId (brw lowers both through nir_lower_compute_system_values with has_base_workgroup_id), 3 for
   NumWorkgroups, 1 per descriptor set it reads a resource from and 1 per set it reads a sampler from (the set's surface
   and sampler offsets, anv_binding_buffer_data). It may count a push dword NIR later drops; checked against ANV's own
   push maps by user/tests/metal/bufoff_isa.c. *app: the push block's dwords alone. Returns 0 when the module is not
   understood. */
#define NVMTL_BO_INLINE_DWORDS 8u
/* how a pointer into the push block (chain = its id) is used, as far as NIR will fold it: 0 nothing that survives, 1 only
   the low 32 bits of a 64-bit value, 2 anything else. Folded: a load whose every use is an OpUConvert / OpSConvert to 32
   bits (1), each of whose uses is an OpBitwiseAnd with a constant c whose only use divides (OpUDiv) or shifts
   (OpShiftRightLogical) it by a constant that leaves nothing of c (0): the remainder idiom of the translator's buffer
   lowering, (entry & 3) / 4, which brw folds to 0. */
static int nvmtl_bo_push_use(const uint32_t *w, size_t nw, const uint32_t *def, uint32_t bound, uint32_t function_pos, size_t chain_at)
{
#define NVMTL_BO_CONST(id) ((id) < bound && def[id] && (w[def[id]] & 0xffffu) == 43 && (w[def[id]] >> 16) == 4 ? w + def[id] : NULL)
    const uint32_t chain = w[chain_at + 2];
    uint32_t load = 0, nconv = 0, conv[8], dead = 1;
    for (size_t at = function_pos; at < nw;) {   /* the chain's uses: one OpLoad, nothing else */
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (at != chain_at) for (uint32_t k = 1; k < wc; k++) if (i[k] == chain && !nvmtl_spv_word_is_literal(i, k)) {
            if (op != 61 || k != 3 || load) return 2;
            load = i[2];
        }
        at += wc;
    }
    if (!load) return 0;
    for (size_t at = function_pos; at < nw;) {   /* the load's uses: 32-bit conversions */
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        for (uint32_t k = 3; k < wc; k++) if (i[k] == load && !nvmtl_spv_word_is_literal(i, k)) {
            if ((op != 113 && op != 114) || wc != 4 || i[1] >= bound || !def[i[1]] || (w[def[i[1]]] & 0xffffu) != 21 ||
                w[def[i[1]] + 2] != 32 || nconv == 8) return 2;
            conv[nconv++] = i[2];
        }
        at += wc;
    }
    if (!nconv) return 0;
    for (uint32_t c = 0; c < nconv && dead; c++)   /* each conversion's uses: (x & c) >> or / d, nothing of c left */
        for (size_t at = function_pos; at < nw && dead;) {
            const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
            for (uint32_t k = 3; k < wc && dead; k++) if (i[k] == conv[c] && !nvmtl_spv_word_is_literal(i, k)) {
                const uint32_t *m = op == 199 && wc == 5 ? NVMTL_BO_CONST(i[k == 3 ? 4 : 3]) : NULL;
                if (!m) { dead = 0; break; }
                uint32_t uses = 0, ok = 1;
                for (size_t a2 = function_pos; a2 < nw;) {
                    const uint32_t *j = w + a2, jc = j[0] >> 16, jo = j[0] & 0xffffu;
                    for (uint32_t q = 3; q < jc; q++) if (j[q] == i[2] && !nvmtl_spv_word_is_literal(j, q)) {
                        uses++;
                        const uint32_t *d = jc == 5 && q == 3 ? NVMTL_BO_CONST(j[4]) : NULL;
                        if (!d || !((jo == 134 && d[3] > m[3]) || (jo == 194 && d[3] < 32 && !(m[3] >> d[3])))) ok = 0;
                    }
                    a2 += jc;
                }
                if (!uses || !ok) dead = 0;
            }
            at += wc;
        }
#undef NVMTL_BO_CONST
    return dead ? 0 : 1;
}
static uint32_t nvmtl_bo_push_dwords(const void *bytes, size_t nbytes, uint32_t *app)
{
    const uint32_t *w = bytes; const size_t nw = nbytes / 4;
    if (app) *app = 0;
    if (!w || nbytes % 4 || nw < 5 || w[0] != 0x07230203u) return 0;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 20)) return 0;
    uint32_t *def = calloc(bound, sizeof *def), *set = calloc(bound, sizeof *set), *bi = calloc(bound, sizeof *bi);
    uint8_t *used = calloc(bound, 1);
    uint32_t result = 0, function_pos = 0, push_var = 0, push_block = 0, nm = 0;
    uint64_t dw = 0;                       /* push dwords named (the block is at most 256 bytes) */
    uint32_t mo[64], mid[64], nmo = 0;     /* the push block's member offsets */
    const uint32_t *pb = NULL;
    if (!def || !set || !bi || !used) goto done;
    for (uint32_t k = 0; k < bound; k++) { set[k] = ~0u; bi[k] = ~0u; }
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (!wc || at + wc > nw) goto done;
        if (op == 54 && !function_pos) function_pos = (uint32_t)at;
        if (op >= 19 && op <= 39 && wc >= 2 && i[1] < bound) def[i[1]] = (uint32_t)at;
        else if ((op == 43 || op == 59) && wc >= 4 && i[2] < bound) {
            def[i[2]] = (uint32_t)at;
            if (op == 59 && i[3] == 9 && !push_var) push_var = i[2];
        }
        if (op == 71 && wc == 4 && i[1] < bound) { if (i[2] == 34) set[i[1]] = i[3]; if (i[2] == 11) bi[i[1]] = i[3]; }
        at += wc;
    }
    if (!function_pos) goto done;
    for (size_t at = function_pos; at < nw;) {   /* the globals the functions name */
        const uint32_t *i = w + at, wc = i[0] >> 16;
        for (uint32_t k = 1; k < wc; k++) if (i[k] < bound && def[i[k]] && !nvmtl_spv_word_is_literal(i, k)) used[i[k]] = 1;
        at += wc;
    }
    if (push_var && def[push_var]) {
        const uint32_t *pv = w + def[push_var], *pp = pv[1] < bound && def[pv[1]] ? w + def[pv[1]] : NULL;
        if (pp && (pp[0] & 0xffffu) == 32 && pp[3] < bound && def[pp[3]] && (w[def[pp[3]]] & 0xffffu) == 30) push_block = pp[3];
    }
    if (push_block) {
        pb = w + def[push_block]; nm = (pb[0] >> 16) - 2;
        for (size_t at = 5; at < function_pos;) {
            const uint32_t *i = w + at, wc = i[0] >> 16;
            if ((i[0] & 0xffffu) == 72 && wc == 5 && i[1] == push_block && i[3] == 35 && i[2] < nm && nmo < 64) { mid[nmo] = i[2]; mo[nmo++] = i[4]; }
            at += wc;
        }
    }
    /* the dwords of member m (component c, or ~0u for all of it) */
#define NVMTL_BO_MARK(m, c) do { \
        uint32_t from_ = ~0u, size_ = 0, comp_ = 0; \
        for (uint32_t x = 0; x < nmo; x++) if (mid[x] == (m)) from_ = mo[x]; \
        if (from_ == ~0u || pb[2 + (m)] >= bound || !def[pb[2 + (m)]]) { dw = ~0ull; break; } \
        const uint32_t *t_ = w + def[pb[2 + (m)]]; \
        if ((t_[0] & 0xffffu) == 21 || (t_[0] & 0xffffu) == 22) size_ = t_[2] / 8; \
        else if ((t_[0] & 0xffffu) == 23 && t_[2] < bound && def[t_[2]]) { comp_ = w[def[t_[2]] + 2] / 8; size_ = comp_ * t_[3]; } \
        if (!size_) { size_ = 256; for (uint32_t x = 0; x < nmo; x++) if (mo[x] > from_ && mo[x] - from_ < size_) size_ = mo[x] - from_; } \
        if ((c) != ~0u && comp_) { from_ += (c) * comp_; size_ = comp_; } \
        for (uint32_t b_ = from_ / 4; b_ < (from_ + size_ + 3) / 4 && b_ < 64; b_++) dw |= 1ull << b_; \
    } while (0)
    for (size_t at = function_pos; push_block && at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if ((op == 65 || op == 66) && wc >= 5 && i[3] == push_var) {
            const uint32_t *c0 = i[4] < bound && def[i[4]] ? w + def[i[4]] : NULL;
            const uint32_t *c1 = wc >= 6 && i[5] < bound && def[i[5]] ? w + def[i[5]] : NULL;
            if (c0 && (c0[0] & 0xffffu) == 43 && c0[3] < nm) {
                const uint32_t comp = c1 && (c1[0] & 0xffffu) == 43 ? c1[3] : ~0u;
                const int k = comp == ~0u ? nvmtl_bo_push_use(w, nw, def, bound, function_pos, at) : 2;
                if (k == 1) {   /* only its low 32 bits are used: the member's first dword */
                    uint32_t from = ~0u; for (uint32_t x = 0; x < nmo; x++) if (mid[x] == c0[3]) from = mo[x];
                    if (from < 256) dw |= 1ull << (from / 4); else dw = ~0ull;
                } else if (k) NVMTL_BO_MARK(c0[3], comp);
            } else dw = ~0ull;   /* a member chosen at run time: all of them */
        } else if (op == 61 && wc >= 4 && i[3] == push_var) {   /* the whole block loaded */
            for (uint32_t m = 0; m < nm; m++) NVMTL_BO_MARK(m, ~0u);
        }
        at += wc;
    }
#undef NVMTL_BO_MARK
    {
        uint32_t drv = 0; uint8_t sres[8] = {0}, ssam[8] = {0};
        for (uint32_t v = 0; v < bound; v++) {
            if (!used[v] || !def[v] || (w[def[v]] & 0xffffu) != 59) continue;
            if (bi[v] == 26 || bi[v] == 28) drv |= 1;   /* WorkgroupId, GlobalInvocationId: base_workgroup */
            if (bi[v] == 24) drv |= 2;                  /* NumWorkgroups */
            if (set[v] < 8) {
                const uint32_t *p = w + def[v], *t = p[1] < bound && def[p[1]] ? w + def[p[1]] : NULL;
                if (t && (t[0] & 0xffffu) == 32 && t[3] < bound && def[t[3]]) {
                    t = w + def[t[3]];
                    while (((t[0] & 0xffffu) == 28 || (t[0] & 0xffffu) == 29) && t[2] < bound && def[t[2]]) t = w + def[t[2]];
                    if ((t[0] & 0xffffu) == 26) ssam[set[v]] = 1; else sres[set[v]] = 1;
                }
            }
        }
        result = (uint32_t)__builtin_popcountll(dw);
        if (app) *app = result;
        result += (drv & 1 ? 3u : 0u) + (drv & 2 ? 3u : 0u);
        for (uint32_t s = 0; s < 8; s++) result += sres[s] + ssam[s];
    }
done:
    free(def); free(set); free(bi); free(used);
    return result;
}
#endif
