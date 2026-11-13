// SPDX-License-Identifier: MIT
// cglzink: a process's Apple OpenGL contexts draw through Mesa's zink instead of Apple's GL engine.
//
// Loaded with DYLD_INSERT_LIBRARIES, into processes chosen by the user (cglzink-app.sh). Apple's CGL objects stay (AppKit
// and the application hold them); each adopted context gets an EGL context of its own, and the context's dispatch table,
// which every gl* symbol goes through, is filled with thunks that drop Apple's context argument and call zink.
//
// What the table does not cover is hooked here: making a context current, locking, presenting (CGLFlushDrawable and
// NSOpenGLContext's flushBuffer), attaching a view, the context's lifetime, IOSurface textures, pbuffers. Four table entries
// are wrapped (glFlush, glFinish, glFramebufferTexture2D, glDeleteTextures) for IOSurface textures that are drawn into,
// and (CGLZINK_SURF_WRITEBACK's default, dirty) glBindFramebuffer and the entries that write pixels (mark_class_of).
// Nothing here is required: when EGL cannot be opened, or CGLZINK_OFF is set, every context stays on Apple's OpenGL.
//
// The context reports Apple's version, GLSL version and extensions for its profile (appleview.h), not zink's 4.6.
//
// Contexts that Core Animation makes for a CAOpenGLLayer / NSOpenGLLayer stay on Apple's OpenGL. IOSurface textures,
// pbuffers and CGLSetOffScreen memory are copies (see their sections). See README.md.
//
// History: prototype in the hackintosh research tree, tools/b580/cgl-zink (0640304, cb58869, ea35ef0, 57e1f0c, 5 Nov 2026).
#define GL_SILENCE_DEPRECATION
#import <AppKit/AppKit.h>
#import <QuartzCore/QuartzCore.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>
#include <OpenGL/CGLIOSurface.h>
#include <OpenGL/gl3.h>
#include <dispatch/dispatch.h>
#include <dlfcn.h>
#include <mach-o/dyld.h>
#include <objc/runtime.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

// The few EGL declarations this needs: no EGL headers on a machine that only has the framework.
typedef void *EGLDisplay, *EGLConfig, *EGLContext, *EGLSurface;
typedef int EGLint;
typedef unsigned EGLBoolean;
#define EGL_BAD_ACCESS 0x3002
#define EGL_NONE 0x3038
#define EGL_OPENGL_API 0x30A2
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 1
#define EGL_WINDOW_BIT 4
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_BIT 8
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_DEPTH_SIZE 0x3025
#define EGL_STENCIL_SIZE 0x3026
#define EGL_SAMPLES 0x3031
#define EGL_SAMPLE_BUFFERS 0x3032
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_MAJOR_VERSION 0x3098
#define EGL_CONTEXT_MINOR_VERSION 0x30FB
#define EGL_CONTEXT_OPENGL_PROFILE_MASK 0x30FD
#define EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT 1
#define EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT 2

