/*
 * NullMoth NVIDIA driver for macOS
 * Copyright (c) 2026 NullMoth Systems.
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */

#pragma once
#include <dlfcn.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <pthread.h>
/* An executable companion of this plugin (the Vulkan driver and loader, the AIR translator): from the plugin's own bundle,
   <bundle>/Contents/Frameworks/nvmtl/<name>, when it is there; otherwise /Library/GPUBundles/nvmtl/<name> as before.
   A self-contained bundle can then live where a sandbox lets a process map code: WebKit's GPU process
   (com.apple.WebKit.GPUProcess.sb) reads /Library/GPUBundles but allows file-map-executable only under /System/Library
   (Extensions, Frameworks, ...), /usr/lib and the cryptexes - the kernel logs "deny(1) file-map-executable
   /Library/GPUBundles/NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver" and Safari's WebGL has no device. Apple's own
   Metal drivers are bundles in /System/Library/Extensions that carry their libraries inside. */
static inline const char *nvmtl_companion(const char *name)
{
    enum { NC = 8 };
    static struct { const char *name; char path[1024]; } cache[NC]; static int n; static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&lk);
    for (int i = 0; i < n; i++) if (!strcmp(cache[i].name, name)) { pthread_mutex_unlock(&lk); return cache[i].path; }
    char path[1024]; path[0] = 0;
    Dl_info di;
    if (dladdr((const void *)nvmtl_companion, &di) && di.dli_fname) {
        const char *m = strstr(di.dli_fname, "/Contents/MacOS/");
        if (m) {
            snprintf(path, sizeof path, "%.*s/Contents/Frameworks/nvmtl/%s", (int)(m - di.dli_fname), di.dli_fname, name);
            if (access(path, R_OK) != 0) path[0] = 0;
        }
    }
    if (!path[0]) snprintf(path, sizeof path, "/Library/GPUBundles/nvmtl/%s", name);
    const char *r = path;
    if (n < NC) { cache[n].name = name; snprintf(cache[n].path, sizeof cache[n].path, "%s", path); r = cache[n++].path; }
    else r = strdup(path);
    pthread_mutex_unlock(&lk);
    return r;
}
#include <stdint.h>
#include <stddef.h>
#include "nvmtl_table_pc.h"
#include "nvmtl_cs_hazard.h"
#include "nvmtl_rt_layout.h"
const char *nvmtl_image_ident(const void *addr, char *out, size_t n);
typedef struct { void *buf; void *mem; void *map; size_t size; size_t alloc; int placed; void *pool; size_t poff; int imported; int bar1; size_t moff; int sparse, sysmem; uint32_t mtype1; /* memory type index + 1 of mem; 0 = not recorded */ size_t vksize; /* the VkBuffer's size when larger than size (a parked buffer reused smaller); 0 = size */
                 void *addr_of; uint64_t addr; /* perf1: device address of VkBuffer addr_of (nvmtl_vk_buffer_address; every creation path zeroes the struct) */ } nvk_buffer;
#include "nvmtl_sample_positions.h"
typedef struct { void *img; void *mem; void *view; uint32_t w, h; size_t alloc; uint32_t layout; uint32_t mips; uint32_t fmt, bpp; uint32_t storage, mtl_type, layers; int placed; uint32_t bw, bh; uint32_t samples; uint32_t sysmem; uint32_t vkflags, usage;  uint32_t vfl;  uint64_t up_token; } nvk_image;
#define NVMTL_NVAR 16
#define NVMTL_NVAR_MAX 4096
#define NVMTL_NCOL 8
#define NVMTL_RPX_MAX  1020
#define NVMTL_RPX_GROW 4
#define NVMTL_LOAD_COLOUR(i) ((i) ? (1u << ((i) + 2)) : 1u)
#define NVMTL_LOAD_DEPTH 2u
#define NVMTL_LOAD_STENCIL 4u
#define NVMTL_LOAD_ALL 0x3ffu
typedef struct { void *rp; void *pipe; void *layout; int has_depth; uint32_t dfmt; uint32_t stex_hi[2];
    void *rpv[4]; uint32_t cfmt;
    void *mod; uint32_t nvar, capvar; struct nvmtl_cvar { uint32_t l[3]; void *pipe; } *var;
    uint32_t vin_mask;
    uint32_t samples;
    uint32_t tess, tess_n, tess_cpidx, tess_nvb, tess_vb[8];
    uint32_t ncol, cfmts[NVMTL_NCOL], has_stencil, rmask;
    void **rpx; uint32_t *rpxk, nrpx, crpx;
    uint32_t vin_dyn, vin_stride[32];
    uint64_t bmask[4]; uint32_t bmask_ok;
    uint64_t gmask[2][4]; uint32_t gmask_ok;
    uint64_t gmask2[2][4], bmask2[4]; uint32_t gmask2_ok, bmask2_ok;   /* the same with the framebuffer-fetch set (3) left out like the heap (NVMTL_DESC_MASK) */
    uint32_t ag_mask, ag_req; void *ag_src; size_t ag_srcn; void *ag_safe;
    uint64_t shape_identity;
    uint32_t lin;
    uint64_t tbm[2][4]; uint32_t tbany;
    uint32_t fetch_mask;   /* colour attachments the fragment shader reads ([[color(n)]], set 3 binding 192+n) */
    uint32_t fetch_zero;   /* framebuffer fetch is off but the fragment shader reads [[color(n)]]: set 3 holds the dummy image */
    uint32_t stencil_emu;  /* the fragment shader's stencil export was lowered to a per-bit discard (push constant 160) */
    uint32_t skip_draws;   /* NVMTL_SKIP_FS names this fragment shader: its draws are not recorded (hang bisection) */
    char ktname[48];       /* the kernel's name, for NVMTL_KTIME */
    const void *dbplan[2]; /* descriptor-buffer model: the stage layouts (nvmtl_dbplan, nvmtl_dbuf.c); NULL in the classic model */
    const void *dbtab[2];  /* descriptor-buffer model: the table plan of a stage whose address table has a set of its own
                              (compute set 1, vertex set 4, fragment set 5; nvmtl_db_table_plan); NULL = in the stage region.
                              A tessellation pipeline's post-tessellation function is stage 0 (set 4). */
    uint32_t dbtset;       /* compute, descriptor-buffer model: the set that holds the table set (1, or for a kernel whose
                              module names set 1 the first free set from 3: nvmtl_db_free_set); the table is stage 0's
                              when dbtab[0] is set, stage 1's when dbtab[1] is. 0 = no table set */
    uint32_t tabneed[2];   /* stage 1d: the address-table entries each stage reads (nvmtl_spirv_tabneed); 0 = the whole table */
    nvmtl_pc_table pc_table[2]; /* opt-in compact address entries, not a descriptor/table snapshot */
    uint32_t cs_acc;       /* compute2: NVMTL_CS_ACC_* - how the kernel reads GPU-written memory (narrow serial barrier) */
    uint8_t cs_reach, cs_wr; /* compute2: every address it touches comes from its pushed table qwords; it writes memory */
    /* glub1 NVMTL_GL_UBO (nvmtl_gl_ubo.h): pc_table entries whose buffer the stage reads through one 64-aligned block load
       (ANV pushes it); each draw checks the pushed address and that the buffer holds the block, else binds ubo_twin */
    uint32_t ubo_mask[2], ubo_ext[2][NVMTL_PC_TABLE_MAX];
    void *ubo_src;         /* what the pipeline was made from (nvmtl_ubo_src), to build ubo_twin when a draw needs it */
    void *ubo_twin;        /* the same pipeline without the pass, made on the first draw whose binding breaks a promise */
    uint32_t ubo_twin_failed;
    uint32_t db_dyn;   /* the depth bounds test and its range are dynamic state of this pipeline (depth attachment + depthBounds feature) */
} nvk_pipeline;
void nvmtl_vk_pipeline_destroy(nvk_pipeline *p);
typedef struct { void *pool; nvk_buffer stage; } nvk_queue;
#include "nvmtl_sampler_desc.h"
typedef struct { void *s; int linear, repeat; float lod_min, lod_max; nvmtl_sampler_desc desc; int full_state; } nvk_sampler;
typedef struct { void *pool, *set; } nvk_setref;
typedef struct {
    uint64_t gpu_tick, uptime_ns, uncertainty_ns;
    uint32_t valid_bits;
    double period_ns;
    unsigned valid;
} nvmtl_gpu_clock;
typedef struct {
    nvmtl_gpu_clock clock;
    uint64_t start_tick, end_tick, host_before_ns, host_after_ns;
    unsigned valid;
} nvmtl_gpu_interval;
void nvmtl_vk_clock_refresh(void);
int nvmtl_vk_interval_seconds(const nvmtl_gpu_interval *t, double *start, double *end);

