// SPDX-License-Identifier: MIT
// vtencq: an objective quality check of a VideoToolbox encoder. Encodes deterministic test sources made in memory, decodes
// the encoder's own CMSampleBuffers with VideoToolbox (no container: the format description the encoder made is used as
// is) and compares every decoded picture with its source: PSNR Y/U/V per pattern, bit rate against the target, encode
// speed, key frames, decode errors.
//
//   vtencq [--codec h264|hevc|av1] [--bitrate N] [--cbr] [--10bit] [--frames N] [--size WxH] [--fps F]
//          [--decoder hw|sw|any] [--encoder intelxe|system|<EncoderID>] [--lib libintelxevenc.dylib] [--metal]
//          [--declib libintelxeav1dec.dylib] [--patterns gradient,noise,text,scroll] [--keyint N] [--noreorder]
//          [--realtime] [--dump DIR] [--dump-source] [--list] [--upload copy,import] [--ring K] [--nodecode]
//
// Encoders: --encoder intelxe (default) asks for io.github.yusufklncc.intelxe.videoencoder.<codec>; system lets
// VideoToolbox pick (Apple's encoders on a Mac that has them); anything else is taken as an EncoderID. The IntelXe
// encoders are registered in-process, so one of these has to bring them in:
//   --lib <libintelxevenc.dylib>  dlopen it and call IntelXeVEnc_Register (as vtenctest --lib does)
//   --metal                       create the Metal devices first: the installed IntelXe Metal plug-in registers its
//                                 encoders and decoders in every process that loads it
// Use one of the two: both register the same EncoderIDs. For --codec av1 INTELXEVENC_CODECS is set to
// "hevc,h264,av1" before either runs (unless it is set already), since AV1 is not registered by default.
// Decoders: --decoder sw (default for h264/hevc) asks for a software decoder, independent of the encoder under test;
// hw requires a hardware decoder; any takes what VideoToolbox picks (default for av1: macOS has no AV1 software decoder
// in VideoToolbox, checked on macOS 27.0.1 arm64 - an av01 session fails with -12906 kVTCouldNotFindVideoDecoderErr -
// so AV1 is decoded by the IntelXe AV1 decoder, which --metal or --declib registers).
// Sources (integer arithmetic only, so every machine and architecture makes the same pictures; the source checksum in
// the report shows it): gradient (smooth moving ramps), noise (a band-limited texture moving 3,1 px a frame with
// temporal grain), text (a page of glyph-like 2 px dots being typed and scrolled a line at a time, screen content),
// scroll (fractal "terrain" scrolling 0.75,2.5 px a frame, bilinear sub-pixel motion).
// Bit rate is the sample payload (the parameter sets / av1C in the format description are not counted) over
// frames/fps. PSNR as FFmpeg's psnr filter computes its average: from the mean squared error over all frames; "avg"
// weights the planes Y:U:V 4:1:1. min-Y is the worst single frame. Encode fps is wall time from the first frame in to
// CompleteFrames out (sources are made beforehand when they fit in 1.5 GB; otherwise frames are made on the fly, which
// a * after the fps marks).
// Upload A/B (the IntelXe encoder's INTELXEVENC_UPLOAD): --upload copy,import encodes every pattern once per mode in
// this process (setenv before each session; a mode written MODE:N also sets INTELXEVENC_ASYNC=N, e.g. import:4) and prints, under each pattern row, the mode, the stream's checksum (FNV-1a
// over sample timing, payload and the format description's atoms) and, with --lib, the encoder's own statistics line
// (IntelXeVEnc_LastStats: fps, CPU ms per frame for the upload and the encode call, copy engine uploads, fallbacks,
// imports, evictions); a stream equal to the first mode's reuses its decode, one that differs is flagged, and the last
// line says whether every mode gave the same streams and the speed ratio. --ring K fills K pool buffers before the clock
// starts and hands them over in turn (frame i shows source frame i % K): encoder throughput without the client's
// per-frame fill. --nodecode skips the PSNR decode (speed only).
// Exit status: 0 when every pattern encoded and decoded every frame without errors, 1 otherwise, 2 on usage errors.
// (Not tools/quality/vtencq.m, the encode driver for raw files that rd.sh runs: this one needs no source files.)
// Build (frameworks only; x86_64 for the B580 machine, arm64 to run Apple's encoders as a reference):
//   clang -arch x86_64 -mmacosx-version-min=15.0 -O2 -fobjc-arc vtencq.m -framework Foundation -framework CoreMedia \
//         -framework CoreVideo -framework VideoToolbox -framework Metal -o vtencq && codesign -f -s - vtencq
#import <Foundation/Foundation.h>
#import <VideoToolbox/VideoToolbox.h>
#import <CoreMedia/CoreMedia.h>
#import <CoreVideo/CoreVideo.h>
#import <Metal/Metal.h>
#include <dlfcn.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

extern OSStatus VTRegisterVideoDecoderWithInfo(FourCharCode, CFDictionaryRef, void *);

// ---------------------------------------------------------------------------------------------------------------------
// Sources: 10-bit 4:4:4 rows (Y 64..940, Cb/Cr 64..960), boxed to 4:2:0 and written as NV12 (rounded to 8 bits) or P010

enum { P_GRADIENT, P_NOISE, P_TEXT, P_SCROLL, P_COUNT };
static const char *pat_names[P_COUNT] = { "gradient", "noise", "text", "scroll" };

static int W = 1920, H = 1080, TEN;
static double FPS = 30;

static inline uint32_t mix32(uint32_t h) { h ^= h >> 16; h *= 0x7feb352du; h ^= h >> 15; h *= 0x846ca68bu; h ^= h >> 16; return h; }
static inline uint32_t hash3(uint32_t a, uint32_t b, uint32_t c) { return mix32(a * 0x9e3779b1u ^ mix32(b * 0x85ebca77u ^ mix32(c + 0x27d4eb2fu))); }
static inline int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

// periodic fractal value noise, TS x TS, octaves from cell size c0 down to c1; each octave's amplitude is the previous
// one times fall/8
#define TS 2048
#define TM (TS - 1)
static int16_t *make_tile(uint32_t seed, int c0, int c1, int fall) {
	int16_t *t = malloc((size_t)TS * TS * sizeof *t);
	dispatch_apply(TS, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t yy) {
		const int y = (int)yy;
		for (int x = 0; x < TS; x++) {
			int64_t sum = 0; int amp = 1024, o = 0;
			for (int c = c0; c >= c1; c >>= 1, o++) {
				const int n = TS / c, ix = x / c, iy = y / c;
				const int64_t fx = ((int64_t)(x % c) << 12) / c, fy = ((int64_t)(y % c) << 12) / c;
				const int64_t sx = fx * fx * (12288 - 2 * fx) >> 24, sy = fy * fy * (12288 - 2 * fy) >> 24;   // smoothstep, Q12
				const int64_t a = (int)(hash3(seed + o, ix, iy) & 4095) - 2048, b = (int)(hash3(seed + o, (ix + 1) % n, iy) & 4095) - 2048;
				const int64_t cc = (int)(hash3(seed + o, ix, (iy + 1) % n) & 4095) - 2048, d = (int)(hash3(seed + o, (ix + 1) % n, (iy + 1) % n) & 4095) - 2048;
				const int64_t top = (a << 12) + (b - a) * sx, bot = (cc << 12) + (d - cc) * sx;
				const int64_t v = ((top << 12) + (bot - top) * sy) >> 24;
				sum += v * amp >> 10;
				amp = amp * fall >> 3;
			}
			t[(size_t)y * TS + x] = (int16_t)clampi((int)sum, -32768, 32767);
		}
	});
	return t;
}
static int16_t *tile_noise, *tile_noise_c, *tile_terrain, *tile_terrain_u, *tile_terrain_v;
static uint8_t glyphs[96][10];   // 6x10 dot bitmaps

