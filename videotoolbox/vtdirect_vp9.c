// SPDX-License-Identifier: MIT
// vtdirect_vp9.c: VP9 superframes and uncompressed headers for the direct Vulkan Video decode path (see
// vtdirect_vp9.h for what it follows and why). Written from the VP9 specification (6.2 uncompressed header syntax,
// 7.2 semantics, 8.4.1 / 8.10 reference updates, Annex B superframes); FFmpeg and Mesa read for their behaviour only.
#include "vtdirect_vp9.h"
#include <stdio.h>
#include <string.h>

typedef struct { const uint8_t *p; size_t nbits, pos; int over; } Bits;
static uint32_t rb(Bits *b, int n) {
	uint32_t v = 0;
	for (int i = 0; i < n; i++) {
		if (b->pos >= b->nbits) { b->over = 1; return 0; }
		v = v << 1 | ((b->p[b->pos >> 3] >> (7 - (b->pos & 7))) & 1);
		b->pos++;
	}
	return v;
}
static int rs(Bits *b, int n) { const int v = (int)rb(b, n); return rb(b, 1) ? -v : v; }   // su(n): magnitude, then sign

static int fail(Vp9dState *s, int r, const char *w) { snprintf(s->why, sizeof s->why, "%s", w); return r; }
const char *vp9d_why(const Vp9dState *s) { return s->why; }

static void past_independence(Vp9dState *s) {
	static const int8_t ref[4] = { 1, 0, -1, -1 };
	memcpy(s->lf_ref, ref, sizeof ref);
	s->lf_mode[0] = s->lf_mode[1] = 0;
	memset(s->feat_en, 0, sizeof s->feat_en); memset(s->feat_val, 0, sizeof s->feat_val);
	s->abs_delta = 0;
}

void vp9d_init(Vp9dState *s, int profile) {
	memset(s, 0, sizeof *s);
	s->profile = profile;
	past_independence(s);
	for (int i = 0; i < 8; i++) s->ref[i].dpb = -1;
}

void vp9d_frame_fix(Vp9dFrame *f) { f->std.pColorConfig = &f->color; f->std.pLoopFilter = &f->lf; f->std.pSegmentation = &f->seg; }

int vp9d_full_range(const Vp9dState *s) { return s->color_known ? s->color_range : -1; }

unsigned vp9d_dpb_busy(const Vp9dState *s) {
	unsigned m = 0;
	for (int i = 0; i < 8; i++) if (s->ref[i].pic && s->ref[i].dpb >= 0 && s->ref[i].dpb < 32) m |= 1u << s->ref[i].dpb;
	return m;
}

void vp9d_lost(Vp9dState *s, uint64_t gen) {
	for (int i = 0; i < 8; i++) if (s->ref[i].pic && s->ref[i].gen == gen) { s->ref[i].pic = 0; s->ref[i].dpb = -1; }
}

void vp9d_flush(Vp9dState *s) {
	// vp9_decode_flush: the frames and references go; CBS keeps its state (ff_cbs_flush has nothing to reset for VP9)
	for (int i = 0; i < 8; i++) { s->ref[i].pic = 0; s->ref[i].dpb = -1; }
	s->have_last = 0;
}

int vp9d_is_key_sample(const uint8_t *d, size_t n) {
	if (n < 1) return 0;
	Bits b = { d, n * 8, 0, 0 };
	if (rb(&b, 2) != 2) return 0;
	const int profile = (int)rb(&b, 1); const int high = (int)rb(&b, 1);
	if ((high << 1 | profile) == 3) rb(&b, 1);
	if (rb(&b, 1)) return 0;   // show_existing_frame
	return !b.over && rb(&b, 1) == 0 && !b.over;
}

// --- one frame: CBS's read of the uncompressed header (cbs_vp9_syntax_template.c), then vp9.c's own checks