typedef struct { void *cb; void *fb; void *pool; void *dset[2]; void *depth_view; void *table;
                 void **fbs; unsigned nfb, capfb;
                 nvk_setref *sets; unsigned nsets, capsets;
                 void *address_pages; unsigned address_count;
                 unsigned nobind, skipped;
                 unsigned skip_oom;   /* of the skipped: for want of memory (descriptor sets, address tables) */
                 void *dcache[32]; void *dcachepool; unsigned ndcache;
                 unsigned in_rp, occlusion_reset; uint32_t wseq;
                 void *fetchview[NVMTL_NCOL];   /* the open pass's colour views, read by [[color(n)]] framebuffer fetch */
                 unsigned fetch_draw, topo;   /* the bound pipeline reads [[color(n)]] in this pass; last MTLPrimitiveType set */
                 /* the last framebuffer fetch set made in this command buffer and what it points at: a draw whose pass
                    and mask are the same reuses it (Core Animation draws hundreds of fetch draws per pass) */
                 void *fetch_last_set; uint32_t fetch_last_mask; void *fetch_last_view[NVMTL_NCOL];
                 /* stencil export emulation (nvmtl_semu_draw): the bound pipeline's layout when it exports stencil, and the
                    stencil reference / write masks / depth write last set, which each per-bit pass changes and restores */
                 void *semu_layout; uint32_t st_ref[2], st_wmask[2]; int depth_write;
                 int skip_group;   /* inside a debug group NVMTL_SKIP_GROUP names: draws are not recorded (hang bisection) */
                 unsigned direct_sass;   /* recorded a dispatch of compiled machine code: never reuse this pool */
                 void *occlusion_pages; uint32_t occlusion_count;
                 void *timing_pool; nvmtl_gpu_interval timing;
                 void *pt_pool; unsigned pt_n, pt_open; uint32_t pt_w[128], pt_h[128], pt_f[128]; uint32_t pt_fs[128][4];
                 void *counter_recording; unsigned counter_error;
                 nvmtl_sample_pattern sample_pattern;
                 void **rvs; unsigned nrv, caprv;
                 unsigned restart_off;  /* GL turned primitive restart off (setPrimitiveRestartEnabled:NO) */
                 uint32_t dyn_out[2];   /* address-table dynamic offset of the set the last snapshot returned, per stage */
                 void *tab_out[2];      /* the table page buffer that set's descriptor names */
                 /* Vulkan graphics bindings persist until changed; compute has its own bind point. */
                 void *graphics_pipeline, *graphics_layout, *graphics_sets[3];
                 uint32_t graphics_set_count, graphics_offset_count, graphics_offsets[2];
                 void *pc_table_layout; uint32_t pc_table_valid;
                 uint64_t pc_table_values[2][NVMTL_PC_TABLE_MAX];
                 uint64_t subseq;   /* its submission's number on the queue (nvmtl_sub_done); 0 = not submitted */
                 void *kt_pool; unsigned kt_n, kt_open;   /* NVMTL_KTIME: a timestamp pair around each dispatch */
                 const void *kt_p[64]; uint32_t kt_tg[64]; uint64_t kt_thr[64];
                 uint32_t kt_sys[2]; uint8_t kt_dsys[64];   /* buffer slots bound to system memory (PCIe), per set; per timed dispatch */
                 /* implicit IOSurface sync (nvmtl_vk_isync): the surfaces this submission writes or reads, whose fences it
                    waits for on the GPU (imported) and to which its own fence is attached after the submit; and a call
                    made right after that attach, on the submitting thread (publication at submission) */
                 uint32_t isync_sid[8]; uint8_t isync_write[8]; unsigned n_isync;
                 void (*on_submit)(void *ctx); void *on_submit_ctx;
                 void *db;   /* descriptor-buffer model: this command buffer's descriptor ring and shadow (nvmtl_dbstate) */
                 /* vertex and index buffers last bound in VkCommandBuffer vbc_cb / ibc_cb (stage 1d): a draw whose binding did
                    not change makes no vkCmdBind* call, so ANV re-emits no VERTEX_BUFFER_STATE / index state either.
                    Vulkan keeps these bindings for the whole command buffer (render passes and pipelines do not reset
                    them); a different VkCommandBuffer, or a zeroed struct (nvmtl_vk_cmd_begin), starts empty.
                    vbc_st: the stride given with vkCmdBindVertexBuffers2, or NVMTL_VBC_NOSTRIDE for vkCmdBindVertexBuffers */
                 void *vbc_cb; uint32_t vbc_valid; void *vbc_buf[32]; uint64_t vbc_off[32], vbc_st[32];
                 void *ibc_cb; void *ibc_buf; uint64_t ibc_off; uint32_t ibc_type;
                 uint32_t tab_need[2];   /* stage 1d: the entries the next address-table snapshot of each stage must hold (0 = all) */
                 /* compute2 NVMTL_CS_HAZARD: the buffer ranges serial dispatches since the last compute barrier can reach,
                    and which of them were written; hz_unknown: one of those dispatches could reach anything */
                 uint64_t hz_lo[32], hz_hi[32]; uint32_t hz_wr, hz_n; uint8_t hz_unknown;
                 void *asq_pages; uint32_t asq_count;   /* acceleration structure compacted-size queries (nvmtl_vk_cmd_write_compacted_size) */
                 uint8_t life_live;   /* nm271 grace: counted in g_cb_live from nvmtl_vk_cmd_begin to nvmtl_cmd_retire */
                 /* the encoder's depth bounds (setDepthTestMinBound:maxBound:), re-emitted when a pipeline that has them as
                    dynamic state is bound, since binding a pipeline without them leaves that state undefined */
                 uint8_t db_on; float db_lo, db_hi;
                 int dead; /* made after the device was lost: records nothing, submits as "device removed" (nvmtl_vk_cmd_begin) */
                 void *sparse;   /* NVMTL_SPARSE_TEXTURES: tile binds made before this recording's commands (nvmtl_sparse_batch) */
               } nvk_cmdbuf;