static void sources_init(int pat) {
	if (pat == P_NOISE && !tile_noise) { tile_noise = make_tile(101, 32, 2, 8); tile_noise_c = make_tile(202, 64, 8, 8); }
	if (pat == P_SCROLL && !tile_terrain) {
		tile_terrain = make_tile(303, 512, 2, 5); tile_terrain_u = make_tile(404, 512, 16, 5); tile_terrain_v = make_tile(505, 512, 16, 5);
	}
	static int have_glyphs;
	if (pat == P_TEXT && !have_glyphs++)
		for (int g = 0; g < 96; g++)
			for (int r = 0; r < 10; r++) {
				// columns 1..4 of rows 1..8 carry the shape (a margin like a font's), about half the dots on
				uint32_t h = hash3(g, r, 0x7e47);
				glyphs[g][r] = r == 0 || r == 9 ? 0 : (uint8_t)(((h & 0xf) | ((h >> 4) & (h >> 8) & 0xf)) << 1);
				if (!glyphs[g][r] && r > 1 && r < 8) glyphs[g][r] = 0x6;
			}
}

static inline int16_t tsample(const int16_t *t, int x, int y) { return t[(size_t)(y & TM) * TS + (x & TM)]; }

static void gen_row(int pat, int y, int t, int16_t *Y, int16_t *U, int16_t *V) {
	switch (pat) {
	case P_GRADIENT:
		for (int x = 0; x < W; x++) {
			const int py = (x + (y >> 1) + 4 * t) & 1023, ty = py < 512 ? py : 1023 - py;
			const int pu = (y + 2 * t + 256) & 1023, tu = pu < 512 ? pu : 1023 - pu;
			const int pv = ((x >> 1) + 1023 * 4 - 3 * t) & 1023, tv = pv < 512 ? pv : 1023 - pv;
			Y[x] = (int16_t)(64 + ty * 876 / 511); U[x] = (int16_t)(512 + (tu - 256) * 3 / 2); V[x] = (int16_t)(512 + (tv - 256) * 3 / 2);
		}
		break;
	case P_NOISE: {
		const int ox = 3 * t, oy = t;
		for (int x = 0; x < W; x++) {
			const int grain = ((int)(hash3(x, y, t + 999) & 7) - 3) * 4;
			Y[x] = (int16_t)clampi(502 + (tsample(tile_noise, x + ox, y + oy) * 36 >> 8) + grain, 64, 940);
			U[x] = (int16_t)clampi(512 + (tsample(tile_noise_c, x + ox, y + oy) * 12 >> 8), 64, 960);
			V[x] = (int16_t)clampi(512 + (tsample(tile_noise_c, y + oy + 777, x + ox) * 12 >> 8), 64, 960);
		}
		break;
	}
	case P_TEXT: {
		enum { CW = 14, CH = 24, GUT = 5 };
		const int scroll = (t / 40) * CH, yy = y + scroll, L = yy / CH, gy = yy % CH;
		const int Lb = (H - 1 + scroll) / CH;                       // the bottom line is being typed
		const int typed = L < Lb ? 1 << 30 : (t % 40) * 4;
		const int len = 20 + (int)(hash3(L, 0, 5) % 100), indent = (int)(hash3(L, 1, 5) % 4) * 2;
		const int kind = (int)(hash3(L, 2, 5) % 6);
		const int inkY = kind == 0 ? 250 : kind == 1 ? 260 : 120, inkU = kind == 0 ? 700 : kind == 1 ? 440 : 512, inkV = kind == 0 ? 430 : kind == 1 ? 700 : 512;
		const int blank_line = hash3(L, 3, 5) % 7 == 0;
		for (int x = 0; x < W; x++) {
			const int cx = x / CW, gx = x % CW;
			int y10 = cx < GUT ? 780 : 920, u10 = 512, v10 = 512, gid = -1, iy = inkY, iu = inkU, iv = inkV;
			if (cx >= 1 && cx <= 3) {                               // line numbers in the gutter
				static const int p10[3] = { 100, 10, 1 };
				gid = (L / p10[cx - 1]) % 10; iy = 400; iu = iv = 512;
			} else if (cx >= GUT && !blank_line) {
				const int c = cx - GUT - indent;
				if (c >= 0 && c < len && c < typed && hash3(L, c, 9) % 6) gid = 10 + (int)(hash3(L, c, 11) % 86);
				if (L == Lb && c == typed && c < len && (t / 15) % 2) { y10 = 120; gid = -1; }   // block cursor
			}
			if (gid >= 0 && gy >= 2 && gy < 22 && gx >= 1 && gx < 13 && (glyphs[gid][(gy - 2) >> 1] >> ((gx - 1) >> 1) & 1)) {
				y10 = iy; u10 = iu; v10 = iv;
			}
			Y[x] = (int16_t)y10; U[x] = (int16_t)u10; V[x] = (int16_t)v10;
		}
		break;
	}
	case P_SCROLL: {
		const int ox = t * 192, oy = t * 640;                       // 0.75 and 2.5 px a frame, Q8
		const int ix = ox >> 8, iy = oy >> 8, fx = ox & 255, fy = oy & 255;
		const int w00 = (256 - fx) * (256 - fy), w10 = fx * (256 - fy), w01 = (256 - fx) * fy, w11 = fx * fy;
#define BIL(tl, X, Y_) ((tsample(tl, X, Y_) * w00 + tsample(tl, (X) + 1, Y_) * w10 + tsample(tl, X, (Y_) + 1) * w01 + tsample(tl, (X) + 1, (Y_) + 1) * w11 + 32768) >> 16)
		for (int x = 0; x < W; x++) {
			const int sx = x + ix, sy = y + iy;
			Y[x] = (int16_t)clampi(502 + (BIL(tile_terrain, sx, sy) * 85 >> 8), 64, 940);
			U[x] = (int16_t)clampi(512 + (BIL(tile_terrain_u, sx, sy) * 24 >> 8), 64, 960);
			V[x] = (int16_t)clampi(512 + (BIL(tile_terrain_v, sx, sy) * 24 >> 8), 64, 960);
		}
#undef BIL
		break;
	}
	}
}