static struct {
    EGLDisplay (*GetDisplay)(void *);
    EGLBoolean (*Initialize)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean (*BindAPI)(unsigned);
    EGLBoolean (*ChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLBoolean (*DestroyContext)(EGLDisplay, EGLContext);
    EGLSurface (*CreateWindowSurface)(EGLDisplay, EGLConfig, void *, const EGLint *);
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
    EGLBoolean (*DestroySurface)(EGLDisplay, EGLSurface);
    EGLBoolean (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean (*SwapBuffers)(EGLDisplay, EGLSurface);
    EGLBoolean (*SwapInterval)(EGLDisplay, EGLint);
    EGLint (*GetError)(void);
    void *(*GetProcAddress)(const char *);
    // optional: only for the log
    EGLBoolean (*GetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
    EGLBoolean (*GetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
} egl;
static EGLDisplay g_display;
static int g_log;
// CGLZINK_FPS=<seconds>: presents per second on stderr, for applications without a counter of their own.
// CGLZINK_APPLE=1: nothing is adopted (Apple's GL), but the counter runs: the same measurement before and after.
static double g_fps_interval;
static int g_apple_only;
// CGLZINK_SURF_WRITEBACK=<list> (docs/design/DESIGN-zink-gpu-gap.md); unset is "dirty":
//   dirty    (default) at glFlush, glFinish and present, copy an IOSurface texture back only when the context drew into
//            one of the framebuffers it is attached to (or uploaded a texture) since the last copy; CGLSetOffScreen
//            memory only when the context drew at all. Same pixels as flush (B580, 13 Nov), 3.3x the frame rate.
//   flush    the copy at every glFlush, glFinish and present, whatever was drawn (the behaviour before 13 Nov)
//   present  opt-in, changes semantics: no copy at glFlush, only at glFinish and present (a reader that syncs on
//            glFlush sees the previous frame); with dirty unless flush is also given
static int g_wb_dirty, g_wb_present;
static uintptr_t g_main_thread;
#define LOG(...) do { if (g_log) { fprintf(stderr, "cglzink: " __VA_ARGS__); fputc('\n', stderr); } } while (0)
// Said once per process, also without CGLZINK_LOG: something the application relies on does not work through zink.
#define WARN_ONCE(...) do { static atomic_flag said_; if (!atomic_flag_test_and_set(&said_)) { fprintf(stderr, "cglzink: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// One per adopted context. The list is short (a process has few contexts) and read under its lock only on a change.
// An entry is never freed, only marked dead: another thread may still hold it as its current context.
typedef struct Adopted {
    struct Adopted *next;
    CGLContextObj cgl;
    GLIContext rend;
    GLIFunctionDispatch apple;
    EGLContext context;
    EGLConfig config;
    EGLSurface offscreen, window;
    EGLSurface pbuffer_surface;   // CGLSetPBuffer / CGLSetOffScreen: the context draws into this EGL pbuffer (wins over a window)
    CGLPBufferObj pbuffer;        // CGLSetPBuffer's, retained while set
    void *offscreen_base;         // CGLSetOffScreen's memory, filled at each flush: rows of offscreen_rowbytes
    GLsizei offscreen_w, offscreen_h; GLint offscreen_rowbytes;
    GLint backing[2];             // kCGLCPSurfaceBackingSize, applied while kCGLCESurfaceBackingSize is enabled
    CALayer *layer;
    NSView *view;              // not retained, as NSOpenGLContext: Apple's context never hears of it
    int best_resolution;       // the view asked for a Retina surface (wantsBestResolutionOpenGLSurface)
    unsigned swaps;
    int applied_interval;      // what eglSwapInterval was last given for the window surface; -1: EGL's default
    _Atomic(uintptr_t) owner;  // the thread the EGL context is current on, 0 for none
    _Atomic int multi;         // seen on two threads: given up at every CGLUnlockContext from then on
    int dead;
    struct AppleView *apple_view;   // Apple's version and extensions for this profile (appleview.h)
    GLenum element_type;       // GL_APPLE_element_array
    const GLvoid *element_pointer;
    // CGLZINK_SURF_WRITEBACK=dirty (default): what was drawn since the last copy-back, and the framebuffers with an IOSurface texture
    int drew_surface, drew_any, surface_fbo_bound;
    GLuint surface_fbos[8]; int n_surface_fbos;
} Adopted;
// What the context draws into: a pbuffer it was given, else its view's window surface, else the 1x1 offscreen one.
static EGLSurface draw_surface(const Adopted *a) { return a->pbuffer_surface ? a->pbuffer_surface : a->window ? a->window : a->offscreen; }
static Adopted *g_adopted;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
// The thread's current context, as the thunks and the hooks see it, and whether EGL really has it current here.
static __thread Adopted *t_current;
static __thread GLIContext t_rend;
static __thread int t_bound;
// Set while this library calls Apple's CGL or EGL itself, so that a backend using CGL is not adopted in its turn.
static __thread int t_inside;
// Set while Core Animation makes a CAOpenGLLayer's context: such a context stays on Apple's OpenGL.
static __thread int t_no_adopt;

static void bind_rend(GLIContext rend);
static void pbuffer_clear(struct Adopted *a);
static void offscreen_fill(struct Adopted *a);
static void update_layer(struct Adopted *a);
static void surftex_forget(struct Adopted *a);
static void surftex_write_back(struct Adopted *a, int at_flush);
static void wrap_entries(void);
#define CGLT_ENTER(c) do { if (__builtin_expect(t_rend != (c), 0)) bind_rend(c); } while (0)
// CGLZINK_SURF_WRITEBACK=dirty (default): the entries that write pixels go through a marking wrapper (wrap_entries)
static void mark_drew(int entry);
#define CGLT_MARK(i) mark_drew(i)
#include "thunks.h"
#include "backend_names.h"
void *cglt_backend[CGLT_N];

// An entry the backend does not have: nothing happens, and what it returns reads as 0. Every entry in the table returns
// void, an integer or a pointer, so one function serves them all.
static uintptr_t missing_entry(void) { return 0; }

static void present(Adopted *a);
#include "appleext.h"
#include "appleview.h"
static AppleView *current_view(void) { return t_current ? t_current->apple_view : NULL; }

static void *open_backend(void) {
    // A sandboxed application cannot read /usr/local, where zink's @rpath and the Vulkan loader's manifests point. With the
    // loader already in the process, dyld answers zink's dlopen by its install name.
    if (!getenv("CGLZINK_EGL")) {
        setenv("VK_ICD_FILENAMES", "/Library/IntelXe/GL/intel_icd.x86_64.json", 0);
        dlopen("/Library/IntelXe/GL/libvulkan.1.dylib", RTLD_NOW | RTLD_GLOBAL);
    }
    const char *path = getenv("CGLZINK_EGL");
    return dlopen(path ? path : "/Library/IntelXe/GL/libEGL.1.dylib", RTLD_NOW | RTLD_LOCAL);
}

static void log_configs(void) {
    if (!g_log || !egl.GetConfigs || !egl.GetConfigAttrib) return;
    EGLConfig configs[256];
    EGLint n = 0, window = 0, msaa = 0, max_samples = 0;
    if (!egl.GetConfigs(g_display, configs, 256, &n)) return;
    for (int i = 0; i < n; i++) {
        EGLint type = 0, samples = 0;
        egl.GetConfigAttrib(g_display, configs[i], EGL_SURFACE_TYPE, &type);
        egl.GetConfigAttrib(g_display, configs[i], EGL_SAMPLES, &samples);
        window += (type & EGL_WINDOW_BIT) != 0;
        msaa += samples > 1;
        if (samples > max_samples) max_samples = samples;
    }
    LOG("%d EGL configs, %d for windows, %d multisampled (up to %d samples)", n, window, msaa, max_samples);
}

// Once, on the first context. Fails soft: g_display stays NULL and nothing is adopted.
static void backend_init(void) {
    if (getenv("CGLZINK_OFF")) return;
    t_inside++;
    void *h = open_backend();
    if (!h) { LOG("no EGL: %s", dlerror()); t_inside--; return; }
#define R(name) if (!(*(void **)&egl.name = dlsym(h, "egl" #name))) { LOG("EGL without egl" #name); t_inside--; return; }
    R(GetDisplay) R(Initialize) R(BindAPI) R(ChooseConfig) R(CreateContext) R(DestroyContext) R(CreateWindowSurface)
    R(CreatePbufferSurface) R(DestroySurface) R(MakeCurrent) R(SwapBuffers) R(SwapInterval) R(GetError) R(GetProcAddress)
#undef R
    *(void **)&egl.GetConfigs = dlsym(h, "eglGetConfigs");
    *(void **)&egl.GetConfigAttrib = dlsym(h, "eglGetConfigAttrib");
    EGLDisplay display = egl.GetDisplay(NULL);
    EGLint major = 0, minor = 0;
    if (!display || !egl.Initialize(display, &major, &minor) || !egl.BindAPI(EGL_OPENGL_API)) {
        LOG("EGL did not start: 0x%x", egl.GetError());
        t_inside--;
        return;
    }
    int found = 0, made = 0;
    for (int i = 0; i < CGLT_N; i++) {
        void *entry = cglt_names[i] ? egl.GetProcAddress(cglt_names[i]) : NULL;
        found += entry != NULL;
        // Apple's own extensions, which zink does not have: the ones applications rely on are made from core functions
        if (!entry && cglt_names[i] && (entry = apple_extension(cglt_names[i]))) made++;
        cglt_backend[i] = entry ? entry : (void *)missing_entry;
    }
    LOG("EGL %d.%d, %d of %d table entries, %d more made from core functions", major, minor, found, CGLT_N, made);
    wrap_entries();
    view_install();
    g_display = display;
    log_configs();
    t_inside--;
}

static EGLConfig choose_config(CGLPixelFormatObj format) {
    GLint color = 24, alpha = 8, depth = 24, stencil = 8, samples = 0, buffers = 0;
    if (format) {
        CGLDescribePixelFormat(format, 0, kCGLPFAColorSize, &color);
        CGLDescribePixelFormat(format, 0, kCGLPFAAlphaSize, &alpha);
        CGLDescribePixelFormat(format, 0, kCGLPFADepthSize, &depth);
        CGLDescribePixelFormat(format, 0, kCGLPFAStencilSize, &stencil);
        CGLDescribePixelFormat(format, 0, kCGLPFASamples, &samples);
        CGLDescribePixelFormat(format, 0, kCGLPFASampleBuffers, &buffers);
    }
    int per_channel = color >= 30 && color != 32 ? 10 : 8;   // Apple's 32 is 8 bits per channel with alpha
    if (depth > 24) depth = 24;                              // Apple's 32 is floating point; EGL's sizes are minimums and go to 24 here
    const int msaa = buffers > 0 && samples > 1;             // Apple can answer one buffer with zero samples
    // A platform may give window and pbuffer surfaces in different configs: an offscreen context without a pbuffer binds
    // no surface at all.
    const EGLint surfaces[] = { EGL_WINDOW_BIT | EGL_PBUFFER_BIT, EGL_WINDOW_BIT, EGL_PBUFFER_BIT };
    // Without a multisampled config (the build Mac's stand-in has none; the B580 offers up to 8 samples) the request goes on
    // without MSAA instead of staying on Apple's GL
    for (int ms = msaa; ms >= 0; ms--) {
        for (int i = 0; i < 3; i++) {
            const EGLint wanted[] = {
                EGL_SURFACE_TYPE, surfaces[i], EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
                EGL_RED_SIZE, per_channel, EGL_GREEN_SIZE, per_channel, EGL_BLUE_SIZE, per_channel,
                EGL_ALPHA_SIZE, alpha, EGL_DEPTH_SIZE, depth, EGL_STENCIL_SIZE, stencil,
                EGL_SAMPLE_BUFFERS, ms ? 1 : 0, EGL_SAMPLES, ms ? samples : 0, EGL_NONE };
            EGLConfig config = NULL;
            EGLint n = 0;
            if (egl.ChooseConfig(g_display, wanted, &config, 1, &n) && n > 0) {
                if (msaa && !ms) WARN_ONCE("no EGL config with %d samples: contexts asking for MSAA get none", samples);
                return config;
            }
        }
    }
    LOG("no EGL config for color %d alpha %d depth %d stencil %d buffers %d samples %d", color, alpha, depth, stencil, buffers, samples);
    return NULL;
}

static Adopted *find_cgl(CGLContextObj cgl) {
    pthread_mutex_lock(&g_lock);
    Adopted *a = g_adopted;
    while (a && a->cgl != cgl) a = a->next;
    pthread_mutex_unlock(&g_lock);
    return a;
}

static Adopted *find_rend(GLIContext rend) {
    pthread_mutex_lock(&g_lock);
    Adopted *a = g_adopted;
    while (a && a->rend != rend) a = a->next;
    pthread_mutex_unlock(&g_lock);
    return a;
}

// For tests: whether a context draws through zink.
__attribute__((visibility("default"))) int cglzink_adopted(CGLContextObj cgl) { return cgl && find_cgl(cgl) != NULL; }

static void adopt(CGLContextObj cgl, CGLPixelFormatObj format, CGLContextObj share) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    if (g_apple_only) return;
    pthread_once(&once, backend_init);
    if (!g_display || t_inside || find_cgl(cgl)) return;
    if (t_no_adopt) { LOG("%p belongs to a CAOpenGLLayer: it stays on Apple's OpenGL", (void *)cgl); return; }
    Adopted *shared = share ? find_cgl(share) : NULL;
    // Objects cannot be shared between Apple's engine and zink: both contexts go one way
    if (share && !shared) { LOG("%p shares with %p, which is on Apple's OpenGL: it stays there as well", (void *)cgl, (void *)share); return; }
    t_inside++;
    GLint profile = kCGLOGLPVersion_Legacy;
    if (format) CGLDescribePixelFormat(format, 0, kCGLPFAOpenGLProfile, &profile);
    // The same versions Apple's GL gives for these profiles. zink is newer than all of them.
    const int core = profile != kCGLOGLPVersion_Legacy;
    const EGLint attributes[] = {
        EGL_CONTEXT_MAJOR_VERSION, core ? (profile == kCGLOGLPVersion_GL4_Core ? 4 : 3) : 2,
        EGL_CONTEXT_MINOR_VERSION, core ? (profile == kCGLOGLPVersion_GL4_Core ? 1 : 2) : 1,
        EGL_CONTEXT_OPENGL_PROFILE_MASK,
        core ? EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT : EGL_CONTEXT_OPENGL_COMPATIBILITY_PROFILE_BIT, EGL_NONE };
    EGLConfig config = choose_config(format);
    EGLContext context = config ? egl.CreateContext(g_display, config, shared ? shared->context : NULL, attributes) : NULL;
    if (!context) {
        if (shared) WARN_ONCE("no EGL context for a context that shares with one on zink: their objects are not shared");
        LOG("no EGL context for profile 0x%x (0x%x): this context stays on Apple's OpenGL", profile, egl.GetError());
        t_inside--;
        return;
    }
    const EGLint size[] = { EGL_WIDTH, 1, EGL_HEIGHT, 1, EGL_NONE };
    EGLSurface offscreen = egl.CreatePbufferSurface(g_display, config, size);
    Adopted *a = calloc(1, sizeof *a);
    a->cgl = cgl;
    a->rend = cgl->rend;
    a->apple = cgl->disp;
    a->context = context;
    a->config = config;
    a->offscreen = offscreen;
    a->applied_interval = -1;
    pthread_mutex_lock(&g_lock);
    a->next = g_adopted;
    g_adopted = a;
    pthread_mutex_unlock(&g_lock);
    a->apple_view = view_from_apple(cgl, core);   // before the table is replaced
    memcpy(&cgl->disp, cglt_table, sizeof cglt_table);
    LOG("adopted %p, profile 0x%x%s; told %s, GLSL %s, %d of Apple's extensions to filter", (void *)cgl, profile,
        shared ? ", shared" : "", a->apple_view->version, a->apple_view->glsl, a->apple_view->napple);
    t_inside--;
}

// The swap interval Apple's context holds (the application's value, or Apple's default: 1 for an NSOpenGLContext on
// macOS 27) goes to the window surface, so that an application that turns vsync off for a benchmark gets it off here too.
static void apply_interval(Adopted *a) {
    if (!a->window) return;
    GLint interval = 0;
    CGLGetParameter(a->cgl, kCGLCPSwapInterval, &interval);
    if (interval != a->applied_interval) {
        egl.SwapInterval(g_display, interval);
        a->applied_interval = interval;
        LOG("%p: swap interval %d", (void *)a->cgl, interval);
    }
}

static void release_owner(Adopted *a) {
    uintptr_t me = (uintptr_t)pthread_self();
    if (a) atomic_compare_exchange_strong(&a->owner, &me, 0);
}

static void release_on_main(void *p);

// eglMakeCurrent for this thread. A context EGL has current on another thread cannot be taken from there (EGL_BAD_ACCESS),
// as Apple's GL can: the other thread gives it up at its next CGLUnlockContext, or, when it is the main thread, in a
// block on the main queue. This thread waits up to 100 ms for that.
static int egl_bind(Adopted *a) {
    EGLSurface surface = draw_surface(a);
    const uintptr_t me = (uintptr_t)pthread_self();
    for (int attempt = 0;; attempt++) {
        if (egl.MakeCurrent(g_display, surface, surface, a->context)) {
            atomic_store(&a->owner, me);
            return 1;
        }
        EGLint error = egl.GetError();
        if (error != EGL_BAD_ACCESS || attempt) {
            if (error == EGL_BAD_ACCESS)
                WARN_ONCE("a context is current on two threads and the other one did not give it up: GL calls here are dropped");
            LOG("eglMakeCurrent %p: 0x%x", (void *)a->cgl, error);
            egl.MakeCurrent(g_display, NULL, NULL, NULL);
            return 0;
        }
        if (!atomic_exchange(&a->multi, 1)) LOG("%p is used on two threads", (void *)a->cgl);
        uintptr_t owner = atomic_load(&a->owner);
        if (owner && owner == g_main_thread && !pthread_main_np())
            dispatch_async_f(dispatch_get_main_queue(), a, release_on_main);
        for (int i = 0; i < 100 && (owner = atomic_load(&a->owner)) && owner != me; i++) usleep(1000);
    }
}

static void bind_context(Adopted *a) {
    t_inside++;
    if (t_current && t_current != a) release_owner(t_current);
    if (a && !a->dead) {
        t_bound = egl_bind(a);
        if (t_bound) apply_interval(a);
    } else {
        if (t_current) egl.MakeCurrent(g_display, NULL, NULL, NULL);
        t_bound = 0;
        a = NULL;
    }
    // Also after a failed bind: the thunks do not retry on every call; the next make-current or lock does.
    t_current = a;
    t_rend = a ? a->rend : NULL;
    t_inside--;
}

// The thread gives its EGL context up but keeps its CGL context: the next GL call binds it again (CGLT_ENTER).
static void unbind_keep_cgl(void) {
    if (!t_current) return;
    t_inside++;
    egl.MakeCurrent(g_display, NULL, NULL, NULL);
    release_owner(t_current);
    t_current = NULL;
    t_rend = NULL;
    t_bound = 0;
    t_inside--;
}

static void release_on_main(void *p) {
    if (t_current == (Adopted *)p && !t_inside) unbind_keep_cgl();
}

// A table call for a context that is not the thread's current one: the CGLMacro.h style, another thread's context, or
// the first call after this thread gave its EGL context up.
static void bind_rend(GLIContext rend) { bind_context(find_rend(rend)); }

static CGLError my_CGLCreateContext(CGLPixelFormatObj format, CGLContextObj share, CGLContextObj *out) {
    CGLError error = CGLCreateContext(format, share, out);
    if (error == kCGLNoError && out && *out && !t_inside) adopt(*out, format, share);
    return error;
}

static void view_layer(Adopted *a, NSView *view);

// The context goes back to Apple's table and its EGL objects go. Only when Apple's object goes too: CGL contexts are
// reference counted, and CGLDestroyContext only releases one reference.
static void forget(Adopted *a) {
    t_inside++;
    if (t_current == a) bind_context(NULL);
    view_layer(a, nil);
    pbuffer_clear(a);
    surftex_forget(a);
    if (a->offscreen) egl.DestroySurface(g_display, a->offscreen);
    egl.DestroyContext(g_display, a->context);
    pthread_mutex_lock(&g_lock);
    for (Adopted **p = &g_adopted; *p; p = &(*p)->next) if (*p == a) { *p = a->next; break; }
    pthread_mutex_unlock(&g_lock);
    a->cgl->disp = a->apple;
    a->dead = 1;
    LOG("%p is gone", (void *)a->cgl);
    t_inside--;
}

// CGLDestroyContext = CGLClearDrawable + CGLReleaseContext (Apple's documentation)
static CGLError my_CGLDestroyContext(CGLContextObj cgl) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (a) {
        if (CGLGetContextRetainCount(cgl) <= 1) forget(a);
        else { t_inside++; view_layer(a, nil); t_inside--; }
    }
    return CGLDestroyContext(cgl);
}

static void my_CGLReleaseContext(CGLContextObj cgl) {
    Adopted *a = t_inside || !cgl ? NULL : find_cgl(cgl);
    if (a && CGLGetContextRetainCount(cgl) <= 1) forget(a);
    CGLReleaseContext(cgl);
}

static CGLError my_CGLSetCurrentContext(CGLContextObj cgl) {
    CGLError error = CGLSetCurrentContext(cgl);
    if (!t_inside && error == kCGLNoError) {
        Adopted *a = cgl ? find_cgl(cgl) : NULL;
        if (a != t_current || (a && !t_bound)) bind_context(a);
    }
    return error;
}

// Several threads drawing with one context lock it around their use (Apple's rule). The thread that locks binds it here
// if it is current to it; a context seen on two threads is given up at every unlock, so that the next thread can bind it.
static CGLError my_CGLLockContext(CGLContextObj cgl) {
    CGLError error = CGLLockContext(cgl);
    if (!t_inside && error == kCGLNoError && cgl && CGLGetCurrentContext() == cgl) {
        Adopted *a = find_cgl(cgl);
        if (a && (a != t_current || !t_bound)) bind_context(a);
    }
    return error;
}

static CGLError my_CGLUnlockContext(CGLContextObj cgl) {
    if (!t_inside && t_current && t_current->cgl == cgl && atomic_load(&t_current->multi)) unbind_keep_cgl();
    return CGLUnlockContext(cgl);
}

static void count_present(void) {
    static _Atomic uint64_t frames, since;
    if (g_fps_interval <= 0) return;
    const uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    uint64_t start = atomic_load(&since);
    if (!start) { atomic_compare_exchange_strong(&since, &start, now); return; }
    const uint64_t n = atomic_fetch_add(&frames, 1) + 1;
    const double seconds = (now - start) / 1e9;
    if (seconds >= g_fps_interval && atomic_compare_exchange_strong(&since, &start, now)) {
        atomic_fetch_sub(&frames, n);
        fprintf(stderr, "cglzink: %.1f fps (%llu presents in %.1f s, %s)\n", n / seconds, (unsigned long long)n, seconds,
                g_apple_only ? "Apple's GL" : "zink");
    }
}

static void present(Adopted *a) {
    if (!a) return;
    if (t_current != a || !t_bound) bind_context(a);
    t_inside++;
    surftex_write_back(a, 0);   // what was drawn into IOSurface textures, before anyone shows the surfaces
    offscreen_fill(a);       // CGLSetOffScreen memory
    if (a->window) {
        if (!egl.SwapBuffers(g_display, a->window)) LOG("eglSwapBuffers 0x%x", egl.GetError());
        if (++a->swaps <= 2 || !(a->swaps % 256)) LOG("%u swaps of %p", a->swaps, (void *)a->cgl);
        if (a->swaps == 1 && g_log) {
            const GLubyte *(*get_string)(GLenum) = (const GLubyte *(*)(GLenum))egl.GetProcAddress("glGetString");
            if (get_string) LOG("%p draws with %s | %s", (void *)a->cgl, get_string(GL_RENDERER), get_string(GL_VERSION));
        }
    } else {
        // No window: as Apple's GL, the swap only flushes.
        static void (*flush)(void);
        if (!flush) flush = (void (*)(void))egl.GetProcAddress("glFlush");
        if (flush) flush();
    }
    t_inside--;
}

static CGLError my_CGLFlushDrawable(CGLContextObj cgl) {
    if (!t_inside) count_present();
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLFlushDrawable(cgl);
    present(a);
    return kCGLNoError;
}

static CGLError my_CGLSetParameter(CGLContextObj cgl, CGLContextParameter name, const GLint *value) {
    CGLError error = CGLSetParameter(cgl, name, value);
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (a && name == kCGLCPSwapInterval && t_current == a && t_bound) { t_inside++; apply_interval(a); t_inside--; }
    if (a && name == kCGLCPSurfaceOpacity && value && !*value)
        WARN_ONCE("a see-through surface (kCGLCPSurfaceOpacity 0) is drawn opaque through zink");
    if (a && name == kCGLCPSurfaceBackingSize && value) {
        a->backing[0] = value[0]; a->backing[1] = value[1];
        LOG("%p: backing size %dx%d", (void *)cgl, value[0], value[1]);
        t_inside++; update_layer(a); t_inside--;
    }
    return error;
}

// ---- IOSurface textures ----
// Apple's engine no longer holds this context's textures, so an IOSurface cannot be attached as Apple does (the texture
// IS the surface there). Here the surface's plane is copied into the texture when it is bound, which is all video needs
// (CVOpenGLTextureCache: a new texture per frame, only read). A texture that is then attached to a framebuffer is drawn
// into: its pixels are copied back into the IOSurface at every glFlush, glFinish and present, so whoever shows or reads
// the surface (a CALayer's contents, another process) sees what was drawn. Copies both ways, so slower than Apple's
// shared memory; changes another process makes to the surface reach the texture only when it is bound again.
typedef struct SurfTex {
    Adopted *a; GLuint tex; GLenum target; IOSurfaceRef surface; GLuint plane; GLenum format, type; int drawn;
} SurfTex;
enum { MAX_SURFTEX = 64 };
static SurfTex g_surftex[MAX_SURFTEX];   // g_lock
static int g_nsurftex;
static struct {
    void (*TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (*GetTexImage)(GLenum, GLint, GLenum, GLenum, void *);
    void (*PixelStorei)(GLenum, GLint);
    void (*GetIntegerv)(GLenum, GLint *);
    void (*BindBuffer)(GLenum, GLuint);
    void (*BindTexture)(GLenum, GLuint);
    void (*ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (*Flush)(void);
} gl;
static void gl_init(void) {
    if (gl.TexImage2D) return;
    *(void **)&gl.PixelStorei = egl.GetProcAddress("glPixelStorei");
    *(void **)&gl.GetIntegerv = egl.GetProcAddress("glGetIntegerv");
    *(void **)&gl.BindBuffer = egl.GetProcAddress("glBindBuffer");
    *(void **)&gl.BindTexture = egl.GetProcAddress("glBindTexture");
    *(void **)&gl.GetTexImage = egl.GetProcAddress("glGetTexImage");
    *(void **)&gl.ReadPixels = egl.GetProcAddress("glReadPixels");
    *(void **)&gl.Flush = egl.GetProcAddress("glFlush");
    *(void **)&gl.TexImage2D = egl.GetProcAddress("glTexImage2D");
}
static GLenum binding_of(GLenum target) {
    switch (target) {
    case GL_TEXTURE_2D: return GL_TEXTURE_BINDING_2D;
    case 0x84F5 /* GL_TEXTURE_RECTANGLE */: return 0x84F6;
    default: return 0;
    }
}
// The pixel-store state a copy changes, saved and put back around it (the application's own state).
typedef struct { GLint v[5]; GLenum names[5]; GLenum buffer; } PixelState;
static void pixels_save(PixelState *s, int pack, size_t row, size_t element) {
    const GLenum unpack[5] = { GL_UNPACK_ROW_LENGTH, GL_UNPACK_ALIGNMENT, GL_UNPACK_SKIP_ROWS, GL_UNPACK_SKIP_PIXELS, GL_PIXEL_UNPACK_BUFFER_BINDING };
    const GLenum packn[5] = { GL_PACK_ROW_LENGTH, GL_PACK_ALIGNMENT, GL_PACK_SKIP_ROWS, GL_PACK_SKIP_PIXELS, GL_PIXEL_PACK_BUFFER_BINDING };
    memcpy(s->names, pack ? packn : unpack, sizeof s->names);
    s->buffer = pack ? GL_PIXEL_PACK_BUFFER : GL_PIXEL_UNPACK_BUFFER;
    for (int i = 0; i < 5; i++) gl.GetIntegerv(s->names[i], &s->v[i]);
    if (s->v[4]) gl.BindBuffer(s->buffer, 0);
    gl.PixelStorei(s->names[0], element ? (GLint)(row / element) : 0);
    gl.PixelStorei(s->names[1], 1);
    gl.PixelStorei(s->names[2], 0);
    gl.PixelStorei(s->names[3], 0);
}
static void pixels_restore(const PixelState *s) {
    for (int i = 0; i < 4; i++) gl.PixelStorei(s->names[i], s->v[i]);
    if (s->v[4]) gl.BindBuffer(s->buffer, (GLuint)s->v[4]);
}
static void surface_plane(IOSurfaceRef surface, GLuint plane, void **base, size_t *row, size_t *element) {
    const size_t planes = IOSurfaceGetPlaneCount(surface);
    *base = planes ? IOSurfaceGetBaseAddressOfPlane(surface, plane) : IOSurfaceGetBaseAddress(surface);
    *row = planes ? IOSurfaceGetBytesPerRowOfPlane(surface, plane) : IOSurfaceGetBytesPerRow(surface);
    *element = planes ? IOSurfaceGetBytesPerElementOfPlane(surface, plane) : IOSurfaceGetBytesPerElement(surface);
}
// g_lock held
static void surftex_drop_at(int i) {
    CFRelease(g_surftex[i].surface);
    g_surftex[i] = g_surftex[--g_nsurftex];
}
static void surftex_note(Adopted *a, GLenum target, IOSurfaceRef surface, GLuint plane, GLenum format, GLenum type) {
    const GLenum binding = binding_of(target);
    if (!binding) return;
    GLint tex = 0;
    gl.GetIntegerv(binding, &tex);
    if (!tex) return;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsurftex; i++) if (g_surftex[i].a == a && g_surftex[i].tex == (GLuint)tex) { surftex_drop_at(i); break; }
    if (g_nsurftex == MAX_SURFTEX) {   // the oldest one that is only read goes first
        int k = 0;
        for (int i = 0; i < g_nsurftex; i++) if (!g_surftex[i].drawn) { k = i; break; }
        surftex_drop_at(k);
    }
    g_surftex[g_nsurftex++] = (SurfTex){ a, (GLuint)tex, target, (IOSurfaceRef)CFRetain(surface), plane, format, type, 0 };
    pthread_mutex_unlock(&g_lock);
}
// Copies the textures of `a` that were drawn into back into their IOSurfaces. `a` is current on this thread. at_flush:
// called for glFlush (CGLZINK_SURF_WRITEBACK=present skips those); with =dirty only when `a` drew into one since the last.
static void surftex_write_back(Adopted *a, int at_flush) {
    if (!g_nsurftex || !gl.GetTexImage || !gl.BindTexture) return;
    if (at_flush && g_wb_present) return;
    static _Atomic unsigned long copies, skipped;
    if (g_wb_dirty && !a->drew_surface) {
        const unsigned long k = atomic_fetch_add(&skipped, 1) + 1;
        if (k <= 2 || !(k % 1024)) LOG("IOSurface copy-back skipped, nothing drawn into the surfaces (%lu skipped, %lu made)", k, atomic_load(&copies));
        return;
    }
    a->drew_surface = 0;
    atomic_fetch_add(&copies, 1);
    SurfTex todo[MAX_SURFTEX]; int n = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsurftex; i++) if (g_surftex[i].a == a && g_surftex[i].drawn) { todo[n] = g_surftex[i]; CFRetain(todo[n].surface); n++; }
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < n; i++) {
        SurfTex *t = &todo[i];
        GLint bound = 0;
        gl.GetIntegerv(binding_of(t->target), &bound);
        gl.BindTexture(t->target, t->tex);
        if (IOSurfaceLock(t->surface, 0, NULL) == kIOReturnSuccess) {
            void *base; size_t row, element;
            surface_plane(t->surface, t->plane, &base, &row, &element);
            PixelState st; pixels_save(&st, 1, row, element);
            gl.GetTexImage(t->target, 0, t->format, t->type, base);
            pixels_restore(&st);
            IOSurfaceUnlock(t->surface, 0, NULL);
        }
        gl.BindTexture(t->target, (GLuint)bound);
        CFRelease(t->surface);
    }
}
static void surftex_forget(Adopted *a) {
    pthread_mutex_lock(&g_lock);
    for (int i = g_nsurftex; i-- > 0; ) if (g_surftex[i].a == a) surftex_drop_at(i);
    pthread_mutex_unlock(&g_lock);
}

// The wrapped table entries (four, and glBindFramebuffer with the default CGLZINK_SURF_WRITEBACK=dirty): called through
// the thunks with this thread's adopted context bound.
static void (*orig_gl_flush)(void), (*orig_gl_finish)(void), (*orig_gl_delete_textures)(GLsizei, const GLuint *);
static void (*orig_gl_fb_texture2d)(GLenum, GLenum, GLenum, GLuint, GLint);
static void wrap_flush(void) { orig_gl_flush(); if (t_current && !t_inside) { t_inside++; surftex_write_back(t_current, 1); offscreen_fill(t_current); t_inside--; } }
static void wrap_finish(void) { if (t_current && !t_inside) { t_inside++; surftex_write_back(t_current, 0); offscreen_fill(t_current); t_inside--; } orig_gl_finish(); }
// CGLZINK_SURF_WRITEBACK=dirty: which framebuffers hold an IOSurface texture (names stay listed after a delete or a
// detach: a stale name only costs a copy), and whether the draw framebuffer bound now is one of them.
static int is_surface_fbo(const Adopted *a, GLuint fbo) {
    for (int i = 0; fbo && i < a->n_surface_fbos; i++) if (a->surface_fbos[i] == fbo) return 1;
    return 0;
}
static void update_surface_fbo_bound(Adopted *a) {
    GLint fbo = 0;
    gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    a->surface_fbo_bound = a->n_surface_fbos > (int)(sizeof a->surface_fbos / sizeof *a->surface_fbos) ||
                           is_surface_fbo(a, (GLuint)fbo);
}
static void wrap_fb_texture2d(GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level) {
    orig_gl_fb_texture2d(target, attachment, textarget, texture, level);
    if (!texture || !t_current || !g_nsurftex) return;
    int attached = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nsurftex; i++)
        if (g_surftex[i].a == t_current && g_surftex[i].tex == texture) {
            attached = 1;
            if (g_surftex[i].drawn) continue;
            g_surftex[i].drawn = 1;
            LOG("%p: IOSurface texture %u is drawn into: copied back at each flush%s", (void *)t_current->cgl, texture,
                g_wb_dirty ? " that follows a draw into it" : "");
        }
    pthread_mutex_unlock(&g_lock);
    if (!attached || !g_wb_dirty) return;
    Adopted *a = t_current;
    GLint fbo = 0;
    gl.GetIntegerv(target == GL_READ_FRAMEBUFFER ? GL_READ_FRAMEBUFFER_BINDING : GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    // more than the list holds: every framebuffer counts from then on (n_surface_fbos past the end)
    if (fbo && !is_surface_fbo(a, (GLuint)fbo)) {
        if (a->n_surface_fbos < (int)(sizeof a->surface_fbos / sizeof *a->surface_fbos)) a->surface_fbos[a->n_surface_fbos] = (GLuint)fbo;
        if (a->n_surface_fbos <= (int)(sizeof a->surface_fbos / sizeof *a->surface_fbos)) a->n_surface_fbos++;
    }
    a->drew_surface = 1;   // attaching may follow a draw the list did not see yet
    update_surface_fbo_bound(a);
}
static void (*orig_gl_bind_framebuffer)(GLenum, GLuint);
static void wrap_bind_framebuffer(GLenum target, GLuint fbo) {
    orig_gl_bind_framebuffer(target, fbo);
    if (t_current && target != GL_READ_FRAMEBUFFER) t_current->surface_fbo_bound = t_current->n_surface_fbos > 0 &&
        (t_current->n_surface_fbos > (int)(sizeof t_current->surface_fbos / sizeof *t_current->surface_fbos) || is_surface_fbo(t_current, fbo));
}
// The entries that write pixels, for CGLZINK_SURF_WRITEBACK=dirty: 1 into the draw framebuffer, 2 into a texture.
static unsigned char g_mark_class[CGLT_N];
static int mark_class_of(const char *n) {
    static const char *const fb[] = { "glClear", "glClearBuffer", "glDraw", "glMultiDraw", "glBlitFramebuffer", "glCallList",
                                      "glRect", "glBitmap", "glCopyPixels", "glAccum", "glEvalMesh", "glEvalPoint", NULL };
    static const char *const tex[] = { "glTexImage", "glTexSubImage", "glCopyTex", "glCompressedTex", NULL };
    if (!strcmp(n, "glEnd")) return 1;   // immediate mode draws at glEnd
    if (!strcmp(n, "glDrawBuffer") || !strcmp(n, "glDrawBuffers") || (!strncmp(n, "glClear", 7) && strcmp(n, "glClear") &&
        strncmp(n, "glClearBuffer", 13))) return 0;   // state, not pixels (glClearColor, glClearDepth, ...)
    for (int i = 0; fb[i]; i++) if (!strncmp(n, fb[i], strlen(fb[i]))) return 1;
    for (int i = 0; tex[i]; i++) if (!strncmp(n, tex[i], strlen(tex[i]))) return 2;
    return 0;
}
static void mark_drew(int entry) {
    Adopted *a = t_current;
    if (!a) return;
    a->drew_any = 1;
    if (g_mark_class[entry] == 2 || a->surface_fbo_bound) a->drew_surface = 1;
}
static void wrap_delete_textures(GLsizei n, const GLuint *textures) {
    if (t_current && g_nsurftex && textures) {
        pthread_mutex_lock(&g_lock);
        for (GLsizei k = 0; k < n; k++)
            for (int i = g_nsurftex; i-- > 0; ) if (g_surftex[i].a == t_current && g_surftex[i].tex == textures[k]) surftex_drop_at(i);
        pthread_mutex_unlock(&g_lock);
    }
    orig_gl_delete_textures(n, textures);
}
static void wrap_entries(void) {
    for (int i = 0; i < CGLT_N; i++) {
        const char *name = cglt_names[i];
        if (!name || cglt_backend[i] == (void *)missing_entry) continue;
#define W(gname, orig, wrap) if (!strcmp(name, gname) && !orig) { *(void **)&orig = cglt_backend[i]; cglt_backend[i] = (void *)wrap; continue; }
        W("glFlush", orig_gl_flush, wrap_flush)
        W("glFinish", orig_gl_finish, wrap_finish)
        W("glFramebufferTexture2D", orig_gl_fb_texture2d, wrap_fb_texture2d)
        W("glDeleteTextures", orig_gl_delete_textures, wrap_delete_textures)
        if (g_wb_dirty) W("glBindFramebuffer", orig_gl_bind_framebuffer, wrap_bind_framebuffer)
#undef W
        if (g_wb_dirty && cglt_backend[i] != cglt_mark_table[i] && (g_mark_class[i] = (unsigned char)mark_class_of(name))) {
            cglt_marked[i] = cglt_backend[i];
            cglt_backend[i] = cglt_mark_table[i];
        }
    }
}

static CGLError tex_from_iosurface(Adopted *a, GLenum target, GLenum internal, GLsizei w, GLsizei h, GLenum format,
                                   GLenum type, IOSurfaceRef surface, GLuint plane) {
    gl_init();
    if (!gl.TexImage2D || !gl.PixelStorei || !gl.GetIntegerv || !gl.BindBuffer || !surface) return kCGLBadValue;
    const size_t planes = IOSurfaceGetPlaneCount(surface);
    if (planes && plane >= planes) return kCGLBadValue;
    if (format == 0x85B9 /* GL_YCBCR_422_APPLE */) {
        WARN_ONCE("CGLTexImageIOSurface2D with GL_YCBCR_422_APPLE is not supported through zink");
        return kCGLBadValue;
    }
    Adopted *previous = t_current;
    if (t_current != a || !t_bound) bind_context(a);
    t_inside++;
    IOSurfaceLock(surface, kIOSurfaceLockReadOnly, NULL);
    void *base; size_t row, element;
    surface_plane(surface, plane, &base, &row, &element);
    PixelState st; pixels_save(&st, 0, row, element);
    gl.TexImage2D(target, 0, (GLint)internal, w, h, 0, format, type, base);
    pixels_restore(&st);
    IOSurfaceUnlock(surface, kIOSurfaceLockReadOnly, NULL);
    surftex_note(a, target, surface, plane, format, type);
    t_inside--;
    if (previous != a) bind_context(previous);
    return kCGLNoError;
}

static CGLError my_CGLTexImageIOSurface2D(CGLContextObj cgl, GLenum target, GLenum internal, GLsizei w, GLsizei h,
                                          GLenum format, GLenum type, IOSurfaceRef surface, GLuint plane) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (a) return tex_from_iosurface(a, target, internal, w, h, format, type, surface, plane);
    return CGLTexImageIOSurface2D(cgl, target, internal, w, h, format, type, surface, plane);
}

