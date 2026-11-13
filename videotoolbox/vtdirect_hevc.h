// SPDX-License-Identifier: MIT
// vtdirect_hevc.h: the HEVC side of the direct Vulkan Video decode path (vtdirect.c): video, sequence and picture
// parameter sets, the slice segment header fields Vulkan Video needs, picture order counts and the decoded picture
// buffer driven by the reference picture sets, for Main / Main 10 / Main Still Picture 4:2:0 streams (8 or 10 bits).
// No Vulkan calls and no FFmpeg: the Std structures only, so it is tested off the GPU (tests/hevcparse_test.c, against
// golden files made from libavcodec, tests/hevc/mkgolden.py).
//
// Written from ITU-T H.265 (V10, 07/2024) 7.3 / 7.4 / 8.3.1 / 8.3.2. Where the hardware must see the same values on
// both paths it follows libavcodec 9.0.2's HEVC decoder (hevcdec.c, refs.c, ps.c) and Vulkan hwaccel (vulkan_hevc.c),
// not only the specification:
// - picture order counts: the previous TemporalId-0 picture's POC split with C's % (libavcodec's ff_hevc_compute_poc2),
//   POC MSB 0 only for BLA pictures (a CRA starting a sequence keeps the MSB derived from the previous picture);
// - an IDR picture, or a picture whose SPS is not the one active before it, drops every reference; otherwise the
//   picture's reference picture set alone marks the references (also after a CRA with NoRaslOutputFlag);
// - RASL pictures of an IRAP picture with NoRaslOutputFlag = 1 are not decoded; NoRaslOutputFlag of a CRA comes from an
//   end of sequence NAL unit (or the start of the stream, or a flush) before it;
// - a reference the RPS names that is not there: ignored in StFoll / LtFoll and for IRAP pictures; for RefPicSetStCurr*
//   / LtCurr of other pictures the picture is not decoded and the references after that entry are dropped (libavcodec
//   stops at the entry);
// - the references passed are every picture the RPS kept (StFoll / LtFoll included), RefPicSetStCurrBefore/After and
//   LtCurr their DPB slots; the SPS's Std fields read by the driver's HCP_PIC_STATE get libavcodec's values (the PCM
//   fields of an SPS without PCM wrap as libavcodec's unsigned subtraction does; the scaling lists as "present" with the
//   defaults when the SPS enables them without coding them).
#ifndef VTDIRECT_HEVC_H
#define VTDIRECT_HEVC_H
#include <stddef.h>
#include <stdint.h>
#include <vk_video/vulkan_video_codec_h265std.h>
#include <vk_video/vulkan_video_codec_h265std_decode.h>

#define HEVCD_SLOTS 16          // DPB slots: up to 15 references and the current picture (the B580's maxDpbSlots)
#define HEVCD_MAX_REFS 15
#define HEVCD_MAX_SUB_LAYERS 7

enum {
	HEVCD_PICTURE = 0,       // a picture: decode it, then hevcd_finish_picture
	HEVCD_NO_PICTURE = 1,    // parameter sets, SEI, a skipped RASL picture ...: nothing to decode
	HEVCD_UNSUPPORTED = -1,  // a stream the direct path does not cover (hevcd_why says what): libavcodec takes over
	HEVCD_ERROR = -2,        // a damaged sample
};

typedef struct { int num_negative, num_positive; int32_t delta[32]; uint32_t used; } HevcDRps;   // derived (7.4.8):
                                                                     // S0 closest first, then S1 closest first

typedef struct {
	StdVideoH265VideoParameterSet std;
	StdVideoH265ProfileTierLevel ptl;
	StdVideoH265DecPicBufMgr dpbm;
	int profile, level_idc;
} HevcDVps;

typedef struct {
	StdVideoH265SequenceParameterSet std;
	StdVideoH265ProfileTierLevel ptl;
	StdVideoH265DecPicBufMgr dpbm;
	StdVideoH265ScalingLists scaling;
	StdVideoH265ShortTermRefPicSet st[64];
	StdVideoH265LongTermRefPicsSps lt;
	StdVideoH265SequenceParameterSetVui vui;
	StdVideoH265HrdParameters hrd;
	StdVideoH265SubLayerHrdParameters hrd_nal[HEVCD_MAX_SUB_LAYERS], hrd_vcl[HEVCD_MAX_SUB_LAYERS];
	HevcDRps rps[65];                     // the SPS's sets, and [64] for the one in the current slice header
	int profile, level_idc;               // general_profile_idc (libavcodec's inference when 0), general_level_idc
	int width, height;                    // luma samples
	int log2_ctb, ctb_w, ctb_h, log2_max_poc_lsb;
	int bit_depth;
	int scaling_coded;                    // sps_scaling_list_data_present_flag as coded
	uint32_t conf[4];                     // conf_win_*_offset as coded
	int pcm_coded;                        // pcm_enabled_flag
	int full_range;                       // the VUI's video_full_range_flag, -1 if absent
	int unsupported;                      // why the direct path cannot decode pictures that use it (0: it can)
	uint64_t gen;                         // changes when an SPS with this id and other content replaces it
	uint8_t *raw; size_t rawn;            // the NAL unit, to recognise a repeated identical SPS
} HevcDSps;

