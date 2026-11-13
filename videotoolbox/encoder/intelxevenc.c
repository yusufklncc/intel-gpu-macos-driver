// SPDX-License-Identifier: MIT
// IntelXe video encoders for VideoToolbox: HEVC (Main, Main10), H.264 (High) and AV1 (Main) encoded on the Arc B580's media
// engine (VDEnc) through Vulkan Video (Mesa ANV), with libavcodec's Vulkan encoders (hevc_vulkan, h264_vulkan, av1_vulkan,
// LGPL, linked dynamically) doing the parameter sets, the GOP and reference bookkeeping and the rate control setup.
//
// The encoders are registered in-process (IntelXeVEnc_Register): macOS 15 VideoToolbox drops encoder bundles in
// /Library/Video/Plug-Ins that are not signed by Apple, the same as decoders. Once registered they are listed by
// VTCopyVideoEncoderList (hardware accelerated, EncoderID io.github.yusufklncc.intelxe.videoencoder.<codec>) and picked by
// VTCompressionSessionCreate for their codec type, by EncoderID or by rating.
//
// Data path: the session's CVPixelBuffer (NV12 / P010, VideoToolbox converts anything else to those first) is uploaded
// into a Vulkan frame (one CPU copy into a staging buffer, then a GPU copy; or, INTELXEVENC_UPLOAD=import, the copy
// engine reads the IOSurface itself, imported once per surface: upload_import), encoded, and the Annex B packet is rewritten
// to 4-byte length-prefixed NAL units (AV1: low-overhead OBUs without temporal delimiters) in a CMSampleBuffer whose format
// description carries the hvcC / avcC / av1C made from the encoder's parameter sets.
#include "vtencplugin.h"
#include <IOKit/IOKitLib.h>
#include <IOSurface/IOSurfaceRef.h>
#include <os/log.h>
#include <pthread.h>
#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <libproc.h>
#include <vulkan/vulkan.h>
#include "libavcodec/avcodec.h"
#include "libavutil/hwcontext.h"
#include "libavutil/hwcontext_vulkan.h"
#include "libavutil/opt.h"
#include "libavutil/pixdesc.h"

#define INTELXE_ANV_DEFAULT "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib"
#define MAX_PENDING 128
#define UP_RING 16      // import uploads in flight per encoder
#define UP_SURFACES 24  // imported IOSurfaces kept per encoder

static os_log_t lg(void) { static os_log_t l; static dispatch_once_t o; dispatch_once(&o, ^{ l = os_log_create("io.github.yusufklncc.intelxe.venc", "encoder"); }); return l; }
static const char *proc(void) { static char n[64]; if (!n[0]) proc_name(getpid(), n, sizeof n); return n; }

// ---------------------------------------------------------------------------------------------------------------------
// Vulkan device: one per process, shared by every session. libavutil's own device creation would dlopen a Vulkan loader;
// there is none on macOS, so the device is made here against ANV's ICD entry point and handed to libavutil whole.

typedef struct {
	PFN_vkGetInstanceProcAddr gipa;
	VkInstance inst;
	VkPhysicalDevice phys;
	VkDevice dev;
	VkPhysicalDeviceFeatures2 f2;
	VkPhysicalDeviceVulkan11Features f11;
	VkPhysicalDeviceVulkan12Features f12;
	VkPhysicalDeviceVulkan13Features f13;
	VkPhysicalDeviceVideoMaintenance1FeaturesKHR fvm1;
	const char *dev_exts[32]; int n_dev_exts;
	AVVulkanDeviceQueueFamily qf[8]; int n_qf;
	VkVideoCodecOperationFlagsKHR enc_ops;
	char name[256];
	int err;
	// the cached-import upload (INTELXEVENC_UPLOAD=import): the copy engine's family (a transfer-only family, the B580's
	// BCS; else the transfer family libavutil uses) and VK_EXT_external_memory_host, enabled on the device but NOT listed
	// to libavutil (it would then try host_map_frame itself; the copy path stays exactly as it was)
	int copy_family, copy_family_listed, has_host_import;
	VkDeviceSize host_align;
	const char *create_exts[40]; int n_create_exts;
} VkShared;
static VkShared g_vk;

static PFN_vkGetInstanceProcAddr anv_gipa(void) {
	static PFN_vkGetInstanceProcAddr gipa; static dispatch_once_t once;
	dispatch_once(&once, ^{
		const char *path = getenv("INTELXE_ANV");
		if (!path || !*path) path = INTELXE_ANV_DEFAULT;
		void *h = dlopen(path, RTLD_NOW | RTLD_LOCAL);
		gipa = h ? (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr") : NULL;
		os_log(lg(), "intelxevenc: %{public}s in %{public}s: %{public}s", path, proc(), gipa ? "loaded" : dlerror());
	});
	return gipa;
}

#define VKF(inst, name) PFN_##name name = (PFN_##name)g_vk.gipa(inst, #name)

static void vk_shared_init(void) {
	VkShared *v = &g_vk;
	v->err = -1;
	if (!(v->gipa = anv_gipa())) return;
	VKF(NULL, vkCreateInstance);
	VkApplicationInfo app = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "intelxevenc",
	                          .pEngineName = "libavcodec", .apiVersion = VK_API_VERSION_1_3 };
	VkInstanceCreateInfo ici = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app };
	if (!vkCreateInstance || vkCreateInstance(&ici, NULL, &v->inst) != VK_SUCCESS) { os_log_error(lg(), "intelxevenc: vkCreateInstance failed"); return; }
	VKF(v->inst, vkEnumeratePhysicalDevices);
	VKF(v->inst, vkGetPhysicalDeviceProperties);
	VKF(v->inst, vkGetPhysicalDeviceFeatures2);
	VKF(v->inst, vkGetPhysicalDeviceQueueFamilyProperties2);
	VKF(v->inst, vkEnumerateDeviceExtensionProperties);
	VKF(v->inst, vkCreateDevice);
	VkPhysicalDevice pd[8]; uint32_t n = 8;
	if (vkEnumeratePhysicalDevices(v->inst, &n, pd) < 0 || !n) { os_log_error(lg(), "intelxevenc: no Vulkan device"); return; }
	for (uint32_t i = 0; i < n && !v->phys; i++) {
		VkPhysicalDeviceProperties p; vkGetPhysicalDeviceProperties(pd[i], &p);
		if (p.vendorID == 0x8086) { v->phys = pd[i]; strlcpy(v->name, p.deviceName, sizeof v->name); }
	}
	if (!v->phys) { os_log_error(lg(), "intelxevenc: no Intel Vulkan device"); return; }

	// queue families: the encode family plus one that can transfer (the upload) and compute
	uint32_t nq = 0; vkGetPhysicalDeviceQueueFamilyProperties2(v->phys, &nq, NULL);
	VkQueueFamilyProperties2 qp[16]; VkQueueFamilyVideoPropertiesKHR qv[16];
	if (nq > 16) nq = 16;
	for (uint32_t i = 0; i < nq; i++) {
		qv[i] = (VkQueueFamilyVideoPropertiesKHR){ .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR };
		qp[i] = (VkQueueFamilyProperties2){ .sType = VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2, .pNext = &qv[i] };
	}
	vkGetPhysicalDeviceQueueFamilyProperties2(v->phys, &nq, qp);
	VkDeviceQueueCreateInfo qci[16]; uint32_t nqci = 0;
	static const float prio[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
	int have_xfer = 0, have_enc = 0;
	v->copy_family = -1;
	for (uint32_t i = 0; i < nq; i++) {
		VkQueueFlags fl = qp[i].queueFamilyProperties.queueFlags;
		int want = 0;
		if (!have_xfer && (fl & VK_QUEUE_COMPUTE_BIT) && (fl & VK_QUEUE_TRANSFER_BIT)) { want = 1; have_xfer = 1; }
		if (!have_enc && (fl & VK_QUEUE_VIDEO_ENCODE_BIT_KHR)) { want = 1; have_enc = 1; v->enc_ops = qv[i].videoCodecOperations; }
		if (!want && v->copy_family < 0 && (fl & VK_QUEUE_TRANSFER_BIT) &&
		    !(fl & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_VIDEO_ENCODE_BIT_KHR | VK_QUEUE_VIDEO_DECODE_BIT_KHR))) {
			// the copy engine: one queue, not in libavutil's base list (vk_device_ref adds it for import sessions)
			v->copy_family = (int)i;
			qci[nqci++] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = i,
			                                        .queueCount = 1, .pQueuePriorities = prio };
			continue;
		}
		if (!want) continue;
		uint32_t cnt = qp[i].queueFamilyProperties.queueCount; if (cnt > 16) cnt = 16;
		qci[nqci++] = (VkDeviceQueueCreateInfo){ .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = i,
		                                        .queueCount = cnt, .pQueuePriorities = prio };
		v->qf[v->n_qf++] = (AVVulkanDeviceQueueFamily){ .idx = (int)i, .num = (int)cnt, .flags = fl,
		                                                 .video_caps = (fl & VK_QUEUE_VIDEO_ENCODE_BIT_KHR) ? qv[i].videoCodecOperations : 0 };
	}
	if (!have_enc) { os_log_error(lg(), "intelxevenc: %{public}s has no video encode queue (ANV video off?)", v->name); return; }
	v->copy_family_listed = 0;
	if (v->copy_family < 0)   // no dedicated copy engine: the transfer family libavutil already has
		for (int i = 0; i < v->n_qf; i++) if (v->qf[i].flags & VK_QUEUE_TRANSFER_BIT) { v->copy_family = v->qf[i].idx; v->copy_family_listed = 1; break; }

	// extensions: the ones libavutil knows and the device has
	static const char *want_exts[] = {
		VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
		VK_KHR_VIDEO_ENCODE_H265_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_AV1_EXTENSION_NAME, VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME,
		VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
	};
	VkExtensionProperties ep[512]; uint32_t ne = 512;
	vkEnumerateDeviceExtensionProperties(v->phys, NULL, &ne, ep);
	for (size_t w = 0; w < sizeof want_exts / sizeof *want_exts; w++)
		for (uint32_t i = 0; i < ne; i++)
			if (!strcmp(ep[i].extensionName, want_exts[w])) { v->dev_exts[v->n_dev_exts++] = want_exts[w]; break; }
	memcpy(v->create_exts, v->dev_exts, sizeof(v->dev_exts[0]) * v->n_dev_exts);
	v->n_create_exts = v->n_dev_exts;
	for (uint32_t i = 0; i < ne; i++)
		if (!strcmp(ep[i].extensionName, VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME)) {
			v->create_exts[v->n_create_exts++] = VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME; v->has_host_import = 1; break;
		}
	if (v->has_host_import) {
		VKF(v->inst, vkGetPhysicalDeviceProperties2);
		VkPhysicalDeviceExternalMemoryHostPropertiesEXT hp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT };
		VkPhysicalDeviceProperties2 pp = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hp };
		if (vkGetPhysicalDeviceProperties2) vkGetPhysicalDeviceProperties2(v->phys, &pp);
		v->host_align = hp.minImportedHostPointerAlignment;
		if (!v->host_align) v->has_host_import = 0;
	}

	// features: everything the device has (Vulkan 1.1-1.3 and video maintenance 1)
	v->fvm1 = (VkPhysicalDeviceVideoMaintenance1FeaturesKHR){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_MAINTENANCE_1_FEATURES_KHR };
	v->f13 = (VkPhysicalDeviceVulkan13Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &v->fvm1 };
	v->f12 = (VkPhysicalDeviceVulkan12Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &v->f13 };
	v->f11 = (VkPhysicalDeviceVulkan11Features){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &v->f12 };
	v->f2 = (VkPhysicalDeviceFeatures2){ .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &v->f11 };
	int has_vm1 = 0;
	for (int i = 0; i < v->n_dev_exts; i++) if (!strcmp(v->dev_exts[i], VK_KHR_VIDEO_MAINTENANCE_1_EXTENSION_NAME)) has_vm1 = 1;
	if (!has_vm1) v->f13.pNext = NULL;
	vkGetPhysicalDeviceFeatures2(v->phys, &v->f2);
	v->f2.features.robustBufferAccess = VK_FALSE;

	VkDeviceCreateInfo dci = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &v->f2, .queueCreateInfoCount = nqci,
	                           .pQueueCreateInfos = qci, .enabledExtensionCount = (uint32_t)v->n_create_exts, .ppEnabledExtensionNames = v->create_exts };
	VkResult r = vkCreateDevice(v->phys, &dci, NULL, &v->dev);
	if (r != VK_SUCCESS) {
		os_log_error(lg(), "intelxevenc: vkCreateDevice on %{public}s -> %d", v->name, r);
		VKF(v->inst, vkDestroyInstance);
		if (vkDestroyInstance) vkDestroyInstance(v->inst, NULL);
		v->inst = NULL;
		return;
	}
	v->err = 0;
	os_log(lg(), "intelxevenc: Vulkan device %{public}s in %{public}s, encode ops 0x%x, %d extensions, copy engine family %d, host import %d (align %llu)",
	       v->name, proc(), v->enc_ops, v->n_dev_exts, v->copy_family, v->has_host_import, (unsigned long long)v->host_align);
}

