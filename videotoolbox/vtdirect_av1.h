// SPDX-License-Identifier: MIT
// vtdirect_av1.h: the AV1 side of the direct Vulkan Video decode path (vtdirect.c): the sequence header, every frame
// header (tiles, quantizer, segmentation, loop filter, CDEF, loop restoration, global motion, film grain), the tile
// groups, the eight reference frame slots with their order hints and saved parameters, show_existing_frame, and the
// DPB slot each picture is decoded into, for Main profile 4:2:0 streams (8 or 10 bits). No Vulkan calls and no FFmpeg:
// the Std structures only, so it is tested off the GPU (tests/av1parse_test.c, against golden files made from FFmpeg's
// trace_headers, tests/av1/mkgolden.py).
//
// Written from the AV1 Bitstream & Decoding Process Specification (v1.0.0 with Errata 1) 5.3-5.12 / 6.8 / 7.20 /
// 7.21 and the Vulkan specification (VK_KHR_video_decode_av1). Where the hardware must see the same values on both
// paths it follows libavcodec 9.0.2's AV1 decoder (av1dec.c, cbs_av1*.c) and Vulkan hwaccel (vulkan_av1.c), not only
// the specification:
// - DPB slots: a picture is decoded into the lowest slot no reference frame slot holds (vulkan_av1.c frame_id);
// - the references passed are those the seven ref_frame_idx[] name, once each, in that order; for KEY and INTRA_ONLY
//   frames ref_frame_idx[] is all 0 (libavcodec's frame header is zeroed), so reference slot 0's picture is passed (if
//   there is one) and OrderHints[] / RefFrameSignBias are those of slot 0's order hint; the hardware does not read
//   references for intra frames;
// - OrderHints[0] and SavedOrderHints[0] are 0; a reference's SavedOrderHints are its own OrderHints[]; the current
//   picture's reference info has no SavedOrderHints (zeros);
// - the loop filter deltas of a frame with loop_filter_delta_enabled = 0 are the defaults (the specification keeps the
//   ones loaded from primary_ref_frame); they matter only to a later frame that loads them;
// - CDEF secondary strengths and segmentation feature values are passed as coded (3 is not turned into 4, no clip);
// - film grain: update_grain = 0 loads the referenced frame's parameters (apply_grain and film_grain_params_ref_idx
//   included, the new grain_seed kept); the Std flags carry no update_grain;
// - SkipModeFrame[] is derived even when skip_mode_present is 0 (frames with reference_select and order hints), and
//   only when every reference has a picture;
// - after a flush the reference slots keep their order hints (libavcodec's frames keep them) but no picture.
#ifndef VTDIRECT_AV1_H
#define VTDIRECT_AV1_H
#include <stddef.h>
#include <stdint.h>
#include <vk_video/vulkan_video_codec_av1std.h>
#include <vk_video/vulkan_video_codec_av1std_decode.h>

#define AV1D_SLOTS 9            // DPB slots: the eight reference frames and the current picture (the B580's maxDpbSlots)
#define AV1D_MAX_TILES 256      // libavcodec's MAX_TILES (the largest any level allows)
#define AV1D_MAX_FRAMES 16      // frame headers in one temporal unit

enum {
	AV1D_PICTURE = 0,       // the sample has frames to decode or to show
	AV1D_NO_PICTURE = 1,    // a sample without frame headers (sequence header, metadata, temporal delimiter)
	AV1D_UNSUPPORTED = -1,  // a stream the direct path does not cover (av1d_why says what): libavcodec takes over
	AV1D_ERROR = -2,        // a damaged sample (the state is as before it)
};

typedef struct {
	StdVideoAV1SequenceHeader std;      // pColorConfig / pTimingInfo point at the two below (av1d_seq_fix)
	StdVideoAV1ColorConfig color;
	StdVideoAV1TimingInfo timing;
	int ops;                            // operating_points_cnt_minus_1 + 1
	uint16_t op_idc[32]; uint8_t level[32], tier[32];
	int decoder_model_info_present, buffer_removal_time_length, frame_presentation_time_length;
	uint8_t decoder_model_present_for_op[32];
	int bit_depth;
	int unsupported;                    // why the direct path cannot decode it (0: it can)
	uint8_t raw[256]; size_t rawn;      // the OBU payload, to recognise a repeated identical sequence header
} Av1dSeq;