#define NVMTL_VBC_NOSTRIDE (~0ull)
int  nvmtl_vk_init(void);
/* Implicit IOSurface synchronization on the GPU (IntelXe.kext surface fences through Mesa ANV): 1 when available. */
int  nvmtl_vk_isync(void);
/* c's submission writes (write=1) or reads surface sid: it waits for the surface's fences on the GPU and its own fence is
   attached to the surface. Up to 8 per submission; returns 0, or -1 when it could not be recorded (not available, full). */
int  nvmtl_vk_cmd_surface_use(nvk_cmdbuf *c, uint32_t sid, int write);
/* CPU wait for surface sid's fences (writer: also its readers), up to timeout_ms: 0 done or nothing to wait for, 1 timed
   out, -1 not available */
int  nvmtl_vk_surface_cpu_wait(uint32_t sid, int writer, unsigned timeout_ms);
/* nvmtl_vk_image_write_host_buf for a foreign IOSurface's pages with implicit sync: the upload waits on the GPU for the
   surface's writers and is left in flight (finished like an async texture upload: before a readback, re-upload or
   destroy of img); rel(ctx) runs when it has landed (it releases the pages' import). Without implicit sync it runs the
   synchronous upload and calls rel(ctx) before returning. */
/* retire implicit-sync uploads that have landed (releases their page imports); cheap, call before taking an import */
void nvmtl_vk_asyncup_reap(void);
int  nvmtl_vk_image_write_host_buf_isync(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, uint32_t sid,
                                         void (*rel)(void *), void *ctx);
int nvmtl_vk_sample_positions_backend(void);
int nvmtl_vk_sample_positions_default(uint32_t count, nvmtl_sample_pattern *out);
int  nvmtl_vk_cmd_abandon(nvk_cmdbuf *c);
int  nvmtl_vk_submit_partial_on(void);   /* NVMTL_SUBMIT_PARTIAL=1: submit recordings with unrecorded draws (before fix30) */
void nvmtl_vk_cmd_counts(uint64_t *begun, uint64_t *submitted, uint64_t *abandoned);
const char *nvmtl_vk_device_name(void);
/* the GPU vendor (0x10de, 0x1002, 0x8086) of this process's Metal device; set from the accelerator it was made on */
uint16_t nvmtl_vk_set_accelerator(mach_port_t accelerator);
uint16_t nvmtl_vk_vendor(void);
void nvmtl_vk_limits(uint32_t *layers, uint32_t *shared_bytes, uint32_t wg[3], uint32_t *wg_inv, uint32_t *sm);
int nvmtl_vk_tg_reject(const uint32_t l[3], const uint32_t *t, const char *kernel, uint32_t lin);
void nvmtl_vk_pubcaps(uint32_t *color_samples, uint32_t *depth_samples, uint64_t *max_storage_range, uint64_t *linear_align);
/* host_visible: 0 VRAM (Private), 1 system RAM (mapped), 2 Shared (VRAM through BAR1 when NVMTL_SHARED_VRAM=1, else
   system RAM), 3 a small dedicated buffer the command streamer reads (ICB slot records): VRAM through BAR1 whatever the
   Shared policy, system RAM if that is refused; never pooled or parked-taken */
int  nvmtl_vk_buffer_create(size_t size, int host_visible, nvk_buffer *out);
enum { NVP_BEGIN, NVP_BUFNEW, NVP_BUFPOOL, NVP_BUFDEL, NVP_HEAP, NVP_SYNCIN, NVP_SYNCOUT, NVP_COPY, NVP_N };
uint64_t nvmtl_perf_now(void);
void nvmtl_perf_note(int what, uint64_t t0, uint64_t bytes);
int  nvmtl_vk_buffer_import_host(void *ptr, size_t size, nvk_buffer *out);
size_t nvmtl_vk_host_import_align(void);
/* flags: the VkBuildAccelerationStructureFlagsKHR of the last build, built: a build was recorded, shape: geometry count
   and per-geometry primitive counts of that build (an update must repeat them) */
typedef struct { void *as; uint64_t addr; size_t size; int instance; uint32_t flags, built; uint64_t shape; } nvk_accel;
/* transform: the device address of the geometry's VkTransformMatrixKHR (3x4 row-major) for the build, 0 = none;
   transform_src / transform_off / transform_rowmajor: the Metal transformationMatrixBuffer (nvk_buffer *), its offset
   and layout, which the build copies into that 3x4 form in its command buffer (nvmtl_vk_cmd_transform_matrix) */
/* rec: the geometry's record in the structure's table (nvmtl_rt_layout.h). A curve geometry (rec.kind 2) is built as
   bounding boxes (is_boxes, count = segments): the build gives boxes the address of the per-segment boxes the curve
   pass writes (nvmtl_vk_cmd_curve_aabbs) */
typedef struct { uint64_t vertices; uint32_t stride; uint32_t count; uint32_t vertex_format; uint32_t max_vertex;
                 uint64_t indices; uint32_t index_type; uint32_t opaque;
                 uint64_t boxes; uint32_t box_stride; uint32_t is_boxes;
                 uint64_t transform; const void *transform_src; uint64_t transform_off; uint32_t transform_rowmajor;
                 nvmtl_geom_rec rec; } nvmtl_geom;
/* VkPhysicalDeviceAccelerationStructurePropertiesKHR: 0 maxGeometryCount, 1 maxInstanceCount, 2 maxPrimitiveCount
   (0 when ray tracing is absent) */
uint64_t nvmtl_vk_rt_limit(int which);
/* Metal's packed float4x3 (column-major, 4 columns of 3 floats) or 3x4 row-major matrix at src+src_off -> a Vulkan
   VkTransformMatrixKHR (3 rows of 4) at dst+dst_off, as copies in c's order (the build reads it when it runs) */
int  nvmtl_vk_cmd_transform_matrix(nvk_cmdbuf *c, const nvk_buffer *src, uint64_t src_off, int rowmajor, nvk_buffer *dst, uint64_t dst_off);
int  nvmtl_vk_rt_available(void);
int  nvmtl_vk_barycentrics_available(void);   // VK_KHR_fragment_shader_barycentric present and enabled-capable
uint32_t nvmtl_vk_cs_simd(void);
/* compute2: the serial-dispatch barrier before a dispatch of p (NVMTL_CS_BAR: narrow sync2 masks for p's reads), and
   NVMTL_CS_HAZARD: 1 = a barrier is needed before p's dispatch (forced: the caller needs one anyway), 0 = p cannot reach
   anything a dispatch since the last barrier wrote, nor write what one read */
