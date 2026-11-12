// SPDX-License-Identifier: MIT
// vksurf: a VK_EXT_metal_surface swapchain in an AppKit window, set up the way Wine's macOS driver sets it up
// (winemac.drv cocoa_window.m: a layer-backed view whose backing layer is a CAMetalLayer with framebufferOnly = YES,
// the device MTLCreateSystemDefaultDevice(), nearest magnification, opaque). Clears each swapchain image to a colour
// that changes every frame, presents N frames, resizes the window halfway (the swapchain must report SUBOPTIMAL or
// OUT_OF_DATE and be recreated, as Wine does), then reads the centre pixel of the IOSurface that ANV's WSI shows
// (the sublayer "org.freedesktop.mesa.wsi.iosurface") and compares it with the last colour. Prints timings and
// PASS/FAIL. Test only.
//
//   vksurf [how to load Vulkan] [options]
//     --icd PATH        an ICD, through vk_icdGetInstanceProcAddr (default: /Library/GPUBundles/nvmtl/libvulkan_intel.dylib)
//     --lib PATH|NAME   a library exporting vkGetInstanceProcAddr, opened as Wine opens it (dlopen of the name: a bare
//                       name such as libMoltenVK.dylib goes through DYLD_LIBRARY_PATH); the provider, or the loader
//     --wine10          with --lib: also resolve vkCreateMetalSurfaceEXT and vkCreateMacOSSurfaceMVK with dlsym, as
//                       Wine 10's winemac.drv does (it refuses Vulkan when either is missing), and create the surface
//                       through the dlsym'd vkCreateMetalSurfaceEXT
//     --mvk             create the surface with vkCreateMacOSSurfaceMVK (the provider's alias) instead
//     --frames N        default 300;  --mailbox / --immediate  present mode (default FIFO)
//     --no-mtl-device   leave the layer's device unset (Wine sets it, which loads the Metal driver in the process)
//     --no-resize       keep the window size;  --size WxH  default 800x600
// Exit status 0 on PASS.
// Build: vulkan/build.sh (or clang -arch x86_64 -fobjc-arc -I<vulkan headers> vksurf.m -framework AppKit
//        -framework QuartzCore -framework Metal -framework IOSurface -o vksurf)
#include <dlfcn.h>
#include <mach/mach_time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#import <AppKit/AppKit.h>
#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>
#define VK_NO_PROTOTYPES
#define VK_USE_PLATFORM_METAL_EXT
#define VK_USE_PLATFORM_MACOS_MVK
#include <vulkan/vulkan.h>

static PFN_vkGetInstanceProcAddr gipa;
static PFN_vkGetDeviceProcAddr gdpa;
static VkInstance inst;
static VkPhysicalDevice pd;
static VkDevice dev;
static VkQueue queue;
static uint32_t qfam;
static VkSurfaceKHR surf;
static VkSwapchainKHR sc;
static VkFormat scfmt;
static VkExtent2D ext;
static uint32_t nimg;
static VkImage imgs[16];
static VkSemaphore done[16];
static VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
static int fails;

#define IF(n) PFN_##n n = (PFN_##n)gipa(inst, #n)
#define DF(n) PFN_##n n = (PFN_##n)gdpa(dev, #n)
#define CHECK(x) do { VkResult r_ = (x); if (r_ < 0) { printf("FAIL %s = %d (line %d)\n", #x, r_, __LINE__); exit(1); } } while (0)

static double now_ms(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return (double)mach_absolute_time() * tb.numer / tb.denom / 1e6;
}

static void pump(void) {
    NSEvent *e;
    while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:[NSDate distantPast] inMode:NSDefaultRunLoopMode dequeue:YES]))
        [NSApp sendEvent:e];
}

// Wine's WineMetalView (dlls/winemac.drv/cocoa_window.m, makeBackingLayer)
@interface WineLikeView : NSView
@property(strong) id<MTLDevice> mtl;
@end
@implementation WineLikeView
- (CALayer *)makeBackingLayer {
    CAMetalLayer *l = [CAMetalLayer layer];
    l.device = self.mtl;
    l.framebufferOnly = YES;
    l.magnificationFilter = kCAFilterNearest;
    l.backgroundColor = CGColorGetConstantColor(kCGColorBlack);
    l.contentsScale = 1.0;
    return l;
}
- (BOOL)isOpaque { return YES; }
@end

