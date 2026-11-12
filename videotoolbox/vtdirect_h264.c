// SPDX-License-Identifier: MIT
// vtdirect_h264.c: H.264 parameter sets, slice headers, picture order counts and reference marking for the direct
// Vulkan Video decode path (see vtdirect_h264.h). Written from ITU-T H.264 (08/2021) 7.3 / 7.4 / 8.2.1 / 8.2.5, with
// libavcodec 9.0.2's choices where the hardware must see the same values as on the libavcodec path.
#include "vtdirect_h264.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// Bit reader over a NAL unit's payload: emulation prevention bytes (00 00 03) are skipped as they are read

typedef struct { const uint8_t *p, *end; int zeros, left, err; uint8_t cur; } BR;

static void br_init(BR *b, const uint8_t *p, size_t n) { *b = (BR){ p, p + n, 0, 0, 0, 0 }; }
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
	return (b->cur >> --b->left) & 1;
}
static uint32_t br_u(BR *b, int n) { uint32_t v = 0; while (n-- > 0) v = v << 1 | br_bit(b); return v; }
static uint32_t br_ue(BR *b) {
	int lz = 0;
	while (!br_bit(b)) { if (++lz > 31 || b->err) { b->err = 1; return 0; } }
	return lz ? ((1u << lz) - 1 + br_u(b, lz)) : 0;
}
static int32_t br_se(BR *b) { const uint32_t k = br_ue(b); return (k & 1) ? (int32_t)((k + 1) / 2) : -(int32_t)(k / 2); }
// more_rbsp_data(): anything before the rbsp_stop_one_bit. Reads ahead without consuming.
static int br_more_rbsp_data(const BR *b) {
	BR t = *b;
	// the remaining payload bits: the stop bit is the last 1; trailing zero bytes (cabac_zero_words) are padding
	int ones = 0;
	for (;;) {
		const unsigned bit = br_bit(&t);
		if (t.err) break;
		ones += bit;
		if (ones > 1) return 1;
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------

static int fail(H264DState *s, int code, const char *why) { snprintf(s->why, sizeof s->why, "%s", why); return code; }
const char *h264d_why(const H264DState *s) { return s->why; }

int h264d_level_idc_to_std(int l) {
	static const struct { int idc; StdVideoH264LevelIdc std; } t[] = {
		{ 10, STD_VIDEO_H264_LEVEL_IDC_1_0 }, { 11, STD_VIDEO_H264_LEVEL_IDC_1_1 }, { 12, STD_VIDEO_H264_LEVEL_IDC_1_2 },
		{ 13, STD_VIDEO_H264_LEVEL_IDC_1_3 }, { 20, STD_VIDEO_H264_LEVEL_IDC_2_0 }, { 21, STD_VIDEO_H264_LEVEL_IDC_2_1 },
		{ 22, STD_VIDEO_H264_LEVEL_IDC_2_2 }, { 30, STD_VIDEO_H264_LEVEL_IDC_3_0 }, { 31, STD_VIDEO_H264_LEVEL_IDC_3_1 },
		{ 32, STD_VIDEO_H264_LEVEL_IDC_3_2 }, { 40, STD_VIDEO_H264_LEVEL_IDC_4_0 }, { 41, STD_VIDEO_H264_LEVEL_IDC_4_1 },
		{ 42, STD_VIDEO_H264_LEVEL_IDC_4_2 }, { 50, STD_VIDEO_H264_LEVEL_IDC_5_0 }, { 51, STD_VIDEO_H264_LEVEL_IDC_5_1 },
		{ 52, STD_VIDEO_H264_LEVEL_IDC_5_2 }, { 60, STD_VIDEO_H264_LEVEL_IDC_6_0 }, { 61, STD_VIDEO_H264_LEVEL_IDC_6_1 },
	};
	for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (t[i].idc == l) return t[i].std;
	return STD_VIDEO_H264_LEVEL_IDC_6_2;   // as libavcodec's convert_to_vk_level_idc
}

// scaling_list() (7.3.2.1.1.1): the values in the bitstream's (zig-zag) order, as StdVideoH264ScalingLists holds them;
// useDefaultScalingMatrixFlag goes to use_default_scaling_matrix_mask (the driver fills the default list)
static void scaling_list(BR *b, uint8_t *list, int size, int idx, StdVideoH264ScalingLists *sl) {
	int last = 8, next = 8;
	for (int j = 0; j < size; j++) {
		if (next) {
			next = (last + br_se(b) + 256) % 256;
			if (j == 0 && next == 0) { sl->use_default_scaling_matrix_mask |= (uint16_t)(1u << idx); }
		}
		list[j] = (uint8_t)(next ? next : last);
		last = list[j];
	}
}
static void scaling_lists(BR *b, StdVideoH264ScalingLists *sl, int n8x8) {
	for (int i = 0; i < 6 + n8x8; i++) {
		if (!br_bit(b)) continue;
		sl->scaling_list_present_mask |= (uint16_t)(1u << i);
		if (i < 6) scaling_list(b, sl->ScalingList4x4[i], 16, i, sl);
		else scaling_list(b, sl->ScalingList8x8[i - 6], 64, i, sl);
	}
}

// seq_parameter_set_rbsp() up to vui_parameters_present_flag (the VUI is not needed for decoding)
static int parse_sps(H264DState *s, const uint8_t *nal, size_t n) {
	BR b; br_init(&b, nal + 1, n - 1);
	H264DSps t; memset(&t, 0, sizeof t);
	StdVideoH264SequenceParameterSet *d = &t.std;
	t.profile_idc = (int)br_u(&b, 8);
	const uint32_t cs = br_u(&b, 8);
	t.level_idc = (int)br_u(&b, 8);
	const uint32_t id = br_ue(&b);
	if (id > 31 || b.err) return fail(s, H264D_ERROR, "SPS id");
	d->flags.constraint_set0_flag = cs >> 7 & 1; d->flags.constraint_set1_flag = cs >> 6 & 1;
	d->flags.constraint_set2_flag = cs >> 5 & 1; d->flags.constraint_set3_flag = cs >> 4 & 1;
	d->flags.constraint_set4_flag = cs >> 3 & 1; d->flags.constraint_set5_flag = cs >> 2 & 1;
	d->profile_idc = (StdVideoH264ProfileIdc)t.profile_idc;
	d->level_idc = (StdVideoH264LevelIdc)h264d_level_idc_to_std(t.level_idc);
	d->seq_parameter_set_id = (uint8_t)id;
	uint32_t chroma = 1, depth_l = 0, depth_c = 0;
	const int p = t.profile_idc;
	if (p == 100 || p == 110 || p == 122 || p == 244 || p == 44 || p == 83 || p == 86 || p == 118 || p == 128 || p == 138 ||
	    p == 139 || p == 134 || p == 135) {
		chroma = br_ue(&b);
		if (chroma == 3) d->flags.separate_colour_plane_flag = br_bit(&b);
		depth_l = br_ue(&b); depth_c = br_ue(&b);
		d->flags.qpprime_y_zero_transform_bypass_flag = br_bit(&b);
		d->flags.seq_scaling_matrix_present_flag = br_bit(&b);
		if (d->flags.seq_scaling_matrix_present_flag) scaling_lists(&b, &t.scaling, chroma != 3 ? 2 : 6);
	}
	if (chroma > 3 || depth_l > 6 || depth_c > 6) return fail(s, H264D_ERROR, "SPS chroma format / bit depth");
	d->chroma_format_idc = (StdVideoH264ChromaFormatIdc)chroma;
	d->bit_depth_luma_minus8 = (uint8_t)depth_l; d->bit_depth_chroma_minus8 = (uint8_t)depth_c;
	const uint32_t lfn = br_ue(&b);
	if (lfn > 12) return fail(s, H264D_ERROR, "SPS log2_max_frame_num");
	d->log2_max_frame_num_minus4 = (uint8_t)lfn;
	t.log2_max_frame_num = (int)lfn + 4;
	const uint32_t poc_type = br_ue(&b);
	if (poc_type > 2) return fail(s, H264D_ERROR, "SPS pic_order_cnt_type");
	d->pic_order_cnt_type = (StdVideoH264PocType)poc_type;
	if (poc_type == 0) {
		const uint32_t l = br_ue(&b);
		if (l > 12) return fail(s, H264D_ERROR, "SPS log2_max_pic_order_cnt_lsb");
		d->log2_max_pic_order_cnt_lsb_minus4 = (uint8_t)l;
		t.log2_max_poc_lsb = (int)l + 4;
	} else if (poc_type == 1) {
		d->flags.delta_pic_order_always_zero_flag = br_bit(&b);
		d->offset_for_non_ref_pic = br_se(&b);
		d->offset_for_top_to_bottom_field = br_se(&b);
		const uint32_t cyc = br_ue(&b);
		if (cyc > 255) return fail(s, H264D_ERROR, "SPS num_ref_frames_in_pic_order_cnt_cycle");
		d->num_ref_frames_in_pic_order_cnt_cycle = (uint8_t)cyc;
		for (uint32_t i = 0; i < cyc; i++) t.offset_for_ref_frame[i] = br_se(&b);
	}
	const uint32_t refs = br_ue(&b);
	if (refs > 16) return fail(s, H264D_ERROR, "SPS max_num_ref_frames");
	d->max_num_ref_frames = (uint8_t)refs;
	d->flags.gaps_in_frame_num_value_allowed_flag = br_bit(&b);
	d->pic_width_in_mbs_minus1 = br_ue(&b);
	d->pic_height_in_map_units_minus1 = br_ue(&b);
	d->flags.frame_mbs_only_flag = br_bit(&b);
	if (!d->flags.frame_mbs_only_flag) d->flags.mb_adaptive_frame_field_flag = br_bit(&b);
	d->flags.direct_8x8_inference_flag = br_bit(&b);
	d->flags.frame_cropping_flag = br_bit(&b);
	if (d->flags.frame_cropping_flag) {
		d->frame_crop_left_offset = br_ue(&b); d->frame_crop_right_offset = br_ue(&b);
		d->frame_crop_top_offset = br_ue(&b); d->frame_crop_bottom_offset = br_ue(&b);
	}
	if (b.err || d->pic_width_in_mbs_minus1 > 1023 || d->pic_height_in_map_units_minus1 > 1023) return fail(s, H264D_ERROR, "SPS truncated or too large");
	// vui_parameters(): only as far as video_full_range_flag (the pixel buffers' range); not passed on
	// (pSequenceParameterSetVui stays NULL: the decode does not use it)
	t.full_range = -1;
	if (br_bit(&b)) {
		if (br_bit(&b) && br_u(&b, 8) == 255) br_u(&b, 32);   // aspect_ratio_info_present_flag, aspect_ratio_idc, sar
		if (br_bit(&b)) br_bit(&b);                            // overscan_info_present_flag, overscan_appropriate_flag
		if (br_bit(&b)) { br_u(&b, 3); t.full_range = (int)br_bit(&b); }   // video_signal_type_present_flag, video_format
		if (b.err) t.full_range = -1;
	}
	t.width_mbs = (int)d->pic_width_in_mbs_minus1 + 1;
	t.height_mbs = (int)(d->pic_height_in_map_units_minus1 + 1) * (d->flags.frame_mbs_only_flag ? 1 : 2);
	// what the direct path decodes: frames (no fields, no MBAFF), 8-bit 4:2:0, Baseline (without FMO, checked in the
	// PPS) / Main / High, no frame_num gaps (libavcodec invents frames for them)
	if (p != 66 && p != 77 && p != 100) t.unsupported = 1;
	else if (!d->flags.frame_mbs_only_flag) t.unsupported = 2;
	else if (chroma != 1 || depth_l || depth_c) t.unsupported = 3;
	else if (d->flags.gaps_in_frame_num_value_allowed_flag) t.unsupported = 4;
	t.present = 1;
	t.std.pOffsetForRefFrame = NULL; t.std.pScalingLists = NULL; t.std.pSequenceParameterSetVui = NULL;
	s->sps[id] = t;
	// pointers into the stored copy
	H264DSps *st = &s->sps[id];
	st->std.pOffsetForRefFrame = st->offset_for_ref_frame;
	st->std.pScalingLists = &st->scaling;
	s->last_sps = (int)id;
	return H264D_NO_PICTURE;
}

static int parse_pps(H264DState *s, const uint8_t *nal, size_t n) {
	BR b; br_init(&b, nal + 1, n - 1);
	const uint32_t id = br_ue(&b), sps_id = br_ue(&b);
	if (id > 255 || sps_id > 31 || b.err) return fail(s, H264D_ERROR, "PPS ids");
	if (!s->sps[sps_id].present) return fail(s, H264D_ERROR, "PPS before its SPS");
	H264DPps t; memset(&t, 0, sizeof t);
	StdVideoH264PictureParameterSet *d = &t.std;
	t.sps_id = (int)sps_id;
	d->pic_parameter_set_id = (uint8_t)id; d->seq_parameter_set_id = (uint8_t)sps_id;
	d->flags.entropy_coding_mode_flag = br_bit(&b);
	d->flags.bottom_field_pic_order_in_frame_present_flag = br_bit(&b);
	t.num_slice_groups = (int)br_ue(&b) + 1;
	if (t.num_slice_groups > 1) {
		// slice groups (Baseline FMO): Vulkan Video has no way to pass them; the syntax is not walked
		s->pps[id] = s->pps[id] ? s->pps[id] : calloc(1, sizeof(H264DPps));
		if (!s->pps[id]) return fail(s, H264D_ERROR, "no memory");
		*s->pps[id] = t; s->pps[id]->present = 1;
		s->last_pps = (int)id;
		return H264D_NO_PICTURE;
	}
	const uint32_t l0 = br_ue(&b), l1 = br_ue(&b);
	if (l0 > 31 || l1 > 31) return fail(s, H264D_ERROR, "PPS num_ref_idx_default_active");
	d->num_ref_idx_l0_default_active_minus1 = (uint8_t)l0; d->num_ref_idx_l1_default_active_minus1 = (uint8_t)l1;
	d->flags.weighted_pred_flag = br_bit(&b);
	d->weighted_bipred_idc = (StdVideoH264WeightedBipredIdc)br_u(&b, 2);
	d->pic_init_qp_minus26 = (int8_t)br_se(&b);
	d->pic_init_qs_minus26 = (int8_t)br_se(&b);
	d->chroma_qp_index_offset = (int8_t)br_se(&b);
	d->flags.deblocking_filter_control_present_flag = br_bit(&b);
	d->flags.constrained_intra_pred_flag = br_bit(&b);
	d->flags.redundant_pic_cnt_present_flag = br_bit(&b);
	d->second_chroma_qp_index_offset = d->chroma_qp_index_offset;
	if (!b.err && br_more_rbsp_data(&b)) {
		d->flags.transform_8x8_mode_flag = br_bit(&b);
		d->flags.pic_scaling_matrix_present_flag = br_bit(&b);
		if (d->flags.pic_scaling_matrix_present_flag)
			scaling_lists(&b, &t.scaling, d->flags.transform_8x8_mode_flag ? (s->sps[sps_id].std.chroma_format_idc != 3 ? 2 : 6) : 0);
		d->second_chroma_qp_index_offset = (int8_t)br_se(&b);
	}
	if (b.err) return fail(s, H264D_ERROR, "PPS truncated");
	if (!s->pps[id] && !(s->pps[id] = calloc(1, sizeof(H264DPps)))) return fail(s, H264D_ERROR, "no memory");
	*s->pps[id] = t;
	s->pps[id]->present = 1;
	s->pps[id]->std.pScalingLists = &s->pps[id]->scaling;
	s->last_pps = (int)id;
	return H264D_NO_PICTURE;
}

// ---------------------------------------------------------------------------------------------------------------------
// Slice headers (7.3.3) as far as the picture's parameters, the reference counts and dec_ref_pic_marking()

typedef struct {
	int first_mb, slice_type, pps_id, frame_num, idr_pic_id, poc_lsb, delta_poc_bottom, delta_poc[2], redundant;
	int active[2];
	int long_term_reference_flag, adaptive, nmmco; struct { int op, a, b; } mmco[66];
} Slice;

static int skip_ref_pic_list_modification(BR *b) {
	for (int i = 0; i < 64; i++) {
		const uint32_t idc = br_ue(b);
		if (idc == 3) return 0;
		if (idc > 5 || b->err) return -1;
		br_ue(b);   // abs_diff_pic_num_minus1 / long_term_pic_num / abs_diff_view_idx_minus1
	}
	return -1;
}

static int parse_slice(H264DState *s, const uint8_t *nal, size_t n, int idr, int ref_idc, int full, Slice *sl) {
	BR b; br_init(&b, nal + 1, n - 1);
	memset(sl, 0, sizeof *sl);
	sl->first_mb = (int)br_ue(&b);
	uint32_t type = br_ue(&b);
	if (type > 9) return fail(s, H264D_ERROR, "slice_type");
	if (type > 4) type -= 5;
	sl->slice_type = (int)type;
	sl->pps_id = (int)br_ue(&b);
	if (sl->pps_id > 255 || b.err || !s->pps[sl->pps_id] || !s->pps[sl->pps_id]->present) return fail(s, H264D_ERROR, "slice without its PPS");
	const H264DPps *pps = s->pps[sl->pps_id];
	const H264DSps *sps = &s->sps[pps->sps_id];
	if (!sps->present) return fail(s, H264D_ERROR, "slice without its SPS");
	if (sps->unsupported) {
		static const char *why[] = { "", "profile not Baseline/Main/High", "field or MBAFF coding", "not 8-bit 4:2:0", "frame_num gaps allowed" };
		return fail(s, H264D_UNSUPPORTED, why[sps->unsupported]);
	}
	if (pps->num_slice_groups > 1) return fail(s, H264D_UNSUPPORTED, "slice groups (FMO)");
	if (type == 3 || type == 4) return fail(s, H264D_UNSUPPORTED, "SP/SI slices");
	sl->frame_num = (int)br_u(&b, sps->log2_max_frame_num);
	// frame_mbs_only_flag is 1 (checked above): no field_pic_flag
	if (idr) sl->idr_pic_id = (int)br_ue(&b);
	const StdVideoH264SequenceParameterSet *d = &sps->std;
	const StdVideoH264PictureParameterSet *pd = &pps->std;
	if (d->pic_order_cnt_type == 0) {
		sl->poc_lsb = (int)br_u(&b, sps->log2_max_poc_lsb);
		if (pd->flags.bottom_field_pic_order_in_frame_present_flag) sl->delta_poc_bottom = br_se(&b);
	} else if (d->pic_order_cnt_type == 1 && !d->flags.delta_pic_order_always_zero_flag) {
		sl->delta_poc[0] = br_se(&b);
		if (pd->flags.bottom_field_pic_order_in_frame_present_flag) sl->delta_poc[1] = br_se(&b);
	}
	if (pd->flags.redundant_pic_cnt_present_flag) sl->redundant = (int)br_ue(&b);
	const int P = type == 0, B = type == 1;
	if (B) br_bit(&b);   // direct_spatial_mv_pred_flag
	sl->active[0] = P || B ? pd->num_ref_idx_l0_default_active_minus1 + 1 : 0;
	sl->active[1] = B ? pd->num_ref_idx_l1_default_active_minus1 + 1 : 0;
	if (P || B) {
		if (br_bit(&b)) {   // num_ref_idx_active_override_flag
			sl->active[0] = (int)br_ue(&b) + 1;
			if (B) sl->active[1] = (int)br_ue(&b) + 1;
		}
		if (sl->active[0] > 32 || sl->active[1] > 32) return fail(s, H264D_ERROR, "num_ref_idx_active");
	}
	if (b.err) return fail(s, H264D_ERROR, "slice header truncated");
	if (!full || !ref_idc) return 0;
	// ref_pic_list_modification()
	if (P || B) {
		if (br_bit(&b) && skip_ref_pic_list_modification(&b)) return fail(s, H264D_ERROR, "ref_pic_list_modification");
		if (B && br_bit(&b) && skip_ref_pic_list_modification(&b)) return fail(s, H264D_ERROR, "ref_pic_list_modification");
	}
	// pred_weight_table()
	if ((pd->flags.weighted_pred_flag && P) || (pd->weighted_bipred_idc == 1 && B)) {
		const int chroma = d->chroma_format_idc != 0;
		br_ue(&b);                 // luma_log2_weight_denom
		if (chroma) br_ue(&b);     // chroma_log2_weight_denom
		for (int list = 0; list < (B ? 2 : 1); list++)
			for (int i = 0; i < sl->active[list]; i++) {
				if (br_bit(&b)) { br_se(&b); br_se(&b); }
				if (chroma && br_bit(&b)) { br_se(&b); br_se(&b); br_se(&b); br_se(&b); }
			}
	}
	// dec_ref_pic_marking()
	if (idr) {
		br_bit(&b);   // no_output_of_prior_pics_flag
		sl->long_term_reference_flag = (int)br_bit(&b);
	} else if ((sl->adaptive = (int)br_bit(&b))) {
		for (;;) {
			const uint32_t op = br_ue(&b);
			if (b.err || op > 6) return fail(s, H264D_ERROR, "memory_management_control_operation");
			if (op == 0) break;
			if (sl->nmmco == 66) return fail(s, H264D_ERROR, "too many memory_management_control_operations");
			int a = 0, c = 0;
			if (op == 1 || op == 3) a = (int)br_ue(&b);                       // difference_of_pic_nums_minus1
			if (op == 2) a = (int)br_ue(&b);                                  // long_term_pic_num
			if (op == 3 || op == 6) c = (int)br_ue(&b);                       // long_term_frame_idx
			if (op == 4) c = (int)br_ue(&b);                                  // max_long_term_frame_idx_plus1
			if ((op == 2 && a > 15) || ((op == 3 || op == 6) && c > 15) || (op == 4 && c > 16))
				return fail(s, H264D_ERROR, "long-term index out of range");
			sl->mmco[sl->nmmco].op = (int)op; sl->mmco[sl->nmmco].a = a; sl->mmco[sl->nmmco].b = c; sl->nmmco++;
		}
	}
	if (b.err) return fail(s, H264D_ERROR, "slice header truncated");
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------

void h264d_init(H264DState *s, int nal_length_size) {
	memset(s, 0, sizeof *s);
	s->nal_length_size = nal_length_size;
	s->active_sps = -1;
	s->prev_poc_msb = 1 << 16; s->prev_poc_lsb = -1; s->prev_frame_num = -1; s->prev_frame_num_offset = 0;
	for (int i = 0; i < 16; i++) s->long_ref[i] = -1;
}

void h264d_free(H264DState *s) {
	for (int i = 0; i < 256; i++) { free(s->pps[i]); s->pps[i] = NULL; }
	free(s->offsets); s->offsets = NULL; s->offsets_cap = 0;
}

static void remove_all_refs(H264DState *s) {
	s->nshort = 0; s->nlong = 0;
	for (int i = 0; i < 16; i++) s->long_ref[i] = -1;
	memset(s->slot_busy, 0, sizeof s->slot_busy);
}

// libavcodec's idr(): every reference goes, the POC state restarts
static void idr(H264DState *s) {
	remove_all_refs(s);
	s->prev_frame_num = s->prev_frame_num_offset = 0;
	s->prev_poc_msb = 1 << 16; s->prev_poc_lsb = -1;
}

void h264d_flush(H264DState *s) {
	idr(s);
	s->prev_frame_num = -1;
}

static int nal_type_of(const uint8_t *nal) { return nal[0] & 0x1f; }

// the sample's NAL units, each behind a nal_length_size-byte big-endian length: 1 and the next one, 0 at the end, -1 if
// the lengths do not add up (empty NAL units are skipped)
typedef struct { const uint8_t *p; size_t n, o; int ls; } NalIt;
static int nal_next(NalIt *it, const uint8_t **nal, size_t *len) {
	while (it->o < it->n) {
		if (it->n - it->o < (size_t)it->ls) return -1;
		size_t l = 0;
		for (int i = 0; i < it->ls; i++) l = l << 8 | it->p[it->o + i];
		it->o += (size_t)it->ls;
		if (l > it->n - it->o) return -1;
		*nal = it->p + it->o; *len = l; it->o += l;
		if (l) return 1;
	}
	return 0;
}

size_t h264d_bitstream_size(const H264DState *s, const uint8_t *data, size_t n) {
	size_t total = 0, len; const uint8_t *nal; int r;
	NalIt it = { data, n, 0, s->nal_length_size };
	while ((r = nal_next(&it, &nal, &len)) > 0) { const int t = nal_type_of(nal); if (t == 1 || t == 5) total += 3 + len; }
	return r < 0 ? 0 : total;
}

int h264d_parse_avcc(H264DState *s, const uint8_t *c, size_t n) {
	// AVCDecoderConfigurationRecord: version, profile, compatibility, level, 6 bits + lengthSizeMinusOne,
	// 3 bits + numOfSequenceParameterSets, (16-bit length, SPS)..., numOfPictureParameterSets, (16-bit length, PPS)...
	if (n < 7 || c[0] != 1) return fail(s, H264D_ERROR, "avcC");
	s->nal_length_size = (c[4] & 3) + 1;
	size_t o = 5;
	for (int list = 0; list < 2; list++) {
		if (o >= n) return fail(s, H264D_ERROR, "avcC truncated");
		const int count = list ? c[o] : (c[o] & 0x1f);
		o++;
		for (int i = 0; i < count; i++) {
			if (o + 2 > n) return fail(s, H264D_ERROR, "avcC truncated");
			const size_t len = (size_t)c[o] << 8 | c[o + 1];
			o += 2;
			if (!len || o + len > n) return fail(s, H264D_ERROR, "avcC truncated");
			const int t = nal_type_of(c + o);
			const int r = t == 7 ? parse_sps(s, c + o, len) : t == 8 ? parse_pps(s, c + o, len) : H264D_NO_PICTURE;
			if (r < 0) return r;
			o += len;
		}
	}
	return H264D_NO_PICTURE;
}

// 8.2.1 as libavcodec computes it (ff_h264_init_poc, frames only)
static void picture_order_count(H264DState *s, const H264DSps *sps, const Slice *sl, int ref_idc, H264DPicture *pic, int *poc_msb, int *frame_num_offset) {
	const int max_frame_num = 1 << sps->log2_max_frame_num;
	int offset = s->prev_frame_num_offset;
	if (sl->frame_num < s->prev_frame_num) offset += max_frame_num;
	*frame_num_offset = offset;
	int64_t poc[2];
	const StdVideoH264SequenceParameterSet *d = &sps->std;
	if (d->pic_order_cnt_type == 0) {
		const int max_lsb = 1 << sps->log2_max_poc_lsb;
		const int prev_lsb = s->prev_poc_lsb < 0 ? sl->poc_lsb : s->prev_poc_lsb;
		int msb;
		if (sl->poc_lsb < prev_lsb && prev_lsb - sl->poc_lsb >= max_lsb / 2) msb = s->prev_poc_msb + max_lsb;
		else if (sl->poc_lsb > prev_lsb && prev_lsb - sl->poc_lsb < -max_lsb / 2) msb = s->prev_poc_msb - max_lsb;
		else msb = s->prev_poc_msb;
		*poc_msb = msb;
		poc[0] = poc[1] = (int64_t)msb + sl->poc_lsb;
		poc[1] += sl->delta_poc_bottom;
	} else if (d->pic_order_cnt_type == 1) {
		const int cycle = d->num_ref_frames_in_pic_order_cnt_cycle;
		int abs_frame_num = cycle ? offset + sl->frame_num : 0;
		if (!ref_idc && abs_frame_num > 0) abs_frame_num--;
		int64_t delta_cycle = 0, expected = 0;
		for (int i = 0; i < cycle; i++) delta_cycle += sps->offset_for_ref_frame[i];
		if (abs_frame_num > 0) {
			const int cnt = (abs_frame_num - 1) / cycle, in_cycle = (abs_frame_num - 1) % cycle;
			expected = (int64_t)cnt * delta_cycle;
			for (int i = 0; i <= in_cycle; i++) expected += sps->offset_for_ref_frame[i];
		}
		if (!ref_idc) expected += d->offset_for_non_ref_pic;
		poc[0] = expected + sl->delta_poc[0];
		poc[1] = poc[0] + d->offset_for_top_to_bottom_field + sl->delta_poc[1];
		*poc_msb = 0;
	} else {
		int v = 2 * (offset + sl->frame_num);
		if (!ref_idc) v--;
		poc[0] = poc[1] = v;
		*poc_msb = 0;
	}
	pic->poc[0] = (int32_t)poc[0]; pic->poc[1] = (int32_t)poc[1];
}

int h264d_parse_sample(H264DState *s, const uint8_t *data, size_t n, uint8_t *out, size_t cap, H264DPicture *pic) {
	memset(pic, 0, sizeof *pic);
	int have = 0, r = 0, more;
	Slice first = { 0 };
	size_t w = 0, len;
	uint32_t nsl = 0;
	const uint8_t *nal;
	NalIt it = { data, n, 0, s->nal_length_size };
	while ((more = nal_next(&it, &nal, &len)) > 0) {
		const int t = nal_type_of(nal), ref_idc = nal[0] >> 5 & 3;
		if (t == 7) { if ((r = parse_sps(s, nal, len)) < 0) return r; }
		else if (t == 8) { if ((r = parse_pps(s, nal, len)) < 0) return r; }
		else if (t >= 2 && t <= 4) return fail(s, H264D_UNSUPPORTED, "data partitioning");
		else if (t == 1 || t == 5) {
			Slice sl;
			if ((r = parse_slice(s, nal, len, t == 5, ref_idc, !have, &sl)) < 0) return r;
			if (sl.redundant > 0) { s->redundant_slices++; continue; }   // libavcodec skips redundant slices too
			if (!have) {
				first = sl; have = 1;
				pic->idr = t == 5; pic->nal_ref_idc = ref_idc; pic->is_intra = 1; pic->slice_type = sl.slice_type;
			}
			if (sl.slice_type != 2) pic->is_intra = 0;
			if (sl.active[0] > pic->max_active_l0) pic->max_active_l0 = sl.active[0];
			if (sl.active[1] > pic->max_active_l1) pic->max_active_l1 = sl.active[1];
			if (w + 3 + len > cap) return fail(s, H264D_ERROR, "bitstream buffer too small");
			if (nsl == s->offsets_cap) {
				const uint32_t nc = s->offsets_cap ? s->offsets_cap * 2 : 64;
				uint32_t *no = realloc(s->offsets, nc * sizeof *no);
				if (!no) return fail(s, H264D_ERROR, "no memory");
				s->offsets = no; s->offsets_cap = nc;
			}
			s->offsets[nsl++] = (uint32_t)w;
			out[w] = 0; out[w + 1] = 0; out[w + 2] = 1;
			memcpy(out + w + 3, nal, len);
			w += 3 + len;
		}
	}
	if (more < 0) return fail(s, H264D_ERROR, "NAL unit lengths");
	if (!have) return H264D_NO_PICTURE;

	const H264DPps *pps = s->pps[first.pps_id];
	const H264DSps *sps = &s->sps[pps->sps_id];
	if (!s->pictures && !pic->idr) return fail(s, H264D_UNSUPPORTED, "the stream does not start with an IDR picture");
	if (s->active_sps >= 0 && s->active_sps != pps->sps_id) {
		const H264DSps *o = &s->sps[s->active_sps];
		if (o->width_mbs != sps->width_mbs || o->height_mbs != sps->height_mbs) return fail(s, H264D_UNSUPPORTED, "the coded size changed");
	}
	if (pic->idr) idr(s);   // libavcodec: at the IDR's NAL unit, before the picture starts

	// frame_num gaps (gaps_in_frame_num_value_allowed_flag is 0 here: a lost picture); libavcodec invents frames
	if (!pic->idr && s->prev_frame_num >= 0 && first.frame_num != s->prev_frame_num &&
	    first.frame_num != (s->prev_frame_num + 1) % (1 << sps->log2_max_frame_num)) { pic->gap = 1; s->gaps++; }

	int poc_msb = 0, frame_num_offset = 0;
	picture_order_count(s, sps, &first, pic->nal_ref_idc, pic, &poc_msb, &frame_num_offset);

	pic->frame_num = first.frame_num; pic->idr_pic_id = first.idr_pic_id;
	pic->sps = sps; pic->pps = pps;
	pic->nslices = nsl; pic->slice_offsets = s->offsets; pic->size = w;
	pic->long_term_reference_flag = first.long_term_reference_flag;
	pic->adaptive = first.adaptive;
	pic->nmmco = first.nmmco; memcpy(pic->mmco, first.mmco, sizeof first.mmco);
	pic->std = (StdVideoDecodeH264PictureInfo){
		.flags = { .field_pic_flag = 0, .is_intra = (uint32_t)pic->is_intra, .IdrPicFlag = (uint32_t)pic->idr, .bottom_field_flag = 0,
		           .is_reference = pic->nal_ref_idc != 0, .complementary_field_pair = 0 },
		.seq_parameter_set_id = (uint8_t)pps->sps_id, .pic_parameter_set_id = (uint8_t)first.pps_id,
		.frame_num = (uint16_t)first.frame_num, .idr_pic_id = (uint16_t)first.idr_pic_id,
		.PicOrderCnt = { pic->poc[0], pic->poc[1] },
	};
	// the references: short-term newest first, then long-term by LongTermFrameIdx (libavcodec's order)
	for (int i = 0; i < s->nshort; i++) {
		const int sl = s->short_ref[i];
		pic->refs[pic->nrefs++] = (H264DRef){ sl, s->slot[sl].frame_num, { s->slot[sl].poc[0], s->slot[sl].poc[1] }, 0 };
	}
	for (int i = 0; i < 16 && pic->nrefs < H264D_MAX_REFS; i++) {
		const int sl = s->long_ref[i];
		if (sl < 0) continue;
		pic->refs[pic->nrefs++] = (H264DRef){ sl, i, { s->slot[sl].poc[0], s->slot[sl].poc[1] }, 1 };
	}
	pic->slot = -1;
	for (int i = 0; i < H264D_SLOTS; i++) if (!s->slot_busy[i]) { pic->slot = i; break; }
	if (pic->slot < 0) return fail(s, H264D_ERROR, "no free DPB slot");
	s->slot[pic->slot] = (H264DSlotInfo){ first.frame_num, { pic->poc[0], pic->poc[1] }, -1 };
	s->active_sps = pps->sps_id;
	s->pictures++;
	// the POC state libavcodec carries to the next picture (ff_h264_field_end), applied by h264d_finish_picture
	s->pending_poc_msb = poc_msb; s->pending_poc_lsb = first.poc_lsb; s->pending_frame_num_offset = frame_num_offset;
	s->why[0] = 0;
	return H264D_PICTURE;
}

// libavcodec's remove_short / remove_long
static void unref_slot(H264DState *s, int slot) { if (slot >= 0) s->slot_busy[slot] = 0; }
static int find_short(const H264DState *s, int frame_num) {
	for (int i = 0; i < s->nshort; i++) if (s->slot[s->short_ref[i]].frame_num == frame_num) return i;
	return -1;
}
static void remove_short_at(H264DState *s, int i, int keep_slot) {
	const int sl = s->short_ref[i];
	memmove(&s->short_ref[i], &s->short_ref[i + 1], (size_t)(s->nshort - i - 1) * sizeof s->short_ref[0]);
	s->nshort--;
	if (!keep_slot) unref_slot(s, sl);
}
static void remove_long(H264DState *s, int idx) {
	const int sl = s->long_ref[idx];
	if (sl < 0) return;
	s->long_ref[idx] = -1; s->nlong--;
	s->slot[sl].long_idx = -1;
	unref_slot(s, sl);
}

void h264d_finish_picture(H264DState *s, const H264DPicture *pic) {
	const H264DSps *sps = pic->sps;
	const int cur = pic->slot;
	if (pic->nal_ref_idc) {
		int assigned = 0;
		// the operations: an IDR's long_term_reference_flag, the adaptive ones, or the sliding window
		int nm = 0; struct { int op, a, b; } m[67];
		if (pic->idr) { if (pic->long_term_reference_flag) { m[0].op = 6; m[0].a = 0; m[0].b = 0; nm = 1; } }
		else if (pic->adaptive) { nm = pic->nmmco; for (int i = 0; i < nm; i++) { m[i].op = pic->mmco[i].op; m[i].a = pic->mmco[i].a; m[i].b = pic->mmco[i].b; } }
		else if (s->nshort && s->nlong + s->nshort >= sps->std.max_num_ref_frames) {
			m[0].op = 1; m[0].a = -1; m[0].b = s->slot[s->short_ref[s->nshort - 1]].frame_num; nm = 1;   // a: absolute frame_num in b
		}
		const int max_frame_num = 1 << sps->log2_max_frame_num;
		for (int i = 0; i < nm; i++) {
			const int op = m[i].op;
			// short-term picture numbers: frames, so PicNum = FrameNumWrap and libavcodec matches on frame_num
			const int fn = op == 1 && m[i].a < 0 ? m[i].b : (pic->frame_num - m[i].a - 1) & (max_frame_num - 1);
			if (op == 1 || op == 3) {
				const int j = find_short(s, fn);
				if (j < 0) continue;   // "mmco: unref short failure"
				if (op == 1) remove_short_at(s, j, 0);
				else {
					const int sl = s->short_ref[j], idx = m[i].b;
					if (s->long_ref[idx] != sl) remove_long(s, idx);
					remove_short_at(s, j, 1);
					s->long_ref[idx] = sl; s->slot[sl].long_idx = idx; s->nlong++;
				}
			} else if (op == 2) {
				remove_long(s, m[i].a);
			} else if (op == 6) {
				const int idx = m[i].b;
				if (s->slot[cur].long_idx >= 0 && s->long_ref[s->slot[cur].long_idx] == cur) {   // already long-term
					const int old = s->slot[cur].long_idx;
					s->long_ref[old] = -1; s->nlong--; s->slot[cur].long_idx = -1;
				}
				if (s->long_ref[idx] != cur) {
					remove_long(s, idx);
					s->long_ref[idx] = cur; s->slot[cur].long_idx = idx; s->nlong++;
				}
				s->slot_busy[cur] = 1;
				assigned = 1;
			} else if (op == 4) {
				for (int j = m[i].b; j < 16; j++) remove_long(s, j);
			} else if (op == 5) {
				while (s->nshort) remove_short_at(s, 0, 0);
				for (int j = 0; j < 16; j++) remove_long(s, j);
				s->slot[cur].frame_num = 0;
				s->mmco5 = 1;
			}
		}
		if (!assigned) {
			const int j = find_short(s, s->slot[cur].frame_num);
			if (j >= 0) remove_short_at(s, j, 0);   // "illegal short term buffer state detected"
			memmove(&s->short_ref[1], &s->short_ref[0], (size_t)s->nshort * sizeof s->short_ref[0]);
			s->short_ref[0] = cur; s->nshort++;
			s->slot_busy[cur] = 1;
		}
		const int max_refs = sps->std.max_num_ref_frames ? sps->std.max_num_ref_frames : 1;
		if (s->nlong + s->nshort > max_refs) {   // "probably corrupt input, discarding one"
			if (s->nlong && !s->nshort) { for (int j = 0; j < 16; j++) if (s->long_ref[j] >= 0) { remove_long(s, j); break; } }
			else remove_short_at(s, s->nshort - 1, 0);
		}
		s->prev_poc_msb = s->pending_poc_msb; s->prev_poc_lsb = s->pending_poc_lsb;
	}
	s->prev_frame_num_offset = s->pending_frame_num_offset;
	s->prev_frame_num = s->mmco5 ? 0 : pic->frame_num;
	s->mmco5 = 0;
}