static int color_config(Vp9dState *s, Bits *b, int profile, Vp9dFrame *f, int *cs, int *range, int *bd) {
	*bd = 8;
	if (profile >= 2) *bd = rb(b, 1) ? 12 : 10;
	*cs = (int)rb(b, 3);
	if (*cs != STD_VIDEO_VP9_COLOR_SPACE_RGB) {
		*range = (int)rb(b, 1);
		if (profile == 1 || profile == 3) return fail(s, VP9D_UNSUPPORTED, "VP9 4:2:2 / 4:4:0 / 4:4:4 (profile 1 / 3)");
	} else {
		*range = 1;
		return fail(s, VP9D_UNSUPPORTED, profile & 1 ? "VP9 RGB (profile 1 / 3)" : "VP9 RGB in profile 0 / 2 (libavcodec refuses it too)");
	}
	if (*bd == 12) return fail(s, VP9D_UNSUPPORTED, "VP9 12-bit");
	(void)f;
	return 0;
}

// the bytes CBS reads a frame from: a frame whose last byte looks like a superframe marker is split at that index,
// and CBS reads the first part (-1: CBS fails on it)
static long cbs_unit_size(const uint8_t *d, size_t n) {
	if (!n) return -1;
	const uint8_t m = d[n - 1];
	if ((m & 0xe0) != 0xc0) return (long)n;
	const size_t isz = 2 + (size_t)(((m & 0x18) >> 3) + 1) * (size_t)((m & 7) + 1);
	if (isz > n) return -1;
	const int len = ((m >> 3) & 3) + 1, nf = (m & 7) + 1;
	size_t pos = 0; long first = -1;
	for (int i = 0; i < nf; i++) {
		uint32_t sz = 0;
		for (int j = 0; j < len; j++) sz |= (uint32_t)d[n - isz + 1 + (size_t)(i * len + j)] << (8 * j);
		if (pos + sz + isz > n) return -1;
		if (i == 0) first = (long)sz;
		pos += sz;
	}
	return first;
}

