// SPDX-License-Identifier: MIT
/* NVMTL_ICB_DGC=1: GPU-encoded indirect command buffers through VK_EXT_device_generated_commands
   (docs/design/DESIGN-icb-dgc.md). Included at the end of nvmtl_vk.c (one translation unit): it uses the device, the
   memory helpers and the compute binding of that file. With the knob off the probe returns at once, nothing here runs
   and the device is created exactly as before.

   ANV exposes the extension for verx10 >= 125 (anv_physical_device.c), so on Xe2. Its limits that shape this file:
   maxIndirectCommandsTokenCount 32, at most 32 push-constant entries and 31 vertex buffers per layout (anv_types.h
   anv_dgc_push_layout / vertex_buffers), indirect execution sets for compute and ray tracing only
   (supportedIndirectCommandsShaderStagesPipelineBinding = COMPUTE | RT: a render pipeline cannot be switched from GPU
   memory), no DRAW_COUNT tokens (deviceGeneratedCommandsMultiDrawIndirectCount = false), and the sequences of one execute
   run in order unless the layout says UNORDERED_SEQUENCES (never asked here: blending needs the order). Without
   explicit preprocessing ANV runs its generation shader inside vkCmdExecuteGeneratedCommandsEXT (genX_cmd_dgc.c
   preprocess_gfx_sequences / preprocess_cs_sequences) followed by a CS stall, so each execute costs one small dispatch
   plus the generated commands. */

static int g_dgc;   /* VK_EXT_device_generated_commands is enabled on this device (NVMTL_ICB_DGC=1) */
static VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT g_dgc_feat;
static VkPhysicalDeviceMaintenance5FeaturesKHR g_dgc_m5;
static VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT g_dgc_props;
static PFN_vkCreateIndirectCommandsLayoutEXT p_dgc_layout;
static PFN_vkGetGeneratedCommandsMemoryRequirementsEXT p_dgc_memreq;
static PFN_vkCmdExecuteGeneratedCommandsEXT p_dgc_exec;

/* Before vkCreateDevice (called beside the other optional features): with the knob, the extension and maintenance5 (which it
   requires: the preprocess buffer's usage is a VkBufferUsageFlags2 bit) are added to exts and their features to *chain. */
