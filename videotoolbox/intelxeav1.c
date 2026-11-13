// SPDX-License-Identifier: MIT
// IntelXe AV1 decoder for VideoToolbox. AV1 temporal units are decoded on the B580's video engine through Vulkan Video (Mesa
// ANV, /Library/GPUBundles/nvmtl/libvulkan_intel.dylib), with libavcodec's AV1 parser and Vulkan hwaccel doing the
// bitstream and reference work. FFmpeg is used unmodified, as the shared libraries next to this one.
//
// - One Vulkan device per process while it decodes: the first session creates it (instance, B580, every queue family
//   libavcodec may use), hands it to libavutil with av_hwdevice_ctx_init(), every later session shares it, and the
//   last session to go destroys it (an idle process, VTDecoderXPCService included, holds no device and no VRAM).
// - No CPU copy: each decoded picture is copied by the B580's copy engine straight into the IOSurface of the pixel
//   buffer VideoToolbox handed out. The IOSurface's pages are imported once per surface (VK_EXT_external_memory_host)
//   and the copy waits on, and advances, the picture's timeline semaphore like libavutil's own transfers do.
// - Pipelined: DecodeFrame returns once the copy is submitted (asynchronous decompression); a serial queue per session
//   waits for each copy and emits the pictures in order, so the next temporal unit decodes while the last one copies.
//   The emit queue also holds a reference to the decoded picture until its copy has completed, so libavcodec's frame
//   pool cannot hand the image to a new decode while the copy engine still reads it, and the picture's last reference
//   (with libavcodec's per-picture cleanup) goes on that queue, not on the decoding thread.
// - The copy and the decodes that read the picture as a reference. ANV only offers DPB_AND_OUTPUT_COINCIDE, so
//   libavcodec decodes in place: the output picture IS the reference picture of the following decodes. That holds for
//   AV1 too: vulkan_av1.c takes a DPB picture of its own only for film grain (alloc_dpb = has_grain || dedicated_dpb);
//   the DPB pool vulkan_decode.c creates for AV1 is otherwise unused. The copy moves the picture to
//   TRANSFER_SRC_OPTIMAL, so it must be ordered against those decodes, which move it back to VIDEO_DECODE_DPB: the
//   copy waits on and advances the picture's timeline semaphores as libavutil's own transfers do (s->advance), and the
//   next decode that references the picture waits for the copy (ff_vk_decode_frame -> ff_vk_exec_add_dep_frame).
//   AV1 keeps its older rule by default (the copy only waits; see codecs-dec4 in the report) unless INTELXEDEC_COPY_DEFER=all.
// - Deferred copy (codecs-dec4, VP9 by default). A copy submitted right after its picture's decode made the NEXT
//   decode, which references that picture (VP9: LAST is always the previous frame), wait for the copy: decode and
//   copy ran one after the other (VP9 4K 10-bit: 528 fps decoding alone, 228 with the copy). The copy of a shown
//   picture is therefore submitted after the next picture's decode has been submitted: it then waits for that decode
//   (which reads the picture in the DPB layout) and runs on the copy engine while the decode after it runs on the
//   VDBOX. A decode that references the picture later still waits for the copy (correct, and rare: golden / altref
//   refreshes). The deferred copy also goes when anything else is emitted (errors, drops: decode order is kept), at
//   FinishDelayedFrames, in synchronous DecodeFrame calls, at teardown, and after INTELXEDEC_COPY_DEFER_MS (12 ms) without
//   a next picture, so a client that waits for each output before sending the next sample still gets it. 12 ms is
//   above the decode period of the largest stream (8K VP9 10-bit: 6.6 ms); a timer that fires first only makes that
//   one copy go before the next decode, as before.
//   INTELXEDEC_COPY_DEFER=0 turns it off, =all turns it on for every codec (tests); INTELXEDEC_TRACE=<prefix> writes one
//   line per picture (CPU times of send_packet and copy submission, GPU start / end of the copy) to <prefix>.<pid>.<n>.txt.
// - The pixel buffers: planes and rows 64-byte aligned (kCVPixelBufferPlaneAlignmentKey / BytesPerRowAlignmentKey) and
//   Metal compatible. CoreVideo otherwise starts plane 0 of a 420v/x420 IOSurface 32 bytes in, and the copy engine
//   writes every row as partial cache lines over PCIe: 2.7 instead of 0.95 ms per 4K 8-bit picture. (A client that
//   asks for Metal compatibility already got such buffers: VideoToolbox merges its attributes into the pool's.)
// - The range: full-range streams get '420f' / 'xf20' buffers, video-range ones '420v' / 'x420', like Apple's decoders
//   (the format description's FullRangeVideo, else VP9's vpcC flag, else the first picture). With video-range buffers
//   only, a client asking for the stream's own format ('420f' for a full-range 4K stream, Video Codec Test) made
//   VideoToolbox convert every picture (45-59 fps instead of 270-720) and expand the range a second time.
// - Streams larger than the device's video decode capabilities (vkGetPhysicalDeviceVideoCapabilitiesKHR) are refused at
//   StartSession, as Apple's decoders refuse what their hardware cannot decode.
// - The Vulkan driver: when the process already has a libvulkan_intel.dylib with the same LC_UUID as INTELXE_ANV loaded
//   (the Metal plug-in's own copy of the same Mesa build), that one is used: a second copy cost ~0.3 s of dyld binding
//   at the first session and a second driver instance in the process.
// - One picture per DecodeFrame. AVFoundation hands over whole temporal units (one shown frame each); FFmpeg's
//   VideoToolbox hwaccel (ffmpeg -hwaccel videotoolbox, mpv, IINA...) hands over one FRAME at a time, hidden frames
//   (show_frame = 0, the alt-refs every libaom/SVT-AV1 stream has) included, waits for each one's picture, and keeps
//   that picture to show later itself when a show_existing_frame header comes (which it never sends). A sample that
//   decodes a frame but shows none is therefore answered with that decoded frame: libavcodec allocates every frame it
//   decodes through get_buffer2, so the last allocation is the hidden frame.
// - H.264 (High and below, 8-bit) and HEVC (Main, Main10) the same way (IntelXeH264Dec_CreateInstance,
//   IntelXeHEVCDec_CreateInstance), as the hardware decoder of last resort when the system has none for them. These answer
//   every sample in DECODE order, with the picture that sample decoded, like Apple's hardware decoders do (the client
//   reorders by presentation time): libavcodec's own output, delayed and reordered for display, is not used.
// - VP9 (Profile 0 8-bit and Profile 2 10-bit, 4:2:0; IntelXeVP9Dec_CreateInstance) like AV1: one shown picture per sample
//   (an MP4/WebM sample is a superframe: hidden alt-ref frames and the shown one; libavcodec splits it), and a sample
//   that shows none (a client that sends the frames of a superframe one by one) is answered with the frame it decoded.
//   It decodes in place, the copy advances the picture's semaphores, and its copies are deferred (see above).
// - The direct path (INTELXEDEC_DIRECT=h264,hevc,av1,vp9, off by default; vtdirect.c, vtdirect_h264.c, vtdirect_hevc.c,
//   vtdirect_av1.c, vtdirect_vp9.c): H.264, HEVC, AV1 and VP9 samples go to vkCmdDecodeVideoKHR with our own header parsing and DPB instead of libavcodec's decoder and
//   Vulkan hwaccel, into frames of a libavutil frames context made like libavcodec's, so everything after the decode
//   (output, the copy, its deferral, the emit queue) is the same. libavcodec is opened only for streams it does not cover, or from the sample
//   on which it hands a stream over. IntelXeDec_LastStats gives the last session's line (path, fps, CPU per call).
#include "vtplugin.h"
#include <IOSurface/IOSurfaceRef.h>
#include <os/log.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <limits.h>
#include <libproc.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <mach-o/loader.h>
#include <vulkan/vulkan.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_vulkan.h>
#include <libavutil/pixdesc.h>
#include "vtdirect.h"
#include "vtdirect_av1.h"
#include "vtdirect_vp9.h"

#ifndef INTELXE_ANV
#define INTELXE_ANV "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
#endif
#define RING 8          // copies in flight per session
#define SURFACES 32     // imported IOSurfaces per session
#define TRACE_Q 256     // INTELXEDEC_TRACE: timestamp query pairs, indexed by copy timeline value

static os_log_t lg(void) { static os_log_t l; static dispatch_once_t o; dispatch_once(&o, ^{ l = os_log_create("io.github.yusufklncc.intelxe.av1", "decoder"); }); return l; }
static const char *proc(void) { static char n[64]; if (!n[0]) proc_name(getpid(), n, sizeof n); return n; }
static uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }

// ---------------------------------------------------------------------------------------------------------------------
// The process's Vulkan device

static struct {
	pthread_mutex_t lock;
	AVBufferRef *dev;                 // AV_HWDEVICE_TYPE_VULKAN, kept while sessions use it
	int users;                        // sessions holding a reference (shared_device / release_device)
	PFN_vkGetInstanceProcAddr gipa;
	VkInstance inst; VkPhysicalDevice pd; VkDevice d;
	VkPhysicalDeviceFeatures2 f; VkPhysicalDeviceVulkan11Features f11; VkPhysicalDeviceVulkan12Features f12;
	VkPhysicalDeviceVulkan13Features f13; VkPhysicalDeviceVideoMaintenance1FeaturesKHR fvm1;
	VkPhysicalDeviceVideoMaintenance2FeaturesKHR fvm2;
	const char *exts[64]; int nexts;
	int copy_family, has_host_import;
	int dec_family, dec_queues;       // the video decode family and its queues (the direct path, vtdirect.c)
	int inline_params;                // VK_KHR_video_maintenance2 and its feature are enabled
	VkDeviceSize host_align;
	float ts_period;                  // ns per GPU timestamp tick (INTELXEDEC_TRACE)
} G = { .lock = PTHREAD_MUTEX_INITIALIZER, .copy_family = -1, .dec_family = -1 };

static void av_log_to_os_log(void *avcl, int level, const char *fmt, va_list vl) {
	if (level > AV_LOG_ERROR) return;
	char b[512]; vsnprintf(b, sizeof b, fmt, vl);
	os_log_error(lg(), "intelxeav1: libav: %{public}s", b);
}

static int uuid_of_header(const struct mach_header_64 *mh, uint8_t out[16]) {
	if (!mh || mh->magic != MH_MAGIC_64) return 0;
	const uint8_t *p = (const uint8_t *)(mh + 1);
	for (uint32_t i = 0; i < mh->ncmds; i++) {
		const struct load_command *lc = (const struct load_command *)p;
		if (lc->cmd == LC_UUID) { memcpy(out, ((const struct uuid_command *)lc)->uuid, 16); return 1; }
		p += lc->cmdsize;
	}
	return 0;
}
static int uuid_of_file(const char *path, uint8_t out[16]) {   // a thin x86_64 Mach-O (the driver as installed)
	uint8_t b[32768];
	const int fd = open(path, O_RDONLY);
	if (fd < 0) return 0;
	const ssize_t n = pread(fd, b, sizeof b, 0);
	close(fd);
	const struct mach_header_64 *mh = (const struct mach_header_64 *)b;
	if (n < (ssize_t)sizeof *mh || mh->magic != MH_MAGIC_64 || sizeof *mh + mh->sizeofcmds > (size_t)n) return 0;
	return uuid_of_header(mh, out);
}
// INTELXE_ANV, or the process's already loaded copy of the same build (same LC_UUID) under another path
static void *open_anv(const char **path) {
	static const char leaf[] = "/libvulkan_intel.dylib";
	uint8_t want[16], have[16];
	if (uuid_of_file(INTELXE_ANV, want))
		for (uint32_t i = 0; i < _dyld_image_count(); i++) {
			const char *n = _dyld_get_image_name(i);
			const size_t l = n ? strlen(n) : 0;
			if (l < sizeof leaf - 1 || strcmp(n + l - (sizeof leaf - 1), leaf)) continue;
			if (!uuid_of_header((const struct mach_header_64 *)_dyld_get_image_header(i), have) || memcmp(have, want, 16)) continue;
			void *h = dlopen(n, RTLD_NOW | RTLD_NOLOAD);
			if (h) { *path = n; return h; }
		}
	*path = INTELXE_ANV;
	return dlopen(INTELXE_ANV, RTLD_NOW | RTLD_LOCAL);
}

