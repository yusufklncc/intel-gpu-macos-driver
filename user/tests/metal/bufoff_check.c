// SPDX-License-Identifier: MIT
// bufoff_check: the plugin's storage-buffer offset pass (nullmoth/plugin/nvmtl_bufoff_pc.h) on real kernels, off the GPU.
// Each module goes the way nvmtl_vk_compute_pipeline_create takes it in the descriptor-buffer model: the translator's
// address lowering (with --lower: libnvmtl_translate.dylib's nvmtl_lower_buffer_addresses_ex, flags as the plugin passes
// them on ANV), cs-table-pc (nvmtl_spirv_table_pc_ex, NVMTL_PC_MERGE | NVMTL_PC_SET0 | NVMTL_PC_EXACT) and then the pass. The outputs go to
// <outdir> for spirv-val; the pass's own checks run here:
//   - every access chain into a converted binding's variable has its element index replaced by an OpIAdd of the old
//     index and (OpLoad of the new push member, converted to 64 bits when the index is) times G / S;
//   - nothing else in the module changed: with the inserted instructions taken out and the push block's new members
//     left off, the instruction stream is the input's, word for word (ids included);
//   - a module the pass refuses is returned as NULL (the plugin keeps it byte for byte).
//
//   clang -O1 -g -Wall -I nullmoth/plugin -o bufoff_check user/tests/metal/bufoff_check.c
//   ./bufoff_check [-v] [--lower <libnvmtl_translate.dylib>] <outdir> <kernel.spv>...
// -v: one line per kernel the pass left (wholly or in part) as it was, with the reasons (NVMTL_BO_R_*).
// --old-pc: cs-table-pc without NVMTL_PC_EXACT (the classic model's pass, and the buffer model's before 13 Nov night).
// Exit: 0 every check passed, 1 a check failed, 2 usage or I/O.
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "nvmtl_table_pc.h"
#include "nvmtl_bufoff_pc.h"

typedef int (*lower_ex_fn)(const uint8_t *, size_t, uint32_t, uint8_t **, size_t *, uint32_t *, char *, size_t);
typedef void (*free_fn)(uint8_t *, size_t);

static uint32_t *slurp(const char *path, size_t *nbytes)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *nbytes = (size_t)n; return b;
}
static void spit(const char *dir, const char *name, const char *ext, const void *b, size_t n)
{
    char p[1024]; const char *base = strrchr(name, '/'); base = base ? base + 1 : name;
    snprintf(p, sizeof p, "%s/%s.%s.spv", dir, base, ext);
    FILE *f = fopen(p, "wb"); if (f) { fwrite(b, 1, n, f); fclose(f); }
}

/* the structural check. Output instructions whose result id is new (>= the input's bound), and the push block's new
   member offsets, are the pass's own and are taken out after checking them: a rewritten chain's element index must be
   an OpIAdd of its old index and the new push member's value (loaded through an access chain into the push variable at a
   new member, converted to 64 bits when the index is, multiplied by G / S of the chain's variable). The push block's
   struct must be the input's with u32 members appended. What is left must be the input, instruction for instruction,
   except that the push block's struct, pointer type and variable may have moved within the global section. */
