// SPDX-License-Identifier: MIT
// vp9parse_test <stream.ivf> [golden.txt]: the direct path's VP9 parser (vtdirect_vp9.c) over a recorded stream,
// without a GPU. Each IVF frame is handed over as the sample an MP4 / WebM carries (a frame or a superframe). Prints,
// in the format of tests/vp9/mkgolden.py (made from FFmpeg 9.0.2's trace_headers and the same rules written again in
// Python; the format is described there):
//   frame / refs / lf / seg / lavc   a decoded frame: its Std picture information, the DPB slot, the references, where
//                                    its headers are in the sample; "lavc" where libavcodec's hwaccel passes other
//                                    segmentation values (see vtdirect_vp9.h)
//   show ...                         a show_existing_frame
//   tu ...                           the sample: its frames and the one shown
// With a golden file it compares line by line and exits 1 at the first difference.
//   vp9parse_test --fuzz <seed> <iterations> <stream>: the samples damaged at random (bytes changed, cut, replaced),
//   parsed again and again: no crash, no read outside the sample (build with -fsanitize=address,undefined), and
//   whatever the parser hands out stays in range (frame offsets in the sample, DPB slots, references).
// Build (here, no FFmpeg): cc -O1 -Wall -I<mesa>/include vp9parse_test.c ../vtdirect_vp9.c -o vp9parse_test
#include "../vtdirect_vp9.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *golden; static int lineno, bad;
static void emit(const char *l) {
	lineno++;
	if (!golden) { printf("%s\n", l); return; }
	char g[8192];
	if (!fgets(g, sizeof g, golden)) { if (!bad) printf("line %d: golden ends; got: %s\n", lineno, l); bad = 1; return; }
	g[strcspn(g, "\n")] = 0;
	if (strcmp(g, l) && !bad) { printf("line %d differs\n  want: %s\n  got:  %s\n", lineno, g, l); bad = 1; }
}

typedef struct { char *b; size_t n, o; } Str;
static void add(Str *s, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void add(Str *s, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	if (s->o < s->n) s->o += (size_t)vsnprintf(s->b + s->o, s->n - s->o, fmt, ap);
	va_end(ap);
	if (s->o >= s->n) s->o = s->n - 1;
}
#define LINE(...) do { char b_[4096]; Str o_ = { b_, sizeof b_, 0 }; add(&o_, __VA_ARGS__); emit(b_); } while (0)

static void bytes(Str *o, const uint8_t *v, int n) { for (int i = 0; i < n; i++) add(o, "%s%u", i ? "," : "", v[i]); }

static void print_frame(int tu, int k, const Vp9dFrame *f) {
	if (f->show_existing) {
		LINE("show tu=%d k=%d slot=%d dpb=%d size=%dx%d missing=%d", tu, k, f->show_slot, f->dpb, f->width, f->height, f->missing);
		return;
	}
	if (f->error && !f->missing) { LINE("frame tu=%d k=%d error", tu, k); return; }
	const StdVideoDecodeVP9PictureInfo *p = &f->std;
	const StdVideoDecodeVP9PictureInfoFlags *fl = &p->flags;
	LINE("frame tu=%d k=%d type=%d show=%u intra=%u er=%u prof=%d size=%dx%d bd=%d refresh=%02x dpb=%d hb=%d hdr=%u/%u at=%u:%u "
	     "ctx=%u reset=%u rctx=%u par=%u hp=%u prev=%u sb=%x interp=%d q=%u/%d/%d/%d tiles=%u/%u color=%u/%u/%d missing=%d",
	     tu, k, p->frame_type, fl->show_frame, fl->intra_only, fl->error_resilient_mode, p->profile, f->width, f->height,
	     f->bit_depth, f->refresh, f->dpb, f->hdr_bits, f->uncomp_size, f->comp_size, f->offset, f->size, p->frame_context_idx,
	     p->reset_frame_context, fl->refresh_frame_context, fl->frame_parallel_decoding_mode, fl->allow_high_precision_mv,
	     fl->UsePrevFrameMvs, p->ref_frame_sign_bias_mask, p->interpolation_filter, p->base_q_idx, p->delta_q_y_dc, p->delta_q_uv_dc,
	     p->delta_q_uv_ac, p->tile_cols_log2, p->tile_rows_log2, f->color.flags.color_range, f->color.BitDepth, f->color.color_space,
	     f->missing);
	{
		char b[512]; Str o = { b, sizeof b, 0 };
		add(&o, " refs idx=%d,%d,%d names=%d,%d,%d list=", f->ref_frame_idx[0], f->ref_frame_idx[1], f->ref_frame_idx[2],
		    f->name_dpb[0], f->name_dpb[1], f->name_dpb[2]);
		for (int i = 0; i < f->nrefs; i++) add(&o, "%s%d@%d:%dx%d", i ? "," : "", f->refs[i].dpb, f->refs[i].slot, f->refs[i].width, f->refs[i].height);
		if (!f->nrefs) add(&o, "-");
		emit(b);
	}
	const StdVideoVP9LoopFilter *l = &f->lf;
	LINE(" lf level=%u sharp=%u en=%u upd=%u uref=%x umode=%x ref=%d,%d,%d,%d mode=%d,%d", l->loop_filter_level, l->loop_filter_sharpness,
	     l->flags.loop_filter_delta_enabled, l->flags.loop_filter_delta_update, l->update_ref_delta, l->update_mode_delta,
	     l->loop_filter_ref_deltas[0], l->loop_filter_ref_deltas[1], l->loop_filter_ref_deltas[2], l->loop_filter_ref_deltas[3],
	     l->loop_filter_mode_deltas[0], l->loop_filter_mode_deltas[1]);
	if (fl->segmentation_enabled) {
		const StdVideoVP9Segmentation *s = &f->seg;
		char b[1024]; Str o = { b, sizeof b, 0 };
		add(&o, " seg map=%u temp=%u data=%u abs=%u tree=", s->flags.segmentation_update_map, s->flags.segmentation_temporal_update,
		    s->flags.segmentation_update_data, s->flags.segmentation_abs_or_delta_update);
		bytes(&o, s->segmentation_tree_probs, 7); add(&o, " pred="); bytes(&o, s->segmentation_pred_prob, 3); add(&o, " feat=");
		for (int i = 0; i < 8; i++) add(&o, "%s%x:%d/%d/%d/%d", i ? "," : "", s->FeatureEnabled[i], s->FeatureData[i][0], s->FeatureData[i][1],
		                                s->FeatureData[i][2], s->FeatureData[i][3]);
		emit(b);
		if (f->lavc_differs) {
			o.o = 0;
			add(&o, " lavc abs=%d tree=", f->lavc_abs); bytes(&o, f->lavc_tree, 7); add(&o, " pred="); bytes(&o, f->lavc_pred, 3);
			emit(b);
		}
	}
}

static uint8_t *slurp(const char *path, size_t *n) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END); const long sz = ftell(fp); fseek(fp, 0, SEEK_SET);
	uint8_t *d = malloc(sz > 0 ? (size_t)sz : 1);
	*n = fread(d, 1, (size_t)(sz > 0 ? sz : 0), fp);
	fclose(fp);
	return d;
}

