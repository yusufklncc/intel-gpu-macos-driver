// SPDX-License-Identifier: MIT
// vtdec <movie> [frames] [--register <decoder binary>] [--lib <libintelxeav1dec.dylib>] [--md5] [--async] [--quiet]:
// decode a movie's video track through VTDecompressionSession (hardware decoder required), report which decoder ran
// (hardware or not), frames, a checksum per frame, the time and the CPU it took.
// --register: register the given decoder binary's IntelXeAV1_CreateInstance for av01 in this process (stage S1(a)).
// --lib: register a test build's decoders (AV1, H.264, HEVC, VP9) in this process, nothing installed; at the end its
//        statistics line (IntelXeDec_LastStats: which path decoded, fps, CPU and wall ms per DecodeFrame, latency).
// --md5: a line per output picture: "frame N pts P WxH fmt F Y[0]=y md5 M" (the visible planes; H.264 / HEVC come in
//        decode order, so two runs compare line by line). --async: kVTDecodeFrame_EnableAsynchronousDecompression (as
//        AVFoundation decodes; the default waits for each picture). frames: how many samples (default 60).
// The last line: "RESULT frames N errors E samples S wall_ms W ms_per_frame M cpu_pct C" (wall and CPU from the first
// sample to the last picture, the whole process: VideoToolbox, the decoder, the checksums if --md5).
#import <Foundation/Foundation.h>
#import <AVFoundation/AVFoundation.h>
#import <VideoToolbox/VideoToolbox.h>
#include <dlfcn.h>
#include <sys/resource.h>
#include <CommonCrypto/CommonDigest.h>
extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode, CFDictionaryRef, void *);
static int frames, bad, md5, quiet;
static void cb(void *ref, void *src, OSStatus st, VTDecodeInfoFlags f, CVImageBufferRef img, CMTime pts, CMTime dur) {
	if (st || !img) { if (st || !(f & kVTDecodeInfo_FrameDropped)) { bad++; if (bad < 4 && !quiet) printf("  frame error %d\n", (int)st); } return; }
	frames++;
	if (md5 || (frames <= 2 && !quiet)) {
		CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
		unsigned char h[16]; CC_MD5_CTX c; CC_MD5_Init(&c);
		const OSType pf = CVPixelBufferGetPixelFormatType(img);
		const int ten = pf == 'x420' || pf == 'xf20';
		for (size_t p = 0; p < CVPixelBufferGetPlaneCount(img); p++) {
			const uint8_t *b = CVPixelBufferGetBaseAddressOfPlane(img, p); const size_t bpr = CVPixelBufferGetBytesPerRowOfPlane(img, p);
			const size_t w = CVPixelBufferGetWidthOfPlane(img, p) * (p ? 2 : 1) * (ten ? 2 : 1);
			for (size_t y = 0; y < CVPixelBufferGetHeightOfPlane(img, p); y++) CC_MD5_Update(&c, b + y * bpr, (CC_LONG)w);
		}
		CC_MD5_Final(h, &c);
		const uint8_t *y0 = CVPixelBufferGetBaseAddressOfPlane(img, 0);
		printf("%s frame %d pts %lld %zux%zu fmt %.4s Y[0]=%u md5 ", md5 ? "" : " ", frames, CMTIME_IS_VALID(pts) ? pts.value : -1,
		       CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img), (char *)&(uint32_t){ CFSwapInt32HostToBig(pf) }, y0 ? y0[0] : 0);
		for (int i = 0; i < 16; i++) printf("%02x", h[i]);
		printf("\n");
		CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	}
}
static uint64_t cpu_ns(void) {
	struct rusage ru; getrusage(RUSAGE_SELF, &ru);
	return (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
}
static void reg(void *h, const char *sym, FourCharCode codec, NSString *cid) {
	void *f = h ? dlsym(h, sym) : NULL;
	NSDictionary *info = @{ @"CMClassImplementationID": cid, @"VTIsHardwareAccelerated": @YES, @"VTRating": @1000 };
	const OSStatus r = f ? VTRegisterVideoDecoderWithInfo(codec, (__bridge CFDictionaryRef)info, f) : -1;
	if (!quiet) printf("register %s (%.4s): %d\n", sym, (char *)&(uint32_t){ CFSwapInt32HostToBig(codec) }, (int)r);
}
#include "pickdev.h"
int main(int argc, char **argv) { @autoreleasepool {
	if (getenv("VT_INTELXE_METAL")) { id<MTLDevice> d = mtl_pick_intelxe(); printf("metal: %s\n", d ? d.name.UTF8String : "none"); }
	if (argc < 2) { printf("usage: vtdec <movie> [frames] [--register bin] [--lib libintelxeav1dec.dylib] [--md5] [--async] [--quiet]\n"); return 2; }
	int max = 60, async = 0; const char *registered = NULL, *lib = NULL;
	for (int a = 2; a < argc; a++) {
		if (!strcmp(argv[a], "--register") && a + 1 < argc) registered = argv[++a];
		else if (!strcmp(argv[a], "--lib") && a + 1 < argc) lib = argv[++a];
		else if (!strcmp(argv[a], "--md5")) md5 = 1;
		else if (!strcmp(argv[a], "--async")) async = 1;
		else if (!strcmp(argv[a], "--quiet")) quiet = 1;
		else max = atoi(argv[a]);
	}
	if (registered) {
		void *h = dlopen(registered, RTLD_NOW); void *f = h ? dlsym(h, "IntelXeAV1_CreateInstance") : NULL;
		NSDictionary *info = @{ @"CMClassImplementationID": @"io.github.yusufklncc.intelxe.videodecoder.av1", @"VTIsHardwareAccelerated": @YES, @"VTRating": @1000 };
		printf("register %s: %d\n", registered, f ? (int)VTRegisterVideoDecoderWithInfo('av01', (__bridge CFDictionaryRef)info, f) : -1);
	}
	int (*last_stats)(char *, size_t) = NULL;
	if (lib) {
		void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
		if (!h) { printf("dlopen %s: %s\n", lib, dlerror()); return 1; }
		reg(h, "IntelXeAV1_CreateInstance", 'av01', @"io.github.yusufklncc.intelxe.videodecoder.av1");
		reg(h, "IntelXeH264Dec_CreateInstance", 'avc1', @"io.github.yusufklncc.intelxe.videodecoder.h264");
		reg(h, "IntelXeHEVCDec_CreateInstance", 'hvc1', @"io.github.yusufklncc.intelxe.videodecoder.hevc");
		reg(h, "IntelXeHEVCDec_CreateInstance", 'hev1', @"io.github.yusufklncc.intelxe.videodecoder.hevc");
		// VideoToolbox looks 'vp09' decoders up only after VTRegisterSupplementalVideoDecoderIfAvailable('vp09') (as the
		// Metal plug-in does before it registers ours; vtdirect-ab.sh keeps the plug-in's registration out)
		void (*sup)(uint32_t) = (void (*)(uint32_t))dlsym(RTLD_DEFAULT, "VTRegisterSupplementalVideoDecoderIfAvailable");
		if (sup) sup('vp09');
		reg(h, "IntelXeVP9Dec_CreateInstance", 'vp09', @"io.github.yusufklncc.intelxe.videodecoder.vp9");
		last_stats = (int (*)(char *, size_t))dlsym(h, "IntelXeDec_LastStats");   // builds from 13 Nov on
	}
	if (!quiet) printf("VTIsHardwareDecodeSupported('av01') = %d\n", VTIsHardwareDecodeSupported('av01'));
	AVURLAsset *asset = [AVURLAsset URLAssetWithURL:[NSURL fileURLWithPath:@(argv[1])] options:nil];
	__block AVAssetTrack *track = nil; dispatch_semaphore_t loaded = dispatch_semaphore_create(0);
	[asset loadTracksWithMediaType:AVMediaTypeVideo completionHandler:^(NSArray<AVAssetTrack *> *tracks, NSError *error) {
		track = tracks.firstObject; dispatch_semaphore_signal(loaded); }];
	dispatch_semaphore_wait(loaded, DISPATCH_TIME_FOREVER);
	if (!track) { printf("no video track\n"); return 1; }
	NSError *e = nil;
	AVAssetReader *r = [AVAssetReader assetReaderWithAsset:asset error:&e];
	AVAssetReaderTrackOutput *out = [AVAssetReaderTrackOutput assetReaderTrackOutputWithTrack:track outputSettings:nil];
	[r addOutput:out]; [r startReading];
	VTDecompressionSessionRef s = NULL; CMVideoFormatDescriptionRef fd = NULL;
	uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), c0 = cpu_ns(); int sent = 0;
	for (CMSampleBufferRef sb; sent < max && (sb = [out copyNextSampleBuffer]); CFRelease(sb)) {
		if (!CMSampleBufferGetNumSamples(sb) || !CMSampleBufferGetFormatDescription(sb)) continue;
		if (!s) {
			fd = (CMVideoFormatDescriptionRef)CMSampleBufferGetFormatDescription(sb);
			NSDictionary *spec = getenv("VTDEC_NOSPEC") ? nil : @{ (id)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder: @YES };
			if (getenv("VTDEC_FD")) printf("format: %s\n", [[(__bridge id)fd description] UTF8String]);
			VTDecompressionOutputCallbackRecord rec = { (VTDecompressionOutputCallback)cb, NULL };
			OSStatus st = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)spec, NULL, &rec, &s);
			CFBooleanRef hw = NULL;
			if (!st) VTSessionCopyProperty(s, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
			printf("session (%.4s, hardware required) -> %d, using hardware %s\n", (char *)&(uint32_t){ CFSwapInt32HostToBig(CMFormatDescriptionGetMediaSubType(fd)) },
			       (int)st, hw == kCFBooleanTrue ? "yes" : "no");
			if (st) return 1;
			t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); c0 = cpu_ns();   // from the first sample: the session's start is not counted
		}
		if (!CMSampleBufferGetNumSamples(sb)) continue;
		VTDecompressionSessionDecodeFrame(s, sb, async ? kVTDecodeFrame_EnableAsynchronousDecompression : 0, NULL, NULL); sent++;
	}
	if (s) VTDecompressionSessionWaitForAsynchronousFrames(s);
	const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW), c1 = cpu_ns();
	if (s) { VTDecompressionSessionInvalidate(s); CFRelease(s); }
	const double wall = (t1 - t0) / 1e6;
	if (!quiet) printf("%d samples sent, %d frames out, %d errors, %.1f ms\n", sent, frames, bad, wall);
	if (last_stats) {
		char line[1024] = "";
		last_stats(line, sizeof line);
		printf("DECODER %s\n", line[0] ? line : "(none: the --lib decoder did not run; another decoder took the stream)");
	}
	printf("RESULT frames %d errors %d samples %d wall_ms %.1f ms_per_frame %.3f cpu_pct %.0f\n", frames, bad, sent, wall,
	       frames ? wall / frames : 0.0, wall > 0 ? 100.0 * (c1 - c0) / 1e6 / wall : 0.0);
	return 0; } }
