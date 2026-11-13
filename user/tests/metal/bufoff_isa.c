// SPDX-License-Identifier: MIT
// bufoff_isa: what the storage-buffer offset pass (nullmoth/plugin/nvmtl_bufoff_pc.h) costs in Intel ISA, off the GPU.
// Each pair <name>.pc.spv / <name>.bo.spv (bufoff_check's outputs: the kernel after cs-table-pc, and after the pass) is
// compiled by Mesa ANV on its XEMAC_FAKE=1 Battlemage G21 (Xe2) device the way the plugin compiles it in the
// descriptor-buffer model (VK_EXT_descriptor_buffer set layouts, VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT, the
// five-binding heap in set 2, robustBufferAccess on, one push range [0, end of the module's push block)), with
// VK_KHR_pipeline_executable_properties statistics. -c also compiles the .pc.spv in the classic model (plain set
// layouts). Prints per kernel: SIMD width, instructions, SENDs, cycles, GRF, spills for each, whether ANV reads the push
// data from memory (its "Shader push map": a push range) or keeps it in the inline registers, and the plugin's own
// estimate of the push dwords (nvmtl_bo_push_dwords, the fit rule of NVMTL_DB_BUFOFF=fit); a summary with the sums and how
// often the estimate's verdict (>= NVMTL_BO_INLINE_DWORDS: memory) disagrees with ANV's.
//
//   clang -arch x86_64 -O1 -Wall -I $MESA_INCLUDE -I nullmoth/plugin -o bufoff_isa user/tests/metal/bufoff_isa.c
//   XEMAC_FAKE=1 BOI_ICD=<mesa build>/src/intel/vulkan/libvulkan_intel.dylib ./bufoff_isa [-c] [-a] <dir-or-.pc.spv>...
// -a: print the brw assembly of both to stderr (VK_KHR_pipeline_executable_properties internal representations).
// BOI_TG: the local size x (default 64; the spec constant 0, as the plugin specialises it).
#include <dirent.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <vulkan/vulkan.h>
#include "nvmtl_bufoff_pc.h"

static PFN_vkGetInstanceProcAddr gipa; static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst; static VkPhysicalDevice pdev; static VkDevice dev;
#define D(n) static PFN_##n p##n;
D(vkCreateShaderModule) D(vkDestroyShaderModule) D(vkCreateDescriptorSetLayout) D(vkDestroyDescriptorSetLayout)
D(vkCreatePipelineLayout) D(vkDestroyPipelineLayout) D(vkCreateComputePipelines) D(vkDestroyPipeline)
D(vkGetPipelineExecutablePropertiesKHR) D(vkGetPipelineExecutableStatisticsKHR) D(vkGetPipelineExecutableInternalRepresentationsKHR)
#undef D

static int vk_init(const char *icd)
{
    void *h = dlopen(icd, RTLD_NOW | RTLD_LOCAL); if (!h) { fprintf(stderr, "dlopen %s: %s\n", icd, dlerror()); return -1; }
    gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
    if (!gipa) gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
    if (!gipa) return -1;
    PFN_vkCreateInstance ci = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "bufoff_isa", 1, "NVMTL", 1, VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
    if (ci(&ici, NULL, &inst)) return -1;
    uint32_t n = 1; ((PFN_vkEnumeratePhysicalDevices)gipa(inst, "vkEnumeratePhysicalDevices"))(inst, &n, &pdev);
    if (!n) { fprintf(stderr, "no device (XEMAC_FAKE=1?)\n"); return -1; }
    static VkPhysicalDeviceDescriptorBufferFeaturesEXT dbf = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_BUFFER_FEATURES_EXT };
    static VkPhysicalDevicePipelineExecutablePropertiesFeaturesKHR pef = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PIPELINE_EXECUTABLE_PROPERTIES_FEATURES_KHR, &dbf };
    static VkPhysicalDeviceVulkan13Features f13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &pef };
    static VkPhysicalDeviceVulkan12Features f12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &f13 };
    static VkPhysicalDeviceVulkan11Features f11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &f12 };
    static VkPhysicalDeviceFeatures2 f2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &f11 };
    ((PFN_vkGetPhysicalDeviceFeatures2)gipa(inst, "vkGetPhysicalDeviceFeatures2"))(pdev, &f2);
    if (!dbf.descriptorBuffer) { fprintf(stderr, "the device has no descriptorBuffer\n"); return -1; }
    dbf.descriptorBufferCaptureReplay = dbf.descriptorBufferImageLayoutIgnored = dbf.descriptorBufferPushDescriptors = VK_FALSE;
    f2.features.robustBufferAccess = VK_TRUE;   // as the plugin
    f13.robustImageAccess = VK_FALSE;
    const char *exts[2] = { "VK_KHR_pipeline_executable_properties", "VK_EXT_descriptor_buffer" };
    float prio = 1; VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, 0, 1, &prio };
    VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &f2, 0, 1, &qci, 0, NULL, 2, exts, NULL };
    VkResult r = ((PFN_vkCreateDevice)gipa(inst, "vkCreateDevice"))(pdev, &dci, NULL, &dev);
    if (r) { fprintf(stderr, "vkCreateDevice %d\n", r); return -1; }
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
#define L(n) p##n = (PFN_##n)gdpa(dev, #n);
    L(vkCreateShaderModule) L(vkDestroyShaderModule) L(vkCreateDescriptorSetLayout) L(vkDestroyDescriptorSetLayout)
    L(vkCreatePipelineLayout) L(vkDestroyPipelineLayout) L(vkCreateComputePipelines) L(vkDestroyPipeline)
    L(vkGetPipelineExecutablePropertiesKHR) L(vkGetPipelineExecutableStatisticsKHR) L(vkGetPipelineExecutableInternalRepresentationsKHR)