// ---- CGLSetOffScreen ----
// The context draws into an EGL pbuffer of the given size; its pixels go into the application's memory at each glFlush,
// glFinish and CGLFlushDrawable, as 32-bit BGRA (GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, the layout of Apple's offscreen
// formats on little-endian Macs), rows rowbytes apart, in OpenGL's order (the first row in memory is the bottom one).
static void offscreen_fill(Adopted *a) {
    if (!a->offscreen_base || !gl.ReadPixels) return;
    if (g_wb_dirty && !a->drew_any) return;   // CGLZINK_SURF_WRITEBACK=dirty: nothing drawn since the last read
    a->drew_any = 0;
    PixelState st; pixels_save(&st, 1, (size_t)a->offscreen_rowbytes, 4);
    gl.ReadPixels(0, 0, a->offscreen_w, a->offscreen_h, 0x80E1 /* GL_BGRA */, 0x8367 /* GL_UNSIGNED_INT_8_8_8_8_REV */, a->offscreen_base);
    pixels_restore(&st);
}

// ---- pbuffers ----
// CGLSetPBuffer: the context draws into an EGL pbuffer surface of the pbuffer's size (made with the context's config).
// CGLTexImagePBuffer: the pbuffer's pixels are read from that surface and given to the target texture of the context
// that asks: a copy through memory, made when it is called, as Apple's documentation allows ("the texture's contents
// are undefined if the pbuffer is drawn into afterwards" holds here as well). Faces and levels other than 0 are not.
static void pbuffer_clear(Adopted *a) {
    a->offscreen_base = NULL;
    if (!a->pbuffer_surface) return;
    if (t_current == a && t_bound) { EGLSurface s = a->window ? a->window : a->offscreen; egl.MakeCurrent(g_display, s, s, a->context); }
    egl.DestroySurface(g_display, a->pbuffer_surface);
    a->pbuffer_surface = NULL;
    if (a->pbuffer) CGLReleasePBuffer(a->pbuffer);
    a->pbuffer = NULL;
}
static CGLError my_CGLSetPBuffer(CGLContextObj cgl, CGLPBufferObj pbuffer, GLenum face, GLint level, GLint screen) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLSetPBuffer(cgl, pbuffer, face, level, screen);
    GLsizei w = 0, h = 0; GLenum target = 0, internal = 0; GLint mipmap = 0;
    if (!pbuffer || CGLDescribePBuffer(pbuffer, &w, &h, &target, &internal, &mipmap) != kCGLNoError || w <= 0 || h <= 0) return kCGLBadValue;
    if (face || level) WARN_ONCE("CGLSetPBuffer with a cube face or a mipmap level other than 0: level 0 is drawn through zink");
    t_inside++;
    pbuffer_clear(a);
    const EGLint size[] = { EGL_WIDTH, w, EGL_HEIGHT, h, EGL_NONE };
    a->pbuffer_surface = egl.CreatePbufferSurface(g_display, a->config, size);
    if (!a->pbuffer_surface) { LOG("eglCreatePbufferSurface %dx%d: 0x%x", w, h, egl.GetError()); t_inside--; return kCGLBadAlloc; }
    a->pbuffer = CGLRetainPBuffer(pbuffer);
    if (t_current == a && t_bound) egl.MakeCurrent(g_display, a->pbuffer_surface, a->pbuffer_surface, a->context);
    LOG("%p draws into pbuffer %p (%dx%d)", (void *)cgl, (void *)pbuffer, w, h);
    t_inside--;
    return kCGLNoError;
}
static CGLError my_CGLTexImagePBuffer(CGLContextObj cgl, CGLPBufferObj pbuffer, GLenum source) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLTexImagePBuffer(cgl, pbuffer, source);
    // the context that draws into it
    Adopted *owner = NULL;
    pthread_mutex_lock(&g_lock);
    for (Adopted *o = g_adopted; o; o = o->next) if (!o->dead && o->pbuffer == pbuffer) { owner = o; break; }
    pthread_mutex_unlock(&g_lock);
    GLsizei w = 0, h = 0; GLenum target = 0, internal = 0; GLint mipmap = 0;
    if (!owner || CGLDescribePBuffer(pbuffer, &w, &h, &target, &internal, &mipmap) != kCGLNoError) {
        WARN_ONCE("CGLTexImagePBuffer for a pbuffer no context through zink draws into: the texture is left as it is");
        return kCGLBadValue;
    }
    gl_init();
    if (!gl.ReadPixels || !gl.TexImage2D) return kCGLBadValue;
    void *pixels = malloc((size_t)w * (size_t)h * 4);
    if (!pixels) return kCGLBadAlloc;
    Adopted *previous = t_current;
    t_inside++;
    if (owner != t_current || !t_bound) bind_context(owner);
    if (!t_bound) { t_inside--; free(pixels); if (previous != t_current) bind_context(previous); return kCGLBadContext; }
    // An EGL pbuffer has one colour buffer, its back buffer (EGL 1.5, 3.5.2), whichever of Apple's GL_FRONT / GL_BACK
    // the application names in `source`. The read framebuffer must be the surface's, not an FBO the owner left bound.
    (void)source;
    GLint read_fbo = 0; gl.GetIntegerv(0x8CAA /* GL_READ_FRAMEBUFFER_BINDING */, &read_fbo);
    static void (*BindFramebuffer)(GLenum, GLuint);
    if (!BindFramebuffer) *(void **)&BindFramebuffer = egl.GetProcAddress("glBindFramebuffer");
    if (read_fbo && BindFramebuffer) BindFramebuffer(0x8CA8 /* GL_READ_FRAMEBUFFER */, 0);
    PixelState st; pixels_save(&st, 1, (size_t)w * 4, 4);
    gl.ReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    pixels_restore(&st);
    if (read_fbo && BindFramebuffer) BindFramebuffer(0x8CA8, (GLuint)read_fbo);
    if (a != t_current || !t_bound) bind_context(a);
    PixelState su; pixels_save(&su, 0, (size_t)w * 4, 4);
    gl.TexImage2D(target ? target : GL_TEXTURE_2D, 0, (GLint)(internal ? internal : GL_RGBA), w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    pixels_restore(&su);
    t_inside--;
    if (previous != t_current) bind_context(previous);
    free(pixels);
    return kCGLNoError;
}