#define GI(name) PFN_##name name = (PFN_##name)G.gipa(G.inst, #name)
static int create_device(void) {
	const char *anv = INTELXE_ANV;
	void *h = open_anv(&anv);
	G.gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr") : NULL;
	if (!G.gipa) { os_log_error(lg(), "intelxeav1: %{public}s: %{public}s", anv, dlerror()); return -1; }
	PFN_vkCreateInstance ci = (PFN_vkCreateInstance)G.gipa(NULL, "vkCreateInstance");
	VkApplicationInfo ai = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "intelxeav1", 1, "libavcodec", LIBAVCODEC_VERSION_INT, VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai };
	VkResult r = ci(&ici, NULL, &G.inst);
	if (r) { os_log_error(lg(), "intelxeav1: vkCreateInstance -> %d", r); return -1; }
	GI(vkEnumeratePhysicalDevices); GI(vkEnumerateDeviceExtensionProperties); GI(vkGetPhysicalDeviceFeatures2);
	GI(vkGetPhysicalDeviceQueueFamilyProperties2); GI(vkCreateDevice); GI(vkGetPhysicalDeviceProperties2);
	uint32_t n = 1;
	r = vkEnumeratePhysicalDevices(G.inst, &n, &G.pd);
	if ((r && r != VK_INCOMPLETE) || !n) { os_log_error(lg(), "intelxeav1: no physical device (%d)", r); return -1; }

	// extensions: what libavutil can use (av_vk_get_optional_device_extensions) and the B580 has, without host image
	// copy (libavutil would then give its frames host-transfer usage and download them on the CPU) and without
	// internally synchronized queues: with that extension enabled libavutil drops its queue mutexes (lock_queue becomes
	// a no-op), but the queues below are created without VK_DEVICE_QUEUE_CREATE_INTERNALLY_SYNCHRONIZED_BIT_KHR, so
	// nothing serialised the sessions of one process on their shared queue (two vkQueueSubmit2 at once crashed in ANV)
	uint32_t ne = 0; vkEnumerateDeviceExtensionProperties(G.pd, NULL, &ne, NULL);
	VkExtensionProperties *ep = calloc(ne, sizeof *ep); vkEnumerateDeviceExtensionProperties(G.pd, NULL, &ne, ep);
	int nopt = 0; const char **opt = av_vk_get_optional_device_extensions(&nopt);
	for (int i = 0; i < nopt && G.nexts < 64; i++) {
		if (!strcmp(opt[i], VK_EXT_HOST_IMAGE_COPY_EXTENSION_NAME)) continue;
#ifdef VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME
		if (!strcmp(opt[i], VK_KHR_INTERNALLY_SYNCHRONIZED_QUEUES_EXTENSION_NAME)) continue;
#endif
		for (uint32_t j = 0; j < ne; j++) if (!strcmp(opt[i], ep[j].extensionName)) { G.exts[G.nexts++] = opt[i]; break; }
	}
	av_free(opt);
	for (int i = 0; i < G.nexts; i++) if (!strcmp(G.exts[i], VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) G.has_host_import = 1;
	free(ep);

	// features: the ones libavutil turns on for a device it creates itself, where the B580 has them
	VkPhysicalDeviceVideoMaintenance2FeaturesKHR svm2 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR };
	VkPhysicalDeviceVideoMaintenance1FeaturesKHR svm1 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR, &svm2 };
	VkPhysicalDeviceVulkan13Features s13 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, &svm1 };
	VkPhysicalDeviceVulkan12Features s12 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &s13 };
	VkPhysicalDeviceVulkan11Features s11 = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &s12 };
	VkPhysicalDeviceFeatures2 s = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &s11 };
	vkGetPhysicalDeviceFeatures2(G.pd, &s);
	G.fvm1 = (VkPhysicalDeviceVideoMaintenance1FeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR };
	G.f13 = (VkPhysicalDeviceVulkan13Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES };
	G.f12 = (VkPhysicalDeviceVulkan12Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, &G.f13 };
	G.f11 = (VkPhysicalDeviceVulkan11Features){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, &G.f12 };
	G.f = (VkPhysicalDeviceFeatures2){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &G.f11 };
	G.fvm2 = (VkPhysicalDeviceVideoMaintenance2FeaturesKHR){ VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_2_FEATURES_KHR };
	int vm1 = 0, vm2 = 0;
	for (int i = 0; i < G.nexts; i++) if (!strcmp(G.exts[i], VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME)) vm1 = 1;
	for (int i = 0; i < G.nexts; i++) if (!strcmp(G.exts[i], VK_KHR_VIDEO_MAINTENANCE_2_EXTENSION_NAME)) vm2 = 1;
	void **tail = &G.f13.pNext;
	if (vm1) { *tail = &G.fvm1; tail = &G.fvm1.pNext; G.fvm1.videoMaintenance1 = svm1.videoMaintenance1; }
	// VK_KHR_video_maintenance2: libavcodec passes the parameter sets inline once the extension is enabled; its feature
	// is enabled with it (the direct path relies on it too)
	if (vm2 && svm2.videoMaintenance2) { *tail = &G.fvm2; G.fvm2.videoMaintenance2 = VK_TRUE; G.inline_params = 1; }
#define C(dst, src, x) dst.x = src.x
	C(G.f, s, features.shaderImageGatherExtended); C(G.f, s, features.shaderStorageImageReadWithoutFormat);
	C(G.f, s, features.shaderStorageImageWriteWithoutFormat); C(G.f, s, features.fragmentStoresAndAtomics);
	C(G.f, s, features.vertexPipelineStoresAndAtomics); C(G.f, s, features.shaderInt64); C(G.f, s, features.shaderInt16);
	C(G.f11, s11, samplerYcbcrConversion); C(G.f11, s11, storagePushConstant16); C(G.f11, s11, storageBuffer16BitAccess);
	C(G.f11, s11, uniformAndStorageBuffer16BitAccess);
	C(G.f12, s12, timelineSemaphore); C(G.f12, s12, scalarBlockLayout); C(G.f12, s12, bufferDeviceAddress);
	C(G.f12, s12, hostQueryReset); C(G.f12, s12, storagePushConstant8); C(G.f12, s12, shaderInt8);
	C(G.f12, s12, storageBuffer8BitAccess); C(G.f12, s12, uniformAndStorageBuffer8BitAccess); C(G.f12, s12, shaderFloat16);
	C(G.f12, s12, vulkanMemoryModel); C(G.f12, s12, vulkanMemoryModelDeviceScope); C(G.f12, s12, uniformBufferStandardLayout);
	C(G.f12, s12, runtimeDescriptorArray); C(G.f12, s12, shaderSubgroupExtendedTypes);
	C(G.f13, s13, dynamicRendering); C(G.f13, s13, maintenance4); C(G.f13, s13, synchronization2);
	C(G.f13, s13, computeFullSubgroups); C(G.f13, s13, subgroupSizeControl); C(G.f13, s13, shaderZeroInitializeWorkgroupMemory);
#undef C
	if (!G.f12.timelineSemaphore || !G.f13.synchronization2) { os_log_error(lg(), "intelxeav1: no timeline semaphores or synchronization2"); return -1; }

	// queues: one of each family (two of the video decode family, for two sessions at once); the copy engine is the
	// transfer family without graphics or compute (the B580's BCS)
	uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties2(G.pd, &nq, NULL);
	if (nq > 16) nq = 16;
	VkQueueFamilyProperties2 qp[16]; VkQueueFamilyVideoPropertiesKHR vp[16];
	for (uint32_t i = 0; i < nq; i++) {
		vp[i] = (VkQueueFamilyVideoPropertiesKHR){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR };
		qp[i] = (VkQueueFamilyProperties2){ VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, &vp[i] };
	}
	vkGetPhysicalDeviceQueueFamilyProperties2(G.pd, &nq, qp);
	static const float prio[4] = { 1, 1, 1, 1 };
	VkDeviceQueueCreateInfo qci[16]; uint32_t nqci = 0;
	AVHWDeviceContext *dc = NULL;
	AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
	if (!ref) return -1;
	dc = (AVHWDeviceContext *)ref->data;
	AVVulkanDeviceContext *hw = dc->hwctx;
	for (uint32_t i = 0; i < nq; i++) {
		const VkQueueFlags fl = qp[i].queueFamilyProperties.queueFlags;
		if (!(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR))) continue;
		uint32_t cnt = (fl & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ? 2 : 1;
		if (cnt > qp[i].queueFamilyProperties.queueCount) cnt = qp[i].queueFamilyProperties.queueCount;
		qci[nqci++] = (VkDeviceQueueCreateInfo){ VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, i, cnt, prio };
		// libavutil's list: the video families only for what they decode (encode stays with the encoder's own device)
		VkQueueFlags use = fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT | VK_QUEUE_VIDEO_DECODE_BIT_KHR);
		hw->qf[hw->nb_qf++] = (AVVulkanDeviceQueueFamily){ (int)i, (int)cnt, use,
			(fl & VK_QUEUE_VIDEO_DECODE_BIT_KHR) ? vp[i].videoCodecOperations : 0 };
		if (G.copy_family < 0 && (fl & VK_QUEUE_TRANSFER_BIT) && !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT))) G.copy_family = (int)i;
		if (G.dec_family < 0 && (fl & VK_QUEUE_VIDEO_DECODE_BIT_KHR) && (vp[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)) {
			G.dec_family = (int)i; G.dec_queues = (int)cnt;
		}
	}
	if (G.copy_family < 0)   // no dedicated copy engine: any transfer-capable family
		for (int i = 0; i < hw->nb_qf; i++) if (hw->qf[i].flags & VK_QUEUE_TRANSFER_BIT) { G.copy_family = hw->qf[i].idx; break; }
	VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &G.f, 0, nqci, qci, 0, NULL, (uint32_t)G.nexts, G.exts, NULL };
	r = vkCreateDevice(G.pd, &dci, NULL, &G.d);
	if (r) { os_log_error(lg(), "intelxeav1: vkCreateDevice -> %d", r); av_buffer_unref(&ref); return -1; }

	VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
	VkPhysicalDeviceProperties2 pp = { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, G.has_host_import ? &hp : NULL };
	vkGetPhysicalDeviceProperties2(G.pd, &pp);
	G.host_align = G.has_host_import ? hp.minImportedHostPointerAlignment : 0;
	G.ts_period = pp.properties.limits.timestampPeriod;

	hw->get_proc_addr = G.gipa; hw->inst = G.inst; hw->phys_dev = G.pd; hw->act_dev = G.d;
	hw->device_features = G.f;
	hw->enabled_dev_extensions = G.exts; hw->nb_enabled_dev_extensions = G.nexts;
	int err = av_hwdevice_ctx_init(ref);
	if (err < 0) { char e[128]; av_strerror(err, e, sizeof e); os_log_error(lg(), "intelxeav1: av_hwdevice_ctx_init: %{public}s", e); av_buffer_unref(&ref); return -1; }
	G.dev = ref;
	os_log(lg(), "intelxeav1: Vulkan device for %{public}s: %{public}s (%{public}s), %d extensions, copy engine family %d, host import %d (align %llu)",
	       proc(), pp.properties.deviceName, anv, G.nexts, G.copy_family, G.has_host_import, (unsigned long long)G.host_align);
	return 0;
}

// a reference to the process's device, created by the first caller (a failed attempt is retried by the next session)
static AVBufferRef *shared_device(void) {
	pthread_mutex_lock(&G.lock);
	if (!G.dev && !G.inst) {
		static dispatch_once_t o; dispatch_once(&o, ^{ av_log_set_callback(av_log_to_os_log); });
		if (create_device() < 0 && G.inst) {
			if (G.d) ((PFN_vkDestroyDevice)G.gipa(G.inst, "vkDestroyDevice"))(G.d, NULL);
			((PFN_vkDestroyInstance)G.gipa(G.inst, "vkDestroyInstance"))(G.inst, NULL);
			G.inst = NULL; G.d = NULL; G.nexts = 0; G.copy_family = -1; G.dec_family = -1; G.inline_params = 0;
		}
	}
	AVBufferRef *r = G.dev ? av_buffer_ref(G.dev) : NULL;
	if (r) G.users++;
	pthread_mutex_unlock(&G.lock);
	return r;
}

