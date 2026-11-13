// SPDX-License-Identifier: MIT
// zgbench: a deferred-renderer-shaped OpenGL frame for measuring zink's GPU work per frame (docs/design/DESIGN-zink-gpu-gap.md).
// Not a game: the passes a shader-pack frame has, sized by flags. Per frame:
//   uploads   instance data into a 16 MiB arena (-u subdata|orphan|mapinv|persist|none, -U KiB per frame) and two
//             uniform buffers (4 KiB and 16 KiB, glBufferSubData)
//   shadow    depth-only pass into an SxS depth texture (-d 24|32f|24s8), instanced boxes from the arena
//   gbuffer   3 colour targets (RGBA8, RGBA16F, RGBA8) + depth, cleared, the same boxes, an atlas texture sampled
//   composite -p full-screen passes, ping-pong RGBA16F, each sampling the G-buffer, the shadow map (compare) and the
//             previous pass; with -m the middle one's output gets glGenerateMipmap and the next one reads its levels
//   final     to the default framebuffer, then present (GLFW window, swap interval 0) or nothing (-e: EGL pbuffer;
//             -a: Apple's OpenGL through CGL, offscreen, into an RGBA8 framebuffer of its own: the same frame on Apple's
//             GL, and a way to check this program on a Mac without zink)
// Prints one "zgbench:" line: fps, CPU ms per frame, GPU ms per frame (GL_TIME_ELAPSED, -q 1 per frame, -q 2 per pass,
// read 3 frames late so they do not stall), and an image hash of a fixed frame rendered at the end: an A/B knob must
// leave it unchanged.
//   zgbench [-e|-a] [-W w] [-H h] [-s seconds] [-p passes] [-m 0|1] [-u mode] [-U KiB] [-d depth] [-S size] [-q 0|1|2] [-n boxes]
//           [-i] [-f flushes]
// -a -i: the final pass draws into an IOSurface (CGLTexImageIOSurface2D, as an offscreen renderer handing frames to a
// CALayer or another process); -f 0-3 adds glFlush after the shadow, G-buffer and composite passes; the frame ends with
// glFlush. Through libcglzink this is the case of its IOSurface copy-back (CGLZINK_SURF_WRITEBACK).
// Libraries: ZGB_GLFW (default /Library/IntelXe/GL/libglfw.3.dylib), ZGB_EGL (default /Library/IntelXe/GL/libEGL.1.dylib).
#define GL_SILENCE_DEPRECATION
#include <OpenGL/gl3.h>
#include <OpenGL/OpenGL.h>
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <IOSurface/IOSurface.h>
#include <OpenGL/CGLIOSurface.h>
#include <CoreFoundation/CoreFoundation.h>
#include <unistd.h>

#define GL_MAP_PERSISTENT_BIT 0x0040
#define GL_MAP_COHERENT_BIT 0x0080
#define GL_DYNAMIC_STORAGE_BIT 0x0100
#define GL_CLIENT_STORAGE_BIT 0x0200

#define GLFUNCS \
    F(const GLubyte *, glGetString, (GLenum)) F(GLenum, glGetError, (void)) F(void, glFinish, (void)) F(void, glFlush, (void)) \
    F(void, glClear, (GLbitfield)) F(void, glClearColor, (GLfloat, GLfloat, GLfloat, GLfloat)) F(void, glClearDepth, (GLdouble)) \
    F(void, glViewport, (GLint, GLint, GLsizei, GLsizei)) F(void, glEnable, (GLenum)) F(void, glDisable, (GLenum)) \
    F(void, glDepthFunc, (GLenum)) F(void, glColorMask, (GLboolean, GLboolean, GLboolean, GLboolean)) \
    F(void, glGenVertexArrays, (GLsizei, GLuint *)) F(void, glBindVertexArray, (GLuint)) \
    F(void, glGenBuffers, (GLsizei, GLuint *)) F(void, glBindBuffer, (GLenum, GLuint)) \
    F(void, glBufferData, (GLenum, GLsizeiptr, const void *, GLenum)) F(void, glBufferSubData, (GLenum, GLintptr, GLsizeiptr, const void *)) \
    F(void *, glMapBufferRange, (GLenum, GLintptr, GLsizeiptr, GLbitfield)) F(GLboolean, glUnmapBuffer, (GLenum)) \
    F(void, glCopyBufferSubData, (GLenum, GLenum, GLintptr, GLintptr, GLsizeiptr)) F(void, glBindBufferBase, (GLenum, GLuint, GLuint)) \
    F(void, glVertexAttribPointer, (GLuint, GLint, GLenum, GLboolean, GLsizei, const void *)) F(void, glEnableVertexAttribArray, (GLuint)) \
    F(void, glVertexAttribDivisor, (GLuint, GLuint)) \
    F(GLuint, glCreateShader, (GLenum)) F(void, glShaderSource, (GLuint, GLsizei, const GLchar *const *, const GLint *)) \
    F(void, glCompileShader, (GLuint)) F(void, glGetShaderiv, (GLuint, GLenum, GLint *)) F(void, glGetShaderInfoLog, (GLuint, GLsizei, GLsizei *, GLchar *)) \
    F(GLuint, glCreateProgram, (void)) F(void, glAttachShader, (GLuint, GLuint)) F(void, glLinkProgram, (GLuint)) \
    F(void, glGetProgramiv, (GLuint, GLenum, GLint *)) F(void, glUseProgram, (GLuint)) F(GLint, glGetUniformLocation, (GLuint, const GLchar *)) \
    F(GLuint, glGetUniformBlockIndex, (GLuint, const GLchar *)) F(void, glUniformBlockBinding, (GLuint, GLuint, GLuint)) \
    F(void, glUniform1i, (GLint, GLint)) F(void, glUniform1f, (GLint, GLfloat)) F(void, glUniform2f, (GLint, GLfloat, GLfloat)) \
    F(void, glDrawArrays, (GLenum, GLint, GLsizei)) F(void, glDrawArraysInstanced, (GLenum, GLint, GLsizei, GLsizei)) \
    F(void, glGenTextures, (GLsizei, GLuint *)) F(void, glBindTexture, (GLenum, GLuint)) F(void, glActiveTexture, (GLenum)) \
    F(void, glTexImage2D, (GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *)) \
    F(void, glTexParameteri, (GLenum, GLenum, GLint)) F(void, glGenerateMipmap, (GLenum)) \
    F(void, glGenFramebuffers, (GLsizei, GLuint *)) F(void, glBindFramebuffer, (GLenum, GLuint)) \
    F(void, glFramebufferTexture2D, (GLenum, GLenum, GLenum, GLuint, GLint)) F(GLenum, glCheckFramebufferStatus, (GLenum)) \
    F(void, glDrawBuffers, (GLsizei, const GLenum *)) F(void, glReadPixels, (GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *)) \
    F(void, glGenQueries, (GLsizei, GLuint *)) F(void, glBeginQuery, (GLenum, GLuint)) F(void, glEndQuery, (GLenum)) \
    F(void, glGetQueryObjectui64v, (GLuint, GLenum, GLuint64 *)) \
    F(GLsync, glFenceSync, (GLenum, GLbitfield)) F(GLenum, glClientWaitSync, (GLsync, GLbitfield, GLuint64)) F(void, glDeleteSync, (GLsync))
