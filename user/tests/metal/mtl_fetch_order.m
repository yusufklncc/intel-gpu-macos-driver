// SPDX-License-Identifier: MIT
// mtl_fetch_order: framebuffer fetch ([[color(0)]]) ordering inside ONE draw, the case the plugin's per-triangle split and
// its NVMTL_FETCH_OVERLAP runs (nvmtl_fetch_order.h) must keep. Every fragment does out.rgb = in.rgb * 0.5 + c (c the
// quad's flat colour) and out.a = in.a + 0.25, so each pixel's value says how many triangles covered it and in which
// order. Each case is one indexed draw (u16 triangle list, float4 position x vertex-uniform matrix as Core Animation draws)
// into a cleared RGBA32Float target; the result is read back and compared exactly with the same arithmetic on the CPU.
//
//   a  overlap      8 quads stacked with offsets (16 triangles): every overlap must show all contributions, in order
//   b  nine-part    3x3 adjacent quads (18 triangles), 1-pixel borders: every pixel exactly one contribution
//   c  t-junction   one quad beside two half-height quads (6 triangles, a T-junction): one contribution per pixel
//   d  non-planar   two quads disjoint in attribute (x, y) but overlapping on screen through different w: the plugin
//                   must see the varying w, fall back to per-triangle draws, and still order the overlap
//   e  strip        24 glyph-like quads, each overlapping the next (48 triangles): many short runs
//   f  grid         a 7x4 grid of adjacent quads (56 triangles, Core Animation's largest mesh): one run
// Each case runs with the vertex data in a Shared buffer, a Managed buffer (didModifyRange) and setVertexBytes.
// A race (two overlapping triangles drawn without the barrier) shows as a wrong value only sometimes: the cases repeat
// for [seconds] (default 6) and must all pass every time.
//
// Build (x86_64 for the B580, macOS 13+):
//   clang -arch x86_64 -mmacosx-version-min=13.0 -fobjc-arc -O1 -g -Wall -o mtl_fetch_order mtl_fetch_order.m \
//     -framework Foundation -framework Metal -framework IOKit
// Run: ./mtl_fetch_order [seconds=6]
//   a plugin build that is not installed: NVMTL_ENABLE=1 NVMTL_BUNDLE=<dir>/NVMTLDriver.bundle ./mtl_fetch_order
//   compare: NVMTL_FETCH_OVERLAP=0 (per-triangle as before); diagnostic plugin: NVMTL_FETCH_STATS=mtl_fetch_order writes a
//   FETCHORDER line to the plugin log every 5 s (a release plugin reads these only with NVMTL_PROCESS_ENV=1)
// Exit: 0 all PASS, 1 a FAIL, 2 setup.
#import <Foundation/Foundation.h>
#import <IOKit/IOKitLib.h>
#import <Metal/Metal.h>
#import <objc/message.h>
#include <math.h>

static NSString *const kSource = @R"(
#include <metal_stdlib>
using namespace metal;
struct VIn { float4 pos [[attribute(0)]]; float4 col [[attribute(1)]]; };
struct VOut { float4 pos [[position]]; float4 col [[flat]]; };
vertex VOut vs(VIn in [[stage_in]], constant float4x4 &m [[buffer(2)]]) {
    VOut o; o.pos = m * in.pos; o.col = in.col; return o;
}
fragment float4 fs(VOut in [[stage_in]], float4 dst [[color(0)]]) {
    return float4(dst.rgb * 0.5 + in.col.rgb, dst.a + 0.25);
}
)";

enum { W = 128, H = 128, MAXQ = 64 };
typedef struct { float x0, y0, x1, y1, c, w; } Quad;   // screen pixels [x0,x1) x [y0,y1); colour c (r = g = b); attribute w
typedef struct { const char *name; int n; Quad q[MAXQ]; } Case;

static id<MTLDevice> bundleDevice(const char *path) {   // as mtl_tests.m: that plugin build in this process only
    NSBundle *b = [NSBundle bundleWithPath:@(path)];
    NSError *e = nil;
    if (![b loadAndReturnError:&e]) { printf("bundle %s: %s\n", path, e.localizedDescription.UTF8String); return nil; }
    Class cls = [b classNamed:@"NVMTLDevice"];
    if (!cls) return nil;
    io_iterator_t it = 0;
    if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IOAccelerator"), &it) != KERN_SUCCESS) return nil;
    id<MTLDevice> dev = nil;
    for (io_service_t svc; !dev && (svc = IOIteratorNext(it)); IOObjectRelease(svc)) {
        CFTypeRef name = IORegistryEntryCreateCFProperty(svc, CFSTR("MetalPluginClassName"), kCFAllocatorDefault, 0);
        if (name && CFEqual(name, CFSTR("NVMTLDevice")))
            dev = ((id (*)(id, SEL, mach_port_t))objc_msgSend)([cls alloc], sel_registerName("initWithAcceleratorPort:"), svc);
        if (name) CFRelease(name);
    }
    IOObjectRelease(it);
    return dev;
}

