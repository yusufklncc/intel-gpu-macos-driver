// SPDX-License-Identifier: MIT
// vtdirect.h: the direct Vulkan Video decode path of the IntelXe decoders (vtdirect.c): a sample goes to
// vkCmdDecodeVideoKHR with our own parsing (vtdirect_h264.c, vtdirect_hevc.c, vtdirect_av1.c) instead of libavcodec's decoder and
// Vulkan hwaccel.
// The pictures are AVFrames of a libavutil Vulkan frames context made like libavcodec's, so the decoder's copy into
// the IOSurface (gpu_copy in intelxeav1.c), its deferral and its emit queue work on them unchanged.
#ifndef VTDIRECT_H
#define VTDIRECT_H
#include <stddef.h>
#include <stdint.h>
#include <vulkan/vulkan.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>

typedef struct {
	PFN_vkGetInstanceProcAddr gipa;
	VkInstance inst; VkPhysicalDevice pd; VkDevice d;
	int dec_family, dec_queues;       // the video decode queue family and its queue count
	int inline_params;                // VK_KHR_video_maintenance2 (inline session parameters) is enabled
	AVBufferRef *avdev;               // libavutil's context of the device (frames contexts, queue and frame locks)
} VtdDevice;

typedef struct Vtd Vtd;

enum { VTD_PICTURE = 0, VTD_NO_PICTURE = 1, VTD_FALLBACK = -1, VTD_ERROR = -2 };

typedef struct {
	uint64_t pictures, errors, no_ref_skips, gaps, redundant_slices;
	uint64_t skipped;                          // HEVC: RASL pictures not decoded (as libavcodec skips them)
	uint64_t parse_ns, record_ns, submit_ns;   // CPU time spent parsing, recording and submitting (wall clock)
	uint64_t bytes;                            // bitstream bytes handed to the hardware
} VtdStats;

// A direct H.264 decoder for a stream with this avcC, or NULL (why says why: a stream or device the path does not
// cover; libavcodec decodes it then)
Vtd *vtd_open_h264(const VtdDevice *dev, const uint8_t *avcc, size_t n, char *why, size_t whyn);
// the same for an HEVC stream with this hvcC (Main / Main 10 / Main Still Picture, 4:2:0, 8 or 10 bits)
Vtd *vtd_open_hevc(const VtdDevice *dev, const uint8_t *hvcc, size_t n, char *why, size_t whyn);
// the same for an AV1 stream with this av1C (Main profile, 4:2:0, 8 or 10 bits; its configOBUs must carry the sequence
// header). A sample is a temporal unit: every frame in it is decoded (hidden alt-ref frames included), the shown one
// (or a show_existing_frame's picture) is handed back; a sample that shows nothing is answered with its last decoded
// frame, as the libavcodec path answers it.
Vtd *vtd_open_av1(const VtdDevice *dev, const uint8_t *av1c, size_t n, char *why, size_t whyn);
// One sample (an access unit, length-prefixed NAL units). VTD_PICTURE: *pic is a new reference to the decoded picture
// (its decode submitted; the frame's timeline semaphore orders whatever reads it next). VTD_NO_PICTURE: nothing to
// show (parameter sets; an HEVC RASL picture that is not decoded). VTD_FALLBACK: from here on the stream needs libavcodec (vtd_why); nothing was submitted.
// VTD_ERROR: a damaged sample, or a picture whose references are missing (not decoded).
int vtd_decode(Vtd *v, const uint8_t *data, size_t n, AVFrame **pic);
// forget the references (a seek: FinishDelayedFrames)
void vtd_flush(Vtd *v);
// the colour range the stream's SPS states: 1 full, 0 video, -1 not stated
int vtd_full_range(const Vtd *v);
const char *vtd_why(const Vtd *v);
const VtdStats *vtd_stats(const Vtd *v);
// waits for its decodes and frees everything
void vtd_close(Vtd *v);
#endif