typedef struct { uint8_t *p; size_t n; } Sample;
static int samples_of(const uint8_t *d, size_t n, Sample **out) {
	int cnt = 0, cap = 0;
	*out = NULL;
	if (n < 32 || memcmp(d, "DKIF", 4)) return 0;
	size_t pos = d[6] | d[7] << 8;
	while (pos + 12 <= n) {
		const size_t sz = (size_t)d[pos] | (size_t)d[pos + 1] << 8 | (size_t)d[pos + 2] << 16 | (size_t)d[pos + 3] << 24;
		pos += 12;
		if (sz > n - pos) break;
		if (cnt == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, (size_t)cap * sizeof **out); }
		(*out)[cnt].p = malloc(sz ? sz : 1); memcpy((*out)[cnt].p, d + pos, sz); (*out)[cnt].n = sz; cnt++;
		pos += sz;
	}
	return cnt;
}

static int check_invariants(const Vp9dSample *out, size_t n) {
	if (out->nframes < 1 || out->nframes > VP9D_MAX_FRAMES || out->shown >= out->nframes) return 1;
	for (int k = 0; k < out->nframes; k++) {
		const Vp9dFrame *f = &out->f[k];
		if (f->offset > n || f->size > n - f->offset) return 2;
		if (f->show_existing) { if (f->dpb >= VP9D_SLOTS || f->show_slot > 7) return 3; continue; }
		if (f->error) continue;
		if (f->dpb < 0 || f->dpb >= VP9D_SLOTS || f->nrefs > 3) return 4;
		if (f->uncomp_size == 0 || f->comp_size == 0 || (uint64_t)f->uncomp_size + f->comp_size > f->size) return 5;
		for (int i = 0; i < f->nrefs; i++) if (f->refs[i].dpb < 0 || f->refs[i].dpb >= VP9D_SLOTS || f->refs[i].dpb == f->dpb) return 6;
		for (int i = 0; i < 3; i++) if (f->name_dpb[i] < -1 || f->name_dpb[i] >= VP9D_SLOTS) return 7;
		const int intra = f->std.frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY || f->std.flags.intra_only;
		if (!intra) for (int i = 0; i < 3; i++) {   // every name of an inter frame is among its references
			int found = 0;
			for (int j = 0; j < f->nrefs; j++) if (f->refs[j].dpb == f->name_dpb[i]) found = 1;
			if (!found) return 8;
		}
		if (f->width < 1 || f->height < 1 || f->width > 65536 || f->height > 65536) return 9;
		if (f->std.pColorConfig != &f->color || f->std.pLoopFilter != &f->lf || f->std.pSegmentation != &f->seg) return 10;
	}
	return 0;
}