static void update_layer(Adopted *a);

static CGLError my_CGLClearDrawable(CGLContextObj cgl) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLClearDrawable(cgl);
    view_layer(a, nil);
    t_inside++; pbuffer_clear(a); t_inside--;
    return kCGLNoError;
}

static CGLError my_CGLUpdateContext(CGLContextObj cgl) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLUpdateContext(cgl);
    update_layer(a);
    return kCGLNoError;
}

static CGLError my_CGLSetOffScreen(CGLContextObj cgl, GLsizei width, GLsizei height, GLint rowbytes, void *base) {
    Adopted *a = t_inside ? NULL : find_cgl(cgl);
    if (!a) return CGLSetOffScreen(cgl, width, height, rowbytes, base);
    if (!base || width <= 0 || height <= 0 || rowbytes < width * 4) return kCGLBadValue;
    t_inside++;
    gl_init();
    pbuffer_clear(a);
    const EGLint size[] = { EGL_WIDTH, width, EGL_HEIGHT, height, EGL_NONE };
    a->pbuffer_surface = egl.CreatePbufferSurface(g_display, a->config, size);
    if (!a->pbuffer_surface) { LOG("eglCreatePbufferSurface %dx%d: 0x%x", width, height, egl.GetError()); t_inside--; return kCGLBadAlloc; }
    a->offscreen_base = base; a->offscreen_w = width; a->offscreen_h = height; a->offscreen_rowbytes = rowbytes;
    if (t_current == a && t_bound) egl.MakeCurrent(g_display, a->pbuffer_surface, a->pbuffer_surface, a->context);
    LOG("%p draws into application memory %dx%d (rowbytes %d)", (void *)cgl, width, height, rowbytes);
    t_inside--;
    return kCGLNoError;
}