void nvmtl_vk_icb_dgc_probe(const char **exts, uint32_t *next, uint32_t cap, void **chain)
{
    if (!nvmtl_icb_dgc_knob()) return;
    int have = 0, have_m5 = 0, listed_m5 = 0; uint32_t na = 0;
    if (pvkEnumerateDeviceExtensionProperties && pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &na, NULL) == VK_SUCCESS && na) {
        VkExtensionProperties *props = calloc(na, sizeof *props);
        if (props && pvkEnumerateDeviceExtensionProperties(g_pdev, NULL, &na, props) == VK_SUCCESS)
            for (uint32_t i = 0; i < na; i++) {
                if (!strcmp(props[i].extensionName, VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME)) have = 1;
                if (!strcmp(props[i].extensionName, "VK_KHR_maintenance5")) have_m5 = 1;
            }
        free(props);
    }
    for (uint32_t k = 0; k < *next; k++) if (!strcmp(exts[k], "VK_KHR_maintenance5")) listed_m5 = 1;
    if (!have || !have_m5 || *next + 2 > cap) {
        nvlog("icb-dgc: NVMTL_ICB_DGC=1 but %s - GPU-encoded ICBs keep the classic execute (their 1024-byte records still hold "
              "the new commands, which are refused)", !have ? "VK_EXT_device_generated_commands is absent" : !have_m5 ? "VK_KHR_maintenance5 is absent"
              : "the device extension list is full");
        return;
    }
    PFN_vkGetPhysicalDeviceFeatures2 gpf = (PFN_vkGetPhysicalDeviceFeatures2)pvkGetInstanceProcAddr(g_inst, "vkGetPhysicalDeviceFeatures2");
    VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT qf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT };
    VkPhysicalDeviceMaintenance5FeaturesKHR qm = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, &qf };
    VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &qm };
    if (gpf) gpf(g_pdev, &f2);
    g_dgc_props = (VkPhysicalDeviceDeviceGeneratedCommandsPropertiesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_PROPERTIES_EXT };
    VkPhysicalDeviceProperties2 p2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &g_dgc_props };
    if (pvkGetPhysicalDeviceProperties2) pvkGetPhysicalDeviceProperties2(g_pdev, &p2);
    if (!qf.deviceGeneratedCommands || !qm.maintenance5 || g_dgc_props.maxIndirectCommandsTokenCount < 2 ||
        g_dgc_props.maxIndirectCommandsIndirectStride < NVMTL_ICB_DGC_RECORD_BYTES || g_dgc_props.maxIndirectCommandsTokenOffset < NVMTL_ICB_DGC_RECORD_BYTES ||
        !(g_dgc_props.supportedIndirectCommandsInputModes & VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT)) {
        nvlog("icb-dgc: VK_EXT_device_generated_commands is present but unusable here (feature %u, maintenance5 %u, %u tokens, stride %u, "
              "offset %u, input modes %#x) - classic execute", qf.deviceGeneratedCommands, qm.maintenance5, g_dgc_props.maxIndirectCommandsTokenCount,
              g_dgc_props.maxIndirectCommandsIndirectStride, g_dgc_props.maxIndirectCommandsTokenOffset, g_dgc_props.supportedIndirectCommandsInputModes);
        return;
    }
    g_dgc_feat = (VkPhysicalDeviceDeviceGeneratedCommandsFeaturesEXT){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEVICE_GENERATED_COMMANDS_FEATURES_EXT, *chain, VK_TRUE, VK_FALSE };
    *chain = &g_dgc_feat;
    if (!listed_m5) {
        g_dgc_m5 = (VkPhysicalDeviceMaintenance5FeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MAINTENANCE_5_FEATURES_KHR, *chain, VK_TRUE };
        *chain = &g_dgc_m5;
        exts[(*next)++] = "VK_KHR_maintenance5";
    }
    exts[(*next)++] = VK_EXT_DEVICE_GENERATED_COMMANDS_EXTENSION_NAME;
    g_dgc = 1;
    nvlog("icb-dgc: VK_EXT_device_generated_commands asked (NVMTL_ICB_DGC=1): %u tokens, %u sequences, stages %#x, pipeline-bindable %#x",
          g_dgc_props.maxIndirectCommandsTokenCount, g_dgc_props.maxIndirectSequenceCount, g_dgc_props.supportedIndirectCommandsShaderStages,
          g_dgc_props.supportedIndirectCommandsShaderStagesPipelineBinding);
}
/* After vkCreateDevice: the entry points. */
void nvmtl_vk_icb_dgc_device_ready(void)
{
    if (!g_dgc) return;
    p_dgc_layout = (PFN_vkCreateIndirectCommandsLayoutEXT)pvkGetDeviceProcAddr(g_dev, "vkCreateIndirectCommandsLayoutEXT");
    p_dgc_memreq = (PFN_vkGetGeneratedCommandsMemoryRequirementsEXT)pvkGetDeviceProcAddr(g_dev, "vkGetGeneratedCommandsMemoryRequirementsEXT");
    p_dgc_exec = (PFN_vkCmdExecuteGeneratedCommandsEXT)pvkGetDeviceProcAddr(g_dev, "vkCmdExecuteGeneratedCommandsEXT");
    if (!p_dgc_layout || !p_dgc_memreq || !p_dgc_exec) g_dgc = 0;
    nvlog("icb-dgc: %s", g_dgc ? "ON - GPU-encodable ICBs execute through vkCmdExecuteGeneratedCommandsEXT (index buffers by address, buffers, "
                                 "compute ICBs)" : "entry points MISSING - classic execute");
}
int nvmtl_vk_icb_dgc_ready(void) { return g_dgc && g_state == 1; }
uint64_t nvmtl_vk_icb_dgc_dummy(void) { return nvmtl_dummy_addr(); }

/* The identity index buffer: uint32 i at element i. A plain draw executes as an indexed one through it (first index = first
   vertex, vertex offset 0: vertex_id is the same), so a run of slots needs one token layout whatever each slot draws.
   NVMTL_ICB_DGC_IOTA=<elements> sizes it (default 2M, 8 MB, VRAM through the BAR when it can); a plain draw that reaches
   past it is refused (the translator checks first vertex + count against word 30). */
