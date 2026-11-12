// SPDX-License-Identifier: MIT
// mtl_sparse_buffer: Metal placement sparse buffers on the Arc B580 (NVMTL_SPARSE_TEXTURES=1 and NVMTL_METAL4=1;
// nullmoth/plugin/NVMTLSparse.m, docs/design/DESIGN-sparse-textures.md "Sparse buffers").
//
// A placement heap of 4 tiles and a sparse buffer of 8 tiles (newBufferWithLength:options:placementSparsePageSize:):
//   1. the buffer answers sparseBufferTier Tier1 (an ordinary buffer None), its length and a GPU address;
//   2. MTL4CommandQueue updateBufferMappings maps buffer tiles 0, 1 and 5 to heap tiles 0, 1 and 2; a kernel writes a
//      pattern over the whole buffer and a second kernel reads every tile's first and last word back: mapped tiles
//      hold the pattern, unmapped tiles read zero (their writes vanished); a blit of the whole buffer agrees;
//   3. a second update unmaps tile 1 and maps tile 3 to heap tile 1: tile 3 now shows what tile 1 held (the heap
//      tile kept its contents), tile 1 reads zero;
//   4. copyBufferMappingsFromBuffer gives a second sparse buffer's tile 2 the first buffer's tile 0: it reads the
//      first buffer's data there.
// Mapping updates go through a Metal 4 queue, which signals an event the Metal 3 command buffers wait for.
// Each check prints PASS or FAIL; the last line counts them.
//
//   NVMTL_ENABLE=1 NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1 NVMTL_METAL4=1 ./mtl_sparse_buffer    (B580)
//   ./mtl_sparse_buffer --any                         the first Metal device when no B580 is present (reference run)
//
// Exit: 0 pass, 1 no B580 device, 2 setup/compile failure, 3 GPU error, 4 wrong results, 5 sparse buffers not offered.
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <IOKit/IOKitLib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

static NSString *const kSource =
    @"#include <metal_stdlib>\n"
     "using namespace metal;\n"
     // word i of the buffer = i * 2654435761 + salt (never 0 for the words read back)
     "kernel void fill(device uint *b [[buffer(0)]], constant uint &salt [[buffer(1)]], uint i [[thread_position_in_grid]]) {\n"
     "    b[i] = i * 2654435761u + salt;\n"
     "}\n"
     // per tile t: its first and last word
     "kernel void probe(const device uint *b [[buffer(0)]], device uint2 *o [[buffer(1)]], constant uint &words [[buffer(2)]],\n"
     "                  uint t [[thread_position_in_grid]]) {\n"
     "    o[t] = uint2(b[t * words], b[t * words + words - 1]);\n"
     "}\n";

static uint64_t nmx_registry_id(void)
{
    io_service_t s = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
    if (!s) return 0;
    uint64_t id = 0;
    IORegistryEntryGetRegistryEntryID(s, &id);
    IOObjectRelease(s);
    return id;
}

static int npass, nfail, gpu_error;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}

enum { TILES = 8, HEAP_TILES = 4 };
static id<MTLDevice> dev;
static id<MTLCommandQueue> queue;
static id<MTLComputePipelineState> fillPS, probePS;
static id<MTLSharedEvent> event;
static uint64_t eventValue;
static NSUInteger tileBytes;

static uint32_t pattern(uint32_t word, uint32_t salt) { return word * 2654435761u + salt; }

