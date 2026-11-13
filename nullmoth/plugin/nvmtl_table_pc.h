/* Compact constant-index address-table reads into push constants. Opt-in experiment.
 * A declined module is left byte-for-byte unchanged by the caller. No shader access
 * or Metal resource ownership is removed: the same 64-bit entries are pushed per draw.
 * Existing push blocks, dynamic indices, escaping table pointers and self addresses
 * stay on the descriptor path. This header has no Vulkan/ObjC dependency for validation.
 */
#ifndef NVMTL_TABLE_PC_H
#define NVMTL_TABLE_PC_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_spv_literal.h"

#define NVMTL_PC_TABLE_MAX 12u
#define NVMTL_PC_TABLE_VS_OFFSET 0u
#define NVMTL_PC_TABLE_FS_OFFSET 176u
typedef struct { uint32_t count, index[NVMTL_PC_TABLE_MAX]; } nvmtl_pc_table;
typedef struct { uint32_t *w; size_t n, cap; int failed; } nvmtl_pc_words;
static void nvmtl_pc_put(nvmtl_pc_words *o, const uint32_t *w, size_t n)
{
    if (o->failed) return;
    if (n > o->cap - o->n) { o->failed = 1; return; }
    memcpy(o->w + o->n, w, n * sizeof *w); o->n += n;
}
#define NVMTL_PC_EMIT(o, ...) do { const uint32_t a_[] = { __VA_ARGS__ }; nvmtl_pc_put((o), a_, sizeof a_ / sizeof *a_); } while (0)
static int nvmtl_pc_const(const uint32_t *w, const uint32_t *def, uint32_t bound, uint32_t id, uint32_t *value)
{
    if (!id || id >= bound || !def[id]) return 0;
    const uint32_t *i = w + def[id], wc = i[0] >> 16;
    if ((i[0] & 0xffffu) != 43 || (wc != 4 && wc != 5) || (wc == 5 && i[4])) return 0;
    if (i[1] >= bound || !def[i[1]]) return 0;
    const uint32_t *ty = w + def[i[1]];
    if ((ty[0] & 0xffffu) != 21 || (ty[2] != 32 && ty[2] != 64)) return 0;
    *value = i[3]; return 1;
}
/* Returns an owned module on success and NULL on a conservative refusal. `meta`
 * is published only on success. base/capacity are disjoint for vertex/fragment,
 * preserving sample positions [96,160) and stencil emulation [160,164).
 */
/* compute2: flags for nvmtl_spirv_table_pc_ex. 0 = the graphics pass exactly as glpc2/prod7.
 * NVMTL_PC_MERGE: a module may already have ONE static push block (a compute kernel's 48-byte dispatch region,
 * translator stage_input/kernel_grid.rs, members at [0,48)). Vulkan allows one push block per entry point
 * (VUID-StandaloneSpirv-OpEntryPoint-06674), which is why prod7 left compute alone: the qwords are appended to that block
 * as members at base+8k instead of a second block. The block's existing members must all end at or below base, the
 * block type may be used only by its PushConstant pointer type (no whole-block load or copy) and that pointer type only
 * by the one variable; otherwise the module is left unchanged.
 * NVMTL_PC_SET0: the table must be decorated DescriptorSet 0 (the values are pushed from set 0's slots). */
#define NVMTL_PC_MERGE 1u
#define NVMTL_PC_SET0  2u
/* NVMTL_PC_EXACT (2026-11-13 night, branch dbufgb; the descriptor-buffer model only, so the classic model's modules stay
 * byte for byte what they were): words the SPIR-V grammar fixes as literals (nvmtl_spv_literal.h) are not taken for
 * uses of the push block, its pointer type, its variable or a table pointer. Without it a module whose dispatch block
 * has id 12 was refused by every `OpVariable .. StorageBuffer`, and one whose table chain id equals a GLSL.std.450
 * instruction number by that OpExtInst. */
