# cglzink: native OpenGL applications drawing through zink (experimental)

`libcglzink.dylib`, loaded with `DYLD_INSERT_LIBRARIES` into a chosen application, fills the dispatch table of each of its CGL
contexts with thunks that call Mesa's zink through EGL. Apple's CGL objects stay; only what they point at changes. It is
the CGL/NSOpenGL counterpart of the GLFW route in `../README.md`: the application's GL calls skip Apple's OpenGL → Metal →
plugin path. Nothing loads it unless the user starts an application through `../cglzink-app.sh`, or turns on the
experimental switch for every application (`sudo ../install.sh --global-gl on`, off by default), with which
`libcglzink-boot.dylib` (`boot.c`) loads it into the applications that take part (`../README.md`).

History: prototype in the hackintosh research tree (`tools/b580/cgl-zink`, `tools/b580/cgl-thunks`; commits 0640304,
cb58869, ea35ef0, aca50fc, 57e1f0c, 5 Nov 2026), imported unchanged in this folder's first commit.

| File | What |
|---|---|
| `cglzink.m` | the layer: adopting contexts, lifetime, current context and threads, present, view → layer, swap interval, IOSurface |
| `boot.c` | `libcglzink-boot.dylib`, the switch for every application: plain C, in every process of the session; decides once per process (application bundles only, not Apple's except Chess, not browsers, Electron/Chromium, video players or IntelXe Manager; `/Library/IntelXe/GL/cglzink-global.txt`), then loads `libcglzink.dylib` with `dlopen` |
| `cglzink_hooks.h` | the table of CGL entry points `libcglzink.dylib` exports for the boot library (dyld ignores the `__interpose` section of a `dlopen`ed image) |
| `appleview.h` | what a context says about itself: Apple's version, GLSL version and extensions for its profile |
| `appleext.h` | Apple's own GL extensions zink lacks (`APPLE_vertex_array_object`, `APPLE_fence`, `APPLE_element_array`, ...) from core functions |
| `gen/` | the thunk generator and the measured symbol → entry map (`gen/README.md`) |
| `test/fakeegl.c` | test stand-in for EGL: a hidden Apple context per EGL context, Mesa's binding rules. Says nothing about zink |
| `test/layertest.m`, `test/run.sh` | pixel hashes on Apple's GL and through the layer, which must agree; the same through the boot library (it must take part with `CGLZINK_GLOBAL_ANY=1` and stay out otherwise, as `layertest` is no application bundle) |
| `build.sh ARCH OUT` | builds the two libraries, the EGL stand-in and `layertest` (x86_64 for the B580, arm64 for tests on Apple silicon), ad-hoc signed |

```
./build.sh x86_64 /tmp/cz      # libcglzink.dylib and libcglzink-boot.dylib for the package (lib/ of ZINK_DIR)
./build.sh arm64 /tmp/czt && LAYERTEST_VIEW=1 test/run.sh /tmp/czt   # plumbing tests on the build Mac
```

Settings: `CGLZINK_OFF=1` (nothing is adopted), `CGLZINK_LOG=1`, `CGLZINK_EGL=<path>` (default `/Library/IntelXe/GL/libEGL.1.dylib`),
`CGLZINK_FPS=<seconds>` (presents per second on stderr, for applications without a counter), `CGLZINK_APPLE=1` (nothing is
adopted but the counter runs: the same measurement on Apple's GL).
The boot library reads `CGLZINK_GLOBAL_OFF=1` (never take part), `CGLZINK_GLOBAL_ANY=1` (take part outside an
application bundle too: the tests), `CGLZINK_LIB=<path>` (another `libcglzink.dylib`) and `CGLZINK_LOG=1` (why it took
part or not).
When EGL does not open, or a context cannot be made, that context stays on Apple's OpenGL.

## What it covers

- CGL: `CGLCreateContext`, `CGLDestroyContext`/`CGLReleaseContext` (by reference count), `CGLSetCurrentContext`,
  `CGLLockContext`/`CGLUnlockContext`, `CGLFlushDrawable`, `CGLSetParameter` (swap interval), `CGLClearDrawable`,
  `CGLUpdateContext`, `CGLTexImageIOSurface2D` (a copy), all 975 dispatch entries (906 straight to zink, 25 rebuilt in
  `appleext.h`, the rest Apple-only hints that do nothing).
- NSOpenGLContext: `initWithFormat:shareContext:`, `initWithCGLContextObj:`, `makeCurrentContext`, `clearCurrentContext`,
  `setView:`, `update`, `flushBuffer`, `clearDrawable`. NSOpenGLView works through these.
- Legacy (2.1 → zink 4.6 compatibility) and core (3.2 / 4.1 → zink core) profiles; shared contexts (both on zink, or both
  stay on Apple's GL); a context used by two threads under `CGLLockContext` (the CVDisplayLink pattern).
- The application sees Apple's GL for its profile, not zink's 4.6: `GL_VERSION`, `GL_SHADING_LANGUAGE_VERSION`,
  `GL_MAJOR/MINOR_VERSION` from Apple's own context (4.1 / 4.10 core, 2.1 / 1.20 legacy), and Apple's extensions without
  those zink lacks (`glGetString`, `glGetStringi`, `GL_NUM_EXTENSIONS`). OpenGL.framework exports nothing past 4.1, so an
  application told 4.6 took 4.4 paths through NULL pointers (GZDoom 4.14: `glBufferStorage`). `GL_VENDOR` names Intel.
- Retina: the surface is in pixels only when the view asks for it (`wantsBestResolutionOpenGLSurface`), as with Apple.
- The swap interval is Apple's context's own value (the application's, or Apple's default 1).
- Contexts Core Animation makes for a `CAOpenGLLayer`/`NSOpenGLLayer` (and the application's subclasses) stay on Apple's GL.

## What it does not

- MSAA: the B580 machine offers 78 multisampled EGL configs (up to 8 samples) and the layer asks for them; not yet
  checked in a window. Without one, a context asking for samples gets none (said once).
- `CGLTexImageIOSurface2D` copies the plane into the texture at the call (right for video textures,
  `CVOpenGLTextureCache`). A texture that is then attached to a framebuffer (`glFramebufferTexture2D`) is drawn into:
  its pixels are copied back into the IOSurface at every `glFlush`, `glFinish` and present (since 12 Nov; checked by
  `test/run.sh`'s "iosurface draw" case through the stand-in EGL, not yet on the B580). Copies both ways cost a
  read-back per flush; a change another process makes to the surface reaches the texture only when it is bound again.
  At most 64 IOSurface textures per process are tracked. `GL_YCBCR_422_APPLE` is refused.
- `CGLSetPBuffer` draws into an EGL pbuffer of the pbuffer's size; `CGLTexImagePBuffer` copies its pixels into the
  asking context's texture at the call (level 0 and face 0 only). Since 12 Nov, not yet run on the B580
  (`LAYERTEST_PBUFFER=1 ./layertest`; Apple's GL on Apple silicon no longer creates pbuffers, so compare on the B580).
- `CGLSetOffScreen` draws into an EGL pbuffer whose pixels are read into the application's memory at each `glFlush`,
  `glFinish` and `CGLFlushDrawable` (BGRA, OpenGL's row order; since 12 Nov, `LAYERTEST_OFFSCREEN=1 ./layertest` on the
  B580 compares the layout with Apple's software renderer). `kCGLCPSurfaceBackingSize` with `kCGLCESurfaceBackingSize`
  on fixes the layer's drawable size, scaled to the view by Core Animation (since 12 Nov, not yet seen in a window).
- `CGLCopyContext`, `kCGLCPSurfaceOpacity` 0, full screen CGL: said once, left to Apple's GL (so they show nothing
  through zink).
- `setView:` puts a layer over the view's own: what the application draws into the view with AppKit stays under it.
- `GL_RENDERER` is zink's (it names the Intel device). Version, GLSL version and extensions are Apple's (below).
- A context two threads use without locking: the second thread's calls are dropped after 100 ms (said once).
- Injection needs `DYLD_INSERT_LIBRARIES` to reach the process: true on this driver's setup (AMFI off) also for hardened
  and Apple applications. With AMFI off `launchctl setenv` passes `DYLD_*` too; the experimental switch for every
  application uses that (`../README.md`, or IntelXe Manager's Zink page with its list of applications). It is off by
  default; on the B580 (11 Nov 2026) Chess drew through it and TextEdit, Calculator, System Settings, Safari and
  1Password stayed on Apple's OpenGL.

Tested on the build Mac (macOS 27, arm64 and x86_64 under Rosetta) through the stand-in only: every hash equal to Apple's
GL, with and without zink's missing names. On the B580 the prototype ran Chess.app (5 Nov); this version ran GZDoom 4.14
(MAP01, uncapped: ~1910 fps, against ~1600 on Apple's GL; 9-10 Nov). The boot library (10 Nov), through the stand-in:
with `CGLZINK_GLOBAL_ANY=1` the layer adopts the contexts and the hashes equal Apple's; without it `layertest` stays
out. On the B580 (11 Nov 2026) with `install.sh --global-gl on`: as above.
