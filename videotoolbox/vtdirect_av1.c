// SPDX-License-Identifier: MIT
// vtdirect_av1.c: AV1 sequence and frame headers, tile groups and the reference frame slots for the direct Vulkan
// Video decode path (see vtdirect_av1.h). Written from the AV1 specification (5.3-5.12, 6.8, 7.20, 7.21) and the
// Vulkan specification, with libavcodec 9.0.2's choices where the hardware must see the same values as on the
// libavcodec path.
#include "vtdirect_av1.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------------------------------------------------
// Bit reader (4.10): MSB first, no emulation prevention in AV1

typedef struct { const uint8_t *p; size_t n, pos; int err; } BR;   // pos in bits

static void br_init(BR *b, const uint8_t *p, size_t n) { *b = (BR){ p, n, 0, 0 }; }
static unsigned br_bit(BR *b) {
	if (b->pos >= b->n * 8) { b->err = 1; return 0; }
	const unsigned v = (b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
	b->pos++;
	return v;
}
static uint32_t f(BR *b, int n) { uint32_t v = 0; while (n-- > 0) v = v << 1 | br_bit(b); return v; }
static int32_t su(BR *b, int n) {   // 4.10.6
	int32_t v = (int32_t)f(b, n);
	const int32_t sign = 1 << (n - 1);
	if (v & sign) v -= 2 * sign;
	return v;
}
static int floor_log2(uint32_t x) { int s = 0; while (x > 1) { x >>= 1; s++; } return s; }
static uint32_t ns(BR *b, uint32_t n) {   // 4.10.7
	if (n <= 1) return 0;
	const int w = floor_log2(n) + 1;
	const uint32_t m = (1u << w) - n;
	const uint32_t v = f(b, w - 1);
	if (v < m) return v;
	return (v << 1) - m + br_bit(b);
}
static uint32_t uvlc(BR *b) {   // 4.10.3
	int lz = 0;
	while (!br_bit(b)) { if (b->err) return 0; lz++; }
	if (lz >= 32) return UINT32_MAX;
	return f(b, lz) + (uint32_t)((1ull << lz) - 1);
}
// leb128 (4.10.5) over bytes: the value, *len the bytes it took; -1 if malformed
static int leb128(const uint8_t *p, size_t n, uint64_t *v, size_t *len) {
	*v = 0;
	for (size_t i = 0; i < 8; i++) {
		if (i >= n) return -1;
		*v |= (uint64_t)(p[i] & 0x7f) << (i * 7);
		if (!(p[i] & 0x80)) { *len = i + 1; return 0; }
	}
	return -1;
}
static void byte_alignment(BR *b) { while (b->pos & 7) br_bit(b); }
static int tile_log2(int blk, int target) { int k = 0; while ((blk << k) < target) k++; return k; }

// ---------------------------------------------------------------------------------------------------------------------

enum { OBU_SEQUENCE_HEADER = 1, OBU_TEMPORAL_DELIMITER, OBU_FRAME_HEADER, OBU_TILE_GROUP, OBU_METADATA, OBU_FRAME,
       OBU_REDUNDANT_FRAME_HEADER, OBU_TILE_LIST, OBU_PADDING = 15 };
enum { KEY_FRAME = 0, INTER_FRAME, INTRA_ONLY_FRAME, SWITCH_FRAME };
enum { PRIMARY_REF_NONE = 7, NUM_REF_FRAMES = 8, REFS_PER_FRAME = 7, SELECT_SCREEN_CONTENT_TOOLS = 2, SELECT_INTEGER_MV = 2,
       SUPERRES_NUM = 8, SUPERRES_DENOM_MIN = 9, SUPERRES_DENOM_BITS = 3, MAX_TILE_WIDTH = 4096, MAX_TILE_AREA = 4096 * 2304,
       MAX_TILE_ROWS = 64, MAX_TILE_COLS = 64, WARPEDMODEL_PREC_BITS = 16, GM_ABS_TRANS_BITS = 12, GM_ABS_TRANS_ONLY_BITS = 9,
       GM_ABS_ALPHA_BITS = 12, GM_ALPHA_PREC_BITS = 15, GM_TRANS_PREC_BITS = 6, GM_TRANS_ONLY_PREC_BITS = 3 };
enum { IDENTITY = 0, TRANSLATION, ROTZOOM, AFFINE };
enum { LAST_FRAME = 1, LAST2_FRAME, LAST3_FRAME, GOLDEN_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME };

static int fail(Av1dState *s, int code, const char *why) { snprintf(s->why, sizeof s->why, "%s", why); return code; }
const char *av1d_why(const Av1dState *s) { return s->why; }

void av1d_seq_fix(Av1dSeq *q) { q->std.pColorConfig = &q->color; q->std.pTimingInfo = &q->timing; }

void av1d_init(Av1dState *s) {
	memset(s, 0, sizeof *s);
	for (int i = 0; i < NUM_REF_FRAMES; i++) s->ref[i].dpb = -1;
}

void av1d_flush(Av1dState *s) {
	for (int i = 0; i < NUM_REF_FRAMES; i++) { s->ref[i].valid = 0; s->ref[i].pic = 0; s->ref[i].dpb = -1; }
	s->keyframes = 0;
}

unsigned av1d_dpb_busy(const Av1dState *s) {
	unsigned m = 0;
	for (int i = 0; i < NUM_REF_FRAMES; i++) if (s->ref[i].pic && s->ref[i].dpb >= 0) m |= 1u << s->ref[i].dpb;
	return m;
}

void av1d_lost(Av1dState *s, uint64_t gen) {
	for (int i = 0; i < NUM_REF_FRAMES; i++) if (s->ref[i].pic && s->ref[i].gen == gen) { s->ref[i].pic = 0; s->ref[i].dpb = -1; }
}

// ---------------------------------------------------------------------------------------------------------------------
// Sequence header (5.5)

static int parse_sequence_header(Av1dState *s, const uint8_t *p, size_t n, Av1dSeq *q) {
	BR b; br_init(&b, p, n);
	memset(q, 0, sizeof *q);
	StdVideoAV1SequenceHeader *d = &q->std;
	StdVideoAV1ColorConfig *c = &q->color;
	d->seq_profile = (StdVideoAV1Profile)f(&b, 3);
	d->flags.still_picture = f(&b, 1);
	d->flags.reduced_still_picture_header = f(&b, 1);
	int buffer_delay_length = 0;
	if (d->flags.reduced_still_picture_header) {
		q->ops = 1; q->op_idc[0] = 0; q->level[0] = (uint8_t)f(&b, 5);
	} else {
		d->flags.timing_info_present_flag = f(&b, 1);
		if (d->flags.timing_info_present_flag) {
			q->timing.num_units_in_display_tick = f(&b, 32);
			q->timing.time_scale = f(&b, 32);
			q->timing.flags.equal_picture_interval = f(&b, 1);
			if (q->timing.flags.equal_picture_interval) q->timing.num_ticks_per_picture_minus_1 = uvlc(&b);
			q->decoder_model_info_present = f(&b, 1);
			if (q->decoder_model_info_present) {
				buffer_delay_length = (int)f(&b, 5) + 1;
				f(&b, 32);   // num_units_in_decoding_tick
				q->buffer_removal_time_length = (int)f(&b, 5) + 1;
				q->frame_presentation_time_length = (int)f(&b, 5) + 1;
			}
		}
		d->flags.initial_display_delay_present_flag = f(&b, 1);
		q->ops = (int)f(&b, 5) + 1;
		for (int i = 0; i < q->ops; i++) {
			q->op_idc[i] = (uint16_t)f(&b, 12);
			q->level[i] = (uint8_t)f(&b, 5);
			q->tier[i] = q->level[i] > 7 ? (uint8_t)f(&b, 1) : 0;
			if (q->decoder_model_info_present) {
				q->decoder_model_present_for_op[i] = (uint8_t)f(&b, 1);
				if (q->decoder_model_present_for_op[i]) { f(&b, buffer_delay_length); f(&b, buffer_delay_length); f(&b, 1); }
			}
			if (d->flags.initial_display_delay_present_flag && f(&b, 1)) f(&b, 4);
		}
	}
	d->frame_width_bits_minus_1 = (uint8_t)f(&b, 4);
	d->frame_height_bits_minus_1 = (uint8_t)f(&b, 4);
	d->max_frame_width_minus_1 = (uint16_t)f(&b, d->frame_width_bits_minus_1 + 1);
	d->max_frame_height_minus_1 = (uint16_t)f(&b, d->frame_height_bits_minus_1 + 1);
	d->flags.frame_id_numbers_present_flag = d->flags.reduced_still_picture_header ? 0 : f(&b, 1);
	if (d->flags.frame_id_numbers_present_flag) {
		d->delta_frame_id_length_minus_2 = (uint8_t)f(&b, 4);
		d->additional_frame_id_length_minus_1 = (uint8_t)f(&b, 3);
	}
	d->flags.use_128x128_superblock = f(&b, 1);
	d->flags.enable_filter_intra = f(&b, 1);
	d->flags.enable_intra_edge_filter = f(&b, 1);
	if (d->flags.reduced_still_picture_header) {
		d->seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
		d->seq_force_integer_mv = SELECT_INTEGER_MV;
	} else {
		d->flags.enable_interintra_compound = f(&b, 1);
		d->flags.enable_masked_compound = f(&b, 1);
		d->flags.enable_warped_motion = f(&b, 1);
		d->flags.enable_dual_filter = f(&b, 1);
		d->flags.enable_order_hint = f(&b, 1);
		if (d->flags.enable_order_hint) {
			d->flags.enable_jnt_comp = f(&b, 1);
			d->flags.enable_ref_frame_mvs = f(&b, 1);
		}
		d->seq_force_screen_content_tools = f(&b, 1) ? SELECT_SCREEN_CONTENT_TOOLS : (uint8_t)f(&b, 1);   // seq_choose_screen_content_tools
		if (d->seq_force_screen_content_tools > 0) d->seq_force_integer_mv = f(&b, 1) ? SELECT_INTEGER_MV : (uint8_t)f(&b, 1);
		else d->seq_force_integer_mv = SELECT_INTEGER_MV;
		if (d->flags.enable_order_hint) d->order_hint_bits_minus_1 = (uint8_t)f(&b, 3);
	}
	d->flags.enable_superres = f(&b, 1);
	d->flags.enable_cdef = f(&b, 1);
	d->flags.enable_restoration = f(&b, 1);
	// color_config (5.5.2)
	const int high = (int)f(&b, 1);
	int twelve = 0;
	if (d->seq_profile == 2 && high) twelve = (int)f(&b, 1);
	q->bit_depth = twelve ? 12 : high ? 10 : 8;
	c->BitDepth = (uint8_t)q->bit_depth;
	c->flags.mono_chrome = d->seq_profile == 1 ? 0 : f(&b, 1);
	c->flags.color_description_present_flag = f(&b, 1);
	if (c->flags.color_description_present_flag) {
		c->color_primaries = (StdVideoAV1ColorPrimaries)f(&b, 8);
		c->transfer_characteristics = (StdVideoAV1TransferCharacteristics)f(&b, 8);
		c->matrix_coefficients = (StdVideoAV1MatrixCoefficients)f(&b, 8);
	} else {
		c->color_primaries = STD_VIDEO_AV1_COLOR_PRIMARIES_UNSPECIFIED;
		c->transfer_characteristics = STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_UNSPECIFIED;
		c->matrix_coefficients = STD_VIDEO_AV1_MATRIX_COEFFICIENTS_UNSPECIFIED;
	}
	if (c->flags.mono_chrome) {
		c->flags.color_range = f(&b, 1);
		c->subsampling_x = c->subsampling_y = 1;
		c->chroma_sample_position = STD_VIDEO_AV1_CHROMA_SAMPLE_POSITION_UNKNOWN;
	} else {
		if (c->color_primaries == STD_VIDEO_AV1_COLOR_PRIMARIES_BT_709 && c->transfer_characteristics == STD_VIDEO_AV1_TRANSFER_CHARACTERISTICS_SRGB &&
		    c->matrix_coefficients == STD_VIDEO_AV1_MATRIX_COEFFICIENTS_IDENTITY) {
			c->flags.color_range = 1;
		} else {
			c->flags.color_range = f(&b, 1);
			if (d->seq_profile == 0) { c->subsampling_x = c->subsampling_y = 1; }
			else if (d->seq_profile == 1) { c->subsampling_x = c->subsampling_y = 0; }
			else if (q->bit_depth == 12) {
				c->subsampling_x = (uint8_t)f(&b, 1);
				c->subsampling_y = c->subsampling_x ? (uint8_t)f(&b, 1) : 0;
			} else { c->subsampling_x = 1; c->subsampling_y = 0; }
			if (c->subsampling_x && c->subsampling_y) c->chroma_sample_position = (StdVideoAV1ChromaSamplePosition)f(&b, 2);
		}
		c->flags.separate_uv_delta_q = f(&b, 1);
	}
	d->flags.film_grain_params_present = f(&b, 1);
	if (b.err) return fail(s, AV1D_ERROR, "sequence header: truncated");
	q->rawn = n < sizeof q->raw ? n : sizeof q->raw;
	memcpy(q->raw, p, q->rawn);
	av1d_seq_fix(q);
	// what the B580 decodes through Vulkan Video here: Main profile (8 or 10 bits) 4:2:0, one spatial layer
	if (d->seq_profile != STD_VIDEO_AV1_PROFILE_MAIN) q->unsupported = 1;
	else if (c->flags.mono_chrome || !c->subsampling_x || !c->subsampling_y) q->unsupported = 2;
	else if (q->bit_depth != 8 && q->bit_depth != 10) q->unsupported = 3;
	else if ((q->op_idc[0] >> 8) & ~1u) q->unsupported = 4;
	return AV1D_NO_PICTURE;
}

static const char *seq_unsupported(int u) {
	static const char *w[] = { "", "profile not Main", "not 4:2:0", "not 8 or 10 bits", "several spatial layers" };
	return u >= 0 && u < 5 ? w[u] : "?";
}

// ---------------------------------------------------------------------------------------------------------------------
// Frame header (5.9) and what the decode gets for it

// the frame being parsed: the syntax and the decoder's variables of the specification
typedef struct {
	int show_existing_frame, frame_to_show_map_idx;
	int frame_type, show_frame, showable_frame, error_resilient_mode, disable_cdf_update;
	int allow_screen_content_tools, force_integer_mv, current_frame_id, frame_size_override_flag, order_hint;
	int primary_ref_frame, refresh_frame_flags, allow_intrabc, frame_refs_short_signaling;
	int ref_frame_idx[7];
	int upscaled_width, frame_width, frame_height, render_width, render_height, render_and_frame_size_different;
	int use_superres, coded_denom, mi_cols, mi_rows;
	int allow_high_precision_mv, is_filter_switchable, interpolation_filter, is_motion_mode_switchable, use_ref_frame_mvs;
	int disable_frame_end_update_cdf, buffer_removal_time_present_flag;
	// tile_info
	int uniform, tile_cols_log2, tile_rows_log2, tile_cols, tile_rows, context_update_tile_id, tile_size_bytes;
	uint16_t col_start_sb[65], row_start_sb[65], width_sbs[64], height_sbs[64];
	// quantization, segmentation, deltas
	int base_q_idx, dq_y_dc, dq_u_dc, dq_u_ac, dq_v_dc, dq_v_ac, diff_uv_delta, using_qmatrix, qm_y, qm_u, qm_v;
	int seg_enabled, seg_update_map, seg_temporal_update, seg_update_data;
	uint8_t feature_enabled[8][8]; int16_t feature_value[8][8];
	int delta_q_present, delta_q_res, delta_lf_present, delta_lf_res, delta_lf_multi;
	int coded_lossless, all_lossless;
	// loop filter, cdef, lr
	int lf_level[4], lf_sharpness, lf_delta_enabled, lf_delta_update; int8_t lf_ref[8], lf_mode[2];
	int cdef_damping_minus_3, cdef_bits; uint8_t cdef_y_pri[8], cdef_y_sec[8], cdef_uv_pri[8], cdef_uv_sec[8];
	int lr_type[3], lr_unit_shift, lr_uv_shift;
	int tx_mode, reference_select, skip_mode_present, allow_warped_motion, reduced_tx_set;
	int gm_type[8]; int32_t gm[8][6];
	int grain_apply; StdVideoAV1FilmGrain grain;
	// libavcodec's derived values (av1dec.c): order_hints, sign bias, skip mode frames
	uint8_t order_hints[8]; uint8_t sign_bias; uint8_t skip_frame[2];
} Hdr;

static int rel_dist(const Av1dSeq *q, int a, int b) {   // get_relative_dist (7.12.2)
	if (!q->std.flags.enable_order_hint) return 0;
	const int bits = q->std.order_hint_bits_minus_1 + 1;
	int diff = a - b;
	const int m = 1 << (bits - 1);
	diff = (diff & (m - 1)) - (diff & m);
	return diff;
}

static void superres_params(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.8
	h->use_superres = q->std.flags.enable_superres ? (int)f(b, 1) : 0;
	int denom = SUPERRES_NUM;
	if (h->use_superres) { h->coded_denom = (int)f(b, SUPERRES_DENOM_BITS); denom = h->coded_denom + SUPERRES_DENOM_MIN; }
	h->upscaled_width = h->frame_width;
	h->frame_width = (h->upscaled_width * SUPERRES_NUM + denom / 2) / denom;
}
static void compute_image_size(Hdr *h) { h->mi_cols = 2 * ((h->frame_width + 7) >> 3); h->mi_rows = 2 * ((h->frame_height + 7) >> 3); }
static void frame_size(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.5
	if (h->frame_size_override_flag) {
		h->frame_width = (int)f(b, q->std.frame_width_bits_minus_1 + 1) + 1;
		h->frame_height = (int)f(b, q->std.frame_height_bits_minus_1 + 1) + 1;
	} else {
		h->frame_width = q->std.max_frame_width_minus_1 + 1;
		h->frame_height = q->std.max_frame_height_minus_1 + 1;
	}
	superres_params(b, q, h);
	compute_image_size(h);
}
static void render_size(BR *b, Hdr *h) {   // 5.9.6
	h->render_and_frame_size_different = (int)f(b, 1);
	if (h->render_and_frame_size_different) { h->render_width = (int)f(b, 16) + 1; h->render_height = (int)f(b, 16) + 1; }
	else { h->render_width = h->upscaled_width; h->render_height = h->frame_height; }
}

// set_frame_refs (7.8): ref_frame_idx[] from last_frame_idx and gold_frame_idx
static void set_frame_refs(const Av1dState *s, const Av1dSeq *q, Hdr *h, int last_idx, int gold_idx) {
	int used[8] = { 0 }, shifted[8];
	for (int i = 0; i < 7; i++) h->ref_frame_idx[i] = -1;
	h->ref_frame_idx[LAST_FRAME - LAST_FRAME] = last_idx;
	h->ref_frame_idx[GOLDEN_FRAME - LAST_FRAME] = gold_idx;
	used[last_idx] = used[gold_idx] = 1;
	const int cur = 1 << q->std.order_hint_bits_minus_1;
	for (int i = 0; i < 8; i++) shifted[i] = cur + rel_dist(q, s->ref[i].order_hint, h->order_hint);
	int last_hint = shifted[last_idx], earliest = shifted[gold_idx];
	int ref = -1;   // ALTREF: the backward reference with the largest hint
	for (int i = 0; i < 8; i++) if (!used[i] && shifted[i] >= cur && (ref < 0 || shifted[i] >= last_hint)) { ref = i; last_hint = shifted[i]; }
	if (ref >= 0) { h->ref_frame_idx[ALTREF_FRAME - LAST_FRAME] = ref; used[ref] = 1; }
	ref = -1;   // BWDREF: the backward reference with the smallest hint
	for (int i = 0; i < 8; i++) if (!used[i] && shifted[i] >= cur && (ref < 0 || shifted[i] < earliest)) { ref = i; earliest = shifted[i]; }
	if (ref >= 0) { h->ref_frame_idx[BWDREF_FRAME - LAST_FRAME] = ref; used[ref] = 1; }
	ref = -1;   // ALTREF2: the next one
	for (int i = 0; i < 8; i++) if (!used[i] && shifted[i] >= cur && (ref < 0 || shifted[i] < earliest)) { ref = i; earliest = shifted[i]; }
	if (ref >= 0) { h->ref_frame_idx[ALTREF2_FRAME - LAST_FRAME] = ref; used[ref] = 1; }
	static const int order[5] = { LAST2_FRAME, LAST3_FRAME, BWDREF_FRAME, ALTREF2_FRAME, ALTREF_FRAME };
	for (int k = 0; k < 5; k++) {   // the remaining ones: forward references, closest first
		const int rf = order[k];
		if (h->ref_frame_idx[rf - LAST_FRAME] >= 0) continue;
		ref = -1;
		for (int i = 0; i < 8; i++) if (!used[i] && shifted[i] < cur && (ref < 0 || shifted[i] >= last_hint)) { ref = i; last_hint = shifted[i]; }
		if (ref >= 0) { h->ref_frame_idx[rf - LAST_FRAME] = ref; used[ref] = 1; }
	}
	ref = -1;   // anything left: the earliest frame
	for (int i = 0; i < 8; i++) if (ref < 0 || shifted[i] < earliest) { ref = i; earliest = shifted[i]; }
	for (int i = 0; i < 7; i++) if (h->ref_frame_idx[i] < 0) h->ref_frame_idx[i] = ref;
}

static int tile_info(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.15
	const int sb128 = q->std.flags.use_128x128_superblock;
	const int sb_cols = sb128 ? (h->mi_cols + 31) >> 5 : (h->mi_cols + 15) >> 4;
	const int sb_rows = sb128 ? (h->mi_rows + 31) >> 5 : (h->mi_rows + 15) >> 4;
	const int sb_shift = sb128 ? 5 : 4, sb_size = sb_shift + 2;
	const int max_tile_width_sb = MAX_TILE_WIDTH >> sb_size;
	int max_tile_area_sb = MAX_TILE_AREA >> (2 * sb_size);
	const int min_log2_tile_cols = tile_log2(max_tile_width_sb, sb_cols);
	const int max_log2_tile_cols = tile_log2(1, sb_cols < MAX_TILE_COLS ? sb_cols : MAX_TILE_COLS);
	const int max_log2_tile_rows = tile_log2(1, sb_rows < MAX_TILE_ROWS ? sb_rows : MAX_TILE_ROWS);
	const int t = tile_log2(max_tile_area_sb, sb_rows * sb_cols);
	const int min_log2_tiles = min_log2_tile_cols > t ? min_log2_tile_cols : t;
	h->uniform = (int)f(b, 1);
	if (h->uniform) {
		h->tile_cols_log2 = min_log2_tile_cols;
		while (h->tile_cols_log2 < max_log2_tile_cols && f(b, 1)) h->tile_cols_log2++;
		const int w = (sb_cols + (1 << h->tile_cols_log2) - 1) >> h->tile_cols_log2;
		int i = 0;
		for (int start = 0; start < sb_cols; start += w) h->col_start_sb[i++] = (uint16_t)start;
		h->tile_cols = i;
		const int min_log2_tile_rows = min_log2_tiles - h->tile_cols_log2 > 0 ? min_log2_tiles - h->tile_cols_log2 : 0;
		h->tile_rows_log2 = min_log2_tile_rows;
		while (h->tile_rows_log2 < max_log2_tile_rows && f(b, 1)) h->tile_rows_log2++;
		const int hh = (sb_rows + (1 << h->tile_rows_log2) - 1) >> h->tile_rows_log2;
		i = 0;
		for (int start = 0; start < sb_rows; start += hh) h->row_start_sb[i++] = (uint16_t)start;
		h->tile_rows = i;
		for (i = 0; i < h->tile_cols; i++) h->width_sbs[i] = (uint16_t)((i < h->tile_cols - 1 ? w : sb_cols - (h->tile_cols - 1) * w) - 1);
		for (i = 0; i < h->tile_rows; i++) h->height_sbs[i] = (uint16_t)((i < h->tile_rows - 1 ? hh : sb_rows - (h->tile_rows - 1) * hh) - 1);
	} else {
		int widest = 0, start = 0, i;
		for (i = 0; start < sb_cols && i < MAX_TILE_COLS; i++) {
			h->col_start_sb[i] = (uint16_t)start;
			const int max_width = sb_cols - start < max_tile_width_sb ? sb_cols - start : max_tile_width_sb;
			h->width_sbs[i] = (uint16_t)ns(b, (uint32_t)max_width);
			const int size = h->width_sbs[i] + 1;
			if (size > widest) widest = size;
			start += size;
		}
		h->tile_cols = i;
		h->tile_cols_log2 = tile_log2(1, i);
		max_tile_area_sb = min_log2_tiles > 0 ? (sb_rows * sb_cols) >> (min_log2_tiles + 1) : sb_rows * sb_cols;
		const int max_tile_height_sb = max_tile_area_sb / widest > 1 ? max_tile_area_sb / widest : 1;
		start = 0;
		for (i = 0; start < sb_rows && i < MAX_TILE_ROWS; i++) {
			h->row_start_sb[i] = (uint16_t)start;
			const int max_height = sb_rows - start < max_tile_height_sb ? sb_rows - start : max_tile_height_sb;
			h->height_sbs[i] = (uint16_t)ns(b, (uint32_t)max_height);
			start += h->height_sbs[i] + 1;
		}
		h->tile_rows = i;
		h->tile_rows_log2 = tile_log2(1, i);
	}
	if (h->tile_cols_log2 > 0 || h->tile_rows_log2 > 0) {
		h->context_update_tile_id = (int)f(b, h->tile_rows_log2 + h->tile_cols_log2);
		h->tile_size_bytes = (int)f(b, 2) + 1;
	} else {
		h->context_update_tile_id = 0;
		h->tile_size_bytes = 1;   // tile_size_bytes_minus_1 is 0 (libavcodec passes 0)
	}
	if (h->tile_cols * h->tile_rows > AV1D_MAX_TILES) return -1;
	return 0;
}

static int delta_q(BR *b) { return f(b, 1) ? su(b, 7) : 0; }

static void quantization_params(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.12
	h->base_q_idx = (int)f(b, 8);
	h->dq_y_dc = delta_q(b);
	h->diff_uv_delta = q->color.flags.separate_uv_delta_q ? (int)f(b, 1) : 0;
	h->dq_u_dc = delta_q(b);
	h->dq_u_ac = delta_q(b);
	if (h->diff_uv_delta) { h->dq_v_dc = delta_q(b); h->dq_v_ac = delta_q(b); }
	else { h->dq_v_dc = h->dq_u_dc; h->dq_v_ac = h->dq_u_ac; }
	h->using_qmatrix = (int)f(b, 1);
	if (h->using_qmatrix) {
		h->qm_y = (int)f(b, 4);
		h->qm_u = (int)f(b, 4);
		h->qm_v = q->color.flags.separate_uv_delta_q ? (int)f(b, 4) : h->qm_u;
	}
}

static void segmentation_params(BR *b, const Av1dState *s, Hdr *h) {   // 5.9.14
	static const int bits[8] = { 8, 6, 6, 6, 6, 3, 0, 0 }, sign[8] = { 1, 1, 1, 1, 1, 0, 0, 0 };
	h->seg_enabled = (int)f(b, 1);
	memset(h->feature_enabled, 0, sizeof h->feature_enabled); memset(h->feature_value, 0, sizeof h->feature_value);
	if (!h->seg_enabled) return;
	if (h->primary_ref_frame == PRIMARY_REF_NONE) {
		h->seg_update_map = 1; h->seg_temporal_update = 0; h->seg_update_data = 1;
	} else {
		h->seg_update_map = (int)f(b, 1);
		h->seg_temporal_update = h->seg_update_map ? (int)f(b, 1) : 0;
		h->seg_update_data = (int)f(b, 1);
	}
	if (h->seg_update_data) {
		for (int i = 0; i < 8; i++)
			for (int j = 0; j < 8; j++) {
				h->feature_enabled[i][j] = (uint8_t)f(b, 1);
				if (h->feature_enabled[i][j] && bits[j]) h->feature_value[i][j] = (int16_t)(sign[j] ? su(b, 1 + bits[j]) : (int)f(b, bits[j]));
			}
	} else {   // the primary reference frame's (load_previous)
		const Av1dRef *r = &s->ref[h->ref_frame_idx[h->primary_ref_frame]];
		memcpy(h->feature_enabled, r->feature_enabled, sizeof h->feature_enabled);
		memcpy(h->feature_value, r->feature_value, sizeof h->feature_value);
	}
}

static const int8_t default_lf_ref[8] = { 1, 0, 0, 0, -1, 0, -1, -1 };

static void loop_filter_params(BR *b, const Av1dState *s, const Av1dSeq *q, Hdr *h) {   // 5.9.11
	(void)q;
	memcpy(h->lf_ref, default_lf_ref, 8); h->lf_mode[0] = h->lf_mode[1] = 0;
	if (h->coded_lossless || h->allow_intrabc) return;
	h->lf_level[0] = (int)f(b, 6);
	h->lf_level[1] = (int)f(b, 6);
	if (h->lf_level[0] || h->lf_level[1]) { h->lf_level[2] = (int)f(b, 6); h->lf_level[3] = (int)f(b, 6); }   // NumPlanes > 1 here
	h->lf_sharpness = (int)f(b, 3);
	h->lf_delta_enabled = (int)f(b, 1);
	if (!h->lf_delta_enabled) return;   // libavcodec: the defaults (see the top)
	if (h->primary_ref_frame != PRIMARY_REF_NONE) {
		const Av1dRef *r = &s->ref[h->ref_frame_idx[h->primary_ref_frame]];
		memcpy(h->lf_ref, r->lf_ref, 8); memcpy(h->lf_mode, r->lf_mode, 2);
	}
	h->lf_delta_update = (int)f(b, 1);
	if (h->lf_delta_update) {
		for (int i = 0; i < 8; i++) if (f(b, 1)) h->lf_ref[i] = (int8_t)su(b, 7);
		for (int i = 0; i < 2; i++) if (f(b, 1)) h->lf_mode[i] = (int8_t)su(b, 7);
	}
}

static void cdef_params(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.19
	if (h->coded_lossless || h->allow_intrabc || !q->std.flags.enable_cdef) return;
	h->cdef_damping_minus_3 = (int)f(b, 2);
	h->cdef_bits = (int)f(b, 2);
	for (int i = 0; i < (1 << h->cdef_bits); i++) {
		h->cdef_y_pri[i] = (uint8_t)f(b, 4); h->cdef_y_sec[i] = (uint8_t)f(b, 2);
		h->cdef_uv_pri[i] = (uint8_t)f(b, 4); h->cdef_uv_sec[i] = (uint8_t)f(b, 2);
	}
}

static void lr_params(BR *b, const Av1dSeq *q, Hdr *h) {   // 5.9.20
	if (h->all_lossless || h->allow_intrabc || !q->std.flags.enable_restoration) return;
	int uses = 0, chroma = 0;
	for (int i = 0; i < 3; i++) { h->lr_type[i] = (int)f(b, 2); if (h->lr_type[i]) { uses = 1; if (i) chroma = 1; } }
	if (!uses) return;
	if (q->std.flags.use_128x128_superblock) h->lr_unit_shift = (int)f(b, 1) + 1;
	else { h->lr_unit_shift = (int)f(b, 1); if (h->lr_unit_shift) h->lr_unit_shift += (int)f(b, 1); }
	h->lr_uv_shift = q->color.subsampling_x && q->color.subsampling_y && chroma ? (int)f(b, 1) : 0;
}

// global motion (5.9.24, 5.9.25, 5.9.26-28)
static int inverse_recenter(int r, int v) { return v > 2 * r ? v : (v & 1) ? r - ((v + 1) >> 1) : r + (v >> 1); }
static int decode_subexp(BR *b, int num_syms) {
	int i = 0, mk = 0;
	const int k = 3;
	for (;;) {
		const int b2 = i ? k + i - 1 : k;
		const int a = 1 << b2;
		if (num_syms <= mk + 3 * a) return (int)ns(b, (uint32_t)(num_syms - mk)) + mk;
		if (!f(b, 1)) return (int)f(b, b2) + mk;
		i++; mk += a;
		if (b->err || i > 24) return 0;
	}
}
static int decode_unsigned_subexp_with_ref(BR *b, int mx, int r) {
	const int v = decode_subexp(b, mx);
	return 2 * r <= mx ? inverse_recenter(r, v) : mx - 1 - inverse_recenter(mx - 1 - r, v);
}
static int decode_signed_subexp_with_ref(BR *b, int low, int high, int r) { return decode_unsigned_subexp_with_ref(b, high - low, r - low) + low; }
static void read_global_param(BR *b, Hdr *h, const int32_t prev[8][6], int type, int ref, int idx) {
	int abs_bits = GM_ABS_ALPHA_BITS, prec_bits = GM_ALPHA_PREC_BITS;
	if (idx < 2) {
		if (type == TRANSLATION) {
			abs_bits = GM_ABS_TRANS_ONLY_BITS - !h->allow_high_precision_mv;
			prec_bits = GM_TRANS_ONLY_PREC_BITS - !h->allow_high_precision_mv;
		} else { abs_bits = GM_ABS_TRANS_BITS; prec_bits = GM_TRANS_PREC_BITS; }
	}
	const int prec_diff = WARPEDMODEL_PREC_BITS - prec_bits;
	const int round = (idx % 3) == 2 ? 1 << WARPEDMODEL_PREC_BITS : 0;
	const int sub = (idx % 3) == 2 ? 1 << prec_bits : 0;
	const int mx = 1 << abs_bits;
	const int r = (prev[ref][idx] >> prec_diff) - sub;
	h->gm[ref][idx] = (int32_t)(((uint32_t)decode_signed_subexp_with_ref(b, -mx, mx + 1, r) << prec_diff) + (uint32_t)round);
}
static void gm_defaults(int32_t gm[8][6]) {
	for (int r = 0; r < 8; r++) for (int i = 0; i < 6; i++) gm[r][i] = r && i % 3 == 2 ? 1 << WARPEDMODEL_PREC_BITS : 0;
}
static void global_motion_params(BR *b, const Av1dState *s, Hdr *h, int intra) {
	gm_defaults(h->gm);
	memset(h->gm_type, 0, sizeof h->gm_type);
	if (intra) return;
	int32_t prev[8][6];
	if (h->primary_ref_frame == PRIMARY_REF_NONE) gm_defaults(prev);
	else memcpy(prev, s->ref[h->ref_frame_idx[h->primary_ref_frame]].gm, sizeof prev);
	for (int ref = LAST_FRAME; ref <= ALTREF_FRAME; ref++) {
		int type = IDENTITY;
		if (f(b, 1)) type = f(b, 1) ? ROTZOOM : f(b, 1) ? TRANSLATION : AFFINE;   // is_global, is_rot_zoom, is_translation
		h->gm_type[ref] = type;
		if (type >= ROTZOOM) {
			read_global_param(b, h, prev, type, ref, 2);
			read_global_param(b, h, prev, type, ref, 3);
			if (type == AFFINE) {
				read_global_param(b, h, prev, type, ref, 4);
				read_global_param(b, h, prev, type, ref, 5);
			} else { h->gm[ref][4] = -h->gm[ref][3]; h->gm[ref][5] = h->gm[ref][2]; }
		}
		if (type >= TRANSLATION) {
			read_global_param(b, h, prev, type, ref, 0);
			read_global_param(b, h, prev, type, ref, 1);
		}
	}
}

static void film_grain_params(BR *b, const Av1dState *s, const Av1dSeq *q, Hdr *h) {   // 5.9.30
	memset(&h->grain, 0, sizeof h->grain); h->grain_apply = 0;
	if (!q->std.flags.film_grain_params_present || (!h->show_frame && !h->showable_frame)) return;
	if (!f(b, 1)) return;   // apply_grain
	const uint16_t seed = (uint16_t)f(b, 16);
	const int update = h->frame_type == INTER_FRAME ? (int)f(b, 1) : 1;
	StdVideoAV1FilmGrain *g = &h->grain;
	if (!update) {   // load_grain_params: the referenced frame's, with the new seed
		const Av1dRef *r = &s->ref[f(b, 3)];
		h->grain_apply = r->grain_apply;
		*g = r->grain;
		g->grain_seed = seed;
		return;
	}
	h->grain_apply = 1;
	g->grain_seed = seed;
	g->num_y_points = (uint8_t)f(b, 4);
	if (g->num_y_points > 14) { b->err = 1; return; }
	for (int i = 0; i < g->num_y_points; i++) { g->point_y_value[i] = (uint8_t)f(b, 8); g->point_y_scaling[i] = (uint8_t)f(b, 8); }
	g->flags.chroma_scaling_from_luma = q->color.flags.mono_chrome ? 0 : f(b, 1);
	if (!(q->color.flags.mono_chrome || g->flags.chroma_scaling_from_luma || (q->color.subsampling_x && q->color.subsampling_y && !g->num_y_points))) {
		g->num_cb_points = (uint8_t)f(b, 4);
		if (g->num_cb_points > 10) { b->err = 1; return; }
		for (int i = 0; i < g->num_cb_points; i++) { g->point_cb_value[i] = (uint8_t)f(b, 8); g->point_cb_scaling[i] = (uint8_t)f(b, 8); }
		g->num_cr_points = (uint8_t)f(b, 4);
		if (g->num_cr_points > 10) { b->err = 1; return; }
		for (int i = 0; i < g->num_cr_points; i++) { g->point_cr_value[i] = (uint8_t)f(b, 8); g->point_cr_scaling[i] = (uint8_t)f(b, 8); }
	}
	g->grain_scaling_minus_8 = (uint8_t)f(b, 2);
	g->ar_coeff_lag = (uint8_t)f(b, 2);
	const int num_pos_luma = 2 * g->ar_coeff_lag * (g->ar_coeff_lag + 1);
	int num_pos_chroma = num_pos_luma;
	if (g->num_y_points) { num_pos_chroma = num_pos_luma + 1; for (int i = 0; i < num_pos_luma; i++) g->ar_coeffs_y_plus_128[i] = (int8_t)f(b, 8); }
	if (g->flags.chroma_scaling_from_luma || g->num_cb_points) for (int i = 0; i < num_pos_chroma; i++) g->ar_coeffs_cb_plus_128[i] = (int8_t)f(b, 8);
	if (g->flags.chroma_scaling_from_luma || g->num_cr_points) for (int i = 0; i < num_pos_chroma; i++) g->ar_coeffs_cr_plus_128[i] = (int8_t)f(b, 8);
	g->ar_coeff_shift_minus_6 = (uint8_t)f(b, 2);
	g->grain_scale_shift = (uint8_t)f(b, 2);
	if (g->num_cb_points) { g->cb_mult = (uint8_t)f(b, 8); g->cb_luma_mult = (uint8_t)f(b, 8); g->cb_offset = (uint16_t)f(b, 9); }
	if (g->num_cr_points) { g->cr_mult = (uint8_t)f(b, 8); g->cr_luma_mult = (uint8_t)f(b, 8); g->cr_offset = (uint16_t)f(b, 9); }
	g->flags.overlap_flag = f(b, 1);
	g->flags.clip_to_restricted_range = f(b, 1);
}

// libavcodec's skip_mode_params (av1dec.c): the two frames of skip mode, from the references' pictures
static void skip_mode_frames(const Av1dState *s, const Av1dSeq *q, Hdr *h) {
	h->skip_frame[0] = h->skip_frame[1] = 0;
	if (h->frame_type == KEY_FRAME || h->frame_type == INTRA_ONLY_FRAME || !h->reference_select || !q->std.flags.enable_order_hint) return;
	int fwd = -1, bwd = -1, fwd_hint = 0, bwd_hint = 0;
	for (int i = 0; i < 7; i++) {
		const Av1dRef *r = &s->ref[h->ref_frame_idx[i]];
		if (!r->pic) return;
		const int d = rel_dist(q, r->order_hint, h->order_hint);
		if (d < 0) { if (fwd < 0 || rel_dist(q, r->order_hint, fwd_hint) > 0) { fwd = i; fwd_hint = r->order_hint; } }
		else if (d > 0) { if (bwd < 0 || rel_dist(q, r->order_hint, bwd_hint) < 0) { bwd = i; bwd_hint = r->order_hint; } }
	}
	if (fwd < 0) return;
	if (bwd >= 0) { h->skip_frame[0] = (uint8_t)(LAST_FRAME + (fwd < bwd ? fwd : bwd)); h->skip_frame[1] = (uint8_t)(LAST_FRAME + (fwd > bwd ? fwd : bwd)); return; }
	int fwd2 = -1, fwd2_hint = 0;
	for (int i = 0; i < 7; i++) {
		const int hint = s->ref[h->ref_frame_idx[i]].order_hint;
		if (rel_dist(q, hint, fwd_hint) < 0 && (fwd2 < 0 || rel_dist(q, hint, fwd2_hint) > 0)) { fwd2 = i; fwd2_hint = hint; }
	}
	if (fwd2 < 0) return;
	h->skip_frame[0] = (uint8_t)(LAST_FRAME + (fwd < fwd2 ? fwd : fwd2));
	h->skip_frame[1] = (uint8_t)(LAST_FRAME + (fwd > fwd2 ? fwd : fwd2));
}

// skip_mode_present (5.9.22): whether it is coded, from the reference slots' order hints
static int skip_mode_allowed(const Av1dState *s, const Av1dSeq *q, const Hdr *h) {
	if (h->frame_type == KEY_FRAME || h->frame_type == INTRA_ONLY_FRAME || !h->reference_select || !q->std.flags.enable_order_hint) return 0;
	int fwd = -1, bwd = -1, fwd_hint = 0, bwd_hint = 0;
	for (int i = 0; i < 7; i++) {
		const int hint = s->ref[h->ref_frame_idx[i]].order_hint;
		const int d = rel_dist(q, hint, h->order_hint);
		if (d < 0) { if (fwd < 0 || rel_dist(q, hint, fwd_hint) > 0) { fwd = i; fwd_hint = hint; } }
		else if (d > 0) { if (bwd < 0 || rel_dist(q, hint, bwd_hint) < 0) { bwd = i; bwd_hint = hint; } }
	}
	if (fwd < 0) return 0;
	if (bwd >= 0) return 1;
	for (int i = 0; i < 7; i++) if (rel_dist(q, s->ref[h->ref_frame_idx[i]].order_hint, fwd_hint) < 0) return 1;
	return 0;
}

// uncompressed_header (5.9.2). 0, or AV1D_ERROR / AV1D_UNSUPPORTED (s->why)
static int uncompressed_header(Av1dState *s, BR *b, const Av1dSeq *q, Hdr *h, int temporal_id, int spatial_id) {
	memset(h, 0, sizeof *h);
	const StdVideoAV1SequenceHeader *d = &q->std;
	const int id_len = d->flags.frame_id_numbers_present_flag ? d->additional_frame_id_length_minus_1 + d->delta_frame_id_length_minus_2 + 3 : 0;
	const int all_frames = (1 << NUM_REF_FRAMES) - 1;
	const int order_hint_bits = d->flags.enable_order_hint ? d->order_hint_bits_minus_1 + 1 : 0;
	int intra;
	if (d->flags.reduced_still_picture_header) {
		h->frame_type = KEY_FRAME; h->show_frame = 1; h->showable_frame = 0; intra = 1;
	} else {
		h->show_existing_frame = (int)f(b, 1);
		if (h->show_existing_frame) {
			h->frame_to_show_map_idx = (int)f(b, 3);
			if (q->decoder_model_info_present && !q->timing.flags.equal_picture_interval) f(b, q->frame_presentation_time_length);
			if (d->flags.frame_id_numbers_present_flag) f(b, id_len);   // display_frame_id
			const Av1dRef *r = &s->ref[h->frame_to_show_map_idx];
			if (!r->valid) return fail(s, AV1D_ERROR, "show_existing_frame of an empty reference slot");
			h->frame_type = r->frame_type;
			h->refresh_frame_flags = h->frame_type == KEY_FRAME ? all_frames : 0;
			return b->err ? fail(s, AV1D_ERROR, "frame header: truncated") : 0;
		}
		h->frame_type = (int)f(b, 2);
		intra = h->frame_type == INTRA_ONLY_FRAME || h->frame_type == KEY_FRAME;
		h->show_frame = (int)f(b, 1);
		if (h->show_frame && q->decoder_model_info_present && !q->timing.flags.equal_picture_interval) f(b, q->frame_presentation_time_length);
		h->showable_frame = h->show_frame ? h->frame_type != KEY_FRAME : (int)f(b, 1);
		h->error_resilient_mode = h->frame_type == SWITCH_FRAME || (h->frame_type == KEY_FRAME && h->show_frame) ? 1 : (int)f(b, 1);
	}
	if (h->frame_type == KEY_FRAME && h->show_frame)
		for (int i = 0; i < NUM_REF_FRAMES; i++) s->ref[i].valid = 0;   // (their order hints: see the top)
	h->disable_cdf_update = (int)f(b, 1);
	h->allow_screen_content_tools = d->seq_force_screen_content_tools == SELECT_SCREEN_CONTENT_TOOLS ? (int)f(b, 1) : d->seq_force_screen_content_tools;
	if (h->allow_screen_content_tools) h->force_integer_mv = d->seq_force_integer_mv == SELECT_INTEGER_MV ? (int)f(b, 1) : d->seq_force_integer_mv;
	if (d->flags.frame_id_numbers_present_flag) {
		h->current_frame_id = (int)f(b, id_len);
		const int diff_len = d->delta_frame_id_length_minus_2 + 2;   // mark references that are too old (7.20 / CBS)
		for (int i = 0; i < NUM_REF_FRAMES; i++) {
			const int fid = (int)s->ref[i].frame_id;
			if (h->current_frame_id > (1 << diff_len)) {
				if (fid > h->current_frame_id || fid < h->current_frame_id - (1 << diff_len)) s->ref[i].valid = 0;
			} else if (fid > h->current_frame_id && fid < (1 << id_len) + h->current_frame_id - (1 << diff_len)) s->ref[i].valid = 0;
		}
	}
	h->frame_size_override_flag = h->frame_type == SWITCH_FRAME ? 1 : d->flags.reduced_still_picture_header ? 0 : (int)f(b, 1);
	h->order_hint = order_hint_bits ? (int)f(b, order_hint_bits) : 0;
	h->primary_ref_frame = intra || h->error_resilient_mode ? PRIMARY_REF_NONE : (int)f(b, 3);
	if (q->decoder_model_info_present) {
		h->buffer_removal_time_present_flag = (int)f(b, 1);
		if (h->buffer_removal_time_present_flag)
			for (int i = 0; i < q->ops; i++) {
				if (!q->decoder_model_present_for_op[i]) continue;
				const int idc = q->op_idc[i];
				if (!idc || (((idc >> temporal_id) & 1) && ((idc >> (spatial_id + 8)) & 1))) f(b, q->buffer_removal_time_length);
			}
	}
	h->refresh_frame_flags = h->frame_type == SWITCH_FRAME || (h->frame_type == KEY_FRAME && h->show_frame) ? all_frames : (int)f(b, 8);
	if ((!intra || h->refresh_frame_flags != all_frames) && h->error_resilient_mode && d->flags.enable_order_hint)
		for (int i = 0; i < NUM_REF_FRAMES; i++) if ((int)f(b, order_hint_bits) != s->ref[i].order_hint) s->ref[i].valid = 0;   // ref_order_hint[i]
	if (intra) {
		frame_size(b, q, h);
		render_size(b, h);
		if (h->allow_screen_content_tools && h->upscaled_width == h->frame_width) h->allow_intrabc = (int)f(b, 1);
		// libavcodec's frame header has ref_frame_idx[] = 0 here (see the top)
	} else {
		if (d->flags.enable_order_hint) {
			h->frame_refs_short_signaling = (int)f(b, 1);
			if (h->frame_refs_short_signaling) {
				const int last = (int)f(b, 3), gold = (int)f(b, 3);
				set_frame_refs(s, q, h, last, gold);
			}
		}
		for (int i = 0; i < REFS_PER_FRAME; i++) {
			if (!h->frame_refs_short_signaling) h->ref_frame_idx[i] = (int)f(b, 3);
			if (d->flags.frame_id_numbers_present_flag) f(b, d->delta_frame_id_length_minus_2 + 2);   // delta_frame_id_minus_1
		}
		if (h->frame_size_override_flag && !h->error_resilient_mode) {   // frame_size_with_refs (5.9.7)
			int found = 0;
			for (int i = 0; i < REFS_PER_FRAME && !found; i++) {
				if (!f(b, 1)) continue;
				const Av1dRef *r = &s->ref[h->ref_frame_idx[i]];
				if (!r->valid) return fail(s, AV1D_ERROR, "frame size from a reference slot that is empty");
				h->upscaled_width = r->upscaled_width; h->frame_width = h->upscaled_width; h->frame_height = r->frame_height;
				h->render_width = r->render_width; h->render_height = r->render_height;
				found = 1;
			}
			if (!found) { frame_size(b, q, h); render_size(b, h); }
			else { superres_params(b, q, h); compute_image_size(h); }
		} else { frame_size(b, q, h); render_size(b, h); }
		h->allow_high_precision_mv = h->force_integer_mv ? 0 : (int)f(b, 1);
		h->is_filter_switchable = (int)f(b, 1);
		h->interpolation_filter = h->is_filter_switchable ? STD_VIDEO_AV1_INTERPOLATION_FILTER_SWITCHABLE : (int)f(b, 2);
		h->is_motion_mode_switchable = (int)f(b, 1);
		h->use_ref_frame_mvs = h->error_resilient_mode || !d->flags.enable_ref_frame_mvs ? 0 : (int)f(b, 1);
	}
	// libavcodec's order_hint_info (av1dec.c): the references' order hints and sign bias, intra frames included
	h->order_hints[0] = 0; h->sign_bias = 0;
	for (int i = 0; i < REFS_PER_FRAME; i++) {
		const int hint = s->ref[h->ref_frame_idx[i]].order_hint;
		h->order_hints[LAST_FRAME + i] = (uint8_t)hint;
		if (d->flags.enable_order_hint && rel_dist(q, hint, h->order_hint) > 0) h->sign_bias |= (uint8_t)(1 << (LAST_FRAME + i));
	}
	h->disable_frame_end_update_cdf = d->flags.reduced_still_picture_header || h->disable_cdf_update ? 1 : (int)f(b, 1);
	if (tile_info(b, q, h) < 0) return fail(s, AV1D_UNSUPPORTED, "more than 256 tiles");
	quantization_params(b, q, h);
	segmentation_params(b, s, h);
	h->delta_q_present = h->base_q_idx > 0 ? (int)f(b, 1) : 0;
	if (h->delta_q_present) h->delta_q_res = (int)f(b, 2);
	if (h->delta_q_present) {
		h->delta_lf_present = h->allow_intrabc ? 0 : (int)f(b, 1);
		if (h->delta_lf_present) { h->delta_lf_res = (int)f(b, 2); h->delta_lf_multi = (int)f(b, 1); }
	}
	h->coded_lossless = 1;
	for (int i = 0; i < 8; i++) {
		int qindex = h->base_q_idx;
		if (h->seg_enabled && h->feature_enabled[i][0]) { qindex += h->feature_value[i][0]; qindex = qindex < 0 ? 0 : qindex > 255 ? 255 : qindex; }
		if (qindex || h->dq_y_dc || h->dq_u_ac || h->dq_u_dc || h->dq_v_ac || h->dq_v_dc) h->coded_lossless = 0;
	}
	h->all_lossless = h->coded_lossless && h->frame_width == h->upscaled_width;
	loop_filter_params(b, s, q, h);
	cdef_params(b, q, h);
	lr_params(b, q, h);
	h->tx_mode = h->coded_lossless ? STD_VIDEO_AV1_TX_MODE_ONLY_4X4 : f(b, 1) ? STD_VIDEO_AV1_TX_MODE_SELECT : STD_VIDEO_AV1_TX_MODE_LARGEST;
	h->reference_select = intra ? 0 : (int)f(b, 1);
	h->skip_mode_present = skip_mode_allowed(s, q, h) ? (int)f(b, 1) : 0;
	h->allow_warped_motion = intra || h->error_resilient_mode || !d->flags.enable_warped_motion ? 0 : (int)f(b, 1);
	h->reduced_tx_set = (int)f(b, 1);
	global_motion_params(b, s, h, intra);
	film_grain_params(b, s, q, h);
	skip_mode_frames(s, q, h);
	if (b->err) return fail(s, AV1D_ERROR, "frame header: truncated");
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// The frame for Vulkan Video, and the reference frame update (7.20)

static StdVideoDecodeAV1ReferenceInfo ref_info(const Av1dRef *r, int with_saved) {
	StdVideoDecodeAV1ReferenceInfo i = { .flags = { .disable_frame_end_update_cdf = (uint32_t)r->disable_frame_end_update_cdf,
	                                                .segmentation_enabled = (uint32_t)r->segmentation_enabled },
	                                     .frame_type = (uint8_t)r->frame_type, .RefFrameSignBias = r->sign_bias, .OrderHint = (uint8_t)r->order_hint };
	if (with_saved) memcpy(i.SavedOrderHints, r->order_hints, 8);
	return i;
}

// the frame's state as a reference slot will hold it
static Av1dRef as_ref(const Hdr *h, int bit_depth) {
	Av1dRef r = { .valid = 1, .frame_id = (uint32_t)h->current_frame_id, .upscaled_width = h->upscaled_width,
	              .frame_width = h->frame_width, .frame_height = h->frame_height, .render_width = h->render_width,
	              .render_height = h->render_height, .frame_type = h->frame_type, .order_hint = h->order_hint,
	              .showable = h->showable_frame, .sign_bias = h->sign_bias, .disable_frame_end_update_cdf = h->disable_frame_end_update_cdf,
	              .segmentation_enabled = h->seg_enabled, .grain_apply = h->grain_apply, .grain = h->grain, .bit_depth = bit_depth, .dpb = -1 };
	memcpy(r.order_hints, h->order_hints, 8);
	memcpy(r.lf_ref, h->lf_ref, 8); memcpy(r.lf_mode, h->lf_mode, 2);
	memcpy(r.feature_enabled, h->feature_enabled, sizeof r.feature_enabled);
	memcpy(r.feature_value, h->feature_value, sizeof r.feature_value);
	memcpy(r.gm, h->gm, sizeof r.gm);
	return r;
}

// fills the frame's Std structures from the header (libavcodec's vk_av1_start_frame values)
static void describe(Av1dState *s, const Av1dSeq *q, const Hdr *h, Av1dFrame *fr) {
	fr->seq = *q; av1d_seq_fix(&fr->seq);
	const int sb_shift = q->std.flags.use_128x128_superblock ? 5 : 4;
	fr->tile = (StdVideoAV1TileInfo){ .flags = { .uniform_tile_spacing_flag = (uint32_t)h->uniform }, .TileCols = (uint8_t)h->tile_cols,
		.TileRows = (uint8_t)h->tile_rows, .context_update_tile_id = (uint16_t)h->context_update_tile_id,
		.tile_size_bytes_minus_1 = (uint8_t)(h->tile_size_bytes - 1),
		.pMiColStarts = fr->mi_col_starts, .pMiRowStarts = fr->mi_row_starts, .pWidthInSbsMinus1 = fr->width_sbs, .pHeightInSbsMinus1 = fr->height_sbs };
	memset(fr->mi_col_starts, 0, sizeof fr->mi_col_starts); memset(fr->mi_row_starts, 0, sizeof fr->mi_row_starts);
	memset(fr->width_sbs, 0, sizeof fr->width_sbs); memset(fr->height_sbs, 0, sizeof fr->height_sbs);
	for (int i = 0; i < h->tile_cols; i++) { fr->mi_col_starts[i] = (uint16_t)(h->col_start_sb[i] << sb_shift); fr->width_sbs[i] = h->width_sbs[i]; }
	for (int i = 0; i < h->tile_rows; i++) { fr->mi_row_starts[i] = (uint16_t)(h->row_start_sb[i] << sb_shift); fr->height_sbs[i] = h->height_sbs[i]; }
	fr->quant = (StdVideoAV1Quantization){ .flags = { .using_qmatrix = (uint32_t)h->using_qmatrix, .diff_uv_delta = (uint32_t)h->diff_uv_delta },
		.base_q_idx = (uint8_t)h->base_q_idx, .DeltaQYDc = (int8_t)h->dq_y_dc, .DeltaQUDc = (int8_t)h->dq_u_dc, .DeltaQUAc = (int8_t)h->dq_u_ac,
		.DeltaQVDc = (int8_t)h->dq_v_dc, .DeltaQVAc = (int8_t)h->dq_v_ac, .qm_y = (uint8_t)h->qm_y, .qm_u = (uint8_t)h->qm_u, .qm_v = (uint8_t)h->qm_v };
	memset(&fr->seg, 0, sizeof fr->seg);
	for (int i = 0; i < 8; i++)
		for (int j = 0; j < 8; j++) { fr->seg.FeatureEnabled[i] |= (uint8_t)(h->feature_enabled[i][j] << j); fr->seg.FeatureData[i][j] = h->feature_value[i][j]; }
	fr->lf = (StdVideoAV1LoopFilter){ .flags = { .loop_filter_delta_enabled = (uint32_t)h->lf_delta_enabled, .loop_filter_delta_update = (uint32_t)h->lf_delta_update },
		.loop_filter_sharpness = (uint8_t)h->lf_sharpness };
	for (int i = 0; i < 4; i++) fr->lf.loop_filter_level[i] = (uint8_t)h->lf_level[i];
	memcpy(fr->lf.loop_filter_ref_deltas, h->lf_ref, 8); memcpy(fr->lf.loop_filter_mode_deltas, h->lf_mode, 2);
	fr->cdef = (StdVideoAV1CDEF){ .cdef_damping_minus_3 = (uint8_t)h->cdef_damping_minus_3, .cdef_bits = (uint8_t)h->cdef_bits };
	memcpy(fr->cdef.cdef_y_pri_strength, h->cdef_y_pri, 8); memcpy(fr->cdef.cdef_y_sec_strength, h->cdef_y_sec, 8);
	memcpy(fr->cdef.cdef_uv_pri_strength, h->cdef_uv_pri, 8); memcpy(fr->cdef.cdef_uv_sec_strength, h->cdef_uv_sec, 8);
	static const StdVideoAV1FrameRestorationType remap[4] = { STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_NONE, STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_SWITCHABLE,
		STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_WIENER, STD_VIDEO_AV1_FRAME_RESTORATION_TYPE_SGRPROJ };
	for (int i = 0; i < 3; i++) fr->lr.FrameRestorationType[i] = remap[h->lr_type[i] & 3];
	fr->lr.LoopRestorationSize[0] = (uint16_t)(1 + h->lr_unit_shift);   // log2(size) - 5, as libavcodec gives it
	fr->lr.LoopRestorationSize[1] = fr->lr.LoopRestorationSize[2] = (uint16_t)(1 + h->lr_unit_shift - h->lr_uv_shift);
	memset(&fr->gm, 0, sizeof fr->gm);
	for (int i = 1; i < 8; i++) { fr->gm.GmType[i] = (uint8_t)h->gm_type[i]; memcpy(fr->gm.gm_params[i], h->gm[i], sizeof h->gm[i]); }
	fr->apply_grain = h->grain_apply;
	fr->grain = h->grain;
	fr->grain.flags.update_grain = 0;
	const int intra = h->frame_type == KEY_FRAME || h->frame_type == INTRA_ONLY_FRAME;
	fr->std = (StdVideoDecodeAV1PictureInfo){
		.flags = { .error_resilient_mode = (uint32_t)h->error_resilient_mode, .disable_cdf_update = (uint32_t)h->disable_cdf_update,
			.use_superres = (uint32_t)h->use_superres, .render_and_frame_size_different = (uint32_t)h->render_and_frame_size_different,
			.allow_screen_content_tools = (uint32_t)h->allow_screen_content_tools, .is_filter_switchable = (uint32_t)h->is_filter_switchable,
			.force_integer_mv = (uint32_t)(h->force_integer_mv || intra), .frame_size_override_flag = (uint32_t)h->frame_size_override_flag,
			.buffer_removal_time_present_flag = (uint32_t)h->buffer_removal_time_present_flag, .allow_intrabc = (uint32_t)h->allow_intrabc,
			.frame_refs_short_signaling = (uint32_t)h->frame_refs_short_signaling, .allow_high_precision_mv = (uint32_t)h->allow_high_precision_mv,
			.is_motion_mode_switchable = (uint32_t)h->is_motion_mode_switchable, .use_ref_frame_mvs = (uint32_t)h->use_ref_frame_mvs,
			.disable_frame_end_update_cdf = (uint32_t)h->disable_frame_end_update_cdf, .allow_warped_motion = (uint32_t)h->allow_warped_motion,
			.reduced_tx_set = (uint32_t)h->reduced_tx_set, .reference_select = (uint32_t)h->reference_select,
			.skip_mode_present = (uint32_t)h->skip_mode_present, .delta_q_present = (uint32_t)h->delta_q_present,
			.delta_lf_present = (uint32_t)h->delta_lf_present, .delta_lf_multi = (uint32_t)h->delta_lf_multi,
			.segmentation_enabled = (uint32_t)h->seg_enabled, .segmentation_update_map = (uint32_t)h->seg_update_map,
			.segmentation_temporal_update = (uint32_t)h->seg_temporal_update, .segmentation_update_data = (uint32_t)h->seg_update_data,
			.UsesLr = (uint32_t)(h->lr_type[0] || h->lr_type[1] || h->lr_type[2]), .apply_grain = (uint32_t)h->grain_apply },
		.frame_type = (StdVideoAV1FrameType)h->frame_type, .current_frame_id = (uint32_t)h->current_frame_id, .OrderHint = (uint8_t)h->order_hint,
		.primary_ref_frame = (uint8_t)h->primary_ref_frame, .refresh_frame_flags = (uint8_t)h->refresh_frame_flags,
		.interpolation_filter = (StdVideoAV1InterpolationFilter)h->interpolation_filter, .TxMode = (StdVideoAV1TxMode)h->tx_mode,
		.delta_q_res = (uint8_t)h->delta_q_res, .delta_lf_res = (uint8_t)h->delta_lf_res,
		.SkipModeFrame = { h->skip_frame[0], h->skip_frame[1] }, .coded_denom = (uint8_t)h->coded_denom,
		.pTileInfo = &fr->tile, .pQuantization = &fr->quant, .pSegmentation = &fr->seg, .pLoopFilter = &fr->lf, .pCDEF = &fr->cdef,
		.pLoopRestoration = &fr->lr, .pGlobalMotion = &fr->gm, .pFilmGrain = h->grain_apply ? &fr->grain : NULL };
	memcpy(fr->std.OrderHints, h->order_hints, 8);
	// the current picture as a reference, and its DPB slot: the lowest one no reference slot holds (vulkan_av1.c)
	const Av1dRef cur = as_ref(h, q->bit_depth);
	fr->setup = ref_info(&cur, 0);
	const unsigned busy = av1d_dpb_busy(s);
	fr->dpb = 0;
	while (busy & (1u << fr->dpb)) fr->dpb++;
	fr->gen = ++s->gen;
	// the references: the slots ref_frame_idx[] names, once each (by DPB slot), those with a picture
	fr->nrefs = 0;
	int missing = 0;
	for (int i = 0; i < REFS_PER_FRAME; i++) {
		const Av1dRef *r = &s->ref[h->ref_frame_idx[i]];
		fr->ref_frame_idx[i] = h->ref_frame_idx[i];
		if (!r->pic) { fr->name_dpb[i] = -1; missing = 1; continue; }
		fr->name_dpb[i] = r->dpb;
		int dup = 0;
		for (int j = 0; j < fr->nrefs; j++) if (fr->refs[j].dpb == r->dpb) dup = 1;
		if (dup) continue;
		fr->refs[fr->nrefs].dpb = r->dpb; fr->refs[fr->nrefs].slot = h->ref_frame_idx[i];
		fr->refs[fr->nrefs].info = ref_info(r, 1);
		fr->nrefs++;
	}
	fr->missing = !intra && missing;
	fr->width = h->upscaled_width; fr->height = h->frame_height; fr->frame_width = h->frame_width;
	fr->refresh = h->refresh_frame_flags;
	fr->shown = h->show_frame;
}

static void refresh(Av1dState *s, const Hdr *h, const Av1dSeq *q, const Av1dFrame *fr) {
	const Av1dRef r = as_ref(h, q->bit_depth);
	for (int i = 0; i < NUM_REF_FRAMES; i++) {
		if (!(h->refresh_frame_flags & (1 << i))) continue;
		s->ref[i] = r;
		s->ref[i].pic = !fr->missing;
		s->ref[i].dpb = fr->missing ? -1 : fr->dpb;
		s->ref[i].gen = fr->gen;
		s->ref[i].grain_dpb = fr->apply_grain;
	}
}

// ---------------------------------------------------------------------------------------------------------------------
// Samples

typedef struct { int type, temporal_id, spatial_id, has_ext; const uint8_t *p; size_t n; size_t off; } Obu;

// the next OBU of p[0..n) at *pos (5.3): 1, 0 at the end, -1 if malformed
static int next_obu(const uint8_t *p, size_t n, size_t *pos, Obu *o) {
	if (*pos >= n) return 0;
	const uint8_t *q = p + *pos;
	size_t left = n - *pos, hl = 1;
	if (q[0] & 0x80) return -1;   // obu_forbidden_bit
	o->type = (q[0] >> 3) & 15;
	o->has_ext = (q[0] >> 2) & 1;
	const int has_size = (q[0] >> 1) & 1;
	o->temporal_id = o->spatial_id = 0;
	if (o->has_ext) {
		if (left < 2) return -1;
		o->temporal_id = q[1] >> 5; o->spatial_id = (q[1] >> 3) & 3; hl = 2;
	}
	uint64_t size;
	if (has_size) {
		size_t len;
		if (leb128(q + hl, left - hl, &size, &len) < 0) return -1;
		hl += len;
	} else size = left - hl;
	if (hl > left || size > left - hl) return -1;
	o->p = q + hl; o->n = (size_t)size; o->off = *pos + hl;
	*pos += hl + (size_t)size;
	return 1;
}

static int take_sequence_header(Av1dState *s, const Obu *o) {
	Av1dSeq q;
	const int r = parse_sequence_header(s, o->p, o->n, &q);
	if (r < 0) return r;
	if (s->have_seq && q.rawn == s->seq.rawn && !memcmp(q.raw, s->seq.raw, q.rawn)) return 0;   // a repeat
	s->seq = q; av1d_seq_fix(&s->seq); s->have_seq = 1;
	s->op_idc = q.op_idc[0];
	if (q.unsupported) { snprintf(s->why, sizeof s->why, "sequence header: %s", seq_unsupported(q.unsupported)); return AV1D_UNSUPPORTED; }
	return 0;
}

int av1d_parse_av1c(Av1dState *s, const uint8_t *c, size_t n) {
	if (n < 4 || c[0] != 0x81) return fail(s, AV1D_ERROR, "av1C: not version 1");
	size_t pos = 4;
	Obu o;
	int r;
	while ((r = next_obu(c, n, &pos, &o)) > 0)
		if (o.type == OBU_SEQUENCE_HEADER && (r = take_sequence_header(s, &o)) < 0) return r;
	if (r < 0) return fail(s, AV1D_ERROR, "av1C: malformed configOBUs");
	return AV1D_NO_PICTURE;
}

// a tile group (5.11.1) of the frame: its tiles' offsets in the sample
static int tile_group(Av1dState *s, const Hdr *h, const uint8_t *data, const Obu *o, size_t start, Av1dFrame *fr, int *next_tile) {
	BR b; br_init(&b, o->p, o->n);
	b.pos = start * 8;
	const int num = h->tile_cols * h->tile_rows;
	int tg_start = 0, tg_end = num - 1;
	if (num > 1 && f(&b, 1)) {
		const int bits = h->tile_cols_log2 + h->tile_rows_log2;
		tg_start = (int)f(&b, bits); tg_end = (int)f(&b, bits);
	}
	byte_alignment(&b);
	if (b.err || tg_start != *next_tile || tg_end < tg_start || tg_end >= num) return fail(s, AV1D_ERROR, "tile group: bad tile range");
	size_t pos = b.pos / 8, end = o->n;
	for (int t = tg_start; t <= tg_end; t++) {
		size_t size;
		if (t == tg_end) size = end - pos;
		else {
			if (end - pos < (size_t)h->tile_size_bytes) return fail(s, AV1D_ERROR, "tile group: truncated");
			size = 0;
			for (int i = 0; i < h->tile_size_bytes; i++) size |= (size_t)o->p[pos + (size_t)i] << (8 * i);
			pos += (size_t)h->tile_size_bytes;
			if (end - pos <= size) return fail(s, AV1D_ERROR, "tile group: tile larger than the OBU");
			size++;
		}
		if (!size) return fail(s, AV1D_ERROR, "tile group: empty tile");
		fr->tile_src[fr->ntiles] = (uint32_t)(o->p + pos - data);
		fr->tile_size[fr->ntiles] = (uint32_t)size;
		fr->ntiles++; fr->size += size;
		pos += size;
	}
	fr->ntg++;
	*next_tile = tg_end + 1;
	return 0;
}

int av1d_parse_sample(Av1dState *s, const uint8_t *data, size_t n, Av1dSample *out) {
	static __thread Av1dState saved;   // the state before the sample, put back when it fails
	saved = *s;
	out->nframes = 0; out->shown = -1; out->extra_shown = 0;
	size_t pos = 0;
	Obu o;
	int r, seen_header = 0, next_tile = 0;
	Hdr h;
	Av1dFrame *fr = NULL;
	while ((r = next_obu(data, n, &pos, &o)) > 0) {
		// OBUs of layers outside operating point 0 are dropped (as CBS drops them)
		if (o.has_ext && s->op_idc && o.type != OBU_SEQUENCE_HEADER && o.type != OBU_TEMPORAL_DELIMITER &&
		    (!((s->op_idc >> o.temporal_id) & 1) || !((s->op_idc >> (o.spatial_id + 8)) & 1))) continue;
		switch (o.type) {
		case OBU_SEQUENCE_HEADER:
			if ((r = take_sequence_header(s, &o)) < 0) goto bad;
			break;
		case OBU_TEMPORAL_DELIMITER:
			seen_header = 0;
			break;
		case OBU_REDUNDANT_FRAME_HEADER:
			if (seen_header) break;
			// fall through: used as the frame header
		case OBU_FRAME_HEADER:
		case OBU_FRAME: {
			if (seen_header) { r = fail(s, AV1D_ERROR, "a second frame header before the frame's tiles"); goto bad; }
			if (!s->have_seq) { r = fail(s, AV1D_ERROR, "a frame before any sequence header"); goto bad; }
			if (out->nframes == AV1D_MAX_FRAMES) { r = fail(s, AV1D_UNSUPPORTED, "more than 16 frames in a sample"); goto bad; }
			BR b; br_init(&b, o.p, o.n);
			if ((r = uncompressed_header(s, &b, &s->seq, &h, o.temporal_id, o.spatial_id)) < 0) goto bad;
			fr = &out->f[out->nframes++];
			memset(fr, 0, sizeof *fr);
			fr->hdr_bits = (int)b.pos;
			if (h.show_existing_frame) {
				const Av1dRef *shown = &s->ref[h.frame_to_show_map_idx];
				fr->show_existing = 1; fr->show_slot = h.frame_to_show_map_idx; fr->shown = 1;
				fr->dpb = shown->pic ? shown->dpb : -1; fr->gen = shown->gen; fr->missing = !shown->pic;
				fr->width = shown->upscaled_width; fr->height = shown->frame_height; fr->frame_width = shown->frame_width;
				fr->apply_grain = shown->grain_apply;
				fr->refresh = h.refresh_frame_flags;
				if (h.frame_type == KEY_FRAME) {   // 7.21: the key frame is loaded and refreshes every slot
					const Av1dRef k = *shown;
					for (int i = 0; i < NUM_REF_FRAMES; i++) s->ref[i] = k;
					s->keyframes++;
				}
				if (out->shown < 0) out->shown = out->nframes - 1; else out->extra_shown++;
				fr = NULL;
				break;
			}
			describe(s, &s->seq, &h, fr);
			if (fr->missing) s->missing_refs++;
			if (h.frame_type == KEY_FRAME) s->keyframes++;
			seen_header = 1; next_tile = 0;
			if (o.type != OBU_FRAME) break;
			byte_alignment(&b);
			if (b.err) { r = fail(s, AV1D_ERROR, "frame: truncated"); goto bad; }
			if ((r = tile_group(s, &h, data, &o, b.pos / 8, fr, &next_tile)) < 0) goto bad;
			goto tiles_done;
		}
		case OBU_TILE_GROUP:
			if (!seen_header || !fr) { r = fail(s, AV1D_ERROR, "a tile group without a frame header"); goto bad; }
			if ((r = tile_group(s, &h, data, &o, 0, fr, &next_tile)) < 0) goto bad;
		tiles_done:
			if (next_tile == h.tile_cols * h.tile_rows) {   // the frame is complete: decode_frame_wrapup
				refresh(s, &h, &s->seq, fr);
				if (h.show_frame) { if (out->shown < 0) out->shown = out->nframes - 1; else out->extra_shown++; }
				seen_header = 0; fr = NULL;
			}
			break;
		case OBU_TILE_LIST:
			r = fail(s, AV1D_UNSUPPORTED, "large scale tile (tile list OBU)");
			goto bad;
		default:   // metadata, padding, reserved
			break;
		}
	}
	if (r < 0) { r = fail(s, AV1D_ERROR, "malformed OBU"); goto bad; }
	if (fr) { r = fail(s, AV1D_ERROR, "a frame without all its tiles"); goto bad; }
	return out->nframes ? AV1D_PICTURE : AV1D_NO_PICTURE;
bad:
	{
		char why[160];
		memcpy(why, s->why, sizeof why);
		const uint64_t damaged = s->damaged + (r == AV1D_ERROR);
		*s = saved;
		memcpy(s->why, why, sizeof why);
		s->damaged = damaged;
	}
	out->nframes = 0; out->shown = -1;
	return r;
}

int av1d_is_key_sample(const uint8_t *data, size_t n) {
	size_t pos = 0;
	Obu o;
	int reduced = 0;
	while (next_obu(data, n, &pos, &o) > 0) {
		if (o.type == OBU_SEQUENCE_HEADER && o.n) reduced = (o.p[0] >> 3) & 1;   // reduced_still_picture_header
		if (o.type != OBU_FRAME_HEADER && o.type != OBU_FRAME && o.type != OBU_REDUNDANT_FRAME_HEADER) continue;
		if (reduced) return 1;
		BR b; br_init(&b, o.p, o.n);
		if (f(&b, 1)) return 0;   // show_existing_frame (of a key frame: libavcodec has no picture for it)
		const int type = (int)f(&b, 2), show = (int)f(&b, 1);
		return !b.err && type == KEY_FRAME && show;
	}
	return 0;
}