#define NVMTL_PC_EXACT 4u
#define NVMTL_PC_TABLE_CS_OFFSET 48u
static uint32_t *nvmtl_spirv_table_pc_ex(const void *bytes, size_t nbytes, uint32_t model, uint32_t base,
                                        uint32_t capacity, uint32_t flags, nvmtl_pc_table *meta, size_t *out_bytes)
{
    const uint32_t *w = bytes; const size_t nw = nbytes / 4;
    if (!w || nbytes % 4 || nw < 5 || w[0] != 0x07230203u || !capacity || capacity > NVMTL_PC_TABLE_MAX
        || (base & 7u) || base + capacity * 8u > 256u) return NULL;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 20) || nw > (1u << 24)) return NULL;
    uint32_t *def = calloc(bound, sizeof *def), *ptr = calloc(bound, sizeof *ptr);
    uint8_t *derived = calloc(bound, 1);
    uint32_t table = 0, entries = 0, entry_pos = 0, function_pos = 0, type_pos = 0, table_pos = 0, table_storage = 0;
    uint32_t push_var = 0, push_ptr = 0, push_block = 0, push_members = 0, table_set = ~0u;
    nvmtl_pc_table plan = {0}; uint32_t *result = NULL; nvmtl_pc_words out = {0};
    if (!def || !ptr || !derived) goto done;
    /* IDs needed by the recognizer. All other instructions are retained verbatim. */
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (!wc || at + wc > nw) goto done;
        if (op == 15) {
            if (wc < 4 || i[1] != model || ++entries != 1) goto done;
            entry_pos = (uint32_t)at;
        }
        if (op == 54 && !function_pos) function_pos = (uint32_t)at;
        if (op >= 19 && op <= 39 && wc >= 2) {
            if (i[1] >= bound) goto done;
            if (!type_pos) type_pos = (uint32_t)at;
            def[i[1]] = (uint32_t)at;
        } else if ((op == 43 || op == 59) && wc >= 4) {
            if (i[2] >= bound) goto done;
            def[i[2]] = (uint32_t)at;
            if (op == 59 && i[3] == 9) {  /* one static push block per entry point */
                if (!(flags & NVMTL_PC_MERGE) || push_var) goto done;
                push_var = i[2]; push_ptr = i[1];
            }
        }
        if (op == 71 && wc == 4 && i[2] == 33 && i[3] == 640) {
            if (table && table != i[1]) goto done;
            table = i[1];
        }
        at += wc;
    }
    if (!table || table >= bound || !def[table] || !type_pos || !function_pos || entries != 1) goto done;
    const uint32_t *tv = w + def[table];
    if ((tv[0] & 0xffffu) != 59 || (tv[0] >> 16) != 4 || (tv[3] != 2 && tv[3] != 12)) goto done;
    table_pos = def[table]; table_storage = tv[3];
    if (tv[1] >= bound || !def[tv[1]]) goto done;
    const uint32_t *tp = w + def[tv[1]];
    if ((tp[0] & 0xffffu) != 32 || (tp[0] >> 16) != 4 || tp[2] != table_storage || tp[3] >= bound || !def[tp[3]]) goto done;
    const uint32_t *block = w + def[tp[3]];
    if ((block[0] & 0xffffu) != 30 || (block[0] >> 16) != 3 || block[2] >= bound || !def[block[2]]) goto done;
    const uint32_t arr_id = block[2], *arr = w + def[arr_id];
    if (((arr[0] & 0xffffu) != 28 && (arr[0] & 0xffffu) != 29) || (arr[0] >> 16) < 3 || arr[2] >= bound || !def[arr[2]]) goto done;
    const uint32_t elem = arr[2], *el = w + def[elem];
    int elem_ok = (el[0] & 0xffffu) == 21 && (el[0] >> 16) == 4 && el[2] == 64;
    if ((el[0] & 0xffffu) == 23 && (el[0] >> 16) == 4 && el[3] == 2 && el[2] < bound && def[el[2]]) {
        const uint32_t *scalar = w + def[el[2]];
        elem_ok = (scalar[0] & 0xffffu) == 21 && (scalar[0] >> 16) == 4 && scalar[2] == 32;
    }
    if (!elem_ok) goto done;
    uint32_t stride = 0, member_offset = ~0u;
    for (size_t at = 5; at < function_pos;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (op == 71 && wc == 4 && i[1] == arr_id && i[2] == 6) stride = i[3];
        if (op == 72 && wc == 5 && i[1] == tp[3] && i[2] == 0 && i[3] == 35) member_offset = i[4];
        at += wc;
    }
    if (stride != 8 || member_offset != 0) goto done;
    if (flags & NVMTL_PC_SET0) {
        for (size_t at = 5; at < function_pos;) {
            const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
            if (op == 71 && wc == 4 && i[1] == table && i[2] == 34) table_set = i[3];
            at += wc;
        }
        if (table_set != 0) goto done;
    }
    if (push_var) {
        /* NVMTL_PC_MERGE: the module's own push block (pointer -> Block struct) gets the qwords as extra members. */
        if (push_ptr >= bound || !def[push_ptr] || push_var >= bound) goto done;
        const uint32_t *pp = w + def[push_ptr];
        if ((pp[0] & 0xffffu) != 32 || (pp[0] >> 16) != 4 || pp[2] != 9 || pp[3] >= bound || !def[pp[3]]) goto done;
        push_block = pp[3];
        const uint32_t *pb = w + def[push_block];
        if ((pb[0] & 0xffffu) != 30 || (pb[0] >> 16) < 3 || (pb[0] >> 16) - 2 > 64) goto done;
        push_members = (pb[0] >> 16) - 2;
        uint64_t placed = 0; int is_block = 0;
        for (size_t at = 5; at < function_pos;) {
            const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
            if (op == 71 && wc == 3 && i[1] == push_block && i[2] == 2) is_block = 1;
            if (op == 72 && wc == 5 && i[1] == push_block && i[3] == 35) {
                if (i[2] >= push_members || i[4] > base) goto done;
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
        if (!is_block || placed != (push_members == 64 ? ~0ull : (1ull << push_members) - 1)) goto done;
        /* The block type is used only by its pointer type, the pointer type only by the variable, and the variable by
         * nothing in the global section (all three move to the end of it, after the element type). Names and
         * decorations may name them. A literal that merely equals one of these ids refuses the module. */
        for (size_t at = 5; at < nw;) {
            const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
            /* id operands only where the grammar is simple; elsewhere every word counts (a literal can only refuse) */
            uint32_t from = 1, to = wc;
            switch (op) {
            case 5: case 6: case 71: case 72: case 15: case 16: case 17: case 1: case 2: case 3: case 4: case 10: case 11: case 14:
            case 21: case 22: case 246: case 247: case 248: case 249: case 250: case 253: case 254: from = to = 0; break;  /* debug, modes, int/float types, merges, labels, branches, returns */
            case 32: from = 3; break;                              /* OpTypePointer: the pointee */
            case 43: case 50: from = 1; to = 2; break;             /* OpConstant: the type, not the value */
            case 61: from = 1; to = 4; break;                      /* OpLoad: type, result, pointer */
            case 62: from = 1; to = 3; break;                      /* OpStore: pointer, object */
            case 81: from = 1; to = 4; break;                      /* OpCompositeExtract: type, result, composite */
            default: break;
            }
            if (!(at == def[push_ptr] || at == def[push_block] || (op == 59 && wc >= 4 && i[2] == push_var))) {
                for (uint32_t k = from; k < to; k++) {
                    if ((flags & NVMTL_PC_EXACT) && nvmtl_spv_word_is_literal(i, k)) continue;
                    if (i[k] == push_block || i[k] == push_ptr) goto done;
                    if (at < function_pos && i[k] == push_var) goto done;
                }
            }
            at += wc;
        }
        if ((w + def[push_var])[0] >> 16 != 4) goto done;   /* no initializer */
    }
    derived[table] = 1;
    /* Derive only access-chain pointers. Any other use of a derived pointer is
     * refused below, including passing it to a function, phi or storing it.
     */
    for (size_t at = function_pos; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if ((op == 65 || op == 66) && wc >= 5 && i[3] < bound && derived[i[3]]) {
            if (i[1] >= bound || !def[i[1]] || i[2] >= bound) goto done;
            const uint32_t *old = w + def[i[1]];
            if ((old[0] & 0xffffu) != 32 || (old[0] >> 16) != 4 || old[2] != table_storage) goto done;
            derived[i[2]] = 1; ptr[i[1]] = 1;
            if (i[3] == table) {
                uint32_t zero, index;
                if (wc < 6 || !nvmtl_pc_const(w, def, bound, i[4], &zero) || zero
                    || !nvmtl_pc_const(w, def, bound, i[5], &index)) goto done;
                /* [32,64) contains self addresses into the old table. */
                if (index >= 32 && index < 64) goto done;
                if (index >= 192) goto done;
                uint32_t k = 0; while (k < plan.count && plan.index[k] != index) k++;
                if (k == plan.count) {
                    if (plan.count == capacity) goto done;
                    plan.index[plan.count++] = index;
                }
            }
        }
        at += wc;
    }
    if (!plan.count) goto done;
    /* Operand words that happen to equal a pointer ID can only cause a refusal.
     * Never silently accept an unrecognized pointer operation.
     */
    for (size_t at = function_pos; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if ((op == 65 || op == 66) && wc >= 5 && i[3] < bound && derived[i[3]]) {
            for (uint32_t k = 4; k < wc; k++) if (i[k] < bound && derived[i[k]]) goto done;
        } else if (op == 61 && wc >= 4 && i[3] < bound && derived[i[3]]) {
            if (i[3] == table) goto done;  /* whole-block loads */
        } else if (op != 8 && op != 317) {
            for (uint32_t k = 1; k < wc; k++)
                if (i[k] < bound && derived[i[k]] && !((flags & NVMTL_PC_EXACT) && nvmtl_spv_word_is_literal(i, k))) goto done;
        }
        at += wc;
    }
    /* Sort so repeated compilation does not depend on instruction use order. */
    for (uint32_t k = 1; k < plan.count; k++) for (uint32_t j = k; j && plan.index[j] < plan.index[j-1]; j--) {
        uint32_t v = plan.index[j]; plan.index[j] = plan.index[j-1]; plan.index[j-1] = v;
    }
    uint32_t next = bound, u32 = 0;
    for (uint32_t id = 1; id < bound; id++) if (def[id]) {
        const uint32_t *i = w + def[id];
        if ((i[0] & 0xffffu) == 21 && (i[0] >> 16) == 4 && i[2] == 32 && !i[3]) { u32 = id; break; }
    }
    if (!u32) goto done;
    const uint32_t pc_block = push_var ? push_block : next++, pc_pointer = push_var ? push_ptr : next++, pc_var = push_var ? push_var : next++;
    const uint32_t first_member = push_var ? push_members : 0;
    uint32_t slot_constant[NVMTL_PC_TABLE_MAX];
    for (uint32_t k = 0; k < plan.count; k++) slot_constant[k] = next++;
    uint32_t ntypes = 0;
    for (uint32_t k = 1; k < bound; k++) if (ptr[k]) { ptr[k] = next++; ntypes++; }
    out.cap = nw + 64 + plan.count * 11 + ntypes * 4;
    out.w = malloc(out.cap * 4); if (!out.w) goto done;
    nvmtl_pc_put(&out, w, 5); out.w[3] = next;
    for (size_t at = 5; at < nw;) {
        const uint32_t *i = w + at, wc = i[0] >> 16, op = i[0] & 0xffffu;
        if (at == type_pos) {
            if (!push_var) NVMTL_PC_EMIT(&out, (3u << 16) | 71u, pc_block, 2u);  /* Block */
            for (uint32_t k = 0; k < plan.count; k++)
                NVMTL_PC_EMIT(&out, (5u << 16) | 72u, pc_block, first_member + k, 35u, base + k * 8);
        }
        if (at == function_pos) {
            if (push_var) {   /* the module's block, its members and then ours */
                const uint32_t *pb = w + def[push_block];
                NVMTL_PC_EMIT(&out, ((2 + push_members + plan.count) << 16) | 30u, pc_block);
                nvmtl_pc_put(&out, pb + 2, push_members);
            } else NVMTL_PC_EMIT(&out, ((2 + plan.count) << 16) | 30u, pc_block);
            for (uint32_t k = 0; k < plan.count; k++) nvmtl_pc_put(&out, &elem, 1);
            NVMTL_PC_EMIT(&out, (4u << 16) | 32u, pc_pointer, 9u, pc_block);
            for (uint32_t k = 0; k < plan.count; k++)
                NVMTL_PC_EMIT(&out, (4u << 16) | 43u, u32, slot_constant[k], first_member + k);
            for (uint32_t k = 1; k < bound; k++) if (ptr[k])
                NVMTL_PC_EMIT(&out, (4u << 16) | 32u, ptr[k], 9u, w[def[k] + 3]);
            NVMTL_PC_EMIT(&out, (4u << 16) | 59u, pc_pointer, pc_var, 9u);
        }
        if (push_var && (at == def[push_block] || at == def[push_ptr] || (op == 59 && wc >= 4 && i[2] == push_var))) { at += wc; continue; }
        if (at == table_pos || ((op == 5 || op == 71) && wc >= 2 && i[1] == table)) { at += wc; continue; }
        if (at == entry_pos) {
            uint32_t name_words = 0; int terminated = 0;
            for (uint32_t k = 3; k < wc; k++) {
                name_words++;
                if (!(i[k] & 0xffu) || !(i[k] & 0xff00u) || !(i[k] & 0xff0000u) || !(i[k] & 0xff000000u)) { terminated = 1; break; }
            }
            if (!terminated) goto done;
            size_t start = out.n;
            nvmtl_pc_put(&out, i, 3 + name_words);
            for (uint32_t k = 3 + name_words; k < wc; k++) if (i[k] != table) nvmtl_pc_put(&out, i + k, 1);
            if (w[1] >= 0x00010400u) {   /* SPIR-V 1.4+: every global the entry point uses is listed (a merged block may be too) */
                int listed = 0;
                for (uint32_t k = 3 + name_words; k < wc; k++) if (i[k] == pc_var) listed = 1;
                if (!listed) nvmtl_pc_put(&out, &pc_var, 1);
            }
            out.w[start] = ((uint32_t)(out.n - start) << 16) | 15u;
        } else if (at >= function_pos && (op == 65 || op == 66) && wc >= 5 && i[3] < bound && derived[i[3]]) {
            if (i[3] == table) {
                uint32_t index = 0; if (!nvmtl_pc_const(w, def, bound, i[5], &index)) goto done;
                uint32_t slot = 0; while (slot < plan.count && plan.index[slot] != index) slot++;
                if (slot == plan.count) goto done;
                NVMTL_PC_EMIT(&out, ((wc - 1) << 16) | op, ptr[i[1]], i[2], pc_var, slot_constant[slot]);
                if (wc > 6) nvmtl_pc_put(&out, i + 6, wc - 6);
            } else {
                NVMTL_PC_EMIT(&out, i[0], ptr[i[1]], i[2]);
                nvmtl_pc_put(&out, i + 3, wc - 3);
            }
        } else nvmtl_pc_put(&out, i, wc);
        at += wc;
    }
    if (out.failed) goto done;
    result = out.w; out.w = NULL;
    *meta = plan; *out_bytes = out.n * 4;
done:
    free(out.w); free(def); free(ptr); free(derived);
    return result;
}
static uint32_t *nvmtl_spirv_table_pc(const void *bytes, size_t nbytes, uint32_t model, uint32_t base,
                                     uint32_t capacity, nvmtl_pc_table *meta, size_t *out_bytes)
{
    return nvmtl_spirv_table_pc_ex(bytes, nbytes, model, base, capacity, 0, meta, out_bytes);
}
#undef NVMTL_PC_EMIT
#endif
