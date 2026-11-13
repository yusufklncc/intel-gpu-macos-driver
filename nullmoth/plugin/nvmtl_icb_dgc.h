// SPDX-License-Identifier: MIT
/* NVMTL_ICB_DGC=1: GPU-encoded indirect command buffers executed through VK_EXT_device_generated_commands
   (docs/design/DESIGN-icb-dgc.md, nvmtl_icb_dgc.c, the record layout in nvmtl_icb_layout.h). With the knob off none of
   this is called and nvmtl_vk_icb_dgc_probe adds nothing to the device. */
#ifndef NVMTL_ICB_DGC_H
#define NVMTL_ICB_DGC_H
#include <stdint.h>
#include "nvmtl_icb_layout.h"

/* What the execute of one pipeline can take from the records. ok: an indirect commands layout exists (DGC can run
   this pipeline); vmask / smask: the vertex and fragment (or kernel) buffer indices a shader may set, the bits the
   CPU writes into words 21 / 22; vtok: the vertex bindings fed by vertex buffer tokens (0: the CPU binds them). */
typedef struct nvmtl_dgc_plan nvmtl_dgc_plan;
struct nvmtl_dgc_plan_info { int ok; uint32_t vmask, smask, vtok; };
/* scratch (preprocess) buffers taken for one command buffer, given back when it completes */
typedef struct nvmtl_dgc_arena nvmtl_dgc_arena;

/* nvmtl_vk.c, device creation: before vkCreateDevice (adds the extensions to exts and the features to *chain, with the
   knob only) and after it (the entry points) */
void nvmtl_vk_icb_dgc_probe(const char **exts, uint32_t *next, uint32_t cap, void **chain);
void nvmtl_vk_icb_dgc_device_ready(void);
/* 1 when the knob is on and the device has VK_EXT_device_generated_commands enabled (after vkCreateDevice) */
int  nvmtl_vk_icb_dgc_ready(void);
/* the identity index buffer (uint32 0..n-1) the records point plain draws at; 0 when it could not be made */
uint64_t nvmtl_vk_icb_dgc_iota(uint32_t *n);
/* the device address unbound buffer entries hold (the plugin's dummy buffer) */
uint64_t nvmtl_vk_icb_dgc_dummy(void);
/* the plan of the graphics pipeline p as bound in c (buffers: the ICB does not inherit buffers, so the records hold them;
   alt: the draw token reads the other-topology draw at word 235 instead of word 2, nvmtl_icb_layout.h) */
const nvmtl_dgc_plan *nvmtl_vk_icb_dgc_plan_gfx(nvk_cmdbuf *c, const nvk_pipeline *p, int buffers, int alt, struct nvmtl_dgc_plan_info *info);
/* the plan of compute pipeline p (kernel buffers through its pushed address table); NULL when DGC cannot run it */
const nvmtl_dgc_plan *nvmtl_vk_icb_dgc_plan_cs(const nvk_pipeline *p, int buffers, struct nvmtl_dgc_plan_info *info);
/* n sequences (draws) from the records at rec + off, stride bytes apart, with the state bound in c. 0 ok, -1 refused. */
int  nvmtl_vk_cmd_icb_dgc_draw(nvk_cmdbuf *c, const nvmtl_dgc_plan *pl, nvk_buffer *rec, size_t off, uint32_t n, uint32_t stride,
                               nvmtl_dgc_arena **arena);
/* one dispatch from the record at rec + off with threadgroup tg (the binding of nvmtl_vk_cmd_dispatch_indirect_tg). */
int  nvmtl_vk_cmd_icb_dgc_dispatch(nvk_cmdbuf *c, nvk_pipeline *p, const nvmtl_dgc_plan *pl, nvk_buffer *rec, size_t off,
                                   const uint32_t tg[3], nvmtl_dgc_arena **arena);
/* gives the arena's scratch buffers back (call when the command buffer has completed) */
void nvmtl_vk_icb_dgc_arena_release(nvmtl_dgc_arena *a);
#endif
