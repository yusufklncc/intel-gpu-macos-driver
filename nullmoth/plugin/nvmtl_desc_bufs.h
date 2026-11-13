// SPDX-License-Identifier: MIT
/* nvmtl_desc_bufs.h: which buffer indices a lowered module still reads through descriptors (NVMTL_ICB_DGC=1, the plan's
   offer of buffers a shader may set; docs/design/DESIGN-icb-dgc.md section 10). Header-only: nvmtl_vk.c and the host
   check user/tests/metal/icb_lower_check.m use the same scan. */
#ifndef NVMTL_DESC_BUFS_H
#define NVMTL_DESC_BUFS_H
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* NVMTL_ICB_DGC=1: the buffer indices m (< 32) a module reads through a buffer descriptor of its own (a StorageBuffer or
   Uniform variable at binding m outside the heap set 2 and the framebuffer-fetch set 3): the address lowering kept them
   (an index from loaded data, wrapper buffer_addresses kept_roots) or refused the whole module. A DGC sequence cannot set
   a descriptor, so a buffer a shader sets by address never reaches such an index: the plan does not offer it
   (nvmtl_dgc_takes) and a shader's set_*_buffer to it refuses the slot instead of the kernel writing the CPU's buffer
   (mtl_gpu_icb2 C0..C2 before 13 Nov). A malformed module gives 0xffffffff (nothing offered). */
static inline uint32_t nvmtl_spirv_desc_bufs(const void *p, size_t n)
{
    const uint32_t *w = (const uint32_t *)p; size_t nw = n / 4;
    if (!p || nw < 5 || w[0] != 0x07230203u) return 0xffffffffu;
    const uint32_t bound = w[3];
    if (!bound || bound > (1u << 24)) return 0xffffffffu;
    uint8_t *sc = (uint8_t *)calloc(bound, 1);   // 1 = a StorageBuffer/Uniform variable
    uint32_t *bind = (uint32_t *)malloc(bound * sizeof *bind), *set = (uint32_t *)calloc(bound, sizeof *set);
    uint32_t mask = 0;
    if (!sc || !bind || !set) { free(sc); free(bind); free(set); return 0xffffffffu; }
    memset(bind, 0xff, bound * sizeof *bind);
    for (size_t i = 5; i < nw; ) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffffu;
        if (!wc || i + wc > nw) { mask = 0xffffffffu; goto out; }
        if (op == 71 && wc == 4 && w[i + 1] < bound) {
            if (w[i + 2] == 33) bind[w[i + 1]] = w[i + 3];
            else if (w[i + 2] == 34) set[w[i + 1]] = w[i + 3];
        } else if (op == 59 && wc >= 4 && w[i + 2] < bound && (w[i + 3] == 12 || w[i + 3] == 2)) sc[w[i + 2]] = 1;
        i += wc;
    }
    for (uint32_t v = 0; v < bound; v++)
        if (sc[v] && bind[v] < 32 && set[v] != 2 && set[v] != 3) mask |= 1u << bind[v];
out:
    free(sc); free(bind); free(set);
    return mask;
}
#endif