// one frame into NV12/P010 planes
static void gen_frame(int pat, int t, uint8_t *yp, size_t ypitch, uint8_t *uvp, size_t uvpitch) {
	dispatch_apply((size_t)H / 2, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t j) {
		int16_t *b = malloc(sizeof(int16_t) * W * 6), *Y0 = b, *U0 = b + W, *V0 = b + 2 * W, *Y1 = b + 3 * W, *U1 = b + 4 * W, *V1 = b + 5 * W;
		gen_row(pat, (int)j * 2, t, Y0, U0, V0);
		gen_row(pat, (int)j * 2 + 1, t, Y1, U1, V1);
		uint8_t *r0 = yp + 2 * j * ypitch, *r1 = r0 + ypitch, *c = uvp + j * uvpitch;
		for (int x = 0; x < W; x += 2) {
			const int u = (U0[x] + U0[x + 1] + U1[x] + U1[x + 1] + 2) >> 2, v = (V0[x] + V0[x + 1] + V1[x] + V1[x + 1] + 2) >> 2;
			if (TEN) {
				uint16_t *a = (uint16_t *)r0, *d = (uint16_t *)r1, *cc = (uint16_t *)c;
				a[x] = (uint16_t)(Y0[x] << 6); a[x + 1] = (uint16_t)(Y0[x + 1] << 6); d[x] = (uint16_t)(Y1[x] << 6); d[x + 1] = (uint16_t)(Y1[x + 1] << 6);
				cc[x] = (uint16_t)(u << 6); cc[x + 1] = (uint16_t)(v << 6);
			} else {
				r0[x] = (uint8_t)((Y0[x] + 2) >> 2); r0[x + 1] = (uint8_t)((Y0[x + 1] + 2) >> 2);
				r1[x] = (uint8_t)((Y1[x] + 2) >> 2); r1[x + 1] = (uint8_t)((Y1[x + 1] + 2) >> 2);
				c[x] = (uint8_t)((u + 2) >> 2); c[x + 1] = (uint8_t)((v + 2) >> 2);
			}
		}
		free(b);
	});
}

// ---------------------------------------------------------------------------------------------------------------------
// One pattern: its frames (packed, pitch = width) when they fit, the encoded samples, the comparison

typedef struct {
	int pat, nframes;
	size_t bps, ysz, fsz;          // bytes per sample, Y plane bytes, frame bytes (packed)
	uint8_t **frames;              // NULL: made on the fly
	uint64_t checksum;             // FNV-1a over every frame's planes
	int ring;                      // --ring K: only frames 0..K-1 exist, frame i shows frame i % K
} Source;

static uint64_t fnv(uint64_t h, const uint8_t *p, size_t n) { for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ull; } return h; }

static const uint8_t *source_frame(Source *s, int i, uint8_t *scratch) {
	if (s->ring) i %= s->ring;
	if (s->frames) return s->frames[i];
	gen_frame(s->pat, i, scratch, W * s->bps, scratch + s->ysz, W * s->bps);
	return scratch;
}

typedef struct {
	pthread_mutex_t mu;
	CFMutableArrayRef samples;     // CMSampleBuffers in the order the encoder emitted them (decode order)
	long errors, dropped;
} EncOut;

static void enc_cb(void *ref, void *src, OSStatus st, VTEncodeInfoFlags fl, CMSampleBufferRef sb) {
	EncOut *o = ref;
	pthread_mutex_lock(&o->mu);
	if (st || !sb) {
		if (fl & kVTEncodeInfo_FrameDropped) o->dropped++;
		else { o->errors++; if (o->errors < 4) fprintf(stderr, "  encode error %d (flags 0x%x)\n", (int)st, fl); }
	} else CFArrayAppendValue(o->samples, sb);
	pthread_mutex_unlock(&o->mu);
}

typedef struct {
	pthread_mutex_t mu;
	Source *src; uint8_t *scratch;
	double mse[3], minY;           // sums of per-frame MSE; the worst frame's PSNR-Y
	int out, errors, dropped, dups, badfmt;
	uint8_t *seen;
	OSType want;
} DecCtx;

static double psnr(double mse, double peak) { return mse <= 0 ? 99.99 : fmin(99.99, 10 * log10(peak * peak / mse)); }

static void dec_cb(void *ref, void *srcref, OSStatus st, VTDecodeInfoFlags fl, CVImageBufferRef img, CMTime pts, CMTime dur) {
	DecCtx *c = ref;
	pthread_mutex_lock(&c->mu);
	if (st || !img) {
		if (!st && (fl & kVTDecodeInfo_FrameDropped)) c->dropped++;
		else { c->errors++; if (c->errors < 4) fprintf(stderr, "  decode error %d (flags 0x%x)\n", (int)st, fl); }
		pthread_mutex_unlock(&c->mu); return;
	}
	const int i = (int)llround(CMTimeGetSeconds(pts) * FPS);
	const OSType pf = CVPixelBufferGetPixelFormatType(img);
	if (i < 0 || i >= c->src->nframes) { c->errors++; fprintf(stderr, "  decoded picture with pts %.4f s: no such frame\n", CMTimeGetSeconds(pts)); pthread_mutex_unlock(&c->mu); return; }
	if (c->seen[i]) { c->dups++; pthread_mutex_unlock(&c->mu); return; }
	if (pf != c->want || (int)CVPixelBufferGetWidth(img) != W || (int)CVPixelBufferGetHeight(img) != H) {
		if (!c->badfmt++) fprintf(stderr, "  decoder gave %.4s %zux%zu, wanted %.4s %dx%d\n", (char *)&(OSType){ CFSwapInt32HostToBig(pf) },
		                          CVPixelBufferGetWidth(img), CVPixelBufferGetHeight(img), (char *)&(OSType){ CFSwapInt32HostToBig(c->want) }, W, H);
		pthread_mutex_unlock(&c->mu); return;
	}
	c->seen[i] = 1; c->out++;
	const uint8_t *ref_f = source_frame(c->src, i, c->scratch);
	const size_t bps = c->src->bps, rp = W * bps;
	CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	uint64_t sse[3] = { 0, 0, 0 };
	for (int p = 0; p < 2; p++) {
		const uint8_t *d = CVPixelBufferGetBaseAddressOfPlane(img, p); const size_t dp = CVPixelBufferGetBytesPerRowOfPlane(img, p);
		const uint8_t *r = ref_f + (p ? c->src->ysz : 0);
		const int rows = p ? H / 2 : H;
		for (int y = 0; y < rows; y++) {
			const uint8_t *dr = d + y * dp, *rr = r + y * rp;
			if (bps == 2) {
				const uint16_t *a = (const uint16_t *)dr, *b = (const uint16_t *)rr;
				for (int x = 0; x < W; x++) { const int e = (a[x] >> 6) - (b[x] >> 6); sse[p ? 1 + (x & 1) : 0] += (uint64_t)(e * e); }
			} else
				for (int x = 0; x < W; x++) { const int e = dr[x] - rr[x]; sse[p ? 1 + (x & 1) : 0] += (uint64_t)(e * e); }
		}
	}
	CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
	const double ny = (double)W * H, nc = ny / 4, peak = bps == 2 ? 1023 : 255;
	c->mse[0] += sse[0] / ny; c->mse[1] += sse[1] / nc; c->mse[2] += sse[2] / nc;
	const double py = psnr(sse[0] / ny, peak);
	if (py < c->minY) c->minY = py;
	pthread_mutex_unlock(&c->mu);
}