void nvmtl_vk_cmd_barrier_c2c_p(nvk_cmdbuf *c, const nvk_pipeline *p);
int  nvmtl_vk_cs_hazard(nvk_cmdbuf *c, const nvk_pipeline *p, int forced);
int  nvmtl_vk_cs_hazard_on(void);   /* compute1: NVMTL_CS_SIMD forced compute SIMD width, 0 = the compiler picks */
/* the Vulkan build flags of a Metal MTLAccelerationStructureUsage: the size query and every build of a structure use them */
uint32_t nvmtl_vk_accel_flags(uint64_t mtl_usage);
int  nvmtl_vk_accel_sizes(const nvmtl_geom *g, uint32_t ngeom, int instance, uint32_t instance_count, uint32_t flags,
                          size_t *as_size, size_t *scratch_size, size_t *update_scratch_size);
/* the Vulkan structure at offset (NVMTL_AS_HEADER: the Metal header before it) of backing, size bytes */
int  nvmtl_vk_accel_create(nvk_buffer *backing, size_t offset, size_t size, int instance, nvk_accel *out);
void nvmtl_vk_accel_destroy(nvk_accel *a);
/* update_src NULL: a build (MODE_BUILD); else an update of update_src into dst (MODE_UPDATE, update_src's flags) */
int  nvmtl_vk_cmd_build_accel(nvk_cmdbuf *c, nvk_accel *dst, const nvmtl_geom *g, uint32_t ngeom, uint64_t instances_addr, uint32_t instance_count,
                              uint64_t scratch_addr, uint32_t flags, nvk_accel *update_src);
/* 1 when an update of a with these geometries is valid Vulkan (built with ALLOW_UPDATE, same geometry count and counts) */
int  nvmtl_vk_accel_can_update(const nvk_accel *a, const nvmtl_geom *g, uint32_t ngeom, uint32_t instance_count);
int  nvmtl_vk_cmd_copy_accel(nvk_cmdbuf *c, nvk_accel *src, nvk_accel *dst, int compact);
/* the compacted size of a (built with ALLOW_COMPACTION) into dst at off, 4 or 8 bytes, on the GPU in c's order; stage:
   an 8-byte device buffer the caller keeps until c retires */
int  nvmtl_vk_cmd_write_compacted_size(nvk_cmdbuf *c, nvk_accel *a, nvk_buffer *dst, size_t off, uint32_t bytes, nvk_buffer *stage);
/* rtopt5: as above, + add (a primitive structure's NVMTL_AS_WRAP) on the GPU (nvmtl_rt_sizeadd.comp); add 0 = the above */
int  nvmtl_vk_cmd_write_compacted_size_add(nvk_cmdbuf *c, nvk_accel *a, nvk_buffer *dst, size_t off, uint32_t bytes, nvk_buffer *stage, uint32_t add);
/* rtopt5: NVMTL_AS_WRAP when primitive structures carry a one-instance top-level wrapper (nvmtl_rt_layout.h), 0 when
   not (NVMTL_RT_WRAP=0, or the driver's one-instance top-level structure does not fit); *scratch: its build scratch */
uint32_t nvmtl_vk_rt_wrap(size_t *scratch);
/* GPU conversion of Metal instance descriptors (nvmtl_rt_instances.comp): addresses and layout as that shader's arguments */
typedef struct { uint64_t src, dst, uid, table, count_addr, rec; uint32_t stride, max_count, type, n_as, flags, wrap; } nvmtl_inst_args;
int  nvmtl_vk_cmd_convert_instances(nvk_cmdbuf *c, const nvmtl_inst_args *a);
/* GPU pass of nvmtl_rt_curves.comp: one VkAabbPositionsKHR per curve segment at dst (the Bezier hull of its control
   points, padded by the largest radius; twice that for flat curves) */
typedef struct { uint64_t cp, radius, index, dst; uint32_t cp_stride, cp_format, r_stride, r_format, index_type, seg_cp, basis, ctype,
                 seg_count, cp_count, pad0, pad1; } nvmtl_curve_args;
int  nvmtl_vk_cmd_curve_aabbs(nvk_cmdbuf *c, const nvmtl_curve_args *a);
/* size bytes of data into b at off, in c's order (a host-visible staging buffer the caller keeps until c retires) */
int  nvmtl_vk_cmd_upload(nvk_cmdbuf *c, nvk_buffer *b, size_t off, const void *data, size_t size, nvk_buffer *stage);
int  nvmtl_vk_image_create(uint32_t w, uint32_t h, nvk_image *out);
int  nvmtl_vk_image_create_fmt(uint32_t w, uint32_t h, int bgra, nvk_image *out);
int  nvmtl_vk_pipeline_create(const void *vs, size_t vn, const void *fs, size_t fn, nvk_pipeline *out);
int  nvmtl_vk_pipeline_create_fmt(const void *vs, size_t vn, const void *fs, size_t fn, int bgra, nvk_pipeline *out);
int  nvmtl_vk_pipeline_create_depth(const void *vs, size_t vn, const void *fs, size_t fn, int bgra, int hasDepth, nvk_pipeline *out);
int  nvmtl_vk_depth_create(uint32_t w, uint32_t h, nvk_image *out);
void nvmtl_vk_cmd_set_depth(nvk_cmdbuf *c, int test, int write, int compare);
int  nvmtl_vk_queue_create(nvk_queue *out);
int  nvmtl_vk_cmd_begin(nvk_queue *q, nvk_cmdbuf *out);
int  nvmtl_vk_cmd_begin_render(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4]);
int  nvmtl_vk_cmd_begin_render_ex(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4], uint32_t load);
int  nvmtl_vk_cmd_begin_render_ms(nvk_cmdbuf *c, nvk_image *img, nvk_pipeline *p, const float clear[4], uint32_t load, nvk_image *resolve);
uint64_t nvmtl_vk_working_set(void);
uint64_t nvmtl_vk_recommended_working_set(void);   /* 931d015: the working set capped at the card less a reserve */
uint64_t nvmtl_vk_vram_bytes(void);
void     nvmtl_vk_mem_split(uint64_t *vram, uint64_t *sys);
int  nvmtl_vk_image_create_ms(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, nvk_image *out);
uint64_t nvmtl_vk_timestamp_frequency(void);
int  nvmtl_vk_sample_rate_shading(void);   /* nm29: the device has sampleRateShading (pull-model interpolation) */
int  nvmtl_vk_image_create_ms_full(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, uint32_t samples, uint32_t mtl_type, uint32_t layers, nvk_image *out);
int  nvmtl_vk_depth_create_ms(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, nvk_image *out);
int  nvmtl_vk_pipeline_create_vin_ms(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend, const void *vin, uint32_t samples, void *out);
void nvmtl_vk_cmd_set_viewport(nvk_cmdbuf *c, float x, float y, float w, float h, float zn, float zf);
void nvmtl_vk_cmd_draw(nvk_cmdbuf *c, uint32_t first, uint32_t count, uint32_t instances, uint32_t baseInstance);
int  nvmtl_vk_cmd_clear_image(nvk_cmdbuf *c, nvk_image *img, const float clear[4]);
void nvmtl_vk_cmd_set_scissor(nvk_cmdbuf *c, int32_t x, int32_t y, uint32_t w, uint32_t h);
void nvmtl_vk_cmd_set_cull(nvk_cmdbuf *c, int mode);
int  nvmtl_vk_cmd_set_topology(nvk_cmdbuf *c, unsigned mtlType);
void nvmtl_vk_cmd_draw_indexed(nvk_cmdbuf *c, nvk_buffer *idx, size_t offset, uint32_t count, int is32,
                               uint32_t instances, int32_t baseVertex, uint32_t baseInstance);