static pthread_mutex_t g_dgc_iota_lock = PTHREAD_MUTEX_INITIALIZER;
static nvk_buffer g_dgc_iota; static uint32_t g_dgc_iota_n; static int g_dgc_iota_failed;
uint64_t nvmtl_vk_icb_dgc_iota(uint32_t *n)
{
    pthread_mutex_lock(&g_dgc_iota_lock);
    if (!g_dgc_iota.buf && !g_dgc_iota_failed) {
        const char *e = getenv("NVMTL_ICB_DGC_IOTA");
        uint32_t want = e && *e ? (uint32_t)strtoul(e, NULL, 0) : (1u << 21);
        if (want < 1024) want = 1024;
        if (want > (1u << 28)) want = 1u << 28;
        if (nvmtl_vk_buffer_create((size_t)want * 4, 3, &g_dgc_iota) || !g_dgc_iota.map) {
            if (g_dgc_iota.buf) nvmtl_vk_buffer_destroy(&g_dgc_iota);
            g_dgc_iota_failed = 1;
            nvlog("icb-dgc: the %u-element identity index buffer was REFUSED - plain draws in GPU-encodable ICBs are refused", want);
        } else {
            uint32_t *w = (uint32_t *)g_dgc_iota.map;
            for (uint32_t i = 0; i < want; i++) w[i] = i;
            g_dgc_iota_n = want;
            nvlog("icb-dgc: identity index buffer of %u elements at 0x%llx", want, (unsigned long long)nvmtl_vk_buffer_address(&g_dgc_iota));
        }
    }
    const uint64_t a = g_dgc_iota.buf ? nvmtl_vk_buffer_address(&g_dgc_iota) : 0;
    *n = a ? g_dgc_iota_n : 0;
    pthread_mutex_unlock(&g_dgc_iota_lock);
    return a;
}

/* ---- plans: the token layout of one pipeline (cached by what it is made of, never by a pointer that can be reused) ---- */
typedef struct { uint32_t type, offset, a, b, c; } nvmtl_dgc_tok;
typedef struct { void *layout; uint32_t cs, n; nvmtl_dgc_tok t[32]; struct nvmtl_dgc_plan_info info; } nvmtl_dgc_key;
struct nvmtl_dgc_plan { nvmtl_dgc_key key; VkIndirectCommandsLayoutEXT icl; int bad; struct nvmtl_dgc_plan *next; };   /* bad: the layout was refused */
static pthread_mutex_t g_dgc_plan_lock = PTHREAD_MUTEX_INITIALIZER;
static nvmtl_dgc_plan *g_dgc_plans;

/* Stage s of p and Metal buffer index m: 1 when a value from the record can be what the stage sees there - it does not read
   address-table entry m at all, or reads it through the pushed table (table-pc, *k = its push slot) and not through a
   64-aligned block load (glub: the promise is checked on the CPU per draw). 0 when it reads the entry from the
   descriptor table, which a DGC sequence cannot change. */