// The device lives while sessions use it: the first encoder that opens creates it, the last one that goes away destroys
// it (an idle process holds no Vulkan device, no VRAM and nothing in the CPU-visible window). Encoder objects take their
// reference when their codec first opens and drop it when VideoToolbox invalidates or frees them, so a flush and
// reopen inside one session keeps the device.
static pthread_mutex_t g_vk_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_vk_refs;
static int vk_acquire(void) {
	pthread_mutex_lock(&g_vk_lock);
	if (!g_vk_refs) { memset(&g_vk, 0, sizeof g_vk); vk_shared_init(); }
	int ok = !g_vk.err;
	if (ok) g_vk_refs++;
	pthread_mutex_unlock(&g_vk_lock);
	return ok;
}
static void vk_release(void) {
	pthread_mutex_lock(&g_vk_lock);
	if (g_vk_refs && !--g_vk_refs) {
		VKF(g_vk.inst, vkDeviceWaitIdle);
		VKF(g_vk.inst, vkDestroyDevice);
		VKF(g_vk.inst, vkDestroyInstance);
		if (vkDeviceWaitIdle) vkDeviceWaitIdle(g_vk.dev);
		if (vkDestroyDevice) vkDestroyDevice(g_vk.dev, NULL);
		if (vkDestroyInstance) vkDestroyInstance(g_vk.inst, NULL);
		os_log(lg(), "intelxevenc: last session gone in %{public}s - Vulkan device destroyed", proc());
		memset(&g_vk, 0, sizeof g_vk);
		g_vk.err = -1;
	}
	pthread_mutex_unlock(&g_vk_lock);
}

static pthread_mutex_t g_vk_queue_lock = PTHREAD_MUTEX_INITIALIZER;
#if FF_API_VULKAN_SYNC_QUEUES
// One lock for the queues of the shared device. Every codec open wraps the device in its own libavutil context, and
// each context's own queue mutexes serialise only that context's submits: two sessions in one process (parallel
// encodes) called vkQueueSubmit2 on the same VkQueue at once, which Vulkan forbids (crashed in ANV's queue submit).
// A submit holds it only around vkQueueSubmit2, so one lock for every queue costs nothing measurable. The copy engine
// uploads (upload_import) take it too.
static void vk_lock_queue(AVHWDeviceContext *ctx, uint32_t family, uint32_t index) {
	(void)ctx; (void)family; (void)index; pthread_mutex_lock(&g_vk_queue_lock);
}
static void vk_unlock_queue(AVHWDeviceContext *ctx, uint32_t family, uint32_t index) {
	(void)ctx; (void)family; (void)index; pthread_mutex_unlock(&g_vk_queue_lock);
}
#endif

// a libavutil device context around the shared device (each codec open gets its own context; the caller holds a
// device reference, vk_acquire)
static AVBufferRef *vk_device_ref(int with_copy_family) {
	if (g_vk.err) return NULL;
	AVBufferRef *ref = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_VULKAN);
	if (!ref) return NULL;
	AVHWDeviceContext *dc = (AVHWDeviceContext *)ref->data;
	AVVulkanDeviceContext *h = dc->hwctx;
	h->get_proc_addr = g_vk.gipa;
	h->inst = g_vk.inst;
	h->phys_dev = g_vk.phys;
	h->act_dev = g_vk.dev;
	h->device_features = g_vk.f2;
	h->enabled_dev_extensions = g_vk.dev_exts;
	h->nb_enabled_dev_extensions = g_vk.n_dev_exts;
	h->enabled_inst_extensions = NULL;
	h->nb_enabled_inst_extensions = 0;
	memcpy(h->qf, g_vk.qf, sizeof(g_vk.qf[0]) * g_vk.n_qf);
	h->nb_qf = g_vk.n_qf;
	// import sessions: the copy engine's family too, last (libavutil's own transfers keep the first transfer family), so
	// the frames context's images are shared with it (VK_SHARING_MODE_CONCURRENT, as with more than one family already)
	if (with_copy_family && g_vk.copy_family >= 0 && !g_vk.copy_family_listed && h->nb_qf < (int)(sizeof h->qf / sizeof h->qf[0]))
		h->qf[h->nb_qf++] = (AVVulkanDeviceQueueFamily){ .idx = g_vk.copy_family, .num = 1, .flags = VK_QUEUE_TRANSFER_BIT };
	dc->free = NULL;   // the instance and device outlive every context
#if FF_API_VULKAN_SYNC_QUEUES
	h->lock_queue = vk_lock_queue;      // libavutil keeps callbacks the caller set (vulkan_device_init)
	h->unlock_queue = vk_unlock_queue;
#endif
	if (av_hwdevice_ctx_init(ref) < 0) { av_buffer_unref(&ref); return NULL; }
	return ref;
}

// ---------------------------------------------------------------------------------------------------------------------
// The encoder object

typedef struct { int64_t key; VTEncoderFrameRef frame; CMTime pts, dur; int force_key; } Pending;

// The cached-import upload (INTELXEVENC_UPLOAD=import, see upload_import)
typedef struct { IOSurfaceID id; IOSurfaceRef surf; VkBuffer buf; VkDeviceMemory mem; uint64_t last_use; } UpSurface;
typedef struct {
	int mode;                        // 0 copy (libavutil's staging upload), 1 import
	int ready, broken;               // the copy engine state exists; import turned off for this open (failures)
	int fail_run;                    // import failures in a row
	VkDevice d; VkQueue q; uint32_t qfam;
	VkCommandPool pool; VkCommandBuffer cmd[UP_RING]; uint64_t cmd_value[UP_RING]; CVPixelBufferRef cmd_pb[UP_RING];
	unsigned next;
	VkSemaphore done; uint64_t submitted;   // timeline: one value per upload
	UpSurface imp[UP_SURFACES];
	struct {
		PFN_vkBeginCommandBuffer Begin; PFN_vkEndCommandBuffer End; PFN_vkResetCommandBuffer Reset;
		PFN_vkCmdPipelineBarrier2 Barrier; PFN_vkCmdCopyBufferToImage Copy; PFN_vkQueueSubmit2 Submit;
		PFN_vkWaitSemaphores Wait; PFN_vkGetSemaphoreCounterValue Counter;
		PFN_vkCreateBuffer CreateBuffer; PFN_vkDestroyBuffer DestroyBuffer; PFN_vkGetBufferMemoryRequirements Reqs;
		PFN_vkAllocateMemory Alloc; PFN_vkFreeMemory Free; PFN_vkBindBufferMemory Bind;
		PFN_vkGetMemoryHostPointerPropertiesEXT HostProps;
		PFN_vkDestroyCommandPool DestroyPool; PFN_vkDestroySemaphore DestroySem;
	} vk;
} Upload;
typedef struct { int64_t imported, fallback, imports, evictions, waits; } UpStats;

typedef struct {
	pthread_mutex_t lock;
	FourCharCode codec;
	VTEncoderSessionRef session;
	CMVideoDimensions dim;
	// properties as the client set them
	int32_t avg_bitrate;             // bits/s, 0 = unset
	int32_t cbr;                     // kVTCompressionPropertyKey_ConstantBitRate (bits/s)
	int64_t max_rate_bytes; double max_rate_secs;
	double quality;                  // < 0 = unset
	int32_t max_keyint; double max_keyint_secs;
	int realtime, allow_reorder, allow_temporal, prio_speed;
	double expected_fps;
	CFStringRef profile_level;
	CFStringRef primaries, transfer, matrix;
	int ten_bit;
	// codec state
	AVBufferRef *dev, *frames;
	AVCodecContext *av; AVFrame *sw, *hw; AVPacket *pkt;
	CMVideoFormatDescriptionRef fd;
	int32_t cv_format;               // the pixel format the frames context was made for
	int32_t timescale;               // of the presentation timestamps coming in
	Pending pend[MAX_PENDING]; unsigned npend;
	int64_t frames_in, frames_out, bytes_out, errors;
	uint64_t t_upload_ns, t_encode_ns, t_start_ns;
	Upload up; UpStats ust;
	int async_depth;                 // libavcodec's async_depth this open (INTELXEVENC_ASYNC)
	int open_failed;
	int vk_held;                     // this encoder holds a device reference
	// the frame rate when the client gives neither ExpectedFrameRate nor frame durations (FFmpeg's *_videotoolbox
	// encoders): the first frame waits for the second, whose timestamp gives the rate the codec is opened with
	struct { VTEncoderFrameRef frame; CVImageBufferRef img; CMTime pts, dur; CFDictionaryRef props; } held;
	double est_fps;
} Enc;

static Enc *enc_of(VTVideoEncoderRef e) { return (Enc *)CMBaseObjectGetDerivedStorage(VTVideoEncoderGetCMBaseObject(e)); }
static const char *codec_name(FourCharCode c) { return c == 'hvc1' ? "HEVC" : c == 'avc1' ? "H.264" : "AV1"; }

// The last encoder's statistics in this process, as one line (IntelXeVEnc_LastStats; vtencq --lib prints it)
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_stats[512];

static void up_fini(Enc *s);
static void close_codec(Enc *s) {
	if (s->frames_in) {
		char line[512];
		snprintf(line, sizeof line, "%s %dx%d upload=%s async=%d frames=%lld out=%lld bytes=%lld errors=%lld fps=%.1f upload_ms=%.3f encode_ms=%.3f "
		         "imported=%lld fallback=%lld imports=%lld evictions=%lld ring_waits=%lld",
		         codec_name(s->codec), s->dim.width, s->dim.height, s->up.mode ? "import" : "copy", s->async_depth, s->frames_in, s->frames_out, s->bytes_out, s->errors,
		         s->t_start_ns ? s->frames_out / ((clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - s->t_start_ns) / 1e9) : 0.0,
		         s->t_upload_ns / 1e6 / s->frames_in, s->t_encode_ns / 1e6 / s->frames_in,
		         s->ust.imported, s->ust.fallback, s->ust.imports, s->ust.evictions, s->ust.waits);
		os_log(lg(), "intelxevenc: close in %{public}s: %{public}s", proc(), line);
		pthread_mutex_lock(&g_stats_lock); strlcpy(g_stats, line, sizeof g_stats); pthread_mutex_unlock(&g_stats_lock);
	}
	up_fini(s);
	avcodec_free_context(&s->av);
	av_buffer_unref(&s->frames); av_buffer_unref(&s->dev);
	av_frame_free(&s->sw); av_frame_free(&s->hw); av_packet_free(&s->pkt);
	if (s->fd) { CFRelease(s->fd); s->fd = NULL; }
	s->frames_in = s->frames_out = s->bytes_out = s->errors = 0;
	s->t_upload_ns = s->t_encode_ns = s->t_start_ns = 0;
	memset(&s->ust, 0, sizeof s->ust);
	s->open_failed = 0;
}
static void release_props(Enc *s) {
	if (s->profile_level) { CFRelease(s->profile_level); s->profile_level = NULL; }
	if (s->primaries) { CFRelease(s->primaries); s->primaries = NULL; }
	if (s->transfer) { CFRelease(s->transfer); s->transfer = NULL; }
	if (s->matrix) { CFRelease(s->matrix); s->matrix = NULL; }
}

static void drop_pending(Enc *s, OSStatus st) {
	while (s->npend) {
		VTEncoderFrameRef f = s->pend[0].frame;
		memmove(&s->pend[0], &s->pend[1], (--s->npend) * sizeof s->pend[0]);
		VTEncoderSessionEmitEncodedFrame(s->session, f, st, st ? 0 : kVTEncodeInfo_FrameDropped, NULL);
	}
}

