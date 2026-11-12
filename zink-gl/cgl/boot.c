// SPDX-License-Identifier: MIT
// libcglzink-boot.dylib: the experimental "every OpenGL application through zink" switch. zink-gl/install.sh
// --global-gl on makes a LaunchAgent put it into DYLD_INSERT_LIBRARIES of the login session (launchctl setenv), which
// macOS honours on the B580 machines because the driver boots with amfi_get_out_of_my_way=0x1. It is then in every
// process the session starts, so it is small and decides at once:
//   - it is plain C (CoreFoundation for the bundle id, OpenGL for the entry points it interposes), never AppKit;
//   - it takes part only in an application bundle (the executable under .app/Contents/MacOS/) that is not Apple's
//     (except the ones allowed below), not a browser, Electron/Chromium app or video player, and not refused in
//     /Library/IntelXe/GL/cglzink-global.txt ("-bundle.id" refuses, "+bundle.id" allows one of Apple's or a listed one);
//   - then it loads libcglzink.dylib with dlopen and hands the CGL calls to its table (cglzink_hooks.h); otherwise
//     every call goes straight to Apple's OpenGL;
//   - it removes DYLD_INSERT_LIBRARIES from the process's environment, so the processes it starts itself do not
//     inherit it (those launchd starts get it again from the session, and decide for themselves).
// libcglzink calls Apple's CGL from inside these calls; dyld applies this library's interposing to libcglzink too, so
// a per-thread depth sends those nested calls to Apple's functions instead of back into libcglzink.
// Tests and diagnosis: CGLZINK_GLOBAL_OFF=1 (never take part), CGLZINK_GLOBAL_ANY=1 (also outside an application
// bundle: test/run.sh), CGLZINK_LIB=<path> (another libcglzink), CGLZINK_LOG=1 (why it took part or not, on stderr).
#include <CoreFoundation/CoreFoundation.h>
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <dlfcn.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "cglzink_hooks.h"

static const struct cglzink_hooks *H;   // set once, in the constructor, before the application runs
static __thread int depth;

#define BLOG(...) do { if (getenv("CGLZINK_LOG")) { fprintf(stderr, "cglzink-boot: " __VA_ARGS__); fputc('\n', stderr); } } while (0)

// Apple's applications that draw with OpenGL and work through zink (tested on the B580); every other com.apple.* stays
static const char *const kAppleAllowed[] = { "com.apple.Chess" };
// never: browsers and Chromium/Electron (they draw with Metal or into IOSurfaces), video players (CAOpenGLLayer),
// the system's interface (it must not depend on an experimental layer)
static const char *const kRefused[] = {
    "org.mozilla.firefox", "com.google.Chrome", "com.microsoft.edgemac", "com.brave.Browser", "company.thebrowser.Browser",
    "com.operasoftware.Opera", "com.vivaldi.Vivaldi", "com.colliderli.iina", "io.mpv", "org.videolan.vlc",
    "io.github.yusufklncc.IntelXeManager",
};

static int listed(const char *id, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++) if (!strcmp(id, list[i])) return 1;
    return 0;
}

// /Library/IntelXe/GL/cglzink-global.txt: one "+id" or "-id" per line, # comments. 1 allowed, -1 refused, 0 not listed.
static int config_says(const char *id) {
    FILE *f = fopen("/Library/IntelXe/GL/cglzink-global.txt", "r");
    if (!f) return 0;
    char line[256];
    int r = 0;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n# \t")] = 0;
        if ((line[0] == '+' || line[0] == '-') && !strcmp(line + 1, id)) r = line[0] == '+' ? 1 : -1;
    }
    fclose(f);
    return r;
}

static int electron_like(const char *exe) {
    // .../X.app/Contents/MacOS/X -> .../X.app/Contents/Frameworks/{Electron Framework,Chromium Embedded Framework}.framework
    char base[PATH_MAX];
    const char *m = strstr(exe, ".app/Contents/MacOS/");
    if (!m || (size_t)(m - exe) + 14 >= sizeof base) return 0;
    size_t n = (size_t)(m - exe) + 14;   // up to and including "Contents/"
    memcpy(base, exe, n);
    base[n] = 0;
    static const char *const fw[] = { "Frameworks/Electron Framework.framework", "Frameworks/Chromium Embedded Framework.framework" };
    struct stat st;
    for (size_t i = 0; i < 2; i++) {
        char p[PATH_MAX];
        if (snprintf(p, sizeof p, "%s%s", base, fw[i]) < (int)sizeof p && stat(p, &st) == 0) return 1;
    }
    return 0;
}

