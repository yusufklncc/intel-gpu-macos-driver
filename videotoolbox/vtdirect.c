// SPDX-License-Identifier: MIT
// vtdirect.c: the direct Vulkan Video decode path (see vtdirect.h). Per sample: the slices' NAL units are copied once,
// with start codes, into a host-visible bitstream buffer (vtdirect_h264.c / vtdirect_hevc.c parse the headers on the
// way), and one
// command buffer does the layout barriers, vkCmdBeginVideoCodingKHR, vkCmdDecodeVideoKHR and the end, with the
// parameter sets inline (VK_KHR_video_maintenance2). What libavcodec 9.0.2 does per picture and this path does not:
// the packet copy and a second pass over every slice to remove emulation prevention bytes (h2645_parse), the full
// slice header and reference list construction (the B580's MFX parses the slice headers itself: ANV programs it in
// short format), two vkCreateImageView / vkDestroyImageView (the views are made once per pooled image here), and the
// execution-context dependency bookkeeping (an AVBufferRef per frame, buffer and view set).
//
// What it decodes like libavcodec's Vulkan hwaccel (vulkan_decode.c / vulkan_h264.c / vulkan_hevc.c), so the hardware
// gets the same
// commands: the pictures come from a libavutil frames context made like ff_vk_frame_params (the format the device
// reports for DPB + output, optimal tiling, the same usage, the profile list), decoded in place
// (DPB_AND_OUTPUT_COINCIDE), the current picture bound as an inactive slot (slotIndex -1) in vkCmdBeginVideoCodingKHR,
// the references (H.264: short-term newest first then long-term; HEVC: every picture the RPS keeps, StFoll / LtFoll
// included, with RefPicSetStCurrBefore/After / LtCurr naming their slots), each frame waited on at its timeline value
// and advanced by one (libavutil's protocol, which the copy into the IOSurface follows too). The bitstream range is
// aligned to minBitstreamBufferSizeAlignment as libavcodec aligns it. The session is reset once at open.
#include "vtdirect.h"
#include "vtdirect_h264.h"
#include "vtdirect_hevc.h"
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define VRING 8   // decodes in flight per session (command buffer + bitstream buffer each)
#define MAX_SLOTS 17   // H264D_SLOTS; HEVCD_SLOTS is 16
#define MAX_REFS 16

typedef struct { VkImage img; VkImageView view; } View;
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *map; VkDeviceSize size; VkCommandBuffer cmd; uint64_t value; } Ring;

enum { CODEC_H264, CODEC_HEVC };

struct Vtd {
	VtdDevice dev;
	int codec, nslots;
	H264DState h;
	HevcDState hv;
	VkVideoDecodeH264ProfileInfoKHR h264prof; VkVideoDecodeH265ProfileInfoKHR h265prof;
	VkVideoDecodeUsageInfoKHR usage; VkVideoProfileInfoKHR profile;
	VkVideoProfileListInfoKHR plist;
	VkVideoCapabilitiesKHR caps; VkVideoDecodeCapabilitiesKHR dcaps; VkVideoDecodeH264CapabilitiesKHR hcaps;
	VkVideoDecodeH265CapabilitiesKHR h265caps;
	VkFormat fmt; uint32_t cw, ch;                 // the pictures' coded size (H.264: whole macroblocks; HEVC: the SPS's)
	int ten;                                       // 10-bit pictures (P010)
	VkVideoSessionKHR session; VkDeviceMemory smem[32]; uint32_t nsmem;
	AVBufferRef *frames;
	VkQueue q; uint32_t qidx;
	VkCommandPool pool; Ring ring[VRING]; unsigned next;
	VkSemaphore vsem; uint64_t submitted;          // timeline: one value per submission
	View *views; int nviews, cviews;
	AVFrame *slot_frame[MAX_SLOTS];                 // the picture in each DPB slot that holds a reference
	uint32_t host_type; int host_coherent; VkDeviceSize atom;
	char why[160];
	VtdStats st;
	struct {
		PFN_vkCreateVideoSessionKHR CreateSession; PFN_vkDestroyVideoSessionKHR DestroySession;
		PFN_vkGetVideoSessionMemoryRequirementsKHR SessionReqs; PFN_vkBindVideoSessionMemoryKHR BindSession;
		PFN_vkAllocateMemory Alloc; PFN_vkFreeMemory Free; PFN_vkMapMemory Map; PFN_vkFlushMappedMemoryRanges FlushRanges;
		PFN_vkCreateBuffer CreateBuffer; PFN_vkDestroyBuffer DestroyBuffer; PFN_vkGetBufferMemoryRequirements BufferReqs;
		PFN_vkBindBufferMemory BindBuffer; PFN_vkCreateCommandPool CreatePool; PFN_vkDestroyCommandPool DestroyPool;
		PFN_vkAllocateCommandBuffers AllocCmd; PFN_vkBeginCommandBuffer Begin; PFN_vkEndCommandBuffer End;
		PFN_vkResetCommandBuffer ResetCmd; PFN_vkCmdPipelineBarrier2 Barrier; PFN_vkCmdBeginVideoCodingKHR BeginCoding;
		PFN_vkCmdControlVideoCodingKHR ControlCoding; PFN_vkCmdDecodeVideoKHR Decode; PFN_vkCmdEndVideoCodingKHR EndCoding;
		PFN_vkCreateSemaphore CreateSem; PFN_vkDestroySemaphore DestroySem; PFN_vkWaitSemaphores Wait;
		PFN_vkQueueSubmit2 Submit; PFN_vkGetDeviceQueue GetQueue; PFN_vkCreateImageView CreateView;
		PFN_vkDestroyImageView DestroyView;
	} vk;
};

