// SPDX-License-Identifier: MIT
// scanout_p4_unit.c: host-side unit test of gpuperf phase 4's per-frame decision (nullmoth/plugin/nvmtl_scanout_p4.h):
// which bound WindowServer composites skip their system-memory readback, the readback modes and their parser, the
// safety rule (the pages are left stale only in phase 4, which marks them, or in the explicit off mode), and real
// IOSurfaces of the B580 test displays' sizes (2560x1440, 3440x1440) made the way WindowServer's composites are.
// No GPU, no plugin. build+run: user/tests/plugin/scanout_p4_unit.sh
#include <stdio.h>
#include <stdlib.h>
#include "../../../nullmoth/plugin/nvmtl_scanout_p4.h"
#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOSurface/IOSurfaceRef.h>
#endif

static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static uint32_t scanout_pitch(uint32_t w) { return (w * 4 + 63) & ~63u; }   // NMXAccel / IntelXeFB's scan-out pitch (nvmtl_scanout_get)
static uint64_t pages4k(uint64_t n) { return (n + 0xfff) & ~0xfffull; }

static void test_parse(void)
{
    CHECK(nvmtl_sorb_parse(NULL) == NVMTL_SORB_P4, "unset -> p4");
    CHECK(nvmtl_sorb_parse("") == NVMTL_SORB_P4, "empty -> p4");
    CHECK(nvmtl_sorb_parse("p4") == NVMTL_SORB_P4 && nvmtl_sorb_parse("4") == NVMTL_SORB_P4 && nvmtl_sorb_parse("auto") == NVMTL_SORB_P4, "p4 names");
    CHECK(nvmtl_sorb_parse("1") == NVMTL_SORB_FULL && nvmtl_sorb_parse("full") == NVMTL_SORB_FULL && nvmtl_sorb_parse("on") == NVMTL_SORB_FULL, "full names");
    CHECK(nvmtl_sorb_parse("0") == NVMTL_SORB_OFF && nvmtl_sorb_parse("off") == NVMTL_SORB_OFF, "off names");
    CHECK(nvmtl_sorb_parse("yes please") == NVMTL_SORB_P4, "unknown -> p4");
    for (int m = 0; m < 3; m++) {
        CHECK(nvmtl_sorb_effective((nvmtl_sorb_mode)m, 0, 0) == (nvmtl_sorb_mode)m, "no flags keep env %d", m);
        CHECK(nvmtl_sorb_effective((nvmtl_sorb_mode)m, 1, 0) == NVMTL_SORB_FULL, "full flag over env %d", m);
        CHECK(nvmtl_sorb_effective((nvmtl_sorb_mode)m, 0, 1) == NVMTL_SORB_OFF, "off flag over env %d", m);
        CHECK(nvmtl_sorb_effective((nvmtl_sorb_mode)m, 1, 1) == NVMTL_SORB_OFF, "off flag wins over full flag (env %d)", m);
    }
}

// the B580 test displays: WindowServer's composite has the scan-out pitch (td512's log: 13760 at 3440 wide)
static void test_modes(void)
{
    static const uint32_t dims[][2] = { { 2560, 1440 }, { 3440, 1440 }, { 1920, 1080 }, { 3840, 2160 }, { 1366, 768 } };
    for (unsigned i = 0; i < sizeof dims / sizeof dims[0]; i++) {
        const uint32_t w = dims[i][0], h = dims[i][1], pitch = scanout_pitch(w);
        const size_t alloc = (size_t)pages4k((uint64_t)pitch * h);
        nvmtl_p4_decision d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, pitch, 0, alloc, 0);
        CHECK(d.p4 && !d.readback && d.why == NVMTL_P4_YES, "%ux%u pitch %u: phase 4 engages (why %s)", w, h, pitch, nvmtl_p4_why_name(d.why));
        d = nvmtl_p4_decide(NVMTL_SORB_FULL, 1, pitch, h, pitch, 0, alloc, 0);
        CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_MODE_FULL, "%ux%u full mode reads back", w, h);
        d = nvmtl_p4_decide(NVMTL_SORB_OFF, 1, pitch, h, pitch, 0, alloc, 0);
        CHECK(!d.p4 && !d.readback && d.why == NVMTL_P4_MODE_OFF, "%ux%u off mode: scan-out only, no mark", w, h);
        d = nvmtl_p4_decide(NVMTL_SORB_P4, 0, pitch, h, pitch, 0, alloc, 0);
        CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_UNBOUND, "%ux%u unbound reads back", w, h);
    }
    // 3440x1440 is 4837.5 pages: the allocation must hold the page-rounded frame, or the kernel's whole-page page-off
    // would stop half a page short (19814400 bytes vs 19816448)
    const uint32_t w = 3440, h = 1440, pitch = scanout_pitch(w);
    CHECK(pitch == 13760, "3440 pitch 13760 (got %u)", pitch);
    CHECK(nvmtl_p4_frame_pages_bytes(pitch, h) == 19816448ull, "3440x1440 page-rounded frame 19816448 (got %llu)", (unsigned long long)nvmtl_p4_frame_pages_bytes(pitch, h));
    nvmtl_p4_decision d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, pitch, 0, (size_t)pitch * h, 0);
    CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_ALLOC_SHORT, "unrounded allocation reads back (why %s)", nvmtl_p4_why_name(d.why));
    d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, 13824, 0, 19906560, 0);   // Apple silicon's default 256-byte row
    CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_PITCH, "other row pitch reads back");
    d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, pitch, 4096, 19820544, 0);
    CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_PLANE_OFFSET, "plane offset reads back");
    d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, pitch, 0, 19816448, 1);
    CHECK(!d.p4 && d.readback && d.why == NVMTL_P4_REFUSED, "refusal hold-off reads back");
    d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, 0, h, 0, 0, 19816448, 0);
    CHECK(!d.p4 && d.readback, "zero pitch never phase 4");
}

