// SPDX-License-Identifier: MIT
// icb_lower_check: the kernels of mtl_gpu_icb2 / mtl_icb_kept the way nvmtl_vk_compute_pipeline_create builds them, off the
// GPU (docs/design/DESIGN-icb-dgc.md section 10). The Mac's Metal front end makes the AIR, the built plugin translates it
// (nvmtl_translate_metallib / nvmtl_compile_library_entry, as xlate_check), the built translator lowers buffer addresses
// (nvmtl_lower_buffer_addresses_ex with the flags the plugin passes on ANV: Uniform table, 4-byte alignment) without and
// with NVMTL_LOWER_PUSH_NOT_DATA (what NVMTL_ICB_DGC=1 asks for a pipeline with supportIndirectCommandBuffers), then
// cs-table-pc (classic model) as the plugin runs it. Printed per kernel and mode: the buffers still read through
// descriptors (nvmtl_desc_bufs.h, the plan's guard) and the pushed table entries; and the buffer indices the DGC plan
// offers for a shader to set (pushed, or not read at all; nvmtl_icb_dgc.c nvmtl_dgc_takes).
//
// Expected: w and kt (o[thread_position_in_grid]): o kept as a descriptor without the flag (the 13 Nov night's C0..C2:
// the kernel wrote the CPU's buffer), reached by address with it, index 0 offered; ki (o[idx[t]]): o stays a descriptor
// in both modes and index 0 is never offered; kc (constant index): by address in both modes.
//
//   NVMTL_LOG_PATH=<log> ./icb_lower_check <NVMTLDriver.bundle/Contents/MacOS/NVMTLDriver> <libnvmtl_translate.dylib>
// x86_64 (Rosetta is fine). Exit: 0 as expected, 1 setup, 4 a kernel not as expected.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <stdio.h>
#include "nvmtl_table_pc.h"
#include "nvmtl_desc_bufs.h"

#define LOWER_PUSH_NOT_DATA 0x10u   /* nvmtl_vk.h NVMTL_LOWER_PUSH_NOT_DATA */

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     "kernel void w(device uint *o [[buffer(0)]], constant uint &tag [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[t] = tag * 1000 + t; }\n"
     "kernel void kt(device uint *o [[buffer(0)]], constant uint &tag [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[t] = tag * 1000 + t; }\n"
     "kernel void ki(device uint *o [[buffer(0)]], const device uint *idx [[buffer(1)]], uint t [[thread_position_in_grid]]) { o[idx[t]] = 7000 + t; }\n"
     "kernel void kc(device uint *o [[buffer(0)]], constant uint &tag [[buffer(1)]]) { o[3] = tag; }\n";

typedef NSDictionary<NSString *, NSData *> *(*translate_fn)(NSData *, NSDictionary **, NSDictionary **);
typedef NSData *(*compile_fn)(NSData *air, NSString *ll, NSString *stage, NSString *name, NSString **error);
typedef int (*lower_ex_fn)(const uint8_t *, size_t, uint32_t, uint8_t **, size_t *, uint32_t *, char *, size_t);
typedef void (*free_fn)(uint8_t *, size_t);