static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static void why(Vtd *v, const char *what) { snprintf(v->why, sizeof v->why, "%s", what); }
const char *vtd_why(const Vtd *v) { return v->why; }
const VtdStats *vtd_stats(const Vtd *v) { return &v->st; }

static void wait_value(Vtd *v, uint64_t value) {
	if (!value) return;
	VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &v->vsem, &value };
	v->vk.Wait(v->dev.d, &wi, UINT64_MAX);
}

static int submit(Vtd *v, const VkSubmitInfo2 *si) {
	AVHWDeviceContext *dc = (AVHWDeviceContext *)v->dev.avdev->data;
	AVVulkanDeviceContext *hw = dc->hwctx;
	(void)hw;
#if FF_API_VULKAN_SYNC_QUEUES
	if (hw->lock_queue) hw->lock_queue(dc, (uint32_t)v->dev.dec_family, v->qidx);
#endif
	const VkResult r = v->vk.Submit(v->q, 1, si, VK_NULL_HANDLE);
#if FF_API_VULKAN_SYNC_QUEUES
	if (hw->unlock_queue) hw->unlock_queue(dc, (uint32_t)v->dev.dec_family, v->qidx);
#endif
	return r == VK_SUCCESS ? 0 : (int)r;
}

static VkImageView view_of(Vtd *v, const AVFrame *f) {
	const AVVkFrame *vkf = (const AVVkFrame *)f->data[0];
	for (int i = 0; i < v->nviews; i++) if (v->views[i].img == vkf->img[0]) return v->views[i].view;
	if (v->nviews == v->cviews) {
		const int nc = v->cviews ? v->cviews * 2 : 32;
		View *nv = realloc(v->views, (size_t)nc * sizeof *nv);
		if (!nv) return VK_NULL_HANDLE;
		v->views = nv; v->cviews = nc;
	}
	VkImageViewUsageCreateInfo uci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO, NULL,
		VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR };
	VkImageViewCreateInfo ci = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &uci, 0, vkf->img[0], VK_IMAGE_VIEW_TYPE_2D, v->fmt,
		{ VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY, VK_COMPONENT_SWIZZLE_IDENTITY },
		{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
	VkImageView view;
	if (v->vk.CreateView(v->dev.d, &ci, NULL, &view) != VK_SUCCESS) return VK_NULL_HANDLE;
	v->views[v->nviews++] = (View){ vkf->img[0], view };
	return view;
}

// a host-visible, coherent bitstream buffer of at least size bytes in this ring entry (system memory preferred: the
// CPU-visible VRAM window is scarce on this machine)
static int ring_reserve(Vtd *v, Ring *r, VkDeviceSize size) {
	if (r->size >= size) return 0;
	if (r->buf) { v->vk.DestroyBuffer(v->dev.d, r->buf, NULL); v->vk.Free(v->dev.d, r->mem, NULL); r->buf = VK_NULL_HANDLE; r->mem = VK_NULL_HANDLE; r->map = NULL; r->size = 0; }
	VkDeviceSize sz = 1 << 20;
	while (sz < size) sz <<= 1;
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &v->plist, 0, sz, VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR, VK_SHARING_MODE_EXCLUSIVE };
	if (v->vk.CreateBuffer(v->dev.d, &bci, NULL, &r->buf)) return -1;
	VkMemoryRequirements mr; v->vk.BufferReqs(v->dev.d, r->buf, &mr);
	VkPhysicalDeviceMemoryProperties mp;
	((PFN_vkGetPhysicalDeviceMemoryProperties)v->dev.gipa(v->dev.inst, "vkGetPhysicalDeviceMemoryProperties"))(v->dev.pd, &mp);
	int best = -1, best_score = -1;
	for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
		if (!(mr.memoryTypeBits & (1u << i))) continue;
		const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
		if (!(f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) continue;
		const int score = (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT ? 2 : 0) + (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT ? 0 : 1);
		if (score > best_score) { best = (int)i; best_score = score; }
	}
	if (best < 0) { v->vk.DestroyBuffer(v->dev.d, r->buf, NULL); r->buf = VK_NULL_HANDLE; return -1; }
	v->host_coherent = !!(mp.memoryTypes[best].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, mr.size, (uint32_t)best };
	void *map = NULL;
	if (v->vk.Alloc(v->dev.d, &mai, NULL, &r->mem) || v->vk.BindBuffer(v->dev.d, r->buf, r->mem, 0) ||
	    v->vk.Map(v->dev.d, r->mem, 0, VK_WHOLE_SIZE, 0, &map)) {
		v->vk.DestroyBuffer(v->dev.d, r->buf, NULL); if (r->mem) v->vk.Free(v->dev.d, r->mem, NULL);
		r->buf = VK_NULL_HANDLE; r->mem = VK_NULL_HANDLE; return -1;
	}
	r->map = map; r->size = sz;
	return 0;
}

static void drop_slot_frames(Vtd *v) { for (int i = 0; i < MAX_SLOTS; i++) av_frame_free(&v->slot_frame[i]); }

