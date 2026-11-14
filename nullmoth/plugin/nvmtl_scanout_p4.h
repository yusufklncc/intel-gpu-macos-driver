/* SPDX-License-Identifier: MIT
 * gpuperf phase 4: what happens to a WindowServer composite that has a scan-out (or readback) binding, decided per
 * frame. Pure C, no plugin state: NVMTLObjects.m / NVMTLEventSync.m use it, user/tests/plugin/scanout_p4_unit.c tests it
 * on the host.
 *
 * A bound composite's frame is always copied into its scan-out buffer (VRAM to VRAM). Its IOSurface's system-memory pages
 * (what a CPU reader of the surface sees) are then handled by the readback mode:
 *   p4    (default) phase 4: no copy into the pages. At publication the plugin marks the binding newer (NMXAccel selector
 *         2); the family's valid bits then say the device copy is current, so a CPU lock of the surface (a screenshot,
 *         a capture), a frame the kernel copies instead of flipping, and an unbind page the frame off from the scan-out
 *         buffer on the copy engine (nmxScanPageoff). A mark the kernel refuses reads that frame back after all.
 *   full  every frame is also read back into the pages over PCIe (before 2026-11-14 the default; 12.8 ms of GPU per
 *         frame on a B580 at 3440x1440, ~2.2 ms at 2560x1440). NVMTL_SCANOUT_READBACK=1, or the root-owned diag flag
 *         /tmp/nvmtl-scanout-readback-full.
 *   off   no copy into the pages and no mark: CPU readers of the composite see whatever frame the pages last got. A
 *         measurement mode only (td016's default). NVMTL_SCANOUT_READBACK=0, or /tmp/nvmtl-no-scanout-readback.
 * Phase 4 needs the pages to have the scan-out buffer's layout, since the kernel's page-off copies the buffer 1:1 from
 * offset 0: the same row pitch, the plane at the start of the allocation, and an allocation that holds the frame rounded
 * up to whole pages (the page-off copies whole pages and stops at the allocation's last whole page). A frame that does not
 * qualify is read back, and the reason is logged once per binding.
 *
 * WAS (until 2026-11-14): phase 4 also required debug.intelxe.surfvram >= 2 (the VRAM-shared IOSurface switch, 0 in the
 * kext unless the boot-arg intelxe_surfvram=2 is set) and the readback switch on. Phase 4 uses nothing of the VRAM-shared
 * surfaces, so on a machine without that boot-arg it never engaged and every frame was read back; with the readback
 * switched off (td016) it could not engage anywhere. */
#ifndef NVMTL_SCANOUT_P4_H
#define NVMTL_SCANOUT_P4_H
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef enum { NVMTL_SORB_P4 = 0, NVMTL_SORB_FULL = 1, NVMTL_SORB_OFF = 2 } nvmtl_sorb_mode;

static inline const char *nvmtl_sorb_name(nvmtl_sorb_mode m)
{
    return m == NVMTL_SORB_FULL ? "full readback" : m == NVMTL_SORB_OFF ? "readback OFF, no marks (CPU readers may see an older frame)"
                                                                        : "phase 4 (no readback; CPU locks page the frame off)";
}
/* NVMTL_SCANOUT_READBACK: unset, empty, "p4", "4" or "auto" = p4; "1", "full", "on" = full; "0", "off" = off. Anything
   else is p4 (the safe default that does not cost a readback). */
static inline nvmtl_sorb_mode nvmtl_sorb_parse(const char *e)
{
    if (!e || !*e) return NVMTL_SORB_P4;
    if (!strcmp(e, "1") || !strcmp(e, "full") || !strcmp(e, "on")) return NVMTL_SORB_FULL;
    if (!strcmp(e, "0") || !strcmp(e, "off")) return NVMTL_SORB_OFF;
    return NVMTL_SORB_P4;
}
/* The run-time diag flags override the environment: off wins over full (a measurement switch is never made safer by
   accident: both set means the measurer asked for the cheapest). */
static inline nvmtl_sorb_mode nvmtl_sorb_effective(nvmtl_sorb_mode env, int flag_full, int flag_off)
{
    return flag_off ? NVMTL_SORB_OFF : flag_full ? NVMTL_SORB_FULL : env;
}

