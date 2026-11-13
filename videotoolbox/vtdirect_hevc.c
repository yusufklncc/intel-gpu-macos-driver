// SPDX-License-Identifier: MIT
// vtdirect_hevc.c: HEVC parameter sets, slice segment headers, picture order counts and the RPS-driven decoded picture
// buffer for the direct Vulkan Video decode path (see vtdirect_hevc.h). Written from ITU-T H.265 (V10) 7.3 / 7.4 /
// 8.3.1 / 8.3.2 / E.2, with libavcodec 9.0.2's choices where the hardware must see the same values as on the
// libavcodec path.
#include "vtdirect_hevc.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// Bit reader over a NAL unit: emulation prevention bytes (00 00 03) are skipped as they are read; pos counts the RBSP
// bits read (the NAL unit header included when the reader starts at the NAL unit)

typedef struct { const uint8_t *p, *end; int zeros, left, err; uint8_t cur; long pos; } BR;

static void br_init(BR *b, const uint8_t *p, size_t n) { *b = (BR){ p, p + n, 0, 0, 0, 0, 0 }; }
static unsigned br_bit(BR *b) {
	if (!b->left) {
		if (b->p >= b->end) { b->err = 1; return 0; }
		uint8_t c = *b->p++;
		if (b->zeros >= 2 && c == 3) {   // emulation_prevention_three_byte
			b->zeros = 0;
			if (b->p >= b->end) { b->err = 1; return 0; }
			c = *b->p++;
		}
		b->zeros = c ? 0 : b->zeros + 1;
		b->cur = c; b->left = 8;
	}
	b->pos++;
	return (b->cur >> --b->left) & 1;
}
static uint32_t br_u(BR *b, int n) { uint32_t v = 0; while (n-- > 0) v = v << 1 | br_bit(b); return v; }
static uint32_t br_ue(BR *b) {
	int lz = 0;
	while (!br_bit(b)) { if (++lz > 31 || b->err) { b->err = 1; return 0; } }
	return lz ? (uint32_t)(((1ull << lz) - 1) + br_u(b, lz)) : 0;
}
static int32_t br_se(BR *b) { const uint32_t k = br_ue(b); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
static int ceil_log2(unsigned v) { int n = 0; while ((1u << n) < v) n++; return n; }

// ---------------------------------------------------------------------------------------------------------------------

static int fail(HevcDState *s, int code, const char *why) { snprintf(s->why, sizeof s->why, "%s", why); return code; }
const char *hevcd_why(const HevcDState *s) { return s->why; }

enum {
	TRAIL_N = 0, TRAIL_R, TSA_N, TSA_R, STSA_N, STSA_R, RADL_N, RADL_R, RASL_N, RASL_R,
	BLA_W_LP = 16, BLA_W_RADL, BLA_N_LP, IDR_W_RADL, IDR_N_LP, CRA_NUT,
	VPS_NUT = 32, SPS_NUT, PPS_NUT, AUD_NUT, EOS_NUT, EOB_NUT,
};

const char *hevcd_nal_type_name(int t) {
	static const char *n[] = { "TRAIL_N", "TRAIL_R", "TSA_N", "TSA_R", "STSA_N", "STSA_R", "RADL_N", "RADL_R", "RASL_N",
	                           "RASL_R" };
	static const char *irap[] = { "BLA_W_LP", "BLA_W_RADL", "BLA_N_LP", "IDR_W_RADL", "IDR_N_LP", "CRA_NUT" };
	return t >= 0 && t <= 9 ? n[t] : t >= 16 && t <= 21 ? irap[t - 16] : "RSV";
}

int hevcd_level_idc_to_std(int l) {   // general_level_idc is 30 x the level number
	static const struct { int idc; StdVideoH265LevelIdc std; } t[] = {
		{ 30, STD_VIDEO_H265_LEVEL_IDC_1_0 }, { 60, STD_VIDEO_H265_LEVEL_IDC_2_0 }, { 63, STD_VIDEO_H265_LEVEL_IDC_2_1 },
		{ 90, STD_VIDEO_H265_LEVEL_IDC_3_0 }, { 93, STD_VIDEO_H265_LEVEL_IDC_3_1 }, { 120, STD_VIDEO_H265_LEVEL_IDC_4_0 },
		{ 123, STD_VIDEO_H265_LEVEL_IDC_4_1 }, { 150, STD_VIDEO_H265_LEVEL_IDC_5_0 }, { 153, STD_VIDEO_H265_LEVEL_IDC_5_1 },
		{ 156, STD_VIDEO_H265_LEVEL_IDC_5_2 }, { 180, STD_VIDEO_H265_LEVEL_IDC_6_0 }, { 183, STD_VIDEO_H265_LEVEL_IDC_6_1 },
	};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].idc == l) return t[i].std;
	return STD_VIDEO_H265_LEVEL_IDC_6_2;
}

// Table 7-6, in coded (up-right diagonal) order
static const uint8_t default_intra[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 16, 17, 16, 17, 18, 17, 18, 18, 17, 18, 21, 19, 20, 21, 20, 19, 21, 24, 22, 22, 24,
	24, 22, 22, 24, 25, 25, 27, 30, 27, 25, 25, 29, 31, 35, 35, 31, 29, 36, 41, 44, 41, 36, 47, 54, 54, 47, 65, 70, 65, 88, 88, 115 };
static const uint8_t default_inter[64] = {
	16, 16, 16, 16, 16, 16, 16, 16, 16, 16, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 20, 20, 20, 20, 20, 20, 20, 24, 24, 24, 24,
	24, 24, 24, 24, 25, 25, 25, 25, 25, 25, 25, 28, 28, 28, 28, 28, 28, 33, 33, 33, 33, 33, 41, 41, 41, 41, 54, 54, 54, 71, 71, 91 };

static uint8_t *list_of(StdVideoH265ScalingLists *sl, int size, int m) {
	return size == 0 ? sl->ScalingList4x4[m] : size == 1 ? sl->ScalingList8x8[m] : size == 2 ? sl->ScalingList16x16[m] : sl->ScalingList32x32[m / 3];
}
static uint8_t *dc_of(StdVideoH265ScalingLists *sl, int size, int m) {
	return size == 2 ? &sl->ScalingListDCCoef16x16[m] : size == 3 ? &sl->ScalingListDCCoef32x32[m / 3] : NULL;
}
static void default_lists(StdVideoH265ScalingLists *sl) {
	memset(sl, 16, sizeof *sl);   // 4x4 lists and the DC coefficients
	for (int m = 0; m < 6; m++) {
		const uint8_t *d = m < 3 ? default_intra : default_inter;
		memcpy(sl->ScalingList8x8[m], d, 64); memcpy(sl->ScalingList16x16[m], d, 64);
		if (m % 3 == 0) memcpy(sl->ScalingList32x32[m / 3], d, 64);
	}
}
// scaling_list_data() (7.3.4): the lists in coded order after prediction (7.4.5), as StdVideoH265ScalingLists holds them
static int scaling_list_data(BR *b, StdVideoH265ScalingLists *sl) {
	default_lists(sl);
	for (int size = 0; size < 4; size++)
		for (int m = 0; m < 6; m += size == 3 ? 3 : 1) {
			uint8_t *l = list_of(sl, size, m), *dc = dc_of(sl, size, m);
			const int n = size ? 64 : 16;
			if (!br_bit(b)) {   // scaling_list_pred_mode_flag 0: a copy of an earlier list, or the default one
				const uint32_t delta = br_ue(b) * (size == 3 ? 3 : 1);
				if (delta > (uint32_t)m) return -1;
				if (delta) {
					memcpy(l, list_of(sl, size, m - (int)delta), (size_t)n);
					if (dc) *dc = *dc_of(sl, size, m - (int)delta);
				}
			} else {
				int next = 8;
				if (size > 1) {
					const int32_t c = br_se(b);   // scaling_list_dc_coef_minus8
					if (c < -7 || c > 247) return -1;
					next = c + 8; *dc = (uint8_t)next;
				}
				for (int i = 0; i < n; i++) {
					const int32_t d = br_se(b);
					if (d < -128 || d > 127) return -1;
					next = (next + d + 256) % 256;
					l[i] = (uint8_t)next;
				}
			}
			if (b->err) return -1;
		}
	return 0;
}

// profile_tier_level(1, max_sub_layers_minus1)
static void profile_tier_level(BR *b, int max_sub_layers_minus1, StdVideoH265ProfileTierLevel *ptl, int *profile, int *level) {
	br_u(b, 2);                                    // general_profile_space
	ptl->flags.general_tier_flag = br_bit(b);
	int p = (int)br_u(b, 5);
	for (int i = 0; i < 32; i++) if (br_bit(b) && p == 0 && i > 0) p = i;   // libavcodec: a 0 profile from the flags
	ptl->flags.general_progressive_source_flag = br_bit(b);
	ptl->flags.general_interlaced_source_flag = br_bit(b);
	ptl->flags.general_non_packed_constraint_flag = br_bit(b);
	ptl->flags.general_frame_only_constraint_flag = br_bit(b);
	br_u(b, 32); br_u(b, 11);                      // the 43 constraint / reserved bits
	br_bit(b);                                     // general_inbld_flag / reserved
	*level = (int)br_u(b, 8);
	*profile = p;
	ptl->general_profile_idc = (StdVideoH265ProfileIdc)p;
	ptl->general_level_idc = (StdVideoH265LevelIdc)hevcd_level_idc_to_std(*level);
	int prof_present[8] = { 0 }, level_present[8] = { 0 };
	for (int i = 0; i < max_sub_layers_minus1; i++) { prof_present[i] = (int)br_bit(b); level_present[i] = (int)br_bit(b); }
	if (max_sub_layers_minus1 > 0) for (int i = max_sub_layers_minus1; i < 8; i++) br_u(b, 2);
	for (int i = 0; i < max_sub_layers_minus1; i++) {
		if (prof_present[i]) { br_u(b, 32); br_u(b, 32); br_u(b, 24); }   // 88 bits
		if (level_present[i]) br_u(b, 8);
	}
}