// a reference frame slot: the saved state of 7.20 and libavcodec's AV1Frame
typedef struct {
	int valid;                          // usable for frame sizes and show_existing_frame (the parsing state)
	int pic;                            // a decoded picture is held, in DPB slot dpb (decode number gen)
	int dpb; uint64_t gen;
	int grain_dpb;                      // the picture was decoded with film grain: its reference is a separate image
	uint32_t frame_id;
	int upscaled_width, frame_width, frame_height, render_width, render_height;
	int frame_type, order_hint, showable;
	uint8_t order_hints[8];             // its OrderHints[] (the SavedOrderHints its reference info gets)
	uint8_t sign_bias;                  // its RefFrameSignBias mask
	int disable_frame_end_update_cdf, segmentation_enabled;
	int8_t lf_ref[8], lf_mode[2];
	uint8_t feature_enabled[8][8]; int16_t feature_value[8][8];
	int32_t gm[8][6];
	int grain_apply; StdVideoAV1FilmGrain grain;
	int bit_depth;
} Av1dRef;

// one frame header of a sample: a frame to decode, or a show_existing_frame
typedef struct {
	int show_existing;                  // 1: no decode, the picture of reference slot show_slot is shown
	int show_slot;
	int shown;                          // show_frame or show_existing_frame
	// what Vulkan Video gets (a decoded frame)
	Av1dSeq seq;                        // the sequence header it was parsed with (av1d_seq_fix'ed)
	StdVideoDecodeAV1PictureInfo std;   // its pointers point into this structure
	StdVideoAV1TileInfo tile; uint16_t mi_col_starts[64], mi_row_starts[64], width_sbs[64], height_sbs[64];
	StdVideoAV1Quantization quant; StdVideoAV1Segmentation seg; StdVideoAV1LoopFilter lf; StdVideoAV1CDEF cdef;
	StdVideoAV1LoopRestoration lr; StdVideoAV1GlobalMotion gm; StdVideoAV1FilmGrain grain;
	StdVideoDecodeAV1ReferenceInfo setup;     // the current picture's reference information
	int32_t name_dpb[7];                      // referenceNameSlotIndices
	int nrefs; struct { int dpb; int slot; StdVideoDecodeAV1ReferenceInfo info; } refs[7];   // slot: the AV1 slot
	int dpb; uint64_t gen;                    // the DPB slot it is decoded into, its decode number
	int apply_grain;
	int width, height, frame_width;           // UpscaledWidth (the coded extent), FrameHeight; FrameWidth (superres)
	uint32_t ntiles; uint32_t tile_src[AV1D_MAX_TILES], tile_size[AV1D_MAX_TILES];   // offsets into the sample
	size_t size;                              // the tiles' bytes
	int missing;                              // an inter frame whose references have no picture: not decoded
	// for the tests (tests/av1parse_test.c)
	int ref_frame_idx[7], refresh, hdr_bits, ntg;
} Av1dFrame;

typedef struct {
	int nframes; Av1dFrame f[AV1D_MAX_FRAMES];
	int shown;                          // the frame shown (the first one: -1 none)
	int extra_shown;                    // shown frames after it (a temporal unit has one)
} Av1dSample;

typedef struct {
	Av1dSeq seq; int have_seq;
	Av1dRef ref[8];
	uint64_t gen;
	int op_idc;                         // operating point 0's idc: OBUs of other layers are dropped
	int keyframes;                      // frames decoded since the start or a flush (for the tests)
	int check;                          // tests: keep the frame header length
	char why[160];
	uint64_t missing_refs, damaged;
} Av1dState;

void av1d_init(Av1dState *s);
// the av1C record (AV1CodecConfigurationRecord) and its configOBUs: AV1D_NO_PICTURE, or AV1D_UNSUPPORTED / AV1D_ERROR
int av1d_parse_av1c(Av1dState *s, const uint8_t *av1c, size_t n);
// one sample (a temporal unit of OBUs, low-overhead format): every frame header and tile group parsed, the reference
// slots updated, the frames described in *out (tile offsets into data). On AV1D_UNSUPPORTED / AV1D_ERROR the state is
// left as it was before the sample.
int av1d_parse_sample(Av1dState *s, const uint8_t *data, size_t n, Av1dSample *out);
// frame f was not decoded after all (no picture buffer, a failed submit): the slots holding its picture lose it
void av1d_lost(Av1dState *s, uint64_t gen);
// the DPB slots a reference frame slot holds a picture in (bit i: slot i)
unsigned av1d_dpb_busy(const Av1dState *s);
// forget every picture (a seek: libavcodec's flush; the next frames need a key frame)
void av1d_flush(Av1dState *s);
// whether the sample's first frame header is a shown key frame (a random access point: libavcodec can start there).
// Reads only the start of that frame header (show_existing_frame, frame_type, show_frame), without any state.
int av1d_is_key_sample(const uint8_t *data, size_t n);
// points the sequence header's pColorConfig / pTimingInfo at its own copies (after the structure was copied)
void av1d_seq_fix(Av1dSeq *q);
const char *av1d_why(const Av1dState *s);
#endif
