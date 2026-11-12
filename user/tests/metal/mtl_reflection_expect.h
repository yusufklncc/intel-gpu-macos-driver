// SPDX-License-Identifier: MIT
// mtl_reflection_expect.h: the bindings Apple's driver reflects for mtl_reflection's shaders (kSource there; the same
// shaders' AIR is user/tests/plugin/refl/), and the check that compares a reflection with them. Shared by
// mtl_reflection.m (any Metal device) and user/tests/plugin/refl_host.m (the plugin's reflection code, no GPU).
#ifndef MTL_REFLECTION_EXPECT_H
#define MTL_REFLECTION_EXPECT_H
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <stdio.h>
#include <stdarg.h>

static int npass, nfail, nskip;
static void check(int ok, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void check(int ok, const char *fmt, ...)
{
    va_list ap; va_start(ap, fmt);
    printf("%s ", ok ? "PASS" : "FAIL"); vprintf(fmt, ap); printf("\n");
    va_end(ap);
    if (ok) npass++; else nfail++;
}
static inline void skip(const char *what) { printf("SKIP %s\n", what); nskip++; }

// One expected binding. size/align: buffer data size and alignment, or threadgroup memory size and alignment.
// dtype: buffer data type, or texture component type. ttype: texture type. stagein: a stage-in buffer (no argument).
typedef struct {
    MTLBindingType type; NSUInteger index; const char *name; MTLBindingAccess access; BOOL used;
    MTLDataType dtype; NSUInteger size, align; MTLTextureType ttype; BOOL depth, stagein;
} expect_t;

static const expect_t kKernel[] = {
    { MTLBindingTypeBuffer, 0, "out", MTLBindingAccessReadWrite, YES, MTLDataTypeFloat4, 16, 16, 0, NO, NO },
    { MTLBindingTypeBuffer, 1, "p", MTLBindingAccessReadOnly, YES, MTLDataTypeStruct, 32, 16, 0, NO, NO },
    { MTLBindingTypeBuffer, 5, "unused", MTLBindingAccessReadOnly, NO, MTLDataTypeUInt, 4, 4, 0, NO, NO },
    { MTLBindingTypeTexture, 0, "src", MTLBindingAccessReadOnly, YES, MTLDataTypeFloat, 0, 0, MTLTextureType2D, NO, NO },
    { MTLBindingTypeTexture, 3, "dst", MTLBindingAccessWriteOnly, YES, MTLDataTypeUInt, 0, 0, MTLTextureType2D, NO, NO },
    { MTLBindingTypeTexture, 4, "dep", MTLBindingAccessReadOnly, YES, MTLDataTypeFloat, 0, 0, MTLTextureType2D, YES, NO },
    { MTLBindingTypeSampler, 2, "s", MTLBindingAccessReadOnly, YES, 0, 0, 0, 0, NO, NO },
    { MTLBindingTypeThreadgroupMemory, 0, "tg", MTLBindingAccessReadWrite, YES, 0, 16, 16, 0, NO, NO },
    { MTLBindingTypeThreadgroupMemory, 1, "tg2", MTLBindingAccessReadWrite, YES, 0, 4, 4, 0, NO, NO },
};
static const expect_t kVertex[] = {
    { MTLBindingTypeBuffer, 0, "vertexBuffer.0", MTLBindingAccessReadOnly, YES, MTLDataTypeStruct, 16, 4, 0, NO, YES },
    { MTLBindingTypeBuffer, 1, "mvp", MTLBindingAccessReadOnly, YES, MTLDataTypeFloat4x4, 64, 16, 0, NO, NO },
};
static const expect_t kFragment[] = {
    { MTLBindingTypeTexture, 1, "t", MTLBindingAccessReadOnly, YES, MTLDataTypeHalf, 0, 0, MTLTextureType2D, NO, NO },
    { MTLBindingTypeSampler, 0, "s", MTLBindingAccessReadOnly, YES, 0, 0, 0, 0, NO, NO },
    { MTLBindingTypeBuffer, 2, "tint", MTLBindingAccessReadOnly, YES, MTLDataTypeFloat4, 16, 16, 0, NO, NO },
};

static const char *tname(MTLBindingType t)
{
    switch (t) {
    case MTLBindingTypeBuffer: return "buffer";
    case MTLBindingTypeTexture: return "texture";
    case MTLBindingTypeSampler: return "sampler";
    case MTLBindingTypeThreadgroupMemory: return "threadgroup";
    default: return "other";
    }
}

// Every binding of `bs` against `ex` (found by type and index, in any order: Apple lists them in declaration order).
static void check_bindings(const char *what, NSArray<id<MTLBinding>> *bs, const expect_t *ex, size_t n)
{
    check(bs.count == n, "%s: %lu bindings (want %zu)", what, (unsigned long)bs.count, n);
    for (size_t i = 0; i < n; i++) {
        const expect_t *e = &ex[i];
        id<MTLBinding> b = nil;
        for (id<MTLBinding> x in bs) if (x.type == e->type && x.index == e->index) { b = x; break; }
        if (!b) { check(0, "%s: %s %lu '%s' is listed", what, tname(e->type), (unsigned long)e->index, e->name); continue; }
        BOOL ok = [b.name isEqualToString:@(e->name)] && b.access == e->access && b.used == e->used && b.argument == !e->stagein;
        NSMutableString *got = [NSMutableString stringWithFormat:@"name '%@' access %lu used %d argument %d", b.name, (unsigned long)b.access, b.used, b.argument];
        if (e->type == MTLBindingTypeBuffer) {
            id<MTLBufferBinding> bb = (id<MTLBufferBinding>)b;
            ok = ok && bb.bufferDataType == e->dtype && bb.bufferDataSize == e->size && bb.bufferAlignment == e->align;
            [got appendFormat:@", data type %lu size %lu alignment %lu", (unsigned long)bb.bufferDataType, (unsigned long)bb.bufferDataSize,
                              (unsigned long)bb.bufferAlignment];
        } else if (e->type == MTLBindingTypeTexture) {
            id<MTLTextureBinding> tb = (id<MTLTextureBinding>)b;
            ok = ok && tb.textureType == e->ttype && tb.textureDataType == e->dtype && tb.depthTexture == e->depth && tb.arrayLength == 1;
            [got appendFormat:@", texture type %lu component %lu depth %d array length %lu", (unsigned long)tb.textureType,
                              (unsigned long)tb.textureDataType, tb.depthTexture, (unsigned long)tb.arrayLength];
        } else if (e->type == MTLBindingTypeThreadgroupMemory) {
            id<MTLThreadgroupBinding> tg = (id<MTLThreadgroupBinding>)b;
            ok = ok && tg.threadgroupMemoryDataSize == e->size && tg.threadgroupMemoryAlignment == e->align;
            [got appendFormat:@", size %lu alignment %lu", (unsigned long)tg.threadgroupMemoryDataSize, (unsigned long)tg.threadgroupMemoryAlignment];
        }
        check(ok, "%s: %s %lu '%s': %s", what, tname(e->type), (unsigned long)e->index, e->name, got.UTF8String);
    }
    // the constant struct: its members by name, offset and type (pad is a float[3])
    for (id<MTLBinding> x in bs) {
        if (x.type != MTLBindingTypeBuffer || ![x.name isEqualToString:@"p"]) continue;
        MTLStructType *st = ((id<MTLBufferBinding>)x).bufferStructType;
        MTLStructMember *sc = [st memberByName:@"scale"], *ct = [st memberByName:@"count"], *pd = [st memberByName:@"pad"];
        check(st.members.count == 3 && sc.offset == 0 && sc.dataType == MTLDataTypeFloat4 && ct.offset == 16 && ct.dataType == MTLDataTypeUInt
              && pd.offset == 20 && pd.dataType == MTLDataTypeArray && pd.arrayType.arrayLength == 3 && pd.arrayType.elementType == MTLDataTypeFloat,
              "%s: struct Params: %lu members, scale @%lu type %lu, count @%lu type %lu, pad @%lu type %lu [%lu x %lu]", what,
              (unsigned long)st.members.count, (unsigned long)sc.offset, (unsigned long)sc.dataType, (unsigned long)ct.offset,
              (unsigned long)ct.dataType, (unsigned long)pd.offset, (unsigned long)pd.dataType, (unsigned long)pd.arrayType.arrayLength,
              (unsigned long)pd.arrayType.elementType);
    }
}

#endif