static CGLError my_CGLCopyContext(CGLContextObj src, CGLContextObj dst, GLbitfield mask) {
    if (!t_inside && (find_cgl(src) || find_cgl(dst))) WARN_ONCE("CGLCopyContext is not supported through zink");
    return CGLCopyContext(src, dst, mask);
}

#define INTERPOSE(replacement, original) \
    __attribute__((used, section("__DATA,__interpose"))) static struct { const void *a, *b; } \
    interpose_##original = { (const void *)replacement, (const void *)original }
INTERPOSE(my_CGLCreateContext, CGLCreateContext);
INTERPOSE(my_CGLDestroyContext, CGLDestroyContext);
INTERPOSE(my_CGLReleaseContext, CGLReleaseContext);
INTERPOSE(my_CGLSetCurrentContext, CGLSetCurrentContext);
INTERPOSE(my_CGLLockContext, CGLLockContext);
INTERPOSE(my_CGLUnlockContext, CGLUnlockContext);
INTERPOSE(my_CGLFlushDrawable, CGLFlushDrawable);
INTERPOSE(my_CGLSetParameter, CGLSetParameter);
INTERPOSE(my_CGLTexImageIOSurface2D, CGLTexImageIOSurface2D);
INTERPOSE(my_CGLClearDrawable, CGLClearDrawable);
INTERPOSE(my_CGLUpdateContext, CGLUpdateContext);
INTERPOSE(my_CGLSetPBuffer, CGLSetPBuffer);
INTERPOSE(my_CGLTexImagePBuffer, CGLTexImagePBuffer);
INTERPOSE(my_CGLSetOffScreen, CGLSetOffScreen);
INTERPOSE(my_CGLCopyContext, CGLCopyContext);

