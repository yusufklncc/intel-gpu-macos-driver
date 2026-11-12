// SPDX-License-Identifier: MIT
// The table libcglzink.dylib exports for libcglzink-boot.dylib (the "every OpenGL application" switch). Inserted into a
// process (DYLD_INSERT_LIBRARIES), libcglzink interposes Apple's CGL entry points itself; loaded by the boot library with
// dlopen it cannot (dyld applies the __interpose tuples of inserted images only), so the boot library interposes them
// and calls these. Bump CGLZINK_HOOKS_VERSION when a member changes.
#ifndef CGLZINK_HOOKS_H
#define CGLZINK_HOOKS_H
#include <OpenGL/OpenGL.h>
#include <OpenGL/CGLIOSurface.h>
#include <stdint.h>

#define CGLZINK_HOOKS_VERSION 2   // 2: TexImagePBuffer

struct cglzink_hooks {
    uint32_t version, size;
    CGLError (*CreateContext)(CGLPixelFormatObj, CGLContextObj, CGLContextObj *);
    CGLError (*DestroyContext)(CGLContextObj);
    void (*ReleaseContext)(CGLContextObj);
    CGLError (*SetCurrentContext)(CGLContextObj);
    CGLError (*LockContext)(CGLContextObj);
    CGLError (*UnlockContext)(CGLContextObj);
    CGLError (*FlushDrawable)(CGLContextObj);
    CGLError (*SetParameter)(CGLContextObj, CGLContextParameter, const GLint *);
    CGLError (*TexImageIOSurface2D)(CGLContextObj, GLenum, GLenum, GLsizei, GLsizei, GLenum, GLenum, IOSurfaceRef, GLuint);
    CGLError (*ClearDrawable)(CGLContextObj);
    CGLError (*UpdateContext)(CGLContextObj);
    CGLError (*SetPBuffer)(CGLContextObj, CGLPBufferObj, GLenum, GLint, GLint);
    CGLError (*SetOffScreen)(CGLContextObj, GLsizei, GLsizei, GLint, void *);
    CGLError (*CopyContext)(CGLContextObj, CGLContextObj, GLbitfield);
    CGLError (*TexImagePBuffer)(CGLContextObj, CGLPBufferObj, GLenum);
};

#endif