static void sub_layer_hrd(BR *b, int cpb_cnt, int sub_pic, StdVideoH265SubLayerHrdParameters *h) {
	for (int j = 0; j <= cpb_cnt && j < 32; j++) {
		h->bit_rate_value_minus1[j] = br_ue(b);
		h->cpb_size_value_minus1[j] = br_ue(b);
		if (sub_pic) { h->cpb_size_du_value_minus1[j] = br_ue(b); h->bit_rate_du_value_minus1[j] = br_ue(b); }
		h->cbr_flag |= br_bit(b) << j;
	}
}
// hrd_parameters(1, max_sub_layers_minus1) (E.2.2)
static int hrd_parameters(BR *b, int max_sub_layers_minus1, StdVideoH265HrdParameters *h, StdVideoH265SubLayerHrdParameters *nal,
                          StdVideoH265SubLayerHrdParameters *vcl) {
	h->flags.nal_hrd_parameters_present_flag = br_bit(b);
	h->flags.vcl_hrd_parameters_present_flag = br_bit(b);
	if (h->flags.nal_hrd_parameters_present_flag || h->flags.vcl_hrd_parameters_present_flag) {
		h->flags.sub_pic_hrd_params_present_flag = br_bit(b);
		if (h->flags.sub_pic_hrd_params_present_flag) {
			h->tick_divisor_minus2 = (uint8_t)br_u(b, 8);
			h->du_cpb_removal_delay_increment_length_minus1 = (uint8_t)br_u(b, 5);
			h->flags.sub_pic_cpb_params_in_pic_timing_sei_flag = br_bit(b);
			h->dpb_output_delay_du_length_minus1 = (uint8_t)br_u(b, 5);
		}
		h->bit_rate_scale = (uint8_t)br_u(b, 4);
		h->cpb_size_scale = (uint8_t)br_u(b, 4);
		if (h->flags.sub_pic_hrd_params_present_flag) h->cpb_size_du_scale = (uint8_t)br_u(b, 4);
		h->initial_cpb_removal_delay_length_minus1 = (uint8_t)br_u(b, 5);
		h->au_cpb_removal_delay_length_minus1 = (uint8_t)br_u(b, 5);
		h->dpb_output_delay_length_minus1 = (uint8_t)br_u(b, 5);
	}
	for (int i = 0; i <= max_sub_layers_minus1; i++) {
		int general = (int)br_bit(b), within = 1, low_delay = 0;
		if (!general) within = (int)br_bit(b);
		h->flags.fixed_pic_rate_general_flag |= (uint32_t)general << i;
		h->flags.fixed_pic_rate_within_cvs_flag |= (uint32_t)within << i;
		if (within) h->elemental_duration_in_tc_minus1[i] = (uint16_t)br_ue(b);
		else low_delay = (int)br_bit(b);
		h->flags.low_delay_hrd_flag |= (uint32_t)low_delay << i;
		uint32_t cpb = 0;
		if (!low_delay) { cpb = br_ue(b); if (cpb > 31) return -1; }
		h->cpb_cnt_minus1[i] = (uint8_t)cpb;
		if (h->flags.nal_hrd_parameters_present_flag) sub_layer_hrd(b, (int)cpb, h->flags.sub_pic_hrd_params_present_flag, &nal[i]);
		if (h->flags.vcl_hrd_parameters_present_flag) sub_layer_hrd(b, (int)cpb, h->flags.sub_pic_hrd_params_present_flag, &vcl[i]);
		if (b->err) return -1;
	}
	return 0;
}

// st_ref_pic_set(idx) (7.3.7, 7.4.8): the syntax into std (when not NULL), the derived set into rps[idx]
static int st_ref_pic_set(BR *b, int idx, int num_sets, HevcDRps *rps, StdVideoH265ShortTermRefPicSet *std, int *ref_num_delta) {
	HevcDRps *r = &rps[idx];
	StdVideoH265ShortTermRefPicSet t; memset(&t, 0, sizeof t);
	memset(r, 0, sizeof *r);
	if (ref_num_delta) *ref_num_delta = 0;
	const int inter = idx ? (int)br_bit(b) : 0;
	t.flags.inter_ref_pic_set_prediction_flag = (uint32_t)inter;
	if (inter) {
		uint32_t delta_idx_minus1 = 0;
		if (idx == num_sets) { delta_idx_minus1 = br_ue(b); if (delta_idx_minus1 >= (uint32_t)idx) return -1; }
		const HevcDRps *ref = &rps[idx - (int)delta_idx_minus1 - 1];
		const int sign = (int)br_bit(b);
		const uint32_t abs_minus1 = br_ue(b);
		if (abs_minus1 > 32767) return -1;
		const int32_t delta_rps = (1 - 2 * sign) * ((int32_t)abs_minus1 + 1);
		t.flags.delta_rps_sign = (uint32_t)sign; t.delta_idx_minus1 = delta_idx_minus1; t.abs_delta_rps_minus1 = (uint16_t)abs_minus1;
		const int nref = ref->num_negative + ref->num_positive;
		if (ref_num_delta) *ref_num_delta = nref;
		int used[33], use_delta[33];
		for (int j = 0; j <= nref; j++) {
			used[j] = (int)br_bit(b); use_delta[j] = 1;
			if (!used[j]) use_delta[j] = (int)br_bit(b);
			t.used_by_curr_pic_flag |= (uint16_t)(used[j] << j);
			t.use_delta_flag |= (uint16_t)(use_delta[j] << j);
		}
		// (7-61), (7-62)
		int i = 0;
		for (int j = ref->num_positive - 1; j >= 0; j--) {
			const int32_t d = ref->delta[ref->num_negative + j] + delta_rps;
			if (d < 0 && use_delta[ref->num_negative + j]) { r->delta[i] = d; r->used |= (uint32_t)used[ref->num_negative + j] << i; i++; }
		}
		if (delta_rps < 0 && use_delta[nref]) { r->delta[i] = delta_rps; r->used |= (uint32_t)used[nref] << i; i++; }
		for (int j = 0; j < ref->num_negative; j++) {
			const int32_t d = ref->delta[j] + delta_rps;
			if (d < 0 && use_delta[j]) { r->delta[i] = d; r->used |= (uint32_t)used[j] << i; i++; }
		}
		r->num_negative = i;
		for (int j = ref->num_negative - 1; j >= 0; j--) {
			const int32_t d = ref->delta[j] + delta_rps;
			if (d > 0 && use_delta[j]) { r->delta[i] = d; r->used |= (uint32_t)used[j] << i; i++; }
		}
		if (delta_rps > 0 && use_delta[nref]) { r->delta[i] = delta_rps; r->used |= (uint32_t)used[nref] << i; i++; }
		for (int j = 0; j < ref->num_positive; j++) {
			const int32_t d = ref->delta[ref->num_negative + j] + delta_rps;
			if (d > 0 && use_delta[ref->num_negative + j]) { r->delta[i] = d; r->used |= (uint32_t)used[ref->num_negative + j] << i; i++; }
		}
		r->num_positive = i - r->num_negative;
		if (r->num_negative > 16 || r->num_positive > 16 || i > 16) return -1;
	} else {
		const uint32_t neg = br_ue(b), pos = br_ue(b);
		if (neg > 16 || pos > 16 || neg + pos > 16) return -1;
		r->num_negative = (int)neg; r->num_positive = (int)pos;
		int32_t prev = 0;
		for (uint32_t i = 0; i < neg; i++) {
			const uint32_t d = br_ue(b);
			if (d > 32767) return -1;
			prev -= (int32_t)d + 1; r->delta[i] = prev;
			r->used |= br_bit(b) << i;
		}
		prev = 0;
		for (uint32_t i = 0; i < pos; i++) {
			const uint32_t d = br_ue(b);
			if (d > 32767) return -1;
			prev += (int32_t)d + 1; r->delta[neg + i] = prev;
			r->used |= br_bit(b) << (neg + i);
		}
	}
	if (b->err) return -1;
	// the Std form of the derived set (libavcodec's: counts, delta_poc_sN_minus1 between neighbours, used bits per list)
	t.num_negative_pics = (uint8_t)r->num_negative; t.num_positive_pics = (uint8_t)r->num_positive;
	for (int i = 0; i < r->num_negative; i++) {
		t.delta_poc_s0_minus1[i] = (uint16_t)((i ? r->delta[i - 1] : 0) - r->delta[i] - 1);
		t.used_by_curr_pic_s0_flag |= (uint16_t)(((r->used >> i) & 1) << i);
	}
	for (int i = 0; i < r->num_positive; i++) {
		t.delta_poc_s1_minus1[i] = (uint16_t)(r->delta[r->num_negative + i] - (i ? r->delta[r->num_negative + i - 1] : 0) - 1);
		t.used_by_curr_pic_s1_flag |= (uint16_t)(((r->used >> (r->num_negative + i)) & 1) << i);
	}
	if (!inter) t.used_by_curr_pic_flag = (uint16_t)r->used;
	if (std) *std = t;
	return 0;
}