// The same entry points for libcglzink-boot.dylib, which loads this library with dlopen (cglzink_hooks.h)
#include "cglzink_hooks.h"
__attribute__((visibility("default"))) const struct cglzink_hooks cglzink_hooks = {
    CGLZINK_HOOKS_VERSION, sizeof(struct cglzink_hooks),
    my_CGLCreateContext, my_CGLDestroyContext, my_CGLReleaseContext, my_CGLSetCurrentContext, my_CGLLockContext,
    my_CGLUnlockContext, my_CGLFlushDrawable, my_CGLSetParameter, my_CGLTexImageIOSurface2D, my_CGLClearDrawable,
    my_CGLUpdateContext, my_CGLSetPBuffer, my_CGLSetOffScreen, my_CGLCopyContext, my_CGLTexImagePBuffer,
};

// The layer's scale: an NSOpenGL view gets a Retina surface only when it asks for one (wantsBestResolutionOpenGLSurface,
// default NO); otherwise Apple renders at the view's size in points and scales up, and the application's viewport is in
// points. The backing scale is read from the view's own layer, which AppKit keeps right, also off the main thread.
static CGFloat layer_scale(Adopted *a) {
    CGFloat backing = a->layer.superlayer ? a->layer.superlayer.contentsScale : 1.0;
    return a->best_resolution && backing > 0 ? backing : 1.0;
}

