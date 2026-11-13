// SPDX-License-Identifier: MIT
// hevcparse_test <stream.265> [golden.txt]: the direct path's HEVC parser (vtdirect_hevc.c) over a recorded Annex B
// stream, without a GPU. The stream is cut into access units and handed over as the length-prefixed samples an MP4
// carries (parameter sets in-band). Prints, in the format of tests/hevc/mkgolden.py (made from FFmpeg 9.0.2: its
// trace_headers bitstream filter and libavcodec's own HEVC decoder; the format is described there):
//   vps / sps / sps-st / sps-lt / sps-scaling / pps / pps-scaling   the parameter sets as they go into the Std structures
//   pic <type> poc=.. ... bef=.. aft=.. lt=.. dpb=..                each picture's POC, RPS, NumBitsForSTRefPicSetInSlice,
//                                                                   slice header length and the references it is decoded
//                                                                   with (what the Vulkan Video decode gets)
//   skip <type> poc=..                                              a RASL picture that is not decoded
// With a golden file it compares line by line and exits 1 at the first difference.
// Build (here, no FFmpeg): cc -O1 -Wall -I<mesa>/include hevcparse_test.c ../vtdirect_hevc.c -o hevcparse_test
#include "../vtdirect_hevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t n; } Nal;

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
#include <stdarg.h>
static void add(Str *s, const char *fmt, ...) {
	va_list ap; va_start(ap, fmt);
	if (s->o < s->n) s->o += (size_t)vsnprintf(s->b + s->o, s->n - s->o, fmt, ap);
	va_end(ap);
	if (s->o >= s->n) s->o = s->n - 1;
}

static void lists(Str *o, const StdVideoH265ScalingLists *sl) {
	for (int size = 0; size < 4; size++)
		for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
			const uint8_t *l = size == 0 ? sl->ScalingList4x4[m] : size == 1 ? sl->ScalingList8x8[m] : size == 2 ? sl->ScalingList16x16[m] : sl->ScalingList32x32[m / 3];
			add(o, " %d.%d=", size, m);
			if (size == 2) add(o, "%u:", sl->ScalingListDCCoef16x16[m]);
			if (size == 3) add(o, "%u:", sl->ScalingListDCCoef32x32[m / 3]);
			for (int i = 0; i < (size ? 64 : 16); i++) add(o, "%s%u", i ? "," : "", l[i]);
		}
}

static void print_vps(const HevcDVps *v) {
	char b[512]; Str o = { b, sizeof b, 0 };
	const StdVideoH265VideoParameterSet *d = &v->std;
	const int k = d->vps_max_sub_layers_minus1;
	add(&o, "vps id=%d sub_layers=%d nesting=%d profile=%d tier=%d level=%d dpb=%u/%u/%u", d->vps_video_parameter_set_id, k + 1,
	    d->flags.vps_temporal_id_nesting_flag, v->profile, v->ptl.flags.general_tier_flag, v->level_idc,
	    v->dpbm.max_dec_pic_buffering_minus1[k], v->dpbm.max_num_reorder_pics[k], v->dpbm.max_latency_increase_plus1[k]);
	emit(b);
}

