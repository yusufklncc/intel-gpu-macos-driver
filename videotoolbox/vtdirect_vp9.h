// SPDX-License-Identifier: MIT
// vtdirect_vp9.h: the VP9 side of the direct Vulkan Video decode path (vtdirect.c): the superframe index, every frame's
// uncompressed header (frame size, reference names and refresh flags, loop filter, quantizer, segmentation, tile
// info), show_existing_frame, the persistent state that the uncompressed header updates (loop filter deltas,
// segmentation features, the reference frame slots' sizes), and the DPB slot each picture is decoded into, for
// Profile 0 (8-bit) and Profile 2 (10-bit) 4:2:0 streams. The compressed header and the tiles are the hardware's (the
// B580's HCP parses them; ANV keeps the four probability contexts in the session). No Vulkan calls and no FFmpeg: the
// Std structures only, so it is tested off the GPU (tests/vp9parse_test.c, against golden files made from FFmpeg's
// trace_headers, tests/vp9/mkgolden.py).
//
// Written from the VP9 Bitstream & Decoding Process Specification (v0.6, 2016) 6.1-6.2 / 7.2 / 8.4.1 / Annex B and the
// Vulkan specification (VK_KHR_video_decode_vp9). Where the hardware must see the same values on both paths it
// follows libavcodec 9.0.2's VP9 decoder (vp9.c, cbs_vp9_syntax_template.c, bsf/vp9_superframe_split.c) and Vulkan
// hwaccel (vulkan_vp9.c), not only the specification:
// - a sample is split at its superframe index as vp9_superframe_split does (an index whose first byte does not repeat
//   the marker is no index; a frame size of 0 or past the index fails the sample); each frame is then read as CBS
//   reads a packet (a frame whose last byte looks like an index marker is cut where that index says);
// - DPB slots: a picture is decoded into the lowest slot no reference frame slot holds (vulkan_vp9.c frame_id); the
//   references passed are the pictures the three ref_frame_idx[] name, once each, in that order; for KEY and
//   intra-only frames ref_frame_idx[] is 0 (libavcodec's header is zeroed), so reference slot 0's picture is passed if
//   there is one (the hardware reads no reference for intra frames);
// - UsePrevFrameMvs as vp9.c's use_last_frame_mvs: not error resilient, the previously parsed frame header was shown,
//   and for inter frames the last frame that was decoded (not a show_existing_frame) has the same size;
// - Std fields the driver reads are libavcodec's: frame_context_idx and reset_frame_context as coded (also for key
//   frames), refresh_frame_flags 0xff for key frames, ref_frame_sign_bias_mask as coded (vp9.c clears it in error
//   resilient mode, the hwaccel does not), interpolation_filter remapped to the Std enum (EIGHTTAP_SMOOTH for intra
//   frames), loop filter deltas and segmentation features as CBS keeps them across frames;
// - two segmentation values follow the specification where the hwaccel passes CBS's stale copies (the hardware decodes
//   those frames wrongly through libavcodec; vp9parse_test prints a "lavc" line for every such frame):
//   segmentation_abs_or_delta_update persists until the next segmentation_update_data (libavcodec passes the current
//   header's, 0 when the frame does not update the data), and the tree and prediction probabilities of a frame with
//   segmentation_update_map are its own (CBS copies them only when the frame also updates the data);
// - Std fields the driver does not read follow the specification: update_ref_delta / update_mode_delta as bit masks,
//   the colour configuration of the last key or intra-only frame for inter frames.
// Broken streams: a frame that does not parse is not decoded and changes nothing; an inter frame whose references
// have no picture (after a flush, a seek, or a frame that was not decoded) is not decoded, its header still updates
// the loop filter deltas, segmentation features and reference sizes (as CBS does), the pictures in the slots stay.
#ifndef VTDIRECT_VP9_H
#define VTDIRECT_VP9_H
#include <stddef.h>
#include <stdint.h>
#include <vk_video/vulkan_video_codec_vp9std.h>
#include <vk_video/vulkan_video_codec_vp9std_decode.h>

#define VP9D_SLOTS 9            // DPB slots: the eight reference frames and the current picture (the B580's maxDpbSlots is 12)
#define VP9D_MAX_FRAMES 8       // frames in one superframe

enum {
	VP9D_PICTURE = 0,       // the sample has frames to decode or to show
	VP9D_UNSUPPORTED = -1,  // a frame the direct path does not cover (vp9d_why says what): libavcodec takes over
	VP9D_ERROR = -2,        // a damaged sample (no frame in it parsed: the state is as before it)
};

