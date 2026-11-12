// SPDX-License-Identifier: MIT
// layertest: draws offscreen through CGL and NSOpenGLContext, in a core and a legacy context, from two threads, and prints a
// hash of the pixels. Run it alone and with DYLD_INSERT_LIBRARIES=libcglzink.dylib: the hashes must agree (run.sh).
// Also: a context's lifetime by reference count, shared contexts, one context handed between two threads, Apple's own
// extensions (APPLE_vertex_array_object, APPLE_fence, APPLE_element_array), an IOSurface texture, a CAOpenGLLayer's
// context (must stay on Apple's GL) and, with LAYERTEST_VIEW=1, a view in a window that is never shown.
#define GL_SILENCE_DEPRECATION
#import <AppKit/AppKit.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLContext.h>
#include <OpenGL/gl.h>
#include <OpenGL/glext.h>
#include <OpenGL/CGLIOSurface.h>
#include <IOSurface/IOSurface.h>
#import <QuartzCore/QuartzCore.h>
#include <dlfcn.h>
#include <stdatomic.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
static uint64_t fnv(const GLubyte *p, int n) { uint64_t h = 1469598103934665603ull; for (int i = 0; i < n; i++) h = (h ^ p[i]) * 1099511628211ull; return h; }
static const char *VS = "#version 120\nvarying vec2 uv; void main(){ uv = gl_MultiTexCoord0.xy; gl_Position = gl_Vertex; }\n";
static const char *FS = "#version 120\nvarying vec2 uv; uniform vec4 k; void main(){ gl_FragColor = vec4(uv, 0.25, 1.0) * k; }\n";
// The ARB handle entry points: pointer-sized handles on macOS, GLuint in the backend.
static uint64_t legacy_arb(void) {
    GLhandleARB v = glCreateShaderObjectARB(GL_VERTEX_SHADER_ARB), f = glCreateShaderObjectARB(GL_FRAGMENT_SHADER_ARB), p = glCreateProgramObjectARB();
    glShaderSourceARB(v, 1, &VS, 0); glCompileShaderARB(v); glShaderSourceARB(f, 1, &FS, 0); glCompileShaderARB(f);
    glAttachObjectARB(p, v); glAttachObjectARB(p, f); glLinkProgramARB(p);
    GLint ok = 0; glGetObjectParameterivARB(p, GL_OBJECT_LINK_STATUS_ARB, &ok); if (!ok) printf("link failed\n");
    GLhandleARB got[4] = {0}; GLsizei n = 0; glGetAttachedObjectsARB(p, 4, &n, got);
    printf("attached %d (%s)\n", n, n == 2 && ((got[0] == v && got[1] == f) || (got[0] == f && got[1] == v)) ? "same handles" : "DIFFERENT handles");
    GLuint fbo, rb; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb); glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 32, 32);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, 32, 32); glClearColor(0.25f, 0.5f, 0.75f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glUseProgramObjectARB(p); glUniform4fARB(glGetUniformLocationARB(p, "k"), 1, 0.5f, 1, 1);
    glBegin(GL_TRIANGLES); glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(1, 0); glVertex2f(3, -1); glTexCoord2f(0, 1); glVertex2f(-1, 3); glEnd();
    glFinish(); static GLubyte px[32 * 32 * 4]; glReadPixels(0, 0, 32, 32, GL_RGBA, GL_UNSIGNED_BYTE, px);
    if (glGetError()) printf("gl error\n");
    return fnv(px, sizeof px);
}
static uint64_t clear_only(float r) {
    GLuint fbo, rb; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glGenRenderbuffers(1, &rb); glBindRenderbuffer(GL_RENDERBUFFER, rb); glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, 16, 16);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, rb);
    glViewport(0, 0, 16, 16); glClearColor(r, 0.5f, 0.25f, 1); glClear(GL_COLOR_BUFFER_BIT); glFinish();
    static __thread GLubyte px[16 * 16 * 4]; glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    return fnv(px, sizeof px);
}

