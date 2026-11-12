// SPDX-License-Identifier: MIT
// h264parse_test <stream.264> [golden.txt]: the direct path's H.264 parser (vtdirect_h264.c) over a recorded Annex B
// stream, without a GPU. The stream is cut into access units and handed over as the length-prefixed samples an MP4
// carries (parameter sets in-band). Prints, in the format of tests/h264/mkgolden.py (made from libavcodec 9.0.2's own
// decoder: ffmpeg -debug pict+mmco and the trace_headers bitstream filter):
//   sps ... / pps ...                    the fields that go into StdVideoH264SequenceParameterSet / PictureParameterSet,
//                                        scaling lists included
//   pic <type> [IDR] frame=.. poc=../.. slices=.. S <fn:poc>... L <idx:fn:poc>...
//                                        each picture's frame_num and POCs, and the reference pictures it is decoded
//                                        with (the DPB before it, in libavcodec's order: what the Vulkan Video decode gets)
// With a golden file it compares line by line and exits 1 at the first difference.
// Build (here, no FFmpeg): cc -O1 -Wall -I<mesa>/include h264parse_test.c ../vtdirect_h264.c -o h264parse_test
#include "../vtdirect_h264.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t n; } Nal;

static int first_mb_is_zero(const uint8_t *nal, size_t n) {   // first_mb_in_slice == 0: ue(v) "1"
	return n > 1 && (nal[1] & 0x80);
}

static void print_list(char *b, size_t bn, const uint8_t *l, int n) {
	size_t o = strlen(b);
	for (int i = 0; i < n && o + 5 < bn; i++) o += (size_t)snprintf(b + o, bn - o, "%s%u", i ? "," : "", l[i]);
}
static void print_scaling(char *b, size_t bn, const StdVideoH264ScalingLists *sl, int present_flag) {
	if (!present_flag) { strlcat(b, " scaling=-", bn); return; }
	char t[64]; snprintf(t, sizeof t, " scaling=%x/%x", sl->scaling_list_present_mask, sl->use_default_scaling_matrix_mask); strlcat(b, t, bn);
	for (int i = 0; i < 12; i++) {
		if (!(sl->scaling_list_present_mask & (1u << i)) || (sl->use_default_scaling_matrix_mask & (1u << i))) continue;
		snprintf(t, sizeof t, " L%d=", i); strlcat(b, t, bn);
		if (i < 6) print_list(b, bn, sl->ScalingList4x4[i], 16); else print_list(b, bn, sl->ScalingList8x8[i - 6], 64);
	}
}
static void line_sps(const H264DSps *p, int id, char *b, size_t bn) {
	const StdVideoH264SequenceParameterSet *d = &p->std;
	snprintf(b, bn, "sps id=%d profile=%d level=%d chroma=%d lfn=%d poc_type=%d lpoc=%d refs=%d w=%u h=%u fmo=%d d8=%d crop=%u,%u,%u,%u",
	         id, p->profile_idc, p->level_idc, d->chroma_format_idc, d->log2_max_frame_num_minus4, d->pic_order_cnt_type,
	         d->log2_max_pic_order_cnt_lsb_minus4, d->max_num_ref_frames, d->pic_width_in_mbs_minus1, d->pic_height_in_map_units_minus1,
	         d->flags.frame_mbs_only_flag, d->flags.direct_8x8_inference_flag, d->frame_crop_left_offset, d->frame_crop_right_offset,
	         d->frame_crop_top_offset, d->frame_crop_bottom_offset);
	print_scaling(b, bn, &p->scaling, d->flags.seq_scaling_matrix_present_flag);
}
static void line_pps(const H264DPps *p, int id, char *b, size_t bn) {
	const StdVideoH264PictureParameterSet *d = &p->std;
	snprintf(b, bn, "pps id=%d sps=%d cabac=%d l0=%d l1=%d wp=%d wb=%d qp=%d qs=%d cqp=%d dbk=%d cip=%d rpc=%d t8=%d cqp2=%d",
	         id, p->sps_id, d->flags.entropy_coding_mode_flag, d->num_ref_idx_l0_default_active_minus1, d->num_ref_idx_l1_default_active_minus1,
	         d->flags.weighted_pred_flag, d->weighted_bipred_idc, d->pic_init_qp_minus26, d->pic_init_qs_minus26, d->chroma_qp_index_offset,
	         d->flags.deblocking_filter_control_present_flag, d->flags.constrained_intra_pred_flag, d->flags.redundant_pic_cnt_present_flag,
	         d->flags.transform_8x8_mode_flag, d->second_chroma_qp_index_offset);
	print_scaling(b, bn, &p->scaling, d->flags.pic_scaling_matrix_present_flag);
}
static void line_pic(const H264DPicture *pic, char *b, size_t bn) {
	static const char type[] = "PBI";
	snprintf(b, bn, "pic %c%s frame=%d poc=%d/%d slices=%u S", type[pic->slice_type], pic->idr ? " IDR" : "", pic->frame_num, pic->poc[0], pic->poc[1], pic->nslices);
	char t[64];
	for (int i = 0; i < pic->nrefs; i++) if (!pic->refs[i].long_term) {
		snprintf(t, sizeof t, " %d:%d", pic->refs[i].frame_num, pic->refs[i].poc[0] < pic->refs[i].poc[1] ? pic->refs[i].poc[0] : pic->refs[i].poc[1]);
		strlcat(b, t, bn);
	}
	strlcat(b, " L", bn);
	for (int i = 0; i < pic->nrefs; i++) if (pic->refs[i].long_term) {
		snprintf(t, sizeof t, " %d:%d", pic->refs[i].frame_num, pic->refs[i].poc[0] < pic->refs[i].poc[1] ? pic->refs[i].poc[0] : pic->refs[i].poc[1]);
		strlcat(b, t, bn);
	}
}