static void print_sps(const HevcDSps *p) {
	static char b[16384]; Str o = { b, sizeof b, 0 };
	const StdVideoH265SequenceParameterSet *d = &p->std;
	const int k = d->sps_max_sub_layers_minus1, id = d->sps_seq_parameter_set_id;
	add(&o, "sps id=%d vps=%d sub_layers=%d profile=%d tier=%d level=%d chroma=%d w=%u h=%u conf=%u,%u,%u,%u bd=%d/%d lpoc=%d dpb=%u/%u/%u",
	    id, d->sps_video_parameter_set_id, k + 1, p->profile, p->ptl.flags.general_tier_flag, p->level_idc, d->chroma_format_idc,
	    d->pic_width_in_luma_samples, d->pic_height_in_luma_samples, p->conf[0], p->conf[1], p->conf[2], p->conf[3],
	    d->bit_depth_luma_minus8 + 8, d->bit_depth_chroma_minus8 + 8, d->log2_max_pic_order_cnt_lsb_minus4 + 4,
	    p->dpbm.max_dec_pic_buffering_minus1[k], p->dpbm.max_num_reorder_pics[k], p->dpbm.max_latency_increase_plus1[k]);
	add(&o, " cb=%d/%d tb=%d/%d thd=%d/%d scaling=%d/%d amp=%d sao=%d", d->log2_min_luma_coding_block_size_minus3 + 3,
	    d->log2_diff_max_min_luma_coding_block_size, d->log2_min_luma_transform_block_size_minus2 + 2,
	    d->log2_diff_max_min_luma_transform_block_size, d->max_transform_hierarchy_depth_inter, d->max_transform_hierarchy_depth_intra,
	    d->flags.scaling_list_enabled_flag, p->scaling_coded, d->flags.amp_enabled_flag, d->flags.sample_adaptive_offset_enabled_flag);
	if (p->pcm_coded) add(&o, " pcm=1:%d/%d/%d/%d/%d", d->pcm_sample_bit_depth_luma_minus1 + 1, d->pcm_sample_bit_depth_chroma_minus1 + 1,
	                      d->log2_min_pcm_luma_coding_block_size_minus3 + 3, d->log2_diff_max_min_pcm_luma_coding_block_size,
	                      d->flags.pcm_loop_filter_disabled_flag);
	else add(&o, " pcm=0");
	add(&o, " strps=%d lt=%d/%d tmvp=%d sis=%d vui=%d range=%d rext=%d", d->num_short_term_ref_pic_sets, d->flags.long_term_ref_pics_present_flag,
	    d->num_long_term_ref_pics_sps, d->flags.sps_temporal_mvp_enabled_flag, d->flags.strong_intra_smoothing_enabled_flag,
	    d->flags.vui_parameters_present_flag, p->full_range, d->flags.sps_range_extension_flag);
	emit(b);
	for (int i = 0; i < d->num_short_term_ref_pic_sets; i++) {
		const HevcDRps *r = &p->rps[i];
		o.o = 0;
		add(&o, "sps-st id=%d i=%d inter=%d d=", id, i, p->st[i].flags.inter_ref_pic_set_prediction_flag);
		for (int j = 0; j < r->num_negative + r->num_positive; j++) add(&o, "%s%d%s", j ? "," : "", r->delta[j], r->used >> j & 1 ? "*" : "");
		if (!(r->num_negative + r->num_positive)) add(&o, "-");
		emit(b);
	}
	if (d->num_long_term_ref_pics_sps) {
		o.o = 0;
		add(&o, "sps-lt id=%d ", id);
		for (int i = 0; i < d->num_long_term_ref_pics_sps; i++)
			add(&o, "%s%u%s", i ? "," : "", p->lt.lt_ref_pic_poc_lsb_sps[i], p->lt.used_by_curr_pic_lt_sps_flag >> i & 1 ? "*" : "");
		emit(b);
	}
	if (p->scaling_coded) { o.o = 0; add(&o, "sps-scaling id=%d", id); lists(&o, &p->scaling); emit(b); }
}

static void print_pps(const HevcDPps *p) {
	static char b[16384]; Str o = { b, sizeof b, 0 };
	const StdVideoH265PictureParameterSet *d = &p->std;
	add(&o, "pps id=%d sps=%d dep=%d ofp=%d xbits=%d sdh=%d cabac_init=%d l0=%d l1=%d qp=%d cip=%d tskip=%d cuqpd=%d/%d cb=%d cr=%d sqp=%d",
	    d->pps_pic_parameter_set_id, p->sps_id, d->flags.dependent_slice_segments_enabled_flag, d->flags.output_flag_present_flag,
	    d->num_extra_slice_header_bits, d->flags.sign_data_hiding_enabled_flag, d->flags.cabac_init_present_flag,
	    d->num_ref_idx_l0_default_active_minus1, d->num_ref_idx_l1_default_active_minus1, d->init_qp_minus26,
	    d->flags.constrained_intra_pred_flag, d->flags.transform_skip_enabled_flag, d->flags.cu_qp_delta_enabled_flag,
	    d->diff_cu_qp_delta_depth, d->pps_cb_qp_offset, d->pps_cr_qp_offset, d->flags.pps_slice_chroma_qp_offsets_present_flag);
	add(&o, " wp=%d wb=%d tqb=%d tiles=%d wpp=%d cols=", d->flags.weighted_pred_flag, d->flags.weighted_bipred_flag,
	    d->flags.transquant_bypass_enabled_flag, d->flags.tiles_enabled_flag, d->flags.entropy_coding_sync_enabled_flag);
	for (int i = 0; i < p->ncols; i++) add(&o, "%s%u", i ? "," : "", p->col[i]);
	add(&o, " rows=");
	for (int i = 0; i < p->nrows; i++) add(&o, "%s%u", i ? "," : "", p->row[i]);
	add(&o, " lfat=%d lfas=%d dbk=%d/%d/%d/%d/%d lmod=%d pml=%d she=%d scaling=%d rext=%d", d->flags.loop_filter_across_tiles_enabled_flag,
	    d->flags.pps_loop_filter_across_slices_enabled_flag, d->flags.deblocking_filter_control_present_flag,
	    d->flags.deblocking_filter_override_enabled_flag, d->flags.pps_deblocking_filter_disabled_flag, d->pps_beta_offset_div2,
	    d->pps_tc_offset_div2, d->flags.lists_modification_present_flag, d->log2_parallel_merge_level_minus2,
	    d->flags.slice_segment_header_extension_present_flag, d->flags.pps_scaling_list_data_present_flag, d->flags.pps_range_extension_flag);
	emit(b);
	if (d->flags.pps_scaling_list_data_present_flag) { o.o = 0; add(&o, "pps-scaling id=%d", d->pps_pic_parameter_set_id); lists(&o, &p->scaling); emit(b); }
}