#define NVMTL_NQUERY 128
#define NVMTL_NQUERY_WORDS ((NVMTL_NQUERY + 63) / 64)
#define NVMTL_NQUERY_NOANS 32
int nvmtl_vk_cmd_sum_occlusion(nvk_cmdbuf *c, nvk_buffer *src, size_t first, uint32_t count, nvk_buffer *dst, size_t offset);
int nvmtl_vk_cmd_alloc_occlusion(nvk_cmdbuf *c, uint32_t *query);
void nvmtl_vk_cmd_retire_occlusion(nvk_cmdbuf *c);
void *nvmtl_vk_ts_pool_create(uint32_t count);
void nvmtl_vk_ts_pool_destroy(void *pool);
int  nvmtl_vk_cmd_ts_write(nvk_cmdbuf *c, void *pool, uint32_t idx, int barrier);
int  nvmtl_vk_cmd_ts_resolve(nvk_cmdbuf *c, void *pool, uint32_t first, uint32_t count, nvk_buffer *dst, size_t off);
int  nvmtl_vk_ts_read(void *pool, uint32_t first, uint32_t count, uint64_t *out);
int  nvmtl_vk_gpu_timestamp(uint64_t *cpu_ns, uint64_t *gpu);
int  nvmtl_vk_ts_supported(void);   /* GPU timestamps (Metal counter sampling) offered; decided once, logged */
int  nvmtl_vk_cmd_begin_occlusion(nvk_cmdbuf *c, uint32_t idx);
void nvmtl_vk_cmd_end_occlusion(nvk_cmdbuf *c, uint32_t idx);
int  nvmtl_vk_cmd_copy_occlusion(nvk_cmdbuf *c, uint32_t first, uint32_t count, nvk_buffer *dst, size_t off);
int  nvmtl_vk_have_polygon_mode(void);
int  nvmtl_vk_cmd_set_depth_clip(nvk_cmdbuf *c, int clip);
int  nvmtl_vk_cmd_set_polygon_mode(nvk_cmdbuf *c, int line);
int  nvmtl_vk_have_provoking_last(void);
void nvmtl_vk_cmd_set_provoking_last(nvk_cmdbuf *c, int last);
void nvmtl_vk_cmd_set_blend_color(nvk_cmdbuf *c, const float rgba[4]);
void nvmtl_vk_cmd_set_stencil_ref(nvk_cmdbuf *c, uint32_t front, uint32_t back);
void nvmtl_vk_cmd_set_depth_bias(nvk_cmdbuf *c, float constant, float slope, float clamp);
int  nvmtl_vk_cmd_set_depth_bounds(nvk_cmdbuf *c, int on, float lo, float hi);
void nvmtl_vk_cmd_set_line_width(nvk_cmdbuf *c, float w);
int  nvmtl_vk_fill_buffer(nvk_queue *q, nvk_buffer *b, size_t offset, size_t size, uint8_t value);
int  nvmtl_vk_bind_buffer(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_buffer *b, size_t offset);
int  nvmtl_vk_trace_buffers(nvk_cmdbuf *c, char *out, size_t cap);
int  nvmtl_vk_bind_buffer_offset(nvk_cmdbuf *c, uint32_t set, uint32_t index, size_t offset);
int  nvmtl_vk_bind_texture(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_image *img);
int  nvmtl_vk_sampler_create(int linear, int repeat, nvk_sampler *out);
int  nvmtl_vk_sampler_create_desc(const nvmtl_sampler_desc *desc, nvk_sampler *out);
void nvmtl_vk_sampler_destroy(nvk_sampler *s);
int  nvmtl_vk_bind_sampler(nvk_cmdbuf *c, uint32_t set, uint32_t index, nvk_sampler *s);
void nvmtl_vk_cmd_bind_set(nvk_cmdbuf *c, nvk_pipeline *p);
int  nvmtl_vk_compute_pipeline_create(const void *cs, size_t n, nvk_pipeline *out);
void nvmtl_vk_cmd_dispatch(nvk_cmdbuf *c, nvk_pipeline *p, uint32_t gx, uint32_t gy, uint32_t gz);
void nvmtl_vk_cmd_dispatch_threads(nvk_cmdbuf *c, nvk_pipeline *p, const uint32_t threads[3], const uint32_t tg[3]);
void nvmtl_vk_cmd_dispatch_indirect_tg(nvk_cmdbuf *c, nvk_pipeline *p, nvk_buffer *b, size_t off, const uint32_t tg[3]);
void nvmtl_vk_cmd_end_render(nvk_cmdbuf *c);
int  nvmtl_vk_submit_wait_at(nvk_cmdbuf *c, const char *who);
typedef struct nvmtl_inflight nvmtl_inflight;
nvmtl_inflight *nvmtl_vk_inflight_new(void);
void nvmtl_vk_inflight_free(nvmtl_inflight *f);
int nvmtl_vk_image_reback(nvk_image *i, int to_sysmem, int a8);
void nvmtl_vk_res1_stats(uint64_t out[6]);
int nvmtl_vk_image_migrate(nvk_image *i, int to_sysmem, int a8, nvk_image *old_out);
void nvmtl_vk_image_free_backing(nvk_image *old);
int nvmtl_vk_vram_room(uint64_t want);
uint64_t nvmtl_vk_vram_free_now(void);
uint64_t nvmtl_vk_vram_headroom(void);
void nvmtl_vk_bindless_rewrite(uint32_t slot, void *view);
void nvmtl_vk_res2_stats(uint64_t out[5]);
int nvmtl_vk_buffer_migrate(nvk_buffer *b, int to_sysmem);
void nvmtl_vk_res3_stats(uint64_t out[6]);
int  nvmtl_vk_submit_begin_at(nvk_cmdbuf *c, nvmtl_inflight *f, const char *who);
#define nvmtl_vk_submit_wait(c) nvmtl_vk_submit_wait_at((c), __func__)
#define nvmtl_vk_submit_begin(c, f) nvmtl_vk_submit_begin_at((c), (f), __func__)
int  nvmtl_vk_submit_finish(nvmtl_inflight *f);
/* GPU reset recovery: submit/finish return -2 backend down, -3 lost (this one saw the loss; the device was recovered),
   -4 lost (in flight across a recovery), -5 its fence gave up with no loss reported. Counters for tests and logs. */