#define F(ret, name, args) static ret(*p_##name) args;
GLFUNCS
#undef F
static void (*p_glBufferStorage)(GLenum, GLsizeiptr, const void *, GLbitfield);   // GL 4.4, not in gl3.h

// GLFW, declared here (the package's patched libglfw.3.dylib is loaded at run time)
typedef struct GLFWwindow GLFWwindow;
static struct {
    int (*Init)(void); void (*WindowHint)(int, int); GLFWwindow *(*CreateWindow)(int, int, const char *, void *, void *);
    void (*MakeContextCurrent)(GLFWwindow *); void (*SwapInterval)(int); void (*SwapBuffers)(GLFWwindow *);
    void (*PollEvents)(void); void *(*GetProcAddress)(const char *); void (*GetFramebufferSize)(GLFWwindow *, int *, int *);
    void (*Terminate)(void);
} glfw;
// EGL (surfaceless pbuffer), declared here too
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
static struct {
    void *(*GetProcAddress)(const char *); EGLDisplay (*GetPlatformDisplay)(unsigned, void *, const intptr_t *);
    unsigned (*Initialize)(EGLDisplay, int *, int *); unsigned (*BindAPI)(unsigned);
    unsigned (*ChooseConfig)(EGLDisplay, const int *, EGLConfig *, int, int *);
    EGLContext (*CreateContext)(EGLDisplay, EGLConfig, EGLContext, const int *);
    EGLSurface (*CreatePbufferSurface)(EGLDisplay, EGLConfig, const int *);
    unsigned (*MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext); int (*GetError)(void);
} egl;

static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec * 1e-9; }
static void die(const char *what) { fprintf(stderr, "zgbench: %s\n", what); exit(1); }

static GLuint shader(GLenum type, const char *src) {
    GLuint s = p_glCreateShader(type); p_glShaderSource(s, 1, &src, NULL); p_glCompileShader(s);
    GLint ok = 0; p_glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[4096]; p_glGetShaderInfoLog(s, sizeof log, NULL, log); fprintf(stderr, "zgbench: shader: %s\n", log); exit(1); }
    return s;
}
static GLuint program(const char *vs, const char *fs) {
    GLuint p = p_glCreateProgram(); p_glAttachShader(p, shader(GL_VERTEX_SHADER, vs)); p_glAttachShader(p, shader(GL_FRAGMENT_SHADER, fs));
    p_glLinkProgram(p); GLint ok = 0; p_glGetProgramiv(p, GL_LINK_STATUS, &ok); if (!ok) die("link");
    return p;
}
static GLuint texture(GLenum internal, GLenum format, GLenum type, int w, int h, int mips) {
    GLuint t; p_glGenTextures(1, &t); p_glBindTexture(GL_TEXTURE_2D, t);
    p_glTexImage2D(GL_TEXTURE_2D, 0, (GLint)internal, w, h, 0, format, type, NULL);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, mips ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (mips) p_glGenerateMipmap(GL_TEXTURE_2D);
    return t;
}
static GLuint fbo(int ncolor, const GLuint *color, GLuint depth, GLenum depth_attachment) {
    GLuint f; p_glGenFramebuffers(1, &f); p_glBindFramebuffer(GL_FRAMEBUFFER, f);
    GLenum bufs[4];
    for (int i = 0; i < ncolor; i++) { p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0 + i, GL_TEXTURE_2D, color[i], 0); bufs[i] = GL_COLOR_ATTACHMENT0 + i; }
    if (depth) p_glFramebufferTexture2D(GL_FRAMEBUFFER, depth_attachment, GL_TEXTURE_2D, depth, 0);
    if (ncolor) p_glDrawBuffers(ncolor, bufs); else { GLenum none = GL_NONE; p_glDrawBuffers(1, &none); }
    if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("framebuffer incomplete");
    return f;
}