// a reference frame slot
typedef struct {
	int valid;                          // a frame refreshed it (its size is known)
	int width, height, bit_depth;       // the size CBS keeps for frame_size_with_refs
	int pic;                            // a decoded picture is held, in DPB slot dpb (decode number gen)
	int dpb; uint64_t gen;
	int pic_width, pic_height;          // that picture's size
} Vp9dRef;

// a reference passed with a frame
typedef struct { int dpb, slot, width, height; } Vp9dRefPic;   // slot: the VP9 reference slot

// one frame of a sample: a frame to decode, or a show_existing_frame
typedef struct {
	int show_existing;                  // 1: no decode, the picture of reference slot show_slot is shown
	int show_slot;
	int shown;                          // show_frame or show_existing_frame
	int error;                          // the frame did not parse or its references have no picture: not decoded
	int missing;                        // (error) an inter frame whose references have no picture / an empty slot shown
	// what Vulkan Video gets (a decoded frame); the pointers of std point into this structure (vp9d_frame_fix)
	StdVideoDecodeVP9PictureInfo std;
	StdVideoVP9ColorConfig color; StdVideoVP9LoopFilter lf; StdVideoVP9Segmentation seg;
	int32_t name_dpb[3];                // referenceNameSlotIndices
	int nrefs; Vp9dRefPic refs[3];
	int dpb; uint64_t gen;              // the DPB slot it is decoded into, its decode number
	int width, height, bit_depth;
	uint32_t offset, size;              // the frame in the sample
	uint32_t uncomp_size, comp_size;    // compressedHeaderOffset; tilesOffset - compressedHeaderOffset
	// what libavcodec's Vulkan hwaccel passes where it differs (see the top): segmentation_abs_or_delta_update and
	// the tree / prediction probabilities (lavc_differs: they matter for this frame)
	int lavc_differs, lavc_abs; uint8_t lavc_tree[7], lavc_pred[3];
	// for the tests (tests/vp9parse_test.c)
	int ref_frame_idx[3], refresh, hdr_bits, intra_only;
} Vp9dFrame;

typedef struct {
	int nframes; Vp9dFrame f[VP9D_MAX_FRAMES];
	int shown;                          // the frame shown (the first one: -1 none)
	int extra_shown;                    // shown frames after it (a superframe has one)
	int superframe;                     // the sample had a superframe index
} Vp9dSample;

typedef struct {
	Vp9dRef ref[8];
	uint64_t gen;
	int profile;                        // the stream's profile (0 or 2), from the vpcC; -1 any of the two
	// the state the uncompressed header carries from frame to frame (CBS's CodedBitstreamVP9Context)
	int8_t lf_ref[4], lf_mode[2];
	uint8_t feat_en[8][4]; int16_t feat_val[8][4];
	int abs_delta;
	uint8_t tree[7], pred[3];           // the last ones a frame coded (segmentation_update_map)
	uint8_t cbs_tree[7], cbs_pred[3];   // CBS's copies (libavcodec's hwaccel passes these)
	int color_space, color_range, bit_depth;   // of the last key or intra-only frame
	// vp9.c: the last parsed header's show_frame, the last frame decoded (its size)
	int last_invisible, have_last, last_w, last_h;
	int color_known;                    // a key frame has been parsed (vp9d_full_range)
	int check;                          // tests: keep the header length
	char why[160];
} Vp9dState;

void vp9d_init(Vp9dState *s, int profile);
// one sample (a frame or a superframe): every frame's uncompressed header parsed, the reference slots updated, the
// frames described in *out (offsets into data). VP9D_PICTURE also when some frames are in error (Vp9dFrame.error); on
// VP9D_UNSUPPORTED / VP9D_ERROR the state is left as it was before the sample.
int vp9d_parse_sample(Vp9dState *s, const uint8_t *data, size_t n, Vp9dSample *out);
// frame gen was not decoded after all (no picture buffer, a failed submit): the slots holding its picture lose it
void vp9d_lost(Vp9dState *s, uint64_t gen);
// the DPB slots a reference frame slot holds a picture in (bit i: slot i)
unsigned vp9d_dpb_busy(const Vp9dState *s);
// forget every picture (a seek: libavcodec's flush; the next frames need a key frame)
void vp9d_flush(Vp9dState *s);
// whether the sample's first frame is a key frame (libavcodec can start there). Reads only the start of its header.
int vp9d_is_key_sample(const uint8_t *data, size_t n);
// points the Std picture information's pColorConfig / pLoopFilter / pSegmentation at the frame's own (after a copy)
void vp9d_frame_fix(Vp9dFrame *f);
// the colour range of the last key frame: 1 full, 0 video, -1 none yet
int vp9d_full_range(const Vp9dState *s);
const char *vp9d_why(const Vp9dState *s);
#endif