int  nvmtl_vk_recover_counts(unsigned *recoveries, unsigned *gen, unsigned long long *hang, unsigned long long *victim);
void nvmtl_vk_gsub_nvk_stats(uint64_t *out, unsigned n);
int nvmtl_vk_pre_open(void);
int nvmtl_vk_pre_copy(nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size);
int nvmtl_vk_pre_fill(nvk_buffer *b, size_t offset, size_t size, uint32_t word);
void nvmtl_vk_pre_hold(void *retained);
extern void (*nvmtl_pre_release_hook)(void *);
extern void (*nvmtl_pre_submit_hook)(void);
enum { NVH_MEMNEW, NVH_MEMFREE, NVH_IMGNEW, NVH_IMGDEL, NVH_PIPE, NVH_SHADERMOD, NVH_XLATE, NVH_SPVMISS, NVH_PARK, NVH_REUSE, NVH_EVICT,
       NVH_SURFOUT, NVH_SURFIN, NVH_SUBMIT, NVH_FENCEWAIT, NVH_WAITCOMPLETED, NVH_WAITSCHEDULED, NVH_GPU, NVH_DONELAG, NVH_EVENTWAIT, NVH_RESWAIT, NVH_SURFFOLD, NVH_STALEOUT, NVH_OOO, NVH_XLATESAMP, NVH_MA_DEVS, NVH_MA_DEVL, NVH_MA_HOST, NVH_MA_IMPORT,
       NVH_C_RPASS, NVH_C_DISPATCH, NVH_C_DRAW, NVH_C_BARRIER, NVH_C_IMGBAR, NVH_C_XFER, NVH_C_SWITCH, NVH_N };   // NVMTL_HITCH_MS
int  nvmtl_hitch_ms(void);
uint64_t nvmtl_hitch_t0(void);
void nvmtl_hitch_add(int what, uint64_t t0, uint64_t bytes);
void nvmtl_hitch_add_ns(int what, uint64_t dt);
void nvmtl_hitch_frame(const char *source);
int  nvmtl_vk_cmd_image_to_host(nvk_cmdbuf *c, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes);
int  nvmtl_vk_cmd_image_to_host_scan(nvk_cmdbuf *c, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, nvk_buffer *scan, size_t scan_row);
int  nvmtl_vk_accel_surface_wait_all(void);
int  nvmtl_vk_image_read(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes);
int  nvmtl_vk_image_write(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes);
int  nvmtl_vk_image_write_host(nvk_queue *q, nvk_image *img, void *base, size_t row_bytes, size_t bytes);
int  nvmtl_vk_image_write_host_buf(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes);
int  nvmtl_vk_image_read_host(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes);
int  nvmtl_vk_image_read_host_scan(nvk_queue *q, nvk_image *img, nvk_buffer *hb, size_t off, size_t row_bytes, nvk_buffer *scan, size_t scan_row);
int  nvmtl_vk_scanout_buffer(size_t size, nvk_buffer *out, uint64_t *va, int visible);
int  nvmtl_vk_scanout_op(uint32_t op, uint32_t sid, uint64_t va, uint64_t size, uint32_t w, uint32_t h, uint32_t pitch, uint32_t *ggtt);
int  nvmtl_vk_copy_buffer(nvk_queue *q, nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size);
int  nvmtl_vk_image_create_mips(uint32_t w, uint32_t h, int bgra, uint32_t mips, nvk_image *out);
int  nvmtl_vk_image_create_ex(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, nvk_image *out);
int  nvmtl_vk_image_create_ex2(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, nvk_image *out);
int  nvmtl_vk_format_is_storage(uint32_t vkfmt);
int  nvmtl_vk_format_mip_blit(uint32_t vkfmt);
int  nvmtl_vk_formats_filter_linear(const uint32_t *vkfmts, int n);
int  nvmtl_vk_formats_msaa(const uint32_t *vkfmts, int n, uint32_t samples);
int  nvmtl_vk_bind_storage_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view);
uint32_t nvmtl_vk_bindless_put(void *view);
void nvmtl_vk_bindless_put_storage(uint32_t slot, void *view);
uint32_t nvmtl_vk_bindless_put_texel(void *bufview, int storage);
uint32_t nvmtl_vk_bindless_sampler_slot(void *vksampler);
uint32_t nvmtl_vk_bindless_sampler_capacity(void);
void     nvmtl_vk_bindless_drop(uint32_t slot);
int  nvmtl_vk_pipeline_create_ex(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, int hasDepth, nvk_pipeline *out);
int  nvmtl_vk_image_view_create_ex(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount, void **out_view);
int  nvmtl_vk_depth_create_ex(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_image *out);
typedef struct { uint32_t enabled, src_rgb, dst_rgb, op_rgb, src_alpha, dst_alpha, op_alpha, write_mask; } nvk_blend_state;
int nvmtl_vk_pipeline_create_blend(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const nvk_blend_state *blend, nvk_pipeline *out);
#define NVMTL_NVIN 31
_Static_assert(NVMTL_NVIN <= 32, "nvk_pipeline.vin_stride and every vin mask hold 32 buffer indices");
#define NVMTL_STRIDE_DYNAMIC 0xFFFFFFFFu
typedef struct { uint32_t nattr; struct { uint32_t location, buffer, mtlfmt, offset; } attr[NVMTL_NVIN];
                 struct { uint32_t stride, step, rate; } layout[NVMTL_NVIN]; } nvk_vertex_input;
#define NVMTL_TESS_DESC_DEFINED
typedef struct { uint32_t patch_type, n, partition, winding, stepfn, scale_en, cpidx; float maxf; int32_t inst_loc; } nvk_tess_desc;
int nvmtl_vk_pipeline_create_tess(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const void *blend,
                                  const void *vin, uint32_t samples, const nvk_tess_desc *td, void *out);
int nvmtl_spirv_tess_info(const void *spv, size_t bytes, uint32_t *patchType, uint32_t *cps);
typedef struct { uint32_t ncol, cfmt[NVMTL_NCOL]; nvk_blend_state blend[NVMTL_NCOL]; uint32_t dfmt, discard, a2c, a2one; } nvk_rt;
typedef struct { nvk_image *col[NVMTL_NCOL]; void *colview[NVMTL_NCOL]; uint32_t colw[NVMTL_NCOL], colh[NVMTL_NCOL];
    nvk_image *res[NVMTL_NCOL]; void *resview[NVMTL_NCOL]; uint32_t resw[NVMTL_NCOL], resh[NVMTL_NCOL];
    float clear[NVMTL_NCOL][4]; nvk_image *ds; void *dsview; float clear_depth; uint32_t clear_stencil, load, w, h;
    uint32_t layers; nvk_image *dres; void *dresview; uint32_t dresmode, sresmode;
    nvmtl_sample_pattern sample_pattern; uint32_t depth_store_options, stencil_store_options;
    uint32_t ds_level, ds_layer;
    uint32_t colfmt[NVMTL_NCOL], resfmt[NVMTL_NCOL];
    uint32_t col_level[NVMTL_NCOL], col_layer[NVMTL_NCOL];  } nvk_pass;
