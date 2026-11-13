/* SPDX-License-Identifier: MIT */
/* nvmtl_desc_stats.h: NVMTL_DESC_STATS=1, what the descriptor work of each model costs (DESCRIPTOR-MODEL.md section 13).
   The same counters in both models, so a night run can compare them per draw: how many draws and dispatches were bound,
   the host time spent binding them (nvmtl_vk_cmd_bind_set and the compute binds: descriptor sets or descriptor-buffer
   regions, the address table, the pushed table entries, and the Vulkan calls they make), and what that work consisted of.

   Off (the default), every site is one test of a cached int. On, a counter is a plain add to this thread's block: no
   locked instruction, so the counting itself does not pay the write-combining drain an atomic pays after a draw
   (DESCRIPTOR-MODEL.md section 12). The blocks of all threads are summed when a line is written: "DESCSTAT" every
   5 s of command-buffer completions (the window since the last line) and "DESCSTAT-TOTAL" when the process exits.
   The format (nvmtl_dstat_format) is pure, so the host model (user/tests/metal/dbuf_model.c) checks it. */
#ifndef NVMTL_DESC_STATS_H
#define NVMTL_DESC_STATS_H
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    NVMTL_DS_DRAW,       /* graphics binds (one per draw that reached nvmtl_vk_cmd_bind_set) */
    NVMTL_DS_DISP,       /* compute binds (one per dispatch, any path) */
    NVMTL_DS_TICKS,      /* host time in those binds, mach_absolute_time ticks */
    NVMTL_DS_NOBIND,     /* binds that failed: their draw or dispatch was not recorded */
    /* classic model */
    NVMTL_DS_SETNEW,     /* descriptor sets built */
    NVMTL_DS_SETREUSE,   /* the last set bound again (also with a new table offset) */
    NVMTL_DS_UPDATES,    /* vkUpdateDescriptorSets calls */
    NVMTL_DS_WRITES,     /* their descriptor writes */
    NVMTL_DS_COPIES,     /* and copies */
    NVMTL_DS_ALLOCS,     /* vkAllocateDescriptorSets calls */
    NVMTL_DS_BINDSETS,   /* vkCmdBindDescriptorSets calls */
    /* descriptor-buffer model */
    NVMTL_DS_REGION,     /* stage regions written */
    NVMTL_DS_REGREUSE,   /* a stage pointed at a region written before (current or recent) */
    NVMTL_DS_KEEP,       /* slot changes that left the current region valid (nvmtl_dbuf_reuse.h "keep") */
    NVMTL_DS_MADE,       /* slot descriptors made (vkGetDescriptorEXT) */
    NVMTL_DS_BYTES,      /* descriptor bytes written into regions */
    NVMTL_DS_OFFSETS,    /* vkCmdSetDescriptorBufferOffsetsEXT calls */
    NVMTL_DS_BUFBINDS,   /* vkCmdBindDescriptorBuffersEXT calls with a changed page address (13 Nov) */
    /* both */
    NVMTL_DS_TABLES,     /* address-table snapshots */
    NVMTL_DS_PUSHES,     /* vkCmdPushConstants calls (table-pc entries, dispatch values, sample positions) */
    NVMTL_DS_LEGACY,     /* classic-set calls made while the buffer model is on (each one an ANV binding-mode switch) */
    NVMTL_DS_N
};
typedef struct { uint64_t v[NVMTL_DS_N]; } nvmtl_dstat;

/* d = a - b, counter by counter */
static inline void nvmtl_dstat_sub(nvmtl_dstat *d, const nvmtl_dstat *a, const nvmtl_dstat *b)
{
    for (int k = 0; k < NVMTL_DS_N; k++) d->v[k] = a->v[k] - b->v[k];
}
/* one line: model "classic" or "buffer", what (e.g. "5.0 s" or "total 12.3 s"), d the counts, tick_ns the length of
   one tick in ns. Returns the length written (snprintf). */
static inline int nvmtl_dstat_format(char *b, size_t n, const char *model, const char *what, const nvmtl_dstat *d, double tick_ns)
{
    const uint64_t *v = d->v;
    const uint64_t binds = v[NVMTL_DS_DRAW] + v[NVMTL_DS_DISP];
    const double ns = (double)v[NVMTL_DS_TICKS] * tick_ns;
    const double per = binds ? ns / (double)binds : 0.0;
    const double pd = binds ? 1.0 / (double)binds : 0.0;
    return snprintf(b, n,
        "%s %s: %llu draws + %llu dispatches bound, %.0f ns each (%.1f ms); "
        "sets %llu new + %llu reused, %llu updates (%llu writes, %llu copies), %llu allocs, %llu binds; "
        "regions %llu new + %llu reused + %llu kept, %llu descriptors made, %.1f KB, %llu offset calls, %llu buffer binds; "
        "%llu tables, %llu pushes, %llu legacy calls, %llu not recorded; per bind: %.2f sets, %.2f regions, %.2f tables, %.0f B",
        model, what, (unsigned long long)v[NVMTL_DS_DRAW], (unsigned long long)v[NVMTL_DS_DISP], per, ns / 1e6,
        (unsigned long long)v[NVMTL_DS_SETNEW], (unsigned long long)v[NVMTL_DS_SETREUSE], (unsigned long long)v[NVMTL_DS_UPDATES],
        (unsigned long long)v[NVMTL_DS_WRITES], (unsigned long long)v[NVMTL_DS_COPIES], (unsigned long long)v[NVMTL_DS_ALLOCS],
        (unsigned long long)v[NVMTL_DS_BINDSETS],
        (unsigned long long)v[NVMTL_DS_REGION], (unsigned long long)v[NVMTL_DS_REGREUSE], (unsigned long long)v[NVMTL_DS_KEEP],
        (unsigned long long)v[NVMTL_DS_MADE], (double)v[NVMTL_DS_BYTES] / 1024.0, (unsigned long long)v[NVMTL_DS_OFFSETS],
        (unsigned long long)v[NVMTL_DS_BUFBINDS],
        (unsigned long long)v[NVMTL_DS_TABLES], (unsigned long long)v[NVMTL_DS_PUSHES], (unsigned long long)v[NVMTL_DS_LEGACY],
        (unsigned long long)v[NVMTL_DS_NOBIND],
        (double)v[NVMTL_DS_SETNEW] * pd, (double)v[NVMTL_DS_REGION] * pd, (double)v[NVMTL_DS_TABLES] * pd, (double)v[NVMTL_DS_BYTES] * pd);
}
#endif