typedef struct { const uint32_t *i; uint32_t wc; } insn;
static size_t insns(const uint32_t *w, size_t nw, insn *out)
{
    size_t n = 0;
    for (size_t at = 5; at < nw;) { const uint32_t wc = w[at] >> 16; if (!wc || at + wc > nw) return 0; out[n++] = (insn){ w + at, wc }; at += wc; }
    return n;
}
static uint32_t result_id(const uint32_t *i)
{
    const uint32_t op = i[0] & 0xffffu, wc = i[0] >> 16;
    if (op >= 19 && op <= 39) return wc >= 2 ? i[1] : 0;
    if (op == 248) return i[1];
    if (op == 59 || op == 43 || op == 61 || op == 65 || op == 66 || op == 113 || op == 128 || op == 132) return wc >= 3 ? i[2] : 0;
    return 0;
}
static const uint32_t *find_def(const insn *v, size_t n, uint32_t id)
{
    for (size_t k = 0; k < n; k++) if (result_id(v[k].i) == id) return v[k].i;
    return NULL;
}
static int check(const uint32_t *in, size_t nin, const uint32_t *out, size_t nout, unsigned *nchains, char *why, size_t wn)
{
    insn *vi = calloc(nin, sizeof *vi), *vo = calloc(nout, sizeof *vo);
    int bad = 1; unsigned chains = 0;
    const size_t ni = vi ? insns(in, nin, vi) : 0, no = vo ? insns(out, nout, vo) : 0;
    const uint32_t bound = in[3];
    uint32_t push_var = 0, push_ptr = 0, push_block = 0, members = 0;
    if (!ni || !no) { snprintf(why, wn, "unparsable"); goto out; }
    for (size_t k = 0; k < ni; k++) if ((vi[k].i[0] & 0xffffu) == 59 && vi[k].i[3] == 9) { push_var = vi[k].i[2]; push_ptr = vi[k].i[1]; }
    { const uint32_t *pp = find_def(vi, ni, push_ptr); if (!pp) { snprintf(why, wn, "no push pointer"); goto out; } push_block = pp[3]; }
    { const uint32_t *pb = find_def(vi, ni, push_block); if (!pb) { snprintf(why, wn, "no push block"); goto out; } members = (pb[0] >> 16) - 2; }
    /* the output, the pass's instructions taken out */
    size_t a = 0;
    for (size_t k = 0; k < no; k++) {
        const uint32_t *o = vo[k].i, op = o[0] & 0xffffu, wc = vo[k].wc;
        if (op == 72 && o[1] == push_block && o[2] >= members) continue;   /* a new member's offset */
        if (op == 30 && o[1] == push_block) {   /* the block: the input's members, then u32 ones */
            const uint32_t *pb = find_def(vi, ni, push_block), *u = NULL;
            if (wc <= pb[0] >> 16 || memcmp(pb + 1, o + 1, ((pb[0] >> 16) - 1) * 4)) { snprintf(why, wn, "push block's old members changed"); goto out; }
            for (uint32_t m = (pb[0] >> 16); m < wc; m++) { u = find_def(vo, no, o[m]); if (!u || (u[0] & 0xffffu) != 21 || u[2] != 32 || u[3]) { snprintf(why, wn, "a new push member is not u32"); goto out; } }
            continue;
        }
        if ((op == 65 || op == 66) && wc >= 6 && o[5] >= bound && o[3] != push_var) {   /* a rewritten chain */
            const uint32_t *add = find_def(vo, no, o[5]);
            if (!add || (add[0] & 0xffffu) != 128) { snprintf(why, wn, "chain %%%u: index is not an OpIAdd", o[2]); goto out; }
            /* the value added: [mul] of [conv] of a load of a new push member */
            const uint32_t *v = find_def(vo, no, add[4]); uint32_t mult = 1;
            if (v && (v[0] & 0xffffu) == 132) { const uint32_t *c = find_def(vo, no, v[4]); if (!c || (c[0] & 0xffffu) != 43) { snprintf(why, wn, "multiplier not a constant"); goto out; } mult = c[3]; v = find_def(vo, no, v[3]); }
            if (v && (v[0] & 0xffffu) == 113) v = find_def(vo, no, v[3]);
            if (!v || (v[0] & 0xffffu) != 61) { snprintf(why, wn, "chain %%%u: the added value is not a load", o[2]); goto out; }
            const uint32_t *pc = find_def(vo, no, v[3]), *mc = pc ? find_def(vo, no, pc[4]) : NULL;
            if (!pc || (pc[0] & 0xffffu) != 65 || pc[3] != push_var || !mc || (mc[0] & 0xffffu) != 43 || mc[3] < members) { snprintf(why, wn, "chain %%%u: not loaded from a new push member", o[2]); goto out; }
            /* G / S: S from the chain's variable */
            const uint32_t *var = find_def(vi, ni, o[3]), *pt = var ? find_def(vi, ni, var[1]) : NULL, *st = pt ? find_def(vi, ni, pt[3]) : NULL;
            uint32_t stride = 0;
            if (st) for (size_t q = 0; q < ni; q++) { const uint32_t *d = vi[q].i; if ((d[0] & 0xffffu) == 71 && (d[0] >> 16) == 4 && d[1] == st[2] && d[2] == 6) stride = d[3]; }
            if (!stride || mult * stride != NVMTL_BUFOFF_GRAN) { snprintf(why, wn, "chain %%%u: multiplier %u for stride %u", o[2], mult, stride); goto out; }
            /* the chain without the pass: the old index */
            if (a >= ni || vi[a].wc != wc || memcmp(vi[a].i, o, 5 * 4) || vi[a].i[5] != add[3] || (wc > 6 && memcmp(vi[a].i + 6, o + 6, (wc - 6) * 4))) {
                snprintf(why, wn, "chain %%%u does not match the input's", o[2]); goto out; }
            chains++; a++; continue;
        }
        if (result_id(o) >= bound) continue;   /* new types, constants, pointer type and per-chain values (checked above) */
        if ((op == 32 && o[1] == push_ptr) || (op == 59 && o[2] == push_var)) {   /* moved: the same words as the input's */
            const uint32_t *d = find_def(vi, ni, op == 32 ? push_ptr : push_var);
            if (!d || (d[0] >> 16) != wc || memcmp(d, o, wc * 4)) { snprintf(why, wn, "push pointer or variable changed"); goto out; }
            continue;
        }
        /* skip the input's moved block, pointer and variable */
        while (a < ni && ((((vi[a].i[0] & 0xffffu) == 30) && vi[a].i[1] == push_block) || (((vi[a].i[0] & 0xffffu) == 32) && vi[a].i[1] == push_ptr) ||
                          (((vi[a].i[0] & 0xffffu) == 59) && vi[a].i[2] == push_var))) a++;
        if (a >= ni || vi[a].wc != wc || memcmp(vi[a].i, o, wc * 4)) { snprintf(why, wn, "output instruction %zu (op %u) is not the input's", k, op); goto out; }
        a++;
    }
    while (a < ni && ((((vi[a].i[0] & 0xffffu) == 30) && vi[a].i[1] == push_block) || (((vi[a].i[0] & 0xffffu) == 32) && vi[a].i[1] == push_ptr) ||
                      (((vi[a].i[0] & 0xffffu) == 59) && vi[a].i[2] == push_var))) a++;
    if (a != ni) { snprintf(why, wn, "input instruction %zu left over", a); goto out; }
    *nchains = chains; bad = 0;
out:
    free(vi); free(vo);
    return bad;
}