#undef L
    return 0;
}

/* the descriptor bindings a module declares in sets 0, 1 and 3+ (set 2 is the heap), and the end of its push block */
typedef struct { uint32_t set, binding; VkDescriptorType type; } refl;
static int reflect(const uint32_t *w, size_t nw, refl *out, int max, uint32_t *push_end, uint32_t *maxset)
{
    const uint32_t bound = w[3];
    uint32_t *def = calloc(bound, 4), *set = calloc(bound, 4), *bnd = calloc(bound, 4), *mof = NULL;
    uint8_t *hs = calloc(bound, 1), *hb = calloc(bound, 1);
    int n = 0; *push_end = 0; *maxset = 0;
    if (!def || !set || !bnd || !hs || !hb) { n = -1; goto out; }
    for (size_t i = 5; i < nw;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (!wc || i + wc > nw) { n = -1; goto out; }
        if (op == 71 && wc >= 4 && w[i + 1] < bound && w[i + 2] == 34) { set[w[i + 1]] = w[i + 3]; hs[w[i + 1]] = 1; }
        if (op == 71 && wc >= 4 && w[i + 1] < bound && w[i + 2] == 33) { bnd[w[i + 1]] = w[i + 3]; hb[w[i + 1]] = 1; }
        if (op >= 19 && op <= 39 && wc >= 2 && w[i + 1] < bound) def[w[i + 1]] = (uint32_t)i;
        if ((op == 43 || op == 59) && wc >= 3 && w[i + 2] < bound) def[w[i + 2]] = (uint32_t)i;
        if (op == 5341 && w[i + 1] < bound) def[w[i + 1]] = (uint32_t)i;
        i += wc;
    }
    for (size_t i = 5; i < nw;) {
        const uint32_t wc = w[i] >> 16, op = w[i] & 0xffff;
        if (op == 59 && wc >= 4 && w[i + 3] == 9) {   // the push block's extent
            const uint32_t *pt = w + def[w[i + 1]], blk = pt[3], *st = w + def[blk];
            for (size_t j = 5; j < nw;) {
                const uint32_t jc = w[j] >> 16, jo = w[j] & 0xffff;
                if (jo == 72 && jc == 5 && w[j + 1] == blk && w[j + 3] == 35) {
                    const uint32_t *mt = w + def[st[2 + w[j + 2]]]; uint32_t sz = 4;
                    if ((mt[0] & 0xffff) == 21 || (mt[0] & 0xffff) == 22) sz = mt[2] / 8;
                    else if ((mt[0] & 0xffff) == 23) { const uint32_t *c = w + def[mt[2]]; sz = c[2] / 8 * mt[3]; }
                    if (w[j + 4] + sz > *push_end) *push_end = w[j + 4] + sz;
                }
                j += jc;
            }
        }
        if (op == 59 && wc >= 4 && w[i + 2] < bound && hs[w[i + 2]] && hb[w[i + 2]]) {
            const uint32_t v = w[i + 2], sc = w[i + 3];
            const uint32_t *pt = w + def[w[i + 1]], *t = w + def[pt[3]];
            if (set[v] == 2) { i += wc; continue; }   // the heap
            if ((t[0] & 0xffff) == 28 || (t[0] & 0xffff) == 29) { n = -1; goto out; }
            VkDescriptorType ty;
            if (sc == 12) ty = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            else if (sc == 2) ty = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            else if ((t[0] & 0xffff) == 25) ty = t[3] == 5 ? (t[7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER)
                                                        : (t[7] == 2 ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE);
            else if ((t[0] & 0xffff) == 26) ty = VK_DESCRIPTOR_TYPE_SAMPLER;
            else if ((t[0] & 0xffff) == 5341) ty = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            else { n = -1; goto out; }
            int dup = 0;
            for (int k = 0; k < n; k++) if (out[k].set == set[v] && out[k].binding == bnd[v]) dup = 1;
            if (!dup && n < max) { out[n++] = (refl){ set[v], bnd[v], ty }; if (set[v] > *maxset) *maxset = set[v]; }
        }
        i += wc;
    }
out:
    free(def); free(set); free(bnd); free(hs); free(hb); free(mof);
    return n;
}

/* ok; the SIMD width; brw's statistics; from ANV's "Shader push map": the dwords promoted to inline data (driver dwords
   from 64 on), and mem = 1 when the push data is read from memory (a push range: two inline dwords are its address) */
typedef struct { int ok; unsigned simd; long long inst, sends, spills, fills, cycles, grf; int mem, ninl, ndrv; } stats;
static VkDescriptorSetLayout heap_layout(int buffer)
{
    static VkDescriptorSetLayout h[2];
    if (h[buffer]) return h[buffer];
    const VkDescriptorSetLayoutBinding hb[5] = {
        { 0, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 131072, VK_SHADER_STAGE_ALL, NULL },
        { 1, VK_DESCRIPTOR_TYPE_SAMPLER, 2048, VK_SHADER_STAGE_ALL, NULL },
        { 2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 131072, VK_SHADER_STAGE_ALL, NULL },
        { 3, VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 131072, VK_SHADER_STAGE_ALL, NULL },
        { 4, VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 131072, VK_SHADER_STAGE_ALL, NULL } };
    VkDescriptorBindingFlags hf = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    const VkDescriptorBindingFlags hfs[5] = { hf, hf, hf, hf, hf };
    VkDescriptorSetLayoutBindingFlagsCreateInfo hfi = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, NULL, 5, hfs };
    VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, buffer ? NULL : &hfi,
        buffer ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT, 5, hb };
    if (pvkCreateDescriptorSetLayout(dev, &li, NULL, &h[buffer])) h[buffer] = VK_NULL_HANDLE;
    return h[buffer];
}
static stats compile(const uint32_t *w, size_t nbytes, int buffer, uint32_t tg, int asm_out, const char *tag)
{
    stats st = {0};
    refl r[128]; uint32_t push_end = 0, maxset = 0;
    const int nr = reflect(w, nbytes / 4, r, 128, &push_end, &maxset);
    if (nr < 0) return st;
    const uint32_t nset = maxset < 2 ? 3 : maxset + 1;
    VkDescriptorSetLayout sl[8] = {0};
    if (nset > 8) return st;
    for (uint32_t s = 0; s < nset; s++) {
        if (s == 2) { sl[s] = heap_layout(buffer); continue; }
        VkDescriptorSetLayoutBinding b[128]; uint32_t nb = 0;
        for (int k = 0; k < nr; k++) if (r[k].set == s) b[nb++] = (VkDescriptorSetLayoutBinding){ r[k].binding, r[k].type, 1, VK_SHADER_STAGE_ALL, NULL };
        VkDescriptorSetLayoutCreateInfo li = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, NULL,
            buffer ? VK_DESCRIPTOR_SET_LAYOUT_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0, nb, nb ? b : NULL };
        if (pvkCreateDescriptorSetLayout(dev, &li, NULL, &sl[s])) return st;
    }
    VkPushConstantRange pcr = { VK_SHADER_STAGE_COMPUTE_BIT, 0, push_end > 48 ? (push_end + 3u) & ~3u : 48 };
    VkPipelineLayoutCreateInfo pli = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, NULL, 0, nset, sl, 1, &pcr };
    VkPipelineLayout pl = VK_NULL_HANDLE; pvkCreatePipelineLayout(dev, &pli, NULL, &pl);
    VkShaderModuleCreateInfo smi = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, NULL, 0, nbytes, w };
    VkShaderModule m = VK_NULL_HANDLE;
    if (!pl || pvkCreateShaderModule(dev, &smi, NULL, &m)) goto out;
    uint32_t sd[4] = { tg, 1, 1, 0 }; VkSpecializationMapEntry me[4] = { { 0, 0, 4 }, { 1, 4, 4 }, { 2, 8, 4 }, { 3, 12, 4 } };
    VkSpecializationInfo si = { 4, me, 16, sd };
    VkComputePipelineCreateInfo cpi = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, NULL,
        VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR | VK_PIPELINE_CREATE_CAPTURE_INTERNAL_REPRESENTATIONS_BIT_KHR |
        (buffer ? VK_PIPELINE_CREATE_DESCRIPTOR_BUFFER_BIT_EXT : 0),
        { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, NULL, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", &si }, pl, 0, -1 };
    VkPipeline p = VK_NULL_HANDLE;
    if (pvkCreateComputePipelines(dev, VK_NULL_HANDLE, 1, &cpi, NULL, &p) || !p) goto out;
    {
        VkPipelineInfoKHR pi = { VK_STRUCTURE_TYPE_PIPELINE_INFO_KHR, NULL, p };
        VkPipelineExecutablePropertiesKHR ep[4]; uint32_t ne = 4;
        for (uint32_t k = 0; k < 4; k++) ep[k] = (VkPipelineExecutablePropertiesKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_PROPERTIES_KHR };
        pvkGetPipelineExecutablePropertiesKHR(dev, &pi, &ne, ep);
        VkPipelineExecutableInfoKHR ei = { VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INFO_KHR, NULL, p, 0 };
        VkPipelineExecutableStatisticKHR sx[64]; uint32_t ns = 64;
        for (uint32_t k = 0; k < 64; k++) sx[k] = (VkPipelineExecutableStatisticKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_STATISTIC_KHR };
        pvkGetPipelineExecutableStatisticsKHR(dev, &ei, &ns, sx);
        st.ok = 1; st.simd = ne ? ep[0].subgroupSize : 0;
        for (uint32_t k = 0; k < ns; k++) {
            const long long v = sx[k].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_UINT64_KHR ? (long long)sx[k].value.u64 :
                                sx[k].format == VK_PIPELINE_EXECUTABLE_STATISTIC_FORMAT_INT64_KHR ? sx[k].value.i64 : 0;
            const char *nm = sx[k].name;
            if (!strcmp(nm, "Instructions")) st.inst = v;
            else if (!strcmp(nm, "SENDs")) st.sends = v;
            else if (!strcmp(nm, "Spills")) st.spills = v;
            else if (!strcmp(nm, "Fills")) st.fills = v;
            else if (!strcmp(nm, "Cycles")) st.cycles = v;
            else if (!strcmp(nm, "GRF registers")) st.grf = v;
        }
        {
            VkPipelineExecutableInternalRepresentationKHR ir[8]; uint32_t ni = 8;
            for (uint32_t k = 0; k < 8; k++) ir[k] = (VkPipelineExecutableInternalRepresentationKHR){ VK_STRUCTURE_TYPE_PIPELINE_EXECUTABLE_INTERNAL_REPRESENTATION_KHR };
            if (pvkGetPipelineExecutableInternalRepresentationsKHR(dev, &ei, &ni, ir) >= 0) {
                for (uint32_t k = 0; k < ni; k++) ir[k].pData = calloc(1, ir[k].dataSize + 1);
                if (pvkGetPipelineExecutableInternalRepresentationsKHR(dev, &ei, &ni, ir) >= 0)
                    for (uint32_t k = 0; k < ni; k++) {
                        const char *t = (const char *)ir[k].pData;
                        if (strstr(ir[k].name, "push map")) {
                            st.mem = strstr(t, "RANGE0") != NULL;
                            const char *q = strstr(t, "Inline promoted dwords:");
                            if (q) for (q += 23; *q && *q != '\n';) {
                                char *e; const long d = strtol(q, &e, 10);
                                if (e == q) { q++; continue; }
                                if (d < 250) { st.ninl++; if (d >= 64) st.ndrv++; }
                                q = e;
                            }
                        }
                        if (asm_out && (strstr(ir[k].name, "ssembly") || getenv("BOI_NIR"))) fprintf(stderr, "==== %s: %s\n%s\n", tag, ir[k].name, t);
                    }
                for (uint32_t k = 0; k < ni; k++) free(ir[k].pData);
            }
        }
        pvkDestroyPipeline(dev, p, NULL);
    }
out:
    if (m) pvkDestroyShaderModule(dev, m, NULL);
    if (pl) pvkDestroyPipelineLayout(dev, pl, NULL);
    for (uint32_t s = 0; s < nset; s++) if (s != 2 && sl[s]) pvkDestroyDescriptorSetLayout(dev, sl[s], NULL);
    return st;
}