enum { Q_UPLOAD, Q_SHADOW, Q_GBUF, Q_COMP, Q_FINAL, Q_N };
static const char *qname[Q_N] = { "upload", "shadow", "gbuffer", "composite", "final" };
enum { RING = 4 };
#define ARENA (16u << 20)

// settings and objects, shared by main() and the frame
static GLuint final_fbo;   // -a: the final pass's target (CGL without a drawable has no default framebuffer)
static int mid_flushes;    // -f: glFlush after the first N of shadow, G-buffer, composite
static int W = 1920, H = 1080, passes = 8, mips = 1, upload_kib = 2048, shadow_size = 2048, queries = 1, boxes = 32768, fw, fh;
static const char *umode = "subdata";
static size_t up, upload_at;
static const size_t piece = 64 * 1024;
static float *inst, ubo_data[4096];
static void *persist;
// -u persist: one fence per third of the staging ring, waited for before the CPU writes that third again (as Sodium
// does). Without it the CPU overwrote data a queued copy had not read yet (undefined in GL, and the image hash varied).
static GLsync persist_fence[3];
static GLuint shadow_tex, shadow_fbo, g[3], g_fbo, pp[2], pp_fbo[2], atlas_tex, vao, empty_vao, arena, staging, orphan_vbo, ubo[2];
static GLuint box_prog, shadow_prog, comp_prog, final_prog, q[RING][Q_N];
static GLint comp_k, comp_lod;
static int qdone[RING];
static double gpu_sum[Q_N], gpu_frame_sum;
static long gpu_frames;