// ---------------------------------------------------------------------------------------------------------------------
// Elementary stream dump (--dump): Annex B for H.264/HEVC, IVF for AV1

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void dump_stream(const char *path, FourCharCode codec, CFArrayRef samples) {
	FILE *f = fopen(path, "wb");
	if (!f) { perror(path); return; }
	for (CFIndex k = 0; k < CFArrayGetCount(samples); k++) {
		CMSampleBufferRef sb = (CMSampleBufferRef)CFArrayGetValueAtIndex(samples, k);
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
		CFDictionaryRef a = att && CFArrayGetCount(att) ? CFArrayGetValueAtIndex(att, 0) : NULL;
		const int key = !(a && CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue);
		CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
		CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
		const size_t len = CMBlockBufferGetDataLength(bb);
		uint8_t *d = malloc(len); CMBlockBufferCopyDataBytes(bb, 0, len, d);
		if (codec == 'av01') {
			if (!k) {
				CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fd);
				uint8_t h[32] = "DKIF"; h[4] = 0; h[5] = 0; h[6] = 32; h[7] = 0; memcpy(h + 8, "AV01", 4);
				h[12] = dim.width; h[13] = dim.width >> 8; h[14] = dim.height; h[15] = dim.height >> 8;
				put_le32(h + 16, (uint32_t)llround(FPS * 1000)); put_le32(h + 20, 1000); put_le32(h + 24, (uint32_t)CFArrayGetCount(samples)); put_le32(h + 28, 0);
				fwrite(h, 1, 32, f);
			}
			CFDictionaryRef atoms = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
			CFDataRef av1c = atoms ? CFDictionaryGetValue(atoms, CFSTR("av1C")) : NULL;
			const size_t seqn = key && av1c && CFDataGetLength(av1c) > 4 ? CFDataGetLength(av1c) - 4 : 0;
			const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb);
			uint8_t fh[12]; put_le32(fh, (uint32_t)(2 + seqn + len));
			const uint64_t idx = (uint64_t)llround(CMTimeGetSeconds(pts) * FPS); put_le32(fh + 4, (uint32_t)idx); put_le32(fh + 8, (uint32_t)(idx >> 32));
			fwrite(fh, 1, 12, f); fwrite("\x12\x00", 1, 2, f);
			if (seqn) fwrite(CFDataGetBytePtr(av1c) + 4, 1, seqn, f);
			fwrite(d, 1, len, f);
		} else {
			if (key) {
				size_t n = 0; int nal = 0;
				if (codec == 'hvc1') CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, 0, NULL, NULL, &n, &nal);
				else CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, 0, NULL, NULL, &n, &nal);
				for (size_t i = 0; i < n; i++) {
					const uint8_t *p; size_t sz;
					OSStatus r = codec == 'hvc1' ? CMVideoFormatDescriptionGetHEVCParameterSetAtIndex(fd, i, &p, &sz, NULL, NULL)
					                             : CMVideoFormatDescriptionGetH264ParameterSetAtIndex(fd, i, &p, &sz, NULL, NULL);
					if (!r) { fwrite("\0\0\0\1", 1, 4, f); fwrite(p, 1, sz, f); }
				}
			}
			for (size_t o = 0; o + 4 <= len;) {
				const uint32_t n = (uint32_t)d[o] << 24 | d[o + 1] << 16 | d[o + 2] << 8 | d[o + 3];
				if (o + 4 + n > len) break;
				fwrite("\0\0\0\1", 1, 4, f); fwrite(d + o + 4, 1, n, f); o += 4 + n;
			}
		}
		free(d);
	}
	fclose(f);
}

// ---------------------------------------------------------------------------------------------------------------------

static void list_encoders(void) {
	CFArrayRef l = NULL;
	VTCopyVideoEncoderList(NULL, &l);
	for (CFIndex i = 0; l && i < CFArrayGetCount(l); i++) {
		NSDictionary *e = (__bridge NSDictionary *)CFArrayGetValueAtIndex(l, i);
		uint32_t t = [e[(__bridge NSString *)kVTVideoEncoderList_CodecType] unsignedIntValue];
		printf("%c%c%c%c  %-50s  %-40s  hw=%s\n", t >> 24, t >> 16, t >> 8, t, [e[(__bridge NSString *)kVTVideoEncoderList_EncoderID] UTF8String],
		       [e[(__bridge NSString *)kVTVideoEncoderList_DisplayName] UTF8String], [[e[(__bridge NSString *)kVTVideoEncoderList_IsHardwareAccelerated] description] UTF8String]);
	}
	if (l) CFRelease(l);
}

typedef struct {
	FourCharCode codec; long bitrate; int cbr, nframes, keyint, reorder, realtime, dec_mode;   // dec_mode 0 any, 1 hw, 2 sw
	const char *encoder, *dump; int dump_source;
	int ring, nodecode;            // --ring K, --nodecode
	const char *upload;            // --upload: this run's INTELXEVENC_UPLOAD (printed with its row), NULL without
} Opts;

typedef struct {
	long frames_out, keys, bytes, enc_err, enc_drop, dec_err, dec_missing; double mse[3], minY, secs; int dec_hw, ok, decoded;
	uint64_t stream;               // FNV-1a over every sample's timing and payload, in output order
	char stats[512];               // the encoder's own line (IntelXeVEnc_LastStats, --lib only)
} Result;

static int (*lib_stats)(char *, size_t);   // IntelXeVEnc_LastStats of the --lib encoder

// ref: an earlier run of the same pattern (another --upload mode); when its stream is the same, its decode is reused
static int run_pattern(const Opts *o, int pat, Result *res, int first, const Result *ref) {
	memset(res, 0, sizeof *res);
	sources_init(pat);
	Source src = { .pat = pat, .nframes = o->nframes, .bps = TEN ? 2 : 1, .ring = o->ring > 0 && o->ring < o->nframes ? o->ring : 0 };
	src.ysz = (size_t)W * H * src.bps; src.fsz = src.ysz * 3 / 2;
	uint8_t *scratch = malloc(src.fsz);
	src.checksum = 1469598103934665603ull;
	const int nmade = src.ring ? src.ring : o->nframes;
	const int store = src.ring || (double)src.fsz * o->nframes <= 1.5e9;
	if (store) src.frames = calloc(nmade, sizeof *src.frames);
	for (int i = 0; i < nmade; i++) {
		uint8_t *f = store ? (src.frames[i] = malloc(src.fsz)) : scratch;
		gen_frame(pat, i, f, W * src.bps, f + src.ysz, W * src.bps);
		src.checksum = fnv(src.checksum, f, src.fsz);
	}
	if (o->dump && o->dump_source) {
		char p[1024]; snprintf(p, sizeof p, "%s/%s_%dx%d_%s.yuv", o->dump, pat_names[pat], W, H, TEN ? "p010" : "nv12");
		FILE *f = fopen(p, "wb");
		for (int i = 0; f && i < o->nframes; i++) fwrite(source_frame(&src, i, scratch), 1, src.fsz, f);
		if (f) fclose(f);
	}

	// encoder
	NSMutableDictionary *spec = [NSMutableDictionary dictionary];
	if (!strcmp(o->encoder, "intelxe"))
		spec[(__bridge NSString *)kVTVideoEncoderSpecification_EncoderID] = o->codec == 'hvc1' ? @"io.github.yusufklncc.intelxe.videoencoder.hevc" :
			o->codec == 'avc1' ? @"io.github.yusufklncc.intelxe.videoencoder.h264" : @"io.github.yusufklncc.intelxe.videoencoder.av1";
	else if (strcmp(o->encoder, "system")) spec[(__bridge NSString *)kVTVideoEncoderSpecification_EncoderID] = @(o->encoder);
	EncOut eo = { .samples = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks) };
	pthread_mutex_init(&eo.mu, NULL);
	VTCompressionSessionRef s = NULL;
	OSStatus r = VTCompressionSessionCreate(NULL, W, H, o->codec, (__bridge CFDictionaryRef)spec, NULL, NULL, enc_cb, &eo, &s);
	if (r) { fprintf(stderr, "VTCompressionSessionCreate (%s) -> %d%s\n", o->encoder, (int)r,
	                 r == kVTCouldNotFindVideoEncoderErr ? " (no such encoder: --lib / --metal registers IntelXe's)" : ""); return -1; }
	if (first) {
		CFStringRef eid = NULL; CFBooleanRef hw = NULL;
		VTSessionCopyProperty(s, kVTCompressionPropertyKey_EncoderID, NULL, &eid);
		VTSessionCopyProperty(s, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, NULL, &hw);
		printf("encoder: %s (hardware %s)\n", eid ? [(__bridge NSString *)eid UTF8String] : "?", hw == kCFBooleanTrue ? "yes" : hw ? "no" : "?");
		if (eid) CFRelease(eid);
		if (hw) CFRelease(hw);
		printf("pattern   frames  key   kbit/s  vs tgt     fps   PSNR-Y PSNR-U PSNR-V    avg  min-Y  dec  err miss  source\n");
	}