static int nvmtl_dgc_takes(const nvk_pipeline *p, uint32_t s, uint32_t m, int *k)
{
    *k = -1;
    const nvmtl_pc_table *pt = &p->pc_table[s];
    if (pt->count) {
        for (uint32_t j = 0; j < pt->count; j++) if (pt->index[j] == m) {
            if ((p->ubo_mask[s] >> j) & 1u) return 0;
            *k = (int)j; return 1;
        }
        return 1;   // a converted stage reads every table entry it uses from its push slots (nvmtl_table_pc.h)
    }
    const uint32_t need = p->tabneed[s];
    if (need == NVMTL_TABNEED(0, 0)) return 1;
    return need && m >= NVMTL_TABNEED_BUFS(need);
}
static void nvmtl_dgc_add(nvmtl_dgc_key *k, uint32_t type, uint32_t offset, uint32_t a, uint32_t b, uint32_t c)
{
    if (k->n < 32) k->t[k->n++] = (nvmtl_dgc_tok){ type, offset, a, b, c };
}
static const nvmtl_dgc_plan *nvmtl_dgc_plan_for(const nvmtl_dgc_key *key)
{
    pthread_mutex_lock(&g_dgc_plan_lock);
    nvmtl_dgc_plan *pl = g_dgc_plans;
    for (; pl; pl = pl->next) if (!memcmp(&pl->key, key, sizeof *key)) break;
    if (!pl && key->info.ok) {
        VkIndirectCommandsLayoutTokenEXT vt[32]; VkIndirectCommandsPushConstantTokenEXT pc[32];
        VkIndirectCommandsVertexBufferTokenEXT vb[32]; VkIndirectCommandsIndexBufferTokenEXT ib = { VK_INDIRECT_COMMANDS_INPUT_MODE_VULKAN_INDEX_BUFFER_EXT };
        for (uint32_t i = 0; i < key->n; i++) {
            const nvmtl_dgc_tok *t = &key->t[i];
            vt[i] = (VkIndirectCommandsLayoutTokenEXT){ VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_TOKEN_EXT, NULL, (VkIndirectCommandsTokenTypeEXT)t->type, { NULL }, t->offset };
            if (t->type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT) {
                pc[i] = (VkIndirectCommandsPushConstantTokenEXT){ { (VkShaderStageFlags)t->a, t->b, t->c } }; vt[i].data.pPushConstant = &pc[i];
            } else if (t->type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT) {
                vb[i] = (VkIndirectCommandsVertexBufferTokenEXT){ t->a }; vt[i].data.pVertexBuffer = &vb[i];
            } else if (t->type == VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT) vt[i].data.pIndexBuffer = &ib;
        }
        // ordered sequences (blending), implicit preprocessing inside the execute
        VkIndirectCommandsLayoutCreateInfoEXT ci = { VK_STRUCTURE_TYPE_INDIRECT_COMMANDS_LAYOUT_CREATE_INFO_EXT, NULL, 0,
            key->cs ? VK_SHADER_STAGE_COMPUTE_BIT : (VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT),
            NVMTL_ICB_DGC_RECORD_BYTES, (VkPipelineLayout)key->layout, key->n, vt };
        VkIndirectCommandsLayoutEXT icl = VK_NULL_HANDLE;
        const VkResult r = p_dgc_layout(g_dev, &ci, NULL, &icl);
        pl = calloc(1, sizeof *pl);
        if (pl) { pl->key = *key; pl->icl = icl; pl->bad = r != VK_SUCCESS; pl->next = g_dgc_plans; g_dgc_plans = pl; }
        nvlog("icb-dgc: %s token layout of %u token(s) (vertex buffers %#x, buffers a shader may set: vertex %#x, %s %#x) -> %d",
              key->cs ? "compute" : "render", key->n, key->info.vtok, key->info.vmask, key->cs ? "kernel" : "fragment", key->info.smask, r);
    }
    pthread_mutex_unlock(&g_dgc_plan_lock);
    return pl;
}
static uint32_t nvmtl_dgc_max_tokens(void)
{
    const uint32_t m = g_dgc_props.maxIndirectCommandsTokenCount;
    return m < 32 ? m : 32;
}
const nvmtl_dgc_plan *nvmtl_vk_icb_dgc_plan_gfx(nvk_cmdbuf *c, const nvk_pipeline *p, int buffers, struct nvmtl_dgc_plan_info *info)
{
    memset(info, 0, sizeof *info);
    if (!g_dgc || !p || !c) return NULL;
    // tessellation draws patches (refused in an ICB anyway); stencil-export emulation replays each draw once per bit, which
    // one execute cannot do; a pipeline whose draws are skipped for hang bisection stays skipped
    if (p->tess || c->semu_layout || p->skip_draws) return NULL;
    nvmtl_dgc_key key; memset(&key, 0, sizeof key);
    // the layout bound with the pipeline (a sampler or GL-UBO variant of p may be what is bound; same push slots)
    key.layout = c->graphics_layout ? c->graphics_layout : p->layout; key.cs = 0;
    nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_INDEX_BUFFER_EXT, NVMTL_ICB_W_IBT * 4, 0, 0, 0);
    uint32_t vm = 0, sm = 0, vtok = 0;
    if (buffers) {
        uint32_t take[2] = { 0, 0 }, push[2] = { 0, 0 }; int slot[2][NVMTL_ICB_MAX_BUFFERS];
        for (uint32_t s = 0; s < 2; s++)
            for (uint32_t m = 0; m < NVMTL_ICB_MAX_BUFFERS; m++) {
                if (nvmtl_dgc_takes(p, s, m, &slot[s][m])) take[s] |= 1u << m;
                if (slot[s][m] >= 0) push[s] |= 1u << m;
            }
        const uint32_t vin = p->vin_mask & ((1u << NVMTL_ICB_MAX_BUFFERS) - 1u);
        const uint32_t room = nvmtl_dgc_max_tokens() - 2;   // the index buffer and draw tokens
        const uint32_t npush = (uint32_t)__builtin_popcount(take[0] & push[0]) + (uint32_t)__builtin_popcount(take[1] & push[1]);
        // vertex fetch: every binding the pipeline fetches from through a token, or none (the CPU binds them)
        if ((vin & ~take[0]) == 0 && npush + (uint32_t)__builtin_popcount(vin) <= room) vtok = vin;
        if (npush + (uint32_t)__builtin_popcount(vtok) <= room) {
            // offered: what each stage takes, less the fetched bindings the CPU binds; a push slot gets a token only for an
            // offered index (any other keeps the value the CPU pushed)
            vm = take[0] & ~(vin & ~vtok); sm = take[1];
            for (uint32_t m = 0; m < NVMTL_ICB_MAX_BUFFERS; m++)
                if (vtok >> m & 1u) nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_VERTEX_BUFFER_EXT, (NVMTL_ICB_W_VB + 4 * m) * 4, m, 0, 0);
            for (uint32_t m = 0; m < NVMTL_ICB_MAX_BUFFERS; m++) {
                if ((vm & push[0]) >> m & 1u)
                    nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT, (NVMTL_ICB_W_VB + 4 * m) * 4,
                                  VK_SHADER_STAGE_VERTEX_BIT, NVMTL_PC_TABLE_VS_OFFSET + 8u * (uint32_t)slot[0][m], 8);
                if ((sm & push[1]) >> m & 1u)
                    nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT, (NVMTL_ICB_W_SB + 2 * m) * 4,
                                  VK_SHADER_STAGE_FRAGMENT_BIT, NVMTL_PC_TABLE_FS_OFFSET + 8u * (uint32_t)slot[1][m], 8);
            }
        } else {
            // too many tokens: only indices no stage reads (a shader may set them, nothing sees them)
            vm = take[0] & ~push[0] & ~vin; sm = take[1] & ~push[1];
        }
    }
    nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_DRAW_INDEXED_EXT, NVMTL_ICB_W_IDX * 4, 0, 0, 0);
    key.info = (struct nvmtl_dgc_plan_info){ 1, vm, sm, vtok };
    const nvmtl_dgc_plan *pl = nvmtl_dgc_plan_for(&key);
    if (pl && !pl->bad) { *info = pl->key.info; return pl; }
    return NULL;
}
const nvmtl_dgc_plan *nvmtl_vk_icb_dgc_plan_cs(const nvk_pipeline *p, int buffers, struct nvmtl_dgc_plan_info *info)
{
    memset(info, 0, sizeof *info);
    if (!p) return NULL;
    uint32_t take = 0, push = 0; int slot[NVMTL_ICB_MAX_BUFFERS];
    for (uint32_t m = 0; m < NVMTL_ICB_MAX_BUFFERS; m++) {
        if (nvmtl_dgc_takes(p, 0, m, &slot[m])) take |= 1u << m;
        if (slot[m] >= 0) push |= 1u << m;
    }
    if (!buffers) return NULL;
    // indices the kernel never reads may be set whatever runs it; pushed ones need DGC (an align-guarded kernel may run its
    // SAFE twin, another module with another table: not through DGC)
    info->smask = take & ~push;
    const uint32_t npush = (uint32_t)__builtin_popcount(take & push);
    if (!g_dgc || !npush || p->ag_mask || npush + 1 > nvmtl_dgc_max_tokens()) return NULL;
    nvmtl_dgc_key key; memset(&key, 0, sizeof key);
    key.layout = p->layout; key.cs = 1;
    for (uint32_t m = 0; m < NVMTL_ICB_MAX_BUFFERS; m++)
        if ((take & push) >> m & 1u)
            nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_PUSH_CONSTANT_EXT, (NVMTL_ICB_W_SB + 2 * m) * 4,
                          VK_SHADER_STAGE_COMPUTE_BIT, NVMTL_PC_TABLE_CS_OFFSET + 8u * (uint32_t)slot[m], 8);
    nvmtl_dgc_add(&key, VK_INDIRECT_COMMANDS_TOKEN_TYPE_DISPATCH_EXT, NVMTL_ICB_W_DISPATCH * 4, 0, 0, 0);
    key.info = (struct nvmtl_dgc_plan_info){ 1, 0, take, 0 };
    const nvmtl_dgc_plan *pl = nvmtl_dgc_plan_for(&key);
    if (pl && !pl->bad) { *info = pl->key.info; return pl; }
    return NULL;
}