#define QB(i) do { if (queries >= 2 || (queries == 1 && (i) == Q_UPLOAD)) p_glBeginQuery(GL_TIME_ELAPSED, q[slot][i]); } while (0)
#define QE(i) do { if (queries >= 2 || (queries == 1 && (i) == Q_FINAL)) p_glEndQuery(GL_TIME_ELAPSED); } while (0)
// One frame. With -q 1 a single GL_TIME_ELAPSED (q[slot][Q_UPLOAD]) spans it, with -q 2 one per pass.
static void draw_frame(unsigned long f, int slot) {
    const float t = (float)(f % 3600) / 60.0f;
    // uploads
    QB(Q_UPLOAD);
    ubo_data[0] = 0.4f + 0.1f * t; ubo_data[1] = 1.0f; ubo_data[2] = 0.3f; ubo_data[4] = 0.3f + t * 0.01f;
    p_glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]); p_glBufferSubData(GL_UNIFORM_BUFFER, 0, 4096, ubo_data);
    for (int i = 0; i < 1024; i++) { ubo_data[i * 4] = 0.5f + 0.5f * (float)((i * 37 + f) % 64) / 64.0f; ubo_data[i * 4 + 1] = 0.8f; ubo_data[i * 4 + 2] = 0.7f; ubo_data[i * 4 + 3] = 1; }
    p_glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]); p_glBufferSubData(GL_UNIFORM_BUFFER, 0, 16384, ubo_data);
    ubo_data[0] = 0.4f + 0.1f * t; ubo_data[1] = 1.0f; ubo_data[2] = 0.3f; ubo_data[3] = 0; ubo_data[4] = 0.3f + t * 0.01f; ubo_data[5] = 0; ubo_data[6] = 0; ubo_data[7] = 0;
    p_glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]); p_glBufferSubData(GL_UNIFORM_BUFFER, 0, 32, ubo_data);
    for (size_t done = 0; done < up && strcmp(umode, "none"); done += piece) {
        const size_t n = up - done < piece ? up - done : piece;
        if (upload_at + n > ARENA) upload_at = 0;
        const void *src = (const char *)inst + upload_at;
        if (!strcmp(umode, "subdata")) { p_glBindBuffer(GL_ARRAY_BUFFER, arena); p_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)upload_at, (GLsizeiptr)n, src); }
        else if (!strcmp(umode, "mapinv")) {
            p_glBindBuffer(GL_ARRAY_BUFFER, arena);
            void *m = p_glMapBufferRange(GL_ARRAY_BUFFER, (GLintptr)upload_at, (GLsizeiptr)n, GL_MAP_WRITE_BIT | GL_MAP_INVALIDATE_RANGE_BIT);
            if (m) { memcpy(m, src, n); p_glUnmapBuffer(GL_ARRAY_BUFFER); }
        } else if (!strcmp(umode, "persist")) {
            const size_t off = (f % 3) * up + done;
            if (done == 0 && persist_fence[f % 3]) {
                p_glClientWaitSync(persist_fence[f % 3], GL_SYNC_FLUSH_COMMANDS_BIT, 2000000000ull);
                p_glDeleteSync(persist_fence[f % 3]); persist_fence[f % 3] = NULL;
            }
            memcpy((char *)persist + off, src, n);
            p_glBindBuffer(GL_COPY_READ_BUFFER, staging); p_glBindBuffer(GL_COPY_WRITE_BUFFER, arena);
            p_glCopyBufferSubData(GL_COPY_READ_BUFFER, GL_COPY_WRITE_BUFFER, (GLintptr)off, (GLintptr)upload_at, (GLsizeiptr)n);
        } else if (!strcmp(umode, "orphan")) {
            if (done == 0) { p_glBindBuffer(GL_ARRAY_BUFFER, orphan_vbo); p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)up, NULL, GL_STREAM_DRAW); }
            p_glBindBuffer(GL_ARRAY_BUFFER, orphan_vbo); p_glBufferSubData(GL_ARRAY_BUFFER, (GLintptr)done, (GLsizeiptr)n, src);
        }
        upload_at += n;
    }
    if (!strcmp(umode, "persist") && up) persist_fence[f % 3] = p_glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    QE(Q_UPLOAD);
    p_glBindVertexArray(vao); p_glBindBuffer(GL_ARRAY_BUFFER, arena);
    p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0); p_glEnableVertexAttribArray(1); p_glVertexAttribDivisor(1, 1);
    if (orphan_vbo) {   // the orphaned buffer feeds the first instances, as a per-frame dynamic vertex stream would
        p_glBindBuffer(GL_ARRAY_BUFFER, orphan_vbo); p_glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 0, 0);
    }
    // shadow
    QB(Q_SHADOW);
    p_glBindFramebuffer(GL_FRAMEBUFFER, shadow_fbo); p_glViewport(0, 0, shadow_size, shadow_size);
    p_glEnable(GL_DEPTH_TEST); p_glDepthFunc(GL_LEQUAL); p_glClearDepth(1.0); p_glClear(GL_DEPTH_BUFFER_BIT);
    p_glUseProgram(shadow_prog); p_glDrawArraysInstanced(GL_TRIANGLES, 0, 36, orphan_vbo ? (GLsizei)(up / 16) : boxes);
    QE(Q_SHADOW);
    if (mid_flushes >= 1) p_glFlush();
    // G-buffer
    QB(Q_GBUF);
    p_glBindFramebuffer(GL_FRAMEBUFFER, g_fbo); p_glViewport(0, 0, W, H);
    p_glClearColor(0.5f, 0.7f, 1.0f, 1.0f); p_glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, atlas_tex);
    p_glUseProgram(box_prog); p_glDrawArraysInstanced(GL_TRIANGLES, 0, 36, orphan_vbo ? (GLsizei)(up / 16) : boxes);
    p_glDisable(GL_DEPTH_TEST);
    QE(Q_GBUF);
    if (mid_flushes >= 2) p_glFlush();
    // composite
    QB(Q_COMP);
    p_glBindVertexArray(empty_vao); p_glUseProgram(comp_prog);
    for (int i = 0; i < 3; i++) { p_glActiveTexture(GL_TEXTURE0 + i); p_glBindTexture(GL_TEXTURE_2D, g[i]); }
    p_glActiveTexture(GL_TEXTURE4); p_glBindTexture(GL_TEXTURE_2D, shadow_tex);
    float lod = 0;
    for (int i = 0; i < passes; i++) {
        const int dst = i & 1;
        p_glBindFramebuffer(GL_FRAMEBUFFER, pp_fbo[dst]);
        p_glActiveTexture(GL_TEXTURE3); p_glBindTexture(GL_TEXTURE_2D, pp[dst ^ 1]);
        p_glUniform1f(comp_k, (float)(i + 1)); p_glUniform1f(comp_lod, lod);
        p_glDrawArrays(GL_TRIANGLES, 0, 3);
        lod = 0;
        if (mips && i == passes / 2) {   // a bloom-like chain: mipmaps of this output, read at level 3 by the next pass
            p_glBindTexture(GL_TEXTURE_2D, pp[dst]); p_glGenerateMipmap(GL_TEXTURE_2D); lod = 3;
        }
    }
    QE(Q_COMP);
    if (mid_flushes >= 3) p_glFlush();
    // final
    QB(Q_FINAL);
    p_glBindFramebuffer(GL_FRAMEBUFFER, final_fbo); p_glViewport(0, 0, fw, fh);
    p_glUseProgram(final_prog); p_glActiveTexture(GL_TEXTURE0); p_glBindTexture(GL_TEXTURE_2D, pp[(passes - 1) & 1]);
    p_glDrawArrays(GL_TRIANGLES, 0, 3);
    QE(Q_FINAL);
}
// The results of the queries of `slot`, issued RING frames ago.
static void collect(int slot) {
    if (!queries || !qdone[slot]) return;
    double total = 0;
    if (queries == 1) { GLuint64 ns = 0; p_glGetQueryObjectui64v(q[slot][Q_UPLOAD], GL_QUERY_RESULT, &ns); total = ns / 1e6; }
    else for (int i = 0; i < Q_N; i++) { GLuint64 ns = 0; p_glGetQueryObjectui64v(q[slot][i], GL_QUERY_RESULT, &ns); gpu_sum[i] += ns / 1e6; total += ns / 1e6; }
    gpu_frame_sum += total; gpu_frames++;
    qdone[slot] = 0;
}