static void print_pic(const HevcDPicture *pic) {
	char b[2048]; Str o = { b, sizeof b, 0 };
	add(&o, "pic %s poc=%d tid=%d slices=%u st=", hevcd_nal_type_name(pic->nal_type), pic->poc, pic->tid, pic->nslices);
	if (pic->st_mode == 0) add(&o, "-"); else if (pic->st_mode == 1) add(&o, "sps:%d", pic->st_idx); else add(&o, "sh");
	add(&o, " rpsbits=%u refdpoc=%u hdr=%d", pic->std.NumBitsForSTRefPicSetInSlice, pic->std.NumDeltaPocsOfRefRpsIdx, pic->hdr_bits);
	static const char *name[3] = { "bef", "aft", "lt" };
	for (int l = 0; l < 3; l++) {
		add(&o, " %s=", name[l]);
		for (int i = 0; i < pic->nlist[l]; i++) add(&o, "%s%s%d", i ? "," : "", pic->found[l][i] ? "" : "?", pic->list[l][i]);
		if (!pic->nlist[l]) add(&o, "-");
	}
	// the references, sorted by POC
	HevcDRef r[HEVCD_MAX_REFS]; memcpy(r, pic->refs, sizeof r);
	for (int i = 1; i < pic->nrefs; i++) for (int j = i; j > 0 && r[j].poc < r[j - 1].poc; j--) { HevcDRef t = r[j]; r[j] = r[j - 1]; r[j - 1] = t; }
	add(&o, " dpb=");
	for (int i = 0; i < pic->nrefs; i++) add(&o, "%s%d:%c", i ? "," : "", r[i].poc, r[i].long_term ? 'L' : 'S');
	if (!pic->nrefs) add(&o, "-");
	if (pic->missing) add(&o, " missing");
	emit(b);
}

// a sample: the NAL units with 4-byte lengths
static uint8_t *pack(const Nal *nals, int n, size_t *size) {
	size_t t = 0; for (int i = 0; i < n; i++) t += 4 + nals[i].n;
	uint8_t *b = malloc(t ? t : 1), *w = b;
	for (int i = 0; i < n; i++) {
		w[0] = (uint8_t)(nals[i].n >> 24); w[1] = (uint8_t)(nals[i].n >> 16); w[2] = (uint8_t)(nals[i].n >> 8); w[3] = (uint8_t)nals[i].n;
		memcpy(w + 4, nals[i].p, nals[i].n); w += 4 + nals[i].n;
	}
	*size = t;
	return b;
}

static int type_of(const Nal *n) { return n->p[0] >> 1 & 0x3f; }
static int layer_of(const Nal *n) { return (n->p[0] & 1) << 5 | n->p[1] >> 3; }

