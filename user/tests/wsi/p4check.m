// SPDX-License-Identifier: MIT
// p4check: checks for gpuperf phase 4 (WindowServer composites scanned out without their system-memory readback,
// nullmoth/plugin/nvmtl_scanout_p4.h): do CPU readers of the screen (screenshots, screen recordings) see the CURRENT
// frame, and how smooth is the desktop. Runs on the B580 test machine; night-phase4.sh drives it.
//
//   p4check show DIR [-n N] [-free MS]   GUI (start it in the console user's session: a launchd agent). A borderless
//       300x300 pt patch near the main display's top-left corner shows colour k = palette[k % 6] for k = 0..N-1.
//       Handshake (default): once colour k has been on screen for 150 ms it writes "k x y w h scale" to DIR/state
//       (the patch's centre 100x100 pt in global top-left points, for screencapture -R; then the backing scale and the
//       main display's size in points, to find the patch in a recording) and waits for DIR/ack to say k
//       (10 s at most) before the next colour: a capturer that reads the screen while k is held must see colour k.
//       -free MS: no handshake, the colour changes every MS ms (for a screen recording); DIR/state is written once, and
//       DIR/seq gets "k uptime_ms" per change; the last colour stays 3 s more. DIR/done at the end.
//   p4check probe PNG K               the colour of a screencapture -R picture (its central half averaged), classified
//       against the palette: prints "probe K got G rgb r g b" and exits 0 when G == K % 6, 1 otherwise (2: unreadable)
//   p4check rawseq FILE               FILE: rgb24, one pixel per video frame (ffmpeg crop + scale=1:1). Classifies each
//       frame, collapses runs and counts the steps between colours: +1 is the next colour (expected), 0 nothing new,
//       -1 (5 mod 6) an OLDER colour after a newer one (a stale frame), others a skip. Exit 1 when a stale step is seen.
//   p4check smooth [-t SEC] [-v]         GUI: the IntelXe Manager smoothness test as a command-line tool (CAMetalLayer at
//       the display's rate, 640x360 pt, MTLDrawable presentedTime): fps, frames a refresh or more late, gap spread.
// build: clang -fobjc-arc -O2 -arch x86_64 -arch arm64 -o p4check p4check.m -framework Cocoa -framework Metal
//        -framework QuartzCore -framework ImageIO -framework CoreGraphics
#import <Cocoa/Cocoa.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#import <ImageIO/ImageIO.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const double kPal[6][3] = { { 1, 0, 0 }, { 0, 1, 0 }, { 0, 0, 1 }, { 1, 1, 0 }, { 1, 0, 1 }, { 0, 1, 1 } };
static int classify(double r, double g, double b)   // 0..1 each; nearest palette colour
{
    int best = 0; double bd = 1e9;
    for (int i = 0; i < 6; i++) {
        const double d = (r - kPal[i][0]) * (r - kPal[i][0]) + (g - kPal[i][1]) * (g - kPal[i][1]) + (b - kPal[i][2]) * (b - kPal[i][2]);
        if (d < bd) { bd = d; best = i; }
    }
    return best;
}
static double now_ms(void) { return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e6; }
static void write_file(NSString *path, NSString *text) { [text writeToFile:path atomically:YES encoding:NSUTF8StringEncoding error:nil]; }