static void make_swapchain(void) {
    IF(vkGetPhysicalDeviceSurfaceCapabilitiesKHR);
    IF(vkGetPhysicalDeviceSurfaceFormatsKHR);
    IF(vkGetPhysicalDeviceSurfacePresentModesKHR);
    DF(vkCreateSwapchainKHR);
    DF(vkDestroySwapchainKHR);
    DF(vkGetSwapchainImagesKHR);
    VkSurfaceCapabilitiesKHR caps;
    CHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, surf, &caps));
    uint32_t nf = 0;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, NULL));
    VkSurfaceFormatKHR fmts[64];
    if (nf > 64) nf = 64;
    CHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(pd, surf, &nf, fmts));
    VkSurfaceFormatKHR f = fmts[0];
    for (uint32_t i = 0; i < nf; i++)   // DXVK's usual choice
        if (fmts[i].format == VK_FORMAT_B8G8R8A8_UNORM && fmts[i].colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) f = fmts[i];
    uint32_t np = 0;
    VkPresentModeKHR pms[16];
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &np, NULL));
    if (np > 16) np = 16;
    CHECK(vkGetPhysicalDeviceSurfacePresentModesKHR(pd, surf, &np, pms));
    int ok = 0;
    for (uint32_t i = 0; i < np; i++) ok |= pms[i] == mode;
    if (!ok) { printf("present mode %d not offered, using FIFO\n", mode); mode = VK_PRESENT_MODE_FIFO_KHR; }
    if (!(caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_DST_BIT)) { printf("FAIL swapchain images cannot be transfer destinations\n"); exit(1); }
    ext = caps.currentExtent;
    if (ext.width == 0xffffffff) ext = (VkExtent2D){800, 600};
    uint32_t want = caps.minImageCount + 1;
    if (caps.maxImageCount && want > caps.maxImageCount) want = caps.maxImageCount;
    VkSwapchainKHR old = sc;
    VkSwapchainCreateInfoKHR ci = {VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, NULL, 0, surf, want, f.format, f.colorSpace, ext, 1,
        VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, VK_SHARING_MODE_EXCLUSIVE, 0, NULL,
        caps.currentTransform, VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, mode, VK_TRUE, old};
    if (!(caps.supportedCompositeAlpha & VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR)) ci.compositeAlpha = VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR;
    double t = now_ms();
    CHECK(vkCreateSwapchainKHR(dev, &ci, NULL, &sc));
    double t2 = now_ms();
    if (old) vkDestroySwapchainKHR(dev, old, NULL);
    scfmt = f.format;
    nimg = 16;
    CHECK(vkGetSwapchainImagesKHR(dev, sc, &nimg, imgs));
    printf("swapchain %ux%u format %d colorspace %d, %u images (min %u), present mode %d, created in %.2f ms%s\n",
           ext.width, ext.height, scfmt, f.colorSpace, nimg, caps.minImageCount, mode, t2 - t, old ? " (recreated)" : "");
}