static FILE *golden; static int lineno, bad;
static void emit(const char *l) {
	lineno++;
	if (!golden) { printf("%s\n", l); return; }
	char g[8192];
	if (!fgets(g, sizeof g, golden)) { if (!bad) printf("line %d: golden ends; got: %s\n", lineno, l); bad = 1; return; }
	g[strcspn(g, "\n")] = 0;
	if (strcmp(g, l) && !bad) { printf("line %d differs\n  want: %s\n  got:  %s\n", lineno, g, l); bad = 1; }
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

static int run_au(H264DState *s, const Nal *au, int n) {
	char l[16384];
	for (int i = 0; i < n; i++) {   // parameter sets one by one first, to print each as it comes
		const int t = au[i].p[0] & 0x1f;
		if (t != 7 && t != 8) continue;
		size_t sz; uint8_t *b = pack(&au[i], 1, &sz);
		H264DPicture pic;
		const int r = h264d_parse_sample(s, b, sz, NULL, 0, &pic);
		free(b);
		if (r < 0) { printf("parameter set: %d %s\n", r, h264d_why(s)); return r; }
		if (t == 7) line_sps(&s->sps[s->last_sps], s->last_sps, l, sizeof l);
		else line_pps(s->pps[s->last_pps], s->last_pps, l, sizeof l);
		emit(l);
	}
	size_t sz; uint8_t *b = pack(au, n, &sz);
	const size_t need = h264d_bitstream_size(s, b, sz);
	uint8_t *out = malloc(need ? need : 1);
	H264DPicture pic;
	const int r = h264d_parse_sample(s, b, sz, out, need, &pic);
	if (r == H264D_PICTURE) {
		if (pic.size != need) { printf("bitstream size %zu, expected %zu\n", pic.size, need); bad = 1; }
		for (uint32_t i = 0; i < pic.nslices; i++)
			if (pic.slice_offsets[i] + 3 > pic.size || out[pic.slice_offsets[i]] || out[pic.slice_offsets[i] + 1] || out[pic.slice_offsets[i] + 2] != 1) {
				printf("slice %u: no start code at %u\n", i, pic.slice_offsets[i]); bad = 1;
			}
		line_pic(&pic, l, sizeof l);
		emit(l);
		h264d_finish_picture(s, &pic);
	} else if (r < 0) printf("sample: %d %s\n", r, h264d_why(s));
	free(b); free(out);
	return r < 0 ? r : 0;
}

int main(int argc, char **argv) {
	if (argc < 2) { fprintf(stderr, "usage: h264parse_test <stream.264> [golden.txt]\n"); return 2; }
	FILE *f = fopen(argv[1], "rb");
	if (!f) { perror(argv[1]); return 2; }
	fseek(f, 0, SEEK_END); const long fl = ftell(f); fseek(f, 0, SEEK_SET);
	uint8_t *d = malloc((size_t)fl + 4);
	if (fread(d, 1, (size_t)fl, f) != (size_t)fl) { perror("read"); return 2; }
	fclose(f);
	if (argc > 2 && !(golden = fopen(argv[2], "r"))) { perror(argv[2]); return 2; }
	// NAL units of the Annex B stream
	Nal *nals = malloc(sizeof(Nal) * ((size_t)fl / 3 + 2)); int nn = 0;
	size_t i = 0, start = 0; int in = 0;
	while (i + 3 <= (size_t)fl) {
		if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
			if (in) { size_t e = i; while (e > start && d[e - 1] == 0) e--; nals[nn++] = (Nal){ d + start, e - start }; }
			start = i + 3; in = 1; i += 3;
		} else i++;
	}
	if (in && (size_t)fl > start) nals[nn++] = (Nal){ d + start, (size_t)fl - start };
	// access units (7.4.1.2.3): a new one at an AUD / SPS / PPS / SEI after a slice, or at a slice with first_mb 0
	H264DState *s = calloc(1, sizeof *s);
	h264d_init(s, 4);
	int au0 = 0, has_slice = 0, aus = 0, rc = 0;
	for (int k = 0; k <= nn && !rc; k++) {
		const int t = k < nn ? nals[k].p[0] & 0x1f : -1;
		const int vcl = t == 1 || t == 5;
		const int boundary = k == nn || (has_slice && ((t >= 6 && t <= 9) || (vcl && first_mb_is_zero(nals[k].p, nals[k].n))));
		if (boundary && k > au0) { rc = run_au(s, nals + au0, k - au0); au0 = k; has_slice = 0; aus++; }
		if (vcl) has_slice = 1;
	}
	if (golden && !bad) { char g[256]; if (fgets(g, sizeof g, golden)) { printf("line %d: golden has more: %s", lineno + 1, g); bad = 1; } }
	fprintf(stderr, "%s: %d access units, %d lines%s%s\n", argv[1], aus, lineno, golden ? (bad ? ", DIFFERS from " : ", identical to ") : "", golden ? argv[2] : "");
	h264d_free(s); free(s); free(nals); free(d);
	return rc ? 3 : bad;
}