/* ---- preprocess memory: sub-allocated from 1 MB+ buffers, held by the command buffer that used them ---- */
typedef struct { VkBuffer buf; VkDeviceMemory mem; VkDeviceSize size; uint64_t addr; uint32_t mtype; } nvmtl_dgc_buf;
struct nvmtl_dgc_arena { nvmtl_dgc_buf *b; unsigned n, cap; VkDeviceSize used; };
static pthread_mutex_t g_dgc_free_lock = PTHREAD_MUTEX_INITIALIZER;
static nvmtl_dgc_buf g_dgc_free[16]; static unsigned g_dgc_nfree;
static void nvmtl_dgc_buf_destroy(nvmtl_dgc_buf *b)
{
    if (g_state != 1) return;
    if (b->buf) pvkDestroyBuffer(g_dev, b->buf, NULL);
    if (b->mem) pvkFreeMemory(g_dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}
static int nvmtl_dgc_buf_make(VkDeviceSize size, uint32_t bits, nvmtl_dgc_buf *out)
{
    memset(out, 0, sizeof *out);
    pthread_mutex_lock(&g_dgc_free_lock);
    for (unsigned i = 0; i < g_dgc_nfree; i++)
        if (g_dgc_free[i].size >= size && (bits >> g_dgc_free[i].mtype & 1u)) { *out = g_dgc_free[i]; g_dgc_free[i] = g_dgc_free[--g_dgc_nfree]; break; }
    pthread_mutex_unlock(&g_dgc_free_lock);
    if (out->buf) return 0;
    VkDeviceSize sz = 1u << 20; while (sz < size) sz <<= 1;
    VkBufferUsageFlags2CreateInfoKHR u2 = { VK_STRUCTURE_TYPE_BUFFER_USAGE_FLAGS_2_CREATE_INFO_KHR, NULL,
        VK_BUFFER_USAGE_2_PREPROCESS_BUFFER_BIT_EXT | VK_BUFFER_USAGE_2_SHADER_DEVICE_ADDRESS_BIT_KHR };
    VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &u2, 0, sz, 0, VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    if (pvkCreateBuffer(g_dev, &bci, NULL, &out->buf) != VK_SUCCESS) { out->buf = VK_NULL_HANDLE; return -1; }
    VkMemoryRequirements mr; pvkGetBufferMemoryRequirements(g_dev, out->buf, &mr);
    int mt = nvmtl_vk_buffer_vram_type(mr.memoryTypeBits & bits);
    if (mt < 0) mt = memtype(mr.memoryTypeBits & bits, 0);
    VkMemoryAllocateFlagsInfo fi = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO, NULL, VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT, 0 };
    VkMemoryAllocateInfo ai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &fi, mr.size, (uint32_t)mt };
    if (mt < 0 || pvkAllocateMemory(g_dev, &ai, NULL, &out->mem) != VK_SUCCESS || pvkBindBufferMemory(g_dev, out->buf, out->mem, 0) != VK_SUCCESS) {
        nvlog("icb-dgc: a %llu-byte preprocess buffer was REFUSED (memory type %d)", (unsigned long long)sz, mt);
        nvmtl_dgc_buf_destroy(out); return -1;
    }
    VkBufferDeviceAddressInfo dai = { VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO, NULL, out->buf };
    out->addr = pvkGetBufferDeviceAddress(g_dev, &dai); out->size = sz; out->mtype = (uint32_t)mt;
    return out->addr ? 0 : (nvmtl_dgc_buf_destroy(out), -1);
}
static int nvmtl_dgc_arena_take(nvmtl_dgc_arena **pa, VkDeviceSize size, VkDeviceSize align, uint32_t bits, uint64_t *addr)
{
    if (!*pa && !(*pa = calloc(1, sizeof **pa))) return -1;
    nvmtl_dgc_arena *a = *pa;
    if (align < 64) align = 64;
    if (a->n) {
        nvmtl_dgc_buf *cur = &a->b[a->n - 1];
        const VkDeviceSize at = (a->used + align - 1) & ~(align - 1);
        if ((bits >> cur->mtype & 1u) && at + size <= cur->size) { *addr = cur->addr + at; a->used = at + size; return 0; }
    }
    if (a->n == a->cap) {
        nvmtl_dgc_buf *nb = realloc(a->b, (a->cap ? a->cap * 2 : 4) * sizeof *nb);
        if (!nb) return -1;
        a->b = nb; a->cap = a->cap ? a->cap * 2 : 4;
    }
    if (nvmtl_dgc_buf_make(size, bits, &a->b[a->n])) return -1;
    *addr = a->b[a->n].addr; a->used = size; a->n++;
    return 0;
}
void nvmtl_vk_icb_dgc_arena_release(nvmtl_dgc_arena *a)
{
    if (!a) return;
    for (unsigned i = 0; i < a->n; i++) {
        pthread_mutex_lock(&g_dgc_free_lock);
        const int kept = g_dgc_nfree < sizeof g_dgc_free / sizeof g_dgc_free[0];
        if (kept) g_dgc_free[g_dgc_nfree++] = a->b[i];
        pthread_mutex_unlock(&g_dgc_free_lock);
        if (!kept) nvmtl_dgc_buf_destroy(&a->b[i]);
    }
    free(a->b); free(a);
}
/* the preprocess range for n sequences of plan pl with pipeline info pi chained */
static int nvmtl_dgc_prep(const VkGeneratedCommandsPipelineInfoEXT *pi, const nvmtl_dgc_plan *pl, uint32_t n, nvmtl_dgc_arena **arena,
                          uint64_t *addr, VkDeviceSize *size)
{
    VkGeneratedCommandsMemoryRequirementsInfoEXT mi = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_MEMORY_REQUIREMENTS_INFO_EXT, pi, VK_NULL_HANDLE, pl->icl, n, 1 };
    VkMemoryRequirements2 mr = { VK_STRUCTURE_TYPE_MEMORY_REQUIREMENTS_2 };
    p_dgc_memreq(g_dev, &mi, &mr);
    *size = mr.memoryRequirements.size; *addr = 0;
    if (!*size) return 0;
    if (nvmtl_dgc_arena_take(arena, *size, mr.memoryRequirements.alignment, mr.memoryRequirements.memoryTypeBits, addr)) {
        static int said; if (said++ < 4) nvlog("icb-dgc: no preprocess memory (%llu bytes) - the execute is SKIPPED", (unsigned long long)*size);
        return -1;
    }
    return 0;
}