// a session's reference is gone (after its own av_buffer_unref): the last one takes the device down
static void release_device(void) {
	pthread_mutex_lock(&G.lock);
	if (G.users && !--G.users && G.dev) {
		((PFN_vkDeviceWaitIdle)G.gipa(G.inst, "vkDeviceWaitIdle"))(G.d);
		av_buffer_unref(&G.dev);   // libavutil's context only; the device and instance are ours
		((PFN_vkDestroyDevice)G.gipa(G.inst, "vkDestroyDevice"))(G.d, NULL);
		((PFN_vkDestroyInstance)G.gipa(G.inst, "vkDestroyInstance"))(G.inst, NULL);
		G.inst = NULL; G.d = NULL; G.pd = NULL; G.nexts = 0; G.copy_family = -1; G.has_host_import = 0; G.host_align = 0;
		G.dec_family = -1; G.dec_queues = 0; G.inline_params = 0;
		os_log(lg(), "intelxeav1: last session gone in %{public}s - Vulkan device destroyed", proc());
	}
	pthread_mutex_unlock(&G.lock);
}

// ---------------------------------------------------------------------------------------------------------------------
// Session

typedef struct { int64_t pts; VTVideoDecoderFrame frame; } Pending;
typedef struct { IOSurfaceID id; IOSurfaceRef surf; VkBuffer buf; VkDeviceMemory mem; uint64_t last_use; } Imported;
typedef struct {
	pthread_mutex_t lock;
	VTVideoDecoderSession session;
	CMVideoFormatDescriptionRef fd;
	int32_t w, h; int ten;
	int full, range_known;            // full-range samples ('420f' / 'xf20' buffers); known from the format description
	AVCodecContext *av; AVBufferRef *dev; AVFrame *hw, *sw; AVPacket *pkt;
	AVFrame *last_alloc; uint64_t allocs;
	AVFrame *first_alloc; uint64_t allocs_mark;   // the first allocation since allocs_mark (HEVC: the current picture,
	                                              // allocated before any missing reference it generates)
	FourCharCode codec;                     // 'av01', 'avc1', 'hvc1' / 'hev1'   // the frame libavcodec allocated last for decoding into (get_buffer2), and a count
	Pending pend[64]; unsigned npend;
	int64_t next_pts;
	dispatch_queue_t emitq;          // waits for each copy and emits in decode order
	dispatch_semaphore_t slots;      // pictures handed to emitq and not yet emitted (back-pressure, see emit_held)
	// the copy engine
	VkDevice d; VkQueue q; int qfam;
	VkCommandPool pool; VkCommandBuffer cmd[RING]; uint64_t cmd_value[RING]; unsigned next_cmd;
	VkSemaphore done; uint64_t submitted;   // timeline: one value per copy
	Imported imp[SURFACES];
	int zero_copy;
	struct {
		PFN_vkBeginCommandBuffer Begin; PFN_vkEndCommandBuffer End; PFN_vkCmdPipelineBarrier2 Barrier;
		PFN_vkCmdCopyImageToBuffer Copy; PFN_vkQueueSubmit2 Submit; PFN_vkWaitSemaphores Wait;
		PFN_vkCreateBuffer CreateBuffer; PFN_vkDestroyBuffer DestroyBuffer; PFN_vkGetBufferMemoryRequirements Reqs;
		PFN_vkAllocateMemory Alloc; PFN_vkFreeMemory Free; PFN_vkBindBufferMemory Bind;
		PFN_vkGetMemoryHostPointerPropertiesEXT HostProps; PFN_vkResetCommandBuffer Reset;
		PFN_vkCmdWriteTimestamp2 Stamp; PFN_vkResetQueryPool ResetQ; PFN_vkGetQueryPoolResults Results;
	} vk;
	_Atomic uint64_t frames, errors, copies_cpu; uint64_t hidden;
	uint64_t t_dec_ns, t_sub_ns; _Atomic uint64_t t_lat_ns;
	// the deferred copy (see the top)
	int advance;                     // the copy waits on AND advances the picture's semaphores
	int defer;                       // this session defers copies
	struct { VTVideoDecoderFrame frame; AVFrame *pic; CVPixelBufferRef pb; uint64_t t0, t_defer, send_ns, seq; } dfr;
	int dfr_on;
	uint64_t defer_ns;               // the liveness timer
	dispatch_queue_t tq; dispatch_source_t timer;
	uint64_t deferred, by_timer;
	// INTELXEDEC_TRACE
	FILE *trace; VkQueryPool qp; uint64_t t_start, send_ns, seq; _Atomic uint64_t gpu0;
	// the direct Vulkan Video path (vtdirect.c, INTELXEDEC_DIRECT): when it decodes, s->av is NULL until a fallback
	Vtd *vtd;
	enum AVCodecID cid; uint8_t *cfg; size_t cfgn;      // what libavcodec is opened with (now, or at a fallback)
	char fallback[160];                                 // why the direct path handed the stream to libavcodec
	uint64_t t_cpu_ns, decodes, t_first_ns; _Atomic uint64_t t_last_emit_ns;
	VtdStats vst;                                       // the direct path's counters, kept at its close
	int was_direct;
	// AV1 / VP9 on the direct path: the samples since the last key frame, replayed into libavcodec (outputs dropped)
	// when the stream is handed over mid-GOP (a frame of another size), so it does not start without its references
	uint8_t *replay; size_t replay_n, replay_cap; uint32_t *replay_len; int replay_cnt, replay_lcap, replay_over;
	int wait_key;                                       // the replay did not fit: libavcodec starts at the next key frame
} Dec;
typedef struct { uint64_t seq, t_out, t_sub, send_ns; char why; } Tr;   // one picture's trace line (why: n next picture,
                                                                         // t timer, e other emit, s sync, f finish, d teardown, - not deferred)

static Dec *dec(VTVideoDecoderRef d) { return (Dec *)CMBaseObjectGetDerivedStorage(VTVideoDecoderGetCMBaseObject(d)); }

static void copy_engine_fini(Dec *s) {
	if (!s->d) return;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)G.gipa(G.inst, "vkGetDeviceProcAddr");
	if (s->submitted) { VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s->done, &s->submitted }; s->vk.Wait(s->d, &wi, UINT64_MAX); }
	for (int i = 0; i < SURFACES; i++) if (s->imp[i].surf) {
		s->vk.DestroyBuffer(s->d, s->imp[i].buf, NULL); s->vk.Free(s->d, s->imp[i].mem, NULL); CFRelease(s->imp[i].surf);
	}
	memset(s->imp, 0, sizeof s->imp);
	if (s->qp) ((PFN_vkDestroyQueryPool)gdpa(s->d, "vkDestroyQueryPool"))(s->d, s->qp, NULL);
	s->qp = NULL;
	if (s->pool) ((PFN_vkDestroyCommandPool)gdpa(s->d, "vkDestroyCommandPool"))(s->d, s->pool, NULL);
	if (s->done) ((PFN_vkDestroySemaphore)gdpa(s->d, "vkDestroySemaphore"))(s->d, s->done, NULL);
	s->pool = NULL; s->done = NULL; s->d = NULL; s->submitted = 0; s->next_cmd = 0; memset(s->cmd_value, 0, sizeof s->cmd_value);
}

static int copy_engine_init(Dec *s) {
	if (!G.has_host_import || G.copy_family < 0) return -1;
	s->d = G.d; s->qfam = G.copy_family;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)G.gipa(G.inst, "vkGetDeviceProcAddr");
#define L(f, n) s->vk.f = (void *)gdpa(s->d, n)
	L(Begin, "vkBeginCommandBuffer"); L(End, "vkEndCommandBuffer"); L(Barrier, "vkCmdPipelineBarrier2"); L(Copy, "vkCmdCopyImageToBuffer");
	L(Submit, "vkQueueSubmit2"); L(Wait, "vkWaitSemaphores"); L(CreateBuffer, "vkCreateBuffer"); L(DestroyBuffer, "vkDestroyBuffer");
	L(Reqs, "vkGetBufferMemoryRequirements"); L(Alloc, "vkAllocateMemory"); L(Free, "vkFreeMemory"); L(Bind, "vkBindBufferMemory");
	L(HostProps, "vkGetMemoryHostPointerPropertiesEXT"); L(Reset, "vkResetCommandBuffer");
	L(Stamp, "vkCmdWriteTimestamp2"); L(ResetQ, "vkResetQueryPool"); L(Results, "vkGetQueryPoolResults");
#undef L
	((PFN_vkGetDeviceQueue)gdpa(s->d, "vkGetDeviceQueue"))(s->d, (uint32_t)s->qfam, 0, &s->q);
	VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, (uint32_t)s->qfam };
	if (((PFN_vkCreateCommandPool)gdpa(s->d, "vkCreateCommandPool"))(s->d, &pci, NULL, &s->pool)) { s->d = NULL; return -1; }
	VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, s->pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, RING };
	VkSemaphoreTypeCreateInfo st = { VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, NULL, VK_SEMAPHORE_TYPE_TIMELINE, 0 };
	VkSemaphoreCreateInfo sci = { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &st };
	if (((PFN_vkAllocateCommandBuffers)gdpa(s->d, "vkAllocateCommandBuffers"))(s->d, &cai, s->cmd) ||
	    ((PFN_vkCreateSemaphore)gdpa(s->d, "vkCreateSemaphore"))(s->d, &sci, NULL, &s->done)) { copy_engine_fini(s); return -1; }
	// INTELXEDEC_TRACE: GPU start and end of every copy (host query reset: vkCmdResetQueryPool is not allowed on a
	// transfer-only queue)
	if (s->trace && G.f12.hostQueryReset && G.ts_period > 0 && s->vk.Stamp && s->vk.ResetQ && s->vk.Results) {
		VkQueryPoolCreateInfo qci = { VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, NULL, 0, VK_QUERY_TYPE_TIMESTAMP, 2 * TRACE_Q };
		if (((PFN_vkCreateQueryPool)gdpa(s->d, "vkCreateQueryPool"))(s->d, &qci, NULL, &s->qp)) s->qp = NULL;
	}
	return 0;
}

// the IOSurface's pages as a transfer-destination buffer, imported on first use and kept while the surface is in the pool
static Imported *import_surface(Dec *s, IOSurfaceRef surf) {
	const IOSurfaceID id = IOSurfaceGetID(surf);
	Imported *e = NULL, *lru = &s->imp[0];
	for (int i = 0; i < SURFACES; i++) {
		if (s->imp[i].surf == surf && s->imp[i].id == id) { s->imp[i].last_use = s->submitted + 1; return &s->imp[i]; }
		if (!s->imp[i].surf && !e) e = &s->imp[i];
		if (s->imp[i].last_use < lru->last_use) lru = &s->imp[i];
	}
	if (!e) {   // a pool that has cycled through more surfaces than we keep: drop the least recently used, once idle
		VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s->done, &lru->last_use };
		s->vk.Wait(s->d, &wi, UINT64_MAX);
		s->vk.DestroyBuffer(s->d, lru->buf, NULL); s->vk.Free(s->d, lru->mem, NULL); CFRelease(lru->surf);
		memset(lru, 0, sizeof *lru); e = lru;
	}
	void *base = IOSurfaceGetBaseAddress(surf);
	VkDeviceSize size = IOSurfaceGetAllocSize(surf);
	if (!base || ((uintptr_t)base % G.host_align)) return NULL;
	size = (size + G.host_align - 1) / G.host_align * G.host_align;
	VkMemoryHostPointerPropertiesEXT hpp = { VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	if (s->vk.HostProps(s->d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hpp) || !hpp.memoryTypeBits) return NULL;
	VkExternalMemoryBufferCreateInfo ebi = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &ebi, 0, size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer buf; VkDeviceMemory mem;
	if (s->vk.CreateBuffer(s->d, &bci, NULL, &buf)) return NULL;
	VkMemoryRequirements mr; s->vk.Reqs(s->d, buf, &mr);
	const uint32_t bits = mr.memoryTypeBits & hpp.memoryTypeBits;
	if (!bits) { s->vk.DestroyBuffer(s->d, buf, NULL); return NULL; }
	VkImportMemoryHostPointerInfoEXT imp = { VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, NULL, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base };
	VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imp, size, (uint32_t)__builtin_ctz(bits) };
	if (s->vk.Alloc(s->d, &mai, NULL, &mem)) { s->vk.DestroyBuffer(s->d, buf, NULL); return NULL; }
	if (s->vk.Bind(s->d, buf, mem, 0)) { s->vk.DestroyBuffer(s->d, buf, NULL); s->vk.Free(s->d, mem, NULL); return NULL; }
	*e = (Imported){ id, (IOSurfaceRef)CFRetain(surf), buf, mem, s->submitted + 1 };
	return e;
}