typedef struct { uint32_t ncol, cfmts[NVMTL_NCOL], has_depth, dfmt, samples; } nvk_rtsig;
int  nvmtl_vk_pipeline_create_rt(const void *vs, size_t vn, const void *fs, size_t fn, const nvk_rt *rt, const void *vin, uint32_t samples,
                                 const nvk_tess_desc *td, void *out);
int  nvmtl_vk_cmd_begin_pass(nvk_cmdbuf *c, const nvk_pass *ps, nvk_pipeline *p);
int  nvmtl_vk_cmd_empty_pass(nvk_cmdbuf *c, const nvk_pass *ps);
int  nvmtl_vk_pass_kind(const nvk_pass *ps);
void nvmtl_vk_cmd_set_stencil(nvk_cmdbuf *c, int enable, const uint32_t *front6, const uint32_t *back6);
int  nvmtl_vk_cmd_retire_view(nvk_cmdbuf *c, void *view);
int  nvmtl_vk_cmd_attachment_view(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, void **out, uint32_t *w, uint32_t *h);
int  nvmtl_vk_depth_create_full(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, uint32_t samples, uint32_t mtl_type,
                                uint32_t layers, uint32_t mips, nvk_image *out);
int  nvmtl_vk_dual_src(void);
int  nvmtl_vk_texel_on(void);
int  nvmtl_vk_texel_view_create(nvk_buffer *b, size_t offset, size_t range, uint32_t vkfmt, int storage, void **alias, void **view, int *storage_ok);
void nvmtl_vk_texel_view_destroy(void *alias, void *view);
int  nvmtl_vk_bind_texel_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view, int storage);
int  nvmtl_spirv_has_index1(const void *p, size_t n);
int  nvmtl_vk_cmd_attachment_view_n(nvk_cmdbuf *c, nvk_image *img, int depth, uint32_t level, uint32_t slice, uint32_t nlayers,
                                    uint32_t vkfmt, int a8, void **out, uint32_t *w, uint32_t *h);
int  nvmtl_vk_image_view_create_range_swz(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount, uint32_t baseLayer,
                                          uint32_t layerCount, uint32_t mtl_view_type, const uint8_t mtl_swz[4], void **out_view);
void nvmtl_vk_pipeline_sig(const nvk_pipeline *p, nvk_rtsig *out);
int  nvmtl_vk_pipeline_compatible(const nvk_rtsig *sig, const nvk_pipeline *p);
int nvmtl_vk_cmd_draw_patches(nvk_cmdbuf *c, const nvk_pipeline *p, uint64_t fac, uint32_t istride, float scale, uint64_t cpx, uint64_t pix,
                              uint32_t start, uint32_t count, uint32_t instances, uint32_t baseInstance, nvk_buffer *ind, size_t indoff);
int nvmtl_vk_pipeline_create_vin(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, const nvk_blend_state *blend,
                                 const nvk_vertex_input *vin, nvk_pipeline *out);
int nvmtl_vk_cmd_vertex_inputs(nvk_cmdbuf *c, const nvk_pipeline *p, const uint32_t *vstride, uint32_t vset);
int  nvmtl_vk_pipeline_create_ex2(const void *vs, size_t vn, const void *fs, size_t fn, uint32_t vkfmt, uint32_t depthfmt, nvk_pipeline *out);
void nvmtl_vk_cmd_bind_pipeline(nvk_cmdbuf *c, nvk_pipeline *p);
int  nvmtl_vk_image_gen_mipmaps(nvk_queue *q, nvk_image *img);
int  nvmtl_vk_image_view_create(nvk_image *img, int bgra, uint32_t baseLevel, uint32_t levelCount, void **out_view);
void nvmtl_vk_image_view_destroy(void *view);
int  nvmtl_vk_bind_texture_view(nvk_cmdbuf *c, uint32_t set, uint32_t index, void *view);
int  nvmtl_vk_image_write_region_level(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes,
                                       uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level);
int  nvmtl_vk_image_read_region_level(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes,
                                      uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level);