static uint32_t *slurp(const char *path, size_t *nbytes)
{
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); const long n = ftell(f); fseek(f, 0, SEEK_SET);
    uint32_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { free(b); fclose(f); return NULL; }
    fclose(f); *nbytes = (size_t)n; return b;
}
static void line(const char *tag, const stats *s)
{
    if (!s->ok) { printf("    %-8s (no pipeline)\n", tag); return; }
    printf("    %-8s SIMD%-2u inst %5lld sends %4lld cycles %8lld grf %4lld spills %3lld fills %3lld push %s (%d inline dwords, %d driver)\n",
           tag, s->simd, s->inst, s->sends, s->cycles, s->grf, s->spills, s->fills, s->mem ? "MEMORY" : "inline", s->ninl, s->ndrv);
}
typedef struct { unsigned n, simd_down, more_sends, more_inst, spill_up, inl2mem, mem0, mem1, est_n, est_bad_mem, est_bad_inl, fit, fit_bad; long long inst[3], sends[3], cycles[3]; } totals;
static totals T;
static int g_classic, g_asm; static uint32_t g_tg = 64;
static void pair(const char *pc_path)
{
    char bo_path[1200]; const size_t L = strlen(pc_path);
    if (L < 8 || strcmp(pc_path + L - 7, ".pc.spv") || L + 2 > sizeof bo_path) return;
    memcpy(bo_path, pc_path, L - 7); strcpy(bo_path + L - 7, ".bo.spv");
    size_t na = 0, nb = 0; uint32_t *a = slurp(pc_path, &na), *b = slurp(bo_path, &nb);
    if (!a || !b) { free(a); free(b); return; }
    const char *base = strrchr(pc_path, '/'); base = base ? base + 1 : pc_path;
    printf("%.*s\n", (int)(strlen(base) - 7), base);
    const stats s0 = compile(a, na, 1, g_tg, g_asm, "buffer, bufoff off"), s1 = compile(b, nb, 1, g_tg, g_asm, "buffer, bufoff on");
    stats sc = {0};
    if (g_classic) sc = compile(a, na, 0, g_tg, 0, "classic");
    line("nobufoff", &s0); line("bufoff", &s1); if (g_classic) line("classic", &sc);
    uint32_t app0 = 0, app1 = 0; const uint32_t e0 = nvmtl_bo_push_dwords(a, na, &app0), e1 = nvmtl_bo_push_dwords(b, nb, &app1);
    printf("    estimate nobufoff %u dwords (%u push block) -> %s, bufoff %u (%u) -> %s; fit rule: %s\n", e0, app0,
           e0 >= NVMTL_BO_INLINE_DWORDS ? "memory" : "inline", e1, app1, e1 >= NVMTL_BO_INLINE_DWORDS ? "memory" : "inline",
           e0 >= NVMTL_BO_INLINE_DWORDS ? "convert" : "keep");
    const stats *ss[2] = { &s0, &s1 }; const uint32_t ee[2] = { e0, e1 };
    for (int k = 0; k < 2; k++) if (ss[k]->ok && ee[k]) {
        T.est_n++;
        if (ss[k]->mem && ee[k] < NVMTL_BO_INLINE_DWORDS) { T.est_bad_inl++; printf("    ESTIMATE says inline, ANV reads memory (%s)\n", k ? "bufoff" : "nobufoff"); }
        if (!ss[k]->mem && ee[k] >= NVMTL_BO_INLINE_DWORDS) { T.est_bad_mem++; printf("    ESTIMATE says memory, ANV keeps inline (%s)\n", k ? "bufoff" : "nobufoff"); }
    }
    if (e0 >= NVMTL_BO_INLINE_DWORDS && s0.ok && s1.ok) { T.fit++; if (!s0.mem && s1.mem) T.fit_bad++; }
    if (s0.ok && s1.ok) {
        T.n++;
        T.inst[0] += s0.inst; T.inst[1] += s1.inst; T.sends[0] += s0.sends; T.sends[1] += s1.sends;
        T.cycles[0] += s0.cycles; T.cycles[1] += s1.cycles;
        if (sc.ok) { T.inst[2] += sc.inst; T.sends[2] += sc.sends; T.cycles[2] += sc.cycles; }
        if (s1.simd < s0.simd) T.simd_down++;
        if (s1.sends > s0.sends) T.more_sends++;
        if (s1.inst > s0.inst) T.more_inst++;
        if (s1.spills + s1.fills > s0.spills + s0.fills) T.spill_up++;
        if (!s0.mem && s1.mem) T.inl2mem++;
        T.mem0 += s0.mem; T.mem1 += s1.mem;
    }
    free(a); free(b);
}
int main(int argc, char **argv)
{
    int i = 1;
    for (; i < argc && argv[i][0] == '-'; i++) {
        if (!strcmp(argv[i], "-c")) g_classic = 1;
        else if (!strcmp(argv[i], "-a")) g_asm = 1;
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (i >= argc) { fprintf(stderr, "usage: XEMAC_FAKE=1 BOI_ICD=<libvulkan_intel.dylib> %s [-c] [-a] <dir-or-.pc.spv>...\n", argv[0]); return 2; }
    const char *icd = getenv("BOI_ICD");
    if (!icd || vk_init(icd)) { fprintf(stderr, "no Vulkan device (BOI_ICD=<libvulkan_intel.dylib>, XEMAC_FAKE=1)\n"); return 2; }
    if (getenv("BOI_TG")) g_tg = (uint32_t)atoi(getenv("BOI_TG"));
    for (; i < argc; i++) {
        struct stat sb;
        if (stat(argv[i], &sb)) continue;
        if (S_ISDIR(sb.st_mode)) {
            struct dirent **ents = NULL; const int ne = scandir(argv[i], &ents, NULL, alphasort);
            for (int k = 0; k < ne; k++) {
                char p[1100]; snprintf(p, sizeof p, "%s/%s", argv[i], ents[k]->d_name);
                pair(p); free(ents[k]);
            }
            free(ents);
        } else pair(argv[i]);
    }
    printf("%u pairs: instructions %lld -> %lld (%+.2f%%), SENDs %lld -> %lld (%+.2f%%), cycles %lld -> %lld (%+.2f%%)\n", T.n,
           T.inst[0], T.inst[1], T.inst[0] ? 100.0 * (T.inst[1] - T.inst[0]) / T.inst[0] : 0.0,
           T.sends[0], T.sends[1], T.sends[0] ? 100.0 * (T.sends[1] - T.sends[0]) / T.sends[0] : 0.0,
           T.cycles[0], T.cycles[1], T.cycles[0] ? 100.0 * (T.cycles[1] - T.cycles[0]) / T.cycles[0] : 0.0);
    printf("  kernels with more instructions %u, more SENDs %u, a narrower SIMD %u, more spills/fills %u\n",
           T.more_inst, T.more_sends, T.simd_down, T.spill_up);
    printf("  push data from memory: %u without the pass, %u with it (%u moved out of the inline registers by it)\n", T.mem0, T.mem1, T.inl2mem);
    printf("  estimate: %u modules, said inline where ANV reads memory %u, said memory where ANV keeps inline %u; the fit rule "
           "converts %u kernels, %u of them moved out of the inline registers by the pass\n", T.est_n, T.est_bad_inl, T.est_bad_mem, T.fit, T.fit_bad);
    if (g_classic) printf("  classic (the .pc.spv without descriptor buffers): instructions %lld, SENDs %lld, cycles %lld\n", T.inst[2], T.sends[2], T.cycles[2]);
    return 0;
}