#define SET(k, v) do { OSStatus e_ = VTSessionSetProperty(s, k, (__bridge CFTypeRef)(v)); if (e_ && first) fprintf(stderr, "set %s -> %d\n", #k, (int)e_); } while (0)
	SET(kVTCompressionPropertyKey_RealTime, o->realtime ? @YES : @NO);
	SET(kVTCompressionPropertyKey_AllowFrameReordering, o->reorder ? @YES : @NO);
	SET(kVTCompressionPropertyKey_ExpectedFrameRate, @(FPS));
	if (o->keyint) SET(kVTCompressionPropertyKey_MaxKeyFrameInterval, @(o->keyint));
	if (o->cbr) SET(kVTCompressionPropertyKey_ConstantBitRate, @(o->bitrate));
	else SET(kVTCompressionPropertyKey_AverageBitRate, @(o->bitrate));
	if (o->codec == 'hvc1') SET(kVTCompressionPropertyKey_ProfileLevel, TEN ? (__bridge id)kVTProfileLevel_HEVC_Main10_AutoLevel : (__bridge id)kVTProfileLevel_HEVC_Main_AutoLevel);
	if (o->codec == 'avc1') SET(kVTCompressionPropertyKey_ProfileLevel, (__bridge id)kVTProfileLevel_H264_High_AutoLevel);
	SET(kVTCompressionPropertyKey_ColorPrimaries, (__bridge id)kCVImageBufferColorPrimaries_ITU_R_709_2);
	SET(kVTCompressionPropertyKey_TransferFunction, (__bridge id)kCVImageBufferTransferFunction_ITU_R_709_2);
	SET(kVTCompressionPropertyKey_YCbCrMatrix, (__bridge id)kCVImageBufferYCbCrMatrix_ITU_R_709_2);