int nvmtl_vk_cmd_icb_dgc_draw(nvk_cmdbuf *c, const nvmtl_dgc_plan *pl, nvk_buffer *rec, size_t off, uint32_t n, uint32_t stride,
                              nvmtl_dgc_arena **arena)
{
    if (!pl || !g_dgc || !c->graphics_pipeline) return -1;
    if (c->nobind || c->skip_group || c->dead || g_state != 1 || !n) return 0;
    VkGeneratedCommandsPipelineInfoEXT pi = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT, NULL, (VkPipeline)c->graphics_pipeline };
    uint64_t pa = 0; VkDeviceSize ps = 0;
    if (nvmtl_dgc_prep(&pi, pl, n, arena, &pa, &ps)) return -1;
    const VkGeneratedCommandsInfoEXT gi = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, &pi, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        VK_NULL_HANDLE, pl->icl, nvmtl_vk_buffer_address(rec) + off, (VkDeviceSize)n * stride, pa, ps, n, 0, 1 };
    p_dgc_exec(c->cb, VK_FALSE, &gi);
    // the index buffer, vertex buffers and push constants the tokens set are undefined after the execute: the next draw
    // binds and pushes them again instead of trusting the caches (nvk_cmdbuf ibc_* / vbc_* / pc_table_*)
    c->ibc_cb = NULL; c->vbc_cb = NULL; c->vbc_valid = 0; c->pc_table_valid = 0;
    if (g_dbuf) nvmtl_db_forget_offsets(c);
    return 0;
}