static int should_take_part(void) {
    if (getenv("CGLZINK_GLOBAL_OFF") || getenv("CGLZINK_OFF")) { BLOG("off (environment)"); return 0; }
    char exe[PATH_MAX];
    uint32_t size = sizeof exe;
    if (_NSGetExecutablePath(exe, &size) != 0) return 0;
    const int any = getenv("CGLZINK_GLOBAL_ANY") != NULL;
    if (!strstr(exe, ".app/Contents/MacOS/") && !any) return 0;   // not an application: quietly out
    char id[256] = "";
    CFBundleRef b = CFBundleGetMainBundle();
    CFStringRef s = b ? CFBundleGetIdentifier(b) : NULL;
    if (s) CFStringGetCString(s, id, sizeof id, kCFStringEncodingUTF8);
    if (!id[0] && !any) { BLOG("%s: no bundle id", exe); return 0; }
    const int conf = id[0] ? config_says(id) : 0;
    if (conf < 0) { BLOG("%s: refused in cglzink-global.txt", id); return 0; }
    if (conf == 0) {
        if (listed(id, kRefused, sizeof kRefused / sizeof *kRefused)) { BLOG("%s: on the built-in refusal list", id); return 0; }
        if (!strncmp(id, "com.apple.", 10) && !listed(id, kAppleAllowed, sizeof kAppleAllowed / sizeof *kAppleAllowed)) {
            BLOG("%s: Apple's, not on the allowed list", id); return 0;
        }
        if (electron_like(exe)) { BLOG("%s: Electron/Chromium", id); return 0; }
    }
    return 1;
}

__attribute__((constructor)) static void boot(void) {
    // the processes this one starts do not inherit the insertion from it
    const char *ins = getenv("DYLD_INSERT_LIBRARIES");
    if (ins && strstr(ins, "libcglzink-boot")) unsetenv("DYLD_INSERT_LIBRARIES");
    if (!should_take_part()) return;
    const char *lib = getenv("CGLZINK_LIB");
    if (!lib) lib = "/Library/IntelXe/GL/libcglzink.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) { BLOG("cannot load %s: %s", lib, dlerror()); return; }
    const struct cglzink_hooks *t = (const struct cglzink_hooks *)dlsym(h, "cglzink_hooks");
    if (!t || t->version != CGLZINK_HOOKS_VERSION || t->size < sizeof *t) {
        BLOG("%s has no matching cglzink_hooks (version %u)", lib, t ? t->version : 0);
        return;   // libcglzink stays loaded but unused: its own constructor only watches for AppKit
    }
    H = t;
    BLOG("taking part through %s", lib);
}

// Every interposed call: libcglzink's version on the outermost level when it is loaded, Apple's otherwise.
#define HOOK(ret, name, params, args) \
    static ret b_##name params { \
        if (H && !depth) { depth++; ret hook_result_ = H->name args; depth--; return hook_result_; } \
        return CGL##name args; \
    }
HOOK(CGLError, CreateContext, (CGLPixelFormatObj p, CGLContextObj s, CGLContextObj *o), (p, s, o))
HOOK(CGLError, DestroyContext, (CGLContextObj c), (c))
HOOK(CGLError, SetCurrentContext, (CGLContextObj c), (c))
HOOK(CGLError, LockContext, (CGLContextObj c), (c))
HOOK(CGLError, UnlockContext, (CGLContextObj c), (c))
HOOK(CGLError, FlushDrawable, (CGLContextObj c), (c))
HOOK(CGLError, SetParameter, (CGLContextObj c, CGLContextParameter n, const GLint *v), (c, n, v))
HOOK(CGLError, TexImageIOSurface2D, (CGLContextObj c, GLenum t, GLenum i, GLsizei w, GLsizei h, GLenum f, GLenum ty,
    IOSurfaceRef s, GLuint pl), (c, t, i, w, h, f, ty, s, pl))
HOOK(CGLError, ClearDrawable, (CGLContextObj c), (c))
HOOK(CGLError, UpdateContext, (CGLContextObj c), (c))
HOOK(CGLError, SetPBuffer, (CGLContextObj c, CGLPBufferObj p, GLenum f, GLint l, GLint s), (c, p, f, l, s))
HOOK(CGLError, SetOffScreen, (CGLContextObj c, GLsizei w, GLsizei h, GLint r, void *b), (c, w, h, r, b))
HOOK(CGLError, CopyContext, (CGLContextObj s, CGLContextObj d, GLbitfield m), (s, d, m))
HOOK(CGLError, TexImagePBuffer, (CGLContextObj c, CGLPBufferObj p, GLenum s), (c, p, s))
static void b_ReleaseContext(CGLContextObj c) {
    if (H && !depth) { depth++; H->ReleaseContext(c); depth--; return; }
    CGLReleaseContext(c);
}

#define INTERPOSE(name) \
    __attribute__((used, section("__DATA,__interpose"))) static struct { const void *a, *b; } \
    interpose_##name = { (const void *)b_##name, (const void *)CGL##name }
INTERPOSE(CreateContext);
INTERPOSE(DestroyContext);
INTERPOSE(ReleaseContext);
INTERPOSE(SetCurrentContext);
INTERPOSE(LockContext);
INTERPOSE(UnlockContext);
INTERPOSE(FlushDrawable);
INTERPOSE(SetParameter);
INTERPOSE(TexImageIOSurface2D);
INTERPOSE(ClearDrawable);
INTERPOSE(UpdateContext);
INTERPOSE(SetPBuffer);
INTERPOSE(SetOffScreen);
INTERPOSE(CopyContext);
INTERPOSE(TexImagePBuffer);