static void finish(id<MTLCommandBuffer> cb, const char *what)
{
    [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) {
        fprintf(stderr, "FAIL: %s: command buffer status %ld: %s\n", what, (long)cb.status, cb.error.localizedDescription.UTF8String ?: "");
        gpu_error = 1;
    }
}
// a Metal 3 command buffer ordered after the Metal 4 queue's last mapping update
static id<MTLCommandBuffer> after_mappings(void)
{
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    [cb encodeWaitForEvent:event value:eventValue];
    return cb;
}
static void fill(id<MTLBuffer> b, uint32_t salt)
{
    id<MTLCommandBuffer> cb = after_mappings();
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:fillPS]; [e setBuffer:b offset:0 atIndex:0]; [e setBytes:&salt length:4 atIndex:1];
    [e dispatchThreads:MTLSizeMake(b.length / 4, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [e endEncoding];
    finish(cb, "fill");
}
// first and last word of every tile, by a kernel (out[2t], out[2t+1]) and by a blit of the whole buffer (copy)
static void probe(id<MTLBuffer> b, uint32_t *out, id<MTLBuffer> copy)
{
    id<MTLBuffer> o = [dev newBufferWithLength:TILES * 8 options:MTLResourceStorageModeShared];
    const uint32_t words = (uint32_t)(tileBytes / 4);
    id<MTLCommandBuffer> cb = after_mappings();
    id<MTLComputeCommandEncoder> e = [cb computeCommandEncoder];
    [e setComputePipelineState:probePS]; [e setBuffer:b offset:0 atIndex:0]; [e setBuffer:o offset:0 atIndex:1]; [e setBytes:&words length:4 atIndex:2];
    [e dispatchThreads:MTLSizeMake(TILES, 1, 1) threadsPerThreadgroup:MTLSizeMake(TILES, 1, 1)];
    [e endEncoding];
    if (copy) { id<MTLBlitCommandEncoder> bl = [cb blitCommandEncoder]; [bl copyFromBuffer:b sourceOffset:0 toBuffer:copy destinationOffset:0 size:b.length]; [bl endEncoding]; }
    finish(cb, "probe");
    memcpy(out, o.contents, TILES * 8);
}
// every tile's first and last word: `src[t]` is the tile whose words tile t holds (its pattern), -1 for zero
static void expect(const char *what, const uint32_t *got, const int *src, uint32_t salt, id<MTLBuffer> copy)
{
    const uint32_t words = (uint32_t)(tileBytes / 4);
    for (int t = 0; t < TILES; t++) {
        const uint32_t w0 = src[t] < 0 ? 0 : pattern((uint32_t)src[t] * words, salt), w1 = src[t] < 0 ? 0 : pattern((uint32_t)src[t] * words + words - 1, salt);
        int ok = got[2 * t] == w0 && got[2 * t + 1] == w1;
        uint32_t c0 = 0, c1 = 0;
        if (copy) { const uint32_t *c = copy.contents; c0 = c[t * words]; c1 = c[t * words + words - 1]; ok = ok && c0 == w0 && c1 == w1; }
        if (src[t] < 0) check(ok, "%s: tile %d is unmapped and reads 0 (kernel 0x%08x 0x%08x%s)", what, t, got[2 * t], got[2 * t + 1], copy ? ", blit too" : "");
        else if (copy) check(ok, "%s: tile %d holds tile %d's pattern (kernel 0x%08x 0x%08x, blit 0x%08x 0x%08x, want 0x%08x 0x%08x)", what, t,
                             src[t], got[2 * t], got[2 * t + 1], c0, c1, w0, w1);
        else check(ok, "%s: tile %d holds tile %d's pattern (kernel 0x%08x 0x%08x, want 0x%08x 0x%08x)", what, t, src[t], got[2 * t], got[2 * t + 1], w0, w1);
    }
}

API_AVAILABLE(macos(26.0))
static int body(void)
{
    BOOL placement = NO;
    if (@available(macOS 26.4, *)) placement = dev.supportsPlacementSparse;
    else placement = [(id)dev respondsToSelector:@selector(supportsPlacementSparse)] && [[(id)dev valueForKey:@"supportsPlacementSparse"] boolValue];
    if (!placement) { printf("SKIP: supportsPlacementSparse NO (B580: NVMTL_PROCESS_ENV=1 NVMTL_SPARSE_TEXTURES=1)\n"); return 5; }
    id<MTL4CommandQueue> q4 = [dev supportsFamily:MTLGPUFamilyMetal4] ? [dev newMTL4CommandQueue] : nil;
    if (!q4) { printf("SKIP: no MTL4CommandQueue (B580: NVMTL_METAL4=1)\n"); return 5; }
    tileBytes = [dev sparseTileSizeInBytesForSparsePageSize:MTLSparsePageSize64];
    printf("tile: %lu bytes for 64 KiB pages\n", (unsigned long)tileBytes);
    if (!tileBytes) { printf("SKIP: no sparse tile size\n"); return 5; }

    NSError *err = nil;
    id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&err];
    if (!lib) { fprintf(stderr, "FAIL: newLibraryWithSource: %s\n", err.localizedDescription.UTF8String); return 2; }
    fillPS = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"fill"] error:&err];
    probePS = [dev newComputePipelineStateWithFunction:[lib newFunctionWithName:@"probe"] error:&err];
    if (!fillPS || !probePS) { fprintf(stderr, "FAIL: pipelines: %s\n", err.localizedDescription.UTF8String); return 2; }
    event = [dev newSharedEvent];

    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = tileBytes * HEAP_TILES;
    hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
    id<MTLHeap> heap = [dev newHeapWithDescriptor:hd];
    if (!heap) { fprintf(stderr, "FAIL: placement heap of %u tiles -> nil\n", HEAP_TILES); return 2; }

    // 1.
    id<MTLBuffer> sb = [dev newBufferWithLength:tileBytes * TILES options:MTLResourceStorageModePrivate placementSparsePageSize:MTLSparsePageSize64];
    check(sb != nil, "newBufferWithLength:%lu options:private placementSparsePageSize:64 KiB -> %p", (unsigned long)(tileBytes * TILES), (__bridge void *)sb);
    if (!sb) return 4;
    id<MTLBuffer> plain = [dev newBufferWithLength:4096 options:MTLResourceStorageModePrivate];
    check(sb.sparseBufferTier == MTLBufferSparseTier1 && plain.sparseBufferTier == MTLBufferSparseTierNone,
          "sparseBufferTier: sparse buffer %ld (want Tier1 = 1), ordinary buffer %ld (want None)", (long)sb.sparseBufferTier, (long)plain.sparseBufferTier);
    check(sb.length == tileBytes * TILES && sb.gpuAddress != 0, "length %lu, gpuAddress 0x%llx", (unsigned long)sb.length, (unsigned long long)sb.gpuAddress);

    // 2.
    const MTL4UpdateSparseBufferMappingOperation m1[2] = {
        { MTLSparseTextureMappingModeMap, NSMakeRange(0, 2), 0 },
        { MTLSparseTextureMappingModeMap, NSMakeRange(5, 1), 2 },
    };
    [q4 updateBufferMappings:sb heap:heap operations:m1 count:2];
    [q4 signalEvent:event value:++eventValue];
    id<MTLBuffer> copy = [dev newBufferWithLength:tileBytes * TILES options:MTLResourceStorageModeShared];
    uint32_t got[TILES * 2];
    fill(sb, 0x1234);
    probe(sb, got, copy);
    { const int src[TILES] = { 0, 1, -1, -1, -1, 5, -1, -1 }; expect("tiles 0,1,5 mapped", got, src, 0x1234, copy); }

    // 3. tile 1's heap tile moves to tile 3
    const MTL4UpdateSparseBufferMappingOperation m2[2] = {
        { MTLSparseTextureMappingModeUnmap, NSMakeRange(1, 1), 0 },
        { MTLSparseTextureMappingModeMap, NSMakeRange(3, 1), 1 },
    };
    [q4 updateBufferMappings:sb heap:heap operations:m2 count:2];
    [q4 signalEvent:event value:++eventValue];
    probe(sb, got, nil);
    { const int src[TILES] = { 0, -1, -1, 1, -1, 5, -1, -1 }; expect("tile 1 unmapped, tile 3 mapped to its heap tile", got, src, 0x1234, nil); }

    // 4. a second buffer's tile 2 aliases the first buffer's tile 0
    id<MTLBuffer> sb2 = [dev newBufferWithLength:tileBytes * TILES options:MTLResourceStorageModePrivate placementSparsePageSize:MTLSparsePageSize64];
    if (!sb2) check(0, "a second sparse buffer -> nil");
    else {
        const MTL4CopySparseBufferMappingOperation c1 = { NSMakeRange(0, 1), 2 };
        [q4 copyBufferMappingsFromBuffer:sb toBuffer:sb2 operations:&c1 count:1];
        [q4 signalEvent:event value:++eventValue];
        probe(sb2, got, nil);
        const int src[TILES] = { -1, -1, 0, -1, -1, -1, -1, -1 };
        expect("copyBufferMappings: second buffer's tile 2 = first buffer's tile 0", got, src, 0x1234, nil);
    }
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const BOOL any = argc > 1 && !strcmp(argv[1], "--any");
        const uint64_t nmx = nmx_registry_id();
        for (id<MTLDevice> d in MTLCopyAllDevices()) {
            const BOOL b580 = (nmx && d.registryID == nmx) || [NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"];
            if (b580 || (any && !dev)) dev = d;
            if (b580) break;
        }
        if (!dev) { fprintf(stderr, "FAIL: no B580 Metal device (--any runs on the first device)\n"); return 1; }
        printf("device: %s (%s)\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String);
        queue = [dev newCommandQueue];
        int rc;
        if (@available(macOS 26.0, *)) rc = body();
        else { printf("SKIP: placement sparse buffers need macOS 26\n"); return 5; }
        if (rc) return rc;
        printf("%d PASS, %d FAIL\n", npass, nfail);
        return gpu_error ? 3 : nfail ? 4 : 0;
    }
}
