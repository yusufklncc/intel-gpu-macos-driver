// SPDX-License-Identifier: MIT
/* GPU-encoded indirect command buffers: the slot record layout shared with the translator
   (translator/translator/src/native/render_command.rs, which lowers air.draw_primitives_render_command,
   air.draw_indexed_primitives_render_command.p1i16/.p1i32 and air.reset_render_command to stores into it).

   An MTLIndirectCommandBuffer whose gpuResourceID is taken (an argument encoder's setIndirectCommandBuffer:atIndex:
   or the app itself) gets a record buffer (VRAM through the BAR when it can, else system RAM; the CPU keeps its own copy
   of what it wrote and never reads the buffer back): one 64-byte record per slot. Its device address is the
   gpuResourceID, which is what a shader's `command_buffer` holds. The CPU writes a record when the app encodes a draw
   through indirectRenderCommandAtIndex:, the GPU when a shader encodes one; executeCommandsInBuffer:withRange: then
   draws every slot from its record with vkCmdDrawIndexedIndirect (a slot with an index buffer) or vkCmdDrawIndirect,
   so the last writer wins on either side, as on Apple's GPUs.

   Words (uint32):
     0      kind: NVMTL_ICB_EMPTY / _DRAW / _INDEXED / _REFUSED                                      CPU + GPU
     1      Metal primitive type of the last draw written                                            CPU + GPU
     2..6   VkDrawIndexedIndirectCommand                                                             CPU + GPU
     7..10  VkDrawIndirectCommand                                                                    CPU + GPU
     11     primitive type the execute draws the slot with (NVMTL_ICB_NO_PRIM: none chosen yet)       CPU
     12,13  device address bound as the slot's index buffer (0: none)                                CPU
     14     bytes from that address to the end of the index buffer                                   CPU
     15     log2 of the index size (1 uint16, 2 uint32, 0 none)                                      CPU
   A write zeroes the counts of the form it does not use. The execute draws a run of consecutive slots that replay the
   same state as one vkCmdDraw*Indirect with drawCount = the run's length and stride 64 (multiDrawIndirect; ~0.03 us per
   slot, as direct draws) and every other slot alone (~2 us): the indexed form when word 15 is non-zero, else the plain
   form (both, until 11 Nov 2026: Xe2 ANV loads each indirect parameter with its
   own MI_LOAD_REGISTER_MEM, ~2.2 us per indirect draw from VRAM and ~5 us from system RAM, user/tests/metal/
   mtl_icb_cost.m). Topology and index buffer are bound by the CPU (core Vulkan takes neither from GPU memory), so a GPU
   draw with another primitive type than word 11, another index type than word 15, an index pointer outside
   [word 12/13, + word 14], or a plain draw into a slot with an index buffer is written with zero counts and kind
   NVMTL_ICB_REFUSED. */
#ifndef NVMTL_ICB_LAYOUT_H
#define NVMTL_ICB_LAYOUT_H
#include <stdint.h>

enum {
    NVMTL_ICB_RECORD_BYTES = 64,
    NVMTL_ICB_W_KIND = 0, NVMTL_ICB_W_PRIM = 1, NVMTL_ICB_W_IDX = 2, NVMTL_ICB_W_DRAW = 7,
    NVMTL_ICB_W_EXEC_PRIM = 11, NVMTL_ICB_W_IB_LO = 12, NVMTL_ICB_W_IB_HI = 13, NVMTL_ICB_W_IB_LIMIT = 14, NVMTL_ICB_W_IB_SHIFT = 15,
    NVMTL_ICB_EMPTY = 0, NVMTL_ICB_DRAW = 1, NVMTL_ICB_INDEXED = 2, NVMTL_ICB_REFUSED = 3,
};
#define NVMTL_ICB_NO_PRIM 0xffffffffu