static void release_held(Enc *s, int encode);
static OSStatus Invalidate(CMBaseObjectRef o) {
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	pthread_mutex_lock(&s->lock);
	if (s->session) { release_held(s, 0); drop_pending(s, 0); }
	close_codec(s);
	if (s->vk_held) { s->vk_held = 0; vk_release(); }
	s->session = NULL;
	pthread_mutex_unlock(&s->lock);
	return 0;
}
static void Finalize(CMBaseObjectRef o) {
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	if (s->held.img) { CVBufferRelease(s->held.img); if (s->held.props) CFRelease(s->held.props); memset(&s->held, 0, sizeof s->held); }
	close_codec(s); release_props(s);
	if (s->vk_held) { s->vk_held = 0; vk_release(); }
	pthread_mutex_destroy(&s->lock);
}
static CFStringRef CopyDebugDescription(CMBaseObjectRef o) {
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	return CFStringCreateWithFormat(NULL, NULL, CFSTR("<IntelXe %s encoder (Vulkan Video) %p>"), codec_name(s->codec), o);
}

// ---------------------------------------------------------------------------------------------------------------------
// Properties

static int num_i64(CFTypeRef v, int64_t *out) { return v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberSInt64Type, out); }
static int num_dbl(CFTypeRef v, double *out) { return v && CFGetTypeID(v) == CFNumberGetTypeID() && CFNumberGetValue(v, kCFNumberDoubleType, out); }
static int boolean(CFTypeRef v, int *out) {
	if (v && CFGetTypeID(v) == CFBooleanGetTypeID()) { *out = CFBooleanGetValue(v); return 1; }
	int64_t i; if (num_i64(v, &i)) { *out = i != 0; return 1; }
	return 0;
}
static void set_str(CFStringRef *dst, CFTypeRef v) {
	if (*dst) CFRelease(*dst);
	*dst = v && CFGetTypeID(v) == CFStringGetTypeID() ? CFRetain(v) : NULL;
}

static OSStatus SetProperty(CMBaseObjectRef o, CFStringRef key, CFTypeRef v) {
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	OSStatus r = 0;
	int64_t i; double d; int b;
	pthread_mutex_lock(&s->lock);
	if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) { if (num_i64(v, &i) && i >= 0) s->avg_bitrate = (int32_t)i; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_ConstantBitRate)) { if (num_i64(v, &i) && i >= 0) s->cbr = (int32_t)i; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_DataRateLimits)) {
		// [bytes, seconds] pairs; the tightest one becomes the peak rate
		s->max_rate_bytes = 0; s->max_rate_secs = 0;
		if (v && CFGetTypeID(v) == CFArrayGetTypeID()) {
			CFIndex n = CFArrayGetCount(v);
			for (CFIndex k = 0; k + 1 < n; k += 2) {
				int64_t by; double se;
				if (num_i64(CFArrayGetValueAtIndex(v, k), &by) && num_dbl(CFArrayGetValueAtIndex(v, k + 1), &se) && se > 0 && by > 0 &&
				    (!s->max_rate_secs || by / se < s->max_rate_bytes / s->max_rate_secs)) { s->max_rate_bytes = by; s->max_rate_secs = se; }
			}
		} else if (v) r = kVTPropertyNotSupportedErr;
	}
	else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) { if (num_dbl(v, &d)) s->quality = d < 0 ? 0 : d > 1 ? 1 : d; else if (!v) s->quality = -1; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) { if (num_i64(v, &i)) s->max_keyint = (int32_t)i; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration)) { if (num_dbl(v, &d)) s->max_keyint_secs = d; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) { if (boolean(v, &b)) s->realtime = b; else if (!v) s->realtime = 0; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) { if (boolean(v, &b)) s->allow_reorder = b; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_AllowTemporalCompression)) { if (boolean(v, &b)) s->allow_temporal = b; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality)) { if (boolean(v, &b)) s->prio_speed = b; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) { if (num_dbl(v, &d) && d > 0) s->expected_fps = d; else r = kVTPropertyNotSupportedErr; }
	else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel)) set_str(&s->profile_level, v);
	else if (CFEqual(key, kVTCompressionPropertyKey_ColorPrimaries)) set_str(&s->primaries, v);
	else if (CFEqual(key, kVTCompressionPropertyKey_TransferFunction)) set_str(&s->transfer, v);
	else if (CFEqual(key, kVTCompressionPropertyKey_YCbCrMatrix)) set_str(&s->matrix, v);
	else if (CFEqual(key, kVTCompressionPropertyKey_MaximizePowerEfficiency) || CFEqual(key, kVTCompressionPropertyKey_ExpectedDuration) ||
	         CFEqual(key, kVTCompressionPropertyKey_MaxFrameDelayCount)) r = 0;   // accepted, no effect
	else r = kVTPropertyNotSupportedErr;
	// a running stream keeps the settings it was opened with; the next session start picks the new ones up
	if (!r && s->av) os_log(lg(), "intelxevenc: %{public}@ changed while encoding - applies from the next session", key);
	pthread_mutex_unlock(&s->lock);
	return r;
}

static CFNumberRef i32(int32_t v) { return CFNumberCreate(NULL, kCFNumberSInt32Type, &v); }
static CFNumberRef dbl(double v) { return CFNumberCreate(NULL, kCFNumberDoubleType, &v); }

static OSStatus CopyProperty(CMBaseObjectRef o, CFStringRef key, CFAllocatorRef a, void *out) {
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	CFTypeRef *res = out;
	OSStatus r = 0;
	pthread_mutex_lock(&s->lock);
	if (CFEqual(key, kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder)) *res = CFRetain(kCFBooleanTrue);
	else if (CFEqual(key, kVTCompressionPropertyKey_AverageBitRate)) *res = i32(s->avg_bitrate);
	else if (CFEqual(key, kVTCompressionPropertyKey_ConstantBitRate)) *res = i32(s->cbr);
	else if (CFEqual(key, kVTCompressionPropertyKey_Quality)) *res = s->quality >= 0 ? dbl(s->quality) : NULL;
	else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameInterval)) *res = i32(s->max_keyint);
	else if (CFEqual(key, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration)) *res = dbl(s->max_keyint_secs);
	else if (CFEqual(key, kVTCompressionPropertyKey_RealTime)) *res = CFRetain(s->realtime ? kCFBooleanTrue : kCFBooleanFalse);
	else if (CFEqual(key, kVTCompressionPropertyKey_AllowFrameReordering)) *res = CFRetain(s->allow_reorder ? kCFBooleanTrue : kCFBooleanFalse);
	else if (CFEqual(key, kVTCompressionPropertyKey_AllowTemporalCompression)) *res = CFRetain(s->allow_temporal ? kCFBooleanTrue : kCFBooleanFalse);
	else if (CFEqual(key, kVTCompressionPropertyKey_ExpectedFrameRate)) *res = dbl(s->expected_fps);
	else if (CFEqual(key, kVTCompressionPropertyKey_ProfileLevel)) *res = s->profile_level ? CFRetain(s->profile_level) : NULL;
	else r = kVTPropertyNotSupportedErr;
	pthread_mutex_unlock(&s->lock);
	return r;
}