static void addq(Case *k, float x0, float y0, float x1, float y1, float c, float w) {
    if (k->n < MAXQ) k->q[k->n++] = (Quad){ x0, y0, x1, y1, c, w };
}
static void makeCases(Case *cs, int *ncase) {
    int n = 0;
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "a overlap (8 stacked quads)";
      for (int i = 0; i < 8; i++) addq(k, 8 + 6 * i, 10 + 4 * i, 72 + 6 * i, 90 + 2 * i, (float)(i + 1) / 16.0f, 1); }
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "b nine-part (3x3, 1 px borders)";
      const float xs[4] = { 10, 11, 109, 110 }, ys[4] = { 20, 21, 99, 100 };
      for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) addq(k, xs[c], ys[r], xs[c + 1], ys[r + 1], (float)(1 + r * 3 + c) / 16.0f, 1); }
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "c t-junction";
      addq(k, 16, 16, 64, 112, 0.25f, 1); addq(k, 64, 16, 112, 64, 0.5f, 1); addq(k, 64, 64, 112, 112, 0.75f, 1); }
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "d non-planar (w differs)";
      addq(k, 8, 8, 72, 72, 0.25f, 1); addq(k, 40, 40, 120, 120, 0.5f, 2); addq(k, 24, 56, 88, 120, 0.125f, 1); }
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "e strip (24 overlapping quads)";
      for (int i = 0; i < 24; i++) addq(k, 4 + 4 * i, 40 + (i & 3), 12 + 4 * i, 72 + (i & 3), (float)((i % 7) + 1) / 16.0f, 1); }
    { Case *k = &cs[n++]; memset(k, 0, sizeof *k); k->name = "f grid (7x4 adjacent quads)";
      for (int r = 0; r < 4; r++) for (int c = 0; c < 7; c++) addq(k, 8 + 16 * c, 16 + 24 * r, 24 + 16 * c, 40 + 24 * r, (float)((r * 7 + c) % 15 + 1) / 16.0f, 1); }
    *ncase = n;
}
// vertices: per quad 4 corners (x*w, y*w, 0, w) + colour; indices (0,1,2)(0,2,3) as a nine-part mesh lists them
static void geometry(const Case *k, float *v, uint16_t *ix) {
    for (int i = 0; i < k->n; i++) {
        const Quad *q = &k->q[i]; const float cx[4] = { q->x0, q->x1, q->x1, q->x0 }, cy[4] = { q->y0, q->y0, q->y1, q->y1 };
        for (int j = 0; j < 4; j++) {
            float *p = v + 8 * (4 * i + j);
            p[0] = cx[j] * q->w; p[1] = cy[j] * q->w; p[2] = 0; p[3] = q->w; p[4] = p[5] = p[6] = q->c; p[7] = 1;
        }
        const uint16_t b = (uint16_t)(4 * i); const uint16_t t[6] = { b, (uint16_t)(b + 1), (uint16_t)(b + 2), b, (uint16_t)(b + 2), (uint16_t)(b + 3) };
        memcpy(ix + 6 * i, t, sizeof t);
    }
}
static void expected(const Case *k, float *out /* W*H*4 */) {
    memset(out, 0, sizeof(float) * W * H * 4);
    for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) {
        float r = 0, a = 0; const float px = x + 0.5f, py = y + 0.5f;
        for (int i = 0; i < k->n; i++) {
            const Quad *q = &k->q[i];
            if (px >= q->x0 && px < q->x1 && py >= q->y0 && py < q->y1) { r = r * 0.5f + q->c; a += 0.25f; }
        }
        float *o = out + 4 * (y * W + x); o[0] = o[1] = o[2] = r; o[3] = a;
    }
}