void vtd_close(Vtd *v) {
	if (!v) return;
	if (v->vsem) wait_value(v, v->submitted);
	drop_slot_frames(v);
	for (int i = 0; i < v->nviews; i++) v->vk.DestroyView(v->dev.d, v->views[i].view, NULL);
	free(v->views);
	av_buffer_unref(&v->frames);
	for (int i = 0; i < VRING; i++) if (v->ring[i].buf) { v->vk.DestroyBuffer(v->dev.d, v->ring[i].buf, NULL); v->vk.Free(v->dev.d, v->ring[i].mem, NULL); }
	if (v->pool) v->vk.DestroyPool(v->dev.d, v->pool, NULL);
	if (v->vsem) v->vk.DestroySem(v->dev.d, v->vsem, NULL);
	if (v->session) v->vk.DestroySession(v->dev.d, v->session, NULL);
	for (uint32_t i = 0; i < v->nsmem; i++) v->vk.Free(v->dev.d, v->smem[i], NULL);
	h264d_free(&v->h);
	hevcd_free(&v->hv);
	free(v);
}

int vtd_full_range(const Vtd *v) {
	if (v->codec == CODEC_HEVC) {
		const int id = v->hv.active_sps >= 0 ? v->hv.active_sps : v->hv.last_sps;
		const HevcDSps *s = id >= 0 ? v->hv.sps[id] : NULL;
		return s ? s->full_range : -1;
	}
	const H264DSps *s = &v->h.sps[v->h.active_sps >= 0 ? v->h.active_sps : v->h.last_sps];
	return s->present ? s->full_range : -1;
}

#define GD(f, n) v->vk.f = (void *)gdpa(dev->d, n)
static Vtd *vtd_alloc(const VtdDevice *dev, int codec) {
	Vtd *v = calloc(1, sizeof *v);
	if (!v) return NULL;
	v->dev = *dev;
	v->codec = codec;
	h264d_init(&v->h, 4);
	hevcd_init(&v->hv, 4);
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)dev->gipa(dev->inst, "vkGetDeviceProcAddr");
	GD(CreateSession, "vkCreateVideoSessionKHR"); GD(DestroySession, "vkDestroyVideoSessionKHR");
	GD(SessionReqs, "vkGetVideoSessionMemoryRequirementsKHR"); GD(BindSession, "vkBindVideoSessionMemoryKHR");
	GD(Alloc, "vkAllocateMemory"); GD(Free, "vkFreeMemory"); GD(Map, "vkMapMemory"); GD(FlushRanges, "vkFlushMappedMemoryRanges");
	GD(CreateBuffer, "vkCreateBuffer"); GD(DestroyBuffer, "vkDestroyBuffer"); GD(BufferReqs, "vkGetBufferMemoryRequirements");
	GD(BindBuffer, "vkBindBufferMemory"); GD(CreatePool, "vkCreateCommandPool"); GD(DestroyPool, "vkDestroyCommandPool");
	GD(AllocCmd, "vkAllocateCommandBuffers"); GD(Begin, "vkBeginCommandBuffer"); GD(End, "vkEndCommandBuffer");
	GD(ResetCmd, "vkResetCommandBuffer"); GD(Barrier, "vkCmdPipelineBarrier2"); GD(BeginCoding, "vkCmdBeginVideoCodingKHR");
	GD(ControlCoding, "vkCmdControlVideoCodingKHR"); GD(Decode, "vkCmdDecodeVideoKHR"); GD(EndCoding, "vkCmdEndVideoCodingKHR");
	GD(CreateSem, "vkCreateSemaphore"); GD(DestroySem, "vkDestroySemaphore"); GD(Wait, "vkWaitSemaphores");
	GD(Submit, "vkQueueSubmit2"); GD(GetQueue, "vkGetDeviceQueue"); GD(CreateView, "vkCreateImageView");
	GD(DestroyView, "vkDestroyImageView");
	return v;
}
#undef GD