static int run_au(HevcDState *s, const Nal *au, int n) {
	for (int i = 0; i < n; i++) {   // the parameter sets one by one first, to print each as it comes (through an hvcC:
		const int t = type_of(&au[i]);   // that path does not touch the decoder's end-of-sequence state)
		if ((t != 32 && t != 33 && t != 34) || layer_of(&au[i])) continue;
		uint8_t *c = calloc(1, 23 + 5 + au[i].n);
		c[0] = 1; c[21] = 3; c[22] = 1; c[23] = (uint8_t)t; c[24] = 0; c[25] = 1;
		c[26] = (uint8_t)(au[i].n >> 8); c[27] = (uint8_t)au[i].n; memcpy(c + 28, au[i].p, au[i].n);
		const int r = hevcd_parse_hvcc(s, c, 28 + au[i].n);
		free(c);
		if (r < 0) { fprintf(stderr, "parameter set skipped (as libavcodec skips it): %s\n", hevcd_why(s)); continue; }
		if (t == 32) print_vps(s->vps[s->last_vps]);
		else if (t == 33) print_sps(s->sps[s->last_sps]);
		else print_pps(s->pps[s->last_pps]);
	}
	size_t sz; uint8_t *b = pack(au, n, &sz);
	const size_t need = hevcd_bitstream_size(s, b, sz);
	uint8_t *out = malloc(need ? need : 1);
	HevcDPicture pic;
	const int r = hevcd_parse_sample(s, b, sz, out, need, &pic);
	if (r == HEVCD_PICTURE) {
		if (pic.size != need) { printf("bitstream size %zu, expected %zu\n", pic.size, need); bad = 1; }
		for (uint32_t i = 0; i < pic.nslices; i++)
			if (pic.slice_offsets[i] + 3 > pic.size || out[pic.slice_offsets[i]] || out[pic.slice_offsets[i] + 1] || out[pic.slice_offsets[i] + 2] != 1) {
				printf("slice %u: no start code at %u\n", i, pic.slice_offsets[i]); bad = 1;
			}
		print_pic(&pic);
		hevcd_finish_picture(s, &pic);
	} else if (r == HEVCD_NO_PICTURE && s->skipped) {
		char l[128]; snprintf(l, sizeof l, "skip %s poc=%d", hevcd_nal_type_name(pic.nal_type), pic.poc); emit(l);
	} else if (r < 0) printf("sample: %d %s\n", r, hevcd_why(s));
	free(b); free(out);
	return r < 0 ? r : 0;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: hevcparse_test <stream.265> [golden.txt]\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); const long fl = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t)fl + 4);
	if (fread(d, 1, (size_t)fl, f) != (size_t)fl) { perror("read"); return 2; }
	fclose(f);
	if (argc > 2 && !(golden = fopen(argv[2], "r"))) { perror(argv[2]); return 2; }
	// NAL units of the Annex B stream (trailing zero bytes belong to the next start code)
	Nal *nals = malloc(sizeof(Nal) * ((size_t)fl / 3 + 2)); int nn = 0;
	size_t i = 0, start = 0; int in = 0;
	while (i + 3 <= (size_t)fl) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			if (in) { size_t e = i; while (e > start && d[e - 1] == 0) e--; if (e - start >= 2) nals[nn++] = (Nal){ d + start, e - start }; }
			start = i + 3; in = 1; i += 3;
		} else i++;
	}
	if (in && (size_t)fl > start + 1) nals[nn++] = (Nal){ d + start, (size_t)fl - start };
	// access units (7.4.2.4.4): a new one at a slice segment with first_slice_segment_in_pic_flag, or at a VPS / SPS /
	// PPS / AUD / prefix SEI / reserved 41..44 / unspecified 48..55 after the last slice segment of one; EOS / EOB after a
	// slice segment end it
	HevcDState *s = calloc(1, sizeof *s);
	hevcd_init(s, 4);
	s->check = 1;
	int au0 = 0, has_slice = 0, aus = 0, rc = 0, ended = 0;
	for (int k = 0; k <= nn && !rc; k++) {
		const int t = k < nn ? type_of(&nals[k]) : -1;
		const int vcl = (t >= 0 && t <= 31) && (t <= 9 || (t >= 16 && t <= 21));
		const int first = vcl && nals[k].n > 2 && (nals[k].p[2] & 0x80);
		const int starter = t == 32 || t == 33 || t == 34 || t == 35 || t == 39 || (t >= 41 && t <= 44) || (t >= 48 && t <= 55);
		const int boundary = k == nn || ended || (has_slice && ((vcl && first) || starter));
		if (boundary && k > au0) { rc = run_au(s, nals + au0, k - au0); au0 = k; has_slice = 0; aus++; ended = 0; }
		if (vcl) has_slice = 1;
		if ((t == 36 || t == 37) && has_slice) ended = 1;   // (one before any slice belongs to the next picture's unit)
	}
	if (golden && !bad) { char g[256]; if (fgets(g, sizeof g, golden)) { printf("line %d: golden has more: %s", lineno + 1, g); bad = 1; } }
	fprintf(stderr, "%s: %d access units, %d lines%s%s\n", argv[1], aus, lineno, golden ? (bad ? ", DIFFERS from " : ", identical to ") : "", golden ? argv[2] : "");
	hevcd_free(s); free(s); free(nals); free(d);
	return rc ? 3 : bad;
}