// ---- show ----
@interface Shower : NSObject
@property NSWindow *window; @property NSView *view; @property NSString *dir; @property int n, k; @property double freeMs;
@end
@implementation Shower
- (void)setColour:(int)k {
    const double *c = kPal[k % 6];
    [CATransaction begin]; [CATransaction setDisableActions:YES];
    self.view.layer.backgroundColor = [NSColor colorWithSRGBRed:c[0] green:c[1] blue:c[2] alpha:1].CGColor;
    [CATransaction commit]; [CATransaction flush];
}
- (NSString *)rect {   // the patch's centre 100x100 pt, global top-left points (screencapture -R), and the backing scale
    NSRect f = self.window.frame; const CGFloat top = NSScreen.screens[0].frame.size.height;
    return [NSString stringWithFormat:@"%d %d 100 100 %g %d %d", (int)(f.origin.x + 100), (int)(top - NSMaxY(f) + 100), self.window.backingScaleFactor,
            (int)NSScreen.screens[0].frame.size.width, (int)top];
}
- (void)step {
    if (self.k >= self.n) {   // free mode: the last colour stays 3 s more, so a recording ends on it
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (self.freeMs > 0 ? 3000 : 0) * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
            write_file([self.dir stringByAppendingPathComponent:@"done"], @"done\n"); [NSApp terminate:nil]; });
        return;
    }
    [self setColour:self.k];
    if (self.freeMs > 0) {
        NSFileHandle *h = [NSFileHandle fileHandleForWritingAtPath:[self.dir stringByAppendingPathComponent:@"seq"]];
        [h seekToEndOfFile]; [h writeData:[[NSString stringWithFormat:@"%d %.1f\n", self.k, now_ms()] dataUsingEncoding:NSUTF8StringEncoding]]; [h closeFile];
        self.k++;
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(self.freeMs * 1e6)), dispatch_get_main_queue(), ^{ [self step]; });
        return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 150 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        write_file([self.dir stringByAppendingPathComponent:@"state"], [NSString stringWithFormat:@"%d %@\n", self.k, [self rect]]);
        [self waitAck:now_ms()];
    });
}
- (void)waitAck:(double)t0 {
    NSString *a = [NSString stringWithContentsOfFile:[self.dir stringByAppendingPathComponent:@"ack"] encoding:NSUTF8StringEncoding error:nil];
    if ((a && a.intValue == self.k && a.length) || now_ms() - t0 > 10000) {
        if (!(a && a.intValue == self.k)) fprintf(stderr, "show: no ack for %d in 10 s\n", self.k);
        self.k++; [self step]; return;
    }
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 20 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [self waitAck:t0]; });
}
@end
static int cmd_show(int argc, char **argv)
{
    if (argc < 3) return 2;
    Shower *s = [Shower new]; s.dir = @(argv[2]); s.n = 24;
    for (int i = 3; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc) s.n = atoi(argv[++i]);
        else if (!strcmp(argv[i], "-free") && i + 1 < argc) s.freeMs = atof(argv[++i]);
    }
    [NSFileManager.defaultManager createDirectoryAtPath:s.dir withIntermediateDirectories:YES attributes:nil error:nil];
    for (NSString *f in @[ @"state", @"ack", @"done", @"seq" ]) [NSFileManager.defaultManager removeItemAtPath:[s.dir stringByAppendingPathComponent:f] error:nil];
    write_file([s.dir stringByAppendingPathComponent:@"seq"], @"");
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    NSScreen *scr = NSScreen.screens[0];
    const NSRect r = NSMakeRect(NSMinX(scr.frame) + 100, NSMaxY(scr.frame) - 100 - 300, 300, 300);
    s.window = [[NSWindow alloc] initWithContentRect:r styleMask:NSWindowStyleMaskBorderless backing:NSBackingStoreBuffered defer:NO];
    s.window.level = NSStatusWindowLevel; s.window.opaque = YES; s.window.hasShadow = NO;
    s.window.collectionBehavior = NSWindowCollectionBehaviorCanJoinAllSpaces | NSWindowCollectionBehaviorStationary;
    s.view = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, 300, 300)]; s.view.wantsLayer = YES; s.window.contentView = s.view;
    [s.window orderFrontRegardless];
    if (s.freeMs > 0) write_file([s.dir stringByAppendingPathComponent:@"state"], [NSString stringWithFormat:@"-1 %@\n", [s rect]]);
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{ [s step]; });
    // never longer than the test needs (a lost capturer)
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(s.freeMs > 0 ? s.n * s.freeMs + 8000 : s.n * 10500.0 + 5000) * NSEC_PER_MSEC),
                   dispatch_get_main_queue(), ^{ fprintf(stderr, "show: time limit\n"); [NSApp terminate:nil]; });
    [NSApp run];
    return 0;
}

// ---- probe ----
static int cmd_probe(int argc, char **argv)
{
    if (argc < 4) return 2;
    const int want = atoi(argv[3]) % 6;
    CGImageSourceRef src = CGImageSourceCreateWithURL((__bridge CFURLRef)[NSURL fileURLWithPath:@(argv[2])], NULL);
    CGImageRef img = src ? CGImageSourceCreateImageAtIndex(src, 0, NULL) : NULL;
    if (!img) { printf("probe %d unreadable %s\n", want, argv[2]); if (src) CFRelease(src); return 2; }
    const size_t w = CGImageGetWidth(img), h = CGImageGetHeight(img);
    uint8_t *px = calloc(w * h, 4);
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);   // drawn into sRGB: the palette's own space
    CGContextRef ctx = CGBitmapContextCreate(px, w, h, 8, w * 4, cs, (CGBitmapInfo)kCGImageAlphaPremultipliedLast);
    CGContextDrawImage(ctx, CGRectMake(0, 0, w, h), img);
    double r = 0, g = 0, b = 0; size_t n = 0;
    for (size_t y = h / 4; y < h - h / 4; y++) for (size_t x = w / 4; x < w - w / 4; x++) {
        const uint8_t *p = px + (y * w + x) * 4; r += p[0]; g += p[1]; b += p[2]; n++; }
    if (n) { r /= n * 255.0; g /= n * 255.0; b /= n * 255.0; }
    const int got = classify(r, g, b);
    printf("probe %d got %d rgb %.0f %.0f %.0f (%zux%zu)\n", want, got, r * 255, g * 255, b * 255, w, h);
    CGContextRelease(ctx); CGColorSpaceRelease(cs); free(px); CGImageRelease(img); CFRelease(src);
    return got == want ? 0 : 1;
}