// records and submits the copy of a decoded picture into pb's IOSurface; returns the timeline value that completes it
static uint64_t gpu_copy(Dec *s, AVFrame *pic, CVPixelBufferRef pb) {
	IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
	if (!surf || CVPixelBufferGetPlaneCount(pb) != 2) return 0;
	Imported *im = import_surface(s, surf);
	if (!im) return 0;
	AVHWFramesContext *fc = (AVHWFramesContext *)pic->hw_frames_ctx->data;
	AVVulkanFramesContext *vfc = fc->hwctx;
	AVVkFrame *vkf = (AVVkFrame *)pic->data[0];
	int nimg = 0; while (nimg < AV_NUM_DATA_POINTERS && vkf->img[nimg]) nimg++;
	if (!nimg) return 0;

	const unsigned slot = s->next_cmd++ % RING;
	if (s->cmd_value[slot]) {   // the ring is full: wait for its oldest copy (back-pressure on the decoder)
		VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s->done, &s->cmd_value[slot] };
		s->vk.Wait(s->d, &wi, UINT64_MAX);
	}
	VkCommandBuffer cb = s->cmd[slot];
	s->vk.Reset(cb, 0);
	VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	s->vk.Begin(cb, &bi);
	const uint32_t qi = (uint32_t)((s->submitted + 1) % TRACE_Q) * 2;   // the pair of this copy's timeline value
	if (s->qp) {   // read by the emit queue once the copy is done; reused TRACE_Q copies later (at most 32 in flight)
		s->vk.ResetQ(s->d, s->qp, qi, 2);
		s->vk.Stamp(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->qp, qi);
	}

	vfc->lock_frame(fc, vkf);
	VkImageMemoryBarrier2 ib[AV_NUM_DATA_POINTERS];
	VkSemaphoreSubmitInfo wait[AV_NUM_DATA_POINTERS + 1], sig[AV_NUM_DATA_POINTERS + 1];
	for (int i = 0; i < nimg; i++) {
		ib[i] = (VkImageMemoryBarrier2){ VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, NULL,
			VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, vkf->access[i], VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_READ_BIT,
			vkf->layout[i], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, vkf->img[i],
			{ VK_IMAGE_ASPECT_COLOR_BIT, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS } };
		wait[i] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vkf->sem[i], vkf->sem_value[i], VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		sig[i] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, vkf->sem[i], vkf->sem_value[i] + 1, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
	}
	VkDependencyInfo dep = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO, NULL, 0, 0, NULL, 0, NULL, (uint32_t)nimg, ib };
	s->vk.Barrier(cb, &dep);
	const int multiplane = nimg == 1;
	for (int p = 0; p < 2; p++) {
		const size_t texel = (size_t)(p ? 2 : 1) * (s->ten ? 2 : 1);
		const size_t off = (uint8_t *)IOSurfaceGetBaseAddressOfPlane(surf, p) - (uint8_t *)IOSurfaceGetBaseAddress(surf);
		const size_t bpr = IOSurfaceGetBytesPerRowOfPlane(surf, p);
		const uint32_t pw = (uint32_t)((p ? (s->w + 1) / 2 : s->w)), ph = (uint32_t)((p ? (s->h + 1) / 2 : s->h));
		VkBufferImageCopy rg = { off, (uint32_t)(bpr / texel), ph,
			{ multiplane ? (p ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT) : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			{ 0, 0, 0 }, { pw, ph, 1 } };
		s->vk.Copy(cb, vkf->img[multiplane ? 0 : p], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, im->buf, 1, &rg);
	}
	if (s->qp) s->vk.Stamp(cb, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, s->qp, qi + 1);
	s->vk.End(cb);
	const uint64_t v = ++s->submitted;
	sig[nimg] = (VkSemaphoreSubmitInfo){ VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, NULL, s->done, v, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
	VkCommandBufferSubmitInfo cbi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, NULL, cb, 0 };
	// the picture's semaphores too (s->advance, see the top), or only the copy's own timeline (AV1 by default)
	const int in_place = s->advance;
	VkSubmitInfo2 si = { VK_STRUCTURE_TYPE_SUBMIT_INFO_2, NULL, 0, (uint32_t)nimg, wait, 1, &cbi,
	                     in_place ? (uint32_t)nimg + 1 : 1u, in_place ? sig : &sig[nimg] };
	AVHWDeviceContext *dc = (AVHWDeviceContext *)s->dev->data;
	AVVulkanDeviceContext *hw = dc->hwctx;
#if FF_API_VULKAN_SYNC_QUEUES
	if (hw->lock_queue) hw->lock_queue(dc, (uint32_t)s->qfam, 0);
#endif
	VkResult r = s->vk.Submit(s->q, 1, &si, VK_NULL_HANDLE);
#if FF_API_VULKAN_SYNC_QUEUES
	if (hw->unlock_queue) hw->unlock_queue(dc, (uint32_t)s->qfam, 0);
#endif
	if (r == VK_SUCCESS)
		for (int i = 0; i < nimg; i++) {
			if (in_place) vkf->sem_value[i]++;
			vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; vkf->access[i] = VK_ACCESS_2_TRANSFER_READ_BIT;
		}
	vfc->unlock_frame(fc, vkf);
	if (r != VK_SUCCESS) { os_log_error(lg(), "intelxeav1: copy submit -> %d", r); s->submitted--; s->next_cmd--; return 0; }
	s->cmd_value[slot] = v; im->last_use = v;
	return v;
}

// the fallback: libavutil downloads the picture, the CPU copies it into the pixel buffer
static OSStatus cpu_copy(Dec *s, AVFrame *pic, CVPixelBufferRef pb) {
	if (av_hwframe_transfer_data(s->sw, pic, 0) < 0) return kVTVideoDecoderBadDataErr;
	CVPixelBufferLockBaseAddress(pb, 0);
	for (int p = 0; p < 2; p++) {
		uint8_t *dst = CVPixelBufferGetBaseAddressOfPlane(pb, p);
		const size_t dpitch = CVPixelBufferGetBytesPerRowOfPlane(pb, p), rows = CVPixelBufferGetHeightOfPlane(pb, p);
		const size_t bytes = CVPixelBufferGetWidthOfPlane(pb, p) * (p ? 2 : 1) * (s->ten ? 2 : 1);
		for (size_t y = 0; y < rows && y < (size_t)(p ? (s->sw->height + 1) / 2 : s->sw->height); y++)
			memcpy(dst + y * dpitch, s->sw->data[p] + y * s->sw->linesize[p], bytes < (size_t)s->sw->linesize[p] ? bytes : (size_t)s->sw->linesize[p]);
	}
	CVPixelBufferUnlockBaseAddress(pb, 0);
	av_frame_unref(s->sw);
	s->copies_cpu++;
	return 0;
}

// Back-pressure. Each picture handed to the emit queue holds a pixel buffer and, until its copy completes, the decoded
// picture; the emit queue runs at the client's pace (VTDecoderSessionEmitDecodedFrame calls the client's output
// callback). A client slower than the decoder made the queue grow without bound, and with it libavcodec's Vulkan
// frame pool (av_hwframe pools grow on demand) until allocation failed: an 8K 10-bit HEVC stream (about 100 MB per
// picture) lost 27 of 120 pictures to "vulkan_pool_alloc failed with error -12" behind an md5-hashing client.
// VideoToolbox lets a decoder block in DecodeFrame (asynchronous decompression only allows it to return before the
// frame is emitted), so DecodeFrame now waits while the pictures in flight would exceed about 768 MB (4 to 32 of them;
// 7 at 8K 10-bit, 15 at 8K 8-bit, 30 at 4K 10-bit, 32 below; a client that keeps up never waits). Not from the emit queue itself (a client
// that decodes from its output callback would wait for itself), and never longer than 10 s.
static const char kEmitQueueKey = 0;
static long inflight_limit(int32_t w, int32_t h, int ten) {
	const double bytes = (double)w * h * 1.5 * (ten ? 2 : 1);
	const long n = (long)(768.0 * 1024 * 1024 / (bytes > 1 ? bytes : 1));
	return n < 4 ? 4 : n > 32 ? 32 : n;
}

// in decode order on the session's emit queue: an error or dropped frame, or a picture once its copy has completed
// pic: a reference to the decoded picture, released here once its copy has completed (NULL: nothing held)
static void flush_deferred(Dec *s, char why);
static void emit_held(Dec *s, VTVideoDecoderFrame frame, OSStatus st, VTDecodeInfoFlags fl, CVPixelBufferRef pb, uint64_t value, uint64_t t0, AVFrame *pic, Tr tr) {
	if (s->dfr_on) flush_deferred(s, 'e');   // a deferred picture is older than this one: decode order
	VTVideoDecoderSession session = s->session;
	VkDevice d = s->d; VkSemaphore done = s->done; PFN_vkWaitSemaphores wait = s->vk.Wait;
	dispatch_semaphore_t slot = NULL;
	if ((pb || pic) && s->slots && !dispatch_get_specific(&kEmitQueueKey) &&
	    !dispatch_semaphore_wait(s->slots, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC)))
		slot = s->slots;
	dispatch_async(s->emitq, ^{
		OSStatus r = st;
		if (value) {
			VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &done, &value };
			if (wait(d, &wi, 5000000000ull) != VK_SUCCESS) r = kVTVideoDecoderMalfunctionErr;
			IOSurfaceUnlock(CVPixelBufferGetIOSurface(pb), 0, NULL);
			s->t_lat_ns += now_ns() - t0;
			if (s->trace) {   // CPU times from StartSession, GPU times from the session's first copy (ns)
				const uint64_t t_done = now_ns() - s->t_start;
				uint64_t ts[2] = { 0, 0 }; double g0 = -1, g1 = -1;
				if (s->qp && !r && s->vk.Results(d, s->qp, (uint32_t)(value % TRACE_Q) * 2, 2, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS) {
					uint64_t z = s->gpu0; if (!z) { z = ts[0]; s->gpu0 = z; }
					g0 = (double)(int64_t)(ts[0] - z) * G.ts_period; g1 = (double)(int64_t)(ts[1] - z) * G.ts_period;
				}
				fprintf(s->trace, "%llu %c send_us %.1f out_us %.1f copy_submit_us %.1f done_us %.1f gpu_copy_us %.1f %.1f\n",
				        (unsigned long long)tr.seq, tr.why, tr.send_ns / 1e3, tr.t_out / 1e3, tr.t_sub / 1e3, t_done / 1e3, g0 / 1e3, g1 / 1e3);
			}
		}
		if (pic) { AVFrame *f = pic; av_frame_free(&f); }
		if (pb && !r) s->frames++; else if (r) s->errors++;
		s->t_last_emit_ns = now_ns();
		VTDecoderSessionEmitDecodedFrame(session, frame, r, fl | kVTDecodeInfo_Asynchronous, r ? NULL : pb);
		if (pb) CVPixelBufferRelease(pb);
		if (slot) dispatch_semaphore_signal(slot);
	});
}
static void emit_later(Dec *s, VTVideoDecoderFrame frame, OSStatus st, VTDecodeInfoFlags fl, CVPixelBufferRef pb, uint64_t value, uint64_t t0) {
	emit_held(s, frame, st, fl, pb, value, t0, NULL, (Tr){ 0 });
}

// copies pic into pb (the copy engine, or the CPU) and hands both to the emit queue; owned: pic is our reference
static void copy_and_emit(Dec *s, VTVideoDecoderFrame frame, AVFrame *pic, int owned, CVPixelBufferRef pb, uint64_t t0, Tr tr) {
	const uint64_t t1 = now_ns();
	OSStatus r = 0;
	uint64_t v = 0;
	if (s->zero_copy) {
		IOSurfaceRef surf = CVPixelBufferGetIOSurface(pb);
		if (surf && IOSurfaceLock(surf, 0, NULL) == kIOReturnSuccess) {
			v = gpu_copy(s, pic, pb);
			if (!v) IOSurfaceUnlock(surf, 0, NULL);
		}
	}
	if (!v) r = cpu_copy(s, pic, pb);
	const uint64_t t2 = now_ns();
	s->t_sub_ns += t2 - t1;
	tr.t_sub = t2 - s->t_start;
	// a GPU copy reads the picture until v: keep it out of the frame pool until then
	AVFrame *hold = v ? (owned ? pic : av_frame_clone(pic)) : NULL;
	if (owned && !v) av_frame_free(&pic);
	if (v && !hold) {   // no memory for the reference: wait for the copy here instead
		VkSemaphoreWaitInfo wi = { VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, NULL, 0, 1, &s->done, &v };
		s->vk.Wait(s->d, &wi, UINT64_MAX);
	}
	emit_held(s, frame, r, 0, r ? NULL : pb, v, t0, hold, tr);
	if (r && pb) CVPixelBufferRelease(pb);
}