static OSStatus CopySupportedPropertyDictionary(VTVideoEncoderRef e, CFDictionaryRef *out) {
	Enc *s = enc_of(e);
	const void *rok[] = { kVTPropertyReadWriteStatusKey }, *rov[] = { kVTPropertyReadWriteStatus_ReadOnly }, *rwv[] = { kVTPropertyReadWriteStatus_ReadWrite };
	CFDictionaryRef ro = CFDictionaryCreate(NULL, rok, rov, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFDictionaryRef rw = CFDictionaryCreate(NULL, rok, rwv, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	// ProfileLevel: the values this encoder honours
	CFMutableArrayRef profiles = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	if (s->codec == 'hvc1') { CFArrayAppendValue(profiles, kVTProfileLevel_HEVC_Main_AutoLevel); CFArrayAppendValue(profiles, kVTProfileLevel_HEVC_Main10_AutoLevel); }
	else if (s->codec == 'avc1') {
		CFArrayAppendValue(profiles, kVTProfileLevel_H264_High_AutoLevel); CFArrayAppendValue(profiles, kVTProfileLevel_H264_Main_AutoLevel);
		CFArrayAppendValue(profiles, kVTProfileLevel_H264_ConstrainedHigh_AutoLevel); CFArrayAppendValue(profiles, kVTProfileLevel_H264_Baseline_AutoLevel);
		CFArrayAppendValue(profiles, kVTProfileLevel_H264_ConstrainedBaseline_AutoLevel);
	}
	const void *plk[] = { kVTPropertyReadWriteStatusKey, kVTPropertySupportedValueListKey }, *plv[] = { kVTPropertyReadWriteStatus_ReadWrite, profiles };
	CFDictionaryRef pl = CFDictionaryCreate(NULL, plk, plv, CFArrayGetCount(profiles) ? 2 : 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *k[] = {
		kVTCompressionPropertyKey_UsingHardwareAcceleratedVideoEncoder, kVTCompressionPropertyKey_AverageBitRate,
		kVTCompressionPropertyKey_ConstantBitRate, kVTCompressionPropertyKey_DataRateLimits, kVTCompressionPropertyKey_Quality,
		kVTCompressionPropertyKey_MaxKeyFrameInterval, kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, kVTCompressionPropertyKey_RealTime,
		kVTCompressionPropertyKey_AllowFrameReordering, kVTCompressionPropertyKey_AllowTemporalCompression,
		kVTCompressionPropertyKey_PrioritizeEncodingSpeedOverQuality, kVTCompressionPropertyKey_ExpectedFrameRate,
		kVTCompressionPropertyKey_ProfileLevel, kVTCompressionPropertyKey_ColorPrimaries, kVTCompressionPropertyKey_TransferFunction,
		kVTCompressionPropertyKey_YCbCrMatrix, kVTCompressionPropertyKey_MaximizePowerEfficiency, kVTCompressionPropertyKey_ExpectedDuration,
		kVTCompressionPropertyKey_MaxFrameDelayCount,
	};
	const void *v[sizeof k / sizeof *k];
	for (size_t i = 0; i < sizeof k / sizeof *k; i++) v[i] = rw;
	v[0] = ro;
	v[12] = pl;
	*out = CFDictionaryCreate(NULL, k, v, sizeof k / sizeof *k, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	CFRelease(ro); CFRelease(rw); CFRelease(pl); CFRelease(profiles);
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Session start: tells VideoToolbox which pixel formats to hand over

static OSStatus StartSession(VTVideoEncoderRef e, VTEncoderSessionRef session, CMVideoDimensions dim, CFDictionaryRef spec) {
	Enc *s = enc_of(e);
	if (dim.width < 64 || dim.height < 64 || dim.width > 8192 || dim.height > 8192) return kVTCouldNotFindVideoEncoderErr;
	if (!anv_gipa()) return kVTCouldNotFindVideoEncoderErr;
	pthread_mutex_lock(&s->lock);
	if (s->session) release_held(s, 0);
	close_codec(s);
	s->session = session; s->dim = dim; s->est_fps = 0;
	pthread_mutex_unlock(&s->lock);
	// NV12 (video and full range) and P010; VideoToolbox converts other sources to these
	int32_t fmts[] = { kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr8BiPlanarFullRange,
	                   kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange, kCVPixelFormatType_420YpCbCr10BiPlanarFullRange };
	int nf = s->codec == 'avc1' ? 2 : 4;   // H.264 is 8-bit only on this engine
	CFMutableArrayRef fa = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
	for (int i = 0; i < nf; i++) { CFNumberRef n = i32(fmts[i]); CFArrayAppendValue(fa, n); CFRelease(n); }
	CFNumberRef w = i32(dim.width), h = i32(dim.height);
	CFDictionaryRef io = CFDictionaryCreate(NULL, NULL, NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	const void *k[] = { kCVPixelBufferWidthKey, kCVPixelBufferHeightKey, kCVPixelBufferPixelFormatTypeKey, kCVPixelBufferIOSurfacePropertiesKey };
	const void *v[] = { w, h, fa, io };
	CFDictionaryRef attrs = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	OSStatus r = VTEncoderSessionSetPixelBufferAttributes(session, attrs);
	CFRelease(attrs); CFRelease(io); CFRelease(w); CFRelease(h); CFRelease(fa);
	os_log(lg(), "intelxevenc: %{public}s session in %{public}s: %dx%d -> %d", codec_name(s->codec), proc(), dim.width, dim.height, (int)r);
	return r;
}

// ---------------------------------------------------------------------------------------------------------------------
// Opening the libavcodec encoder at the first frame (the client sets its properties between session start and then)

static int is10(int32_t f) { return f == kCVPixelFormatType_420YpCbCr10BiPlanarVideoRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; }
static int isfull(int32_t f) { return f == kCVPixelFormatType_420YpCbCr8BiPlanarFullRange || f == kCVPixelFormatType_420YpCbCr10BiPlanarFullRange; }

static enum AVColorPrimaries map_primaries(CFStringRef p) {
	if (!p) return AVCOL_PRI_UNSPECIFIED;
	if (CFEqual(p, kCVImageBufferColorPrimaries_ITU_R_709_2)) return AVCOL_PRI_BT709;
	if (CFEqual(p, kCVImageBufferColorPrimaries_ITU_R_2020)) return AVCOL_PRI_BT2020;
	if (CFEqual(p, kCVImageBufferColorPrimaries_SMPTE_C)) return AVCOL_PRI_SMPTE170M;
	if (CFEqual(p, kCVImageBufferColorPrimaries_EBU_3213)) return AVCOL_PRI_BT470BG;
	if (CFEqual(p, kCVImageBufferColorPrimaries_P3_D65)) return AVCOL_PRI_SMPTE432;
	return AVCOL_PRI_UNSPECIFIED;
}
static enum AVColorTransferCharacteristic map_transfer(CFStringRef t) {
	if (!t) return AVCOL_TRC_UNSPECIFIED;
	if (CFEqual(t, kCVImageBufferTransferFunction_ITU_R_709_2)) return AVCOL_TRC_BT709;
	if (CFEqual(t, kCVImageBufferTransferFunction_SMPTE_ST_2084_PQ)) return AVCOL_TRC_SMPTE2084;
	if (CFEqual(t, kCVImageBufferTransferFunction_ITU_R_2100_HLG)) return AVCOL_TRC_ARIB_STD_B67;
	if (CFEqual(t, kCVImageBufferTransferFunction_sRGB)) return AVCOL_TRC_IEC61966_2_1;
	if (CFEqual(t, kCVImageBufferTransferFunction_Linear)) return AVCOL_TRC_LINEAR;
	return AVCOL_TRC_UNSPECIFIED;
}
static enum AVColorSpace map_matrix(CFStringRef m) {
	if (!m) return AVCOL_SPC_UNSPECIFIED;
	if (CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_709_2)) return AVCOL_SPC_BT709;
	if (CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_601_4)) return AVCOL_SPC_SMPTE170M;
	if (CFEqual(m, kCVImageBufferYCbCrMatrix_ITU_R_2020)) return AVCOL_SPC_BT2020_NCL;
	return AVCOL_SPC_UNSPECIFIED;
}

static int open_codec(Enc *s, int32_t cv_format, CMTime pts, CMTime dur) {
	s->cv_format = cv_format;
	s->ten_bit = is10(cv_format);
	if (s->ten_bit && s->codec == 'avc1') return AVERROR(EINVAL);
	s->timescale = pts.timescale > 0 ? pts.timescale : 1000000;
	if (!s->vk_held) { if (!vk_acquire()) return AVERROR(ENODEV); s->vk_held = 1; }
	{	// INTELXEVENC_UPLOAD=import|copy (default import since 0.1.5: 1-9% faster in 7 of 8 B580 runs of 13 Nov, the
		// streams bit-identical; copy where the device has no copy family or host import): see upload_import
		const char *um = getenv("INTELXEVENC_UPLOAD");
		const int want_import = !um || !*um || strcmp(um, "copy") != 0;
		s->up.mode = want_import && g_vk.has_host_import && g_vk.copy_family >= 0;
		if (um && !strcmp(um, "import") && !s->up.mode)
			os_log_error(lg(), "intelxevenc: INTELXEVENC_UPLOAD=import without a copy family or host import (%d, %d) - staging copies",
			             g_vk.copy_family, g_vk.has_host_import);
	}
	if (!(s->dev = vk_device_ref(s->up.mode))) return AVERROR(ENODEV);
	s->frames = av_hwframe_ctx_alloc(s->dev);
	if (!s->frames) return AVERROR(ENOMEM);
	AVHWFramesContext *fc = (AVHWFramesContext *)s->frames->data;
	fc->format = AV_PIX_FMT_VULKAN;
	fc->sw_format = s->ten_bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
	fc->width = s->dim.width; fc->height = s->dim.height;
	int err = av_hwframe_ctx_init(s->frames);
	if (err < 0) return err;

	const char *name = s->codec == 'hvc1' ? "hevc_vulkan" : s->codec == 'avc1' ? "h264_vulkan" : "av1_vulkan";
	const AVCodec *codec = avcodec_find_encoder_by_name(name);
	if (!codec || !(s->av = avcodec_alloc_context3(codec))) return AVERROR_ENCODER_NOT_FOUND;
	AVCodecContext *c = s->av;
	c->width = s->dim.width; c->height = s->dim.height;
	c->pix_fmt = AV_PIX_FMT_VULKAN;
	c->sw_pix_fmt = fc->sw_format;
	c->hw_frames_ctx = av_buffer_ref(s->frames);
	c->time_base = (AVRational){ 1, s->timescale };
	double fps = s->expected_fps > 0 ? s->expected_fps : CMTIME_IS_NUMERIC(dur) && dur.value > 0 ? (double)dur.timescale / dur.value :
	             s->est_fps > 0 ? s->est_fps : 30.0;
	c->framerate = av_d2q(fps, 1001000);
	c->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
	c->color_range = isfull(cv_format) ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
	c->color_primaries = map_primaries(s->primaries);
	c->color_trc = map_transfer(s->transfer);
	c->colorspace = map_matrix(s->matrix);

	// GOP: a key frame at least every max_keyint frames / max_keyint_secs seconds (VideoToolbox's default is none)
	int gop = s->max_keyint > 0 ? s->max_keyint : 0;
	if (s->max_keyint_secs > 0) { int g = (int)floor(s->max_keyint_secs * fps); if (g > 0 && (!gop || g < gop)) gop = g; }
	if (!s->allow_temporal) gop = 1;
	c->gop_size = gop > 0 ? gop : 300;
	// B-frames only when reordering is allowed and latency does not matter, in Intel's default GOPs for VDEnc at TU4
	// (oneVPL on Xe2, logged by ffmpeg {hevc,av1,h264}_qsv -v verbose):
	// - HEVC and AV1: GopRefDist 8, BRefType pyramid: 7 B frames between the anchors in a dyadic pyramid of 3 levels
	//   (FFmpeg b_depth 3), which the HuC BRC models per level (Mesa 0075, as media-driver
	//   encode_huc_brc_{init,update}_packet.cpp / encode_av1_brc.cpp). HEVC 1080p60 VBR 3/6/12 Mbit/s on Aerial: BD-rate
	//   -12.9% PSNR-Y / -3.5% VMAF against 2 flat B frames. AV1 needs FFmpeg to send several hidden frames per temporal
	//   unit (codecs-enc2 libavcodec); before, it had 1 B frame.
	// - H.264: GopRefDist 4, pyramid: 3 B frames in 2 levels (b_depth 2). Needs Mesa 0076 (H.264 slice headers with
	//   reference list modifications and MMCOs); before, 2 flat B frames.
	// One P reference: Intel's P frames use two (NumRefActiveP 2 at TU4), which now encode right (Mesa 0077, codecs-enc2
	// libavcodec) but measured worse on the B580 here (INTELXEVENC_REFS=2 to try).
	c->max_b_frames = s->allow_reorder && !s->realtime && gop != 1 ? (s->codec == 'avc1' ? 3 : 7) : 0;
	{ const char *be = getenv("INTELXEVENC_BFRAMES"); if (be && *be && c->max_b_frames) c->max_b_frames = atoi(be); }   // tests
	int b_depth = c->max_b_frames >= 7 ? 3 : c->max_b_frames >= 3 ? 2 : 1;
	{ const char *bd = getenv("INTELXEVENC_BDEPTH"); if (bd && *bd) b_depth = atoi(bd); }   // tests

	// rate control: constant bit rate, average bit rate (VBR, optionally capped by the data rate limits), or quality
	const char *rc = "cqp";
	if (s->cbr > 0) { rc = "cbr"; c->bit_rate = s->cbr; c->rc_max_rate = s->cbr; }
	else if (s->avg_bitrate > 0) {
		rc = "vbr"; c->bit_rate = s->avg_bitrate;
		if (s->max_rate_secs > 0) c->rc_max_rate = (int64_t)(s->max_rate_bytes * 8 / s->max_rate_secs);
		if (c->rc_max_rate < c->bit_rate) c->rc_max_rate = 0;
		{	/* H.264 with no data rate limit from the client: a peak of twice the average, as VAAPI encoders get by
			 * default (FFmpeg vaapi_encode: rc_max_rate unset -> 2 x bit_rate). Without it the Vulkan encoder runs
			 * VBR with the peak at the average, which squeezes I frames (BD-rate against x264 +10.5% -> +9.5% on
			 * Aerial). HEVC and AV1 keep the peak at the average: there 2x was neutral on Aerial and cost HEVC 1.0
			 * point of PSNR-Y BD-rate on DrivingPOV. INTELXEVENC_PEAK=x sets the factor for any codec, 0 turns it off. */
			const char *pe = getenv("INTELXEVENC_PEAK");
			const double f = pe && *pe ? atof(pe) : s->codec == 'avc1' ? 2.0 : 0.0;
			if (!c->rc_max_rate && f > 1.0) c->rc_max_rate = (int64_t)(f * c->bit_rate);
		}
	}
	av_opt_set(c->priv_data, "rc_mode", rc, 0);
	if (b_depth > 1) av_opt_set_int(c->priv_data, "b_depth", b_depth, 0);
	{ const char *re = getenv("INTELXEVENC_REFS"); if (re && *re) c->refs = atoi(re); }   // tests
	if (!strcmp(rc, "cqp")) {
		// Quality 0..1 (1 = best) onto QP 48..12 (AV1: q index 240..48); unset = 0.5-ish, VideoToolbox's default
		double q = s->quality >= 0 ? s->quality : 0.6;
		int qp = s->codec == 'av01' ? (int)lround(240 - q * 192) : (int)lround(48 - q * 36);
		av_opt_set_int(c->priv_data, "qp", qp, 0);
	}
	if (c->bit_rate) c->rc_buffer_size = (int)(c->rc_max_rate ? c->rc_max_rate : c->bit_rate);   // one second
	{	/* AV1: the buffer starts half full, oneVPL's default (av1ehw_base_general.cpp: InitialDelayInKB =
		 * BufferSizeInKB / 2) instead of FFmpeg's 3/4. With 3/4 the HuC BRC spends the extra quarter second up
		 * front: +8.4% over the target on DrivingPOV 2160p (5 s clips), +1.4..+4.3% elsewhere; with 1/2 -2.0..+2.5%.
		 * INTELXEVENC_INITBUF=x sets the fraction for any codec (tests). */
		const char *ib = getenv("INTELXEVENC_INITBUF");
		const double f = ib && *ib ? atof(ib) : s->codec == 'av01' ? 0.5 : 0.0;
		if (f > 0 && c->rc_buffer_size) c->rc_initial_buffer_occupancy = (int)(f * c->rc_buffer_size);
	}
	if (s->realtime) av_opt_set(c->priv_data, "tune", "ll", 0);
	// VDEnc target usage (ANV maps the HEVC and H.264 quality levels 0/1/2 onto TU 7/4/1, AV1 onto TU 7/4/2 with
	// Mesa 0049; H.264 had one level, TU 4, before Mesa 0084). AV1 used to get no level (always 0 = TU 7). Level 2
	// (TU 2) measured -1.1..-1.8% PSNR-Y BD-rate better than TU 4 on 4 of 5 cells but at half the 2160p speed, so
	// balanced (TU 4, Intel's default) stays the default.
	// FFmpeg's default, level 0, is the fastest (TU 7). Like VideoToolbox's own encoders, balanced unless the client
	// asks for speed (RealTime / PrioritizeEncodingSpeedOverQuality). INTELXEVENC_QUALITY overrides (tests).
	if (s->codec == 'hvc1' || s->codec == 'av01' || s->codec == 'avc1') {
		int ql = s->realtime || s->prio_speed ? 0 : 1;
		const char *qe = getenv("INTELXEVENC_QUALITY");
		if (qe && *qe) ql = atoi(qe);
		av_opt_set_int(c->priv_data, "quality", ql, 0);
	}

	// Encodes in flight (libavcodec's async_depth, default 2): with two issued, avcodec_receive_packet blocks in
	// vkWaitForFences on the older one before the next frame is uploaded and issued (hw_base_encode.c, async_encode).
	// It only sizes the encode FIFO and the timestamp ring (the dts values stay the same); INTELXEVENC_ASYNC=N to measure
	// deeper pipelines (tests, 12 Nov: not measured yet).
	s->async_depth = 2;
	{ const char *ad = getenv("INTELXEVENC_ASYNC"); if (ad && atoi(ad) >= 1 && atoi(ad) <= 64) { s->async_depth = atoi(ad); av_opt_set_int(c->priv_data, "async_depth", s->async_depth, 0); } }

	// profile
	if (s->codec == 'hvc1') c->profile = s->ten_bit ? AV_PROFILE_HEVC_MAIN_10 : AV_PROFILE_HEVC_MAIN;
	else if (s->codec == 'avc1') {
		c->profile = AV_PROFILE_H264_HIGH;
		if (s->profile_level && (CFStringHasPrefix(s->profile_level, CFSTR("H264_Main")))) c->profile = AV_PROFILE_H264_MAIN;
		if (s->profile_level && (CFStringHasPrefix(s->profile_level, CFSTR("H264_ConstrainedBaseline")) ||
		                         CFStringHasPrefix(s->profile_level, CFSTR("H264_Baseline")))) {
			c->profile = AV_PROFILE_H264_CONSTRAINED_BASELINE; c->max_b_frames = 0;
		}
	} else c->profile = AV_PROFILE_AV1_MAIN;

	err = avcodec_open2(c, codec, NULL);
	if (err < 0) return err;
	s->sw = av_frame_alloc(); s->hw = av_frame_alloc(); s->pkt = av_packet_alloc();
	if (!s->sw || !s->hw || !s->pkt) return AVERROR(ENOMEM);
	os_log(lg(), "intelxevenc: %{public}s open in %{public}s: %dx%d %{public}s %{public}s range, %.2f fps, gop %d, %d B-frames, rc %{public}s %lld/%lld bit/s%{public}s, upload %{public}s",
	       codec_name(s->codec), proc(), c->width, c->height, s->ten_bit ? "10-bit" : "8-bit", isfull(cv_format) ? "full" : "video",
	       fps, c->gop_size, c->max_b_frames, rc, (long long)c->bit_rate, (long long)c->rc_max_rate, s->realtime ? ", real time" : "",
	       s->up.mode ? "import (copy engine)" : "copy");
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Upload, INTELXEVENC_UPLOAD=import: the pixel buffer's IOSurface is read by the copy engine straight into the
// encoder's input image, without the CPU copy into libavutil's staging buffer (INTELXEVENC_UPLOAD=copy; import is the
// default since 0.1.5).
// - Each IOSurface's pages are imported once (VK_EXT_external_memory_host, a transfer-source buffer over the whole
//   surface) and kept, keyed by IOSurfaceID and the surface itself (retained, so its ID cannot be reused meanwhile):
//   CVPixelBuffer pools recycle a handful of surfaces, so after the first frames every upload is a cache hit. An import
//   per frame (libavutil's host_map_frame) cost more than the ~0.3 ms 1080p CPU copy it saves (11 Nov: 245 vs 352 fps).
//   A surface that only the cache still holds (the client's pool let it go: a new pool, a size change) is dropped once
//   its last upload is done; past UP_SURFACES surfaces the least recently used one goes.
// - The copy: vkCmdCopyBufferToImage on the copy engine (a transfer-only family; else libavutil's transfer family),
//   synchronised with libavutil exactly as its own uploads are (vulkan_transfer_frame, ff_vk_exec_add_dep_frame,
//   ff_vk_exec_submit): under the frame's lock_frame, a barrier from the frame's current layout/access to
//   TRANSFER_DST_OPTIMAL (the images are VK_SHARING_MODE_CONCURRENT across the listed families, the copy family
//   included, so no ownership transfer), a wait on each image's timeline semaphore at sem_value and a signal at
//   sem_value + 1, then sem_value++, layout and access updated after a successful submit. The encode submission
//   waits on that value (ff_vk_exec_add_dep_frame in vulkan_encode_issue) and moves the image from
//   TRANSFER_DST_OPTIMAL to VIDEO_ENCODE_SRC, as after libavutil's upload.
// - The pixel buffer: locked read-only (CVPixelBufferLockBaseAddress, the same lock the copy path takes around its CPU
//   read: it is what makes the IOSurface's memory current for a reader) and retained from before the copy is recorded
//   until it has completed, then unlocked and released (reaped at the next frames, or when its ring slot is reused).
//   VideoToolbox clients hand a buffer over once they have rendered it and do not write to it while it is retained, so
//   the pool cannot give it back to the client mid-copy.
// - Planes: NV12 / P010, plane offsets and pitches from the IOSurface (bufferRowLength = bytes per row / texel size);
//   a plane offset or pitch the copy engine cannot take (offsets must be multiples of 4 on a transfer-only queue)
//   falls back to the copy path, as does any failure (import, submit); after 8 failures in a row the session stops
//   trying.

static void up_unlock_slot(Upload *u, unsigned slot) {
	if (u->cmd_pb[slot]) {
		CVPixelBufferUnlockBaseAddress(u->cmd_pb[slot], kCVPixelBufferLock_ReadOnly);
		CVPixelBufferRelease(u->cmd_pb[slot]);
		u->cmd_pb[slot] = NULL;
	}
}

static void up_drop_surface(Upload *u, UpSurface *e) {
	u->vk.DestroyBuffer(u->d, e->buf, NULL); u->vk.Free(u->d, e->mem, NULL); CFRelease(e->surf);
	memset(e, 0, sizeof *e);
}

// completed uploads: unlock and release their pixel buffers; drop surfaces only the cache still holds
static void up_reap(Enc *s) {
	Upload *u = &s->up;
	uint64_t done = 0;
	if (u->vk.Counter(u->d, u->done, &done) != VK_SUCCESS) return;
	for (unsigned i = 0; i < UP_RING; i++) if (u->cmd_pb[i] && u->cmd_value[i] <= done) up_unlock_slot(u, i);
	for (int i = 0; i < UP_SURFACES; i++)
		if (u->imp[i].surf && u->imp[i].last_use <= done && CFGetRetainCount(u->imp[i].surf) == 1) { up_drop_surface(u, &u->imp[i]); s->ust.evictions++; }
}

static void up_fini(Enc *s) {
	Upload *u = &s->up;
	if (u->ready) {
		if (u->submitted) {
			VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &u->done, .pValues = &u->submitted };
			u->vk.Wait(u->d, &wi, 5000000000ull);
		}
		for (unsigned i = 0; i < UP_RING; i++) up_unlock_slot(u, i);
		for (int i = 0; i < UP_SURFACES; i++) if (u->imp[i].surf) up_drop_surface(u, &u->imp[i]);
		if (u->pool) u->vk.DestroyPool(u->d, u->pool, NULL);
		if (u->done) u->vk.DestroySem(u->d, u->done, NULL);
	}
	const int mode = u->mode;
	memset(u, 0, sizeof *u);
	u->mode = mode;
}

static int up_init(Enc *s) {
	Upload *u = &s->up;
	if (!g_vk.has_host_import || g_vk.copy_family < 0) return -1;
	u->d = g_vk.dev; u->qfam = (uint32_t)g_vk.copy_family;
	PFN_vkGetDeviceProcAddr gdpa = (PFN_vkGetDeviceProcAddr)g_vk.gipa(g_vk.inst, "vkGetDeviceProcAddr");
	if (!gdpa) return -1;
#define L(f, n) u->vk.f = (void *)gdpa(u->d, n)
	L(Begin, "vkBeginCommandBuffer"); L(End, "vkEndCommandBuffer"); L(Reset, "vkResetCommandBuffer"); L(Barrier, "vkCmdPipelineBarrier2");
	L(Copy, "vkCmdCopyBufferToImage"); L(Submit, "vkQueueSubmit2"); L(Wait, "vkWaitSemaphores"); L(Counter, "vkGetSemaphoreCounterValue");
	L(CreateBuffer, "vkCreateBuffer"); L(DestroyBuffer, "vkDestroyBuffer"); L(Reqs, "vkGetBufferMemoryRequirements");
	L(Alloc, "vkAllocateMemory"); L(Free, "vkFreeMemory"); L(Bind, "vkBindBufferMemory");
	L(HostProps, "vkGetMemoryHostPointerPropertiesEXT"); L(DestroyPool, "vkDestroyCommandPool"); L(DestroySem, "vkDestroySemaphore");
#undef L
	PFN_vkGetDeviceQueue gq = (PFN_vkGetDeviceQueue)gdpa(u->d, "vkGetDeviceQueue");
	PFN_vkCreateCommandPool cp = (PFN_vkCreateCommandPool)gdpa(u->d, "vkCreateCommandPool");
	PFN_vkAllocateCommandBuffers acb = (PFN_vkAllocateCommandBuffers)gdpa(u->d, "vkAllocateCommandBuffers");
	PFN_vkCreateSemaphore cs = (PFN_vkCreateSemaphore)gdpa(u->d, "vkCreateSemaphore");
	if (!gq || !cp || !acb || !cs || !u->vk.HostProps || !u->vk.Submit || !u->vk.Counter || !u->vk.Barrier) return -1;
	gq(u->d, u->qfam, 0, &u->q);
	VkCommandPoolCreateInfo pci = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
	                                .queueFamilyIndex = u->qfam };
	VkCommandBufferAllocateInfo cai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
	                                    .commandBufferCount = UP_RING };
	VkSemaphoreTypeCreateInfo st = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE };
	VkSemaphoreCreateInfo sci = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st };
	u->ready = 1;   // from here up_fini cleans up
	if (cp(u->d, &pci, NULL, &u->pool) != VK_SUCCESS) { u->pool = VK_NULL_HANDLE; return -1; }
	cai.commandPool = u->pool;
	if (acb(u->d, &cai, u->cmd) != VK_SUCCESS || cs(u->d, &sci, NULL, &u->done) != VK_SUCCESS) return -1;
	return 0;
}