static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

static int fuzz(Sample *sm, int cnt, uint64_t seed, int iters) {
	rng = seed * 0x9E3779B97F4A7C15ull + 1;
	static Vp9dSample out;
	long pictures = 0, errors = 0, unsupported = 0;
	for (int it = 0; it < iters; it++) {
		static Vp9dState s; vp9d_init(&s, it & 1 ? -1 : 0);
		for (int i = 0; i < cnt; i++) {
			size_t n = sm[i].n;
			uint8_t *d = malloc(n + 64);
			memcpy(d, sm[i].p, n);
			const int what = (int)(rnd() % 8);
			if (what < 4 && n) { for (int k = 0, m = 1 + (int)(rnd() % 8); k < m; k++) d[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); }
			else if (what == 4 && n) n = rnd() % n;   // cut
			else if (what == 5 && n > 4) { const size_t at = rnd() % n; d[at] = (uint8_t)rnd(); }
			else if (what == 6 && n > 1) d[n - 1] = (uint8_t)(0xc0 | (rnd() & 0x1f));   // a superframe marker
			uint8_t *e = malloc(n ? n : 1);   // exactly n bytes: ASan sees any read past the sample
			memcpy(e, d, n);
			free(d);
			const int r = vp9d_parse_sample(&s, e, n, &out);
			if (r == VP9D_PICTURE) { pictures++; const int c = check_invariants(&out, n); if (c) { printf("fuzz: iteration %d sample %d: invariant %d broken\n", it, i, c); free(e); return 1; } }
			else if (r == VP9D_ERROR) errors++;
			else if (r == VP9D_UNSUPPORTED) unsupported++;
			for (int k = 0; k < out.nframes && r == VP9D_PICTURE; k++)   // as vtdirect.c: a frame it could not decode
				if (!out.f[k].show_existing && !out.f[k].error && (rnd() % 16) == 0) vp9d_lost(&s, out.f[k].gen);
			if ((rnd() % 64) == 0) vp9d_flush(&s);
			free(e);
		}
	}
	printf("fuzz: %d iterations of %d samples: %ld with pictures, %ld damaged, %ld unsupported, no invariant broken\n", iters, cnt, pictures, errors, unsupported);
	return 0;
}

int main(int argc, char **argv) {
	if (argc >= 5 && !strcmp(argv[1], "--fuzz")) {
		size_t n; uint8_t *d = slurp(argv[4], &n);
		if (!d) { perror(argv[4]); return 2; }
		Sample *sm; const int cnt = samples_of(d, n, &sm);
		return fuzz(sm, cnt, strtoull(argv[2], NULL, 10), atoi(argv[3]));
	}
	if (argc < 2) { fprintf(stderr, "usage: vp9parse_test <stream.ivf> [golden.txt] | --fuzz <seed> <iterations> <stream.ivf>\n"); return 2; }
	size_t n; uint8_t *d = slurp(argv[1], &n);
	if (!d) { perror(argv[1]); return 2; }
	if (argc > 2 && !(golden = fopen(argv[2], "r"))) { perror(argv[2]); return 2; }
	Sample *sm; const int cnt = samples_of(d, n, &sm);
	if (!cnt) { fprintf(stderr, "%s: not an IVF file with frames\n", argv[1]); return 2; }
	static Vp9dState s; vp9d_init(&s, -1);
	s.check = 1;
	static Vp9dSample out;
	for (int i = 0; i < cnt; i++) {
		const int r = vp9d_parse_sample(&s, sm[i].p, sm[i].n, &out);
		if (r == VP9D_UNSUPPORTED || (r == VP9D_ERROR && !out.nframes)) {
			char l[256]; snprintf(l, sizeof l, "tu %d sample: %d %s", i, r, vp9d_why(&s));
			if (golden) { if (!bad) printf("%s\n", l); bad = 1; } else printf("%s\n", l);
			if (r == VP9D_UNSUPPORTED) break;
			continue;
		}
		for (int k = 0; k < out.nframes; k++) print_frame(i, k, &out.f[k]);
		LINE("tu %d frames=%d shown=%d extra=%d sf=%d", i, out.nframes, out.shown, out.extra_shown, out.superframe);
	}
	if (golden) {
		char g[256];
		if (!bad && fgets(g, sizeof g, golden)) { printf("line %d: output ends; golden has: %s", lineno + 1, g); bad = 1; }
		if (!bad) printf("%s: %d lines identical to %s\n", argv[1], lineno, argv[2]);
	}
	return bad;
}