// everything after the codec's profile (v->profile, its pNext chain, v->dcaps.pNext): capabilities, the picture format,
// the session and its memory, the frames context, the queue, commands and timeline, the reset. NULL on failure (v freed).
#define FAIL(...) do { snprintf(whyp, whyn, __VA_ARGS__); vtd_close(v); return NULL; } while (0)
static Vtd *open_session(Vtd *v, char *whyp, size_t whyn) {
	const VtdDevice *dev = &v->dev;
	for (size_t i = 0; i < sizeof v->vk / sizeof(void *); i++) if (!((void **)&v->vk)[i]) FAIL("a Vulkan Video entry point is missing");
	if (!dev->inline_params) FAIL("VK_KHR_video_maintenance2 (inline parameter sets) is not enabled");
	if (dev->dec_family < 0 || dev->dec_queues < 1) FAIL("no video decode queue");
	v->plist = (VkVideoProfileListInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR, NULL, 1, &v->profile };
	v->dcaps = (VkVideoDecodeCapabilitiesKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, v->dcaps.pNext };
	v->caps = (VkVideoCapabilitiesKHR){ VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &v->dcaps };
	VkResult r = ((PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)dev->gipa(dev->inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR"))(dev->pd, &v->profile, &v->caps);
	if (r) FAIL("vkGetPhysicalDeviceVideoCapabilitiesKHR -> %d", r);
	if (!(v->dcaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR)) FAIL("the device does not decode in place (DPB_AND_OUTPUT_COINCIDE)");
	if (v->cw > v->caps.maxCodedExtent.width || v->ch > v->caps.maxCodedExtent.height) FAIL("%ux%u is larger than the device decodes", v->cw, v->ch);
	// the picture format: the one the device reports for DPB + output images (NV12 for 8-bit 4:2:0, P010 for 10-bit)
	const VkFormat want = v->ten ? VK_FORMAT_G10X6_B10X6R10X6_2PLANE_420_UNORM_3PACK16 : VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	VkPhysicalDeviceVideoFormatInfoKHR fi = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &v->plist,
		VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT };
	PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR fmts = (PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR)dev->gipa(dev->inst, "vkGetPhysicalDeviceVideoFormatPropertiesKHR");
	uint32_t nf = 0;
	VkVideoFormatPropertiesKHR fp[16];
	if (fmts(dev->pd, &fi, &nf, NULL) || !nf) FAIL("no video format for DPB + output images");
	if (nf > 16) nf = 16;
	for (uint32_t i = 0; i < nf; i++) fp[i] = (VkVideoFormatPropertiesKHR){ VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR };
	r = fmts(dev->pd, &fi, &nf, fp);
	if (r && r != VK_INCOMPLETE) FAIL("vkGetPhysicalDeviceVideoFormatPropertiesKHR -> %d", r);
	for (uint32_t i = 0; i < nf; i++) if (fp[i].format == want && fp[i].imageTiling == VK_IMAGE_TILING_OPTIMAL) v->fmt = fp[i].format;
	if (!v->fmt) FAIL("the device offers no optimal-tiled %s decode images", v->ten ? "P010" : "NV12");

	// the session (libavcodec's sizes: the device's largest picture, DPB slots and active references), its memory
	VkVideoSessionCreateInfoKHR sci = { VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR, NULL, (uint32_t)dev->dec_family,
		VK_VIDEO_SESSION_CREATE_INLINE_SESSION_PARAMETERS_BIT_KHR, &v->profile, v->fmt, v->caps.maxCodedExtent, v->fmt,
		v->caps.maxDpbSlots, v->caps.maxActiveReferencePictures, &v->caps.stdHeaderVersion };
	if (v->codec == CODEC_H264 && (v->caps.maxDpbSlots < H264D_SLOTS || v->caps.maxActiveReferencePictures < H264D_MAX_REFS))
		FAIL("the device has fewer than 17 DPB slots / 16 references");
	if (v->codec == CODEC_HEVC && (v->caps.maxDpbSlots < HEVCD_SLOTS || v->caps.maxActiveReferencePictures < 8))
		FAIL("the device has fewer than 16 DPB slots / 8 active references");
	if ((r = v->vk.CreateSession(dev->d, &sci, NULL, &v->session))) FAIL("vkCreateVideoSessionKHR -> %d", r);
	uint32_t nreq = 0;
	v->vk.SessionReqs(dev->d, v->session, &nreq, NULL);
	VkVideoSessionMemoryRequirementsKHR req[32];
	if (nreq > 32) FAIL("%u session memory bindings", nreq);
	for (uint32_t i = 0; i < nreq; i++) req[i] = (VkVideoSessionMemoryRequirementsKHR){ VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR };
	v->vk.SessionReqs(dev->d, v->session, &nreq, req);
	VkPhysicalDeviceMemoryProperties mp;
	((PFN_vkGetPhysicalDeviceMemoryProperties)dev->gipa(dev->inst, "vkGetPhysicalDeviceMemoryProperties"))(dev->pd, &mp);
	VkBindVideoSessionMemoryInfoKHR bind[32];
	for (uint32_t i = 0; i < nreq; i++) {
		int t = -1;
		for (uint32_t k = 0; k < mp.memoryTypeCount && t < 0; k++)   // device-local first, as libavutil's allocator
			if ((req[i].memoryRequirements.memoryTypeBits & (1u << k)) && (mp.memoryTypes[k].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) t = (int)k;
		for (uint32_t k = 0; k < mp.memoryTypeCount && t < 0; k++) if (req[i].memoryRequirements.memoryTypeBits & (1u << k)) t = (int)k;
		VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, NULL, req[i].memoryRequirements.size, (uint32_t)t };
		if (t < 0 || (r = v->vk.Alloc(dev->d, &mai, NULL, &v->smem[v->nsmem]))) FAIL("session memory: %d", r);
		bind[i] = (VkBindVideoSessionMemoryInfoKHR){ VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR, NULL, req[i].memoryBindIndex,
			v->smem[v->nsmem], 0, req[i].memoryRequirements.size };
		v->nsmem++;
	}
	if (nreq && (r = v->vk.BindSession(dev->d, v->session, nreq, bind))) FAIL("vkBindVideoSessionMemoryKHR -> %d", r);

	// the pictures: a libavutil frames context made as ff_vk_frame_params makes libavcodec's
	AVBufferRef *fr = av_hwframe_ctx_alloc(dev->avdev);
	if (!fr) FAIL("no memory");
	AVHWFramesContext *fc = (AVHWFramesContext *)fr->data;
	AVVulkanFramesContext *vfc = fc->hwctx;
	fc->format = AV_PIX_FMT_VULKAN; fc->sw_format = v->ten ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	fc->width = (int)((v->cw + 1) & ~1u); fc->height = (int)((v->ch + 1) & ~1u);
	vfc->format[0] = v->fmt;
	vfc->tiling = VK_IMAGE_TILING_OPTIMAL;
	vfc->usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
	             VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
	vfc->create_pnext = &v->plist;
	int e = av_hwframe_ctx_init(fr);
	v->frames = fr;
	if (e < 0) FAIL("av_hwframe_ctx_init -> %d", e);

	// the queue (sessions take the family's queues in turn, as libavcodec's patched exec pools do), commands, timeline
	static _Atomic unsigned phase;
	v->qidx = atomic_fetch_add(&phase, 1) % (unsigned)dev->dec_queues;
	v->vk.GetQueue(dev->d, (uint32_t)dev->dec_family, v->qidx, &v->q);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, (uint32_t)dev->dec_family };
	if ((r = v->vk.CreatePool(dev->d, &pci, NULL, &v->pool))) FAIL("vkCreateCommandPool -> %d", r);
	VkCommandBuffer cb[VRING];
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, v->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, VRING };
	if ((r = v->vk.AllocCmd(dev->d, &cai, cb))) FAIL("vkAllocateCommandBuffers -> %d", r);
	for (int i = 0; i < VRING; i++) v->ring[i].cmd = cb[i];
	VkSemaphoreTypeCreateInfo st = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
	VkSemaphoreCreateInfo semi = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st };
	if ((r = v->vk.CreateSem(dev->d, &semi, NULL, &v->vsem))) FAIL("vkCreateSemaphore -> %d", r);

	// the session starts with a reset (libavcodec's decode_reset)
	Ring *rg = &v->ring[v->next++ % VRING];
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	VkVideoBeginCodingInfoKHR begin = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR, NULL, 0, v->session, VK_NULL_HANDLE, 0, NULL };
	VkVideoCodingControlInfoKHR ctl = { VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR, NULL, VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR };
	VkVideoEndCodingInfoKHR end = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
	v->vk.Begin(rg->cmd, &bi);
	v->vk.BeginCoding(rg->cmd, &begin); v->vk.ControlCoding(rg->cmd, &ctl); v->vk.EndCoding(rg->cmd, &end);
	v->vk.End(rg->cmd);
	VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, rg->cmd, 0 };
	VkSemaphoreSubmitInfo sig = { VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, v->vsem, v->submitted + 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
	VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, 0, NULL, 1, &cbi, 1, &sig };
	if ((e = submit(v, &si))) FAIL("reset submit -> %d", e);
	rg->value = ++v->submitted;
	return v;
}