// vui_parameters() (E.2.1)
static int vui_parameters(BR *b, HevcDSps *t) {
	StdVideoH265SequenceParameterSetVui *v = &t->vui;
	v->flags.aspect_ratio_info_present_flag = br_bit(b);
	if (v->flags.aspect_ratio_info_present_flag) {
		v->aspect_ratio_idc = (StdVideoH265AspectRatioIdc)br_u(b, 8);
		if (v->aspect_ratio_idc == 255) { v->sar_width = (uint16_t)br_u(b, 16); v->sar_height = (uint16_t)br_u(b, 16); }
	}
	v->flags.overscan_info_present_flag = br_bit(b);
	if (v->flags.overscan_info_present_flag) v->flags.overscan_appropriate_flag = br_bit(b);
	v->flags.video_signal_type_present_flag = br_bit(b);
	t->full_range = -1;
	if (v->flags.video_signal_type_present_flag) {
		v->video_format = (uint8_t)br_u(b, 3);
		v->flags.video_full_range_flag = br_bit(b);
		t->full_range = (int)v->flags.video_full_range_flag;
		v->flags.colour_description_present_flag = br_bit(b);
		if (v->flags.colour_description_present_flag) {
			v->colour_primaries = (uint8_t)br_u(b, 8); v->transfer_characteristics = (uint8_t)br_u(b, 8); v->matrix_coeffs = (uint8_t)br_u(b, 8);
		}
	}
	v->flags.chroma_loc_info_present_flag = br_bit(b);
	if (v->flags.chroma_loc_info_present_flag) {
		v->chroma_sample_loc_type_top_field = (uint8_t)br_ue(b); v->chroma_sample_loc_type_bottom_field = (uint8_t)br_ue(b);
	}
	v->flags.neutral_chroma_indication_flag = br_bit(b);
	v->flags.field_seq_flag = br_bit(b);
	v->flags.frame_field_info_present_flag = br_bit(b);
	v->flags.default_display_window_flag = br_bit(b);
	if (v->flags.default_display_window_flag) {
		v->def_disp_win_left_offset = (uint16_t)br_ue(b); v->def_disp_win_right_offset = (uint16_t)br_ue(b);
		v->def_disp_win_top_offset = (uint16_t)br_ue(b); v->def_disp_win_bottom_offset = (uint16_t)br_ue(b);
	}
	v->flags.vui_timing_info_present_flag = br_bit(b);
	if (v->flags.vui_timing_info_present_flag) {
		v->vui_num_units_in_tick = br_u(b, 32); v->vui_time_scale = br_u(b, 32);
		v->flags.vui_poc_proportional_to_timing_flag = br_bit(b);
		if (v->flags.vui_poc_proportional_to_timing_flag) v->vui_num_ticks_poc_diff_one_minus1 = br_ue(b);
		v->flags.vui_hrd_parameters_present_flag = br_bit(b);
		if (v->flags.vui_hrd_parameters_present_flag &&
		    hrd_parameters(b, t->std.sps_max_sub_layers_minus1, &t->hrd, t->hrd_nal, t->hrd_vcl)) return -1;
	}
	v->flags.bitstream_restriction_flag = br_bit(b);
	if (v->flags.bitstream_restriction_flag) {
		v->flags.tiles_fixed_structure_flag = br_bit(b);
		v->flags.motion_vectors_over_pic_boundaries_flag = br_bit(b);
		v->flags.restricted_ref_pic_lists_flag = br_bit(b);
		v->min_spatial_segmentation_idc = (uint16_t)br_ue(b);
		v->max_bytes_per_pic_denom = (uint8_t)br_ue(b);
		v->max_bits_per_min_cu_denom = (uint8_t)br_ue(b);
		v->log2_max_mv_length_horizontal = (uint8_t)br_ue(b);
		v->log2_max_mv_length_vertical = (uint8_t)br_ue(b);
	}
	return b->err ? -1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Parameter sets. A replaced set is kept until the next sample (a picture of this sample may point at it).

static void retire(HevcDState *s, void *p, uint8_t *raw) {
	if (!p) return;
	free(raw);
	if (s->nretired == 128) { free(p); return; }   // more than one sample of sane size replaces
	s->retired[s->nretired++] = p;
}
static void free_retired(HevcDState *s) { for (int i = 0; i < s->nretired; i++) free(s->retired[i]); s->nretired = 0; }
static int same_raw(const uint8_t *raw, size_t rawn, const uint8_t *nal, size_t n) { return raw && rawn == n && !memcmp(raw, nal, n); }

static int parse_vps(HevcDState *s, const uint8_t *nal, size_t n) {
	BR b; br_init(&b, nal + 2, n - 2);
	const uint32_t vid = nal[2] >> 4;
	if (s->vps[vid] && same_raw(s->vps[vid]->raw, s->vps[vid]->rawn, nal, n)) { s->last_vps = (int)vid; return HEVCD_NO_PICTURE; }
	HevcDVps *t = calloc(1, sizeof *t);
	if (!t) return fail(s, HEVCD_ERROR, "no memory");
	StdVideoH265VideoParameterSet *d = &t->std;
	const uint32_t id = br_u(&b, 4);
	br_u(&b, 2); br_u(&b, 6);   // vps_base_layer_internal_flag, vps_base_layer_available_flag, vps_max_layers_minus1
	d->vps_video_parameter_set_id = (uint8_t)id;
	d->vps_max_sub_layers_minus1 = (uint8_t)br_u(&b, 3);
	d->flags.vps_temporal_id_nesting_flag = br_bit(&b);
	br_u(&b, 16);
	if (d->vps_max_sub_layers_minus1 >= HEVCD_MAX_SUB_LAYERS) { free(t); return fail(s, HEVCD_ERROR, "VPS sub-layers"); }
	profile_tier_level(&b, d->vps_max_sub_layers_minus1, &t->ptl, &t->profile, &t->level_idc);
	d->flags.vps_sub_layer_ordering_info_present_flag = br_bit(&b);
	const int start = d->flags.vps_sub_layer_ordering_info_present_flag ? 0 : d->vps_max_sub_layers_minus1;
	for (int i = start; i <= d->vps_max_sub_layers_minus1; i++) {
		t->dpbm.max_dec_pic_buffering_minus1[i] = (uint8_t)br_ue(&b);
		t->dpbm.max_num_reorder_pics[i] = (uint8_t)br_ue(&b);
		t->dpbm.max_latency_increase_plus1[i] = br_ue(&b);
	}
	for (int i = 0; i < start; i++) {
		t->dpbm.max_dec_pic_buffering_minus1[i] = t->dpbm.max_dec_pic_buffering_minus1[start];
		t->dpbm.max_num_reorder_pics[i] = t->dpbm.max_num_reorder_pics[start];
		t->dpbm.max_latency_increase_plus1[i] = t->dpbm.max_latency_increase_plus1[start];
	}
	// vps_max_layer_id, the layer sets, the timing information and its HRD parameters: not used by the decode
	const uint32_t max_layer_id = br_u(&b, 6), sets = br_ue(&b);
	if (sets > 1023) { free(t); return fail(s, HEVCD_ERROR, "VPS layer sets"); }
	for (uint32_t i = 1; i <= sets; i++) for (uint32_t j = 0; j <= max_layer_id; j++) br_bit(&b);
	d->flags.vps_timing_info_present_flag = br_bit(&b);
	if (d->flags.vps_timing_info_present_flag) {
		d->vps_num_units_in_tick = br_u(&b, 32); d->vps_time_scale = br_u(&b, 32);
		d->flags.vps_poc_proportional_to_timing_flag = br_bit(&b);
		if (d->flags.vps_poc_proportional_to_timing_flag) d->vps_num_ticks_poc_diff_one_minus1 = br_ue(&b);
	}
	if (b.err) { free(t); return fail(s, HEVCD_ERROR, "VPS truncated"); }
	d->pDecPicBufMgr = &t->dpbm; d->pProfileTierLevel = &t->ptl; d->pHrdParameters = NULL;
	if (!(t->raw = malloc(n))) { free(t); return fail(s, HEVCD_ERROR, "no memory"); }
	memcpy(t->raw, nal, n); t->rawn = n;
	if (s->vps[id]) {   // libavcodec (remove_vps): a VPS with other content drops the SPSs that use it, and their PPSs
		retire(s, s->vps[id], s->vps[id]->raw);
		for (int k = 0; k < 16; k++) if (s->sps[k] && s->sps[k]->std.sps_video_parameter_set_id == id) {
			for (int i = 0; i < 64; i++) if (s->pps[i] && s->pps[i]->sps_id == k) { retire(s, s->pps[i], s->pps[i]->raw); s->pps[i] = NULL; }
			retire(s, s->sps[k], s->sps[k]->raw); s->sps[k] = NULL;
		}
	}
	s->vps[id] = t;
	s->last_vps = (int)id;
	return HEVCD_NO_PICTURE;
}

static int parse_sps(HevcDState *s, const uint8_t *nal, size_t n) {
	BR b; br_init(&b, nal + 2, n - 2);
	const uint32_t vps_id = br_u(&b, 4), sub_minus1 = br_u(&b, 3);
	if (sub_minus1 >= HEVCD_MAX_SUB_LAYERS) return fail(s, HEVCD_ERROR, "SPS sub-layers");
	HevcDSps *t = calloc(1, sizeof *t);
	if (!t) return fail(s, HEVCD_ERROR, "no memory");
#define BAD(w) do { free(t); return fail(s, HEVCD_ERROR, w); } while (0)
	StdVideoH265SequenceParameterSet *d = &t->std;
	d->sps_video_parameter_set_id = (uint8_t)vps_id;
	d->sps_max_sub_layers_minus1 = (uint8_t)sub_minus1;
	d->flags.sps_temporal_id_nesting_flag = br_bit(&b);
	profile_tier_level(&b, (int)sub_minus1, &t->ptl, &t->profile, &t->level_idc);
	const uint32_t id = br_ue(&b);
	if (id > 15 || b.err) BAD("SPS id");
	d->sps_seq_parameter_set_id = (uint8_t)id;
	const uint32_t chroma = br_ue(&b);
	if (chroma > 3) BAD("SPS chroma_format_idc");
	d->chroma_format_idc = (StdVideoH265ChromaFormatIdc)chroma;
	if (chroma == 3) d->flags.separate_colour_plane_flag = br_bit(&b);
	d->pic_width_in_luma_samples = br_ue(&b); d->pic_height_in_luma_samples = br_ue(&b);
	d->flags.conformance_window_flag = br_bit(&b);
	if (d->flags.conformance_window_flag) {
		for (int i = 0; i < 4; i++) t->conf[i] = br_ue(&b);
		d->conf_win_left_offset = t->conf[0]; d->conf_win_right_offset = t->conf[1];
		d->conf_win_top_offset = t->conf[2]; d->conf_win_bottom_offset = t->conf[3];
	}
	const uint32_t bdl = br_ue(&b), bdc = br_ue(&b), lpoc = br_ue(&b);
	if (bdl > 8 || bdc > 8 || lpoc > 12) BAD("SPS bit depth / log2_max_pic_order_cnt_lsb");
	d->bit_depth_luma_minus8 = (uint8_t)bdl; d->bit_depth_chroma_minus8 = (uint8_t)bdc;
	d->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)lpoc;
	t->log2_max_poc_lsb = (int)lpoc + 4; t->bit_depth = (int)bdl + 8;
	d->flags.sps_sub_layer_ordering_info_present_flag = br_bit(&b);
	const int start = d->flags.sps_sub_layer_ordering_info_present_flag ? 0 : (int)sub_minus1;
	for (int i = start; i <= (int)sub_minus1; i++) {
		const uint32_t dpb = br_ue(&b), reorder = br_ue(&b), lat = br_ue(&b);
		if (dpb > 15 || reorder > 15) BAD("SPS sps_max_dec_pic_buffering");
		t->dpbm.max_dec_pic_buffering_minus1[i] = (uint8_t)dpb; t->dpbm.max_num_reorder_pics[i] = (uint8_t)reorder;
		t->dpbm.max_latency_increase_plus1[i] = lat;
	}
	for (int i = 0; i < start; i++) {
		t->dpbm.max_dec_pic_buffering_minus1[i] = t->dpbm.max_dec_pic_buffering_minus1[start];
		t->dpbm.max_num_reorder_pics[i] = t->dpbm.max_num_reorder_pics[start];
		t->dpbm.max_latency_increase_plus1[i] = t->dpbm.max_latency_increase_plus1[start];
	}
	const uint32_t mincb = br_ue(&b), diffcb = br_ue(&b), mintb = br_ue(&b), difftb = br_ue(&b);
	if (mincb > 3 || mincb + diffcb > 3 || mintb > 3 || mintb + difftb > 3) BAD("SPS block sizes");
	d->log2_min_luma_coding_block_size_minus3 = (uint8_t)mincb; d->log2_diff_max_min_luma_coding_block_size = (uint8_t)diffcb;
	d->log2_min_luma_transform_block_size_minus2 = (uint8_t)mintb; d->log2_diff_max_min_luma_transform_block_size = (uint8_t)difftb;
	d->max_transform_hierarchy_depth_inter = (uint8_t)br_ue(&b); d->max_transform_hierarchy_depth_intra = (uint8_t)br_ue(&b);
	d->flags.scaling_list_enabled_flag = br_bit(&b);
	if (d->flags.scaling_list_enabled_flag) {
		t->scaling_coded = (int)br_bit(&b);
		if (t->scaling_coded) { if (scaling_list_data(&b, &t->scaling)) BAD("SPS scaling_list_data"); }
		else default_lists(&t->scaling);
		d->flags.sps_scaling_list_data_present_flag = 1;   // libavcodec: the lists (the defaults if none are coded) are passed
	}
	d->flags.amp_enabled_flag = br_bit(&b);
	d->flags.sample_adaptive_offset_enabled_flag = br_bit(&b);
	d->flags.pcm_enabled_flag = br_bit(&b);
	t->pcm_coded = (int)d->flags.pcm_enabled_flag;
	if (d->flags.pcm_enabled_flag) {
		d->pcm_sample_bit_depth_luma_minus1 = (uint8_t)br_u(&b, 4); d->pcm_sample_bit_depth_chroma_minus1 = (uint8_t)br_u(&b, 4);
		d->log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)br_ue(&b);
		d->log2_diff_max_min_pcm_luma_coding_block_size = (uint8_t)br_ue(&b);
		d->flags.pcm_loop_filter_disabled_flag = br_bit(&b);
	} else {
		// libavcodec passes its zeroed PCM fields minus their offsets, as unsigned 8-bit values; the driver programs
		// them into HCP_PIC_STATE (masked) even without PCM
		d->pcm_sample_bit_depth_luma_minus1 = (uint8_t)(0 - 1); d->pcm_sample_bit_depth_chroma_minus1 = (uint8_t)(0 - 1);
		d->log2_min_pcm_luma_coding_block_size_minus3 = (uint8_t)(0 - 3);
	}
	const uint32_t nsets = br_ue(&b);
	if (nsets > 64) BAD("SPS num_short_term_ref_pic_sets");
	d->num_short_term_ref_pic_sets = (uint8_t)nsets;
	for (uint32_t i = 0; i < nsets; i++) if (st_ref_pic_set(&b, (int)i, (int)nsets, t->rps, &t->st[i], NULL)) BAD("SPS st_ref_pic_set");
	d->flags.long_term_ref_pics_present_flag = br_bit(&b);
	if (d->flags.long_term_ref_pics_present_flag) {
		const uint32_t nlt = br_ue(&b);
		if (nlt > 32) BAD("SPS num_long_term_ref_pics_sps");
		d->num_long_term_ref_pics_sps = (uint8_t)nlt;
		for (uint32_t i = 0; i < nlt; i++) {
			t->lt.lt_ref_pic_poc_lsb_sps[i] = br_u(&b, t->log2_max_poc_lsb);
			t->lt.used_by_curr_pic_lt_sps_flag |= br_bit(&b) << i;
		}
	}
	d->flags.sps_temporal_mvp_enabled_flag = br_bit(&b);
	d->flags.strong_intra_smoothing_enabled_flag = br_bit(&b);
	d->flags.vui_parameters_present_flag = br_bit(&b);
	t->full_range = -1;
	if (d->flags.vui_parameters_present_flag && vui_parameters(&b, t)) BAD("SPS VUI");
	d->flags.sps_extension_present_flag = br_bit(&b);
	int scc = 0;
	if (d->flags.sps_extension_present_flag) {
		d->flags.sps_range_extension_flag = br_bit(&b);
		br_bit(&b); br_bit(&b);   // sps_multilayer_extension_flag, sps_3d_extension_flag (other layers, not decoded here)
		scc = (int)br_bit(&b);
		br_u(&b, 4);              // sps_extension_4bits: extension data, ignored as decoders must
		if (d->flags.sps_range_extension_flag) {
			d->flags.transform_skip_rotation_enabled_flag = br_bit(&b);
			d->flags.transform_skip_context_enabled_flag = br_bit(&b);
			d->flags.implicit_rdpcm_enabled_flag = br_bit(&b);
			d->flags.explicit_rdpcm_enabled_flag = br_bit(&b);
			d->flags.extended_precision_processing_flag = br_bit(&b);
			d->flags.intra_smoothing_disabled_flag = br_bit(&b);
			d->flags.high_precision_offsets_enabled_flag = br_bit(&b);
			d->flags.persistent_rice_adaptation_enabled_flag = br_bit(&b);
			d->flags.cabac_bypass_alignment_enabled_flag = br_bit(&b);
		}
	}
	if (b.err) BAD("SPS truncated");
	const int mincb_size = 1 << (mincb + 3);
	if (!d->pic_width_in_luma_samples || !d->pic_height_in_luma_samples || d->pic_width_in_luma_samples % (uint32_t)mincb_size ||
	    d->pic_height_in_luma_samples % (uint32_t)mincb_size || d->pic_width_in_luma_samples > 16888 || d->pic_height_in_luma_samples > 16888)
		BAD("SPS picture size");
	t->width = (int)d->pic_width_in_luma_samples; t->height = (int)d->pic_height_in_luma_samples;
	t->log2_ctb = (int)(mincb + diffcb) + 3;
	t->ctb_w = (t->width + (1 << t->log2_ctb) - 1) >> t->log2_ctb; t->ctb_h = (t->height + (1 << t->log2_ctb) - 1) >> t->log2_ctb;
	// what the direct path decodes: Main / Main 10 / Main Still Picture, 4:2:0, 8 or 10 bits (luma = chroma), without
	// the range or screen content coding extensions
	if (t->profile != 1 && t->profile != 2 && t->profile != 3) t->unsupported = 1;
	else if (chroma != 1 || d->flags.separate_colour_plane_flag) t->unsupported = 2;
	else if (bdl != bdc || (bdl != 0 && bdl != 2)) t->unsupported = 3;
	else if (d->flags.sps_range_extension_flag || scc) t->unsupported = 4;
	d->pProfileTierLevel = &t->ptl; d->pDecPicBufMgr = &t->dpbm;
	d->pScalingLists = d->flags.scaling_list_enabled_flag ? &t->scaling : NULL;
	d->pShortTermRefPicSet = t->st; d->pLongTermRefPicsSps = &t->lt;
	d->pSequenceParameterSetVui = d->flags.vui_parameters_present_flag ? &t->vui : NULL;
	t->vui.pHrdParameters = &t->hrd; t->hrd.pSubLayerHrdParametersNal = t->hrd_nal; t->hrd.pSubLayerHrdParametersVcl = t->hrd_vcl;
	d->pPredictorPaletteEntries = NULL;