// What the layer says about a context, on stderr: the hashes on stdout stay comparable with Apple's GL.
static void adopted(const char *what, CGLContextObj c) {
    int (*f)(CGLContextObj) = (int (*)(CGLContextObj))dlsym(RTLD_DEFAULT, "cglzink_adopted");
    if (f) fprintf(stderr, "layertest: %s %s\n", what, f(c) ? "through the layer" : "on Apple's GL");
}
static CGLContextObj make(CGLOpenGLProfile profile, CGLContextObj share) {
    CGLPixelFormatAttribute a[] = { kCGLPFAAccelerated, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)profile, 0 };
    CGLPixelFormatObj pf; GLint np; CGLChoosePixelFormat(a, &pf, &np);
    CGLContextObj c = NULL; CGLCreateContext(pf, share, &c); CGLReleasePixelFormat(pf);
    return c;
}
// Reference counts: CGLDestroyContext with another reference held leaves a working context.
static void lifetime(void) {
    CGLContextObj c = make(kCGLOGLPVersion_GL3_Core, NULL);
    CGLRetainContext(c);
    CGLDestroyContext(c);
    adopted("after destroy with a reference held:", c);
    CGLSetCurrentContext(c);
    printf("lifetime  %016llx\n", clear_only(0.125f));
    CGLSetCurrentContext(NULL);
    CGLReleaseContext(c);
    CGLContextObj d = make(kCGLOGLPVersion_GL3_Core, NULL);
    adopted("a new context after the last release:", d);
    CGLDestroyContext(d);
}
// A texture made in one context, read through a framebuffer in another that shares with it.
static void shared(void) {
    CGLContextObj a = make(kCGLOGLPVersion_GL3_Core, NULL), b = make(kCGLOGLPVersion_GL3_Core, a);
    adopted("shared context:", b);
    CGLSetCurrentContext(a);
    GLubyte texels[4 * 4 * 4]; for (int i = 0; i < 64; i++) texels[i] = (GLubyte)(i * 37 + 11);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 4, 4, 0, GL_RGBA, GL_UNSIGNED_BYTE, texels); glFinish();
    CGLSetCurrentContext(b);
    GLuint fbo; glGenFramebuffers(1, &fbo); glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    GLubyte px[64] = {0}; glReadPixels(0, 0, 4, 4, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("shared    %016llx%s\n", fnv(px, sizeof px), memcmp(px, texels, 64) ? " DIFFERENT texels" : "");
    CGLSetCurrentContext(NULL); CGLDestroyContext(b); CGLDestroyContext(a);
}
// The CVDisplayLink pattern: the main thread made the context current and keeps it so, a render thread locks it and draws.
static CGLContextObj g_hand;
static atomic_int g_hand_done;
static uint64_t g_hand_hash;
static void *render_thread(void *unused) {
    CGLLockContext(g_hand); CGLSetCurrentContext(g_hand);
    g_hand_hash = clear_only(0.625f);
    CGLUnlockContext(g_hand); CGLSetCurrentContext(NULL);
    atomic_store(&g_hand_done, 1);
    return 0;
}
static void handoff(void) {
    g_hand = make(kCGLOGLPVersion_GL3_Core, NULL);
    CGLSetCurrentContext(g_hand);
    printf("hand main %016llx\n", clear_only(0.375f));
    pthread_t t; pthread_create(&t, 0, render_thread, 0);
    while (!atomic_load(&g_hand_done)) CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.01, true);   // the main queue runs here
    pthread_join(t, 0);
    printf("hand thr  %016llx\n", g_hand_hash);
    CGLLockContext(g_hand);
    printf("hand back %016llx\n", clear_only(0.875f));
    CGLUnlockContext(g_hand);
    CGLSetCurrentContext(NULL); CGLDestroyContext(g_hand);
}
// Apple's extensions in a 2.1 context, as an application written for the Mac uses them without asking.
static void apple_extensions(void) {
    CGLContextObj c = make(kCGLOGLPVersion_Legacy, NULL);
    CGLSetCurrentContext(c);
    GLuint fbo, rb; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb); glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 16, 16);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, 16, 16); glClearColor(0.1f, 0.2f, 0.3f, 1); glClear(GL_COLOR_BUFFER_BIT);
    static const GLfloat v[] = { -1, -1, 1, -1, 1, 1, -1, 1 };
    static const GLushort idx[] = { 9, 9, 0, 1, 2, 0, 2, 3 };   // the first two are skipped by "first"
    GLuint vao = 0; glGenVertexArraysAPPLE(1, &vao); glBindVertexArrayAPPLE(vao);
    glEnableClientState(GL_VERTEX_ARRAY); glVertexPointer(2, GL_FLOAT, 0, v);
    glEnableClientState(GL_ELEMENT_ARRAY_APPLE); glElementPointerAPPLE(GL_UNSIGNED_SHORT, idx);
    glColor4f(0.75f, 0.5f, 0.25f, 1);
    glDrawElementArrayAPPLE(GL_TRIANGLES, 2, 3);
    glColor4f(0.25f, 1, 0.5f, 1);
    glDrawRangeElementArrayAPPLE(GL_TRIANGLES, 0, 3, 5, 3);
    GLuint fence = 0; glGenFencesAPPLE(1, &fence);
    int before = glIsFenceAPPLE(fence);
    glSetFenceAPPLE(fence); glFinishFenceAPPLE(fence);
    int test = glTestFenceAPPLE(fence), is = glIsFenceAPPLE(fence);
    glDeleteFencesAPPLE(1, &fence);
    glBindVertexArrayAPPLE(0);
    int isvao = glIsVertexArrayAPPLE(vao); glDeleteVertexArraysAPPLE(1, &vao);
    glFlushRenderAPPLE(); glFinishRenderAPPLE();
    GLubyte px[16 * 16 * 4]; glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("apple ext %016llx fence %d/%d/%d\n", fnv(px, sizeof px), before, is, test);
    // Through the stand-in this is 0: the layer's glIsVertexArrayAPPLE is the core glIsVertexArray, which Apple's own 2.1
    // context (the stand-in's backend) does not have. zink's compatibility context does.
    fprintf(stderr, "layertest: glIsVertexArrayAPPLE %d\n", isvao);
    CGLSetCurrentContext(NULL); CGLDestroyContext(c);
}
// A rectangle texture from an IOSurface, drawn into a framebuffer: as CVOpenGLTextureCache gives video frames.
static void iosurface(void) {
    NSDictionary *props = @{ (id)kIOSurfaceWidth: @8, (id)kIOSurfaceHeight: @8, (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfaceBytesPerRow: @64, (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    IOSurfaceRef s = IOSurfaceCreate((CFDictionaryRef)props);
    IOSurfaceLock(s, 0, NULL);
    GLubyte *p = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s);
    for (int y = 0; y < 8; y++) for (int x = 0; x < 32; x++) p[y * row + x] = (GLubyte)(x * 7 + y * 29);
    IOSurfaceUnlock(s, 0, NULL);
    CGLContextObj c = make(kCGLOGLPVersion_Legacy, NULL);
    CGLSetCurrentContext(c);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    CGLError e = CGLTexImageIOSurface2D(c, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 8, 8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s, 0);
    glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_RECTANGLE_ARB, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo, rb; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb); glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 8, 8);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, 8, 8); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_RECTANGLE_ARB); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS); glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(8, 0); glVertex2f(1, -1); glTexCoord2f(8, 8); glVertex2f(1, 1); glTexCoord2f(0, 8); glVertex2f(-1, 1); glEnd();
    GLubyte px[8 * 8 * 4]; glReadPixels(0, 0, 8, 8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, px);
    int same = 1; for (int y = 0; y < 8; y++) same &= !memcmp(px + y * 32, p + y * row, 32);
    printf("iosurface %016llx error %d %s\n", fnv(px, sizeof px), e, same ? "texels equal the surface" : "texels DIFFER from the surface");
    CGLSetCurrentContext(NULL); CGLDestroyContext(c); CFRelease(s);
}
// Drawing INTO an IOSurface texture (an offscreen renderer handing frames to a CALayer or another process): the texture
// is a framebuffer's colour attachment; after glFlush the surface's own bytes must hold what was drawn.
static void iosurface_draw(void) {
    NSDictionary *props = @{ (id)kIOSurfaceWidth: @8, (id)kIOSurfaceHeight: @8, (id)kIOSurfaceBytesPerElement: @4,
                             (id)kIOSurfaceBytesPerRow: @64, (id)kIOSurfacePixelFormat: @((uint32_t)'BGRA') };
    IOSurfaceRef s = IOSurfaceCreate((CFDictionaryRef)props);
    CGLContextObj c = make(kCGLOGLPVersion_Legacy, NULL);
    CGLSetCurrentContext(c);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_RECTANGLE_ARB, tex);
    CGLError e = CGLTexImageIOSurface2D(c, GL_TEXTURE_RECTANGLE_ARB, GL_RGBA, 8, 8, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, s, 0);
    GLuint fbo; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glFramebufferTexture2DEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_TEXTURE_RECTANGLE_ARB, tex, 0);
    glViewport(0, 0, 8, 8); glClearColor(0.2f, 0.4f, 0.6f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glDisable(GL_TEXTURE_RECTANGLE_ARB);
    glBegin(GL_TRIANGLES); glColor3f(1, 0, 0); glVertex2f(-1, -1); glColor3f(0, 1, 0); glVertex2f(1, -1); glColor3f(0, 0, 1); glVertex2f(-1, 1); glEnd();
    glFlush();
    IOSurfaceLock(s, kIOSurfaceLockReadOnly, NULL);
    const GLubyte *p = IOSurfaceGetBaseAddress(s); size_t row = IOSurfaceGetBytesPerRow(s);
    GLubyte bytes[8 * 32]; for (int y = 0; y < 8; y++) memcpy(bytes + y * 32, p + y * row, 32);
    IOSurfaceUnlock(s, kIOSurfaceLockReadOnly, NULL);
    int blank = 1; for (size_t k = 0; k < sizeof bytes; k++) blank &= bytes[k] == 0;
    printf("iosurface draw %016llx error %d %s\n", fnv(bytes, sizeof bytes), e, blank ? "surface EMPTY" : "surface holds the drawing");
    glDeleteFramebuffersEXT(1, &fbo); glDeleteTextures(1, &tex);
    CGLSetCurrentContext(NULL); CGLDestroyContext(c); CFRelease(s);
}
// A pbuffer drawn by one context and made a texture in another (CGLSetPBuffer, CGLTexImagePBuffer). Only with
// LAYERTEST_PBUFFER=1: the stand-in EGL has no real pbuffer surfaces, so run.sh leaves it out; on the B580 compare a run
// on Apple's GL with one through the layer and zink.
static void pbuffer(void) {
    CGLPBufferObj pb = NULL;
    if (CGLCreatePBuffer(16, 16, GL_TEXTURE_2D, GL_RGBA, 0, &pb) != kCGLNoError) { printf("pbuffer: CGLCreatePBuffer failed\n"); return; }
    CGLContextObj drawer = make(kCGLOGLPVersion_Legacy, NULL), reader = make(kCGLOGLPVersion_Legacy, NULL);
    CGLError e1 = CGLSetPBuffer(drawer, pb, 0, 0, 0);
    CGLSetCurrentContext(drawer);
    glViewport(0, 0, 16, 16); glClearColor(0.3f, 0.6f, 0.9f, 1); glClear(GL_COLOR_BUFFER_BIT);
    glBegin(GL_TRIANGLES); glColor3f(1, 1, 0); glVertex2f(-1, -1); glVertex2f(1, -1); glVertex2f(-1, 1); glEnd();
    glFlush();
    CGLSetCurrentContext(reader);
    GLuint tex; glGenTextures(1, &tex); glBindTexture(GL_TEXTURE_2D, tex);
    CGLError e2 = CGLTexImagePBuffer(reader, pb, GL_FRONT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    GLuint fbo, rb; glGenFramebuffersEXT(1, &fbo); glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, fbo);
    glGenRenderbuffersEXT(1, &rb); glBindRenderbufferEXT(GL_RENDERBUFFER_EXT, rb); glRenderbufferStorageEXT(GL_RENDERBUFFER_EXT, GL_RGBA8, 16, 16);
    glFramebufferRenderbufferEXT(GL_FRAMEBUFFER_EXT, GL_COLOR_ATTACHMENT0_EXT, GL_RENDERBUFFER_EXT, rb);
    glViewport(0, 0, 16, 16); glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glBegin(GL_QUADS); glTexCoord2f(0, 0); glVertex2f(-1, -1); glTexCoord2f(1, 0); glVertex2f(1, -1); glTexCoord2f(1, 1); glVertex2f(1, 1); glTexCoord2f(0, 1); glVertex2f(-1, 1); glEnd();
    GLubyte px[16 * 16 * 4]; glReadPixels(0, 0, 16, 16, GL_RGBA, GL_UNSIGNED_BYTE, px);
    printf("pbuffer %016llx errors %d/%d corner %u,%u,%u center %u,%u,%u\n", fnv(px, sizeof px), e1, e2,
           px[0], px[1], px[2], px[(8 * 16 + 12) * 4], px[(8 * 16 + 12) * 4 + 1], px[(8 * 16 + 12) * 4 + 2]);
    CGLSetCurrentContext(NULL); CGLDestroyContext(drawer); CGLDestroyContext(reader); CGLReleasePBuffer(pb);
}
// Core Animation's own context for a CAOpenGLLayer: must stay on Apple's GL.
static void calayer(void) {
    CAOpenGLLayer *layer = [CAOpenGLLayer layer];
    CGLPixelFormatObj pf = [layer copyCGLPixelFormatForDisplayMask:~0u];
    CGLContextObj c = [layer copyCGLContextForPixelFormat:pf];
    adopted("CAOpenGLLayer context:", c);
    [layer releaseCGLContext:c]; [layer releaseCGLPixelFormat:pf];
}
// A view in a window that is never shown: the layer's scale, the swap interval it takes from Apple's context (the fake EGL
// reports the last one), a present.
static void view(void) {
    [NSApplication sharedApplication];
    NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 160, 120) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
    for (int best = 0; best < 2; best++) {
        NSOpenGLPixelFormatAttribute na[] = { NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer, 0 };
        NSOpenGLContext *ns = [[NSOpenGLContext alloc] initWithFormat:[[NSOpenGLPixelFormat alloc] initWithAttributes:na] shareContext:nil];
        w.contentView.wantsBestResolutionOpenGLSurface = best;
        [ns setView:w.contentView]; [ns makeCurrentContext];
        glClearColor(0.5f, 0.5f, 0.5f, 1); glClear(GL_COLOR_BUFFER_BIT); [ns flushBuffer];
        CALayer *sub = w.contentView.layer.sublayers.lastObject;
        fprintf(stderr, "layertest: window scale %.1f, best resolution %d: %s scale %.1f\n", w.backingScaleFactor, best,
                [sub isKindOfClass:[CAMetalLayer class]] ? "layer" : "no layer of ours,", sub.contentsScale);
        [ns clearDrawable]; [NSOpenGLContext clearCurrentContext];
    }
}