typedef struct {
    uint32_t kind, prim;
    uint32_t idx[5];    /* indexCount, instanceCount, firstIndex, vertexOffset, firstInstance */
    uint32_t draw[4];   /* vertexCount, instanceCount, firstVertex, firstInstance */
    uint32_t execPrim, ibLo, ibHi, ibLimit, ibShift;
} nvmtl_icb_record;
_Static_assert(sizeof(nvmtl_icb_record) == NVMTL_ICB_RECORD_BYTES, "the translator addresses a slot as 16 words");
_Static_assert(__builtin_offsetof(nvmtl_icb_record, idx) == NVMTL_ICB_W_IDX * 4, "indexed draw at word 2");
_Static_assert(__builtin_offsetof(nvmtl_icb_record, draw) == NVMTL_ICB_W_DRAW * 4, "plain draw at word 7");
_Static_assert(__builtin_offsetof(nvmtl_icb_record, execPrim) == NVMTL_ICB_W_EXEC_PRIM * 4, "execute topology at word 11");
_Static_assert(__builtin_offsetof(nvmtl_icb_record, ibShift) == NVMTL_ICB_W_IB_SHIFT * 4, "index shift at word 15");

/* The pipeline's topology class gives the primitive type of a slot that has no CPU-encoded draw:
   MTLPrimitiveTopologyClass Point 1 -> point (0), Line 2 -> line (1), Triangle 3 / Unspecified 0 -> triangle (3). */
static inline uint32_t nvmtl_icb_prim_for_class(unsigned long cls) { return cls == 1 ? 0u : cls == 2 ? 1u : 3u; }

/* NVMTL_ICB_DGC=1 (docs/design/DESIGN-icb-dgc.md; the translator's render_command/dgc.rs keeps the same words): every
   GPU-encodable ICB gets 1024-byte records. Words 0..15 are the record above; the rest hold what
   VK_EXT_device_generated_commands reads per sequence (index buffer token, vertex buffer tokens, the buffer addresses its
   push-constant tokens copy), the compute words, and CPU-written words that tell a shader what the execute can express.
   The translator lowers the state commands (set_vertex_buffer, set_fragment_buffer, set_kernel_buffer, set_pipeline_state,
   barriers, concurrent_dispatch_*, compute reset) only with the knob; both sides read the same variable. */