// ---- rawseq ----
static int cmd_rawseq(int argc, char **argv)
{
    if (argc < 3) return 2;
    FILE *f = fopen(argv[2], "rb"); if (!f) { printf("rawseq: cannot open %s\n", argv[2]); return 2; }
    uint8_t px[3]; int last = -1; unsigned frames = 0, runs = 0, fwd = 0, stale = 0, skip = 0;
    char seq[4096]; size_t sl = 0; seq[0] = 0;
    while (fread(px, 1, 3, f) == 3) {
        frames++;
        const int c = classify(px[0] / 255.0, px[1] / 255.0, px[2] / 255.0);
        if (c == last) continue;
        if (last >= 0) {
            const int step = (c - last + 6) % 6;
            if (step == 1) fwd++; else if (step == 5) { stale++; printf("rawseq: STALE step at frame %u: colour %d after %d\n", frames, c, last); }
            else skip++;
        }
        runs++; last = c;
        if (sl + 3 < sizeof seq) sl += (size_t)snprintf(seq + sl, sizeof seq - sl, "%d", c);
    }
    fclose(f);
    printf("rawseq: %u frames, %u runs (%s), %u forward, %u stale, %u skipped\n", frames, runs, seq, fwd, stale, skip);
    return stale ? 1 : 0;
}