// the IOSurface's pages as a transfer-source buffer: imported on first use, kept while the surface is in the pool
static UpSurface *up_import(Enc *s, IOSurfaceRef surf) {
	Upload *u = &s->up;
	const IOSurfaceID id = IOSurfaceGetID(surf);
	UpSurface *e = NULL, *lru = NULL;
	for (int i = 0; i < UP_SURFACES; i++) {
		if (u->imp[i].surf == surf && u->imp[i].id == id) return &u->imp[i];
		if (!u->imp[i].surf) { if (!e) e = &u->imp[i]; }
		else if (!lru || u->imp[i].last_use < lru->last_use) lru = &u->imp[i];
	}
	if (!e) {   // more surfaces than kept: the least recently used one goes, once its last upload is done
		VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &u->done, .pValues = &lru->last_use };
		if (u->vk.Wait(u->d, &wi, 5000000000ull) != VK_SUCCESS) return NULL;
		up_drop_surface(u, lru); s->ust.evictions++;
		e = lru;
	}
	const VkDeviceSize align = g_vk.host_align;
	void *base = IOSurfaceGetBaseAddress(surf);
	VkDeviceSize size = IOSurfaceGetAllocSize(surf);
	if (!base || !size || ((uintptr_t)base % align)) return NULL;
	size = (size + align - 1) / align * align;
	VkMemoryHostPointerPropertiesEXT hpp = { .sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT };
	if (u->vk.HostProps(u->d, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, base, &hpp) != VK_SUCCESS || !hpp.memoryTypeBits) return NULL;
	VkExternalMemoryBufferCreateInfo ebi = { .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO,
	                                         .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT };
	VkBufferCreateInfo bci = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &ebi, .size = size,
	                           .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT, .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
	VkBuffer buf; VkDeviceMemory mem;
	if (u->vk.CreateBuffer(u->d, &bci, NULL, &buf) != VK_SUCCESS) return NULL;
	VkMemoryRequirements mr; u->vk.Reqs(u->d, buf, &mr);
	const uint32_t bits = mr.memoryTypeBits & hpp.memoryTypeBits;
	if (!bits) { u->vk.DestroyBuffer(u->d, buf, NULL); return NULL; }
	VkImportMemoryHostPointerInfoEXT imp = { .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT,
	                                         .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = base };
	VkMemoryAllocateInfo mai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = size,
	                             .memoryTypeIndex = (uint32_t)__builtin_ctz(bits) };
	if (u->vk.Alloc(u->d, &mai, NULL, &mem) != VK_SUCCESS) { u->vk.DestroyBuffer(u->d, buf, NULL); return NULL; }
	if (u->vk.Bind(u->d, buf, mem, 0) != VK_SUCCESS) { u->vk.DestroyBuffer(u->d, buf, NULL); u->vk.Free(u->d, mem, NULL); return NULL; }
	*e = (UpSurface){ id, (IOSurfaceRef)CFRetain(surf), buf, mem, 0 };
	s->ust.imports++;
	return e;
}