Vtd *vtd_open_h264(const VtdDevice *dev, const uint8_t *avcc, size_t n, char *whyp, size_t whyn) {
	Vtd *v = vtd_alloc(dev, CODEC_H264);
	if (!v) { snprintf(whyp, whyn, "no memory"); return NULL; }
	v->nslots = H264D_SLOTS;
	if (h264d_parse_avcc(&v->h, avcc, n) < 0) FAIL("avcC: %s", h264d_why(&v->h));
	const H264DSps *sps = &v->h.sps[v->h.last_sps];
	if (!sps->present) FAIL("no SPS in the avcC");
	if (sps->unsupported) {
		static const char *w[] = { "", "profile not Baseline/Main/High", "field or MBAFF coding", "not 8-bit 4:2:0", "frame_num gaps allowed" };
		FAIL("%s", w[sps->unsupported]);
	}
	for (int i = 0; i < 256; i++) if (v->h.pps[i] && v->h.pps[i]->num_slice_groups > 1) FAIL("slice groups (FMO)");
	v->cw = (uint32_t)sps->width_mbs * 16; v->ch = (uint32_t)sps->height_mbs * 16;
	// the profile, as libavcodec's vulkan_setup_profile makes it for this stream
	v->h264prof = (VkVideoDecodeH264ProfileInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR, NULL,
		(StdVideoH264ProfileIdc)sps->profile_idc, VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR };
	v->usage = (VkVideoDecodeUsageInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR, &v->h264prof, VK_VIDEO_DECODE_USAGE_DEFAULT_KHR };
	v->profile = (VkVideoProfileInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, &v->usage, VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR,
		VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR, VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR };
	v->hcaps = (VkVideoDecodeH264CapabilitiesKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR };
	v->dcaps.pNext = &v->hcaps;
	return open_session(v, whyp, whyn);
}

Vtd *vtd_open_hevc(const VtdDevice *dev, const uint8_t *hvcc, size_t n, char *whyp, size_t whyn) {
	Vtd *v = vtd_alloc(dev, CODEC_HEVC);
	if (!v) { snprintf(whyp, whyn, "no memory"); return NULL; }
	v->nslots = HEVCD_SLOTS;
	if (hevcd_parse_hvcc(&v->hv, hvcc, n) < 0) FAIL("hvcC: %s", hevcd_why(&v->hv));
	const HevcDSps *sps = v->hv.last_sps >= 0 ? v->hv.sps[v->hv.last_sps] : NULL;
	if (!sps) FAIL("no SPS in the hvcC");   // (hev1 with every parameter set in band: libavcodec decodes it)
	if (sps->unsupported) {
		static const char *w[] = { "", "profile not Main/Main 10/Main Still Picture", "not 4:2:0", "not 8 or 10 bits", "SPS extensions" };
		FAIL("%s", w[sps->unsupported]);
	}
	for (int i = 0; i < 64; i++) if (v->hv.pps[i] && v->hv.pps[i]->unsupported) FAIL("PPS extensions");
	v->cw = (uint32_t)sps->width; v->ch = (uint32_t)sps->height;
	v->ten = sps->bit_depth == 10;
	// the profile, as libavcodec's vulkan_setup_profile makes it: general_profile_idc, the stream's bit depth
	const VkVideoComponentBitDepthFlagBitsKHR depth = v->ten ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	v->h265prof = (VkVideoDecodeH265ProfileInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR, NULL, (StdVideoH265ProfileIdc)sps->profile };
	v->usage = (VkVideoDecodeUsageInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_USAGE_INFO_KHR, &v->h265prof, VK_VIDEO_DECODE_USAGE_DEFAULT_KHR };
	v->profile = (VkVideoProfileInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR, &v->usage, VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
		VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, depth, depth };
	v->h265caps = (VkVideoDecodeH265CapabilitiesKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR };
	v->dcaps.pNext = &v->h265caps;
	return open_session(v, whyp, whyn);
}
#undef FAIL