#undef BAD
	s->last_sps = (int)id;
	HevcDSps *old = s->sps[id];
	if (old && same_raw(old->raw, old->rawn, nal, n)) { free(t); return HEVCD_NO_PICTURE; }   // a repeat: nothing changes
	if (!(t->raw = malloc(n))) { free(t); return fail(s, HEVCD_ERROR, "no memory"); }
	memcpy(t->raw, nal, n); t->rawn = n;
	t->gen = ++s->gen;
	if (old) {   // libavcodec: an SPS with other content drops the PPSs that refer to its id
		retire(s, old, old->raw);
		for (int i = 0; i < 64; i++) if (s->pps[i] && s->pps[i]->sps_id == (int)id) { retire(s, s->pps[i], s->pps[i]->raw); s->pps[i] = NULL; }
	}
	s->sps[id] = t;
	return HEVCD_NO_PICTURE;
}

static int parse_pps(HevcDState *s, const uint8_t *nal, size_t n) {
	BR b; br_init(&b, nal + 2, n - 2);
	const uint32_t id = br_ue(&b), sps_id = br_ue(&b);
	if (id > 63 || sps_id > 15 || b.err) return fail(s, HEVCD_ERROR, "PPS ids");
	const HevcDSps *sps = s->sps[sps_id];
	if (!sps) return fail(s, HEVCD_ERROR, "PPS before its SPS");
	s->last_pps = (int)id;
	if (s->pps[id] && same_raw(s->pps[id]->raw, s->pps[id]->rawn, nal, n)) return HEVCD_NO_PICTURE;
	HevcDPps *t = calloc(1, sizeof *t);
	if (!t) return fail(s, HEVCD_ERROR, "no memory");
#define BAD(w) do { free(t); return fail(s, HEVCD_ERROR, w); } while (0)
	StdVideoH265PictureParameterSet *d = &t->std;
	t->sps_id = (int)sps_id;
	d->pps_pic_parameter_set_id = (uint8_t)id; d->pps_seq_parameter_set_id = (uint8_t)sps_id;
	d->sps_video_parameter_set_id = sps->std.sps_video_parameter_set_id;
	d->flags.dependent_slice_segments_enabled_flag = br_bit(&b);
	d->flags.output_flag_present_flag = br_bit(&b);
	d->num_extra_slice_header_bits = (uint8_t)br_u(&b, 3);
	d->flags.sign_data_hiding_enabled_flag = br_bit(&b);
	d->flags.cabac_init_present_flag = br_bit(&b);
	const uint32_t l0 = br_ue(&b), l1 = br_ue(&b);
	if (l0 > 14 || l1 > 14) BAD("PPS num_ref_idx_default_active");
	d->num_ref_idx_l0_default_active_minus1 = (uint8_t)l0; d->num_ref_idx_l1_default_active_minus1 = (uint8_t)l1;
	d->init_qp_minus26 = (int8_t)br_se(&b);
	d->flags.constrained_intra_pred_flag = br_bit(&b);
	d->flags.transform_skip_enabled_flag = br_bit(&b);
	d->flags.cu_qp_delta_enabled_flag = br_bit(&b);
	if (d->flags.cu_qp_delta_enabled_flag) d->diff_cu_qp_delta_depth = (uint8_t)br_ue(&b);
	d->pps_cb_qp_offset = (int8_t)br_se(&b); d->pps_cr_qp_offset = (int8_t)br_se(&b);
	d->flags.pps_slice_chroma_qp_offsets_present_flag = br_bit(&b);
	d->flags.weighted_pred_flag = br_bit(&b); d->flags.weighted_bipred_flag = br_bit(&b);
	d->flags.transquant_bypass_enabled_flag = br_bit(&b);
	d->flags.tiles_enabled_flag = br_bit(&b);
	d->flags.entropy_coding_sync_enabled_flag = br_bit(&b);
	d->flags.loop_filter_across_tiles_enabled_flag = 1;
	t->ncols = 1; t->nrows = 1; t->col[0] = (uint16_t)sps->ctb_w; t->row[0] = (uint16_t)sps->ctb_h;
	if (d->flags.tiles_enabled_flag) {
		const uint32_t cols = br_ue(&b), rows = br_ue(&b);
		if (cols > 19 || rows > 21 || (int)cols >= sps->ctb_w || (int)rows >= sps->ctb_h) BAD("PPS tiles");
		d->num_tile_columns_minus1 = (uint8_t)cols; d->num_tile_rows_minus1 = (uint8_t)rows;
		t->ncols = (int)cols + 1; t->nrows = (int)rows + 1;
		d->flags.uniform_spacing_flag = br_bit(&b);
		// 6.5.1: the column widths and row heights in CTBs; the Std arrays get all but the last (libavcodec's way)
		if (d->flags.uniform_spacing_flag) {
			for (int i = 0; i < t->ncols; i++) t->col[i] = (uint16_t)(((i + 1) * sps->ctb_w) / t->ncols - (i * sps->ctb_w) / t->ncols);
			for (int i = 0; i < t->nrows; i++) t->row[i] = (uint16_t)(((i + 1) * sps->ctb_h) / t->nrows - (i * sps->ctb_h) / t->nrows);
		} else {
			int sum = 0;
			for (int i = 0; i < t->ncols - 1; i++) { t->col[i] = (uint16_t)(br_ue(&b) + 1); sum += t->col[i]; }
			if (sum >= sps->ctb_w) BAD("PPS column widths");
			t->col[t->ncols - 1] = (uint16_t)(sps->ctb_w - sum);
			sum = 0;
			for (int i = 0; i < t->nrows - 1; i++) { t->row[i] = (uint16_t)(br_ue(&b) + 1); sum += t->row[i]; }
			if (sum >= sps->ctb_h) BAD("PPS row heights");
			t->row[t->nrows - 1] = (uint16_t)(sps->ctb_h - sum);
		}
		for (int i = 0; i < t->ncols - 1; i++) d->column_width_minus1[i] = (uint16_t)(t->col[i] - 1);
		for (int i = 0; i < t->nrows - 1; i++) d->row_height_minus1[i] = (uint16_t)(t->row[i] - 1);
		d->flags.loop_filter_across_tiles_enabled_flag = br_bit(&b);
	}
	d->flags.pps_loop_filter_across_slices_enabled_flag = br_bit(&b);
	d->flags.deblocking_filter_control_present_flag = br_bit(&b);
	if (d->flags.deblocking_filter_control_present_flag) {
		d->flags.deblocking_filter_override_enabled_flag = br_bit(&b);
		d->flags.pps_deblocking_filter_disabled_flag = br_bit(&b);
		if (!d->flags.pps_deblocking_filter_disabled_flag) {
			const int32_t beta = br_se(&b), tc = br_se(&b);
			if (beta < -6 || beta > 6 || tc < -6 || tc > 6) BAD("PPS deblocking offsets");
			d->pps_beta_offset_div2 = (int8_t)beta; d->pps_tc_offset_div2 = (int8_t)tc;
		}
	}
	d->flags.pps_scaling_list_data_present_flag = br_bit(&b);
	if (d->flags.pps_scaling_list_data_present_flag && scaling_list_data(&b, &t->scaling)) BAD("PPS scaling_list_data");
	d->flags.lists_modification_present_flag = br_bit(&b);
	d->log2_parallel_merge_level_minus2 = (uint8_t)br_ue(&b);
	d->flags.slice_segment_header_extension_present_flag = br_bit(&b);
	d->flags.pps_extension_present_flag = br_bit(&b);
	if (d->flags.pps_extension_present_flag) {
		d->flags.pps_range_extension_flag = br_bit(&b);
		br_bit(&b); br_bit(&b);   // pps_multilayer_extension_flag, pps_3d_extension_flag
		if (d->flags.pps_range_extension_flag || br_bit(&b)) t->unsupported = 1;   // the range or SCC extension
		br_u(&b, 4);              // pps_extension_4bits
	}
	// absent syntax with non-zero inferred values (libavcodec's defaults)
	d->pps_act_y_qp_offset_plus5 = 5; d->pps_act_cb_qp_offset_plus5 = 5; d->pps_act_cr_qp_offset_plus3 = 3;
	if (b.err) BAD("PPS truncated");
	d->pScalingLists = d->flags.pps_scaling_list_data_present_flag ? &t->scaling : NULL;
	d->pPredictorPaletteEntries = NULL;
#undef BAD
	if (!(t->raw = malloc(n))) { free(t); return fail(s, HEVCD_ERROR, "no memory"); }
	memcpy(t->raw, nal, n); t->rawn = n;
	if (s->pps[id]) retire(s, s->pps[id], s->pps[id]->raw);
	s->pps[id] = t;
	return HEVCD_NO_PICTURE;
}