#undef SET
	r = VTCompressionSessionPrepareToEncodeFrames(s);
	if (r && first) fprintf(stderr, "PrepareToEncodeFrames -> %d\n", (int)r);

	const OSType pf = TEN ? kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange;
	NSDictionary *pba = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(pf), (id)kCVPixelBufferWidthKey: @(W), (id)kCVPixelBufferHeightKey: @(H),
	                       (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	CVPixelBufferPoolRef pool = NULL;
	CVPixelBufferPoolCreate(NULL, NULL, (__bridge CFDictionaryRef)pba, &pool);
	const int32_t ts = (int32_t)llround(FPS * 1000);
	// --ring K: K pool buffers filled before the clock starts and handed over again and again (the encoder's own
	// throughput, without the client's per-frame fill); otherwise a pool buffer is filled for every frame, as a client does
	CVPixelBufferRef ring[64] = { NULL };
	for (int i = 0; i < src.ring; i++) {
		if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &ring[i])) { fprintf(stderr, "CVPixelBufferPoolCreatePixelBuffer failed\n"); res->enc_err++; break; }
		CVPixelBufferLockBaseAddress(ring[i], 0);
		uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(ring[i], 0), *uvp = CVPixelBufferGetBaseAddressOfPlane(ring[i], 1);
		const size_t ypitch = CVPixelBufferGetBytesPerRowOfPlane(ring[i], 0), uvpitch = CVPixelBufferGetBytesPerRowOfPlane(ring[i], 1);
		const uint8_t *f = src.frames[i];
		for (int y = 0; y < H; y++) memcpy(yp + y * ypitch, f + (size_t)y * W * src.bps, W * src.bps);
		for (int y = 0; y < H / 2; y++) memcpy(uvp + y * uvpitch, f + src.ysz + (size_t)y * W * src.bps, W * src.bps);
		CVPixelBufferUnlockBaseAddress(ring[i], 0);
	}
	const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	for (int i = 0; i < o->nframes && !res->enc_err; i++) {
		CVPixelBufferRef pb = NULL;
		if (src.ring) pb = CVPixelBufferRetain(ring[i % src.ring]);
		else {
			if (CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb)) { fprintf(stderr, "CVPixelBufferPoolCreatePixelBuffer failed\n"); res->enc_err++; break; }
			CVPixelBufferLockBaseAddress(pb, 0);
			uint8_t *yp = CVPixelBufferGetBaseAddressOfPlane(pb, 0), *uvp = CVPixelBufferGetBaseAddressOfPlane(pb, 1);
			const size_t ypitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 0), uvpitch = CVPixelBufferGetBytesPerRowOfPlane(pb, 1);
			if (store) {
				const uint8_t *f = src.frames[i];
				for (int y = 0; y < H; y++) memcpy(yp + y * ypitch, f + (size_t)y * W * src.bps, W * src.bps);
				for (int y = 0; y < H / 2; y++) memcpy(uvp + y * uvpitch, f + src.ysz + (size_t)y * W * src.bps, W * src.bps);
			} else gen_frame(pat, i, yp, ypitch, uvp, uvpitch);
			CVPixelBufferUnlockBaseAddress(pb, 0);
		}
		VTEncodeInfoFlags fl = 0;
		r = VTCompressionSessionEncodeFrame(s, pb, CMTimeMake((int64_t)i * 1000, ts), CMTimeMake(1000, ts), NULL, NULL, &fl);
		CVPixelBufferRelease(pb);
		if (r) { fprintf(stderr, "EncodeFrame %d -> %d\n", i, (int)r); res->enc_err++; break; }
	}
	r = VTCompressionSessionCompleteFrames(s, kCMTimeInvalid);
	const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	if (r) { fprintf(stderr, "CompleteFrames -> %d\n", (int)r); res->enc_err++; }
	VTCompressionSessionInvalidate(s); CFRelease(s);
	for (int i = 0; i < src.ring; i++) if (ring[i]) CVPixelBufferRelease(ring[i]);
	CVPixelBufferPoolRelease(pool);
	res->secs = (t1 - t0) / 1e9;
	if (lib_stats) lib_stats(res->stats, sizeof res->stats);
	res->enc_err += eo.errors; res->enc_drop = eo.dropped;
	const CFIndex ns = CFArrayGetCount(eo.samples);
	res->frames_out = ns;
	for (CFIndex k = 0; k < ns; k++) {
		CMSampleBufferRef sb = (CMSampleBufferRef)CFArrayGetValueAtIndex(eo.samples, k);
		CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, false);
		CFDictionaryRef a = att && CFArrayGetCount(att) ? CFArrayGetValueAtIndex(att, 0) : NULL;
		res->keys += !(a && CFDictionaryGetValue(a, kCMSampleAttachmentKey_NotSync) == kCFBooleanTrue);
		res->bytes += CMSampleBufferGetTotalSampleSize(sb);
	}
	// the stream's identity: sample timing and payload in output order, and the format description's atoms (parameter sets)
	res->stream = 1469598103934665603ull;
	for (CFIndex k = 0; k < ns; k++) {
		CMSampleBufferRef sb = (CMSampleBufferRef)CFArrayGetValueAtIndex(eo.samples, k);
		const CMTime pts = CMSampleBufferGetPresentationTimeStamp(sb), dts = CMSampleBufferGetDecodeTimeStamp(sb);
		const int64_t tv[2] = { pts.value, CMTIME_IS_NUMERIC(dts) ? dts.value : -1 };
		res->stream = fnv(res->stream, (const uint8_t *)tv, sizeof tv);
		CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
		const size_t len = bb ? CMBlockBufferGetDataLength(bb) : 0;
		uint8_t *d = len ? malloc(len) : NULL;
		if (d && !CMBlockBufferCopyDataBytes(bb, 0, len, d)) res->stream = fnv(res->stream, d, len);
		free(d);
		if (!k) {
			CFDictionaryRef ex = CMFormatDescriptionGetExtensions(CMSampleBufferGetFormatDescription(sb));
			CFDictionaryRef atoms = ex ? CFDictionaryGetValue(ex, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms) : NULL;
			const void *keys[8], *vals[8]; CFIndex na = atoms ? CFDictionaryGetCount(atoms) : 0;
			if (na > 8) na = 8;
			if (na) CFDictionaryGetKeysAndValues(atoms, keys, vals);
			for (CFIndex q = 0; q < na; q++)
				if (CFGetTypeID(vals[q]) == CFDataGetTypeID()) res->stream = fnv(res->stream, CFDataGetBytePtr(vals[q]), CFDataGetLength(vals[q]));
		}
	}
	if (o->dump && ns) {
		char p[1024]; snprintf(p, sizeof p, "%s/%s.%s", o->dump, pat_names[pat], o->codec == 'av01' ? "ivf" : o->codec == 'hvc1' ? "hevc" : "h264");
		dump_stream(p, o->codec, eo.samples);
	}

	// decoder: the samples as they came out, with the encoder's format description (not again for a stream that is the
	// same as ref's: its numbers are ref's; nothing with --nodecode)
	const int reuse = ref && ref->decoded && ref->stream == res->stream && ref->frames_out == res->frames_out;
	const int decode = !o->nodecode && !reuse;
	DecCtx dc = { .src = &src, .scratch = store ? NULL : scratch, .minY = 99.99, .seen = calloc(o->nframes, 1), .want = pf };
	pthread_mutex_init(&dc.mu, NULL);
	VTDecompressionSessionRef ds = NULL; CMFormatDescriptionRef cur = NULL;
	NSMutableDictionary *dspec = [NSMutableDictionary dictionary];
	if (o->dec_mode == 1) dspec[(__bridge NSString *)kVTVideoDecoderSpecification_RequireHardwareAcceleratedVideoDecoder] = @YES;
	if (o->dec_mode == 2) dspec[(__bridge NSString *)kVTVideoDecoderSpecification_EnableHardwareAcceleratedVideoDecoder] = @NO;
	NSDictionary *dst = @{ (id)kCVPixelBufferPixelFormatTypeKey: @(pf), (id)kCVPixelBufferIOSurfacePropertiesKey: @{} };
	res->dec_hw = -1;
	int dec_failed = 0;
	for (CFIndex k = 0; decode && k < ns && !dec_failed; k++) {
		CMSampleBufferRef sb = (CMSampleBufferRef)CFArrayGetValueAtIndex(eo.samples, k);
		CMFormatDescriptionRef fd = CMSampleBufferGetFormatDescription(sb);
		if (!ds || (fd != cur && !CMFormatDescriptionEqual(fd, cur) && !VTDecompressionSessionCanAcceptFormatDescription(ds, fd))) {
			if (ds) { VTDecompressionSessionFinishDelayedFrames(ds); VTDecompressionSessionWaitForAsynchronousFrames(ds); VTDecompressionSessionInvalidate(ds); CFRelease(ds); ds = NULL; }
			VTDecompressionOutputCallbackRecord rec = { dec_cb, &dc };
			r = VTDecompressionSessionCreate(NULL, fd, (__bridge CFDictionaryRef)dspec, (__bridge CFDictionaryRef)dst, &rec, &ds);
			if (r) {
				fprintf(stderr, "VTDecompressionSessionCreate (decoder %s) -> %d%s\n", o->dec_mode == 1 ? "hw" : o->dec_mode == 2 ? "sw" : "any", (int)r,
				        r == kVTCouldNotFindVideoDecoderErr ? " (no decoder: for AV1 use --metal or --declib and --decoder any|hw)" : "");
				dec_failed = 1; ds = NULL; break;
			}
			CFBooleanRef hw = NULL;
			VTSessionCopyProperty(ds, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder, NULL, &hw);
			res->dec_hw = hw == kCFBooleanTrue ? 1 : 0;
			if (hw) CFRelease(hw);
			cur = fd;
		}
		VTDecodeInfoFlags fl = 0;
		r = VTDecompressionSessionDecodeFrame(ds, sb, kVTDecodeFrame_EnableAsynchronousDecompression, NULL, &fl);
		if (r) { pthread_mutex_lock(&dc.mu); dc.errors++; pthread_mutex_unlock(&dc.mu); if (dc.errors < 4) fprintf(stderr, "  DecodeFrame %ld -> %d\n", (long)k, (int)r); }
	}
	if (ds) { VTDecompressionSessionFinishDelayedFrames(ds); VTDecompressionSessionWaitForAsynchronousFrames(ds); VTDecompressionSessionInvalidate(ds); CFRelease(ds); }
	if (decode) {
		res->decoded = 1;
		res->dec_err = dc.errors + dc.badfmt + dc.dups + (dec_failed ? 1 : 0);
		res->dec_missing = o->nframes - dc.out;
		for (int p = 0; p < 3; p++) res->mse[p] = dc.out ? dc.mse[p] / dc.out : 0;
		res->minY = dc.minY;
	} else if (reuse) {
		res->decoded = 1; res->dec_hw = ref->dec_hw; res->dec_err = ref->dec_err; res->dec_missing = ref->dec_missing;
		for (int p = 0; p < 3; p++) res->mse[p] = ref->mse[p];
		res->minY = ref->minY; dc.out = o->nframes - (int)ref->dec_missing;
	} else res->minY = 99.99;
	res->ok = !res->enc_err && !res->enc_drop && res->frames_out == o->nframes && !res->dec_err && (!res->decoded || !res->dec_missing) && !dec_failed;

	const double peak = TEN ? 1023 : 255, dur = o->nframes / FPS, kbps = res->bytes * 8.0 / 1000 / dur, tgt = o->bitrate / 1000.0;
	const double avg = (4 * res->mse[0] + res->mse[1] + res->mse[2]) / 6;
	if (dc.out)
		printf("%-8s %4ld/%-4d %3ld %8.0f %+6.1f%% %7.1f%s  %6.2f %6.2f %6.2f %6.2f %6.2f  %-4s %3ld %4ld  %016llx\n", pat_names[pat], res->frames_out, o->nframes,
		       res->keys, kbps, (kbps / tgt - 1) * 100, o->nframes / res->secs, store ? " " : "*", psnr(res->mse[0], peak), psnr(res->mse[1], peak),
		       psnr(res->mse[2], peak), psnr(avg, peak), res->minY, res->dec_hw < 0 ? "-" : res->dec_hw ? "hw" : "sw", res->dec_err + res->enc_err + res->enc_drop,
		       res->dec_missing, (unsigned long long)src.checksum);
	else
		printf("%-8s %4ld/%-4d %3ld %8.0f %+6.1f%% %7.1f%s  %6s %6s %6s %6s %6s  %-4s %3ld %4ld  %016llx\n", pat_names[pat], res->frames_out, o->nframes, res->keys,
		       kbps, (kbps / tgt - 1) * 100, o->nframes / res->secs, store ? " " : "*", "-", "-", "-", "-", "-", res->dec_hw < 0 ? "-" : res->dec_hw ? "hw" : "sw",
		       res->dec_err + res->enc_err + res->enc_drop, res->dec_missing, (unsigned long long)src.checksum);
	if (o->upload || res->stats[0])
		printf("  %-7s stream %016llx%s%s%s\n", o->upload ? o->upload : "", (unsigned long long)res->stream, reuse ? " (same stream: decode reused)" : "",
		       res->stats[0] ? " | " : "", res->stats);
	fflush(stdout);

	CFRelease(eo.samples);
	free(dc.seen); free(scratch);
	if (store) { for (int i = 0; i < nmade; i++) free(src.frames[i]); free(src.frames); }
	return res->ok ? 0 : 1;
}