// NSOpenGLContext's update / CGLUpdateContext: size and scale again. zink's platform makes the swapchain anew when
// bounds × contentsScale changes.
static void update_layer(Adopted *a) {
    CALayer *layer = a->layer;
    if (!layer || !layer.superlayer) return;
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    const CGRect bounds = layer.superlayer.bounds;
    const CGFloat scale = layer_scale(a);
    layer.frame = bounds;
    layer.contentsScale = scale;
    // as the prototype that ran Chess did: in case the Vulkan WSI presents into this layer itself. A fixed backing size
    // (kCGLCPSurfaceBackingSize while kCGLCESurfaceBackingSize is on, read from Apple's context, which keeps that state)
    // is the drawable's size whatever the view's; Core Animation scales it to the layer (contentsGravity resize).
    GLint fixed = 0;
    CGLIsEnabled(a->cgl, kCGLCESurfaceBackingSize, &fixed);
    ((CAMetalLayer *)layer).drawableSize = fixed && a->backing[0] > 0 && a->backing[1] > 0
        ? CGSizeMake(a->backing[0], a->backing[1]) : CGSizeMake(bounds.size.width * scale, bounds.size.height * scale);
    [CATransaction commit];
}

static void view_layer(Adopted *a, NSView *view) {
    if (view && view == a->view && a->window) return;   // NSOpenGLView sets its view again and again: the surface stays
    if (view) LOG("setView %p: %.0fx%.0f", (void *)view, view.bounds.size.width, view.bounds.size.height);
    a->view = view;
    t_inside++;
    if (a->window) {
        if (t_current == a && t_bound) egl.MakeCurrent(g_display, a->offscreen, a->offscreen, a->context);
        egl.DestroySurface(g_display, a->window);
        a->window = NULL;
        a->applied_interval = -1;
    }
    [a->layer removeFromSuperlayer];
    [a->layer release];
    a->layer = nil;
    if (view) {
        // The view presents through a layer of its own, as the EGL platform expects: EGLNativeWindowType is a CALayer.
        CAMetalLayer *layer = [CAMetalLayer layer];
        a->best_resolution = [view respondsToSelector:@selector(wantsBestResolutionOpenGLSurface)] &&
                             view.wantsBestResolutionOpenGLSurface;
        // A sublayer, not view.layer: a view whose layer is set is layer-hosting and AppKit stops calling its drawRect:,
        // which is where an NSOpenGLView draws.
        view.wantsLayer = YES;
        [view.layer addSublayer:layer];
        a->layer = [layer retain];
        layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
        update_layer(a);
        LOG("layer scale %.1f, the view %s a Retina surface", layer.contentsScale, a->best_resolution ? "asks for" : "does not ask for");
        a->window = egl.CreateWindowSurface(g_display, a->config, (void *)layer, NULL);
        if (!a->window) LOG("eglCreateWindowSurface 0x%x", egl.GetError());
        if (t_current == a && t_bound) {
            EGLSurface surface = draw_surface(a);
            egl.MakeCurrent(g_display, surface, surface, a->context);
            apply_interval(a);
        }
    }
    t_inside--;
}