// submits the deferred copy (its picture's successor has been submitted, or it must go now) and emits it
static void flush_deferred(Dec *s, char why) {
	if (!s->dfr_on) return;
	s->dfr_on = 0;
	if (why == 't') s->by_timer++;
	Tr tr = { s->dfr.seq, s->dfr.t_defer - s->t_start, 0, s->dfr.send_ns, why };
	copy_and_emit(s, s->dfr.frame, s->dfr.pic, 1, s->dfr.pb, s->dfr.t0, tr);
	memset(&s->dfr, 0, sizeof s->dfr);
}

// the liveness timer: no next picture came within defer_ns. trylock: the thread holding the session flushes itself
// (every path that holds it emits, finishes or tears down), and teardown can wait for this handler while holding it.
static void defer_fire(void *ctx) {
	Dec *s = ctx;
	if (pthread_mutex_trylock(&s->lock)) {
		if (s->timer) dispatch_source_set_timer(s->timer, dispatch_time(DISPATCH_TIME_NOW, 1000000), DISPATCH_TIME_FOREVER, 250000);
		return;
	}
	if (s->dfr_on) {
		const uint64_t age = now_ns() - s->dfr.t_defer;
		if (age >= s->defer_ns - s->defer_ns / 8) flush_deferred(s, 't');
		// early (a retry above replaced the deadline of a newer deferral): wait for the rest of it
		else dispatch_source_set_timer(s->timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)(s->defer_ns - age)), DISPATCH_TIME_FOREVER, s->defer_ns / 16);
	}
	pthread_mutex_unlock(&s->lock);
}

static OSStatus set_output_size(Dec *s, int32_t width, int32_t height);
static void output(Dec *s, VTVideoDecoderFrame frame, AVFrame *pic, int no_output) {
	if (no_output) { emit_later(s, frame, 0, kVTDecodeInfo_FrameDropped, NULL, 0, 0); return; }
	const uint64_t t0 = now_ns();
	// VP9 changes the frame size inside a stream (scaled references): the pixel buffers follow the picture, as
	// Apple's decoders do, and the copy (s->w x s->h) never reads outside it. (Not for the others: H.264 / HEVC
	// pictures from get_buffer2 carry the coded size, before cropping.)
	if (s->codec == 'vp09' && (pic->width != s->w || pic->height != s->h) && pic->width > 0 && pic->height > 0) {
		flush_deferred(s, 'e');   // its copy uses the old size
		if (set_output_size(s, pic->width, pic->height)) {
			emit_later(s, frame, kVTVideoDecoderMalfunctionErr, 0, NULL, 0, 0);
			return;
		}
	}
	if (!s->range_known) {   // neither the format description nor the codec configuration said: the stream's own
		s->range_known = 1;
		if ((pic->color_range == AVCOL_RANGE_JPEG) != s->full) {
			flush_deferred(s, 'e');
			s->full = pic->color_range == AVCOL_RANGE_JPEG;
			if (set_output_size(s, s->w, s->h)) { emit_later(s, frame, kVTVideoDecoderMalfunctionErr, 0, NULL, 0, 0); return; }
		}
	}
	CVPixelBufferPoolRef pool = VTDecoderSessionGetPixelBufferPool(s->session);
	CVPixelBufferRef pb = NULL;
	OSStatus r = pool ? CVPixelBufferPoolCreatePixelBuffer(NULL, pool, &pb) : kVTAllocationFailedErr;
	if (r) {
		emit_later(s, frame, r, 0, NULL, 0, t0);
		if (pb) CVPixelBufferRelease(pb);
		return;
	}
	const uint64_t seq = ++s->seq;
	AVFrame *c = s->defer && s->zero_copy ? av_frame_clone(pic) : NULL;
	// this picture's decode has been submitted: the previous picture's copy goes now, after it
	flush_deferred(s, 'n');
	if (c) {
		const uint64_t t = now_ns();
		s->dfr.frame = frame; s->dfr.pic = c; s->dfr.pb = pb; s->dfr.t0 = t0; s->dfr.t_defer = t; s->dfr.send_ns = s->send_ns; s->dfr.seq = seq;
		s->dfr_on = 1; s->deferred++;
		if (s->timer) dispatch_source_set_timer(s->timer, dispatch_time(DISPATCH_TIME_NOW, (int64_t)s->defer_ns), DISPATCH_TIME_FOREVER, s->defer_ns / 16);
		return;
	}
	copy_and_emit(s, frame, pic, 0, pb, t0, (Tr){ seq, t0 - s->t_start, 0, s->send_ns, '-' });
}
// The statistics line of the last session closed in this process (IntelXeDec_LastStats; vtdec --lib prints it):
// codec, size, which path decoded (direct: vtdirect.c; libavcodec; direct>libavcodec: handed over mid-stream), pictures,
// errors, fps from the first DecodeFrame to the last emitted picture, and per DecodeFrame call: wall time, the calling
// thread's CPU time, copy submission; per picture: until emitted; the direct path's own parse / record / submit times.
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_stats[640];
static const char *codec_name(FourCharCode c);
static void save_stats(Dec *s) {
	char line[640];
	const double span = s->t_last_emit_ns > s->t_first_ns && s->t_first_ns ? (s->t_last_emit_ns - s->t_first_ns) / 1e9 : 0;
	const double nd = s->decodes ? (double)s->decodes : 1, np = s->vst.pictures ? (double)s->vst.pictures : 1;
	snprintf(line, sizeof line, "%s %dx%d path=%s frames=%llu errors=%llu fps=%.1f decode_ms=%.3f cpu_ms=%.3f copy_submit_ms=%.3f latency_ms=%.3f "
	         "direct_pictures=%llu parse_ms=%.3f record_ms=%.3f submit_ms=%.3f no_ref=%llu gaps=%llu skipped=%llu fallback=%s",
	         codec_name(s->codec), s->w, s->h, s->was_direct ? (s->av ? "direct>libavcodec" : "direct") : "libavcodec",
	         (unsigned long long)s->frames, (unsigned long long)s->errors, span > 0 ? s->frames / span : 0.0,
	         s->t_dec_ns / 1e6 / nd, s->t_cpu_ns / 1e6 / nd, s->t_sub_ns / 1e6 / nd, s->frames ? s->t_lat_ns / 1e6 / s->frames : 0.0,
	         (unsigned long long)s->vst.pictures, s->vst.parse_ns / 1e6 / np, s->vst.record_ns / 1e6 / np, s->vst.submit_ns / 1e6 / np,
	         (unsigned long long)s->vst.no_ref_skips, (unsigned long long)s->vst.gaps, (unsigned long long)s->vst.skipped,
	         s->fallback[0] ? s->fallback : "-");
	os_log(lg(), "intelxeav1: %{public}s", line);
	pthread_mutex_lock(&g_stats_lock); strlcpy(g_stats, line, sizeof g_stats); pthread_mutex_unlock(&g_stats_lock);
}
__attribute__((visibility("default"))) int IntelXeDec_LastStats(char *out, size_t n) {
	if (!out || !n) return 0;
	pthread_mutex_lock(&g_stats_lock); strlcpy(out, g_stats, n); pthread_mutex_unlock(&g_stats_lock);
	return (int)strlen(out);
}

static void teardown(Dec *s) {
	if (s->timer) {   // the handler never blocks on s->lock (trylock): waiting for it here is safe with the lock held
		dispatch_source_cancel(s->timer);
		dispatch_sync(s->tq, ^{});
		dispatch_release(s->timer); s->timer = NULL;
	}
	flush_deferred(s, 'd');
	if (s->emitq) dispatch_sync(s->emitq, ^{});
	if (s->vtd) { s->vst = *vtd_stats(s->vtd); vtd_close(s->vtd); s->vtd = NULL; }   // waits for its decodes
	if (s->frames || s->errors) {
		os_log(lg(), "intelxeav1: close in %{public}s: %llu pictures (%llu by CPU copy, %llu hidden frames, %llu copies deferred, %llu by the timer), %llu errors | per picture ms: decode %.2f, copy submit %.2f, until emitted %.2f",
		       proc(), s->frames, s->copies_cpu, s->hidden, s->deferred, s->by_timer, s->errors, s->frames ? s->t_dec_ns / 1e6 / s->frames : 0.0,
		       s->frames ? s->t_sub_ns / 1e6 / s->frames : 0.0, s->frames ? s->t_lat_ns / 1e6 / s->frames : 0.0);
		save_stats(s);
	}
	if (s->trace) { fclose(s->trace); s->trace = NULL; }
	avcodec_free_context(&s->av);
	av_frame_free(&s->last_alloc); av_frame_free(&s->first_alloc);   // before the device can go (release_device)
	s->allocs = 0;
	copy_engine_fini(s);
	if (s->dev) { av_buffer_unref(&s->dev); release_device(); }
	av_frame_free(&s->hw); av_frame_free(&s->sw); av_packet_free(&s->pkt);
	if (s->fd) { CFRelease(s->fd); s->fd = NULL; }
	s->frames = s->errors = s->copies_cpu = 0; s->hidden = 0; s->t_dec_ns = s->t_sub_ns = 0; s->t_lat_ns = 0;
	s->deferred = s->by_timer = 0; s->seq = 0; s->send_ns = 0; s->gpu0 = 0; s->defer = 0;
	av_freep(&s->cfg); s->cfgn = 0; s->fallback[0] = 0; s->was_direct = 0;
	free(s->replay); free(s->replay_len); s->replay = NULL; s->replay_len = NULL;
	s->replay_n = s->replay_cap = 0; s->replay_cnt = s->replay_lcap = s->replay_over = 0; s->wait_key = 0;
	s->t_cpu_ns = s->decodes = s->t_first_ns = 0; s->t_last_emit_ns = 0; memset(&s->vst, 0, sizeof s->vst);
}
static OSStatus Invalidate(CMBaseObjectRef o) { Dec *s = CMBaseObjectGetDerivedStorage(o); pthread_mutex_lock(&s->lock); teardown(s); pthread_mutex_unlock(&s->lock); return 0; }
static void Finalize(CMBaseObjectRef o) {
	Dec *s = CMBaseObjectGetDerivedStorage(o);
	teardown(s);
	if (s->emitq) dispatch_release(s->emitq);
	if (s->slots) dispatch_release(s->slots);   // every slot is back: teardown drained the emit queue
	if (s->tq) dispatch_release(s->tq);
	pthread_mutex_destroy(&s->lock);
}
static const char *codec_name(FourCharCode c) { return c == 'av01' ? "AV1" : c == 'avc1' ? "H.264" : c == 'vp09' ? "VP9" : "HEVC"; }
// H.264 and HEVC answer every sample in decode order with the picture it decoded; AV1 and VP9 show one picture per sample
static int decode_order(const Dec *s) { return s->codec == 'avc1' || s->codec == 'hvc1'; }
static CFStringRef CopyDebugDescription(CMBaseObjectRef o) {
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<IntelXe %s decoder (Vulkan Video) %p>"), codec_name(((Dec *)CMBaseObjectGetDerivedStorage(o))->codec), o);
}
static OSStatus CopyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out) {
	if (CFEqual(key, kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder)) { *(CFBooleanRef *)out = kCFBooleanTrue; return 0; }
	return kVTPropertyNotSupportedErr;
}

static enum AVPixelFormat get_format(AVCodecContext *c, const enum AVPixelFormat *fmts) {
	for (const enum AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; p++) if (*p == AV_PIX_FMT_VULKAN) return *p;
	os_log_error(lg(), "intelxeav1: the %{public}s decoder offered no Vulkan format", codec_name(((Dec *)c->opaque)->codec));
	return AV_PIX_FMT_NONE;
}

// every frame the AV1 decoder decodes into, shown or not, comes from here (av1dec: ff_progress_frame_get_buffer; the
// Vulkan hwaccel has no alloc_frame of its own)
static int get_buffer2(AVCodecContext *c, AVFrame *f, int flags) {
	int r = avcodec_default_get_buffer2(c, f, flags);
	Dec *s = c->opaque;
	if (r >= 0 && s && s->last_alloc && f->format == AV_PIX_FMT_VULKAN) {
		if (s->first_alloc && s->allocs == s->allocs_mark) { av_frame_unref(s->first_alloc); av_frame_ref(s->first_alloc, f); }
		av_frame_unref(s->last_alloc);
		if (av_frame_ref(s->last_alloc, f) >= 0) s->allocs++;
	}
	return r;
}

