// SPDX-License-Identifier: MIT
// vtdirect_h264.h: the H.264 side of the direct Vulkan Video decode path (vtdirect.c): parameter sets, the slice header
// fields Vulkan Video needs, picture order counts and the decoded picture buffer (reference marking), for frame-coded
// 8-bit 4:2:0 streams. No Vulkan calls and no FFmpeg: the Std structures only, so it is tested off the GPU
// (tests/h264parse_test.c, against libavcodec's own POC and reference lists).
//
// The picture order counts and the marking follow libavcodec 9.0.2's H.264 decoder (h264_parse.c ff_h264_init_poc,
// h264_refs.c ff_h264_execute_ref_pic_marking, h264dec.c idr()), not only the specification, so the B580 gets the same
// values on both paths and decodes the same pictures:
// - an IDR sets the previous POC MSB to 65536 and the previous LSB to "unset" (the IDR's own LSB): POCs are offset by
//   65536 (the hardware uses their differences);
// - memory_management_control_operation 5 sets the picture's frame_num to 0 and keeps its POC (8.2.1 would subtract
//   tempPicOrderCnt);
// - the sliding window removes the oldest short-term reference once short + long >= max_num_ref_frames, before the
//   current picture is added; a short-term reference with the current frame_num is dropped when it is added; more
//   than max(max_num_ref_frames, 1) references drops the oldest short-term one (or the first long-term one);
// - a long-term reference's FrameNum is its LongTermFrameIdx (libavcodec passes pic_id, which its reference list
//   construction sets to the same value).
#ifndef VTDIRECT_H264_H
#define VTDIRECT_H264_H
#include <stddef.h>
#include <stdint.h>
#include <vk_video/vulkan_video_codec_h264std.h>
#include <vk_video/vulkan_video_codec_h264std_decode.h>

#define H264D_SLOTS 17          // DPB slots: 16 references and the current picture
#define H264D_MAX_REFS 16

enum {
	H264D_PICTURE = 0,       // a picture: decode it, then h264d_finish_picture
	H264D_NO_PICTURE = 1,    // parameter sets, SEI, ...: nothing to decode
	H264D_UNSUPPORTED = -1,  // a stream the direct path does not cover (h264d_why says what): libavcodec takes over
	H264D_ERROR = -2,        // a damaged sample
};

typedef struct {
	int present;
	StdVideoH264SequenceParameterSet std;
	StdVideoH264ScalingLists scaling;
	int32_t offset_for_ref_frame[256];
	int profile_idc, level_idc;
	int width_mbs, height_mbs;            // the coded size in macroblocks (frames only)
	int log2_max_frame_num, log2_max_poc_lsb;
	int unsupported;                      // why the direct path cannot decode pictures that use it (0: it can)
	int full_range;                       // the VUI's video_full_range_flag, -1 if absent
} H264DSps;

typedef struct {
	int present;
	StdVideoH264PictureParameterSet std;
	StdVideoH264ScalingLists scaling;
	int sps_id;
	int num_slice_groups;
} H264DPps;

typedef struct {
	int slot;                             // DPB slot index
	int frame_num;                        // FrameNum, or LongTermFrameIdx for a long-term reference
	int32_t poc[2];
	int long_term;
} H264DRef;

// one picture: what Vulkan Video gets for it
typedef struct {
	int idr, nal_ref_idc, is_intra, frame_num, idr_pic_id;
	int slice_type;                       // the first slice's (0 P, 1 B, 2 I)
	int32_t poc[2];                       // TopFieldOrderCnt, BottomFieldOrderCnt (libavcodec's numbering, above)
	StdVideoDecodeH264PictureInfo std;
	const H264DSps *sps; const H264DPps *pps;
	uint32_t nslices; const uint32_t *slice_offsets;   // into the bitstream written by h264d_parse_sample
	size_t size;                          // bytes written (start codes and slice NAL units)
	int nrefs; H264DRef refs[H264D_MAX_REFS];         // the DPB before this picture: short-term newest first, then long-term
	int slot;                             // the slot this picture is set up in
	int max_active_l0, max_active_l1;     // the largest num_ref_idx_active of its slices (0 for intra slices)
	// reference marking, applied by h264d_finish_picture
	int long_term_reference_flag, adaptive;
	int nmmco; struct { int op, a, b; } mmco[66];
	int gap;                              // frame_num jumped (not allowed by the SPS: a lost picture)
} H264DPicture;

typedef struct {
	int frame_num;                        // libavcodec's pic->frame_num (0 after MMCO 5)
	int32_t poc[2];
	int long_idx;                         // LongTermFrameIdx, or -1 for a short-term reference
} H264DSlotInfo;

typedef struct {
	int nal_length_size;                  // the avcC's lengthSizeMinusOne + 1
	H264DSps sps[32];
	H264DPps *pps[256];
	int active_sps;                       // the SPS of the last picture (-1 before any)
	int last_sps, last_pps;               // the ids of the last parameter sets parsed (tests)
	// picture order count state (libavcodec's H264POCContext)
	int prev_poc_msb, prev_poc_lsb, prev_frame_num_offset, prev_frame_num;
	// the decoded picture buffer
	int short_ref[H264D_MAX_REFS], nshort;   // slots, newest first
	int long_ref[16];                        // slot by LongTermFrameIdx, -1 if none
	int nlong;
	H264DSlotInfo slot[H264D_SLOTS];
	int slot_busy[H264D_SLOTS];              // holds a reference
	int pictures;                            // pictures parsed since h264d_init (0: the next must be an IDR)
	int pending_poc_msb, pending_poc_lsb, pending_frame_num_offset, mmco5;   // the parsed picture's, for finish
	uint32_t *offsets; uint32_t offsets_cap; // slice offsets of the current picture
	char why[128];                           // the reason of the last H264D_UNSUPPORTED / H264D_ERROR
	uint64_t gaps, redundant_slices;
} H264DState;

// state for a stream whose samples carry NAL units with nal_length_size-byte big-endian lengths (avcC); the state must
// be zeroed before the first call (h264d_free releases what it allocates)
void h264d_init(H264DState *s, int nal_length_size);
void h264d_free(H264DState *s);
// the SPS and PPS of an avcC record; H264D_NO_PICTURE, or H264D_UNSUPPORTED / H264D_ERROR (h264d_why)
int h264d_parse_avcc(H264DState *s, const uint8_t *avcc, size_t n);
// the size h264d_parse_sample writes for this sample (start code + NAL unit for every slice); 0 for a malformed sample
size_t h264d_bitstream_size(const H264DState *s, const uint8_t *data, size_t n);
// one sample (an access unit): parameter sets are taken, the slices' NAL units copied into out (at least
// h264d_bitstream_size bytes) with start codes, and the picture described in *pic. H264D_PICTURE or another code above.
int h264d_parse_sample(H264DState *s, const uint8_t *data, size_t n, uint8_t *out, size_t cap, H264DPicture *pic);
// the picture has been submitted: reference marking (the DPB after it) and the POC state for the next one
void h264d_finish_picture(H264DState *s, const H264DPicture *pic);
// forget every reference (libavcodec's flush: the next pictures decode with what they find)
void h264d_flush(H264DState *s);
const char *h264d_why(const H264DState *s);

// helpers shared with the tests
int h264d_level_idc_to_std(int level_idc);
#endif