// parses frame k of a sample (d, n: the frame); fills f, updates s. 0, or VP9D_UNSUPPORTED (s may be changed: the
// caller restores it), or VP9D_ERROR (the frame did not parse: s unchanged)
static int parse_frame(Vp9dState *s, const uint8_t *d, size_t n, Vp9dFrame *f) {
	const long usz = cbs_unit_size(d, n);
	if (usz <= 0) return fail(s, VP9D_ERROR, "a frame CBS cannot read (superframe-like last byte)");
	Bits b = { d, (size_t)usz * 8, 0, 0 };
	if (rb(&b, 2) != 2) return fail(s, VP9D_ERROR, "invalid frame marker");
	const int lo = (int)rb(&b, 1), hi = (int)rb(&b, 1), profile = hi << 1 | lo;
	if (profile == 3 && rb(&b, 1)) return fail(s, VP9D_ERROR, "reserved bit set");
	if (b.over) return fail(s, VP9D_ERROR, "truncated frame header");
	if (rb(&b, 1)) {   // show_existing_frame
		f->show_existing = 1; f->shown = 1;
		f->show_slot = (int)rb(&b, 3);
		while (!b.over && (b.pos & 7)) if (rb(&b, 1)) return fail(s, VP9D_ERROR, "trailing bits not zero");
		if (b.over) return fail(s, VP9D_ERROR, "truncated frame header");
		const Vp9dRef *r = &s->ref[f->show_slot];
		f->dpb = r->pic ? r->dpb : -1; f->gen = r->pic ? r->gen : 0;
		f->width = r->pic_width; f->height = r->pic_height;
		if (!r->pic) { f->error = f->missing = 1; snprintf(s->why, sizeof s->why, "show_existing_frame of an empty slot %d", f->show_slot); }
		return 0;
	}
	if (profile == 1 || profile == 3) return fail(s, VP9D_UNSUPPORTED, "VP9 profile 1 / 3 (4:2:2, 4:4:0, 4:4:4)");
	if (s->profile >= 0 && profile != s->profile) return fail(s, VP9D_UNSUPPORTED, "a frame of another profile than the vpcC's");
	StdVideoDecodeVP9PictureInfo *p = &f->std;
	const int frame_type = (int)rb(&b, 1), show_frame = (int)rb(&b, 1), error_res = (int)rb(&b, 1);
	int intra_only = 0, reset_ctx = 0, refresh = 0, cs = s->color_space, range = s->color_range, bd = s->bit_depth ? s->bit_depth : 8;
	int idx[3] = { 0, 0, 0 }, sign[4] = { 0, 0, 0, 0 }, w = 0, h = 0, hp = 0, switchable = 0, raw_filter = 0;
	int new_color = 0, r;
	if (frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY) {
		if (rb(&b, 8) != 0x49 || rb(&b, 8) != 0x83 || rb(&b, 8) != 0x42) return fail(s, VP9D_ERROR, b.over ? "truncated frame header" : "invalid sync code");
		if ((r = color_config(s, &b, profile, f, &cs, &range, &bd))) return r;
		new_color = 1;
		w = (int)rb(&b, 16) + 1; h = (int)rb(&b, 16) + 1;
		if (rb(&b, 1)) rb(&b, 32);   // render_size
		refresh = 0xff;
	} else {
		intra_only = show_frame ? 0 : (int)rb(&b, 1);
		reset_ctx = error_res ? 0 : (int)rb(&b, 2);
		if (intra_only) {
			if (rb(&b, 8) != 0x49 || rb(&b, 8) != 0x83 || rb(&b, 8) != 0x42) return fail(s, VP9D_ERROR, b.over ? "truncated frame header" : "invalid sync code");
			if (profile > 0) { if ((r = color_config(s, &b, profile, f, &cs, &range, &bd))) return r; }
			else { cs = STD_VIDEO_VP9_COLOR_SPACE_BT_601; range = 0; bd = 8; }   // (CBS infers no range: 0; vp9.c: video range)
			new_color = 1;
			refresh = (int)rb(&b, 8);
			w = (int)rb(&b, 16) + 1; h = (int)rb(&b, 16) + 1;
			if (rb(&b, 1)) rb(&b, 32);
		} else {
			refresh = (int)rb(&b, 8);
			for (int i = 0; i < 3; i++) { idx[i] = (int)rb(&b, 3); sign[1 + i] = (int)rb(&b, 1); }
			int found = 0;
			for (int i = 0; i < 3 && !found; i++) if (rb(&b, 1)) {
				const Vp9dRef *rf = &s->ref[idx[i]];
				w = rf->width; h = rf->height; bd = rf->valid ? rf->bit_depth : bd; found = 1;
			}
			if (!found) { w = (int)rb(&b, 16) + 1; h = (int)rb(&b, 16) + 1; }
			if (rb(&b, 1)) rb(&b, 32);
			hp = (int)rb(&b, 1);
			switchable = (int)rb(&b, 1);
			if (!switchable) raw_filter = (int)rb(&b, 2);
		}
	}
	const int refresh_ctx = error_res ? 0 : (int)rb(&b, 1);
	const int parallel = error_res ? 1 : (int)rb(&b, 1);
	const int ctx_idx = (int)rb(&b, 2);
	// loop filter: the deltas this frame codes, applied below to the state
	const int lf_level = (int)rb(&b, 6), lf_sharp = (int)rb(&b, 3), lf_en = (int)rb(&b, 1);
	int lf_upd = 0, uref = 0, umode = 0; int8_t dref[4] = { 0 }, dmode[2] = { 0 };
	if (lf_en && (lf_upd = (int)rb(&b, 1))) {
		for (int i = 0; i < 4; i++) if (rb(&b, 1)) { uref |= 1 << i; dref[i] = (int8_t)rs(&b, 6); }
		for (int i = 0; i < 2; i++) if (rb(&b, 1)) { umode |= 1 << i; dmode[i] = (int8_t)rs(&b, 6); }
	}
	const int base_q = (int)rb(&b, 8);
	int dq[3];
	for (int i = 0; i < 3; i++) dq[i] = rb(&b, 1) ? rs(&b, 4) : 0;
	// segmentation
	const int seg_en = (int)rb(&b, 1);
	int upd_map = 0, temporal = 0, upd_data = 0, abs = 0;
	uint8_t tree[7], pred[3], fen[8][4]; int16_t fval[8][4];
	memset(tree, 255, sizeof tree); memset(pred, 255, sizeof pred); memset(fen, 0, sizeof fen); memset(fval, 0, sizeof fval);
	if (seg_en) {
		if ((upd_map = (int)rb(&b, 1))) {
			for (int i = 0; i < 7; i++) tree[i] = rb(&b, 1) ? (uint8_t)rb(&b, 8) : 255;
			temporal = (int)rb(&b, 1);
			for (int i = 0; i < 3; i++) pred[i] = temporal && rb(&b, 1) ? (uint8_t)rb(&b, 8) : 255;
		}
		if ((upd_data = (int)rb(&b, 1))) {
			static const int bits[4] = { 8, 6, 2, 0 }, sgn[4] = { 1, 1, 0, 0 };
			abs = (int)rb(&b, 1);
			for (int i = 0; i < 8; i++) for (int j = 0; j < 4; j++) {
				if (!(fen[i][j] = (uint8_t)rb(&b, 1)) || !bits[j]) continue;
				const int v = (int)rb(&b, bits[j]);
				fval[i][j] = (int16_t)(sgn[j] && rb(&b, 1) ? -v : v);
			}
		}
	}
	// tile info, from the frame width in 64x64 superblocks
	const int sb64 = (((w + 7) >> 3) + 7) >> 3;
	int min_log2 = 0, max_log2 = 0;
	while ((64 << min_log2) < sb64) min_log2++;
	while ((sb64 >> (max_log2 + 1)) >= 4) max_log2++;
	int tcols = min_log2;
	while (tcols < max_log2 && rb(&b, 1)) tcols++;
	int trows = (int)rb(&b, 1);
	if (trows) trows += (int)rb(&b, 1);
	const int comp = (int)rb(&b, 16);
	const size_t hdr_bits = b.pos;
	while (!b.over && (b.pos & 7)) if (rb(&b, 1)) return fail(s, VP9D_ERROR, "trailing bits not zero");
	if (b.over) return fail(s, VP9D_ERROR, "truncated frame header");
	if (w <= 0 || h <= 0) return fail(s, VP9D_ERROR, "a frame size from a reference slot that was never refreshed");
	// vp9.c on what CBS read: the compressed header must be there and start with a zero marker bit
	const size_t uncomp = (b.pos + 7) / 8;
	if ((size_t)comp > n - uncomp) return fail(s, VP9D_ERROR, "invalid compressed header size");
	if (comp == 0) return fail(s, VP9D_ERROR, "no compressed header");
	if (d[uncomp] & 0x80) return fail(s, VP9D_ERROR, "compressed header marker bit set");

	// --- the header parsed: the state CBS and vp9.c carry (also for a frame whose references are missing)
	const int intra = frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY || intra_only;
	const int last_invisible = s->last_invisible;
	s->last_invisible = !show_frame;
	if (intra || error_res) past_independence(s);
	if (lf_upd) {
		for (int i = 0; i < 4; i++) if (uref & 1 << i) s->lf_ref[i] = dref[i];
		for (int i = 0; i < 2; i++) if (umode & 1 << i) s->lf_mode[i] = dmode[i];
	}
	if (upd_data) {
		memcpy(s->feat_en, fen, sizeof fen); memcpy(s->feat_val, fval, sizeof fval);
		s->abs_delta = abs;
		if (upd_map) { memcpy(s->cbs_tree, tree, sizeof tree); memcpy(s->cbs_pred, pred, sizeof pred); }
	}
	if (upd_map) { memcpy(s->tree, tree, sizeof tree); memcpy(s->pred, pred, sizeof pred); }
	if (new_color) { s->color_space = cs; s->color_range = range; s->bit_depth = bd; }
	if (frame_type == STD_VIDEO_VP9_FRAME_TYPE_KEY) s->color_known = 1;
	// the references: every one named must hold a picture (vp9.c "Not all references are available")
	int missing = 0;
	if (!intra) for (int i = 0; i < 3; i++) if (!s->ref[idx[i]].pic) missing = 1;
	const int use_prev = !error_res && !last_invisible && (intra || (s->have_last && s->last_w == w && s->last_h == h));
	for (int i = 0; i < 8; i++) if (refresh & 1 << i) s->ref[i] = (Vp9dRef){ 1, w, h, bd, s->ref[i].pic, s->ref[i].dpb, s->ref[i].gen, s->ref[i].pic_width, s->ref[i].pic_height };

	f->shown = show_frame; f->intra_only = intra_only; f->refresh = refresh; f->hdr_bits = (int)hdr_bits;
	f->width = w; f->height = h; f->bit_depth = bd;
	f->uncomp_size = (uint32_t)uncomp; f->comp_size = (uint32_t)comp;
	for (int i = 0; i < 3; i++) f->ref_frame_idx[i] = idx[i];
	// the Std picture information, as vulkan_vp9.c fills it
	static const StdVideoVP9InterpolationFilter remap[4] = { STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SMOOTH,
		STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP, STD_VIDEO_VP9_INTERPOLATION_FILTER_EIGHTTAP_SHARP, STD_VIDEO_VP9_INTERPOLATION_FILTER_BILINEAR };
	memset(p, 0, sizeof *p);
	p->flags.error_resilient_mode = (uint32_t)error_res; p->flags.intra_only = (uint32_t)intra_only;
	p->flags.allow_high_precision_mv = (uint32_t)hp; p->flags.refresh_frame_context = (uint32_t)refresh_ctx;
	p->flags.frame_parallel_decoding_mode = (uint32_t)parallel; p->flags.segmentation_enabled = (uint32_t)seg_en;
	p->flags.show_frame = (uint32_t)show_frame; p->flags.UsePrevFrameMvs = (uint32_t)use_prev;
	p->profile = (StdVideoVP9Profile)profile; p->frame_type = (StdVideoVP9FrameType)frame_type;
	p->frame_context_idx = (uint8_t)ctx_idx; p->reset_frame_context = (uint8_t)reset_ctx; p->refresh_frame_flags = (uint8_t)refresh;
	p->ref_frame_sign_bias_mask = (uint8_t)(sign[1] << 1 | sign[2] << 2 | sign[3] << 3);
	p->interpolation_filter = switchable ? STD_VIDEO_VP9_INTERPOLATION_FILTER_SWITCHABLE : remap[raw_filter];
	p->base_q_idx = (uint8_t)base_q; p->delta_q_y_dc = (int8_t)dq[0]; p->delta_q_uv_dc = (int8_t)dq[1]; p->delta_q_uv_ac = (int8_t)dq[2];
	p->tile_cols_log2 = (uint8_t)tcols; p->tile_rows_log2 = (uint8_t)trows;
	f->color = (StdVideoVP9ColorConfig){ .flags = { .color_range = (uint32_t)s->color_range }, .BitDepth = (uint8_t)s->bit_depth,
		.subsampling_x = 1, .subsampling_y = 1, .color_space = (StdVideoVP9ColorSpace)s->color_space };
	f->lf = (StdVideoVP9LoopFilter){ .flags = { .loop_filter_delta_enabled = (uint32_t)lf_en, .loop_filter_delta_update = (uint32_t)lf_upd },
		.loop_filter_level = (uint8_t)lf_level, .loop_filter_sharpness = (uint8_t)lf_sharp, .update_ref_delta = (uint8_t)uref,
		.update_mode_delta = (uint8_t)umode };
	memcpy(f->lf.loop_filter_ref_deltas, s->lf_ref, 4); memcpy(f->lf.loop_filter_mode_deltas, s->lf_mode, 2);
	memset(&f->seg, 0, sizeof f->seg);
	f->seg.flags.segmentation_update_map = (uint32_t)upd_map; f->seg.flags.segmentation_temporal_update = (uint32_t)temporal;
	f->seg.flags.segmentation_update_data = (uint32_t)upd_data; f->seg.flags.segmentation_abs_or_delta_update = (uint32_t)s->abs_delta;
	memcpy(f->seg.segmentation_tree_probs, s->tree, 7); memcpy(f->seg.segmentation_pred_prob, s->pred, 3);
	for (int i = 0; i < 8; i++) {
		for (int j = 0; j < 4; j++) { f->seg.FeatureEnabled[i] |= (uint8_t)(s->feat_en[i][j] << j); f->seg.FeatureData[i][j] = s->feat_val[i][j]; }
	}
	// libavcodec's values where they differ, when they matter to this frame (ANV reads the segmentation of segmented
	// frames: the probabilities when the map is coded, the abs/delta flag for the quantizer and loop filter features)
	f->lavc_abs = upd_data ? abs : 0;
	memcpy(f->lavc_tree, s->cbs_tree, 7); memcpy(f->lavc_pred, s->cbs_pred, 3);
	int feat_ql = 0;
	for (int i = 0; i < 8; i++) feat_ql |= f->seg.FeatureEnabled[i] & 3;
	f->lavc_differs = seg_en && ((upd_map && (memcmp(s->cbs_tree, s->tree, 7) || memcmp(s->cbs_pred, s->pred, 3))) ||
	                             (feat_ql && f->lavc_abs != s->abs_delta));
	if (missing) {
		f->error = f->missing = 1;
		snprintf(s->why, sizeof s->why, "an inter frame whose references have no picture");
		return 0;
	}
	// decoded: the DPB slot (the lowest none of the reference slots holds), the references, then the slots refreshed
	const unsigned busy = vp9d_dpb_busy(s);
	int slot = 0;
	while (slot < 31 && busy & (1u << slot)) slot++;
	f->dpb = slot; f->gen = ++s->gen;
	f->nrefs = 0;
	for (int i = 0; i < 3; i++) {
		const Vp9dRef *rf = &s->ref[idx[i]];
		f->name_dpb[i] = rf->pic ? rf->dpb : -1;
		if (!rf->pic) continue;
		int dup = 0;
		for (int k = 0; k < f->nrefs; k++) if (f->refs[k].dpb == rf->dpb) dup = 1;
		if (!dup) f->refs[f->nrefs++] = (Vp9dRefPic){ rf->dpb, idx[i], rf->pic_width, rf->pic_height };
	}
	for (int i = 0; i < 8; i++) if (refresh & 1 << i) { s->ref[i].pic = 1; s->ref[i].dpb = slot; s->ref[i].gen = f->gen; s->ref[i].pic_width = w; s->ref[i].pic_height = h; }
	s->have_last = 1; s->last_w = w; s->last_h = h;
	return 0;
}