int main(int argc, char **argv) {
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        const double seconds = argc > 1 ? atof(argv[1]) : 6;
        id<MTLDevice> dev = nil;
        const char *bundle = getenv("NVMTL_BUNDLE");
        if (bundle) dev = bundleDevice(bundle);
        if (!dev) for (id<MTLDevice> d in MTLCopyAllDevices()) if ([NSStringFromClass([d class]) isEqualToString:@"NVMTLDevice"]) dev = d;
        if (!dev) dev = MTLCreateSystemDefaultDevice();
        if (!dev) { printf("no Metal device\n"); return 2; }
        printf("device %s (%s), %.0f s, NVMTL_FETCH_OVERLAP=%s\n", dev.name.UTF8String, NSStringFromClass([dev class]).UTF8String, seconds,
               getenv("NVMTL_FETCH_OVERLAP") ?: "unset");
        NSError *e = nil;
        id<MTLLibrary> lib = [dev newLibraryWithSource:kSource options:nil error:&e];
        if (!lib) { printf("library (framebuffer fetch needs the plugin or an Apple GPU): %s\n", e.description.UTF8String); return 2; }
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA32Float;
        MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
        vd.attributes[0].format = MTLVertexFormatFloat4; vd.attributes[0].offset = 0; vd.attributes[0].bufferIndex = 1;
        vd.attributes[1].format = MTLVertexFormatFloat4; vd.attributes[1].offset = 16; vd.attributes[1].bufferIndex = 1;
        vd.layouts[1].stride = 32; vd.layouts[1].stepFunction = MTLVertexStepFunctionPerVertex;
        pd.vertexDescriptor = vd;
        id<MTLRenderPipelineState> ps = [dev newRenderPipelineStateWithDescriptor:pd error:&e];
        if (!ps) { printf("pipeline: %s\n", e.description.UTF8String); return 2; }
        id<MTLCommandQueue> q = [dev newCommandQueue];
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA32Float width:W height:H mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> rt = [dev newTextureWithDescriptor:td];
        id<MTLBuffer> rb = [dev newBufferWithLength:W * H * 16 options:MTLResourceStorageModeShared];
        // pixels -> clip: x_ndc = x / 64 - 1, y_ndc = 1 - y / 64, applied to (x*w, y*w, 0, w): the same screen point for any w
        const float m[16] = { 2.0f / W, 0, 0, 0,   0, -2.0f / H, 0, 0,   0, 0, 1, 0,   -1, 1, 0, 1 };
        Case cs[8]; int ncase = 0; makeCases(cs, &ncase);
        static const char *const modeName[3] = { "shared", "managed", "setVertexBytes" };
        long runs[8][3] = { { 0 } }, fails[8][3] = { { 0 } };
        float *want = malloc(sizeof(float) * W * H * 4);
        const CFAbsoluteTime t0 = CFAbsoluteTimeGetCurrent();
        int pass = 0;
        do {
            for (int c = 0; c < ncase; c++) for (int mode = 0; mode < 3; mode++) {
                const Case *k = &cs[c];
                static float v[MAXQ * 4 * 8]; static uint16_t ix[MAXQ * 6];
                geometry(k, v, ix);
                const NSUInteger vlen = (NSUInteger)k->n * 4 * 32, ilen = (NSUInteger)k->n * 6 * 2;
                id<MTLBuffer> ib = [dev newBufferWithLength:ilen + 512 options:MTLResourceStorageModeShared];
                memset(ib.contents, 0, 512); memcpy((uint8_t *)ib.contents + 512, ix, ilen);   // the indices at offset 512, as Core Animation's
                id<MTLBuffer> vb = nil;
                if (mode == 0) vb = [dev newBufferWithBytes:v length:vlen options:MTLResourceStorageModeShared];
                if (mode == 1) {
                    if (dev.hasUnifiedMemory) continue;   // no Managed storage there
                    vb = [dev newBufferWithLength:vlen options:MTLResourceStorageModeManaged];
                    memcpy(vb.contents, v, vlen); [vb didModifyRange:NSMakeRange(0, vlen)];
                }
                id<MTLCommandBuffer> cb = [q commandBuffer];
                MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
                rp.colorAttachments[0].texture = rt; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
                id<MTLRenderCommandEncoder> re = [cb renderCommandEncoderWithDescriptor:rp];
                [re setRenderPipelineState:ps];
                if (vb) [re setVertexBuffer:vb offset:0 atIndex:1]; else [re setVertexBytes:v length:vlen atIndex:1];
                [re setVertexBytes:m length:sizeof m atIndex:2];
                [re drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:(NSUInteger)k->n * 6 indexType:MTLIndexTypeUInt16
                              indexBuffer:ib indexBufferOffset:512];
                [re endEncoding];
                id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
                [be copyFromTexture:rt sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(W, H, 1)
                           toBuffer:rb destinationOffset:0 destinationBytesPerRow:W * 16 destinationBytesPerImage:W * H * 16];
                [be endEncoding];
                [cb commit]; [cb waitUntilCompleted];
                if (cb.status != MTLCommandBufferStatusCompleted) { printf("FAIL %s / %s: command buffer status %ld\n", k->name, modeName[mode], (long)cb.status); fails[c][mode]++; continue; }
                expected(k, want);
                const float *got = (const float *)rb.contents; long bad = 0; int bx = -1, by = -1;
                for (int p = 0; p < W * H; p++) for (int ch = 0; ch < 4; ch++)
                    if (got[4 * p + ch] != want[4 * p + ch]) { if (!bad) { bx = p % W; by = p / W; } bad++; break; }
                runs[c][mode]++;
                if (bad) {
                    if (fails[c][mode]++ < 3) {
                        const float *g = got + 4 * (by * W + bx), *w = want + 4 * (by * W + bx);
                        printf("  pass %d %s / %s: %ld pixels wrong, first (%d,%d): got (%g, a %g) want (%g, a %g)\n", pass, k->name, modeName[mode],
                               bad, bx, by, g[0], g[3], w[0], w[3]);
                    }
                }
            }
            pass++;
        } while (CFAbsoluteTimeGetCurrent() - t0 < seconds);
        int anyfail = 0;
        for (int c = 0; c < ncase; c++) for (int mode = 0; mode < 3; mode++) {
            printf("%s %-34s %-15s %ld runs, %ld wrong\n", fails[c][mode] ? "FAIL" : "PASS", cs[c].name, modeName[mode], runs[c][mode], fails[c][mode]);
            anyfail |= fails[c][mode] != 0;
        }
        printf("%s (%d passes)\n", anyfail ? "FAIL" : "PASS", pass);
        free(want);
        return anyfail ? 1 : 0;
    }
}