// What a context says about itself must be Apple's for its profile (4.1 core / 2.1 legacy, Apple's extensions), whatever
// draws: an application resolves functions from OpenGL.framework, which has nothing past 4.1 (GZDoom's NULL glBufferStorage).
// gl3.h's names; this file includes gl.h for the 2.1 parts
#define GL_MAJOR_VERSION 0x821B
#define GL_MINOR_VERSION 0x821C
#define GL_NUM_EXTENSIONS 0x821D
extern const GLubyte *glGetStringi(GLenum name, GLuint index);
static void strings(const char *label, CGLOpenGLProfile profile) {
    CGLContextObj c = make(profile, NULL);
    CGLSetCurrentContext(c);
    const char *version = (const char *)glGetString(GL_VERSION), *glsl = (const char *)glGetString(GL_SHADING_LANGUAGE_VERSION);
    int vmaj = 0, vmin = 0; sscanf(version ? version : "", "%d.%d", &vmaj, &vmin);
    uint64_t h = 1469598103934665603ull; int n = 0, bad = 0;
    const char *problem = "";
    if (profile != kCGLOGLPVersion_Legacy) {
        GLint major = 0, minor = 0, count = 0;
        glGetIntegerv(GL_MAJOR_VERSION, &major); glGetIntegerv(GL_MINOR_VERSION, &minor); glGetIntegerv(GL_NUM_EXTENSIONS, &count);
        if (major != vmaj || minor != vmin) problem = " MAJOR/MINOR DIFFER from GL_VERSION";
        for (GLint i = 0; i < count; i++) {
            const char *e = (const char *)glGetStringi(GL_EXTENSIONS, (GLuint)i);
            if (!e) { problem = " NULL extension"; continue; }
            n++; h = fnv((const GLubyte *)e, (int)strlen(e)) ^ (h * 31);
            bad += !strcmp(e, "GL_ARB_buffer_storage");
        }
        if (glGetString(GL_EXTENSIONS)) problem = " GL_EXTENSIONS string in a core context";
    } else {
        const char *all = (const char *)glGetString(GL_EXTENSIONS);
        char *copy = strdup(all ? all : ""), *save = NULL;
        for (char *t = strtok_r(copy, " ", &save); t; t = strtok_r(NULL, " ", &save)) {
            n++; h = fnv((const GLubyte *)t, (int)strlen(t)) ^ (h * 31);
            bad += !strcmp(t, "GL_ARB_buffer_storage");
        }
        free(copy);
    }
    while (glGetError()) {}
    if (bad) problem = " ADVERTISES GL_ARB_buffer_storage";
    if (vmaj > 4 || (vmaj == 4 && vmin > 1)) problem = " VERSION ABOVE 4.1";
    printf("strings %-6s %s | GLSL %s | %d extensions %016llx%s\n", label, version, glsl, n, h, *problem ? problem : " consistent");
    CGLSetCurrentContext(NULL); CGLDestroyContext(c);
}
static void no_late_functions(void) {
    // past Apple's 4.1 (glTexStorage2D is not: Apple has ARB_texture_storage and exports it)
    const char *late[] = { "glBufferStorage", "glBindImageTexture", "glDispatchCompute", "glClearTexImage" };
    printf("dlsym 4.2+ functions:");
    int found = 0; for (int i = 0; i < 4; i++) if (dlsym(RTLD_DEFAULT, late[i])) { printf(" %s", late[i]); found++; }
    printf("%s\n", found ? " FOUND" : " none");
}
static CGLContextObj g_core;
static uint64_t g_thread_hash;
static void *other_thread(void *a) { CGLSetCurrentContext(g_core); g_thread_hash = clear_only(0.75f); CGLSetCurrentContext(NULL); return 0; }
int main(void) { @autoreleasepool {
    // CGL, core profile
    CGLPixelFormatAttribute a3[] = { kCGLPFAAccelerated, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL3_Core, 0 };
    CGLPixelFormatObj pf; GLint np; CGLChoosePixelFormat(a3, &pf, &np);
    CGLCreateContext(pf, NULL, &g_core); CGLSetCurrentContext(g_core);
    printf("cgl core  %016llx\n", clear_only(0.25f));
    CGLFlushDrawable(g_core);
    CGLSetCurrentContext(NULL);
    pthread_t t; pthread_create(&t, 0, other_thread, 0); pthread_join(t, 0);
    printf("thread    %016llx\n", g_thread_hash);
    // NSOpenGLContext, legacy profile, with the ARB handle functions
    // Double-buffered: a window has no front-buffer drawing any more, and a single-buffered context stays black.
    NSOpenGLPixelFormatAttribute na[] = { NSOpenGLPFAAccelerated, NSOpenGLPFADoubleBuffer, 0 };
    NSOpenGLPixelFormat *format = [[NSOpenGLPixelFormat alloc] initWithAttributes:na];
    NSOpenGLContext *ns = [[NSOpenGLContext alloc] initWithFormat:format shareContext:nil];
    [ns makeCurrentContext];
    printf("nsgl arb  %016llx\n", legacy_arb());
    [ns flushBuffer];
    GLint i = 1; [ns setValues:&i forParameter:NSOpenGLContextParameterSwapInterval];
    // A real window, when asked: a triangle over a changing colour for LAYERTEST_WINDOW seconds, and the frame rate.
    if (getenv("LAYERTEST_WINDOW")) {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        double x = getenv("LAYERTEST_X") ? atof(getenv("LAYERTEST_X")) : 200;
        NSWindow *w = [[NSWindow alloc] initWithContentRect:NSMakeRect(x, 300, 480, 320) styleMask:NSWindowStyleMaskTitled backing:NSBackingStoreBuffered defer:NO];
        w.title = getenv("LAYERTEST_TITLE") ? @(getenv("LAYERTEST_TITLE")) : @"cglzink test";
        [w makeKeyAndOrderFront:nil];
        [NSApp activateIgnoringOtherApps:YES];
        [ns setView:w.contentView]; [ns update]; [ns makeCurrentContext];
        GLint interval = getenv("LAYERTEST_INTERVAL") ? atoi(getenv("LAYERTEST_INTERVAL")) : 0;
        [ns setValues:&interval forParameter:NSOpenGLContextParameterSwapInterval];
        printf("scale %.1f, view %.0fx%.0f, interval %d\n", w.backingScaleFactor, w.contentView.bounds.size.width, w.contentView.bounds.size.height, interval);
        glBindFramebufferEXT(GL_FRAMEBUFFER_EXT, 0); glUseProgramObjectARB(0);
        double seconds = atof(getenv("LAYERTEST_WINDOW")), start = CFAbsoluteTimeGetCurrent(), t = 0; long frames = 0;
        printf("renderer %s | %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        while ((t = CFAbsoluteTimeGetCurrent() - start) < seconds) {
            @autoreleasepool {
                NSEvent *e; while ((e = [NSApp nextEventMatchingMask:NSEventMaskAny untilDate:nil inMode:NSDefaultRunLoopMode dequeue:YES])) [NSApp sendEvent:e];
                glViewport(0, 0, (GLsizei)w.contentView.bounds.size.width, (GLsizei)w.contentView.bounds.size.height);
                glClearColor(0.5f + 0.5f * sin(t), 0.2f, 0.5f - 0.5f * sin(t), 1); glClear(GL_COLOR_BUFFER_BIT);
                glBegin(GL_TRIANGLES); glColor3f(1, 0, 0); glVertex2f(-0.6f, -0.6f); glColor3f(0, 1, 0); glVertex2f(0.6f, -0.6f); glColor3f(0, 0, 1); glVertex2f(0, 0.7f); glEnd();
                [ns flushBuffer]; frames++;
            }
        }
        printf("window: %ld frames, %.0f fps, gl error 0x%x\n", frames, frames / t, glGetError());
        [ns clearDrawable];
    }
    // CGLMacro.h style: the entry called directly for a context that is not current
    [NSOpenGLContext clearCurrentContext];
    CGLContextObj c = ns.CGLContextObj;
    GLfloat clear[4] = {0};
    c->disp.get_floatv(c->rend, GL_COLOR_CLEAR_VALUE, clear);
    printf("direct    %.2f %.2f %.2f\n", clear[0], clear[1], clear[2]);
    CGLDestroyContext(g_core);
    lifetime();
    shared();
    handoff();
    apple_extensions();
    iosurface();
    iosurface_draw();
    if (getenv("LAYERTEST_PBUFFER")) pbuffer();
    calayer();
    strings("core4", kCGLOGLPVersion_GL4_Core);
    strings("core3", kCGLOGLPVersion_GL3_Core);
    strings("legacy", kCGLOGLPVersion_Legacy);
    no_late_functions();
    if (getenv("LAYERTEST_VIEW")) view();
    printf("done\n");
    return 0;
} }