int  nvmtl_vk_image_write_region(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes,
                                 uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  nvmtl_vk_image_read_region(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes,
                                uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  nvmtl_vk_copy_image(nvk_queue *q, nvk_image *src, uint32_t sx, uint32_t sy,
                         nvk_image *dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
int  nvmtl_vk_copy_buffer_to_image(nvk_queue *q, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                   nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h);
int  nvmtl_vk_copy_image_to_buffer(nvk_queue *q, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                   nvk_buffer *b, size_t off, uint32_t row_bytes);
void nvmtl_vk_cmd_draw_indirect(nvk_cmdbuf *c, nvk_buffer *b, size_t off);
void nvmtl_vk_cmd_draw_indexed_indirect(nvk_cmdbuf *c, nvk_buffer *idx, size_t idxOff, int is32, nvk_buffer *b, size_t off);
/* n draws whose arguments are stride bytes apart, as one vkCmdDraw*Indirect when multiDrawIndirect is on */
void nvmtl_vk_cmd_draw_indirect_n(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t n, uint32_t stride);
void nvmtl_vk_cmd_draw_indexed_indirect_n(nvk_cmdbuf *c, nvk_buffer *idx, size_t idxOff, int is32, nvk_buffer *b, size_t off,
                                          uint32_t n, uint32_t stride);
void nvmtl_vk_cmd_dispatch_indirect(nvk_cmdbuf *c, nvk_pipeline *p, nvk_buffer *b, size_t off);
void nvmtl_vk_cmd_barrier(nvk_cmdbuf *c);
void nvmtl_vk_buffer_destroy(nvk_buffer *b);
void nvmtl_vk_image_destroy(nvk_image *i);
int nvmtl_vk_defer_image_free(nvk_image *img, nvk_buffer *buf);   /* svfix2: free after the submissions made so far; 0 = not deferred */
// Finish any in-flight asynchronous texture upload to this image (NVMTL_ASYNC_TEX_UPLOAD) before its memory
// is read back, migrated or freed. A no-op when none is pending, which is always the case with the flag off.
void nvmtl_vk_image_upload_barrier(nvk_image *img);
void nvmtl_vk_queue_destroy(nvk_queue *q);
size_t nvmtl_vk_allocated_bytes(void);
uint64_t nvmtl_vk_buffer_address(nvk_buffer *b);

int nvmtl_vk_image_create_typed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_image *out);
typedef struct { void *mem; void *map; size_t size; uint32_t memtype; int host_visible; } nvk_heap;
int  nvmtl_vk_heap_create(size_t size, int host_visible, nvk_heap *out);
int  nvmtl_vk_heap_zero(nvk_heap *h, const char *what);   /* 5fcf1ad: clear a fresh heap before its first placement (NVMTL_HEAP_ZERO) */
int  nvmtl_vk_format_block(uint32_t vkfmt, uint32_t *bw, uint32_t *bh, uint32_t *block_bytes);
static inline uint32_t nvmtl_vk_layers_for_type(uint32_t t, uint32_t n) { n = n ? n : 1; return (t == 1 || t == 3 || t == 8) ? n : t == 5 ? 6u : t == 6 ? 6u * n : 1u; }
int nvmtl_vk_cmd_prepare_sampled_image(nvk_cmdbuf *c, nvk_image *img, int storage);
int  nvmtl_vk_image_view_create_range(nvk_image *img, uint32_t vkfmt, int a8, uint32_t baseLevel, uint32_t levelCount, uint32_t baseLayer, uint32_t layerCount, uint32_t mtl_view_type, void **out_view);
int  nvmtl_vk_image_view_create_range_aspect(nvk_image *img, uint32_t vkfmt, int a8, uint32_t aspect, uint32_t baseLevel, uint32_t levelCount, uint32_t baseLayer, uint32_t layerCount, uint32_t mtl_view_type, void **out_view);
int  nvmtl_vk_image_write_region_level_layer(nvk_queue *q, nvk_image *img, const void *src, size_t row_bytes, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer);
int  nvmtl_vk_image_read_region_level_layer(nvk_queue *q, nvk_image *img, void *dst, size_t row_bytes, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer);
int  nvmtl_vk_cmd_copy_image_sub(nvk_cmdbuf *c, nvk_image *src, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy, nvk_image *dst, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);
/* Level 0, layer 0, colour: copies with format conversion (an sRGB side is decoded on read and encoded on write). */
int  nvmtl_vk_cmd_blit_image_2d(nvk_cmdbuf *c, nvk_image *src, nvk_image *dst, uint32_t w, uint32_t h);
/* 1 when the VkFormat is an 8-bit sRGB colour format. */
int  nvmtl_vk_srgb_format(uint32_t fmt);
int  nvmtl_vk_cmd_copy_image_sub3(nvk_cmdbuf *c, nvk_image *src, uint32_t sl, uint32_t ss, uint32_t sx, uint32_t sy, uint32_t sz, nvk_image *dst, uint32_t dl, uint32_t ds, uint32_t dx, uint32_t dy, uint32_t dz, uint32_t w, uint32_t h, uint32_t depth);
int  nvmtl_vk_image_view_create_aspect(nvk_image *img, uint32_t vkfmt, uint32_t aspect, uint32_t baseLevel, uint32_t levelCount, void **out_view);
void nvmtl_vk_heap_destroy(nvk_heap *h);
int  nvmtl_vk_buffer_create_placed(size_t size, nvk_heap *heap, size_t offset, nvk_buffer *out);
int  nvmtl_vk_image_create_buffer_alias(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                        const nvk_buffer *b, size_t offset, size_t row_pitch, nvk_image *out);
int  nvmtl_vk_image_placed_fits(const nvk_image *img, size_t len);
int  nvmtl_vk_image_create_typed_placed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, uint32_t mips, int storage, uint32_t mtl_type, uint32_t layers, nvk_heap *heap, size_t offset, nvk_image *out);
int  nvmtl_vk_depth_create_ex_placed(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t aspect, nvk_heap *heap, size_t offset, nvk_image *out);
int  nvmtl_vk_buffer_size_align(size_t size, size_t *out_size, size_t *out_align);
int  nvmtl_vk_image_size_align(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t mips, uint32_t mtl_type, uint32_t layers, int depth_attachment, int storage, size_t *out_size, size_t *out_align);
int nvmtl_vk_copy_buffer_to_image_layer(nvk_queue *q, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer);
int nvmtl_vk_copy_image_to_buffer_layer(nvk_queue *q, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t layer);

int nvmtl_vk_cmd_fill_buffer(nvk_cmdbuf *c, nvk_buffer *b, size_t offset, size_t size, uint8_t value);
int nvmtl_vk_cmd_write_word(nvk_cmdbuf *c, nvk_buffer *b, size_t offset, uint32_t word);

int nvmtl_vk_cmd_copy_buffer(nvk_cmdbuf *c, nvk_buffer *src, size_t srcOff, nvk_buffer *dst, size_t dstOff, size_t size);

int nvmtl_vk_cmd_copy_image(nvk_cmdbuf *c, nvk_image *src, uint32_t sx, uint32_t sy,
                        nvk_image *dst, uint32_t dx, uint32_t dy, uint32_t w, uint32_t h);

void nvmtl_vk_copy_aspect_override(uint32_t aspect, uint32_t bpp);
int nvmtl_vk_cmd_copy_buffer_to_image_level_layer(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t row_bytes, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t level, uint32_t layer);
int nvmtl_vk_cmd_copy_image_to_buffer_level_layer(nvk_cmdbuf *c, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t level, uint32_t layer);
int nvmtl_vk_cmd_copy_buffer_to_image_layer(nvk_cmdbuf *c, nvk_buffer *b, size_t off, uint32_t row_bytes,
                                  nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t layer);

int nvmtl_vk_cmd_copy_image_to_buffer_layer(nvk_cmdbuf *c, nvk_image *img, uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                                  nvk_buffer *b, size_t off, uint32_t row_bytes, uint32_t layer);

int nvmtl_vk_cmd_image_gen_mipmaps(nvk_cmdbuf *c, nvk_image *img);

int nvmtl_vk_surface_share_on(void);
int nvmtl_vk_surface_vram(uint32_t surfaceID, uint32_t plane, size_t need, nvk_buffer *out);
/* the same with create = 0: only import a backing another process (or this one) made (B580); NVIDIA: as above */
int nvmtl_vk_surface_vram_ex(uint32_t surfaceID, uint32_t plane, size_t need, int create, nvk_buffer *out);
/* the layout of a backing its creator renders tiled (modifier 0: linear, the IOSurface's own row pitch) */
typedef struct { uint64_t modifier; uint32_t pitch, cpp, width, height; } nvmtl_vram_layout;
int nvmtl_vk_surface_vram_layout(uint32_t surfaceID, uint32_t plane, size_t need, int create, nvk_buffer *out, nvmtl_vram_layout *lay);
int nvmtl_vk_image_create_buffer_alias_tiled(uint32_t w, uint32_t h, uint32_t vkfmt, uint32_t bpp, int a8, int storage, int render,
                                             const nvk_buffer *b, uint64_t modifier, size_t pitch, nvk_image *out);
/* 1: B580 VRAM-shared IOSurfaces are on in this process (debug.intelxe.surfvram, Mesa 0014) */
int nvmtl_vk_anv_vram(void);
int nvmtl_vk_anv_level(void);
int nvmtl_vk_scan_newer(uint32_t sid, int newer);
/* 0 marked, 1 lock taken (try), -1 refused, -2 demoted (rb7f: a CPU-read surface; copy out into its pages instead) */
int nvmtl_vk_surface_dirty(uint32_t surfaceID, uint32_t plane);
int nvmtl_vk_surface_dirty_try(uint32_t surfaceID, uint32_t plane);
uint32_t nvmtl_vk_surface_gpu_mark(uint32_t surfaceID, int pending);
int nvmtl_vk_accel_surface_wait(void);
#include "nvmtl_sparse.h"   /* Metal sparse textures (NVMTL_SPARSE_TEXTURES, nvmtl_sparse_vk.c) */