// img into s->hw (a frame from s->frames already) by the copy engine; 0 when submitted, < 0: nothing was done, the
// caller uploads through libavutil instead
static int upload_import(Enc *s, CVPixelBufferRef img) {
	Upload *u = &s->up;
	if (u->broken) return -1;
	if (!u->ready && up_init(s) < 0) {
		os_log_error(lg(), "intelxevenc: %{public}s in %{public}s: no copy engine upload (family %d, host import %d) - staging copies",
		             codec_name(s->codec), proc(), g_vk.copy_family, g_vk.has_host_import);
		up_fini(s); u->broken = 1; return -1;
	}
	IOSurfaceRef surf = CVPixelBufferGetIOSurface(img);
	if (!surf || CVPixelBufferGetPlaneCount(img) != 2 || IOSurfaceGetPlaneCount(surf) != 2) return -1;
	up_reap(s);

	AVHWFramesContext *fc = (AVHWFramesContext *)s->hw->hw_frames_ctx->data;
	AVVulkanFramesContext *vfc = fc->hwctx;
	AVVkFrame *vkf = (AVVkFrame *)s->hw->data[0];
	int nimg = 0; while (nimg < AV_NUM_DATA_POINTERS && vkf->img[nimg]) nimg++;
	if (nimg != 1 && nimg != 2) return -1;

	// the regions first: a surface the copy engine cannot read as is goes the CPU way without being imported
	const size_t bps = s->ten_bit ? 2 : 1;
	VkBufferImageCopy rg[2];
	for (int p = 0; p < 2; p++) {
		const size_t texel = (p ? 2 : 1) * bps;
		const size_t off = (size_t)((uint8_t *)IOSurfaceGetBaseAddressOfPlane(surf, p) - (uint8_t *)IOSurfaceGetBaseAddress(surf));
		const size_t bpr = IOSurfaceGetBytesPerRowOfPlane(surf, p);
		const uint32_t pw = (uint32_t)(p ? (s->dim.width + 1) / 2 : s->dim.width), ph = (uint32_t)(p ? (s->dim.height + 1) / 2 : s->dim.height);
		if ((off & 3) || off % texel || bpr % texel || bpr / texel < pw || IOSurfaceGetHeightOfPlane(surf, p) < ph ||
		    off + bpr * (ph - 1) + pw * texel > IOSurfaceGetAllocSize(surf)) return -1;
		rg[p] = (VkBufferImageCopy){ .bufferOffset = off, .bufferRowLength = (uint32_t)(bpr / texel), .bufferImageHeight = ph,
			.imageSubresource = { nimg == 1 ? (p ? VK_IMAGE_ASPECT_PLANE_1_BIT : VK_IMAGE_ASPECT_PLANE_0_BIT) : VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
			.imageOffset = { 0, 0, 0 }, .imageExtent = { pw, ph, 1 } };
	}

	// the ring slot: wait for its last upload if it is still running (back-pressure on the client)
	const unsigned slot = u->next % UP_RING;
	if (u->cmd_pb[slot] || u->cmd_value[slot]) {
		VkSemaphoreWaitInfo wi = { .sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &u->done, .pValues = &u->cmd_value[slot] };
		uint64_t have = 0;
		if (u->vk.Counter(u->d, u->done, &have) == VK_SUCCESS && have < u->cmd_value[slot]) s->ust.waits++;
		if (u->vk.Wait(u->d, &wi, 5000000000ull) != VK_SUCCESS) { u->broken = 1; return -1; }
		up_unlock_slot(u, slot);
	}

	// the CPU must see the surface as current: the same read-only lock as the copy path, held until the copy is done
	if (CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return -1;
	UpSurface *im = up_import(s, surf);
	if (!im) { CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly); return -1; }

	VkCommandBuffer cb = u->cmd[slot];
	u->vk.Reset(cb, 0);
	VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
	u->vk.Begin(cb, &bi);

	vfc->lock_frame(fc, vkf);
	VkImageMemoryBarrier2 ib[2];
	VkSemaphoreSubmitInfo wait[2], sig[3];
	int ok = 1;
	for (int i = 0; i < nimg; i++) {
		// concurrent sharing (more than one family listed): no ownership to move; anything else is not ours to touch
		if (vkf->queue_family[i] != VK_QUEUE_FAMILY_IGNORED) ok = 0;
		ib[i] = (VkImageMemoryBarrier2){ .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2,
			.srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = (VkAccessFlags2)vkf->access[i],
			.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT,
			.oldLayout = vkf->layout[i], .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
			.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = vkf->img[i],
			.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 } };
		wait[i] = (VkSemaphoreSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = vkf->sem[i], .value = vkf->sem_value[i],
		                                   .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT };
		sig[i] = (VkSemaphoreSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = vkf->sem[i], .value = vkf->sem_value[i] + 1,
		                                  .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT };
	}
	VkResult r = VK_ERROR_UNKNOWN;
	if (ok) {
		VkDependencyInfo dep = { .sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = (uint32_t)nimg, .pImageMemoryBarriers = ib };
		u->vk.Barrier(cb, &dep);
		for (int p = 0; p < 2; p++) u->vk.Copy(cb, im->buf, vkf->img[nimg == 1 ? 0 : p], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rg[p]);
		u->vk.End(cb);
		const uint64_t v = u->submitted + 1;
		sig[nimg] = (VkSemaphoreSubmitInfo){ .sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = u->done, .value = v,
		                                     .stageMask = VK_PIPELINE_STAGE_2_COPY_BIT };
		VkCommandBufferSubmitInfo cbi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb };
		VkSubmitInfo2 si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = (uint32_t)nimg, .pWaitSemaphoreInfos = wait,
		                     .commandBufferInfoCount = 1, .pCommandBufferInfos = &cbi,
		                     .signalSemaphoreInfoCount = (uint32_t)nimg + 1, .pSignalSemaphoreInfos = sig };
		pthread_mutex_lock(&g_vk_queue_lock);   // every submit to the shared device's queues (vk_lock_queue)
		r = u->vk.Submit(u->q, 1, &si, VK_NULL_HANDLE);
		pthread_mutex_unlock(&g_vk_queue_lock);
		if (r == VK_SUCCESS) {
			u->submitted = v;
			for (int i = 0; i < nimg; i++) {
				vkf->sem_value[i]++;
				vkf->layout[i] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
				vkf->access[i] = VK_ACCESS_TRANSFER_WRITE_BIT;
			}
			u->cmd_value[slot] = v; u->cmd_pb[slot] = CVPixelBufferRetain(img); u->next++;
			im->last_use = v;
		}
	}
	vfc->unlock_frame(fc, vkf);
	if (r != VK_SUCCESS) {
		CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
		os_log_error(lg(), "intelxevenc: %{public}s copy engine upload in %{public}s: %{public}s %d", codec_name(s->codec), proc(),
		             ok ? "submit" : "frame not shared with the copy family, queue family", ok ? (int)r : (int)vkf->queue_family[0]);
		if (!ok) u->broken = 1;
		return -1;
	}
	return 0;
}

// ---------------------------------------------------------------------------------------------------------------------
// Bitstream rewriting

static const uint8_t *next_sc(const uint8_t *p, const uint8_t *end, int *sclen) {
	for (; p + 3 <= end; p++)
		if (!p[0] && !p[1] && (p[2] == 1 || (p + 4 <= end && !p[2] && p[3] == 1))) { *sclen = p[2] == 1 ? 3 : 4; return p; }
	*sclen = 0; return end;
}
typedef struct { const uint8_t *p; size_t n; } Nal;
static int split_annexb(const uint8_t *b, size_t n, Nal *out, int max) {
	int cnt = 0, sc;
	const uint8_t *end = b + n, *p = next_sc(b, end, &sc);
	while (p < end && cnt < max) {
		const uint8_t *start = p + sc, *q = next_sc(start, end, &sc);
		size_t len = q - start;
		while (len && !start[len - 1]) len--;   // trailing zero bytes belong to the next start code
		if (len) out[cnt++] = (Nal){ start, len };
		p = q;
	}
	return cnt;
}
static int nal_type(FourCharCode codec, const uint8_t *p) { return codec == 'hvc1' ? (p[0] >> 1) & 0x3f : p[0] & 0x1f; }
static int is_param_set(FourCharCode codec, int t) { return codec == 'hvc1' ? (t >= 32 && t <= 34) : (t == 7 || t == 8); }
static int is_aud(FourCharCode codec, int t) { return codec == 'hvc1' ? t == 35 : t == 9; }

// AV1: walk the OBUs of a temporal unit (low overhead format, every OBU carries its size)
static size_t leb128(const uint8_t *p, const uint8_t *end, uint64_t *v) {
	*v = 0;
	for (size_t i = 0; i < 8 && p + i < end; i++) { *v |= (uint64_t)(p[i] & 0x7f) << (7 * i); if (!(p[i] & 0x80)) return i + 1; }
	return 0;
}