void vtd_flush(Vtd *v) {
	if (v->codec == CODEC_HEVC) hevcd_flush(&v->hv);
	else h264d_flush(&v->h);
	drop_slot_frames(v);
}

// one picture as the recording below needs it, whatever the codec
typedef struct {
	int nrefs; struct { int slot, long_term; } ref[MAX_REFS];
	int slot, missing;
	size_t size;
	const int *slot_busy;
} Desc;

int vtd_decode(Vtd *v, const uint8_t *data, size_t n, AVFrame **out) {
	*out = NULL;
	const int hevc = v->codec == CODEC_HEVC;
	const uint64_t t0 = now_ns();
	const size_t need = hevc ? hevcd_bitstream_size(&v->hv, data, n) : h264d_bitstream_size(&v->h, data, n);
	if (!need && n) {   // malformed, or no slice: parameter sets still count
		int r;
		if (hevc) { HevcDPicture pic; r = hevcd_parse_sample(&v->hv, data, n, NULL, 0, &pic); r = r == HEVCD_UNSUPPORTED ? VTD_FALLBACK : r < 0 ? VTD_ERROR : VTD_NO_PICTURE; }
		else { H264DPicture pic; r = h264d_parse_sample(&v->h, data, n, NULL, 0, &pic); r = r == H264D_UNSUPPORTED ? VTD_FALLBACK : r < 0 ? VTD_ERROR : VTD_NO_PICTURE; }
		if (r != VTD_NO_PICTURE) why(v, hevc ? hevcd_why(&v->hv) : h264d_why(&v->h));
		if (r == VTD_ERROR) v->st.errors++;
		return r;
	}
	Ring *rg = &v->ring[v->next % VRING];
	wait_value(v, rg->value);   // its last decode has completed: the command and bitstream buffers are free
	const VkDeviceSize align = v->caps.minBitstreamBufferSizeAlignment ? v->caps.minBitstreamBufferSizeAlignment : 1;
	const VkDeviceSize range = (need + align - 1) / align * align;   // libavcodec aligns the range the same way
	if (ring_reserve(v, rg, range)) { snprintf(v->why, sizeof v->why, "no bitstream buffer of %zu bytes", (size_t)range); v->st.errors++; return VTD_ERROR; }
	H264DPicture hpic; HevcDPicture vpic;
	Desc dsc = { 0 };
	int pr;
	if (hevc) {
		pr = hevcd_parse_sample(&v->hv, data, n, rg->map, (size_t)rg->size, &vpic);
		pr = pr == HEVCD_PICTURE ? 0 : pr == HEVCD_NO_PICTURE ? 1 : pr == HEVCD_UNSUPPORTED ? -1 : -2;
	} else {
		pr = h264d_parse_sample(&v->h, data, n, rg->map, (size_t)rg->size, &hpic);
		pr = pr == H264D_PICTURE ? 0 : pr == H264D_NO_PICTURE ? 1 : pr == H264D_UNSUPPORTED ? -1 : -2;
	}
	const uint64_t t1 = now_ns();
	v->st.parse_ns += t1 - t0;
	if (pr == -1) { why(v, hevc ? hevcd_why(&v->hv) : h264d_why(&v->h)); return VTD_FALLBACK; }
	if (pr < 0) { why(v, hevc ? hevcd_why(&v->hv) : h264d_why(&v->h)); v->st.errors++; return VTD_ERROR; }
	if (pr == 1) { if (hevc && v->hv.skipped) v->st.skipped++; return VTD_NO_PICTURE; }
	uint32_t cw, chh;
	if (hevc) {
		cw = (uint32_t)vpic.sps->width; chh = (uint32_t)vpic.sps->height;
		dsc.nrefs = vpic.nrefs; dsc.slot = vpic.slot; dsc.missing = vpic.missing; dsc.size = vpic.size; dsc.slot_busy = v->hv.slot_busy;
		for (int i = 0; i < vpic.nrefs; i++) { dsc.ref[i].slot = vpic.refs[i].slot; dsc.ref[i].long_term = vpic.refs[i].long_term; }
		if (vpic.sps->bit_depth != (v->ten ? 10 : 8)) {
			snprintf(v->why, sizeof v->why, "bit depth %d, the session's is %d", vpic.sps->bit_depth, v->ten ? 10 : 8);
			return VTD_FALLBACK;
		}
	} else {
		cw = (uint32_t)hpic.sps->width_mbs * 16; chh = (uint32_t)hpic.sps->height_mbs * 16;
		dsc.nrefs = hpic.nrefs; dsc.slot = hpic.slot; dsc.size = hpic.size; dsc.slot_busy = v->h.slot_busy;
		dsc.missing = !hpic.is_intra && !hpic.nrefs;
		for (int i = 0; i < hpic.nrefs; i++) { dsc.ref[i].slot = hpic.refs[i].slot; dsc.ref[i].long_term = hpic.refs[i].long_term; }
		v->st.gaps = v->h.gaps; v->st.redundant_slices = v->h.redundant_slices;
	}
	if (cw != v->cw || chh != v->ch) {
		// not the size the session and its pictures were made for (an SPS in the stream that the avcC/hvcC did not have)
		snprintf(v->why, sizeof v->why, "coded size %ux%u, the session's is %ux%u", cw, chh, v->cw, v->ch);
		return VTD_FALLBACK;
	}

	AVFrame *f = av_frame_alloc();
	if (!f || av_hwframe_get_buffer(v->frames, f, 0) < 0) {
		// no picture to decode into: the DPB still moves on, as if it had been decoded (its references go missing)
		av_frame_free(&f); why(v, "no picture buffer"); v->st.errors++;
		if (hevc) hevcd_finish_picture(&v->hv, &vpic); else h264d_finish_picture(&v->h, &hpic);
		for (int i = 0; i < v->nslots; i++) if (!dsc.slot_busy[i] && v->slot_frame[i]) av_frame_free(&v->slot_frame[i]);
		return VTD_ERROR;
	}
	if (vtd_full_range(v) >= 0) f->color_range = vtd_full_range(v) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	// the frames this decode touches: the picture, then its references
	AVFrame *fr[1 + MAX_REFS]; int nfr = 0;
	fr[nfr++] = f;
	int missing = 0;
	for (int i = 0; i < dsc.nrefs; i++) { AVFrame *rf = v->slot_frame[dsc.ref[i].slot]; if (rf) fr[nfr++] = rf; else missing++; }
	int status = VTD_PICTURE;
	if (missing || dsc.missing) {
		// a P/B picture without its references (after a flush or a seek to a non-IDR picture): the hardware would read
		// pictures that are not there. Not decoded. H.264: it still takes its place in the DPB (contents undefined) so
		// that the following pictures see the references the stream describes; HEVC: as libavcodec, it does not.
		v->st.no_ref_skips++;
		snprintf(v->why, sizeof v->why, "a P/B picture without its references (%d missing)", missing ? missing : 1);
		status = VTD_ERROR;
		if (hevc) vpic.missing = 1;
	} else {
		VkImageView view = view_of(v, f);
		StdVideoDecodeH264ReferenceInfo sref[MAX_REFS + 1];
		VkVideoDecodeH264DpbSlotInfoKHR sdpb[MAX_REFS + 1];
		StdVideoDecodeH265ReferenceInfo sref5[MAX_REFS + 1];
		VkVideoDecodeH265DpbSlotInfoKHR sdpb5[MAX_REFS + 1];
		VkVideoPictureResourceInfoKHR res[MAX_REFS + 1];
		VkVideoReferenceSlotInfoKHR slots[MAX_REFS + 2];
		const VkExtent2D ext = { v->cw, v->ch };
		int bad_view = !view;
		for (int i = 0; i <= dsc.nrefs; i++) {
			const int cur = i == dsc.nrefs;
			const void *slot_info;
			if (hevc) {
				sref5[i] = (StdVideoDecodeH265ReferenceInfo){
					.flags = { .used_for_long_term_reference = cur ? 0 : (uint32_t)vpic.refs[i].long_term, .unused_for_reference = 0 },
					.PicOrderCntVal = cur ? vpic.poc : vpic.refs[i].poc };
				sdpb5[i] = (VkVideoDecodeH265DpbSlotInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_DPB_SLOT_INFO_KHR, NULL, &sref5[i] };
				slot_info = &sdpb5[i];
			} else {
				const H264DRef *rf = cur ? NULL : &hpic.refs[i];
				sref[i] = (StdVideoDecodeH264ReferenceInfo){
					.flags = { .top_field_flag = 0, .bottom_field_flag = 0, .used_for_long_term_reference = cur ? 0 : (uint32_t)rf->long_term, .is_non_existing = 0 },
					.FrameNum = (uint16_t)(cur ? hpic.frame_num : rf->frame_num),
					.PicOrderCnt = { cur ? hpic.poc[0] : rf->poc[0], cur ? hpic.poc[1] : rf->poc[1] } };
				sdpb[i] = (VkVideoDecodeH264DpbSlotInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR, NULL, &sref[i] };
				slot_info = &sdpb[i];
			}
			const VkImageView rv = cur ? view : view_of(v, v->slot_frame[dsc.ref[i].slot]);
			if (!rv) bad_view = 1;
			res[i] = (VkVideoPictureResourceInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR, NULL, { 0, 0 }, ext, 0, rv };
			slots[i] = (VkVideoReferenceSlotInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, slot_info, cur ? dsc.slot : dsc.ref[i].slot, &res[i] };
		}
		// the current picture is also bound, inactive, at the end of vkCmdBeginVideoCodingKHR's list
		slots[dsc.nrefs + 1] = slots[dsc.nrefs]; slots[dsc.nrefs + 1].slotIndex = -1;
		VkVideoDecodeH264InlineSessionParametersInfoKHR inl4;
		VkVideoDecodeH264PictureInfoKHR hpi4;
		VkVideoDecodeH265InlineSessionParametersInfoKHR inl5;
		VkVideoDecodeH265PictureInfoKHR hpi5;
		const void *pic_info;
		if (hevc) {
			inl5 = (VkVideoDecodeH265InlineSessionParametersInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_INLINE_SESSION_PARAMETERS_INFO_KHR, NULL,
				vpic.vps ? &vpic.vps->std : NULL, &vpic.sps->std, &vpic.pps->std };
			hpi5 = (VkVideoDecodeH265PictureInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PICTURE_INFO_KHR, &inl5, &vpic.std, vpic.nslices, vpic.slice_offsets };
			pic_info = &hpi5;
		} else {
			inl4 = (VkVideoDecodeH264InlineSessionParametersInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_INLINE_SESSION_PARAMETERS_INFO_KHR, NULL,
				&hpic.sps->std, &hpic.pps->std };
			hpi4 = (VkVideoDecodeH264PictureInfoKHR){ VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR, &inl4, &hpic.std, hpic.nslices, hpic.slice_offsets };
			pic_info = &hpi4;
		}
		VkVideoDecodeInfoKHR di = { VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, pic_info, 0, rg->buf, 0, range, res[dsc.nrefs],
			&slots[dsc.nrefs], (uint32_t)dsc.nrefs, slots };
		VkVideoBeginCodingInfoKHR begin = { VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR, NULL, 0, v->session, VK_NULL_HANDLE,
			(uint32_t)dsc.nrefs + 1, NULL };
		VkVideoReferenceSlotInfoKHR bslots[MAX_REFS + 1];
		for (int i = 0; i < dsc.nrefs; i++) bslots[i] = slots[i];
		bslots[dsc.nrefs] = slots[dsc.nrefs + 1];
		begin.pReferenceSlots = bslots;
		VkVideoEndCodingInfoKHR end = { VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR };
		if (range > need) memset(rg->map + need, 0, (size_t)(range - need));
		if (!v->host_coherent) {
			VkMappedMemoryRange mr = { VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE, NULL, rg->mem, 0, VK_WHOLE_SIZE };
			v->vk.FlushRanges(v->dev.d, 1, &mr);
		}

		// under each frame's lock: its layout to the DPB layout, a wait on its timeline value and the next one signalled
		VkImageMemoryBarrier2 bar[1 + MAX_REFS];
		VkSemaphoreSubmitInfo wait[1 + MAX_REFS], sig[2 + MAX_REFS];
		for (int i = 0; i < nfr; i++) {
			AVHWFramesContext *hfc = (AVHWFramesContext *)fr[i]->hw_frames_ctx->data;
			AVVulkanFramesContext *hvfc = hfc->hwctx;
			AVVkFrame *vkf = (AVVkFrame *)fr[i]->data[0];
			hvfc->lock_frame(hfc, vkf);
			bar[i] = (VkImageMemoryBarrier2){ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
				VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR, VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR,
				i ? VK_ACCESS_2_VIDEO_DECODE_READ_BIT_KHR | VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR : VK_ACCESS_2_VIDEO_DECODE_WRITE_BIT_KHR,
				vkf->layout[0], VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkf->img[0],
				{ VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
			wait[i] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vkf->sem[0], vkf->sem_value[0], VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR };
			sig[i] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vkf->sem[0], vkf->sem_value[0] + 1, VK_PIPELINE_STAGE_2_VIDEO_DECODE_BIT_KHR };
		}
		sig[nfr] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, v->vsem, v->submitted + 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		const uint64_t t2 = now_ns();
		int e = bad_view ? -1 : 0;
		if (!e) {
			v->vk.ResetCmd(rg->cmd, 0);
			VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
			v->vk.Begin(rg->cmd, &bi);
			VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, VK_DEPENDENCY_BY_REGION_BIT, 0, NULL, 0, NULL, (uint32_t)nfr, bar };
			v->vk.Barrier(rg->cmd, &dep);
			v->vk.BeginCoding(rg->cmd, &begin);
			v->vk.Decode(rg->cmd, &di);
			v->vk.EndCoding(rg->cmd, &end);
			e = v->vk.End(rg->cmd);
		}
		const uint64_t t3 = now_ns();
		if (!e) {
			VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, rg->cmd, 0 };
			VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, (uint32_t)nfr, wait, 1, &cbi, (uint32_t)nfr + 1, sig };
			e = submit(v, &si);
		}
		for (int i = 0; i < nfr; i++) {
			AVHWFramesContext *hfc = (AVHWFramesContext *)fr[i]->hw_frames_ctx->data;
			AVVulkanFramesContext *hvfc = hfc->hwctx;
			AVVkFrame *vkf = (AVVkFrame *)fr[i]->data[0];
			if (!e) {
				vkf->sem_value[0]++;
				vkf->layout[0] = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
				vkf->access[0] = bar[i].dstAccessMask;
				vkf->queue_family[0] = VK_QUEUE_FAMILY_IGNORED;
			}
			hvfc->unlock_frame(hfc, vkf);
		}
		v->st.record_ns += t3 - t2 + (t2 - t1);
		v->st.submit_ns += now_ns() - t3;
		if (e) { snprintf(v->why, sizeof v->why, "decode submit -> %d", e); v->st.errors++; status = VTD_ERROR; }
		else { rg->value = ++v->submitted; v->next++; v->st.pictures++; v->st.bytes += dsc.size; }
	}
	// the DPB after the picture; the slots' frames follow it
	if (hevc) hevcd_finish_picture(&v->hv, &vpic); else h264d_finish_picture(&v->h, &hpic);
	if (dsc.slot_busy[dsc.slot]) { av_frame_free(&v->slot_frame[dsc.slot]); v->slot_frame[dsc.slot] = av_frame_clone(f); }
	for (int i = 0; i < v->nslots; i++) if (!dsc.slot_busy[i] && v->slot_frame[i]) av_frame_free(&v->slot_frame[i]);
	if (status != VTD_PICTURE) { av_frame_free(&f); return status; }
	*out = f;
	return VTD_PICTURE;
}
