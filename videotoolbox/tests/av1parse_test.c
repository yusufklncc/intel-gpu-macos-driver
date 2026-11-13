// SPDX-License-Identifier: MIT
// av1parse_test <stream.ivf|.obu> [golden.txt]: the direct path's AV1 parser (vtdirect_av1.c) over a recorded stream,
// without a GPU. Each IVF frame (or, for a low-overhead .obu stream, each temporal unit from one temporal delimiter
// to the next) is handed over as the sample an MP4 carries. Prints, in the format of tests/av1/mkgolden.py (made from
// FFmpeg 9.0.2's trace_headers and the same rules written again in Python; the format is described there):
//   seq ...                         a sequence header as it goes into StdVideoAV1SequenceHeader / ColorConfig
//   frame / refs / q / seg / lf / cdef / lr / tile / gm / grain / setup / tiles
//                                   a decoded frame: its Std picture information, the DPB slot, the references with
//                                   their reference information, and where its tiles are in the sample
//   show ...                        a show_existing_frame
//   tu ...                          the sample: its frames and the one shown
// With a golden file it compares line by line and exits 1 at the first difference.
//   av1parse_test --fuzz <seed> <iterations> <stream>: the samples damaged at random (bytes changed, cut, repeated),
//   parsed again and again: no crash, no read outside the sample (build with -fsanitize=address,undefined), and
//   whatever the parser hands out stays in range (tile offsets in the sample, DPB slots, reference counts).
// Build (here, no FFmpeg): cc -O1 -Wall -I<mesa>/include av1parse_test.c ../vtdirect_av1.c -o av1parse_test
#include "../vtdirect_av1.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *golden; static int lineno, bad;
static void emit(const char *l) {
	lineno++;
	if (!golden) { printf("%s\n", l); return; }
	char g[65536];
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
#define LINE(...) do { char b_[8192]; Str o_ = { b_, sizeof b_, 0 }; add(&o_, __VA_ARGS__); emit(b_); } while (0)

static void print_seq(const Av1dSeq *q) {
	char b[2048]; Str o = { b, sizeof b, 0 };
	const StdVideoAV1SequenceHeader *d = &q->std;
	const StdVideoAV1ColorConfig *c = &q->color;
	const StdVideoAV1SequenceHeaderFlags *fl = &d->flags;
	add(&o, "seq profile=%d ops=%d op0=%03x level=%d tier=%d w=%d h=%d bits=%d/%d fid=%d/%d/%d flags=", d->seq_profile, q->ops, q->op_idc[0],
	    q->level[0], q->tier[0], d->max_frame_width_minus_1 + 1, d->max_frame_height_minus_1 + 1, d->frame_width_bits_minus_1 + 1,
	    d->frame_height_bits_minus_1 + 1, fl->frame_id_numbers_present_flag, d->delta_frame_id_length_minus_2,
	    d->additional_frame_id_length_minus_1);
	const unsigned v[] = { fl->still_picture, fl->reduced_still_picture_header, fl->use_128x128_superblock, fl->enable_filter_intra,
		fl->enable_intra_edge_filter, fl->enable_interintra_compound, fl->enable_masked_compound, fl->enable_warped_motion,
		fl->enable_dual_filter, fl->enable_order_hint, fl->enable_jnt_comp, fl->enable_ref_frame_mvs, fl->frame_id_numbers_present_flag,
		fl->enable_superres, fl->enable_cdef, fl->enable_restoration, fl->film_grain_params_present, fl->timing_info_present_flag,
		fl->initial_display_delay_present_flag };
	for (size_t i = 0; i < sizeof v / sizeof *v; i++) add(&o, "%u", v[i]);
	add(&o, " ohbits=%d sct=%d imv=%d bd=%d mono=%d ss=%d/%d range=%d cdp=%d cp=%d tc=%d mc=%d csp=%d sep=%d", d->order_hint_bits_minus_1 + 1,
	    d->seq_force_screen_content_tools, d->seq_force_integer_mv, c->BitDepth, c->flags.mono_chrome, c->subsampling_x, c->subsampling_y,
	    c->flags.color_range, c->flags.color_description_present_flag, c->color_primaries, c->transfer_characteristics,
	    c->matrix_coefficients, c->chroma_sample_position, c->flags.separate_uv_delta_q);
	if (fl->timing_info_present_flag)
		add(&o, " timing=%u/%u/%u/%u", q->timing.num_units_in_display_tick, q->timing.time_scale, q->timing.flags.equal_picture_interval,
		    q->timing.num_ticks_per_picture_minus_1);
	add(&o, " dm=%d", q->decoder_model_info_present);
	emit(b);
}

static void ref_info(Str *o, const StdVideoDecodeAV1ReferenceInfo *r, int saved) {
	add(o, "%u:%u:%02x:%u:%u", r->frame_type, r->OrderHint, r->RefFrameSignBias, r->flags.disable_frame_end_update_cdf, r->flags.segmentation_enabled);
	if (saved) { add(o, ":"); for (int i = 0; i < 8; i++) add(o, "%s%u", i ? "_" : "", r->SavedOrderHints[i]); }
}

static void print_frame(int tu, const Av1dFrame *fr) {
	char b[16384]; Str o = { b, sizeof b, 0 };
	if (fr->show_existing) {
		LINE("show tu=%d slot=%d dpb=%d refresh=%02x size=%dx%d grain=%d missing=%d", tu, fr->show_slot, fr->dpb, fr->refresh, fr->width,
		     fr->height, fr->apply_grain, fr->missing);
		return;
	}
	const StdVideoDecodeAV1PictureInfo *p = &fr->std;
	const StdVideoDecodeAV1PictureInfoFlags *fl = &p->flags;
	add(&o, "frame tu=%d type=%d show=%d oh=%u prf=%u refresh=%02x fid=%u size=%dx%d fw=%d denom=%u dpb=%d hb=%d flags=", tu, p->frame_type,
	    fr->shown, p->OrderHint, p->primary_ref_frame, p->refresh_frame_flags, p->current_frame_id, fr->width, fr->height, fr->frame_width,
	    p->coded_denom, fr->dpb, fr->hdr_bits);
	const unsigned v[] = { fl->error_resilient_mode, fl->disable_cdf_update, fl->use_superres, fl->render_and_frame_size_different,
		fl->allow_screen_content_tools, fl->is_filter_switchable, fl->force_integer_mv, fl->frame_size_override_flag,
		fl->buffer_removal_time_present_flag, fl->allow_intrabc, fl->frame_refs_short_signaling, fl->allow_high_precision_mv,
		fl->is_motion_mode_switchable, fl->use_ref_frame_mvs, fl->disable_frame_end_update_cdf, fl->allow_warped_motion, fl->reduced_tx_set,
		fl->reference_select, fl->skip_mode_present, fl->delta_q_present, fl->delta_lf_present, fl->delta_lf_multi, fl->segmentation_enabled,
		fl->segmentation_update_map, fl->segmentation_temporal_update, fl->segmentation_update_data, fl->UsesLr, fl->usesChromaLr,
		fl->apply_grain };
	for (size_t i = 0; i < sizeof v / sizeof *v; i++) add(&o, "%u", v[i]);
	emit(b);
	o.o = 0;
	add(&o, " refs idx=");
	for (int i = 0; i < 7; i++) add(&o, "%s%d", i ? "," : "", fr->ref_frame_idx[i]);
	add(&o, " names=");
	for (int i = 0; i < 7; i++) add(&o, "%s%d", i ? "," : "", fr->name_dpb[i]);
	add(&o, " ohs=");
	for (int i = 0; i < 8; i++) add(&o, "%s%u", i ? "," : "", p->OrderHints[i]);
	add(&o, " skip=%u,%u list=", p->SkipModeFrame[0], p->SkipModeFrame[1]);
	for (int i = 0; i < fr->nrefs; i++) { add(&o, "%s%d@%d=", i ? " " : "", fr->refs[i].dpb, fr->refs[i].slot); ref_info(&o, &fr->refs[i].info, 1); }
	if (!fr->nrefs) add(&o, "-");
	add(&o, " missing=%d", fr->missing);
	emit(b);
	const StdVideoAV1Quantization *q = p->pQuantization;
	LINE(" q base=%u dq=%d,%d,%d,%d,%d diffuv=%u qm=%u:%u,%u,%u dqres=%u dlfres=%u interp=%d tx=%d", q->base_q_idx, q->DeltaQYDc, q->DeltaQUDc,
	     q->DeltaQUAc, q->DeltaQVDc, q->DeltaQVAc, q->flags.diff_uv_delta, q->flags.using_qmatrix, q->qm_y, q->qm_u, q->qm_v, p->delta_q_res,
	     p->delta_lf_res, p->interpolation_filter, p->TxMode);
	o.o = 0;
	add(&o, " seg");
	for (int i = 0; i < 8; i++) {
		add(&o, " %02x:", p->pSegmentation->FeatureEnabled[i]);
		for (int j = 0; j < 8; j++) add(&o, "%s%d", j ? "," : "", p->pSegmentation->FeatureData[i][j]);
	}
	emit(b);
	const StdVideoAV1LoopFilter *lf = p->pLoopFilter;
	LINE(" lf lvl=%u,%u,%u,%u sharp=%u de=%u du=%u ref=%d,%d,%d,%d,%d,%d,%d,%d mode=%d,%d", lf->loop_filter_level[0], lf->loop_filter_level[1],
	     lf->loop_filter_level[2], lf->loop_filter_level[3], lf->loop_filter_sharpness, lf->flags.loop_filter_delta_enabled,
	     lf->flags.loop_filter_delta_update, lf->loop_filter_ref_deltas[0], lf->loop_filter_ref_deltas[1], lf->loop_filter_ref_deltas[2],
	     lf->loop_filter_ref_deltas[3], lf->loop_filter_ref_deltas[4], lf->loop_filter_ref_deltas[5], lf->loop_filter_ref_deltas[6],
	     lf->loop_filter_ref_deltas[7], lf->loop_filter_mode_deltas[0], lf->loop_filter_mode_deltas[1]);
	const StdVideoAV1CDEF *cd = p->pCDEF;
	o.o = 0;
	add(&o, " cdef damp=%u bits=%u y=", cd->cdef_damping_minus_3, cd->cdef_bits);
	for (int i = 0; i < 8; i++) add(&o, "%s%u/%u", i ? "," : "", cd->cdef_y_pri_strength[i], cd->cdef_y_sec_strength[i]);
	add(&o, " uv=");
	for (int i = 0; i < 8; i++) add(&o, "%s%u/%u", i ? "," : "", cd->cdef_uv_pri_strength[i], cd->cdef_uv_sec_strength[i]);
	emit(b);
	const StdVideoAV1LoopRestoration *lr = p->pLoopRestoration;
	LINE(" lr type=%d,%d,%d size=%u,%u,%u", lr->FrameRestorationType[0], lr->FrameRestorationType[1], lr->FrameRestorationType[2],
	     lr->LoopRestorationSize[0], lr->LoopRestorationSize[1], lr->LoopRestorationSize[2]);
	const StdVideoAV1TileInfo *t = p->pTileInfo;
	o.o = 0;
	add(&o, " tile cols=%u rows=%u uni=%u ctx=%u tsb=%u mc=", t->TileCols, t->TileRows, t->flags.uniform_tile_spacing_flag,
	    t->context_update_tile_id, t->tile_size_bytes_minus_1);
	for (int i = 0; i < t->TileCols; i++) add(&o, "%s%u", i ? "," : "", t->pMiColStarts[i]);
	add(&o, " mr=");
	for (int i = 0; i < t->TileRows; i++) add(&o, "%s%u", i ? "," : "", t->pMiRowStarts[i]);
	add(&o, " w=");
	for (int i = 0; i < t->TileCols; i++) add(&o, "%s%u", i ? "," : "", t->pWidthInSbsMinus1[i]);
	add(&o, " h=");
	for (int i = 0; i < t->TileRows; i++) add(&o, "%s%u", i ? "," : "", t->pHeightInSbsMinus1[i]);
	emit(b);
	o.o = 0;
	add(&o, " gm");
	int any = 0;
	for (int r = 1; r < 8; r++) {
		if (!p->pGlobalMotion->GmType[r]) continue;
		any = 1;
		add(&o, " %d:%u:", r, p->pGlobalMotion->GmType[r]);
		for (int i = 0; i < 6; i++) add(&o, "%s%d", i ? "," : "", p->pGlobalMotion->gm_params[r][i]);
	}
	if (!any) add(&o, " -");
	emit(b);
	if (p->pFilmGrain) {
		const StdVideoAV1FilmGrain *g = p->pFilmGrain;
		o.o = 0;
		add(&o, " grain seed=%u ref=%u y=", g->grain_seed, g->film_grain_params_ref_idx);
		for (int i = 0; i < g->num_y_points; i++) add(&o, "%s%u:%u", i ? "," : "", g->point_y_value[i], g->point_y_scaling[i]);
		add(&o, " cfl=%u cb=", g->flags.chroma_scaling_from_luma);
		for (int i = 0; i < g->num_cb_points; i++) add(&o, "%s%u:%u", i ? "," : "", g->point_cb_value[i], g->point_cb_scaling[i]);
		add(&o, " cr=");
		for (int i = 0; i < g->num_cr_points; i++) add(&o, "%s%u:%u", i ? "," : "", g->point_cr_value[i], g->point_cr_scaling[i]);
		add(&o, " scal=%u lag=%u ary=", g->grain_scaling_minus_8, g->ar_coeff_lag);
		for (int i = 0; i < 24; i++) add(&o, "%s%d", i ? "," : "", g->ar_coeffs_y_plus_128[i]);
		add(&o, " arcb=");
		for (int i = 0; i < 25; i++) add(&o, "%s%d", i ? "," : "", g->ar_coeffs_cb_plus_128[i]);
		add(&o, " arcr=");
		for (int i = 0; i < 25; i++) add(&o, "%s%d", i ? "," : "", g->ar_coeffs_cr_plus_128[i]);
		add(&o, " shift=%u gss=%u cb=%u/%u/%u cr=%u/%u/%u ov=%u clip=%u", g->ar_coeff_shift_minus_6, g->grain_scale_shift, g->cb_mult,
		    g->cb_luma_mult, g->cb_offset, g->cr_mult, g->cr_luma_mult, g->cr_offset, g->flags.overlap_flag, g->flags.clip_to_restricted_range);
		emit(b);
	}
	o.o = 0;
	add(&o, " setup ");
	ref_info(&o, &fr->setup, 0);
	add(&o, " tiles n=%u tg=%d size=%zu at=", fr->ntiles, fr->ntg, fr->size);
	for (uint32_t i = 0; i < fr->ntiles; i++) add(&o, "%s%u:%u", i ? "," : "", fr->tile_src[i], fr->tile_size[i]);
	emit(b);
}

// ---------------------------------------------------------------------------------------------------------------------

typedef struct { uint8_t *p; size_t n; } Sample;

static uint8_t *slurp(const char *path, size_t *n) {
	FILE *fp = fopen(path, "rb");
	if (!fp) return NULL;
	fseek(fp, 0, SEEK_END); long l = ftell(fp); fseek(fp, 0, SEEK_SET);
	uint8_t *d = malloc(l > 0 ? (size_t)l : 1);
	*n = d ? fread(d, 1, (size_t)(l > 0 ? l : 0), fp) : 0;
	fclose(fp);
	return d;
}

// the samples of an IVF file (its frames) or of a low-overhead OBU stream (cut at temporal delimiters)
static int samples_of(const uint8_t *d, size_t n, Sample **out) {
	int cnt = 0, cap = 0;
	*out = NULL;
	if (n >= 32 && !memcmp(d, "DKIF", 4)) {
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
	size_t pos = 0, start = 0;
	while (pos < n) {
		const size_t at = pos;
		const int type = (d[pos] >> 3) & 15, ext = (d[pos] >> 2) & 1, has_size = (d[pos] >> 1) & 1;
		size_t h = 1 + (size_t)ext;
		uint64_t sz = 0;
		if (!has_size) break;
		for (int i = 0; i < 8; i++) { if (pos + h >= n) return cnt; sz |= (uint64_t)(d[pos + h] & 0x7f) << (7 * i); if (!(d[pos + h++] & 0x80)) break; }
		if (sz > n - pos - h) break;
		pos += h + (size_t)sz;
		if (type == 2 && at > start) {   // a temporal delimiter starts the next unit
			if (cnt == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, (size_t)cap * sizeof **out); }
			(*out)[cnt].p = malloc(at - start); memcpy((*out)[cnt].p, d + start, at - start); (*out)[cnt].n = at - start; cnt++;
			start = at;
		}
	}
	if (pos > start) {
		if (cnt == cap) { cap = cap ? cap * 2 : 64; *out = realloc(*out, (size_t)cap * sizeof **out); }
		(*out)[cnt].p = malloc(pos - start); memcpy((*out)[cnt].p, d + start, pos - start); (*out)[cnt].n = pos - start; cnt++;
	}
	return cnt;
}

static int check_invariants(const Av1dSample *out, size_t n) {
	if (out->nframes < 0 || out->nframes > AV1D_MAX_FRAMES || out->shown >= out->nframes) return 1;
	for (int k = 0; k < out->nframes; k++) {
		const Av1dFrame *fr = &out->f[k];
		if (fr->show_existing) { if (fr->dpb >= AV1D_SLOTS || fr->show_slot > 7) return 2; continue; }
		if (fr->dpb < 0 || fr->dpb >= AV1D_SLOTS || fr->nrefs > 7 || fr->ntiles > AV1D_MAX_TILES) return 3;
		size_t total = 0;
		for (uint32_t i = 0; i < fr->ntiles; i++) {
			if (fr->tile_src[i] > n || fr->tile_size[i] > n - fr->tile_src[i]) return 4;
			total += fr->tile_size[i];
		}
		if (total != fr->size) return 5;
		for (int i = 0; i < fr->nrefs; i++) if (fr->refs[i].dpb < 0 || fr->refs[i].dpb >= AV1D_SLOTS || fr->refs[i].dpb == fr->dpb) return 6;
		for (int i = 0; i < 7; i++) if (fr->name_dpb[i] < -1 || fr->name_dpb[i] >= AV1D_SLOTS) return 7;
		if (fr->std.pTileInfo->TileCols * fr->std.pTileInfo->TileRows > (int)AV1D_MAX_TILES) return 8;
	}
	return 0;
}

static uint64_t rng;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)(rng >> 11); }