int main(int argc, char **argv) {
    int use_egl = 0, use_cgl = 0, use_iosurface = 0;
    double seconds = 10;
    const char *depth = "24";
    int c;
    while ((c = getopt(argc, argv, "eaiW:H:s:p:m:u:U:d:S:q:n:f:")) != -1) switch (c) {
        case 'e': use_egl = 1; break;
        case 'a': use_cgl = 1; break;
        case 'W': W = atoi(optarg); break;
        case 'H': H = atoi(optarg); break;
        case 's': seconds = atof(optarg); break;
        case 'p': passes = atoi(optarg); break;
        case 'm': mips = atoi(optarg); break;
        case 'u': umode = optarg; break;
        case 'U': upload_kib = atoi(optarg); break;
        case 'd': depth = optarg; break;
        case 'S': shadow_size = atoi(optarg); break;
        case 'q': queries = atoi(optarg); break;
        case 'n': boxes = atoi(optarg); break;
        case 'i': use_iosurface = 1; break;
        case 'f': mid_flushes = atoi(optarg); break;
        default: fprintf(stderr, "usage: zgbench [-e|-a] [-W w] [-H h] [-s seconds] [-p passes] [-m 0|1] [-u subdata|orphan|mapinv|persist|none] [-U KiB] [-d 24|32f|24s8] [-S size] [-q 0|1|2] [-n boxes] [-i] [-f 0-3]\n"); return 2;
    }
    if (seconds > 30) seconds = 30;
    if (passes < 2) passes = 2;
    if ((size_t)boxes * 16 > ARENA) boxes = (int)(ARENA / 16);
    setvbuf(stdout, NULL, _IOLBF, 0);

    GLFWwindow *win = NULL;
    void *(*gpa)(const char *) = NULL;
    if (use_cgl) {
        CGLPixelFormatAttribute pa[] = { kCGLPFAAccelerated, kCGLPFAOpenGLProfile, (CGLPixelFormatAttribute)kCGLOGLPVersion_GL4_Core, 0 };
        CGLPixelFormatObj pf; GLint npf = 0; CGLContextObj ctx;
        if (CGLChoosePixelFormat(pa, &pf, &npf) || !pf || CGLCreateContext(pf, NULL, &ctx) || CGLSetCurrentContext(ctx)) die("CGL context");
        void *gl = dlopen("/System/Library/Frameworks/OpenGL.framework/OpenGL", RTLD_NOW);
        if (!gl) die(dlerror());
#define F(ret, name, args) if (!(*(void **)&p_##name = dlsym(gl, #name))) die("GL entry missing: " #name);
        GLFUNCS
#undef F
        gpa = NULL;
    } else if (use_egl) {
        const char *path = getenv("ZGB_EGL") ? getenv("ZGB_EGL") : "/Library/IntelXe/GL/libEGL.1.dylib";
        void *h = dlopen(path, RTLD_NOW); if (!h) die(dlerror());
#define E(n) if (!(*(void **)&egl.n = dlsym(h, "egl" #n))) die("EGL entry missing: egl" #n);
        E(GetProcAddress) E(Initialize) E(BindAPI) E(ChooseConfig) E(CreateContext) E(CreatePbufferSurface) E(MakeCurrent) E(GetError)
#undef E
        *(void **)&egl.GetPlatformDisplay = egl.GetProcAddress("eglGetPlatformDisplayEXT");
        if (!egl.GetPlatformDisplay) die("no eglGetPlatformDisplayEXT");
        EGLDisplay d = egl.GetPlatformDisplay(0x31DD /* EGL_PLATFORM_SURFACELESS_MESA */, NULL, NULL);
        int maj, min; if (!d || !egl.Initialize(d, &maj, &min) || !egl.BindAPI(0x30A2 /* EGL_OPENGL_API */)) die("EGL init");
        const int ca[] = { 0x3033, 0x0001 /* SURFACE_TYPE PBUFFER */, 0x3040, 0x0008 /* RENDERABLE OPENGL */, 0x3024, 8, 0x3023, 8, 0x3022, 8, 0x3021, 8, 0x3038 };
        EGLConfig cfg; int n = 0; if (!egl.ChooseConfig(d, ca, &cfg, 1, &n) || n < 1) die("EGL config");
        const int cx[] = { 0x3098, 4, 0x30FB, 1, 0x30FD, 0x1 /* 4.1 core */, 0x3038 };
        EGLContext ctx = egl.CreateContext(d, cfg, NULL, cx); if (!ctx) die("EGL context");
        const int pa[] = { 0x3057, W, 0x3056, H, 0x3038 };
        EGLSurface s = egl.CreatePbufferSurface(d, cfg, pa); if (!s) die("EGL pbuffer");
        if (!egl.MakeCurrent(d, s, s, ctx)) die("EGL make current");
        gpa = egl.GetProcAddress;
    } else {
        const char *path = getenv("ZGB_GLFW") ? getenv("ZGB_GLFW") : "/Library/IntelXe/GL/libglfw.3.dylib";
        void *h = dlopen(path, RTLD_NOW); if (!h) die(dlerror());
#define G(n) if (!(*(void **)&glfw.n = dlsym(h, "glfw" #n))) die("GLFW entry missing: glfw" #n);
        G(Init) G(WindowHint) G(CreateWindow) G(MakeContextCurrent) G(SwapInterval) G(SwapBuffers) G(PollEvents) G(GetProcAddress)
        G(GetFramebufferSize) G(Terminate)
#undef G
        if (!glfw.Init()) die("glfwInit");
        glfw.WindowHint(0x00022001 /* CLIENT_API */, 0x00030001 /* OPENGL_API */);
        glfw.WindowHint(0x0002200B /* CONTEXT_CREATION_API */, 0x00036001 /* NATIVE */);
        glfw.WindowHint(0x00022002, 4); glfw.WindowHint(0x00022003, 1);           // 4.1
        glfw.WindowHint(0x00022008 /* PROFILE */, 0x00032001 /* CORE */); glfw.WindowHint(0x00022006 /* FORWARD_COMPAT */, 1);
        glfw.WindowHint(0x0002200C /* SCALE_TO_MONITOR */, 0);
        win = glfw.CreateWindow(W, H, "zgbench", NULL, NULL); if (!win) die("glfwCreateWindow");
        glfw.MakeContextCurrent(win); glfw.SwapInterval(0);
        gpa = glfw.GetProcAddress;
    }
    if (gpa) {
#define F(ret, name, args) if (!(*(void **)&p_##name = gpa(#name))) die("GL entry missing: " #name);
        GLFUNCS
#undef F
        *(void **)&p_glBufferStorage = gpa("glBufferStorage");
    }
    fw = W; fh = H; if (win) glfw.GetFramebufferSize(win, &fw, &fh);
    printf("zgbench: %s | %s | %s %dx%d (drawable %dx%d) passes %d mips %d upload %s %d KiB depth %s shadow %d boxes %d queries %d\n",
           p_glGetString(GL_RENDERER), p_glGetString(GL_VERSION), use_cgl ? (use_iosurface ? "cgl-iosurface" : "cgl") : use_egl ? "pbuffer" : "window", W, H, fw, fh, passes, mips, umode,
           upload_kib, depth, shadow_size, boxes, queries);

    // ---- resources
    GLenum dint = GL_DEPTH_COMPONENT24, dfmt = GL_DEPTH_COMPONENT, dtype = GL_UNSIGNED_INT, datt = GL_DEPTH_ATTACHMENT;
    if (!strcmp(depth, "32f")) { dint = GL_DEPTH_COMPONENT32F; dtype = GL_FLOAT; }
    else if (!strcmp(depth, "24s8")) { dint = GL_DEPTH24_STENCIL8; dfmt = GL_DEPTH_STENCIL; dtype = GL_UNSIGNED_INT_24_8; datt = GL_DEPTH_STENCIL_ATTACHMENT; }
    shadow_tex = texture(dint, dfmt, dtype, shadow_size, shadow_size, 0);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE); p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    shadow_fbo = fbo(0, NULL, shadow_tex, datt);
    g[0] = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, W, H, 0); g[1] = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, W, H, 0);
    g[2] = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, W, H, 0);
    GLuint gdepth = texture(dint, dfmt, dtype, W, H, 0);
    g_fbo = fbo(3, g, gdepth, datt);
    pp[0] = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, W, H, mips); pp[1] = texture(GL_RGBA16F, GL_RGBA, GL_HALF_FLOAT, W, H, mips);
    pp_fbo[0] = fbo(1, &pp[0], 0, 0); pp_fbo[1] = fbo(1, &pp[1], 0, 0);
    // atlas: 1024x1024 RGBA8 with mipmaps, a pattern
    unsigned char *atlas = malloc(1024 * 1024 * 4);
    for (int y = 0; y < 1024; y++) for (int x = 0; x < 1024; x++) {
        unsigned char *p = atlas + (y * 1024 + x) * 4; p[0] = (unsigned char)(x ^ y); p[1] = (unsigned char)(x * 3); p[2] = (unsigned char)(y * 5); p[3] = 255;
    }
    p_glGenTextures(1, &atlas_tex); p_glBindTexture(GL_TEXTURE_2D, atlas_tex);
    p_glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 1024, 1024, 0, GL_RGBA, GL_UNSIGNED_BYTE, atlas); free(atlas);
    p_glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR); p_glGenerateMipmap(GL_TEXTURE_2D);

    // geometry: a unit box (36 vertices) and per-instance vec4 from the arena
    static const float cube[] = {
        -1,-1,-1, 1,-1,-1, 1,1,-1, -1,-1,-1, 1,1,-1, -1,1,-1,   -1,-1,1, 1,1,1, 1,-1,1, -1,-1,1, -1,1,1, 1,1,1,
        -1,-1,-1, -1,1,1, -1,-1,1, -1,-1,-1, -1,1,-1, -1,1,1,   1,-1,-1, 1,-1,1, 1,1,1, 1,-1,-1, 1,1,1, 1,1,-1,
        -1,-1,-1, -1,-1,1, 1,-1,1, -1,-1,-1, 1,-1,1, 1,-1,-1,   -1,1,-1, 1,1,1, -1,1,1, -1,1,-1, 1,1,-1, 1,1,1 };
    GLuint cube_vbo;
    p_glGenVertexArrays(1, &vao); p_glBindVertexArray(vao);
    p_glGenBuffers(1, &cube_vbo); p_glBindBuffer(GL_ARRAY_BUFFER, cube_vbo); p_glBufferData(GL_ARRAY_BUFFER, sizeof cube, cube, GL_STATIC_DRAW);
    p_glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, 0); p_glEnableVertexAttribArray(0);
    inst = malloc(ARENA);
    for (size_t i = 0; i < ARENA / 16; i++) {   // a grid of boxes over [-1,1]^2, heights from a hash
        const int gx = (int)(i % 256), gz = (int)((i / 256) % 256), layer = (int)(i / 65536);
        inst[i * 4 + 0] = (gx + 0.5f) / 128.0f - 1.0f; inst[i * 4 + 1] = (float)((gx * 7 + gz * 13 + layer * 3) % 17) / 40.0f - 0.2f;
        inst[i * 4 + 2] = (gz + 0.5f) / 128.0f - 1.0f; inst[i * 4 + 3] = 0.0035f;
    }
    p_glGenBuffers(1, &arena); p_glBindBuffer(GL_ARRAY_BUFFER, arena);
    p_glBufferData(GL_ARRAY_BUFFER, ARENA, inst, GL_DYNAMIC_DRAW);
    up = (size_t)upload_kib * 1024 > ARENA ? ARENA : (size_t)upload_kib * 1024;
    if (!strcmp(umode, "persist")) {
        if (!p_glBufferStorage) die("persist needs glBufferStorage");
        p_glGenBuffers(1, &staging); p_glBindBuffer(GL_COPY_READ_BUFFER, staging);
        p_glBufferStorage(GL_COPY_READ_BUFFER, (GLsizeiptr)(up * 3 + piece), NULL, GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT | GL_CLIENT_STORAGE_BIT);
        persist = p_glMapBufferRange(GL_COPY_READ_BUFFER, 0, (GLsizeiptr)(up * 3 + piece), GL_MAP_WRITE_BIT | GL_MAP_PERSISTENT_BIT | GL_MAP_COHERENT_BIT);
        if (!persist) die("persistent map");
    } else if (!strcmp(umode, "orphan")) {
        p_glGenBuffers(1, &orphan_vbo); p_glBindBuffer(GL_ARRAY_BUFFER, orphan_vbo); p_glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)up, NULL, GL_STREAM_DRAW);
    }
    p_glGenBuffers(2, ubo);
    p_glBindBuffer(GL_UNIFORM_BUFFER, ubo[0]); p_glBufferData(GL_UNIFORM_BUFFER, 4096, NULL, GL_DYNAMIC_DRAW);
    p_glBindBuffer(GL_UNIFORM_BUFFER, ubo[1]); p_glBufferData(GL_UNIFORM_BUFFER, 16384, NULL, GL_DYNAMIC_DRAW);
    p_glBindBufferBase(GL_UNIFORM_BUFFER, 0, ubo[0]); p_glBindBufferBase(GL_UNIFORM_BUFFER, 1, ubo[1]);

    // ---- programs
    const char *box_vs =
        "#version 410 core\nlayout(location=0) in vec3 p; layout(location=1) in vec4 inst;\n"
        "layout(std140) uniform Frame { vec4 light; vec4 cam; vec4 pad[254]; };\n"
        "layout(std140) uniform Big { vec4 tint[1024]; };\n"
        "uniform int shadow_pass; out vec3 wpos; out vec3 nrm; out vec4 col;\n"
        "void main(){ vec3 w = inst.xyz + p * inst.w; wpos = w; nrm = p; col = tint[gl_InstanceID & 1023];\n"
        "  vec3 e = shadow_pass == 1 ? light.xyz : cam.xyz; vec3 d = w - e; float c = cos(e.x), s = sin(e.x);\n"
        "  vec3 v = vec3(c * w.x - s * w.z, w.y - 0.1, s * w.x + c * w.z);\n"
        "  gl_Position = shadow_pass == 1 ? vec4(w.x, w.z, w.y * 0.5 + 0.5, 1.0) : vec4(v.x, v.y + v.z * 0.6, v.z * 0.4 + 0.5 + 0.0 * d.x, 1.0); }\n";
    const char *shadow_fs = "#version 410 core\nvoid main(){}\n";
    const char *gbuf_fs =
        "#version 410 core\nin vec3 wpos; in vec3 nrm; in vec4 col; uniform sampler2D atlas;\n"
        "layout(location=0) out vec4 albedo; layout(location=1) out vec4 normal; layout(location=2) out vec4 extra;\n"
        "void main(){ vec4 t = texture(atlas, wpos.xz * 4.0 + nrm.xy * 0.1); albedo = t * col; normal = vec4(normalize(nrm), wpos.y);\n"
        "  extra = vec4(fract(wpos * 8.0), 1.0); }\n";
    const char *quad_vs = "#version 410 core\nout vec2 uv; void main(){ vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    const char *comp_fs =
        "#version 410 core\nin vec2 uv; uniform sampler2D albedo, normal, extra, prev; uniform sampler2DShadow shadow_map; uniform float k; uniform float lod;\n"
        "layout(std140) uniform Frame { vec4 light; vec4 cam; vec4 pad[254]; };\nout vec4 o;\n"
        "void main(){ vec4 a = texture(albedo, uv), n = texture(normal, uv), e = texture(extra, uv);\n"
        "  vec4 acc = vec4(0); for (int i = -2; i <= 2; i++) acc += textureLod(prev, uv + vec2(float(i) * k, float(i) * k * 0.5) * 0.002, lod);\n"
        "  float sh = texture(shadow_map, vec3(uv, n.w * 0.5 + 0.5));\n"
        "  o = vec4(a.rgb * (0.3 + 0.7 * sh) * max(dot(n.xyz, normalize(light.xyz)), 0.1) + acc.rgb * 0.15 + e.rgb * 0.02, 1.0); }\n";
    const char *final_fs = "#version 410 core\nin vec2 uv; uniform sampler2D src; out vec4 o; void main(){ vec3 c = texture(src, uv).rgb; o = vec4(c / (c + 1.0), 1.0); }\n";
    box_prog = program(box_vs, gbuf_fs); shadow_prog = program(box_vs, shadow_fs); comp_prog = program(quad_vs, comp_fs); final_prog = program(quad_vs, final_fs);
    GLuint progs_ubo[] = { box_prog, shadow_prog, comp_prog };
    for (int i = 0; i < 3; i++) {
        GLuint b = p_glGetUniformBlockIndex(progs_ubo[i], "Frame"); if (b != GL_INVALID_INDEX) p_glUniformBlockBinding(progs_ubo[i], b, 0);
        b = p_glGetUniformBlockIndex(progs_ubo[i], "Big"); if (b != GL_INVALID_INDEX) p_glUniformBlockBinding(progs_ubo[i], b, 1);
    }
    p_glUseProgram(box_prog); p_glUniform1i(p_glGetUniformLocation(box_prog, "atlas"), 0); p_glUniform1i(p_glGetUniformLocation(box_prog, "shadow_pass"), 0);
    p_glUseProgram(shadow_prog); p_glUniform1i(p_glGetUniformLocation(shadow_prog, "shadow_pass"), 1);
    p_glUseProgram(comp_prog);
    p_glUniform1i(p_glGetUniformLocation(comp_prog, "albedo"), 0); p_glUniform1i(p_glGetUniformLocation(comp_prog, "normal"), 1);
    p_glUniform1i(p_glGetUniformLocation(comp_prog, "extra"), 2); p_glUniform1i(p_glGetUniformLocation(comp_prog, "prev"), 3);
    p_glUniform1i(p_glGetUniformLocation(comp_prog, "shadow_map"), 4);
    comp_k = p_glGetUniformLocation(comp_prog, "k"); comp_lod = p_glGetUniformLocation(comp_prog, "lod");
    p_glUseProgram(final_prog); p_glUniform1i(p_glGetUniformLocation(final_prog, "src"), 0);
    p_glGenVertexArrays(1, &empty_vao);
    if (use_cgl && use_iosurface) {
        const int bpe = 4, bpr = fw * 4;
        CFStringRef k[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
        const int pixel_format = 'BGRA';
        CFNumberRef v[] = { CFNumberCreate(NULL, kCFNumberIntType, &fw), CFNumberCreate(NULL, kCFNumberIntType, &fh),
                            CFNumberCreate(NULL, kCFNumberIntType, &bpe), CFNumberCreate(NULL, kCFNumberIntType, &bpr),
                            CFNumberCreate(NULL, kCFNumberIntType, &pixel_format) };
        CFDictionaryRef props = CFDictionaryCreate(NULL, (const void **)k, (const void **)v, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        IOSurfaceRef surface = IOSurfaceCreate(props);
        if (!surface) die("IOSurfaceCreate");
        GLuint rt; p_glGenTextures(1, &rt); p_glBindTexture(GL_TEXTURE_RECTANGLE, rt);
        if (CGLTexImageIOSurface2D(CGLGetCurrentContext(), GL_TEXTURE_RECTANGLE, GL_RGBA8, fw, fh, GL_BGRA, GL_UNSIGNED_INT_8_8_8_8_REV, surface, 0))
            die("CGLTexImageIOSurface2D");
        GLuint f; p_glGenFramebuffers(1, &f); p_glBindFramebuffer(GL_FRAMEBUFFER, f);
        p_glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_RECTANGLE, rt, 0);
        if (p_glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) die("IOSurface framebuffer incomplete");
        final_fbo = f;
    } else if (use_cgl) { GLuint rt = texture(GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE, fw, fh, 0); final_fbo = fbo(1, &rt, 0, 0); }
    else if (use_iosurface) die("-i needs -a");
    p_glGenQueries(RING * Q_N, &q[0][0]);
    unsigned long frame = 0;
    // warm-up: 1 s (pipelines, caches), then the measured run
    const double t_warm = now();
    while (now() - t_warm < 1.0) {
        const int slot = (int)(frame % RING);
        draw_frame(frame++, slot);   // its queries' results are dropped (qdone stays 0)
        if (win) { glfw.SwapBuffers(win); glfw.PollEvents(); } else p_glFinish();
    }
    p_glFinish();
    const double t0 = now();
    unsigned long measured = 0;
    double last_report = t0;
    while (now() - t0 < seconds) {
        const int slot = (int)(frame % RING);
        collect(slot);
        draw_frame(frame++, slot); qdone[slot] = 1;
        if (win) { glfw.SwapBuffers(win); glfw.PollEvents(); }
        else if ((frame & 3) == 0) p_glFinish();
        else if (use_cgl) p_glFlush();   // CGL offscreen: the frame's end, as a present would flush
        measured++;
        if (now() - last_report >= 2.0) { last_report = now(); fprintf(stderr, "zgbench: %lu frames\n", measured); }
    }
    p_glFinish();
    const double dt = now() - t0;
    for (int s = 0; s < RING; s++) collect(s);

    // a fixed frame for the image hash: frame index 1000, read from the last composite target (not the window)
    // (from cleared history, twice: the first pass of a frame reads the previous frame's last one)
    const int qs = queries; queries = 0;
    // Each from upload offset 0: the uploads go on from wherever the timed loop stopped, and with -u orphan the frame
    // draws what it uploaded, so the hash depended on the number of frames run (it varied between runs of one config).
    for (int i = 0; i < 2; i++) { p_glBindFramebuffer(GL_FRAMEBUFFER, pp_fbo[i]); p_glClearColor(0, 0, 0, 0); p_glClear(GL_COLOR_BUFFER_BIT); }
    upload_at = 0; draw_frame(1000, 0); upload_at = 0; draw_frame(1000, 0); p_glFinish();
    queries = qs;
    p_glBindFramebuffer(GL_FRAMEBUFFER, pp_fbo[(passes - 1) & 1]);
    const int rw = 256, rh = 144; static uint16_t px[256 * 144 * 4];
    p_glReadPixels(W / 2 - rw / 2, H / 2 - rh / 2, rw, rh, GL_RGBA, GL_HALF_FLOAT, px);
    uint64_t hash = 1469598103934665603ull;
    for (size_t i = 0; i < sizeof px / sizeof *px; i++) hash = (hash ^ px[i]) * 1099511628211ull;
    const GLenum err = p_glGetError();

    printf("zgbench: fps %.1f | frames %lu in %.2f s | cpu %.3f ms/frame", measured / dt, measured, dt, dt * 1000.0 / measured);
    if (gpu_frames) {
        printf(" | gpu %.3f ms/frame (%ld frames)", gpu_frame_sum / gpu_frames, gpu_frames);
        if (queries >= 2) for (int i = 0; i < Q_N; i++) printf(" %s %.3f", qname[i], gpu_sum[i] / gpu_frames);
    }
    printf(" | image %016llx | glGetError 0x%x | drawn %lu\n", (unsigned long long)hash, err, frame + 2);   // + the 2 hash frames
    if (win) glfw.Terminate();
    return err ? 3 : 0;
}