/* nvmtl_vk_cmd_dispatch_indirect_tg with the dispatch (and the kernel buffers a shader set) taken through DGC */
int nvmtl_vk_cmd_icb_dgc_dispatch(nvk_cmdbuf *c, nvk_pipeline *p, const nvmtl_dgc_plan *pl, nvk_buffer *rec, size_t off,
                                  const uint32_t tgIn[3], nvmtl_dgc_arena **arena)
{
    if (!pl || !g_dgc || p->ag_mask) return -1;
    uint32_t tg[3];
    for (int d = 0; d < 3; d++) { if (!tgIn[d]) return -1; tg[d] = tgIn[d]; }
    VkPipeline vp = nvmtl_compute_variant(p, tg);
    if (!vp) { c->skipped++; return 0; }
    const uint64_t dt0 = nvmtl_dstat_t0();
    if (g_dbuf) {   // the descriptor-buffer model's regions and offsets (DGC keeps descriptor-buffer offsets; ANV: anv_dgc_layout.c)
        if (nvmtl_db_bind_compute(c, p)) { nvmtl_dstat_bind(dt0, NVMTL_DS_DISP, 0); return 0; }
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vp);
    } else {
        VkDescriptorSet s[3] = { nvmtl_snapshot_set_c(c, 0, p), nvmtl_snapshot_set_c(c, 1, p), g_heapSet };
        if (!s[0] || !s[1]) { c->nobind = 1; nvmtl_dstat_bind(dt0, NVMTL_DS_DISP, 0); return 0; }
        c->nobind = 0;
        pvkCmdBindPipeline(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, vp);
        pvkCmdBindDescriptorSets(c->cb, VK_PIPELINE_BIND_POINT_COMPUTE, (VkPipelineLayout)p->layout, 0, g_heapSet ? 3 : 2, s, nvmtl_table_ubo() ? 2 : 0, c->dyn_out);
    }
    nvmtl_dstat_bind(dt0, NVMTL_DS_DISP, 1);
    // the grid from NumWorkgroups, as an indirect dispatch (translator stage_input/kernel_grid.rs); the CPU's table qwords
    // are the template the push-constant tokens of the GPU-set buffers overwrite
    const uint32_t F = 0x80000000u;
    uint32_t pc[12 + 2 * NVMTL_PC_TABLE_MAX] = { F | tg[0], F | tg[1], F | tg[2], 0, 0, 0, 0, 0, 0, F, F, F };
    const uint32_t pcb = nvmtl_cs_pc_values(c, p, pc);
    pvkCmdPushConstants(c->cb, (VkPipelineLayout)p->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pcb, pc);
    if (c->dead || g_state != 1) return 0;
    VkGeneratedCommandsPipelineInfoEXT pi = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_PIPELINE_INFO_EXT, NULL, vp };
    uint64_t pa = 0; VkDeviceSize ps = 0;
    if (nvmtl_dgc_prep(&pi, pl, 1, arena, &pa, &ps)) return 0;
    const VkGeneratedCommandsInfoEXT gi = { VK_STRUCTURE_TYPE_GENERATED_COMMANDS_INFO_EXT, &pi, VK_SHADER_STAGE_COMPUTE_BIT, VK_NULL_HANDLE, pl->icl,
        nvmtl_vk_buffer_address(rec) + off, NVMTL_ICB_DGC_RECORD_BYTES, pa, ps, 1, 0, 1 };
    nvmtl_kt_begin(c, p, tg, 0);
    p_dgc_exec(c->cb, VK_FALSE, &gi);
    if (g_dbuf) nvmtl_db_forget_offsets(c);
    nvmtl_order_barrier(c);
    nvmtl_kt_end(c);
    return 0;
}