// ---------------------------------------------------------------------------------------------------------------------
// Slice segment headers (7.3.6.1)

typedef struct {
	int first, dependent, pps_id, slice_type;
	int poc_lsb, st_sps_flag, st_idx, st_bits, ref_num_delta;
	int nlt; int32_t lt_poc[32]; int lt_used[32], lt_msb[32];
	int hdr_bits;
} Slice;

static int is_irap(int t) { return t >= BLA_W_LP && t <= 23; }
static int is_idr(int t) { return t == IDR_W_RADL || t == IDR_N_LP; }
static int is_bla(int t) { return t >= BLA_W_LP && t <= BLA_N_LP; }

// libavcodec's ff_hevc_compute_poc2
static int32_t compute_poc(int log2_max_lsb, int32_t poc_tid0, int lsb, int nal_type) {
	const int32_t max = 1 << log2_max_lsb, prev_lsb = poc_tid0 % max, prev_msb = poc_tid0 - prev_lsb;
	int32_t msb;
	if (lsb < prev_lsb && prev_lsb - lsb >= max / 2) msb = prev_msb + max;
	else if (lsb > prev_lsb && lsb - prev_lsb > max / 2) msb = prev_msb - max;
	else msb = prev_msb;
	if (is_bla(nal_type)) msb = 0;
	return msb + lsb;
}