static OSStatus make_format_description(Enc *s, const uint8_t *pkt, size_t pktn) {
	const uint8_t *hdr = s->av->extradata; size_t hn = s->av->extradata_size;
	if (!hdr || !hn) { hdr = pkt; hn = pktn; }   // in-band only: take them from the first key frame
	OSStatus r;
	CFMutableDictionaryRef ext = CFDictionaryCreateMutable(NULL, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
	if (s->primaries) CFDictionarySetValue(ext, kCMFormatDescriptionExtension_ColorPrimaries, s->primaries);
	if (s->transfer) CFDictionarySetValue(ext, kCMFormatDescriptionExtension_TransferFunction, s->transfer);
	if (s->matrix) CFDictionarySetValue(ext, kCMFormatDescriptionExtension_YCbCrMatrix, s->matrix);
	if (isfull(s->cv_format)) CFDictionarySetValue(ext, kCMFormatDescriptionExtension_FullRangeVideo, kCFBooleanTrue);
	if (s->codec == 'av01') {
		// av1C: marker/version, seq_profile/level, tier/bitdepth/chroma, then the sequence header OBU
		const uint8_t *p = hdr, *end = hdr + hn, *seq = NULL; size_t seqn = 0;
		while (p < end) {
			int type = (p[0] >> 3) & 0xf, hasext = (p[0] >> 2) & 1, hassize = (p[0] >> 1) & 1;
			size_t h = 1 + hasext; uint64_t sz = 0; size_t l = 0;
			if (hassize) { l = leb128(p + h, end, &sz); if (!l) break; } else sz = end - p - h;
			if (type == 1) { seq = p; seqn = h + l + sz; break; }
			p += h + l + sz;
		}
		if (!seq) { CFRelease(ext); return kVTVideoEncoderMalfunctionErr; }
		uint8_t c[4 + 512]; size_t cn = 4;
		const uint8_t *sh = seq + 1 + ((seq[0] >> 2) & 1); uint64_t shsz; sh += leb128(sh, seq + seqn, &shsz);
		int profile = sh[0] >> 5, level = 0, tier = 0;
		// seq_profile(3) still_picture(1) reduced_still_picture_header(1); operating point 0: idc(12) level(5) tier(1)
		if (!((sh[0] >> 3) & 1)) {
			// timing_info_present_flag is the next bit; ffmpeg's Vulkan AV1 encoder writes none, so the operating points follow
			uint32_t bits = (uint32_t)sh[0] << 24 | (uint32_t)sh[1] << 16 | (uint32_t)sh[2] << 8 | sh[3];
			// bit 26 = timing_info_present, 25 = initial_display_delay_present, 24..20 = operating_points_cnt_minus_1,
			// 19..8 = operating_point_idc[0], 7..3 = seq_level_idx[0], 2 = seq_tier[0] (when level > 7)
			level = (bits >> 3) & 0x1f; tier = level > 7 ? (bits >> 2) & 1 : 0;
		} else level = (sh[0] & 7) << 2 | sh[1] >> 6;
		c[0] = 0x81; c[1] = (uint8_t)(profile << 5 | level);
		c[2] = (uint8_t)(tier << 7 | (s->ten_bit ? 1 : 0) << 6 | 0 << 5 | 0 << 4 | 1 << 3 | 1 << 2 | 0);
		c[3] = 0;
		if (seqn > 512) seqn = 512;
		memcpy(c + 4, seq, seqn); cn += seqn;
		CFDataRef d = CFDataCreate(NULL, c, cn);
		const void *ak[] = { CFSTR("av1C") }, *av[] = { d };
		CFDictionaryRef atoms = CFDictionaryCreate(NULL, ak, av, 1, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_SampleDescriptionExtensionAtoms, atoms);
		CFNumberRef depth = i32(s->ten_bit ? 30 : 24);
		CFDictionarySetValue(ext, kCMFormatDescriptionExtension_Depth, depth);
		r = CMVideoFormatDescriptionCreate(NULL, 'av01', s->dim.width, s->dim.height, ext, &s->fd);
		CFRelease(depth); CFRelease(atoms); CFRelease(d);
	} else {
		Nal nal[64]; int n = split_annexb(hdr, hn, nal, 64);
		const uint8_t *ps[8]; size_t psn[8]; int np = 0;
		// hvcC wants VPS, SPS, PPS; avcC SPS, PPS
		for (int want = 0; want < 3; want++)
			for (int i = 0; i < n && np < 8; i++) {
				int t = nal_type(s->codec, nal[i].p);
				int match = s->codec == 'hvc1' ? t == 32 + want : (want < 2 && t == (want ? 8 : 7));
				if (match) { ps[np] = nal[i].p; psn[np] = nal[i].n; np++; }
			}
		if (s->codec == 'hvc1')
			r = CMVideoFormatDescriptionCreateFromHEVCParameterSets(NULL, np, ps, psn, 4, ext, &s->fd);
		else
			r = CMVideoFormatDescriptionCreateFromH264ParameterSets(NULL, np, ps, psn, 4, &s->fd);
		if (!r && s->codec == 'avc1' && CFDictionaryGetCount(ext)) {
			// the H.264 call takes no extensions: remake it with the parameter sets' atoms plus the color tags
			CFDictionaryRef have = CMFormatDescriptionGetExtensions(s->fd);
			CFMutableDictionaryRef all = CFDictionaryCreateMutableCopy(NULL, 0, have);
			CFIndex ne = CFDictionaryGetCount(ext); const void *ek[8], *ev[8];
			if (ne > 8) ne = 8;
			CFDictionaryGetKeysAndValues(ext, ek, ev);
			for (CFIndex i = 0; i < ne; i++) CFDictionarySetValue(all, ek[i], ev[i]);
			CMVideoFormatDescriptionRef fd2 = NULL;
			if (!CMVideoFormatDescriptionCreate(NULL, 'avc1', s->dim.width, s->dim.height, all, &fd2)) { CFRelease(s->fd); s->fd = fd2; }
			CFRelease(all);
		}
	}
	CFRelease(ext);
	return r;
}

// one packet out: rewrite, wrap, emit for its frame token
static void emit_packet(Enc *s, AVPacket *pkt) {
	// the pending frame this packet belongs to (packet pts = the frame's key)
	unsigned idx = s->npend;
	for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].key == pkt->pts) { idx = i; break; }
	if (idx == s->npend) { os_log_error(lg(), "intelxevenc: packet pts %lld has no pending frame", pkt->pts); s->errors++; return; }
	Pending pf = s->pend[idx];
	memmove(&s->pend[idx], &s->pend[idx + 1], (s->npend - idx - 1) * sizeof s->pend[0]);
	s->npend--;

	OSStatus r = 0;
	const int key = !!(pkt->flags & AV_PKT_FLAG_KEY);
	if (!s->fd) r = make_format_description(s, pkt->data, pkt->size);

	// payload: NAL units with 4-byte big endian lengths, parameter sets and AUDs dropped (they live in the format
	// description); AV1 keeps its OBUs minus the temporal delimiter
	uint8_t *buf = r ? NULL : malloc(pkt->size + 64);
	size_t bn = 0;
	if (buf && s->codec == 'av01') {
		const uint8_t *p = pkt->data, *end = pkt->data + pkt->size;
		while (p < end) {
			int type = (p[0] >> 3) & 0xf, hasext = (p[0] >> 2) & 1, hassize = (p[0] >> 1) & 1;
			size_t h = 1 + hasext, l = 0; uint64_t sz;
			if (hassize) { l = leb128(p + h, end, &sz); if (!l) break; } else sz = end - p - h;
			size_t total = h + l + sz;
			if (p + total > end) break;
			if (type != 2) { memcpy(buf + bn, p, total); bn += total; }
			p += total;
		}
	} else if (buf) {
		Nal nal[256]; int n = split_annexb(pkt->data, pkt->size, nal, 256);
		for (int i = 0; i < n; i++) {
			int t = nal_type(s->codec, nal[i].p);
			if (is_param_set(s->codec, t) || is_aud(s->codec, t)) continue;
			buf[bn++] = nal[i].n >> 24; buf[bn++] = nal[i].n >> 16; buf[bn++] = nal[i].n >> 8; buf[bn++] = nal[i].n;
			memcpy(buf + bn, nal[i].p, nal[i].n); bn += nal[i].n;
		}
	}
	CMSampleBufferRef sb = NULL;
	if (!r && buf && bn) {
		CMBlockBufferRef bb = NULL;
		r = CMBlockBufferCreateWithMemoryBlock(NULL, buf, bn, kCFAllocatorMalloc, NULL, 0, bn, 0, &bb);
		if (!r) {
			buf = NULL;   // owned by the block buffer now
			CMSampleTimingInfo ti = { .duration = pf.dur, .presentationTimeStamp = pf.pts,
			                          .decodeTimeStamp = pkt->dts == AV_NOPTS_VALUE ? kCMTimeInvalid : CMTimeMake(pkt->dts, s->timescale) };
			if (s->av->max_b_frames == 0) ti.decodeTimeStamp = kCMTimeInvalid;   // decode order = presentation order
			size_t sizes[1] = { bn };
			r = CMSampleBufferCreateReady(NULL, bb, s->fd, 1, 1, &ti, 1, sizes, &sb);
			CFRelease(bb);
		}
		if (sb) {
			CFArrayRef att = CMSampleBufferGetSampleAttachmentsArray(sb, true);
			CFMutableDictionaryRef a = att && CFArrayGetCount(att) ? (CFMutableDictionaryRef)CFArrayGetValueAtIndex(att, 0) : NULL;
			if (a) {
				CFDictionarySetValue(a, kCMSampleAttachmentKey_NotSync, key ? kCFBooleanFalse : kCFBooleanTrue);
				CFDictionarySetValue(a, kCMSampleAttachmentKey_DependsOnOthers, key ? kCFBooleanFalse : kCFBooleanTrue);
			}
		}
	} else if (!r) r = kVTVideoEncoderMalfunctionErr;
	free(buf);
	if (r) s->errors++; else { s->frames_out++; s->bytes_out += bn; }
	VTEncoderSessionEmitEncodedFrame(s->session, pf.frame, r, 0, r ? NULL : sb);
	if (sb) CFRelease(sb);
}

static int drain(Enc *s) {
	int err;
	while ((err = avcodec_receive_packet(s->av, s->pkt)) == 0) { emit_packet(s, s->pkt); av_packet_unref(s->pkt); }
	return err == AVERROR(EAGAIN) || err == AVERROR_EOF ? 0 : err;
}

// ---------------------------------------------------------------------------------------------------------------------
// Encoding

static OSStatus encode_frame(Enc *s, VTEncoderFrameRef frame, CVImageBufferRef img, CMTime pts, CMTime dur,
                             CFDictionaryRef props, VTEncodeInfoFlags *info) {
	if (info) *info = 0;
	pthread_mutex_lock(&s->lock);
	OSStatus r = 0;
	const int32_t f = CVPixelBufferGetPixelFormatType(img);
	if (!s->av && !s->open_failed) {
		int err = open_codec(s, f, pts, dur);
		if (err < 0) {
			char m[128]; av_strerror(err, m, sizeof m);
			os_log_error(lg(), "intelxevenc: %{public}s encoder in %{public}s did not open: %{public}s", codec_name(s->codec), proc(), m);
			close_codec(s); s->open_failed = 1;
		}
		s->t_start_ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	}
	if (!s->av) r = kVTVideoEncoderNotAvailableNowErr;
	else if (f != s->cv_format || (int)CVPixelBufferGetWidth(img) != s->dim.width || (int)CVPixelBufferGetHeight(img) != s->dim.height)
		r = kVTPixelTransferNotSupportedErr;
	else if (s->npend == MAX_PENDING) r = kVTVideoEncoderMalfunctionErr;
	if (r) { s->errors++; pthread_mutex_unlock(&s->lock); VTEncoderSessionEmitEncodedFrame(s->session, frame, r, 0, NULL); return 0; }

	// upload: the copy engine reads the IOSurface (INTELXEVENC_UPLOAD=import), or the pixel buffer's planes go straight
	// into libavutil's staging copy
	const uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	int err = av_hwframe_get_buffer(s->frames, s->hw, 0);
	int imported = 0;
	if (err >= 0 && s->up.mode) {
		imported = upload_import(s, img) == 0;
		if (imported) { s->ust.imported++; s->up.fail_run = 0; }
		else {
			s->ust.fallback++;
			if (++s->up.fail_run == 8 && !s->up.broken) {
				s->up.broken = 1;
				os_log_error(lg(), "intelxevenc: %{public}s in %{public}s: 8 copy engine uploads failed in a row - staging copies from here", codec_name(s->codec), proc());
			}
		}
		// a client that hands over a new IOSurface for (nearly) every frame instead of recycling a pool's: an import
		// per frame costs more than the CPU copy (11 Nov), so this session goes back to it
		if (!s->up.broken && s->ust.imported == 48 && s->ust.imports > 24) {
			s->up.broken = 1;
			os_log(lg(), "intelxevenc: %{public}s in %{public}s: %lld surfaces in 48 frames (not recycled) - staging copies from here",
			       codec_name(s->codec), proc(), s->ust.imports);
		}
	}
	if (err >= 0 && !imported) {
		CVPixelBufferLockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
		s->sw->format = s->ten_bit ? AV_PIX_FMT_P010 : AV_PIX_FMT_NV12;
		s->sw->width = s->dim.width; s->sw->height = s->dim.height;
		for (int p = 0; p < 2; p++) {
			s->sw->data[p] = CVPixelBufferGetBaseAddressOfPlane(img, p);
			s->sw->linesize[p] = (int)CVPixelBufferGetBytesPerRowOfPlane(img, p);
		}
		err = av_hwframe_transfer_data(s->hw, s->sw, 0);
		CVPixelBufferUnlockBaseAddress(img, kCVPixelBufferLock_ReadOnly);
		memset(s->sw->data, 0, sizeof s->sw->data);
	}
	const uint64_t t1 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
	s->t_upload_ns += t1 - t0;

	if (err >= 0) {
		// the frame's key: its pts in the stream timebase, unique and increasing (VideoToolbox requires increasing pts)
		int64_t k = CMTimeConvertScale(pts, s->timescale, kCMTimeRoundingMethod_Default).value;
		if (s->npend && k <= s->pend[s->npend - 1].key) k = s->pend[s->npend - 1].key + 1;
		int force_key = props && CFDictionaryGetValue(props, kVTEncodeFrameOptionKey_ForceKeyFrame) == kCFBooleanTrue;
		s->pend[s->npend++] = (Pending){ k, frame, pts, dur, force_key };
		s->hw->pts = k;
		s->hw->duration = CMTIME_IS_NUMERIC(dur) ? CMTimeConvertScale(dur, s->timescale, kCMTimeRoundingMethod_Default).value : 0;
		s->hw->pict_type = force_key ? AV_PICTURE_TYPE_I : AV_PICTURE_TYPE_NONE;
		if (force_key) s->hw->flags |= AV_FRAME_FLAG_KEY;
		s->frames_in++;
		err = avcodec_send_frame(s->av, s->hw);
		if (err == AVERROR(EAGAIN)) { drain(s); err = avcodec_send_frame(s->av, s->hw); }
		if (err < 0) {
			// take the token back out and fail it
			for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].frame == frame) { memmove(&s->pend[i], &s->pend[i + 1], (--s->npend - i) * sizeof s->pend[0]); break; }
		} else err = drain(s);
	}
	av_frame_unref(s->hw);
	s->t_encode_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t1;
	if (err < 0) {
		char m[128]; av_strerror(err, m, sizeof m);
		os_log_error(lg(), "intelxevenc: %{public}s frame %lld in %{public}s: %{public}s", codec_name(s->codec), s->frames_in, proc(), m);
		s->errors++;
		int still_pending = 0;
		for (unsigned i = 0; i < s->npend; i++) if (s->pend[i].frame == frame) still_pending = 1;
		pthread_mutex_unlock(&s->lock);
		if (!still_pending) VTEncoderSessionEmitEncodedFrame(s->session, frame, kVTVideoEncoderMalfunctionErr, 0, NULL);
		return 0;
	}
	if (info) *info |= kVTEncodeInfo_Asynchronous;
	pthread_mutex_unlock(&s->lock);
	return 0;
}