// NSOpenGLContext. Its methods call CGL inside AppKit, which interposing reaches, but they are hooked as well: setView:
// and flushBuffer must not reach Apple's surface at all; adopt() and bind_context() do nothing the second time.
static IMP orig_init, orig_initCGL, orig_flush, orig_setView, orig_update, orig_clearDrawable, orig_makeCurrent, orig_clearCurrent;

static id hook_init(NSOpenGLContext *self, SEL cmd, NSOpenGLPixelFormat *format, NSOpenGLContext *share) {
    id made = ((id (*)(id, SEL, id, id))orig_init)(self, cmd, format, share);
    if (made && !t_inside)
        adopt(((NSOpenGLContext *)made).CGLContextObj, format.CGLPixelFormatObj, share.CGLContextObj);
    return made;
}

static id hook_initCGL(NSOpenGLContext *self, SEL cmd, void *cgl) {
    id made = ((id (*)(id, SEL, void *))orig_initCGL)(self, cmd, cgl);
    if (made && !t_inside) adopt(cgl, CGLGetPixelFormat(cgl), NULL);
    return made;
}

static void hook_flush(NSOpenGLContext *self, SEL cmd) {
    if (!t_inside) count_present();
    Adopted *a = t_inside ? NULL : find_cgl(self.CGLContextObj);
    if (a) { present(a); return; }
    t_inside++;   // AppKit's CGLFlushDrawable inside is not counted again
    ((void (*)(id, SEL))orig_flush)(self, cmd);
    t_inside--;
}

static void hook_setView(NSOpenGLContext *self, SEL cmd, NSView *view) {
    Adopted *a = t_inside ? NULL : find_cgl(self.CGLContextObj);
    // Apple's context gets no view: it would attach its own surface to the view we hand to EGL.
    if (a) view_layer(a, view); else ((void (*)(id, SEL, id))orig_setView)(self, cmd, view);
}

static void hook_update(NSOpenGLContext *self, SEL cmd) {
    Adopted *a = t_inside ? NULL : find_cgl(self.CGLContextObj);
    if (a) update_layer(a); else ((void (*)(id, SEL))orig_update)(self, cmd);
}

static void hook_clearDrawable(NSOpenGLContext *self, SEL cmd) {
    Adopted *a = t_inside ? NULL : find_cgl(self.CGLContextObj);
    if (a) view_layer(a, nil); else ((void (*)(id, SEL))orig_clearDrawable)(self, cmd);
}

static void hook_makeCurrent(NSOpenGLContext *self, SEL cmd) {
    ((void (*)(id, SEL))orig_makeCurrent)(self, cmd);
    Adopted *a = t_inside ? NULL : find_cgl(self.CGLContextObj);
    if (!t_inside && (a != t_current || (a && !t_bound))) bind_context(a);
}

static void hook_clearCurrent(id self, SEL cmd) {
    ((void (*)(id, SEL))orig_clearCurrent)(self, cmd);
    if (!t_inside && t_current) bind_context(NULL);
}

static IMP swap(Class c, SEL s, IMP replacement) {
    Method m = class_getInstanceMethod(c, s);
    return m ? method_setImplementation(m, replacement) : NULL;
}

// Whether class c itself (not a superclass) implements s.
static int defines(Class c, SEL s) {
    unsigned n = 0;
    Method *methods = class_copyMethodList(c, &n);
    int found = 0;
    for (unsigned i = 0; i < n && !found; i++) found = method_getName(methods[i]) == s;
    free(methods);
    return found;
}

// Core Animation's contexts (CAOpenGLLayer, NSOpenGLLayer and their subclasses) draw into IOSurfaces Apple's way: their
// context-making methods run with adoption off. Each class that defines one gets its own wrapper.
static void keep_on_apple(Class c, SEL s) {
    if (!c || !defines(c, s)) return;
    Method m = class_getInstanceMethod(c, s);
    IMP original = method_getImplementation(m);
    IMP wrapper = imp_implementationWithBlock(^void *(id self, void *format) {
        t_no_adopt++;
        void *made = ((void *(*)(id, SEL, void *))original)(self, s, format);
        t_no_adopt--;
        return made;
    });
    method_setImplementation(m, wrapper);
}

static int is_subclass(Class c, Class of) {
    for (; c; c = class_getSuperclass(c)) if (c == of) return 1;
    return 0;
}

// An application's images only: Apple's own subclasses are named in on_image, and asking for every class of the shared
// cache would realise thousands of them.
static void scan_layer_classes(const char *image) {
    Class layer = objc_getClass("CAOpenGLLayer");
    if (!layer || !image || _dyld_shared_cache_contains_path(image)) return;
    unsigned n = 0;
    const char **names = objc_copyClassNamesForImage(image, &n);
    for (unsigned i = 0; i < n; i++) {
        Class c = objc_getClass(names[i]);
        if (!is_subclass(c, layer)) continue;
        keep_on_apple(c, @selector(copyCGLContextForPixelFormat:));
        keep_on_apple(c, @selector(openGLContextForPixelFormat:));   // NSOpenGLLayer
    }
    free(names);
}

// AppKit and QuartzCore may load after this library, and an application's own CAOpenGLLayer classes with a bundle, so
// this runs as images arrive.
static void on_image(const struct mach_header *header, intptr_t slide) {
    static atomic_flag done;
    static _Atomic int ready;
    if (atomic_load(&ready)) {
        Dl_info info;
        if (dladdr(header, &info)) scan_layer_classes(info.dli_fname);
        return;
    }
    Class c = objc_getClass("NSOpenGLContext");
    if (!c || atomic_flag_test_and_set(&done)) return;
    // AppKit is here, so QuartzCore is: Apple's layer classes, every image so far once, then each new one as it comes
    keep_on_apple(objc_getClass("CAOpenGLLayer"), @selector(copyCGLContextForPixelFormat:));
    keep_on_apple(objc_getClass("NSOpenGLLayer"), @selector(copyCGLContextForPixelFormat:));
    keep_on_apple(objc_getClass("NSOpenGLLayer"), @selector(openGLContextForPixelFormat:));
    uint32_t n = _dyld_image_count();
    for (uint32_t i = 0; i < n; i++) scan_layer_classes(_dyld_get_image_name(i));
    atomic_store(&ready, 1);
    orig_init = swap(c, @selector(initWithFormat:shareContext:), (IMP)hook_init);
    orig_initCGL = swap(c, @selector(initWithCGLContextObj:), (IMP)hook_initCGL);
    orig_flush = swap(c, @selector(flushBuffer), (IMP)hook_flush);
    orig_setView = swap(c, @selector(setView:), (IMP)hook_setView);
    orig_update = swap(c, @selector(update), (IMP)hook_update);
    orig_clearDrawable = swap(c, @selector(clearDrawable), (IMP)hook_clearDrawable);
    orig_makeCurrent = swap(c, @selector(makeCurrentContext), (IMP)hook_makeCurrent);
    orig_clearCurrent = method_setImplementation(class_getClassMethod(c, @selector(clearCurrentContext)), (IMP)hook_clearCurrent);
}

__attribute__((constructor)) static void start(void) {
    if (getenv("CGLZINK_OFF")) return;
    g_log = getenv("CGLZINK_LOG") != NULL;
    g_fps_interval = getenv("CGLZINK_FPS") ? atof(getenv("CGLZINK_FPS")) : 0;
    g_apple_only = getenv("CGLZINK_APPLE") != NULL;
    const char *wb = getenv("CGLZINK_SURF_WRITEBACK");
    g_wb_dirty = !wb || !strstr(wb, "flush") || strstr(wb, "dirty") != NULL;   // dirty unless only flush is asked for
    g_wb_present = wb && strstr(wb, "present") != NULL;
    g_main_thread = (uintptr_t)pthread_self();   // an inserted library's constructor runs on the main thread
    _dyld_register_func_for_add_image(on_image);
}