int vp9d_parse_sample(Vp9dState *s, const uint8_t *d, size_t n, Vp9dSample *out) {
	memset(out, 0, sizeof *out);
	out->shown = -1;
	if (!n) return fail(s, VP9D_ERROR, "empty sample");
	// the superframe index, as vp9_superframe_split reads it
	uint32_t off[VP9D_MAX_FRAMES], sz[VP9D_MAX_FRAMES]; int nf = 1;
	off[0] = 0; sz[0] = (uint32_t)n;
	const uint8_t m = d[n - 1];
	if ((m & 0xe0) == 0xc0) {
		const int len = 1 + ((m >> 3) & 3), cnt = 1 + (m & 7);
		const size_t isz = 2 + (size_t)(cnt * len);
		if (n >= isz && d[n - isz] == m) {
			uint64_t total = 0;
			for (int i = 0; i < cnt; i++) {
				uint32_t v = 0;
				for (int j = 0; j < len; j++) v |= (uint32_t)d[n - isz + 1 + (size_t)(i * len + j)] << (8 * j);
				total += v;
				if (!v || total > n - isz) return fail(s, VP9D_ERROR, "invalid frame size in a superframe");
				off[i] = (uint32_t)(total - v); sz[i] = v;
			}
			nf = cnt; out->superframe = 1;
		}
	}
	static __thread Vp9dState saved;   // (a few hundred bytes; per thread: sessions decode on several threads)
	saved = *s;
	int parsed = 0;
	for (int k = 0; k < nf; k++) {
		Vp9dFrame *f = &out->f[k];
		memset(f, 0, sizeof *f);
		f->dpb = -1; f->name_dpb[0] = f->name_dpb[1] = f->name_dpb[2] = -1;
		f->offset = off[k]; f->size = sz[k];
		const int r = parse_frame(s, d + off[k], sz[k], f);
		if (r == VP9D_UNSUPPORTED) { char w[160]; memcpy(w, s->why, sizeof w); *s = saved; memcpy(s->why, w, sizeof w); memset(out, 0, sizeof *out); out->shown = -1; return r; }
		if (r == VP9D_ERROR) { f->error = 1; continue; }
		vp9d_frame_fix(f);
		parsed++;
		if (f->shown) { if (out->shown < 0) out->shown = k; else out->extra_shown++; }
	}
	out->nframes = nf;
	if (!parsed) return VP9D_ERROR;   // (every frame failed to parse: nothing changed)
	return VP9D_PICTURE;
}