typedef struct {
	StdVideoH265PictureParameterSet std;
	StdVideoH265ScalingLists scaling;
	int sps_id;
	int ncols, nrows; uint16_t col[20], row[22];   // tile column widths / row heights in CTBs (6.5.1)
	int unsupported;
	uint8_t *raw; size_t rawn;
} HevcDPps;

typedef struct {
	int slot;                             // DPB slot index
	int32_t poc;
	int long_term;
} HevcDRef;

// one picture: what Vulkan Video gets for it
typedef struct {
	int nal_type, tid, irap, idr;
	int32_t poc;
	StdVideoDecodeH265PictureInfo std;
	const HevcDVps *vps; const HevcDSps *sps; const HevcDPps *pps;
	uint32_t nslices; const uint32_t *slice_offsets;   // into the bitstream written by hevcd_parse_sample
	size_t size;                          // bytes written (start codes and slice segment NAL units)
	int nrefs; HevcDRef refs[HEVCD_MAX_REFS];          // the references held while it is decoded, in slot order
	int slot;                             // the slot this picture is set up in
	int missing;                          // a reference of RefPicSetStCurr* / LtCurr is not there: not decoded
	// for the tests (tests/hevcparse_test.c): what the golden files print
	int st_mode, st_idx;                  // 0: IDR, 1: short_term_ref_pic_set_idx from the SPS, 2: in the slice header
	int hdr_bits;                         // the first slice segment header's length (only with HevcDState.check)
	int nlist[3]; int32_t list[3][8]; int8_t found[3][8];   // StCurrBefore, StCurrAfter, LtCurr: POCs, found
} HevcDPicture;

typedef struct {
	int nal_length_size;                  // the hvcC's lengthSizeMinusOne + 1
	HevcDVps *vps[16];
	HevcDSps *sps[16];
	HevcDPps *pps[64];
	void *retired[64]; int nretired;      // replaced parameter sets, freed at the next sample (a picture may use them)
	uint64_t gen;
	int active_sps; uint64_t active_gen;  // the SPS of the last picture (-1 before any)
	int active_w, active_h, active_bd;    // its size and bit depth
	int last_vps, last_sps, last_pps;     // the ids of the last parameter sets parsed (tests)
	// libavcodec's decoder state
	int eos, last_eos, no_rasl_output_flag;
	int32_t poc_tid0;
	// the decoded picture buffer
	int slot_busy[HEVCD_SLOTS]; int32_t slot_poc[HEVCD_SLOTS]; int slot_long[HEVCD_SLOTS];
	int pictures;                         // pictures parsed since hevcd_init (0: the next must be an IRAP picture)
	int skipped;                          // the last sample was a RASL picture that is not decoded
	int check;                            // tests: parse every first slice segment header to its end (hdr_bits)
	uint32_t *offsets; uint32_t offsets_cap;   // slice segment offsets of the current picture
	char why[160];                        // the reason of the last HEVCD_UNSUPPORTED / HEVCD_ERROR
	uint64_t missing_refs, rasl_skipped, bad_param_sets;
} HevcDState;

// state for a stream whose samples carry NAL units with nal_length_size-byte big-endian lengths (hvcC); the state must
// be zeroed before the first call (hevcd_free releases what it allocates)
void hevcd_init(HevcDState *s, int nal_length_size);
void hevcd_free(HevcDState *s);
// the parameter sets of an hvcC record (and its lengthSizeMinusOne); HEVCD_NO_PICTURE, or HEVCD_UNSUPPORTED /
// HEVCD_ERROR (hevcd_why)
int hevcd_parse_hvcc(HevcDState *s, const uint8_t *hvcc, size_t n);
// the size hevcd_parse_sample writes for this sample (start code + NAL unit for every slice segment); 0 for a malformed
// sample or one without slices
size_t hevcd_bitstream_size(const HevcDState *s, const uint8_t *data, size_t n);
// one sample (an access unit): parameter sets are taken, the slice segments' NAL units copied into out (at least
// hevcd_bitstream_size bytes) with start codes, the references marked by the picture's RPS, and the picture described
// in *pic. HEVCD_PICTURE (pic->missing: a P/B picture whose references are not there) or another code above.
int hevcd_parse_sample(HevcDState *s, const uint8_t *data, size_t n, uint8_t *out, size_t cap, HevcDPicture *pic);
// the picture has been submitted (or could not be): a decoded picture becomes a short-term reference
void hevcd_finish_picture(HevcDState *s, const HevcDPicture *pic);
// forget every reference (libavcodec's flush: the next CRA starts a sequence, its RASL pictures are not decoded)
void hevcd_flush(HevcDState *s);
const char *hevcd_why(const HevcDState *s);

// helpers shared with the tests
const char *hevcd_nal_type_name(int t);
int hevcd_level_idc_to_std(int general_level_idc);
#endif