// ---- smooth (IntelXe Manager's SmoothTest.swift) ----
@interface Smooth : NSObject { @public NSMutableArray<NSNumber *> *presented; NSLock *lock; int frame; NSMutableArray<NSArray<NSNumber *> *> *draws; }
@property NSWindow *window; @property CAMetalLayer *layer; @property id<MTLCommandQueue> queue;
@end
@implementation Smooth
- (void)draw {
    const double t0 = now_ms(); id<CAMetalDrawable> d = [self.layer nextDrawable]; const double t1 = now_ms();
    if (draws) [draws addObject:@[@(t0 / 1e3), @(t1 - t0)]];   // -v: when the app drew, how long nextDrawable blocked
    id<MTLCommandBuffer> cb = [self.queue commandBuffer];
    if (!d || !cb) return;
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    const double t = (double)(frame % 120) / 120; frame++;
    rp.colorAttachments[0].texture = d.texture; rp.colorAttachments[0].loadAction = MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(t, 0.3, 1 - t, 1); rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
    __weak Smooth *w = self;
    [d addPresentedHandler:^(id<MTLDrawable> dr) {
        Smooth *s = w; if (!s || dr.presentedTime <= 0) return;
        [s->lock lock]; [s->presented addObject:@(dr.presentedTime)]; [s->lock unlock];
    }];
    [cb presentDrawable:d]; [cb commit];
}
@end
static int cmd_smooth(int argc, char **argv)
{
    double secs = 5; int verbose = 0;
    for (int i = 2; i < argc; i++) { if (!strcmp(argv[i], "-t") && i + 1 < argc) secs = atof(argv[++i]); else if (!strcmp(argv[i], "-v")) verbose = 1; }
    [NSApplication sharedApplication]; [NSApp setActivationPolicy:NSApplicationActivationPolicyAccessory];
    id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); NSScreen *scr = NSScreen.screens[0];
    if (!dev || !scr) { printf("smooth: no Metal device or screen\n"); return 2; }
    Smooth *s = [Smooth new]; s->presented = [NSMutableArray new]; s->lock = [NSLock new]; if (verbose) s->draws = [NSMutableArray new];
    const NSSize sz = NSMakeSize(640, 360);
    s.window = [[NSWindow alloc] initWithContentRect:NSMakeRect(NSMidX(scr.frame) - sz.width / 2, NSMidY(scr.frame) - sz.height / 2, sz.width, sz.height)
                                           styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    s.window.title = @"p4check smooth"; s.window.level = NSStatusWindowLevel;
    NSView *v = [[NSView alloc] initWithFrame:NSMakeRect(0, 0, sz.width, sz.height)]; v.wantsLayer = YES;
    s.layer = [CAMetalLayer layer]; s.layer.device = dev; s.layer.pixelFormat = MTLPixelFormatBGRA8Unorm; s.layer.framebufferOnly = YES;
    s.layer.drawableSize = CGSizeMake(sz.width * scr.backingScaleFactor, sz.height * scr.backingScaleFactor);
    s.layer.displaySyncEnabled = YES; s.layer.maximumDrawableCount = 3;
    v.layer = s.layer; s.window.contentView = v; s.queue = [dev newCommandQueue];
    [s.window orderFrontRegardless];
    const double hz = scr.maximumFramesPerSecond > 0 ? scr.maximumFramesPerSecond : 60;
    __block double end = 0;
    __block void (^tick)(void);
    void (^__block __weak weakTick)(void);
    tick = ^{
        if (now_ms() >= end) {
            NSArray<NSNumber *> *p; [s->lock lock]; p = [s->presented sortedArrayUsingSelector:@selector(compare:)]; [s->lock unlock];
            const double period = 1.0 / hz; unsigned late = 0; double mn = 1e9, mx = 0, sum = 0, sum2 = 0;
            for (NSUInteger i = 1; i < p.count; i++) {
                const double g = p[i].doubleValue - p[i - 1].doubleValue;
                if (g >= period * 1.5) late++;
                mn = MIN(mn, g); mx = MAX(mx, g); sum += g; sum2 += g * g;
            }
            const NSUInteger ng = p.count > 1 ? p.count - 1 : 0;
            const double span = ng ? p.lastObject.doubleValue - p.firstObject.doubleValue : 0, mean = ng ? sum / ng : 0;
            printf("smooth: %.0f Hz display %gx%g, %lu frames in %.2f s = %.1f fps, %u of %lu late (>= 1.5 refreshes), gap mean %.2f ms min %.2f max %.2f sd %.2f\n",
                   hz, scr.frame.size.width * scr.backingScaleFactor, scr.frame.size.height * scr.backingScaleFactor, (unsigned long)p.count, span,
                   span > 0 ? ng / span : 0, late, (unsigned long)ng, mean * 1e3, (ng ? mn : 0) * 1e3, mx * 1e3,
                   ng ? sqrt(fmax(0, sum2 / ng - mean * mean)) * 1e3 : 0);
            if (verbose) {   // each late gap: when (ms from the first frame), how long, and the app side just before it
                for (NSUInteger i = 1; i < p.count; i++) {
                    const double a = p[i - 1].doubleValue, b = p[i].doubleValue;
                    if (b - a < period * 1.5) continue;
                    double maxwait = 0, maxdraw = 0, prev = 0; unsigned nd = 0;
                    for (NSArray<NSNumber *> *d in s->draws) {   // draws in the 50 ms before the late frame
                        const double td = d[0].doubleValue; if (td < b - 0.05 || td > b) { prev = td; continue; }
                        if (prev > 0) maxdraw = MAX(maxdraw, (td - prev) * 1e3);
                        maxwait = MAX(maxwait, d[1].doubleValue); prev = td; nd++;
                    }
                    printf("late: at %.1f ms gap %.2f ms (%.1f refreshes) | app in the 50 ms before: %u draws, longest draw interval %.2f ms, longest nextDrawable wait %.2f ms\n",
                           (a - p.firstObject.doubleValue) * 1e3, (b - a) * 1e3, (b - a) / period, nd, maxdraw, maxwait);
                }
            }
            fflush(stdout); [NSApp terminate:nil]; return;
        }
        [s draw];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(1e9 / hz / 2)), dispatch_get_main_queue(), weakTick);
    };
    weakTick = tick;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 400 * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
        [s->lock lock]; [s->presented removeAllObjects]; [s->lock unlock];   // after the window settled
        end = now_ms() + secs * 1000; tick(); });
    [NSApp run];
    return 0;
}

int main(int argc, char **argv)
{
    @autoreleasepool {
        setvbuf(stdout, NULL, _IOLBF, 0);
        if (argc >= 2 && !strcmp(argv[1], "show")) return cmd_show(argc, argv);
        if (argc >= 2 && !strcmp(argv[1], "probe")) return cmd_probe(argc, argv);
        if (argc >= 2 && !strcmp(argv[1], "rawseq")) return cmd_rawseq(argc, argv);
        if (argc >= 2 && !strcmp(argv[1], "smooth")) return cmd_smooth(argc, argv);
        fprintf(stderr, "usage: p4check show DIR [-n N] [-free MS] | probe PNG K | rawseq FILE | smooth [-t SEC] [-v]\n");
        return 2;
    }
}