// The rule that keeps CPU readers correct: a frame leaves the pages stale only under phase 4 (marked newer: a CPU lock
// pages it off) or in the explicit off mode. Every combination of inputs.
static void test_safety(void)
{
    static const uint32_t pitches[] = { 0, 10240, 13760, 13824 };
    static const size_t rows[] = { 0, 10240, 13760, 13824 }, offs[] = { 0, 64, 4096 };
    static const size_t allocs[] = { 0, 19814400, 19816448, 19906560, 14745600 };
    unsigned n = 0;
    for (int m = 0; m < 3; m++) for (int b = 0; b < 2; b++) for (unsigned p = 0; p < 4; p++) for (unsigned r = 0; r < 4; r++)
    for (unsigned o = 0; o < 3; o++) for (unsigned a = 0; a < 5; a++) for (int rf = 0; rf < 2; rf++) {
        const nvmtl_p4_decision d = nvmtl_p4_decide((nvmtl_sorb_mode)m, b, pitches[p], 1440, rows[r], offs[o], allocs[a], rf);
        n++;
        CHECK(d.why > 0 && d.why < NVMTL_P4_NWHY, "why in range");
        CHECK(!(d.p4 && d.readback), "phase 4 never also reads back");
        if (!d.readback) CHECK(d.p4 || (m == NVMTL_SORB_OFF && b), "pages left stale only by phase 4 or the off mode (mode %d bound %d)", m, b);
        if (d.p4) CHECK(b && m == NVMTL_SORB_P4 && pitches[p] && (size_t)pitches[p] == rows[r] && !offs[o] && !rf &&
                        allocs[a] >= nvmtl_p4_frame_pages_bytes(pitches[p], 1440), "phase 4 only with the scan-out layout");
        if (!b) CHECK(d.readback && d.why == NVMTL_P4_UNBOUND, "unbound always reads back");
    }
    printf("  safety: %u combinations\n", n);
}

// The old condition (until 2026-11-14): phase 4 also needed the readback switch on and debug.intelxe.surfvram >= 2.
static void test_regression(void)
{
    const uint32_t pitch = 13760, h = 1440; const size_t alloc = 19816448;
    for (int level = 0; level <= 3; level++) for (int rbon = 0; rbon <= 1; rbon++) {
        const int old = rbon && level >= 2;   // so && readback && pitch == row && base == imp->base && level >= 2
        const nvmtl_p4_decision d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, pitch, 0, alloc, 0);
        CHECK(d.p4, "level %d readback switch %d: phase 4 engages now (old condition: %d)", level, rbon, old);
        if (!old) printf("  surfvram %d, readback switch %d: old condition refused phase 4, new engages\n", level, rbon);
    }
}

#ifdef __APPLE__
static void test_iosurface(void)
{
    static const uint32_t dims[][2] = { { 2560, 1440 }, { 3440, 1440 } };
    for (unsigned i = 0; i < 2; i++) {
        const uint32_t w = dims[i][0], h = dims[i][1], pitch = scanout_pitch(w);
        int vw = (int)w, vh = (int)h, bpe = 4, bpr = (int)pitch, fmt = 0x42475241;
        const void *k[] = { kIOSurfaceWidth, kIOSurfaceHeight, kIOSurfaceBytesPerElement, kIOSurfaceBytesPerRow, kIOSurfacePixelFormat };
        CFNumberRef v[] = { CFNumberCreate(NULL, kCFNumberIntType, &vw), CFNumberCreate(NULL, kCFNumberIntType, &vh), CFNumberCreate(NULL, kCFNumberIntType, &bpe),
                            CFNumberCreate(NULL, kCFNumberIntType, &bpr), CFNumberCreate(NULL, kCFNumberIntType, &fmt) };
        CFDictionaryRef props = CFDictionaryCreate(NULL, k, (const void **)v, 5, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        IOSurfaceRef s = IOSurfaceCreate(props);
        CHECK(s != NULL, "IOSurfaceCreate %ux%u", w, h);
        if (s) {
            const size_t row = IOSurfaceGetBytesPerRowOfPlane(s, 0), alloc = IOSurfaceGetAllocSize(s);
            void *base = IOSurfaceGetBaseAddress(s), *plane = IOSurfaceGetBaseAddressOfPlane(s, 0);
            CHECK(plane == base, "%ux%u: plane 0 of a planeless surface is its base (%p %p)", w, h, plane, base);
            const nvmtl_p4_decision d = nvmtl_p4_decide(NVMTL_SORB_P4, 1, pitch, h, row, (size_t)((char *)plane - (char *)base), alloc, 0);
            printf("  IOSurface %ux%u: row %zu, %zu bytes (page-rounded frame %llu): %s\n", w, h, row, alloc,
                   (unsigned long long)nvmtl_p4_frame_pages_bytes(pitch, h), nvmtl_p4_why_name(d.why));
            CHECK(d.p4, "%ux%u IOSurface with the scan-out row: phase 4 engages (why %s)", w, h, nvmtl_p4_why_name(d.why));
            CFRelease(s);
        }
        CFRelease(props); for (int j = 0; j < 5; j++) CFRelease(v[j]);
    }
}
#endif

int main(void)
{
    test_parse(); test_modes(); test_safety(); test_regression();
#ifdef __APPLE__
    test_iosurface();
#endif
    printf("%s: %d checks, %d failed\n", fails ? "FAILED" : "PASSED", checks, fails);
    return fails ? 1 : 0;
}