// the held first frame goes in (the codec opens with whatever rate is known by then)
static void release_held(Enc *s, int encode) {
	if (!s->held.frame) return;
	VTEncoderFrameRef f = s->held.frame; CVImageBufferRef img = s->held.img; CFDictionaryRef props = s->held.props;
	CMTime pts = s->held.pts, dur = s->held.dur;
	memset(&s->held, 0, sizeof s->held);
	if (encode) encode_frame(s, f, img, pts, dur, props, NULL);
	else VTEncoderSessionEmitEncodedFrame(s->session, f, 0, kVTEncodeInfo_FrameDropped, NULL);
	CVBufferRelease(img);
	if (props) CFRelease(props);
}

static OSStatus EncodeFrame(VTVideoEncoderRef e, VTEncoderFrameRef frame, CVImageBufferRef img, CMTime pts, CMTime dur,
                            CFDictionaryRef props, VTEncodeInfoFlags *info) {
	Enc *s = enc_of(e);
	pthread_mutex_lock(&s->lock);
	const int unknown_rate = !s->av && !s->open_failed && !(s->expected_fps > 0) && !(CMTIME_IS_NUMERIC(dur) && dur.value > 0) &&
	                         !(s->est_fps > 0) && CMTIME_IS_NUMERIC(pts);
	if (unknown_rate && !s->held.frame) {
		// hold it: VideoToolbox lets an encoder keep the image buffer until it emits the frame
		s->held.frame = frame; s->held.img = CVBufferRetain(img); s->held.pts = pts; s->held.dur = dur;
		s->held.props = props ? CFRetain(props) : NULL;
		pthread_mutex_unlock(&s->lock);
		if (info) *info = kVTEncodeInfo_Asynchronous;
		return 0;
	}
	if (s->held.frame) {
		const double dt = CMTimeGetSeconds(CMTimeSubtract(pts, s->held.pts));
		s->est_fps = dt > 1.0 / 1000 && dt < 2 ? 1 / dt : 30.0;
		os_log(lg(), "intelxevenc: %{public}s in %{public}s: no frame rate or durations given - %.3f fps from the first timestamps",
		       codec_name(s->codec), proc(), s->est_fps);
	}
	pthread_mutex_unlock(&s->lock);
	release_held(s, 1);
	return encode_frame(s, frame, img, pts, dur, props, info);
}

// flush: everything sent so far comes out (libavcodec's encoders flush only at end of stream, so the encoder is drained
// and reopened for the frames after; VideoToolbox clients call this at the end or before a seek-like discontinuity)
static OSStatus CompleteFrames(VTVideoEncoderRef e, CMTime until) {
	Enc *s = enc_of(e);
	release_held(s, 1);   // a lone frame (FFmpeg's extradata probe): the default rate is as good as any
	pthread_mutex_lock(&s->lock);
	if (s->av && s->npend) {
		avcodec_send_frame(s->av, NULL);
		drain(s);
		drop_pending(s, 0);
		// reopen lazily at the next frame with the same settings
		int64_t fi = s->frames_in, fo = s->frames_out, bo = s->bytes_out, er = s->errors;
		uint64_t tu = s->t_upload_ns, te = s->t_encode_ns, ts = s->t_start_ns;
		UpStats us = s->ust;
		s->frames_in = 0;   // no close log for this intermediate flush
		close_codec(s);
		s->frames_in = fi; s->frames_out = fo; s->bytes_out = bo; s->errors = er; s->t_upload_ns = tu; s->t_encode_ns = te; s->t_start_ns = ts;
		s->ust = us;
	}
	pthread_mutex_unlock(&s->lock);
	return 0;
}

static const struct __attribute__((aligned(8))) { uint32_t pad; CMBaseClass c; } kBase = { 0, { 1, sizeof(Enc), NULL, Invalidate, Finalize,
    CopyDebugDescription, CopyProperty, SetProperty, NULL, NULL } };
static const struct __attribute__((aligned(8))) { uint32_t pad; VTVideoEncoderClass c; } kEnc = { 0, { 1, StartSession, EncodeFrame,
    CompleteFrames, CopySupportedPropertyDictionary, { 0 } } };
static const VTVideoEncoderVTable kVT = { 0, &kBase.c, &kEnc.c };

static OSStatus create(FourCharCode codec, CFAllocatorRef a, VTVideoEncoderRef *out) {
	if (!out) return kVTParameterErr;
	CMBaseObjectRef o = NULL;
	OSStatus r = CMDerivedObjectCreate(a, &kVT, VTVideoEncoderGetClassID(), &o);
	if (r || !o) return r ? r : kVTAllocationFailedErr;
	Enc *s = CMBaseObjectGetDerivedStorage(o);
	memset(s, 0, sizeof *s);
	// recursive: VideoToolbox may run the client's output callback on this thread from inside EncodeFrame, and the
	// callback may set a property or finish frames on the same session
	pthread_mutexattr_t ma;
	pthread_mutexattr_init(&ma);
	pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&s->lock, &ma);
	pthread_mutexattr_destroy(&ma);
	s->codec = codec;
	s->quality = -1;
	s->allow_reorder = codec != 'av01';   // VideoToolbox's default: reordering allowed (AV1: ffmpeg's encoder has no B-frames here)
	s->allow_temporal = 1;
	*out = (VTVideoEncoderRef)o;
	return 0;
}
__attribute__((visibility("default"))) OSStatus IntelXeHEVC_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoEncoderRef *out) { return create('hvc1', a, out); }
__attribute__((visibility("default"))) OSStatus IntelXeH264_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoEncoderRef *out) { return create('avc1', a, out); }
__attribute__((visibility("default"))) OSStatus IntelXeAV1_CreateInstance(FourCharCode c, CFAllocatorRef a, VTVideoEncoderRef *out) { return create('av01', a, out); }

// The card's name for the encoders' names: IntelXe.kext's "model" property ("Intel Arc B570 Graphics") at or above
// NMXAccel, without " Graphics", as the GPU plug-in names its codecs (nvmtl_registry_model, nvmtl_codec_name).
// "Intel Arc B580" when the registry has none.
static void gpu_name(char *out, size_t n) {
	strlcpy(out, "Intel Arc B580", n);
	io_service_t sv = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("NMXAccel"));
	if (!sv) return;
	CFTypeRef m = IORegistryEntrySearchCFProperty(sv, kIOServicePlane, CFSTR("model"), kCFAllocatorDefault,
	                                              kIORegistryIterateRecursively | kIORegistryIterateParents);
	IOObjectRelease(sv);
	char b[128] = "";
	if (m && CFGetTypeID(m) == CFDataGetTypeID()) {
		CFIndex len = 0; const CFIndex max = CFDataGetLength(m); const UInt8 *p = CFDataGetBytePtr(m);
		while (len < max && len < (CFIndex)sizeof b - 1 && p[len]) { b[len] = (char)p[len]; len++; }
		b[len] = 0;
	} else if (m && CFGetTypeID(m) == CFStringGetTypeID() && !CFStringGetCString(m, b, sizeof b, kCFStringEncodingUTF8)) b[0] = 0;
	if (m) CFRelease(m);
	size_t len = strlen(b);
	while (len && (b[len - 1] == ' ' || b[len - 1] == '\n' || b[len - 1] == '\t')) b[--len] = 0;
	if (len > 9 && !strcmp(b + len - 9, " Graphics")) b[len -= 9] = 0;
	const char *t = b; while (*t == ' ') t++;
	if (*t && !strcasestr(t, "unknown")) strlcpy(out, t, n);
}

// The statistics line of the last encoder closed in this process (codec, size, upload mode, frames, fps, CPU ms per
// frame for the upload and for the encode call, copy engine uploads, fallbacks, imports, evictions); "" before any.
// For test tools that load this library themselves (vtencq --lib).
__attribute__((visibility("default"))) int IntelXeVEnc_LastStats(char *out, size_t n) {
	if (!out || !n) return 0;
	pthread_mutex_lock(&g_stats_lock); strlcpy(out, g_stats, n); pthread_mutex_unlock(&g_stats_lock);
	return (int)strlen(out);
}

// Registers the encoders the device can run (one call per process; returns how many were registered). Called by the GPU
// plug-in once VideoToolbox is loaded in the process, or by a test tool.
__attribute__((visibility("default"))) int IntelXeVEnc_Register(void) {
	static int count = -1; static dispatch_once_t once;
	dispatch_once(&once, ^{
		count = 0;
		const char *e = getenv("INTELXEVENC_CODECS");   // e.g. "hevc,h264"; default: HEVC, H.264 and AV1
		struct { FourCharCode t; const char *name, *id, *codec; OSStatus (*fac)(FourCharCode, CFAllocatorRef, VTVideoEncoderRef *); } enc[] = {
			{ 'hvc1', "hevc", "io.github.yusufklncc.intelxe.videoencoder.hevc", "HEVC", IntelXeHEVC_CreateInstance },
			{ 'avc1', "h264", "io.github.yusufklncc.intelxe.videoencoder.h264", "H.264", IntelXeH264_CreateInstance },
			{ 'av01', "av1", "io.github.yusufklncc.intelxe.videoencoder.av1", "AV1", IntelXeAV1_CreateInstance },
		};
		char gpu[128];
		gpu_name(gpu, sizeof gpu);
		for (size_t i = 0; i < sizeof enc / sizeof *enc; i++) {
			int on = e ? strstr(e, enc[i].name) != NULL : 1;
			if (!on) continue;
			char name[160];
			snprintf(name, sizeof name, "%s %s", gpu, enc[i].codec);   // "Intel Arc B580 HEVC"
			CFStringRef id = CFStringCreateWithCString(NULL, enc[i].id, kCFStringEncodingUTF8);
			CFStringRef disp = CFStringCreateWithCString(NULL, name, kCFStringEncodingUTF8);
			CFNumberRef rating = i32(1000);
			// VideoToolbox lists a dynamically registered encoder as "Dynamically Registered <codec> Video Encoder" whatever
			// name it is given; the ID is what clients select it by
			const void *k[] = { CFSTR("CMClassImplementationID"), CFSTR("CMClassImplementationName"), CFSTR("VTRating"),
			                    CFSTR("VTIsHardwareAccelerated") };
			const void *v[] = { id, disp, rating, kCFBooleanTrue };
			CFDictionaryRef info = CFDictionaryCreate(NULL, k, v, 4, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
			OSStatus r = VTRegisterVideoEncoderWithInfo(enc[i].t, info, enc[i].fac);
			os_log(lg(), "intelxevenc: %{public}s encoder %{public}s in %{public}s (VTRegisterVideoEncoderWithInfo -> %d)", name,
			       r ? "NOT registered" : "registered", proc(), (int)r);
			if (!r) count++;
			CFRelease(info); CFRelease(rating); CFRelease(disp); CFRelease(id);
		}
	});
	return count;
}