static CFDataRef atom_of(CMFormatDescriptionRef fd, CFStringRef name) {
	CFDictionaryRef atoms = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms);
	CFTypeRef v = atoms && CFGetTypeID(atoms) == CFDictionaryGetTypeID() ? CFDictionaryGetValue(atoms, name) : NULL;
	return v && CFGetTypeID(v) == CFDataGetTypeID() ? (CFDataRef)v : NULL;
}

// The video decode capabilities of the device for this stream's profile: 0 if it can decode w x h, -1 if not, 1 if the
// query itself failed (then libavcodec decides at the first picture, as before). Larger streams must be refused here:
// accepted, they fail at every picture (ANV decodes up to 4096x4096; an 8K AV1 stream got kVTVideoDecoderBadDataErr
// for each sample and VTDecompressionSessionWaitForAsynchronousFrames never returned).
static int fits_device(FourCharCode codec, int profile_idc, int ten, int32_t w, int32_t h, VkExtent2D *max) {
	PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR caps = (PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR)G.gipa(G.inst, "vkGetPhysicalDeviceVideoCapabilitiesKHR");
	if (!caps) return 1;
	VkVideoDecodeH264ProfileInfoKHR p264 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR, NULL,
		profile_idc == 66 ? STD_VIDEO_H264_PROFILE_IDC_BASELINE : profile_idc == 77 ? STD_VIDEO_H264_PROFILE_IDC_MAIN : STD_VIDEO_H264_PROFILE_IDC_HIGH,
		VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR };
	VkVideoDecodeH265ProfileInfoKHR p265 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_PROFILE_INFO_KHR, NULL,
		ten ? STD_VIDEO_H265_PROFILE_IDC_MAIN_10 : STD_VIDEO_H265_PROFILE_IDC_MAIN };
	VkVideoDecodeAV1ProfileInfoKHR pav1 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_PROFILE_INFO_KHR, NULL, STD_VIDEO_AV1_PROFILE_MAIN, VK_FALSE };
	VkVideoDecodeVP9ProfileInfoKHR pvp9 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_PROFILE_INFO_KHR, NULL, ten ? STD_VIDEO_VP9_PROFILE_2 : STD_VIDEO_VP9_PROFILE_0 };
	const VkVideoComponentBitDepthFlagsKHR depth = ten ? VK_VIDEO_COMPONENT_BIT_DEPTH_10_BIT_KHR : VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	VkVideoProfileInfoKHR prof = { VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR,
		codec == 'av01' ? (void *)&pav1 : codec == 'avc1' ? (void *)&p264 : codec == 'vp09' ? (void *)&pvp9 : (void *)&p265,
		codec == 'av01' ? VK_VIDEO_CODEC_OPERATION_DECODE_AV1_BIT_KHR : codec == 'avc1' ? VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR :
		codec == 'vp09' ? VK_VIDEO_CODEC_OPERATION_DECODE_VP9_BIT_KHR : VK_VIDEO_CODEC_OPERATION_DECODE_H265_BIT_KHR,
		VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR, depth, depth };
	VkVideoDecodeH264CapabilitiesKHR c264 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR };
	VkVideoDecodeH265CapabilitiesKHR c265 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_H265_CAPABILITIES_KHR };
	VkVideoDecodeAV1CapabilitiesKHR cav1 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_AV1_CAPABILITIES_KHR };
	VkVideoDecodeVP9CapabilitiesKHR cvp9 = { VK_STRUCTURE_TYPE_VIDEO_DECODE_VP9_CAPABILITIES_KHR };
	VkVideoDecodeCapabilitiesKHR dc = { VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR,
		codec == 'av01' ? (void *)&cav1 : codec == 'avc1' ? (void *)&c264 : codec == 'vp09' ? (void *)&cvp9 : (void *)&c265 };
	VkVideoCapabilitiesKHR vc = { VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR, &dc };
	if (caps(G.pd, &prof, &vc) != VK_SUCCESS) return 1;
	*max = vc.maxCodedExtent;
	return (uint32_t)w <= vc.maxCodedExtent.width && (uint32_t)h <= vc.maxCodedExtent.height ? 0 : -1;
}

// the session's pixel buffers: w x h, 4:2:0 8- or 10-bit
static OSStatus set_output_size(Dec *s, int32_t width, int32_t height) {
	VTVideoDecoderSession session = s->session;
	const int high = s->ten;
	CMVideoDimensions dim = { width, height };
	const int32_t pf = high ? (s->full ? kCVPixelFormatType_420YpCbCr10BiPlanarFullRange : kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange)
	                        : (s->full ? kCVPixelFormatType_420YpCbCr8BiPlanarFullRange : kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange);
	CFNumberRef w = CFNumberCreate(NULL, kCFNumberSInt32Type, &dim.width), h = CFNumberCreate(NULL, kCFNumberSInt32Type, &dim.height),
	            f = CFNumberCreate(NULL, kCFNumberSInt32Type, &pf);
	CFDictionaryRef io = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	// 64-byte aligned planes and rows for the copy engine (see the top); Metal compatible like Apple's decoders' buffers
	const int32_t a64 = 64;
	CFNumberRef al = CFNumberCreate(NULL, kCFNumberSInt32Type, &a64);
	const void *k[] = { kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey,
	                    kCVPixelBufferPlaneAlignmentKey, kCVPixelBufferBytesPerRowAlignmentKey, kCVPixelBufferMetalCompatibilityKey };
	const void *v[] = { w, h, f, io, al, al, kCFBooleanTrue };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 7, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(al);
	OSStatus r = VTDecoderSessionSetPixelBufferAttributes(session, attrs);
	if (!r) { s->w = width; s->h = height; }
	CFRelease(attrs); CFRelease(io); CFRelease(w); CFRelease(h); CFRelease(f);
	return r;
}

// INTELXEDEC_DIRECT: the codecs the direct Vulkan Video path (vtdirect.c) decodes instead of libavcodec, read when a
// session starts: a list of "h264", "hevc" (or "h265"), "av1", "vp9", or "1" / "all": every codec it covers (H.264, HEVC, AV1, VP9); unset
// or "0": none (the default until the B580 A/B, tests/vtdirect-ab.sh, decides). Like every INTELXE knob, a release
// Metal plug-in keeps it from the environment only with NVMTL_PROCESS_ENV=1; /Library/GPUBundles/nvmtl/nvrm610.conf
// sets it for every process.
static int direct_wanted(FourCharCode codec) {
	const char *e = getenv("INTELXEDEC_DIRECT");
	if (!e || !*e || !strcmp(e, "0")) return 0;
	if (!strcmp(e, "1") || strstr(e, "all")) return codec == 'avc1' || codec == 'hvc1' || codec == 'av01' || codec == 'vp09';
	if (codec == 'avc1') return strstr(e, "h264") != NULL;
	if (codec == 'hvc1') return strstr(e, "hevc") || strstr(e, "h265");
	if (codec == 'av01') return strstr(e, "av1") != NULL;
	if (codec == 'vp09') return strstr(e, "vp9") != NULL;
	return 0;
}
// a sample libavcodec can start decoding at (after a hand-over without the samples before it)
static int key_sample(const Dec *s, const uint8_t *d, size_t n) { return s->codec == 'vp09' ? vp9d_is_key_sample(d, n) : av1d_is_key_sample(d, n); }

// libavcodec for the session's stream (s->cid, s->cfg): at StartSession, or when the direct path hands the stream over
static int open_avcodec(Dec *s) {
	const AVCodec *codec = avcodec_find_decoder(s->cid);
	if (!codec || !(s->av = avcodec_alloc_context3(codec))) return AVERROR(ENOMEM);
	const CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(s->fd);
	s->av->hw_device_ctx = av_buffer_ref(s->dev);
	s->av->get_format = get_format;
	s->av->get_buffer2 = get_buffer2;
	s->av->opaque = s;
	if (!s->last_alloc) s->last_alloc = av_frame_alloc();
	if (!s->first_alloc) s->first_alloc = av_frame_alloc();
	if (!(s->av->extradata = av_mallocz(s->cfgn + AV_INPUT_BUFFER_PADDING_SIZE))) return AVERROR(ENOMEM);
	if (s->cfgn) memcpy(s->av->extradata, s->cfg, s->cfgn);
	s->av->extradata_size = (int)s->cfgn;
	s->av->width = dim.width; s->av->height = dim.height;
	s->av->pkt_timebase = (AVRational){ 1, 1000000 };
	return avcodec_open2(s->av, codec, NULL);
}