enum {
    NVMTL_ICB_DGC_RECORD_BYTES = 1024,
    NVMTL_ICB_W_IBT = 16,        /* VkBindIndexBufferIndirectCommandEXT                                   CPU + GPU */
    NVMTL_ICB_W_MODE = 20,       /* bit 0: this ICB's render executes run through DGC                     CPU */
    NVMTL_ICB_W_VMASK = 21,      /* vertex buffer indices a shader may set                                CPU */
    NVMTL_ICB_W_SMASK = 22,      /* fragment (render) / kernel (compute) buffer indices a shader may set  CPU */
    NVMTL_ICB_W_STICKY = 23,     /* a state command the execute cannot express was encoded here           GPU */
    NVMTL_ICB_W_EXEC_PIPE = 24,  /* resource ID of the pipeline the execute binds (0 not known)           CPU */
    NVMTL_ICB_W_GPU_PIPE = 26,   /* the resource ID a shader set                                          GPU */
    NVMTL_ICB_W_IOTA = 28, NVMTL_ICB_W_IOTA_N = 30,   /* the identity index buffer (uint32 0..n-1)       CPU */
    NVMTL_ICB_W_BARRIER = 31,    /* 1 after set_barrier                                                   GPU */
    NVMTL_ICB_W_DISPATCH = 32,   /* VkDispatchIndirectCommand                                             CPU + GPU */
    NVMTL_ICB_W_TG = 35,         /* threads per threadgroup a shader asked for                            GPU */
    NVMTL_ICB_W_EXEC_TG = 38,    /* threads per threadgroup the execute dispatches with (0 none yet)      CPU */
    NVMTL_ICB_W_VB = 48,         /* vertex buffer m: VkBindVertexBufferIndirectCommandEXT at 48 + 4m      CPU + GPU */
    NVMTL_ICB_W_SB = 172,        /* fragment / kernel buffer m: its address at 172 + 2m                   CPU + GPU */
    NVMTL_ICB_W_ALT_PRIM = 234,  /* the other topology of the class a slot without a CPU draw may draw  CPU */
    NVMTL_ICB_W_ALT_IDX = 235,   /* VkDrawIndexedIndirectCommand of a draw of that topology               GPU */
    NVMTL_ICB_MAX_BUFFERS = 31,
    NVMTL_ICB_DISPATCH = 4,      /* kind: a dispatch                                                      */
    NVMTL_ICB_MODE_DGC_DRAW = 1,
};
typedef struct {
    nvmtl_icb_record head;                          /* 0..15 */
    uint32_t ibt[4];                                /* 16..19: address lo, hi, size, VkIndexType */
    uint32_t mode, vmask, smask, sticky;            /* 20..23 */
    uint32_t execPipe[2], gpuPipe[2];               /* 24..27 */
    uint32_t iota[2], iotaN, barrier;               /* 28..31 */
    uint32_t dispatch[3], tg[3], execTg[3];         /* 32..40 */
    uint32_t scratch[7];                            /* 41..47 */
    uint32_t vb[NVMTL_ICB_MAX_BUFFERS][4];          /* 48..171: address lo, hi, size, stride */
    uint32_t sb[NVMTL_ICB_MAX_BUFFERS][2];          /* 172..233 */
    uint32_t altPrim, altIdx[5];                    /* 234..239 */
    uint32_t spare[16];
} nvmtl_icb_dgc_record;
_Static_assert(sizeof(nvmtl_icb_dgc_record) == NVMTL_ICB_DGC_RECORD_BYTES, "the translator addresses a slot as 256 words with the knob");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, ibt) == NVMTL_ICB_W_IBT * 4, "index buffer token at word 16");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, sticky) == NVMTL_ICB_W_STICKY * 4, "sticky refusal at word 23");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, iotaN) == NVMTL_ICB_W_IOTA_N * 4, "identity count at word 30");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, dispatch) == NVMTL_ICB_W_DISPATCH * 4, "dispatch at word 32");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, execTg) == NVMTL_ICB_W_EXEC_TG * 4, "execute threadgroup at word 38");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, vb) == NVMTL_ICB_W_VB * 4, "vertex buffers at word 48");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, sb) == NVMTL_ICB_W_SB * 4, "stage buffers at word 172");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, altPrim) == NVMTL_ICB_W_ALT_PRIM * 4, "the other topology at word 234");
_Static_assert(__builtin_offsetof(nvmtl_icb_dgc_record, altIdx) == NVMTL_ICB_W_ALT_IDX * 4, "its draw at word 235");

/* A slot with no CPU-encoded draw executes with its pipeline's class as a list (word 11); a shader may draw the class's
   strip instead (WebKit's vsICBIndirect* encode a draw of the render bundle's own topology into such slots). In DGC mode
   the translator accepts the type in word 234 too and puts that draw's counts at words 235..239 (the main area's are
   zero). The execute draws the slot with the strip once a completed execute has shown one there (nvmtlExecuteDGC; with
   NVMTL_ICB_DGC_STRIPS=1 both topologies every time): core Vulkan and the extension take no topology from GPU memory. */
static inline uint32_t nvmtl_icb_strip_of(uint32_t prim) { return prim == 1 ? 2u : prim == 3 ? 4u : NVMTL_ICB_NO_PRIM; }
/* NVMTL_ICB_DGC=1 (exactly 1), read once. With it off nothing above is used and every record is 64 bytes, as before. */
#include <stdlib.h>
#include <string.h>
static inline int nvmtl_icb_dgc_knob(void)
{
    static int on = -1;
    if (on < 0) { const char *e = getenv("NVMTL_ICB_DGC"); on = e && !strcmp(e, "1"); }
    return on;
}
#endif