int main(int argc, char **argv) {
    @autoreleasepool {
    const char *icd = "/Library/GPUBundles/nvmtl/libvulkan_intel.dylib", *lib = NULL;
    int frames = 300, wine10 = 0, mvk = 0, mtldev = 1, resize = 1, W = 800, H = 600;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--icd") && i + 1 < argc) icd = argv[++i];
        else if (!strcmp(argv[i], "--lib") && i + 1 < argc) lib = argv[++i];
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) frames = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--wine10")) wine10 = 1;
        else if (!strcmp(argv[i], "--mvk")) mvk = 1;
        else if (!strcmp(argv[i], "--mailbox")) mode = VK_PRESENT_MODE_MAILBOX_KHR;
        else if (!strcmp(argv[i], "--immediate")) mode = VK_PRESENT_MODE_IMMEDIATE_KHR;
        else if (!strcmp(argv[i], "--no-mtl-device")) mtldev = 0;
        else if (!strcmp(argv[i], "--no-resize")) resize = 0;
        else if (!strcmp(argv[i], "--size") && i + 1 < argc) sscanf(argv[++i], "%dx%d", &W, &H);
        else { printf("usage: see the comment at the top of vksurf.m\n"); return 2; }
    }
    if (frames < 4) frames = 4;
    PFN_vkCreateMetalSurfaceEXT dl_metal = NULL;
    void *h;
    if (lib) {
        h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("FAIL dlopen %s: %s\n", lib, dlerror()); return 1; }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vkGetInstanceProcAddr");
        PFN_vkGetDeviceProcAddr g = (PFN_vkGetDeviceProcAddr)dlsym(h, "vkGetDeviceProcAddr");
        if (!gipa || !g) { printf("FAIL %s lacks vkGetInstanceProcAddr/vkGetDeviceProcAddr (Wine needs both)\n", lib); return 1; }
        dl_metal = (PFN_vkCreateMetalSurfaceEXT)dlsym(h, "vkCreateMetalSurfaceEXT");
        void *dl_mvk = dlsym(h, "vkCreateMacOSSurfaceMVK");
        printf("library %s: vkCreateMetalSurfaceEXT %s, vkCreateMacOSSurfaceMVK %s (Wine 11 needs the first, Wine 10 both)\n",
               lib, dl_metal ? "exported" : "MISSING", dl_mvk ? "exported" : "MISSING");
        if (wine10 && (!dl_metal || !dl_mvk)) { printf("FAIL Wine 10's winemac.drv would refuse this library\n"); return 1; }
        const char *(*anvp)(void) = (const char *(*)(void))dlsym(h, "intelxe_vk_anv_path");
        const char *ver = (const char *)dlsym(h, "intelxe_vk_version");
        if (ver) printf("provider: %s\n", ver);
        if (anvp) printf("provider loads ANV from %s\n", anvp() ?: "(failed)");
    } else {
        h = dlopen(icd, RTLD_NOW | RTLD_LOCAL);
        if (!h) { printf("FAIL dlopen %s: %s\n", icd, dlerror()); return 1; }
        gipa = (PFN_vkGetInstanceProcAddr)dlsym(h, "vk_icdGetInstanceProcAddr");
        if (!gipa) { printf("FAIL %s has no vk_icdGetInstanceProcAddr\n", icd); return 1; }
        printf("ICD %s\n", icd);
    }
    if (!wine10) dl_metal = NULL;

    [NSApplication sharedApplication];
    [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
    NSWindow *win = [[NSWindow alloc] initWithContentRect:NSMakeRect(100, 100, W, H)
        styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable backing:NSBackingStoreBuffered defer:NO];
    win.title = @"vksurf";
    WineLikeView *view = [[WineLikeView alloc] initWithFrame:NSMakeRect(0, 0, W, H)];
    if (mtldev) view.mtl = MTLCreateSystemDefaultDevice();
    view.wantsLayer = YES;
    view.layerContentsRedrawPolicy = NSViewLayerContentsRedrawNever;
    win.contentView = view;
    [win makeKeyAndOrderFront:nil];
    [NSApp activateIgnoringOtherApps:YES];
    [NSApp finishLaunching];
    pump();
    CAMetalLayer *layer = (CAMetalLayer *)view.layer;
    if (![layer isKindOfClass:[CAMetalLayer class]]) { printf("FAIL the view has no CAMetalLayer\n"); return 1; }
    printf("layer: CAMetalLayer, device %s\n", layer.device ? layer.device.name.UTF8String : "(none)");

    uint32_t n = 0;
    PFN_vkEnumerateInstanceExtensionProperties eie = (PFN_vkEnumerateInstanceExtensionProperties)gipa(NULL, "vkEnumerateInstanceExtensionProperties");
    CHECK(eie(NULL, &n, NULL));
    VkExtensionProperties *ie = calloc(n, sizeof *ie);
    CHECK(eie(NULL, &n, ie));
    int has_metal = 0, has_mvk = 0;
    for (uint32_t i = 0; i < n; i++) {
        has_metal |= !strcmp(ie[i].extensionName, VK_EXT_METAL_SURFACE_EXTENSION_NAME);
        has_mvk |= !strcmp(ie[i].extensionName, VK_MVK_MACOS_SURFACE_EXTENSION_NAME);
    }
    printf("instance: %u extensions, VK_EXT_metal_surface %s, VK_MVK_macos_surface %s\n", n, has_metal ? "yes" : "NO", has_mvk ? "yes" : "no");
    if (mvk && !has_mvk) { printf("FAIL --mvk but VK_MVK_macos_surface is not offered\n"); return 1; }
    const char *exts[2] = {VK_KHR_SURFACE_EXTENSION_NAME, mvk ? VK_MVK_MACOS_SURFACE_EXTENSION_NAME : VK_EXT_METAL_SURFACE_EXTENSION_NAME};
    VkApplicationInfo ai = {VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "vksurf", 1, "vksurf", 1, VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &ai, 0, NULL, 2, exts};
    PFN_vkCreateInstance ci_ = (PFN_vkCreateInstance)gipa(NULL, "vkCreateInstance");
    CHECK(ci_(&ici, NULL, &inst));
    gdpa = (PFN_vkGetDeviceProcAddr)gipa(inst, "vkGetDeviceProcAddr");
    IF(vkEnumeratePhysicalDevices);
    IF(vkGetPhysicalDeviceProperties);
    IF(vkGetPhysicalDeviceQueueFamilyProperties);
    IF(vkGetPhysicalDeviceSurfaceSupportKHR);
    IF(vkCreateDevice);
    IF(vkDestroySurfaceKHR);
    IF(vkDestroyInstance);
    n = 1;
    CHECK(vkEnumeratePhysicalDevices(inst, &n, &pd));
    if (!n) { printf("FAIL no physical device\n"); return 1; }
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(pd, &props);
    printf("device: %s, Vulkan %u.%u.%u\n", props.deviceName, VK_API_VERSION_MAJOR(props.apiVersion),
           VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));

    VkMetalSurfaceCreateInfoEXT msci = {VK_STRUCTURE_TYPE_METAL_SURFACE_CREATE_INFO_EXT, NULL, 0, layer};
    double t0 = now_ms();
    if (mvk) {
        PFN_vkCreateMacOSSurfaceMVK f = (PFN_vkCreateMacOSSurfaceMVK)gipa(inst, "vkCreateMacOSSurfaceMVK");
        if (!f) { printf("FAIL no vkCreateMacOSSurfaceMVK\n"); return 1; }
        VkMacOSSurfaceCreateInfoMVK m = {VK_STRUCTURE_TYPE_MACOS_SURFACE_CREATE_INFO_MVK, NULL, 0, (__bridge const void *)layer};
        CHECK(f(inst, &m, NULL, &surf));
    } else if (dl_metal) {
        CHECK(dl_metal(inst, &msci, NULL, &surf));   // the exported symbol, as Wine 10 calls it
    } else {
        PFN_vkCreateMetalSurfaceEXT f = (PFN_vkCreateMetalSurfaceEXT)gipa(inst, "vkCreateMetalSurfaceEXT");
        if (!f) { printf("FAIL no vkCreateMetalSurfaceEXT\n"); return 1; }
        CHECK(f(inst, &msci, NULL, &surf));
    }
    printf("surface created in %.2f ms (%s)\n", now_ms() - t0, mvk ? "vkCreateMacOSSurfaceMVK" : dl_metal ? "dlsym'd vkCreateMetalSurfaceEXT" : "vkCreateMetalSurfaceEXT");

    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, NULL);
    VkQueueFamilyProperties qp[16];
    if (nq > 16) nq = 16;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nq, qp);
    qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq && qfam == UINT32_MAX; i++) {
        VkBool32 sup = 0;
        CHECK(vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surf, &sup));
        if (sup && (qp[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) qfam = i;
    }
    if (qfam == UINT32_MAX) { printf("FAIL no graphics queue family can present to the surface\n"); return 1; }
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, qfam, 1, &prio};
    const char *dexts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci = {VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL, 1, dexts, NULL};
    CHECK(vkCreateDevice(pd, &dci, NULL, &dev));
    if (!gdpa) { printf("FAIL vkGetDeviceProcAddr not available\n"); return 1; }
    DF(vkGetDeviceQueue);
    DF(vkCreateCommandPool);
    DF(vkAllocateCommandBuffers);
    DF(vkBeginCommandBuffer);
    DF(vkEndCommandBuffer);
    DF(vkResetCommandBuffer);
    DF(vkCmdPipelineBarrier);
    DF(vkCmdClearColorImage);
    DF(vkQueueSubmit);
    DF(vkQueuePresentKHR);
    DF(vkAcquireNextImageKHR);
    DF(vkCreateSemaphore);
    DF(vkDestroySemaphore);
    DF(vkCreateFence);
    DF(vkWaitForFences);
    DF(vkResetFences);
    DF(vkDeviceWaitIdle);
    DF(vkDestroySwapchainKHR);
    DF(vkDestroyCommandPool);
    DF(vkDestroyFence);
    DF(vkDestroyDevice);
    vkGetDeviceQueue(dev, qfam, 0, &queue);
    make_swapchain();

    enum { F = 2 };
    VkCommandPool pool;
    VkCommandPoolCreateInfo pci = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, qfam};
    CHECK(vkCreateCommandPool(dev, &pci, NULL, &pool));
    VkCommandBuffer cb[F];
    VkCommandBufferAllocateInfo cai = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, F};
    CHECK(vkAllocateCommandBuffers(dev, &cai, cb));
    VkSemaphore acq[F];
    VkFence fence[F];
    VkSemaphoreCreateInfo sci = {VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fci = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, NULL, VK_FENCE_CREATE_SIGNALED_BIT};
    for (int i = 0; i < F; i++) { CHECK(vkCreateSemaphore(dev, &sci, NULL, &acq[i])); CHECK(vkCreateFence(dev, &fci, NULL, &fence[i])); }
    for (int i = 0; i < 16; i++) CHECK(vkCreateSemaphore(dev, &sci, NULL, &done[i]));

    double *acq_ms = calloc(frames, sizeof(double)), *pres_ms = calloc(frames, sizeof(double)), *iv_ms = calloc(frames, sizeof(double));
    int presented = 0, suboptimal = 0, outofdate = 0, recreated = 0, resized = 0;
    float last[4] = {0};
    double prev = 0, start = now_ms();
    for (int fr = 0; fr < frames; fr++) {
        pump();
        if (resize && !resized && fr == frames / 2) {
            [win setContentSize:NSMakeSize(W * 3 / 4, H * 3 / 4)];
            pump();
            resized = 1;
        }
        int s = fr % F;
        CHECK(vkWaitForFences(dev, 1, &fence[s], VK_TRUE, UINT64_MAX));
        uint32_t idx;
        double a = now_ms();
        VkResult r = vkAcquireNextImageKHR(dev, sc, UINT64_MAX, acq[s], VK_NULL_HANDLE, &idx);
        acq_ms[fr] = now_ms() - a;
        if (r == VK_ERROR_OUT_OF_DATE_KHR) { outofdate++; CHECK(vkDeviceWaitIdle(dev)); make_swapchain(); recreated++; fr--; continue; }
        if (r == VK_SUBOPTIMAL_KHR) suboptimal++;
        else CHECK(r);
        CHECK(vkResetFences(dev, 1, &fence[s]));
        float t = (float)fr / frames;
        VkClearColorValue c = {{t, 1.0f - t, (fr & 1) ? 1.0f : 0.25f, 1.0f}};
        memcpy(last, c.float32, sizeof last);
        VkImageSubresourceRange rng = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkCommandBufferBeginInfo bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT};
        CHECK(vkResetCommandBuffer(cb[s], 0));
        CHECK(vkBeginCommandBuffer(cb[s], &bi));
        VkImageMemoryBarrier b1 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
            VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[idx], rng};
        vkCmdPipelineBarrier(cb[s], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, 1, &b1);
        vkCmdClearColorImage(cb[s], imgs[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &c, 1, &rng);
        VkImageMemoryBarrier b2 = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, 0,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED, imgs[idx], rng};
        vkCmdPipelineBarrier(cb[s], VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, NULL, 0, NULL, 1, &b2);
        CHECK(vkEndCommandBuffer(cb[s]));
        VkPipelineStageFlags ws = VK_PIPELINE_STAGE_TRANSFER_BIT;
        VkSubmitInfo si = {VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 1, &acq[s], &ws, 1, &cb[s], 1, &done[idx]};
        CHECK(vkQueueSubmit(queue, 1, &si, fence[s]));
        VkPresentInfoKHR pi = {VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, NULL, 1, &done[idx], 1, &sc, &idx, NULL};
        double p = now_ms();
        r = vkQueuePresentKHR(queue, &pi);
        double q = now_ms();
        pres_ms[presented] = q - p;
        if (prev) iv_ms[presented - 1] = q - prev;
        prev = q;
        presented++;
        if (r == VK_SUBOPTIMAL_KHR || r == VK_ERROR_OUT_OF_DATE_KHR) {
            if (r == VK_SUBOPTIMAL_KHR) suboptimal++; else outofdate++;
            CHECK(vkDeviceWaitIdle(dev)); make_swapchain(); recreated++;
        } else CHECK(r);
    }
    CHECK(vkDeviceWaitIdle(dev));
    double total = now_ms() - start;
    usleep(100000);
    pump();

    // what the window server shows: ANV's IOSurface sublayer
    int pix = -1;
    for (CALayer *sub in layer.sublayers) {
        if (![sub.name isEqualToString:@"org.freedesktop.mesa.wsi.iosurface"]) continue;
        id c = sub.contents;
        if (!c || CFGetTypeID((__bridge CFTypeRef)c) != IOSurfaceGetTypeID()) { printf("IOSurface layer has no IOSurface contents\n"); break; }
        IOSurfaceRef s = (__bridge IOSurfaceRef)c;
        IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
        size_t w = IOSurfaceGetWidth(s), hh = IOSurfaceGetHeight(s), bpr = IOSurfaceGetBytesPerRow(s);
        uint32_t pf = IOSurfaceGetPixelFormat(s);
        const uint8_t *px = (const uint8_t *)IOSurfaceGetBaseAddress(s) + (hh / 2) * bpr + (w / 2) * 4;
        uint8_t b = px[0], g = px[1], rr = px[2];
        IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
        int er = (int)(last[0] * 255 + .5f), eg = (int)(last[1] * 255 + .5f), eb = (int)(last[2] * 255 + .5f);
        if (pf == 'BGRA') {
            pix = abs(rr - er) <= 2 && abs(g - eg) <= 2 && abs(b - eb) <= 2;
            printf("shown IOSurface %zux%zu 'BGRA': centre r%d g%d b%d, last clear r%d g%d b%d: %s\n", w, hh, rr, g, b, er, eg, eb, pix ? "match" : "MISMATCH");
        } else printf("shown IOSurface %zux%zu pixel format 0x%08x: not compared\n", w, hh, pf);
        break;
    }
    if (pix < 0) printf("no IOSurface sublayer read (copying WSI path or another driver): pixel check skipped\n");

    int m = presented > 1 ? presented - 1 : 1;
    double sum = 0, mx = 0, psum = 0, pmx = 0, amx = 0;
    for (int i = 0; i < m; i++) { sum += iv_ms[i]; if (iv_ms[i] > mx) mx = iv_ms[i]; }
    for (int i = 0; i < presented; i++) { psum += pres_ms[i]; if (pres_ms[i] > pmx) pmx = pres_ms[i]; }
    for (int i = 0; i < frames; i++) if (acq_ms[i] > amx) amx = acq_ms[i];
    printf("frames %d presented %d in %.0f ms: %.1f fps; frame interval avg %.2f ms max %.2f ms; vkQueuePresentKHR avg %.3f ms max %.3f ms; acquire max %.3f ms\n",
           frames, presented, total, presented * 1000.0 / total, sum / m, mx, psum / presented, pmx, amx);
    printf("suboptimal %d, out of date %d, swapchains recreated %d%s\n", suboptimal, outofdate, recreated,
           resized ? " (window resized halfway)" : "");
    if (presented < frames) { printf("FAIL only %d of %d frames presented\n", presented, frames); fails++; }
    if (resized && !recreated) { printf("FAIL the resize did not make the swapchain SUBOPTIMAL or OUT_OF_DATE\n"); fails++; }
    if (pix == 0) fails++;
    if (mode == VK_PRESENT_MODE_FIFO_KHR && sum / m < 2.0) { printf("FAIL FIFO frames came faster than any display refresh (%.2f ms)\n", sum / m); fails++; }

    vkDestroySwapchainKHR(dev, sc, NULL);
    for (int i = 0; i < 16; i++) vkDestroySemaphore(dev, done[i], NULL);
    for (int i = 0; i < F; i++) { vkDestroySemaphore(dev, acq[i], NULL); vkDestroyFence(dev, fence[i], NULL); }
    vkDestroyCommandPool(dev, pool, NULL);
    vkDestroyDevice(dev, NULL);
    vkDestroySurfaceKHR(inst, surf, NULL);
    vkDestroyInstance(inst, NULL);
    printf("%s\n", fails ? "FAIL" : "PASS");
    return fails ? 1 : 0;
    }
}