static OSStatus StartSession(VTVideoDecoderRef d, VTVideoDecoderSession session, CMVideoFormatDescriptionRef fd) {
	Dec *s = dec(d);
	const FourCharCode sub = CMFormatDescriptionGetMediaSubType(fd);
	if (sub != s->codec && !(s->codec == 'hvc1' && sub == 'hev1')) return kVTVideoDecoderBadDataErr;
	CFDataRef cfg = atom_of(fd, s->codec == 'av01' ? CFSTR("av1C") : s->codec == 'avc1' ? CFSTR("avcC") : s->codec == 'vp09' ? CFSTR("vpcC") : CFSTR("hvcC"));
	CFIndex cn = cfg ? CFDataGetLength(cfg) : 0;
	const uint8_t *c = cfg ? CFDataGetBytePtr(cfg) : NULL;
	int high = 0, profile_idc = 0, vp9_full = -1;
	const uint8_t *vpcc = NULL; size_t vpccn = 0;   // VP9: the vpcC for the direct path (libavcodec takes no extradata)
	enum AVCodecID cid = AV_CODEC_ID_AV1;
	if (s->codec == 'av01') {
		if (cn < 4) { os_log_error(lg(), "intelxeav1: av01 without an av1C atom - refused"); return kVTVideoDecoderBadDataErr; }
		const int profile = c[1] >> 5, twelve = (c[2] >> 5) & 1, mono = (c[2] >> 4) & 1, sx = (c[2] >> 3) & 1, sy = (c[2] >> 2) & 1;
		high = (c[2] >> 6) & 1;
		if (profile != 0 || twelve || mono || !sx || !sy) { os_log(lg(), "intelxeav1: AV1 profile %d (12-bit %d, mono %d, 4:2:0 %d) is not Main 4:2:0 - refused", profile, twelve, mono, sx && sy); return kVTVideoDecoderUnsupportedDataFormatErr; }
	} else if (s->codec == 'vp09') {
		// vpcC (VP Codec ISO Media File Format Binding, a FullBox): version 1, 3 flag bytes, profile, level,
		// bitDepth (4 bits) chromaSubsampling (3) videoFullRangeFlag (1), ... The B580 (ANV) decodes Profile 0 (8-bit) and
		// Profile 2 (10-bit) 4:2:0; Profiles 1 and 3 (4:2:2, 4:4:0, 4:4:4) are refused.
		if (cn < 8 || c[0] != 1) { os_log_error(lg(), "intelxeav1: vp09 without a vpcC atom - refused"); return kVTVideoDecoderBadDataErr; }
		const int profile = c[4], depth = c[6] >> 4, chroma = (c[6] >> 1) & 7;
		vp9_full = c[6] & 1;
		if (!((profile == 0 && depth == 8) || (profile == 2 && depth == 10)) || chroma > 1) {
			os_log(lg(), "intelxeav1: VP9 profile %d, %d-bit, chroma subsampling %d is not Profile 0 / 2 4:2:0 - refused", profile, depth, chroma);
			return kVTVideoDecoderUnsupportedDataFormatErr;
		}
		high = depth == 10;
		cid = AV_CODEC_ID_VP9;
		vpcc = c; vpccn = (size_t)cn;
		cn = 0;   // libavcodec's VP9 decoder takes no extradata
	} else if (s->codec == 'avc1') {
		// avcC: version, profile_idc, compatibility, level_idc ... Baseline / Main / High are 8-bit 4:2:0 by definition
		if (cn < 7 || c[0] != 1) { os_log_error(lg(), "intelxeav1: avc1 without an avcC atom - refused"); return kVTVideoDecoderBadDataErr; }
		if (c[1] != 66 && c[1] != 77 && c[1] != 100) { os_log(lg(), "intelxeav1: H.264 profile_idc %d is not Baseline/Main/High - refused", c[1]); return kVTVideoDecoderUnsupportedDataFormatErr; }
		cid = AV_CODEC_ID_H264;
		profile_idc = c[1];
	} else {
		// hvcC: general_profile_idc in byte 1, chroma_format_idc in byte 16, bit_depth_luma_minus8 in byte 17
		if (cn < 23 || c[0] != 1) { os_log_error(lg(), "intelxeav1: hvc1 without an hvcC atom - refused"); return kVTVideoDecoderBadDataErr; }
		const int profile = c[1] & 0x1f, chroma = c[16] & 3, depth = (c[17] & 7) + 8, cdepth = (c[18] & 7) + 8;
		if ((profile != 1 && profile != 2 && profile != 3) || chroma != 1 || depth > 10 || cdepth != depth) {
			os_log(lg(), "intelxeav1: HEVC profile %d, chroma %d, %d/%d-bit is not Main/Main10 4:2:0 - refused", profile, chroma, depth, cdepth);
			return kVTVideoDecoderUnsupportedDataFormatErr;
		}
		high = depth == 10;
		cid = AV_CODEC_ID_HEVC;
	}
	AVBufferRef *dev = shared_device();
	if (!dev) return kVTVideoDecoderNotAvailableNowErr;
	CMVideoDimensions dim = CMVideoFormatDescriptionGetDimensions(fd);
	VkExtent2D max = { 0, 0 };
	if (fits_device(s->codec, profile_idc, high, dim.width, dim.height, &max) < 0) {
		os_log(lg(), "intelxeav1: %{public}s %dx%d is larger than the B580 decodes (%ux%u) - refused", codec_name(s->codec), dim.width, dim.height, max.width, max.height);
		av_buffer_unref(&dev); release_device();
		return kVTVideoDecoderUnsupportedDataFormatErr;
	}
	pthread_mutex_lock(&s->lock);
	teardown(s);
	if (!s->emitq) {
		s->emitq = dispatch_queue_create("io.github.yusufklncc.intelxe.av1.emit", DISPATCH_QUEUE_SERIAL);
		dispatch_queue_set_specific(s->emitq, &kEmitQueueKey, (void *)&kEmitQueueKey, NULL);
	}
	if (s->slots) dispatch_release(s->slots);   // teardown above drained the emit queue: every slot is back
	s->slots = dispatch_semaphore_create(inflight_limit(dim.width, dim.height, high));
	s->session = session; s->w = dim.width; s->h = dim.height; s->ten = high; s->fd = (CMVideoFormatDescriptionRef)CFRetain(fd);
	// the sample range, as Apple's decoders follow it: the format description's FullRangeVideo (CoreMedia sets it from the
	// SPS/VPS VUI or the track's colr atom), else VP9's vpcC flag, else the first decoded picture (see output). Buffers of
	// the other range made VideoToolbox convert every picture for a client that asks for the stream's own ('420f' for a
	// full-range 4K stream: 45-59 fps instead of 270-720), and labelled full-range samples as video range.
	CFTypeRef fr = CMFormatDescriptionGetExtension(fd, kCMFormatDescriptionExtension_FullRangeVideo);
	s->range_known = fr && CFGetTypeID(fr) == CFBooleanGetTypeID();
	s->full = s->range_known ? CFBooleanGetValue((CFBooleanRef)fr) : vp9_full == 1;
	if (vp9_full >= 0) s->range_known = 1;
	s->dev = dev;
	s->t_start = now_ns();
	// deferred copies (see the top): VP9 by default; INTELXEDEC_COPY_DEFER=0 none, =all every codec
	const char *dv = getenv("INTELXEDEC_COPY_DEFER");
	s->defer = dv && !strcmp(dv, "0") ? 0 : dv && !strcmp(dv, "all") ? 1 : s->codec == 'vp09';
	s->advance = s->codec != 'av01' || s->defer;
	const char *dm = getenv("INTELXEDEC_COPY_DEFER_MS");
	long ms = dm ? strtol(dm, NULL, 10) : 12;
	s->defer_ns = (uint64_t)(ms < 1 ? 1 : ms > 100 ? 100 : ms) * 1000000ull;
	const char *tp = getenv("INTELXEDEC_TRACE");
	if (tp && *tp) {
		static _Atomic int n;
		char path[1024]; snprintf(path, sizeof path, "%s.%d.%d.txt", tp, getpid(), ++n);
		if ((s->trace = fopen(path, "w")))
			fprintf(s->trace, "# intelxedec trace: %s %dx%d %d-bit, defer %d (%llu ms), advance %d\n", codec_name(s->codec), dim.width, dim.height,
			        high ? 10 : 8, s->defer, (unsigned long long)(s->defer_ns / 1000000), s->advance);
	}
	s->zero_copy = copy_engine_init(s) == 0 && !getenv("INTELXEAV1_CPU_COPY");
	if (s->defer && s->zero_copy) {
		if (!s->tq) s->tq = dispatch_queue_create("io.github.yusufklncc.intelxe.av1.defer", DISPATCH_QUEUE_SERIAL);
		s->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, s->tq);
		if (s->timer) {
			dispatch_set_context(s->timer, s);
			dispatch_source_set_event_handler_f(s->timer, defer_fire);
			dispatch_source_set_timer(s->timer, DISPATCH_TIME_FOREVER, DISPATCH_TIME_FOREVER, 0);
			dispatch_resume(s->timer);
		} else s->defer = 0;   // no timer, no deferral (a client waiting for each output would stall)
	} else s->defer = 0;
	int err = 0;
	s->cid = cid;
	s->cfg = av_mallocz(cn + AV_INPUT_BUFFER_PADDING_SIZE); s->cfgn = s->cfg ? (size_t)cn : 0;
	if (s->cfg) memcpy(s->cfg, c, cn);
	s->hw = av_frame_alloc(); s->sw = av_frame_alloc(); s->pkt = av_packet_alloc();
	// the direct path (INTELXEDEC_DIRECT, see direct_wanted): libavcodec is opened only if it cannot take the stream,
	// or later when it hands the stream over
	if (direct_wanted(s->codec)) {
		VtdDevice vd = { G.gipa, G.inst, G.pd, G.d, G.dec_family, G.dec_queues, G.inline_params, s->dev };
		char why[160] = "";
		s->vtd = !s->cfg ? NULL : s->codec == 'avc1' ? vtd_open_h264(&vd, s->cfg, s->cfgn, why, sizeof why) :
		         s->codec == 'av01' ? vtd_open_av1(&vd, s->cfg, s->cfgn, why, sizeof why) :
		         s->codec == 'vp09' ? vtd_open_vp9(&vd, vpcc, vpccn, why, sizeof why) : vtd_open_hevc(&vd, s->cfg, s->cfgn, why, sizeof why);
		s->was_direct = s->vtd != NULL;
		if (!s->vtd) {
			snprintf(s->fallback, sizeof s->fallback, "%s", why);
			os_log(lg(), "intelxeav1: direct path not used for this %{public}s stream (%{public}s): libavcodec decodes it", codec_name(s->codec), why);
		}
	}
	if (!s->vtd) err = open_avcodec(s);
	pthread_mutex_unlock(&s->lock);
	if (err < 0) { char e[128]; av_strerror(err, e, sizeof e); os_log_error(lg(), "intelxeav1: Vulkan %{public}s decoder in %{public}s: %{public}s", codec_name(s->codec), proc(), e); return kVTVideoDecoderNotAvailableNowErr; }
	OSStatus r = set_output_size(s, dim.width, dim.height);
	os_log(lg(), "intelxeav1: session in %{public}s: %dx%d %{public}s %d-bit %{public}s on the B580 (Vulkan Video%{public}s, %{public}s%{public}s) -> %d", proc(), dim.width, dim.height, codec_name(s->codec),
	       high ? 10 : 8, !s->range_known ? "range from the stream" : s->full ? "full range" : "video range", s->vtd ? " direct" : "",
	       s->zero_copy ? "copy engine into the IOSurface" : "CPU copy", s->defer ? ", copies deferred" : "", (int)r);
	return r;
}

// the token of the oldest pending temporal unit not later than pts (a shown frame answers its own unit)
static VTVideoDecoderFrame take(Dec *s, int64_t pts, int *no_output) {
	unsigned best = s->npend;
	for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].pts == pts) { best = i; break; }
	if (best == s->npend) best = 0;
	VTVideoDecoderFrame f = s->npend ? s->pend[best].frame : NULL;
	if (no_output) *no_output = s->npend ? (int)(s->pend[best].pts >> 62 & 1) : 0;
	if (s->npend) { memmove(&s->pend[best], &s->pend[best + 1], (s->npend - best - 1) * sizeof s->pend[0]); s->npend--; }
	return f;
}
#define NO_OUTPUT_BIT (1ll << 62)   // kVTDecodeFrame_DoNotOutputFrame, kept in the pending entry's pts
static void drain(Dec *s) {
	while (avcodec_receive_frame(s->av, s->hw) == 0) {
		int no_output = 0;
		unsigned i = 0; while (i < s->npend && (s->pend[i].pts & ~NO_OUTPUT_BIT) != s->hw->pts) i++;
		VTVideoDecoderFrame f = take(s, i < s->npend ? s->pend[i].pts : s->hw->pts, &no_output);
		if (f) output(s, f, s->hw, no_output);
		av_frame_unref(s->hw);
	}
}

static uint64_t thread_cpu_ns(void) { return clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); }

// the direct path (vtdirect.c) handed the stream over: libavcodec from this sample on (s->pkt holds it)
static int fall_back(Dec *s) {
	snprintf(s->fallback, sizeof s->fallback, "%s", vtd_why(s->vtd));
	os_log(lg(), "intelxeav1: direct path -> libavcodec in %{public}s after %llu pictures: %{public}s", proc(),
	       (unsigned long long)vtd_stats(s->vtd)->pictures, s->fallback);
	s->vst = *vtd_stats(s->vtd);
	vtd_close(s->vtd);   // waits for its decodes; pictures still being copied hold their frames (and frames context)
	s->vtd = NULL;
	const int err = open_avcodec(s);
	if (err < 0) { avcodec_free_context(&s->av); return err; }
	if (s->codec == 'av01' || s->codec == 'vp09') {
		// libavcodec gets the samples since the last key frame first (their pictures dropped), so the sample handed over
		// finds its references; without them (the replay did not fit) it starts at the next key frame. VP9: a sample
		// that starts with a key frame needs none (a key frame of another size: libavcodec starts there)
		if (s->replay_over) s->wait_key = !key_sample(s, s->pkt->data, (size_t)s->pkt->size);
		else if (!(s->codec == 'vp09' && vp9d_is_key_sample(s->pkt->data, (size_t)s->pkt->size))) {
			AVPacket *p = av_packet_alloc();
			size_t off = 0;
			for (int i = 0; p && i < s->replay_cnt; i++) {
				if (av_new_packet(p, (int)s->replay_len[i]) < 0) break;
				memcpy(p->data, s->replay + off, s->replay_len[i]); off += s->replay_len[i];
				p->pts = -1 - i;
				int e = avcodec_send_packet(s->av, p);
				while (avcodec_receive_frame(s->av, s->hw) == 0) av_frame_unref(s->hw);
				if (e == AVERROR(EAGAIN)) { avcodec_send_packet(s->av, p); while (avcodec_receive_frame(s->av, s->hw) == 0) av_frame_unref(s->hw); }
				av_packet_unref(p);
			}
			av_packet_free(&p);
			os_log(lg(), "intelxeav1: %d %{public}s samples since the last key frame replayed into libavcodec", s->replay_cnt, codec_name(s->codec));
		}
		free(s->replay); free(s->replay_len); s->replay = NULL; s->replay_len = NULL; s->replay_n = s->replay_cap = 0; s->replay_cnt = s->replay_lcap = 0;
	}
	return 0;
}

// AV1 / VP9: keeps the sample for a replay into libavcodec (see fall_back); a key frame (AV1: a shown one) starts the
// list again
#define REPLAY_MAX (32u << 20)
static void keep_for_replay(Dec *s, const uint8_t *data, size_t n) {
	if (key_sample(s, data, n)) { s->replay_n = 0; s->replay_cnt = 0; s->replay_over = 0; }
	if (s->replay_over) return;
	if (s->replay_n + n > REPLAY_MAX) { s->replay_over = 1; free(s->replay); s->replay = NULL; s->replay_cap = s->replay_n = 0; s->replay_cnt = 0; return; }
	if (s->replay_n + n > s->replay_cap) {
		size_t c = s->replay_cap ? s->replay_cap : 1 << 20;
		while (c < s->replay_n + n) c *= 2;
		uint8_t *r = realloc(s->replay, c);
		if (!r) { s->replay_over = 1; return; }
		s->replay = r; s->replay_cap = c;
	}
	if (s->replay_cnt == s->replay_lcap) {
		const int c = s->replay_lcap ? s->replay_lcap * 2 : 64;
		uint32_t *l = realloc(s->replay_len, (size_t)c * sizeof *l);
		if (!l) { s->replay_over = 1; return; }
		s->replay_len = l; s->replay_lcap = c;
	}
	memcpy(s->replay + s->replay_n, data, n);
	s->replay_n += n; s->replay_len[s->replay_cnt++] = (uint32_t)n;
}