static void usage(void) {
	fprintf(stderr, "usage: vtencq [--codec h264|hevc|av1] [--bitrate N] [--cbr] [--10bit] [--frames N] [--size WxH] [--fps F]\n"
	                "              [--decoder hw|sw|any] [--encoder intelxe|system|<EncoderID>] [--lib libintelxevenc.dylib] [--metal]\n"
	                "              [--declib libintelxeav1dec.dylib] [--patterns gradient,noise,text,scroll] [--keyint N] [--noreorder]\n"
	                "              [--realtime] [--dump DIR] [--dump-source] [--list] [--upload copy,import] [--ring K] [--nodecode]\n");
}

int main(int argc, char **argv) {
	@autoreleasepool {
		setvbuf(stdout, NULL, _IOLBF, 0);   // lines in order with stderr when piped (ssh | tee)
		Opts o = { .codec = 'hvc1', .nframes = 120, .reorder = 1, .dec_mode = -1, .encoder = "intelxe" };
		const char *lib = NULL, *declib = NULL, *pats = NULL, *uploads = NULL; int metal = 0, listonly = 0;
		for (int i = 1; i < argc; i++) {
			const char *a = argv[i]; const int more = i + 1 < argc;
			if (!strcmp(a, "--codec") && more) {
				const char *c = argv[++i];
				if (!strcmp(c, "hevc")) o.codec = 'hvc1'; else if (!strcmp(c, "h264")) o.codec = 'avc1'; else if (!strcmp(c, "av1")) o.codec = 'av01';
				else { usage(); return 2; }
			}
			else if (!strcmp(a, "--bitrate") && more) o.bitrate = atol(argv[++i]);
			else if (!strcmp(a, "--cbr")) o.cbr = 1;
			else if (!strcmp(a, "--10bit")) TEN = 1;
			else if (!strcmp(a, "--frames") && more) o.nframes = atoi(argv[++i]);
			else if (!strcmp(a, "--size") && more) { const char *sz = argv[++i]; W = atoi(sz); const char *xp = strchr(sz, 'x'); H = xp ? atoi(xp + 1) : 0; }
			else if (!strcmp(a, "--fps") && more) FPS = atof(argv[++i]);
			else if (!strcmp(a, "--decoder") && more) {
				const char *d = argv[++i];
				o.dec_mode = !strcmp(d, "any") ? 0 : !strcmp(d, "hw") ? 1 : !strcmp(d, "sw") ? 2 : -2;
				if (o.dec_mode == -2) { usage(); return 2; }
			}
			else if (!strcmp(a, "--encoder") && more) o.encoder = argv[++i];
			else if (!strcmp(a, "--lib") && more) lib = argv[++i];
			else if (!strcmp(a, "--declib") && more) declib = argv[++i];
			else if (!strcmp(a, "--metal")) metal = 1;
			else if (!strcmp(a, "--patterns") && more) pats = argv[++i];
			else if (!strcmp(a, "--keyint") && more) o.keyint = atoi(argv[++i]);
			else if (!strcmp(a, "--noreorder")) o.reorder = 0;
			else if (!strcmp(a, "--realtime")) o.realtime = 1;
			else if (!strcmp(a, "--dump") && more) o.dump = argv[++i];
			else if (!strcmp(a, "--dump-source")) o.dump_source = 1;
			else if (!strcmp(a, "--list")) listonly = 1;
			else if (!strcmp(a, "--upload") && more) uploads = argv[++i];
			else if (!strcmp(a, "--ring") && more) { o.ring = atoi(argv[++i]); if (o.ring < 0 || o.ring > 64) { usage(); return 2; } }
			else if (!strcmp(a, "--nodecode")) o.nodecode = 1;
			else { usage(); return 2; }
		}
		if (W < 64 || H < 64 || (W | H) & 1 || o.nframes < 1 || FPS <= 0 || (TEN && o.codec == 'avc1')) {
			fprintf(stderr, "vtencq: bad size, frame count, frame rate, or 10-bit H.264\n"); return 2;
		}
		if (!o.bitrate) o.bitrate = o.codec == 'av01' ? 6000000 : 8000000;
		if (o.dec_mode < 0) o.dec_mode = o.codec == 'av01' ? 0 : 2;
		if (o.dump) mkdir(o.dump, 0755);

		// IntelXe's AV1 encoder is registered only when asked for (IntelXeVEnc_Register reads this, in-process)
		if (o.codec == 'av01' && !getenv("INTELXEVENC_CODECS")) setenv("INTELXEVENC_CODECS", "hevc,h264,av1", 1);
		if (lib) {
			void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
			int (*reg)(void) = h ? (int (*)(void))dlsym(h, "IntelXeVEnc_Register") : NULL;
			if (!reg) { fprintf(stderr, "dlopen %s: %s\n", lib, dlerror()); return 1; }
			printf("IntelXeVEnc_Register (%s) -> %d encoders\n", lib, reg());
			lib_stats = (int (*)(char *, size_t))dlsym(h, "IntelXeVEnc_LastStats");   // builds from 12 Nov on
		}
		if (declib) {
			void *h = dlopen(declib, RTLD_NOW | RTLD_LOCAL); void *f = h ? dlsym(h, "IntelXeAV1_CreateInstance") : NULL;
			if (!f) { fprintf(stderr, "dlopen %s: %s\n", declib, dlerror()); return 1; }
			NSDictionary *info = @{ @"CMClassImplementationID": @"io.github.yusufklncc.intelxe.videodecoder.av1", @"VTIsHardwareAccelerated": @YES, @"VTRating": @1000 };
			printf("AV1 decoder from %s: VTRegisterVideoDecoderWithInfo -> %d\n", declib, (int)VTRegisterVideoDecoderWithInfo('av01', (__bridge CFDictionaryRef)info, f));
		}
		if (metal) {
			const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
			NSArray<id<MTLDevice>> *devs = MTLCopyAllDevices();
			printf("Metal: %lu device(s) (%.0f ms):", (unsigned long)devs.count, (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
			for (id<MTLDevice> d in devs) printf(" [%s]", d.name.UTF8String);
			printf("\n");
		}
		if (listonly) { list_encoders(); return 0; }

		int sel[P_COUNT], nsel = 0;
		if (pats) {
			char *dup = strdup(pats), *save = NULL;
			for (char *t = strtok_r(dup, ",", &save); t; t = strtok_r(NULL, ",", &save)) {
				int k = 0; while (k < P_COUNT && strcmp(t, pat_names[k])) k++;
				if (k == P_COUNT) { fprintf(stderr, "vtencq: no pattern %s\n", t); return 2; }
				sel[nsel++] = k;
			}
			free(dup);
		} else for (int k = 0; k < P_COUNT; k++) sel[nsel++] = k;

		printf("vtencq: %s %dx%d %s, %d frames per pattern at %.3g fps, %s %.0f kbit/s, %s, decoder %s, VTIsHardwareDecodeSupported %d\n",
		       o.codec == 'hvc1' ? "HEVC" : o.codec == 'avc1' ? "H.264" : "AV1", W, H, TEN ? "10-bit" : "8-bit", o.nframes, FPS, o.cbr ? "CBR" : "VBR",
		       o.bitrate / 1000.0, o.reorder ? "reordering" : "no reordering", o.dec_mode == 1 ? "hw" : o.dec_mode == 2 ? "sw" : "any", VTIsHardwareDecodeSupported(o.codec));
		// --upload copy,import: every pattern once per INTELXEVENC_UPLOAD mode, in this process (the encoder reads the variable
		// when its codec opens); a mode's stream that equals the first mode's reuses its decode
		const char *modes[4]; int nmodes = 0; char *updup = uploads ? strdup(uploads) : NULL;
		if (updup) { char *save = NULL; for (char *t = strtok_r(updup, ",", &save); t && nmodes < 4; t = strtok_r(NULL, ",", &save)) modes[nmodes++] = t; }
		if (!nmodes) modes[nmodes++] = NULL;
		char *async0 = getenv("INTELXEVENC_ASYNC") ? strdup(getenv("INTELXEVENC_ASYNC")) : NULL;   // restored for modes without :N
		int fails = 0, printed = 0, differ = 0;
		Result tot[4]; double mse_sum[4][3]; int with_psnr[4];
		for (int m = 0; m < 4; m++) { memset(&tot[m], 0, sizeof tot[m]); tot[m].minY = 99.99; mse_sum[m][0] = mse_sum[m][1] = mse_sum[m][2] = 0; with_psnr[m] = 0; }
		for (int k = 0; k < nsel; k++) {
			Result first_res;
			for (int m = 0; m < nmodes; m++) {
				Result res;
				if (modes[m]) {   // MODE or MODE:N (N = INTELXEVENC_ASYNC, libavcodec's encode depth)
					char mb[32]; strlcpy(mb, modes[m], sizeof mb);
					char *colon = strchr(mb, ':');
					if (colon) { *colon = 0; setenv("INTELXEVENC_ASYNC", colon + 1, 1); }
					else if (async0) setenv("INTELXEVENC_ASYNC", async0, 1);
					else unsetenv("INTELXEVENC_ASYNC");
					setenv("INTELXEVENC_UPLOAD", mb, 1);
				}
				o.upload = modes[m];
				int rc = run_pattern(&o, sel[k], &res, !printed, m ? &first_res : NULL);
				printed = 1;
				if (rc < 0) return 1;
				fails += rc;
				if (!m) first_res = res;
				else if (res.stream != first_res.stream) {
					differ++;
					printf("  %s: %s stream differs from %s (compare the PSNR rows)\n", pat_names[sel[k]], modes[m], modes[0]);
				}
				Result *t = &tot[m];
				t->frames_out += res.frames_out; t->keys += res.keys; t->bytes += res.bytes; t->secs += res.secs;
				t->enc_err += res.enc_err + res.enc_drop; t->dec_err += res.dec_err; t->dec_missing += res.dec_missing;
				if (res.decoded && res.frames_out - res.dec_missing > 0 && res.dec_missing < o.nframes) { for (int p = 0; p < 3; p++) mse_sum[m][p] += res.mse[p]; with_psnr[m]++; }
				if (res.minY < t->minY) t->minY = res.minY;
			}
		}
		const double peak = TEN ? 1023 : 255, tgt = o.bitrate / 1000.0;
		for (int m = 0; m < nmodes; m++) {
			const Result *t = &tot[m];
			const double kbps = t->bytes * 8.0 / 1000 / (nsel * o.nframes / FPS);
			char name[16]; snprintf(name, sizeof name, "%s", modes[m] ? modes[m] : "all");
			if (with_psnr[m]) {
				for (int p = 0; p < 3; p++) mse_sum[m][p] /= with_psnr[m];
				printf("%-8s %4ld/%-4d %3ld %8.0f %+6.1f%% %7.1f   %6.2f %6.2f %6.2f %6.2f %6.2f       %3ld %4ld\n", name, t->frames_out, nsel * o.nframes, t->keys, kbps,
				       (kbps / tgt - 1) * 100, nsel * o.nframes / t->secs, psnr(mse_sum[m][0], peak), psnr(mse_sum[m][1], peak), psnr(mse_sum[m][2], peak),
				       psnr((4 * mse_sum[m][0] + mse_sum[m][1] + mse_sum[m][2]) / 6, peak), t->minY, t->dec_err + t->enc_err, t->dec_missing);
			} else
				printf("%-8s %4ld/%-4d %3ld %8.0f %+6.1f%% %7.1f   (not decoded)                              %3ld\n", name, t->frames_out, nsel * o.nframes, t->keys, kbps,
				       (kbps / tgt - 1) * 100, nsel * o.nframes / t->secs, t->enc_err);
		}
		if (nmodes > 1) {
			printf("UPLOAD: %s", differ ? "streams DIFFER between modes" : "bit-identical streams in every mode");
			for (int m = 1; m < nmodes; m++) printf(" | %s/%s fps %.3f", modes[m], modes[0], tot[0].secs > 0 ? tot[0].secs / tot[m].secs : 0.0);
			printf("\n");
		}
		free(updup); free(async0);
		printf("%s\n", fails ? "RESULT: errors (see err/miss)" : "RESULT: ok");
		return fails ? 1 : 0;
	}
}