/* Why a bound composite's frame is (or is not) handled by phase 4. 0 is "not decided yet" (a binding's first frame). */
enum {
    NVMTL_P4_YES = 1,        /* phase 4: scan-out buffer only, the binding marked newer at publication */
    NVMTL_P4_UNBOUND,        /* no binding: read back (the only copy of the frame for other processes) */
    NVMTL_P4_MODE_FULL,      /* readback mode full */
    NVMTL_P4_MODE_OFF,       /* readback mode off: scan-out buffer only, no mark */
    NVMTL_P4_PITCH,          /* the pages' row pitch differs from the scan-out buffer's */
    NVMTL_P4_PLANE_OFFSET,   /* the plane does not start at the allocation's start */
    NVMTL_P4_ALLOC_SHORT,    /* the allocation is shorter than the frame rounded up to pages */
    NVMTL_P4_REFUSED,        /* the kernel refused a recent mark for this binding (held off for a while) */
    NVMTL_P4_NWHY
};
static inline const char *nvmtl_p4_why_name(int w)
{
    switch (w) {
    case NVMTL_P4_YES: return "engaged";
    case NVMTL_P4_UNBOUND: return "not bound";
    case NVMTL_P4_MODE_FULL: return "readback mode full";
    case NVMTL_P4_MODE_OFF: return "readback mode off";
    case NVMTL_P4_PITCH: return "row pitch differs from the scan-out buffer's";
    case NVMTL_P4_PLANE_OFFSET: return "plane not at the start of the allocation";
    case NVMTL_P4_ALLOC_SHORT: return "allocation shorter than the page-rounded frame";
    case NVMTL_P4_REFUSED: return "kernel refused a recent mark";
    default: return "?";
    }
}
typedef struct {
    int p4;        /* skip the readback and mark the binding newer at publication */
    int readback;  /* copy the frame into the IOSurface's pages */
    int why;       /* NVMTL_P4_* */
} nvmtl_p4_decision;

static inline uint64_t nvmtl_p4_frame_pages_bytes(uint32_t pitch, uint32_t h)
{
    return ((uint64_t)pitch * h + 0xfffull) & ~0xfffull;
}
/* bound: the surface has a scan-out or readback binding (its frame goes into that buffer). pitch, h: the binding's.
   row: the plane's bytes per row; plane_off: the plane's offset from IOSurfaceGetBaseAddress; alloc: IOSurfaceGetAllocSize.
   refused: the kernel refused a mark for this binding recently. */
static inline nvmtl_p4_decision nvmtl_p4_decide(nvmtl_sorb_mode mode, int bound, uint32_t pitch, uint32_t h, size_t row,
                                                size_t plane_off, size_t alloc, int refused)
{
    nvmtl_p4_decision d = { 0, 1, NVMTL_P4_UNBOUND };
    if (!bound) return d;
    if (mode == NVMTL_SORB_FULL) { d.why = NVMTL_P4_MODE_FULL; return d; }
    if (mode == NVMTL_SORB_OFF) { d.readback = 0; d.why = NVMTL_P4_MODE_OFF; return d; }
    if (!pitch || (size_t)pitch != row) { d.why = NVMTL_P4_PITCH; return d; }
    if (plane_off) { d.why = NVMTL_P4_PLANE_OFFSET; return d; }
    if ((uint64_t)alloc < nvmtl_p4_frame_pages_bytes(pitch, h)) { d.why = NVMTL_P4_ALLOC_SHORT; return d; }
    if (refused) { d.why = NVMTL_P4_REFUSED; return d; }
    d.p4 = 1; d.readback = 0; d.why = NVMTL_P4_YES;
    return d;
}
/* What the selector-2 answer means (NMXAccel kNmxSc*; -1: no present client or an NMXAccel without selector 2). */
static inline const char *nvmtl_p4_mark_status_name(int st)
{
    switch (st) {
    case 0: return "marked";
    case 4: return "busy";
    case 7: return "not bound by this client";
    case 8: return "no family resource (a CPU lock would not page it off)";
    case 9: return "caller is not WindowServer";
    case -1: return "no present client / NMXAccel without phase 4";
    default: return "refused";
    }
}
#endif