static void bits(char *s, size_t n, uint32_t m)
{
    if (m == 0xffffffffu) { snprintf(s, n, "ALL"); return; }
    size_t k = 0; s[0] = 0;
    for (uint32_t b = 0; b < 32 && k + 4 < n; b++) if (m >> b & 1u) k += (size_t)snprintf(s + k, n - k, "%s%u", k ? "," : "", b);
    if (!k) snprintf(s, n, "-");
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IOLBF, 0);
    @autoreleasepool {
        if (argc < 3) { fprintf(stderr, "usage: %s NVMTLDriver libnvmtl_translate.dylib\n", argv[0]); return 1; }
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice();
        if (!dev) { fprintf(stderr, "no Metal device to run the front end\n"); return 1; }
        NSError *err = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:[MTLCompileOptions new] error:&err];
        if (!lib) { fprintf(stderr, "front end: %s\n", err.localizedDescription.UTF8String); return 1; }
        NSData *mtlb = [(id)lib valueForKey:@"libraryDataContents"];
        if (![mtlb isKindOfClass:[NSData class]]) { fprintf(stderr, "no libraryDataContents\n"); return 1; }
        void *h = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *x = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!h || !x) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
        translate_fn tr = (translate_fn)dlsym(h, "nvmtl_translate_metallib");
        compile_fn compile = (compile_fn)dlsym(h, "nvmtl_compile_library_entry");
        lower_ex_fn lower = (lower_ex_fn)dlsym(x, "nvmtl_lower_buffer_addresses_ex");
        free_fn xfree = (free_fn)dlsym(x, "nvmtl_translate_free");
        if (!tr || !compile || !lower || !xfree) { fprintf(stderr, "missing entry points\n"); return 1; }
        NSDictionary *stages = nil, *airs = nil;
        NSDictionary<NSString *, NSData *> *fns = tr(mtlb, &stages, &airs);
        int bad = 0;
        for (NSString *name in @[ @"w", @"kt", @"ki", @"kc" ]) {
            NSString *why = nil;
            NSData *spv = fns[name] ? compile(fns[name], airs[name], stages[name], name, &why) : nil;
            if (!spv) { printf("FAIL %-3s no SPIR-V: %s\n", name.UTF8String, why.UTF8String ?: "not in the library"); bad++; continue; }
            uint32_t got[2] = { 0, 0 }, offered[2] = { 0, 0 };
            for (int mode = 0; mode < 2; mode++) {
                uint8_t *o = NULL; size_t on = 0; uint32_t info[4] = { 0 }; char e[512] = { 0 };
                const uint32_t flags = 2u | (2u << 8) | (mode ? LOWER_PUSH_NOT_DATA : 0u);
                if (lower(spv.bytes, spv.length, flags, &o, &on, info, e, sizeof e)) {
                    printf("     %-3s %-13s lowering REFUSED (%s): every buffer stays a descriptor\n", name.UTF8String, mode ? "icb-lower" : "default", e);
                    got[mode] = 0xffffffffu; continue;
                }
                nvmtl_pc_table pc = { 0 }; size_t pn = 0;
                uint32_t *p = nvmtl_spirv_table_pc_ex(o, on, 5, NVMTL_PC_TABLE_CS_OFFSET, NVMTL_PC_TABLE_MAX, NVMTL_PC_MERGE | NVMTL_PC_SET0, &pc, &pn);
                const void *fin = p ? (const void *)p : (const void *)o; const size_t fn = p ? pn : on;
                got[mode] = nvmtl_spirv_desc_bufs(fin, fn);
                // nvmtl_dgc_takes' first rule: an index the kernel reads through a descriptor is never offered (the others
                // depend on the table plan: a pushed entry is, as is an index the kernel does not read)
                uint32_t pushed = 0; for (uint32_t k = 0; k < pc.count; k++) pushed |= 1u << pc.index[k];
                offered[mode] = ~got[mode] & 0x7fffffffu;
                char d[96], q[96], f[96]; bits(d, sizeof d, got[mode]); bits(q, sizeof q, pushed); bits(f, sizeof f, offered[mode] & 3u);
                printf("     %-3s %-13s descriptors {%s}  pushed table entries {%s}  offered of {0,1}: {%s}\n", name.UTF8String,
                       mode ? "icb-lower" : "default", d, q, f);
                free(p); xfree(o, on);
            }
            int ok;
            if ([name isEqualToString:@"ki"]) ok = (got[0] & 1u) && (got[1] & 1u) && !(offered[1] & 1u);
            else if ([name isEqualToString:@"kc"]) ok = !(got[0] & 1u) && !(got[1] & 1u) && (offered[1] & 1u);
            else ok = (got[0] & 1u) && !(got[1] & 1u) && (offered[1] & 1u);
            printf("%s %-3s %s\n", ok ? "PASS" : "FAIL", name.UTF8String,
                   [name isEqualToString:@"ki"] ? "o[idx[t]] keeps its descriptor either way; index 0 is never offered" :
                   [name isEqualToString:@"kc"] ? "o[3] by address either way" :
                   "o[thread_position_in_grid]: a descriptor before, by address with the ICB lowering; index 0 offered");
            bad += !ok;
        }
        printf("icb_lower_check: %s\n", bad ? "FAIL" : "PASS");
        return bad ? 4 : 0;
    }
}