int main(int argc, char **argv)
{
    int k = 1, verbose = 0; lower_ex_fn lower = NULL; free_fn xfree = NULL;
    static const char *const rname[] = { "module", "none", "shape", "length", "call", "chain", "other", "room" };
    unsigned rcount[8] = {0};
    uint32_t pcflags = NVMTL_PC_MERGE | NVMTL_PC_SET0 | NVMTL_PC_EXACT;
    for (; k < argc; k++) {
        if (!strcmp(argv[k], "-v")) verbose = 1;
        else if (!strcmp(argv[k], "--old-pc")) pcflags &= ~NVMTL_PC_EXACT;
        else break;
    }
    if (k + 1 < argc && !strcmp(argv[k], "--lower")) {
        void *h = dlopen(argv[k + 1], RTLD_NOW | RTLD_LOCAL);
        if (!h) { fprintf(stderr, "dlopen %s: %s\n", argv[k + 1], dlerror()); return 2; }
        lower = (lower_ex_fn)dlsym(h, "nvmtl_lower_buffer_addresses_ex"); xfree = (free_fn)dlsym(h, "nvmtl_translate_free");
        if (!lower || !xfree) { fprintf(stderr, "no nvmtl_lower_buffer_addresses_ex in %s\n", argv[k + 1]); return 2; }
        k += 2;
    }
    if (k + 1 >= argc) { fprintf(stderr, "usage: %s [--lower dylib] <outdir> <kernel.spv>...\n", argv[0]); return 2; }
    const char *dir = argv[k++];
    unsigned files = 0, converted = 0, bindings = 0, chains = 0, failed = 0, pcs = 0, kernels = 0;
    for (; k < argc; k++) {
        size_t n = 0; uint32_t *spv = slurp(argv[k], &n);
        if (!spv) { fprintf(stderr, "%s: unreadable\n", argv[k]); failed++; continue; }
        files++;
        int model = -1;   /* the entry point's execution model */
        for (size_t at = 5; at < n / 4;) { const uint32_t wc = spv[at] >> 16; if (!wc) break; if ((spv[at] & 0xffffu) == 15) { model = (int)spv[at + 1]; break; } at += wc; }
        if (model != 5) { free(spv); continue; }
        kernels++;
        if (lower) {
            uint8_t *o = NULL; size_t on = 0; uint32_t info[4] = {0}; char err[512] = {0};
            if (lower((const uint8_t *)spv, n, 2u | (2u << 8), &o, &on, info, err, sizeof err)) { free(spv); continue; }   /* refused: kept as-is by the plugin, no pass */
            free(spv); spv = malloc(on); memcpy(spv, o, on); n = on; xfree(o, on);
        }
        nvmtl_pc_table pc = {0}; size_t pn = 0;
        uint32_t *p = nvmtl_spirv_table_pc_ex(spv, n, 5, NVMTL_PC_TABLE_CS_OFFSET, NVMTL_PC_TABLE_MAX, pcflags, &pc, &pn);
        if (p) { free(spv); spv = p; n = pn; pcs++; } else pc.count = 0;
        spit(dir, argv[k], "pc", spv, n);
        nvmtl_bufoff m = {0}; size_t bn = 0;
        uint32_t *bo = nvmtl_spirv_bufoff_pc(spv, n, NVMTL_PC_TABLE_CS_OFFSET + 8 * pc.count, 256, 32, &m, &bn);
        for (unsigned r = 0; r < 8; r++) if (m.why >> r & 1u) rcount[r]++;
        if (verbose && m.why) {
            printf("keep %s: bindings %#x of %#x kept:", argv[k], m.have & ~m.mask, m.have);
            for (unsigned r = 0; r < 8; r++) if (m.why >> r & 1u) printf(" %s", rname[r]);
            if (m.op) printf(" (op %u)", m.op);
            printf("\n");
        }
        if (bo) {
            unsigned c = 0; char why[256] = {0};
            if (check(spv, n / 4, bo, bn / 4, &c, why, sizeof why)) { printf("FAIL %s: %s\n", argv[k], why); failed++; }
            else if (!c) { printf("FAIL %s: converted bindings %#x but no chain rewritten\n", argv[k], m.mask); failed++; }
            else { converted++; bindings += m.count; chains += c; printf("ok   %s: bindings %#x, %u chains, table-pc %u\n", argv[k], m.mask, c, pc.count); }
            spit(dir, argv[k], "bo", bo, bn);
            free(bo);
        }
        free(spv);
    }
    printf("bufoff_check: %u files, %u kernels, %u with table-pc, %u converted (%u bindings, %u chains): %u FAILED\n",
           files, kernels, pcs, converted, bindings, chains, failed);
    printf("bufoff_check: kernels with a binding or the module left as it was, by reason:");
    for (unsigned r = 0; r < 8; r++) printf(" %s %u", rname[r], rcount[r]);
    printf("\n");
    return failed ? 1 : 0;
}