// H.264 / HEVC / AV1 / VP9 through the direct path: this sample's own picture (H.264 / HEVC: in decode order; AV1 / VP9:
// the frame the temporal unit / superframe shows, or its last decoded one if it shows none), like the libavcodec path below.
// 1 if the sample is done, 0 if libavcodec takes it (fallback: s->pkt then holds the sample)
static int decode_direct(Dec *s, VTVideoDecoderFrame frame, CMBlockBufferRef bb, size_t n, VTDecodeFrameFlags flags) {
	// the sample's bytes where they are (AVAssetReader's buffers are contiguous), else one copy
	char *ptr = NULL; size_t len = 0, total = 0;
	const uint8_t *data = NULL;
	if (CMBlockBufferGetDataPointer(bb, 0, &len, &total, &ptr) == kCMBlockBufferNoErr && len == n && total == n) data = (const uint8_t *)ptr;
	else if (av_new_packet(s->pkt, (int)n) >= 0) { CMBlockBufferCopyDataBytes(bb, 0, n, s->pkt->data); data = s->pkt->data; }
	if (!data) { emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0); return 1; }
	const uint64_t t0 = now_ns();
	AVFrame *pic = NULL;
	const int r = vtd_decode(s->vtd, data, n, &pic);
	if (r == VTD_FALLBACK) {
		if (data != s->pkt->data) {
			if (av_new_packet(s->pkt, (int)n) < 0) { emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0); return 1; }
			memcpy(s->pkt->data, data, n);
		}
		if (fall_back(s) < 0) { av_packet_unref(s->pkt); emit_later(s, frame, kVTVideoDecoderMalfunctionErr, 0, NULL, 0, 0); return 1; }
		return 0;
	}
	if (s->codec == 'av01' || s->codec == 'vp09') keep_for_replay(s, data, n);
	if (data == s->pkt->data) av_packet_unref(s->pkt);
	s->send_ns = now_ns() - t0;
	if (r == VTD_PICTURE) { output(s, frame, pic, !!(flags & kVTDecodeFrame_DoNotOutputFrame)); av_frame_free(&pic); }
	else if (r == VTD_NO_PICTURE) emit_later(s, frame, 0, kVTDecodeInfo_FrameDropped, NULL, 0, 0);   // parameter sets, SEI, a skipped RASL picture
	else {
		static _Atomic int logged;
		if (logged++ < 8) os_log_error(lg(), "intelxeav1: direct %{public}s: %{public}s", codec_name(s->codec), vtd_why(s->vtd));
		emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0);
	}
	s->t_dec_ns += now_ns() - t0;
	return 1;
}

// one sample, under the session lock
static void decode_locked(Dec *s, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags) {
	CMBlockBufferRef bb = CMSampleBufferGetDataBuffer(sb);
	const size_t n = bb ? CMBlockBufferGetDataLength(bb) : 0;
	if ((!s->av && !s->vtd) || !n || n > INT_MAX) {
		if (s->emitq) emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0);
		else VTDecoderSessionEmitDecodedFrame(s->session, frame, kVTVideoDecoderBadDataErr, 0, NULL);
		return;
	}
	int have_pkt = 0;
	if (s->vtd) {
		if (decode_direct(s, frame, bb, n, flags)) return;
		have_pkt = 1;   // handed over: the sample is in s->pkt
	}
	if (!have_pkt) {
		if (av_new_packet(s->pkt, (int)n) < 0) { emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0); return; }
		CMBlockBufferCopyDataBytes(bb, 0, n, s->pkt->data);
	}
	if (decode_order(s)) {
		// H.264 / HEVC: this sample's own picture, in decode order (libavcodec's reordered output is dropped)
		const uint64_t t0 = now_ns(), allocs0 = s->allocs;
		s->allocs_mark = s->allocs;
		int err = avcodec_send_packet(s->av, s->pkt);
		if (err == AVERROR(EAGAIN)) { while (avcodec_receive_frame(s->av, s->hw) == 0) av_frame_unref(s->hw); err = avcodec_send_packet(s->av, s->pkt); }
		s->send_ns = now_ns() - t0;
		av_packet_unref(s->pkt);
		while (avcodec_receive_frame(s->av, s->hw) == 0) av_frame_unref(s->hw);
		if (err < 0) emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0);
		else if (s->allocs != allocs0 && s->last_alloc->buf[0])
			// H.264 allocates the frames it invents for frame_num gaps before the current picture, HEVC the missing
			// references it generates after it
			output(s, frame, s->codec == 'avc1' ? s->last_alloc : s->first_alloc, !!(flags & kVTDecodeFrame_DoNotOutputFrame));
		else emit_later(s, frame, 0, kVTDecodeInfo_FrameDropped, NULL, 0, 0);   // no picture in it (parameter sets, SEI)
		s->t_dec_ns += now_ns() - t0;
		return;
	}
	if (s->wait_key) {   // handed over without the samples before: libavcodec starts at a key frame
		if (!key_sample(s, s->pkt->data, (size_t)s->pkt->size)) { av_packet_unref(s->pkt); emit_later(s, frame, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0); return; }
		s->wait_key = 0;
	}
	const int64_t pts = s->next_pts++;   // a private sequence number: one temporal unit, one shown picture
	s->pkt->pts = pts;
	if (s->npend == 64) { VTVideoDecoderFrame old = take(s, s->pend[0].pts, NULL); emit_later(s, old, 0, kVTDecodeInfo_FrameDropped, NULL, 0, 0); }
	s->pend[s->npend++] = (Pending){ pts | ((flags & kVTDecodeFrame_DoNotOutputFrame) ? NO_OUTPUT_BIT : 0), frame };
	const uint64_t t0 = now_ns(), allocs0 = s->allocs;
	int err = avcodec_send_packet(s->av, s->pkt);
	if (err == AVERROR(EAGAIN)) { drain(s); err = avcodec_send_packet(s->av, s->pkt); }
	s->send_ns = now_ns() - t0;   // the decode's submission, host waits included (ff_vk_exec_get)
	av_packet_unref(s->pkt);
	unsigned i = 0; while (i < s->npend && (s->pend[i].pts & ~NO_OUTPUT_BIT) != pts) i++;
	if (err < 0) {
		if (i < s->npend) { VTVideoDecoderFrame f = take(s, s->pend[i].pts, NULL); emit_later(s, f, kVTVideoDecoderBadDataErr, 0, NULL, 0, 0); }
	} else {
		drain(s);
		// still pending: the sample decoded no shown frame. If it decoded a hidden one, that is its picture (see the top).
		i = 0; while (i < s->npend && (s->pend[i].pts & ~NO_OUTPUT_BIT) != pts) i++;
		if (i < s->npend && s->allocs != allocs0 && s->last_alloc->buf[0]) {
			int no_output = 0;
			VTVideoDecoderFrame f = take(s, s->pend[i].pts, &no_output);
			output(s, f, s->last_alloc, no_output);
			s->hidden++;
		}
	}
	s->t_dec_ns += now_ns() - t0;
}

static OSStatus DecodeFrame(VTVideoDecoderRef d, VTVideoDecoderFrame frame, CMSampleBufferRef sb, VTDecodeFrameFlags flags, VTDecodeInfoFlags *info) {
	Dec *s = dec(d);
	if (info) *info = kVTDecodeInfo_Asynchronous;
	pthread_mutex_lock(&s->lock);
	const uint64_t c0 = thread_cpu_ns();
	if (!s->t_first_ns) s->t_first_ns = now_ns();
	s->decodes++;
	decode_locked(s, frame, sb, flags);
	// without asynchronous decompression every frame must be emitted before DecodeFrame returns
	if (!(flags & kVTDecodeFrame_EnableAsynchronousDecompression)) flush_deferred(s, 's');
	s->t_cpu_ns += thread_cpu_ns() - c0;
	dispatch_queue_t q = s->emitq;
	pthread_mutex_unlock(&s->lock);
	if (!(flags & kVTDecodeFrame_EnableAsynchronousDecompression) && q) dispatch_sync(q, ^{});
	return 0;
}
static OSStatus FinishDelayedFrames(VTVideoDecoderRef d) {
	Dec *s = dec(d);
	pthread_mutex_lock(&s->lock);
	if (s->vtd) vtd_flush(s->vtd);   // as libavcodec's flush below: the references go
	if (s->av && decode_order(s)) {   // every sample was answered at decode; the delayed display output goes
		avcodec_send_packet(s->av, NULL);
		while (avcodec_receive_frame(s->av, s->hw) == 0) av_frame_unref(s->hw);
		avcodec_flush_buffers(s->av);
	} else if (s->av) { avcodec_send_packet(s->av, NULL); drain(s); avcodec_flush_buffers(s->av); }
	flush_deferred(s, 'f');
	while (s->npend) { VTVideoDecoderFrame f = take(s, s->pend[0].pts, NULL); emit_later(s, f, 0, kVTDecodeInfo_FrameDropped, NULL, 0, 0); }
	dispatch_queue_t q = s->emitq;
	pthread_mutex_unlock(&s->lock);
	if (q) dispatch_sync(q, ^{});
	return 0;
}
static OSStatus CopySupportedPropertyDictionary(VTVideoDecoderRef d, CFDictionaryRef *out) {
	const void *ik[] = { kVTPropertyReadWriteStatusKey }, *iv[] = { kVTPropertyReadWriteStatus_ReadOnly };
	CFDictionaryRef ro = CFDictionaryCreate(NULL, ik, iv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kVTDecompressionPropertyKey_UsingHardwareAcceleratedVideoDecoder }, *v[] = { ro };
	*out = CFDictionaryCreate(NULL, k, v, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(ro);
	return 0;
}
static OSStatus SetProperties(VTVideoDecoderRef d, CFDictionaryRef p) { return 0; }
static OSStatus CopySerializableProperties(VTVideoDecoderRef d, CFAllocatorRef a, CFDictionaryRef *out) { *out = NULL; return 0; }
static Boolean CanAcceptFormatDescription(VTVideoDecoderRef d, CMFormatDescriptionRef fd) { Dec *s = dec(d); return s->fd && CMFormatDescriptionEqual(s->fd, fd); }

static const struct __attribute__((aligned(8))) { uint32_t pad; CMBaseClass c; } kBase = { 0, { 1, sizeof(Dec), NULL, Invalidate, Finalize,
    CopyDebugDescription, CopyProperty, NULL, NULL, NULL } };
static const struct __attribute__((aligned(8))) { uint32_t pad; VTVideoDecoderClass c; } kDec = { 0, { 1, StartSession, DecodeFrame,
    CopySupportedPropertyDictionary, SetProperties, CopySerializableProperties, CanAcceptFormatDescription, FinishDelayedFrames, { 0 } } };
static const VTVideoDecoderVTable kVT = { 0, &kBase.c, &kDec.c };

static OSStatus create(FourCharCode codec, CFAllocatorRef a, VTVideoDecoderRef *out) {
	if (!out) return kVTParameterErr;
	CMBaseObjectRef o = NULL;
	OSStatus r = CMDerivedObjectCreate(a, &kVT, VTVideoDecoderGetClassID(), &o);
	if (r || !o) return r ? r : kVTAllocationFailedErr;
	Dec *s = CMBaseObjectGetDerivedStorage(o);
	memset(s, 0, sizeof *s);
	s->codec = codec;
	pthread_mutex_init(&s->lock, NULL);
	*out = (VTVideoDecoderRef)o;
	return 0;
}
__attribute__((visibility("default"))) OSStatus IntelXeAV1_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoDecoderRef *out) { return create('av01', a, out); }
__attribute__((visibility("default"))) OSStatus IntelXeH264Dec_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoDecoderRef *out) { return create('avc1', a, out); }
__attribute__((visibility("default"))) OSStatus IntelXeHEVCDec_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoDecoderRef *out) { return create('hvc1', a, out); }
__attribute__((visibility("default"))) OSStatus IntelXeVP9Dec_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoDecoderRef *out) { return create('vp09', a, out); }