static int fuzz(Sample *sm, int cnt, uint64_t seed, int iters) {
	rng = seed * 0x9E3779B97F4A7C15ull + 1;
	static Av1dSample out;
	long pictures = 0, errors = 0, unsupported = 0;
	for (int it = 0; it < iters; it++) {
		Av1dState s; av1d_init(&s);
		for (int i = 0; i < cnt; i++) {
			size_t n = sm[i].n;
			uint8_t *d = malloc(n + 64);
			memcpy(d, sm[i].p, n);
			const int what = (int)(rnd() % 8);
			if (what < 4 && n) { for (int k = 0, m = 1 + (int)(rnd() % 8); k < m; k++) d[rnd() % n] ^= (uint8_t)(1u << (rnd() % 8)); }
			else if (what == 4 && n) n = rnd() % n;   // cut
			else if (what == 5 && n > 4) { const size_t at = rnd() % n; d[at] = (uint8_t)rnd(); }
			uint8_t *e = malloc(n ? n : 1);   // exactly n bytes: ASan sees any read past the sample
			memcpy(e, d, n);
			free(d);
			const int r = av1d_parse_sample(&s, e, n, &out);
			if (r == AV1D_PICTURE) { pictures++; const int c = check_invariants(&out, n); if (c) { printf("fuzz: iteration %d sample %d: invariant %d broken\n", it, i, c); free(e); return 1; } }
			else if (r == AV1D_ERROR) errors++;
			else if (r == AV1D_UNSUPPORTED) unsupported++;
			for (int k = 0; k < out.nframes && r == AV1D_PICTURE; k++)   // as vtdirect.c: a frame it could not decode
				if (!out.f[k].show_existing && (rnd() % 16) == 0) av1d_lost(&s, out.f[k].gen);
			if ((rnd() % 64) == 0) av1d_flush(&s);
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
	if (argc < 2) { fprintf(stderr, "usage: av1parse_test <stream.ivf|.obu> [golden.txt] | --fuzz <seed> <iterations> <stream>\n"); return 2; }
	size_t n; uint8_t *d = slurp(argv[1], &n);
	if (!d) { perror(argv[1]); return 2; }
	if (argc > 2 && !(golden = fopen(argv[2], "r"))) { perror(argv[2]); return 2; }
	Sample *sm; const int cnt = samples_of(d, n, &sm);
	static Av1dState s; av1d_init(&s);
	s.check = 1;
	static Av1dSample out;
	uint8_t seen[256]; size_t seenn = 0;
	for (int i = 0; i < cnt; i++) {
		const int r = av1d_parse_sample(&s, sm[i].p, sm[i].n, &out);
		if (s.have_seq && (s.seq.rawn != seenn || memcmp(s.seq.raw, seen, seenn))) {
			print_seq(&s.seq);
			seenn = s.seq.rawn; memcpy(seen, s.seq.raw, seenn);
		}
		if (r < 0) {
			char l[256]; snprintf(l, sizeof l, "tu %d sample: %d %s", i, r, av1d_why(&s));
			if (golden) { if (!bad) printf("%s\n", l); bad = 1; } else printf("%s\n", l);
			if (r == AV1D_UNSUPPORTED) break;
			continue;
		}
		for (int k = 0; k < out.nframes; k++) print_frame(i, &out.f[k]);
		LINE("tu %d frames=%d shown=%d extra=%d", i, out.nframes, out.shown, out.extra_shown);
	}
	if (golden) {
		char g[256];
		if (!bad && fgets(g, sizeof g, golden)) { printf("line %d: output ends; golden has: %s", lineno + 1, g); bad = 1; }
		if (!bad) printf("%s: %d lines identical to %s\n", argv[1], lineno, argv[2]);
	}
	return bad;
}