// pred_weight_table() (7.3.6.3): only walked (check mode)
static void pred_weight_table(BR *b, int chroma, int B, const int active[2]) {
	br_ue(b);                    // luma_log2_weight_denom
	if (chroma) br_se(b);        // delta_chroma_log2_weight_denom
	for (int l = 0; l < (B ? 2 : 1); l++) {
		int lw[16] = { 0 }, cw[16] = { 0 };
		for (int i = 0; i < active[l]; i++) lw[i] = (int)br_bit(b);
		if (chroma) for (int i = 0; i < active[l]; i++) cw[i] = (int)br_bit(b);
		for (int i = 0; i < active[l]; i++) {
			if (lw[i]) { br_se(b); br_se(b); }
			if (cw[i]) for (int j = 0; j < 2; j++) { br_se(b); br_se(b); }
		}
	}
}

// the header of a slice segment. full: the first slice segment of a picture (the RPS); check: walked to its end
static int parse_slice(HevcDState *s, const uint8_t *nal, size_t n, int nal_type, int full, int check, int32_t *poc_out, Slice *sl) {
	BR b; br_init(&b, nal, n);
	br_u(&b, 16);   // nal_unit_header()
	memset(sl, 0, sizeof *sl);
	sl->first = (int)br_bit(&b);
	if (is_irap(nal_type)) br_bit(&b);   // no_output_of_prior_pics_flag
	const uint32_t pps_id = br_ue(&b);
	if (pps_id > 63 || b.err || !s->pps[pps_id]) return fail(s, HEVCD_ERROR, "slice without its PPS");
	sl->pps_id = (int)pps_id;
	const HevcDPps *pps = s->pps[pps_id];
	const HevcDSps *sps = s->sps[pps->sps_id];
	if (!sps) return fail(s, HEVCD_ERROR, "slice without its SPS");
	const StdVideoH265PictureParameterSet *pd = &pps->std;
	const StdVideoH265SequenceParameterSet *d = &sps->std;
	if (!sl->first) {
		if (pd->flags.dependent_slice_segments_enabled_flag) sl->dependent = (int)br_bit(&b);
		const uint32_t addr = br_u(&b, ceil_log2((unsigned)(sps->ctb_w * sps->ctb_h)));
		if (addr >= (uint32_t)(sps->ctb_w * sps->ctb_h)) return fail(s, HEVCD_ERROR, "slice_segment_address");
	}
	if (!full && !check) return b.err ? fail(s, HEVCD_ERROR, "slice header truncated") : 0;
	int sao_luma = 0, sao_chroma = 0, tmvp = 0;
	int P = 0, B = 0;
	if (!sl->dependent) {
		for (int i = 0; i < pd->num_extra_slice_header_bits; i++) br_bit(&b);
		const uint32_t type = br_ue(&b);
		if (type > 2) return fail(s, HEVCD_ERROR, "slice_type");
		sl->slice_type = (int)type; B = type == 0; P = type == 1;
		if (pd->flags.output_flag_present_flag) br_bit(&b);    // pic_output_flag
		if (d->flags.separate_colour_plane_flag) br_u(&b, 2);   // colour_plane_id
		if (!is_idr(nal_type)) {
			sl->poc_lsb = (int)br_u(&b, sps->log2_max_poc_lsb);
			if (full) *poc_out = compute_poc(sps->log2_max_poc_lsb, s->poc_tid0, sl->poc_lsb, nal_type);
			sl->st_sps_flag = (int)br_bit(&b);
			const long p0 = b.pos;
			if (!sl->st_sps_flag) {
				// the set in the header: entry num_short_term_ref_pic_sets of the SPS's derived sets (after its own)
				if (st_ref_pic_set(&b, d->num_short_term_ref_pic_sets, d->num_short_term_ref_pic_sets, (HevcDRps *)sps->rps, NULL, &sl->ref_num_delta))
					return fail(s, HEVCD_ERROR, "slice st_ref_pic_set");
				sl->st_bits = (int)(b.pos - p0);
				sl->st_idx = d->num_short_term_ref_pic_sets;
			} else {
				if (!d->num_short_term_ref_pic_sets) return fail(s, HEVCD_ERROR, "no short-term RPS in the SPS");
				if (d->num_short_term_ref_pic_sets > 1) sl->st_idx = (int)br_u(&b, ceil_log2(d->num_short_term_ref_pic_sets));
				if (sl->st_idx >= d->num_short_term_ref_pic_sets) return fail(s, HEVCD_ERROR, "short_term_ref_pic_set_idx");
			}
			if (d->flags.long_term_ref_pics_present_flag) {
				uint32_t nsps = 0;
				if (d->num_long_term_ref_pics_sps) nsps = br_ue(&b);
				const uint32_t npics = br_ue(&b);
				if (nsps > d->num_long_term_ref_pics_sps || nsps + npics > 32) return fail(s, HEVCD_ERROR, "long-term pictures");
				sl->nlt = (int)(nsps + npics);
				const int32_t max = 1 << sps->log2_max_poc_lsb;
				int32_t prev_msb = 0;
				for (int i = 0; i < sl->nlt; i++) {
					int32_t lsb; int used;
					if (i < (int)nsps) {
						const uint32_t k = d->num_long_term_ref_pics_sps > 1 ? br_u(&b, ceil_log2(d->num_long_term_ref_pics_sps)) : 0;
						if (k >= d->num_long_term_ref_pics_sps) return fail(s, HEVCD_ERROR, "lt_idx_sps");
						lsb = (int32_t)sps->lt.lt_ref_pic_poc_lsb_sps[k]; used = (int)(sps->lt.used_by_curr_pic_lt_sps_flag >> k & 1);
					} else {
						lsb = (int32_t)br_u(&b, sps->log2_max_poc_lsb); used = (int)br_bit(&b);
					}
					sl->lt_used[i] = used;
					sl->lt_msb[i] = (int)br_bit(&b);   // delta_poc_msb_present_flag
					sl->lt_poc[i] = lsb;
					if (sl->lt_msb[i]) {
						int64_t delta = br_ue(&b);
						if (i && i != (int)nsps) delta += prev_msb;
						if (full) {
							const int64_t poc = (int64_t)lsb + *poc_out - delta * max - sl->poc_lsb;
							if (poc != (int32_t)poc) return fail(s, HEVCD_ERROR, "long-term POC");
							sl->lt_poc[i] = (int32_t)poc;
						}
						prev_msb = (int32_t)delta;
					}
				}
			}
			if (d->flags.sps_temporal_mvp_enabled_flag) tmvp = (int)br_bit(&b);
		} else if (full) *poc_out = 0;
		if (b.err) return fail(s, HEVCD_ERROR, "slice header truncated");
		if (!check) return 0;
		if (d->flags.sample_adaptive_offset_enabled_flag) { sao_luma = (int)br_bit(&b); if (d->chroma_format_idc) sao_chroma = (int)br_bit(&b); }
		if (P || B) {
			int active[2] = { pd->num_ref_idx_l0_default_active_minus1 + 1, B ? pd->num_ref_idx_l1_default_active_minus1 + 1 : 0 };
			if (br_bit(&b)) { active[0] = (int)br_ue(&b) + 1; if (B) active[1] = (int)br_ue(&b) + 1; }
			if (active[0] > 15 || active[1] > 15) return fail(s, HEVCD_ERROR, "num_ref_idx_active");
			// NumPicTotalCurr
			const HevcDRps *r = &sps->rps[sl->st_sps_flag ? sl->st_idx : d->num_short_term_ref_pic_sets];
			int total = 0;
			for (int i = 0; i < r->num_negative + r->num_positive; i++) total += (int)(r->used >> i & 1);
			for (int i = 0; i < sl->nlt; i++) total += sl->lt_used[i];
			if (pd->flags.lists_modification_present_flag && total > 1) {
				const int bits = ceil_log2((unsigned)total);
				for (int l = 0; l < (B ? 2 : 1); l++) if (br_bit(&b)) for (int i = 0; i < active[l]; i++) br_u(&b, bits);
			}
			if (B) br_bit(&b);                                       // mvd_l1_zero_flag
			if (pd->flags.cabac_init_present_flag) br_bit(&b);       // cabac_init_flag
			if (tmvp) {
				int from_l0 = 1;
				if (B) from_l0 = (int)br_bit(&b);
				if ((from_l0 && active[0] > 1) || (!from_l0 && active[1] > 1)) br_ue(&b);   // collocated_ref_idx
			}
			if ((pd->flags.weighted_pred_flag && P) || (pd->flags.weighted_bipred_flag && B)) pred_weight_table(&b, d->chroma_format_idc != 0, B, active);
			br_ue(&b);                                               // five_minus_max_num_merge_cand
		}
		br_se(&b);                                                   // slice_qp_delta
		if (pd->flags.pps_slice_chroma_qp_offsets_present_flag) { br_se(&b); br_se(&b); }
		if (pd->flags.chroma_qp_offset_list_enabled_flag) br_bit(&b);
		int override = 0, dbk_disabled = (int)pd->flags.pps_deblocking_filter_disabled_flag;
		if (pd->flags.deblocking_filter_override_enabled_flag) override = (int)br_bit(&b);
		if (override) { dbk_disabled = (int)br_bit(&b); if (!dbk_disabled) { br_se(&b); br_se(&b); } }
		if (pd->flags.pps_loop_filter_across_slices_enabled_flag && (sao_luma || sao_chroma || !dbk_disabled)) br_bit(&b);
	}
	if (pd->flags.tiles_enabled_flag || pd->flags.entropy_coding_sync_enabled_flag) {
		const uint32_t entries = br_ue(&b);
		if (entries > (uint32_t)(sps->ctb_w * sps->ctb_h)) return fail(s, HEVCD_ERROR, "num_entry_point_offsets");
		if (entries) {
			const uint32_t len = br_ue(&b) + 1;
			if (len > 32) return fail(s, HEVCD_ERROR, "offset_len_minus1");
			for (uint32_t i = 0; i < entries; i++) br_u(&b, (int)len);
		}
	}
	if (pd->flags.slice_segment_header_extension_present_flag) {
		const uint32_t len = br_ue(&b);
		if (len > 256) return fail(s, HEVCD_ERROR, "slice_segment_header_extension_length");
		for (uint32_t i = 0; i < len; i++) br_u(&b, 8);
	}
	if (!br_bit(&b)) return fail(s, HEVCD_ERROR, "slice header: no alignment_bit_equal_to_one");
	while (b.left) if (br_bit(&b)) return fail(s, HEVCD_ERROR, "slice header: alignment bit not zero");
	if (b.err) return fail(s, HEVCD_ERROR, "slice header truncated");
	sl->hdr_bits = (int)b.pos;
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------

void hevcd_init(HevcDState *s, int nal_length_size) {
	memset(s, 0, sizeof *s);
	s->nal_length_size = nal_length_size;
	s->active_sps = -1; s->last_vps = s->last_sps = s->last_pps = -1;
	s->eos = 1;   // libavcodec starts as after an end of sequence: a first CRA's RASL pictures are not decoded
}

void hevcd_free(HevcDState *s) {
	free_retired(s);
	for (int i = 0; i < 16; i++) { if (s->vps[i]) free(s->vps[i]->raw); free(s->vps[i]); s->vps[i] = NULL; if (s->sps[i]) free(s->sps[i]->raw); free(s->sps[i]); s->sps[i] = NULL; }
	for (int i = 0; i < 64; i++) { if (s->pps[i]) free(s->pps[i]->raw); free(s->pps[i]); s->pps[i] = NULL; }
	free(s->offsets); s->offsets = NULL; s->offsets_cap = 0;
}

static void drop_refs(HevcDState *s) { memset(s->slot_busy, 0, sizeof s->slot_busy); }

void hevcd_flush(HevcDState *s) {
	drop_refs(s);
	s->eos = 1;
}

static int nal_type_of(const uint8_t *nal) { return nal[0] >> 1 & 0x3f; }
static int layer_of(const uint8_t *nal) { return (nal[0] & 1) << 5 | nal[1] >> 3; }
static int is_vcl(int t) { return (t >= 0 && t <= 9) || (t >= 16 && t <= 21); }

// the sample's NAL units, each behind a nal_length_size-byte big-endian length: 1 and the next one, 0 at the end, -1 if
// the lengths do not add up or a NAL unit has no header (empty NAL units are skipped)
typedef struct { const uint8_t *p; size_t n, o; int ls; } NalIt;
static int nal_next(NalIt *it, const uint8_t **nal, size_t *len) {
	while (it->o < it->n) {
		if (it->n - it->o < (size_t)it->ls) return -1;
		size_t l = 0;
		for (int i = 0; i < it->ls; i++) l = l << 8 | it->p[it->o + i];
		it->o += (size_t)it->ls;
		if (l > it->n - it->o) return -1;
		*nal = it->p + it->o; *len = l; it->o += l;
		if (l == 1) return -1;
		if (l) return 1;
	}
	return 0;
}

size_t hevcd_bitstream_size(const HevcDState *s, const uint8_t *data, size_t n) {
	size_t total = 0, len; const uint8_t *nal; int r;
	NalIt it = { data, n, 0, s->nal_length_size };
	while ((r = nal_next(&it, &nal, &len)) > 0) if (is_vcl(nal_type_of(nal)) && !layer_of(nal)) total += 3 + len;
	return r < 0 ? 0 : total;
}

static int parse_param(HevcDState *s, const uint8_t *nal, size_t n) {
	const int t = nal_type_of(nal);
	if (layer_of(nal)) return HEVCD_NO_PICTURE;   // other layers: not decoded (libavcodec decodes the base layer)
	if (n < 3) return fail(s, HEVCD_ERROR, "parameter set truncated");
	return t == VPS_NUT ? parse_vps(s, nal, n) : t == SPS_NUT ? parse_sps(s, nal, n) : t == PPS_NUT ? parse_pps(s, nal, n) : HEVCD_NO_PICTURE;
}

int hevcd_parse_hvcc(HevcDState *s, const uint8_t *c, size_t n) {
	// HEVCDecoderConfigurationRecord: 22 bytes of profile / format fields (lengthSizeMinusOne in the low bits of byte
	// 21), numOfArrays, then per array: type byte, 16-bit numNalus, (16-bit length, NAL unit)...
	if (n < 23 || c[0] != 1) return fail(s, HEVCD_ERROR, "hvcC");
	s->nal_length_size = (c[21] & 3) + 1;
	if (s->nal_length_size == 3) return fail(s, HEVCD_ERROR, "hvcC lengthSizeMinusOne");
	size_t o = 23;
	for (int a = 0; a < c[22]; a++) {
		if (o + 3 > n) return fail(s, HEVCD_ERROR, "hvcC truncated");
		const int count = c[o + 1] << 8 | c[o + 2];
		o += 3;
		for (int i = 0; i < count; i++) {
			if (o + 2 > n) return fail(s, HEVCD_ERROR, "hvcC truncated");
			const size_t len = (size_t)c[o] << 8 | c[o + 1];
			o += 2;
			if (len < 2 || o + len > n) return fail(s, HEVCD_ERROR, "hvcC truncated");
			const int r = parse_param(s, c + o, len);
			if (r < 0) return r;
			o += len;
		}
	}
	return HEVCD_NO_PICTURE;
}

// 8.3.2 as libavcodec's ff_hevc_frame_rps runs it: the references the RPS names are kept (short-term by POC, long-term
// by POC or its LSBs), every other one is dropped. 0, or 1 when an entry of the current lists is missing (non-IRAP).
static int apply_rps(HevcDState *s, const HevcDSps *sps, const Slice *sl, HevcDPicture *pic) {
	int keep[HEVCD_SLOTS] = { 0 }, keep_long[HEVCD_SLOTS] = { 0 }, stopped = 0;
	const HevcDRps *r = pic->idr ? NULL : &sps->rps[sl->st_sps_flag ? sl->st_idx : sps->std.num_short_term_ref_pic_sets];
	const int nst = r ? r->num_negative + r->num_positive : 0, nlt = r ? sl->nlt : 0;
	const int32_t mask = (1 << sps->log2_max_poc_lsb) - 1;
	for (int e = 0; e < nst + nlt; e++) {
		const int lt = e >= nst, i = lt ? e - nst : e;
		const int used = lt ? sl->lt_used[i] : (int)(r->used >> i & 1);
		const int msb = lt ? sl->lt_msb[i] : 1;
		const int32_t poc = lt ? sl->lt_poc[i] : pic->poc + r->delta[i];
		int slot = -1;
		for (int k = 0; k < HEVCD_SLOTS && slot < 0; k++)
			if (s->slot_busy[k] && (msb ? s->slot_poc[k] == poc : ((s->slot_poc[k] & mask) == poc && s->slot_poc[k] != pic->poc))) slot = k;
		const int list = !used ? -1 : lt ? 2 : i < r->num_negative ? 0 : 1;
		if (list >= 0 && pic->nlist[list] < 8) {
			pic->list[list][pic->nlist[list]] = slot >= 0 ? s->slot_poc[slot] : poc;
			pic->found[list][pic->nlist[list]] = (int8_t)(slot >= 0);
			if (slot >= 0) {   // the Std lists hold the slots of the pictures found (libavcodec's: those it has)
				uint8_t *dst = list == 0 ? pic->std.RefPicSetStCurrBefore : list == 1 ? pic->std.RefPicSetStCurrAfter : pic->std.RefPicSetLtCurr;
				dst[pic->nlist[list]] = (uint8_t)slot;
			}
			pic->nlist[list]++;
		}
		// a missing entry of the current lists of a non-IRAP picture: libavcodec stops there (the picture is not
		// decoded; the references it had not reached yet are dropped)
		if (slot < 0 && list >= 0 && !pic->irap) stopped = 1;
		if (slot >= 0 && !stopped) { keep[slot] = 1; keep_long[slot] = lt; }
	}
	for (int k = 0; k < HEVCD_SLOTS; k++) {
		if (!keep[k]) s->slot_busy[k] = 0;
		else s->slot_long[k] = keep_long[k];
	}
	return stopped;
}

int hevcd_parse_sample(HevcDState *s, const uint8_t *data, size_t n, uint8_t *out, size_t cap, HevcDPicture *pic) {
	memset(pic, 0, sizeof *pic);
	free_retired(s);
	s->skipped = 0;
	int have = 0, r = 0, more;
	Slice first = { 0 };
	size_t w = 0, len;
	uint32_t nsl = 0;
	const uint8_t *nal;
	// libavcodec's end of sequence: an EOS / EOB NAL unit first in the access unit applies to it, later ones to the next
	s->last_eos = s->eos; s->eos = 0;
	{
		NalIt it = { data, n, 0, s->nal_length_size };
		int at_start = 1;
		while ((more = nal_next(&it, &nal, &len)) > 0) {
			const int t = nal_type_of(nal);
			if (t == EOS_NUT || t == EOB_NUT) { if (at_start) s->last_eos = 1; else s->eos = 1; }
			else at_start = 0;
		}
		if (more < 0) return fail(s, HEVCD_ERROR, "NAL unit lengths");
	}
	int32_t poc = 0;
	int skip = 0;
	const HevcDPps *pps = NULL; const HevcDSps *sps = NULL;   // the first slice segment's (kept alive if replaced later)
	NalIt it = { data, n, 0, s->nal_length_size };
	while ((more = nal_next(&it, &nal, &len)) > 0) {
		const int t = nal_type_of(nal);
		if (t == VPS_NUT || t == SPS_NUT || t == PPS_NUT) {
			// a parameter set that does not parse (a PPS before its SPS, ...) is skipped, as libavcodec skips it
			if (parse_param(s, nal, len) < 0) s->bad_param_sets++;
			continue;
		}
		if (!is_vcl(t) || layer_of(nal)) continue;   // SEI, AUD, EOS, filler, reserved types, other layers
		if (len < 3) return fail(s, HEVCD_ERROR, "slice segment truncated");
		const int first_in_pic = nal[2] >> 7;
		if (!have) {
			if (!first_in_pic) return fail(s, HEVCD_ERROR, "the first slice segment of the picture is missing");
			Slice sl;
			if ((r = parse_slice(s, nal, len, t, 1, s->check, &poc, &sl)) < 0) return r;
			pps = s->pps[sl.pps_id]; sps = s->sps[pps->sps_id];
			if (sps->unsupported) {
				static const char *why[] = { "", "profile not Main/Main 10/Main Still Picture", "not 4:2:0", "not 8 or 10 bits",
				                             "SPS extensions" };
				return fail(s, HEVCD_UNSUPPORTED, why[sps->unsupported]);
			}
			if (pps->unsupported) return fail(s, HEVCD_UNSUPPORTED, "PPS extensions");
			first = sl; have = 1;
			pic->nal_type = t; pic->tid = (nal[1] & 7) - 1; pic->irap = is_irap(t); pic->idr = is_idr(t);
			// RASL pictures of an IRAP picture with NoRaslOutputFlag: libavcodec skips them before anything else
			if ((t == RASL_N || t == RASL_R) && s->no_rasl_output_flag) skip = 1;
		} else if (first_in_pic) {
			return fail(s, HEVCD_ERROR, "two pictures in one sample");
		} else if (!skip) {
			Slice sl;
			if ((r = parse_slice(s, nal, len, t, 0, 0, &poc, &sl)) < 0) return r;
			if (sl.pps_id != first.pps_id) return fail(s, HEVCD_ERROR, "PPS changed between slice segments");
		}
		if (skip) continue;
		if (w + 3 + len > cap) return fail(s, HEVCD_ERROR, "bitstream buffer too small");
		if (nsl == s->offsets_cap) {
			const uint32_t nc = s->offsets_cap ? s->offsets_cap * 2 : 64;
			uint32_t *no = realloc(s->offsets, nc * sizeof *no);
			if (!no) return fail(s, HEVCD_ERROR, "no memory");
			s->offsets = no; s->offsets_cap = nc;
		}
		s->offsets[nsl++] = (uint32_t)w;
		out[w] = 0; out[w + 1] = 0; out[w + 2] = 1;
		memcpy(out + w + 3, nal, len);
		w += 3 + len;
	}
	if (more < 0) return fail(s, HEVCD_ERROR, "NAL unit lengths");
	if (!have) return HEVCD_NO_PICTURE;
	pic->poc = poc;
	if (skip) { s->skipped = 1; s->rasl_skipped++; s->why[0] = 0; return HEVCD_NO_PICTURE; }

	if (!s->pictures && !pic->irap) return fail(s, HEVCD_UNSUPPORTED, "the stream does not start with an IRAP picture");
	if (s->active_sps >= 0 && (s->active_w != sps->width || s->active_h != sps->height || s->active_bd != sps->bit_depth))
		return fail(s, HEVCD_UNSUPPORTED, "the coded size or bit depth changed");
	// libavcodec's hevc_frame_start: another SPS (or one re-sent with other content) or an IDR picture drops every
	// reference; NoRaslOutputFlag at IRAP pictures; the POC of the previous TemporalId-0 picture
	if (pps->sps_id != s->active_sps || sps->gen != s->active_gen || pic->idr) drop_refs(s);
	s->active_sps = pps->sps_id; s->active_gen = sps->gen;
	s->active_w = sps->width; s->active_h = sps->height; s->active_bd = sps->bit_depth;
	if (pic->irap) s->no_rasl_output_flag = pic->idr || is_bla(pic->nal_type) || (pic->nal_type == CRA_NUT && s->last_eos);
	const int t = pic->nal_type;
	if (pic->tid == 0 && t != TRAIL_N && t != TSA_N && t != STSA_N && t != RADL_N && t != RADL_R && t != RASL_N && t != RASL_R)
		s->poc_tid0 = poc;
	memset(pic->std.RefPicSetStCurrBefore, STD_VIDEO_H265_NO_REFERENCE_PICTURE, 8);
	memset(pic->std.RefPicSetStCurrAfter, STD_VIDEO_H265_NO_REFERENCE_PICTURE, 8);
	memset(pic->std.RefPicSetLtCurr, STD_VIDEO_H265_NO_REFERENCE_PICTURE, 8);
	pic->missing = apply_rps(s, sps, &first, pic);
	if (pic->missing) s->missing_refs++;

	pic->vps = s->vps[sps->std.sps_video_parameter_set_id];
	pic->sps = sps; pic->pps = pps;
	pic->nslices = nsl; pic->slice_offsets = s->offsets; pic->size = w;
	pic->st_mode = pic->idr ? 0 : first.st_sps_flag ? 1 : 2; pic->st_idx = first.st_idx;
	pic->hdr_bits = first.hdr_bits;
	pic->std.flags.IrapPicFlag = (uint32_t)pic->irap;
	pic->std.flags.IdrPicFlag = (uint32_t)pic->idr;
	pic->std.flags.IsReference = t < 16 ? (uint32_t)(t & 1) : 1;   // libavcodec's
	pic->std.flags.short_term_ref_pic_set_sps_flag = (uint32_t)first.st_sps_flag;
	pic->std.sps_video_parameter_set_id = sps->std.sps_video_parameter_set_id;
	pic->std.pps_seq_parameter_set_id = (uint8_t)pps->sps_id;
	pic->std.pps_pic_parameter_set_id = (uint8_t)first.pps_id;
	pic->std.NumDeltaPocsOfRefRpsIdx = (uint8_t)(pic->st_mode == 2 ? first.ref_num_delta : 0);
	pic->std.PicOrderCntVal = poc;
	pic->std.NumBitsForSTRefPicSetInSlice = (uint16_t)(pic->st_mode == 2 ? first.st_bits : 0);
	// the references: every slot the RPS kept, in slot order; the current picture in the first free slot
	for (int k = 0; k < HEVCD_SLOTS; k++)
		if (s->slot_busy[k] && pic->nrefs < HEVCD_MAX_REFS) pic->refs[pic->nrefs++] = (HevcDRef){ k, s->slot_poc[k], s->slot_long[k] };
	pic->slot = -1;
	for (int k = 0; k < HEVCD_SLOTS; k++) if (!s->slot_busy[k]) { pic->slot = k; break; }
	if (pic->slot < 0) return fail(s, HEVCD_ERROR, "no free DPB slot");
	s->pictures++;
	s->why[0] = 0;
	return HEVCD_PICTURE;
}

void hevcd_finish_picture(HevcDState *s, const HevcDPicture *pic) {
	if (pic->missing || pic->slot < 0) return;   // not decoded: libavcodec drops it
	s->slot_busy[pic->slot] = 1; s->slot_poc[pic->slot] = pic->poc; s->slot_long[pic->slot] = 0;
}
